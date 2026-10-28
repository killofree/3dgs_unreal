// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "GaussianDataTypes.h"
#include "GaussianClusterTypes.h"
#include "RenderResource.h"
#include "RHI.h"
#include "RHIResources.h"
#include <atomic>

class UGaussianSplatComponent;
class UGaussianSplatAsset;
class FGaussianSplatRenderData;

struct FGaussianCapturedDepthShadowRenderData
{
	FTextureRHIRef DepthTexture;
	FMatrix44f WorldToLightView = FMatrix44f::Identity;
	FMatrix44f WorldToLightClip = FMatrix44f::Identity;
	FVector4f Params = FVector4f(10.0f, 3000.0f, 5.0f, 0.001f);
	FVector4f ReceiverBoundsMin = FVector4f(-HALF_WORLD_MAX, -HALF_WORLD_MAX, -HALF_WORLD_MAX, 0.0f);
	FVector4f ReceiverBoundsMax = FVector4f(HALF_WORLD_MAX, HALF_WORLD_MAX, HALF_WORLD_MAX, 0.0f);
	uint32 bEnabled = 0;
};


/**
 * GPU resources for Gaussian Splatting rendering
 * FBufferRHIRef: Raw GPU Memory
 * ShaderResourceViewRHIRef(SRV): interpret the memory into corresponding type, shader read only
 * UnorderedAccessViewRHIRef(UAV): interpret the memory into corresponding type, shader read/write
 */
class FGaussianSplatGPUResources : public FRenderResource
{
public:
	FGaussianSplatGPUResources();
	virtual ~FGaussianSplatGPUResources();

	/** Initialize resources from asset data */
	void Initialize(UGaussianSplatAsset* Asset);

	/** Provide the component-local proxy normal grid before Initialize/InitResource. */
	void SetProxyNormalGridData(const TArray<FFloat16Color>& InData, const FVector4f& InMinCellSize, const FVector4f& InDimensionsStatus)
	{
		PendingProxyNormalGridData = InData;
		ProxyNormalGridMinCellSize = InMinCellSize;
		ProxyNormalGridDimensionsStatus = InDimensionsStatus;
		ProxyNormalGridCount = static_cast<uint32>(InData.Num());
	}

	/** v2842: per-source-splat, scene-independent packed snow correspondence. */
	void SetPuddleDampenPackedData(const TArray<uint32>& InData)
	{
		PendingPuddleDampenPackedData = InData;
		PuddleDampenPackedCount = static_cast<uint32>(InData.Num());
	}

	void SetSnowCoveragePackedData(const TArray<uint32>& InData)
	{
		PendingSnowCoveragePackedData = InData;
		SnowCoveragePackedCount = static_cast<uint32>(InData.Num());
	}

	void SetSnowBakedFieldData2873(const TArray<uint32>& InData, float PatternCm, const FTransform& BakeTransform)
	{
		PendingSnowBakedFieldData2873 = InData;
		SnowBakedFieldCount2873 = static_cast<uint32>(InData.Num());
		SnowBakedFieldPatternCm2873 = PatternCm;
		SnowBakedFieldToWorld2873 = BakeTransform.ToMatrixWithScale();
	}

	/** Provide bounded Phase-1 stable bindings before Initialize/InitResource. */
	void SetStableSplatBindingData(TSharedPtr<const TArray<FStableProxySplatBindingGPU>, ESPMode::ThreadSafe> InData)
	{
		PendingStableSplatBindingData = MoveTemp(InData);
		StableSplatBindingCount = PendingStableSplatBindingData.IsValid()
			? static_cast<uint32>(PendingStableSplatBindingData->Num()) : 0u;
	}

	void UpdateStableSplatBindingData_RenderThread(
		FRHICommandListBase& RHICmdList,
		const TArray<FStableProxySplatBindingGPU>& InData);

	void SetStableProxySurfaceData(
		const TArray<FStableProxyVertexGPU>& InVertices,
		const TArray<FStableProxyTriangleGPU>& InTriangles,
		const TArray<FVector4f>& InTriangleVertexNormals)
	{
		PendingStableProxyVertexData = InVertices;
		PendingStableProxyTriangleData = InTriangles;
		PendingStableProxyTriangleVertexNormalData = InTriangleVertexNormals;
		StableProxyVertexCount = static_cast<uint32>(InVertices.Num());
		StableProxyTriangleCount = static_cast<uint32>(InTriangles.Num());
		StableProxyTriangleVertexNormalCount = static_cast<uint32>(InTriangleVertexNormals.Num());
	}

	/** Provide an already-completed deterministic BVH ray field before InitRHI. */
	void SetStableProxyRayProofData(const TArray<FVector4f>& InProofs)
	{
		PendingStableProxyRayProofData = InProofs;
	}

