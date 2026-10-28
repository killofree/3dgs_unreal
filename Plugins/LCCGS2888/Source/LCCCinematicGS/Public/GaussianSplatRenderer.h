// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "RenderGraphBuilder.h"
#include "SceneView.h"

class FGaussianSplatSceneProxy;
class FGaussianSplatGPUResources;
struct FGaussianGlobalAccumulator;
struct FGaussianSplatLightingData;
struct FGaussianCapturedDepthShadowRenderData;

/**
 * Handles the rendering of Gaussian Splats
 * Orchestrates compute passes for view calculation, sorting, and final rendering
 */
class LCCCINEMATICGS_API FGaussianSplatRenderer
{
public:
    static void DispatchPCVRSourceCompact(FRHICommandListImmediate& Cmd,FGaussianSplatGPUResources* R,FGaussianGlobalAccumulator* A,uint32 Count,uint32 OriginalCount,bool UseLOD);

    static void DispatchPCVRStableCompact(FRHICommandListImmediate& Cmd,FGaussianGlobalAccumulator* A,int32 Count,const FSceneView& View);

	static int32 GetVRDiagnosticMode_RenderThread();
    static bool IsSourceQuadTrimEnabled_RenderThread();
    static uint32 GetViewDataEffectsHash_RenderThread();
    static uint32 GetVisibilityEffectsHash_RenderThread(const FGaussianSplatGPUResources* Resources);
	static bool IsVRDiagnosticLogging_RenderThread();
	static void CompositeVRDiagnostic(FRHICommandListImmediate& RHICmdList,
		const FSceneView& View, FTextureRHIRef Source, const FIntRect& SourceRect,
		const FIntRect& OutputRect, bool bAfterTonemap);
	static void SetWetReceiverTestMode(int32 Mode, bool bPreviewRipples);
	static void SetPuddleDampenStrength(float Strength, bool bHideOriginalSource, const FVector2f& SourceWetSettings);
	static void SetPuddleWaterVisualSettings(const FVector4f& Settings, float RippleSizeCm, float ReflectionStrength, float RoofRasterCellCm, const FVector4f& RoofFXParams);
	static int32 GetWetReceiverTestMode();
	FGaussianSplatRenderer();
	~FGaussianSplatRenderer();
	static void SetProxyShadowCaptureSettings(const FVector4f& Settings);
	static void SetProxyShadowDetailSettings(const FVector4f& Settings);
	static void SetProxyShadowEvidenceSettings(
		float FullBlackReceiverLevel, float LightingMatchStrength,
		float ClosureAreaScale, float ClosureEdgeSoftness);
	static void SetProxyShadowShapingSettings(const FVector4f& Settings);
	static void SetProxyShadowColorSettings(float Saturation, bool bEnableShadowLighting);
	static void SetProxyReflectedLightSettings(const FVector4f& Settings);
	static void SetProxyReflectedColorSettings(const FVector4f& Settings);
	static void SetProxyShadowFinalCompositeSettings(const FVector4f& Settings, const FVector3f& Tint);
	static void SetProxyVolumetricShaftSettings(const FVector4f& Settings, const FVector3f& Tint);
	/** Fog-only bridge: lets the late GS composite use the same distance/colour authority as UE Exponential/Volumetric Fog. */
	static void SetNativeFogCompositeSettings(
		const FVector4f& Settings,
		const FVector4f& Color,
		const FVector4f& Profile0,
		const FVector4f& Profile1);
	/** Render-thread query used only to allocate the fog-depth auxiliary target while dense fog is active. */
	static bool IsNativeFogCompositeEnabled_RenderThread();
	static void SetProxyShadowCaptureProjection(
		const FVector3f& Origin, const FVector3f& Forward,
		const FVector3f& Right, const FVector3f& Up,
		float HorizontalFOVDegrees, float AspectRatio);
	/** Stable debug selector. Render-thread only; does not recreate or mutate a SceneProxy. */
	static void SetStableShadowDebugMode(int32 DebugMode);
	static int32 GetStableShadowDebugMode();
	/** Translate the actor debug selector into a pixel-only LightingPreviewMode override. */
	static int32 ResolveStableShadowDebugLightingPreviewMode(int32 ExistingLightingPreviewMode);
	static void SetProxyShadowOcclusionBoost(float Boost);
	/** Phase-2 one-shot welded-proxy 1x1 depth compare. Returns true only when a complete field was written. */
	static bool DispatchBuildStableProxyVertexVisibility(
		FRHICommandListImmediate& RHICmdList,
		FGaussianSplatGPUResources* GPUResources,
		const FMatrix& LocalToWorld,
		float DepthBiasCm,
		bool bCommitToFront = true);

