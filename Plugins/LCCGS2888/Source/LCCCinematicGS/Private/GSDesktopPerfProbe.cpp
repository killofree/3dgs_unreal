#include "GSDesktopPerfProbe.h"
#include "GSAutomaticWorldScope.h"
#include "GaussianSplatActor.h"
#include "GaussianSplatComponent.h"
#include "GaussianSplatAsset.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "UObject/UObjectIterator.h"
#include "Engine/Engine.h"
#include "UnrealClient.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/PlatformMisc.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerStart.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/SceneCaptureComponent.h"
#include "Engine/StaticMeshActor.h"
#include "InputCoreTypes.h"
#include "IXRTrackingSystem.h"

#include "GSHitchTelemetry.inl"

static TAutoConsoleVariable<int32> CVarEditorVRPreview(TEXT("gs.PCVR.EditorPreview"),0,TEXT("Enable a manual PIE VR-preview helper: authored scene, PlayerStart, GS-only visibility, keyboard movement, no timed run or asset replacement."),ECVF_Default);

// Standalone-only diagnostic scene isolation. No editor/map save or asset edit.
static int32 GetGSSceneCompareMode()
{
    static const int32 Mode=[](){int32 V=0;FParse::Value(FCommandLine::Get(),TEXT("GSSceneCompare="),V);return FMath::Clamp(V,0,3);}();
    return Mode; // 1 GS only, 2 native meshes only, 3 full scene, 0 unchanged.
}
static bool ApplyGSSceneCompare(UWorld* World,bool bLog)
{
    const int32 Mode=GetGSSceneCompareMode();
    if(Mode==0)return true;
    if(!World || GIsEditor || !World->IsGameWorld())return false;
    int32 GS=0,GSVisible=0,Native=0,Hidden=0,ProxyKept=0,TransformChanges=0;
    for(TActorIterator<AActor> It(World);It;++It)
    {
        AActor* A=*It;
        if(auto* G=Cast<AGaussianSplatActor>(A))
        {
            auto* C=G->GaussianSplatComponent.Get();if(!C || !C->GetSplatAsset())continue;
            ++GS;
            if(Mode==2)
            {
                G->SetActorHiddenInGame(true);C->SetHiddenInGame(true,false);C->SetVisibility(false,false);
                G->SetActorTickEnabled(false);
                TArray<UActorComponent*> Components;G->GetComponents(Components);
                for(auto* Item:Components)
                {
                    Item->SetComponentTickEnabled(false);
                    if(auto* Capture=Cast<USceneCaptureComponent>(Item))
                    {Capture->bCaptureEveryFrame=false;Capture->bCaptureOnMovement=false;Capture->Deactivate();}
                }
            }
            GSVisible+=!G->IsHidden() && C->IsVisible() && !C->bHiddenInGame?1:0;
            continue;
        }
        bool GSOwned=false,PlayerOwned=false;
        for(AActor* Owner=A;Owner;Owner=Owner->GetOwner())
        {GSOwned|=Owner->IsA<AGaussianSplatActor>();PlayerOwned|=Owner->IsA<APawn>();}
        for(AActor* Parent=A->GetAttachParentActor();Parent;Parent=Parent->GetAttachParentActor())
        {GSOwned|=Parent->IsA<AGaussianSplatActor>();PlayerOwned|=Parent->IsA<APawn>();}
        // Keep the same sky and XR controller presentation in all three runs.
        const bool Sky=A->GetName().Contains(TEXT("SkySphere")) || A->GetClass()->GetName().Contains(TEXT("SkySphere"));
        if(GSOwned || PlayerOwned || Sky)continue;
        TArray<UPrimitiveComponent*> Primitives;A->GetComponents(Primitives);
        for(auto* C:Primitives)
        {
            const bool Landscape=A->GetClass()->GetName().Contains(TEXT("Landscape"));
            if(!C->IsA<UStaticMeshComponent>() && !C->IsA<USkinnedMeshComponent>() && !Landscape)continue;
            const auto* Mesh=Cast<UStaticMeshComponent>(C);
            if(C->bVisibleInSceneCaptureOnly || (Mesh && GetPathNameSafe(Mesh->GetStaticMesh()).Contains(TEXT("/collider/")))){++ProxyKept;continue;}
            ++Native;
            const FTransform Before=C->GetComponentTransform();
            if(Mode==1){C->SetHiddenInGame(true,false);C->SetCastShadow(false);}
            Hidden+=C->bHiddenInGame?1:0;
            TransformChanges+=Before.Equals(C->GetComponentTransform(),.001)?0:1;
        }
    }
    const bool NativeSceneTest=FParse::Param(FCommandLine::Get(),TEXT("GSVRNativeSceneTest"));
    const bool SceneContentValid=NativeSceneTest ? (Mode==3 && GS==0) : GS>0;
    const bool Valid=SceneContentValid && Native>0 && TransformChanges==0 && (Mode==2?GSVisible==0:GSVisible==GS) && (Mode!=1 || Hidden==Native);
    if(bLog)UE_LOG(LogTemp,Display,TEXT("[GSSceneCompare][State] Mode=%d GSActors=%d GSVisible=%d NativeComponents=%d NativeHidden=%d ProxyKept=%d TransformChanges=%d Valid=%d TransientOnly=1 NoLevelSave=1"),Mode,GS,GSVisible,Native,Hidden,ProxyKept,TransformChanges,Valid?1:0);
    return Valid;
}

// Opt-in test validation: the possessed XR pawn must own the tracked view.
static bool ValidateVRPlayer(UWorld* World, bool bLog)
{
    auto* PC=World ? World->GetFirstPlayerController() : nullptr;
    APawn* Pawn=PC ? PC->GetPawn() : nullptr;
    TArray<UCameraComponent*> Cameras;
    if(Pawn) Pawn->GetComponents<UCameraComponent>(Cameras);
    UCameraComponent* Camera=nullptr; int32 Active=0;
    for(auto* C:Cameras) if(C->IsActive()){ Camera=C; ++Active; }
    const bool Tracked=(GEngine && GEngine->XRSystem.IsValid() && GEngine->XRSystem->HasValidTrackingPosition());
    const bool HMD=(GEngine && GEngine->XRSystem.IsValid() && GEngine->XRSystem->IsHeadTrackingAllowedForWorld(*World));
    const bool Valid=Pawn && Pawn->GetClass()->GetPathName()==TEXT("/Game/XRFramework/Blueprints/BP_XRPawn.BP_XRPawn_C") &&
        Pawn->GetController()==PC && PC->GetViewTarget()==Pawn && Active==1 && Camera->bLockToHmd && Tracked && HMD;
    if(bLog) UE_LOG(LogTemp,Display,TEXT("[GSVREffects][PlayerView] Valid=%d Pawn=%s ViewTarget=%s Camera=%s ActiveCameras=%d LockToHMD=%d HMD=%d Tracked=%d PawnLocation=%s CameraLocation=%s CameraRotation=%s"),
        Valid?1:0,*GetPathNameSafe(Pawn),PC?*GetPathNameSafe(PC->GetViewTarget()):TEXT("NONE"),*GetNameSafe(Camera),Active,
        Camera&&Camera->bLockToHmd?1:0,HMD?1:0,Tracked?1:0,Pawn?*Pawn->GetActorLocation().ToString():TEXT("NONE"),
        Camera?*Camera->GetComponentLocation().ToString():TEXT("NONE"),Camera?*Camera->GetComponentRotation().ToString():TEXT("NONE"));
    return Valid;
}

