#pragma once
#include "CoreMinimal.h"
#include "RenderGraphResources.h"
class FSceneInterface;
class FSceneView;
class FRDGBuilder;
// Render-thread-only, same single weather authority as the existing wet pass.
void PublishGaussianWetMesh(const FSceneInterface* Scene, TArray<FVector4f>&& Vertices);
FRDGTextureRef RenderGaussianWetMesh(FRDGBuilder& Graph, const FSceneView& View,
    const FIntRect& Rect, FIntPoint Extent, FRDGTextureRef GSDepth);
