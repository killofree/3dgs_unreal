#include "SceneFlowDirector.h"
#include "SceneTransitionSubsystem.h"
#include "SceneFlowContinuation.h"
#include "SceneFlowStartHandle.h"
#include "SceneEffectLibrary.h"
#include "LatentActions.h"
#if WITH_EDITOR
#include "Editor.h"
#include "Engine/Selection.h"
#endif
#include "Camera/PlayerCameraManager.h"
#include "Camera/CameraComponent.h"
#include "Components/AudioComponent.h"
#include "Components/SplineComponent.h"
#include "Components/SceneComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "UObject/UnrealType.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "Engine/Level.h"

namespace
{
void SetFlowVisibility(AActor* Actor, bool bVisible, TSet<AActor*>& Visited)
{
    if (!IsValid(Actor) || Visited.Contains(Actor)) return;
    Visited.Add(Actor);
    Actor->SetActorHiddenInGame(!bVisible);
    TInlineComponentArray<USceneComponent*> Components(Actor);
    for (auto* Component : Components) { Component->SetHiddenInGame(!bVisible); Component->SetVisibility(bVisible); }
    TArray<AActor*> Attached;
    Actor->GetAttachedActors(Attached);
    for (auto* Child : Attached) SetFlowVisibility(Child, bVisible, Visited);
    if (auto* Instance = Cast<ILevelInstanceInterface>(Actor))
        if (ULevel* Level = Instance->GetLoadedLevel())
            for (AActor* Child : Level->Actors) SetFlowVisibility(Child, bVisible, Visited);
}
FVector PlayerOriginAt(AActor* Actor, FVector Destination, FName Target)
{
    if (Target == TEXT("Player"))
        if (auto* Camera = Actor->FindComponentByClass<UCameraComponent>())
        {
            FVector Offset = Camera->GetComponentLocation() - Actor->GetActorLocation();
            Offset.Z = 0;
            Destination -= Offset;
        }
    return Destination;
}
}

ASceneFlowDirector::ASceneFlowDirector()
{
    PrimaryActorTick.bCanEverTick = true;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("DirectorRoot")));
}
void ASceneFlowDirector::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    RebuildSlotPaths();
#if WITH_EDITORONLY_DATA
    if (IsValid(StartHandle)) StartHandle->SetActorLocation(GetActorTransform().TransformPosition(StartPosition));
