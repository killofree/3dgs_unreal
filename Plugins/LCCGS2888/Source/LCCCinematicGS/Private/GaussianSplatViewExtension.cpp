// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatViewExtension.h"
#include "GaussianPCVR.h"
#include "GaussianSplatShaders.h"
#include "RenderGraphUtils.h"
#include "GaussianSplatSceneProxy.h"
#include "GaussianSplatRenderer.h"
#include "GaussianWetMesh.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphBuilder.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "ScreenPass.h"
#include "SceneRendering.h"

// V13 VR Preview-only bloom ablation. The editor switches this on only inside PIE,
// and restores the original value at EndPIE. Does not change shadow or HDR grade.
static TAutoConsoleVariable<int32> CVarPCVRDisableAllBloom(
    TEXT("gs.PCVR.DisableAllBloom"), 0,
    TEXT("When enabled during VR Preview, suppress GS atmospheric bloom source and GS spatial bloom. UE post bloom is separately disabled by the preview editor."),
    ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> CVarPCVRBloomCache(TEXT("gs.PCVR.BloomExtractCache"),0,TEXT("Cache full precision per-pixel bloom extraction before the unchanged spatial 40 taps + core. PCVR only; 0 reference."),ECVF_RenderThreadSafe);
extern TAutoConsoleVariable<int32> CVarGSPanoramaGSCompositeMode;
extern TAutoConsoleVariable<float> CVarGSPanoramaGSToneMapEV;
extern TAutoConsoleVariable<int32> CVarGSPanoramaPerfTrace;
extern TAutoConsoleVariable<int32> CVarGSPanoramaSkipHDRClear;
extern TAutoConsoleVariable<int32> CVarGSPanoramaRainWetSafeClear;
extern TAutoConsoleVariable<int32> CVarGSPanoramaRainBloomHalfSamples;
extern TAutoConsoleVariable<int32> CVarGSPanoramaRainGPUDrainInterval;
extern TAutoConsoleVariable<int32> CVarGSPanoramaSnowGPUDrainInterval;

BEGIN_SHADER_PARAMETER_STRUCT(FGaussianPostGradePassParameters, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, IntermediateTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, IntermediateDepthTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SnowIntermediateTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SnowIntermediateDepthTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SnowOcclusionDepthTexture)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FGaussianBloomCompositePassParameters, )
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, RawFogCoverageTexture)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, BloomExtractTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GradedTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, IntermediateDepthTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, IntermediateFogDepthTexture)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FGaussianWetSurfacePassParameters, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GradedTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, IntermediateDepthTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, IntermediateNormalTexture)
	// RainSkyReflection v26: explicit RDG dependency on incoming Unreal SceneColor.
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FGaussianVRDiagnosticCompositePassParameters, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, IntermediateTexture)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

FGaussianSplatViewExtension* FGaussianSplatViewExtension::Instance = nullptr;
static TAtomic<bool> GGaussianRainNiagaraCompositeVisible(false);
// v2896: baked snow is rendered by the actor-owned SnowGaussianComponent, not by
// the legacy SnowIntermediate overlay pass. Keep an explicit cross-thread state so
// panorama back-pressure can run before Nanite/Lumen/GS work for each ViewFamily.
static TAtomic<bool> GGaussianBakedSnowGaussianVisible(false);

// High-resolution panorama residency guard helpers. These only change how many
// square panorama/offline subviews may remain queued on the GPU at once; they do
// not change splat count, render-target resolution, shading, weather, exposure,
// shadowing or any pixel math. Keep the historical >=256 square-view heuristic,
// but do not impose an upper bound so 4K/8K panorama presets remain protected.
static bool IsGaussianPanoramaLikeSquareSubview(const FIntRect& ViewRect)
{
	return ViewRect.Width() == ViewRect.Height() && ViewRect.Width() >= 256;
}

static int32 GetGaussianPanoramaEffectiveDrainInterval(int32 ConfiguredInterval, int32 SubviewWidth)
{
	if (ConfiguredInterval <= 0)
	{
		return 0;
	}

	// 2048x2048 and larger subviews can carry 16x+ the pixels of the original
	// 512x512 panorama face, so retire every completed face. 1024x1024 faces
	// are bounded to at most two queued faces. 512-class behavior is unchanged.
	if (SubviewWidth >= 2048)
	{
		return 1;
	}
	if (SubviewWidth >= 1024)
	{
		return FMath::Min(ConfiguredInterval, 2);
	}
	return ConfiguredInterval;
}

static int32 GetGaussianPanoramaHighResDrainInterval(int32 SubviewWidth)
{
	if (SubviewWidth >= 2048)
	{
		return 1;
	}
	if (SubviewWidth >= 1024)
	{
		return 2;
	}
	return 0;
}

FGaussianSplatViewExtension::FGaussianSplatViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
	Instance = this;
}

FGaussianSplatViewExtension::~FGaussianSplatViewExtension()
{
	if (Instance == this)
	{
		Instance = nullptr;
	}
}

FGaussianSplatViewExtension* FGaussianSplatViewExtension::Get()
{
	return Instance;
}

void FGaussianSplatViewExtension::SetRainNiagaraCompositeVisible(bool bEnabled)
{
	GGaussianRainNiagaraCompositeVisible.Store(bEnabled);
}