	void UpdateStableProxySurfaceData_RenderThread(
		FRHICommandListBase& RHICmdList,
		const TArray<FStableProxyVertexGPU>& InVertices,
		const TArray<FStableProxyTriangleGPU>& InTriangles,
		const TArray<FVector4f>& InTriangleVertexNormals);
	void UpdateStableProxyRayProofData_RenderThread(
		FRHICommandListImmediate& RHICmdList,
		const TArray<FVector4f>& InProofs);

	/** Check if resources are valid */
	bool IsValid() const { return bInitialized && SplatCount > 0 && PackedSplatBufferSRV.IsValid() && (RuntimePositionPrecision == 0u || Float32PositionBufferSRV.IsValid()); }

	/** Get number of splats */
	int32 GetSplatCount() const { return SplatCount; }

	//~ Begin FRenderResource Interface
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual void ReleaseRHI() override;
	//~ End FRenderResource Interface

public:
	/** Packed splat data buffer (legacy 16 bytes/splat: RGBA + FP16 position + compact rotation/scale). */
	FBufferRHIRef PackedSplatBuffer;
	FShaderResourceViewRHIRef PackedSplatBufferSRV;

	/** Full precision position data (raw float3, 12 bytes per splat). */
	FBufferRHIRef Float32PositionBuffer;
	FShaderResourceViewRHIRef Float32PositionBufferSRV;


	/** Old standalone position buffer declaration retained for source compatibility; not used. */
	FBufferRHIRef PositionBuffer;
	FShaderResourceViewRHIRef PositionBufferSRV;

	/** Rotation + Scale data buffer (legacy, unused when packed format is active) */
	FBufferRHIRef OtherDataBuffer;
	FShaderResourceViewRHIRef OtherDataBufferSRV;

	/** Spherical harmonics buffer */
	FBufferRHIRef SHBuffer;
	FShaderResourceViewRHIRef SHBufferSRV;

	/** Chunk info buffer */
	FBufferRHIRef ChunkBuffer;
	FShaderResourceViewRHIRef ChunkBufferSRV;

	/** View data buffer (computed per-frame) */
	FBufferRHIRef ViewDataBuffer;
	FUnorderedAccessViewRHIRef ViewDataBufferUAV;
	FShaderResourceViewRHIRef ViewDataBufferSRV;

	/** Sort distance buffer */
	FBufferRHIRef SortDistanceBuffer;
	FUnorderedAccessViewRHIRef SortDistanceBufferUAV;
	FShaderResourceViewRHIRef SortDistanceBufferSRV;

	/** Sort keys buffer (double buffered) */
	FBufferRHIRef SortKeysBuffer;
	FUnorderedAccessViewRHIRef SortKeysBufferUAV;
	FShaderResourceViewRHIRef SortKeysBufferSRV;

	FBufferRHIRef SortKeysBufferAlt;
	FUnorderedAccessViewRHIRef SortKeysBufferAltUAV;
	FShaderResourceViewRHIRef SortKeysBufferAltSRV;

	/** Sort distance buffer alt (for radix sort ping-pong) */
	FBufferRHIRef SortDistanceBufferAlt;
	FUnorderedAccessViewRHIRef SortDistanceBufferAltUAV;

	/** Radix sort histogram buffer */
	FBufferRHIRef RadixHistogramBuffer;
	FUnorderedAccessViewRHIRef RadixHistogramBufferUAV;

	/** Radix sort digit offset buffer */
	FBufferRHIRef RadixDigitOffsetBuffer;
	FUnorderedAccessViewRHIRef RadixDigitOffsetBufferUAV;

	/** Sort indirect dispatch args buffer (for indirect radix sort)
	 * Format: uint3 (NumTiles, 1, 1) - used by CountCS and ScatterCS
	 * Written by PrepareIndirectArgs, read by DispatchIndirectComputeShader
	 */
	FBufferRHIRef SortIndirectArgsBuffer;
	FUnorderedAccessViewRHIRef SortIndirectArgsBufferUAV;

	/** Sort parameters buffer (for indirect radix sort)
	 * Format: uint2 (SortCount, NumTiles)
	 * Written by PrepareIndirectArgs, read by radix sort shaders
	 */
	FBufferRHIRef SortParamsBuffer;
	FUnorderedAccessViewRHIRef SortParamsBufferUAV;
	FShaderResourceViewRHIRef SortParamsBufferSRV;

	/** Index buffer for quad rendering */
	FBufferRHIRef IndexBuffer;

	/** Color texture reference */
	FTextureRHIRef ColorTexture;
	FShaderResourceViewRHIRef ColorTextureSRV;

	/** Dummy white texture for fallback when ColorTexture isn't available */
	FTextureRHIRef DummyWhiteTexture;
	FShaderResourceViewRHIRef DummyWhiteTextureSRV;

