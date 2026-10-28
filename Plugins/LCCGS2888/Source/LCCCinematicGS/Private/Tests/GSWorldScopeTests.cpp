#include "GSAutomaticWorldScope.h"
#include "GaussianSplatActor.h"
#include "GaussianSplatComponent.h"
#include "GaussianSplatAsset.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGSAutomaticWorldIsolation,
 "LCCCinematicGS.WorldScope.PresenceIsolation",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGSAutomaticWorldIsolation::RunTest(const FString&)
{
 UWorld* Empty=UWorld::CreateWorld(EWorldType::Game,false);
 UWorld* Other=UWorld::CreateWorld(EWorldType::Game,false);
 TestFalse(TEXT("Empty world has no GS"),GSAutomaticWorldScope::HasGS(Empty));
 auto* GS=Other->SpawnActor<AGaussianSplatActor>();
 auto* C=GS->GaussianSplatComponent.Get();
 TestFalse(TEXT("An empty GS actor is not a valid source"),GSAutomaticWorldScope::HasGS(Other));
 auto* Asset=NewObject<UGaussianSplatAsset>();
 TArray<FGaussianSplatData> Splats; Splats.SetNum(1);
 Splats[0].Position=FVector3f::ZeroVector;Splats[0].Scale=FVector3f(1);Splats[0].Opacity=1;
 Asset->InitializeFromSplatData(Splats,EGaussianQualityLevel::Medium);
 C->SetSplatAsset(Asset);
 TestTrue(TEXT("Registered GS source activates only its world"),GSAutomaticWorldScope::HasGS(Other));
 TestFalse(TEXT("Other-world GS never activates an empty world"),GSAutomaticWorldScope::HasGS(Empty));
 C->SetSplatAsset(nullptr);
 TestFalse(TEXT("Removing the last source deactivates automatic GS"),GSAutomaticWorldScope::HasGS(Other));
 Other->DestroyWorld(false);Empty->DestroyWorld(false);
 return true;
}
#endif
