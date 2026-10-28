// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianDataTypes.generated.h"

/**
 * Position data compression format
 */
UENUM(BlueprintType)
enum class EGaussianPositionFormat : uint8
{
	Float32		UMETA(DisplayName = "Float32 (12 bytes)"),
	Norm16		UMETA(DisplayName = "Norm16 (6 bytes)"),
	Norm11		UMETA(DisplayName = "Norm11 (4 bytes)"),
	Norm6		UMETA(DisplayName = "Norm6 (2 bytes)")
};

/**
 * Color/opacity data compression format
 */
UENUM(BlueprintType)
enum class EGaussianColorFormat : uint8
{
	Float32x4	UMETA(DisplayName = "Float32x4 (16 bytes)"),
	Float16x4	UMETA(DisplayName = "Float16x4 (8 bytes)"),
	Norm8x4		UMETA(DisplayName = "Norm8x4 (4 bytes)"),
	BC7			UMETA(DisplayName = "BC7 (~1 byte)")
};

/**
 * Spherical harmonics compression format
 */
UENUM(BlueprintType)
enum class EGaussianSHFormat : uint8
{
	Float32		UMETA(DisplayName = "Float32"),
	Float16		UMETA(DisplayName = "Float16"),
	Norm11		UMETA(DisplayName = "Norm11"),
	Norm6		UMETA(DisplayName = "Norm6"),
	Cluster4k	UMETA(DisplayName = "Cluster 4k"),
	Cluster8k	UMETA(DisplayName = "Cluster 8k"),
	Cluster16k	UMETA(DisplayName = "Cluster 16k"),
	Cluster32k	UMETA(DisplayName = "Cluster 32k"),
	Cluster64k	UMETA(DisplayName = "Cluster 64k")
};

/**
 * Quality level preset for import
 */
UENUM(BlueprintType)
enum class EGaussianQualityLevel : uint8
{
	VeryHigh	UMETA(DisplayName = "Very High (~48 bytes/splat)"),
	High		UMETA(DisplayName = "High (~24 bytes/splat)"),
	Medium		UMETA(DisplayName = "Medium (~12 bytes/splat)"),
	Low			UMETA(DisplayName = "Low (~8 bytes/splat)"),
	VeryLow		UMETA(DisplayName = "Very Low (~4 bytes/splat)")
};

/**
 * Raw splat data read from PLY file (CPU-side, before compression)
 * Total: ~248 bytes per splat
 */
USTRUCT()
struct FGaussianSplatData
{
	GENERATED_BODY()

	/** World position */
	FVector3f Position = FVector3f::ZeroVector;

	/** Orientation quaternion */
	FQuat4f Rotation = FQuat4f::Identity;

	/** 3D scale factors */
	FVector3f Scale = FVector3f::OneVector;

	/** Alpha opacity [0,1] */
	float Opacity = 1.0f;

	/** Spherical harmonic band 0 (base color, DC term) */
	FVector3f SH_DC = FVector3f::ZeroVector;

	/** Spherical harmonic bands 1-3 (15 coefficients, each RGB) */
	FVector3f SH[15];

	FGaussianSplatData()
	{
		for (int32 i = 0; i < 15; i++)
		{
			SH[i] = FVector3f::ZeroVector;
		}
	}

	/** Serialization */
	friend FArchive& operator<<(FArchive& Ar, FGaussianSplatData& Splat)
	{
		Ar << Splat.Position;
		Ar << Splat.Rotation;
		Ar << Splat.Scale;
		Ar << Splat.Opacity;
		Ar << Splat.SH_DC;
		for (int32 i = 0; i < 15; i++)
		{
			Ar << Splat.SH[i];
		}
		return Ar;
	}
};

/**
 * Per-frame view data computed by compute shader, used by vertex shader.
 * Total: 80 bytes per splat. Nonlinear target-look controls are intentionally
 * not duplicated per splat; they run once on the fully accumulated HDR image.
 */
USTRUCT()
struct FGaussianSplatViewData
{
    GENERATED_BODY()

    FVector4f ClipPosition = FVector4f::Zero();
    FVector3f TranslatedWorldPos = FVector3f::ZeroVector;
    float TranslatedWorldPosPad = 0.0f;
    uint32 PackedColorRG = 0;
    uint32 PackedColorBA = 0;
    uint32 PackedDirectRG = 0;
    uint32 PackedDirectBA = 0;
    FVector2f Axis1 = FVector2f::ZeroVector;
    FVector2f Axis2 = FVector2f::ZeroVector;
    uint32 ClusterID = 0;
    uint32 Padding = 0;
    uint32 StructPadding0 = 0;
    uint32 StructPadding1 = 0;
};

static_assert(sizeof(FGaussianSplatViewData) == 80,
	"FGaussianSplatViewData must match the 80-byte HLSL structured-buffer layout");