	/** v2781: dense light-space cast visibility grid from matched receiver-only and Self+External depths. */
	static bool DispatchBuildStableProxyCastGrid(
		FRHICommandListImmediate& RHICmdList,
		FGaussianSplatGPUResources* GPUResources,
		float DepthBiasCm);

	static void SetProxyLightDepthCapture(
		FTextureRHIRef DepthTexture,
		FTextureRHIRef ReceiverDepthTexture,
		const FVector3f& Origin,
		const FVector3f& Forward,
		const FVector3f& Right,
		const FVector3f& Up,
		const FVector4f& Settings,
		uint32 CaptureRevision);
	static uint32 GetProxyLightDepthCaptureRevision_RenderThread();
	static void SetProxySurfaceEffect(
		FTextureRHIRef DepthTexture,
		const FVector4f& Settings0,
		const FVector4f& Settings1,
		const FVector4f& Settings2,
		const FVector3f& Origin,
		const FVector3f& Forward,
		const FVector3f& Right,
		const FVector3f& Up,
		float HorizontalFOVDegrees,
		float AspectRatio);
	/** Publishes direct Snow/Wet controls without depending on a proxy camera capture. */
	static void SetDirectSurfaceEffectSettings(
		const FVector4f& Settings0,
		const FVector4f& Settings1,
		const FVector4f& Settings2,
		const FVector4f& Settings3);
	/** Snow-only tuning: x ground fill, y structure removal, z structure slope threshold. */
	static void SetDirectSnowTuningSettings(const FVector4f& Settings);
	static void SetSnowSourceCoverSettings(const FVector4f& Settings);
	/** Rain-only post-grade color overlay. Neutral outside WeatherMode::Rain. */
	static void SetDirectRainOverlaySettings(const FVector4f& Settings0, const FVector4f& Settings1);
	/** Rain-only runtime precipitation gate. OFF means residual puddles while Rain mode stays active. */
	static void SetDirectRainRuntimeState(bool bRainModeActive, bool bPrecipitationActive);
	/** v31: runtime-only moon direction/color for camera-independent Wet moon cue. */
	static void SetDirectMoonReflectionState(
		const FVector3f& SurfaceToMoonDirection,
		const FVector3f& MoonColor,
		float Strength,
		float AngularDiameterDegrees,
		bool bEnabled);
	/** v2785: true only while the direct Snow mode needs the second GS overlay pass. */
	static bool IsDirectSnowOverlayEnabled_RenderThread();

	/**
	 * Dispatch the view data calculation compute shader
	 * @param bUseLODRendering If true, skip splats covered by parent LOD clusters
	 */
	static void DispatchCalcViewData(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FGaussianSplatGPUResources* GPUResources,
		const FMatrix& LocalToWorld,
		int32 SplatCount,
		int32 SHOrder,
		float OpacityScale,
		float SplatScale,
		bool bUseLODRendering = false,
		const FGaussianSplatLightingData* LightingData = nullptr,
		const FGaussianCapturedDepthShadowRenderData* CapturedDepthData = nullptr
	);

	/**
	 * Dispatch the distance calculation compute shader
	 */
	static void DispatchCalcDistances(
		FRHICommandListImmediate& RHICmdList,
		FGaussianSplatGPUResources* GPUResources,
		int32 SplatCount
	);

	/**
	 * Dispatch radix sort for back-to-front ordering
	 */
	static void DispatchRadixSort(
		FRHICommandListImmediate& RHICmdList,
		FGaussianSplatGPUResources* GPUResources,
		int32 SplatCount
	);

