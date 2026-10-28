#include "LCCCinematicGS.h"
#include "PCVRTileRenderer.h"
#include "IRenderCaptureProvider.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
// Copyright Epic Games, Inc. All Rights Reserved.

#include "GSDesktopPerfProbe.h"
#include "Engine/World.h"
#include "GaussianPCVR.h"
#include "GaussianSplatViewExtension.h"
#include "GaussianSplatRenderer.h"
#include "GaussianSplatShaders.h"
#include "GaussianSplatSceneProxy.h"
#include "GaussianGlobalAccumulator.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"
#include "SceneViewExtension.h"
#include "Misc/CoreDelegates.h"
#include "Engine/Engine.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "SceneRendering.h"
#include "ScreenPass.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "HAL/IConsoleManager.h"

    static TAutoConsoleVariable<int32> PackedFog(TEXT("gs.PCVR.PackedFog"),0,
        TEXT("PCVR lean path: 0 RGBA16F reference, 1 RG16F depth/coverage, 2 R16F depth with identical raw color alpha as coverage."),ECVF_RenderThreadSafe);
#define LOCTEXT_NAMESPACE "FLCCCinematicGSModule"
static TAutoConsoleVariable<int32> CVarPCInputCacheFix(TEXT("gs.PC.CacheFix"),1,
    TEXT("Keep exact geometry caching while refreshing colour-only effects without re-sorting. Mode 3 ignores unused lighting inputs. 0 reference, 1 enabled."),ECVF_RenderThreadSafe);


BEGIN_SHADER_PARAMETER_STRUCT(FGSPCVRTilePassParameters, )
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, TileDepthDependency)
    RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FGaussianSnowOverlayPassParameters, )
	// Dummy SRV dependency on the base GS accumulation. RDG then guarantees the
	// global sort/ViewData work issued by the base pass has completed before the
	// dedicated snow shell draw consumes those same external RHI buffers.
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, BaseDependency)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

// Pass 2 parameter struct: declares IntermediateTexture as an RDG-tracked shader resource
// so that RDG inserts the proper RTV-to-SRV barrier between Pass 1 (write) and Pass 2 (read).
//----------------------------------------------------------------------
// Console Variables for Gaussian Splatting
//----------------------------------------------------------------------

// Opt-in-only MRQ/Panorama diagnostics; does not change the rendering path.
// 0 = off, 1 = first 64 matching render views, 2 = first 256 matching render views.
TAutoConsoleVariable<int32> CVarGSPanoramaShadowTrace(
    TEXT("gs.PanoramaShadowTrace"),
    0,
    TEXT("Trace Stable BVH preparation and render-thread panorama views.\n")
    TEXT(" 0: Off (default)\n 1: First 64 matching views\n 2: First 256 matching views"),
    ECVF_RenderThreadSafe);

// Non-invasive GPU workload / buffer lifetime diagnostics for MRQ panorama.
// These counters report CPU-submitted work, NOT GPU completion or GPU timing.
TAutoConsoleVariable<int32> CVarGSPanoramaGPUTrace(
    TEXT("gs.PanoramaGPUTrace"),
    0,
    TEXT("Trace global GS dispatch inputs and buffer generations in MRQ/panorama.\n")
    TEXT(" 0: Off (default)\n 1: First 8 and every 16th view\n 2: First 256 and every 16th thereafter"),
    ECVF_RenderThreadSafe);

// v2886: shadow-value sampler. No GPU work or shadow changes at 0.
TAutoConsoleVariable<int32> CVarGSPanoramaShadowStats(
    TEXT("gs.PanoramaShadowStats"), 0,
    TEXT("Opt-in sampled CPU BVH + actual GPU ViewData shadow diagnostics. 0 off, 1 on. ")
    TEXT("GPU readback is asynchronous; no RHI flush or GPU idle wait."), ECVF_RenderThreadSafe);

// v2886: debug-only grade/fog/bloom bypass on GS shadow mask; UE tonemapper still runs.
TAutoConsoleVariable<int32> CVarGSPanoramaShadowMaskProbe(
    TEXT("gs.PanoramaShadowMaskProbe"), 0,
    TEXT("0 legacy rendering. 1 ungraded GS diagnostic mask + normal PreExposure. ")
    TEXT("2 ungraded GS mask WITHOUT GS PreExposure (UE tonemapper remains). ")
    TEXT("Only effective for Stable debug 102/103/104."), ECVF_RenderThreadSafe);

// v2888: the visually verified mode 1 is now the no-console default.
// Mode 0 remains available for regression comparison, while mode 2 remains
// the user-selected SDR-only post-tonemap isolation mode. This does not change
// any sky, weather, shadow or GS lighting UI settings.
TAutoConsoleVariable<int32> CVarGSPanoramaGSCompositeMode(
    TEXT("gs.PanoramaGSCompositeMode"), 1,
    TEXT("0 original UE pre-tonemap GS blend (manual compatibility). ")
    TEXT("1 DEFAULT: original HDR/GS look with GS PreExposure forced to 1 (UE PP still applies). ")
    TEXT("2 EXPERIMENTAL: GS-owned HDR->SDR curve, GS composite after UE Tonemap. ")
    TEXT("Mode 2 is not an HDR EXR output path and may require MRQ pane verification. ")
    TEXT("It falls back to mode 0 while GS weather Niagara particles are active."),
    ECVF_RenderThreadSafe);

// v2888: opt-in, bounded CPU-side work/buffer diagnostics. Existing GPU event
// labels (GaussianSplatRendering_Global, GaussianSplatPostGradeHDR,
// GaussianSplatPostGradeBloomComposite and GaussianSplatDedicatedWetSurface)
// are available to Unreal Insights / ProfileGPU for actual GPU timings.
// This counter never issues GPU readback, fences or RHI flushing.
TAutoConsoleVariable<int32> CVarGSPanoramaPerfTrace(
    TEXT("gs.PanoramaPerfTrace"), 0,
    TEXT("Low-overhead 3DGS CPU scheduling / HDR target diagnostics. ")
    TEXT("0 off (default), 1 first 4 views and every 64th view. ")
    TEXT("Not GPU milliseconds: capture GPU timings with Unreal Insights or ProfileGPU."),
    ECVF_RenderThreadSafe);

// v2888: enabled for full-extent views only. 0 allows regression testing with
// the original HDR target clear behavior, without changing the color pipeline.
TAutoConsoleVariable<int32> CVarGSPanoramaSkipHDRClear(
    TEXT("gs.PanoramaSkipHDRClear"), 1,
    TEXT("1 default: skip redundant RGBA16F PostGrade/Wet clear on complete view. ")
    TEXT("0 compatibility: use original clear for all views. Cropped MRQ views ")
    TEXT("always clear, regardless of this setting."),
    ECVF_RenderThreadSafe);

// v2889 rain-only GPU safety. These do not alter Clear/Fog/Snow/weather presets.
TAutoConsoleVariable<int32> CVarGSPanoramaRainWetSafeClear(
    TEXT("gs.PanoramaRainWetSafeClear"), 1,
    TEXT("Rain dedicated Wet HDR target: 1 force a transparent clear before wet rendering (default/safe); ")
    TEXT("0 use the v2888 full-extent no-clear path. Non-wet weather is unchanged."),
    ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarGSPanoramaRainBloomHalfSamples(
    TEXT("gs.PanoramaRainBloomHalfSamples"), 1,
    TEXT("Rain dedicated Wet bloom only: 1 use 5 rings x 4 directions (20 taps, default); ")
    TEXT("0 use the original 5 x 8 directions (40 taps). Non-wet weather is unchanged."),
    ECVF_RenderThreadSafe);

// v2890: Rain-only distance-dispatch watchdog mitigation. The exact same splats are
// processed and written to the exact same indices; only one very large compute
// dispatch is split into bounded chunks. Clear/Fog/Snow and all authored weather
// settings remain on their existing path.
TAutoConsoleVariable<int32> CVarGSPanoramaRainGSChunkedDistances(
    TEXT("gs.PanoramaRainGSChunkedDistances"), 1,
    TEXT("Rain GS CalcDistances safety: 1 split the global distance compute into bounded dispatches (default); ")
    TEXT("0 use the original single dispatch. This never drops splats or changes sort math."),
    ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarGSPanoramaRainGSChunkSize(
    TEXT("gs.PanoramaRainGSChunkSize"), 524288,
    TEXT("Rain GS CalcDistances splats per dispatch. Default 524288; clamped and aligned to 256 threads."),
    ECVF_RenderThreadSafe);

// v2893: Rain panorama residency-pressure guard. MRQ panorama can queue many
// 512x512 ViewFamilies before the RTX 3060 Ti finishes them, keeping transient
// Lumen/TSR/fog/GS render targets resident at once. Drain only rain panorama-like
// square subviews, and only every N completed GS composites. This changes command
// queue depth only; it does not change splat count, weather, lighting, HDR or pixels.
TAutoConsoleVariable<int32> CVarGSPanoramaRainGPUDrainInterval(
    TEXT("gs.PanoramaRainGPUDrainInterval"), 4,
    TEXT("Rain panorama VRAM safety: block for GPU completion after every N square panorama subviews. ")
    TEXT("Default 4; 0 disables. This is a queue-depth/residency guard only and does not change image quality."),
    ECVF_RenderThreadSafe);

// v2896: Snow adds a second actor-owned GS shell and can raise the working set
// from ~4.95M to ~8.08M splats. The legacy SnowIntermediate overlay is not used
// for baked relief, so drain before each panorama ViewFamily rather than waiting
// for the final composite. Default 1 fully bounds in-flight snow panorama views.
TAutoConsoleVariable<int32> CVarGSPanoramaSnowGPUDrainInterval(
    TEXT("gs.PanoramaSnowGPUDrainInterval"), 1,
    TEXT("Snow panorama GPU safety: block for GPU completion before every N square panorama ViewFamilies while baked snow GS is visible. ")
    TEXT("Default 1; 0 disables. This changes queue depth only; snow splats, lighting and image quality are unchanged."),
    ECVF_RenderThreadSafe);

TAutoConsoleVariable<float> CVarGSPanoramaGSToneMapEV(
    TEXT("gs.PanoramaGSToneMapEV"), 0.0f,
    TEXT("Mode 2 only: EV compensation (-4..+4) before independent GS display mapping. Default 0."),
    ECVF_RenderThreadSafe);

/** Show cluster debug visualization (Nanite-style coloring) */
TAutoConsoleVariable<int32> CVarShowClusterBounds(
	TEXT("gs.ShowClusterBounds"),
	0,
	TEXT("Debug visualization for Gaussian Splat clusters (Nanite-style).\n")
	TEXT("When enabled, shows cluster colors on black background (like Nanite debug view).\n")
	TEXT(" 0: Off (default)\n")
	TEXT(" 1: Show cluster colors (each cluster gets a unique random color)"),
	ECVF_RenderThreadSafe);

/** Legacy render-budget control retained for console compatibility.
 *  Correctness Fix 1.1 intentionally ignores positive values until a camera-priority
 *  cluster budget is implemented. Index-order truncation can remove nearby geometry. */
TAutoConsoleVariable<int32> CVarMaxRenderBudget(
	TEXT("gs.MaxRenderBudget"),
	0,
	TEXT("Reserved render budget control. Correctness Fix 1.1 renders the complete visible set.\n")
	TEXT("Positive values are currently ignored because compacted splat order is not camera-priority ordered.\n")
	TEXT("Default: 0 (complete visible rendering)."),
	ECVF_RenderThreadSafe);

/** Transition-only diagnostics for the global sort cache.
 *  Logs only when the cache decision changes, avoiding per-frame spam. */
TAutoConsoleVariable<int32> CVarSortCacheDiagnostics(
	TEXT("gs.SortCacheDiagnostics"),
	0,
	TEXT("Log Gaussian global-cache state transitions.\n")
	TEXT(" 0: Off (default)\n")
	TEXT(" 1: Log transitions and CPU invalidation reason"),
	ECVF_RenderThreadSafe);

/** Debug: Force a specific LOD level for debugging LOD hierarchy */
TAutoConsoleVariable<int32> CVarDebugForceLODLevel(
	TEXT("gs.DebugForceLODLevel"),
	-1,
	TEXT("Force rendering of a specific LOD level for debugging (only affects Nanite-enabled assets).\n")
	TEXT("This ignores normal LOD selection and forces all clusters to use specified level.\n")
	TEXT(" -1: Auto - normal LOD selection based on distance/error (default)\n")
	TEXT("  0: Force leaf clusters only - render original splats (finest detail)\n")
	TEXT("  1+: Force specific LOD level (1 = first parent level, 2 = second, etc.)\n")
	TEXT("Note: Higher levels have fewer, coarser splats. Max level depends on asset size.\n")
	TEXT("Use with gs.ShowClusterBounds 2 to visualize which LOD level is being rendered."),
	ECVF_RenderThreadSafe);

// Export for other modules
int32 GGaussianSplatShowClusterBounds = 0;

// Helper to get the renderer module
static IRendererModule& GetRendererModuleRef()
{
	return FModuleManager::GetModuleChecked<IRendererModule>("Renderer");
}

void FLCCCinematicGSModule::StartupModule()
{
    VREffectsWorldTickHandle=FWorldDelegates::OnWorldTickStart.AddLambda([](UWorld* World,ELevelTick,float){AGSDesktopPerfProbe::TryStartVREffectsTest(World);});
	// Register the shader directory so we can use our custom shaders
	FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("LCCCinematicGS"))->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/LCCCinematicGS"), PluginShaderDir);

	// Allocate the global accumulator (buffers are created lazily on first render)
	GlobalAccumulator = MakeUnique<FGaussianGlobalAccumulator>();

	// Register post-opaque render delegate for rendering
	PostOpaqueRenderDelegateHandle = GetRendererModuleRef().RegisterPostOpaqueRenderDelegate(
		FPostOpaqueRenderDelegate::CreateRaw(this, &FLCCCinematicGSModule::OnPostOpaqueRender_RenderThread));

	// Defer view extension creation until GEngine is valid
	// (StartupModule runs before GEngine is initialized, causing an ensure failure)
	PostEngineInitDelegateHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FLCCCinematicGSModule::OnPostEngineInit);

	UE_LOG(LogTemp, Log, TEXT("LCCCinematicGS module started. Shader directory: %s"), *PluginShaderDir);
}