/** Camera-independent splat -> proxy-triangle binding used by the stable shadow-field path.
 *  GPU layout is uint2: x triangle index, y packed half2 barycentric (U,V), W=1-U-V.
 *  TriangleIndex == MAX_uint32 means processed-but-invalid. MAX_uint32-1 is reserved for unprocessed debug state. */
struct FStableProxySplatBindingGPU
{
    uint32 TriangleIndex = MAX_uint32;
    uint32 PackedBarycentric = 0u;
    // v2810: per-splat multi-triangle continuous proxy normal.
    uint32 PackedSmoothNormal = 0u;
    uint32 SmoothNormalValid = 0u;
};

static_assert(sizeof(FStableProxySplatBindingGPU) == 16,
    "FStableProxySplatBindingGPU must match StructuredBuffer<uint4>");

/** Stable proxy-surface vertex in Gaussian-component local space. */
USTRUCT()
struct FStableProxyVertexGPU
{
    GENERATED_BODY()
    FVector4f Position = FVector4f(0, 0, 0, 1);
};
static_assert(sizeof(FStableProxyVertexGPU) == 16,
    "FStableProxyVertexGPU must match StructuredBuffer<float4>");

/** Source proxy-triangle -> welded stable-vertex indices. v2745 packs a 10+10-bit octahedral
 *  proxy normal plus 6-bit local and 6-bit broad stable geometry-closure openness in W.
 *  W==0 means invalid/degenerate. */
USTRUCT()
struct FStableProxyTriangleGPU
{
    GENERATED_BODY()
    uint32 Vertex0 = MAX_uint32;
    uint32 Vertex1 = MAX_uint32;
    uint32 Vertex2 = MAX_uint32;
    uint32 Valid = 0u;
};
static_assert(sizeof(FStableProxyTriangleGPU) == 16,
    "FStableProxyTriangleGPU must match StructuredBuffer<uint4>");

/**
 * Chunk info for quantized/compressed data
 * Each chunk contains 256 splats with shared min/max bounds for dequantization
 * Total: 72 bytes per chunk
 */
USTRUCT()
struct FGaussianChunkInfo
{
	GENERATED_BODY()

	/** Position min/max per axis (for dequantization) */
	FVector2f PosMinMaxX = FVector2f::ZeroVector;
	FVector2f PosMinMaxY = FVector2f::ZeroVector;
	FVector2f PosMinMaxZ = FVector2f::ZeroVector;

	/** Color channel min/max (packed) */
	uint32 ColorMinMaxR = 0;
	uint32 ColorMinMaxG = 0;
	uint32 ColorMinMaxB = 0;
	uint32 ColorMinMaxA = 0;

	/** Scale min/max (packed) */
	uint32 ScaleMinMaxX = 0;
	uint32 ScaleMinMaxY = 0;
	uint32 ScaleMinMaxZ = 0;

	/** SH min/max (packed) */
	uint32 SHMinMaxR = 0;
	uint32 SHMinMaxG = 0;
	uint32 SHMinMaxB = 0;

	/** Serialization */
	friend FArchive& operator<<(FArchive& Ar, FGaussianChunkInfo& Chunk)
	{
		Ar << Chunk.PosMinMaxX;
		Ar << Chunk.PosMinMaxY;
		Ar << Chunk.PosMinMaxZ;
		Ar << Chunk.ColorMinMaxR;
		Ar << Chunk.ColorMinMaxG;
		Ar << Chunk.ColorMinMaxB;
		Ar << Chunk.ColorMinMaxA;
		Ar << Chunk.ScaleMinMaxX;
		Ar << Chunk.ScaleMinMaxY;
		Ar << Chunk.ScaleMinMaxZ;
		Ar << Chunk.SHMinMaxR;
		Ar << Chunk.SHMinMaxG;
		Ar << Chunk.SHMinMaxB;
		return Ar;
	}
};


/**
 * Lightweight render-thread lighting payload for Gaussian splat cinematic shading.
 * This is intentionally independent from UE deferred lighting because this plugin
 * renders splats in a custom post-opaque pass. The payload is filled from active
 * Unreal LightComponents and consumed by CalcViewData.usf.
 */
struct FGaussianSplatLightingData
{
	static constexpr int32 MaxLights = 8;

	uint32 bReceiveLighting = 0;
	uint32 LightCount = 0;
	float LightingInfluence = 0.62f;
	float Padding0 = 0.0f;

	// Directional-light angle response controls. NormalMode: 0 camera-facing, 1 raw, 2 world up, 3 hybrid up, 4 proxy, 5 hybrid proxy.
	float DirectionalResponse = 0.90f;
	float LightWrap = 0.10f;
	uint32 NormalMode = 5;
	float Padding1 = 0.0f;

