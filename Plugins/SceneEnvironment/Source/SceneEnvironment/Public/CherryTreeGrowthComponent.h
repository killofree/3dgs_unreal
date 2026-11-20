#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "CherryTreeGrowthComponent.generated.h"
class UMeshComponent;

UCLASS(ClassGroup=(Vegetation), meta=(BlueprintSpawnableComponent, DisplayName="Cherry Tree Growth Controls"))
class SCENEENVIRONMENT_API UCherryTreeGrowthComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UCherryTreeGrowthComponent();
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, BlueprintSetter=SetFlowerSize, Category="Tree Growth", meta=(ClampMin="0", ClampMax="2", UIMin="0", UIMax="2", ToolTip="0 hides flowers, 1 authored size, 2 double size. Branches stay fixed."))
    float FlowerSize=1.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, BlueprintSetter=SetLeafSize, Category="Tree Growth", meta=(ClampMin="0", ClampMax="2", UIMin="0", UIMax="2", ToolTip="0 hides leaves, 1 authored size, 2 double size. Scales around the baked leaf attachment pivot."))
    float LeafSize=1.f;
    UFUNCTION(BlueprintCallable, Category="Tree Growth") void SetFlowerSize(float Value);
    UFUNCTION(BlueprintCallable, Category="Tree Growth") void SetLeafSize(float Value);
    UFUNCTION(BlueprintCallable, CallInEditor, Category="Tree Growth") void RefreshGrowthTargets();
    virtual void OnRegister() override;

#if WITH_EDITOR
    virtual void PreEditChange(FProperty* Property) override;
    virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
private:
    void ApplySizes();
    TArray<TWeakObjectPtr<UMeshComponent>> FlowerTargets;
    TArray<TWeakObjectPtr<UMeshComponent>> LeafTargets;
    float LastFlower=-1.f, LastLeaf=-1.f;
};
