#pragma once
#include "CoreMinimal.h"
class FSceneView;
class FSceneViewFamily;
class UWorld;
namespace GSPCVR
{
    void ObserveRuntimeFamily(const FSceneViewFamily& Family);
    bool IsRuntimeWorld(const UWorld* World);
    bool OrderedCaptures(const UWorld* World);
    bool LeanTargets(const FSceneView& View);
    bool IsRealtimeHMD(const FSceneView& View);
    bool IsActive(const FSceneView& View);
    bool PreSort(const FSceneView& View);
    bool RasterClip(const FSceneView& View);
    float RasterClipMinArea();
    bool StereoPair(const FSceneView& View, const FSceneView*& Left, const FSceneView*& Right);
    bool BlockOfflineDrain(const FSceneViewFamily& Family);
    bool SkipCaptureDrain(const FSceneViewFamily& Family);
    uint32 CacheKey(const FSceneView& View);
}
