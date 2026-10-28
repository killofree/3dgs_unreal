// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GaussianSplatActor.generated.h"

class UGaussianSplatComponent;
class UGaussianSplatAsset;
class UStaticMeshComponent;
class UStaticMesh;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class UDirectionalLightComponent;
class UCameraComponent;
class UNiagaraComponent;
class UNiagaraSystem;
class UExponentialHeightFogComponent;
class USkyAtmosphereComponent;
class UVolumetricCloudComponent;
class USkyLightComponent;
class UPostProcessComponent;


UENUM(BlueprintType)
enum class EStableProxyShadowDebugMode : uint8
{
	Final = 0 UMETA(DisplayName = "01 - Final 3DGS (Shadow Debug Off)"),
	StableProxyLightingMatch = 72 UMETA(DisplayName = "02 - LIVE Light - Stable Proxy Lighting"),
	ProxyMeshOnly = 73 UMETA(DisplayName = "03 - LIVE Light - Proxy Mesh Only - UE Lit"),
	ReceiverDepthDelta = 74 UMETA(DisplayName = "04 - Cast Depth Data - Receiver A/B Delta"),
	BlockerDepthDelta = 75 UMETA(DisplayName = "05 - Cast Depth Data - Blocker A/B Delta"),
	DepthCompareMargin = 76 UMETA(DisplayName = "06 - Cast Depth Data - Compare Margin"),
	VisibilityFlip = 77 UMETA(DisplayName = "07 - Cast Depth Data - Visibility A/B Flip"),
	ReceiverCoverageInstability = 78 UMETA(DisplayName = "08 - Receiver Self/Other Ownership Flip - Red = Unstable"),
	StableProxyNdotLOnly = 79 UMETA(DisplayName = "09 - Stable Proxy N.L Only (Black=0 / White=1)"),
	StableNormalAttachmentOnly = 80 UMETA(DisplayName = "10 - Stable Normal Attachment (Black=GS / White=Proxy)"),
	StableBoundBarycentricNdotLOnly = 81 UMETA(DisplayName = "11 - Bound Barycentric Proxy N.L Only"),
	StableMultiTriangleNdotLOnly = 82 UMETA(DisplayName = "12 - Multi-Triangle Smooth Proxy N.L"),
	StableRawRayVisibilityOnly = 102 UMETA(DisplayName = "13 - Raw Stable Ray Visibility (Black=Shadow / White=Lit)"),
	StableSmoothedRayVisibilityOnly = 103 UMETA(DisplayName = "14 - Smoothed Stable Ray Visibility (Black=Shadow / White=Lit)"),
	StableFinalCastVisibilityOnly = 104 UMETA(DisplayName = "15 - Final Cast Visibility (After Density/Softness/Minimum)")
};


/** One fully generated Snow state; level zero intentionally has no Snow Gaussian asset.
 * Asset, source correspondence and Snow field all belong to the SAME bake stage.
 * Each stage is saved with the level, without any viewport/camera dependency.
 */
USTRUCT()
struct FSnowHeightBakeStage2878
{
    GENERATED_BODY()

    UPROPERTY()
    float HeightCm = 0.0f;

    // v27 universal-scene safety: a baked stage is valid only for the exact
    // source 3DGS + collider + collider transform used to create it.  This
    // prevents a stage from a different scene being reused just because the
    // splat counts happen to match.
    UPROPERTY()
    TObjectPtr<UGaussianSplatAsset> SourceSplatAsset = nullptr;

    UPROPERTY()
    TObjectPtr<UStaticMesh> SourceProxyMesh = nullptr;

    UPROPERTY()
    FTransform SourceProxyLocalTransform = FTransform::Identity;

    UPROPERTY(Instanced)
    TObjectPtr<UGaussianSplatAsset> SnowAsset = nullptr;

    UPROPERTY()
    TArray<uint32> SourceCoverageMask;

    UPROPERTY()
    int32 SourceCoverageCount = 0;

    UPROPERTY()
    TArray<uint32> SnowFieldPacked;

    UPROPERTY()
    int32 SnowFieldCount = 0;

    UPROPERTY()
    float SnowFieldPatternCm = 0.0f;

    UPROPERTY()
    FTransform SnowFieldLocalToWorld = FTransform::Identity;

    UPROPERTY()
    float SourcePatchThreshold = 0.33f;

    UPROPERTY()
    int32 OriginalGaussianCount = 0;
};

