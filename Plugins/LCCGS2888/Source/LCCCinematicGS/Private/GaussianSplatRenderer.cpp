// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatRenderer.h"
#include "GSPreviewPassCounters.h"
#include <atomic>

namespace
{
    std::atomic<bool> GGSPreviewCountEnabled{false};
    std::atomic<uint64> GGSPreviewCounts[GSPreviewPassCounters::NumPasses]{};
    std::atomic<uint64> GGSPreviewCPUCycles[GSPreviewPassCounters::NumPasses]{};
    std::atomic<uint64> GGSPreviewWork[GSPreviewPassCounters::NumPasses]{};
    std::atomic<uint64> GGSPreviewSkips[GSPreviewPassCounters::NumPasses]{};
    std::atomic<uint64> GGSPreviewRaster[GSPreviewPassCounters::RasterEyeCount][GSPreviewPassCounters::RasterMetricCount]{};
}
void GSPreviewPassCounters::SetEnabled(bool bEnabled)
{
    GGSPreviewCountEnabled.store(bEnabled, std::memory_order_release);
}
void GSPreviewPassCounters::Snapshot(uint64* OutCounts, int32 Count)
{
    if (!OutCounts) return;
    for (int32 I = 0; I < FMath::Min(Count, int32(NumPasses)); ++I)
        OutCounts[I] = GGSPreviewCounts[I].load(std::memory_order_relaxed);
}
bool GSPreviewPassCounters::IsEnabled()
{
    return GGSPreviewCountEnabled.load(std::memory_order_relaxed);
}
void GSPreviewPassCounters::AddCPUTime(EPass Pass, uint64 Cycles)
{
    GGSPreviewCPUCycles[static_cast<int32>(Pass)].fetch_add(Cycles, std::memory_order_relaxed);
}
void GSPreviewPassCounters::SnapshotCPUCycles(uint64* OutCycles, int32 Count)
{
    if (!OutCycles) return;
    for (int32 I = 0; I < FMath::Min(Count, int32(NumPasses)); ++I)
        OutCycles[I] = GGSPreviewCPUCycles[I].load(std::memory_order_relaxed);
}
void GSPreviewPassCounters::AddWork(EPass Pass, uint64 WorkItems)
{
    if (IsEnabled()) GGSPreviewWork[static_cast<int32>(Pass)].fetch_add(WorkItems, std::memory_order_relaxed);
}
void GSPreviewPassCounters::SnapshotWork(uint64* OutWork, int32 Count)
{
    if (!OutWork) return;
    for (int32 I=0; I<FMath::Min(Count,int32(NumPasses)); ++I)
        OutWork[I]=GGSPreviewWork[I].load(std::memory_order_relaxed);
}
void GSPreviewPassCounters::MarkSkip(EPass Pass)
{
    if (IsEnabled()) GGSPreviewSkips[static_cast<int32>(Pass)].fetch_add(1,std::memory_order_relaxed);
}
void GSPreviewPassCounters::SnapshotSkips(uint64* OutSkips, int32 Count)
{
    if (!OutSkips) return;
    for (int32 I=0; I<FMath::Min(Count,int32(NumPasses)); ++I)
        OutSkips[I]=GGSPreviewSkips[I].load(std::memory_order_relaxed);
}
void GSPreviewPassCounters::Hit(EPass Pass)
{
    if (GGSPreviewCountEnabled.load(std::memory_order_relaxed))
        GGSPreviewCounts[static_cast<int32>(Pass)].fetch_add(1, std::memory_order_relaxed);
}

void GSPreviewPassCounters::RecordRasterDraw(int32 EyeIndex, uint32 SourceCapacity, uint64 ViewportPixels)
{
    if (!IsEnabled() || EyeIndex < 0 || EyeIndex >= RasterEyeCount) return;
    GGSPreviewRaster[EyeIndex][0].fetch_add(1, std::memory_order_relaxed);
    GGSPreviewRaster[EyeIndex][1].fetch_add(SourceCapacity, std::memory_order_relaxed);
    GGSPreviewRaster[EyeIndex][2].fetch_add(ViewportPixels, std::memory_order_relaxed);
}
void GSPreviewPassCounters::RecordRasterReadback(int32 EyeIndex, uint32 DrawnInstances)
{
    if (!IsEnabled() || EyeIndex < 0 || EyeIndex >= RasterEyeCount) return;
    GGSPreviewRaster[EyeIndex][3].fetch_add(1, std::memory_order_relaxed);
    GGSPreviewRaster[EyeIndex][4].fetch_add(DrawnInstances, std::memory_order_relaxed);
}
void GSPreviewPassCounters::SnapshotRaster(uint64 OutMetrics[RasterEyeCount][RasterMetricCount])
{
    if (!OutMetrics) return;
    for (int32 E=0; E<RasterEyeCount; ++E)
        for (int32 M=0; M<RasterMetricCount; ++M)
            OutMetrics[E][M] = GGSPreviewRaster[E][M].load(std::memory_order_relaxed);
}

#include "PCVRTileRenderer.h"
#include "GaussianPCVR.h"
#include "GSRemovalBoxRenderState.h"
#include "GaussianSplatShaders.h"
#include "GaussianSplatSceneProxy.h"
#include "GaussianGlobalAccumulator.h"
#include "GaussianClusterTypes.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "GlobalShader.h"
#include "ShaderParameterUtils.h"
#include "PipelineStateCache.h"
#include "RHIStaticStates.h"
#include "SceneView.h"
#include "StereoRendering.h"
#include "SceneRendering.h"
#include "SceneManagement.h"
#include "RenderCore.h"
#include "CommonRenderResources.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

