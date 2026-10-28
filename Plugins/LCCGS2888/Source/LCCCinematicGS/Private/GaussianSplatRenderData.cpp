// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatRenderData.h"
#include "GaussianSplatAsset.h"
#include "RHICommandList.h"

namespace
{
	static float DecodeSRGBChannelToLinear(float Value)
	{
		Value = FMath::Max(Value, 0.0f);
		return Value <= 0.04045f
			? Value / 12.92f
			: FMath::Pow((Value + 0.055f) / 1.055f, 2.4f);
	}

	static FVector3f DecodeAssetColorToLinear(const FFloat16Color& Pixel)
	{
		return FVector3f(
			DecodeSRGBChannelToLinear(Pixel.R.GetFloat()),
			DecodeSRGBChannelToLinear(Pixel.G.GetFloat()),
			DecodeSRGBChannelToLinear(Pixel.B.GetFloat()));
	}

	static float LinearLuminance(const FVector3f& C)
	{
		return FMath::Max(C.X * 0.2126f + C.Y * 0.7152f + C.Z * 0.0722f, 1.0e-5f);
	}

	static FVector3f NormalizeTint(const FVector3f& C)
	{
		return C / LinearLuminance(C);
	}
}

FGaussianSplatRenderData::FGaussianSplatRenderData()
{
}

FGaussianSplatRenderData::~FGaussianSplatRenderData()
{
	ReleaseGPUBuffers();
}

