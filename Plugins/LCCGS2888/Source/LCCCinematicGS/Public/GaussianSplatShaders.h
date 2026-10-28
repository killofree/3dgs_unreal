// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderPermutation.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphBuilder.h"
#include "SceneView.h"
#include "GaussianDataTypes.h"
#include "GaussianClusterTypes.h"

/**
 * Compute shader for calculating view-dependent data for each Gaussian splat
 * This runs per-frame to transform splats to screen space and evaluate spherical harmonics
 */
class FGaussianSplatCalcViewDataCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatCalcViewDataCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatCalcViewDataCS, FGlobalShader);

	class FVRBaseline : SHADER_PERMUTATION_BOOL("GS_VR_BASELINE");
	using FPermutationDomain = TShaderPermutationDomain<FVRBaseline>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(ByteAddressBuffer, PackedSplatBuffer)  // 16 bytes/splat packed data
		SHADER_PARAMETER_SRV(ByteAddressBuffer, Float32PositionBuffer) // tightly packed float3 (12 bytes/splat)
		SHADER_PARAMETER(uint32, UseFloat32Position)
		SHADER_PARAMETER_TEXTURE(Texture2D, HighPrecisionColorTexture) // existing PF_FloatRGBA asset texture
		SHADER_PARAMETER(uint32, UseHighPrecisionColorOpacity)
		SHADER_PARAMETER(uint32, UseAdaptiveFootprint)
		SHADER_PARAMETER(FVector4f, FootprintParams0)
		SHADER_PARAMETER(FVector4f, FootprintParams1)
		SHADER_PARAMETER(FVector4f, FootprintParams2)
		SHADER_PARAMETER(FVector4f, FootprintParams3)
		SHADER_PARAMETER_SRV(ByteAddressBuffer, SHBuffer)           // View-dependent spherical-harmonic color data
		SHADER_PARAMETER_UAV(RWStructuredBuffer<FGaussianSplatViewData>, ViewDataBuffer)
		// Cluster visibility integration (UNIFIED APPROACH)
		// SplatClusterIndexBuffer maps ALL splats to their cluster (original->leaf, LOD->parent)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, SplatClusterIndexBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, ClusterVisibilityBitmap)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, LODClusterSelectedBitmap)  // For LOD splat visibility
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, SelectedClusterBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint2>, ClusterLightingBuffer)
		SHADER_PARAMETER(uint32, ClusterLightingCount)
		SHADER_PARAMETER(FVector4f, SpatialLightingGridMinCellSize)
		SHADER_PARAMETER(FVector4f, SpatialLightingGridDimensionsStatus)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint2>, ProxyNormalGridBuffer)
		SHADER_PARAMETER(uint32, ProxyNormalGridCount)
		SHADER_PARAMETER(FVector4f, ProxyNormalGridMinCellSize)
		SHADER_PARAMETER(FVector4f, ProxyNormalGridDimensionsStatus)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint32>, SnowCoveragePackedBuffer)
		SHADER_PARAMETER(uint32, SnowCoveragePackedCount)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint32>, PuddleDampenPackedMask)
		SHADER_PARAMETER(uint32, PuddleDampenPackedCount)
		SHADER_PARAMETER(float, PuddleDampenStrength)
		SHADER_PARAMETER(FVector2f, PuddleSourceWetSettings)
		SHADER_PARAMETER(uint32, PuddleWaterLayer)
		SHADER_PARAMETER(uint32, PuddleHideMainSource)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint32>, SnowBakedFieldBuffer2873)
		SHADER_PARAMETER(uint32, SnowBakedFieldCount2873)
		SHADER_PARAMETER(float, SnowBakedFieldPatternCm2873)
		SHADER_PARAMETER(uint32, SnowBakedFieldValid2873)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint4>, StableSplatBindingBuffer)
		SHADER_PARAMETER(uint32, StableSplatBindingCount)
		SHADER_PARAMETER_SRV(StructuredBuffer<FStableProxyTriangleGPU>, StableProxyTriangleBuffer)
		SHADER_PARAMETER(uint32, StableProxyTriangleCount)
		SHADER_PARAMETER_SRV(StructuredBuffer<FVector4f>, StableProxyTriangleVertexNormalBuffer)
		SHADER_PARAMETER(uint32, StableProxyTriangleVertexNormalCount)
		SHADER_PARAMETER_SRV(StructuredBuffer<FVector4f>, StableProxyVertexProofBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<FVector4f>, StableProxyVertexProofPreviousBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint32>, StableProxyVertexOwnerBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint32>, StableProxyVertexOwnerPreviousBuffer)
		SHADER_PARAMETER(uint32, StableProxyVertexProofCount)
		SHADER_PARAMETER(uint32, StableProxyFieldPairValid)
		SHADER_PARAMETER(uint32, StableProxyLiveTargetValid)
		SHADER_PARAMETER(float, StableProxyFieldABBlend)
		SHADER_PARAMETER_SRV(StructuredBuffer<FVector4f>, StableProxyVertexBuffer)
		SHADER_PARAMETER(uint32, StableProxyVertexCount)
		SHADER_PARAMETER_SRV(StructuredBuffer<float>, StableProxyCastGridBuffer)
		SHADER_PARAMETER(uint32, StableProxyCastGridWidth)
		SHADER_PARAMETER(uint32, StableProxyCastGridHeight)
		SHADER_PARAMETER(uint32, StableProxyCastGridValid)
		SHADER_PARAMETER(FVector4f, StableProxyCastGridOriginWidth)
		SHADER_PARAMETER(FVector4f, StableProxyCastGridForwardEnabled)
		SHADER_PARAMETER(FVector4f, StableProxyCastGridRightRange)
		SHADER_PARAMETER(FVector4f, StableProxyCastGridUpSettings)
		SHADER_PARAMETER(uint32, StableProxyFieldValid)
		SHADER_PARAMETER(uint32, StableShadowDebugMode)
		SHADER_PARAMETER(uint32, UseClusterCulling)
		SHADER_PARAMETER(uint32, UseLODRendering)  // 1 = enable LOD rendering (unified approach)
		SHADER_PARAMETER(uint32, OriginalSplatCount)  // Count of original splats (LOD splats start after)
		// Compaction mode: process only visible splats from CompactedSplatIndices
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, CompactedSplatIndices)
		SHADER_PARAMETER(uint32, UseCompaction)  // 1 = use compacted indices
		SHADER_PARAMETER(uint32, VisibleSplatCount)  // Number of visible splats (when using compaction)
		// Transform matrices
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)
		SHADER_PARAMETER(FMatrix44f, WorldToPLY)  // Transforms world direction to PLY/SH space for SH evaluation
		SHADER_PARAMETER(FMatrix44f, WorldToClip)
		SHADER_PARAMETER(FMatrix44f, WorldToView)
		SHADER_PARAMETER(FVector3f, PreViewTranslation)  // For velocity calculation (TranslatedWorld = World + PreViewTranslation)
		SHADER_PARAMETER(FVector3f, CameraPosition)
		SHADER_PARAMETER(FVector2f, ScreenSize)
		SHADER_PARAMETER(FVector2f, FocalLength)
		SHADER_PARAMETER(uint32, SplatCount)
		SHADER_PARAMETER(uint32, SHOrder)
		SHADER_PARAMETER(uint32, NumSHCoeffs)      // Coefficients to evaluate (based on SHOrder: 4, 9, or 16)
		SHADER_PARAMETER(uint32, SHBufferCoeffs)   // Coefficients actually stored per splat in SHBuffer (based on asset SHBands)
		SHADER_PARAMETER(uint32, UseSHRendering)   // 1 = use view-dependent SH evaluation
		SHADER_PARAMETER(float, OpacityScale)
		SHADER_PARAMETER(float, SplatScale)
		// Cinematic Unreal light bridge
		SHADER_PARAMETER(uint32, ReceiveUnrealLighting)
		SHADER_PARAMETER(uint32, UnrealLightCount)
		SHADER_PARAMETER(float, UnrealLightingInfluence)
		SHADER_PARAMETER(float, UnrealLightingPadding0)
		SHADER_PARAMETER(float, UnrealDirectionalResponse)
		SHADER_PARAMETER(float, UnrealLightWrap)
		SHADER_PARAMETER(uint32, UnrealNormalMode)
		SHADER_PARAMETER(float, UnrealLightingPadding1)
		SHADER_PARAMETER(FVector4f, UnrealAmbientColorIntensity)
		SHADER_PARAMETER(FVector4f, UnrealIndirectLightParams)
		SHADER_PARAMETER(FVector4f, UnrealIndirectShadowTint)
		SHADER_PARAMETER(FVector4f, StableLightingParams0)
		SHADER_PARAMETER(FVector4f, StableLightingParams1)
		SHADER_PARAMETER(FVector4f, StableLightingParams2)
		SHADER_PARAMETER(FVector4f, StableLightingParams3)
		SHADER_PARAMETER(FVector4f, StableLightingParams4)
		SHADER_PARAMETER(FVector4f, UnrealShadowParams)
		SHADER_PARAMETER(FVector4f, UnrealShadowParams2)
		SHADER_PARAMETER(FVector4f, UnrealShadowBounds)
		SHADER_PARAMETER(FVector4f, UnrealShadowHeight)
		SHADER_PARAMETER(FVector4f, UnrealRelightParams)
		SHADER_PARAMETER(FVector4f, UnrealRelightLookParams)
		SHADER_PARAMETER(FVector4f, UnrealRelightSunTint)
		SHADER_PARAMETER(FVector4f, UnrealRelightSkyTint)
		SHADER_PARAMETER(FVector4f, TimeOfDayGradeTint)
		SHADER_PARAMETER(FVector4f, UnrealRelightDetailParams)
		SHADER_PARAMETER(FVector4f, UnrealRelightDetailParams2)
		SHADER_PARAMETER(FVector4f, SpatialDelightingParams0)
		SHADER_PARAMETER(FVector4f, SpatialDelightingParams1)
		SHADER_PARAMETER(FVector4f, SpatialDelightingParams2)
		SHADER_PARAMETER(FVector4f, SpatialDelightingParams3)
		SHADER_PARAMETER(FVector4f, SpatialDelightingParams4)
		SHADER_PARAMETER(FVector4f, ProxyNormalParams)
		SHADER_PARAMETER(FVector4f, GaussianSunAccentParams0)
		SHADER_PARAMETER(FVector4f, GaussianSunAccentParams1)
		SHADER_PARAMETER(FVector4f, GaussianSunAccentTint)
		SHADER_PARAMETER(FVector4f, GaussianPlantTransmissionParams0)
		SHADER_PARAMETER(FVector4f, GaussianPlantTransmissionParams1)
		SHADER_PARAMETER(FVector4f, GaussianPlantTransmissionTint)
		SHADER_PARAMETER(FVector4f, UnrealAtmosphereDistanceParams)
		SHADER_PARAMETER(FVector4f, UnrealAtmosphereFogShapeParams)
		SHADER_PARAMETER(FVector4f, UnrealAtmosphereFogAmountParams)
		SHADER_PARAMETER(FVector4f, UnrealAtmosphereHighlightShapeParams)
		SHADER_PARAMETER(FVector4f, UnrealAtmosphereBloomParams)
		SHADER_PARAMETER(FVector4f, UnrealAtmosphereGlareParams)
		SHADER_PARAMETER(FVector4f, UnrealAtmosphereShaftParams)
		SHADER_PARAMETER(FVector4f, SeasonalColorSelectParams)
		SHADER_PARAMETER(FVector4f, SeasonalColorAdjustParams)
		SHADER_PARAMETER(FVector4f, SeasonalSourceColorFrom)
		SHADER_PARAMETER(FVector4f, SeasonalSourceColorTo)
		SHADER_PARAMETER(FVector4f, SeasonalTargetColorFrom)
		SHADER_PARAMETER(FVector4f, SeasonalTargetColorTo)
		SHADER_PARAMETER(FVector4f, DirectSplatSurfaceEffectParams0)
		SHADER_PARAMETER(FVector4f, DirectSplatSurfaceEffectParams1)
		SHADER_PARAMETER(FVector4f, DirectSplatSurfaceEffectParams2)
		SHADER_PARAMETER(FVector4f, DirectSnowTuningParams)
		SHADER_PARAMETER(FVector4f, SnowSourceCoverParams)
		SHADER_PARAMETER_ARRAY(FVector4f, UnrealLightPositionRadius, [8])
		SHADER_PARAMETER_ARRAY(FVector4f, UnrealLightDirectionType, [8])
		SHADER_PARAMETER_ARRAY(FVector4f, UnrealLightColorIntensity, [8])
		SHADER_PARAMETER_ARRAY(FVector4f, UnrealLightConeAngles, [8])
		SHADER_PARAMETER(uint32, GlobalBaseOffset)  // Offset into global ViewDataBuffer (non-compaction global path)
		// Global compaction path: GPU prefix-sum offsets
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, GlobalBaseOffsetsBuffer)  // prefix sums per proxy
		SHADER_PARAMETER(uint32, ProxyIndex)               // which proxy index this dispatch belongs to
		SHADER_PARAMETER(uint32, UseGlobalCompactionPath)  // 1 = read base from GlobalBaseOffsetsBuffer[ProxyIndex]
		SHADER_PARAMETER(uint32, MaxRenderBudget)          // Physical buffer capacity guard: skip writes at indices >= this value (0 = no guard)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 256);
	}
};