void FGaussianSplatViewExtension::SetBakedSnowGaussianVisible(bool bEnabled)
{
	GGaussianBakedSnowGaussianVisible.Store(bEnabled);
}

void FGaussianSplatViewExtension::GetRegisteredProxies(TArray<FGaussianSplatSceneProxy*>& OutProxies) const
{
	FScopeLock Lock(&ProxyLock);
	OutProxies = RegisteredProxies;
}

// Filter both activation contexts and render callbacks by their actual scene.
static bool HasGSProxyForScene(const FGaussianSplatViewExtension& Extension, const FSceneInterface* Scene)
{
    if (!Scene)
    {
        return false;
    }
    TArray<FGaussianSplatSceneProxy*> Proxies;
    Extension.GetRegisteredProxies(Proxies);
    for (const FGaussianSplatSceneProxy* Proxy : Proxies)
    {
        if (Proxy && &Proxy->GetScene() == Scene)
        {
            return true;
        }
    }
    return false;
}

bool FGaussianSplatViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
    if (Context.Scene) return HasGSProxyForScene(*this, Context.Scene);
    if (const UWorld* World = Context.GetWorld()) return HasGSProxyForScene(*this, World->Scene);
    // Some offscreen callers supply neither. Their view family is checked again.
    FScopeLock Lock(&ProxyLock);
    return RegisteredProxies.Num() > 0;
}

void FGaussianSplatViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
}

void FGaussianSplatViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
    if (!HasGSProxyForScene(*this, InViewFamily.Scene))
    {
        return;
    }
	// Weather Niagara only: remember the actual view Unreal is rendering. This does
	// not alter GS lighting/composite state and is ignored by every non-Rain feature.
	if (!InViewFamily.Scene)
	{
		return;
	}
	for (const FSceneView* View : InViewFamily.Views)
	{
		if (View && !View->bIsSceneCapture)
		{
			FScopeLock Lock(&ActiveViewLock);
			LatestRenderedViewLocations.FindOrAdd(InViewFamily.Scene) = View->ViewLocation;
			break;
		}
	}
}

bool FGaussianSplatViewExtension::GetLatestRenderedViewLocation(
	const FSceneInterface* Scene, FVector& OutLocation) const
{
	if (!Scene)
	{
		return false;
	}
	FScopeLock Lock(&ActiveViewLock);
	if (const FVector* Found = LatestRenderedViewLocations.Find(Scene))
	{
		OutLocation = *Found;
		return true;
	}
	return false;
}

void FGaussianSplatViewExtension::RegisterProxy(FGaussianSplatSceneProxy* Proxy)
{
	if (Proxy)
	{
		FScopeLock Lock(&ProxyLock);
		RegisteredProxies.AddUnique(Proxy);
	}
}

void FGaussianSplatViewExtension::UnregisterProxy(FGaussianSplatSceneProxy* Proxy)
{
	if (Proxy)
	{
		FScopeLock Lock(&ProxyLock);
		RegisteredProxies.Remove(Proxy);
	}
}

void FGaussianSplatViewExtension::PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView)
{
}