static TAutoConsoleVariable<int32> CVarRemovalPreCull(TEXT("gs.PCVR.RemovalPreCull"),0,TEXT("Conservative full projected-quad box containment before stable compaction/sort. Boundaries keep exact pixel clipping."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarGSPCVREarlyAlpha(TEXT("gs.PCVR.EarlyAlphaBeforeLighting"),0,
    TEXT("V15-A: conservative ordinary-source alpha discard before per-pixel final relighting. 0 original, 1 opt-in."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarGSPCVRTrimFogMRT(TEXT("gs.PCVR.TrimInactiveFogMRT"),0,
    TEXT("V15-B: PCVR lean source path with inactive native GS fog uses only color+depth MRT. Does NOT turn off active fog. 0 original 3 MRT, 1 auto."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarPCVertexLighting(TEXT("gs.PC.VertexLighting"),0,
    TEXT("Move unchanged Final per-Gaussian lighting arithmetic to vertices; pixel weather/coverage and all MRTs remain. 0 reference, 1 optimized."),ECVF_RenderThreadSafe);
// V17: optional radial centre-depth reuse, avoiding repeated sqrt of the same
// centre vector for every surviving GS pixel. No shadow/weather setting is changed.
static TAutoConsoleVariable<int32> CVarPCVRReuseRadialDepth(TEXT("gs.PCVR.ReuseRadialDepth"),0,
    TEXT("V17: compute each splat centre radial depth in VS, reuse it for raster depth/fog/snow writes. 0 original pixel sqrt; 1 cached VS depth."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarPCSourceQuadTrim(TEXT("gs.PC.SourceQuadTrim"),1,
    TEXT("Final source GS only: conservative vertex quad bounds for the unchanged final alpha cutoff. Water/debug retain original bounds."),ECVF_RenderThreadSafe);
bool FGaussianSplatRenderer::IsSourceQuadTrimEnabled_RenderThread(){return CVarPCSourceQuadTrim.GetValueOnRenderThread()!=0;}
static TAutoConsoleVariable<int32> CVarGSVRDiagMode(
    TEXT("gs.VRDiagMode"), 0,
    TEXT("0 production; 1 basic PS/same MRT; 2 basic PS/2 MRT; 3 no-weather trained-SH baseline/1 MRT/conservative alpha support. Modes 0-2 unchanged."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarGSVRDiagLog(
    TEXT("gs.VRDiagLog"), 0,
    TEXT("Log view worksets and asynchronous indirect draw instance counts. 0 off, 1 on."), ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarGSPreviewIndirectSamples(
    TEXT("gs.PCVR.RecordIndirectCounts"), 0,
    TEXT("V7 profiler only: sample indirect-draw instance counts asynchronously (about once per eye every 2 sec). 0=off(default), 1=on. Extra copy/readback cost."), ECVF_RenderThreadSafe);
// Diagnostic-only eye label: use view-level stereo helpers rather than
// assuming or naming a particular EStereoscopicPass enum constant.
static int32 GSRasterEyeBucket(const FSceneView& View)
{
    if (!IStereoRendering::IsStereoEyeView(View)) return 2;
    if (IStereoRendering::IsAPrimaryView(View)) return 0;
    if (IStereoRendering::IsASecondaryView(View)) return 1;
    return 2;
}
int32 FGaussianSplatRenderer::GetVRDiagnosticMode_RenderThread()
{
    return FMath::Clamp(CVarGSVRDiagMode.GetValueOnRenderThread(), 0, 3);
}
bool FGaussianSplatRenderer::IsVRDiagnosticLogging_RenderThread()
{
    return CVarGSVRDiagLog.GetValueOnRenderThread() != 0;
}

// One nonblocking readback in flight; never synchronously wait for diagnostic counts.
static void LogVRDiagnosticIndirectCount(FRHICommandListImmediate& RHICmdList,
    FGaussianGlobalAccumulator* Accumulator, int32 Mode,int32 Eye=0,uint32 SourceLimit=0,bool PackedRoute=false)
{
    const bool bVerbose = FGaussianSplatRenderer::IsVRDiagnosticLogging_RenderThread();
    const bool bCapture = GSPreviewPassCounters::IsEnabled() && CVarGSPreviewIndirectSamples.GetValueOnRenderThread()!=0;
    if (!bVerbose && !bCapture) return;
    // Tiled counts are copied and logged by the following outside-raster RDG pass.
    const auto* TileMode=IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("gs.PCVR.TileComposite"));
    if(TileMode && TileMode->GetValueOnRenderThread()!=0)return;
    struct FCountReadbackState
    {
        TUniquePtr<FRHIGPUBufferReadback> Readback;
        bool bPending=false;
        uint64 SubmittedFrame=0;
        int32 SubmittedMode=0,SubmittedEye=0;
        uint32 SubmittedSource=0;
        bool SubmittedCompact=false,SubmittedPacked=false;
        double LastRequest=-100.0;
    };
    static TMap<int32,TUniquePtr<FCountReadbackState>> EyeReadbacks;
    auto& Entry=EyeReadbacks.FindOrAdd(Eye);
    if(!Entry) Entry=MakeUnique<FCountReadbackState>();
    auto& State=*Entry;
    if (State.bPending && State.Readback->IsReady())
    {
        const uint32* Args = static_cast<const uint32*>(State.Readback->Lock(5 * sizeof(uint32)));
        if (Args)
        {
            if (bCapture)
                GSPreviewPassCounters::RecordRasterReadback(State.SubmittedEye, Args[1]);
            if (bVerbose)
            {
                UE_LOG(LogTemp, Display, TEXT("[GSVRDiag][IndirectCount] Mode=%d SubmittedFrame=%llu Instances=%u IndicesPerInstance=%u DelayedReadback=1"),
                    State.SubmittedMode, State.SubmittedFrame, Args[1], Args[0]);
                UE_LOG(LogTemp,Display,TEXT("[GSPCVR][Counts] Eye=%d SourceLimit=%u Sorted=%u Drawn=%u PreSort=%d PackedRoute=%d SubmittedFrame=%llu DelayedReadback=1"),
                    State.SubmittedEye,State.SubmittedSource,(State.SubmittedCompact||State.SubmittedPacked)?Args[1]:State.SubmittedSource,Args[1],State.SubmittedCompact?1:0,State.SubmittedPacked?1:0,State.SubmittedFrame);
            }
            State.Readback->Unlock();
        }
        State.bPending = false;
    }
    const double Now = FPlatformTime::Seconds();
    if (!State.bPending && Now - State.LastRequest >= 2.0 && Accumulator->GlobalDrawIndirectArgsBuffer.IsValid())
    {
        if (!State.Readback) State.Readback = MakeUnique<FRHIGPUBufferReadback>(TEXT("GSVRDiagnosticCount"));
        RHICmdList.Transition(FRHITransitionInfo(Accumulator->GlobalDrawIndirectArgsBuffer, ERHIAccess::IndirectArgs, ERHIAccess::CopySrc));
        State.Readback->EnqueueCopy(RHICmdList, Accumulator->GlobalDrawIndirectArgsBuffer, 5 * sizeof(uint32));
        RHICmdList.Transition(FRHITransitionInfo(Accumulator->GlobalDrawIndirectArgsBuffer, ERHIAccess::CopySrc, ERHIAccess::IndirectArgs));
        State.bPending = true; State.SubmittedFrame = GFrameNumberRenderThread; State.SubmittedMode = Mode; State.LastRequest = Now;
        State.SubmittedEye=Eye;State.SubmittedSource=SourceLimit;State.SubmittedCompact=Accumulator->bPCVRPreSortCompacted;State.SubmittedPacked=PackedRoute;
    }
}

extern TAutoConsoleVariable<int32> CVarGSPanoramaShadowStats;
extern TAutoConsoleVariable<int32> CVarGSPanoramaShadowMaskProbe;
extern TAutoConsoleVariable<int32> CVarGSPanoramaRainGSChunkedDistances;
extern TAutoConsoleVariable<int32> CVarGSPanoramaRainGSChunkSize;
// Render-thread state, published by the existing world-weather authority.
static int32 GWetReceiverTestMode = 0;
static float GPuddleDampenStrength = 0.0f;
static FVector2f GPuddleSourceWetSettings(0.0f, 0.0f);
static bool GPuddleHideOriginalSource = false;
// P10 shared render-thread settings (used ONLY for the per-splat water marker).
static FVector4f GPuddleWaterVisualSettings(0.14f, 1.0f, 85.0f, 0.62f);
static float GPuddleWaterRippleSizeCm = 45.0f;
static float GPuddleWaterReflectionStrength = 0.85f;
static float GPuddleRoofRasterCellCm = 40.0f;
static FVector4f GPuddleRoofFXParams(1.0f, 0.0f, 1.0f, 0.9f);
void FGaussianSplatRenderer::SetPuddleWaterVisualSettings(const FVector4f& Settings, float RippleSizeCm, float ReflectionStrength, float RoofRasterCellCm, const FVector4f& RoofFXParams)
{
	GPuddleWaterVisualSettings = FVector4f(
		FMath::Clamp(Settings.X, 0.0f, 0.90f), FMath::Clamp(Settings.Y, 0.0f, 2.0f),
		FMath::Clamp(Settings.Z, 10.0f, 500.0f), FMath::Clamp(Settings.W, 0.0f, 5.0f));
	GPuddleWaterRippleSizeCm = FMath::Clamp(RippleSizeCm, 5.0f, 200.0f);
    GPuddleWaterReflectionStrength = FMath::Clamp(ReflectionStrength, 0.0f, 2.0f);
    GPuddleRoofRasterCellCm = FMath::Clamp(RoofRasterCellCm, 15.0f, 100.0f);
    GPuddleRoofFXParams = FVector4f(FMath::Clamp(RoofFXParams.X, 0.0f, 1.0f), FMath::Clamp(RoofFXParams.Y, 0.0f, 3.0f),
                                     FMath::Clamp(RoofFXParams.Z, 0.0f, 2.0f), FMath::Clamp(RoofFXParams.W, 0.0f, 3.0f));
}

void FGaussianSplatRenderer::SetPuddleDampenStrength(float Strength, bool bHideOriginalSource, const FVector2f& SourceWetSettings)
{
	GPuddleDampenStrength = FMath::Clamp(Strength, 0.0f, 0.85f);
	GPuddleHideOriginalSource = bHideOriginalSource;
	GPuddleSourceWetSettings = FVector2f(FMath::Clamp(SourceWetSettings.X, 0.0f, 1.0f),
		FMath::Clamp(SourceWetSettings.Y, 0.0f, 3.0f));
}
static bool GWetMeshPreviewRipples = false;
void FGaussianSplatRenderer::SetWetReceiverTestMode(int32 Mode, bool bPreviewRipples)
{
	GWetReceiverTestMode = FMath::Clamp(Mode, 0, 4);
	GWetMeshPreviewRipples = bPreviewRipples;
}
int32 FGaussianSplatRenderer::GetWetReceiverTestMode() { return GWetReceiverTestMode; }
// Console variables (declared in GaussianSplatting.cpp)
extern TAutoConsoleVariable<int32> CVarShowClusterBounds;
extern TAutoConsoleVariable<int32> CVarDebugForceLODLevel;

static FVector4f GProxyShadowCaptureSettings(1.5f, 0.75f, 0.12f, 1.0f);
static FVector4f GProxyShadowDetailSettings(14.0f, 0.008f, 0.72f, 1.0f);
static FVector4f GProxyShadowEvidenceSettings(0.22f, 1.0f, 1.0f, 1.0f);
static FVector4f GProxyShadowShapingSettings(0.25f, 0.85f, 1.0f, 0.65f);
static FVector4f GProxyCaptureOriginTanFOV(0, 0, 0, 1);
static FVector4f GProxyCaptureForwardAspect(1, 0, 0, 1);
static FVector4f GProxyCaptureRightEnabled(0, 1, 0, 0);
static FVector4f GProxyCaptureUp(0, 0, 1, 0);
static float GProxyShadowOcclusionBoost = 1.35f;
static FVector2f GProxyShadowColorSettings(0.72f, 0.0f);
static FVector4f GProxyReflectedLightSettings(0.26f, 0.35f, 0.78f, 0.15f);
static FVector4f GProxyReflectedColorSettings(0.70f, 0.94f, 0.35f, 0.45f);
static FVector4f GProxyShadowBaseSettings(0.70f, 0.55f, 0.85f, 1.0f);
static FVector3f GProxyShadowBaseTint(0.78f, 0.86f, 1.0f);
static TAutoConsoleVariable<int32> CVarPCVRSpecializeRemoval(TEXT("gs.PCVR.SpecializeRemoval"),1,
    TEXT("PCVR cached Final: compile away pixel box clipping only when actual scene has zero removal boxes. 0 reference."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarPCVRComputeFinalColor(TEXT("gs.PCVR.ComputeFinalColor"),0,
    TEXT("Experimental PCVR Final: evaluate shared lighting once per visible GS into float32 RGB; 0 pixel reference."),ECVF_RenderThreadSafe);
// V16-D: dispatch the existing per-visible-splat FinalColor shader using the
// GPU-generated radix sort tile count (1024 source slots per threadgroup).
// Does not merge the two eyes' SH colour/projection; each eye retains its own ViewData.
static TAutoConsoleVariable<int32> CVarPCVRFinalColorIndirect(TEXT("gs.PCVR.FinalColorIndirectDispatch"),0,
    TEXT("V16-D: use GPU visible-sort tile count for per-splat lighting dispatch. 0 fixed source-capacity dispatch; 1 indirect compact dispatch."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarPCVRFinalOnly(TEXT("gs.PCVR.SpecializedFinal"),1,
    TEXT("PCVR lean production path: compile Final equations without unreachable diagnostic/water branches. 0 reference."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarPCVRShadowBlueOverlay(TEXT("gs.PCVR.ShadowBlueOverlay"),0,
    TEXT("Diagnostic only: tint valid stable cast-shadow regions blue over the Final image. 0 normal; 1 blue."),ECVF_RenderThreadSafe);
static FTextureRHIRef GProxyLightDepthCaptureTexture;
static FTextureRHIRef GProxyReceiverLightDepthCaptureTexture;
static FVector4f GProxyLightDepthOriginWidth(0, 0, 0, 1);
static FVector4f GProxyLightDepthForwardEnabled(1, 0, 0, 0);
static FVector4f GProxyLightDepthRightRange(0, 1, 0, 1000);
static FVector4f GProxyLightDepthUpSettings(0, 0, 1, 0);
// v2772: revision belongs to the texture + projection packet above and changes only
// when the LightDepth producer performs a new capture.
static uint32 GProxyLightDepthCaptureRevision = 0u;
static FVector4f GProxyVolumetricShaftSettings(0.18f, 0.65f, 12000.0f, 0.65f);
static FVector3f GProxyVolumetricShaftTint(1.0f, 0.86f, 0.62f);
// Fog-only late-composite bridge. x=strength, y=visibility cm, z=max opacity, w=proxy-light influence.
static FVector4f GNativeFogCompositeSettings(0.0f, 6500.0f, 0.985f, 0.0f);
// rgb = the exact UE ExponentialHeightFog inscattering colour used by WeatherSkyFogComponent.
static FVector4f GNativeFogCompositeColor(0.25f, 0.30f, 0.38f, 1.0f);
// x=overall density scale, y=occluded-pocket scale, z=macro variation, w=height-falloff scale.
// Defaults are normalized around the current Fog preset, so existing Fog visuals remain unchanged.
static FVector4f GNativeFogCompositeProfile0(1.0f, 1.0f, 0.38f, 1.0f);
// x=UE native StartDistance in centimeters; y=distance-shape; z,w=near-air fractions.
static FVector4f GNativeFogCompositeProfile1(0.0f, 0.0f, 0.0f, 0.0f);
static FTextureRHIRef GProxySurfaceDepthTexture;
static FVector4f GProxySurfaceEffectSettings0(0, 0, 0.72f, 0.12f);
static FVector4f GProxySurfaceEffectSettings1(0.48f, 0.64f, 0.78f, 0.75f);
static FVector4f GDirectSplatSurfaceEffectSettings0(0, 0, 0.72f, 0.12f);
// Raw weather-selected surface mode before strength gating: 0 other, 1 Rain/WetPuddles, 2 Snow.
static float GDirectWeatherSurfaceMode = 0.0f;
static FVector4f GDirectSplatSurfaceEffectSettings1(0.82f, 0.90f, 1.0f, 0.75f);
static FVector4f GDirectSplatSurfaceEffectSettings2(4.0f, 1.10f, 0, 0);
static FVector4f GDirectWetRippleSettings(1.0f, 45.0f, 85.0f, 0.62f);
static FVector4f GDirectSnowTuningSettings(0.72f, 0.68f, 0.70f, 0.0f);
// v2872 x=matched-source-cover boost, y/z/w reserved; renderer-only snow path.
static FVector4f GSnowSourceCoverSettings(0.0f, 0.0f, 0.0f, 0.0f);
// Track the exact global effect inputs consumed by CalcViewData. Colour/opacity
// refreshes do not alter its projected depth or clip.w membership in modes 0-2.
uint32 FGaussianSplatRenderer::GetViewDataEffectsHash_RenderThread()
{
    uint32 H=GetTypeHash(GDirectSplatSurfaceEffectSettings0);
    H=HashCombine(H,GetTypeHash(GDirectSplatSurfaceEffectSettings1));
    H=HashCombine(H,GetTypeHash(GDirectSplatSurfaceEffectSettings2));
    H=HashCombine(H,GetTypeHash(GDirectSnowTuningSettings));
    H=HashCombine(H,GetTypeHash(GSnowSourceCoverSettings));
    H=HashCombine(H,GetTypeHash(GPuddleDampenStrength));
    H=HashCombine(H,GetTypeHash(GPuddleSourceWetSettings));
    return H;
}
uint32 FGaussianSplatRenderer::GetVisibilityEffectsHash_RenderThread(const FGaussianSplatGPUResources* R)
{
    // Mirror the two source-hiding branches before projection in ProcessSplat.
    const bool HidePuddle=R && R->bPuddleMainSource && GPuddleHideOriginalSource;
    const bool HideSnow=R && R->SnowCoveragePackedCount>0 && GDirectSplatSurfaceEffectSettings0.X>1.5f &&
        GDirectSnowTuningSettings.W>=1.0f && GDirectSplatSurfaceEffectSettings2.Z>2.5f;
    return (HidePuddle?1u:0u)|(HideSnow?2u:0u);
}

// Rain-only post-grade overlay: x strength, y exposure EV, z gamma, w saturation.
static FVector4f GDirectRainOverlaySettings0(0.0f, 0.0f, 1.0f, 1.0f);
// rgb tint, w tint strength.
static FVector4f GDirectRainOverlaySettings1(1.0f, 1.0f, 1.0f, 0.0f);
// v32: runtime-only Rain state; never serialized into Clear/Fog/Snow presets.
static bool GDirectRainModeActive = false;
static bool GDirectRainPrecipitationActive = false;
// v31: runtime-only Day/Night moon payload for the dedicated Wet pass.
// xyz = surface-to-moon world direction, w = angular diameter in degrees.
static FVector4f GDirectMoonReflectionDirectionSize(0.0f, 0.0f, 1.0f, 0.7357f);
// rgb = authored moon color, w = 0..1 visibility/energy blend.
static FVector4f GDirectMoonReflectionColorStrength(0.92f, 0.97f, 1.04f, 0.0f);
static FVector4f GProxySurfaceOriginTanFOV(0, 0, 0, 1);
static FVector4f GProxySurfaceForwardAspect(1, 0, 0, 1);
static FVector4f GProxySurfaceRightEnabled(0, 1, 0, 0);
static FVector4f GProxySurfaceUp(0, 0, 1, 0);
// Isolated actor debug selector. It is written and read only on the render thread and
// never participates in SceneProxy creation, LOD, sort validity, or component visibility.
static int32 GStableShadowDebugMode = 0;

static TAutoConsoleVariable<int32> CVarStableVertexFlipLog(
	TEXT("gs.StableVertexFlipLog"),
	0,
	TEXT("Read back previous/current welded proxy vertex proofs and append only visibility flips.\n")
	TEXT(" 0: Off (default, no GPU readback)\n")
	TEXT(" 1: Append Saved/Diagnostics/GS_StableVertexFlips.csv"),
	ECVF_Default);

static void RecordStableVertexFlips(
	FRHICommandListImmediate& RHICmdList,
	FGaussianSplatGPUResources* GPUResources,
	float DepthBiasCm)
{
	if (CVarStableVertexFlipLog.GetValueOnRenderThread() == 0 || !GPUResources ||
		GPUResources->StableProxyVertexCount == 0u ||
		!GPUResources->StableProxyVertexProofBuffer.IsValid() ||
		!GPUResources->StableProxyVertexProofTargetBuffer.IsValid() ||
		GPUResources->StableProxyFieldFrontCaptureRevision == 0u)
	{
		return;
	}

	const uint32 VertexCount = GPUResources->StableProxyVertexCount;
	const uint32 NumBytes = VertexCount * sizeof(FVector4f);
	FRHIGPUBufferReadback PreviousReadback(TEXT("GSStableVertexPrevious"));
	FRHIGPUBufferReadback CurrentReadback(TEXT("GSStableVertexCurrent"));
	PreviousReadback.EnqueueCopy(RHICmdList, GPUResources->StableProxyVertexProofBuffer, NumBytes);
	CurrentReadback.EnqueueCopy(RHICmdList, GPUResources->StableProxyVertexProofTargetBuffer, NumBytes);
	RHICmdList.SubmitCommandsAndFlushGPU();
	RHICmdList.BlockUntilGPUIdle();

	if (!PreviousReadback.IsReady() || !CurrentReadback.IsReady())
	{
		UE_LOG(LogTemp, Warning, TEXT("[GSStableVertexFlip] ReadbackNotReady Rev=%u"), GProxyLightDepthCaptureRevision);
		return;
	}

	const FVector4f* Previous = static_cast<const FVector4f*>(PreviousReadback.Lock(NumBytes));
	const FVector4f* Current = static_cast<const FVector4f*>(CurrentReadback.Lock(NumBytes));
	if (!Previous || !Current)
	{
		if (Previous) PreviousReadback.Unlock();
		if (Current) CurrentReadback.Unlock();
		return;
	}

	FString Rows;
	int32 FlipCount = 0;
	for (uint32 VertexId = 0; VertexId < VertexCount; ++VertexId)
	{
		const FVector4f& A = Previous[VertexId];
		const FVector4f& B = Current[VertexId];
		const float PreviousSelfDepth = A.W;
		const float CurrentSelfDepth = B.W;
		const bool bPreviousLit = A.Y >= 0.5f;
		const bool bCurrentLit = B.Y >= 0.5f;
		if (bPreviousLit == bCurrentLit)
		{
			continue;
		}

		const float PreviousDeltaCm = PreviousSelfDepth + DepthBiasCm - A.Z;
		const float CurrentDeltaCm = CurrentSelfDepth + DepthBiasCm - B.Z;
		Rows += FString::Printf(
			TEXT("%u,%u,%u,%.9f,%.9f,%.9f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d\n"),
			GProxyLightDepthCaptureRevision,
			GPUResources->StableProxyFieldFrontCaptureRevision,
			VertexId,
			GProxyLightDepthForwardEnabled.X, GProxyLightDepthForwardEnabled.Y, GProxyLightDepthForwardEnabled.Z,
			A.Y, B.Y, A.Z, B.Z, PreviousDeltaCm, CurrentDeltaCm,
			bPreviousLit ? 1 : 0, bCurrentLit ? 1 : 0);
		++FlipCount;
	}

	PreviousReadback.Unlock();
	CurrentReadback.Unlock();

	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Diagnostics"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString CsvPath = FPaths::Combine(Directory, TEXT("GS_StableVertexFlips.csv"));
	if (!IFileManager::Get().FileExists(*CsvPath))
	{
		Rows = TEXT("current_revision,previous_revision,vertex_id,light_forward_x,light_forward_y,light_forward_z,previous_visibility,current_visibility,previous_receiver_depth,current_receiver_depth,previous_self_delta_cm,current_self_delta_cm,previous_lit,current_lit\n") + Rows;
	}
	if (!Rows.IsEmpty())
	{
		FFileHelper::SaveStringToFile(Rows, *CsvPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(), FILEWRITE_Append);
	}
	UE_LOG(LogTemp, Log,
		TEXT("[GSStableVertexFlip] CurrentRev=%u PreviousRev=%u Vertices=%u Flips=%d Csv=%s"),
		GProxyLightDepthCaptureRevision, GPUResources->StableProxyFieldFrontCaptureRevision,
		VertexCount, FlipCount, *CsvPath);
}

void FGaussianSplatRenderer::SetProxyShadowCaptureSettings(const FVector4f& Settings)
{
	GProxyShadowCaptureSettings = Settings;
}

void FGaussianSplatRenderer::SetProxyShadowDetailSettings(const FVector4f& Settings)
{
	GProxyShadowDetailSettings = FVector4f(
		FMath::Clamp(Settings.X, 1.0f, 100.0f),
		FMath::Clamp(Settings.Y, 0.0f, 0.05f),
		FMath::Clamp(Settings.Z, 0.0f, 1.0f),
		FMath::Clamp(Settings.W, 0.0f, 1.0f));
}

void FGaussianSplatRenderer::SetProxyShadowEvidenceSettings(
	float FullBlackReceiverLevel,
	float LightingMatchStrength,
	float ClosureAreaScale,
	float ClosureEdgeSoftness)
{
	GProxyShadowEvidenceSettings = FVector4f(
		FMath::Clamp(FullBlackReceiverLevel, 0.0f, 0.95f),
		FMath::Clamp(LightingMatchStrength, 0.0f, 1.0f),
		FMath::Clamp(ClosureAreaScale, 0.0f, 2.5f),
		FMath::Clamp(ClosureEdgeSoftness, 0.25f, 3.0f));
}

void FGaussianSplatRenderer::SetProxyShadowCaptureProjection(
	const FVector3f& Origin, const FVector3f& Forward,
	const FVector3f& Right, const FVector3f& Up,
	float HorizontalFOVDegrees, float AspectRatio)
{
	GProxyCaptureOriginTanFOV = FVector4f(Origin, FMath::Tan(FMath::DegreesToRadians(HorizontalFOVDegrees * 0.5f)));
	GProxyCaptureForwardAspect = FVector4f(Forward, FMath::Max(AspectRatio, 0.001f));
	GProxyCaptureRightEnabled = FVector4f(Right, 1.0f);
	GProxyCaptureUp = FVector4f(Up, 0.0f);
}

void FGaussianSplatRenderer::SetStableShadowDebugMode(int32 DebugMode)
{
	if (GStableShadowDebugMode != DebugMode)
	{
		UE_LOG(LogTemp, Log, TEXT("[GSStableDebugRT] Mode %d -> %d"), GStableShadowDebugMode, DebugMode);
	}
	GStableShadowDebugMode = DebugMode;
}

int32 FGaussianSplatRenderer::GetStableShadowDebugMode()
{
	return GStableShadowDebugMode;
}

int32 FGaussianSplatRenderer::ResolveStableShadowDebugLightingPreviewMode(int32 ExistingLightingPreviewMode)
{
	switch (GStableShadowDebugMode)
	{
	case 0:  return ExistingLightingPreviewMode;
	case 72: return 43; // stabilized energy-preserving Stable Proxy Lighting
	case 73: return ExistingLightingPreviewMode; // real UE-lit proxy owns the main pass
	case 79: return 43; // pure Stable proxy N.L diagnostic, carried through DirectColor
	case 80: return 43; // stable-normal attachment diagnostic, carried through DirectColor
	case 81: return 43; // forced bound barycentric proxy N.L diagnostic
	case 82: return 43; // multi-triangle smooth proxy N.L diagnostic, carried through DirectColor
	case 102: return 43; // raw CPU-BVH ray visibility
	case 103: return 43; // current smoothed/EMA Stable ray visibility
	case 104: return 43; // final cast visibility after shaping
	default: return ExistingLightingPreviewMode;
	}
}

void FGaussianSplatRenderer::SetProxyShadowShapingSettings(const FVector4f& Settings)
{
	GProxyShadowShapingSettings = FVector4f(
		FMath::Clamp(Settings.X, 0.01f, 4.0f),
		FMath::Clamp(Settings.Y, 0.25f, 4.0f),
		FMath::Clamp(Settings.Z, 0.0f, 1.0f),
		FMath::Clamp(Settings.W, 0.05f, 1.0f));
}

void FGaussianSplatRenderer::SetProxyShadowOcclusionBoost(float Boost)
{
	// The Details panel owns the authored range. Do not silently clamp the value
	// to 1 here: values above 1 intentionally produce deeper closed shadows.
	GProxyShadowOcclusionBoost = FMath::Max(Boost, 0.0f);
}

void FGaussianSplatRenderer::SetProxyShadowColorSettings(float Saturation, bool bEnableShadowLighting)
{
	GProxyShadowColorSettings = FVector2f(
		FMath::Clamp(Saturation, 0.0f, 1.0f),
		bEnableShadowLighting ? 1.0f : 0.0f);
}


void FGaussianSplatRenderer::SetProxyReflectedLightSettings(const FVector4f& Settings)
{
	GProxyReflectedLightSettings = FVector4f(
		FMath::Clamp(Settings.X, 0.0f, 1.0f),
		FMath::Clamp(Settings.Y, 0.0f, 1.0f),
		FMath::Clamp(Settings.Z, 0.25f, 4.0f),
		FMath::Clamp(Settings.W, 0.0f, 1.0f));
}

void FGaussianSplatRenderer::SetProxyReflectedColorSettings(const FVector4f& Settings)
{
	GProxyReflectedColorSettings = FVector4f(
		FMath::Clamp(Settings.X, 0.0f, 1.0f),
		FMath::Clamp(Settings.Y, 0.0f, 1.0f),
		FMath::Clamp(Settings.Z, 0.0f, 2.0f),
		FMath::Clamp(Settings.W, 0.05f, 1.0f));
}

void FGaussianSplatRenderer::SetProxyShadowFinalCompositeSettings(const FVector4f& Settings, const FVector3f& Tint)
{
	GProxyShadowBaseSettings = FVector4f(
		FMath::Clamp(Settings.X, 0.0f, 1.0f),
		FMath::Clamp(Settings.Y, 0.0f, 1.0f),
		FMath::Clamp(Settings.Z, 0.0f, 1.0f),
		Settings.W > 0.5f ? 1.0f : 0.0f);
	GProxyShadowBaseTint = FVector3f(
		FMath::Max(Tint.X, 0.0f),
		FMath::Max(Tint.Y, 0.0f),
		FMath::Max(Tint.Z, 0.0f));
}

void FGaussianSplatRenderer::SetProxyVolumetricShaftSettings(
	const FVector4f& Settings,
	const FVector3f& Tint)
{
	GProxyVolumetricShaftSettings = FVector4f(
		FMath::Clamp(Settings.X, 0.0f, 2.0f),
		FMath::Clamp(Settings.Y, 0.0f, 2.0f),
		FMath::Clamp(Settings.Z, 100.0f, 50000.0f),
		FMath::Clamp(Settings.W, 0.0f, 1.0f));
	GProxyVolumetricShaftTint = FVector3f(
		FMath::Max(Tint.X, 0.0f),
		FMath::Max(Tint.Y, 0.0f),
		FMath::Max(Tint.Z, 0.0f));
}

void FGaussianSplatRenderer::SetNativeFogCompositeSettings(
	const FVector4f& Settings,
	const FVector4f& Color,
	const FVector4f& Profile0,
	const FVector4f& Profile1)
{
	GNativeFogCompositeSettings = FVector4f(
		FMath::Clamp(Settings.X, 0.0f, 1.0f),
		FMath::Clamp(Settings.Y, 0.0f, 64.0f),
		FMath::Clamp(Settings.Z, 0.0f, 1.0f),
		FMath::Clamp(Settings.W, 0.001f, 2.0f));
	GNativeFogCompositeColor = FVector4f(
		FMath::Max(Color.X, 0.0f),
		FMath::Max(Color.Y, 0.0f),
		FMath::Max(Color.Z, 0.0f), 1.0f);
	GNativeFogCompositeProfile0 = FVector4f(
		FMath::Clamp(Profile0.X, 0.0f, 1000000.0f),
		Profile0.Y, 0.0f, 0.0f);
	// P48: no opaque veil. Keep reserved parameters cleared.
	GNativeFogCompositeProfile1 = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
}


bool FGaussianSplatRenderer::IsNativeFogCompositeEnabled_RenderThread()
{
	return GNativeFogCompositeSettings.X > 0.0001f;
}

void FGaussianSplatRenderer::SetProxySurfaceEffect(
	FTextureRHIRef DepthTexture,
	const FVector4f& Settings0,
	const FVector4f& Settings1,
	const FVector4f& Settings2,
	const FVector3f& Origin,
	const FVector3f& Forward,
	const FVector3f& Right,
	const FVector3f& Up,
	float HorizontalFOVDegrees,
	float AspectRatio)
{
	// Legacy camera-depth publication is retained only for shadow diagnostics.
	// Weather is published independently by SetDirectSurfaceEffectSettings.
	GProxySurfaceDepthTexture = DepthTexture;
	GProxySurfaceEffectSettings0 = FVector4f(0, 0, Settings0.Z, Settings0.W);
	GProxySurfaceEffectSettings1 = Settings1;
	GProxySurfaceOriginTanFOV = FVector4f(
		Origin, FMath::Tan(FMath::DegreesToRadians(HorizontalFOVDegrees * 0.5f)));
	GProxySurfaceForwardAspect = FVector4f(Forward, FMath::Max(AspectRatio, 0.001f));
	GProxySurfaceRightEnabled = FVector4f(Right, DepthTexture.IsValid() ? 1.0f : 0.0f);
	GProxySurfaceUp = FVector4f(Up, 0.0f);
}

void FGaussianSplatRenderer::SetDirectSurfaceEffectSettings(
	const FVector4f& Settings0,
	const FVector4f& Settings1,
	const FVector4f& Settings2,
	const FVector4f& Settings3)
{
	// Preserve the selected weather mode even if the authored effect strength is zero.
	// This state is render-thread-only and does not change any visual parameter.
	GDirectWeatherSurfaceMode = Settings0.X;
	const bool bEnabled = Settings0.X > 0.5f && Settings0.Y > 0.0001f;
	GDirectSplatSurfaceEffectSettings0 = bEnabled
		? Settings0
		: FVector4f(0, 0, Settings0.Z, Settings0.W);
	GDirectSplatSurfaceEffectSettings1 = Settings1;
	GDirectSplatSurfaceEffectSettings2 = Settings2;
	GDirectWetRippleSettings = Settings3;
}

void FGaussianSplatRenderer::SetDirectSnowTuningSettings(const FVector4f& Settings)
{
	GDirectSnowTuningSettings = FVector4f(
		FMath::Clamp(Settings.X, 0.0f, 1.0f),
		FMath::Clamp(Settings.Y, 0.0f, 1.0f),
		FMath::Clamp(Settings.Z, 0.0f, 1.0f),
		// v2838: previously this fourth component was overwritten to ZERO here,
		// so the hybrid selection/sky flag never reached CalcViewData.
		// 0: legacy snow overlay; 1..1.5: hybrid without sky culling;
		// 2..2.5: hybrid with sky culling. Fraction carries live tint strength.
		FMath::Clamp(Settings.W, 0.0f, 2.5f));
}

void FGaussianSplatRenderer::SetSnowSourceCoverSettings(const FVector4f& Settings)
{
	GSnowSourceCoverSettings = FVector4f(FMath::Clamp(Settings.X,0.0f,1.0f),0.0f,0.0f,0.0f);
}

void FGaussianSplatRenderer::SetDirectRainOverlaySettings(
	const FVector4f& Settings0, const FVector4f& Settings1)
{
	GDirectRainOverlaySettings0 = Settings0;
	GDirectRainOverlaySettings1 = Settings1;
}

void FGaussianSplatRenderer::SetDirectRainRuntimeState(bool bRainModeActive, bool bPrecipitationActive)
{
	GDirectRainModeActive = bRainModeActive;
	GDirectRainPrecipitationActive = bRainModeActive && bPrecipitationActive;
}

void FGaussianSplatRenderer::SetDirectMoonReflectionState(
	const FVector3f& SurfaceToMoonDirection,
	const FVector3f& MoonColor,
	float Strength,
	float AngularDiameterDegrees,
	bool bEnabled)
{
	FVector3f Direction = SurfaceToMoonDirection.GetSafeNormal();
	if (Direction.IsNearlyZero())
	{
		Direction = FVector3f(0.0f, 0.0f, 1.0f);
	}

	GDirectMoonReflectionDirectionSize = FVector4f(
		Direction, FMath::Clamp(AngularDiameterDegrees, 0.10f, 8.0f));
	GDirectMoonReflectionColorStrength = FVector4f(
		FMath::Max(MoonColor.X, 0.0f),
		FMath::Max(MoonColor.Y, 0.0f),
		FMath::Max(MoonColor.Z, 0.0f),
		bEnabled ? FMath::Clamp(Strength, 0.0f, 1.0f) : 0.0f);
}

bool FGaussianSplatRenderer::IsDirectSnowOverlayEnabled_RenderThread()
{
	return
		GDirectSplatSurfaceEffectSettings0.X > 1.5f &&
		GDirectSplatSurfaceEffectSettings0.Y > 0.0001f &&
		// Source GS is the base snow surface when baked relief is enabled.
		// Never composite a second legacy camera-space snow seed layer over it.
		GDirectSnowTuningSettings.W < 0.5f;
}

bool FGaussianSplatRenderer::IsPCVRClearSurface_RenderThread()
{ return GDirectSplatSurfaceEffectSettings0.X < 0.5f; }

bool FGaussianSplatRenderer::IsDirectWetSurfaceEnabled_RenderThread()
{
	return GDirectSplatSurfaceEffectSettings0.X > 0.5f &&
		GDirectSplatSurfaceEffectSettings0.X < 1.5f &&
		GDirectSplatSurfaceEffectSettings0.Y > 0.0001f;
}

void FGaussianSplatRenderer::SetProxyLightDepthCapture(
	FTextureRHIRef DepthTexture,
	FTextureRHIRef ReceiverDepthTexture,
	const FVector3f& Origin,
	const FVector3f& Forward,
	const FVector3f& Right,
	const FVector3f& Up,
	const FVector4f& Settings,
	uint32 CaptureRevision)
{
	GProxyLightDepthCaptureTexture = DepthTexture;
	GProxyReceiverLightDepthCaptureTexture = ReceiverDepthTexture;
	GProxyLightDepthOriginWidth = FVector4f(Origin, FMath::Max(Settings.X, 1.0f));
	GProxyLightDepthForwardEnabled = FVector4f(Forward, DepthTexture.IsValid() ? 1.0f : 0.0f);
	GProxyLightDepthRightRange = FVector4f(Right, FMath::Max(Settings.Y, 1.0f));
	GProxyLightDepthUpSettings = FVector4f(Up, 0.0f);
	GProxyLightDepthUpSettings.W = Settings.Z;
	// Settings.W is the distance-softness multiplier; store it in OriginWidth.w's
	// companion channel by packing it into the enabled vector's w only when valid.
	GProxyLightDepthForwardEnabled.W = DepthTexture.IsValid() ? Settings.W : -1.0f;
	GProxyLightDepthCaptureRevision = DepthTexture.IsValid() ? CaptureRevision : 0u;
}

uint32 FGaussianSplatRenderer::GetProxyLightDepthCaptureRevision_RenderThread()
{
	return GProxyLightDepthCaptureRevision;
}

static bool BuildReceiverTriangleIdGrid(
	FRHICommandListImmediate& RHICmdList,
	FGaussianSplatGPUResources* GPUResources,
	const FMatrix& LocalToWorld)
{
	if (!GPUResources || !GPUResources->StableProxyReceiverTriangleIdBufferUAV.IsValid() ||
		!GPUResources->StableProxyVertexBufferSRV.IsValid() ||
		!GPUResources->StableProxyTriangleBufferSRV.IsValid() ||
		!GProxyReceiverLightDepthCaptureTexture.IsValid())
	{
		return false;
	}
	const FRHITextureDesc& Desc = GProxyReceiverLightDepthCaptureTexture->GetDesc();
	const uint32 Width = Desc.Extent.X;
	const uint32 Height = Desc.Extent.Y;
	if (Width == 0u || Height == 0u || Width > 2048u || Height > 2048u)
	{
		return false;
	}
	// v2802 safety: deterministic-BVH mode intentionally keeps only 1x1 dummy
	// legacy grids.  If any stale/queued legacy request reaches this function
	// during MRQ/Panorama startup, never dispatch a 2048x2048 compute pass into
	// that dummy UAV.  The previous code had no capacity check here and could
	// write out of bounds, causing D3D12 GPU device removal / GPU crash dumps.
	if (Width > GPUResources->StableProxyCastGridWidth ||
		Height > GPUResources->StableProxyCastGridHeight)
	{
		return false;
	}
	TShaderMapRef<FBuildStableProxyReceiverTriangleIdCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	if (!Shader.IsValid()) return false;
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->StableProxyReceiverTriangleIdBuffer,
		ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	auto DispatchPass = [&](uint32 PassMode, uint32 ItemCount)
	{
		FBuildStableProxyReceiverTriangleIdCS::FParameters P;
		P.ReceiverLightDepth = GProxyReceiverLightDepthCaptureTexture;
		P.StableProxyVertexBuffer = GPUResources->StableProxyVertexBufferSRV;
		P.StableProxyTriangleBuffer = GPUResources->StableProxyTriangleBufferSRV;
		P.OutReceiverTriangleId = GPUResources->StableProxyReceiverTriangleIdBufferUAV;
		P.LocalToWorld = FMatrix44f(LocalToWorld);
		P.LightOriginWidth = GProxyLightDepthOriginWidth;
		P.LightForwardEnabled = GProxyLightDepthForwardEnabled;
		P.LightRightRange = GProxyLightDepthRightRange;
		P.LightUpSettings = GProxyLightDepthUpSettings;
		P.VertexCount = GPUResources->StableProxyVertexCount;
		P.TriangleCount = GPUResources->StableProxyTriangleCount;
		P.GridWidth = Width;
		P.GridHeight = Height;
		P.DepthMatchToleranceCm = FMath::Clamp(GProxyLightDepthOriginWidth.W / FMath::Max(float(Width), 1.0f) * 1.25f, 0.75f, 12.0f);
		P.PassMode = PassMode;
		SetComputePipelineState(RHICmdList, Shader.GetComputeShader());
		SetShaderParameters(RHICmdList, Shader, Shader.GetComputeShader(), P);
		RHICmdList.DispatchComputeShader(FMath::DivideAndRoundUp(ItemCount, 64u), 1u, 1u);
		UnsetShaderUAVs(RHICmdList, Shader, Shader.GetComputeShader());
	};
	DispatchPass(0u, Width * Height);
	DispatchPass(1u, GPUResources->StableProxyTriangleCount);
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->StableProxyReceiverTriangleIdBuffer,
		ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
	return true;
}

bool FGaussianSplatRenderer::DispatchBuildStableProxyVertexVisibility(
	FRHICommandListImmediate& RHICmdList,
	FGaussianSplatGPUResources* GPUResources,
	const FMatrix& LocalToWorld,
	float DepthBiasCm,
	bool bCommitToFront)
{
	if (!GPUResources || GPUResources->StableProxyVertexCount == 0u ||
		!GPUResources->StableProxyVertexBufferSRV.IsValid() ||
		!GPUResources->StableProxyVertexProofTargetBuffer.IsValid() ||
		!GPUResources->StableProxyVertexProofTargetBufferSRV.IsValid() ||
		!GPUResources->StableProxyVertexProofTargetBufferUAV.IsValid() ||
		!GPUResources->StableProxyVertexOwnerTargetBufferUAV.IsValid() ||
		!GPUResources->StableProxyReceiverTriangleIdBufferSRV.IsValid() ||
		!GProxyLightDepthCaptureTexture.IsValid() ||
		!GProxyReceiverLightDepthCaptureTexture.IsValid() ||
		GProxyLightDepthForwardEnabled.W < 0.0f)
	{
		return false;
	}
	if (!BuildReceiverTriangleIdGrid(RHICmdList, GPUResources, LocalToWorld))
	{
		return false;
	}

	TShaderMapRef<FBuildStableProxyVertexVisibilityCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	if (!ComputeShader.IsValid())
	{
		return false;
	}

	// v2762: write a hidden target buffer. The current front field is never placed
	// in UAV state while it is being consumed by CalcViewData.
	RHICmdList.Transition(FRHITransitionInfo(
		GPUResources->StableProxyVertexProofTargetBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(
		GPUResources->StableProxyVertexOwnerTargetBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	FBuildStableProxyVertexVisibilityCS::FParameters Parameters;
	Parameters.ProxyLightDepth = GProxyLightDepthCaptureTexture;
	Parameters.ProxyReceiverLightDepth = GProxyReceiverLightDepthCaptureTexture;
	Parameters.ReceiverTriangleId = GPUResources->StableProxyReceiverTriangleIdBufferSRV;
	Parameters.StableProxyVertexBuffer = GPUResources->StableProxyVertexBufferSRV;
	Parameters.StableProxyTriangleBuffer = GPUResources->StableProxyTriangleBufferSRV;
	Parameters.PreviousVertexProof = GPUResources->StableProxyVertexProofBufferSRV;
	Parameters.PreviousVertexOwner = GPUResources->StableProxyVertexOwnerBufferSRV;
	Parameters.OutVertexProof = GPUResources->StableProxyVertexProofTargetBufferUAV;
	Parameters.OutVertexOwner = GPUResources->StableProxyVertexOwnerTargetBufferUAV;
	Parameters.LocalToWorld = FMatrix44f(LocalToWorld);
	Parameters.LightOriginWidth = GProxyLightDepthOriginWidth;
	Parameters.LightForwardEnabled = GProxyLightDepthForwardEnabled;
	Parameters.LightRightRange = GProxyLightDepthRightRange;
	Parameters.LightUpSettings = GProxyLightDepthUpSettings;
	Parameters.VertexCount = GPUResources->StableProxyVertexCount;
	Parameters.PreviousFieldValid = GPUResources->StableProxyFieldFrontCaptureRevision > 0u ? 1u : 0u;
	Parameters.DepthBiasCm = FMath::Max(DepthBiasCm, 0.0f);
	Parameters.StabilityDiagnosticMode = static_cast<uint32>(FMath::Max(0, GStableShadowDebugMode));

	SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
	SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
	const uint32 NumGroups = FMath::DivideAndRoundUp(GPUResources->StableProxyVertexCount, 64u);
	RHICmdList.DispatchComputeShader(NumGroups, 1, 1);
	UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());
	RHICmdList.Transition(FRHITransitionInfo(
		GPUResources->StableProxyVertexProofTargetBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
	RHICmdList.Transition(FRHITransitionInfo(
		GPUResources->StableProxyVertexOwnerTargetBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));

	RecordStableVertexFlips(RHICmdList, GPUResources, FMath::Max(DepthBiasCm, 0.0f));

	// All commands that populate Target are ordered before later CalcViewData work on
	// this command list. Modes 02/17/18 commit the completed target atomically to Front.
	// Mode 19 deliberately leaves the freshly completed target uncommitted so CalcViewData
	// can consume the current LightDepth classification without changing the historical
	// Front/A pair. This keeps the *lighting equation* identical to mode 02 and changes
	// only the visibility-field source.
	if (bCommitToFront)
	{
		Swap(GPUResources->StableProxyVertexProofBuffer, GPUResources->StableProxyVertexProofTargetBuffer);
		Swap(GPUResources->StableProxyVertexProofBufferSRV, GPUResources->StableProxyVertexProofTargetBufferSRV);
		Swap(GPUResources->StableProxyVertexProofBufferUAV, GPUResources->StableProxyVertexProofTargetBufferUAV);
		Swap(GPUResources->StableProxyVertexOwnerBuffer, GPUResources->StableProxyVertexOwnerTargetBuffer);
		Swap(GPUResources->StableProxyVertexOwnerBufferSRV, GPUResources->StableProxyVertexOwnerTargetBufferSRV);
		Swap(GPUResources->StableProxyVertexOwnerBufferUAV, GPUResources->StableProxyVertexOwnerTargetBufferUAV);
	}
	return true;
}

bool FGaussianSplatRenderer::DispatchBuildStableProxyCastGrid(
	FRHICommandListImmediate& RHICmdList,
	FGaussianSplatGPUResources* GPUResources,
	float DepthBiasCm)
{
	if (!GPUResources || !GPUResources->StableProxyCastGridBufferUAV.IsValid() ||
		!GProxyLightDepthCaptureTexture.IsValid() || !GProxyReceiverLightDepthCaptureTexture.IsValid() ||
		GProxyLightDepthForwardEnabled.W < 0.0f)
	{
		if (GPUResources) GPUResources->StableProxyCastGridValid = 0u;
		return false;
	}
	const FRHITextureDesc& Desc = GProxyLightDepthCaptureTexture->GetDesc();
	const FRHITextureDesc& ReceiverDesc = GProxyReceiverLightDepthCaptureTexture->GetDesc();
	const uint32 Width = FMath::Min<uint32>(Desc.Extent.X, ReceiverDesc.Extent.X);
	const uint32 Height = FMath::Min<uint32>(Desc.Extent.Y, ReceiverDesc.Extent.Y);
	if (Width == 0u || Height == 0u || Width > GPUResources->StableProxyCastGridWidth || Height > GPUResources->StableProxyCastGridHeight)
	{
		GPUResources->StableProxyCastGridValid = 0u;
		return false;
	}
	TShaderMapRef<FBuildStableProxyCastGridCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	if (!ComputeShader.IsValid())
	{
		GPUResources->StableProxyCastGridValid = 0u;
		return false;
	}
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->StableProxyCastGridBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	FBuildStableProxyCastGridCS::FParameters Parameters;
	Parameters.CombinedLightDepth = GProxyLightDepthCaptureTexture;
	Parameters.ReceiverLightDepth = GProxyReceiverLightDepthCaptureTexture;
	Parameters.OutCastVisibility = GPUResources->StableProxyCastGridBufferUAV;
	Parameters.GridWidth = Width;
	Parameters.GridHeight = Height;
	Parameters.DepthBiasCm = FMath::Max(DepthBiasCm, 0.0f);
	Parameters.WorldUnitsPerTexel = GProxyLightDepthOriginWidth.W / FMath::Max(float(Width), 1.0f);
	SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
	SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
	RHICmdList.DispatchComputeShader(FMath::DivideAndRoundUp(Width, 8u), FMath::DivideAndRoundUp(Height, 8u), 1u);
	UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->StableProxyCastGridBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
	GPUResources->StableProxyCastGridValid = 1u;
	GPUResources->StableProxyCastGridActiveWidth = Width;
	GPUResources->StableProxyCastGridActiveHeight = Height;
	return true;
}

static void SetProxyCapturePSParameters(FGaussianSplatPS::FParameters& Params, const FSceneView& View)
{
	Params.ProxyShadowedCapture = GWhiteTexture->TextureRHI;
	Params.ProxyUnshadowedCapture = GWhiteTexture->TextureRHI;
	Params.ProxyCaptureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	const FIntRect Rect = View.UnscaledViewRect;
	Params.ProxyCaptureViewRect = FVector4f(Rect.Min.X, Rect.Min.Y, Rect.Width(), Rect.Height());
	Params.ProxyScreenCaptureSettings = FVector4f::Zero();
	Params.ProxyCaptureOriginTanFOV = GProxyCaptureOriginTanFOV;
	Params.ProxyCaptureForwardAspect = GProxyCaptureForwardAspect;
	Params.ProxyCaptureRightEnabled = GProxyCaptureRightEnabled;
	Params.ProxyCaptureUp = GProxyCaptureUp;
	Params.ProxyCaptureSettings = GProxyShadowCaptureSettings;
	Params.ProxyShadowCameraDistanceSettings = FVector4f(0, 0, 0, 1);
	Params.ProxyShadowDetailSettings = GProxyShadowDetailSettings;
    Params.PCVRShadowBlueOverlay = CVarPCVRShadowBlueOverlay.GetValueOnRenderThread()!=0 ? 1u : 0u;
	const bool bHasSurfaceDepth = GProxySurfaceDepthTexture.IsValid();
	Params.ProxySurfaceDepthTexture = bHasSurfaceDepth
		? GProxySurfaceDepthTexture
		: GWhiteTexture->TextureRHI;
	Params.ProxySurfaceDepthSampler =
		TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Params.ProxySurfaceDepthEnabled = bHasSurfaceDepth ? 1.0f : 0.0f;
	Params.ProxyHistoryShadowedCapture = GWhiteTexture->TextureRHI;
	Params.ProxyHistoryUnshadowedCapture = GWhiteTexture->TextureRHI;
	Params.ProxyHistorySurfaceDepthTexture = GWhiteTexture->TextureRHI;
	Params.ProxyHistoryOriginTanFOV = FVector4f(0, 0, 0, 1);
	Params.ProxyHistoryForwardAspect = FVector4f(1, 0, 0, 1);
	Params.ProxyHistoryRightEnabled = FVector4f(0, 1, 0, 0);
	Params.ProxyHistoryUp = FVector4f(0, 0, 1, 0);
	Params.ProxyShadowTemporalSettings = FVector4f(0, 1, 1, 0);
	Params.ProxyShadowEvidenceSettings = GProxyShadowEvidenceSettings;
	Params.ProxyShadowShapingSettings = GProxyShadowShapingSettings;
	Params.ProxyShadowOcclusionBoost = GProxyShadowOcclusionBoost;
	Params.ProxyShadowColorSettings = GProxyShadowColorSettings;
	Params.ProxyReflectedLightSettings = GProxyReflectedLightSettings;
	Params.ProxyReflectedColorSettings = GProxyReflectedColorSettings;
	Params.ProxyShadowBaseSettings = GProxyShadowBaseSettings;
	Params.ProxyShadowBaseTint = GProxyShadowBaseTint;
	Params.ProxyLightDepthCapture = GProxyLightDepthCaptureTexture.IsValid() ? GProxyLightDepthCaptureTexture : GWhiteTexture->TextureRHI;
	// Continuous depth interpolation prevents the light-space projection from
	// snapping at raw texel centers while an animated sun rotates.
	Params.ProxyLightDepthSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Params.ProxyLightDepthOriginWidth = GProxyLightDepthOriginWidth;
	Params.ProxyLightDepthForwardEnabled = GProxyLightDepthForwardEnabled;
	Params.ProxyLightDepthRightRange = GProxyLightDepthRightRange;
	Params.ProxyLightDepthUpSettings = GProxyLightDepthUpSettings;
	Params.ProxyVolumetricShaftSettings = GProxyVolumetricShaftSettings;
	Params.ProxyVolumetricShaftTint = GProxyVolumetricShaftTint;
}

// Helper: Set pixel shader velocity parameters using self-tracked previous frame data.
// UE5's PrevViewInfo is not populated for PostOpaqueRender callbacks, so we store
// current frame matrices and use them as "previous" data next frame.
// Data is tracked per-view (keyed by FSceneViewState*) to handle multiple viewports
// (e.g., main editor viewport + camera preview) without cross-contamination.
//
// IMPORTANT: We use UN-JITTERED projection matrices for velocity calculation.
// This ensures ClipPosition is stable when camera is static (we skip CalcViewData
// when camera doesn't move, so jittered positions would cause velocity errors).
// Patch39: Render-thread-only scene scoped collider volumes; works for panoramic
// subviews because each sample is clipped against the same absolute world boxes.
template<typename TParameters>
static void SetGSRemovalBoxesPS(TParameters& Params, const FSceneView& View)
{
    FGSRemovalBoxGPUData Boxes[GSGSRemovalBoxMax];
    const FSceneInterface* Scene = View.Family ? View.Family->Scene : nullptr;
    Params.GSRemovalBoxCount = GetGSRemovalBoxes_RenderThread(Scene, Boxes);
    // Per-scene/eye throttle: PIE and editor boxes must never be conflated.
    if(Params.GSRemovalBoxCount && (int32(View.StereoPass)==1 || int32(View.StereoPass)==2))
    {
        static TMap<const FSceneInterface*,TMap<int32,double>> Last;
        double& Stamp=Last.FindOrAdd(Scene).FindOrAdd(int32(View.StereoPass));
        const double Now=FPlatformTime::Seconds();
        if(Now-Stamp>2.0)
        {
            Stamp=Now;const auto& R=View.UnscaledViewRect;
            UE_LOG(LogTemp,Display,TEXT("[GSPreview][RemovalEye] Frame=%llu Eye=%d Scene=%p Boxes=%u Rect=%d,%d,%d,%d Origin=%s PreTranslation=%s InvVP=%s Box0=%s"),uint64(GFrameNumberRenderThread),int32(View.StereoPass),static_cast<const void*>(Scene),Params.GSRemovalBoxCount,R.Min.X,R.Min.Y,R.Max.X,R.Max.Y,*View.ViewMatrices.GetViewOrigin().ToString(),*View.ViewMatrices.GetPreViewTranslation().ToString(),*View.ViewMatrices.GetInvViewProjectionMatrix().ToString(),*Boxes[0].Center.ToString());
        }
    }

    for (uint32 i=0; i<GSGSRemovalBoxMax; ++i)
    {
        const FGSRemovalBoxGPUData B = (i < Params.GSRemovalBoxCount) ? Boxes[i] : FGSRemovalBoxGPUData();
        Params.GSRemovalBoxCenters[i] = B.Center;
        Params.GSRemovalBoxAxisX[i] = B.X;
        Params.GSRemovalBoxAxisY[i] = B.Y;
        Params.GSRemovalBoxAxisZ[i] = B.Z;
    }
}

static void SetVelocityPSParameters(
	FGaussianSplatPS::FParameters& PSParameters,
	const FSceneView& View,
	FGaussianGlobalAccumulator* Accumulator)
{
	// Current frame data - use UN-JITTERED matrix to match CalcViewData's WorldToClip
	FMatrix CurTranslatedVP = View.ViewMatrices.GetTranslatedViewMatrix() * View.ViewMatrices.GetProjectionNoAAMatrix();
	FVector CurPreViewTranslation = View.ViewMatrices.GetPreViewTranslation();

	// Use View.State as per-view key (unique per persistent viewport)
	const void* ViewKey = View.State;

	// Look up previous frame data for this specific view
	FGaussianGlobalAccumulator::FPrevFrameViewData* PrevData = nullptr;
	if (Accumulator && ViewKey)
	{
		PrevData = Accumulator->PrevFrameDataPerView.Find(ViewKey);
	}

	if (PrevData)
	{
		PSParameters.PrevTranslatedWorldToClip = FMatrix44f(PrevData->TranslatedViewProjectionMatrix);
		PSParameters.PrevPreViewTranslation = FVector3f(PrevData->PreViewTranslation);
	}
	else
	{
		// First frame for this view: use current frame data (produces zero velocity)
		PSParameters.PrevTranslatedWorldToClip = FMatrix44f(CurTranslatedVP);
		PSParameters.PrevPreViewTranslation = FVector3f(CurPreViewTranslation);
	}

	PSParameters.PreViewTranslation = FVector3f(CurPreViewTranslation);
	// P10: per-view inverse projection reconstructs world XY at the baked water
	// Gaussian's fixed Z. It never determines where water exists.
	PSParameters.PuddleClipToWorld = FMatrix44f(View.ViewMatrices.GetInvViewProjectionMatrix());
	const FIntRect WaterRect = View.UnscaledViewRect;
	PSParameters.PuddleViewport = FVector4f(float(WaterRect.Min.X), float(WaterRect.Min.Y),
		float(FMath::Max(1, WaterRect.Width())), float(FMath::Max(1, WaterRect.Height())));
	PSParameters.PuddleWaterParams = GPuddleWaterVisualSettings;
	PSParameters.PuddleWaterRippleSizeCm = GPuddleWaterRippleSizeCm;
    PSParameters.PuddleWaterReflectionStrength = GPuddleWaterReflectionStrength;
    PSParameters.PuddleRoofRasterCellCm = GPuddleRoofRasterCellCm;
    PSParameters.PuddleRoofFXParams = GPuddleRoofFXParams;
	PSParameters.PuddleWaterTimeSeconds = View.Family ? float(View.Family->Time.GetRealTimeSeconds()) : 0.0f;
	SetGSRemovalBoxesPS(PSParameters, View);
	PSParameters.LightingPreviewMode = 0;
	PSParameters.StableShadowDebugMode = static_cast<uint32>(FMath::Max(0, GStableShadowDebugMode));
	PSParameters.RenderQualityMode = 0;
	PSParameters.UseProxyLightingMaskAsMain = 0;
	PSParameters.ProxyLightingIntensityScale = 1.0f;
	PSParameters.DayNightBlend = 0.0f;
	PSParameters.RegionalColorGradeParams0 = FVector4f(1.0f, 1.0f, 1.0f, 0.0f);
	PSParameters.RegionalColorGradeParams1 = FVector4f(1.0f, 1.0f, 1.0f, 0.0f);
	SetProxyCapturePSParameters(PSParameters, View);

	// Store current frame data for next frame
	if (Accumulator && ViewKey)
	{
		FGaussianGlobalAccumulator::FPrevFrameViewData& Data = Accumulator->PrevFrameDataPerView.FindOrAdd(ViewKey);
		Data.TranslatedViewProjectionMatrix = CurTranslatedVP;
		Data.PreViewTranslation = CurPreViewTranslation;
	}
}

FGaussianSplatRenderer::FGaussianSplatRenderer()
{
}

FGaussianSplatRenderer::~FGaussianSplatRenderer()
{
}

uint32 FGaussianSplatRenderer::NextPowerOfTwo(uint32 Value)
{
	Value--;
	Value |= Value >> 1;
	Value |= Value >> 2;
	Value |= Value >> 4;
	Value |= Value >> 8;
	Value |= Value >> 16;
	Value++;
	return Value;
}

// Compute WorldToPLY matrix for SH evaluation
// SH coefficients are stored in PLY space, so we need to transform view direction from world to PLY space
// This combines: (1) WorldToLocal from actor transform, and (2) LocalToPLY coordinate system conversion
// LocalToPLY rotation: UE(X,Y,Z) -> PLY(Y,-Z,X) where PLY is X-right, Y-down, Z-forward
static FMatrix ComputeWorldToPLY(const FMatrix& LocalToWorld)
{
	static const FMatrix LocalToPLY(
		FPlane(0, 0, 1, 0),   // UE X -> PLY Z
		FPlane(1, 0, 0, 0),   // UE Y -> PLY X
		FPlane(0, -1, 0, 0),  // UE Z -> PLY -Y
		FPlane(0, 0, 0, 1)
	);
	return LocalToWorld.Inverse() * LocalToPLY;
}

static void SetAdaptiveFootprintParameters(FGaussianSplatCalcViewDataCS::FParameters& Parameters, const FGaussianSplatGPUResources* GPUResources)
{
	if (GPUResources)
	{
		Parameters.UseAdaptiveFootprint = GPUResources->RuntimeFootprintMode == 1u ? 1u : 0u;
		Parameters.FootprintParams0 = GPUResources->FootprintParams0;
		Parameters.FootprintParams1 = GPUResources->FootprintParams1;
		Parameters.FootprintParams2 = GPUResources->FootprintParams2;
		Parameters.FootprintParams3 = GPUResources->FootprintParams3;
	}
	else
	{
		Parameters.UseAdaptiveFootprint = 0u;
		Parameters.FootprintParams0 = FVector4f(0.0f, 1.15f, 0.85f, 0.65f);
		Parameters.FootprintParams1 = FVector4f(8.0f, 10.0f, 14.0f, 12.0f);
		Parameters.FootprintParams2 = FVector4f(2500.0f, 9000.0f, 0.0f, 1.0f);
		Parameters.FootprintParams3 = FVector4f(0.0f, 1.0f, 0.0f, 0.0f);
	}
}

static void SetSpatialDelightingResources(FGaussianSplatCalcViewDataCS::FParameters& Parameters,
	const FGaussianSplatGPUResources* GPUResources, const FMatrix& LocalToWorld)
{
	check(GPUResources != nullptr);
	Parameters.ClusterLightingBuffer = GPUResources->ClusterLightingBufferSRV;
	Parameters.ClusterLightingCount = GPUResources->ClusterLightingBufferSRV.IsValid()
		? GPUResources->SpatialLightingCellCount
		: 0u;
	Parameters.SpatialLightingGridMinCellSize = GPUResources->SpatialLightingGridMinCellSize;
	Parameters.SpatialLightingGridDimensionsStatus = GPUResources->SpatialLightingGridDimensionsStatus;
	Parameters.ProxyNormalGridBuffer = GPUResources->ProxyNormalGridBufferSRV;
	Parameters.ProxyNormalGridCount = GPUResources->ProxyNormalGridBufferSRV.IsValid()
		? GPUResources->ProxyNormalGridCount
		: 0u;
	Parameters.ProxyNormalGridMinCellSize = GPUResources->ProxyNormalGridMinCellSize;
	Parameters.ProxyNormalGridDimensionsStatus = GPUResources->ProxyNormalGridDimensionsStatus;
	Parameters.PuddleDampenPackedMask = GPUResources->PuddleDampenPackedBufferSRV;
	Parameters.PuddleDampenPackedCount = GPUResources->PuddleDampenPackedCount;
	Parameters.PuddleDampenStrength = GPUResources->bPuddleMainSource ? GPuddleDampenStrength : 0.0f;
	Parameters.PuddleSourceWetSettings = GPUResources->bPuddleMainSource ? GPuddleSourceWetSettings : FVector2f::ZeroVector;
	Parameters.PuddleWaterLayer = GPUResources->bPuddleWaterLayer ? 1u : 0u;
	Parameters.PuddleHideMainSource = (GPUResources->bPuddleMainSource && GPuddleHideOriginalSource) ? 1u : 0u;
	Parameters.SnowCoveragePackedBuffer = GPUResources->SnowCoveragePackedBufferSRV;
	Parameters.SnowCoveragePackedCount = GPUResources->SnowCoveragePackedCount;
	Parameters.SnowBakedFieldBuffer2873 = GPUResources->SnowBakedFieldBufferSRV2873;
	Parameters.SnowBakedFieldCount2873 = GPUResources->SnowBakedFieldCount2873;
	Parameters.SnowBakedFieldPatternCm2873 = GPUResources->SnowBakedFieldPatternCm2873;
	// The cached pattern was authored in WORLD coordinates. One matrix check
	// per dispatch (not per splat) protects actor animation and relocated PLYs.
	Parameters.SnowBakedFieldValid2873 =
		(GPUResources->SnowBakedFieldCount2873 > 0u &&
		 GPUResources->SnowBakedFieldBufferSRV2873.IsValid() &&
		 GPUResources->SnowBakedFieldToWorld2873.Equals(LocalToWorld, 0.001)) ? 1u : 0u;
	Parameters.StableSplatBindingBuffer = GPUResources->StableSplatBindingBufferSRV;
	Parameters.StableSplatBindingCount = GPUResources->StableSplatBindingBufferSRV.IsValid()
		? GPUResources->StableSplatBindingCount
		: 0u;
	Parameters.StableProxyTriangleBuffer = GPUResources->StableProxyTriangleBufferSRV;
	Parameters.StableProxyTriangleCount = GPUResources->StableProxyTriangleBufferSRV.IsValid()
		? GPUResources->StableProxyTriangleCount
		: 0u;
	Parameters.StableProxyTriangleVertexNormalBuffer = GPUResources->StableProxyTriangleVertexNormalBufferSRV;
	Parameters.StableProxyTriangleVertexNormalCount = GPUResources->StableProxyTriangleVertexNormalBufferSRV.IsValid()
		? GPUResources->StableProxyTriangleVertexNormalCount
		: 0u;
	Parameters.StableProxyVertexProofBuffer = GPUResources->StableProxyVertexProofBufferSRV;
	Parameters.StableProxyVertexProofPreviousBuffer = GPUResources->StableProxyVertexProofTargetBufferSRV;
	Parameters.StableProxyVertexOwnerBuffer = GPUResources->StableProxyVertexOwnerBufferSRV;
	Parameters.StableProxyVertexOwnerPreviousBuffer = GPUResources->StableProxyVertexOwnerTargetBufferSRV;
	Parameters.StableProxyVertexProofCount = GPUResources->StableProxyVertexProofBufferSRV.IsValid()
		? GPUResources->StableProxyVertexCount
		: 0u;
	Parameters.StableProxyFieldPairValid =
		GPUResources->StableProxyVertexProofTargetBufferSRV.IsValid()
		? GPUResources->StableProxyFieldPairValid
		: 0u;
	Parameters.StableProxyLiveTargetValid =
		GPUResources->StableProxyVertexProofTargetBufferSRV.IsValid()
		? GPUResources->StableProxyLiveTargetValid
		: 0u;
	// Deterministic midpoint reconstruction between the previous and current fully
	// completed fields. Production cast shading consumes this value as well as the
	// A/B diagnostics. Depth is NEVER interpolated; only classified visibility.
	// Production visibility is already a recursive CPU EMA; do not midpoint A/B again.
	Parameters.StableProxyFieldABBlend = 1.0f;
	Parameters.StableProxyVertexBuffer = GPUResources->StableProxyVertexBufferSRV;
	Parameters.StableProxyVertexCount = GPUResources->StableProxyVertexBufferSRV.IsValid()
		? GPUResources->StableProxyVertexCount
		: 0u;
	Parameters.StableProxyCastGridBuffer = GPUResources->StableProxyCastGridBufferSRV;
	Parameters.StableProxyCastGridWidth = GPUResources->StableProxyCastGridActiveWidth;
	Parameters.StableProxyCastGridHeight = GPUResources->StableProxyCastGridActiveHeight;
	Parameters.StableProxyCastGridValid = GPUResources->StableProxyCastGridValid;
	Parameters.StableProxyCastGridOriginWidth = GProxyLightDepthOriginWidth;
	Parameters.StableProxyCastGridForwardEnabled = GProxyLightDepthForwardEnabled;
	Parameters.StableProxyCastGridRightRange = GProxyLightDepthRightRange;
	Parameters.StableProxyCastGridUpSettings = GProxyLightDepthUpSettings;
	Parameters.StableProxyFieldValid = GPUResources->StableProxyFieldValid;
	Parameters.StableShadowDebugMode = static_cast<uint32>(FMath::Max(0, GStableShadowDebugMode));
}

static void SetGaussianLightingParameters(FGaussianSplatCalcViewDataCS::FParameters& Parameters, const FGaussianSplatLightingData* LightingData)
{
	FGaussianSplatLightingData DefaultLighting;
	DefaultLighting.Reset();
	const FGaussianSplatLightingData& Data = LightingData ? *LightingData : DefaultLighting;

	Parameters.ReceiveUnrealLighting = Data.bReceiveLighting;
	constexpr uint32 MaxShaderLights = FGaussianSplatLightingData::MaxLights - 2;
	Parameters.UnrealLightCount = FMath::Min<uint32>(Data.LightCount, MaxShaderLights);
	Parameters.UnrealLightingInfluence = Data.LightingInfluence;
	Parameters.UnrealLightingPadding0 = 0.0f;
	Parameters.UnrealDirectionalResponse = Data.DirectionalResponse;
	Parameters.UnrealLightWrap = Data.LightWrap;
	Parameters.UnrealNormalMode = Data.NormalMode;
	Parameters.UnrealLightingPadding1 = 0.0f;
	Parameters.UnrealAmbientColorIntensity = Data.AmbientColorIntensity;
	Parameters.UnrealIndirectLightParams = Data.IndirectLightParams;
	Parameters.UnrealIndirectShadowTint = Data.IndirectShadowTint;
	Parameters.StableLightingParams0 = Data.StableLightingParams0;
	Parameters.StableLightingParams1 = Data.StableLightingParams1;
	Parameters.StableLightingParams2 = Data.StableLightingParams2;
	Parameters.StableLightingParams3 = Data.StableLightingParams3;
	Parameters.StableLightingParams4 = Data.StableLightingParams4;
	Parameters.UnrealShadowParams = Data.ShadowParams;
	Parameters.UnrealShadowParams2 = Data.ShadowParams2;
	Parameters.UnrealShadowParams2.W = Data.AtmosphereParams.X;
	Parameters.UnrealShadowBounds = Data.ShadowBounds;
	Parameters.UnrealShadowHeight = Data.ShadowHeight;
	Parameters.UnrealRelightParams = Data.RelightParams;
	Parameters.UnrealRelightLookParams = Data.RelightLookParams;
	Parameters.UnrealRelightSunTint = Data.RelightSunTint;
	Parameters.UnrealRelightSkyTint = Data.RelightSkyTint;
	Parameters.TimeOfDayGradeTint = Data.TimeOfDayGradeTint;
	Parameters.UnrealRelightDetailParams = Data.RelightDetailParams;
	Parameters.UnrealRelightDetailParams2 = Data.RelightDetailParams2;
	Parameters.SpatialDelightingParams0 = Data.SpatialDelightingParams0;
	Parameters.SpatialDelightingParams1 = Data.SpatialDelightingParams1;
	Parameters.SpatialDelightingParams2 = Data.SpatialDelightingParams2;
	Parameters.SpatialDelightingParams3 = Data.SpatialDelightingParams3;
	Parameters.SpatialDelightingParams4 = Data.SpatialDelightingParams4;
	Parameters.ProxyNormalParams = Data.ProxyNormalParams;
	Parameters.GaussianSunAccentParams0 = Data.GaussianSunAccentParams0;
	Parameters.GaussianSunAccentParams1 = Data.GaussianSunAccentParams1;
	Parameters.GaussianSunAccentTint = Data.GaussianSunAccentTint;
	Parameters.GaussianPlantTransmissionParams0 = Data.GaussianPlantTransmissionParams0;
	Parameters.GaussianPlantTransmissionParams1 = Data.GaussianPlantTransmissionParams1;
	Parameters.GaussianPlantTransmissionTint = Data.GaussianPlantTransmissionTint;
	Parameters.UnrealShadowHeight.Z = Data.AtmosphereParams.Y;
	Parameters.UnrealShadowHeight.W = Data.AtmosphereParams.Z;
	Parameters.UnrealAtmosphereDistanceParams = Data.AtmosphereDistanceParams;
	Parameters.UnrealAtmosphereFogShapeParams = Data.AtmosphereFogShapeParams;
	Parameters.UnrealAtmosphereFogAmountParams = Data.AtmosphereFogAmountParams;
	Parameters.UnrealAtmosphereHighlightShapeParams = Data.AtmosphereHighlightShapeParams;
	Parameters.UnrealAtmosphereBloomParams = Data.AtmosphereBloomParams;
    // V13: a dedicated negative bloom-strength sentinel suppresses only the
    // GS atmospheric bloom seed in CalcViewData, preserving fog, shadows, relight.
    // The CVar is scoped to the PIE comparison by the editor module.
    if (const IConsoleVariable* BloomDisable = IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.DisableAllBloom")))
    {
        if (BloomDisable->GetInt()!=0)
            Parameters.UnrealAtmosphereBloomParams.Z=-100.0f;
    }
	Parameters.UnrealAtmosphereGlareParams = Data.AtmosphereGlareParams;
	Parameters.UnrealAtmosphereShaftParams = Data.AtmosphereShaftParams;
	Parameters.SeasonalColorSelectParams = Data.SeasonalColorSelectParams;
	Parameters.SeasonalColorAdjustParams = Data.SeasonalColorAdjustParams;
	Parameters.SeasonalSourceColorFrom = Data.SeasonalSourceColorFrom;
	Parameters.SeasonalSourceColorTo = Data.SeasonalSourceColorTo;
	Parameters.SeasonalTargetColorFrom = Data.SeasonalTargetColorFrom;
	Parameters.SeasonalTargetColorTo = Data.SeasonalTargetColorTo;
	Parameters.DirectSplatSurfaceEffectParams0 = GDirectSplatSurfaceEffectSettings0;
	Parameters.DirectSplatSurfaceEffectParams1 = GDirectSplatSurfaceEffectSettings1;
	Parameters.DirectSplatSurfaceEffectParams2 = GDirectSplatSurfaceEffectSettings2;
	Parameters.DirectSnowTuningParams = GDirectSnowTuningSettings;
	Parameters.SnowSourceCoverParams = GSnowSourceCoverSettings;

	for (int32 i = 0; i < FGaussianSplatLightingData::MaxLights; ++i)
	{
		Parameters.UnrealLightPositionRadius[i] = Data.LightPositionRadius[i];
		Parameters.UnrealLightDirectionType[i] = Data.LightDirectionType[i];
		Parameters.UnrealLightColorIntensity[i] = Data.LightColorIntensity[i];
		Parameters.UnrealLightConeAngles[i] = Data.LightConeAngles[i];
	}

	Parameters.UnrealLightConeAngles[6] = Data.AtmosphereGradeParams;
	Parameters.UnrealLightConeAngles[7] = Data.AtmosphereHighlightParams;
}

void FGaussianSplatRenderer::DispatchCalcViewData(
	FRHICommandListImmediate& RHICmdList,
	const FSceneView& View,
	FGaussianSplatGPUResources* GPUResources,
	const FMatrix& LocalToWorld,
	int32 SplatCount,
	int32 SHOrder,
	float OpacityScale,
	float SplatScale,
	bool bUseLODRendering,
	const FGaussianSplatLightingData* LightingData,
	const FGaussianCapturedDepthShadowRenderData* CapturedDepthData)
{
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatCalcViewData);

	FGaussianSplatCalcViewDataCS::FPermutationDomain ViewPermutation;
	ViewPermutation.Set<FGaussianSplatCalcViewDataCS::FVRBaseline>(GetVRDiagnosticMode_RenderThread() == 3);
	TShaderMapRef<FGaussianSplatCalcViewDataCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), ViewPermutation);

	if (!ComputeShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FGaussianSplatCalcViewDataCS shader not valid"));
		return;
	}

	// Transition buffers for compute
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->ViewDataBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	FGaussianSplatCalcViewDataCS::FParameters Parameters;
	Parameters.PackedSplatBuffer = GPUResources->PackedSplatBufferSRV;
	Parameters.Float32PositionBuffer = GPUResources->Float32PositionBufferSRV;
	Parameters.UseFloat32Position = GPUResources->ShouldUseFloat32Positions() ? 1u : 0u;
	Parameters.HighPrecisionColorTexture = GPUResources->GetColorTextureOrDummy();
	Parameters.UseHighPrecisionColorOpacity = GPUResources->ShouldUseHighPrecisionColorOpacity() ? 1u : 0u;
	SetSpatialDelightingResources(Parameters, GPUResources, LocalToWorld);
	SetAdaptiveFootprintParameters(Parameters, GPUResources);
	Parameters.SHBuffer = GPUResources->SHBufferSRV;
	Parameters.ViewDataBuffer = GPUResources->ViewDataBufferUAV;

	// Cluster visibility integration (UNIFIED APPROACH)
	Parameters.SplatClusterIndexBuffer = GPUResources->SplatClusterIndexBufferSRV;
	Parameters.ClusterVisibilityBitmap = GPUResources->ClusterVisibilityBitmapSRV;
	Parameters.LODClusterSelectedBitmap = GPUResources->LODClusterSelectedBitmapSRV;
	Parameters.SelectedClusterBuffer = GPUResources->SelectedClusterBufferSRV;
	Parameters.CompactedSplatIndices = GPUResources->CompactedSplatIndicesBufferSRV;
	Parameters.UseCompaction = 0;
	Parameters.VisibleSplatCount = 0;

	if (GPUResources->bHasClusterData)
	{
		Parameters.UseClusterCulling = 1;
		Parameters.UseLODRendering = bUseLODRendering ? 1 : 0;
		Parameters.OriginalSplatCount = SplatCount - GPUResources->LODSplatCount;
	}
	else
	{
		Parameters.UseClusterCulling = 0;
		Parameters.UseLODRendering = 0;
		Parameters.OriginalSplatCount = SplatCount;
	}

		// Matrices
	Parameters.LocalToWorld = FMatrix44f(LocalToWorld);
	Parameters.WorldToPLY = FMatrix44f(ComputeWorldToPLY(LocalToWorld));
	// Use un-jittered projection matrix so ClipPosition is stable when camera is static
	// (TAA/TSR jitter changes every frame, but we skip CalcViewData when camera is static)
	Parameters.WorldToClip = FMatrix44f(View.ViewMatrices.GetViewMatrix() * View.ViewMatrices.GetProjectionNoAAMatrix());
	Parameters.PreViewTranslation = FVector3f(View.ViewMatrices.GetPreViewTranslation());
	Parameters.WorldToView = FMatrix44f(View.ViewMatrices.GetViewMatrix());
	Parameters.CameraPosition = FVector3f(View.ViewMatrices.GetViewOrigin());

	FIntRect ViewRect = View.UnscaledViewRect;
	Parameters.ScreenSize = FVector2f(ViewRect.Width(), ViewRect.Height());

	const FMatrix& ProjMatrix = View.ViewMatrices.GetProjectionMatrix();
	Parameters.FocalLength = FVector2f(
		ProjMatrix.M[0][0] * ViewRect.Width() * 0.5f,
		ProjMatrix.M[1][1] * ViewRect.Height() * 0.5f
	);

	Parameters.SplatCount = SplatCount;
	// Use effective SH order: min(requested order, available data)
	int32 EffectiveSHOrder = FMath::Min(SHOrder, GPUResources->GetSHBands());
	Parameters.SHOrder = EffectiveSHOrder;
	// SH buffer includes DC + higher-order coefficients: band1=4, band2=9, band3=16
	Parameters.NumSHCoeffs = (EffectiveSHOrder == 0) ? 0 : (EffectiveSHOrder == 1) ? 4 : (EffectiveSHOrder == 2) ? 9 : 16;
	{
		int32 StoredSHBands = GPUResources->GetSHBands();
		Parameters.SHBufferCoeffs = (StoredSHBands == 0) ? 0 : (StoredSHBands == 1) ? 4 : (StoredSHBands == 2) ? 9 : 16;
	}
	Parameters.UseSHRendering = (EffectiveSHOrder > 0) ? 1 : 0;
	Parameters.OpacityScale = OpacityScale;
	Parameters.SplatScale = SplatScale;
	SetGaussianLightingParameters(Parameters, LightingData);

	// Not using global compaction path
	Parameters.GlobalBaseOffsetsBuffer = GPUResources->CompactedSplatIndicesBufferSRV;  // dummy
	Parameters.ProxyIndex = 0;
	Parameters.UseGlobalCompactionPath = 0;
	Parameters.MaxRenderBudget = 0;

	// Dispatch compute shader
	const uint32 ThreadGroupSize = 256;
	const uint32 NumGroups = FMath::DivideAndRoundUp((uint32)SplatCount, ThreadGroupSize);

	SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
	SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
	RHICmdList.DispatchComputeShader(NumGroups, 1, 1);
	UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());

	// Transition buffer for next stage
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->ViewDataBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
}

void FGaussianSplatRenderer::DispatchCalcDistances(
	FRHICommandListImmediate& RHICmdList,
	FGaussianSplatGPUResources* GPUResources,
	int32 SplatCount)
{
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatCalcDistances);

	TShaderMapRef<FGaussianSplatCalcDistancesCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	if (!ComputeShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FGaussianSplatCalcDistancesCS shader not valid"));
		return;
	}

	// Transition buffers
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortDistanceBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortKeysBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	FGaussianSplatCalcDistancesCS::FParameters Parameters;
	Parameters.ViewDataBuffer = GPUResources->ViewDataBufferSRV;
	Parameters.DistanceBuffer = GPUResources->SortDistanceBufferUAV;
	Parameters.KeyBuffer = GPUResources->SortKeysBufferUAV;
	Parameters.SplatCount = SplatCount;
	Parameters.DispatchBaseIndex = 0u;

	// Dispatch only for actual SplatCount ??no power-of-2 padding needed for radix sort
	const uint32 ThreadGroupSize = 256;
	const uint32 NumGroups = FMath::DivideAndRoundUp((uint32)SplatCount, ThreadGroupSize);

	SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
	SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
	RHICmdList.DispatchComputeShader(NumGroups, 1, 1);
	UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());
}

void FGaussianSplatRenderer::DispatchRadixSort(
	FRHICommandListImmediate& RHICmdList,
	FGaussianSplatGPUResources* GPUResources,
	int32 SplatCount)
{
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatRadixSort);

	TShaderMapRef<FRadixSortCountCS> CountShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FRadixSortPrefixSumCS> PrefixSumShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FRadixSortDigitPrefixSumCS> DigitPrefixSumShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FRadixSortScatterCS> ScatterShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	if (!CountShader.IsValid() || !PrefixSumShader.IsValid() ||
		!DigitPrefixSumShader.IsValid() || !ScatterShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Radix sort shaders not valid"));
		return;
	}

	// Radix sort works with any count ??no power-of-2 padding needed
	uint32 SortCount = (uint32)SplatCount;
	uint32 NumTiles = FMath::DivideAndRoundUp(SortCount, 1024u);

	// Distance buffers: [0] = primary, [1] = alt
	FUnorderedAccessViewRHIRef DistUAVs[2] = {
		GPUResources->SortDistanceBufferUAV,
		GPUResources->SortDistanceBufferAltUAV
	};
	FBufferRHIRef DistBuffers[2] = {
		GPUResources->SortDistanceBuffer,
		GPUResources->SortDistanceBufferAlt
	};

	// Key buffers: [0] = primary, [1] = alt
	FUnorderedAccessViewRHIRef KeyUAVs[2] = {
		GPUResources->SortKeysBufferUAV,
		GPUResources->SortKeysBufferAltUAV
	};
	FBufferRHIRef KeyBuffers[2] = {
		GPUResources->SortKeysBuffer,
		GPUResources->SortKeysBufferAlt
	};

	// Ensure alt buffers are in UAVCompute state
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortDistanceBufferAlt, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortKeysBufferAlt, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->RadixHistogramBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->RadixDigitOffsetBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	// SortParamsBuffer is bound as SRV (unused when UseIndirectSort=0, but needs valid state)
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortParamsBuffer, ERHIAccess::Unknown, ERHIAccess::SRVCompute));

	// 4 radix passes: bits 0-7, 8-15, 16-23, 24-31
	for (uint32 Pass = 0; Pass < 4; Pass++)
	{
		uint32 RadixShift = Pass * 8;
		uint32 SrcIdx = Pass & 1;
		uint32 DstIdx = 1 - SrcIdx;

		// --- CountCS: build per-tile histograms ---
		{
			FRadixSortCountCS::FParameters Params;
			Params.HistogramBuffer = GPUResources->RadixHistogramBufferUAV;
			Params.SrcKeys = DistUAVs[SrcIdx];
			Params.SortParams = GPUResources->SortParamsBufferSRV;
			Params.RadixShift = RadixShift;
			Params.Count = SortCount;
			Params.NumTiles = NumTiles;
			Params.UseIndirectSort = 0;

			SetComputePipelineState(RHICmdList, CountShader.GetComputeShader());
			SetShaderParameters(RHICmdList, CountShader, CountShader.GetComputeShader(), Params);
			RHICmdList.DispatchComputeShader(NumTiles, 1, 1);
			UnsetShaderUAVs(RHICmdList, CountShader, CountShader.GetComputeShader());
		}

		// UAV barrier
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->RadixHistogramBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

		// --- PrefixSumCS: exclusive prefix sum per digit across tiles ---
		{
			FRadixSortPrefixSumCS::FParameters Params;
			Params.HistogramBuffer = GPUResources->RadixHistogramBufferUAV;
			Params.DigitOffsetBuffer = GPUResources->RadixDigitOffsetBufferUAV;
			Params.SortParams = GPUResources->SortParamsBufferSRV;
			Params.NumTiles = NumTiles;
			Params.UseIndirectSort = 0;

			SetComputePipelineState(RHICmdList, PrefixSumShader.GetComputeShader());
			SetShaderParameters(RHICmdList, PrefixSumShader, PrefixSumShader.GetComputeShader(), Params);
			RHICmdList.DispatchComputeShader(256, 1, 1);
			UnsetShaderUAVs(RHICmdList, PrefixSumShader, PrefixSumShader.GetComputeShader());
		}

		// UAV barrier
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->RadixDigitOffsetBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

		// --- DigitPrefixSumCS: exclusive prefix sum across digit totals ---
		{
			FRadixSortDigitPrefixSumCS::FParameters Params;
			Params.DigitOffsetBuffer = GPUResources->RadixDigitOffsetBufferUAV;

			SetComputePipelineState(RHICmdList, DigitPrefixSumShader.GetComputeShader());
			SetShaderParameters(RHICmdList, DigitPrefixSumShader, DigitPrefixSumShader.GetComputeShader(), Params);
			RHICmdList.DispatchComputeShader(1, 1, 1);
			UnsetShaderUAVs(RHICmdList, DigitPrefixSumShader, DigitPrefixSumShader.GetComputeShader());
		}

		// UAV barrier
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->RadixDigitOffsetBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->RadixHistogramBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

		// --- ScatterCS: scatter keys+values to sorted positions ---
		{
			FRadixSortScatterCS::FParameters Params;
			Params.SrcKeys = DistUAVs[SrcIdx];
			Params.SrcVals = KeyUAVs[SrcIdx];
			Params.DstKeys = DistUAVs[DstIdx];
			Params.DstVals = KeyUAVs[DstIdx];
			Params.HistogramBuffer = GPUResources->RadixHistogramBufferUAV;
			Params.DigitOffsetBuffer = GPUResources->RadixDigitOffsetBufferUAV;
			Params.SortParams = GPUResources->SortParamsBufferSRV;
			Params.RadixShift = RadixShift;
			Params.Count = SortCount;
			Params.NumTiles = NumTiles;
			Params.UseIndirectSort = 0;

			SetComputePipelineState(RHICmdList, ScatterShader.GetComputeShader());
			SetShaderParameters(RHICmdList, ScatterShader, ScatterShader.GetComputeShader(), Params);
			RHICmdList.DispatchComputeShader(NumTiles, 1, 1);
			UnsetShaderUAVs(RHICmdList, ScatterShader, ScatterShader.GetComputeShader());
		}

		// UAV barriers between passes
		RHICmdList.Transition(FRHITransitionInfo(DistBuffers[DstIdx], ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
		RHICmdList.Transition(FRHITransitionInfo(KeyBuffers[DstIdx], ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
	}

	// After 4 passes (even count), result is back in primary buffers [0]
	// Transition SortKeysBuffer for rendering
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortKeysBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVGraphics));
}

void FGaussianSplatRenderer::DispatchRadixSortIndirect(
	FRHICommandListImmediate& RHICmdList,
	FGaussianSplatGPUResources* GPUResources)
{
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatRadixSort);

	TShaderMapRef<FRadixSortCountCS> CountShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FRadixSortPrefixSumCS> PrefixSumShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FRadixSortDigitPrefixSumCS> DigitPrefixSumShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FRadixSortScatterCS> ScatterShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	if (!CountShader.IsValid() || !PrefixSumShader.IsValid() ||
		!DigitPrefixSumShader.IsValid() || !ScatterShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Radix sort shaders not valid"));
		return;
	}

	// Distance buffers: [0] = primary, [1] = alt
	FUnorderedAccessViewRHIRef DistUAVs[2] = {
		GPUResources->SortDistanceBufferUAV,
		GPUResources->SortDistanceBufferAltUAV
	};
	FBufferRHIRef DistBuffers[2] = {
		GPUResources->SortDistanceBuffer,
		GPUResources->SortDistanceBufferAlt
	};

	// Key buffers: [0] = primary, [1] = alt
	FUnorderedAccessViewRHIRef KeyUAVs[2] = {
		GPUResources->SortKeysBufferUAV,
		GPUResources->SortKeysBufferAltUAV
	};
	FBufferRHIRef KeyBuffers[2] = {
		GPUResources->SortKeysBuffer,
		GPUResources->SortKeysBufferAlt
	};

	// Ensure alt buffers are in UAVCompute state
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortDistanceBufferAlt, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortKeysBufferAlt, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->RadixHistogramBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->RadixDigitOffsetBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	// SortIndirectArgsBuffer should already be in IndirectArgs state from DispatchPrepareIndirectArgs
	// SortParamsBuffer should already be in SRVCompute state from DispatchPrepareIndirectArgs

	// 4 radix passes: bits 0-7, 8-15, 16-23, 24-31
	for (uint32 Pass = 0; Pass < 4; Pass++)
	{
		uint32 RadixShift = Pass * 8;
		uint32 SrcIdx = Pass & 1;
		uint32 DstIdx = 1 - SrcIdx;

		// --- CountCS: build per-tile histograms (INDIRECT DISPATCH) ---
		{
			FRadixSortCountCS::FParameters Params;
			Params.HistogramBuffer = GPUResources->RadixHistogramBufferUAV;
			Params.SrcKeys = DistUAVs[SrcIdx];
			Params.SortParams = GPUResources->SortParamsBufferSRV;
			Params.RadixShift = RadixShift;
			Params.Count = 0;       // Unused when UseIndirectSort=1
			Params.NumTiles = 0;    // Unused when UseIndirectSort=1
			Params.UseIndirectSort = 1;

			SetComputePipelineState(RHICmdList, CountShader.GetComputeShader());
			SetShaderParameters(RHICmdList, CountShader, CountShader.GetComputeShader(), Params);
			RHICmdList.DispatchIndirectComputeShader(GPUResources->SortIndirectArgsBuffer, 0);
			UnsetShaderUAVs(RHICmdList, CountShader, CountShader.GetComputeShader());
		}

		// UAV barrier
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->RadixHistogramBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

		// --- PrefixSumCS: exclusive prefix sum per digit across tiles ---
		// Fixed dispatch (256, 1, 1) - one group per digit
		// NumTiles is read from SortParams buffer
		{
			FRadixSortPrefixSumCS::FParameters Params;
			Params.HistogramBuffer = GPUResources->RadixHistogramBufferUAV;
			Params.DigitOffsetBuffer = GPUResources->RadixDigitOffsetBufferUAV;
			Params.SortParams = GPUResources->SortParamsBufferSRV;
			Params.NumTiles = 0;    // Unused when UseIndirectSort=1
			Params.UseIndirectSort = 1;

			SetComputePipelineState(RHICmdList, PrefixSumShader.GetComputeShader());
			SetShaderParameters(RHICmdList, PrefixSumShader, PrefixSumShader.GetComputeShader(), Params);
			RHICmdList.DispatchComputeShader(256, 1, 1);
			UnsetShaderUAVs(RHICmdList, PrefixSumShader, PrefixSumShader.GetComputeShader());
		}

		// UAV barrier
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->RadixDigitOffsetBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

		// --- DigitPrefixSumCS: exclusive prefix sum across digit totals ---
		// No changes needed - doesn't use Count or NumTiles
		{
			FRadixSortDigitPrefixSumCS::FParameters Params;
			Params.DigitOffsetBuffer = GPUResources->RadixDigitOffsetBufferUAV;

			SetComputePipelineState(RHICmdList, DigitPrefixSumShader.GetComputeShader());
			SetShaderParameters(RHICmdList, DigitPrefixSumShader, DigitPrefixSumShader.GetComputeShader(), Params);
			RHICmdList.DispatchComputeShader(1, 1, 1);
			UnsetShaderUAVs(RHICmdList, DigitPrefixSumShader, DigitPrefixSumShader.GetComputeShader());
		}

		// UAV barrier
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->RadixDigitOffsetBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->RadixHistogramBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

		// --- ScatterCS: scatter keys+values to sorted positions (INDIRECT DISPATCH) ---
		{
			FRadixSortScatterCS::FParameters Params;
			Params.SrcKeys = DistUAVs[SrcIdx];
			Params.SrcVals = KeyUAVs[SrcIdx];
			Params.DstKeys = DistUAVs[DstIdx];
			Params.DstVals = KeyUAVs[DstIdx];
			Params.HistogramBuffer = GPUResources->RadixHistogramBufferUAV;
			Params.DigitOffsetBuffer = GPUResources->RadixDigitOffsetBufferUAV;
			Params.SortParams = GPUResources->SortParamsBufferSRV;
			Params.RadixShift = RadixShift;
			Params.Count = 0;       // Unused when UseIndirectSort=1
			Params.NumTiles = 0;    // Unused when UseIndirectSort=1
			Params.UseIndirectSort = 1;

			SetComputePipelineState(RHICmdList, ScatterShader.GetComputeShader());
			SetShaderParameters(RHICmdList, ScatterShader, ScatterShader.GetComputeShader(), Params);
			RHICmdList.DispatchIndirectComputeShader(GPUResources->SortIndirectArgsBuffer, 0);
			UnsetShaderUAVs(RHICmdList, ScatterShader, ScatterShader.GetComputeShader());
		}

		// UAV barriers between passes
		RHICmdList.Transition(FRHITransitionInfo(DistBuffers[DstIdx], ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
		RHICmdList.Transition(FRHITransitionInfo(KeyBuffers[DstIdx], ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
	}

	// After 4 passes (even count), result is back in primary buffers [0]
	// Transition SortKeysBuffer for rendering
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortKeysBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVGraphics));
}

void FGaussianSplatRenderer::DrawSplats(
	FRHICommandListImmediate& RHICmdList,
	const FSceneView& View,
	FGaussianSplatGPUResources* GPUResources,
	int32 SplatCount)
{
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatDraw);

	if (!GPUResources || !GPUResources->IndexBuffer.IsValid())
	{
		return;
	}

	TShaderMapRef<FGaussianSplatVS> VertexShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), FGaussianSplatVS::FPermutationDomain{});
	TShaderMapRef<FGaussianSplatPS> PixelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), FGaussianSplatPS::FPermutationDomain());

	if (!VertexShader.IsValid() || !PixelShader.IsValid())
	{
		return;
	}

	// Transition view data buffer for graphics reads.
	// Use Unknown source: handles both fresh compute (SRVCompute) and cached (SRVGraphics) paths.
	if (GPUResources->ViewDataBuffer.IsValid())
	{
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->ViewDataBuffer, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));
	}

	// Set up graphics pipeline state
	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
	// Depth test enabled (CF_DepthNearOrEqual) so splats are occluded by scene geometry
	// Depth write disabled (false) because splats are transparent and blend among themselves
	// Read the scene depth only. 3DGS must never modify the scene depth/stencil buffers:
	// doing so leaks the plugin into later rendering of ordinary Unreal primitives.
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<
		false, CF_DepthNearOrEqual
	>::GetRHI();

	// Blend mode for MRT:
	// RT0 (Color): Premultiplied alpha "over" for back-to-front compositing in scene-linear HDR.
	//   result = src + dst * (1 - srcAlpha)
	//   Using CW_RGBA preserves coverage; the before-tonemap composite only applies PreExposure.
	// RT1 (Velocity): Simple replacement - velocity values should not be blended
	GraphicsPSOInit.BlendState = TStaticBlendState<
		// RT0: ColorWriteMask, ColorBlendOp, ColorSrcBlend, ColorDestBlend, AlphaBlendOp, AlphaSrcBlend, AlphaDestBlend
		CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha,
		// RT1: ColorWriteMask, ColorBlendOp, ColorSrcBlend, ColorDestBlend, AlphaBlendOp, AlphaSrcBlend, AlphaDestBlend
		CW_RGBA, BO_Add, BF_One, BF_Zero, BO_Add, BF_One, BF_Zero,
		// RT2 v35: keep RGB replacement for the stable normal, but accumulate alpha
		// as eligible-water coverage with premultiplied-over. This makes the Rain
		// receiver mask independent of which Gaussian happens to overwrite RT2 last.
		CW_RGBA, BO_Add, BF_One, BF_Zero, BO_Add, BF_One, BF_Zero
	>::GetRHI();

	GraphicsPSOInit.PrimitiveType = PT_TriangleList;
	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

	FIntRect ViewRect = View.UnscaledViewRect;
	RHICmdList.SetViewport(
		ViewRect.Min.X,
		ViewRect.Min.Y,
		0.0f,
		ViewRect.Max.X,
		ViewRect.Max.Y,
		1.0f
	);

	// Set vertex shader parameters
	FGaussianSplatVS::FParameters VSParameters{};
	VSParameters.PerSplatLightingEnabled = 0;
    VSParameters.PCVRReuseRadialDepth = CVarPCVRReuseRadialDepth.GetValueOnRenderThread()!=0 ? 1u : 0u;
	VSParameters.BaselineRenderQualityMode = 0;
    VSParameters.BaselineRasterClipParams = FVector4f(0,0,0,0);
	VSParameters.ViewDataBuffer = GPUResources->ViewDataBufferSRV;
	VSParameters.SortKeysBuffer = GPUResources->SortKeysBufferSRV;
	VSParameters.SplatCount = SplatCount;
	// Debug mode from console variable (Nanite-style cluster coloring)
	VSParameters.DebugMode = static_cast<uint32>(FMath::Max(0, CVarShowClusterBounds.GetValueOnRenderThread()));
	// Pass Nanite enabled state for debug visualization (non-Nanite assets render black in debug mode)
	VSParameters.EnableNanite = GPUResources->bEnableNanite ? 1 : 0;

	SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), VSParameters);

	// Pixel shader parameters
	FGaussianSplatPS::FParameters PSParameters;
	PSParameters.PerSplatLightingEnabled = 0;
    PSParameters.PCVREarlyAlphaEnabled = CVarGSPCVREarlyAlpha.GetValueOnRenderThread()!=0 ? 1u : 0u;
    PSParameters.PCVRReuseRadialDepth = CVarPCVRReuseRadialDepth.GetValueOnRenderThread()!=0 ? 1u : 0u;
	PSParameters.StableShadowColorRetention = 1.0f;
	SetVelocityPSParameters(PSParameters, View, nullptr);
	SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PSParameters);

	// Draw instanced quads
	// 4 vertices per quad, N instances (either from SplatCount or indirect args)
	// Using index buffer: 6 indices per quad (2 triangles)
	RHICmdList.SetStreamSource(0, nullptr, 0);

	// Use indirect draw when Nanite is enabled and cluster data is available
	// This allows the GPU to determine how many instances to draw
	if (GPUResources->bEnableNanite && GPUResources->bSupportsIndirectDraw && GPUResources->bHasClusterData)
	{
		// GPU-driven rendering: instance count comes from cluster culling results
		RHICmdList.DrawIndexedPrimitiveIndirect(
			GPUResources->IndexBuffer,
			GPUResources->IndirectDrawArgsBuffer,
			0  // ArgumentOffset
		);
	}
	else
	{
		// Fallback: CPU-driven rendering with fixed splat count (non-Nanite path)
		RHICmdList.DrawIndexedPrimitive(
			GPUResources->IndexBuffer,
			0,  // BaseVertexIndex
			0,  // FirstInstance
			4,  // NumVertices
			0,  // StartIndex
			2,  // NumPrimitives (2 triangles per quad)
			SplatCount  // NumInstances
		);
	}
}

// NOTE: DispatchCalcLODViewDataGPUDriven and DispatchUpdateDrawArgs have been removed
// in the unified approach. LOD splats are now processed by CalcViewData shader using
// the same buffers as original splats.

//----------------------------------------------------------------------
// Splat Compaction Implementation
//----------------------------------------------------------------------

void FGaussianSplatRenderer::DispatchCompactSplats(
	FRHICommandListImmediate& RHICmdList,
	FGaussianSplatGPUResources* GPUResources,
	int32 TotalSplatCount,
	int32 OriginalSplatCount,
	bool bUseLODRendering)
{
    GSPreviewPassCounters::FScopedCPUTime GSPreviewCPUTimeScope(GSPreviewPassCounters::CompactLegacy);
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatCompactSplats);

	TShaderMapRef<FCompactSplatsCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	if (!ComputeShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FCompactSplatsCS shader not valid"));
		return;
	}

    // Culling/LOD is identical for both eyes. Reuse its compact index list too;
    // appending a second time without the culling reset would corrupt the counter.
    if(GPUResources->bPCVRSharedCull && GPUResources->bPCVRCompactedShared &&
       GPUResources->PCVRCullFrame==GFrameNumberRenderThread) return;
	// 2.8.74: counter is already cleared on GPU by the preceding
	// DispatchClusterCulling reset shader. Never lock a live GPU buffer here:
	// that CPU submission/read-modify-write path can serialize render work.
	// This compact path is only called after DispatchClusterCulling.
	// Transition buffers
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->CompactedSplatIndicesBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->VisibleSplatCountBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	FCompactSplatsCS::FParameters Parameters;
	Parameters.SplatClusterIndexBuffer = GPUResources->SplatClusterIndexBufferSRV;
	Parameters.ClusterVisibilityBitmap = GPUResources->ClusterVisibilityBitmapSRV;
	Parameters.LODClusterSelectedBitmap = GPUResources->LODClusterSelectedBitmapSRV;
	Parameters.SelectedClusterBuffer = GPUResources->SelectedClusterBufferSRV;
	Parameters.CompactedSplatIndices = GPUResources->CompactedSplatIndicesBufferUAV;
	Parameters.VisibleSplatCount = GPUResources->VisibleSplatCountBufferUAV;
	Parameters.TotalSplatCount = TotalSplatCount;
	Parameters.OriginalSplatCount = OriginalSplatCount;
	Parameters.UseLODRendering = bUseLODRendering ? 1 : 0;
	Parameters.OutputCapacity = GPUResources->CompactionCapacity;

	const uint32 ThreadGroupSize = 256;
	const uint32 NumGroups = FMath::DivideAndRoundUp((uint32)TotalSplatCount, ThreadGroupSize);

	SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
	SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
	RHICmdList.DispatchComputeShader(NumGroups, 1, 1);
	UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());

	// Transition for next stage
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->CompactedSplatIndicesBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->VisibleSplatCountBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
    GPUResources->bPCVRCompactedShared=GPUResources->bPCVRSharedCull;
}

void FGaussianSplatRenderer::DispatchPrepareIndirectArgs(
	FRHICommandListImmediate& RHICmdList,
	FGaussianSplatGPUResources* GPUResources)
{
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatPrepareIndirectArgs);

	TShaderMapRef<FPrepareIndirectArgsCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	if (!ComputeShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FPrepareIndirectArgsCS shader not valid"));
		return;
	}

	// Transition buffers
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->IndirectDispatchArgsBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->IndirectDrawArgsBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortIndirectArgsBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortParamsBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	FPrepareIndirectArgsCS::FParameters Parameters;
	Parameters.VisibleSplatCount = GPUResources->VisibleSplatCountBufferSRV;
	Parameters.IndirectDispatchArgs = GPUResources->IndirectDispatchArgsBufferUAV;
	Parameters.IndirectDrawArgs = GPUResources->IndirectDrawArgsBufferUAV;
	Parameters.SortIndirectArgs = GPUResources->SortIndirectArgsBufferUAV;
	Parameters.SortParamsBuffer = GPUResources->SortParamsBufferUAV;
	Parameters.ComputeThreadGroupSize = 256;

	SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
	SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
	RHICmdList.DispatchComputeShader(1, 1, 1);
	UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());

	// Transition for indirect dispatch/draw
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->IndirectDispatchArgsBuffer, ERHIAccess::UAVCompute, ERHIAccess::IndirectArgs));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->IndirectDrawArgsBuffer, ERHIAccess::UAVCompute, ERHIAccess::IndirectArgs));
	// Transition sort buffers for radix sort indirect dispatch
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortIndirectArgsBuffer, ERHIAccess::UAVCompute, ERHIAccess::IndirectArgs));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortParamsBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
}

