#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SceneFlowStartHandle.generated.h"

// Editor helper owned by a director. Moving it updates the director's saved start point.
UCLASS(NotBlueprintable)
class SCENETRANSITIONTEST_API ASceneFlowStartHandle : public AActor
{
    GENERATED_BODY()
public:
    ASceneFlowStartHandle();
    UPROPERTY(VisibleInstanceOnly, Category="Scene Flow") TObjectPtr<class ASceneFlowDirector> Director;
    UPROPERTY(VisibleInstanceOnly, Category="Scene Flow") FGuid SlotId;
    UPROPERTY(VisibleAnywhere, Category="Scene Flow") TObjectPtr<class UStaticMeshComponent> HandleMesh;
    UPROPERTY(VisibleAnywhere, Category="Scene Flow") TObjectPtr<class UArrowComponent> Arrow;
    UFUNCTION(CallInEditor, Category="Scene Flow", meta=(DisplayName="디렉터 슬롯 목록으로 돌아가기")) void SelectDirector();
#if WITH_EDITOR
    void SaveToDirector();
    virtual void PostEditMove(bool bFinished) override;
    virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
};