void FGaussianSplatRenderData::Initialize(UGaussianSplatAsset* Asset)
{
	FScopeLock Lock(&InitLock);

	if (bIsInitialized)
	{
		return;
	}

	if (!Asset || !Asset->IsValid())
	{
		return;
	}

	AssetName = Asset->GetName();
	SplatCount = Asset->GetSplatCount();
	PositionFormat = Asset->PositionFormat;

	// Keep the source Float16 color texture available through cluster-stat generation.
	TArray<uint8> RawColorTextureData;

	// --- Pack splat data (16 bytes/splat) ---
	{
		TArray<uint8> RawPositionData;
		TArray<uint8> RawOtherData;
		Asset->GetPositionData(RawPositionData);
		Asset->GetOtherData(RawOtherData);
		Asset->GetColorTextureData(RawColorTextureData);

		const int64 RequiredPositionBytes = static_cast<int64>(SplatCount) * 3 * sizeof(float);
		const int64 RequiredOtherBytes = static_cast<int64>(SplatCount) * 7 * sizeof(float);
		if (RawPositionData.Num() < RequiredPositionBytes || RawOtherData.Num() < RequiredOtherBytes)
		{
			UE_LOG(LogTemp, Error, TEXT("GaussianSplatRenderData: Asset '%s' has incomplete runtime bulk data (Position=%d/%lld, Other=%d/%lld)."),
				*AssetName, RawPositionData.Num(), static_cast<long long>(RequiredPositionBytes), RawOtherData.Num(), static_cast<long long>(RequiredOtherBytes));
			return;
		}

		const int32 ColorTexWidth = Asset->ColorTextureWidth;
		const int32 ColorTexHeight = Asset->ColorTextureHeight;
		const FFloat16Color* ColorPixels = nullptr;
		bool bHasColor = (RawColorTextureData.Num() > 0 && ColorTexWidth > 0 && ColorTexHeight > 0);
		if (bHasColor)
		{
			ColorPixels = reinterpret_cast<const FFloat16Color*>(RawColorTextureData.GetData());
		}

		PackedSplatData.SetNumUninitialized(SplatCount * GaussianSplattingConstants::PackedSplatStride);
		Float32PositionData.SetNumUninitialized(SplatCount);
		uint32* PackedPtr = reinterpret_cast<uint32*>(PackedSplatData.GetData());
		const float* PosFloats = reinterpret_cast<const float*>(RawPositionData.GetData());
		const float* OtherFloats = reinterpret_cast<const float*>(RawOtherData.GetData());

		for (int32 i = 0; i < SplatCount; i++)
		{
			FVector3f Position(
				PosFloats[i * 3 + 0],
				PosFloats[i * 3 + 1],
				PosFloats[i * 3 + 2]);

			Float32PositionData[i] = Position;

			const float* OtherBase = OtherFloats + i * 7;
			FQuat4f Rotation(OtherBase[0], OtherBase[1], OtherBase[2], OtherBase[3]);
			FVector3f Scale(OtherBase[4], OtherBase[5], OtherBase[6]);


			float ColorR = 1.0f, ColorG = 1.0f, ColorB = 1.0f, Opacity = 1.0f;
			if (bHasColor)
			{
				int32 TexX, TexY;
				GaussianSplattingUtils::SplatIndexToTextureCoord(i, ColorTexWidth, TexX, TexY);
				if (TexY < ColorTexHeight)
				{
					const FFloat16Color& Pixel = ColorPixels[TexY * ColorTexWidth + TexX];
					ColorR = FMath::Clamp(Pixel.R.GetFloat(), 0.0f, 1.0f);
					ColorG = FMath::Clamp(Pixel.G.GetFloat(), 0.0f, 1.0f);
					ColorB = FMath::Clamp(Pixel.B.GetFloat(), 0.0f, 1.0f);
					Opacity = FMath::Clamp(Pixel.A.GetFloat(), 0.0f, 1.0f);
				}
			}

			GaussianSplattingUtils::PackSplatToUint4(
				Position, Rotation, Scale,
				ColorR, ColorG, ColorB, Opacity,
				&PackedPtr[i * 4]);
		}
	}

	// Load chunk data
	CachedChunkData = Asset->ChunkData;

	// Load SH data
	if (Asset->SHBands > 0)
	{
		Asset->GetSHData(SHData);
		SHBands = Asset->SHBands;
	}

	// Load Nanite cluster data
	bEnableNanite = Asset->IsNaniteEnabled();

	if (bEnableNanite && Asset->HasClusterHierarchy())
	{
		const FGaussianClusterHierarchy& Hierarchy = Asset->GetClusterHierarchy();
		Hierarchy.ToGPUClusters(CachedClusterData);
		ClusterCount = CachedClusterData.Num();
		LeafClusterCount = Hierarchy.NumLeafClusters;
		bHasClusterData = true;

		const uint32 OriginalSplatCount = Hierarchy.TotalSplatCount;
		LODSplatCount = Hierarchy.TotalLODSplatCount;
		bHasLODSplats = (LODSplatCount > 0);

		// Build splat-to-cluster index mapping
		CachedSplatClusterIndices.SetNumZeroed(SplatCount);

		for (int32 ClusterIdx = 0; ClusterIdx < Hierarchy.Clusters.Num(); ++ClusterIdx)
		{
			const FGaussianCluster& Cluster = Hierarchy.Clusters[ClusterIdx];
			if (Cluster.IsLeaf())
			{
				for (uint32 i = 0; i < Cluster.SplatCount; ++i)
				{
					uint32 SplatIdx = Cluster.SplatStartIndex + i;
					if (SplatIdx < OriginalSplatCount)
					{
						CachedSplatClusterIndices[SplatIdx] = ClusterIdx;
					}
				}
			}
		}

		for (int32 ClusterIdx = 0; ClusterIdx < Hierarchy.Clusters.Num(); ++ClusterIdx)
		{
			const FGaussianCluster& Cluster = Hierarchy.Clusters[ClusterIdx];
			if (!Cluster.IsLeaf() && Cluster.LODSplatCount > 0)
			{
				for (uint32 i = 0; i < Cluster.LODSplatCount; ++i)
				{
					uint32 SplatIdx = Cluster.LODSplatStartIndex + i;
					if (SplatIdx < static_cast<uint32>(SplatCount))
					{
						CachedSplatClusterIndices[SplatIdx] = ClusterIdx;
					}
				}
			}
		}
	}
	else
	{
		bHasClusterData = false;
		ClusterCount = 0;
		LeafClusterCount = 0;
		bHasLODSplats = false;
		LODSplatCount = 0;
	}

	// Build a Nanite-independent, dense spatial-grid low-frequency illumination field.
	// Every original splat maps to a cell from its local Float32 position in the shader,
	// so this path works with or without the render/LOD cluster hierarchy and requires no
	// additional splat-to-cell index buffer. Lighting Foundation 1.2 uses a finer
	// material-aware grid capped at 524,288 cells (4 MiB as half4), generated once
	// per shared asset render data instance.
	CachedClusterLightingData.Reset();
	SpatialLightingGridMin = FVector3f::ZeroVector;
	SpatialLightingGridDimensions = FIntVector::ZeroValue;
	SpatialLightingCellSize = 1.0f;
	SpatialLightingCacheStatus = 0u;
	SpatialLightingCellCount = 0;
	SpatialLightingOccupiedCellCount = 0;
	SpatialLightingCoverage = 0.0f;
	SpatialLightingAverageConfidence = 0.0f;

	const int32 ColorTexWidth = Asset->ColorTextureWidth;
	const int32 ColorTexHeight = Asset->ColorTextureHeight;
	const int64 RequiredColorBytes = static_cast<int64>(ColorTexWidth) * ColorTexHeight * sizeof(FFloat16Color);
	const bool bHasSpatialColorData = RawColorTextureData.Num() >= RequiredColorBytes && ColorTexWidth > 0 && ColorTexHeight > 0;
	const int32 OriginalSplatCountForGrid = FMath::Min(
		FMath::Min(Asset->GetOriginalSplatCount(), SplatCount),
		Float32PositionData.Num());

	if (!bHasSpatialColorData)
	{
		SpatialLightingCacheStatus = 2u; // Missing color data.
		CachedClusterLightingData.Add(FFloat16Color(FLinearColor(1.0f, 1.0f, 1.0f, 0.0f)));
		UE_LOG(LogTemp, Warning, TEXT("GaussianSplatRenderData: Spatial de-lighting cache disabled for '%s': missing/invalid Float16 color data."), *AssetName);
	}
	else if (OriginalSplatCountForGrid <= 0)
	{
		SpatialLightingCacheStatus = 3u; // Invalid positions/count.
		CachedClusterLightingData.Add(FFloat16Color(FLinearColor(1.0f, 1.0f, 1.0f, 0.0f)));
		UE_LOG(LogTemp, Warning, TEXT("GaussianSplatRenderData: Spatial de-lighting cache disabled for '%s': no valid original splat positions."), *AssetName);
	}
	else
	{
		FVector3f GridBoundsMin(TNumericLimits<float>::Max(), TNumericLimits<float>::Max(), TNumericLimits<float>::Max());
		FVector3f GridBoundsMax(-TNumericLimits<float>::Max(), -TNumericLimits<float>::Max(), -TNumericLimits<float>::Max());
		int32 FinitePositionCount = 0;
		for (int32 SplatIndex = 0; SplatIndex < OriginalSplatCountForGrid; ++SplatIndex)
		{
			const FVector3f& P = Float32PositionData[SplatIndex];
			if (!FMath::IsFinite(P.X) || !FMath::IsFinite(P.Y) || !FMath::IsFinite(P.Z))
			{
				continue;
			}
			GridBoundsMin.X = FMath::Min(GridBoundsMin.X, P.X);
			GridBoundsMin.Y = FMath::Min(GridBoundsMin.Y, P.Y);
			GridBoundsMin.Z = FMath::Min(GridBoundsMin.Z, P.Z);
			GridBoundsMax.X = FMath::Max(GridBoundsMax.X, P.X);
			GridBoundsMax.Y = FMath::Max(GridBoundsMax.Y, P.Y);
			GridBoundsMax.Z = FMath::Max(GridBoundsMax.Z, P.Z);
			++FinitePositionCount;
		}

		const FVector3f GridExtent = GridBoundsMax - GridBoundsMin;
		const float MaxExtent = FMath::Max3(GridExtent.X, GridExtent.Y, GridExtent.Z);
		if (FinitePositionCount <= 0 || !FMath::IsFinite(MaxExtent) || MaxExtent <= 1.0e-4f)
		{
			SpatialLightingCacheStatus = 3u;
			CachedClusterLightingData.Add(FFloat16Color(FLinearColor(1.0f, 1.0f, 1.0f, 0.0f)));
			UE_LOG(LogTemp, Warning, TEXT("GaussianSplatRenderData: Spatial de-lighting cache disabled for '%s': invalid local bounds."), *AssetName);
		}
		else
		{
			// Lighting Foundation 1.2: use a finer local grid so walls, foliage and ground
			// are less likely to share one illumination estimate. The dense half4 cache
			// remains bounded to 4 MiB at the maximum cell count.
			constexpr int32 TargetLongestAxisCells = 80;
			constexpr int32 MaxAxisCells = 128;
			constexpr int64 MaxGridCellCount = 524288;
			float CellSize = FMath::Max(MaxExtent / static_cast<float>(TargetLongestAxisCells), 1.0e-3f);

			auto ComputeGridDimensions = [&GridExtent, MaxAxisCells](float InCellSize)
			{
				return FIntVector(
					FMath::Clamp(FMath::CeilToInt(GridExtent.X / InCellSize) + 2, 1, MaxAxisCells),
					FMath::Clamp(FMath::CeilToInt(GridExtent.Y / InCellSize) + 2, 1, MaxAxisCells),
					FMath::Clamp(FMath::CeilToInt(GridExtent.Z / InCellSize) + 2, 1, MaxAxisCells));
			};

			FIntVector GridDims = ComputeGridDimensions(CellSize);
			auto GridCellCount64 = [&GridDims]() -> int64
			{
				return static_cast<int64>(GridDims.X) * GridDims.Y * GridDims.Z;
			};
			while (GridCellCount64() > MaxGridCellCount)
			{
				CellSize *= 1.18f;
				GridDims = ComputeGridDimensions(CellSize);
			}

			const int32 GridCellCount = static_cast<int32>(GridCellCount64());
			const FVector3f GridMin = GridBoundsMin - FVector3f(CellSize * 0.5f, CellSize * 0.5f, CellSize * 0.5f);
			const FFloat16Color* ColorPixels = reinterpret_cast<const FFloat16Color*>(RawColorTextureData.GetData());

			auto FlattenCell = [&GridDims](int32 X, int32 Y, int32 Z) -> int32
			{
				return X + GridDims.X * (Y + GridDims.Y * Z);
			};
			auto PositionToCell = [&GridMin, CellSize, &GridDims](const FVector3f& P, FIntVector& OutCell) -> bool
			{
				const FVector3f Relative = (P - GridMin) / CellSize;
				OutCell = FIntVector(
					FMath::FloorToInt(Relative.X),
					FMath::FloorToInt(Relative.Y),
					FMath::FloorToInt(Relative.Z));
				return OutCell.X >= 0 && OutCell.Y >= 0 && OutCell.Z >= 0 &&
					OutCell.X < GridDims.X && OutCell.Y < GridDims.Y && OutCell.Z < GridDims.Z;
			};

			TArray<FVector3f> SumRGB;
			TArray<double> SumLogL;
			TArray<double> SumLogL2;
			TArray<double> SumWeight;
			SumRGB.SetNumZeroed(GridCellCount);
			SumLogL.SetNumZeroed(GridCellCount);
			SumLogL2.SetNumZeroed(GridCellCount);
			SumWeight.SetNumZeroed(GridCellCount);

			int32 MappedSplatCount = 0;
			for (int32 SplatIndex = 0; SplatIndex < OriginalSplatCountForGrid; ++SplatIndex)
			{
				FIntVector Cell;
				if (!PositionToCell(Float32PositionData[SplatIndex], Cell))
				{
					continue;
				}

				int32 TexX = 0, TexY = 0;
				GaussianSplattingUtils::SplatIndexToTextureCoord(SplatIndex, ColorTexWidth, TexX, TexY);
				if (TexX < 0 || TexY < 0 || TexX >= ColorTexWidth || TexY >= ColorTexHeight)
				{
					continue;
				}

				const FFloat16Color& Pixel = ColorPixels[TexY * ColorTexWidth + TexX];
				const float Opacity = FMath::Clamp(Pixel.A.GetFloat(), 0.0f, 1.0f);
				const float Weight = FMath::Max(Opacity, 0.02f);
				const FVector3f LinearColor = DecodeAssetColorToLinear(Pixel);
				const float LogL = FMath::Loge(LinearLuminance(LinearColor));
				const int32 CellIndex = FlattenCell(Cell.X, Cell.Y, Cell.Z);

				SumRGB[CellIndex] += LinearColor * Weight;
				SumLogL[CellIndex] += static_cast<double>(LogL * Weight);
				SumLogL2[CellIndex] += static_cast<double>(LogL * LogL * Weight);
				SumWeight[CellIndex] += Weight;
				++MappedSplatCount;
			}

			TArray<int32> OccupiedCells;
			TArray<float> MeanLogL;
			TArray<FVector3f> MeanTint;
			MeanLogL.SetNumZeroed(GridCellCount);
			MeanTint.SetNumZeroed(GridCellCount);
			OccupiedCells.Reserve(FMath::Min(GridCellCount, MappedSplatCount));
			TArray<float> OccupiedLogLuminances;

			for (int32 CellIndex = 0; CellIndex < GridCellCount; ++CellIndex)
			{
				if (SumWeight[CellIndex] <= 0.0)
				{
					continue;
				}
				const double Weight = SumWeight[CellIndex];
				MeanLogL[CellIndex] = static_cast<float>(SumLogL[CellIndex] / Weight);
				MeanTint[CellIndex] = NormalizeTint(SumRGB[CellIndex] / static_cast<float>(Weight));
				OccupiedCells.Add(CellIndex);
				OccupiedLogLuminances.Add(MeanLogL[CellIndex]);
			}

			if (MappedSplatCount <= 0 || OccupiedCells.Num() <= 0)
			{
				SpatialLightingCacheStatus = 4u;
				CachedClusterLightingData.Add(FFloat16Color(FLinearColor(1.0f, 1.0f, 1.0f, 0.0f)));
				UE_LOG(LogTemp, Warning, TEXT("GaussianSplatRenderData: Spatial de-lighting cache disabled for '%s': no splats mapped into the spatial grid."), *AssetName);
			}
			else
			{
				OccupiedLogLuminances.Sort();
				const float GlobalLogReference = OccupiedLogLuminances[OccupiedLogLuminances.Num() / 2];
				CachedClusterLightingData.Init(FFloat16Color(FLinearColor(1.0f, 1.0f, 1.0f, 0.0f)), GridCellCount);

				double ConfidenceSum = 0.0;
				for (const int32 CellIndex : OccupiedCells)
				{
					const int32 CellX = CellIndex % GridDims.X;
					const int32 CellYZ = CellIndex / GridDims.X;
					const int32 CellY = CellYZ % GridDims.Y;
					const int32 CellZ = CellYZ / GridDims.Y;

					double SmoothedLogSum = 0.0;
					FVector3f SmoothedTintSum = FVector3f::ZeroVector;
					double SmoothWeight = 0.0;
					int32 SupportingCells = 0;

					for (int32 DZ = -2; DZ <= 2; ++DZ)
					for (int32 DY = -2; DY <= 2; ++DY)
					for (int32 DX = -2; DX <= 2; ++DX)
					{
						const int32 NX = CellX + DX;
						const int32 NY = CellY + DY;
						const int32 NZ = CellZ + DZ;
						if (NX < 0 || NY < 0 || NZ < 0 || NX >= GridDims.X || NY >= GridDims.Y || NZ >= GridDims.Z)
						{
							continue;
						}
						const int32 NeighborIndex = FlattenCell(NX, NY, NZ);
						if (SumWeight[NeighborIndex] <= 0.0)
						{
							continue;
						}

						const float DistanceSq = static_cast<float>(DX * DX + DY * DY + DZ * DZ);
						const float SpatialWeight = FMath::Exp(-DistanceSq * 0.42f);
						const FVector3f TintDelta = MeanTint[CellIndex] - MeanTint[NeighborIndex];
						const float TintSimilarity = FMath::Exp(-FMath::Clamp(TintDelta.SizeSquared(), 0.0f, 12.0f) * 8.0f);
						const float MaterialEdgeWeight = 0.015f + 0.985f * TintSimilarity;
						const float SampleSupport = FMath::Clamp(FMath::Sqrt(static_cast<float>(SumWeight[NeighborIndex]) / 32.0f), 0.35f, 1.0f);
						const double NeighborWeight = static_cast<double>(SpatialWeight * MaterialEdgeWeight * SampleSupport);

						SmoothedLogSum += static_cast<double>(MeanLogL[NeighborIndex]) * NeighborWeight;
						SmoothedTintSum += MeanTint[NeighborIndex] * static_cast<float>(NeighborWeight);
						SmoothWeight += NeighborWeight;
						++SupportingCells;
					}

					const float SmoothedLogL = SmoothWeight > 1.0e-6
						? static_cast<float>(SmoothedLogSum / SmoothWeight)
						: MeanLogL[CellIndex];
					FVector3f SmoothedTint = SmoothWeight > 1.0e-6
						? NormalizeTint(SmoothedTintSum / static_cast<float>(SmoothWeight))
						: MeanTint[CellIndex];

					// A small reference pull keeps material base brightness from being interpreted
					// entirely as illumination while preserving broad spatial light gradients.
					const float ReferenceBalancedLogL = FMath::Lerp(SmoothedLogL, GlobalLogReference, 0.25f);
					float RelativeLuminance = FMath::Exp(ReferenceBalancedLogL - GlobalLogReference);
					RelativeLuminance = FMath::Clamp(RelativeLuminance, 0.55f, 1.75f);

					SmoothedTint.X = FMath::Clamp(SmoothedTint.X, 0.75f, 1.30f);
					SmoothedTint.Y = FMath::Clamp(SmoothedTint.Y, 0.75f, 1.30f);
					SmoothedTint.Z = FMath::Clamp(SmoothedTint.Z, 0.75f, 1.30f);
					SmoothedTint = NormalizeTint(SmoothedTint);

					const double CellWeight = FMath::Max(SumWeight[CellIndex], 1.0e-6);
					const double CellMeanLog = SumLogL[CellIndex] / CellWeight;
					const double Variance = FMath::Max(SumLogL2[CellIndex] / CellWeight - CellMeanLog * CellMeanLog, 0.0);
					const float VarianceConfidence = FMath::Exp(-static_cast<float>(Variance) * 0.55f);
					const float SampleConfidence = FMath::Clamp(FMath::Sqrt(static_cast<float>(CellWeight) / 64.0f), 0.0f, 1.0f);
					const float NeighbourConfidence = FMath::Clamp(static_cast<float>(SupportingCells) / 16.0f, 0.0f, 1.0f);
					const float Confidence = FMath::Clamp(VarianceConfidence * SampleConfidence * NeighbourConfidence, 0.0f, 1.0f);
					const FVector3f LightingRGB = SmoothedTint * RelativeLuminance;

					CachedClusterLightingData[CellIndex] = FFloat16Color(FLinearColor(
						LightingRGB.X, LightingRGB.Y, LightingRGB.Z, Confidence));
					ConfidenceSum += Confidence;
				}

				SpatialLightingGridMin = GridMin;
				SpatialLightingGridDimensions = GridDims;
				SpatialLightingCellSize = CellSize;
				SpatialLightingCacheStatus = 1u;
				SpatialLightingCellCount = GridCellCount;
				SpatialLightingOccupiedCellCount = OccupiedCells.Num();
				SpatialLightingCoverage = static_cast<float>(MappedSplatCount) / static_cast<float>(FMath::Max(OriginalSplatCountForGrid, 1));
				SpatialLightingAverageConfidence = static_cast<float>(ConfidenceSum / static_cast<double>(OccupiedCells.Num()));

				UE_LOG(LogTemp, Log, TEXT("GaussianSplatRenderData: Built Nanite-independent spatial de-lighting grid for '%s': Grid=%dx%dx%d, CellSize=%.3f, Occupied=%d/%d, Coverage=%.2f%%, AvgConfidence=%.3f, Cache=%.2f MiB"),
					*AssetName,
					GridDims.X, GridDims.Y, GridDims.Z,
					CellSize,
					SpatialLightingOccupiedCellCount,
					SpatialLightingCellCount,
					SpatialLightingCoverage * 100.0f,
					SpatialLightingAverageConfidence,
					double(CachedClusterLightingData.Num() * sizeof(FFloat16Color)) / (1024.0 * 1024.0));
			}
		}
	}

	bIsInitialized = true;

	UE_LOG(LogTemp, Verbose, TEXT("GaussianSplatRenderData: Created shared CPU data for asset '%s' (%d splats)"),
		*AssetName, SplatCount);
}

