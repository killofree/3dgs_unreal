#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CherryGrowthMaterialLibrary.generated.h"
class UMaterial;
class UStaticMesh;
class UPrimitiveComponent;
class AActor;
class UWorld;
class UBlueprint;
UCLASS()
class SCENEENVIRONMENTEDITOR_API UCherryGrowthMaterialLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="Sun Control") static void FinishControlCompilation();
    UFUNCTION(BlueprintCallable, Category="Sun Control") static int32 ConfigureSunControlMaterial(UMaterial* Material);
    UFUNCTION(BlueprintPure, Category="Sun Control") static float ReadControlData(const UPrimitiveComponent* Component, int32 Index);
    // Only use on a dedicated material duplicate. UV4/5 hold local XYZ pivots.
    UFUNCTION(BlueprintCallable, Category="Tree Growth") static bool AddPivotScale(UMaterial* Material);
    UFUNCTION(BlueprintCallable, Category="Tree Growth") static void ConfigurePivotMesh(UStaticMesh* Mesh);
    UFUNCTION(BlueprintPure, Category="Tree Growth") static float ReadGrowthScale(const UPrimitiveComponent* Component);
    UFUNCTION(BlueprintCallable, Category="Tree Growth") static AActor* LoadGrowthTarget(UWorld* World, const FString& ActorPath);
    UFUNCTION(BlueprintCallable, Category="Tree Growth") static bool SaveGrowthTarget(AActor* Actor);
    UFUNCTION(BlueprintCallable, Category="Tree Growth") static UBlueprint* CreateTreeBlueprint(AActor* Actor, const FString& AssetPath);
};