	// xyz = ambient color, w = ambient intensity
	FVector4f AmbientColorIntensity = FVector4f(0.84f, 0.91f, 1.0f, 0.09f);
	// IndirectLightParams: x = sky/indirect strength, y = ground bounce,
	// z = shadow saturation, w = enable shadow lighting.
	FVector4f IndirectLightParams = FVector4f(0.07f, 0.04f, 0.82f, 0.0f);
	// rgb = shadow/indirect tint, w reserved.
	FVector4f IndirectShadowTint = FVector4f(0.78f, 0.86f, 1.0f, 0.0f);

	// v2749 unified stable proxy-light tuning. Closure is a final/top-priority lighting mask.
	// Duplicate strength/gamma/max-darkness
	// controls are folded into one density value for each shadow family.
	// Params0: x cast density, y geometry-closure density, z closure area, w closure softness.
	// Params1: x closed indirect floor, y derived closed bounce floor, z cast softness, w cast minimum.
	// Params2: x reflected/shadow bounce strength, y sky/indirect strength, z ground bounce, w reserved.
	// Params3: x stable sun scale, y global light influence, z ambient intensity, w reserved.
	// Params4: x = runtime night blend (0 day, 1 full night), y = horizon cast stability, z/w = Stable lighting-normal full/max proxy distance (cm).
	FVector4f StableLightingParams0 = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
	FVector4f StableLightingParams1 = FVector4f(0.42f, 0.0f, 0.75f, 0.08f);
	FVector4f StableLightingParams2 = FVector4f(0.26f, 0.07f, 0.04f, 0.0f);
	FVector4f StableLightingParams3 = FVector4f(0.42f, 0.72f, 0.14f, 0.0f);
	FVector4f StableLightingParams4 = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);

	// Approximate cinematic shadow/detail controls for the custom post-opaque splat path.
	// ShadowParams: x = shadow strength, y = contact shadow strength, z = light contrast, w = micro-normal strength.
	FVector4f ShadowParams = FVector4f(0.65f, 0.35f, 1.65f, 0.35f);
	// ShadowParams2: x = shadow softness, y = breakup/noise amount, z = enabled, w = reserved.
	FVector4f ShadowParams2 = FVector4f(0.18f, 0.18f, 1.0f, 0.0f);
	// ShadowBounds: xyz = component bounds center, w = horizontal radius.
	FVector4f ShadowBounds = FVector4f(0, 0, 0, 1);
	// ShadowHeight: x = min Z, y = max Z, z/w reserved.
	FVector4f ShadowHeight = FVector4f(0, 1, 0, 0);

	// Second-integration relighting controls.
	// RelightParams: x = enabled, y = original/relit blend, z = approximate albedo strength, w = baked-shadow lift.
	FVector4f RelightParams = FVector4f(1.0f, 0.58f, 0.58f, 0.10f);
	// RelightLookParams: x = baked-highlight reduction, y = target-look strength, z = selective green boost, w = global gamma exponent.
	FVector4f RelightLookParams = FVector4f(0.65f, 1.0f, 0.12f, 1.35f);
	// rgb = warm direct-sun tint, w = direct-light energy scale.
	FVector4f RelightSunTint = FVector4f(1.06f, 0.93f, 0.74f, 0.42f);
	// rgb = sky/ambient tint, w = final shadow lift used by the target daylight grade.
	FVector4f RelightSkyTint = FVector4f(0.74f, 0.86f, 1.0f, 0.025f);
	// x = baked-sun suppression, y = filmic shoulder strength, z = bright-green luminance compression, w = fine local contrast.
	FVector4f RelightDetailParams = FVector4f(0.65f, 0.65f, 0.18f, 0.33f);
	// x = legacy accumulated-image sharpen, y = plugin bloom soft-knee fraction,
	// z = master effects enabled, w = original 3DGS brightness.
	FVector4f RelightDetailParams2 = FVector4f(0.15f, 0.25f, 0.0f, 1.0f);
	// x = pre-gamma exposure EV, y = legacy colour-bloom strength mirror,
	// z = global saturation, w = shadow saturation retention.
	FVector4f RelightToneParams = FVector4f(0.0f, 1.0f, 1.65f, 0.95f);
	// x = filmic shoulder start, y = green saturation recovery, z = green luminance reduction, w = coarse local contrast.
	FVector4f RelightHighlightParams = FVector4f(0.48f, 0.15f, 0.28f, 0.18f);
	// x = final sharpen strength, y = highlight sharpen protection, z = coverage-edge protection, w = composite diagnostic view.
	FVector4f RelightSharpenParams = FVector4f(0.05f, 0.90f, 0.90f, 0.0f);
	// x = sunlit gamma, y = sunlit saturation, z = sunlit brightness, w = sunlit grade strength.
	FVector4f RegionalColorGradeParams0 = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
	// x = shadow gamma, y = shadow saturation, z = shadow brightness, w = shadow grade strength.
	FVector4f RegionalColorGradeParams1 = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
	// rgb = scene-linear time-of-day hue multiplier, w = blend strength. Luminance is preserved in shader.
	FVector4f TimeOfDayGradeTint = FVector4f(1.0f, 1.0f, 1.0f, 0.0f);

	// Component-local proxy normal controls: x enabled, y blend strength, z confidence threshold, w foliage strength.
	FVector4f ProxyNormalParams = FVector4f(1.0f, 0.85f, 0.12f, 0.30f);


	// Gaussian-native additive lighting features. These are evaluated entirely in CalcViewData and do not read the proxy shadow capture.
	// SunAccentParams0: x enabled, y strength, z N.L start, w N.L full.
	FVector4f GaussianSunAccentParams0 = FVector4f(1.0f, 0.48f, 0.18f, 0.72f);
	// SunAccentParams1: x gamma, y tint strength, z highlight compression, w material detail retention.
	FVector4f GaussianSunAccentParams1 = FVector4f(0.85f, 0.18f, 0.55f, 0.88f);
	FVector4f GaussianSunAccentTint = FVector4f(1.0f, 0.92f, 0.76f, 0.0f);
	// PlantTransmissionParams0: x enabled, y strength, z backlight start, w backlight full.
	FVector4f GaussianPlantTransmissionParams0 = FVector4f(1.0f, 0.32f, 0.08f, 0.70f);
	// PlantTransmissionParams1: x gamma, y tint strength, z saturation, w view-phase power.
	FVector4f GaussianPlantTransmissionParams1 = FVector4f(0.82f, 0.35f, 1.10f, 2.0f);
	FVector4f GaussianPlantTransmissionTint = FVector4f(1.0f, 0.67f, 0.22f, 0.0f);

	// Cluster-cached spatial de-lighting controls. Disabled by default after the v1.0.x experiment.
	// x enabled, y strength, z chromatic-light removal strength, w minimum lighting factor.
	FVector4f SpatialDelightingParams0 = FVector4f(0.0f, 0.55f, 0.12f, 0.60f);
	// x maximum lighting factor, y dark protection, z maximum approximate-albedo luminance, w diagnostic mode.
	FVector4f SpatialDelightingParams1 = FVector4f(1.55f, 0.70f, 1.20f, 0.0f);
	// x foliage protection, y maximum tint scale, z difference diagnostic gain, w reserved.
	FVector4f SpatialDelightingParams2 = FVector4f(0.65f, 1.35f, 4.0f, 0.0f);
	// x evidence start EV, y evidence full EV, z maximum correction weight, w foliage correction scale.
	FVector4f SpatialDelightingParams3 = FVector4f(0.10f, 0.65f, 0.50f, 0.35f);
	// x maximum baked-shadow lift, y minimum baked-highlight retention, z signed-difference EV range, w reserved.
	FVector4f SpatialDelightingParams4 = FVector4f(1.25f, 0.75f, 1.0f, 0.0f);

	// GS-native atmosphere/grade controls.
	// AtmosphereParams: x = strength, y = low-ground density, z = distance haze, w = warm sun scatter.
	FVector4f AtmosphereParams = FVector4f(0.75f, 0.65f, 0.70f, 0.70f);
	// AtmosphereGradeParams: x = cool shadow tint, y = shadow crush, z = dappled light, w = dappled scale.
	FVector4f AtmosphereGradeParams = FVector4f(0.55f, 0.35f, 0.35f, 1.0f);
	// AtmosphereHighlightParams: x = highlight bloom push, y = sun exposure, z = shadow saturation, w = warm sun scatter.
	FVector4f AtmosphereHighlightParams = FVector4f(1.05f, 0.20f, 0.20f, 0.70f);
	// AtmosphereDistanceParams: x = far light fade, y = far saturation fade, z = highlight glare scale, w = color saturation boost.
	FVector4f AtmosphereDistanceParams = FVector4f(0.30f, 0.10f, 0.85f, 0.18f);
	// AtmosphereFogShapeParams: x/y = camera distance start/end scale, z/w = near-air start/end scale.
	FVector4f AtmosphereFogShapeParams = FVector4f(0.018f, 0.48f, 0.010f, 0.24f);
	// AtmosphereFogAmountParams: x = distance fog weight, y = near fog weight, z/w = haze density min/max.
	FVector4f AtmosphereFogAmountParams = FVector4f(0.44f, 0.16f, 0.16f, 0.46f);
	// AtmosphereHighlightShapeParams: x/y = highlight luma gate, z/w = sun light-mask gate.
	FVector4f AtmosphereHighlightShapeParams = FVector4f(0.66f, 1.02f, 0.48f, 0.94f);
	// AtmosphereBloomParams: x/y = local bloom light-mask gate, z = local bloom strength, w = glare contribution.
	FVector4f AtmosphereBloomParams = FVector4f(0.68f, 0.98f, 0.42f, 0.22f);
	// CompositeBloomParams: x = spatial spread in pixels, y = scatter strength,
	// z = source threshold, w = directional-light colour influence.
	FVector4f CompositeBloomParams = FVector4f(24.0f, 3.0f, 0.42f, 1.0f);
	// CompositeColorBloomParams: x = colour bloom strength, y = peak-channel threshold,
	// z = minimum saturation, w = saturation feather width.
	FVector4f CompositeColorBloomParams = FVector4f(1.0f, 0.25f, 0.20f, 0.15f);
	// AtmosphereGlareParams: x/y = lit-air near/far weights, z = haze glow strength, w = lit-air glow strength.
	FVector4f AtmosphereGlareParams = FVector4f(0.18f, 0.34f, 0.38f, 0.34f);
	// AtmosphereShaftParams: x = shaft base strength, y = shaft highlight strength, z/w = sun lift base/distance.
	FVector4f AtmosphereShaftParams = FVector4f(0.18f, 0.20f, 0.14f, 0.22f);

	// Seasonal range remap runs on the trained color before lighting/fog.
	// x = enabled, y = RGB tolerance, z = feather, w = minimum saturation.
	FVector4f SeasonalColorSelectParams = FVector4f(0.0f, 0.22f, 0.08f, 0.10f);
	// x = saturation scale, y = value scale, z = replacement strength, w = debug mode.
	FVector4f SeasonalColorAdjustParams = FVector4f(1.0f, 1.0f, 0.0f, 0.0f);
	FVector4f SeasonalSourceColorFrom = FVector4f(0.03f, 0.10f, 0.015f, 0.0f);
	FVector4f SeasonalSourceColorTo = FVector4f(0.62f, 0.78f, 0.20f, 0.0f);
	FVector4f SeasonalTargetColorFrom = FVector4f(0.45f, 0.08f, 0.015f, 0.0f);
	FVector4f SeasonalTargetColorTo = FVector4f(1.00f, 0.66f, 0.04f, 0.0f);

	// xyz = world position, w = attenuation radius. Directional lights use w = 0.
	FVector4f LightPositionRadius[MaxLights];

	// xyz = direction. For directional lights this is surface->light. For spot lights this is light emission direction. w = type.
	// type: 0 directional, 1 point, 2 spot, 3 rect-as-local.
	FVector4f LightDirectionType[MaxLights];

	// rgb = linear light color, w = normalized intensity multiplier for the splat shader.
	FVector4f LightColorIntensity[MaxLights];

	// x = cos(inner cone), y = cos(outer cone), z/w reserved. Used by spot lights.
	FVector4f LightConeAngles[MaxLights];

	void Reset()
	{
		bReceiveLighting = 0;
		LightCount = 0;
		LightingInfluence = 0.62f;
		Padding0 = 0.0f;
		DirectionalResponse = 0.90f;
		LightWrap = 0.10f;
		NormalMode = 5;
		Padding1 = 0.0f;
		AmbientColorIntensity = FVector4f(0.84f, 0.91f, 1.0f, 0.09f);
		IndirectLightParams = FVector4f(0.07f, 0.04f, 0.82f, 0.0f);
		IndirectShadowTint = FVector4f(0.78f, 0.86f, 1.0f, 0.0f);
		StableLightingParams0 = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
		StableLightingParams1 = FVector4f(0.42f, 0.565f, 0.75f, 0.08f);
		StableLightingParams2 = FVector4f(0.26f, 0.07f, 0.04f, 0.0f);
		StableLightingParams3 = FVector4f(0.42f, 0.72f, 0.14f, 0.0f);
		StableLightingParams4 = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
		ShadowParams = FVector4f(0.65f, 0.35f, 1.65f, 0.35f);
		ShadowParams2 = FVector4f(0.18f, 0.18f, 1.0f, 0.0f);
		ShadowBounds = FVector4f(0, 0, 0, 1);
		ShadowHeight = FVector4f(0, 1, 0, 0);
		RelightParams = FVector4f(1.0f, 0.58f, 0.58f, 0.10f);
		RelightLookParams = FVector4f(0.65f, 1.0f, 0.12f, 1.35f);
		RelightSunTint = FVector4f(1.06f, 0.93f, 0.74f, 0.42f);
		RelightSkyTint = FVector4f(0.74f, 0.86f, 1.0f, 0.025f);
		RelightDetailParams = FVector4f(0.65f, 0.65f, 0.18f, 0.33f);
		RelightDetailParams2 = FVector4f(0.15f, 0.25f, 0.0f, 1.0f);
		RelightToneParams = FVector4f(0.0f, 1.0f, 1.65f, 0.95f);
		RelightHighlightParams = FVector4f(0.48f, 0.15f, 0.28f, 0.18f);
		RelightSharpenParams = FVector4f(0.05f, 0.90f, 0.90f, 0.0f);
		RegionalColorGradeParams0 = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
		RegionalColorGradeParams1 = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
		TimeOfDayGradeTint = FVector4f(1.0f, 1.0f, 1.0f, 0.0f);
		ProxyNormalParams = FVector4f(1.0f, 0.85f, 0.12f, 0.30f);

		GaussianSunAccentParams0 = FVector4f(1.0f, 0.24f, 0.18f, 0.72f);
		GaussianSunAccentParams1 = FVector4f(0.85f, 0.18f, 0.55f, 0.88f);
		GaussianSunAccentTint = FVector4f(1.0f, 0.92f, 0.76f, 0.0f);
		GaussianPlantTransmissionParams0 = FVector4f(1.0f, 0.18f, 0.08f, 0.70f);
		GaussianPlantTransmissionParams1 = FVector4f(0.82f, 0.35f, 1.10f, 2.0f);
		GaussianPlantTransmissionTint = FVector4f(1.0f, 0.67f, 0.22f, 0.0f);
		SpatialDelightingParams0 = FVector4f(0.0f, 0.55f, 0.12f, 0.60f);
		SpatialDelightingParams1 = FVector4f(1.55f, 0.70f, 1.20f, 0.0f);
		SpatialDelightingParams2 = FVector4f(0.65f, 1.35f, 4.0f, 0.0f);
		SpatialDelightingParams3 = FVector4f(0.10f, 0.65f, 0.50f, 0.35f);
		SpatialDelightingParams4 = FVector4f(1.25f, 0.75f, 1.0f, 0.0f);
		AtmosphereParams = FVector4f(0.75f, 0.65f, 0.70f, 0.70f);
		AtmosphereGradeParams = FVector4f(0.55f, 0.35f, 0.35f, 1.0f);
		AtmosphereHighlightParams = FVector4f(1.05f, 0.20f, 0.20f, 0.70f);
		AtmosphereDistanceParams = FVector4f(0.30f, 0.10f, 0.85f, 0.18f);
		AtmosphereFogShapeParams = FVector4f(0.018f, 0.48f, 0.010f, 0.24f);
		AtmosphereFogAmountParams = FVector4f(0.44f, 0.16f, 0.16f, 0.46f);
		AtmosphereHighlightShapeParams = FVector4f(0.66f, 1.02f, 0.48f, 0.94f);
		AtmosphereBloomParams = FVector4f(0.68f, 0.98f, 0.42f, 0.22f);
		CompositeBloomParams = FVector4f(24.0f, 3.0f, 0.42f, 1.0f);
		CompositeColorBloomParams = FVector4f(1.0f, 0.25f, 0.20f, 0.15f);
		AtmosphereGlareParams = FVector4f(0.18f, 0.34f, 0.38f, 0.34f);
		AtmosphereShaftParams = FVector4f(0.18f, 0.20f, 0.14f, 0.22f);
		SeasonalColorSelectParams = FVector4f(0.0f, 0.22f, 0.08f, 0.10f);
		SeasonalColorAdjustParams = FVector4f(1.0f, 1.0f, 0.0f, 0.0f);
		SeasonalSourceColorFrom = FVector4f(0.03f, 0.10f, 0.015f, 0.0f);
		SeasonalSourceColorTo = FVector4f(0.62f, 0.78f, 0.20f, 0.0f);
		SeasonalTargetColorFrom = FVector4f(0.45f, 0.08f, 0.015f, 0.0f);
		SeasonalTargetColorTo = FVector4f(1.00f, 0.66f, 0.04f, 0.0f);
		for (int32 i = 0; i < MaxLights; ++i)
		{
			LightPositionRadius[i] = FVector4f(0, 0, 0, 0);
			LightDirectionType[i] = FVector4f(0, 0, 1, 1);
			LightColorIntensity[i] = FVector4f(0, 0, 0, 0);
			LightConeAngles[i] = FVector4f(-1, -1, 0, 0);
		}
	}
};

