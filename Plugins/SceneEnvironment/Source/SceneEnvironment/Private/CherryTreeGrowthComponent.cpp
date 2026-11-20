#include "CherryTreeGrowthComponent.h"
#include "Components/MeshComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"

UCherryTreeGrowthComponent::UCherryTreeGrowthComponent()
{
    PrimaryComponentTick.bCanEverTick=false;
#if WITH_EDITOR
    bTickInEditor=false;
#endif
}
void UCherryTreeGrowthComponent::OnRegister() { Super::OnRegister(); RefreshGrowthTargets(); }
void UCherryTreeGrowthComponent::RefreshGrowthTargets()
{
    FlowerTargets.Reset(); LeafTargets.Reset();
    if (AActor* Owner=GetOwner())
    {
        TInlineComponentArray<UMeshComponent*> Components(Owner);
        for (UMeshComponent* Component:Components)
        {
            if (Component->ComponentHasTag(TEXT("CherryGrowthFlowers"))) FlowerTargets.Add(Component);
            if (Component->ComponentHasTag(TEXT("CherryGrowthLeaves"))) LeafTargets.Add(Component);
        }
    }
    LastFlower=LastLeaf=-1.f; ApplySizes();
}
void UCherryTreeGrowthComponent::ApplySizes()
{
    FlowerSize=FMath::Clamp(FlowerSize,0.f,2.f); LeafSize=FMath::Clamp(LeafSize,0.f,2.f);
    if (LastFlower!=FlowerSize)
    {
        for (auto Target:FlowerTargets) if (UMeshComponent* C=Target.Get()) C->SetCustomPrimitiveDataFloat(31,FlowerSize);
        LastFlower=FlowerSize;
    }
    if (LastLeaf!=LeafSize)
    {
        for (auto Target:LeafTargets) if (UMeshComponent* C=Target.Get()) C->SetCustomPrimitiveDataFloat(31,LeafSize);
        LastLeaf=LeafSize;
    }
}
void UCherryTreeGrowthComponent::SetFlowerSize(float Value) { FlowerSize=Value; ApplySizes(); }
void UCherryTreeGrowthComponent::SetLeafSize(float Value) { LeafSize=Value; ApplySizes(); }
#if WITH_EDITOR
static bool IsSizeEdit(const FProperty* P)
{ return P && (P->GetFName()==TEXT("FlowerSize") || P->GetFName()==TEXT("LeafSize")); }
void UCherryTreeGrowthComponent::PreEditChange(FProperty* Property)
{
    if (IsSizeEdit(Property)) UObject::PreEditChange(Property);
    else Super::PreEditChange(Property);
}
void UCherryTreeGrowthComponent::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
    if (IsSizeEdit(Event.MemberProperty ? Event.MemberProperty : Event.Property))
    { ApplySizes(); UObject::PostEditChangeProperty(Event); }
    else Super::PostEditChangeProperty(Event);
}
#endif
