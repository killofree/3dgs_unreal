#include "CherryGrowthMaterialLibrary.h"
#include "AssetCompilingManager.h"
#include "Engine/StaticMesh.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "FileHelpers.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionGetMaterialAttributes.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTransformPosition.h"
#include "Materials/MaterialExpressionWorldPosition.h"

void UCherryGrowthMaterialLibrary::FinishControlCompilation() { FAssetCompilingManager::Get().FinishAllCompilation(); }
int32 UCherryGrowthMaterialLibrary::ConfigureSunControlMaterial(UMaterial* Material)
{
    if (!Material) return 0;
    int32 Count=0; Material->Modify();
    for (UMaterialExpression* Expression:Material->GetExpressions())
        if (auto* Scalar=Cast<UMaterialExpressionScalarParameter>(Expression))
        {
            int32 Index=-1;
            if (Scalar->ParameterName==TEXT("CoreEmission")) Index=25;
            if (Scalar->ParameterName==TEXT("GlowStrength")) Index=26;
            if (Scalar->ParameterName==TEXT("RayStrength")) Index=27;
            if (Index>=0) { Scalar->bUseCustomPrimitiveData=true; Scalar->PrimitiveDataIndex=Index; ++Count; }
        }
    if (Count) Material->PostEditChange();
    return Count;
}
float UCherryGrowthMaterialLibrary::ReadControlData(const UPrimitiveComponent* Component,int32 Index)
{
    if (!Component) return -1;
    const auto& Data=Component->GetCustomPrimitiveData().Data;
    return Data.IsValidIndex(Index) ? Data[Index] : -1;
}

void UCherryGrowthMaterialLibrary::ConfigurePivotMesh(UStaticMesh* Mesh)
{
    if (!Mesh || Mesh->GetNumSourceModels()==0) return;
    Mesh->Modify();
    auto& Settings=Mesh->GetSourceModel(0).BuildSettings;
    Settings.bUseFullPrecisionUVs=true;
    Settings.bGenerateLightmapUVs=false;
    Mesh->PostEditChange();
}
float UCherryGrowthMaterialLibrary::ReadGrowthScale(const UPrimitiveComponent* Component)
{
    if (!Component) return -1.f;
    const auto& Data=Component->GetCustomPrimitiveData().Data;
    return Data.IsValidIndex(31) ? Data[31] : -1.f;
}
AActor* UCherryGrowthMaterialLibrary::LoadGrowthTarget(UWorld* World,const FString& ActorPath)
{
    if (!World) return nullptr;
    if (AActor* Actor=FindObject<AActor>(nullptr,*ActorPath)) return Actor;
    if (UWorldPartition* Partition=World->GetWorldPartition())
        if (const auto* Desc=Partition->GetActorDescInstanceByPath(ActorPath))
        {
            Partition->PinActors({Desc->GetGuid()});
            return Desc->GetActor();
        }
    return nullptr;
}
bool UCherryGrowthMaterialLibrary::SaveGrowthTarget(AActor* Actor)
{
    if (!Actor) return false;
    UPackage* Package=Actor->GetExternalPackage();
    if (!Package) return false; // Whole-map saving is deliberately a separate operation.
    Package->MarkPackageDirty();
    return UEditorLoadingAndSavingUtils::SavePackages({Package},true);
}
UBlueprint* UCherryGrowthMaterialLibrary::CreateTreeBlueprint(AActor* Actor,const FString& AssetPath)
{
    if (!Actor) return nullptr;
    FKismetEditorUtilities::FCreateBlueprintFromActorParams Params;
    Params.bReplaceActor=false; Params.bOpenBlueprint=false; Params.bKeepMobility=true;
    return FKismetEditorUtilities::CreateBlueprintFromActor(AssetPath,Actor,Params);
}

