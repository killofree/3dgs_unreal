#include "RainSceneController.h"
#include "SceneEffectLibrary.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "GaussianSplatActor.h"
#include "GaussianSplatComponent.h"
#include "UObject/UnrealType.h"
#include "Engine/World.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Components/ExponentialHeightFogComponent.h"

namespace
{
UObject* Source(AActor* Actor)
{
    const FObjectPropertyBase* P=IsValid(Actor)?FindFProperty<FObjectPropertyBase>(Actor->GetClass(),TEXT("GaussianSplatComponent")):nullptr;
    return P?P->GetObjectPropertyValue_InContainer(Actor):nullptr;
}
void Float(UObject* O,const TCHAR* Key,float V){FString E;USceneEffectLibrary::WriteFloat(O,Key,V,E);}
void Bool(UObject* O,const TCHAR* Key,bool V){FString E;USceneEffectLibrary::WriteBool(O,Key,V,E);}
void Enum(UObject* O,const TCHAR* Key,const FString& V){FString E;USceneEffectLibrary::WriteEnum(O,Key,V,E);}
void Color(UObject* O,FName Key,FLinearColor V)
{
    FStructProperty* P=O?FindFProperty<FStructProperty>(O->GetClass(),Key):nullptr;
    if(P && P->Struct==TBaseStructure<FLinearColor>::Get()) *P->ContainerPtrToValuePtr<FLinearColor>(O)=V;
}
void SnowSystem(UObject* O,UNiagaraSystem* System)
{
    if(FObjectPropertyBase* P=O?FindFProperty<FObjectPropertyBase>(O->GetClass(),TEXT("SnowWeatherNiagara")):nullptr)
        P->SetObjectPropertyValue_InContainer(O,System);
}
void RainSystemForGS(UObject* O,UNiagaraSystem* System)
{
    if(FObjectPropertyBase* P=O?FindFProperty<FObjectPropertyBase>(O->GetClass(),TEXT("RainWeatherNiagara")):nullptr)
        P->SetObjectPropertyValue_InContainer(O,System);
}

}
void ARainSceneController::CaptureSeasonSequenceBaseline()
{
    UObject* GS=Source(GaussianTarget);if(!GS)return;
    SeasonBaselineTarget=GaussianTarget;SeasonBaselineFloats.Reset();SeasonBaselineColors.Reset();SeasonBaselineBools.Reset();
    const TCHAR* Floats[]={TEXT("SeasonSaturationScale"),TEXT("SeasonBrightnessScale"),TEXT("SeasonReplacementStrength"),TEXT("SeasonOriginalDetailPreservation"),TEXT("SeasonTargetStartColorBias"),TEXT("FogWeatherStrength"),TEXT("ProxySurfaceEffectStrength")};
    for(const TCHAR* Key:Floats){float V;FString E;if(USceneEffectLibrary::ReadFloat(GS,Key,V,E))SeasonBaselineFloats.Add(Key,V);}
    const TCHAR* Colors[]={TEXT("SeasonTargetColorFrom"),TEXT("SeasonTargetColorTo")};
    for(const TCHAR* Key:Colors) if(FStructProperty* P=FindFProperty<FStructProperty>(GS->GetClass(),Key))
        if(P->Struct==TBaseStructure<FLinearColor>::Get())SeasonBaselineColors.Add(Key,*P->ContainerPtrToValuePtr<FLinearColor>(GS));
    for(const TCHAR* Key:{TEXT("bEnableSeasonalColor"),TEXT("bEnableFogOverlayInOtherWeather")})
    {bool V;FString E;if(USceneEffectLibrary::ReadBool(GS,Key,V,E))SeasonBaselineBools.Add(Key,V);}
    FString E;USceneEffectLibrary::ReadFloat(GaussianTarget,TEXT("SnowRuntimeHeightCm2878"),SeasonBaselineSnowHeight,E);
    USceneEffectLibrary::ReadEnum(GS,TEXT("WeatherMode"),SeasonBaselineWeather,E);
    USceneEffectLibrary::ReadEnum(GS,TEXT("ProxySurfaceEffectMode"),SeasonBaselineSurface,E);
    if(FObjectPropertyBase* P=FindFProperty<FObjectPropertyBase>(GS->GetClass(),TEXT("SnowWeatherNiagara")))
        SeasonBaselineSnowSystem=Cast<UNiagaraSystem>(P->GetObjectPropertyValue_InContainer(GS));
    if(FObjectPropertyBase* P=FindFProperty<FObjectPropertyBase>(GS->GetClass(),TEXT("RainWeatherNiagara")))
        SeasonBaselineRainSystem=Cast<UNiagaraSystem>(P->GetObjectPropertyValue_InContainer(GS));
}
void ARainSceneController::RestoreSeasonSequenceBaseline()
{
    UObject* GS=Source(SeasonBaselineTarget);if(!GS)return;
    for(const auto& V:SeasonBaselineFloats)Float(GS,*V.Key.ToString(),V.Value);
    for(const auto& V:SeasonBaselineColors)Color(GS,V.Key,V.Value);
    for(const auto& V:SeasonBaselineBools)Bool(GS,*V.Key.ToString(),V.Value);
    Enum(GS,TEXT("WeatherMode"),SeasonBaselineWeather);Enum(GS,TEXT("ProxySurfaceEffectMode"),SeasonBaselineSurface);
    SnowSystem(GS,SeasonBaselineSnowSystem);
    RainSystemForGS(GS,SeasonBaselineRainSystem);
    Float(SeasonBaselineTarget,TEXT("SnowRuntimeHeightCm2878"),SeasonBaselineSnowHeight);
    LastRain=-1.f;
    if(SequenceSnowComponent)SequenceSnowComponent->Deactivate();
    if(SeasonBaselineTarget)SeasonBaselineTarget->Tags.Remove(TEXT("SeasonSequenceSmoothSnow"));
    if(RainComponent)RainComponent->ComponentTags.Remove(TEXT("SeasonSequencePrecipitation"));
    if(SeasonBaselineTarget)SeasonBaselineTarget->RemoveTickPrerequisiteActor(this);
    bSeasonSequenceWasActive=false;
}
void ARainSceneController::ApplySeasonSequence()
{
    if(!bEnableSeasonSequence || !GaussianTarget)
    {if(bSeasonSequenceWasActive)RestoreSeasonSequenceBaseline();return;}
    if(SeasonBaselineTarget!=GaussianTarget || SeasonBaselineFloats.IsEmpty())
    {if(bSeasonSequenceWasActive)RestoreSeasonSequenceBaseline();CaptureSeasonSequenceBaseline();}
    UObject* GS=Source(GaussianTarget);if(!GS){SeasonSequenceStatus=TEXT("Target has no GaussianSplatComponent");return;}
    bSeasonSequenceWasActive=true;GaussianTarget->AddTickPrerequisiteActor(this);
    GaussianTarget->Tags.AddUnique(TEXT("SeasonSequenceSmoothSnow"));
    RainComponent->ComponentTags.AddUnique(TEXT("SeasonSequencePrecipitation"));
    const float W=FMath::Clamp(WinterBlend,0.f,1.f);
    // Absolute, baseline-relative interpolation. Never integrate DeltaTime:
    // Sequencer seeks produce the same color and requested snow height.
    // The sequence does not rewrite the source scan colour grade.
    const float Snow=FMath::Clamp(SnowfallIntensity,0.f,1.f);
    const float Height=FMath::Clamp(SnowAccumulationHeightCm,0.f,50.f);
    const bool ActiveSnow=Snow>0.001f || Height>0.001f;
    const bool ActiveRain=!ActiveSnow && RainAmount>0.001f;
    Enum(GS,TEXT("WeatherMode"),ActiveSnow?TEXT("Snow"):(ActiveRain?TEXT("Rain"):*SeasonBaselineWeather));
    Enum(GS,TEXT("ProxySurfaceEffectMode"),ActiveSnow?TEXT("Snow"):*SeasonBaselineSurface);
    Float(GaussianTarget,TEXT("SnowRuntimeHeightCm2878"),Height);
    float Maximum=7.f;FString Error;
    USceneEffectLibrary::ReadFloat(GaussianTarget,TEXT("SnowBakeMaximumHeightCm2878"),Maximum,Error);
    Float(GS,TEXT("ProxySurfaceEffectStrength"),ActiveSnow?FMath::Clamp(Height/FMath::Max(Maximum,1.f),0.f,1.f):SeasonBaselineFloats.FindRef(TEXT("ProxySurfaceEffectStrength")));
    // Native GS weather owns precipitation. Never clear its authored system references.
    const float RainFog=bLinkHeavyRainFog?HeavyRainFogStrength*FMath::SmoothStep(0.f,1.f,FMath::Clamp(RainAmount,0.f,1.f)):0.f;
    const float Fog=FMath::Clamp(FMath::Max(SequenceFogStrength,RainFog),0.f,2.f);
    Bool(GS,TEXT("bEnableFogOverlayInOtherWeather"),Fog>0.001f);
    Float(GS,TEXT("FogWeatherStrength"),Fog);
    const TPair<FName,float> FogValues[]={ {TEXT("AtmosphereStrength"),1.38f},{TEXT("GroundFogDensity"),1.12f},{TEXT("DistanceHaze"),1.55f},{TEXT("FogHeightFalloff"),1.65f},{TEXT("FogDensityVariation"),.38f} };
    for(const auto& V:FogValues)
    {
        if(!SeasonBaselineFloats.Contains(V.Key))
        {float Base=0.f;FString E;if(USceneEffectLibrary::ReadFloat(GS,V.Key.ToString(),Base,E))SeasonBaselineFloats.Add(V.Key,Base);}
        if(const float* Base=SeasonBaselineFloats.Find(V.Key))
            Float(GS,*V.Key.ToString(),FMath::Lerp(*Base,V.Value,FMath::Clamp(Fog,0.f,1.f)));
    }
    // One fog authority. GS synchronizes its native mesh fog and late GS bridge.
    if(RainFogActor)RainFogActor->GetComponent()->SetVisibility(false);
    // The current GS proxy captures OpacityScale at creation. Refresh only the
    // auxiliary snow proxy in small visible increments, never the source GS.
    if(FObjectPropertyBase* P=FindFProperty<FObjectPropertyBase>(GaussianTarget->GetClass(),TEXT("SnowGaussianComponent")))
    {
        if(UPrimitiveComponent* SnowLayer=Cast<UPrimitiveComponent>(P->GetObjectPropertyValue_InContainer(GaussianTarget)))
        {
            float Opacity=1.f;FString E;
            if(USceneEffectLibrary::ReadFloat(SnowLayer,TEXT("OpacityScale"),Opacity,E) &&
                !FMath::IsNearlyEqual(Opacity,LastSequenceSnowOpacity))
            {
                SnowLayer->MarkRenderDynamicDataDirty();
                LastSequenceSnowOpacity=Opacity;
            }
        }
    }
    const FArrayProperty* Stages=FindFProperty<FArrayProperty>(GaussianTarget->GetClass(),TEXT("BakedSnowHeightStages2878"));
    const int32 Count=Stages?FScriptArrayHelper(Stages,Stages->ContainerPtrToValuePtr<void>(GaussianTarget)).Num():0;
    SeasonSequenceStatus=FString::Printf(TEXT("Winter %.2f | Snow %.2f | Height %.1fcm | Fog %.2f | baked stages %d%s"),W,Snow,Height,Fog,Count,Count==0?TEXT(" (snow relief bake required)"):TEXT(""));
}