/**
 * Constants for Gaussian Splatting
 */
namespace GaussianSplattingConstants
{
	/** Number of splats per chunk for quantization */
	constexpr int32 SplatsPerChunk = 256;

	/** Color texture width (Morton-swizzled) */
	constexpr int32 ColorTextureWidth = 2048;

	/** Morton tile size for texture swizzling */
	constexpr int32 MortonTileSize = 16;

	/** Number of SH coefficients per color channel (bands 1-3) */
	constexpr int32 NumSHCoefficients = 15;

	/** SH C0 coefficient for converting SH to color */
	constexpr float SH_C0 = 0.28209479177387814f;

	/** Maximum supported SH order (0-3) */
	constexpr int32 MaxSHOrder = 3;

	/** Log-scale encoding range for packed splat format (in centimeters) */
	constexpr float LN_SCALE_MIN = -8.0f;   // covers ~0.0003 cm
	constexpr float LN_SCALE_MAX = 14.0f;    // covers ~1.2 million cm

	/** Packed splat stride in bytes (4 x uint32) */
	constexpr int32 PackedSplatStride = 16;
}

/**
 * Helper functions for bit packing/unpacking
 */
namespace GaussianSplattingUtils
{
	/** Pack a float to 16-bit half float representation */
	inline uint16 FloatToHalf(float Value)
	{
		return FFloat16(Value).Encoded;
	}

