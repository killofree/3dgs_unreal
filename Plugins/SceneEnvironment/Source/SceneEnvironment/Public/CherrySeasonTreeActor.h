#pragma once
#include "CoreMinimal.h"
#include "Engine/StaticMeshActor.h"
#include "CherrySeasonTreeActor.generated.h"
class UCherryTreeGrowthComponent;

UCLASS(Blueprintable, meta=(DisplayName="Seasonal Cherry Tree", PrioritizeCategories="Tree Growth"))
class SCENEENVIRONMENT_API ACherrySeasonTreeActor : public AStaticMeshActor
{
    GENERATED_BODY()
public:
    ACherrySeasonTreeActor();
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, BlueprintSetter=SetFlowerSize, Category="Tree Growth", meta=(DisplayName="Flower Size", DisplayPriority="0", ClampMin="0", ClampMax="2", UIMin="0", UIMax="2"))
    float FlowerSize=1.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, BlueprintSetter=SetLeafSize, Category="Tree Growth", meta=(DisplayName="Leaf Size", DisplayPriority="1", ClampMin="0", ClampMax="2", UIMin="0", UIMax="2"))
    float LeafSize=1.f;
    UFUNCTION(BlueprintCallable, Category="Tree Growth") void SetFlowerSize(float Value);
    UFUNCTION(BlueprintCallable, Category="Tree Growth") void SetLeafSize(float Value);
    UFUNCTION(BlueprintCallable, CallInEditor, Category="Tree Growth") void RefreshTree();
    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void Tick(float DeltaSeconds) override;
#if WITH_EDITOR
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
    virtual void PreEditChange(FProperty* Property) override;
    virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
private:
    void ApplyControls();
    UPROPERTY() TObjectPtr<UCherryTreeGrowthComponent> GrowthDriver;
};