/** Builds an exact light-space receiver triangle-ID grid matched to SceneDepth. */
class FBuildStableProxyReceiverTriangleIdCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FBuildStableProxyReceiverTriangleIdCS);
	SHADER_USE_PARAMETER_STRUCT(FBuildStableProxyReceiverTriangleIdCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_TEXTURE(Texture2D, ReceiverLightDepth)
		SHADER_PARAMETER_SRV(StructuredBuffer<FVector4f>, StableProxyVertexBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<FStableProxyTriangleGPU>, StableProxyTriangleBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, OutReceiverTriangleId)
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)
		SHADER_PARAMETER(FVector4f, LightOriginWidth)
		SHADER_PARAMETER(FVector4f, LightForwardEnabled)
		SHADER_PARAMETER(FVector4f, LightRightRange)
		SHADER_PARAMETER(FVector4f, LightUpSettings)
		SHADER_PARAMETER(uint32, VertexCount)
		SHADER_PARAMETER(uint32, TriangleCount)
		SHADER_PARAMETER(uint32, GridWidth)
		SHADER_PARAMETER(uint32, GridHeight)
		SHADER_PARAMETER(float, DepthMatchToleranceCm)
		SHADER_PARAMETER(uint32, PassMode)
	END_SHADER_PARAMETER_STRUCT()
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/** Phase-2 one-shot stable proxy-surface LightDepth classification.
 *  Keep the receiver-only depth texture represented in this parameter struct
 *  even though the production deterministic BVH path bypasses this legacy pass.
 */
class FBuildStableProxyVertexVisibilityCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FBuildStableProxyVertexVisibilityCS);
	SHADER_USE_PARAMETER_STRUCT(FBuildStableProxyVertexVisibilityCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_TEXTURE(Texture2D, ProxyLightDepth)
		SHADER_PARAMETER_TEXTURE(Texture2D, ProxyReceiverLightDepth)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint32>, ReceiverTriangleId)
		SHADER_PARAMETER_SRV(StructuredBuffer<FVector4f>, StableProxyVertexBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<FStableProxyTriangleGPU>, StableProxyTriangleBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<FVector4f>, PreviousVertexProof)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint32>, PreviousVertexOwner)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<FVector4f>, OutVertexProof)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint32>, OutVertexOwner)
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)
		SHADER_PARAMETER(FVector4f, LightOriginWidth)
		SHADER_PARAMETER(FVector4f, LightForwardEnabled)
		SHADER_PARAMETER(FVector4f, LightRightRange)
		SHADER_PARAMETER(FVector4f, LightUpSettings)
		SHADER_PARAMETER(uint32, VertexCount)
		SHADER_PARAMETER(uint32, PreviousFieldValid)
		SHADER_PARAMETER(float, DepthBiasCm)
		SHADER_PARAMETER(uint32, StabilityDiagnosticMode)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/** v2886: opt-in 4096-sample GPU readback of the already-built 80-byte ViewData.
 * Does not modify any live splat field or color. */
class FGaussianShadowValueSampleCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianShadowValueSampleCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianShadowValueSampleCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(StructuredBuffer<FGaussianSplatViewData>, ViewDataBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<FVector4f>, OutSamples)
		SHADER_PARAMETER(uint32, SplatCount)
		SHADER_PARAMETER(uint32, SampleCount)
	END_SHADER_PARAMETER_STRUCT()
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/** v2781 dense light-space cast grid. */
class FBuildStableProxyCastGridCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FBuildStableProxyCastGridCS);
	SHADER_USE_PARAMETER_STRUCT(FBuildStableProxyCastGridCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_TEXTURE(Texture2D, CombinedLightDepth)
		SHADER_PARAMETER_TEXTURE(Texture2D, ReceiverLightDepth)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<float>, OutCastVisibility)
		SHADER_PARAMETER(uint32, GridWidth)
		SHADER_PARAMETER(uint32, GridHeight)
		SHADER_PARAMETER(float, DepthBiasCm)
		SHADER_PARAMETER(float, WorldUnitsPerTexel)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// NOTE: FGaussianSplatCalcLODViewDataGPUDrivenCS and FUpdateDrawArgsCS have been removed
// in the unified approach. LOD splats are now processed by CalcViewData using the same
// buffers as original splats. See CalcViewData.usf for unified LOD handling.

//----------------------------------------------------------------------
// Splat Compaction Shaders (for GPU-driven work reduction)
//----------------------------------------------------------------------

/**
 * Compute shader that builds a compact list of visible splat indices
 * This enables subsequent passes to only process visible splats
 */
class FCompactSplatsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCompactSplatsCS);
	SHADER_USE_PARAMETER_STRUCT(FCompactSplatsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input: Cluster visibility data
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, SplatClusterIndexBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, ClusterVisibilityBitmap)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, LODClusterSelectedBitmap)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, SelectedClusterBuffer)
		// Output: Compact list of visible splat indices
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, CompactedSplatIndices)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, VisibleSplatCount)
		// Parameters
		SHADER_PARAMETER(uint32, TotalSplatCount)
		SHADER_PARAMETER(uint32, OriginalSplatCount)
		SHADER_PARAMETER(uint32, UseLODRendering)
		SHADER_PARAMETER(uint32, OutputCapacity)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 256);
	}
};

/**
 * Compute shader that prepares indirect dispatch and draw arguments
 * from the visible splat count
 */
class FPrepareIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FPrepareIndirectArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FPrepareIndirectArgsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input: Visible splat count
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, VisibleSplatCount)
		// Output: Indirect dispatch args (uint3: numGroupsX, numGroupsY, numGroupsZ)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, IndirectDispatchArgs)
		// Output: Indirect draw args (5 uints for DrawIndexedIndirect)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, IndirectDrawArgs)
		// Output: Sort indirect dispatch args (uint3: NumTiles, 1, 1)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, SortIndirectArgs)
		// Output: Sort parameters (uint2: SortCount, NumTiles)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, SortParamsBuffer)
		// Thread group size for compute shaders
		SHADER_PARAMETER(uint32, ComputeThreadGroupSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/**
 * Compute shader for calculating sort distances (depth) for each splat
 */
class FGaussianSplatCalcDistancesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatCalcDistancesCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatCalcDistancesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(StructuredBuffer<FGaussianSplatViewData>, ViewDataBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, DistanceBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, KeyBuffer)
		SHADER_PARAMETER(uint32, SplatCount)
		// Base output/input index for bounded direct dispatches. Indirect and legacy
		// paths explicitly bind zero, preserving their byte-for-byte indexing.
		SHADER_PARAMETER(uint32, DispatchBaseIndex)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 256);
	}
};

/**
 * Vertex shader for rendering Gaussian splats as quads
 */
class FGaussianSplatVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatVS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatVS, FGlobalShader);

	class FVRBaseline : SHADER_PERMUTATION_BOOL("GS_VR_BASELINE");
	class FVRRasterClip : SHADER_PERMUTATION_BOOL("GS_VR_RASTER_CLIP");
	class FPCVRColorCache : SHADER_PERMUTATION_BOOL("GS_PCVR_COLOR_CACHE");
    using FPermutationDomain = TShaderPermutationDomain<FVRBaseline,FVRRasterClip,FPCVRColorCache>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(StructuredBuffer<FGaussianSplatViewData>, ViewDataBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<float4>, PCVRFinalColors)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, SortKeysBuffer)
		SHADER_PARAMETER(uint32, BaselineRenderQualityMode)
		SHADER_PARAMETER(FVector4f, BaselineRasterClipParams) // viewport xy, minimum clip area z, production source trim enable w
		SHADER_PARAMETER(uint32, PerSplatLightingEnabled)
        SHADER_PARAMETER(uint32, PCVRReuseRadialDepth)
		SHADER_PARAMETER(float, DayNightBlend)
		SHADER_PARAMETER(FVector4f, RegionalColorGradeParams0)
		SHADER_PARAMETER(FVector4f, RegionalColorGradeParams1)
		SHADER_PARAMETER(float, StableShadowColorRetention)
		SHADER_PARAMETER(FVector4f, ProxyShadowDetailSettings)
		SHADER_PARAMETER(FVector4f, ProxyReflectedLightSettings)
		SHADER_PARAMETER(FVector4f, ProxyReflectedColorSettings)
		SHADER_PARAMETER(FVector4f, ProxyShadowBaseSettings)
		SHADER_PARAMETER(FVector3f, ProxyShadowBaseTint)
		SHADER_PARAMETER(FVector4f, ProxyLightDepthForwardEnabled)
		SHADER_PARAMETER(uint32, SplatCount)
		SHADER_PARAMETER(uint32, DebugMode)  // 0=off, 1=cluster colors (Nanite-style debug)
		SHADER_PARAMETER(uint32, EnableNanite)  // 1=Nanite enabled, 0=disabled (hidden in debug mode)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		const FPermutationDomain Permutation(Parameters.PermutationId);
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5) &&
            (!Permutation.Get<FPCVRColorCache>() || (!Permutation.Get<FVRBaseline>() && !Permutation.Get<FVRRasterClip>()));
	}
};

