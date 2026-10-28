// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianGlobalAccumulator.h"
#include "GaussianDataTypes.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "HAL/IConsoleManager.h"

extern TAutoConsoleVariable<int32> CVarGSPanoramaGPUTrace;

void FGaussianGlobalAccumulator::ResizeIfNeeded(FRHICommandListBase& RHICmdList, uint32 NewTotalCount)
{
	// Correctness fix: allocate for the complete source working set. A hard splat
	// budget cannot safely truncate this buffer until cluster-priority selection is
	// implemented, because compacted indices are not ordered by camera distance.

	const uint32 MinimumUsefulCount = FMath::Max(NewTotalCount, 4096u);
	const bool bNeedsGrowth = MinimumUsefulCount > AllocatedCount;
	// Release oversized buffers when the required source working set remains far
	// below a previous peak. This preserves the Memory Foundation shrink behavior
	// without dropping visible splats.
	const bool bNeedsShrink = AllocatedCount > 0u &&
		uint64(AllocatedCount) > uint64(MinimumUsefulCount) * 2ull;
	if (!bNeedsGrowth && !bNeedsShrink)
	{
		return;
	}

	// Allocate the exact complete source count. The former ten-percent headroom cost
	// roughly 203 MiB for the 21.96M-splat production asset even though its source
	// count is immutable during ordinary rendering. Growth is still handled safely
	// when the visible proxy set really changes.
	uint32 NewAllocatedCount = FMath::Max(NewTotalCount, 4096u);
	uint32 NewNumTiles = FMath::DivideAndRoundUp(NewAllocatedCount, 1024u);
	const uint64 ViewDataBytes = uint64(NewAllocatedCount) * sizeof(FGaussianSplatViewData);
	const uint64 HistogramBytes = uint64(NewNumTiles) * 256ull * sizeof(uint32);
	if (ViewDataBytes > uint64(MAX_uint32) || HistogramBytes > uint64(MAX_uint32))
	{
		UE_LOG(LogTemp, Error,
			TEXT("[GSPanoramaGPU][Abort] Global buffer byte count exceeds 32-bit RHI allocation size: Count=%u ViewDataBytes=%llu HistogramBytes=%llu"),
			NewAllocatedCount, static_cast<unsigned long long>(ViewDataBytes),
			static_cast<unsigned long long>(HistogramBytes));
		return; // Caller verifies allocated capacity before submitting any compute.
	}

	const double EstimatedMiB = (double(NewAllocatedCount) * (sizeof(FGaussianSplatViewData) + 4.0 * sizeof(uint32)) +
		double(256u * NewNumTiles + 258u) * sizeof(uint32)) / (1024.0 * 1024.0);
	UE_LOG(LogTemp, Log, TEXT("GaussianGlobalAccumulator: Resizing from %u to %u splats (%u tiles, estimated %.1f MiB working set)"),
		AllocatedCount, NewAllocatedCount, NewNumTiles, EstimatedMiB);
	if (CVarGSPanoramaGPUTrace.GetValueOnRenderThread() > 0)
	{
		UE_LOG(LogTemp, Display,
			TEXT("[GSPanoramaGPU][RT][Resize] GenNext=%llu OldCapacity=%u NewCapacity=%u ViewStride=%u ViewBytes=%llu HistogramBytes=%llu"),
			static_cast<unsigned long long>(BufferGeneration + 1ull), AllocatedCount,
			NewAllocatedCount, uint32(sizeof(FGaussianSplatViewData)),
			static_cast<unsigned long long>(ViewDataBytes),
			static_cast<unsigned long long>(HistogramBytes));
	}

	// Release old buffers before reallocation
	Release();

	const uint32 UintStride = sizeof(uint32);
	const uint32 ViewDataStride = sizeof(FGaussianSplatViewData);

	// --- ViewData buffer ---
	{
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GlobalViewDataBuffer"),
			NewAllocatedCount * ViewDataStride,
			ViewDataStride,
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		GlobalViewDataBuffer = RHICmdList.CreateBuffer(Desc);
		GlobalViewDataBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalViewDataBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(ViewDataStride));
		GlobalViewDataBufferSRV = RHICmdList.CreateShaderResourceView(
			GlobalViewDataBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(ViewDataStride));
	}

	// --- Sort distance buffer (primary) ---
	{
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GlobalSortDistanceBuffer"),
			NewAllocatedCount * UintStride,
			UintStride,
			BUF_UnorderedAccess | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		GlobalSortDistanceBuffer = RHICmdList.CreateBuffer(Desc);
		GlobalSortDistanceBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalSortDistanceBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- Sort distance buffer (alt) ---
	{
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GlobalSortDistanceBufferAlt"),
			NewAllocatedCount * UintStride,
			UintStride,
			BUF_UnorderedAccess | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		GlobalSortDistanceBufferAlt = RHICmdList.CreateBuffer(Desc);
		GlobalSortDistanceBufferAltUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalSortDistanceBufferAlt, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- Sort keys buffer (primary) ---
	{
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GlobalSortKeysBuffer"),
			NewAllocatedCount * UintStride,
			UintStride,
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		GlobalSortKeysBuffer = RHICmdList.CreateBuffer(Desc);
		GlobalSortKeysBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalSortKeysBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
		GlobalSortKeysBufferSRV = RHICmdList.CreateShaderResourceView(
			GlobalSortKeysBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- Sort keys buffer (alt) ---
	{
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GlobalSortKeysBufferAlt"),
			NewAllocatedCount * UintStride,
			UintStride,
			BUF_UnorderedAccess | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		GlobalSortKeysBufferAlt = RHICmdList.CreateBuffer(Desc);
		GlobalSortKeysBufferAltUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalSortKeysBufferAlt, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- Radix histogram: 256 * NumTiles ---
	{
		uint32 HistogramCount = 256 * NewNumTiles;
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GlobalRadixHistogramBuffer"),
			HistogramCount * UintStride,
			UintStride,
			BUF_UnorderedAccess | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		GlobalRadixHistogramBuffer = RHICmdList.CreateBuffer(Desc);
		GlobalRadixHistogramBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalRadixHistogramBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- Radix digit offset: 256 entries ---
	{
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GlobalRadixDigitOffsetBuffer"),
			256 * UintStride,
			UintStride,
			BUF_UnorderedAccess | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		GlobalRadixDigitOffsetBuffer = RHICmdList.CreateBuffer(Desc);
		GlobalRadixDigitOffsetBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalRadixDigitOffsetBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- SortParams: 2 uints — SRV for non-compaction sort, UAV written by PrefixSumCS ---
	{
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GlobalSortParamsBuffer"),
			2 * UintStride,
			UintStride,
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		GlobalSortParamsBuffer = RHICmdList.CreateBuffer(Desc);
		GlobalSortParamsBufferSRV = RHICmdList.CreateShaderResourceView(
			GlobalSortParamsBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
		GlobalSortParamsBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalSortParamsBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}


	AllocatedCount = NewAllocatedCount;
	AllocatedNumTiles = NewNumTiles;
	++BufferGeneration;

	// Invalidate cache since buffers changed
	bHasCachedSortData = false;
}

void FGaussianGlobalAccumulator::EnsureCompactionBuffersAllocated(FRHICommandListBase& RHICmdList)
{
	if (bCompactionBuffersAllocated)
	{
		return;
	}

	UE_LOG(LogTemp, Verbose, TEXT("GaussianGlobalAccumulator: Allocating compaction prefix-sum buffers (MaxProxies=%u)"), MAX_PROXY_COUNT);

	const uint32 UintStride = sizeof(uint32);

	// --- GlobalVisibleCountArray: [MAX_PROXY_COUNT] uints ---
	{
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GlobalVisibleCountArrayBuffer"),
			MAX_PROXY_COUNT * UintStride,
			UintStride,
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		GlobalVisibleCountArrayBuffer = RHICmdList.CreateBuffer(Desc);
		GlobalVisibleCountArrayBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalVisibleCountArrayBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
		GlobalVisibleCountArrayBufferSRV = RHICmdList.CreateShaderResourceView(
			GlobalVisibleCountArrayBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- GlobalBaseOffsets: [MAX_PROXY_COUNT + 1] uints ---
	{
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GlobalBaseOffsetsBuffer"),
			(MAX_PROXY_COUNT + 1) * UintStride,
			UintStride,
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		GlobalBaseOffsetsBuffer = RHICmdList.CreateBuffer(Desc);
		GlobalBaseOffsetsBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalBaseOffsetsBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
		GlobalBaseOffsetsBufferSRV = RHICmdList.CreateShaderResourceView(
			GlobalBaseOffsetsBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- GlobalCalcDistIndirectArgs: [3] uints ---
	{
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GlobalCalcDistIndirectArgsBuffer"),
			3 * UintStride,
			UintStride,
			BUF_UnorderedAccess | BUF_DrawIndirect | BUF_StructuredBuffer | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVCompute);
		GlobalCalcDistIndirectArgsBuffer = RHICmdList.CreateBuffer(Desc);
		GlobalCalcDistIndirectArgsBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalCalcDistIndirectArgsBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- GlobalSortIndirectArgsGlobal: [3] uints ---
	{
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GlobalSortIndirectArgsGlobalBuffer"),
			3 * UintStride,
			UintStride,
			BUF_UnorderedAccess | BUF_DrawIndirect | BUF_StructuredBuffer | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVCompute);
		GlobalSortIndirectArgsGlobalBuffer = RHICmdList.CreateBuffer(Desc);
		GlobalSortIndirectArgsGlobalBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalSortIndirectArgsGlobalBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	// --- GlobalDrawIndirectArgs: [5] uints ---
	{
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GlobalDrawIndirectArgsBuffer"),
			5 * UintStride,
			UintStride,
			BUF_UnorderedAccess | BUF_DrawIndirect | BUF_StructuredBuffer | BUF_ShaderResource)
			.SetInitialState(ERHIAccess::UAVCompute);
		GlobalDrawIndirectArgsBuffer = RHICmdList.CreateBuffer(Desc);
        GlobalDrawIndirectArgsBufferSRV = RHICmdList.CreateShaderResourceView(GlobalDrawIndirectArgsBuffer, FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(UintStride));
		GlobalDrawIndirectArgsBufferUAV = RHICmdList.CreateUnorderedAccessView(
			GlobalDrawIndirectArgsBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(UintStride));
	}

	bCompactionBuffersAllocated = true;
}

void FGaussianGlobalAccumulator::Release()
{
	GlobalViewDataBuffer.SafeRelease();
	GlobalViewDataBufferUAV.SafeRelease();
	GlobalViewDataBufferSRV.SafeRelease();

	GlobalSortDistanceBuffer.SafeRelease();
	GlobalSortDistanceBufferUAV.SafeRelease();
	GlobalSortDistanceBufferAlt.SafeRelease();
	GlobalSortDistanceBufferAltUAV.SafeRelease();

	GlobalSortKeysBuffer.SafeRelease();
	GlobalSortKeysBufferUAV.SafeRelease();
	GlobalSortKeysBufferSRV.SafeRelease();
	GlobalSortKeysBufferAlt.SafeRelease();
	GlobalSortKeysBufferAltUAV.SafeRelease();

	GlobalRadixHistogramBuffer.SafeRelease();
	GlobalRadixHistogramBufferUAV.SafeRelease();

	GlobalRadixDigitOffsetBuffer.SafeRelease();
	GlobalRadixDigitOffsetBufferUAV.SafeRelease();

	GlobalSortParamsBuffer.SafeRelease();
	GlobalSortParamsBufferSRV.SafeRelease();
	GlobalSortParamsBufferUAV.SafeRelease();


	// Compaction prefix-sum buffers
	GlobalVisibleCountArrayBuffer.SafeRelease();
	GlobalVisibleCountArrayBufferUAV.SafeRelease();
	GlobalVisibleCountArrayBufferSRV.SafeRelease();

	GlobalBaseOffsetsBuffer.SafeRelease();
	GlobalBaseOffsetsBufferUAV.SafeRelease();
	GlobalBaseOffsetsBufferSRV.SafeRelease();

	GlobalCalcDistIndirectArgsBuffer.SafeRelease();
	GlobalCalcDistIndirectArgsBufferUAV.SafeRelease();

	GlobalSortIndirectArgsGlobalBuffer.SafeRelease();
	GlobalSortIndirectArgsGlobalBufferUAV.SafeRelease();

	GlobalDrawIndirectArgsBuffer.SafeRelease();
 GlobalDrawIndirectArgsBufferSRV.SafeRelease();
	GlobalDrawIndirectArgsBufferUAV.SafeRelease();

    TileResources.Reset();
    PCVRFinalColorSRV.SafeRelease(); PCVRFinalColorUAV.SafeRelease(); PCVRFinalColorBuffer.SafeRelease();
    PCVRFinalColorCapacity=0;
    PCVRBlockBuffer.SafeRelease(); PCVRBlockUAV.SafeRelease();
    PCVRBlockCapacity=0; bPCVRPreSortCompacted=false;
	AllocatedCount = 0;
	AllocatedNumTiles = 0;
	bHasCachedSortData = false;
	CachedTotalSplatCount = 0;
	CachedProxySetHash = 0;
	CachedLightingHash = 0;
	CachedViewProjectionMatrix = FMatrix::Identity;
	LastCacheDecision = 255;
	bCompactionBuffersAllocated = false;
}

void FGaussianGlobalAccumulator::EnsurePCVRBlocks(FRHICommandListBase& RHICmdList,uint32 Count)
{
    if(PCVRBlockCapacity>=Count && PCVRBlockBuffer.IsValid()) return;
    auto Desc=FRHIBufferCreateDesc::Create(TEXT("GSPCVRStableBlocks"),2*Count*sizeof(uint32),sizeof(uint32),BUF_UnorderedAccess|BUF_ShaderResource|BUF_StructuredBuffer).SetInitialState(ERHIAccess::UAVCompute);
    PCVRBlockBuffer=RHICmdList.CreateBuffer(Desc);
    PCVRBlockUAV=RHICmdList.CreateUnorderedAccessView(PCVRBlockBuffer,FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
    PCVRBlockCapacity=Count;
}

void FGaussianGlobalAccumulator::EnsurePCVRFinalColors(FRHICommandListBase& Cmd,uint32 Count)
{
    if (!Count || uint64(Count)*sizeof(FVector4f)>MAX_uint32) return;
    if (PCVRFinalColorCapacity>=Count && PCVRFinalColorBuffer.IsValid()) return;
    auto Desc=FRHIBufferCreateDesc::Create(TEXT("GSPCVRFinalColors"),Count*sizeof(FVector4f),sizeof(FVector4f),
        BUF_UnorderedAccess|BUF_ShaderResource|BUF_StructuredBuffer).SetInitialState(ERHIAccess::UAVCompute);
    PCVRFinalColorBuffer=Cmd.CreateBuffer(Desc);
    PCVRFinalColorUAV=Cmd.CreateUnorderedAccessView(PCVRFinalColorBuffer,FRHIViewDesc::CreateBufferUAV()
        .SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(FVector4f)));
    PCVRFinalColorSRV=Cmd.CreateShaderResourceView(PCVRFinalColorBuffer,FRHIViewDesc::CreateBufferSRV()
        .SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(FVector4f)));
    PCVRFinalColorCapacity=Count;
}