void FGaussianSplatRenderData::CreateGPUBuffers(FRHICommandListBase& RHICmdList)
{
	FScopeLock Lock(&GPUInitLock);

	if (bGPUBuffersCreated)
	{
		return;
	}

	int32 SharedBufferCount = 0;

	// --- Full precision position buffer (tightly packed Float32 xyz = 12 bytes/splat) ---
	{
		static_assert(sizeof(FVector3f) == 12, "FVector3f must remain a tightly packed float3 for the raw position buffer");
		const uint32 PositionCount = FMath::Max(Float32PositionData.Num(), 1);
		const uint32 PositionBufferSize = PositionCount * sizeof(FVector3f);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianFloat32PositionBuffer"),
			PositionBufferSize,
			0,
			BUF_Static | BUF_ShaderResource | BUF_ByteAddressBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		Float32PositionBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(Float32PositionBuffer, 0, PositionBufferSize, RLM_WriteOnly);
		if (Float32PositionData.Num() > 0)
		{
			FMemory::Memcpy(Data, Float32PositionData.GetData(), Float32PositionData.Num() * sizeof(FVector3f));
		}
		else
		{
			FMemory::Memzero(Data, PositionBufferSize);
		}
		RHICmdList.UnlockBuffer(Float32PositionBuffer);

		Float32PositionBufferSRV = RHICmdList.CreateShaderResourceView(
			Float32PositionBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Raw));
		SharedBufferCount++;
	}

	// --- Packed splat buffer (legacy attributes + legacy FP16 position) ---
	if (PackedSplatData.Num() > 0)
	{
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianPackedSplatBuffer"),
			PackedSplatData.Num(),
			0,
			BUF_Static | BUF_ShaderResource | BUF_ByteAddressBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		PackedSplatBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(PackedSplatBuffer, 0, PackedSplatData.Num(), RLM_WriteOnly);
		FMemory::Memcpy(Data, PackedSplatData.GetData(), PackedSplatData.Num());
		RHICmdList.UnlockBuffer(PackedSplatBuffer);

		PackedSplatBufferSRV = RHICmdList.CreateShaderResourceView(
			PackedSplatBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Raw));
		SharedBufferCount++;
	}

	// --- SH buffer (always create at least a dummy for shader binding) ---
	{
		uint32 SHDataSize = SHData.Num();
		if (SHDataSize == 0)
		{
			SHDataSize = 16;
		}

		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianSHBuffer"),
			SHDataSize,
			0,
			BUF_Static | BUF_ShaderResource | BUF_ByteAddressBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		SHBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(SHBuffer, 0, SHDataSize, RLM_WriteOnly);
		if (SHData.Num() > 0)
		{
			FMemory::Memcpy(Data, SHData.GetData(), SHData.Num());
		}
		else
		{
			FMemory::Memzero(Data, SHDataSize);
		}
		RHICmdList.UnlockBuffer(SHBuffer);

		SHBufferSRV = RHICmdList.CreateShaderResourceView(
			SHBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Raw));
		SharedBufferCount++;
	}

	// --- Chunk buffer (always create at least a dummy for shader binding) ---
	{
		uint32 ChunkCount = CachedChunkData.Num();
		if (ChunkCount == 0)
		{
			ChunkCount = 1;
		}

		const uint32 ChunkSize = ChunkCount * sizeof(FGaussianChunkInfo);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianChunkBuffer"),
			ChunkSize,
			sizeof(FGaussianChunkInfo),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		ChunkBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(ChunkBuffer, 0, ChunkSize, RLM_WriteOnly);
		if (CachedChunkData.Num() > 0)
		{
			FMemory::Memcpy(Data, CachedChunkData.GetData(), ChunkSize);
		}
		else
		{
			FMemory::Memzero(Data, ChunkSize);
		}
		RHICmdList.UnlockBuffer(ChunkBuffer);

		ChunkBufferSRV = RHICmdList.CreateShaderResourceView(
			ChunkBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(FGaussianChunkInfo)));
		SharedBufferCount++;
	}

	// --- Index buffer (6 indices per quad) ---
	{
		TArray<uint16> Indices = { 0, 1, 2, 1, 3, 2 };

		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianSplatIndexBuffer"),
			Indices.Num() * sizeof(uint16),
			sizeof(uint16),
			BUF_Static | BUF_IndexBuffer)
			.SetInitialState(ERHIAccess::VertexOrIndexBuffer);
		IndexBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(IndexBuffer, 0, Indices.Num() * sizeof(uint16), RLM_WriteOnly);
		FMemory::Memcpy(Data, Indices.GetData(), Indices.Num() * sizeof(uint16));
		RHICmdList.UnlockBuffer(IndexBuffer);
		SharedBufferCount++;
	}

	// --- Cluster buffer (static, from asset) ---
	if (bHasClusterData && CachedClusterData.Num() > 0)
	{
		const uint32 BufferSize = CachedClusterData.Num() * sizeof(FGaussianGPUCluster);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianClusterBuffer"),
			BufferSize,
			sizeof(FGaussianGPUCluster),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		ClusterBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(ClusterBuffer, 0, BufferSize, RLM_WriteOnly);
		FMemory::Memcpy(Data, CachedClusterData.GetData(), BufferSize);
		RHICmdList.UnlockBuffer(ClusterBuffer);

		ClusterBufferSRV = RHICmdList.CreateShaderResourceView(
			ClusterBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(FGaussianGPUCluster)));
		SharedBufferCount++;
	}

	// --- Splat-to-cluster index buffer (static, from asset; or dummy for non-Nanite) ---
	{
		const uint32 BufferSize = CachedSplatClusterIndices.Num() > 0
			? CachedSplatClusterIndices.Num() * sizeof(uint32)
			: sizeof(uint32);  // dummy 1-element buffer for shader binding

		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianSplatClusterIndexBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		SplatClusterIndexBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(SplatClusterIndexBuffer, 0, BufferSize, RLM_WriteOnly);
		if (CachedSplatClusterIndices.Num() > 0)
		{
			FMemory::Memcpy(Data, CachedSplatClusterIndices.GetData(), BufferSize);
		}
		else
		{
			FMemory::Memzero(Data, BufferSize);
		}
		RHICmdList.UnlockBuffer(SplatClusterIndexBuffer);

		SplatClusterIndexBufferSRV = RHICmdList.CreateShaderResourceView(
			SplatClusterIndexBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		SharedBufferCount++;
	}

	// --- Cluster-cached spatial de-lighting buffer (half4 = RGB lighting factor + confidence) ---
	{
		const uint32 EntryCount = FMath::Max(CachedClusterLightingData.Num(), 1);
		const uint32 BufferSize = EntryCount * sizeof(FFloat16Color);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianClusterLightingBuffer"),
			BufferSize,
			sizeof(FFloat16Color),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		ClusterLightingBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(ClusterLightingBuffer, 0, BufferSize, RLM_WriteOnly);
		if (CachedClusterLightingData.Num() > 0)
		{
			FMemory::Memcpy(Data, CachedClusterLightingData.GetData(), CachedClusterLightingData.Num() * sizeof(FFloat16Color));
		}
		else
		{
			FMemory::Memzero(Data, BufferSize);
		}
		RHICmdList.UnlockBuffer(ClusterLightingBuffer);
		ClusterLightingBufferSRV = RHICmdList.CreateShaderResourceView(
			ClusterLightingBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(FFloat16Color)));
		SharedBufferCount++;
	}

	// Free CPU-side cached data after GPU upload
	PackedSplatData.Empty();
	Float32PositionData.Empty();
	SHData.Empty();
	CachedChunkData.Empty();
	CachedClusterData.Empty();
	CachedSplatClusterIndices.Empty();
	CachedClusterLightingData.Empty();

	bGPUBuffersCreated = true;

	const double StaticPositionMiB = (double(SplatCount) * sizeof(FVector3f)) / (1024.0 * 1024.0);
	const double PackedAttributeMiB = (double(SplatCount) * GaussianSplattingConstants::PackedSplatStride) / (1024.0 * 1024.0);
	UE_LOG(LogTemp, Log, TEXT("GaussianSplatRenderData: Created %d shared GPU buffers for asset '%s' (Float32 positions %.1f MiB, packed attributes %.1f MiB)"),
		SharedBufferCount, *AssetName, StaticPositionMiB, PackedAttributeMiB);
}

void FGaussianSplatRenderData::ReleaseGPUBuffers()
{
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
	bGPUBuffersCreated = false;
}