	/** Get a valid ColorTexture SRV (returns dummy if real one not available) */
	FShaderResourceViewRHIRef GetColorTextureSRVOrDummy() const
	{
		return ColorTextureSRV.IsValid() ? ColorTextureSRV : DummyWhiteTextureSRV;
	}

	/** Position format used by this asset (Float32, Norm16, etc.) */
	EGaussianPositionFormat PositionFormat = EGaussianPositionFormat::Float32;

	/** Per-proxy A/B selection: 0 legacy packed FP16, 1 Float32 position buffer. */
	uint32 RuntimePositionPrecision = 1;
	bool ShouldUseFloat32Positions() const { return RuntimePositionPrecision == 1u && Float32PositionBufferSRV.IsValid(); }

	/** Per-proxy color source: 0 packed RGBA8, 1 existing PF_FloatRGBA texture. */
	uint32 RuntimeColorPrecision = 1;
	bool ShouldUseHighPrecisionColorOpacity() const { return ColorTexture.IsValid(); }
	FTextureRHIRef GetColorTextureOrDummy() const { return ColorTexture.IsValid() ? ColorTexture : DummyWhiteTexture; }

	/** Maximum entries allocated in the per-proxy compacted splat index list. */
	uint32 CompactionCapacity = 0;

	/** Per-proxy footprint selection and stabilized adaptive EWA controls. */
	uint32 RuntimeFootprintMode = 0;
	FVector4f FootprintParams0 = FVector4f(0.55f, 1.15f, 0.85f, 0.65f); // strength, near/mid/far minimum radius
	FVector4f FootprintParams1 = FVector4f(8.0f, 10.0f, 14.0f, 12.0f); // near/mid/far maximum radius, anisotropy
	FVector4f FootprintParams2 = FVector4f(2500.0f, 9000.0f, 0.18f, 1.25f); // start/full distance, opacity strength/max exponent
	FVector4f FootprintParams3 = FVector4f(0.125f, 0.35f, 0.0f, 0.0f); // far quantization, adaptive look safety


	//----------------------------------------------------------------------
	// Cluster culling resources (Nanite-style optimization)
	//----------------------------------------------------------------------

	/** Cluster data buffer (static, loaded from asset) */
	FBufferRHIRef ClusterBuffer;
	FShaderResourceViewRHIRef ClusterBufferSRV;

	/** Visible cluster indices buffer (written by culling shader) */
	FBufferRHIRef VisibleClusterBuffer;
	FUnorderedAccessViewRHIRef VisibleClusterBufferUAV;
	FShaderResourceViewRHIRef VisibleClusterBufferSRV;

	/** Visible cluster count buffer (atomic counter) */
	FBufferRHIRef VisibleClusterCountBuffer;
	FUnorderedAccessViewRHIRef VisibleClusterCountBufferUAV;
	FShaderResourceViewRHIRef VisibleClusterCountBufferSRV;

	/** Number of clusters */
	int32 ClusterCount = 0;

	/** Number of leaf clusters (for rendering) */
	int32 LeafClusterCount = 0;

	/** Whether cluster culling is available */
	bool bHasClusterData = false;

	/** Whether Nanite is enabled for this asset (per-asset setting) */
    // Render-thread cache, valid only for this resource generation and exact stereo pair.
    const void* PCVRIdentityBuffer=nullptr;
    uint32 PCVRIdentityCount=0;
    uint64 PCVRCullFrame=MAX_uint64;
    const void* PCVRCullFamily=nullptr;
    FMatrix PCVRCullLocal=FMatrix::Identity, PCVRCullLeft=FMatrix::Identity, PCVRCullRight=FMatrix::Identity;
    float PCVRCullError=0;
    int32 PCVRCullDebug=0;
    bool bPCVRCullLOD=false, bPCVRSharedCull=false, bPCVRCompactedShared=false;
    const void* PCVRCullBufferIdentity=nullptr;
	bool bEnableNanite = false;

	//----------------------------------------------------------------------
	// LOD splat resources (for parent cluster rendering)
	//----------------------------------------------------------------------

	/** LOD splat data buffer (static, loaded from asset) */
	FBufferRHIRef LODSplatBuffer;
	FShaderResourceViewRHIRef LODSplatBufferSRV;

	/** Number of LOD splats */
	int32 LODSplatCount = 0;

	/** Whether LOD splats are available */
	bool bHasLODSplats = false;

	//----------------------------------------------------------------------
	// Indirect draw resources (GPU-driven rendering)
	//----------------------------------------------------------------------

