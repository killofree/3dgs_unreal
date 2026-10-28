#include "RainSceneController.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Components/SceneComponent.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "Materials/MaterialParameterCollection.h"
#include "Engine/ExponentialHeightFog.h"
#include "Components/ExponentialHeightFogComponent.h"

ARainSceneController::ARainSceneController()
{
 PrimaryActorTick.bCanEverTick=true;
 SceneRoot=CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));SetRootComponent(SceneRoot);
 RainComponent=CreateDefaultSubobject<UNiagaraComponent>(TEXT("Rain"));RainComponent->SetupAttachment(SceneRoot);
 RainComponent->SetRelativeLocation(FVector(0,0,RainHeight));RainComponent->SetAutoActivate(false);
}
void ARainSceneController::OnConstruction(const FTransform& T){Super::OnConstruction(T);ApplyParameters(true);}
void ARainSceneController::BeginPlay(){Super::BeginPlay();ApplyParameters(true);}
void ARainSceneController::SetRainAmount(float Amount){RainAmount=FMath::Clamp(Amount,0.f,1.f);ApplyParameters(true);}
void ARainSceneController::RefreshRain(){ApplyParameters(true);}
#if WITH_EDITOR
void ARainSceneController::PostEditChangeProperty(FPropertyChangedEvent& E){Super::PostEditChangeProperty(E);ApplyParameters(true);}
#endif
void ARainSceneController::ApplyParameters(bool Force)
{
 UWorld* W=GetWorld();if(!W)return;
 RainAmount=FMath::Clamp(RainAmount,0.f,1.f);Wetness=FMath::Clamp(Wetness,0.f,1.f);
 FlowTiling=FMath::Clamp(FlowTiling,.25f,16.f);RippleTiling=FMath::Clamp(RippleTiling,.25f,16.f);
 FlowStrength=FMath::Clamp(FlowStrength,0.f,2.f);RippleStrength=FMath::Clamp(RippleStrength,0.f,2.f);
 SplashSizeMultiplier=FMath::Clamp(SplashSizeMultiplier,0.f,3.f);SplashFrequencyMultiplier=FMath::Clamp(SplashFrequencyMultiplier,0.f,4.f);
 if(RainComponent->GetAsset()!=RainSystem){RainComponent->SetAsset(RainSystem);Force=true;}
 const bool Changed=Force || LastWorld.Get()!=W || LastControls.Get()!=RainControls || LastWetness.Get()!=SurfaceWetness || LastRain!=RainAmount || LastWet!=Wetness || LastFlowTiling!=FlowTiling || LastRippleTiling!=RippleTiling || LastFlowStrength!=FlowStrength || LastRippleStrength!=RippleStrength || LastFogActor.Get()!=RainFogActor || LastSpawn!=FullRainSpawnMultiplier || LastLightSpeed!=LightRainSpeed || LastFullSpeed!=FullRainSpeed || LastImpactRate!=FullRainImpactRate || LastFogDensity!=FullRainFogDensity || LastFogOpacity!=FullRainFogOpacity;
 if(!Changed && LastSplashSize==SplashSizeMultiplier && LastSplashFrequency==SplashFrequencyMultiplier)return;
 if(RainControls)
 {
  UKismetMaterialLibrary::SetScalarParameterValue(this,RainControls,TEXT("RainAmount"),RainAmount);
  UKismetMaterialLibrary::SetScalarParameterValue(this,RainControls,TEXT("FlowTiling"),FlowTiling);
  UKismetMaterialLibrary::SetScalarParameterValue(this,RainControls,TEXT("RippleTiling"),RippleTiling);
  UKismetMaterialLibrary::SetScalarParameterValue(this,RainControls,TEXT("FlowStrength"),FlowStrength);
  UKismetMaterialLibrary::SetScalarParameterValue(this,RainControls,TEXT("RippleStrength"),RippleStrength);
  UKismetMaterialLibrary::SetScalarParameterValue(this,RainControls,TEXT("ImpactRate"),RainAmount>0.f?FMath::Lerp(.6f,FMath::Max(.1f,FullRainImpactRate),RainAmount):0.f);
  UKismetMaterialLibrary::SetScalarParameterValue(this,RainControls,TEXT("FlowSpeed"),RainAmount>0.f?FMath::Lerp(.25f,1.8f,RainAmount):0.f);
  UKismetMaterialLibrary::SetScalarParameterValue(this,RainControls,TEXT("SplashSize"),SplashSizeMultiplier);
  UKismetMaterialLibrary::SetScalarParameterValue(this,RainControls,TEXT("SplashFrequency"),SplashFrequencyMultiplier);
 }
 if(SurfaceWetness)UKismetMaterialLibrary::SetScalarParameterValue(this,SurfaceWetness,TEXT("Humidity"),Wetness);
 if(RainSystem)
 {
  RainComponent->SetVariableFloat(TEXT("User.SpawnRateMultiplyer"),RainAmount*FMath::Clamp(FullRainSpawnMultiplier,0.f,8.f));
  RainComponent->SetCustomTimeDilation(FMath::Lerp(FMath::Clamp(LightRainSpeed,.1f,3.f),FMath::Clamp(FullRainSpeed,.1f,3.f),RainAmount));
  if(RainAmount>0.f){RainComponent->SetVisibility(true);if(!RainComponent->IsActive())RainComponent->Activate(false);}
  else {RainComponent->Deactivate();RainComponent->SetVisibility(false);}
 }
 if(RainFogActor)
 {
  UExponentialHeightFogComponent* Fog=RainFogActor->GetComponent();
  Fog->SetVolumetricFog(false);
  Fog->SetFogDensity(FMath::Clamp(FullRainFogDensity,0.f,.1f)*FMath::Pow(RainAmount,1.35f));
  Fog->SetFogMaxOpacity(FMath::Clamp(FullRainFogOpacity,0.f,1.f)*RainAmount);
  Fog->SetStartDistance(150.f);
  Fog->SetFogHeightFalloff(.1f);
  Fog->SetFogInscatteringColor(FLinearColor(.48f,.53f,.58f));
  Fog->SetVisibility(RainAmount>0.f);
 }
 LastWorld=W;LastControls=RainControls;LastWetness=SurfaceWetness;
 LastRain=RainAmount;LastWet=Wetness;LastFlowTiling=FlowTiling;LastRippleTiling=RippleTiling;LastFlowStrength=FlowStrength;LastRippleStrength=RippleStrength;
 LastFogActor=RainFogActor;LastSpawn=FullRainSpawnMultiplier;LastLightSpeed=LightRainSpeed;LastFullSpeed=FullRainSpeed;LastImpactRate=FullRainImpactRate;LastFogDensity=FullRainFogDensity;LastFogOpacity=FullRainFogOpacity;
 LastSplashSize=SplashSizeMultiplier;LastSplashFrequency=SplashFrequencyMultiplier;
}
void ARainSceneController::Tick(float DeltaSeconds)
{
 Super::Tick(DeltaSeconds);ApplyParameters(false);
 if(bFollowPlayerCamera && GetWorld() && GetWorld()->IsGameWorld())
 {
  if(APlayerCameraManager* Camera=UGameplayStatics::GetPlayerCameraManager(this,0))
   RainComponent->SetWorldLocation(Camera->GetCameraLocation()+FVector(0,0,RainHeight));
 }
}