void FLCCCinematicGSModule::OnPostEngineInit()
{
	// Skip view extension creation during cooking/commandlet ??GEngine is null in those contexts
	if (IsRunningCommandlet() || !GEngine)
	{
		UE_LOG(LogTemp, Log, TEXT("LCCCinematicGS: Skipping ViewExtension creation (commandlet/cook mode)"));
		return;
	}

	// Now GEngine is valid ??safe to create the view extension
	ViewExtension = FSceneViewExtensions::NewExtension<FGaussianSplatViewExtension>();

	if (!ViewExtension.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("LCCCinematicGS: Failed to create ViewExtension!"));
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("LCCCinematicGS: ViewExtension created successfully (deferred init)"));
	}
}

// Opt-in capture only. Modelled on RenderCore's RDG FScopedCapture, but with
// flags=0 so it never launches an interactive RenderDoc window. No idle waits
// or readbacks occur in normal operation. The provider synchronises only the
// two diagnostic captures, whose frame times are not production benchmarks.
static TAutoConsoleVariable<int32> CVarGSPCVRDrawCaptureMask(
    TEXT("gs.PCVR.DrawCaptureMask"),0,
    TEXT("Diagnostic: capture the next actual PCVR GS pass once per eye (1 left, 2 right, 3 both). Requires -AttachRenderDoc."),ECVF_RenderThreadSafe);

class FGSPCVRDrawCaptureScope
{
    FRDGBuilder* Builder=nullptr;
    int32 Eye=0;
public:
    FGSPCVRDrawCaptureScope(FRDGBuilder& GraphBuilder,const FSceneView& View)
    {
        static uint32 ConsumedEyes=0;
        const uint32 Requested=uint32(CVarGSPCVRDrawCaptureMask.GetValueOnRenderThread())&3u;
        if(!Requested || !GSPCVR::IsActive(View) || !GSPCVR::IsRealtimeHMD(View)) return;
        Eye=int32(View.StereoPass);
        const uint32 EyeBit=Eye==1 ? 1u : (Eye==2 ? 2u : 0u);
        if(!EyeBit || !(Requested&EyeBit) || (ConsumedEyes&EyeBit)) return;
        if(!IRenderCaptureProvider::IsAvailable())
        {
            ConsumedEyes|=EyeBit;
            UE_LOG(LogTemp,Error,TEXT("[GSDrawCapture][Unavailable] Eye=%d Provider=0"),Eye);
            return;
        }
        ConsumedEyes|=EyeBit;
        FString Prefix;
        if(!FParse::Value(FCommandLine::Get(),TEXT("GSVRDrawCapturePrefix="),Prefix))
            Prefix=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("RenderDocCaptures/GSDraw"));
        const FString CapturePath=FString::Printf(TEXT("%s_Eye%d.rdc"),*Prefix,Eye);
        SetEmitDrawEvents(true); // Capture-only markers; normal launches remain unchanged.
        Builder=&GraphBuilder;
        const FIntRect Rect=View.UnscaledViewRect;
        GraphBuilder.AddPass(RDG_EVENT_NAME("GSDrawCaptureBegin Eye=%d",Eye),ERDGPassFlags::NeverCull,
            [CapturePath,Rect,CaptureEye=Eye](FRHICommandListImmediate& Cmd)
            {
                UE_LOG(LogTemp,Display,TEXT("[GSDrawCapture][Begin] Eye=%d Rect=%dx%d File=%s DiagnosticOnly=1"),
                    CaptureEye,Rect.Width(),Rect.Height(),*CapturePath);
                IRenderCaptureProvider::Get().BeginCapture(&Cmd,0,CapturePath);
            });
    }
    ~FGSPCVRDrawCaptureScope()
    {
        if(Builder)
            Builder->AddPass(RDG_EVENT_NAME("GSDrawCaptureEnd Eye=%d",Eye),ERDGPassFlags::NeverCull,
                [CaptureEye=Eye](FRHICommandListImmediate& Cmd)
                {
                    IRenderCaptureProvider::Get().EndCapture(&Cmd);
                    UE_LOG(LogTemp,Display,TEXT("[GSDrawCapture][End] Eye=%d DiagnosticOnly=1"),CaptureEye);
                });
    }
};