/**
 * v2785: second draw over the same sorted GS data. It reads the compact
 * SnowAmount + stable-proxy normal payload written by CalcViewData and renders
 * a physically offset snow shell without modifying the original splat pass.
 */
class FGaussianSnowOverlayVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSnowOverlayVS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSnowOverlayVS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(StructuredBuffer<FGaussianSplatViewData>, ViewDataBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, SortKeysBuffer)
		SHADER_PARAMETER(uint32, SplatCount)
		SHADER_PARAMETER(FMatrix44f, SnowTranslatedWorldToClip)
		SHADER_PARAMETER(FVector4f, SnowOverlayParams)
		SHADER_PARAMETER(FVector4f, SnowOverlayColor)
		SHADER_PARAMETER(FVector4f, SnowTuningParams)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FGaussianSnowOverlayPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSnowOverlayPS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSnowOverlayPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, GSRemovalBoxCount)
		SHADER_PARAMETER_ARRAY(FVector4f, GSRemovalBoxCenters, [8])
		SHADER_PARAMETER_ARRAY(FVector4f, GSRemovalBoxAxisX, [8])
		SHADER_PARAMETER_ARRAY(FVector4f, GSRemovalBoxAxisY, [8])
		SHADER_PARAMETER_ARRAY(FVector4f, GSRemovalBoxAxisZ, [8])
		SHADER_PARAMETER(FVector3f, PreViewTranslation)
		SHADER_PARAMETER(FMatrix44f, PuddleClipToWorld)
		SHADER_PARAMETER(FVector4f, PuddleViewport)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/**
 * Pixel shader for rendering Gaussian splats with gaussian falloff
 */
class FGaussianSplatPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatPS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatPS, FGlobalShader);
	class FVRDiagnosticMode : SHADER_PERMUTATION_INT("GS_VR_DIAGNOSTIC_MODE", 4);
	class FPCVRLeanOutputs : SHADER_PERMUTATION_BOOL("GS_PCVR_LEAN_OUTPUTS");
    class FPCVRFinalOnly : SHADER_PERMUTATION_BOOL("GS_PCVR_FINAL_ONLY");
    class FPCVRColorCache : SHADER_PERMUTATION_BOOL("GS_PCVR_COLOR_CACHE");
    class FPCVRNoRemoval : SHADER_PERMUTATION_BOOL("GS_PCVR_NO_REMOVAL");
    class FPCVRNoFogOutput : SHADER_PERMUTATION_BOOL("GS_PCVR_NO_FOG_OUTPUT");
    using FPermutationDomain = TShaderPermutationDomain<FVRDiagnosticMode,FPCVRLeanOutputs,FPCVRFinalOnly,FPCVRColorCache,FPCVRNoRemoval,FPCVRNoFogOutput>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, PerSplatLightingEnabled)
        SHADER_PARAMETER(uint32, PCVRReuseRadialDepth)
        SHADER_PARAMETER(uint32, PCVREarlyAlphaEnabled)
		// Velocity calculation: transform translated world position to previous clip space
		// NOTE: Uses UN-JITTERED matrix to ensure stable velocity when camera is static
		SHADER_PARAMETER(FMatrix44f, PrevTranslatedWorldToClip)
		// PreViewTranslation for converting TranslatedWorld back to World
		SHADER_PARAMETER(FVector3f, PreViewTranslation)
		// Previous frame's PreViewTranslation for correct velocity calculation
		SHADER_PARAMETER(FVector3f, PrevPreViewTranslation)
		SHADER_PARAMETER(FMatrix44f, PuddleClipToWorld)
		SHADER_PARAMETER(FVector4f, PuddleViewport)
		SHADER_PARAMETER(uint32, GSRemovalBoxCount)
		SHADER_PARAMETER_ARRAY(FVector4f, GSRemovalBoxCenters, [8])
		SHADER_PARAMETER_ARRAY(FVector4f, GSRemovalBoxAxisX, [8])
		SHADER_PARAMETER_ARRAY(FVector4f, GSRemovalBoxAxisY, [8])
		SHADER_PARAMETER_ARRAY(FVector4f, GSRemovalBoxAxisZ, [8])

		SHADER_PARAMETER(FVector4f, PuddleWaterParams) // alpha, ripple strength, drop spacing cm, speed multiplier
		SHADER_PARAMETER(float, PuddleWaterRippleSizeCm)
        SHADER_PARAMETER(float, PuddleWaterReflectionStrength)
        SHADER_PARAMETER(float, PuddleRoofRasterCellCm)
        SHADER_PARAMETER(FVector4f, PuddleRoofFXParams) // roof flow enabled, opaque debug, strength, speed
		SHADER_PARAMETER(float, PuddleWaterTimeSeconds)
		// 0=final, 1=light mask, 2=shadow mask, 3=light heatmap
		SHADER_PARAMETER(uint32, LightingPreviewMode)
		// Exact actor stable-shadow selector (0, 66..78). 77/78 use this directly.
		SHADER_PARAMETER(uint32, StableShadowDebugMode)
		// 0=normal resolve, 3=high quality sharper resolve
		SHADER_PARAMETER(uint32, RenderQualityMode)
		SHADER_PARAMETER(uint32, PCVRShadowBlueOverlay)
		// When a proxy shadow catcher is active, the composite pass owns the
		// final light/shadow grade. This prevents the older splat-space mask
		// from fighting the proxy-derived mask as the directional light rotates.
		SHADER_PARAMETER(uint32, UseProxyLightingMaskAsMain)
		SHADER_PARAMETER(float, ProxyLightingIntensityScale)
		// Shared 0..1 sun-altitude phase; used only to keep the photographed carrier from re-brightening under moon authority.
		SHADER_PARAMETER(float, DayNightBlend)
		SHADER_PARAMETER(FVector4f, RegionalColorGradeParams0)
		SHADER_PARAMETER(FVector4f, RegionalColorGradeParams1)
		SHADER_PARAMETER(float, StableShadowColorRetention)
		SHADER_PARAMETER_TEXTURE(Texture2D, ProxyShadowedCapture)
		SHADER_PARAMETER_TEXTURE(Texture2D, ProxyUnshadowedCapture)
		SHADER_PARAMETER_SAMPLER(SamplerState, ProxyCaptureSampler)
		SHADER_PARAMETER(FVector4f, ProxyCaptureViewRect)
		// X=screen-AOV enabled, Y=final-to-capture NDC scale X,
		// Z=final-to-capture NDC scale Y, W=reserved.
		SHADER_PARAMETER(FVector4f, ProxyScreenCaptureSettings)
		SHADER_PARAMETER(FVector4f, ProxyCaptureOriginTanFOV)
		SHADER_PARAMETER(FVector4f, ProxyCaptureForwardAspect)
		SHADER_PARAMETER(FVector4f, ProxyCaptureRightEnabled)
		SHADER_PARAMETER(FVector4f, ProxyCaptureUp)
		// X=softness pixels, Y=gamma, Z=minimum shadow, W=mask strength
		SHADER_PARAMETER(FVector4f, ProxyCaptureSettings)
		// X=camera fade start cm, Y=fade end cm, Z=far extra softness, W=far retained strength
		SHADER_PARAMETER(FVector4f, ProxyShadowCameraDistanceSettings)
		// X=absolute depth tolerance cm, Y=relative distance tolerance, Z=detail preservation, W=depth-edge protection.
		SHADER_PARAMETER(FVector4f, ProxyShadowDetailSettings)
		SHADER_PARAMETER_TEXTURE(Texture2D, ProxySurfaceDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, ProxySurfaceDepthSampler)
		SHADER_PARAMETER(float, ProxySurfaceDepthEnabled)
		// Previous primary capture, sampled only for scalar cast visibility.
		SHADER_PARAMETER_TEXTURE(Texture2D, ProxyHistoryShadowedCapture)
		SHADER_PARAMETER_TEXTURE(Texture2D, ProxyHistoryUnshadowedCapture)
		SHADER_PARAMETER_TEXTURE(Texture2D, ProxyHistorySurfaceDepthTexture)
		SHADER_PARAMETER(FVector4f, ProxyHistoryOriginTanFOV)
		SHADER_PARAMETER(FVector4f, ProxyHistoryForwardAspect)
		SHADER_PARAMETER(FVector4f, ProxyHistoryRightEnabled)
		SHADER_PARAMETER(FVector4f, ProxyHistoryUp)
		// X=history enabled, Y=current-frame weight, Z/W reserved.
		SHADER_PARAMETER(FVector4f, ProxyShadowTemporalSettings)
		// X=full-black receiver level, Y=closure strength, Z=closure area scale, W=edge softness.
		SHADER_PARAMETER(FVector4f, ProxyShadowEvidenceSettings)
		// X=direct HDR white point, Y=direct response gamma, Z=closed-shadow max darkness, W=closed-shadow start receiver level
		SHADER_PARAMETER(FVector4f, ProxyShadowShapingSettings)
		SHADER_PARAMETER(float, ProxyShadowOcclusionBoost)
		// X=final proxy-shadow bounce strength, Y=enable unified colored bounce
		SHADER_PARAMETER(FVector2f, ProxyShadowColorSettings)
		SHADER_PARAMETER(FVector4f, ProxyReflectedLightSettings)
		// X=deep tint neutralization, Y=deep source-color retention, Z=highlight compression, W=tint transition softness
		SHADER_PARAMETER(FVector4f, ProxyReflectedColorSettings)
		// X=base transfer strength, Y=base minimum light, Z=base saturation, W=enabled
		SHADER_PARAMETER(FVector4f, ProxyShadowBaseSettings)
		SHADER_PARAMETER(FVector3f, ProxyShadowBaseTint)
		SHADER_PARAMETER_TEXTURE(Texture2D, ProxyLightDepthCapture)
		SHADER_PARAMETER_SAMPLER(SamplerState, ProxyLightDepthSampler)
		SHADER_PARAMETER(FVector4f, ProxyLightDepthOriginWidth)
		SHADER_PARAMETER(FVector4f, ProxyLightDepthForwardEnabled)
		SHADER_PARAMETER(FVector4f, ProxyLightDepthRightRange)
		SHADER_PARAMETER(FVector4f, ProxyLightDepthUpSettings)
		// X=strength, Y=density, Z=max camera-ray distance (cm), W=forward scattering
		SHADER_PARAMETER(FVector4f, ProxyVolumetricShaftSettings)
		SHADER_PARAMETER(FVector3f, ProxyVolumetricShaftTint)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		const FPermutationDomain Permutation(Parameters.PermutationId);
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5) &&
            (!Permutation.Get<FPCVRLeanOutputs>() || Permutation.Get<FVRDiagnosticMode>()==0) &&
            (!Permutation.Get<FPCVRFinalOnly>() || Permutation.Get<FPCVRLeanOutputs>()) &&
            (!Permutation.Get<FPCVRColorCache>() || Permutation.Get<FPCVRFinalOnly>()) &&
            (!Permutation.Get<FPCVRNoRemoval>() || Permutation.Get<FPCVRColorCache>()) &&
            (!Permutation.Get<FPCVRNoFogOutput>() ||
                (Permutation.Get<FPCVRLeanOutputs>() && Permutation.Get<FVRDiagnosticMode>()==0));
	}
};

/**
 * Vertex shader for the before-tonemap HDR composite pass (full-screen triangle)
 */
class FGaussianPCVRFinalColorCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FGaussianPCVRFinalColorCS);
    SHADER_USE_PARAMETER_STRUCT(FGaussianPCVRFinalColorCS,FGlobalShader);
    class FVisibleIndirect : SHADER_PERMUTATION_BOOL("GS_PCVR_FINAL_INDIRECT");
    using FPermutationDomain=TShaderPermutationDomain<FVisibleIndirect>;
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
        SHADER_PARAMETER_SRV(StructuredBuffer<FGaussianSplatViewData>, ViewDataBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<uint>, SortKeysBuffer)
        SHADER_PARAMETER_SRV(StructuredBuffer<uint>, SortParams)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<float4>, PCVRFinalColorsOut)
        SHADER_PARAMETER(uint32, SourceCapacity)
        SHADER_PARAMETER(float, DayNightBlend)
        SHADER_PARAMETER(FVector4f, RegionalColorGradeParams0)
        SHADER_PARAMETER(FVector4f, RegionalColorGradeParams1)
        SHADER_PARAMETER(float, StableShadowColorRetention)
        SHADER_PARAMETER(FVector4f, ProxyShadowDetailSettings)
        SHADER_PARAMETER(FVector4f, ProxyReflectedLightSettings)
        SHADER_PARAMETER(FVector4f, ProxyReflectedColorSettings)
        SHADER_PARAMETER(FVector4f, ProxyShadowBaseSettings)
        SHADER_PARAMETER(FVector3f, ProxyShadowBaseTint)
        SHADER_PARAMETER(FVector4f, ProxyLightDepthForwardEnabled)
    END_SHADER_PARAMETER_STRUCT()
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
    { return IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM5); }
};

class FGaussianSplatCompositeVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatCompositeVS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatCompositeVS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/**
 * First post-process pixel shader. It converts the accumulated GS image into
 * the exact final, graded scene-linear HDR source used by the bloom pass.
 */