void FGaussianSplatRenderer::DispatchCalcViewDataCompacted(
	FRHICommandListImmediate& RHICmdList,
	const FSceneView& View,
	FGaussianSplatGPUResources* GPUResources,
	const FMatrix& LocalToWorld,
	int32 SplatCount,
	int32 OriginalSplatCount,
	int32 SHOrder,
	float OpacityScale,
	float SplatScale,
	const FGaussianSplatLightingData* LightingData,
	const FGaussianCapturedDepthShadowRenderData* CapturedDepthData)
{
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatCalcViewDataCompacted);

	FGaussianSplatCalcViewDataCS::FPermutationDomain ViewPermutation;
	ViewPermutation.Set<FGaussianSplatCalcViewDataCS::FVRBaseline>(GetVRDiagnosticMode_RenderThread() == 3);
	TShaderMapRef<FGaussianSplatCalcViewDataCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), ViewPermutation);
	if (!ComputeShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FGaussianSplatCalcViewDataCS shader not valid"));
		return;
	}

	// Transition buffers for compute
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->ViewDataBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	FGaussianSplatCalcViewDataCS::FParameters Parameters;
	Parameters.PackedSplatBuffer = GPUResources->PackedSplatBufferSRV;
	Parameters.Float32PositionBuffer = GPUResources->Float32PositionBufferSRV;
	Parameters.UseFloat32Position = GPUResources->ShouldUseFloat32Positions() ? 1u : 0u;
	Parameters.HighPrecisionColorTexture = GPUResources->GetColorTextureOrDummy();
	Parameters.UseHighPrecisionColorOpacity = GPUResources->ShouldUseHighPrecisionColorOpacity() ? 1u : 0u;
	SetSpatialDelightingResources(Parameters, GPUResources, LocalToWorld);
	SetAdaptiveFootprintParameters(Parameters, GPUResources);
	Parameters.SHBuffer = GPUResources->SHBufferSRV;
	Parameters.ViewDataBuffer = GPUResources->ViewDataBufferUAV;

	// Cluster visibility data (needed for debug visualization)
	Parameters.SplatClusterIndexBuffer = GPUResources->SplatClusterIndexBufferSRV;
	Parameters.ClusterVisibilityBitmap = GPUResources->ClusterVisibilityBitmapSRV;
	Parameters.LODClusterSelectedBitmap = GPUResources->LODClusterSelectedBitmapSRV;
	Parameters.SelectedClusterBuffer = GPUResources->SelectedClusterBufferSRV;
	Parameters.UseClusterCulling = 1;
	Parameters.UseLODRendering = 0;
	Parameters.OriginalSplatCount = OriginalSplatCount;

	// COMPACTION MODE: Use compacted indices
	Parameters.CompactedSplatIndices = GPUResources->CompactedSplatIndicesBufferSRV;
	Parameters.UseCompaction = 1;
	Parameters.VisibleSplatCount = SplatCount;

		// Matrices
	Parameters.LocalToWorld = FMatrix44f(LocalToWorld);
	Parameters.WorldToPLY = FMatrix44f(ComputeWorldToPLY(LocalToWorld));
	// Use un-jittered projection matrix so ClipPosition is stable when camera is static
	// (TAA/TSR jitter changes every frame, but we skip CalcViewData when camera is static)
	Parameters.WorldToClip = FMatrix44f(View.ViewMatrices.GetViewMatrix() * View.ViewMatrices.GetProjectionNoAAMatrix());
	Parameters.PreViewTranslation = FVector3f(View.ViewMatrices.GetPreViewTranslation());
	Parameters.WorldToView = FMatrix44f(View.ViewMatrices.GetViewMatrix());
	Parameters.CameraPosition = FVector3f(View.ViewMatrices.GetViewOrigin());

	FIntRect ViewRect = View.UnscaledViewRect;
	Parameters.ScreenSize = FVector2f(ViewRect.Width(), ViewRect.Height());

	const FMatrix& ProjMatrix = View.ViewMatrices.GetProjectionMatrix();
	Parameters.FocalLength = FVector2f(
		ProjMatrix.M[0][0] * ViewRect.Width() * 0.5f,
		ProjMatrix.M[1][1] * ViewRect.Height() * 0.5f
	);

	Parameters.SplatCount = SplatCount;
	// Use effective SH order: min(requested order, available data)
	int32 EffectiveSHOrder = FMath::Min(SHOrder, GPUResources->GetSHBands());
	Parameters.SHOrder = EffectiveSHOrder;
	// SH buffer includes DC + higher-order coefficients: band1=4, band2=9, band3=16
	Parameters.NumSHCoeffs = (EffectiveSHOrder == 0) ? 0 : (EffectiveSHOrder == 1) ? 4 : (EffectiveSHOrder == 2) ? 9 : 16;
	{
		int32 StoredSHBands = GPUResources->GetSHBands();
		Parameters.SHBufferCoeffs = (StoredSHBands == 0) ? 0 : (StoredSHBands == 1) ? 4 : (StoredSHBands == 2) ? 9 : 16;
	}
	Parameters.UseSHRendering = (EffectiveSHOrder > 0) ? 1 : 0;
	Parameters.OpacityScale = OpacityScale;
	Parameters.SplatScale = SplatScale;
	SetGaussianLightingParameters(Parameters, LightingData);

	// Per-proxy compaction path
	Parameters.GlobalBaseOffsetsBuffer = GPUResources->CompactedSplatIndicesBufferSRV;  // dummy
	Parameters.ProxyIndex = 0;
	Parameters.UseGlobalCompactionPath = 0;
	Parameters.MaxRenderBudget = 0;

	// INDIRECT DISPATCH using prepared args
	SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
	SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
	RHICmdList.DispatchIndirectComputeShader(GPUResources->IndirectDispatchArgsBuffer, 0);
	UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());

	// Transition buffer for next stage
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->ViewDataBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
}