	/** Unpack 16-bit half float to float */
	inline float HalfToFloat(uint16 Value)
	{
		FFloat16 Half;
		Half.Encoded = Value;
		return Half.GetFloat();
	}

	/** Pack two half floats into a uint32 */
	inline uint32 PackHalf2x16(float A, float B)
	{
		return (static_cast<uint32>(FloatToHalf(B)) << 16) | static_cast<uint32>(FloatToHalf(A));
	}

	/** Unpack uint32 to two floats */
	inline void UnpackHalf2x16(uint32 Packed, float& OutA, float& OutB)
	{
		OutA = HalfToFloat(static_cast<uint16>(Packed & 0xFFFF));
		OutB = HalfToFloat(static_cast<uint16>(Packed >> 16));
	}

	/** Encode 2D coordinate to Morton code (16x16 tile) */
	inline uint32 EncodeMorton2D_16x16(uint32 X, uint32 Y)
	{
		// Interleave bits of X and Y within 16x16 tile
		X = X & 0xF;
		Y = Y & 0xF;

		X = (X | (X << 2)) & 0x33;
		X = (X | (X << 1)) & 0x55;

		Y = (Y | (Y << 2)) & 0x33;
		Y = (Y | (Y << 1)) & 0x55;

		return X | (Y << 1);
	}

