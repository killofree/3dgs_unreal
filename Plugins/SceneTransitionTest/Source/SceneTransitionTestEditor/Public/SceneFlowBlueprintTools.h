#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SceneFlowBlueprintTools.generated.h"
class ASceneFlowDirector;
class UBlueprint;
UCLASS()
class SCENETRANSITIONTESTEDITOR_API USceneFlowBlueprintTools : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="Scene Flow|Editor")
    static UBlueprint* CreateSceneDirectorBlueprint(ASceneFlowDirector* Source, const FString& AssetPath);
};
