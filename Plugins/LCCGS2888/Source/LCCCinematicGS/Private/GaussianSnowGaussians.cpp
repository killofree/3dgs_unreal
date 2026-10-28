// Copyright Epic Games, Inc. All Rights Reserved.
// v2868: supported optical bridge fill and finer sigma, keeping proxy-normal priority.
// v2859: source-first snow with frontal-view support volumetrics for floor/roof patches,
// reduced top-down-only anisotropy, and optional micro-cluster support to avoid thin plate-like splats.
// original-splat-only support (no hierarchy LOD/sky-scale evidence), and floor-safe proxy gating.
// Proxy provides coarse receiver/slope/sky eligibility; multiple ORIGINAL Gaussians
// determine world-space elevation, smoothed normal and fine relief orientation.
// The scene-wide baked mask and world-fixed generation remain camera independent.
// No camera or MRQ view is used in generation.
#include "GaussianSplatActor.h"
#include "GaussianSplatComponent.h"
#include "GaussianSplatAsset.h"
#include "GaussianClusterBuilder.h"
#include "Engine/StaticMesh.h"
#include "Math/RotationMatrix.h"
#include "StaticMeshResources.h"

namespace LCCSnowGaussian
{
	struct FTriangle
	{
		FVector A, B, C;
		FVector Normal;
		float Up = 0.0f;
		bool bValid = false;
		int32 Welded[3] = { INDEX_NONE, INDEX_NONE, INDEX_NONE };
	};

	// One bounded real source-GS covariance sample per occupied world voxel.
	// Never replace an authored scale by an arbitrary camera-facing billboard.
	struct FSourceShape
	{
		FVector WorldPosition = FVector::ZeroVector;
		FQuat Rotation = FQuat::Identity;
		FVector Scale = FVector::OneVector;
		double Planarity = 0.0;
	};

	// A compact, multilayer 3D index built from ORIGINAL splats, not proxy vertices.
	// Independent XYZ bins keep a roof and the ground directly underneath apart.
	// Every cell combines many measured surface positions/normals, rather than
	// retaining an arbitrary single Gaussian and copying its spatial noise.
	struct FSourceSurfaceCell
	{
		FVector PositionSum = FVector::ZeroVector;
		FVector NormalSum = FVector::ZeroVector;
		double WeightSum = 0.0;
		double VerifiedUpWeight = 0.0; // original individual covariance normals; do not trust an upward average of wall noise
		int32 Samples = 0;
	};

	struct FWeldVertex
	{
		FVector Position = FVector::ZeroVector;
		int32 PositionSamples = 0;
		int32 IncidentFaces = 0;
		double MinUp = 1.0;
		double MaxUp = 0.0;
		double MinNeighborZ = 1.0e30;
		double MaxNeighborZ = -1.0e30;
		FVector ProxyNormalSum = FVector::ZeroVector; // area-reweighted face normals
	};

	struct FProxyEdge
	{
		int32 A = INDEX_NONE, B = INDEX_NONE;
		int32 FirstFace = INDEX_NONE, SecondFace = INDEX_NONE;
		int32 NumFaces = 0;
		bool bTopLip = false;
		bool bCreviceShelf = false;
	};

	static FIntPoint XYCell(const FVector& P, double CellCm)
	{
		return FIntPoint(FMath::FloorToInt(P.X / CellCm), FMath::FloorToInt(P.Y / CellCm));
	}

	static FIntVector XYZCell(const FVector& P, double CellCm)
	{
		return FIntVector(
			FMath::FloorToInt(P.X / CellCm),
			FMath::FloorToInt(P.Y / CellCm),
			FMath::FloorToInt(P.Z / CellCm));
	}

	// Vertical ray / real triangle intersection (not a height-from-neighbor-cell guess).
	static bool HeightAtXY(const FTriangle& T, double X, double Y, double& OutZ)
	{
		const double Den = (T.B.Y - T.C.Y) * (T.A.X - T.C.X) +
			(T.C.X - T.B.X) * (T.A.Y - T.C.Y);
		if (FMath::Abs(Den) < 1.0e-7) return false;
		const double W0 = ((T.B.Y - T.C.Y) * (X - T.C.X) +
			(T.C.X - T.B.X) * (Y - T.C.Y)) / Den;
		const double W1 = ((T.C.Y - T.A.Y) * (X - T.C.X) +
			(T.A.X - T.C.X) * (Y - T.C.Y)) / Den;
		const double W2 = 1.0 - W0 - W1;
		if (W0 < -1.0e-7 || W1 < -1.0e-7 || W2 < -1.0e-7) return false;
		OutZ = T.A.Z * W0 + T.B.Z * W1 + T.C.Z * W2;
		return FMath::IsFinite(OutZ);
	}

	static uint32 CellHash(int32 X, int32 Y)
	{
		uint32 N = static_cast<uint32>(X) * 0x9e3779b9u ^
			static_cast<uint32>(Y) * 0x85ebca6bu ^ 0x51ed270bu;
		N ^= N >> 16u; N *= 0x7feb352du; N ^= N >> 15u;
		N *= 0x846ca68bu; N ^= N >> 16u;
		return N;
	}

	// Identical signed-integer hash + smooth value field is evaluated in
	// CalcViewData.usf. The SOURCE splat albedo and baked proxy relief share
	// the *same* continuous accumulation field and saved selection threshold.
	static double SnowValueNoise2D(double X, double Y)
	{
		const int32 IX = FMath::FloorToInt(X);
		const int32 IY = FMath::FloorToInt(Y);
		double FX = X - (double)IX;
		double FY = Y - (double)IY;
		FX = FX * FX * (3.0 - 2.0 * FX);
		FY = FY * FY * (3.0 - 2.0 * FY);
		const auto H = [](int32 A, int32 B) -> double
		{
			return double(CellHash(A, B) & 0x00ffffffu) / 16777215.0;
		};
		const double A = FMath::Lerp(H(IX, IY), H(IX + 1, IY), FX);
		const double B = FMath::Lerp(H(IX, IY + 1), H(IX + 1, IY + 1), FX);
		return FMath::Lerp(A, B, FY);
	}

	static double SnowAccumulationField(const FVector& P, double ScaleCm)
	{
		const double S = FMath::Max(50.0, ScaleCm);
		return FMath::Clamp(
			0.60 * SnowValueNoise2D(P.X / (S * 2.70), P.Y / (S * 2.70)) +
			0.30 * SnowValueNoise2D(P.X / S + 17.13, P.Y / S - 9.41) +
			0.10 * SnowValueNoise2D(P.X / (S * 0.46) - 31.7, P.Y / (S * 0.46) + 14.29),
			0.0, 1.0);
	}
}

void AGaussianSplatActor::DeleteSnowGaussians()
{
	if (!GaussianSplatComponent || !SnowGaussianComponent) return;
#if WITH_EDITOR
	Modify();
	GaussianSplatComponent->Modify();
	SnowGaussianComponent->Modify();
#endif
	// Release the old render proxy *before* dropping the actor-owned asset.
	SnowGaussianComponent->SetVisibility(false, false);
	SnowGaussianComponent->SetHiddenInGame(true);
	SnowGaussianComponent->SetSplatAsset(nullptr);
    BakedSnowGaussianAsset = nullptr;
    BakedSnowHeightStages2878.Reset();
    SnowBakeRollbackStages2879.Reset();
    SnowBakeRollbackAsset2879 = nullptr;
    SnowBakedMaximumHeightCm2878 = 0.0f;
    AppliedSnowHeightStage2878 = -1;
    SnowHeightBakeStatus2878 = TEXT("베이크 삭제됨");
    GaussianSplatComponent->bUseSnowGaussianLayer = false;
	GaussianSplatComponent->SnowCoveragePackedMask.Reset();
	GaussianSplatComponent->SnowBakedFieldPacked2873.Reset();
	GaussianSplatComponent->SnowBakedFieldSourceAsset2873 = nullptr;
	GaussianSplatComponent->SnowBakedFieldSplatCount2873 = 0;
	GaussianSplatComponent->SnowBakedFieldPatternCm2873 = 0.0f;
	GaussianSplatComponent->SnowBakedFieldSummary2873 = TEXT("적설 캐시 삭제됨. 새 눈 베이크에서 생성됩니다.");
	SnowGaussianComponent->SnowBakedFieldPacked2873.Reset();
	SnowGaussianComponent->SnowBakedFieldSourceAsset2873 = nullptr;
	SnowGaussianComponent->SnowBakedFieldSplatCount2873 = 0;
	SnowGaussianComponent->SnowBakedFieldPatternCm2873 = 0.0f;
	SnowGaussianComponent->SnowBakedFieldSummary2873.Empty();
	GaussianSplatComponent->SnowCoverageMaskSourceAsset = nullptr;
	GaussianSplatComponent->SnowCoverageMaskSourceCount = 0;
	GaussianSplatComponent->RefreshSnowCoverageMaskRenderState();
	GaussianSplatComponent->SnowGaussianGeneratedCount = 0;
	GaussianSplatComponent->SnowCliffCompactSummary2872.Empty();
	GaussianSplatComponent->SnowGaussianBuildStatus = TEXT("삭제됨 (원본 3DGS 유지)");
	GaussianSplatComponent->SnowResampleLastBuild = TEXT("눈 삭제됨");
	GaussianSplatComponent->SnowResampleReceiverCells = 0;
	GaussianSplatComponent->SnowResampleGenerated = 0;
	GaussianSplatComponent->SnowResampleRefillPlaced = 0;
	GaussianSplatComponent->SnowResampleUnmetCenters = 0;
	GaussianSplatComponent->SnowResampleUnmetFine = 0;
	GaussianSplatComponent->SnowResampleOpticalUnmetCenters = 0;
	GaussianSplatComponent->SnowResampleGeometricUnmetCenters = 0;
	GaussianSplatComponent->SnowResampleQuadSites = 0;
	GaussianSplatComponent->SnowResampleUnmetQuadSites = 0;
	GaussianSplatComponent->SnowResampleUnusedSurfaceSlots = 0;
	GaussianSplatComponent->SnowResampleRejectedSupport = 0;
	GaussianSplatComponent->SnowCohesionBridgeCandidates = 0;
	GaussianSplatComponent->SnowFineCandidates2869 = 0;
	GaussianSplatComponent->SnowFinePlaced2869 = 0;
	GaussianSplatComponent->SnowFineUnmet2869 = 0;
	GaussianSplatComponent->SnowFinePasses2869 = 0;
	GaussianSplatComponent->SnowOpticalFinishChanged2870 = 0;
	GaussianSplatComponent->SnowOpticalFinishCandidateSites2870 = 0;
	GaussianSplatComponent->SnowOpticalFinishUnmetBefore2870 = 0;
	GaussianSplatComponent->SnowOpticalFinishUnmetAfter2870 = 0;
	GaussianSplatComponent->SnowOpticalUnmetFloor2870 = 0;
	GaussianSplatComponent->SnowOpticalUnmetRoof2870 = 0;
	GaussianSplatComponent->SnowOpticalUnmetStone2870 = 0;
	GaussianSplatComponent->SnowCohesionBridgePlaced = 0;
	GaussianSplatComponent->SnowResampleTotalGenerated = 0;
	GaussianSplatComponent->SnowResampleRefillCandidateSites = 0;
	GaussianSplatComponent->SnowResampleMeasuredRounds = 0;
	GaussianSplatComponent->SnowResampleMeasuredRequiredCount = -1;
	GaussianSplatComponent->SnowResampleStopReason = TEXT("눈 삭제됨");
	GaussianSplatComponent->SnowProxyNormalApplied = 0;
	GaussianSplatComponent->SnowProxyNormalSoftFloor = 0;
	GaussianSplatComponent->SnowProxyNormalMissing = 0;
	GaussianSplatComponent->SnowProxyNormalInvalid = 0;
	GaussianSplatComponent->SnowProxyNormalSourceGateRejected = 0;
	GaussianSplatComponent->SnowResampleMaskStats = TEXT("삭제됨");
	GaussianSplatComponent->SnowResampleDiagnosticSummary = TEXT("눈 삭제됨: 재생성 후 진단이 다시 표시됩니다.");
	SyncWeatherEffects();
#if WITH_EDITOR
	MarkPackageDirty();
#endif
}