#endif
}
void ASceneFlowDirector::SaveStartPositionFromHandle(FVector WorldLocation)
{
    if (bUpdatingHandles) return;
    Modify();
    bUseStartPosition = true;
    StartPosition = GetActorTransform().InverseTransformPosition(WorldLocation);
    if (bLinkStartToPath) if (auto* Path = GetSlotPath(StartPathSlotIndex))
    {
        Path->Modify();
        Path->SetLocationAtSplinePoint(0, WorldLocation, ESplineCoordinateSpace::World);
    }
    MarkPackageDirty();
}
void ASceneFlowDirector::SaveSlotPositionFromHandle(FGuid SlotId, FTransform WorldTransform)
{
    if (bUpdatingHandles || bRunning) return;
    auto& Source = FlowAsset ? FlowAsset->Slots : Slots;
    for (auto& Slot : Source) if (Slot.PathId == SlotId)
    {
        Modify();
        if (FlowAsset) FlowAsset->Modify();
        if (Slot.Action == ESceneFlowAction::MoveToPosition) Slot.Destination = WorldTransform.GetLocation();
        else Slot.Placement = WorldTransform.GetRelativeTransform(GetActorTransform());
        if (FlowAsset) FlowAsset->MarkPackageDirty();
        MarkPackageDirty();
        return;
    }
}
AActor* ASceneFlowDirector::GetSlotPositionHandle(int32 Index) const
{
#if WITH_EDITORONLY_DATA
    const auto& Source = FlowAsset ? FlowAsset->Slots : Slots;
    if (Source.IsValidIndex(Index)) return SlotPositionHandles.FindRef(Source[Index].PathId);
#endif
    return nullptr;
}
void ASceneFlowDirector::RefreshPositionHandles()
{
#if WITH_EDITOR
    if (!GetWorld() || GetWorld()->IsGameWorld() || IsTemplate() || bUpdatingHandles) return;
    TGuardValue<bool> Guard(bUpdatingHandles, true);
    auto& Source = FlowAsset ? FlowAsset->Slots : Slots;
    TSet<FGuid> Used;
    for (int32 Index = 0; Index < Source.Num(); ++Index)
    {
        auto& Slot = Source[Index];
        Slot.PositionHandle = nullptr;
        const bool bPositionSlot = Slot.Action == ESceneFlowAction::MoveToPosition ||
            ((Slot.Action == ESceneFlowAction::PlaceAtMarker || Slot.Action == ESceneFlowAction::Spawn) && Slot.MarkerOrPath.IsNone());
        if (!Slot.bEnabled || !Slot.bShowPositionHandle || !bPositionSlot) continue;
        Used.Add(Slot.PathId);
        ASceneFlowStartHandle* Handle = SlotPositionHandles.FindRef(Slot.PathId);
        if (!IsValid(Handle) || Handle->Director != this)
        {
            FActorSpawnParameters Parameters;
            Parameters.OverrideLevel = GetLevel(); Parameters.Owner = this;
            Handle = GetWorld()->SpawnActor<ASceneFlowStartHandle>(Parameters);
            if (!Handle) continue;
            Handle->Director = this; Handle->SlotId = Slot.PathId;
            SlotPositionHandles.Add(Slot.PathId, Handle);
            Handle->SetFolderPath(TEXT("SceneFlow_EditorHelpers"));
        }
        Slot.PositionHandle = Handle;
        const FTransform Transform = Slot.Action == ESceneFlowAction::MoveToPosition ? FTransform(Slot.Destination) : Slot.Placement * GetActorTransform();
        Handle->SetActorTransform(Transform);
        Handle->SetActorLabel(FString::Printf(TEXT("SLOT_%02d_%s_위치"), Index + 1, *Slot.Name.ToString()));
    }
    for (auto It = SlotPositionHandles.CreateIterator(); It; ++It) if (!Used.Contains(It.Key()))
    {
        if (IsValid(It.Value()) && It.Value()->Director == this)
        {
            if (GEditor && GEditor->GetSelectedActors()->IsSelected(It.Value())) GEditor->SelectActor(It.Value(), false, true);
            GetWorld()->DestroyActor(It.Value());
        }
        It.RemoveCurrent();
    }
    if (bUseStartPosition)
    {
        if (!IsValid(StartHandle) || StartHandle->Director != this)
        {
            FActorSpawnParameters Parameters; Parameters.OverrideLevel = GetLevel(); Parameters.Owner = this;
            StartHandle = GetWorld()->SpawnActor<ASceneFlowStartHandle>(Parameters);
            if (StartHandle) { StartHandle->Director = this; StartHandle->SetActorLabel(TEXT("START_시작위치_이동하면디렉터에반영")); StartHandle->SetFolderPath(TEXT("SceneFlow_EditorHelpers")); }
        }
        if (StartHandle) { StartHandle->SetActorHiddenInGame(true); StartHandle->SetIsTemporarilyHiddenInEditor(false); StartHandle->SetActorLocation(GetActorTransform().TransformPosition(StartPosition)); }
    }
    else if (IsValid(StartHandle) && StartHandle->Director == this) StartHandle->SetIsTemporarilyHiddenInEditor(true);
#endif
}
void ASceneFlowDirector::EditStartPosition()
{
#if WITH_EDITOR
    if (!GetWorld() || GetWorld()->IsGameWorld()) return;
    bEditingStartHandle = true;
    EditingSlotId.Invalidate();
    if (!IsValid(StartHandle))
    {
        FActorSpawnParameters Parameters;
        Parameters.OverrideLevel = GetLevel();
        Parameters.Owner = this;
        StartHandle = GetWorld()->SpawnActor<ASceneFlowStartHandle>(GetActorTransform().TransformPosition(StartPosition), FRotator::ZeroRotator, Parameters);
        if (!StartHandle) return;
        StartHandle->Director = this;
        StartHandle->SetActorLabel(TEXT("START_시작위치_이동하면디렉터에반영"));
        StartHandle->SetFolderPath(TEXT("SceneFlow_EditorHelpers"));
    }
    Modify(); bUseStartPosition = true;
    StartHandle->SetActorLocation(GetActorTransform().TransformPosition(StartPosition));
    ApplyEditorSlotVisibility();
    if (GEditor) { GEditor->SelectNone(false, true); GEditor->SelectActor(StartHandle, true, true); }
    MarkPackageDirty();
#endif
}
void ASceneFlowDirector::ApplyEditorSlotVisibility()
{
#if WITH_EDITOR
    for (auto& Pair : SlotPositionHandles)
        if (IsValid(Pair.Value)) Pair.Value->SetIsTemporarilyHiddenInEditor(bShowOnlySelectedSlot && (bEditingStartHandle || Pair.Key != EditingSlotId));
    for (auto& Pair : SlotPaths)
        if (IsValid(Pair.Value)) Pair.Value->SetVisibility(!bShowOnlySelectedSlot || (!bEditingStartHandle && Pair.Key == EditingSlotId));
    if (IsValid(StartHandle)) StartHandle->SetIsTemporarilyHiddenInEditor(!bUseStartPosition || (bShowOnlySelectedSlot && !bEditingStartHandle));
#endif
}
bool ASceneFlowDirector::SelectSlotEditorHandle(int32 Index)
{
#if WITH_EDITOR
    if (!GEditor || !GetWorld() || GetWorld()->IsGameWorld() || bRunning) return false;
    RebuildSlotPaths();
    const auto& Source = FlowAsset ? FlowAsset->Slots : Slots;
    if (!Source.IsValidIndex(Index) || !Source[Index].bEnabled) return false;
    EditingSlotId = Source[Index].PathId;
    bEditingStartHandle = false;
    ApplyEditorSlotVisibility();
    if (Source[Index].Action == ESceneFlowAction::MovePath)
    {
        USplineComponent* Path = GetSlotPath(Index);
        if (!Path && !Source[Index].bUseDirectorPath)
            if (auto* External = ResolveTarget(Source[Index].MarkerOrPath)) Path = External->FindComponentByClass<USplineComponent>();
        if (!Path) return false;
        GEditor->SelectNone(false, true);
        GEditor->SelectActor(Path->GetOwner(), true, false);
        GEditor->SelectComponent(Path, true, true);
        GEditor->NoteSelectionChange();
        GEditor->RedrawLevelEditingViewports();
        return true;
    }
    if (AActor* Handle = GetSlotPositionHandle(Index))
    {
        GEditor->SelectNone(false, true);
        GEditor->SelectActor(Handle, true, true);
        GEditor->RedrawLevelEditingViewports();
        return true;
    }
#endif
    return false;
}
USplineComponent* ASceneFlowDirector::GetSlotPath(int32 Index) const
{
    const auto& Source = FlowAsset ? FlowAsset->Slots : Slots;
    if (!Source.IsValidIndex(Index)) return nullptr;
    const auto* Found = SlotPaths.Find(Source[Index].PathId);
    return Found ? Found->Get() : nullptr;
}
void ASceneFlowDirector::SetStartFromPath()
{
    if (auto* Path = GetSlotPath(StartPathSlotIndex))
    {
        Modify(); bUseStartPosition = true;
        StartPosition = GetActorTransform().InverseTransformPosition(Path->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World));
    }
}
void ASceneFlowDirector::RebuildSlotPaths()
{
    if (bUpdatingPaths || bRunning || IsTemplate()) return;
    TGuardValue<bool> Guard(bUpdatingPaths, true);
    auto& Source = FlowAsset ? FlowAsset->Slots : Slots;
    TSet<FGuid> Ids;
    for (auto& Slot : Source)
    {
        if (!Slot.PathId.IsValid() || Ids.Contains(Slot.PathId)) Slot.PathId = FGuid::NewGuid();
        Ids.Add(Slot.PathId);
    }
    TSet<FGuid> Used;
    for (auto& Slot : Source)
    {
        if (!Slot.bEnabled || Slot.Action != ESceneFlowAction::MovePath || !Slot.bUseDirectorPath) continue;
        if (!Slot.PathId.IsValid() || Used.Contains(Slot.PathId)) Slot.PathId = FGuid::NewGuid();
        Used.Add(Slot.PathId);
        USplineComponent* Path = SlotPaths.FindRef(Slot.PathId);
        if (!IsValid(Path))
        {
            FString Label = Slot.Name.ToString();
            for (TCHAR& Character : Label) if (!FChar::IsAlnum(Character) && Character != TEXT('_')) Character = TEXT('_');
            const FName ComponentName = MakeUniqueObjectName(this, USplineComponent::StaticClass(), FName(*FString::Printf(TEXT("Path_%s"), *Label)));
            Path = NewObject<USplineComponent>(this, ComponentName, RF_Transactional);
            Path->SetupAttachment(GetRootComponent());
            AddInstanceComponent(Path);
            Path->RegisterComponent();
            SlotPaths.Add(Slot.PathId, Path);
        }
        if (Slot.PathShape.Position.Points.IsEmpty()) Slot.PathShape = Path->GetSplineCurves();
        Path->SetSpline(Slot.PathShape);
        Path->UpdateSpline();
#if WITH_EDITORONLY_DATA
        Path->bModifiedByConstructionScript = false;
        Path->bSplineHasBeenEdited = true;
#endif
        Path->GetOnSplineUpdated().RemoveAll(this);
        const FGuid Id = Slot.PathId;
        TWeakObjectPtr<USplineComponent> WeakPath(Path);
        Path->GetOnSplineUpdated().AddWeakLambda(this, [this, Id, WeakPath]()
        {
            if (bUpdatingPaths || bRunning || !WeakPath.IsValid()) return;
            auto& Current = FlowAsset ? FlowAsset->Slots : Slots;
            for (auto& Item : Current) if (Item.PathId == Id)
            {
                Modify();
                if (FlowAsset) FlowAsset->Modify();
                Item.PathShape = WeakPath->GetSplineCurves();
                break;
            }
        });
    }
    for (auto It = SlotPaths.CreateIterator(); It; ++It)
        if (!Used.Contains(It.Key()))
        {
            if (IsValid(It.Value())) { RemoveInstanceComponent(It.Value()); It.Value()->DestroyComponent(); }
            It.RemoveCurrent();
        }
    RefreshPositionHandles();
    ApplyEditorSlotVisibility();
}
void ASceneFlowDirector::BeginPlay() { Super::BeginPlay(); if (bAutoStart && !bRunning) StartFlow(); }
void ASceneFlowDirector::EndPlay(const EEndPlayReason::Type Reason) { CleanupFlow(!bHandingOff); Super::EndPlay(Reason); }

