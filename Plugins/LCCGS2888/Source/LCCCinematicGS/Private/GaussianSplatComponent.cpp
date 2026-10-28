// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "GaussianSplatActor.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatRenderData.h"
#include "GaussianSplatSceneProxy.h"
#include "GaussianPCVR.h"
#include "HAL/ThreadSafeCounter.h"
#include "GaussianSplatViewExtension.h"
#include "Engine/World.h"
#include "Components/StaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "UnrealClient.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#if WITH_EDITOR
#include "FileHelpers.h"
#endif
#include "RenderCore.h"
#include "RHICommandList.h"
#include "DrawDebugHelpers.h"

static void ApplyRainColorIsolationBase(UGaussianSplatComponent* Component);
static bool HasLegacyFogStateLeakedIntoRain(const UGaussianSplatComponent* Component);
static void ResetRainFogLeakStateToOriginalBaseline(UGaussianSplatComponent* Component, bool bForce);
#include "StaticMeshResources.h"
#include "HAL/IConsoleManager.h"

extern TAutoConsoleVariable<int32> CVarGSPanoramaShadowTrace;
extern TAutoConsoleVariable<int32> CVarGSPanoramaShadowStats;
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Async/ParallelFor.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Modules/ModuleManager.h"
#endif

static float SmoothGSPhase(float Edge0, float Edge1, float Value)
{
	if (Edge1 <= Edge0 + UE_KINDA_SMALL_NUMBER)
	{
		return Value >= Edge1 ? 1.0f : 0.0f;
	}
	const float T = FMath::Clamp((Value - Edge0) / (Edge1 - Edge0), 0.0f, 1.0f);
	return T * T * (3.0f - 2.0f * T);
}

static float ComputeGSDirectionalHorizonPresence(const UDirectionalLightComponent* Light)
{
	if (!Light)
	{
		return 0.0f;
	}
	const FVector SurfaceToLight = (-Light->GetForwardVector()).GetSafeNormal();
	const float T = FMath::Clamp(((float)SurfaceToLight.Z + 0.12f) / 0.22f, 0.0f, 1.0f);
	return T * T * (3.0f - 2.0f * T);
}

static float ComputeGSDirectionalAuthorityScore(const UDirectionalLightComponent* Light, const UGaussianSplatComponent* Component)
{
	if (!Light)
	{
		return -1.0e9f;
	}
	if (Component && Component->bEnableAutomaticDayNightCycle && Component->bRuntimeActiveCelestialDirectionValid)
	{
		const FVector SurfaceToLight = (-Light->GetForwardVector()).GetSafeNormal();
		const float Alignment = FVector::DotProduct(SurfaceToLight, Component->RuntimeActiveCelestialDirection.GetSafeNormal());
		// The day/night controller owns one celestial direction. Alignment dominates authored lux
		// so a bright below-horizon sun cannot steal Stable shadow authority from the moon.
		return Alignment * 1000000000.0f + FMath::Min(FMath::Max(Light->Intensity, 0.0f), 1000.0f) * 0.001f;
	}
	return FMath::Max(0.0f, Light->Intensity) * ComputeGSDirectionalHorizonPresence(Light);
}

namespace
{
constexpr uint32 StableCastCacheMagic = 0x53434331u; // SCC1
constexpr uint32 StableCastCacheVersion = 2u;

struct FStableCastCacheHeader
{
	uint32 Magic = StableCastCacheMagic;
	uint32 Version = StableCastCacheVersion;
	uint64 BindingHash = 0u;
	int32 DirectionX = 0;
	int32 DirectionY = 0;
	int32 DirectionZ = 0;
	int32 DepthBiasMillionths = 0;
	int32 BoundarySofteningMillionths = 0;
	uint32 VertexCount = 0u;
};

static int32 QuantizeStableCastValue(float Value)
{
	return FMath::RoundToInt(Value * 1000000.0f);
}

static FString BuildStableCastCachePath(
	uint64 BindingHash,
	const FVector3f& Direction,
	float DepthBiasCm,
	float BoundarySoftening)
{
	const int32 DX = QuantizeStableCastValue(Direction.X);
	const int32 DY = QuantizeStableCastValue(Direction.Y);
	const int32 DZ = QuantizeStableCastValue(Direction.Z);
	const int32 Bias = QuantizeStableCastValue(DepthBiasCm);
	const int32 Softening = QuantizeStableCastValue(BoundarySoftening);
	const FString Directory = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("LCCCinematicGS"), TEXT("StableCastCache"),
		FString::Printf(TEXT("%016llX"), static_cast<unsigned long long>(BindingHash)));
	return FPaths::Combine(Directory, FString::Printf(
		TEXT("D_%d_%d_%d_B_%d_S_%d.bin"), DX, DY, DZ, Bias, Softening));
}

static bool LoadStableCastCache(
	const FString& Path,
	uint64 BindingHash,
	const FVector3f& Direction,
	float DepthBiasCm,
	float BoundarySoftening,
	int32 ExpectedVertexCount,
	TArray<FVector4f>& OutProofs)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path) ||
		Bytes.Num() < static_cast<int32>(sizeof(FStableCastCacheHeader)))
	{
		return false;
	}

	FStableCastCacheHeader Header;
	FMemory::Memcpy(&Header, Bytes.GetData(), sizeof(Header));
	const int64 ExpectedBytes = sizeof(FStableCastCacheHeader) +
		static_cast<int64>(ExpectedVertexCount) * sizeof(float);
	if (Header.Magic != StableCastCacheMagic || Header.Version != StableCastCacheVersion ||
		Header.BindingHash != BindingHash || Header.VertexCount != static_cast<uint32>(ExpectedVertexCount) ||
		Header.DirectionX != QuantizeStableCastValue(Direction.X) ||
		Header.DirectionY != QuantizeStableCastValue(Direction.Y) ||
		Header.DirectionZ != QuantizeStableCastValue(Direction.Z) ||
		Header.DepthBiasMillionths != QuantizeStableCastValue(DepthBiasCm) ||
		Header.BoundarySofteningMillionths != QuantizeStableCastValue(BoundarySoftening) ||
		Bytes.Num() != ExpectedBytes)
	{
		return false;
	}

	const float* Visibility = reinterpret_cast<const float*>(
		Bytes.GetData() + sizeof(FStableCastCacheHeader));
	OutProofs.SetNumUninitialized(ExpectedVertexCount);
	ParallelFor(ExpectedVertexCount, [&OutProofs, Visibility](int32 VertexIndex)
	{
		const float V = Visibility[VertexIndex];
		OutProofs[VertexIndex] = FVector4f(V, V, 1.0f, 0.0f);
	});
	return true;
}

static bool SaveStableCastCache(
	const FString& Path,
	uint64 BindingHash,
	const FVector3f& Direction,
	float DepthBiasCm,
	float BoundarySoftening,
	const TArray<FVector4f>& Proofs)
{
	FStableCastCacheHeader Header;
	Header.BindingHash = BindingHash;
	Header.DirectionX = QuantizeStableCastValue(Direction.X);
	Header.DirectionY = QuantizeStableCastValue(Direction.Y);
	Header.DirectionZ = QuantizeStableCastValue(Direction.Z);
	Header.DepthBiasMillionths = QuantizeStableCastValue(DepthBiasCm);
	Header.BoundarySofteningMillionths = QuantizeStableCastValue(BoundarySoftening);
	Header.VertexCount = static_cast<uint32>(Proofs.Num());

	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(sizeof(Header) + Proofs.Num() * sizeof(float));
	FMemory::Memcpy(Bytes.GetData(), &Header, sizeof(Header));
	float* Visibility = reinterpret_cast<float*>(Bytes.GetData() + sizeof(Header));
	ParallelFor(Proofs.Num(), [&Proofs, Visibility](int32 VertexIndex)
	{
		Visibility[VertexIndex] = Proofs[VertexIndex].X;
	});
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	return FFileHelper::SaveArrayToFile(Bytes, *Path);
}

TAutoConsoleVariable<int32> CVarProxyShadowProbeDebug(
	TEXT("gs.ProxyShadowProbeDebug"),
	0,
	TEXT("Experimental hidden-proxy shadow probe visualization. 0=off, 1=draw lit/shadow samples."),
	ECVF_Default);

void SyncOwnerProxyShadowReceiver(UGaussianSplatComponent* Component)
{
	if (!Component)
	{
		return;
	}
	if (AGaussianSplatActor* SplatActor = Cast<AGaussianSplatActor>(Component->GetOwner()))
	{
		SplatActor->SyncProxyShadowReceiver();
	}
}

struct FStableBindingTriangle
{
	FVector3f A = FVector3f::ZeroVector;
	FVector3f B = FVector3f::ZeroVector;
	FVector3f C = FVector3f::ZeroVector;
	FVector3f BoundsMin = FVector3f::ZeroVector;
	FVector3f BoundsMax = FVector3f::ZeroVector;
	FVector3f Centroid = FVector3f::ZeroVector;
	FVector3f Normal = FVector3f(0.0f, 0.0f, 1.0f);
	uint32 TriangleIndex = MAX_uint32;
};

struct FStableBindingBVHNode
{
	FVector3f BoundsMin = FVector3f::ZeroVector;
	FVector3f BoundsMax = FVector3f::ZeroVector;
	int32 Left = INDEX_NONE;
	int32 Right = INDEX_NONE;
	int32 Start = 0;
	int32 Count = 0;
};

static uint32 PackStableProxyNormalAndClosure(
	const FVector3f& InNormal,
	float LocalClosureOpen = 1.0f,
	float BroadClosureOpen = 1.0f)
{
	FVector3f N = InNormal.GetSafeNormal();
	if (N.IsNearlyZero())
	{
		N = FVector3f(0.0f, 0.0f, 1.0f);
	}
	const float L1 = FMath::Max(FMath::Abs(N.X) + FMath::Abs(N.Y) + FMath::Abs(N.Z), 1.0e-6f);
	N /= L1;
	FVector2f Oct(N.X, N.Y);
	if (N.Z < 0.0f)
	{
		const float OldX = Oct.X;
		Oct.X = (1.0f - FMath::Abs(Oct.Y)) * (OldX >= 0.0f ? 1.0f : -1.0f);
		Oct.Y = (1.0f - FMath::Abs(OldX)) * (Oct.Y >= 0.0f ? 1.0f : -1.0f);
	}

	// v2745: 10+10-bit oct normal + 6-bit local closure + 6-bit broad closure.
	// This keeps normal precision high while letting the existing "폐쇄 그림자 영역 크기"
	// blend between two camera-independent geometric scales without rebuilding the field.
	const uint32 X = static_cast<uint32>(FMath::Clamp(FMath::RoundToInt((Oct.X * 0.5f + 0.5f) * 1023.0f), 0, 1023));
	const uint32 Y = static_cast<uint32>(FMath::Clamp(FMath::RoundToInt((Oct.Y * 0.5f + 0.5f) * 1023.0f), 0, 1023));
	const uint32 LocalClosure = static_cast<uint32>(FMath::Clamp(
		FMath::RoundToInt(FMath::Clamp(LocalClosureOpen, 0.0f, 1.0f) * 63.0f), 0, 63));
	const uint32 BroadClosure = static_cast<uint32>(FMath::Clamp(
		FMath::RoundToInt(FMath::Clamp(BroadClosureOpen, 0.0f, 1.0f) * 63.0f), 0, 63));
	uint32 Packed = X | (Y << 10) | (LocalClosure << 20) | (BroadClosure << 26);
	if ((Packed & 0x000FFFFFu) == 0u)
	{
		Packed |= 1u;
	}
	return Packed;
}

static float StableAxisValue(const FVector3f& V, int32 Axis)
{
	return Axis == 0 ? V.X : (Axis == 1 ? V.Y : V.Z);
}

static float StableDistanceSqToBox(const FVector3f& P, const FVector3f& BMin, const FVector3f& BMax)
{
	float D2 = 0.0f;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const float V = StableAxisValue(P, Axis);
		const float MinV = StableAxisValue(BMin, Axis);
		const float MaxV = StableAxisValue(BMax, Axis);
		if (V < MinV)
		{
			const float D = MinV - V;
			D2 += D * D;
		}
		else if (V > MaxV)
		{
			const float D = V - MaxV;
			D2 += D * D;
		}
	}
	return D2;
}

static FVector3f StableClosestPointOnTriangle(
	const FVector3f& P,
	const FStableBindingTriangle& T,
	FVector3f& OutBarycentric)
{
	const FVector3f AB = T.B - T.A;
	const FVector3f AC = T.C - T.A;
	const FVector3f AP = P - T.A;
	const float D1 = FVector3f::DotProduct(AB, AP);
	const float D2 = FVector3f::DotProduct(AC, AP);
	if (D1 <= 0.0f && D2 <= 0.0f)
	{
		OutBarycentric = FVector3f(1.0f, 0.0f, 0.0f);
		return T.A;
	}

	const FVector3f BP = P - T.B;
	const float D3 = FVector3f::DotProduct(AB, BP);
	const float D4 = FVector3f::DotProduct(AC, BP);
	if (D3 >= 0.0f && D4 <= D3)
	{
		OutBarycentric = FVector3f(0.0f, 1.0f, 0.0f);
		return T.B;
	}

	const float VC = D1 * D4 - D3 * D2;
	if (VC <= 0.0f && D1 >= 0.0f && D3 <= 0.0f)
	{
		const float V = D1 / FMath::Max(D1 - D3, 1.0e-12f);
		OutBarycentric = FVector3f(1.0f - V, V, 0.0f);
		return T.A + AB * V;
	}

	const FVector3f CP = P - T.C;
	const float D5 = FVector3f::DotProduct(AB, CP);
	const float D6 = FVector3f::DotProduct(AC, CP);
	if (D6 >= 0.0f && D5 <= D6)
	{
		OutBarycentric = FVector3f(0.0f, 0.0f, 1.0f);
		return T.C;
	}

	const float VB = D5 * D2 - D1 * D6;
	if (VB <= 0.0f && D2 >= 0.0f && D6 <= 0.0f)
	{
		const float W = D2 / FMath::Max(D2 - D6, 1.0e-12f);
		OutBarycentric = FVector3f(1.0f - W, 0.0f, W);
		return T.A + AC * W;
	}

	const float VA = D3 * D6 - D5 * D4;
	if (VA <= 0.0f && (D4 - D3) >= 0.0f && (D5 - D6) >= 0.0f)
	{
		const FVector3f BC = T.C - T.B;
		const float W = (D4 - D3) / FMath::Max((D4 - D3) + (D5 - D6), 1.0e-12f);
		OutBarycentric = FVector3f(0.0f, 1.0f - W, W);
		return T.B + BC * W;
	}

	const float Denom = 1.0f / FMath::Max(VA + VB + VC, 1.0e-12f);
	const float V = VB * Denom;
	const float W = VC * Denom;
	OutBarycentric = FVector3f(1.0f - V - W, V, W);
	return T.A + AB * V + AC * W;
}

static int32 BuildStableBindingBVH(
	const TArray<FStableBindingTriangle>& Triangles,
	TArray<int32>& Order,
	TArray<FStableBindingBVHNode>& Nodes,
	int32 Start,
	int32 Count)
{
	const int32 NodeIndex = Nodes.AddDefaulted();
	FStableBindingBVHNode Node;
	Node.Start = Start;
	Node.Count = Count;
	Node.BoundsMin = FVector3f(BIG_NUMBER, BIG_NUMBER, BIG_NUMBER);
	Node.BoundsMax = FVector3f(-BIG_NUMBER, -BIG_NUMBER, -BIG_NUMBER);
	FVector3f CentroidMin(BIG_NUMBER, BIG_NUMBER, BIG_NUMBER);
	FVector3f CentroidMax(-BIG_NUMBER, -BIG_NUMBER, -BIG_NUMBER);

	for (int32 I = Start; I < Start + Count; ++I)
	{
		const FStableBindingTriangle& T = Triangles[Order[I]];
		Node.BoundsMin = Node.BoundsMin.ComponentMin(T.BoundsMin);
		Node.BoundsMax = Node.BoundsMax.ComponentMax(T.BoundsMax);
		CentroidMin = CentroidMin.ComponentMin(T.Centroid);
		CentroidMax = CentroidMax.ComponentMax(T.Centroid);
	}

	if (Count <= 8)
	{
		Nodes[NodeIndex] = Node;
		return NodeIndex;
	}

	const FVector3f Extent = CentroidMax - CentroidMin;
	int32 Axis = 0;
	if (Extent.Y > Extent.X && Extent.Y >= Extent.Z) Axis = 1;
	else if (Extent.Z > Extent.X && Extent.Z > Extent.Y) Axis = 2;
	const float SplitValue = 0.5f * (StableAxisValue(CentroidMin, Axis) + StableAxisValue(CentroidMax, Axis));

	int32 L = Start;
	int32 R = Start + Count - 1;
	while (L <= R)
	{
		while (L <= R && StableAxisValue(Triangles[Order[L]].Centroid, Axis) < SplitValue) ++L;
		while (L <= R && StableAxisValue(Triangles[Order[R]].Centroid, Axis) >= SplitValue) --R;
		if (L < R)
		{
			Swap(Order[L], Order[R]);
			++L;
			--R;
		}
	}

	int32 LeftCount = L - Start;
	if (LeftCount <= 0 || LeftCount >= Count)
	{
		LeftCount = Count / 2;
	}
	Node.Count = 0;
	Node.Left = BuildStableBindingBVH(Triangles, Order, Nodes, Start, LeftCount);
	Node.Right = BuildStableBindingBVH(Triangles, Order, Nodes, Start + LeftCount, Count - LeftCount);
	Nodes[NodeIndex] = Node;
	return NodeIndex;
}

static void QueryStableBindingBVH(
	const TArray<FStableBindingTriangle>& Triangles,
	const TArray<int32>& Order,
	const TArray<FStableBindingBVHNode>& Nodes,
	int32 NodeIndex,
	const FVector3f& P,
	float& InOutBestDistanceSq,
	uint32& OutTriangleIndex,
	FVector3f& OutBarycentric)
{
	if (!Nodes.IsValidIndex(NodeIndex))
	{
		return;
	}
	const FStableBindingBVHNode& Node = Nodes[NodeIndex];
	if (StableDistanceSqToBox(P, Node.BoundsMin, Node.BoundsMax) > InOutBestDistanceSq)
	{
		return;
	}

	if (Node.Left == INDEX_NONE && Node.Right == INDEX_NONE)
	{
		for (int32 I = Node.Start; I < Node.Start + Node.Count; ++I)
		{
			const FStableBindingTriangle& T = Triangles[Order[I]];
			if (StableDistanceSqToBox(P, T.BoundsMin, T.BoundsMax) > InOutBestDistanceSq)
			{
				continue;
			}
			FVector3f Barycentric;
			const FVector3f Closest = StableClosestPointOnTriangle(P, T, Barycentric);
			const float DistanceSq = (P - Closest).SizeSquared();
			if (DistanceSq < InOutBestDistanceSq)
			{
				InOutBestDistanceSq = DistanceSq;
				OutTriangleIndex = T.TriangleIndex;
				OutBarycentric = Barycentric;
			}
		}
		return;
	}

	const float LeftDistance = Nodes.IsValidIndex(Node.Left)
		? StableDistanceSqToBox(P, Nodes[Node.Left].BoundsMin, Nodes[Node.Left].BoundsMax)
		: BIG_NUMBER;
	const float RightDistance = Nodes.IsValidIndex(Node.Right)
		? StableDistanceSqToBox(P, Nodes[Node.Right].BoundsMin, Nodes[Node.Right].BoundsMax)
		: BIG_NUMBER;
	if (LeftDistance <= RightDistance)
	{
		QueryStableBindingBVH(Triangles, Order, Nodes, Node.Left, P, InOutBestDistanceSq, OutTriangleIndex, OutBarycentric);
		QueryStableBindingBVH(Triangles, Order, Nodes, Node.Right, P, InOutBestDistanceSq, OutTriangleIndex, OutBarycentric);
	}
	else
	{
		QueryStableBindingBVH(Triangles, Order, Nodes, Node.Right, P, InOutBestDistanceSq, OutTriangleIndex, OutBarycentric);
		QueryStableBindingBVH(Triangles, Order, Nodes, Node.Left, P, InOutBestDistanceSq, OutTriangleIndex, OutBarycentric);
	}
}


// v2810: nearby compatible-triangle collection for continuous per-splat normals.
struct FStableBindingNormalCandidate
{
	uint32 TriangleIndex = MAX_uint32;
	float DistanceSq = BIG_NUMBER;
	FVector3f Barycentric = FVector3f(1.0f, 0.0f, 0.0f);
};

static void AddStableNormalCandidate(TArray<FStableBindingNormalCandidate>& Candidates, const FStableBindingNormalCandidate& Candidate, int32 MaxCandidates)
{
	if (Candidates.Num() < MaxCandidates)
	{
		Candidates.Add(Candidate);
		return;
	}
	int32 WorstIndex = 0;
	float WorstDistanceSq = Candidates[0].DistanceSq;
	for (int32 I = 1; I < Candidates.Num(); ++I)
	{
		if (Candidates[I].DistanceSq > WorstDistanceSq)
		{
			WorstDistanceSq = Candidates[I].DistanceSq;
			WorstIndex = I;
		}
	}
	if (Candidate.DistanceSq < WorstDistanceSq)
	{
		Candidates[WorstIndex] = Candidate;
	}
}

static void QueryStableBindingBVHNeighborhood(
	const TArray<FStableBindingTriangle>& Triangles, const TArray<int32>& Order,
	const TArray<FStableBindingBVHNode>& Nodes, int32 NodeIndex, const FVector3f& P,
	float MaxDistanceSq, TArray<FStableBindingNormalCandidate>& OutCandidates, int32 MaxCandidates)
{
	if (!Nodes.IsValidIndex(NodeIndex) || StableDistanceSqToBox(P, Nodes[NodeIndex].BoundsMin, Nodes[NodeIndex].BoundsMax) > MaxDistanceSq)
	{
		return;
	}
	const FStableBindingBVHNode& Node = Nodes[NodeIndex];
	if (Node.Left == INDEX_NONE && Node.Right == INDEX_NONE)
	{
		for (int32 I = Node.Start; I < Node.Start + Node.Count; ++I)
		{
			if (!Order.IsValidIndex(I) || !Triangles.IsValidIndex(Order[I])) continue;
			const FStableBindingTriangle& T = Triangles[Order[I]];
			if (StableDistanceSqToBox(P, T.BoundsMin, T.BoundsMax) > MaxDistanceSq) continue;
			FVector3f Barycentric;
			const FVector3f Closest = StableClosestPointOnTriangle(P, T, Barycentric);
			const float DistanceSq = (P - Closest).SizeSquared();
			if (DistanceSq <= MaxDistanceSq)
			{
				FStableBindingNormalCandidate C; C.TriangleIndex = T.TriangleIndex; C.DistanceSq = DistanceSq; C.Barycentric = Barycentric;
				AddStableNormalCandidate(OutCandidates, C, MaxCandidates);
			}
		}
		return;
	}
	if (Node.Left != INDEX_NONE) QueryStableBindingBVHNeighborhood(Triangles, Order, Nodes, Node.Left, P, MaxDistanceSq, OutCandidates, MaxCandidates);
	if (Node.Right != INDEX_NONE) QueryStableBindingBVHNeighborhood(Triangles, Order, Nodes, Node.Right, P, MaxDistanceSq, OutCandidates, MaxCandidates);
}

static FVector3f EvaluateStableTriangleCornerNormal(const TArray<FVector4f>& CornerNormals, uint32 TriangleIndex, const FVector3f& Barycentric)
{
	const int32 BaseIndex = static_cast<int32>(TriangleIndex) * 3;
	if (!CornerNormals.IsValidIndex(BaseIndex + 2)) return FVector3f::ZeroVector;
	const FVector4f& N0 = CornerNormals[BaseIndex + 0];
	const FVector4f& N1 = CornerNormals[BaseIndex + 1];
	const FVector4f& N2 = CornerNormals[BaseIndex + 2];
	FVector3f N(
		N0.X * Barycentric.X + N1.X * Barycentric.Y + N2.X * Barycentric.Z,
		N0.Y * Barycentric.X + N1.Y * Barycentric.Y + N2.Y * Barycentric.Z,
		N0.Z * Barycentric.X + N1.Z * Barycentric.Y + N2.Z * Barycentric.Z);
	return N.GetSafeNormal();
}


static bool StableRayBoxInterval(
	const FVector3f& Origin,
	const FVector3f& Direction,
	const FVector3f& BoundsMin,
	const FVector3f& BoundsMax,
	float MaxDistance,
	float& OutNearT)
{
	float NearT = 0.0f;
	float FarT = MaxDistance;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const float O = StableAxisValue(Origin, Axis);
		const float D = StableAxisValue(Direction, Axis);
		const float MinV = StableAxisValue(BoundsMin, Axis);
		const float MaxV = StableAxisValue(BoundsMax, Axis);
		if (FMath::Abs(D) < 1.0e-8f)
		{
			if (O < MinV || O > MaxV)
			{
				return false;
			}
			continue;
		}
		const float InvD = 1.0f / D;
		float T0 = (MinV - O) * InvD;
		float T1 = (MaxV - O) * InvD;
		if (T0 > T1) Swap(T0, T1);
		NearT = FMath::Max(NearT, T0);
		FarT = FMath::Min(FarT, T1);
		if (NearT > FarT)
		{
			return false;
		}
	}
	OutNearT = NearT;
	return FarT >= 0.0f && NearT <= MaxDistance;
}

static bool StableRayTriangleDistance(
	const FVector3f& Origin,
	const FVector3f& Direction,
	const FStableBindingTriangle& Triangle,
	float MaxDistance,
	float& OutT)
{
	const FVector3f E1 = Triangle.B - Triangle.A;
	const FVector3f E2 = Triangle.C - Triangle.A;
	const FVector3f P = FVector3f::CrossProduct(Direction, E2);
	const float Det = FVector3f::DotProduct(E1, P);
	if (FMath::Abs(Det) < 1.0e-8f)
	{
		return false;
	}
	const float InvDet = 1.0f / Det;
	const FVector3f T = Origin - Triangle.A;
	const float U = FVector3f::DotProduct(T, P) * InvDet;
	if (U < 0.0f || U > 1.0f)
	{
		return false;
	}
	const FVector3f Q = FVector3f::CrossProduct(T, E1);
	const float V = FVector3f::DotProduct(Direction, Q) * InvDet;
	if (V < 0.0f || U + V > 1.0f)
	{
		return false;
	}
	const float HitT = FVector3f::DotProduct(E2, Q) * InvDet;
	if (HitT <= 1.0e-3f || HitT >= MaxDistance)
	{
		return false;
	}
	OutT = HitT;
	return true;
}

static void QueryStableClosureRayBVH(
	const TArray<FStableBindingTriangle>& Triangles,
	const TArray<int32>& Order,
	const TArray<FStableBindingBVHNode>& Nodes,
	int32 NodeIndex,
	const FVector3f& Origin,
	const FVector3f& Direction,
	uint32 IgnoreTriangleIndex,
	float& InOutClosestT)
{
	if (!Nodes.IsValidIndex(NodeIndex))
	{
		return;
	}
	const FStableBindingBVHNode& Node = Nodes[NodeIndex];
	float NearT = 0.0f;
	if (!StableRayBoxInterval(Origin, Direction, Node.BoundsMin, Node.BoundsMax, InOutClosestT, NearT) || NearT > InOutClosestT)
	{
		return;
	}
	if (Node.Left == INDEX_NONE && Node.Right == INDEX_NONE)
	{
		for (int32 I = Node.Start; I < Node.Start + Node.Count; ++I)
		{
			const FStableBindingTriangle& T = Triangles[Order[I]];
			if (T.TriangleIndex == IgnoreTriangleIndex)
			{
				continue;
			}
			float HitT = 0.0f;
			if (StableRayTriangleDistance(Origin, Direction, T, InOutClosestT, HitT))
			{
				InOutClosestT = HitT;
			}
		}
		return;
	}
	QueryStableClosureRayBVH(Triangles, Order, Nodes, Node.Left, Origin, Direction, IgnoreTriangleIndex, InOutClosestT);
	QueryStableClosureRayBVH(Triangles, Order, Nodes, Node.Right, Origin, Direction, IgnoreTriangleIndex, InOutClosestT);
}

static void QueryStableCastRayBVH(
	const TArray<FStableBindingTriangle>& Triangles,
	const TArray<int32>& Order,
	const TArray<FStableBindingBVHNode>& Nodes,
	const TArray<FStableProxyTriangleGPU>& Topology,
	int32 NodeIndex,
	const FVector3f& Origin,
	const FVector3f& Direction,
	uint32 ReceiverVertexIndex,
	float& InOutClosestT)
{
	if (!Nodes.IsValidIndex(NodeIndex)) return;
	const FStableBindingBVHNode& Node = Nodes[NodeIndex];
	float NearT = 0.0f;
	if (!StableRayBoxInterval(Origin, Direction, Node.BoundsMin, Node.BoundsMax, InOutClosestT, NearT) || NearT > InOutClosestT)
	{
		return;
	}
	if (Node.Left == INDEX_NONE && Node.Right == INDEX_NONE)
	{
		for (int32 I = Node.Start; I < Node.Start + Node.Count; ++I)
		{
			const FStableBindingTriangle& T = Triangles[Order[I]];
			// RayData is built only from valid StableProxyTriangles, so the
			// topology index is guaranteed here. Avoid repeating IsValidIndex
			// and the extra branch for every candidate triangle of every ray.
			const FStableProxyTriangleGPU& G = Topology[T.TriangleIndex];
			if (G.Vertex0 == ReceiverVertexIndex || G.Vertex1 == ReceiverVertexIndex || G.Vertex2 == ReceiverVertexIndex)
			{
				continue;
			}
			float HitT = 0.0f;
			if (StableRayTriangleDistance(Origin, Direction, T, InOutClosestT, HitT))
			{
				InOutClosestT = HitT;
			}
		}
		return;
	}

	// Visit the nearer child first. The first hit then tightens InOutClosestT,
	// allowing the second child's box and triangles to be rejected earlier.
	// This changes traversal order only; both children are still queried, so
	// the visibility result is unchanged.
	float LeftNearT = BIG_NUMBER;
	float RightNearT = BIG_NUMBER;
	const bool bLeftHit = Nodes.IsValidIndex(Node.Left) &&
		StableRayBoxInterval(Origin, Direction, Nodes[Node.Left].BoundsMin, Nodes[Node.Left].BoundsMax, InOutClosestT, LeftNearT);
	const bool bRightHit = Nodes.IsValidIndex(Node.Right) &&
		StableRayBoxInterval(Origin, Direction, Nodes[Node.Right].BoundsMin, Nodes[Node.Right].BoundsMax, InOutClosestT, RightNearT);
	if (LeftNearT <= RightNearT)
	{
		if (bLeftHit)
		{
			QueryStableCastRayBVH(Triangles, Order, Nodes, Topology, Node.Left, Origin, Direction, ReceiverVertexIndex, InOutClosestT);
		}
		if (bRightHit)
		{
			QueryStableCastRayBVH(Triangles, Order, Nodes, Topology, Node.Right, Origin, Direction, ReceiverVertexIndex, InOutClosestT);
		}
	}
	else
	{
		if (bRightHit)
		{
			QueryStableCastRayBVH(Triangles, Order, Nodes, Topology, Node.Right, Origin, Direction, ReceiverVertexIndex, InOutClosestT);
		}
		if (bLeftHit)
		{
			QueryStableCastRayBVH(Triangles, Order, Nodes, Topology, Node.Left, Origin, Direction, ReceiverVertexIndex, InOutClosestT);
		}
	}
}

static TAutoConsoleVariable<int32> CVarPCVRAnyHit(TEXT("gs.PCVR.AnyHit"),1,
    TEXT("RuntimeMode 1 Final: exact binary occlusion using first hit; distance diagnostic modes retain closest-hit traversal."));
static TAutoConsoleVariable<int32> CVarPCVRValidateAnyHit(TEXT("gs.PCVR.ValidateAnyHit"),0,
    TEXT("Validation only: compare every runtime any-hit ray against the original closest-hit query. Adds CPU cost."));
static bool UsePCVRAnyHit(const UGaussianSplatComponent* Component)
{
    const AGaussianSplatActor* Actor=Cast<AGaussianSplatActor>(Component->GetOwner());
    return CVarPCVRAnyHit.GetValueOnGameThread()!=0 &&
        Component->LightingPreviewMode==ELCCCinematicGSLightingPreviewMode::Final &&
        Actor && static_cast<int32>(Actor->ShadowDebugMode)==0 &&
        GSPCVR::IsRuntimeWorld(Component->GetWorld());
}

// Same geometry, box and triangle predicates as the closest-hit authority.
// Final consumes visibility (X/Y), not closest blocker distance (W).
static bool QueryStableCastAnyHitBVH(
    const TArray<FStableBindingTriangle>& Triangles,
    const TArray<int32>& Order,
    const TArray<FStableBindingBVHNode>& Nodes,
    const TArray<FStableProxyTriangleGPU>& Topology,
    int32 NodeIndex, const FVector3f& Origin, const FVector3f& Direction,
    uint32 ReceiverVertexIndex, float MaxDistance)
{
    if (!Nodes.IsValidIndex(NodeIndex)) return false;
    const FStableBindingBVHNode& Node=Nodes[NodeIndex];
    float NearT=0;
    if (!StableRayBoxInterval(Origin,Direction,Node.BoundsMin,Node.BoundsMax,MaxDistance,NearT)) return false;
    if (Node.Left==INDEX_NONE && Node.Right==INDEX_NONE)
    {
        for (int32 I=Node.Start; I<Node.Start+Node.Count; ++I)
        {
            const auto& T=Triangles[Order[I]];
            const auto& G=Topology[T.TriangleIndex];
            if (G.Vertex0==ReceiverVertexIndex || G.Vertex1==ReceiverVertexIndex || G.Vertex2==ReceiverVertexIndex) continue;
            float HitT=0;
            if (StableRayTriangleDistance(Origin,Direction,T,MaxDistance,HitT)) return true;
        }
        return false;
    }
    float LeftNear=BIG_NUMBER,RightNear=BIG_NUMBER;
    const bool LeftHit=Nodes.IsValidIndex(Node.Left) && StableRayBoxInterval(Origin,Direction,
        Nodes[Node.Left].BoundsMin,Nodes[Node.Left].BoundsMax,MaxDistance,LeftNear);
    const bool RightHit=Nodes.IsValidIndex(Node.Right) && StableRayBoxInterval(Origin,Direction,
        Nodes[Node.Right].BoundsMin,Nodes[Node.Right].BoundsMax,MaxDistance,RightNear);
    const int32 First=LeftNear<=RightNear?Node.Left:Node.Right;
    const int32 Second=LeftNear<=RightNear?Node.Right:Node.Left;
    const bool FirstHit=LeftNear<=RightNear?LeftHit:RightHit;
    const bool SecondHit=LeftNear<=RightNear?RightHit:LeftHit;
    return (FirstHit && QueryStableCastAnyHitBVH(Triangles,Order,Nodes,Topology,First,Origin,Direction,ReceiverVertexIndex,MaxDistance)) ||
        (SecondHit && QueryStableCastAnyHitBVH(Triangles,Order,Nodes,Topology,Second,Origin,Direction,ReceiverVertexIndex,MaxDistance));
}

static FVector2f ComputeStableTriangleClosureOpenDual(
	const TArray<FStableBindingTriangle>& Triangles,
	const TArray<int32>& Order,
	const TArray<FStableBindingBVHNode>& Nodes,
	int32 RootNode,
	const FStableBindingTriangle& Triangle,
	float LocalRadiusCm,
	float BroadRadiusCm)
{
	const float LocalRadius = FMath::Max(LocalRadiusCm, 25.0f);
	const float BroadRadius = FMath::Max(BroadRadiusCm, LocalRadius + 25.0f);
	FVector3f N = Triangle.Normal.GetSafeNormal();
	if (N.IsNearlyZero())
	{
		return FVector2f(1.0f, 1.0f);
	}
	const FVector3f Reference = FMath::Abs(N.Z) < 0.85f ? FVector3f(0.0f, 0.0f, 1.0f) : FVector3f(1.0f, 0.0f, 0.0f);
	FVector3f Tangent = FVector3f::CrossProduct(Reference, N).GetSafeNormal();
	FVector3f Bitangent = FVector3f::CrossProduct(N, Tangent).GetSafeNormal();
	const float OriginBias = FMath::Clamp(LocalRadius * 0.004f, 0.6f, 2.0f);
	const FVector3f Origin = Triangle.Centroid + N * OriginBias;

	// v2745 dual-scale closure. One BVH trace is shared by the local and broad fields,
	// so the broad receiver area is added without doubling the ray-query cost.
	float LocalVisibilitySum = 0.0f;
	float BroadVisibilitySum = 0.0f;
	int32 RayCount = 0;
	auto OpennessAtRadius = [](float ClosestT, float Radius)
	{
		return ClosestT >= Radius - 1.0e-3f
			? 1.0f
			: FMath::Pow(FMath::Clamp(ClosestT / Radius, 0.0f, 1.0f), 0.70f);
	};
	auto TraceDirection = [&](const FVector3f& Dir)
	{
		float ClosestT = BroadRadius;
		QueryStableClosureRayBVH(
			Triangles, Order, Nodes, RootNode, Origin, Dir.GetSafeNormal(),
			Triangle.TriangleIndex, ClosestT);
		LocalVisibilitySum += OpennessAtRadius(ClosestT, LocalRadius);
		BroadVisibilitySum += OpennessAtRadius(ClosestT, BroadRadius);
		++RayCount;
	};

	TraceDirection(N);
	constexpr float RingUp = 0.62f;
	constexpr float RingSide = 0.7846018f; // sqrt(1 - RingUp^2)
	for (int32 I = 0; I < 6; ++I)
	{
		const float Angle = (2.0f * PI * float(I)) / 6.0f;
		const FVector3f Ring = Tangent * FMath::Cos(Angle) + Bitangent * FMath::Sin(Angle);
		TraceDirection(N * RingUp + Ring * RingSide);
	}

	if (RayCount <= 0)
	{
		return FVector2f(1.0f, 1.0f);
	}
	return FVector2f(
		FMath::Clamp(LocalVisibilitySum / float(RayCount), 0.0f, 1.0f),
		FMath::Clamp(BroadVisibilitySum / float(RayCount), 0.0f, 1.0f));
}

static uint64 HashStableBindings(const TArray<FStableProxySplatBindingGPU>& Bindings)
{
	uint64 Hash = 1469598103934665603ull;
	for (const FStableProxySplatBindingGPU& Binding : Bindings)
	{
		const uint32 Values[2] = { Binding.TriangleIndex, Binding.PackedBarycentric };
		for (uint32 Value : Values)
		{
			for (int32 ByteIndex = 0; ByteIndex < 4; ++ByteIndex)
			{
				Hash ^= uint64((Value >> (ByteIndex * 8)) & 0xFFu);
				Hash *= 1099511628211ull;
			}
		}
	}
	return Hash;
}
}

/** Opaque Phase-1B build context. It owns the in-progress full binding so the last completed
 *  front binding remains untouched until every splat has been processed. */
struct FStableProxyBindingBuildContext
{
	TArray<FStableBindingTriangle> Triangles;
	TArray<int32> TriangleOrder;
	TArray<FStableBindingBVHNode> BVHNodes;
	TArray<FStableProxySplatBindingGPU> PendingBindings;
	TArray<FStableProxyVertexGPU> PendingWeldedVertices;
	TArray<FStableProxyTriangleGPU> PendingTriangleVertices;
	// v2804: retain the authored normal at each source-triangle corner so Stable
	// direct lighting can reproduce the UE proxy's barycentric normal variation.
	TArray<FVector4f> PendingTriangleVertexNormals;
	TSet<uint32> UniqueTriangleIDs;
	int32 RootNode = INDEX_NONE;
	int32 NextSplatIndex = 0;
	int32 TotalSplatCount = 0;
	float MaxDistanceSq = 0.0f;
	double StartSeconds = 0.0;
	int32 LastLoggedProgressBucket = -1;
};

UGaussianSplatComponent::UGaussianSplatComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Lightweight light-value polling only. No captures, mesh uploads, or proxy
	// synchronization are performed from TickComponent.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickInterval = 0.05f;
	bTickInEditor = true;

	bUseAsOccluder = false;
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	SetGenerateOverlapEvents(false);

	// Enable dynamic rendering
	Mobility = EComponentMobility::Movable;

	CachedLightingData.Reset();
	PreviousSeasonMode = SeasonMode;
	PreviousWeatherMode = WeatherMode;
	RefreshCurrentSeasonPresetStatus();
	RefreshCurrentWeatherPresetStatus();
	RefreshCurrentWeatherFogProfileStatus();
	RefreshNumberedSeasonPresetStatus();
	RefreshNumberedWeatherPresetStatus();
}

void UGaussianSplatComponent::PostLoad()
{
	Super::PostLoad();

	// Preserve every serialized look, lighting, shadow, exposure, saturation and
	// sharpening value authored in the level. Render Foundation 1.1 changes only
	// the class defaults and applies Adaptive safety at render time; it never rewrites
	// the user's tuned color parameters during load.
	CachedLightingData.Reset();
	PreviousSeasonMode = SeasonMode;
	PreviousWeatherMode = WeatherMode;
	RefreshCurrentSeasonPresetStatus();
	RefreshCurrentWeatherPresetStatus();
	RefreshCurrentWeatherFogProfileStatus();
	RefreshNumberedSeasonPresetStatus();
	RefreshNumberedWeatherPresetStatus();

	// Clean only stale Fog-atmosphere values that older fog test builds could
	// serialize into Rain. Shadow/relighting values are intentionally untouched.
	if (WeatherMode == ELCCCinematicGSWeatherMode::Rain)
	{
		ResetRainFogLeakStateToOriginalBaseline(this, false);

		// v32 Rain-state migration. Older levels can carry a disabled/zero Wet state
		// even though WeatherMode is Rain, so rain particles appear but the surface
		// pass is silent. Repair ONLY clearly invalid legacy Rain state; preserve any
		// non-zero artist-authored Wet values.
		if (ProxySurfaceEffectMode != ELCCCinematicGSProxySurfaceEffectMode::WetPuddles)
		{
			ProxySurfaceEffectMode = ELCCCinematicGSProxySurfaceEffectMode::WetPuddles;
		}
		if (ProxySurfaceEffectStrength <= 0.0001f)
		{
			ProxySurfaceEffectStrength = 0.48f;
		}
		if (ProxySurfaceSpecularStrength <= 0.0001f)
		{
			ProxySurfaceSpecularStrength = 0.82f;
		}

		// v29-v31 introduced an intrinsic splat-size gate while these serialized
		// properties still showed the old 12/35 cm proxy-distance defaults. Migrate
		// ONLY that exact untouched legacy pair to the real v32 size thresholds.
		if (FMath::IsNearlyEqual(ProxyWetGroundFullDistanceCm, 12.0f, 0.001f) &&
			FMath::IsNearlyEqual(ProxyWetGroundMaxDistanceCm, 35.0f, 0.001f))
		{
			ProxyWetGroundFullDistanceCm = 80.0f;
			ProxyWetGroundMaxDistanceCm = 140.0f;
		}
	}

	// The v1.0.x spatial de-lighting experiment remains an advanced opt-in.
	// Do not overwrite an explicitly saved user choice during load.
	// Migrate the former default Hybrid World-Up mode to the new proxy-backed
	// hybrid only when this component already has a receiver proxy assigned.
	if (ProxyShadowMesh && bEnableProxySurfaceNormals &&
		UnrealLightingNormalMode == ELCCCinematicGSLightingNormalMode::HybridWorldUp)
	{
		UnrealLightingNormalMode = ELCCCinematicGSLightingNormalMode::HybridProxy;
	}
	// UI-restore migration: if a receiver proxy and explicit external casters are
	// already serialized, the component is clearly configured for proxy shadows.
	// Re-enable the capture path if an earlier UI cleanup left the enable flag off.
	if (ProxyShadowMesh && ExternalShadowCasters.Num() > 0)
	{
		bUseProxyMeshForShadowCatcher = true;
	}
	// v21.2 migration: only update untouched defaults of the two newly added
	// Gaussian-native light tests.  No shadow, closure, reflected-light, color,
	// exposure, or user-authored values are changed.
	if (FMath::IsNearlyEqual(GaussianSunAccentStrength, 0.24f, KINDA_SMALL_NUMBER))
	{
		GaussianSunAccentStrength = 0.48f;
	}
	if (FMath::IsNearlyEqual(GaussianPlantTransmissionStrength, 0.18f, KINDA_SMALL_NUMBER))
	{
		GaussianPlantTransmissionStrength = 0.32f;
	}
	// v2822 bright-day / scene-clock migration. Update only exact defaults from the
	// previous build so any artist-authored values are preserved.
	if (FMath::IsNearlyEqual(BrightDayExposureCompensation, -0.45f, KINDA_SMALL_NUMBER) ||
		FMath::IsNearlyEqual(BrightDayExposureCompensation, -0.75f, KINDA_SMALL_NUMBER) ||
		FMath::IsNearlyEqual(BrightDayExposureCompensation, -0.20f, KINDA_SMALL_NUMBER))
	{
		BrightDayExposureCompensation = 0.0f;
	}
	if (FMath::IsNearlyEqual(BrightDayExposureStartDegrees, 10.0f, KINDA_SMALL_NUMBER) ||
		FMath::IsNearlyEqual(BrightDayExposureStartDegrees, 18.0f, KINDA_SMALL_NUMBER))
	{
		BrightDayExposureStartDegrees = 35.0f;
	}
	if (FMath::IsNearlyEqual(BrightDayExposureFullDegrees, 38.0f, KINDA_SMALL_NUMBER) ||
		FMath::IsNearlyEqual(BrightDayExposureFullDegrees, 50.0f, KINDA_SMALL_NUMBER))
	{
		BrightDayExposureFullDegrees = 55.0f;
	}
	// v2796 night-visibility migration: v2795 stacked several full-night darkening
	// defaults (3% sky, 3.5% ambient, 18% carrier, gamma 1.45 and brightness 0.52).
	// Update only values that still exactly match those shipped v2795 defaults so any
	// artist-authored night look is preserved. Source PLY/originalLinear is untouched.
	if (FMath::IsNearlyEqual(NightAmbientIntensity, 0.035f, KINDA_SMALL_NUMBER) ||
		FMath::IsNearlyEqual(NightAmbientIntensity, 0.10f, KINDA_SMALL_NUMBER))
	{
		// Time-flow deep-night migration: move only untouched shipped defaults.
		NightAmbientIntensity = 0.065f;
	}
	if (FMath::IsNearlyEqual(NightSkyAtmosphereLuminanceScale, 0.03f, KINDA_SMALL_NUMBER))
	{
		NightSkyAtmosphereLuminanceScale = 0.10f;
	}
	if (FMath::IsNearlyEqual(NightSkyIndirectScale, 0.18f, KINDA_SMALL_NUMBER))
	{
		NightSkyIndirectScale = 0.35f;
	}
	if (FMath::IsNearlyEqual(NightGamma, 1.45f, KINDA_SMALL_NUMBER))
	{
		NightGamma = 1.12f;
	}
	if (FMath::IsNearlyEqual(NightSaturation, 0.68f, KINDA_SMALL_NUMBER))
	{
		NightSaturation = 0.78f;
	}
	if (FMath::IsNearlyEqual(NightBrightness, 0.52f, KINDA_SMALL_NUMBER) ||
		FMath::IsNearlyEqual(NightBrightness, 0.88f, KINDA_SMALL_NUMBER))
	{
		// Time-flow deep-night migration: preserve custom values and only update
		// the exact defaults shipped by the previous night-visibility pass.
		NightBrightness = 0.75f;
	}
	// v2797 broad twilight migration: update only untouched v2796 transition defaults.
	// Existing artist-authored timings are preserved.
	if (FMath::IsNearlyEqual(DayNightTransitionStart, 0.05f, KINDA_SMALL_NUMBER))
	{
		DayNightTransitionStart = 0.26f;
	}
	if (FMath::IsNearlyEqual(DayNightFullNight, -0.12f, KINDA_SMALL_NUMBER))
	{
		DayNightFullNight = -0.30f;
	}
	// v2798: migrate only untouched v2797 twilight defaults to the stronger real solar-phase look.
	if (FMath::IsNearlyEqual(TwilightSkyAtmosphereTintStrength, 0.38f, KINDA_SMALL_NUMBER)) TwilightSkyAtmosphereTintStrength = 0.78f;
	if (FMath::IsNearlyEqual(TwilightColorGradeStrength, 0.58f, KINDA_SMALL_NUMBER)) TwilightColorGradeStrength = 0.78f;
	if (FMath::IsNearlyEqual(TwilightGamma, 0.94f, KINDA_SMALL_NUMBER)) TwilightGamma = 0.92f;
	if (FMath::IsNearlyEqual(TwilightSaturation, 1.08f, KINDA_SMALL_NUMBER)) TwilightSaturation = 1.16f;
	if (FMath::IsNearlyEqual(TwilightBrightness, 0.96f, KINDA_SMALL_NUMBER)) TwilightBrightness = 1.00f;
	// v2799: migrate untouched v2798 defaults to the slower, separate dusk/dawn look.
	if (FMath::IsNearlyEqual(TimeOfDayGlobalTintStrength, 0.82f, KINDA_SMALL_NUMBER)) TimeOfDayGlobalTintStrength = 0.74f;
	if (FMath::IsNearlyEqual(TwilightColorGradeStrength, 0.78f, KINDA_SMALL_NUMBER)) TwilightColorGradeStrength = 0.72f;
	if (FMath::IsNearlyEqual(TwilightGamma, 0.92f, KINDA_SMALL_NUMBER)) TwilightGamma = 0.94f;
	if (FMath::IsNearlyEqual(TwilightSaturation, 1.16f, KINDA_SMALL_NUMBER)) TwilightSaturation = 1.12f;
	// v2808 stable-normal authority migration. v2807 diagnostics showed fixed floor holes
	// wherever untouched 35/90 cm distance blending dropped back to noisy GS normals.
	// Only migrate the shipped v2804-v2807 defaults; artist-authored distances remain intact.
	if (FMath::IsNearlyEqual(StableLightingNormalFullDistanceCm, 35.0f, KINDA_SMALL_NUMBER) &&
		FMath::IsNearlyEqual(StableLightingNormalMaxDistanceCm, 90.0f, KINDA_SMALL_NUMBER))
	{
		StableLightingNormalFullDistanceCm = 80.0f;
		StableLightingNormalMaxDistanceCm = 220.0f;
	}
	TargetCompositeDebugView = ELCCCinematicGSCompositeDebugView::Final;
	// v2778 production cleanup: the component-level legacy lighting/shadow preview
	// selector is retired. The actor 01/02/03 selector is the only public render mode.
	LightingPreviewMode = ELCCCinematicGSLightingPreviewMode::Final;
	// Retired snapshot flags may still deserialize as true from older levels, but they
	// no longer participate in Final. Normalize them so runtime state is unambiguous.
	bOneShotCastShadowLatch = false;
	bFreezeStableShadowField = false;
	bCastShadowSnapshotLatched = false;
	CastShadowSnapshotCaptureCount = 0;
	CastShadowSnapshotIgnoredChanges = 0;
	StableFieldIgnoredLightChanges = 0;
}

void UGaussianSplatComponent::RefreshSpatialDelightingCacheStatus()
{
	SpatialDelightingCacheStatus = ELCCCinematicGSSpatialDelightingCacheStatus::Unknown;
	SpatialDelightingGridCellCount = 0;
	SpatialDelightingOccupiedCellCount = 0;
	SpatialDelightingCoveragePercent = 0.0f;
	SpatialDelightingAverageConfidence = 0.0f;
	SpatialDelightingGridCellSize = 0.0f;

	if (!SplatAsset || !SplatAsset->IsValid())
	{
		return;
	}

	const TSharedPtr<FGaussianSplatRenderData> RenderData = SplatAsset->GetOrCreateRenderData();
	if (!RenderData.IsValid() || !RenderData->IsInitialized())
	{
		return;
	}

	switch (RenderData->SpatialLightingCacheStatus)
	{
	case 1u:
		SpatialDelightingCacheStatus = ELCCCinematicGSSpatialDelightingCacheStatus::ValidSpatialGrid;
		break;
	case 2u:
		SpatialDelightingCacheStatus = ELCCCinematicGSSpatialDelightingCacheStatus::MissingColorData;
		break;
	case 3u:
		SpatialDelightingCacheStatus = ELCCCinematicGSSpatialDelightingCacheStatus::InvalidPositionData;
		break;
	case 4u:
		SpatialDelightingCacheStatus = ELCCCinematicGSSpatialDelightingCacheStatus::NoMappedSamples;
		break;
	default:
		SpatialDelightingCacheStatus = ELCCCinematicGSSpatialDelightingCacheStatus::Unknown;
		break;
	}

	SpatialDelightingGridCellCount = RenderData->SpatialLightingCellCount;
	SpatialDelightingOccupiedCellCount = RenderData->SpatialLightingOccupiedCellCount;
	SpatialDelightingCoveragePercent = RenderData->SpatialLightingCoverage * 100.0f;
	SpatialDelightingAverageConfidence = RenderData->SpatialLightingAverageConfidence;
	SpatialDelightingGridCellSize = RenderData->SpatialLightingCellSize;
}

void UGaussianSplatComponent::RebuildProxyNormalCache()
{
	ProxyNormalGridData.Reset();
	ProxyNormalGridMinCellSize = FVector4f(0, 0, 0, 1);
	ProxyNormalGridDimensionsStatus = FVector4f(0, 0, 0, 0);
	ProxyNormalCoveragePercent = 0.0f;
	ProxyNormalGridCellCount = 0;
	ProxyNormalCacheStatus = bEnableProxySurfaceNormals
		? ELCCCinematicGSProxyNormalCacheStatus::MissingProxyMesh
		: ELCCCinematicGSProxyNormalCacheStatus::Disabled;

	if (!bEnableProxySurfaceNormals)
	{
		MarkRenderStateDirty();
		return;
	}
	if (!SplatAsset || !SplatAsset->IsValid() || !ProxyShadowMesh)
	{
		MarkRenderStateDirty();
		return;
	}

	const FStaticMeshRenderData* MeshRenderData = ProxyShadowMesh->GetRenderData();
	if (!MeshRenderData || MeshRenderData->LODResources.Num() <= 0)
	{
		ProxyNormalCacheStatus = ELCCCinematicGSProxyNormalCacheStatus::MissingRenderData;
		MarkRenderStateDirty();
		return;
	}

	const FStaticMeshLODResources& LOD = MeshRenderData->LODResources[0];
	const FPositionVertexBuffer& PositionBuffer = LOD.VertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& VertexBuffer = LOD.VertexBuffers.StaticMeshVertexBuffer;
	const uint32 VertexCount = PositionBuffer.GetNumVertices();
	if (VertexCount == 0 || VertexBuffer.GetNumVertices() < VertexCount)
	{
		ProxyNormalCacheStatus = ELCCCinematicGSProxyNormalCacheStatus::MissingRenderData;
		MarkRenderStateDirty();
		return;
	}

	// v2804: size the normal grid from the actual transformed proxy, not the entire
	// Gaussian asset bounds. A few distant/outlier splats can make the GS bounds many
	// times larger than the receiver mesh, collapsing normal-grid resolution and making
	// Stable lighting fall back to one coarse triangle normal over broad areas.
	const FTransform ProxyToSplat = GetProxyShadowMeshLocalTransform();
	FBox ProxyBounds(ForceInit);
	for (uint32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
	{
		ProxyBounds += ProxyToSplat.TransformPosition(FVector(PositionBuffer.VertexPosition(VertexIndex)));
	}
	const float GridMarginCm = FMath::Max(ProxyNormalGridMarginCm, 0.0f);
	if (ProxyBounds.IsValid && GridMarginCm > 0.0f)
	{
		ProxyBounds = ProxyBounds.ExpandBy(GridMarginCm);
	}
	const FVector BoundsSize = ProxyBounds.GetSize();
	const double MaxExtent = FMath::Max3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z);
	if (!ProxyBounds.IsValid || MaxExtent <= UE_SMALL_NUMBER)
	{
		ProxyNormalCacheStatus = ELCCCinematicGSProxyNormalCacheStatus::NoSamples;
		MarkRenderStateDirty();
		return;
	}

	const int32 TargetResolution = FMath::Clamp(ProxyNormalGridResolution, 16, 128);
	constexpr int32 MaxAxisCells = 128;
	constexpr int64 MaxCellCount = 262144;
	float CellSize = FMath::Max(static_cast<float>(MaxExtent / TargetResolution), 0.01f);
	auto ComputeDimensions = [&BoundsSize, MaxAxisCells](float InCellSize)
	{
		return FIntVector(
			FMath::Clamp(FMath::CeilToInt(BoundsSize.X / InCellSize) + 2, 1, MaxAxisCells),
			FMath::Clamp(FMath::CeilToInt(BoundsSize.Y / InCellSize) + 2, 1, MaxAxisCells),
			FMath::Clamp(FMath::CeilToInt(BoundsSize.Z / InCellSize) + 2, 1, MaxAxisCells));
	};
	FIntVector GridDimensions = ComputeDimensions(CellSize);
	while (static_cast<int64>(GridDimensions.X) * GridDimensions.Y * GridDimensions.Z > MaxCellCount)
	{
		CellSize *= 1.12f;
		GridDimensions = ComputeDimensions(CellSize);
	}

	const int32 CellCount = GridDimensions.X * GridDimensions.Y * GridDimensions.Z;
	const FVector GridMin = ProxyBounds.Min - FVector(CellSize);
	TArray<FVector3f> NormalSums;
	TArray<float> SampleWeights;
	NormalSums.Init(FVector3f::ZeroVector, CellCount);
	SampleWeights.Init(0.0f, CellCount);

	auto CellIndexForPosition = [&GridMin, CellSize, GridDimensions](const FVector& P) -> int32
	{
		const FVector Relative = (P - GridMin) / CellSize;
		const FIntVector Cell(FMath::FloorToInt(Relative.X), FMath::FloorToInt(Relative.Y), FMath::FloorToInt(Relative.Z));
		if (Cell.X < 0 || Cell.Y < 0 || Cell.Z < 0 || Cell.X >= GridDimensions.X || Cell.Y >= GridDimensions.Y || Cell.Z >= GridDimensions.Z)
		{
			return INDEX_NONE;
		}
		return Cell.X + GridDimensions.X * (Cell.Y + GridDimensions.Y * Cell.Z);
	};

	auto AddSample = [&](const FVector& ProxyLocalPosition, const FVector& ProxyLocalNormal, float Weight)
	{
		const FVector SplatLocalPosition = ProxyToSplat.TransformPosition(ProxyLocalPosition);
		const int32 CellIndex = CellIndexForPosition(SplatLocalPosition);
		if (CellIndex == INDEX_NONE)
		{
			return;
		}
		const FVector SplatLocalNormal = ProxyToSplat.TransformVectorNoScale(ProxyLocalNormal).GetSafeNormal();
		if (SplatLocalNormal.IsNearlyZero())
		{
			return;
		}
		NormalSums[CellIndex] += FVector3f(SplatLocalNormal) * Weight;
		SampleWeights[CellIndex] += Weight;
	};

	for (uint32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
	{
		const FVector3f Position = PositionBuffer.VertexPosition(VertexIndex);
		const FVector4f TangentZ = VertexBuffer.VertexTangentZ(VertexIndex);
		AddSample(FVector(Position), FVector(TangentZ.X, TangentZ.Y, TangentZ.Z), 1.0f);
	}

	const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
	const int32 TriangleCount = Indices.Num() / 3;
	const int32 TriangleStride = FMath::Max(1, FMath::DivideAndRoundUp(TriangleCount, 200000));
	for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; TriangleIndex += TriangleStride)
	{
		const uint32 I0 = Indices[TriangleIndex * 3 + 0];
		const uint32 I1 = Indices[TriangleIndex * 3 + 1];
		const uint32 I2 = Indices[TriangleIndex * 3 + 2];
		if (I0 >= VertexCount || I1 >= VertexCount || I2 >= VertexCount)
		{
			continue;
		}
		const FVector P0(PositionBuffer.VertexPosition(I0));
		const FVector P1(PositionBuffer.VertexPosition(I1));
		const FVector P2(PositionBuffer.VertexPosition(I2));
		const FVector FaceNormal = FVector::CrossProduct(P1 - P0, P2 - P0).GetSafeNormal();
		if (FaceNormal.IsNearlyZero())
		{
			continue;
		}
		const float MaxEdge = FMath::Max3((P1 - P0).Size(), (P2 - P1).Size(), (P0 - P2).Size());
		const int32 Subdivisions = FMath::Clamp(FMath::CeilToInt(MaxEdge / FMath::Max(CellSize, 1.0f)), 1, 4);
		for (int32 U = 0; U <= Subdivisions; ++U)
		{
			for (int32 V = 0; V <= Subdivisions - U; ++V)
			{
				const float FU = static_cast<float>(U) / Subdivisions;
				const float FV = static_cast<float>(V) / Subdivisions;
				const float FW = 1.0f - FU - FV;
				AddSample(P0 * FW + P1 * FU + P2 * FV, FaceNormal, 1.5f);
			}
		}
	}

	TArray<FVector3f> GridNormals;
	TArray<float> GridConfidence;
	GridNormals.Init(FVector3f::ZeroVector, CellCount);
	GridConfidence.Init(0.0f, CellCount);
	int32 OccupiedCells = 0;
	for (int32 CellIndex = 0; CellIndex < CellCount; ++CellIndex)
	{
		if (SampleWeights[CellIndex] > 0.0f)
		{
			const float NormalMagnitude = NormalSums[CellIndex].Size();
			const float DirectionalCoherence = FMath::Clamp(NormalMagnitude / SampleWeights[CellIndex], 0.0f, 1.0f);
			GridNormals[CellIndex] = NormalSums[CellIndex].GetSafeNormal();
			const float SampleConfidence = FMath::Clamp(SampleWeights[CellIndex] / 5.0f, 0.0f, 1.0f);
			GridConfidence[CellIndex] = SampleConfidence * DirectionalCoherence;
			++OccupiedCells;
		}
	}

	const int32 PropagationIterations = FMath::Clamp(ProxyNormalPropagationCells, 0, 4);
	const FIntVector NeighborOffsets[6] = {
		FIntVector(-1, 0, 0), FIntVector(1, 0, 0), FIntVector(0, -1, 0),
		FIntVector(0, 1, 0), FIntVector(0, 0, -1), FIntVector(0, 0, 1) };
	for (int32 Iteration = 0; Iteration < PropagationIterations; ++Iteration)
	{
		TArray<FVector3f> NextNormals = GridNormals;
		TArray<float> NextConfidence = GridConfidence;
		for (int32 Z = 0; Z < GridDimensions.Z; ++Z)
		{
			for (int32 Y = 0; Y < GridDimensions.Y; ++Y)
			{
				for (int32 X = 0; X < GridDimensions.X; ++X)
				{
					const int32 CellIndex = X + GridDimensions.X * (Y + GridDimensions.Y * Z);
					if (GridConfidence[CellIndex] > 0.0f)
					{
						continue;
					}
					FVector3f SumNormal = FVector3f::ZeroVector;
					float SumConfidence = 0.0f;
					int32 NeighborCount = 0;
					for (const FIntVector& Offset : NeighborOffsets)
					{
						const FIntVector N(X + Offset.X, Y + Offset.Y, Z + Offset.Z);
						if (N.X < 0 || N.Y < 0 || N.Z < 0 || N.X >= GridDimensions.X || N.Y >= GridDimensions.Y || N.Z >= GridDimensions.Z)
						{
							continue;
						}
						const int32 NeighborIndex = N.X + GridDimensions.X * (N.Y + GridDimensions.Y * N.Z);
						if (GridConfidence[NeighborIndex] <= 0.0f)
						{
							continue;
						}
						SumNormal += GridNormals[NeighborIndex] * GridConfidence[NeighborIndex];
						SumConfidence += GridConfidence[NeighborIndex];
						++NeighborCount;
					}
					if (NeighborCount > 0 && !SumNormal.IsNearlyZero())
					{
						NextNormals[CellIndex] = SumNormal.GetSafeNormal();
						NextConfidence[CellIndex] = FMath::Clamp((SumConfidence / NeighborCount) * 0.55f, 0.0f, 0.65f);
					}
				}
			}
		}
		GridNormals = MoveTemp(NextNormals);
		GridConfidence = MoveTemp(NextConfidence);
	}

	ProxyNormalGridData.SetNumUninitialized(CellCount);
	int32 CoveredCells = 0;
	for (int32 CellIndex = 0; CellIndex < CellCount; ++CellIndex)
	{
		const FVector3f N = GridNormals[CellIndex].GetSafeNormal();
		const float Confidence = FMath::Clamp(GridConfidence[CellIndex], 0.0f, 1.0f);
		if (Confidence > 0.0f)
		{
			++CoveredCells;
		}
		const FVector3f Encoded = N * 0.5f + FVector3f(0.5f, 0.5f, 0.5f);
		ProxyNormalGridData[CellIndex] = FFloat16Color(FLinearColor(Encoded.X, Encoded.Y, Encoded.Z, Confidence));
	}

	if (CoveredCells <= 0)
	{
		ProxyNormalGridData.Reset();
		ProxyNormalCacheStatus = ELCCCinematicGSProxyNormalCacheStatus::NoSamples;
		MarkRenderStateDirty();
		return;
	}

	ProxyNormalGridMinCellSize = FVector4f(static_cast<float>(GridMin.X), static_cast<float>(GridMin.Y), static_cast<float>(GridMin.Z), CellSize);
	ProxyNormalGridDimensionsStatus = FVector4f(static_cast<float>(GridDimensions.X), static_cast<float>(GridDimensions.Y), static_cast<float>(GridDimensions.Z), 1.0f);
	ProxyNormalGridCellCount = CellCount;
	ProxyNormalCoveragePercent = 100.0f * static_cast<float>(CoveredCells) / FMath::Max(CellCount, 1);
	ProxyNormalCacheStatus = ELCCCinematicGSProxyNormalCacheStatus::Valid;

	UE_LOG(LogTemp, Log, TEXT("LCCCinematicGS: Built proxy-centric normal grid for '%s': %dx%dx%d, CellSize=%.2f, Margin=%.1fcm, DirectCells=%d, Coverage=%.2f%%, Cache=%.2f MiB"),
		*GetName(), GridDimensions.X, GridDimensions.Y, GridDimensions.Z, CellSize, GridMarginCm, OccupiedCells, ProxyNormalCoveragePercent,
		double(ProxyNormalGridData.Num() * sizeof(FFloat16Color)) / (1024.0 * 1024.0));
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::PreviewLegacyPackedPositions()
{
	Modify();
	RuntimePositionPrecision = ELCCCinematicGSRuntimePositionPrecision::LegacyPackedFP16;
	MarkPackageDirty();
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::PreviewFloat32Positions()
{
	Modify();
	RuntimePositionPrecision = ELCCCinematicGSRuntimePositionPrecision::Float32Position;
	MarkPackageDirty();
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::PreviewLegacyPackedColorOpacity()
{
	Modify();
	RuntimeColorPrecision = ELCCCinematicGSRuntimeColorPrecision::LegacyPackedRGBA8;
	MarkPackageDirty();
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::PreviewFloat16ColorOpacity()
{
	Modify();
	RuntimeColorPrecision = ELCCCinematicGSRuntimeColorPrecision::Float16ColorOpacity;
	MarkPackageDirty();
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::PreviewLegacyProjectedFootprint()
{
	Modify();
	RuntimeFootprintMode = ELCCCinematicGSRuntimeFootprintMode::LegacyProjectedFootprint;
	MarkPackageDirty();
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::PreviewAdaptiveEWAFootprint()
{
	Modify();
	RuntimeFootprintMode = ELCCCinematicGSRuntimeFootprintMode::AdaptiveEWA;
	MarkPackageDirty();
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::PreviewFinalLighting()
{
	LightingPreviewMode = ELCCCinematicGSLightingPreviewMode::Final;
	MarkRenderStateDirty();
	SyncOwnerProxyShadowReceiver(this);
}

void UGaussianSplatComponent::PreviewLightMask()
{
	LightingPreviewMode = ELCCCinematicGSLightingPreviewMode::LightMask;
	MarkRenderStateDirty();
	SyncOwnerProxyShadowReceiver(this);
}

void UGaussianSplatComponent::PreviewShadowMask()
{
	LightingPreviewMode = ELCCCinematicGSLightingPreviewMode::ShadowMask;
	MarkRenderStateDirty();
	SyncOwnerProxyShadowReceiver(this);
}

void UGaussianSplatComponent::PreviewBlockerDistance()
{
	LightingPreviewMode = ELCCCinematicGSLightingPreviewMode::BlockerDistance;
	MarkRenderStateDirty();
	SyncOwnerProxyShadowReceiver(this);
}

void UGaussianSplatComponent::ApplyTargetDaylightPreset()
{
	Modify();
	bEnableCinematicEffects = true;
	bReceiveUnrealLights = true;
	RuntimeColorPrecision = ELCCCinematicGSRuntimeColorPrecision::Float16ColorOpacity;
	bEnableRelightingCore = true;
	RelightStrength = 0.82f;
	ApproxAlbedoStrength = 0.78f;
	ApproxAlbedoShadowLift = 0.14f;
	ApproxAlbedoHighlightReduction = 0.60f;
	TargetLookStrength = 1.0f;
	TargetExposureEV = -0.38f;
	TargetToeStrength = 0.0f;
	TargetChromaRecovery = 1.52f;
	TargetShadowColorRetention = 0.95f;
	TargetGreenBoost = 0.12f;
	TargetMidtoneContrast = 1.68f;
	SunlitGradeStrength = 1.0f;
	SunlitGamma = 1.0f;
	SunlitSaturation = 1.0f;
	SunlitBrightness = 1.0f;
	ShadowGradeStrength = 1.0f;
	ShadowAreaGamma = 1.0f;
	ShadowAreaSaturation = 1.0f;
	ShadowAreaBrightness = 1.0f;
	TargetShadowLift = 0.018f;
	TargetSunTint = FLinearColor(1.12f, 0.88f, 0.60f, 1.0f);
	TargetSkyTint = FLinearColor(0.72f, 0.84f, 1.0f, 1.0f);
	TargetDirectScale = 0.82f;
	TargetBakedSunSuppression = 0.68f;
	TargetFilmicShoulderStart = 0.68f;
	TargetFilmicShoulderStrength = 0.32f;
	TargetGreenSaturationRecovery = 0.32f;
	TargetBrightGreenCompression = 0.10f;
	TargetGreenLuminanceReduction = 0.14f;
	TargetMicroContrast = 0.30f;
	TargetCoarseContrast = 0.14f;
	TargetSplatSharpness = 0.28f;
	TargetHighlightSharpenProtection = 0.90f;
	TargetCoverageEdgeProtection = 0.90f;
	TargetCompositeDebugView = ELCCCinematicGSCompositeDebugView::Final;
	UnrealLightInfluence = 0.62f;
	UnrealAmbientIntensity = 0.19f;
	UnrealAmbientColor = FLinearColor(0.90f, 0.94f, 1.0f, 1.0f);
	UnrealIndirectLightIntensity = 0.15f;
	UnrealGroundBounce = 0.07f;
	UnrealShadowSaturation = 1.0f;
	UnrealShadowTint = FLinearColor(0.92f, 0.96f, 1.02f, 1.0f);
	CapturedBaseShadowTransferStrength = 0.56f;
	CapturedBaseShadowMinimum = 0.66f;
	CapturedBaseShadowSaturation = 1.0f;
	CapturedBaseShadowTint = FLinearColor(0.94f, 0.97f, 1.0f, 1.0f);
	CapturedReflectedLightTintStrength = 0.15f;
	CapturedReflectedLightDeepTintNeutralization = 0.70f;
	CapturedReflectedLightDeepSaturationRetention = 0.94f;
	CapturedReflectedLightHighlightCompression = 0.35f;
	CapturedReflectedLightTintSoftness = 0.45f;
	CapturedShadowBounceStrength = 0.26f;
	CapturedReflectedLightClosureOcclusion = 0.32f;
	CapturedReflectedLightResponseGamma = 0.78f;
	CapturedReflectedLightHorizonRetention = 0.0f;
	CapturedShadowSoftness = 0.85f;
	CapturedShadowGamma = 0.85f;
	CapturedShadowMinimum = 0.08f;
	ProxyLightingMatchStrength = 1.0f;
	CapturedClosedShadowMaxDarkness = 1.0f;
	CapturedClosedShadowFullBlackEnd = 0.22f;
	CapturedClosedShadowStart = 0.65f;
	CapturedClosedShadowAreaScale = 1.0f;
	CapturedClosedShadowEdgeSoftness = 1.0f;
	CapturedDirectLightWhitePoint = 0.25f;
	CapturedDirectLightResponseGamma = 0.85f;
	CapturedOcclusionShadowBoost = 1.35f;
	CapturedShadowPhysicalSoftness = 0.85f;
	UnrealDirectionalResponse = 0.90f;
	UnrealLightWrap = 0.10f;
	UnrealLightingNormalMode = ELCCCinematicGSLightingNormalMode::HybridProxy;
	bEnableSpatialDelighting = true;
	SpatialDelightingStrength = 0.68f;
	SpatialDelightingChromaStrength = 0.12f;
	SpatialDelightingMaxCorrectionWeight = 0.58f;
	SpatialDelightingFoliageProtection = 0.78f;
	SpatialDelightingFoliageCorrectionScale = 0.24f;
	AtmosphereStrength = 0.28f;
	GroundFogDensity = 0.16f;
	DistanceHaze = 0.30f;
	WarmSunScatter = 0.34f;
	HighlightBloomBoost = 0.72f;
	SunExposureBoost = 0.22f;
	HighlightGlareScale = 0.90f;
	LocalBloomStrength = 0.82f;
	LocalBloomGlareMix = 0.34f;
	NearFogWeight = 0.02f;
	DistanceFogWeight = 0.24f;
	LightingPreviewMode = ELCCCinematicGSLightingPreviewMode::Final;
	RenderProfile = ELCCCinematicGSRenderProfile::HighQuality;
	MarkPackageDirty();
	UpdateCachedSceneLighting();
	RebuildStableProxyRayVisibility();
	MarkRenderDynamicDataDirty();
	MarkRenderStateDirty();
	SyncOwnerProxyShadowReceiver(this);
}

FLCCCinematicGSSeasonPresetSlot* UGaussianSplatComponent::GetSeasonPresetSlot(ELCCCinematicGSSeasonMode Mode)
{
	switch (Mode)
	{
	case ELCCCinematicGSSeasonMode::Spring: return &SpringPresetSlot;
	case ELCCCinematicGSSeasonMode::Summer: return &SummerPresetSlot;
	case ELCCCinematicGSSeasonMode::Autumn: return &AutumnPresetSlot;
	case ELCCCinematicGSSeasonMode::Winter: return &WinterPresetSlot;
	default: return nullptr;
	}
}

static bool IsSeasonPresetProperty(const FName Name)
{
	// Season owns only vegetation/foliage recoloring. Weather, lighting, exposure,
	// shadows, atmosphere and surface effects are deliberately excluded.
	static const TSet<FName> SeasonProperties = {
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableSeasonalColor),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bPreviewSeasonColorMask),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonSourceColorFrom),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonSourceColorTo),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonTargetColorFrom),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonTargetColorTo),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonColorTolerance),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonColorFeather),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonMinSaturation),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonMinGreenDominance),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonSaturationScale),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonBrightnessScale),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonReplacementStrength),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonOriginalDetailPreservation),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonTargetStartColorBias)
	};
	return SeasonProperties.Contains(Name);
}

void UGaussianSplatComponent::CaptureCurrentEffectsToSeason(ELCCCinematicGSSeasonMode Mode, bool bMarkAsUserSave)
{
	FLCCCinematicGSSeasonPresetSlot* Slot = GetSeasonPresetSlot(Mode);
	if (!Slot)
	{
		return;
	}

	Slot->PropertyValues.Reset();
	for (TFieldIterator<FProperty> It(StaticClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (Property->GetOwnerClass() != StaticClass() || !IsSeasonPresetProperty(Property->GetFName()))
		{
			continue;
		}

		FString ExportedValue;
		Property->ExportText_InContainer(0, ExportedValue, this, this, this, PPF_None);
		Slot->PropertyValues.Add(Property->GetFName(), MoveTemp(ExportedValue));
	}
	Slot->bValid = Slot->PropertyValues.Num() > 0;
	if (bMarkAsUserSave)
	{
		Slot->bUserSaved = true;
		++Slot->UserSaveRevision;
	}
	if (Mode == SeasonMode)
	{
		RefreshCurrentSeasonPresetStatus();
	}
}

bool UGaussianSplatComponent::RestoreEffectsFromSeason(ELCCCinematicGSSeasonMode Mode)
{
	FLCCCinematicGSSeasonPresetSlot* Slot = GetSeasonPresetSlot(Mode);
	if (!Slot || !Slot->bValid)
	{
		return false;
	}

	for (const TPair<FName, FString>& Entry : Slot->PropertyValues)
	{
		if (IsSeasonPresetProperty(Entry.Key))
		{
			if (FProperty* Property = FindFProperty<FProperty>(StaticClass(), Entry.Key))
			{
				Property->ImportText_InContainer(*Entry.Value, this, this, PPF_None);
			}
		}
	}
	RefreshCurrentSeasonPresetStatus();
	return true;
}

void UGaussianSplatComponent::RefreshCurrentSeasonPresetStatus()
{
	if (FLCCCinematicGSSeasonPresetSlot* Slot = GetSeasonPresetSlot(SeasonMode))
	{
		LastSavedSeasonOptionCount = 0;
		for (const TPair<FName, FString>& Entry : Slot->PropertyValues)
		{
			LastSavedSeasonOptionCount += IsSeasonPresetProperty(Entry.Key) ? 1 : 0;
		}
		bCurrentSeasonUserPresetSaved = Slot->bUserSaved;
		CurrentSeasonUserSaveRevision = Slot->UserSaveRevision;
	}
	else
	{
		LastSavedSeasonOptionCount = 0;
		bCurrentSeasonUserPresetSaved = false;
		CurrentSeasonUserSaveRevision = 0;
	}
}

void UGaussianSplatComponent::SaveCurrentSeasonPreset()
{
	Modify();
	CaptureCurrentEffectsToSeason(SeasonMode, true);
	PreviousSeasonMode = SeasonMode;
	bSaveCurrentSeasonPresetNow = false;
	MarkPackageDirty();
#if WITH_EDITOR
	if (UWorld* World = GetWorld())
	{
		if (World->PersistentLevel)
		{
			FEditorFileUtils::SaveLevel(World->PersistentLevel);
		}
	}
#endif
}

void UGaussianSplatComponent::RefreshNumberedSeasonPresetStatus()
{
	NumberedSeasonPresetIndex = FMath::Clamp(NumberedSeasonPresetIndex, 1, 999);
	if (const FLCCCinematicGSSeasonPresetSlot* Slot = NumberedSeasonPresetSlots.Find(NumberedSeasonPresetIndex))
	{
		bSelectedNumberedSeasonPresetSaved = Slot->bValid && Slot->bUserSaved;
		SelectedNumberedSeasonOptionCount = 0;
		for (const TPair<FName, FString>& Entry : Slot->PropertyValues)
		{
			SelectedNumberedSeasonOptionCount += IsSeasonPresetProperty(Entry.Key) ? 1 : 0;
		}
		SelectedNumberedSeasonSaveRevision = Slot->UserSaveRevision;
	}
	else
	{
		bSelectedNumberedSeasonPresetSaved = false;
		SelectedNumberedSeasonOptionCount = 0;
		SelectedNumberedSeasonSaveRevision = 0;
	}
}

void UGaussianSplatComponent::SaveNumberedSeasonPreset()
{
	Modify();
	NumberedSeasonPresetIndex = FMath::Clamp(NumberedSeasonPresetIndex, 1, 999);
	FLCCCinematicGSSeasonPresetSlot& Slot = NumberedSeasonPresetSlots.FindOrAdd(NumberedSeasonPresetIndex);
	Slot.PropertyValues.Reset();
	for (TFieldIterator<FProperty> It(StaticClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (Property->GetOwnerClass() != StaticClass() || !IsSeasonPresetProperty(Property->GetFName()))
		{
			continue;
		}
		FString ExportedValue;
		Property->ExportText_InContainer(0, ExportedValue, this, this, this, PPF_None);
		Slot.PropertyValues.Add(Property->GetFName(), MoveTemp(ExportedValue));
	}
	Slot.bValid = Slot.PropertyValues.Num() > 0;
	Slot.bUserSaved = Slot.bValid;
	++Slot.UserSaveRevision;
	bSaveNumberedSeasonPresetNow = false;
	RefreshNumberedSeasonPresetStatus();
	MarkPackageDirty();
#if WITH_EDITOR
	if (UWorld* World = GetWorld())
	{
		if (World->PersistentLevel)
		{
			FEditorFileUtils::SaveLevel(World->PersistentLevel);
		}
	}
#endif
	UE_LOG(LogTemp, Display, TEXT("[GSSeasonPreset] Saved Slot=%d Options=%d Revision=%d"),
		NumberedSeasonPresetIndex, Slot.PropertyValues.Num(), Slot.UserSaveRevision);
}

void UGaussianSplatComponent::LoadNumberedSeasonPreset()
{
	Modify();
	NumberedSeasonPresetIndex = FMath::Clamp(NumberedSeasonPresetIndex, 1, 999);
	bLoadNumberedSeasonPresetNow = false;
	const FLCCCinematicGSSeasonPresetSlot* Slot = NumberedSeasonPresetSlots.Find(NumberedSeasonPresetIndex);
	if (!Slot || !Slot->bValid || !Slot->bUserSaved)
	{
		RefreshNumberedSeasonPresetStatus();
		UE_LOG(LogTemp, Warning, TEXT("[GSSeasonPreset] Load skipped: Slot=%d is empty"), NumberedSeasonPresetIndex);
		return;
	}
	for (const TPair<FName, FString>& Entry : Slot->PropertyValues)
	{
		if (IsSeasonPresetProperty(Entry.Key))
		{
			if (FProperty* Property = FindFProperty<FProperty>(StaticClass(), Entry.Key))
			{
				Property->ImportText_InContainer(*Entry.Value, this, this, PPF_None);
			}
		}
	}
	RefreshNumberedSeasonPresetStatus();
	MarkPackageDirty();
	UpdateCachedSceneLighting();
	MarkRenderDynamicDataDirty();
	MarkRenderStateDirty();
	UE_LOG(LogTemp, Display, TEXT("[GSSeasonPreset] Loaded Slot=%d Options=%d Revision=%d"),
		NumberedSeasonPresetIndex, SelectedNumberedSeasonOptionCount, Slot->UserSaveRevision);
}

FLCCCinematicGSSeasonPresetSlot* UGaussianSplatComponent::GetWeatherPresetSlot(ELCCCinematicGSWeatherMode Mode)
{
	switch (Mode)
	{
	case ELCCCinematicGSWeatherMode::Clear: return &ClearWeatherPresetSlot;
	case ELCCCinematicGSWeatherMode::Dawn: return &DawnWeatherPresetSlot;
	case ELCCCinematicGSWeatherMode::Fog: return &FogWeatherPresetSlot;
	case ELCCCinematicGSWeatherMode::Rain: return &RainWeatherPresetSlot;
	case ELCCCinematicGSWeatherMode::Snow: return &SnowWeatherPresetSlot;
	default: return nullptr;
	}
}

FLCCCinematicGSSeasonPresetSlot* UGaussianSplatComponent::GetWeatherFogProfileSlot(ELCCCinematicGSWeatherMode Mode)
{
	switch (Mode)
	{
	case ELCCCinematicGSWeatherMode::Clear: return &ClearFogProfileSlot;
	case ELCCCinematicGSWeatherMode::Dawn: return &DawnFogProfileSlot;
	case ELCCCinematicGSWeatherMode::Fog: return &FogFogProfileSlot;
	case ELCCCinematicGSWeatherMode::Rain: return &RainFogProfileSlot;
	case ELCCCinematicGSWeatherMode::Snow: return &SnowFogProfileSlot;
	default: return nullptr;
	}
}

// Only fog controls belong to the dedicated per-weather fog profile. Keep this
// intentionally narrow so saving fog can never alter Rain wetness, shadows,
// seasonal colour, lighting, Niagara, snow or any other weather behaviour.
static bool IsWeatherFogProfileProperty(const FName Name)
{
	static const TSet<FName> FogProfileProperties = {
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableFogOverlayInOtherWeather),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogColor),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, DenseFogScatteringColor),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogWeatherStrength),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogVisibilityDistance),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NativeFogDensity),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NativeFogStartDistanceCm),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NativeFogMaxOpacity),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableNativeVolumetricFog),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, AtmosphereStrength),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GroundFogDensity),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, DistanceHaze),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogHeightFalloff),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogDensityVariation),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, WarmSunScatter),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogDistanceStart),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogDistanceEnd),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NearAirStart),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NearAirEnd)
	};
	return FogProfileProperties.Contains(Name);
}

void UGaussianSplatComponent::CaptureCurrentWeatherFogProfile(ELCCCinematicGSWeatherMode Mode, bool bMarkAsUserSave)
{
	FLCCCinematicGSSeasonPresetSlot* Slot = GetWeatherFogProfileSlot(Mode);
	if (!Slot)
	{
		return;
	}

	Slot->PropertyValues.Reset();
	for (TFieldIterator<FProperty> It(StaticClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (Property->GetOwnerClass() != StaticClass() || !IsWeatherFogProfileProperty(Property->GetFName()))
		{
			continue;
		}
		FString ExportedValue;
		Property->ExportText_InContainer(0, ExportedValue, this, this, this, PPF_None);
		Slot->PropertyValues.Add(Property->GetFName(), MoveTemp(ExportedValue));
	}

	Slot->bValid = Slot->PropertyValues.Num() > 0;
	if (bMarkAsUserSave)
	{
		Slot->bUserSaved = Slot->bValid;
		if (Slot->bUserSaved)
		{
			++Slot->UserSaveRevision;
		}
	}
	RefreshCurrentWeatherFogProfileStatus();
}

bool UGaussianSplatComponent::RestoreWeatherFogProfile(ELCCCinematicGSWeatherMode Mode)
{
	FLCCCinematicGSSeasonPresetSlot* Slot = GetWeatherFogProfileSlot(Mode);
	if (!Slot || !Slot->bValid || !Slot->bUserSaved)
	{
		return false;
	}

	for (const TPair<FName, FString>& Entry : Slot->PropertyValues)
	{
		if (!IsWeatherFogProfileProperty(Entry.Key))
		{
			continue;
		}
		if (FProperty* Property = FindFProperty<FProperty>(StaticClass(), Entry.Key))
		{
			Property->ImportText_InContainer(*Entry.Value, this, this, PPF_None);
		}
	}
	RefreshCurrentWeatherFogProfileStatus();
	return true;
}

void UGaussianSplatComponent::RefreshCurrentWeatherFogProfileStatus()
{
	bCurrentWeatherFogProfileSaved = false;
	CurrentWeatherFogProfileOptionCount = 0;
	CurrentWeatherFogProfileSaveRevision = 0;

	if (const FLCCCinematicGSSeasonPresetSlot* Slot = GetWeatherFogProfileSlot(WeatherMode))
	{
		bCurrentWeatherFogProfileSaved = Slot->bValid && Slot->bUserSaved;
		for (const TPair<FName, FString>& Entry : Slot->PropertyValues)
		{
			CurrentWeatherFogProfileOptionCount += IsWeatherFogProfileProperty(Entry.Key) ? 1 : 0;
		}
		CurrentWeatherFogProfileSaveRevision = Slot->UserSaveRevision;
	}
}

void UGaussianSplatComponent::SaveCurrentWeatherFogProfile()
{
	Modify();
	CaptureCurrentWeatherFogProfile(WeatherMode, true);
	bSaveCurrentWeatherFogProfileNow = false;
	MarkPackageDirty();
#if WITH_EDITOR
	if (UWorld* World = GetWorld())
	{
		if (World->PersistentLevel)
		{
			FEditorFileUtils::SaveLevel(World->PersistentLevel);
		}
	}
#endif
	UE_LOG(LogTemp, Display, TEXT("[GSFogProfile] Saved Weather=%d Options=%d Revision=%d"),
		static_cast<int32>(WeatherMode), CurrentWeatherFogProfileOptionCount, CurrentWeatherFogProfileSaveRevision);
}

static bool IsWeatherOverlayProperty(const FName Name)
{
	static const TSet<FName> WeatherProperties = {
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, AtmosphereStrength),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GroundFogDensity),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, DistanceHaze),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogHeightFalloff),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogDensityVariation),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogWeatherStrength),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogVisibilityDistance),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, DenseFogScatteringColor),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NativeFogDensity),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NativeFogStartDistanceCm),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NativeFogMaxOpacity),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableNativeVolumetricFog),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, WarmSunScatter),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, HighlightBloomBoost),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LightShaftBaseStrength),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LightShaftHighlightStrength),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetExposureEV),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableCinematicAutoExposure),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CinematicExposureMinEV100),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CinematicExposureMaxEV100),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CinematicExposureSpeedUp),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CinematicExposureSpeedDown),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CinematicExposureBias),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CinematicFilmToe),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CinematicFilmShoulder),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CinematicVignette),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CinematicBloomIntensity),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CinematicBloomThreshold),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetChromaRecovery),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetSplatSharpness),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetMidtoneContrast),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetSunTint),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetSkyTint),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedBaseShadowMinimum),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedBaseShadowSaturation),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedBaseShadowTransferStrength),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedShadowGamma),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedShadowMinimum),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedShadowStrength),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedShadowBounceStrength),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedShadowSoftness),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ShadowCrush),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, DistanceFogWeight),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NearFogWeight),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogDistanceStart),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogDistanceEnd),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NearAirStart),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NearAirEnd),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, HazeDensityMin),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, HazeDensityMax),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableProxyVolumetricShafts),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyVolumetricShaftStrength),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyVolumetricShaftDensity),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyVolumetricShaftMaxDistance),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyVolumetricShaftForwardScattering),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyVolumetricShaftTint),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FarSaturationFade),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LocalBloomStrength),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, PluginBloomScatter),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, PluginBloomSpreadPixels),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, PluginBloomSunColorInfluence),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySurfaceEffectMode),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySurfaceEffectStrength),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySurfaceHorizontalBias),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySurfacePatternScale),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySurfaceEffectColor),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySurfaceSpecularStrength),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySnowThicknessCm),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySnowSplatExpansion),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySnowGroundFillStrength),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySnowStructureRemoval),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySnowStructureSlopeThreshold),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyWetDarkening),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyPuddleAmount),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyWetGroundFullDistanceCm),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyWetGroundMaxDistanceCm),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bPreviewWetGroundMask),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyRainRippleStrength),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyRainRippleSizeCm),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyRainRippleSpacingCm),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyRainRippleSpeed),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, WeatherNiagaraOffset),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, WeatherNiagaraScale)
	};
	return WeatherProperties.Contains(Name);
}

// Rain is an overlay on the already-established lighting state.  It must never
// save, restore or author shadow/relighting controls; otherwise toggling Rain can
// make the same sun angle look much more contrasty than Clear.  Other weather
// presets keep their existing shadow behaviour.
static bool IsRainShadowIsolationProperty(const FName Name)
{
	static const TSet<FName> RainShadowProperties = {
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedBaseShadowMinimum),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedBaseShadowSaturation),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedBaseShadowTransferStrength),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedShadowGamma),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedShadowMinimum),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedShadowStrength),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedShadowBounceStrength),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedShadowSoftness),
		GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ShadowCrush)
	};
	return RainShadowProperties.Contains(Name);
}

void UGaussianSplatComponent::CaptureCurrentEffectsToWeather(ELCCCinematicGSWeatherMode Mode, bool bMarkAsUserSave)
{
	FLCCCinematicGSSeasonPresetSlot* Slot = GetWeatherPresetSlot(Mode);
	if (!Slot)
	{
		return;
	}
	Slot->PropertyValues.Reset();
	for (TFieldIterator<FProperty> It(StaticClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (Property->GetOwnerClass() != StaticClass() || !IsWeatherOverlayProperty(Property->GetFName()))
		{
			continue;
		}
		if (Mode == ELCCCinematicGSWeatherMode::Rain && IsRainShadowIsolationProperty(Property->GetFName()))
		{
			continue;
		}
		FString ExportedValue;
		Property->ExportText_InContainer(0, ExportedValue, this, this, this, PPF_None);
		Slot->PropertyValues.Add(Property->GetFName(), MoveTemp(ExportedValue));
	}
	Slot->bValid = Slot->PropertyValues.Num() > 0;
	if (bMarkAsUserSave)
	{
		Slot->bUserSaved = true;
		++Slot->UserSaveRevision;
	}
	RefreshCurrentWeatherPresetStatus();
}

bool UGaussianSplatComponent::RestoreEffectsFromWeather(ELCCCinematicGSWeatherMode Mode)
{
	FLCCCinematicGSSeasonPresetSlot* Slot = GetWeatherPresetSlot(Mode);
	// Auto-generated defaults are deliberately regenerated from code so preset
	// improvements reach existing levels. Only an explicit user save locks a
	// weather slot and takes precedence over the shipped defaults.
	if (!Slot || !Slot->bValid || !Slot->bUserSaved)
	{
		return false;
	}
	for (const TPair<FName, FString>& Entry : Slot->PropertyValues)
	{
		if (IsWeatherOverlayProperty(Entry.Key))
		{
			if (Mode == ELCCCinematicGSWeatherMode::Rain && IsRainShadowIsolationProperty(Entry.Key))
			{
				continue;
			}
			// Old Rain presets may contain global sun/sky tints. Do not let those leak
			// into the isolated base color; Clear also leaves the season tints intact.
			if (Mode == ELCCCinematicGSWeatherMode::Rain &&
				(Entry.Key == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetSunTint) ||
				 Entry.Key == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetSkyTint)))
			{
				continue;
			}
			if (FProperty* Property = FindFProperty<FProperty>(StaticClass(), Entry.Key))
			{
				Property->ImportText_InContainer(*Entry.Value, this, this, PPF_None);
			}
		}
	}
	if (Mode == ELCCCinematicGSWeatherMode::Rain)
	{
		ApplyRainColorIsolationBase(this);
	}
	RefreshCurrentWeatherPresetStatus();
	return true;
}

void UGaussianSplatComponent::RefreshCurrentWeatherPresetStatus()
{
	if (FLCCCinematicGSSeasonPresetSlot* Slot = GetWeatherPresetSlot(WeatherMode))
	{
		LastSavedWeatherOptionCount = 0;
		for (const TPair<FName, FString>& Entry : Slot->PropertyValues)
		{
			const bool bRainShadow = WeatherMode == ELCCCinematicGSWeatherMode::Rain && IsRainShadowIsolationProperty(Entry.Key);
			LastSavedWeatherOptionCount += (IsWeatherOverlayProperty(Entry.Key) && !bRainShadow) ? 1 : 0;
		}
		bCurrentWeatherUserPresetSaved = Slot->bUserSaved;
		CurrentWeatherUserSaveRevision = Slot->UserSaveRevision;
	}
}

void UGaussianSplatComponent::SaveCurrentWeatherPreset()
{
	Modify();
	CaptureCurrentEffectsToWeather(WeatherMode, true);
	PreviousWeatherMode = WeatherMode;
	bSaveCurrentWeatherPresetNow = false;
	MarkPackageDirty();
#if WITH_EDITOR
	if (UWorld* World = GetWorld())
	{
		if (World->PersistentLevel)
		{
			FEditorFileUtils::SaveLevel(World->PersistentLevel);
		}
	}
#endif
}

void UGaussianSplatComponent::RefreshNumberedWeatherPresetStatus()
{
	NumberedWeatherPresetIndex = FMath::Clamp(NumberedWeatherPresetIndex, 1, 999);
	if (const FLCCCinematicGSSeasonPresetSlot* Slot = NumberedWeatherPresetSlots.Find(NumberedWeatherPresetIndex))
	{
		bSelectedNumberedWeatherPresetSaved = Slot->bValid && Slot->bUserSaved;
		SelectedNumberedWeatherOptionCount = 0;
		for (const TPair<FName, FString>& Entry : Slot->PropertyValues)
		{
			const bool bRainShadow = WeatherMode == ELCCCinematicGSWeatherMode::Rain && IsRainShadowIsolationProperty(Entry.Key);
			SelectedNumberedWeatherOptionCount += (IsWeatherOverlayProperty(Entry.Key) && !bRainShadow) ? 1 : 0;
		}
		SelectedNumberedWeatherSaveRevision = Slot->UserSaveRevision;
	}
	else
	{
		bSelectedNumberedWeatherPresetSaved = false;
		SelectedNumberedWeatherOptionCount = 0;
		SelectedNumberedWeatherSaveRevision = 0;
	}
}

void UGaussianSplatComponent::SaveNumberedWeatherPreset()
{
	Modify();
	NumberedWeatherPresetIndex = FMath::Clamp(NumberedWeatherPresetIndex, 1, 999);
	FLCCCinematicGSSeasonPresetSlot& Slot = NumberedWeatherPresetSlots.FindOrAdd(NumberedWeatherPresetIndex);
	Slot.PropertyValues.Reset();
	for (TFieldIterator<FProperty> It(StaticClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (Property->GetOwnerClass() != StaticClass() || !IsWeatherOverlayProperty(Property->GetFName()))
		{
			continue;
		}
		if (WeatherMode == ELCCCinematicGSWeatherMode::Rain && IsRainShadowIsolationProperty(Property->GetFName()))
		{
			continue;
		}

		FString ExportedValue;
		Property->ExportText_InContainer(0, ExportedValue, this, this, this, PPF_None);
		Slot.PropertyValues.Add(Property->GetFName(), MoveTemp(ExportedValue));
	}
	Slot.bValid = Slot.PropertyValues.Num() > 0;
	Slot.bUserSaved = Slot.bValid;
	++Slot.UserSaveRevision;
	bSaveNumberedWeatherPresetNow = false;
	RefreshNumberedWeatherPresetStatus();
	MarkPackageDirty();
#if WITH_EDITOR
	if (UWorld* World = GetWorld())
	{
		if (World->PersistentLevel)
		{
			FEditorFileUtils::SaveLevel(World->PersistentLevel);
		}
	}
#endif
	UE_LOG(LogTemp, Display, TEXT("[GSWeatherPreset] Saved Slot=%d Options=%d Revision=%d"),
		NumberedWeatherPresetIndex, Slot.PropertyValues.Num(), Slot.UserSaveRevision);
}

void UGaussianSplatComponent::LoadNumberedWeatherPreset()
{
	Modify();
	NumberedWeatherPresetIndex = FMath::Clamp(NumberedWeatherPresetIndex, 1, 999);
	bLoadNumberedWeatherPresetNow = false;
	const FLCCCinematicGSSeasonPresetSlot* Slot = NumberedWeatherPresetSlots.Find(NumberedWeatherPresetIndex);
	if (!Slot || !Slot->bValid || !Slot->bUserSaved)
	{
		RefreshNumberedWeatherPresetStatus();
		UE_LOG(LogTemp, Warning, TEXT("[GSWeatherPreset] Load skipped: Slot=%d is empty"), NumberedWeatherPresetIndex);
		return;
	}

	for (const TPair<FName, FString>& Entry : Slot->PropertyValues)
	{
		if (IsWeatherOverlayProperty(Entry.Key))
		{
			if (WeatherMode == ELCCCinematicGSWeatherMode::Rain && IsRainShadowIsolationProperty(Entry.Key))
			{
				continue;
			}
			if (WeatherMode == ELCCCinematicGSWeatherMode::Rain &&
				(Entry.Key == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetSunTint) ||
				 Entry.Key == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetSkyTint)))
			{
				continue;
			}
			if (FProperty* Property = FindFProperty<FProperty>(StaticClass(), Entry.Key))
			{
				Property->ImportText_InContainer(*Entry.Value, this, this, PPF_None);
			}
		}
	}
	if (WeatherMode == ELCCCinematicGSWeatherMode::Rain)
	{
		ApplyRainColorIsolationBase(this);
	}
	RestoreWeatherFogProfile(WeatherMode);
	RefreshCurrentWeatherFogProfileStatus();
	RefreshNumberedWeatherPresetStatus();
	MarkPackageDirty();
	UpdateCachedSceneLighting();
	MarkRenderDynamicDataDirty();
	MarkRenderStateDirty();
	UE_LOG(LogTemp, Display, TEXT("[GSWeatherPreset] Loaded Slot=%d Options=%d Revision=%d"),
		NumberedWeatherPresetIndex, Slot->PropertyValues.Num(), Slot->UserSaveRevision);
}

void UGaussianSplatComponent::ApplySpringSeasonPreset()
{
	Modify();
	SeasonMode = ELCCCinematicGSSeasonMode::Spring;
	bEnableSeasonalColor = true;
	bPreviewSeasonColorMask = false;
	SeasonSourceColorFrom = FLinearColor(0.008f, 0.022f, 0.004f, 1.0f);
	SeasonSourceColorTo = FLinearColor(0.66f, 0.82f, 0.24f, 1.0f);
	SeasonTargetColorFrom = FLinearColor(0.12f, 0.30f, 0.045f, 1.0f);
	SeasonTargetColorTo = FLinearColor(0.76f, 0.94f, 0.38f, 1.0f);
	SeasonColorTolerance = 0.21f;
	SeasonColorFeather = 0.08f;
	SeasonMinSaturation = 0.17f;
	SeasonMinGreenDominance = 0.025f;
	SeasonSaturationScale = 1.02f;
	SeasonBrightnessScale = 1.02f;
	SeasonReplacementStrength = 0.34f;
	SeasonOriginalDetailPreservation = 0.62f;
	SeasonTargetStartColorBias = 0.18f;
	MarkPackageDirty();
	UpdateCachedSceneLighting();
	MarkRenderDynamicDataDirty();
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::ApplySummerSeasonPreset()
{
	Modify();
	SeasonMode = ELCCCinematicGSSeasonMode::Summer;
	bEnableSeasonalColor = true;
	bPreviewSeasonColorMask = false;
	SeasonSourceColorFrom = FLinearColor(0.008f, 0.022f, 0.004f, 1.0f);
	SeasonSourceColorTo = FLinearColor(0.66f, 0.82f, 0.24f, 1.0f);
	SeasonTargetColorFrom = FLinearColor(0.025f, 0.14f, 0.010f, 1.0f);
	SeasonTargetColorTo = FLinearColor(0.38f, 0.82f, 0.16f, 1.0f);
	SeasonColorTolerance = 0.22f;
	SeasonColorFeather = 0.09f;
	SeasonMinSaturation = 0.16f;
	SeasonMinGreenDominance = 0.025f;
	SeasonSaturationScale = 1.08f;
	SeasonBrightnessScale = 1.00f;
	SeasonReplacementStrength = 0.28f;
	SeasonOriginalDetailPreservation = 0.66f;
	SeasonTargetStartColorBias = 0.0f;
	MarkPackageDirty();
	UpdateCachedSceneLighting();
	MarkRenderDynamicDataDirty();
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::ApplyAutumnSeasonPreset()
{
	Modify();
	SeasonMode = ELCCCinematicGSSeasonMode::Autumn;
	bEnableSeasonalColor = true;
	bPreviewSeasonColorMask = false;
	SeasonSourceColorFrom = FLinearColor(0.008f, 0.022f, 0.004f, 1.0f);
	SeasonSourceColorTo = FLinearColor(0.66f, 0.82f, 0.24f, 1.0f);
	SeasonTargetColorFrom = FLinearColor(0.34f, 0.035f, 0.008f, 1.0f);
	SeasonTargetColorTo = FLinearColor(1.00f, 0.70f, 0.035f, 1.0f);
	SeasonColorTolerance = 0.30f;
	SeasonColorFeather = 0.09f;
	SeasonMinSaturation = 0.16f;
	SeasonMinGreenDominance = 0.025f;
	SeasonSaturationScale = 0.96f;
	SeasonBrightnessScale = 0.96f;
	SeasonReplacementStrength = 0.46f;
	SeasonOriginalDetailPreservation = 0.58f;
	SeasonTargetStartColorBias = 0.62f;
	MarkPackageDirty();
	UpdateCachedSceneLighting();
	MarkRenderDynamicDataDirty();
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::ApplyWinterSeasonPreset()
{
	Modify();
	SeasonMode = ELCCCinematicGSSeasonMode::Winter;
	bEnableSeasonalColor = true;
	bPreviewSeasonColorMask = false;
	SeasonSourceColorFrom = FLinearColor(0.008f, 0.022f, 0.004f, 1.0f);
	SeasonSourceColorTo = FLinearColor(0.66f, 0.82f, 0.24f, 1.0f);
	SeasonTargetColorFrom = FLinearColor(0.16f, 0.13f, 0.10f, 1.0f);
	SeasonTargetColorTo = FLinearColor(0.72f, 0.78f, 0.82f, 1.0f);
	SeasonColorTolerance = 0.23f;
	SeasonColorFeather = 0.10f;
	SeasonMinSaturation = 0.14f;
	SeasonMinGreenDominance = 0.02f;
	SeasonSaturationScale = 0.16f;
	SeasonBrightnessScale = 0.82f;
	SeasonReplacementStrength = 0.62f;
	SeasonOriginalDetailPreservation = 0.48f;
	SeasonTargetStartColorBias = 0.20f;
	MarkPackageDirty();
	UpdateCachedSceneLighting();
	MarkRenderDynamicDataDirty();
	MarkRenderStateDirty();
}

static bool HasLegacyFogStateLeakedIntoRain(const UGaussianSplatComponent* Component)
{
	if (!Component || Component->WeatherMode != ELCCCinematicGSWeatherMode::Rain ||
		Component->bEnableFogOverlayInOtherWeather)
	{
		return false;
	}

	// These values are the distinctive dense-Fog preset signature visible in the
	// user's Rain screenshots (1.65 / 0.38 plus Fog haze/shadow values). Requiring
	// several matches avoids rewriting intentional Rain tuning.
	const bool bFogShapeSignature =
		FMath::IsNearlyEqual(Component->FogHeightFalloff, 1.65f, 0.02f) &&
		FMath::IsNearlyEqual(Component->FogDensityVariation, 0.38f, 0.02f) &&
		Component->HazeDensityMax > 1.20f;
	return bFogShapeSignature;
}

static void ResetRainFogLeakStateToOriginalBaseline(UGaussianSplatComponent* Component, bool bForce)
{
	if (!Component || Component->bEnableFogOverlayInOtherWeather)
	{
		return;
	}
	if (!bForce && !HasLegacyFogStateLeakedIntoRain(Component))
	{
		return;
	}

	// Exact v2819 non-shadow Fog baseline. Rain never rewrites shadow/relighting
	// controls; this cleanup is limited to stale atmospheric Fog values only.
	Component->FogHeightFalloff = 1.35f;
	Component->FogDensityVariation = 0.28f;
	Component->FogDistanceStart = 0.018f;
	Component->FogDistanceEnd = 0.48f;
	Component->NearAirStart = 0.010f;
	Component->NearAirEnd = 0.24f;
	Component->HazeDensityMin = 0.16f;
	Component->HazeDensityMax = 0.46f;
	Component->bEnableProxyVolumetricShafts = true;
	Component->ProxyVolumetricShaftStrength = 0.18f;
	Component->ProxyVolumetricShaftDensity = 0.65f;
	Component->ProxyVolumetricShaftForwardScattering = 0.65f;
	Component->ProxyVolumetricShaftTint = FLinearColor(1.0f, 0.86f, 0.62f, 1.0f);
}

static void ApplyRainColorIsolationBase(UGaussianSplatComponent* Component)
{
	if (!Component) return;
	// Rain-only color isolation. These are the same common color/atmosphere values
	// used by the shipped Clear preset. Shadow fields are deliberately absent here.
	Component->AtmosphereStrength = 0.16f;
	Component->GroundFogDensity = 0.01f;
	Component->DistanceHaze = 0.08f;
	Component->WarmSunScatter = 0.22f;
	Component->HighlightBloomBoost = 0.24f;
	Component->TargetExposureEV = -0.08f;
	Component->TargetChromaRecovery = 1.05f;
	Component->TargetSplatSharpness = 0.42f;
	Component->TargetMidtoneContrast = 1.36f;
	Component->DistanceFogWeight = 0.08f;
	Component->NearFogWeight = 0.0f;
	Component->FarSaturationFade = 0.02f;
	Component->LocalBloomStrength = 0.22f;
}

void UGaussianSplatComponent::ApplyWeatherPreset()
{
	Modify();
	switch (WeatherMode)
	{
	case ELCCCinematicGSWeatherMode::Clear:
		AtmosphereStrength = 0.16f; GroundFogDensity = 0.01f; DistanceHaze = 0.08f;
		WarmSunScatter = 0.22f; HighlightBloomBoost = 0.24f; TargetExposureEV = -0.08f;
		TargetChromaRecovery = 1.05f; TargetSplatSharpness = 0.42f; TargetMidtoneContrast = 1.36f;
		CapturedBaseShadowMinimum = 0.66f; CapturedShadowSoftness = 0.72f;
		DistanceFogWeight = 0.08f; NearFogWeight = 0.0f; FarSaturationFade = 0.02f;
		LocalBloomStrength = 0.22f; ProxySurfaceEffectMode = ELCCCinematicGSProxySurfaceEffectMode::Disabled;
		break;
	case ELCCCinematicGSWeatherMode::Dawn:
		AtmosphereStrength = 0.46f; GroundFogDensity = 0.24f; DistanceHaze = 0.38f;
		WarmSunScatter = 0.82f; HighlightBloomBoost = 2.20f; TargetExposureEV = -0.72f;
		TargetChromaRecovery = 0.96f; TargetSplatSharpness = 0.32f; TargetMidtoneContrast = 1.48f;
		TargetSunTint = FLinearColor(1.15f, 0.72f, 0.42f, 1.0f);
		TargetSkyTint = FLinearColor(0.46f, 0.60f, 0.92f, 1.0f);
		CapturedShadowGamma = 1.65f; // compatibility-only
		CapturedShadowMinimum = 0.04f;
		CapturedShadowStrength = 1.65f; // v2748 unified cast density
		CapturedBaseShadowTransferStrength = 1.0f;
		CapturedBaseShadowMinimum = 0.20f;
		CapturedBaseShadowSaturation = 0.92f;
		CapturedShadowBounceStrength = 0.08f;
		CapturedShadowSoftness = 1.05f;
		DistanceFogWeight = 0.42f; NearFogWeight = 0.22f; FarSaturationFade = 0.16f;
		PluginBloomSpreadPixels = 34.0f;
		PluginBloomScatter = 2.40f;
		CinematicBloomThreshold = 0.72f;
		PluginBloomSunColorInfluence = 0.92f;
		LocalBloomStrength = 0.34f; ProxySurfaceEffectMode = ELCCCinematicGSProxySurfaceEffectMode::Disabled;
		break;
	case ELCCCinematicGSWeatherMode::Fog:
		AtmosphereStrength = 1.38f; GroundFogDensity = 1.12f; DistanceHaze = 1.55f;
		FogHeightFalloff = 1.65f; FogDensityVariation = 0.38f;
		// Fog vQualityPreserve: density belongs to the fog system; do not use the
		// weather preset as a second colour/quality grade on the captured 3DGS.
		// Keep the same neutral grade/detail baseline as Clear so enabling Fog does
		// not soften splats, suppress chroma or shift exposure before fog is applied.
		WarmSunScatter = 0.04f; HighlightBloomBoost = 0.08f; TargetExposureEV = -0.08f;
		TargetChromaRecovery = 1.05f; TargetSplatSharpness = 0.42f; TargetMidtoneContrast = 1.36f;
		ShadowCrush = 0.34f;
		CapturedBaseShadowMinimum = 0.42f;
		CapturedBaseShadowSaturation = 0.72f;
		CapturedShadowBounceStrength = 0.12f;
		CapturedShadowSoftness = 1.55f;
		DistanceFogWeight = 1.72f; NearFogWeight = 0.54f; FarSaturationFade = 0.86f;
		FogDistanceStart = 0.055f; FogDistanceEnd = 0.30f;
		NearAirStart = 0.025f; NearAirEnd = 0.19f;
		HazeDensityMin = 0.82f; HazeDensityMax = 1.92f;
		bEnableProxyVolumetricShafts = true;
		ProxyVolumetricShaftStrength = 0.32f;
		ProxyVolumetricShaftDensity = 0.88f;
		ProxyVolumetricShaftForwardScattering = 0.78f;
		ProxyVolumetricShaftTint = FLinearColor(0.68f, 0.74f, 0.88f, 1.0f);
		LocalBloomStrength = 0.12f; ProxySurfaceEffectMode = ELCCCinematicGSProxySurfaceEffectMode::Disabled;
		break;
	case ELCCCinematicGSWeatherMode::Rain:
		// Rain is a visual/wet overlay only. Never author shadow/relighting values here:
		// the exact shadow state that was active before Rain remains active in Rain.
		// Stale Fog atmosphere values are cleaned without touching any shadow field.
		ResetRainFogLeakStateToOriginalBaseline(this, false);
		ApplyRainColorIsolationBase(this);
		bRainPrecipitationEnabled = true;
		ProxySurfaceEffectMode = ELCCCinematicGSProxySurfaceEffectMode::WetPuddles;
		ProxySurfaceEffectStrength = 0.48f;
		ProxySurfaceEffectColor = FLinearColor(0.58f, 0.68f, 0.82f, 1.0f);
		ProxySurfaceSpecularStrength = 0.82f;
		ProxyWetDarkening = 0.28f;
		ProxyPuddleAmount = 0.55f;
		ProxyWetGroundFullDistanceCm = 80.0f;
		ProxyWetGroundMaxDistanceCm = 140.0f;
		bPreviewWetGroundMask = false;
		break;
	case ELCCCinematicGSWeatherMode::Snow:
		AtmosphereStrength = 0.48f; GroundFogDensity = 0.20f; DistanceHaze = 0.48f;
		WarmSunScatter = 0.0f; HighlightBloomBoost = 0.30f; TargetExposureEV = -0.22f;
		TargetChromaRecovery = 0.68f; TargetSplatSharpness = 0.48f; TargetMidtoneContrast = 1.42f;
		TargetSunTint = FLinearColor(0.90f, 0.98f, 1.10f, 1.0f);
		TargetSkyTint = FLinearColor(0.64f, 0.78f, 1.0f, 1.0f);
		CapturedBaseShadowMinimum = 0.68f; CapturedShadowSoftness = 1.10f;
		DistanceFogWeight = 0.48f; NearFogWeight = 0.18f; FarSaturationFade = 0.38f;
		LocalBloomStrength = 0.24f; ProxySurfaceEffectMode = ELCCCinematicGSProxySurfaceEffectMode::Snow;
		ProxySurfaceEffectStrength = 0.78f;
		ProxySurfaceEffectColor = FLinearColor(0.92f, 0.96f, 1.0f, 1.0f);
		ProxySurfaceSpecularStrength = 0.68f;
		ProxySnowThicknessCm = 4.0f;
		ProxySnowSplatExpansion = 1.06f;
		ProxySnowGroundFillStrength = 0.72f;
		ProxySnowStructureRemoval = 0.68f;
		ProxySnowStructureSlopeThreshold = 0.70f;
		break;
	}
	MarkPackageDirty();
	UpdateCachedSceneLighting();
	MarkRenderDynamicDataDirty();
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::PreviewSeasonColorMask()
{
	Modify();
	bEnableSeasonalColor = true;
	bPreviewSeasonColorMask = true;
	UpdateCachedSceneLighting();
	MarkRenderDynamicDataDirty();
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::PreviewSeasonFinalColor()
{
	Modify();
	bPreviewSeasonColorMask = false;
	UpdateCachedSceneLighting();
	MarkRenderDynamicDataDirty();
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::EnableCinematicEffects()
{
	Modify();
	bEnableCinematicEffects = true;
	MarkPackageDirty();
	UpdateCachedSceneLighting();
	MarkRenderDynamicDataDirty();
	MarkRenderStateDirty();
	SyncOwnerProxyShadowReceiver(this);
}

void UGaussianSplatComponent::DisableCinematicEffects()
{
	Modify();
	bEnableCinematicEffects = false;
	MarkPackageDirty();
	UpdateCachedSceneLighting();
	MarkRenderDynamicDataDirty();
	MarkRenderStateDirty();
	SyncOwnerProxyShadowReceiver(this);
}

FTransform UGaussianSplatComponent::GetProxyShadowMeshLocalTransform() const
{
	return FTransform(
		ProxyShadowMeshRotationOffset,
		ProxyShadowMeshLocationOffset,
		ProxyShadowMeshScaleOffset);
}

FTransform UGaussianSplatComponent::GetProxyShadowMeshWorldTransform() const
{
	return GetProxyShadowMeshLocalTransform() * GetComponentTransform();
}

void UGaussianSplatComponent::InvalidateStableProxyBindings(const TCHAR* Reason)
{
	const bool bHadBindingState = (StableSplatBindings.IsValid() && StableSplatBindings->Num() > 0) || StableBindingBuildContext.IsValid() ||
		StableBindingState != EStableProxyBindingState::Uninitialized;
	StableBindingBuildContext.Reset();
	StableSplatBindings.Reset();
	StableProxyVertices.Reset();
	StableProxyTriangles.Reset();
	StableProxyTriangleVertexNormals.Reset();
	StableProxyRayProofs.Reset();
	StableProxyRainExposure.Reset();
	StableProxyTemporalProofs.Reset();
	LastStableRayLightDirection = FVector3f::ZeroVector;
	LastStableRayBuildFrame = MAX_uint64;
	StableFieldVertexCount = 0;
	StableFieldTriangleCount = 0;
	StableBindingState = EStableProxyBindingState::Uninitialized;
	StableBindingHash = 0;
	StableBindingInputCount = 0;
	StableBindingProcessedCount = 0;
	StableBindingUnprocessedCount = 0;
	StableBindingValidCount = 0;
	StableBindingInvalidCount = 0;
	StableBindingUniqueTriangleCount = 0;
	StableBindingProgressPercent = 0.0f;
	if (bHadBindingState)
	{
		++StableBindingRevision;
		MarkRenderDynamicDataDirty();
		UE_LOG(LogTemp, Log, TEXT("[GSStableBinding] State=Uninitialized Rev=%u Reason=%s"),
			StableBindingRevision, Reason ? Reason : TEXT("Unknown"));
	}
}

FString UGaussianSplatComponent::BuildStableBindingCacheKey() const
{
	if (!SplatAsset || !ProxyShadowMesh)
	{
		return FString();
	}

	uint32 ProxyVertices = 0;
	uint32 ProxyIndices = 0;
	if (const FStaticMeshRenderData* RenderData = ProxyShadowMesh->GetRenderData();
		RenderData && RenderData->LODResources.Num() > 0)
	{
		const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
		ProxyVertices = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
		ProxyIndices = LOD.IndexBuffer.GetNumIndices();
	}

	const FVector SourceBoundsSize = SplatAsset->GetBounds().GetSize();
	const FVector ProxyBoundsSize = ProxyShadowMesh->GetBoundingBox().GetSize();
	const FString SourceDescription = FString::Printf(
		TEXT("StableBindingCacheV7|%s|%d|%lld|%.6g,%.6g,%.6g|%s|%u|%u|%.6g,%.6g,%.6g|%s|%.9g|%.9g"),
		*SplatAsset->GetPathName(),
		SplatAsset->GetSplatCount(), SplatAsset->GetMemoryUsage(),
        SourceBoundsSize.X, SourceBoundsSize.Y, SourceBoundsSize.Z,
		*ProxyShadowMesh->GetPathName(),
		ProxyVertices, ProxyIndices,
        ProxyBoundsSize.X, ProxyBoundsSize.Y, ProxyBoundsSize.Z,
		*GetProxyShadowMeshLocalTransform().ToString(),
		StableBindingMaxDistanceCm, StableClosureRadiusCm);

	FTCHARToUTF8 Utf8(*SourceDescription);
	return FMD5::HashBytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()).ToUpper();
}

bool UGaussianSplatComponent::SaveStableProxyBindingCache()
{
	if (StableBindingState != EStableProxyBindingState::Ready || !StableSplatBindings.IsValid() || StableSplatBindings->IsEmpty() ||
		StableProxyVertices.IsEmpty() || StableProxyTriangles.IsEmpty() ||
		StableProxyTriangleVertexNormals.Num() != StableProxyTriangles.Num() * 3)
	{
		return false;
	}

	const FString CacheKey = BuildStableBindingCacheKey();
	if (CacheKey.IsEmpty())
	{
		return false;
	}

	const FString CacheDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("LCCCinematicGS"), TEXT("StableBindingCache"));
	IFileManager::Get().MakeDirectory(*CacheDir, true);
	const FString CachePath = FPaths::Combine(CacheDir, CacheKey + TEXT(".bin"));
	TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*CachePath));
	if (!Writer)
	{
		StableBindingCacheStatus = TEXT("Save Failed");
		return false;
	}

	uint32 Magic = 0x53424331u; // SBC1
	uint32 Version = 6u; // v2811: 16-byte uint4 binding payload + strict cache-shape validation.
	uint32 BindingStride = sizeof(FStableProxySplatBindingGPU);
	int32 BindingCount = StableSplatBindings->Num();
	int32 VertexCount = StableProxyVertices.Num();
	int32 TriangleCount = StableProxyTriangles.Num();
	int32 TriangleNormalCount = StableProxyTriangleVertexNormals.Num();
	*Writer << Magic << Version << BindingStride;
	*Writer << BindingCount << VertexCount << TriangleCount << TriangleNormalCount;
	*Writer << StableBindingHash << StableBindingValidCount << StableBindingInvalidCount << StableBindingUniqueTriangleCount;
	Writer->Serialize(const_cast<FStableProxySplatBindingGPU*>(StableSplatBindings->GetData()), int64(BindingCount) * sizeof(FStableProxySplatBindingGPU));
	Writer->Serialize(StableProxyVertices.GetData(), int64(VertexCount) * sizeof(FStableProxyVertexGPU));
	Writer->Serialize(StableProxyTriangles.GetData(), int64(TriangleCount) * sizeof(FStableProxyTriangleGPU));
	Writer->Serialize(StableProxyTriangleVertexNormals.GetData(), int64(TriangleNormalCount) * sizeof(FVector4f));
	Writer->Close();

	if (Writer->IsError())
	{
		IFileManager::Get().Delete(*CachePath);
		StableBindingCacheStatus = TEXT("Save Failed");
		return false;
	}
	StableBindingCacheStatus = FString::Printf(TEXT("Saved: %s"), *CacheKey.Left(12));
	UE_LOG(LogTemp, Log, TEXT("[GSStableCache] Saved Key=%s Bindings=%d Vertices=%d Triangles=%d Path=%s"),
		*CacheKey, BindingCount, VertexCount, TriangleCount, *CachePath);
	return true;
}

bool UGaussianSplatComponent::TryLoadStableProxyBindingCache()
{
	if (!SplatAsset || !SplatAsset->IsValid() || !ProxyShadowMesh)
	{
		StableBindingCacheStatus = TEXT("Rebuild Required: Missing Source");
		return false;
	}
	const FString CacheKey = BuildStableBindingCacheKey();
	if (CacheKey == LastStableBindingCacheLoadKey)
	{
		return false;
	}
	LastStableBindingCacheLoadKey = CacheKey;
	FString CachePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("LCCCinematicGS"),
		TEXT("StableBindingCache"), CacheKey + TEXT(".bin"));
	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*CachePath));
	if (!Reader)
	{
		// v2811 intentionally does NOT migrate or probe older cache files. v2810
		// expanded FStableProxySplatBindingGPU from uint2 (8 bytes) to uint4 (16 bytes),
		// so accepting a legacy file by directory fallback can silently reinterpret the
		// old byte stream with the new stride and invalidate every following binding.
		StableBindingCacheStatus = TEXT("Rebuild Required");
		UE_LOG(LogTemp, Warning, TEXT("[GSStableCache] Strict V6 miss Key=%s Path=%s"), *CacheKey, *CachePath);
		return false;
	}

	uint32 Magic = 0, Version = 0, BindingStride = 0;
	int32 BindingCount = 0, VertexCount = 0, TriangleCount = 0, TriangleNormalCount = 0;
	uint64 CachedHash = 0;
	int32 ValidCount = 0, InvalidCount = 0, UniqueCount = 0;
	*Reader << Magic << Version << BindingStride;
	*Reader << BindingCount << VertexCount << TriangleCount << TriangleNormalCount;
	*Reader << CachedHash << ValidCount << InvalidCount << UniqueCount;
	uint32 ExpectedTriangles = 0;
	if (const FStaticMeshRenderData* RenderData = ProxyShadowMesh->GetRenderData();
		RenderData && RenderData->LODResources.Num() > 0)
	{
		const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
		ExpectedTriangles = LOD.IndexBuffer.GetNumIndices() / 3u;
	}
	const bool bHeaderValid = Magic == 0x53424331u && Version == 6u &&
		BindingStride == sizeof(FStableProxySplatBindingGPU) &&
		BindingCount == SplatAsset->GetSplatCount() && BindingCount > 0 &&
		TriangleCount == int32(ExpectedTriangles) &&
		TriangleNormalCount == TriangleCount * 3 &&
		VertexCount > 0 && VertexCount < 100000000 && TriangleCount > 0;
	const int64 ExpectedPayloadBytes = bHeaderValid
		? int64(BindingCount) * int64(sizeof(FStableProxySplatBindingGPU))
			+ int64(VertexCount) * int64(sizeof(FStableProxyVertexGPU))
			+ int64(TriangleCount) * int64(sizeof(FStableProxyTriangleGPU))
			+ int64(TriangleNormalCount) * int64(sizeof(FVector4f))
		: -1;
	const int64 RemainingBytes = Reader->TotalSize() - Reader->Tell();
	const bool bPayloadShapeValid = bHeaderValid && RemainingBytes == ExpectedPayloadBytes;
	if (!bPayloadShapeValid)
	{
		StableBindingCacheStatus = TEXT("Rebuild Required: Invalid Cache");
		UE_LOG(LogTemp, Warning,
			TEXT("[GSStableCache] Reject Cache Version=%u Stride=%u ExpectedStride=%u Bindings=%d Expected=%d Vertices=%d Triangles=%d ExpectedTriangles=%u RemainingBytes=%lld ExpectedPayloadBytes=%lld"),
			Version, BindingStride, uint32(sizeof(FStableProxySplatBindingGPU)), BindingCount,
			SplatAsset->GetSplatCount(), VertexCount, TriangleCount, ExpectedTriangles,
			RemainingBytes, ExpectedPayloadBytes);
		return false;
	}

	TArray<FStableProxySplatBindingGPU> LoadedBindings;
	LoadedBindings.SetNumZeroed(BindingCount);
	StableProxyVertices.SetNumUninitialized(VertexCount);
	StableProxyTriangles.SetNumUninitialized(TriangleCount);
	StableProxyTriangleVertexNormals.SetNumUninitialized(TriangleNormalCount);
	Reader->Serialize(LoadedBindings.GetData(), int64(BindingCount) * sizeof(FStableProxySplatBindingGPU));
	Reader->Serialize(StableProxyVertices.GetData(), int64(VertexCount) * sizeof(FStableProxyVertexGPU));
	Reader->Serialize(StableProxyTriangles.GetData(), int64(TriangleCount) * sizeof(FStableProxyTriangleGPU));
	Reader->Serialize(StableProxyTriangleVertexNormals.GetData(), int64(TriangleNormalCount) * sizeof(FVector4f));
	if (Reader->IsError())
	{
		StableSplatBindings.Reset();
		StableProxyVertices.Reset();
		StableProxyTriangles.Reset();
		StableProxyTriangleVertexNormals.Reset();
		StableBindingCacheStatus = TEXT("Rebuild Required: Read Failed");
		return false;
	}
	StableSplatBindings = MakeShared<const TArray<FStableProxySplatBindingGPU>, ESPMode::ThreadSafe>(MoveTemp(LoadedBindings));
	StableProxyRainExposure.Reset();

	StableBindingBuildContext.Reset();
	StableBindingHash = CachedHash;
	StableBindingInputCount = BindingCount;
	StableBindingProcessedCount = BindingCount;
	StableBindingUnprocessedCount = 0;
	StableBindingValidCount = ValidCount;
	StableBindingInvalidCount = InvalidCount;
	StableBindingUniqueTriangleCount = UniqueCount;
	StableBindingProgressPercent = 100.0f;
	StableFieldVertexCount = VertexCount;
	StableFieldTriangleCount = TriangleCount;
	StableBindingState = EStableProxyBindingState::Ready;
	++StableBindingRevision;
	StableBindingCacheStatus = FString::Printf(TEXT("Loaded: %s"), *CacheKey.Left(12));
	MarkRenderDynamicDataDirty();
	UE_LOG(LogTemp, Log, TEXT("[GSStableCache] Loaded Key=%s Bindings=%d Vertices=%d Triangles=%d"),
		*CacheKey, BindingCount, VertexCount, TriangleCount);
	if (!CachePath.EndsWith(CacheKey + TEXT(".bin")))
	{
		SaveStableProxyBindingCache();
		UE_LOG(LogTemp, Log, TEXT("[GSStableCache] Migrated legacy cache to stable V2 key=%s"), *CacheKey);
	}
	return true;
}

void UGaussianSplatComponent::RebuildStableProxyBindings()
{
	++StableBindingRebuildCount;
	StableBindingBuildContext.Reset();
	StableProxyRainExposure.Reset();
	StableBindingState = EStableProxyBindingState::Building;
	// Keep the last completed front hash untouched while the back binding is rebuilt.
	StableBindingInputCount = 0;
	StableBindingProcessedCount = 0;
	StableBindingUnprocessedCount = 0;
	StableBindingValidCount = 0;
	StableBindingInvalidCount = 0;
	StableBindingUniqueTriangleCount = 0;
	StableBindingProgressPercent = 0.0f;

	if (!SplatAsset || !SplatAsset->IsValid() || !ProxyShadowMesh)
	{
		StableBindingState = EStableProxyBindingState::Failed;
		UE_LOG(LogTemp, Warning, TEXT("[GSStableBinding] State=Failed Reason=MissingAssetOrProxy FrontRev=%u"), StableBindingRevision);
		return;
	}

	const FStaticMeshRenderData* RenderData = ProxyShadowMesh->GetRenderData();
	if (!RenderData || RenderData->LODResources.Num() == 0)
	{
		StableBindingState = EStableProxyBindingState::Failed;
		UE_LOG(LogTemp, Warning, TEXT("[GSStableBinding] State=Failed Reason=ProxyCPUDataUnavailable FrontRev=%u"), StableBindingRevision);
		return;
	}

	const int32 TotalSplatCount = SplatAsset->GetSplatCount();
	if (TotalSplatCount <= 0)
	{
		StableBindingState = EStableProxyBindingState::Failed;
		UE_LOG(LogTemp, Warning, TEXT("[GSStableBinding] State=Failed Reason=NoSplats FrontRev=%u"), StableBindingRevision);
		return;
	}

	TSharedPtr<FStableProxyBindingBuildContext> Build = MakeShared<FStableProxyBindingBuildContext>();
	Build->TotalSplatCount = TotalSplatCount;
	Build->NextSplatIndex = 0;
	Build->StartSeconds = FPlatformTime::Seconds();
	Build->PendingBindings.Init(FStableProxySplatBindingGPU(), TotalSplatCount);

	const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	const FPositionVertexBuffer& PositionBuffer = LOD.VertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& StaticMeshVertexBuffer = LOD.VertexBuffers.StaticMeshVertexBuffer;
	const uint32 VertexCount = PositionBuffer.GetNumVertices();
	const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
	const int32 SourceTriangleCount = Indices.Num() / 3;
	const FTransform ProxyToSplat = GetProxyShadowMeshLocalTransform();

    // v27 universal-scene calibration.  The plugin must not assume that every
    // imported 3DGS/collider scene has the same physical extent.  Keep an
    // explicitly edited Max Proxy Distance as artist authority, but when the
    // field is still at its shipped 250cm default derive an equivalent distance
    // from the actual source/proxy bounds.  The reference is intentionally
    // conservative and clamped, so normal UE-centimeter scenes keep the old
    // behavior while very small/large scenes do not inherit a bad fixed radius.
    FBox ProxyBoundsInSplat(ForceInit);
    for (uint32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
    {
        ProxyBoundsInSplat += ProxyToSplat.TransformPosition(
            FVector(PositionBuffer.VertexPosition(VertexIndex)));
    }
    const float SourceDiagonalCm = SplatAsset->GetBounds().IsValid
        ? static_cast<float>(SplatAsset->GetBounds().GetSize().Length()) : 0.0f;
    const float ProxyDiagonalCm = ProxyBoundsInSplat.IsValid
        ? static_cast<float>(ProxyBoundsInSplat.GetSize().Length()) : 0.0f;
    float ReferenceDiagonalCm = 0.0f;
    if (SourceDiagonalCm > 1.0f && ProxyDiagonalCm > 1.0f)
    {
        ReferenceDiagonalCm = FMath::Min(SourceDiagonalCm, ProxyDiagonalCm);
    }
    else
    {
        ReferenceDiagonalCm = FMath::Max(SourceDiagonalCm, ProxyDiagonalCm);
    }
    float EffectiveStableBindingMaxDistanceCm = FMath::Max(StableBindingMaxDistanceCm, 1.0f);
    const bool bUseAutomaticDefaultDistance =
        FMath::IsNearlyEqual(StableBindingMaxDistanceCm, 250.0f, 0.01f) &&
        ReferenceDiagonalCm > 1.0f;
    if (bUseAutomaticDefaultDistance)
    {
        EffectiveStableBindingMaxDistanceCm = FMath::Clamp(
            ReferenceDiagonalCm * 0.0125f, 50.0f, 500.0f);
    }
    Build->MaxDistanceSq = FMath::Square(EffectiveStableBindingMaxDistanceCm);
    UE_LOG(LogTemp, Display,
        TEXT("[GSSceneCalibration] Source=%s Proxy=%s SourceDiag=%.1fcm ProxyDiag=%.1fcm StableMax=%.1fcm Auto=%d"),
        *GetNameSafe(SplatAsset.Get()), *GetNameSafe(ProxyShadowMesh.Get()),
        SourceDiagonalCm, ProxyDiagonalCm, EffectiveStableBindingMaxDistanceCm,
        bUseAutomaticDefaultDistance ? 1 : 0);

	// Camera-independent proxy-surface topology. Position-quantized welding makes
	// seam duplicates share one stable LightDepth sample.
	Build->PendingTriangleVertices.SetNum(SourceTriangleCount);
	Build->PendingTriangleVertexNormals.Init(FVector4f(0, 0, 1, 0), SourceTriangleCount * 3);

	// v2809 keeps the cast/light-depth weld topology independent from the lighting-normal
	// topology. Stable cast vertices remain very tightly welded, while the normal field
	// gets a slightly wider positional weld so UV/material seams and tiny export splits
	// can still share one geometric one-ring without altering cast geometry.
	TArray<FVector3f> GeometricFaceNormals;
	GeometricFaceNormals.Init(FVector3f::ZeroVector, SourceTriangleCount);
	TArray<float> GeometricFaceAreas;
	GeometricFaceAreas.Init(0.0f, SourceTriangleCount);
	TArray<uint32> NormalCornerVertexIDs;
	NormalCornerVertexIDs.Init(MAX_uint32, SourceTriangleCount * 3);
	TMap<FIntVector, uint32> NormalWeldVertexMap;
	NormalWeldVertexMap.Reserve(VertexCount);
	uint32 NormalWeldVertexCount = 0u;
	constexpr float NormalWeldInvTolerance = 4.0f; // 0.25 cm positional tolerance
	auto FindOrAddNormalWeldVertex = [&](const FVector3f& P) -> uint32
	{
		const FIntVector Key(
			FMath::RoundToInt(P.X * NormalWeldInvTolerance),
			FMath::RoundToInt(P.Y * NormalWeldInvTolerance),
			FMath::RoundToInt(P.Z * NormalWeldInvTolerance));
		if (const uint32* Existing = NormalWeldVertexMap.Find(Key))
		{
			return *Existing;
		}
		const uint32 NewIndex = NormalWeldVertexCount++;
		NormalWeldVertexMap.Add(Key, NewIndex);
		return NewIndex;
	};

	TMap<FIntVector, uint32> WeldedVertexMap;
	WeldedVertexMap.Reserve(VertexCount);
	constexpr float WeldInvTolerance = 100.0f; // 0.01 cm
	auto FindOrAddWeldedVertex = [&Build, &WeldedVertexMap](const FVector3f& P) -> uint32
	{
		const FIntVector Key(
			FMath::RoundToInt(P.X * WeldInvTolerance),
			FMath::RoundToInt(P.Y * WeldInvTolerance),
			FMath::RoundToInt(P.Z * WeldInvTolerance));
		if (const uint32* Existing = WeldedVertexMap.Find(Key))
		{
			return *Existing;
		}
		const uint32 NewIndex = static_cast<uint32>(Build->PendingWeldedVertices.Num());
		FStableProxyVertexGPU V;
		V.Position = FVector4f(P.X, P.Y, P.Z, 1.0f);
		Build->PendingWeldedVertices.Add(V);
		WeldedVertexMap.Add(Key, NewIndex);
		return NewIndex;
	};

	Build->Triangles.Reserve(SourceTriangleCount);
	for (int32 TriangleIndex = 0; TriangleIndex < SourceTriangleCount; ++TriangleIndex)
	{
		const uint32 I0 = Indices[TriangleIndex * 3 + 0];
		const uint32 I1 = Indices[TriangleIndex * 3 + 1];
		const uint32 I2 = Indices[TriangleIndex * 3 + 2];
		if (I0 >= VertexCount || I1 >= VertexCount || I2 >= VertexCount)
		{
			continue;
		}
		FStableBindingTriangle T;
		T.A = FVector3f(ProxyToSplat.TransformPosition(FVector(PositionBuffer.VertexPosition(I0))));
		T.B = FVector3f(ProxyToSplat.TransformPosition(FVector(PositionBuffer.VertexPosition(I1))));
		T.C = FVector3f(ProxyToSplat.TransformPosition(FVector(PositionBuffer.VertexPosition(I2))));
		if (FVector3f::CrossProduct(T.B - T.A, T.C - T.A).SizeSquared() <= 1.0e-8f)
		{
			continue;
		}
		FStableProxyTriangleGPU& StableTri = Build->PendingTriangleVertices[TriangleIndex];
		StableTri.Vertex0 = FindOrAddWeldedVertex(T.A);
		StableTri.Vertex1 = FindOrAddWeldedVertex(T.B);
		StableTri.Vertex2 = FindOrAddWeldedVertex(T.C);

		// Preserve mesh winding, but keep a clean geometric face normal/area for the
		// v2809 topology-derived Stable normal field. Imported vertex normals are only
		// used below to orient the geometric normal consistently and as a last-resort
		// fallback; they are no longer the smoothing source.
		const FVector3f FaceCross = FVector3f::CrossProduct(T.B - T.A, T.C - T.A);
		const float FaceCrossLength = FaceCross.Size();
		FVector3f GeometricNormal = FaceCrossLength > 1.0e-6f ? (FaceCross / FaceCrossLength) : FVector3f(0, 0, 1);
		FVector3f ProxyNormal = GeometricNormal;
		FVector3f CornerNormals[3] = { GeometricNormal, GeometricNormal, GeometricNormal };
		if (StaticMeshVertexBuffer.GetNumVertices() > FMath::Max(I0, FMath::Max(I1, I2)))
		{
			const FVector4f TZ0 = StaticMeshVertexBuffer.VertexTangentZ(I0);
			const FVector4f TZ1 = StaticMeshVertexBuffer.VertexTangentZ(I1);
			const FVector4f TZ2 = StaticMeshVertexBuffer.VertexTangentZ(I2);
			const FVector SourceNormals[3] = {
				FVector(TZ0.X, TZ0.Y, TZ0.Z),
				FVector(TZ1.X, TZ1.Y, TZ1.Z),
				FVector(TZ2.X, TZ2.Y, TZ2.Z) };
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const FVector Authored = ProxyToSplat.TransformVectorNoScale(SourceNormals[Corner]).GetSafeNormal();
				if (!Authored.IsNearlyZero())
				{
					CornerNormals[Corner] = FVector3f(Authored);
				}
			}
			const FVector3f Smoothed = (CornerNormals[0] + CornerNormals[1] + CornerNormals[2]).GetSafeNormal();
			if (!Smoothed.IsNearlyZero())
			{
				ProxyNormal = Smoothed;
			}
		}
		// Orient the geometric face normal toward the authored hemisphere when possible.
		// This protects meshes whose section winding differs from their shading normal,
		// without importing authored high-frequency noise into the Stable smooth field.
		const FVector3f AuthoredAverage = (CornerNormals[0] + CornerNormals[1] + CornerNormals[2]).GetSafeNormal();
		if (!AuthoredAverage.IsNearlyZero() && FVector3f::DotProduct(GeometricNormal, AuthoredAverage) < 0.0f)
		{
			GeometricNormal *= -1.0f;
		}
		GeometricFaceNormals[TriangleIndex] = GeometricNormal;
		GeometricFaceAreas[TriangleIndex] = 0.5f * FaceCrossLength;
		NormalCornerVertexIDs[TriangleIndex * 3 + 0] = FindOrAddNormalWeldVertex(T.A);
		NormalCornerVertexIDs[TriangleIndex * 3 + 1] = FindOrAddNormalWeldVertex(T.B);
		NormalCornerVertexIDs[TriangleIndex * 3 + 2] = FindOrAddNormalWeldVertex(T.C);

		// Keep exact authored corner normals separately from the welded position topology.
		// Welding is correct for cast visibility, but a single welded normal would smear
		// hard edges. The shader interpolates these three source-corner normals using the
		// already cached nearest-point barycentrics for each splat.
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const FVector3f N = CornerNormals[Corner].GetSafeNormal();
			Build->PendingTriangleVertexNormals[TriangleIndex * 3 + Corner] = FVector4f(N.X, N.Y, N.Z, 1.0f);
		}
		StableTri.Valid = PackStableProxyNormalAndClosure(ProxyNormal, 1.0f);
		T.Normal = ProxyNormal;
		T.BoundsMin = T.A.ComponentMin(T.B).ComponentMin(T.C);
		T.BoundsMax = T.A.ComponentMax(T.B).ComponentMax(T.C);
		T.Centroid = (T.A + T.B + T.C) / 3.0f;
		T.TriangleIndex = static_cast<uint32>(TriangleIndex);
		Build->Triangles.Add(T);
	}

	if (Build->Triangles.Num() == 0)
	{
		StableBindingState = EStableProxyBindingState::Failed;
		UE_LOG(LogTemp, Warning, TEXT("[GSStableBinding] State=Failed Reason=NoValidProxyTriangles FrontRev=%u"), StableBindingRevision);
		return;
	}

	Build->TriangleOrder.SetNumUninitialized(Build->Triangles.Num());
	for (int32 Index = 0; Index < Build->Triangles.Num(); ++Index)
	{
		Build->TriangleOrder[Index] = Index;
	}
	Build->BVHNodes.Reserve(Build->Triangles.Num() * 2);
	Build->RootNode = BuildStableBindingBVH(
		Build->Triangles, Build->TriangleOrder, Build->BVHNodes, 0, Build->Triangles.Num());

	// v2745 dual-scale stable geometry closure. The local and broad fields are traced
	// from the same seven deterministic rays, then normal-aware adjacency smoothing
	// removes low-frequency blotches on broad planar receivers without bleeding across
	// hard edges. Camera/FOV/sort/light direction are intentionally absent.
	const double ClosureStartSeconds = FPlatformTime::Seconds();
	const float LocalClosureRadius = FMath::Max(StableClosureRadiusCm, 25.0f);
	const float BroadClosureRadius = FMath::Max(LocalClosureRadius * 2.5f, LocalClosureRadius + 250.0f);

	TArray<FVector2f> SmoothedClosure;
	SmoothedClosure.Init(FVector2f(1.0f, 1.0f), SourceTriangleCount);
	TArray<FVector3f> StableTriangleNormals;
	StableTriangleNormals.Init(FVector3f::ZeroVector, SourceTriangleCount);
	TArray<uint8> ValidClosureTriangle;
	ValidClosureTriangle.Init(0u, SourceTriangleCount);

	// Compact CSR-style vertex->triangle adjacency avoids hundreds of thousands of
	// small heap allocations on dense proxy meshes.
	const int32 StableVertexCount = Build->PendingWeldedVertices.Num();
	TArray<int32> VertexTriangleCounts;
	VertexTriangleCounts.Init(0, StableVertexCount);

	for (const FStableBindingTriangle& T : Build->Triangles)
	{
		const int32 SourceTri = static_cast<int32>(T.TriangleIndex);
		if (!Build->PendingTriangleVertices.IsValidIndex(SourceTri))
		{
			continue;
		}
		SmoothedClosure[SourceTri] = ComputeStableTriangleClosureOpenDual(
			Build->Triangles, Build->TriangleOrder, Build->BVHNodes, Build->RootNode,
			T, LocalClosureRadius, BroadClosureRadius);
		StableTriangleNormals[SourceTri] = T.Normal.GetSafeNormal();
		ValidClosureTriangle[SourceTri] = 1u;

		const FStableProxyTriangleGPU& StableTri = Build->PendingTriangleVertices[SourceTri];
		const uint32 VertexIDs[3] = { StableTri.Vertex0, StableTri.Vertex1, StableTri.Vertex2 };
		for (uint32 VertexID : VertexIDs)
		{
			if (VertexTriangleCounts.IsValidIndex(static_cast<int32>(VertexID)))
			{
				++VertexTriangleCounts[VertexID];
			}
		}
	}

	TArray<int32> VertexTriangleOffsets;
	VertexTriangleOffsets.SetNumZeroed(StableVertexCount + 1);
	for (int32 VertexID = 0; VertexID < StableVertexCount; ++VertexID)
	{
		VertexTriangleOffsets[VertexID + 1] =
			VertexTriangleOffsets[VertexID] + VertexTriangleCounts[VertexID];
	}
	TArray<int32> VertexTriangleWrite = VertexTriangleOffsets;
	TArray<int32> VertexTriangleIndices;
	VertexTriangleIndices.SetNumUninitialized(VertexTriangleOffsets.Last());

	for (const FStableBindingTriangle& T : Build->Triangles)
	{
		const int32 SourceTri = static_cast<int32>(T.TriangleIndex);
		if (!ValidClosureTriangle.IsValidIndex(SourceTri) || ValidClosureTriangle[SourceTri] == 0u)
		{
			continue;
		}
		const FStableProxyTriangleGPU& StableTri = Build->PendingTriangleVertices[SourceTri];
		const uint32 VertexIDs[3] = { StableTri.Vertex0, StableTri.Vertex1, StableTri.Vertex2 };
		for (uint32 VertexID : VertexIDs)
		{
			if (VertexTriangleCounts.IsValidIndex(static_cast<int32>(VertexID)))
			{
				VertexTriangleIndices[VertexTriangleWrite[VertexID]++] = SourceTri;
			}
		}
	}

	// v2809 TOPOLOGY-DERIVED STABLE NORMALS. v2808 showed that smoothing imported
	// corner normals still preserves export noise and hard seams. Build a separate
	// normal adjacency from a slightly wider positional weld, then reconstruct each
	// Stable corner normal from neighboring *geometric face normals*. Similar faces
	// inside 45 degrees are area/angle weighted; architectural hard edges stay split.
	// Up-facing floor/roof corners reject side-wall candidates so the floor converges
	// toward a coherent surface instead of inheriting vertical noise.
	{
		TArray<int32> NormalVertexTriangleCounts;
		NormalVertexTriangleCounts.Init(0, static_cast<int32>(NormalWeldVertexCount));
		for (const FStableBindingTriangle& T : Build->Triangles)
		{
			const int32 SourceTri = static_cast<int32>(T.TriangleIndex);
			if (!ValidClosureTriangle.IsValidIndex(SourceTri) || ValidClosureTriangle[SourceTri] == 0u)
			{
				continue;
			}
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 VertexID = NormalCornerVertexIDs[SourceTri * 3 + Corner];
				if (VertexID != MAX_uint32 && NormalVertexTriangleCounts.IsValidIndex(static_cast<int32>(VertexID)))
				{
					++NormalVertexTriangleCounts[VertexID];
				}
			}
		}

		TArray<int32> NormalVertexTriangleOffsets;
		NormalVertexTriangleOffsets.SetNumZeroed(NormalVertexTriangleCounts.Num() + 1);
		for (int32 VertexID = 0; VertexID < NormalVertexTriangleCounts.Num(); ++VertexID)
		{
			NormalVertexTriangleOffsets[VertexID + 1] = NormalVertexTriangleOffsets[VertexID] + NormalVertexTriangleCounts[VertexID];
		}
		TArray<int32> NormalVertexTriangleWrite = NormalVertexTriangleOffsets;
		TArray<int32> NormalVertexTriangleIndices;
		NormalVertexTriangleIndices.SetNumUninitialized(NormalVertexTriangleOffsets.Last());
		for (const FStableBindingTriangle& T : Build->Triangles)
		{
			const int32 SourceTri = static_cast<int32>(T.TriangleIndex);
			if (!ValidClosureTriangle.IsValidIndex(SourceTri) || ValidClosureTriangle[SourceTri] == 0u)
			{
				continue;
			}
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 VertexID = NormalCornerVertexIDs[SourceTri * 3 + Corner];
				if (VertexID != MAX_uint32 && NormalVertexTriangleCounts.IsValidIndex(static_cast<int32>(VertexID)))
				{
					NormalVertexTriangleIndices[NormalVertexTriangleWrite[VertexID]++] = SourceTri;
				}
			}
		}

		const TArray<FVector4f> ImportedCornerNormals = Build->PendingTriangleVertexNormals;
		TArray<FVector4f> TopologyCornerNormals = ImportedCornerNormals;
		constexpr float SmoothDotMin = 0.70710678f; // cos(45 degrees)
		constexpr float FloorBaseZ = 0.70f;
		constexpr float FloorNeighborMinZ = 0.50f;

		for (const FStableBindingTriangle& T : Build->Triangles)
		{
			const int32 SourceTri = static_cast<int32>(T.TriangleIndex);
			if (!ValidClosureTriangle.IsValidIndex(SourceTri) || ValidClosureTriangle[SourceTri] == 0u ||
				!GeometricFaceNormals.IsValidIndex(SourceTri))
			{
				continue;
			}
			const FVector3f BaseFace = GeometricFaceNormals[SourceTri].GetSafeNormal();
			if (BaseFace.IsNearlyZero())
			{
				continue;
			}
			const bool bFloorLike = BaseFace.Z >= FloorBaseZ;

			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 CornerIndex = SourceTri * 3 + Corner;
				const uint32 VertexID = NormalCornerVertexIDs[CornerIndex];
				FVector3f NormalSum = FVector3f::ZeroVector;
				float WeightSum = 0.0f;

				if (VertexID != MAX_uint32 && NormalVertexTriangleCounts.IsValidIndex(static_cast<int32>(VertexID)))
				{
					const int32 AdjStart = NormalVertexTriangleOffsets[VertexID];
					const int32 AdjEnd = NormalVertexTriangleOffsets[VertexID + 1];
					for (int32 AdjIndex = AdjStart; AdjIndex < AdjEnd; ++AdjIndex)
					{
						const int32 NeighborTri = NormalVertexTriangleIndices[AdjIndex];
						if (!GeometricFaceNormals.IsValidIndex(NeighborTri) || !GeometricFaceAreas.IsValidIndex(NeighborTri))
						{
							continue;
						}
						const FVector3f Candidate = GeometricFaceNormals[NeighborTri].GetSafeNormal();
						if (Candidate.IsNearlyZero())
						{
							continue;
						}
						const float FaceDot = FVector3f::DotProduct(BaseFace, Candidate);
						if (FaceDot < SmoothDotMin)
						{
							continue;
						}
						if (bFloorLike && Candidate.Z < FloorNeighborMinZ)
						{
							continue;
						}
						const float AngleT = FMath::Clamp((FaceDot - SmoothDotMin) / (1.0f - SmoothDotMin), 0.0f, 1.0f);
						const float AreaWeight = FMath::Sqrt(FMath::Max(GeometricFaceAreas[NeighborTri], 1.0e-4f));
						const float Weight = AreaWeight * (0.20f + 0.80f * AngleT * AngleT);
						NormalSum += Candidate * Weight;
						WeightSum += Weight;
					}
				}

				FVector3f SmoothNormal = WeightSum > 1.0e-5f ? (NormalSum / WeightSum).GetSafeNormal() : BaseFace;
				if (SmoothNormal.IsNearlyZero())
				{
					SmoothNormal = BaseFace;
				}
				// Keep the final hemisphere coherent with the imported shading normal if one exists,
				// but do not blend imported noise back into the result.
				if (ImportedCornerNormals.IsValidIndex(CornerIndex))
				{
					FVector3f Imported(ImportedCornerNormals[CornerIndex].X, ImportedCornerNormals[CornerIndex].Y, ImportedCornerNormals[CornerIndex].Z);
					Imported = Imported.GetSafeNormal();
					if (!Imported.IsNearlyZero() && FVector3f::DotProduct(SmoothNormal, Imported) < 0.0f)
					{
						SmoothNormal *= -1.0f;
					}
				}
				TopologyCornerNormals[CornerIndex] = FVector4f(SmoothNormal.X, SmoothNormal.Y, SmoothNormal.Z, 1.0f);
			}
		}

		Build->PendingTriangleVertexNormals = MoveTemp(TopologyCornerNormals);
		for (const FStableBindingTriangle& T : Build->Triangles)
		{
			const int32 SourceTri = static_cast<int32>(T.TriangleIndex);
			const int32 BaseIndex = SourceTri * 3;
			if (!StableTriangleNormals.IsValidIndex(SourceTri) || !Build->PendingTriangleVertexNormals.IsValidIndex(BaseIndex + 2))
			{
				continue;
			}
			const FVector4f& N0 = Build->PendingTriangleVertexNormals[BaseIndex + 0];
			const FVector4f& N1 = Build->PendingTriangleVertexNormals[BaseIndex + 1];
			const FVector4f& N2 = Build->PendingTriangleVertexNormals[BaseIndex + 2];
			FVector3f Avg(N0.X + N1.X + N2.X, N0.Y + N1.Y + N2.Y, N0.Z + N1.Z + N2.Z);
			Avg = Avg.GetSafeNormal();
			if (!Avg.IsNearlyZero())
			{
				StableTriangleNormals[SourceTri] = Avg;
			}
		}
		UE_LOG(LogTemp, Log, TEXT("[GSStableNormals] TopologyDerived=1 NormalWeldVertices=%u SmoothAngleDeg=45.0 WeldToleranceCm=0.25 FloorProtect=1"), NormalWeldVertexCount);
	}

	// Two normal-aware one-ring passes approximate a 2-ring surface filter. Shared
	// positions alone are not enough: the dot threshold keeps walls/eaves from
	// bleeding their closure onto a flat floor.
	TArray<FVector2f> NextClosure = SmoothedClosure;
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		for (const FStableBindingTriangle& T : Build->Triangles)
		{
			const int32 SourceTri = static_cast<int32>(T.TriangleIndex);
			if (!ValidClosureTriangle.IsValidIndex(SourceTri) || ValidClosureTriangle[SourceTri] == 0u)
			{
				continue;
			}

			const FVector3f N = StableTriangleNormals[SourceTri];
			FVector2f Sum = SmoothedClosure[SourceTri];
			float WeightSum = 1.0f;
			float CoherenceSum = 1.0f;
			float CoherenceWeight = 1.0f;

			const FStableProxyTriangleGPU& StableTri = Build->PendingTriangleVertices[SourceTri];
			const uint32 VertexIDs[3] = { StableTri.Vertex0, StableTri.Vertex1, StableTri.Vertex2 };
			for (uint32 VertexID : VertexIDs)
			{
				if (!VertexTriangleCounts.IsValidIndex(static_cast<int32>(VertexID)))
				{
					continue;
				}
				const int32 AdjStart = VertexTriangleOffsets[VertexID];
				const int32 AdjEnd = VertexTriangleOffsets[VertexID + 1];
				for (int32 AdjIndex = AdjStart; AdjIndex < AdjEnd; ++AdjIndex)
				{
					const int32 NeighborTri = VertexTriangleIndices[AdjIndex];
					if (NeighborTri == SourceTri ||
						!ValidClosureTriangle.IsValidIndex(NeighborTri) ||
						ValidClosureTriangle[NeighborTri] == 0u)
					{
						continue;
					}
					const float NormalDot = FVector3f::DotProduct(N, StableTriangleNormals[NeighborTri]);
					if (NormalDot < 0.86f)
					{
						continue;
					}
					const float NormalT = FMath::Clamp((NormalDot - 0.86f) / 0.14f, 0.0f, 1.0f);
					const float Weight = 0.15f + 0.85f * NormalT * NormalT;
					Sum += SmoothedClosure[NeighborTri] * Weight;
					WeightSum += Weight;
					CoherenceSum += NormalDot * Weight;
					CoherenceWeight += Weight;
				}
			}

			FVector2f Filtered = Sum / FMath::Max(WeightSum, 1.0e-4f);
			Filtered = SmoothedClosure[SourceTri] * 0.30f + Filtered * 0.70f;

			// Generic planar stabilization: coherent, already-open surfaces are biased
			// toward fully open. This removes broad floor/roof mottling but does not touch
			// genuinely enclosed or high-curvature regions.
			const float Coherence = CoherenceSum / FMath::Max(CoherenceWeight, 1.0e-4f);
			const float PlanarConfidence =
				FMath::Clamp((Coherence - 0.94f) / 0.055f, 0.0f, 1.0f) *
				FMath::Clamp((Filtered.X - 0.58f) / 0.34f, 0.0f, 1.0f);
			Filtered.X = FMath::Lerp(Filtered.X, 1.0f, PlanarConfidence * 0.32f);
			Filtered.Y = FMath::Lerp(Filtered.Y, 1.0f, PlanarConfidence * 0.16f);
			NextClosure[SourceTri] = FVector2f(
				FMath::Clamp(Filtered.X, 0.0f, 1.0f),
				FMath::Clamp(Filtered.Y, 0.0f, 1.0f));
		}
		Swap(SmoothedClosure, NextClosure);
	}

	double LocalClosureSum = 0.0;
	double BroadClosureSum = 0.0;
	int32 ClosureCount = 0;
	for (const FStableBindingTriangle& T : Build->Triangles)
	{
		const int32 SourceTri = static_cast<int32>(T.TriangleIndex);
		if (!Build->PendingTriangleVertices.IsValidIndex(SourceTri) ||
			!ValidClosureTriangle.IsValidIndex(SourceTri) || ValidClosureTriangle[SourceTri] == 0u)
		{
			continue;
		}
		const FVector2f ClosureOpen = SmoothedClosure[SourceTri];
		FStableProxyTriangleGPU& StableTri = Build->PendingTriangleVertices[SourceTri];
		StableTri.Valid = PackStableProxyNormalAndClosure(
			StableTriangleNormals[SourceTri], ClosureOpen.X, ClosureOpen.Y);
		LocalClosureSum += ClosureOpen.X;
		BroadClosureSum += ClosureOpen.Y;
		++ClosureCount;
	}

	UE_LOG(LogTemp, Log,
		TEXT("[GSStableClosure] Ready Triangles=%d RaysPerTriangle=7 LocalRadiusCm=%.1f BroadRadiusCm=%.1f MeanLocalOpen=%.3f MeanBroadOpen=%.3f SmoothPasses=2 NormalAware=1 PlanarStabilize=1 BuildMs=%.2f CameraIndependent=1"),
		ClosureCount, LocalClosureRadius, BroadClosureRadius,
		ClosureCount > 0 ? float(LocalClosureSum / double(ClosureCount)) : 1.0f,
		ClosureCount > 0 ? float(BroadClosureSum / double(ClosureCount)) : 1.0f,
		(FPlatformTime::Seconds() - ClosureStartSeconds) * 1000.0);

	StableBindingInputCount = TotalSplatCount;
	StableBindingProcessedCount = 0;
	StableBindingUnprocessedCount = TotalSplatCount;
	StableBindingBuildContext = MoveTemp(Build);

	UE_LOG(LogTemp, Log,
		TEXT("[GSStableBinding] State=Building FrontRev=%u Rebuilds=%u Total=%d Chunk=%d ProxyTriangles=%d BVHNodes=%d AtomicFront=Preserved"),
		StableBindingRevision, StableBindingRebuildCount, StableBindingInputCount,
		FMath::Clamp(StableBindingChunkSize, 5000, 250000),
		StableBindingBuildContext->Triangles.Num(), StableBindingBuildContext->BVHNodes.Num());
	UE_LOG(LogTemp, Log, TEXT("[GSStableSurface] Prepared WeldedVertices=%d SourceTriangles=%d"),
		StableBindingBuildContext->PendingWeldedVertices.Num(), StableBindingBuildContext->PendingTriangleVertices.Num());
 if(IsRunningCommandlet() && FParse::Param(FCommandLine::Get(),TEXT("GSPCVRPreprocess"))){
  int32 Steps=0;while(StableBindingState==EStableProxyBindingState::Building && ++Steps<100000)ContinueStableProxyBindingBuild();
  UE_LOG(LogTemp,Display,TEXT("[GSPCVRLOD][BindingPrepared] Inputs=%d Valid=%d State=%d SavedLevel=0"),StableBindingInputCount,StableBindingValidCount,int32(StableBindingState));
 }

}

void UGaussianSplatComponent::ContinueStableProxyBindingBuild()
{
	if (StableBindingState != EStableProxyBindingState::Building || !StableBindingBuildContext.IsValid())
	{
		return;
	}

	TSharedPtr<FStableProxyBindingBuildContext> Build = StableBindingBuildContext;
	if (!SplatAsset || !SplatAsset->IsValid() || SplatAsset->GetSplatCount() != Build->TotalSplatCount)
	{
		StableBindingBuildContext.Reset();
		StableBindingState = EStableProxyBindingState::Failed;
		UE_LOG(LogTemp, Warning, TEXT("[GSStableBinding] State=Failed Reason=AssetChangedDuringBuild FrontRev=%u"), StableBindingRevision);
		return;
	}

	const int32 StartIndex = Build->NextSplatIndex;
	if (StartIndex >= Build->TotalSplatCount)
	{
		return;
	}
	const int32 ChunkSize = FMath::Clamp(StableBindingChunkSize, 5000, 250000);
	const int32 Count = FMath::Min(ChunkSize, Build->TotalSplatCount - StartIndex);

	TArray<FVector3f> SplatPositions;
	if (!SplatAsset->GetDecompressedPositionRange(StartIndex, Count, SplatPositions) || SplatPositions.Num() != Count)
	{
		StableBindingBuildContext.Reset();
		StableBindingState = EStableProxyBindingState::Failed;
		UE_LOG(LogTemp, Warning,
			TEXT("[GSStableBinding] State=Failed Reason=PositionRangeReadFailed Start=%d Count=%d FrontRev=%u"),
			StartIndex, Count, StableBindingRevision);
		return;
	}

	for (int32 LocalIndex = 0; LocalIndex < Count; ++LocalIndex)
	{
		const int32 SplatIndex = StartIndex + LocalIndex;
		float BestDistanceSq = Build->MaxDistanceSq;
		uint32 BestTriangleIndex = MAX_uint32;
		FVector3f BestBarycentric(1.0f, 0.0f, 0.0f);
		QueryStableBindingBVH(
			Build->Triangles, Build->TriangleOrder, Build->BVHNodes, Build->RootNode,
			SplatPositions[LocalIndex], BestDistanceSq, BestTriangleIndex, BestBarycentric);

		FStableProxySplatBindingGPU& Binding = Build->PendingBindings[SplatIndex];
		if (BestTriangleIndex != MAX_uint32 && BestDistanceSq <= Build->MaxDistanceSq)
		{
			Binding.TriangleIndex = BestTriangleIndex;
			Binding.PackedSmoothNormal = 0u;
			Binding.SmoothNormalValid = 0u;
			const float U = FMath::Clamp(BestBarycentric.Y, 0.0f, 1.0f);
			const float V = FMath::Clamp(BestBarycentric.Z, 0.0f, 1.0f);
			Binding.PackedBarycentric = GaussianSplattingUtils::PackHalf2x16(U, V);

			const FVector3f NearestNormal = EvaluateStableTriangleCornerNormal(Build->PendingTriangleVertexNormals, BestTriangleIndex, BestBarycentric);
			if (!NearestNormal.IsNearlyZero())
			{
				const float BestDistanceCm = FMath::Sqrt(FMath::Max(BestDistanceSq, 0.0f));
				const float SearchRadiusCm = FMath::Clamp(BestDistanceCm + 35.0f, 25.0f, 80.0f);
				TArray<FStableBindingNormalCandidate> Candidates; Candidates.Reserve(8);
				QueryStableBindingBVHNeighborhood(Build->Triangles, Build->TriangleOrder, Build->BVHNodes, Build->RootNode, SplatPositions[LocalIndex], FMath::Square(SearchRadiusCm), Candidates, 8);
				const bool bFloorLike = NearestNormal.Z >= 0.70f;
				const float CompatibleDotMin = 0.76604444f;
				const float SigmaCm = FMath::Max(SearchRadiusCm * 0.45f, 12.0f);
				const float SigmaSq = SigmaCm * SigmaCm;
				FVector3f NormalSum = NearestNormal * 2.0f;
				float WeightSum = 2.0f;
				for (const FStableBindingNormalCandidate& Candidate : Candidates)
				{
					if (Candidate.TriangleIndex == MAX_uint32) continue;
					const FVector3f CandidateNormal = EvaluateStableTriangleCornerNormal(Build->PendingTriangleVertexNormals, Candidate.TriangleIndex, Candidate.Barycentric);
					if (CandidateNormal.IsNearlyZero()) continue;
					const float NormalDot = FVector3f::DotProduct(NearestNormal, CandidateNormal);
					if (NormalDot < CompatibleDotMin || (bFloorLike && CandidateNormal.Z < 0.50f)) continue;
					const float AngleT = FMath::Clamp((NormalDot - CompatibleDotMin) / (1.0f - CompatibleDotMin), 0.0f, 1.0f);
					const float DistanceWeight = 1.0f / (1.0f + Candidate.DistanceSq / FMath::Max(SigmaSq, 1.0f));
					const float Weight = DistanceWeight * (0.20f + 0.80f * AngleT * AngleT);
					NormalSum += CandidateNormal * Weight; WeightSum += Weight;
				}
				const FVector3f SmoothNormal = (NormalSum / FMath::Max(WeightSum, 1.0e-4f)).GetSafeNormal();
				if (!SmoothNormal.IsNearlyZero())
				{
					Binding.PackedSmoothNormal = PackStableProxyNormalAndClosure(SmoothNormal, 1.0f, 1.0f);
					Binding.SmoothNormalValid = 1u;
				}
			}
			++StableBindingValidCount;
			Build->UniqueTriangleIDs.Add(BestTriangleIndex);
		}
		else
		{
			Binding.TriangleIndex = MAX_uint32;
			Binding.PackedBarycentric = 0u;
			Binding.PackedSmoothNormal = 0u;
			Binding.SmoothNormalValid = 0u;
			++StableBindingInvalidCount;
		}
	}

	Build->NextSplatIndex += Count;
	StableBindingProcessedCount = Build->NextSplatIndex;
	StableBindingUnprocessedCount = FMath::Max(Build->TotalSplatCount - Build->NextSplatIndex, 0);
	StableBindingUniqueTriangleCount = Build->UniqueTriangleIDs.Num();
	StableBindingProgressPercent = 100.0f * float(StableBindingProcessedCount) / float(FMath::Max(StableBindingInputCount, 1));

	const int32 ProgressBucket = FMath::Clamp(FMath::FloorToInt(StableBindingProgressPercent / 10.0f), 0, 10);
	if (ProgressBucket > Build->LastLoggedProgressBucket)
	{
		Build->LastLoggedProgressBucket = ProgressBucket;
		UE_LOG(LogTemp, Log,
			TEXT("[GSStableBindingProgress] %.1f%% Processed=%d/%d Valid=%d Invalid=%d UniqueTriangles=%d FrontRev=%u"),
			StableBindingProgressPercent, StableBindingProcessedCount, StableBindingInputCount,
			StableBindingValidCount, StableBindingInvalidCount, StableBindingUniqueTriangleCount, StableBindingRevision);
	}

	if (Build->NextSplatIndex < Build->TotalSplatCount)
	{
		return;
	}

	const int32 ProxyTriangleCount = Build->Triangles.Num();
	const int32 BVHNodeCount = Build->BVHNodes.Num();
	const double BuildMs = (FPlatformTime::Seconds() - Build->StartSeconds) * 1000.0;
	const uint64 CompletedHash = HashStableBindings(Build->PendingBindings);
	StableSplatBindings = MakeShared<const TArray<FStableProxySplatBindingGPU>, ESPMode::ThreadSafe>(MoveTemp(Build->PendingBindings));
	StableProxyVertices = MoveTemp(Build->PendingWeldedVertices);
	StableProxyTriangles = MoveTemp(Build->PendingTriangleVertices);
	StableProxyTriangleVertexNormals = MoveTemp(Build->PendingTriangleVertexNormals);
	StableProxyRainExposure.Reset();
	// Keep the immutable BVH after the nearest-surface binding build. The dynamic
	// cast path reuses it and never rasterizes a rotating LightDepth texture.
	StableRayBVHContext = Build;
	StableFieldVertexCount = StableProxyVertices.Num();
	StableFieldTriangleCount = StableProxyTriangles.Num();
	StableBindingHash = CompletedHash;
	StableBindingState = StableBindingValidCount > 0 ? EStableProxyBindingState::Ready : EStableProxyBindingState::Failed;
	StableBindingProgressPercent = 100.0f;
	StableBindingUnprocessedCount = 0;
	++StableBindingRevision;
	StableBindingBuildContext.Reset();
	MarkRenderDynamicDataDirty();
	if (StableBindingState == EStableProxyBindingState::Ready)
	{
		SaveStableProxyBindingCache();
	}

	UE_LOG(LogTemp, Log,
		TEXT("[GSStableBinding] State=%s Rev=%u Hash=0x%016llX Rebuilds=%u Total=%d Processed=%d Valid=%d Invalid=%d Unprocessed=%d UniqueTriangles=%d ProxyTriangles=%d BVHNodes=%d BuildMs=%.2f AtomicCommit=1"),
		StableBindingState == EStableProxyBindingState::Ready ? TEXT("Ready") : TEXT("Failed"),
		StableBindingRevision, static_cast<unsigned long long>(StableBindingHash), StableBindingRebuildCount,
		StableBindingInputCount, StableBindingProcessedCount, StableBindingValidCount, StableBindingInvalidCount,
		StableBindingUnprocessedCount, StableBindingUniqueTriangleCount, ProxyTriangleCount, BVHNodeCount, BuildMs);
}

void UGaussianSplatComponent::RequestStableProxyFieldBuild()
{
	RequestStableProxyFieldBuildForCapture(0u);
}

void UGaussianSplatComponent::RequestStableProxyFieldBuildForCapture(uint32 LightDepthCaptureRevision)
{
	if (StableBindingState != EStableProxyBindingState::Ready || !StableSplatBindings.IsValid() || StableSplatBindings->Num() == 0 ||
		StableProxyVertices.Num() == 0 || StableProxyTriangles.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GSStableField] Request rejected: binding/surface not ready"));
		return;
	}
	RebuildStableProxyRayVisibility();
}

void UGaussianSplatComponent::RebuildStableRainExposure()
{
	StableProxyRainExposure.Reset();
	if (!StableRayBVHContext.IsValid() || StableProxyVertices.IsEmpty() || StableProxyTriangles.IsEmpty())
	{
		return;
	}

	// RainOcclusion v5: rain is a geometry-only sky visibility test.  The wet effect
	// is fail-open on all 3DGS; only a proxy surface encountered above a receiver can
	// make it dry. Stable proxy vertices are stored in Gaussian-component local
	// coordinates, so transform world +Z through the component only.
	const FVector LocalUpD = GetComponentTransform()
		.InverseTransformVectorNoScale(FVector::UpVector).GetSafeNormal();
	const FVector3f LocalUp(LocalUpD);
	if (LocalUp.IsNearlyZero())
	{
		return;
	}

	const TSharedPtr<FStableProxyBindingBuildContext> RayData = StableRayBVHContext;
	const float SceneSpan = RayData->BVHNodes.IsValidIndex(RayData->RootNode)
		? (RayData->BVHNodes[RayData->RootNode].BoundsMax - RayData->BVHNodes[RayData->RootNode].BoundsMin).Length()
		: 100000.0f;
	const float MaxRayDistance = FMath::Max(SceneSpan * 2.0f, 1000.0f);
	const float OriginBias = FMath::Max(0.75f, StableFieldDepthBiasCm + 0.5f);

	// Five PARALLEL rays make roof/eave borders spatially soft without turning the
	// shelter test into an angle-dependent hemisphere test.  Existing Stable Ray
	// Boundary Softening is reused only as a spatial edge-width control; it does not
	// alter the production self-cast result calculated below.
	const float Softness = FMath::Clamp(StableRayBoundarySoftening, 0.0f, 1.0f);
	const float OffsetRadiusCm = 35.0f * Softness;
	const FVector3f Reference = FMath::Abs(LocalUp.Z) < 0.9f
		? FVector3f(0.0f, 0.0f, 1.0f) : FVector3f(0.0f, 1.0f, 0.0f);
	const FVector3f Tangent = FVector3f::CrossProduct(LocalUp, Reference).GetSafeNormal();
	const FVector3f Bitangent = FVector3f::CrossProduct(LocalUp, Tangent).GetSafeNormal();
	const int32 RayCount = OffsetRadiusCm > KINDA_SMALL_NUMBER ? 5 : 1;

	StableProxyRainExposure.SetNumUninitialized(StableProxyVertices.Num());
	ParallelFor(StableProxyVertices.Num(), [this, RayData, LocalUp, Tangent, Bitangent,
		MaxRayDistance, OriginBias, OffsetRadiusCm, RayCount](int32 VertexIndex)
	{
		const FVector4f P4 = StableProxyVertices[VertexIndex].Position;
		const FVector3f BaseOrigin(P4.X, P4.Y, P4.Z);
		const FVector3f Offsets[5] =
		{
			FVector3f::ZeroVector,
			 Tangent * OffsetRadiusCm,
			-Tangent * OffsetRadiusCm,
			 Bitangent * OffsetRadiusCm,
			-Bitangent * OffsetRadiusCm
		};
		float VisibilitySum = 0.0f;
		for (int32 SampleIndex = 0; SampleIndex < RayCount; ++SampleIndex)
		{
			float ClosestT = MaxRayDistance;
			const FVector3f Origin = BaseOrigin + Offsets[SampleIndex] + LocalUp * OriginBias;
			QueryStableCastRayBVH(
				RayData->Triangles, RayData->TriangleOrder, RayData->BVHNodes, StableProxyTriangles,
				RayData->RootNode, Origin, LocalUp, static_cast<uint32>(VertexIndex), ClosestT);
			VisibilitySum += ClosestT < MaxRayDistance ? 0.0f : 1.0f;
		}
		StableProxyRainExposure[VertexIndex] = VisibilitySum / static_cast<float>(RayCount);
	});

	// One topology-aware averaging pass removes welded-triangle stair steps at roof
	// borders while retaining the actual shelter silhouette.
	if (Softness > KINDA_SMALL_NUMBER)
	{
		TArray<float> Sums;
		TArray<uint32> Counts;
		Sums.SetNumZeroed(StableProxyVertices.Num());
		Counts.SetNumZeroed(StableProxyVertices.Num());
		for (const FStableProxyTriangleGPU& Triangle : StableProxyTriangles)
		{
			if (Triangle.Valid == 0u || !StableProxyRainExposure.IsValidIndex(Triangle.Vertex0) ||
				!StableProxyRainExposure.IsValidIndex(Triangle.Vertex1) || !StableProxyRainExposure.IsValidIndex(Triangle.Vertex2))
			{
				continue;
			}
			const float FaceMean = (StableProxyRainExposure[Triangle.Vertex0] +
				StableProxyRainExposure[Triangle.Vertex1] + StableProxyRainExposure[Triangle.Vertex2]) / 3.0f;
			Sums[Triangle.Vertex0] += FaceMean; Sums[Triangle.Vertex1] += FaceMean; Sums[Triangle.Vertex2] += FaceMean;
			++Counts[Triangle.Vertex0]; ++Counts[Triangle.Vertex1]; ++Counts[Triangle.Vertex2];
		}
		TArray<float> Smoothed = StableProxyRainExposure;
		ParallelFor(Smoothed.Num(), [&Smoothed, this, &Sums, &Counts, Softness](int32 VertexIndex)
		{
			if (Counts[VertexIndex] == 0u) return;
			const float Neighbour = Sums[VertexIndex] / static_cast<float>(Counts[VertexIndex]);
			Smoothed[VertexIndex] = FMath::Clamp(FMath::Lerp(
				StableProxyRainExposure[VertexIndex], Neighbour, Softness * 0.45f), 0.0f, 1.0f);
		});
		StableProxyRainExposure = MoveTemp(Smoothed);
	}

	UE_LOG(LogTemp, Log, TEXT("[GSRainExposure] Built Vertices=%d Rays=%d OffsetRadiusCm=%.2f WorldUp=1"),
		StableProxyRainExposure.Num(), RayCount, OffsetRadiusCm);
}

bool UGaussianSplatComponent::BuildStableCastProofsForDirection(
	const FVector3f& LocalDirection, TArray<FVector4f>& OutProofs) const
{
	if (!StableRayBVHContext.IsValid() || StableProxyVertices.IsEmpty())
	{
		return false;
	}
	const FVector3f LocalDir = LocalDirection.GetSafeNormal();
	if (LocalDir.IsNearlyZero())
	{
		return false;
	}
	const TSharedPtr<FStableProxyBindingBuildContext> RayData = StableRayBVHContext;
	const float SceneSpan = RayData->BVHNodes.IsValidIndex(RayData->RootNode)
		? (RayData->BVHNodes[RayData->RootNode].BoundsMax - RayData->BVHNodes[RayData->RootNode].BoundsMin).Length()
		: 100000.0f;
	const float MaxRayDistance = FMath::Max(SceneSpan * 2.0f, 1000.0f);
	const float OriginBias = FMath::Max(0.5f, StableFieldDepthBiasCm + 0.5f);
	const float CastBoundarySoftening = FMath::Clamp(StableRayBoundarySoftening, 0.0f, 1.0f);
	const int32 CastRaySampleCount = CastBoundarySoftening > KINDA_SMALL_NUMBER ? 5 : 1;
	const float CastConeHalfAngleDegrees = 0.45f * CastBoundarySoftening;
	const float CastConeSlope = FMath::Tan(FMath::DegreesToRadians(CastConeHalfAngleDegrees));
	const FVector3f ReferenceAxis = FMath::Abs(LocalDir.Z) < 0.9f
		? FVector3f(0.0f, 0.0f, 1.0f) : FVector3f(0.0f, 1.0f, 0.0f);
	const FVector3f LightTangent = FVector3f::CrossProduct(LocalDir, ReferenceAxis).GetSafeNormal();
	const FVector3f LightBitangent = FVector3f::CrossProduct(LocalDir, LightTangent).GetSafeNormal();

    const bool bRuntimeAnyHit=UsePCVRAnyHit(this);
    const bool bValidateAnyHit=bRuntimeAnyHit && CVarPCVRValidateAnyHit.GetValueOnGameThread()!=0;
    FThreadSafeCounter AnyHitMismatches;
	OutProofs.SetNumUninitialized(StableProxyVertices.Num());
	ParallelFor(StableProxyVertices.Num(), [this, RayData, LocalDir, LightTangent, LightBitangent,
		MaxRayDistance, OriginBias, CastRaySampleCount, CastConeSlope, &OutProofs, bRuntimeAnyHit, bValidateAnyHit, &AnyHitMismatches](int32 VertexIndex)
	{
		const FVector4f P4 = StableProxyVertices[VertexIndex].Position;
		const FVector3f Origin(P4.X, P4.Y, P4.Z);
		const FVector3f SampleDirections[5] =
		{
			LocalDir,
			(LocalDir + LightTangent * CastConeSlope).GetSafeNormal(),
			(LocalDir - LightTangent * CastConeSlope).GetSafeNormal(),
			(LocalDir + LightBitangent * CastConeSlope).GetSafeNormal(),
			(LocalDir - LightBitangent * CastConeSlope).GetSafeNormal()
		};
		float VisibilitySum = 0.0f;
		float NearestHitT = MaxRayDistance;
		for (int32 SampleIndex = 0; SampleIndex < CastRaySampleCount; ++SampleIndex)
		{
			const FVector3f SampleDirection = SampleDirections[SampleIndex];
			float ClosestT = MaxRayDistance;
            if (bRuntimeAnyHit)
            {
                const bool Hit=QueryStableCastAnyHitBVH(RayData->Triangles,RayData->TriangleOrder,
                    RayData->BVHNodes,StableProxyTriangles,RayData->RootNode,
                    Origin+SampleDirection*OriginBias,SampleDirection,static_cast<uint32>(VertexIndex),MaxRayDistance);
                if (bValidateAnyHit)
                {
                    float ReferenceT=MaxRayDistance;
                    QueryStableCastRayBVH(RayData->Triangles,RayData->TriangleOrder,RayData->BVHNodes,StableProxyTriangles,
                        RayData->RootNode,Origin+SampleDirection*OriginBias,SampleDirection,static_cast<uint32>(VertexIndex),ReferenceT);
                    if (Hit!=(ReferenceT<MaxRayDistance)) AnyHitMismatches.Increment();
                }
                ClosestT=Hit?0.0f:MaxRayDistance;
            }
            else
            {
			QueryStableCastRayBVH(
				RayData->Triangles, RayData->TriangleOrder, RayData->BVHNodes, StableProxyTriangles,
				RayData->RootNode, Origin + SampleDirection * OriginBias, SampleDirection,
				static_cast<uint32>(VertexIndex), ClosestT);
            }
			VisibilitySum += ClosestT < MaxRayDistance ? 0.0f : 1.0f;
			NearestHitT = FMath::Min(NearestHitT, ClosestT);
		}
		const float Visibility = VisibilitySum / static_cast<float>(CastRaySampleCount);
		OutProofs[VertexIndex] = FVector4f(Visibility, Visibility, 1.0f, bRuntimeAnyHit ? -1.0f : NearestHitT);
	});

    if (bValidateAnyHit)
        UE_LOG(LogTemp,Display,TEXT("[GSPCVR][AnyHitValidation] Rays=%d Mismatches=%d"),
            StableProxyVertices.Num()*CastRaySampleCount,AnyHitMismatches.GetValue());
	TArray<float> VisibilitySums;
	TArray<uint32> VisibilityCounts;
	VisibilitySums.SetNumZeroed(StableProxyVertices.Num());
	VisibilityCounts.SetNumZeroed(StableProxyVertices.Num());
	for (const FStableProxyTriangleGPU& Triangle : StableProxyTriangles)
	{
		if (Triangle.Valid == 0u || !OutProofs.IsValidIndex(Triangle.Vertex0) ||
			!OutProofs.IsValidIndex(Triangle.Vertex1) || !OutProofs.IsValidIndex(Triangle.Vertex2))
		{
			continue;
		}
		const float FaceMean = (OutProofs[Triangle.Vertex0].X + OutProofs[Triangle.Vertex1].X +
			OutProofs[Triangle.Vertex2].X) / 3.0f;
		VisibilitySums[Triangle.Vertex0] += FaceMean;
		VisibilitySums[Triangle.Vertex1] += FaceMean;
		VisibilitySums[Triangle.Vertex2] += FaceMean;
		++VisibilityCounts[Triangle.Vertex0];
		++VisibilityCounts[Triangle.Vertex1];
		++VisibilityCounts[Triangle.Vertex2];
	}
	ParallelFor(OutProofs.Num(), [&OutProofs, &VisibilitySums, &VisibilityCounts, CastBoundarySoftening](int32 VertexIndex)
	{
		if (VisibilityCounts[VertexIndex] == 0u) return;
		const float NeighbourVisibility = VisibilitySums[VertexIndex] / static_cast<float>(VisibilityCounts[VertexIndex]);
		const float SoftVisibility = FMath::Clamp(FMath::Lerp(
			OutProofs[VertexIndex].X, NeighbourVisibility, CastBoundarySoftening), 0.0f, 1.0f);
		OutProofs[VertexIndex].X = SoftVisibility;
		OutProofs[VertexIndex].Y = SoftVisibility;
	});
	return true;
}

void UGaussianSplatComponent::ApplyStableCastTemporalFilter(
	const TArray<FVector4f>& RawProofs,
	const FVector3f& LocalDirection)
{
	if (RawProofs.IsEmpty())
	{
		return;
	}

	const float ResponseTime = FMath::Max(0.0f, StableCastTemporalResponseTime);
	const float DeltaSeconds = GetWorld() ? GetWorld()->GetDeltaSeconds() : (1.0f / 30.0f);
	const bool bInvalidHistory = StableProxyTemporalProofs.Num() != RawProofs.Num();
	const bool bDiscontinuousTime = !FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.0f || DeltaSeconds > 0.25f;
	const float DirectionDot = LastStableRayLightDirection.IsNearlyZero()
		? -1.0f
		: FVector3f::DotProduct(LocalDirection, LastStableRayLightDirection);
	// A large direction jump is a cut/scrub, not motion that should leave a trail.
	const bool bLightJump = DirectionDot < FMath::Cos(FMath::DegreesToRadians(12.0f));
	const bool bResetHistory = ResponseTime <= KINDA_SMALL_NUMBER || bInvalidHistory || bDiscontinuousTime || bLightJump;

	if (bResetHistory)
	{
		StableProxyTemporalProofs = RawProofs;
	}
	else
	{
		const float Alpha = FMath::Clamp(1.0f - FMath::Exp(-DeltaSeconds / ResponseTime), 0.0f, 1.0f);
		ParallelFor(RawProofs.Num(), [this, &RawProofs, Alpha](int32 VertexIndex)
		{
			const float PreviousVisibility = FMath::Clamp(StableProxyTemporalProofs[VertexIndex].Y, 0.0f, 1.0f);
			const float CurrentVisibility = FMath::Clamp(RawProofs[VertexIndex].Y, 0.0f, 1.0f);
			const float FilteredVisibility = FMath::Lerp(PreviousVisibility, CurrentVisibility, Alpha);
			FVector4f FilteredProof = RawProofs[VertexIndex];
			// X is the pre-temporal BVH visibility for diagnostic mode 102.
			// Y remains the exact pre-patch production temporal visibility for mode 103/Final.
			// No change to output from existing normal shadow rendering (which reads Y).
			FilteredProof.X = CurrentVisibility;
			FilteredProof.Y = FilteredVisibility;
			StableProxyTemporalProofs[VertexIndex] = FilteredProof;
		});
	}

	// RainOcclusion v5: proof Z was only a legacy validity/depth diagnostic in the
	// deterministic BVH production path (production cast reads X/Y). Encode the
	// static rain exposure as 2..3 so shaders can distinguish it from legacy Z=1.
	if (StableProxyRainExposure.Num() == StableProxyTemporalProofs.Num())
	{
		ParallelFor(StableProxyTemporalProofs.Num(), [this](int32 VertexIndex)
		{
			StableProxyTemporalProofs[VertexIndex].Z = 2.0f +
				FMath::Clamp(StableProxyRainExposure[VertexIndex], 0.0f, 1.0f);
		});
	}

	StableProxyRayProofs = StableProxyTemporalProofs;
	if (CVarGSPanoramaShadowStats.GetValueOnGameThread() == 1)
	{
		struct FVisibilitySummary
		{
			float Min = 1.0f, Max = 0.0f;
			double Sum = 0.0;
			int32 Valid = 0, Dark = 0, NonFinite = 0;
			void Add(float Value)
			{
				if (!FMath::IsFinite(Value)) { ++NonFinite; return; }
				Min = FMath::Min(Min, Value);
				Max = FMath::Max(Max, Value);
				Sum += Value;
				++Valid;
				if (Value < 0.95f) { ++Dark; }
			}
		};
		FVisibilitySummary RawStats, SmoothStats;
		for (const FVector4f& Proof : StableProxyRayProofs)
		{
			RawStats.Add(Proof.X);
			SmoothStats.Add(Proof.Y);
		}
		UE_LOG(LogTemp, Display,
			TEXT("[GSPanoramaShadowValues][GT][BVHVertices] Frame=%llu NextRev=%u Vertices=%d ")
			TEXT("RawMin=%.5f RawMean=%.5f RawMax=%.5f RawBelow095=%d ")
			TEXT("SmoothMin=%.5f SmoothMean=%.5f SmoothMax=%.5f SmoothBelow095=%d ")
			TEXT("NonFinite=%d ResetHistory=%d Note=VertexDistributionNotPixelDistribution"),
			static_cast<unsigned long long>(GFrameCounter), StableRayProofRevision + 1u,
			StableProxyRayProofs.Num(), RawStats.Valid ? RawStats.Min : -1.0f,
			RawStats.Valid ? RawStats.Sum / double(RawStats.Valid) : -1.0,
			RawStats.Valid ? RawStats.Max : -1.0f, RawStats.Dark,
			SmoothStats.Valid ? SmoothStats.Min : -1.0f,
			SmoothStats.Valid ? SmoothStats.Sum / double(SmoothStats.Valid) : -1.0,
			SmoothStats.Valid ? SmoothStats.Max : -1.0f, SmoothStats.Dark,
			RawStats.NonFinite + SmoothStats.NonFinite, bResetHistory ? 1 : 0);
	}
	UE_LOG(LogTemp, Verbose, TEXT("[GSStableTemporal] Reset=%d Response=%.4f Delta=%.6f DirectionDot=%.7f Vertices=%d"),
		bResetHistory ? 1 : 0, ResponseTime, DeltaSeconds, DirectionDot, RawProofs.Num());
}

void UGaussianSplatComponent::RebuildStableProxyRayVisibility()
{
    RebuildStableProxyRayVisibilityInternal(true);
}

void UGaussianSplatComponent::RebuildStableProxyRayVisibilityInternal(bool bNotifyRenderUpdate)
{
	const bool bTracePanoramaRay = CVarGSPanoramaShadowTrace.GetValueOnGameThread() > 0;
	auto TraceRaySkip = [this, bTracePanoramaRay](const TCHAR* Reason)
	{
		static uint32 SkippedRayLogCount = 0u; // game thread only; trace cap
		if (bTracePanoramaRay && SkippedRayLogCount < 128u)
		{
			++SkippedRayLogCount;
			UE_LOG(LogTemp, Warning,
				TEXT("[GSPanoramaTrace][GT][RaySkip] Owner=%s Reason=%s State=%d BindingRev=%u RayRev=%u Vertices=%d Triangles=%d Proofs=%d LightCount=%u Frame=%llu"),
				*GetNameSafe(GetOwner()), Reason, static_cast<int32>(StableBindingState), StableBindingRevision,
				StableRayProofRevision, StableProxyVertices.Num(), StableProxyTriangles.Num(),
				StableProxyRayProofs.Num(), CachedLightingData.LightCount,
				static_cast<unsigned long long>(GFrameCounter));
		}
	};
	if (StableProxyVertices.IsEmpty() || StableProxyTriangles.IsEmpty())
	{
		TraceRaySkip(TEXT("MissingProxyGeometry"));
		return;
	}
	if (!StableRayBVHContext.IsValid())
	{
		TSharedPtr<FStableProxyBindingBuildContext> NewRayData = MakeShared<FStableProxyBindingBuildContext>();
		NewRayData->Triangles.Reserve(StableProxyTriangles.Num());
		for (int32 TriangleIndex = 0; TriangleIndex < StableProxyTriangles.Num(); ++TriangleIndex)
		{
			const FStableProxyTriangleGPU& G = StableProxyTriangles[TriangleIndex];
			if (G.Valid == 0u || !StableProxyVertices.IsValidIndex(G.Vertex0) ||
				!StableProxyVertices.IsValidIndex(G.Vertex1) || !StableProxyVertices.IsValidIndex(G.Vertex2)) continue;
			FStableBindingTriangle T;
			const FVector4f PA = StableProxyVertices[G.Vertex0].Position;
			const FVector4f PB = StableProxyVertices[G.Vertex1].Position;
			const FVector4f PC = StableProxyVertices[G.Vertex2].Position;
			T.A = FVector3f(PA.X, PA.Y, PA.Z);
			T.B = FVector3f(PB.X, PB.Y, PB.Z);
			T.C = FVector3f(PC.X, PC.Y, PC.Z);
			T.BoundsMin = T.A.ComponentMin(T.B).ComponentMin(T.C);
			T.BoundsMax = T.A.ComponentMax(T.B).ComponentMax(T.C);
			T.Centroid = (T.A + T.B + T.C) / 3.0f;
			T.TriangleIndex = static_cast<uint32>(TriangleIndex);
			NewRayData->Triangles.Add(T);
		}
		NewRayData->TriangleOrder.SetNumUninitialized(NewRayData->Triangles.Num());
		for (int32 I = 0; I < NewRayData->TriangleOrder.Num(); ++I) NewRayData->TriangleOrder[I] = I;
		if (!NewRayData->Triangles.IsEmpty())
		{
			NewRayData->RootNode = BuildStableBindingBVH(
				NewRayData->Triangles, NewRayData->TriangleOrder, NewRayData->BVHNodes, 0, NewRayData->Triangles.Num());
			StableRayBVHContext = NewRayData;
		}
	}
	if (!StableRayBVHContext.IsValid())
	{
		TraceRaySkip(TEXT("BVHUnavailable"));
		return;
	}

	if (StableProxyRainExposure.Num() != StableProxyVertices.Num())
	{
		RebuildStableRainExposure();
	}

	// MRQ can ask for the same cast field more than once while producing one engine
	// frame. Rain exposure above is static, but the light-dependent cast stays once/frame.
	const uint64 CurrentBuildFrame = GFrameCounter;
	if (LastStableRayBuildFrame == CurrentBuildFrame)
	{
		TraceRaySkip(TEXT("AlreadyBuiltThisEngineFrame"));
		return;
	}
	if (CachedLightingData.LightCount == 0u)
	{
		TraceRaySkip(TEXT("NoLightsCached"));
		return;
	}

	FVector WorldDir = FVector::ZeroVector;
	if (bEnableAutomaticDayNightCycle && bRuntimeStableShadowDirectionValid)
	{
		WorldDir = RuntimeStableShadowDirection.GetSafeNormal();
	}
	else
	{
		int32 DirectionalIndex = INDEX_NONE;
		for (uint32 I = 0; I < CachedLightingData.LightCount; ++I)
		{
			if (FMath::Abs(CachedLightingData.LightDirectionType[I].W) < 0.25f)
			{
				DirectionalIndex = static_cast<int32>(I);
				break;
			}
		}
		if (DirectionalIndex == INDEX_NONE)
		{
			TraceRaySkip(TEXT("NoDirectionalLightCached"));
			return;
		}
		const FVector4f WorldDir4 = CachedLightingData.LightDirectionType[DirectionalIndex];
		WorldDir = FVector(WorldDir4.X, WorldDir4.Y, WorldDir4.Z).GetSafeNormal();
	}
	const FVector LocalDirD = GetProxyShadowMeshWorldTransform().InverseTransformVectorNoScale(WorldDir).GetSafeNormal();
	const FVector3f LocalDir(LocalDirD);
	if (LocalDir.IsNearlyZero())
	{
		TraceRaySkip(TEXT("InvalidLocalLightDirection"));
		return;
	}
    const bool bRuntimeAnyHit=UsePCVRAnyHit(this);
	if (StableProxyRayProofs.Num() == StableProxyVertices.Num() &&
        !StableProxyRayProofs.IsEmpty() && (StableProxyRayProofs[0].W<0.0f)==bRuntimeAnyHit &&
		FVector3f::DotProduct(LocalDir, LastStableRayLightDirection) > 0.99999995f)
	{
		// A matching field is intentionally reused, not re-uploaded or regenerated per pane.
		return;
	}

	const float CastBoundarySoftening = FMath::Clamp(StableRayBoundarySoftening, 0.0f, 1.0f);
	const int32 CastRaySampleCount = CastBoundarySoftening > KINDA_SMALL_NUMBER ? 5 : 1;
	const float CastConeHalfAngleDegrees = 0.45f * CastBoundarySoftening;
    const bool bPCVRRuntime = GSPCVR::IsRuntimeWorld(GetWorld());
    double CastLookupMs = 0.0;
    double CastBuildMs = 0.0;
    double CastSaveMs = 0.0;
    double CastFilterMs = 0.0;
    bool bRuntimeCacheHit = false;
    // Angle-cache interpolation changes the field. Runtime mode uses exact
    // directions until a separate visual validation enables approximations.
	if (bUseStableCastAngleCache && !bPCVRRuntime)
	{
		const float StepDegrees = FMath::Clamp(StableCastAngleCacheStepDegrees, 0.1f, 5.0f);
		const float YawDegrees = FMath::Fmod(FMath::RadiansToDegrees(FMath::Atan2(LocalDir.Y, LocalDir.X)) + 360.0f, 360.0f);
		const float ElevationRadians = FMath::Asin(FMath::Clamp(LocalDir.Z, -1.0f, 1.0f));
		const float LowerYaw = FMath::FloorToFloat(YawDegrees / StepDegrees) * StepDegrees;
		const float UpperYaw = FMath::Fmod(LowerYaw + StepDegrees, 360.0f);
		const float AngleBlend = FMath::Clamp((YawDegrees - LowerYaw) / StepDegrees, 0.0f, 1.0f);
		auto MakeDirection = [ElevationRadians](float Yaw)
		{
			const float YawRadians = FMath::DegreesToRadians(Yaw);
			const float Horizontal = FMath::Cos(ElevationRadians);
			return FVector3f(Horizontal * FMath::Cos(YawRadians), Horizontal * FMath::Sin(YawRadians), FMath::Sin(ElevationRadians)).GetSafeNormal();
		};
		const FVector3f LowerDirection = MakeDirection(LowerYaw);
		const FVector3f UpperDirection = MakeDirection(UpperYaw);
		TArray<FVector4f> LowerProofs;
		TArray<FVector4f> UpperProofs;
		const FString LowerPath = BuildStableCastCachePath(StableBindingHash, LowerDirection, StableFieldDepthBiasCm, CastBoundarySoftening);
		const FString UpperPath = BuildStableCastCachePath(StableBindingHash, UpperDirection, StableFieldDepthBiasCm, CastBoundarySoftening);
		const bool bLowerLoaded = LoadStableCastCache(LowerPath, StableBindingHash, LowerDirection,
			StableFieldDepthBiasCm, CastBoundarySoftening, StableProxyVertices.Num(), LowerProofs);
		const bool bUpperLoaded = LoadStableCastCache(UpperPath, StableBindingHash, UpperDirection,
			StableFieldDepthBiasCm, CastBoundarySoftening, StableProxyVertices.Num(), UpperProofs);
		if (bLowerLoaded && bUpperLoaded)
		{
			TArray<FVector4f> RawProofs;
			RawProofs.SetNumUninitialized(StableProxyVertices.Num());
			ParallelFor(RawProofs.Num(), [&RawProofs, &LowerProofs, &UpperProofs, AngleBlend](int32 VertexIndex)
			{
				RawProofs[VertexIndex] = FMath::Lerp(LowerProofs[VertexIndex], UpperProofs[VertexIndex], AngleBlend);
			});
			ApplyStableCastTemporalFilter(RawProofs, LocalDir);
			LastStableRayLightDirection = LocalDir;
			LastStableRayBuildFrame = CurrentBuildFrame;
			++StableRayProofRevision;
			StableFieldRequestRevision = StableRayProofRevision;
			StableFieldExpectedLightDepthRevision = 0u;
			if(bNotifyRenderUpdate)MarkRenderDynamicDataDirty();
			UE_LOG(LogTemp, Log, TEXT("[GSStableAngleCache] Interpolate Rev=%u Yaw=%.4f Lower=%.4f Upper=%.4f Blend=%.4f Elevation=%.4f Frame=%llu"),
				StableRayProofRevision, YawDegrees, LowerYaw, UpperYaw, AngleBlend,
				FMath::RadiansToDegrees(ElevationRadians), static_cast<unsigned long long>(CurrentBuildFrame));
			return;
		}
	}
	const FString StableCastCachePath = BuildStableCastCachePath(
		StableBindingHash, LocalDir, StableFieldDepthBiasCm, CastBoundarySoftening) +
        (bRuntimeAnyHit ? TEXT("_PCVRAnyHit") : TEXT(""));
	TArray<FVector4f> RawProofs;
    const double LookupStart = FPlatformTime::Seconds();
    bool bLoadedCast = false;
    if (bPCVRRuntime)
    {
        if (const TArray<FVector4f>* Cached = PCVRRuntimeCastCache.Find(StableCastCachePath))
        {
            if (Cached->Num() == StableProxyVertices.Num())
            { RawProofs = *Cached; bLoadedCast = true; bRuntimeCacheHit = true; }
        }
        // Once rendering is established, newly encountered light directions
        // must never trigger synchronous file reads or writes on the game thread.
    }
    else bLoadedCast = LoadStableCastCache(
        StableCastCachePath, StableBindingHash, LocalDir, StableFieldDepthBiasCm,
        CastBoundarySoftening, StableProxyVertices.Num(), RawProofs);
    CastLookupMs = (FPlatformTime::Seconds() - LookupStart) * 1000.0;
	if (bLoadedCast)
	{
		ApplyStableCastTemporalFilter(RawProofs, LocalDir);
		LastStableRayLightDirection = LocalDir;
		LastStableRayBuildFrame = CurrentBuildFrame;
		++StableRayProofRevision;
		StableFieldRequestRevision = StableRayProofRevision;
		StableFieldExpectedLightDepthRevision = 0u;
		if(bNotifyRenderUpdate)MarkRenderDynamicDataDirty();
		UE_LOG(LogTemp, Log,
			TEXT("[GSStableRayCache] Hit Rev=%u Vertices=%d Direction=(%.6f,%.6f,%.6f) Frame=%llu Path=%s"),
			StableRayProofRevision, StableProxyRayProofs.Num(), LocalDir.X, LocalDir.Y, LocalDir.Z,
			static_cast<unsigned long long>(CurrentBuildFrame), *StableCastCachePath);
		return;
	}
	const double StartSeconds = FPlatformTime::Seconds();
	if (!BuildStableCastProofsForDirection(LocalDir, RawProofs)) return;
    CastBuildMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
    const double SaveStart = FPlatformTime::Seconds();
    bool bCacheSaved = false;
    if (bPCVRRuntime)
    {
        constexpr uint64 RuntimeCacheBudget = 8ull * 1024ull * 1024ull;
        const uint64 EntryBytes = uint64(RawProofs.Num()) * sizeof(FVector4f);
        if (EntryBytes <= RuntimeCacheBudget)
        {
            uint64 Used = 0;
            for (const auto& Entry : PCVRRuntimeCastCache) Used += uint64(Entry.Value.GetAllocatedSize());
            while (!PCVRRuntimeCastOrder.IsEmpty() &&
                   (PCVRRuntimeCastOrder.Num() >= 16 || Used + EntryBytes > RuntimeCacheBudget))
            {
                const FString Oldest = PCVRRuntimeCastOrder[0];
                if (const auto* Old = PCVRRuntimeCastCache.Find(Oldest)) Used -= uint64(Old->GetAllocatedSize());
                PCVRRuntimeCastCache.Remove(Oldest);
                PCVRRuntimeCastOrder.RemoveAt(0);
            }
            PCVRRuntimeCastCache.Add(StableCastCachePath, RawProofs);
            PCVRRuntimeCastOrder.Add(StableCastCachePath);
        }
    }
    else bCacheSaved = SaveStableCastCache(
        StableCastCachePath, StableBindingHash, LocalDir, StableFieldDepthBiasCm,
        CastBoundarySoftening, RawProofs);
    CastSaveMs = (FPlatformTime::Seconds() - SaveStart) * 1000.0;
    const double FilterStart = FPlatformTime::Seconds();
	ApplyStableCastTemporalFilter(RawProofs, LocalDir);
    CastFilterMs = (FPlatformTime::Seconds() - FilterStart) * 1000.0;
    if (bPCVRRuntime)
        UE_LOG(LogTemp, Log, TEXT("[GSPCVR][CastStages] Policy=RuntimeExact LookupMs=%.3f RayBuildMs=%.3f StoreMs=%.3f FilterMs=%.3f Entries=%d DiskRead=0 DiskWrite=0 RaySamples=%d AnyHit=%d Frame=%llu"),
            CastLookupMs, CastBuildMs, CastSaveMs, CastFilterMs, PCVRRuntimeCastCache.Num(),
            CastRaySampleCount, bRuntimeAnyHit ? 1 : 0, static_cast<unsigned long long>(CurrentBuildFrame));
	LastStableRayLightDirection = LocalDir;
	LastStableRayBuildFrame = CurrentBuildFrame;
	++StableRayProofRevision;
	StableFieldRequestRevision = StableRayProofRevision;
	StableFieldExpectedLightDepthRevision = 0u;
	if(bNotifyRenderUpdate)MarkRenderDynamicDataDirty();
	UE_LOG(LogTemp, Log, TEXT("[GSStableRay] Rev=%u Vertices=%d BuildMs=%.2f Direction=(%.6f,%.6f,%.6f) LightDepth=0 FrameOnce=1 BoundarySoftening=%.3f RaySamples=%d ConeHalfAngleDeg=%.3f CacheSaved=%d Frame=%llu"),
		StableRayProofRevision, StableProxyRayProofs.Num(),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0,
		LocalDir.X, LocalDir.Y, LocalDir.Z, CastBoundarySoftening,
		CastRaySampleCount, CastConeHalfAngleDegrees, bCacheSaved ? 1 : 0,
		static_cast<unsigned long long>(CurrentBuildFrame));
}

void UGaussianSplatComponent::BakeStableCastAngleCache()
{
	if (!StableRayBVHContext.IsValid() || StableProxyVertices.IsEmpty())
	{
		StableCastAngleCacheStatus = TEXT("Stable binding/BVH is not ready. Run Rebuild Stable Binding first.");
		UE_LOG(LogTemp, Warning, TEXT("[GSStableAngleBake] Aborted: stable binding/BVH is not ready"));
		return;
	}
	const float StepDegrees = FMath::Clamp(StableCastAngleCacheStepDegrees, 0.1f, 5.0f);
	const float ElevationDegrees = FMath::Clamp(StableCastBakeElevationDegrees, -89.0f, 89.0f);
	const float ElevationRadians = FMath::DegreesToRadians(ElevationDegrees);
	const float Horizontal = FMath::Cos(ElevationRadians);
	const float CastBoundarySoftening = FMath::Clamp(StableRayBoundarySoftening, 0.0f, 1.0f);
	const int32 FieldCount = FMath::Max(1, FMath::CeilToInt(360.0f / StepDegrees));
	int32 BuiltCount = 0;
	int32 ExistingCount = 0;
	int32 FailedCount = 0;
	const double BakeStartSeconds = FPlatformTime::Seconds();
	StableCastAngleCacheStatus = FString::Printf(TEXT("Baking 0/%d"), FieldCount);
	for (int32 FieldIndex = 0; FieldIndex < FieldCount; ++FieldIndex)
	{
		const float YawDegrees = FMath::Fmod(static_cast<float>(FieldIndex) * StepDegrees, 360.0f);
		const float YawRadians = FMath::DegreesToRadians(YawDegrees);
		const FVector3f Direction(Horizontal * FMath::Cos(YawRadians), Horizontal * FMath::Sin(YawRadians), FMath::Sin(ElevationRadians));
		const FString CachePath = BuildStableCastCachePath(
			StableBindingHash, Direction, StableFieldDepthBiasCm, CastBoundarySoftening);
		TArray<FVector4f> Proofs;
		if (LoadStableCastCache(CachePath, StableBindingHash, Direction, StableFieldDepthBiasCm,
			CastBoundarySoftening, StableProxyVertices.Num(), Proofs))
		{
			++ExistingCount;
		}
		else if (BuildStableCastProofsForDirection(Direction, Proofs) &&
			SaveStableCastCache(CachePath, StableBindingHash, Direction, StableFieldDepthBiasCm,
				CastBoundarySoftening, Proofs))
		{
			++BuiltCount;
		}
		else
		{
			++FailedCount;
		}
		if ((FieldIndex + 1) % 25 == 0 || FieldIndex + 1 == FieldCount)
		{
			StableCastAngleCacheStatus = FString::Printf(TEXT("Baking %d/%d"), FieldIndex + 1, FieldCount);
			UE_LOG(LogTemp, Log, TEXT("[GSStableAngleBake] Progress=%d/%d Built=%d Existing=%d Failed=%d"),
				FieldIndex + 1, FieldCount, BuiltCount, ExistingCount, FailedCount);
		}
	}
	const double BakeSeconds = FPlatformTime::Seconds() - BakeStartSeconds;
	StableCastAngleCacheStatus = FString::Printf(
		TEXT("Ready: %d fields (new %d, reused %d), %.1fs"), FieldCount, BuiltCount, ExistingCount, BakeSeconds);
	bUseStableCastAngleCache = FailedCount == 0;
	LastStableRayLightDirection = FVector3f::ZeroVector;
	LastStableRayBuildFrame = MAX_uint64;
	MarkPackageDirty();
	UE_LOG(LogTemp, Log, TEXT("[GSStableAngleBake] Complete Fields=%d Built=%d Existing=%d Failed=%d Step=%.3f Elevation=%.3f Seconds=%.2f"),
		FieldCount, BuiltCount, ExistingCount, FailedCount, StepDegrees, ElevationDegrees, BakeSeconds);
}

void UGaussianSplatComponent::ResetProxyShadowMeshAlignment()
{
	Modify();
	ProxyShadowMeshLocationOffset = FVector::ZeroVector;
	ProxyShadowMeshRotationOffset = FRotator::ZeroRotator;
	ProxyShadowMeshScaleOffset = FVector::OneVector;
	InvalidateStableProxyBindings(TEXT("ProxyAlignmentReset"));
	RebuildProxyNormalCache();
	MarkPackageDirty();
	MarkRenderStateDirty();
	if (AGaussianSplatActor* SplatActor = Cast<AGaussianSplatActor>(GetOwner()))
	{
		SplatActor->SyncProxyShadowReceiver();
	}
}

void UGaussianSplatComponent::AutoAssignProxyShadowMesh()
{
#if WITH_EDITOR
	const FString TargetName = SplatAsset
		? SplatAsset->GetName()
		: (GetOwner() ? GetOwner()->GetName() : GetName());
	const FString TargetLower = TargetName.ToLower();
	const FString TargetPackagePath = SplatAsset
		? FPackageName::GetLongPackagePath(SplatAsset->GetOutermost()->GetName())
		: FString();

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	FARFilter Filter;
	Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	if (!TargetPackagePath.IsEmpty())
	{
		Filter.PackagePaths.Add(*TargetPackagePath);
	}

	TArray<FAssetData> MeshAssets;
	AssetRegistryModule.Get().GetAssets(Filter, MeshAssets);

	if (MeshAssets.Num() == 0 && !TargetPackagePath.IsEmpty())
	{
		Filter.PackagePaths.Reset();
		AssetRegistryModule.Get().GetAssets(Filter, MeshAssets);
	}

	auto NormalizeName = [](FString Name)
	{
		Name = Name.ToLower();
		Name.ReplaceInline(TEXT("gaussian"), TEXT(""));
		Name.ReplaceInline(TEXT("splat"), TEXT(""));
		Name.ReplaceInline(TEXT("proxy"), TEXT(""));
		Name.ReplaceInline(TEXT("shadow"), TEXT(""));
		Name.ReplaceInline(TEXT("catcher"), TEXT(""));
		Name.ReplaceInline(TEXT("mesh"), TEXT(""));
		Name.ReplaceInline(TEXT("static"), TEXT(""));
		Name.ReplaceInline(TEXT("_"), TEXT(""));
		Name.ReplaceInline(TEXT("-"), TEXT(""));
		Name.ReplaceInline(TEXT(" "), TEXT(""));
		return Name;
	};

	const FString NormalizedTarget = NormalizeName(TargetName);
	int32 BestScore = MIN_int32;
	FAssetData BestAsset;

	for (const FAssetData& AssetData : MeshAssets)
	{
		const FString CandidateName = AssetData.AssetName.ToString();
		const FString CandidateLower = CandidateName.ToLower();
		const FString NormalizedCandidate = NormalizeName(CandidateName);
		int32 Score = 0;

		if (CandidateLower == TargetLower || NormalizedCandidate == NormalizedTarget)
		{
			Score += 1000;
		}
		if (CandidateLower.Contains(TargetLower) || (!NormalizedTarget.IsEmpty() && NormalizedCandidate.Contains(NormalizedTarget)))
		{
			Score += 700;
		}
		if (TargetLower.Contains(CandidateLower) || (!NormalizedCandidate.IsEmpty() && NormalizedTarget.Contains(NormalizedCandidate)))
		{
			Score += 450;
		}
		if (CandidateLower.Contains(TEXT("proxy")))
		{
			Score += 180;
		}
		if (CandidateLower.Contains(TEXT("shadow")) || CandidateLower.Contains(TEXT("catcher")))
		{
			Score += 120;
		}
		if (!TargetPackagePath.IsEmpty() && AssetData.PackagePath.ToString() == TargetPackagePath)
		{
			Score += 220;
		}

		if (Score > BestScore)
		{
			BestScore = Score;
			BestAsset = AssetData;
		}
	}

	if (BestScore <= 0 || !BestAsset.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("LCCCinematicGS: No matching StaticMesh proxy found for '%s'. Import an OBJ/FBX as StaticMesh or assign ProxyShadowMesh manually."), *TargetName);
		return;
	}

	if (UStaticMesh* FoundMesh = Cast<UStaticMesh>(BestAsset.GetAsset()))
	{
		Modify();
		ProxyShadowMesh = FoundMesh;
		bUseProxyMeshForShadowCatcher = true;
		InvalidateStableProxyBindings(TEXT("ProxyAutoAssigned"));
		RebuildProxyNormalCache();
		MarkPackageDirty();
		MarkRenderStateDirty();
		if (AGaussianSplatActor* SplatActor = Cast<AGaussianSplatActor>(GetOwner()))
		{
			SplatActor->SyncProxyShadowReceiver();
		}
		UE_LOG(LogTemp, Log, TEXT("LCCCinematicGS: Assigned proxy shadow mesh '%s' for splat '%s'."), *FoundMesh->GetPathName(), *TargetName);
	}
#else
	UE_LOG(LogTemp, Warning, TEXT("LCCCinematicGS: AutoAssignProxyShadowMesh is editor-only."));
#endif
}

#if WITH_EDITOR
static bool IsRealtimeGaussianSplatEditorProperty(const FProperty* Property)
{
	if (!Property || Property->GetOwnerClass() != UGaussianSplatComponent::StaticClass())
	{
		return false;
	}

	const FName PropertyName = Property->GetFName();
	const bool bRequiresPrimitiveRebuild =
		PropertyName == FName(TEXT("SplatAsset")) ||
		PropertyName == FName(TEXT("SHOrder")) ||
		PropertyName == FName(TEXT("RenderProfile")) ||
		PropertyName == FName(TEXT("RuntimePositionPrecision")) ||
		PropertyName == FName(TEXT("RuntimeColorPrecision")) ||
		PropertyName == FName(TEXT("RuntimeFootprintMode")) ||
		PropertyName == FName(TEXT("ProxyShadowMesh")) ||
		PropertyName == FName(TEXT("ProxyShadowMeshLocationOffset")) ||
		PropertyName == FName(TEXT("ProxyShadowMeshRotationOffset")) ||
		PropertyName == FName(TEXT("ProxyShadowMeshScaleOffset")) ||
		PropertyName == FName(TEXT("bEnableProxySurfaceNormals")) ||
		PropertyName == FName(TEXT("ProxyNormalGridResolution")) ||
		PropertyName == FName(TEXT("ProxyNormalGridMarginCm")) ||
		PropertyName == FName(TEXT("ProxyNormalPropagationCells"));
	return !bRequiresPrimitiveRebuild;
}

void UGaussianSplatComponent::PreEditChange(FProperty* PropertyAboutToChange)
{
	UE_LOG(LogTemp, Warning,
		TEXT("[GSParamTrace] PreEdit Component=%p Owner=%s Property=%s Realtime=%d Registered=%d"),
		this, *GetNameSafe(GetOwner()), *GetNameSafe(PropertyAboutToChange),
		IsRealtimeGaussianSplatEditorProperty(PropertyAboutToChange) ? 1 : 0,
		IsRegistered() ? 1 : 0);
	// UActorComponent::PreEditChange unregisters the component before the new value
	// is committed. Runtime shader controls do not need that lifecycle operation;
	// bypassing it keeps the existing SceneProxy and proxy-normal cache alive while
	// an editor slider is dragged.
	if (IsRealtimeGaussianSplatEditorProperty(PropertyAboutToChange))
	{
		bEditorRealtimePropertyChangeActive = true;
		return;
	}
	Super::PreEditChange(PropertyAboutToChange);
}

void UGaussianSplatComponent::PreEditChange(FEditPropertyChain& PropertyAboutToChange)
{
	FProperty* ActiveProperty = PropertyAboutToChange.GetActiveNode()
		? PropertyAboutToChange.GetActiveNode()->GetValue()
		: nullptr;
	if (IsRealtimeGaussianSplatEditorProperty(ActiveProperty))
	{
		// UObject's chain overload continues into archetype/default-subobject
		// propagation after calling the FProperty overload. That propagation is what
		// unregisters the placed native component on every Details slider tick.
		PreEditChange(ActiveProperty);
		return;
	}
	UObject::PreEditChange(PropertyAboutToChange);
}

void UGaussianSplatComponent::PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedEvent)
{
	FProperty* ActiveProperty = PropertyChangedEvent.PropertyChain.GetActiveNode()
		? PropertyChangedEvent.PropertyChain.GetActiveNode()->GetValue()
		: nullptr;
	if (IsRealtimeGaussianSplatEditorProperty(ActiveProperty))
	{
		FPropertyChangedEvent PropertyEvent(ActiveProperty, PropertyChangedEvent.ChangeType);
		if (PropertyChangedEvent.PropertyChain.GetActiveMemberNode())
		{
			PropertyEvent.SetActiveMemberProperty(
				PropertyChangedEvent.PropertyChain.GetActiveMemberNode()->GetValue());
		}
		PostEditChangeProperty(PropertyEvent);
		return;
	}
	Super::PostEditChangeChainProperty(PropertyChangedEvent);
}

void UGaussianSplatComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	const FProperty* ChangedProperty = PropertyChangedEvent.MemberProperty
		? PropertyChangedEvent.MemberProperty
		: PropertyChangedEvent.Property;
	// Snow Gaussian is a BAKED, camera-independent asset. Parameter edits must
	// never silently leave an old snow shape looking as if it used the new UI
	// values. Flag only the snow-specific bake as stale; no other effects change.
	const bool bSnowGaussianBakeSettingChanged =
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowGaussianSpacingCm) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowGaussianThicknessCm) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowGaussianMoundVariationCm) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowGaussianMoundScaleCm) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowGaussianSlopeThreshold) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowGaussianSupportRadiusCm) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bSnowGaussianSkyExposure) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowGaussianMaxCount) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowGaussianColor) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowGaussianOpacity) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowGaussianDetailStrength) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowGaussianDetailScale) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowGaussianReliefBudgetFraction) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowGaussianViewResponseStrength) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bSnowGaussianLandformFeatures) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowGaussianFeatureStrength) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowGaussianLedgeRadiusCm) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowGaussianTipRadiusCm) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SnowGaussianVolumeStrength);
	if (bSnowGaussianBakeSettingChanged && SnowGaussianGeneratedCount > 0)
	{
		SnowGaussianBuildStatus = FString::Printf(
			TEXT("설정이 변경됨: 이전 Snow Gaussian %d개 표시 중. [눈·베이크 재생성]을 눌러 변경사항을 적용하세요."),
			SnowGaussianGeneratedCount);
	}
	// Production look/light/shadow/environment controls are shader/runtime data.
	// Recreating the SceneProxy for every slider tick temporarily removes the 3DGS from
	// the scene, which is the visible OFF/ON flash seen in the editor. Classify our own
	// non-structural properties by ownership instead of localized category text: category
	// strings can change or be unavailable for nested/array edits.
	const bool bRealtimeCinematicTuning =
		IsRealtimeGaussianSplatEditorProperty(ChangedProperty);
	UE_LOG(LogTemp, Warning,
		TEXT("[GSParamTrace] PostEdit Component=%p Owner=%s Property=%s ChangeType=%u Realtime=%d Registered=%d"),
		this, *GetNameSafe(GetOwner()), *PropertyName.ToString(),
		static_cast<uint32>(PropertyChangedEvent.ChangeType),
		bRealtimeCinematicTuning ? 1 : 0, IsRegistered() ? 1 : 0);
	bEditorRealtimePropertyChangeActive = false;
	TGuardValue<bool> RenderStateRecreationGuard(
		bSuppressRenderStateRecreationForEditorTuning,
		bRealtimeCinematicTuning);

	if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SplatAsset))
	{
		OnAssetChanged();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bSaveCurrentSeasonPresetNow))
	{
		if (bSaveCurrentSeasonPresetNow)
		{
			SaveCurrentSeasonPreset();
		}
		return;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bSaveCurrentWeatherPresetNow))
	{
		if (bSaveCurrentWeatherPresetNow)
		{
			SaveCurrentWeatherPreset();
		}
		return;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bSaveCurrentWeatherFogProfileNow))
	{
		if (bSaveCurrentWeatherFogProfileNow)
		{
			SaveCurrentWeatherFogProfile();
		}
		return;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NumberedSeasonPresetIndex))
	{
		RefreshNumberedSeasonPresetStatus();
		return;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bSaveNumberedSeasonPresetNow))
	{
		if (bSaveNumberedSeasonPresetNow)
		{
			SaveNumberedSeasonPreset();
		}
		return;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bLoadNumberedSeasonPresetNow))
	{
		if (bLoadNumberedSeasonPresetNow)
		{
			LoadNumberedSeasonPreset();
		}
		return;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NumberedWeatherPresetIndex))
	{
		RefreshNumberedWeatherPresetStatus();
		return;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bSaveNumberedWeatherPresetNow))
	{
		if (bSaveNumberedWeatherPresetNow)
		{
			SaveNumberedWeatherPreset();
		}
		return;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bLoadNumberedWeatherPresetNow))
	{
		if (bLoadNumberedWeatherPresetNow)
		{
			LoadNumberedWeatherPreset();
		}
		return;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonMode))
	{
		const ELCCCinematicGSSeasonMode RequestedMode = SeasonMode;
		CaptureCurrentEffectsToSeason(PreviousSeasonMode);
		if (!RestoreEffectsFromSeason(RequestedMode))
		{
			switch (RequestedMode)
			{
			case ELCCCinematicGSSeasonMode::Spring:
				ApplySpringSeasonPreset();
				break;
			case ELCCCinematicGSSeasonMode::Summer:
				ApplySummerSeasonPreset();
				break;
			case ELCCCinematicGSSeasonMode::Autumn:
				ApplyAutumnSeasonPreset();
				break;
			case ELCCCinematicGSSeasonMode::Winter:
				ApplyWinterSeasonPreset();
				break;
			default:
				break;
			}
			CaptureCurrentEffectsToSeason(RequestedMode);
		}
		SeasonMode = RequestedMode;
		PreviousSeasonMode = RequestedMode;
		RefreshCurrentSeasonPresetStatus();
		// Weather is a second layer: establish the selected season first, then
		// reapply the current weather overlay without baking it into the season.
		if (!RestoreEffectsFromWeather(WeatherMode))
		{
			ApplyWeatherPreset();
		}
		// Dedicated fog profiles are a final weather-local layer. Reapply them after
		// the season and normal weather preset so changing season never destroys the
		// saved fog look for the active weather.
		RestoreWeatherFogProfile(WeatherMode);
		RefreshCurrentWeatherFogProfileStatus();
		MarkPackageDirty();
		UpdateCachedSceneLighting();
		MarkRenderDynamicDataDirty();
		MarkRenderStateDirty();
		return;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, WeatherMode))
	{
		const ELCCCinematicGSWeatherMode RequestedWeather = WeatherMode;
		const bool bEnteringRain = RequestedWeather == ELCCCinematicGSWeatherMode::Rain;
		// Rain must be shadow-neutral. Snapshot the exact pre-Rain shadow/relighting
		// state before any weather preset work, then restore it after the Rain overlay.
		const float PreRainCapturedBaseShadowMinimum = CapturedBaseShadowMinimum;
		const float PreRainCapturedBaseShadowSaturation = CapturedBaseShadowSaturation;
		const float PreRainCapturedBaseShadowTransferStrength = CapturedBaseShadowTransferStrength;
		const float PreRainCapturedShadowGamma = CapturedShadowGamma;
		const float PreRainCapturedShadowMinimum = CapturedShadowMinimum;
		const float PreRainCapturedShadowStrength = CapturedShadowStrength;
		const float PreRainCapturedShadowBounceStrength = CapturedShadowBounceStrength;
		const float PreRainCapturedShadowSoftness = CapturedShadowSoftness;
		const float PreRainShadowCrush = ShadowCrush;
		const float PreRainProxyLightingMatchStrength = ProxyLightingMatchStrength;
		// Return to the stored season base before applying another weather. This
		// prevents fog/rain/snow values from accumulating across weather changes.
		if (!RestoreEffectsFromSeason(SeasonMode))
		{
			switch (SeasonMode)
			{
			case ELCCCinematicGSSeasonMode::Spring: ApplySpringSeasonPreset(); break;
			case ELCCCinematicGSSeasonMode::Summer: ApplySummerSeasonPreset(); break;
			case ELCCCinematicGSSeasonMode::Autumn: ApplyAutumnSeasonPreset(); break;
			case ELCCCinematicGSSeasonMode::Winter: ApplyWinterSeasonPreset(); break;
			default: break;
			}
		}
		WeatherMode = RequestedWeather;
		if (!RestoreEffectsFromWeather(RequestedWeather))
		{
			ApplyWeatherPreset();
			CaptureCurrentEffectsToWeather(RequestedWeather);
		}
		if (bEnteringRain)
		{
			CapturedBaseShadowMinimum = PreRainCapturedBaseShadowMinimum;
			CapturedBaseShadowSaturation = PreRainCapturedBaseShadowSaturation;
			CapturedBaseShadowTransferStrength = PreRainCapturedBaseShadowTransferStrength;
			CapturedShadowGamma = PreRainCapturedShadowGamma;
			CapturedShadowMinimum = PreRainCapturedShadowMinimum;
			CapturedShadowStrength = PreRainCapturedShadowStrength;
			CapturedShadowBounceStrength = PreRainCapturedShadowBounceStrength;
			CapturedShadowSoftness = PreRainCapturedShadowSoftness;
			ShadowCrush = PreRainShadowCrush;
			ProxyLightingMatchStrength = PreRainProxyLightingMatchStrength;
		}
		// Apply the saved fog profile last. It contains only fog fields, so Rain
		// shadow-neutrality and every non-fog weather feature remain untouched.
		RestoreWeatherFogProfile(RequestedWeather);
		WeatherMode = RequestedWeather;
		PreviousWeatherMode = RequestedWeather;
		RefreshCurrentWeatherPresetStatus();
		RefreshCurrentWeatherFogProfileStatus();
		MarkPackageDirty();
		UpdateCachedSceneLighting();
		MarkRenderDynamicDataDirty();
		MarkRenderStateDirty();
		return;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, StableRayBoundarySoftening))
	{
		StableRayBoundarySoftening = FMath::Clamp(StableRayBoundarySoftening, 0.0f, 1.0f);
		// The light may not have moved, but changing the spatial filter must rebuild
		// the proof once. Keep the BVH and all non-cast render state intact.
		LastStableRayLightDirection = FVector3f::ZeroVector;
		LastStableRayBuildFrame = MAX_uint64;
		StableProxyRainExposure.Reset();
		RebuildStableProxyRayVisibility();
		MarkPackageDirty();
		return;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, StableCastTemporalResponseTime))
	{
		StableCastTemporalResponseTime = FMath::Clamp(StableCastTemporalResponseTime, 0.0f, 0.5f);
		// A response-time edit starts from the next complete raw field instead of
		// carrying history produced with a different filter constant.
		StableProxyTemporalProofs.Reset();
		LastStableRayLightDirection = FVector3f::ZeroVector;
		LastStableRayBuildFrame = MAX_uint64;
		RebuildStableProxyRayVisibility();
		MarkPackageDirty();
		return;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, StableCastAngleCacheStepDegrees) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, StableCastBakeElevationDegrees) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bUseStableCastAngleCache))
	{
		StableCastAngleCacheStepDegrees = FMath::Clamp(StableCastAngleCacheStepDegrees, 0.1f, 5.0f);
		StableCastBakeElevationDegrees = FMath::Clamp(StableCastBakeElevationDegrees, -89.0f, 89.0f);
		StableCastAngleCacheStatus = TEXT("Settings changed; bake again");
		LastStableRayLightDirection = FVector3f::ZeroVector;
		LastStableRayBuildFrame = MAX_uint64;
		MarkPackageDirty();
		return;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SHOrder) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, RenderProfile) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, RuntimePositionPrecision) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, RuntimeColorPrecision) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, RuntimeFootprintMode) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, AdaptiveFootprintStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FootprintNearMinRadiusPixels) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FootprintMinRadiusPixels) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FootprintFarMinRadiusPixels) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FootprintNearMaxRadiusPixels) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FootprintMidMaxRadiusPixels) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FootprintFarMaxRadiusPixels) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FootprintNearDistance) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FootprintFarDistance) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FootprintAnisotropyLimit) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FootprintOpacityCompensation) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FootprintOpacityCompensationMaxExponent) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FootprintRadiusQuantization) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FootprintAdaptiveLookSafety) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bForceFullSortEveryFrame) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bDisableLODInReference) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableCinematicEffects) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableSeasonalColor) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonSourceColorFrom) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonSourceColorTo) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonTargetColorFrom) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonTargetColorTo) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonColorTolerance) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonColorFeather) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonMinSaturation) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonMinGreenDominance) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonSaturationScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonBrightnessScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonReplacementStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonOriginalDetailPreservation) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SeasonTargetStartColorBias) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bPreviewSeasonColorMask) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bReceiveUnrealLights) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LightingPreviewMode) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bAutoCollectSceneLights) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, MaxUnrealLights) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LightSearchRadius) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, UnrealLightInfluence) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, UnrealLightIntensityScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, UnrealAmbientIntensity) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, UnrealAmbientColor) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, UnrealIndirectLightIntensity) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, UnrealGroundBounce) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, UnrealShadowSaturation) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableUnrealShadowLighting) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, UnrealShadowTint) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, UnrealLightingNormalMode) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, UnrealDirectionalResponse) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, UnrealLightWrap) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableProxySurfaceNormals) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyNormalStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyNormalConfidenceThreshold) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FoliageProxyNormalStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyNormalGridResolution) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyNormalGridMarginCm) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyNormalPropagationCells) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, StableLightingNormalFullDistanceCm) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, StableLightingNormalMaxDistanceCm) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableApproximateShadows) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, UnrealShadowStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, UnrealContactShadowStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, UnrealLightingContrast) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, UnrealMicroNormalStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, UnrealShadowSoftness) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, UnrealShadowBreakup) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySelfShadowDepthBias) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyExternalShadowDepthBias) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableRelightingCore) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, RelightStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ApproxAlbedoStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ApproxAlbedoShadowLift) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ApproxAlbedoHighlightReduction) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableSpatialDelighting) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SpatialDelightingStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SpatialDelightingChromaStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SpatialDelightingMinLighting) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SpatialDelightingMaxLighting) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SpatialDelightingDarkProtection) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SpatialDelightingMaxAlbedoLuminance) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SpatialDelightingFoliageProtection) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SpatialDelightingMaxChromaScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SpatialDelightingDifferenceGain) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SpatialDelightingEvidenceMinEV) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SpatialDelightingEvidenceMaxEV) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SpatialDelightingMaxCorrectionWeight) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SpatialDelightingFoliageCorrectionScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SpatialDelightingMaxShadowLift) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SpatialDelightingMinHighlightSuppression) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SpatialDelightingSignedDifferenceRangeEV) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetLookStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, Original3DGSBrightness) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetExposureEV) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetToeStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetChromaRecovery) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetShadowColorRetention) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetGreenBoost) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetMidtoneContrast) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SunlitGradeStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SunlitGamma) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SunlitSaturation) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SunlitBrightness) ||

			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableGaussianSunAccent) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GaussianSunAccentStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GaussianSunAccentStart) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GaussianSunAccentFull) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GaussianSunAccentGamma) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GaussianSunAccentTint) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GaussianSunAccentTintStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GaussianSunAccentHighlightCompression) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GaussianSunAccentDetailRetention) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableGaussianPlantTransmission) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GaussianPlantTransmissionStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GaussianPlantBacklightStart) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GaussianPlantBacklightFull) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GaussianPlantTransmissionGamma) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GaussianPlantTransmissionTint) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GaussianPlantTransmissionTintStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GaussianPlantTransmissionSaturation) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GaussianPlantViewPhasePower) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ShadowGradeStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ShadowAreaGamma) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ShadowAreaSaturation) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ShadowAreaBrightness) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetShadowLift) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetSunTint) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetSkyTint) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetDirectScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetBakedSunSuppression) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetFilmicShoulderStart) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetFilmicShoulderStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetGreenSaturationRecovery) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetBrightGreenCompression) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetGreenLuminanceReduction) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetMicroContrast) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetCoarseContrast) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetSplatSharpness) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetHighlightSharpenProtection) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetCoverageEdgeProtection) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, TargetCompositeDebugView) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, AtmosphereStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, GroundFogDensity) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, DistanceHaze) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogHeightFalloff) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogDensityVariation) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogWeatherStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogVisibilityDistance) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NativeFogDensity) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NativeFogStartDistanceCm) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NativeFogMaxOpacity) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableNativeVolumetricFog) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableFogOverlayInOtherWeather) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogColor) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, DenseFogScatteringColor) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, WarmSunScatter) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CoolShadowTint) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ShadowCrush) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, DappledLightStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, DappledLightScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, HighlightBloomBoost) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SunExposureBoost) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ShadowDesaturation) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FarLightFade) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FarSaturationFade) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, HighlightGlareScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ColorSaturationBoost) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogDistanceStart) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FogDistanceEnd) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NearAirStart) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NearAirEnd) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, DistanceFogWeight) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, NearFogWeight) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, HazeDensityMin) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, HazeDensityMax) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, HighlightLumaGateLow) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, HighlightLumaGateHigh) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SunMaskGateLow) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SunMaskGateHigh) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LocalBloomMaskLow) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LocalBloomMaskHigh) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LocalBloomStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LocalBloomGlareMix) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CinematicBloomThreshold) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, PluginBloomSpreadPixels) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, PluginBloomScatter) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, PluginBloomSunColorInfluence) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, PluginColorBloomStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, PluginColorBloomThreshold) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, PluginColorBloomMinSaturation) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, PluginBloomSoftKnee) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LitAirNearWeight) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LitAirFarWeight) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, HazeGlowStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LitAirGlowStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LightShaftBaseStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LightShaftHighlightStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SunLiftBase) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SunLiftDistance) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableProxyVolumetricShafts) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyVolumetricShaftStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyVolumetricShaftDensity) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyVolumetricShaftMaxDistance) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyVolumetricShaftForwardScattering) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyVolumetricShaftTint) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySurfaceEffectMode) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySurfaceEffectStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySurfaceHorizontalBias) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySurfacePatternScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySurfaceEffectColor) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySurfaceSpecularStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySnowThicknessCm) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySnowSplatExpansion) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySnowGroundFillStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySnowStructureRemoval) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySnowStructureSlopeThreshold) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bRainPrecipitationEnabled) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, RainOverlayStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, RainOverlayExposureEV) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, RainOverlayGamma) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, RainOverlaySaturation) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, RainOverlayTint) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, RainOverlayTintStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyWetDarkening) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyPuddleAmount) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyWetGroundFullDistanceCm) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyWetGroundMaxDistanceCm) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bPreviewWetGroundMask) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyRainRippleStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyRainRippleSizeCm) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyRainRippleSpacingCm) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyRainRippleSpeed) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bUseProxyMeshForShadowCatcher) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowMesh) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowReceiverMaterial) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ExternalShadowCasters) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyCaptureResolutionScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyCaptureMaxDimension) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedShadowSoftness) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedShadowGamma) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedShadowMinimum) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedShadowStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyDepthShadowStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyLightingMatchStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedClosedShadowMaxDarkness) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedClosedShadowFullBlackEnd) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedClosedShadowStart) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedClosedShadowAreaScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedClosedShadowEdgeSoftness) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedDirectLightWhitePoint) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedDirectLightResponseGamma) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedBaseShadowTransferStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedBaseShadowMinimum) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedBaseShadowSaturation) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedBaseShadowTint) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedReflectedLightTintStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedReflectedLightDeepTintNeutralization) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedReflectedLightDeepSaturationRetention) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedReflectedLightHighlightCompression) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedReflectedLightTintSoftness) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedShadowBounceStrength) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedReflectedLightClosureOcclusion) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedReflectedLightResponseGamma) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedReflectedLightHorizonRetention) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedOcclusionShadowBoost) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, StableClosedSkyFloor) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, StableClosedBounceFloor) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, StableClosureRadiusCm) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedShadowDistanceLightening) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedShadowDistanceRange) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CapturedShadowPhysicalSoftness) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowSampleSpacing) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, MaxProxyShadowSamples) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowMeshLocationOffset) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowMeshRotationOffset) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowMeshScaleOffset) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, OpacityScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SplatScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LODErrorThreshold))
	{
		const bool bStableBindingSourceChanged =
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowMesh) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, StableBindingMaxDistanceCm) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, StableClosureRadiusCm) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowMeshLocationOffset) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowMeshRotationOffset) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowMeshScaleOffset);
		if (bStableBindingSourceChanged)
		{
			InvalidateStableProxyBindings(TEXT("ProxySourceChanged"));
		}

		const bool bProxyNormalSourceChanged =
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowMesh) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowMeshLocationOffset) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowMeshRotationOffset) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowMeshScaleOffset) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableProxySurfaceNormals) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyNormalGridResolution) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyNormalGridMarginCm) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyNormalPropagationCells);
		if (bProxyNormalSourceChanged)
		{
			RebuildProxyNormalCache();
		}
		UpdateCachedSceneLighting();
		MarkRenderDynamicDataDirty();
		if (!bRealtimeCinematicTuning)
		{
			MarkRenderStateDirty();
		}
		const bool bNeedsProxyReceiverSync =
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bEnableCinematicEffects) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bUseProxyMeshForShadowCatcher) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowMesh) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyCaptureResolutionScale) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyCaptureMaxDimension) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowReceiverMaterial) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ExternalShadowCasters) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowMeshLocationOffset) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowMeshRotationOffset) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowMeshScaleOffset) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxySurfaceEffectMode);
		if (bNeedsProxyReceiverSync)
		{
			if (AGaussianSplatActor* SplatActor = Cast<AGaussianSplatActor>(GetOwner()))
			{
				SplatActor->SyncProxyShadowReceiver();
			}
		}
	}

	if ((PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowMesh) ||
		 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ExternalShadowCasters)) &&
		ProxyShadowMesh && (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ProxyShadowMesh) || ExternalShadowCasters.Num() > 0))
	{
		bUseProxyMeshForShadowCatcher = true;
		bEnableApproximateShadows = false;
		if (bEnableProxySurfaceNormals)
		{
			UnrealLightingNormalMode = ELCCCinematicGSLightingNormalMode::HybridProxy;
		}
		UpdateCachedSceneLighting();
		MarkRenderDynamicDataDirty();
		if (!bRealtimeCinematicTuning)
		{
			MarkRenderStateDirty();
		}
		if (AGaussianSplatActor* SplatActor = Cast<AGaussianSplatActor>(GetOwner()))
		{
			SplatActor->SyncProxyShadowReceiver();
		}
	}
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LightingPreviewMode))
	{
		if (UWorld* World = GetWorld())
		{
			FlushPersistentDebugLines(World);
		}
		bProxyVertexDebugDrawn = false;
		bProxyShadowProbeWorkActive = false;
		bProxyShadowProbeRebuildPending = true;
		ProxyShadowProbeDebugAccumulator = 0.0f;
	}
	// Do not call any parent PostEdit function for pure runtime tuning. Even the
	// UObject-level notification is observed by the owning actor and can rerun its
	// construction path, which unregisters/registers this component and temporarily
	// removes the 3DGS while a slider is held. The value is already committed and the
	// render-thread payload was dirtied above, so no parent notification is required.
	if (bRealtimeCinematicTuning)
	{
		return;
	}
	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif

void UGaussianSplatComponent::OnRegister()
{
	UE_LOG(LogTemp, Warning,
		TEXT("[GSParamTrace] OnRegister Component=%p Owner=%s CreationMethod=%d"),
		this, *GetNameSafe(GetOwner()), static_cast<int32>(CreationMethod));
	Super::OnRegister();

#if WITH_EDITOR
	if (!LightPropertyChangedDelegateHandle.IsValid())
	{
		LightPropertyChangedDelegateHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddUObject(
			this, &UGaussianSplatComponent::OnAnyObjectPropertyChanged);
	}
#endif

	// Registration also repairs stale Fog-atmosphere values only. Rain shadow state
	// remains untouched during editor rebuild/hot-reload.
	if (WeatherMode == ELCCCinematicGSWeatherMode::Rain)
	{
		ResetRainFogLeakStateToOriginalBaseline(this, false);
	}

	UpdateCachedSceneLighting();
	MarkRenderDynamicDataDirty();

	if (SplatAsset)
	{
		bBoundsCached = false;
		SubscribeToAssetChanges();
		RefreshSpatialDelightingCacheStatus();
		RebuildProxyNormalCache();
		if (StableBindingState == EStableProxyBindingState::Uninitialized)
		{
			TryLoadStableProxyBindingCache();
		}
	}
}

void UGaussianSplatComponent::OnUnregister()
{
	UE_LOG(LogTemp, Warning,
		TEXT("[GSParamTrace] OnUnregister Component=%p Owner=%s CreationMethod=%d"),
		this, *GetNameSafe(GetOwner()), static_cast<int32>(CreationMethod));
#if WITH_EDITOR
	if (bEditorRealtimePropertyChangeActive)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GSParamTrace] Realtime property edit forced OnUnregister; call stack follows"));
		FDebug::DumpStackTraceToLog(ELogVerbosity::Warning);
	}
#endif
#if WITH_EDITOR
	if (LightPropertyChangedDelegateHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(LightPropertyChangedDelegateHandle);
		LightPropertyChangedDelegateHandle.Reset();
	}
#endif
	UnsubscribeFromAssetChanges();
	Super::OnUnregister();
}

void UGaussianSplatComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	const FGaussianSplatLightingData PreviousLighting = CachedLightingData;
	UpdateCachedSceneLighting();
	if (FMemory::Memcmp(&PreviousLighting, &CachedLightingData, sizeof(FGaussianSplatLightingData)) != 0)
	{
		MarkRenderDynamicDataDirty();
	}
	UpdateProxyShadowProbeDebug(DeltaTime);
}

void UGaussianSplatComponent::UpdateProxyShadowProbeDebug(float DeltaTime)
{
	constexpr float LightDebounceSeconds = 0.15f;
	constexpr float CandidateRadius = 6000.0f;
	constexpr float CameraRebuildDistance = 500.0f;
	constexpr int32 ProbeBudgetPerTick = 3000;

	if (CVarProxyShadowProbeDebug.GetValueOnGameThread() == 0 || !ProxyShadowMesh)
	{
		return;
	}

	UWorld* World = GetWorld();
	// Vertex classification is now an explicit Mesh Debug diagnostic only. Final
	// and Shadow Mask must never leave persistent debug points over the 3DGS output.
	const bool bCpuValidationMode =
		LightingPreviewMode == ELCCCinematicGSLightingPreviewMode::ProxyMeshDebug;
	if (!bCpuValidationMode)
	{
		// Non-reference preview modes do not display the shared validation points.
		if (World && (bProxyVertexDebugDrawn || bProxyShadowProbeWorkActive))
		{
			FlushPersistentDebugLines(World);
		}
		bProxyVertexDebugDrawn = false;
		bProxyVertexShadowClassified = false;
		bProxyShadowProbeWorkActive = false;
		bProxyShadowProbeRebuildPending = true;
		ProxyShadowProbeDebugAccumulator = 0.0f;
		return;
	}

	const FStaticMeshRenderData* RenderData = ProxyShadowMesh->GetRenderData();
	if (!World || !RenderData || RenderData->LODResources.Num() == 0)
	{
		return;
	}

	UDirectionalLightComponent* MainDirectional = nullptr;
	USkyLightComponent* MainSky = nullptr;
	for (TObjectIterator<ULightComponent> It; It; ++It)
	{
		ULightComponent* Light = *It;
		if (!Light || Light->GetWorld() != World || !Light->IsRegistered() || !Light->IsVisible())
		{
			continue;
		}
		if (UDirectionalLightComponent* Directional = Cast<UDirectionalLightComponent>(Light))
		{
			const float CandidateScore = ComputeGSDirectionalAuthorityScore(Directional, this);
			const float CurrentScore = MainDirectional
				? ComputeGSDirectionalAuthorityScore(MainDirectional, this)
				: -1.0e9f;
			if (!MainDirectional || CandidateScore > CurrentScore)
			{
				MainDirectional = Directional;
			}
		}
	}
	for (TObjectIterator<USkyLightComponent> It; It; ++It)
	{
		USkyLightComponent* Sky = *It;
		if (Sky && Sky->GetWorld() == World && Sky->IsRegistered() && Sky->IsVisible() &&
			(!MainSky || Sky->Intensity > MainSky->Intensity))
		{
			MainSky = Sky;
		}
	}

	if (!MainDirectional)
	{
		return;
	}

	// v2800: proxy shadow probes use the same explicit celestial authority as the
	// renderer and LightDepth, rather than re-reading a selected light component.
	const FVector CurrentSurfaceToLight =
		(bEnableAutomaticDayNightCycle && bRuntimeStableShadowDirectionValid)
		? RuntimeStableShadowDirection.GetSafeNormal()
		: (-MainDirectional->GetForwardVector()).GetSafeNormal();
	if (!bProxyShadowLightDirectionValid)
	{
		ProxyShadowLightDirection = CurrentSurfaceToLight;
		bProxyShadowLightDirectionValid = true;
	}
	else if (!ProxyShadowLightDirection.Equals(CurrentSurfaceToLight, 0.0001f))
	{
		ProxyShadowLightDirection = CurrentSurfaceToLight;
		ProxyShadowProbeDebugAccumulator = 0.0f;
		bProxyShadowProbeRebuildPending = true;
		bProxyShadowProbeWorkActive = false;
		return;
	}

	// Debounce interactive light rotation before replacing the current work queue.
	ProxyShadowProbeDebugAccumulator += DeltaTime;
	if (bProxyShadowProbeRebuildPending && ProxyShadowProbeDebugAccumulator < LightDebounceSeconds)
	{
		return;
	}

	const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
	const uint32 VertexCount = Positions.GetNumVertices();
	const uint32 IndexCount = LOD.IndexBuffer.GetNumIndices();
	if (VertexCount == 0 || IndexCount < 3)
	{
		return;
	}

	FVector CameraLocation = GetComponentLocation();
	bool bHasCamera = false;
	if (APlayerController* PlayerController = World->GetFirstPlayerController())
	{
		if (PlayerController->PlayerCameraManager)
		{
			CameraLocation = PlayerController->PlayerCameraManager->GetCameraLocation();
			bHasCamera = true;
		}
	}
#if WITH_EDITOR
	if (!bHasCamera && GEditor && GEditor->GetActiveViewport())
	{
		if (FEditorViewportClient* ViewportClient = static_cast<FEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient()))
		{
			CameraLocation = ViewportClient->GetViewLocation();
			bHasCamera = true;
		}
	}
#endif

	if (!bProxyShadowProbeSamplesBuilt)
	{
		const FTransform ProxyToWorld = GetProxyShadowMeshWorldTransform();
		const FStaticMeshVertexBuffer& VertexData = LOD.VertexBuffers.StaticMeshVertexBuffer;
		TMap<FVector3f, FVector3f> UniquePositionNormals;
		UniquePositionNormals.Reserve(VertexCount);
		for (uint32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
		{
			const FVector3f LocalPosition = Positions.VertexPosition(VertexIndex);
			if (!UniquePositionNormals.Contains(LocalPosition))
			{
				FVector3f LocalNormal(0.0f, 0.0f, 1.0f);
				if (VertexIndex < VertexData.GetNumVertices())
				{
					const FVector4f TangentZ = VertexData.VertexTangentZ(VertexIndex);
					LocalNormal = FVector3f(TangentZ.X, TangentZ.Y, TangentZ.Z);
				}
				UniquePositionNormals.Add(LocalPosition, LocalNormal);
			}
		}
		ProxyShadowProbeWorldPositions.Reserve(UniquePositionNormals.Num());
		ProxyShadowProbeWorldNormals.Reserve(UniquePositionNormals.Num());
		ProxyShadowProbeColors.Init(FColor::Cyan, UniquePositionNormals.Num());
		for (const TPair<FVector3f, FVector3f>& Pair : UniquePositionNormals)
		{
			ProxyShadowProbeWorldPositions.Add(ProxyToWorld.TransformPosition((FVector)Pair.Key));
			ProxyShadowProbeWorldNormals.Add(ProxyToWorld.TransformVectorNoScale((FVector)Pair.Value).GetSafeNormal());
		}
		bProxyShadowProbeSamplesBuilt = true;
		bProxyShadowProbeRebuildPending = true;
	}

	if (bHasCamera && FVector::DistSquared(CameraLocation, ProxyShadowProbeCameraLocation) > FMath::Square(CameraRebuildDistance))
	{
		bProxyShadowProbeRebuildPending = true;
	}

	const bool bClassifyShadows = true;
	if (bProxyShadowProbeRebuildPending)
	{
		ProxyShadowProbeCandidates.Reset();
		const float RadiusSquared = FMath::Square(CandidateRadius);
		for (int32 SampleIndex = 0; SampleIndex < ProxyShadowProbeWorldPositions.Num(); ++SampleIndex)
		{
			if (!bHasCamera || FVector::DistSquared(CameraLocation, ProxyShadowProbeWorldPositions[SampleIndex]) <= RadiusSquared)
			{
				ProxyShadowProbeCandidates.Add(SampleIndex);
			}
		}
		ProxyShadowProbeCandidates.Sort([this, CameraLocation](int32 A, int32 B)
		{
			return FVector::DistSquared(CameraLocation, ProxyShadowProbeWorldPositions[A]) <
				FVector::DistSquared(CameraLocation, ProxyShadowProbeWorldPositions[B]);
		});
		ProxyShadowProbeCameraLocation = CameraLocation;
		ProxyShadowProbeWorkCursor = 0;
		ProxyShadowProbeDirectLit = 0;
		ProxyShadowProbeSelfOrBackface = 0;
		ProxyShadowProbeExternalCast = 0;
		ProxyShadowProbeWorkStartSeconds = FPlatformTime::Seconds();
		bProxyShadowProbeWorkActive = true;
		bProxyShadowProbeRebuildPending = false;
		ProxyShadowProbeDebugAccumulator = 0.0f;
		FlushPersistentDebugLines(World);
		bProxyVertexDebugDrawn = false;
		UE_LOG(LogTemp, Display, TEXT("GS_PROXY_INCREMENTAL_BEGIN candidates=%d totalUnique=%d radius=%.0fcm budget=%d"),
			ProxyShadowProbeCandidates.Num(), ProxyShadowProbeWorldPositions.Num(), CandidateRadius, ProbeBudgetPerTick);
	}

	if (!bProxyShadowProbeWorkActive)
	{
		return;
	}

		UStaticMeshComponent* ProxyReceiver = nullptr;
		if (AGaussianSplatActor* SplatActor = Cast<AGaussianSplatActor>(GetOwner()))
		{
			ProxyReceiver = SplatActor->ProxyShadowReceiverComponent;
		}
		const FVector SurfaceToLight = ProxyShadowLightDirection;
		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(GaussianProxyVertexShadowClassify), true);
		QueryParams.bTraceComplex = true;
		QueryParams.bReturnPhysicalMaterial = false;
		const int32 WorkEnd = FMath::Min(ProxyShadowProbeWorkCursor + ProbeBudgetPerTick, ProxyShadowProbeCandidates.Num());
		for (; ProxyShadowProbeWorkCursor < WorkEnd; ++ProxyShadowProbeWorkCursor)
		{
			const int32 SampleIndex = ProxyShadowProbeCandidates[ProxyShadowProbeWorkCursor];
			const FVector& WorldPosition = ProxyShadowProbeWorldPositions[SampleIndex];
			FColor PointColor = FColor::Cyan;
			if (bClassifyShadows)
			{
				const FVector& WorldNormal = ProxyShadowProbeWorldNormals[SampleIndex];
				if (FVector::DotProduct(WorldNormal, SurfaceToLight) <= 0.001)
				{
					PointColor = FColor::Red;
					++ProxyShadowProbeSelfOrBackface;
				}
				else
				{
					FHitResult Hit;
					const FVector TraceStart = WorldPosition + SurfaceToLight * 5.0;
					const FVector TraceEnd = TraceStart + SurfaceToLight * 200000.0;
					if (World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, QueryParams))
					{
						if (Hit.GetComponent() == ProxyReceiver)
						{
							PointColor = FColor::Red;
							++ProxyShadowProbeSelfOrBackface;
						}
						else
						{
							PointColor = FColor::Blue;
							++ProxyShadowProbeExternalCast;
						}
					}
					else
					{
						++ProxyShadowProbeDirectLit;
					}
				}
			}
			ProxyShadowProbeColors[SampleIndex] = PointColor;
			const bool bMaskPreview = LightingPreviewMode == ELCCCinematicGSLightingPreviewMode::ShadowMask;
			const FColor DrawColor = bMaskPreview
				? (PointColor == FColor::Cyan ? FColor::Black : FColor::White)
				: PointColor;
			if (!bMaskPreview)
			{
				DrawDebugPoint(
					World,
					WorldPosition,
					4.0f,
					DrawColor,
					false,
					3600.0f,
					0);
			}
		}

		if (ProxyShadowProbeWorkCursor >= ProxyShadowProbeCandidates.Num())
		{
			bProxyShadowProbeWorkActive = false;
			bProxyVertexDebugDrawn = true;
			bProxyVertexShadowClassified |= bClassifyShadows;
			UE_LOG(LogTemp, Display,
				TEXT("GS_PROXY_INCREMENTAL_DONE renderVertices=%u uniquePositionVertices=%d candidates=%d triangles=%u classified=%d directLit=%u selfOrBackface=%u externalCast=%u elapsedMs=%.1f"),
				VertexCount, ProxyShadowProbeWorldPositions.Num(), ProxyShadowProbeCandidates.Num(), IndexCount / 3,
				bClassifyShadows ? 1 : 0, ProxyShadowProbeDirectLit, ProxyShadowProbeSelfOrBackface,
				ProxyShadowProbeExternalCast, (FPlatformTime::Seconds() - ProxyShadowProbeWorkStartSeconds) * 1000.0);
		}
	}

#if WITH_EDITOR
void UGaussianSplatComponent::OnAnyObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent)
{
	ULightComponent* ChangedLight = Cast<ULightComponent>(Object);
	if (!ChangedLight || ChangedLight->GetWorld() != GetWorld())
	{
		return;
	}

	UpdateCachedSceneLighting();
	MarkRenderDynamicDataDirty();
}
#endif

void UGaussianSplatComponent::SendRenderDynamicData_Concurrent()
{
	Super::SendRenderDynamicData_Concurrent();

	if (!SceneProxy)
	{
		return;
	}

	FGaussianSplatSceneProxy* SplatProxy = static_cast<FGaussianSplatSceneProxy*>(SceneProxy);
	FGaussianSplatLightingData LightingCopy = CachedLightingData;
	ENQUEUE_RENDER_COMMAND(UpdateGaussianSplatLightingData)(
		[SplatProxy, LightingCopy](FRHICommandListImmediate& RHICmdList)
		{
			if (SplatProxy)
			{
				SplatProxy->UpdateLightingData_RenderThread(LightingCopy);
			}
		});

	if (LastSubmittedStableBindingRevision != StableBindingRevision)
	{
		TSharedPtr<const TArray<FStableProxySplatBindingGPU>, ESPMode::ThreadSafe> BindingPayload = StableSplatBindings;
		TArray<FStableProxyVertexGPU> VertexCopy = StableProxyVertices;
		TArray<FStableProxyTriangleGPU> TriangleCopy = StableProxyTriangles;
		TArray<FVector4f> TriangleNormalCopy = StableProxyTriangleVertexNormals;
		const uint32 BindingRevisionCopy = StableBindingRevision;
		LastSubmittedStableBindingRevision = StableBindingRevision;
		ENQUEUE_RENDER_COMMAND(UpdateGaussianStableSplatBindings)(
			[SplatProxy, BindingPayload = MoveTemp(BindingPayload), VertexCopy = MoveTemp(VertexCopy), TriangleCopy = MoveTemp(TriangleCopy), TriangleNormalCopy = MoveTemp(TriangleNormalCopy), BindingRevisionCopy](FRHICommandListImmediate& RHICmdList)
			{
				if (SplatProxy)
				{
					static const TArray<FStableProxySplatBindingGPU> EmptyBindings;
					SplatProxy->UpdateStableSplatBindings_RenderThread(RHICmdList, BindingPayload.IsValid() ? *BindingPayload : EmptyBindings, VertexCopy, TriangleCopy, TriangleNormalCopy, BindingRevisionCopy);
				}
			});
	}

	if (LastSubmittedStableFieldRequestRevision != StableFieldRequestRevision)
	{
		if (StableProxyRayProofs.Num() == StableProxyVertices.Num() && StableRayProofRevision != 0u)
		{
			// CPU BVH calculation completes before this immutable upload. The render
			// thread never sees a partial field or a previous light direction.
		}
		else
		{
		const uint32 RequestRevisionCopy = StableFieldRequestRevision;
		const uint32 ExpectedLightDepthRevisionCopy = StableFieldExpectedLightDepthRevision;
		const float DepthBiasCopy = StableFieldDepthBiasCm;
		LastSubmittedStableFieldRequestRevision = StableFieldRequestRevision;
		ENQUEUE_RENDER_COMMAND(RequestGaussianStableProxyField)(
			[SplatProxy, RequestRevisionCopy, ExpectedLightDepthRevisionCopy, DepthBiasCopy](FRHICommandListImmediate&)
			{
				if (SplatProxy)
				{
					SplatProxy->RequestStableProxyFieldBuild_RenderThread(RequestRevisionCopy, ExpectedLightDepthRevisionCopy, DepthBiasCopy);
				}
			});
		}
	}

	if (LastSubmittedStableRayProofRevision != StableRayProofRevision &&
		StableRayProofRevision != 0u && StableProxyRayProofs.Num() == StableProxyVertices.Num())
	{
		TArray<FVector4f> ProofCopy = StableProxyRayProofs;
		const uint32 RayRevisionCopy = StableRayProofRevision;
		LastSubmittedStableRayProofRevision = StableRayProofRevision;
		LastSubmittedStableFieldRequestRevision = StableFieldRequestRevision;
		ENQUEUE_RENDER_COMMAND(UpdateGaussianStableProxyRayProofs)(
			[SplatProxy, ProofCopy = MoveTemp(ProofCopy), RayRevisionCopy](FRHICommandListImmediate& RHICmdList)
			{
				if (SplatProxy)
				{
					SplatProxy->UpdateStableProxyRayProofs_RenderThread(RHICmdList, ProofCopy, RayRevisionCopy);
				}
			});
	}

}

FPrimitiveSceneProxy* UGaussianSplatComponent::CreateSceneProxy()
{
	if (!SplatAsset || !SplatAsset->IsValid())
	{
		return nullptr;
	}

	// Don't create proxy for preview worlds (Blueprint editor, etc.)
	UWorld* World = GetWorld();
	if (World)
	{
		EWorldType::Type WorldType = World->WorldType;
		if (WorldType == EWorldType::EditorPreview || WorldType == EWorldType::GamePreview)
		{
			return nullptr;
		}
	}

	UpdateCachedSceneLighting();

	const bool bTracePanoramaProxy = CVarGSPanoramaShadowTrace.GetValueOnGameThread() > 0;
	if (bTracePanoramaProxy)
	{
		uint32 DirectionalLights = 0u;
		for (uint32 I = 0u; I < CachedLightingData.LightCount; ++I)
		{
			DirectionalLights += FMath::Abs(CachedLightingData.LightDirectionType[I].W) < 0.25f ? 1u : 0u;
		}
		const bool bProofReady = StableRayProofRevision != 0u &&
			StableProxyRayProofs.Num() == StableProxyVertices.Num() && !StableProxyVertices.IsEmpty();
		UE_LOG(LogTemp, Log,
			TEXT("[GSPanoramaTrace][GT][PreProxy] Owner=%s World=%s Scene=%p WorldType=%d GameWorld=%d BindingState=%d BindingRev=%u BindingHash=%016llX Bindings=%d Vertices=%d Triangles=%d RayRev=%u Proofs=%d BVH=%d Lights=%u DirLights=%u ProofReady=%d Cache=%s Frame=%llu"),
			*GetNameSafe(GetOwner()), *GetNameSafe(World), World ? static_cast<const void*>(World->Scene) : nullptr,
			World ? static_cast<int32>(World->WorldType) : -1, World && World->IsGameWorld() ? 1 : 0,
			static_cast<int32>(StableBindingState), StableBindingRevision,
			static_cast<unsigned long long>(StableBindingHash),
			StableSplatBindings.IsValid() ? StableSplatBindings->Num() : 0,
			StableProxyVertices.Num(), StableProxyTriangles.Num(), StableRayProofRevision,
			StableProxyRayProofs.Num(), StableRayBVHContext.IsValid() ? 1 : 0,
			CachedLightingData.LightCount, DirectionalLights, bProofReady ? 1 : 0,
			*StableBindingCacheStatus, static_cast<unsigned long long>(GFrameCounter));
	}

	// Panoramic MRQ submits every pane immediately after the duplicated PIE world
	// creates its render proxies.  Stable ray proofs are intentionally transient,
	// so the duplicated component can have a valid persistent binding but no proof
	// payload yet.  Waiting for Actor::Tick in that case lets the first panorama
	// panes create an unshadowed SceneProxy (RayRev=0 / Proofs=0).
	//
	// Bootstrap only the missing render payload, synchronously, at the last
	// game-thread point before the new SceneProxy snapshots component data.  This
	// uses the existing deterministic BVH/cache path unchanged; it does not change
	// shadow classification, filtering, strength, or any editor-world behaviour.
	const bool bNeedsGameWorldStableRayBootstrap =
		World && World->IsGameWorld() &&
		StableBindingState == EStableProxyBindingState::Ready &&
		!StableProxyVertices.IsEmpty() &&
		(StableRayProofRevision == 0u ||
		 StableProxyRayProofs.Num() != StableProxyVertices.Num());
	if (bNeedsGameWorldStableRayBootstrap)
	{
		UE_LOG(LogTemp, Log,
			TEXT("[GSPanoramaBootstrap] Building missing StableRay before SceneProxy creation BindingRev=%u Vertices=%d"),
			StableBindingRevision, StableProxyVertices.Num());
        // This new proxy snapshots the rebuilt proofs below. Scheduling another
        // end-of-frame component update from CreateSceneProxy is illegal while
        // UWorld is already sending end-of-frame updates (bPostTickComponentUpdate).
        RebuildStableProxyRayVisibilityInternal(false);
        UE_LOG(LogTemp,Display,TEXT("[GSStableBootstrap] NewProxySnapshot=1 RecursiveDirtySuppressed=1 NormalTickNotificationUnchanged=1"));
		const bool bStableRayReady =
			StableRayProofRevision != 0u && StableProxyRayProofs.Num() == StableProxyVertices.Num();
		if (bStableRayReady)
		{
			UE_LOG(LogTemp, Log,
				TEXT("[GSPanoramaBootstrap] Pre-SceneProxy StableRay Ready=1 RayRev=%u Proofs=%d Vertices=%d"),
				StableRayProofRevision, StableProxyRayProofs.Num(), StableProxyVertices.Num());
		}
		else
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[GSPanoramaBootstrap] Pre-SceneProxy StableRay Ready=0 RayRev=%u Proofs=%d Vertices=%d"),
				StableRayProofRevision, StableProxyRayProofs.Num(), StableProxyVertices.Num());
		}
	}
	if (bTracePanoramaProxy)
	{
		const bool bProofReady = StableRayProofRevision != 0u &&
			StableProxyRayProofs.Num() == StableProxyVertices.Num() && !StableProxyVertices.IsEmpty();
		UE_LOG(LogTemp, Log,
			TEXT("[GSPanoramaTrace][GT][PostBootstrap] Owner=%s Scene=%p BindingState=%d RayRev=%u Proofs=%d Vertices=%d Ready=%d"),
			*GetNameSafe(GetOwner()), World ? static_cast<const void*>(World->Scene) : nullptr,
			static_cast<int32>(StableBindingState), StableRayProofRevision,
			StableProxyRayProofs.Num(), StableProxyVertices.Num(), bProofReady ? 1 : 0);
	}
	return new FGaussianSplatSceneProxy(this);
}

FBoxSphereBounds UGaussianSplatComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (SplatAsset && SplatAsset->IsValid())
	{
		FBox LocalBox = SplatAsset->GetBounds();

		// Transform to world space
		FBox WorldBox = LocalBox.TransformBy(LocalToWorld);

		return FBoxSphereBounds(WorldBox);
	}

	// Return small default bounds if no asset
	return FBoxSphereBounds(FVector::ZeroVector, FVector(100.0f), 100.0f);
}

void UGaussianSplatComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	// Gaussian splatting doesn't use traditional materials
	// But we might add a material for composite pass later
}

void UGaussianSplatComponent::RefreshPuddleDampenRenderState()
{
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::SetSplatAsset(UGaussianSplatAsset* NewAsset)
{
	if (SplatAsset != NewAsset)
	{
		// Unsubscribe from old asset
		UnsubscribeFromAssetChanges();

		SplatAsset = NewAsset;

		// Subscribe to new asset
		if (IsRegistered())
		{
			SubscribeToAssetChanges();
		}

		OnAssetChanged();
	}
}

int32 UGaussianSplatComponent::GetSplatCount() const
{
	return SplatAsset ? SplatAsset->GetSplatCount() : 0;
}


void UGaussianSplatComponent::UpdateCachedSceneLighting()
{
	FGaussianSplatLightingData NewLighting;
	NewLighting.Reset();

	const bool bApplyEffects = bEnableCinematicEffects;
	NewLighting.bReceiveLighting = (bApplyEffects && bReceiveUnrealLights) ? 1u : 0u;

	// Fog vQualityPreserve: old level instances may have serialized the previous
	// Fog preset (-0.34 EV / 0.68 chroma / 0.10 sharpness).  Do not require the
	// user to re-apply a preset: while Fog is active, publish a neutral GS grade
	// baseline and let only the late volumetric bridge change atmospheric radiance.
	// Other weather modes and all shadow/lighting controls remain untouched.
	const bool bFogPreserveSourceQuality = WeatherMode == ELCCCinematicGSWeatherMode::Fog;
	const float RuntimeTargetExposureEV = bFogPreserveSourceQuality ? -0.08f : TargetExposureEV;
	const float RuntimeTargetChromaRecovery = bFogPreserveSourceQuality ? 1.05f : TargetChromaRecovery;
	const float RuntimeTargetSplatSharpness = bFogPreserveSourceQuality ? 0.42f : TargetSplatSharpness;
	const float RuntimeTargetMidtoneContrast = bFogPreserveSourceQuality ? 1.36f : TargetMidtoneContrast;
	// Keep the bridge subtle even when old level instances have aggressive serialized values.
	// The splat color is already baked; this pass should guide luminance, not recolor the scan.
	NewLighting.LightingInfluence = FMath::Clamp(UnrealLightInfluence, 0.0f, 4.0f);
	NewLighting.DirectionalResponse = FMath::Clamp(UnrealDirectionalResponse, 0.0f, 1.0f);
	NewLighting.LightWrap = FMath::Clamp(UnrealLightWrap, 0.0f, 1.0f);
	NewLighting.NormalMode = static_cast<uint32>(UnrealLightingNormalMode);
	NewLighting.ProxyNormalParams = FVector4f(
		(bEnableProxySurfaceNormals && ProxyNormalCacheStatus == ELCCCinematicGSProxyNormalCacheStatus::Valid) ? 1.0f : 0.0f,
		FMath::Clamp(ProxyNormalStrength, 0.0f, 1.0f),
		FMath::Clamp(ProxyNormalConfidenceThreshold, 0.0f, 1.0f),
		FMath::Clamp(FoliageProxyNormalStrength, 0.0f, 1.0f));
	const bool bUseProxyLightingPath = ProxyShadowMesh != nullptr;
	const bool bUseApproximateShadowsForLighting = bEnableApproximateShadows && !bUseProxyLightingPath;
	// Respect the authored ambient value over the complete Details-panel range.
	// Older builds silently capped this to 0.08/0.18 when relighting was disabled,
	// which made most of the visible slider range appear broken.
	const float NightBlend = bEnableAutomaticDayNightCycle ? FMath::Clamp(RuntimeNightBlend, 0.0f, 1.0f) : 0.0f;
	const bool bUnifiedTOD = bEnableAutomaticDayNightCycle && bUseUnifiedPhysicalTimeOfDay;
	const float UnifiedTODDirectEnergy = bUnifiedTOD ? FMath::Clamp(RuntimeUnifiedTODDirectEnergy, 0.0f, 1.5f) : 1.0f;
	const float UnifiedTODSkyEnergy = bUnifiedTOD ? FMath::Clamp(RuntimeUnifiedTODSkyEnergy, 0.0f, 1.0f) : 1.0f;
	const float UnifiedTODCarrierBrightness = bUnifiedTOD ? FMath::Clamp(RuntimeUnifiedTODCarrierBrightness, 0.20f, 1.0f) : 1.0f;
	const float UnifiedTODGamma = bUnifiedTOD ? FMath::Clamp(RuntimeUnifiedTODGamma, 0.75f, 1.60f) : 1.0f;
	const float UnifiedTODSaturation = bUnifiedTOD ? FMath::Clamp(RuntimeUnifiedTODSaturation, 0.35f, 1.20f) : 1.0f;
	const float UnifiedTODAfternoonLift = bUnifiedTOD ? FMath::Clamp(RuntimeUnifiedTODAfternoonLift, 0.0f, 0.8f) : 0.0f;
	const float UnifiedTODDirectResponse = bUnifiedTOD
		? FMath::Clamp(UnifiedTODDirectEnergy / FMath::Max(UnifiedTODPeakSunScale, 0.001f), 0.0f, 1.0f)
		: 1.0f;
	const float BrightDayStartDeg = FMath::Clamp(BrightDayExposureStartDegrees, 0.0f, 89.0f);
	const float BrightDayFullDeg = FMath::Clamp(BrightDayExposureFullDegrees, BrightDayStartDeg + 0.5f, 90.0f);
	const float BrightDayPhase = (!bUnifiedTOD && bEnableAutomaticDayNightCycle && bAutoProtectBrightDayExposure)
		? SmoothGSPhase(BrightDayStartDeg, BrightDayFullDeg, RuntimeSolarElevationDegrees)
		: 0.0f;
	// Energy and chroma do not share the same curve. The old 0.58 -> 0.985 energy
	// window compressed most ambient/indirect change into only a few degrees below the
	// horizon, so shadow contrast could collapse abruptly while time was moving smoothly.
	// Energy starts first; tone follows over a broad interval so dusk/blue-hour/night
	// evolve continuously instead of concentrating the visible change into a few degrees.
	const float NightEnergyPhase = bUnifiedTOD
		? (1.0f - UnifiedTODSkyEnergy)
		: SmoothGSPhase(0.16f, 0.985f, NightBlend);
	const float NightTonePhase = bUnifiedTOD
		? FMath::Clamp(RuntimeUnifiedTODGradeStrength, 0.0f, 1.0f)
		: SmoothGSPhase(0.12f, 0.995f, NightBlend);
	const float RuntimeNightEnergyScale = bEnableAutomaticDayNightCycle
		? FMath::Clamp(RuntimeNightEnvironmentEnergyScale, 0.45f, 1.25f)
		: 1.0f;
	const FLinearColor RuntimeNightColor = bEnableAutomaticDayNightCycle
		? RuntimeNightEnvironmentColor
		: NightAmbientColor;
	// v2799: use the normalized, direction-aware time-of-day colour authored by the
	// actor rather than always forcing the sunset orange into both dusk and dawn.
	const float PhaseToneStrength = bEnableAutomaticDayNightCycle
		? FMath::Clamp(RuntimeTimeOfDayGradeStrength * 0.86f, 0.0f, 1.0f)
		: 0.0f;
	const float SkySyncStrength =
		(!bUnifiedTOD && bEnableAutomaticDayNightCycle && bSync3DGSAmbientFromSkyLight)
		? FMath::Clamp(SkyLightColorSyncStrength, 0.0f, 1.0f)
		: 0.0f;
	const float EffectiveAmbientIntensity = bUnifiedTOD
		? FMath::Lerp(
			FMath::Clamp(NightAmbientIntensity, 0.0f, 1.0f),
			FMath::Clamp(UnrealAmbientIntensity, 0.0f, 2.0f),
			UnifiedTODSkyEnergy)
		: FMath::Lerp(
			FMath::Clamp(UnrealAmbientIntensity, 0.0f, 2.0f),
			FMath::Clamp(NightAmbientIntensity * RuntimeNightEnergyScale, 0.0f, 1.0f),
			NightEnergyPhase);
	const FLinearColor PhaseTimeColor = bEnableAutomaticDayNightCycle ? RuntimeTimeOfDayGradeColor : FLinearColor::White;
	const FLinearColor TwilightAmbient = FMath::Lerp(UnrealAmbientColor, PhaseTimeColor, PhaseToneStrength);
	const FLinearColor PhaseAmbientColor = FMath::Lerp(TwilightAmbient, RuntimeNightColor, NightTonePhase);
	const FLinearColor EffectiveAmbientColor = bUnifiedTOD
		? RuntimeUnifiedTODAmbientColor
		: FMath::Lerp(PhaseAmbientColor, RuntimeSkyAmbientColor, SkySyncStrength);
	const float EffectiveIndirectIntensity = FMath::Clamp(UnrealIndirectLightIntensity, 0.0f, 1.0f) *
		(bUnifiedTOD
			? FMath::Lerp(FMath::Clamp(NightSkyIndirectScale, 0.0f, 1.0f), 1.0f, UnifiedTODSkyEnergy)
			: FMath::Lerp(1.0f, FMath::Clamp(NightSkyIndirectScale * RuntimeNightEnergyScale, 0.0f, 1.0f), NightEnergyPhase));
	const float EffectiveGroundBounce = FMath::Clamp(UnrealGroundBounce, 0.0f, 1.0f) *
		(bUnifiedTOD
			? FMath::Lerp(0.18f, 1.0f, UnifiedTODSkyEnergy)
			: FMath::Lerp(1.0f, FMath::Clamp(0.28f * RuntimeNightEnergyScale, 0.12f, 0.35f), NightEnergyPhase));
	const FLinearColor EffectiveShadowTint = FMath::Lerp(
		UnrealShadowTint,
		EffectiveAmbientColor,
		0.65f * SkySyncStrength);
	NewLighting.AmbientColorIntensity = FVector4f(
		EffectiveAmbientColor.R,
		EffectiveAmbientColor.G,
		EffectiveAmbientColor.B,
		EffectiveAmbientIntensity);
	// v2779: proxy lighting no longer zeros the Final sky/ground terms. The old
	// camera-shadow path did that to avoid double-lighting, but Final now has one
	// Stable shadow authority. Keep the same indirect/ground controls alive so open
	// ground and back-facing surfaces track the UE-lit proxy instead of collapsing dark.
	NewLighting.IndirectLightParams = FVector4f(
		EffectiveIndirectIntensity,
		EffectiveGroundBounce,
		FMath::Clamp(UnrealShadowSaturation, 0.0f, 1.0f),
		bEnableUnrealShadowLighting ? 1.0f : 0.0f);
	NewLighting.IndirectShadowTint = FVector4f(
		FMath::Max(EffectiveShadowTint.R, 0.0f),
		FMath::Max(EffectiveShadowTint.G, 0.0f),
		FMath::Max(EffectiveShadowTint.B, 0.0f),
		0.0f);

	// v2783: one artist-facing density per shadow family. Closure Density changes
	// darkness only; Area/Softness own spatial extent and the user floor is absolute.
	const float UnifiedCastDensity = FMath::Clamp(CapturedShadowStrength, 0.0f, 2.0f);
	const float UnifiedClosureDensity = FMath::Clamp(ProxyLightingMatchStrength, 0.0f, 2.0f);
	const float ClosedMinimumBrightness = FMath::Clamp(StableClosedSkyFloor, 0.0f, 1.0f);

	NewLighting.StableLightingParams0 = FVector4f(
		UnifiedCastDensity,
		UnifiedClosureDensity,
		FMath::Clamp(CapturedClosedShadowAreaScale, 0.0f, 2.50f),
		FMath::Clamp(CapturedClosedShadowEdgeSoftness, 0.25f, 3.00f));
	NewLighting.StableLightingParams1 = FVector4f(
		ClosedMinimumBrightness,
		0.0f, // reserved: closure is a final/top lighting layer.
		FMath::Clamp(CapturedShadowSoftness, 0.0f, 1.0f),
		FMath::Clamp(CapturedShadowMinimum, 0.0f, 1.0f));
	NewLighting.StableLightingParams2 = FVector4f(
		FMath::Clamp(CapturedShadowBounceStrength, 0.0f, 1.0f),
		EffectiveIndirectIntensity,
		EffectiveGroundBounce,
		0.0f);
	// v2779: artist-facing sun controls are now consumed by the Stable proxy direct
	// path instead of the retired Gaussian-normal/camera-shadow authority.
	const bool bStableLightEffectsEnabled = bApplyEffects && bReceiveUnrealLights;
	NewLighting.StableLightingParams3 = FVector4f(
		bStableLightEffectsEnabled ? FMath::Clamp(TargetDirectScale, 0.0f, 3.0f) : 0.0f,
		bStableLightEffectsEnabled ? FMath::Clamp(UnrealLightInfluence, 0.0f, 4.0f) : 0.0f,
		bStableLightEffectsEnabled ? EffectiveAmbientIntensity : 0.0f,
		BrightDayPhase);
	// Params4.x carries the single day/night phase into Stable lighting so hidden
	// sky/ground/bounce floors cannot re-brighten the scene after the moon becomes active.
	// Params4.y remains the low-angle stability signal. v2820 caps its Final cast
	// visibility lift to +0.02 so it cannot erase a valid shadow near +/-2 degrees.
	float HorizonShadowStability = 1.0f;
	if (bEnableAutomaticDayNightCycle && bStabilizeHorizonShadows && bRuntimeStableShadowDirectionValid)
	{
		const float OuterDeg = FMath::Clamp(HorizonShadowStabilizationDegrees, 3.0f, 20.0f);
		const float InnerDeg = FMath::Min(2.0f, OuterDeg * 0.22f);
		const float InnerSin = FMath::Sin(FMath::DegreesToRadians(InnerDeg));
		const float OuterSin = FMath::Sin(FMath::DegreesToRadians(OuterDeg));
		HorizonShadowStability = SmoothGSPhase(
			InnerSin, FMath::Max(OuterSin, InnerSin + 0.001f),
			FMath::Abs((float)RuntimeStableShadowDirection.GetSafeNormal().Z));
	}
	const float StableNormalFullDistance = FMath::Max(StableLightingNormalFullDistanceCm, 0.0f);
	const float StableNormalMaxDistance = FMath::Max(StableLightingNormalMaxDistanceCm, StableNormalFullDistance + 0.1f);
	NewLighting.StableLightingParams4 = FVector4f(
		NightEnergyPhase, HorizonShadowStability, StableNormalFullDistance, StableNormalMaxDistance);
	const FBoxSphereBounds LightingBounds = Bounds.SphereRadius > 1.0f ? Bounds : CalcBounds(GetComponentTransform());
	const FVector BoundsOrigin = LightingBounds.Origin;
	const FVector BoundsExtent = LightingBounds.BoxExtent;
	const float HorizontalRadius = FMath::Max3((float)BoundsExtent.X, (float)BoundsExtent.Y, 1.0f);
	NewLighting.ShadowParams = FVector4f(
		FMath::Clamp(UnrealShadowStrength, 0.0f, 1.0f),
		FMath::Clamp(UnrealContactShadowStrength, 0.0f, 0.55f),
		FMath::Clamp(UnrealLightingContrast, 0.25f, 2.20f),
		FMath::Clamp(UnrealMicroNormalStrength, 0.0f, 0.55f));
	NewLighting.ShadowParams2 = FVector4f(
		FMath::Clamp(UnrealShadowSoftness, 0.0f, 1.0f),
		FMath::Clamp(UnrealShadowBreakup, 0.0f, 0.35f),
		(LightingPreviewMode == ELCCCinematicGSLightingPreviewMode::Final ||
		 LightingPreviewMode == ELCCCinematicGSLightingPreviewMode::ShadowMask)
			? -1.0f
			: (bUseApproximateShadowsForLighting ? 1.0f : 0.0f),
		0.0f);
	NewLighting.ShadowBounds = FVector4f((float)BoundsOrigin.X, (float)BoundsOrigin.Y, (float)BoundsOrigin.Z, HorizontalRadius);
	NewLighting.ShadowHeight = FVector4f(
		(float)(BoundsOrigin.Z - BoundsExtent.Z),
		(float)(BoundsOrigin.Z + BoundsExtent.Z),
		FMath::Clamp(ProxySelfShadowDepthBias, 0.0f, 500.0f),
		FMath::Clamp(ProxyExternalShadowDepthBias, 0.0f, 100.0f));
	NewLighting.RelightParams = FVector4f(
		(bApplyEffects && bEnableRelightingCore) ? 1.0f : 0.0f,
		FMath::Clamp(RelightStrength, 0.0f, 1.0f),
		FMath::Clamp(ApproxAlbedoStrength, 0.0f, 1.0f),
		FMath::Clamp(ApproxAlbedoShadowLift, 0.0f, 0.6f));
	NewLighting.RelightLookParams = FVector4f(
		FMath::Clamp(ApproxAlbedoHighlightReduction, 0.0f, 0.6f),
		bApplyEffects ? FMath::Clamp(TargetLookStrength, 0.0f, 1.0f) : 0.0f,
		FMath::Clamp(TargetGreenBoost, 0.0f, 0.5f),
		FMath::Clamp(RuntimeTargetMidtoneContrast, 0.25f, 4.0f));
	const FLinearColor TwilightDirectTint = FMath::Lerp(
		TargetSunTint,
		PhaseTimeColor,
		FMath::Clamp(RuntimeTimeOfDayGradeStrength * 0.50f, 0.0f, 1.0f));
	const FLinearColor EffectiveDirectTint = bUnifiedTOD
		? RuntimeUnifiedTODSunColor
		: FMath::Lerp(TwilightDirectTint, FMath::Lerp(MoonDirectionalColor, RuntimeNightColor, 0.22f), NightTonePhase);
	const FLinearColor TwilightSkyTint = FMath::Lerp(
		TargetSkyTint,
		PhaseTimeColor,
		FMath::Clamp(RuntimeTimeOfDayGradeStrength * 0.58f, 0.0f, 1.0f));
	const FLinearColor PhaseSkyTint = FMath::Lerp(TwilightSkyTint, RuntimeNightColor, NightTonePhase);
	const FLinearColor EffectiveSkyTint = bUnifiedTOD
		? RuntimeUnifiedTODAmbientColor
		: FMath::Lerp(PhaseSkyTint, RuntimeSkyAmbientColor, SkySyncStrength);
	const float BrightDayDirectScale = bUnifiedTOD ? 1.0f : FMath::Lerp(1.0f, 0.48f, BrightDayPhase);
	NewLighting.RelightSunTint = FVector4f(
		FMath::Max(EffectiveDirectTint.R, 0.0f),
		FMath::Max(EffectiveDirectTint.G, 0.0f),
		FMath::Max(EffectiveDirectTint.B, 0.0f),
		FMath::Clamp(TargetDirectScale, 0.0f, 3.0f) * BrightDayDirectScale);
	NewLighting.RelightSkyTint = FVector4f(
		FMath::Max(EffectiveSkyTint.R, 0.0f),
		FMath::Max(EffectiveSkyTint.G, 0.0f),
		FMath::Max(EffectiveSkyTint.B, 0.0f),
		FMath::Clamp(TargetShadowLift, 0.0f, 0.3f));
	const float BaseHighlightCompression = FMath::Clamp(TargetFilmicShoulderStrength, 0.0f, 1.0f);
	const float RuntimeHighlightCompression = bUnifiedTOD
		? FMath::Clamp(
			FMath::Lerp(FMath::Max(BaseHighlightCompression, 0.78f), FMath::Max(BaseHighlightCompression, 0.92f), UnifiedTODDirectResponse)
			+ UnifiedTODAfternoonLift * 0.14f, 0.0f, 1.0f)
		: FMath::Lerp(
			BaseHighlightCompression,
			FMath::Max(BaseHighlightCompression, 0.94f),
			BrightDayPhase);
	const float BaseBakedSunSuppression = FMath::Clamp(TargetBakedSunSuppression, 0.0f, 1.0f);
	const float RuntimeBakedSunSuppression = bUnifiedTOD
		? FMath::Clamp(
			FMath::Lerp(0.98f, FMath::Max(BaseBakedSunSuppression, 0.84f), UnifiedTODDirectResponse)
			+ UnifiedTODAfternoonLift * 0.20f, 0.0f, 0.99f)
		: FMath::Lerp(
			BaseBakedSunSuppression,
			FMath::Max(BaseBakedSunSuppression, 0.975f),
			BrightDayPhase);
	NewLighting.RelightDetailParams = FVector4f(
		RuntimeBakedSunSuppression,
		RuntimeHighlightCompression,
		FMath::Clamp(TargetBrightGreenCompression, 0.0f, 1.0f),
		FMath::Clamp(TargetMicroContrast, 0.0f, 1.0f));
	NewLighting.RelightDetailParams2 = FVector4f(
		FMath::Clamp(RuntimeTargetSplatSharpness, 0.0f, 1.0f),
		FMath::Clamp(PluginBloomSoftKnee, 0.0f, 1.0f),
		bApplyEffects ? 1.0f : 0.0f,
		FMath::Max(Original3DGSBrightness, 0.0f));
	// v2823 high-day protection is highlight-only inside the GS path. Do not stack a
	// second scene-wide EV reduction here; the previous extra EV made 09:00-12:00
	// darker than full night while low-angle sunrise remained bright.
	NewLighting.RelightToneParams = FVector4f(
		FMath::Clamp(RuntimeTargetExposureEV, -1.0f, 1.0f),
		FMath::Clamp(PluginColorBloomStrength, 0.0f, 4.0f),
		FMath::Clamp(RuntimeTargetChromaRecovery, 0.0f, 2.50f),
		FMath::Clamp(TargetShadowColorRetention, 0.0f, 1.0f));
	const float BaseShoulderStart = FMath::Clamp(TargetFilmicShoulderStart, 0.10f, 1.50f);
	const float RuntimeShoulderStart = bUnifiedTOD
		? FMath::Clamp(
			FMath::Lerp(FMath::Min(BaseShoulderStart, 0.38f), FMath::Min(BaseShoulderStart, 0.30f), UnifiedTODDirectResponse)
			- UnifiedTODAfternoonLift * 0.07f, 0.22f, 1.50f)
		: FMath::Lerp(
			BaseShoulderStart,
			FMath::Min(BaseShoulderStart, 0.30f),
			BrightDayPhase);
	NewLighting.RelightHighlightParams = FVector4f(
		RuntimeShoulderStart,
		FMath::Clamp(TargetGreenSaturationRecovery, 0.0f, 1.0f),
		FMath::Clamp(TargetGreenLuminanceReduction, 0.0f, 1.0f),
		FMath::Clamp(TargetCoarseContrast, 0.0f, 1.0f));
	NewLighting.RelightSharpenParams = FVector4f(
		FMath::Clamp(RuntimeTargetSplatSharpness, 0.0f, 1.0f),
		FMath::Clamp(TargetHighlightSharpenProtection, 0.0f, 1.0f),
		FMath::Clamp(TargetCoverageEdgeProtection, 0.0f, 1.0f),
		static_cast<float>(TargetCompositeDebugView));
	// v2799 DIRECTION-AWARE TWILIGHT GRADE. Sunset and dawn have independent grade
	// controls. The phase amount is broad and uses the actor's solar-motion state, so
	// sunrise is no longer just the sunset grade played backwards.
	const float PhaseGradePresence = FMath::Clamp(
		FMath::Max3(RuntimeGoldenHourBlend * 0.78f, RuntimeSunsetBlend, RuntimeBlueHourBlend * 0.62f),
		0.0f, 1.0f);
	const bool bUseDawnGrade = bEnableAutomaticDayNightCycle && bRuntimeSolarRising;
	const bool bPhaseGradeEnabled = bUseDawnGrade ? bEnableDawnColorGrade : bEnableTwilightColorGrade;
	const float AuthoredPhaseGradeStrength = bUseDawnGrade ? DawnColorGradeStrength : TwilightColorGradeStrength;
	const float TwilightGradePhase = bPhaseGradeEnabled
		? FMath::Clamp(PhaseGradePresence * FMath::Clamp(AuthoredPhaseGradeStrength, 0.0f, 1.0f), 0.0f, 1.0f)
		: 0.0f;
	const float TwilightGradeGamma = FMath::Clamp(bUseDawnGrade ? DawnGamma : TwilightGamma, 0.25f, 4.0f);
	const float TwilightGradeSaturation = FMath::Clamp(bUseDawnGrade ? DawnSaturation : TwilightSaturation, 0.0f, 3.0f);
	const float TwilightGradeBrightness = FMath::Clamp(bUseDawnGrade ? DawnBrightness : TwilightBrightness, 0.0f, 3.0f);

	const float NightGradePhase = (bEnableAutomaticDayNightCycle && bEnableNightColorGrade)
		? FMath::Clamp(SmoothGSPhase(0.10f, 0.985f, NightBlend) *
			FMath::Clamp(NightColorGradeStrength, 0.0f, 1.0f), 0.0f, 1.0f)
		: 0.0f;
	const float NightGradeGamma = FMath::Clamp(RuntimeNightGradeGamma, 0.25f, 4.0f);
	const float NightGradeSaturation = FMath::Clamp(RuntimeNightGradeSaturation, 0.0f, 3.0f);
	const float NightGradeBrightness = FMath::Clamp(RuntimeNightGradeBrightness, 0.0f, 3.0f);

	// v2823 do not darken high-day midtones through regional grade. Highlight shoulder
	// and the small post-process EV own clipping control; retain only a tiny chroma assist.
	const float DayProtectedSunlitGamma = FMath::Clamp(SunlitGamma, 0.25f, 4.0f);
	const float DayProtectedSunlitSaturation = FMath::Lerp(
		FMath::Clamp(SunlitSaturation, 0.0f, 3.0f),
		FMath::Max(FMath::Clamp(SunlitSaturation, 0.0f, 3.0f), 1.03f),
		BrightDayPhase);
	const float DayProtectedSunlitBrightness = FMath::Lerp(
		FMath::Clamp(SunlitBrightness, 0.0f, 3.0f),
		FMath::Min(FMath::Clamp(SunlitBrightness, 0.0f, 3.0f), 0.80f),
		BrightDayPhase);
	const float LitTwilightGamma = FMath::Lerp(DayProtectedSunlitGamma, TwilightGradeGamma, TwilightGradePhase);
	const float LitTwilightSaturation = FMath::Lerp(DayProtectedSunlitSaturation, TwilightGradeSaturation, TwilightGradePhase);
	const float LitTwilightBrightness = FMath::Lerp(DayProtectedSunlitBrightness, TwilightGradeBrightness, TwilightGradePhase);
	const float LitTwilightStrength = FMath::Lerp(FMath::Clamp(SunlitGradeStrength, 0.0f, 1.0f), 1.0f, TwilightGradePhase);

	const float DayProtectedShadowGamma = FMath::Clamp(ShadowAreaGamma, 0.25f, 4.0f);
	const float DayProtectedShadowSaturation = FMath::Lerp(
		FMath::Clamp(ShadowAreaSaturation, 0.0f, 3.0f),
		FMath::Max(FMath::Clamp(ShadowAreaSaturation, 0.0f, 3.0f), 1.02f),
		BrightDayPhase);
	const float DayProtectedShadowBrightness = FMath::Clamp(ShadowAreaBrightness, 0.0f, 3.0f);
	const float ShadowTwilightGamma = FMath::Lerp(DayProtectedShadowGamma, TwilightGradeGamma, TwilightGradePhase);
	const float ShadowTwilightSaturation = FMath::Lerp(DayProtectedShadowSaturation, TwilightGradeSaturation, TwilightGradePhase);
	const float ShadowTwilightBrightness = FMath::Lerp(DayProtectedShadowBrightness, TwilightGradeBrightness, TwilightGradePhase);
	const float ShadowTwilightStrength = FMath::Lerp(FMath::Clamp(ShadowGradeStrength, 0.0f, 1.0f), 1.0f, TwilightGradePhase);

	if (bUnifiedTOD)
	{
		const float UnifiedLitGamma = FMath::Clamp(FMath::Lerp(FMath::Clamp(SunlitGamma, 0.25f, 4.0f), UnifiedTODGamma, 0.90f), 0.25f, 4.0f);
		const float UnifiedShadowGamma = FMath::Clamp(FMath::Lerp(FMath::Clamp(ShadowAreaGamma, 0.25f, 4.0f), UnifiedTODGamma, 0.82f), 0.25f, 4.0f);
		const float UnifiedLitSaturation = FMath::Clamp(FMath::Clamp(SunlitSaturation, 0.0f, 3.0f) * UnifiedTODSaturation, 0.0f, 3.0f);
		const float UnifiedShadowSaturation = FMath::Clamp(FMath::Clamp(ShadowAreaSaturation, 0.0f, 3.0f) * UnifiedTODSaturation, 0.0f, 3.0f);
		const float UnifiedLitBrightness = FMath::Clamp(FMath::Clamp(SunlitBrightness, 0.0f, 3.0f) * UnifiedTODCarrierBrightness, 0.0f, 3.0f);
		const float UnifiedShadowBrightness = FMath::Clamp(FMath::Clamp(ShadowAreaBrightness, 0.0f, 3.0f) * UnifiedTODCarrierBrightness, 0.0f, 3.0f);
		NewLighting.RegionalColorGradeParams0 = FVector4f(
			UnifiedLitGamma, UnifiedLitSaturation, UnifiedLitBrightness, 1.0f);
		NewLighting.RegionalColorGradeParams1 = FVector4f(
			UnifiedShadowGamma, UnifiedShadowSaturation, UnifiedShadowBrightness, 1.0f);
	}
	else
	{
		NewLighting.RegionalColorGradeParams0 = FVector4f(
			FMath::Lerp(LitTwilightGamma, NightGradeGamma, NightGradePhase),
			FMath::Lerp(LitTwilightSaturation, NightGradeSaturation, NightGradePhase),
			FMath::Lerp(LitTwilightBrightness, NightGradeBrightness, NightGradePhase),
			FMath::Lerp(LitTwilightStrength, 1.0f, NightGradePhase));
		NewLighting.RegionalColorGradeParams1 = FVector4f(
			FMath::Lerp(ShadowTwilightGamma, NightGradeGamma, NightGradePhase),
			FMath::Lerp(ShadowTwilightSaturation, NightGradeSaturation, NightGradePhase),
			FMath::Lerp(ShadowTwilightBrightness, NightGradeBrightness, NightGradePhase),
			FMath::Lerp(ShadowTwilightStrength, 1.0f, NightGradePhase));
	}

	const FLinearColor TODTint = bUnifiedTOD
		? RuntimeUnifiedTODAmbientColor
		: (bEnableAutomaticDayNightCycle ? RuntimeTimeOfDayGradeColor : FLinearColor::White);
	NewLighting.TimeOfDayGradeTint = FVector4f(
		FMath::Max(TODTint.R, 0.0f), FMath::Max(TODTint.G, 0.0f), FMath::Max(TODTint.B, 0.0f),
		bUnifiedTOD
			? FMath::Clamp(RuntimeUnifiedTODGradeStrength, 0.0f, 1.0f)
			: (bEnableAutomaticDayNightCycle ? FMath::Clamp(RuntimeTimeOfDayGradeStrength, 0.0f, 1.0f) : 0.0f));

	const float BrightDaySunAccentScale = bUnifiedTOD
		? FMath::Lerp(0.55f, 1.0f, 1.0f - UnifiedTODDirectResponse)
		: FMath::Lerp(1.0f, 0.42f, BrightDayPhase);
	NewLighting.GaussianSunAccentParams0 = FVector4f(
		(bApplyEffects && bEnableGaussianSunAccent) ? 1.0f : 0.0f,
		FMath::Clamp(GaussianSunAccentStrength, 0.0f, 2.0f) * BrightDaySunAccentScale,
		FMath::Clamp(GaussianSunAccentStart, 0.0f, 1.0f),
		FMath::Max(FMath::Clamp(GaussianSunAccentFull, 0.01f, 1.0f), GaussianSunAccentStart + 0.001f));
	NewLighting.GaussianSunAccentParams1 = FVector4f(
		FMath::Clamp(GaussianSunAccentGamma, 0.25f, 4.0f),
		FMath::Clamp(GaussianSunAccentTintStrength, 0.0f, 1.0f),
		FMath::Clamp(GaussianSunAccentHighlightCompression, 0.0f, 2.0f),
		FMath::Clamp(GaussianSunAccentDetailRetention, 0.0f, 1.0f));
	NewLighting.GaussianSunAccentTint = FVector4f(
		FMath::Max(GaussianSunAccentTint.R, 0.0f),
		FMath::Max(GaussianSunAccentTint.G, 0.0f),
		FMath::Max(GaussianSunAccentTint.B, 0.0f), 0.0f);
	NewLighting.GaussianPlantTransmissionParams0 = FVector4f(
		(bApplyEffects && bEnableGaussianPlantTransmission) ? 1.0f : 0.0f,
		FMath::Clamp(GaussianPlantTransmissionStrength, 0.0f, 2.0f),
		FMath::Clamp(GaussianPlantBacklightStart, 0.0f, 1.0f),
		FMath::Max(FMath::Clamp(GaussianPlantBacklightFull, 0.01f, 1.0f), GaussianPlantBacklightStart + 0.001f));
	NewLighting.GaussianPlantTransmissionParams1 = FVector4f(
		FMath::Clamp(GaussianPlantTransmissionGamma, 0.25f, 4.0f),
		FMath::Clamp(GaussianPlantTransmissionTintStrength, 0.0f, 1.0f),
		FMath::Clamp(GaussianPlantTransmissionSaturation, 0.0f, 2.0f),
		FMath::Clamp(GaussianPlantViewPhasePower, 0.25f, 8.0f));
	NewLighting.GaussianPlantTransmissionTint = FVector4f(
		FMath::Max(GaussianPlantTransmissionTint.R, 0.0f),
		FMath::Max(GaussianPlantTransmissionTint.G, 0.0f),
		FMath::Max(GaussianPlantTransmissionTint.B, 0.0f), 0.0f);
	NewLighting.SpatialDelightingParams0 = FVector4f(
		(bApplyEffects && (bEnableSpatialDelighting || bEnableRelightingCore)) ? 1.0f : 0.0f,
		FMath::Clamp(SpatialDelightingStrength, 0.0f, 1.0f),
		FMath::Clamp(SpatialDelightingChromaStrength, 0.0f, 0.5f),
		FMath::Clamp(SpatialDelightingMinLighting, 0.25f, 1.0f));
	NewLighting.SpatialDelightingParams1 = FVector4f(
		FMath::Max(FMath::Clamp(SpatialDelightingMaxLighting, 1.0f, 3.0f), SpatialDelightingMinLighting),
		FMath::Clamp(SpatialDelightingDarkProtection, 0.0f, 1.0f),
		FMath::Clamp(SpatialDelightingMaxAlbedoLuminance, 0.25f, 3.0f),
		static_cast<float>(LightingPreviewMode));
	NewLighting.SpatialDelightingParams2 = FVector4f(
		FMath::Clamp(SpatialDelightingFoliageProtection, 0.0f, 1.0f),
		FMath::Clamp(SpatialDelightingMaxChromaScale, 1.0f, 2.0f),
		FMath::Clamp(SpatialDelightingDifferenceGain, 1.0f, 10.0f),
		0.0f); // Unused reserved lighting payload component
	NewLighting.SpatialDelightingParams3 = FVector4f(
		FMath::Clamp(SpatialDelightingEvidenceMinEV, 0.0f, 1.0f),
		FMath::Max(FMath::Clamp(SpatialDelightingEvidenceMaxEV, 0.05f, 2.0f), SpatialDelightingEvidenceMinEV + 0.001f),
		FMath::Clamp(SpatialDelightingMaxCorrectionWeight, 0.0f, 1.0f),
		FMath::Clamp(SpatialDelightingFoliageCorrectionScale, 0.0f, 1.0f));
	NewLighting.SpatialDelightingParams4 = FVector4f(
		FMath::Clamp(SpatialDelightingMaxShadowLift, 1.0f, 2.0f),
		FMath::Clamp(SpatialDelightingMinHighlightSuppression, 0.50f, 1.0f),
		FMath::Clamp(SpatialDelightingSignedDifferenceRangeEV, 0.25f, 4.0f),
		0.0f);
	const float WeatherFogBlend =
		WeatherMode == ELCCCinematicGSWeatherMode::Fog
			? FMath::Clamp(FogWeatherStrength, 0.0f, 1.0f)
			: 1.0f;
	// Fog vQualityPreserve: dense Fog is composited once, after the final GS grade.
	// Publishing zero legacy atmosphere strength here makes CalcViewData return the
	// original GS radiance immediately instead of applying shadow-crush, local
	// contrast, saturation fade and other old per-splat "fog look" operations first.
	// Clear/Dawn/Rain/Snow retain their existing atmosphere path byte-for-byte.
	// P46: cross-weather fog must not also run the old per-splat fog curve.
	// Other atmospheric colour grading remains unchanged when fog is disabled.
	const bool bP46NativeFogOverlay = FogWeatherStrength > 0.001f &&
		(WeatherMode == ELCCCinematicGSWeatherMode::Fog || bEnableFogOverlayInOtherWeather) &&
		NativeFogDensity > 0.000001f;
	const float LegacyAtmosphereStrength = (bFogPreserveSourceQuality || bP46NativeFogOverlay)
		? 0.0f
		: (bApplyEffects ? FMath::Clamp(AtmosphereStrength * WeatherFogBlend, 0.0f, 2.0f) : 0.0f);
	NewLighting.AtmosphereParams = FVector4f(
		LegacyAtmosphereStrength,
		FMath::Clamp(GroundFogDensity * WeatherFogBlend, 0.0f, 2.0f),
		FMath::Clamp(DistanceHaze * WeatherFogBlend, 0.0f, 2.0f),
		FMath::Clamp(WarmSunScatter, 0.0f, 2.0f));
	NewLighting.AtmosphereGradeParams = FVector4f(
		FMath::Clamp(CoolShadowTint, 0.0f, 1.0f),
		FMath::Clamp(ShadowCrush, 0.0f, 1.0f),
		FMath::Clamp(FogDensityVariation, 0.0f, 1.0f),
		FMath::Clamp(FogHeightFalloff, 0.1f, 6.0f));
	NewLighting.AtmosphereHighlightParams = FVector4f(
		FMath::Clamp(HighlightBloomBoost, 0.0f, 5.0f),
		FMath::Clamp(SunExposureBoost, 0.0f, 1.0f),
		FMath::Clamp(ShadowDesaturation, 0.0f, 1.0f),
		FMath::Clamp(WarmSunScatter, 0.0f, 2.0f));
	NewLighting.AtmosphereDistanceParams = FVector4f(
		FMath::Clamp(FarLightFade, 0.0f, 1.0f),
		FMath::Clamp(FarSaturationFade, 0.0f, 1.0f),
		FMath::Clamp(HighlightGlareScale, 0.0f, 3.0f),
		FMath::Clamp(ColorSaturationBoost, 0.0f, 2.0f));
	const float FogEndFromVisibility =
		WeatherMode == ELCCCinematicGSWeatherMode::Fog
			? FMath::Clamp(FogVisibilityDistance / FMath::Max(LightingBounds.SphereRadius, 500.0f), 0.01f, 2.0f)
			: FogDistanceEnd;
	NewLighting.AtmosphereFogShapeParams = FVector4f(
		FMath::Clamp(FogDistanceStart, 0.0f, 1.0f),
		FMath::Clamp(FogEndFromVisibility, 0.01f, 2.0f),
		FMath::Clamp(NearAirStart, 0.0f, 1.0f),
		FMath::Clamp(NearAirEnd, 0.01f, 2.0f));
	NewLighting.AtmosphereFogAmountParams = FVector4f(
		FMath::Clamp(DistanceFogWeight * WeatherFogBlend, 0.0f, 2.0f),
		FMath::Clamp(NearFogWeight * WeatherFogBlend, 0.0f, 2.0f),
		FMath::Clamp(HazeDensityMin, 0.0f, 2.0f),
		FMath::Clamp(HazeDensityMax, 0.0f, 2.0f));
	NewLighting.AtmosphereHighlightShapeParams = FVector4f(
		FMath::Clamp(HighlightLumaGateLow, 0.0f, 4.0f),
		FMath::Clamp(HighlightLumaGateHigh, 0.0f, 4.0f),
		FMath::Clamp(SunMaskGateLow, 0.0f, 1.0f),
		FMath::Clamp(SunMaskGateHigh, 0.0f, 1.0f));
	NewLighting.AtmosphereBloomParams = FVector4f(
		FMath::Clamp(LocalBloomMaskLow, 0.0f, 1.0f),
		FMath::Clamp(LocalBloomMaskHigh, 0.0f, 1.0f),
		FMath::Clamp(LocalBloomStrength, 0.0f, 8.0f),
		FMath::Clamp(LocalBloomGlareMix, 0.0f, 8.0f));
	NewLighting.CompositeBloomParams = FVector4f(
		FMath::Max(PluginBloomSpreadPixels, 0.0f),
		FMath::Max(PluginBloomScatter, 0.0f),
		CinematicBloomThreshold,
		PluginBloomSunColorInfluence);
	NewLighting.CompositeColorBloomParams = FVector4f(
		FMath::Clamp(PluginColorBloomStrength, 0.0f, 4.0f),
		FMath::Clamp(PluginColorBloomThreshold, 0.0f, 4.0f),
		FMath::Clamp(PluginColorBloomMinSaturation, 0.0f, 1.0f),
		0.15f);
	NewLighting.AtmosphereGlareParams = FVector4f(
		FMath::Clamp(LitAirNearWeight, 0.0f, 2.0f),
		FMath::Clamp(LitAirFarWeight, 0.0f, 2.0f),
		FMath::Clamp(HazeGlowStrength, 0.0f, 3.0f),
		FMath::Clamp(LitAirGlowStrength, 0.0f, 3.0f));
	NewLighting.AtmosphereShaftParams = FVector4f(
		FMath::Clamp(LightShaftBaseStrength, 0.0f, 3.0f),
		FMath::Clamp(LightShaftHighlightStrength, 0.0f, 3.0f),
		FMath::Clamp(SunLiftBase, 0.0f, 2.0f),
		FMath::Clamp(SunLiftDistance, 0.0f, 2.0f));
	NewLighting.SeasonalColorSelectParams = FVector4f(
		(bApplyEffects && bEnableSeasonalColor) ? 1.0f : 0.0f,
		FMath::Clamp(SeasonColorTolerance, 0.001f, 1.0f),
		FMath::Clamp(SeasonColorFeather, 0.001f, 1.0f),
		FMath::Clamp(SeasonMinSaturation, 0.0f, 1.0f));
	NewLighting.SeasonalColorAdjustParams = FVector4f(
		FMath::Clamp(SeasonSaturationScale, 0.0f, 2.5f),
		FMath::Clamp(SeasonBrightnessScale, 0.0f, 2.0f),
		FMath::Clamp(SeasonReplacementStrength, 0.0f, 1.0f),
		bPreviewSeasonColorMask ? 1.0f : 0.0f);
	NewLighting.SeasonalSourceColorFrom = FVector4f(
		FMath::Max(SeasonSourceColorFrom.R, 0.0f),
		FMath::Max(SeasonSourceColorFrom.G, 0.0f),
		FMath::Max(SeasonSourceColorFrom.B, 0.0f),
		FMath::Clamp(SeasonMinGreenDominance, -0.25f, 0.5f));
	NewLighting.SeasonalSourceColorTo = FVector4f(
		FMath::Max(SeasonSourceColorTo.R, 0.0f),
		FMath::Max(SeasonSourceColorTo.G, 0.0f),
		FMath::Max(SeasonSourceColorTo.B, 0.0f), 0.0f);
	NewLighting.SeasonalTargetColorFrom = FVector4f(
		FMath::Max(SeasonTargetColorFrom.R, 0.0f),
		FMath::Max(SeasonTargetColorFrom.G, 0.0f),
		FMath::Max(SeasonTargetColorFrom.B, 0.0f),
		FMath::Clamp(SeasonOriginalDetailPreservation, 0.0f, 1.0f));
	NewLighting.SeasonalTargetColorTo = FVector4f(
		FMath::Max(SeasonTargetColorTo.R, 0.0f),
		FMath::Max(SeasonTargetColorTo.G, 0.0f),
		FMath::Max(SeasonTargetColorTo.B, 0.0f),
		FMath::Clamp(SeasonTargetStartColorBias, 0.0f, 1.0f));

	if (!bReceiveUnrealLights || !bAutoCollectSceneLights)
	{
		CachedLightingData = NewLighting;
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		CachedLightingData = NewLighting;
		return;
	}

	struct FLightCandidate
	{
		ULightComponent* Light = nullptr;
		float Score = 0.0f;
		int32 Type = 1;
		float Radius = 0.0f;
	};

	const FVector Origin = Bounds.Origin.IsNearlyZero() ? GetComponentLocation() : Bounds.Origin;
	const float SearchRadius = FMath::Max(0.0f, LightSearchRadius);
	TArray<FLightCandidate> Candidates;

	for (TObjectIterator<ULightComponent> It; It; ++It)
	{
		ULightComponent* Light = *It;
		if (!Light || Light->GetWorld() != World || !Light->IsRegistered() || !Light->IsVisible())
		{
			continue;
		}

		FLightCandidate Candidate;
		Candidate.Light = Light;

		if (UDirectionalLightComponent* Directional = Cast<UDirectionalLightComponent>(Light))
		{
			Candidate.Type = 0;
			Candidate.Radius = 0.0f;
			Candidate.Score = -ComputeGSDirectionalAuthorityScore(Directional, this); // one day/night authority first
		}
		else if (ULocalLightComponent* LocalLight = Cast<ULocalLightComponent>(Light))
		{
			Candidate.Radius = FMath::Max(1.0f, LocalLight->AttenuationRadius);
			const float Dist = FVector::Dist(Origin, Light->GetComponentLocation());
			if (SearchRadius > 0.0f && Dist > SearchRadius + Candidate.Radius)
			{
				continue;
			}

			if (Cast<USpotLightComponent>(Light))
			{
				Candidate.Type = 2;
			}
			else if (Cast<URectLightComponent>(Light))
			{
				Candidate.Type = 3;
			}
			else
			{
				Candidate.Type = 1;
			}

			Candidate.Score = Dist / FMath::Max(1.0f, Light->Intensity);
		}
		else
		{
			continue;
		}

		Candidates.Add(Candidate);
	}

	Candidates.Sort([](const FLightCandidate& A, const FLightCandidate& B)
	{
		return A.Score < B.Score;
	});

	const int32 MaxLightsToCopy = FMath::Clamp(MaxUnrealLights, 1, FGaussianSplatLightingData::MaxLights - 2);
	bool bCopiedDayNightDirectional = false;
	for (const FLightCandidate& Candidate : Candidates)
	{
		if ((int32)NewLighting.LightCount >= MaxLightsToCopy)
		{
			break;
		}

		ULightComponent* Light = Candidate.Light;
		// Automatic day/night has one celestial authority. Do not send both the bright
		// authored sun and generated moon into the GS shader in the same frame.
		if (Candidate.Type == 0 && bEnableAutomaticDayNightCycle && bRuntimeActiveCelestialDirectionValid)
		{
			if (bCopiedDayNightDirectional)
			{
				continue;
			}
			bCopiedDayNightDirectional = true;
		}
		const int32 Index = (int32)NewLighting.LightCount;
		const FVector Pos = Light->GetComponentLocation();
		const FVector Forward = Light->GetComponentTransform().GetUnitAxis(EAxis::X).GetSafeNormal();
		FLinearColor Color = Light->GetLightColor();
		// Directional lights are authored in lux. Send a linear ratio to the splat
		// shader so UE meshes and 3DGS respond proportionally to the same main light.
		float RawIntensity = FMath::Max(0.0f, Light->Intensity);
		const float UserScale = FMath::Clamp(UnrealLightIntensityScale, 0.0f, 2.0f);
		float DirectionalPresence = Candidate.Type == 0
			? ComputeGSDirectionalHorizonPresence(Cast<UDirectionalLightComponent>(Light))
			: 1.0f;

		const bool bUseContinuousGSCelestial =
			Candidate.Type == 0 &&
			bEnableAutomaticDayNightCycle &&
			bRuntimeStableShadowDirectionValid &&
			bRuntimeContinuousCelestialStateValid;
		if (bUseContinuousGSCelestial)
		{
			Color = RuntimeContinuousCelestialColor;
			RawIntensity = FMath::Max(RuntimeContinuousCelestialIntensity, 0.0f);
			// The synthetic GS celestial energy is already blended by the broad day/night
			// envelope. Applying the selected physical Sun/Moon horizon gate again would
			// recreate the near-zero trough exactly where the authority changes.
			DirectionalPresence = 1.0f;
		}

		const float NormalizedIntensity = Candidate.Type == 0
			? FMath::Clamp((RawIntensity / 35.0f) * UserScale * DirectionalPresence, 0.0f, 8.0f)
			: FMath::Clamp(FMath::Sqrt(RawIntensity) * UserScale * 0.055f, 0.0f, 0.85f);

		NewLighting.LightPositionRadius[Index] = FVector4f((float)Pos.X, (float)Pos.Y, (float)Pos.Z, Candidate.Radius);

		if (Candidate.Type == 0)
		{
			// Use the same continuous upper-hemisphere direction already used by Stable
			// cast/LightDepth. The visual Sun/Moon can still swap at the horizon, but GS
			// N.L must not rotate 180 degrees on that single frame.
			const FVector SurfaceToLight =
				(bEnableAutomaticDayNightCycle && bRuntimeStableShadowDirectionValid)
				? RuntimeStableShadowDirection.GetSafeNormal()
				: (-Forward).GetSafeNormal();
			NewLighting.LightDirectionType[Index] = FVector4f((float)SurfaceToLight.X, (float)SurfaceToLight.Y, (float)SurfaceToLight.Z, 0.0f);
		}
		else
		{
			// Local/spot/rect lights: store emitted direction for spot falloff.
			NewLighting.LightDirectionType[Index] = FVector4f((float)Forward.X, (float)Forward.Y, (float)Forward.Z, (float)Candidate.Type);
		}

		NewLighting.LightColorIntensity[Index] = FVector4f(Color.R, Color.G, Color.B, NormalizedIntensity);

		if (USpotLightComponent* Spot = Cast<USpotLightComponent>(Light))
		{
			const float Inner = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Spot->InnerConeAngle, 0.0f, 89.0f)));
			const float Outer = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Spot->OuterConeAngle, 0.0f, 89.0f)));
			NewLighting.LightConeAngles[Index] = FVector4f(Inner, Outer, 0, 0);
		}

		NewLighting.LightCount++;
	}

	CachedLightingData = NewLighting;
}

bool UGaussianSplatComponent::HasCurrentSnowCoverageMask() const
{
	return SplatAsset && SnowCoverageMaskSourceAsset == SplatAsset &&
		SnowCoverageMaskSourceCount == SplatAsset->GetSplatCount() &&
		SnowCoveragePackedMask.Num() == (SnowCoverageMaskSourceCount + 15) / 16;
}

bool UGaussianSplatComponent::HasCurrentSnowBakedField2873() const
{
	// An older map without this baked payload uses the original GPU noise path.
	// Treat a moved/scaled source or a changed artist pattern as an invalid cache.
	return SplatAsset &&
		SnowBakedFieldSourceAsset2873 == SplatAsset &&
		SnowBakedFieldSplatCount2873 == SplatAsset->GetSplatCount() &&
		SnowBakedFieldPacked2873.Num() == (SnowBakedFieldSplatCount2873 + 3) / 4 &&
		SnowBakedFieldPatternCm2873 >= 50.0f &&
		FMath::IsNearlyEqual(SnowGaussianMoundScaleCm, SnowBakedFieldPatternCm2873, 0.005f) &&
		GetComponentTransform().Equals(SnowBakedFieldLocalToWorld2873, 0.001);
}

void UGaussianSplatComponent::RefreshSnowCoverageMaskRenderState()
{
	// Recreate only this component's render state so the new packed snow mask
	// is uploaded to the next SceneProxy (also used by panorama/MRQ views).
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::OnAssetChanged()
{
	bBoundsCached = false;
	UpdateBounds();
	RefreshSpatialDelightingCacheStatus();
	InvalidateStableProxyBindings(TEXT("SplatAssetChanged"));
	RebuildProxyNormalCache();
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::MarkRenderStateDirty()
{
	MarkRenderDynamicDataDirty();

	if (IsRegistered() && !bSuppressRenderStateRecreationForEditorTuning)
	{
		Super::MarkRenderStateDirty();
	}
}

void UGaussianSplatComponent::OnAssetDataChanged(UGaussianSplatAsset* ChangedAsset)
{
	// Only respond if this is our asset
	if (ChangedAsset == SplatAsset)
	{
		UE_LOG(LogTemp, Log, TEXT("GaussianSplat: Asset data changed (Nanite state), recreating scene proxy"));

		// Invalidate cached bounds since splat count may have changed
		bBoundsCached = false;
		UpdateBounds();
		RefreshSpatialDelightingCacheStatus();
		InvalidateStableProxyBindings(TEXT("SplatAssetDataChanged"));
		RebuildProxyNormalCache();

		// Recreate the scene proxy with updated asset data
		// This will cause CreateSceneProxy to be called again with the new Nanite state
		MarkRenderStateDirty();
	}
}

void UGaussianSplatComponent::SubscribeToAssetChanges()
{
	if (SplatAsset && !AssetChangedDelegateHandle.IsValid())
	{
		AssetChangedDelegateHandle = SplatAsset->OnAssetChanged.AddUObject(this, &UGaussianSplatComponent::OnAssetDataChanged);
		UE_LOG(LogTemp, Verbose, TEXT("GaussianSplat: Subscribed to asset change notifications"));
	}
}

void UGaussianSplatComponent::UnsubscribeFromAssetChanges()
{
	if (SplatAsset && AssetChangedDelegateHandle.IsValid())
	{
		SplatAsset->OnAssetChanged.Remove(AssetChangedDelegateHandle);
		AssetChangedDelegateHandle.Reset();
		UE_LOG(LogTemp, Verbose, TEXT("GaussianSplat: Unsubscribed from asset change notifications"));
	}
}
