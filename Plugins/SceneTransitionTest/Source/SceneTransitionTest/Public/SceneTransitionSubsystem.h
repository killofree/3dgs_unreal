#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "SceneTransitionSubsystem.generated.h"

// GameInstance lifetime keeps transition state alive through OpenLevel.
UCLASS(Config=Game, BlueprintType)
class SCENETRANSITIONTEST_API USceneTransitionSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual void Tick(float DeltaTime) override;
    virtual bool IsTickable() const override { return !IsTemplate(); }
    virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(SceneTransitionTest, STATGROUP_Tickables); }
    virtual UWorld* GetTickableGameObjectWorld() const override { return GetWorld(); }

    UFUNCTION(BlueprintCallable, Category="Scene Transition") void StartTest();
    UFUNCTION(BlueprintCallable, Category="Scene Transition") void TravelToNextMap();
    UFUNCTION(BlueprintCallable, Category="Scene Transition") bool TravelToMap(const FString& Map, float FadeOutTime = 1.5f, float FadeInTime = 1.5f, FLinearColor Color = FLinearColor::Black);
    UFUNCTION(BlueprintCallable, Category="Scene Transition") void SetFlowControl(bool bActive) { bFlowControl = bActive; }
    void HoldInitialFadeForDirector(bool bHold) { bDirectorOwnsInitialFade = bHold; }
    UFUNCTION(BlueprintPure, Category="Scene Transition") bool IsTransitioning() const { return Phase != EPhase::Ready; }

    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Scene Transition") bool bStartTestOnLaunch = false;
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Scene Transition") FString FirstMap = TEXT("/Game/level/j_02");
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Scene Transition") FString SecondMap = TEXT("/Game/level/j_m_01");
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Scene Transition") bool bRequireStableFrameRate = false;
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Scene Transition") float StableFrameMilliseconds = 20.f;
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Scene Transition") int32 RequiredStableFrames = 90;
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Scene Transition") float MaximumWarmupSeconds = 60.f;
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Scene Transition") FLinearColor FadeColor = FLinearColor::Black;
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Scene Transition") float KeyboardMoveSpeed = 150.f;
private:
    enum class EPhase : uint8 { Warmup, FadeIn, Ready, FadeOut, Loading, Failed };
    void OnMapLoaded(UWorld* World);
    void ApplyFade(float Amount);
    void ShowLoadingCover();
    bool ApplyFirstCutState();
    uint64 FirstCutAppliedFrame = MAX_uint64;
    bool bFirstCutLogged = false;
    bool bEnabled = false;
    bool bFlowControl = false;
    bool bDirectorOwnsInitialFade = false;
    bool bAutoRoundTrip = false;
    bool bNextHeld = false;
    bool bWarned = false;
    int32 CompletedTrips = 0;
    int32 StableFrames = 0;
    float Elapsed = 0.f;
    EPhase Phase = EPhase::Warmup;
    FString PendingMap;
    float TravelFadeOut = -1;
    float TravelFadeIn = -1;
    FDelegateHandle MapLoadedHandle;
    FDelegateHandle TravelFailureHandle;
    UPROPERTY(Transient) TObjectPtr<class UTexture2D> CoverTexture;
};