	/** Decode Morton code to 2D coordinate (16x16 tile) */
	inline void DecodeMorton2D_16x16(uint32 Morton, uint32& OutX, uint32& OutY)
	{
		uint32 X = Morton & 0x55;
		uint32 Y = (Morton >> 1) & 0x55;

		X = (X | (X >> 1)) & 0x33;
		X = (X | (X >> 2)) & 0x0F;

		Y = (Y | (Y >> 1)) & 0x33;
		Y = (Y | (Y >> 2)) & 0x0F;

		OutX = X;
		OutY = Y;
	}

	/** Convert splat index to pixel coordinates in color texture (Morton-swizzled) */
	inline void SplatIndexToTextureCoord(int32 SplatIndex, int32 TextureWidth, int32& OutX, int32& OutY)
	{
		const int32 TileSize = GaussianSplattingConstants::MortonTileSize;
		const int32 TilesPerRow = TextureWidth / TileSize;

		int32 TileIndex = SplatIndex / (TileSize * TileSize);
		int32 LocalIndex = SplatIndex % (TileSize * TileSize);

		int32 TileX = TileIndex % TilesPerRow;
		int32 TileY = TileIndex / TilesPerRow;

		uint32 LocalX, LocalY;
		DecodeMorton2D_16x16(LocalIndex, LocalX, LocalY);

		OutX = TileX * TileSize + LocalX;
		OutY = TileY * TileSize + LocalY;
	}

