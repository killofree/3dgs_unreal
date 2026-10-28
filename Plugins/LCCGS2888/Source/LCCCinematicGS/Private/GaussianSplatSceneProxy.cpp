// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatSceneProxy.h"
#include "GaussianSplatComponent.h"
#include "GaussianSplatActor.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatRenderData.h"
#include "GaussianSplatRenderer.h"
#include "GaussianSplatViewExtension.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RHICommandList.h"
#include "RenderGraphBuilder.h"
#include "SceneView.h"
#include "SceneManagement.h"
#include "DynamicMeshBuilder.h"
#include "Materials/Material.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "StaticMeshResources.h"
#include "UObject/UObjectIterator.h"
#include "HAL/IConsoleManager.h"

extern TAutoConsoleVariable<int32> CVarGSPanoramaShadowTrace;

// v2801: the production stable-shadow path is CPU deterministic BVH only. Keeping
// this authority in one place lets Panorama/MRQ proxies avoid allocating the two
// legacy 2048x2048 light-depth grids (~32 MiB per SceneProxy) that are never read
// by the production shader path. One-entry dummy SRVs are still created so shader
// parameter binding remains valid.
static constexpr bool GDeterministicBVHCastOnly = true;

//////////////////////////////////////////////////////////////////////////
// FGaussianSplatGPUResources

FGaussianSplatGPUResources::FGaussianSplatGPUResources()
{
}

FGaussianSplatGPUResources::~FGaussianSplatGPUResources()
{
}

void FGaussianSplatGPUResources::Initialize(UGaussianSplatAsset* Asset)
{
	if (!Asset || !Asset->IsValid())
	{
		return;
	}

	// Get or create shared render data (CPU-side cached data, loaded once per asset)
	SharedData = Asset->GetOrCreateRenderData();
	if (!SharedData.IsValid() || !SharedData->IsInitialized())
	{
		return;
	}

	// Copy metadata from shared render data
	SplatCount = SharedData->SplatCount;
	PositionFormat = SharedData->PositionFormat;
	SHBands = SharedData->SHBands;
	bEnableNanite = SharedData->bEnableNanite;
	bHasClusterData = SharedData->bHasClusterData;
	ClusterCount = SharedData->ClusterCount;
	LeafClusterCount = SharedData->LeafClusterCount;
	LODSplatCount = SharedData->LODSplatCount;
	bHasLODSplats = SharedData->bHasLODSplats;
	SpatialLightingGridMinCellSize = FVector4f(
		SharedData->SpatialLightingGridMin.X,
		SharedData->SpatialLightingGridMin.Y,
		SharedData->SpatialLightingGridMin.Z,
		SharedData->SpatialLightingCellSize);
	SpatialLightingGridDimensionsStatus = FVector4f(
		static_cast<float>(SharedData->SpatialLightingGridDimensions.X),
		static_cast<float>(SharedData->SpatialLightingGridDimensions.Y),
		static_cast<float>(SharedData->SpatialLightingGridDimensions.Z),
		static_cast<float>(SharedData->SpatialLightingCacheStatus));
	SpatialLightingCellCount = static_cast<uint32>(FMath::Max(SharedData->SpatialLightingCellCount, 0));
	SpatialLightingOccupiedCellCount = SharedData->SpatialLightingOccupiedCellCount;
	SpatialLightingCoverage = SharedData->SpatialLightingCoverage;
	SpatialLightingAverageConfidence = SharedData->SpatialLightingAverageConfidence;

	// Initialize render resource
	if (!bInitialized)
	{
		InitResource(FRHICommandListImmediate::Get());
		bInitialized = true;
	}
}

void FGaussianSplatGPUResources::InitRHI(FRHICommandListBase& RHICmdList)
{
	if (SplatCount <= 0 || !SharedData.IsValid())
	{
		return;
	}

	// Create shared GPU buffers (only runs once per asset, thread-safe)
	SharedData->CreateGPUBuffers(RHICmdList);

	// Copy shared GPU buffer refs (just ref-count increments, no GPU allocation)
	PackedSplatBuffer = SharedData->PackedSplatBuffer;
	PackedSplatBufferSRV = SharedData->PackedSplatBufferSRV;
	Float32PositionBuffer = SharedData->Float32PositionBuffer;
	Float32PositionBufferSRV = SharedData->Float32PositionBufferSRV;
	SHBuffer = SharedData->SHBuffer;
	SHBufferSRV = SharedData->SHBufferSRV;
	ChunkBuffer = SharedData->ChunkBuffer;
	ChunkBufferSRV = SharedData->ChunkBufferSRV;
	IndexBuffer = SharedData->IndexBuffer;
	ClusterBuffer = SharedData->ClusterBuffer;
	ClusterBufferSRV = SharedData->ClusterBufferSRV;
	SplatClusterIndexBuffer = SharedData->SplatClusterIndexBuffer;
	SplatClusterIndexBufferSRV = SharedData->SplatClusterIndexBufferSRV;
	ClusterLightingBuffer = SharedData->ClusterLightingBuffer;
	ClusterLightingBufferSRV = SharedData->ClusterLightingBufferSRV;

	// Component-local proxy normal grid. Always create a one-entry dummy so the
	// shader binding remains valid when the feature is disabled or unavailable.
	{
		const uint32 EntryCount = FMath::Max(PendingProxyNormalGridData.Num(), 1);
		const uint32 BufferSize = EntryCount * sizeof(FFloat16Color);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianProxyNormalGridBuffer"),
			BufferSize,
			sizeof(FFloat16Color),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		ProxyNormalGridBuffer = RHICmdList.CreateBuffer(Desc);
		void* Data = RHICmdList.LockBuffer(ProxyNormalGridBuffer, 0, BufferSize, RLM_WriteOnly);
		if (PendingProxyNormalGridData.Num() > 0)
		{
			FMemory::Memcpy(Data, PendingProxyNormalGridData.GetData(), PendingProxyNormalGridData.Num() * sizeof(FFloat16Color));
		}
		else
		{
			const FFloat16Color Dummy(FLinearColor(0.5f, 0.5f, 1.0f, 0.0f));
			FMemory::Memcpy(Data, &Dummy, sizeof(Dummy));
		}
		RHICmdList.UnlockBuffer(ProxyNormalGridBuffer);
		ProxyNormalGridBufferSRV = RHICmdList.CreateShaderResourceView(
			ProxyNormalGridBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(FFloat16Color)));
		PendingProxyNormalGridData.Empty();
	}

	// v2842 snow-only source correspondence. A 1-entry zero dummy guarantees
	// P08 source-only, world-space broad-ground membership. Allocate once on the
	// render thread. A dummy SRV keeps all weather / layer permutations bind-safe.
	{
		const uint32 Entries = FMath::Max(1, PendingPuddleDampenPackedData.Num());
		const uint32 Bytes = Entries * sizeof(uint32);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GSPuddleGroundDampenMask"), Bytes, sizeof(uint32),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		PuddleDampenPackedBuffer = RHICmdList.CreateBuffer(Desc);
		void* Dest = RHICmdList.LockBuffer(PuddleDampenPackedBuffer, 0, Bytes, RLM_WriteOnly);
		if (PuddleDampenPackedCount > 0u)
			FMemory::Memcpy(Dest, PendingPuddleDampenPackedData.GetData(), PuddleDampenPackedCount * sizeof(uint32));
		else FMemory::Memzero(Dest, Bytes);
		RHICmdList.UnlockBuffer(PuddleDampenPackedBuffer);
		PuddleDampenPackedBufferSRV = RHICmdList.CreateShaderResourceView(
			PuddleDampenPackedBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
		PendingPuddleDampenPackedData.Empty();
	}
	// valid bindings for all rain/clear and standalone snow-layer permutations.
	{
		const uint32 Entries = FMath::Max(1, PendingSnowCoveragePackedData.Num());
		const uint32 Bytes = Entries * sizeof(uint32);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianSnowSourceMask"), Bytes, sizeof(uint32),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		SnowCoveragePackedBuffer = RHICmdList.CreateBuffer(Desc);
		void* Dest = RHICmdList.LockBuffer(SnowCoveragePackedBuffer, 0, Bytes, RLM_WriteOnly);
		if (SnowCoveragePackedCount > 0)
			FMemory::Memcpy(Dest, PendingSnowCoveragePackedData.GetData(), SnowCoveragePackedCount * sizeof(uint32));
		else
			FMemory::Memzero(Dest, Bytes);
		RHICmdList.UnlockBuffer(SnowCoveragePackedBuffer);
		SnowCoveragePackedBufferSRV = RHICmdList.CreateShaderResourceView(
			SnowCoveragePackedBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
		PendingSnowCoveragePackedData.Empty();
	}
	// 2.8.73: one byte of baked world accumulation pattern for each original
	// source splat. Allocate once per SceneProxy; all runtime views use SRV reads.
	// A dummy keeps legacy assets/standalone Snow components safely bindable.
	{
		const uint32 Entries = FMath::Max(1, PendingSnowBakedFieldData2873.Num());
		const uint32 Bytes = Entries * sizeof(uint32);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianSnowBakedWorldField2873"), Bytes, sizeof(uint32),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		SnowBakedFieldBuffer2873 = RHICmdList.CreateBuffer(Desc);
		void* Dest = RHICmdList.LockBuffer(SnowBakedFieldBuffer2873, 0, Bytes, RLM_WriteOnly);
		if (SnowBakedFieldCount2873 > 0u)
			FMemory::Memcpy(Dest, PendingSnowBakedFieldData2873.GetData(),
				SnowBakedFieldCount2873 * sizeof(uint32));
		else
			FMemory::Memzero(Dest, Bytes);
		RHICmdList.UnlockBuffer(SnowBakedFieldBuffer2873);
		SnowBakedFieldBufferSRV2873 = RHICmdList.CreateShaderResourceView(
			SnowBakedFieldBuffer2873, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
		PendingSnowBakedFieldData2873.Empty();
	}

	// Phase-1 stable splat -> proxy triangle binding. Always allocate a one-entry
	// invalid dummy so shader binding stays valid before the explicit proof build.
	{
		const int32 PendingBindingCount = PendingStableSplatBindingData.IsValid() ? PendingStableSplatBindingData->Num() : 0;
		const uint32 EntryCount = FMath::Max(PendingBindingCount, 1);
		StableSplatBindingCount = static_cast<uint32>(PendingBindingCount);
		StableSplatBindingCapacity = EntryCount;
		const uint32 BufferSize = EntryCount * sizeof(FStableProxySplatBindingGPU);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianStableSplatBindingBuffer"),
			BufferSize,
			sizeof(FStableProxySplatBindingGPU),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		StableSplatBindingBuffer = RHICmdList.CreateBuffer(Desc);
		void* Data = RHICmdList.LockBuffer(StableSplatBindingBuffer, 0, BufferSize, RLM_WriteOnly);
		if (PendingBindingCount > 0)
		{
			FMemory::Memcpy(Data, PendingStableSplatBindingData->GetData(), PendingBindingCount * sizeof(FStableProxySplatBindingGPU));
		}
		else
		{
			const FStableProxySplatBindingGPU Dummy;
			FMemory::Memcpy(Data, &Dummy, sizeof(Dummy));
		}
		RHICmdList.UnlockBuffer(StableSplatBindingBuffer);
		StableSplatBindingBufferSRV = RHICmdList.CreateShaderResourceView(
			StableSplatBindingBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(FStableProxySplatBindingGPU)));
		PendingStableSplatBindingData.Reset();
	}

	// Phase-2 fixed proxy surface. Always create one-entry dummies; the proof buffer
	// is UAV-written exactly when an explicit stable-field revision is requested.
	UpdateStableProxySurfaceData_RenderThread(RHICmdList, PendingStableProxyVertexData, PendingStableProxyTriangleData, PendingStableProxyTriangleVertexNormalData);
	PendingStableProxyVertexData.Empty();
	PendingStableProxyTriangleData.Empty();
	PendingStableProxyTriangleVertexNormalData.Empty();

	// v2802: the Panorama bootstrap is performed while the proof buffers are
	// created in UpdateStableProxySurfaceData_RenderThread().  Do not lock the same
	// D3D12 UAV/SRV buffers a second time inside InitRHI; on some drivers that
	// back-to-back static-buffer relock can result in a GPU device removal during
	// MRQ pane creation.  The pending CPU payload is consumed after initialization.
	PendingStableProxyRayProofData.Empty();

	// TotalSplatCount is needed by per-instance buffer sizing
	TotalSplatCount = SplatCount;

	// Create dummy white texture (tiny, per-proxy is fine)
	CreateDummyWhiteTexture(RHICmdList);

	// Create per-instance buffers (cluster visibility, compaction, sort args, etc.)
	CreatePerInstanceBuffers(RHICmdList);

	int32 PerInstanceBufferCount = 0;
	if (bSupportsCompaction) PerInstanceBufferCount += 15; // cluster/compaction/sort buffers
	if (bSupportsIndirectDraw) PerInstanceBufferCount += 1;
	UE_LOG(LogTemp, Verbose, TEXT("GaussianSplatGPUResources: Created %d per-instance buffers (shared data: %s)"),
		PerInstanceBufferCount, *SharedData->GetAssetName());
}

void FGaussianSplatGPUResources::UpdateStableSplatBindingData_RenderThread(
	FRHICommandListBase& RHICmdList,
	const TArray<FStableProxySplatBindingGPU>& InData)
{
	StableSplatBindingCount = static_cast<uint32>(InData.Num());
	const uint32 EntryCount = FMath::Max(InData.Num(), 1);
	if (!StableSplatBindingBuffer.IsValid() || EntryCount > StableSplatBindingCapacity)
	{
		StableSplatBindingBufferSRV.SafeRelease();
		StableSplatBindingBuffer.SafeRelease();
		StableSplatBindingCapacity = EntryCount;
		const uint32 CapacityBytes = StableSplatBindingCapacity * sizeof(FStableProxySplatBindingGPU);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianStableSplatBindingBuffer"), CapacityBytes,
			sizeof(FStableProxySplatBindingGPU),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		StableSplatBindingBuffer = RHICmdList.CreateBuffer(Desc);
		StableSplatBindingBufferSRV = RHICmdList.CreateShaderResourceView(
			StableSplatBindingBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(FStableProxySplatBindingGPU)));
	}
	const uint32 UploadBytes = EntryCount * sizeof(FStableProxySplatBindingGPU);
	void* Data = RHICmdList.LockBuffer(StableSplatBindingBuffer, 0, UploadBytes, RLM_WriteOnly);
	if (InData.Num() > 0)
	{
		FMemory::Memcpy(Data, InData.GetData(), InData.Num() * sizeof(FStableProxySplatBindingGPU));
	}
	else
	{
		const FStableProxySplatBindingGPU Dummy;
		FMemory::Memcpy(Data, &Dummy, sizeof(Dummy));
	}
	RHICmdList.UnlockBuffer(StableSplatBindingBuffer);
}

