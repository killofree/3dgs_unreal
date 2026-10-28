#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SceneFlowDirector.h"
#include "SceneFlowPath.h"
#include "SceneFlowStartHandle.h"
#include "Components/SplineComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "UObject/Script.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneFlowTimingTest, "SceneFlow.TimedMovementAndEventGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSceneFlowTimingTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->SetGameInstance(NewObject<UGameInstance>(World));
    ASceneFlowDirector* Director = World->SpawnActor<ASceneFlowDirector>();
    Director->bStartWithBlack = false; // Timing test has no player camera/world context.
    ASceneFlowMarker* Moving = World->SpawnActor<ASceneFlowMarker>();
    ASceneFlowPath* Path = World->SpawnActor<ASceneFlowPath>();
    Path->Path->ClearSplinePoints();
    Path->Path->AddSplinePoint(FVector::ZeroVector, ESplineCoordinateSpace::World);
    Path->Path->AddSplinePoint(FVector(100, 0, 0), ESplineCoordinateSpace::World);
    Moving->SetActorRotation(FRotator(0, 37, 0));
    Director->Bindings.Add(TEXT("Moving"), Moving);
    Director->Bindings.Add(TEXT("Path"), Path);
    FSceneFlowSlot Move;
    Move.Action = ESceneFlowAction::MovePath; Move.Target = TEXT("Moving"); Move.MarkerOrPath = TEXT("Path"); Move.Duration = 2; Move.bUseDirectorPath = false;
    Director->Slots.Add(Move);
    FSceneFlowSlot Gate;
    Gate.Action = ESceneFlowAction::WaitEvent; Gate.PropertyOrEvent = TEXT("ControllerSweep");
    Director->Slots.Add(Gate);
    Director->StartFlow();
    Director->Tick(0); // enqueue move
    Director->Tick(1);
    TestTrue(TEXT("Halfway after half the requested duration"), Moving->GetActorLocation().Equals(FVector(50, 0, 0), 0.01));
    TestTrue(TEXT("Movement preserves viewpoint rotation"), FMath::IsNearlyEqual(Moving->GetActorRotation().Yaw, 37.f));
    Director->Tick(1); // move completes and event gate is queued
    Director->SendEvent(TEXT("ControllerSweep")); // earlier events must not open a new gate
    Director->Tick(0);
    Director->Tick(20);
    TestTrue(TEXT("Event gate does not advance with time or an earlier event"), Director->bRunning);
    Director->SendEvent(TEXT("ControllerSweep"));
    Director->Tick(0);
    TestFalse(TEXT("Controller event completes flow"), Director->bRunning);
    TestTrue(TEXT("Movement reaches endpoint at requested duration"), Moving->GetActorLocation().Equals(FVector(100, 0, 0), 0.01));
    Director->StartFlow();
    Director->Tick(0);
    Director->Tick(0.5f);
    Director->StopFlow();
    const FVector Stopped = Moving->GetActorLocation();
    Director->Tick(10);
    TestTrue(TEXT("Stopped flow no longer moves actor"), Moving->GetActorLocation().Equals(Stopped));
    Director->Slots.Reset();
    FSceneFlowSlot Disabled;
    Disabled.Action = ESceneFlowAction::SetVisible; Disabled.Target = TEXT("Moving"); Disabled.bEnabled = false; Disabled.bValue = false;
    Director->Slots.Add(Disabled);
    FSceneFlowSlot DirectMove;
    DirectMove.Action = ESceneFlowAction::MoveToPosition; DirectMove.TargetActor = Moving;
    DirectMove.Destination = FVector(200, 100, 0); DirectMove.Duration = 2;
    Director->Slots.Add(DirectMove);
    Director->StartFlow();
    Director->Tick(0); // skip disabled slot
    Director->Tick(0); // queue direct move
    Director->Tick(2);
    TestFalse(TEXT("Disabled visibility slot does not hide the target"), Moving->IsHidden());
    TestTrue(TEXT("Director moves to coordinates without a marker or path actor"), Moving->GetActorLocation().Equals(DirectMove.Destination, 0.01));
    Director->Slots.Reset();
    Move.bUseDirectorPath = true; Move.MarkerOrPath = NAME_None;
    Director->Slots.Add(Move);
    Director->RebuildSlotPaths();
    USplineComponent* InternalPath = Director->GetSlotPath(0);
    TestNotNull(TEXT("A path slot creates a spline owned by the director"), InternalPath);
    if (InternalPath)
    {
        InternalPath->SetLocationAtSplinePoint(1, FVector(400, 200, 0), ESplineCoordinateSpace::Local);
        Director->RebuildSlotPaths();
        Director->StartFlow(); Director->Tick(0); Director->Tick(2);
        TestTrue(TEXT("Edited internal path survives rebuild and drives movement"), Moving->GetActorLocation().Equals(FVector(400, 200, 0), 0.01));
        Director->StartPathSlotIndex = 0;
        Director->SaveStartPositionFromHandle(FVector(20, 30, 40));
        TestTrue(TEXT("Moving the start handle saves the director start point"), Director->StartPosition.Equals(FVector(20, 30, 40), .01));
        TestTrue(TEXT("Moving start point also moves the linked path's first point"), InternalPath->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World).Equals(FVector(20, 30, 40), .01));
        Director->Slots[0].bEnabled = false;
        Director->RebuildSlotPaths();
        TestNull(TEXT("Disabled path removes its generated spline"), Director->GetSlotPath(0));
    }
    World->DestroyWorld(false);
    GEngine->DestroyWorldContext(World);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneFlowHandleTest, "SceneFlow.EditorSlotHandles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSceneFlowHandleTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    GEngine->CreateNewWorldContext(EWorldType::EditorPreview).SetCurrentWorld(World);
    ASceneFlowDirector* Director = World->SpawnActor<ASceneFlowDirector>();
    Director->SetActorLocation(FVector(100, 200, 0));
    FSceneFlowSlot Move; Move.Name = TEXT("Move"); Move.Action = ESceneFlowAction::MoveToPosition; Move.Destination = FVector(500, 600, 0);
    FSceneFlowSlot Place; Place.Name = TEXT("Place"); Place.Action = ESceneFlowAction::PlaceAtMarker; Place.Placement.SetLocation(FVector(30, 40, 0));
    Director->Slots = {Move, Place};
    Director->RebuildSlotPaths();
    auto* MoveHandle = Cast<ASceneFlowStartHandle>(Director->GetSlotPositionHandle(0));
    auto* PlaceHandle = Cast<ASceneFlowStartHandle>(Director->GetSlotPositionHandle(1));
    TestNotNull(TEXT("Position slot automatically creates its helper"), MoveHandle);
    TestNotNull(TEXT("Placement slot automatically creates its helper"), PlaceHandle);
    if (MoveHandle && PlaceHandle)
    {
        TestTrue(TEXT("Placement helper uses director-relative coordinates"), PlaceHandle->GetActorLocation().Equals(FVector(130, 240, 0), .01));
        TestTrue(TEXT("Slot edit selects its own helper"), Director->SelectSlotEditorHandle(0));
        TestTrue(TEXT("Destination helper is selected instead of director"), GEditor->GetSelectedActors()->IsSelected(MoveHandle));
        TestFalse(TEXT("Selected slot helper is visible"), MoveHandle->IsTemporarilyHiddenInEditor());
        TestTrue(TEXT("Other slot helper is hidden"), PlaceHandle->IsTemporarilyHiddenInEditor());
        MoveHandle->SetActorLocation(FVector(700, 800, 0)); MoveHandle->PostEditMove(true);
        TestTrue(TEXT("Dragging destination saves its slot"), Director->Slots[0].Destination.Equals(FVector(700, 800, 0), .01));
        PlaceHandle->SetActorLocation(FVector(200, 300, 0)); PlaceHandle->PostEditMove(true);
        TestTrue(TEXT("Dragging placement saves local coordinates"), Director->Slots[1].Placement.GetLocation().Equals(FVector(100, 100, 0), .01));
        Director->Slots.Swap(0, 1);
        Director->RebuildSlotPaths();
        TestTrue(TEXT("Slot reorder preserves helper identity"), Director->GetSlotPositionHandle(1) == MoveHandle);
        Director->Slots[1].bEnabled = false;
        Director->RebuildSlotPaths();
        TestNull(TEXT("Disabled slot removes its helper"), Director->GetSlotPositionHandle(1));
        Director->Slots[1].bEnabled = true;
        Director->RebuildSlotPaths();
        TestTrue(TEXT("Re-enabled slot restores edited position"), Director->GetSlotPositionHandle(1)->GetActorLocation().Equals(FVector(700, 800, 0), .01));
        MoveHandle = Cast<ASceneFlowStartHandle>(Director->GetSlotPositionHandle(1));
        FSceneFlowSlot PathSlot; PathSlot.Name = TEXT("Path"); PathSlot.Action = ESceneFlowAction::MovePath;
        Director->Slots.Add(PathSlot);
        Director->RebuildSlotPaths();
        TestTrue(TEXT("Path slot edit selects its spline"), Director->SelectSlotEditorHandle(2));
        TestTrue(TEXT("Spline component is selected instead of root"), GEditor->GetSelectedComponents()->IsSelected(Director->GetSlotPath(2)));
        TestTrue(TEXT("Position helper is hidden when path is selected"), MoveHandle->IsTemporarilyHiddenInEditor());
    }
    GEditor->SelectNone(false, true);
    World->DestroyWorld(false);
    GEngine->DestroyWorldContext(World);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneGraphExecutionTest, "SceneFlow.GraphOwnsExecutionOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSceneGraphExecutionTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->SetGameInstance(NewObject<UGameInstance>(World));
    auto* Director = World->SpawnActor<ASceneFlowDirector>();
    Director->bStartWithBlack = false;
    auto* Target = World->SpawnActor<ASceneFlowMarker>();
    FSceneFlowSlot Move;
    Move.Name = TEXT("Move"); Move.Action = ESceneFlowAction::MoveToPosition;
    Move.TargetActor = Target; Move.Destination = FVector(100,0,0); Move.Duration = 20; Move.bEaseMovement = false;
    FSceneFlowSlot Hide;
    Hide.Name = TEXT("Hide"); Hide.Action = ESceneFlowAction::SetVisible; Hide.TargetActor = Target; Hide.bValue = false;
    Director->Slots = {Move, Hide};
    Director->BeginSceneGraph(); Director->Tick(100);
    TestTrue(TEXT("Graph does not start list automatically"), Target->GetActorLocation().IsZero());
    FLatentActionInfo Info; Info.CallbackTarget = Director; Info.UUID = 100;
    Director->RunSceneStep(TEXT("Move"), 2.f, Info); Director->Tick(1);
    TestEqual(TEXT("Graph duration overrides stored duration"), Target->GetActorLocation().X, 50.0);
    TestEqual(TEXT("Completion not signalled halfway"), Director->GetGraphCompletedStep(), uint64(0));
    Director->Tick(1);
    TestEqual(TEXT("Completion signalled at endpoint"), Director->GetGraphCompletedStep(), uint64(1));
    Director->Tick(100);
    TestFalse(TEXT("Following list slot is not run automatically"), Target->IsHidden());
    Info.UUID = 101; Director->RunSceneStep(TEXT("Hide"), -1, Info); Director->Tick(0);
    TestTrue(TEXT("Graph chooses next named step"), Target->IsHidden());
    TestEqual(TEXT("Second step completion"), Director->GetGraphCompletedStep(), uint64(2));
    TestTrue(TEXT("Session stays open between graph steps"), Director->bRunning);
    Director->FinishSceneGraph();
    TestFalse(TEXT("Explicit graph finish releases session"), Director->bRunning);
    GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneGraphBlueprintTest, "SceneFlow.FirstCutBlueprintGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSceneGraphBlueprintTest::RunTest(const FString& Parameters)
{
    FEditorScriptExecutionGuard AllowTestWorldBlueprints;
    UClass* Class = LoadClass<ASceneFlowDirector>(nullptr, TEXT("/Game/SceneFlow/Blueprints/BP_SceneDirector.BP_SceneDirector_C"));
    if (!TestNotNull(TEXT("Real saved Blueprint class exists"), Class)) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->SetGameInstance(NewObject<UGameInstance>(World));
    auto* Director = World->SpawnActor<ASceneFlowDirector>(Class);
    Director->bAutoStart = false;
    Director->DispatchBeginPlay();
    Director->Tick(10);
    TestFalse(TEXT("Auto Start off leaves Blueprint preview idle"), Director->bRunning);
    TestEqual(TEXT("Auto Start off does not execute a scene step"), Director->CurrentSlotIndex, -1);
    Director->Destroy();
    Director = World->SpawnActor<ASceneFlowDirector>(Class);
    Director->bAutoStart = true;
    auto* Target = World->SpawnActor<ASceneFlowMarker>();
    Director->bStartWithBlack = false; Director->bUseStartPosition = false;
    // Isolate the saved executable graph from heavy GS rendering and physical VR hardware.
    for (auto& Slot : Director->Slots)
    {
        Slot.TargetActor = Target;
        if (Slot.Action == ESceneFlowAction::PlaySound) Slot.bEnabled = false; // -nosound headless test; preserve authored audio in the actual asset.
        if (Slot.Action == ESceneFlowAction::SetBool) { Slot.Action = ESceneFlowAction::Wait; Slot.Duration = 0; }
    }
    Director->DispatchBeginPlay();
    TestTrue(TEXT("Auto Start on enters Blueprint graph"), Director->bRunning);
    for (int32 Frame=0; Frame<30; ++Frame)
    {
        Director->Tick(10);
        World->GetLatentActionManager().BeginFrame();
        World->GetLatentActionManager().ProcessLatentActions(Director,10);
    }
    TestTrue(TEXT("Saved graph executes every named step"), Director->CurrentSlotName == TEXT("08_마당에서_60초감상"));
    TestFalse(TEXT("Saved graph reaches Finish Scene Graph"), Director->bRunning);
    TestTrue(TEXT("Saved graph has no runtime errors"), Director->LastError.IsEmpty());
    if (auto* Path = Director->GetSlotPath(6))
        TestTrue(TEXT("Graph timed move reaches its actual path endpoint"), Target->GetActorLocation().Equals(Path->GetLocationAtSplinePoint(Path->GetNumberOfSplinePoints()-1, ESplineCoordinateSpace::World), .01));
    GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
    return true;
}
#endif
