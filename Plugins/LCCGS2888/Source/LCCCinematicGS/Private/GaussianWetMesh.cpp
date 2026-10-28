#include "GaussianWetMesh.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "ShaderParameterUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "PipelineStateCache.h"
#include "RHIStaticStates.h"
#include "CommonRenderResources.h"
#include "SceneView.h"

static const FSceneInterface* GWetMeshScene = nullptr;
static TArray<FVector4f> GWetMeshVertices;
void PublishGaussianWetMesh(const FSceneInterface* Scene, TArray<FVector4f>&& Vertices)
{
    check(IsInRenderingThread());
    GWetMeshScene = Scene;
    GWetMeshVertices = MoveTemp(Vertices);
}

class FGSWetMeshVS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FGSWetMeshVS);
    SHADER_USE_PARAMETER_STRUCT(FGSWetMeshVS, FGlobalShader);
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, WetMeshVertices)
        SHADER_PARAMETER(FMatrix44f, WetMeshClip)
        SHADER_PARAMETER(FVector3f, WetMeshCamera)
    END_SHADER_PARAMETER_STRUCT()
};
class FGSWetMeshPS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FGSWetMeshPS);
    SHADER_USE_PARAMETER_STRUCT(FGSWetMeshPS, FGlobalShader);
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, WetMeshGSDepth)
    END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FGSWetMeshVS, "/Plugin/LCCCinematicGS/Private/GaussianWetMesh.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FGSWetMeshPS, "/Plugin/LCCCinematicGS/Private/GaussianWetMesh.usf", "MainPS", SF_Pixel);
BEGIN_SHADER_PARAMETER_STRUCT(FGSWetMeshPass, )
    SHADER_PARAMETER_STRUCT_INCLUDE(FGSWetMeshVS::FParameters, VS)
    SHADER_PARAMETER_STRUCT_INCLUDE(FGSWetMeshPS::FParameters, PS)
    RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

FRDGTextureRef RenderGaussianWetMesh(FRDGBuilder& Graph, const FSceneView& View,
    const FIntRect& Rect, FIntPoint Extent, FRDGTextureRef GSDepth)
{
    FRDGTextureRef Surface = Graph.CreateTexture(FRDGTextureDesc::Create2D(Extent,
        PF_A32B32G32R32F, FClearValueBinding::Transparent,
        TexCreate_RenderTargetable | TexCreate_ShaderResource), TEXT("GSWetMeshSurface"));
    if (!View.Family || GWetMeshScene != View.Family->Scene || GWetMeshVertices.IsEmpty())
    {
        AddClearRenderTargetPass(Graph, Surface);
        return Surface;
    }
    FRDGTextureRef Depth = Graph.CreateTexture(FRDGTextureDesc::Create2D(Extent,
        PF_DepthStencil, FClearValueBinding::DepthFar, TexCreate_DepthStencilTargetable), TEXT("GSWetMeshDepth"));
    auto* P = Graph.AllocParameters<FGSWetMeshPass>();
    auto* Buffer = CreateStructuredBuffer(Graph, TEXT("GSWetMeshTriangles"), sizeof(FVector4f),
        GWetMeshVertices.Num(), GWetMeshVertices.GetData(), GWetMeshVertices.Num() * sizeof(FVector4f));
    P->VS.WetMeshVertices = Graph.CreateSRV(Buffer);
    P->VS.WetMeshClip = FMatrix44f(View.ViewMatrices.GetViewProjectionMatrix());
    P->VS.WetMeshCamera = FVector3f(View.ViewLocation);
    P->PS.WetMeshGSDepth = GSDepth;
    P->RenderTargets[0] = FRenderTargetBinding(Surface, ERenderTargetLoadAction::EClear);
    P->RenderTargets.DepthStencil = FDepthStencilBinding(Depth, ERenderTargetLoadAction::EClear,
        ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);
    const uint32 Triangles = GWetMeshVertices.Num() / 3;
    const auto FeatureLevel = View.GetFeatureLevel();
    Graph.AddPass(RDG_EVENT_NAME("GaussianWetMesh_RasterTriangles"), P, ERDGPassFlags::Raster,
        [P, Rect, Triangles, FeatureLevel](FRHICommandList& Cmd)
        {
            TShaderMapRef<FGSWetMeshVS> VS(GetGlobalShaderMap(FeatureLevel));
            TShaderMapRef<FGSWetMeshPS> PS(GetGlobalShaderMap(FeatureLevel));
            FGraphicsPipelineStateInitializer State;
            Cmd.ApplyCachedRenderTargets(State);
            State.BlendState = TStaticBlendState<>::GetRHI();
            State.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
            State.DepthStencilState = TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI();
            State.PrimitiveType = PT_TriangleList;
            State.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
            State.BoundShaderState.VertexShaderRHI = VS.GetVertexShader();
            State.BoundShaderState.PixelShaderRHI = PS.GetPixelShader();
            SetGraphicsPipelineState(Cmd, State, 0);
            SetShaderParameters(Cmd, VS, VS.GetVertexShader(), P->VS);
            SetShaderParameters(Cmd, PS, PS.GetPixelShader(), P->PS);
            Cmd.SetViewport(Rect.Min.X, Rect.Min.Y, 0, Rect.Max.X, Rect.Max.Y, 1);
            Cmd.DrawPrimitive(0, Triangles, 1);
        });
    return Surface;
}