void FGaussianSplatGPUResources::UpdateStableProxySurfaceData_RenderThread(
	FRHICommandListBase& RHICmdList,
	const TArray<FStableProxyVertexGPU>& InVertices,
	const TArray<FStableProxyTriangleGPU>& InTriangles,
	const TArray<FVector4f>& InTriangleVertexNormals)
{
	StableProxyCastGridValid = 0u;
	StableProxyCastGridActiveWidth = 0u;
	StableProxyCastGridActiveHeight = 0u;
	StableProxyVertexCount = static_cast<uint32>(InVertices.Num());
	StableProxyTriangleCount = static_cast<uint32>(InTriangles.Num());
	StableProxyFieldValid = 0u;
	StableProxyFieldBuildCount = 0u;
	StableProxyFieldPairValid = 0u;
	StableProxyLiveTargetValid = 0u;
	StableProxyFieldFrontCaptureRevision = 0u;
	StableProxyFieldPreviousCaptureRevision = 0u;
	StableProxyLiveTargetCaptureRevision = 0u;

	const FStableProxyVertexGPU DummyVertex;
	const uint32 VertexEntries = FMath::Max(InVertices.Num(), 1);
	const uint32 VertexBytes = VertexEntries * sizeof(FStableProxyVertexGPU);
	const bool bVertexCapacityGrew = !StableProxyVertexBuffer.IsValid() || VertexEntries > StableProxyVertexCapacity;
	if (bVertexCapacityGrew)
	{
		StableProxyVertexOwnerTargetBufferUAV.SafeRelease();
		StableProxyVertexOwnerTargetBufferSRV.SafeRelease();
		StableProxyVertexOwnerTargetBuffer.SafeRelease();
		StableProxyVertexOwnerBufferUAV.SafeRelease();
		StableProxyVertexOwnerBufferSRV.SafeRelease();
		StableProxyVertexOwnerBuffer.SafeRelease();
		StableProxyVertexProofTargetBufferUAV.SafeRelease();
		StableProxyVertexProofTargetBufferSRV.SafeRelease();
		StableProxyVertexProofTargetBuffer.SafeRelease();
		StableProxyVertexProofBufferUAV.SafeRelease();
		StableProxyVertexProofBufferSRV.SafeRelease();
		StableProxyVertexProofBuffer.SafeRelease();
		StableProxyVertexBufferSRV.SafeRelease();
		StableProxyVertexBuffer.SafeRelease();
		StableProxyVertexCapacity = VertexEntries;
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianStableProxyVertexBuffer"), StableProxyVertexCapacity * sizeof(FStableProxyVertexGPU), sizeof(FStableProxyVertexGPU),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer).SetInitialState(ERHIAccess::SRVMask);
		StableProxyVertexBuffer = RHICmdList.CreateBuffer(Desc);
		StableProxyVertexBufferSRV = RHICmdList.CreateShaderResourceView(StableProxyVertexBuffer,
			FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(FStableProxyVertexGPU)));
	}
	{
		void* Data = RHICmdList.LockBuffer(StableProxyVertexBuffer, 0, VertexBytes, RLM_WriteOnly);
		if (InVertices.Num() > 0) FMemory::Memcpy(Data, InVertices.GetData(), InVertices.Num() * sizeof(FStableProxyVertexGPU));
		else FMemory::Memcpy(Data, &DummyVertex, sizeof(DummyVertex));
		RHICmdList.UnlockBuffer(StableProxyVertexBuffer);
	}

	const FStableProxyTriangleGPU DummyTriangle;
	const uint32 TriangleEntries = FMath::Max(InTriangles.Num(), 1);
	const uint32 TriangleBytes = TriangleEntries * sizeof(FStableProxyTriangleGPU);
	if (!StableProxyTriangleBuffer.IsValid() || TriangleEntries > StableProxyTriangleCapacity)
	{
		StableProxyTriangleBufferSRV.SafeRelease();
		StableProxyTriangleBuffer.SafeRelease();
		StableProxyTriangleCapacity = TriangleEntries;
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianStableProxyTriangleBuffer"), StableProxyTriangleCapacity * sizeof(FStableProxyTriangleGPU), sizeof(FStableProxyTriangleGPU),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer).SetInitialState(ERHIAccess::SRVMask);
		StableProxyTriangleBuffer = RHICmdList.CreateBuffer(Desc);
		StableProxyTriangleBufferSRV = RHICmdList.CreateShaderResourceView(StableProxyTriangleBuffer,
			FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(FStableProxyTriangleGPU)));
	}
	{
		void* Data = RHICmdList.LockBuffer(StableProxyTriangleBuffer, 0, TriangleBytes, RLM_WriteOnly);
		if (InTriangles.Num() > 0) FMemory::Memcpy(Data, InTriangles.GetData(), InTriangles.Num() * sizeof(FStableProxyTriangleGPU));
		else FMemory::Memcpy(Data, &DummyTriangle, sizeof(DummyTriangle));
		RHICmdList.UnlockBuffer(StableProxyTriangleBuffer);
	}

	// v2804 authored source-corner normals. Keep this independent from welded
	// position topology so hard edges do not get averaged accidentally.
	StableProxyTriangleVertexNormalCount = static_cast<uint32>(InTriangleVertexNormals.Num());
	const FVector4f DummyNormal(0.0f, 0.0f, 1.0f, 0.0f);
	const uint32 NormalEntries = FMath::Max(InTriangleVertexNormals.Num(), 1);
	const uint32 NormalBytes = NormalEntries * sizeof(FVector4f);
	if (!StableProxyTriangleVertexNormalBuffer.IsValid() || NormalEntries > StableProxyTriangleVertexNormalCapacity)
	{
		StableProxyTriangleVertexNormalBufferSRV.SafeRelease();
		StableProxyTriangleVertexNormalBuffer.SafeRelease();
		StableProxyTriangleVertexNormalCapacity = NormalEntries;
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianStableProxyTriangleVertexNormalBuffer"),
			StableProxyTriangleVertexNormalCapacity * sizeof(FVector4f), sizeof(FVector4f),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer).SetInitialState(ERHIAccess::SRVMask);
		StableProxyTriangleVertexNormalBuffer = RHICmdList.CreateBuffer(Desc);
		StableProxyTriangleVertexNormalBufferSRV = RHICmdList.CreateShaderResourceView(
			StableProxyTriangleVertexNormalBuffer,
			FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(FVector4f)));
	}
	{
		void* Data = RHICmdList.LockBuffer(StableProxyTriangleVertexNormalBuffer, 0, NormalBytes, RLM_WriteOnly);
		if (InTriangleVertexNormals.Num() > 0)
			FMemory::Memcpy(Data, InTriangleVertexNormals.GetData(), InTriangleVertexNormals.Num() * sizeof(FVector4f));
		else
			FMemory::Memcpy(Data, &DummyNormal, sizeof(DummyNormal));
		RHICmdList.UnlockBuffer(StableProxyTriangleVertexNormalBuffer);
	}

	const uint32 ProofBytes = VertexEntries * sizeof(FVector4f);
	const bool bHasBootstrapProof =
		PendingStableProxyRayProofData.Num() > 0 &&
		PendingStableProxyRayProofData.Num() == static_cast<int32>(StableProxyVertexCount);
	auto CreateStableProofBuffer = [&](
		const TCHAR* DebugName,
		FBufferRHIRef& OutBuffer,
		FShaderResourceViewRHIRef& OutSRV,
		FUnorderedAccessViewRHIRef& OutUAV)
	{
		if (!OutBuffer.IsValid())
		{
			FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
				DebugName, StableProxyVertexCapacity * sizeof(FVector4f), sizeof(FVector4f),
				BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer).SetInitialState(ERHIAccess::SRVMask);
			OutBuffer = RHICmdList.CreateBuffer(Desc);
			OutSRV = RHICmdList.CreateShaderResourceView(OutBuffer,
				FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(FVector4f)));
			OutUAV = RHICmdList.CreateUnorderedAccessView(OutBuffer,
				FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(FVector4f)));
		}
		void* Data = RHICmdList.LockBuffer(OutBuffer, 0, ProofBytes, RLM_WriteOnly);
		if (bHasBootstrapProof)
		{
			// v2802: initialize Front and Previous directly from the completed CPU BVH
			// field during the buffer's normal creation upload.  This avoids a second
			// immediate LockBuffer/UnlockBuffer pair on a static UAV/SRV buffer.
			FMemory::Memcpy(Data, PendingStableProxyRayProofData.GetData(),
				StableProxyVertexCount * sizeof(FVector4f));
		}
		else
		{
			FVector4f* Proof = static_cast<FVector4f*>(Data);
			for (uint32 I = 0; I < VertexEntries; ++I)
			{
				Proof[I] = FVector4f(1.0f, 0.0f, 0.0f, -1.0f);
			}
		}
		RHICmdList.UnlockBuffer(OutBuffer);
	};
	CreateStableProofBuffer(
		TEXT("GaussianStableProxyVertexProofFront"),
		StableProxyVertexProofBuffer, StableProxyVertexProofBufferSRV, StableProxyVertexProofBufferUAV);
	CreateStableProofBuffer(
		TEXT("GaussianStableProxyVertexProofTarget"),
		StableProxyVertexProofTargetBuffer, StableProxyVertexProofTargetBufferSRV, StableProxyVertexProofTargetBufferUAV);

	if (bHasBootstrapProof)
	{
		StableProxyFieldValid = 1u;
		StableProxyFieldPairValid = 1u;
		StableProxyLiveTargetValid = 1u;
		StableProxyCastGridValid = 0u;
	}

	const uint32 OwnerBytes = VertexEntries * sizeof(uint32);
	auto CreateOwnerBuffer = [&](const TCHAR* DebugName, FBufferRHIRef& OutBuffer,
		FShaderResourceViewRHIRef& OutSRV, FUnorderedAccessViewRHIRef& OutUAV)
	{
		if (!OutBuffer.IsValid())
		{
			FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
				DebugName, StableProxyVertexCapacity * sizeof(uint32), sizeof(uint32),
				BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer).SetInitialState(ERHIAccess::SRVMask);
			OutBuffer = RHICmdList.CreateBuffer(Desc);
			OutSRV = RHICmdList.CreateShaderResourceView(OutBuffer,
				FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
			OutUAV = RHICmdList.CreateUnorderedAccessView(OutBuffer,
				FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
		}
		void* Data = RHICmdList.LockBuffer(OutBuffer, 0, OwnerBytes, RLM_WriteOnly);
		FMemory::Memset(Data, 0xFF, OwnerBytes);
		RHICmdList.UnlockBuffer(OutBuffer);
	};
	CreateOwnerBuffer(TEXT("GaussianStableProxyVertexOwnerFront"), StableProxyVertexOwnerBuffer,
		StableProxyVertexOwnerBufferSRV, StableProxyVertexOwnerBufferUAV);
	CreateOwnerBuffer(TEXT("GaussianStableProxyVertexOwnerTarget"), StableProxyVertexOwnerTargetBuffer,
		StableProxyVertexOwnerTargetBufferSRV, StableProxyVertexOwnerTargetBufferUAV);

	// v2803 Panorama/D3D12 safety: deterministic BVH is the only production cast
	// authority, so the legacy LightDepth resources are deliberately non-writable.
	// Keep a 1-entry SRV-only dummy for shader binding, but DO NOT create UAVs. Any
	// stale legacy dispatch therefore fails its existing IsValid() guard before a GPU
	// command can be recorded. This preserves the VRAM saving without exposing a 1x1
	// UAV to old 2048x2048 code paths.
	StableProxyCastGridWidth = GDeterministicBVHCastOnly ? 1u : 2048u;
	StableProxyCastGridHeight = GDeterministicBVHCastOnly ? 1u : 2048u;
	StableProxyCastGridActiveWidth = 0u;
	StableProxyCastGridActiveHeight = 0u;
	StableProxyCastGridValid = 0u;
	const uint32 CastGridEntries = StableProxyCastGridWidth * StableProxyCastGridHeight;
	const uint32 CastGridBytes = CastGridEntries * sizeof(float);
	if (!StableProxyCastGridBuffer.IsValid())
	{
		const EBufferUsageFlags Usage = GDeterministicBVHCastOnly
			? (BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
			: (BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianStableProxyCastGridBuffer"), CastGridBytes, sizeof(float), Usage)
			.SetInitialState(ERHIAccess::SRVMask);
		StableProxyCastGridBuffer = RHICmdList.CreateBuffer(Desc);
		StableProxyCastGridBufferSRV = RHICmdList.CreateShaderResourceView(StableProxyCastGridBuffer,
			FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(float)));
		if (!GDeterministicBVHCastOnly)
		{
			StableProxyCastGridBufferUAV = RHICmdList.CreateUnorderedAccessView(StableProxyCastGridBuffer,
				FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(float)));
		}
	}
	if (!StableProxyReceiverTriangleIdBuffer.IsValid())
	{
		const EBufferUsageFlags Usage = GDeterministicBVHCastOnly
			? (BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
			: (BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianStableProxyReceiverTriangleId"), CastGridEntries * sizeof(uint32), sizeof(uint32), Usage)
			.SetInitialState(ERHIAccess::SRVMask);
		StableProxyReceiverTriangleIdBuffer = RHICmdList.CreateBuffer(Desc);
		StableProxyReceiverTriangleIdBufferSRV = RHICmdList.CreateShaderResourceView(StableProxyReceiverTriangleIdBuffer,
			FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
		if (!GDeterministicBVHCastOnly)
		{
			StableProxyReceiverTriangleIdBufferUAV = RHICmdList.CreateUnorderedAccessView(StableProxyReceiverTriangleIdBuffer,
				FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
		}
	}

}

void FGaussianSplatGPUResources::UpdateStableProxyRayProofData_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	const TArray<FVector4f>& InProofs)
{
	if (!StableProxyVertexProofBuffer.IsValid() ||
		InProofs.Num() != static_cast<int32>(StableProxyVertexCount))
	{
		return;
	}
	const uint32 Bytes = StableProxyVertexCount * sizeof(FVector4f);

	// Preserve the last fully uploaded deterministic BVH result before replacing
	// Front.  Previously both buffers received InProofs, so the shader's A/B blend
	// always interpolated two identical fields and could not soften a moving edge.
	if (StableProxyFieldValid != 0u && StableProxyVertexProofTargetBuffer.IsValid())
	{
		RHICmdList.Transition(FRHITransitionInfo(
			StableProxyVertexProofBuffer, ERHIAccess::SRVMask, ERHIAccess::CopySrc));
		RHICmdList.Transition(FRHITransitionInfo(
			StableProxyVertexProofTargetBuffer, ERHIAccess::SRVMask, ERHIAccess::CopyDest));
		RHICmdList.CopyBufferRegion(
			StableProxyVertexProofTargetBuffer, 0,
			StableProxyVertexProofBuffer, 0, Bytes);
		RHICmdList.Transition(FRHITransitionInfo(
			StableProxyVertexProofBuffer, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
		RHICmdList.Transition(FRHITransitionInfo(
			StableProxyVertexProofTargetBuffer, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
	}

	void* Front = RHICmdList.LockBuffer(StableProxyVertexProofBuffer, 0, Bytes, RLM_WriteOnly);
	FMemory::Memcpy(Front, InProofs.GetData(), Bytes);
	RHICmdList.UnlockBuffer(StableProxyVertexProofBuffer);
	if (StableProxyFieldValid == 0u && StableProxyVertexProofTargetBuffer.IsValid())
	{
		// The first field has no history; initialize Previous to the same complete
		// result so startup cannot blend against uninitialized GPU memory.
		void* Previous = RHICmdList.LockBuffer(StableProxyVertexProofTargetBuffer, 0, Bytes, RLM_WriteOnly);
		FMemory::Memcpy(Previous, InProofs.GetData(), Bytes);
		RHICmdList.UnlockBuffer(StableProxyVertexProofTargetBuffer);
	}
	StableProxyFieldValid = 1u;
	StableProxyFieldPairValid = 1u;
	StableProxyLiveTargetValid = 1u;
	StableProxyCastGridValid = 0u;
}

void FGaussianSplatGPUResources::ReleaseRHI()
{
	// Release shared buffer refs (just ref-count decrements, actual GPU memory
	// is freed when SharedData releases its refs)
	PackedSplatBuffer.SafeRelease();
	PackedSplatBufferSRV.SafeRelease();
	Float32PositionBuffer.SafeRelease();
	Float32PositionBufferSRV.SafeRelease();
	SHBuffer.SafeRelease();
	SHBufferSRV.SafeRelease();
	ChunkBuffer.SafeRelease();
	ChunkBufferSRV.SafeRelease();
	IndexBuffer.SafeRelease();
	ClusterBuffer.SafeRelease();
	ClusterBufferSRV.SafeRelease();
	SplatClusterIndexBuffer.SafeRelease();
	SplatClusterIndexBufferSRV.SafeRelease();
	ClusterLightingBuffer.SafeRelease();
	ClusterLightingBufferSRV.SafeRelease();
	ProxyNormalGridBuffer.SafeRelease();
	ProxyNormalGridBufferSRV.SafeRelease();
	StableSplatBindingBuffer.SafeRelease();
	StableSplatBindingBufferSRV.SafeRelease();
	PuddleDampenPackedBuffer.SafeRelease();
	PuddleDampenPackedBufferSRV.SafeRelease();
	SnowCoveragePackedBuffer.SafeRelease();
	SnowCoveragePackedBufferSRV.SafeRelease();
	SnowBakedFieldBuffer2873.SafeRelease();
	SnowBakedFieldBufferSRV2873.SafeRelease();
	StableProxyVertexBuffer.SafeRelease();
	StableProxyVertexBufferSRV.SafeRelease();
	StableProxyTriangleBuffer.SafeRelease();
	StableProxyTriangleBufferSRV.SafeRelease();
	StableProxyTriangleVertexNormalBuffer.SafeRelease();
	StableProxyTriangleVertexNormalBufferSRV.SafeRelease();
	StableProxyVertexProofBuffer.SafeRelease();
	StableProxyVertexProofBufferSRV.SafeRelease();
	StableProxyVertexProofBufferUAV.SafeRelease();
	StableProxyVertexProofTargetBuffer.SafeRelease();
	StableProxyVertexProofTargetBufferSRV.SafeRelease();
	StableProxyVertexProofTargetBufferUAV.SafeRelease();
	StableProxyVertexOwnerBuffer.SafeRelease();
	StableProxyVertexOwnerBufferSRV.SafeRelease();
	StableProxyVertexOwnerBufferUAV.SafeRelease();
	StableProxyVertexOwnerTargetBuffer.SafeRelease();
	StableProxyVertexOwnerTargetBufferSRV.SafeRelease();
	StableProxyVertexOwnerTargetBufferUAV.SafeRelease();
	StableProxyReceiverTriangleIdBuffer.SafeRelease();
	StableProxyReceiverTriangleIdBufferSRV.SafeRelease();
	StableProxyReceiverTriangleIdBufferUAV.SafeRelease();
	StableProxyCastGridBuffer.SafeRelease();
	StableProxyCastGridBufferSRV.SafeRelease();
	StableProxyCastGridBufferUAV.SafeRelease();
	StableSplatBindingCapacity = 0u;
	StableProxyVertexCapacity = 0u;
	StableProxyTriangleCapacity = 0u;
	StableProxyTriangleVertexNormalCapacity = 0u;
	StableProxyTriangleVertexNormalCount = 0u;

	// Release legacy buffers
	PositionBuffer.SafeRelease();
	PositionBufferSRV.SafeRelease();
	OtherDataBuffer.SafeRelease();
	OtherDataBufferSRV.SafeRelease();

	// Release per-instance buffers
	ViewDataBuffer.SafeRelease();
	ViewDataBufferUAV.SafeRelease();
	ViewDataBufferSRV.SafeRelease();
	SortDistanceBuffer.SafeRelease();
	SortDistanceBufferUAV.SafeRelease();
	SortDistanceBufferSRV.SafeRelease();
	SortKeysBuffer.SafeRelease();
	SortKeysBufferUAV.SafeRelease();
	SortKeysBufferSRV.SafeRelease();
	SortKeysBufferAlt.SafeRelease();
	SortKeysBufferAltUAV.SafeRelease();
	SortKeysBufferAltSRV.SafeRelease();
	SortDistanceBufferAlt.SafeRelease();
	SortDistanceBufferAltUAV.SafeRelease();
	RadixHistogramBuffer.SafeRelease();
	RadixHistogramBufferUAV.SafeRelease();
	RadixDigitOffsetBuffer.SafeRelease();
	RadixDigitOffsetBufferUAV.SafeRelease();
	SortIndirectArgsBuffer.SafeRelease();
	SortIndirectArgsBufferUAV.SafeRelease();
	SortParamsBuffer.SafeRelease();
	SortParamsBufferUAV.SafeRelease();
	SortParamsBufferSRV.SafeRelease();
	ColorTexture.SafeRelease();
	ColorTextureSRV.SafeRelease();
	DummyWhiteTexture.SafeRelease();
	DummyWhiteTextureSRV.SafeRelease();
	VisibleClusterBuffer.SafeRelease();
	VisibleClusterBufferUAV.SafeRelease();
	VisibleClusterBufferSRV.SafeRelease();
	VisibleClusterCountBuffer.SafeRelease();
	VisibleClusterCountBufferUAV.SafeRelease();
	VisibleClusterCountBufferSRV.SafeRelease();

	// Release LOD splat buffers
	LODSplatBuffer.SafeRelease();
	LODSplatBufferSRV.SafeRelease();

	// Release indirect draw buffers
	IndirectDrawArgsBuffer.SafeRelease();
	IndirectDrawArgsBufferUAV.SafeRelease();

	// Release cluster visibility integration buffers
	ClusterVisibilityBitmap.SafeRelease();
	ClusterVisibilityBitmapUAV.SafeRelease();
	ClusterVisibilityBitmapSRV.SafeRelease();
	SelectedClusterBuffer.SafeRelease();
	SelectedClusterBufferUAV.SafeRelease();
	SelectedClusterBufferSRV.SafeRelease();

	// Release LOD cluster tracking buffers
	LODClusterBuffer.SafeRelease();
	LODClusterBufferUAV.SafeRelease();
	LODClusterBufferSRV.SafeRelease();
	LODClusterCountBuffer.SafeRelease();
	LODClusterCountBufferUAV.SafeRelease();
	LODClusterCountBufferSRV.SafeRelease();
	LODClusterSelectedBitmap.SafeRelease();
	LODClusterSelectedBitmapUAV.SafeRelease();
	LODClusterSelectedBitmapSRV.SafeRelease();
	LODSplatTotalBuffer.SafeRelease();
	LODSplatTotalBufferUAV.SafeRelease();
	LODSplatTotalBufferSRV.SafeRelease();

	// Release GPU-driven LOD rendering buffers
	LODSplatClusterIndexBuffer.SafeRelease();
	LODSplatClusterIndexBufferSRV.SafeRelease();
	LODSplatOutputCountBuffer.SafeRelease();
	LODSplatOutputCountBufferUAV.SafeRelease();
	LODSplatOutputCountBufferSRV.SafeRelease();

	// Release splat compaction buffers
	CompactedSplatIndicesBuffer.SafeRelease();
	CompactedSplatIndicesBufferUAV.SafeRelease();
	CompactedSplatIndicesBufferSRV.SafeRelease();
	VisibleSplatCountBuffer.SafeRelease();
	VisibleSplatCountBufferUAV.SafeRelease();
	VisibleSplatCountBufferSRV.SafeRelease();
	IndirectDispatchArgsBuffer.SafeRelease();
	IndirectDispatchArgsBufferUAV.SafeRelease();

	// Release shared data reference
	SharedData.Reset();

	bInitialized = false;
}

void FGaussianSplatGPUResources::CreateDummyWhiteTexture(FRHICommandListBase& RHICmdList)
{
	// Create a 1x1 white texture
	const FRHITextureCreateDesc TextureDesc =
		FRHITextureCreateDesc::Create2D(TEXT("GaussianSplatDummyWhiteTexture"), 1, 1, PF_R8G8B8A8)
		.SetFlags(ETextureCreateFlags::ShaderResource)
		.SetInitialState(ERHIAccess::SRVMask);

	DummyWhiteTexture = RHICreateTexture(TextureDesc);

	// Fill with white color
	uint32 WhitePixel = 0xFFFFFFFF;  // RGBA = (255, 255, 255, 255)
	FUpdateTextureRegion2D Region(0, 0, 0, 0, 1, 1);
	RHIUpdateTexture2D(DummyWhiteTexture, 0, Region, sizeof(uint32), (const uint8*)&WhitePixel);

	// Create SRV
	DummyWhiteTextureSRV = RHICmdList.CreateShaderResourceView(
		DummyWhiteTexture,
		FRHIViewDesc::CreateTextureSRV()
			.SetDimension(ETextureDimension::Texture2D));
}

void FGaussianSplatGPUResources::CreatePerInstanceBuffers(FRHICommandListBase& RHICmdList)
{
	// When Nanite is disabled (no cluster data), create dummy buffers for shader binding
	if (!bHasClusterData)
	{
		// Dummy ClusterVisibilityBitmap (1 uint)
		{
			const uint32 BufferSize = sizeof(uint32);
			FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
				TEXT("GaussianClusterVisibilityBitmapDummy"),
				BufferSize,
				sizeof(uint32),
				BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
				.SetInitialState(ERHIAccess::SRVMask);
			ClusterVisibilityBitmap = RHICmdList.CreateBuffer(Desc);

			uint32 Zero = 0;
			void* Data = RHICmdList.LockBuffer(ClusterVisibilityBitmap, 0, BufferSize, RLM_WriteOnly);
			FMemory::Memcpy(Data, &Zero, BufferSize);
			RHICmdList.UnlockBuffer(ClusterVisibilityBitmap);

			ClusterVisibilityBitmapSRV = RHICmdList.CreateShaderResourceView(
				ClusterVisibilityBitmap, FRHIViewDesc::CreateBufferSRV()
					.SetType(FRHIViewDesc::EBufferType::Structured)
					.SetStride(sizeof(uint32)));
		}

		// Dummy LODClusterSelectedBitmap (1 uint)
		{
			const uint32 BufferSize = sizeof(uint32);
			FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
				TEXT("GaussianLODClusterSelectedBitmapDummy"),
				BufferSize,
				sizeof(uint32),
				BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
				.SetInitialState(ERHIAccess::SRVMask);
			LODClusterSelectedBitmap = RHICmdList.CreateBuffer(Desc);

			uint32 Zero = 0;
			void* Data = RHICmdList.LockBuffer(LODClusterSelectedBitmap, 0, BufferSize, RLM_WriteOnly);
			FMemory::Memcpy(Data, &Zero, BufferSize);
			RHICmdList.UnlockBuffer(LODClusterSelectedBitmap);

			LODClusterSelectedBitmapSRV = RHICmdList.CreateShaderResourceView(
				LODClusterSelectedBitmap, FRHIViewDesc::CreateBufferSRV()
					.SetType(FRHIViewDesc::EBufferType::Structured)
					.SetStride(sizeof(uint32)));
		}

		// Dummy SelectedClusterBuffer (1 uint)
		{
			const uint32 BufferSize = sizeof(uint32);
			FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
				TEXT("GaussianSelectedClusterBufferDummy"),
				BufferSize,
				sizeof(uint32),
				BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
				.SetInitialState(ERHIAccess::SRVMask);
			SelectedClusterBuffer = RHICmdList.CreateBuffer(Desc);

			uint32 Zero = 0;
			void* Data = RHICmdList.LockBuffer(SelectedClusterBuffer, 0, BufferSize, RLM_WriteOnly);
			FMemory::Memcpy(Data, &Zero, BufferSize);
			RHICmdList.UnlockBuffer(SelectedClusterBuffer);

			SelectedClusterBufferSRV = RHICmdList.CreateShaderResourceView(
				SelectedClusterBuffer, FRHIViewDesc::CreateBufferSRV()
					.SetType(FRHIViewDesc::EBufferType::Structured)
					.SetStride(sizeof(uint32)));
		}

		// Dummy CompactedSplatIndicesBuffer (1 uint) - needed even when UseCompaction=0
		{
			const uint32 BufferSize = sizeof(uint32);
			FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
				TEXT("GaussianCompactedSplatIndicesBufferDummy"),
				BufferSize,
				sizeof(uint32),
				BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
				.SetInitialState(ERHIAccess::SRVMask);
			CompactedSplatIndicesBuffer = RHICmdList.CreateBuffer(Desc);

			uint32 Zero = 0;
			void* Data = RHICmdList.LockBuffer(CompactedSplatIndicesBuffer, 0, BufferSize, RLM_WriteOnly);
			FMemory::Memcpy(Data, &Zero, BufferSize);
			RHICmdList.UnlockBuffer(CompactedSplatIndicesBuffer);

			CompactedSplatIndicesBufferSRV = RHICmdList.CreateShaderResourceView(
				CompactedSplatIndicesBuffer, FRHIViewDesc::CreateBufferSRV()
					.SetType(FRHIViewDesc::EBufferType::Structured)
					.SetStride(sizeof(uint32)));
		}

		return;
	}

	// Create visible cluster buffer (dynamic, written by culling shader)
	// Size = max possible visible clusters (all clusters could be visible)
	{
		const uint32 BufferSize = ClusterCount * sizeof(uint32);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianVisibleClusterBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		VisibleClusterBuffer = RHICmdList.CreateBuffer(Desc);

		VisibleClusterBufferUAV = RHICmdList.CreateUnorderedAccessView(
			VisibleClusterBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		VisibleClusterBufferSRV = RHICmdList.CreateShaderResourceView(
			VisibleClusterBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	// Create visible cluster count buffer (single uint, atomic counter)
	{
		const uint32 BufferSize = sizeof(uint32);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianVisibleClusterCountBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		VisibleClusterCountBuffer = RHICmdList.CreateBuffer(Desc);

		VisibleClusterCountBufferUAV = RHICmdList.CreateUnorderedAccessView(
			VisibleClusterCountBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		VisibleClusterCountBufferSRV = RHICmdList.CreateShaderResourceView(
			VisibleClusterCountBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	// Create indirect draw argument buffer for GPU-driven rendering
	// Structure: IndexCountPerInstance(4), InstanceCount(4), StartIndexLocation(4), BaseVertexLocation(4), StartInstanceLocation(4)
	// Total: 20 bytes, but we use 32 bytes for alignment
	{
		const uint32 BufferSize = 32;  // 5 uints + padding for alignment
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianIndirectDrawArgsBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_DrawIndirect | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::IndirectArgs);
		IndirectDrawArgsBuffer = RHICmdList.CreateBuffer(Desc);

		// Initialize with default values
		// IndexCountPerInstance = 6 (2 triangles per quad)
		// InstanceCount = SplatCount (will be updated by culling shader)
		// StartIndexLocation = 0
		// BaseVertexLocation = 0
		// StartInstanceLocation = 0
		uint32 InitData[8] = { 6, (uint32)SplatCount, 0, 0, 0, 0, 0, 0 };
		void* Data = RHICmdList.LockBuffer(IndirectDrawArgsBuffer, 0, BufferSize, RLM_WriteOnly);
		FMemory::Memcpy(Data, InitData, BufferSize);
		RHICmdList.UnlockBuffer(IndirectDrawArgsBuffer);

		IndirectDrawArgsBufferUAV = RHICmdList.CreateUnorderedAccessView(
			IndirectDrawArgsBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));

		bSupportsIndirectDraw = true;
		UE_LOG(LogTemp, Verbose, TEXT("GaussianSplatGPUResources: Created indirect draw buffer"));
	}

	// Create cluster visibility bitmap buffer
	// One bit per cluster, rounded up to uint32 boundary
	{
		uint32 BitmapSize = FMath::DivideAndRoundUp(ClusterCount, 32) * sizeof(uint32);
		BitmapSize = FMath::Max(BitmapSize, (uint32)sizeof(uint32));  // At least one uint32

		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianClusterVisibilityBitmap"),
			BitmapSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		ClusterVisibilityBitmap = RHICmdList.CreateBuffer(Desc);

		ClusterVisibilityBitmapUAV = RHICmdList.CreateUnorderedAccessView(
			ClusterVisibilityBitmap, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		ClusterVisibilityBitmapSRV = RHICmdList.CreateShaderResourceView(
			ClusterVisibilityBitmap, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));

		UE_LOG(LogTemp, Verbose, TEXT("GaussianSplatGPUResources: Created cluster visibility bitmap (%d bytes for %d clusters)"), BitmapSize, ClusterCount);
	}

	// Create selected cluster buffer for Nanite-style debug visualization
	// One entry per leaf cluster, stores which cluster ID is selected based on LOD
	{
		uint32 BufferSize = LeafClusterCount * sizeof(uint32);
		BufferSize = FMath::Max(BufferSize, (uint32)sizeof(uint32));  // At least one uint32

		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianSelectedClusterBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		SelectedClusterBuffer = RHICmdList.CreateBuffer(Desc);

		SelectedClusterBufferUAV = RHICmdList.CreateUnorderedAccessView(
			SelectedClusterBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		SelectedClusterBufferSRV = RHICmdList.CreateShaderResourceView(
			SelectedClusterBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));

		UE_LOG(LogTemp, Verbose, TEXT("GaussianSplatGPUResources: Created selected cluster buffer (%d bytes for %d leaf clusters)"), BufferSize, LeafClusterCount);
	}

	// Create LOD cluster tracking buffers for LOD rendering
	// LOD cluster buffer - stores unique parent cluster indices
	{
		uint32 BufferSize = ClusterCount * sizeof(uint32);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianLODClusterBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		LODClusterBuffer = RHICmdList.CreateBuffer(Desc);

		LODClusterBufferUAV = RHICmdList.CreateUnorderedAccessView(
			LODClusterBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		LODClusterBufferSRV = RHICmdList.CreateShaderResourceView(
			LODClusterBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	// LOD cluster count buffer (atomic counter)
	{
		uint32 BufferSize = sizeof(uint32);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianLODClusterCountBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		LODClusterCountBuffer = RHICmdList.CreateBuffer(Desc);

		LODClusterCountBufferUAV = RHICmdList.CreateUnorderedAccessView(
			LODClusterCountBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		LODClusterCountBufferSRV = RHICmdList.CreateShaderResourceView(
			LODClusterCountBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	// LOD cluster selected bitmap (same size as cluster visibility bitmap)
	// Also needs SRV for GPU-driven LOD shader to read
	{
		uint32 BitmapSize = FMath::DivideAndRoundUp(ClusterCount, 32) * sizeof(uint32);
		BitmapSize = FMath::Max(BitmapSize, (uint32)sizeof(uint32));

		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianLODClusterSelectedBitmap"),
			BitmapSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		LODClusterSelectedBitmap = RHICmdList.CreateBuffer(Desc);

		LODClusterSelectedBitmapUAV = RHICmdList.CreateUnorderedAccessView(
			LODClusterSelectedBitmap, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		LODClusterSelectedBitmapSRV = RHICmdList.CreateShaderResourceView(
			LODClusterSelectedBitmap, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	// LOD splat total buffer (atomic counter for total LOD splats)
	{
		uint32 BufferSize = sizeof(uint32);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianLODSplatTotalBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		LODSplatTotalBuffer = RHICmdList.CreateBuffer(Desc);

		LODSplatTotalBufferUAV = RHICmdList.CreateUnorderedAccessView(
			LODSplatTotalBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		LODSplatTotalBufferSRV = RHICmdList.CreateShaderResourceView(
			LODSplatTotalBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	// LOD splat output count buffer (atomic counter for valid LOD splat output)
	{
		uint32 BufferSize = sizeof(uint32);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianLODSplatOutputCountBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		LODSplatOutputCountBuffer = RHICmdList.CreateBuffer(Desc);

		LODSplatOutputCountBufferUAV = RHICmdList.CreateUnorderedAccessView(
			LODSplatOutputCountBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		LODSplatOutputCountBufferSRV = RHICmdList.CreateShaderResourceView(
			LODSplatOutputCountBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	UE_LOG(LogTemp, Verbose, TEXT("GaussianSplatGPUResources: Created LOD cluster tracking buffers"));

	// Create splat compaction buffers (GPU-driven work reduction). Correctness
	// requires capacity for every source splat because atomic append order is not
	// camera-distance ordered. The bounded shader append remains as an OOB guard.
	// CompactedSplatIndicesBuffer - stores visible splat indices.
	{
		CompactionCapacity = FMath::Max(1u, uint32(TotalSplatCount));
		const uint32 BufferSize = CompactionCapacity * sizeof(uint32);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianCompactedSplatIndicesBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		CompactedSplatIndicesBuffer = RHICmdList.CreateBuffer(Desc);

		CompactedSplatIndicesBufferUAV = RHICmdList.CreateUnorderedAccessView(
			CompactedSplatIndicesBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		CompactedSplatIndicesBufferSRV = RHICmdList.CreateShaderResourceView(
			CompactedSplatIndicesBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	// VisibleSplatCountBuffer - atomic counter for visible splat count
	{
		const uint32 BufferSize = sizeof(uint32);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianVisibleSplatCountBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		VisibleSplatCountBuffer = RHICmdList.CreateBuffer(Desc);

		VisibleSplatCountBufferUAV = RHICmdList.CreateUnorderedAccessView(
			VisibleSplatCountBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		VisibleSplatCountBufferSRV = RHICmdList.CreateShaderResourceView(
			VisibleSplatCountBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	// IndirectDispatchArgsBuffer - for indirect compute dispatch
	// Format: uint3 (numGroupsX, numGroupsY, numGroupsZ)
	{
		const uint32 BufferSize = 3 * sizeof(uint32);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianIndirectDispatchArgsBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_DrawIndirect | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::IndirectArgs);
		IndirectDispatchArgsBuffer = RHICmdList.CreateBuffer(Desc);

		// Initialize with default values (1, 1, 1)
		uint32 InitData[3] = { 1, 1, 1 };
		void* Data = RHICmdList.LockBuffer(IndirectDispatchArgsBuffer, 0, BufferSize, RLM_WriteOnly);
		FMemory::Memcpy(Data, InitData, BufferSize);
		RHICmdList.UnlockBuffer(IndirectDispatchArgsBuffer);

		IndirectDispatchArgsBufferUAV = RHICmdList.CreateUnorderedAccessView(
			IndirectDispatchArgsBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	// Sort indirect args + sort params: needed by PrepareIndirectArgs in the
	// Nanite compaction path. Created here (not in CreateDynamicBuffers) because
	// per-proxy dynamic buffers are skipped under the global accumulator, but
	// PrepareIndirectArgs still runs per-proxy to set up indirect dispatch for
	// CalcViewDataCompactedGlobal.
	{
		const uint32 BufferSize = 3 * sizeof(uint32);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianSortIndirectArgsBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_DrawIndirect | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		SortIndirectArgsBuffer = RHICmdList.CreateBuffer(Desc);
		SortIndirectArgsBufferUAV = RHICmdList.CreateUnorderedAccessView(
			SortIndirectArgsBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}
	{
		const uint32 BufferSize = 2 * sizeof(uint32);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianSortParamsBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		SortParamsBuffer = RHICmdList.CreateBuffer(Desc);
		SortParamsBufferUAV = RHICmdList.CreateUnorderedAccessView(
			SortParamsBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		SortParamsBufferSRV = RHICmdList.CreateShaderResourceView(
			SortParamsBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	bSupportsCompaction = true;
	UE_LOG(LogTemp, Log, TEXT("GaussianSplatGPUResources: Compaction list capacity %u / %d source splats (%.1f MiB)"), CompactionCapacity, TotalSplatCount, double(CompactionCapacity * sizeof(uint32)) / (1024.0 * 1024.0));
	UE_LOG(LogTemp, Verbose, TEXT("GaussianSplatGPUResources: Created cluster buffers for %d clusters"), ClusterCount);
}

//////////////////////////////////////////////////////////////////////////
// FGaussianSplatSceneProxy

//Constructor. Copies rendering parameters from the component
FGaussianSplatSceneProxy::FGaussianSplatSceneProxy(const UGaussianSplatComponent* InComponent)
	: FPrimitiveSceneProxy(InComponent)
	, CachedAsset(InComponent->SplatAsset)
	, SplatCount(InComponent->SplatAsset ? InComponent->SplatAsset->GetSplatCount() : 0)
	, SHOrder(InComponent->SHOrder)
	, OpacityScale(InComponent->OpacityScale)
	, SplatScale(InComponent->SplatScale)
	, LODErrorThreshold(InComponent->LODErrorThreshold)
	, RenderProfileValue(static_cast<int32>(InComponent->RenderProfile))
	, LightingPreviewMode(static_cast<int32>(InComponent->LightingPreviewMode))
	, RuntimePositionPrecisionValue(static_cast<int32>(InComponent->RuntimePositionPrecision))
	, RuntimeColorPrecisionValue(static_cast<int32>(InComponent->RuntimeColorPrecision))
	, RuntimeFootprintModeValue(static_cast<int32>(InComponent->RuntimeFootprintMode))
	, RuntimeFootprintParams0(InComponent->AdaptiveFootprintStrength, InComponent->FootprintNearMinRadiusPixels, InComponent->FootprintMinRadiusPixels, InComponent->FootprintFarMinRadiusPixels)
	, RuntimeFootprintParams1(InComponent->FootprintNearMaxRadiusPixels, InComponent->FootprintMidMaxRadiusPixels, InComponent->FootprintFarMaxRadiusPixels, InComponent->FootprintAnisotropyLimit)
	, RuntimeFootprintParams2(InComponent->FootprintNearDistance, InComponent->FootprintFarDistance, InComponent->FootprintOpacityCompensation, InComponent->FootprintOpacityCompensationMaxExponent)
	, RuntimeFootprintParams3(InComponent->FootprintRadiusQuantization, InComponent->FootprintAdaptiveLookSafety, 0.0f, 0.0f)
	, ProxyNormalGridData(InComponent->GetProxyNormalGridData())
	, ProxyNormalGridMinCellSize(InComponent->GetProxyNormalGridMinCellSize())
	, ProxyNormalGridDimensionsStatus(InComponent->GetProxyNormalGridDimensionsStatus())
	, StableSplatBindingData(InComponent->GetStableSplatBindingPayload())
	, PuddleDampenPackedData([InComponent]() -> TArray<uint32>
	{
		const AGaussianSplatActor* A = Cast<AGaussianSplatActor>(InComponent->GetOwner());
		return A && A->GaussianSplatComponent == InComponent && A->HasCurrentPuddleDampen(InComponent)
			? A->PuddleSourceDampenPackedBits : TArray<uint32>();
	}())
	, SnowCoveragePackedData(InComponent->HasCurrentSnowCoverageMask() ? InComponent->GetSnowCoveragePackedMask() : TArray<uint32>())
	, SnowBakedFieldPackedData2873(InComponent->HasCurrentSnowBakedField2873() ? InComponent->GetSnowBakedFieldPacked2873() : TArray<uint32>())
	, SnowBakedFieldPatternCm2873(InComponent->GetSnowBakedFieldPatternCm2873())
	, SnowBakedFieldTransform2873(InComponent->GetSnowBakedFieldLocalToWorld2873())
	, StableProxyVertexData(InComponent->GetStableProxyVertices())
	, StableProxyTriangleData(InComponent->GetStableProxyTriangles())
	, StableProxyTriangleVertexNormalData(InComponent->GetStableProxyTriangleVertexNormals())
	, StableProxyRayProofData(InComponent->GetStableProxyRayProofs())
	, StableRayProofRevisionValue(InComponent->GetStableRayProofRevision())
	, StableBindingRevisionValue(InComponent->GetStableBindingRevision())
	, StableFieldRequestedRevisionRT(InComponent->GetStableFieldRequestRevision())
	, StableFieldExpectedLightDepthRevisionRT(InComponent->GetStableFieldExpectedLightDepthRevision())
	, StableFieldDepthBiasCmRT(InComponent->GetStableFieldDepthBiasCm())
	, bForceFullSortEveryFrame(InComponent->bForceFullSortEveryFrame)
	, bDisableLODInReference(InComponent->bDisableLODInReference)
	, LightingData(InComponent->GetCachedLightingData())
{
	bWillEverBeLit = LightingData.bReceiveLighting != 0;
	const AGaussianSplatActor* WaterOwner = Cast<AGaussianSplatActor>(InComponent->GetOwner());
	bPuddleProxyMainSource = WaterOwner && WaterOwner->GaussianSplatComponent == InComponent;
	bPuddleProxyWaterLayer = WaterOwner && WaterOwner->PuddleGaussianComponent == InComponent;
	if (const AGaussianSplatActor* SplatActor = Cast<AGaussianSplatActor>(InComponent->GetOwner()))
	{
	}

}

FGaussianSplatSceneProxy::~FGaussianSplatSceneProxy()
{
}

SIZE_T FGaussianSplatSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

uint32 FGaussianSplatSceneProxy::GetMemoryFootprint() const
{
	return sizeof(*this) + GetAllocatedSize();
}

FPrimitiveViewRelevance FGaussianSplatSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View);
	Result.bShadowRelevance = false; // Shadows are handled in the custom splat lighting bridge later; not a UE shadow caster yet.
	Result.bDynamicRelevance = true;
	Result.bStaticRelevance = false;
	Result.bRenderInMainPass = true;
	Result.bUsesLightingChannels = LightingData.bReceiveLighting != 0;
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();

	return Result;
}

#if WITH_EDITOR
HHitProxy* FGaussianSplatSceneProxy::CreateHitProxies(UPrimitiveComponent* Component, TArray<TRefCountPtr<HHitProxy>>& OutHitProxies)
{
	// Let the base class create the default HActor hit proxy for the owning actor.
	HHitProxy* DefaultProxy = FPrimitiveSceneProxy::CreateHitProxies(Component, OutHitProxies);
	// Cache it so GetDynamicMeshElements can use it without touching game-thread UObjects.
	SelectionHitProxy = DefaultProxy;
	return DefaultProxy;
}
#endif

void FGaussianSplatSceneProxy::GetDynamicMeshElements(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector) const
{
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		if (VisibilityMap & (1 << ViewIndex))
		{
			FPrimitiveDrawInterface* PDI = Collector.GetPDI(ViewIndex);

			// Draw bounds when selected
			if (IsSelected())
			{
				RenderBounds(PDI, ViewFamily.EngineShowFlags, GetBounds(), true);
			}

#if WITH_EDITOR
			// During the hit proxy pass, render a solid box covering the local bounds.
			// This makes the splat selectable by clicking anywhere within its bounds in
			// the editor viewport. The box is invisible in normal rendering.
			if (ViewFamily.EngineShowFlags.HitProxies)
			{
				const FBox LocalBox = GetLocalBounds().GetBox();
				const FVector3f Min(LocalBox.Min);
				const FVector3f Max(LocalBox.Max);

				FDynamicMeshBuilder MeshBuilder(Views[ViewIndex]->GetFeatureLevel());

				// Tangent basis (arbitrary ??not shaded, just needs to be valid)
				const FVector2f UV(0.f, 0.f);
				const FVector3f TX(1.f, 0.f, 0.f);
				const FVector3f TY(0.f, 1.f, 0.f);
				const FVector3f TZ(0.f, 0.f, 1.f);
				const FColor White = FColor::White;

				// 8 corners  (named by which axes are at Max: 0=Min, 1=Max)
				const int32 V000 = MeshBuilder.AddVertex(FVector3f(Min.X, Min.Y, Min.Z), UV, TX, TY, TZ, White);
				const int32 V100 = MeshBuilder.AddVertex(FVector3f(Max.X, Min.Y, Min.Z), UV, TX, TY, TZ, White);
				const int32 V010 = MeshBuilder.AddVertex(FVector3f(Min.X, Max.Y, Min.Z), UV, TX, TY, TZ, White);
				const int32 V110 = MeshBuilder.AddVertex(FVector3f(Max.X, Max.Y, Min.Z), UV, TX, TY, TZ, White);
				const int32 V001 = MeshBuilder.AddVertex(FVector3f(Min.X, Min.Y, Max.Z), UV, TX, TY, TZ, White);
				const int32 V101 = MeshBuilder.AddVertex(FVector3f(Max.X, Min.Y, Max.Z), UV, TX, TY, TZ, White);
				const int32 V011 = MeshBuilder.AddVertex(FVector3f(Min.X, Max.Y, Max.Z), UV, TX, TY, TZ, White);
				const int32 V111 = MeshBuilder.AddVertex(FVector3f(Max.X, Max.Y, Max.Z), UV, TX, TY, TZ, White);

				// -Z face
				MeshBuilder.AddTriangle(V000, V010, V100);
				MeshBuilder.AddTriangle(V010, V110, V100);
				// +Z face
				MeshBuilder.AddTriangle(V001, V101, V011);
				MeshBuilder.AddTriangle(V101, V111, V011);
				// -Y face
				MeshBuilder.AddTriangle(V000, V100, V001);
				MeshBuilder.AddTriangle(V100, V101, V001);
				// +Y face
				MeshBuilder.AddTriangle(V010, V011, V110);
				MeshBuilder.AddTriangle(V011, V111, V110);
				// -X face
				MeshBuilder.AddTriangle(V000, V001, V010);
				MeshBuilder.AddTriangle(V001, V011, V010);
				// +X face
				MeshBuilder.AddTriangle(V100, V110, V101);
				MeshBuilder.AddTriangle(V110, V111, V101);

				// Use default opaque surface material ??only its depth output matters here.
				// bDisableBackfaceCulling=true so all faces are drawn regardless of winding.
				UMaterialInterface* HitMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
				MeshBuilder.GetMesh(
					GetLocalToWorld(),
					HitMaterial->GetRenderProxy(),
					SDPG_World,
					/*bDisableBackfaceCulling=*/true,
					/*bReceivesDecals=*/false,
					/*bUseSelectionOutline=*/false,
					ViewIndex,
					Collector,
					SelectionHitProxy);
			}
#endif // WITH_EDITOR
		}
	}
}

//critical setup. creates FGaussianSplatGPUResources which uploads all the GPU buffers 
// registers itself with the ViewExtension
void FGaussianSplatSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	if (CachedAsset && CachedAsset->IsValid())
	{
		GPUResources = new FGaussianSplatGPUResources();
		GPUResources->bPuddleMainSource = bPuddleProxyMainSource;
		GPUResources->bPuddleWaterLayer = bPuddleProxyWaterLayer;
		GPUResources->RuntimePositionPrecision = (RuntimePositionPrecisionValue == 1) ? 1u : 0u;
		GPUResources->RuntimeColorPrecision = (RuntimeColorPrecisionValue == 1) ? 1u : 0u;
		GPUResources->RuntimeFootprintMode = (RuntimeFootprintModeValue == 1) ? 1u : 0u;
		GPUResources->FootprintParams0 = RuntimeFootprintParams0;
		GPUResources->FootprintParams1 = RuntimeFootprintParams1;
		GPUResources->FootprintParams2 = RuntimeFootprintParams2;
		GPUResources->FootprintParams3 = RuntimeFootprintParams3;
		GPUResources->SetProxyNormalGridData(ProxyNormalGridData, ProxyNormalGridMinCellSize, ProxyNormalGridDimensionsStatus);
		GPUResources->SetStableSplatBindingData(StableSplatBindingData);
		GPUResources->SetPuddleDampenPackedData(PuddleDampenPackedData);
		GPUResources->SetSnowCoveragePackedData(SnowCoveragePackedData);
		GPUResources->SetSnowBakedFieldData2873(SnowBakedFieldPackedData2873,
			SnowBakedFieldPatternCm2873, SnowBakedFieldTransform2873);
		GPUResources->SetStableProxySurfaceData(StableProxyVertexData, StableProxyTriangleData, StableProxyTriangleVertexNormalData);
		GPUResources->SetStableProxyRayProofData(StableProxyRayProofData);
		GPUResources->Initialize(CachedAsset);

		if (CVarGSPanoramaShadowTrace.GetValueOnRenderThread() > 0)
		{
			const bool bProofCountMatches = StableProxyRayProofData.Num() > 0 &&
				StableProxyRayProofData.Num() == static_cast<int32>(GPUResources->StableProxyVertexCount);
			UE_LOG(LogTemp, Log,
				TEXT("[GSPanoramaTrace][RT][ProxyInit] Proxy=%p Scene=%p BindingRev=%u CPUProofRev=%u CPUProofs=%d ProxyVerts=%u Triangles=%u Bindings=%u ProofCountMatches=%d GPUFieldValid=%u ProofSRV=%d Ready=%d"),
				static_cast<const void*>(this), static_cast<const void*>(&GetScene()), StableBindingRevisionValue,
				StableRayProofRevisionValue, StableProxyRayProofData.Num(), GPUResources->StableProxyVertexCount,
				GPUResources->StableProxyTriangleCount, GPUResources->StableSplatBindingCount,
				bProofCountMatches ? 1 : 0, GPUResources->StableProxyFieldValid,
				GPUResources->StableProxyVertexProofBufferSRV.IsValid() ? 1 : 0,
				(bProofCountMatches && GPUResources->StableProxyFieldValid != 0u &&
				 GPUResources->StableProxyVertexProofBufferSRV.IsValid() && StableRayProofRevisionValue != 0u) ? 1 : 0);
		}

		// v2801 Panorama/MRQ bootstrap. MRQ can create a fresh SceneProxy after the
		// component has already submitted this revision to an older proxy. Stage the
		// completed deterministic BVH field directly into every new proxy so the very
		// first panorama pane has StableProxyFieldValid=1 instead of failing open.
		if (StableRayProofRevisionValue != 0u && GPUResources->StableProxyFieldValid != 0u)
		{
			bStableProxyRayFieldActiveRT = true;
			bStableProxyRayFieldSkipLoggedRT = false;
			StableFieldRequestedRevisionRT = StableRayProofRevisionValue;
			StableFieldCompletedRevisionRT = StableRayProofRevisionValue;
			StableFieldCompletedLightDepthRevisionRT = 0u;
			UE_LOG(LogTemp, Log,
				TEXT("[GSPanoramaBootstrap] StableRay staged on SceneProxy creation Rev=%u Vertices=%u FieldValid=%u"),
				StableRayProofRevisionValue, GPUResources->StableProxyVertexCount, GPUResources->StableProxyFieldValid);
		}
		else if (StableBindingRevisionValue != 0u)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[GSPanoramaBootstrap] StableRay not ready at SceneProxy creation BindingRev=%u RayRev=%u Proofs=%d Vertices=%u; waiting for dynamic upload"),
				StableBindingRevisionValue, StableRayProofRevisionValue, StableProxyRayProofData.Num(),
				GPUResources->StableProxyVertexCount);
		}

		// The component keeps the authoritative completed CPU binding. This SceneProxy staging copy
		// is needed only during resource creation; release it immediately to avoid a permanent
		// second full-size CPU copy (about 8 bytes per splat).
		StableSplatBindingData.Reset();
		StableProxyVertexData.Empty();
		StableProxyTriangleData.Empty();
		StableProxyRayProofData.Empty();

		// Get color texture reference
		if (CachedAsset->ColorTexture)
		{
			// If platform data exists but no resource, try to create it
			FTextureResource* TextureResource = CachedAsset->ColorTexture->GetResource();
			FTexturePlatformData* PlatformData = CachedAsset->ColorTexture->GetPlatformData();
			int64 BulkDataSize = 0;
			if (PlatformData && PlatformData->Mips.Num() > 0)
			{
				BulkDataSize = PlatformData->Mips[0].BulkData.GetBulkDataSize();
			}

			if (!TextureResource && PlatformData && BulkDataSize > 0)
			{
				CachedAsset->ColorTexture->UpdateResource();
				TextureResource = CachedAsset->ColorTexture->GetResource();
			}

			if (TextureResource && TextureResource->TextureRHI)
			{
				GPUResources->ColorTexture = TextureResource->TextureRHI;
				GPUResources->ColorTextureSRV = RHICmdList.CreateShaderResourceView(
					GPUResources->ColorTexture,
					FRHIViewDesc::CreateTextureSRV()
						.SetDimension(ETextureDimension::Texture2D));
			}
		}

		// Register with view extension for rendering
		FGaussianSplatViewExtension* ViewExtension = FGaussianSplatViewExtension::Get();
		if (ViewExtension)
		{
			ViewExtension->RegisterProxy(const_cast<FGaussianSplatSceneProxy*>(this));
		}
	}
}

void FGaussianSplatSceneProxy::DestroyRenderThreadResources()
{
	// CRITICAL: Mark as pending destruction FIRST, before any other operations.
	// This prevents render commands that have already captured a pointer to this proxy
	// from accessing our resources. The atomic flag is checked by IsValidForRendering().
	MarkPendingDestruction();

	// Unregister from view extension (under lock, so no new render passes will see us)
	FGaussianSplatViewExtension* ViewExtension = FGaussianSplatViewExtension::Get();
	if (ViewExtension)
	{
		ViewExtension->UnregisterProxy(const_cast<FGaussianSplatSceneProxy*>(this));
	}

	// Flush any pending render commands that might be referencing our resources.
	// This ensures that any RDG passes that captured our proxy pointer have completed
	// before we release the GPU resources.
	// Note: We're already on the render thread, so this flushes GPU work.
	FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
	RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);

	if (GPUResources)
	{
		GPUResources->ReleaseResource();
		delete GPUResources;
		GPUResources = nullptr;
	}

}

void FGaussianSplatSceneProxy::UpdateStableSplatBindings_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	const TArray<FStableProxySplatBindingGPU>& InBindings,
	const TArray<FStableProxyVertexGPU>& InVertices,
	const TArray<FStableProxyTriangleGPU>& InTriangles,
	const TArray<FVector4f>& InTriangleVertexNormals,
	uint32 InRevision)
{
	if (bPendingDestruction.load(std::memory_order_acquire) || !GPUResources)
	{
		return;
	}
	if (StableBindingRevisionValue == InRevision)
	{
		return;
	}

	GPUResources->UpdateStableSplatBindingData_RenderThread(RHICmdList, InBindings);
	GPUResources->UpdateStableProxySurfaceData_RenderThread(RHICmdList, InVertices, InTriangles, InTriangleVertexNormals);
	StableBindingRevisionValue = InRevision;
	StableFieldCompletedRevisionRT = 0u;
	StableFieldCompletedLightDepthRevisionRT = 0u;
	bStableProxyRayFieldActiveRT = false;
	bStableProxyRayFieldSkipLoggedRT = false;
	GPUResources->StableProxyLiveTargetValid = 0u;
	GPUResources->StableProxyFieldFrontCaptureRevision = 0u;
	GPUResources->StableProxyFieldPreviousCaptureRevision = 0u;
	GPUResources->StableProxyLiveTargetCaptureRevision = 0u;
	UE_LOG(LogTemp, Log,
		TEXT("[GSStableBindingRT] Uploaded Rev=%u Count=%d SRV=%d WeldedVertices=%d Triangles=%d"),
		StableBindingRevisionValue, InBindings.Num(),
		(GPUResources->StableSplatBindingBufferSRV.IsValid() ? 1 : 0), InVertices.Num(), InTriangles.Num());
}

void FGaussianSplatSceneProxy::UpdateStableProxyRayProofs_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	const TArray<FVector4f>& InProofs,
	uint32 InRevision)
{
	if (bPendingDestruction.load(std::memory_order_acquire) || !GPUResources)
	{
		return;
	}

	// v2803: a fresh MRQ/Panorama SceneProxy is already initialized from the
	// component's completed CPU BVH field in InitRHI. SendRenderDynamicData still
	// submits the same revision once because its game-thread LastSubmitted value is
	// intentionally independent of SceneProxy lifetime. Re-uploading that identical
	// revision made the D3D12 path copy/transition/Lock the proof buffers while the
	// first panorama pane could already be consuming them. Skip the duplicate.
	if (InRevision != 0u &&
		StableFieldCompletedRevisionRT == InRevision &&
		GPUResources->StableProxyFieldValid != 0u)
	{
		if (CVarGSPanoramaShadowTrace.GetValueOnRenderThread() > 0)
		{
			UE_LOG(LogTemp, Log, TEXT("[GSPanoramaTrace][RT][RayUploadSkip] Proxy=%p Scene=%p Rev=%u GPUFieldRev=%u Valid=%u Reason=DuplicateRevision"),
				static_cast<const void*>(this), static_cast<const void*>(&GetScene()),
				InRevision, StableFieldCompletedRevisionRT, GPUResources->StableProxyFieldValid);
		}
		UE_LOG(LogTemp, Verbose,
			TEXT("[GSPanoramaBootstrap] Duplicate StableRay upload skipped Rev=%u FieldValid=1"),
			InRevision);
		return;
	}

	GPUResources->UpdateStableProxyRayProofData_RenderThread(RHICmdList, InProofs);
	bStableProxyRayFieldActiveRT = true;
	bStableProxyRayFieldSkipLoggedRT = false;
	// Consume any legacy request already queued before this deterministic field
	// arrived. EnsureStableProxyField_RenderThread also guards future requests.
	StableFieldRequestedRevisionRT = InRevision;
	StableFieldCompletedRevisionRT = InRevision;
	StableFieldCompletedLightDepthRevisionRT = 0u;
	if (CVarGSPanoramaShadowTrace.GetValueOnRenderThread() > 0)
	{
		UE_LOG(LogTemp, Log,
			TEXT("[GSPanoramaTrace][RT][RayUpload] Proxy=%p Scene=%p Rev=%u Proofs=%d GPUFieldRev=%u GPUFieldValid=%u ProofSRV=%d"),
			static_cast<const void*>(this), static_cast<const void*>(&GetScene()), InRevision,
			InProofs.Num(), StableFieldCompletedRevisionRT, GPUResources->StableProxyFieldValid,
			GPUResources->StableProxyVertexProofBufferSRV.IsValid() ? 1 : 0);
	}
	UE_LOG(LogTemp, Log, TEXT("[GSStableRayRT] Uploaded Rev=%u Vertices=%d DeterministicBVH=1 LightDepth=0"),
		InRevision, InProofs.Num());
}

void FGaussianSplatSceneProxy::EnsureStableProxyField_RenderThread(FRHICommandListImmediate& RHICmdList, const FMatrix& InLocalToWorld)
{
	if (bPendingDestruction.load(std::memory_order_acquire) || !GPUResources)
	{
		return;
	}

	// Deterministic BVH is the sole cast-shadow authority. MRQ creates additional
	// scene proxies which may receive a legacy LightDepth request before their first
	// CPU ray upload; allowing those requests to run can still write the shared cast
	// buffers and reintroduce the old frame-dependent result. Never dispatch the
	// legacy vertex visibility or dense cast-grid builders for any proxy.
	if (GDeterministicBVHCastOnly)
	{
		if (!bStableProxyRayFieldSkipLoggedRT)
		{
			UE_LOG(LogTemp, Log,
				TEXT("[GSStableFieldRT] DisabledGlobally Reason=DeterministicBVHCastOnly RayActive=%d Rev=%u LegacyLightDepthDispatch=0 DenseCastGridDispatch=0"),
				bStableProxyRayFieldActiveRT ? 1 : 0, StableFieldCompletedRevisionRT);
			bStableProxyRayFieldSkipLoggedRT = true;
		}
		return;
	}

	const uint32 CurrentLightDepthRevision = FGaussianSplatRenderer::GetProxyLightDepthCaptureRevision_RenderThread();
	const bool bRequestPending =
		StableFieldRequestedRevisionRT != 0u && StableFieldCompletedRevisionRT != StableFieldRequestedRevisionRT;
	if (!bRequestPending)
	{
		return;
	}

	// Keep the packet-bound safety proven during the v2771/v2772 investigation, even
	// though commit timing itself was not the dominant flicker source. Never classify a
	// field against a LightDepth texture/projection packet from a different revision.
	if (StableFieldExpectedLightDepthRevisionRT != 0u &&
		CurrentLightDepthRevision != StableFieldExpectedLightDepthRevisionRT)
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("[GSStablePacket] Deferred RequestRev=%u ExpectedLightDepthRev=%u CurrentLightDepthRev=%u FrontCaptureRev=%u"),
			StableFieldRequestedRevisionRT, StableFieldExpectedLightDepthRevisionRT,
			CurrentLightDepthRevision, GPUResources->StableProxyFieldFrontCaptureRevision);
		return;
	}

	const uint32 BuildLightDepthRevision = StableFieldExpectedLightDepthRevisionRT != 0u
		? StableFieldExpectedLightDepthRevisionRT
		: CurrentLightDepthRevision;
	const uint32 PreviousFrontCaptureRevision = GPUResources->StableProxyFieldFrontCaptureRevision;

	const bool bTargetReady = FGaussianSplatRenderer::DispatchBuildStableProxyVertexVisibility(
		RHICmdList, GPUResources, InLocalToWorld, StableFieldDepthBiasCmRT, true);
	const bool bCastGridReady = bTargetReady &&
		FGaussianSplatRenderer::DispatchBuildStableProxyCastGrid(
			RHICmdList, GPUResources, StableFieldDepthBiasCmRT);

	if (bTargetReady)
	{
		GPUResources->StableProxyLiveTargetValid = 0u;
		GPUResources->StableProxyLiveTargetCaptureRevision = 0u;
		GPUResources->StableProxyFieldPreviousCaptureRevision = PreviousFrontCaptureRevision;
		GPUResources->StableProxyFieldFrontCaptureRevision = BuildLightDepthRevision;
		GPUResources->StableProxyFieldValid = 1u;
		++GPUResources->StableProxyFieldBuildCount;
		GPUResources->StableProxyFieldPairValid =
			GPUResources->StableProxyFieldBuildCount >= 2u ? 1u : 0u;

		StableFieldCompletedLightDepthRevisionRT = BuildLightDepthRevision;
		StableFieldCompletedRevisionRT = StableFieldRequestedRevisionRT;

		UE_LOG(LogTemp, Log,
			TEXT("[GSStableFieldRT] State=Ready Rev=%u LightDepthRev=%u CurrentLightDepthRev=%u Vertices=%u Triangles=%u BiasCm=%.3f ContinuousCast21=1 DenseCastGrid=%d AtomicTargetCommit=1 FrontCaptureRev=%u"),
			StableFieldCompletedRevisionRT, StableFieldCompletedLightDepthRevisionRT, CurrentLightDepthRevision,
			GPUResources->StableProxyVertexCount, GPUResources->StableProxyTriangleCount, StableFieldDepthBiasCmRT,
			bCastGridReady ? 1 : 0,
			GPUResources->StableProxyFieldFrontCaptureRevision);
	}
	else
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[GSStableFieldRT] Target build deferred/failed RequestedRev=%u ExpectedLightDepthRev=%u CurrentLightDepthRev=%u FrontRev=%u FrontCaptureRev=%u FrontValid=%u"),
			StableFieldRequestedRevisionRT, StableFieldExpectedLightDepthRevisionRT, CurrentLightDepthRevision,
			StableFieldCompletedRevisionRT, GPUResources->StableProxyFieldFrontCaptureRevision,
			GPUResources->StableProxyFieldValid);
	}
}

void FGaussianSplatSceneProxy::TryInitializeColorTexture(FRHICommandListBase& RHICmdList)
{
	// Safety check: don't access resources if proxy is being destroyed
	if (bPendingDestruction.load(std::memory_order_acquire))
	{
		return;
	}

	if (!GPUResources || GPUResources->ColorTextureSRV.IsValid())
	{
		// Already initialized or no resources
		return;
	}

	if (!CachedAsset || !CachedAsset->ColorTexture)
	{
		return;
	}

	FTextureResource* TextureResource = CachedAsset->ColorTexture->GetResource();

	// If resource doesn't exist but platform data does, try to create it
	if (!TextureResource)
	{
		FTexturePlatformData* PlatformData = CachedAsset->ColorTexture->GetPlatformData();
		int64 BulkDataSize = 0;
		if (PlatformData && PlatformData->Mips.Num() > 0)
		{
			BulkDataSize = PlatformData->Mips[0].BulkData.GetBulkDataSize();
		}

		if (PlatformData && BulkDataSize > 0)
		{
			CachedAsset->ColorTexture->UpdateResource();
			TextureResource = CachedAsset->ColorTexture->GetResource();
		}
	}

	if (TextureResource && TextureResource->TextureRHI)
	{
		GPUResources->ColorTexture = TextureResource->TextureRHI;
		GPUResources->ColorTextureSRV = RHICmdList.CreateShaderResourceView(
			GPUResources->ColorTexture,
			FRHIViewDesc::CreateTextureSRV()
				.SetDimension(ETextureDimension::Texture2D));
	}
}
