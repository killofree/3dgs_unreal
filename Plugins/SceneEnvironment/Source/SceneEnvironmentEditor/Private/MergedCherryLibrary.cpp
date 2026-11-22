#include "MergedCherryLibrary.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "MeshDescription.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "HAL/PlatformMemory.h"

bool UMergedCherryLibrary::ConfigureMergedLeafMaterial(UMaterial* Material)
{
    if (!Material) return false;
    for (UMaterialExpression* Expression:Material->GetExpressions())
        if (auto* P=Cast<UMaterialExpressionScalarParameter>(Expression);P && P->ParameterName==TEXT("CherryGrowthScale"))
        {
            Material->Modify();P->bUseCustomPrimitiveData=true;P->PrimitiveDataIndex=30;
            Material->PostEditChange();Material->MarkPackageDirty();return true;
        }
    return false;
}

UStaticMesh* UMergedCherryLibrary::MergeAssembledTree(AActor* Source,const FString& AssetPath,const TMap<UMaterialInterface*,UMaterialInterface*>& MaterialOverrides,int64 MaxTriangles)
{
    if (!Source || !FPackageName::IsValidLongPackageName(AssetPath)) return nullptr;
    if (FindObject<UStaticMesh>(nullptr,*(AssetPath+TEXT(".")+FPackageName::GetShortName(AssetPath)))) return nullptr;
    TInlineComponentArray<UStaticMeshComponent*> Components(Source);
    int64 Triangles=0,Vertices=0;
    for (auto* C:Components) if (UStaticMesh* Mesh=C->GetStaticMesh())
    {
        if (!Mesh->GetRenderData() || Mesh->GetRenderData()->LODResources.IsEmpty()) return nullptr;
        const auto& LOD=Mesh->GetRenderData()->LODResources[0];
        const int64 Count=Cast<UInstancedStaticMeshComponent>(C)?Cast<UInstancedStaticMeshComponent>(C)->GetInstanceCount():1;
        Triangles+=int64(LOD.GetNumTriangles())*Count;Vertices+=int64(LOD.GetNumVertices())*Count;
    }
    if (Triangles<=0 || Triangles>MaxTriangles || Vertices>MAX_int32 || FPlatformMemory::GetStats().AvailablePhysical<uint64(4)*1024*1024*1024)
    {
        UE_LOG(LogTemp,Error,TEXT("Tree merge refused: %lld triangles / %lld vertices; limit %lld; require 4 GiB free physical RAM"),Triangles,Vertices,MaxTriangles);
        return nullptr;
    }
    UE_LOG(LogTemp,Display,TEXT("TREE_MERGE_BEGIN triangles=%lld vertices=%lld"),Triangles,Vertices);
    FMeshDescription Merged;
    FStaticMeshAttributes MergedAttributes(Merged);MergedAttributes.Register();
    MergedAttributes.GetVertexInstanceUVs().SetNumChannels(6);
    Merged.ReserveNewVertices(int32(Vertices));Merged.ReserveNewVertexInstances(int32(Vertices));
    Merged.ReserveNewTriangles(int32(Triangles));Merged.ReserveNewPolygons(int32(Triangles));
    Merged.ReserveNewEdges(int32(FMath::Min<int64>(Triangles*2,MAX_int32)));
    TArray<UMaterialInterface*> Materials;
    TArray<FPolygonGroupID> Groups;
    const FTransform ActorTransform=Source->GetActorTransform();
    int32 Completed=0;
    for (auto* C:Components) if (UStaticMesh* Mesh=C->GetStaticMesh())
    {
        const auto& LOD=Mesh->GetRenderData()->LODResources[0];
        FMeshDescription Base;FStaticMeshAttributes Attr(Base);Attr.Register();
        auto Positions=Attr.GetVertexPositions();auto Normals=Attr.GetVertexInstanceNormals();auto Tangents=Attr.GetVertexInstanceTangents();
        auto Signs=Attr.GetVertexInstanceBinormalSigns();auto Colors=Attr.GetVertexInstanceColors();auto UVs=Attr.GetVertexInstanceUVs();UVs.SetNumChannels(6);
        TArray<FVertexInstanceID> Instances;Instances.Reserve(LOD.GetNumVertices());
        Base.ReserveNewVertices(LOD.GetNumVertices());Base.ReserveNewVertexInstances(LOD.GetNumVertices());
        for (int32 V=0;V<LOD.GetNumVertices();++V)
        {
            const FVertexID Vertex=Base.CreateVertex();Positions[Vertex]=LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(V);
            const FVertexInstanceID Instance=Base.CreateVertexInstance(Vertex);Instances.Add(Instance);
            const FVector3f X=FVector3f(LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentX(V));
            const FVector3f Y=FVector3f(LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentY(V));
            const FVector3f Z=FVector3f(LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(V));
            Normals[Instance]=Z;Tangents[Instance]=X;Signs[Instance]=FVector3f::DotProduct(FVector3f::CrossProduct(Z,X),Y)<0?-1.f:1.f;
            Colors[Instance]=LOD.VertexBuffers.ColorVertexBuffer.GetNumVertices()>uint32(V)?FVector4f(LOD.VertexBuffers.ColorVertexBuffer.VertexColor(V).ReinterpretAsLinear()):FVector4f(1,1,1,1);
            for (int32 Channel=0;Channel<6;++Channel) UVs.Set(Instance,Channel,Channel<int32(LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords())?LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(V,Channel):FVector2f::ZeroVector);
        }
        TMap<FPolygonGroupID,FPolygonGroupID> GroupMap;
        for (const auto& Section:LOD.Sections)
        {
            UMaterialInterface* Material=C->GetMaterial(Section.MaterialIndex);
            if (auto* const* Override=MaterialOverrides.Find(Material)) Material=*Override;
            int32 Slot=Materials.IndexOfByKey(Material);
            if (Slot==INDEX_NONE)
            {
                Slot=Materials.Add(Material);const auto Group=Merged.CreatePolygonGroup();Groups.Add(Group);
                MergedAttributes.GetPolygonGroupMaterialSlotNames()[Group]=FName(*FString::Printf(TEXT("Slot_%02d"),Slot));
            }
            const FPolygonGroupID SourceGroup=Base.CreatePolygonGroup();GroupMap.Add(SourceGroup,Groups[Slot]);
            for (uint32 T=0;T<Section.NumTriangles;++T)
            {
                const uint32 I=Section.FirstIndex+T*3;
                const FVertexInstanceID Corners[]={Instances[LOD.IndexBuffer.GetIndex(I)],Instances[LOD.IndexBuffer.GetIndex(I+1)],Instances[LOD.IndexBuffer.GetIndex(I+2)]};
                Base.CreateTriangle(SourceGroup,TArrayView<const FVertexInstanceID>(Corners,3));
            }
        }
        const auto* ISM=Cast<UInstancedStaticMeshComponent>(C);const int32 Count=ISM?ISM->GetInstanceCount():1;
        const bool bGrowth=C->ComponentHasTag(TEXT("CherryGrowthFlowers")) || C->ComponentHasTag(TEXT("CherryGrowthLeaves"));
        for (int32 I=0;I<Count;++I)
        {
            FTransform WorldTransform=C->GetComponentTransform();if (ISM) ISM->GetInstanceTransform(I,WorldTransform,true);
            const FTransform Local=WorldTransform.GetRelativeTransform(ActorTransform);
            FMeshDescription Piece(Base);
            if (bGrowth)
            {
                auto Pivots=FStaticMeshAttributes(Piece).GetVertexInstanceUVs();
                for (auto ID:Piece.VertexInstances().GetElementIDs())
                {
                    const auto XY=Pivots.Get(ID,4);const auto Z=Pivots.Get(ID,5);
                    const FVector P=Local.TransformPosition(FVector(XY.X,XY.Y,Z.X));
                    Pivots.Set(ID,4,FVector2f(P.X,P.Y));Pivots.Set(ID,5,FVector2f(P.Z,Z.Y));
                }
            }
            FStaticMeshOperations::FAppendSettings Settings;for (bool& Channel:Settings.bMergeUVChannels) Channel=true;
            Settings.MeshTransform=Local;
            Settings.PolygonGroupsDelegate.BindLambda([&GroupMap](const FMeshDescription&,FMeshDescription&,PolygonGroupMap& Remap) { Remap=GroupMap; });
            FStaticMeshOperations::AppendMeshDescription(Piece,Merged,Settings);
        }
        UE_LOG(LogTemp,Display,TEXT("TREE_MERGE_PART %d/%d %s instances=%d"),++Completed,Components.Num(),*C->GetName(),Count);
    }
    if (Merged.Triangles().Num()!=Triangles) return nullptr;
    UPackage* Package=CreatePackage(*AssetPath);
    auto* Result=NewObject<UStaticMesh>(Package,*FPackageName::GetShortName(AssetPath),RF_Public|RF_Standalone);
    Result->InitResources();Result->SetLightingGuid();Result->GetNaniteSettings().bEnabled=false;Result->bAllowCPUAccess=true;
    auto& SourceModel=Result->AddSourceModel();auto& Build=SourceModel.BuildSettings;
    Build.bRecomputeNormals=false;Build.bRecomputeTangents=false;Build.bGenerateLightmapUVs=false;
    Build.bUseFullPrecisionUVs=true;Build.bRemoveDegenerates=false;Build.bBuildReversedIndexBuffer=false;Build.DistanceFieldResolutionScale=0;
    for (int32 I=0;I<Materials.Num();++I) Result->GetStaticMaterials().Add(FStaticMaterial(Materials[I],FName(*FString::Printf(TEXT("Slot_%02d"),I))));
    Result->CreateMeshDescription(0,MoveTemp(Merged));Result->CommitMeshDescription(0);
    TArray<FText> Errors;Result->Build(true,&Errors);
    for (const auto& Error:Errors) UE_LOG(LogTemp,Warning,TEXT("TREE_MERGE_BUILD %s"),*Error.ToString());
    FAssetRegistryModule::AssetCreated(Result);Result->MarkPackageDirty();
    UE_LOG(LogTemp,Display,TEXT("TREE_MERGE_COMPLETE %s triangles=%lld materials=%d"),*AssetPath,Triangles,Materials.Num());
    return Result;
}
