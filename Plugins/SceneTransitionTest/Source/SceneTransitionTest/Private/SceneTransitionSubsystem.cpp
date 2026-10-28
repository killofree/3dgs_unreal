#include "SceneTransitionSubsystem.h"
#include "SceneTransitionSettings.h"
#include "EngineUtils.h"
#include "Engine/Level.h"
#include "Components/MeshComponent.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "UObject/UnrealType.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "MoviePlayer.h"
#include "Widgets/Images/SImage.h"
#include "Brushes/SlateColorBrush.h"
#include "XRLoadingScreenFunctionLibrary.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, SceneTransitionTest)
DEFINE_LOG_CATEGORY_STATIC(LogSceneTransition, Log, All);

void USceneTransitionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    MapLoadedHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &ThisClass::OnMapLoaded);
    if (GEngine)
    {
        TravelFailureHandle = GEngine->OnTravelFailure().AddWeakLambda(this,
            [this](UWorld* World, ETravelFailure::Type, const FString& Error)
            {
                if (bEnabled && World && World->GetGameInstance() == GetGameInstance())
                {
                    Phase = EPhase::Failed;
                    UE_LOG(LogSceneTransition, Error, TEXT("Travel failed; cover held: %s"), *Error);
                }
            });
    }
    bAutoRoundTrip = FParse::Param(FCommandLine::Get(), TEXT("SceneTransitionAuto"));
    if (bStartTestOnLaunch || FParse::Param(FCommandLine::Get(), TEXT("SceneTransitionTest"))) StartTest();
}

void USceneTransitionSubsystem::Deinitialize()
{
    FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(MapLoadedHandle);
    if (GEngine) GEngine->OnTravelFailure().Remove(TravelFailureHandle);
    if (bEnabled) UXRLoadingScreenFunctionLibrary::HideLoadingScreen();
    bEnabled = false;
    Super::Deinitialize();
}

void USceneTransitionSubsystem::StartTest()
{
    if (bEnabled || IsRunningCommandlet()) return;
    bEnabled = true;
    Phase = EPhase::Warmup;
    Elapsed = 0;
    ShowLoadingCover();
    UE_LOG(LogSceneTransition, Display, TEXT("Test enabled. N / right primary / left primary: next map. Auto=%d"), bAutoRoundTrip);
}

void USceneTransitionSubsystem::ApplyFade(float Amount)
{
    if (APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0))
        if (PC->PlayerCameraManager)
            PC->PlayerCameraManager->SetManualCameraFade(Amount, FadeColor, true);
}

void USceneTransitionSubsystem::ShowLoadingCover()
{
    // Compositor loading cover remains submitted while OpenLevel blocks the game thread.
    CoverTexture = UTexture2D::CreateTransient(4, 4, PF_B8G8R8A8);
    if (CoverTexture)
    {
        FColor* Pixels = static_cast<FColor*>(CoverTexture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE));
        for (int32 Index = 0; Index < 16; ++Index) Pixels[Index] = FadeColor.ToFColor(true);
        CoverTexture->GetPlatformData()->Mips[0].BulkData.Unlock();
        CoverTexture->UpdateResource();
        UXRLoadingScreenFunctionLibrary::SetLoadingScreen(CoverTexture, FVector2D(20,20), FVector(1,0,0));
        UXRLoadingScreenFunctionLibrary::ShowLoadingScreen();
    }
    if (IsMoviePlayerEnabled())
    {
        // Desktop loading cover; XR compositor behavior still requires HMD verification.
        static FSlateColorBrush Brush(FLinearColor::Black);
        Brush.TintColor = FSlateColor(FadeColor);
        FLoadingScreenAttributes Attributes;
        Attributes.bAutoCompleteWhenLoadingCompletes = true;
        Attributes.bMoviesAreSkippable = false;
        Attributes.WidgetLoadingScreen = SNew(SImage).Image(&Brush);
        GetMoviePlayer()->SetupLoadingScreen(Attributes);
    }
}