void FGaussianSplatViewExtension::PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
    if (!HasGSProxyForScene(*this, InViewFamily.Scene))
    {
        return;
    }
    GSPCVR::ObserveRuntimeFamily(InViewFamily);
	if (InViewFamily.Views.Num() == 0)
	{
		return;
	}

	const FSceneView* View = InViewFamily.Views[0];
	if (!View)
	{
		return;
	}

    // Helper captures never render GS (OnPostOpaque rejects bIsSceneCapture).
    // Do not classify their square RTs as memory-heavy panorama GS faces.
    if(GSPCVR::SkipCaptureDrain(InViewFamily))
    {
        static double LastCaptureSkipLog=-100.0;
        static uint32 CaptureSkipCount=0;
        ++CaptureSkipCount;
        const double Now=FPlatformTime::Seconds();
        if(FGaussianSplatRenderer::IsVRDiagnosticLogging_RenderThread() && Now-LastCaptureSkipLog>=1.0)
        {
            UE_LOG(LogTemp,Display,TEXT("[GSPCVR][CaptureDrainSkipped] Count=%u Views=%d Rect=%dx%d Capture=1 Offline=%d Realtime=%d MultipleFamily=%d AdditionalFamily=%d MainPanoramaGuardPreserved=1"),
                CaptureSkipCount,InViewFamily.Views.Num(),View->UnconstrainedViewRect.Width(),View->UnconstrainedViewRect.Height(),
                View->bIsOfflineRender?1:0,InViewFamily.bRealtimeUpdate?1:0,InViewFamily.bIsMultipleViewFamily?1:0,InViewFamily.bAdditionalViewFamily?1:0);
            CaptureSkipCount=0; LastCaptureSkipLog=Now;
        }
        return;
    }
    // HMD eyes can also be square: geometry alone must not identify offline views.
    if(GSPCVR::BlockOfflineDrain(InViewFamily)) return;
	const FIntRect ViewRect = View->UnconstrainedViewRect;
	if (!IsGaussianPanoramaLikeSquareSubview(ViewRect))
	{
		return;
	}

	// v2896 path preserved: baked-height snow is an actor-owned Gaussian shell,
	// so it must drain before the next panorama ViewFamily starts. The only
	// high-resolution change is that user-configured intervals >1 are reduced to
	// 2 at 1024-class faces and 1 at 2048+ faces. Default snow interval 1 is
	// therefore bit-for-bit scheduling-equivalent to the existing behavior.
	const int32 ConfiguredSnowGPUDrainInterval = FMath::Clamp(
		CVarGSPanoramaSnowGPUDrainInterval.GetValueOnRenderThread(), 0, 64);
	if (GGaussianBakedSnowGaussianVisible.Load())
	{
		const int32 SnowGPUDrainInterval = GetGaussianPanoramaEffectiveDrainInterval(
			ConfiguredSnowGPUDrainInterval, ViewRect.Width());
		if (SnowGPUDrainInterval <= 0)
		{
			return;
		}

		static uint64 SnowPanoramaPreViewSerial = 0;
		const uint64 Serial = ++SnowPanoramaPreViewSerial;
		if ((Serial % static_cast<uint64>(SnowGPUDrainInterval)) != 0ull)
		{
			return;
		}

		FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
		if (Serial <= 12ull || (Serial % 32ull) == 0ull)
		{
			UE_LOG(LogTemp, Display,
				TEXT("[GSSnowPanoramaPreViewDrain] Serial=%llu Frame=%llu Interval=%d View=%dx%d ActiveBakedSnow=1 Action=SubmitAndBlockUntilGPUIdle"),
				static_cast<unsigned long long>(Serial),
				static_cast<unsigned long long>(GFrameNumberRenderThread),
				SnowGPUDrainInterval, ViewRect.Width(), ViewRect.Height());
		}
		SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatSnowPanoramaPreViewGPUDrain);
		RHICmdList.SubmitAndBlockUntilGPUIdle();
		return;
	}

	// High-resolution clear/fog/general GS path. Do not touch realtime editor/PIE
	// views: this guard is limited to offline or multiple-view-family rendering,
	// which is the MRQ/panorama class of workload. Rain keeps its existing
	// post-composite guard below so it is never double-drained here.
	const bool bOfflineOrMultipleViewFamily =
		!InViewFamily.bRealtimeUpdate ||
		InViewFamily.bIsMultipleViewFamily ||
		InViewFamily.bAdditionalViewFamily;
	const bool bRainWetPassActive =
		FGaussianSplatRenderer::IsDirectWetSurfaceEnabled_RenderThread();
	const int32 HighResGPUDrainInterval = GetGaussianPanoramaHighResDrainInterval(ViewRect.Width());
	if (!bOfflineOrMultipleViewFamily || bRainWetPassActive || HighResGPUDrainInterval <= 0)
	{
		return;
	}

	static uint64 HighResPanoramaPreViewSerial = 0;
	const uint64 Serial = ++HighResPanoramaPreViewSerial;
	if ((Serial % static_cast<uint64>(HighResGPUDrainInterval)) != 0ull)
	{
		return;
	}

	FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
	if (Serial <= 12ull || (Serial % 32ull) == 0ull)
	{
		UE_LOG(LogTemp, Display,
			TEXT("[GSHighResPanoramaPreViewDrain] Serial=%llu Frame=%llu Interval=%d View=%dx%d OfflineOrMultiFamily=1 Action=SubmitAndBlockUntilGPUIdle"),
			static_cast<unsigned long long>(Serial),
			static_cast<unsigned long long>(GFrameNumberRenderThread),
			HighResGPUDrainInterval, ViewRect.Width(), ViewRect.Height());
	}
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatHighResPanoramaPreViewGPUDrain);
	RHICmdList.SubmitAndBlockUntilGPUIdle();
}

void FGaussianSplatViewExtension::PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
}

void FGaussianSplatViewExtension::PostRenderBasePassDeferred_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView, const FRenderTargetBindingSlots& RenderTargets, TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
	// Rendering is handled by PostOpaqueRenderDelegate in FLCCCinematicGSModule
	// This hook is too early in the pipeline (before lighting) for Gaussian splats
}

void FGaussianSplatViewExtension::PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs)
{
}

