#include "CherrySeasonTreeActor.h"
#include "CherryTreeGrowthComponent.h"
#include "UObject/UnrealType.h"
#if WITH_EDITOR
#include "AI/NavigationSystemBase.h"
#include "EditorSupportDelegates.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Components/SceneComponent.h"
#include "LevelUtils.h"
#endif

ACherrySeasonTreeActor::ACherrySeasonTreeActor()
{
    PrimaryActorTick.bCanEverTick=false;
#if WITH_EDITOR
    bRunConstructionScriptOnDrag=false;
#endif
    GrowthDriver=CreateDefaultSubobject<UCherryTreeGrowthComponent>(TEXT("GrowthDriver"));
}
void ACherrySeasonTreeActor::ApplyControls()
{
    FlowerSize=FMath::Clamp(FlowerSize,0.f,2.f); LeafSize=FMath::Clamp(LeafSize,0.f,2.f);
    if (GrowthDriver) { GrowthDriver->SetFlowerSize(FlowerSize); GrowthDriver->SetLeafSize(LeafSize); }
}
void ACherrySeasonTreeActor::SetFlowerSize(float Value) { FlowerSize=Value; ApplyControls(); }
void ACherrySeasonTreeActor::SetLeafSize(float Value) { LeafSize=Value; ApplyControls(); }
void ACherrySeasonTreeActor::RefreshTree()
{ if (GrowthDriver) GrowthDriver->RefreshGrowthTargets(); ApplyControls(); }
void ACherrySeasonTreeActor::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    // SCS flower/leaf components exist only after construction completes.
    // Cache them once here; PostRegisterAllComponents can occur earlier on spawn.
    if (!bInitialConstructionComplete) { RefreshTree(); bInitialConstructionComplete=true; }
}
void ACherrySeasonTreeActor::PostRegisterAllComponents()
{
    Super::PostRegisterAllComponents();
    // Initial load/spawn/explicit BP recompilation only; never polled per frame.
    RefreshTree();
}
#if WITH_EDITOR
void ACherrySeasonTreeActor::PostEditMove(bool bFinished)
{
    // The assembled tree has no construction-time placement to regenerate.
    // Preserve editor notifications/culling/navigation without the base class
    // unregistering and reconstructing every SCS instance component.
    if (!FLevelUtils::IsMovingLevel() && GEngine) GEngine->BroadcastOnActorMoving(this);
    if (bFinished)
    {
        if (UWorld* World=GetWorld())
        {
            World->UpdateCullDistanceVolumes(this);
            World->bAreConstraintsDirty=true;
        }
        FEditorSupportDelegates::RefreshPropertyWindows.Broadcast();
        if (GEngine) GEngine->BroadcastOnActorMoved(this);
        FEditorSupportDelegates::UpdateUI.Broadcast();
    }
    if (RootComponent) RootComponent->PostEditComponentMove(bFinished);
    if (bFinished) FNavigationSystem::OnPostEditActorMove(*this);
}
namespace
{
bool IsGrowthSizeProperty(const FProperty* Property)
{
    if (!Property) return false;
    const FName Name=Property->GetFName();
    return Name==GET_MEMBER_NAME_CHECKED(ACherrySeasonTreeActor,FlowerSize)
        || Name==GET_MEMBER_NAME_CHECKED(ACherrySeasonTreeActor,LeafSize);
}
}
void ACherrySeasonTreeActor::PreEditChange(FProperty* Property)
{
    if (IsGrowthSizeProperty(Property))
    {
        // AActor's path unregisters all components before a property edit.
        // Uniform growth values do not change any component or instance layout.
        UObject::PreEditChange(Property);
        return;
    }
    Super::PreEditChange(Property);
}
void ACherrySeasonTreeActor::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
    if (IsGrowthSizeProperty(Event.MemberProperty ? Event.MemberProperty : Event.Property))
    {
        ApplyControls();
        // Bypass AActor::PostEditChangeProperty: it can rerun construction and
        // recreate every SCS ISM, copying the full foliage instance arrays.
        UObject::PostEditChangeProperty(Event);
        return;
    }
    Super::PostEditChangeProperty(Event);
}
#endif