void FGaussianSplatRenderer::DispatchCalcDistancesIndirect(
	FRHICommandListImmediate& RHICmdList,
	FGaussianSplatGPUResources* GPUResources)
{
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatCalcDistancesIndirect);

	TShaderMapRef<FGaussianSplatCalcDistancesCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	if (!ComputeShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FGaussianSplatCalcDistancesCS shader not valid"));
		return;
	}

	// Transition buffers
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortDistanceBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortKeysBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	// No need to clear distance buffer - indirect radix sort only sorts VisibleSplatCount elements,
	// and DrawSplats only renders VisibleSplatCount instances via indirect draw.
	// Unused buffer entries beyond VisibleSplatCount are never read.

	FGaussianSplatCalcDistancesCS::FParameters Parameters;
	Parameters.ViewDataBuffer = GPUResources->ViewDataBufferSRV;
	Parameters.DistanceBuffer = GPUResources->SortDistanceBufferUAV;
	Parameters.KeyBuffer = GPUResources->SortKeysBufferUAV;
	Parameters.SplatCount = GPUResources->TotalSplatCount;  // Max count for bounds
	Parameters.DispatchBaseIndex = 0u;

	// INDIRECT DISPATCH
	SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
	SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
	RHICmdList.DispatchIndirectComputeShader(GPUResources->IndirectDispatchArgsBuffer, 0);
	UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());
}

// v2886: raw GPU values are sampled AFTER CalcViewData and BEFORE raster/grade.
// This readback never stalls the GPU and allocates only 4096 * 16 = 64 KiB.
// The sampled DirectColor reflects the actor's current 102/103/104 debug view.
void FGaussianSplatRenderer::TraceShadowViewDataSamples(
	FRHICommandListImmediate& RHICmdList,
	FGaussianGlobalAccumulator* GlobalAccumulator,
	uint32 TotalSplatCount,
	uint64 RenderFrameNumber,
	int32 ShadowDebugMode)
{
	if (CVarGSPanoramaShadowStats.GetValueOnRenderThread() <= 0) return;
	if (!GlobalAccumulator || !GlobalAccumulator->GlobalViewDataBufferSRV.IsValid() ||
		!GlobalAccumulator->GlobalViewDataBuffer.IsValid() || TotalSplatCount == 0u ||
		TotalSplatCount > GlobalAccumulator->AllocatedCount) return;

	struct FPendingReadback
	{
		TUniquePtr<FRHIGPUBufferReadback> Readback;
		FBufferRHIRef SampleBuffer;
		uint32 SampleCount = 0u;
		uint64 Frame = 0u;
		int32 Mode = 0;
	};
	static FPendingReadback Pending;
	static uint64 LastSubmissionFrame = MAX_uint64;
    static double LastFinalSubmissionSeconds=-100.0;
	if (Pending.Readback && Pending.Readback->IsReady())
	{
		const uint32 Bytes = Pending.SampleCount * sizeof(FVector4f);
		const FVector4f* Values = static_cast<const FVector4f*>(Pending.Readback->Lock(Bytes));
		if (Values)
		{
			uint32 Valid = 0u, RawBelow = 0u, FinalBelow = 0u, Negative = 0u, CarrierAbove1 = 0u;
			double RawSum = 0.0, FinalSum = 0.0, CarrierSum = 0.0;
			float RawMin = 1.0f, RawMax = 0.0f, FinalMin = 1.0f, FinalMax = 0.0f;
			float CarrierMin = 1.0e30f, CarrierMax = 0.0f;
			for (uint32 I = 0; I < Pending.SampleCount; ++I)
			{
				const FVector4f& Sample = Values[I];
				if (Sample.W <= 0.5f || !FMath::IsFinite(Sample.X) ||
					!FMath::IsFinite(Sample.Y) || !FMath::IsFinite(Sample.Z)) continue;
				if (Sample.X < 0.0f || Sample.Y < 0.0f) { ++Negative; continue; }
				const float Raw = FMath::Clamp(Sample.X, 0.0f, 1.0f);
				const float Final = FMath::Clamp(Sample.Y, 0.0f, 1.0f);
				RawMin = FMath::Min(RawMin, Raw); RawMax = FMath::Max(RawMax, Raw);
				FinalMin = FMath::Min(FinalMin, Final); FinalMax = FMath::Max(FinalMax, Final);
				RawSum += Raw; FinalSum += Final;
				CarrierMin = FMath::Min(CarrierMin, Sample.Z);
				CarrierMax = FMath::Max(CarrierMax, Sample.Z);
				CarrierSum += Sample.Z;
				CarrierAbove1 += Sample.Z > 1.0f ? 1u : 0u;
				RawBelow += Raw < 0.95f ? 1u : 0u;
				FinalBelow += Final < 0.95f ? 1u : 0u;
				++Valid;
			}
			UE_LOG(LogTemp, Display,
				TEXT("[GSPanoramaShadowValues][GPU][ViewData] Frame=%llu DebugMode=%d Samples=%u Valid=%u Sentinel=%u ")
				TEXT("DebugMin=%.5f DebugMean=%.5f DebugMax=%.5f DebugBelow095=%u ")
				TEXT("CastMin=%.5f CastMean=%.5f CastMax=%.5f CastBelow095=%u ")
				TEXT("CarrierMin=%.5f CarrierMean=%.5f CarrierMax=%.5f CarrierAbove1=%u ")
				TEXT("Note=StratifiedGPUViewDataBeforeRasterNotPixelHistogram"),
				static_cast<unsigned long long>(Pending.Frame), Pending.Mode,
				Pending.SampleCount, Valid, Negative,
				Valid ? RawMin : -1.0f, Valid ? RawSum / double(Valid) : -1.0,
				Valid ? RawMax : -1.0f, RawBelow,
				Valid ? FinalMin : -1.0f, Valid ? FinalSum / double(Valid) : -1.0,
				Valid ? FinalMax : -1.0f, FinalBelow,
				Valid ? CarrierMin : -1.0f, Valid ? CarrierSum / double(Valid) : -1.0,
				Valid ? CarrierMax : -1.0f, CarrierAbove1);
			Pending.Readback->Unlock();
		}
		Pending.Readback.Reset();
		Pending.SampleBuffer.SafeRelease();
	}

	// A pending readback owns one GPU output buffer until the copy finishes.
	if (Pending.Readback || LastSubmissionFrame == RenderFrameNumber ||
				(ShadowDebugMode != 102 && ShadowDebugMode != 103 && ShadowDebugMode != 104 &&
         !(ShadowDebugMode==0 && CVarGSPanoramaShadowStats.GetValueOnRenderThread()==2))) return;
    // Mode 2 samples production Final without changing its colors/effects.
    // Throttle to one 64 KiB asynchronous readback per second, never wait.
    const double SampleNow=FPlatformTime::Seconds();
    if(ShadowDebugMode==0 && SampleNow-LastFinalSubmissionSeconds<1.0) return;
	TShaderMapRef<FGaussianShadowValueSampleCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	if (!Shader.IsValid()) return;
	const uint32 SampleCount = FMath::Min<uint32>(TotalSplatCount, 4096u);
	const uint32 Bytes = SampleCount * sizeof(FVector4f);
	FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
		TEXT("GSPanoramaShadowValueSamples"), Bytes, sizeof(FVector4f),
		BUF_UnorderedAccess | BUF_StructuredBuffer).SetInitialState(ERHIAccess::UAVCompute);
	FBufferRHIRef Output = RHICmdList.CreateBuffer(Desc);
	if (!Output.IsValid()) return;
	FUnorderedAccessViewRHIRef OutputUAV = RHICmdList.CreateUnorderedAccessView(
		Output, FRHIViewDesc::CreateBufferUAV()
			.SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(FVector4f)));
	if (!OutputUAV.IsValid()) return;

	// The next distance pass expects UAVCompute as the starting state; restore it.
	RHICmdList.Transition(FRHITransitionInfo(
		GlobalAccumulator->GlobalViewDataBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
	FGaussianShadowValueSampleCS::FParameters Parameters;
	Parameters.ViewDataBuffer = GlobalAccumulator->GlobalViewDataBufferSRV;
	Parameters.OutSamples = OutputUAV;
	Parameters.SplatCount = TotalSplatCount;
	Parameters.SampleCount = SampleCount;
	SetComputePipelineState(RHICmdList, Shader.GetComputeShader());
	SetShaderParameters(RHICmdList, Shader, Shader.GetComputeShader(), Parameters);
	RHICmdList.DispatchComputeShader(FMath::DivideAndRoundUp(SampleCount, 64u), 1, 1);
	UnsetShaderUAVs(RHICmdList, Shader, Shader.GetComputeShader());
	RHICmdList.Transition(FRHITransitionInfo(
		GlobalAccumulator->GlobalViewDataBuffer, ERHIAccess::SRVCompute, ERHIAccess::UAVCompute));

	Pending.SampleBuffer = Output;
	Pending.Readback = MakeUnique<FRHIGPUBufferReadback>(TEXT("GSPanoramaShadowValueReadback"));
	Pending.Readback->EnqueueCopy(RHICmdList, Output, Bytes);
	Pending.SampleCount = SampleCount;
	Pending.Frame = RenderFrameNumber;
	Pending.Mode = ShadowDebugMode;
	LastSubmissionFrame = RenderFrameNumber;
    if(ShadowDebugMode==0) LastFinalSubmissionSeconds=SampleNow;
	UE_LOG(LogTemp, Display,
		TEXT("[GSPanoramaShadowValues][GPU][Queued] Frame=%llu Mode=%d SampleCount=%u Bytes=%u Async=1"),
		static_cast<unsigned long long>(RenderFrameNumber), ShadowDebugMode, SampleCount, Bytes);
}

//----------------------------------------------------------------------
// Global Accumulator dispatch implementations
//----------------------------------------------------------------------

void FGaussianSplatRenderer::DispatchCalcViewDataGlobal(
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
	const FGaussianSplatLightingData* LightingData,
	const FGaussianCapturedDepthShadowRenderData* CapturedDepthData)
{
    GSPreviewPassCounters::FScopedCPUTime GSPreviewCPUTimeScope(GSPreviewPassCounters::ViewDataFull);
    GSPreviewPassCounters::AddWork(GSPreviewPassCounters::ViewDataFull, SplatCount>0 ? uint64(SplatCount):0);
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatCalcViewDataGlobal);

	FGaussianSplatCalcViewDataCS::FPermutationDomain ViewPermutation;
	ViewPermutation.Set<FGaussianSplatCalcViewDataCS::FVRBaseline>(GetVRDiagnosticMode_RenderThread() == 3);
	TShaderMapRef<FGaussianSplatCalcViewDataCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), ViewPermutation);
	if (!ComputeShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FGaussianSplatCalcViewDataCS shader not valid"));
		return;
	}

	// Transition global buffer to UAV for writing (Unknown covers both first-proxy and subsequent calls)
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalViewDataBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	FGaussianSplatCalcViewDataCS::FParameters Parameters;
	Parameters.PackedSplatBuffer = GPUResources->PackedSplatBufferSRV;
	Parameters.Float32PositionBuffer = GPUResources->Float32PositionBufferSRV;
	Parameters.UseFloat32Position = GPUResources->ShouldUseFloat32Positions() ? 1u : 0u;
	Parameters.HighPrecisionColorTexture = GPUResources->GetColorTextureOrDummy();
	Parameters.UseHighPrecisionColorOpacity = GPUResources->ShouldUseHighPrecisionColorOpacity() ? 1u : 0u;
	SetSpatialDelightingResources(Parameters, GPUResources, LocalToWorld);
	SetAdaptiveFootprintParameters(Parameters, GPUResources);
	Parameters.SHBuffer = GPUResources->SHBufferSRV;

	// Write into the GLOBAL buffer at GlobalBaseOffset
	Parameters.ViewDataBuffer = GlobalAccumulator->GlobalViewDataBufferUAV;

	// Cluster visibility
	Parameters.SplatClusterIndexBuffer = GPUResources->SplatClusterIndexBufferSRV;
	Parameters.ClusterVisibilityBitmap = GPUResources->ClusterVisibilityBitmapSRV;
	Parameters.LODClusterSelectedBitmap = GPUResources->LODClusterSelectedBitmapSRV;
	Parameters.SelectedClusterBuffer = GPUResources->SelectedClusterBufferSRV;

	if (GPUResources->bHasClusterData)
	{
		Parameters.UseClusterCulling = 1;
		Parameters.UseLODRendering = bUseLODRendering ? 1 : 0;
		Parameters.OriginalSplatCount = SplatCount - GPUResources->LODSplatCount;
	}
	else
	{
		Parameters.UseClusterCulling = 0;
		Parameters.UseLODRendering = 0;
		Parameters.OriginalSplatCount = SplatCount;
	}

	Parameters.CompactedSplatIndices = GPUResources->CompactedSplatIndicesBufferSRV;
	Parameters.UseCompaction = 0;
	Parameters.VisibleSplatCount = 0;

		// Transform matrices
	Parameters.LocalToWorld = FMatrix44f(LocalToWorld);
	Parameters.WorldToPLY = FMatrix44f(ComputeWorldToPLY(LocalToWorld));
	// Use un-jittered projection matrix so ClipPosition is stable when camera is static
	// (TAA/TSR jitter changes every frame, but we skip CalcViewData when camera is static)
	Parameters.WorldToClip = FMatrix44f(View.ViewMatrices.GetViewMatrix() * View.ViewMatrices.GetProjectionNoAAMatrix());
	Parameters.PreViewTranslation = FVector3f(View.ViewMatrices.GetPreViewTranslation());
	Parameters.WorldToView = FMatrix44f(View.ViewMatrices.GetViewMatrix());
	Parameters.CameraPosition = FVector3f(View.ViewMatrices.GetViewOrigin());

	FIntRect ViewRect = View.UnscaledViewRect;
	Parameters.ScreenSize = FVector2f(ViewRect.Width(), ViewRect.Height());

	const FMatrix& ProjMatrix = View.ViewMatrices.GetProjectionMatrix();
	Parameters.FocalLength = FVector2f(
		ProjMatrix.M[0][0] * ViewRect.Width() * 0.5f,
		ProjMatrix.M[1][1] * ViewRect.Height() * 0.5f
	);

	Parameters.SplatCount = SplatCount;
	// Use effective SH order: min(requested order, available data)
	int32 EffectiveSHOrder = FMath::Min(SHOrder, GPUResources->GetSHBands());
	Parameters.SHOrder = EffectiveSHOrder;
	// SH buffer includes DC + higher-order coefficients: band1=4, band2=9, band3=16
	Parameters.NumSHCoeffs = (EffectiveSHOrder == 0) ? 0 : (EffectiveSHOrder == 1) ? 4 : (EffectiveSHOrder == 2) ? 9 : 16;
	{
		int32 StoredSHBands = GPUResources->GetSHBands();
		Parameters.SHBufferCoeffs = (StoredSHBands == 0) ? 0 : (StoredSHBands == 1) ? 4 : (StoredSHBands == 2) ? 9 : 16;
	}
	Parameters.UseSHRendering = (EffectiveSHOrder > 0) ? 1 : 0;
	Parameters.OpacityScale = OpacityScale;
	Parameters.SplatScale = SplatScale;
	SetGaussianLightingParameters(Parameters, LightingData);

	// KEY: tell the shader where to write in the global buffer
	Parameters.GlobalBaseOffset = GlobalBaseOffset;

	// Non-compaction global path
	Parameters.GlobalBaseOffsetsBuffer = GPUResources->CompactedSplatIndicesBufferSRV;  // dummy
	Parameters.ProxyIndex = 0;
	Parameters.UseGlobalCompactionPath = 0;
	// Hard capacity guard: the compute shader checks every output index before
	// writing either valid or invalid ViewData into the global UAV.
	Parameters.MaxRenderBudget = GlobalAccumulator ? GlobalAccumulator->AllocatedCount : 0u;

	const uint32 ThreadGroupSize = 256;
	const uint32 NumGroups = FMath::DivideAndRoundUp((uint32)SplatCount, ThreadGroupSize);

	SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
	SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
	RHICmdList.DispatchComputeShader(NumGroups, 1, 1);
	UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());
	// Note: caller is responsible for the final SRVCompute transition after all proxies have written
}