// The command line is constant for this process; parse the optional test start
// once. Keep OriginalSunRotation intact so EndPlay restores the authored light.
static TOptional<float> GetVREffectsSunStartPitch()
{
    static const TOptional<float> StartPitch=[]()
    {
        float Value=0;
        return FParse::Value(FCommandLine::Get(),TEXT("GSVRSunStartPitch="),Value)
            ? TOptional<float>(FMath::Clamp(Value,-179.0f,179.0f)) : TOptional<float>();
    }();
    return StartPitch;
}

// Diagnostic locomotion: translate the possessed XR pawn; never replace its
// tracked camera or overwrite the user's head rotation. No collision sweep lets
// the operator leave enclosing test meshes without changing those meshes.
static void TickVRKeyboardMove(UWorld* World,float DeltaSeconds)
{
    static const bool Enabled=FParse::Param(FCommandLine::Get(),TEXT("GSVRKeyboardMove"));
    if(!World || (!Enabled && !(World->WorldType==EWorldType::PIE && CVarEditorVRPreview.GetValueOnGameThread()!=0)))return;
    auto* PC=World->GetFirstPlayerController();APawn* Pawn=PC?PC->GetPawn():nullptr;
    if(!Pawn)return;
    const float Forward=float(PC->IsInputKeyDown(EKeys::Up))-float(PC->IsInputKeyDown(EKeys::Down));
    const float Right=float(PC->IsInputKeyDown(EKeys::Right))-float(PC->IsInputKeyDown(EKeys::Left));
    const float Up=float(PC->IsInputKeyDown(EKeys::PageUp))-float(PC->IsInputKeyDown(EKeys::PageDown));
    if(Forward==0 && Right==0 && Up==0)return;
    TArray<UCameraComponent*> Cameras;Pawn->GetComponents<UCameraComponent>(Cameras);
    const float Yaw=Cameras.Num()==1?Cameras[0]->GetComponentRotation().Yaw:Pawn->GetActorRotation().Yaw;
    const FRotationMatrix Basis(FRotator(0,Yaw,0));
    const FVector Direction=(Basis.GetUnitAxis(EAxis::X)*Forward+Basis.GetUnitAxis(EAxis::Y)*Right+FVector::UpVector*Up).GetSafeNormal();
    const float Speed=(PC->IsInputKeyDown(EKeys::LeftShift)||PC->IsInputKeyDown(EKeys::RightShift))?450.f:150.f;
    Pawn->AddActorWorldOffset(Direction*Speed*FMath::Clamp(DeltaSeconds,0.f,.1f),false,nullptr,ETeleportType::TeleportPhysics);
    static double LastLog=0;const double Now=FPlatformTime::Seconds();
    if(Now-LastLog>=.5){LastLog=Now;UE_LOG(LogTemp,Display,TEXT("[GSVRKeyboard][Move] Forward=%.0f Right=%.0f SpeedCmPerSec=%.0f PawnLocation=%s HeadYaw=%.2f TrackingPreserved=1"),Forward,Right,Speed,*Pawn->GetActorLocation().ToString(),Yaw);}
}

// A standalone collider reference may be editor-hidden while the game actor
// itself is not. Keep only the exact GS shadow-collider asset out of the main
// view; collision, transforms and GS shadow input data stay untouched.

// Diagnostic only: never reposition geometry to compensate for a camera issue.
static void LogPreviewPlacement(UWorld* World)
{
#if WITH_EDITOR
    static TMap<TWeakObjectPtr<UWorld>,int32> Snapshots;
    int32& Count=Snapshots.FindOrAdd(TWeakObjectPtr<UWorld>(World));if(Count>=3)return;++Count;
    UWorld* EditorWorld=nullptr;
    for(const FWorldContext& Context:GEngine->GetWorldContexts())
        if(Context.WorldType==EWorldType::Editor) EditorWorld=Context.World();
    TMap<FString,TArray<AStaticMeshActor*>> Originals;
    if(EditorWorld) for(TActorIterator<AStaticMeshActor> It(EditorWorld);It;++It)
    {
        auto* M=It->GetStaticMeshComponent();if(!M || !M->GetStaticMesh())continue;
        Originals.FindOrAdd(It->GetActorLabel()+TEXT("|")+M->GetStaticMesh()->GetPathName()).Add(*It);
    }
    int32 Compared=0,Changed=0,Unmatched=0;
    for(TActorIterator<AActor> It(World);It;++It)
    {
        if(It->GetClass()->GetName().Contains(TEXT("LevelInstance")))
            UE_LOG(LogTemp,Display,TEXT("[GSPreview][LevelPlacement] Actor=%s Class=%s Transform=%s"),*It->GetName(),*It->GetClass()->GetName(),*It->GetActorTransform().ToString());
        auto* A=Cast<AStaticMeshActor>(*It);if(!A)continue;
        auto* M=A->GetStaticMeshComponent();if(!M || !M->GetStaticMesh())continue;
        const auto* Match=Originals.Find(A->GetActorLabel()+TEXT("|")+M->GetStaticMesh()->GetPathName());
        if(!Match || Match->Num()!=1){++Unmatched;continue;}
        auto* E=(*Match)[0];const bool Same=A->GetActorTransform().Equals(E->GetActorTransform(),0.01);
        ++Compared;if(!Same)++Changed;
        if(!Same)UE_LOG(LogTemp,Warning,TEXT("[GSPreview][PlacementMismatch] Label=%s Mesh=%s Editor=%s PIE=%s EditorLevel=%s PIELevel=%s"),*A->GetActorLabel(),*M->GetStaticMesh()->GetPathName(),*E->GetActorTransform().ToString(),*A->GetActorTransform().ToString(),*GetPathNameSafe(E->GetLevel()),*GetPathNameSafe(A->GetLevel()));
    }
    UE_LOG(LogTemp,Display,TEXT("[GSPreview][PlacementSummary] Compared=%d Changed=%d UnmatchedOrAmbiguous=%d EditorWorld=%s NoGeometryModified=1"),Compared,Changed,Unmatched,*GetNameSafe(EditorWorld));
#endif
}