/**
 * Simple actor for placing Gaussian Splats in the level.
 * Drag and drop a Gaussian Splat Asset from the Content Browser to spawn this actor.
 */
UCLASS(NotPlaceable)
class LCCCINEMATICGS_API AGaussianSplatActor : public AActor
{
	GENERATED_BODY()

public:
	AGaussianSplatActor();
	UFUNCTION()
	void GenerateWetSurfaceMesh();
	UFUNCTION()
	void DeleteWetSurfaceMesh();
	void SyncWetSurfaceMesh();
	UPROPERTY()
	TArray<FVector> WetSurfaceMeshVertices;
	uint32 WetMeshRevision = 1;
	uint32 WetMeshPublishedRevision = 0;
	FTransform WetMeshPublishedTransform;
	const void* WetMeshPublishedScene = nullptr;
	int32 WetMeshPublishedMode = -1;

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostLoad() override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
#if WITH_EDITOR
	virtual void PreEditChange(FProperty* PropertyAboutToChange) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Syncs the optional hidden proxy mesh used for mesh-to-GS shadow casting. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "05 프록시 및 Stable")
	void SyncProxyShadowReceiver();
	void SyncWeatherEffects();
	void SyncCinematicPostProcess();
	void SyncDayNightCycle();


	/** Minimal shadow test selector. 02 is the stabilized Stable Proxy Lighting path;
	 * 03 exposes the real UE-lit proxy as a reference. Additional diagnostics are added
	 * only when a new investigation explicitly needs them. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "05 프록시 및 Stable|06 디버그", meta = (DisplayName = "Debug Mode"))
	EStableProxyShadowDebugMode ShadowDebugMode = EStableProxyShadowDebugMode::Final;

	/** True only while the explicit Proxy Mesh Only presentation owns component visibility. */
	bool bStableProxyMeshOnlyPresentationActive = false;

#if WITH_EDITORONLY_DATA
	/** Set while a nested runtime-only component property is edited in Details. */
	bool bSuppressActorReconstructionForComponentTuning = false;
