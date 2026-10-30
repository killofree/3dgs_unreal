// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Scene.h"
#include "GaussianDataTypes.h"
#include "GaussianSplatComponent.generated.h"

class UGaussianSplatAsset;
class UMaterialInterface;
class UStaticMesh;
class UTextureRenderTarget2D;
class UNiagaraSystem;
class FGaussianSplatSceneProxy;
class AActor;
struct FStableProxyBindingBuildContext;

UENUM(BlueprintType)
enum class EGSWetReceiverTestMode : uint8
{
	SplatSize UMETA(DisplayName = "기존 방식 - 스플렛 크기"),
	ProxyWet UMETA(DisplayName = "테스트 - 프록시 젖음"),
	ProxyMask UMETA(DisplayName = "테스트 - 프록시 젖음 마스크"),
	WetMesh UMETA(DisplayName = "실제 젖음 메시 - 반사 및 물결"),
	WetMeshMask UMETA(DisplayName = "실제 젖음 메시 - 영역 표시")
};

/** Render profile for Gaussian Splatting. Realtime keeps the original fast path; Cinematic and Reference favor stable offline/MRQ output. */
UENUM(BlueprintType)
enum class ELCCCinematicGSRenderProfile : uint8
{
	Realtime UMETA(DisplayName = "Realtime"),
	Cinematic UMETA(DisplayName = "Cinematic"),
	Reference UMETA(DisplayName = "Reference"),
	HighQuality UMETA(DisplayName = "High Quality"),
	Original UMETA(DisplayName = "Original - Unlit")
};

UENUM(BlueprintType)
enum class ELCCCinematicGSSeasonMode : uint8
{
	Custom UMETA(DisplayName = "사용자 설정"),
	Spring UMETA(DisplayName = "봄"),
	Summer UMETA(DisplayName = "여름"),
	Autumn UMETA(DisplayName = "가을"),
	Winter UMETA(DisplayName = "겨울")
};

UENUM(BlueprintType)
enum class ELCCCinematicGSWeatherMode : uint8
{
	Clear UMETA(DisplayName = "쾌청"),
	Dawn UMETA(DisplayName = "새벽"),
	Fog UMETA(DisplayName = "안개"),
	Rain UMETA(DisplayName = "비"),
	Snow UMETA(DisplayName = "눈")
};

UENUM(BlueprintType)
enum class ELCCCinematicGSProxySurfaceEffectMode : uint8
{
	Disabled UMETA(DisplayName = "사용 안 함"),
	WetPuddles UMETA(DisplayName = "젖은 바닥 / 물웅덩이"),
	Snow UMETA(DisplayName = "눈 적설")
};

/** User-authored proxy surface layers. Materials are assigned by proxy mesh slot order. */
USTRUCT(BlueprintType)
struct FLCCCinematicGSProxySurfaceLayer
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "04 표면 레이어"))
	bool bEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "04 표면 레이어"))
	TObjectPtr<UMaterialInterface> Material = nullptr;

};

USTRUCT()
struct FLCCCinematicGSSeasonPresetSlot
{
	GENERATED_BODY()

	UPROPERTY()
	bool bValid = false;

	/** True only after the user explicitly presses the save control for this season. */
	UPROPERTY()
	bool bUserSaved = false;

	/** Persistent per-season save counter, useful for confirming the active slot changed. */
	UPROPERTY()
	int32 UserSaveRevision = 0;

	UPROPERTY()
	TMap<FName, FString> PropertyValues;
};

/** Runtime position source used by CalcViewData. Float32 is the quality baseline; Legacy reproduces the old absolute-FP16 packing. */
UENUM(BlueprintType)
enum class ELCCCinematicGSRuntimePositionPrecision : uint8
{
	LegacyPackedFP16 UMETA(DisplayName = "Legacy Packed FP16 (A/B Reference)"),
	Float32Position UMETA(DisplayName = "Float32 Position (Recommended)")
};

// Kept as private compatibility types for old serialized assets; the production
// renderer now always uses the legacy packed color and projected footprint paths.
enum class ELCCCinematicGSRuntimeColorPrecision : uint8
{
	LegacyPackedRGBA8,
	Float16ColorOpacity
};

enum class ELCCCinematicGSRuntimeFootprintMode : uint8
{
	LegacyProjectedFootprint,
	AdaptiveEWA
};

/** Runtime state of the Nanite-independent spatial de-lighting cache. */
UENUM(BlueprintType)
enum class ELCCCinematicGSSpatialDelightingCacheStatus : uint8
{
	Unknown UMETA(DisplayName = "Unknown / Not Built"),
	ValidSpatialGrid UMETA(DisplayName = "Valid Spatial Grid"),
	MissingColorData UMETA(DisplayName = "Missing Color Data"),
	InvalidPositionData UMETA(DisplayName = "Invalid Position Data / Bounds"),
	NoMappedSamples UMETA(DisplayName = "No Mapped Samples")
};

/** Runtime state of the component-local proxy surface-normal cache. */
UENUM(BlueprintType)
enum class ELCCCinematicGSProxyNormalCacheStatus : uint8
{
	Disabled UMETA(DisplayName = "Disabled"),
	Valid UMETA(DisplayName = "Valid Proxy Normal Grid"),
	MissingProxyMesh UMETA(DisplayName = "Missing Proxy Mesh"),
	MissingRenderData UMETA(DisplayName = "Proxy Mesh CPU Data Unavailable"),
	NoSamples UMETA(DisplayName = "No Proxy Samples In GS Bounds")
};

/** Phase-1 state of the camera-independent splat -> proxy triangle binding. */
UENUM(BlueprintType)
enum class EStableProxyBindingState : uint8
{
	Uninitialized UMETA(DisplayName = "Uninitialized"),
	Building UMETA(DisplayName = "Building"),
	Ready UMETA(DisplayName = "Ready"),
	Failed UMETA(DisplayName = "Failed")
};

/** Normal source used by the cinematic Unreal light bridge. */
UENUM(BlueprintType)
enum class ELCCCinematicGSLightingNormalMode : uint8
{
	SplatAxisCameraFacing = 0 UMETA(DisplayName = "Splat Axis - Camera Facing"),
	SplatAxisRaw = 1 UMETA(DisplayName = "Splat Axis - Raw"),
	WorldUp = 2 UMETA(DisplayName = "World Up"),
	HybridWorldUp = 3 UMETA(DisplayName = "Hybrid World Up"),
	ProxySurface = 4 UMETA(DisplayName = "Proxy Surface Normal"),
	HybridProxy = 5 UMETA(DisplayName = "Hybrid Proxy / Splat (Recommended)")
};

/** Debug preview for the cinematic lighting mask before color grading. */
UENUM(BlueprintType)
enum class ELCCCinematicGSLightingPreviewMode : uint8
{
	Final = 0 UMETA(DisplayName = "Final"),
	LightMask = 1 UMETA(DisplayName = "Light Mask"),
	ShadowMask = 2 UMETA(DisplayName = "Shadow Mask"),
	LightHeatmap = 3 UMETA(Hidden),
	ProxyMeshDebug = 4 UMETA(DisplayName = "Proxy Mesh Reference"),
	BlockerDistance = 5 UMETA(DisplayName = "Blocker-Receiver Distance"),
	OriginalProjectedRadius = 6 UMETA(Hidden),
	AdjustedProjectedRadius = 7 UMETA(Hidden),
	ProjectedAnisotropy = 8 UMETA(Hidden),
	FootprintOpacityCompensation = 9 UMETA(Hidden),
	FootprintCoverage = 10 UMETA(Hidden),
	OriginalGS = 11 UMETA(Hidden),
	BakedLightingEstimate = 12 UMETA(Hidden),
	ApproxAlbedo = 13 UMETA(Hidden),
	RemovedBakedLighting = 14 UMETA(Hidden),
	RelitIndirect = 15 UMETA(Hidden),
	RelitDirect = 16 UMETA(DisplayName = "진단 - 실시간 직사광만"),
	SpatialShadowVisibility = 17 UMETA(Hidden),
	OriginalFinalDifference = 18 UMETA(Hidden),
	SpatialCacheConfidence = 19 UMETA(Hidden),
	SpatialCellID = 20 UMETA(Hidden),
	SpatialCorrectionWeight = 21 UMETA(Hidden),
	SpatialCacheStatus = 22 UMETA(Hidden),
	SpatialLegacyDifference = 23 UMETA(Hidden),
	SpatialSignedLuminanceDifference = 24 UMETA(Hidden),
	SpatialChromaDifference = 25 UMETA(Hidden),
	SpatialCorrectionMagnitude = 26 UMETA(Hidden),
	ProxySurfaceNormal = 27 UMETA(DisplayName = "Surface Normal - Proxy"),
	BlendedLightingNormal = 28 UMETA(DisplayName = "Surface Normal - Final Blend"),
	ProxyNormalConfidence = 29 UMETA(DisplayName = "Surface Normal - Confidence"),
	LightingNdotL = 30 UMETA(DisplayName = "Surface Normal - N dot L"),
	LightDepthCapture = 34 UMETA(DisplayName = "그림자 - 라이트 Depth"),
	FinalShadowVisibility = 35 UMETA(DisplayName = "그림자 - 최종 프록시 직사광 + 지평선 게이트"),
	ReflectedLightOnly = 36 UMETA(DisplayName = "조명 - 반사광만"),
	DirectAndReflected = 37 UMETA(DisplayName = "조명 - 직사광 + 반사광"),
	GaussianSunAccentOnly = 38 UMETA(DisplayName = "테스트 - Gaussian 직사광만"),
	GaussianPlantTransmissionOnly = 39 UMETA(DisplayName = "테스트 - 식물 투과광만")
};

/** Diagnostic view for the final accumulated GS color foundation pass. */
UENUM(BlueprintType)
enum class ELCCCinematicGSCompositeDebugView : uint8
{
	Final UMETA(DisplayName = "Final"),
	OriginalAccumulated UMETA(Hidden, DisplayName = "Original Accumulated"),
	GradedAccumulated UMETA(Hidden, DisplayName = "Graded Accumulated"),
	DifferenceX5 UMETA(Hidden, DisplayName = "Difference x5"),
	Luminance UMETA(Hidden, DisplayName = "Luminance"),
	MidtoneMask UMETA(Hidden, DisplayName = "Gamma Midtone Mask"),
	GreenLuminanceMask UMETA(Hidden, DisplayName = "Green Luminance Mask"),
	FineLocalDetail UMETA(Hidden, DisplayName = "Fine Local Detail"),
	CoarseLocalDetail UMETA(Hidden, DisplayName = "Coarse Local Detail"),
	Coverage UMETA(Hidden, DisplayName = "GS Coverage"),
	ChromaScale UMETA(Hidden, DisplayName = "Saturation Scale"),
	FilmicRatio UMETA(Hidden, DisplayName = "Gamma/Highlight Luminance Ratio")
};

/**
 * Component for rendering Gaussian Splatting assets in the scene
 */
UCLASS(
	ClassGroup = (Rendering),
	meta = (BlueprintSpawnableComponent),
	hidecategories = (
		Activation,
		AssetUserData,
		Collision,
		ComponentTick,
		Cooking,
		Navigation,
		Physics,
		RayTracing,
		Replication,
		Tags,
		Variable
	))
class LCCCINEMATICGS_API UGaussianSplatComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UGaussianSplatComponent(const FObjectInitializer& ObjectInitializer);

	//~ Begin UObject Interface
	virtual void PostLoad() override;
#if WITH_EDITOR
	virtual void PreEditChange(FProperty* PropertyAboutToChange) override;
	virtual void PreEditChange(FEditPropertyChain& PropertyAboutToChange) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedEvent) override;
	bool IsEditorRealtimePropertyChangeActive() const { return bEditorRealtimePropertyChangeActive; }