static void PreserveShadowProxyPresentation(UWorld* World)
{
    TSet<UStaticMesh*> ProxyMeshes;
    for(TActorIterator<AGaussianSplatActor> It(World);It;++It)if(auto* C=It->GaussianSplatComponent.Get())if(C->GetProxyShadowMesh())ProxyMeshes.Add(C->GetProxyShadowMesh());
    for(TActorIterator<AStaticMeshActor> It(World);It;++It){auto* M=It->GetStaticMeshComponent();if(!M || !ProxyMeshes.Contains(M->GetStaticMesh()) || !GetPathNameSafe(M->GetStaticMesh()).Contains(TEXT("/collider/")))continue;
        const FTransform Before=It->GetActorTransform();M->SetVisibility(false,false);M->SetHiddenInGame(true,false);
        UE_LOG(LogTemp,Display,TEXT("[GSPreview][StandaloneCollider] Actor=%s Mesh=%s Transform=%s TransformUnchanged=%d MainVisualHidden=1 CollisionUnchanged=1"),*It->GetName(),*GetPathNameSafe(M->GetStaticMesh()),*Before.ToHumanReadableString(),Before.Equals(It->GetActorTransform())?1:0);
    }
    for(TActorIterator<AGaussianSplatActor> It(World);It;++It)if(auto* R=It->ProxyShadowReceiverComponent.Get())UE_LOG(LogTemp,Display,TEXT("[GSPreview][ProxyReceiver] Actor=%s Transform=%s Visible=%d CaptureOnly=%d"),*It->GetName(),*R->GetComponentTransform().ToHumanReadableString(),R->IsVisible()?1:0,R->bVisibleInSceneCaptureOnly?1:0);
}

AGSDesktopPerfProbe::AGSDesktopPerfProbe()
{
    PrimaryActorTick.bCanEverTick = true;
    GetCameraComponent()->SetFieldOfView(90.0f);
}