#endif

	/** Explicit Phase-1 build. This does not recreate the SceneProxy and does not touch Final shading. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "05 프록시 및 Stable", meta = (DisplayName = "Rebuild Stable Binding"))
	void RebuildStableProxyBinding();

	/** Camera-independent fixed LightDepth classification. Builds the validated welded-vertex 3x3 direct field; closure is stored in the stable proxy binding. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "05 프록시 및 Stable", meta = (DisplayName = "Rebuild Stable Field (Stable 3x3)"))
	void RebuildStableProxyField();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "05 프록시 및 Stable", meta = (DisplayName = "Bake Stable Cast Angle Cache"))
	void BakeStableCastAngleCache();


	/** The Gaussian Splat component */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "01 기본 설정")
	TObjectPtr<UGaussianSplatComponent> GaussianSplatComponent;

	// P07 Stage-2: world-fixed isolated puddle Gaussian test. Nothing in the
	// original GS, snow renderer, proxy shadow or old wet mesh is modified.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "04 환경 및 날씨",
		meta = (DisplayName = "비 물 Gaussian 레이어"))
	TObjectPtr<UGaussianSplatComponent> PuddleGaussianComponent;

	UPROPERTY(Instanced)
	TObjectPtr<UGaussianSplatAsset> BakedPuddleGaussianAsset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "물 Gaussian 사용 (비 상태)"))
	bool bShowPuddleGaussianTest = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "바닥 물GS 간격 (베이크)", ClampMin = "15.0", ClampMax = "100.0", Units = "cm"))
	float PuddleGaussianSpacingCm = 40.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "바닥 기준 최대 높이 (베이크)", ClampMin = "30.0", ClampMax = "600.0", Units = "cm"))
	float PuddleGroundMaxHeightCm = 220.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "[P07] 웅덩이 반경 기준 (재생성)", ClampMin = "100.0", ClampMax = "1200.0", Units = "cm"))
	float PuddleGaussianPatternCm = 350.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "[이전 P06] 미사용 노이즈 임계값", AdvancedDisplay))
	float PuddleGaussianThreshold = -0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "바닥 표면 간격 (베이크)", ClampMin = "0.5", ClampMax = "10.0", Units = "cm"))
	float PuddleGaussianLiftCm = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "[P09] 전체 바닥 덮임 테스트 (재베이크)", ToolTip = "월드 좌표 기반으로 유효 지면 셀 모두에 청록색 스플랫을 베이크합니다. 수평 웅덩이 생성 방식과 다르게 지면 높이를 따라갑니다. 실제 물웅덩이가 아니라 생성·가림 진단입니다."))
	bool bPuddleGaussianDebugFullGround = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "[P11] 지붕/상부 표면 포함 (재베이크)", ToolTip = "P09 전체 바닥 덮임 테스트 재베이크 시 눈처럼 위를 향한 상부 표면(지붕/상부 판)도 함께 포함합니다. 카메라와 무관한 월드 좌표 베이크입니다."))
	bool bPuddleIncludeUpperSurfaces = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "[P07] 웅덩이 수 (재생성)", ClampMin = "1", ClampMax = "32"))
	int32 PuddleGaussianPatchCount = 14;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "[P07] 수면 깊이 (재생성)", ClampMin = "2.0", ClampMax = "35.0", Units = "cm"))
	float PuddleGaussianWaterDepthCm = 8.0f;

	// Isolates new Gaussian diagnostics from the legacy Wet mesh and ripple shader.
	// Original Wet properties are NOT modified, so turning this OFF restores them.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "[P07] 기존 젖음 합성 분리", ToolTip = "새 물 Gaussian 테스트가 보일 때 기존 젖음/메시 물결 합성을 잠시 비활성화합니다. 원래 설정은 그대로 보존됩니다."))
	bool bPuddleGaussianIsolateOldWet = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "물GS 생성 상한 (베이크)", ClampMin = "1000", ClampMax = "100000"))
	int32 PuddleGaussianMaxCount = 30000;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "물 Gaussian 생성 결과"))
	FString PuddleGaussianBuildStatus = TEXT("미생성: 단색 물 스플렛 베이크 필요");

	// P08 diagnostic: these settings are explicitly reversible and never modify the PLY.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "[P08] 진단 - 원본 GS 임시 숨김", ToolTip = "물 스플렛의 위치를 단독으로 확인합니다. OFF 또는 Rain 종료 시 원래 표시 상태로 복원합니다."))
	bool bPuddleDebugHideSource = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "[P08] 진단 - 고불투명 단색 (재베이크)", ToolTip = "이 옵션 변경 후 [P07] 물 Gaussian 베이크를 다시 실행합니다."))
	bool bPuddleDebugOpaqueBake = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "[P08] 큰 바닥 GS 약화 강도", ClampMin = "0.0", ClampMax = "0.85", UIMin = "0.0", UIMax = "0.85", ToolTip = "0은 완전 비활성. 웅덩이와 겹치는 넓고 평평한 원본 GS만 베이크해 알파를 약화합니다. 원본 자산은 바꾸지 않습니다."))
	float PuddleBroadGroundDampen = 0.0f;

	// P10 full-floor water/ripple TEST. Does not change Gaussian placement or original splat asset.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "물 수막 투명도", ClampMin = "0.0", ClampMax = "0.90", UIMin = "0.0", UIMax = "0.60", ToolTip = "기존 P09 전체 바닥 물 Gaussian 위에 적용. 0=보이지 않음, 재베이크 불필요."))
	float PuddleWaterFilmOpacity = 0.14f;

	// P12: visual-only reflection, controlled without rebuilding the water asset.
	// The first implementation is a view/light Fresnel preview, NOT scene-color ray tracing.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "물 수막 반사 강도", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "2.0",
		ToolTip = "투명 수막에 시선/광원 반응 광택을 더합니다. 실제 주변 건물의 색상 반사 추적은 아직 아닙니다."))
	float PuddleWaterReflectionStrength = 0.85f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "지붕 최대 경사 (베이크)", ClampMin = "25.0", ClampMax = "75.0", Units = "deg"))
	float PuddleRoofMaxSlopeDegrees = 70.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "지붕 수막 두께 비율 (베이크)", ClampMin = "0.1", ClampMax = "1.0", UIMin = "0.1", UIMax = "1.0"))
	float PuddleRoofAlphaMultiplier = 0.55f;

	// P16: roof splat spacing is deliberately independent from the successful 40cm floor raster.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "지붕 물GS 간격 (베이크)", ClampMin = "12.0", ClampMax = "25.0", UIMin = "12.0", UIMax = "25.0", Units = "cm",
		ToolTip = "눈처럼 지붕에 더 작고 촘촘한 물 Gaussian을 생성합니다. 바닥 Gaussian의 간격은 변경하지 않습니다."))
	float PuddleRoofMicroSpacingCm = 16.0f;

	// P17: independent World-Z lift for tiled roof crests. The P09 ground offset is untouched.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "[P17] 지붕 물GS 높이 보정 (재베이크)", ClampMin = "0.0", ClampMax = "20.0", UIMin = "0.0", UIMax = "20.0", Units = "cm",
		ToolTip = "기와 봉우리 위에 물 GS가 표시되는지 비교하는 지붕 전용 Z 오프셋입니다. 프록시 자체나 바닥 물 GS는 수정하지 않습니다. 값을 바꾸면 다시 베이크해야 합니다."))
	float PuddleRoofLiftCm = 10.0f; // P18: retained for compatibility; source-fit path does not use this on supported cells.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "원본 GS 지붕 복원 (베이크)", ToolTip = "기와 지붕의 표면 높이/노멀을 원본 Gaussian 공분산에서 복원하며 불확실한 셀은 생성하지 않습니다."))
	bool bPuddleSourceFittedRoof = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "식생 물방울 생성 (베이크)", ToolTip = "원본 GS의 녹색 식생 후보와 공분산을 사용해 작고 성긴 물방울을 만듭니다. 지붕 평면 물막은 사용하지 않습니다."))
	bool bPuddleSourceFoliage = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "지붕 물방울 흐름", ToolTip = "지붕은 동심원 파장 대신 경사 방향으로 내려가는 물방울/물줄기 표현을 사용합니다."))
	bool bPuddleRoofUseDownflow = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "[P14] 지붕 불투명 진단 보기 (실시간)", ToolTip = "지붕 물 Gaussian이 실제로 어디에 생성되었는지 확인하기 위한 불투명 디버그 표시입니다. 지붕 표면에만 강한 색으로 표시됩니다."))
	bool bPuddleRoofOpaqueDebugView = false; // Backward compatibility with P14

	// P15: 0=off, 1=all baked roof splats blue (raw location test),
	// 2=4x4 world-coverage clipped roof splats blue. UI buttons below are authoritative.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "[P18] 지붕/식생 파랑 진단 모드", ClampMin = "0", ClampMax = "3", ToolTip = "0: 끔, 1: 지붕 원본 물GS 모두 파랑, 2: 지붕 월드 커버리지 클리핑 후 파랑. 화면 깊이와 무관하며 물막 알파를 무시합니다."))
	int32 PuddleRoofBlueDebugMode = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "지붕 물방울 강도", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "2.0"))
	float PuddleRoofFlowStrength = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "지붕 물방울 속도", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "3.0"))
	float PuddleRoofFlowSpeed = 0.9f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "[P10] 빗방울 동심원 파장 강도", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float PuddleWaterRippleStrength = 0.50f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "[P10] 빗방울 간격", ClampMin = "60.0", ClampMax = "600.0", Units = "cm"))
	float PuddleWaterDropSpacingCm = 170.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "[P10] 파장 확산 속도", ClampMin = "10.0", ClampMax = "400.0", Units = "cm/s"))
	float PuddleWaterRippleSpeed = 95.0f;

	// P19: Per-ORIGINAL-GS coverage, like snow's source-albedo correspondence.
	// 0 = real wet effect, 1 = original roof GS vivid blue, 2 = original foliage GS cyan,
	// 3 = both. Independent of P15/P18 ADDED water-splat debug modes.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "[P19] 원본 GS 지붕/나무 커버리지 진단", ClampMin = "0", ClampMax = "3"))
	int32 PuddleSourceCoverageDebug = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02A 비 효과", DisplayName = "지붕/나무 원본 GS 젖음 강도", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float PuddleSourceWetStrength = 0.65f;

	// Packed 4-bit flags per original splat: bit0 ground damping, bit1 roof source,
	// bit2 vegetation source. Kept under the old name for scene-asset compatibility.
	// P19 re-bake is required; P18 1-bit arrays deliberately fail version validation.
	// Same baked world-cell coverage drives both diagnostic and optional source damping.
	UPROPERTY()
	TArray<uint32> PuddleSourceDampenPackedBits;
	UPROPERTY()
	TObjectPtr<UGaussianSplatAsset> PuddleDampenSourceAsset;
	UPROPERTY()
	FTransform PuddleDampenSourceTransform = FTransform::Identity;
	bool HasCurrentPuddleDampen(const UGaussianSplatComponent* Source) const;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "04 환경 및 날씨", meta = (DisplayName = "비 물 Gaussian 재생성"))
	void GeneratePuddleGaussians();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "04 환경 및 날씨", meta = (DisplayName = "비 물 Gaussian 제거"))
	void DeletePuddleGaussians();

	/** v2831: Independent splat-only snow layer, rendered by the SAME global GS sorter.
	 * The base splat asset remains untouched and this layer never depends on the view. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "04 환경 및 날씨", meta = (DisplayName = "생성된 Snow Gaussian Renderer"))
	TObjectPtr<UGaussianSplatComponent> SnowGaussianComponent;

	/** Serialized actor-owned asset: survives editor save/load and duplicated MRQ worlds. */
	UPROPERTY(Instanced)
	TObjectPtr<UGaussianSplatAsset> BakedSnowGaussianAsset;

    /** Authoring range (not a runtime recalculation). One rebuild creates the configured nonzero stages. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
        meta = (GSGroup = "02B 눈 효과", DisplayName = "최대 베이크 눈높이 (재생성 필요)",
            ClampMin = "4.0", ClampMax = "50.0", UIMin = "4.0", UIMax = "30.0", Units = "cm"))
    float SnowBakeMaximumHeightCm2878 = 7.0f;

    /** New bakes can use 4..12 nonzero stages. Existing 2.8.78 four-stage saves remain compatible. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
        meta = (GSGroup = "02B 눈 효과", DisplayName = "베이크 구간 수 (4~12, 재생성 필요)",
            ClampMin = "4", ClampMax = "12", UIMin = "4", UIMax = "12"))
    int32 SnowBakeIntervalCount2879 = 8;

    /** Requests a height in centimeters; source shading changes continuously, geometry uses nearest stored stage. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
        meta = (GSGroup = "02B 눈 효과", DisplayName = "실시간 눈높이 선택 (베이크 단계)",
            ClampMin = "0.0", ClampMax = "50.0", UIMin = "0.0", UIMax = "30.0", Units = "cm"))
    float SnowRuntimeHeightCm2878 = 7.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "04 환경 및 날씨",
        meta = (GSGroup = "02B 눈 효과", DisplayName = "현재 적용된 눈높이 상태"))
    FString SnowHeightBakeStatus2878 = TEXT("기존 단일 Snow 자산: 다단계 베이크를 생성하세요.");

    /** Stage list has 4..12 nonzero independent baked states; zero is snow-off. */
    UPROPERTY()
    TArray<FSnowHeightBakeStage2878> BakedSnowHeightStages2878;

    UPROPERTY()
    float SnowBakedMaximumHeightCm2878 = 0.0f;

    /** Editor-only generator reentry guard; never serialized. */
    bool bBuildingSnowHeightStages2878 = false;

    /** During a lengthy multistage bake, keep old assets GC-reachable until all stages succeed. */
    UPROPERTY(Transient)
    TArray<FSnowHeightBakeStage2878> SnowBakeRollbackStages2879;

    UPROPERTY(Transient)
    TObjectPtr<UGaussianSplatAsset> SnowBakeRollbackAsset2879 = nullptr;

    /** Runtime transient: prevents component asset and mask churn on ordinary Tick. */
    int32 AppliedSnowHeightStage2878 = -1;

    /** Snow-only delayed GPU-cache reclamation. Keep the shared source renderer
     * untouched; inactive baked stages are checked after the chosen height
     * has stayed unchanged for a short period. No RHI work on the game thread. */
    int32 SnowCacheTrackedStage2880 = -100;
    int32 SnowCacheStableTicks2880 = 0;
    bool bSnowCacheCleanupQueued2880 = false;

    UFUNCTION(BlueprintCallable, Category = "04 환경 및 날씨|02B 눈 효과", meta = (DisplayName = "프록시 Snow Gaussian 생성"))
    void GenerateSnowGaussians();

	UFUNCTION(BlueprintCallable, Category = "04 환경 및 날씨|02B 눈 효과", meta = (DisplayName = "프록시 Snow Gaussian 삭제"))
	void DeleteSnowGaussians();

	/** Plugin-managed secondary Directional Light used as the moon after the authored sun crosses the horizon. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "02 조명 및 그림자")
	TObjectPtr<UDirectionalLightComponent> MoonDirectionalLightComponent;

	/** Runtime/editor proxy mesh hidden from the main view and used for proxy lighting/shadow capture. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, AdvancedDisplay, Category = "07 진단 및 고급")
	TObjectPtr<UStaticMeshComponent> ProxyShadowReceiverComponent;


	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "04 환경 및 날씨")
	TObjectPtr<UNiagaraComponent> WeatherNiagaraComponent;

	TWeakObjectPtr<UNiagaraSystem> LastWeatherNiagaraSystem;

	/** Native UE fog layer used to fog sky, ordinary meshes and the background. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "04 환경 및 날씨")
	TObjectPtr<UExponentialHeightFogComponent> WeatherSkyFogComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "03 색상 및 후처리")
	TObjectPtr<UPostProcessComponent> CinematicPostProcessComponent;

	uint8 LastSyncedWeatherMode = 255;
	float LastSyncedFogWeatherStrength = -1.0f;
	float LastSyncedFogVisibilityDistance = -1.0f;
	uint32 LastSyncedFogControlHash = 0u;
	UPROPERTY()
	bool bSharedFogScatteringOverride = false;
	UPROPERTY()
	FLinearColor OriginalFogSkyAmbient = FLinearColor::White;
	UPROPERTY()
	FLinearColor OriginalFogDirectionalScatter = FLinearColor::Black;
	UPROPERTY()
	float OriginalFogSkyLightScatter = 0.0f;
	TWeakObjectPtr<USkyAtmosphereComponent> CachedWeatherSkyAtmosphere;
	/** Cached world sky components used by the day/night bridge. */
	TWeakObjectPtr<USkyLightComponent> CachedDayNightSkyLight;
	// P21 fallback sky components: hidden if the level already authors its own sky.
	UPROPERTY(VisibleAnywhere, Transient, Category="02 조명 및 그림자")
	TObjectPtr<USkyAtmosphereComponent> GaussianAutoSkyAtmosphere = nullptr;
	UPROPERTY(VisibleAnywhere, Transient, Category="02 조명 및 그림자")
	TObjectPtr<UVolumetricCloudComponent> GaussianAutoVolumetricCloud = nullptr;
	float OriginalDayNightSunSourceAngle = 0.5357f;
	bool bCachedDayNightSunSourceAngle = false;
	bool bP21TriedCloudMaterial = false;
	TWeakObjectPtr<USkyAtmosphereComponent> CachedDayNightSkyAtmosphere;
	TWeakObjectPtr<USkyAtmosphereComponent> CachedDayNightBaseSkyAtmosphere;
	bool bCachedDayNightBaseSkyFactors = false;
	FLinearColor DayNightBaseSkyLuminanceFactor = FLinearColor::White;
	FLinearColor DayNightBaseSkyAerialLuminanceFactor = FLinearColor::White;
	// P24 restores the authored scattering exactly on disable/sky replacement.
	TWeakObjectPtr<USkyAtmosphereComponent> P24SkyScatteringOwner;
	FLinearColor P24OriginalRayleigh = FLinearColor::Black;
	FLinearColor P24OriginalMie = FLinearColor::Black;
	bool bP24SavedSkyScattering = false;

	TWeakObjectPtr<UDirectionalLightComponent> CachedDayNightSunLight;
	int32 CachedDayNightSunForwardPriority = 0;
	bool bCachedDayNightSunForwardPriority = false;
	FLinearColor CachedDayNightSunBaseColor = FLinearColor::White;
	bool bCachedDayNightSunBaseColor = false;
	float CachedDayNightSunBaseIntensity = 0.0f;
	bool bCachedDayNightSunBaseIntensity = false;
	// P23: distinguish the light's last calculated value from a user edit in Details.
	float P23LastAppliedSunLux = -1.0f;
	bool bP23HasAppliedSunLux = false;
	bool bP23OriginalSunLightShaftBloom = false;
	bool bP23CachedSunLightShaftBloom = false;
	bool bP23OriginalMoonLightShaftBloom = false;
	bool bP23CachedMoonLightShaftBloom = false;

	// v2799: solar-motion state. Solar elevation alone cannot distinguish sunrise
	// from sunset, so keep the last non-trivial elevation derivative and use it to
	// select independent dawn vs dusk palettes/grades.
	float LastDayNightSunHeight = 0.0f;
	bool bHasLastDayNightSunHeight = false;
	bool bDayNightSunRising = false;
	bool bCachedOriginalSkyAtmosphere = false;
	float OriginalRayleighScatteringScale = 1.0f;
	float OriginalMieScatteringScale = 1.0f;
	float OriginalMieAbsorptionScale = 1.0f;
	float OriginalMieAnisotropy = 0.8f;
	float OriginalAerialPerspectiveScale = 1.0f;
	float OriginalAerialPerspectiveStartDepth = 0.1f;
	float OriginalHeightFogContribution = 1.0f;
	FLinearColor OriginalSkyLuminanceFactor = FLinearColor::White;
	FLinearColor OriginalSkyAerialLuminanceFactor = FLinearColor::White;

	UPROPERTY(Transient)
	TObjectPtr<USceneCaptureComponent2D> ProxyLightDepthCapture;

	UPROPERTY(Transient)
	TObjectPtr<USceneCaptureComponent2D> ProxyReceiverLightDepthCapture;

	UPROPERTY(Transient)
	TObjectPtr<USceneCaptureComponent2D> ProxySurfaceDepthCapture;

	/** Camera-projected color capture of the proxy material. It is composited over
	 * the final 3DGS image; the proxy remains hidden from the main pass. */
	UPROPERTY(Transient)
	TObjectPtr<USceneCaptureComponent2D> ProxySurfaceMaterialCapture;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> ProxyLightDepthTarget;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> ProxyReceiverLightDepthTarget;


	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> ProxySurfaceDepthTarget;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> ProxySurfaceMaterialTarget;

	FVector CurrentProxyCaptureLocation = FVector::ZeroVector;
	FRotator CurrentProxyCaptureRotation = FRotator::ZeroRotator;
	float CurrentProxyCaptureFOV = 90.0f;
	float CurrentProxyCaptureAspectRatio = 1.0f;

	FTransform LastProxyCaptureActorTransform = FTransform::Identity;
	FVector LastProxyCaptureViewLocation = FVector::ZeroVector;
	FRotator LastProxyCaptureViewRotation = FRotator::ZeroRotator;
	FRotator LastProxyCaptureLightRotation = FRotator::ZeroRotator;
	FIntPoint LastProxyCaptureViewportSize = FIntPoint::ZeroValue;
	float LastProxyCaptureViewFOV = 0.0f;
	float LastProxyCaptureViewAspectRatio = 0.0f;
	float LastProxyCaptureLightIntensity = -1.0f;
	TWeakObjectPtr<AActor> LastProxyCaptureViewTarget;
	bool bHasProxyCaptureState = false;
	uint32 LastProxySurfaceMaterialHash = 0u;

	// Light-space depth is camera-independent. Track it separately so editor/game
	// camera motion does not re-render the directional depth capture every frame.
	FTransform LastProxyLightDepthReceiverTransform = FTransform::Identity;
	FVector LastProxyLightDepthReceiverBoundsMin = FVector::ZeroVector;
	FVector LastProxyLightDepthReceiverBoundsMax = FVector::ZeroVector;
	TWeakObjectPtr<UStaticMesh> LastProxyLightDepthReceiverMesh;
	FRotator LastProxyLightDepthLightRotation = FRotator::ZeroRotator;
	TWeakObjectPtr<UDirectionalLightComponent> LastProxyLightDepthDirectional;
	uint32 LastProxyLightDepthCasterHash = 0;
	bool bHasProxyLightDepthState = false;

	// v2778: last committed camera-independent LightDepth projection packet.
	// These values travel with the capture revision used by the Stable Field.
	FVector3f LastProxyLightDepthCaptureOrigin = FVector3f::ZeroVector;
	FVector3f LastProxyLightDepthCaptureForward = FVector3f(1, 0, 0);
	FVector3f LastProxyLightDepthCaptureRight = FVector3f(0, 1, 0);
	FVector3f LastProxyLightDepthCaptureUp = FVector3f(0, 0, 1);
	float LastProxyLightDepthCaptureWidth = 1.0f;

	// v2772: monotonically identifies the exact LightDepth texture + projection packet.
	// Stable Field requests carry this revision so the render thread never classifies
	// one texture using another frame's light-space transform. Zero means no capture yet.
	uint32 ProxyLightDepthCaptureRevision = 0u;


};
