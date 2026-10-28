#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SceneEffectLibrary.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneEffectReadWriteTest, "SceneEffects.ParameterReadWrite",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSceneEffectReadWriteTest::RunTest(const FString& Parameters)
{
    FString Error; float Number = 0;
    TestFalse(TEXT("Missing object is reported"), USceneEffectLibrary::ReadFloat(nullptr, TEXT("Missing"), Number, Error));
    TestFalse(TEXT("Missing target returns a reason"), Error.IsEmpty());
    UClass* GSClass = FindObject<UClass>(nullptr, TEXT("/Script/LCCCinematicGS.GaussianSplatComponent"));
    if (GSClass)
    {
        UObject* GS = NewObject<UObject>(GetTransientPackage(), GSClass);
        TestTrue(TEXT("Read actual GS fog strength"), USceneEffectLibrary::ReadFloat(GS, TEXT("FogWeatherStrength"), Number, Error));
        TestTrue(TEXT("Write actual GS fog strength"), USceneEffectLibrary::WriteFloat(GS, TEXT("FogWeatherStrength"), .4f, Error));
        USceneEffectLibrary::ReadFloat(GS, TEXT("FogWeatherStrength"), Number, Error);
        TestEqual(TEXT("Actual GS value readback"), Number, .4f);
        TestTrue(TEXT("Set actual weather enum by native name"), USceneEffectLibrary::WriteEnum(GS, TEXT("WeatherMode"), TEXT("Fog"), Error));
        FString Mode;
        TestTrue(TEXT("Read actual weather enum"), USceneEffectLibrary::ReadEnum(GS, TEXT("WeatherMode"), Mode, Error));
        TestEqual(TEXT("Actual weather enum readback"), Mode, FString(TEXT("Fog")));
        TestFalse(TEXT("Wrong property type is rejected"), USceneEffectLibrary::WriteFloat(GS, TEXT("WeatherMode"), 1, Error));
        TestFalse(TEXT("Misspelled property is rejected"), USceneEffectLibrary::WriteFloat(GS, TEXT("FogStrengthTypo"), 1, Error));
        TestTrue(TEXT("Lists actual available properties"), USceneEffectLibrary::ListParameters(GS).ContainsByPredicate([](const auto& P) { return P.Path == TEXT("FogWeatherStrength"); }));
    }
    else AddWarning(TEXT("GS plugin is not loaded; GS-specific adapter test skipped."));

    UNiagaraSystem* System = NewObject<UNiagaraSystem>();
    const FNiagaraVariable Rate(FNiagaraTypeDefinition::GetFloatDef(), TEXT("User.SpawnRateMultiplyer"));
    System->GetExposedParameters().SetParameterValue<float>(3.f, Rate, true);
    const FNiagaraVariable Count(FNiagaraTypeDefinition::GetIntDef(), TEXT("User.Count"));
    System->GetExposedParameters().SetParameterValue<int32>(2, Count, true);
    UNiagaraComponent* Niagara = NewObject<UNiagaraComponent>();
    Niagara->SetAsset(System);
    TestTrue(TEXT("Read Niagara default before any explicit override"), USceneEffectLibrary::ReadNiagaraFloat(Niagara, TEXT("SpawnRateMultiplyer"), Number, Error));
    TestEqual(TEXT("Reads Niagara system default"), Number, 3.f);
    TestTrue(TEXT("Write existing Niagara parameter"), USceneEffectLibrary::WriteNiagaraFloat(Niagara, TEXT("User.SpawnRateMultiplyer"), 5.f, Error));
    USceneEffectLibrary::ReadNiagaraFloat(Niagara, TEXT("User.SpawnRateMultiplyer"), Number, Error);
    TestEqual(TEXT("Niagara override readback"), Number, 5.f);
    TestFalse(TEXT("Do not silently create typo overrides"), USceneEffectLibrary::WriteNiagaraFloat(Niagara, TEXT("User.Typo"), 4.f, Error));
    TestFalse(TEXT("Reject mismatched Niagara types"), USceneEffectLibrary::WriteNiagaraFloat(Niagara, TEXT("User.Count"), 4.f, Error));
    return true;
}
#endif