void AGSDesktopPerfProbe::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if(bManualVRPreview){
        if (!GSAutomaticWorldScope::HasGS(GetWorld())) { Destroy(); return; }
        TickVRKeyboardMove(GetWorld(),DeltaSeconds);
        if(auto* PC=GetWorld()->GetFirstPlayerController())if(APawn* Pawn=PC->GetPawn()){
            if(PC->WasInputKeyJustPressed(EKeys::Home))for(TActorIterator<APlayerStart> It(GetWorld());It;++It){Pawn->SetActorTransform(It->GetActorTransform(),false,nullptr,ETeleportType::TeleportPhysics);break;}
        }
        const double Now=FPlatformTime::Seconds();if(Now-LastVRLogSeconds>=2){LastVRLogSeconds=Now;ValidateVRPlayer(GetWorld(),true);LogPreviewPlacement(GetWorld());}
        return;
    }
    if (bFinished) return;
    if(bVREffectsTest)TickVRKeyboardMove(GetWorld(),DeltaSeconds);
    const double Now = FPlatformTime::Seconds();
    if(bVREffectsTest && GetGSSceneCompareMode()!=0)
    {
        static double LastCompare=0;
        if(Now-LastCompare>=1.0){LastCompare=Now;if(!ApplyGSSceneCompare(GetWorld(),true)){UE_LOG(LogTemp,Error,TEXT("[GSSceneCompare][Invalid] MeasurementInvalid=1"));bFinished=true;if(!GIsEditor)FPlatformMisc::RequestExit(false);return;}}
    }
    if(bVREffectsTest && GetGSSceneCompareMode()!=2 && FParse::Param(FCommandLine::Get(),TEXT("GSPrepareStableBinding")))
    {
        bool Preparing=false;
        for(TActorIterator<AGaussianSplatActor> It(GetWorld());It;++It)
            if(auto* C=It->GaussianSplatComponent.Get())
                if(C->StableBindingState==EStableProxyBindingState::Building) Preparing=true;
        if(Preparing)
        {
            if(Now-LastVRLogSeconds>=1.0)
            {
                UE_LOG(LogTemp,Display,TEXT("[GSVREffects][Preparing] StableBindingBuilding=1 MeasurementStarted=0 CameraPreserved=1"));
                LastVRLogSeconds=Now;
            }
            StartWallSeconds=0; PreviousWallSeconds=0;
            return;
        }
        if(StartWallSeconds==0) LastVRLogSeconds=0;
    }
    if(bVREffectsTest)
    {
        int32 GSActors=0;bool VisibilityValid=true;
        for(TActorIterator<AGaussianSplatActor> It(GetWorld());It;++It)
        {
            auto* C=It->GaussianSplatComponent.Get();if(!C || !C->GetSplatAsset())continue;
            ++GSActors;VisibilityValid &= GetGSSceneCompareMode()==2 ? (It->IsHidden() && !C->IsVisible() && C->bHiddenInGame) : (!It->IsHidden() && C->IsVisible() && !C->bHiddenInGame && C->IsRegistered());
        }
        const bool NativeSceneTest=FParse::Param(FCommandLine::Get(),TEXT("GSVRNativeSceneTest"));
        if((GSActors==0 && !NativeSceneTest) || (NativeSceneTest && GSActors!=0) || !VisibilityValid)
        {
            UE_LOG(LogTemp,Error,TEXT("[GSVREffects][InvalidGSVisibility] Actors=%d Valid=%d MeasurementInvalid=1"),GSActors,VisibilityValid?1:0);
            bFinished=true;if(!GIsEditor)FPlatformMisc::RequestExit(false);return;
        }
    }
    if(bVREffectsTest && !ValidateVRPlayer(GetWorld(),false))
    {
        ValidateVRPlayer(GetWorld(),true);
        UE_LOG(LogTemp,Error,TEXT("[GSVREffects][InvalidPlayerView] MeasurementInvalid=1; actual possessed XR pawn / HMD view is required."));
        bFinished=true;
        if(!GIsEditor) FPlatformMisc::RequestExit(false);
        return;
    }
    if (StartWallSeconds == 0) StartWallSeconds = Now;
    const double Elapsed = Now - StartWallSeconds;
    if(bVREffectsTest)RecordGSVRHitchTelemetry(Elapsed,Now);
    if(bVREffectsTest && FParse::Param(FCommandLine::Get(),TEXT("GSVRAblationSuite"))){
        static const int32 Modes[]={0,1,0,5,0,2,0,3,0,4,0};
        const int32 Phase=FMath::Clamp(int32(Elapsed/15.0),0,10),Mode=Modes[Phase];
        static int32 LastPhase=-1;
        if(Phase!=LastPhase){
            if(auto* V=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.Ablation")))V->Set(Mode,ECVF_SetByConsole);
            UE_LOG(LogTemp,Display,TEXT("[GSAblation][Phase] Elapsed=%.4f Phase=%d Mode=%d PlannedStart=%d PlannedEnd=%d DiagnosticOnly=1 ProductionFPS=0 ResolutionUnchanged=1"),Elapsed,Phase,Mode,Phase*15,(Phase+1)*15);
            LastPhase=Phase;
        }
    }
    // Do not capture during initial binding construction. The opt-in request
    // reaches the renderer only after warmup in a real, user-authorised VR run.
    static bool bDrawCaptureRequested=false;
    if(bVREffectsTest && !bDrawCaptureRequested && Elapsed>=12.0 &&
        FParse::Param(FCommandLine::Get(),TEXT("GSVRDrawCapture")))
    {
        bDrawCaptureRequested=true;
        if(auto* CaptureMask=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.DrawCaptureMask")))
        {
            CaptureMask->Set(3,ECVF_SetByConsole);
            UE_LOG(LogTemp,Display,TEXT("[GSDrawCapture][Requested] Elapsed=%.2f Eyes=3 ProductionFPS=0"),Elapsed);
        }
    }
    // Wall-clock timing keeps the camera moving even when engine delta is fixed.
    const double T = bStationary ? 0.0 : FMath::Max(0.0, Elapsed - 5.0);
    const double Phase = T / 40.0;
    const float Yaw = static_cast<float>(Phase * 360.0);
    const float Pitch = static_cast<float>(25.0 * FMath::Sin(T * 0.38));
    const FVector Position(60.0 * FMath::Sin(T * 0.21), 60.0 * FMath::Sin(T * 0.31),
                           170.0 + 20.0 * FMath::Sin(T * 0.27));
    if (!bVREffectsTest) SetActorLocationAndRotation(Position, FRotator(Pitch, Yaw, 0));
    if (bVREffectsTest && VRTestSun.IsValid())
    {
        // Rotate Unreal Y / Pitch: change solar elevation; retain authored yaw and roll.
        FRotator Rotation=OriginalSunRotation;
        const TOptional<float> StartPitch=GetVREffectsSunStartPitch();
        if(StartPitch.IsSet()) Rotation.Pitch=StartPitch.GetValue();
        const float Travel=float(FMath::Max(0.0,Elapsed-5.0))*SunPitchDegreesPerSecond;
        // Bounded Y/Pitch sweep; the requested range may pass through the zenith.
        const float SweepPhase=SunPitchSweepDegrees>0 ? FMath::Fmod(Travel,2.0f*SunPitchSweepDegrees) : Travel;
        Rotation.Pitch+=SunPitchSweepDegrees>0 ? FMath::Min(SweepPhase,2.0f*SunPitchSweepDegrees-SweepPhase) : Travel;
        VRTestSun->SetWorldRotation(Rotation);
    }
    if(bAnimateTestLight)
    {
        if(!TestLight.IsValid())
        {
            TestLight=GetWorld()->SpawnActor<ADirectionalLight>();
            if(TestLight.IsValid()) TestLight->GetLightComponent()->SetMobility(EComponentMobility::Movable);
        }
        if(TestLight.IsValid())
        {
            const double LightTime=FParse::Param(FCommandLine::Get(),TEXT("GSPCPerfLightShotHold")) && Elapsed>=3.0 && Elapsed<=5.0 ? 3.0 : Elapsed;
            TestLight->SetActorRotation(FRotator(-45.0f,float(LightTime*25.0),0));
            TestLight->GetLightComponent()->SetIntensity(float(5.0+3.0*FMath::Sin(LightTime)));
        }
    }
    if(FParse::Param(FCommandLine::Get(),TEXT("GSPCPerfQuadABA")))
    {
        const int32 PhaseIndex=FMath::Clamp(int32(FMath::Max(0.0,Elapsed-5.0)/10.0),0,3);
        const int32 TrimMode=PhaseIndex%2;
        if(auto* Variable=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PC.SourceQuadTrim")))
            if(Variable->GetInt()!=TrimMode)
            {
                Variable->Set(TrimMode,ECVF_SetByConsole);
                UE_LOG(LogTemp,Display,TEXT("[GSPCPerf][QuadABA] Elapsed=%.4f SourceQuadTrim=%d Phase=%d"),Elapsed,TrimMode,PhaseIndex);
            }
    }
    // Opt-in delayed capture: allow GPU asset upload and camera settling first.
    if(Elapsed>=4.0 && !Tags.Contains(TEXT("GSPCPerfScreenshotRequested")))
    {
        FString ScreenshotPath;
        if(FParse::Value(FCommandLine::Get(),TEXT("GSPCPerfScreenshot="),ScreenshotPath))
        {
            FScreenshotRequest::RequestScreenshot(ScreenshotPath,false,false);
            UE_LOG(LogTemp,Display,TEXT("[GSPCPerf][Screenshot] %s"),*ScreenshotPath);
        }
        Tags.Add(TEXT("GSPCPerfScreenshotRequested"));
    }
    if(PreviousWallSeconds>=StartWallSeconds+5.0 && Now>PreviousWallSeconds) FrameMs.Add((Now-PreviousWallSeconds)*1000.0);
    PreviousWallSeconds=Now;
    if (bVREffectsTest && Elapsed-LastVRLogSeconds>=1.0)
    {
        ValidateVRPlayer(GetWorld(),true);
        double Sum=0; for(double Ms:FrameMs) Sum+=Ms;
        const int32 Samples=FrameMs.Num()-LastVRLogSampleCount;
        const double SampleMs=Sum-LastVRLogSampleSeconds;
        const double Mean=Samples>0 ? SampleMs/Samples : 0;
        UE_LOG(LogTemp,Display,TEXT("[GSVREffects][Second] Elapsed=%.2f Samples=%d WallFrameMs=%.4f ActualFPS=%.2f Sun=%s Rotation=%s Warmup=%d CameraPreserved=1"),
            Elapsed,Samples,Mean,Mean>0 ? 1000.0/Mean:0,*GetNameSafe(VRTestSun.Get()),
            VRTestSun.IsValid()?*VRTestSun->GetComponentRotation().ToString():TEXT("NONE"),Elapsed<5.0?1:0);
        for(TActorIterator<AGaussianSplatActor> It(GetWorld());It;++It)
            if(auto* C=It->GaussianSplatComponent.Get())
            {
                UE_LOG(LogTemp,Display,TEXT("[GSVREffects][Readiness] Elapsed=%.2f Actor=%s BindingState=%d BindingRev=%u RayRev=%u Valid=%d Inputs=%d Proxy=%s Master=%d ReceiveLights=%d Weather=%d"),
                    Elapsed,*It->GetName(),int32(C->StableBindingState),C->GetStableBindingRevision(),C->GetStableRayProofRevision(),
                    C->StableBindingValidCount,C->StableBindingInputCount,*GetNameSafe(C->GetProxyShadowMesh()),C->bEnableCinematicEffects?1:0,C->bReceiveUnrealLights?1:0,int32(C->WeatherMode));
                UE_LOG(LogTemp,Display,TEXT("[GSVREffects][SolarState] Elapsed=%.2f Automatic=%d ElevationDeg=%.3f Phase=%s Time=%s"),
                    Elapsed,C->bEnableAutomaticDayNightCycle?1:0,C->RuntimeSolarElevationDegrees,*C->RuntimeTimeOfDayPhase,*C->RuntimeTimeOfDayText);
                const auto& Proofs=C->GetStableProxyRayProofs();
                int32 Ready=0,Shadowed=0;double ShadowSum=0;float MinV=1,MaxV=0;
                for(const FVector4f& P:Proofs) if(P.Z>0 && FMath::IsFinite(P.X))
                {
                    ++Ready;const float V=FMath::Clamp(P.X,0.0f,1.0f);
                    Shadowed+=V<0.99f?1:0;ShadowSum+=V;MinV=FMath::Min(MinV,V);MaxV=FMath::Max(MaxV,V);
                }
                UE_LOG(LogTemp,Display,TEXT("[GSVREffects][ShadowProof] Elapsed=%.2f Actor=%s Strength=%.4f Gamma=%.4f Proofs=%d Ready=%d Shadowed=%d MinVisibility=%.4f MaxVisibility=%.4f MeanVisibility=%.4f ComputeFinalColor=%d"),
                    Elapsed,*It->GetName(),C->CapturedShadowStrength,C->CapturedShadowGamma,Proofs.Num(),Ready,Shadowed,MinV,MaxV,Ready?ShadowSum/Ready:1.0,
                    IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.ComputeFinalColor"))->GetInt());
            }
        if(GEngine) GEngine->AddOnScreenDebugMessage(uint64(GetUniqueID()),1.5f,FColor::Green,
            FString::Printf(TEXT("VR EFFECTS TEST  %.0f/%.0fs | %.1f fps | %.2f ms | sun %.1f deg/s"),Elapsed,DurationSeconds,Mean>0?1000.0/Mean:0,Mean,SunPitchDegreesPerSecond));
        LastVRLogSeconds=Elapsed; LastVRLogSampleCount=FrameMs.Num(); LastVRLogSampleSeconds=Sum;
    }
    if (Elapsed >= DurationSeconds)
    {
        if(bVREffectsTest && FParse::Param(FCommandLine::Get(),TEXT("GSVRAblationSuite")))
            if(auto* V=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.Ablation")))V->Set(0,ECVF_SetByConsole);
        bFinished = true;
        FrameMs.Sort();
        double Sum = 0;
        for (double Ms : FrameMs) Sum += Ms;
        const int32 Count = FrameMs.Num();
        const double Mean = Count ? Sum / Count : 0;
        const double P95 = Count ? FrameMs[FMath::Min(Count-1, int32(Count*0.95))] : 0;
        UE_LOG(LogTemp, Display, TEXT("[GSPCPerf][Complete] Samples=%d WallFrameMeanMs=%.4f ActualFPS=%.2f WallP95Ms=%.4f Warmup=5 WallSeconds=%.2f MovingCamera=%d SampleWallSeconds=%.3f"),
            Count, Mean, Mean > 0 ? 1000.0/Mean : 0, P95, Elapsed,(bStationary||bVREffectsTest)?0:1,Sum/1000.0);
        if(bVREffectsTest && GetGSSceneCompareMode()!=0)UE_LOG(LogTemp,Display,TEXT("[GSSceneCompare][Complete] Mode=%d Samples=%d WallFrameMeanMs=%.4f ActualFPS=%.2f WallP95Ms=%.4f Duration=%.2f"),GetGSSceneCompareMode(),Count,Mean,Mean>0?1000.0/Mean:0,P95,Elapsed);
        if(bVREffectsTest)
            UE_LOG(LogTemp,Display,TEXT("[GSVREffects][Complete] Samples=%d WallFrameMeanMs=%.4f ActualFPS=%.2f WallP95Ms=%.4f Duration=%.2f CameraPreserved=1"),Count,Mean,Mean>0?1000.0/Mean:0,P95,Elapsed);
        if (((!bVREffectsTest && FParse::Param(FCommandLine::Get(), TEXT("GSPCPerfAutoExit"))) || (bVREffectsTest && FParse::Param(FCommandLine::Get(), TEXT("GSVREffectsAutoExit")))) && !GIsEditor)
            FPlatformMisc::RequestExit(false);
    }
}

static FAutoConsoleCommandWithWorldAndArgs GSPCPerfTest(
    TEXT("gs.PCPerfTest"),
    TEXT("Desktop-only transient moving camera capture. Usage: gs.PCPerfTest 45. No assets are modified."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
    {
        if (!World || !World->IsGameWorld()) return;
        APlayerController* PC = World->GetFirstPlayerController();
        if (!PC) return;
        AGSDesktopPerfProbe* Probe = World->SpawnActor<AGSDesktopPerfProbe>();
        if (!Probe) return;
        Probe->DurationSeconds = Args.Num() ? FMath::Clamp(FCString::Atof(*Args[0]), 10.f, 120.f) : 45.f;
        Probe->bStationary=FParse::Param(FCommandLine::Get(),TEXT("GSPCPerfStationary"));
        Probe->bAnimateTestLight=FParse::Param(FCommandLine::Get(),TEXT("GSPCPerfAnimateLight"));
        PC->SetViewTarget(Probe);
        int32 W=0,H=0;
        PC->GetViewportSize(W,H);
        UE_LOG(LogTemp, Display, TEXT("[GSPCPerf][Start] Viewport=%dx%d Duration=%.1f NoHMD=%d CameraFOV=90"),
            W,H,Probe->DurationSeconds,FParse::Param(FCommandLine::Get(), TEXT("NoHMD")) ? 1 : 0);
        for (const TCHAR* Name : {TEXT("r.ForwardShading"),TEXT("r.AntiAliasingMethod"),TEXT("r.MSAACount"),
             TEXT("r.ScreenPercentage"),TEXT("r.DynamicRes.OperationMode"),TEXT("r.VSync"),TEXT("t.MaxFPS"),
             TEXT("r.RayTracing"),TEXT("r.DynamicGlobalIlluminationMethod"),TEXT("r.ReflectionMethod"),TEXT("gs.VRDiagMode"),TEXT("gs.PC.CacheFix"),TEXT("gs.PCVR.DesktopTest"),TEXT("r.VolumetricCloud")})
        {
            if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
                UE_LOG(LogTemp, Display, TEXT("[GSPCPerf][Setting] %s=%s"),Name,*Var->GetString());
        }
    }));


// Opt-in PIE/game harness: never changes the editor world or steals the VR pawn's view.
void AGSDesktopPerfProbe::TryStartVREffectsTest(UWorld* World)
{
    // GS diagnostics must never take ownership of a mesh-only scene.
    if (!GSAutomaticWorldScope::HasGS(World)) return;
    if(GetGSSceneCompareMode()!=0 && FParse::Param(FCommandLine::Get(),TEXT("GSSceneCompareValidate")) && World && World->IsGameWorld() && World->HasBegunPlay() && !GIsEditor)
    {
        static double Started=FPlatformTime::Seconds();
        if(GetGSSceneCompareMode()!=2)for(TActorIterator<AGaussianSplatActor> It(World);It;++It)if(auto* C=It->GaussianSplatComponent.Get()){It->SetActorHiddenInGame(false);C->SetHiddenInGame(false,false);C->SetVisibility(true,false);}
        const bool Valid=ApplyGSSceneCompare(World,false);
        if(FPlatformTime::Seconds()-Started>=4.0){ApplyGSSceneCompare(World,true);UE_LOG(LogTemp,Display,TEXT("[GSSceneCompare][ValidationComplete] Mode=%d Valid=%d PerformanceTest=0"),GetGSSceneCompareMode(),Valid?1:0);FPlatformMisc::RequestExit(false);}
        return;
    }

    // Editor VR Preview uses its own manual path. It never enters the timed
    // benchmark, swaps the GS asset, rotates the sun or exits the editor.
    if(!FParse::Param(FCommandLine::Get(),TEXT("GSVREffectsTest")) && CVarEditorVRPreview.GetValueOnGameThread()!=0 && World && World->WorldType==EWorldType::PIE && GSAutomaticWorldScope::HasGS(World) && World->HasBegunPlay() && GEngine && GEngine->IsStereoscopic3D() && GEngine->XRSystem.IsValid() && GEngine->XRSystem->HasValidTrackingPosition()){
        auto* PC=World->GetFirstPlayerController();APawn* Pawn=PC?PC->GetPawn():nullptr;
        if(!Pawn)return;
        for(TActorIterator<AGSDesktopPerfProbe> It(World);It;++It)if(It->bManualVRPreview)return;
        if(Pawn->GetClass()->GetPathName()!=TEXT("/Game/XRFramework/Blueprints/BP_XRPawn.BP_XRPawn_C"))return;
        TArray<UCameraComponent*> Cameras;Pawn->GetComponents<UCameraComponent>(Cameras);
        if(Cameras.Num()!=1)return;
        Cameras[0]->Activate();Cameras[0]->bLockToHmd=true;PC->SetViewTarget(Pawn);
        for(TActorIterator<APlayerStart> It(World);It;++It){Pawn->SetActorTransform(It->GetActorTransform(),false,nullptr,ETeleportType::TeleportPhysics);UE_LOG(LogTemp,Display,TEXT("[GSPreview][Start] PlayerStart=%s Pawn=%s TrackingPreserved=1"),*It->GetActorTransform().ToHumanReadableString(),*Pawn->GetActorLocation().ToString());break;}
        int32 Shown=0;
        for(TActorIterator<AGaussianSplatActor> It(World);It;++It){auto* C=It->GaussianSplatComponent.Get();if(!C)continue;
            const FTransform Before=It->GetActorTransform();
            It->SetActorHiddenInGame(false);C->SetHiddenInGame(false,false);C->SetVisibility(true,false);
            C->bEnableCinematicEffects=true;C->bReceiveUnrealLights=true;C->LightingPreviewMode=ELCCCinematicGSLightingPreviewMode::Final;It->ShadowDebugMode=EStableProxyShadowDebugMode::Final;C->EnableCinematicEffects();It->SyncProxyShadowReceiver();
            UE_LOG(LogTemp,Display,TEXT("[GSPreview][GS] Actor=%s Asset=%s TransformUnchanged=%d ChildVisibilityPropagated=0 ProxyCaptureOnlyRestored=1 Weather=%d"),*It->GetName(),*GetPathNameSafe(C->GetSplatAsset()),Before.Equals(It->GetActorTransform())?1:0,int32(C->WeatherMode));++Shown;
        }
        PreserveShadowProxyPresentation(World);
        LogPreviewPlacement(World);
        auto* Helper=World->SpawnActor<AGSDesktopPerfProbe>();if(!Helper)return;
        Helper->bManualVRPreview=true;
        for (const auto& Setting : TArray<TPair<const TCHAR*, const TCHAR*>>{
            {TEXT("xr.SecondaryScreenPercentage.HMDRenderTarget"),TEXT("112.765426")},
            {TEXT("r.VSync"),TEXT("0")}, {TEXT("t.MaxFPS"),TEXT("0")},
            {TEXT("r.ScreenPercentage"),TEXT("100")}, {TEXT("r.DynamicRes.OperationMode"),TEXT("0")}})
        {
            if (auto* V=IConsoleManager::Get().FindConsoleVariable(Setting.Key))
            {
                const uint32 Priority=V->GetFlags() & ECVF_SetByMask;
                // Respect explicit operator/command-line overrides.
                if (Priority >= ECVF_SetByCommandline) continue;
                const FString Before=V->GetString();
                V->SetWithCurrentPriority(Setting.Value);
                Helper->PreviewSettings.Add({Setting.Key,Before,V->GetString(),Priority});
            }
        }
        Helper->GetCameraComponent()->Deactivate();PC->SetViewTarget(Pawn);PC->SetInputMode(FInputModeGameOnly());
        UE_LOG(LogTemp,Display,TEXT("[GSPreview][Enabled] TimedTest=0 AutoExit=0 AssetSwap=0 SunOverride=0 GSActors=%d Keyboard=Arrows_Shift_PageUp_PageDown_Home"),Shown);
        GEngine->AddOnScreenDebugMessage(0x4753565250524556ull,12,FColor::Cyan,TEXT("VR Preview: Arrows move | Shift fast | PgUp/PgDn height | Home PlayerStart"));
        return;
    }

    if(!FParse::Param(FCommandLine::Get(),TEXT("GSVREffectsTest")) || !World || !World->IsGameWorld() ||
        !World->HasBegunPlay() || !World->GetFirstPlayerController()) return;
    for(TActorIterator<AGSDesktopPerfProbe> It(World);It;++It) if(It->bVREffectsTest) return;
    auto* PC=World->GetFirstPlayerController();
    APawn* Pawn=PC->GetPawn();
    // Wait for normal GameMode possession and XR startup. Never benchmark a fallback camera.
    if(!Pawn || !(GEngine && GEngine->XRSystem.IsValid() && GEngine->XRSystem->HasValidTrackingPosition())) return;
    float StartX=0,StartY=0,StartZ=0,StartYaw=0;
    const bool bCopyJ1Start=FParse::Value(FCommandLine::Get(),TEXT("GSVRStartX="),StartX) && FParse::Value(FCommandLine::Get(),TEXT("GSVRStartY="),StartY) && FParse::Value(FCommandLine::Get(),TEXT("GSVRStartZ="),StartZ) && FParse::Value(FCommandLine::Get(),TEXT("GSVRStartYaw="),StartYaw);
    if(bCopyJ1Start){Pawn->SetActorLocationAndRotation(FVector(StartX,StartY,StartZ),FRotator(0,StartYaw,0),false,nullptr,ETeleportType::TeleportPhysics);PC->SetControlRotation(FRotator(0,StartYaw,0));UE_LOG(LogTemp,Display,TEXT("[GSVREffects][SpawnOverride] Source=J1PlayerStart Location=%s Yaw=%.3f TrackingPreserved=1 NoLevelSave=1"),*Pawn->GetActorLocation().ToString(),StartYaw);}
    TArray<UCameraComponent*> Cameras; Pawn->GetComponents<UCameraComponent>(Cameras);
    if(Cameras.Num()==1) { Cameras[0]->Activate(); Cameras[0]->bLockToHmd=true; }
    PC->SetViewTarget(Pawn);
    if(!ValidateVRPlayer(World,true))
    {
        UE_LOG(LogTemp,Error,TEXT("[GSVREffects][InvalidPlayerView] Startup validation failed. MeasurementInvalid=1"));
        if(!GIsEditor) FPlatformMisc::RequestExit(false);
        return;
    }
    int32 Updated=0;
    for(TActorIterator<AGaussianSplatActor> It(World);It;++It)
    {
        auto* Component=It->GaussianSplatComponent.Get();
        if(!Component) continue;
        if(GetGSSceneCompareMode()==2){It->SetActorHiddenInGame(true);Component->SetHiddenInGame(true,false);Component->SetVisibility(false,false);++Updated;continue;}
        // Full-scene measurement must not depend on the editing hide flag.
        // Affect only the transient game actor and GS body, never child proxies.
        const bool WasHidden=It->IsHidden();
        It->SetActorHiddenInGame(false);
        Component->SetHiddenInGame(false,false);
        Component->SetVisibility(true,false);
        UE_LOG(LogTemp,Display,TEXT("[GSVREffects][FullGSVisibility] Actor=%s Asset=%s SavedActorHidden=%d ActorHidden=%d ComponentVisible=%d HiddenInGame=%d ChildPropagation=0 NoLevelSave=1"),
            *It->GetName(),*GetPathNameSafe(Component->GetSplatAsset()),WasHidden?1:0,It->IsHidden()?1:0,Component->IsVisible()?1:0,Component->bHiddenInGame?1:0);
        FString PreparedPath,PreparedSource;
        FParse::Value(FCommandLine::Get(),TEXT("GSPCVRPreparedSource="),PreparedSource);
        if(FParse::Value(FCommandLine::Get(),TEXT("GSPCVRPreparedAsset="),PreparedPath) && Component->GetSplatAsset() && Component->GetSplatAsset()->GetPathName()==PreparedSource){
            auto* Prepared=LoadObject<UGaussianSplatAsset>(nullptr,*PreparedPath);
            if(!Prepared || !Prepared->HasClusterHierarchy() || !Prepared->IsNaniteEnabled()){UE_LOG(LogTemp,Error,TEXT("[GSPCVRLOD][InvalidAsset] %s"),*PreparedPath);continue;}
            Component->SetSplatAsset(Prepared);
            // Full-scene test explicitly requires visible GS. Override only the
            // transient game-world actor; retain the user's saved editor flags.
            It->SetActorHiddenInGame(false);
            Component->SetHiddenInGame(false,false);
            Component->SetVisibility(true,false);
            UE_LOG(LogTemp,Display,TEXT("[GSPCVRLOD][Asset] Path=%s Original=%d Total=%d Clusters=%d Levels=%d FullScene=1 DynamicEffects=1"),*PreparedPath,Prepared->GetOriginalSplatCount(),Prepared->GetSplatCount(),Prepared->GetClusterCount(),Prepared->GetNumLODLevels());
            UE_LOG(LogTemp,Display,TEXT("[GSPCVRLOD][Visibility] ActorHidden=%d ComponentVisible=%d RequestedHiddenInGame=0 NoLevelSave=1"),It->IsHidden()?1:0,Component->IsVisible()?1:0);
        }
        Component->bEnableCinematicEffects=true;
        Component->bReceiveUnrealLights=true;
        // The low-sun test explicitly exercises the authored time-of-day system.
        // This is only a transient play-world change; never save the level.
        if(GetVREffectsSunStartPitch().IsSet()) Component->bEnableAutomaticDayNightCycle=true;
        Component->LightingPreviewMode=ELCCCinematicGSLightingPreviewMode::Final;
        It->ShadowDebugMode=EStableProxyShadowDebugMode::Final;
        if(Component->RenderProfile==ELCCCinematicGSRenderProfile::Original)
            Component->RenderProfile=ELCCCinematicGSRenderProfile::Realtime;
        Component->EnableCinematicEffects();
        It->SyncProxyShadowReceiver();
        UE_LOG(LogTemp,Display,TEXT("[GSVREffects][StableInputs] Actor=%s Source=%s Proxy=%s State=%d BindingRev=%u RayRev=%u Valid=%d Inputs=%d Cache=%s"),
            *It->GetName(),*GetPathNameSafe(Component->GetSplatAsset()),*GetPathNameSafe(Component->GetProxyShadowMesh()),
            int32(Component->StableBindingState),Component->GetStableBindingRevision(),Component->GetStableRayProofRevision(),
            Component->StableBindingValidCount,Component->StableBindingInputCount,*Component->StableBindingCacheStatus);
        if(FParse::Param(FCommandLine::Get(),TEXT("GSPrepareStableBinding")) && Component->StableBindingState!=EStableProxyBindingState::Ready)
        {
            if(Component->GetSplatAsset() && Component->GetProxyShadowMesh())
            {
                UE_LOG(LogTemp,Display,TEXT("[GSVREffects][StablePrepare] ExistingProxy=1 Action=RebuildBinding NoLevelSave=1"));
                Component->RebuildStableProxyBindings();
            }
            else UE_LOG(LogTemp,Warning,TEXT("[GSVREffects][StablePrepare] MissingSourceOrProxy=1 FinalEffectsReady=0 NoSubstituteGeometryCreated=1"));
        }
        UE_LOG(LogTemp,Display,TEXT("[GSVREffects][Actor] Actor=%s Effects=1 ReceiveUnrealLights=1 Weather=%d Profile=%d Final=1 WeatherUnchanged=1"),
            *It->GetName(),int32(Component->WeatherMode),int32(Component->RenderProfile));
        ++Updated;
    }
    if(FParse::Param(FCommandLine::Get(),TEXT("GSVRNativeSceneTest")))UE_LOG(LogTemp,Display,TEXT("[GSVRNativeScene][Start] World=%s GSActors=%d ExplicitNativeTest=1 NoAssetSwap=1 NoNativeGeometryHidden=1"),*World->GetName(),Updated);
    PreserveShadowProxyPresentation(World);
    ApplyGSSceneCompare(World,true);
    UDirectionalLightComponent* Sun=nullptr; float Best=-1;
    for(TObjectIterator<UDirectionalLightComponent> It;It;++It)
    {
        auto* Candidate=*It;
        // Match the plugin's authored sun selection, excluding generated moon lights.
        if(Candidate->GetWorld()!=World || !Candidate->IsRegistered() || !Candidate->IsVisible()) continue;
        if(auto* Owner=Cast<AGaussianSplatActor>(Candidate->GetOwner()))
            if(Owner->MoonDirectionalLightComponent==Candidate) continue;
        float Score=FMath::Max(0.0f,Candidate->Intensity)+
            (Candidate->bAtmosphereSunLight && Candidate->GetAtmosphereSunLightIndex()==0u?1000000.0f:0.0f);
        if(!Sun || Score>Best){Sun=Candidate;Best=Score;}
    }
    auto* Probe=World->SpawnActor<AGSDesktopPerfProbe>();
    if(!Probe) return;
    Probe->bVREffectsTest=true; Probe->DurationSeconds=180;
    if(FParse::Param(FCommandLine::Get(),TEXT("GSVRKeyboardMove"))){
        PC->SetInputMode(FInputModeGameOnly());
        UE_LOG(LogTemp,Display,TEXT("[GSVRKeyboard][Enabled] Arrows=HeadRelativeHorizontal Shift=Fast SpeedCmPerSec=150 ShiftSpeedCmPerSec=450 CollisionSweep=0 TrackingPreserved=1"));
    }
    Probe->GetCameraComponent()->Deactivate();
    PC->SetViewTarget(Pawn);
    float Duration=180;
    if(FParse::Value(FCommandLine::Get(),TEXT("GSVREffectsSeconds="),Duration)) Probe->DurationSeconds=FMath::Clamp(Duration,10.f,600.f);
    float SunSpeed=1.0f,SunSweep=0.0f;
    FParse::Value(FCommandLine::Get(),TEXT("GSVRSunSpeed="),SunSpeed);
    FParse::Value(FCommandLine::Get(),TEXT("GSVRSunSweep="),SunSweep);
    Probe->SunPitchDegreesPerSecond=FMath::Clamp(SunSpeed,0.0f,10.0f);
    Probe->SunPitchSweepDegrees=FMath::Clamp(SunSweep,0.0f,360.0f);
    if(Sun)
    {
        Probe->VRTestSun=Sun; Probe->OriginalSunRotation=Sun->GetComponentRotation();
        Probe->OriginalSunMobility=Sun->Mobility; Sun->SetMobility(EComponentMobility::Movable);
        const TOptional<float> StartPitch=GetVREffectsSunStartPitch();
        if(StartPitch.IsSet())
        {
            FRotator StartRotation=Probe->OriginalSunRotation;
            StartRotation.Pitch=StartPitch.GetValue();Sun->SetWorldRotation(StartRotation);
        }
    }
    auto Set=[](const TCHAR* Name,int32 Value){if(auto* Var=IConsoleManager::Get().FindConsoleVariable(Name)) Var->Set(Value,ECVF_SetByConsole);};
    Set(TEXT("gs.VRDiagMode"),0); Set(TEXT("gs.VRDiagLog"),1); Set(TEXT("gs.PCVR.Enable"),1);
    Set(TEXT("gs.PC.VertexLighting"),0); // Experimental vertex path has no established speedup.
    World->GetFirstPlayerController()->ConsoleCommand(TEXT("stat fps"),false);
    World->GetFirstPlayerController()->ConsoleCommand(TEXT("stat unit"),false);
    UE_LOG(LogTemp,Display,TEXT("[GSVREffects][Start] World=%s Actors=%d Sun=%s Duration=%.1f Mode=0 VertexLighting=0 Axis=Y_Pitch PitchDegreesPerSecond=%.2f SweepDegrees=%.2f StartPitch=%.2f Warmup=5 CameraPreserved=1 WeatherUnchanged=1"),
        *World->GetName(),Updated,*GetNameSafe(Sun),Probe->DurationSeconds,Probe->SunPitchDegreesPerSecond,Probe->SunPitchSweepDegrees,
        GetVREffectsSunStartPitch().Get(Probe->OriginalSunRotation.Pitch));
    const auto* BlueOverlay=IConsoleManager::Get().FindConsoleVariable(TEXT("gs.PCVR.ShadowBlueOverlay"));
    UE_LOG(LogTemp,Display,TEXT("[GSVREffects][ShadowOverlay] Blue=%d Payload=StableCastVisibility ClosureIncluded=0 StartPitch=%.2f EndPitch=%.2f"),
        BlueOverlay ? BlueOverlay->GetInt() : 0,
        GetVREffectsSunStartPitch().Get(Probe->OriginalSunRotation.Pitch),
        GetVREffectsSunStartPitch().Get(Probe->OriginalSunRotation.Pitch)+Probe->SunPitchSweepDegrees);
    if(!Sun) UE_LOG(LogTemp,Warning,TEXT("[GSVREffects] No authored directional sun found; effects/FPS recorded but no light can rotate."));
}

void AGSDesktopPerfProbe::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if(bVREffectsTest && VRTestSun.IsValid())
    {
        VRTestSun->SetWorldRotation(OriginalSunRotation);
        VRTestSun->SetMobility(OriginalSunMobility);
    }
    for (const auto& Setting : PreviewSettings)
        if (auto* V=IConsoleManager::Get().FindConsoleVariable(*Setting.Name))
            if (V->GetString()==Setting.Applied && (V->GetFlags() & ECVF_SetByMask)==Setting.Priority)
                V->SetWithCurrentPriority(*Setting.Before);
    PreviewSettings.Empty();
    Super::EndPlay(EndPlayReason);
}
