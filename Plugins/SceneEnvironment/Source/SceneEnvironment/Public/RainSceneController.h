#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RainSceneController.generated.h"
class UNiagaraComponent;
class UNiagaraSystem;
class UMaterialParameterCollection;
class USceneComponent;
class AExponentialHeightFog;

UCLASS(Blueprintable, meta=(DisplayName="Rain Scene Controller"))
class SCENEENVIRONMENT_API ARainSceneController : public AActor
{
 GENERATED_BODY()
public:
 ARainSceneController();
 UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Rain Scene") TObjectPtr<USceneComponent> SceneRoot;
 UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Rain Scene") TObjectPtr<UNiagaraComponent> RainComponent;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, Category="Rain Scene|Rain", meta=(DisplayName="Rain Intensity (0-1)",ClampMin="0",ClampMax="1",ToolTip="Controls rain count/speed, haze, surface flow, ripples and splashes. Zero leaves wet reflective surfaces.")) float RainAmount=.6f;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rain Scene|Rain", meta=(ClampMin="0",ClampMax="8")) float FullRainSpawnMultiplier=3.f;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rain Scene|Rain", meta=(ClampMin=".1",ClampMax="3")) float LightRainSpeed=.65f;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rain Scene|Rain", meta=(ClampMin=".1",ClampMax="3")) float FullRainSpeed=1.6f;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rain Scene|Surface", meta=(ClampMin=".1",ClampMax="8")) float FullRainImpactRate=4.f;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rain Scene|Splash", meta=(DisplayName="Splash Size Multiplier",ClampMin="0",ClampMax="3",UIMin="0",UIMax="3",ToolTip="Size of surface spray only. 1 preserves the current size; 0 hides spray. Rain, ripples and mist keep their settings.")) float SplashSizeMultiplier=1.f;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rain Scene|Splash", meta=(DisplayName="Splash Frequency Multiplier",ClampMin="0",ClampMax="4",UIMin="0",UIMax="4",ToolTip="Surface spray event frequency and density. 1 preserves current timing; 0 hides spray. Separate from ripple frequency.")) float SplashFrequencyMultiplier=1.f;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rain Scene|Haze") TObjectPtr<AExponentialHeightFog> RainFogActor;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rain Scene|Haze", meta=(ClampMin="0",ClampMax=".1")) float FullRainFogDensity=.035f;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rain Scene|Haze", meta=(ClampMin="0",ClampMax="1")) float FullRainFogOpacity=.7f;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, Category="Rain Scene|Rain", meta=(DisplayName="Surface Wetness (0-1)",ClampMin="0",ClampMax="1")) float Wetness=1.f;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rain Scene|Surface", meta=(DisplayName="Flow Tiling (larger = smaller drops)",ClampMin=".25",ClampMax="16")) float FlowTiling=4.f;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rain Scene|Surface", meta=(DisplayName="Ripple Tiling (larger = smaller waves)",ClampMin=".25",ClampMax="16")) float RippleTiling=3.f;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rain Scene|Surface", meta=(ClampMin="0",ClampMax="2")) float FlowStrength=.6f;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rain Scene|Surface", meta=(ClampMin="0",ClampMax="2")) float RippleStrength=.25f;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rain Scene|Setup") TObjectPtr<UNiagaraSystem> RainSystem;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rain Scene|Setup") TObjectPtr<UMaterialParameterCollection> RainControls;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rain Scene|Setup") TObjectPtr<UMaterialParameterCollection> SurfaceWetness;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rain Scene|Setup") bool bFollowPlayerCamera=true;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rain Scene|Setup", meta=(ClampMin="0")) float RainHeight=500.f;
 // Opt-in sequence bridge. The existing rain-only controller is unchanged until enabled.
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Season Sequence", meta=(DisplayName="Enable Season Sequence")) bool bEnableSeasonSequence=false;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Season Sequence") TObjectPtr<AActor> GaussianTarget;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, Category="Season Sequence", meta=(ClampMin="0",ClampMax="1")) float WinterBlend=0.f;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, Category="Season Sequence", meta=(ClampMin="0",ClampMax="1")) float SnowfallIntensity=0.f;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, Category="Season Sequence", meta=(ClampMin="0",ClampMax="50",Units="cm")) float SnowAccumulationHeightCm=0.f;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, Category="Season Sequence", meta=(ClampMin="0",ClampMax="2")) float SequenceFogStrength=0.f;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Season Sequence") bool bLinkHeavyRainFog=true;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Season Sequence", meta=(ClampMin="0",ClampMax="2")) float HeavyRainFogStrength=1.2f;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Season Sequence") TObjectPtr<UNiagaraSystem> SequenceSnowSystem;
 UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Season Sequence") TObjectPtr<UNiagaraComponent> SequenceSnowComponent;
 UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Season Sequence") FString SeasonSequenceStatus;
 UFUNCTION(BlueprintCallable, CallInEditor, Category="Season Sequence") void CaptureSeasonSequenceBaseline();
 UFUNCTION(BlueprintCallable, CallInEditor, Category="Season Sequence") void RestoreSeasonSequenceBaseline();
 UPROPERTY() TObjectPtr<AActor> SeasonBaselineTarget;
 UPROPERTY() TMap<FName,float> SeasonBaselineFloats;
 UPROPERTY() TMap<FName,FLinearColor> SeasonBaselineColors;
 UPROPERTY() TMap<FName,bool> SeasonBaselineBools;
 UPROPERTY() float SeasonBaselineSnowHeight=0.f;
 UPROPERTY() FString SeasonBaselineWeather;
 UPROPERTY() FString SeasonBaselineSurface;
 UPROPERTY() TObjectPtr<UNiagaraSystem> SeasonBaselineSnowSystem;
 UPROPERTY() TObjectPtr<UNiagaraSystem> SeasonBaselineRainSystem;
 // Backward-compatible binding used by the authored original 60-second sequence.
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, Category="Rain Scene|Rain", meta=(DisplayName="Legacy Sequencer Rain Amount")) float SequencerRainAmount=0.f;
 UFUNCTION(BlueprintCallable, Category="Rain Scene") void SetSequencerRainAmount(float Amount);
 UFUNCTION(BlueprintCallable, Category="Rain Scene") void SetRainAmount(float Amount);
 UFUNCTION(BlueprintCallable, Category="Rain Scene") void RefreshRain();
 virtual void OnConstruction(const FTransform& Transform) override;
 virtual void BeginPlay() override;
 virtual void Tick(float DeltaSeconds) override;
 virtual bool ShouldTickIfViewportsOnly() const override { return true; }
#if WITH_EDITOR
 virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
private:
 void ApplyParameters(bool Force);
 void ApplySeasonSequence();
 bool ApplyPluginPrecipitation();
 bool bSeasonSequenceWasActive=false;
 float LastSequenceSnowOpacity=-1.f;
 TWeakObjectPtr<UWorld> LastWorld;
 TWeakObjectPtr<UMaterialParameterCollection> LastControls,LastWetness;
 float LastRain=-1.f,LastWet=-1.f,LastFlowTiling=-1.f,LastRippleTiling=-1.f,LastFlowStrength=-1.f,LastRippleStrength=-1.f;
 TWeakObjectPtr<AExponentialHeightFog> LastFogActor;
 float LastSpawn=-1.f,LastLightSpeed=-1.f,LastFullSpeed=-1.f,LastImpactRate=-1.f,LastFogDensity=-1.f,LastFogOpacity=-1.f;
 float LastSplashSize=-1.f,LastSplashFrequency=-1.f;
};
