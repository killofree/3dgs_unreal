#include "VR360SkyboxActor.h"
#include "Components/StaticMeshComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MediaPlayer.h"
#include "MediaSource.h"
#include "MediaTexture.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#if WITH_EDITOR
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionCustom.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogVR360Skybox, Log, All);

AVR360SkyboxActor::AVR360SkyboxActor()
{
    PrimaryActorTick.bCanEverTick = true;
    SkySphere = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SkySphere"));
    SetRootComponent(SkySphere);
    SkySphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    SkySphere->SetCastShadow(false);
    SkySphere->SetGenerateOverlapEvents(false);
    SkySphere->bReceivesDecals = false;

    FloorPlane = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FloorPlane"));
    FloorPlane->SetupAttachment(SkySphere);
    // Prevent huge sky-cube scale from affecting the floor reference size.
    FloorPlane->SetAbsolute(false, true, true);
    FloorPlane->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    FloorPlane->SetCastShadow(false);
    FloorPlane->SetGenerateOverlapEvents(false);
    FloorPlane->SetVisibility(false);
    FloorPlane->SetHiddenInGame(true);

    static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereAsset(TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (SphereAsset.Succeeded())
    {
        SkySphere->SetStaticMesh(SphereAsset.Object);
    }
    static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneAsset(TEXT("/Engine/BasicShapes/Plane.Plane"));
    if (PlaneAsset.Succeeded())
    {
        FloorPlane->SetStaticMesh(PlaneAsset.Object);
    }


}

void AVR360SkyboxActor::BeginPlay()
{
    Super::BeginPlay();

    // /Engine/BasicShapes/Cube has a nominal 50uu half-extent.
    SkySphere->SetRelativeScale3D(FVector(FMath::Max(100.0f, SkyRadius) / 50.0f));
    SkySphere->SetRelativeRotation(FRotator::ZeroRotator);

    if (!SkyMaterial)
    {
        UE_LOG(LogVR360Skybox, Error, TEXT("SkyMaterial missing. Select the actor and click Create / Assign Cube Material in Details (Editor only)."));
    }
    else
    {
        DynamicMaterial = UMaterialInstanceDynamic::Create(SkyMaterial, this);
        SkySphere->SetMaterial(0, DynamicMaterial);
        DynamicMaterial->SetScalarParameterValue(TEXT("Brightness"), Brightness);
        DynamicMaterial->SetScalarParameterValue(TEXT("YawDegrees"), YawDegrees);
        DynamicMaterial->SetScalarParameterValue(TEXT("PanoramaPitchDegrees"), PanoramaPitchDegrees);
    }

    MediaPlayer = NewObject<UMediaPlayer>(this, TEXT("VR360MediaPlayer"));
    MediaTexture = NewObject<UMediaTexture>(this, TEXT("VR360MediaTexture"));
    MediaTexture->SetMediaPlayer(MediaPlayer);
    MediaTexture->UpdateResource();
    MediaPlayer->SetLooping(bLoop);
    MediaPlayer->OnMediaOpened.AddDynamic(this, &AVR360SkyboxActor::HandleMediaOpened);

    if (DynamicMaterial)
    {
        DynamicMaterial->SetTextureParameterValue(TEXT("VideoTexture"), MediaTexture);
    }

    UpdateFloorPlane();

    if (VideoSource)
    {
        // OpenSource is asynchronous; OnMediaOpened will start playback.
        if (!MediaPlayer->OpenSource(VideoSource))
        {
            UE_LOG(LogVR360Skybox, Error, TEXT("Could not open video MediaSource."));
        }
    }
    else
    {
        UE_LOG(LogVR360Skybox, Warning, TEXT("No VideoSource assigned to VR360SkyboxActor."));
    }
}

void AVR360SkyboxActor::HandleMediaOpened(FString OpenedUrl)
{
    if (MediaPlayer && bAutoPlay)
    {
        MediaPlayer->Play();
    }
}

void AVR360SkyboxActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    if (bFollowHMDPosition)
    {
        if (APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(this, 0))
        {
            SetActorLocation(CameraManager->GetCameraLocation());
        }
    }
    SkySphere->SetRelativeRotation(FRotator::ZeroRotator);
    UpdateFloorPlane();
    if (DynamicMaterial)
    {
        DynamicMaterial->SetScalarParameterValue(TEXT("Brightness"), Brightness);
        DynamicMaterial->SetScalarParameterValue(TEXT("YawDegrees"), YawDegrees);
        DynamicMaterial->SetScalarParameterValue(TEXT("PanoramaPitchDegrees"), PanoramaPitchDegrees);
    }
}


