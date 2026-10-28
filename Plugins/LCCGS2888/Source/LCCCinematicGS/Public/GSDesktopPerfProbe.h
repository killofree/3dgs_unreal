#pragma once
#include "CoreMinimal.h"
#include "Camera/CameraActor.h"
#include "GSDesktopPerfProbe.generated.h"
class ADirectionalLight;
class UDirectionalLightComponent;

/** Opt-in, transient moving camera for repeatable desktop performance captures. */
UCLASS(Transient, NotPlaceable)
class AGSDesktopPerfProbe : public ACameraActor
{
    GENERATED_BODY()
public:
    AGSDesktopPerfProbe();
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    static void TryStartVREffectsTest(UWorld* World);
    bool bManualVRPreview=false;
    bool bVREffectsTest=false;
    float DurationSeconds = 45.0f;
    float SunPitchDegreesPerSecond=1.0f;
    float SunPitchSweepDegrees=0.0f;
    bool bStationary=false;
    bool bAnimateTestLight=false;
private:
    struct FPreviewSetting { FString Name, Before, Applied; uint32 Priority; };
    TArray<FPreviewSetting> PreviewSettings;
    double StartWallSeconds = 0;
    double PreviousWallSeconds=0;
    TWeakObjectPtr<UDirectionalLightComponent> VRTestSun;
    FRotator OriginalSunRotation=FRotator::ZeroRotator;
    EComponentMobility::Type OriginalSunMobility=EComponentMobility::Movable;
    double LastVRLogSeconds=0;
    double LastVRLogSampleSeconds=0;
    int32 LastVRLogSampleCount=0;
    TWeakObjectPtr<ADirectionalLight> TestLight;
    bool bFinished = false;
    TArray<double> FrameMs;
};