void FGaussianSplatRenderer::DispatchCalcDistancesGlobal(
	FRHICommandListImmediate& RHICmdList,
	FGaussianGlobalAccumulator* GlobalAccumulator,
	int32 TotalSplatCount)
{
    GSPreviewPassCounters::FScopedCPUTime GSPreviewCPUTimeScope(GSPreviewPassCounters::DistancesFull);
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatCalcDistancesGlobal);

	TShaderMapRef<FGaussianSplatCalcDistancesCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	if (!ComputeShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FGaussianSplatCalcDistancesCS shader not valid"));
		return;
	}

	// Transition global ViewData to SRV (all proxies have finished writing)
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalViewDataBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortDistanceBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortKeysBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	FGaussianSplatCalcDistancesCS::FParameters Parameters;
	Parameters.ViewDataBuffer = GlobalAccumulator->GlobalViewDataBufferSRV;
	Parameters.DistanceBuffer = GlobalAccumulator->GlobalSortDistanceBufferUAV;
	Parameters.KeyBuffer = GlobalAccumulator->GlobalSortKeysBufferUAV;
	Parameters.SplatCount = TotalSplatCount;

	const uint32 ThreadGroupSize = 256u;
	const uint32 TotalCount = static_cast<uint32>(FMath::Max(TotalSplatCount, 0));

	// WeatherMode::Rain forces DirectSettings0.x to WetPuddles (= 1) even when
	// the user's wet strength is zero. Snow uses 2 and all other modes use 0,
	// so this test does not alter Clear/Fog/Snow scheduling.
	const bool bActualRainRenderThread =
		GDirectWeatherSurfaceMode > 0.5f &&
		GDirectWeatherSurfaceMode < 1.5f;
	const bool bChunkRainDistances =
		bActualRainRenderThread &&
		CVarGSPanoramaRainGSChunkedDistances.GetValueOnRenderThread() != 0;

	uint32 ChunkSize = static_cast<uint32>(FMath::Clamp(
		CVarGSPanoramaRainGSChunkSize.GetValueOnRenderThread(), 65536, 1048576));
	ChunkSize = FMath::Max(ThreadGroupSize, (ChunkSize / ThreadGroupSize) * ThreadGroupSize);

	SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());

	if (bChunkRainDistances && TotalCount > ChunkSize)
	{
		// Same buffer, same indices, same depth/key math. Only command granularity
		// changes so long rain+panorama workloads reach a dispatch boundary sooner.
		for (uint32 BaseIndex = 0u; BaseIndex < TotalCount; BaseIndex += ChunkSize)
		{
			const uint32 ThisCount = FMath::Min(ChunkSize, TotalCount - BaseIndex);
			Parameters.DispatchBaseIndex = BaseIndex;
			SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
			RHICmdList.DispatchComputeShader(FMath::DivideAndRoundUp(ThisCount, ThreadGroupSize), 1, 1);
			UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());
		}
	}
	else
	{
		Parameters.DispatchBaseIndex = 0u;
		SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
		RHICmdList.DispatchComputeShader(FMath::DivideAndRoundUp(TotalCount, ThreadGroupSize), 1, 1);
		UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());
	}
}

void FGaussianSplatRenderer::DispatchRadixSortGlobal(
	FRHICommandListImmediate& RHICmdList,
	FGaussianGlobalAccumulator* GlobalAccumulator,
	int32 TotalSplatCount)
{
    GSPreviewPassCounters::FScopedCPUTime GSPreviewCPUTimeScope(GSPreviewPassCounters::RadixSortFull);
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatRadixSortGlobal);

	TShaderMapRef<FRadixSortCountCS> CountShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FRadixSortPrefixSumCS> PrefixSumShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FRadixSortDigitPrefixSumCS> DigitPrefixSumShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FRadixSortScatterCS> ScatterShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	if (!CountShader.IsValid() || !PrefixSumShader.IsValid() ||
		!DigitPrefixSumShader.IsValid() || !ScatterShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Radix sort global shaders not valid"));
		return;
	}

	uint32 SortCount = (uint32)TotalSplatCount;
	uint32 NumTiles = FMath::DivideAndRoundUp(SortCount, 1024u);

	FUnorderedAccessViewRHIRef DistUAVs[2] = {
		GlobalAccumulator->GlobalSortDistanceBufferUAV,
		GlobalAccumulator->GlobalSortDistanceBufferAltUAV
	};
	FBufferRHIRef DistBuffers[2] = {
		GlobalAccumulator->GlobalSortDistanceBuffer,
		GlobalAccumulator->GlobalSortDistanceBufferAlt
	};
	FUnorderedAccessViewRHIRef KeyUAVs[2] = {
		GlobalAccumulator->GlobalSortKeysBufferUAV,
		GlobalAccumulator->GlobalSortKeysBufferAltUAV
	};
	FBufferRHIRef KeyBuffers[2] = {
		GlobalAccumulator->GlobalSortKeysBuffer,
		GlobalAccumulator->GlobalSortKeysBufferAlt
	};

	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortDistanceBufferAlt, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortKeysBufferAlt, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalRadixHistogramBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalRadixDigitOffsetBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortParamsBuffer, ERHIAccess::Unknown, ERHIAccess::SRVCompute));

	for (uint32 Pass = 0; Pass < 4; Pass++)
	{
		uint32 RadixShift = Pass * 8;
		uint32 SrcIdx = Pass & 1;
		uint32 DstIdx = 1 - SrcIdx;

		// CountCS
		{
			FRadixSortCountCS::FParameters Params;
			Params.HistogramBuffer = GlobalAccumulator->GlobalRadixHistogramBufferUAV;
			Params.SrcKeys = DistUAVs[SrcIdx];
			Params.SortParams = GlobalAccumulator->GlobalSortParamsBufferSRV;
			Params.RadixShift = RadixShift;
			Params.Count = SortCount;
			Params.NumTiles = NumTiles;
			Params.UseIndirectSort = 0;

			SetComputePipelineState(RHICmdList, CountShader.GetComputeShader());
			SetShaderParameters(RHICmdList, CountShader, CountShader.GetComputeShader(), Params);
			RHICmdList.DispatchComputeShader(NumTiles, 1, 1);
			UnsetShaderUAVs(RHICmdList, CountShader, CountShader.GetComputeShader());
		}

		RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalRadixHistogramBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

		// PrefixSumCS
		{
			FRadixSortPrefixSumCS::FParameters Params;
			Params.HistogramBuffer = GlobalAccumulator->GlobalRadixHistogramBufferUAV;
			Params.DigitOffsetBuffer = GlobalAccumulator->GlobalRadixDigitOffsetBufferUAV;
			Params.SortParams = GlobalAccumulator->GlobalSortParamsBufferSRV;
			Params.NumTiles = NumTiles;
			Params.UseIndirectSort = 0;

			SetComputePipelineState(RHICmdList, PrefixSumShader.GetComputeShader());
			SetShaderParameters(RHICmdList, PrefixSumShader, PrefixSumShader.GetComputeShader(), Params);
			RHICmdList.DispatchComputeShader(256, 1, 1);
			UnsetShaderUAVs(RHICmdList, PrefixSumShader, PrefixSumShader.GetComputeShader());
		}

		RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalRadixDigitOffsetBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

		// DigitPrefixSumCS
		{
			FRadixSortDigitPrefixSumCS::FParameters Params;
			Params.DigitOffsetBuffer = GlobalAccumulator->GlobalRadixDigitOffsetBufferUAV;

			SetComputePipelineState(RHICmdList, DigitPrefixSumShader.GetComputeShader());
			SetShaderParameters(RHICmdList, DigitPrefixSumShader, DigitPrefixSumShader.GetComputeShader(), Params);
			RHICmdList.DispatchComputeShader(1, 1, 1);
			UnsetShaderUAVs(RHICmdList, DigitPrefixSumShader, DigitPrefixSumShader.GetComputeShader());
		}

		RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalRadixDigitOffsetBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
		RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalRadixHistogramBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

		// ScatterCS
		{
			FRadixSortScatterCS::FParameters Params;
			Params.SrcKeys = DistUAVs[SrcIdx];
			Params.SrcVals = KeyUAVs[SrcIdx];
			Params.DstKeys = DistUAVs[DstIdx];
			Params.DstVals = KeyUAVs[DstIdx];
			Params.HistogramBuffer = GlobalAccumulator->GlobalRadixHistogramBufferUAV;
			Params.DigitOffsetBuffer = GlobalAccumulator->GlobalRadixDigitOffsetBufferUAV;
			Params.SortParams = GlobalAccumulator->GlobalSortParamsBufferSRV;
			Params.RadixShift = RadixShift;
			Params.Count = SortCount;
			Params.NumTiles = NumTiles;
			Params.UseIndirectSort = 0;

			SetComputePipelineState(RHICmdList, ScatterShader.GetComputeShader());
			SetShaderParameters(RHICmdList, ScatterShader, ScatterShader.GetComputeShader(), Params);
			RHICmdList.DispatchComputeShader(NumTiles, 1, 1);
			UnsetShaderUAVs(RHICmdList, ScatterShader, ScatterShader.GetComputeShader());
		}

		RHICmdList.Transition(FRHITransitionInfo(DistBuffers[DstIdx], ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
		RHICmdList.Transition(FRHITransitionInfo(KeyBuffers[DstIdx], ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
	}

	// After 4 passes (even), result is in primary buffers [0]
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortKeysBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVGraphics));
}

void FGaussianSplatRenderer::DrawSplatsGlobal(
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
	int32 VRDiagnosticMode,
    bool bPCVRLeanOutputs, FRHITexture* TileSceneDepth)
{
    GSPreviewPassCounters::FScopedCPUTime GSPreviewCPUTimeScope(GSPreviewPassCounters::DrawFull);
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatDrawGlobal);

	if (!IndexBuffer.IsValid())
	{
		return;
	}

    const bool bPCVRComputeCandidate=bPCVRLeanOutputs && VRDiagnosticMode==0 && RenderQualityMode!=4 &&
        CVarPCVRFinalOnly.GetValueOnRenderThread()!=0 && CVarPCVRComputeFinalColor.GetValueOnRenderThread()!=0 &&
        !GSPCVR::RasterClip(View) && GlobalAccumulator->bPCVRPreSortCompacted && GlobalAccumulator->GlobalSortParamsBufferSRV.IsValid();
    if (bPCVRComputeCandidate) GlobalAccumulator->EnsurePCVRFinalColors(RHICmdList,GlobalAccumulator->AllocatedCount);
    const bool bPCVRComputeColor=bPCVRComputeCandidate && GlobalAccumulator->PCVRFinalColorSRV.IsValid();
    // Log the actual cache route (not just the editor checkbox).
    if (IsVRDiagnosticLogging_RenderThread() && GSPCVR::IsActive(View))
    {
        static TMap<int32,double> V16LastLog;
        const int32 EyeId=int32(View.StereoPass);
        double& Last=V16LastLog.FindOrAdd(EyeId);
        const double Now=FPlatformTime::Seconds();
        if (Now-Last>=2.0)
        {
            Last=Now;
            UE_LOG(LogTemp,Display,TEXT("[GSPCVR][V16FinalLighting] Eye=%d CacheRequested=%d CacheApplied=%d IndirectRequested=%d Lean=%d PreSort=%d RasterClip=%d Capacity=%u"),
                EyeId,CVarPCVRComputeFinalColor.GetValueOnRenderThread(),bPCVRComputeColor?1:0,
                CVarPCVRFinalColorIndirect.GetValueOnRenderThread(),bPCVRLeanOutputs?1:0,
                GlobalAccumulator->bPCVRPreSortCompacted?1:0,GSPCVR::RasterClip(View)?1:0,GlobalAccumulator->AllocatedCount);
        }
    }
	FGaussianSplatVS::FPermutationDomain VertexPermutation;
    VertexPermutation.Set<FGaussianSplatVS::FPCVRColorCache>(bPCVRComputeColor);
	VertexPermutation.Set<FGaussianSplatVS::FVRBaseline>(VRDiagnosticMode == 3);
    VertexPermutation.Set<FGaussianSplatVS::FVRRasterClip>((VRDiagnosticMode==3 || bPCVRLeanOutputs) && GSPCVR::RasterClip(View));
	TShaderMapRef<FGaussianSplatVS> VertexShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), VertexPermutation);
	FGaussianSplatPS::FPermutationDomain Permutation;
	Permutation.Set<FGaussianSplatPS::FVRDiagnosticMode>(VRDiagnosticMode);
    Permutation.Set<FGaussianSplatPS::FPCVRLeanOutputs>(bPCVRLeanOutputs);
    Permutation.Set<FGaussianSplatPS::FPCVRFinalOnly>(bPCVRLeanOutputs && CVarPCVRFinalOnly.GetValueOnRenderThread()!=0);
    Permutation.Set<FGaussianSplatPS::FPCVRColorCache>(bPCVRComputeColor);
    const bool bV15NoFogMRT=bPCVRLeanOutputs && VRDiagnosticMode==0 &&
        CVarGSPCVRTrimFogMRT.GetValueOnRenderThread()!=0 && !IsNativeFogCompositeEnabled_RenderThread();
    Permutation.Set<FGaussianSplatPS::FPCVRNoFogOutput>(bV15NoFogMRT);
    FGSRemovalBoxGPUData RemovalProbe[GSGSRemovalBoxMax];
    const uint32 RemovalCount=bPCVRComputeColor ? GetGSRemovalBoxes_RenderThread(View.Family?View.Family->Scene:nullptr,RemovalProbe) : MAX_uint32;
    const bool bNoRemoval=bPCVRComputeColor && RemovalCount==0 && CVarPCVRSpecializeRemoval.GetValueOnRenderThread()!=0;
    Permutation.Set<FGaussianSplatPS::FPCVRNoRemoval>(bNoRemoval);
    if (bPCVRComputeColor && IsVRDiagnosticLogging_RenderThread())
    {
        static TMap<int32,double> LastRemovalLog;
        double& Last=LastRemovalLog.FindOrAdd(int32(View.StereoPass));
        const double Now=FPlatformTime::Seconds();
        if (Now-Last>=1.0)
        {
            Last=Now;
            UE_LOG(LogTemp,Display,TEXT("[GSPCVR][RemovalPS] Boxes=%u Specialized=%d Eye=%d"),RemovalCount,bNoRemoval?1:0,int32(View.StereoPass));
        }
    }
	TShaderMapRef<FGaussianSplatPS> PixelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), Permutation);
	SCOPED_DRAW_EVENTF(RHICmdList, GSVRDiagnosticDraw, TEXT("GS_VRDiag_Draw_Mode%d"), VRDiagnosticMode);

	if (!VertexShader.IsValid() || !PixelShader.IsValid())
	{
		return;
	}

	// Transition global ViewData for graphics reads
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalViewDataBuffer, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));

    if (VRDiagnosticMode == 3 && !GlobalAccumulator->bPCVRPreSortCompacted)
    {
        SCOPED_DRAW_EVENT(RHICmdList, GS_VRBaseline_VisiblePrefix);
        GlobalAccumulator->EnsureCompactionBuffersAllocated(RHICmdList);
        TShaderMapRef<FGSVRBaselineDrawArgsCS> ArgsShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortDistanceBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
        RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalDrawIndirectArgsBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
        FGSVRBaselineDrawArgsCS::FParameters Args;
        Args.SortedDistances = GlobalAccumulator->GlobalSortDistanceBufferUAV;
        Args.DrawArgs = GlobalAccumulator->GlobalDrawIndirectArgsBufferUAV;
        Args.SourceCount = static_cast<uint32>(TotalSplatCount);
        SetComputePipelineState(RHICmdList, ArgsShader.GetComputeShader());
        SetShaderParameters(RHICmdList, ArgsShader, ArgsShader.GetComputeShader(), Args);
        RHICmdList.DispatchComputeShader(1,1,1);
        UnsetShaderUAVs(RHICmdList, ArgsShader, ArgsShader.GetComputeShader());
        RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalDrawIndirectArgsBuffer, ERHIAccess::UAVCompute, ERHIAccess::IndirectArgs));
    }
	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
	// The global path must obey the same isolation rule as the per-proxy path:
	// test against UE scene depth, but never write shared depth or stencil.
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<
		false, CF_DepthNearOrEqual
	>::GetRHI();
	// Premultiplied-alpha accumulation into the GS-only color target.
	GraphicsPSOInit.BlendState = TStaticBlendState<
		// RT0: normal premultiplied GS color accumulation.
		CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha,
		// RT1: existing nearest GS radial depth.
		CW_RED, BO_Min, BF_One, BF_One, BO_Min, BF_One, BF_One,
		// RT2 v35: RGB remains replacement, alpha accumulates eligible-water coverage.
		CW_RGBA, BO_Add, BF_One, BF_Zero, BO_Add, BF_One, BF_Zero,
		// RT3: snow-only coverage-aware occlusion depth.
		CW_RED, BO_Min, BF_One, BF_One, BO_Min, BF_One, BF_One,
		// RT4: fog-only premultiplied visible-depth accumulation.
		CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha
	>::GetRHI();
    if (bPCVRLeanOutputs)
    {
        if (bV15NoFogMRT)
            GraphicsPSOInit.BlendState=TStaticBlendState<
                CW_RGBA,BO_Add,BF_One,BF_InverseSourceAlpha,BO_Add,BF_One,BF_InverseSourceAlpha,
                CW_RED,BO_Min,BF_One,BF_One,BO_Min,BF_One,BF_One>::GetRHI();
        else
            GraphicsPSOInit.BlendState=TStaticBlendState<
                CW_RGBA,BO_Add,BF_One,BF_InverseSourceAlpha,BO_Add,BF_One,BF_InverseSourceAlpha,
                CW_RED,BO_Min,BF_One,BF_One,BO_Min,BF_One,BF_One,
                CW_RGBA,BO_Add,BF_One,BF_InverseSourceAlpha,BO_Add,BF_One,BF_InverseSourceAlpha>::GetRHI();
    }
    else if (VRDiagnosticMode == 3)
    {
        GraphicsPSOInit.BlendState = TStaticBlendState<
            CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();
    }
    else if (VRDiagnosticMode == 2)
    {
        GraphicsPSOInit.BlendState = TStaticBlendState<
            CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha,
            CW_RED, BO_Min, BF_One, BF_One, BO_Min, BF_One, BF_One>::GetRHI();
    }
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;
	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

	FIntRect ViewRect = View.UnscaledViewRect;
	RHICmdList.SetViewport(
		ViewRect.Min.X, ViewRect.Min.Y, 0.0f,
		ViewRect.Max.X, ViewRect.Max.Y, 1.0f
	);

	FGaussianSplatVS::FParameters VSParameters{};
	VSParameters.PerSplatLightingEnabled = 0;
    VSParameters.PCVRReuseRadialDepth = CVarPCVRReuseRadialDepth.GetValueOnRenderThread()!=0 ? 1u : 0u;
	VSParameters.BaselineRenderQualityMode = 0;
    VSParameters.BaselineRasterClipParams = FVector4f(0,0,0,0);
	VSParameters.ViewDataBuffer = GlobalAccumulator->GlobalViewDataBufferSRV;
	VSParameters.SortKeysBuffer = GlobalAccumulator->GlobalSortKeysBufferSRV;
	VSParameters.SplatCount = TotalSplatCount;
	VSParameters.BaselineRenderQualityMode = static_cast<uint32>(FMath::Max(0, RenderQualityMode));
    VSParameters.BaselineRasterClipParams = FVector4f(float(ViewRect.Width()),float(ViewRect.Height()),GSPCVR::RasterClipMinArea(),0);
    VSParameters.BaselineRasterClipParams.W=(VRDiagnosticMode==0 && DebugMode==0 && LightingPreviewMode==0 &&
        !(GetStableShadowDebugMode()>=74 && GetStableShadowDebugMode()<=78) && GSPCVR::IsActive(View) && IsSourceQuadTrimEnabled_RenderThread())?1.0f:0.0f;
	VSParameters.DebugMode = static_cast<uint32>(FMath::Max(0, DebugMode));
	VSParameters.EnableNanite = 1;



	FGaussianSplatPS::FParameters PSParameters;
	PSParameters.PerSplatLightingEnabled = 0;
    PSParameters.PCVREarlyAlphaEnabled = CVarGSPCVREarlyAlpha.GetValueOnRenderThread()!=0 ? 1u : 0u;
    PSParameters.PCVRReuseRadialDepth = CVarPCVRReuseRadialDepth.GetValueOnRenderThread()!=0 ? 1u : 0u;
	SetVelocityPSParameters(PSParameters, View, GlobalAccumulator);
	PSParameters.LightingPreviewMode = static_cast<uint32>(FMath::Max(0, LightingPreviewMode));
	PSParameters.RenderQualityMode = static_cast<uint32>(FMath::Max(0, RenderQualityMode));
	PSParameters.UseProxyLightingMaskAsMain = bUseProxyLightingMaskAsMain ? 1u : 0u;
	PSParameters.ProxyLightingIntensityScale = FMath::Max(0.0f, ProxyLightingIntensityScale);
	PSParameters.DayNightBlend = FMath::Clamp(DayNightBlend, 0.0f, 1.0f);
	PSParameters.RegionalColorGradeParams0 = RegionalColorGradeParams0;
	PSParameters.RegionalColorGradeParams1 = RegionalColorGradeParams1;
	PSParameters.StableShadowColorRetention = FMath::Clamp(StableShadowColorRetention, 0.0f, 1.0f);
	SetProxyCapturePSParameters(PSParameters, View);
	VSParameters.PerSplatLightingEnabled = (VRDiagnosticMode==0 && DebugMode==0 && LightingPreviewMode==0 && RenderQualityMode!=4 &&
        !(GetStableShadowDebugMode()>=74 && GetStableShadowDebugMode()<=78) && GSPCVR::IsActive(View) && CVarPCVertexLighting.GetValueOnRenderThread()!=0) ? 1u : 0u;
	PSParameters.PerSplatLightingEnabled = VSParameters.PerSplatLightingEnabled;
	VSParameters.DayNightBlend = PSParameters.DayNightBlend;
	VSParameters.RegionalColorGradeParams0 = PSParameters.RegionalColorGradeParams0;
	VSParameters.RegionalColorGradeParams1 = PSParameters.RegionalColorGradeParams1;
	VSParameters.StableShadowColorRetention = PSParameters.StableShadowColorRetention;
	VSParameters.ProxyShadowDetailSettings = PSParameters.ProxyShadowDetailSettings;
	VSParameters.ProxyReflectedLightSettings = PSParameters.ProxyReflectedLightSettings;
	VSParameters.ProxyReflectedColorSettings = PSParameters.ProxyReflectedColorSettings;
	VSParameters.ProxyShadowBaseSettings = PSParameters.ProxyShadowBaseSettings;
	VSParameters.ProxyShadowBaseTint = PSParameters.ProxyShadowBaseTint;
	VSParameters.ProxyLightDepthForwardEnabled = PSParameters.ProxyLightDepthForwardEnabled;
    if (bPCVRComputeColor)
    {
        SCOPED_DRAW_EVENT(RHICmdList,GaussianSplatPCVRFinalColor);
        const bool bV16IndirectColor=CVarPCVRFinalColorIndirect.GetValueOnRenderThread()!=0 &&
            GlobalAccumulator->GlobalSortIndirectArgsGlobalBuffer.IsValid();
        FGaussianPCVRFinalColorCS::FPermutationDomain ColorPermutation;
        ColorPermutation.Set<FGaussianPCVRFinalColorCS::FVisibleIndirect>(bV16IndirectColor);
        TShaderMapRef<FGaussianPCVRFinalColorCS> ColorCS(GetGlobalShaderMap(GMaxRHIFeatureLevel),ColorPermutation);
        FGaussianPCVRFinalColorCS::FParameters P{};
        P.ViewDataBuffer=GlobalAccumulator->GlobalViewDataBufferSRV;
        P.SortKeysBuffer=GlobalAccumulator->GlobalSortKeysBufferSRV;
        P.SortParams=GlobalAccumulator->GlobalSortParamsBufferSRV;
        P.PCVRFinalColorsOut=GlobalAccumulator->PCVRFinalColorUAV;
        P.SourceCapacity=GlobalAccumulator->AllocatedCount;
        P.DayNightBlend=PSParameters.DayNightBlend;
        P.RegionalColorGradeParams0=PSParameters.RegionalColorGradeParams0;
        P.RegionalColorGradeParams1=PSParameters.RegionalColorGradeParams1;
        P.StableShadowColorRetention=PSParameters.StableShadowColorRetention;
        P.ProxyShadowDetailSettings=PSParameters.ProxyShadowDetailSettings;
        P.ProxyReflectedLightSettings=PSParameters.ProxyReflectedLightSettings;
        P.ProxyReflectedColorSettings=PSParameters.ProxyReflectedColorSettings;
        P.ProxyShadowBaseSettings=PSParameters.ProxyShadowBaseSettings;
        P.ProxyShadowBaseTint=PSParameters.ProxyShadowBaseTint;
        P.ProxyLightDepthForwardEnabled=PSParameters.ProxyLightDepthForwardEnabled;
        RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalViewDataBuffer,ERHIAccess::Unknown,ERHIAccess::SRVCompute));
        RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortKeysBuffer,ERHIAccess::Unknown,ERHIAccess::SRVCompute));
        RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->PCVRFinalColorBuffer,ERHIAccess::Unknown,ERHIAccess::UAVCompute));
        SetComputePipelineState(RHICmdList,ColorCS.GetComputeShader());
        SetShaderParameters(RHICmdList,ColorCS,ColorCS.GetComputeShader(),P);
        // V16-D: reuse the *current eye's* GPU compacted sort count. The indirect
        // buffer contains ceil(visible/1024) groups and was produced by the normal
        // compact/prefix path. V16 indirect HLSL processes 4 slots per lane.
        if (bV16IndirectColor)
            RHICmdList.DispatchIndirectComputeShader(GlobalAccumulator->GlobalSortIndirectArgsGlobalBuffer,0);
        else
            RHICmdList.DispatchComputeShader(FMath::DivideAndRoundUp(P.SourceCapacity,256u),1,1);
        UnsetShaderUAVs(RHICmdList,ColorCS,ColorCS.GetComputeShader());
        RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->PCVRFinalColorBuffer,ERHIAccess::UAVCompute,ERHIAccess::SRVGraphics));
        RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalViewDataBuffer,ERHIAccess::SRVCompute,ERHIAccess::SRVGraphics));
        RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortKeysBuffer,ERHIAccess::SRVCompute,ERHIAccess::SRVGraphics));
        VSParameters.PCVRFinalColors=GlobalAccumulator->PCVRFinalColorSRV;
        VSParameters.PerSplatLightingEnabled=2u;
        PSParameters.PerSplatLightingEnabled=2u;
        SetGraphicsPipelineState(RHICmdList,GraphicsPSOInit,0);
    }
    FBufferRHIRef TileRasterArgs;
    bool bTileDrawn=false;
    if (bPCVRComputeColor && (bNoRemoval || IsPCVRTileRemovalEnabled_RenderThread() || FParse::Param(FCommandLine::Get(),TEXT("GSPCVRLODTest"))) && VRDiagnosticMode==0 && DebugMode==0 && LightingPreviewMode==0 &&
        GetStableShadowDebugMode()==0 && CVarPCVRShadowBlueOverlay.GetValueOnRenderThread()==0 &&
        IsPCVRClearSurface_RenderThread() && !IsDirectWetSurfaceEnabled_RenderThread() && !IsDirectSnowOverlayEnabled_RenderThread() &&
        DrawPCVRTileComposite(RHICmdList,View,GlobalAccumulator,TileSceneDepth,RenderQualityMode,VSParameters.BaselineRasterClipParams.W>0.5f,TileRasterArgs))
    {
        bTileDrawn=true;
        SetGraphicsPipelineState(RHICmdList,GraphicsPSOInit,0);
    }
    if(IsVRDiagnosticLogging_RenderThread())
    {
        static TMap<int32,double> LastRoute;
        double& Last=LastRoute.FindOrAdd(int32(View.StereoPass));
        const double Now=FPlatformTime::Seconds();
        if(Now-Last>=1.0)
        {
            Last=Now;
            const auto* Tile=IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("gs.PCVR.TileComposite"));
            const auto* Fog=IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("gs.PCVR.PackedFog"));
            UE_LOG(LogTemp,Display,TEXT("[GSPCVR][Route] Eye=%d Tiled=%d Requested=%d PackedFog=%d ComputeColor=%d RemovalBoxes=%u RemovalOptIn=%d SourceCapacity=%u Quality=%d Clear=%d Wet=%d Snow=%d Diagnostic=%d Debug=%d LightingPreview=%d StableDebug=%d Blue=%d Rect=%dx%d"),
                int32(View.StereoPass),bTileDrawn?1:0,Tile?Tile->GetValueOnRenderThread():-1,Fog?Fog->GetValueOnRenderThread():-1,bPCVRComputeColor?1:0,RemovalCount,IsPCVRTileRemovalEnabled_RenderThread()?1:0,GlobalAccumulator->AllocatedCount,RenderQualityMode,
                IsPCVRClearSurface_RenderThread()?1:0,IsDirectWetSurfaceEnabled_RenderThread()?1:0,IsDirectSnowOverlayEnabled_RenderThread()?1:0,VRDiagnosticMode,DebugMode,LightingPreviewMode,GetStableShadowDebugMode(),CVarPCVRShadowBlueOverlay.GetValueOnRenderThread(),View.UnscaledViewRect.Width(),View.UnscaledViewRect.Height());
        }
    }
	SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), VSParameters);
	SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PSParameters);

	RHICmdList.SetStreamSource(0, nullptr, 0);
    const bool bIndirectDraw=TileRasterArgs.IsValid() || VRDiagnosticMode==3 || GlobalAccumulator->bPCVRPreSortCompacted;
    {
        // Only the actual raster draw: exclude final-color compute, setup and
        // diagnostic count copies from the GPU timer being investigated.
        SCOPED_DRAW_EVENTF(RHICmdList,GSPCVRRasterOnly,TEXT("GSPCVR_RasterOnly Eye=%d"),int32(View.StereoPass));
        const int32 EyeBucket = GSRasterEyeBucket(View);
        GSPreviewPassCounters::RecordRasterDraw(EyeBucket, uint32(FMath::Max(0,TotalSplatCount)),
            uint64(ViewRect.Width())*uint64(ViewRect.Height()));
        const auto SubmitDraw = [&]()
        {
            if (bIndirectDraw)
                RHICmdList.DrawIndexedPrimitiveIndirect(IndexBuffer,TileRasterArgs.IsValid()?TileRasterArgs:GlobalAccumulator->GlobalDrawIndirectArgsBuffer,0);
            else
                RHICmdList.DrawIndexedPrimitive(IndexBuffer,0,0,4,0,2,TotalSplatCount);
        };
        if (EyeBucket==0) { SCOPED_DRAW_EVENT(RHICmdList,GSPCVRRaster_LeftEye); SubmitDraw(); }
        else if (EyeBucket==1) { SCOPED_DRAW_EVENT(RHICmdList,GSPCVRRaster_RightEye); SubmitDraw(); }
        else { SCOPED_DRAW_EVENT(RHICmdList,GSPCVRRaster_OtherEye); SubmitDraw(); }
    }
    if(bIndirectDraw)
        LogVRDiagnosticIndirectCount(RHICmdList,GlobalAccumulator,VRDiagnosticMode,GSRasterEyeBucket(View),uint32(TotalSplatCount),false);
}


