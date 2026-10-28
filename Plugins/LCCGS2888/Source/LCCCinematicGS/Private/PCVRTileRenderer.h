#pragma once
#include "CoreMinimal.h"
#include "RHI.h"
#include "RenderGraphFwd.h"
struct FGaussianGlobalAccumulator;
class FSceneView;
// Returns true only after writing all three existing lean MRTs.
bool DrawPCVRTileComposite(FRHICommandListImmediate& Cmd,const FSceneView& View,
 FGaussianGlobalAccumulator* Accumulator,FRHITexture* SceneDepth,int32 Quality,bool bTrim,FBufferRHIRef& RasterArgs);

// Copy/consume diagnostics only after the owning eye raster pass has ended.
void EnqueuePCVRTileDiagnostics(FRDGBuilder& GraphBuilder,FGaussianGlobalAccumulator* Accumulator,FRDGTextureRef RenderedColor);

void CapturePCVRLODProof(FBufferRHIRef LOD,FBufferRHIRef Visible,uint32 Original,uint32 Total);

// Explicit opt-in for existing exact per-pixel removal support; no LOD-test side effects.
bool IsPCVRTileRemovalEnabled_RenderThread();
