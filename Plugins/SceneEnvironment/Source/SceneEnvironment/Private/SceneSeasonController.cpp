#include "SceneSeasonController.h"
#include "CherrySeasonTreeActor.h"
#include "Components/MeshComponent.h"
#include "Components/SceneComponent.h"
#include "EngineUtils.h"
#include "UObject/UnrealType.h"

ASceneSeasonController::ASceneSeasonController()
{
    PrimaryActorTick.bCanEverTick=true;
    RootComponent=CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    TargetTreeClass=ACherrySeasonTreeActor::StaticClass();
}
void ASceneSeasonController::RefreshTargets()
{
    Trees.Reset(); SunMeshes.Reset();
    if (GetWorld() && TargetTreeClass)
        for (TActorIterator<ACherrySeasonTreeActor> It(GetWorld()); It; ++It)
            if (It->IsA(TargetTreeClass) && (TreeTag.IsNone() || It->ActorHasTag(TreeTag))) Trees.Add(*It);
    if (IsValid(SunActor))
    {
        TArray<AActor*> Actors; SunActor->GetAttachedActors(Actors,true,true); Actors.Add(SunActor);
        for (AActor* Actor:Actors)
        {
            TInlineComponentArray<UMeshComponent*> Meshes(Actor);
            for (UMeshComponent* Mesh:Meshes)
                if (Mesh->ComponentHasTag(TEXT("SceneSunCoreControl")) || Mesh->ComponentHasTag(TEXT("SceneSunHaloControl"))) SunMeshes.AddUnique(Mesh);
        }
    }
    TreeCount=Trees.Num(); SunMeshCount=SunMeshes.Num();
    LastValues=FVector(-100); ApplyControls();
}
void ASceneSeasonController::ApplyControls()
{
    FlowerSize=FMath::Clamp(FlowerSize,0.f,2.f); LeafSize=FMath::Clamp(LeafSize,0.f,2.f); SunSharpness=FMath::Clamp(SunSharpness,0.f,1.f);
    const FVector Values(FlowerSize,LeafSize,SunSharpness), Range(FullEmission,FullGlow,FullRays);
    if (Values==LastValues && Range==LastSunRange && SoftEmission==LastSoft) return;
    for (auto& Weak:Trees) if (auto* Tree=Weak.Get()) { Tree->SetFlowerSize(FlowerSize); Tree->SetLeafSize(LeafSize); }
    for (auto& Weak:SunMeshes) if (auto* Mesh=Weak.Get())
    {
        if (Mesh->ComponentHasTag(TEXT("SceneSunCoreControl"))) Mesh->SetCustomPrimitiveDataFloat(25,FMath::Lerp(SoftEmission,FullEmission,SunSharpness));
        if (Mesh->ComponentHasTag(TEXT("SceneSunHaloControl")))
        {
            Mesh->SetCustomPrimitiveDataFloat(26,FullGlow*SunSharpness);
            Mesh->SetCustomPrimitiveDataFloat(27,FullRays*SunSharpness);
        }
    }
    LastValues=Values; LastSunRange=Range; LastSoft=SoftEmission;
}
void ASceneSeasonController::SetFlowerSize(float Value) { FlowerSize=Value; ApplyControls(); }
void ASceneSeasonController::SetLeafSize(float Value) { LeafSize=Value; ApplyControls(); }
void ASceneSeasonController::SetSunSharpness(float Value) { SunSharpness=Value; ApplyControls(); }
void ASceneSeasonController::OnConstruction(const FTransform& Transform) { Super::OnConstruction(Transform); RefreshTargets(); }
void ASceneSeasonController::BeginPlay() { Super::BeginPlay(); RefreshTargets(); }
void ASceneSeasonController::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds); RefreshElapsed+=DeltaSeconds;
    if (RefreshElapsed>=1 || LastValues.X < -99) { RefreshElapsed=0; RefreshTargets(); } else ApplyControls();
}
#if WITH_EDITOR
static bool IsUniform(const FProperty* P)
{
    return P && (P->GetFName()==TEXT("FlowerSize") || P->GetFName()==TEXT("LeafSize") || P->GetFName()==TEXT("SunSharpness"));
}
void ASceneSeasonController::PreEditChange(FProperty* P) { if (IsUniform(P)) UObject::PreEditChange(P); else Super::PreEditChange(P); }
void ASceneSeasonController::PostEditChangeProperty(FPropertyChangedEvent& E)
{
    if (IsUniform(E.MemberProperty ? E.MemberProperty:E.Property)) { ApplyControls(); UObject::PostEditChangeProperty(E); }
    else { Super::PostEditChangeProperty(E); RefreshTargets(); }
}
#endif