void FGaussianSplatRenderer::DrawSnowSplatsGlobal(
	FRHICommandListImmediate& RHICmdList,
	const FSceneView& View,
	FGaussianGlobalAccumulator* GlobalAccumulator,
	FBufferRHIRef IndexBuffer,
	int32 TotalSplatCount)
{
    GSPreviewPassCounters::FScopedCPUTime GSPreviewCPUTimeScope(GSPreviewPassCounters::SnowDrawFull);
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSnowOverlayGlobal);

	if (!GlobalAccumulator || !IndexBuffer.IsValid() || TotalSplatCount <= 0 ||
		!IsDirectSnowOverlayEnabled_RenderThread())
	{
		return;
	}

	TShaderMapRef<FGaussianSnowOverlayVS> VertexShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FGaussianSnowOverlayPS> PixelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	if (!VertexShader.IsValid() || !PixelShader.IsValid())
	{
		return;
	}

	RHICmdList.Transition(FRHITransitionInfo(
		GlobalAccumulator->GlobalViewDataBuffer,
		ERHIAccess::Unknown,
		ERHIAccess::SRVGraphics));

	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<
		false, CF_DepthNearOrEqual
	>::GetRHI();
	GraphicsPSOInit.BlendState = TStaticBlendState<
		CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha,
		CW_RED, BO_Min, BF_One, BF_One, BO_Min, BF_One, BF_One
	>::GetRHI();
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;
	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

	const FIntRect ViewRect = View.UnscaledViewRect;
	RHICmdList.SetViewport(
		ViewRect.Min.X, ViewRect.Min.Y, 0.0f,
		ViewRect.Max.X, ViewRect.Max.Y, 1.0f);

	FGaussianSnowOverlayVS::FParameters VSParameters;
	VSParameters.ViewDataBuffer = GlobalAccumulator->GlobalViewDataBufferSRV;
	VSParameters.SortKeysBuffer = GlobalAccumulator->GlobalSortKeysBufferSRV;
	VSParameters.SplatCount = static_cast<uint32>(FMath::Max(0, TotalSplatCount));
	VSParameters.SnowTranslatedWorldToClip = FMatrix44f(
		View.ViewMatrices.GetTranslatedViewMatrix() *
		View.ViewMatrices.GetProjectionNoAAMatrix());
	VSParameters.SnowOverlayParams = FVector4f(
		FMath::Clamp(GDirectSplatSurfaceEffectSettings2.X, 0.0f, 50.0f),
		FMath::Clamp(GDirectSplatSurfaceEffectSettings2.Y, 1.0f, 1.20f),
		0.018f,
		1.35f);
	VSParameters.SnowOverlayColor = FVector4f(
		FMath::Max(GDirectSplatSurfaceEffectSettings1.X, 0.0f),
		FMath::Max(GDirectSplatSurfaceEffectSettings1.Y, 0.0f),
		FMath::Max(GDirectSplatSurfaceEffectSettings1.Z, 0.0f),
		FMath::Clamp(GDirectSplatSurfaceEffectSettings1.W, 0.0f, 2.0f));
	VSParameters.SnowTuningParams = GDirectSnowTuningSettings;
	SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), VSParameters);

	FGaussianSnowOverlayPS::FParameters RemovalPSParameters;
	SetGSRemovalBoxesPS(RemovalPSParameters, View);
	RemovalPSParameters.PreViewTranslation = FVector3f(View.ViewMatrices.GetPreViewTranslation());
	RemovalPSParameters.PuddleClipToWorld = FMatrix44f(View.ViewMatrices.GetInvViewProjectionMatrix());
	RemovalPSParameters.PuddleViewport = FVector4f(float(ViewRect.Min.X),float(ViewRect.Min.Y),
		float(FMath::Max(1,ViewRect.Width())),float(FMath::Max(1,ViewRect.Height())));
	SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), RemovalPSParameters);

	RHICmdList.SetStreamSource(0, nullptr, 0);
    if(GlobalAccumulator->bPCVRPreSortCompacted)
        RHICmdList.DrawIndexedPrimitiveIndirect(IndexBuffer,GlobalAccumulator->GlobalDrawIndirectArgsBuffer,0);
    else
	RHICmdList.DrawIndexedPrimitive(
		IndexBuffer,
		0,
		0,
		4,
		0,
		2,
		TotalSplatCount);
}

//----------------------------------------------------------------------
// Global Accumulator + Nanite Compaction dispatch implementations
//----------------------------------------------------------------------

void FGaussianSplatRenderer::DispatchGatherVisibleCount(
	FRHICommandListImmediate& RHICmdList,
	FGaussianSplatGPUResources* GPUResources,
	FGaussianGlobalAccumulator* GlobalAccumulator,
	int32 ProxyIndex)
{
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatGatherVisibleCount);

	TShaderMapRef<FGatherVisibleCountsCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	if (!ComputeShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FGatherVisibleCountsCS shader not valid"));
		return;
	}

	// VisibleSplatCountBuffer is already in SRVCompute after DispatchCompactSplats
	// GlobalVisibleCountArrayBuffer: transition to UAV for writing
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalVisibleCountArrayBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	FGatherVisibleCountsCS::FParameters Parameters;
	Parameters.PerProxyVisibleCount  = GPUResources->VisibleSplatCountBufferSRV;
	Parameters.GlobalVisibleCountArray = GlobalAccumulator->GlobalVisibleCountArrayBufferUAV;
	Parameters.ProxyIndex            = (uint32)ProxyIndex;

	SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
	SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
	RHICmdList.DispatchComputeShader(1, 1, 1);
	UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());

	// UAV barrier so next proxy's write (or the PrefixSum read) sees this result
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalVisibleCountArrayBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
}

void FGaussianSplatRenderer::DispatchPrefixSumVisibleCounts(
	FRHICommandListImmediate& RHICmdList,
	FGaussianGlobalAccumulator* GlobalAccumulator,
	int32 ProxyCount,
	uint32 MaxRenderBudget)
{
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatPrefixSumVisibleCounts);

	TShaderMapRef<FPrefixSumVisibleCountsCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	if (!ComputeShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FPrefixSumVisibleCountsCS shader not valid"));
		return;
	}

	// GlobalVisibleCountArray: all GatherCS writes done ??transition to SRV for reading
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalVisibleCountArrayBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));

	// Output buffers ??UAV for writing
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalBaseOffsetsBuffer,            ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalCalcDistIndirectArgsBuffer,    ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortIndirectArgsGlobalBuffer,  ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortParamsBuffer,              ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalDrawIndirectArgsBuffer,        ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	FPrefixSumVisibleCountsCS::FParameters Parameters;
	Parameters.GlobalVisibleCountArray   = GlobalAccumulator->GlobalVisibleCountArrayBufferSRV;
	Parameters.GlobalBaseOffsets         = GlobalAccumulator->GlobalBaseOffsetsBufferUAV;
	Parameters.GlobalCalcDistIndirectArgs = GlobalAccumulator->GlobalCalcDistIndirectArgsBufferUAV;
	Parameters.GlobalSortIndirectArgs    = GlobalAccumulator->GlobalSortIndirectArgsGlobalBufferUAV;
	Parameters.GlobalSortParams          = GlobalAccumulator->GlobalSortParamsBufferUAV;
	Parameters.GlobalDrawIndirectArgs    = GlobalAccumulator->GlobalDrawIndirectArgsBufferUAV;
	Parameters.ProxyCount                = (uint32)ProxyCount;
	Parameters.MaxRenderBudget           = MaxRenderBudget;

	SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
	SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
	RHICmdList.DispatchComputeShader(1, 1, 1);
	UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());

	// GlobalBaseOffsetsBuffer ??SRV so CalcViewDataCompactedGlobal can read it
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalBaseOffsetsBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));

	// Indirect arg buffers ??IndirectArgs state for the GPU-driven dispatches/draw
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalCalcDistIndirectArgsBuffer,   ERHIAccess::UAVCompute, ERHIAccess::IndirectArgs));
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortIndirectArgsGlobalBuffer, ERHIAccess::UAVCompute, ERHIAccess::IndirectArgs));
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalDrawIndirectArgsBuffer,       ERHIAccess::UAVCompute, ERHIAccess::IndirectArgs));

	// GlobalSortParamsBuffer ??SRV for radix sort shaders (UseIndirectSort=1)
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortParamsBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
}

void FGaussianSplatRenderer::DispatchCalcViewDataCompactedGlobal(
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
	const FGaussianSplatLightingData* LightingData,
	const FGaussianCapturedDepthShadowRenderData* CapturedDepthData)
{
    GSPreviewPassCounters::FScopedCPUTime GSPreviewCPUTimeScope(GSPreviewPassCounters::ViewDataCompacted);
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatCalcViewDataCompactedGlobal);

	FGaussianSplatCalcViewDataCS::FPermutationDomain ViewPermutation;
	ViewPermutation.Set<FGaussianSplatCalcViewDataCS::FVRBaseline>(GetVRDiagnosticMode_RenderThread() == 3);
	TShaderMapRef<FGaussianSplatCalcViewDataCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), ViewPermutation);
	if (!ComputeShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FGaussianSplatCalcViewDataCS shader not valid"));
		return;
	}

	// Transition global ViewData buffer to UAV (Unknown handles first proxy and subsequent proxies)
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalViewDataBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	FGaussianSplatCalcViewDataCS::FParameters Parameters;
	Parameters.PackedSplatBuffer = GPUResources->PackedSplatBufferSRV;
	Parameters.Float32PositionBuffer = GPUResources->Float32PositionBufferSRV;
	Parameters.UseFloat32Position = GPUResources->ShouldUseFloat32Positions() ? 1u : 0u;
	Parameters.HighPrecisionColorTexture = GPUResources->GetColorTextureOrDummy();
	Parameters.UseHighPrecisionColorOpacity = GPUResources->ShouldUseHighPrecisionColorOpacity() ? 1u : 0u;
	SetSpatialDelightingResources(Parameters, GPUResources, LocalToWorld);
	SetAdaptiveFootprintParameters(Parameters, GPUResources);
	Parameters.SHBuffer = GPUResources->SHBufferSRV;

	// Write into GLOBAL buffer
	Parameters.ViewDataBuffer = GlobalAccumulator->GlobalViewDataBufferUAV;

	// Cluster visibility data
	Parameters.SplatClusterIndexBuffer   = GPUResources->bHasClusterData ? GPUResources->SplatClusterIndexBufferSRV : GPUResources->VisibleSplatCountBufferSRV;
	Parameters.ClusterVisibilityBitmap   = GPUResources->bHasClusterData ? GPUResources->ClusterVisibilityBitmapSRV : GPUResources->VisibleSplatCountBufferSRV;
	Parameters.LODClusterSelectedBitmap  = GPUResources->bHasClusterData ? GPUResources->LODClusterSelectedBitmapSRV : GPUResources->VisibleSplatCountBufferSRV;
	Parameters.SelectedClusterBuffer     = GPUResources->bHasClusterData ? GPUResources->SelectedClusterBufferSRV : GPUResources->VisibleSplatCountBufferSRV;
	Parameters.UseClusterCulling         = GPUResources->bHasClusterData ? 1u : 0u;
	Parameters.UseLODRendering           = 0;
	Parameters.OriginalSplatCount        = OriginalSplatCount;

	// COMPACTION MODE
	Parameters.CompactedSplatIndices = GPUResources->CompactedSplatIndicesBufferSRV;
	Parameters.UseCompaction         = 1;
	Parameters.VisibleSplatCount     = SplatCount;

	// GLOBAL COMPACTION PATH
	Parameters.GlobalBaseOffsetsBuffer   = GlobalAccumulator->GlobalBaseOffsetsBufferSRV;
	Parameters.ProxyIndex                = (uint32)ProxyIndex;
	Parameters.UseGlobalCompactionPath   = 1;
	Parameters.GlobalBaseOffset          = 0;
	Parameters.MaxRenderBudget           = MaxRenderBudget;

		// Transform matrices
	Parameters.LocalToWorld    = FMatrix44f(LocalToWorld);
	Parameters.WorldToPLY      = FMatrix44f(ComputeWorldToPLY(LocalToWorld));
	// Use un-jittered projection matrix so ClipPosition is stable when camera is static
	// (TAA/TSR jitter changes every frame, but we skip CalcViewData when camera is static)
	Parameters.WorldToClip     = FMatrix44f(View.ViewMatrices.GetViewMatrix() * View.ViewMatrices.GetProjectionNoAAMatrix());
	Parameters.PreViewTranslation = FVector3f(View.ViewMatrices.GetPreViewTranslation());
	Parameters.WorldToView     = FMatrix44f(View.ViewMatrices.GetViewMatrix());
	Parameters.CameraPosition  = FVector3f(View.ViewMatrices.GetViewOrigin());

	FIntRect ViewRect = View.UnscaledViewRect;
	Parameters.ScreenSize = FVector2f(ViewRect.Width(), ViewRect.Height());

	const FMatrix& ProjMatrix = View.ViewMatrices.GetProjectionMatrix();
	Parameters.FocalLength = FVector2f(
		ProjMatrix.M[0][0] * ViewRect.Width() * 0.5f,
		ProjMatrix.M[1][1] * ViewRect.Height() * 0.5f
	);

	Parameters.SplatCount    = SplatCount;
	// Use actual available SH data, not requested SH order
	int32 EffectiveSHOrder = FMath::Min(SHOrder, GPUResources->GetSHBands());
	Parameters.SHOrder       = EffectiveSHOrder;
	// SH buffer includes DC + higher-order coefficients: band1=4, band2=9, band3=16
	Parameters.NumSHCoeffs   = (EffectiveSHOrder == 0) ? 0 : (EffectiveSHOrder == 1) ? 4 : (EffectiveSHOrder == 2) ? 9 : 16;
	{
		int32 StoredSHBands = GPUResources->GetSHBands();
		Parameters.SHBufferCoeffs = (StoredSHBands == 0) ? 0 : (StoredSHBands == 1) ? 4 : (StoredSHBands == 2) ? 9 : 16;
	}
	Parameters.UseSHRendering = (EffectiveSHOrder > 0) ? 1 : 0;
	Parameters.OpacityScale  = OpacityScale;
	Parameters.SplatScale    = SplatScale;
	SetGaussianLightingParameters(Parameters, LightingData);

	// Use the per-proxy indirect dispatch args (filled by DispatchPrepareIndirectArgs)
	SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
	SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
	RHICmdList.DispatchIndirectComputeShader(GPUResources->IndirectDispatchArgsBuffer, 0);
	UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());

	// Note: caller transitions GlobalViewDataBuffer to SRVCompute after all proxies are done
}

void FGaussianSplatRenderer::DispatchCalcDistancesGlobalIndirect(
	FRHICommandListImmediate& RHICmdList,
	FGaussianGlobalAccumulator* GlobalAccumulator)
{
    GSPreviewPassCounters::FScopedCPUTime GSPreviewCPUTimeScope(GSPreviewPassCounters::DistancesIndirect);
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatCalcDistancesGlobalIndirect);

	TShaderMapRef<FGaussianSplatCalcDistancesCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	if (!ComputeShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FGaussianSplatCalcDistancesCS shader not valid"));
		return;
	}

	// All proxies have finished writing ??transition global ViewData to SRV
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalViewDataBuffer,    ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortDistanceBuffer, ERHIAccess::Unknown,   ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortKeysBuffer,     ERHIAccess::Unknown,   ERHIAccess::UAVCompute));

	FGaussianSplatCalcDistancesCS::FParameters Parameters;
	Parameters.ViewDataBuffer  = GlobalAccumulator->GlobalViewDataBufferSRV;
	Parameters.DistanceBuffer  = GlobalAccumulator->GlobalSortDistanceBufferUAV;
	Parameters.KeyBuffer       = GlobalAccumulator->GlobalSortKeysBufferUAV;
	// AllocatedCount is always >= TotalVisible ??safe upper bound for the shader guard
	Parameters.SplatCount      = GlobalAccumulator->AllocatedCount;
	Parameters.DispatchBaseIndex = 0u;

	// GlobalCalcDistIndirectArgsBuffer is in IndirectArgs state from DispatchPrefixSumVisibleCounts
	SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
	SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
	RHICmdList.DispatchIndirectComputeShader(GlobalAccumulator->GlobalCalcDistIndirectArgsBuffer, 0);
	UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());
}

void FGaussianSplatRenderer::DispatchRadixSortGlobalIndirect(
	FRHICommandListImmediate& RHICmdList,
	FGaussianGlobalAccumulator* GlobalAccumulator)
{
    GSPreviewPassCounters::FScopedCPUTime GSPreviewCPUTimeScope(GSPreviewPassCounters::RadixSortIndirect);
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatRadixSortGlobalIndirect);

	TShaderMapRef<FRadixSortCountCS> CountShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FRadixSortPrefixSumCS> PrefixSumShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FRadixSortDigitPrefixSumCS> DigitPrefixSumShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FRadixSortScatterCS> ScatterShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	if (!CountShader.IsValid() || !PrefixSumShader.IsValid() ||
		!DigitPrefixSumShader.IsValid() || !ScatterShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Radix sort global indirect shaders not valid"));
		return;
	}

	FUnorderedAccessViewRHIRef DistUAVs[2] = {
		GlobalAccumulator->GlobalSortDistanceBufferUAV,
		GlobalAccumulator->GlobalSortDistanceBufferAltUAV
	};
	FBufferRHIRef DistBuffers[2] = {
		GlobalAccumulator->GlobalSortDistanceBuffer,
		GlobalAccumulator->GlobalSortDistanceBufferAlt
	};
	FUnorderedAccessViewRHIRef KeyUAVs[2] = {
		GlobalAccumulator->GlobalSortKeysBufferUAV,
		GlobalAccumulator->GlobalSortKeysBufferAltUAV
	};
	FBufferRHIRef KeyBuffers[2] = {
		GlobalAccumulator->GlobalSortKeysBuffer,
		GlobalAccumulator->GlobalSortKeysBufferAlt
	};

	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortDistanceBufferAlt,  ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortKeysBufferAlt,      ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalRadixHistogramBuffer,   ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalRadixDigitOffsetBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	// GlobalSortParamsBuffer and GlobalSortIndirectArgsGlobalBuffer are already
	// in SRVCompute / IndirectArgs state from DispatchPrefixSumVisibleCounts

	// 4 radix passes: bits 0-7, 8-15, 16-23, 24-31
	for (uint32 Pass = 0; Pass < 4; Pass++)
	{
		uint32 RadixShift = Pass * 8;
		uint32 SrcIdx = Pass & 1;
		uint32 DstIdx = 1 - SrcIdx;

		// --- CountCS: indirect dispatch (GlobalSortIndirectArgsGlobalBuffer) ---
		{
			FRadixSortCountCS::FParameters Params;
			Params.HistogramBuffer = GlobalAccumulator->GlobalRadixHistogramBufferUAV;
			Params.SrcKeys         = DistUAVs[SrcIdx];
			Params.SortParams      = GlobalAccumulator->GlobalSortParamsBufferSRV;
			Params.RadixShift      = RadixShift;
			Params.Count           = 0;   // Unused when UseIndirectSort=1
			Params.NumTiles        = 0;   // Unused when UseIndirectSort=1
			Params.UseIndirectSort = 1;

			SetComputePipelineState(RHICmdList, CountShader.GetComputeShader());
			SetShaderParameters(RHICmdList, CountShader, CountShader.GetComputeShader(), Params);
			RHICmdList.DispatchIndirectComputeShader(GlobalAccumulator->GlobalSortIndirectArgsGlobalBuffer, 0);
			UnsetShaderUAVs(RHICmdList, CountShader, CountShader.GetComputeShader());
		}

		RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalRadixHistogramBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

		// --- PrefixSumCS: fixed dispatch (256, 1, 1) ??reads NumTiles from GlobalSortParamsBuffer ---
		{
			FRadixSortPrefixSumCS::FParameters Params;
			Params.HistogramBuffer    = GlobalAccumulator->GlobalRadixHistogramBufferUAV;
			Params.DigitOffsetBuffer  = GlobalAccumulator->GlobalRadixDigitOffsetBufferUAV;
			Params.SortParams         = GlobalAccumulator->GlobalSortParamsBufferSRV;
			Params.NumTiles           = 0;   // Unused when UseIndirectSort=1
			Params.UseIndirectSort    = 1;

			SetComputePipelineState(RHICmdList, PrefixSumShader.GetComputeShader());
			SetShaderParameters(RHICmdList, PrefixSumShader, PrefixSumShader.GetComputeShader(), Params);
			RHICmdList.DispatchComputeShader(256, 1, 1);
			UnsetShaderUAVs(RHICmdList, PrefixSumShader, PrefixSumShader.GetComputeShader());
		}

		RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalRadixDigitOffsetBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

		// --- DigitPrefixSumCS: single dispatch ---
		{
			FRadixSortDigitPrefixSumCS::FParameters Params;
			Params.DigitOffsetBuffer = GlobalAccumulator->GlobalRadixDigitOffsetBufferUAV;

			SetComputePipelineState(RHICmdList, DigitPrefixSumShader.GetComputeShader());
			SetShaderParameters(RHICmdList, DigitPrefixSumShader, DigitPrefixSumShader.GetComputeShader(), Params);
			RHICmdList.DispatchComputeShader(1, 1, 1);
			UnsetShaderUAVs(RHICmdList, DigitPrefixSumShader, DigitPrefixSumShader.GetComputeShader());
		}

		RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalRadixDigitOffsetBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
		RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalRadixHistogramBuffer,   ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

		// --- ScatterCS: indirect dispatch ---
		{
			FRadixSortScatterCS::FParameters Params;
			Params.SrcKeys         = DistUAVs[SrcIdx];
			Params.SrcVals         = KeyUAVs[SrcIdx];
			Params.DstKeys         = DistUAVs[DstIdx];
			Params.DstVals         = KeyUAVs[DstIdx];
			Params.HistogramBuffer = GlobalAccumulator->GlobalRadixHistogramBufferUAV;
			Params.DigitOffsetBuffer = GlobalAccumulator->GlobalRadixDigitOffsetBufferUAV;
			Params.SortParams      = GlobalAccumulator->GlobalSortParamsBufferSRV;
			Params.RadixShift      = RadixShift;
			Params.Count           = 0;   // Unused when UseIndirectSort=1
			Params.NumTiles        = 0;   // Unused when UseIndirectSort=1
			Params.UseIndirectSort = 1;

			SetComputePipelineState(RHICmdList, ScatterShader.GetComputeShader());
			SetShaderParameters(RHICmdList, ScatterShader, ScatterShader.GetComputeShader(), Params);
			RHICmdList.DispatchIndirectComputeShader(GlobalAccumulator->GlobalSortIndirectArgsGlobalBuffer, 0);
			UnsetShaderUAVs(RHICmdList, ScatterShader, ScatterShader.GetComputeShader());
		}

		RHICmdList.Transition(FRHITransitionInfo(DistBuffers[DstIdx], ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
		RHICmdList.Transition(FRHITransitionInfo(KeyBuffers[DstIdx],  ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
	}

	// After 4 passes (even), result is in primary buffers [0]
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortKeysBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVGraphics));
}

void FGaussianSplatRenderer::DrawSplatsGlobalIndirect(
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
	int32 VRDiagnosticMode,
    bool bPCVRLeanOutputs, FRHITexture* TileSceneDepth)
{
    GSPreviewPassCounters::FScopedCPUTime GSPreviewCPUTimeScope(GSPreviewPassCounters::DrawIndirect);
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatDrawGlobalIndirect);

	if (!IndexBuffer.IsValid())
	{
		return;
	}

    const bool bPCVRComputeCandidate=bPCVRLeanOutputs && VRDiagnosticMode==0 && RenderQualityMode!=4 &&
        CVarPCVRFinalOnly.GetValueOnRenderThread()!=0 && CVarPCVRComputeFinalColor.GetValueOnRenderThread()!=0 &&
        !GSPCVR::RasterClip(View) && GlobalAccumulator->bPCVRPreSortCompacted && GlobalAccumulator->GlobalSortParamsBufferSRV.IsValid();
    if (bPCVRComputeCandidate) GlobalAccumulator->EnsurePCVRFinalColors(RHICmdList,GlobalAccumulator->AllocatedCount);
    const bool bPCVRComputeColor=bPCVRComputeCandidate && GlobalAccumulator->PCVRFinalColorSRV.IsValid();
    // Log the actual cache route (not just the editor checkbox).
    if (IsVRDiagnosticLogging_RenderThread() && GSPCVR::IsActive(View))
    {
        static TMap<int32,double> V16LastLog;
        const int32 EyeId=int32(View.StereoPass);
        double& Last=V16LastLog.FindOrAdd(EyeId);
        const double Now=FPlatformTime::Seconds();
        if (Now-Last>=2.0)
        {
            Last=Now;
            UE_LOG(LogTemp,Display,TEXT("[GSPCVR][V16FinalLighting] Eye=%d CacheRequested=%d CacheApplied=%d IndirectRequested=%d Lean=%d PreSort=%d RasterClip=%d Capacity=%u"),
                EyeId,CVarPCVRComputeFinalColor.GetValueOnRenderThread(),bPCVRComputeColor?1:0,
                CVarPCVRFinalColorIndirect.GetValueOnRenderThread(),bPCVRLeanOutputs?1:0,
                GlobalAccumulator->bPCVRPreSortCompacted?1:0,GSPCVR::RasterClip(View)?1:0,GlobalAccumulator->AllocatedCount);
        }
    }
	FGaussianSplatVS::FPermutationDomain VertexPermutation;
    VertexPermutation.Set<FGaussianSplatVS::FPCVRColorCache>(bPCVRComputeColor);
	VertexPermutation.Set<FGaussianSplatVS::FVRBaseline>(VRDiagnosticMode == 3);
    VertexPermutation.Set<FGaussianSplatVS::FVRRasterClip>((VRDiagnosticMode==3 || bPCVRLeanOutputs) && GSPCVR::RasterClip(View));
	TShaderMapRef<FGaussianSplatVS> VertexShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), VertexPermutation);
	FGaussianSplatPS::FPermutationDomain Permutation;
	Permutation.Set<FGaussianSplatPS::FVRDiagnosticMode>(VRDiagnosticMode);
    Permutation.Set<FGaussianSplatPS::FPCVRLeanOutputs>(bPCVRLeanOutputs);
    Permutation.Set<FGaussianSplatPS::FPCVRFinalOnly>(bPCVRLeanOutputs && CVarPCVRFinalOnly.GetValueOnRenderThread()!=0);
    Permutation.Set<FGaussianSplatPS::FPCVRColorCache>(bPCVRComputeColor);
    const bool bV15NoFogMRT=bPCVRLeanOutputs && VRDiagnosticMode==0 &&
        CVarGSPCVRTrimFogMRT.GetValueOnRenderThread()!=0 && !IsNativeFogCompositeEnabled_RenderThread();
    Permutation.Set<FGaussianSplatPS::FPCVRNoFogOutput>(bV15NoFogMRT);
    FGSRemovalBoxGPUData RemovalProbe[GSGSRemovalBoxMax];
    const uint32 RemovalCount=bPCVRComputeColor ? GetGSRemovalBoxes_RenderThread(View.Family?View.Family->Scene:nullptr,RemovalProbe) : MAX_uint32;
    const bool bNoRemoval=bPCVRComputeColor && RemovalCount==0 && CVarPCVRSpecializeRemoval.GetValueOnRenderThread()!=0;
    Permutation.Set<FGaussianSplatPS::FPCVRNoRemoval>(bNoRemoval);
    if (bPCVRComputeColor && IsVRDiagnosticLogging_RenderThread())
    {
        static TMap<int32,double> LastRemovalLog;
        double& Last=LastRemovalLog.FindOrAdd(int32(View.StereoPass));
        const double Now=FPlatformTime::Seconds();
        if (Now-Last>=1.0)
        {
            Last=Now;
            UE_LOG(LogTemp,Display,TEXT("[GSPCVR][RemovalPS] Boxes=%u Specialized=%d Eye=%d"),RemovalCount,bNoRemoval?1:0,int32(View.StereoPass));
        }
    }
	TShaderMapRef<FGaussianSplatPS> PixelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), Permutation);
	SCOPED_DRAW_EVENTF(RHICmdList, GSVRDiagnosticDraw, TEXT("GS_VRDiag_Draw_Mode%d"), VRDiagnosticMode);

	if (!VertexShader.IsValid() || !PixelShader.IsValid())
	{
		return;
	}

	// Transition global ViewData for graphics reads (SortKeysBuffer already in SRVGraphics)
	RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalViewDataBuffer, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));

	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
	// Read shared scene depth only; never write shared depth or stencil.
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<
		false, CF_DepthNearOrEqual
	>::GetRHI();
	// Premultiplied-alpha accumulation into the GS-only color target.
	GraphicsPSOInit.BlendState = TStaticBlendState<
		// RT0: normal premultiplied GS color accumulation.
		CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha,
		// RT1: existing nearest GS radial depth.
		CW_RED, BO_Min, BF_One, BF_One, BO_Min, BF_One, BF_One,
		// RT2 v35: RGB remains replacement, alpha accumulates eligible-water coverage.
		CW_RGBA, BO_Add, BF_One, BF_Zero, BO_Add, BF_One, BF_Zero,
		// RT3: snow-only coverage-aware occlusion depth.
		CW_RED, BO_Min, BF_One, BF_One, BO_Min, BF_One, BF_One,
		// RT4: fog-only premultiplied visible-depth accumulation.
		CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha
	>::GetRHI();
    if (bPCVRLeanOutputs)
    {
        if (bV15NoFogMRT)
            GraphicsPSOInit.BlendState=TStaticBlendState<
                CW_RGBA,BO_Add,BF_One,BF_InverseSourceAlpha,BO_Add,BF_One,BF_InverseSourceAlpha,
                CW_RED,BO_Min,BF_One,BF_One,BO_Min,BF_One,BF_One>::GetRHI();
        else
            GraphicsPSOInit.BlendState=TStaticBlendState<
                CW_RGBA,BO_Add,BF_One,BF_InverseSourceAlpha,BO_Add,BF_One,BF_InverseSourceAlpha,
                CW_RED,BO_Min,BF_One,BF_One,BO_Min,BF_One,BF_One,
                CW_RGBA,BO_Add,BF_One,BF_InverseSourceAlpha,BO_Add,BF_One,BF_InverseSourceAlpha>::GetRHI();
    }
    else if (VRDiagnosticMode == 3)
    {
        GraphicsPSOInit.BlendState = TStaticBlendState<
            CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();
    }
    else if (VRDiagnosticMode == 2)
    {
        GraphicsPSOInit.BlendState = TStaticBlendState<
            CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha,
            CW_RED, BO_Min, BF_One, BF_One, BO_Min, BF_One, BF_One>::GetRHI();
    }
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;
	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI  = PixelShader.GetPixelShader();

	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

	FIntRect ViewRect = View.UnscaledViewRect;
	RHICmdList.SetViewport(
		ViewRect.Min.X, ViewRect.Min.Y, 0.0f,
		ViewRect.Max.X, ViewRect.Max.Y, 1.0f
	);

	// GlobalDrawIndirectArgsBuffer is in IndirectArgs state from DispatchPrefixSumVisibleCounts
	// (or retained from previous frame when bCanSkip is true)
	FGaussianSplatVS::FParameters VSParameters{};
	VSParameters.PerSplatLightingEnabled = 0;
    VSParameters.PCVRReuseRadialDepth = CVarPCVRReuseRadialDepth.GetValueOnRenderThread()!=0 ? 1u : 0u;
	VSParameters.BaselineRenderQualityMode = 0;
    VSParameters.BaselineRasterClipParams = FVector4f(0,0,0,0);
	VSParameters.ViewDataBuffer  = GlobalAccumulator->GlobalViewDataBufferSRV;
	VSParameters.SortKeysBuffer  = GlobalAccumulator->GlobalSortKeysBufferSRV;
	VSParameters.SplatCount      = GlobalAccumulator->AllocatedCount;  // Upper bound for VS guard
	VSParameters.BaselineRenderQualityMode = static_cast<uint32>(FMath::Max(0, RenderQualityMode));
    VSParameters.BaselineRasterClipParams = FVector4f(float(ViewRect.Width()),float(ViewRect.Height()),GSPCVR::RasterClipMinArea(),0);
    VSParameters.BaselineRasterClipParams.W=(VRDiagnosticMode==0 && DebugMode==0 && LightingPreviewMode==0 &&
        !(GetStableShadowDebugMode()>=74 && GetStableShadowDebugMode()<=78) && GSPCVR::IsActive(View) && IsSourceQuadTrimEnabled_RenderThread())?1.0f:0.0f;
	VSParameters.DebugMode       = static_cast<uint32>(FMath::Max(0, DebugMode));
	VSParameters.EnableNanite    = 1;



	FGaussianSplatPS::FParameters PSParameters;
	PSParameters.PerSplatLightingEnabled = 0;
    PSParameters.PCVREarlyAlphaEnabled = CVarGSPCVREarlyAlpha.GetValueOnRenderThread()!=0 ? 1u : 0u;
    PSParameters.PCVRReuseRadialDepth = CVarPCVRReuseRadialDepth.GetValueOnRenderThread()!=0 ? 1u : 0u;
	SetVelocityPSParameters(PSParameters, View, GlobalAccumulator);
	PSParameters.LightingPreviewMode = static_cast<uint32>(FMath::Max(0, LightingPreviewMode));
	PSParameters.RenderQualityMode = static_cast<uint32>(FMath::Max(0, RenderQualityMode));
	PSParameters.UseProxyLightingMaskAsMain = bUseProxyLightingMaskAsMain ? 1u : 0u;
	PSParameters.ProxyLightingIntensityScale = FMath::Max(0.0f, ProxyLightingIntensityScale);
	PSParameters.DayNightBlend = FMath::Clamp(DayNightBlend, 0.0f, 1.0f);
	PSParameters.RegionalColorGradeParams0 = RegionalColorGradeParams0;
	PSParameters.RegionalColorGradeParams1 = RegionalColorGradeParams1;
	PSParameters.StableShadowColorRetention = FMath::Clamp(StableShadowColorRetention, 0.0f, 1.0f);
	SetProxyCapturePSParameters(PSParameters, View);
	VSParameters.PerSplatLightingEnabled = (VRDiagnosticMode==0 && DebugMode==0 && LightingPreviewMode==0 && RenderQualityMode!=4 &&
        !(GetStableShadowDebugMode()>=74 && GetStableShadowDebugMode()<=78) && GSPCVR::IsActive(View) && CVarPCVertexLighting.GetValueOnRenderThread()!=0) ? 1u : 0u;
	PSParameters.PerSplatLightingEnabled = VSParameters.PerSplatLightingEnabled;
	VSParameters.DayNightBlend = PSParameters.DayNightBlend;
	VSParameters.RegionalColorGradeParams0 = PSParameters.RegionalColorGradeParams0;
	VSParameters.RegionalColorGradeParams1 = PSParameters.RegionalColorGradeParams1;
	VSParameters.StableShadowColorRetention = PSParameters.StableShadowColorRetention;
	VSParameters.ProxyShadowDetailSettings = PSParameters.ProxyShadowDetailSettings;
	VSParameters.ProxyReflectedLightSettings = PSParameters.ProxyReflectedLightSettings;
	VSParameters.ProxyReflectedColorSettings = PSParameters.ProxyReflectedColorSettings;
	VSParameters.ProxyShadowBaseSettings = PSParameters.ProxyShadowBaseSettings;
	VSParameters.ProxyShadowBaseTint = PSParameters.ProxyShadowBaseTint;
	VSParameters.ProxyLightDepthForwardEnabled = PSParameters.ProxyLightDepthForwardEnabled;
    if (bPCVRComputeColor)
    {
        SCOPED_DRAW_EVENT(RHICmdList,GaussianSplatPCVRFinalColor);
        const bool bV16IndirectColor=CVarPCVRFinalColorIndirect.GetValueOnRenderThread()!=0 &&
            GlobalAccumulator->GlobalSortIndirectArgsGlobalBuffer.IsValid();
        FGaussianPCVRFinalColorCS::FPermutationDomain ColorPermutation;
        ColorPermutation.Set<FGaussianPCVRFinalColorCS::FVisibleIndirect>(bV16IndirectColor);
        TShaderMapRef<FGaussianPCVRFinalColorCS> ColorCS(GetGlobalShaderMap(GMaxRHIFeatureLevel),ColorPermutation);
        FGaussianPCVRFinalColorCS::FParameters P{};
        P.ViewDataBuffer=GlobalAccumulator->GlobalViewDataBufferSRV;
        P.SortKeysBuffer=GlobalAccumulator->GlobalSortKeysBufferSRV;
        P.SortParams=GlobalAccumulator->GlobalSortParamsBufferSRV;
        P.PCVRFinalColorsOut=GlobalAccumulator->PCVRFinalColorUAV;
        P.SourceCapacity=GlobalAccumulator->AllocatedCount;
        P.DayNightBlend=PSParameters.DayNightBlend;
        P.RegionalColorGradeParams0=PSParameters.RegionalColorGradeParams0;
        P.RegionalColorGradeParams1=PSParameters.RegionalColorGradeParams1;
        P.StableShadowColorRetention=PSParameters.StableShadowColorRetention;
        P.ProxyShadowDetailSettings=PSParameters.ProxyShadowDetailSettings;
        P.ProxyReflectedLightSettings=PSParameters.ProxyReflectedLightSettings;
        P.ProxyReflectedColorSettings=PSParameters.ProxyReflectedColorSettings;
        P.ProxyShadowBaseSettings=PSParameters.ProxyShadowBaseSettings;
        P.ProxyShadowBaseTint=PSParameters.ProxyShadowBaseTint;
        P.ProxyLightDepthForwardEnabled=PSParameters.ProxyLightDepthForwardEnabled;
        RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalViewDataBuffer,ERHIAccess::Unknown,ERHIAccess::SRVCompute));
        RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortKeysBuffer,ERHIAccess::Unknown,ERHIAccess::SRVCompute));
        RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->PCVRFinalColorBuffer,ERHIAccess::Unknown,ERHIAccess::UAVCompute));
        SetComputePipelineState(RHICmdList,ColorCS.GetComputeShader());
        SetShaderParameters(RHICmdList,ColorCS,ColorCS.GetComputeShader(),P);
        // V16-D: reuse the *current eye's* GPU compacted sort count. The indirect
        // buffer contains ceil(visible/1024) groups and was produced by the normal
        // compact/prefix path. V16 indirect HLSL processes 4 slots per lane.
        if (bV16IndirectColor)
            RHICmdList.DispatchIndirectComputeShader(GlobalAccumulator->GlobalSortIndirectArgsGlobalBuffer,0);
        else
            RHICmdList.DispatchComputeShader(FMath::DivideAndRoundUp(P.SourceCapacity,256u),1,1);
        UnsetShaderUAVs(RHICmdList,ColorCS,ColorCS.GetComputeShader());
        RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->PCVRFinalColorBuffer,ERHIAccess::UAVCompute,ERHIAccess::SRVGraphics));
        RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalViewDataBuffer,ERHIAccess::SRVCompute,ERHIAccess::SRVGraphics));
        RHICmdList.Transition(FRHITransitionInfo(GlobalAccumulator->GlobalSortKeysBuffer,ERHIAccess::SRVCompute,ERHIAccess::SRVGraphics));
        VSParameters.PCVRFinalColors=GlobalAccumulator->PCVRFinalColorSRV;
        VSParameters.PerSplatLightingEnabled=2u;
        PSParameters.PerSplatLightingEnabled=2u;
        SetGraphicsPipelineState(RHICmdList,GraphicsPSOInit,0);
    }
    FBufferRHIRef TileRasterArgs;
    if (bPCVRComputeColor && bNoRemoval && VRDiagnosticMode==0 && DebugMode==0 && LightingPreviewMode==0 &&
        GetStableShadowDebugMode()==0 && CVarPCVRShadowBlueOverlay.GetValueOnRenderThread()==0 &&
        IsPCVRClearSurface_RenderThread() && !IsDirectWetSurfaceEnabled_RenderThread() && !IsDirectSnowOverlayEnabled_RenderThread() &&
        DrawPCVRTileComposite(RHICmdList,View,GlobalAccumulator,TileSceneDepth,RenderQualityMode,VSParameters.BaselineRasterClipParams.W>0.5f,TileRasterArgs))
        SetGraphicsPipelineState(RHICmdList,GraphicsPSOInit,0);
	SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), VSParameters);
	SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PSParameters);

	RHICmdList.SetStreamSource(0, nullptr, 0);
    {
        SCOPED_DRAW_EVENTF(RHICmdList,GSPCVRRasterOnly,TEXT("GSPCVR_RasterOnly Eye=%d"),int32(View.StereoPass));
        const int32 EyeBucket = GSRasterEyeBucket(View);
        GSPreviewPassCounters::RecordRasterDraw(EyeBucket,GlobalAccumulator->AllocatedCount,
            uint64(ViewRect.Width())*uint64(ViewRect.Height()));
        const auto SubmitDraw = [&]()
        {
            RHICmdList.DrawIndexedPrimitiveIndirect(IndexBuffer,TileRasterArgs.IsValid()?TileRasterArgs:GlobalAccumulator->GlobalDrawIndirectArgsBuffer,0);
        };
        if (EyeBucket==0) { SCOPED_DRAW_EVENT(RHICmdList,GSPCVRRaster_LeftEye); SubmitDraw(); }
        else if (EyeBucket==1) { SCOPED_DRAW_EVENT(RHICmdList,GSPCVRRaster_RightEye); SubmitDraw(); }
        else { SCOPED_DRAW_EVENT(RHICmdList,GSPCVRRaster_OtherEye); SubmitDraw(); }
    }
	LogVRDiagnosticIndirectCount(RHICmdList,GlobalAccumulator,VRDiagnosticMode,GSRasterEyeBucket(View),GlobalAccumulator->AllocatedCount,true);
}