	/** Indirect draw argument buffer for GPU-driven rendering
	 * Structure: IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation
	 * Written by cluster culling, read by DrawIndexedPrimitiveIndirect
	 */
	FBufferRHIRef IndirectDrawArgsBuffer;
	FUnorderedAccessViewRHIRef IndirectDrawArgsBufferUAV;

	/** Whether indirect draw is available */
	bool bSupportsIndirectDraw = false;

	//----------------------------------------------------------------------
	// Cluster visibility integration resources
	//----------------------------------------------------------------------

	/** Shared spatial-grid low-frequency baked-light estimate. */

	/** Nanite-independent spatial de-lighting grid metadata copied from shared render data. */
	FVector4f SpatialLightingGridMinCellSize = FVector4f(0, 0, 0, 1);
	FVector4f SpatialLightingGridDimensionsStatus = FVector4f(0, 0, 0, 0);
	uint32 SpatialLightingCellCount = 0u;
	int32 SpatialLightingOccupiedCellCount = 0;
	float SpatialLightingCoverage = 0.0f;
	float SpatialLightingAverageConfidence = 0.0f;

	FBufferRHIRef ClusterLightingBuffer;
	FShaderResourceViewRHIRef ClusterLightingBufferSRV;

	/** Component-local proxy surface-normal grid, packed half4 (encoded normal RGB + confidence). */
	FBufferRHIRef ProxyNormalGridBuffer;
	FShaderResourceViewRHIRef ProxyNormalGridBufferSRV;
	FVector4f ProxyNormalGridMinCellSize = FVector4f(0, 0, 0, 1);
	FVector4f ProxyNormalGridDimensionsStatus = FVector4f(0, 0, 0, 0);
	uint32 ProxyNormalGridCount = 0u;

	/** Phase-1 splat -> proxy TriangleID + barycentric binding. */
	FBufferRHIRef PuddleDampenPackedBuffer;
	FShaderResourceViewRHIRef PuddleDampenPackedBufferSRV;
	uint32 PuddleDampenPackedCount = 0u;
	bool bPuddleMainSource = false;
	bool bPuddleWaterLayer = false;
	TArray<uint32> PendingPuddleDampenPackedData;
	FBufferRHIRef SnowCoveragePackedBuffer;
	FShaderResourceViewRHIRef SnowCoveragePackedBufferSRV;
	uint32 SnowCoveragePackedCount = 0u;
	TArray<uint32> PendingSnowCoveragePackedData;
	FBufferRHIRef SnowBakedFieldBuffer2873;
	FShaderResourceViewRHIRef SnowBakedFieldBufferSRV2873;
	uint32 SnowBakedFieldCount2873 = 0u;
	float SnowBakedFieldPatternCm2873 = 0.0f;
	FMatrix SnowBakedFieldToWorld2873 = FMatrix::Identity;
	TArray<uint32> PendingSnowBakedFieldData2873;

	FBufferRHIRef StableSplatBindingBuffer;
	FShaderResourceViewRHIRef StableSplatBindingBufferSRV;
	uint32 StableSplatBindingCount = 0u;
	uint32 StableSplatBindingCapacity = 0u;

	/** Phase-2 fixed proxy surface and one-shot vertex depth proof. */
	FBufferRHIRef StableProxyVertexBuffer;
	FShaderResourceViewRHIRef StableProxyVertexBufferSRV;
	uint32 StableProxyVertexCount = 0u;
	uint32 StableProxyVertexCapacity = 0u;
	FBufferRHIRef StableProxyTriangleBuffer;
	FShaderResourceViewRHIRef StableProxyTriangleBufferSRV;
	uint32 StableProxyTriangleCount = 0u;
	uint32 StableProxyTriangleCapacity = 0u;
	/** v2804: three authored local-space vertex normals per source triangle. */
	FBufferRHIRef StableProxyTriangleVertexNormalBuffer;
	FShaderResourceViewRHIRef StableProxyTriangleVertexNormalBufferSRV;
	uint32 StableProxyTriangleVertexNormalCount = 0u;
	uint32 StableProxyTriangleVertexNormalCapacity = 0u;
	// v2762 front/target pair. CalcViewData reads only the front buffer. A rebuild
	// writes the target buffer completely and swaps the pair only after the dispatch
	// and UAV->SRV transition have been queued.
	FBufferRHIRef StableProxyVertexProofBuffer;
	FShaderResourceViewRHIRef StableProxyVertexProofBufferSRV;
	FUnorderedAccessViewRHIRef StableProxyVertexProofBufferUAV;
	FBufferRHIRef StableProxyVertexProofTargetBuffer;
	FShaderResourceViewRHIRef StableProxyVertexProofTargetBufferSRV;
	FUnorderedAccessViewRHIRef StableProxyVertexProofTargetBufferUAV;
	FBufferRHIRef StableProxyVertexOwnerBuffer;
	FShaderResourceViewRHIRef StableProxyVertexOwnerBufferSRV;
	FUnorderedAccessViewRHIRef StableProxyVertexOwnerBufferUAV;
	FBufferRHIRef StableProxyVertexOwnerTargetBuffer;
	FShaderResourceViewRHIRef StableProxyVertexOwnerTargetBufferSRV;
	FUnorderedAccessViewRHIRef StableProxyVertexOwnerTargetBufferUAV;
	/** Actual triangle owning each Receiver SceneDepth texel (2048^2 uint = 16 MiB). */
	FBufferRHIRef StableProxyReceiverTriangleIdBuffer;
	FShaderResourceViewRHIRef StableProxyReceiverTriangleIdBufferSRV;
	FUnorderedAccessViewRHIRef StableProxyReceiverTriangleIdBufferUAV;
	// v2769: after each atomic swap Target contains the previous COMPLETE field A,
	// while Front contains the current COMPLETE field B. No partial field is sampled.
	uint32 StableProxyFieldBuildCount = 0u;
	uint32 StableProxyFieldPairValid = 0u;
	uint32 StableProxyLiveTargetValid = 0u;
	uint32 StableProxyFieldValid = 0u;
	// v2772 packet provenance for complete Front/Previous/LiveTarget fields.
	uint32 StableProxyFieldFrontCaptureRevision = 0u;
	uint32 StableProxyFieldPreviousCaptureRevision = 0u;
	uint32 StableProxyLiveTargetCaptureRevision = 0u;