class FGaussianSplatCompositePS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatCompositePS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatCompositePS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_TEXTURE(Texture2D, IntermediateTexture)
        SHADER_PARAMETER_TEXTURE(Texture2D, IntermediateDepthTexture)
		SHADER_PARAMETER_TEXTURE(Texture2D, SnowIntermediateTexture)
		SHADER_PARAMETER_TEXTURE(Texture2D, SnowIntermediateDepthTexture)
		SHADER_PARAMETER_TEXTURE(Texture2D, SnowOcclusionDepthTexture)
		SHADER_PARAMETER(FVector4f, SnowCompositeParams)
		SHADER_PARAMETER(FVector4f, SnowTuningParams)
		SHADER_PARAMETER_TEXTURE(Texture2D, ProxyCompositeLightDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, ProxyCompositeLightDepthSampler)
		SHADER_PARAMETER(uint32, PanoramaShadowMaskProbe)
		SHADER_PARAMETER(FVector4f, GaussianCompositeParams0)
		SHADER_PARAMETER(FVector4f, GaussianCompositeParams1)
		SHADER_PARAMETER(FVector4f, GaussianCompositeParams2)
		SHADER_PARAMETER(FVector4f, GaussianCompositeParams3)
		SHADER_PARAMETER(FVector4f, GaussianCompositeParams4)
		SHADER_PARAMETER(float, GaussianCompositeSourceBrightness)
		SHADER_PARAMETER(FVector4f, GaussianCompositeSourceRect)
		SHADER_PARAMETER(FVector4f, GaussianCompositeOutputRect)
		SHADER_PARAMETER(FMatrix44f, ProxyCompositeInvViewProjection)
		SHADER_PARAMETER(FVector4f, ProxyCompositeCameraOriginValid)
		SHADER_PARAMETER(FVector4f, ProxyCompositeLightOriginWidth)
		SHADER_PARAMETER(FVector4f, ProxyCompositeLightForwardSoftness)
		SHADER_PARAMETER(FVector4f, ProxyCompositeLightRightRange)
		SHADER_PARAMETER(FVector4f, ProxyCompositeLightUp)
		SHADER_PARAMETER(FVector4f, ProxyCompositeShaftSettings)
		SHADER_PARAMETER(FVector3f, ProxyCompositeShaftTint)
		SHADER_PARAMETER(FVector4f, NativeFogCompositeSettings)
		SHADER_PARAMETER(FVector4f, NativeFogCompositeColor)
		SHADER_PARAMETER(FVector4f, NativeFogCompositeProfile0)
		SHADER_PARAMETER(FVector4f, NativeFogCompositeProfile1)
    END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

};

/**
 * Second post-process pixel shader. It extracts spatial bloom from the graded
 * HDR source and premultiplied-alpha blends the combined result onto SceneColor.
 */
class FGaussianPCVRBloomExtractCS : public FGlobalShader {
 DECLARE_GLOBAL_SHADER(FGaussianPCVRBloomExtractCS);
 SHADER_USE_PARAMETER_STRUCT(FGaussianPCVRBloomExtractCS,FGlobalShader);
 BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
  SHADER_PARAMETER_RDG_TEXTURE(Texture2D,IntermediateTexture)
  SHADER_PARAMETER(FVector4f,GaussianCompositeBloomParams)
  SHADER_PARAMETER(FVector4f,GaussianCompositeColorBloomParams)
  SHADER_PARAMETER(float,GaussianCompositeBloomSoftKnee)
  SHADER_PARAMETER(FIntPoint,PCVRBloomExtractExtent)
  SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>,PCVRBloomExtractOut)
 END_SHADER_PARAMETER_STRUCT()
 static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P){return IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM5);}
};
class FGaussianSplatBloomCompositePS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatBloomCompositePS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatBloomCompositePS, FGlobalShader);

 public:
 class FExtractCache:SHADER_PERMUTATION_BOOL("GS_PCVR_BLOOM_CACHE");
 using FPermutationDomain=TShaderPermutationDomain<FExtractCache>;
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		
		SHADER_PARAMETER_TEXTURE(Texture2D, IntermediateTexture)
        SHADER_PARAMETER_TEXTURE(Texture2D, PCVRBloomExtractTexture)
        SHADER_PARAMETER_TEXTURE(Texture2D, PCVRFogCoverageTexture)
		SHADER_PARAMETER_TEXTURE(Texture2D, IntermediateDepthTexture)
		SHADER_PARAMETER_TEXTURE(Texture2D, IntermediateFogDepthTexture)
		SHADER_PARAMETER_TEXTURE(Texture2D, ProxyCompositeLightDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, ProxyCompositeLightDepthSampler)
		SHADER_PARAMETER(uint32, StableShadowDebugMode)
		SHADER_PARAMETER(uint32, StableShadowDebugLightDepthValid)
		SHADER_PARAMETER(uint32, PanoramaShadowMaskProbe)
        SHADER_PARAMETER(uint32, PanoramaGSCompositeMode)
		SHADER_PARAMETER(FVector4f, GaussianCompositeParams0)
		SHADER_PARAMETER(FVector4f, GaussianCompositeBloomParams)
		SHADER_PARAMETER(FVector4f, GaussianCompositeColorBloomParams)
		SHADER_PARAMETER(FVector4f, GaussianCompositeBloomTint)
		SHADER_PARAMETER(float, GaussianCompositeBloomSoftKnee)
		SHADER_PARAMETER(FVector4f, GaussianCompositeSourceRect)
		SHADER_PARAMETER(FVector4f, GaussianCompositeOutputRect)
		// Fog-only parameters used by MainBloomCompositePS.  These mirror the
		// existing composite-pass values; no non-fog render path is changed.
		SHADER_PARAMETER(FMatrix44f, ProxyCompositeInvViewProjection)
		SHADER_PARAMETER(FVector4f, ProxyCompositeCameraOriginValid)
		SHADER_PARAMETER(FVector4f, ProxyCompositeLightOriginWidth)
		SHADER_PARAMETER(FVector4f, ProxyCompositeLightForwardSoftness)
		SHADER_PARAMETER(FVector4f, ProxyCompositeLightRightRange)
		SHADER_PARAMETER(FVector4f, ProxyCompositeLightUp)
		SHADER_PARAMETER(FVector3f, ProxyCompositeShaftTint)
		SHADER_PARAMETER(FVector4f, NativeFogCompositeSettings)
		SHADER_PARAMETER(FVector4f, NativeFogCompositeColor)
		SHADER_PARAMETER(FVector4f, NativeFogCompositeProfile0)
		SHADER_PARAMETER(FVector4f, NativeFogCompositeProfile1)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/** Dedicated post-grade wet surface pass. GS colour/depth remain the rendered
 * source; v36 uses only the proxy/collider DEPTH capture as continuous receiver
 * geometry so individual Gaussian footprints cannot define the wet boundary. */
