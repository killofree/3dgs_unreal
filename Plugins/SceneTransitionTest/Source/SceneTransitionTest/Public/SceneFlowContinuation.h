#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SceneFlowDirector.h"
#include "SceneFlowContinuation.generated.h"

// Internal transport: the single authored director's slot list survives OpenLevel.
UCLASS()
class SCENETRANSITIONTEST_API USceneFlowContinuation : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    void Prepare(ASceneFlowDirector* Director, const TArray<FSceneFlowSlot>& Slots, int32 NextSlot, const FString& Map);
    void Cancel();
private:
    void OnMapLoaded(UWorld* World);
    UPROPERTY(Transient) TArray<FSceneFlowSlot> SavedSlots;
    UPROPERTY(Transient) TSubclassOf<ASceneFlowDirector> DirectorClass;
    UPROPERTY(Transient) TMap<FName, TSoftObjectPtr<AActor>> SavedBindings;
    TMap<int32, TSoftObjectPtr<AActor>> SavedTargets;
    int32 ResumeIndex = 0;
    FString Destination;
    FTransform DirectorTransform;
    bool bPending = false;
    FDelegateHandle LoadedHandle;
};
