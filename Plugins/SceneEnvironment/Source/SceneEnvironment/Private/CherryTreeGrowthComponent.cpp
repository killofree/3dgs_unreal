#include "CherryTreeGrowthComponent.h"
#include "Components/MeshComponent.h"
#include "GameFramework/Actor.h"

UCherryTreeGrowthComponent::UCherryTreeGrowthComponent()
{
    PrimaryComponentTick.bCanEverTick=true;
#if WITH_EDITOR
    bTickInEditor=true;
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
void UCherryTreeGrowthComponent::TickComponent(float DeltaTime,ELevelTick TickType,FActorComponentTickFunction* TickFunction)
{ Super::TickComponent(DeltaTime,TickType,TickFunction); ApplySizes(); }
#if WITH_EDITOR
void UCherryTreeGrowthComponent::PostEditChangeProperty(FPropertyChangedEvent& Event)
{ Super::PostEditChangeProperty(Event); RefreshGrowthTargets(); }
#endif