AActor* ASceneFlowDirector::ResolveTarget(FName Name) const
{
    if (Name == TEXT("Player")) return UGameplayStatics::GetPlayerPawn(this, 0);
    if (const auto* Found = Spawned.Find(Name)) return IsValid(*Found) ? Found->Get() : nullptr;
    if (const auto* Found = Bindings.Find(Name)) return IsValid(*Found) ? Found->Get() : nullptr;
    AActor* Result = nullptr;
    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
        if (It->ActorHasTag(Name)) { if (Result) return nullptr; Result = *It; }
    return Result;
}

void ASceneFlowDirector::StartFlow(int32 StartIndex)
{
    StopFlow();
    RebuildSlotPaths();
    RunningSlots = FlowAsset ? FlowAsset->Slots : Slots;
    if (!RunningSlots.IsValidIndex(StartIndex)) { Fail(TEXT("Start index is outside the slot list.")); return; }
    NextIndex = StartIndex;
    bRunning = true;
    LastError.Reset();
    CurrentSlotIndex = -1;
    CurrentSlotName = NAME_None;
    PlaybackStatus = TEXT("시작 준비");
    bStartPlaced = !bUseStartPosition;
    if (auto* Transition = GetGameInstance()->GetSubsystem<USceneTransitionSubsystem>())
    {
        Transition->HoldInitialFadeForDirector(bStartWithBlack);
        Transition->StartTest();
        Transition->SetFlowControl(true);
        bWaitingForStartup = Transition->IsTransitioning();
    }
    if (bStartWithBlack)
        if (auto* Camera = UGameplayStatics::GetPlayerCameraManager(this, 0)) Camera->SetManualCameraFade(1, FLinearColor::Black, true);
}