void FGaussianSplatViewExtension::SetPendingComposite_RenderThread(
	const FRDGBuilder* OwningGraphBuilder,
	const FSceneView* View,
	FRDGTexture* IntermediateTexture,
	FRDGTexture* IntermediateDepthTexture,
	FRDGTexture* IntermediateNormalTexture,
	FRDGTexture* IntermediateFogDepthTexture,
	FRDGTexture* SnowIntermediateTexture,
	FRDGTexture* SnowIntermediateDepthTexture,
	FRDGTexture* SnowOcclusionDepthTexture,
	const FIntRect& ViewRect,
	const FVector4f& CompositeLookParams0,
	const FVector4f& CompositeLookParams1,
	const FVector4f& CompositeLookParams2,
	const FVector4f& CompositeLookParams3,
	const FVector4f& CompositeLookParams4,
	const FVector4f& CompositeBloomParams,
	const FVector4f& CompositeColorBloomParams,
	const FVector4f& CompositeBloomTint,
	float CompositeBloomSoftKnee, int32 VRDiagnosticMode)
{
	if (View && IntermediateTexture)
	{
		FPendingComposite& Pending = PendingComposites.FindOrAdd(View);
		Pending.OwningGraphBuilder = OwningGraphBuilder;
		Pending.VRDiagnosticMode = VRDiagnosticMode;
		Pending.RenderFrameNumber = GFrameNumberRenderThread;
		Pending.IntermediateTexture = IntermediateTexture;
		Pending.IntermediateDepthTexture = IntermediateDepthTexture;
		Pending.IntermediateNormalTexture = IntermediateNormalTexture;
		Pending.IntermediateFogDepthTexture = IntermediateFogDepthTexture;
		Pending.SnowIntermediateTexture = SnowIntermediateTexture;
		Pending.SnowIntermediateDepthTexture = SnowIntermediateDepthTexture;
		Pending.SnowOcclusionDepthTexture = SnowOcclusionDepthTexture;
		Pending.ViewRect = ViewRect;
		Pending.CompositeLookParams0 = CompositeLookParams0;
		Pending.CompositeLookParams1 = CompositeLookParams1;
		Pending.CompositeLookParams2 = CompositeLookParams2;
		Pending.CompositeLookParams3 = CompositeLookParams3;
		Pending.CompositeLookParams4 = CompositeLookParams4;
		Pending.CompositeBloomParams = CompositeBloomParams;
		Pending.CompositeColorBloomParams = CompositeColorBloomParams;
		Pending.CompositeBloomTint = CompositeBloomTint;
		Pending.CompositeBloomSoftKnee = CompositeBloomSoftKnee;
		const int32 RequestedMode = FMath::Clamp(CVarGSPanoramaGSCompositeMode.GetValueOnRenderThread(), 0, 2);
		// Rain/snow Niagara can composite after DOF. Do not reorder those particles:
		// opt-in isolation is suspended and all original weather behavior is retained.
		Pending.PanoramaGSCompositeMode =
			(RequestedMode == 2 && GGaussianRainNiagaraCompositeVisible.Load())
				? 0u : static_cast<uint32>(RequestedMode);
		Pending.bAfterTonemap = (Pending.PanoramaGSCompositeMode == 2u);
	}
}

void FGaussianSplatViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView, FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
    if (!InView.Family || !HasGSProxyForScene(*this, InView.Family->Scene))
    {
        return;
    }
	// Original pass selection remains byte-for-byte equivalent in mode 0 and 1.
	const bool bWeatherNiagaraVisible = GGaussianRainNiagaraCompositeVisible.Load();
	const bool bIsolated =
		CVarGSPanoramaGSCompositeMode.GetValueOnAnyThread() == 2 && !bWeatherNiagaraVisible;
	const EPostProcessingPass TargetPass = bIsolated
		? EPostProcessingPass::Tonemap
		: (bWeatherNiagaraVisible ? EPostProcessingPass::AfterDOF : EPostProcessingPass::MotionBlur);
	if (Pass == TargetPass && (!bIsolated || bIsPassEnabled))
	{
		InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(
			this, bIsolated
				? &FGaussianSplatViewExtension::PostProcessPassAfterTonemap_RenderThread
				: &FGaussianSplatViewExtension::PostProcessPassBeforeTonemap_RenderThread));
	}
}

FScreenPassTexture FGaussianSplatViewExtension::PostProcessPassBeforeTonemap_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
{
	return CompositePending_RenderThread(GraphBuilder, View, Inputs, false);
}

FScreenPassTexture FGaussianSplatViewExtension::PostProcessPassAfterTonemap_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
{
	return CompositePending_RenderThread(GraphBuilder, View, Inputs, true);
}

