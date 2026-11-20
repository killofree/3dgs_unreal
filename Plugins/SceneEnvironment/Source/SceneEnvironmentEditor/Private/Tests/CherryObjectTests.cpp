#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "CherrySeasonTreeActor.h"
#include "SceneSeasonController.h"
#include "CherryTreeGrowthComponent.h"
#include "CherryGrowthMaterialLibrary.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Editor.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCherryObjectMoveTest,"SceneEnvironment.Cherry.AssembledObject",EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCherryObjectMoveTest::RunTest(const FString& Parameters)
{
    UWorld* World=FAutomationEditorCommonUtils::CreateNewMap();
    auto* Blueprint=LoadObject<UBlueprint>(nullptr,TEXT("/Game/SceneFlow/SeasonalTrees/CherryActors/BP_SeasonalCherry_C.BP_SeasonalCherry_C"));
    if (!TestNotNull(TEXT("Existing assembled C blueprint"),Blueprint)) return false;
    auto* Tree=World->SpawnActor<ACherrySeasonTreeActor>(Blueprint->GeneratedClass);
    if (!TestNotNull(TEXT("Tree"),Tree)) return false;
    TInlineComponentArray<UInstancedStaticMeshComponent*> Before(Tree);
    TArray<int32> Counts;
    int64 Total=0;
    for (auto* C:Before) { Counts.Add(C->GetInstanceCount()); Total+=C->GetInstanceCount(); }
    TestTrue(TEXT("Real assembled instances loaded"),Total>0);
    TestFalse(TEXT("Actor cannot tick"),Tree->PrimaryActorTick.bCanEverTick!=0);
    auto* Driver=Tree->FindComponentByClass<UCherryTreeGrowthComponent>();
    TestNotNull(TEXT("Growth driver"),Driver);
    if (Driver) TestFalse(TEXT("Driver cannot tick"),Driver->PrimaryComponentTick.bCanEverTick!=0);
    const double Start=FPlatformTime::Seconds();
    for (int32 I=0;I<10;++I)
    {
        Tree->SetActorLocation(FVector(I*10,0,0));
        Tree->PostEditMove(false);
    }
    Tree->PostEditMove(true);
    TInlineComponentArray<UInstancedStaticMeshComponent*> After(Tree);
    TestEqual(TEXT("Component count retained"),After.Num(),Before.Num());
    for (int32 I=0;I<Before.Num();++I)
    {
        TestTrue(TEXT("Same component objects after drag and release"),After.Contains(Before[I]) && IsValid(Before[I]));
        if (IsValid(Before[I])) TestEqual(TEXT("Instance count retained"),Before[I]->GetInstanceCount(),Counts[I]);
    }
    Tree->SetFlowerSize(.25f); Tree->SetLeafSize(.75f);
    for (auto* C:After)
    {
        if (C->ComponentHasTag(TEXT("CherryGrowthFlowers"))) TestEqual(TEXT("Flower material data"),UCherryGrowthMaterialLibrary::ReadGrowthScale(C),.25f);
        if (C->ComponentHasTag(TEXT("CherryGrowthLeaves"))) TestEqual(TEXT("Leaf material data"),UCherryGrowthMaterialLibrary::ReadGrowthScale(C),.75f);
    }
    FProperty* Property=FindFProperty<FProperty>(Tree->GetClass(),TEXT("FlowerSize"));
    Tree->PreEditChange(Property); Tree->FlowerSize=.4f;
    FPropertyChangedEvent Event(Property); Tree->PostEditChangeProperty(Event);
    TInlineComponentArray<UInstancedStaticMeshComponent*> AfterEdit(Tree);
    for (auto* C:Before) TestTrue(TEXT("Material edit retains component identity"),AfterEdit.Contains(C) && IsValid(C));
    AddInfo(FString::Printf(TEXT("Checked %lld instances, %d components; drag checks %.3f seconds (includes render/editor notifications)"),Total,Before.Num(),FPlatformTime::Seconds()-Start));
    World->DestroyActor(Tree);
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSeasonControllerNoPollingTest,"SceneEnvironment.SeasonController.NoPolling",EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSeasonControllerNoPollingTest::RunTest(const FString& Parameters)
{
    UWorld* World=FAutomationEditorCommonUtils::CreateNewMap();
    auto* Tree=World->SpawnActor<ACherrySeasonTreeActor>();
    auto* Controller=World->SpawnActor<ASceneSeasonController>();
    TestFalse(TEXT("Controller cannot tick"),Controller->PrimaryActorTick.bCanEverTick!=0);
    Controller->RefreshTargets();
    TestEqual(TEXT("Initial target"),Controller->TreeCount,1);
    Controller->SetLeafSize(.2f);
    TestEqual(TEXT("Value changes immediately without tick"),Tree->LeafSize,.2f);
    auto* NewTree=World->SpawnActor<ACherrySeasonTreeActor>();
    // A construction call or moving the controller must not rescan the world.
    Controller->OnConstruction(Controller->GetActorTransform());
    TestEqual(TEXT("No rescan on construction/move"),Controller->TreeCount,1);
    Controller->RefreshTargets();
    TestEqual(TEXT("Explicit refresh finds added tree"),Controller->TreeCount,2);
    TestEqual(TEXT("New target receives current value"),NewTree->LeafSize,.2f);
    Controller->SetTreeTag(TEXT("ChosenTree"));
    TestEqual(TEXT("Target setting change refreshes once"),Controller->TreeCount,0);
    World->DestroyActor(Controller);World->DestroyActor(Tree);World->DestroyActor(NewTree);
    return !HasAnyErrors();
}
#endif