	/** Apply sigmoid function: 1 / (1 + exp(-x)) */
	inline float Sigmoid(float X)
	{
		return 1.0f / (1.0f + FMath::Exp(-X));
	}

	/** Convert SH DC component to base color */
	inline FVector3f SHDCToColor(const FVector3f& SHDC)
	{
		return FVector3f(
			0.5f + GaussianSplattingConstants::SH_C0 * SHDC.X,
			0.5f + GaussianSplattingConstants::SH_C0 * SHDC.Y,
			0.5f + GaussianSplattingConstants::SH_C0 * SHDC.Z
		);
	}

	/** Normalize quaternion */
	inline FQuat4f NormalizeQuat(const FQuat4f& Q)
	{
		float Length = FMath::Sqrt(Q.X * Q.X + Q.Y * Q.Y + Q.Z * Q.Z + Q.W * Q.W);
		if (Length > SMALL_NUMBER)
		{
			float InvLength = 1.0f / Length;
			return FQuat4f(Q.X * InvLength, Q.Y * InvLength, Q.Z * InvLength, Q.W * InvLength);
		}
		return FQuat4f::Identity;
	}

	/** Encode a linear scale value to uint8 using log encoding */
	inline uint8 EncodeScaleUint8(float Scale)
	{
		if (Scale < 1e-10f) return 0;  // sentinel for truly zero scale
		float LnScale = FMath::Loge(Scale);
		float Normalized = (LnScale - GaussianSplattingConstants::LN_SCALE_MIN)
			* 254.0f / (GaussianSplattingConstants::LN_SCALE_MAX - GaussianSplattingConstants::LN_SCALE_MIN);
		return (uint8)FMath::Clamp(FMath::RoundToInt(Normalized) + 1, 1, 255);
	}