void AVR360SkyboxActor::UpdateFloorPlane()
{
    if (!FloorPlane) return;
    FloorPlane->SetVisibility(bShowFloorPlane);
    FloorPlane->SetHiddenInGame(!bShowFloorPlane);
    // Plane is a child of the sky cube; convert desired world elevation to component local space.
    // Using world transform keeps the reference floor stationary even while cube follows the HMD.
    FloorPlane->SetWorldLocation(FVector(GetActorLocation().X, GetActorLocation().Y, FloorWorldZ));
    FloorPlane->SetWorldRotation(FRotator::ZeroRotator);
    FloorPlane->SetWorldScale3D(FVector(FMath::Max(FloorSizeCm, 100.0f) / 100.0f));
}

#if WITH_EDITOR
void AVR360SkyboxActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);
    if (FloorPlane) UpdateFloorPlane();
}
#endif

bool AVR360SkyboxActor::OpenAndPlay(UMediaSource* NewSource)
{
    if (!MediaPlayer || !NewSource) return false;
    VideoSource = NewSource;
    bAutoPlay = true;
    return MediaPlayer->OpenSource(NewSource);
}

bool AVR360SkyboxActor::PlayVideo()
{
    return MediaPlayer && MediaPlayer->Play();
}

bool AVR360SkyboxActor::PauseVideo()
{
    return MediaPlayer && MediaPlayer->Pause();
}

bool AVR360SkyboxActor::SeekVideoSeconds(float Seconds)
{
    return MediaPlayer && MediaPlayer->Seek(FTimespan::FromSeconds(FMath::Max(0.f, Seconds)));
}

bool AVR360SkyboxActor::IsVideoPlaying() const
{
    return MediaPlayer && MediaPlayer->IsPlaying();
}

void AVR360SkyboxActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (MediaPlayer)
    {
        MediaPlayer->OnMediaOpened.RemoveDynamic(this, &AVR360SkyboxActor::HandleMediaOpened);
        MediaPlayer->Close();
    }
    Super::EndPlay(EndPlayReason);
}

