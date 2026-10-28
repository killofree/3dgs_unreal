// Patch39: render-thread-only removal volumes; no per-view screen mask.
#pragma once
#include "CoreMinimal.h"

class FSceneInterface;
static constexpr uint32 GSGSRemovalBoxMax = 8;
struct FGSRemovalBoxGPUData
{
    FVector4f Center = FVector4f(0,0,0,0);
    FVector4f X = FVector4f(1,0,0,0);
    FVector4f Y = FVector4f(0,1,0,0);
    FVector4f Z = FVector4f(0,0,1,0);
};
void SetGSRemovalBox_RenderThread(const FSceneInterface* Scene, uint64 ActorId, const FGSRemovalBoxGPUData& Data);
void ClearGSRemovalBox_RenderThread(const FSceneInterface* Scene, uint64 ActorId);
uint32 GetGSRemovalBoxes_RenderThread(const FSceneInterface* Scene, FGSRemovalBoxGPUData OutBoxes[GSGSRemovalBoxMax]);