	/** v2781 dense 2048x2048 camera-independent light-space cast visibility grid. */
	FBufferRHIRef StableProxyCastGridBuffer;
	FShaderResourceViewRHIRef StableProxyCastGridBufferSRV;
	FUnorderedAccessViewRHIRef StableProxyCastGridBufferUAV;
	uint32 StableProxyCastGridWidth = 2048u;
	uint32 StableProxyCastGridHeight = 2048u;
	uint32 StableProxyCastGridActiveWidth = 0u;
	uint32 StableProxyCastGridActiveHeight = 0u;
	uint32 StableProxyCastGridValid = 0u;

	/** Phase-4 six-sample per-triangle micro visibility field. */

	/** Maps each splat index to its cluster index
	 * Used by CalcViewData to check if splat's cluster is visible
	 */
	FBufferRHIRef SplatClusterIndexBuffer;
	FShaderResourceViewRHIRef SplatClusterIndexBufferSRV;

	/** Bitmap for cluster visibility (1 bit per cluster)
	 * Written by cluster culling, read by CalcViewData
	 * Size: ceil(ClusterCount / 32) uint32s
	 */
	FBufferRHIRef ClusterVisibilityBitmap;
	FUnorderedAccessViewRHIRef ClusterVisibilityBitmapUAV;
	FShaderResourceViewRHIRef ClusterVisibilityBitmapSRV;

	/** Selected cluster ID buffer for Nanite-style debug visualization
	 * Maps each leaf cluster to its selected cluster ID (may be parent if LOD is used)
	 * Written by cluster culling, read by CalcViewData
	 * Size: LeafClusterCount uint32s
	 */
	FBufferRHIRef SelectedClusterBuffer;
	FUnorderedAccessViewRHIRef SelectedClusterBufferUAV;
	FShaderResourceViewRHIRef SelectedClusterBufferSRV;

	//----------------------------------------------------------------------
	// LOD cluster selection output (for LOD rendering)
	//----------------------------------------------------------------------

	/** Buffer of unique parent cluster indices that need their LOD splats rendered
	 * Written by cluster culling, read by LOD rendering pass
	 * Size: ClusterCount uint32s (worst case all clusters could be selected)
	 */
	FBufferRHIRef LODClusterBuffer;
	FUnorderedAccessViewRHIRef LODClusterBufferUAV;
	FShaderResourceViewRHIRef LODClusterBufferSRV;

	/** Count of unique LOD clusters (atomic counter)
	 * Written by cluster culling, read back for LOD rendering dispatch
	 */
	FBufferRHIRef LODClusterCountBuffer;
	FUnorderedAccessViewRHIRef LODClusterCountBufferUAV;
	FShaderResourceViewRHIRef LODClusterCountBufferSRV;

	/** Bitmap to track which parent clusters have been claimed (1 bit per cluster)
	 * Used during cluster culling to ensure each parent is only added once
	 * Also read by GPU-driven LOD shader to check if cluster is selected
	 */
	FBufferRHIRef LODClusterSelectedBitmap;
	FUnorderedAccessViewRHIRef LODClusterSelectedBitmapUAV;
	FShaderResourceViewRHIRef LODClusterSelectedBitmapSRV;

