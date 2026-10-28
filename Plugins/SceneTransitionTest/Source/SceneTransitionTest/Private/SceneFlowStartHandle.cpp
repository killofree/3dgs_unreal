#include "SceneFlowStartHandle.h"
#include "SceneFlowDirector.h"
#include "Components/StaticMeshComponent.h"
#include "Components/ArrowComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"
#if WITH_EDITOR
#include "Editor.h"
#endif

void ASceneFlowStartHandle::SelectDirector()
{
#if WITH_EDITOR
    if (GEditor && IsValid(Director))
    {
        GEditor->SelectNone(false, true);
        GEditor->SelectActor(Director, true, true);
    }
#endif
}

ASceneFlowStartHandle::ASceneFlowStartHandle()
{
    bIsEditorOnlyActor = true;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("HandleRoot")));
    HandleMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("StartPositionHandle"));
    HandleMesh->SetupAttachment(GetRootComponent());
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Mesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
    if (Mesh.Succeeded()) HandleMesh->SetStaticMesh(Mesh.Object);
    HandleMesh->SetRelativeScale3D(FVector(.7f, .7f, .12f));
    HandleMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    HandleMesh->SetHiddenInGame(true);
    Arrow = CreateDefaultSubobject<UArrowComponent>(TEXT("StartDirection"));
    Arrow->SetupAttachment(GetRootComponent());
    Arrow->SetAbsolute(false, false, true);
    Arrow->ArrowColor = FColor::Cyan;
    Arrow->ArrowSize = 2;
    Arrow->SetHiddenInGame(true);
}
#if WITH_EDITOR
void ASceneFlowStartHandle::SaveToDirector()
{
    if (!IsValid(Director)) return;
    if (SlotId.IsValid()) Director->SaveSlotPositionFromHandle(SlotId, GetActorTransform());
    else Director->SaveStartPositionFromHandle(GetActorLocation());
}
void ASceneFlowStartHandle::PostEditMove(bool bFinished)
{
    Super::PostEditMove(bFinished);
    SaveToDirector();
}
void ASceneFlowStartHandle::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
    Super::PostEditChangeProperty(Event);
    SaveToDirector();
}
#endif
