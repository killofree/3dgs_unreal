#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SceneFlowPath.generated.h"

UCLASS(Blueprintable)
class SCENETRANSITIONTEST_API ASceneFlowPath : public AActor
{
    GENERATED_BODY()
public:
    ASceneFlowPath();
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Scene Flow") TObjectPtr<class USplineComponent> Path;
};

UCLASS(Blueprintable)
class SCENETRANSITIONTEST_API ASceneFlowMarker : public AActor
{
    GENERATED_BODY()
public:
    ASceneFlowMarker();
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Scene Flow") TObjectPtr<class UArrowComponent> Marker;
};