void FGaussianSplatRenderer::DrawSnowSplatsGlobalIndirect(
	FRHICommandListImmediate& RHICmdList,
	const FSceneView& View,
	FGaussianGlobalAccumulator* GlobalAccumulator,
	FBufferRHIRef IndexBuffer)
{
    GSPreviewPassCounters::FScopedCPUTime GSPreviewCPUTimeScope(GSPreviewPassCounters::SnowDrawIndirect);
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSnowOverlayGlobalIndirect);

	if (!GlobalAccumulator || !IndexBuffer.IsValid() ||
		!GlobalAccumulator->GlobalDrawIndirectArgsBuffer.IsValid() ||
		!IsDirectSnowOverlayEnabled_RenderThread())
	{
		return;
	}

	TShaderMapRef<FGaussianSnowOverlayVS> VertexShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FGaussianSnowOverlayPS> PixelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	if (!VertexShader.IsValid() || !PixelShader.IsValid())
	{
		return;
	}

	RHICmdList.Transition(FRHITransitionInfo(
		GlobalAccumulator->GlobalViewDataBuffer,
		ERHIAccess::Unknown,
		ERHIAccess::SRVGraphics));

	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<
		false, CF_DepthNearOrEqual
	>::GetRHI();
	GraphicsPSOInit.BlendState = TStaticBlendState<
		CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha,
		CW_RED, BO_Min, BF_One, BF_One, BO_Min, BF_One, BF_One
	>::GetRHI();
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;
	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

	const FIntRect ViewRect = View.UnscaledViewRect;
	RHICmdList.SetViewport(
		ViewRect.Min.X, ViewRect.Min.Y, 0.0f,
		ViewRect.Max.X, ViewRect.Max.Y, 1.0f);

	FGaussianSnowOverlayVS::FParameters VSParameters;
	VSParameters.ViewDataBuffer = GlobalAccumulator->GlobalViewDataBufferSRV;
	VSParameters.SortKeysBuffer = GlobalAccumulator->GlobalSortKeysBufferSRV;
	VSParameters.SplatCount = GlobalAccumulator->AllocatedCount;
	VSParameters.SnowTranslatedWorldToClip = FMatrix44f(
		View.ViewMatrices.GetTranslatedViewMatrix() *
		View.ViewMatrices.GetProjectionNoAAMatrix());
	VSParameters.SnowOverlayParams = FVector4f(
		FMath::Clamp(GDirectSplatSurfaceEffectSettings2.X, 0.0f, 50.0f),
		FMath::Clamp(GDirectSplatSurfaceEffectSettings2.Y, 1.0f, 1.20f),
		0.018f,
		1.35f);
	VSParameters.SnowOverlayColor = FVector4f(
		FMath::Max(GDirectSplatSurfaceEffectSettings1.X, 0.0f),
		FMath::Max(GDirectSplatSurfaceEffectSettings1.Y, 0.0f),
		FMath::Max(GDirectSplatSurfaceEffectSettings1.Z, 0.0f),
		FMath::Clamp(GDirectSplatSurfaceEffectSettings1.W, 0.0f, 2.0f));
	VSParameters.SnowTuningParams = GDirectSnowTuningSettings;
	SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), VSParameters);

	FGaussianSnowOverlayPS::FParameters RemovalPSParameters;
	SetGSRemovalBoxesPS(RemovalPSParameters, View);
	RemovalPSParameters.PreViewTranslation = FVector3f(View.ViewMatrices.GetPreViewTranslation());
	RemovalPSParameters.PuddleClipToWorld = FMatrix44f(View.ViewMatrices.GetInvViewProjectionMatrix());
	RemovalPSParameters.PuddleViewport = FVector4f(float(ViewRect.Min.X),float(ViewRect.Min.Y),
		float(FMath::Max(1,ViewRect.Width())),float(FMath::Max(1,ViewRect.Height())));
	SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), RemovalPSParameters);

	RHICmdList.SetStreamSource(0, nullptr, 0);
	RHICmdList.DrawIndexedPrimitiveIndirect(
		IndexBuffer,
		GlobalAccumulator->GlobalDrawIndirectArgsBuffer,
		0);
}

void FGaussianSplatRenderer::ExtractFrustumPlanes(const FMatrix& ViewProjection, FVector4f OutPlanes[6])
{
	// Extract frustum planes from view-projection matrix
	// Using the Gribb/Hartmann method
	// Each row of VP matrix gives us coefficients for a plane equation
	// Plane equation: dot(Normal, P) + D = 0

	const FMatrix& M = ViewProjection;

	// Left plane: Row3 + Row0
	OutPlanes[0] = FVector4f(
		M.M[0][3] + M.M[0][0],
		M.M[1][3] + M.M[1][0],
		M.M[2][3] + M.M[2][0],
		M.M[3][3] + M.M[3][0]
	);

	// Right plane: Row3 - Row0
	OutPlanes[1] = FVector4f(
		M.M[0][3] - M.M[0][0],
		M.M[1][3] - M.M[1][0],
		M.M[2][3] - M.M[2][0],
		M.M[3][3] - M.M[3][0]
	);

	// Bottom plane: Row3 + Row1
	OutPlanes[2] = FVector4f(
		M.M[0][3] + M.M[0][1],
		M.M[1][3] + M.M[1][1],
		M.M[2][3] + M.M[2][1],
		M.M[3][3] + M.M[3][1]
	);

	// Top plane: Row3 - Row1
	OutPlanes[3] = FVector4f(
		M.M[0][3] - M.M[0][1],
		M.M[1][3] - M.M[1][1],
		M.M[2][3] - M.M[2][1],
		M.M[3][3] - M.M[3][1]
	);

	// Near plane: Row3 + Row2 (for reversed-Z: Row2)
	OutPlanes[4] = FVector4f(
		M.M[0][2],
		M.M[1][2],
		M.M[2][2],
		M.M[3][2]
	);

	// Far plane: Row3 - Row2
	OutPlanes[5] = FVector4f(
		M.M[0][3] - M.M[0][2],
		M.M[1][3] - M.M[1][2],
		M.M[2][3] - M.M[2][2],
		M.M[3][3] - M.M[3][2]
	);

	// Normalize planes (important for distance calculations)
	for (int32 i = 0; i < 6; i++)
	{
		float Length = FMath::Sqrt(
			OutPlanes[i].X * OutPlanes[i].X +
			OutPlanes[i].Y * OutPlanes[i].Y +
			OutPlanes[i].Z * OutPlanes[i].Z
		);
		if (Length > SMALL_NUMBER)
		{
			OutPlanes[i] = OutPlanes[i] / Length;
		}
	}
}

int32 FGaussianSplatRenderer::DispatchClusterCulling(
	FRHICommandListImmediate& RHICmdList,
	const FSceneView& View,
	FGaussianSplatGPUResources* GPUResources,
	const FMatrix& LocalToWorld,
	float ErrorThreshold,
	bool bUseLODRendering)
{
    GSPreviewPassCounters::FScopedCPUTime GSPreviewCPUTimeScope(GSPreviewPassCounters::ClusterCull);
    if (GPUResources) GSPreviewPassCounters::AddWork(GSPreviewPassCounters::ClusterCull, GPUResources->ClusterCount > 0 ? uint64(GPUResources->ClusterCount) : 0);
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatClusterCulling);

	if (!GPUResources->bHasClusterData || GPUResources->ClusterCount <= 0)
	{
		return 0;
	}

	TShaderMapRef<FClusterCullingResetCS> ResetShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FClusterCullingCS> CullingShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	if (!ResetShader.IsValid() || !CullingShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Cluster culling shaders not valid"));
		return 0;
	}

    const FSceneView *StereoLeft=nullptr,*StereoRight=nullptr;
    const bool bUnion=GSPCVR::StereoPair(View,StereoLeft,StereoRight);
    const int32 PCVRDebug=CVarDebugForceLODLevel.GetValueOnRenderThread();
    const void* BufferIdentity=GPUResources->ClusterBufferSRV.GetReference();
    if(bUnion && GPUResources->bPCVRSharedCull && GPUResources->PCVRCullFrame==GFrameNumberRenderThread &&
       GPUResources->PCVRCullFamily==View.Family && GPUResources->PCVRCullBufferIdentity==BufferIdentity &&
       GPUResources->PCVRCullLocal.Equals(LocalToWorld,0) && GPUResources->PCVRCullError==ErrorThreshold &&
       GPUResources->bPCVRCullLOD==bUseLODRendering && GPUResources->PCVRCullDebug==PCVRDebug &&
       GPUResources->PCVRCullLeft.Equals(StereoLeft->ViewMatrices.GetViewProjectionMatrix(),0) &&
       GPUResources->PCVRCullRight.Equals(StereoRight->ViewMatrices.GetViewProjectionMatrix(),0))
    {
        GSPreviewPassCounters::MarkSkip(GSPreviewPassCounters::ClusterCull);
        SCOPED_DRAW_EVENT(RHICmdList,GSPCVRStereoCullReuse);
        return GPUResources->LeafClusterCount;
    }
    GPUResources->bPCVRSharedCull=bUnion;
    GPUResources->bPCVRCompactedShared=false;
	// Calculate visibility bitmap size
	uint32 VisibilityBitmapSize = FMath::DivideAndRoundUp(GPUResources->ClusterCount, 32);
	VisibilityBitmapSize = FMath::Max(VisibilityBitmapSize, 1u);

	// Transition buffers for compute
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->VisibleClusterBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->VisibleClusterCountBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->VisibleSplatCountBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->ClusterVisibilityBitmap, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SelectedClusterBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	if (GPUResources->bSupportsIndirectDraw)
	{
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->IndirectDrawArgsBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	}

	// Transition LOD cluster tracking buffers (always needed for reset)
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->LODClusterBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->LODClusterCountBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->LODClusterSelectedBitmap, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->LODSplatTotalBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->LODSplatOutputCountBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	// Step 1: Reset the visible cluster counter, indirect draw args, visibility bitmap, selected cluster buffer, and LOD tracking buffers
	{
		FClusterCullingResetCS::FParameters ResetParams;
		ResetParams.VisibleClusterCountBuffer = GPUResources->VisibleClusterCountBufferUAV;
		ResetParams.VisibleSplatCount = GPUResources->VisibleSplatCountBufferUAV;
		ResetParams.IndirectDrawArgsBuffer = GPUResources->IndirectDrawArgsBufferUAV;
		ResetParams.ClusterVisibilityBitmap = GPUResources->ClusterVisibilityBitmapUAV;
		ResetParams.SelectedClusterBuffer = GPUResources->SelectedClusterBufferUAV;
		ResetParams.LODClusterBuffer = GPUResources->LODClusterBufferUAV;
		ResetParams.LODClusterCountBuffer = GPUResources->LODClusterCountBufferUAV;
		ResetParams.LODClusterSelectedBitmap = GPUResources->LODClusterSelectedBitmapUAV;
		ResetParams.LODSplatTotalBuffer = GPUResources->LODSplatTotalBufferUAV;
		ResetParams.LODSplatOutputCountBuffer = GPUResources->LODSplatOutputCountBufferUAV;
		ResetParams.ClusterVisibilityBitmapSize = VisibilityBitmapSize;
		ResetParams.LeafClusterCount = GPUResources->LeafClusterCount;
		ResetParams.DebugForceLODLevel = CVarDebugForceLODLevel.GetValueOnRenderThread();  // Must bind (shared .usf)

		// Dispatch enough threads to clear the bitmap and initialize selected cluster buffer
		uint32 NumGroups = FMath::DivideAndRoundUp(FMath::Max(VisibilityBitmapSize, (uint32)GPUResources->LeafClusterCount), 64u);

		SetComputePipelineState(RHICmdList, ResetShader.GetComputeShader());
		SetShaderParameters(RHICmdList, ResetShader, ResetShader.GetComputeShader(), ResetParams);
		RHICmdList.DispatchComputeShader(NumGroups, 1, 1);
		UnsetShaderUAVs(RHICmdList, ResetShader, ResetShader.GetComputeShader());
	}

	// UAV barrier
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->VisibleClusterCountBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->VisibleSplatCountBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->ClusterVisibilityBitmap, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SelectedClusterBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->LODClusterCountBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->LODClusterSelectedBitmap, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->LODSplatTotalBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

	// Step 2: Run cluster culling with LOD selection
	{
		// Extract frustum planes from world-space ViewProjection matrix
		// The shader transforms cluster bounds to world space, so frustum planes must also be in world space
		FVector4f FrustumPlanes[6];
		ExtractFrustumPlanes((bUnion ? StereoLeft : &View)->ViewMatrices.GetViewProjectionMatrix(),FrustumPlanes);
        FVector4f OtherPlanes[6];
        ExtractFrustumPlanes((bUnion ? StereoRight : &View)->ViewMatrices.GetViewProjectionMatrix(),OtherPlanes);

		FClusterCullingCS::FParameters CullingParams;
		CullingParams.ClusterBuffer = GPUResources->ClusterBufferSRV;
		CullingParams.VisibleClusterBuffer = GPUResources->VisibleClusterBufferUAV;
		CullingParams.VisibleClusterCountBuffer = GPUResources->VisibleClusterCountBufferUAV;
		CullingParams.IndirectDrawArgsBuffer = GPUResources->IndirectDrawArgsBufferUAV;
		CullingParams.ClusterVisibilityBitmap = GPUResources->ClusterVisibilityBitmapUAV;
		CullingParams.SelectedClusterBuffer = GPUResources->SelectedClusterBufferUAV;
		CullingParams.LODClusterBuffer = GPUResources->LODClusterBufferUAV;
		CullingParams.LODClusterCountBuffer = GPUResources->LODClusterCountBufferUAV;
		CullingParams.LODClusterSelectedBitmap = GPUResources->LODClusterSelectedBitmapUAV;
		CullingParams.LODSplatTotalBuffer = GPUResources->LODSplatTotalBufferUAV;
		CullingParams.LocalToWorld = FMatrix44f(LocalToWorld);
		CullingParams.WorldToClip = FMatrix44f(View.ViewMatrices.GetViewProjectionMatrix());
		CullingParams.ClusterCount = GPUResources->ClusterCount;
		CullingParams.LeafClusterCount = GPUResources->LeafClusterCount;

		for (int32 i = 0; i < 6; i++)
		{
			CullingParams.FrustumPlanes[i] = FrustumPlanes[i];
            CullingParams.StereoFrustumPlanes[i]=OtherPlanes[i];
		}

		// LOD selection parameters
		CullingParams.UseStereoUnion=bUnion?(FParse::Param(FCommandLine::Get(),TEXT("GSPCVRLODTest"))?2u:1u):0u;
        if(CullingParams.UseStereoUnion==2 && (GFrameNumberRenderThread%120)==0)UE_LOG(LogTemp,Display,TEXT("[GSPCVRLOD][Cull] SharedUnion=1 MaxLOD=1 UseLOD=%d ErrorThreshold=%.6f Clusters=%d Leaves=%d OriginalCapacity=%d"),bUseLODRendering?1:0,ErrorThreshold,GPUResources->ClusterCount,GPUResources->LeafClusterCount,GPUResources->GetSplatCount()-GPUResources->LODSplatCount);
        CullingParams.CameraPosition=FVector3f((bUnion?StereoLeft:&View)->ViewMatrices.GetViewOrigin());
        CullingParams.StereoCameraPosition=FVector3f((bUnion?StereoRight:&View)->ViewMatrices.GetViewOrigin());
		// Use projection matrix scaling factor for FOV-based LOD (resolution-independent, like Nanite)
		// ProjMatrix[1][1] = 1/tan(HalfFOV_Y), depends only on FOV, not viewport pixel size
		const FMatrix& ProjMatrix = View.ViewMatrices.GetProjectionMatrix();
		CullingParams.ScreenHeight = FMath::Max(ProjMatrix.M[0][0], ProjMatrix.M[1][1]);
        if(bUnion)
        {
            const FMatrix &LP=StereoLeft->ViewMatrices.GetProjectionMatrix(),&RP=StereoRight->ViewMatrices.GetProjectionMatrix();
            CullingParams.ScreenHeight=FMath::Max(FMath::Max(LP.M[0][0],LP.M[1][1]),FMath::Max(RP.M[0][0],RP.M[1][1]));
        }
		CullingParams.ErrorThreshold = FMath::Max(0.001f, ErrorThreshold);
		CullingParams.LODBias = 0.0f;         // No bias (can be made configurable)
		// v2.8.74: these matrix rows are identical for every GPU leaf thread.
		// Cache the exact row-norm based bounding-sphere scale in the per-view uniforms.
		const auto AxisLengthSquared = [&LocalToWorld](int32 Row) -> double
		{
			return FMath::Square(LocalToWorld.M[Row][0]) +
				FMath::Square(LocalToWorld.M[Row][1]) +
				FMath::Square(LocalToWorld.M[Row][2]);
		};
		CullingParams.ClusterWorldMaxScale = float(FMath::Sqrt(FMath::Max3(
			AxisLengthSquared(0), AxisLengthSquared(1), AxisLengthSquared(2))));
		CullingParams.UseLODRendering = bUseLODRendering ? 1 : 0;
		// Debug: Force specific LOD level (-1 = auto, 0 = leaf, 1+ = specific level)
		CullingParams.DebugForceLODLevel = CVarDebugForceLODLevel.GetValueOnRenderThread();

		const uint32 ThreadGroupSize = 64;
		const uint32 NumGroups = FMath::DivideAndRoundUp((uint32)GPUResources->LeafClusterCount, ThreadGroupSize);

		SetComputePipelineState(RHICmdList, CullingShader.GetComputeShader());
		SetShaderParameters(RHICmdList, CullingShader, CullingShader.GetComputeShader(), CullingParams);
		RHICmdList.DispatchComputeShader(NumGroups, 1, 1);
		UnsetShaderUAVs(RHICmdList, CullingShader, CullingShader.GetComputeShader());
	}

	// Transition for potential readback (for debugging/statistics)
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->VisibleClusterCountBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->VisibleClusterBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));

	// Transition visibility bitmap and selected cluster buffer for CalcViewData to read
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->ClusterVisibilityBitmap, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SelectedClusterBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));

	// Transition LOD cluster buffers for LOD rendering pass
	if (bUseLODRendering)
	{
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->LODClusterBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->LODClusterCountBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->LODSplatTotalBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
		// Transition LODClusterSelectedBitmap to SRV for GPU-driven LOD shader to read
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->LODClusterSelectedBitmap, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
	}

	// Transition indirect draw args buffer for draw call
	if (GPUResources->bSupportsIndirectDraw)
	{
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->IndirectDrawArgsBuffer, ERHIAccess::UAVCompute, ERHIAccess::IndirectArgs));
	}

    if(bUnion)
    {
        GPUResources->PCVRCullFrame=GFrameNumberRenderThread;GPUResources->PCVRCullFamily=View.Family;
        GPUResources->PCVRCullLocal=LocalToWorld;GPUResources->PCVRCullError=ErrorThreshold;
        GPUResources->bPCVRCullLOD=bUseLODRendering;GPUResources->PCVRCullDebug=PCVRDebug;
        GPUResources->PCVRCullBufferIdentity=BufferIdentity;
        GPUResources->PCVRCullLeft=StereoLeft->ViewMatrices.GetViewProjectionMatrix();
        GPUResources->PCVRCullRight=StereoRight->ViewMatrices.GetViewProjectionMatrix();
    }
	// Return leaf cluster count as placeholder (actual count would require GPU readback)
	// In a real implementation, you'd use the VisibleClusterBuffer in subsequent passes
	return GPUResources->LeafClusterCount;
}