namespace
{
class FSceneGraphStepAction : public FPendingLatentAction
{
    TWeakObjectPtr<ASceneFlowDirector> Director;
    uint64 Generation, Step;
    FName Function;
    int32 Link;
    FWeakObjectPtr Callback;
public:
    FSceneGraphStepAction(ASceneFlowDirector* InDirector, uint64 InGeneration, uint64 InStep, const FLatentActionInfo& Info)
        : Director(InDirector), Generation(InGeneration), Step(InStep), Function(Info.ExecutionFunction), Link(Info.Linkage), Callback(Info.CallbackTarget) {}
    virtual void UpdateOperation(FLatentResponse& Response) override
    {
        const bool Cancelled = !Director.IsValid() || Director->GetGraphGeneration() != Generation;
        if (Cancelled) { Response.DoneIf(true); return; }
        Response.FinishAndTriggerIf(Director->GetGraphCompletedStep() >= Step, Function, Link, Callback);
    }
};
}
void ASceneFlowDirector::BeginSceneGraph()
{
    StartFlow(0);
    if (!bRunning) return;
    bGraphMode = true;
    NextIndex = RunningSlots.Num();
    GraphStep = GraphCompletedStep = 0;
}
void ASceneFlowDirector::RunSceneStep(FName StepName, float DurationOverride, FLatentActionInfo Info)
{
    if (!bGraphMode || !bRunning) { Fail(TEXT("Begin Scene Graph must run before Run Scene Step.")); return; }
    auto& Manager = GetWorld()->GetLatentActionManager();
    if (Manager.FindExistingAction<FSceneGraphStepAction>(Info.CallbackTarget, Info.UUID)) return;
    if (!Jobs.IsEmpty()) { Fail(TEXT("A scene step is still running. Connect steps through the completion execution pin.")); return; }
    int32 Index = INDEX_NONE;
    for (int32 I = 0; I < RunningSlots.Num(); ++I) if (RunningSlots[I].Name == StepName)
    { if (Index != INDEX_NONE) { Fail(TEXT("Scene step names must be unique.")); return; } Index = I; }
    if (Index == INDEX_NONE) { Fail(FString::Printf(TEXT("Unknown scene step: %s"), *StepName.ToString())); return; }
    if (RunningSlots[Index].Action == ESceneFlowAction::ChangeScene)
    { Fail(TEXT("Use Travel To Map for graph scene changes; the destination director begins its own graph. Slot continuation belongs to StartFlow mode.")); return; }
    const uint64 Step = ++GraphStep;
    if (DurationOverride >= 0) RunningSlots[Index].Duration = DurationOverride;
    if (RunningSlots[Index].bEnabled) { FJob Job; Job.Index = Index; Jobs.Add(Job); }
    else GraphCompletedStep = Step;
    Manager.AddNewAction(Info.CallbackTarget, Info.UUID, new FSceneGraphStepAction(this, RunVersion, Step, Info));
}
void ASceneFlowDirector::FinishSceneGraph()
{
    if (!bGraphMode) return;
    bGraphMode = false;
    bRunning = false;
    PlaybackStatus = TEXT("완료");
    if (GetGameInstance()) if (auto* Transition = GetGameInstance()->GetSubsystem<USceneTransitionSubsystem>()) Transition->SetFlowControl(false);
    OnFinished.Broadcast();
}