void FLCCCinematicGSModule::OnPostOpaqueRender_RenderThread(FPostOpaqueRenderParameters& Parameters)
{
	FGaussianSplatViewExtension* Ext = FGaussianSplatViewExtension::Get();
	if (!Ext || !Parameters.GraphBuilder || !Parameters.View)
	{
		return;
	}

	FRDGBuilder& GraphBuilder = *Parameters.GraphBuilder;
	const FSceneView* SceneView = reinterpret_cast<const FSceneView*>(Parameters.View);
	if (!SceneView->Family || !SceneView->Family->Scene || SceneView->bIsSceneCapture)
	{
		return;
	}

	// A globally registered post-opaque delegate can run for unrelated levels,
	// Niagara previews and asset editor scenes. Leave those scenes untouched.
	TArray<FGaussianSplatSceneProxy*> Proxies;
	Ext->GetRegisteredProxies(Proxies);
	Proxies.RemoveAll([Scene = SceneView->Family->Scene](const FGaussianSplatSceneProxy* Proxy)
	{
		return !Proxy || &Proxy->GetScene() != Scene;
	});
	if (Proxies.IsEmpty())
	{
		return;
	}

    const bool bPCVREyeCache=GSPCVR::IsActive(*SceneView) && GSPCVR::IsRealtimeHMD(*SceneView);
    if(bPCVREyeCache) PCVRLastStereoFrame=GFrameNumberRenderThread;
    // Reclaim after a quiet period, not while another graph could hold a raw
    // accumulator pointer from an earlier eye in the current render frame.
    if(!bPCVREyeCache && GFrameNumberRenderThread>PCVRLastStereoFrame+120) PCVREyeAccumulators.Empty();
    TUniquePtr<FGaussianGlobalAccumulator>& ActiveAccumulator=bPCVREyeCache
        ? PCVREyeAccumulators.FindOrAdd(static_cast<int32>(SceneView->StereoPass)) : this->GlobalAccumulator;
    if(!ActiveAccumulator.IsValid()) ActiveAccumulator=MakeUnique<FGaussianGlobalAccumulator>();

	// The proxy-mask SceneCapture views only need the hidden proxy mesh. Rendering
	// Gaussian splats into those nested view families also queues an after-tonemap
	// composite whose RDG texture belongs to the capture graph. If that pointer is
	// later consumed by the editor view graph RDG validation correctly reports a
	// dangling foreign resource. Never create GS intermediate/composite resources
	// for SceneCapture view families.
	if (!SceneView->Family || SceneView->bIsSceneCapture)
	{
		return;
	}

	// Mode 03 presents the real UE-lit proxy mesh only. Do not hide the GS scene
	// component itself because it is also the attachment parent of that proxy mesh;
	// suppress just this custom splat pass so switching modes is immediate and cannot
	// strand either child or parent in an invisible hierarchy state.
	if (FGaussianSplatRenderer::GetStableShadowDebugMode() == 73)
	{
		return;
	}


	// Sort proxies back-to-front for correct depth ordering
	if (Proxies.Num() > 1)
	{
		FVector CameraPosition = SceneView->ViewMatrices.GetViewOrigin();
		Proxies.Sort([CameraPosition](const FGaussianSplatSceneProxy& A, const FGaussianSplatSceneProxy& B)
		{
			float DistA = FVector::DistSquared(CameraPosition, A.GetBounds().Origin);
			float DistB = FVector::DistSquared(CameraPosition, B.GetBounds().Origin);
			return DistA > DistB;
		});
	}

	FRDGTexture* ColorTexture = Parameters.ColorTexture;
	FRDGTexture* DepthTexture = Parameters.DepthTexture;
	if (!ColorTexture)
	{
		return;
	}

    FGSPCVRDrawCaptureScope DrawCaptureScope(GraphBuilder,*SceneView);
	const int32 VRDiagnosticMode = FGaussianSplatRenderer::GetVRDiagnosticMode_RenderThread();
    // The GS intermediate is single-sampled. PostOpaque supplies the MSAA TARGET,
    // which cannot be attached as its DSV. Use UE's already-resolved scene depth.
    if (DepthTexture && DepthTexture->Desc.NumSamples > 1)
    {
        FRDGTexture* ResolvedDepth = Parameters.SceneTexturesUniformParams
            ? Parameters.SceneTexturesUniformParams->GetParameters()->SceneDepthTexture : nullptr;
        if (!ensureMsgf(ResolvedDepth && ResolvedDepth->Desc.NumSamples == 1 &&
            ResolvedDepth->Desc.Extent == ColorTexture->Desc.Extent &&
            EnumHasAnyFlags(ResolvedDepth->Desc.Flags, TexCreate_DepthStencilTargetable | TexCreate_DepthStencilResolveTarget),
            TEXT("GS single-sample MRTs require a compatible resolved scene depth texture"))) return;
        DepthTexture = ResolvedDepth;
    }

	int32 DebugMode = CVarShowClusterBounds.GetValueOnRenderThread();

    struct FSharedTargets
    {
        const FRDGBuilder* Graph=nullptr;const FSceneViewFamily* Family=nullptr;uint64 Frame=0;
        int32 Mode=-1;bool Fog=false;bool Lean=false;uint8 Packed=0;uint8 EyeMask=0;FIntPoint Extent=FIntPoint::ZeroValue;FIntRect Rect;
        FRDGTexture *Color=nullptr,*Depth=nullptr,*Normal=nullptr,*Snow=nullptr,*FogDepth=nullptr;
    };
    static FSharedTargets Shared;
    const FSceneView *PairLeft=nullptr,*PairRight=nullptr;
    const bool bShareTargets=GSPCVR::StereoPair(*SceneView,PairLeft,PairRight) && SceneView->UnscaledViewRect==Parameters.ViewportRect;
    const uint8 EyeBit=SceneView==PairLeft?1u:2u;
    // Conservative first consumer mask. Multiple proxies and every wet/snow
    // surface mode retain all original outputs, including diagnostic previews.
    const bool bPCVRLeanOutputs=VRDiagnosticMode==0 && DebugMode==0 && Proxies.Num()==1 &&
        Proxies[0]->GetLightingPreviewMode()==0 && FGaussianSplatRenderer::GetStableShadowDebugMode()==0 &&
        GSPCVR::LeanTargets(*SceneView) &&
        !FGaussianSplatRenderer::IsDirectWetSurfaceEnabled_RenderThread() &&
        !FGaussianSplatRenderer::IsDirectSnowOverlayEnabled_RenderThread() &&
        FGaussianSplatRenderer::IsPCVRClearSurface_RenderThread();
    const int32 FogPackingMode=bPCVRLeanOutputs ? FMath::Clamp(PackedFog.GetValueOnRenderThread(),0,2) : 0;
    const bool bPackedFog=FogPackingMode!=0;
    // V15-B: retain the existing fog MRT whenever GS native fog is active.
    // Only a true PCVR lean source pass with *inactive* GS fog may remove RT2.
    const bool bNativeFogEnabled=FGaussianSplatRenderer::IsNativeFogCompositeEnabled_RenderThread();
    const auto* TrimFogCVar=IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("gs.PCVR.TrimInactiveFogMRT"));
    const bool bTrimInactiveFog=TrimFogCVar && TrimFogCVar->GetValueOnRenderThread()!=0;
    const bool bFogTarget=VRDiagnosticMode<2 &&
        (bNativeFogEnabled || (bPCVRLeanOutputs && !bTrimInactiveFog));
    const FIntRect Rect=Parameters.ViewportRect;
    const bool bDisjoint=Rect.Min.X>=Shared.Rect.Max.X || Rect.Max.X<=Shared.Rect.Min.X || Rect.Min.Y>=Shared.Rect.Max.Y || Rect.Max.Y<=Shared.Rect.Min.Y;
    const bool bReuseTargets=bShareTargets && Shared.Graph==&GraphBuilder && Shared.Family==SceneView->Family &&
        Shared.Frame==GFrameNumberRenderThread && Shared.Mode==VRDiagnosticMode && Shared.Fog==bFogTarget && Shared.Lean==bPCVRLeanOutputs && Shared.Packed==FogPackingMode &&
        Shared.Extent==ColorTexture->Desc.Extent && bDisjoint && Shared.Color &&
        Shared.EyeMask!=0 && (Shared.EyeMask&EyeBit)==0;
    const ERenderTargetLoadAction GSLoad=bReuseTargets?ERenderTargetLoadAction::ELoad:ERenderTargetLoadAction::EClear;

	// Scene-linear HDR intermediate. GS color is decoded to linear in CalcViewData,
	// then all splats are accumulated here before Unreal's shared tonemapper.
	FRDGTextureDesc IntermediateDesc = FRDGTextureDesc::Create2D(
		ColorTexture->Desc.Extent,
		PF_FloatRGBA,  // Need alpha channel for accumulation tracking
		FClearValueBinding(FLinearColor::Transparent),
		TexCreate_RenderTargetable | TexCreate_ShaderResource);
	FRDGTexture* IntermediateTexture = bReuseTargets ? Shared.Color : GraphBuilder.CreateTexture(IntermediateDesc, TEXT("GaussianSplatIntermediateRT"));
	FRDGTextureDesc IntermediateDepthDesc = FRDGTextureDesc::Create2D(
		ColorTexture->Desc.Extent, PF_R32_FLOAT,
		FClearValueBinding(FLinearColor(HALF_WORLD_MAX, HALF_WORLD_MAX, HALF_WORLD_MAX, 1.0f)),
		TexCreate_RenderTargetable | TexCreate_ShaderResource);
	FRDGTexture* IntermediateDepthTexture = VRDiagnosticMode == 3 ? nullptr : (bReuseTargets ? Shared.Depth : GraphBuilder.CreateTexture(
		IntermediateDepthDesc, TEXT("GaussianSplatIntermediateDepthRT")));

	// Fog-only panorama-safe visible-depth accumulation. This target exists only
	// while dense fog is active; non-fog modes keep the previous MRT bandwidth.
	FRDGTexture* IntermediateFogDepthTexture = nullptr;
	if (bFogTarget)
	{
		FRDGTextureDesc IntermediateFogDepthDesc = FRDGTextureDesc::Create2D(
			ColorTexture->Desc.Extent,
			FogPackingMode==2 ? PF_R16F : (bPackedFog ? PF_G16R16F : PF_FloatRGBA),
			FClearValueBinding(FLinearColor::Transparent),
			TexCreate_RenderTargetable | TexCreate_ShaderResource);
		IntermediateFogDepthTexture = bReuseTargets ? Shared.FogDepth : GraphBuilder.CreateTexture(
			IntermediateFogDepthDesc, TEXT("GaussianSplatFogVisibleDepthRT"));
	}

	FRDGTextureDesc IntermediateNormalDesc = FRDGTextureDesc::Create2D(
		ColorTexture->Desc.Extent, PF_FloatRGBA,
		FClearValueBinding(FLinearColor(0.5f, 0.5f, 1.0f, 0.0f)),
		TexCreate_RenderTargetable | TexCreate_ShaderResource);
	FRDGTexture* IntermediateNormalTexture = (VRDiagnosticMode >= 2 || bPCVRLeanOutputs) ? nullptr : (bReuseTargets ? Shared.Normal : GraphBuilder.CreateTexture(
		IntermediateNormalDesc, TEXT("GaussianSplatStableNormalRT")));
	// Snow-only occlusion depth. It is written by the normal GS pass with an
	// opacity threshold, so faint foreground Gaussian tails cannot cut deeper snow.
	// No existing GS/depth/weather target is repurposed.
	FRDGTexture* SnowOcclusionDepthTexture = (VRDiagnosticMode >= 2 || bPCVRLeanOutputs) ? nullptr : (bReuseTargets ? Shared.Snow : GraphBuilder.CreateTexture(
		IntermediateDepthDesc, TEXT("GaussianSplatSnowOcclusionDepthRT")));
	FRDGTexture* SnowIntermediateTexture = nullptr;
	FRDGTexture* SnowIntermediateDepthTexture = nullptr;
	bool bSnowOverlayEnabled = false;
    if(bShareTargets)
    {
        Shared.Graph=&GraphBuilder;Shared.Family=SceneView->Family;Shared.Frame=GFrameNumberRenderThread;
        Shared.Mode=VRDiagnosticMode;Shared.Fog=bFogTarget;Shared.Lean=bPCVRLeanOutputs;Shared.Packed=FogPackingMode;Shared.Extent=ColorTexture->Desc.Extent;Shared.Rect=Rect;
        Shared.EyeMask=bReuseTargets?uint8(Shared.EyeMask|EyeBit):EyeBit;
        Shared.Color=IntermediateTexture;Shared.Depth=IntermediateDepthTexture;Shared.Normal=IntermediateNormalTexture;
        Shared.Snow=SnowOcclusionDepthTexture;Shared.FogDepth=IntermediateFogDepthTexture;
    }


	// Pass 1: Render splats to the scene-linear HDR intermediate
	FGSPCVRTilePassParameters* Pass1Parameters = GraphBuilder.AllocParameters<FGSPCVRTilePassParameters>();
    Pass1Parameters->TileDepthDependency = DepthTexture;
	Pass1Parameters->RenderTargets[0] = FRenderTargetBinding(IntermediateTexture, GSLoad);
	if (IntermediateDepthTexture)
	{
	Pass1Parameters->RenderTargets[1] = FRenderTargetBinding(IntermediateDepthTexture, GSLoad);
	}
    if (VRDiagnosticMode < 2 && !bPCVRLeanOutputs)
    {
	Pass1Parameters->RenderTargets[2] = FRenderTargetBinding(IntermediateNormalTexture, GSLoad);
	Pass1Parameters->RenderTargets[3] = FRenderTargetBinding(SnowOcclusionDepthTexture, GSLoad);
    }
	if (IntermediateFogDepthTexture)
	{
		Pass1Parameters->RenderTargets[bPCVRLeanOutputs ? 2 : 4] = FRenderTargetBinding(
			IntermediateFogDepthTexture, GSLoad);
	}
	if (DepthTexture)
	{
		// Scene depth is input-only. The plugin may occlude its own 3DGS against UE
		// geometry, but it must not alter depth/stencil seen by any other primitive.
		Pass1Parameters->RenderTargets.DepthStencil = FDepthStencilBinding(
			DepthTexture,
			ERenderTargetLoadAction::ELoad,
			ERenderTargetLoadAction::ELoad,
			FExclusiveDepthStencil::DepthRead_StencilRead
		);
	}

	if (!ActiveAccumulator.IsValid())
	{
		return;
	}

	//------------------------------------------------------------------
	// GLOBAL ACCUMULATOR PATH: Phase 1 (per-proxy CalcViewData) +
	// Phase 2 (single CalcDistances + RadixSort) + single DrawSplats
	//------------------------------------------------------------------

		// Build the list of visible proxies and compute total splat count (CPU-side)
		struct FProxyRenderInfo
		{
			FGaussianSplatSceneProxy* Proxy;
			FMatrix LocalToWorld;
			uint32 GlobalBaseOffset;
			bool bUseLODRendering;
			float DistanceToCamera;  // For budget priority sorting (closer = higher priority)
		};
		TArray<FProxyRenderInfo> VisibleProxies;
		uint32 TotalSplatCount = 0;
		bool bAllNanite = true;  // True if every visible proxy supports compaction
		bool bForceFullSortRequested = false;

		FVector CameraLocation = SceneView->ViewLocation;

		for (FGaussianSplatSceneProxy* Proxy : Proxies)
		{
			if (!Proxy) continue;
			if (&Proxy->GetScene() != SceneView->Family->Scene) continue;
			if (!Proxy->IsShown(SceneView)) continue;

			const FBoxSphereBounds& Bounds = Proxy->GetBounds();
			if (!SceneView->ViewFrustum.IntersectBox(Bounds.Origin, Bounds.BoxExtent)) continue;

			FGaussianSplatGPUResources* GPUResources = Proxy->GetGPUResources();
			if (!GPUResources || !GPUResources->IsValid()) continue;

			FProxyRenderInfo Info;
			Info.Proxy = Proxy;
			Info.LocalToWorld = Proxy->GetLocalToWorld();
			Info.GlobalBaseOffset = 0;  // Will be computed after sorting
			Info.bUseLODRendering = GPUResources->bEnableNanite && GPUResources->bHasLODSplats && (!Proxy->ShouldDisableLODInReference() || FParse::Param(FCommandLine::Get(),TEXT("GSPCVRLODTest")));
			Info.DistanceToCamera = FVector::Dist(Bounds.Origin, CameraLocation);
			bForceFullSortRequested |= Proxy->ShouldForceFullSortEveryFrame();
			// Receiving Unreal lighting does not change splat depth order. Lighting-only
			// changes refresh ViewData color while reusing the existing distance sort.
			VisibleProxies.Add(Info);

			// All proxies must support Nanite compaction for the fast global path
			if (!GPUResources->bEnableNanite || !GPUResources->bHasClusterData || !GPUResources->bSupportsCompaction)
			{
				bAllNanite = false;
			}
		}


		// Compute GlobalBaseOffset and TotalSplatCount for the complete visible proxy list
		uint64 Count64 = 0;
		for (FProxyRenderInfo& Info : VisibleProxies)
		{
			const int32 ProxyCount = Info.Proxy->GetSplatCount();
			// The non-compaction distance pass uses an int32 count, and output
			// offsets are uint32. Never wrap either one when preparing a dispatch.
			if (ProxyCount <= 0 || Count64 + uint64(ProxyCount) > uint64(MAX_int32))
			{
				UE_LOG(LogTemp, Error,
					TEXT("[GSPanoramaGPU][Abort] Invalid visible-proxy splat range: Proxy=%p Count=%d PreviousTotal=%llu"),
					static_cast<const void*>(Info.Proxy), ProxyCount,
					static_cast<unsigned long long>(Count64));
				return; // Never silently truncate an otherwise visible GS proxy.
			}
			Info.GlobalBaseOffset = static_cast<uint32>(Count64);
			Count64 += uint64(ProxyCount);
		}
		TotalSplatCount = static_cast<uint32>(Count64);

		// Safety: global accumulator only supports up to MAX_PROXY_COUNT proxies
		if ((uint32)VisibleProxies.Num() > FGaussianGlobalAccumulator::MAX_PROXY_COUNT)
		{
			bAllNanite = false;
		}

		// Trace the actual view that will enter the GS renderer (not an inferred MRQ Pane ID).
		// Record scene identity and per-proxy GPU status before the first CalcViewData pass.
		// This is deliberately capped across the trace session to avoid MRQ log flooding.
		// Per-scene budgets prevent active editor viewports from consuming an MRQ world's trace.
		static TMap<const FSceneInterface*, uint32> PanoramaShadowViewsPerScene; // render thread only
		const int32 PanoramaTraceLevel = CVarGSPanoramaShadowTrace.GetValueOnRenderThread();
		if (PanoramaTraceLevel <= 0)
		{
			if (!PanoramaShadowViewsPerScene.IsEmpty()) PanoramaShadowViewsPerScene.Empty();
		}
		else if (PanoramaShadowViewsPerScene.FindRef(SceneView->Family->Scene) < (PanoramaTraceLevel >= 2 ? 256u : 64u))
		{
			uint32& PanoramaShadowViewLogCount = PanoramaShadowViewsPerScene.FindOrAdd(SceneView->Family->Scene);
			++PanoramaShadowViewLogCount;
			const FVector AxisX = SceneView->ViewMatrices.GetInvViewMatrix().GetUnitAxis(EAxis::X);
			UE_LOG(LogTemp, Log,
				TEXT("[GSPanoramaTrace][RT][View] Sample=%u Scene=%p View=%p RenderFrame=%llu Origin=(%.2f,%.2f,%.2f) InvViewXAxis=(%.4f,%.4f,%.4f) VisibleProxies=%d Splats=%u DebugMode=%d"),
				PanoramaShadowViewLogCount, static_cast<const void*>(SceneView->Family->Scene),
				static_cast<const void*>(SceneView), static_cast<unsigned long long>(GFrameNumberRenderThread),
				SceneView->ViewLocation.X, SceneView->ViewLocation.Y, SceneView->ViewLocation.Z,
				AxisX.X, AxisX.Y, AxisX.Z, VisibleProxies.Num(), TotalSplatCount,
				FGaussianSplatRenderer::GetStableShadowDebugMode());
			for (const FProxyRenderInfo& Info : VisibleProxies)
			{
				const FGaussianSplatGPUResources* Res = Info.Proxy->GetGPUResources();
				if (!Res) continue;
				const bool bFieldReady =
					Res->StableProxyFieldValid != 0u &&
					Res->StableProxyVertexProofBufferSRV.IsValid() &&
					Res->StableProxyVertexCount > 0u &&
					Info.Proxy->GetStableFieldRevision_RenderThread() != 0u;
				UE_LOG(LogTemp, Log,
					TEXT("[GSPanoramaTrace][RT][ViewProxy] Sample=%u Proxy=%p BindingRev=%u FieldRequested=%u GPUFieldRev=%u GPUFieldValid=%u ProofSRV=%d ProxyVerts=%u ProxyTris=%u BindingCount=%u Lights=%u Ready=%d"),
					PanoramaShadowViewLogCount, static_cast<const void*>(Info.Proxy),
					Info.Proxy->GetStableBindingRevision_RenderThread(),
					Info.Proxy->GetStableFieldRequestRevision_RenderThread(),
					Info.Proxy->GetStableFieldRevision_RenderThread(),
					Res->StableProxyFieldValid, Res->StableProxyVertexProofBufferSRV.IsValid() ? 1 : 0,
					Res->StableProxyVertexCount, Res->StableProxyTriangleCount, Res->StableSplatBindingCount,
					Info.Proxy->GetLightingData().LightCount, bFieldReady ? 1 : 0);
			}
		}

		if (TotalSplatCount == 0)
		{
			return;
		}

		// An independent counter from PanoramaShadowTrace: no Pane/time-sample
		// identity is implied by this number. Logging is opt-in and throttled.
		static TMap<const FSceneInterface*, uint32> GPUTraceViewsPerScene;
		uint32 GPUTraceViewSerial = 0;
		bool bTraceThisGPUView = false;
		const int32 GPUTraceLevel = CVarGSPanoramaGPUTrace.GetValueOnRenderThread();
		if (GPUTraceLevel <= 0)
		{
			if (!GPUTraceViewsPerScene.IsEmpty()) GPUTraceViewsPerScene.Empty();
		}
		else
		{
			uint32& Serial = GPUTraceViewsPerScene.FindOrAdd(SceneView->Family->Scene);
			GPUTraceViewSerial = ++Serial;
			bTraceThisGPUView = GPUTraceLevel >= 2
				? (Serial <= 256u || Serial % 16u == 0u)
				: (Serial <= 8u || Serial % 16u == 0u);
		}

		// Check camera-static skip: if nothing has changed, skip Phase 1+2 and reuse cached sort
		// Use ProjectionNoAAMatrix to ignore TSR/TAA per-frame jitter that changes every frame
        if(!bAllNanite && GSPCVR::IsActive(*SceneView) && VisibleProxies.Num()<=FGaussianGlobalAccumulator::MAX_PROXY_COUNT)
            for(const auto& I:VisibleProxies)
            { const auto* R=I.Proxy->GetGPUResources(); if(R && R->bEnableNanite && R->bHasClusterData && R->bSupportsCompaction){bAllNanite=true;break;} }
		FMatrix CurrentVP = SceneView->ViewMatrices.GetWorldToView() * SceneView->ViewMatrices.GetViewToClipNoAA();
		int32 CurrentDebugMode = DebugMode;
		int32 CurrentDebugForceLODLevel = CVarDebugForceLODLevel.GetValueOnRenderThread();
        const bool bPCInputCacheFix=CVarPCInputCacheFix.GetValueOnRenderThread()!=0 && GSPCVR::IsActive(*SceneView);
        const bool bBaselineCacheIsolation=bPCInputCacheFix && VRDiagnosticMode==3;
		uint32 CurrentLightingHash = GetTypeHash(VRDiagnosticMode);
		// Stable diagnostic selection is lighting/ViewData-only. Including it here lets
		// 70 <-> 71 refresh the packed stable visibility channel without touching geometry or sort.
		CurrentLightingHash = HashCombine(CurrentLightingHash,
			GetTypeHash(FGaussianSplatRenderer::GetStableShadowDebugMode()));
		uint32 CurrentProxySetHash = HashCombine(GetTypeHash(VRDiagnosticMode), GSPCVR::CacheKey(*SceneView)); // Changing alpha culling requires a fresh sort.
        if(GSPCVR::IsActive(*SceneView))
        {
            CurrentProxySetHash=HashCombine(CurrentProxySetHash,GetTypeHash(Parameters.ViewportRect.Min));
            CurrentProxySetHash=HashCombine(CurrentProxySetHash,GetTypeHash(Parameters.ViewportRect.Max));
            CurrentProxySetHash=HashCombine(CurrentProxySetHash,GetTypeHash(ColorTexture->Desc.Extent));
            // A union LOD selection depends on both eyes, even when this eye is stationary.
            const FSceneView *CacheLeft=nullptr,*CacheRight=nullptr;
            if((!bPCInputCacheFix || bAllNanite) && GSPCVR::StereoPair(*SceneView,CacheLeft,CacheRight))
            {
                const FMatrix LeftVP=CacheLeft->ViewMatrices.GetWorldToView()*CacheLeft->ViewMatrices.GetViewToClipNoAA();
                const FMatrix RightVP=CacheRight->ViewMatrices.GetWorldToView()*CacheRight->ViewMatrices.GetViewToClipNoAA();
                CurrentProxySetHash=HashCombine(CurrentProxySetHash,FCrc::MemCrc32(&LeftVP,sizeof(LeftVP)));
                CurrentProxySetHash=HashCombine(CurrentProxySetHash,FCrc::MemCrc32(&RightVP,sizeof(RightVP)));
            }
        }
        CurrentProxySetHash=HashCombine(CurrentProxySetHash,GetTypeHash(bPCInputCacheFix));
		for (const FProxyRenderInfo& Info : VisibleProxies)
		{
			CurrentProxySetHash = HashCombine(CurrentProxySetHash, PointerHash(Info.Proxy));
            if(bPCVREyeCache)
            {
                // A per-proxy "last eye" cache alone cannot detect transforms
                // already updated by the other eye. Hash view-independent state.
                CurrentProxySetHash=HashCombine(CurrentProxySetHash,FCrc::MemCrc32(&Info.LocalToWorld,sizeof(Info.LocalToWorld)));
                CurrentProxySetHash=HashCombine(CurrentProxySetHash,GetTypeHash(Info.Proxy->GetOpacityScale()));
                CurrentProxySetHash=HashCombine(CurrentProxySetHash,GetTypeHash(Info.Proxy->GetEffectiveSplatScale()));
                CurrentProxySetHash=HashCombine(CurrentProxySetHash,GetTypeHash(Info.Proxy->GetEffectiveLODErrorThreshold()));
                CurrentProxySetHash=HashCombine(CurrentProxySetHash,GetTypeHash(CurrentDebugForceLODLevel));
            }
            if(bPCInputCacheFix)
            {
                const auto* R=Info.Proxy->GetGPUResources();
                CurrentProxySetHash=HashCombine(CurrentProxySetHash,GetTypeHash(Info.Proxy->GetSHOrder()));
                CurrentProxySetHash=HashCombine(CurrentProxySetHash,FCrc::MemCrc32(&Info.LocalToWorld,sizeof(Info.LocalToWorld)));
                CurrentProxySetHash=HashCombine(CurrentProxySetHash,GetTypeHash(Info.Proxy->GetOpacityScale()));
                CurrentProxySetHash=HashCombine(CurrentProxySetHash,GetTypeHash(Info.Proxy->GetEffectiveSplatScale()));
                if(R)
                {
                    CurrentProxySetHash=HashCombine(CurrentProxySetHash,PointerHash(R->PackedSplatBuffer.GetReference()));
                    CurrentProxySetHash=HashCombine(CurrentProxySetHash,PointerHash(R->Float32PositionBuffer.GetReference()));
                    CurrentProxySetHash=HashCombine(CurrentProxySetHash,PointerHash(R->SHBuffer.GetReference()));
                    CurrentProxySetHash=HashCombine(CurrentProxySetHash,GetTypeHash(R->ShouldUseFloat32Positions()));
                    CurrentProxySetHash=HashCombine(CurrentProxySetHash,GetTypeHash(R->ShouldUseHighPrecisionColorOpacity()));
                    CurrentProxySetHash=HashCombine(CurrentProxySetHash,GetTypeHash(R->GetSHBands()));
                    if(VRDiagnosticMode!=3)
                        CurrentProxySetHash=HashCombine(CurrentProxySetHash,FGaussianSplatRenderer::GetVisibilityEffectsHash_RenderThread(R));
                }
            }
			if (Info.Proxy)
			{
				// Stable binding/field revisions are lighting-only inputs. A new field request
				// invalidates ViewData once, but never geometry/culling/sort. The actual one-shot
				// compute dispatch happens inside the RDG raster pass where RHICmdList is valid.
				// Stable binding data is consumed while CalcViewData builds each splat's
				// diagnostic payload. A completed binding upload must therefore invalidate
				// ViewData once, but it must NOT invalidate geometry, culling, or sort.
				// Treat the binding revision like lighting-only data so the existing
				// LightingRefresh path reruns CalcViewData without recreating SceneProxy.
				CurrentLightingHash = HashCombine(
					CurrentLightingHash,
					GetTypeHash(Info.Proxy->GetStableBindingRevision_RenderThread()));
				CurrentLightingHash = HashCombine(
					CurrentLightingHash,
					GetTypeHash(Info.Proxy->GetStableFieldRequestRevision_RenderThread()));
				CurrentLightingHash = HashCombine(
					CurrentLightingHash,
					GetTypeHash(Info.Proxy->GetStableFieldRevision_RenderThread()));

				const int32 StableDebugModeForHash = FGaussianSplatRenderer::GetStableShadowDebugMode();
				const bool bStableLightingConsumesLightingData =
					StableDebugModeForHash == 72 || StableDebugModeForHash == 75;
				if (Info.Proxy->GetLightingData().bReceiveLighting != 0 || bStableLightingConsumesLightingData || (bPCInputCacheFix && VRDiagnosticMode!=3))
				{
					// v2746: stable lighting diagnostics consume StableLightingParams even when
					// ReceiveUnrealLighting is disabled. Always hash the payload in those modes
					// so slider edits trigger LightingRefresh on a static camera.
					const FGaussianSplatLightingData& Lighting = Info.Proxy->GetLightingData();
					CurrentLightingHash = HashCombine(CurrentLightingHash, FCrc::MemCrc32(&Lighting, sizeof(Lighting)));
				}
			}
		}

        if(bPCInputCacheFix && VRDiagnosticMode!=3)
            CurrentLightingHash=HashCombine(CurrentLightingHash,FGaussianSplatRenderer::GetViewDataEffectsHash_RenderThread());
        const uint32 RawLightingHash=CurrentLightingHash;
        // The trained-SH baseline returns before all of these lighting inputs.
        if(bBaselineCacheIsolation) CurrentLightingHash=GetTypeHash(VRDiagnosticMode);

		// Geometry/view cache validity is intentionally independent from lighting.
		// Lighting changes affect ViewData color but not projected depth ordering.
		// Exact view/proxy/LOD comparisons below already invalidate the cache whenever
		// depth order can change. Re-sorting an unchanged camera every frame only
		// burns GPU time, so the legacy force flag is retained for asset compatibility
		// but no longer disables the correctness-preserving cache.
		const bool bCacheMissing = !ActiveAccumulator->bHasCachedSortData;
		const bool bSplatCountChanged =
			ActiveAccumulator->CachedTotalSplatCount != TotalSplatCount;
		const bool bProxySetChanged =
			ActiveAccumulator->CachedProxySetHash != CurrentProxySetHash;
		// Editor and CineCamera view matrices can drift by tiny floating-point
		// amounts while visually stationary. Exact equality repeatedly invalidated
		// sorting for tens of millions of source splats.
		const bool bViewProjectionChanged =
			!ActiveAccumulator->CachedViewProjectionMatrix.Equals(CurrentVP, bPCVREyeCache ? 0.0f : 1.0e-3f);
		bool bCanReuseGeometryAndSort =
			!bCacheMissing && !bSplatCountChanged && !bProxySetChanged && !bViewProjectionChanged;
		bool bPerProxyStateChanged = false;

		if (bCanReuseGeometryAndSort)
		{
			for (const FProxyRenderInfo& Info : VisibleProxies)
			{
				FGaussianSplatGPUResources* GPUResources = Info.Proxy->GetGPUResources();
				float ProxyErrorThreshold = FMath::Max(0.001f, Info.Proxy->GetEffectiveLODErrorThreshold());
				if (!GPUResources || !GPUResources->bHasCachedSortData ||
					(!bPCVREyeCache && !GPUResources->CachedViewProjectionMatrix.Equals(CurrentVP, 1.0e-3f)) ||
					!GPUResources->CachedLocalToWorld.Equals(Info.LocalToWorld, 1.0e-5f) ||
					GPUResources->CachedOpacityScale != Info.Proxy->GetOpacityScale() ||
					GPUResources->CachedSplatScale != Info.Proxy->GetEffectiveSplatScale() ||
					GPUResources->CachedErrorThreshold != ProxyErrorThreshold ||
					GPUResources->CachedDebugMode != CurrentDebugMode ||
					GPUResources->CachedDebugForceLODLevel != CurrentDebugForceLODLevel)
				{
					bPerProxyStateChanged = true;
					bCanReuseGeometryAndSort = false;
					break;
				}
			}
		}

		const bool bLightingChanged =
			!ActiveAccumulator->bHasCachedSortData ||
			ActiveAccumulator->CachedLightingHash != CurrentLightingHash;
        // Validity can change with effect/opacity state; compacted membership must be rebuilt.
        if(GSPCVR::PreSort(*SceneView) && bLightingChanged && !bPCInputCacheFix) bCanReuseGeometryAndSort=false;
		const bool bCanReuseEverything = bCanReuseGeometryAndSort && !bLightingChanged;
        if(FGaussianSplatRenderer::IsVRDiagnosticLogging_RenderThread())
        {
            struct FReasonWindow
            {
                double Last=0;uint32 RawHash=0;bool HasRaw=false;
                uint32 Frames=0,Reuse=0,Refresh=0,Missing=0,Count=0,Proxy=0,Camera=0,State=0,Lighting=0,RawChanges=0;
            };
            static TMap<int32,FReasonWindow> Windows;
            auto& W=Windows.FindOrAdd(int32(SceneView->StereoPass)*4+VRDiagnosticMode);
            ++W.Frames;W.Reuse+=bCanReuseEverything;W.Refresh+=bCanReuseGeometryAndSort&&!bCanReuseEverything;
            W.Missing+=bCacheMissing;W.Count+=bSplatCountChanged;W.Proxy+=bProxySetChanged;
            W.Camera+=bViewProjectionChanged;W.State+=bPerProxyStateChanged;W.Lighting+=bLightingChanged;
            W.RawChanges+=W.HasRaw && W.RawHash!=RawLightingHash;W.RawHash=RawLightingHash;W.HasRaw=true;
            const double Now=FPlatformTime::Seconds();
            if(Now-W.Last>=1.0)
            {
                UE_LOG(LogTemp,Display,TEXT("[GSPC][CacheReasons] Mode=%d Eye=%d Frames=%u FullReuse=%u LightingRefresh=%u Missing=%u Count=%u ProxyInputs=%u Camera=%u ProxyState=%u Lighting=%u RawLightingChanges=%u CacheFix=%d BaselineIsolation=%d SourceQuadTrim=%d Rect=%dx%d"),
                    VRDiagnosticMode,int32(SceneView->StereoPass),W.Frames,W.Reuse,W.Refresh,W.Missing,W.Count,W.Proxy,W.Camera,W.State,W.Lighting,W.RawChanges,
                    bPCInputCacheFix?1:0,bBaselineCacheIsolation?1:0,(VRDiagnosticMode==0 && GSPCVR::IsActive(*SceneView) && FGaussianSplatRenderer::IsSourceQuadTrimEnabled_RenderThread())?1:0,Parameters.ViewportRect.Width(),Parameters.ViewportRect.Height());
                const uint32 LastHash=W.RawHash;W=FReasonWindow();W.Last=Now;W.RawHash=LastHash;W.HasRaw=true;
            }
        }
		// Report plan decisions, not GPU completion time. Sample sparsely to keep
		// 360/MRQ render logging from becoming its own frame-time cost.
		if (CVarGSPanoramaPerfTrace.GetValueOnRenderThread() > 0)
		{
			static uint64 PerfViewSerial = 0;
			const uint64 Serial = ++PerfViewSerial;
			if (Serial <= 4ull || (Serial % 64ull) == 0ull)
			{
				UE_LOG(LogTemp, Display,
					TEXT("[GSPerf][Workset] View=%llu Frame=%llu SourceSplats=%u Proxies=%d AllCompaction=%d ReuseSort=%d ReuseViewData=%d Reasons=CacheMissing:%d SplatCount:%d ProxySet:%d Camera:%d ProxyState:%d Lighting:%d"),
					static_cast<unsigned long long>(Serial),
					static_cast<unsigned long long>(GFrameNumberRenderThread),
					TotalSplatCount, VisibleProxies.Num(), bAllNanite ? 1 : 0,
					bCanReuseGeometryAndSort ? 1 : 0,
					bCanReuseEverything ? 1 : 0,
					bCacheMissing ? 1 : 0, bSplatCountChanged ? 1 : 0,
					bProxySetChanged ? 1 : 0, bViewProjectionChanged ? 1 : 0,
					bPerProxyStateChanged ? 1 : 0, bLightingChanged ? 1 : 0);
			}
		}

        // Logging is opt-in and rate-limited. Source count != compacted GPU draw count.
        if (FGaussianSplatRenderer::IsVRDiagnosticLogging_RenderThread())
        {
            static TMap<int32,double> LastEyeLogTimes;
            double& LastLogTime=LastEyeLogTimes.FindOrAdd(int32(SceneView->StereoPass)*4+VRDiagnosticMode);
            const double Now = FPlatformTime::Seconds();
            if (Now - LastLogTime >= 1.0)
            {
                LastLogTime = Now;
                const FVector Origin = SceneView->ViewMatrices.GetViewOrigin();
                UE_LOG(LogTemp, Display, TEXT("[GSVRDiag][Workset] Mode=%d Frame=%llu Eye=%d Rect=%dx%d Origin=(%.2f,%.2f,%.2f) SourceSplats=%u Proxies=%d Indirect=%d RTs=%d Profile=%d LOD=%.6f Effects=%d ModeChangesLOD=0 ViewDataBaseline=%d"),
                    VRDiagnosticMode, static_cast<uint64>(GFrameNumberRenderThread), static_cast<int32>(SceneView->StereoPass),
                    Parameters.ViewportRect.Width(), Parameters.ViewportRect.Height(), Origin.X, Origin.Y, Origin.Z,
                    TotalSplatCount, VisibleProxies.Num(), (bAllNanite || VRDiagnosticMode == 3) ? 1 : 0,
                    VRDiagnosticMode == 3 ? 1 : (VRDiagnosticMode == 2 ? 2 : (bPCVRLeanOutputs ? (IntermediateFogDepthTexture ? 3 : 2) : (IntermediateFogDepthTexture ? 5 : 4))),
                    VisibleProxies[0].Proxy->GetRenderProfileValue(), VisibleProxies[0].Proxy->GetEffectiveLODErrorThreshold(),
                    VisibleProxies[0].Proxy->GetLightingData().RelightDetailParams2.Z >= 0.5f ? 1 : 0, VRDiagnosticMode == 3 ? 1 : 0);
                UE_LOG(LogTemp,Display,TEXT("[GSPCVR][FogRT] Packed=%d Format=%s BytesPerPixel=%d Lean=%d Frame=%llu"),
                    FogPackingMode,FogPackingMode==2?TEXT("R16F"):(bPackedFog?TEXT("RG16F"):TEXT("RGBA16F")),FogPackingMode==2?2:(bPackedFog?4:8),bPCVRLeanOutputs?1:0,uint64(GFrameNumberRenderThread));
                UE_LOG(LogTemp,Display,TEXT("[GSPCVR][Plan] Eye=%d Enabled=%d PreSort=%d StereoEyeCache=%d SharedTargets=%d PackedRoute=%d Frame=%llu GeometrySortReuse=%d FullReuse=%d RasterClip=%d RasterClipMinArea=%.1f"),
                    int32(SceneView->StereoPass),GSPCVR::IsActive(*SceneView)?1:0,GSPCVR::PreSort(*SceneView)?1:0,
                    bPCVREyeCache?1:0,bReuseTargets?1:0,bAllNanite?1:0,uint64(GFrameNumberRenderThread),bCanReuseGeometryAndSort?1:0,bCanReuseEverything?1:0,(VRDiagnosticMode==3 && GSPCVR::RasterClip(*SceneView))?1:0,GSPCVR::RasterClipMinArea());
            }
        }
		// Grab index buffer from the first proxy (all proxies use identical quad geometry)
		FBufferRHIRef SharedIndexBuffer;
		if (VisibleProxies.Num() > 0)
		{
			FGaussianSplatGPUResources* FirstRes = VisibleProxies[0].Proxy->GetGPUResources();
			SharedIndexBuffer = FirstRes ? FirstRes->IndexBuffer : FBufferRHIRef();
		}

		// v2785 B: Snow is a second 3DGS shell, not a mutation of the base pass.
		// Keep it out of all diagnostic views so existing Stable Self-Cast debug
		// channels preserve their exact v2783 semantics.
		bSnowOverlayEnabled =
			VRDiagnosticMode == 0 && DebugMode == 0 &&
			FGaussianSplatRenderer::GetStableShadowDebugMode() == 0 &&
			VisibleProxies.Num() > 0 &&
			VisibleProxies[0].Proxy &&
			VisibleProxies[0].Proxy->GetLightingPreviewMode() == 0 &&
			FGaussianSplatRenderer::IsDirectSnowOverlayEnabled_RenderThread();
		if (bSnowOverlayEnabled)
		{
			SnowIntermediateTexture = GraphBuilder.CreateTexture(
				IntermediateDesc, TEXT("GaussianSplatSnowOverlayRT"));
			SnowIntermediateDepthTexture = GraphBuilder.CreateTexture(
				IntermediateDepthDesc, TEXT("GaussianSplatSnowOverlayDepthRT"));
		}

		FGaussianGlobalAccumulator* RawAccumulator = ActiveAccumulator.Get();

		// Correctness Fix 1.1: user render-budget truncation is disabled until
		// camera-priority cluster selection exists. Shader guards use the physical
		// accumulator capacity only.

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GaussianSplat_RenderToIntermediate"),
			Pass1Parameters,
			ERDGPassFlags::Raster,
			[SceneView, VisibleProxies, TotalSplatCount, VRDiagnosticMode, bBaselineCacheIsolation, bCanReuseGeometryAndSort, bCanReuseEverything, bAllNanite, RawAccumulator,
			 SharedIndexBuffer, CurrentVP, CurrentDebugMode,
			 CurrentDebugForceLODLevel, DebugMode, CurrentLightingHash, CurrentProxySetHash,
			 bForceFullSortRequested, bCacheMissing, bSplatCountChanged, bProxySetChanged,
			 bViewProjectionChanged, bPerProxyStateChanged, bTraceThisGPUView, GPUTraceViewSerial, bPCVRLeanOutputs, DepthTexture](FRHICommandListImmediate& RHICmdList)
			{
				if (!SceneView) return;
				SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatRendering_Global);

				// SAFETY CHECK: Re-validate all proxies before rendering.
				// Proxies may have been destroyed between when we built VisibleProxies
				// and when this lambda executes (RDG deferred execution).
				// We need to rebuild the list with only valid proxies.
				TArray<FProxyRenderInfo> ValidProxies;
				ValidProxies.Reserve(VisibleProxies.Num());
				uint32 NewTotalSplatCount = 0;

				for (const auto& Info : VisibleProxies)
				{
					// Check if proxy is still valid (not destroyed or pending destruction)
					if (Info.Proxy && Info.Proxy->IsValidForRendering())
					{
						const int32 ProxyCount = Info.Proxy->GetSplatCount();
						if (ProxyCount <= 0 ||
							uint64(NewTotalSplatCount) + uint64(ProxyCount) > uint64(MAX_int32))
						{
							UE_LOG(LogTemp, Error,
								TEXT("[GSPanoramaGPU][Abort] Proxy count changed before RDG dispatch: Proxy=%p Count=%d Base=%u"),
								static_cast<const void*>(Info.Proxy), ProxyCount, NewTotalSplatCount);
							return;
						}
						FProxyRenderInfo ValidInfo = Info;
						ValidInfo.GlobalBaseOffset = NewTotalSplatCount;
						NewTotalSplatCount += static_cast<uint32>(ProxyCount);
						ValidProxies.Add(ValidInfo);
					}
				}

				// If no valid proxies remain, skip rendering entirely
				if (ValidProxies.Num() == 0 || NewTotalSplatCount == 0)
				{
					return;
				}

				// Phase 2: build the requested stable proxy-surface visibility field once.
				// Inputs are only proxy surface + fixed light-space depth. No camera/view/sort
				// values participate. Because the request revision was included in the lighting
				// hash above, a successful build in this pass forces CalcViewData to refresh
				// without touching the cached geometry or sort order.
				if(!bBaselineCacheIsolation) for (const auto& Info : ValidProxies)
				{
					Info.Proxy->EnsureStableProxyField_RenderThread(RHICmdList, Info.LocalToWorld);
				}

				int32 LightingPreviewMode = 0;
				bool bValidProxyLightingMaskAsMain = false;
				for (const auto& Info : ValidProxies)
				{
					LightingPreviewMode = FMath::Max(LightingPreviewMode, Info.Proxy->GetLightingPreviewMode());
				}
				// Stable-shadow diagnostics are a pixel-only render-thread override. The
				// component/SceneProxy keeps its authored LightingPreviewMode, so changing
				// this selector cannot invalidate proxy lifetime, LOD, or sort caches.
				LightingPreviewMode = FGaussianSplatRenderer::ResolveStableShadowDebugLightingPreviewMode(LightingPreviewMode);
				// The abandoned point/voxel/triangle shadow-transfer experiment has
				// been removed from the render path. Final remains lit without a custom
				// shadow multiplier while the mesh-derived screen-space matte replaces it.
				bValidProxyLightingMaskAsMain = false;
				const bool bNeedLegacyLightingMaskForDraw = !bValidProxyLightingMaskAsMain ||
					(LightingPreviewMode > 0 && LightingPreviewMode < 4);
				float ProxyLightingIntensityScale = 1.0f;
				if (bValidProxyLightingMaskAsMain)
				{
					ProxyLightingIntensityScale = 0.0f;
					if (ValidProxies.Num() > 0)
					{
						const FGaussianSplatLightingData& LightingData = ValidProxies[0].Proxy->GetLightingData();
						for (uint32 LightIndex = 0;
							LightIndex < LightingData.LightCount && LightIndex < FGaussianSplatLightingData::MaxLights;
							++LightIndex)
						{
							const bool bIsDirectional = LightingData.LightDirectionType[LightIndex].W < 0.5f;
							if (bIsDirectional)
							{
								ProxyLightingIntensityScale = FMath::Clamp(LightingData.LightColorIntensity[LightIndex].W, 0.0f, 1.0f);
								break;
							}
						}
					}
				}

				// Invalidate cache skip if the proxy list changed (some proxies were destroyed)
				// This ensures we don't use stale cached data when the scene has changed
				bool bCanReuseGeometryAndSortAdjusted = bCanReuseGeometryAndSort;
				bool bCanReuseEverythingAdjusted = bCanReuseEverything;
				if (ValidProxies.Num() != VisibleProxies.Num() || NewTotalSplatCount != TotalSplatCount)
				{
					bCanReuseGeometryAndSortAdjusted = false;
					bCanReuseEverythingAdjusted = false;
					// Also invalidate the global accumulator cache since proxy set changed.
					RawAccumulator->bHasCachedSortData = false;
				}
				const bool bLightingRefreshOnly =
					bCanReuseGeometryAndSortAdjusted && !bCanReuseEverythingAdjusted;

				if (CVarSortCacheDiagnostics.GetValueOnRenderThread() != 0)
				{
					const uint8 CacheDecision = bCanReuseEverythingAdjusted ? 2u : (bLightingRefreshOnly ? 1u : 0u);
					if (RawAccumulator->LastCacheDecision != CacheDecision)
					{
						const TCHAR* DecisionName = CacheDecision == 2u ? TEXT("FullReuse") :
							(CacheDecision == 1u ? TEXT("LightingRefresh") : TEXT("FullRebuild"));
						UE_LOG(LogTemp, Log, TEXT("LCCCinematicGS SortCache: %s (proxies=%d, sourceSplats=%u)"),
							DecisionName, ValidProxies.Num(), NewTotalSplatCount);
						if (CacheDecision == 0u)
						{
							UE_LOG(LogTemp, Log,
								TEXT("LCCCinematicGS SortInvalidation: cacheMissing=%d count=%d proxySet=%d view=%d proxyState=%d viewState=%p family=%p origin=(%.1f,%.1f,%.1f)"),
								bCacheMissing ? 1 : 0,
								bSplatCountChanged ? 1 : 0,
								bProxySetChanged ? 1 : 0,
								bViewProjectionChanged ? 1 : 0,
								bPerProxyStateChanged ? 1 : 0,
								SceneView->State,
								SceneView->Family,
								SceneView->ViewLocation.X,
								SceneView->ViewLocation.Y,
								SceneView->ViewLocation.Z);
						}
						RawAccumulator->LastCacheDecision = CacheDecision;
					}
					static bool bLoggedSuppressedLegacyForce = false;
					if (bForceFullSortRequested && !bLoggedSuppressedLegacyForce)
					{
						UE_LOG(LogTemp, Log,
							TEXT("LCCCinematicGS SortCache: legacy ForceFullSortEveryFrame is suppressed; exact state invalidation remains active."));
						bLoggedSuppressedLegacyForce = true;
					}
				}

				// Initialize color textures (deferred init) - only for valid proxies
				for (const auto& Info : ValidProxies)
				{
					Info.Proxy->TryInitializeColorTexture(RHICmdList);
				}

				// Ensure global buffers are large enough for all splats
				RawAccumulator->ResizeIfNeeded(RHICmdList, NewTotalSplatCount);
				if (RawAccumulator->AllocatedCount < NewTotalSplatCount ||
					!RawAccumulator->GlobalViewDataBuffer.IsValid() ||
					!RawAccumulator->GlobalViewDataBufferUAV.IsValid() ||
					!RawAccumulator->GlobalViewDataBufferSRV.IsValid() ||
					!RawAccumulator->GlobalSortDistanceBuffer.IsValid() ||
					!RawAccumulator->GlobalSortDistanceBufferUAV.IsValid() ||
					!RawAccumulator->GlobalSortKeysBuffer.IsValid() ||
					!RawAccumulator->GlobalSortKeysBufferUAV.IsValid())
				{
					UE_LOG(LogTemp, Error,
						TEXT("[GSPanoramaGPU][Abort] Global buffers not ready: View=%p Requested=%u Capacity=%u Generation=%llu"),
						static_cast<const void*>(SceneView), NewTotalSplatCount,
						RawAccumulator->AllocatedCount,
						static_cast<unsigned long long>(RawAccumulator->BufferGeneration));
					return; // Missing data is an error, never a successful partial render.
				}
				for (const auto& Info : ValidProxies)
				{
					const int32 Count = Info.Proxy->GetSplatCount();
					const FGaussianSplatGPUResources* Res = Info.Proxy->GetGPUResources();
					const uint64 End = uint64(Info.GlobalBaseOffset) + uint64(Count);
					if (!Res || !Res->IsValid() || Count <= 0 ||
						Res->GetSplatCount() < Count ||
						End > uint64(RawAccumulator->AllocatedCount))
					{
						UE_LOG(LogTemp, Error,
							TEXT("[GSPanoramaGPU][Abort] Invalid proxy GPU range: Proxy=%p Count=%d GPUCount=%d Offset=%u End=%llu Capacity=%u ResourceOK=%d"),
							static_cast<const void*>(Info.Proxy), Count, Res ? Res->GetSplatCount() : 0,
							Info.GlobalBaseOffset, static_cast<unsigned long long>(End),
							RawAccumulator->AllocatedCount, Res && Res->IsValid() ? 1 : 0);
						return;
					}
					// The packed source is byte-addressed at 16 bytes per splat;
					// Float32 positions (when selected) occupy 12 bytes per splat.
					// A non-null SRV alone does not guarantee its range is sufficient.
					const uint64 ExpectedPackedBytes = uint64(Count) * 16ull;
					const uint64 ExpectedFloat32Bytes = uint64(Count) * sizeof(FVector3f);
					const uint64 ActualPackedBytes = Res->PackedSplatBuffer.IsValid()
						? uint64(Res->PackedSplatBuffer->GetSize()) : 0ull;
					const uint64 ActualFloat32Bytes = Res->Float32PositionBuffer.IsValid()
						? uint64(Res->Float32PositionBuffer->GetSize()) : 0ull;
					if (ActualPackedBytes < ExpectedPackedBytes ||
						(Res->ShouldUseFloat32Positions() && ActualFloat32Bytes < ExpectedFloat32Bytes))
					{
						UE_LOG(LogTemp, Error,
							TEXT("[GSPanoramaGPU][Abort] Truncated GS input buffer: Proxy=%p Splats=%d Packed=%llu/%llu Float32=%llu/%llu UseFloat32=%d"),
							static_cast<const void*>(Info.Proxy), Count,
							static_cast<unsigned long long>(ActualPackedBytes),
							static_cast<unsigned long long>(ExpectedPackedBytes),
							static_cast<unsigned long long>(ActualFloat32Bytes),
							static_cast<unsigned long long>(ExpectedFloat32Bytes),
							Res->ShouldUseFloat32Positions() ? 1 : 0);
						return;
					}
				}
				if (bTraceThisGPUView)
				{
					UE_LOG(LogTemp, Display,
						TEXT("[GSPanoramaGPU][RT][Workset] Serial=%u Scene=%p View=%p Frame=%llu Proxies=%d SourceSplats=%u Capacity=%u Gen=%llu ViewStride=%u ViewBytes=%llu SortBytesEach=%llu ViewRebuild=%d LightingRefresh=%d"),
						GPUTraceViewSerial, static_cast<const void*>(SceneView->Family->Scene),
						static_cast<const void*>(SceneView),
						static_cast<unsigned long long>(GFrameNumberRenderThread),
						ValidProxies.Num(), NewTotalSplatCount, RawAccumulator->AllocatedCount,
						static_cast<unsigned long long>(RawAccumulator->BufferGeneration),
						uint32(sizeof(FGaussianSplatViewData)),
						static_cast<unsigned long long>(uint64(RawAccumulator->AllocatedCount) * sizeof(FGaussianSplatViewData)),
						static_cast<unsigned long long>(uint64(RawAccumulator->AllocatedCount) * sizeof(uint32)),
						bCanReuseGeometryAndSort ? 0 : 1, bCanReuseEverything ? 0 : 1);
				}

				// Use the complete allocated working set as the sole capacity guard.
				// ResizeIfNeeded now allocates for every source splat, so this cannot
				// truncate visible geometry by asset/index order.
				const uint32 SafeRenderBudget = RawAccumulator->AllocatedCount;
				if (SafeRenderBudget == 0u)
				{
					return;
				}

				// Re-check if all valid proxies support Nanite compaction
				bool bAllValidNanite = true;
				for (const auto& Info : ValidProxies)
				{
					FGaussianSplatGPUResources* GPUResources = Info.Proxy->GetGPUResources();
					if (!GPUResources || !GPUResources->bEnableNanite || !GPUResources->bHasClusterData || !GPUResources->bSupportsCompaction)
					{
						bAllValidNanite = false;
						break;
					}
				}

                if(!bAllValidNanite && GSPCVR::IsActive(*SceneView))
                    for(const auto& I:ValidProxies)
                    { const auto* R=I.Proxy->GetGPUResources(); if(R && R->bEnableNanite && R->bHasClusterData && R->bSupportsCompaction){bAllValidNanite=true;break;} }
				// Cap to MAX_PROXY_COUNT
				if ((uint32)ValidProxies.Num() > FGaussianGlobalAccumulator::MAX_PROXY_COUNT)
				{
					bAllValidNanite = false;
				}

				if (bAllValidNanite)
				{
                    RawAccumulator->bPCVRPreSortCompacted=false;
					if (bTraceThisGPUView)
					{
						UE_LOG(LogTemp, Display,
							TEXT("[GSPanoramaGPU][RT][Path] Serial=%u Type=Compaction MaxSourceSplats=%u Capacity=%u Gen=%llu"),
							GPUTraceViewSerial, NewTotalSplatCount, RawAccumulator->AllocatedCount,
							static_cast<unsigned long long>(RawAccumulator->BufferGeneration));
					}
					//==================================================
					// GLOBAL + COMPACTION PATH
					// All proxies are Nanite-enabled: GPU compaction
					// reduces working set from TotalSplatCount ??TotalVisible
					// (~140x reduction at LOD5 for a 719K-splat tile).
					//==================================================

					// Ensure fixed-size prefix-sum buffers exist (allocated once)
					RawAccumulator->EnsureCompactionBuffersAllocated(RHICmdList);

					if (!bCanReuseGeometryAndSortAdjusted)
					{
						// --------------------------------------------------
						// Phase 0: Per-proxy culling + compaction + indirect args.
						// Process every visible proxy; no index/order-based budget skip.
						// --------------------------------------------------
						const int32 NumProcessedProxies = ValidProxies.Num();

						for (const auto& Info : ValidProxies)
						{
							FGaussianSplatGPUResources* GPUResources = Info.Proxy->GetGPUResources();
							if (!GPUResources) continue;  // Extra safety check
							int32 SplatCount = Info.Proxy->GetSplatCount();
							int32 OriginalSplatCount = SplatCount - GPUResources->LODSplatCount;

							// Cluster culling ??fills ClusterVisibilityBitmap
                            if(GPUResources->bHasClusterData)
							FGaussianSplatRenderer::DispatchClusterCulling(
								RHICmdList, *SceneView, GPUResources,
								Info.LocalToWorld, Info.Proxy->GetEffectiveLODErrorThreshold(), Info.bUseLODRendering);

							// Compact ??fills CompactedSplatIndices + VisibleSplatCountBuffer
                            if(GSPCVR::IsActive(*SceneView))
                                FGaussianSplatRenderer::DispatchPCVRSourceCompact(RHICmdList,GPUResources,RawAccumulator,SplatCount,OriginalSplatCount,Info.bUseLODRendering);
                            else
							FGaussianSplatRenderer::DispatchCompactSplats(
								RHICmdList, GPUResources,
								SplatCount, OriginalSplatCount, Info.bUseLODRendering);

							// PrepareIndirectArgs ??fills IndirectDispatchArgsBuffer for CalcViewData
							FGaussianSplatRenderer::DispatchPrepareIndirectArgs(RHICmdList, GPUResources);

						}

						// --------------------------------------------------
						// Phase 1: Gather visible counts + GPU prefix sum
						// Only gather from proxies that were actually processed
						// --------------------------------------------------
						for (int32 i = 0; i < NumProcessedProxies; i++)
						{
							FGaussianSplatGPUResources* GPUResources = ValidProxies[i].Proxy->GetGPUResources();
							if (!GPUResources) continue;  // Extra safety check
							FGaussianSplatRenderer::DispatchGatherVisibleCount(
								RHICmdList, GPUResources, RawAccumulator, i);
						}

						// Single 1-thread dispatch: computes prefix sums + writes all indirect args
						FGaussianSplatRenderer::DispatchPrefixSumVisibleCounts(
							RHICmdList, RawAccumulator, NumProcessedProxies, SafeRenderBudget);

						// --------------------------------------------------
						// Phase 2: Per-proxy CalcViewData ??global buffer
						// (indirect dispatch, only visible splats per proxy)
						// Only process proxies that went through culling/compaction
						// --------------------------------------------------
						for (int32 i = 0; i < NumProcessedProxies; i++)
						{
							const auto& Info = ValidProxies[i];
							FGaussianSplatGPUResources* GPUResources = Info.Proxy->GetGPUResources();
							if (!GPUResources) continue;  // Extra safety check
							int32 SplatCount = Info.Proxy->GetSplatCount();
							int32 OriginalSplatCount = SplatCount - GPUResources->LODSplatCount;

							FGaussianSplatRenderer::DispatchCalcViewDataCompactedGlobal(
								RHICmdList, *SceneView, GPUResources,
								Info.LocalToWorld,
								SplatCount,
								OriginalSplatCount,
								Info.Proxy->GetSHOrder(),
								Info.Proxy->GetOpacityScale(),
								Info.Proxy->GetEffectiveSplatScale(),
								i,
								RawAccumulator,
								SafeRenderBudget,
								&Info.Proxy->GetLightingData(),
								&Info.Proxy->GetCapturedDepthShadowData());
						}

						// --------------------------------------------------
						// Phase 3: Single global CalcDistances + RadixSort
						// (all indirect ??count driven by GPU prefix sum)
						// --------------------------------------------------
						FGaussianSplatRenderer::DispatchCalcDistancesGlobalIndirect(RHICmdList, RawAccumulator);
						FGaussianSplatRenderer::DispatchRadixSortGlobalIndirect(RHICmdList, RawAccumulator);

						// Update caches ??only for processed proxies
						RawAccumulator->bHasCachedSortData = true;
						RawAccumulator->CachedTotalSplatCount = NewTotalSplatCount;
						RawAccumulator->CachedProxySetHash = CurrentProxySetHash;
						RawAccumulator->CachedLightingHash = CurrentLightingHash;
						RawAccumulator->CachedViewProjectionMatrix = CurrentVP;

						for (int32 i = 0; i < ValidProxies.Num(); i++)
						{
							FGaussianSplatGPUResources* GPUResources = ValidProxies[i].Proxy->GetGPUResources();
							if (!GPUResources) continue;

							const auto& Info = ValidProxies[i];
							GPUResources->CachedViewProjectionMatrix = CurrentVP;
							GPUResources->CachedLocalToWorld = Info.LocalToWorld;
							GPUResources->CachedOpacityScale = Info.Proxy->GetOpacityScale();
							GPUResources->CachedSplatScale = Info.Proxy->GetEffectiveSplatScale();
							GPUResources->CachedErrorThreshold = FMath::Max(0.001f, Info.Proxy->GetEffectiveLODErrorThreshold());
							GPUResources->CachedDebugMode = CurrentDebugMode;
							GPUResources->CachedDebugForceLODLevel = CurrentDebugForceLODLevel;
							GPUResources->bHasCachedSortData = true;
						}
					}
					else if (bLightingRefreshOnly)
					{
						// Geometry, compaction, visible counts and depth order are unchanged.
						// Refresh only the per-splat ViewData (including relit color) and
						// reuse the existing global radix-sort key order.
						for (int32 i = 0; i < ValidProxies.Num(); ++i)
						{
							const auto& Info = ValidProxies[i];
							FGaussianSplatGPUResources* GPUResources = Info.Proxy->GetGPUResources();
							if (!GPUResources) continue;
							const int32 SplatCount = Info.Proxy->GetSplatCount();
							const int32 OriginalSplatCount = SplatCount - GPUResources->LODSplatCount;

							FGaussianSplatRenderer::DispatchCalcViewDataCompactedGlobal(
								RHICmdList, *SceneView, GPUResources,
								Info.LocalToWorld,
								SplatCount,
								OriginalSplatCount,
								Info.Proxy->GetSHOrder(),
								Info.Proxy->GetOpacityScale(),
								Info.Proxy->GetEffectiveSplatScale(),
								i,
								RawAccumulator,
								SafeRenderBudget,
								&Info.Proxy->GetLightingData(),
								&Info.Proxy->GetCapturedDepthShadowData());
						}
						RawAccumulator->CachedLightingHash = CurrentLightingHash;
					}

                    // PrefixSumVisibleCounts already writes the exact count, sort dispatch,
                    // and five-word draw arguments. CalcDistances uses compacted ViewData
                    // indices and radix sort preserves those indices. Reuse these resources
                    // in the existing PCVR final-color/tile path after either geometry or
                    // lighting-only update; no extra full-source scan is needed.
                    if(GSPCVR::IsActive(*SceneView) && FParse::Param(FCommandLine::Get(),TEXT("GSPCVRLODTest"))){
                        RawAccumulator->bPCVRPreSortCompacted=true;
                        if(GFrameNumberRenderThread%120==0)UE_LOG(LogTemp,Display,TEXT("[GSPCVRLOD][Bridge] ExactPrefixCount=1 CompactedViewIndices=1 SortParamsReady=1 DrawArgsWords=5 ExtraFullScan=0 Capacity=%u"),RawAccumulator->AllocatedCount);
                    }
					// Single draw call ??instance count from GlobalDrawIndirectArgsBuffer
					FGaussianSplatRenderer::DrawSplatsGlobalIndirect(
						RHICmdList, *SceneView, RawAccumulator, SharedIndexBuffer, DebugMode, LightingPreviewMode,
						ValidProxies.Num() > 0 ? ValidProxies[0].Proxy->GetRenderProfileValue() : 0,
						bValidProxyLightingMaskAsMain,
						ProxyLightingIntensityScale,
						ValidProxies.Num() > 0 ? ValidProxies[0].Proxy->GetLightingData().StableLightingParams4.X : 0.0f,
						ValidProxies.Num() > 0 ? ValidProxies[0].Proxy->GetLightingData().RegionalColorGradeParams0 : FVector4f(1, 1, 1, 0),
						ValidProxies.Num() > 0 ? ValidProxies[0].Proxy->GetLightingData().RegionalColorGradeParams1 : FVector4f(1, 1, 1, 0),
						ValidProxies.Num() > 0 ? ValidProxies[0].Proxy->GetLightingData().RelightToneParams.W : 1.0f, VRDiagnosticMode, bPCVRLeanOutputs, DepthTexture ? DepthTexture->GetRHI() : nullptr);
				}
				else
				{
					//==================================================
					// NON-COMPACTION GLOBAL PATH (fallback)
					// Not all proxies are Nanite-enabled.
					// Sorts all NewTotalSplatCount splats (no compaction benefit).
					// Still provides correct cross-tile alpha blending.
					//==================================================

					// Correctness path: process and sort the complete visible proxy set.
					const uint32 CappedTotalSplatCount = NewTotalSplatCount;
					if (bTraceThisGPUView)
					{
						UE_LOG(LogTemp, Display,
							TEXT("[GSPanoramaGPU][RT][Path] Serial=%u Type=NonCompaction SourceSplats=%u DistanceGroups=%u Capacity=%u Gen=%llu"),
							GPUTraceViewSerial, CappedTotalSplatCount,
							FMath::DivideAndRoundUp(CappedTotalSplatCount, 256u),
							RawAccumulator->AllocatedCount,
							static_cast<unsigned long long>(RawAccumulator->BufferGeneration));
					}

					if (!bCanReuseGeometryAndSortAdjusted)
					{
						// --------------------------------------------------
						// Phase 1: Per-proxy ClusterCulling + CalcViewData
						// --------------------------------------------------
						for (const auto& Info : ValidProxies)
						{
							FGaussianSplatGPUResources* GPUResources = Info.Proxy->GetGPUResources();
							if (!GPUResources) continue;  // Extra safety check

							// Cluster culling for Nanite-enabled proxies
							if (GPUResources->bEnableNanite && GPUResources->bHasClusterData)
							{
								FGaussianSplatRenderer::DispatchClusterCulling(
									RHICmdList, *SceneView, GPUResources,
									Info.LocalToWorld, Info.Proxy->GetEffectiveLODErrorThreshold(), Info.bUseLODRendering);
							}

							// CalcViewData ??writes to GlobalViewDataBuffer at GlobalBaseOffset
							if (bTraceThisGPUView)
							{
								const int32 Count = Info.Proxy->GetSplatCount();
								UE_LOG(LogTemp, Display,
									TEXT("[GSPanoramaGPU][RT][SubmitViewData] Serial=%u Proxy=%p Offset=%u Count=%d End=%llu Groups=%u ResourceSplats=%d Float32=%d PackedBytes=%llu Float32Bytes=%llu SHBytes=%llu Gen=%llu"),
									GPUTraceViewSerial, static_cast<const void*>(Info.Proxy), Info.GlobalBaseOffset, Count,
									static_cast<unsigned long long>(uint64(Info.GlobalBaseOffset) + uint64(Count)),
									FMath::DivideAndRoundUp(static_cast<uint32>(Count), 256u), GPUResources->GetSplatCount(),
									GPUResources->ShouldUseFloat32Positions() ? 1 : 0,
									static_cast<unsigned long long>(GPUResources->PackedSplatBuffer.IsValid() ? GPUResources->PackedSplatBuffer->GetSize() : 0),
									static_cast<unsigned long long>(GPUResources->Float32PositionBuffer.IsValid() ? GPUResources->Float32PositionBuffer->GetSize() : 0),
									static_cast<unsigned long long>(GPUResources->SHBuffer.IsValid() ? GPUResources->SHBuffer->GetSize() : 0),
									static_cast<unsigned long long>(RawAccumulator->BufferGeneration));
							}
							FGaussianSplatRenderer::DispatchCalcViewDataGlobal(
								RHICmdList, *SceneView, GPUResources,
								Info.LocalToWorld,
								Info.Proxy->GetSplatCount(),
								Info.Proxy->GetSHOrder(),
								Info.Proxy->GetOpacityScale(),
								Info.Proxy->GetEffectiveSplatScale(),
								Info.bUseLODRendering,
								Info.GlobalBaseOffset,
								RawAccumulator,
								&Info.Proxy->GetLightingData(),
								&Info.Proxy->GetCapturedDepthShadowData());
						}

						// v2886: read back a tiny, stratified sample of the completed ViewData;
						// diagnostic only, never modifies the actual global splat buffer.
						if (CVarGSPanoramaShadowStats.GetValueOnRenderThread() > 0)
						{
							FGaussianSplatRenderer::TraceShadowViewDataSamples(
								RHICmdList, RawAccumulator, CappedTotalSplatCount,
								GFrameNumberRenderThread, FGaussianSplatRenderer::GetStableShadowDebugMode());
						}

						// --------------------------------------------------
						// Phase 2: Single global CalcDistances + RadixSort
						// --------------------------------------------------
						if (bTraceThisGPUView)
						{
							UE_LOG(LogTemp, Display,
								TEXT("[GSPanoramaGPU][RT][SubmitDistances] Serial=%u InputCount=%u InputCapacity=%u Groups=%u Gen=%llu"),
								GPUTraceViewSerial, CappedTotalSplatCount, RawAccumulator->AllocatedCount,
								FMath::DivideAndRoundUp(CappedTotalSplatCount, 256u),
								static_cast<unsigned long long>(RawAccumulator->BufferGeneration));
						}
                        RawAccumulator->bPCVRPreSortCompacted=false;
                        if (!GSPCVR::PreSort(*SceneView))
						FGaussianSplatRenderer::DispatchCalcDistancesGlobal(RHICmdList, RawAccumulator, (int32)CappedTotalSplatCount);
						if (bTraceThisGPUView)
						{
							UE_LOG(LogTemp, Display,
								TEXT("[GSPanoramaGPU][RT][SubmitSort] Serial=%u InputCount=%u Gen=%llu"),
								GPUTraceViewSerial, CappedTotalSplatCount,
								static_cast<unsigned long long>(RawAccumulator->BufferGeneration));
						}
                        if(GSPCVR::PreSort(*SceneView))
                            FGaussianSplatRenderer::DispatchPCVRStableCompact(RHICmdList,RawAccumulator,(int32)CappedTotalSplatCount,*SceneView);
                        else
						FGaussianSplatRenderer::DispatchRadixSortGlobal(RHICmdList, RawAccumulator, (int32)CappedTotalSplatCount);

						// Update caches
						RawAccumulator->bHasCachedSortData = true;
						RawAccumulator->CachedTotalSplatCount = NewTotalSplatCount;
						RawAccumulator->CachedProxySetHash = CurrentProxySetHash;
						RawAccumulator->CachedLightingHash = CurrentLightingHash;
						RawAccumulator->CachedViewProjectionMatrix = CurrentVP;

						for (const auto& Info : ValidProxies)
						{
							FGaussianSplatGPUResources* GPUResources = Info.Proxy->GetGPUResources();
							if (!GPUResources) continue;  // Extra safety check
							GPUResources->CachedViewProjectionMatrix = CurrentVP;
							GPUResources->CachedLocalToWorld = Info.LocalToWorld;
							GPUResources->CachedOpacityScale = Info.Proxy->GetOpacityScale();
							GPUResources->CachedSplatScale = Info.Proxy->GetEffectiveSplatScale();

							GPUResources->CachedErrorThreshold = FMath::Max(0.001f, Info.Proxy->GetEffectiveLODErrorThreshold());
							GPUResources->CachedDebugMode = CurrentDebugMode;
							GPUResources->CachedDebugForceLODLevel = CurrentDebugForceLODLevel;
							GPUResources->bHasCachedSortData = true;
						}
					}
					else if (bLightingRefreshOnly)
					{
						// Non-compaction fallback still has stable source indices while the
						// camera/geometry is unchanged. Rebuild ViewData color only and retain
						// the cached depth sort.
						for (const auto& Info : ValidProxies)
						{
							FGaussianSplatGPUResources* GPUResources = Info.Proxy->GetGPUResources();
							if (!GPUResources) continue;
							FGaussianSplatRenderer::DispatchCalcViewDataGlobal(
								RHICmdList, *SceneView, GPUResources,
								Info.LocalToWorld,
								Info.Proxy->GetSplatCount(),
								Info.Proxy->GetSHOrder(),
								Info.Proxy->GetOpacityScale(),
								Info.Proxy->GetEffectiveSplatScale(),
								Info.bUseLODRendering,
								Info.GlobalBaseOffset,
								RawAccumulator,
								&Info.Proxy->GetLightingData(),
								&Info.Proxy->GetCapturedDepthShadowData());
						}
						if (CVarGSPanoramaShadowStats.GetValueOnRenderThread() > 0)
						{
							FGaussianSplatRenderer::TraceShadowViewDataSamples(
								RHICmdList, RawAccumulator, CappedTotalSplatCount,
								GFrameNumberRenderThread, FGaussianSplatRenderer::GetStableShadowDebugMode());
						}
						RawAccumulator->CachedLightingHash = CurrentLightingHash;
					}

					// Single draw call for the complete visible proxy set
					FGaussianSplatRenderer::DrawSplatsGlobal(
						RHICmdList, *SceneView, RawAccumulator,
						SharedIndexBuffer, (int32)CappedTotalSplatCount, DebugMode, LightingPreviewMode,
						ValidProxies.Num() > 0 ? ValidProxies[0].Proxy->GetRenderProfileValue() : 0,
						bValidProxyLightingMaskAsMain,
						ProxyLightingIntensityScale,
						ValidProxies.Num() > 0 ? ValidProxies[0].Proxy->GetLightingData().StableLightingParams4.X : 0.0f,
						ValidProxies.Num() > 0 ? ValidProxies[0].Proxy->GetLightingData().RegionalColorGradeParams0 : FVector4f(1, 1, 1, 0),
						ValidProxies.Num() > 0 ? ValidProxies[0].Proxy->GetLightingData().RegionalColorGradeParams1 : FVector4f(1, 1, 1, 0),
						ValidProxies.Num() > 0 ? ValidProxies[0].Proxy->GetLightingData().RelightToneParams.W : 1.0f, VRDiagnosticMode, bPCVRLeanOutputs, DepthTexture ? DepthTexture->GetRHI() : nullptr);
				}
			}
		);


		EnqueuePCVRTileDiagnostics(GraphBuilder,RawAccumulator,IntermediateTexture);

		// Pass 1B: render accumulated snow into its own color/depth pair. It uses
		// the same globally sorted splat order but reads SnowAmount + stable-proxy
		// normal from the 80-byte ViewData payload written by CalcViewData.
		if (bSnowOverlayEnabled && SnowIntermediateTexture && SnowIntermediateDepthTexture)
		{
			FGaussianSnowOverlayPassParameters* SnowPassParameters =
				GraphBuilder.AllocParameters<FGaussianSnowOverlayPassParameters>();
			SnowPassParameters->BaseDependency = IntermediateTexture;
			SnowPassParameters->RenderTargets[0] = FRenderTargetBinding(
				SnowIntermediateTexture, ERenderTargetLoadAction::EClear);
			SnowPassParameters->RenderTargets[1] = FRenderTargetBinding(
				SnowIntermediateDepthTexture, ERenderTargetLoadAction::EClear);
			if (DepthTexture)
			{
				SnowPassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
					DepthTexture,
					ERenderTargetLoadAction::ELoad,
					ERenderTargetLoadAction::ELoad,
					FExclusiveDepthStencil::DepthRead_StencilRead);
			}

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("GaussianSplat_RenderSnowOverlay"),
				SnowPassParameters,
				ERDGPassFlags::Raster,
				[SceneView, RawAccumulator, SharedIndexBuffer, TotalSplatCount, bAllNanite]
				(FRHICommandListImmediate& RHICmdList)
				{
					if (!SceneView || !RawAccumulator || !SharedIndexBuffer.IsValid())
					{
						return;
					}
					const int32 SafeTotalSplatCount = FMath::Min<int32>(
						static_cast<int32>(TotalSplatCount),
						static_cast<int32>(RawAccumulator->AllocatedCount));
					if (bAllNanite)
					{
						FGaussianSplatRenderer::DrawSnowSplatsGlobalIndirect(
							RHICmdList, *SceneView, RawAccumulator, SharedIndexBuffer);
					}
					else
					{
						FGaussianSplatRenderer::DrawSnowSplatsGlobal(
							RHICmdList, *SceneView, RawAccumulator,
							SharedIndexBuffer, SafeTotalSplatCount);
					}
				});
		}

		// Final nonlinear daylight controls are evaluated once on the fully accumulated
		// GS HDR image. The first visible proxy defines the shared look, matching the
		// global path's existing first-proxy render-profile convention.
		FVector4f CompositeLookParams0 = FVector4f::Zero();
		FVector4f CompositeLookParams1 = FVector4f::Zero();
		FVector4f CompositeLookParams2 = FVector4f::Zero();
		FVector4f CompositeLookParams3 = FVector4f::Zero();
		FVector4f CompositeLookParams4 = FVector4f::Zero();
		FVector4f CompositeBloomParams = FVector4f::Zero();
		FVector4f CompositeColorBloomParams = FVector4f(1.0f, 0.25f, 0.20f, 0.15f);
		FVector4f CompositeBloomTint = FVector4f(1, 1, 1, 0);
		float CompositeBloomSoftKnee = 0.25f;
		if (VisibleProxies.Num() > 0 && VisibleProxies[0].Proxy)
		{
			const FGaussianSplatLightingData& Lighting = VisibleProxies[0].Proxy->GetLightingData();
			const int32 StableShadowDebugMode = FGaussianSplatRenderer::GetStableShadowDebugMode();
			const bool bCompositeInsetOnlyDebug =
				StableShadowDebugMode >= 66 && StableShadowDebugMode <= 68;
			const bool bLiteralLightingPreview =
				VisibleProxies[0].Proxy->GetLightingPreviewMode() != 0 ||
				(StableShadowDebugMode != 0 && !bCompositeInsetOnlyDebug);
			// One record per MRQ render frame rather than per pane. CPU-side settings
			// are NOT a measurement of the actual output luminance/tonemapped PNG.
			if (CVarGSPanoramaShadowStats.GetValueOnRenderThread() > 0)
			{
				static uint64 LastShadowLookTraceFrame = MAX_uint64;
				if (LastShadowLookTraceFrame != GFrameNumberRenderThread)
				{
					LastShadowLookTraceFrame = GFrameNumberRenderThread;
					UE_LOG(LogTemp, Display,
						TEXT("[GSPanoramaShadowValues][RT][Look] Frame=%llu DebugMode=%d ViewPreExposure=%.6f ")
						TEXT("CastDensity=%.5f CastSoftness=%.5f CastMinimum=%.5f ClosureDensity=%.5f ")
						TEXT("GSExposureEV=%.5f SourceBrightness=%.5f BloomStrength=%.5f ")
						TEXT("MaskProbe=%d Note=PostTonemapStillUEControlled"),
						static_cast<unsigned long long>(GFrameNumberRenderThread), StableShadowDebugMode,
						(SceneView->bIsViewInfo
							? static_cast<const FViewInfo&>(*SceneView).PreExposure : 1.0f),
						Lighting.StableLightingParams0.X,
						Lighting.StableLightingParams1.Z, Lighting.StableLightingParams1.W,
						Lighting.StableLightingParams0.Y, Lighting.RelightToneParams.X,
						Lighting.RelightDetailParams2.W, Lighting.CompositeBloomParams.Y,
						CVarGSPanoramaShadowMaskProbe.GetValueOnRenderThread());
				}
			}
			// Params are packed to match GaussianSplatComposite.usf exactly.
			// P0: look strength, green boost, global gamma exponent, reserved.
			CompositeLookParams0 = FVector4f(
				bLiteralLightingPreview ? 0.0f : Lighting.RelightLookParams.Y,
				Lighting.RelightLookParams.Z,
				Lighting.RelightLookParams.W,
				Lighting.RelightDetailParams2.W);
			// P1: global saturation, highlight compression, bright-green compression, fine local contrast.
			CompositeLookParams1 = FVector4f(
				Lighting.RelightToneParams.Z,
				Lighting.RelightDetailParams.Y,
				Lighting.RelightDetailParams.Z,
				Lighting.RelightDetailParams.W);
			// P2: pre-gamma exposure EV, bloom colour-highlight sensitivity,
			// shadow saturation retention, green saturation recovery.
			CompositeLookParams2 = FVector4f(
				Lighting.RelightToneParams.X,
				Lighting.RelightToneParams.Y,
				1.0f, // v2782: shadow color retention is applied per-splat from Stable cast*closure, not luminance.
				Lighting.RelightHighlightParams.Y);
			// P3: highlight start, green luminance reduction, log-detail clamp, coarse local contrast.
			CompositeLookParams3 = FVector4f(
				Lighting.RelightHighlightParams.X,
				Lighting.RelightHighlightParams.Z,
				0.12f,
				Lighting.RelightHighlightParams.W);
			CompositeLookParams4 = Lighting.RelightSharpenParams;
			CompositeBloomParams = Lighting.CompositeBloomParams;
			CompositeColorBloomParams = Lighting.CompositeColorBloomParams;
			CompositeBloomTint = Lighting.RelightSunTint;
			CompositeBloomTint.W = Lighting.AtmosphereHighlightParams.X;
			CompositeBloomSoftKnee = Lighting.RelightDetailParams2.Y;

			// RAW/unlit is controlled only by the explicit master-effects flag
			// packed in RelightDetailParams2.Z. TargetLookStrength may
			// legitimately be zero while lighting/shadows remain enabled.
			const bool bRawUnlitMode = Lighting.RelightDetailParams2.Z < 0.5f;
			if (bRawUnlitMode)
			{
				CompositeLookParams0 = FVector4f(0.0f, 0.0f, 1.0f, 0.0f);
				CompositeLookParams1 = FVector4f(1.0f, 0.0f, 0.0f, 0.0f);
				CompositeLookParams2 = FVector4f(0.0f, 0.0f, 1.0f, 0.0f);
				CompositeLookParams3 = FVector4f(0.72f, 0.0f, 0.12f, 0.0f);
				// W=12 is the internal RAW marker. The view extension uses it
				// to defer this composite until after UE's tonemapper.
				CompositeLookParams4 = FVector4f(0.0f, 1.0f, 1.0f, 12.0f);
				CompositeBloomParams = FVector4f::Zero();
				CompositeColorBloomParams = FVector4f::Zero();
				CompositeBloomSoftKnee = 0.0f;
			}

			// Adaptive EWA changes the spatial frequency content of the accumulated GS image.
			// Reusing a look tuned for the broader Legacy footprint can expose individual splat
			// boundaries. Preserve the authored values, but apply a render-time safety multiplier
			// only while the experimental footprint is active.
			if (const FGaussianSplatGPUResources* FirstGPUResources = VisibleProxies[0].Proxy->GetGPUResources())
			{
				if (FirstGPUResources->RuntimeFootprintMode == 1u)
				{
					const float AdaptiveLookSafety = FMath::Clamp(FirstGPUResources->FootprintParams3.Y, 0.0f, 1.0f);
					CompositeLookParams1.W *= AdaptiveLookSafety; // fine local contrast
					CompositeLookParams3.W *= AdaptiveLookSafety; // coarse local contrast
					CompositeLookParams4.X *= AdaptiveLookSafety; // final sharpen
				}
			}
			if (bLiteralLightingPreview)
			{
				CompositeLookParams0.X = 0.0f; // disable nonlinear color grade
				CompositeLookParams1.W = 0.0f; // disable fine local contrast
				CompositeLookParams3.W = 0.0f; // disable coarse local contrast
				CompositeLookParams4.X = 0.0f; // disable final sharpen
				CompositeLookParams4.W = 0.0f; // force final diagnostic mode
				// Stable debug values are literal data. Bloom must not turn a depth/mask
				// diagnostic into a beauty effect or hide black/white thresholds.
				if (StableShadowDebugMode != 0 && !bCompositeInsetOnlyDebug)
				{
					CompositeBloomParams = FVector4f::Zero();
					CompositeColorBloomParams = FVector4f::Zero();
					CompositeBloomSoftKnee = 0.0f;
				}
			}
		}

		// Pass 2: Composite the linear HDR intermediate onto pre-tonemap SceneColor
		// Use FGaussianCompositePassParameters to declare IntermediateTexture as an RDG-tracked
		// shader resource input. This ensures RDG inserts the required RTV-to-SRV resource barrier
		// between Pass 1 (which writes IntermediateTexture as a render target) and this pass
		// (which reads it as a shader resource). Without this, the GPU may read stale/partial
		// data from IntermediateTexture, producing rectangular block artifacts.
		// Never clear the shared SceneColor. Debug visualization is restricted to
		// covered GS pixels; discarded pixels must preserve the existing UE scene.
		Ext->SetPendingComposite_RenderThread(
				&GraphBuilder, SceneView, IntermediateTexture, IntermediateDepthTexture, IntermediateNormalTexture,
				IntermediateFogDepthTexture,
				SnowIntermediateTexture, SnowIntermediateDepthTexture, SnowOcclusionDepthTexture, Parameters.ViewportRect,
				CompositeLookParams0, CompositeLookParams1, CompositeLookParams2,
				CompositeLookParams3, CompositeLookParams4,
				CompositeBloomParams, CompositeColorBloomParams,
				CompositeBloomTint, CompositeBloomSoftKnee, VRDiagnosticMode);
}