	/**
	 * Dispatch radix sort with indirect dispatch (GPU-driven sort count)
	 * Uses SortIndirectArgsBuffer for CountCS/ScatterCS dispatch dimensions
	 * and SortParamsBuffer for Count/NumTiles read by shaders.
	 * Only sorts the visible splats after compaction.
	 */
	static void DispatchRadixSortIndirect(
		FRHICommandListImmediate& RHICmdList,
		FGaussianSplatGPUResources* GPUResources
	);

	/**
	 * Draw the Gaussian splats
	 */
	static void DrawSplats(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FGaussianSplatGPUResources* GPUResources,
		int32 SplatCount
	);

	/**
	 * Dispatch cluster culling compute shader (Nanite-style optimization)
	 * Tests cluster bounding spheres against view frustum
	 * @param ErrorThreshold Screen-space error threshold in pixels for LOD selection
	 * @param bUseLODRendering If true, track unique LOD clusters for later rendering
	 * @return Number of visible clusters (for statistics)
	 */
	static int32 DispatchClusterCulling(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FGaussianSplatGPUResources* GPUResources,
		const FMatrix& LocalToWorld,
		float ErrorThreshold,
		bool bUseLODRendering = false
	);

	// NOTE: DispatchCalcLODViewDataGPUDriven and DispatchUpdateDrawArgs have been removed
	// in the unified approach. LOD splats are now processed by DispatchCalcViewData.

	//----------------------------------------------------------------------
	// Splat Compaction (GPU-driven work reduction)
	//----------------------------------------------------------------------

	/**
	 * Dispatch the splat compaction compute shader
	 * Builds a compact list of visible splat indices using atomics
	 */
	static void DispatchCompactSplats(
		FRHICommandListImmediate& RHICmdList,
		FGaussianSplatGPUResources* GPUResources,
		int32 TotalSplatCount,
		int32 OriginalSplatCount,
		bool bUseLODRendering
	);

	/**
	 * Dispatch the prepare indirect args compute shader
	 * Prepares indirect dispatch and draw arguments from visible splat count
	 */
	static void DispatchPrepareIndirectArgs(
		FRHICommandListImmediate& RHICmdList,
		FGaussianSplatGPUResources* GPUResources
	);

	/**
	 * Dispatch CalcViewData with compaction (indirect dispatch)
	 * Only processes visible splats from compacted list
	 */
	static void DispatchCalcViewDataCompacted(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FGaussianSplatGPUResources* GPUResources,
		const FMatrix& LocalToWorld,
		int32 SplatCount,
		int32 OriginalSplatCount,
		int32 SHOrder,
		float OpacityScale,
		float SplatScale,
		const FGaussianSplatLightingData* LightingData = nullptr,
		const FGaussianCapturedDepthShadowRenderData* CapturedDepthData = nullptr
	);

	/**
	 * Dispatch CalcDistances with indirect dispatch
	 * Only processes visible splats
	 */
	static void DispatchCalcDistancesIndirect(
		FRHICommandListImmediate& RHICmdList,
		FGaussianSplatGPUResources* GPUResources
	);

	//----------------------------------------------------------------------
	// Global Accumulator dispatch (one-draw-call path)
	//----------------------------------------------------------------------

	/**
	 * Dispatch CalcViewData writing into GlobalAccumulator->GlobalViewDataBuffer
	 * at GlobalBaseOffset, instead of the per-proxy ViewDataBuffer.
	 */
	/** v2886: purely observational GPU sampling; off by default. */
	static void TraceShadowViewDataSamples(
		FRHICommandListImmediate& RHICmdList,
		FGaussianGlobalAccumulator* GlobalAccumulator,
		uint32 TotalSplatCount,
		uint64 RenderFrameNumber,
		int32 ShadowDebugMode);

	static void DispatchCalcViewDataGlobal(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FGaussianSplatGPUResources* GPUResources,
		const FMatrix& LocalToWorld,
		int32 SplatCount,
		int32 SHOrder,
		float OpacityScale,
		float SplatScale,
		bool bUseLODRendering,
		uint32 GlobalBaseOffset,
		FGaussianGlobalAccumulator* GlobalAccumulator,
		const FGaussianSplatLightingData* LightingData = nullptr,
		const FGaussianCapturedDepthShadowRenderData* CapturedDepthData = nullptr
	);