void ASceneFlowDirector::StopFlow()
{
    CleanupFlow(true);
}
void ASceneFlowDirector::CleanupFlow(bool bCancelContinuation)
{
    ++RunVersion;
    bRunning = false;
    bGraphMode = false;
    PlaybackStatus = TEXT("정지");
    Jobs.Reset();
    for (auto& Pair : AudioChannels) if (IsValid(Pair.Value)) Pair.Value->Stop();
    AudioChannels.Reset();
    for (auto& Pair : Spawned) if (IsValid(Pair.Value)) Pair.Value->Destroy();
    Spawned.Reset();
    Events.Reset();
    if (bCancelContinuation && GetGameInstance())
        if (auto* Continuation = GetGameInstance()->GetSubsystem<USceneFlowContinuation>()) Continuation->Cancel();
    bHandingOff = false;
    if (GetGameInstance()) if (auto* Transition = GetGameInstance()->GetSubsystem<USceneTransitionSubsystem>())
    {
        Transition->SetFlowControl(false);
        Transition->HoldInitialFadeForDirector(false);
    }
}

void ASceneFlowDirector::SendEvent(FName Event) { ++Events.FindOrAdd(Event); }
void ASceneFlowDirector::CreateFirstCutTemplate()
{
    if (bRunning || !Slots.IsEmpty()) return;
    Modify();
    auto Add = [this](FName Name, ESceneFlowAction Action) -> FSceneFlowSlot&
    { FSceneFlowSlot& Slot = Slots.AddDefaulted_GetRef(); Slot.Name = Name; Slot.Action = Action; return Slot; };
    bStartWithBlack = true;
    auto& Mesh = Add(TEXT("01_대체메쉬_끄기"), ESceneFlowAction::SetVisible);
    Mesh.Target = TEXT("ReplacementMesh"); Mesh.bValue = false; Mesh.bIncludeComponents = true;
    auto& Box = Add(TEXT("02_3DGS자르기_끄기"), ESceneFlowAction::SetBool);
    Box.Target = TEXT("RemovalBox"); Box.PropertyOrEvent = TEXT("bRemovalEnabled"); Box.bValue = false;
    auto& Pause = Add(TEXT("03_검정화면_안정화대기"), ESceneFlowAction::Wait); Pause.Duration = 1;
    auto& Reveal = Add(TEXT("04_입구에서_밝아짐"), ESceneFlowAction::FadeIn);
    Reveal.Duration = 2;
    auto& Ambient = Add(TEXT("05_환경음_소리지정후사용"), ESceneFlowAction::PlaySound);
    Ambient.bEnabled = false; Ambient.bWaitForCompletion = false; Ambient.AudioChannel = TEXT("Environment"); Ambient.Volume = .6f;
    auto& Narration = Add(TEXT("06_첫내레이션_소리지정후사용"), ESceneFlowAction::PlaySound);
    Narration.bEnabled = false; Narration.bWaitForCompletion = false; Narration.AudioChannel = TEXT("Narration");
    StartPathSlotIndex = Slots.Num();
    auto& Move = Add(TEXT("07_입구에서마당_60초이동"), ESceneFlowAction::MovePath);
    Move.Target = TEXT("Player"); Move.Duration = 60;
    auto& Observe = Add(TEXT("08_마당에서_60초감상"), ESceneFlowAction::Wait); Observe.Duration = 60;
    RebuildSlotPaths();
    SetStartFromPath();
}
void ASceneFlowDirector::Fail(const FString& Message)
{
    UE_LOG(LogTemp, Error, TEXT("SceneFlow %s: %s"), *GetName(), *Message);
    StopFlow();
    LastError = Message;
    PlaybackStatus = TEXT("오류로 정지");
    OnError.Broadcast(Message);
}