#endif
	//~ End UObject Interface

	//~ Begin UActorComponent Interface
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void SendRenderDynamicData_Concurrent() override;
	//~ End UActorComponent Interface

	//~ Begin UPrimitiveComponent Interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;
	//~ End UPrimitiveComponent Interface

	/** Set the Gaussian Splat asset to render */
	UFUNCTION(BlueprintCallable, Category = "01 기본 설정")
	void SetSplatAsset(UGaussianSplatAsset* NewAsset);
	// P08: safe public refresh for the owning actor after a world-space puddle bake.
	void RefreshPuddleDampenRenderState();

	/** Get the currently assigned asset */
	UFUNCTION(BlueprintCallable, Category = "01 기본 설정")
	UGaussianSplatAsset* GetSplatAsset() const { return SplatAsset; }

	/** Get the number of splats being rendered */
	UFUNCTION(BlueprintCallable, Category = "01 기본 설정")
	int32 GetSplatCount() const;

	/** Get the proxy mesh assigned for future mesh-to-GS shadow catching. */
	UFUNCTION(BlueprintCallable, Category = "05 프록시 및 Stable")
	UStaticMesh* GetProxyShadowMesh() const { return ProxyShadowMesh; }

	/** Local transform from the Gaussian splat component to the assigned proxy shadow mesh. */
	UFUNCTION(BlueprintCallable, Category = "05 프록시 및 Stable")
	FTransform GetProxyShadowMeshLocalTransform() const;

	/** World transform for the assigned proxy shadow mesh after applying the manual alignment offset. */
	UFUNCTION(BlueprintCallable, Category = "05 프록시 및 Stable")
	FTransform GetProxyShadowMeshWorldTransform() const;

	/** Latest CPU-side lighting payload collected from active Unreal LightComponents. */
	const FGaussianSplatLightingData& GetCachedLightingData() const { return CachedLightingData; }

	/** Rebuild/read the shared spatial de-lighting cache metadata and expose its current state in Details. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "02 조명 및 그림자")
	void RefreshSpatialDelightingCacheStatus();

	/** Rebuild the component-local proxy-mesh normal grid used by Hybrid Proxy lighting. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "05 프록시 및 Stable")
	void RebuildProxyNormalCache();

	/** Phase 1: begin a full camera-independent TriangleID + barycentric binding build. Work is spread over editor/runtime ticks. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "05 프록시 및 Stable", meta = (DisplayName = "Rebuild Stable Binding"))
	void RebuildStableProxyBindings();
	/** Load the last manually built binding cache when all source inputs still match. */
	bool TryLoadStableProxyBindingCache();
	FString LastStableBindingCacheLoadKey;

	/** Continue exactly one bounded binding chunk. Called by the owning actor before capture work. */
	void ContinueStableProxyBindingBuild();

	void InvalidateStableProxyBindings(const TCHAR* Reason = TEXT("SourceChanged"));
	const TArray<FStableProxySplatBindingGPU>& GetStableSplatBindings() const
	{
		static const TArray<FStableProxySplatBindingGPU> EmptyBindings;
		return StableSplatBindings.IsValid() ? *StableSplatBindings : EmptyBindings;
	}
	TSharedPtr<const TArray<FStableProxySplatBindingGPU>, ESPMode::ThreadSafe> GetStableSplatBindingPayload() const { return StableSplatBindings; }
	const TArray<FStableProxyVertexGPU>& GetStableProxyVertices() const { return StableProxyVertices; }
	const TArray<FStableProxyTriangleGPU>& GetStableProxyTriangles() const { return StableProxyTriangles; }
	const TArray<FVector4f>& GetStableProxyTriangleVertexNormals() const { return StableProxyTriangleVertexNormals; }
	const TArray<FVector4f>& GetStableProxyRayProofs() const { return StableProxyRayProofs; }
	uint32 GetStableBindingRevision() const { return StableBindingRevision; }
	uint32 GetStableRayProofRevision() const { return StableRayProofRevision; }

	/** Phase 2/3/4: request one camera-independent LightDepth classification of the welded proxy surface plus adaptive micro-surface samples. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "05 프록시 및 Stable", meta = (DisplayName = "Rebuild Stable Field (Stable 3x3)"))
	void RequestStableProxyFieldBuild();
	// Internal live-light path: bind the field request to one exact LightDepth packet.
	void RequestStableProxyFieldBuildForCapture(uint32 LightDepthCaptureRevision);
	void RebuildStableProxyRayVisibility();
	/** Static world-up rain exposure field. 1=open sky, 0=blocked by proxy geometry. */
	void RebuildStableRainExposure();
	/** Precompute deterministic BVH cast fields around a full local-light yaw orbit. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "05 프록시 및 Stable",
		meta = (DisplayName = "Bake Stable Cast Angle Cache"))
	void BakeStableCastAngleCache();
	uint32 GetStableFieldRequestRevision() const { return StableFieldRequestRevision; }
	uint32 GetStableFieldExpectedLightDepthRevision() const { return StableFieldExpectedLightDepthRevision; }
	float GetStableFieldDepthBiasCm() const { return StableFieldDepthBiasCm; }

	const TArray<FFloat16Color>& GetProxyNormalGridData() const { return ProxyNormalGridData; }
	FVector4f GetProxyNormalGridMinCellSize() const { return ProxyNormalGridMinCellSize; }
	FVector4f GetProxyNormalGridDimensionsStatus() const { return ProxyNormalGridDimensionsStatus; }

public:
	/**
	 * Details-panel save trigger. Enable once to capture every editable 3DGS option
	 * into the currently selected season slot; it resets itself immediately.
	 * This remains visible when the component is displayed inline on its owning actor,
	 * where component CallInEditor buttons can be hidden by the editor.
	 */
	UPROPERTY(EditAnywhere, Category = "04 환경 및 날씨",
		meta = (GSGroup = "01 계절 프리셋", DisplayName = "현재 계절 프리셋 저장", ToolTip = "체크하면 계절 전용 식생 색상 옵션만 현재 계절 슬롯에 저장합니다."))
	bool bSaveCurrentSeasonPresetNow = false;

	/** Number of options stored in the currently selected season slot. */
	UPROPERTY(Transient)
	int32 LastSavedSeasonOptionCount = 0;

	UPROPERTY(Transient)
	bool bCurrentSeasonUserPresetSaved = false;

	UPROPERTY(Transient)
	int32 CurrentSeasonUserSaveRevision = 0;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "04 환경 및 날씨",
		meta = (DisplayName = "현재 값을 현재 계절에 저장"))
	void SaveCurrentSeasonPreset();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "01 계절 프리셋", ClampMin = "1", ClampMax = "999", UIMin = "1", UIMax = "100",
			DisplayName = "사용자 계절 프리셋 번호", ToolTip = "1~999 중 저장하거나 불러올 사용자 계절 프리셋 번호입니다."))
	int32 NumberedSeasonPresetIndex = 1;

	UPROPERTY(EditAnywhere, Category = "04 환경 및 날씨",
		meta = (GSGroup = "01 계절 프리셋", DisplayName = "선택 번호에 계절 저장"))
	bool bSaveNumberedSeasonPresetNow = false;

	UPROPERTY(EditAnywhere, Category = "04 환경 및 날씨",
		meta = (GSGroup = "01 계절 프리셋", DisplayName = "선택 번호 계절 불러오기"))
	bool bLoadNumberedSeasonPresetNow = false;

	UPROPERTY(VisibleAnywhere, Transient, Category = "04 환경 및 날씨",
		meta = (GSGroup = "01 계절 프리셋", DisplayName = "선택 계절 번호 저장됨"))
	bool bSelectedNumberedSeasonPresetSaved = false;

	UPROPERTY(VisibleAnywhere, Transient, Category = "04 환경 및 날씨",
		meta = (GSGroup = "01 계절 프리셋", DisplayName = "선택 계절 번호 옵션 수"))
	int32 SelectedNumberedSeasonOptionCount = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "04 환경 및 날씨",
		meta = (GSGroup = "01 계절 프리셋", DisplayName = "선택 계절 번호 저장 회차"))
	int32 SelectedNumberedSeasonSaveRevision = 0;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "04 환경 및 날씨", meta = (DisplayName = "선택 번호에 계절 저장"))
	void SaveNumberedSeasonPreset();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "04 환경 및 날씨", meta = (DisplayName = "선택 번호 계절 불러오기"))
	void LoadNumberedSeasonPreset();

	UPROPERTY()
	TMap<int32, FLCCCinematicGSSeasonPresetSlot> NumberedSeasonPresetSlots;

	/** Top-level season selector. Changing it applies the complete matching preset immediately. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "01 계절 프리셋", DisplayName = "계절 모드"))
	ELCCCinematicGSSeasonMode SeasonMode = ELCCCinematicGSSeasonMode::Autumn;

	/** Serialized user-authored effect values for each season. Hidden from Details; switching SeasonMode saves/loads them. */
	UPROPERTY()
	FLCCCinematicGSSeasonPresetSlot SpringPresetSlot;

	UPROPERTY()
	FLCCCinematicGSSeasonPresetSlot SummerPresetSlot;

	UPROPERTY()
	FLCCCinematicGSSeasonPresetSlot AutumnPresetSlot;

	UPROPERTY()
	FLCCCinematicGSSeasonPresetSlot WinterPresetSlot;

	UPROPERTY(Transient)
	ELCCCinematicGSSeasonMode PreviousSeasonMode = ELCCCinematicGSSeasonMode::Autumn;

	/** Weather is an independent presentation layer applied after the season preset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 날씨 프리셋", DisplayName = "날씨 모드"))
	ELCCCinematicGSWeatherMode WeatherMode = ELCCCinematicGSWeatherMode::Clear;

	/**
	 * P49: animate optical visibility for a soft clearing shot.
	 * All distances retain continuous height fog; there is no moving hard start.
	 * 1..2 interpolates log density up to dense near-field fog at strength 2.
	 * Native meshes and GS share the applied physical fog parameters.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, Category = "04 환경 및 날씨",
		meta = (GSGroup = "03 대기 및 안개", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "2.0",
			DisplayName = "안개 강도 / 부드러운 걷힘 (0~2)",
			ToolTip = "0: 안개 없음, 1: 기본 안개, 2: 매우 짙은 근거리 안개. 2에서 0으로 애니메이션하면 밀도가 연속적으로 낮아져 가시거리가 늘어납니다. 가까운 곳에도 옅은 안개가 남으며 원형 시작 경계를 만들지 않습니다."))
	float FogWeatherStrength = 1.0f;

	/** Approximate distance in centimeters at which dense fog reaches full coverage. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "03 대기 및 안개", ClampMin = "100.0", ClampMax = "1000000.0", UIMin = "1000.0", UIMax = "100000.0",
			DisplayName = "안개 가시거리 (cm) - GS·메시 공통"))
	float FogVisibilityDistance = 6500.0f;

	/** Enables the same panorama-safe fog overlay while Clear/Dawn/Rain/Snow weather is active. Fog weather always uses the overlay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "03 대기 및 안개", DisplayName = "다른 날씨에서도 안개 사용"))
	bool bEnableFogOverlayInOtherWeather = false;

	// P44: A single native UE ExponentialHeightFogComponent controls world meshes.
	// These settings are authored in the existing fog group and are independent
	// of the deprecated P41 distant-fog and P42 DOF experiments.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "03 대기 및 안개", DisplayName = "언리얼 기본 포그 밀도",
			ClampMin = "0.0", ClampMax = "0.2", UIMin = "0.0", UIMax = "0.05",
			ToolTip = "Unreal Exponential Height Fog의 Fog Density 기준값입니다. 가시거리와 안개 강도도 반영됩니다."))
	float NativeFogDensity = 0.02f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "03 대기 및 안개", DisplayName = "근거리 안개 완화 거리 (cm) - GS·메시 공통",
			ClampMin = "0.0", ClampMax = "1000000.0", UIMin = "0.0", UIMax = "100000.0", Units = "cm",
			ToolTip = "값을 높이면 기본 안개 밀도를 낮춰 근거리 가시거리를 부드럽게 늘립니다. 이 거리 안쪽을 완전히 비우지 않습니다. 강도 2에서는 짙은 안개로 수렴합니다. 기존 시작 거리 저장값과 키는 유지됩니다."))
	float NativeFogStartDistanceCm = 1500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "03 대기 및 안개", DisplayName = "언리얼 기본 포그 최대 불투명도",
			ClampMin = "0.0", ClampMax = "0.999", UIMin = "0.0", UIMax = "0.99"))
	float NativeFogMaxOpacity = 0.95f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "03 대기 및 안개", DisplayName = "언리얼 기본 체적 포그 사용",
			ToolTip = "기본값은 OFF입니다. 360도 파노라마에서는 화면 공간 볼륨 포그의 타일 차이를 검증하기 전까지 OFF를 권장합니다."))
	bool bEnableNativeVolumetricFog = false;

	/** Shared fog inscattering colour for UE Exponential/Volumetric Fog and the late 3DGS fog bridge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "03 대기 및 안개", DisplayName = "안개 색상"))
	FLinearColor FogColor = FLinearColor(0.25f, 0.30f, 0.38f, 1.0f);

	/** Diffuse daylight scattering target at strength 2; shared by meshes and GS. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "03 대기 및 안개", DisplayName = "짙은 안개 색상 (강도 2)",
			ToolTip = "강도 1~2에서 기본 안개 색상으로부터 이 색상으로 부드럽게 전환합니다. 기본값은 흐린 날의 밝은 중성 회색입니다. 화면 덮개가 아닌 실제 안개 산란색이며 GS와 메시에 공통 적용됩니다."))
	FLinearColor DenseFogScatteringColor = FLinearColor(0.62f, 0.65f, 0.67f, 1.0f);

	/** Saves only the fog controls for the currently selected WeatherMode.
	 *  This is deliberately independent from the full weather preset so Rain/Snow
	 *  lighting, wetness, shadows and other weather systems are never touched. */
	UPROPERTY(EditAnywhere, Category = "04 환경 및 날씨",
		meta = (GSGroup = "03 대기 및 안개", DisplayName = "현재 날씨 안개 설정 저장",
			ToolTip = "현재 03 대기 및 안개의 안개 관련 값만 현재 날씨(쾌청/새벽/안개/비/눈) 슬롯에 저장합니다."))
	bool bSaveCurrentWeatherFogProfileNow = false;

	UPROPERTY(VisibleAnywhere, Transient, Category = "04 환경 및 날씨",
		meta = (GSGroup = "03 대기 및 안개", DisplayName = "현재 날씨 안개 저장됨"))
	bool bCurrentWeatherFogProfileSaved = false;

	UPROPERTY(VisibleAnywhere, Transient, Category = "04 환경 및 날씨",
		meta = (GSGroup = "03 대기 및 안개", DisplayName = "현재 날씨 안개 옵션 수"))
	int32 CurrentWeatherFogProfileOptionCount = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "04 환경 및 날씨",
		meta = (GSGroup = "03 대기 및 안개", DisplayName = "현재 날씨 안개 저장 회차"))
	int32 CurrentWeatherFogProfileSaveRevision = 0;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "04 환경 및 날씨", meta = (DisplayName = "현재 날씨 안개 설정 저장"))
	void SaveCurrentWeatherFogProfile();

	// Dedicated per-weather fog profiles. These slots contain only fog controls and
	// are applied after the normal weather preset, so each weather can have its own
	// fog without changing any non-fog behaviour.
	UPROPERTY()
	FLCCCinematicGSSeasonPresetSlot ClearFogProfileSlot;
	UPROPERTY()
	FLCCCinematicGSSeasonPresetSlot DawnFogProfileSlot;
	UPROPERTY()
	FLCCCinematicGSSeasonPresetSlot FogFogProfileSlot;
	UPROPERTY()
	FLCCCinematicGSSeasonPresetSlot RainFogProfileSlot;
	UPROPERTY()
	FLCCCinematicGSSeasonPresetSlot SnowFogProfileSlot;


	UPROPERTY(EditAnywhere, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02 날씨 프리셋", DisplayName = "현재 날씨 프리셋 저장", ToolTip = "체크하면 날씨 전용 대기/안개/노출/효과 값만 현재 날씨 슬롯에 저장합니다."))
	bool bSaveCurrentWeatherPresetNow = false;

	UPROPERTY(Transient)
	int32 LastSavedWeatherOptionCount = 0;

	UPROPERTY(Transient)
	bool bCurrentWeatherUserPresetSaved = false;

	UPROPERTY(Transient)
	int32 CurrentWeatherUserSaveRevision = 0;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "04 환경 및 날씨", meta = (DisplayName = "현재 값을 현재 날씨에 저장"))
	void SaveCurrentWeatherPreset();

	/** User-selected persistent weather preset slot. Stored with this component in the level. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02 날씨 프리셋", ClampMin = "1", ClampMax = "999", UIMin = "1", UIMax = "100",
			DisplayName = "사용자 날씨 프리셋 번호", ToolTip = "1~999 중 저장하거나 불러올 사용자 날씨 프리셋 번호입니다."))
	int32 NumberedWeatherPresetIndex = 1;

	UPROPERTY(EditAnywhere, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02 날씨 프리셋", DisplayName = "선택 번호에 날씨 저장", ToolTip = "체크하면 현재 날씨 수치를 선택한 사용자 프리셋 번호에 저장하고 자동으로 체크를 해제합니다."))
	bool bSaveNumberedWeatherPresetNow = false;

	UPROPERTY(EditAnywhere, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02 날씨 프리셋", DisplayName = "선택 번호 날씨 불러오기", ToolTip = "체크하면 선택한 사용자 프리셋 번호의 날씨 수치를 불러오고 자동으로 체크를 해제합니다."))
	bool bLoadNumberedWeatherPresetNow = false;

	UPROPERTY(VisibleAnywhere, Transient, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02 날씨 프리셋", DisplayName = "선택 날씨 번호 저장됨"))
	bool bSelectedNumberedWeatherPresetSaved = false;

	UPROPERTY(VisibleAnywhere, Transient, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02 날씨 프리셋", DisplayName = "선택 날씨 번호 옵션 수"))
	int32 SelectedNumberedWeatherOptionCount = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "04 환경 및 날씨",
		meta = (GSGroup = "02 날씨 프리셋", DisplayName = "선택 날씨 번호 저장 회차"))
	int32 SelectedNumberedWeatherSaveRevision = 0;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "04 환경 및 날씨", meta = (DisplayName = "선택 번호에 날씨 저장"))
	void SaveNumberedWeatherPreset();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "04 환경 및 날씨", meta = (DisplayName = "선택 번호 날씨 불러오기"))
	void LoadNumberedWeatherPreset();

	/** Persistent user weather presets indexed by NumberedWeatherPresetIndex. */
	UPROPERTY()
	TMap<int32, FLCCCinematicGSSeasonPresetSlot> NumberedWeatherPresetSlots;

	UPROPERTY()
	FLCCCinematicGSSeasonPresetSlot ClearWeatherPresetSlot;
	UPROPERTY()
	FLCCCinematicGSSeasonPresetSlot DawnWeatherPresetSlot;
	UPROPERTY()
	FLCCCinematicGSSeasonPresetSlot FogWeatherPresetSlot;
	UPROPERTY()
	FLCCCinematicGSSeasonPresetSlot RainWeatherPresetSlot;
	UPROPERTY()
	FLCCCinematicGSSeasonPresetSlot SnowWeatherPresetSlot;
	UPROPERTY(Transient)
	ELCCCinematicGSWeatherMode PreviousWeatherMode = ELCCCinematicGSWeatherMode::Clear;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 날씨 프리셋", DisplayName = "쾌청 이펙트"))
	TObjectPtr<UNiagaraSystem> ClearWeatherNiagara;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 날씨 프리셋", DisplayName = "새벽 이펙트"))
	TObjectPtr<UNiagaraSystem> DawnWeatherNiagara;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 날씨 프리셋", DisplayName = "안개 이펙트"))
	TObjectPtr<UNiagaraSystem> FogWeatherNiagara;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 날씨 프리셋", DisplayName = "비 이펙트"))
	TObjectPtr<UNiagaraSystem> RainWeatherNiagara;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 날씨 프리셋", DisplayName = "눈 이펙트"))
	TObjectPtr<UNiagaraSystem> SnowWeatherNiagara;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 날씨 프리셋", DisplayName = "이펙트 위치 오프셋"))
	FVector WeatherNiagaraOffset = FVector(0.0, 0.0, 300.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 날씨 프리셋", ClampMin = "0.01", ClampMax = "100.0", DisplayName = "이펙트 스케일"))
	float WeatherNiagaraScale = 1.0f;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "04 환경 및 날씨", meta = (DisplayName = "현재 날씨 다시 적용"))
	void ApplyWeatherPreset();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "01 기본 설정", meta = (DisplayName = "전체 효과 적용"))
	bool bEnableCinematicEffects = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "01 기본 설정", meta = (DisplayName = "3DGS 에셋"))
	TObjectPtr<UGaussianSplatAsset> SplatAsset;
	#if 0

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "01 그림자 합성", ClampMin = "0.0", ClampMax = "8.0", DisplayName = "그림자 부드러움"))
	#endif
	/** Rejects only neighboring filter taps that belong to another proxy surface depth. */
	UPROPERTY()
	float CapturedShadowDepthEdgeProtection = 1.0f;

	/** Absolute forward-depth tolerance used to trust neighboring filtered samples. */
	UPROPERTY()
	float CapturedShadowDepthToleranceCm = 14.0f;

	/** Additional filter tolerance proportional to camera distance. */
	UPROPERTY()
	float CapturedShadowDepthToleranceScale = 0.008f;

	#if 0

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "01 그림자 합성", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "그림자 합성 강도"))
	float CapturedBaseShadowTransferStrength = 0.70f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "01 그림자 합성", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "그림자 최소 밝기"))
	float CapturedBaseShadowMinimum = 0.55f;
	#endif
	UPROPERTY()
	float CapturedBaseShadowTransferStrength = 0.70f;
	UPROPERTY()
	float CapturedBaseShadowMinimum = 0.55f;
	#if 0

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "01 그림자 합성", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "그림자 원본 색 유지"))
	#endif
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "01 그림자 합성", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", DisplayName = "반사광 원본 색상 유지"))
	float CapturedBaseShadowSaturation = 1.0f;
	#if 0

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "01 그림자 합성", DisplayName = "그림자 반사광 색상"))
	#endif
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "01 그림자 합성", DisplayName = "반사광 색상"))
	FLinearColor CapturedBaseShadowTint = FLinearColor(0.94f, 0.97f, 1.0f, 1.0f);

	/** Luminance-preserving strength of the reflected-light tint. Zero is neutral white; one uses the authored tint fully. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "01 그림자 합성", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.5", DisplayName = "반사광 색상 적용 강도"))
	float CapturedReflectedLightTintStrength = 0.15f;

	/** Fades the authored tint back toward neutral in the deepest shadows, preventing blue/colored black crush. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "01 그림자 합성", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", DisplayName = "깊은 그림자 틴트 중성화"))
	float CapturedReflectedLightDeepTintNeutralization = 0.70f;

	/** Material-color retention used only in deep shadow. High values keep wood, foliage, and stone hues readable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "01 그림자 합성", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.5", UIMax = "1.0", DisplayName = "깊은 그림자 원본 색상 유지"))
	float CapturedReflectedLightDeepSaturationRetention = 0.94f;

	/** Compresses HDR source luminance before reflected fill is added, preventing pale surfaces from turning milky white. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "01 그림자 합성", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "1.0", DisplayName = "반사광 하이라이트 압축"))
	float CapturedReflectedLightHighlightCompression = 0.35f;

	/** Width of the tint fade-in through penumbra. Larger values make shadow color changes gentler. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "01 그림자 합성", ClampMin = "0.05", ClampMax = "1.0", UIMin = "0.1", UIMax = "0.8", DisplayName = "반사광 색상 전환 부드러움"))
	float CapturedReflectedLightTintSoftness = 0.45f;
	#if 0

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "01 그림자 합성", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "그림자 반사광 강도"))
	#endif
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "01 그림자 합성", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.8", DisplayName = "반사광 강도"))
	float CapturedShadowBounceStrength = 0.26f;

	/** How much receiver closure is allowed to block reflected fill. Zero preserves reflection in closed areas; one lets closure remove it fully. */
	// v2748 compatibility-only. Stable closure now owns reflected-light attenuation automatically.
	UPROPERTY()
	float CapturedReflectedLightClosureOcclusion = 0.35f;

	/** Shapes how rapidly reflected fill appears as direct visibility is lost. Below one spreads fill into penumbrae; above one confines it to deeper shadows. */
	// v2748 compatibility-only. The unified cast/closure density controls shape the response.
	UPROPERTY()
	float CapturedReflectedLightResponseGamma = 0.78f;

	/** Deprecated and intentionally ignored. Sun-derived reflected fill always disappears below the weather horizon. */
	UPROPERTY()
	float CapturedReflectedLightHorizonRetention = 0.0f;

	/** Rendering profile. Cinematic and Reference disable the camera-static sort cache for stable frame-by-frame output. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "06 렌더링 및 성능")
	ELCCCinematicGSRenderProfile RenderProfile = ELCCCinematicGSRenderProfile::Realtime;

	/** Runtime position precision. Float32 removes the absolute half-float position grid without requiring asset reimport. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "06 렌더링 및 성능", meta = (DisplayName = "런타임 위치 정밀도"))
	ELCCCinematicGSRuntimePositionPrecision RuntimePositionPrecision = ELCCCinematicGSRuntimePositionPrecision::Float32Position;

	// Compatibility-only, intentionally not reflected in the Details panel.
	ELCCCinematicGSRuntimeColorPrecision RuntimeColorPrecision = ELCCCinematicGSRuntimeColorPrecision::Float16ColorOpacity;
	ELCCCinematicGSRuntimeFootprintMode RuntimeFootprintMode = ELCCCinematicGSRuntimeFootprintMode::LegacyProjectedFootprint;
	float AdaptiveFootprintStrength = 0.0f;
	float FootprintNearMinRadiusPixels = 1.15f;
	float FootprintMinRadiusPixels = 0.85f;
	float FootprintFarMinRadiusPixels = 0.65f;
	float FootprintNearMaxRadiusPixels = 8.0f;
	float FootprintMidMaxRadiusPixels = 10.0f;
	float FootprintFarMaxRadiusPixels = 14.0f;
	float FootprintNearDistance = 2500.0f;
	float FootprintFarDistance = 9000.0f;
	float FootprintAnisotropyLimit = 12.0f;
	float FootprintOpacityCompensation = 0.0f;
	float FootprintOpacityCompensationMaxExponent = 1.0f;
	float FootprintRadiusQuantization = 0.0f;
	float FootprintAdaptiveLookSafety = 1.0f;

	/** Force recalculation and radix sort every rendered frame. Recommended for Movie Render Queue / sequencer output. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "06 렌더링 및 성능")
	bool bForceFullSortEveryFrame = false;

	/** Disable LOD splat rendering for maximum fidelity. This is expensive but useful for final still/reference checks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "06 렌더링 및 성능")
	bool bDisableLODInReference = true;

	/** Receive active Unreal LightComponents through the cinematic splat lighting bridge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", DisplayName = "언리얼 조명 받기"))
	bool bReceiveUnrealLights = true;

	/** Retained for serialized compatibility. Public shadow diagnostics now live only on the actor 01/02/03 selector. */
	UPROPERTY()
	ELCCCinematicGSLightingPreviewMode LightingPreviewMode = ELCCCinematicGSLightingPreviewMode::Final;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "05 프록시 및 Stable", meta = (DisplayName = "최종 결과로 돌아가기"))
	void PreviewFinalLighting();

	UFUNCTION(BlueprintCallable, Category = "07 진단 및 고급")
	void PreviewLightMask();

	UFUNCTION(BlueprintCallable, Category = "07 진단 및 고급")
	void PreviewShadowMask();


	/** Visualize normalized light-space blocker-to-receiver distance. Black is contact; white reaches Distance Range. */
	UFUNCTION(BlueprintCallable, Category = "07 진단 및 고급")
	void PreviewBlockerDistance();

	/** Apply the second-integration outdoor daylight preset used for the supplied target reference. */
	UFUNCTION(BlueprintCallable, Category = "02 조명 및 그림자")
	void ApplyTargetDaylightPreset();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "04 환경 및 날씨", meta = (DisplayName = "봄 프리셋 적용"))
	void ApplySpringSeasonPreset();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "04 환경 및 날씨", meta = (DisplayName = "여름 프리셋 적용"))
	void ApplySummerSeasonPreset();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "04 환경 및 날씨", meta = (DisplayName = "가을 프리셋 적용"))
	void ApplyAutumnSeasonPreset();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "04 환경 및 날씨", meta = (DisplayName = "겨울 프리셋 적용"))
	void ApplyWinterSeasonPreset();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "04 환경 및 날씨", meta = (DisplayName = "계절 선택 마스크 보기"))
	void PreviewSeasonColorMask();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "04 환경 및 날씨", meta = (DisplayName = "계절 최종 결과 보기"))
	void PreviewSeasonFinalColor();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 계절 색상", DisplayName = "계절 컬러 변경 사용", ToolTip = "식생 등 선택된 원본 색상 범위를 현재 계절의 목표 색상 범위로 변환합니다."))
	bool bEnableSeasonalColor = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 날씨 프리셋", DisplayName = "프록시 표면 효과"))
	ELCCCinematicGSProxySurfaceEffectMode ProxySurfaceEffectMode = ELCCCinematicGSProxySurfaceEffectMode::Disabled;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 날씨 프리셋", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "효과 강도"))
	float ProxySurfaceEffectStrength = 0.65f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 날씨 프리셋", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "수평면 선택"))
	float ProxySurfaceHorizontalBias = 0.72f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 날씨 프리셋", ClampMin = "0.001", ClampMax = "2.0", DisplayName = "패턴 크기"))
	float ProxySurfacePatternScale = 0.12f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 날씨 프리셋", DisplayName = "효과 색상"))
	FLinearColor ProxySurfaceEffectColor = FLinearColor(0.48f, 0.64f, 0.78f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 날씨 프리셋", ClampMin = "0.0", ClampMax = "2.0", DisplayName = "반사 / 반짝임"))
	float ProxySurfaceSpecularStrength = 0.75f;

	// v2786: direct weather controls. Snow is rendered as a dedicated second GS shell;
	// stable proxy binding supplies orientation, while lighting uses the same moving Stable cast as the base GS.
	UPROPERTY( BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (ClampMin = "0.0", ClampMax = "20.0", Units = "cm", DisplayName = "원본 GS 분류 두께(내부)", EditCondition = "ProxySurfaceEffectMode == ELCCCinematicGSProxySurfaceEffectMode::Snow", EditConditionHides))
	float ProxySnowThicknessCm  = 4.0f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float ProxySnowSplatExpansion  = 1.06f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02B 눈 효과", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", DisplayName = "바닥 눈 채우기", EditCondition = "ProxySurfaceEffectMode == ELCCCinematicGSProxySurfaceEffectMode::Snow", EditConditionHides, ToolTip = "바닥의 누락된 눈을 얼마나 적극적으로 메울지 조절합니다. 값이 높을수록 바닥 fallback, bridge, selective hole fill이 강해집니다."))
	float ProxySnowGroundFillStrength  = 0.72f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02B 눈 효과", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", DisplayName = "구조물 눈 제거", EditCondition = "ProxySurfaceEffectMode == ELCCCinematicGSProxySurfaceEffectMode::Snow", EditConditionHides, ToolTip = "돌탑, 난간, 벽 같은 구조물의 경사/옆면 눈을 얼마나 강하게 제거할지 조절합니다. 0은 완화, 1은 윗면 Snow Cap 위주입니다."))
	float ProxySnowStructureRemoval  = 0.68f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02B 눈 효과", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", DisplayName = "구조물 경사 기준", EditCondition = "ProxySurfaceEffectMode == ELCCCinematicGSProxySurfaceEffectMode::Snow", EditConditionHides, ToolTip = "구조물 표면이 얼마나 위를 향해야 눈을 유지할지 조절합니다. 값이 높을수록 거의 수평인 윗면에만 눈이 남습니다."))
	float ProxySnowStructureSlopeThreshold  = 0.70f;

	/** v2831: Generated, camera-independent GS layer. This is the one retained baked snow geometry path. */
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	bool bUseSnowGaussianLayer  = false;

	// v2849: Experimental, source-surface-first snow. The original splat
	// topology is reconstructed as multiple connected height layers BEFORE
	// either the snow splats or the two-bit original-GS coverage mask is baked.
	// Disable this to compare against the untouched v2848 generator.

	// v2838: Under hybrid snow the SOURCE splats themselves receive a snow albedo
	// in CalcViewData. Their original position, covariance, opacity and view-dependent
	// SH stay untouched. No extra screen-space coating is rendered in this mode.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02B 눈 효과", DisplayName = "원본 GS 적설 색상 강도", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", ToolTip = "원본 GS가 가진 크기·회전·SH·투명도를 유지한 채, 지형 노멀 및 월드 상향 차폐 마스크에서 눈 색상으로 변환합니다. 기존 별도 화면 Snow Overlay는 중복 출력되지 않습니다. 재생성 없이 실시간 변경 가능."))
	float SnowHybridBaseCoverageStrength  = 0.78f;

	// v2841: total dense blanket uses a 60% baseline reserve PLUS this
	// fraction of the authored max count. Existing saved 0.10 => 70%.
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianReliefBudgetFraction  = 0.10f;

	// Saves the proxy-baked accumulation cutoff and sends it to the original-GS
	// shader. This is NOT an artist-facing independent mask or a transient value.
	UPROPERTY(BlueprintReadOnly, Category = "04 환경 및 날씨")
	float SnowGaussianBakedPatchThreshold  = 0.33f;

	// v2842: ACTUAL baked geometry correspondence, indexed by source splat ID.
	// Two bits/splat: 0 no baked snow, 1 fringe, 2 partial, 3 supported blanket.
	// Unlike v2841 XY noise, this also distinguishes roof from ground in Z.
	// Stored on the source component and copied to its new SceneProxy for MRQ.
	UPROPERTY()
	TArray<uint32> SnowCoveragePackedMask;

	UPROPERTY()
	TObjectPtr<UGaussianSplatAsset> SnowCoverageMaskSourceAsset = nullptr;

	UPROPERTY()
	int32 SnowCoverageMaskSourceCount = 0;

	// 2.8.73: once-per-bake source-splat accumulation pattern. Four UNORM8 samples
	// share a uint32. Unlike the 2-bit correspondence above this is continuous
	// world-locked noise, so the source view shader no longer needs 12 hash
	// evaluations per splat/frame. Serialized with the level for PIE/MRQ.
	UPROPERTY()
	TArray<uint32> SnowBakedFieldPacked2873;

	UPROPERTY()
	TObjectPtr<UGaussianSplatAsset> SnowBakedFieldSourceAsset2873 = nullptr;

	UPROPERTY()
	int32 SnowBakedFieldSplatCount2873 = 0;

	UPROPERTY()
	float SnowBakedFieldPatternCm2873 = 0.0f;

	UPROPERTY()
	FTransform SnowBakedFieldLocalToWorld2873 = FTransform::Identity;


	UPROPERTY(BlueprintReadOnly, Category = "04 환경 및 날씨")
	FString SnowBakedFieldSummary2873 ;

	bool HasCurrentSnowCoverageMask() const;
	bool HasCurrentSnowBakedField2873() const;

	/** Snow-only public hook: refresh the source SceneProxy after baking or deleting the correspondence mask. */
	void RefreshSnowCoverageMaskRenderState();

	const TArray<uint32>& GetSnowCoveragePackedMask() const { return SnowCoveragePackedMask; }
	const TArray<uint32>& GetSnowBakedFieldPacked2873() const { return SnowBakedFieldPacked2873; }
	float GetSnowBakedFieldPatternCm2873() const { return SnowBakedFieldPatternCm2873; }
	const FTransform& GetSnowBakedFieldLocalToWorld2873() const { return SnowBakedFieldLocalToWorld2873; }

	// Independent of the coverage above: extra snow Gaussians are subtle relief,
	// not a second opaque white blanket. Both parameters require regenerating snow.
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianDetailStrength  = 0.34f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianDetailScale  = 0.60f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianSpacingCm  = 26.0f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianThicknessCm  = 7.0f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianMoundVariationCm  = 2.5f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianMoundScaleCm  = 190.0f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianSlopeThreshold  = 0.55f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianSupportRadiusCm  = 65.0f;

	/** Fit snow elevation and tangent normals to several nearby original GS splats. */
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianSourceDetailStrength  = 0.88f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianSourceDetailCellCm  = 23.0f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianSourceHeightToleranceCm  = 48.0f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	bool bSnowGaussianSkyExposure  = true;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	int32 SnowGaussianMaxCount  = 250000;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	FLinearColor SnowGaussianColor  = FLinearColor(0.90f, 0.94f, 0.99f, 1.0f);

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianOpacity  = 0.82f;

	/** Baked SH1 color response to the camera direction; geometry remains world-fixed. */
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianViewResponseStrength  = 0.72f;

	// v2833: Additional, world-space feature passes. Editing requires baking snow again.
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	bool bSnowGaussianLandformFeatures  = true;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianFeatureStrength  = 0.85f;

	/** Baked snow transport is world fixed, independent from MRQ camera and daylight. */
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianWindDirectionDegrees  = 45.0f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianWindStrength  = 0.60f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	bool bSnowGaussianRailCaps  = true;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianRailStrength  = 0.70f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianLedgeRadiusCm  = 22.0f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianTipRadiusCm  = 17.0f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowGaussianVolumeStrength  = 0.65f;

	UPROPERTY(Transient)
	FString SnowGaussianBuildStatus  = TEXT("생성 전");

	UPROPERTY(Transient)
	int32 SnowGaussianGeneratedCount  = 0;




	// 2.8.74: old 2.8.56 test-only gate overrides and the retired
	// 2.8.57 strict-proxy rollback switch are removed. Production snow retains
	// the verified receiver policy; obsolete ROI export has been retired.

	// 2.8.57: source-backed ground recovery. These parameters do not relax
	// shelf/rail/crown/cliff validation or modify the original imported PLY.
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	bool bSnowFloorPreferSource  = true;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowFloorProxyHeightMultiplier  = 1.65f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowFloorProxyMinUp  = 0.63f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	int32 SnowFloorBlockerNeighborHits  = 2;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowFloorRadiusScale  = 0.80f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowFloorDepthScale  = 0.78f;


	UPROPERTY(Transient)
	FString SnowFloorBuildSummary ;

	// 2.8.72: source GS colour compensation is ONLY authorized by the existing
	// baked 2-bit correspondence. Code 0/1, original covariance, opacity and
	// physical cast shadows are never modified by this slider.
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	bool bSnowSourceCover2872  = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02B 눈 효과", DisplayName = "원본 눈 커버 강도 (실시간)", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", ToolTip = "0=2.8.71 원본색 보존, 1=실제 베이크 완전 매칭 원본면의 돌바닥/촬영 얼룩 대비를 많이 축소. 경계·미매칭 면은 보호합니다."))
	float SnowSourceCoverStrength2872  = 0.65f;

	// Restrict compact snow to REAL reconstructed cliff shelves (Kind 2).
	// Floor, roof, source PLY, cast shadow and fog keep their 2.8.71 path.
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	bool bSnowCliffCompact2872  = true;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowCliffCompactScale2872  = 0.76f;

	UPROPERTY(Transient)
	FString SnowCliffCompactSummary2872 ;

	// 2.8.62: surface-wide receiver coverage policy. When disabled, use the
	// exact v2861 parent -> 2-4 child algorithm to compare identical camera
	// and exposure settings. Both paths use the same original-GS mask and
	// world-space snow asset; this toggle only changes new snow placement.
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	bool bSnowSurfaceResampling  = true;

	// v2863: limits only the NEW fine resampling path; the 2.8.61 A/B
	// fallback and original source PLY keep their existing world covariance.
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowResampleWorldSigmaCm  = 6.5f;

	// New property ensures existing levels with a SERIALIZED 6.5cm size still
	// shrink their new snow on 2.8.68; changing the old CDO default alone would
	// not affect serialized actor overrides.
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowFineSizeFactor  = 0.80f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowResampleCoverageTarget  = 0.38f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	int32 SnowResampleRefillRounds  = 2;

	// v2864: sample inside four mutually compatible real surface receivers.
	// This is world-space sampling within verified support, NOT arbitrary
	// extrapolation through walls, roofs or unsupported scan holes.
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	bool bSnowResampleQuadRefill  = true;

	// v2866: Optics are NOT spatial coverage. A 0.34-alpha Gaussian at its
	// center must not be counted as physically missing just because the old
	// optical threshold is 0.38. Leave the old optical policy as an explicit A/B.
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	bool bSnowResampleGeometricCoverage  = true;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowResampleGeometricFootprintSigma  = 1.20f;

	// v2865: diagnostic count-to-convergence. The total SnowGaussianMaxCount
	// and its 88% surface sub-cap do NOT apply to the supported broad-surface
	// generator while ON. Geometry/support/distance tests remain unchanged.
	// The work set is finite (validated MLS receiver centers / links / quads);
	// CPU/RAM/GPU capacity may still prevent completing extremely large bakes.
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	bool bSnowResampleMeasureUncapped  = true;

	// 2.8.67: camera-independent snow-only normal source A/B. The source GS
	// MLS height, support/topology and original tint still use the 2.8.66 path.
	// Using a matched proxy does NOT require a proxy on GS-supported floors.
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	bool bSnowProxyNormalPriority  = true;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowProxyNormalBlend  = 0.85f;

	// v2868: small optical bridge layer complements spatial-only coverage.
	// Still attached to the same world-space, verified receiver + proxy normal.
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	bool bSnowCohesionBridges  = true;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowCohesionBridgeStrength  = 0.85f;

	// v2.8.69: a SECOND, separately switchable fine snow layer. Serialized
	// v2.8.68 actors retain their old size factor; this additional multiplier
	// is a new property, so existing levels receive the finer 4.16cm default.
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	bool bSnowFineLayer2869  = true;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowFineLayerScale2869  = 0.80f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowFineMicroSigma2869  = 2.60f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowFineOpticalGoal2869  = 0.52f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	int32 SnowFineOpticalPasses2869  = 2;

	UPROPERTY(Transient)
	int32 SnowFineCandidates2869  = 0;

	UPROPERTY(Transient)
	int32 SnowFinePlaced2869  = 0;

	UPROPERTY(Transient)
	int32 SnowFineUnmet2869  = 0;

	UPROPERTY(Transient)
	int32 SnowFinePasses2869  = 0;

	// 2.8.70: optical finishing adjusts opacity of existing SUPPORTED fine
	// snow splats only; it does not add splats, grow covariance, or edit source GS.
	// The switch is intentionally independent of 2.8.69's fine-layer switch.
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	bool bSnowOpticalFinish2870  = true;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowOpticalFinishStrength2870  = 0.25f;

	UPROPERTY(Transient)
	int32 SnowOpticalFinishChanged2870  = 0;

	UPROPERTY(Transient)
	int32 SnowOpticalFinishCandidateSites2870  = 0;

	UPROPERTY(Transient)
	int32 SnowOpticalFinishUnmetBefore2870  = 0;

	UPROPERTY(Transient)
	int32 SnowOpticalFinishUnmetAfter2870  = 0;

	UPROPERTY(Transient)
	int32 SnowOpticalUnmetFloor2870  = 0;

	UPROPERTY(Transient)
	int32 SnowOpticalUnmetRoof2870  = 0;

	UPROPERTY(Transient)
	int32 SnowOpticalUnmetStone2870  = 0;

	UPROPERTY(Transient)
	int32 SnowCohesionBridgeCandidates  = 0;

	UPROPERTY(Transient)
	int32 SnowCohesionBridgePlaced  = 0;

	UPROPERTY(Transient)
	int32 SnowProxyNormalApplied  = 0;

	UPROPERTY(Transient)
	int32 SnowProxyNormalSoftFloor  = 0;

	UPROPERTY(Transient)
	int32 SnowProxyNormalMissing  = 0;

	UPROPERTY(Transient)
	int32 SnowProxyNormalInvalid  = 0;

	UPROPERTY(Transient)
	int32 SnowProxyNormalSourceGateRejected  = 0;

	// v2864: read-only facts from the LAST SUCCESSFULLY COMPLETED bake, not
	// percentages of physically visible screen pixels. Always accessible in
	// the 02E Snow UI even when ROI CSV is disabled.
	UPROPERTY(Transient)
	FString SnowResampleLastBuild  = TEXT("아직 2.8.69로 눈 생성하지 않음");

	UPROPERTY(Transient)
	int32 SnowResampleReceiverCells  = 0;

	UPROPERTY(Transient)
	int32 SnowResampleGenerated  = 0;

	UPROPERTY(Transient)
	int32 SnowResampleRefillPlaced  = 0;

	UPROPERTY(Transient)
	int32 SnowResampleUnmetCenters  = 0;

	UPROPERTY(Transient)
	int32 SnowResampleUnmetFine  = 0;

	UPROPERTY(Transient)
	int32 SnowResampleOpticalUnmetCenters  = 0;

	UPROPERTY(Transient)
	int32 SnowResampleGeometricUnmetCenters  = 0;

	UPROPERTY(Transient)
	int32 SnowResampleQuadSites  = 0;

	UPROPERTY(Transient)
	int32 SnowResampleUnmetQuadSites  = 0;

	UPROPERTY(Transient)
	int32 SnowResampleUnusedSurfaceSlots  = 0;

	UPROPERTY(Transient)
	int32 SnowResampleRejectedSupport  = 0;

	UPROPERTY(Transient)
	int32 SnowResampleTotalGenerated  = 0;

	UPROPERTY(Transient)
	int32 SnowResampleRefillCandidateSites  = 0;

	UPROPERTY(Transient)
	int32 SnowResampleMeasuredRounds  = 0;

	UPROPERTY(Transient)
	int32 SnowResampleMeasuredRequiredCount  = -1;

	UPROPERTY(Transient)
	FString SnowResampleStopReason  = TEXT("눈을 재생성하면 나타납니다.");

	UPROPERTY(Transient)
	FString SnowResampleMaskStats  = TEXT("눈 재생성 후 표시 (진단 2=원본, 3=추가 눈)");

	UPROPERTY(Transient)
	FString SnowResampleDiagnosticSummary  = TEXT("눈 재생성 후 표시; 이 값은 화면 픽셀 커버리지가 아니라 월드 공간의 표본 검사 결과입니다.");

	// 2.8.61: actual world-space replacement of oversized broad-surface snow
	// Gaussians with at least two compact child grains per supported receiver.
	// Keeps the 2.8.59 large parent path only when the toggle is OFF.
	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	bool bSnowUseMicroClusters  = true;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowMicroClusterStrength  = 0.88f;

	UPROPERTY(BlueprintReadWrite, Category = "04 환경 및 날씨")
	float SnowMicroMaxSigmaCm  = 14.0f;

	UPROPERTY(Transient)
	FString SnowMicroClusterBuildSummary ;

	// Rain precipitation gate. Turning this OFF keeps WeatherMode=Rain but stops only
	// the Rain Niagara and switches the Wet pass to residual-puddle presentation.
	// No Clear/Fog/Snow/season/shadow value is modified.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", DisplayName = "비 내림 활성 (끄면 웅덩이 유지)", EditCondition = "WeatherMode == ELCCCinematicGSWeatherMode::Rain", EditConditionHides, ToolTip = "끄면 빗줄기는 멈추고 현재 Rain 상태를 유지한 채 바닥의 잔류 웅덩이만 남깁니다. 다시 켜면 일반 비+젖음+파동 상태로 돌아옵니다."))
	bool bRainPrecipitationEnabled = true;

	// Rain-only color overlay. These values are consumed exclusively by the dedicated
	// WetSurface pass while WeatherMode == Rain. They never enter Clear/Snow/Fog
	// lighting, grading, shadow or composite code paths.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "비 색감 오버레이 강도", EditCondition = "WeatherMode == ELCCCinematicGSWeatherMode::Rain", EditConditionHides))
	float RainOverlayStrength = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", ClampMin = "-2.0", ClampMax = "2.0", UIMin = "-1.0", UIMax = "1.0", DisplayName = "비 전용 노출 EV", EditCondition = "WeatherMode == ELCCCinematicGSWeatherMode::Rain", EditConditionHides))
	float RainOverlayExposureEV = -0.16f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", ClampMin = "0.50", ClampMax = "2.0", UIMin = "0.70", UIMax = "1.40", DisplayName = "비 전용 감마", EditCondition = "WeatherMode == ELCCCinematicGSWeatherMode::Rain", EditConditionHides))
	float RainOverlayGamma = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "1.5", DisplayName = "비 전용 채도", EditCondition = "WeatherMode == ELCCCinematicGSWeatherMode::Rain", EditConditionHides))
	float RainOverlaySaturation = 0.82f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", DisplayName = "비 전용 색조", EditCondition = "WeatherMode == ELCCCinematicGSWeatherMode::Rain", EditConditionHides))
	FLinearColor RainOverlayTint = FLinearColor(0.72f, 0.80f, 0.92f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "비 전용 색조 강도", EditCondition = "WeatherMode == ELCCCinematicGSWeatherMode::Rain", EditConditionHides))
	float RainOverlayTintStrength = 0.18f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "젖음 어두워짐", EditCondition = "ProxySurfaceEffectMode == ELCCCinematicGSProxySurfaceEffectMode::WetPuddles", EditConditionHides))
	float ProxyWetDarkening = 0.28f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "물웅덩이 양", EditCondition = "ProxySurfaceEffectMode == ELCCCinematicGSProxySurfaceEffectMode::WetPuddles", EditConditionHides))
	float ProxyPuddleAmount = 0.55f;

	// Rain water-optics oversized-splat gate. Property names are retained for asset
	// compatibility, but v32 uses these x/y values as camera-independent intrinsic
	// splat-size thresholds instead of the retired proxy-distance gate.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", ClampMin = "0.0", ClampMax = "500.0", UIMin = "0.0", UIMax = "250.0", Units = "cm", DisplayName = "물 효과를 받을 스플렛의 최소 크기", EditCondition = "ProxySurfaceEffectMode == ELCCCinematicGSProxySurfaceEffectMode::WetPuddles", EditConditionHides, ToolTip = "최소 크기 이상, 최대 크기 이하인 splat만 젖음/웅덩이/파동/반사 효과를 받습니다. 범위 밖은 페이드 없이 완전히 제외합니다. 카메라 거리/FOV와 무관합니다."))
	float ProxyWetGroundFullDistanceCm = 80.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", DisplayName = "젖음 위치 테스트 모드", ToolTip = "기존 방식은 최소/최대 스플렛 크기를 사용합니다. 프록시 테스트는 크기를 무시하고 프록시 캡처 표면을 사용합니다. 마스크는 흰색=허용, 검정=제외입니다. 별도 젖음 메시 생성 기능은 아닙니다."))
	EGSWetReceiverTestMode WetReceiverTestMode = EGSWetReceiverTestMode::SplatSize;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", DisplayName = "젖음 메시 최대 경사", ClampMin = "0", ClampMax = "80", Units = "deg"))
	float WetMeshMaxSlopeDegrees = 50.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", DisplayName = "젖음 메시 표면 간격", ClampMin = "0", ClampMax = "10", Units = "cm"))
	float WetMeshSurfaceOffsetCm = 0.5f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", DisplayName = "젖음 메시 물결 미리보기", ToolTip = "실제 젖음 메시 모드에서 비 내림을 꺼도 물결을 재생합니다. 테스트 후 끄면 비가 멎을 때 잔류 웅덩이만 유지합니다."))
	bool bWetMeshPreviewRipples = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", DisplayName = "젖음 메시 위쪽 비 차폐 검사"))
	bool bWetMeshCheckRainExposure = true;
	UPROPERTY(VisibleAnywhere, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", DisplayName = "젖음 메시 생성 결과"))
	FString WetMeshBuildStatus = TEXT("젖음 메시 생성 버튼을 누르세요");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", ClampMin = "0.0", ClampMax = "800.0", UIMin = "0.0", UIMax = "400.0", Units = "cm", DisplayName = "물 효과를 받을 스플렛의 최대 크기", EditCondition = "ProxySurfaceEffectMode == ELCCCinematicGSProxySurfaceEffectMode::WetPuddles", EditConditionHides, ToolTip = "이 월드 크기를 초과하는 splat은 젖음/웅덩이/파동/반사 효과에서 완전히 제외합니다. 최대값이 최소값보다 작으면 모든 크기를 제외합니다."))
	float ProxyWetGroundMaxDistanceCm = 140.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", DisplayName = "웅덩이 지면 마스크 보기", EditCondition = "ProxySurfaceEffectMode == ELCCCinematicGSProxySurfaceEffectMode::WetPuddles", EditConditionHides, ToolTip = "켜면 최종 지면 선택 마스크를 흑백으로 표시합니다. 흰색=적용, 회색=거리 페이드, 검정=차단. 실제 Wet 합성은 잠시 대신 표시됩니다."))
	bool bPreviewWetGroundMask = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", ClampMin = "0.0", ClampMax = "2.0", DisplayName = "물 파동 강도", EditCondition = "ProxySurfaceEffectMode == ELCCCinematicGSProxySurfaceEffectMode::WetPuddles", EditConditionHides))
	float ProxyRainRippleStrength = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", ClampMin = "5.0", ClampMax = "200.0", Units = "cm", DisplayName = "물 파동 크기", EditCondition = "ProxySurfaceEffectMode == ELCCCinematicGSProxySurfaceEffectMode::WetPuddles", EditConditionHides))
	float ProxyRainRippleSizeCm = 45.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", ClampMin = "10.0", ClampMax = "500.0", Units = "cm", DisplayName = "물 파동 간격", EditCondition = "ProxySurfaceEffectMode == ELCCCinematicGSProxySurfaceEffectMode::WetPuddles", EditConditionHides))
	float ProxyRainRippleSpacingCm = 85.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02A 비 효과", ClampMin = "0.0", ClampMax = "5.0", DisplayName = "물 파동 속도", EditCondition = "ProxySurfaceEffectMode == ELCCCinematicGSProxySurfaceEffectMode::WetPuddles", EditConditionHides))
	float ProxyRainRippleSpeed = 0.62f;

	/** Optional custom materials, mapped to the proxy mesh material slots in array order. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 날씨 프리셋", DisplayName = "3DGS 표면 오버레이 머티리얼 슬롯", TitleProperty = "Material"))
	TArray<FLCCCinematicGSProxySurfaceLayer> ProxySurfaceLayers;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 계절 색상", DisplayName = "선택 원본 색상 시작", EditCondition = "bEnableSeasonalColor", ToolTip = "계절 변환 대상으로 선택할 원본 색상 범위의 시작 색입니다."))
	FLinearColor SeasonSourceColorFrom = FLinearColor(0.008f, 0.022f, 0.004f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 계절 색상", DisplayName = "선택 원본 색상 끝", EditCondition = "bEnableSeasonalColor", ToolTip = "계절 변환 대상으로 선택할 원본 색상 범위의 끝 색입니다."))
	FLinearColor SeasonSourceColorTo = FLinearColor(0.66f, 0.82f, 0.24f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 계절 색상", DisplayName = "변환 목표 색상 시작", EditCondition = "bEnableSeasonalColor", ToolTip = "선택된 원본 색상 범위를 매핑할 목표 색상의 시작 색입니다."))
	FLinearColor SeasonTargetColorFrom = FLinearColor(0.45f, 0.08f, 0.015f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 계절 색상", DisplayName = "변환 목표 색상 끝", EditCondition = "bEnableSeasonalColor", ToolTip = "선택된 원본 색상 범위를 매핑할 목표 색상의 끝 색입니다."))
	FLinearColor SeasonTargetColorTo = FLinearColor(1.00f, 0.66f, 0.04f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 계절 색상", ClampMin = "0.001", ClampMax = "1.0", UIMin = "0.001", UIMax = "0.40", DisplayName = "색상 선택 허용 오차", EditCondition = "bEnableSeasonalColor", ToolTip = "원본 색상 범위에서 얼마나 떨어진 색까지 선택할지 조절합니다. 셰이더의 실효 범위는 최대 0.40입니다."))
	float SeasonColorTolerance = 0.22f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 계절 색상", ClampMin = "0.001", ClampMax = "1.0", UIMin = "0.001", UIMax = "0.11", DisplayName = "선택 경계 부드러움", EditCondition = "bEnableSeasonalColor", ToolTip = "선택 영역의 경계를 부드럽게 섞습니다. 셰이더의 실효 범위는 최대 0.11입니다."))
	float SeasonColorFeather = 0.09f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 계절 색상", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.14", UIMax = "1.0", DisplayName = "최소 채도", EditCondition = "bEnableSeasonalColor", ToolTip = "채도가 낮은 벽이나 중성 표면이 식생으로 잘못 선택되는 것을 막습니다. 셰이더의 실효 최솟값은 0.14입니다."))
	float SeasonMinSaturation = 0.16f;

	/** Relative green evidence: (G - max(R,B)) / max(R,G,B). Keeps neutral/brown architecture out while retaining dark green grass. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 계절 색상", ClampMin = "-0.25", ClampMax = "0.5", UIMin = "0.02", UIMax = "0.5", DisplayName = "최소 녹색 우세도", EditCondition = "bEnableSeasonalColor", ToolTip = "녹색 성분이 다른 색보다 얼마나 우세해야 선택할지 정합니다. 셰이더의 실효 최솟값은 0.02입니다."))
	float SeasonMinGreenDominance = 0.025f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 계절 색상", ClampMin = "0.0", ClampMax = "2.5", UIMin = "0.0", UIMax = "1.08", DisplayName = "변환 채도", EditCondition = "bEnableSeasonalColor", ToolTip = "변환된 계절 색상의 채도를 조절합니다. 셰이더의 실효 최댓값은 1.08입니다."))
	float SeasonSaturationScale = 1.26f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 계절 색상", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "1.04", DisplayName = "변환 밝기", EditCondition = "bEnableSeasonalColor", ToolTip = "변환된 계절 색상의 밝기를 조절합니다. 셰이더의 실효 최댓값은 1.04입니다."))
	float SeasonBrightnessScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 계절 색상", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.76", DisplayName = "색상 교체 강도", EditCondition = "bEnableSeasonalColor", ToolTip = "원본 색상과 목표 색상을 섞는 강도입니다. 셰이더의 실효 최댓값은 0.76입니다."))
	float SeasonReplacementStrength = 0.88f;

	/** Restores fine color variation around the mapped range so foliage does not collapse into a flat tint. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 계절 색상", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "원본 디테일 보존", EditCondition = "bEnableSeasonalColor", ToolTip = "잎과 잔디의 원래 미세 색 변화가 단색으로 뭉개지지 않도록 보존합니다."))
	float SeasonOriginalDetailPreservation = 0.32f;

	/** Curves the source-range position toward the target start colour, retaining more red/orange in autumn foliage. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 계절 색상", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "목표 시작색 편향", EditCondition = "bEnableSeasonalColor", ToolTip = "목표 범위의 시작 색 쪽 비중을 높입니다. 가을의 붉은색과 주황색 분포를 조절할 때 사용합니다."))
	float SeasonTargetStartColorBias = 0.50f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "02 계절 색상", DisplayName = "선택 마스크 미리보기", EditCondition = "bEnableSeasonalColor", ToolTip = "최종 색상 대신 현재 계절 컬러 변환에 선택된 영역을 흑백으로 표시합니다."))
	bool bPreviewSeasonColorMask = false;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "01 기본 설정", meta = (DisplayName = "효과 적용"))
	void EnableCinematicEffects();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "01 기본 설정", meta = (DisplayName = "효과 적용 안 함"))
	void DisableCinematicEffects();

	/** Switch to the original 16-byte absolute-FP16 position path for direct A/B comparison. */
	UFUNCTION(BlueprintCallable, Category = "07 진단 및 고급")
	void PreviewLegacyPackedPositions();

	/** Switch to the new full Float32 runtime position buffer. */
	UFUNCTION(BlueprintCallable, Category = "07 진단 및 고급")
	void PreviewFloat32Positions();

	// Compatibility helpers retained for old Blueprint bytecode; no Details-panel buttons.
	void PreviewLegacyPackedColorOpacity();
	void PreviewFloat16ColorOpacity();
	void PreviewLegacyProjectedFootprint();
	void PreviewAdaptiveEWAFootprint();


	/** Find a StaticMesh imported from OBJ/FBX with a name close to this splat asset and assign it as the shadow catcher proxy. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "05 프록시 및 Stable")
	void AutoAssignProxyShadowMesh();

	/** Enable the assigned proxy mesh for the upcoming mesh/GS shadow-catcher pass. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "05 프록시 및 Stable", meta = (GSGroup = "01 프록시 설정", DisplayName = "프록시 그림자 사용"))
	bool bUseProxyMeshForShadowCatcher = false;

	/** Imported OBJ/FBX StaticMesh used as a geometric proxy for mesh-to-GS shadow catching. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "05 프록시 및 Stable", meta = (GSGroup = "01 프록시 설정", DisplayName = "그림자 수신 프록시 메시"))
	TObjectPtr<UStaticMesh> ProxyShadowMesh = nullptr;

	/** Phase-1 binding status. Camera motion and debug-mode changes must never modify these values. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "02 Stable Binding", DisplayName = "Binding State"))
	EStableProxyBindingState StableBindingState = EStableProxyBindingState::Uninitialized;

	UPROPERTY(VisibleAnywhere, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "02 Stable Binding", DisplayName = "Binding Revision"))
	uint32 StableBindingRevision = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "02 Stable Binding", DisplayName = "Binding Hash"))
	uint64 StableBindingHash = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "02 Stable Binding", DisplayName = "Rebuild Count"))
	uint32 StableBindingRebuildCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "02 Stable Binding", DisplayName = "Total Splats"))
	int32 StableBindingInputCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "02 Stable Binding", DisplayName = "Processed Splats"))
	int32 StableBindingProcessedCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "02 Stable Binding", DisplayName = "Unprocessed Splats"))
	int32 StableBindingUnprocessedCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "02 Stable Binding", DisplayName = "Valid Bindings"))
	int32 StableBindingValidCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "02 Stable Binding", DisplayName = "Invalid Bindings"))
	int32 StableBindingInvalidCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "02 Stable Binding", DisplayName = "Unique Triangle IDs"))
	int32 StableBindingUniqueTriangleCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "02 Stable Binding", DisplayName = "Build Progress (%)"))
	float StableBindingProgressPercent = 0.0f;

	UPROPERTY(VisibleAnywhere, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "02 Stable Binding", DisplayName = "Persistent Cache"))
	FString StableBindingCacheStatus = TEXT("Not Checked");

	/** Number of splats processed per actor tick. The final binding always covers the complete asset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "05 프록시 및 Stable", meta = (GSGroup = "02 Stable Binding", ClampMin = "5000", ClampMax = "250000", UIMin = "10000", UIMax = "100000", DisplayName = "Binding Chunk Splats"))
	int32 StableBindingChunkSize = 50000;

	/** Bindings farther than this from the proxy surface are marked invalid/fail-open. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "05 프록시 및 Stable", meta = (GSGroup = "02 Stable Binding", ClampMin = "1.0", ClampMax = "2000.0", UIMin = "10.0", UIMax = "500.0", DisplayName = "Max Proxy Distance (cm)"))
	float StableBindingMaxDistanceCm = 250.0f;

	/** v2745 internal local closure radius. The artist-facing area control is the existing
	 *  "폐쇄 그림자 영역 크기" parameter below; the broad field is derived automatically. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "05 프록시 및 Stable", meta = (GSGroup = "03 Stable Field", ClampMin = "25.0", ClampMax = "1000.0", UIMin = "100.0", UIMax = "500.0", DisplayName = "Stable Closure Base Radius (cm)"))
	float StableClosureRadiusCm = 260.0f;

	/** Compatibility values retained so older assets deserialize cleanly. v2745 no longer exposes
	 *  duplicate Stable Lighting controls; the existing Shadow UI below is the authority. */
	UPROPERTY()
	float StableCastShadowStrength = 1.25f;
	UPROPERTY()
	float StableCastShadowGamma = 1.45f;
	UPROPERTY()
	float StableClosureStrength = 1.35f;
	UPROPERTY()
	float StableClosurePower = 1.75f;

	/** Compatibility-only old split bounce floor; v2748 derives bounce floor from StableClosedSkyFloor. */
	UPROPERTY()
	float StableClosedBounceFloor = 0.58f;

	/** Phase-2 request counter. Camera movement and debug changes must never increment it. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "03 Stable Field", DisplayName = "Field Request Revision"))
	uint32 StableFieldRequestRevision = 0;

	// v2772 packet expected by the latest request. 0 preserves manual/legacy rebuild behavior.
	uint32 StableFieldExpectedLightDepthRevision = 0;
	float StableFieldLastRequestedDepthBiasCm = TNumericLimits<float>::Lowest();

	UPROPERTY(VisibleAnywhere, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "03 Stable Field", DisplayName = "Field Rebuild Count"))
	uint32 StableFieldRebuildCount = 0;

	/** Retired v2764 serialization compatibility. v2778 Final always uses the live Stable Field. */
	UPROPERTY()
	bool bOneShotCastShadowLatch = false;

	/** True after the dedicated one-shot LightDepth target has received a complete capture. */
	UPROPERTY(Transient)
	bool bCastShadowSnapshotLatched = false;

	/** Number of complete one-shot LightDepth snapshots captured during this actor lifetime. */
	UPROPERTY(Transient)
	int32 CastShadowSnapshotCaptureCount = 0;

	/** Light/caster/receiver changes intentionally ignored after the one-shot snapshot is latched. */
	UPROPERTY(Transient)
	int32 CastShadowSnapshotIgnoredChanges = 0;

	/** Retired v2762 serialization compatibility. v2778 Final never freezes light updates. */
	UPROPERTY()
	bool bFreezeStableShadowField = false;

	/** Counts live light/caster changes intentionally ignored while the legacy stable snapshot is frozen. */
	UPROPERTY(Transient)
	uint32 StableFieldIgnoredLightChanges = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "03 Stable Field", DisplayName = "Welded Proxy Vertices"))
	int32 StableFieldVertexCount = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "03 Stable Field", DisplayName = "Proxy Triangles"))
	int32 StableFieldTriangleCount = 0;

	/** Shared receiver-depth bias for both the validated 1x1 reference and the 3x3 PCF field. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "05 프록시 및 Stable", meta = (GSGroup = "03 Stable Field", ClampMin = "0.0", ClampMax = "100.0", UIMin = "0.0", UIMax = "30.0", DisplayName = "Stable Depth Bias (cm)"))
	float StableFieldDepthBiasCm = 0.0f;

	/** Deterministic one-ring filtering applied only to the BVH cast boundary.
	 *  Zero preserves the hard ray result; one replaces it with the adjacent-face mean. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "05 프록시 및 Stable",
		meta = (GSGroup = "03 Stable Field", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0",
			DisplayName = "BVH 캐스트 경계 부드러움",
			ToolTip = "BVH 캐스트 그림자의 공간 경계만 부드럽게 합니다. 0은 날카로운 원본 광선 결과, 1은 인접 프록시 면 평균입니다. 시간 누적이나 카메라 정보는 사용하지 않습니다."))
	float StableRayBoundarySoftening = 0.40f;

	/** Time, in seconds, for the deterministic BVH cast field to approach a changed
	 *  light direction. The accumulator is frame-rate independent and is shared by
	 *  the main splats and the snow shell. Zero disables temporal filtering. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "05 프록시 및 Stable",
		meta = (GSGroup = "03 Stable Field", ClampMin = "0.0", ClampMax = "0.5", UIMin = "0.0", UIMax = "0.25",
			DisplayName = "Stable 캐스트 시간 반응 (초)",
			ToolTip = "BVH 캐스트 경계가 새 라이트 방향으로 이동하는 시간입니다. 0은 시간 누적을 끕니다. 기본 0.10초는 30fps에서 약 3~4프레임입니다."))
	float StableCastTemporalResponseTime = 0.10f;

	/** Use two pre-baked angular BVH fields surrounding the current light direction and interpolate them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "05 프록시 및 Stable",
		meta = (GSGroup = "03 Stable Field", DisplayName = "각도 캐시 보간 사용"))
	bool bUseStableCastAngleCache = true;

	/** Angular spacing of pre-baked fields. Smaller values are smoother and require more bake time/storage. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "05 프록시 및 Stable",
		meta = (GSGroup = "03 Stable Field", ClampMin = "0.10", ClampMax = "5.0", UIMin = "0.10", UIMax = "2.0",
			DisplayName = "각도 캐시 간격 (도)"))
	float StableCastAngleCacheStepDegrees = 0.50f;

	/** Elevation used by the full-yaw bake, expressed in proxy-local direction degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "05 프록시 및 Stable",
		meta = (GSGroup = "03 Stable Field", ClampMin = "-89.0", ClampMax = "89.0", DisplayName = "베이크 고도각 (도)"))
	float StableCastBakeElevationDegrees = 0.0f;

	UPROPERTY(VisibleAnywhere, Transient, Category = "05 프록시 및 Stable",
		meta = (GSGroup = "03 Stable Field", DisplayName = "각도 캐시 베이크 상태"))
	FString StableCastAngleCacheStatus = TEXT("Not baked");

	/** Optional material for the proxy shadow receiver mesh. Use a transparent/shadow-catcher material if you do not want the proxy color to cover the splats. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "05 프록시 및 Stable", meta = (GSGroup = "01 프록시 설정", DisplayName = "프록시 수신 머티리얼"))
	TObjectPtr<UMaterialInterface> ProxyShadowReceiverMaterial = nullptr;

	/** World actors whose primitive components cast shadows onto the proxy capture. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "05 프록시 및 Stable", meta = (GSGroup = "01 프록시 설정", DisplayName = "외부 그림자 캐스터"))
	TArray<TObjectPtr<AActor>> ExternalShadowCasters;

	/** Scale applied to proxy shadow capture resolution before the maximum-dimension cap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "06 렌더링 및 성능", meta = (ClampMin = "0.25", ClampMax = "1.0"))
	float ProxyCaptureResolutionScale = 1.0f;

	/** Maximum width or height of proxy shadow capture targets. Limits 4K editor/cinematic capture VRAM growth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "06 렌더링 및 성능", meta = (ClampMin = "512", ClampMax = "4096"))
	int32 ProxyCaptureMaxDimension = 1600;

	/** Shapes the 0-1 mask response. Below 1 lifts midtones; above 1 deepens them. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "02 캐스트 및 폐쇄 그림자", ClampMin = "0.1", ClampMax = "4.0", DisplayName = "그림자 농도 / 감마"))
	#endif
	// v2748 compatibility-only. Cast strength + gamma are unified into CapturedShadowStrength.
	UPROPERTY()
	float CapturedShadowGamma = 0.85f;

	/** Minimum light retained in fully shadowed pixels, approximating indirect bounce. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "02 캐스트 및 폐쇄 그림자", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "직접광 그림자 최소 밝기"))
	#endif
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "02 캐스트 및 폐쇄 그림자", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "Stable 캐스트 최소 밝기"))
	float CapturedShadowMinimum = 0.08f;

	/** Overall contribution of the captured mask to the new direct-light layer. Zero disables it; one applies it fully. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "02 캐스트 및 폐쇄 그림자", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "프록시 그림자 강도"))
	#endif
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "02 캐스트 및 폐쇄 그림자", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "2.0", DisplayName = "Stable 캐스트 그림자 농도",
			ToolTip = "0은 OFF, 1은 원본 Stable 3x3 캐스트 응답, 1보다 크면 약한 blocker coverage도 연속적으로 강화합니다. step/임계값은 사용하지 않습니다."))
	float CapturedShadowStrength = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "02 캐스트 및 폐쇄 그림자", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", DisplayName = "Stable 캐스트 연속성",
			ToolTip = "0이면 농도 곡선을 더 강하게 사용하고, 1이면 원본 Stable 3x3 PCF 연속값을 최대한 보존합니다. 이 값은 캐스트를 이진화하지 않습니다."))
	float CapturedShadowSoftness = 0.85f;

	/** Compatibility value for the disabled light-depth reinforcement path. Hidden to avoid a duplicate no-op shadow control. */
	UPROPERTY()
	float ProxyDepthShadowStrength = 1.0f;

	/** Geometry-closure darkness only. Spatial support is controlled independently by Area. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "02 캐스트 및 폐쇄 그림자", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "2.0",
			DisplayName = "폐쇄 그림자 농도", ToolTip = "폐쇄 영역 안의 어두움만 조절합니다. 0은 OFF, 1은 기본 응답, 2는 같은 Geometry 영역 안에서 더 진하게 만듭니다. 영역 자체를 넓히지 않으며 전체 화면을 임계값으로 검게 만들지 않습니다."))
	float ProxyLightingMatchStrength = 1.0f;

	// v2748 compatibility-only; folded into ProxyLightingMatchStrength.
	UPROPERTY()
	float CapturedClosedShadowMaxDarkness = 1.0f;

	// v2748 compatibility-only receiver-light heuristic; stable geometry closure no longer reads it.
	UPROPERTY()
	float CapturedClosedShadowFullBlackEnd = 0.22f;

	// v2748 compatibility-only receiver-light heuristic; stable geometry closure no longer reads it.
	UPROPERTY()
	float CapturedClosedShadowStart = 0.65f;

	/** Expands closure only from local proxy geometry toward the precomputed broad geometry field. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "02 캐스트 및 폐쇄 그림자", ClampMin = "0.0", ClampMax = "2.5", UIMin = "0.0", UIMax = "2.0",
			DisplayName = "폐쇄 그림자 영역 크기", ToolTip = "0이면 OFF, 1은 Proxy Geometry의 Local 폐쇄 코어, 1보다 크면 Broad Geometry 폐쇄 필드까지만 연속 확장됩니다. Cast Shadow나 반사광 영역을 폐쇄의 씨앗으로 사용하지 않습니다."))
	float CapturedClosedShadowAreaScale = 1.0f;

	/** Widens or narrows the transition of the expanded top-layer closure mask. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "02 캐스트 및 폐쇄 그림자", ClampMin = "0.25", ClampMax = "3.0", UIMin = "0.50", UIMax = "2.0",
			DisplayName = "폐쇄 그림자 부드러움", ToolTip = "Local 폐쇄에서 Broad Geometry 폐쇄로 넘어가는 전환 곡선만 조절합니다. 농도/최소 밝기에는 영향을 주지 않고 Geometry 밖에 새 폐쇄 영역을 만들지 않습니다."))
	float CapturedClosedShadowEdgeSoftness = 1.0f;

	/** Absolute minimum final brightness retained in closed regions, independent of Density. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "02 캐스트 및 폐쇄 그림자", ClampMin = "0.00", ClampMax = "1.00", UIMin = "0.00", UIMax = "0.80", DisplayName = "폐쇄 그림자 최소 밝기",
			ToolTip = "폐쇄 영역에 남길 절대 최소 밝기입니다. 농도와 무관하게 이 값 아래로 내려가지 않습니다. 완전 검정이 필요할 때만 0으로 설정합니다."))
	float StableClosedSkyFloor = 0.42f;

	/** HDR white point used to compress the proxy's direct-light capture before it becomes a 0-1 receiver mask. */
	// v2748 compatibility-only old camera-capture receiver response.
	UPROPERTY()
	float CapturedDirectLightWhitePoint = 0.25f;

	/** Gamma applied after HDR compression. Below one lifts the transition; above one makes it steeper. */
	// v2748 compatibility-only old camera-capture receiver response.
	UPROPERTY()
	float CapturedDirectLightResponseGamma = 0.85f;

	/** Deepens only near-contact shadow regions without darkening distant penumbrae. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "02 캐스트 및 폐쇄 그림자", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "접촉 그림자 강도"))
	#endif
	// v2748 compatibility-only; folded into ProxyLightingMatchStrength.
	UPROPERTY()
	float CapturedOcclusionShadowBoost = 1.35f;

	/** Deprecated screen-space average setting. Retained only for asset compatibility. */
	UPROPERTY()
	float CapturedShadowDistanceFade = 0.35f;

	/** Deprecated screen-space average setting. Retained only for asset compatibility. */
	UPROPERTY()
	float CapturedShadowDistanceSoftness = 12.0f;

	/** Reduces shadow strength as blocker-to-receiver distance increases. Zero keeps full strength; one fades distant shadows toward fully lit. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "02 캐스트 및 폐쇄 그림자", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "거리별 강도 감소"))
	#endif
	UPROPERTY()
	float CapturedShadowDistanceLightening = 0.30f;

	/** Blocker-to-receiver distance in centimeters that reaches the full distance effect. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "02 캐스트 및 폐쇄 그림자", ClampMin = "1.0", ClampMax = "10000.0", DisplayName = "그림자 거리 범위 (cm)"))
	#endif
	UPROPERTY()
	float CapturedShadowDistanceRange = 1000.0f;

	/** Additional screen-space softness at the full blocker distance. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "02 캐스트 및 폐쇄 그림자", ClampMin = "0.0", ClampMax = "4.0", DisplayName = "거리별 부드러움"))
	#endif
	UPROPERTY()
	float CapturedShadowPhysicalSoftness = 1.5f;

	/** Approximate triangle raster spacing for proxy mesh shadow samples, in Unreal centimeters. Lower values fill proxy shadows better but cost more. */
	UPROPERTY()
	float ProxyShadowSampleSpacing = 45.0f;

	/** Maximum proxy mesh surface samples uploaded for the custom GS shadow map. */
	UPROPERTY()
	int32 MaxProxyShadowSamples = 300000;

	/** Editor-only visualization of the exact LOD0 surface used for Stable binding. */
	UPROPERTY(EditAnywhere, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "01 프록시 설정", DisplayName = "콜라이더 표시 (정렬 확인)", ToolTip = "선택한 액터의 그림자 수신 프록시 메시를 와이어로 표시합니다. 아래 오프셋을 조절해 3DGS와 겹치세요. 물리 충돌 단순화 메시가 아닌 실제 Stable 바인딩용 LOD0 표면입니다."))
	bool bShowProxyAlignment = false;

	UPROPERTY(EditAnywhere, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "01 프록시 설정", DisplayName = "콜라이더 표시 색상", EditCondition = "bShowProxyAlignment"))
	FLinearColor ProxyAlignmentColor = FLinearColor(0.0f, 1.0f, 0.25f);

	UPROPERTY(EditAnywhere, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "01 프록시 설정", DisplayName = "콜라이더 항상 위에 표시", EditCondition = "bShowProxyAlignment", ToolTip = "가려진 프록시 선도 보이게 합니다. 정렬 후 끄면 앞뒤 관계를 확인할 수 있습니다."))
	bool bProxyAlignmentForeground = true;

	/** Local position offset used to align the proxy OBJ/FBX mesh to the Gaussian splat scan. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "05 프록시 및 Stable", meta = (GSGroup = "01 프록시 설정", DisplayName = "위치 오프셋"))
	FVector ProxyShadowMeshLocationOffset = FVector::ZeroVector;

	/** Local rotation offset used to align the proxy OBJ/FBX mesh axis to the Gaussian splat scan. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "05 프록시 및 Stable", meta = (GSGroup = "01 프록시 설정", DisplayName = "회전 오프셋"))
	FRotator ProxyShadowMeshRotationOffset = FRotator::ZeroRotator;

	/** Local scale multiplier used to align the proxy OBJ/FBX mesh size to the Gaussian splat scan. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "05 프록시 및 Stable", meta = (GSGroup = "01 프록시 설정", ClampMin = "0.0001", DisplayName = "스케일 오프셋"))
	FVector ProxyShadowMeshScaleOffset = FVector::OneVector;

	/** Reset proxy mesh alignment values to identity. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "05 프록시 및 Stable")
	void ResetProxyShadowMeshAlignment();

	/** Automatically collect Directional, Point, Spot, and Rect lights from the current world. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", DisplayName = "씬 라이트 자동 수집"))
	bool bAutoCollectSceneLights = true;

	/** Maximum number of Unreal lights sent to the splat shader. Two internal slots are reserved for cinematic atmosphere controls. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", ClampMin = "1", ClampMax = "6", DisplayName = "최대 언리얼 라이트 수"))
	int32 MaxUnrealLights = 6;

	/** Local-light search radius around this splat actor. Directional lights are always included. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", ClampMin = "0.0", DisplayName = "라이트 검색 반경"))
	float LightSearchRadius = 100000.0f;

	/** Secondary multiplier for all collected Unreal lights. Values above one are intentionally supported. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", ClampMin = "0.0", ClampMax = "4.0", DisplayName = "전체 라이트 영향 (고급)"))
	float UnrealLightInfluence = 0.72f;

	/** Normalized scale applied to Unreal light intensity before it reaches the splat shader. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", ClampMin = "0.0", ClampMax = "2.0", DisplayName = "씬 라이트 입력 배율 (고급)"))
	float UnrealLightIntensityScale = 0.18f;

	/** Primary Stable-proxy direct-sun control. Uses the same proxy normal, cast visibility and closure as Final shadowing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", DisplayName = "Stable 실시간 태양광 강도", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "2.0", ToolTip = "현재 Directional Light 강도/색상과 Stable Proxy Normal/캐스트 그림자를 함께 사용합니다."))
	float TargetDirectScale = 0.42f;

	/** Warm tint applied only to the newly generated direct-light contribution. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", DisplayName = "Stable 태양광 보조 색상"))
	FLinearColor TargetSunTint = FLinearColor(1.06f, 0.93f, 0.74f, 1.0f);

	/** Automatically replace the old below-horizon blackout with a moon/night transition. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03A 시간대 · 태양/달", DisplayName = "자동 일몰/달 전환"))
	bool bEnableAutomaticDayNightCycle = true;

	/** Unified continuous solar-lighting model. When enabled, one analytic solar-elevation function drives sun energy, sky energy, carrier brightness, color, gamma and saturation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03A 시간대 · 태양/달", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "통합 물리 시간광"))
	bool bUseUnifiedPhysicalTimeOfDay = true;

	/** Maximum direct-sun energy relative to the authored Directional Light. Keeps noon from over-lighting baked 3DGS while leaving the authored value as the ceiling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && bUseUnifiedPhysicalTimeOfDay", ClampMin = "0.1", ClampMax = "1.5", UIMin = "0.3", UIMax = "1.0", DisplayName = "통합 시간광 - 정오 태양 최대"))
	float UnifiedTODPeakSunScale = 0.60f;

	/** Maximum brightness retained from the photographed 3DGS carrier at high noon. Lower values prevent baked daylight from looking white even before relighting is added. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && bUseUnifiedPhysicalTimeOfDay", ClampMin = "0.4", ClampMax = "1.2", UIMin = "0.6", UIMax = "1.0", DisplayName = "통합 시간광 - 낮 원본 최대 밝기"))
	float UnifiedTODDayCarrierPeak = 0.84f;

	/** Smooth artistic lift applied only from solar 12:00 to 16:00. 0 keeps the analytic curve unchanged; higher values raise afternoon direct/midtone energy while highlight protection rises with the same curve. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && bUseUnifiedPhysicalTimeOfDay", ClampMin = "0.0", ClampMax = "0.8", UIMin = "0.0", UIMax = "0.6", DisplayName = "통합 시간광 - 오후 12~16시 밝기 상승"))
	float UnifiedTODAfternoonBrightnessBoost = 0.42f;

	/** Shape of the solar-elevation response. Larger values keep morning/evening dimmer for longer without creating time-band thresholds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && bUseUnifiedPhysicalTimeOfDay", ClampMin = "0.5", ClampMax = "3.0", UIMin = "0.8", UIMax = "2.2", DisplayName = "통합 시간광 - 태양 고도 곡선"))
	float UnifiedTODSolarResponseExponent = 1.35f;

	/** Atmospheric extinction applied through relative optical air mass. Larger values reduce low-angle morning/evening sunlight more strongly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && bUseUnifiedPhysicalTimeOfDay", ClampMin = "0.0", ClampMax = "0.8", UIMin = "0.0", UIMax = "0.4", DisplayName = "통합 시간광 - 대기 감쇠"))
	float UnifiedTODAtmosphericExtinction = 0.18f;

	/** Residual sky illumination at the darkest point of the continuous curve. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && bUseUnifiedPhysicalTimeOfDay", ClampMin = "0.0", ClampMax = "0.20", UIMin = "0.0", UIMax = "0.10", DisplayName = "통합 시간광 - 밤 하늘 최소광"))
	float UnifiedTODNightSkyFloor = 0.025f;

	/** Lowest carrier brightness reached continuously as the Sun approaches its deepest night position. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && bUseUnifiedPhysicalTimeOfDay", ClampMin = "0.20", ClampMax = "0.90", UIMin = "0.35", UIMax = "0.75", DisplayName = "통합 시간광 - 밤 최소 밝기"))
	float UnifiedTODNightBrightness = 0.50f;

	/** Strength of continuous warm-horizon / blue-hour / deep-night chroma changes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && bUseUnifiedPhysicalTimeOfDay", ClampMin = "0.0", ClampMax = "1.5", UIMin = "0.0", UIMax = "1.2", DisplayName = "통합 시간광 - 색 변화 강도"))
	float UnifiedTODColorStrength = 0.88f;

	/** Strength of the continuous gamma response from daylight to deep night. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && bUseUnifiedPhysicalTimeOfDay", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "1.5", DisplayName = "통합 시간광 - 감마 변화 강도"))
	float UnifiedTODGammaStrength = 0.85f;

	/** Sun surface-to-light Z where the broad golden-hour/twilight transition begins. +0.26 is roughly 15 degrees above the horizon. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && !bUseUnifiedPhysicalTimeOfDay", ClampMin = "-0.5", ClampMax = "0.6", UIMin = "0.0", UIMax = "0.45", DisplayName = "황혼/새벽 전환 시작"))
	float DayNightTransitionStart = 0.26f;

	/** Sun surface-to-light Z where astronomical twilight has finished and full night grading is allowed to settle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && !bUseUnifiedPhysicalTimeOfDay", ClampMin = "-0.6", ClampMax = "0.2", UIMin = "-0.45", UIMax = "0.0", DisplayName = "완전한 밤 시작"))
	float DayNightFullNight = -0.30f;

	/** Intensity of the plugin-managed moon Directional Light at full night. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle", ClampMin = "0.0", ClampMax = "100.0", UIMin = "0.0", UIMax = "30.0", DisplayName = "달빛 강도"))
	float MoonDirectionalIntensity = 0.50f;

	/** Color used by the moon Directional Light and the 3DGS night direct-light tint. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "달빛 색상"))
	FLinearColor MoonDirectionalColor = FLinearColor(0.52f, 0.65f, 1.00f, 1.0f);

	/** Ambient intensity retained at full night instead of forcing the 3DGS to black. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.4", DisplayName = "밤 환경광 강도"))
	float NightAmbientIntensity = 0.065f;

	/** Cool environment tint blended in as the sun goes below the horizon. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03C 시간대 색상", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "밤 환경광 색상"))
	FLinearColor NightAmbientColor = FLinearColor(0.22f, 0.30f, 0.52f, 1.0f);


	/** Warm pre-sunset/pre-sunrise tint while the sun is still above the horizon. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03C 시간대 색상", EditCondition = "bEnableAutomaticDayNightCycle && !bUseUnifiedPhysicalTimeOfDay", DisplayName = "골든아워 색상"))
	FLinearColor GoldenHourAmbientColor = FLinearColor(1.00f, 0.72f, 0.42f, 1.0f);

	/** Strong horizon tint at sunset/sunrise. Render-only; source 3DGS colours are untouched. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03C 시간대 색상", EditCondition = "bEnableAutomaticDayNightCycle && !bUseUnifiedPhysicalTimeOfDay", DisplayName = "일몰 지평선 색상"))
	FLinearColor TwilightAmbientColor = FLinearColor(1.00f, 0.38f, 0.14f, 1.0f);

	/** Cool post-sunset blue-hour tint. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03C 시간대 색상", EditCondition = "bEnableAutomaticDayNightCycle && !bUseUnifiedPhysicalTimeOfDay", DisplayName = "일몰 블루아워 색상"))
	FLinearColor BlueHourAmbientColor = FLinearColor(0.36f, 0.48f, 0.82f, 1.0f);

	/** Dawn blue-hour is intentionally lighter/more lavender than post-sunset blue hour. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03C 시간대 색상", EditCondition = "bEnableAutomaticDayNightCycle && !bUseUnifiedPhysicalTimeOfDay", DisplayName = "새벽 블루아워 색상"))
	FLinearColor DawnBlueHourAmbientColor = FLinearColor(0.50f, 0.56f, 0.82f, 1.0f);

	/** Dawn horizon colour: softer peach/pink, not the saturated orange-red sunset palette. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03C 시간대 색상", EditCondition = "bEnableAutomaticDayNightCycle && !bUseUnifiedPhysicalTimeOfDay", DisplayName = "새벽 지평선 색상"))
	FLinearColor DawnHorizonAmbientColor = FLinearColor(1.00f, 0.62f, 0.50f, 1.0f);

	/** Early-morning warm tone after the sun clears the horizon. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03C 시간대 색상", EditCondition = "bEnableAutomaticDayNightCycle && !bUseUnifiedPhysicalTimeOfDay", DisplayName = "아침 골든아워 색상"))
	FLinearColor DawnGoldenHourAmbientColor = FLinearColor(1.00f, 0.82f, 0.68f, 1.0f);

	/** Global hue-only time-of-day grade for the baked GS carrier. Luminance is preserved. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && !bUseUnifiedPhysicalTimeOfDay", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", DisplayName = "시간대 전체 톤 연동 강도"))
	float TimeOfDayGlobalTintStrength = 0.74f;

	/** Broadens the colour bands around sunrise/sunset. 1 = wide/slow transitions, 0 = compact/fast. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && !bUseUnifiedPhysicalTimeOfDay", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", DisplayName = "시간대 색상 전환 완만함"))
	float TimeOfDayColorTransitionSoftness = 0.78f;

	/** Stabilize the low-angle Stable shadow response near the horizon while preserving nearly all final cast-shadow contrast. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "지평선 그림자 안정화"))
	bool bStabilizeHorizonShadows = true;

	/** Solar-elevation range used by the low-angle stability signal. Final cast visibility lift is capped so shadows remain visible inside this range. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && bStabilizeHorizonShadows", ClampMin = "3.0", ClampMax = "20.0", UIMin = "3.0", UIMax = "15.0", DisplayName = "지평선 그림자 안정화 각도"))
	float HorizonShadowStabilizationDegrees = 10.0f;

	/** Strength of the warm SkyAtmosphere multiplier while the sun is close to the horizon. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && bDriveSkyAtmosphereLuminanceFromSun", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", DisplayName = "시간대 스카이 톤 강도"))
	float TwilightSkyAtmosphereTintStrength = 0.78f;

	/** Lock the whole scene to fixed Manual exposure while automatic day/night is active.
	 *  This prevents UE eye adaptation from brightening the sky back up after sunset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03A 시간대 · 태양/달", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "낮/밤 장면 노출 고정"))
	bool bLockSceneExposureForDayNight = true;

	/** Exposure Compensation used by the plugin-managed unbound Manual post process. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03A 시간대 · 태양/달", EditCondition = "bEnableAutomaticDayNightCycle && bLockSceneExposureForDayNight", ClampMin = "-8.0", ClampMax = "8.0", UIMin = "-4.0", UIMax = "4.0", DisplayName = "고정 노출 보정"))
	float DayNightFixedExposureCompensation = 0.0f;


	/** Extra negative exposure added only while the sun is high, keeping bright daytime foliage/water from washing to flat white. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && bLockSceneExposureForDayNight && !bUseUnifiedPhysicalTimeOfDay", DisplayName = "밝은 낮 하이라이트 보호"))
	bool bAutoProtectBrightDayExposure = true;

	/** Exposure compensation added at full bright-day protection. Negative values darken bright daytime scenes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && bLockSceneExposureForDayNight && bAutoProtectBrightDayExposure && !bUseUnifiedPhysicalTimeOfDay", ClampMin = "-4.0", ClampMax = "0.0", UIMin = "-2.0", UIMax = "0.0", DisplayName = "밝은 낮 노출 보정"))
	float BrightDayExposureCompensation = 0.0f;

	/** Solar elevation where bright-day protection begins to fade in. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && bLockSceneExposureForDayNight && bAutoProtectBrightDayExposure && !bUseUnifiedPhysicalTimeOfDay", ClampMin = "0.0", ClampMax = "89.0", UIMin = "0.0", UIMax = "45.0", DisplayName = "밝은 낮 보호 시작 고도"))
	float BrightDayExposureStartDegrees = 35.0f;

	/** Solar elevation where the full bright-day exposure compensation is reached. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && bLockSceneExposureForDayNight && bAutoProtectBrightDayExposure && !bUseUnifiedPhysicalTimeOfDay", ClampMin = "1.0", ClampMax = "90.0", UIMin = "5.0", UIMax = "70.0", DisplayName = "밝은 낮 보호 최대 고도"))
	float BrightDayExposureFullDegrees = 55.0f;

	// P21: optional world-sky fallback. These properties affect only Unreal sky and
	// the authored sun disk, never source PLY, GS weather, cast shadow masks or rain/snow.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03B 하늘 · 구름", DisplayName = "[P21] 실제 대기 자동 보완", ToolTip = "SkyAtmosphere가 레벨에 없을 때만 내장 대기를 켭니다. 기존 대기가 있으면 그것을 사용합니다."))
	bool bAutoCompleteSkyAtmosphere = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03B 하늘 · 구름", DisplayName = "[P21] 실시간 입체 구름", ToolTip = "씬에 다른 VolumetricCloud가 없다면 Engine 기본 구름 머티리얼을 사용합니다. 8K 파노라마에서는 비용을 확인하세요."))
	bool bAutoEnableVolumetricClouds = true;

	// P22: angular diameters are independent of the camera position and FOV.  These
	// are UE SkyAtmosphere directional-light disks, not camera-facing sprites.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03A 시간대 · 태양/달", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "[P22] 태양 디스크 각지름 (도)", ClampMin = "0.53", ClampMax = "3.00", UIMin = "0.53", UIMax = "2.00", ToolTip = "태양 디스크의 각지름. 카메라 거리와 무관합니다. 조명 그림자 부드러움에도 영향을 줄 수 있습니다."))
	float ManagedSunDiskAngleDegrees = 1.10f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03A 시간대 · 태양/달", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "[P22] 달 디스크 각지름 (도)", ClampMin = "0.53", ClampMax = "3.00", UIMin = "0.53", UIMax = "2.00", ToolTip = "달 Directional Light의 실제 대기 디스크 각지름. 카메라 위치와 무관합니다."))
	float ManagedMoonDiskAngleDegrees = 1.20f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03A 시간대 · 태양/달", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "[P22] 태양 디스크 발광 배율", ClampMin = "0.05", ClampMax = "2.0", UIMin = "0.10", UIMax = "1.0", ToolTip = "태양 원반 발광량만 조절합니다. Bloom과 대기 Mie 산란은 별도 효과이므로 완전히 제거되지 않을 수 있습니다."))
	float ManagedSunDiskEmissionGain = 0.50f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03A 시간대 · 태양/달", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "[P22] 달 디스크 발광 배율", ClampMin = "0.05", ClampMax = "2.0", UIMin = "0.10", UIMax = "1.0"))
	float ManagedMoonDiskEmissionGain = 0.80f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "[P22] 밤 하늘 최소 밝기", ClampMin = "0.05", ClampMax = "0.95", UIMin = "0.10", UIMax = "0.80", ToolTip = "대기 산란 결과의 최소 배율입니다. 실제 빛이 없는 픽셀에 색을 더하는 것은 아닙니다."))
	float ManagedNightSkyMinimum = 0.45f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "[P22] 밤 대기 밝기 보강", ClampMin = "1.0", ClampMax = "16.0", UIMin = "1.0", UIMax = "12.0", ToolTip = "달/대기 산란의 야간 화면 밝기만 증폭합니다. 기존 태양 Lux 및 원본 3DGS 밝기는 변경하지 않습니다."))
	float ManagedNightSkyLuminanceBoost = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "[P22] 하늘 시간대 색상 연동 강도", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", ToolTip = "기존 3DGS 골든아워/노을/새벽/블루아워/밤 색상을 실제 SkyAtmosphere 보정에 반영하는 비율입니다."))
	float ManagedSkyTimeColorSyncStrength = 0.90f;

	// P23: SUN BASE is authored separately from the runtime light Intensity, so a
	// night frame can never overwrite the design-time lux setting.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "[P23] 태양 고도별 실제 Lux 제어"))
	bool bP23AngleDrivenSunLux = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && bP23AngleDrivenSunLux", ClampMin = "-1.0", ClampMax = "300000.0", UIMin = "-1.0", UIMax = "120000.0", DisplayName = "[P23] 기준 태양 Lux (-1 = 기존 라이트값)", ToolTip = "-1이면 최초 감지한 Directional Light Lux를 기준으로 사용합니다. 저장된 라이트가 0이라면 이 값을 직접 입력해 주세요."))
	float P23BaseSunLux = -1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && bP23AngleDrivenSunLux", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "1.2", DisplayName = "[P23] 태양 Lux 전체 배율"))
	float P23SunLuxGain = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle", ClampMin = "0.0", ClampMax = "40.0", UIMin = "0.0", UIMax = "12.0", DisplayName = "[P23] 야간 달빛 보조 Lux", ToolTip = "깊은 밤 SkyAtmosphere가 검게 사라지는 현상을 완화할 수 있는 최소 달빛입니다. 밤에만 작동합니다."))
	float P23MoonSkyFillLux = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03A 시간대 · 태양/달", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "[P23] 태양/달 Light Shaft Bloom 제한"))
	bool bP23DisableLightShaftBloom = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03A 시간대 · 태양/달", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "[P23] 수동 노출 PostProcess Bloom 분리", ToolTip = "플러그인 전용 노출 PP에서만 Bloom을 제어합니다. 기존 레벨의 다른 PP나 구름 산란은 바꾸지 않습니다."))
	bool bP23OverridePostProcessBloom = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03A 시간대 · 태양/달", EditCondition = "bEnableAutomaticDayNightCycle && bP23OverridePostProcessBloom", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "1.0", DisplayName = "[P23] 후처리 Bloom 강도"))
	float P23PostProcessBloomIntensity = 0.08f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", DisplayName = "[P23] 조명 계산 상태"))
	FString RuntimeP23LightingStatus = TEXT("아직 계산되지 않음");

	// P24: independent photometric controls + authored time-color bridge for UE SkyAtmosphere.
	// These are the only new production controls; source 3DGS tone and weather remain unchanged.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03A 시간대 · 태양/달", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "[P24] 시간대 하늘/조명 통합 제어"))
	bool bP24UnifiedAtmosphereLighting = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03A 시간대 · 태양/달", EditCondition = "bEnableAutomaticDayNightCycle && bP24UnifiedAtmosphereLighting", ClampMin = "0.0", ClampMax = "300000.0", UIMin = "0.0", UIMax = "10000.0", DisplayName = "[P24] 태양 기준 Lux (저장되는 설계값)", ToolTip = "0 Lux부터 직접 입력 가능합니다. 시간대별 태양 밝기의 기준이며, 실제 적용값은 낮 태양 최대 Lux 상한도 따릅니다. 10000 Lux를 넘는 기존 값도 숫자 입력으로 유지/변경할 수 있습니다."))
	float P24ReferenceSunLux = 100000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03A 시간대 · 태양/달", EditCondition = "bEnableAutomaticDayNightCycle && bP24UnifiedAtmosphereLighting", ClampMin = "0.0", ClampMax = "12.0", UIMin = "0.0", UIMax = "5.0", DisplayName = "[P24] 해질녘 태양 잔광 Lux"))
	float P24TwilightSunLux = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03A 시간대 · 태양/달", EditCondition = "bEnableAutomaticDayNightCycle && bP24UnifiedAtmosphereLighting", ClampMin = "0.1", ClampMax = "20.0", UIMin = "0.2", UIMax = "6.0", DisplayName = "[P24] 밤 달빛 Lux (태양과 별개)"))
	float P24MoonSkyLux = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03B 하늘 · 구름", EditCondition = "bEnableAutomaticDayNightCycle && bP24UnifiedAtmosphereLighting", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", DisplayName = "[P24] 기존 시간대 색상 → 하늘 산란 연동"))
	float P24ScatteringColorSync = 0.75f;

	/** 0: automatic rising/falling from the authored sun motion; 1: force dawn; 2: force sunset.
	 *  A static directional orientation alone cannot distinguish a sunrise from a sunset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "03C 시간대 색상", EditCondition = "bEnableAutomaticDayNightCycle", ClampMin = "0", ClampMax = "2", UIMin = "0", UIMax = "2", DisplayName = "[P32] 새벽/노을 방향 (0 자동 · 1 새벽 · 2 노을)", ToolTip = "태양을 한 위치로 바로 이동하면 상승/하강을 알 수 없습니다. 이 경우 1=새벽 또는 2=노을로 직접 지정하세요."))
	int32 P32DawnDuskMode = 0;

	/** Independent pre-sunrise sky lift. 1 = unchanged; affects the atmospheric sky only, not GS or Sun/Moon lux. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "03C 시간대 색상", EditCondition = "bEnableAutomaticDayNightCycle && bP24UnifiedAtmosphereLighting",
		ClampMin = "1.0", ClampMax = "2.0", UIMin = "1.0", UIMax = "1.7",
		DisplayName = "[P33] 새벽 하늘 밝기 배율", ToolTip = "새벽 -10~+12도 사이에서만 적용합니다. 1.0 = Patch32, 기본 1.30."))
	float P33DawnSkyBrightness = 1.30f;

	/** Extra warm dusk scattering/tint. 1 = original, >1 makes orange-red horizon more prominent. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "03C 시간대 색상", EditCondition = "bEnableAutomaticDayNightCycle && bP24UnifiedAtmosphereLighting",
		ClampMin = "1.0", ClampMax = "2.5", UIMin = "1.0", UIMax = "2.25",
		DisplayName = "[P33] 노을 주황/붉은빛 강도", ToolTip = "노을 -10~+12도 구간의 하늘만 따뜻하게. 태양 Lux, 원본 GS, 밤은 유지합니다."))
	float P33DuskWarmth = 1.85f;

	/** Saturation of warm sunset palette in native Mie scattering. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "03C 시간대 색상", EditCondition = "bEnableAutomaticDayNightCycle && bP24UnifiedAtmosphereLighting",
		ClampMin = "1.0", ClampMax = "2.0", UIMin = "1.0", UIMax = "1.75",
		DisplayName = "[P33] 노을 색상 채도", ToolTip = "선명한 붉은 주황 하늘을 강조합니다. 1.0 = 기존 채도."))
	float P33DuskSaturation = 1.35f;

	/** Natural horizon warmth, using the existing morning/evening sky colours and real atmosphere scattering. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "03B 하늘 · 구름", EditCondition = "bEnableAutomaticDayNightCycle && bP24UnifiedAtmosphereLighting", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", DisplayName = "[P32] 새벽/노을 지평선 색상 강도"))
	float P32HorizonColorStrength = 0.85f;

	/** Additional fixed exposure offset at high solar elevations; helps white-clipped clouds when manual exposure is fixed for 8K panorama. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "03A 시간대 · 태양/달", EditCondition = "bEnableAutomaticDayNightCycle && bLockSceneExposureForDayNight", ClampMin = "0.0", ClampMax = "2.5", UIMin = "0.0", UIMax = "2.0", DisplayName = "[P32] 낮 하이라이트 노출 보호 (EV)", ToolTip = "낮의 구름과 하늘을 함께 보호하는 노출 보정입니다. 전체 씬에도 작용합니다. 0이면 비활성화."))
	float P32DayExposureGuardEV = 0.85f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03B 하늘 · 구름", EditCondition = "bEnableAutomaticDayNightCycle && bP24UnifiedAtmosphereLighting", ClampMin = "0.5", ClampMax = "6.0", UIMin = "1.0", UIMax = "4.0", DisplayName = "[P24] 밤 대기 밝기 (달빛 연동)", ToolTip = "SkyAtmosphere 산란광에 적용됩니다. 태양/달 자체 Lux를 변경하지 않습니다."))
	float P24NightSkyBrightness = 2.4f;

	// P27: day-sky luminance guard for manual exposure; the authored sky scattering
	// curves, original 3DGS tone and night sky remain unchanged. Set 1 to bypass.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "03B 하늘 · 구름", EditCondition = "bEnableAutomaticDayNightCycle && bP24UnifiedAtmosphereLighting",
		DisplayName = "낮 하늘 광량 (과노출 억제)", ClampMin = "0.01", ClampMax = "1.0", UIMin = "0.01", UIMax = "1.0"))
	float P27DaySkyLuminance = 0.12f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "03B 하늘 · 구름", EditCondition = "bEnableAutomaticDayNightCycle && bP24UnifiedAtmosphereLighting",
		DisplayName = "낮 태양 최대 Lux (원경 메시 밝기 제한)", ClampMin = "0.0", ClampMax = "300000.0", UIMin = "0.0", UIMax = "10000.0", ToolTip = "시간대별 자동 태양 Lux 상한을 0 Lux까지 낮출 수 있습니다. 슬라이더는 저광량 조절을 위해 0~10000 Lux이며 더 높은 값도 직접 입력할 수 있습니다. 태양 각도/달빛/하늘색 시간대 계산은 유지됩니다."))
	float P27SunLuxLimit = 30000.0f;

	// P40: runtime Sun Lux editing for distant mesh colour matching. This controls
	// only the scene's authored Directional Light intensity; TOD, moon, sky and GS
	// grading remain on their existing independent paths. Disabled by default.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "03A 시간대 · 태양/달", EditCondition = "bEnableAutomaticDayNightCycle",
		DisplayName = "[P40] 태양 Lux 수동 테스트", ToolTip = "켜면 태양의 시간대별 자동 Lux 덮어쓰기를 중단합니다. 아래 수동 Lux가 -1이면 Directional Light의 강도를 직접 편집할 수 있습니다. 끄면 시간대별 자동 Lux로 복귀합니다."))
	bool bP40ManualSunLuxMode = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "03A 시간대 · 태양/달", EditCondition = "bEnableAutomaticDayNightCycle && bP40ManualSunLuxMode",
		ClampMin = "-1.0", ClampMax = "300000.0", UIMin = "-1.0", UIMax = "10000.0",
		DisplayName = "[P40] 수동 태양 Lux (-1 = 라이트 직접 편집)", ToolTip = "-1이면 Directional Light의 Lux를 직접 편집하고 플러그인은 덮어쓰지 않습니다. 0 이상이면 이 Lux 값을 고정 적용합니다. 수동 모드를 끄면 자동 Lux로 돌아갑니다."))
	float P40ManualSunLux = -1.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "02 조명 및 그림자", meta = (GSGroup = "03B 하늘 · 구름", DisplayName = "[P24] 하늘/조명 적용 결과"))
	FString RuntimeP24SkyLightingStatus = TEXT("하늘 동기화 대기");



	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03B 하늘 · 구름", DisplayName = "[P21] 구름 아래 높이 (km)", ClampMin = "0.8", ClampMax = "6.0", UIMin = "1.0", UIMax = "4.0"))
	float ManagedCloudBaseAltitudeKm = 2.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "02 조명 및 그림자", meta = (GSGroup = "03B 하늘 · 구름", DisplayName = "[P21] 대기/구름 연결 상태"))
	FString RuntimeManagedSkyStatus = TEXT("대기 확인 대기 중");

	/** Drive the rendered SkyAtmosphere luminance from the same sun-altitude phase used by the moon transition.
	 *  This changes the Unreal sky itself; it does NOT multiply the baked/original 3DGS color. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "태양 고도 스카이 밝기 연동"))
	bool bDriveSkyAtmosphereLuminanceFromSun = true;

	/** Full-night luminance multiplier for SkyAtmosphere. 0.10 keeps a readable deep-blue night sky while remaining clearly below daytime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && bDriveSkyAtmosphereLuminanceFromSun", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.25", DisplayName = "밤 스카이 휘도 비율"))
	float NightSkyAtmosphereLuminanceScale = 0.10f;

	/** Keep a Movable/Stationary SkyLight capturing SkyAtmosphere while the sun moves.
	 *  This is Unreal's intended time-of-day path and gives the plugin a real sky irradiance source. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03B 하늘 · 구름", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "SkyLight 리얼타임 캡처"))
	bool bEnableDayNightSkyLightRealTimeCapture = true;

	/** Use the SkyLight's captured irradiance color to tint 3DGS ambient/sky lighting.
	 *  No screen readback is used; the cached spherical-harmonic sky lighting is read directly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03B 하늘 · 구름", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "3DGS 스카이 색 연동"))
	bool bSync3DGSAmbientFromSkyLight = true;

	/** Blend strength between the authored 3DGS ambient tint and the captured Unreal sky irradiance tint. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03B 하늘 · 구름", EditCondition = "bEnableAutomaticDayNightCycle && bSync3DGSAmbientFromSkyLight", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", DisplayName = "스카이 색 연동 강도"))
	float SkyLightColorSyncStrength = 0.90f;

	/** Multiplier applied to Stable/relight sky fill at full night. Direct moon light is separate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03B 하늘 · 구름", EditCondition = "bEnableAutomaticDayNightCycle", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.5", DisplayName = "밤 하늘 간접광 비율"))
	float NightSkyIndirectScale = 0.35f;

	/** Extra chromatic multiplier applied only as the atmosphere reaches night. The hue comes from NightAmbientColor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", EditCondition = "bEnableAutomaticDayNightCycle && bDriveSkyAtmosphereLuminanceFromSun", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", DisplayName = "밤 스카이 색상 전환 강도"))
	float NightSkyAtmosphereTintStrength = 0.72f;


	/** Show the compact time/solar-state overlay on the viewport after scene rendering. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", DisplayName = "화면 시간대 표시"))
	bool bShowTimeOfDaySceneOverlay = true;

	/** Current authored Directional Light pitch, exposed for angle/time debugging. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", DisplayName = "현재 라이트 Pitch (도)"))
	float RuntimeDirectionalLightPitchDegrees = 0.0f;

	/** Solar elevation derived from the Directional Light direction. Horizon = 0 degrees. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", DisplayName = "현재 태양 고도 (도)"))
	float RuntimeSolarElevationDegrees = 0.0f;

	/** Equinox-style 24-hour solar time derived from elevation plus rising/setting direction. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", DisplayName = "현재 시간 (24h)"))
	float RuntimeTimeOfDayHours = 12.0f;

	/** Human-readable HH:MM form of RuntimeTimeOfDayHours. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", DisplayName = "현재 시간 표시"))
	FString RuntimeTimeOfDayText = TEXT("12:00");

	/** Current solar phase derived only from solar elevation and rising/setting direction. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", DisplayName = "현재 시간대"))
	FString RuntimeTimeOfDayPhase = TEXT("Day");

	/** Runtime blend written by AGaussianSplatActor. 0 = day, 1 = full night. */
	UPROPERTY(Transient)
	float RuntimeNightBlend = 0.0f;


	/** Runtime combined warm twilight amount retained for compatibility with older grade paths. */
	UPROPERTY(Transient)
	float RuntimeTwilightBlend = 0.0f;

	UPROPERTY(Transient)
	float RuntimeGoldenHourBlend = 0.0f;

	UPROPERTY(Transient)
	float RuntimeSunsetBlend = 0.0f;

	UPROPERTY(Transient)
	float RuntimeBlueHourBlend = 0.0f;

	/** True while the authored Sun is moving upward in solar elevation; false while setting. */
	UPROPERTY(Transient)
	bool bRuntimeSolarRising = false;

	UPROPERTY(Transient)
	FLinearColor RuntimeTimeOfDayGradeColor = FLinearColor::White;

	UPROPERTY(Transient)
	float RuntimeTimeOfDayGradeStrength = 0.0f;

	/** Runtime outputs from the unified continuous solar model. */
	UPROPERTY(Transient)
	float RuntimeUnifiedTODDirectEnergy = 1.0f;

	UPROPERTY(Transient)
	float RuntimeUnifiedTODSkyEnergy = 1.0f;

	UPROPERTY(Transient)
	float RuntimeUnifiedTODCarrierBrightness = 1.0f;

	UPROPERTY(Transient)
	float RuntimeUnifiedTODGamma = 1.0f;

	UPROPERTY(Transient)
	float RuntimeUnifiedTODSaturation = 1.0f;

	/** Runtime 12:00->14:00->16:00 smooth afternoon lift amount (already includes artist strength). */
	UPROPERTY(Transient)
	float RuntimeUnifiedTODAfternoonLift = 0.0f;

	UPROPERTY(Transient)
	FLinearColor RuntimeUnifiedTODSunColor = FLinearColor::White;

	UPROPERTY(Transient)
	FLinearColor RuntimeUnifiedTODAmbientColor = FLinearColor(0.72f, 0.84f, 1.0f, 1.0f);

	UPROPERTY(Transient)
	float RuntimeUnifiedTODGradeStrength = 0.0f;

	/** Physically staged night environment color derived from solar depression and clock time. */
	UPROPERTY(Transient)
	FLinearColor RuntimeNightEnvironmentColor = FLinearColor(0.22f, 0.30f, 0.52f, 1.0f);

	/** Night energy multiplier: civil twilight > early night > midnight minimum > pre-dawn recovery. */
	UPROPERTY(Transient)
	float RuntimeNightEnvironmentEnergyScale = 1.0f;

	/** Runtime night grade values shared by the GS carrier and sky bridge. */
	UPROPERTY(Transient)
	float RuntimeNightGradeGamma = 1.12f;

	UPROPERTY(Transient)
	float RuntimeNightGradeSaturation = 0.78f;

	UPROPERTY(Transient)
	float RuntimeNightGradeBrightness = 0.75f;

	/** Camera-independent celestial direction used by GS direct lighting (Sun by day, Moon by night). */
	UPROPERTY(Transient)
	FVector RuntimeActiveCelestialDirection = FVector::UpVector;

	UPROPERTY(Transient)
	bool bRuntimeActiveCelestialDirectionValid = false;

	/** v2819 continuous upper-hemisphere direction used by Stable cast/LightDepth/proxy shadow.
	 *  Unlike the visual Sun/Moon authority this never flips 180 degrees at the horizon. */
	UPROPERTY(Transient)
	FVector RuntimeStableShadowDirection = FVector::UpVector;

	UPROPERTY(Transient)
	bool bRuntimeStableShadowDirectionValid = false;

	/** GS-only continuous celestial light state. This prevents the direct-light side and energy
	 *  from snapping when the visual Sun/Moon authority changes at the horizon. */
	UPROPERTY(Transient)
	FLinearColor RuntimeContinuousCelestialColor = FLinearColor::White;

	UPROPERTY(Transient)
	float RuntimeContinuousCelestialIntensity = 0.0f;

	UPROPERTY(Transient)
	bool bRuntimeContinuousCelestialStateValid = false;

	UPROPERTY(Transient)
	bool bRuntimeActiveCelestialIsMoon = false;

	/** Captured/fallback Unreal sky tint written by AGaussianSplatActor. */
	UPROPERTY(Transient)
	FLinearColor RuntimeSkyAmbientColor = FLinearColor(0.60f, 0.72f, 1.0f, 1.0f);

	/** True when RuntimeSkyAmbientColor came from the current SkyLight irradiance capture. */
	UPROPERTY(Transient)
	bool bRuntimeSkyAmbientColorValid = false;

	/** Ambient multiplier used when Unreal light receiving is enabled. Lower values make unlit areas darker. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "2.0", EditCondition = "bEnableCinematicEffects && bReceiveUnrealLights", DisplayName = "환경광 강도", ToolTip = "언리얼 조명을 받는 3DGS의 환경광 배율입니다. 전체 효과 또는 언리얼 라이트 수신이 꺼져 있으면 적용되지 않습니다."))
	float UnrealAmbientIntensity = 0.14f;

	/** Ambient color applied before direct lights. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", DisplayName = "환경광 색상"))
	FLinearColor UnrealAmbientColor = FLinearColor(0.88f, 0.93f, 1.0f, 1.0f);

	/** Stable proxy sky/environment fill. This remains on open back-facing surfaces like UE SkyLight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "03 씬 라이트", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.5", DisplayName = "Stable 하늘 간접광"))
	float UnrealIndirectLightIntensity = 0.10f;

	/** Stable proxy warm ground bounce. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "03 씬 라이트", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "0.4", DisplayName = "Stable 지면 반사광"))
	float UnrealGroundBounce = 0.06f;

	/** Saturation retained in shadowed splats. 1 = original color, 0 = grayscale. */
	UPROPERTY()
	float UnrealShadowSaturation = 0.82f;

    /** Apply the indirect-light grade inside shadowed regions for comparison. */
	UPROPERTY()
	bool bEnableUnrealShadowLighting = false;

	/** Color carried into shadowed regions by the indirect-light approximation. */
	UPROPERTY()
	FLinearColor UnrealShadowTint = FLinearColor(0.78f, 0.86f, 1.0f, 1.0f);

	/** Normal source for directional light response. Hybrid Proxy falls back to covariance normals where the proxy is unavailable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "05 프록시 및 Stable", meta = (GSGroup = "04 표면 노멀", DisplayName = "조명 노멀 모드"))
	ELCCCinematicGSLightingNormalMode UnrealLightingNormalMode = ELCCCinematicGSLightingNormalMode::HybridProxy;

	/** How strongly directional lights use angle-based N dot L shading. 0 = intensity only, 1 = full angle response. */
	UPROPERTY()
	float UnrealDirectionalResponse = 1.0f;

	/** Wraps light around surfaces to avoid completely black 3DGS areas. 0 = hard Lambert, 1 = very soft wrap. */
	UPROPERTY()
	float UnrealLightWrap = 0.0f;

	/** Build and use a component-local proxy-mesh normal grid. Falls back to the selected splat normal when unavailable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "05 프록시 및 Stable", meta = (GSGroup = "04 표면 노멀", DisplayName = "프록시 표면 노멀 사용"))
	bool bEnableProxySurfaceNormals = true;

	/** Blend strength of the proxy normal after cache confidence is applied. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "05 프록시 및 Stable", meta = (GSGroup = "04 표면 노멀", ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bEnableProxySurfaceNormals", DisplayName = "프록시 노멀 강도"))
	float ProxyNormalStrength = 0.85f;

	/** Confidence threshold below which the renderer falls back to the splat covariance normal. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "05 프록시 및 Stable", meta = (GSGroup = "04 표면 노멀", ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bEnableProxySurfaceNormals", DisplayName = "프록시 노멀 신뢰도 기준"))
	float ProxyNormalConfidenceThreshold = 0.12f;

	/** Remaining proxy-normal strength on strongly green foliage. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "05 프록시 및 Stable", meta = (GSGroup = "04 표면 노멀", ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bEnableProxySurfaceNormals"))
	float FoliageProxyNormalStrength = 0.30f;

	/** Longest-axis grid resolution used when rasterizing the proxy mesh normals. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "05 프록시 및 Stable", meta = (GSGroup = "04 표면 노멀", ClampMin = "16", ClampMax = "128", EditCondition = "bEnableProxySurfaceNormals"))
	int32 ProxyNormalGridResolution = 72;

	/** Extra component-local padding around the transformed proxy bounds used by the normal grid.
	 *  Keeping the grid proxy-centric prevents far-away GS outliers from collapsing normal resolution. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "05 프록시 및 Stable", meta = (GSGroup = "04 표면 노멀", ClampMin = "0.0", ClampMax = "500.0", UIMin = "0.0", UIMax = "150.0", EditCondition = "bEnableProxySurfaceNormals", DisplayName = "Proxy Normal Grid Margin (cm)"))
	float ProxyNormalGridMarginCm = 75.0f;

	/** Full influence distance for the authored proxy vertex normal used by Stable direct lighting. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "05 프록시 및 Stable", meta = (GSGroup = "04 표면 노멀", ClampMin = "0.0", ClampMax = "500.0", UIMin = "0.0", UIMax = "220.0", DisplayName = "Stable Lighting Normal Full Distance (cm)"))
	float StableLightingNormalFullDistanceCm = 80.0f;

	/** Beyond this distance the Stable direct-light normal falls back to the splat/proxy-grid lighting normal.
	 *  Shadow binding can remain broad, so cast coverage is not reduced. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "05 프록시 및 Stable", meta = (GSGroup = "04 표면 노멀", ClampMin = "1.0", ClampMax = "1000.0", UIMin = "20.0", UIMax = "350.0", DisplayName = "Stable Lighting Normal Max Distance (cm)"))
	float StableLightingNormalMaxDistanceCm = 220.0f;

	/** Number of neighbor-fill iterations used to cover sparse proxy meshes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "05 프록시 및 Stable", meta = (GSGroup = "04 표면 노멀", ClampMin = "0", ClampMax = "4", EditCondition = "bEnableProxySurfaceNormals"))
	int32 ProxyNormalPropagationCells = 2;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "04 표면 노멀", DisplayName = "Proxy Normal Cache"))
	ELCCCinematicGSProxyNormalCacheStatus ProxyNormalCacheStatus = ELCCCinematicGSProxyNormalCacheStatus::Disabled;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "04 표면 노멀", DisplayName = "Proxy Normal Grid Coverage"))
	float ProxyNormalCoveragePercent = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "05 프록시 및 Stable", meta = (GSGroup = "04 표면 노멀", DisplayName = "Proxy Normal Grid Cells"))
	int32 ProxyNormalGridCellCount = 0;

	/** Enable an approximate self/contact shadow layer for the custom splat renderer. This is not UE Virtual Shadow Map yet, but gives cinematic light direction and grounded shadows. */
	UPROPERTY()
	bool bEnableApproximateShadows = true;

	/** Strength of the approximate directional self-shadow and shadow-ray layer. */
	UPROPERTY()
	float UnrealShadowStrength = 0.65f;

	/** Extra darkening near the lower part of the scan volume, useful for ground contact and under-eave darkness. */
	UPROPERTY()
	float UnrealContactShadowStrength = 0.35f;

	/** Higher values make light falloff less uniform and darker at grazing angles. */
	UPROPERTY()
	float UnrealLightingContrast = 1.65f;

	/** Blends covariance-axis detail back into WorldUp/Hybrid normals so large scans do not shade as one flat plate. */
	UPROPERTY()
	float UnrealMicroNormalStrength = 0.35f;

	/** Softens the transition between lit and shadowed splats. */
	UPROPERTY()
	float UnrealShadowSoftness = 0.18f;

	/** Adds subtle world-space breakup so the shadow layer does not look like one uniform ramp. */
	UPROPERTY()
	float UnrealShadowBreakup = 0.18f;

	/** Depth tolerance in centimetres for proxy self-shadowing. Raise this to remove floor striping; lower it to retain tighter self-occlusion. */
	UPROPERTY()
	float ProxySelfShadowDepthBias = 120.0f;

	/** Depth tolerance in centimetres for external casters such as Cube. Keep this much lower than the self-shadow bias. */
	UPROPERTY()
	float ProxyExternalShadowDepthBias = 6.0f;

	/** Enable the second-integration hybrid relighting path. Original GS and relit color remain continuously blendable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", DisplayName = "리라이팅 사용"))
	bool bEnableRelightingCore = true;

	/** Blend from the original scene-linear GS color to the approximate-albedo relit result. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "리라이팅 강도"))
	float RelightStrength = 0.58f;

	/** Strength of bounded approximate de-lighting. This reduces baked highlights and lifts baked shadows without spatial filtering. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "근사 알베도 강도"))
	#endif
	UPROPERTY()
	float ApproxAlbedoStrength = 0.58f;

	/** Maximum baked-shadow lift used while estimating approximate albedo. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", ClampMin = "0.0", ClampMax = "0.6", DisplayName = "알베도 그림자 리프트"))
	#endif
	UPROPERTY()
	float ApproxAlbedoShadowLift = 0.10f;

	/** Maximum baked-highlight reduction used while estimating approximate albedo. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", ClampMin = "0.0", ClampMax = "0.6", DisplayName = "알베도 하이라이트 감소"))
	#endif
	UPROPERTY()
	float ApproxAlbedoHighlightReduction = 0.65f;

	/** Current build state of the Nanite-independent spatial lighting grid. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", DisplayName = "Cache Status"))
	ELCCCinematicGSSpatialDelightingCacheStatus SpatialDelightingCacheStatus = ELCCCinematicGSSpatialDelightingCacheStatus::Unknown;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", DisplayName = "Grid Cell Count"))
	int32 SpatialDelightingGridCellCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", DisplayName = "Occupied Cell Count"))
	int32 SpatialDelightingOccupiedCellCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", DisplayName = "Mapped Splat Coverage"))
	float SpatialDelightingCoveragePercent = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", DisplayName = "Average Cache Confidence"))
	float SpatialDelightingAverageConfidence = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", DisplayName = "Grid Cell Size"))
	float SpatialDelightingGridCellSize = 0.0f;

	/** Use Nanite-independent spatial-grid low-frequency lighting to remove broad baked illumination before Unreal relighting. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거"))
	bool bEnableSpatialDelighting = false;

	/** Blend strength of the spatial low-frequency de-lighting estimate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", EditCondition = "bEnableCinematicEffects && (bEnableSpatialDelighting || bEnableRelightingCore)", DisplayName = "공간 디라이팅 강도", ToolTip = "공간 저주파 조명 제거 강도입니다. 리라이팅 사용 중에도 0부터 1까지 입력값을 그대로 사용합니다."))
	float SpatialDelightingStrength = 0.55f;

	/** Amount of the cached low-frequency color tint removed with the luminance field. Keep low to preserve material color. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", ClampMin = "0.0", ClampMax = "0.5"))
	float SpatialDelightingChromaStrength = 0.12f;

	/** Minimum allowed baked-light factor. Higher values reduce shadow lifting and noise amplification. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", ClampMin = "0.25", ClampMax = "1.0"))
	float SpatialDelightingMinLighting = 0.60f;

	/** Maximum allowed baked-light factor. Lower values reduce aggressive highlight removal. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", ClampMin = "1.0", ClampMax = "3.0"))
	float SpatialDelightingMaxLighting = 1.55f;

	/** Protects very dark trained colors from division-based amplification. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", ClampMin = "0.0", ClampMax = "1.0"))
	float SpatialDelightingDarkProtection = 0.70f;

	/** Maximum scene-linear luminance allowed for reconstructed approximate albedo. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", ClampMin = "0.25", ClampMax = "3.0"))
	float SpatialDelightingMaxAlbedoLuminance = 1.20f;

	/** Reduces de-lighting strength on strongly green foliage to preserve leaf chroma and avoid grey vegetation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", ClampMin = "0.0", ClampMax = "1.0"))
	float SpatialDelightingFoliageProtection = 0.65f;

	/** Maximum per-channel tint change relative to the original color. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", ClampMin = "1.0", ClampMax = "2.0"))
	float SpatialDelightingMaxChromaScale = 1.35f;

	/** Multiplier used by legacy absolute-difference diagnostic views. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", ClampMin = "1.0", ClampMax = "10.0"))
	float SpatialDelightingDifferenceGain = 4.0f;

	/** Minimum broad lighting deviation, in exposure values, before spatial correction starts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", DisplayName = "Evidence Start (EV)", ClampMin = "0.0", ClampMax = "1.0"))
	float SpatialDelightingEvidenceMinEV = 0.10f;

	/** Broad lighting deviation, in exposure values, at which spatial correction reaches full evidence. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", DisplayName = "Evidence Full (EV)", ClampMin = "0.05", ClampMax = "2.0"))
	float SpatialDelightingEvidenceMaxEV = 0.65f;

	/** Hard cap on spatial correction even when Strength is 1.0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", ClampMin = "0.0", ClampMax = "1.0"))
	float SpatialDelightingMaxCorrectionWeight = 0.50f;

	/** Remaining correction scale on strongly green foliage after foliage protection is applied. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", ClampMin = "0.0", ClampMax = "1.0"))
	float SpatialDelightingFoliageCorrectionScale = 0.35f;

	/** Maximum allowed lift when removing a baked shadow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", ClampMin = "1.0", ClampMax = "2.0"))
	float SpatialDelightingMaxShadowLift = 1.25f;

	/** Minimum retained brightness when suppressing a baked highlight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", ClampMin = "0.50", ClampMax = "1.0"))
	float SpatialDelightingMinHighlightSuppression = 0.75f;

	/** Exposure range mapped from black to white by the signed luminance diagnostic. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "02 조명 및 그림자", meta = (GSGroup = "04 리라이팅 및 촬영광 제거", ClampMin = "0.25", ClampMax = "4.0"))
	float SpatialDelightingSignedDifferenceRangeEV = 1.0f;

	/** Strength of the target-reference daylight grade, applied in scene-linear HDR before Unreal tonemapping. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "01 노출 및 필름", DisplayName = "전체 룩 강도", ClampMin = "0.0", ClampMax = "1.0"))
	#endif
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "01 노출 및 필름", DisplayName = "전체 색조 효과 강도", ClampMin = "0.0", ClampMax = "1.0"))
	float TargetLookStrength = 1.0f;

	/** Linear multiplier applied to the untouched 3DGS color before gamma, saturation, seasonal color, bloom and surface effects. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "01 노출 및 필름", ClampMin = "0.0", UIMin = "0.0", UIMax = "8.0", DisplayName = "원본 3DGS 밝기"))
	float Original3DGSBrightness = 1.0f;

	/** Optional GS-only exposure trim applied before the explicit gamma stage. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "01 노출 및 필름", DisplayName = "전체 노출 EV", ClampMin = "-1.0", ClampMax = "1.0"))
	float TargetExposureEV = 0.0f;
	#if 0

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "01 노출 및 필름", DisplayName = "시네마틱 자동 노출 사용"))
	bool bEnableCinematicAutoExposure = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "01 노출 및 필름", ClampMin = "-10.0", ClampMax = "20.0", DisplayName = "자동 노출 최소 EV100"))
	float CinematicExposureMinEV100 = -3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "01 노출 및 필름", ClampMin = "-10.0", ClampMax = "20.0", DisplayName = "자동 노출 최대 EV100"))
	float CinematicExposureMaxEV100 = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "01 노출 및 필름", ClampMin = "0.02", ClampMax = "10.0", DisplayName = "밝아지는 적응 속도"))
	float CinematicExposureSpeedUp = 1.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "01 노출 및 필름", ClampMin = "0.02", ClampMax = "10.0", DisplayName = "어두워지는 적응 속도"))
	float CinematicExposureSpeedDown = 0.45f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "01 노출 및 필름", ClampMin = "-4.0", ClampMax = "4.0", DisplayName = "후처리 노출 보정"))
	float CinematicExposureBias = -0.30f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "01 노출 및 필름", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "필름 섀도우 곡선"))
	float CinematicFilmToe = 0.48f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "01 노출 및 필름", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "필름 하이라이트 곡선"))
	float CinematicFilmShoulder = 0.30f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "01 노출 및 필름", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "렌즈 비네팅"))
	float CinematicVignette = 0.18f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "01 노출 및 필름", ClampMin = "0.0", ClampMax = "4.0", DisplayName = "렌즈 블룸 강도"))
	float CinematicBloomIntensity = 0.30f;
	#endif
	UPROPERTY()
	bool bEnableCinematicAutoExposure = false;
	UPROPERTY()
	float CinematicExposureMinEV100 = -3.0f;
	UPROPERTY()
	float CinematicExposureMaxEV100 = 1.5f;
	UPROPERTY()
	float CinematicExposureSpeedUp = 1.2f;
	UPROPERTY()
	float CinematicExposureSpeedDown = 0.45f;
	UPROPERTY()
	float CinematicExposureBias = -0.30f;
	UPROPERTY()
	float CinematicFilmToe = 0.48f;
	UPROPERTY()
	float CinematicFilmShoulder = 0.30f;
	UPROPERTY()
	float CinematicVignette = 0.18f;
	UPROPERTY()
	float CinematicBloomIntensity = 0.30f;

	/** Unreal spatial bloom threshold applied after the scene-linear GS composite. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "06 블룸 및 글레어", DisplayName = "Bloom Threshold", ClampMin = "-1.0", ClampMax = "10.0"))
	float CinematicBloomThreshold = 0.42f;

	/** Pure plugin-side screen-space spread radius for bright 3DGS splats. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "06 블룸 및 글레어", DisplayName = "3DGS Bloom Spread (px)", ClampMin = "0.0", UIMin = "0.0", UIMax = "64.0"))
	float PluginBloomSpreadPixels = 24.0f;

	/** Energy scattered from bright splats into neighbouring 3DGS pixels. No hidden maximum is applied. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "06 블룸 및 글레어", DisplayName = "3DGS Bloom Scatter", ClampMin = "0.0", UIMin = "0.0", UIMax = "8.0"))
	float PluginBloomScatter = 3.0f;

	/** Blend between source highlight colour and the captured Unreal directional-light colour. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "06 블룸 및 글레어", DisplayName = "Bloom Sun Color Influence", UIMin = "0.0", UIMax = "1.0"))
	float PluginBloomSunColorInfluence = 1.0f;

	/** Legacy colour-highlight value retained only so existing assets still deserialize safely. */
	UPROPERTY()
	float PluginBloomLitMaskStrength = 1.0f;

	/** Independent strength of the saturation-gated colour-bloom channel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "06 블룸 및 글레어", DisplayName = "색상 블룸 강도", ClampMin = "0.0", ClampMax = "4.0",
			UIMin = "0.0", UIMax = "2.0"))
	float PluginColorBloomStrength = 1.0f;

	/** Brightest RGB channel required for the independent colour-bloom path. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "06 블룸 및 글레어", DisplayName = "색상 블룸 임계값", ClampMin = "0.0", ClampMax = "4.0",
			UIMin = "0.0", UIMax = "1.5"))
	float PluginColorBloomThreshold = 0.25f;

	/** Minimum HSV-style saturation required before a pixel can enter the colour-bloom path. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "06 블룸 및 글레어", DisplayName = "색상 블룸 최소 채도", ClampMin = "0.0", ClampMax = "1.0",
			UIMin = "0.0", UIMax = "1.0"))
	float PluginColorBloomMinSaturation = 0.20f;

	/** Fraction of the bloom threshold used for the smooth transition below/above the cutoff. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "06 블룸 및 글레어", DisplayName = "블룸 소프트 니", ClampMin = "0.0", ClampMax = "1.0",
			UIMin = "0.0", UIMax = "1.0"))
	float PluginBloomSoftKnee = 0.25f;

	/** Legacy serialization compatibility only; the current composite has no separate toe curve. */
	UPROPERTY()
	float TargetToeStrength = 0.0f;

	/** Direct final-composite saturation multiplier. 1.0 is neutral. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "02 전체 색감", DisplayName = "전체 채도", ClampMin = "0.5", ClampMax = "2.5"))
	#endif
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "02 전체 색감", DisplayName = "전체 채도", ClampMin = "0.0", ClampMax = "2.5", UIMin = "0.0", UIMax = "2.5"))
	float TargetChromaRecovery = 1.65f;

	/** Amount of source chroma retained in deep shadows after log-luminance shaping. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "02 전체 색감", DisplayName = "Stable 그림자 원본 색상 유지", ClampMin = "0.0", ClampMax = "1.0",
		ToolTip = "현재 Stable 캐스트 × 폐쇄 그림자 영역에서만 원본 3DGS의 색상/채도를 유지합니다. 그림자 밝기를 다시 올리지는 않습니다."))
	float TargetShadowColorRetention = 0.95f;

	/** Selective green enhancement. It is hue- and highlight-gated to avoid fluorescent foliage. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "02 전체 색감", DisplayName = "녹색 증폭", ClampMin = "0.0", ClampMax = "0.5"))
	float TargetGreenBoost = 0.12f;

	/** Direct final-composite gamma exponent. Values above 1.0 darken the middle values while preserving black. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "02 전체 색감", DisplayName = "중간톤 대비", ClampMin = "0.55", ClampMax = "2.40"))
	#endif
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "02 전체 색감", DisplayName = "전체 감마", ClampMin = "0.25", ClampMax = "4.0", UIMin = "0.5", UIMax = "3.0", ToolTip = "1보다 크면 중간톤이 어두워지고, 1보다 작으면 중간톤이 밝아집니다."))
	float TargetMidtoneContrast = 1.35f;

	/** One UE post-process grade after GS/mesh composition; only the primary scene owner publishes it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "00 전체 씬 공통", DisplayName = "3DGS + 일반 에셋 공통 후처리", ToolTip = "전체 씬에 UE 후처리를 적용합니다. 기존 GS 전용 보정은 유지됩니다. 공통 후처리는 톤매핑 이전 GS 합성 모드 0을 사용합니다."))
	bool bEnableWholeScenePostProcess = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "00 전체 씬 공통", DisplayName = "전체 씬 후처리 설정", EditCondition = "bEnableWholeScenePostProcess"))
	FPostProcessSettings WholeScenePostProcessSettings;

	/** Color grade applied only where the authoritative proxy direct-light capture is bright. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "03 빛받는 부분", DisplayName = "빛받는 부분 조정 강도", ClampMin = "0.0", ClampMax = "1.0"))
	float SunlitGradeStrength = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "03 빛받는 부분", DisplayName = "빛받는 부분 감마", ClampMin = "0.25", ClampMax = "4.0", UIMin = "0.5", UIMax = "2.0"))
	float SunlitGamma = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "03 빛받는 부분", DisplayName = "빛받는 부분 채도", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "2.0"))
	float SunlitSaturation = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "03 빛받는 부분", DisplayName = "빛받는 부분 밝기", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "2.0"))
	float SunlitBrightness = 1.0f;


	/** Gaussian-native additive sun lobe. It is evaluated per splat from Approx Albedo, the selected stable normal and the collected Directional Light. It never reads or modifies the proxy shadow capture. */
	UPROPERTY()
	bool bEnableGaussianSunAccent = true;

	UPROPERTY()
	float GaussianSunAccentStrength = 0.48f;

	UPROPERTY()
	float GaussianSunAccentStart = 0.18f;

	UPROPERTY()
	float GaussianSunAccentFull = 0.72f;

	UPROPERTY()
	float GaussianSunAccentGamma = 0.85f;

	UPROPERTY()
	FLinearColor GaussianSunAccentTint = FLinearColor(1.0f, 0.92f, 0.76f, 1.0f);

	UPROPERTY()
	float GaussianSunAccentTintStrength = 0.18f;

	UPROPERTY()
	float GaussianSunAccentHighlightCompression = 0.55f;

	UPROPERTY()
	float GaussianSunAccentDetailRetention = 0.88f;

	/** Gaussian-native thin-leaf transmission approximation. It uses the pre-lighting vegetation classifier and per-splat normal/view/light geometry only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "05 식물 투과광", DisplayName = "식물 투과광 사용"))
	bool bEnableGaussianPlantTransmission = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "05 식물 투과광", DisplayName = "식물 투과광 강도", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "1.0"))
	float GaussianPlantTransmissionStrength = 0.32f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "05 식물 투과광", DisplayName = "역광 시작", ClampMin = "0.0", ClampMax = "1.0"))
	float GaussianPlantBacklightStart = 0.08f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "05 식물 투과광", DisplayName = "역광 완전 투과", ClampMin = "0.01", ClampMax = "1.0"))
	float GaussianPlantBacklightFull = 0.70f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "05 식물 투과광", DisplayName = "투과광 전환 감마", ClampMin = "0.25", ClampMax = "4.0", UIMin = "0.5", UIMax = "2.0"))
	float GaussianPlantTransmissionGamma = 0.82f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "05 식물 투과광", DisplayName = "투과광 색상"))
	FLinearColor GaussianPlantTransmissionTint = FLinearColor(1.0f, 0.67f, 0.22f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "05 식물 투과광", DisplayName = "투과광 색조 적용 강도", ClampMin = "0.0", ClampMax = "1.0"))
	float GaussianPlantTransmissionTintStrength = 0.35f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "05 식물 투과광", DisplayName = "투과광 채도", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "1.5"))
	float GaussianPlantTransmissionSaturation = 1.10f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자",
		meta = (GSGroup = "05 식물 투과광", DisplayName = "카메라 역광 집중도", ClampMin = "0.25", ClampMax = "8.0", UIMin = "0.5", UIMax = "4.0"))
	float GaussianPlantViewPhasePower = 2.0f;

	/** Color grade applied before the final proxy visibility multiply. It cannot recreate direct light in a zero-light region. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "04 그늘 부분", DisplayName = "그늘 부분 조정 강도", ClampMin = "0.0", ClampMax = "1.0"))
	float ShadowGradeStrength = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "04 그늘 부분", DisplayName = "그늘 부분 감마", ClampMin = "0.25", ClampMax = "4.0", UIMin = "0.5", UIMax = "2.0"))
	float ShadowAreaGamma = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "04 그늘 부분", DisplayName = "그늘 부분 채도", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "2.0"))
	float ShadowAreaSaturation = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "04 그늘 부분", DisplayName = "그늘 부분 밝기", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "2.0", ToolTip = "프록시 직사광이 0인 곳은 마지막 직사광 곱셈에서 계속 0으로 유지됩니다."))
	float ShadowAreaBrightness = 1.0f;

	/** Sunset-only grade. Dawn has a separate palette and grade below. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "05 일몰 전용", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "일몰 컬러 보정"))
	bool bEnableTwilightColorGrade = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "05 일몰 전용", EditCondition = "bEnableAutomaticDayNightCycle && bEnableTwilightColorGrade", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", DisplayName = "일몰 보정 강도"))
	float TwilightColorGradeStrength = 0.72f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "05 일몰 전용", EditCondition = "bEnableAutomaticDayNightCycle && bEnableTwilightColorGrade", ClampMin = "0.25", ClampMax = "4.0", UIMin = "0.5", UIMax = "2.0", DisplayName = "일몰 감마"))
	float TwilightGamma = 0.94f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "05 일몰 전용", EditCondition = "bEnableAutomaticDayNightCycle && bEnableTwilightColorGrade", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "2.0", DisplayName = "일몰 채도"))
	float TwilightSaturation = 1.12f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "05 일몰 전용", EditCondition = "bEnableAutomaticDayNightCycle && bEnableTwilightColorGrade", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "1.5", DisplayName = "일몰 밝기"))
	float TwilightBrightness = 1.00f;

	/** Independent dawn grade; deliberately softer and less orange than sunset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "06 새벽 전용", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "새벽 컬러 보정"))
	bool bEnableDawnColorGrade = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "06 새벽 전용", EditCondition = "bEnableAutomaticDayNightCycle && bEnableDawnColorGrade", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", DisplayName = "새벽 보정 강도"))
	float DawnColorGradeStrength = 0.58f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "06 새벽 전용", EditCondition = "bEnableAutomaticDayNightCycle && bEnableDawnColorGrade", ClampMin = "0.25", ClampMax = "4.0", UIMin = "0.5", UIMax = "2.0", DisplayName = "새벽 감마"))
	float DawnGamma = 0.98f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "06 새벽 전용", EditCondition = "bEnableAutomaticDayNightCycle && bEnableDawnColorGrade", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "2.0", DisplayName = "새벽 채도"))
	float DawnSaturation = 1.04f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "06 새벽 전용", EditCondition = "bEnableAutomaticDayNightCycle && bEnableDawnColorGrade", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "1.5", DisplayName = "새벽 밝기"))
	float DawnBrightness = 1.00f;

	/** Final-image color grade that fades in only with RuntimeNightBlend. It does not rewrite originalLinear or the source PLY color. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "07 밤 전용", EditCondition = "bEnableAutomaticDayNightCycle", DisplayName = "밤 전용 컬러 보정"))
	bool bEnableNightColorGrade = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "07 밤 전용", EditCondition = "bEnableAutomaticDayNightCycle && bEnableNightColorGrade", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", DisplayName = "밤 컬러 보정 강도"))
	float NightColorGradeStrength = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "07 밤 전용", EditCondition = "bEnableAutomaticDayNightCycle && bEnableNightColorGrade", ClampMin = "0.25", ClampMax = "4.0", UIMin = "0.5", UIMax = "2.5", DisplayName = "밤 감마", ToolTip = "1보다 크면 밤의 중간톤/하이라이트가 어두워집니다. 기본값은 과도한 암전을 피하도록 완만하게 설정되어 있으며 원본 3DGS 데이터는 수정하지 않습니다."))
	float NightGamma = 1.12f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "07 밤 전용", EditCondition = "bEnableAutomaticDayNightCycle && bEnableNightColorGrade", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "2.0", DisplayName = "밤 채도"))
	float NightSaturation = 0.78f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "07 밤 전용", EditCondition = "bEnableAutomaticDayNightCycle && bEnableNightColorGrade", ClampMin = "0.0", ClampMax = "3.0", UIMin = "0.0", UIMax = "1.5", DisplayName = "밤 밝기", ToolTip = "최종 합성 단계의 밤 전용 밝기 배율입니다. originalLinear/PLY 원본 색상은 그대로 유지합니다."))
	float NightBrightness = 0.75f;

	/** Retained light in deep GS shadows after the daylight grade. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", DisplayName = "간접광 그림자 리프트", ClampMin = "0.0", ClampMax = "0.3"))
	float TargetShadowLift = 0.025f;

	/** Cool-neutral sky tint used by the relit ambient/indirect layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "02 조명 및 그림자", meta = (GSGroup = "03 씬 라이트", DisplayName = "간접광 색조"))
	FLinearColor TargetSkyTint = FLinearColor(0.74f, 0.86f, 1.0f, 1.0f);

	/** Suppresses likely baked sunlight before the new direct-light layer is added. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "03 색상 및 후처리", meta = (GSGroup = "05 디테일 및 선택 색상", DisplayName = "촬영 태양광 억제", ClampMin = "0.0", ClampMax = "1.0"))
	float TargetBakedSunSuppression = 0.65f;

	/** Scene-linear luminance where the smooth filmic shoulder begins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "05 디테일 및 선택 색상", DisplayName = "하이라이트 압축 시작", ClampMin = "0.08", ClampMax = "1.50"))
	float TargetFilmicShoulderStart = 0.48f;

	/** Smooth log-luminance shoulder strength. It compresses highlights without adding white energy back. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "05 디테일 및 선택 색상", DisplayName = "하이라이트 압축 강도", ClampMin = "0.0", ClampMax = "1.0"))
	float TargetFilmicShoulderStrength = 0.65f;

	/** Additional green chroma recovery after bright foliage luminance has been reduced. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "05 디테일 및 선택 색상", DisplayName = "녹색 채도 복원", ClampMin = "0.0", ClampMax = "1.0"))
	float TargetGreenSaturationRecovery = 0.15f;

	/** Reduces luminance in already-bright green foliage before selective color enhancement. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "05 디테일 및 선택 색상", DisplayName = "밝은 녹색 압축", ClampMin = "0.0", ClampMax = "1.0"))
	float TargetBrightGreenCompression = 0.18f;

	/** Extra luminance reduction for already-bright green foliage before saturation enhancement. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "05 디테일 및 선택 색상", DisplayName = "녹색 밝기 감소", ClampMin = "0.0", ClampMax = "1.0"))
	float TargetGreenLuminanceReduction = 0.28f;

	/** Accumulated-image local contrast strength. It is coverage-, shadow- and highlight-gated. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "05 디테일 및 선택 색상", DisplayName = "미세 로컬 대비", ClampMin = "0.0", ClampMax = "1.0"))
	float TargetMicroContrast = 0.33f;

	/** Broad three-pixel log-luminance contrast. This adds material weight without creating one-pixel halos. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "05 디테일 및 선택 색상", DisplayName = "넓은 로컬 대비", ClampMin = "0.0", ClampMax = "1.0"))
	float TargetCoarseContrast = 0.18f;

	/** Edge-aware accumulated-image sharpening. It is automatically reduced near GS coverage edges, highlights and deep shadows. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "05 디테일 및 선택 색상", DisplayName = "최종 선명도", ClampMin = "0.0", ClampMax = "1.0"))
	float TargetSplatSharpness = 0.05f;

	/** Protects compressed highlights from sharpening halos. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "05 디테일 및 선택 색상", DisplayName = "하이라이트 선명도 보호", ClampMin = "0.0", ClampMax = "1.0"))
	float TargetHighlightSharpenProtection = 0.90f;

	/** Suppresses sharpening where accumulated GS coverage changes abruptly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "03 색상 및 후처리", meta = (GSGroup = "05 디테일 및 선택 색상", DisplayName = "스플렛 경계 보호", ClampMin = "0.0", ClampMax = "1.0"))
	float TargetCoverageEdgeProtection = 0.90f;

	/** Diagnostic output for verifying that the final accumulated color pass is active. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "07 진단 및 고급")
	ELCCCinematicGSCompositeDebugView TargetCompositeDebugView = ELCCCinematicGSCompositeDebugView::Final;

	/** Overall strength of the GS-native aerial perspective and cinematic grade. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "03 대기 및 안개", ClampMin = "0.0", ClampMax = "2.0", DisplayName = "대기 효과 강도"))
	float AtmosphereStrength = 0.62f;

	/** Extra low-level haze near the bottom of the splat bounds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "03 대기 및 안개", ClampMin = "0.0", ClampMax = "2.0", DisplayName = "지면 안개 밀도"))
	float GroundFogDensity = 0.65f;

	/** How strongly distant splats blend into atmospheric color. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "03 대기 및 안개", ClampMin = "0.0", ClampMax = "2.0", DisplayName = "원거리 헤이즈"))
	float DistanceHaze = 0.48f;

	/** Controls how quickly fog density falls with height. Higher values keep fog close to the ground. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "03 대기 및 안개", ClampMin = "0.1", ClampMax = "6.0", DisplayName = "높이 밀도 감쇠"))
	float FogHeightFalloff = 1.35f;

	/** Low-frequency world-space density variation. This creates layered banks instead of a flat grey veil. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "03 대기 및 안개", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "안개 밀도 변화"))
	float FogDensityVariation = 0.28f;

	/** Warm forward-scattering color in sunlit haze. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "03 대기 및 안개", ClampMin = "0.0", ClampMax = "2.0", DisplayName = "따뜻한 태양 산란"))
	float WarmSunScatter = 0.70f;

	/** Blue-green separation in shadowed foliage and tree canopy. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "03 대기 및 안개", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "차가운 그림자 색조"))
	#endif
	UPROPERTY()
	float CoolShadowTint = 0.55f;

	/** Darkens low-light areas to create the under-canopy contrast seen in forest reference shots. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "04 그늘 부분", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "그늘 압축"))
	float ShadowCrush = 0.05f;

	/** Deprecated compatibility value; procedural dapple was removed because it produced floor banding. */
	float DappledLightStrength = 0.35f;

	/** Deprecated compatibility value, intentionally not reflected. */
	float DappledLightScale = 1.0f;

	/** Pushes bright sunlit haze above the base GS color so bloom has something to catch. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "06 블룸 및 글레어", ClampMin = "0.0", ClampMax = "5.0", DisplayName = "하이라이트 블룸 증폭"))
	float HighlightBloomBoost = 1.05f;

	/** Raises sunlit regions without lifting the entire scan. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "03 빛받는 부분", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "태양 노출 증폭"))
	float SunExposureBoost = 0.18f;

	/** Desaturates deep shadow slightly while retaining captured color detail. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "04 그늘 부분", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "기존 그늘 채도 감소"))
	float ShadowDesaturation = 0.20f;

	/** Reduces added atmospheric light on distant splats so the background does not wash out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "05 디테일 및 선택 색상", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "원거리 광량 감소"))
	float FarLightFade = 0.30f;

	/** Desaturates distant splats by camera distance, useful for softer aerial perspective. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "05 디테일 및 선택 색상", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "원거리 채도 감소"))
	float FarSaturationFade = 0.10f;

	/** Multiplies warm highlight glare, light shafts, and bloom-driving sun haze. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "05 디테일 및 선택 색상", ClampMin = "0.0", ClampMax = "3.0", DisplayName = "하이라이트 글레어"))
	float HighlightGlareScale = 0.85f;

	/** Restores vivid captured color, especially in sunlit foliage and sky-lit highlights. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "05 디테일 및 선택 색상", ClampMin = "0.0", ClampMax = "2.0", DisplayName = "채도 증폭"))
	float ColorSaturationBoost = 0.18f;

	/** Camera-distance range where GS aerial perspective begins, as a fraction of splat bounds radius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "03 대기 및 안개", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "GS 거리 안개 시작 (비율)"))
	float FogDistanceStart = 0.018f;

	/** Camera-distance range where GS aerial perspective reaches full strength, as a fraction of splat bounds radius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "03 대기 및 안개", ClampMin = "0.01", ClampMax = "2.0", DisplayName = "GS 거리 안개 끝 (비율)"))
	float FogDistanceEnd = 0.48f;

	/** Near-camera air range start, as a fraction of splat bounds radius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "03 대기 및 안개", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "GS 근거리 대기 시작 (비율)"))
	float NearAirStart = 0.010f;

	/** Near-camera air range end, as a fraction of splat bounds radius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "03 대기 및 안개", ClampMin = "0.01", ClampMax = "2.0", DisplayName = "GS 근거리 대기 끝 (비율)"))
	float NearAirEnd = 0.24f;

	/** How much camera distance contributes to haze. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "03 대기 및 안개", ClampMin = "0.0", ClampMax = "2.0", DisplayName = "거리 안개 가중치"))
	#endif
	UPROPERTY()
	float DistanceFogWeight = 0.44f;

	/** How much near-air distance contributes to haze. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "03 대기 및 안개", ClampMin = "0.0", ClampMax = "2.0", DisplayName = "근거리 안개 가중치"))
	#endif
	UPROPERTY()
	float NearFogWeight = 0.16f;

	/** Minimum haze density multiplier used by DistanceHaze. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "03 대기 및 안개", ClampMin = "0.0", ClampMax = "2.0", DisplayName = "최소 헤이즈 밀도"))
	#endif
	UPROPERTY()
	float HazeDensityMin = 0.16f;

	/** Maximum haze density multiplier used by DistanceHaze. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨", meta = (GSGroup = "03 대기 및 안개", ClampMin = "0.0", ClampMax = "2.0", DisplayName = "최대 헤이즈 밀도"))
	#endif
	UPROPERTY()
	float HazeDensityMax = 0.46f;

	/** Source color luma where local highlight bloom starts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "06 블룸 및 글레어", ClampMin = "0.0", ClampMax = "4.0", DisplayName = "하이라이트 밝기 하한"))
	float HighlightLumaGateLow = 0.66f;

	/** Source color luma where local highlight bloom reaches full strength. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "06 블룸 및 글레어", ClampMin = "0.0", ClampMax = "4.0", DisplayName = "하이라이트 밝기 상한"))
	float HighlightLumaGateHigh = 1.02f;

	/** Light-mask value where sunlit haze begins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "06 블룸 및 글레어", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "태양 마스크 하한"))
	float SunMaskGateLow = 0.48f;

	/** Light-mask value where sunlit haze reaches full strength. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "06 블룸 및 글레어", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "태양 마스크 상한"))
	float SunMaskGateHigh = 0.94f;

	/** Light-mask value where local bloom begins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "06 블룸 및 글레어", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "로컬 블룸 마스크 하한"))
	float LocalBloomMaskLow = 0.68f;

	/** Light-mask value where local bloom reaches full strength. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "06 블룸 및 글레어", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "로컬 블룸 마스크 상한"))
	float LocalBloomMaskHigh = 0.98f;

	/** Strength of bloom added only to bright sunlit highlights. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "06 블룸 및 글레어", ClampMin = "0.0", ClampMax = "8.0", DisplayName = "로컬 블룸 강도"))
	float LocalBloomStrength = 0.42f;

	/** Legacy secondary bloom multiplier. Hidden because HighlightBloomBoost and LocalBloomStrength now own the unified path. */
	UPROPERTY()
	float LocalBloomGlareMix = 0.22f;

	/** Near-air weight for the warm glare layer. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "06 블룸 및 글레어", ClampMin = "0.0", ClampMax = "2.0"))
	#endif
	UPROPERTY()
	float LitAirNearWeight = 0.18f;

	/** Far-distance weight for the warm glare layer. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "06 블룸 및 글레어", ClampMin = "0.0", ClampMax = "2.0"))
	#endif
	UPROPERTY()
	float LitAirFarWeight = 0.34f;

	/** How much fog amount feeds broad sun haze glow. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "06 블룸 및 글레어", ClampMin = "0.0", ClampMax = "3.0"))
	#endif
	UPROPERTY()
	float HazeGlowStrength = 0.38f;

	/** How much lit-air amount feeds broad sun haze glow. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "06 블룸 및 글레어", ClampMin = "0.0", ClampMax = "3.0"))
	#endif
	UPROPERTY()
	float LitAirGlowStrength = 0.34f;

	/** Tonal separation applied only to splats identified as receiving direct sunlight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "06 블룸 및 글레어", DisplayName = "빛받는 부분 로컬 대비", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "1.0"))
	float LightShaftBaseStrength = 0.42f;

	/** Bounded highlight lift applied only inside the direct-sun mask. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리",
		meta = (GSGroup = "06 블룸 및 글레어", DisplayName = "빛받는 부분 하이라이트 분리", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "1.0"))
	float LightShaftHighlightStrength = 0.32f;
	#if 0

	/** 카메라 광선상에서 프록시 Light Depth를 적분해 체적광을 생성합니다. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "05 체적광", DisplayName = "프록시 체적광 사용"))
	#endif
	UPROPERTY()
	bool bEnableProxyVolumetricShafts = true;
	#if 0

	/** 최종 선형 HDR 체적광 강도입니다. 2를 넘겨 색을 태우지 않도록 제한됩니다. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "05 체적광", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "1.0",
			DisplayName = "체적광 강도"))
	#endif
	UPROPERTY()
	float ProxyVolumetricShaftStrength = 0.18f;
	#if 0

	/** 카메라 광선에서 누적되는 공기 산란 밀도입니다. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "05 체적광", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "1.5",
			DisplayName = "체적광 밀도"))
	#endif
	UPROPERTY()
	float ProxyVolumetricShaftDensity = 0.65f;
	#if 0

	/** 카메라에서 체적광을 적분하는 최대 거리(cm)입니다. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "05 체적광", ClampMin = "100.0", ClampMax = "50000.0", UIMin = "1000.0", UIMax = "30000.0",
			DisplayName = "체적광 최대 거리 (cm)"))
	#endif
	UPROPERTY()
	float ProxyVolumetricShaftMaxDistance = 12000.0f;
	#if 0

	/** 카메라가 태양 방향을 바라볼 때 산란이 집중되는 정도입니다. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "05 체적광", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "전방 산란 집중도"))
	#endif
	UPROPERTY()
	float ProxyVolumetricShaftForwardScattering = 0.65f;
	#if 0

	/** 체적광의 선형 HDR 색상입니다. 휘도는 내부에서 정규화됩니다. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "04 환경 및 날씨",
		meta = (GSGroup = "05 체적광", DisplayName = "체적광 색상"))
	#endif
	UPROPERTY()
	FLinearColor ProxyVolumetricShaftTint = FLinearColor(1.0f, 0.86f, 0.62f, 1.0f);

	/** Base amount of sun lift added to sunlit areas. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "03 빛받는 부분", ClampMin = "0.0", ClampMax = "2.0"))
	#endif
	UPROPERTY()
	float SunLiftBase = 0.14f;

	/** Extra sun lift added by camera distance. */
	#if 0
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "03 색상 및 후처리", meta = (GSGroup = "03 빛받는 부분", ClampMin = "0.0", ClampMax = "2.0"))
	#endif
	UPROPERTY()
	float SunLiftDistance = 0.22f;

	/** Spherical Harmonic order to use for rendering (0-3). Higher = more color detail but slower. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "06 렌더링 및 성능", meta = (ClampMin = "0", ClampMax = "3"))
	int32 SHOrder = 3;

	/** Global opacity multiplier */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "06 렌더링 및 성능", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float OpacityScale = 1.0f;

	/** Scale multiplier for splat sizes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "06 렌더링 및 성능", meta = (ClampMin = "0.1", ClampMax = "10.0"))
	float SplatScale = 1.0f;

	/** Projected error threshold for LOD selection (resolution-independent, like Nanite).
	 *  Lower values = more conservative (keep detail longer, less LOD savings)
	 *  Higher values = more aggressive (switch to LOD sooner, better performance)
	 *  Uses projection-space units. ~0.03 ??32 pixels at 1080p with 90° FOV. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "06 렌더링 및 성능", meta = (ClampMin = "0.001", ClampMax = "1.0"))
	float LODErrorThreshold = 0.03f;

protected:
	FLCCCinematicGSSeasonPresetSlot* GetSeasonPresetSlot(ELCCCinematicGSSeasonMode Mode);
	void CaptureCurrentEffectsToSeason(ELCCCinematicGSSeasonMode Mode, bool bMarkAsUserSave = false);
	bool RestoreEffectsFromSeason(ELCCCinematicGSSeasonMode Mode);
	void RefreshCurrentSeasonPresetStatus();
	void RefreshNumberedSeasonPresetStatus();
	FLCCCinematicGSSeasonPresetSlot* GetWeatherPresetSlot(ELCCCinematicGSWeatherMode Mode);
	void CaptureCurrentEffectsToWeather(ELCCCinematicGSWeatherMode Mode, bool bMarkAsUserSave = false);
	bool RestoreEffectsFromWeather(ELCCCinematicGSWeatherMode Mode);
	void RefreshCurrentWeatherPresetStatus();
	void RefreshNumberedWeatherPresetStatus();
	FLCCCinematicGSSeasonPresetSlot* GetWeatherFogProfileSlot(ELCCCinematicGSWeatherMode Mode);
	void CaptureCurrentWeatherFogProfile(ELCCCinematicGSWeatherMode Mode, bool bMarkAsUserSave = false);
	bool RestoreWeatherFogProfile(ELCCCinematicGSWeatherMode Mode);
	void RefreshCurrentWeatherFogProfileStatus();

	/** Called when the asset changes */
	void OnAssetChanged();

	/** Mark the render state as dirty */
	void MarkRenderStateDirty();

	/** Collect active Unreal LightComponents and build the render-thread lighting payload. */
	void UpdateCachedSceneLighting();

	/** Experimental CPU proof: visualize proxy samples shadowed by world collision. */
	void UpdateProxyShadowProbeDebug(float DeltaTime);