// Read the real plugin fields and reuse its weather component, view anchor and
// composite path. The mesh-only controller remains available for non-GS levels.
bool ARainSceneController::ApplyPluginPrecipitation()
{
    AGaussianSplatActor* Target=Cast<AGaussianSplatActor>(GaussianTarget.Get());
    if(!Target || !Target->GaussianSplatComponent || !Target->WeatherNiagaraComponent)return false;
    UGaussianSplatComponent* GS=Target->GaussianSplatComponent;
    const bool bSnow=bEnableSeasonSequence && (SnowfallIntensity>0.001f || SnowAccumulationHeightCm>0.001f);
    if(RainSystem)GS->RainWeatherNiagara=RainSystem;
    if(SequenceSnowSystem)GS->SnowWeatherNiagara=SequenceSnowSystem;
    GS->WeatherMode=bSnow?ELCCCinematicGSWeatherMode::Snow:
        (RainAmount>0.001f?ELCCCinematicGSWeatherMode::Rain:ELCCCinematicGSWeatherMode::Clear);
    GS->bRainPrecipitationEnabled=!bSnow && RainAmount>0.001f;
    // Native fog covers both GS and ordinary geometry during the original opening too.
    const float RainFog=(!bSnow && bLinkHeavyRainFog)?HeavyRainFogStrength*FMath::SmoothStep(0.f,1.f,FMath::Clamp(RainAmount,0.f,1.f)):0.f;
    const float Fog=FMath::Max(RainFog,bEnableSeasonSequence?SequenceFogStrength:0.f);
    GS->bEnableFogOverlayInOtherWeather=Fog>0.001f;
    GS->FogWeatherStrength=Fog;
    if(RainFogActor)RainFogActor->GetComponent()->SetVisibility(false);
    if(bEnableSeasonSequence)Target->Tags.AddUnique(TEXT("SeasonSequenceWorldSnow"));
    GS->SetCustomPrimitiveDataFloat(4,bSnow?0.f:FMath::Clamp(RainAmount,0.f,1.f));
    Target->AddTickPrerequisiteActor(this);
    // This is the rain system's actual exposed parameter, including its authored spelling.
    Target->WeatherNiagaraComponent->SetVariableFloat(TEXT("User.SpawnRateMultiplyer"),
        FMath::Clamp(RainAmount,0.f,1.f)*FullRainSpawnMultiplier);
    Target->WeatherNiagaraComponent->SetVariableFloat(TEXT("User.SeasonSnowSpawnRate"),
        700.f*FMath::Clamp(SnowfallIntensity,0.f,1.f));
    Target->WeatherNiagaraComponent->SetCustomTimeDilation(bSnow?1.f:FMath::Lerp(LightRainSpeed,FullRainSpeed,FMath::Clamp(RainAmount,0.f,1.f)));
    if(RainComponent){RainComponent->DeactivateImmediate();RainComponent->SetVisibility(false);}
    if(SequenceSnowComponent){SequenceSnowComponent->DeactivateImmediate();SequenceSnowComponent->SetVisibility(false);}
    return true;
}