bool ASceneFlowDirector::UpdateJob(FJob& Job, float DeltaSeconds)
{
    const uint64 Version = RunVersion;
    const FSceneFlowSlot Slot = RunningSlots[Job.Index];
    Job.Time += DeltaSeconds;
    if (Job.Time < FMath::Max(0.f, Slot.Delay)) return false;
    const float Elapsed = Job.Time - FMath::Max(0.f, Slot.Delay);
    const float Alpha = Slot.Duration <= 0 ? 1.f : FMath::Clamp(Elapsed / Slot.Duration, 0.f, 1.f);
    if (!Job.bStarted)
    {
        Job.bStarted = true;
        CurrentSlotIndex = Job.Index;
        CurrentSlotName = Slot.Name;
        PlaybackStatus = Slot.Action == ESceneFlowAction::WaitEvent ? TEXT("이벤트 대기") : TEXT("실행 중");
        UE_LOG(LogTemp, Display, TEXT("SceneFlow slot started: %d %s Action=%s"), Job.Index, *Slot.Name.ToString(), *StaticEnum<ESceneFlowAction>()->GetNameStringByValue(int64(Slot.Action)));
        Job.EventVersion = Events.FindRef(Slot.PropertyOrEvent);
        Job.Actor = IsValid(Slot.TargetActor) ? Slot.TargetActor.Get() : ResolveTarget(Slot.Target);
        Job.Reference = ResolveTarget(Slot.MarkerOrPath);
        if (Slot.Action == ESceneFlowAction::MovePath)
            Job.Spline = Slot.bUseDirectorPath ? GetSlotPath(Job.Index) : (Job.Reference.IsValid() ? Job.Reference->FindComponentByClass<USplineComponent>() : nullptr);
        AActor* Actor = Job.Actor.Get();
        const bool bNeedsActor = Slot.Action == ESceneFlowAction::MovePath || Slot.Action == ESceneFlowAction::PlaceAtMarker ||
            Slot.Action == ESceneFlowAction::SetVisible || Slot.Action == ESceneFlowAction::SetBool || Slot.Action == ESceneFlowAction::SetFloat || Slot.Action == ESceneFlowAction::Rotate || Slot.Action == ESceneFlowAction::MoveToPosition;
        if (bNeedsActor && !Actor) { Fail(FString::Printf(TEXT("Slot %d: missing or ambiguous target %s"), Job.Index, *Slot.Target.ToString())); return true; }
        if (((Slot.Action == ESceneFlowAction::MovePath && !Slot.bUseDirectorPath) || ((Slot.Action == ESceneFlowAction::PlaceAtMarker || Slot.Action == ESceneFlowAction::Spawn) && !Slot.MarkerOrPath.IsNone())) && !Job.Reference.IsValid())
        { Fail(TEXT("Missing marker/path binding.")); return true; }
        if (Slot.Action == ESceneFlowAction::Rotate) Job.InitialRotation = Actor->GetActorRotation();
        if (Slot.Action == ESceneFlowAction::MoveToPosition) Job.InitialPosition = Actor->GetActorLocation();
        if (Slot.Action == ESceneFlowAction::SetFloat)
        {
            FString Error;
            if (!USceneEffectLibrary::ReadFloat(Actor, Slot.PropertyOrEvent.ToString(), Job.InitialValue, Error)) { Fail(Error); return true; }
        }
        if (Slot.Action == ESceneFlowAction::SetBool)
        {
            FString Error;
            if (!USceneEffectLibrary::WriteBool(Actor, Slot.PropertyOrEvent.ToString(), Slot.bValue, Error)) { Fail(Error); return true; }
        }
        if (Slot.Action == ESceneFlowAction::SetVisible)
        {
            Actor->SetActorHiddenInGame(!Slot.bValue);
            if (Slot.bIncludeComponents)
            {
                TSet<AActor*> Visited;
                SetFlowVisibility(Actor, Slot.bValue, Visited);
            }
        }
        if (Slot.Action == ESceneFlowAction::PlaceAtMarker)
        {
            const FVector Position = Job.Reference.IsValid() ? Job.Reference->GetActorLocation() : (Slot.Placement * GetActorTransform()).GetLocation();
            Actor->SetActorLocation(PlayerOriginAt(Actor, Position, Slot.Target), false, nullptr, ETeleportType::TeleportPhysics);
        }
        if (Slot.Action == ESceneFlowAction::Spawn)
        {
            if (!Slot.SpawnClass || Slot.Target.IsNone() || Spawned.Contains(Slot.Target)) { Fail(TEXT("Spawn requires a class and unused target binding.")); return true; }
            const FTransform Transform = Job.Reference.IsValid() ? Job.Reference->GetActorTransform() : Slot.Placement * GetActorTransform();
            AActor* NewActor = GetWorld()->SpawnActor<AActor>(Slot.SpawnClass, Transform);
            if (!NewActor) { Fail(TEXT("Spawn failed.")); return true; }
            Spawned.Add(Slot.Target, NewActor);
        }
        if (Slot.Action == ESceneFlowAction::DestroySpawned)
        {
            if (auto* Owned = Spawned.Find(Slot.Target)) { if (IsValid(*Owned)) (*Owned)->Destroy(); Spawned.Remove(Slot.Target); }
        }
        if (Slot.Action == ESceneFlowAction::PlaySound)
        {
            if (!Slot.Sound) { Fail(TEXT("PlaySound requires a sound asset.")); return true; }
            if (auto* Old = AudioChannels.Find(Slot.AudioChannel)) if (IsValid(*Old)) (*Old)->Stop();
            UAudioComponent* Audio = nullptr;
            if (Slot.bSpatialSound)
            {
                if (!Actor) { Fail(TEXT("Spatial sound requires a target.")); return true; }
                Audio = UGameplayStatics::SpawnSoundAtLocation(this, Slot.Sound, Actor->GetActorLocation(), FRotator::ZeroRotator, Slot.Volume);
            }
            else Audio = UGameplayStatics::SpawnSound2D(this, Slot.Sound, Slot.Volume);
            if (!Audio) { Fail(TEXT("Audio playback could not start.")); return true; }
            AudioChannels.Add(Slot.AudioChannel, Audio);
            Job.Audio = Audio;
        }
        if (Slot.Action == ESceneFlowAction::StopSound)
            if (auto* Audio = AudioChannels.Find(Slot.AudioChannel)) if (IsValid(*Audio)) (*Audio)->FadeOut(FMath::Max(0.f, Slot.Duration), 0);
        OnSlotStarted.Broadcast(Job.Index, Slot.Name);
        if (!bRunning || Version != RunVersion) return true;
    }
    AActor* Actor = Job.Actor.Get();
    if (Slot.Action == ESceneFlowAction::WaitEvent)
        return Events.FindRef(Slot.PropertyOrEvent) > Job.EventVersion;
    if (Slot.Action == ESceneFlowAction::PlaySound)
        return !Slot.bWaitForCompletion || !Job.Audio.IsValid() || !Job.Audio->IsPlaying();
    if (Slot.Action == ESceneFlowAction::ChangeScene)
    {
        auto* Transition = GetGameInstance()->GetSubsystem<USceneTransitionSubsystem>();
        auto* Continuation = GetGameInstance()->GetSubsystem<USceneFlowContinuation>();
        const FString Map = Slot.DestinationMap.GetLongPackageName();
        if (!Transition || !Continuation || Map.IsEmpty()) { Fail(TEXT("ChangeScene requires a destination map.")); return true; }
        Continuation->Prepare(this, RunningSlots, Job.Index + 1, Map);
        if (!Transition->TravelToMap(Map, Slot.Duration, Slot.FadeInSeconds, Slot.FadeColor))
        { Continuation->Cancel(); Fail(TEXT("ChangeScene rejected: destination missing or transition is not ready.")); return true; }
        bHandingOff = true;
        bRunning = false;
        return true;
    }
    if (Slot.Action == ESceneFlowAction::MoveToPosition)
    {
        if (!Actor) { Fail(TEXT("Movement target was destroyed.")); return true; }
        const float Progress = Slot.bEaseMovement ? FMath::InterpEaseInOut(0.f, 1.f, Alpha, 2.f) : Alpha;
        Actor->SetActorLocation(FMath::Lerp(Job.InitialPosition, Slot.Destination, Progress), false, nullptr, ETeleportType::TeleportPhysics);
    }
    if (Slot.Action == ESceneFlowAction::MovePath)
    {
        USplineComponent* Spline = Job.Spline.Get();
        if (!Actor || !Spline) { Fail(TEXT("MovePath needs a valid actor and a spline component.")); return true; }
        const float Progress = Slot.bEaseMovement ? FMath::InterpEaseInOut(0.f, 1.f, Alpha, 2.f) : Alpha;
        Actor->SetActorLocation(PlayerOriginAt(Actor, Spline->GetLocationAtDistanceAlongSpline(Spline->GetSplineLength() * Progress, ESplineCoordinateSpace::World), Slot.Target), false, nullptr, ETeleportType::TeleportPhysics);
    }
    if (Slot.Action == ESceneFlowAction::SetFloat)
    {
        if (!Actor) { Fail(TEXT("Float target was destroyed.")); return true; }
        FString Error;
        if (!USceneEffectLibrary::WriteFloat(Actor, Slot.PropertyOrEvent.ToString(), FMath::Lerp(Job.InitialValue, Slot.Value, Alpha), Error)) { Fail(Error); return true; }
    }
    if (Slot.Action == ESceneFlowAction::Rotate)
    {
        if (!Actor) { Fail(TEXT("Rotation target was destroyed.")); return true; }
        Actor->SetActorRotation(FQuat::Slerp(Job.InitialRotation.Quaternion(), Slot.Rotation.Quaternion(), Alpha));
    }
    if (Slot.Action == ESceneFlowAction::FadeScreen)
        if (auto* Camera = UGameplayStatics::GetPlayerCameraManager(this, 0)) Camera->SetManualCameraFade(FMath::Lerp(Slot.Volume, Slot.Value, Alpha), Slot.FadeColor, true);
    if (Slot.Action == ESceneFlowAction::FadeIn || Slot.Action == ESceneFlowAction::FadeOut)
        if (auto* Camera = UGameplayStatics::GetPlayerCameraManager(this, 0)) Camera->SetManualCameraFade(Slot.Action == ESceneFlowAction::FadeOut ? Alpha : 1.f - Alpha, Slot.FadeColor, true);
    switch (Slot.Action)
    {
    case ESceneFlowAction::SetBool: case ESceneFlowAction::SetVisible: case ESceneFlowAction::PlaceAtMarker:
    case ESceneFlowAction::Spawn: case ESceneFlowAction::DestroySpawned: case ESceneFlowAction::BlueprintEvent: return true;
    default: return Alpha >= 1;
    }
}