void USceneTransitionSubsystem::TravelToNextMap()
{
    if (!bEnabled || Phase != EPhase::Ready) return;
    const FString Current = UGameplayStatics::GetCurrentLevelName(GetWorld(), true);
    const auto& Maps = GetDefault<USceneTransitionSettings>()->MapCycle;
    if (Maps.IsEmpty())
    {
        UE_LOG(LogSceneTransition, Error, TEXT("MapCycle is empty; configure Project Settings > Plugins > Scene Transition."));
        return;
    }
    const int32 CurrentIndex = Maps.IndexOfByPredicate([&Current](const FString& Map)
        { return FPackageName::GetShortName(Map) == Current; });
    const auto* Settings = GetDefault<USceneTransitionSettings>();
    TravelToMap(Maps[(CurrentIndex + 1) % Maps.Num()], Settings->FadeOutSeconds, Settings->FadeInSeconds, FadeColor);
}

bool USceneTransitionSubsystem::TravelToMap(const FString& Map, float FadeOutTime, float FadeInTime, FLinearColor Color)
{
    if (!bEnabled || Phase != EPhase::Ready) return false;
    if (!FPackageName::DoesPackageExist(Map))
    {
        UE_LOG(LogSceneTransition, Error, TEXT("Destination missing or not cooked: %s"), *Map);
        return false;
    }
    PendingMap = Map;
    TravelFadeOut = FMath::Max(0.f, FadeOutTime);
    TravelFadeIn = FMath::Max(0.f, FadeInTime);
    FadeColor = Color;
    Phase = EPhase::FadeOut;
    Elapsed = 0;
    UE_LOG(LogSceneTransition, Display, TEXT("Fade out -> %s"), *PendingMap);
    return true;
}

void USceneTransitionSubsystem::OnMapLoaded(UWorld* World)
{
    if (!bEnabled || !World || World->GetGameInstance() != GetGameInstance()) return;
    Phase = EPhase::Warmup;
    Elapsed = 0;
    StableFrames = 0;
    bWarned = false;
    FirstCutAppliedFrame = MAX_uint64;
    bFirstCutLogged = false;
    ApplyFade(1);
    UE_LOG(LogSceneTransition, Display, TEXT("Map loaded: %s; warmup begins"), *World->GetName());
}

bool USceneTransitionSubsystem::ApplyFirstCutState()
{
    const auto* Settings = GetDefault<USceneTransitionSettings>();
    if (!Settings->bApplyFirstCutState || CompletedTrips != 0 ||
        FName(*UGameplayStatics::GetCurrentLevelName(GetWorld(), true)) != Settings->FirstCutMap) return true;

    TArray<AActor*> Actors;
    for (TActorIterator<AActor> It(GetWorld()); It; ++It) Actors.Add(*It);
    bool bReady = true;
    int32 HiddenMeshes = 0;
    int32 DisabledProperties = 0;
    TSet<AActor*> Visited;
    TFunction<bool(AActor*)> HideMeshes = [&](AActor* Actor)
    {
        if (!IsValid(Actor) || Visited.Contains(Actor)) return true;
        Visited.Add(Actor);
        TInlineComponentArray<UMeshComponent*> Meshes(Actor);
        for (UMeshComponent* Mesh : Meshes)
        {
            if (!Mesh->bHiddenInGame) Mesh->SetHiddenInGame(true);
            if (Mesh->IsVisible()) Mesh->SetVisibility(false);
            if (Mesh->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
                Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            ++HiddenMeshes;
        }
        bool bChildrenReady = true;
        if (ILevelInstanceInterface* Instance = Cast<ILevelInstanceInterface>(Actor))
        {
            ULevel* Loaded = Instance->GetLoadedLevel();
            if (!Loaded) return false;
            for (AActor* Child : Loaded->Actors) bChildrenReady = HideMeshes(Child) && bChildrenReady;
        }
        TArray<AActor*> Attached;
        Actor->GetAttachedActors(Attached);
        for (AActor* Child : Attached) bChildrenReady = HideMeshes(Child) && bChildrenReady;
        return bChildrenReady;
    };
    for (const FString& Prefix : Settings->FirstCutHiddenActorPrefixes)
    {
        int32 Matches = 0;
        for (AActor* Actor : Actors)
            if (!Prefix.IsEmpty() && Actor->GetName().StartsWith(Prefix))
            {
                ++Matches;
                bReady = HideMeshes(Actor) && bReady;
            }
        bReady = Matches == 1 && bReady;
    }
    for (const auto& Binding : Settings->FirstCutDisabledBoolProperties)
    {
        int32 Matches = 0;
        for (AActor* Actor : Actors)
            if (!Binding.Key.IsEmpty() && Actor->GetName().StartsWith(Binding.Key))
            {
                ++Matches;
                FBoolProperty* Property = FindFProperty<FBoolProperty>(Actor->GetClass(), Binding.Value);
                if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit)) { bReady = false; continue; }
                Property->SetPropertyValue_InContainer(Actor, false);
                ++DisabledProperties;
            }
        bReady = Matches == 1 && bReady;
    }
    if (!bReady) return false;
    if (!bFirstCutLogged)
    {
        bFirstCutLogged = true;
        FirstCutAppliedFrame = GFrameCounter;
        UE_LOG(LogSceneTransition, Display, TEXT("FirstCut initialized: hidden meshes=%d, disabled bools=%d; transforms preserved"), HiddenMeshes, DisabledProperties);
    }
    // Give the target actor's Tick and render thread time to publish disabled removal.
    return GFrameCounter > FirstCutAppliedFrame + 1;
}