void AGaussianSplatActor::GenerateSnowGaussians()
{
	using namespace LCCSnowGaussian;
	if (!GaussianSplatComponent || !SnowGaussianComponent) return;

    UGaussianSplatComponent* Source = GaussianSplatComponent.Get();
    // 2.8.78: the existing, camera-independent snow generator is the authoritative
    // geometry source. Run it four times at REAL authored thicknesses, retaining
    // each complete asset/LOD, source 2-bit correspondence and per-snow packed field.
    // No geometry construction or surface queries are required for level switches.
    if (!bBuildingSnowHeightStages2878)
    {
        const float FullHeight = FMath::Clamp(SnowBakeMaximumHeightCm2878, 4.0f, 50.0f);
        const int32 StageCount2879 = FMath::Clamp(SnowBakeIntervalCount2879, 4, 12);
        const float SavedThickness = Source->SnowGaussianThicknessCm;
        const float SavedRuntimeHeight = SnowRuntimeHeightCm2878;
        const float OldBakedMaximum = SnowBakedMaximumHeightCm2878;
        const TObjectPtr<UGaussianSplatAsset> OldAsset = BakedSnowGaussianAsset;
        const TArray<FSnowHeightBakeStage2878> OldStages = BakedSnowHeightStages2878;
        const TArray<uint32> OldSourceMask = Source->SnowCoveragePackedMask;
        const float OldSourceThreshold = Source->SnowGaussianBakedPatchThreshold;
        const TArray<uint32> OldSnowField = SnowGaussianComponent->SnowBakedFieldPacked2873;
        const int32 OldSnowFieldCount = SnowGaussianComponent->SnowBakedFieldSplatCount2873;
        const float OldSnowFieldPattern = SnowGaussianComponent->SnowBakedFieldPatternCm2873;
        const FTransform OldSnowFieldTransform = SnowGaussianComponent->SnowBakedFieldLocalToWorld2873;
        const int32 OldSourceCoverageCount = Source->SnowCoverageMaskSourceCount;
        const int32 OldGeneratedCount = Source->SnowGaussianGeneratedCount;
        const TArray<uint32> OldSourceSnowField = Source->SnowBakedFieldPacked2873;
        const int32 OldSourceFieldCount = Source->SnowBakedFieldSplatCount2873;
        const float OldSourceFieldPattern = Source->SnowBakedFieldPatternCm2873;
        const FTransform OldSourceFieldTransform = Source->SnowBakedFieldLocalToWorld2873;
        const TObjectPtr<UGaussianSplatAsset> OldSourceFieldAsset = Source->SnowBakedFieldSourceAsset2873;
        SnowBakeRollbackStages2879 = OldStages;
        SnowBakeRollbackAsset2879 = OldAsset;
        BakedSnowHeightStages2878.Reset();
        bBuildingSnowHeightStages2878 = true;
        bool bAllStagesSucceeded = true;
        FString FailureReason;
        for (int32 Stage = 1; Stage <= StageCount2879; ++Stage)
        {
            const float StageHeight = FullHeight * (float(Stage) / float(StageCount2879));
            Source->SnowGaussianThicknessCm = StageHeight;
            UGaussianSplatAsset* PreviousAsset = BakedSnowGaussianAsset.Get();
            UE_LOG(LogTemp, Display, TEXT("[Snow2879Bake] Begin %d/%d, actual thickness %.2fcm"), Stage, StageCount2879, StageHeight);
            GenerateSnowGaussians(); // guard enters the pre-existing single-stage generator
            if (!BakedSnowGaussianAsset || !BakedSnowGaussianAsset->IsValid() ||
                BakedSnowGaussianAsset.Get() == PreviousAsset ||
                Source->SnowCoveragePackedMask.IsEmpty() ||
                SnowGaussianComponent->SnowBakedFieldPacked2873.IsEmpty())
            {
                bAllStagesSucceeded = false;
                FailureReason = FString::Printf(TEXT("단계 %d/%d (%.2fcm) 생성 실패: %s"),
                    Stage, StageCount2879, StageHeight, *Source->SnowGaussianBuildStatus);
                UE_LOG(LogTemp, Error, TEXT("[Snow2878Bake] %s"), *FailureReason);
                break;
            }
            FSnowHeightBakeStage2878& Finished = BakedSnowHeightStages2878.AddDefaulted_GetRef();
            Finished.HeightCm = StageHeight;
            Finished.SourceSplatAsset = Source->GetSplatAsset();
            Finished.SourceProxyMesh = Source->GetProxyShadowMesh();
            Finished.SourceProxyLocalTransform = Source->GetProxyShadowMeshLocalTransform();
            Finished.SnowAsset = BakedSnowGaussianAsset;
            Finished.SourceCoverageMask = Source->SnowCoveragePackedMask;
            Finished.SourceCoverageCount = Source->SnowCoverageMaskSourceCount;
            Finished.SnowFieldPacked = SnowGaussianComponent->SnowBakedFieldPacked2873;
            Finished.SnowFieldCount = SnowGaussianComponent->SnowBakedFieldSplatCount2873;
            Finished.SnowFieldPatternCm = SnowGaussianComponent->SnowBakedFieldPatternCm2873;
            Finished.SnowFieldLocalToWorld = SnowGaussianComponent->SnowBakedFieldLocalToWorld2873;
            Finished.SourcePatchThreshold = Source->SnowGaussianBakedPatchThreshold;
            Finished.OriginalGaussianCount = BakedSnowGaussianAsset->GetOriginalSplatCount();
            UE_LOG(LogTemp, Display, TEXT("[Snow2879Bake] Stored %d/%d %.2fcm, %d Snow GS, source mask %d words"),
                Stage, StageCount2879, StageHeight, Finished.OriginalGaussianCount, Finished.SourceCoverageMask.Num());
        }
        Source->SnowGaussianThicknessCm = SavedThickness; // runtime edits never change authoring settings
        bBuildingSnowHeightStages2878 = false;
        if (!bAllStagesSucceeded)
        {
            // Keep the old working snow state if any of the four bakes fails.
            BakedSnowHeightStages2878 = OldStages;
            SnowBakedMaximumHeightCm2878 = OldBakedMaximum;
            BakedSnowGaussianAsset = OldAsset;
            Source->SnowCoveragePackedMask = OldSourceMask;
            Source->SnowCoverageMaskSourceCount = OldSourceCoverageCount;
            Source->SnowGaussianBakedPatchThreshold = OldSourceThreshold;
            Source->SnowGaussianGeneratedCount = OldGeneratedCount;
            Source->SnowBakedFieldPacked2873 = OldSourceSnowField;
            Source->SnowBakedFieldSplatCount2873 = OldSourceFieldCount;
            Source->SnowBakedFieldPatternCm2873 = OldSourceFieldPattern;
            Source->SnowBakedFieldLocalToWorld2873 = OldSourceFieldTransform;
            Source->SnowBakedFieldSourceAsset2873 = OldSourceFieldAsset.Get();
            SnowGaussianComponent->SnowBakedFieldPacked2873 = OldSnowField;
            SnowGaussianComponent->SnowBakedFieldSplatCount2873 = OldSnowFieldCount;
            SnowGaussianComponent->SnowBakedFieldPatternCm2873 = OldSnowFieldPattern;
            SnowGaussianComponent->SnowBakedFieldLocalToWorld2873 = OldSnowFieldTransform;
            SnowGaussianComponent->SnowBakedFieldSourceAsset2873 = OldAsset.Get();
            SnowGaussianComponent->SetSplatAsset(OldAsset.Get());
            SnowGaussianComponent->RefreshSnowCoverageMaskRenderState();
            Source->RefreshSnowCoverageMaskRenderState();
            SnowRuntimeHeightCm2878 = SavedRuntimeHeight;
            AppliedSnowHeightStage2878 = -1;
            SnowHeightBakeStatus2878 = FailureReason;
            SnowBakeRollbackStages2879.Reset();
            SnowBakeRollbackAsset2879 = nullptr;
            SyncWeatherEffects();
            return;
        }
        SnowBakedMaximumHeightCm2878 = FullHeight;
        SnowBakeRollbackStages2879.Reset();
        SnowBakeRollbackAsset2879 = nullptr;
        SnowRuntimeHeightCm2878 = FullHeight; // 100% matches the unchanged existing single-height generator
        AppliedSnowHeightStage2878 = -1;
        SnowHeightBakeStatus2878 = FString::Printf(
            TEXT("0~%.2fcm 베이크 완료 / %d구간 / 총 %d개 상태 / 조작 시 nearest stage 사용"),
            FullHeight, StageCount2879, StageCount2879 + 1);
        SyncWeatherEffects();
#if WITH_EDITOR
        MarkPackageDirty();
#endif
        return;
    }
    Source->SnowMicroClusterBuildSummary=TEXT("군집 생성 대기 중");
	Source->SnowResampleLastBuild=TEXT("2.8.70 생성 중 / 완료 대기");
	Source->SnowResampleDiagnosticSummary=TEXT("2.8.70 생성 진행 중: 이전 수치는 유효하지 않습니다.");
	Source->SnowResampleMaskStats=TEXT("원본 GS 적설 마스크 생성 중");
	Source->SnowResampleReceiverCells=0;
	Source->SnowResampleGenerated=0;
	Source->SnowResampleRefillPlaced=0;
	Source->SnowResampleUnmetCenters=0;
	Source->SnowResampleUnmetFine=0;
	Source->SnowResampleOpticalUnmetCenters=0;
	Source->SnowResampleGeometricUnmetCenters=0;
	Source->SnowResampleQuadSites=0;
	Source->SnowResampleUnmetQuadSites=0;
	Source->SnowResampleUnusedSurfaceSlots=0;
	Source->SnowResampleRejectedSupport=0;
	Source->SnowCohesionBridgeCandidates=0;
	Source->SnowFineCandidates2869 = 0;
	Source->SnowFinePlaced2869 = 0;
	Source->SnowFineUnmet2869 = 0;
	Source->SnowFinePasses2869 = 0;
	Source->SnowOpticalFinishChanged2870 = 0;
	Source->SnowOpticalFinishCandidateSites2870 = 0;
	Source->SnowOpticalFinishUnmetBefore2870 = 0;
	Source->SnowOpticalFinishUnmetAfter2870 = 0;
	Source->SnowOpticalUnmetFloor2870 = 0;
	Source->SnowOpticalUnmetRoof2870 = 0;
	Source->SnowOpticalUnmetStone2870 = 0;
	Source->SnowCohesionBridgePlaced=0;
	Source->SnowResampleTotalGenerated=0;
	Source->SnowResampleRefillCandidateSites=0;
	Source->SnowResampleMeasuredRounds=0;
	Source->SnowResampleMeasuredRequiredCount=-1;
	Source->SnowResampleStopReason=TEXT("생성 중");
	Source->SnowProxyNormalApplied=0;
	Source->SnowProxyNormalSoftFloor=0;
	Source->SnowProxyNormalMissing=0;
	Source->SnowProxyNormalInvalid=0;
	Source->SnowProxyNormalSourceGateRejected=0;
	const UStaticMesh* ProxyMesh = Source->GetProxyShadowMesh();
	UGaussianSplatAsset* SourceAsset = Source->GetSplatAsset();
	auto Fail = [Source](const FString& Why)
	{
		Source->SnowGaussianBuildStatus = Why;
		Source->SnowResampleLastBuild = TEXT("2.8.69 생성 실패");
		Source->SnowResampleStopReason = Why;
		Source->SnowResampleDiagnosticSummary = Why;
		UE_LOG(LogTemp, Warning, TEXT("[SnowGaussian] %s"), *Why);
	};

	if (!ProxyMesh || !SourceAsset || !SourceAsset->IsValid())
	{
		Fail(TEXT("실패: 프록시 메쉬와 원본 GS Asset이 모두 필요합니다."));
		return;
	}
	const FStaticMeshRenderData* MeshData = ProxyMesh->GetRenderData();
	if (!MeshData || MeshData->LODResources.IsEmpty())
	{
		Fail(TEXT("실패: Proxy LOD0의 CPU 메시 데이터를 찾지 못했습니다."));
		return;
	}
	const FStaticMeshLODResources& LOD = MeshData->LODResources[0];
	const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
	const FRawStaticIndexBuffer& Indices = LOD.IndexBuffer;
	const int32 SourceTriangleCount = static_cast<int32>(Indices.GetNumIndices() / 3u);
	if (Positions.GetNumVertices() < 3 || SourceTriangleCount == 0)
	{
		Fail(TEXT("실패: Proxy LOD0에 삼각형이 없습니다."));
		return;
	}

	// All geometry decisions happen once, in world space. The renderer may project
	// these positions from any MRQ pane or cubemap view without re-seeding them.
	const FTransform ProxyWorld = Source->GetProxyShadowMeshWorldTransform();
	const FTransform GSWorld = Source->GetComponentTransform();
	// The source-normal fit applies the inverse-transpose: reject invalid actor
	// scale before reading source covariances or dividing normal components.
	const FVector SafeGSScale = GSWorld.GetScale3D();
	if (FMath::Min3(FMath::Abs(SafeGSScale.X), FMath::Abs(SafeGSScale.Y), FMath::Abs(SafeGSScale.Z)) < 0.001)
	{
		Fail(TEXT("실패: GS component scale의 한 축이 0에 가깝습니다."));
		return;
	}
	const double UpLimit = FMath::Clamp((double)Source->SnowGaussianSlopeThreshold, 0.0, 0.98);
	const double BaseThickness = FMath::Clamp((double)Source->SnowGaussianThicknessCm, 1.0, 50.0);
	const double MoundCm = FMath::Clamp((double)Source->SnowGaussianMoundVariationCm, 0.0, 25.0);
	const double MoundScale = FMath::Max(50.0, (double)Source->SnowGaussianMoundScaleCm);
	const double SupportRadius = FMath::Clamp((double)Source->SnowGaussianSupportRadiusCm, 10.0, 250.0);
	const int32 MaxSnowCount = FMath::Clamp(Source->SnowGaussianMaxCount, 1000, 2000000);
	int32 ProxySnowUsed = 0, ProxySnowSoft = 0, ProxySnowNoMatch = 0;
	int32 ProxySnowInvalid = 0, ProxySnowSourceRejected = 0;
	// 2.8.40 accidentally devoted just 10% of the cap to the entire visible
	// snow surface. That produced painted source GS beside isolated bright discs.
	// Restore the contiguous v2835-sized geometry budget, while the source GS
	// keeps its original covariance and trained SH as the foundation. Existing
	// serialized 0.10 settings remain valid and now represent the adjustable
	// fraction ABOVE a 0.60 coverage reserve (0.10 -> 0.70 of the authored cap).
	const double ReliefFraction = FMath::Clamp(
		0.60 + (double)Source->SnowGaussianReliefBudgetFraction, 0.62, 0.90);
	const int32 SurfaceBudget = FMath::Max(64,
		FMath::FloorToInt(double(MaxSnowCount) * ReliefFraction));
	const int32 PrimaryBudget = FMath::Max(48, FMath::FloorToInt(double(SurfaceBudget) * 0.91));
	const int32 ShoulderBudget = FMath::Max(0, SurfaceBudget - PrimaryBudget);
	const bool bFeatures = Source->bSnowGaussianLandformFeatures && Source->SnowGaussianFeatureStrength > 0.01f;
	const double FeatureStrength = FMath::Clamp((double)Source->SnowGaussianFeatureStrength, 0.0, 1.5);
	const double LedgeRadius = FMath::Clamp((double)Source->SnowGaussianLedgeRadiusCm, 8.0, 80.0);
	const double PeakRadius = FMath::Clamp((double)Source->SnowGaussianTipRadiusCm, 5.0, 90.0);
	const double VolumeStrength = FMath::Clamp((double)Source->SnowGaussianVolumeStrength, 0.0, 1.0);
	const double WindStrength = bFeatures ? FMath::Clamp((double)Source->SnowGaussianWindStrength, 0.0, 1.0) : 0.0;
	const double WindAngle = FMath::DegreesToRadians(FMath::Fmod((double)Source->SnowGaussianWindDirectionDegrees, 360.0));
	const FVector WindTo(FMath::Cos(WindAngle), FMath::Sin(WindAngle), 0.0);
	const bool bRailSnow = bFeatures && Source->bSnowGaussianRailCaps && Source->SnowGaussianRailStrength > 0.01f;
	const double RailStrength = bRailSnow ? FMath::Clamp((double)Source->SnowGaussianRailStrength, 0.0, 1.0) : 0.0;

	// Weld at 3-cm world precision: StaticMesh LODs often duplicate vertices at
	// UV/material seams. Only the *geometry-analysis* representation is welded.
	TArray<FWeldVertex> WeldedVertices;
	TMap<FIntVector, int32> WeldedLookup;
	if (bFeatures || Source->bSnowProxyNormalPriority)
	{
		WeldedLookup.Reserve(FMath::Min<int32>((int32)Positions.GetNumVertices(), 500000));
		WeldedVertices.Reserve(FMath::Min<int32>((int32)Positions.GetNumVertices(), 500000));
	}
	const auto WeldPoint = [&WeldedLookup, &WeldedVertices](const FVector& P, double Up, const FVector& Normal, double Area2) -> int32
	{
		const FIntVector Key(FMath::RoundToInt(P.X / 3.0), FMath::RoundToInt(P.Y / 3.0), FMath::RoundToInt(P.Z / 3.0));
		int32* Found = WeldedLookup.Find(Key);
		const int32 Index = Found ? *Found : WeldedVertices.AddDefaulted();
		if (!Found) WeldedLookup.Add(Key, Index);
		FWeldVertex& V = WeldedVertices[Index];
		V.Position += P;
		++V.PositionSamples;
		++V.IncidentFaces;
		V.MinUp = FMath::Min(V.MinUp, Up);
		V.MaxUp = FMath::Max(V.MaxUp, Up);
		// Square-root area weighting avoids oversized/uneven imported triangles
		// overwhelming the local vertex direction. Hard folds are handled at use.
		V.ProxyNormalSum += Normal * FMath::Sqrt(FMath::Clamp(Area2, 1.0, 10000.0));
		return Index;
	};

	TArray<FTriangle> Triangles;
	Triangles.SetNum(SourceTriangleCount);
	double TotalProjectedArea = 0.0;
	int32 EligibleTriangles = 0;
	FBox ProxySurfaceBounds(ForceInit);
	for (int32 I = 0; I < SourceTriangleCount; ++I)
	{
		FTriangle& T = Triangles[I];
		const uint32 Ia = Indices.GetIndex(I * 3u);
		const uint32 Ib = Indices.GetIndex(I * 3u + 1u);
		const uint32 Ic = Indices.GetIndex(I * 3u + 2u);
		if (Ia >= Positions.GetNumVertices() || Ib >= Positions.GetNumVertices() || Ic >= Positions.GetNumVertices()) continue;
		T.A = ProxyWorld.TransformPosition(FVector(Positions.VertexPosition(Ia)));
		T.B = ProxyWorld.TransformPosition(FVector(Positions.VertexPosition(Ib)));
		T.C = ProxyWorld.TransformPosition(FVector(Positions.VertexPosition(Ic)));
		if (T.A.ContainsNaN() || T.B.ContainsNaN() || T.C.ContainsNaN()) continue;
		const FVector Cross = FVector::CrossProduct(T.B - T.A, T.C - T.A);
		const double Area2 = Cross.Size();
		if (Area2 < 1.0e-5) continue;
		T.Normal = Cross / Area2;
		// Imported proxy winding can be inverted. Snow chooses the top face,
		// NOT a camera-facing triangle normal.
		if (T.Normal.Z < 0.0) T.Normal *= -1.0;
		T.Up = (float)T.Normal.Z;
		T.bValid = true;
		ProxySurfaceBounds += T.A;
		ProxySurfaceBounds += T.B;
		ProxySurfaceBounds += T.C;
		if (bFeatures || Source->bSnowProxyNormalPriority)
		{
			T.Welded[0] = WeldPoint(T.A, T.Up, T.Normal, Area2);
			T.Welded[1] = WeldPoint(T.B, T.Up, T.Normal, Area2);
			T.Welded[2] = WeldPoint(T.C, T.Up, T.Normal, Area2);
		}
		if (T.Up >= UpLimit)
		{
			const double XYArea = 0.5 * FMath::Abs(Cross.Z);
			if (XYArea > 1.0) { TotalProjectedArea += XYArea; ++EligibleTriangles; }
		}
	}
	if (!EligibleTriangles || TotalProjectedArea < 1.0)
	{
		Fail(TEXT("실패: 위쪽을 향한 Proxy 삼각형이 없습니다. 경사 허용값을 낮춰주세요."));
		return;
	}

	// Conservatively fit the sampling lattice inside the authored global cap. It
	// adapts ONCE to proxy area, never to the camera distance / MRQ tile.
	const double WantedSpacing = FMath::Clamp((double)Source->SnowGaussianSpacingCm, 10.0, 250.0);
	// A conservative initial estimate only. The active support/sky-exposed area
	// is measured below before choosing the actual surface lattice pitch.
	double EffectiveSpacing = FMath::Max(WantedSpacing,
		FMath::Sqrt(TotalProjectedArea / (0.78 * SurfaceBudget)));
	const double SkyCell = FMath::Max(120.0, EffectiveSpacing * 3.0);
	
	// Geometric snow traps, computed ONLY from the saved proxy. An exposed rim
	// joins an upward shelf to a steeper wall (or ends at a real top boundary).
	// A recessed shelf joins two sharply different, partially upward faces.
	// Smooth vertical walls are not selected: no uniform wall whitening.
	TArray<FProxyEdge> ProxyEdges;
	TArray<int32> FeatureEdgeIndices;
	int32 CandidateTips = 0;
	if (bFeatures)
	{
		for (FWeldVertex& V : WeldedVertices)
			if (V.PositionSamples > 0) V.Position /= (double)V.PositionSamples;

		TMap<uint64, int32> EdgeIndex;
		EdgeIndex.Reserve(SourceTriangleCount * 2);
		const auto AddEdge = [&EdgeIndex, &ProxyEdges, &WeldedVertices](int32 A, int32 B, int32 Face)
		{
			if (A == B || A < 0 || B < 0) return;
			if (A > B) Swap(A, B);
			const uint64 Key = (uint64(uint32(A)) << 32u) | uint64(uint32(B));
			int32* Existing = EdgeIndex.Find(Key);
			const int32 E = Existing ? *Existing : ProxyEdges.AddDefaulted();
			if (!Existing)
			{
				EdgeIndex.Add(Key, E);
				ProxyEdges[E].A = A;
				ProxyEdges[E].B = B;
			}
			FProxyEdge& Edge = ProxyEdges[E];
			if (Edge.NumFaces == 0) Edge.FirstFace = Face;
			if (Edge.NumFaces == 1) Edge.SecondFace = Face;
			++Edge.NumFaces;
		};
		for (int32 I = 0; I < Triangles.Num(); ++I)
		{
			const FTriangle& T = Triangles[I];
			if (!T.bValid) continue;
			AddEdge(T.Welded[0], T.Welded[1], I);
			AddEdge(T.Welded[1], T.Welded[2], I);
			AddEdge(T.Welded[2], T.Welded[0], I);
		}
		const auto OppositeZ = [&Triangles](int32 FaceIndex, int32 A, int32 B) -> double
		{
			if (FaceIndex < 0) return -1.0e30;
			const FTriangle& T = Triangles[FaceIndex];
			for (int32 J = 0; J < 3; ++J)
				if (T.Welded[J] != A && T.Welded[J] != B)
					return J == 0 ? T.A.Z : (J == 1 ? T.B.Z : T.C.Z);
			return -1.0e30;
		};
		for (int32 I = 0; I < ProxyEdges.Num(); ++I)
		{
			FProxyEdge& E = ProxyEdges[I];
			if (E.NumFaces > 2) continue; // Non-manifold/overlapping proxy: untrusted feature.
			FWeldVertex& A = WeldedVertices[E.A];
			FWeldVertex& B = WeldedVertices[E.B];
			A.MinNeighborZ = FMath::Min(A.MinNeighborZ, B.Position.Z);
			A.MaxNeighborZ = FMath::Max(A.MaxNeighborZ, B.Position.Z);
			B.MinNeighborZ = FMath::Min(B.MinNeighborZ, A.Position.Z);
			B.MaxNeighborZ = FMath::Max(B.MaxNeighborZ, A.Position.Z);
			const FVector Segment = B.Position - A.Position;
			const double Length = Segment.Size();
			if (Length < 12.0 || FMath::Abs(Segment.Z) > Length * 0.25) continue;
			const FTriangle& F0 = Triangles[E.FirstFace];
			const bool bBoundary = E.NumFaces == 1;
			const FTriangle& F1 = Triangles[bBoundary ? E.FirstFace : E.SecondFace];
			const double LowUp = FMath::Min((double)F0.Up, (double)F1.Up);
			const double HighUp = FMath::Max((double)F0.Up, (double)F1.Up);
			const bool bSharp = bBoundary ||
				FMath::Abs((double)F0.Up - (double)F1.Up) > 0.20 ||
				FMath::Abs(FVector::DotProduct(F0.Normal, F1.Normal)) < 0.82;
			const double EdgeHeight = (A.Position.Z + B.Position.Z) * 0.5;
			const bool bAboveAdjacentFace =
				(F0.Up < UpLimit && EdgeHeight > OppositeZ(E.FirstFace, E.A, E.B) + 12.0) ||
				(!bBoundary && F1.Up < UpLimit && EdgeHeight > OppositeZ(E.SecondFace, E.A, E.B) + 12.0);
			// The top lip of a cliff/structure, including unclosed roofless proxies.
			E.bTopLip = bSharp && bAboveAdjacentFace && LowUp < FMath::Max(0.35, UpLimit * 0.75) &&
				(bBoundary || HighUp >= 0.22);
			// Sharp sloped recesses catch small snow patches; pure vertical facets do not.
			E.bCreviceShelf = !E.bTopLip && !bBoundary && bSharp &&
				LowUp > 0.10 && HighUp >= 0.23 && LowUp < UpLimit;
			if (E.bTopLip || E.bCreviceShelf) FeatureEdgeIndices.Add(I);
		}
	}

	// Rasterize EVERY original splat into a coarse XYZ-cell + 4x4x4 fine-voxel
	// occupancy mask. v2831 kept only the FIRST 12 points of a coarse cell:
	// in a dense scan that silently discarded real geometry evidence and left
	// apparently random holes in the snow. 64 bits/cell is order-independent and
	// bounded even for extremely dense input data.
	// This is a conservative occupancy test: a source point anywhere in an
	// occupied microcell provides evidence within the specified support distance
	// of the microcell AABB (not necessarily of that point). Fine cells bound the
	// maximum over-acceptance without resurrecting floating proxy triangles.
	const double SupportCell = FMath::Max(30.0, SupportRadius);
	const double MicroCell = SupportCell * 0.25;
	const double SupportRadiusSq = SupportRadius * SupportRadius;
	// Independent of the proxy mesh resolution: the source may contain fine roof
	// ridges, stone edges and height changes that no proxy triangle represents.
	const double SourceDetailCellCm = FMath::Clamp((double)Source->SnowGaussianSourceDetailCellCm, 12.0, 45.0);
	const double SourceHeightToleranceCm = FMath::Clamp((double)Source->SnowGaussianSourceHeightToleranceCm, 10.0, 90.0);
	const double SourceDetailStrength = FMath::Clamp((double)Source->SnowGaussianSourceDetailStrength, 0.0, 1.0);
	const double SourceMaxWorldAxis = FMath::Max3(FMath::Abs(SafeGSScale.X), FMath::Abs(SafeGSScale.Y), FMath::Abs(SafeGSScale.Z));
	int32 OversizedSourceShapeRejects = 0;
	struct FSupportVoxelMask { uint64 Bits = 0ULL; };
	TMap<FIntVector, FSupportVoxelMask> SupportGrid;
	TMap<FIntVector, FSourceShape> SourceShapeGrid;
	TMap<FIntVector, FSourceSurfaceCell> SourceSurfaceGrid;
	SourceShapeGrid.Reserve(40000);
	SourceSurfaceGrid.Reserve(FMath::Min(SourceAsset->GetOriginalSplatCount() / 18, 320000));
	const FBox SourceDetailBounds = ProxySurfaceBounds.ExpandBy(SourceHeightToleranceCm + SupportRadius);
	bool bSourceCovarianceAvailable = true;
	{
		const int32 SourceCount = SourceAsset->GetSplatCount();
		SupportGrid.Reserve(FMath::Min(SourceCount / 24, 650000));
		constexpr int32 PointChunkSize = 65536;
		TArray<FVector3f> PointChunk;
		PointChunk.Reserve(PointChunkSize);
		TArray<FQuat4f> SourceRotations;
		TArray<FVector3f> SourceScales;
		SourceRotations.Reserve(PointChunkSize);
		SourceScales.Reserve(PointChunkSize);
		for (int32 Offset = 0; Offset < SourceCount; Offset += PointChunkSize)
		{
			const int32 ThisCount = FMath::Min(PointChunkSize, SourceCount - Offset);
			if (!SourceAsset->GetDecompressedPositionRange(Offset, ThisCount, PointChunk))
			{
				Fail(TEXT("실패: 원본 GS splat 위치를 읽을 수 없습니다. (bulk position data)"));
				return;
			}
			const bool bCovarianceChunk = bSourceCovarianceAvailable &&
				SourceAsset->GetSnowSourceCovarianceRange(Offset, ThisCount, SourceRotations, SourceScales);
			if (!bCovarianceChunk) bSourceCovarianceAvailable = false;
			for (int32 LocalIndex = 0; LocalIndex < PointChunk.Num(); ++LocalIndex)
			{
				const FVector3f& PointLocal = PointChunk[LocalIndex];
				if (PointLocal.ContainsNaN()) continue;
				const FVector P = GSWorld.TransformPosition(FVector(PointLocal));
				if (P.ContainsNaN()) continue;
				// Hierarchy LOD splats are synthetic representatives, frequently very
				// wide and never trustworthy evidence of a physical floor or rooftop.
				// Only original imported splats may prove surface support / geometry.
				if (Offset + LocalIndex >= SourceAsset->GetOriginalSplatCount()) continue;
				const FIntVector C = XYZCell(P, SupportCell);
				const int32 SX = FMath::Clamp(FMath::FloorToInt((P.X - (double)C.X * SupportCell) / MicroCell), 0, 3);
				const int32 SY = FMath::Clamp(FMath::FloorToInt((P.Y - (double)C.Y * SupportCell) / MicroCell), 0, 3);
				const int32 SZ = FMath::Clamp(FMath::FloorToInt((P.Z - (double)C.Z * SupportCell) / MicroCell), 0, 3);
				SupportGrid.FindOrAdd(C).Bits |= (uint64(1) << (SX + 4 * SY + 16 * SZ));
				if (bCovarianceChunk && LocalIndex < SourceScales.Num() && LocalIndex < SourceRotations.Num())
				{
					const FVector3f& Sc = SourceScales[LocalIndex];
					const FQuat4f& R = SourceRotations[LocalIndex];
					if (Sc.ContainsNaN() || R.ContainsNaN()) continue;
					const double SMin = FMath::Min3(FMath::Abs(double(Sc.X)), FMath::Abs(double(Sc.Y)), FMath::Abs(double(Sc.Z)));
					const double SMax = FMath::Max3(FMath::Abs(double(Sc.X)), FMath::Abs(double(Sc.Y)), FMath::Abs(double(Sc.Z)));
					if (SMin < 1.0e-5 || SMax < 1.0e-5) continue;
					const double Planarity = 1.0 - SMin / SMax;
					if (Planarity < 0.34)
					{
						continue;
					}
					// Very large fitted splats (distant backdrop / sky / LOD-like
					// geometry) must never seed the local snow receiver topology.
					if (SMax * SourceMaxWorldAxis > 460.0) { ++OversizedSourceShapeRejects;
						continue; }
					// The smallest covariance axis is the measured tangent-plane normal.
					// Discard near-spherical or vertical clutter before local surface fitting.
					if (Offset + LocalIndex < SourceAsset->GetOriginalSplatCount() &&
						SourceDetailBounds.IsInsideOrOn(P))
					{
						const double SXAbs = FMath::Abs(double(Sc.X));
						const double SYAbs = FMath::Abs(double(Sc.Y));
						const double SZAbs = FMath::Abs(double(Sc.Z));
						const double MidScale = SXAbs + SYAbs + SZAbs - SMin - SMax;
						const double SheetQuality = 1.0 - SMin / FMath::Max(1.0e-5, MidScale);
						if (SheetQuality >= 0.30)
						{
							const FQuat LocalRotation((double)R.X, (double)R.Y, (double)R.Z, (double)R.W);
							const FVector SmallAxis = SXAbs <= SYAbs && SXAbs <= SZAbs ? FVector::ForwardVector :
								(SYAbs <= SZAbs ? FVector::RightVector : FVector::UpVector);
							const FVector LocalNormal = LocalRotation.GetNormalized().RotateVector(SmallAxis);
							const FVector S = GSWorld.GetScale3D();
							FVector WorldNormal = GSWorld.GetRotation().RotateVector(FVector(
								LocalNormal.X / S.X, LocalNormal.Y / S.Y, LocalNormal.Z / S.Z)).GetSafeNormal();
							if (WorldNormal.Z < 0.0) WorldNormal *= -1.0;

							if (WorldNormal.Z < 0.075)
								++ProxySnowSourceRejected;
							if (WorldNormal.Z >= 0.075 && !WorldNormal.ContainsNaN())
							{
								const double QualityWeight = FMath::Clamp(SheetQuality, 0.30, 0.95);
								FSourceSurfaceCell& Cell = SourceSurfaceGrid.FindOrAdd(XYZCell(P, SourceDetailCellCm));
								Cell.PositionSum += P * QualityWeight;
								Cell.NormalSum += WorldNormal * QualityWeight;
								Cell.WeightSum += QualityWeight;
								if (WorldNormal.Z >= 0.77) Cell.VerifiedUpWeight += QualityWeight;
								++Cell.Samples;

							}
						}
					}
					FSourceShape* Existing = SourceShapeGrid.Find(C);
					if (!Existing || Existing->Planarity < Planarity)
					{
						FSourceShape& Dest = SourceShapeGrid.FindOrAdd(C);
						Dest.WorldPosition = P;
						Dest.Rotation = FQuat((double)R.X, (double)R.Y, (double)R.Z, (double)R.W).GetNormalized();
						Dest.Scale = FVector(FMath::Abs(double(Sc.X)), FMath::Abs(double(Sc.Y)), FMath::Abs(double(Sc.Z)));
						Dest.Planarity = Planarity;
					}
				}

			}
		}
	}
	// v2846: if the source has covariance, the original Gaussian surface is the
	// PRIMARY sampling domain. Proxy triangles never seed these snow patches.
	const bool bSourceDrivenSnow = bSourceCovarianceAvailable && !SourceSurfaceGrid.IsEmpty();
	// The v2848 comparison generator was retired: source-surface reconstruction is the sole snow geometry path.
	const bool bReconstructedSnow = bSourceDrivenSnow;
	if (!bSourceDrivenSnow)
	{
		Fail(TEXT("2.8.47 원본 GS의 공분산/표면 데이터가 부족하여 Source-first 적설을 만들 수 없습니다. 기존 적설 결과는 유지합니다."));
		return;
	}
	if (SupportGrid.IsEmpty())
	{
		Fail(TEXT("실패: 원본 GS의 월드 위치 데이터를 읽을 수 없습니다."));
		return;
	}
	for (TPair<FIntVector, FSourceSurfaceCell>& Entry : SourceSurfaceGrid)
	{
		FSourceSurfaceCell& Cell = Entry.Value;
		if (Cell.WeightSum > 0.0)
		{
			Cell.PositionSum /= Cell.WeightSum;
			Cell.NormalSum.Normalize();
		}
	}
	// 2.8.67: area-weighted weld normals at the matched LOD0 triangle.
	// Never smooth across an actual hard edge / disconnected layer: if a weld
	// normal deviates strongly from the selected face, retain that face normal.
	// Vertices are welded for normal analysis only, no mesh is created or edited.
	const auto SnowProxyNormalAt = [&Triangles, &WeldedVertices](int32 Receiver, const FVector& P) -> FVector
	{
		if (!Triangles.IsValidIndex(Receiver) || !Triangles[Receiver].bValid) return FVector::ZeroVector;
		const FTriangle& T = Triangles[Receiver];
		const FVector FaceN = T.Normal;
		const double Den = (T.B.Y-T.C.Y)*(T.A.X-T.C.X)+(T.C.X-T.B.X)*(T.A.Y-T.C.Y);
		if (FMath::Abs(Den) < 1.0e-6) return FaceN;
		const double A = ((T.B.Y-T.C.Y)*(P.X-T.C.X)+(T.C.X-T.B.X)*(P.Y-T.C.Y))/Den;
		const double B = ((T.C.Y-T.A.Y)*(P.X-T.C.X)+(T.A.X-T.C.X)*(P.Y-T.C.Y))/Den;
		const double C = 1.0-A-B;
		FVector Sum = FVector::ZeroVector;
		const double Weights[3] = {FMath::Clamp(A,0.0,1.0),FMath::Clamp(B,0.0,1.0),FMath::Clamp(C,0.0,1.0)};
		for (int32 I=0; I<3; ++I)
		{
			FVector VN = FaceN;
			const int32 V = T.Welded[I];
			if (WeldedVertices.IsValidIndex(V))
			{
				const FVector Candidate = WeldedVertices[V].ProxyNormalSum.GetSafeNormal();
				if (!Candidate.IsNearlyZero() && FVector::DotProduct(Candidate,FaceN) >= 0.88)
					VN = Candidate;
			}
			Sum += VN * Weights[I];
		}
		const FVector Result = Sum.GetSafeNormal();
		return !Result.IsNearlyZero() && Result.Z >= 0.10 ? Result : FaceN;
	};
	int32 SourceFitAccepted = 0;
	int32 SourceFitFallback = 0;
	// Fit a supported local surface at a proposed world position. Proxy height
	// only chooses WHICH physical layer to fit, never the output height. A
	// covariance-normal-consistent neighbor supplies a predicted surface Z;
	// multiple cells combine via robust spatial and elevation weights.
	const bool bProxyFitNormals = bReconstructedSnow && Source->bSnowSurfaceResampling &&
		Source->bSnowUseMicroClusters && Source->SnowMicroClusterStrength > 0.005f &&
		Source->bSnowProxyNormalPriority;
	const auto FitSourceSurface = [&SourceSurfaceGrid, SourceDetailCellCm,
		SourceHeightToleranceCm, SourceDetailStrength, Source, bProxyFitNormals](const FVector& ProxyP,
		const FVector& ProxyNormal, FVector& OutP, FVector& OutN) -> bool
	{
		OutP = ProxyP;
		OutN = ProxyNormal;
		if (SourceDetailStrength < 0.001 || SourceSurfaceGrid.IsEmpty()) return false;
		const double SearchRadius = FMath::Clamp(SourceDetailCellCm * 2.45, 30.0, 85.0);
		const int32 Span = FMath::CeilToInt(FMath::Max(SearchRadius, SourceHeightToleranceCm) / SourceDetailCellCm);
		const double RadiusSq = SearchRadius * SearchRadius;
		const FIntVector CellC = XYZCell(ProxyP, SourceDetailCellCm);
		double WeightedHeight = 0.0, TotalWeight = 0.0, NearestDistSq = TNumericLimits<double>::Max();
		FVector WeightedNormal = FVector::ZeroVector;
		int32 NeighborCells = 0;
		for (int32 DZ = -Span; DZ <= Span; ++DZ)
		for (int32 DY = -Span; DY <= Span; ++DY)
		for (int32 DX = -Span; DX <= Span; ++DX)
		{
			const FSourceSurfaceCell* Cell = SourceSurfaceGrid.Find(CellC + FIntVector(DX, DY, DZ));
			if (!Cell || Cell->Samples < 1 || Cell->WeightSum <= 0.0) continue;
			const FVector& Sample = Cell->PositionSum;
			const FVector N = Cell->NormalSum.GetSafeNormal();
			if (N.Z < 0.25 || FVector::DotProduct(N, ProxyNormal) < 0.42) continue;
			const double DXS = ProxyP.X - Sample.X, DYS = ProxyP.Y - Sample.Y;
			const double Dist2D = DXS * DXS + DYS * DYS;
			const double ZError = FMath::Abs(Sample.Z - ProxyP.Z);
			if (Dist2D > RadiusSq || ZError > SourceHeightToleranceCm) continue;
			const double PlaneZ = Sample.Z - (N.X * DXS + N.Y * DYS) / FMath::Max(0.25, N.Z);
			if (!FMath::IsFinite(PlaneZ) || FMath::Abs(PlaneZ - ProxyP.Z) > SourceHeightToleranceCm) continue;
			// A broad field is strongly discouraged from crossing separated roof,
			// vegetation or scanned-noise layers at the same XY location.
			const double W = FMath::Min(3.0, FMath::Sqrt(double(Cell->Samples))) *
				FMath::Exp(-Dist2D / FMath::Max(1.0, RadiusSq * 0.68)) *
				FMath::Exp(-FMath::Square(ZError / SourceHeightToleranceCm) * 2.5) *
				FMath::Clamp(FVector::DotProduct(N, ProxyNormal), 0.0, 1.0);
			WeightedHeight += PlaneZ * W;
			WeightedNormal += N * W;
			TotalWeight += W;
			NearestDistSq = FMath::Min(NearestDistSq, Dist2D);
			++NeighborCells;
		}
		if (NeighborCells < 2 || TotalWeight < 0.20 || NearestDistSq > RadiusSq * 0.70) return false;
		const double SurfaceZ = WeightedHeight / TotalWeight;
		const double Offset = FMath::Clamp(SurfaceZ - ProxyP.Z,
			-SourceHeightToleranceCm, SourceHeightToleranceCm);
		const double Confidence = FMath::Clamp(double(NeighborCells) / 5.0, 0.35, 1.0) * SourceDetailStrength;
		OutP.Z = ProxyP.Z + Offset * Confidence;
		// The proxy's triangle tangent is the A/B orientation baseline, never a
		// replacement for source height / thickness. No influence if A/B is OFF.
		const double ProxyWeight = bProxyFitNormals ?
			FMath::Clamp(double(Source->SnowProxyNormalBlend), 0.0, 1.0) : 0.0;
		OutN = FMath::Lerp(ProxyNormal, WeightedNormal.GetSafeNormal(),
			Confidence * 0.88 * (1.0 - ProxyWeight)).GetSafeNormal();
		return !OutP.ContainsNaN() && !OutN.IsNearlyZero();
	};
	const auto HasSourceGS = [&SupportGrid, SupportCell, MicroCell, SupportRadiusSq](const FVector& P) -> bool
	{
		const FIntVector C = XYZCell(P, SupportCell);
		// The support radius is <= coarse-cell size, so +/-1 includes every
		// possible occupied neighbor even if the query is on a cell boundary.
		for (int32 DZ = -1; DZ <= 1; ++DZ)
		for (int32 DY = -1; DY <= 1; ++DY)
		for (int32 DX = -1; DX <= 1; ++DX)
		{
			const FIntVector N = C + FIntVector(DX, DY, DZ);
			const FSupportVoxelMask* Occupancy = SupportGrid.Find(N);
			if (!Occupancy || Occupancy->Bits == 0) continue;
			const FVector Base((double)N.X * SupportCell, (double)N.Y * SupportCell, (double)N.Z * SupportCell);
			// Reject the full coarse cell quickly if it cannot overlap the sphere.
			const FVector CoarseMax = Base + FVector(SupportCell);
			const double CX = FMath::Max(0.0, FMath::Max(Base.X - P.X, P.X - CoarseMax.X));
			const double CY = FMath::Max(0.0, FMath::Max(Base.Y - P.Y, P.Y - CoarseMax.Y));
			const double CZ = FMath::Max(0.0, FMath::Max(Base.Z - P.Z, P.Z - CoarseMax.Z));
			if (CX * CX + CY * CY + CZ * CZ > SupportRadiusSq) continue;
			// Keep all occupied microcells, not 12 arbitrary representative GS
			// points. Compare against their bounds, not their potentially distant centers.
			const uint64 Bits = Occupancy->Bits;
			for (int32 K = 0; K < 64; ++K)
			{
				if ((Bits & (uint64(1) << K)) == 0) continue;
				const int32 X = K & 3;
				const int32 Y = (K >> 2) & 3;
				const int32 Z = (K >> 4) & 3;
				const FVector Min = Base + FVector((double)X * MicroCell, (double)Y * MicroCell, (double)Z * MicroCell);
				const FVector Max = Min + FVector(MicroCell);
				const double AX = FMath::Max(0.0, FMath::Max(Min.X - P.X, P.X - Max.X));
				const double AY = FMath::Max(0.0, FMath::Max(Min.Y - P.Y, P.Y - Max.Y));
				const double AZ = FMath::Max(0.0, FMath::Max(Min.Z - P.Z, P.Z - Max.Z));
				if (AX * AX + AY * AY + AZ * AZ <= SupportRadiusSq) return true;
			}
		}
		return false;
	};

	// World-space roof/soffit masking. A random detached proxy polygon must NOT
	// block snow underneath it: an occluding surface must have actual original-GS
	// support at the vertical ray hit as well. The camera never participates.
	TMap<FIntPoint, TArray<int32>> OverheadCells;
	TArray<int32> OversizedOverhead;
	// Shared broad-phase: used by BOTH +Z sky tests and source-GS / proxy
	// recovery. Do not leave the recovery grid empty when sky tests are disabled.
	{
		for (int32 I = 0; I < Triangles.Num(); ++I)
		{
			const FTriangle& T = Triangles[I];
			if (!T.bValid || T.Up < 0.05f) continue;
			const double MinX = FMath::Min3(T.A.X, T.B.X, T.C.X);
			const double MinY = FMath::Min3(T.A.Y, T.B.Y, T.C.Y);
			const double MaxX = FMath::Max3(T.A.X, T.B.X, T.C.X);
			const double MaxY = FMath::Max3(T.A.Y, T.B.Y, T.C.Y);
			const FIntPoint MinCell = XYCell(FVector(MinX, MinY, 0.0), SkyCell);
			const FIntPoint MaxCell = XYCell(FVector(MaxX, MaxY, 0.0), SkyCell);
			const int64 NumCells = (int64(MaxCell.X) - MinCell.X + 1) * (int64(MaxCell.Y) - MinCell.Y + 1);
			if (NumCells > 512 || NumCells <= 0) { OversizedOverhead.Add(I); continue; }
			for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
			for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
				OverheadCells.FindOrAdd(FIntPoint(X, Y)).Add(I);
		}
	}
	// The v2.8.58 production preset bypassed all four strict proxy blockers.
	// Retain a trivial callable for the current production paths but remove the
	// unreachable legacy roof-ray intersection and 13-way A/B implementation.
	const auto IsSheltered = [](int32, const FVector&) -> bool { return false; };

	// 2.8.57 floor-only blocker corroboration. A real roof normally has original
	// GS support over adjacent nearby rays. A solitary proxy hit with only coarse
	// splat support should not delete a dense *measured* ground patch. Never call
	// this from the cliff/rail/roof or general receiver pipeline.
	const auto IsFloorSheltered = [&](int32 Receiver, const FVector& P) -> bool
	{
		if (!Source->bSnowGaussianSkyExposure || !IsSheltered(Receiver, P)) return false;
		if (!Source->bSnowFloorPreferSource) return true;
		const double Offset = SourceDetailCellCm * 0.70;
		const FVector Delta[4] = {
			FVector( Offset,0.0,0.0), FVector(-Offset,0.0,0.0),
			FVector(0.0, Offset,0.0), FVector(0.0,-Offset,0.0)};
		int32 NeighborHits = 0;
		for (const FVector& D : Delta)
			if (IsSheltered(INDEX_NONE, P + D)) ++NeighborHits;
		return NeighborHits >= FMath::Clamp(Source->SnowFloorBlockerNeighborHits, 1, 4);
	};

	// v2844: Project ORIGINAL GS positions onto REAL upward proxy triangles.
	// This is the missing bridge between source tint and Snow-Gaussian geometry:
	// a region can be eligible without already containing a generated splat.
	// Each height query is local to world-XY bins; it is not a screen/depth query.
	// A detailed original scan can legitimately rise above a simplified proxy;
	// allow that offset for candidate lookup, then validate with the original GS fit.
	const double ReceiverZTolerance = FMath::Clamp(
		FMath::Max(SupportRadius * 0.48, SourceHeightToleranceCm * 0.90), 17.0, 70.0);
	const auto FindSnowReceiver = [&Triangles, &OverheadCells, &OversizedOverhead, &IsSheltered,
		SkyCell, ReceiverZTolerance, UpLimit, Source, SourceHeightToleranceCm](const FVector& P, int32& OutTriangle, FVector& OutSurface) -> bool
	{
		int32 CandidateTriangle = INDEX_NONE;
		double CandidateZ = 0.0;
		// Allow searching a farther REAL receiver in the controlled A/B experiment.
		double BestHeightError = FMath::Max(ReceiverZTolerance, SourceHeightToleranceCm * 4.0);
		const auto TryTriangle = [&](int32 Index)
		{
			const FTriangle& T = Triangles[Index];
			if (!T.bValid || T.Up < 0.10) return;
			double Height = 0.0;
			if (!HeightAtXY(T, P.X, P.Y, Height)) return;
			const double Error = FMath::Abs(P.Z - Height);
			if (Error >= BestHeightError) return;
			BestHeightError = Error;
			CandidateTriangle = Index;
			CandidateZ = Height;
		};
		if (const TArray<int32>* Nearby = OverheadCells.Find(XYCell(P, SkyCell)))
			for (int32 Index : *Nearby) TryTriangle(Index);
		for (int32 Index : OversizedOverhead) TryTriangle(Index);
		if (CandidateTriangle == INDEX_NONE) return false;
		const FVector Surface(P.X, P.Y, CandidateZ);
		if (Source->bSnowGaussianSkyExposure && IsSheltered(CandidateTriangle, Surface))
			return false;
		OutTriangle = CandidateTriangle;
		OutSurface = Surface;
		return true;
	};

	// Source-backed, floor-only tolerant proxy lookup. The output remains the
	// real GS MLS Z: the proxy is used ONLY as optional support/layer evidence.
	const auto FindSoftFloorReceiver = [&Triangles, &OverheadCells, &OversizedOverhead,
		&IsFloorSheltered, Source, SkyCell, ReceiverZTolerance, SourceHeightToleranceCm]
		(const FVector& P, int32& OutTriangle, FVector& OutSurface) -> bool
	{
		if (!Source->bSnowFloorPreferSource) return false;
		const double Limit = FMath::Clamp(FMath::Max(ReceiverZTolerance,
			SourceHeightToleranceCm * FMath::Clamp((double)Source->SnowFloorProxyHeightMultiplier, 1.0, 2.5)),
			ReceiverZTolerance, 110.0);
		const double MinUp = FMath::Clamp((double)Source->SnowFloorProxyMinUp, 0.50, 0.84);
		double BestError = Limit;
		int32 BestIndex = INDEX_NONE;
		double BestZ = 0.0;
		const auto Check = [&](int32 Index)
		{
			const FTriangle& T = Triangles[Index];
			if (!T.bValid || T.Up < MinUp) return;
			double Z = 0.0;
			if (!HeightAtXY(T, P.X, P.Y, Z)) return;
			const double D = FMath::Abs(P.Z - Z);
			if (D >= BestError) return;
			BestError = D;
			BestIndex = Index;
			BestZ = Z;
		};
		if (const TArray<int32>* Nearby = OverheadCells.Find(XYCell(P, SkyCell)))
			for (int32 Index : *Nearby) Check(Index);
		for (int32 Index : OversizedOverhead) Check(Index);
		if (BestIndex == INDEX_NONE || IsFloorSheltered(BestIndex, P)) return false;
		OutTriangle = BestIndex;
		OutSurface = FVector(P.X, P.Y, BestZ);
		return true;
	};

	// The source-GS tint samples this exact same field in CalcViewData, at
	// the same world position and same authoring pattern scale. Never derive
	// drift placement from a camera view or a second unrelated random mask.
	const auto DriftField = [MoundScale](const FVector& P) -> double
	{
		return SnowAccumulationField(P, MoundScale);
	};

	// Snow does not follow a flat 2D raster boundary; cross-scale coherent noise
	// and local Z change the margin, while source-GS geometry is still the support.
	const auto SoftSnowField = [&DriftField, MoundScale](const FVector& P) -> double
	{
		const FVector Q(P.X + 0.20 * MoundScale * FMath::PerlinNoise3D((P + FVector(37.0, 19.0, 0.0)) / (MoundScale * 0.85)),
			P.Y + 0.20 * MoundScale * FMath::PerlinNoise3D((P + FVector(-13.0, 71.0, 0.0)) / (MoundScale * 0.85)), P.Z);
		return FMath::Clamp(0.78 * DriftField(Q) + 0.22 * (0.5 + 0.5 * FMath::PerlinNoise3D(P / (MoundScale * 0.72))), 0.0, 1.0);
	};
	const auto Smooth01 = [](double T) -> double
	{
		const double C = FMath::Clamp(T, 0.0, 1.0);
		return C * C * (3.0 - 2.0 * C);
	};

	// The 2.8.34 lattice was sized for *all* upward proxy triangles, even
	// unsupported and roof-sheltered geometry. That needlessly enlarged spacing
	// in the very places that should have continuous snow. Probe a coarse,
	// deterministic lattice on actual GS-supported, sky-exposed proxy samples.
	// This prepass never uses a render view and never creates a snow asset.
	const double PilotSpacing = FMath::Max(WantedSpacing * 1.8, EffectiveSpacing * 1.8);
	TSet<FIntVector> PilotUnique;
	// Estimate a scene-specific quantile of the supported/exposed proxy area.
	// Only its high-coverage, spatially coherent fraction receives extra splats.
	int64 PilotPatchHistogram[64] = {};
	int32 PilotAccepted = 0;
	int32 PilotVisited = 0;
	for (int32 TriIndex = 0; !bSourceDrivenSnow && TriIndex < Triangles.Num(); ++TriIndex)
	{
		const FTriangle& T = Triangles[TriIndex];
		if (!T.bValid || T.Up < UpLimit) continue;
		const int32 X0 = FMath::FloorToInt(FMath::Min3(T.A.X, T.B.X, T.C.X) / PilotSpacing);
		const int32 X1 = FMath::FloorToInt(FMath::Max3(T.A.X, T.B.X, T.C.X) / PilotSpacing);
		const int32 Y0 = FMath::FloorToInt(FMath::Min3(T.A.Y, T.B.Y, T.C.Y) / PilotSpacing);
		const int32 Y1 = FMath::FloorToInt(FMath::Max3(T.A.Y, T.B.Y, T.C.Y) / PilotSpacing);
		const int64 AreaCells = (int64(X1)-X0+1) * (int64(Y1)-Y0+1);
		if (AreaCells <= 0) continue;
		// An oversized proxy triangle can still be sampled instead of being
		// discarded wholesale. Its coarser pilot coverage is approximate.
		const int32 Step = FMath::Max(1, FMath::CeilToInt(FMath::Sqrt(double(AreaCells) / 400000.0)));
		for (int32 Y = Y0; Y <= Y1; Y += Step)
		for (int32 X = X0; X <= X1; X += Step)
		{
			const double PX = (double(X) + 0.5 + ((Y & 1) ? 0.23 : -0.23)) * PilotSpacing;
			const double PY = (double(Y) + 0.5) * PilotSpacing;
			double Z = 0.0;
			if (!HeightAtXY(T, PX, PY, Z)) continue;
			++PilotVisited;
			const FVector P(PX, PY, Z);
			if (!HasSourceGS(P) || (Source->bSnowGaussianSkyExposure && IsSheltered(TriIndex, P))) continue;
			const FIntVector Key = XYZCell(P, FMath::Max(10.0, PilotSpacing * 0.52));
			if (!PilotUnique.Contains(Key))
			{
				PilotUnique.Add(Key);
				// Weight an oversized-triangle pilot sample by the skipped cells.
				const int32 Weight = Step * Step;
				PilotAccepted += Weight;
				const double D = FMath::Clamp(DriftField(P), 0.0, 0.99999);
				const int32 Bucket = FMath::Clamp(FMath::FloorToInt(D * 64.0), 0, 63);
				PilotPatchHistogram[Bucket] += Weight;
			}
		}
	}
	// The v2835 full-coverage layer is restored in broad, coherent regions;
	// relief is no longer artificially restricted to a few white dots.
	// A histogram of truly supported, outdoor proxy area fits the threshold
	// to the available budget before we emit any Gaussian in triangle order.
	double PatchThreshold = 0.33;
	double TargetPatchFraction = 0.72;
	if (PilotAccepted >= 24)
	{
		const double ActiveArea = double(PilotAccepted) * PilotSpacing * PilotSpacing;
		const double CoverableAtAuthoredPitch = double(PrimaryBudget) * WantedSpacing * WantedSpacing * 0.89 /
			FMath::Max(1.0, ActiveArea);
		// Fit as much outdoor area as practical while reserving room for
		// coherent local relief. Never inflate the lattice just to stamp a few
		// white spots across an oversized scene.
		TargetPatchFraction = FMath::Clamp(CoverableAtAuthoredPitch, 0.20, 0.83);
		if (CoverableAtAuthoredPitch < 0.20)
		{
			EffectiveSpacing = FMath::Max(WantedSpacing,
				FMath::Sqrt(ActiveArea * TargetPatchFraction / (double(PrimaryBudget) * 0.89)));
		}
		else
		{
			EffectiveSpacing = WantedSpacing;
		}
		int64 UpperTail = 0;
		for (int32 Bin = 63; Bin >= 0; --Bin)
		{
			UpperTail += PilotPatchHistogram[Bin];
			if (double(UpperTail) >= double(PilotAccepted) * TargetPatchFraction)
			{
				PatchThreshold = double(Bin) / 64.0;
				break;
			}
		}
	}

	TArray<FGaussianSplatData> SnowSplats;
	SnowSplats.Reserve(FMath::Min(MaxSnowCount, 250000));
	TArray<FGaussianSplatData> PatchShoulders;
	PatchShoulders.Reserve(ShoulderBudget);
	int32 DriftMaskRejects = 0;
	TArray<FGaussianSplatData> OptionalVolume;
	OptionalVolume.Reserve(FMath::Min(SurfaceBudget / 8, 16000));
	TArray<uint8> OptionalVolumeKinds; // 1=lower body; retained 2 code for older stats compatibility
	OptionalVolumeKinds.Reserve(FMath::Min(SurfaceBudget / 8, 16000));
	int32 UnsupportedSamples = 0;
	int32 ShelteredSamples = 0;
	int32 DuplicateSamples = 0;
	int32 HugeTriangleSkips = 0;
	int32 XYInsideSamples = 0;
	int32 TipCount = 0;
	int32 LipCount = 0;
	int32 ShelfCount = 0;
	int32 FeatureGSRejects = 0;
	int32 FeatureShelterRejects = 0;
	TSet<FIntVector> UniqueSnowCells;
	TSet<FIntVector> UniqueFeatureCells;
	// Ellipsoids are authored in WORLD centimeters, not divided by the actor's
	// average scale. Each covariance axis is converted back through its own GS
	// component transform so differently-scaled actors retain the requested shape.
	const FVector AbsoluteScale = GSWorld.GetScale3D().GetAbs();
	if (FMath::Min3(AbsoluteScale.X, AbsoluteScale.Y, AbsoluteScale.Z) < 0.001)
	{
		Fail(TEXT("실패: GS component scale의 한 축이 0에 가깝습니다."));
		return;
	}
	int32 MadeLower = 0;
	int32 MadeUpper = 0;
	double SumTopXY = 0.0;
	double SumTopZ = 0.0;
	int32 TopSamples = 0;
	const double SHC0 = 0.28209479177387814;
	const FLinearColor SnowColor = Source->SnowGaussianColor;
	const float SnowOpacity = FMath::Clamp(Source->SnowGaussianOpacity, 0.1f, 1.0f);
	const float ViewResponse = FMath::Clamp(Source->SnowGaussianViewResponseStrength, 0.0f, 1.0f);
	const double DetailScale = FMath::Clamp((double)Source->SnowGaussianDetailScale, 0.40, 1.0);
	// 2.8.60: keep camera-independent baked geometry. This is a WORLD-space
	// footprint budget, never a camera-facing or distance-dependent billboard.
	const bool bMicroClusters = Source->bSnowUseMicroClusters && Source->SnowMicroClusterStrength > 0.005f;
	// Retain v2861 as an explicit A/B fallback. Resampling is a different
	// placement policy, not a larger number of children around each parent.
	const bool bSurfaceResampling = bReconstructedSnow && bMicroClusters && Source->bSnowSurfaceResampling;
	// Uncapped MEASUREMENT applies only to the independently resampled snow surface.
	// Finite source receiver candidates and integer array indices still bound any bake.
	const bool bSnowMeasureUncapped = bSurfaceResampling && Source->bSnowResampleMeasureUncapped;
	const double MicroStrength = bMicroClusters ? FMath::Clamp((double)Source->SnowMicroClusterStrength, 0.0, 1.0) : 0.0;
	const double MicroSigmaCapCm = FMath::Clamp((double)Source->SnowMicroMaxSigmaCm, 7.0, 28.0);
	const bool bCliffCompact2872 = bReconstructedSnow && Source->bSnowCliffCompact2872;
	const double CliffScale2872 = bCliffCompact2872 ?
		FMath::Clamp((double)Source->SnowCliffCompactScale2872,0.55,1.0) : 1.0;
	// The actual split child cap is substantially below the former 14cm parent.
	const double SnowSplitWorldCapCm = FMath::Min(9.6, MicroSigmaCapCm * 0.64);
	int32 SnowMicroAdded = 0;
	int32 SnowMicroEligible = 0;
	int32 SnowSplitParentsOmitted = 0;
	int32 SnowSplitChildrenFailed = 0;
	int32 SnowSplitSeedsWithoutPair = 0;
	int32 ResampleEligible = 0, ResampleEmitted = 0, ResampleBridges = 0;
	int32 ResampleCovered = 0, ResampleUnsupported = 0, ResampleUncovered = 0;
	int32 ResampleRefillTotal = 0, ResampleUncoveredFineTotal = 0;
	int32 ResampleOpticalUnmetCenters = 0, ResampleGeometricUnmetCenters = 0;
	int32 ResampleQuadSitesTotal = 0, ResampleUnmetQuadSitesTotal = 0;
	int32 ResampleUnusedSlotsTotal = 0;
	int32 ResampleRefillCandidateSitesTotal = 0;
	int32 ResampleCohesionBridgeCandidates = 0;
	int32 ResampleCohesionBridgesPlaced = 0;
	int32 SnowFineCandidates = 0;
	int32 SnowFinePlaced = 0;
	int32 SnowFineUnmet = 0;
	int32 SnowFinePasses = 0;
	int32 SnowFinishChanged = 0, SnowFinishSites = 0;
	int32 SnowFinishUnmetBefore = 0, SnowFinishUnmetAfter = 0;
	int32 SnowFinishFloor = 0, SnowFinishRoof = 0, SnowFinishStone = 0;
	int32 ResampleMeasuredRounds = 0;
	bool bResampleStalled = false;
	bool bResampleCapacityReached = false;
	double ResampleProjectionErrorSum = 0.0, ResampleNormalVariationSum = 0.0;
	double SnowSplitMaxWorldSigma = 0.0;
	const float DetailStrength = FMath::Clamp(Source->SnowGaussianDetailStrength, 0.10f, 1.0f);
	// The original GS (not this asset) is the broad snow blanket. Keep
	// extra splats low-opacity so they do not become glowing cotton balls.
	const float DetailOpacity = SnowOpacity * DetailStrength;
	// Actual blanket has priority. Feature stamps and subsurface volumes must
	// NEVER deplete its global budget or increase base splat footprints.

	// All three generated classes use the same world-space to local-space conversion;
	// the existing GS renderer still does the normal view-dependent projection.
	const auto BuildPatchBasis = [](const FVector& WorldN, const FVector& TangentHint,
		double TwistRadians, FVector& OutX, FVector& OutY, FVector& OutZ)
	{
		OutZ = WorldN.GetSafeNormal();
		FVector X = TangentHint - OutZ * FVector::DotProduct(TangentHint, OutZ);
		if (X.SizeSquared() < 1.0e-6)
		{
			const FVector Fallback = (FMath::Abs(OutZ.Z) < 0.92) ? FVector::UpVector : FVector::ForwardVector;
			X = FVector::CrossProduct(Fallback, OutZ);
		}
		X.Normalize();
		FVector Y = FVector::CrossProduct(OutZ, X).GetSafeNormal();
		const double C = FMath::Cos(TwistRadians);
		const double S = FMath::Sin(TwistRadians);
		const FVector RX = X * C + Y * S;
		const FVector RY = -X * S + Y * C;
		OutX = RX.GetSafeNormal();
		OutY = RY.GetSafeNormal();
	};

	int32 RealSourceShapeMatches = 0;
	const auto MakeSplat = [&](const FVector& WorldP, const FVector& WorldN, const FVector& TangentHint,
		double RadiusXcm, double RadiusYcm, double DepthCm, float Opacity, float Tone,
		double TwistRadians, FGaussianSplatData& Out) -> bool
	{
		// WORLD-space surface patch scale; adapt shape without altering sampling
		// intervals/feature influence radii. Reproject covariance at any MRQ view.
		RadiusXcm *= DetailScale;
		RadiusYcm *= DetailScale;
		const double MeanRadius = 0.5 * (RadiusXcm + RadiusYcm);
		const double MaxPlanarAspect = 1.85;
		if (RadiusXcm > RadiusYcm * MaxPlanarAspect) RadiusXcm = RadiusYcm * MaxPlanarAspect;
		if (RadiusYcm > RadiusXcm * MaxPlanarAspect) RadiusYcm = RadiusXcm * MaxPlanarAspect;
		const double MinDepth = FMath::Max(1.8, MeanRadius * 0.42);
		const double MaxDepth = FMath::Max(2.4, MeanRadius * 0.95);
		DepthCm = FMath::Clamp(DepthCm, MinDepth, MaxDepth);
		FVector SelectedTangent = TangentHint;
		double SelectedTwist = TwistRadians;
		const FIntVector SourceCell = XYZCell(WorldP, SupportCell);
		const FSourceShape* BestShape = nullptr;
		double BestShapeDist = FMath::Square(SupportRadius * 1.15);
		for (int32 DZ = -1; DZ <= 1; ++DZ)
		for (int32 DY = -1; DY <= 1; ++DY)
		for (int32 DX = -1; DX <= 1; ++DX)
		{
			if (const FSourceShape* Candidate = SourceShapeGrid.Find(SourceCell + FIntVector(DX, DY, DZ)))
			{
				const double Distance = FVector::DistSquared(Candidate->WorldPosition, WorldP);
				if (Distance < BestShapeDist) { BestShapeDist = Distance; BestShape = Candidate; }
			}
		}
		if (BestShape)
		{
			const FVector Sc = BestShape->Scale;
			const int32 MinAxis = Sc.X <= Sc.Y && Sc.X <= Sc.Z ? 0 : (Sc.Y <= Sc.Z ? 1 : 2);
			const int32 LongAxis = Sc.X >= Sc.Y && Sc.X >= Sc.Z ? 0 : (Sc.Y >= Sc.Z ? 1 : 2);
			const FVector WorldNormals[3] = {
				GSWorld.TransformVector(BestShape->Rotation.RotateVector(FVector::ForwardVector) * Sc.X).GetSafeNormal(),
				GSWorld.TransformVector(BestShape->Rotation.RotateVector(FVector::RightVector) * Sc.Y).GetSafeNormal(),
				GSWorld.TransformVector(BestShape->Rotation.RotateVector(FVector::UpVector) * Sc.Z).GetSafeNormal()};
			const double Alignment = FMath::Abs(FVector::DotProduct(WorldNormals[MinAxis], WorldN.GetSafeNormal()));
			if (Alignment >= 0.76 && LongAxis != MinAxis)
			{
				FVector Projected = WorldNormals[LongAxis] - WorldN * FVector::DotProduct(WorldNormals[LongAxis], WorldN);
				if (Projected.Normalize())
				{
					SelectedTangent = Projected;
					SelectedTwist *= 0.10; // trained shape carries the primary tangent orientation
					const int32 MidAxis = 3 - LongAxis - MinAxis;
					const double Long = (&Sc.X)[LongAxis];
					const double Mid = FMath::Max(1.0e-6, (&Sc.X)[MidAxis]);
					const double Ratio = FMath::Clamp(FMath::Sqrt(Long / Mid), 1.0, 1.38);
					RadiusXcm *= Ratio;
					RadiusYcm /= Ratio;
					++RealSourceShapeMatches;
				}
			}
		}
		const FVector P = GSWorld.InverseTransformPosition(WorldP);
		const FVector N = GSWorld.InverseTransformVectorNoScale(WorldN).GetSafeNormal();
		const FVector T = GSWorld.InverseTransformVectorNoScale(SelectedTangent).GetSafeNormal();
		if (P.ContainsNaN() || N.IsNearlyZero()) return false;
		FVector BasisX, BasisY, BasisZ;
		BuildPatchBasis(N, T.IsNearlyZero() ? FVector::ForwardVector : T, SelectedTwist, BasisX, BasisY, BasisZ);
		const FQuat Facing = FRotationMatrix::MakeFromXZ(BasisX, BasisZ).ToQuat();
		Out.Position = FVector3f(P);
		Out.Rotation = FQuat4f((float)Facing.X, (float)Facing.Y, (float)Facing.Z, (float)Facing.W);
		// X/Y/Z are true WORLD-space Gaussian covariance radii. Dividing by
		// MeanScale previously deformed the volume on non-uniformly scaled actors.
		const double AxisX = GSWorld.TransformVector(BasisX).Size();
		const double AxisY = GSWorld.TransformVector(BasisY).Size();
		const double AxisZ = GSWorld.TransformVector(BasisZ).Size();
		if (AxisX < 0.001 || AxisY < 0.001 || AxisZ < 0.001) return false;
		Out.Scale = FVector3f(
			(float)(FMath::Max(1.0, RadiusXcm) / AxisX),
			(float)(FMath::Max(1.0, RadiusYcm) / AxisY),
			(float)(FMath::Max(1.4, DepthCm) / AxisZ));
		Out.Opacity = FMath::Clamp(Opacity, 0.05f, 1.0f);
		// Keep a little highlight headroom; SH1 adds only a small view-facing
		// snow-backscatter approximation, not an artificial billboard rotation.
		// The camera never enters the bake: SH is evaluated by CalcViewData in each view.
		const float Headroom = 0.012f * ViewResponse;
		Out.SH_DC = FVector3f(
			(FMath::Clamp(SnowColor.R * Tone - Headroom, 0.0f, 1.0f) - 0.5f) / SHC0,
			(FMath::Clamp(SnowColor.G * Tone - Headroom, 0.0f, 1.0f) - 0.5f) / SHC0,
			(FMath::Clamp(SnowColor.B * Tone - Headroom, 0.0f, 1.0f) - 0.5f) / SHC0);
		// CalcViewData.usf: viewDirPLY=(-viewLocal.Y,+viewLocal.Z,-viewLocal.X),
		// and the engine's SH1 basis is SH_C1*(-y*SH[0]+z*SH[1]-x*SH[2]).
		// These coefficients therefore encode: Color += Amount * dot(viewLocal, normalLocal).
		// The effect is deliberately subtle, so physically diffuse snow remains diffuse.
		constexpr float SHC1 = 0.4886025119029199f;
		const float Amount = 0.060f * ViewResponse;
		const FVector3f Tint(Amount * 0.89f / SHC1, Amount * 0.96f / SHC1, Amount / SHC1);
		Out.SH[0] = Tint * (float)-N.Z;
		Out.SH[1] = Tint * (float)-N.X;
		Out.SH[2] = Tint * (float) N.Y;
		return true;
	};

	// World-space footprint limiter: preserve the source-aligned tangent frame,
	// reduce only generated snow covariance axes, never the source 3DGS PLY.
	// It is intentionally only called for the reconstructed floor/roof/crown
	// and their local gap patches. Narrow cliff ledges/rails keep their rules.
	const auto LimitSnowWorldSigma = [&GSWorld](FGaussianSplatData& G, double MaxSigmaCm)
	{
		const FQuat Q(double(G.Rotation.X), double(G.Rotation.Y),
			double(G.Rotation.Z), double(G.Rotation.W));
		const double WorldX = GSWorld.TransformVector(Q.RotateVector(FVector(double(G.Scale.X), 0.0, 0.0))).Size();
		const double WorldY = GSWorld.TransformVector(Q.RotateVector(FVector(0.0, double(G.Scale.Y), 0.0))).Size();
		if (WorldX > MaxSigmaCm && WorldX > 1.0e-5) G.Scale.X *= float(MaxSigmaCm / WorldX);
		if (WorldY > MaxSigmaCm && WorldY > 1.0e-5) G.Scale.Y *= float(MaxSigmaCm / WorldY);
	};

	// Gaussian sigma is a blending footprint, NOT the height of a mound. A
	// giant, sparse sigma produces visible white ovals; random large crowns also
	// create isolated balls. Keep a modest XY sigma and obtain continuity from
	// a denser, stable world-space lattice. A coherent noise field varies HEIGHT,
	// not individual primitive diameter.
	// Bake the footprint AFTER accounting for the user's scale multiplier.
	// A 26cm lattice with 0.60 detail scale used to become only 11cm sigma,
	// leaving separate white dots. Keep adjacent outdoor patches overlapping.
	const double SurfaceSigma = FMath::Clamp(
		EffectiveSpacing * 0.77 / FMath::Max(0.40, DetailScale), 9.0, 34.0);
	const double FeatureSigma = FMath::Clamp(WantedSpacing * 0.63, 3.5, 10.0);
	// The difference between splat sigma and pitch can no longer be hidden by
	// claiming overlap. A low-count global spray is explicitly disabled.
	const bool bCoverageLimited = EffectiveSpacing > SurfaceSigma * 2.35;
	const int32 MaxFeatureCount = bFeatures ? FMath::Max(1, FMath::FloorToInt(MaxSnowCount * 0.09)) : 0;
	int32 FeatureSplats = 0;
	const auto AddSnowAt = [&](int32 TriIndex, const FTriangle& T, double PX, double PY) -> bool
	{
		if (SnowSplats.Num() >= PrimaryBudget) return false;
		double PZ = 0.0;
		if (!HeightAtXY(T, PX, PY, PZ)) return false;
		++XYInsideSamples;
		const FVector ProxySurface(PX, PY, PZ);
		if (!HasSourceGS(ProxySurface)) { ++UnsupportedSamples; return false; }
		if (Source->bSnowGaussianSkyExposure && IsSheltered(TriIndex, ProxySurface)) { ++ShelteredSamples; return false; }
		FVector Surface = ProxySurface;
		FVector MeasuredNormal = T.Normal;
		if (FitSourceSurface(ProxySurface, T.Normal, Surface, MeasuredNormal)) ++SourceFitAccepted;
		else ++SourceFitFallback;
		// The same world-fixed drift mask is evaluated on the proxy receiver XY;
		// fine elevation fitting must not resample a different snow pattern.
		// Reject singleton white dots: keep only coherent local drifts with a
		// neighborhood of similarly snow-retaining positions in world space.
		const double D = DriftField(Surface);
		if (D < PatchThreshold) { ++DriftMaskRejects; return false; }
		// The field itself is spatially coherent; do not apply another unrelated
		// four-neighbour rejection that punches holes in legitimate snow patches.
		const FIntVector Key = XYZCell(Surface, FMath::Max(10.0, EffectiveSpacing * 0.52));
		if (UniqueSnowCells.Contains(Key)) { ++DuplicateSamples; return false; }

		const double Macro = double(FMath::PerlinNoise2D(FVector2D(PX / MoundScale, PY / MoundScale)));
		const double Broad = double(FMath::PerlinNoise2D(FVector2D(PX / (MoundScale * 2.7), PY / (MoundScale * 2.7))));
		const double ClusterField = 0.5 + 0.5 * double(FMath::PerlinNoise3D(Surface / FMath::Max(38.0, MoundScale * 0.52)));
		// Wind-smoothed deposits have coherent, spatially varying thickness.
		// No independent coin-flip that makes an entire 90-cm isolated clump.
		const double DriftFill = FMath::Clamp((DriftField(Surface) - PatchThreshold + 0.11) / 0.26, 0.0, 1.0);
		const double Height = FMath::Max(1.4, BaseThickness * (0.56 + 0.44 * DriftFill) +
			MoundCm * (0.62 * Broad + 0.38 * Macro));
		const float Tone = FMath::Clamp(0.965f + float(Macro * 0.016 + Broad * 0.008), 0.91f, 1.0f);

		FVector TriTangentX = (T.B - T.A).GetSafeNormal();
		TriTangentX = (TriTangentX - T.Normal * FVector::DotProduct(TriTangentX, T.Normal)).GetSafeNormal();
		if (TriTangentX.IsNearlyZero())
			TriTangentX = FVector::CrossProduct((FMath::Abs(T.Normal.Z) < 0.95) ? FVector::UpVector : FVector::ForwardVector, T.Normal).GetSafeNormal();
		const FVector TriTangentY = FVector::CrossProduct(T.Normal, TriTangentX).GetSafeNormal();
		const double Probe = FMath::Max(10.0, SurfaceSigma * 0.82);
		const auto NoiseField = [&](const FVector& W) -> double
		{
			return 0.68 * double(FMath::PerlinNoise2D(FVector2D(W.X / MoundScale, W.Y / MoundScale))) +
				0.32 * double(FMath::PerlinNoise2D(FVector2D(W.X / (MoundScale * 0.72), W.Y / (MoundScale * 0.72))));
		};
		const double GradX = NoiseField(Surface + TriTangentX * Probe) - NoiseField(Surface - TriTangentX * Probe);
		const double GradY = NoiseField(Surface + TriTangentY * Probe) - NoiseField(Surface - TriTangentY * Probe);
		FVector FlowDir = (TriTangentX * GradX + TriTangentY * GradY);
		if (!FlowDir.Normalize())
		{
			const uint32 FallbackHash = CellHash(FMath::FloorToInt(PX / EffectiveSpacing), FMath::FloorToInt(PY / EffectiveSpacing));
			const double A = 2.0 * PI * double(FallbackHash & 65535u) / 65535.0;
			FlowDir = (TriTangentX * FMath::Cos(A) + TriTangentY * FMath::Sin(A)).GetSafeNormal();
		}
		FVector AcrossDir = FVector::CrossProduct(T.Normal, FlowDir).GetSafeNormal();
		if (AcrossDir.IsNearlyZero()) AcrossDir = TriTangentY;
		const double SlopeBias = FMath::Clamp(1.0 - double(T.Up), 0.0, 1.0);
		const double NoiseStrength = FMath::Clamp(FMath::Abs(GradX) + FMath::Abs(GradY), 0.0, 1.0);
		FVector SurfaceNormal = (
			FVector::UpVector * (0.25 + 0.18 * double(T.Up)) +
			MeasuredNormal * (0.90 - 0.08 * SlopeBias) +
			FlowDir * (0.09 * GradX) +
			AcrossDir * (0.09 * GradY)).GetSafeNormal();
		if (SurfaceNormal.IsNearlyZero()) SurfaceNormal = FVector::UpVector;

		const uint32 Hash = CellHash(FMath::FloorToInt(PX / EffectiveSpacing), FMath::FloorToInt(PY / EffectiveSpacing));
		const double Twist = (double((Hash >> 8u) & 65535u) / 65535.0 - 0.5) * PI * (0.85 + 0.35 * SlopeBias + 0.25 * NoiseStrength);
		// Coherent blanket first; restrained anisotropy for drift directions.
		// Large ellipses aligned across roof tiles read as repeated white ribbons.
		const double Elongation = FMath::Clamp(1.00 + 0.20 * NoiseStrength + 0.13 * SlopeBias, 1.0, 1.38);
		const double BaseRadiusX = SurfaceSigma * Elongation;
		const double BaseRadiusY = SurfaceSigma * FMath::Clamp(0.96 - 0.11 * NoiseStrength, 0.80, 1.00);
		const double Depth = FMath::Max(1.6, Height * (0.78 + 0.36 * VolumeStrength + 0.10 * NoiseStrength));

		FGaussianSplatData Splat;
		if (!MakeSplat(Surface + FVector::UpVector * (Height * 0.54), SurfaceNormal, FlowDir,
			BaseRadiusX, BaseRadiusY, Depth, DetailOpacity, Tone, Twist, Splat)) return false;
		UniqueSnowCells.Add(Key);
		SnowSplats.Add(Splat);
		// Report actual baked sigma after radius and depth clamps.
		SumTopXY += 0.5 * (BaseRadiusX + BaseRadiusY) * DetailScale;
		SumTopZ += FMath::Min(Depth, FMath::Max(1.4, FMath::Max(BaseRadiusX, BaseRadiusY) * DetailScale * 0.86));
		++TopSamples;

		// Small clustered shoulder splats form a continuous layer and break the
		// regular dotted lattice. They are low-opacity and tangent-offset, not
		// tall random crowns.
		const int32 ExtraCount = (ClusterField > 0.72 ? 2 : (ClusterField > 0.48 ? 1 : 0));
		for (int32 ExtraIndex = 0; ExtraIndex < ExtraCount && PatchShoulders.Num() < ShoulderBudget; ++ExtraIndex)
		{
			const double SignA = ((Hash >> (ExtraIndex * 5 + 2)) & 1u) ? 1.0 : -1.0;
			const double SignB = ((Hash >> (ExtraIndex * 5 + 3)) & 1u) ? 1.0 : -1.0;
			const double OffsetA = (0.34 + 0.18 * ExtraIndex) * BaseRadiusX * SignA;
			const double OffsetB = (0.22 + 0.16 * ExtraIndex) * BaseRadiusY * SignB;
			const FVector Offset = FlowDir * OffsetA + AcrossDir * OffsetB + FVector::UpVector * (Height * (0.05 + 0.04 * ExtraIndex));
			FGaussianSplatData Shoulder;
			if (MakeSplat(Surface + Offset + FVector::UpVector * (Height * 0.32), SurfaceNormal, AcrossDir,
				BaseRadiusX * (0.78 - 0.07 * ExtraIndex), BaseRadiusY * (0.82 - 0.05 * ExtraIndex),
				Depth * (0.86 - 0.08 * ExtraIndex), DetailOpacity * (0.68f - 0.08f * ExtraIndex),
				Tone * (0.992f - 0.006f * ExtraIndex), -Twist * (0.62 + 0.18 * ExtraIndex), Shoulder))
			{
				PatchShoulders.Add(Shoulder);
			}
		}

		// A minority of small, LOW-opacity Gaussians forms subsurface thickness.
		// No random tall, bright upper crowns, which were responsible for blobs.
		const double Choice = double(Hash & 65535u) / 65535.0;
		if (VolumeStrength > 0.01 && Choice < VolumeStrength * 0.27 &&
			OptionalVolume.Num() < SurfaceBudget / 4)
		{
			FGaussianSplatData Body;
			const double BodyRadiusX = BaseRadiusX * 0.70;
			const double BodyRadiusY = BaseRadiusY * 0.74;
			const double BodyDepth = FMath::Max(1.6, Height * 0.72);
			if (MakeSplat(Surface + FVector::UpVector * (Height * 0.18), FVector::UpVector, FlowDir,
				BodyRadiusX, BodyRadiusY, BodyDepth,
				DetailOpacity * (0.15f + float(VolumeStrength) * 0.14f),
				Tone * 0.985f, Twist * 0.33, Body))
			{
				OptionalVolume.Add(Body);
				OptionalVolumeKinds.Add(1u);
			}
		}
		return true;
	};

int32 MainDriftCount = 0, CommittedShoulders = 0, BaseSnowCount = 0;
	int32 VolumeCount = 0;
	bool bVolumeBudgetHit = false;
	if (!bSourceDrivenSnow)
	{
for (int32 TriIndex = 0; TriIndex < Triangles.Num() && SnowSplats.Num() < PrimaryBudget; ++TriIndex)
	{
		const FTriangle& T = Triangles[TriIndex];
		if (!T.bValid || T.Up < UpLimit) continue;
		const double MinX = FMath::Min3(T.A.X, T.B.X, T.C.X);
		const double MinY = FMath::Min3(T.A.Y, T.B.Y, T.C.Y);
		const double MaxX = FMath::Max3(T.A.X, T.B.X, T.C.X);
		const double MaxY = FMath::Max3(T.A.Y, T.B.Y, T.C.Y);
		const int32 X0 = FMath::FloorToInt(MinX / EffectiveSpacing);
		const int32 X1 = FMath::FloorToInt(MaxX / EffectiveSpacing);
		const int32 Y0 = FMath::FloorToInt(MinY / EffectiveSpacing);
		const int32 Y1 = FMath::FloorToInt(MaxY / EffectiveSpacing);
		const int64 CandidateCount = (int64(X1) - X0 + 1) * (int64(Y1) - Y0 + 1);
		if (CandidateCount <= 0) continue;
		// Respect pathological proxy bounds without dropping huge surfaces.
		const int32 Step = FMath::Max(1, FMath::CeilToInt(FMath::Sqrt(double(CandidateCount) / 850000.0)));
		if (Step > 1) ++HugeTriangleSkips;
		for (int32 Y = Y0; Y <= Y1 && SnowSplats.Num() < PrimaryBudget; Y += Step)
		for (int32 X = X0; X <= X1 && SnowSplats.Num() < PrimaryBudget; X += Step)
		{
			const uint32 Hash = CellHash(X, Y);
			// Hashed, world-anchored stratified jitter breaks fixed grid/roof stripes.
			// Max +/-0.25 cell retains blanket coverage at existing sigma/spacings.
			// All views (including panoramic MRQ faces) sample identical positions.
			const double JX = (double(Hash & 65535u) / 65535.0 - 0.5) * 0.66;
			const double JY = (double((Hash >> 16u) & 65535u) / 65535.0 - 0.5) * 0.66;
			AddSnowAt(TriIndex, T, (double(X) + 0.5 + JX) * EffectiveSpacing,
				(double(Y) + 0.5 + JY) * EffectiveSpacing);
		}
	}
	// Previous versions unconditionally stamped ONE splat at each triangle's
	// centroid when the grid found none. On dense cliff proxies that created
	// thousands of isolated bright dots despite patch noise and jitter.
	MainDriftCount = SnowSplats.Num();
	CommittedShoulders = FMath::Min(PatchShoulders.Num(),
		FMath::Max(0, SurfaceBudget - SnowSplats.Num()));
	for (int32 Index = 0; Index < CommittedShoulders; ++Index)
		SnowSplats.Add(PatchShoulders[Index]);
	BaseSnowCount = SnowSplats.Num();

	if (bFeatures && SnowSplats.Num() < MaxSnowCount)
	{
		const double FeatureCellCm = FMath::Max(6.0, WantedSpacing * 0.82);
		const double BlanketCellCm = FMath::Max(10.0, EffectiveSpacing * 0.52);
		const auto AlreadyCoveredByBlanket = [&](const FVector& P) -> bool
		{
			// Inspect accepted base layer cells, rather than distance to an arbitrary
			// proxy polygon. Otherwise dense roof snow gets a second bright edge ribbon.
			const FIntVector C = XYZCell(P, BlanketCellCm);
			for (int32 DZ = -1; DZ <= 1; ++DZ)
			for (int32 DY = -1; DY <= 1; ++DY)
			for (int32 DX = -1; DX <= 1; ++DX)
			{
				if (UniqueSnowCells.Contains(C + FIntVector(DX, DY, DZ))) return true;
			}
			return false;
		};
		const auto AddFeature = [&](const FVector& P, double RadiusCm, double DepthCm,
			double LiftCm, float Opacity, bool bTip, const FVector& Inward) -> int32
		{
			if (SnowSplats.Num() >= MaxSnowCount || FeatureSplats >= MaxFeatureCount) return 0;
			// Even the ledge/cap pass needs a coherent accumulation neighborhood.
			// A narrow vertical facet alone must not make an isolated bright stamp.
			if (DriftField(P) < PatchThreshold - (bTip ? 0.03 : 0.00)) return 0;
			if (!HasSourceGS(P)) { ++FeatureGSRejects; return 0; }
			if (Source->bSnowGaussianSkyExposure && IsSheltered(-1, P)) { ++FeatureShelterRejects; return 0; }
			FVector ActualAnchor = P;
			FVector ActualNormal = FVector::UpVector;
			if (FitSourceSurface(P, FVector::UpVector, ActualAnchor, ActualNormal)) ++SourceFitAccepted;
			else ++SourceFitFallback;
			// Existing broad snow already fills this region. Do not stamp a bright
			// duplicate row over the roof crown or the upper edge of a hillside.
			if (!bTip && AlreadyCoveredByBlanket(ActualAnchor)) return 0;
			if (!AlreadyCoveredByBlanket(P))
			{
				// Prevent a singleton Gaussian from appearing as a lit white spot
				// on a black cliff: a cap/ledge needs a supported contiguous shoulder.
				const double R = FMath::Max(12.0, FeatureSigma * 1.8);
				int32 Neighbors = 0;
				for (int32 I = 0; I < 4; ++I)
				{
					const double A = PI * 0.5 * double(I);
					const FVector Neighbor = P + FVector(FMath::Cos(A) * R, FMath::Sin(A) * R, 0.0);
					if (HasSourceGS(Neighbor) && (!Source->bSnowGaussianSkyExposure || !IsSheltered(-1, Neighbor)))
						++Neighbors;
				}
				if (Neighbors < (bTip ? 2 : 3)) return 0;
			}
			const FIntVector Key = XYZCell(ActualAnchor, FeatureCellCm);
			if (UniqueFeatureCells.Contains(Key)) return 0;
			UniqueFeatureCells.Add(Key);
			// The UI radius denotes the MAX patch area, NOT a Gaussian sigma.
			// Features have a small, physically plausible cross-section even when
			// the user asks for a large 80-90 cm influence region.
			const double Patch = FMath::Min3(RadiusCm, SupportRadius * 0.34,
				FMath::Max(11.0, WantedSpacing * (bTip ? 1.55 : 1.35)));
			const double MicroSigma = FMath::Min(FeatureSigma, FMath::Max(3.0, Patch * 0.62));
			const double SnowDepth = FMath::Max(1.2, FMath::Min3(DepthCm,
				BaseThickness * (0.38 + 0.20 * VolumeStrength), MicroSigma * 0.92));
			const double Noise = double(FMath::PerlinNoise2D(FVector2D(P.X / MoundScale, P.Y / MoundScale)));
			const FVector Center = ActualAnchor + FVector::UpVector * (FMath::Max(0.8, LiftCm * 0.62) + Noise * FMath::Min(MoundCm * 0.3, 2.0));
			int32 Added = 0;
			const auto Put = [&](const FVector& At, float Alpha, double Sigma) -> bool
			{
				if (FeatureSplats >= MaxFeatureCount || SnowSplats.Num() >= MaxSnowCount) return false;
				FGaussianSplatData One;
				FVector FeatureNormal = (ActualNormal * (bTip ? 0.87 : 0.79) +
					FVector::UpVector * 0.15 - Inward.GetSafeNormal() * (bTip ? 0.0 : 0.11)).GetSafeNormal();
				FVector FeatureTangent = bTip ? FVector::CrossProduct(FeatureNormal, FVector::RightVector).GetSafeNormal() : FVector::CrossProduct(FeatureNormal, Inward.GetSafeNormal()).GetSafeNormal();
				if (FeatureTangent.IsNearlyZero()) FeatureTangent = FVector::CrossProduct((FMath::Abs(FeatureNormal.Z) < 0.95) ? FVector::UpVector : FVector::ForwardVector, FeatureNormal).GetSafeNormal();
				if (!MakeSplat(At, FeatureNormal, FeatureTangent, Sigma * (bTip ? 1.06 : 1.18), Sigma * (bTip ? 0.94 : 0.78), SnowDepth,
					Alpha, 0.965f + float(Noise * 0.015), 0.0, One)) return false;
				SnowSplats.Add(One);
				++FeatureSplats;
				++Added;
				return true;
			};
			Put(Center, Opacity * 0.70f, MicroSigma);
			// A crown can occupy a small area, but never a single massive oval.
			// Place modest satellites around a *supported* apex; an 80-cm user
			// radius cannot turn a 10-cm rock tip into a metre-wide floating disk.
			if (bTip && Patch > MicroSigma * 1.2 && FeatureSplats + 6 <= MaxFeatureCount)
			{
				const uint32 H = CellHash(FMath::FloorToInt(P.X / 7.0), FMath::FloorToInt(P.Y / 7.0));
				const double Phase = 2.0 * PI * double(H & 65535u) / 65535.0;
				for (int32 I = 0; I < 6; ++I)
				{
					// Golden-angle scatter with varying radius/height: no 6-dot halo.
					const double A = Phase + double(I) * 2.399963229728653;
					const double R = Patch * 0.72 * FMath::Sqrt((double(I) + 0.45) / 6.5);
					const FVector Offset(FMath::Cos(A) * R, FMath::Sin(A) * R,
						-BaseThickness * (0.13 + 0.11 * double((H >> (I * 4)) & 7u) / 7.0));
					if (HasSourceGS(P + Offset) && !AlreadyCoveredByBlanket(P + Offset))
						Put(Center + Offset, Opacity * 0.20f, MicroSigma * 0.86);
				}
			}
			// Ledges retain narrow, supported inland accumulation instead of
			// making a 90-cm white oval over the vertical cliff face.
			if (!bTip && !Inward.IsNearlyZero() && Patch >= MicroSigma * 1.8)
			{
				const double MaxInset = FMath::Min(Patch * 0.70, MicroSigma * 1.5);
				const FVector Offset = Inward.GetSafeNormal() * MaxInset;
				if (HasSourceGS(P + Offset) &&
					(!Source->bSnowGaussianSkyExposure || !IsSheltered(-1, P + Offset)))
					Put(Center + Offset + FVector::UpVector * (BaseThickness * -0.10),
						Opacity * 0.30f, MicroSigma * 0.88);
			}
			return Added;
		};

		// A narrow spire has virtually zero horizontal projected triangle area.
		// Choose an exposed *local maximum* from welded proxy vertices, requiring
		// nearby lower geometry and steep incident faces (no snow on flat roof centers).
		const double PeakSearch = FMath::Max3(75.0, PeakRadius * 3.0, EffectiveSpacing * 1.5);
		TMap<FIntPoint, TArray<int32>> PeakBuckets;
		PeakBuckets.Reserve(WeldedVertices.Num());
		for (int32 I = 0; I < WeldedVertices.Num(); ++I)
			PeakBuckets.FindOrAdd(XYCell(WeldedVertices[I].Position, PeakSearch)).Add(I);
		const double MinDrop = FMath::Max(16.0, BaseThickness * 2.5);
		const int32 MaxTipGroups = FMath::Clamp(SurfaceBudget / 85, 32, 500);
		int32 TipGroups = 0;
		TSet<FIntVector> AcceptedTipGroups;
		for (int32 I = 0; I < WeldedVertices.Num() &&
			SnowSplats.Num() < MaxSnowCount && FeatureSplats < MaxFeatureCount; ++I)
		{
			const FWeldVertex& V = WeldedVertices[I];
			if (TipGroups >= MaxTipGroups) break;
			if (V.IncidentFaces < 2 || V.MinUp > 0.38 ||
				V.MaxUp < FMath::Max(0.40, UpLimit * 0.80) || V.Position.ContainsNaN()) continue;
			const FVector& P = V.Position;
			const FIntPoint C = XYCell(P, PeakSearch);
			bool bHigher = false;
			bool bLower = V.MinNeighborZ < P.Z - MinDrop;
			for (int32 DY = -1; DY <= 1 && !bHigher; ++DY)
			for (int32 DX = -1; DX <= 1 && !bHigher; ++DX)
			{
				if (const TArray<int32>* Nearby = PeakBuckets.Find(FIntPoint(C.X + DX, C.Y + DY)))
				{
					for (const int32 J : *Nearby)
					{
						if (J == I) continue;
						const FVector& Other = WeldedVertices[J].Position;
						const double XYDistSq = FVector2D(P.X - Other.X, P.Y - Other.Y).SizeSquared();
						if (XYDistSq > PeakSearch * PeakSearch) continue;
						if (Other.Z > P.Z + 1.0 || (FMath::Abs(Other.Z - P.Z) <= 1.0 && J < I))
						{ bHigher = true; break; }
						if (Other.Z < P.Z - MinDrop) bLower = true;
					}
				}
			}
			if (bHigher || !bLower) continue;
			const FIntVector TipKey = XYZCell(P, FMath::Max(95.0, PeakRadius * 2.2));
			if (AcceptedTipGroups.Contains(TipKey)) continue;
			++CandidateTips;
			const int32 Added = AddFeature(P, PeakRadius,
				FMath::Max(2.5, BaseThickness * (0.45 + 0.18 * FeatureStrength)),
				BaseThickness * 0.44, DetailOpacity * 0.67f, true, FVector::ZeroVector);
			if (Added > 0)
			{
				TipCount += Added;
				AcceptedTipGroups.Add(TipKey);
				++TipGroups;
			}
		}

		// Estimate a globally even edge budget. Otherwise early index-order edges
		// consume all detail splats and later cliffs receive nothing.
		double RequestedFeatureSamples = 0.0;
		const double BaseEdgeSpacing = FMath::Clamp(WantedSpacing /
			FMath::Sqrt(FMath::Max(0.35, FeatureStrength)), 6.5, 17.0);
		for (const int32 EdgeID : FeatureEdgeIndices)
		{
			const FProxyEdge& E = ProxyEdges[EdgeID];
			const double Len = (WeldedVertices[E.A].Position - WeldedVertices[E.B].Position).Size();
			RequestedFeatureSamples += FMath::Clamp(FMath::CeilToInt(Len / BaseEdgeSpacing), 1, 512) * 1.8;
		}
		const double FeatureSpacingMultiplier = FMath::Max(1.0,
			RequestedFeatureSamples / FMath::Max(1.0, double(MaxFeatureCount - FeatureSplats)));

		// Horizontal cliff crowns, steps and fracture shelves. The samples live on
		// *actual* mesh edges, not along a camera silhouette. Pure vertical walls
		// with no exposed top lip / slope break never enter this pass.
		for (const int32 EdgeID : FeatureEdgeIndices)
		{
			if (SnowSplats.Num() >= MaxSnowCount || FeatureSplats >= MaxFeatureCount) break;
			const FProxyEdge& E = ProxyEdges[EdgeID];
			const FVector& A = WeldedVertices[E.A].Position;
			const FVector& B = WeldedVertices[E.B].Position;
			const double Radius = E.bTopLip ? LedgeRadius : LedgeRadius * 0.68;
			// Sample a ridge as a sequence of narrow patches. Never stretch one
			// Gaussian across the user-controlled (up to 80 cm) ledge extent.
			const double Spacing = BaseEdgeSpacing * FeatureSpacingMultiplier;
			const int32 Samples = FMath::Clamp(FMath::CeilToInt((B - A).Size() / Spacing), 1, 512);
			for (int32 K = 0; K < Samples &&
				SnowSplats.Num() < MaxSnowCount && FeatureSplats < MaxFeatureCount; ++K)
			{
				const double T = (double(K) + 0.5) / Samples;
				const FVector EdgeP = FMath::Lerp(A, B, T);
				// Snow sticks in spatially coherent, irregular pockets. Uniform stamps
				// along every eligible mesh edge were creating long neon-white ribbons.
				const double Coarse = FMath::PerlinNoise3D(EdgeP / FMath::Max(42.0, Radius * 3.1));
				const double Fine = FMath::PerlinNoise3D(EdgeP / FMath::Max(17.0, Radius * 1.25));
				const double PatchCoverage = FMath::Clamp(0.46 + 0.78 * Coarse + 0.26 * Fine, 0.0, 1.0);
				if (PatchCoverage < (E.bTopLip ? 0.57 : 0.64)) continue;
				const uint32 JitterHash = CellHash(FMath::FloorToInt(EdgeP.X / 9.0),
					FMath::FloorToInt(EdgeP.Y / 9.0) ^ (K * 131));
				// No continuous edge-following underline: only some supported, noisy
				// stretches retain deposits, with extra stochastic gaps between clumps.
				const double EdgeAcceptance = 0.12 + 0.43 * PatchCoverage;
				const double EdgeChance = double((JitterHash >> 16u) & 65535u) / 65535.0;
				if (EdgeChance > EdgeAcceptance) continue;
				const double Along = (double(JitterHash & 65535u) / 65535.0 - 0.5) * Spacing * 0.58;
				const FVector ScatterP = EdgeP + (B - A).GetSafeNormal() * Along;
				const double Depth = BaseThickness * (E.bTopLip ? 0.43 : 0.31);
				const double Lift = BaseThickness * (E.bTopLip ? 0.43 : 0.28);
				const FTriangle& First = Triangles[E.FirstFace];
				const FTriangle& Second = Triangles[E.SecondFace >= 0 ? E.SecondFace : E.FirstFace];
				const FTriangle& UpFace = First.Up >= Second.Up ? First : Second;
				FVector Inward = (UpFace.A + UpFace.B + UpFace.C) / 3.0 - ScatterP;
				Inward.Z = 0.0;
				const double Inset = FMath::Min(Radius * 0.40, FeatureSigma * 1.4) *
					(0.24 + 0.58 * double((JitterHash >> 16u) & 65535u) / 65535.0);
				const FVector P = ScatterP + Inward.GetSafeNormal() * Inset;
				const int32 Added = AddFeature(P, Radius, Depth, Lift,
					DetailOpacity * (E.bTopLip ? 0.41f : 0.28f) * float(PatchCoverage), false, Inward);
				if (E.bTopLip) LipCount += Added;
				else ShelfCount += Added;
			}
		}
	}
	// Merge saved sparse lower-body centers into the independent snow asset;
	// the overall Gaussian cap includes ALL shell, feature and volume splats.
	VolumeCount = FMath::Min(OptionalVolume.Num(), MaxSnowCount - SnowSplats.Num());
	for (int32 I = 0; I < VolumeCount; ++I) SnowSplats.Add(OptionalVolume[I]);
	bVolumeBudgetHit = VolumeCount < OptionalVolume.Num();
	// Count only the candidates that actually entered the asset after the cap.
	for (int32 I = 0; I < VolumeCount; ++I)
	{
		if (OptionalVolumeKinds[I] == 1u) ++MadeLower;
		else if (OptionalVolumeKinds[I] == 2u) ++MadeUpper;
	}

	} // End of legacy proxy-seeded Gaussian generator (never used by v2846).

	// Source-first topology measures physical surfaces; no camera rays and no
	// speculative proxy extrusions. Neighbor searches distinguish roof, cliff and
	// thin elevated rails even when the proxy has no rail mesh.
	const auto SameSnowLayer = [&SourceSurfaceGrid, SourceDetailCellCm](const FIntVector& At,
		const FVector& P, const FVector& N, bool bRequireUp) -> bool
	{
		for (int32 DZ = -1; DZ <= 1; ++DZ)
		{
			const FSourceSurfaceCell* Other = SourceSurfaceGrid.Find(At + FIntVector(0, 0, DZ));
			if (!Other || Other->Samples < 1) continue;
			const FVector OtherN = Other->NormalSum.GetSafeNormal();
			if ((bRequireUp && OtherN.Z < 0.58) || FVector::DotProduct(OtherN, N) < 0.64) continue;
			if (FMath::Abs(Other->PositionSum.Z - P.Z) <= SourceDetailCellCm * 1.48) return true;
		}
		return false;
	};
	struct FSourceLandformEvidence
	{
		int32 RailRun = 0;
		int32 RailFlanks = 0;
		FVector RailAxis = FVector::ForwardVector;
		int32 VerticalRock = 0;
		double Lee = 0.0;
		double Concavity = 0.0;
		bool bRail = false;
		bool bCliffLedge = false;
	};
	const auto MeasureLandform = [&](const FIntVector& C, const FVector& P, const FVector& N,
		bool bFindFineFeatures) -> FSourceLandformEvidence
	{
		FSourceLandformEvidence E;
		// Concavity is the relative elevation of nearby supported terrain. A
		// smooth height field, rather than per-point randomness, controls deposits.
		double SumHigher = 0.0; int32 NearbyUp = 0;
		for (int32 DY = -1; DY <= 1; ++DY)
		for (int32 DX = -1; DX <= 1; ++DX)
		{
			if (!(DX || DY)) continue;
			for (int32 DZ = -2; DZ <= 2; ++DZ)
			{
				const FSourceSurfaceCell* Other = SourceSurfaceGrid.Find(C + FIntVector(DX, DY, DZ));
				if (!Other || Other->Samples < 1 || Other->NormalSum.Z < 0.43) continue;
				if (FMath::Abs(Other->PositionSum.Z - P.Z) > SourceDetailCellCm * 1.55) continue;
				SumHigher += FMath::Clamp((Other->PositionSum.Z - P.Z) / SourceDetailCellCm, -1.0, 1.0);
				++NearbyUp; break;
			}
		}
		E.Concavity = NearbyUp ? FMath::Clamp(SumHigher / double(NearbyUp), -1.0, 1.0) : 0.0;
		// Search the upwind column for an ACTUAL original-GS obstacle above
		// this layer. Its leeward drift is enhanced; an exposed upwind crest erodes.
		if (WindStrength > 0.005)
		{
			for (int32 Step = 2; Step <= 5 && E.Lee < 0.9; ++Step)
			{
				const FVector UpwindP = P - WindTo * (double(Step) * SourceDetailCellCm);
				const FIntVector UpwindC = XYZCell(UpwindP, SourceDetailCellCm);
				for (int32 DZ = 1; DZ <= 6; ++DZ)
				for (int32 Lateral = -1; Lateral <= 1; ++Lateral)
				{
					const FVector Side(-WindTo.Y * Lateral * SourceDetailCellCm, WindTo.X * Lateral * SourceDetailCellCm, 0.0);
					const FIntVector Q = XYZCell(UpwindP + Side + FVector::UpVector * (DZ * SourceDetailCellCm), SourceDetailCellCm);
					const FSourceSurfaceCell* H = SourceSurfaceGrid.Find(Q);
					if (!H || H->Samples < 1 || H->PositionSum.Z - P.Z < 25.0) continue;
					E.Lee = FMath::Max(E.Lee, FMath::Clamp((H->PositionSum.Z - P.Z) / 130.0, 0.0, 1.0) * (1.0 - Step * 0.065));
				}
			}
		}
		if (!bFindFineFeatures) return E;
		// A railing is a long, NARROW top-supported line, not a line inferred
		// from regular proxy triangle borders. Support across the side of the line
		// must be low; otherwise this is a roof/ground patch, not a rail.
		if (bRailSnow && N.Z > 0.63)
		{
			const FIntPoint Directions[] = { FIntPoint(1,0), FIntPoint(0,1), FIntPoint(1,1), FIntPoint(1,-1) };
			for (const FIntPoint& D : Directions)
			{
				int32 Run = 0;
				for (int32 Sign = -1; Sign <= 1; Sign += 2)
				for (int32 Step = 1; Step <= 3; ++Step)
				{
					if (SameSnowLayer(C + FIntVector(D.X * Step * Sign, D.Y * Step * Sign, 0), P, N, true)) ++Run;
				}
				const FIntPoint Across(-D.Y, D.X);
				int32 Flanks = 0;
				for (int32 Sign = -1; Sign <= 1; Sign += 2)
				for (int32 Step = 1; Step <= 2; ++Step)
				{
					if (SameSnowLayer(C + FIntVector(Across.X * Step * Sign, Across.Y * Step * Sign, 0), P, N, true)) ++Flanks;
				}
				if (Run > E.RailRun || (Run == E.RailRun && Flanks < E.RailFlanks))
				{
					E.RailRun = Run; E.RailFlanks = Flanks;
					E.RailAxis = FVector(double(D.X), double(D.Y), 0.0).GetSafeNormal();
				}
			}
			// Actual source support DOWN the post / rail at several heights.
			// Prevent floating snow atop a detached leaf or scanning outlier.
			bool bUnderSupport = false;
			for (int32 Depth = 35; Depth <= 140 && !bUnderSupport; Depth += 35)
				bUnderSupport = HasSourceGS(P - FVector::UpVector * Depth);
			E.bRail = E.RailRun >= 4 && E.RailFlanks <= 1 && bUnderSupport;
		}
		// Cliff shelf = upward local patch supported by one or more genuine
		// steep cliff cells directly below, with lateral upper support. An isolated
		// wall Gaussian cannot become a bright white dot by itself.
		if (N.Z > 0.49)
		{
			for (int32 DY = -1; DY <= 1; ++DY)
			for (int32 DX = -1; DX <= 1; ++DX)
			for (int32 DZ = -1; DZ >= -4; --DZ)
			{
				const FSourceSurfaceCell* Below = SourceSurfaceGrid.Find(C + FIntVector(DX, DY, DZ));
				if (Below && Below->Samples >= 1 && Below->NormalSum.Z < 0.43 &&
					Below->PositionSum.Z < P.Z - SourceDetailCellCm * 0.65)
					++E.VerticalRock;
			}
			E.bCliffLedge = E.VerticalRock >= 2 && NearbyUp >= 2;
		}
		return E;
	};
	int32 SnowRailCount = 0, SnowCliffShelfCount = 0, WindShadowCandidates = 0;
	int32 CliffWallRejected = 0, RailCandidates = 0;
	int32 SourceCandidateCount = 0;
	int32 SourceProxyAccepted = 0;
	int32 SourceOnlyAccepted = 0;
	int32 SourceIsolatedRejected = 0;
	int32 SourceOccludedRejected = 0;
	int32 SourceGradientRejected = 0;
	int32 SourceSurfaceSnowCount = 0;
	int32 SparseFloorSourceAccepted = 0;

	// v2849: Reconstruct the actual snow surface BEFORE sampling its rendering
	// Gaussian representation. This is a separate algorithm, not a new step in
	// the v2848 candidate -> generated splats -> nearest splat mask pipeline.
	struct FContinuousSnowSurface
	{
		FVector P = FVector::ZeroVector; // MLS-fitted original surface, WORLD cm
		FVector N = FVector::UpVector;
		float Coverage = 0.0f;           // Independent of any generated Gaussian
		float HeightCm = 0.0f;
		uint8 Kind = 0;                 // 0=ordinary roof, 1=rail, 2=cliff shelf, 3=broad floor, 4=rounded stone crown
		bool bInferredGap = false;
		bool bProxyVerified = false;
	};
	TMap<FIntVector, FContinuousSnowSurface> ReconstructedSnowField;
	// Must outlive the reconstruction branch: snow LOD is built later.
	TSet<FIntVector> SplitSeedCells;
	int32 ReconstructionRawNodes = 0;
	int32 ReconstructionBridges = 0;
	int32 ReconstructionProxyRejects = 0;
	int32 ReconstructionSkyRejects = 0;
	int32 ReconstructionSmoothNodes = 0;
	int32 ReconstructionOutputNodes = 0;
	int32 ReconstructionGroupedNodes = 0;
	int32 ReconstructionRailCount = 0;
	int32 ReconstructionShelfCount = 0;
	int32 ReconstructionCliffRejected = 0;
	int32 ReconstructionUnsupportedShelves = 0;
	int32 ReconstructionSlopeRejected = 0;
	int32 ReconstructionMaskNormalRejects = 0;
	int32 ReconstructionCrownCount = 0;
	int32 ReconstructionCrownRejected = 0;
	int32 ReconstructionFloorCount = 0;
	int32 ReconstructionShelfMicroCount = 0;
	int32 SnowCliffCompactMain2872 = 0, SnowCliffCompactMicro2872 = 0;
	// World XYZ support identity survives ClusterBuilder's splat reordering.
	TSet<FIntVector> SnowCliffShelfCells2872;
	int32 ReconstructionOutputCrown = 0;
	int32 ReconstructionFloorTintCount = 0;
	int32 GroundSourceOnlyCount = 0;
	int32 GroundSoftReceiverCount = 0;
	int32 GroundFloorWeakBlockerRecovered = 0;
	if (bReconstructedSnow)
	{
		const double Cm = SourceDetailCellCm;
		ReconstructedSnowField.Reserve(FMath::Min(SourceSurfaceGrid.Num() * 5 / 4, 650000));
		// First pass: use locally measured GS covariance normals and a robust,
		// multi-sample (weighted local plane) MLS height, in independently indexed
		// XYZ layers. Never use one global XY heightmap: roof and ground may share XY.
		for (const TPair<FIntVector, FSourceSurfaceCell>& Entry : SourceSurfaceGrid)
		{
			const FSourceSurfaceCell& Center = Entry.Value;
			if (Center.Samples < 1 || Center.WeightSum <= 0) continue;
			const FVector P = Center.PositionSum;
			const FVector N = Center.NormalSum.GetSafeNormal();
			// Allow moderately sloped original GS here. The surface TYPE below
			// decides whether it is a verified stone crown or a real cliff shelf.
			// The old early cutoff discarded the structure before its classification.
			if (P.ContainsNaN() || N.ContainsNaN() ||
				(N.Z < 0.46))
			{ continue; }
			const FSourceLandformEvidence Land = MeasureLandform(Entry.Key, P, N, bFeatures);
			// Full source topology is the receiving surface. A large detached sky
			// Gaussian and the synthetic LOD were already filtered at ingestion.
			int32 Neighbors = 0;
			double WSum = 0.0, ZSum = 0.0;
			FVector NormalSum = FVector::ZeroVector;
			for (int32 DY = -2; DY <= 2; ++DY)
			for (int32 DX = -2; DX <= 2; ++DX)
			for (int32 DZ = -2; DZ <= 2; ++DZ)
			{
				const FSourceSurfaceCell* S = SourceSurfaceGrid.Find(Entry.Key + FIntVector(DX,DY,DZ));
				if (!S || S->Samples < 1 || S->WeightSum <= 0) continue;
				const FVector Q = S->PositionSum;
				const FVector SN = S->NormalSum.GetSafeNormal();
				if (SN.Z < 0.45 || FVector::DotProduct(N,SN) < 0.56 ||
					FMath::Abs(Q.Z - P.Z) > Cm * 2.0) continue;
				const double D2 = FMath::Square(Q.X - P.X) + FMath::Square(Q.Y - P.Y);
				if (D2 > FMath::Square(Cm * 2.8)) continue;
				const double Weight = FMath::Exp(-D2 / (2.0 * Cm * Cm * 1.9 * 1.9)) *
					FMath::Min(6.0, double(S->Samples));
				if (Weight < 0.0001) continue;
				// Project the neighboring ORIGINAL GS tangent plane onto this XY.
				const double PlaneZ = Q.Z - (SN.X * (P.X-Q.X) + SN.Y * (P.Y-Q.Y)) / FMath::Max(0.28, SN.Z);
				ZSum += FMath::Clamp(PlaneZ, P.Z-Cm*1.4, P.Z+Cm*1.4) * Weight;
				NormalSum += SN * Weight;
				WSum += Weight;
				if (DX || DY) ++Neighbors;
			}
			if (WSum < 0.01 || (Neighbors < 2 && !Land.bRail && !Land.bCliffLedge))
			{
			  continue; }
			const FVector SmoothN = NormalSum.GetSafeNormal();
			const double MLSZ = FMath::Lerp(P.Z, ZSum/WSum, 0.72);
			const FVector Surface(P.X,P.Y,MLSZ);
			int32 Receiver = INDEX_NONE;
			FVector ProxyP;
			const bool bReceiver = FindSnowReceiver(Surface, Receiver, ProxyP);
			// 2.8.51: The fitted normal on a scanned vertical rock can drift upward
			// when small wall splats are averaged. It is NOT proof of a shelf.
			// Require actual, laterally connected, upward source material for a
			// cliff shelf, and separate this from ordinary ground / roof sampling.
			int32 FlatSupport = 0;
			uint8 ShelfSides = 0;
			for (int32 DY=-2; DY<=2; ++DY)
			for (int32 DX=-2; DX<=2; ++DX)
			{
				if (!(DX || DY)) continue;
				bool bNeighborSupports = false;
				for (int32 DZ=-1; DZ<=1 && !bNeighborSupports; ++DZ)
				{
					const FSourceSurfaceCell* Neighbor = SourceSurfaceGrid.Find(Entry.Key + FIntVector(DX,DY,DZ));
					if (!Neighbor || Neighbor->Samples < 1) continue;
					const FVector NN = Neighbor->NormalSum.GetSafeNormal();
					if (NN.Z < 0.77 || FVector::DotProduct(NN,SmoothN) < 0.73 ||
						FMath::Abs(Neighbor->PositionSum.Z-Surface.Z) > Cm*0.92) continue;
					bNeighborSupports = true;
				}
				if (!bNeighborSupports) continue;
				++FlatSupport;
				if (DX<0) ShelfSides |= 1u;
				if (DX>0) ShelfSides |= 2u;
				if (DY<0) ShelfSides |= 4u;
				if (DY>0) ShelfSides |= 8u;
			}
			const bool bLinearSupport = (ShelfSides & 3u)==3u || (ShelfSides & 12u)==12u;
			const double RealUpFraction=Center.VerifiedUpWeight/FMath::Max(1.0e-6,Center.WeightSum);
			const bool bVerifiedShelf = Land.bCliffLedge && RealUpFraction >= 0.52 &&
				N.Z >= 0.71 && SmoothN.Z >= 0.72 &&
				FlatSupport >= 4 && bLinearSupport && Land.VerticalRock >= 2;
			const bool bVerifiedRail = Land.bRail && RealUpFraction >= 0.52 &&
				N.Z >= 0.70 && SmoothN.Z >= 0.73 && Neighbors >= 2;
			// A broad floor is a SUPPORTED horizontal receiver, not a census of
			// every scanned cobblestone's smallest covariance axis. Stone joints and
			// rough rocks may be individually tilted even while the floor is level.
			// Require proximity to a real upward proxy, spatial neighbors and measured
			// original GS so a vertical wall/sky splat cannot become a floor.
			
			// The floor test is controlled independently from real proxy lookup.
			// A receiver-less A/B point needs measured original-GS support; it does
			// not invent a proxy height or pretend to be proxy-verified.
			// Preserve the released v2.8.58 floor receiver rules, not old test toggles.
			const bool bReceiverSupportedFloor = bReceiver ||
				(N.Z >= 0.66 && SmoothN.Z >= 0.66 && Neighbors >= 3);
			const bool bFloorTopologyOK = N.Z >= 0.54 && SmoothN.Z >= 0.63 &&
				Neighbors >= 3 && RealUpFraction >= 0.16 && Center.Samples >= 1;
			const bool bBaselineVerifiedFloor = bReceiverSupportedFloor && bFloorTopologyOK;
			// Source-first alternative ONLY for broad, multilaterally connected
			// real-GS ground. The original flat-support / uphill neighbor votes
			// reject sparse wall splats, floating canopy and sharp cliff shelves.
			const bool bMeasuredBroadFloor = Source->bSnowFloorPreferSource &&
				!Land.bCliffLedge && !Land.bRail && FlatSupport >= 5 && bLinearSupport &&
				Center.Samples >= 1 && Neighbors >= 4 &&
				RealUpFraction >= 0.50 && N.Z >= 0.76 && SmoothN.Z >= 0.76 &&
				HasSourceGS(Surface);
			int32 SoftFloorReceiver = INDEX_NONE;
			FVector SoftFloorProxy = FVector::ZeroVector;
			bool bSoftReceiverFound = false;
			bool bSourceFloorPromoted = false;
			if (bMeasuredBroadFloor && !bBaselineVerifiedFloor &&
				(!Source->bSnowGaussianSkyExposure || !IsFloorSheltered(bReceiver ? Receiver : INDEX_NONE, Surface)))
			{
				bSoftReceiverFound = FindSoftFloorReceiver(Surface, SoftFloorReceiver, SoftFloorProxy);
				// Even without a valid proxy triangle, the measured XYZ-layer
				// can receive snow when there is no corroborated roof blocker.
				// Never use another proxy surface as the output geometry.
				bSourceFloorPromoted = true;
				if (bSoftReceiverFound) ++GroundSoftReceiverCount;
				else ++GroundSourceOnlyCount;
				if (Source->bSnowGaussianSkyExposure &&
					IsSheltered(bReceiver ? Receiver : INDEX_NONE, Surface))
					++GroundFloorWeakBlockerRecovered;
			}
			const bool bVerifiedFloor = bBaselineVerifiedFloor || bSourceFloorPromoted;

			// Rounded/pointed stone ornaments are not broad floors or vertical
			// cliffs. Detect a supported 3D crown: height must rise inward and fall
			// outward ALONG the measured normal's horizontal direction. The flank
			// normals must also turn around the surface. This excludes planar cliff
			// faces and ordinary hills with a constant normal field.
			const auto FindCrownNeighbor = [&SourceSurfaceGrid, Cm](const FVector& Target,
				const FVector& Reference, FVector& OutP, FVector& OutN) -> bool
			{
				const FIntVector Key=XYZCell(Target,Cm);
				double Best=FMath::Square(Cm*2.10);
				bool Found=false;
				for (int32 DX=-1;DX<=1;++DX)
				for (int32 DY=-1;DY<=1;++DY)
				for (int32 DZ=-5;DZ<=5;++DZ)
				{
					const FSourceSurfaceCell* S=SourceSurfaceGrid.Find(Key+FIntVector(DX,DY,DZ));
					if (!S || S->Samples<1 || S->WeightSum<=0.0) continue;
					const FVector& Q=S->PositionSum;
					if (FMath::Abs(Q.Z-Reference.Z)>Cm*5.2) continue;
					const double D=FMath::Square(Q.X-Target.X)+FMath::Square(Q.Y-Target.Y);
					if (D >= Best) continue;
					const FVector QN=S->NormalSum.GetSafeNormal();
					if (QN.Z < 0.28) continue;
					Best=D;Found=true;OutP=Q;OutN=QN;
				}
				return Found;
			};
			bool bVerifiedCrown=false;
			if (bFeatures && bReceiver && !bVerifiedFloor && !bVerifiedShelf && !bVerifiedRail &&
				SmoothN.Z >= 0.52 && N.Z >= 0.49 && N.Z <= 0.84 &&
				RealUpFraction >= 0.23 && Neighbors >= 3 && Center.Samples >= 2 &&
				Triangles[Receiver].Up >= 0.32 && Triangles[Receiver].Up <= 0.84 &&
				FMath::Abs(Surface.Z-ProxyP.Z) <= SourceHeightToleranceCm * 1.10)
			{
				const FVector Down=FVector(SmoothN.X,SmoothN.Y,0.0).GetSafeNormal();
				if (!Down.IsNearlyZero())
				{
					const FVector Side(-Down.Y,Down.X,0.0);
					FVector InP,OutP,LeftP,RightP,InN,OutN,LeftN,RightN;
					// Only candidate surface branches pay for this 3D-neighborhood
					// test. All queries are world-fixed and independent of the camera.
					const double Radius=Cm*2.5;
					const bool bIn=FindCrownNeighbor(Surface-Down*Radius,Surface,InP,InN);
					const bool bOut=FindCrownNeighbor(Surface+Down*Radius,Surface,OutP,OutN);
					const bool bLeft=FindCrownNeighbor(Surface-Side*Radius,Surface,LeftP,LeftN);
					const bool bRight=FindCrownNeighbor(Surface+Side*Radius,Surface,RightP,RightN);
					// Cliff slopes can rise and fall along Down, but they will
					// not turn outward on BOTH sides like a compact spire.
					const double SpreadL=bLeft ? FVector::DotProduct(FVector(LeftN.X,LeftN.Y,0.0),Side) : 0.0;
					const double SpreadR=bRight ? FVector::DotProduct(FVector(RightN.X,RightN.Y,0.0),Side) : 0.0;
					const bool bCurved=bLeft && bRight && SpreadR-SpreadL>0.19;
					const bool bRising=bIn && bOut && InP.Z>Surface.Z+Cm*0.24 &&
						OutP.Z<Surface.Z-Cm*0.24 && InN.Z>=0.38 && OutN.Z>=0.29;
					// Must have real GS below the sloping stone instead of a
					// detached scan ghost, crown cap or nearby flat background.
					bVerifiedCrown=bCurved && bRising && HasSourceGS(Surface-FVector::UpVector*Cm*2.2);
				}
			}
			if (!bVerifiedCrown && N.Z<0.81 && bReceiver) ++ReconstructionCrownRejected;
			const bool bBroadGround = (RealUpFraction >= 0.57 && N.Z >= 0.81 && SmoothN.Z >= 0.81) ||
				(RealUpFraction >= 0.66 && N.Z >= 0.77 && SmoothN.Z >= 0.78 && Neighbors >= 6 && bReceiver &&
					Triangles[Receiver].Up >= 0.60 &&
					FVector::DotProduct(SmoothN,Triangles[Receiver].Normal) >= 0.70);
			if (!bBroadGround && !bVerifiedFloor && !bVerifiedShelf && !bVerifiedRail && !bVerifiedCrown)
			{
				if (Land.bCliffLedge) ++ReconstructionUnsupportedShelves;
				else if (N.Z < 0.81) ++ReconstructionCliffRejected;
				else ++ReconstructionSlopeRejected;
				
				continue;
			}
			// Only stable ordinary surfaces may bypass coarse proxy omissions.
			const bool bStrongSource = bBroadGround && Neighbors >= 5 && Center.Samples >= 2 && SmoothN.Z >= 0.83;
			// A floor must still be tied to its verified proxy layer. Crown flanks
			// require their own verified geometry; never skip the sky/roof tests.
			if (!bReceiver && !bStrongSource && !bVerifiedRail && !bVerifiedShelf &&
				!bSourceFloorPromoted &&
				!(bBroadGround && Neighbors >= 2 && N.Z >= 0.66))
			{ ++ReconstructionProxyRejects; continue; }
			if (Source->bSnowGaussianSkyExposure &&
				(bSourceFloorPromoted ? IsFloorSheltered(bReceiver ? Receiver : INDEX_NONE, Surface) :
				 IsSheltered(bReceiver ? Receiver : INDEX_NONE, Surface)))
			{ ++ReconstructionSkyRejects;
			  continue; }
			// Unlike the old binary region cutoff, all confidently measured exposed
			// ground participates. A continuous thickness field controls the edge.
			const double Field = SoftSnowField(Surface);
			const double Downwind = WindStrength * (0.11 * Land.Lee + 0.08 * FMath::Max(0.0,Land.Concavity));
			const double Gradient = FMath::Clamp((SmoothN.Z-0.52)/0.32,0.0,1.0);
			const double C = FMath::Clamp(0.49 + 0.34 * Field + 0.13 * Gradient + Downwind +
				(bVerifiedFloor ? 0.09 : (bVerifiedCrown ? -0.04 : 0.0)),0.28,0.99);
			const double Large = SnowValueNoise2D(Surface.X/(MoundScale*3.4)+5.83, Surface.Y/(MoundScale*3.4)-8.31);
			const double Small = SnowValueNoise2D(Surface.X/MoundScale - 3.7,Surface.Y/MoundScale + 17.6);
			FContinuousSnowSurface Out;
			Out.P = Surface;
			// The snowy floor follows a broad surface normal, not each stone's
			// sharp microfacet. Stone spires retain their sloped rounded profile.
			// v2862: trust the measured smooth surface on real slopes and stone.
			// Flattening every floor normal by 60% caused stacked horizontal discs.
			// A modest confidence-dependent correction remains only on rough ground.
			const double UpBias = bSurfaceResampling ?
				(bVerifiedFloor ? FMath::Clamp((1.0 - RealUpFraction) * 0.21, 0.04, 0.21) :
				 (bVerifiedCrown ? 0.035 : 0.085)) : -1.0;
			const FVector ExistingSnowNormal = bSurfaceResampling ?
				((1.0-UpBias)*SmoothN + UpBias*FVector::UpVector).GetSafeNormal() :
				(bVerifiedFloor ? (0.40*SmoothN+0.60*FVector::UpVector) :
				 (bVerifiedCrown ? (0.79*SmoothN+0.21*FVector::UpVector) :
				  (0.83*SmoothN+0.17*FVector::UpVector))).GetSafeNormal();
			Out.N = ExistingSnowNormal;
			// Keep exact v2.8.66 placement and classification when OFF. The
			// source-backed floor remains valid even without a matching proxy.
			if (bSurfaceResampling && Source->bSnowProxyNormalPriority)
			{
				const int32 NormalFace = bReceiver ? Receiver :
					(bSourceFloorPromoted && bSoftReceiverFound ? SoftFloorReceiver : INDEX_NONE);
				const FVector NormalProxyP = bReceiver ? ProxyP : SoftFloorProxy;
				if (NormalFace == INDEX_NONE)
				{
					++ProxySnowNoMatch;
				}
				else
				{
					const FVector PN = SnowProxyNormalAt(NormalFace,NormalProxyP);
					const double HeightError = FMath::Abs(Surface.Z-NormalProxyP.Z);
					const double MaxHeight = SourceHeightToleranceCm * (bReceiver ? 1.20 : 1.65);
					// Reject a proxy on the other side of a scan layer or one
					// whose tangent almost contradicts verified original support.
					if (PN.ContainsNaN() || PN.Z < 0.19 || HeightError > MaxHeight ||
						FVector::DotProduct(SmoothN,PN) < 0.20)
					{
						++ProxySnowInvalid;
					}
					else
					{
						// Confidence tapers to zero at the accepted height bound.
						const double Confidence = FMath::Clamp(1.0-HeightError/FMath::Max(1.0,MaxHeight),0.0,1.0);
						const double ProxyWeight = FMath::Clamp(double(Source->SnowProxyNormalBlend),0.0,1.0)*
							FMath::Lerp(0.30,1.0,Confidence);
						Out.N = FMath::Lerp(ExistingSnowNormal,PN,ProxyWeight).GetSafeNormal();
						if (ProxyWeight > 0.0 && !Out.N.IsNearlyZero())
						{
							++ProxySnowUsed;
							if (!bReceiver) ++ProxySnowSoft;
						}
						else Out.N=ExistingSnowNormal;
					}
				}
			}

			Out.Coverage = (float)C;
			Out.HeightCm = (float)FMath::Max(1.4, BaseThickness * (0.47 + 0.60*C) +
				MoundCm * (0.66 * Large + 0.34 * Small - 0.42) + BaseThickness*Downwind);
			Out.Kind = bVerifiedRail ? 1u : (bVerifiedShelf ? 2u : (bVerifiedFloor ? 3u : (bVerifiedCrown ? 4u : 0u)));
			if (Out.Kind==3) ++ReconstructionFloorCount;
			if (Out.Kind==4) ++ReconstructionCrownCount;
			Out.bProxyVerified = bSourceFloorPromoted ? bSoftReceiverFound : bReceiver;
			
			ReconstructedSnowField.Add(Entry.Key,Out);
		}
		ReconstructionRawNodes = ReconstructedSnowField.Num();
		if (ReconstructionRawNodes < 200)
		{
			Fail(FString::Printf(TEXT("2.8.52 실패: 연속 적설 표면 후보가 %d개입니다. 원본 GS 공분산과 프록시 차폐를 확인하세요."), ReconstructionRawNodes));
			return;
		}
		// Second pass: bridge ONLY a single missing XYZ source cell with support
		// on >=3 compatible neighboring surface samples. No open-air extrapolation
		// and no crossing from the roof down to the floor of the same XY column.
		TMap<FIntVector,FContinuousSnowSurface> GapCandidates;
		GapCandidates.Reserve(FMath::Min(ReconstructionRawNodes / 8, 45000));
		const FIntPoint Adjacent[] = {FIntPoint(1,0),FIntPoint(-1,0),FIntPoint(0,1),FIntPoint(0,-1),
			FIntPoint(1,1),FIntPoint(-1,-1),FIntPoint(1,-1),FIntPoint(-1,1)};
		for (const TPair<FIntVector,FContinuousSnowSurface>& Entry : ReconstructedSnowField)
		{
			const FIntVector K = Entry.Key;
			if ((Entry.Value.Kind != 0 && Entry.Value.Kind != 3) || Entry.Value.N.Z < 0.73) continue;
			for (const FIntPoint& D : Adjacent)
			{
				const FIntVector Target(K.X+D.X,K.Y+D.Y,K.Z);
				if (ReconstructedSnowField.Contains(Target) || GapCandidates.Contains(Target)) continue;
				FVector SumP = FVector::ZeroVector, SumN = FVector::ZeroVector;
				double SumCover = 0,SumHeight=0, MinZ=1.e30,MaxZ=-1.e30;
				int32 Support=0;
				for (const FIntPoint& B : Adjacent)
				{
					const FContinuousSnowSurface* Other = nullptr;
					double BestLayerError = Cm * 1.40;
					for (int32 ZZ=-1; ZZ<=1; ++ZZ)
					{
						const FContinuousSnowSurface* Test = ReconstructedSnowField.Find(Target+FIntVector(B.X,B.Y,ZZ));
						if (!Test || (Test->Kind!=0 && Test->Kind!=3) || Test->N.Z<0.70 ||
							FVector::DotProduct(Test->N,Entry.Value.N)<0.78) continue;
						const double Error=FMath::Abs(Test->P.Z-Entry.Value.P.Z);
						if (Error<BestLayerError) {BestLayerError=Error;Other=Test;}
					}
					if (!Other) continue;
					SumP+=Other->P;SumN+=Other->N;SumCover+=Other->Coverage;SumHeight+=Other->HeightCm;
					MinZ=FMath::Min(MinZ,Other->P.Z); MaxZ=FMath::Max(MaxZ,Other->P.Z);
					++Support;
				}
				if (Support < 3 || MaxZ-MinZ>Cm*1.25) continue;
				const FVector Q((double(Target.X)+0.5)*Cm,(double(Target.Y)+0.5)*Cm,SumP.Z/double(Support));
				// Source-ground bridging can use the same corroborated sky evidence as
				// the main floor field. Roofs and all non-ground candidates stay strict.
				int32 SameFloorSupport = 0;
				if (Source->bSnowFloorPreferSource)
					for (const FIntPoint& B : Adjacent)
						for (int32 ZZ = -1; ZZ <= 1; ++ZZ)
						{
							const FContinuousSnowSurface* NearbyFloor = ReconstructedSnowField.Find(Target + FIntVector(B.X,B.Y,ZZ));
							if (NearbyFloor && NearbyFloor->Kind == 3 && NearbyFloor->N.Z >= 0.78 &&
								FMath::Abs(NearbyFloor->P.Z-Q.Z) < Cm * 0.93)
							{ ++SameFloorSupport; break; }
						}
				const bool bSourceFloorBridge = Source->bSnowFloorPreferSource &&
					SameFloorSupport >= 4 && Entry.Value.Kind == 3;
				if (Source->bSnowGaussianSkyExposure &&
					(bSourceFloorBridge ? IsFloorSheltered(INDEX_NONE,Q) : IsSheltered(INDEX_NONE,Q))) continue;
				int32 Receiver = INDEX_NONE; FVector ProxyP;
				const bool bProxy = FindSnowReceiver(Q,Receiver,ProxyP);
				// The short gap has measured GS on three sides even if the coarse
				// proxy did not capture this local floor/roof triangle.
				if (bProxy && FMath::Abs(Q.Z-ProxyP.Z)>SourceHeightToleranceCm*1.4) continue;
				FContinuousSnowSurface Fill;
				Fill.P=Q;Fill.N=SumN.GetSafeNormal();
				Fill.Coverage=(float)(SumCover/double(Support)*0.96);
				Fill.HeightCm=(float)(SumHeight/double(Support));
				Fill.bInferredGap=true;Fill.bProxyVerified=bProxy;
				// Reuse the prevalidated same-height neighboring floor support.
				Fill.Kind=(bProxy && Triangles[Receiver].Up>=0.84) ||
					(bSourceFloorBridge && Fill.N.Z >= 0.78) ? 3u : 0u;
				GapCandidates.Add(Target,Fill);
			}
		}
		for (const TPair<FIntVector,FContinuousSnowSurface>& G : GapCandidates)
		{
			// v2865: do not truncate supported gap receivers at 650k when
			// measuring the complete snow count. The integer container limit still
			// applies, and unverified height layers are never bridged.
			if (!bSnowMeasureUncapped && ReconstructedSnowField.Num() >= 650000) break;
			ReconstructedSnowField.Add(G.Key,G.Value);++ReconstructionBridges;
		}
		// Third pass: share surface height and normal across connected cells.
		// This is a continuous reconstructed layer, not random per-splat offsets.
		TMap<FIntVector,FContinuousSnowSurface> SmoothField;
		SmoothField.Reserve(ReconstructedSnowField.Num());
		for (const TPair<FIntVector,FContinuousSnowSurface>& E : ReconstructedSnowField)
		{
			FContinuousSnowSurface Out=E.Value;
			if (Out.Kind == 0 || Out.Kind == 3)
			{
				double SumZ=Out.P.Z * 2.0, W=2.0;
				FVector NN=Out.N*2.0;
				for (const FIntPoint& D : Adjacent)
				{
					const FContinuousSnowSurface* S=nullptr;
					double BestLayerError = Cm * 1.30;
					for (int32 ZZ=-1; ZZ<=1; ++ZZ)
					{
						const FContinuousSnowSurface* Test=ReconstructedSnowField.Find(E.Key+FIntVector(D.X,D.Y,ZZ));
						if (!Test || (Test->Kind!=0 && Test->Kind!=3) || FVector::DotProduct(Test->N,Out.N)<0.80) continue;
						const double Error=FMath::Abs(Test->P.Z-Out.P.Z);
						if (Error<BestLayerError) {BestLayerError=Error;S=Test;}
					}
					if (!S) continue;
					const double Weight = FVector::DotProduct(S->N,Out.N)>0.90 ? 1.0 : 0.55;
					SumZ+=S->P.Z*Weight; NN+=S->N*Weight; W+=Weight;
				}
				if (W>2.0) {Out.P.Z=FMath::Lerp(Out.P.Z,SumZ/W,0.47);Out.N=NN.GetSafeNormal();++ReconstructionSmoothNodes;}
			}
			SmoothField.Add(E.Key,Out);
		}
		ReconstructedSnowField=MoveTemp(SmoothField);
		// v2852: Keep the v2850 non-grid renderer; per-type validation happens upstream.
		// Their equal-width slices projected as repeated horizontal lines on a cliff.
		// Instead, select actual MLS surface samples using three deterministic,
		// surface-aware blue-noise passes. A sparse preliminary pass distributes
		// relief over the whole region before a denser pass fills the remaining
		// holes. The underlying reconstructed field remains unchanged for the
		// original-GS albedo mask, regardless of how many splats are emitted.
		struct FSurfaceDart
		{
			FIntVector Cell = FIntVector::ZeroValue;
			const FContinuousSnowSurface* Surface = nullptr;
			uint32 Rank = 0u;
		};
		TArray<FSurfaceDart> Darts;
		Darts.Reserve(ReconstructedSnowField.Num());
		for (const TPair<FIntVector, FContinuousSnowSurface>& E : ReconstructedSnowField)
		{
			FSurfaceDart D;
			D.Cell = E.Key;
			D.Surface = &E.Value;
			D.Rank = CellHash(E.Key.X ^ (E.Key.Z * 3967), E.Key.Y ^ (E.Key.Z * 7411));
			Darts.Add(D);
		}
		Darts.Sort([](const FSurfaceDart& A, const FSurfaceDart& B)
		{
			if (A.Rank != B.Rank) return A.Rank < B.Rank;
			if (A.Cell.X != B.Cell.X) return A.Cell.X < B.Cell.X;
			if (A.Cell.Y != B.Cell.Y) return A.Cell.Y < B.Cell.Y;
			return A.Cell.Z < B.Cell.Z;
		});
		// 2.8.67: do not turn the requested 250k limit (or its 88% sub-cap) into a
		// surface count restriction in measurement mode. Only the array/indexing
		// representation's unavoidable int32 upper bound remains. This is NOT
		// pre-allocated and is never intended to be reached in a normal bake.
		const int32 SurfaceCap = bSnowMeasureUncapped ? (MAX_int32 - 1024) :
			FMath::Max(128, FMath::FloorToInt(double(MaxSnowCount) * 0.88));
		// v2861: each accepted floor/ordinary roof/crown receiver costs AT LEAST
		// two REAL, smaller splats. Reserve that capacity before selecting surface
		// seeds, not after depositing a large parent Gaussian. If the cap fills,
		// stop accepting new receivers before splitting; never leave the last
		// half of the scene with no geometric representation.
		const int32 ReceiverBudget = bMicroClusters && !bSurfaceResampling ? SurfaceCap / 2 : SurfaceCap;
		struct FMicroSeed
		{
			FVector P = FVector::ZeroVector;
			FVector N = FVector::UpVector;
			FVector Tangent = FVector::ForwardVector;
			double RX = 0.0, RY = 0.0, Lift = 0.0;
			float Coverage = 0.0f, OriginalOpacity = 0.0f;
			double OriginalCenterLift = 0.0;
			uint32 Hash = 0u;
			uint8 Kind = 0u;
		};
		TArray<FMicroSeed> MicroSeeds;
		if (bMicroClusters)
		{
			MicroSeeds.Reserve(FMath::Min(150000, Darts.Num()));
			SplitSeedCells.Reserve(FMath::Min(150000, Darts.Num()));
		}
		const double CapacityRatio = FMath::Max(1.0, double(Darts.Num()) / double(SurfaceCap));
		const double FirstSeparation = FMath::Clamp(Cm * (0.67 + 0.28 * FMath::Sqrt(CapacityRatio)),
			Cm * 0.69, Cm * 2.35);
		const double BinPitch = FMath::Max(Cm, FirstSeparation);
		// Only accepted samples use this grid. The grid is a neighbor-lookup
		// acceleration structure, NOT a spatial quantizer or emission lattice.
		TMap<FIntVector, TArray<int32>> SelectedNeighbors;
		SelectedNeighbors.Reserve(FMath::Min(SurfaceCap, 230000));
		TArray<FVector> SelectedPositions;
		TArray<FVector> SelectedNormals;
		SelectedPositions.Reserve(FMath::Min(SurfaceCap, Darts.Num()));
		SelectedNormals.Reserve(FMath::Min(SurfaceCap, Darts.Num()));
		TSet<FIntVector> ChosenSourceCells;
		ChosenSourceCells.Reserve(FMath::Min(SurfaceCap, Darts.Num()));
		int32 BlueNoiseRejected = 0;
		int32 BlueNoiseFills = 0;
		const int32 ReservedFeatureCount = FMath::Max(64, SurfaceCap / 9);
		const double RenderPitch = FMath::Clamp(Cm * (0.91 + 0.20 * FMath::Sqrt(CapacityRatio)),
			Cm * 0.95, Cm * 1.68);
		const auto SameLayerTooClose = [&](const FVector& P, const FVector& N,
			double LocalSeparation) -> bool
		{
			const FIntVector C = XYZCell(P, BinPitch);
			for (int32 Z = -1; Z <= 1; ++Z)
			for (int32 Y = -1; Y <= 1; ++Y)
			for (int32 X = -1; X <= 1; ++X)
			{
				const TArray<int32>* Others = SelectedNeighbors.Find(C + FIntVector(X, Y, Z));
				if (!Others) continue;
				for (int32 Index : *Others)
				{
					const FVector& Q = SelectedPositions[Index];
					const FVector& QN = SelectedNormals[Index];
					// Different sides of a wall / two stacked floors must never
					// push one another out of the selected snow layer.
					if (FVector::DotProduct(N, QN) < 0.72) continue;
					const FVector Diff = P - Q;
					const double OffPlane = FVector::DotProduct(Diff, N);
					if (FMath::Abs(OffPlane) > Cm * 0.53) continue;
					const double InPlane2 = FMath::Max(0.0, Diff.SizeSquared() - OffPlane * OffPlane);
					if (InPlane2 < FMath::Square(LocalSeparation)) return true;
				}
			}
			return false;
		};
		// The buffer budget is reserved BEFORE placement; no world-axis tile
		// boundary or early break in input-Ply order can cause a stripe.
		for (int32 Pass = 0; Pass < 3 &&
			(bMicroClusters ? SelectedPositions.Num() < ReceiverBudget : SnowSplats.Num() < SurfaceCap); ++Pass)
		{
			const double Spacing = FirstSeparation * (Pass == 0 ? 1.0 : (Pass == 1 ? 0.77 : 0.56));
			for (const FSurfaceDart& D : Darts)
			{
				if (bMicroClusters ? SelectedPositions.Num() >= ReceiverBudget : SnowSplats.Num() >= SurfaceCap) break;
				if (ChosenSourceCells.Contains(D.Cell)) continue;
				const FContinuousSnowSurface& S = *D.Surface;
				// Keep physical receiver types distinct: floor / roof / rail /
				// cliff shelf / convex stone crown share only the snow renderer.
				if (bSurfaceResampling && (S.Kind == 0 || S.Kind == 3 || S.Kind == 4)) continue;
				if ((S.Kind == 1 || S.Kind == 2 || S.Kind == 4) &&
					(ReconstructionRailCount + ReconstructionShelfCount + ReconstructionOutputCrown) >= ReservedFeatureCount)
					continue;
				const FVector N = S.N.GetSafeNormal();
				if (N.IsNearlyZero()) continue;
				const uint32 Hash = D.Rank;
				const double PhaseX = 2.0 * double(Hash & 0xffffu) / 65535.0 - 1.0;
				const double PhaseY = 2.0 * double((Hash >> 16u) & 0xffffu) / 65535.0 - 1.0;
				// Tangent-plane jitter is small, deterministic and WORLD-space.
				// The original MLS position is still the center of the patch:
				// this does not fabricate new floating cliff geometry.
				const FVector Contour = FVector::CrossProduct(FVector::UpVector, N).GetSafeNormal();
				FVector U = S.Kind == 1 ? FVector::CrossProduct(N, WindTo).GetSafeNormal() : Contour;
				if (U.IsNearlyZero())
					U = (WindTo - N * FVector::DotProduct(WindTo, N)).GetSafeNormal();
				if (U.IsNearlyZero())
					U = FVector::CrossProduct(FVector::RightVector, N).GetSafeNormal();
				const FVector V = FVector::CrossProduct(N, U).GetSafeNormal();
				const double TangentJitter = S.Kind == 0 ? (Cm * 0.145) : (Cm * 0.072);
				const FVector P = S.P + (U * PhaseX + V * PhaseY) * TangentJitter;
				const double MinimumDistance = Spacing * (S.Kind == 1 ? 0.60 : (S.Kind == 2 ? 0.69 : (S.Kind == 4 ? 0.83 : 1.0)));
				if (SameLayerTooClose(P, N, MinimumDistance))
				{
					++BlueNoiseRejected;
					continue;
				}
				// Local drift and covariance are evaluated on the fitted original
				// surface, not on the world Z slice used by the 2.8.49 tiles.
				const double C = FMath::Clamp(double(S.Coverage), 0.0, 1.0);
				const double Lift = FMath::Max(1.4, double(S.HeightCm));
				const double Slope = FMath::Clamp(1.0 - N.Z, 0.0, 1.0);
				const double OrientationNoise = PhaseX * 0.22;
				const FVector T = (U * FMath::Cos(OrientationNoise) + V * FMath::Sin(OrientationNoise)).GetSafeNormal();
				// Steeper local shelves get short, narrow patches, not parallel
				// 30-cm ellipses repeated at equal world Z values.
				const double RadiusX = S.Kind == 1 ? FMath::Max(6.0, Cm * 0.71) :
					(S.Kind == 2 ? FMath::Min(16.0, RenderPitch * 0.68) :
					 (S.Kind == 4 ? RenderPitch * 0.87 : RenderPitch * (1.02 + 0.13 * Slope)));
				const double RadiusY = S.Kind == 1 ? FMath::Max(3.0, Cm * 0.23) :
					(S.Kind == 2 ? FMath::Min(12.0, RenderPitch * 0.49) :
					 (S.Kind == 4 ? RenderPitch * 0.76 : RenderPitch * (0.87 - 0.06 * Slope)));
				const bool bBroadFloor = (S.Kind == 3);
				const bool bOrdinaryRoof = (S.Kind == 0);
				const double FloorRadiusScale = bBroadFloor ?
					FMath::Clamp((double)Source->SnowFloorRadiusScale, 0.60, 1.0) : 1.0;
				const double FloorDepthScale = bBroadFloor ?
					FMath::Clamp((double)Source->SnowFloorDepthScale, 0.55, 1.0) : 1.0;
				const double FrontViewRadiusScale = (bBroadFloor || bOrdinaryRoof) ? (bBroadFloor ? 0.84 : 0.88) : 1.0;
				const double FrontViewDepthScale = (bBroadFloor || bOrdinaryRoof) ? (bBroadFloor ? 1.22 : 1.16) : 1.0;
				const bool bStoneCrown = (S.Kind == 4);
				const double ParentScale = bMicroClusters ? (1.0 - MicroStrength *
					(bBroadFloor ? 0.31 : (bOrdinaryRoof ? 0.26 : (bStoneCrown ? 0.19 : 0.0)))) : 1.0;
				const double ParentDepthScale = bMicroClusters && (bBroadFloor || bOrdinaryRoof || bStoneCrown) ?
					(1.0 - MicroStrength * 0.13) : 1.0;
				FGaussianSplatData G;
				if (!MakeSplat(P + FVector::UpVector * (Lift * (S.Kind == 1 ? 0.43 :
					(bBroadFloor ? 0.67 * FloorDepthScale : (bOrdinaryRoof ? 0.61 : 0.67)))),
					N, T, RadiusX * FloorRadiusScale * FrontViewRadiusScale * ParentScale / DetailScale,
					RadiusY * FloorRadiusScale * FrontViewRadiusScale * ParentScale / DetailScale,
					FMath::Clamp(Lift * (0.54 + VolumeStrength * 0.25) * FloorDepthScale * FrontViewDepthScale * ParentDepthScale, 2.1, 19.0),
					FMath::Min(S.Kind == 1 ? 0.36f : (S.Kind == 4 ? 0.38f : 0.47f),
						DetailOpacity * float(0.60 + 0.32 * C)),
					0.935f + float(0.025 * C), OrientationNoise, G)) continue;
				if (bMicroClusters && (bBroadFloor || bOrdinaryRoof || bStoneCrown))
					LimitSnowWorldSigma(G, MicroSigmaCapCm * (bStoneCrown ? 0.90 : 1.0));
				// v2872: limit ONLY a verified cliff shelf (Kind=2), not
				// ordinary roof/floor/crown, or the real source GS. Two-sided
				// rock walls remain excluded by the original shelf classifier.
				if (bCliffCompact2872 && S.Kind == 2u)
				{
					LimitSnowWorldSigma(G,FMath::Max(4.0,FMath::Min(9.0,Cm*0.42)*CliffScale2872));
					++SnowCliffCompactMain2872;
					SnowCliffShelfCells2872.Add(XYZCell(P,SourceDetailCellCm));
				}
				const int32 SelectedIndex = SelectedPositions.Add(P);
				SelectedNormals.Add(N);
				SelectedNeighbors.FindOrAdd(XYZCell(P, BinPitch)).Add(SelectedIndex);
				ChosenSourceCells.Add(D.Cell);
				
				// For true splitting the parent G is only an acceptance/shape check.
				// NEVER append the large parent to the rendered snow asset.
				const bool bReplaceParent = bMicroClusters && (bBroadFloor || bOrdinaryRoof || bStoneCrown);
				if (!bReplaceParent) SnowSplats.Add(G);
				// Old 2.8.59 immediate one-splat support path is retained for
				// A/B rollback when the new cluster toggle is OFF.
				if (!bMicroClusters && (bBroadFloor || bOrdinaryRoof) && SnowSplats.Num() < SurfaceCap && C >= 0.38 && ((Hash >> 5u) & 1u) == 0u)
				{
					const FVector SupportSide = FVector::CrossProduct(N, T).GetSafeNormal();
					if (!SupportSide.IsNearlyZero())
					{
						const double SideSign = ((Hash >> 7u) & 1u) ? 1.0 : -1.0;
						const FVector SupportAnchor = P + SupportSide * (SideSign * RenderPitch * (bBroadFloor ? 0.11 : 0.08));
						FGaussianSplatData GSupport;
						if (MakeSplat(SupportAnchor + FVector::UpVector * (Lift * (bBroadFloor ? 0.53 : 0.48)),
							N, SupportSide,
							(RadiusX * (bBroadFloor ? 0.46 : 0.42)) / DetailScale,
							(RadiusY * (bBroadFloor ? 0.42 : 0.38)) / DetailScale,
							FMath::Clamp(Lift * (bBroadFloor ? 0.70 : 0.62), 2.2, 11.0),
							FMath::Min(0.29f, DetailOpacity * float(0.42 + 0.16 * C)),
							0.928f + float(0.018 * C), OrientationNoise + SideSign * 0.73, GSupport))
						{
							SnowSplats.Add(GSupport);
						}
					}
				}
				// Accepted source-backed surface only; every omitted parent MUST
				// become a seed, independent of its coverage or the feature normal.
				// A previous C>=0.37 condition stranded some omitted patches.
				if (bReplaceParent)
				{
					FMicroSeed Seed;
					Seed.P = P; Seed.N = N; Seed.Tangent = T; Seed.Lift = Lift;
					Seed.RX = RadiusX * FloorRadiusScale * FrontViewRadiusScale;
					Seed.RY = RadiusY * FloorRadiusScale * FrontViewRadiusScale;
					Seed.Coverage = float(C); Seed.Hash = Hash; Seed.Kind = S.Kind;
					Seed.OriginalOpacity = G.Opacity;
					Seed.OriginalCenterLift = Lift * (bBroadFloor ? 0.67 * FloorDepthScale : (bOrdinaryRoof ? 0.61 : 0.67));
					MicroSeeds.Add(Seed);
					SplitSeedCells.Add(XYZCell(P, SourceDetailCellCm));
					++SnowSplitParentsOmitted;
				}
				++ReconstructionOutputNodes;
				if (S.Kind == 1) ++ReconstructionRailCount;
				else if (S.Kind == 2) ++ReconstructionShelfCount;
				if (S.Kind == 4) ++ReconstructionOutputCrown;
				if (Pass > 0) ++BlueNoiseFills;
			}
		}
		// v2861: TRUE replacement of each broad floor, ordinary roof and crown
		// Gaussian. No large parent survives. Produce at least two compact children
		// per accepted MLS receiver, in round-robin world-space passes. Reserve
		// capacity before seed collection above, so an arbitrary source order can
		// never spend all the budget on a few dense clusters.
		SnowMicroEligible = MicroSeeds.Num();
		if (bMicroClusters && !MicroSeeds.IsEmpty())
		{
			const int32 AvailableForChildren = FMath::Max(0, SurfaceCap - SnowSplats.Num());
			const int32 GuaranteedPairs = FMath::Min(MicroSeeds.Num(), AvailableForChildren / 2);
			const int32 DesiredMaxLayers = 2 + FMath::Clamp(FMath::RoundToInt(2.0 * MicroStrength), 0, 2);
			const double ChildSigmaCap = SnowSplitWorldCapCm;
			for (int32 Layer = 0; Layer < DesiredMaxLayers && SnowSplats.Num() < SurfaceCap; ++Layer)
			{
				for (int32 I = 0; I < MicroSeeds.Num() && SnowSplats.Num() < SurfaceCap; ++I)
				{
					const FMicroSeed& Seed = MicroSeeds[I];
					// The primary two passes are guaranteed their slots; optional
					// later layers only consume the remaining reserved capacity.
					if (Layer < 2 && I >= GuaranteedPairs) continue;
					const FVector N = Seed.N.GetSafeNormal();
					const FVector T = (Seed.Tangent - N * FVector::DotProduct(N, Seed.Tangent)).GetSafeNormal();
					const FVector B = FVector::CrossProduct(N, T).GetSafeNormal();
					if (N.IsNearlyZero() || T.IsNearlyZero() || B.IsNearlyZero())
					{
						++SnowSplitChildrenFailed;
						continue;
					}
					// The first two grains straddle the original measured patch;
					// later grains fill orthogonal corners without a grid or ring.
					const uint32 H = CellHash(int32(Seed.Hash ^ (uint32(Layer + 1) * 0x9e3779b9u)),
						int32((Seed.Hash >> 8u) ^ (uint32(Layer + 1) * 0x85ebca6bu)));
					const double Phase = double(H & 65535u) / 65535.0;
					const double Variant = double((H >> 16u) & 65535u) / 65535.0;
					const double Angle = 6.283185307179586 * (double(Seed.Hash & 65535u) / 65535.0)
						+ (Layer == 0 ? 0.0 : (Layer == 1 ? 3.141592653589793
						: (Layer == 2 ? 1.570796326794897 : 4.71238898038469)))
						+ (Phase - 0.5) * 0.24;
					const double ParentRadius = 0.5 * (Seed.RX + Seed.RY);
					const double OffsetCm = FMath::Min(Cm * 0.26, ParentRadius * (Layer < 2 ? 0.29 : 0.43))
						* (0.86 + 0.24 * Variant);
					const FVector Offset = (T * FMath::Cos(Angle) + B * FMath::Sin(Angle)) * OffsetCm;
					const bool bStone = Seed.Kind == 4;
					const double ScaleFactor = (Layer < 2 ? 0.65 : 0.53) * (0.94 + 0.12 * Phase);
					const double HeightOffset = (Variant - 0.5) * FMath::Min(1.15, Seed.Lift * 0.09);
					const double Depth = FMath::Clamp(Seed.Lift * (bStone ? 0.46 : 0.49), 2.0, 10.0);
					const float ChildOpacity = FMath::Min(0.48f, Seed.OriginalOpacity *
						float(Layer < 2 ? 1.37 : 1.02));
					FGaussianSplatData Child;
					if (!MakeSplat(Seed.P + Offset + FVector::UpVector * (Seed.OriginalCenterLift + HeightOffset),
						N, T, Seed.RX * ScaleFactor / DetailScale,
						Seed.RY * ScaleFactor / DetailScale, Depth, ChildOpacity,
						float(0.938 + 0.021 * Phase), (Phase - 0.5) * 0.40, Child))
					{
						++SnowSplitChildrenFailed;
						continue;
					}
					LimitSnowWorldSigma(Child, ChildSigmaCap * (bStone ? 0.90 : 1.0));
					// Shorten overly elongated source-oriented grains after the
					// world-space limit. The original scan covariance is untouched.
					const FQuat CQ(double(Child.Rotation.X), double(Child.Rotation.Y),
						double(Child.Rotation.Z), double(Child.Rotation.W));
					const double CX = GSWorld.TransformVector(CQ.RotateVector(FVector(double(Child.Scale.X), 0.0, 0.0))).Size();
					const double CY = GSWorld.TransformVector(CQ.RotateVector(FVector(0.0, double(Child.Scale.Y), 0.0))).Size();
					if (CX > CY * 1.48 && CX > 1.0e-5) Child.Scale.X *= float((CY * 1.48) / CX);
					if (CY > CX * 1.48 && CY > 1.0e-5) Child.Scale.Y *= float((CX * 1.48) / CY);
					const double FinalCX = GSWorld.TransformVector(CQ.RotateVector(FVector(double(Child.Scale.X), 0.0, 0.0))).Size();
					const double FinalCY = GSWorld.TransformVector(CQ.RotateVector(FVector(0.0, double(Child.Scale.Y), 0.0))).Size();
					SnowSplitMaxWorldSigma = FMath::Max(SnowSplitMaxWorldSigma, FMath::Max(FinalCX, FinalCY));
					SnowSplats.Add(Child);
					++SnowMicroAdded;

				}
			}
			SnowSplitSeedsWithoutPair = MicroSeeds.Num() - GuaranteedPairs;
		}
		// v2864: coverage-driven resurfacing including 4-supported sub-cell gaps. The measured XYZ receivers define
		// every possible snow position. Stage 1 spreads one fine patch across the
		// entire field, stage 2 inspects supported links, and stage 3 ACTUALLY
		// spends the remaining budget on measured low-coverage sites. The source
		// PLY and its baked tint mask are not modified by this geometry path.
		if (bSurfaceResampling)
		{
			struct FPlacedSurfacePatch
			{
				FVector P = FVector::ZeroVector;
				FVector N = FVector::UpVector;
				FVector AxisX = FVector::ForwardVector;
				FVector AxisY = FVector::RightVector;
				float SigmaX = 0.0f;
				float SigmaY = 0.0f;
				float Alpha = 0.0f;
				uint8 Kind = 0;
				bool bBridge = false;
				int32 SplatIndex = INDEX_NONE; // index within SnowSplats, never source GS
			};
			TArray<FPlacedSurfacePatch> Placed;
			Placed.Reserve(FMath::Min(SurfaceCap, Darts.Num()));
			const bool bFineLayer2869 = Source->bSnowResampleGeometricCoverage && Source->bSnowFineLayer2869;
			const double MaxSmallSigma = FMath::Min(SnowSplitWorldCapCm,
				FMath::Clamp((double)Source->SnowResampleWorldSigmaCm, 3.5, 9.0) *
				FMath::Clamp((double)Source->SnowFineSizeFactor, 0.65, 1.0) *
				(bFineLayer2869 ? FMath::Clamp((double)Source->SnowFineLayerScale2869,0.65,1.0) : 1.0));
			const double TargetCoverage = FMath::Clamp((double)Source->SnowResampleCoverageTarget, 0.23, 0.70);
			const bool bGeometricCoverage = Source->bSnowResampleGeometricCoverage;
			const double GeometricFootprint = FMath::Clamp((double)Source->SnowResampleGeometricFootprintSigma, 0.90, 1.80);
			const double GeometricFootprintSq = FMath::Square(GeometricFootprint);
			const double BinCm = FMath::Max(Cm * 1.50, MaxSmallSigma * 3.50);
			TMap<FIntVector, TArray<int32>> PatchBins;
			PatchBins.Reserve(FMath::Min(SurfaceCap, 230000));
			const int32 SnowStart = SnowSplats.Num();
			const auto IsBroad = [](uint8 K) -> bool { return K == 0 || K == 3 || K == 4; };
			const auto Compatible = [Cm](const FContinuousSnowSurface& A,
				const FContinuousSnowSurface& B) -> bool
			{
				if (A.Kind != B.Kind || (A.Kind != 0 && A.Kind != 3 && A.Kind != 4)) return false;
				const double MinDot = A.Kind == 4 ? 0.79 : 0.83;
				const double MaxZ = Cm * (A.Kind == 4 ? 0.68 : 0.86);
				return FVector::DotProduct(A.N, B.N) >= MinDot &&
					FMath::Abs(A.P.Z - B.P.Z) < MaxZ &&
					FVector::DistSquared2D(A.P, B.P) < FMath::Square(Cm * 1.82);
			};
			// Each candidate receives a new MLS fit; a neighboring roof/floor at a
			// different Z layer is not a valid interpolant. Reprojection never
			// depends on the editor/MRQ camera or on projected screen pixels.
			const auto Reproject = [&](const FIntVector& Key,
				const FContinuousSnowSurface& Origin, const FVector& Requested,
				FVector& P, FVector& N, double& Variation, int32& Support) -> bool
			{
				FVector SumN = Origin.N * 0.18;
				double SumZ = Origin.P.Z * 0.18, W = 0.18;
				Variation = 0.0;
				Support = 0;
				for (int32 DZ=-1; DZ<=1; ++DZ)
				for (int32 DY=-1; DY<=1; ++DY)
				for (int32 DX=-1; DX<=1; ++DX)
				{
					const FContinuousSnowSurface* Q = ReconstructedSnowField.Find(Key+FIntVector(DX,DY,DZ));
					if (!Q || !Compatible(Origin,*Q)) continue;
					const double Horizontal2 = FVector::DistSquared2D(Requested,Q->P);
					if (Horizontal2 > FMath::Square(Cm*1.64)) continue;
					const double PlaneZ = Q->P.Z -
						(Q->N.X*(Requested.X-Q->P.X)+Q->N.Y*(Requested.Y-Q->P.Y)) /
						FMath::Max(0.43,Q->N.Z);
					const double Weight = FMath::Exp(-Horizontal2/FMath::Square(Cm*0.98));
					SumZ += FMath::Clamp(PlaneZ,Origin.P.Z-Cm*0.54,Origin.P.Z+Cm*0.54)*Weight;
					SumN += Q->N*Weight;
					Variation += Weight*(1.0-FMath::Clamp(FVector::DotProduct(Origin.N,Q->N),-1.0,1.0));
					W += Weight;
					++Support;
				}
				if (!Support || W < 0.20) return false;
				P=FVector(Requested.X,Requested.Y,
					FMath::Clamp(SumZ/W,Origin.P.Z-Cm*0.49,Origin.P.Z+Cm*0.49));
				N=SumN.GetSafeNormal();
				Variation=FMath::Clamp(Variation/W,0.0,1.0);
				if (N.Z < (Origin.Kind==4 ? 0.43 : 0.61) ||
					FVector::DotProduct(Origin.N,N) < (Origin.Kind==4 ? 0.77 : 0.84)) return false;
				return !P.ContainsNaN() && !N.ContainsNaN();
			};
			// Use the TRUE fitted, anisotropic axes, not a circular radius that
			// overestimates coverage along a Gaussian's short axis. Multiply optical
			// transmittances; this is an approximation to world-space alpha coverage,
			// not a camera-dependent pixel accumulation.
			const auto CoverageAt = [&](const FVector& P,const FVector& N,uint8 Kind) -> double
			{
				const FIntVector C=XYZCell(P,BinCm);
				double T=1.0;
				for (int32 DZ=-1; DZ<=1; ++DZ)
				for (int32 DY=-1; DY<=1; ++DY)
				for (int32 DX=-1; DX<=1; ++DX)
				{
					const TArray<int32>* Bin=PatchBins.Find(C+FIntVector(DX,DY,DZ));
					if (!Bin) continue;
					for (int32 Index : *Bin)
					{
						const FPlacedSurfacePatch& Q=Placed[Index];
						if (Q.Kind!=Kind || FVector::DotProduct(N,Q.N)<(Kind==4 ? 0.77 : 0.84)) continue;
						const FVector D=P-Q.P;
						const double Plane=FVector::DotProduct(D,Q.N);
						if (FMath::Abs(Plane)>Cm*(Kind==4 ? 0.52 : 0.64)) continue;
						const double TX=FVector::DotProduct(D,Q.AxisX)/FMath::Max(1.0,double(Q.SigmaX));
						const double TY=FVector::DotProduct(D,Q.AxisY)/FMath::Max(1.0,double(Q.SigmaY));
						const double R2=TX*TX+TY*TY;
						if (R2>10.24) continue;
						T*=1.0-FMath::Clamp(double(Q.Alpha)*FMath::Exp(-0.5*R2),0.0,0.90);
					}
				}
				return 1.0-T;
			};
			// 2.8.67: actual world-space ellipse distance on one supported height
			// layer. Optical alpha is still measured separately with CoverageAt.
			// Returning a large value means NO compatible patch at this point.
			const auto GeometryR2At = [&](const FVector& P,const FVector& N,uint8 Kind) -> double
			{
				const FIntVector C=XYZCell(P,BinCm);
				double NearestR2=1.0e12;
				for (int32 DZ=-1; DZ<=1; ++DZ)
				for (int32 DY=-1; DY<=1; ++DY)
				for (int32 DX=-1; DX<=1; ++DX)
				{
					const TArray<int32>* Bin=PatchBins.Find(C+FIntVector(DX,DY,DZ));
					if (!Bin) continue;
					for (int32 Index : *Bin)
					{
						const FPlacedSurfacePatch& Q=Placed[Index];
						if (Q.Kind!=Kind || FVector::DotProduct(N,Q.N)<(Kind==4 ? 0.77 : 0.84)) continue;
						const FVector Diff=P-Q.P;
						if (FMath::Abs(FVector::DotProduct(Diff,Q.N))>Cm*(Kind==4 ? 0.52 : 0.64)) continue;
						const double TX=FVector::DotProduct(Diff,Q.AxisX)/FMath::Max(1.0,double(Q.SigmaX));
						const double TY=FVector::DotProduct(Diff,Q.AxisY)/FMath::Max(1.0,double(Q.SigmaY));
						NearestR2=FMath::Min(NearestR2,TX*TX+TY*TY);
					}
				}
				return NearestR2;
			};
			const auto CoveredAt = [&](const FVector& P,const FVector& N,uint8 Kind) -> bool
			{
				return bGeometricCoverage ? GeometryR2At(P,N,Kind)<=GeometricFootprintSq :
					CoverageAt(P,N,Kind)>=TargetCoverage*(Kind==4 ? 0.92 : 1.0);
			};
			int32 ResampleRefill=0, ResampleTooClose=0, ResampleBudgetMiss=0;
			int32 ResampleUncoveredFine=0;
			const int32 SurfaceAvailable=FMath::Max(0,SurfaceCap-SnowStart);
			const int32 SeedQuota=bSnowMeasureUncapped ? SurfaceCap : FMath::FloorToInt(SurfaceAvailable*0.54);
			const int32 LinkQuota=bSnowMeasureUncapped ? SurfaceCap : FMath::FloorToInt(SurfaceAvailable*0.79);
			// A support-aware small grain is never replaced by a larger emergency
			// patch. If the budget runs out, residual gaps are reported explicitly.
			const auto TryPlace = [&](const FSurfaceDart& D,const FVector& Requested,
				bool bBetween,bool bRefill) -> bool
			{
				if (SnowSplats.Num()>=SurfaceCap) return false;
				const FContinuousSnowSurface& S=*D.Surface;
				FVector P,N;
				double Variation=0.0;
				int32 Support=0;
				++ResampleEligible;
				if (!Reproject(D.Cell,S,Requested,P,N,Variation,Support) ||
					(bBetween && Support<2))
				{ ++ResampleUnsupported; return false; }
				const double LocalTarget=TargetCoverage*(S.Kind==4 ? 0.92 : 1.0);
				// A 0.34-alpha snow Gaussian can already cover the site in world
				// space: never require a second one solely to reach alpha 0.38.
				if (bGeometricCoverage && GeometryR2At(P,N,S.Kind)<=GeometricFootprintSq)
				{ ++ResampleCovered; return false; }
				const double CurrentAlpha=CoverageAt(P,N,S.Kind);
				if (!bGeometricCoverage && CurrentAlpha>=LocalTarget)
				{ ++ResampleCovered; return false; }
				const double SupportFactor=Support<=2 ? 0.85 : (Support<=4 ? 0.94 : 1.0);
				const double LocalSigmaCap=MaxSmallSigma*(S.Kind==4 ? 0.86 : 1.0);
			const double Sigma=FMath::Clamp(Cm*(0.33-0.14*Variation)*SupportFactor,
					FMath::Min(bFineLayer2869 ? 2.50 : 3.30,LocalSigmaCap),LocalSigmaCap);
				// Reject exact piles which create bright white discs instead of a
				// connected surface. Neighbors farther away are freely permitted.
				const FIntVector C=XYZCell(P,BinCm);
				for (int32 DZ=-1;DZ<=1;++DZ)
				for (int32 DY=-1;DY<=1;++DY)
				for (int32 DX=-1;DX<=1;++DX)
				{
					const TArray<int32>* Bin=PatchBins.Find(C+FIntVector(DX,DY,DZ));
					if (!Bin) continue;
					for (int32 Index : *Bin)
					{
						const FPlacedSurfacePatch& Q=Placed[Index];
						if (Q.Kind!=S.Kind || FVector::DotProduct(N,Q.N)<0.82) continue;
						const FVector Diff=P-Q.P;
						const double OffPlane=FVector::DotProduct(Diff,N);
						if (FMath::Abs(OffPlane)>Cm*0.40) continue;
						const double Tangent2=FMath::Max(0.0,Diff.SizeSquared()-OffPlane*OffPlane);
						if (Tangent2<FMath::Square(FMath::Min(Sigma,FMath::Min(double(Q.SigmaX),double(Q.SigmaY)))*0.62))
						{ ++ResampleTooClose; return false; }
					}
				}
				const double Lift=FMath::Max(1.4,double(S.HeightCm));
				const double DepthScale=S.Kind==3 ?
					FMath::Clamp((double)Source->SnowFloorDepthScale,0.55,1.0) : 1.0;
				const double CenterLift=Lift*DepthScale*(S.Kind==4 ? 0.38 : 0.47);
				const uint32 H=CellHash(FMath::FloorToInt(P.X/7.0)^D.Cell.Z,
					FMath::FloorToInt(P.Y/7.0)^(D.Cell.Z*31));
				const double Angle=0.19*(double(H&65535u)/65535.0-0.5);
				FVector Tangent=FVector::CrossProduct(FVector::UpVector,N).GetSafeNormal();
				if (Tangent.IsNearlyZero()) Tangent=FVector::CrossProduct(FVector::ForwardVector,N).GetSafeNormal();
				// More samples share the same target optical weight. In regions with
				// pre-existing cover a new sample contributes less opacity; an empty
				// location never receives an exaggerated compensation stamp.
				const float BaseAlpha=FMath::Min(0.34f,DetailOpacity*float(0.76+0.24*S.Coverage));
				const float Alpha=FMath::Clamp(BaseAlpha*float(1.0-0.22*CurrentAlpha/LocalTarget),0.075f,0.34f);
				FGaussianSplatData G;
				if (!MakeSplat(P+N*CenterLift,N,Tangent,Sigma/DetailScale,
					Sigma*(S.Kind==4 ? 0.90 : 0.96)/DetailScale,
					FMath::Clamp(Lift*DepthScale*0.44,2.0,10.0),Alpha,
					float(0.939+0.020*S.Coverage),Angle,G))
				{ ++SnowSplitChildrenFailed; return false; }
				LimitSnowWorldSigma(G,Sigma);
				const FQuat R(double(G.Rotation.X),double(G.Rotation.Y),double(G.Rotation.Z),double(G.Rotation.W));
				const FVector WorldX=GSWorld.TransformVector(R.RotateVector(FVector(double(G.Scale.X),0,0)));
				const FVector WorldY=GSWorld.TransformVector(R.RotateVector(FVector(0,double(G.Scale.Y),0)));
				const double SX=WorldX.Size(),SY=WorldY.Size();
				// MakeSplat may align to source covariance: bound its final ellipse.
				if (SX>SY*1.30 && SX>0.001) G.Scale.X*=float(SY*1.30/SX);
				if (SY>SX*1.30 && SY>0.001) G.Scale.Y*=float(SX*1.30/SY);
				const FVector FinalX=GSWorld.TransformVector(R.RotateVector(FVector(double(G.Scale.X),0,0)));
				const FVector FinalY=GSWorld.TransformVector(R.RotateVector(FVector(0,double(G.Scale.Y),0)));
				const double FinalSX=FinalX.Size(),FinalSY=FinalY.Size();
				if (FinalSX<0.001 || FinalSY<0.001) { ++SnowSplitChildrenFailed; return false; }
				SnowSplitMaxWorldSigma=FMath::Max(SnowSplitMaxWorldSigma,FMath::Max(FinalSX,FinalSY));
				SnowSplats.Add(G);
				FPlacedSurfacePatch Q;
				Q.P=P; Q.N=N;
				Q.AxisX=FinalX/FinalSX; Q.AxisY=FinalY/FinalSY;
				Q.SigmaX=float(FinalSX); Q.SigmaY=float(FinalSY);
				Q.Alpha=G.Opacity; Q.Kind=S.Kind;
				Q.SplatIndex=SnowSplats.Num()-1;
				const int32 Index=Placed.Add(Q);
				PatchBins.FindOrAdd(XYZCell(P,BinCm)).Add(Index);
				SplitSeedCells.Add(XYZCell(P,Cm)); // existing snow-only LOD policy
				++ResampleEmitted;
				ResampleProjectionErrorSum+=FMath::Abs(P.Z-Requested.Z);
				ResampleNormalVariationSum+=Variation;
				if (bBetween) ++ResampleBridges;
				if (bRefill) ++ResampleRefill;
				
				return true;
			};
			// Stage 1: worldwide, unbiased hashed seed distribution.
			for (const FSurfaceDart& D : Darts)
			{
				if (SnowSplats.Num()>=SurfaceCap || ResampleEmitted>=SeedQuota) break;
				if (IsBroad(D.Surface->Kind)) TryPlace(D,D.Surface->P,false,false);
			}
			// Stage 2: verified empty intervals, not more siblings at the same
			// parent. Only graph links on a single height layer can be bridged.
			const FIntVector Links[]={FIntVector(1,0,0),FIntVector(0,1,0),
				FIntVector(1,1,0),FIntVector(1,-1,0)};
			for (const FSurfaceDart& D : Darts)
			{
				if (SnowSplats.Num()>=SurfaceCap || ResampleEmitted>=LinkQuota) break;
				const FContinuousSnowSurface& S=*D.Surface;
				if (!IsBroad(S.Kind)) continue;
				for (const FIntVector& Offset : Links)
				{
					if (SnowSplats.Num()>=SurfaceCap || ResampleEmitted>=LinkQuota) break;
					const FContinuousSnowSurface* Other=nullptr;
					for (int32 DZ=-1; DZ<=1; ++DZ)
					{
						const FContinuousSnowSurface* Test=ReconstructedSnowField.Find(D.Cell+Offset+FIntVector(0,0,DZ));
						if (Test && Compatible(S,*Test)) { Other=Test; break; }
					}
					if (Other) TryPlace(D,(S.P+Other->P)*0.5,true,false);
				}
			}
			// Stage 3: real coverage repair. Score each supported receiver and
			// quarter-link position by the coverage that exists NOW. Sort darkest
			// locations first; re-evaluate upon insertion to prevent duplicate piles.
			struct FRefillSite
			{
				int32 DartIndex=INDEX_NONE;
				FVector P=FVector::ZeroVector;
				float Coverage=0.0f;
				uint32 Tie=0;
				bool bLink=false;
				bool bQuad=false;
			};
			TArray<FRefillSite> Sites;
			// In measurement mode inspect the entire finite supported site set, not
			// an arbitrary first-1.2M diagnostic slice. int64 avoids overflow.
			const int32 MaxSites = bSnowMeasureUncapped ?
				int32(FMath::Min(int64(MAX_int32 - 1024), int64(Darts.Num()) * 8LL)) :
				FMath::Min(1200000,SurfaceCap*4);
			Sites.Reserve(SnowSplats.Num()<SurfaceCap ? FMath::Min(MaxSites,Darts.Num()*3) : 0);
			const auto AddSite = [&](int32 I,const FVector& P,bool bLink,bool bQuad,uint32 Salt)
			{
				if (Sites.Num()>=MaxSites) return;
				const FSurfaceDart& D=Darts[I];
				double Score=0.0;
				if (bGeometricCoverage)
				{
					const double SpatialR2=GeometryR2At(P,D.Surface->N,D.Surface->Kind);
					if (SpatialR2<=GeometricFootprintSq) return;
					// A more distant uncovered site should sort FIRST (ascending score).
					// Previously 1 - distance/footprint was always negative here, so
					// every spatially uncovered site tied at zero regardless of gap size.
					Score=FMath::Clamp(GeometricFootprint/FMath::Max(1.0e-5,FMath::Sqrt(SpatialR2)),0.0,1.0);
				}
				else
				{
					Score=CoverageAt(P,D.Surface->N,D.Surface->Kind);
					if (Score>=TargetCoverage*(D.Surface->Kind==4 ? 0.92 : 1.0)) return;
				}
				FRefillSite Q;
				Q.DartIndex=I; Q.P=P; Q.Coverage=float(Score);
				Q.Tie=CellHash(int32(D.Rank^Salt),int32((D.Rank>>9)^Salt));
				Q.bLink=bLink;
				Q.bQuad=bQuad;
				Sites.Add(Q);
			};
			for (int32 I=0; I<Darts.Num() && SnowSplats.Num()<SurfaceCap && Sites.Num()<MaxSites; ++I)
			{
				const FSurfaceDart& D=Darts[I];
				const FContinuousSnowSurface& S=*D.Surface;
				if (!IsBroad(S.Kind)) continue;
				AddSite(I,S.P,false,false,0x70127u);
				const FIntVector ShortLinks[]={FIntVector(1,0,0),FIntVector(0,1,0)};
				for (const FIntVector& Offset : ShortLinks)
				{
					if (Sites.Num()>=MaxSites) break;
					const FContinuousSnowSurface* Other=nullptr;
					for (int32 DZ=-1;DZ<=1;++DZ)
					{
						const FContinuousSnowSurface* Test=ReconstructedSnowField.Find(D.Cell+Offset+FIntVector(0,0,DZ));
						if (Test && Compatible(S,*Test)) { Other=Test; break; }
					}
					if (!Other) continue;
					// Check the supported segment interior, including its midpoint:
					// v2863 only sampled 1/4 and 3/4, leaving the longest gap untested.
					AddSite(I,FMath::Lerp(S.P,Other->P,0.25),true,false,0x14b32u);
					AddSite(I,FMath::Lerp(S.P,Other->P,0.50),true,false,0x51df7u);
					AddSite(I,FMath::Lerp(S.P,Other->P,0.75),true,false,0x7c5b1u);
				}
				if (!bGeometricCoverage && Source->bSnowResampleQuadRefill && Sites.Num()<MaxSites)
				{
					// All four corners must belong to the SAME snow-receiving
					// height layer and type. Two diagonal points alone are not
					// sufficient evidence to bridge a cliff, stone gap, or roof edge.
					const auto Corner = [&](int32 DX,int32 DY)->const FContinuousSnowSurface*
					{
						for (int32 DZ=-1;DZ<=1;++DZ)
						{
							const FContinuousSnowSurface* Q=ReconstructedSnowField.Find(D.Cell+FIntVector(DX,DY,DZ));
							if (Q && Compatible(S,*Q)) return Q;
						}
						return nullptr;
					};
					const FContinuousSnowSurface* East=Corner(1,0);
					const FContinuousSnowSurface* North=Corner(0,1);
					const FContinuousSnowSurface* Northeast=Corner(1,1);
					if (East && North && Northeast &&
						Compatible(*East,*Northeast) && Compatible(*North,*Northeast) &&
						FMath::Abs(East->P.Z-North->P.Z)<Cm*0.70)
					{
						// Interior is bilinear in measured WORLD coordinates; do
						// not emit an imaginary receiver outside this 2x2 patch.
						const FVector QuadCenter=(S.P+East->P+North->P+Northeast->P)*0.25;
						AddSite(I,QuadCenter,true,true,0xe403bu);
					}
				}
			}
			Sites.Sort([](const FRefillSite& A,const FRefillSite& B)
			{
				if (A.Coverage!=B.Coverage) return A.Coverage<B.Coverage;
				if (A.Tie!=B.Tie) return A.Tie<B.Tie;
				if (A.DartIndex!=B.DartIndex) return A.DartIndex<B.DartIndex;
				if (A.bQuad!=B.bQuad) return A.bQuad<B.bQuad;
				return A.bLink<B.bLink;
			});
			const int32 RefillRounds=FMath::Clamp(Source->SnowResampleRefillRounds,1,3);
			ResampleRefillCandidateSitesTotal = Sites.Num(); // geometric quad samples counted as streamed, below
			// Count-to-convergence: as long as the previous full supported-site sweep
			// inserted at least one real splat, keep trying; stop on no progress.
			for (int32 Round=0;(bSnowMeasureUncapped || Round<RefillRounds) && SnowSplats.Num()<SurfaceCap;++Round)
			{
				const int32 Before=ResampleEmitted;
				++ResampleMeasuredRounds;
				for (const FRefillSite& Site : Sites)
				{
					if (SnowSplats.Num()>=SurfaceCap) break;
					TryPlace(Darts[Site.DartIndex],Site.P,Site.bLink,true);
				}
				if (ResampleEmitted==Before) { bResampleStalled=true; break; }
			}
			// v2866: scan the INTERIOR of each corroborated four-corner
			// surface quad, not just its center. Stream these samples: a multi-
			// million candidate array would defeat uncapped measurement by RAM use.
			// Same type, height layer, four mutually compatible corners, and
			// sample-local MLS support are mandatory; never extrapolate outdoors.
			if (bGeometricCoverage && Source->bSnowResampleQuadRefill)
			{
				const double ConservativeSigma=FMath::Min(MaxSmallSigma,FMath::Max(3.3,Cm*0.26));
				const int32 QuadSubdiv=bFineLayer2869 ?
					FMath::Clamp(FMath::CeilToInt(Cm/FMath::Max(3.0,ConservativeSigma*1.10)),2,9) :
					FMath::Clamp(FMath::CeilToInt(Cm/FMath::Max(3.5,ConservativeSigma*1.40)),2,7);
				int32 SupportedQuadCells=0;
				int32 InsertedQuadSamples=0;
				const auto VisitSupportedQuadSamples = [&](bool bPlace)->void
				{
					for (const FSurfaceDart& D : Darts)
					{
						if (bPlace && SnowSplats.Num()>=SurfaceCap) break;
						const FContinuousSnowSurface& S=*D.Surface;
						if (!IsBroad(S.Kind)) continue;
						const auto Corner = [&](int32 DX,int32 DY)->const FContinuousSnowSurface*
						{
							for (int32 DZ=-1;DZ<=1;++DZ)
							{
								const FContinuousSnowSurface* Q=ReconstructedSnowField.Find(D.Cell+FIntVector(DX,DY,DZ));
								if (Q && Compatible(S,*Q)) return Q;
							}
							return nullptr;
						};
						const FContinuousSnowSurface* East=Corner(1,0);
						const FContinuousSnowSurface* North=Corner(0,1);
						const FContinuousSnowSurface* Northeast=Corner(1,1);
						if (!East || !North || !Northeast ||
							!Compatible(*East,*Northeast) || !Compatible(*North,*Northeast) ||
							FMath::Abs(East->P.Z-North->P.Z)>=Cm*0.70) continue;
						if (bPlace) ++SupportedQuadCells;
						for (int32 J=0;J<QuadSubdiv;++J)
						for (int32 I=0;I<QuadSubdiv;++I)
						{
							if (bPlace && SnowSplats.Num()>=SurfaceCap) return;
							const double U=(double(I)+0.5)/double(QuadSubdiv);
							const double V=(double(J)+0.5)/double(QuadSubdiv);
							const FVector P=FMath::Lerp(FMath::Lerp(S.P,East->P,U),
								FMath::Lerp(North->P,Northeast->P,U),V);
							if (bPlace)
							{
								// TryPlace performs the only per-candidate reprojection,
								// and rejects unsupported surfaces BEFORE writing splats.
								++ResampleRefillCandidateSitesTotal;
								if (TryPlace(D,P,true,true)) ++InsertedQuadSamples;
							}
							else
							{
								FVector RP,RN;
								double Variation=0.0;
								int32 Support=0;
								if (!Reproject(D.Cell,S,P,RP,RN,Variation,Support) || Support<2) continue;
								++ResampleQuadSitesTotal;
								if (!CoveredAt(RP,RN,S.Kind)) ++ResampleUnmetQuadSitesTotal;
							}
						}
					}
				};
				// A one-pass geometric sampler does not waste repeated sweeps on
				// identical points; any blocked samples are disclosed as unmet.
				VisitSupportedQuadSamples(true);
				VisitSupportedQuadSamples(false);
				UE_LOG(LogTemp,Display,TEXT("[Snow2867Spatial] verified_quads=%d subdivisions=%d quad_inserted=%d inspected_sites=%d remaining_sites=%d footprint=%.2f_sigma"),
					SupportedQuadCells,QuadSubdiv,InsertedQuadSamples,ResampleQuadSitesTotal,ResampleUnmetQuadSitesTotal,GeometricFootprint);
			}

			// v2868: optical continuity is intentionally independent of the world-
			// space GEOMETRIC fill criterion. A small, translucent Gaussian can
			// count as spatially present while revealing dark source-GS between
			// neighboring patches. Place SUB-SIGMA bridge grains only where at
			// least two existing patches corroborate the same local receiver layer.
			// No bridge is generated in unsupported space or on a lone parent;
			// bridge centers are reprojected using the existing proxy-priority N.
			if (bGeometricCoverage && Source->bSnowCohesionBridges && !bFineLayer2869)
			{
				const double BridgeStrength = FMath::Clamp(double(Source->SnowCohesionBridgeStrength),0.25,1.25);
				const auto TryOpticalBridge = [&](const FSurfaceDart& D,
					const FVector& Requested,bool bBetween) -> bool
				{
					if (SnowSplats.Num()>=SurfaceCap) return false;
					const FContinuousSnowSurface& S = *D.Surface;
					if (!IsBroad(S.Kind)) return false;
					++ResampleCohesionBridgeCandidates;
					FVector P,N;
					double Variation=0.0;
					int32 Support=0;
					if (!Reproject(D.Cell,S,Requested,P,N,Variation,Support) || (bBetween && Support<2))
						return false;
					const double OpticalNow=CoverageAt(P,N,S.Kind);
					const double Goal=TargetCoverage*(S.Kind==4 ? 0.92 : 1.0);
					if (OpticalNow>=Goal*0.96) return false;
					// Bridge ONLY an already supported interface of existing small snow.
					// Truly uncovered sites belong to TryPlace's geometry path instead.
					if (GeometryR2At(P,N,S.Kind)>GeometricFootprintSq*1.25) return false;
					const double BridgeSigma=FMath::Clamp(MaxSmallSigma*0.72*(S.Kind==4 ? 0.90 : 1.0),
						2.35,MaxSmallSigma*0.82);
					const FIntVector C=XYZCell(P,BinCm);
					int32 NearbyPatches=0;
					for (int32 DZ=-1;DZ<=1;++DZ)
					for (int32 DY=-1;DY<=1;++DY)
					for (int32 DX=-1;DX<=1;++DX)
					{
						const TArray<int32>* Bin=PatchBins.Find(C+FIntVector(DX,DY,DZ));
						if (!Bin) continue;
						for (int32 Index : *Bin)
						{
							const FPlacedSurfacePatch& Q=Placed[Index];
							if (Q.Kind!=S.Kind || FVector::DotProduct(N,Q.N)<(S.Kind==4 ? 0.77 : 0.84)) continue;
							const FVector Diff=P-Q.P;
							const double OffPlane=FVector::DotProduct(Diff,Q.N);
							if (FMath::Abs(OffPlane)>Cm*(S.Kind==4 ? 0.52 : 0.64)) continue;
							const double TX=FVector::DotProduct(Diff,Q.AxisX)/FMath::Max(1.0,double(Q.SigmaX));
							const double TY=FVector::DotProduct(Diff,Q.AxisY)/FMath::Max(1.0,double(Q.SigmaY));
							const double Normalized2=TX*TX+TY*TY;
							const double Tangent2=FMath::Max(0.0,Diff.SizeSquared()-OffPlane*OffPlane);
							if (Q.bBridge && Tangent2<FMath::Square(BridgeSigma*0.86)) return false;
							// Do not stack a bridge directly onto a bright primary core.
							if (!Q.bBridge && Tangent2<FMath::Square(BridgeSigma*0.62)) return false;
							if (!Q.bBridge && Normalized2<=FMath::Square(GeometricFootprint*1.95))
								++NearbyPatches;
						}
					}
					if (NearbyPatches<2) return false;
					const double Lift=FMath::Max(1.4,double(S.HeightCm));
					const double DepthScale=S.Kind==3 ?
						FMath::Clamp(double(Source->SnowFloorDepthScale),0.55,1.0) : 1.0;
					const double AlphaNeeded=FMath::Clamp((Goal-OpticalNow)/FMath::Max(0.15,1.0-OpticalNow),0.0,1.0);
					const float Alpha=float(FMath::Clamp(AlphaNeeded*0.78*BridgeStrength,0.075,0.205));
					FVector Tangent=FVector::CrossProduct(FVector::UpVector,N).GetSafeNormal();
					if (Tangent.IsNearlyZero()) Tangent=FVector::CrossProduct(FVector::ForwardVector,N).GetSafeNormal();
					const uint32 H=CellHash(FMath::FloorToInt(P.X/6.0)^D.Cell.Z,
						FMath::FloorToInt(P.Y/6.0)^(D.Cell.Z*17));
					const double Twist=0.11*(double(H&65535u)/65535.0-0.5);
					FGaussianSplatData G;
					if (!MakeSplat(P+N*Lift*DepthScale*0.33,N,Tangent,BridgeSigma/DetailScale,
						BridgeSigma*(S.Kind==4 ? 0.91 : 0.96)/DetailScale,
						FMath::Clamp(Lift*DepthScale*0.34,1.8,7.0),Alpha,
						float(0.939+0.020*S.Coverage),Twist,G)) return false;
					LimitSnowWorldSigma(G,BridgeSigma);
					const FQuat R(double(G.Rotation.X),double(G.Rotation.Y),double(G.Rotation.Z),double(G.Rotation.W));
					const FVector WorldX=GSWorld.TransformVector(R.RotateVector(FVector(double(G.Scale.X),0,0)));
					const FVector WorldY=GSWorld.TransformVector(R.RotateVector(FVector(0,double(G.Scale.Y),0)));
					const double SX=WorldX.Size(),SY=WorldY.Size();
					if (SX<0.001 || SY<0.001) return false;
					// This low-alpha patch is never allowed to grow into a white disc.
					SnowSplats.Add(G);
					FPlacedSurfacePatch Q;
					Q.P=P;Q.N=N;Q.AxisX=WorldX/SX;Q.AxisY=WorldY/SY;
					Q.SigmaX=float(SX);Q.SigmaY=float(SY);Q.Alpha=G.Opacity;
					Q.Kind=S.Kind;Q.bBridge=true;
					Q.SplatIndex=SnowSplats.Num()-1;
					const int32 Index=Placed.Add(Q);
					PatchBins.FindOrAdd(XYZCell(P,BinCm)).Add(Index);
					SplitSeedCells.Add(XYZCell(P,Cm));
					++ResampleEmitted;
					++ResampleCohesionBridgesPlaced;
					SnowSplitMaxWorldSigma=FMath::Max(SnowSplitMaxWorldSigma,FMath::Max(SX,SY));
					return true;
				};
				// Two explicit stable passes: receiver centers, then supported links.
				// Never seed from the camera, and never join separated height layers.
				for (const FSurfaceDart& D : Darts)
				{
					if (SnowSplats.Num()>=SurfaceCap) break;
					if (IsBroad(D.Surface->Kind)) TryOpticalBridge(D,D.Surface->P,false);
				}
				const FIntVector BridgeLinks[]={FIntVector(1,0,0),FIntVector(0,1,0)};
				for (const FSurfaceDart& D : Darts)
				{
					if (SnowSplats.Num()>=SurfaceCap) break;
					const FContinuousSnowSurface& S=*D.Surface;
					if (!IsBroad(S.Kind)) continue;
					for (const FIntVector& Offset : BridgeLinks)
					{
						if (SnowSplats.Num()>=SurfaceCap) break;
						const FContinuousSnowSurface* Other=nullptr;
						for (int32 DZ=-1;DZ<=1;++DZ)
						{
							const FContinuousSnowSurface* Test=ReconstructedSnowField.Find(D.Cell+Offset+FIntVector(0,0,DZ));
							if (Test && Compatible(S,*Test)) { Other=Test; break; }
						}
						if (Other) TryOpticalBridge(D,(S.P+Other->P)*0.5,true);
					}
				}
				UE_LOG(LogTemp,Display,TEXT("[Snow2868Cohesion] candidates=%d bridges=%d sigma_cap_cm=%.2f bridge_strength=%.2f"),
					ResampleCohesionBridgeCandidates,ResampleCohesionBridgesPlaced,MaxSmallSigma,BridgeStrength);
			}

			// 2.8.69: the confirmed layer, not an individual parent Gaussian, is
			// the placement domain. Sub-cell optical samples target the actual
			// overlapping transmittance, but only inside already reconstructed,
			// same-layer support. Geometric holes still go through TryPlace.
			if (bFineLayer2869 && Source->bSnowCohesionBridges)
			{
				const double FineGoal=FMath::Clamp(double(Source->SnowFineOpticalGoal2869),0.38,0.65);
				const double FineSigma=FMath::Min(MaxSmallSigma*0.72,
					FMath::Clamp(double(Source->SnowFineMicroSigma2869),1.8,3.2));
				const double BridgeStrength=FMath::Clamp(double(Source->SnowCohesionBridgeStrength),0.25,1.25);
				const int32 RequestedFinePasses=FMath::Clamp(Source->SnowFineOpticalPasses2869,1,3);
				const int32 FineSubdiv=FMath::Clamp(FMath::CeilToInt(Cm/FMath::Max(3.5,FineSigma*2.10)),2,6);
				const auto TryFineGrain = [&](const FSurfaceDart& D,
					const FVector& Requested,bool bInterior) -> bool
				{
					if (SnowSplats.Num()>=SurfaceCap) return false;
					const FContinuousSnowSurface& S=*D.Surface;
					if (!IsBroad(S.Kind)) return false;
					++SnowFineCandidates;
					++ResampleCohesionBridgeCandidates;
					FVector P,N;
					double Variation=0.0;
					int32 Support=0;
					if (!Reproject(D.Cell,S,Requested,P,N,Variation,Support) || (bInterior && Support<2))
						return false;
					const double Goal=FineGoal*(S.Kind==4 ? 0.93 : 1.0);
					const double OpticalNow=CoverageAt(P,N,S.Kind);
					if (OpticalNow>=Goal*0.985) return false;
					const double GeometricR2=GeometryR2At(P,N,S.Kind);
					if (GeometricR2>GeometricFootprintSq)
					{
						// Refill a genuinely empty region with the primary small
						// receiver-validated path instead of using a floating bridge.
						return TryPlace(D,Requested,bInterior,true);
					}
					const double LocalSigma=FineSigma*(S.Kind==4 ? 0.90 : 1.0);
					const FIntVector BinKey=XYZCell(P,BinCm);
					int32 NearbyPatches=0;
					for (int32 DZ=-1;DZ<=1;++DZ)
					for (int32 DY=-1;DY<=1;++DY)
					for (int32 DX=-1;DX<=1;++DX)
					{
						const TArray<int32>* Bin=PatchBins.Find(BinKey+FIntVector(DX,DY,DZ));
						if (!Bin) continue;
						for (int32 Index : *Bin)
						{
							const FPlacedSurfacePatch& Q=Placed[Index];
							if (Q.Kind!=S.Kind || FVector::DotProduct(N,Q.N)<(S.Kind==4 ? 0.77 : 0.84)) continue;
							const FVector Delta=P-Q.P;
							const double OffPlane=FVector::DotProduct(Delta,Q.N);
							if (FMath::Abs(OffPlane)>Cm*(S.Kind==4 ? 0.52 : 0.64)) continue;
							const double TX=FVector::DotProduct(Delta,Q.AxisX)/FMath::Max(1.0,double(Q.SigmaX));
							const double TY=FVector::DotProduct(Delta,Q.AxisY)/FMath::Max(1.0,double(Q.SigmaY));
							const double Normalized2=TX*TX+TY*TY;
							const double Tangent2=FMath::Max(0.0,Delta.SizeSquared()-OffPlane*OffPlane);
							const double MinimumSpacing=LocalSigma*(Q.bBridge ? 0.56 : 0.48);
							if (Tangent2<FMath::Square(MinimumSpacing)) return false;
							if (Normalized2<=FMath::Square(GeometricFootprint*2.0)) ++NearbyPatches;
						}
					}
					// Never grow an isolated low-opacity splat into a new airborne
					// patch. One neighbor suffices only with robust MLS support.
					if (NearbyPatches<2 && !(NearbyPatches>=1 && Support>=3)) return false;
					const double Lift=FMath::Max(1.4,double(S.HeightCm));
					const double DepthScale=S.Kind==3 ?
						FMath::Clamp(double(Source->SnowFloorDepthScale),0.55,1.0) : 1.0;
					const double NeededAlpha=FMath::Clamp((Goal-OpticalNow)/FMath::Max(0.10,1.0-OpticalNow),0.0,1.0);
					const float Alpha=float(FMath::Clamp(NeededAlpha*BridgeStrength,0.06,0.24));
					FVector Tangent=FVector::CrossProduct(FVector::UpVector,N).GetSafeNormal();
					if (Tangent.IsNearlyZero()) Tangent=FVector::CrossProduct(FVector::ForwardVector,N).GetSafeNormal();
					const uint32 H=CellHash(FMath::FloorToInt(P.X/4.0)^D.Cell.Z,
						FMath::FloorToInt(P.Y/4.0)^(D.Cell.Z*17));
					const double Twist=0.095*(double(H&65535u)/65535.0-0.5);
					const double SubtleLift=(double((H>>16u)&65535u)/65535.0-0.5)*0.28;
					const double CenterLift=Lift*DepthScale*(S.Kind==4 ? 0.39 : 0.46)+SubtleLift;
					FGaussianSplatData G;
					if (!MakeSplat(P+N*CenterLift,N,Tangent,LocalSigma/DetailScale,
						LocalSigma*(S.Kind==4 ? 0.91 : 0.97)/DetailScale,
						FMath::Clamp(Lift*DepthScale*0.32,1.8,5.5),Alpha,
						float(0.939+0.020*S.Coverage),Twist,G)) return false;
					LimitSnowWorldSigma(G,LocalSigma);
					const FQuat R(double(G.Rotation.X),double(G.Rotation.Y),double(G.Rotation.Z),double(G.Rotation.W));
					const FVector WorldX=GSWorld.TransformVector(R.RotateVector(FVector(double(G.Scale.X),0,0)));
					const FVector WorldY=GSWorld.TransformVector(R.RotateVector(FVector(0,double(G.Scale.Y),0)));
					const double SX=WorldX.Size(),SY=WorldY.Size();
					if (SX<0.001 || SY<0.001) return false;
					SnowSplats.Add(G);
					FPlacedSurfacePatch Q;
					Q.P=P;Q.N=N;Q.AxisX=WorldX/SX;Q.AxisY=WorldY/SY;
					Q.SigmaX=float(SX);Q.SigmaY=float(SY);Q.Alpha=G.Opacity;
					Q.Kind=S.Kind;Q.bBridge=true;
					Q.SplatIndex=SnowSplats.Num()-1;
					const int32 Index=Placed.Add(Q);
					PatchBins.FindOrAdd(XYZCell(P,BinCm)).Add(Index);
					SplitSeedCells.Add(XYZCell(P,Cm));
					SnowSplitMaxWorldSigma=FMath::Max(SnowSplitMaxWorldSigma,FMath::Max(SX,SY));
					++ResampleEmitted;
					++SnowFinePlaced;
					++ResampleCohesionBridgesPlaced;
					return true;
				};
				const auto FineCorner=[&](const FSurfaceDart& D,int32 DX,int32 DY)->const FContinuousSnowSurface*
				{
					const FContinuousSnowSurface& S=*D.Surface;
					for (int32 DZ=-1;DZ<=1;++DZ)
					{
						const FContinuousSnowSurface* Q=ReconstructedSnowField.Find(D.Cell+FIntVector(DX,DY,DZ));
						if (Q && Compatible(S,*Q)) return Q;
					}
					return nullptr;
				};
				for (int32 Pass=0;Pass<RequestedFinePasses && SnowSplats.Num()<SurfaceCap;++Pass)
				{
					const int32 Before=SnowFinePlaced;
					++SnowFinePasses;
					for (const FSurfaceDart& D : Darts)
					{
						if (SnowSplats.Num()>=SurfaceCap) break;
						const FContinuousSnowSurface& S=*D.Surface;
						if (!IsBroad(S.Kind)) continue;
						TryFineGrain(D,S.P,false);
						const FContinuousSnowSurface* East=FineCorner(D,1,0);
						const FContinuousSnowSurface* North=FineCorner(D,0,1);
						if (East)
						{
							TryFineGrain(D,FMath::Lerp(S.P,East->P,0.25),true);
							TryFineGrain(D,FMath::Lerp(S.P,East->P,0.75),true);
						}
						if (North)
						{
							TryFineGrain(D,FMath::Lerp(S.P,North->P,0.25),true);
							TryFineGrain(D,FMath::Lerp(S.P,North->P,0.75),true);
						}
						if (!Source->bSnowResampleQuadRefill || !East || !North) continue;
						const FContinuousSnowSurface* NE=FineCorner(D,1,1);
						if (!NE || !Compatible(*East,*NE) || !Compatible(*North,*NE) ||
							FMath::Abs(East->P.Z-North->P.Z)>=Cm*0.70) continue;
						for (int32 J=0;J<FineSubdiv && SnowSplats.Num()<SurfaceCap;++J)
						for (int32 I=0;I<FineSubdiv && SnowSplats.Num()<SurfaceCap;++I)
						{
							const double U=(double(I)+0.5)/double(FineSubdiv);
							const double V=(double(J)+0.5)/double(FineSubdiv);
							const FVector P=FMath::Lerp(FMath::Lerp(S.P,East->P,U),
								FMath::Lerp(North->P,NE->P,U),V);
							TryFineGrain(D,P,true);
						}
					}
					if (SnowFinePlaced==Before) break;
				}
				// 2.8.70 conservative optical finish: avoid millions more fine
				// Gaussians. Each receiver probes its existing, same-height fine
				// neighbors; request a tiny bounded alpha adjustment on one *existing*
				// fine patch if the local optical field is thin. Primary splats and
				// original GS/mask are never changed. No view- or camera-dependent work.
				const bool bFinish2870=Source->bSnowOpticalFinish2870 &&
					Source->SnowOpticalFinishStrength2870>0.001f;
				if (bFinish2870)
				{
					const double FinishStrength=FMath::Clamp(
						double(Source->SnowOpticalFinishStrength2870),0.0,0.40);
					TArray<float> RequestedAlpha;
					RequestedAlpha.Init(0.0f,Placed.Num());
					for (const FSurfaceDart& D : Darts)
					{
						const FContinuousSnowSurface& S=*D.Surface;
						if (!IsBroad(S.Kind)) continue;
						const double Goal=FineGoal*(S.Kind==4 ? 0.93 : 1.0);
						const double Here=CoverageAt(S.P,S.N,S.Kind);
						if (Here>=Goal) continue;
						++SnowFinishUnmetBefore;
						// Do not "fix" a genuinely empty area by brightening an
						// unrelated Gaussian over a gap, cliff or roof edge.
						if (GeometryR2At(S.P,S.N,S.Kind)>GeometricFootprintSq ||
							Here<FMath::Min(0.17,Goal*0.40)) continue;
						const FIntVector C=XYZCell(S.P,BinCm);
						int32 Neighbors=0;
						int32 BestIndex=INDEX_NONE;
						double BestWeight=0.0;
						for (int32 DZ=-1;DZ<=1;++DZ)
						for (int32 DY=-1;DY<=1;++DY)
						for (int32 DX=-1;DX<=1;++DX)
						{
							const TArray<int32>* Bin=PatchBins.Find(C+FIntVector(DX,DY,DZ));
							if (!Bin) continue;
							for (int32 I : *Bin)
							{
								const FPlacedSurfacePatch& Q=Placed[I];
								if (Q.Kind!=S.Kind ||
									FVector::DotProduct(S.N,Q.N)<(S.Kind==4 ? 0.77 : 0.84)) continue;
								const FVector Delta=S.P-Q.P;
								if (FMath::Abs(FVector::DotProduct(Delta,Q.N))>
									Cm*(S.Kind==4 ? 0.52 : 0.64)) continue;
								const double TX=FVector::DotProduct(Delta,Q.AxisX)/
									FMath::Max(1.0,double(Q.SigmaX));
								const double TY=FVector::DotProduct(Delta,Q.AxisY)/
									FMath::Max(1.0,double(Q.SigmaY));
								const double R2=TX*TX+TY*TY;
								if (R2>1.69) continue;
								++Neighbors;
								// Favor an existing small bridge, not a larger primary
								// Gaussian whose silhouette could create a bright stamp.
								if (!Q.bBridge || Q.SplatIndex==INDEX_NONE ||
									Q.SigmaX>FineSigma*1.15 || Q.SigmaY>FineSigma*1.15) continue;
								const double W=FMath::Exp(-0.5*R2)*double(Q.Alpha);
								if (W>BestWeight) { BestWeight=W; BestIndex=I; }
							}
						}
						// Two overlapping snow patches and a verified receiver prevent
						// isolated highlights outside the supported surface.
						if (Neighbors<2 || BestIndex==INDEX_NONE) continue;
						++SnowFinishSites;
						const FPlacedSurfacePatch& Q=Placed[BestIndex];
						const double Deficit=FMath::Clamp((Goal-Here)/
							FMath::Max(0.10,Goal),0.0,1.0);
						const float Proposed=float(FMath::Min(0.30,
							double(Q.Alpha)*(1.0+FinishStrength*Deficit)));
						RequestedAlpha[BestIndex]=FMath::Max(RequestedAlpha[BestIndex],Proposed);
					}
					for (int32 I=0;I<Placed.Num();++I)
					{
						const float NewAlpha=RequestedAlpha[I];
						FPlacedSurfacePatch& Q=Placed[I];
						if (NewAlpha<=Q.Alpha+0.0005f ||
							!SnowSplats.IsValidIndex(Q.SplatIndex)) continue;
						Q.Alpha=NewAlpha;
						SnowSplats[Q.SplatIndex].Opacity=NewAlpha;
						++SnowFinishChanged;
					}
				}
				// Same world-space sampled receiver set before and after finishing;
				// counts reflect optical alpha proxies, NOT rendered screen pixels.
				for (const FSurfaceDart& D : Darts)
				{
					if (!IsBroad(D.Surface->Kind)) continue;
					const double Goal=FineGoal*(D.Surface->Kind==4 ? 0.93 : 1.0);
					if (CoverageAt(D.Surface->P,D.Surface->N,D.Surface->Kind)>=Goal) continue;
					++SnowFineUnmet;
					if (D.Surface->Kind==3) ++SnowFinishFloor;
					else if (D.Surface->Kind==4) ++SnowFinishStone;
					else ++SnowFinishRoof;
				}
				SnowFinishUnmetAfter=SnowFineUnmet;
				if (!bFinish2870) SnowFinishUnmetBefore=SnowFineUnmet;
				UE_LOG(LogTemp,Display,TEXT("[Snow2870OpticalFinish] mode=%d adjusted_existing=%d eligible_thin_sites=%d before_unmet=%d after_unmet=%d floor=%d roof=%d stone=%d total_snow_delta=0"),
					bFinish2870?1:0,SnowFinishChanged,SnowFinishSites,SnowFinishUnmetBefore,
					SnowFinishUnmetAfter,SnowFinishFloor,SnowFinishRoof,SnowFinishStone);
				UE_LOG(LogTemp,Display,TEXT("[Snow2869FineLayer] max_sigma_cm=%.2f fine_sigma_cm=%.2f target_alpha=%.3f subdivision=%d tested=%d added=%d remaining_center_unmet=%d passes=%d source_proxy_normal=%d"),
					MaxSmallSigma,FineSigma,FineGoal,FineSubdiv,SnowFineCandidates,SnowFinePlaced,SnowFineUnmet,SnowFinePasses,Source->bSnowProxyNormalPriority?1:0);
			}
			// Report the remaining 4-corner supported gaps AFTER the refill,
			// rather than using insertion counts as a stand-in for actual coverage.
			for (const FRefillSite& Site : Sites)
			{
				if (bGeometricCoverage || !Site.bQuad) continue;
				const FSurfaceDart& Dart=Darts[Site.DartIndex];
				FVector Projected,LocalN;
				double Curvature=0.0;
				int32 Support=0;
				if (!Reproject(Dart.Cell,*Dart.Surface,Site.P,Projected,LocalN,Curvature,Support) || Support<2)
					continue; // Unsupported gaps never count as confirmed snow receiver area.
				++ResampleQuadSitesTotal;
				if (!CoveredAt(Projected,LocalN,Dart.Surface->Kind))
					++ResampleUnmetQuadSitesTotal;
			}
			// Use exactly the same coverage threshold for insertion AND final
			// diagnosis. Previously 0.205 was treated as complete while the final
			// checker only counted alpha<0.12, hiding visually empty snow regions.
			for (const FSurfaceDart& D : Darts)
			{
				if (!IsBroad(D.Surface->Kind)) continue;
				const double Goal=TargetCoverage*(D.Surface->Kind==4 ? 0.92 : 1.0);
				const double OpticalHere=CoverageAt(D.Surface->P,D.Surface->N,D.Surface->Kind);
				const bool bOpticalUnmet=OpticalHere<Goal;
				const bool bSpatialUnmet=GeometryR2At(D.Surface->P,D.Surface->N,D.Surface->Kind)>GeometricFootprintSq;
				if (bOpticalUnmet) ++ResampleOpticalUnmetCenters;
				if (bSpatialUnmet) ++ResampleGeometricUnmetCenters;
				const bool bUncovered=bGeometricCoverage ? bSpatialUnmet : bOpticalUnmet;
				if (bUncovered) ++ResampleUncovered;
				// A center can be covered while an interval to its neighbor stays
				// empty. Count such fine-grain supported gaps separately.
				bool bLinkUncovered=false;
				if (IsBroad(D.Surface->Kind))
				{
					const FIntVector Axes[]={FIntVector(1,0,0),FIntVector(0,1,0)};
					for (const FIntVector& Offset : Axes)
					{
						for (int32 DZ=-1;DZ<=1;++DZ)
						{
							const FContinuousSnowSurface* Neighbor=ReconstructedSnowField.Find(D.Cell+Offset+FIntVector(0,0,DZ));
							if (!Neighbor || !Compatible(*D.Surface,*Neighbor)) continue;
							const FVector Midpoint=(D.Surface->P+Neighbor->P)*0.5;
							if (!CoveredAt(Midpoint,D.Surface->N,D.Surface->Kind)) bLinkUncovered=true;
							break;
						}
					}
				}
				if (bUncovered || bLinkUncovered) ++ResampleUncoveredFine;
				
			}
			bResampleCapacityReached = SnowSplats.Num() >= SurfaceCap;
			ResampleBudgetMiss = bSnowMeasureUncapped ? -1 : SurfaceCap-SnowSplats.Num();
			ResampleUnusedSlotsTotal = bSnowMeasureUncapped ? -1 : FMath::Max(0,ResampleBudgetMiss);
			ResampleRefillTotal=ResampleRefill;
			ResampleUncoveredFineTotal=ResampleUncoveredFine;
			SnowMicroAdded+=ResampleEmitted;
			SnowMicroEligible=ResampleEligible;
			UE_LOG(LogTemp,Display,TEXT("[Snow2867Surface] field=%d candidates=%d emitted=%d links=%d gap_refill=%d rejected_close=%d already_covered=%d rejected_support=%d unmet_centers=%d unmet_fine=%d quad_checked=%d quad_unmet=%d unused_surface_slots=%d(-1=uncapped) max_sigma=%.2fcm optical_target=%.3f target_cap=%.2fcm budget=%d"),
				ReconstructedSnowField.Num(),ResampleEligible,ResampleEmitted,ResampleBridges,ResampleRefill,
				ResampleTooClose,ResampleCovered,ResampleUnsupported,ResampleUncovered,ResampleUncoveredFine,
				ResampleQuadSitesTotal,ResampleUnmetQuadSitesTotal,
				ResampleBudgetMiss,SnowSplitMaxWorldSigma,TargetCoverage,MaxSmallSigma,SurfaceCap);
			if (ResampleUncovered>0)
				UE_LOG(LogTemp,Warning,TEXT("[Snow2867Surface] Unmet sampled coverage: %d supported centers remain below the selected spatial/optical target (surface slots=%d; -1 means uncapped). Do not enlarge splats to disguise the gap; compare diagnostic source-only vs generated-snow-only."),
					ResampleUncovered,ResampleBudgetMiss);
			
		}
		UE_LOG(LogTemp, Display, TEXT("[Snow2861LegacySplit] enabled=%d replaced_parents=%d split_seeds=%d split_children=%d failed_children=%d seeds_without_reserved_pair=%d max_world_sigma=%.2fcm baseline_receiver=%d generated_pre_shelf=%d budget=%d"),
			bMicroClusters ? 1 : 0, SnowSplitParentsOmitted, SnowMicroEligible, SnowMicroAdded,
			SnowSplitChildrenFailed, SnowSplitSeedsWithoutPair, SnowSplitMaxWorldSigma,
			ReconstructionOutputNodes, SnowSplats.Num(), SurfaceCap);

		// A second, bounded micro-layer only on already SUPPORTED cliff ledges.
		// This tightens the remaining gaps without re-whitening vertical rock or
		// consuming the broad ground pass's budget. No world-X/Y rows are used.
		// Secondary shelf patches keep their authored fraction, but they must
		// not be disabled merely because uncapped surface sampling passed 250k.
		const int32 ShelfBudget = bSnowMeasureUncapped ?
			FMath::Max(0, FMath::FloorToInt(double(MaxSnowCount) * 0.035)) :
			FMath::Min(FMath::Max(0, MaxSnowCount - SnowSplats.Num()),
				FMath::Max(0, FMath::FloorToInt(double(MaxSnowCount) * 0.035)));
		for (const FSurfaceDart& D : Darts)
		{
			if (ReconstructionShelfMicroCount >= ShelfBudget) break;
			const FContinuousSnowSurface& S = *D.Surface;
			if (S.Kind != 2 || !ChosenSourceCells.Contains(D.Cell)) continue;
			// Only accept a second small patch if an adjacent *same-layer* shelf
			// supports the offset; never extrude unsupported floating snow.
			const FVector Tangent = FVector::CrossProduct(FVector::UpVector, S.N).GetSafeNormal();
			if (Tangent.IsNearlyZero()) continue;
			const uint32 H = D.Rank;
			const double Side = (H & 1u) ? 1.0 : -1.0;
			const FVector Offset=Tangent * (Side * Cm * 0.20);
			const FVector Anchor=S.P + Offset;
			bool bNearShelf=false, bOppositeShelf=false;
			for (int32 DY=-1;DY<=1;++DY)
			for (int32 DX=-1;DX<=1;++DX)
			{
				if (!(DX||DY)) continue;
				for (int32 DZ=-1;DZ<=1;++DZ)
				{
					const FContinuousSnowSurface* Q=ReconstructedSnowField.Find(D.Cell+FIntVector(DX,DY,DZ));
					if (!Q || Q->Kind!=2 || FVector::DotProduct(Q->N,S.N)<=0.78 ||
						FMath::Abs(Q->P.Z-S.P.Z)>=Cm*0.90) continue;
					bNearShelf=true;
					// A second tiny supported grain is allowed ONLY on an
					// observed opposite-side shelf, never across an open wall.
					const double Along=FVector::DotProduct(Q->P-S.P,Tangent)*Side;
					if (Along < -Cm*0.15 && FVector::DistSquared(Q->P,S.P)<FMath::Square(Cm*1.55))
						bOppositeShelf=true;
				}
			}
			if (!bNearShelf) continue;
			FGaussianSplatData G;
			if (MakeSplat(Anchor + FVector::UpVector*FMath::Max(1.0,double(S.HeightCm)*0.53),
				S.N,Tangent, FMath::Min(13.5,Cm*0.54)/DetailScale,
				FMath::Min(8.0,Cm*0.34)/DetailScale,
				FMath::Clamp(double(S.HeightCm)*0.48,1.8,11.0),
				FMath::Min(0.19f,DetailOpacity*0.28f),0.945f,0.0,G))
			{
				if (bCliffCompact2872)
				{
					LimitSnowWorldSigma(G,FMath::Max(2.8,FMath::Min(6.3,Cm*0.31)*CliffScale2872));
					++SnowCliffCompactMicro2872;
					SnowCliffShelfCells2872.Add(XYZCell(Anchor,SourceDetailCellCm));
				}
				SnowSplats.Add(G);
				++ReconstructionShelfMicroCount;
			}
			if (bCliffCompact2872 && bOppositeShelf &&
				ReconstructionShelfMicroCount < ShelfBudget)
			{
				// Fine overlapping shelf grain with a second REAL supported
				// neighbor. Reduced opacity avoids disconnected bright ellipses.
				const FVector SecondAnchor=S.P-Offset*0.80;
				FGaussianSplatData Micro;
				if (MakeSplat(SecondAnchor+FVector::UpVector*FMath::Max(1.0,double(S.HeightCm)*0.52),
					S.N,Tangent,FMath::Min(10.0,Cm*0.40)/DetailScale,
					FMath::Min(7.0,Cm*0.29)/DetailScale,
					FMath::Clamp(double(S.HeightCm)*0.46,1.8,10.0),
					FMath::Min(0.15f,DetailOpacity*0.23f),0.943f,0.17,Micro))
				{
					LimitSnowWorldSigma(Micro,FMath::Max(2.8,5.5*CliffScale2872));
					SnowSplats.Add(Micro);
					++ReconstructionShelfMicroCount;
					++SnowCliffCompactMicro2872;
					SnowCliffShelfCells2872.Add(XYZCell(SecondAnchor,SourceDetailCellCm));
				}
			}
		}
		ReconstructionGroupedNodes = Darts.Num();
		UE_LOG(LogTemp, Log, TEXT("[Snow2855] Surface blue-noise: measured=%d drawn=%d repulsion=%d refill=%d separation=%.1fcm; world XY tiling removed"),
			Darts.Num(), ReconstructionOutputNodes, BlueNoiseRejected, BlueNoiseFills, FirstSeparation);

		if (SnowSplats.Num()<200)
		{
			Fail(FString::Printf(TEXT("2.8.52 실패: 연속 적설 렌더 표면이 %d개입니다."),SnowSplats.Num()));return;
		}
		SourceSurfaceSnowCount=SnowSplats.Num();
		SourceCandidateCount=ReconstructionRawNodes;
		SourceProxyAccepted=ReconstructionRawNodes-ReconstructionProxyRejects;
		SnowRailCount=ReconstructionRailCount;
		SnowCliffShelfCount=ReconstructionShelfCount;
		MainDriftCount=SnowSplats.Num();
		BaseSnowCount=SnowSplats.Num();
	}

	if (SnowSplats.IsEmpty())
	{
		Fail(FString::Printf(TEXT("실패: Snow Gaussian 0. Proxy tri=%d / 평탄 tri=%d / 지지 제외=%d / 차폐 제외=%d. GS 지지 거리 또는 경사 허용을 조정하세요."),
			SourceTriangleCount, EligibleTriangles, UnsupportedSamples, ShelteredSamples));
		return;
	}

	// Important: preserve the existing Nanite/compaction renderer. A Snow asset
	// without its own hierarchy would force the combined scene into the slow
	// non-compacted global sorter when the source asset uses hierarchical LOD.
	int32 SourceSnowCount = SnowSplats.Num();
	// v2844: Keep the dense 2.8.41 blanket. Source-anchored recovery is
	// generated *before* the two-bit mask; the finished geometry then defines
	// the original-GS shading confidence. Both share proxy support and sky rules.
	const double MaskCellCm = FMath::Clamp(EffectiveSpacing * 1.35, 24.0, 85.0);
	const double BaseMaskRadius = FMath::Clamp(FMath::Max(EffectiveSpacing * 0.76,
		SurfaceSigma * DetailScale * 1.65), 15.0, 62.0);
	const double MaskZTolerance = FMath::Clamp(SupportRadius * 0.78, 20.0, 60.0);
	TMap<FIntVector, FVector4f> ActualSnowCells;
	ActualSnowCells.Reserve(FMath::Min(MaxSnowCount, 500000));
	const auto RebuildSnowCells = [&]()
	{
		ActualSnowCells.Reset();
		for (const FGaussianSplatData& S : SnowSplats)
		{
			const FVector P = GSWorld.TransformPosition(FVector(S.Position));
			if (P.ContainsNaN()) continue;
			const FIntVector Key = XYZCell(P, MaskCellCm);
			if (FVector4f* V = ActualSnowCells.Find(Key))
			{
				V->X += float(P.X); V->Y += float(P.Y); V->Z += float(P.Z); V->W += 1.0f;
			}
			else ActualSnowCells.Add(Key, FVector4f(float(P.X), float(P.Y), float(P.Z), 1.0f));
		}
		for (TPair<FIntVector, FVector4f>& It : ActualSnowCells)
		{
			FVector4f& V = It.Value;
			if (V.W > 0.5f) { V.X /= V.W; V.Y /= V.W; V.Z /= V.W; }
		}
	};
	RebuildSnowCells();
	// The retired v2854 floor gap pass has been replaced by the supported
	// world-space, same-height surface resampling coverage pass.
	int32 VerifiedFloorGapRelief = 0;
	int32 FloorGapUnsupported = 0;

	// The obsolete v2847 source-gap recovery generator has been deleted.
	int32 RecoveredBaseCount = 0;
	int32 RecoveryEligible = 0;
	int32 RecoveryRejectedIsolated = 0;
	int32 RecoveryRejectedSloped = 0;
	int32 RecoveryRejectedSky = 0;
	int32 RecoveryRejectedHeight = 0;
	SourceSnowCount = SnowSplats.Num();
	const int32 SourceMaskSplatCount = SourceAsset->GetSplatCount();
	TArray<uint32> PackedSourceSnowMask;
	PackedSourceSnowMask.SetNumZeroed((SourceMaskSplatCount + 15) / 16);
	// 2.8.73: one byte per *source splat*, including source LOD splats.
	// This uses the bounded position chunks already decompressed for the mask:
	// no new full-scene position array and no per-frame procedural field build.
	TArray<uint32> PackedSourceSnowField2873;
	PackedSourceSnowField2873.SetNumZeroed((SourceMaskSplatCount + 3) / 4);
	int32 MaskFull = 0, MaskPartial = 0, MaskFringe = 0;
	int32 MaskUnmatched = 0;
	int32 MaskCellsSearched = 0;
	// v2854: floor-only correspondence diagnostics. No changes to cliffs, stone
	// crowns, roofs, rails, or the renderer's per-view splat projection.
	int32 FloorCenterRecovered = 0;
	int32 FloorCovarianceRecovered = 0;
	int32 FloorLargeOverlapChecked = 0;
	int32 FloorLODInherited = 0;
	int32 FloorLayerRejects = 0;
	struct FOriginalFloorVotes
	{
		uint32 Count = 0;
		uint32 CoverageSum = 0; // sum of baked codes (0..3), per real source splat
	};
	TMap<FIntVector, FOriginalFloorVotes> OriginalFloorVotes;
	const double FloorVoteCellCm = SourceDetailCellCm * 1.45;
	if (bReconstructedSnow)
		OriginalFloorVotes.Reserve(FMath::Min(SourceSurfaceGrid.Num() / 3, 140000));
	const int32 SourceOriginalCount = SourceAsset->GetOriginalSplatCount();
	const FBox ExpandedSnowBounds = ProxySurfaceBounds.ExpandBy(FMath::Max(BaseMaskRadius * 1.9 + MaskZTolerance, ReceiverZTolerance + 6.0));
	{
		// Original + cluster LOD indices use the same packed source indexing.
		// The positions are fetched in bounded chunks, keeping peak CPU memory
		// independent of the scene's full PLY point count.
		constexpr int32 ChunkSize = 65536;
		TArray<FVector3f> Points;
		Points.Reserve(ChunkSize);
		TArray<FQuat4f> MaskRotations;
		TArray<FVector3f> MaskScales;
		MaskRotations.Reserve(ChunkSize);
		MaskScales.Reserve(ChunkSize);
		for (int32 Offset = 0; Offset < SourceMaskSplatCount; Offset += ChunkSize)
		{
			const int32 Count = FMath::Min(ChunkSize, SourceMaskSplatCount - Offset);
			if (!SourceAsset->GetDecompressedPositionRange(Offset, Count, Points))
			{
				Fail(TEXT("실패: 원본 GS 적설 마스크 생성 중 위치 데이터를 읽을 수 없습니다."));
				return;
			}
			// A second bounded covariance pass is needed: XYZ proximity by itself
			// cannot distinguish wall from a snowy horizontal shelf above it.
			const bool bMaskHasCovariance = bReconstructedSnow &&
				SourceAsset->GetSnowSourceCovarianceRange(Offset, Count, MaskRotations, MaskScales);
			if (bReconstructedSnow && !bMaskHasCovariance)
			{
				Fail(TEXT("실패: 눈 표면 마스크의 원본 GS 공분산을 읽을 수 없습니다."));
				return;
			}
			for (int32 J = 0; J < Points.Num(); ++J)
			{
				const FVector3f& LocalP = Points[J];
				if (LocalP.ContainsNaN()) { ++MaskUnmatched; continue; }
				const FVector P = GSWorld.TransformPosition(FVector(LocalP));
				if (!P.ContainsNaN())
				{
					const uint32 FieldUNorm = uint32(FMath::Clamp(
						FMath::RoundToInt(LCCSnowGaussian::SnowAccumulationField(
							P, Source->SnowGaussianMoundScaleCm) * 255.0), 0, 255));
					PackedSourceSnowField2873[(Offset + J) >> 2] |=
						FieldUNorm << (8u * uint32((Offset + J) & 3));
				}
				if (P.ContainsNaN() || !ExpandedSnowBounds.IsInsideOrOn(P))
				{
					
					++MaskUnmatched; continue;
				}
				const FIntVector C = XYZCell(P, MaskCellCm);
				uint32 Code = 0u;
				double BestScore = TNumericLimits<double>::Max();
				// For the vast majority of source splats the center cell is enough.
				// Neighbor cells are checked only if the center has no strong proof.
				const auto Evaluate = [&](const FIntVector& Candidate)
				{
					if (const FVector4f* V = ActualSnowCells.Find(Candidate))
					{
						++MaskCellsSearched;
						const double DZ = FMath::Abs(P.Z - double(V->Z));
						if (DZ > MaskZTolerance) return;
						const double DX = P.X - double(V->X);
						const double DY = P.Y - double(V->Y);
						const double Planar = FMath::Sqrt(DX * DX + DY * DY);
						// Allow small height uncertainty in approximate PLY, but never
						// treat a horizontal cliff or roof-floor column as one surface.
						const double Score = Planar / BaseMaskRadius + DZ / (MaskZTolerance * 1.80);
						BestScore = FMath::Min(BestScore, Score);
					}
				};
				if (bReconstructedSnow)
				{
					// The field is a *physical* XYZ layer. Keep the old strict
					// classification for cliff/rail/crown/roof splats. Broad verified
					// floors alone use receiver ownership instead of one noisy pebble normal.
					const bool bSourceLOD = (Offset + J) >= SourceOriginalCount;
					bool bFloorMatched = false;
					bool bHadFloorEvidence = false;
					FVector OriginalN = FVector::ZeroVector;
					FVector WorldAxes[3] = { FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector };
					bool bAxesValid = false;
					if (J < MaskRotations.Num() && J < MaskScales.Num())
					{
						const FVector3f& Sc = MaskScales[J];
						const FQuat4f& R = MaskRotations[J];
						if (!Sc.ContainsNaN() && !R.ContainsNaN())
						{
							const FVector AbsSc(FMath::Abs((double)Sc.X), FMath::Abs((double)Sc.Y), FMath::Abs((double)Sc.Z));
							const double Small = FMath::Min3(AbsSc.X, AbsSc.Y, AbsSc.Z);
							const double Large = FMath::Max3(AbsSc.X, AbsSc.Y, AbsSc.Z);
							if (Large > 0.00001 && Small / Large < 0.72)
							{
								const FVector Axis = AbsSc.X <= AbsSc.Y && AbsSc.X <= AbsSc.Z ? FVector::ForwardVector :
									(AbsSc.Y <= AbsSc.Z ? FVector::RightVector : FVector::UpVector);
								const FQuat Q((double)R.X, (double)R.Y, (double)R.Z, (double)R.W);
								const FQuat NQ = Q.GetNormalized();
								const FVector LocalN = NQ.RotateVector(Axis);
								OriginalN = GSWorld.GetRotation().RotateVector(FVector(
									LocalN.X / SafeGSScale.X, LocalN.Y / SafeGSScale.Y, LocalN.Z / SafeGSScale.Z)).GetSafeNormal();
								if (OriginalN.Z < 0.0) OriginalN *= -1.0;
								// Transform all three covariance semi-axes through the actual
								// non-uniformly scaled component; mean scale is not correct.
								WorldAxes[0] = GSWorld.TransformVector(NQ.RotateVector(FVector(AbsSc.X, 0.0, 0.0)));
								WorldAxes[1] = GSWorld.TransformVector(NQ.RotateVector(FVector(0.0, AbsSc.Y, 0.0)));
								WorldAxes[2] = GSWorld.TransformVector(NQ.RotateVector(FVector(0.0, 0.0, AbsSc.Z)));
								bAxesValid = !WorldAxes[0].ContainsNaN() && !WorldAxes[1].ContainsNaN() && !WorldAxes[2].ContainsNaN();
							}
						}
					}
					const FIntVector Cell = XYZCell(P, SourceDetailCellCm);
					const FSourceSurfaceCell* MeasuredCell = SourceSurfaceGrid.Find(Cell);
					const bool bMeasuredFloor = MeasuredCell && MeasuredCell->Samples >= 2 &&
						MeasuredCell->WeightSum > 0.001 && MeasuredCell->NormalSum.Z >= 0.58 &&
						MeasuredCell->VerifiedUpWeight / MeasuredCell->WeightSum >= 0.22;
					// An unreliable individual covariance is allowed only when REAL
					// neighboring GS material also proves that this is an upward layer.
					// An isolated wall splat beside a path cannot borrow its snow.
					const bool bFloorSource = !bSourceLOD && (OriginalN.Z >= 0.44 || bMeasuredFloor);
					const double FloorMaxZ = FMath::Min(30.0, SourceDetailCellCm * 1.24);
					const auto FloorCoverageAt = [&](const FVector& Query, bool& OutSupported) -> double
					{
						double BestFloor = 0.0;
						const FIntVector QCell = XYZCell(Query, SourceDetailCellCm);
						for (int32 DZ = -1; DZ <= 1; ++DZ)
						for (int32 DY = -1; DY <= 1; ++DY)
						for (int32 DX = -1; DX <= 1; ++DX)
						{
							const FContinuousSnowSurface* S = ReconstructedSnowField.Find(QCell + FIntVector(DX,DY,DZ));
							if (!S || S->Kind != 3 ||
				S->N.Z < 0.78) continue;
							const double Z = FMath::Abs(Query.Z - S->P.Z);
							if (Z > FloorMaxZ) continue;
							const double XY = FVector2D(Query.X-S->P.X, Query.Y-S->P.Y).Size();
							if (XY > SourceDetailCellCm * 1.95) continue;
							OutSupported = true;
							const double FootprintWeight = FMath::Clamp(1.11 - XY / (SourceDetailCellCm * 2.8), 0.0, 1.0);
							const double HeightWeight = FMath::Clamp(1.07 - Z / (FloorMaxZ * 2.20), 0.0, 1.0);
							BestFloor = FMath::Max(BestFloor, double(S->Coverage) * FootprintWeight * HeightWeight);
						}
						return BestFloor;
					};
					double Best = 0.0;
					// Strict legacy non-ground correspondence, unchanged in character.
					for (int32 DY=-1; DY<=1; ++DY)
					for (int32 DX=-1; DX<=1; ++DX)
					for (int32 DZ=-1; DZ<=1; ++DZ)
					{
						const FContinuousSnowSurface* S = ReconstructedSnowField.Find(Cell + FIntVector(DX,DY,DZ));
						if (!S || S->Kind == 3) continue;
						if (OriginalN.IsNearlyZero() || OriginalN.Z < 0.27) continue;
						const double RequiredUp = S->Kind == 4 ? 0.47 : 0.63;
						if (OriginalN.Z < RequiredUp) continue;
						const double DirectionMatch = FVector::DotProduct(OriginalN, S->N);
						const double MatchMin = S->Kind == 4 ? 0.66 : (S->Kind == 0 ? 0.79 : 0.74);
						if (DirectionMatch < MatchMin) continue;
						const double OffPlane = FMath::Abs(FVector::DotProduct(P-S->P, S->N));
						if (OffPlane > FMath::Max(14.0, SourceDetailCellCm * 0.80)) continue;
						const double Vertical = FMath::Abs(P.Z-S->P.Z);
						if (Vertical > FMath::Max(28.0, SourceDetailCellCm * 1.45)) continue;
						const double Planar = FVector2D(P.X-S->P.X, P.Y-S->P.Y).Size();
						if (Planar > SourceDetailCellCm * 1.9) continue;
						const double Weight = FMath::Clamp(1.0-Planar/(SourceDetailCellCm*2.4),0.0,1.0) *
							FMath::Clamp(1.0-Vertical/(SourceDetailCellCm*2.5),0.0,1.0) *
							FMath::Clamp((DirectionMatch-(S->Kind==4 ? 0.59 : 0.70)) /
								(S->Kind==4 ? 0.34 : 0.28),0.0,1.0);
						Best = FMath::Max(Best, double(S->Coverage)*Weight);
					}
					// Floor-only: the center belongs to a supported proxy receiver, so
					// do not lose paving stones just because their fitted normals wobble.
					double FloorScore = 0.0;
					if (bFloorSource)
					{
						FloorScore = FloorCoverageAt(P, bHadFloorEvidence);
						if (bHadFloorEvidence && (OriginalN.IsNearlyZero() || OriginalN.Z < 0.63))
							++FloorCenterRecovered;
						if (bAxesValid && bHadFloorEvidence)
						{
							int32 First = INDEX_NONE, Second = INDEX_NONE;
							double FirstSize = 0.0, SecondSize = 0.0;
							for (int32 Axis = 0; Axis < 3; ++Axis)
							{
								const double Size = FVector2D(WorldAxes[Axis].X, WorldAxes[Axis].Y).Size();
								if (Size > FirstSize) {Second = First; SecondSize = FirstSize; First = Axis; FirstSize = Size;}
								else if (Size > SecondSize) {Second = Axis; SecondSize = Size;}
							}
							if (FirstSize > SourceDetailCellCm * 1.20)
							{
								++FloorLargeOverlapChecked;
								double FootprintTotal = FloorScore;
								int32 Probes = 1;
								const int32 Axes[2] = { First, Second };
								for (int32 Slot = 0; Slot < 2; ++Slot)
								{
									const int32 Axis = Axes[Slot];
									if (Axis == INDEX_NONE) continue;
									const double XYLen = FVector2D(WorldAxes[Axis].X, WorldAxes[Axis].Y).Size();
									if (XYLen < SourceDetailCellCm * 0.85) continue;
									const FVector Delta = FVector(WorldAxes[Axis].X, WorldAxes[Axis].Y, 0.0) *
										(FMath::Min(XYLen * 0.8, SourceDetailCellCm * 2.0) / FMath::Max(0.001, XYLen));
									for (int32 Sign = -1; Sign <= 1; Sign += 2)
									{
										bool bOffsetSupported = false;
										FootprintTotal += FloorCoverageAt(P + Delta * double(Sign), bOffsetSupported);
										++Probes;
									}
								}
								const double Average = FootprintTotal / double(Probes);
								// A wide ellipse straddling uncovered ground must not paint
								// its whole footprint bright white from one covered center.
								const double Blended = FMath::Lerp(FloorScore, Average, 0.48);
								if (Blended > FloorScore + 0.03) ++FloorCovarianceRecovered;
								FloorScore = Blended;
							}
						}
						Best = FMath::Max(Best, FloorScore);
					}
					if (!bFloorSource && (OriginalN.IsNearlyZero() || OriginalN.Z < 0.27))
						++ReconstructionMaskNormalRejects;
					if (Best > 0.51) {Code = 3u; ++MaskFull;}
					else if (Best > 0.31) {Code = 2u; ++MaskPartial;}
					else if (Best > 0.15) {Code = 1u; ++MaskFringe;}
					else ++MaskUnmatched;
					if (Code > 0u && bHadFloorEvidence && FloorScore > 0.15) {++ReconstructionFloorTintCount; bFloorMatched = true;}
					if (bHadFloorEvidence && Code == 0u) ++FloorLayerRejects;
					// Store ground-only original votes; synthetic LOD covariance is
					// not proof of a surface and will never add to this evidence.
					if (!bSourceLOD && bFloorMatched)
					{
						FOriginalFloorVotes& Vote = OriginalFloorVotes.FindOrAdd(XYZCell(P, FloorVoteCellCm));
						++Vote.Count;
						Vote.CoverageSum += Code;
					}
					if (bSourceLOD)
					{
						// Source LODs may have an identity rotation; inherit their
						// local REAL floor's already-tested coverage, not the LOD normal.
						bool bSameFloor = false;
						const double LocalCoverage = FloorCoverageAt(P, bSameFloor);
						if (bSameFloor && LocalCoverage > 0.28)
						{
							const FIntVector VoteCell = XYZCell(P, FloorVoteCellCm);
							uint32 Sum = 0u, CountVotes = 0u;
							for (int32 DZ=-1; DZ<=1; ++DZ)
							for (int32 DY=-1; DY<=1; ++DY)
							for (int32 DX=-1; DX<=1; ++DX)
							{
								if (const FOriginalFloorVotes* V = OriginalFloorVotes.Find(VoteCell+FIntVector(DX,DY,DZ)))
								{
									Sum += V->CoverageSum;
									CountVotes += V->Count;
								}
							}
							if (CountVotes >= 6)
							{
								const double AverageCode = double(Sum) / double(CountVotes);
								const uint32 LocalCode = AverageCode > 2.30 && LocalCoverage > 0.50 ? 3u :
									(AverageCode > 1.45 ? 2u : (AverageCode > 0.70 ? 1u : 0u));
								if (LocalCode > Code)
								{
									// The old code was already included in mask counters.
									if (Code == 0u) --MaskUnmatched;
									else if (Code == 1u) --MaskFringe;
									else if (Code == 2u) --MaskPartial;
									Code = LocalCode;
									if (Code == 1u) ++MaskFringe;
									else if (Code == 2u) ++MaskPartial;
									else ++MaskFull;
									++FloorLODInherited;
								}
							}
						}
					}
				}
				else
				{
				Evaluate(C);
				if (BestScore > 0.86)
				{
					for (int32 Z = -1; Z <= 1 && BestScore > 0.86; ++Z)
					for (int32 Y = -1; Y <= 1 && BestScore > 0.86; ++Y)
					for (int32 X = -1; X <= 1 && BestScore > 0.86; ++X)
					{
						if (X || Y || Z) Evaluate(C + FIntVector(X, Y, Z));
					}
				}
				if (BestScore <= 0.86) { Code = 3u; ++MaskFull; }
				else if (BestScore <= 1.27) { Code = 2u; ++MaskPartial; }
				else if (BestScore <= 1.68) { Code = 1u; ++MaskFringe; }
				else
				{
					// Never mistake an arbitrary nearby white splat for real snow.
					// A source splat can receive a *subtle* frost fringe without
					// dense added geometry ONLY if it belongs to an actual exposed
					// upward proxy surface. No proof => no white paint.
					int32 ReceiverIndex = INDEX_NONE;
					FVector Surface;
					if (!bSourceDrivenSnow && DriftField(P) >= PatchThreshold - 0.11 &&
						FindSnowReceiver(P, ReceiverIndex, Surface))
					{
						// This source splat is a real measured point. Its fine XYZ cell
						// validates local surface support without a full fit for every
						// source point in the multi-million-splat packed-mask loop.
						const FSourceSurfaceCell* LocalCell = SourceSurfaceGrid.Find(XYZCell(P, SourceDetailCellCm));
						if (LocalCell && LocalCell->Samples >= 2 &&
							FMath::Abs(P.Z - Surface.Z) <= SourceHeightToleranceCm * 0.65)
						{ Code = 1u; ++MaskFringe; }
						else ++MaskUnmatched;
					}
					else ++MaskUnmatched;
				}
				}
				
				PackedSourceSnowMask[(Offset + J) >> 4] |= (Code << (2u * uint32((Offset + J) & 15)));
			}
		}
	}
	if (MaskFull + MaskPartial < 10)
	{
		Fail(TEXT("실패: 생성된 눈과 원본 GS의 3D 위치가 일치하지 않습니다. 프록시/GS 변환을 확인하세요."));
		return;
	}

	FGaussianClusterHierarchy Hierarchy;
	FGaussianClusterBuilder::FBuildSettings ClusterSettings;
	ClusterSettings.bReorderSplats = true;
	ClusterSettings.bGenerateLOD = true;
	ClusterSettings.SplatsPerCluster = 128;
	if (!FGaussianClusterBuilder::BuildClusterHierarchy(SnowSplats, Hierarchy, ClusterSettings))
	{
		Fail(TEXT("실패: Snow Gaussian cluster hierarchy 생성 오류"));
		return;
	}
	if (Hierarchy.LODSplats.Num() > 0)
	{
		// Generic cluster builder zeroes SH on merged LODs. Restore SNOW-only
		// angular response from a nearby original splat without touching shared
		// GaussianClusterBuilder or changing the main scene's LOD behavior.
		const double SHCell = FMath::Max(18.0, SurfaceSigma * 3.0);
		TMap<FIntVector, int32> FineLookup;
		FineLookup.Reserve(FMath::Min(SourceSnowCount, 200000));
		for (int32 I = 0; I < SourceSnowCount; ++I)
		{
			FineLookup.FindOrAdd(XYZCell(GSWorld.TransformPosition(FVector(SnowSplats[I].Position)), SHCell), I);
		}
		for (FGaussianSplatData& Lod : Hierarchy.LODSplats)
		{
			const FVector WorldP = GSWorld.TransformPosition(FVector(Lod.Position));
			const FIntVector C = XYZCell(WorldP, SHCell);
			double Best = TNumericLimits<double>::Max();
			int32 BestIndex = INDEX_NONE;
			for (int32 Z = -1; Z <= 1; ++Z)
			for (int32 Y = -1; Y <= 1; ++Y)
			for (int32 X = -1; X <= 1; ++X)
			{
				if (const int32* Index = FineLookup.Find(C + FIntVector(X, Y, Z)))
				{
					const double D = FVector::DistSquared(WorldP,
						GSWorld.TransformPosition(FVector(SnowSplats[*Index].Position)));
					if (D < Best) { Best = D; BestIndex = *Index; }
				}
			}
			if (BestIndex != INDEX_NONE)
			{
				// Generic LOD also replaces covariance rotation with identity.
				// Snow is a thin 3D surface layer: preserve its local tangent frame
				// so low-detail roof/cliff splats still reproject anisotropically.
				Lod.Rotation = SnowSplats[BestIndex].Rotation;
				const FVector3f& ReferenceScale = SnowSplats[BestIndex].Scale;
				Lod.Scale.X = FMath::Min(Lod.Scale.X, ReferenceScale.X * 1.30f);
				Lod.Scale.Y = FMath::Min(Lod.Scale.Y, ReferenceScale.Y * 1.30f);
				Lod.Scale.Z = FMath::Min(Lod.Scale.Z, ReferenceScale.Z * 1.30f);
				// 2.8.72: a generic merged LOD can re-inflate a compact shelf
				// into a white cliff disk. Only use SAME XYZ receiver evidence
				// (not XY proximity, so floors under ledges remain separate).
				if (bCliffCompact2872 &&
					SnowCliffShelfCells2872.Contains(XYZCell(WorldP,SourceDetailCellCm)))
					LimitSnowWorldSigma(Lod,FMath::Max(4.8,8.0*CliffScale2872));
				if (bMicroClusters)
				{
					// Re-aggregating split grains into a giant Gaussian LOD would
					// recreate the original frontal-view artifact. Apply the tight
					// cap only when the nearest fine particle is itself small
					// and has an upward-facing tangent frame. Cliff and rail LODs
					// retain the legacy covariance policy.
					const FQuat R(double(SnowSplats[BestIndex].Rotation.X),
						double(SnowSplats[BestIndex].Rotation.Y),
						double(SnowSplats[BestIndex].Rotation.Z),
						double(SnowSplats[BestIndex].Rotation.W));
					const FVector RefNormal = GSWorld.TransformVector(R.RotateVector(FVector::UpVector)).GetSafeNormal();
					const double RefWorldX = GSWorld.TransformVector(R.RotateVector(FVector(double(ReferenceScale.X),0,0))).Size();
					const double RefWorldY = GSWorld.TransformVector(R.RotateVector(FVector(0,double(ReferenceScale.Y),0))).Size();
					// The nearest fine particle must belong to a true split MLS
					// receiver. Angular tests alone would modify cliff LOD.
					const FIntVector SeedC = XYZCell(
						GSWorld.TransformPosition(FVector(SnowSplats[BestIndex].Position)), SourceDetailCellCm);
					bool bMatchedSplit = false;
					for (int32 DZ = -1; DZ <= 1 && !bMatchedSplit; ++DZ)
					for (int32 DY = -1; DY <= 1 && !bMatchedSplit; ++DY)
					for (int32 DX = -1; DX <= 1 && !bMatchedSplit; ++DX)
						bMatchedSplit = SplitSeedCells.Contains(SeedC + FIntVector(DX,DY,DZ));
					if (bMatchedSplit && RefNormal.Z > 0.62 &&
						FMath::Max(RefWorldX, RefWorldY) <= (bSurfaceResampling ?
						FMath::Min(SnowSplitWorldCapCm,(double)Source->SnowResampleWorldSigmaCm)*1.12 :
						MicroSigmaCapCm*0.77))
						LimitSnowWorldSigma(Lod, bSurfaceResampling ?
						FMath::Min(SnowSplitWorldCapCm,(double)Source->SnowResampleWorldSigmaCm)*1.04 :
						SnowSplitWorldCapCm*1.10);
				}
				for (int32 I = 0; I < 3; ++I)
					Lod.SH[I] = SnowSplats[BestIndex].SH[I] * 0.62f;
				if (bReconstructedSnow)
				{
					// A generic cluster centroid may bridge unrelated cliff ledges
					// or two different roof heights. No new large, uniformly rotated
					// ellipsoid is allowed far away from the fine measured snow.
					// Preserve the cluster bookkeeping for MRQ; only the synthetic
					// snow LOD opacity/covariance is reduced when unsupported.
					const double FineDistance = FMath::Sqrt(Best);
					if (FineDistance > SourceDetailCellCm * 3.8)
					{
						Lod.Opacity = FMath::Min(Lod.Opacity, DetailOpacity * 0.16f);
						Lod.Scale.X = FMath::Min(Lod.Scale.X, ReferenceScale.X * 1.05f);
						Lod.Scale.Y = FMath::Min(Lod.Scale.Y, ReferenceScale.Y * 1.05f);
						Lod.Scale.Z = FMath::Min(Lod.Scale.Z, ReferenceScale.Z * 1.05f);
					}
				}

			}
		}
		// Generic LOD merging averages scales and resets rotation to identity.
		// Enforce a snow-only minimum Z radius so distant snow does not revert
		// to a thin card when a panorama pane selects a coarser cluster.
		for (FGaussianSplatData& LODSplat : Hierarchy.LODSplats)
		{
			// Snow LOD may blend many far-apart feature Gaussians. Preserve a
			// finite footprint so it cannot grow into an entire white cliff disk.
			// 2.8.68: prevent small generated snow from becoming an oversized
			// snow-only LOD sheet. Generic scene GS and source-GS LOD are unchanged.
			const double SnowLODWorldCap = bSurfaceResampling ?
				(Source->bSnowFineLayer2869 && Source->bSnowResampleGeometricCoverage ?
					FMath::Max(10.0,double(Source->SnowResampleWorldSigmaCm) *
						FMath::Clamp(double(Source->SnowFineSizeFactor),0.65,1.0) *
						FMath::Clamp(double(Source->SnowFineLayerScale2869),0.65,1.0) * 2.0) :
					FMath::Max(12.5, double(Source->SnowResampleWorldSigmaCm) *
						FMath::Clamp(double(Source->SnowFineSizeFactor), 0.65, 1.0) * 2.4)) :
				FMath::Max(16.0, SurfaceSigma * 2.2);
			const float MaxLocalSigma = float(SnowLODWorldCap /
				FMath::Max(0.001, GSWorld.GetScale3D().GetAbsMax()));
			LODSplat.Scale.X = FMath::Min(LODSplat.Scale.X, MaxLocalSigma);
			LODSplat.Scale.Y = FMath::Min(LODSplat.Scale.Y, MaxLocalSigma);
			const float XYRadius = FMath::Min(LODSplat.Scale.X, LODSplat.Scale.Y);
			LODSplat.Scale.Z = FMath::Max(LODSplat.Scale.Z, XYRadius * (0.30f + 0.12f * float(VolumeStrength)));
			LODSplat.Opacity = FMath::Min(LODSplat.Opacity, DetailOpacity);
		}
		for (FGaussianCluster& Cluster : Hierarchy.Clusters)
			if (Cluster.LODSplatCount > 0) Cluster.LODSplatStartIndex += SourceSnowCount;
		SnowSplats.Append(Hierarchy.LODSplats);
		Hierarchy.LODSplats.Empty();
	}

#if WITH_EDITOR
	Modify();
	Source->Modify();
	SnowGaussianComponent->Modify();
#endif
	UGaussianSplatAsset* NewSnowAsset = NewObject<UGaussianSplatAsset>(this, NAME_None, RF_Transactional);
	NewSnowAsset->SHBands = 1; // SH1: per-view snow backscatter, world-anchored coefficients.
	NewSnowAsset->OriginalSplatCount = SourceSnowCount;
	NewSnowAsset->ClusterHierarchy = MoveTemp(Hierarchy);
	NewSnowAsset->InitializeFromSplatData(SnowSplats, EGaussianQualityLevel::Medium);
	NewSnowAsset->bEnableNanite = true; // SetNaniteEnabled() is editor-only in the original asset API.
	if (!NewSnowAsset->IsValid())
	{
		Fail(TEXT("실패: Gaussian Asset 압축/초기화 오류"));
		return;
	}
	// 2.8.73: also precompute the field for the *generated* Snow asset.
	// This is the large 2.3M+ layer: reducing its live per-splat hash cost is
	// just as important as the original GS. Read final compressed/indexed asset
	// positions in chunks so a cluster reorder or LOD insertion stays aligned.
	TArray<uint32> PackedGeneratedSnowField2873;
	const int32 GeneratedFieldCount2873 = NewSnowAsset->GetSplatCount();
	PackedGeneratedSnowField2873.SetNumZeroed((GeneratedFieldCount2873 + 3) / 4);
	{
		constexpr int32 FieldChunkSize = 65536;
		TArray<FVector3f> SnowDecodedPositions;
		SnowDecodedPositions.Reserve(FieldChunkSize);
		for (int32 Offset = 0; Offset < GeneratedFieldCount2873; Offset += FieldChunkSize)
		{
			const int32 Count = FMath::Min(FieldChunkSize, GeneratedFieldCount2873 - Offset);
			if (!NewSnowAsset->GetDecompressedPositionRange(Offset, Count, SnowDecodedPositions))
			{
				Fail(TEXT("실패: Snow Asset 월드 적설 필드 위치 데이터 확인 오류"));
				return;
			}
			for (int32 J = 0; J < SnowDecodedPositions.Num(); ++J)
			{
				const FVector3f& LocalP = SnowDecodedPositions[J];
				if (LocalP.ContainsNaN()) continue;
				const FVector WorldP = GSWorld.TransformPosition(FVector(LocalP));
				if (WorldP.ContainsNaN()) continue;
				const uint32 FieldByte = uint32(FMath::Clamp(
					FMath::RoundToInt(LCCSnowGaussian::SnowAccumulationField(
						WorldP, Source->SnowGaussianMoundScaleCm) * 255.0), 0, 255));
				PackedGeneratedSnowField2873[(Offset + J) >> 2] |=
					FieldByte << (8u * uint32((Offset + J) & 3));
			}
		}
	}

	// Swap atomically only after successful generation. Store the UAsset nested
	// inside this actor for level serialization / PIE+MRQ world duplication.
	SnowGaussianComponent->SetVisibility(false, false);
	SnowGaussianComponent->SetHiddenInGame(true);
	SnowGaussianComponent->SetSplatAsset(nullptr);
	BakedSnowGaussianAsset = NewSnowAsset;
	Source->SnowCoveragePackedMask = MoveTemp(PackedSourceSnowMask);
	Source->SnowBakedFieldPacked2873 = MoveTemp(PackedSourceSnowField2873);
	Source->SnowBakedFieldSourceAsset2873 = SourceAsset;
	Source->SnowBakedFieldSplatCount2873 = SourceMaskSplatCount;
	Source->SnowBakedFieldPatternCm2873 = Source->SnowGaussianMoundScaleCm;
	Source->SnowBakedFieldLocalToWorld2873 = GSWorld;
	Source->SnowBakedFieldSummary2873 = FString::Printf(
		TEXT("2.8.73 원본 GS별 적설 패턴 %d개 베이크, %.1fMB 압축\n")
		TEXT("패턴 %.1fcm / 기존 2-bit 대응 마스크 보존\n")
		TEXT("캐시가 없거나 소스 변환·패턴 변경 시 종전 실시간 계산"),
		SourceMaskSplatCount, double(Source->SnowBakedFieldPacked2873.Num() * sizeof(uint32)) / 1048576.0,
		Source->SnowBakedFieldPatternCm2873);
	Source->SnowCoverageMaskSourceAsset = SourceAsset;
	Source->SnowCoverageMaskSourceCount = SourceMaskSplatCount;
	Source->RefreshSnowCoverageMaskRenderState(); // uploads the serialized mask on its new SceneProxy / MRQ duplicate
	SnowGaussianComponent->SetSplatAsset(BakedSnowGaussianAsset.Get());
	SnowGaussianComponent->SetRelativeTransform(FTransform::Identity);
	SnowGaussianComponent->SnowBakedFieldPacked2873 = MoveTemp(PackedGeneratedSnowField2873);
	SnowGaussianComponent->SnowBakedFieldSourceAsset2873 = BakedSnowGaussianAsset.Get();
	SnowGaussianComponent->SnowBakedFieldSplatCount2873 = GeneratedFieldCount2873;
	SnowGaussianComponent->SnowBakedFieldPatternCm2873 = Source->SnowGaussianMoundScaleCm;
	SnowGaussianComponent->SnowBakedFieldLocalToWorld2873 = GSWorld;
	SnowGaussianComponent->SnowBakedFieldSummary2873 = FString::Printf(
		TEXT("2.8.73 눈 레이어 베이크 필드 %d개 (%.1fMB)"),
		GeneratedFieldCount2873,
		double(SnowGaussianComponent->SnowBakedFieldPacked2873.Num() * sizeof(uint32)) / 1048576.0);
	SnowGaussianComponent->RefreshSnowCoverageMaskRenderState();
	SnowGaussianComponent->RenderProfile = Source->RenderProfile;
	SnowGaussianComponent->SHOrder = 1;
	SnowGaussianComponent->bReceiveUnrealLights = false;
	SnowGaussianComponent->bForceFullSortEveryFrame = Source->bForceFullSortEveryFrame;
	SnowGaussianComponent->bDisableLODInReference = Source->bDisableLODInReference;

	Source->SnowCliffCompactSummary2872 = FString::Printf(
		TEXT("2.8.72 절벽 축소 %s / 대상 턱 본입자 %d / 보강입자 %d\n기본 바닥·지붕·원본 GS는 크기 변경 없음\n원본 커버 보정은 눈 재생성 없이 즉시 적용"),
		bCliffCompact2872?TEXT("ON"):TEXT("OFF"),SnowCliffCompactMain2872,SnowCliffCompactMicro2872);
	Source->bUseSnowGaussianLayer = true;
	// Serialized into the scene so editor, PIE, and MRQ all use the SAME
	// world-space field cut as the baked geometry. No camera-dependent masks.
	Source->SnowGaussianBakedPatchThreshold = (float)PatchThreshold;
	Source->SnowGaussianGeneratedCount = SourceSnowCount;
	Source->SnowGaussianBuildStatus = FString::Printf(
		TEXT("[2.8.55 비재구성 비교 경로] 생성 %d (원본 표면 직접 %d / 하부 %d / 바닥 보완 %d) / 원본 후보 %d / 프록시 검증 %d / 프록시 없는 원본면 %d / 고립 제외 %d / 위쪽 차폐·경사 제외 %d / 높이오차 제외 %d / 원본 GS 표면셀 %d / 간격 입력 %.1fcm / 셀 크기 %.1fcm / 적설 두께 %.1fcm / 볼륨 강도 %.2f / 마스크 완전 %d / 부분 %d / 경계 %d / 미매칭 %d / 마스크 %.1fKB / LOD %d / 절벽 평면 제외 %d / 절벽 선반 %d / 난간 상단 %d (난간 후보 %d) / 풍하 후보 %d / 바람 %.0f도 강도 %.2f / 희박 바닥 통과 %d / 근접 복원 후보 %d / 복원 거부(경사/차폐/높이/고립) %d/%d/%d/%d / 초대형 GS 형상 배제 %d / MRQ 카메라 독립"),
		SourceSnowCount, SourceSurfaceSnowCount, MadeLower, RecoveredBaseCount, SourceCandidateCount,
		SourceProxyAccepted, SourceOnlyAccepted, SourceIsolatedRejected,
		SourceOccludedRejected, SourceGradientRejected, SourceSurfaceGrid.Num(),
		WantedSpacing, SourceDetailCellCm, BaseThickness, VolumeStrength,
		MaskFull, MaskPartial, MaskFringe, MaskUnmatched,
		Source->SnowCoveragePackedMask.Num() * sizeof(uint32) / 1024.0,
		SnowSplats.Num() - SourceSnowCount, CliffWallRejected,
		SnowCliffShelfCount, SnowRailCount, RailCandidates, WindShadowCandidates,
		Source->SnowGaussianWindDirectionDegrees, WindStrength, SparseFloorSourceAccepted, RecoveryEligible,
		RecoveryRejectedSloped, RecoveryRejectedSky, RecoveryRejectedHeight, RecoveryRejectedIsolated,
		OversizedSourceShapeRejects);
	if (bReconstructedSnow)
	{
		Source->SnowGaussianBuildStatus=FString::Printf(
			TEXT("[2.8.67 표면 세부 빈틈 보완·프록시 4종 제한 기본해제] GS 셀 %d / MLS 수신면 %d / 구멍 보완 %d / 평활화 %d / 전체 후보 %d / Snow GS %d / 난간 %d / 절벽 턱 %d (+촘촘 보강 %d) / 확정 바닥 %d / 곡면 구조물 %d (출력 %d / 제외 %d) / 원본 바닥색 보완 %d / 프록시 거부 %d / 상부 차폐 %d / GS 마스크 완전 %d 부분 %d 경계 %d 미적용 %d / 절벽 탈락 %d / 선반 지지 부족 %d / 경사 탈락 %d / 원본 노멀 불일치 %d / 바닥 노멀 복구 %d / 대형 공분산 검사 %d (보정 %d) / LOD 바닥 계승 %d / 바닥 지지 미적용 %d / 바닥 실제 눈 보완 %d (지지 부족 %d) / 카메라 독립 MRQ"),
			SourceSurfaceGrid.Num(),ReconstructionRawNodes,ReconstructionBridges,
			ReconstructionSmoothNodes,ReconstructionGroupedNodes,SourceSnowCount,
			ReconstructionRailCount,ReconstructionShelfCount,ReconstructionShelfMicroCount,
			ReconstructionFloorCount,ReconstructionCrownCount,ReconstructionOutputCrown,
			ReconstructionCrownRejected,ReconstructionFloorTintCount,
			ReconstructionProxyRejects,ReconstructionSkyRejects,
			MaskFull,MaskPartial,MaskFringe,MaskUnmatched,
			ReconstructionCliffRejected,ReconstructionUnsupportedShelves,
			ReconstructionSlopeRejected,ReconstructionMaskNormalRejects,
			FloorCenterRecovered,FloorLargeOverlapChecked,FloorCovarianceRecovered,
			FloorLODInherited,FloorLayerRejects,VerifiedFloorGapRelief,FloorGapUnsupported);
	}
	if (bSurfaceResampling)
		Source->SnowMicroClusterBuildSummary = FString::Printf(
			TEXT("2.8.70 | 표면 후보 %d / 신규 눈 GS %d / 연결 생성 %d\n실제 빈틈 보완 %d / 이미 덮인 곳 %d / 지지 불가 %d\n미충족 중심 %d / 세부 미충족 %d\n최대 XY sigma %.1fcm / 총 Snow GS %d"),
			ResampleEligible,ResampleEmitted,ResampleBridges,ResampleRefillTotal,ResampleCovered,
			ResampleUnsupported,ResampleUncovered,ResampleUncoveredFineTotal,SnowSplitMaxWorldSigma,SourceSnowCount);
	else
		Source->SnowMicroClusterBuildSummary = FString::Printf(
			TEXT("2.8.67 기존 방식 A/B | 부모 %d / 생성 GS %d / 예약 누락 %d / 실패 %d / 최대 XY sigma %.1fcm / 전체 눈 GS %d"),
			SnowSplitParentsOmitted, SnowMicroAdded, SnowSplitSeedsWithoutPair,
			SnowSplitChildrenFailed, SnowSplitMaxWorldSigma, SourceSnowCount);
	UE_LOG(LogTemp, Display, TEXT("[Snow2867SurfaceSummary] %s"), *Source->SnowMicroClusterBuildSummary);
	// Visible in 02B/02E regardless of ROI CSV, with no parsed-log dependency.
	// Measured receiver count is NOT equivalent to physical ground coverage:
	// holes with no original GS / verified proxy remain unknown to this method.
	Source->SnowProxyNormalApplied = ProxySnowUsed;
	Source->SnowProxyNormalSoftFloor = ProxySnowSoft;
	Source->SnowProxyNormalMissing = ProxySnowNoMatch;
	Source->SnowProxyNormalInvalid = ProxySnowInvalid;
	Source->SnowProxyNormalSourceGateRejected = ProxySnowSourceRejected;
	UE_LOG(LogTemp, Display, TEXT("[Snow2867ProxyNormal] mode=%s blend=%.2f applied=%d soft_floor=%d no_receiver=%d rejected_mismatch=%d early_source_normal_reject=%d"),
		Source->bSnowProxyNormalPriority && bSurfaceResampling ? TEXT("proxy_primary") : TEXT("source_2866"),
		FMath::Clamp(double(Source->SnowProxyNormalBlend),0.0,1.0),ProxySnowUsed,ProxySnowSoft,
		ProxySnowNoMatch,ProxySnowInvalid,ProxySnowSourceRejected);
	Source->SnowResampleLastBuild = bSurfaceResampling ? TEXT("2.8.70 재샘플링 완료") : TEXT("2.8.70 기존 방식 A/B 완료");
	Source->SnowResampleReceiverCells = ReconstructedSnowField.Num();
	Source->SnowResampleGenerated = ResampleEmitted;
	Source->SnowResampleRefillPlaced = ResampleRefillTotal;
	Source->SnowResampleUnmetCenters = ResampleUncovered;
	Source->SnowResampleUnmetFine = ResampleUncoveredFineTotal;
	Source->SnowResampleOpticalUnmetCenters = ResampleOpticalUnmetCenters;
	Source->SnowResampleGeometricUnmetCenters = ResampleGeometricUnmetCenters;
	Source->SnowResampleQuadSites = ResampleQuadSitesTotal;
	Source->SnowResampleUnmetQuadSites = ResampleUnmetQuadSitesTotal;
	Source->SnowResampleUnusedSurfaceSlots = ResampleUnusedSlotsTotal;
	Source->SnowResampleRejectedSupport = ResampleUnsupported;
	Source->SnowCohesionBridgeCandidates = ResampleCohesionBridgeCandidates;
	Source->SnowCohesionBridgePlaced = ResampleCohesionBridgesPlaced;
	Source->SnowFineCandidates2869 = SnowFineCandidates;
	Source->SnowFinePlaced2869 = SnowFinePlaced;
	Source->SnowFineUnmet2869 = SnowFineUnmet;
	Source->SnowFinePasses2869 = SnowFinePasses;
	Source->SnowOpticalFinishChanged2870 = SnowFinishChanged;
	Source->SnowOpticalFinishCandidateSites2870 = SnowFinishSites;
	Source->SnowOpticalFinishUnmetBefore2870 = SnowFinishUnmetBefore;
	Source->SnowOpticalFinishUnmetAfter2870 = SnowFinishUnmetAfter;
	Source->SnowOpticalUnmetFloor2870 = SnowFinishFloor;
	Source->SnowOpticalUnmetRoof2870 = SnowFinishRoof;
	Source->SnowOpticalUnmetStone2870 = SnowFinishStone;
	Source->SnowResampleTotalGenerated = SourceSnowCount;
	Source->SnowResampleRefillCandidateSites = ResampleRefillCandidateSitesTotal;
	Source->SnowResampleMeasuredRounds = ResampleMeasuredRounds;
	// Count only an ACTUALLY CONVERGED sampled field as "required". Do not
	// invent a number from uncovered-center counts: neighbors can cover many sites.
	const bool bFineGoalUnmet = bSurfaceResampling && Source->bSnowResampleGeometricCoverage &&
		Source->bSnowFineLayer2869 && Source->bSnowCohesionBridges && SnowFineUnmet>0;
	const bool bResampleConverged = bSurfaceResampling &&
		ResampleUncovered==0 && ResampleUncoveredFineTotal==0 && ResampleUnmetQuadSitesTotal==0 && !bFineGoalUnmet;
	Source->SnowResampleMeasuredRequiredCount = bResampleConverged ? SourceSnowCount : -1;
	Source->SnowResampleStopReason = !bSurfaceResampling ?
		TEXT("이전 A/B 경로: 필요 개수 미측정") : bResampleConverged ?
		TEXT("검증된 표본 목표 충족: 실생성 총 GS(추가 눈만)의 관측 필요 수") :
		bResampleCapacityReached ? TEXT("배열 정수 인덱스 한계에 도달: 필요 수 미확정") :
		!bSnowMeasureUncapped && ResampleUnusedSlotsTotal==0 ? TEXT("설정 개수/88% 표면 상한 도달: 필요 수 미확정") :
		bFineGoalUnmet ? TEXT("2.8.69 미세층 광학 목표 미충족: 02F 수신면 진단 확인 (필요 수 미확정)") :
		bResampleStalled ? TEXT("초기 배치 후보 정체, 이후 4점 내부/미세층은 별도 검사됨: 필요 수 미확정") :
		bSnowMeasureUncapped ? TEXT("지지 후보 검사 종료, 목표 미달 지점 존재: 필요 수 미확정") :
		TEXT("설정된 반복 횟수 종료, 목표 미달: 필요 수 미확정");
	UE_LOG(LogTemp, Display, TEXT("[Snow2867Count] uncapped=%d total_snow_no_LOD=%d surface_generated=%d gap_refill=%d candidate_sites=%d sweeps=%d unmet_center=%d unmet_center_or_link=%d unmet_quad=%d/%d converged=%d required=%d stop=%s"),
		bSnowMeasureUncapped?1:0,SourceSnowCount,ResampleEmitted,ResampleRefillTotal,
		ResampleRefillCandidateSitesTotal,ResampleMeasuredRounds,ResampleUncovered,
		ResampleUncoveredFineTotal,ResampleUnmetQuadSitesTotal,ResampleQuadSitesTotal,
		bResampleConverged?1:0,Source->SnowResampleMeasuredRequiredCount,*Source->SnowResampleStopReason);
	UE_LOG(LogTemp,Display,TEXT("[Snow2867Coverage] mode=%s footprint=%.2f_sigma geometric_unmet_centers=%d optical_unmet_centers=%d actual_unmet_centers=%d supported_quad_samples=%d supported_quad_unmet=%d"),
		bSurfaceResampling && Source->bSnowResampleGeometricCoverage ? TEXT("geometry") : TEXT("legacy_optical"),
		FMath::Clamp(double(Source->SnowResampleGeometricFootprintSigma),0.90,1.80),
		ResampleGeometricUnmetCenters,ResampleOpticalUnmetCenters,ResampleUncovered,
		ResampleQuadSitesTotal,ResampleUnmetQuadSitesTotal);
	Source->SnowResampleMaskStats = FString::Printf(TEXT("원본 GS 눈 코드: 완전 %d / 부분 %d / 경계 %d / 미적용 %d"),
		MaskFull,MaskPartial,MaskFringe,MaskUnmatched);
	Source->SnowResampleDiagnosticSummary = bSurfaceResampling ? FString::Printf(
		TEXT("2.8.69 / 공간 커버리지 %s / 생성 상한 %s\n")
		TEXT("총 Snow GS %d(LOD 제외), 신규 %d, 재보완 %d\n")
		TEXT("수신면 %d, 검사 후보 %d, 재검사 %d\n")
		TEXT("공간 미충족 중심 %d / 광학 미충족 %d\n")
		TEXT("배치 미충족 중심 %d, 연결 %d, 4점 내부 %d/%d\n")
		TEXT("월드 표본 검사이며 화면 픽셀 면적이 아닙니다.\n%s"),
		Source->bSnowResampleGeometricCoverage ? TEXT("ON") : TEXT("OFF(A/B)"),bSnowMeasureUncapped ? TEXT("해제") : TEXT("유지"),
		SourceSnowCount,ResampleEmitted,ResampleRefillTotal,ReconstructedSnowField.Num(),
		ResampleRefillCandidateSitesTotal,ResampleMeasuredRounds,ResampleGeometricUnmetCenters,
		ResampleOpticalUnmetCenters,ResampleUncovered,ResampleUncoveredFineTotal,
		ResampleUnmetQuadSitesTotal,ResampleQuadSitesTotal,*Source->SnowResampleStopReason) :
		TEXT("2.8.67 이전 분할 A/B 모드: 표면 커버리지 검사 비활성. 02E 표면 재샘플링을 켜고 다시 생성하세요.");
	// Production proxy receiver policy is the verified 2.8.72 default.
	Source->SnowFloorBuildSummary = FString::Printf(
		TEXT("2.8.74 | 원본 우선 바닥 %s | 소스만 인정 %d / 완화된 실프록시 %d / 약한 차폐 복구 %d / 바닥 실제 눈 보완 %d | 크기 %.2f / 두께 %.2f / 프록시 높이 x%.2f / 최소 Up %.2f / 차폐 주변점 %d"),
		Source->bSnowFloorPreferSource ? TEXT("ON") : TEXT("OFF"),
		GroundSourceOnlyCount, GroundSoftReceiverCount, GroundFloorWeakBlockerRecovered,
		VerifiedFloorGapRelief,
		FMath::Clamp((double)Source->SnowFloorRadiusScale,0.60,1.0),
		FMath::Clamp((double)Source->SnowFloorDepthScale,0.55,1.0),
		FMath::Clamp((double)Source->SnowFloorProxyHeightMultiplier,1.0,2.5),
		FMath::Clamp((double)Source->SnowFloorProxyMinUp,0.50,0.84),
		FMath::Clamp(Source->SnowFloorBlockerNeighborHits,1,4));
	UE_LOG(LogTemp, Display, TEXT("[Snow2858Floor] %s"), *Source->SnowFloorBuildSummary);
	SyncWeatherEffects();
#if WITH_EDITOR
	MarkPackageDirty();
#endif
	UE_LOG(LogTemp, Log, TEXT("[SnowGaussian] %s | XY candidates=%d / Supported=%d / Unsupported=%d / Shelter=%d / Duplicate=%d / SupportCell=%.1fcm / MicroCell=%.1fcm"),
		*Source->SnowGaussianBuildStatus, XYInsideSamples,
		XYInsideSamples - UnsupportedSamples, UnsupportedSamples, ShelteredSamples, DuplicateSamples, SupportCell, MicroCell);
}
