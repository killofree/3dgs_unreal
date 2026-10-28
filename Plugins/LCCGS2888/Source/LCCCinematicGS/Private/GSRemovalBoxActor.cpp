#include "GSRemovalBoxActor.h"
#include "GSRemovalBoxRenderState.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

AGSRemovalBoxActor::AGSRemovalBoxActor()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = true;
    RemovalBounds = CreateDefaultSubobject<UBoxComponent>(TEXT("RemovalBounds"));
    RootComponent = RemovalBounds;
    RemovalBounds->InitBoxExtent(FVector(150.0,150.0,150.0));
    RemovalBounds->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    RemovalBounds->SetHiddenInGame(true);
    SetActorHiddenInGame(true);
}

void AGSRemovalBoxActor::UpdateRenderBox(bool bForce)
{
    if (!RemovalBounds || !GetWorld()) return;
    const FSceneInterface* Scene = GetWorld()->Scene;
    const FTransform Transform = RemovalBounds->GetComponentTransform();
    const FVector Extent = RemovalBounds->GetScaledBoxExtent();
    const bool bEnabled = bRemovalEnabled && !IsActorBeingDestroyed() &&
        Extent.GetMin() > KINDA_SMALL_NUMBER;

    if (!bForce && bPublished && Scene == LastScene && bEnabled == bLastEnabled &&
        Transform.Equals(LastTransform, 0.001) && Extent.Equals(LastExtent, 0.001)) return;

    if (bPublished && LastScene && (LastScene != Scene || !bEnabled)) RemoveRenderBox();

    LastTransform = Transform;
    LastExtent = Extent;
    bLastEnabled = bEnabled;
    if (!Scene || !bEnabled) return;

    FGSRemovalBoxGPUData Box;
    const FVector Origin = RemovalBounds->GetComponentLocation();
    const FQuat Rotation = RemovalBounds->GetComponentQuat();
    auto MakeAxis = [](const FVector& Axis, double HalfExtent)
    {
        return FVector4f(float(Axis.X),float(Axis.Y),float(Axis.Z),float(HalfExtent));
    };
    Box.Center = FVector4f(float(Origin.X),float(Origin.Y),float(Origin.Z),0.0f);
    Box.X = MakeAxis(Rotation.GetAxisX(), Extent.X);
    Box.Y = MakeAxis(Rotation.GetAxisY(), Extent.Y);
    Box.Z = MakeAxis(Rotation.GetAxisZ(), Extent.Z);
    const uint64 Id = uint64(reinterpret_cast<UPTRINT>(this));
    ENQUEUE_RENDER_COMMAND(LCCGSUpdateRemovalBox)([Scene,Id,Box](FRHICommandListImmediate&)
    {
        SetGSRemovalBox_RenderThread(Scene, Id, Box);
    });
    LastScene = Scene;
    bPublished = true;
}

void AGSRemovalBoxActor::RemoveRenderBox()
{
    if (!bPublished || !LastScene) return;
    const FSceneInterface* Scene = LastScene;
    const uint64 Id = uint64(reinterpret_cast<UPTRINT>(this));
    ENQUEUE_RENDER_COMMAND(LCCGSRemoveRemovalBox)([Scene,Id](FRHICommandListImmediate&)
    {
        ClearGSRemovalBox_RenderThread(Scene, Id);
    });
    LastScene = nullptr;
    bPublished = false;
}

void AGSRemovalBoxActor::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    UpdateRenderBox(true);
}
void AGSRemovalBoxActor::BeginPlay() { Super::BeginPlay(); UpdateRenderBox(true); }
void AGSRemovalBoxActor::Tick(float DeltaSeconds) { Super::Tick(DeltaSeconds); UpdateRenderBox(); }
void AGSRemovalBoxActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    RemoveRenderBox();
    Super::EndPlay(EndPlayReason);
}
void AGSRemovalBoxActor::Destroyed() { RemoveRenderBox(); Super::Destroyed(); }
#if WITH_EDITOR
void AGSRemovalBoxActor::PostEditMove(bool bFinished)
{
    Super::PostEditMove(bFinished);
    UpdateRenderBox(true);
}
void AGSRemovalBoxActor::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
    Super::PostEditChangeProperty(Event);
    UpdateRenderBox(true);
}
#endif