void USceneTransitionSubsystem::Tick(float DeltaTime)
{
    if (!GetWorld() || !GetWorld()->IsGameWorld()) return;
    Elapsed += DeltaTime;
    APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
    if (PC && PC->WasInputKeyJustPressed(EKeys::Escape))
    {
        UKismetSystemLibrary::QuitGame(GetWorld(), PC, EQuitPreference::Quit, false);
        return;
    }
    // Idle preview handles only explicit keyboard input. Never fade, relocate, or apply first-cut state.
    if (!bEnabled)
    {
        if (PC && PC->GetPawn() && PC->PlayerCameraManager)
        {
            const float Forward = float(PC->IsInputKeyDown(EKeys::Up)) - float(PC->IsInputKeyDown(EKeys::Down));
            const float Right = float(PC->IsInputKeyDown(EKeys::Right)) - float(PC->IsInputKeyDown(EKeys::Left));
            const FRotator Heading(0, PC->PlayerCameraManager->GetCameraRotation().Yaw, 0);
            const FVector Direction = Heading.RotateVector(FVector(Forward,Right,0)).GetClampedToMaxSize(1);
            PC->GetPawn()->AddActorWorldOffset(Direction * FMath::Max(0.f,KeyboardMoveSpeed) * DeltaTime,true);
        }
        if (PC && PC->WasInputKeyJustPressed(EKeys::N))
        { bEnabled = true; Phase = EPhase::Ready; TravelToNextMap(); }
        return;
    }
    const USceneTransitionSettings* Settings = GetDefault<USceneTransitionSettings>();
    // Explicit scene state must not rely on an editor-only preview helper.
    if (Phase == EPhase::Warmup) for (const auto& Visibility : Settings->ActorHiddenOverrides)
    {
        if (Visibility.Key.IsEmpty()) continue;
        for (TActorIterator<AActor> It(GetWorld()); It; ++It)
        {
            if (It->GetName().StartsWith(Visibility.Key) && It->IsHidden() != Visibility.Value)
            {
                It->SetActorHiddenInGame(Visibility.Value);
                UE_LOG(LogSceneTransition, Display, TEXT("Actor visibility applied: %s HiddenInGame=%d"), *It->GetName(), Visibility.Value);
            }
        }
    }
    const bool bFirstCutReady = Phase != EPhase::Warmup || ApplyFirstCutState();
    const float Duration = FMath::Max(KINDA_SMALL_NUMBER,
        Phase == EPhase::FadeOut ? (TravelFadeOut >= 0 ? TravelFadeOut : Settings->FadeOutSeconds) : (TravelFadeIn >= 0 ? TravelFadeIn : Settings->FadeInSeconds));
    if (Phase == EPhase::Warmup)
    {
        ApplyFade(1);
        const bool bCameraReady = PC && PC->PlayerCameraManager && PC->GetPawn();
        const FName MapName(*UGameplayStatics::GetCurrentLevelName(GetWorld(), true));
        const float* MapWarmup = Settings->WarmupSecondsByMap.Find(MapName);
        const float WarmupSeconds = FMath::Max(0.f, MapWarmup ? *MapWarmup : Settings->DefaultWarmupSeconds);
        StableFrames = bCameraReady && DeltaTime * 1000 <= StableFrameMilliseconds ? StableFrames + 1 : 0;
        const bool bFrameRateReady = !bRequireStableFrameRate || StableFrames >= FMath::Max(1, RequiredStableFrames);
        if (bCameraReady && bFirstCutReady && Elapsed >= WarmupSeconds && bFrameRateReady)
        {
            UXRLoadingScreenFunctionLibrary::HideLoadingScreen();
            Phase = bDirectorOwnsInitialFade ? EPhase::Ready : EPhase::FadeIn;
            bDirectorOwnsInitialFade = false;
            Elapsed = 0;
            UE_LOG(LogSceneTransition, Display, TEXT("Warmup complete; fade in (frame rate gate=%d)"), bRequireStableFrameRate);
        }
        else if (Elapsed >= MaximumWarmupSeconds && !bWarned)
        {
            bWarned = true;
            UE_LOG(LogSceneTransition, Warning, TEXT("Warmup timeout; waiting under cover. CameraReady=%d FirstCutReady=%d FrameRateReady=%d"), bCameraReady, bFirstCutReady, bFrameRateReady);
        }
    }
    else if (Phase == EPhase::FadeIn)
    {
        ApplyFade(1 - FMath::Clamp(Elapsed / Duration, 0.f, 1.f));
        if (Elapsed >= Duration)
        {
            Phase = EPhase::Ready;
            Elapsed = 0;
            UE_LOG(LogSceneTransition, Display, TEXT("READY: %s (completed trips %d)"), *GetWorld()->GetName(), CompletedTrips);
        }
    }
    else if (Phase == EPhase::Ready)
    {
        if (bFlowControl) return;
        if (PC && PC->GetPawn() && PC->PlayerCameraManager)
        {
            const float ForwardInput = float(PC->IsInputKeyDown(EKeys::Up)) - float(PC->IsInputKeyDown(EKeys::Down));
            const float RightInput = float(PC->IsInputKeyDown(EKeys::Right)) - float(PC->IsInputKeyDown(EKeys::Left));
            const FRotator Heading(0, PC->PlayerCameraManager->GetCameraRotation().Yaw, 0);
            const FVector Direction = Heading.RotateVector(FVector(ForwardInput, RightInput, 0)).GetClampedToMaxSize(1);
            PC->GetPawn()->AddActorWorldOffset(Direction * FMath::Max(0.f, KeyboardMoveSpeed) * DeltaTime, true);
        }
        const bool bHeld = PC && (PC->IsInputKeyDown(EKeys::N) || PC->IsInputKeyDown(FKey(TEXT("OculusTouch_Right_FaceButton1"))) || PC->IsInputKeyDown(FKey(TEXT("OculusTouch_Left_FaceButton1"))));
        if ((bHeld && !bNextHeld) || (bAutoRoundTrip && CompletedTrips < Settings->MapCycle.Num() && Elapsed >= FMath::Max(0.f, Settings->AutoAdvanceSeconds))) TravelToNextMap();
        bNextHeld = bHeld;
    }
    else if (Phase == EPhase::FadeOut)
    {
        ApplyFade(FMath::Clamp(Elapsed / Duration, 0.f, 1.f));
        if (Elapsed >= Duration)
        {
            ShowLoadingCover();
            Phase = EPhase::Loading;
            Elapsed = 0;
        }
    }
    else if (Phase == EPhase::Loading)
    {
        ApplyFade(1);
        // Allow the compositor a few frames to receive the cover before blocking.
        if (Elapsed >= .25f)
        {
            ++CompletedTrips;
            const FString Destination = PendingMap;
            Phase = EPhase::Failed; // PostLoadMap changes this to Warmup on success.
            UGameplayStatics::OpenLevel(GetWorld(), FName(*Destination));
        }
    }
    else if (Phase == EPhase::Failed) ApplyFade(1);
}