void ASceneFlowDirector::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!bRunning) return;
    if (!bStartPlaced)
    {
        AActor* Player = ResolveTarget(TEXT("Player"));
        if (!Player) return;
        Player->SetActorLocation(PlayerOriginAt(Player, GetActorTransform().TransformPosition(StartPosition), TEXT("Player")), false, nullptr, ETeleportType::TeleportPhysics);
        bStartPlaced = true;
    }
    if (bWaitingForStartup)
    {
        if (auto* Transition = GetGameInstance()->GetSubsystem<USceneTransitionSubsystem>()) if (Transition->IsTransitioning()) return;
        bWaitingForStartup = false;
    }
    // Tick a local job copy so Blueprint callbacks may safely stop/restart the director.
    TArray<FJob> Previous = MoveTemp(Jobs);
    const uint64 Version = RunVersion;
    bool bBlocked = false;
    for (FJob& Job : Previous)
    {
        const bool bDone = UpdateJob(Job, DeltaSeconds);
        if (!bRunning || Version != RunVersion) return;
        if (bDone)
        {
            UE_LOG(LogTemp, Display, TEXT("SceneFlow slot completed: %d %s"), Job.Index, *RunningSlots[Job.Index].Name.ToString());
            OnSlotCompleted.Broadcast(Job.Index, RunningSlots[Job.Index].Name);
        }
        else { bBlocked |= RunningSlots[Job.Index].bWaitForCompletion; Jobs.Add(Job); }
        if (!bRunning || Version != RunVersion) return;
    }
    if (!bBlocked && NextIndex < RunningSlots.Num())
    {
        // Finish world-bound concurrent jobs before leaving the level.
        if (RunningSlots[NextIndex].Action == ESceneFlowAction::ChangeScene && !Jobs.IsEmpty()) return;
        const int32 Index = NextIndex++;
        if (RunningSlots[Index].bEnabled) { FJob Job; Job.Index = Index; Jobs.Add(Job); }
    }
    if (bGraphMode && Jobs.IsEmpty()) GraphCompletedStep = GraphStep;
    if (!bGraphMode && NextIndex >= RunningSlots.Num() && Jobs.IsEmpty())
    {
        bRunning = false;
        PlaybackStatus = TEXT("완료");
        if (auto* Transition = GetGameInstance()->GetSubsystem<USceneTransitionSubsystem>()) Transition->SetFlowControl(false);
        OnFinished.Broadcast();
    }
}
