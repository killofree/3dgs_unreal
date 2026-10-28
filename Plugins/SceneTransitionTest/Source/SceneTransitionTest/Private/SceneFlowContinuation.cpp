#include "SceneFlowContinuation.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/PackageName.h"

void USceneFlowContinuation::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    LoadedHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &ThisClass::OnMapLoaded);
}
void USceneFlowContinuation::Deinitialize()
{
    FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(LoadedHandle);
    Cancel();
    Super::Deinitialize();
}
void USceneFlowContinuation::Cancel()
{
    bPending = false; SavedSlots.Reset(); SavedBindings.Reset(); SavedTargets.Reset(); DirectorClass = nullptr;
}
void USceneFlowContinuation::Prepare(ASceneFlowDirector* Director, const TArray<FSceneFlowSlot>& Slots, int32 NextSlot, const FString& Map)
{
    Cancel();
    SavedSlots = Slots;
    DirectorClass = Director->GetClass();
    DirectorTransform = Director->GetActorTransform();
    for (const auto& Pair : Director->Bindings) SavedBindings.Add(Pair.Key, Pair.Value.Get());
    // Never keep old-world actors alive during travel.
    for (int32 Index = 0; Index < SavedSlots.Num(); ++Index)
    {
        if (SavedSlots[Index].TargetActor) SavedTargets.Add(Index, SavedSlots[Index].TargetActor.Get());
        SavedSlots[Index].TargetActor = nullptr;
    }
    ResumeIndex = NextSlot; Destination = Map; bPending = true;
}
void USceneFlowContinuation::OnMapLoaded(UWorld* World)
{
    if (!bPending || !World || World->GetGameInstance() != GetGameInstance()) return;
    if (UGameplayStatics::GetCurrentLevelName(World, true) != FPackageName::GetShortName(Destination)) { Cancel(); return; }
    bPending = false;
    if (!SavedSlots.IsValidIndex(ResumeIndex)) { Cancel(); return; }
    ASceneFlowDirector* Director = World->SpawnActorDeferred<ASceneFlowDirector>(DirectorClass, DirectorTransform);
    if (!Director) { Cancel(); return; }
    Director->bAutoStart = false;
    Director->bUseStartPosition = false;
    Director->bStartWithBlack = false;
    Director->FlowAsset = nullptr;
    Director->Slots = SavedSlots;
    for (const auto& Pair : SavedBindings) if (Pair.Value.IsValid()) Director->Bindings.Add(Pair.Key, Pair.Value.Get());
    for (const auto& Pair : SavedTargets) if (Pair.Value.IsValid()) Director->Slots[Pair.Key].TargetActor = Pair.Value.Get();
    const int32 Index = ResumeIndex;
    Cancel();
    Director->FinishSpawning(DirectorTransform);
    Director->StartFlow(Index);
}