bool UCherryGrowthMaterialLibrary::AddPivotScale(UMaterial* Material)
{
    if (!Material) return false;
    auto* Data=Material->GetEditorOnlyData();
    if (!Data) return false;
    // Dedicated STATIC/ISM copies use six UV channels for pivots. Do not retain
    // skeletal permutations from the source PVE material (GPU skinning allows four).
    Material->SetUsageByFlag(MATUSAGE_SkeletalMesh,false);
    Material->SetUsageByFlag(MATUSAGE_MorphTargets,false);
    Material->SetUsageByFlag(MATUSAGE_Clothing,false);
    Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes,true);
    for (UMaterialExpression* E:Material->GetExpressions())
        if (auto* P=Cast<UMaterialExpressionScalarParameter>(E); P && P->ParameterName==TEXT("CherryGrowthScale")) return true;
    Material->Modify();
    auto New=[&]<typename T>() { T* E=NewObject<T>(Material); Material->GetExpressionCollection().AddExpression(E); return E; };
    auto* XY=New.operator()<UMaterialExpressionTextureCoordinate>(); XY->CoordinateIndex=4;
    auto* ZUV=New.operator()<UMaterialExpressionTextureCoordinate>(); ZUV->CoordinateIndex=5;
    auto* Z=New.operator()<UMaterialExpressionComponentMask>(); Z->R=true; Z->G=Z->B=Z->A=false; Z->Input.Connect(0,ZUV);
    auto* Pivot=New.operator()<UMaterialExpressionAppendVector>(); Pivot->A.Connect(0,XY); Pivot->B.Connect(0,Z);
    auto* PivotWorld=New.operator()<UMaterialExpressionTransformPosition>();
    PivotWorld->TransformSourceType=TRANSFORMPOSSOURCE_Instance;
    PivotWorld->TransformType=TRANSFORMPOSSOURCE_TranslatedWorld;
    PivotWorld->Input.Connect(0,Pivot);
    auto* Position=New.operator()<UMaterialExpressionWorldPosition>(); Position->WorldPositionShaderOffset=WPT_CameraRelativeNoOffsets;
    auto* Scale=New.operator()<UMaterialExpressionScalarParameter>(); Scale->ParameterName=TEXT("CherryGrowthScale");
    Scale->DefaultValue=1.f; Scale->bUseCustomPrimitiveData=true; Scale->PrimitiveDataIndex=31;
    auto* WPO=New.operator()<UMaterialExpressionCustom>(); WPO->OutputType=CMOT_Float3;
    WPO->Code=TEXT("float s=clamp(Size,0.0,2.0); return (Position-PivotWorld)*(s-1.0)+OldWPO*s;");
    WPO->Description=TEXT("Cherry per-element pivot growth; branch material is unchanged");
    auto Input=[&](FName Name,UMaterialExpression* Expression,int32 Index=0)
    { FCustomInput I; I.InputName=Name; I.Input.Connect(Index,Expression); WPO->Inputs.Add(I); };
    WPO->Inputs.Reset(); Input(TEXT("Position"),Position); Input(TEXT("PivotWorld"),PivotWorld); Input(TEXT("Size"),Scale);
    if (Material->bUseMaterialAttributes)
    {
        if (!Data->MaterialAttributes.Expression) return false;
        auto* Get=New.operator()<UMaterialExpressionGetMaterialAttributes>(); Get->MaterialAttributes=Data->MaterialAttributes;
        const int32 Output=Get->CreateOrGetOutputAttribute(MP_WorldPositionOffset);
        Input(TEXT("OldWPO"),Get,Output);
        auto* Set=New.operator()<UMaterialExpressionSetMaterialAttributes>();
        Set->GetInput(0)->Connect(Data->MaterialAttributes.OutputIndex,Data->MaterialAttributes.Expression);
        Set->ConnectInputAttribute(MP_WorldPositionOffset,WPO);
        Data->MaterialAttributes.Connect(0,Set);
    }
    else
    {
        if (Data->WorldPositionOffset.Expression) Input(TEXT("OldWPO"),Data->WorldPositionOffset.Expression,Data->WorldPositionOffset.OutputIndex);
        else Input(TEXT("OldWPO"),New.operator()<UMaterialExpressionConstant3Vector>());
        Data->WorldPositionOffset.Connect(0,WPO);
    }
    Material->PostEditChange(); Material->MarkPackageDirty();
    return true;
}
