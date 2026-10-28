#include "GSRemovalBoxRenderState.h"
#include "RenderingThread.h"

namespace
{
    // Only accessed by render-thread commands and shader-parameter setup.
    TMap<const FSceneInterface*, TMap<uint64, FGSRemovalBoxGPUData>> SceneBoxes;
}

void SetGSRemovalBox_RenderThread(const FSceneInterface* Scene, uint64 ActorId, const FGSRemovalBoxGPUData& Data)
{
    check(IsInRenderingThread());
    if (Scene) SceneBoxes.FindOrAdd(Scene).Add(ActorId, Data);
}

void ClearGSRemovalBox_RenderThread(const FSceneInterface* Scene, uint64 ActorId)
{
    check(IsInRenderingThread());
    if (auto* Boxes = SceneBoxes.Find(Scene))
    {
        Boxes->Remove(ActorId);
        if (Boxes->IsEmpty()) SceneBoxes.Remove(Scene);
    }
}

uint32 GetGSRemovalBoxes_RenderThread(const FSceneInterface* Scene, FGSRemovalBoxGPUData OutBoxes[GSGSRemovalBoxMax])
{
    check(IsInRenderingThread());
    uint32 Count = 0;
    if (const auto* Boxes = SceneBoxes.Find(Scene))
    {
        for (const auto& Pair : *Boxes)
        {
            if (Count == GSGSRemovalBoxMax) break;
            OutBoxes[Count++] = Pair.Value;
        }
    }
    return Count;
}