	/**
	 * Dispatch CalcDistances over the full global ViewDataBuffer.
	 * Must be called after all Phase-1 CalcViewData dispatches.
	 */
	static void DispatchCalcDistancesGlobal(
		FRHICommandListImmediate& RHICmdList,
		FGaussianGlobalAccumulator* GlobalAccumulator,
		int32 TotalSplatCount
	);


	/**
	 * Dispatch radix sort over the full global distance/key buffers.
	 */
	static void DispatchRadixSortGlobal(
		FRHICommandListImmediate& RHICmdList,
		FGaussianGlobalAccumulator* GlobalAccumulator,
		int32 TotalSplatCount
	);

	/**
	 * Draw all splats using global sorted keys and ViewData.
	 * Borrows the IndexBuffer from the first valid proxy.
	 */
	static void DrawSplatsGlobal(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FGaussianGlobalAccumulator* GlobalAccumulator,
		FBufferRHIRef IndexBuffer,
		int32 TotalSplatCount,
		int32 DebugMode,
		int32 LightingPreviewMode,
		int32 RenderQualityMode,
		bool bUseProxyLightingMaskAsMain,
		float ProxyLightingIntensityScale,
		float DayNightBlend,
		const FVector4f& RegionalColorGradeParams0,
		const FVector4f& RegionalColorGradeParams1,
		float StableShadowColorRetention,
		int32 VRDiagnosticMode = 0,
        bool bPCVRLeanOutputs = false,
        FRHITexture* TileSceneDepth = nullptr
	);

	/** v2785: draw the same sorted splats into a separate snow-only accumulation target. */
	static void DrawSnowSplatsGlobal(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FGaussianGlobalAccumulator* GlobalAccumulator,
		FBufferRHIRef IndexBuffer,
		int32 TotalSplatCount
	);

	//----------------------------------------------------------------------
	// Global Accumulator + Nanite Compaction dispatch (one-draw-call path)
	// Phase sequence: GatherVisibleCount × N ??PrefixSumVisibleCounts ×1 ??	//   CalcViewDataCompactedGlobal × N ??CalcDistancesGlobalIndirect ×1 ??	//   RadixSortGlobalIndirect ×1 ??DrawSplatsGlobalIndirect ×1
	//----------------------------------------------------------------------

	/**
	 * Copy GPUResources->VisibleSplatCountBuffer[0] into
	 * GlobalAccumulator->GlobalVisibleCountArrayBuffer[ProxyIndex].
	 * Dispatch: (1,1,1) per proxy, after DispatchCompactSplats.
	 */
	static void DispatchGatherVisibleCount(
		FRHICommandListImmediate& RHICmdList,
		FGaussianSplatGPUResources* GPUResources,
		FGaussianGlobalAccumulator* GlobalAccumulator,
		int32 ProxyIndex
	);

	/**
	 * Compute exclusive prefix sums over GlobalVisibleCountArray and write
	 * all indirect dispatch/draw args for Phase-3 passes.
	 * Dispatch: (1,1,1) once, after all GatherVisibleCount dispatches.
	 */
	static void DispatchPrefixSumVisibleCounts(
		FRHICommandListImmediate& RHICmdList,
		FGaussianGlobalAccumulator* GlobalAccumulator,
		int32 ProxyCount,
		uint32 MaxRenderBudget
	);

	/**
	 * CalcViewData for proxy ProxyIndex, writing into GlobalViewDataBuffer
	 * at offset GlobalBaseOffsetsBuffer[ProxyIndex].
	 * Uses IndirectDispatchArgsBuffer from GPUResources (set by PrepareIndirectArgs).
	 */
	static void DispatchCalcViewDataCompactedGlobal(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FGaussianSplatGPUResources* GPUResources,
		const FMatrix& LocalToWorld,
		int32 SplatCount,
		int32 OriginalSplatCount,
		int32 SHOrder,
		float OpacityScale,
		float SplatScale,
		int32 ProxyIndex,
		FGaussianGlobalAccumulator* GlobalAccumulator,
		uint32 MaxRenderBudget,
		const FGaussianSplatLightingData* LightingData = nullptr,
		const FGaussianCapturedDepthShadowRenderData* CapturedDepthData = nullptr
	);