	/** Encode quaternion to octahedral axis-angle representation (3 bytes: U, V, Angle) */
	inline void EncodeQuatOctahedral(FQuat4f Q, uint8& OutU, uint8& OutV, uint8& OutAngle)
	{
		// Normalize
		Q = NormalizeQuat(Q);

		// Ensure W >= 0 so theta is in [0, pi]
		if (Q.W < 0.0f)
		{
			Q.X = -Q.X; Q.Y = -Q.Y; Q.Z = -Q.Z; Q.W = -Q.W;
		}

		// Compute rotation angle
		float Theta = 2.0f * FMath::Acos(FMath::Clamp(Q.W, -1.0f, 1.0f));

		// Extract rotation axis
		float AxisLen = FMath::Sqrt(Q.X * Q.X + Q.Y * Q.Y + Q.Z * Q.Z);
		FVector3f Axis;
		if (AxisLen < 1e-8f)
		{
			Axis = FVector3f(1.0f, 0.0f, 0.0f);  // Arbitrary axis for identity rotation
		}
		else
		{
			float InvLen = 1.0f / AxisLen;
			Axis = FVector3f(Q.X * InvLen, Q.Y * InvLen, Q.Z * InvLen);
		}

		// Octahedral projection: map unit sphere to [-1,1]^2
		float AbsSum = FMath::Abs(Axis.X) + FMath::Abs(Axis.Y) + FMath::Abs(Axis.Z);
		float Px = Axis.X / AbsSum;
		float Py = Axis.Y / AbsSum;

		// Fold lower hemisphere
		if (Axis.Z < 0.0f)
		{
			float OrigPx = Px;
			float OrigPy = Py;
			Px = (1.0f - FMath::Abs(OrigPy)) * (OrigPx >= 0.0f ? 1.0f : -1.0f);
			Py = (1.0f - FMath::Abs(OrigPx)) * (OrigPy >= 0.0f ? 1.0f : -1.0f);
		}

		// Map [-1,1] to [0,1]
		float U = Px * 0.5f + 0.5f;
		float V = Py * 0.5f + 0.5f;

		// Quantize to uint8
		OutU = (uint8)FMath::Clamp(FMath::RoundToInt(U * 255.0f), 0, 255);
		OutV = (uint8)FMath::Clamp(FMath::RoundToInt(V * 255.0f), 0, 255);
		OutAngle = (uint8)FMath::Clamp(FMath::RoundToInt(Theta / UE_PI * 255.0f), 0, 255);
	}


	/**
	 * Pack a single splat into 4 uint32 words (16 bytes total).
	 * Layout matches SparkJS PackedSplats format.
	 * @param Position UE centimeter coordinates (float16 covers ±655m from origin)
	 * @param Rotation UE convention quaternion (X,Y,Z,W)
	 * @param Scale UE centimeter linear scale
	 * @param ColorR/G/B sRGB color [0,1] (clamped to uint8)
	 * @param Opacity Linear opacity [0,1]
	 */
	inline void PackSplatToUint4(
		const FVector3f& Position,
		const FQuat4f& Rotation,
		const FVector3f& Scale,
		float ColorR, float ColorG, float ColorB, float Opacity,
		uint32 OutWords[4])
	{
		// Word 0: R|G|B|A (uint8 each)
		uint8 R = (uint8)FMath::Clamp(FMath::RoundToInt(ColorR * 255.0f), 0, 255);
		uint8 G = (uint8)FMath::Clamp(FMath::RoundToInt(ColorG * 255.0f), 0, 255);
		uint8 B = (uint8)FMath::Clamp(FMath::RoundToInt(ColorB * 255.0f), 0, 255);
		uint8 A = (uint8)FMath::Clamp(FMath::RoundToInt(Opacity * 255.0f), 0, 255);
		OutWords[0] = (uint32)R | ((uint32)G << 8) | ((uint32)B << 16) | ((uint32)A << 24);

		// Word 1: PosX (float16) | PosY (float16)
		OutWords[1] = PackHalf2x16(Position.X, Position.Y);

		// Word 2: PosZ (float16) | QuatU (uint8) | QuatV (uint8)
		uint8 QuatU, QuatV, QuatAngle;
		EncodeQuatOctahedral(Rotation, QuatU, QuatV, QuatAngle);
		uint16 PosZHalf = FloatToHalf(Position.Z);
		OutWords[2] = (uint32)PosZHalf | ((uint32)QuatU << 16) | ((uint32)QuatV << 24);

		// Word 3: ScaleX|ScaleY|ScaleZ (uint8 log-encoded) | QuatAngle (uint8)
		uint8 SX = EncodeScaleUint8(Scale.X);
		uint8 SY = EncodeScaleUint8(Scale.Y);
		uint8 SZ = EncodeScaleUint8(Scale.Z);
		OutWords[3] = (uint32)SX | ((uint32)SY << 8) | ((uint32)SZ << 16) | ((uint32)QuatAngle << 24);
	}
}