void AVR360SkyboxActor::CreateAndAssignSkyMaterial()
{
#if WITH_EDITOR
    static const FString PackagePath(TEXT("/Game/VR360SkyboxGenerated/M_VR360CubeSky_FloorV05"));
    UMaterial* Material = LoadObject<UMaterial>(nullptr, *PackagePath);
    if (!Material)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            UE_LOG(LogVR360Skybox, Error, TEXT("Could not create material package."));
            return;
        }
        Material = NewObject<UMaterial>(Package, TEXT("M_VR360CubeSky_FloorV05"), RF_Public | RF_Standalone | RF_Transactional);
        if (!Material) return;
        Material->SetShadingModel(MSM_Unlit);
        Material->TwoSided = true;
        // UMaterialEditingLibrary is MinimalAPI in UE 5.8. Its non-exported
        // C++ methods cannot be linked from external plugins (LNK2019).
        // Construct the editor-only material graph directly instead.
        auto* Sample = NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
        auto* Bright = NewObject<UMaterialExpressionScalarParameter>(Material);
        auto* Multiply = NewObject<UMaterialExpressionMultiply>(Material);
        auto* CameraVector = NewObject<UMaterialExpressionCameraVectorWS>(Material);
        auto* Yaw = NewObject<UMaterialExpressionScalarParameter>(Material);
        auto* Pitch = NewObject<UMaterialExpressionScalarParameter>(Material);
        auto* CubeProjection = NewObject<UMaterialExpressionCustom>(Material);
        if (!Sample || !Bright || !Multiply || !CameraVector || !Yaw || !Pitch || !CubeProjection || !Material->GetEditorOnlyData())
        {
            UE_LOG(LogVR360Skybox, Error, TEXT("Failed to create material graph."));
            return;
        }
        Sample->MaterialExpressionEditorX = -440;
        Sample->MaterialExpressionEditorY = 0;
        Bright->MaterialExpressionEditorX = -440;
        Bright->MaterialExpressionEditorY = 220;
        Multiply->MaterialExpressionEditorX = -150;
        Multiply->MaterialExpressionEditorY = 0;
        CameraVector->MaterialExpressionEditorX = -850;
        CameraVector->MaterialExpressionEditorY = -320;
        Yaw->MaterialExpressionEditorX = -850;
        Yaw->MaterialExpressionEditorY = -90;
        Pitch->MaterialExpressionEditorX = -850;
        Pitch->MaterialExpressionEditorY = 50;
        CubeProjection->MaterialExpressionEditorX = -570;
        CubeProjection->MaterialExpressionEditorY = -280;

        Sample->ParameterName = TEXT("VideoTexture");
        Sample->Texture = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
        Sample->SamplerType = SAMPLERTYPE_Color;
        Bright->ParameterName = TEXT("Brightness");
        Bright->DefaultValue = 1.0f;
        Yaw->ParameterName = TEXT("YawDegrees");
        Yaw->DefaultValue = 0.0f;
        Pitch->ParameterName = TEXT("PanoramaPitchDegrees");
        Pitch->DefaultValue = 0.0f;

        // Per-pixel direction sampling works on all six cube faces, without mesh UV seams.
        // Unreal CameraVectorWS points from the pixel toward the eye, hence negate it.
        CubeProjection->Description = TEXT("Cube direction to Equirectangular UV (VR per-eye)");
        CubeProjection->OutputType = CMOT_Float2;
        CubeProjection->Code = TEXT(R"HLSL(
float3 D = normalize(-ViewToEye);
float angle = radians(YawAngle);
float sn = sin(angle);
float cs = cos(angle);
D = float3(D.x * cs - D.y * sn, D.x * sn + D.y * cs, D.z);
float pitchAngle = radians(PitchAngle);
float sp = sin(pitchAngle);
float cp = cos(pitchAngle);
D = float3(D.x * cp - D.z * sp, D.y, D.x * sp + D.z * cp);
float U = frac(0.5 + atan2(D.y, D.x) * 0.15915494309189535);
float V = 0.5 - asin(clamp(D.z, -1.0, 1.0)) * 0.3183098861837907;
return float2(U, V);
)HLSL");
        {
            FCustomInput& DirectionInput = CubeProjection->Inputs.AddDefaulted_GetRef();
            DirectionInput.InputName = TEXT("ViewToEye");
            DirectionInput.Input.Connect(0, CameraVector);
            FCustomInput& YawInput = CubeProjection->Inputs.AddDefaulted_GetRef();
            YawInput.InputName = TEXT("YawAngle");
            YawInput.Input.Connect(0, Yaw);
            FCustomInput& PitchInput = CubeProjection->Inputs.AddDefaulted_GetRef();
            PitchInput.InputName = TEXT("PitchAngle");
            PitchInput.Input.Connect(0, Pitch);
        }

        Material->GetExpressionCollection().AddExpression(Sample);
        Material->GetExpressionCollection().AddExpression(Bright);
        Material->GetExpressionCollection().AddExpression(Multiply);
        Material->GetExpressionCollection().AddExpression(CameraVector);
        Material->GetExpressionCollection().AddExpression(Yaw);
        Material->GetExpressionCollection().AddExpression(Pitch);
        Material->GetExpressionCollection().AddExpression(CubeProjection);
        Sample->Coordinates.Connect(0, CubeProjection);
        Multiply->A.Connect(0, Sample);
        Multiply->B.Connect(0, Bright);
        Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Multiply);
        Material->PreEditChange(nullptr);
        Material->PostEditChange();
        FAssetRegistryModule::AssetCreated(Material);
        Package->MarkPackageDirty();
        UE_LOG(LogVR360Skybox, Display, TEXT("Created /Game/VR360SkyboxGenerated/M_VR360CubeSky_FloorV05. Save the asset in Content Browser."));
    }
    Modify();
    SkyMaterial = Material;
    SkySphere->SetMaterial(0, Material);
    MarkPackageDirty();
    UE_LOG(LogVR360Skybox, Display, TEXT("Assigned Cube Material to %s"), *GetName());
#else
    UE_LOG(LogVR360Skybox, Warning, TEXT("Material generation is editor only."));
#endif
}
