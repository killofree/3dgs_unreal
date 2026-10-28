#pragma once
#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "SceneTransitionSettings.generated.h"

// Project Settings -> Plugins -> Scene Transition. Saved in DefaultGame.ini.
UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Scene Transition"))
class SCENETRANSITIONTEST_API USceneTransitionSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
    virtual FName GetSectionName() const override { return TEXT("SceneTransition"); }

    UPROPERTY(Config, EditAnywhere, Category="Maps", meta=(ToolTip="Full map package paths in N-key cycle order. Include these maps when packaging."))
    TArray<FString> MapCycle = { TEXT("/Game/level/j_02"), TEXT("/Game/level/j_m_01"), TEXT("/Game/j_v_01") };

    UPROPERTY(Config, EditAnywhere, Category="Timing", meta=(ClampMin="0.0", Units="s", ToolTip="Time to cover the current scene before loading."))
    float FadeOutSeconds = 1.5f;

    UPROPERTY(Config, EditAnywhere, Category="Timing", meta=(ClampMin="0.0", Units="s", ToolTip="Time to reveal the destination after loading and warmup."))
    float FadeInSeconds = 1.5f;

    UPROPERTY(Config, EditAnywhere, Category="Timing", meta=(ClampMin="0.0", Units="s", ToolTip="Additional covered wait AFTER map loading. Does not shorten actual asset loading. Pawn and camera must also be ready."))
    float DefaultWarmupSeconds = 5.f;

    UPROPERTY(Config, EditAnywhere, Category="Timing", meta=(ToolTip="Optional wait overrides by short map name, e.g. j_02 or j_m_01. Values are seconds; zero skips the added wait."))
    TMap<FName, float> WarmupSecondsByMap;

    UPROPERTY(Config, EditAnywhere, Category="Auto Test", meta=(ClampMin="0.0", Units="s", ToolTip="Visible pause before each automatic trip when -SceneTransitionAuto is enabled."))
    float AutoAdvanceSeconds = 8.f;

    UPROPERTY(Config, EditAnywhere, Category="First Cut")
    bool bApplyFirstCutState = false;

    UPROPERTY(Config, EditAnywhere, Category="First Cut")
    FName FirstCutMap = TEXT("j_02");

    UPROPERTY(Config, EditAnywhere, Category="First Cut", meta=(ToolTip="Stable actor name prefixes, not editor labels. Includes mesh actors inside a Level Instance. Existing transforms are preserved."))
    TArray<FString> FirstCutHiddenActorPrefixes;

    UPROPERTY(Config, EditAnywhere, Category="First Cut", meta=(ToolTip="Actor name prefix -> editable bool property to disable. Validated at runtime; no dependency on a specific GS plugin."))
    TMap<FString, FName> FirstCutDisabledBoolProperties;

    UPROPERTY(Config, EditAnywhere, Category="Actor Visibility", meta=(ToolTip="Stable actor name prefix -> Hidden In Game override. Applies to loaded actors on every map entry, including return trips. Does not change child component visibility."))
    TMap<FString, bool> ActorHiddenOverrides;
};
