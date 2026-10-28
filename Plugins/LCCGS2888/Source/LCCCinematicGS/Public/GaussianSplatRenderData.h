// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianDataTypes.h"
#include "GaussianClusterTypes.h"
#include "RHI.h"
#include "RHIResources.h"

class UGaussianSplatAsset;

/**
 * Shared render data for a Gaussian Splat asset.
 * Holds CPU-side cached data and shared GPU buffers that are created once
 * per asset and shared across all FGaussianSplatGPUResources instances
 * referencing the same asset.
 */
class FGaussianSplatRenderData
{
public:
	FGaussianSplatRenderData();
	~FGaussianSplatRenderData();

	/** Initialize CPU-side data from asset. Only runs once (guarded by bIsInitialized). */
	void Initialize(UGaussianSplatAsset* Asset);

	/** Create shared GPU buffers. Thread-safe, only runs once. Must be called on render thread. */
	void CreateGPUBuffers(FRHICommandListBase& RHICmdList);

	/** Release shared GPU buffers. */
	void ReleaseGPUBuffers();

	/** Whether CPU-side data has been initialized */
	bool IsInitialized() const { return bIsInitialized; }

	/** Whether GPU buffers have been created */
	bool AreGPUBuffersCreated() const { return bGPUBuffersCreated; }

	/** Get asset name for logging */
	const FString& GetAssetName() const { return AssetName; }

public:
	// ---- Shared GPU buffers (created once, shared across all proxies) ----

	/** Packed splat data buffer (16 bytes/splat) */
	FBufferRHIRef PackedSplatBuffer;
	FShaderResourceViewRHIRef PackedSplatBufferSRV;

	/** Full precision local positions stored as tightly packed float3 (12 bytes/splat). */
	FBufferRHIRef Float32PositionBuffer;
	FShaderResourceViewRHIRef Float32PositionBufferSRV;


	/** Spherical harmonics buffer */
	FBufferRHIRef SHBuffer;
	FShaderResourceViewRHIRef SHBufferSRV;

	/** Chunk info buffer */
	FBufferRHIRef ChunkBuffer;
	FShaderResourceViewRHIRef ChunkBufferSRV;

	/** Index buffer for quad rendering */
	FBufferRHIRef IndexBuffer;

	/** Cluster data buffer (static, loaded from asset) */
	FBufferRHIRef ClusterBuffer;
	FShaderResourceViewRHIRef ClusterBufferSRV;

	/** Splat-to-cluster index buffer (static, loaded from asset) */
	FBufferRHIRef SplatClusterIndexBuffer;
	FShaderResourceViewRHIRef SplatClusterIndexBufferSRV;

	/** Nanite-independent spatial-grid low-frequency baked-light estimate, packed as half4 (RGB factor + confidence). */
	FBufferRHIRef ClusterLightingBuffer;
	FShaderResourceViewRHIRef ClusterLightingBufferSRV;

	// ---- Metadata ----

	int32 SplatCount = 0;
	int32 SHBands = 0;
	int32 ClusterCount = 0;
	int32 LeafClusterCount = 0;
	int32 LODSplatCount = 0;

	/** Spatial de-lighting grid metadata. Status: 0 unknown, 1 valid, 2 missing color, 3 invalid positions/bounds, 4 no mapped samples. */
	FVector3f SpatialLightingGridMin = FVector3f::ZeroVector;
	FIntVector SpatialLightingGridDimensions = FIntVector::ZeroValue;
	float SpatialLightingCellSize = 1.0f;
	uint32 SpatialLightingCacheStatus = 0u;
	int32 SpatialLightingCellCount = 0;
	int32 SpatialLightingOccupiedCellCount = 0;
	float SpatialLightingCoverage = 0.0f;
	float SpatialLightingAverageConfidence = 0.0f;

	bool bEnableNanite = false;
	bool bHasClusterData = false;
	bool bHasLODSplats = false;
	EGaussianPositionFormat PositionFormat = EGaussianPositionFormat::Float32;

private:
	// ---- CPU-side cached data (freed after GPU upload) ----

	TArray<uint8> PackedSplatData;
	TArray<FVector3f> Float32PositionData;
	TArray<uint8> SHData;
	TArray<FGaussianChunkInfo> CachedChunkData;
	TArray<FGaussianGPUCluster> CachedClusterData;
	TArray<uint32> CachedSplatClusterIndices;
	TArray<FFloat16Color> CachedClusterLightingData; // Dense spatial-grid cells; name retained for shader/source compatibility.

	bool bIsInitialized = false;
	bool bGPUBuffersCreated = false;
	FString AssetName;
	FCriticalSection InitLock;
	FCriticalSection GPUInitLock;
};