FScreenPassTexture FGaussianSplatViewExtension::CompositePending_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs,
	bool bAfterTonemap)
{
	const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
	check(SceneColor.IsValid());

	const FPendingComposite* ExistingPending = PendingComposites.Find(&View);
	if (!ExistingPending || ExistingPending->bAfterTonemap != bAfterTonemap)
	{
		return SceneColor;
	}
	// RDG resources are owned by exactly one graph and one render frame. A
	// post-process subscription can change while weather particles activate or
	// deactivate, leaving a pending entry from a pass that was not called. Never
	// submit those stale pointers to a later graph.
	if (ExistingPending->OwningGraphBuilder != &GraphBuilder ||
		ExistingPending->RenderFrameNumber != GFrameNumberRenderThread)
	{
		PendingComposites.Remove(&View);
		return SceneColor;
	}

	FPendingComposite Pending;
	if (!PendingComposites.RemoveAndCopyValue(&View, Pending) || !Pending.IntermediateTexture)
	{
		return SceneColor;
	}

	// Report the active experimental route exactly once per change on the render
	// thread. This is useful in MRQ logs even when individual panorama panes differ.
	static uint32 LastReportedMode = ~0u;
	if (LastReportedMode != Pending.PanoramaGSCompositeMode)
	{
		LastReportedMode = Pending.PanoramaGSCompositeMode;
		UE_LOG(LogTemp, Display,
			TEXT("[GSPanoramaComposite] ActiveMode=%u AfterTonemap=%d Source=%dx%d"),
			Pending.PanoramaGSCompositeMode, Pending.bAfterTonemap ? 1 : 0,
			Pending.ViewRect.Width(), Pending.ViewRect.Height());
	}

	FScreenPassRenderTarget Output = Inputs.OverrideOutput;
	if (!Output.IsValid())
	{
		Output = FScreenPassRenderTarget(SceneColor.Texture, SceneColor.ViewRect, ERenderTargetLoadAction::ELoad);
	}
	else
	{
		AddDrawTexturePass(GraphBuilder, View, SceneColor, Output);
	}

    const auto* AblationVar=IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("gs.PCVR.Ablation"));
    const int32 Ablation=AblationVar?AblationVar->GetValueOnRenderThread():0;
    if (Pending.VRDiagnosticMode >= 2 || Ablation==3 || Ablation==4)
    {
        FGaussianVRDiagnosticCompositePassParameters* Pass = GraphBuilder.AllocParameters<FGaussianVRDiagnosticCompositePassParameters>();
        Pass->IntermediateTexture = Pending.IntermediateTexture;
        Pass->RenderTargets[0] = FRenderTargetBinding(Output.Texture, ERenderTargetLoadAction::ELoad);
        GraphBuilder.AddPass(RDG_EVENT_NAME("GS_VRDiag_MinimalComposite"), Pass, ERDGPassFlags::Raster,
            [Source=Pending.IntermediateTexture, SourceRect=Pending.ViewRect, OutputRect=Output.ViewRect, &View, bAfterTonemap]
            (FRHICommandListImmediate& RHICmdList)
            {
                FGaussianSplatRenderer::CompositeVRDiagnostic(RHICmdList, View, Source->GetRHI(), SourceRect, OutputRect, bAfterTonemap);
            });
        return FScreenPassTexture(Output.Texture, Output.ViewRect);
    }
	// Pass A: evaluate the complete GS look once and store the exact final,
	// un-pre-exposed scene-linear HDR result. Bloom must read this texture so
	// every highlight visible to the user is also visible to bloom extraction.
	FRDGTextureDesc GradedDesc = FRDGTextureDesc::Create2D(
		Pending.IntermediateTexture->Desc.Extent,
		PF_FloatRGBA,
		FClearValueBinding(FLinearColor::Transparent),
		TexCreate_RenderTargetable | TexCreate_ShaderResource);
	FRDGTexture* GradedTexture = GraphBuilder.CreateTexture(
		GradedDesc, TEXT("GaussianSplatPostGradeHDRRT"));
	// v2888: a full-surface fullscreen triangle writes EVERY pixel, including
	// transparent GS pixels (the grade PS explicitly returns float4(0) there).
	// On a full-extent view, a separate RGBA16F clear writes an entire additional
	// HDR image for no visible result. Keep the original clear for cropped MRQ
	// tiles/editor atlas views, whose pixels outside ViewRect are not overwritten.
	const bool bFullExtentView =
		CVarGSPanoramaSkipHDRClear.GetValueOnRenderThread() != 0 &&
		Pending.ViewRect.Min == FIntPoint::ZeroValue &&
		Pending.ViewRect.Max == GradedDesc.Extent;
	// Rain-only GPU safety state for this RDG graph. No other weather mode uses it.
	const bool bRainWetPassActive =
		FGaussianSplatRenderer::IsDirectWetSurfaceEnabled_RenderThread() &&
		Pending.IntermediateNormalTexture != nullptr;
	const bool bRainWetSafeClear = bRainWetPassActive &&
		(CVarGSPanoramaRainWetSafeClear.GetValueOnRenderThread() != 0);
	const bool bRainBloomHalfSamples = bRainWetPassActive &&
		(CVarGSPanoramaRainBloomHalfSamples.GetValueOnRenderThread() != 0);
	// v2893/v2894: MRQ panorama's internal faces are square (512x512 in the
	// current preset). Rain uses the proven interval-4 guard. Snow carries a
	// much larger GS working set, so v2895 uses interval 1 by default: every
	// completed snow panorama face is retired before another can accumulate.
	// Realtime/non-panorama views remain untouched.
	const int32 ConfiguredRainGPUDrainInterval = FMath::Clamp(
		CVarGSPanoramaRainGPUDrainInterval.GetValueOnRenderThread(), 0, 64);
	const int32 ConfiguredSnowGPUDrainInterval = FMath::Clamp(
		CVarGSPanoramaSnowGPUDrainInterval.GetValueOnRenderThread(), 0, 64);
	const bool bPanoramaLikeSquareSubview =
		IsGaussianPanoramaLikeSquareSubview(Pending.ViewRect) && !(GSPCVR::IsRealtimeHMD(View) && GSPCVR::IsActive(View));
	const int32 RainGPUDrainInterval = bPanoramaLikeSquareSubview
		? GetGaussianPanoramaEffectiveDrainInterval(ConfiguredRainGPUDrainInterval, Pending.ViewRect.Width())
		: ConfiguredRainGPUDrainInterval;
	const int32 SnowGPUDrainInterval = bPanoramaLikeSquareSubview
		? GetGaussianPanoramaEffectiveDrainInterval(ConfiguredSnowGPUDrainInterval, Pending.ViewRect.Width())
		: ConfiguredSnowGPUDrainInterval;
	const bool bSnowOverlayPassActive =
		Pending.SnowIntermediateTexture != nullptr &&
		Pending.SnowIntermediateDepthTexture != nullptr;
	const bool bRainPanoramaGPUDrain =
		bRainWetPassActive && bPanoramaLikeSquareSubview && RainGPUDrainInterval > 0;
	const bool bSnowPanoramaGPUDrain =
		bSnowOverlayPassActive && bPanoramaLikeSquareSubview && SnowGPUDrainInterval > 0;
	const ERenderTargetLoadAction GradeLoadAction = bFullExtentView
		? ERenderTargetLoadAction::ENoAction
		: ERenderTargetLoadAction::EClear;
	if (CVarGSPanoramaPerfTrace.GetValueOnRenderThread() > 0)
	{
		static uint64 TargetTraceSerial = 0;
		const uint64 Serial = ++TargetTraceSerial;
		if (Serial <= 4ull || (Serial % 64ull) == 0ull)
		{
			const bool bWet = bRainWetPassActive;
			const uint64 Pixels = uint64(GradedDesc.Extent.X) * uint64(GradedDesc.Extent.Y);
			UE_LOG(LogTemp, Display,
				TEXT("[GSPerf][Post] View=%llu Frame=%llu Mode=%u Extent=%dx%d FullExtent=%d SkipGradeClear=%d SkipWetClear=%d GradeBytes=%llu WetBytes=%llu Snow=%d Fog=%d Note=GPUEventTimingInProfileGPU"),
				static_cast<unsigned long long>(Serial),
				static_cast<unsigned long long>(GFrameNumberRenderThread),
				Pending.PanoramaGSCompositeMode, GradedDesc.Extent.X, GradedDesc.Extent.Y,
				bFullExtentView ? 1 : 0, bFullExtentView ? 1 : 0,
				bFullExtentView && bWet ? 1 : 0,
				static_cast<unsigned long long>(Pixels * 8ull),
				static_cast<unsigned long long>(bWet ? Pixels * 8ull : 0ull),
				Pending.SnowIntermediateTexture ? 1 : 0,
				Pending.IntermediateFogDepthTexture ? 1 : 0);
		}
	}

	FGaussianPostGradePassParameters* GradePassParameters =
		GraphBuilder.AllocParameters<FGaussianPostGradePassParameters>();
	GradePassParameters->IntermediateTexture = Pending.IntermediateTexture;
	GradePassParameters->IntermediateDepthTexture = Pending.IntermediateDepthTexture;
	GradePassParameters->SnowIntermediateTexture =
		Pending.SnowIntermediateTexture ? Pending.SnowIntermediateTexture : Pending.IntermediateTexture;
	GradePassParameters->SnowIntermediateDepthTexture =
		Pending.SnowIntermediateDepthTexture ? Pending.SnowIntermediateDepthTexture : Pending.IntermediateDepthTexture;
	GradePassParameters->SnowOcclusionDepthTexture =
		Pending.SnowOcclusionDepthTexture ? Pending.SnowOcclusionDepthTexture : Pending.IntermediateDepthTexture;
	GradePassParameters->RenderTargets[0] = FRenderTargetBinding(
		GradedTexture, GradeLoadAction);
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("GaussianSplat_PostGradeHDRSource"),
		GradePassParameters,
		ERDGPassFlags::Raster,
		[&View, GradePassParameters,
			 bSnowOverlayEnabled = Pending.SnowIntermediateTexture != nullptr && Pending.SnowIntermediateDepthTexture != nullptr,
			 ViewRect = Pending.ViewRect,
			 CompositeLookParams0 = Pending.CompositeLookParams0,
			 CompositeLookParams1 = Pending.CompositeLookParams1,
			 CompositeLookParams2 = Pending.CompositeLookParams2,
			 CompositeLookParams3 = Pending.CompositeLookParams3,
			 CompositeLookParams4 = Pending.CompositeLookParams4](FRHICommandListImmediate& RHICmdList)
			{
				FGaussianSplatRenderer::GradeToTexture(
					RHICmdList, View,
					GradePassParameters->IntermediateTexture->GetRHI(),
					GradePassParameters->IntermediateDepthTexture->GetRHI(),
					GradePassParameters->SnowIntermediateTexture->GetRHI(),
					GradePassParameters->SnowIntermediateDepthTexture->GetRHI(),
					GradePassParameters->SnowOcclusionDepthTexture->GetRHI(),
					bSnowOverlayEnabled,
					ViewRect, ViewRect,
					CompositeLookParams0, CompositeLookParams1, CompositeLookParams2,
					CompositeLookParams3, CompositeLookParams4);
			});

	// Pass B: the dedicated wet surface authority. It consumes only the GS-owned
	// post-grade HDR color, radial depth and stable per-splat normal targets.
	FRDGTexture* FinalHDRTexture = GradedTexture;
	if (bRainWetPassActive)
	{
		FRDGTexture* WetTexture = GraphBuilder.CreateTexture(GradedDesc, TEXT("GaussianSplatDedicatedWetHDRRT"));
		FGaussianWetSurfacePassParameters* WetParameters =
			GraphBuilder.AllocParameters<FGaussianWetSurfacePassParameters>();
		WetParameters->GradedTexture = GradedTexture;
		WetParameters->IntermediateDepthTexture = Pending.IntermediateDepthTexture;
		WetParameters->IntermediateNormalTexture = Pending.IntermediateNormalTexture;
		if (FGaussianSplatRenderer::GetWetReceiverTestMode() >= 3)
		{
			WetParameters->IntermediateNormalTexture = RenderGaussianWetMesh(GraphBuilder, View,
				Pending.ViewRect, GradedDesc.Extent, Pending.IntermediateDepthTexture);
		}
		WetParameters->SceneColorTexture = SceneColor.Texture;
		// The dedicated wet shader also returns zero for uncovered pixels. Only
		// skip the clear when the view owns every pixel of the output texture.
		WetParameters->RenderTargets[0] = FRenderTargetBinding(
			WetTexture, (bFullExtentView && !bRainWetSafeClear)
				? ERenderTargetLoadAction::ENoAction
				: ERenderTargetLoadAction::EClear);
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("GaussianSplat_DedicatedWetSurface"),
			WetParameters,
			ERDGPassFlags::Raster,
			[&View, WetParameters, ViewRect = Pending.ViewRect, SceneColorViewRect = SceneColor.ViewRect, bWetAfterTonemap = bAfterTonemap](FRHICommandListImmediate& RHICmdList)
			{
				// UE 5.8 stores current-frame PreExposure on FViewInfo. Keep the sky
				// reflection in the same scene-linear scale as the GS Wet source.
				const float ViewPreExposure = View.bIsViewInfo
					? FMath::Max(static_cast<const FViewInfo&>(View).PreExposure, 1.0e-4f)
					: 1.0f;
				const float SceneColorInvPreExposure = bWetAfterTonemap
					? 1.0f
					: (1.0f / ViewPreExposure);
				FGaussianSplatRenderer::ApplyWetSurfaceToTexture(
					RHICmdList, View,
					WetParameters->GradedTexture->GetRHI(),
					WetParameters->IntermediateDepthTexture->GetRHI(),
					WetParameters->IntermediateNormalTexture->GetRHI(),
					WetParameters->SceneColorTexture->GetRHI(),
					ViewRect, SceneColorViewRect, ViewRect, SceneColorInvPreExposure);
			});
		FinalHDRTexture = WetTexture;
	}

	// Final pass: extract spatial bloom from the post-grade/wet HDR source, apply view
	// pre-exposure exactly once, then blend the result onto SceneColor.
	FGaussianBloomCompositePassParameters* BloomPassParameters =
		GraphBuilder.AllocParameters<FGaussianBloomCompositePassParameters>();
	BloomPassParameters->GradedTexture = FinalHDRTexture;
    BloomPassParameters->RawFogCoverageTexture = Pending.IntermediateTexture;
	BloomPassParameters->IntermediateDepthTexture = Pending.IntermediateDepthTexture;
	BloomPassParameters->IntermediateFogDepthTexture =
		Pending.IntermediateFogDepthTexture ? Pending.IntermediateFogDepthTexture : Pending.IntermediateDepthTexture;
	BloomPassParameters->RenderTargets[0] = Output.GetRenderTargetBinding();

	// Avoid adding a new global-shader uniform: the first v2889 build failed SM6
	// shader-parameter binding. Encode the rain-only
	// 20-tap switch in the otherwise 0..1 Bloom Sun Color Influence value by +2.
	// The shader removes +2 before using the original tint influence, so the visual
	// value is preserved exactly. Non-rain paths keep the original parameter bit-for-bit.
	const bool bDisableAllBloom = CVarPCVRDisableAllBloom.GetValueOnRenderThread()!=0 && GSPCVR::IsActive(View);
    // GS spatial bloom is a subroutine inside the indispensable HDR/fog/alpha
    // composite. Zero spread+scatter makes it return BEFORE the 40 sample taps.
    FVector4f CompositeBloomParamsForPass = Pending.CompositeBloomParams;
    FVector4f CompositeBloomTintForPass = Pending.CompositeBloomTint;
    if (bDisableAllBloom)
    {
        CompositeBloomParamsForPass.X=0.0f; // spread
        CompositeBloomParamsForPass.Y=0.0f; // scatter
        CompositeBloomTintForPass.W=0.0f; // shaped bloom core
    }
	if (bRainBloomHalfSamples && !bDisableAllBloom)
	{
		CompositeBloomParamsForPass.W += 2.0f;
	}

    const bool bUseBloomCache=!bDisableAllBloom && CVarPCVRBloomCache.GetValueOnRenderThread()!=0 &&
        (int32(View.StereoPass)==1 || int32(View.StereoPass)==2) &&
        Pending.CompositeBloomParams.X>0.001f && Pending.CompositeBloomParams.Y>0.0001f;
    BloomPassParameters->BloomExtractTexture=FinalHDRTexture;
    if(bUseBloomCache){
        const FIntPoint Extent=FinalHDRTexture->Desc.Extent;
        auto Desc=FRDGTextureDesc::Create2D(Extent,PF_A32B32G32R32F,FClearValueBinding::None,TexCreate_ShaderResource|TexCreate_UAV);
        auto Extract=GraphBuilder.CreateTexture(Desc,TEXT("GSPCVRBloomExtractFP32"));
        auto* P=GraphBuilder.AllocParameters<FGaussianPCVRBloomExtractCS::FParameters>();
        P->IntermediateTexture=FinalHDRTexture;P->GaussianCompositeBloomParams=CompositeBloomParamsForPass;
        P->GaussianCompositeColorBloomParams=Pending.CompositeColorBloomParams;
        P->GaussianCompositeBloomSoftKnee=FMath::Clamp(Pending.CompositeBloomSoftKnee,0.0f,1.0f);
        P->PCVRBloomExtractExtent=Extent;P->PCVRBloomExtractOut=GraphBuilder.CreateUAV(Extract);
        TShaderMapRef<FGaussianPCVRBloomExtractCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        const FIntVector Groups(FMath::DivideAndRoundUp(Extent.X,8),FMath::DivideAndRoundUp(Extent.Y,8),1);
        ClearUnusedGraphResources(CS,P);
        GraphBuilder.AddPass(RDG_EVENT_NAME("GSPCVRBloomExtractCache"),P,ERDGPassFlags::Compute,
            [P,CS,Groups](FRHIComputeCommandList& RHICmdList){
                SCOPED_DRAW_EVENT(RHICmdList,GSPCVRBloomExtractCache);
                FComputeShaderUtils::Dispatch(RHICmdList,CS,*P,Groups);
            });
        BloomPassParameters->BloomExtractTexture=Extract;
        static FIntPoint LoggedExtent=FIntPoint::ZeroValue;
        if(LoggedExtent!=Extent){UE_LOG(LogTemp,Display,TEXT("[GSPCVR][BloomExtractCache] Enabled=1 Extent=%dx%d Bytes=%llu Precision=FP32 SamplePositionsUnchanged=1 TapsUnchanged=1"),Extent.X,Extent.Y,uint64(Extent.X)*Extent.Y*16);LoggedExtent=Extent;}
    }

	GraphBuilder.AddPass(
		bAfterTonemap
			? RDG_EVENT_NAME("GaussianSplat_AfterTonemapPostGradeComposite")
			: RDG_EVENT_NAME("GaussianSplat_BeforeTonemapPostGradeBloomComposite"),
		BloomPassParameters,
		ERDGPassFlags::Raster,
		[&View, BloomPassParameters,bUseBloomCache,
			 ViewRect = Pending.ViewRect,
			 OutputViewRect = Output.ViewRect,
			 CompositeBloomParams = CompositeBloomParamsForPass,
			 CompositeColorBloomParams = Pending.CompositeColorBloomParams,
			 CompositeBloomTint = CompositeBloomTintForPass,
			 CompositeBloomSoftKnee = Pending.CompositeBloomSoftKnee,
			 PanoramaMode = Pending.PanoramaGSCompositeMode,
			 bDrainRainPanorama = bRainPanoramaGPUDrain,
			 RainDrainInterval = RainGPUDrainInterval,
			 bDrainSnowPanorama = bSnowPanoramaGPUDrain,
			 SnowDrainInterval = SnowGPUDrainInterval,
			 DisplayEV = FMath::Clamp(CVarGSPanoramaGSToneMapEV.GetValueOnRenderThread(), -4.0f, 4.0f)](FRHICommandListImmediate& RHICmdList)
			{
				FGaussianSplatRenderer::CompositeToSceneColor(
					RHICmdList, View, BloomPassParameters->GradedTexture->GetRHI(),
                    BloomPassParameters->RawFogCoverageTexture->GetRHI(),
					BloomPassParameters->IntermediateDepthTexture->GetRHI(),
					BloomPassParameters->IntermediateFogDepthTexture->GetRHI(),
					ViewRect, OutputViewRect, CompositeBloomParams,
					CompositeColorBloomParams, CompositeBloomTint, CompositeBloomSoftKnee,
					PanoramaMode, DisplayEV,bUseBloomCache?BloomPassParameters->BloomExtractTexture->GetRHI():nullptr);

				// v2893/v2894: keep panorama ViewFamily residency bounded after the
				// final GS composite. Snow is intentionally more conservative because
				// the generated snow shell adds millions of splats to the same frame.
				// Snow takes precedence if both weather paths are ever active, avoiding
				// two GPU-idle waits on the same subview. Rendered pixels are complete.
				if (bDrainSnowPanorama && SnowDrainInterval > 0)
				{
					static uint64 SnowPanoramaCompositeSerial = 0;
					const uint64 Serial = ++SnowPanoramaCompositeSerial;
					if ((Serial % static_cast<uint64>(SnowDrainInterval)) == 0ull)
					{
						// v2895: emit sparse proof that the *actual* snow drain path ran.
						// Do not log every face forever; that would add avoidable CPU/I/O cost.
						if (Serial <= 8ull || (Serial % 32ull) == 0ull)
						{
							UE_LOG(LogTemp, Display,
								TEXT("[GSSnowPanoramaDrain] Serial=%llu Frame=%llu Interval=%d View=%dx%d Action=SubmitAndBlockUntilGPUIdle"),
								static_cast<unsigned long long>(Serial),
								static_cast<unsigned long long>(GFrameNumberRenderThread),
								SnowDrainInterval, ViewRect.Width(), ViewRect.Height());
						}
						SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatSnowPanoramaGPUDrain);
						RHICmdList.SubmitAndBlockUntilGPUIdle();
					}
				}
				else if (bDrainRainPanorama && RainDrainInterval > 0)
				{
					static uint64 RainPanoramaCompositeSerial = 0;
					const uint64 Serial = ++RainPanoramaCompositeSerial;
					if ((Serial % static_cast<uint64>(RainDrainInterval)) == 0ull)
					{
						SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatRainPanoramaGPUDrain);
						RHICmdList.SubmitAndBlockUntilGPUIdle();
					}
				}
			});

	return MoveTemp(Output);
}
