#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SceneSeasonController.generated.h"
class ACherrySeasonTreeActor;
class UMeshComponent;

/** Bind one controller to Sequencer or SceneFlow SetFloat tracks. */
UCLASS(Blueprintable, meta=(DisplayName="Season and Sun Controller", PrioritizeCategories="Unified Controls"))
class SCENEENVIRONMENT_API ASceneSeasonController : public AActor
{
    GENERATED_BODY()
public:
    ASceneSeasonController();
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, BlueprintSetter=SetFlowerSize, Category="Unified Controls", meta=(ClampMin="0",ClampMax="2")) float FlowerSize=1;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, BlueprintSetter=SetLeafSize, Category="Unified Controls", meta=(ClampMin="0",ClampMax="2")) float LeafSize=1;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, BlueprintSetter=SetSunSharpness, Category="Unified Controls", meta=(ClampMin="0",ClampMax="1", ToolTip="0: soft sun, 1: full mesh emission, halo and rays")) float SunSharpness=1;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Targets") TSubclassOf<ACherrySeasonTreeActor> TargetTreeClass;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Targets") FName TreeTag;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Targets") TObjectPtr<AActor> SunActor;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Targets") int32 TreeCount=0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Targets") int32 SunMeshCount=0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sun Range", meta=(ClampMin="0")) float SoftEmission=10;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sun Range", meta=(ClampMin="0")) float FullEmission=80;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sun Range", meta=(ClampMin="0")) float FullGlow=8;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sun Range", meta=(ClampMin="0")) float FullRays=.18f;
    UFUNCTION(BlueprintCallable, Category="Unified Controls") void SetFlowerSize(float Value);
    UFUNCTION(BlueprintCallable, Category="Unified Controls") void SetLeafSize(float Value);
    UFUNCTION(BlueprintCallable, Category="Unified Controls") void SetSunSharpness(float Value);
    UFUNCTION(BlueprintCallable, CallInEditor, Category="Unified Controls") void RefreshTargets();
    UFUNCTION(BlueprintCallable, Category="Unified Controls") void ApplyControls();
    virtual void Tick(float DeltaSeconds) override;
    virtual void BeginPlay() override;
    virtual void OnConstruction(const FTransform& Transform) override;
#if WITH_EDITOR
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
    virtual void PreEditChange(FProperty* Property) override;
    virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
private:
    TArray<TWeakObjectPtr<ACherrySeasonTreeActor>> Trees;
    TArray<TWeakObjectPtr<UMeshComponent>> SunMeshes;
    float RefreshElapsed=0;
    FVector LastValues=FVector(-100);
    FVector LastSunRange=FVector(-100);
    float LastSoft=-100;
};