	/** Total LOD splats to render (sum of all selected parent cluster LOD splat counts)
	 * Written by cluster culling, used for indirect draw args
	 */
	FBufferRHIRef LODSplatTotalBuffer;
	FUnorderedAccessViewRHIRef LODSplatTotalBufferUAV;
	FShaderResourceViewRHIRef LODSplatTotalBufferSRV;

	//----------------------------------------------------------------------
	// GPU-driven LOD rendering resources
	//----------------------------------------------------------------------

	/** Maps each LOD splat index to its owning cluster ID
	 * Used by GPU-driven LOD shader to check if LOD splat's cluster is selected
	 * Size: TotalLODSplats * sizeof(uint32)
	 */
	FBufferRHIRef LODSplatClusterIndexBuffer;
	FShaderResourceViewRHIRef LODSplatClusterIndexBufferSRV;

	/** Atomic counter for valid LOD splat output
	 * Written by CalcLODViewDataGPUDriven, read by UpdateDrawArgs
	 * Size: 4 bytes
	 */
	FBufferRHIRef LODSplatOutputCountBuffer;
	FUnorderedAccessViewRHIRef LODSplatOutputCountBufferUAV;
	FShaderResourceViewRHIRef LODSplatOutputCountBufferSRV;

	/** Total splat count for buffer allocation (SplatCount + TotalLODSplats) */
	int32 TotalSplatCount = 0;

	//----------------------------------------------------------------------
	// Splat compaction resources (GPU-driven work reduction)
	//----------------------------------------------------------------------

	/** Compacted list of visible splat indices
	 * Written by CompactSplats, read by CalcViewData (when using compaction)
	 * Size: TotalSplatCount * sizeof(uint32) (worst case all visible)
	 */
	FBufferRHIRef CompactedSplatIndicesBuffer;
	FUnorderedAccessViewRHIRef CompactedSplatIndicesBufferUAV;
	FShaderResourceViewRHIRef CompactedSplatIndicesBufferSRV;

	/** Atomic counter for visible splat count
	 * Written by CompactSplats, read by PrepareIndirectArgs
	 * Size: 4 bytes (single uint32)
	 */
	FBufferRHIRef VisibleSplatCountBuffer;
	FUnorderedAccessViewRHIRef VisibleSplatCountBufferUAV;
	FShaderResourceViewRHIRef VisibleSplatCountBufferSRV;

	/** Indirect dispatch arguments for compute shaders (CalcViewData, CalcDistances)
	 * Written by PrepareIndirectArgs, read by DispatchComputeIndirect
	 * Format: uint3 (numGroupsX, numGroupsY, numGroupsZ)
	 * Size: 12 bytes (3 uint32)
	 */
	FBufferRHIRef IndirectDispatchArgsBuffer;
	FUnorderedAccessViewRHIRef IndirectDispatchArgsBufferUAV;


	/** Whether splat compaction is enabled and buffers are available */
	bool bSupportsCompaction = false;

	/** Get position format as uint for shader */
	uint32 GetPositionFormatUint() const { return static_cast<uint32>(PositionFormat); }

	/** Get number of SH bands stored in SHBuffer (0 = no SH data) */
	int32 GetSHBands() const { return SHBands; }

private:
	/** Create dummy white texture for fallback */
	void CreateDummyWhiteTexture(FRHICommandListBase& RHICmdList);

	/** Create per-instance cluster/compaction/sort buffers */
	void CreatePerInstanceBuffers(FRHICommandListBase& RHICmdList);

private:
	/** CPU staging data for the component-local proxy normal grid. */
	TArray<FFloat16Color> PendingProxyNormalGridData;

	/** CPU staging data for the stable binding and welded proxy surface. */
	TSharedPtr<const TArray<FStableProxySplatBindingGPU>, ESPMode::ThreadSafe> PendingStableSplatBindingData;
	TArray<FStableProxyVertexGPU> PendingStableProxyVertexData;
	TArray<FStableProxyTriangleGPU> PendingStableProxyTriangleData;
	TArray<FVector4f> PendingStableProxyTriangleVertexNormalData;
	TArray<FVector4f> PendingStableProxyRayProofData;

	/** Shared render data (holds shared GPU buffers, ref-counted) */
	TSharedPtr<FGaussianSplatRenderData> SharedData;

	int32 SplatCount = 0;
	int32 SHBands = 0;  // Number of SH bands stored in SHBuffer (0 = no SH data)
	bool bInitialized = false;

public:
	/** Cached state for camera-static sort skipping */
	FMatrix CachedViewProjectionMatrix = FMatrix::Identity;
	FMatrix CachedLocalToWorld = FMatrix::Identity;
	float CachedOpacityScale = -1.0f;
	float CachedSplatScale = -1.0f;
	float CachedErrorThreshold = -1.0f;
	int32 CachedDebugMode = -1;
	int32 CachedDebugForceLODLevel = -1;
	bool bHasCachedSortData = false;
};