#if WITH_EDITOR
	void OnAnyObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent);
#endif

	/** Called when the asset's data changes (e.g., Nanite enabled/disabled) */
	void OnAssetDataChanged(class UGaussianSplatAsset* ChangedAsset);

	/** Subscribe to asset change notifications */
	void SubscribeToAssetChanges();

	/** Unsubscribe from asset change notifications */
	void UnsubscribeFromAssetChanges();

private:
	FString BuildStableBindingCacheKey() const;
	bool SaveStableProxyBindingCache();
	/** Prevent nested editor tuning helpers from recreating the SceneProxy. */
	bool bSuppressRenderStateRecreationForEditorTuning = false;
	bool bEditorRealtimePropertyChangeActive = false;

	/** Component-local dense proxy-normal grid, packed as half4 (encoded normal RGB + confidence). */
	TArray<FFloat16Color> ProxyNormalGridData;
	FVector4f ProxyNormalGridMinCellSize = FVector4f(0, 0, 0, 1);
	FVector4f ProxyNormalGridDimensionsStatus = FVector4f(0, 0, 0, 0);

	/** Last completed binding only. A building result lives in StableBindingBuildContext until the whole asset is ready. */
	/** Immutable payload shared by the component, SceneProxy staging, and queued render upload. */
	TSharedPtr<const TArray<FStableProxySplatBindingGPU>, ESPMode::ThreadSafe> StableSplatBindings;
	TArray<FStableProxyVertexGPU> StableProxyVertices;
	TArray<FStableProxyTriangleGPU> StableProxyTriangles;
	/** Three authored vertex normals per source triangle, in Gaussian-component local space.
	 *  Indexed as TriangleID*3 + corner and interpolated with the existing stable barycentrics. */
	TArray<FVector4f> StableProxyTriangleVertexNormals;
	TSharedPtr<FStableProxyBindingBuildContext> StableBindingBuildContext;
	/** Retained immutable proxy BVH used only by the deterministic self-cast ray path. */
	TSharedPtr<FStableProxyBindingBuildContext> StableRayBVHContext;
	TArray<FVector4f> StableProxyRayProofs;
	/** Per welded proxy vertex: world-up rain visibility. Geometry-only and light independent. */
	TArray<float> StableProxyRainExposure;
	/** Persistent recursive EMA result. Never contains an unfiltered A/B midpoint. */
	TArray<FVector4f> StableProxyTemporalProofs;
	bool BuildStableCastProofsForDirection(const FVector3f& LocalDirection, TArray<FVector4f>& OutProofs) const;
	void ApplyStableCastTemporalFilter(const TArray<FVector4f>& RawProofs, const FVector3f& LocalDirection);
	FVector3f LastStableRayLightDirection = FVector3f::ZeroVector;
	uint64 LastStableRayBuildFrame = MAX_uint64;
    // Exact raw fields only; temporal history remains outside the cache.
    // FIFO capped at 16 entries (~4 MiB for the current 16,080-vertex proxy).
    TMap<FString, TArray<FVector4f>> PCVRRuntimeCastCache;
    TArray<FString> PCVRRuntimeCastOrder;
    void RebuildStableProxyRayVisibilityInternal(bool bNotifyRenderUpdate);
	uint32 StableRayProofRevision = 0u;
	uint32 LastSubmittedStableRayProofRevision = MAX_uint32;
	uint32 LastSubmittedStableBindingRevision = MAX_uint32;
	uint32 LastSubmittedStableFieldRequestRevision = MAX_uint32;

	/** Latest lighting payload sent to the render thread. */
	FGaussianSplatLightingData CachedLightingData;

	float ProxyShadowProbeDebugAccumulator = 0.0f;
	bool bProxyVertexDebugDrawn = false;
	bool bProxyVertexShadowClassified = false;
	bool bProxyShadowLightDirectionValid = false;
	bool bProxyShadowProbeSamplesBuilt = false;
	bool bProxyShadowProbeWorkActive = false;
	bool bProxyShadowProbeRebuildPending = true;
	FVector ProxyShadowLightDirection = FVector::ZeroVector;
	FVector ProxyShadowProbeCameraLocation = FVector::ZeroVector;
	TArray<FVector> ProxyShadowProbeWorldPositions;
	TArray<FVector> ProxyShadowProbeWorldNormals;
	TArray<FColor> ProxyShadowProbeColors;
	// CPU-reference classification volume used only by Shadow Mask preview.
	// 0=empty, 1=direct/lit, 2=self/backface/external shadow.
	TArray<int32> ProxyShadowProbeCandidates;
	int32 ProxyShadowProbeWorkCursor = 0;
	uint32 ProxyShadowProbeDirectLit = 0;
	uint32 ProxyShadowProbeSelfOrBackface = 0;
	uint32 ProxyShadowProbeExternalCast = 0;
	double ProxyShadowProbeWorkStartSeconds = 0.0;

	/** Cached bounds */
	mutable FBoxSphereBounds CachedBounds;
	mutable bool bBoundsCached = false;

	/** Delegate handle for asset change subscription */
	FDelegateHandle AssetChangedDelegateHandle;
#if WITH_EDITOR
	FDelegateHandle LightPropertyChangedDelegateHandle;
#endif
};