class FGaussianSplatWetSurfacePS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatWetSurfacePS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatWetSurfacePS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_TEXTURE(Texture2D, WetSourceTexture)
		SHADER_PARAMETER_TEXTURE(Texture2D, WetDepthTexture)
		SHADER_PARAMETER_TEXTURE(Texture2D, WetNormalTexture)
		// v36 continuous collider/proxy receiver surface. This is a depth-only
		// geometry authority; it never supplies reflected colour.
		SHADER_PARAMETER_TEXTURE(Texture2D, WetProxySurfaceDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, WetProxySurfaceDepthSampler)
		SHADER_PARAMETER(FVector4f, WetProxySurfaceOriginTanFOV)
		SHADER_PARAMETER(FVector4f, WetProxySurfaceForwardAspect)
		SHADER_PARAMETER(FVector4f, WetProxySurfaceRightEnabled)
		SHADER_PARAMETER(FVector4f, WetProxySurfaceUp)
		// RainSkyReflection v26: read-only Unreal SceneColor environment source.
		SHADER_PARAMETER_TEXTURE(Texture2D, WetSceneColorTexture)
		SHADER_PARAMETER(FVector4f, WetSourceRect)
		SHADER_PARAMETER(FVector4f, WetSceneColorRect)
		SHADER_PARAMETER(FVector4f, WetOutputRect)
		SHADER_PARAMETER(float, WetSceneColorInvPreExposure)
		SHADER_PARAMETER(FMatrix44f, WetInvViewProjection)
		SHADER_PARAMETER(FMatrix44f, WetWorldToClip)
		SHADER_PARAMETER(FVector4f, WetCameraTime)
		SHADER_PARAMETER(FVector4f, WetSettings0)
		SHADER_PARAMETER(FVector4f, WetSettings1)
		SHADER_PARAMETER(FVector4f, WetSettings2)
		SHADER_PARAMETER(FVector4f, WetSettings3)
		SHADER_PARAMETER(FVector4f, WetRainGrade0)
		SHADER_PARAMETER(FVector4f, WetRainGrade1)
		SHADER_PARAMETER(FVector4f, WetLightDirectionValid)
		SHADER_PARAMETER(FVector4f, WetLightColorIntensity)
		// v31: xyz surface-to-moon world direction, w source angular diameter in degrees.
		SHADER_PARAMETER(FVector4f, WetMoonDirectionSize)
		// v31: rgb authored moon color, w 0..1 runtime visibility/energy.
		SHADER_PARAMETER(FVector4f, WetMoonColorStrength)
	END_SHADER_PARAMETER_STRUCT()
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/**
 * Radix sort - CountCS: per-tile histogram of 256 digit bins
 */
class FRadixSortCountCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRadixSortCountCS);
	SHADER_USE_PARAMETER_STRUCT(FRadixSortCountCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, HistogramBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, SrcKeys)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, SortParams)
		SHADER_PARAMETER(uint32, RadixShift)
		SHADER_PARAMETER(uint32, Count)
		SHADER_PARAMETER(uint32, NumTiles)
		SHADER_PARAMETER(uint32, UseIndirectSort)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("COUNT_CS"), 1);
	}
};

/**
 * Radix sort - PrefixSumCS: exclusive prefix sum per digit across tiles
 */
class FRadixSortPrefixSumCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRadixSortPrefixSumCS);
	SHADER_USE_PARAMETER_STRUCT(FRadixSortPrefixSumCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, HistogramBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, DigitOffsetBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, SortParams)
		SHADER_PARAMETER(uint32, NumTiles)
		SHADER_PARAMETER(uint32, UseIndirectSort)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("PREFIX_SUM_CS"), 1);
	}
};

/**
 * Radix sort - DigitPrefixSumCS: exclusive prefix sum across 256 digit totals
 */
class FRadixSortDigitPrefixSumCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRadixSortDigitPrefixSumCS);
	SHADER_USE_PARAMETER_STRUCT(FRadixSortDigitPrefixSumCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, DigitOffsetBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DIGIT_PREFIX_SUM_CS"), 1);
	}
};

/**
 * Radix sort - ScatterCS: write keys+values to sorted positions
 */
class FRadixSortScatterCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRadixSortScatterCS);
	SHADER_USE_PARAMETER_STRUCT(FRadixSortScatterCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, SrcKeys)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, SrcVals)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, DstKeys)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, DstVals)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, HistogramBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, DigitOffsetBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, SortParams)
		SHADER_PARAMETER(uint32, RadixShift)
		SHADER_PARAMETER(uint32, Count)
		SHADER_PARAMETER(uint32, NumTiles)
		SHADER_PARAMETER(uint32, UseIndirectSort)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SCATTER_CS"), 1);
	}
};

//----------------------------------------------------------------------
// Cluster Culling Shaders (Nanite-style optimization)
//----------------------------------------------------------------------

/**
 * Compute shader to reset the visible cluster counter, indirect draw args, and visibility bitmap
 */
class FClusterCullingResetCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FClusterCullingResetCS);
	SHADER_USE_PARAMETER_STRUCT(FClusterCullingResetCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, VisibleClusterCountBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, VisibleSplatCount)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, IndirectDrawArgsBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, ClusterVisibilityBitmap)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, SelectedClusterBuffer)
		// LOD cluster tracking buffers
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, LODClusterBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, LODClusterCountBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, LODClusterSelectedBitmap)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, LODSplatTotalBuffer)
		// GPU-driven LOD rendering
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, LODSplatOutputCountBuffer)
		SHADER_PARAMETER(uint32, ClusterVisibilityBitmapSize)
		SHADER_PARAMETER(uint32, LeafClusterCount)
		// Debug: must match MainCS parameters (shared .usf file)
		SHADER_PARAMETER(int32, DebugForceLODLevel)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
	}
};

/**
 * Compute shader for frustum culling and LOD selection of clusters
 * Tests each cluster's bounding sphere against the view frustum
 * Calculates screen-space error for LOD selection
 * Outputs list of visible cluster indices with LOD flags
 */
class FClusterCullingCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FClusterCullingCS);
	SHADER_USE_PARAMETER_STRUCT(FClusterCullingCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(StructuredBuffer<FGaussianGPUCluster>, ClusterBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, VisibleClusterBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, VisibleClusterCountBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, IndirectDrawArgsBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, ClusterVisibilityBitmap)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, SelectedClusterBuffer)
		// LOD cluster tracking buffers
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, LODClusterBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, LODClusterCountBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, LODClusterSelectedBitmap)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, LODSplatTotalBuffer)
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)
		SHADER_PARAMETER(FMatrix44f, WorldToClip)
		SHADER_PARAMETER(uint32, ClusterCount)
		SHADER_PARAMETER(uint32, LeafClusterCount)
		SHADER_PARAMETER_ARRAY(FVector4f, FrustumPlanes, [6])
        SHADER_PARAMETER_ARRAY(FVector4f, StereoFrustumPlanes, [6])
        SHADER_PARAMETER(uint32, UseStereoUnion)
        SHADER_PARAMETER(FVector3f, StereoCameraPosition)
		// LOD selection parameters
		SHADER_PARAMETER(FVector3f, CameraPosition)
		SHADER_PARAMETER(float, ScreenHeight)
		SHADER_PARAMETER(float, ErrorThreshold)
		SHADER_PARAMETER(float, LODBias)
		SHADER_PARAMETER(float, ClusterWorldMaxScale)
		SHADER_PARAMETER(uint32, UseLODRendering)
		// Debug: Force specific LOD level (-1 = auto, 0 = leaf, 1+ = specific level)
		SHADER_PARAMETER(int32, DebugForceLODLevel)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 64);
	}
};

//----------------------------------------------------------------------
// Global Accumulator + Compaction prefix-sum shaders
//----------------------------------------------------------------------

/**
 * GatherCS: 1 thread per proxy.
 * Copies GPUResources->VisibleSplatCountBuffer[0] into GlobalVisibleCountArray[ProxyIndex].
 */
class FGatherVisibleCountsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGatherVisibleCountsCS);
	SHADER_USE_PARAMETER_STRUCT(FGatherVisibleCountsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, PerProxyVisibleCount)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, GlobalVisibleCountArray)
		SHADER_PARAMETER(uint32, ProxyIndex)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("GATHER_CS"), 1);
	}
};

/**
 * PrefixSumCS: 1 thread total.
 * Computes prefix sums over GlobalVisibleCountArray and writes all indirect
 * dispatch/draw args for the global Phase-3 passes (CalcDistances, RadixSort, DrawSplats).
 */
class FPrefixSumVisibleCountsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FPrefixSumVisibleCountsCS);
	SHADER_USE_PARAMETER_STRUCT(FPrefixSumVisibleCountsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>,   GlobalVisibleCountArray)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, GlobalBaseOffsets)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, GlobalCalcDistIndirectArgs)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, GlobalSortIndirectArgs)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, GlobalSortParams)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, GlobalDrawIndirectArgs)
		SHADER_PARAMETER(uint32, ProxyCount)
		SHADER_PARAMETER(uint32, MaxRenderBudget)  // Physical buffer capacity guard: clamp total visible to this value (0 = no guard)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VISIBLE_PREFIX_SUM_CS"), 1);
	}
};

/** Minimal premultiplied-alpha composite, used only by gs.VRDiagMode 2. */
class FGaussianSplatVRDiagnosticCompositePS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatVRDiagnosticCompositePS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatVRDiagnosticCompositePS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_TEXTURE(Texture2D, VRDiagnosticSource)
		SHADER_PARAMETER(FVector4f, VRDiagnosticSourceRect)
		SHADER_PARAMETER(FVector4f, VRDiagnosticOutputRect)
		SHADER_PARAMETER(float, VRDiagnosticExposure)
	END_SHADER_PARAMETER_STRUCT()
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/** Find the valid prefix of already sorted distances, preserving alpha order. */
class FGSVRBaselineDrawArgsCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FGSVRBaselineDrawArgsCS);
    SHADER_USE_PARAMETER_STRUCT(FGSVRBaselineDrawArgsCS, FGlobalShader);
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, SortedDistances)
        SHADER_PARAMETER_UAV(RWBuffer<uint>, DrawArgs)
        SHADER_PARAMETER(uint32, SourceCount)
    END_SHADER_PARAMETER_STRUCT()
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    { return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); }
};

class FGSPCVRCompactCountCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FGSPCVRCompactCountCS);
    SHADER_USE_PARAMETER_STRUCT(FGSPCVRCompactCountCS,FGlobalShader);
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
        SHADER_PARAMETER_SRV(StructuredBuffer<FGaussianSplatViewData>,ViewData)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,ScratchDepth)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,ScratchRank)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,Blocks)
        SHADER_PARAMETER(uint32,SourceCount)
        SHADER_PARAMETER(uint32,PreCullBoxCount)
        SHADER_PARAMETER(FVector3f,PreCullTranslation)
        SHADER_PARAMETER(FMatrix44f,PreCullClipToWorld)
        SHADER_PARAMETER_ARRAY(FVector4f,PreCullCenters,[8])
        SHADER_PARAMETER_ARRAY(FVector4f,PreCullX,[8])
        SHADER_PARAMETER_ARRAY(FVector4f,PreCullY,[8])
        SHADER_PARAMETER_ARRAY(FVector4f,PreCullZ,[8])
    END_SHADER_PARAMETER_STRUCT()
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P){return IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM5);}
};
class FGSPCVRCompactPrefixCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FGSPCVRCompactPrefixCS);
    SHADER_USE_PARAMETER_STRUCT(FGSPCVRCompactPrefixCS,FGlobalShader);
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,Blocks)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,SortParamsOut)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,SortDispatch)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,DrawArgsOut)
        SHADER_PARAMETER(uint32,BlockCount)
    END_SHADER_PARAMETER_STRUCT()
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P){return IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM5);}
};
class FGSPCVRCompactScatterCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FGSPCVRCompactScatterCS);
    SHADER_USE_PARAMETER_STRUCT(FGSPCVRCompactScatterCS,FGlobalShader);
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,ScratchDepth)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,ScratchRank)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,Blocks)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,OutDepth)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,OutIndex)
        SHADER_PARAMETER(uint32,SourceCount)
        SHADER_PARAMETER(uint32,BlockCount)
    END_SHADER_PARAMETER_STRUCT()
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P){return IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM5);}
};

class FGSPCVRSourceCountCS : public FGlobalShader
{
DECLARE_GLOBAL_SHADER(FGSPCVRSourceCountCS);
SHADER_USE_PARAMETER_STRUCT(FGSPCVRSourceCountCS,FGlobalShader);
BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
SHADER_PARAMETER_SRV(StructuredBuffer<uint>,SourceCluster)
SHADER_PARAMETER_SRV(StructuredBuffer<uint>,LeafVisibility)
SHADER_PARAMETER_SRV(StructuredBuffer<uint>,ParentVisibility)
SHADER_PARAMETER_SRV(StructuredBuffer<uint>,SelectedSourceCluster)
SHADER_PARAMETER(uint32,UseSourceClusters)
SHADER_PARAMETER(uint32,UseSourceLOD)
SHADER_PARAMETER(uint32,OriginalSourceCount)
SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,ScratchDepth)
SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,ScratchRank)
SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,Blocks)
SHADER_PARAMETER(uint32,SourceCount)
END_SHADER_PARAMETER_STRUCT()
static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P){return IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM5);}
static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P,FShaderCompilerEnvironment& E)
{ FGlobalShader::ModifyCompilationEnvironment(P,E);E.SetDefine(TEXT("PCVR_SOURCE_COMPACT"),1); }
};

class FGSPCVRSourcePrefixCS : public FGlobalShader
{
DECLARE_GLOBAL_SHADER(FGSPCVRSourcePrefixCS);
SHADER_USE_PARAMETER_STRUCT(FGSPCVRSourcePrefixCS,FGlobalShader);
BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,Blocks)
SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,VisibleCountOut)
SHADER_PARAMETER(uint32,BlockCount)
END_SHADER_PARAMETER_STRUCT()
static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P){return IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM5);}
static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P,FShaderCompilerEnvironment& E)
{ FGlobalShader::ModifyCompilationEnvironment(P,E);E.SetDefine(TEXT("PCVR_SOURCE_COMPACT"),1); }
};

class FGSPCVRSourceScatterCS : public FGlobalShader
{
DECLARE_GLOBAL_SHADER(FGSPCVRSourceScatterCS);
SHADER_USE_PARAMETER_STRUCT(FGSPCVRSourceScatterCS,FGlobalShader);
BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,ScratchDepth)
SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,ScratchRank)
SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,Blocks)
SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>,OutIndex)
SHADER_PARAMETER(uint32,SourceCount)
SHADER_PARAMETER(uint32,BlockCount)
END_SHADER_PARAMETER_STRUCT()
static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P){return IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM5);}
static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P,FShaderCompilerEnvironment& E)
{ FGlobalShader::ModifyCompilationEnvironment(P,E);E.SetDefine(TEXT("PCVR_SOURCE_COMPACT"),1); }
};
