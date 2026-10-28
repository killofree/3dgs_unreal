#include "GaussianPCVR.h"
#include "SceneView.h"
#include "StereoRendering.h"
#include "HAL/IConsoleManager.h"
#include "Engine/World.h"
#include "Misc/ScopeLock.h"
static TAutoConsoleVariable<int32> Enable(TEXT("gs.PCVR.Enable"),0,TEXT("Opt-in realtime PCVR optimizations. Offline/capture paths are excluded."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> RuntimeMode(TEXT("gs.PCVR.RuntimeMode"),0,
    TEXT("0 existing reference; 1 realtime PCVR runtime policy. Requires an observed realtime main view; offline views revoke eligibility. Effects and AA are unchanged."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> OrderedCaptureEnabled(TEXT("gs.PCVR.OrderedCaptures"),1,
    TEXT("RuntimeMode 1: use ordered render commands for realtime captures; readback/offline paths retain Flush."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> LeanTargetsEnabled(TEXT("gs.PCVR.LeanTargets"),1,
    TEXT("RuntimeMode 1: consumer-gated 3-MRT production shader for one non-wet/non-snow proxy. Effects stay enabled."),ECVF_RenderThreadSafe);
bool GSPCVR::OrderedCaptures(const UWorld* World)
{ return OrderedCaptureEnabled.GetValueOnGameThread()!=0 && IsRuntimeWorld(World); }
bool GSPCVR::LeanTargets(const FSceneView& View)
{ return RuntimeMode.GetValueOnRenderThread()==1 && LeanTargetsEnabled.GetValueOnRenderThread()!=0 && IsActive(View); }
namespace
{
    FCriticalSection RuntimeSceneLock;
    TMap<const FSceneInterface*, double> RuntimeScenes;
}
void GSPCVR::ObserveRuntimeFamily(const FSceneViewFamily& F)
{
    if (!F.Scene) return;
    bool HasMain = false, Eligible = true;
    for (const FSceneView* V : F.Views)
    {
        if (!V) { Eligible = false; continue; }
        if (V->bIsSceneCapture) continue;
        HasMain = true;
        Eligible &= IsActive(*V);
    }
    if (!HasMain) return; // Helper captures never grant runtime permission.
    FScopeLock Lock(&RuntimeSceneLock);
    const double Now = FPlatformTime::Seconds();
    for (auto It = RuntimeScenes.CreateIterator(); It; ++It)
        if (Now - It.Value() > 2.0) It.RemoveCurrent();
    if (RuntimeMode.GetValueOnRenderThread() == 1 && Eligible)
        RuntimeScenes.Add(F.Scene, Now);
    else RuntimeScenes.Remove(F.Scene);
}
bool GSPCVR::IsRuntimeWorld(const UWorld* World)
{
    check(IsInGameThread());
    if (!World || !World->IsGameWorld() || IsRunningCommandlet() ||
        Enable.GetValueOnGameThread() == 0 || RuntimeMode.GetValueOnGameThread() != 1) return false;
    FScopeLock Lock(&RuntimeSceneLock);
    const double* Seen = RuntimeScenes.Find(World->Scene);
    return Seen && FPlatformTime::Seconds() - *Seen < 2.0;
}
static TAutoConsoleVariable<int32> Desktop(TEXT("gs.PCVR.DesktopTest"),0,TEXT("Apply the PCVR opt-in path to a realtime mono view for controlled tests."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> Compact(TEXT("gs.PCVR.PreSortCompact"),1,TEXT("Stable GPU compaction before sorting the non-cluster/mixed global path."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> SharedCull(TEXT("gs.PCVR.SharedCulling"),1,TEXT("Conservative OR of two eye frusta, shared finest-required cluster LOD."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> RasterClipEnabled(TEXT("gs.PCVR.RasterClip"),0,TEXT("Mode 3: clip empty quad corners before pixel shading. 0 reference, 1 clipped."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<float> RasterClipArea(TEXT("gs.PCVR.RasterClipMinArea"),256.0f,TEXT("Minimum projected Gaussian quad area in pixels for corner clipping. Small splats keep the quad."),ECVF_RenderThreadSafe);
static TAutoConsoleVariable<int32> SkipCaptureIdle(TEXT("gs.PCVR.SkipCaptureDrain"),1,
    TEXT("Exclude helper SceneCaptures (which never draw GS) from panorama pre-view GPU idle waits. Main offline/panorama views retain their guards. 0 reference, 1 fix."),ECVF_RenderThreadSafe);
bool GSPCVR::SkipCaptureDrain(const FSceneViewFamily& F)
{
    if(Enable.GetValueOnRenderThread()==0 || SkipCaptureIdle.GetValueOnRenderThread()==0 || F.Views.IsEmpty()) return false;
    // GS post-opaque explicitly excludes these views. They therefore cannot own
    // the large GS working set the panorama drain protects. Test every view so a
    // mixed family with a genuine main/offline GS view retains its existing guard.
    for(const FSceneView* V:F.Views) if(!V || !V->bIsSceneCapture) return false;
    return true;
}
bool GSPCVR::RasterClip(const FSceneView& V) { return IsActive(V) && RasterClipEnabled.GetValueOnRenderThread()!=0; }
float GSPCVR::RasterClipMinArea() { return FMath::Clamp(RasterClipArea.GetValueOnRenderThread(),0.0f,1.e9f); }
bool GSPCVR::IsRealtimeHMD(const FSceneView& V)
{ return V.Family && !V.bIsOfflineRender && !V.bIsSceneCapture && IStereoRendering::IsStereoEyeView(V); }
bool GSPCVR::IsActive(const FSceneView& V)
{ return Enable.GetValueOnRenderThread()!=0 && V.Family && !V.bIsOfflineRender && !V.bIsSceneCapture &&
    (IsRealtimeHMD(V) || (Desktop.GetValueOnRenderThread()!=0 && V.Family->bRealtimeUpdate)); }
bool GSPCVR::PreSort(const FSceneView& V) { return IsActive(V) && Compact.GetValueOnRenderThread()!=0; }
bool GSPCVR::StereoPair(const FSceneView& V,const FSceneView*& L,const FSceneView*& R)
{
    L=R=nullptr;
    if (!IsActive(V) || !IsRealtimeHMD(V) || SharedCull.GetValueOnRenderThread()==0) return false;
    int32 N=0;
    for (const FSceneView* Eye:V.Family->Views)
        if (Eye && IsRealtimeHMD(*Eye)) { if (N==0)L=Eye; else if(N==1)R=Eye; ++N; }
    return N==2 && L && R;
}
bool GSPCVR::BlockOfflineDrain(const FSceneViewFamily& F)
{
    if (Enable.GetValueOnRenderThread()==0) return false;
    for(const FSceneView* V:F.Views) if(V && IsRealtimeHMD(*V)) return true;
    return false;
}
uint32 GSPCVR::CacheKey(const FSceneView& V)
{ return IsActive(V) ? 1u | (PreSort(V)?2u:0u) | (SharedCull.GetValueOnRenderThread()!=0?4u:0u) | (RuntimeMode.GetValueOnRenderThread()==1?8u:0u) : 0u; }