void FLCCCinematicGSModule::ShutdownModule()
{
    FWorldDelegates::OnWorldTickStart.Remove(VREffectsWorldTickHandle);
    VREffectsWorldTickHandle.Reset();
	// Remove deferred init delegate
	if (PostEngineInitDelegateHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitDelegateHandle);
		PostEngineInitDelegateHandle.Reset();
	}

	// Unregister post-opaque render delegate
	if (PostOpaqueRenderDelegateHandle.IsValid())
	{
		GetRendererModuleRef().RemovePostOpaqueRenderDelegate(PostOpaqueRenderDelegateHandle);
		PostOpaqueRenderDelegateHandle.Reset();
	}

	// Release global accumulator GPU buffers from the render thread
	if (GlobalAccumulator.IsValid())
	{
		FGaussianGlobalAccumulator* RawAccumulator = GlobalAccumulator.Release();
		ENQUEUE_RENDER_COMMAND(ReleaseGlobalAccumulator)(
			[RawAccumulator](FRHICommandListImmediate& RHICmdList)
			{
				RawAccumulator->Release();
				delete RawAccumulator;
			});
	}

    for(auto& Entry:PCVREyeAccumulators)
    {
        FGaussianGlobalAccumulator* A=Entry.Value.Release();
        ENQUEUE_RENDER_COMMAND(ReleasePCVREye)([A](FRHICommandListImmediate& Cmd){A->Release();delete A;});
    }
    PCVREyeAccumulators.Empty();
	// Clear the view extension
	ViewExtension.Reset();
}

FLCCCinematicGSModule& FLCCCinematicGSModule::Get()
{
	return FModuleManager::LoadModuleChecked<FLCCCinematicGSModule>("LCCCinematicGS");
}

bool FLCCCinematicGSModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded("LCCCinematicGS");
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FLCCCinematicGSModule, LCCCinematicGS)
