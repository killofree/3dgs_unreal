#include "CherrySeasonTreeActor.h"
#include "CherryTreeGrowthComponent.h"
#include "UObject/UnrealType.h"

ACherrySeasonTreeActor::ACherrySeasonTreeActor()
{
    PrimaryActorTick.bCanEverTick=true;
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
{ Super::OnConstruction(Transform); RefreshTree(); }
void ACherrySeasonTreeActor::Tick(float DeltaSeconds)
{ Super::Tick(DeltaSeconds); ApplyControls(); }
#if WITH_EDITOR
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
    RefreshTree();
}
#endif