void FGaussianSplatRenderer::GradeToTexture(
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
	const FVector4f& CompositeLookParams4)
{
	GSPreviewPassCounters::FScopedCPUTime GSPreviewCPUTimeScope(GSPreviewPassCounters::PostGradeHDR);
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatPostGradeHDR);

	TShaderMapRef<FGaussianSplatCompositeVS> VertexShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FGaussianSplatCompositePS> PixelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	if (!VertexShader.IsValid() || !PixelShader.IsValid())
	{
		return;
	}

	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
	// No depth testing for full-screen composite
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
	// v2888: the full-extent grade shader writes transparent zeros for uncovered
	// pixels, allowing an ENoAction load; cropped views still clear the target.
	GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;
	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

	RHICmdList.SetViewport(
		OutputViewRect.Min.X, OutputViewRect.Min.Y, 0.0f,
		OutputViewRect.Max.X, OutputViewRect.Max.Y, 1.0f
	);

	// Set pixel shader parameters
    FGaussianSplatCompositePS::FParameters PSParameters;
	PSParameters.PanoramaShadowMaskProbe =
		(GStableShadowDebugMode >= 102 && GStableShadowDebugMode <= 104)
			? static_cast<uint32>(FMath::Clamp(CVarGSPanoramaShadowMaskProbe.GetValueOnRenderThread(), 0, 2))
			: 0u;
    PSParameters.IntermediateTexture = IntermediateTexture;
    PSParameters.IntermediateDepthTexture = IntermediateDepthTexture;
	PSParameters.SnowIntermediateTexture =
		SnowIntermediateTexture.IsValid() ? SnowIntermediateTexture : IntermediateTexture;
	PSParameters.SnowIntermediateDepthTexture =
		SnowIntermediateDepthTexture.IsValid() ? SnowIntermediateDepthTexture : IntermediateDepthTexture;
	PSParameters.SnowOcclusionDepthTexture =
		SnowOcclusionDepthTexture.IsValid() ? SnowOcclusionDepthTexture : IntermediateDepthTexture;
	PSParameters.SnowCompositeParams = FVector4f(
		bSnowOverlayEnabled && SnowIntermediateTexture.IsValid() && SnowIntermediateDepthTexture.IsValid() ? 1.0f : 0.0f,
		FMath::Max(4.0f, GDirectSplatSurfaceEffectSettings2.X * 1.5f + 4.0f),
		static_cast<float>(HALF_WORLD_MAX),
		0.0f);
	PSParameters.SnowTuningParams = GDirectSnowTuningSettings;
	PSParameters.ProxyCompositeLightDepthTexture =
		GProxyLightDepthCaptureTexture.IsValid()
			? GProxyLightDepthCaptureTexture
			: GWhiteTexture->TextureRHI;
	PSParameters.ProxyCompositeLightDepthSampler =
		TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PSParameters.GaussianCompositeParams0 = FVector4f(
		1.0f,
		CompositeLookParams0.X,
		CompositeLookParams0.Y,
		CompositeLookParams0.Z);
	PSParameters.GaussianCompositeParams1 = CompositeLookParams1;
	PSParameters.GaussianCompositeParams2 = CompositeLookParams2;
	PSParameters.GaussianCompositeParams3 = CompositeLookParams3;
	PSParameters.GaussianCompositeParams4 = CompositeLookParams4;
	PSParameters.GaussianCompositeSourceBrightness = FMath::Max(CompositeLookParams0.W, 0.0f);
	PSParameters.GaussianCompositeSourceRect = FVector4f(
		SourceViewRect.Min.X, SourceViewRect.Min.Y,
		SourceViewRect.Width(), SourceViewRect.Height());
	PSParameters.GaussianCompositeOutputRect = FVector4f(
		OutputViewRect.Min.X, OutputViewRect.Min.Y,
		OutputViewRect.Width(), OutputViewRect.Height());
	PSParameters.ProxyCompositeInvViewProjection =
		FMatrix44f(View.ViewMatrices.GetInvViewProjectionMatrix());
	PSParameters.ProxyCompositeCameraOriginValid = FVector4f(
		FVector3f(View.ViewLocation),
		GProxyLightDepthCaptureTexture.IsValid() && GProxyLightDepthForwardEnabled.W >= 0.0f
			? 1.0f
			: 0.0f);
	PSParameters.ProxyCompositeLightOriginWidth = GProxyLightDepthOriginWidth;
	PSParameters.ProxyCompositeLightForwardSoftness = GProxyLightDepthForwardEnabled;
	PSParameters.ProxyCompositeLightRightRange = GProxyLightDepthRightRange;
	PSParameters.ProxyCompositeLightUp = GProxyLightDepthUpSettings;
	PSParameters.ProxyCompositeShaftSettings = GProxyVolumetricShaftSettings;
	PSParameters.ProxyCompositeShaftTint = GProxyVolumetricShaftTint;
	PSParameters.NativeFogCompositeSettings = GNativeFogCompositeSettings;
	PSParameters.NativeFogCompositeColor = GNativeFogCompositeColor;
	PSParameters.NativeFogCompositeProfile0 = GNativeFogCompositeProfile0;
	PSParameters.NativeFogCompositeProfile1 = GNativeFogCompositeProfile1;
    SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PSParameters);

	// Draw full-screen triangle (3 vertices, no index buffer)
	RHICmdList.SetStreamSource(0, nullptr, 0);
	RHICmdList.DrawPrimitive(0, 1, 1);
}

void FGaussianSplatRenderer::CompositeToSceneColor(
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
    FRHITexture* BloomExtractTexture)
{
	GSPreviewPassCounters::FScopedCPUTime GSPreviewCPUTimeScope(GSPreviewPassCounters::BloomComposite);
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatPostGradeBloomComposite);

	TShaderMapRef<FGaussianSplatCompositeVS> VertexShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FGaussianSplatBloomCompositePS::FPermutationDomain BloomPermutation;
    BloomPermutation.Set<FGaussianSplatBloomCompositePS::FExtractCache>(BloomExtractTexture!=nullptr);
    TShaderMapRef<FGaussianSplatBloomCompositePS> PixelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel),BloomPermutation);
	if (!VertexShader.IsValid() || !PixelShader.IsValid())
	{
		return;
	}

	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
	// Premultiplied-alpha blend the graded GS base plus its additive bloom onto
	// SceneColor while preserving SceneColor alpha for Movie Render Queue export.
	GraphicsPSOInit.BlendState = TStaticBlendState<
		CW_RGB, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha
	>::GetRHI();
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;
	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

	RHICmdList.SetViewport(
		OutputViewRect.Min.X, OutputViewRect.Min.Y, 0.0f,
		OutputViewRect.Max.X, OutputViewRect.Max.Y, 1.0f);

	FGaussianSplatBloomCompositePS::FParameters PSParameters;FMemory::Memzero(PSParameters);
    PSParameters.PCVRBloomExtractTexture=BloomExtractTexture?BloomExtractTexture:GBlackTexture->TextureRHI.GetReference();
	PSParameters.IntermediateTexture = GradedTexture;
    PSParameters.PCVRFogCoverageTexture = RawFogCoverageTexture;
	PSParameters.IntermediateDepthTexture = IntermediateDepthTexture;
	PSParameters.IntermediateFogDepthTexture =
		IntermediateFogDepthTexture.IsValid() ? IntermediateFogDepthTexture : IntermediateDepthTexture;
	PSParameters.ProxyCompositeLightDepthTexture =
		GProxyLightDepthCaptureTexture.IsValid()
			? GProxyLightDepthCaptureTexture
			: GWhiteTexture->TextureRHI;
	PSParameters.ProxyCompositeLightDepthSampler =
		TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PSParameters.StableShadowDebugMode = uint32(FMath::Max(GStableShadowDebugMode, 0));
	PSParameters.PanoramaShadowMaskProbe =
		(GStableShadowDebugMode >= 102 && GStableShadowDebugMode <= 104)
			? static_cast<uint32>(FMath::Clamp(CVarGSPanoramaShadowMaskProbe.GetValueOnRenderThread(), 0, 2))
			: 0u;
	PSParameters.PanoramaGSCompositeMode = FMath::Min(PanoramaGSCompositeMode, 2u);
	PSParameters.StableShadowDebugLightDepthValid =
		(GProxyLightDepthCaptureTexture.IsValid() && GProxyLightDepthForwardEnabled.W >= 0.0f) ? 1u : 0u;
	// Composite into the current view's pre-exposed SceneColor.  ViewState stores
	// exposure history and is not authoritative for MRQ panoramic panes (each pane
	// can have a different/current PreExposure while sharing or recreating state).
	// Using the stale history value here over-brightens GS and visually erases its
	// stable self-shadow in the stitched panorama.
	// Current-frame exposure belongs to FViewInfo in UE 5.8, not FSceneView.
	const float ViewPreExposure = View.bIsViewInfo
		? FMath::Max(static_cast<const FViewInfo&>(View).PreExposure, 1.0e-4f) : 1.0f;
	// Mode 1: test the actual Final (not just mask debug) without the extra
	// GS PreExposure. Mode 2: own HDR->display mapping after UE Tonemap.
	const float AppliedGSPreExposure = PanoramaGSCompositeMode == 0u ? ViewPreExposure : 1.0f;
	PSParameters.GaussianCompositeParams0 = FVector4f(
		AppliedGSPreExposure, FMath::Clamp(PanoramaGSToneMapEV, -4.0f, 4.0f),
        IntermediateFogDepthTexture.IsValid() ? (IntermediateFogDepthTexture->GetDesc().Format==PF_R16F ? 2.0f : (IntermediateFogDepthTexture->GetDesc().Format==PF_G16R16F ? 1.0f : 0.0f)) : 0.0f, 0.0f);
	PSParameters.GaussianCompositeBloomParams = CompositeBloomParams;
	PSParameters.GaussianCompositeColorBloomParams = CompositeColorBloomParams;
	PSParameters.GaussianCompositeBloomTint = CompositeBloomTint;
	PSParameters.GaussianCompositeBloomSoftKnee = FMath::Clamp(CompositeBloomSoftKnee, 0.0f, 1.0f);
	PSParameters.GaussianCompositeSourceRect = FVector4f(
		SourceViewRect.Min.X, SourceViewRect.Min.Y,
		SourceViewRect.Width(), SourceViewRect.Height());
	PSParameters.GaussianCompositeOutputRect = FVector4f(
		OutputViewRect.Min.X, OutputViewRect.Min.Y,
		OutputViewRect.Width(), OutputViewRect.Height());
	// Fog-only bindings for the final bloom/composite entry point.  Reuse the
	// existing proxy LightDepth and native-fog state without altering their owners.
	PSParameters.ProxyCompositeInvViewProjection =
		FMatrix44f(View.ViewMatrices.GetInvViewProjectionMatrix());
	PSParameters.ProxyCompositeCameraOriginValid = FVector4f(
		FVector3f(View.ViewLocation),
		GProxyLightDepthCaptureTexture.IsValid() && GProxyLightDepthForwardEnabled.W >= 0.0f
			? 1.0f
			: 0.0f);
	PSParameters.ProxyCompositeLightOriginWidth = GProxyLightDepthOriginWidth;
	PSParameters.ProxyCompositeLightForwardSoftness = GProxyLightDepthForwardEnabled;
	PSParameters.ProxyCompositeLightRightRange = GProxyLightDepthRightRange;
	PSParameters.ProxyCompositeLightUp = GProxyLightDepthUpSettings;
	PSParameters.ProxyCompositeShaftTint = GProxyVolumetricShaftTint;
	PSParameters.NativeFogCompositeSettings = GNativeFogCompositeSettings;
	// P50: mode 1 isolates GS surface exposure, not participating-media light.
	// Native height fog is pre-exposed by ViewPreExposure. Scale only the GS
	// scattering input so the final GS multiplication produces the same fog RGB.
	const float FogScatteringExposure = PanoramaGSCompositeMode == 2u
		? 1.0f : ViewPreExposure / FMath::Max(AppliedGSPreExposure, 1.0e-4f);
	PSParameters.NativeFogCompositeColor = FVector4f(
		GNativeFogCompositeColor.X * FogScatteringExposure,
		GNativeFogCompositeColor.Y * FogScatteringExposure,
		GNativeFogCompositeColor.Z * FogScatteringExposure, 1.0f);
	PSParameters.NativeFogCompositeProfile0 = GNativeFogCompositeProfile0;
	PSParameters.NativeFogCompositeProfile1 = GNativeFogCompositeProfile1;
	SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PSParameters);

	RHICmdList.SetStreamSource(0, nullptr, 0);
	RHICmdList.DrawPrimitive(0, 1, 1);
}

void FGaussianSplatRenderer::ApplyWetSurfaceToTexture(
	FRHICommandListImmediate& RHICmdList,
	const FSceneView& View,
	FTextureRHIRef SourceTexture,
	FTextureRHIRef DepthTexture,
	FTextureRHIRef NormalTexture,
	FTextureRHIRef SceneColorTexture,
	const FIntRect& SourceViewRect,
	const FIntRect& SceneColorViewRect,
	const FIntRect& OutputViewRect,
	float SceneColorInvPreExposure)
{
	GSPreviewPassCounters::FScopedCPUTime GSPreviewCPUTimeScope(GSPreviewPassCounters::WetSurface);
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatDedicatedWetSurface);
	TShaderMapRef<FGaussianSplatCompositeVS> VertexShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FGaussianSplatWetSurfacePS> PixelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	if (!VertexShader.IsValid() || !PixelShader.IsValid() || !SourceTexture.IsValid() ||
		!DepthTexture.IsValid() || !NormalTexture.IsValid()) return;

	FGraphicsPipelineStateInitializer PSO;
	RHICmdList.ApplyCachedRenderTargets(PSO);
	PSO.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
	PSO.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
	PSO.BlendState = TStaticBlendState<>::GetRHI();
	PSO.PrimitiveType = PT_TriangleList;
	PSO.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
	PSO.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
	PSO.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
	SetGraphicsPipelineState(RHICmdList, PSO, 0);
	RHICmdList.SetViewport(OutputViewRect.Min.X, OutputViewRect.Min.Y, 0.0f,
		OutputViewRect.Max.X, OutputViewRect.Max.Y, 1.0f);

	FGaussianSplatWetSurfacePS::FParameters P;
	P.WetSourceTexture = SourceTexture;
	P.WetDepthTexture = DepthTexture;
	P.WetNormalTexture = NormalTexture;
	// v36: use the existing proxy/collider surface-depth capture only as receiver
	// geometry. If it is unavailable the shader falls back to the legacy stable
	// normal/size field, so scenes without a baked collider are not hard-disabled.
	const bool bHasWetProxySurface = GProxySurfaceDepthTexture.IsValid() && GProxySurfaceRightEnabled.W > 0.5f;
	P.WetProxySurfaceDepthTexture = bHasWetProxySurface
		? GProxySurfaceDepthTexture
		: GWhiteTexture->TextureRHI;
	P.WetProxySurfaceDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	P.WetProxySurfaceOriginTanFOV = GProxySurfaceOriginTanFOV;
	P.WetProxySurfaceForwardAspect = GProxySurfaceForwardAspect;
	P.WetProxySurfaceRightEnabled = bHasWetProxySurface
		? GProxySurfaceRightEnabled
		: FVector4f(GProxySurfaceRightEnabled.X, GProxySurfaceRightEnabled.Y, GProxySurfaceRightEnabled.Z, 0.0f);
	P.WetProxySurfaceUp = GProxySurfaceUp;
	// W was unused by this shader. Keep the existing shader parameter layout
	// compatible with old binaries; old producers publish W=0 (normal mode).
	P.WetProxySurfaceUp.W = static_cast<float>(GWetReceiverTestMode);
	P.WetSceneColorTexture = SceneColorTexture;
	P.WetSourceRect = FVector4f(SourceViewRect.Min.X, SourceViewRect.Min.Y, SourceViewRect.Width(), SourceViewRect.Height());
	P.WetSceneColorRect = FVector4f(SceneColorViewRect.Min.X, SceneColorViewRect.Min.Y, SceneColorViewRect.Width(), SceneColorViewRect.Height());
	P.WetOutputRect = FVector4f(OutputViewRect.Min.X, OutputViewRect.Min.Y, OutputViewRect.Width(), OutputViewRect.Height());
	P.WetSceneColorInvPreExposure = FMath::Clamp(SceneColorInvPreExposure, 0.001f, 1000.0f);
	P.WetInvViewProjection = FMatrix44f(View.ViewMatrices.GetInvViewProjectionMatrix());
	P.WetWorldToClip = FMatrix44f(View.ViewMatrices.GetViewProjectionMatrix());
	P.WetCameraTime = FVector4f(FVector3f(View.ViewLocation), View.Family ? View.Family->Time.GetRealTimeSeconds() : 0.0f);
	P.WetSettings0 = GDirectSplatSurfaceEffectSettings0;
	// v32: encode post-rain residual-puddle state only in the local Wet-pass copy.
	// 1.00 = active rain; 1.25 = Rain mode with precipitation stopped.
	if (GDirectRainModeActive && !GDirectRainPrecipitationActive &&
		!(GWetReceiverTestMode >= 3 && GWetMeshPreviewRipples))
	{
		P.WetSettings0.X += 0.25f;
	}
	P.WetSettings1 = GDirectSplatSurfaceEffectSettings1;
	P.WetSettings2 = GDirectSplatSurfaceEffectSettings2;
	P.WetSettings3 = GDirectWetRippleSettings;
	P.WetRainGrade0 = GDirectRainOverlaySettings0;
	P.WetRainGrade1 = GDirectRainOverlaySettings1;
	P.WetLightDirectionValid = GProxyLightDepthForwardEnabled;
	P.WetLightColorIntensity = FVector4f(GDirectSplatSurfaceEffectSettings1.X, GDirectSplatSurfaceEffectSettings1.Y,
		GDirectSplatSurfaceEffectSettings1.Z, 1.0f);
	P.WetMoonDirectionSize = GDirectMoonReflectionDirectionSize;
	P.WetMoonColorStrength = GDirectMoonReflectionColorStrength;
	SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), P);
	RHICmdList.SetStreamSource(0, nullptr, 0);
	RHICmdList.DrawPrimitive(0, 1, 1);
}

void FGaussianSplatRenderer::CompositeVRDiagnostic(FRHICommandListImmediate& RHICmdList,
    const FSceneView& View, FTextureRHIRef Source, const FIntRect& SourceRect,
    const FIntRect& OutputRect, bool bAfterTonemap)
{
    SCOPED_DRAW_EVENT(RHICmdList, GS_VRDiag_MinimalComposite);
    TShaderMapRef<FGaussianSplatCompositeVS> VS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    TShaderMapRef<FGaussianSplatVRDiagnosticCompositePS> PS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    FGraphicsPipelineStateInitializer Pipeline;
    RHICmdList.ApplyCachedRenderTargets(Pipeline);
    Pipeline.RasterizerState = TStaticRasterizerState<>::GetRHI();
    Pipeline.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
    Pipeline.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha,
        BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();
    Pipeline.PrimitiveType = PT_TriangleList;
    Pipeline.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
    Pipeline.BoundShaderState.VertexShaderRHI = VS.GetVertexShader();
    Pipeline.BoundShaderState.PixelShaderRHI = PS.GetPixelShader();
    SetGraphicsPipelineState(RHICmdList, Pipeline, 0);
    RHICmdList.SetViewport(OutputRect.Min.X, OutputRect.Min.Y, 0, OutputRect.Max.X, OutputRect.Max.Y, 1);
    FGaussianSplatVRDiagnosticCompositePS::FParameters Params;
    Params.VRDiagnosticSource = Source;
    Params.VRDiagnosticSourceRect = FVector4f(SourceRect.Min.X, SourceRect.Min.Y, SourceRect.Width(), SourceRect.Height());
    Params.VRDiagnosticOutputRect = FVector4f(OutputRect.Min.X, OutputRect.Min.Y, OutputRect.Width(), OutputRect.Height());
    Params.VRDiagnosticExposure = !bAfterTonemap && View.bIsViewInfo
        ? static_cast<const FViewInfo&>(View).PreExposure : 1.0f;
    SetShaderParameters(RHICmdList, PS, PS.GetPixelShader(), Params);
    RHICmdList.SetStreamSource(0, nullptr, 0);
    RHICmdList.DrawPrimitive(0, 1, 1);
}

void FGaussianSplatRenderer::DispatchPCVRStableCompact(FRHICommandListImmediate& Cmd,FGaussianGlobalAccumulator* A,int32 Count,const FSceneView& View)
{
    GSPreviewPassCounters::FScopedCPUTime GSPreviewCPUTimeScope(GSPreviewPassCounters::CompactStablePCVR);
    SCOPED_DRAW_EVENT(Cmd,GSPCVRStableCompact);
    const uint32 N=FMath::Max(0,Count), Blocks=FMath::DivideAndRoundUp(N,1024u);
    if(!Blocks) return;
    A->EnsureCompactionBuffersAllocated(Cmd); A->EnsurePCVRBlocks(Cmd,Blocks);
    auto Barrier=[&](FBufferRHIRef B,ERHIAccess Dest){Cmd.Transition(FRHITransitionInfo(B,ERHIAccess::Unknown,Dest));};
    Barrier(A->GlobalViewDataBuffer,ERHIAccess::SRVCompute);
    Barrier(A->GlobalSortDistanceBufferAlt,ERHIAccess::UAVCompute);
    Barrier(A->GlobalSortKeysBufferAlt,ERHIAccess::UAVCompute);
    Barrier(A->PCVRBlockBuffer,ERHIAccess::UAVCompute);
    TShaderMapRef<FGSPCVRCompactCountCS> CountShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    FGSPCVRCompactCountCS::FParameters C;
    C.ViewData=A->GlobalViewDataBufferSRV;C.ScratchDepth=A->GlobalSortDistanceBufferAltUAV;
    C.ScratchRank=A->GlobalSortKeysBufferAltUAV;C.Blocks=A->PCVRBlockUAV;C.SourceCount=N;
    FGSRemovalBoxGPUData CullBoxes[GSGSRemovalBoxMax];
    C.PreCullBoxCount=0;C.PreCullTranslation=FVector3f(View.ViewMatrices.GetPreViewTranslation());C.PreCullClipToWorld=FMatrix44f(View.ViewMatrices.GetInvViewProjectionMatrix());
    if(CVarRemovalPreCull.GetValueOnRenderThread()!=0 && GSPCVR::IsActive(View) && GetVRDiagnosticMode_RenderThread()==0 && IsPCVRClearSurface_RenderThread() && !IsDirectWetSurfaceEnabled_RenderThread() && !IsDirectSnowOverlayEnabled_RenderThread())
        C.PreCullBoxCount=GetGSRemovalBoxes_RenderThread(View.Family?View.Family->Scene:nullptr,CullBoxes);
    for(uint32 i=0;i<8;++i){const auto B=i<C.PreCullBoxCount?CullBoxes[i]:FGSRemovalBoxGPUData();C.PreCullCenters[i]=B.Center;C.PreCullX[i]=B.X;C.PreCullY[i]=B.Y;C.PreCullZ[i]=B.Z;}
    if(IsVRDiagnosticLogging_RenderThread()){
        static TMap<int32,double> Last;double& T=Last.FindOrAdd(int32(View.StereoPass));const double Now=FPlatformTime::Seconds();
        if(Now-T>=1){T=Now;UE_LOG(LogTemp,Display,TEXT("[GSPCVR][RemovalPreCull] Eye=%d Enabled=%d Boxes=%u SourceCapacity=%u BeforeSort=1 FullQuadContainment=1 BoundaryFallback=1"),int32(View.StereoPass),CVarRemovalPreCull.GetValueOnRenderThread(),C.PreCullBoxCount,N);}
    }
    SetComputePipelineState(Cmd,CountShader.GetComputeShader());SetShaderParameters(Cmd,CountShader,CountShader.GetComputeShader(),C);
    Cmd.DispatchComputeShader(Blocks,1,1);UnsetShaderUAVs(Cmd,CountShader,CountShader.GetComputeShader());
    Barrier(A->PCVRBlockBuffer,ERHIAccess::UAVCompute);
    Barrier(A->GlobalSortParamsBuffer,ERHIAccess::UAVCompute);
    Barrier(A->GlobalSortIndirectArgsGlobalBuffer,ERHIAccess::UAVCompute);
    Barrier(A->GlobalDrawIndirectArgsBuffer,ERHIAccess::UAVCompute);
    TShaderMapRef<FGSPCVRCompactPrefixCS> PrefixShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    FGSPCVRCompactPrefixCS::FParameters P;
    P.Blocks=A->PCVRBlockUAV;P.SortParamsOut=A->GlobalSortParamsBufferUAV;
    P.SortDispatch=A->GlobalSortIndirectArgsGlobalBufferUAV;P.DrawArgsOut=A->GlobalDrawIndirectArgsBufferUAV;P.BlockCount=Blocks;
    SetComputePipelineState(Cmd,PrefixShader.GetComputeShader());SetShaderParameters(Cmd,PrefixShader,PrefixShader.GetComputeShader(),P);
    Cmd.DispatchComputeShader(1,1,1);UnsetShaderUAVs(Cmd,PrefixShader,PrefixShader.GetComputeShader());
    Barrier(A->PCVRBlockBuffer,ERHIAccess::UAVCompute);
    Barrier(A->GlobalSortDistanceBufferAlt,ERHIAccess::UAVCompute);Barrier(A->GlobalSortKeysBufferAlt,ERHIAccess::UAVCompute);
    Barrier(A->GlobalSortDistanceBuffer,ERHIAccess::UAVCompute);Barrier(A->GlobalSortKeysBuffer,ERHIAccess::UAVCompute);
    TShaderMapRef<FGSPCVRCompactScatterCS> ScatterShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    FGSPCVRCompactScatterCS::FParameters S;
    S.ScratchDepth=A->GlobalSortDistanceBufferAltUAV;S.ScratchRank=A->GlobalSortKeysBufferAltUAV;S.Blocks=A->PCVRBlockUAV;
    S.OutDepth=A->GlobalSortDistanceBufferUAV;S.OutIndex=A->GlobalSortKeysBufferUAV;S.SourceCount=N;S.BlockCount=Blocks;
    SetComputePipelineState(Cmd,ScatterShader.GetComputeShader());SetShaderParameters(Cmd,ScatterShader,ScatterShader.GetComputeShader(),S);
    Cmd.DispatchComputeShader(Blocks,1,1);UnsetShaderUAVs(Cmd,ScatterShader,ScatterShader.GetComputeShader());
    Barrier(A->GlobalSortDistanceBuffer,ERHIAccess::UAVCompute);Barrier(A->GlobalSortKeysBuffer,ERHIAccess::UAVCompute);
    Barrier(A->GlobalSortParamsBuffer,ERHIAccess::SRVCompute);Barrier(A->GlobalSortIndirectArgsGlobalBuffer,ERHIAccess::IndirectArgs);
    Barrier(A->GlobalDrawIndirectArgsBuffer,ERHIAccess::IndirectArgs);
    A->bPCVRPreSortCompacted=true;
    DispatchRadixSortGlobalIndirect(Cmd,A);
}

void FGaussianSplatRenderer::DispatchPCVRSourceCompact(FRHICommandListImmediate& Cmd,FGaussianSplatGPUResources* R,FGaussianGlobalAccumulator* A,uint32 Count,uint32 OriginalCount,bool UseLOD)
{
    GSPreviewPassCounters::FScopedCPUTime GSPreviewCPUTimeScope(GSPreviewPassCounters::CompactPCVR);
    GSPreviewPassCounters::AddWork(GSPreviewPassCounters::CompactPCVR, Count);
    SCOPED_DRAW_EVENT(Cmd,GSPCVRSourceCompact);
    const bool Clusters=R->bEnableNanite && R->bHasClusterData && R->bSupportsCompaction;
    if(Clusters && R->bPCVRSharedCull && R->bPCVRCompactedShared && R->PCVRCullFrame==GFrameNumberRenderThread) {
        GSPreviewPassCounters::MarkSkip(GSPreviewPassCounters::CompactPCVR); return; }
    if(!Clusters && R->PCVRIdentityCount==Count && R->PCVRIdentityBuffer==R->CompactedSplatIndicesBuffer.GetReference() && R->CompactionCapacity>=Count) {
        GSPreviewPassCounters::MarkSkip(GSPreviewPassCounters::CompactPCVR); return; }
    if(!Clusters)
    {
        // Small weather proxies get an identity input list. Do not change their
        // cluster capability flags; the original renderer remains available.
        auto Make=[&](FBufferRHIRef& B,FUnorderedAccessViewRHIRef& U,FShaderResourceViewRHIRef* V,uint32 N,bool Indirect)
        {
            auto Desc=FRHIBufferCreateDesc::Create(TEXT("GSPCVRIdentity"),N*sizeof(uint32),sizeof(uint32),BUF_UnorderedAccess|BUF_ShaderResource|BUF_StructuredBuffer|(Indirect?BUF_DrawIndirect:BUF_None)).SetInitialState(ERHIAccess::UAVCompute);
            B=Cmd.CreateBuffer(Desc);
            U=Cmd.CreateUnorderedAccessView(B,FRHIViewDesc::CreateBufferUAV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
            if(V) *V=Cmd.CreateShaderResourceView(B,FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Structured).SetStride(sizeof(uint32)));
        };
        Make(R->CompactedSplatIndicesBuffer,R->CompactedSplatIndicesBufferUAV,&R->CompactedSplatIndicesBufferSRV,FMath::Max(1u,Count),false);
        Make(R->VisibleSplatCountBuffer,R->VisibleSplatCountBufferUAV,&R->VisibleSplatCountBufferSRV,1,false);
        Make(R->IndirectDispatchArgsBuffer,R->IndirectDispatchArgsBufferUAV,nullptr,3,true);
        Make(R->IndirectDrawArgsBuffer,R->IndirectDrawArgsBufferUAV,nullptr,5,true);
        Make(R->SortIndirectArgsBuffer,R->SortIndirectArgsBufferUAV,nullptr,3,true);
        Make(R->SortParamsBuffer,R->SortParamsBufferUAV,&R->SortParamsBufferSRV,2,false);
        R->CompactionCapacity=Count;
    }
    const uint32 Blocks=FMath::DivideAndRoundUp(Count,1024u);
    A->EnsurePCVRBlocks(Cmd,Blocks);
    auto Barrier=[&](FBufferRHIRef B,ERHIAccess Dest){Cmd.Transition(FRHITransitionInfo(B,ERHIAccess::Unknown,Dest));};
    Barrier(A->PCVRBlockBuffer,ERHIAccess::UAVCompute);Barrier(A->GlobalSortDistanceBufferAlt,ERHIAccess::UAVCompute);Barrier(A->GlobalSortKeysBufferAlt,ERHIAccess::UAVCompute);
    Barrier(R->VisibleSplatCountBuffer,ERHIAccess::SRVCompute);
    if(Clusters)
    {
        Barrier(R->SplatClusterIndexBuffer,ERHIAccess::SRVCompute);Barrier(R->ClusterVisibilityBitmap,ERHIAccess::SRVCompute);
        Barrier(R->LODClusterSelectedBitmap,ERHIAccess::SRVCompute);Barrier(R->SelectedClusterBuffer,ERHIAccess::SRVCompute);
    }
    TShaderMapRef<FGSPCVRSourceCountCS> CShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    FGSPCVRSourceCountCS::FParameters C;
    auto Dummy=R->VisibleSplatCountBufferSRV;
    C.SourceCluster=Clusters?R->SplatClusterIndexBufferSRV:Dummy;C.LeafVisibility=Clusters?R->ClusterVisibilityBitmapSRV:Dummy;
    C.ParentVisibility=Clusters?R->LODClusterSelectedBitmapSRV:Dummy;C.SelectedSourceCluster=Clusters?R->SelectedClusterBufferSRV:Dummy;
    C.UseSourceClusters=Clusters?1u:0u;C.UseSourceLOD=UseLOD?1u:0u;C.OriginalSourceCount=OriginalCount;
    C.ScratchDepth=A->GlobalSortDistanceBufferAltUAV;C.ScratchRank=A->GlobalSortKeysBufferAltUAV;C.Blocks=A->PCVRBlockUAV;C.SourceCount=Count;
    SetComputePipelineState(Cmd,CShader.GetComputeShader());SetShaderParameters(Cmd,CShader,CShader.GetComputeShader(),C);Cmd.DispatchComputeShader(Blocks,1,1);UnsetShaderUAVs(Cmd,CShader,CShader.GetComputeShader());
    Barrier(A->PCVRBlockBuffer,ERHIAccess::UAVCompute);Barrier(R->VisibleSplatCountBuffer,ERHIAccess::UAVCompute);
    TShaderMapRef<FGSPCVRSourcePrefixCS> PShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    FGSPCVRSourcePrefixCS::FParameters P;P.Blocks=A->PCVRBlockUAV;P.VisibleCountOut=R->VisibleSplatCountBufferUAV;P.BlockCount=Blocks;
    SetComputePipelineState(Cmd,PShader.GetComputeShader());SetShaderParameters(Cmd,PShader,PShader.GetComputeShader(),P);Cmd.DispatchComputeShader(1,1,1);UnsetShaderUAVs(Cmd,PShader,PShader.GetComputeShader());
    Barrier(A->PCVRBlockBuffer,ERHIAccess::UAVCompute);Barrier(A->GlobalSortDistanceBufferAlt,ERHIAccess::UAVCompute);Barrier(A->GlobalSortKeysBufferAlt,ERHIAccess::UAVCompute);
    Barrier(R->CompactedSplatIndicesBuffer,ERHIAccess::UAVCompute);
    TShaderMapRef<FGSPCVRSourceScatterCS> SShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
    FGSPCVRSourceScatterCS::FParameters S;S.ScratchDepth=A->GlobalSortDistanceBufferAltUAV;S.ScratchRank=A->GlobalSortKeysBufferAltUAV;S.Blocks=A->PCVRBlockUAV;
    S.OutIndex=R->CompactedSplatIndicesBufferUAV;S.SourceCount=Count;S.BlockCount=Blocks;
    SetComputePipelineState(Cmd,SShader.GetComputeShader());SetShaderParameters(Cmd,SShader,SShader.GetComputeShader(),S);Cmd.DispatchComputeShader(Blocks,1,1);UnsetShaderUAVs(Cmd,SShader,SShader.GetComputeShader());
    Barrier(R->CompactedSplatIndicesBuffer,ERHIAccess::SRVCompute);Barrier(R->VisibleSplatCountBuffer,ERHIAccess::SRVCompute);
    if(Clusters && UseLOD && R->bPCVRSharedCull && FParse::Param(FCommandLine::Get(),TEXT("GSPCVRLODTest")))CapturePCVRLODProof(R->LODSplatTotalBuffer,R->VisibleSplatCountBuffer,OriginalCount,Count);
    R->bPCVRCompactedShared=R->bPCVRSharedCull;
    if(!Clusters){R->PCVRIdentityCount=Count;R->PCVRIdentityBuffer=R->CompactedSplatIndicesBuffer.GetReference();}
}
