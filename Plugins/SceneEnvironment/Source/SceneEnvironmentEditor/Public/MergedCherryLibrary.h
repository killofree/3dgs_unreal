#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MergedCherryLibrary.generated.h"
class UStaticMesh;
class UMaterial;
class UMaterialInterface;
class AActor;
UCLASS()
class SCENEENVIRONMENTEDITOR_API UMergedCherryLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="Tree Merge")
    static UStaticMesh* MergeAssembledTree(AActor* Source,const FString& AssetPath,const TMap<UMaterialInterface*,UMaterialInterface*>& MaterialOverrides,int64 MaxTriangles=3000000);
    UFUNCTION(BlueprintCallable, Category="Tree Merge")
    static bool ConfigureMergedLeafMaterial(UMaterial* Material);
};