/**
 * Scene proxy for rendering Gaussian Splatting
 */
class FGaussianSplatSceneProxy : public FPrimitiveSceneProxy
{
public:
	FGaussianSplatSceneProxy(const UGaussianSplatComponent* InComponent);
	virtual ~FGaussianSplatSceneProxy();

	//~ Begin FPrimitiveSceneProxy Interface
	virtual SIZE_T GetTypeHash() const override;
	virtual uint32 GetMemoryFootprint() const override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;

	virtual void GetDynamicMeshElements(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override;

	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;
	virtual void DestroyRenderThreadResources() override;
#if WITH_EDITOR
	virtual HHitProxy* CreateHitProxies(UPrimitiveComponent* Component, TArray<TRefCountPtr<HHitProxy>>& OutHitProxies) override;
#endif
	//~ End FPrimitiveSceneProxy Interface

	/** Get GPU resources (may return nullptr if pending destruction) */
	FGaussianSplatGPUResources* GetGPUResources() const { return GPUResources; }

	/** Try to initialize color texture SRV if not already done */
	void TryInitializeColorTexture(FRHICommandListBase& RHICmdList);

	/** Get splat count */
	int32 GetSplatCount() const { return SplatCount; }
	uint32 GetStableBindingRevision_RenderThread() const { return StableBindingRevisionValue; }

	/** Get rendering parameters */
	int32 GetSHOrder() const { return SHOrder; }
	float GetOpacityScale() const { return OpacityScale; }
	float GetSplatScale() const { return SplatScale; }
	float GetEffectiveSplatScale() const { return SplatScale; }
	float GetLODErrorThreshold() const { return LODErrorThreshold; }
	float GetEffectiveLODErrorThreshold() const { return (RenderProfileValue == 3 || RenderProfileValue == 4) ? FMath::Min(LODErrorThreshold, 0.006f) : LODErrorThreshold; }
	int32 GetRenderProfileValue() const { return RenderProfileValue; }
	int32 GetLightingPreviewMode() const { return LightingPreviewMode; }
	int32 GetRuntimePositionPrecisionValue() const { return RuntimePositionPrecisionValue; }
	int32 GetRuntimeColorPrecisionValue() const { return RuntimeColorPrecisionValue; }
	const FGaussianCapturedDepthShadowRenderData& GetCapturedDepthShadowData() const { return CapturedDepthShadowData; }
	// High Quality changes resolve quality, not sort validity. Only the explicit
	// switch should force a full radix sort every frame.
	bool ShouldForceFullSortEveryFrame() const { return bForceFullSortEveryFrame; }
	bool ShouldDisableLODInReference() const { return (bDisableLODInReference && RenderProfileValue == 2) || RenderProfileValue == 3 || RenderProfileValue == 4; }

	/** Lighting payload collected from Unreal LightComponents for the cinematic splat shader. */
	const FGaussianSplatLightingData& GetLightingData() const { return LightingData; }

	/** Proxy mesh triangles used to build the light-space GS shadow map. */

	/** Render-thread update for dynamic light data. */
	void UpdateLightingData_RenderThread(const FGaussianSplatLightingData& InLightingData)
	{
		LightingData = InLightingData;
	}


	void UpdateStableSplatBindings_RenderThread(
		FRHICommandListImmediate& RHICmdList,
		const TArray<FStableProxySplatBindingGPU>& InBindings,
		const TArray<FStableProxyVertexGPU>& InVertices,
		const TArray<FStableProxyTriangleGPU>& InTriangles,
		const TArray<FVector4f>& InTriangleVertexNormals,
		uint32 InRevision);
	void UpdateStableProxyRayProofs_RenderThread(
		FRHICommandListImmediate& RHICmdList,
		const TArray<FVector4f>& InProofs,
		uint32 InRevision);

	void RequestStableProxyFieldBuild_RenderThread(uint32 InRequestRevision, uint32 InExpectedLightDepthRevision, float InDepthBiasCm)
	{
		StableFieldRequestedRevisionRT = InRequestRevision;
		StableFieldExpectedLightDepthRevisionRT = InExpectedLightDepthRevision;
		StableFieldDepthBiasCmRT = InDepthBiasCm;
	}
	void EnsureStableProxyField_RenderThread(FRHICommandListImmediate& RHICmdList, const FMatrix& InLocalToWorld);
	uint32 GetStableFieldRevision_RenderThread() const { return StableFieldCompletedRevisionRT; }
	uint32 GetStableFieldRequestRevision_RenderThread() const { return StableFieldRequestedRevisionRT; }

	void UpdateCapturedDepthShadowData_RenderThread(const FGaussianCapturedDepthShadowRenderData& InData)
	{
		CapturedDepthShadowData = InData;
	}

	/** Check if this proxy is safe to use for rendering.
	 *  Returns false if proxy is being destroyed or has invalid resources.
	 *  Thread-safe - can be called from render thread.
	 */
	bool IsValidForRendering() const
	{
		// Check destruction flag first (atomic, no lock needed)
		if (bPendingDestruction.load(std::memory_order_acquire))
		{
			return false;
		}
		// Then check resources
		return GPUResources != nullptr && GPUResources->IsValid();
	}

	/** Mark this proxy as pending destruction. Called at start of DestroyRenderThreadResources. */
	void MarkPendingDestruction()
	{
		bPendingDestruction.store(true, std::memory_order_release);
	}

private:
	/** Atomic flag indicating this proxy is being destroyed.
	 *  Set at the start of DestroyRenderThreadResources to prevent
	 *  render commands from accessing resources during/after destruction.
	 */
	std::atomic<bool> bPendingDestruction{false};


	/** GPU resources */
	FGaussianSplatGPUResources* GPUResources = nullptr;

	/** Cached asset for initialization */
	UGaussianSplatAsset* CachedAsset = nullptr;

	/** Rendering parameters */
	int32 SplatCount = 0;
	int32 SHOrder = 3;
	float OpacityScale = 1.0f;
	float SplatScale = 1.0f;
	float LODErrorThreshold = 0.03f;
	int32 RenderProfileValue = 0;
	int32 LightingPreviewMode = 0;
	int32 RuntimePositionPrecisionValue = 1;
	int32 RuntimeColorPrecisionValue = 1;
	int32 RuntimeFootprintModeValue = 0;
	FVector4f RuntimeFootprintParams0 = FVector4f(0.55f, 1.15f, 0.85f, 0.65f);
	FVector4f RuntimeFootprintParams1 = FVector4f(8.0f, 10.0f, 14.0f, 12.0f);
	FVector4f RuntimeFootprintParams2 = FVector4f(2500.0f, 9000.0f, 0.18f, 1.25f);
	FVector4f RuntimeFootprintParams3 = FVector4f(0.125f, 0.35f, 0.0f, 0.0f);
	TArray<FFloat16Color> ProxyNormalGridData;
	FVector4f ProxyNormalGridMinCellSize = FVector4f(0, 0, 0, 1);
	FVector4f ProxyNormalGridDimensionsStatus = FVector4f(0, 0, 0, 0);
	TSharedPtr<const TArray<FStableProxySplatBindingGPU>, ESPMode::ThreadSafe> StableSplatBindingData;
	TArray<uint32> PuddleDampenPackedData;
	bool bPuddleProxyMainSource = false;
	bool bPuddleProxyWaterLayer = false;
	TArray<uint32> SnowCoveragePackedData;
	TArray<uint32> SnowBakedFieldPackedData2873;
	float SnowBakedFieldPatternCm2873 = 0.0f;
	FTransform SnowBakedFieldTransform2873 = FTransform::Identity;

	TArray<FStableProxyVertexGPU> StableProxyVertexData;
	TArray<FStableProxyTriangleGPU> StableProxyTriangleData;
	TArray<FVector4f> StableProxyTriangleVertexNormalData;
	// v2801 Panorama/MRQ bootstrap: a newly-created SceneProxy must carry the
	// already-completed deterministic BVH field on its very first render. The
	// component-level LastSubmitted revision belongs to the previous SceneProxy and
	// therefore cannot be used as proof that this proxy has received the payload.
	TArray<FVector4f> StableProxyRayProofData;
	uint32 StableRayProofRevisionValue = 0u;
	uint32 StableBindingRevisionValue = 0u;
	uint32 StableFieldRequestedRevisionRT = 0u;
	uint32 StableFieldCompletedRevisionRT = 0u;
	uint32 StableFieldExpectedLightDepthRevisionRT = 0u;
	uint32 StableFieldCompletedLightDepthRevisionRT = 0u;
	float StableFieldDepthBiasCmRT = 0.0f;
	// Once deterministic CPU BVH visibility is uploaded, the legacy LightDepth
	// builder must not overwrite the same proof buffers later in the frame.
	bool bStableProxyRayFieldActiveRT = false;
	bool bStableProxyRayFieldSkipLoggedRT = false;
	FGaussianCapturedDepthShadowRenderData CapturedDepthShadowData;
	bool bForceFullSortEveryFrame = false;
	bool bDisableLODInReference = true;
	FGaussianSplatLightingData LightingData;


#if WITH_EDITOR
	/** Cached hit proxy created in CreateHitProxies, used for editor viewport click selection. */
	HHitProxy* SelectionHitProxy = nullptr;
#endif
};
