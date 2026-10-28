// Patch39: world-space collider volume for non-destructive 3DGS removal.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GSRemovalBoxActor.generated.h"

class UBoxComponent;
class FSceneInterface;

UCLASS(Blueprintable, meta=(DisplayName="GS Removal Box"))
class LCCCINEMATICGS_API AGSRemovalBoxActor : public AActor
{
    GENERATED_BODY()
public:
    AGSRemovalBoxActor();

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="GS Removal")
    TObjectPtr<UBoxComponent> RemovalBounds;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GS Removal", meta=(DisplayName="Enable GS removal"))
    bool bRemovalEnabled = true;

    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Destroyed() override;
#if WITH_EDITOR
    virtual void PostEditMove(bool bFinished) override;
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
private:
    void UpdateRenderBox(bool bForce=false);
    void RemoveRenderBox();
    FTransform LastTransform;
    FVector LastExtent = FVector::ZeroVector;
    bool bLastEnabled = false;
    bool bPublished = false;
    const FSceneInterface* LastScene = nullptr;
};