	/**
	 * CalcDistances over the global ViewDataBuffer using indirect dispatch
	 * (count from GlobalCalcDistIndirectArgsBuffer written by PrefixSumCS).
	 */
	static void DispatchCalcDistancesGlobalIndirect(
		FRHICommandListImmediate& RHICmdList,
		FGaussianGlobalAccumulator* GlobalAccumulator
	);

	/**
	 * Radix sort over the global distance/key buffers using indirect dispatch
	 * (count/numTiles from GlobalSortParamsBuffer written by PrefixSumCS).
	 */
	static void DispatchRadixSortGlobalIndirect(
		FRHICommandListImmediate& RHICmdList,
		FGaussianGlobalAccumulator* GlobalAccumulator
	);

	/**
	 * Draw all visible splats using GlobalDrawIndirectArgsBuffer
	 * (instance count written by PrefixSumCS, not a CPU constant).
	 */
	static void DrawSplatsGlobalIndirect(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FGaussianGlobalAccumulator* GlobalAccumulator,
		FBufferRHIRef IndexBuffer,
		int32 DebugMode,
		int32 LightingPreviewMode,
		int32 RenderQualityMode,
		bool bUseProxyLightingMaskAsMain,
		float ProxyLightingIntensityScale,
		float DayNightBlend,
		const FVector4f& RegionalColorGradeParams0,
		const FVector4f& RegionalColorGradeParams1,
		float StableShadowColorRetention,
		int32 VRDiagnosticMode = 0,
        bool bPCVRLeanOutputs = false,
        FRHITexture* TileSceneDepth = nullptr
	);

	/** v2785: indirect variant of the dedicated snow shell draw. */
	static void DrawSnowSplatsGlobalIndirect(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FGaussianGlobalAccumulator* GlobalAccumulator,
		FBufferRHIRef IndexBuffer
	);

	/** Build the final un-pre-exposed, post-grade HDR source used by bloom. */
	static void GradeToTexture(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FTextureRHIRef IntermediateTexture,
		FTextureRHIRef IntermediateDepthTexture,
		FTextureRHIRef SnowIntermediateTexture,
		FTextureRHIRef SnowIntermediateDepthTexture,
		FTextureRHIRef SnowOcclusionDepthTexture,
		bool bSnowOverlayEnabled,
		const FIntRect& SourceViewRect,
		const FIntRect& OutputViewRect,
		const FVector4f& CompositeLookParams0,
		const FVector4f& CompositeLookParams1,
		const FVector4f& CompositeLookParams2,
		const FVector4f& CompositeLookParams3,
		const FVector4f& CompositeLookParams4
	);

	/** Extract post-grade spatial bloom and blend the result onto HDR SceneColor. */
	static void CompositeToSceneColor(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FTextureRHIRef GradedTexture,
		FTextureRHIRef RawFogCoverageTexture,
		FTextureRHIRef IntermediateDepthTexture,
		FTextureRHIRef IntermediateFogDepthTexture,
		const FIntRect& SourceViewRect,
		const FIntRect& OutputViewRect,
		const FVector4f& CompositeBloomParams,
		const FVector4f& CompositeColorBloomParams,
		const FVector4f& CompositeBloomTint,
		float CompositeBloomSoftKnee,
		uint32 PanoramaGSCompositeMode,
		float PanoramaGSToneMapEV,
        FRHITexture* BloomExtractTexture = nullptr
	);

	static bool IsDirectWetSurfaceEnabled_RenderThread();
    static bool IsPCVRClearSurface_RenderThread();
	static void ApplyWetSurfaceToTexture(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FTextureRHIRef SourceTexture,
		FTextureRHIRef DepthTexture,
		FTextureRHIRef NormalTexture,
		FTextureRHIRef SceneColorTexture,
		const FIntRect& SourceViewRect,
		const FIntRect& SceneColorViewRect,
		const FIntRect& OutputViewRect,
		float SceneColorInvPreExposure);

private:
	/** Calculate next power of 2 */
	static uint32 NextPowerOfTwo(uint32 Value);

	/**
	 * Extract frustum planes from view-projection matrix
	 * Planes are in world space, normalized with normal pointing inward
	 * Order: Left, Right, Bottom, Top, Near, Far
	 */
	static void ExtractFrustumPlanes(const FMatrix& ViewProjection, FVector4f OutPlanes[6]);
};
