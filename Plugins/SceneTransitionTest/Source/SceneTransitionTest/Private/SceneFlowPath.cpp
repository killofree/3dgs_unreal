#include "SceneFlowPath.h"
#include "Components/SplineComponent.h"
#include "Components/ArrowComponent.h"

ASceneFlowPath::ASceneFlowPath()
{
    Path = CreateDefaultSubobject<USplineComponent>(TEXT("Path"));
    SetRootComponent(Path);
}
ASceneFlowMarker::ASceneFlowMarker()
{
    Marker = CreateDefaultSubobject<UArrowComponent>(TEXT("Marker"));
    SetRootComponent(Marker);
}
