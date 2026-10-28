// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatActor.h"
#include "GaussianSplatComponent.h"
#include "GaussianPCVR.h"
#include "GaussianSplatRenderer.h"
#include "GaussianSplatViewExtension.h"
#include "GaussianSplatAsset.h"
#include "Materials/MaterialInterface.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/LocalPlayer.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Camera/CameraComponent.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/Material.h"
#include "RenderingThread.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Engine/Scene.h"
#include "Math/SHMath.h"
#include "DrawDebugHelpers.h"
#if WITH_EDITOR
#include "Editor.h"
#include "LevelEditorViewport.h"
#endif


static float SmoothCelestialBlend(float Edge0, float Edge1, float Value)
{
	if (Edge1 <= Edge0 + UE_KINDA_SMALL_NUMBER)
	{
		return Value >= Edge1 ? 1.0f : 0.0f;
	}
	const float T = FMath::Clamp((Value - Edge0) / (Edge1 - Edge0), 0.0f, 1.0f);
	return T * T * (3.0f - 2.0f * T);
}

// v2799: quintic smootherstep for authored time-of-day colour phases. The previous
// cubic windows were visually too compressed around the horizon and made the whole
// scene jump through blue/orange bands while the Directional Light was rotated.
static float SmootherCelestialBlend(float Edge0, float Edge1, float Value)
{
	if (Edge1 <= Edge0 + UE_KINDA_SMALL_NUMBER)
	{
		return Value >= Edge1 ? 1.0f : 0.0f;
	}
	const float T = FMath::Clamp((Value - Edge0) / (Edge1 - Edge0), 0.0f, 1.0f);
	return T * T * T * (T * (T * 6.0f - 15.0f) + 10.0f);
}

static float GetDirectionalHorizonPresence(const UDirectionalLightComponent* Light)
{
	if (!Light)
	{
		return 0.0f;
	}
	const FVector SurfaceToLight = (-Light->GetForwardVector()).GetSafeNormal();
	return SmoothCelestialBlend(-0.12f, 0.10f, (float)SurfaceToLight.Z);
}

static float GetDirectionalActiveScore(const UDirectionalLightComponent* Light)
{
	return Light ? FMath::Max(0.0f, Light->Intensity) * GetDirectionalHorizonPresence(Light) : 0.0f;
}

static float GetDirectionalDayNightAuthorityScore(const UDirectionalLightComponent* Light, const UGaussianSplatComponent* Component)
{
	if (!Light)
	{
		return -1.0e9f;
	}
	if (Component && Component->bEnableAutomaticDayNightCycle && Component->bRuntimeActiveCelestialDirectionValid)
	{
		const FVector SurfaceToLight = (-Light->GetForwardVector()).GetSafeNormal();
		const float Alignment = FVector::DotProduct(SurfaceToLight, Component->RuntimeActiveCelestialDirection.GetSafeNormal());
		return Alignment * 1000000000.0f + FMath::Min(FMath::Max(Light->Intensity, 0.0f), 1000.0f) * 0.001f;
	}
	return GetDirectionalActiveScore(Light);
}

static AGaussianSplatActor* FindPrimaryDayNightOwner(UWorld* World, AGaussianSplatActor* Fallback)
{
	AGaussianSplatActor* Owner = Fallback;
	if (!World || !Owner)
	{
		return Owner;
	}

	for (TActorIterator<AGaussianSplatActor> It(World); It; ++It)
	{
		AGaussianSplatActor* Candidate = *It;
		if (Candidate && Candidate->GetUniqueID() < Owner->GetUniqueID())
		{
			Owner = Candidate;
		}
	}
	return Owner;
}

// Weather post processing and Niagara composite timing are world-global render
// state. Let exactly one actor publish them. An actor with active weather wins
// over Clear actors so adding a second GS cannot clear Rain every other tick.
static AGaussianSplatActor* FindPrimaryWeatherOwner(UWorld* World, AGaussianSplatActor* Fallback)
{
	AGaussianSplatActor* Owner = Fallback;
	if (!World || !Owner)
	{
		return Owner;
	}

	auto WeatherPriority = [](const AGaussianSplatActor* Actor)
	{
		if (!Actor || !Actor->GaussianSplatComponent)
		{
			return -1;
		}
		switch (Actor->GaussianSplatComponent->WeatherMode)
		{
		case ELCCCinematicGSWeatherMode::Rain:
			// Prefer a fully configured, visible Rain actor. Old/hidden GS actors can
			// remain in the world after an asset replacement; choosing one of those by
			// UniqueID alone publishes Wet=off and the wrong Niagara composite order.
			return 300 +
				(Actor->GaussianSplatComponent->RainWeatherNiagara ? 20 : 0) +
				(!Actor->IsHidden() ? 4 : 0) +
				(Actor->GaussianSplatComponent->IsVisible() ? 2 : 0);
		case ELCCCinematicGSWeatherMode::Snow:
		case ELCCCinematicGSWeatherMode::Fog:
		case ELCCCinematicGSWeatherMode::Dawn: return 200;
		case ELCCCinematicGSWeatherMode::Clear:
		default: return 0;
		}
	};

	int32 OwnerPriority = WeatherPriority(Owner);
	for (TActorIterator<AGaussianSplatActor> It(World); It; ++It)
	{
		AGaussianSplatActor* Candidate = *It;
		const int32 CandidatePriority = WeatherPriority(Candidate);
		if (Candidate && (CandidatePriority > OwnerPriority ||
			(CandidatePriority == OwnerPriority && Candidate->GetUniqueID() < Owner->GetUniqueID())))
		{
			Owner = Candidate;
			OwnerPriority = CandidatePriority;
		}
	}
	return Owner;
}

static FLinearColor ScaleSkyColor(const FLinearColor& InColor, float Scale)
{
	const float S = FMath::Max(0.0f, Scale);
	return FLinearColor(InColor.R * S, InColor.G * S, InColor.B * S, InColor.A);
}

static FLinearColor NormalizeCapturedSkyTint(const FLinearColor& InColor)
{
	const float R = FMath::Max(InColor.R, 0.0f);
	const float G = FMath::Max(InColor.G, 0.0f);
	const float B = FMath::Max(InColor.B, 0.0f);
	const float MaxChannel = FMath::Max3(R, G, B);
	if (MaxChannel <= 1.0e-5f)
	{
		return FLinearColor::White;
	}

	return FLinearColor(
		FMath::Clamp(R / MaxChannel, 0.02f, 1.0f),
		FMath::Clamp(G / MaxChannel, 0.02f, 1.0f),
		FMath::Clamp(B / MaxChannel, 0.02f, 1.0f),
		1.0f);
}


struct FUnifiedPhysicalTODState
{
	float DirectEnergy = 1.0f;
	float SkyEnergy = 1.0f;
	float CarrierBrightness = 1.0f;
	float Gamma = 1.0f;
	float Saturation = 1.0f;
	float GradeStrength = 0.0f;
	float AfternoonLift = 0.0f;
	FLinearColor SunColor = FLinearColor::White;
	FLinearColor AmbientColor = FLinearColor(0.72f, 0.84f, 1.0f, 1.0f);
};

static float GaussianTODWeight(float Value, float Center, float Width)
{
	const float SafeWidth = FMath::Max(Width, 0.001f);
	const float X = (Value - Center) / SafeWidth;
	return FMath::Exp(-0.5f * X * X);
}

// One continuous solar-elevation model drives every visible time-of-day quantity.
// There are no morning/noon/evening lighting bands here. Above the horizon, direct
// energy follows solar altitude plus Kasten-Young relative optical air mass; below
// the horizon, twilight and deep-night energy decay continuously with solar depression.
static FUnifiedPhysicalTODState EvaluateUnifiedPhysicalTimeOfDay(
	float SolarElevationDegrees,
	bool bSolarRising,
	const UGaussianSplatComponent* Component)
{
	FUnifiedPhysicalTODState State;
	if (!Component)
	{
		return State;
	}

	const float Elevation = FMath::Clamp(SolarElevationDegrees, -90.0f, 90.0f);
	const float Depression = FMath::Max(-Elevation, 0.0f);
	const float PeakSunScale = FMath::Clamp(Component->UnifiedTODPeakSunScale, 0.1f, 1.5f);
	const float SolarPower = FMath::Clamp(Component->UnifiedTODSolarResponseExponent, 0.5f, 3.0f);
	const float Extinction = FMath::Clamp(Component->UnifiedTODAtmosphericExtinction, 0.0f, 0.8f);
	const float NightSkyFloor = FMath::Clamp(Component->UnifiedTODNightSkyFloor, 0.0f, 0.20f);
	const float NightBrightness = FMath::Clamp(Component->UnifiedTODNightBrightness, 0.20f, 0.90f);
	const float DayCarrierPeak = FMath::Clamp(Component->UnifiedTODDayCarrierPeak, 0.40f, 1.20f);
	const float AfternoonBoostStrength = FMath::Clamp(Component->UnifiedTODAfternoonBrightnessBoost, 0.0f, 0.8f);
	const float ColorStrength = FMath::Clamp(Component->UnifiedTODColorStrength, 0.0f, 1.5f);
	const float GammaStrength = FMath::Clamp(Component->UnifiedTODGammaStrength, 0.0f, 2.0f);

	float DirectSolarResponse = 0.0f;
	if (Elevation > 0.0f)
	{
		const float ElevationRad = FMath::DegreesToRadians(Elevation);
		const float SinElevation = FMath::Max(FMath::Sin(ElevationRad), 0.0f);
		const float ZenithDegrees = 90.0f - Elevation;
		const float ZenithRad = FMath::DegreesToRadians(ZenithDegrees);
		const float AirMassDenominator =
			FMath::Cos(ZenithRad) +
			0.50572f * FMath::Pow(FMath::Max(96.07995f - ZenithDegrees, 0.01f), -1.6364f);
		const float RelativeAirMass = 1.0f / FMath::Max(AirMassDenominator, 0.001f);
		const float AtmosphericTransmission = FMath::Exp(-Extinction * FMath::Max(RelativeAirMass - 1.0f, 0.0f));
		DirectSolarResponse = FMath::Clamp(
			FMath::Pow(SinElevation, SolarPower) * AtmosphericTransmission, 0.0f, 1.0f);
	}

	// v2825 unified afternoon arc. Keep the analytic solar response as the base, then
	// add one smooth artistic lift only on the setting half of the day: zero at 12:00,
	// peak near 14:00, back to zero at 16:00. There are no discrete lighting bands.
	// The same lift is exposed to the GS tone path so highlight compression/baked-light
	// suppression rise with brightness instead of letting pale surfaces wash to white.
	const float SolarTimeHours = bSolarRising
		? (Elevation + 90.0f) / 15.0f
		: 18.0f - Elevation / 15.0f;
	const float AfternoonRise = SmootherCelestialBlend(12.0f, 14.0f, SolarTimeHours);
	const float AfternoonFall = 1.0f - SmootherCelestialBlend(14.0f, 16.0f, SolarTimeHours);
	const float AfternoonCurve = bSolarRising ? 0.0f : FMath::Clamp(AfternoonRise * AfternoonFall, 0.0f, 1.0f);
	State.AfternoonLift = AfternoonCurve * AfternoonBoostStrength;

	const float BaseDirectEnergy = PeakSunScale * DirectSolarResponse;
	// Blend toward a restrained afternoon target instead of multiplying the falling
	// physical curve. This makes the visible peak stay near 14:00 and prevents a
	// runaway white-hot direct term while still letting 16:00 return to the base curve.
	const float AfternoonPeakDirect = PeakSunScale * (1.0f + AfternoonBoostStrength * 0.30f);
	State.DirectEnergy = FMath::Clamp(
		FMath::Lerp(BaseDirectEnergy, AfternoonPeakDirect, AfternoonCurve),
		0.0f, AfternoonPeakDirect);

	// Diffuse sky stays readable at low sun angles but no longer jumps to full daylight.
	const float DayDiffuse = NightSkyFloor +
		(1.0f - NightSkyFloor) * FMath::Pow(FMath::Clamp(DirectSolarResponse, 0.0f, 1.0f), 0.45f);
	const float TwilightDiffuse = 0.36f * FMath::Exp(-Depression / 5.4f);
	const float DeepNight = SmootherCelestialBlend(18.0f, 82.0f, Depression);
	const float DeepNightSkyFloor = NightSkyFloor * FMath::Lerp(1.0f, 0.58f, DeepNight);
	State.SkyEnergy = FMath::Clamp(FMath::Max3(
		Elevation > 0.0f ? DayDiffuse : DeepNightSkyFloor,
		TwilightDiffuse,
		DeepNightSkyFloor), 0.0f, 1.0f);

	// The photographed 3DGS carrier follows diffuse daylight continuously. This is the
	// part that keeps 07:00-09:00 visibly below noon instead of making it full daylight.
	const float ReadableDaylight = FMath::Pow(FMath::Clamp(State.SkyEnergy, 0.0f, 1.0f), 0.58f);
	State.CarrierBrightness = FMath::Lerp(NightBrightness, DayCarrierPeak, ReadableDaylight);
	// Only a restrained midtone lift follows the afternoon arc. Most of the perceived
	// brightness comes from direct energy; limiting carrier lift preserves texture/color.
	State.CarrierBrightness += State.AfternoonLift * 0.055f;
	State.CarrierBrightness *= FMath::Lerp(1.0f, 0.86f, DeepNight);
	const float AfternoonCarrierCeiling = DayCarrierPeak + AfternoonBoostStrength * 0.055f;
	State.CarrierBrightness = FMath::Clamp(State.CarrierBrightness, 0.25f, FMath::Max(AfternoonCarrierCeiling, 0.25f));

	State.Gamma = FMath::Clamp(
		1.0f + GammaStrength * ((1.0f - State.CarrierBrightness) * 0.62f + DeepNight * 0.10f),
		0.80f, 1.55f);

	const float HorizonWarmWeight = GaussianTODWeight(Elevation, 2.0f, 13.0f);
	const float BlueHourWeight = GaussianTODWeight(Elevation, -8.0f, 5.5f);
	const float NightColorWeight = SmootherCelestialBlend(3.0f, 24.0f, Depression);

	const FLinearColor DaySkyColor(0.72f, 0.84f, 1.00f, 1.0f);
	const FLinearColor HorizonColor = bSolarRising
		? FLinearColor(1.00f, 0.70f, 0.50f, 1.0f)
		: FLinearColor(1.00f, 0.48f, 0.24f, 1.0f);
	const FLinearColor BlueHourColor = bSolarRising
		? FLinearColor(0.47f, 0.54f, 0.84f, 1.0f)
		: FLinearColor(0.30f, 0.42f, 0.78f, 1.0f);
	const FLinearColor NavyColor(0.13f, 0.18f, 0.34f, 1.0f);
	const FLinearColor MidnightColor(0.055f, 0.067f, 0.105f, 1.0f);

	FLinearColor AmbientTint = DaySkyColor;
	AmbientTint = FMath::Lerp(
		AmbientTint, HorizonColor, FMath::Clamp(HorizonWarmWeight * 0.78f * ColorStrength, 0.0f, 1.0f));
	AmbientTint = FMath::Lerp(
		AmbientTint, BlueHourColor, FMath::Clamp(BlueHourWeight * 0.90f * ColorStrength, 0.0f, 1.0f));
	const FLinearColor DeepNightColor = FMath::Lerp(NavyColor, MidnightColor, DeepNight);
	AmbientTint = FMath::Lerp(
		AmbientTint, DeepNightColor, FMath::Clamp(NightColorWeight * ColorStrength, 0.0f, 1.0f));
	State.AmbientColor = NormalizeCapturedSkyTint(AmbientTint);

	const FLinearColor NeutralSun(1.00f, 0.96f, 0.86f, 1.0f);
	const FLinearColor LowSun = bSolarRising
		? FLinearColor(1.00f, 0.66f, 0.44f, 1.0f)
		: FLinearColor(1.00f, 0.50f, 0.26f, 1.0f);
	const float LowSunWarmth = FMath::Clamp(FMath::Exp(-FMath::Max(Elevation, 0.0f) / 18.0f) * ColorStrength, 0.0f, 1.0f);
	State.SunColor = NormalizeCapturedSkyTint(FMath::Lerp(NeutralSun, LowSun, LowSunWarmth));

	State.Saturation = FMath::Clamp(
		1.0f - ColorStrength * (BlueHourWeight * 0.10f + DeepNight * 0.38f), 0.48f, 1.05f);
	State.GradeStrength = FMath::Clamp(
		FMath::Max3(HorizonWarmWeight * 0.62f, BlueHourWeight * 0.82f, NightColorWeight * 0.92f) * ColorStrength,
		0.0f, 1.0f);
	return State;
}

static TAutoConsoleVariable<int32> CVarProxyCaptureDiagnostics(
	TEXT("gs.ProxyCaptureDiagnostics"),
	0,
	TEXT("Log proxy shadow capture invalidation reasons.\n")
	TEXT(" 0: Off (default)\n")
	TEXT(" 1: Log camera-matte, secondary-camera and light-depth capture updates"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarCastDepthDataLog(
	TEXT("gs.CastDepthDataLog"),
	0,
	TEXT("Read back current Combined/Receiver LightDepth and append machine-readable diagnostics.\n")
	TEXT(" 0: Off (default, no readback or extra memory)\n")
	TEXT(" 1: Log and append Saved/Diagnostics/GS_CastDepthData.csv"),
	ECVF_Default);

static void RecordCastDepthData(
	UTextureRenderTarget2D* CombinedTarget,
	UTextureRenderTarget2D* ReceiverTarget,
	uint32 Revision,
	const FRotator& LightRotation,
	const FVector& CaptureOrigin,
	const FVector& CaptureForward,
	float OrthoWidth)
{
	if (CVarCastDepthDataLog.GetValueOnGameThread() == 0 ||
		!CombinedTarget || !ReceiverTarget)
	{
		return;
	}

	FTextureRenderTargetResource* CombinedResource = CombinedTarget->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* ReceiverResource = ReceiverTarget->GameThread_GetRenderTargetResource();
	if (!CombinedResource || !ReceiverResource)
	{
		return;
	}

	const int32 Width = CombinedTarget->SizeX;
	const int32 Height = CombinedTarget->SizeY;
	const int32 SampleStride = 8;
	const int32 SampleWidth = FMath::DivideAndRoundUp(Width, SampleStride);
	const int32 SampleHeight = FMath::DivideAndRoundUp(Height, SampleStride);

	// R32f is not supported by ReadFloat16Pixels (PF_FloatRGBA only). Use the
	// generic linear readback and immediately compact Combined depth to the sampled
	// grid before reading Receiver, keeping peak diagnostic memory to one full image.
	TArray<FLinearColor> FullPixels;
	if (!CombinedResource->ReadLinearColorPixels(FullPixels) || FullPixels.Num() != Width * Height)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GSCastDepthData] Rev=%u R32f Combined readback failed Pixels=%d"),
			Revision, FullPixels.Num());
		return;
	}
	TArray<float> SampledBlockerDepth;
	SampledBlockerDepth.SetNumUninitialized(SampleWidth * SampleHeight);
	for (int32 SampleY = 0, Y = 0; Y < Height; Y += SampleStride, ++SampleY)
	{
		for (int32 SampleX = 0, X = 0; X < Width; X += SampleStride, ++SampleX)
		{
			SampledBlockerDepth[SampleY * SampleWidth + SampleX] = FullPixels[Y * Width + X].R;
		}
	}
	FullPixels.Reset();
	FullPixels.Shrink();
	if (!ReceiverResource->ReadLinearColorPixels(FullPixels) || FullPixels.Num() != Width * Height)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GSCastDepthData] Rev=%u R32f Receiver readback failed Pixels=%d"),
			Revision, FullPixels.Num());
		return;
	}
	int64 SampleCount = 0;
	int64 ReceiverValidCount = 0;
	int64 BlockerValidCount = 0;
	int64 PairValidCount = 0;
	int64 ShadowSideCount = 0;
	int64 NearBoundaryCount = 0;
	double DeltaSum = 0.0;
	double DeltaSqSum = 0.0;
	float DeltaMin = TNumericLimits<float>::Max();
	float DeltaMax = TNumericLimits<float>::Lowest();

	for (int32 SampleY = 0, Y = 0; Y < Height; Y += SampleStride, ++SampleY)
	{
		for (int32 SampleX = 0, X = 0; X < Width; X += SampleStride, ++SampleX)
		{
			const int32 Index = Y * Width + X;
			const int32 SampleIndex = SampleY * SampleWidth + SampleX;
			if (!FullPixels.IsValidIndex(Index) || !SampledBlockerDepth.IsValidIndex(SampleIndex))
			{
				continue;
			}
			++SampleCount;
			const float BlockerDepth = SampledBlockerDepth[SampleIndex];
			const float ReceiverDepth = FullPixels[Index].R;
			const bool bBlockerValid = BlockerDepth > 0.01f && BlockerDepth < 60000.0f;
			const bool bReceiverValid = ReceiverDepth > 0.01f && ReceiverDepth < 60000.0f;
			BlockerValidCount += bBlockerValid ? 1 : 0;
			ReceiverValidCount += bReceiverValid ? 1 : 0;
			if (!bBlockerValid || !bReceiverValid)
			{
				continue;
			}
			++PairValidCount;
			const float DeltaCm = BlockerDepth - ReceiverDepth;
			ShadowSideCount += DeltaCm < 0.0f ? 1 : 0;
			NearBoundaryCount += FMath::Abs(DeltaCm) <= 2.0f ? 1 : 0;
			DeltaMin = FMath::Min(DeltaMin, DeltaCm);
			DeltaMax = FMath::Max(DeltaMax, DeltaCm);
			DeltaSum += DeltaCm;
			DeltaSqSum += static_cast<double>(DeltaCm) * static_cast<double>(DeltaCm);
		}
	}

	const double Mean = PairValidCount > 0 ? DeltaSum / static_cast<double>(PairValidCount) : 0.0;
	const double Variance = PairValidCount > 0
		? FMath::Max(DeltaSqSum / static_cast<double>(PairValidCount) - Mean * Mean, 0.0)
		: 0.0;
	const double StdDev = FMath::Sqrt(Variance);
	if (PairValidCount == 0)
	{
		DeltaMin = 0.0f;
		DeltaMax = 0.0f;
	}

	const FString DiagnosticsDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Diagnostics"));
	IFileManager::Get().MakeDirectory(*DiagnosticsDir, true);
	const FString CsvPath = FPaths::Combine(DiagnosticsDir, TEXT("GS_CastDepthData.csv"));
	if (!IFileManager::Get().FileExists(*CsvPath))
	{
		FFileHelper::SaveStringToFile(
			TEXT("revision,pitch,yaw,roll,origin_x,origin_y,origin_z,forward_x,forward_y,forward_z,ortho_width,res_x,res_y,stride,samples,receiver_valid,blocker_valid,pair_valid,shadow_side,near_2cm,delta_min_cm,delta_mean_cm,delta_max_cm,delta_stddev_cm\n"),
			*CsvPath);
	}
	const FString CsvLine = FString::Printf(
		TEXT("%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.9f,%.9f,%.9f,%.3f,%d,%d,%d,%lld,%lld,%lld,%lld,%lld,%lld,%.6f,%.6f,%.6f,%.6f\n"),
		Revision, LightRotation.Pitch, LightRotation.Yaw, LightRotation.Roll,
		CaptureOrigin.X, CaptureOrigin.Y, CaptureOrigin.Z,
		CaptureForward.X, CaptureForward.Y, CaptureForward.Z,
		OrthoWidth, Width, Height, SampleStride,
		SampleCount, ReceiverValidCount, BlockerValidCount, PairValidCount,
		ShadowSideCount, NearBoundaryCount, DeltaMin, Mean, DeltaMax, StdDev);
	FFileHelper::SaveStringToFile(
		CsvLine, *CsvPath, FFileHelper::EEncodingOptions::AutoDetect,
		&IFileManager::Get(), FILEWRITE_Append);

	UE_LOG(LogTemp, Log,
		TEXT("[GSCastDepthData] Rev=%u Rot=(%.4f,%.4f,%.4f) Samples=%lld Pair=%lld Shadow=%lld Near2cm=%lld DeltaCm=(%.3f,%.3f,%.3f) Std=%.3f Csv=%s"),
		Revision, LightRotation.Pitch, LightRotation.Yaw, LightRotation.Roll,
		SampleCount, PairValidCount, ShadowSideCount, NearBoundaryCount,
		DeltaMin, Mean, DeltaMax, StdDev, *CsvPath);
}

static void SubmitStableShadowDebugMode(int32 DebugMode)
{
	// This is intentionally a render-thread-only diagnostic channel. Do not route
	// the selector through UGaussianSplatComponent dynamic data or SceneProxy state.
	ENQUEUE_RENDER_COMMAND(LCCGSSubmitStableShadowDebugMode)(
		[DebugMode](FRHICommandListImmediate& RHICmdList)
		{
			FGaussianSplatRenderer::SetStableShadowDebugMode(DebugMode);
		});
}

static EStableProxyShadowDebugMode NormalizeStableShadowDebugMode(EStableProxyShadowDebugMode Mode)
{
	// v2813: 82 is the valid mode-12 multi-triangle N.L diagnostic; never normalize it back to mode 02.
	const int32 Raw = static_cast<int32>(Mode);
	if (Raw == 0 || Raw == 72 || Raw == 73 || Raw == 79 || Raw == 80 || Raw == 81 || Raw == 82 || Raw == 102 || Raw == 103 || Raw == 104 || (Raw >= 74 && Raw <= 78))
	{
		return Mode;
	}

	// v2774 menu cleanup: old diagnostic values (04..26) are intentionally retired.
	// A level saved while one of those tests was selected returns to mode 02, because
	// mode 21's continuous cast shaping is now the default mode-02 shadow pass.
	return EStableProxyShadowDebugMode::StableProxyLightingMatch;
}


static UTextureRenderTarget2D* ResolveProxyRenderTarget(
	UObject* Owner,
	UTextureRenderTarget2D* CurrentTarget,
	FName ObjectName,
	ETextureRenderTargetFormat Format,
	const FLinearColor& ClearColor,
	int32 InitialSizeX,
	int32 InitialSizeY)
{
	if (!Owner || Owner->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_ClassDefaultObject))
	{
		return nullptr;
	}

	UTextureRenderTarget2D* Target = CurrentTarget;
	if (!Target)
	{
		// PIE/MRQ duplicates transient instanced objects before their TObjectPtr
		// properties are necessarily fixed up. Reattach the one fixed-name child
		// instead of trying to replace it or creating ever more unique objects.
		Target = FindObjectFast<UTextureRenderTarget2D>(Owner, ObjectName);
	}

	if (Target)
	{
		// A matching PIE duplicate exists, but CoreUObject has not completed its
		// load yet. Defer to a later Tick; never replace a half-loaded UObject.
		if (Target->HasAnyFlags(
				RF_NeedLoad | RF_NeedPostLoad | RF_BeginDestroyed | RF_FinishDestroyed))
		{
			return nullptr;
		}
		return Target;
	}

	Target = NewObject<UTextureRenderTarget2D>(
		Owner, ObjectName, RF_Transient);
	Target->RenderTargetFormat = Format;
	Target->ClearColor = ClearColor;
	Target->InitAutoFormat(
		FMath::Max(InitialSizeX, 1),
		FMath::Max(InitialSizeY, 1));
	Target->UpdateResourceImmediate(true);
	return Target;
}

static bool RequiresProxyShadowCaptures(ELCCCinematicGSLightingPreviewMode PreviewMode)
{
	switch (PreviewMode)
	{
	case ELCCCinematicGSLightingPreviewMode::ShadowMask:
	case ELCCCinematicGSLightingPreviewMode::BlockerDistance:
	case ELCCCinematicGSLightingPreviewMode::LightDepthCapture:
	case ELCCCinematicGSLightingPreviewMode::FinalShadowVisibility:
		return true;
	default:
		return false;
	}
}


static void UpdateTimeOfDayScreenOverlay(
	UGaussianSplatComponent* Component)
{
	if (!Component)
	{
		return;
	}

	const bool bVisible = Component->bEnableAutomaticDayNightCycle && Component->bShowTimeOfDaySceneOverlay;
	if (!bVisible || !GEngine)
	{
		return;
	}

	const FString OverlayText = FString::Printf(
		TEXT("TIME %s | %s | Sun %.1f deg | Pitch %.1f deg"),
		*Component->RuntimeTimeOfDayText,
		*Component->RuntimeTimeOfDayPhase,
		Component->RuntimeSolarElevationDegrees,
		Component->RuntimeDirectionalLightPitchDegrees);

	// v2827 screen-only clock. Keep only the compact engine screen overlay requested
	// by the user. This is drawn after the 3D scene, so GS color/lighting cannot tint it.
	const uint64 MessageKey = 0x4C4343475354494Dull; // "LCCGSTIM"
	GEngine->AddOnScreenDebugMessage(
		MessageKey,
		0.08f,
		FColor(255, 238, 70, 255),
		OverlayText,
		false,
		FVector2D(1.05f, 1.05f));
}

AGaussianSplatActor::AGaussianSplatActor()
{
	GaussianSplatComponent = CreateDefaultSubobject<UGaussianSplatComponent>(TEXT("GaussianSplatComponent"));
	RootComponent = GaussianSplatComponent;

	// v2831: Independent view-invariant snow splats (no UE procedural snow triangles).
	SnowGaussianComponent = CreateDefaultSubobject<UGaussianSplatComponent>(TEXT("SnowGaussianComponent"));
	SnowGaussianComponent->SetupAttachment(GaussianSplatComponent);
	SnowGaussianComponent->SetVisibility(false, false);
	SnowGaussianComponent->SetHiddenInGame(true);
	SnowGaussianComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SnowGaussianComponent->bReceiveUnrealLights = false; // snow SH1 uses local normal; avoid touching source GS lighting path
	SnowGaussianComponent->SHOrder = 1;
	SnowGaussianComponent->SplatScale = 1.0f;

	// P06: isolated Gaussian puddle diagnostic layer. The source and snow components
	// are unaffected; this component owns only its generated test asset.
	PuddleGaussianComponent = CreateDefaultSubobject<UGaussianSplatComponent>(TEXT("PuddleGaussianComponent"));
	PuddleGaussianComponent->SetupAttachment(GaussianSplatComponent);
	PuddleGaussianComponent->SetVisibility(false, false);
	PuddleGaussianComponent->SetHiddenInGame(true);
	PuddleGaussianComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PuddleGaussianComponent->bReceiveUnrealLights = false;
	PuddleGaussianComponent->SHOrder = 0;
	PuddleGaussianComponent->SplatScale = 1.0f;

	MoonDirectionalLightComponent = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("AutoMoonDirectionalLight"));
	MoonDirectionalLightComponent->SetupAttachment(GaussianSplatComponent);
	MoonDirectionalLightComponent->SetMobility(EComponentMobility::Movable);
	MoonDirectionalLightComponent->SetAtmosphereSunLight(true);
	MoonDirectionalLightComponent->SetAtmosphereSunLightIndex(1);
	MoonDirectionalLightComponent->SetAtmosphereSunDiskColorScale(FLinearColor(0.52f, 0.65f, 1.0f, 1.0f));
	MoonDirectionalLightComponent->SetCastShadows(true);
	MoonDirectionalLightComponent->SetIntensity(0.0f);
	MoonDirectionalLightComponent->SetVisibility(false, true);

	// This component is retained only for explicit UE-lit reference/debug display.
	// Production proxy coverage/depth uses the mesh data buffers and never SceneCapture2D.
	ProxyShadowReceiverComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ProxyShadowReceiverComponent"));
	ProxyShadowReceiverComponent->SetupAttachment(GaussianSplatComponent);
	ProxyShadowReceiverComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ProxyShadowReceiverComponent->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Ignore);
	ProxyShadowReceiverComponent->SetGenerateOverlapEvents(false);
	ProxyShadowReceiverComponent->SetCastShadow(false);
	ProxyShadowReceiverComponent->bCastHiddenShadow = false;
	ProxyShadowReceiverComponent->bReceivesDecals = false;
	ProxyShadowReceiverComponent->SetVisibility(false, true);
	ProxyShadowReceiverComponent->SetHiddenInGame(true);
	ProxyShadowReceiverComponent->SetRenderInMainPass(false);
	ProxyShadowReceiverComponent->SetRenderInDepthPass(false);
	ProxyShadowReceiverComponent->SetVisibleInSceneCaptureOnly(false);



	WeatherNiagaraComponent = CreateDefaultSubobject<UNiagaraComponent>(TEXT("WeatherNiagaraComponent"));
	WeatherNiagaraComponent->SetupAttachment(GaussianSplatComponent);
	WeatherNiagaraComponent->SetAutoActivate(false);

	WeatherSkyFogComponent = CreateDefaultSubobject<UExponentialHeightFogComponent>(TEXT("WeatherSkyFogComponent"));
	WeatherSkyFogComponent->SetupAttachment(GaussianSplatComponent);
	WeatherSkyFogComponent->SetVisibility(false);

	// P21: keep both fallback components invisible until we have confirmed that
	// this is the sole time-of-day owner AND no authored external sky exists.
	GaussianAutoSkyAtmosphere = CreateDefaultSubobject<USkyAtmosphereComponent>(TEXT("GaussianAutoSkyAtmosphere"));
	GaussianAutoSkyAtmosphere->SetupAttachment(GaussianSplatComponent);
	GaussianAutoSkyAtmosphere->SetVisibility(false);
	GaussianAutoVolumetricCloud = CreateDefaultSubobject<UVolumetricCloudComponent>(TEXT("GaussianAutoVolumetricCloud"));
	GaussianAutoVolumetricCloud->SetupAttachment(GaussianSplatComponent);
	GaussianAutoVolumetricCloud->SetVisibility(false);


	CinematicPostProcessComponent = CreateDefaultSubobject<UPostProcessComponent>(TEXT("CinematicPostProcessComponent"));
	CinematicPostProcessComponent->SetupAttachment(GaussianSplatComponent);
	CinematicPostProcessComponent->bUnbound = true;
	CinematicPostProcessComponent->Priority = 1000.0f;
	CinematicPostProcessComponent->BlendWeight = 0.0f;
	CinematicPostProcessComponent->bEnabled = false;


	ProxyLightDepthCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("ProxyLightDepthCapture"));
	ProxyLightDepthCapture->SetupAttachment(GaussianSplatComponent);
	ProxyLightDepthCapture->bCaptureEveryFrame = false;
	ProxyLightDepthCapture->bCaptureOnMovement = false;
	ProxyLightDepthCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	ProxyLightDepthCapture->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
	ProxyLightDepthCapture->ProjectionType = ECameraProjectionMode::Orthographic;
	ProxyLightDepthCapture->ShowFlags.SetPostProcessing(false);
	ProxyLightDepthCapture->ShowFlags.SetEyeAdaptation(false);
	ProxyLightDepthCapture->ShowFlags.SetTemporalAA(false);
	ProxyLightDepthCapture->ShowFlags.SetMotionBlur(false);

	ProxyReceiverLightDepthCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("ProxyReceiverLightDepthCapture"));
	ProxyReceiverLightDepthCapture->SetupAttachment(GaussianSplatComponent);
	ProxyReceiverLightDepthCapture->bCaptureEveryFrame = false;
	ProxyReceiverLightDepthCapture->bCaptureOnMovement = false;
	ProxyReceiverLightDepthCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	ProxyReceiverLightDepthCapture->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
	ProxyReceiverLightDepthCapture->ProjectionType = ECameraProjectionMode::Orthographic;
	ProxyReceiverLightDepthCapture->ShowFlags.SetPostProcessing(false);
	ProxyReceiverLightDepthCapture->ShowFlags.SetEyeAdaptation(false);
	ProxyReceiverLightDepthCapture->ShowFlags.SetTemporalAA(false);
	ProxyReceiverLightDepthCapture->ShowFlags.SetMotionBlur(false);

	ProxySurfaceDepthCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("ProxySurfaceDepthCapture"));
	ProxySurfaceDepthCapture->SetupAttachment(GaussianSplatComponent);
	ProxySurfaceDepthCapture->bCaptureEveryFrame = false;
	ProxySurfaceDepthCapture->bCaptureOnMovement = false;
	ProxySurfaceDepthCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	ProxySurfaceDepthCapture->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
	ProxySurfaceDepthCapture->ShowFlags.SetPostProcessing(false);
	ProxySurfaceDepthCapture->ShowFlags.SetEyeAdaptation(false);
	ProxySurfaceDepthCapture->ShowFlags.SetTemporalAA(false);
	ProxySurfaceDepthCapture->ShowFlags.SetMotionBlur(false);

	ProxySurfaceMaterialCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("ProxySurfaceMaterialCapture"));
	ProxySurfaceMaterialCapture->SetupAttachment(GaussianSplatComponent);
	ProxySurfaceMaterialCapture->bCaptureEveryFrame = false;
	ProxySurfaceMaterialCapture->bCaptureOnMovement = false;
	ProxySurfaceMaterialCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	ProxySurfaceMaterialCapture->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
	ProxySurfaceMaterialCapture->ShowFlags.SetPostProcessing(false);
	ProxySurfaceMaterialCapture->ShowFlags.SetEyeAdaptation(false);
	ProxySurfaceMaterialCapture->ShowFlags.SetTemporalAA(false);
	ProxySurfaceMaterialCapture->ShowFlags.SetMotionBlur(false);

	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	// Sequencer evaluates camera cuts and animated lights during the world update.
	// Capture after that evaluation so the texture and its projection metadata
	// describe the same camera/light state that the viewport renders this frame.
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
	PrimaryActorTick.bTickEvenWhenPaused = true;
}

void AGaussianSplatActor::PostLoad()
{
    Super::PostLoad();
    AppliedSnowHeightStage2878 = -1;
	// v2831: The generated asset is serialized as an actor-owned subobject.
	// Only the diagnostic string/count are transient; rebuild their display
	// when opening a saved level or an MRQ-duplicated world.
	if (GaussianSplatComponent && BakedSnowGaussianAsset && BakedSnowGaussianAsset->IsValid())
	{
		GaussianSplatComponent->SnowGaussianGeneratedCount = BakedSnowGaussianAsset->GetOriginalSplatCount();
		GaussianSplatComponent->SnowGaussianBuildStatus = FString::Printf(
			TEXT("저장된 Snow Gaussian 로드: %d points"),
			GaussianSplatComponent->SnowGaussianGeneratedCount);
	}

	// v2898: Baked snow assets are instanced subobjects of this actor.  They
	// cannot be true soft-loaded without changing the bake storage format, but
	// their expensive transient GPU ColorTexture can still be created only for
	// the selected stage.  Tag all existing stages BEFORE an MRQ/PIE duplicate
	// is made so each duplicated asset can make its PostLoad decision locally.
	const int32 SnowStageCount2898 = BakedSnowHeightStages2878.Num();
	if (SnowStageCount2898 > 0)
	{
		int32 PreferredStageIndex2898 = INDEX_NONE;
		const float RequestedHeight2898 = SnowBakedMaximumHeightCm2878 > KINDA_SMALL_NUMBER
			? FMath::Clamp(SnowRuntimeHeightCm2878, 0.0f, SnowBakedMaximumHeightCm2878)
			: FMath::Max(0.0f, SnowRuntimeHeightCm2878);
		float BestDistance2898 = RequestedHeight2898; // zero-height state is a valid winner
		for (int32 StageIndex2898 = 0; StageIndex2898 < SnowStageCount2898; ++StageIndex2898)
		{
			const float Distance2898 = FMath::Abs(
				RequestedHeight2898 - BakedSnowHeightStages2878[StageIndex2898].HeightCm);
			if (Distance2898 <= BestDistance2898)
			{
				BestDistance2898 = Distance2898;
				PreferredStageIndex2898 = StageIndex2898;
			}
		}

		for (int32 StageIndex2898 = 0; StageIndex2898 < SnowStageCount2898; ++StageIndex2898)
		{
			if (UGaussianSplatAsset* StageAsset2898 = BakedSnowHeightStages2878[StageIndex2898].SnowAsset.Get())
			{
				StageAsset2898->ConfigureSnowHeightStageTexturePolicy2898(
					true, StageIndex2898 == PreferredStageIndex2898);
			}
		}

		UE_LOG(LogTemp, Display,
			TEXT("[Snow2898Policy] Tagged=%d Preferred=%d RuntimeHeight=%.2fcm PIECopySafe=1"),
			SnowStageCount2898, PreferredStageIndex2898 + 1, SnowRuntimeHeightCm2878);
	}

	// A package saved after v2898 may already defer inactive textures during
	// asset PostLoad.  Ensure the serialized current stage is ready before any
	// component registration/rebind path can consume it.
	if (BakedSnowGaussianAsset)
	{
		BakedSnowGaussianAsset->EnsureSnowHeightStageColorTexture2898();
	}

	ShadowDebugMode = NormalizeStableShadowDebugMode(ShadowDebugMode);
	SyncProxyShadowReceiver();
	SyncWeatherEffects();
	SyncDayNightCycle();
	SyncCinematicPostProcess();
	SubmitStableShadowDebugMode(static_cast<int32>(ShadowDebugMode));
}

void AGaussianSplatActor::OnConstruction(const FTransform& Transform)
{
	UE_LOG(LogTemp, Warning,
		TEXT("[GSParamTrace] ActorConstruction Actor=%p Name=%s Component=%p"),
		this, *GetName(), GaussianSplatComponent.Get());
	Super::OnConstruction(Transform);
	ShadowDebugMode = NormalizeStableShadowDebugMode(ShadowDebugMode);
	SyncProxyShadowReceiver();
	SyncWeatherEffects();
	SyncDayNightCycle();
	SyncCinematicPostProcess();
	SubmitStableShadowDebugMode(static_cast<int32>(ShadowDebugMode));
}

void AGaussianSplatActor::BeginPlay()
{
	Super::BeginPlay();
	ShadowDebugMode = NormalizeStableShadowDebugMode(ShadowDebugMode);
	SyncDayNightCycle();
	SyncCinematicPostProcess();

	// MRQ/Sequencer renders use a duplicated game world. Phase-1/2 proof data is
	// intentionally transient in the editor component, so it is not serialized into
	// that duplicate. Rebuild the proof deterministically before the first rendered
	// frame whenever a stable-binding/field diagnostic is explicitly selected.
	// This is a render-test bootstrap only: camera/view values never participate in
	// the binding or field keys, and no SceneProxy/visibility/LOD mode is toggled.
	SubmitStableShadowDebugMode(static_cast<int32>(ShadowDebugMode));

	UWorld* World = GetWorld();
	const bool bGameRenderWorld = World && World->IsGameWorld();
	const int32 RequestedDebugMode = static_cast<int32>(ShadowDebugMode);
	// v2778 production handoff: Final (01) and Stable Proxy Lighting (02) use the
	// same camera-independent Stable Binding + Stable Field. 03 remains the UE-lit
	// proxy reference and does not build the custom field.
	if (RequestedDebugMode == 73)
	{
		SyncProxyShadowReceiver();
	}
	const bool bNeedsStableBinding = RequestedDebugMode == 0 || RequestedDebugMode == 72 || RequestedDebugMode == 79 ||
		RequestedDebugMode == 80 || RequestedDebugMode == 81 || RequestedDebugMode == 82 || RequestedDebugMode == 102 || RequestedDebugMode == 103 || RequestedDebugMode == 104 ||
		(RequestedDebugMode >= 74 && RequestedDebugMode <= 78);
	const bool bNeedsStableField = bNeedsStableBinding && RequestedDebugMode != 79 && RequestedDebugMode != 80 && RequestedDebugMode != 81 && RequestedDebugMode != 82;

	if (bGameRenderWorld && GaussianSplatComponent && bNeedsStableBinding)
	{
		UE_LOG(LogTemp, Log,
			TEXT("[GSStableMRQ] BeginPlay DebugMode=%d WorldType=%d BindingState=%d Rev=%u FieldReq=%u"),
			RequestedDebugMode, static_cast<int32>(World->WorldType),
			static_cast<int32>(GaussianSplatComponent->StableBindingState),
			GaussianSplatComponent->StableBindingRevision,
			GaussianSplatComponent->GetStableFieldRequestRevision());

		if (GaussianSplatComponent->StableBindingState != EStableProxyBindingState::Ready)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[GSStableMRQ] Persistent cache unavailable. Manual Rebuild Stable Binding is required; automatic rebuild is disabled."));
		}

		if (GaussianSplatComponent->StableBindingState == EStableProxyBindingState::Ready)
		{
			if (bNeedsStableField && GaussianSplatComponent->GetStableFieldRequestRevision() == 0u &&
				ProxyLightDepthCaptureRevision > 0u)
			{
				GaussianSplatComponent->RequestStableProxyFieldBuildForCapture(ProxyLightDepthCaptureRevision);
			}

			UE_LOG(LogTemp, Log,
				TEXT("[GSStableMRQ] Ready DebugMode=%d BindingRev=%u Hash=0x%016llX FieldReq=%u Splats=%d Valid=%d Invalid=%d"),
				RequestedDebugMode, GaussianSplatComponent->StableBindingRevision,
				static_cast<unsigned long long>(GaussianSplatComponent->StableBindingHash),
				GaussianSplatComponent->GetStableFieldRequestRevision(),
				GaussianSplatComponent->StableBindingInputCount,
				GaussianSplatComponent->StableBindingValidCount,
				GaussianSplatComponent->StableBindingInvalidCount);
		}
		else
		{
			UE_LOG(LogTemp, Error,
				TEXT("[GSStableMRQ] Binding bootstrap FAILED DebugMode=%d State=%d"),
				RequestedDebugMode, static_cast<int32>(GaussianSplatComponent->StableBindingState));
		}
	}
}

void AGaussianSplatActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const int32 RequestedStableDebugMode = static_cast<int32>(ShadowDebugMode);

	// v2778 editor/runtime auto-bootstrap. Final and mode 02 share one production
	// Stable Binding/Field. The arrays remain transient to avoid serializing ~22M
	// bindings into the level, but no manual rebuild button is required.
	const bool bStableProductionShadow = RequestedStableDebugMode == 0 || RequestedStableDebugMode == 72 ||
		RequestedStableDebugMode == 79 || RequestedStableDebugMode == 80 || RequestedStableDebugMode == 81 || RequestedStableDebugMode == 82 || RequestedStableDebugMode == 102 || RequestedStableDebugMode == 103 || RequestedStableDebugMode == 104 ||
		(RequestedStableDebugMode >= 74 && RequestedStableDebugMode <= 78);
	// Stable binding is deliberately manual. OnRegister restores the persistent
	// source-keyed cache; a missing/mismatched cache stays Rebuild Required.

	// Phase-1B full binding build is intentionally advanced before any camera-capture work.
	// It consumes a fixed splat chunk per tick and never depends on the current view.
	if (GaussianSplatComponent)
	{
		// References may become valid after registration/construction. The loader
		// internally suppresses repeat attempts for an unchanged source key.
		if (GaussianSplatComponent->StableBindingState == EStableProxyBindingState::Uninitialized)
		{
			GaussianSplatComponent->TryLoadStableProxyBindingCache();
		}
		GaussianSplatComponent->ContinueStableProxyBindingBuild();
		if (bStableProductionShadow &&
			GaussianSplatComponent->StableBindingState == EStableProxyBindingState::Ready &&
			GaussianSplatComponent->GetStableFieldRequestRevision() == 0u &&
			ProxyLightDepthCaptureRevision > 0u)
		{
			GaussianSplatComponent->RequestStableProxyFieldBuildForCapture(ProxyLightDepthCaptureRevision);
			UE_LOG(LogTemp, Log, TEXT("[GSStableAutoBootstrap] FieldStart ProductionStable=1 CaptureRev=%u ManualClickRequired=0"),
				ProxyLightDepthCaptureRevision);
		}
	}
	SyncWeatherEffects();
	SyncDayNightCycle();
	SyncCinematicPostProcess();
	// v2778: Final and mode 02 are the same live Stable Field authority. Legacy
	// one-shot/freeze behavior is not part of production anymore.
	const bool bStableFieldDebugNeedsLightDepth = bStableProductionShadow;
	if (!GaussianSplatComponent || !ProxyShadowReceiverComponent ||
		(!RequiresProxyShadowCaptures(GaussianSplatComponent->LightingPreviewMode) &&
			 !bStableFieldDebugNeedsLightDepth))
	{
		return;
	}
	if (!ProxyLightDepthTarget || !ProxySurfaceDepthTarget)
	{
		// On PIE/Movie Render Queue duplication the fixed-name transient targets
		// can still be in PostLoad when OnConstruction runs. Retry only until all
		// references have been safely rebound.
		SyncProxyShadowReceiver();
		if (!ProxyLightDepthTarget || !ProxySurfaceDepthTarget)
		{
			return;
		}
	}

	FVector ViewLocation = GetActorLocation();
	FRotator ViewRotation = GetActorRotation();
	float ViewFOV = 90.0f;
	float ViewAspectRatio = 0.0f;
	FIntPoint ViewportSize(1280, 720);
	TWeakObjectPtr<AActor> ViewTarget;
	bool bUsingGameCamera = false;
	if (UWorld* World = GetWorld(); World && World->IsGameWorld())
	{
		if (APlayerController* PlayerController = World->GetFirstPlayerController())
		{
			ViewTarget = PlayerController->GetViewTarget();
			PlayerController->GetPlayerViewPoint(ViewLocation, ViewRotation);
			if (PlayerController->PlayerCameraManager)
			{
				ViewFOV = PlayerController->PlayerCameraManager->GetFOVAngle();
			}
			if (ULocalPlayer* LocalPlayer = PlayerController->GetLocalPlayer())
			{
				if (LocalPlayer->ViewportClient && LocalPlayer->ViewportClient->Viewport)
				{
					ViewportSize = LocalPlayer->ViewportClient->Viewport->GetSizeXY();
				}
			}
			bUsingGameCamera = true;
		}
	}
#if WITH_EDITOR
	if (!bUsingGameCamera && GCurrentLevelEditingViewportClient)
	{
		FLevelEditorViewportClient* ViewportClient = GCurrentLevelEditingViewportClient;
		ViewLocation = ViewportClient->GetViewLocation();
		ViewRotation = ViewportClient->GetViewRotation();
		ViewFOV = ViewportClient->ViewFOV;
		ViewTarget = ViewportClient->GetActiveActorLock();
		if (UCameraComponent* LockedCamera = ViewportClient->GetCameraComponentForView())
		{
			// Sequencer camera cuts do not always populate ActiveActorLock. Use the
			// camera component's owner as the stable capture identity as well.
			ViewTarget = LockedCamera->GetOwner();
			FMinimalViewInfo CameraView;
			LockedCamera->GetCameraView(0.0f, CameraView);
			ViewLocation = CameraView.Location;
			ViewRotation = CameraView.Rotation;
			ViewFOV = CameraView.FOV;
			ViewAspectRatio = CameraView.AspectRatio;
		}
		if (ViewportClient->Viewport)
		{
			ViewportSize = ViewportClient->Viewport->GetSizeXY();
		}
	}
#endif
	ViewportSize.X = FMath::Max(ViewportSize.X, 16);
	ViewportSize.Y = FMath::Max(ViewportSize.Y, 16);
	if (ViewAspectRatio <= UE_KINDA_SMALL_NUMBER)
	{
		ViewAspectRatio = float(ViewportSize.X) / float(ViewportSize.Y);
	}
	// Overscan keeps the viewport inside the captured mask while the editor camera
	// moves. This also prevents newly revealed edges from sampling stale borders.
	const float CaptureFOV = FMath::Clamp(ViewFOV + 24.0f, 5.0f, 170.0f);
	const ELCCCinematicGSLightingPreviewMode PreviewMode =
		GaussianSplatComponent->LightingPreviewMode;
	// Panorama-safe fog: never drive a camera-following SceneCapture for fog.
	// Fog depth is resolved from the actual GS render view, so editor, MRQ tiles
	// and 360/panoramic panes all use their own native view without a helper camera.
	const bool bNeedsCameraSurfaceDepthCapture = false;
	const bool bNeedsAnyCameraCapture = bNeedsCameraSurfaceDepthCapture;
	UDirectionalLightComponent* MainDirectional = nullptr;
	float MainDirectionalScore = -1.0e9f;
	for (TObjectIterator<UDirectionalLightComponent> It; It; ++It)
	{
		UDirectionalLightComponent* Candidate = *It;
		if (!Candidate || Candidate->GetWorld() != GetWorld() || !Candidate->IsRegistered() || !Candidate->IsVisible())
		{
			continue;
		}
		const float CandidateScore = GetDirectionalDayNightAuthorityScore(Candidate, GaussianSplatComponent);
		if (!MainDirectional || CandidateScore > MainDirectionalScore)
		{
			MainDirectional = Candidate;
			MainDirectionalScore = CandidateScore;
		}
	}
	// v2828 keep LightDepth component authority on the authored Sun. The actual ray
	// direction already comes from RuntimeStableShadowDirection, so swapping the backing
	// component to the generated Moon at Z=0 only invalidates/rebuilds the field on one
	// frame and can look like a light/shadow flash.
	if (GaussianSplatComponent && GaussianSplatComponent->bEnableAutomaticDayNightCycle &&
		CachedDayNightSunLight.IsValid())
	{
		MainDirectional = CachedDayNightSunLight.Get();
	}
	// v2800: LightDepth must use the same authoritative celestial vector as Stable/GS.
	// Do not re-derive the angle from whichever Directional component happened to win
	// candidate selection; that could disagree during the Sun/Moon hand-off.
	const bool bUseRuntimeShadowDirection =
		GaussianSplatComponent && GaussianSplatComponent->bEnableAutomaticDayNightCycle &&
		GaussianSplatComponent->bRuntimeStableShadowDirectionValid;
	const FVector ActiveSurfaceToLight = bUseRuntimeShadowDirection
		? GaussianSplatComponent->RuntimeStableShadowDirection.GetSafeNormal()
		: (MainDirectional ? (-MainDirectional->GetForwardVector()).GetSafeNormal() : FVector::UpVector);
	const FVector ActiveRayDirection = (-ActiveSurfaceToLight).GetSafeNormal();
	const FRotator LightRotation = ActiveRayDirection.Rotation();
	const float LightIntensity = MainDirectional ? MainDirectional->Intensity : 0.0f;

	// Camera-projected shadow matte depends on the active view and light state.
	const bool bCameraCaptureStateChanged =
		bNeedsAnyCameraCapture && (
		!bHasProxyCaptureState ||
		!LastProxyCaptureActorTransform.Equals(GetActorTransform(), 0.001f) ||
		!LastProxyCaptureViewLocation.Equals(ViewLocation, 0.5f) ||
		!LastProxyCaptureViewRotation.Equals(ViewRotation, 0.01f) ||
		!LastProxyCaptureLightRotation.Equals(LightRotation, 0.01f) ||
		LastProxyCaptureViewportSize != ViewportSize ||
		LastProxyCaptureViewTarget != ViewTarget ||
		!FMath::IsNearlyEqual(LastProxyCaptureViewFOV, ViewFOV, 0.01f) ||
		!FMath::IsNearlyEqual(LastProxyCaptureViewAspectRatio, ViewAspectRatio, 0.001f) ||
		!FMath::IsNearlyEqual(LastProxyCaptureLightIntensity, LightIntensity, 0.01f));

	// Directional light-space depth is camera-independent. Build its geometry
	// state from the receiver plus explicit caster transforms/bounds so camera
	// movement alone never triggers this capture.
	FBox LightCaptureBounds = ProxyShadowReceiverComponent->Bounds.GetBox();
	uint32 CurrentLightDepthCasterHash = 0u;
	for (AActor* CasterActor : GaussianSplatComponent->ExternalShadowCasters)
	{
		if (!IsValid(CasterActor) || CasterActor->GetWorld() != GetWorld())
		{
			continue;
		}
		const FBox CasterBounds = CasterActor->GetComponentsBoundingBox(true);
		LightCaptureBounds += CasterBounds;
		const FMatrix CasterMatrix = CasterActor->GetActorTransform().ToMatrixWithScale();
		CurrentLightDepthCasterHash = HashCombine(
			CurrentLightDepthCasterHash, FCrc::MemCrc32(&CasterMatrix, sizeof(CasterMatrix)));
		CurrentLightDepthCasterHash = HashCombine(
			CurrentLightDepthCasterHash, FCrc::MemCrc32(&CasterBounds.Min, sizeof(CasterBounds.Min)));
		CurrentLightDepthCasterHash = HashCombine(
			CurrentLightDepthCasterHash, FCrc::MemCrc32(&CasterBounds.Max, sizeof(CasterBounds.Max)));
	}

	const FTransform ReceiverTransform = ProxyShadowReceiverComponent->GetComponentTransform();
	const FBox ReceiverBounds = ProxyShadowReceiverComponent->Bounds.GetBox();
	const bool bLightDepthReceiverTransformChanged =
		!LastProxyLightDepthReceiverTransform.Equals(ReceiverTransform, 0.001f);
	const bool bLightDepthReceiverMinChanged =
		!LastProxyLightDepthReceiverBoundsMin.Equals(ReceiverBounds.Min, 0.01f);
	const bool bLightDepthReceiverMaxChanged =
		!LastProxyLightDepthReceiverBoundsMax.Equals(ReceiverBounds.Max, 0.01f);
	const bool bLightDepthReceiverMeshChanged =
		LastProxyLightDepthReceiverMesh.Get() != ProxyShadowReceiverComponent->GetStaticMesh();
	// Offline/PIE render worlds must classify the exact Sequencer temporal-sample
	// light transform. The editor tolerance is useful while dragging, but applying
	// it to MRQ lets several samples reuse an older field and then jump when the
	// accumulated delta crosses 0.01 degrees (visible as lag/sparkle).
	const bool bExactRenderSample = GetWorld() && GetWorld()->IsGameWorld();
	const float LightRotationToleranceDegrees = bExactRenderSample ? 1.0e-6f : 0.01f;
	const bool bLightDepthRotationChanged =
		!LastProxyLightDepthLightRotation.Equals(LightRotation, LightRotationToleranceDegrees);
	const bool bLightDepthDirectionalChanged = LastProxyLightDepthDirectional.Get() != MainDirectional;
	const bool bLightDepthCasterChanged = LastProxyLightDepthCasterHash != CurrentLightDepthCasterHash;
	const bool bLightDepthStateChanged =
		!bHasProxyLightDepthState ||
		bLightDepthReceiverTransformChanged || bLightDepthReceiverMinChanged ||
		bLightDepthReceiverMaxChanged || bLightDepthReceiverMeshChanged ||
		bLightDepthRotationChanged || bLightDepthDirectionalChanged || bLightDepthCasterChanged;
	if (bHasProxyLightDepthState && bLightDepthStateChanged && CVarProxyCaptureDiagnostics.GetValueOnGameThread() != 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[GSLightDepthInvalidation] Transform=%d BoundsMin=%d BoundsMax=%d Mesh=%d Rotation=%d Directional=%d Caster=%d RotOld=(%.6f,%.6f,%.6f) RotNew=(%.6f,%.6f,%.6f)"),
			bLightDepthReceiverTransformChanged ? 1 : 0,
			bLightDepthReceiverMinChanged ? 1 : 0,
			bLightDepthReceiverMaxChanged ? 1 : 0,
			bLightDepthReceiverMeshChanged ? 1 : 0,
			bLightDepthRotationChanged ? 1 : 0,
			bLightDepthDirectionalChanged ? 1 : 0,
			bLightDepthCasterChanged ? 1 : 0,
			LastProxyLightDepthLightRotation.Pitch, LastProxyLightDepthLightRotation.Yaw, LastProxyLightDepthLightRotation.Roll,
			LightRotation.Pitch, LightRotation.Yaw, LightRotation.Roll);
	}
	// A CineCamera's horizontal FOV is defined for its own aspect ratio. Rendering
	// that projection into the editor viewport's differently-shaped target shifts
	// the shadow lookup and can make Final appear as a mostly black mask. Give the
	// capture the same aspect ratio as the active camera while retaining the
	// viewport width for stable detail.
	const float CaptureResolutionScale = FMath::Clamp(GaussianSplatComponent->ProxyCaptureResolutionScale, 0.25f, 1.0f);
	const int32 CaptureMaxDimension = FMath::Clamp(GaussianSplatComponent->ProxyCaptureMaxDimension, 512, 4096);
	FIntPoint CaptureTargetSize(
		FMath::Max(16, FMath::RoundToInt(float(ViewportSize.X) * CaptureResolutionScale)),
		FMath::Max(16, FMath::RoundToInt(float(ViewportSize.Y) * CaptureResolutionScale)));
	CaptureTargetSize.Y = FMath::Max(16, FMath::RoundToInt(float(CaptureTargetSize.X) / FMath::Max(ViewAspectRatio, 0.01f)));
	const int32 LargestCaptureDimension = FMath::Max(CaptureTargetSize.X, CaptureTargetSize.Y);
	if (LargestCaptureDimension > CaptureMaxDimension)
	{
		const float Downscale = float(CaptureMaxDimension) / float(LargestCaptureDimension);
		CaptureTargetSize.X = FMath::Max(16, FMath::RoundToInt(float(CaptureTargetSize.X) * Downscale));
		CaptureTargetSize.Y = FMath::Max(16, FMath::RoundToInt(float(CaptureTargetSize.Y) * Downscale));
	}
	bool bPrimaryCaptureTargetsResized = false;
	if (bNeedsCameraSurfaceDepthCapture)
	{
		for (UTextureRenderTarget2D* Target : { ProxySurfaceDepthTarget.Get(), ProxySurfaceMaterialTarget.Get() })
		{
			if (Target && (Target->SizeX != CaptureTargetSize.X || Target->SizeY != CaptureTargetSize.Y))
			{
				Target->ResizeTarget(CaptureTargetSize.X, CaptureTargetSize.Y);
				bPrimaryCaptureTargetsResized = true;
			}
		}
	}
	if (bCameraCaptureStateChanged)
	{
		// Surface depth is also the authoritative geometry source for depth-aware
		// shadow filtering and neutral receiver-light reconstruction. Capture it
		// whenever the proxy shadow camera changes, even when weather surface effects
		// are disabled.
		if (bNeedsCameraSurfaceDepthCapture && ProxySurfaceDepthCapture && ProxySurfaceDepthTarget)
		{
			ProxySurfaceDepthCapture->TextureTarget = ProxySurfaceDepthTarget;
			ProxySurfaceDepthCapture->SetWorldLocationAndRotation(ViewLocation, ViewRotation);
			ProxySurfaceDepthCapture->FOVAngle = CaptureFOV;
			ProxySurfaceDepthCapture->CaptureScene();
		}
		CurrentProxyCaptureLocation = ViewLocation;
		CurrentProxyCaptureRotation = ViewRotation;
		CurrentProxyCaptureFOV = CaptureFOV;
		CurrentProxyCaptureAspectRatio = float(CaptureTargetSize.X) / float(CaptureTargetSize.Y);
		if (CVarProxyCaptureDiagnostics.GetValueOnGameThread() != 0)
		{
			UE_LOG(LogTemp, Log, TEXT("LCCCinematicGS ProxyCapture: CameraMatte updated for %s"), *GetName());
		}
	}
	// v2778 PRODUCTION STABLE LIGHT-DEPTH. The legacy one-shot/freeze snapshot
	// machinery has been removed from the Final authority. A single camera-independent
	// directional LightDepth target follows only light/receiver/caster state changes;
	// camera motion never invalidates it. The completed Stable Field is rebuilt from
	// this exact packet and then consumed by both Final and mode 02.
	FTextureRHIRef LightDepthTexture;
	FTextureRHIRef ReceiverLightDepthTexture;
	FVector3f LightCaptureOrigin = FVector3f::ZeroVector;
	FVector3f LightCaptureForward(1, 0, 0);
	FVector3f LightCaptureRight(0, 1, 0);
	FVector3f LightCaptureUp(0, 0, 1);
	float LightCaptureWidth = 1.0f;

	if (MainDirectional && ProxyLightDepthCapture && ProxyLightDepthTarget && ProxyReceiverLightDepthCapture && ProxyReceiverLightDepthTarget)
	{
		const FVector Center = LightCaptureBounds.GetCenter();
		const float Radius = FMath::Max(LightCaptureBounds.GetExtent().Size(), 100.0f);
		const FVector RayDirection = ActiveRayDirection;
		const FRotator CaptureRotation = RayDirection.Rotation();
		const float OrthoWidth = Radius * 2.2f;
		const FVector CaptureOrigin = Center - RayDirection * (Radius + 100.0f);

		ProxyLightDepthCapture->TextureTarget = ProxyLightDepthTarget;
		ProxyReceiverLightDepthCapture->TextureTarget = ProxyReceiverLightDepthTarget;
		ProxyLightDepthCapture->SetWorldLocationAndRotation(CaptureOrigin, CaptureRotation);
		ProxyReceiverLightDepthCapture->SetWorldLocationAndRotation(CaptureOrigin, CaptureRotation);
		ProxyLightDepthCapture->OrthoWidth = OrthoWidth;
		ProxyReceiverLightDepthCapture->OrthoWidth = OrthoWidth;

		if (bLightDepthStateChanged)
		{
			ProxyReceiverLightDepthCapture->CaptureScene();
			const int32 ExternalPrimitiveCount = ProxyLightDepthCapture->ShowOnlyComponents.Num();
			if (ExternalPrimitiveCount > 0)
			{
				ProxyLightDepthCapture->CaptureScene();
			}
			else
			{
				// An empty PRM_UseShowOnlyList capture can leave the previous render-target
				// contents untouched. Explicitly publish "no blocker" so an old receiver
				// depth cannot masquerade as the external-caster texture in MRQ.
				UKismetRenderingLibrary::ClearRenderTarget2D(
					this, ProxyLightDepthTarget,
					FLinearColor(HALF_WORLD_MAX, HALF_WORLD_MAX, HALF_WORLD_MAX, 1.0f));
			}
			// CaptureScene only enqueues render work. The same render targets are reused
			// by the next MRQ temporal sample, so without a fence that sample can
			// overwrite LightDepth while the current visibility pass is still reading it.
			// Complete this exact light-space packet before publishing its revision.
            if (!GSPCVR::OrderedCaptures(GetWorld()) || CVarCastDepthDataLog.GetValueOnGameThread()!=0)
            {
                FlushRenderingCommands();
            }
            else
            {
                // Realtime CaptureScene enqueues its render work before the
                // immutable light packet and before this frame's main view.
                // The next game frame's capture is enqueued after that main
                // view. FIFO render/RHI ordering preserves target read/write
                // ownership without waiting for the entire renderer on GT.
                // MRQ and synchronous diagnostic readback retain the old guard.
                static double LastOrderedCaptureLog = 0.0;
                const double Now = FPlatformTime::Seconds();
                if (Now-LastOrderedCaptureLog>=1.0)
                {
                    LastOrderedCaptureLog=Now;
                    UE_LOG(LogTemp,Display,TEXT("[GSPCVR][OrderedCapture] NoGlobalFlush=1 NextRevision=%u ExternalPrimitives=%d Readback=0"),
                        ProxyLightDepthCaptureRevision+1u, ExternalPrimitiveCount);
                }
            }
			ProxyLightDepthCaptureRevision = (ProxyLightDepthCaptureRevision == MAX_uint32)
				? 1u : (ProxyLightDepthCaptureRevision + 1u);
			RecordCastDepthData(
				ProxyLightDepthTarget, ProxyReceiverLightDepthTarget,
				ProxyLightDepthCaptureRevision, LightRotation,
				CaptureOrigin, RayDirection, OrthoWidth);
			if (CVarCastDepthDataLog.GetValueOnGameThread() != 0)
			{
				UE_LOG(LogTemp, Log,
					TEXT("[GSCastDepthSetup] Rev=%u ExternalPrimitives=%d Combined=%s Receiver=%s SameTarget=%d"),
					ProxyLightDepthCaptureRevision, ExternalPrimitiveCount,
					*GetNameSafe(ProxyLightDepthTarget), *GetNameSafe(ProxyReceiverLightDepthTarget),
					ProxyLightDepthTarget == ProxyReceiverLightDepthTarget ? 1 : 0);
			}
			if (CVarProxyCaptureDiagnostics.GetValueOnGameThread() != 0)
			{
				UE_LOG(LogTemp, Log,
					TEXT("[GSStableProduction] LightDepthCapture Rev=%u Rot=(%.4f,%.4f,%.4f) Width=%.3f"),
					ProxyLightDepthCaptureRevision, LightRotation.Pitch, LightRotation.Yaw, LightRotation.Roll, OrthoWidth);
			}
		}

		LightCaptureOrigin = FVector3f(CaptureOrigin);
		LightCaptureForward = FVector3f(ProxyLightDepthCapture->GetForwardVector().GetSafeNormal());
		LightCaptureRight = FVector3f(ProxyLightDepthCapture->GetRightVector().GetSafeNormal());
		LightCaptureUp = FVector3f(ProxyLightDepthCapture->GetUpVector().GetSafeNormal());
		LightCaptureWidth = OrthoWidth;

		if (FTextureRenderTargetResource* Resource = ProxyLightDepthTarget->GameThread_GetRenderTargetResource())
		{
			LightDepthTexture = Resource->GetRenderTargetTexture();
		}
		if (FTextureRenderTargetResource* Resource = ProxyReceiverLightDepthTarget->GameThread_GetRenderTargetResource())
		{
			ReceiverLightDepthTexture = Resource->GetRenderTargetTexture();
		}
	}
	if (bCameraCaptureStateChanged)
	{
		LastProxyCaptureActorTransform = GetActorTransform();
		LastProxyCaptureViewLocation = ViewLocation;
		LastProxyCaptureViewRotation = ViewRotation;
		LastProxyCaptureLightRotation = LightRotation;
		LastProxyCaptureViewportSize = ViewportSize;
		LastProxyCaptureViewFOV = ViewFOV;
		LastProxyCaptureViewAspectRatio = ViewAspectRatio;
		LastProxyCaptureLightIntensity = LightIntensity;
		LastProxyCaptureViewTarget = ViewTarget;
		bHasProxyCaptureState = true;
	}
	if (bLightDepthStateChanged && MainDirectional && ProxyLightDepthCapture && ProxyLightDepthTarget)
	{
		LastProxyLightDepthReceiverTransform = ReceiverTransform;
		LastProxyLightDepthReceiverBoundsMin = ReceiverBounds.Min;
		LastProxyLightDepthReceiverBoundsMax = ReceiverBounds.Max;
		LastProxyLightDepthReceiverMesh = ProxyShadowReceiverComponent->GetStaticMesh();
		LastProxyLightDepthLightRotation = LightRotation;
		LastProxyLightDepthDirectional = MainDirectional;
		LastProxyLightDepthCasterHash = CurrentLightDepthCasterHash;
		LastProxyLightDepthCaptureOrigin = LightCaptureOrigin;
		LastProxyLightDepthCaptureForward = LightCaptureForward;
		LastProxyLightDepthCaptureRight = LightCaptureRight;
		LastProxyLightDepthCaptureUp = LightCaptureUp;
		LastProxyLightDepthCaptureWidth = LightCaptureWidth;
		bHasProxyLightDepthState = true;

		const bool bStableFieldHasBeenRequested =
			GaussianSplatComponent && GaussianSplatComponent->GetStableFieldRequestRevision() > 0u;
		if (GaussianSplatComponent &&
			GaussianSplatComponent->StableBindingState == EStableProxyBindingState::Ready &&
			(bStableFieldDebugNeedsLightDepth || bStableFieldHasBeenRequested))
		{
			GaussianSplatComponent->RequestStableProxyFieldBuildForCapture(ProxyLightDepthCaptureRevision);
		}
	}

	// Always bind the projection that actually produced the texture. The live
	// camera can move below the capture invalidation threshold between captures.
	const FVector CaptureProjectionLocation = CurrentProxyCaptureLocation;
	const FRotator CaptureProjectionRotation = CurrentProxyCaptureRotation;
	const float CaptureProjectionFOV = CurrentProxyCaptureFOV;
	const float CaptureProjectionAspect = CurrentProxyCaptureAspectRatio;
	const FRotationMatrix CaptureRotationMatrix(CaptureProjectionRotation);
	const FVector3f CaptureProjectionForward(CaptureRotationMatrix.GetUnitAxis(EAxis::X));
	const FVector3f CaptureProjectionRight(CaptureRotationMatrix.GetUnitAxis(EAxis::Y));
	const FVector3f CaptureProjectionUp(CaptureRotationMatrix.GetUnitAxis(EAxis::Z));

	// v2748: the old camera-capture path follows the same unified controls as Stable Lighting.
	const float UnifiedCastDensity = FMath::Clamp(GaussianSplatComponent->CapturedShadowStrength, 0.0f, 2.0f);
	const float UnifiedCastStrength = FMath::Clamp(UnifiedCastDensity, 0.0f, 1.0f);
	const float UnifiedCastGamma = FMath::Clamp(FMath::Max(UnifiedCastDensity, 0.10f), 0.10f, 2.0f);
	const float UnifiedClosureDensity = FMath::Clamp(GaussianSplatComponent->ProxyLightingMatchStrength, 0.0f, 2.0f);
	const float UnifiedClosureStrength = FMath::Clamp(UnifiedClosureDensity, 0.0f, 1.0f);
	const float UnifiedClosureCurve = FMath::Clamp(FMath::Max(UnifiedClosureDensity, 0.25f), 0.25f, 2.0f);
	const FVector4f CaptureSettings(
		GaussianSplatComponent->CapturedShadowSoftness,
		UnifiedCastGamma,
		GaussianSplatComponent->CapturedShadowMinimum,
		UnifiedCastStrength);
	const FVector4f ShadowDetailSettings(
		GaussianSplatComponent->CapturedShadowDepthToleranceCm,
		GaussianSplatComponent->CapturedShadowDepthToleranceScale,
		0.0f,
		GaussianSplatComponent->CapturedShadowDepthEdgeProtection);
	const FVector4f ShadowShapingSettings(
		GaussianSplatComponent->CapturedDirectLightWhitePoint,
		UnifiedCastGamma,
		UnifiedClosureStrength,
		GaussianSplatComponent->CapturedClosedShadowStart);
	const FVector4f LightDepthSettings(
		LightCaptureWidth,
		GaussianSplatComponent->CapturedShadowDistanceRange,
		GaussianSplatComponent->CapturedShadowDistanceLightening,
		GaussianSplatComponent->CapturedShadowPhysicalSoftness);
	const FVector4f FinalCompositeSettings(
		GaussianSplatComponent->CapturedBaseShadowTransferStrength,
		GaussianSplatComponent->CapturedBaseShadowMinimum,
		GaussianSplatComponent->CapturedBaseShadowSaturation,
		// 1.0 is the backwards-compatible neutral marker. Values above one
		// carry the visible "Indirect Shadow Lift" control into Final.
		1.0f + FMath::Clamp(GaussianSplatComponent->TargetShadowLift, 0.0f, 0.3f));
	const FVector4f ReflectedLightSettings(
		GaussianSplatComponent->CapturedShadowBounceStrength,
		UnifiedClosureStrength,
		UnifiedClosureCurve,
		GaussianSplatComponent->CapturedReflectedLightTintStrength);
	const FVector4f ReflectedColorSettings(
		GaussianSplatComponent->CapturedReflectedLightDeepTintNeutralization,
		GaussianSplatComponent->CapturedReflectedLightDeepSaturationRetention,
		GaussianSplatComponent->CapturedReflectedLightHighlightCompression,
		GaussianSplatComponent->CapturedReflectedLightTintSoftness);
	const FVector3f FinalCompositeTint(
		GaussianSplatComponent->CapturedBaseShadowTint.R * GaussianSplatComponent->TargetSkyTint.R,
		GaussianSplatComponent->CapturedBaseShadowTint.G * GaussianSplatComponent->TargetSkyTint.G,
		GaussianSplatComponent->CapturedBaseShadowTint.B * GaussianSplatComponent->TargetSkyTint.B);
	// Deprecated: this path added scattering once per splat and once again in
	// the fullscreen composite, overlapping the unified atmosphere.
	const FVector4f VolumetricShaftSettings(
		0.0f,
		GaussianSplatComponent->ProxyVolumetricShaftDensity,
		GaussianSplatComponent->ProxyVolumetricShaftMaxDistance,
		GaussianSplatComponent->ProxyVolumetricShaftForwardScattering);
	const FVector3f VolumetricShaftTint(
		GaussianSplatComponent->ProxyVolumetricShaftTint.R,
		GaussianSplatComponent->ProxyVolumetricShaftTint.G,
		GaussianSplatComponent->ProxyVolumetricShaftTint.B);
	FTextureRHIRef SurfaceDepthTexture;
	if (FTextureRenderTargetResource* Resource = ProxySurfaceDepthTarget
		? ProxySurfaceDepthTarget->GameThread_GetRenderTargetResource()
		: nullptr)
	{
		SurfaceDepthTexture = Resource->GetRenderTargetTexture();
	}
	const FLinearColor SurfaceColor = GaussianSplatComponent->ProxySurfaceEffectColor;
	const bool bHasBakedSnowGeometry =
		GaussianSplatComponent->WeatherMode == ELCCCinematicGSWeatherMode::Snow &&
		GaussianSplatComponent->ProxySurfaceEffectMode == ELCCCinematicGSProxySurfaceEffectMode::Snow &&
		GaussianSplatComponent->bUseSnowGaussianLayer &&
		BakedSnowGaussianAsset && BakedSnowGaussianAsset->IsValid();
	const FVector4f SurfaceEffectSettings0(
		static_cast<float>(GaussianSplatComponent->ProxySurfaceEffectMode),
		// The mask must be computed at full authored strength even when the
		// artist dials source-snow tint to zero. W of DirectSnowTuning is the mix.
		GaussianSplatComponent->ProxySurfaceEffectStrength,
		(bHasBakedSnowGeometry ? GaussianSplatComponent->SnowGaussianSlopeThreshold
			: GaussianSplatComponent->ProxySurfaceHorizontalBias),
		(bHasBakedSnowGeometry ? GaussianSplatComponent->SnowGaussianMoundScaleCm
			: GaussianSplatComponent->ProxySurfacePatternScale));
	const FLinearColor SnowSourceColor = bHasBakedSnowGeometry
		? GaussianSplatComponent->SnowGaussianColor : SurfaceColor;
	const FVector4f SurfaceEffectSettings1(
		SnowSourceColor.R, SnowSourceColor.G, SnowSourceColor.B,
		GaussianSplatComponent->ProxySurfaceSpecularStrength);
	// GroundWetControl v3: Settings2 x/y are mode-specific to avoid expanding any
	// shader parameter structure. Snow keeps thickness/expansion exactly as before.
	// Wet reuses those otherwise-unused slots for receiver plane Full/Max distance.
	// A negative Wet Max distance is only a compact flag for the ground-mask preview.
	const bool bWetSurfaceMode =
		GaussianSplatComponent->ProxySurfaceEffectMode == ELCCCinematicGSProxySurfaceEffectMode::WetPuddles;
	const float WetGroundFullDistanceCm = FMath::Max(GaussianSplatComponent->ProxyWetGroundFullDistanceCm, 0.0f);
	const float WetGroundMaxDistanceCm = FMath::Max(
		GaussianSplatComponent->ProxyWetGroundMaxDistanceCm, 0.0f);
	const float EncodedWetGroundMaxDistanceCm = GaussianSplatComponent->bPreviewWetGroundMask
		? -WetGroundMaxDistanceCm
		: WetGroundMaxDistanceCm;
	const FVector4f SurfaceEffectSettings2(
		bWetSurfaceMode ? WetGroundFullDistanceCm : GaussianSplatComponent->ProxySnowThicknessCm,
		bWetSurfaceMode ? EncodedWetGroundMaxDistanceCm :
			(bHasBakedSnowGeometry ? GaussianSplatComponent->SnowGaussianBakedPatchThreshold : GaussianSplatComponent->ProxySnowSplatExpansion),
		GaussianSplatComponent->ProxyWetDarkening,
		GaussianSplatComponent->ProxyPuddleAmount);
	const uint32 LightDepthCaptureRevisionForRender = ProxyLightDepthCaptureRevision;
	ENQUEUE_RENDER_COMMAND(UpdateGaussianProxyShadowCaptures)(
		[LightDepthTexture, ReceiverLightDepthTexture, SurfaceDepthTexture,
		 SurfaceEffectSettings0, SurfaceEffectSettings1, SurfaceEffectSettings2, CaptureSettings,
		 ShadowDetailSettings, ShadowShapingSettings, LightCaptureOrigin,
		 LightCaptureForward, LightCaptureRight, LightCaptureUp, LightDepthSettings,
		 CaptureProjectionOrigin = FVector3f(CaptureProjectionLocation), CaptureProjectionForward,
	 CaptureProjectionRight, CaptureProjectionUp, CaptureProjectionFOV, CaptureProjectionAspect,
	 OcclusionBoost = UnifiedClosureCurve,
	 ClosedShadowFullBlackEnd = GaussianSplatComponent->CapturedClosedShadowFullBlackEnd,
	 ClosedShadowAreaScale = GaussianSplatComponent->CapturedClosedShadowAreaScale,
	 ClosedShadowEdgeSoftness = GaussianSplatComponent->CapturedClosedShadowEdgeSoftness,
	 ProxyDepthShadowStrength = GaussianSplatComponent->ProxyDepthShadowStrength,
	 ProxyLightingMatchStrength = UnifiedClosureStrength,
	 ShadowSaturation = GaussianSplatComponent->CapturedShadowBounceStrength,
	 ReflectedLightSettings, ReflectedColorSettings, FinalCompositeSettings, FinalCompositeTint, VolumetricShaftSettings, VolumetricShaftTint,
	 LightDepthCaptureRevisionForRender,
    bEnableShadowLighting = true](FRHICommandListImmediate&)
		{
			FGaussianSplatRenderer::SetProxyShadowCaptureSettings(CaptureSettings);
			FGaussianSplatRenderer::SetProxyShadowDetailSettings(ShadowDetailSettings);
			FGaussianSplatRenderer::SetProxyShadowEvidenceSettings(
				ClosedShadowFullBlackEnd, ProxyLightingMatchStrength,
				ClosedShadowAreaScale, ClosedShadowEdgeSoftness);
			FGaussianSplatRenderer::SetProxyShadowShapingSettings(ShadowShapingSettings);
			FGaussianSplatRenderer::SetProxyShadowCaptureProjection(
				CaptureProjectionOrigin, CaptureProjectionForward, CaptureProjectionRight, CaptureProjectionUp,
				CaptureProjectionFOV, CaptureProjectionAspect);
			FGaussianSplatRenderer::SetProxyShadowOcclusionBoost(OcclusionBoost);
			FGaussianSplatRenderer::SetProxyShadowColorSettings(ShadowSaturation, bEnableShadowLighting);
			FGaussianSplatRenderer::SetProxyReflectedLightSettings(ReflectedLightSettings);
			FGaussianSplatRenderer::SetProxyReflectedColorSettings(ReflectedColorSettings);
			FGaussianSplatRenderer::SetProxyShadowFinalCompositeSettings(FinalCompositeSettings, FinalCompositeTint);
			FGaussianSplatRenderer::SetProxyVolumetricShaftSettings(
				VolumetricShaftSettings, VolumetricShaftTint);
			FGaussianSplatRenderer::SetProxySurfaceEffect(
				SurfaceDepthTexture, SurfaceEffectSettings0, SurfaceEffectSettings1, SurfaceEffectSettings2,
				CaptureProjectionOrigin, CaptureProjectionForward, CaptureProjectionRight,
				CaptureProjectionUp, CaptureProjectionFOV, CaptureProjectionAspect);
			FGaussianSplatRenderer::SetProxyLightDepthCapture(
				LightDepthTexture, ReceiverLightDepthTexture, LightCaptureOrigin, LightCaptureForward, LightCaptureRight, LightCaptureUp, LightDepthSettings,
				LightDepthCaptureRevisionForRender);
		});
}

void AGaussianSplatActor::RebuildStableProxyBinding()
{
	if (GaussianSplatComponent)
	{
		GaussianSplatComponent->RebuildStableProxyBindings();
	}
}

void AGaussianSplatActor::RebuildStableProxyField()
{
	if (GaussianSplatComponent)
	{
		GaussianSplatComponent->RequestStableProxyFieldBuildForCapture(ProxyLightDepthCaptureRevision);
	}
}

void AGaussianSplatActor::BakeStableCastAngleCache()
{
	if (GaussianSplatComponent)
	{
		GaussianSplatComponent->BakeStableCastAngleCache();
	}
}


#if WITH_EDITOR
void AGaussianSplatActor::PreEditChange(FProperty* PropertyAboutToChange)
{
	// PostEditChangeProperty handles this render-thread diagnostic selector without
	// calling Super. Match that path here: AActor::PreEditChange unregisters all
	// components, and skipping only the matching PostEdit Super call left the GS
	// SceneProxy permanently unregistered after the first mode switch.
    const FName EditingName = PropertyAboutToChange ? PropertyAboutToChange->GetFName() : NAME_None;
    if (EditingName == GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, ShadowDebugMode) ||
        EditingName == GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, SnowRuntimeHeightCm2878) ||
        EditingName == GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, SnowBakeMaximumHeightCm2878) ||
        EditingName == GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, SnowBakeIntervalCount2879))
	{
		return;
	}
	if (GaussianSplatComponent && GaussianSplatComponent->IsEditorRealtimePropertyChangeActive())
	{
		bSuppressActorReconstructionForComponentTuning = true;
		UE_LOG(LogTemp, Warning,
			TEXT("[GSParamTrace] ActorPreEdit bypassed component unregister Actor=%p Property=%s"),
			this, *GetNameSafe(PropertyAboutToChange));
		return;
	}
	Super::PreEditChange(PropertyAboutToChange);
}

void AGaussianSplatActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	if (bSuppressActorReconstructionForComponentTuning)
	{
		bSuppressActorReconstructionForComponentTuning = false;
		UE_LOG(LogTemp, Warning,
			TEXT("[GSParamTrace] ActorPostEdit bypassed reconstruction Actor=%p Property=%s"),
			this, *PropertyChangedEvent.GetPropertyName().ToString());
		return;
	}
    if (PropertyChangedEvent.GetPropertyName() ==
        GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, SnowRuntimeHeightCm2878))
    {
        // Paired with PreEditChange bypass above. Never unregister source/Snow scene proxies.
        SnowRuntimeHeightCm2878 = FMath::Clamp(SnowRuntimeHeightCm2878, 0.0f, 50.0f);
        SyncWeatherEffects();
        return;
    }
    if (PropertyChangedEvent.GetPropertyName() ==
        GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, SnowBakeMaximumHeightCm2878))
    {
        SnowBakeMaximumHeightCm2878 = FMath::Clamp(SnowBakeMaximumHeightCm2878, 4.0f, 50.0f);
        SnowHeightBakeStatus2878 = TEXT("최대 높이 변경됨: 눈높이 베이크 재생성이 필요합니다. 기존 베이크는 유지됩니다.");
        return;
    }
    if (PropertyChangedEvent.GetPropertyName() ==
        GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, SnowBakeIntervalCount2879))
    {
        SnowBakeIntervalCount2879 = FMath::Clamp(SnowBakeIntervalCount2879, 4, 12);
        SnowHeightBakeStatus2878 = TEXT("베이크 구간 수 변경됨: 재생성 전까지 기존 단계가 그대로 유지됩니다.");
        return;
    }
    // Step 1 safety rule: changing only the debug selector must never touch
	// SceneProxy lifetime, component visibility, capture validity, LOD, or sort state.
	if (PropertyChangedEvent.GetPropertyName() ==
		GET_MEMBER_NAME_CHECKED(AGaussianSplatActor, ShadowDebugMode))
	{
		ShadowDebugMode = NormalizeStableShadowDebugMode(ShadowDebugMode);
		const int32 RequestedMode = static_cast<int32>(ShadowDebugMode);
		SubmitStableShadowDebugMode(RequestedMode);

		// Only mode 03 changes visible presentation. Mode 02 prepares the same
		// camera-independent stable binding/field used by the stabilized shadow pass.
		SyncProxyShadowReceiver();
		if ((RequestedMode == 72 || RequestedMode == 79 || RequestedMode == 80 || RequestedMode == 81 || RequestedMode == 82 || RequestedMode == 102 || RequestedMode == 103 || RequestedMode == 104 ||
			(RequestedMode >= 74 && RequestedMode <= 78)) && GaussianSplatComponent)
		{
			if (GaussianSplatComponent->StableBindingState != EStableProxyBindingState::Ready)
			{
				GaussianSplatComponent->RebuildStableProxyBindings();
			}
			else if (RequestedMode != 79 && RequestedMode != 80 && RequestedMode != 81 && RequestedMode != 82)
			{
				GaussianSplatComponent->RequestStableProxyFieldBuildForCapture(ProxyLightDepthCaptureRevision);
			}
		}

		UE_LOG(LogTemp, Log, TEXT("[GSStableDebugUI] Requested=%d MinimalSet=1 ProxyMeshOnlyActive=%d"),
			RequestedMode, bStableProxyMeshOnlyPresentationActive ? 1 : 0);
		return;
	}

	// AActor's implementation may rerun the construction script. Do it only after
	// the render-thread-only debug selector has taken its early-return path above;
	// otherwise a mode change duplicates the ~22M stable bindings and recreates all
	// large GS GPU resources even though no component property changed.
	Super::PostEditChangeProperty(PropertyChangedEvent);

	SyncProxyShadowReceiver();
	SyncWeatherEffects();
	SyncDayNightCycle();
	SyncCinematicPostProcess();
}
#endif


void AGaussianSplatActor::SyncDayNightCycle()
{
	if (!GaussianSplatComponent || !MoonDirectionalLightComponent)
	{
		return;
	}

	GaussianSplatComponent->RuntimeNightBlend = 0.0f;
	GaussianSplatComponent->RuntimeDirectionalLightPitchDegrees = 0.0f;
	GaussianSplatComponent->RuntimeSolarElevationDegrees = 0.0f;
	GaussianSplatComponent->RuntimeTimeOfDayHours = 12.0f;
	GaussianSplatComponent->RuntimeTimeOfDayText = TEXT("12:00");
	GaussianSplatComponent->RuntimeTimeOfDayPhase = TEXT("Day");
	GaussianSplatComponent->RuntimeTwilightBlend = 0.0f;
	GaussianSplatComponent->RuntimeGoldenHourBlend = 0.0f;
	GaussianSplatComponent->RuntimeSunsetBlend = 0.0f;
	GaussianSplatComponent->RuntimeBlueHourBlend = 0.0f;
	GaussianSplatComponent->bRuntimeSolarRising = bDayNightSunRising;

	GaussianSplatComponent->RuntimeTimeOfDayGradeColor = FLinearColor::White;
	GaussianSplatComponent->RuntimeTimeOfDayGradeStrength = 0.0f;
	GaussianSplatComponent->RuntimeUnifiedTODDirectEnergy = 1.0f;
	GaussianSplatComponent->RuntimeUnifiedTODSkyEnergy = 1.0f;
	GaussianSplatComponent->RuntimeUnifiedTODCarrierBrightness = 1.0f;
	GaussianSplatComponent->RuntimeUnifiedTODGamma = 1.0f;
	GaussianSplatComponent->RuntimeUnifiedTODSaturation = 1.0f;
	GaussianSplatComponent->RuntimeUnifiedTODAfternoonLift = 0.0f;
	GaussianSplatComponent->RuntimeUnifiedTODSunColor = FLinearColor::White;
	GaussianSplatComponent->RuntimeUnifiedTODAmbientColor = FLinearColor(0.72f, 0.84f, 1.0f, 1.0f);
	GaussianSplatComponent->RuntimeUnifiedTODGradeStrength = 0.0f;
	GaussianSplatComponent->RuntimeNightEnvironmentColor = GaussianSplatComponent->NightAmbientColor;
	GaussianSplatComponent->RuntimeNightEnvironmentEnergyScale = 1.0f;
	GaussianSplatComponent->RuntimeNightGradeGamma = GaussianSplatComponent->NightGamma;
	GaussianSplatComponent->RuntimeNightGradeSaturation = GaussianSplatComponent->NightSaturation;
	GaussianSplatComponent->RuntimeNightGradeBrightness = GaussianSplatComponent->NightBrightness;
	GaussianSplatComponent->RuntimeSkyAmbientColor = GaussianSplatComponent->UnrealAmbientColor;
	GaussianSplatComponent->bRuntimeSkyAmbientColorValid = false;
	GaussianSplatComponent->RuntimeActiveCelestialDirection = FVector::UpVector;
	GaussianSplatComponent->bRuntimeActiveCelestialDirectionValid = false;
	GaussianSplatComponent->RuntimeStableShadowDirection = FVector::UpVector;
	GaussianSplatComponent->bRuntimeStableShadowDirectionValid = false;
	GaussianSplatComponent->RuntimeContinuousCelestialColor = FLinearColor::White;
	GaussianSplatComponent->RuntimeContinuousCelestialIntensity = 0.0f;
	GaussianSplatComponent->bRuntimeContinuousCelestialStateValid = false;
	GaussianSplatComponent->bRuntimeActiveCelestialIsMoon = false;

	UWorld* World = GetWorld();
	const bool bOwnsWorldDayNight = FindPrimaryDayNightOwner(World, this) == this;
	if (!GaussianSplatComponent->bEnableAutomaticDayNightCycle)
	{
		if (GaussianAutoSkyAtmosphere) GaussianAutoSkyAtmosphere->SetVisibility(false);
		if (GaussianAutoVolumetricCloud) GaussianAutoVolumetricCloud->SetVisibility(false);
		if (bCachedDayNightSunSourceAngle && CachedDayNightSunLight.IsValid())
		{
			CachedDayNightSunLight->SetLightSourceAngle(OriginalDayNightSunSourceAngle);
			bCachedDayNightSunSourceAngle = false;
		}
		if (CachedDayNightSunLight.IsValid() && bP23HasAppliedSunLux && bCachedDayNightSunBaseIntensity)
		{
			CachedDayNightSunLight->SetIntensity(CachedDayNightSunBaseIntensity);
		}
		if (CachedDayNightSunLight.IsValid() && bP23CachedSunLightShaftBloom)
		{
			CachedDayNightSunLight->SetEnableLightShaftBloom(bP23OriginalSunLightShaftBloom);
		}
		if (bP23CachedMoonLightShaftBloom)
		{
			MoonDirectionalLightComponent->SetEnableLightShaftBloom(bP23OriginalMoonLightShaftBloom);
		}
		bP23HasAppliedSunLux = false;
		if (bP24SavedSkyScattering && P24SkyScatteringOwner.IsValid())
		{
			P24SkyScatteringOwner->SetRayleighScattering(P24OriginalRayleigh);
			P24SkyScatteringOwner->SetMieScattering(P24OriginalMie);
			P24SkyScatteringOwner.Reset();
			bP24SavedSkyScattering = false;
		}
		bP23CachedSunLightShaftBloom = false;
		bP23CachedMoonLightShaftBloom = false;
		MoonDirectionalLightComponent->SetIntensity(0.0f);
		MoonDirectionalLightComponent->SetVisibility(false, true);
		MoonDirectionalLightComponent->SetForwardShadingPriority(0);

		if (bOwnsWorldDayNight && CachedDayNightSunLight.IsValid())
		{
			if (bCachedDayNightSunForwardPriority)
			{
				CachedDayNightSunLight->SetForwardShadingPriority(CachedDayNightSunForwardPriority);
			}
			if (bCachedDayNightSunBaseColor)
			{
				CachedDayNightSunLight->SetLightColor(CachedDayNightSunBaseColor);
			}
			if (bCachedDayNightSunSourceAngle)
			{
				CachedDayNightSunLight->SetLightSourceAngle(OriginalDayNightSunSourceAngle);
			}
			// The authored base Lux and light-shaft bloom were restored above.
		}
		bCachedDayNightSunForwardPriority = false;
		bCachedDayNightSunBaseColor = false;
		bCachedDayNightSunBaseIntensity = false;
		CachedDayNightSunLight.Reset();
		bHasLastDayNightSunHeight = false;

		// Only the dedicated Fog weather owns SkyAtmosphere. Cross-weather fog is
		// additive and must never take ownership of Rain/Snow/Clear/Dawn sky lighting.
		const bool bWeatherFogOwnsSkyOnReset =
			GaussianSplatComponent->WeatherMode == ELCCCinematicGSWeatherMode::Fog &&
			GaussianSplatComponent->FogWeatherStrength > 0.001f;
		if (bOwnsWorldDayNight && !bWeatherFogOwnsSkyOnReset &&
			bCachedDayNightBaseSkyFactors && CachedDayNightBaseSkyAtmosphere.IsValid())
		{
			CachedDayNightBaseSkyAtmosphere->SetSkyLuminanceFactor(DayNightBaseSkyLuminanceFactor);
			CachedDayNightBaseSkyAtmosphere->SetSkyAndAerialPerspectiveLuminanceFactor(DayNightBaseSkyAerialLuminanceFactor);
		}
		if (bOwnsWorldDayNight)
		{
			ENQUEUE_RENDER_COMMAND(DisableGaussianMoonWetCue)(
				[](FRHICommandListImmediate&)
				{
					FGaussianSplatRenderer::SetDirectMoonReflectionState(
						FVector3f(0.0f, 0.0f, 1.0f), FVector3f(0.0f, 0.0f, 0.0f), 0.0f, 0.7357f, false);
				});
		}
		return;
	}

	// Prefer the authored Atmosphere Sun Light Index 0 when present. Falling back to
	// the strongest external Directional Light preserves old levels that never enabled
	// Atmosphere Sun Light explicitly. Plugin-generated moons are never sun candidates.
	UDirectionalLightComponent* AuthoredSun = nullptr;
	float BestSunScore = -1.0f;
	for (TObjectIterator<UDirectionalLightComponent> It; It; ++It)
	{
		UDirectionalLightComponent* Candidate = *It;
		bool bIsGeneratedMoon = Candidate == MoonDirectionalLightComponent;
		if (Candidate)
		{
			if (AGaussianSplatActor* CandidateOwner = Cast<AGaussianSplatActor>(Candidate->GetOwner()))
			{
				bIsGeneratedMoon = bIsGeneratedMoon || CandidateOwner->MoonDirectionalLightComponent == Candidate;
			}
		}
		if (!Candidate || bIsGeneratedMoon || Candidate->GetWorld() != World ||
			!Candidate->IsRegistered() || !Candidate->IsVisible())
		{
			continue;
		}

		const float CandidateIntensity = FMath::Max(0.0f, Candidate->Intensity);
		const bool bPrimaryAtmosphereSun = Candidate->bAtmosphereSunLight && Candidate->GetAtmosphereSunLightIndex() == 0u;
		const float CandidateScore = CandidateIntensity + (bPrimaryAtmosphereSun ? 1000000.0f : 0.0f);
		if (!AuthoredSun || CandidateScore > BestSunScore)
		{
			AuthoredSun = Candidate;
			BestSunScore = CandidateScore;
		}
	}

	if (!AuthoredSun)
	{
		MoonDirectionalLightComponent->SetIntensity(0.0f);
		MoonDirectionalLightComponent->SetVisibility(false, true);
		MoonDirectionalLightComponent->SetForwardShadingPriority(0);
		if (bOwnsWorldDayNight)
		{
			ENQUEUE_RENDER_COMMAND(DisableGaussianMoonWetCueNoSun)(
				[](FRHICommandListImmediate&)
				{
					FGaussianSplatRenderer::SetDirectMoonReflectionState(
						FVector3f(0.0f, 0.0f, 1.0f), FVector3f(0.0f, 0.0f, 0.0f), 0.0f, 0.7357f, false);
				});
		}
		return;
	}

	// Remember the user's authored forward priority and restore it when the selected
	// sun changes or when the automatic cycle is disabled.
	if (CachedDayNightSunLight.Get() != AuthoredSun)
	{
		if (CachedDayNightSunLight.IsValid())
		{
			if (bCachedDayNightSunForwardPriority)
			{
				CachedDayNightSunLight->SetForwardShadingPriority(CachedDayNightSunForwardPriority);
			}
			if (bCachedDayNightSunBaseColor)
			{
				CachedDayNightSunLight->SetLightColor(CachedDayNightSunBaseColor);
			}
			if (bCachedDayNightSunBaseIntensity && bP23HasAppliedSunLux)
			{
				CachedDayNightSunLight->SetIntensity(CachedDayNightSunBaseIntensity);
			}
			if (bP23CachedSunLightShaftBloom)
			{
				CachedDayNightSunLight->SetEnableLightShaftBloom(bP23OriginalSunLightShaftBloom);
			}
			bP23CachedSunLightShaftBloom = false;
			bP23HasAppliedSunLux = false;
		}
		CachedDayNightSunLight = AuthoredSun;
		bP23HasAppliedSunLux = false;
		P23LastAppliedSunLux = -1.0f;
		bP23OriginalSunLightShaftBloom = AuthoredSun->bEnableLightShaftBloom != 0;
		bP23CachedSunLightShaftBloom = true;
		CachedDayNightSunForwardPriority = AuthoredSun->ForwardShadingPriority;
		bCachedDayNightSunForwardPriority = true;
		CachedDayNightSunBaseColor = AuthoredSun->GetLightColor();
		// Recover authored daytime colour when migrating a level that was saved
		// during a black/zero-energy sky state. Never modify the authored Lux.
		if (CachedDayNightSunBaseColor.R + CachedDayNightSunBaseColor.G + CachedDayNightSunBaseColor.B < 0.001f)
		{
			CachedDayNightSunBaseColor = FLinearColor::White;
		}
		bCachedDayNightSunBaseColor = true;
		CachedDayNightSunBaseIntensity = FMath::Max(AuthoredSun->Intensity, 0.0f);
		bCachedDayNightSunBaseIntensity = true;
		OriginalDayNightSunSourceAngle = AuthoredSun->LightSourceAngle;
		bCachedDayNightSunSourceAngle = true;
		bHasLastDayNightSunHeight = false;
	}

	// Make the authored primary light explicit for SkyAtmosphere. This does not change
	// its transform/intensity; it only gives the atmosphere a deterministic Sun slot.
	if (bOwnsWorldDayNight)
	{
		AuthoredSun->SetAtmosphereSunLight(true);
		AuthoredSun->SetAtmosphereSunLightIndex(0);
		const float DesiredSunAngle = FMath::Clamp(GaussianSplatComponent->ManagedSunDiskAngleDegrees, 0.53f, 3.00f);
		if (!FMath::IsNearlyEqual(AuthoredSun->LightSourceAngle, DesiredSunAngle, 0.005f))
		{
			AuthoredSun->SetLightSourceAngle(DesiredSunAngle);
		}
		// P22: scale only the actual atmospheric disk, rather than growing a
		// camera-facing billboard or moving anything toward the camera.
		const float DiskGain = FMath::Clamp(GaussianSplatComponent->ManagedSunDiskEmissionGain, 0.05f, 2.0f);
		AuthoredSun->SetAtmosphereSunDiskColorScale(FLinearColor(DiskGain, DiskGain, DiskGain, 1.0f));
	}

	const FVector SunSurfaceToLight = (-AuthoredSun->GetForwardVector()).GetSafeNormal();
	const float SunHeight = (float)SunSurfaceToLight.Z;
	const float SolarElevationDegrees = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(SunHeight, -1.0f, 1.0f)));
	GaussianSplatComponent->RuntimeDirectionalLightPitchDegrees = AuthoredSun->GetComponentRotation().Pitch;
	GaussianSplatComponent->RuntimeSolarElevationDegrees = SolarElevationDegrees;
	const float TransitionStart = FMath::Max(
		GaussianSplatComponent->DayNightTransitionStart,
		GaussianSplatComponent->DayNightFullNight + 0.001f);
	const float FullNight = FMath::Min(GaussianSplatComponent->DayNightFullNight, TransitionStart - 0.001f);
	// v2797: use one deliberately broad day->twilight->night envelope instead of the
	// narrow v2796 horizon band. With the new defaults this spans roughly +15deg to
	// -17deg solar elevation, so rotating the authored sun never causes a one-frame
	// daylight/night jump near the horizon.
	const float DayPresence = SmoothCelestialBlend(FullNight, TransitionStart, SunHeight);
	float NightBlend = 1.0f - DayPresence;
	const bool bUseUnifiedTOD = GaussianSplatComponent->bUseUnifiedPhysicalTimeOfDay;
	FUnifiedPhysicalTODState UnifiedTODState;
	GaussianSplatComponent->RuntimeNightBlend = NightBlend;

	// P23: Directional Intensity carries photometric energy; RGB carries hue only.
	// The two controls must never be multiplied together to remove the sky's chroma.
	const float SunEnergyPhase = SmootherCelestialBlend(-0.18f, 0.22f, SunHeight);
	if (bOwnsWorldDayNight && bCachedDayNightSunBaseIntensity &&
		!GaussianSplatComponent->bP40ManualSunLuxMode &&
		GaussianSplatComponent->P23BaseSunLux < 0.0f)
	{
		// While the user edits the actual Directional Light in Details, treat a value
		// DIFFERENT from the value we applied last tick as a new authored baseline.
		const float CurrentLux = FMath::Max(AuthoredSun->Intensity, 0.0f);
		const float Tolerance = FMath::Max(0.05f, 0.001f * FMath::Abs(P23LastAppliedSunLux));
		if (!bP23HasAppliedSunLux || FMath::Abs(CurrentLux - P23LastAppliedSunLux) > Tolerance)
		{
			CachedDayNightSunBaseIntensity = CurrentLux;
		}
	}
	if (bOwnsWorldDayNight)
	{
		if (GaussianSplatComponent->bP23DisableLightShaftBloom)
		{
			if (AuthoredSun->bEnableLightShaftBloom) AuthoredSun->SetEnableLightShaftBloom(false);
		}
		else if (bP23CachedSunLightShaftBloom)
		{
			AuthoredSun->SetEnableLightShaftBloom(bP23OriginalSunLightShaftBloom);
		}
	}

	// v2799: distinguish sunrise from sunset using the signed change in solar elevation.
	// A single elevation value is physically ambiguous; keeping the last non-trivial
	// derivative lets a manually rotated Directional Light select a dedicated dawn look.
	if (!bHasLastDayNightSunHeight)
	{
		LastDayNightSunHeight = SunHeight;
		bHasLastDayNightSunHeight = true;
	}
	else
	{
		const float SunHeightDelta = SunHeight - LastDayNightSunHeight;
		if (FMath::Abs(SunHeightDelta) > 0.00025f)
		{
			bDayNightSunRising = SunHeightDelta > 0.0f;
		}
		LastDayNightSunHeight = SunHeight;
	}
	// P32: sunrise and sunset share the same elevation; without movement history,
	// the artist must be able to explicitly choose which colour palette to preview.
	if (GaussianSplatComponent->P32DawnDuskMode == 1) bDayNightSunRising = true;
	else if (GaussianSplatComponent->P32DawnDuskMode == 2) bDayNightSunRising = false;
	GaussianSplatComponent->bRuntimeSolarRising = bDayNightSunRising;

	if (bUseUnifiedTOD)
	{
		UnifiedTODState = EvaluateUnifiedPhysicalTimeOfDay(
			SolarElevationDegrees, bDayNightSunRising, GaussianSplatComponent);

		// Compatibility phase for the existing moon/shadow bridge only. Visible lighting
		// no longer uses discrete time bands when the unified model is enabled.
		NightBlend = 1.0f - SmootherCelestialBlend(-18.0f, 6.0f, SolarElevationDegrees);
		GaussianSplatComponent->RuntimeNightBlend = FMath::Clamp(NightBlend, 0.0f, 1.0f);
		GaussianSplatComponent->RuntimeUnifiedTODDirectEnergy = UnifiedTODState.DirectEnergy;
		GaussianSplatComponent->RuntimeUnifiedTODSkyEnergy = UnifiedTODState.SkyEnergy;
		GaussianSplatComponent->RuntimeUnifiedTODCarrierBrightness = UnifiedTODState.CarrierBrightness;
		GaussianSplatComponent->RuntimeUnifiedTODGamma = UnifiedTODState.Gamma;
		GaussianSplatComponent->RuntimeUnifiedTODSaturation = UnifiedTODState.Saturation;
		GaussianSplatComponent->RuntimeUnifiedTODAfternoonLift = UnifiedTODState.AfternoonLift;
		GaussianSplatComponent->RuntimeUnifiedTODSunColor = UnifiedTODState.SunColor;
		GaussianSplatComponent->RuntimeUnifiedTODAmbientColor = UnifiedTODState.AmbientColor;
		GaussianSplatComponent->RuntimeUnifiedTODGradeStrength = UnifiedTODState.GradeStrength;
		GaussianSplatComponent->RuntimeTimeOfDayGradeColor = UnifiedTODState.AmbientColor;
		GaussianSplatComponent->RuntimeTimeOfDayGradeStrength = UnifiedTODState.GradeStrength;
		GaussianSplatComponent->RuntimeNightEnvironmentColor = UnifiedTODState.AmbientColor;
		GaussianSplatComponent->RuntimeNightEnvironmentEnergyScale = UnifiedTODState.SkyEnergy;
		GaussianSplatComponent->RuntimeNightGradeGamma = UnifiedTODState.Gamma;
		GaussianSplatComponent->RuntimeNightGradeSaturation = UnifiedTODState.Saturation;
		GaussianSplatComponent->RuntimeNightGradeBrightness = UnifiedTODState.CarrierBrightness;

		if (bOwnsWorldDayNight && bCachedDayNightSunBaseColor)
		{
			const float ColorStrength = FMath::Clamp(GaussianSplatComponent->UnifiedTODColorStrength, 0.0f, 1.5f);
			FLinearColor SunOutputColor = FMath::Lerp(
				CachedDayNightSunBaseColor, UnifiedTODState.SunColor, FMath::Clamp(ColorStrength, 0.0f, 1.0f));
			// Small physically-inspired twilight tail: lets SkyAtmosphere form a warm
			// horizon until ~-4 deg, without relighting 3DGS throughout the night.
			// Hue must stay intact through sunset and night; Lux is animated below.
			SunOutputColor.A = 1.0f;
			AuthoredSun->SetLightColor(SunOutputColor);
		}
	}

	// v2820 ANGLE->CLOCK DISPLAY. A Directional Light rotation already contains the
	// complete time-of-day signal used by the lighting system. Convert its solar
	// elevation to an equinox-style 24-hour clock (sunrise 06:00, noon 12:00, sunset
	// 18:00), using the existing rising/setting state to resolve the two possible times
	// for the same elevation. This is display/Blueprint state only; it never drives or
	// rewrites the user's Directional Light transform.
	float SolarTimeHours = bDayNightSunRising
		? (SolarElevationDegrees + 90.0f) / 15.0f
		: 18.0f - SolarElevationDegrees / 15.0f;
	SolarTimeHours = FMath::Fmod(SolarTimeHours + 24.0f, 24.0f);
	GaussianSplatComponent->RuntimeTimeOfDayHours = SolarTimeHours;
	const int32 SolarTotalMinutes = FMath::RoundToInt(SolarTimeHours * 60.0f) % (24 * 60);
	const int32 SolarHour = SolarTotalMinutes / 60;
	const int32 SolarMinute = SolarTotalMinutes % 60;
	GaussianSplatComponent->RuntimeTimeOfDayText = FString::Printf(TEXT("%02d:%02d"), SolarHour, SolarMinute);
	if (SolarElevationDegrees >= 20.0f)
	{
		GaussianSplatComponent->RuntimeTimeOfDayPhase = TEXT("Day");
	}
	else if (SolarElevationDegrees >= 6.0f)
	{
		GaussianSplatComponent->RuntimeTimeOfDayPhase = bDayNightSunRising ? TEXT("Morning Golden Hour") : TEXT("Golden Hour");
	}
	else if (SolarElevationDegrees >= 0.0f)
	{
		GaussianSplatComponent->RuntimeTimeOfDayPhase = bDayNightSunRising ? TEXT("Sunrise") : TEXT("Sunset");
	}
	else if (SolarElevationDegrees >= -6.0f)
	{
		GaussianSplatComponent->RuntimeTimeOfDayPhase = TEXT("Civil Twilight");
	}
	else if (SolarElevationDegrees >= -12.0f)
	{
		GaussianSplatComponent->RuntimeTimeOfDayPhase = TEXT("Nautical / Blue Hour");
	}
	else if (SolarElevationDegrees >= -18.0f)
	{
		GaussianSplatComponent->RuntimeTimeOfDayPhase = TEXT("Astronomical Twilight");
	}
	else
	{
		const float HourFromMidnight = FMath::Min(FMath::Abs(SolarTimeHours), 24.0f - FMath::Abs(SolarTimeHours));
		if (HourFromMidnight <= 1.5f)
		{
			GaussianSplatComponent->RuntimeTimeOfDayPhase = TEXT("Solar Midnight");
		}
		else if (bDayNightSunRising)
		{
			GaussianSplatComponent->RuntimeTimeOfDayPhase = TEXT("Late Night / Pre-dawn");
		}
		else
		{
			GaussianSplatComponent->RuntimeTimeOfDayPhase = TEXT("Early Night");
		}
	}

	// v2828 PHYSICALLY-ANCHORED NIGHT STAGING. Use the standard solar-depression
	// boundaries (0/-6/-12/-18 degrees) as physical anchors, then vary the fully
	// dark interval around midnight so a long night is not one static blue grade.
	// This drives both the GS carrier and the SkyAtmosphere bridge; source PLY color
	// and the user's authored Directional Light rotation remain untouched.
	const float SolarDepressionDegrees = FMath::Max(-SolarElevationDegrees, 0.0f);
	const float MidnightDistanceHours = FMath::Min(FMath::Abs(SolarTimeHours), 24.0f - FMath::Abs(SolarTimeHours));
	const float MidnightBlend = 1.0f - SmootherCelestialBlend(1.25f, 4.25f, MidnightDistanceHours);
	FLinearColor StagedNightColor = GaussianSplatComponent->NightAmbientColor;
	float StagedNightEnergy = 1.0f;
	float StagedNightGamma = GaussianSplatComponent->NightGamma;
	float StagedNightSaturation = GaussianSplatComponent->NightSaturation;
	float StagedNightBrightness = GaussianSplatComponent->NightBrightness;

	if (bUseUnifiedTOD)
	{
		StagedNightColor = UnifiedTODState.AmbientColor;
		StagedNightEnergy = UnifiedTODState.SkyEnergy;
		StagedNightGamma = UnifiedTODState.Gamma;
		StagedNightSaturation = UnifiedTODState.Saturation;
		StagedNightBrightness = UnifiedTODState.CarrierBrightness;
	}
	else if (SolarElevationDegrees < 0.0f)
	{
		// Evening moves warm/lavender -> cobalt -> desaturated navy. Deep night is
		// intentionally less saturated than blue hour; solar midnight is the darkest
		// point when the Sun is lowest below the horizon (moon/weather can of course vary).
		const FLinearColor CivilHorizonColor = bDayNightSunRising
			? FLinearColor(0.48f, 0.50f, 0.70f, 1.0f)
			: FLinearColor(0.50f, 0.44f, 0.58f, 1.0f);
		const FLinearColor CivilEndColor = bDayNightSunRising
			? FLinearColor(0.32f, 0.38f, 0.64f, 1.0f)
			: FLinearColor(0.28f, 0.34f, 0.61f, 1.0f);
		const FLinearColor NauticalEndColor = bDayNightSunRising
			? FLinearColor(0.20f, 0.25f, 0.46f, 1.0f)
			: FLinearColor(0.16f, 0.22f, 0.43f, 1.0f);
		const FLinearColor AstronomicalEndColor = bDayNightSunRising
			? FLinearColor(0.115f, 0.145f, 0.255f, 1.0f)
			: FLinearColor(0.090f, 0.125f, 0.230f, 1.0f);
		const FLinearColor EarlyLateNightColor = bDayNightSunRising
			? FLinearColor(0.095f, 0.115f, 0.205f, 1.0f)
			: FLinearColor(0.075f, 0.095f, 0.175f, 1.0f);
		const FLinearColor MidnightColor(0.038f, 0.050f, 0.082f, 1.0f);

		if (SolarDepressionDegrees < 6.0f)
		{
			const float T = SmootherCelestialBlend(0.0f, 6.0f, SolarDepressionDegrees);
			StagedNightColor = FMath::Lerp(CivilHorizonColor, CivilEndColor, T);
			StagedNightEnergy = FMath::Lerp(1.08f, 0.94f, T);
			StagedNightGamma = FMath::Lerp(1.02f, 1.10f, T);
			StagedNightSaturation = FMath::Lerp(0.94f, 0.84f, T);
			StagedNightBrightness = FMath::Lerp(0.92f, 0.80f, T);
		}
		else if (SolarDepressionDegrees < 12.0f)
		{
			const float T = SmootherCelestialBlend(6.0f, 12.0f, SolarDepressionDegrees);
			StagedNightColor = FMath::Lerp(CivilEndColor, NauticalEndColor, T);
			StagedNightEnergy = FMath::Lerp(0.94f, 0.78f, T);
			StagedNightGamma = FMath::Lerp(1.10f, 1.20f, T);
			StagedNightSaturation = FMath::Lerp(0.84f, 0.70f, T);
			StagedNightBrightness = FMath::Lerp(0.80f, 0.68f, T);
		}
		else if (SolarDepressionDegrees < 18.0f)
		{
			const float T = SmootherCelestialBlend(12.0f, 18.0f, SolarDepressionDegrees);
			StagedNightColor = FMath::Lerp(NauticalEndColor, AstronomicalEndColor, T);
			StagedNightEnergy = FMath::Lerp(0.78f, 0.66f, T);
			StagedNightGamma = FMath::Lerp(1.20f, 1.28f, T);
			StagedNightSaturation = FMath::Lerp(0.70f, 0.56f, T);
			StagedNightBrightness = FMath::Lerp(0.68f, 0.58f, T);
		}
		else
		{
			StagedNightColor = FMath::Lerp(EarlyLateNightColor, MidnightColor, MidnightBlend);
			StagedNightEnergy = FMath::Lerp(0.66f, 0.50f, MidnightBlend);
			StagedNightGamma = FMath::Lerp(1.28f, 1.38f, MidnightBlend);
			StagedNightSaturation = FMath::Lerp(0.56f, 0.40f, MidnightBlend);
			StagedNightBrightness = FMath::Lerp(0.58f, 0.49f, MidnightBlend);
		}

		// Pre-dawn reference images are typically a little more lavender and readable
		// than post-midnight frames at the same nominal darkness. Keep that asymmetry.
		if (bDayNightSunRising)
		{
			StagedNightEnergy *= 1.055f;
			StagedNightGamma = FMath::Max(1.0f, StagedNightGamma - 0.035f);
			StagedNightSaturation = FMath::Min(StagedNightSaturation + 0.045f, 1.0f);
			StagedNightBrightness = FMath::Min(StagedNightBrightness + 0.035f, 1.0f);
		}
	}

	GaussianSplatComponent->RuntimeNightEnvironmentColor = StagedNightColor;
	GaussianSplatComponent->RuntimeNightEnvironmentEnergyScale = bUseUnifiedTOD
		? FMath::Clamp(StagedNightEnergy, 0.0f, 1.0f)
		: FMath::Clamp(StagedNightEnergy, 0.45f, 1.25f);
	GaussianSplatComponent->RuntimeNightGradeGamma = FMath::Clamp(StagedNightGamma, 0.75f, 1.60f);
	GaussianSplatComponent->RuntimeNightGradeSaturation = FMath::Clamp(StagedNightSaturation, 0.38f, 1.10f);
	GaussianSplatComponent->RuntimeNightGradeBrightness = FMath::Clamp(StagedNightBrightness, 0.46f, 1.05f);

	UpdateTimeOfDayScreenOverlay(GaussianSplatComponent);

	// v2799: much broader, quintic phase windows. The artist softness parameter expands
	// the solar-elevation range instead of speeding a filter in screen space, so the same
	// Directional-Light angle always produces the same look regardless of frame rate.
	const float PhaseSoftness = FMath::Clamp(GaussianSplatComponent->TimeOfDayColorTransitionSoftness, 0.0f, 1.0f);
	const float PhaseSpread = FMath::Lerp(0.72f, 1.38f, PhaseSoftness);
	const float GoldenHourBlend = FMath::Clamp(
		(1.0f - SmootherCelestialBlend(0.22f * PhaseSpread, 0.48f * PhaseSpread, SunHeight)) *
		SmootherCelestialBlend(-0.12f * PhaseSpread, 0.10f * PhaseSpread, SunHeight), 0.0f, 1.0f);
	const float SunsetBlend = FMath::Clamp(
		(1.0f - SmootherCelestialBlend(0.02f * PhaseSpread, 0.24f * PhaseSpread, SunHeight)) *
		SmootherCelestialBlend(-0.24f * PhaseSpread, -0.04f * PhaseSpread, SunHeight), 0.0f, 1.0f);
	const float BlueHourBlend = FMath::Clamp(
		(1.0f - SmootherCelestialBlend(-0.10f * PhaseSpread, 0.05f * PhaseSpread, SunHeight)) *
		SmootherCelestialBlend(-0.50f * PhaseSpread, -0.24f * PhaseSpread, SunHeight), 0.0f, 1.0f);
	const float TwilightBlend = FMath::Clamp(FMath::Max(GoldenHourBlend, SunsetBlend), 0.0f, 1.0f);
	GaussianSplatComponent->RuntimeGoldenHourBlend = GoldenHourBlend;
	GaussianSplatComponent->RuntimeSunsetBlend = SunsetBlend;
	GaussianSplatComponent->RuntimeBlueHourBlend = BlueHourBlend;
	GaussianSplatComponent->RuntimeTwilightBlend = TwilightBlend;

	// Dawn is intentionally not a time-reversed copy of sunset. Morning uses a lighter
	// lavender blue-hour, a peach/pink horizon and a cleaner warm-gold exit into daytime.
	const FLinearColor ActiveGoldenColor = bDayNightSunRising
		? GaussianSplatComponent->DawnGoldenHourAmbientColor
		: GaussianSplatComponent->GoldenHourAmbientColor;
	const FLinearColor ActiveHorizonColor = bDayNightSunRising
		? GaussianSplatComponent->DawnHorizonAmbientColor
		: GaussianSplatComponent->TwilightAmbientColor;
	const FLinearColor ActiveBlueColor = bDayNightSunRising
		? GaussianSplatComponent->DawnBlueHourAmbientColor
		: GaussianSplatComponent->BlueHourAmbientColor;

	// Blend phase colours by normalized weights rather than three sequential LERPs.
	// Sequential LERPs made whichever band updated last appear to snap over the others.
	const float PhaseWeightSum = GoldenHourBlend + SunsetBlend + BlueHourBlend;
	FLinearColor TimeOfDayGrade = FLinearColor::White;
	float PhasePresence = 0.0f;
	if (bUseUnifiedTOD)
	{
		TimeOfDayGrade = UnifiedTODState.AmbientColor;
		PhasePresence = UnifiedTODState.GradeStrength;
		GaussianSplatComponent->RuntimeTimeOfDayGradeColor = UnifiedTODState.AmbientColor;
		GaussianSplatComponent->RuntimeTimeOfDayGradeStrength = UnifiedTODState.GradeStrength;
	}
	else
	{
		if (PhaseWeightSum > 0.0001f)
		{
			const float InvPhaseWeight = 1.0f / PhaseWeightSum;
			TimeOfDayGrade =
				ActiveGoldenColor * (GoldenHourBlend * InvPhaseWeight) +
				ActiveHorizonColor * (SunsetBlend * InvPhaseWeight) +
				ActiveBlueColor * (BlueHourBlend * InvPhaseWeight);
			TimeOfDayGrade.A = 1.0f;
		}
		PhasePresence = FMath::Clamp(
			FMath::Max3(GoldenHourBlend * 0.78f, SunsetBlend, BlueHourBlend * 0.86f), 0.0f, 1.0f);
		GaussianSplatComponent->RuntimeTimeOfDayGradeColor = TimeOfDayGrade;
		GaussianSplatComponent->RuntimeTimeOfDayGradeStrength = FMath::Clamp(
			PhasePresence * FMath::Clamp(GaussianSplatComponent->TimeOfDayGlobalTintStrength, 0.0f, 1.0f), 0.0f, 1.0f);
	}

	// Drive the actual atmospheric Sun colour with only the warm bands. Blue hour is
	// atmosphere/ambient-owned once the Sun is below the horizon.
	if (!bUseUnifiedTOD && bOwnsWorldDayNight && bCachedDayNightSunBaseColor)
	{
		const float WarmWeightSum = GoldenHourBlend + SunsetBlend;
		FLinearColor WarmSolarColor = CachedDayNightSunBaseColor;
		if (WarmWeightSum > 0.0001f)
		{
			WarmSolarColor =
				ActiveGoldenColor * (GoldenHourBlend / WarmWeightSum) +
				ActiveHorizonColor * (SunsetBlend / WarmWeightSum);
			WarmSolarColor.A = 1.0f;
		}
		const float WarmSolarStrength = FMath::Clamp(
			FMath::Max(GoldenHourBlend * 0.52f, SunsetBlend * (bDayNightSunRising ? 0.58f : 0.72f)),
			0.0f, 0.82f);
		FLinearColor SunOutputColor = FMath::Lerp(CachedDayNightSunBaseColor, WarmSolarColor, WarmSolarStrength);
		SunOutputColor.A = 1.0f;
		AuthoredSun->SetLightColor(SunOutputColor);
	}

	// P24: the actual Sun RGB follows the existing authored dawn/golden/sunset
	// colours (shared with the 3DGS grading); intensity remains Lux-only below.
	if (bOwnsWorldDayNight && GaussianSplatComponent->bP24UnifiedAtmosphereLighting && bCachedDayNightSunBaseColor)
	{
		const float WarmSum = GoldenHourBlend + SunsetBlend;
		if (WarmSum > 1.0e-4f)
		{
			FLinearColor WarmTint = (ActiveGoldenColor * GoldenHourBlend + ActiveHorizonColor * SunsetBlend) / WarmSum;
			WarmTint.A = 1.0f;
			const float HueMix = FMath::Clamp(FMath::Max(GoldenHourBlend, SunsetBlend) *
				GaussianSplatComponent->ManagedSkyTimeColorSyncStrength, 0.0f, 0.90f);
			const FLinearColor LightHue = NormalizeCapturedSkyTint(
				FMath::Lerp(CachedDayNightSunBaseColor, WarmTint, HueMix));
			AuthoredSun->SetLightColor(LightHue);
		}
	}

	// P23: Lux is the only scalar that fades with elevation. The reference is
	// stored separately from the runtime Intensity, so the user can edit either the
	// P23 reference field or the directional light itself (auto reference mode).
	if (bOwnsWorldDayNight && bCachedDayNightSunBaseIntensity)
	{
		// P24 reference never reads the night-time applied Intensity back as the design lux.
		const float ReferenceLux = GaussianSplatComponent->bP24UnifiedAtmosphereLighting
			? FMath::Clamp(GaussianSplatComponent->P24ReferenceSunLux, 0.0f, 300000.0f)
			: (GaussianSplatComponent->P23BaseSunLux >= 0.0f
				? GaussianSplatComponent->P23BaseSunLux : CachedDayNightSunBaseIntensity);
		const float SunWeight = bUseUnifiedTOD
			? FMath::Clamp(UnifiedTODState.DirectEnergy + 0.006f * SmootherCelestialBlend(-5.0f, 0.8f, SolarElevationDegrees), 0.0f, 1.5f)
			: FMath::Clamp(SunEnergyPhase, 0.0f, 1.0f);
		if (GaussianSplatComponent->bP40ManualSunLuxMode)
		{
			// P40: explicit control for backdrop mesh tests. The time of day, sky,
			// moon and GS relighting still update normally. The Sun's automatically
			// computed Intensity is the only value bypassed here.
			const float RequestedManualLux = GaussianSplatComponent->P40ManualSunLux;
			if (RequestedManualLux >= 0.0f)
			{
				const float ManualLux = FMath::Clamp(RequestedManualLux, 0.0f, 300000.0f);
				if (!FMath::IsNearlyEqual(AuthoredSun->Intensity, ManualLux, FMath::Max(0.01f, ManualLux * 0.0001f)))
				{
					AuthoredSun->SetIntensity(ManualLux);
				}
				GaussianSplatComponent->RuntimeP23LightingStatus = FString::Printf(
					TEXT("P40 수동 고정: 태양 %.2f Lux / 고도 %.1f° / 시간대 색상·달 유지"),
					ManualLux, SolarElevationDegrees);
			}
			else
			{
				// -1: leave the Directional Light Details Intensity under artist
				// control without any write-back from this plugin.
				GaussianSplatComponent->RuntimeP23LightingStatus = FString::Printf(
					TEXT("P40 라이트 직접 편집: 태양 %.2f Lux / 고도 %.1f° / 자동 덮어쓰기 중단"),
					FMath::Max(0.0f, AuthoredSun->Intensity), SolarElevationDegrees);
			}
			P23LastAppliedSunLux = FMath::Max(0.0f, AuthoredSun->Intensity);
			bP23HasAppliedSunLux = true;
		}
		else if (GaussianSplatComponent->bP24UnifiedAtmosphereLighting || GaussianSplatComponent->bP23AngleDrivenSunLux)
		{
			// Only solar twilight (not the full night) retains a small positive lux.
			// At full night the independent Moon reference above provides sky illumination.
			const float TwilightWeight = SmootherCelestialBlend(-9.0f, -1.0f, SolarElevationDegrees)
				* (1.0f - SmootherCelestialBlend(0.0f, 8.0f, SolarElevationDegrees));
			const float TwilightTail = GaussianSplatComponent->bP24UnifiedAtmosphereLighting
				? FMath::Max(0.0f, GaussianSplatComponent->P24TwilightSunLux) * TwilightWeight : 0.0f;
			const float SunGain = GaussianSplatComponent->bP24UnifiedAtmosphereLighting
				? 1.0f : FMath::Clamp(GaussianSplatComponent->P23SunLuxGain, 0.0f, 2.0f);
            // P27: cap only the current solar intensity, never the user's base Lux.
            // Keep the original low-light curve and moon transition intact.
            const float DaySunLuxCap = GaussianSplatComponent->bP24UnifiedAtmosphereLighting
                ? FMath::Clamp(GaussianSplatComponent->P27SunLuxLimit, 0.0f, 300000.0f)
                : 450000.0f;
			const float AppliedLux = FMath::Clamp(ReferenceLux * SunGain * SunWeight + TwilightTail, 0.0f, DaySunLuxCap);
			if (!FMath::IsNearlyEqual(AuthoredSun->Intensity, AppliedLux, FMath::Max(0.01f, AppliedLux * 0.0001f)))
			{
				AuthoredSun->SetIntensity(AppliedLux);
			}
			P23LastAppliedSunLux = AppliedLux;
			bP23HasAppliedSunLux = true;
			GaussianSplatComponent->RuntimeP23LightingStatus = FString::Printf(
				TEXT("태양 기준 %.1f Lux → 실제 %.2f Lux / 고도 %.1f° / RGB 별도 / 달 최소 %.2f Lux"),
				ReferenceLux, AppliedLux, SolarElevationDegrees,
				GaussianSplatComponent->bP24UnifiedAtmosphereLighting
					? GaussianSplatComponent->P24MoonSkyLux : GaussianSplatComponent->P23MoonSkyFillLux);
		}
		else
		{
			if (bP23HasAppliedSunLux)
			{
				AuthoredSun->SetIntensity(ReferenceLux);
				bP23HasAppliedSunLux = false;
			}
			GaussianSplatComponent->RuntimeP23LightingStatus = TEXT("태양 Lux 수동 유지 / 자동 고도 계산 해제");
		}
	}

	// v2800: place the moon on the opposite side of the celestial sphere.
	// v2799 mirrored only Z, which kept the moon on the sun's horizontal azimuth and
	// made Stable shadows appear to rotate mostly on one side. A full 180-degree
	// inversion makes sunset/sunrise and moonrise/moonset occupy opposite horizons,
	// so the horizontal shadow direction reverses when the celestial source does.
	FVector MoonSurfaceToLight = (-SunSurfaceToLight).GetSafeNormal();
	if (MoonSurfaceToLight.IsNearlyZero())
	{
		MoonSurfaceToLight = FVector::UpVector;
	}
	const FVector MoonForward = -MoonSurfaceToLight;
	MoonDirectionalLightComponent->SetWorldRotation(FRotationMatrix::MakeFromX(MoonForward).Rotator());

	// v2819 split visual-light authority from Stable shadow authority. The actual
	// Sun/Moon direct-light direction may switch to the opposite celestial source at
	// night, but a cast field must never rotate 180 degrees on the exact horizon frame.
	const bool bMoonAboveHorizon = SunHeight < 0.0f;
	GaussianSplatComponent->RuntimeActiveCelestialDirection =
		bMoonAboveHorizon ? MoonSurfaceToLight : SunSurfaceToLight;
	GaussianSplatComponent->bRuntimeActiveCelestialDirectionValid = true;

	FVector ContinuousShadowDirection(SunSurfaceToLight.X, SunSurfaceToLight.Y, FMath::Abs(SunSurfaceToLight.Z));
	ContinuousShadowDirection = ContinuousShadowDirection.GetSafeNormal();
	if (ContinuousShadowDirection.IsNearlyZero())
	{
		ContinuousShadowDirection = FVector::UpVector;
	}
	GaussianSplatComponent->RuntimeStableShadowDirection = ContinuousShadowDirection;
	GaussianSplatComponent->bRuntimeStableShadowDirectionValid = true;
	GaussianSplatComponent->bRuntimeActiveCelestialIsMoon = bMoonAboveHorizon;

	const FLinearColor MoonColor = GaussianSplatComponent->bP24UnifiedAtmosphereLighting
		? NormalizeCapturedSkyTint(FMath::Lerp(GaussianSplatComponent->MoonDirectionalColor,
			GaussianSplatComponent->NightAmbientColor, 0.15f))
		: GaussianSplatComponent->MoonDirectionalColor;
	MoonDirectionalLightComponent->SetLightColor(MoonColor);
	// P22: separate moon angular diameter and disk luminance, preserving the
	// authored moon Lux and the existing GS shadow / wet-reflection bridge.
	if (bOwnsWorldDayNight)
	{
		const float MoonAngle = FMath::Clamp(GaussianSplatComponent->ManagedMoonDiskAngleDegrees, 0.53f, 3.0f);
		if (!FMath::IsNearlyEqual(MoonDirectionalLightComponent->LightSourceAngle, MoonAngle, 0.005f))
		{
			MoonDirectionalLightComponent->SetLightSourceAngle(MoonAngle);
		}
	}
	const float MoonDiskGain = FMath::Clamp(GaussianSplatComponent->ManagedMoonDiskEmissionGain, 0.05f, 2.0f);
	MoonDirectionalLightComponent->SetAtmosphereSunDiskColorScale(ScaleSkyColor(MoonColor, MoonDiskGain));
	MoonDirectionalLightComponent->SetAtmosphereSunLight(true);
	MoonDirectionalLightComponent->SetAtmosphereSunLightIndex(1);
	if (!bP23CachedMoonLightShaftBloom)
	{
		bP23OriginalMoonLightShaftBloom = MoonDirectionalLightComponent->bEnableLightShaftBloom != 0;
		bP23CachedMoonLightShaftBloom = true;
	}
	if (bOwnsWorldDayNight)
	{
		const bool bWantMoonShaftBloom = !GaussianSplatComponent->bP23DisableLightShaftBloom && bP23OriginalMoonLightShaftBloom;
		if ((MoonDirectionalLightComponent->bEnableLightShaftBloom != 0) != bWantMoonShaftBloom)
		{
			MoonDirectionalLightComponent->SetEnableLightShaftBloom(bWantMoonShaftBloom);
		}
	}

	// Keep the rendered UE Sun/Moon behavior unchanged, but give the 3DGS lighting bridge
	// one continuous celestial state across the whole authored day/night envelope.
	// Previously GS switched to the moon on the exact horizon frame while the moon lux was
	// still near zero, creating a direct-light trough and an apparent disappearing shadow.
	const float ContinuousCelestialBlend = FMath::Clamp(NightBlend, 0.0f, 1.0f);
	const FLinearColor RuntimeSunColor = AuthoredSun->GetLightColor();
	GaussianSplatComponent->RuntimeContinuousCelestialColor = FMath::Lerp(
		RuntimeSunColor, MoonColor, ContinuousCelestialBlend);
	GaussianSplatComponent->RuntimeContinuousCelestialColor.A = 1.0f;
	GaussianSplatComponent->RuntimeContinuousCelestialIntensity = FMath::Lerp(
		FMath::Max(AuthoredSun->Intensity, 0.0f) *
			((GaussianSplatComponent->bP24UnifiedAtmosphereLighting || GaussianSplatComponent->bP23AngleDrivenSunLux) ? 1.0f :
			(bUseUnifiedTOD ? FMath::Clamp(UnifiedTODState.DirectEnergy, 0.0f, 1.0f) : SunEnergyPhase)),
		FMath::Max(GaussianSplatComponent->MoonDirectionalIntensity, 0.0f),
		ContinuousCelestialBlend);
	GaussianSplatComponent->bRuntimeContinuousCelestialStateValid = true;

	// Fade moon energy in more gently than the sky phase. This prevents the low-lux
	// moon from re-brightening Stable direct light immediately after sunset.
	const float MoonLightBlend = NightBlend * NightBlend;
	const float MoonElevationBlend = SmoothCelestialBlend(0.0f, FMath::Max(-FullNight, 0.02f), -SunHeight);
	// P24: a moon is a separate source of atmospheric radiance after sunset.
	// Do not reuse the sun's near-zero lux or the P23 high fill when P24 is enabled.
	const bool bP24SkyLighting = GaussianSplatComponent->bP24UnifiedAtmosphereLighting;
	const float MoonReferenceLux = bP24SkyLighting
		? FMath::Max(0.1f, GaussianSplatComponent->P24MoonSkyLux)
		: FMath::Max(FMath::Max(0.0f, GaussianSplatComponent->MoonDirectionalIntensity),
			FMath::Clamp(GaussianSplatComponent->P23MoonSkyFillLux, 0.0f, 40.0f));
	const float RuntimeMoonIntensity = bOwnsWorldDayNight
		? MoonReferenceLux * MoonLightBlend * MoonElevationBlend
		: 0.0f;
	MoonDirectionalLightComponent->SetIntensity(RuntimeMoonIntensity);
	// P24 keeps the GS ambient/direct light bridge on the same actually-rendered
	// moon lux, rather than its older static 0.5-lux placeholder.
	if (bOwnsWorldDayNight && bP24SkyLighting)
	{
		GaussianSplatComponent->RuntimeContinuousCelestialIntensity = FMath::Lerp(
			FMath::Max(AuthoredSun->Intensity, 0.0f), RuntimeMoonIntensity, ContinuousCelestialBlend);
	}
	MoonDirectionalLightComponent->SetVisibility(bOwnsWorldDayNight, true);

	// v31: feed the existing Day/Night moon into the dedicated Wet pass. The Wet shader
	// uses this only as a camera-independent moonlight/glint cue; it never traces SceneColor
	// or uses the view vector to decide where the wet effect exists.
	if (bOwnsWorldDayNight)
	{
		const FVector3f MoonDirectionForRender(MoonSurfaceToLight);
		const FVector3f MoonColorForRender(MoonColor.R, MoonColor.G, MoonColor.B);
		const float MoonWetStrength = bMoonAboveHorizon
			? FMath::Clamp(MoonLightBlend * MoonElevationBlend, 0.0f, 1.0f)
			: 0.0f;
		const float MoonAngularDiameterDegrees = FMath::Clamp(
			MoonDirectionalLightComponent->LightSourceAngle, 0.10f, 8.0f);
		ENQUEUE_RENDER_COMMAND(UpdateGaussianMoonWetCue)(
			[MoonDirectionForRender, MoonColorForRender, MoonWetStrength, MoonAngularDiameterDegrees](FRHICommandListImmediate&)
			{
				FGaussianSplatRenderer::SetDirectMoonReflectionState(
					MoonDirectionForRender, MoonColorForRender, MoonWetStrength,
					MoonAngularDiameterDegrees, MoonWetStrength > 0.0001f);
			});
	}

	// v2797: do NOT flip ForwardShadingPriority at the horizon. Even though Sun/Moon
	// directions meet there, their lux values do not, so the discrete priority swap could
	// create the exact one-frame brightness jump seen in v2796. Keep the authored Sun as
	// the single forward/fog authority (avoids the UE multiple-directional warning) while
	// GS Stable/LightDepth shadow angle continues to switch continuously to the mirrored
	// Moon direction below the horizon.
	if (bOwnsWorldDayNight)
	{
		AuthoredSun->SetForwardShadingPriority(1);
		MoonDirectionalLightComponent->SetForwardShadingPriority(0);
	}
	else
	{
		MoonDirectionalLightComponent->SetForwardShadingPriority(0);
	}

	// P21: activate a physically rendered sky ONLY if the world does not have
	// another visible SkyAtmosphere. Do not stack two atmospheres or clouds.
	bool bExternalSkyExists = false;
	bool bExternalCloudExists = false;
	for (TObjectIterator<USkyAtmosphereComponent> It; It; ++It)
	{
		USkyAtmosphereComponent* Candidate = *It;
		if (Candidate && Candidate != GaussianAutoSkyAtmosphere && Candidate->GetWorld() == World && Candidate->IsRegistered() && Candidate->IsVisible())
		{
			bExternalSkyExists = true;
			break;
		}
	}
	for (TObjectIterator<UVolumetricCloudComponent> It; It; ++It)
	{
		UVolumetricCloudComponent* Candidate = *It;
		if (Candidate && Candidate != GaussianAutoVolumetricCloud && Candidate->GetWorld() == World && Candidate->IsRegistered() && Candidate->IsVisible())
		{
			bExternalCloudExists = true;
			break;
		}
	}
	const bool bEnableFallbackSky = bOwnsWorldDayNight && GaussianSplatComponent->bAutoCompleteSkyAtmosphere && !bExternalSkyExists;
	if (GaussianAutoSkyAtmosphere)
	{
		if (GaussianAutoSkyAtmosphere->IsVisible() != bEnableFallbackSky)
		{
			GaussianAutoSkyAtmosphere->SetVisibility(bEnableFallbackSky);
		}
	}
	const bool bEnableFallbackCloud = bOwnsWorldDayNight && GaussianSplatComponent->bAutoEnableVolumetricClouds && !bExternalCloudExists;
	if (GaussianAutoVolumetricCloud)
	{
		if (bEnableFallbackCloud && !GaussianAutoVolumetricCloud->GetMaterial() && !bP21TriedCloudMaterial)
		{
			bP21TriedCloudMaterial = true;
			// Stock engine cloud material: do not ship large engine-owned textures.
			static const TCHAR* EngineCloudPath = TEXT("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst.m_SimpleVolumetricCloud_Inst");
			if (UMaterialInterface* CloudMat = LoadObject<UMaterialInterface>(nullptr, EngineCloudPath))
			{
				GaussianAutoVolumetricCloud->SetMaterial(CloudMat);
			}
		}
		const float BaseAltitudeKm = FMath::Clamp(GaussianSplatComponent->ManagedCloudBaseAltitudeKm, 0.8f, 6.0f);
		const float TargetCloudAltitudeKm = (GaussianSplatComponent->WeatherMode == ELCCCinematicGSWeatherMode::Rain)
			? FMath::Min(BaseAltitudeKm, 1.3f) : BaseAltitudeKm;
		if (!FMath::IsNearlyEqual(GaussianAutoVolumetricCloud->LayerBottomAltitude, TargetCloudAltitudeKm, 0.01f))
		{
			GaussianAutoVolumetricCloud->SetLayerBottomAltitude(TargetCloudAltitudeKm);
		}
		const bool bCloudReady = bEnableFallbackCloud && GaussianAutoVolumetricCloud->GetMaterial() != nullptr;
		if (GaussianAutoVolumetricCloud->IsVisible() != bCloudReady)
		{
			GaussianAutoVolumetricCloud->SetVisibility(bCloudReady);
		}
	}
	GaussianSplatComponent->RuntimeManagedSkyStatus = FString::Printf(
		TEXT("대기:%s / 구름:%s%s"),
		bExternalSkyExists ? TEXT("기존 씬") : (bEnableFallbackSky ? TEXT("자동 생성") : TEXT("꺼짐")),
		bExternalCloudExists ? TEXT("기존 씬") : ((GaussianAutoVolumetricCloud && GaussianAutoVolumetricCloud->IsVisible()) ? TEXT("자동 생성") : TEXT("꺼짐")),
		(bEnableFallbackCloud && GaussianAutoVolumetricCloud && !GaussianAutoVolumetricCloud->GetMaterial()) ? TEXT(" [구름 소재 없음]") : TEXT(""));
	// Find the world's SkyAtmosphere. Its rendered luminance is driven by the exact
	// same sun-altitude phase used above, while its native scattering still owns hue:
	// blue daylight -> warm sunset -> dark night. This never touches originalLinear.
	USkyAtmosphereComponent* SkyAtmosphere = CachedDayNightSkyAtmosphere.Get();
	if (!SkyAtmosphere || SkyAtmosphere->GetWorld() != World || !SkyAtmosphere->IsRegistered())
	{
		SkyAtmosphere = nullptr;
		for (TObjectIterator<USkyAtmosphereComponent> It; It; ++It)
		{
			USkyAtmosphereComponent* Candidate = *It;
			if (Candidate && Candidate->GetWorld() == World && Candidate->IsRegistered() && Candidate->IsVisible())
			{
				SkyAtmosphere = Candidate;
				break;
			}
		}
		CachedDayNightSkyAtmosphere = SkyAtmosphere;
	}

	if (SkyAtmosphere && (!bCachedDayNightBaseSkyFactors || CachedDayNightBaseSkyAtmosphere.Get() != SkyAtmosphere))
	{
		CachedDayNightBaseSkyAtmosphere = SkyAtmosphere;
		DayNightBaseSkyLuminanceFactor = SkyAtmosphere->SkyLuminanceFactor;
		DayNightBaseSkyAerialLuminanceFactor = SkyAtmosphere->SkyAndAerialPerspectiveLuminanceFactor;
		bCachedDayNightBaseSkyFactors = true;
	}

	// Fog weather owns its own atmospheric scattering; P24 must not override it.
	const bool bWeatherFogOwnsSkyForDayNight =
		GaussianSplatComponent->WeatherMode == ELCCCinematicGSWeatherMode::Fog &&
		GaussianSplatComponent->FogWeatherStrength > 0.001f;

	// P24 keeps an authored scattering baseline; old P22 only tinted a
	// post-scattering multiplier, which could not bring back dusk or midnight hue.
	if (bP24SavedSkyScattering &&
		(!SkyAtmosphere || P24SkyScatteringOwner.Get() != SkyAtmosphere ||
		 !GaussianSplatComponent->bP24UnifiedAtmosphereLighting || !bOwnsWorldDayNight ||
		 bWeatherFogOwnsSkyForDayNight))
	{
		if (USkyAtmosphereComponent* OldSky = P24SkyScatteringOwner.Get())
		{
			OldSky->SetRayleighScattering(P24OriginalRayleigh);
			OldSky->SetMieScattering(P24OriginalMie);
		}
		bP24SavedSkyScattering = false;
		P24SkyScatteringOwner.Reset();
	}
	if (bOwnsWorldDayNight && SkyAtmosphere &&
		 GaussianSplatComponent->bP24UnifiedAtmosphereLighting && !bWeatherFogOwnsSkyForDayNight && !bP24SavedSkyScattering)
	{
		P24SkyScatteringOwner = SkyAtmosphere;
		P24OriginalRayleigh = SkyAtmosphere->RayleighScattering;
		P24OriginalMie = SkyAtmosphere->MieScattering;
		bP24SavedSkyScattering = true;
	}

	if (bOwnsWorldDayNight && SkyAtmosphere &&
		GaussianSplatComponent->bDriveSkyAtmosphereLuminanceFromSun &&
		!bWeatherFogOwnsSkyForDayNight)
	{
		// v2797: do not darken the sky with a sub-linear power at the first hint of
		// sunset. Delay deep-night attenuation and spread it across the full twilight
		// envelope so the sky has time to pass through golden hour / blue hour.
		const float SkyNightPhaseRaw = 1.0f - SmoothCelestialBlend(FullNight, -0.08f, SunHeight);
		const float SkyNightPhase = FMath::Pow(FMath::Clamp(SkyNightPhaseRaw, 0.0f, 1.0f), 1.08f);
		// Real twilight should remain visible even when the captured 3DGS carrier
		// uses much stronger night attenuation. Avoid a solid black atmosphere.
		const float NightSkyScale = FMath::Max(
			bUseUnifiedTOD
				? FMath::Clamp(GaussianSplatComponent->NightSkyAtmosphereLuminanceScale, 0.0f, 1.0f)
				: FMath::Clamp(GaussianSplatComponent->NightSkyAtmosphereLuminanceScale *
					GaussianSplatComponent->RuntimeNightEnvironmentEnergyScale, 0.0f, 1.0f),
			FMath::Clamp(GaussianSplatComponent->ManagedNightSkyMinimum, 0.05f, 0.95f));
		const float UnifiedSkyResponse = FMath::Pow(
			FMath::Clamp(GaussianSplatComponent->RuntimeUnifiedTODSkyEnergy, 0.0f, 1.0f), 0.65f);
		const float BaseSkyLuminanceScale = bUseUnifiedTOD
			? FMath::Lerp(NightSkyScale, 1.0f, UnifiedSkyResponse)
			: FMath::Lerp(1.0f, NightSkyScale, SkyNightPhase);
		// P22: do not double-dim an already low-lux nighttime atmosphere.
		// Boost is evaluated from sun elevation, not camera distance/exposure.
		const float RealNightWeight = SmootherCelestialBlend(0.12f, 0.96f, SkyNightPhase);
		const float NightLuminanceBoost = FMath::Lerp(1.0f,
			FMath::Clamp(GaussianSplatComponent->ManagedNightSkyLuminanceBoost, 1.0f, 16.0f), RealNightWeight);
		// This multiplier requires moon/solar radiance; it cannot invent light from
		// black radiance. P23 moon fill provides the source at deep night.
		const float SkyLuminanceScale = GaussianSplatComponent->bP24UnifiedAtmosphereLighting
			? FMath::Lerp(1.0f,
				FMath::Clamp(GaussianSplatComponent->P24NightSkyBrightness, 0.5f, 6.0f), RealNightWeight)
			: FMath::Max(0.75f, BaseSkyLuminanceScale * NightLuminanceBoost);
		const float AerialNightScale = FMath::Max(NightSkyScale, 0.12f);
		const float AerialLuminanceScale = bUseUnifiedTOD
			? FMath::Lerp(AerialNightScale, 1.0f, FMath::Pow(FMath::Clamp(UnifiedSkyResponse, 0.0f, 1.0f), 0.85f))
			: FMath::Lerp(1.0f, AerialNightScale, SkyNightPhase);

		const FLinearColor BaseSky = bCachedDayNightBaseSkyFactors ? DayNightBaseSkyLuminanceFactor : FLinearColor::White;
		const FLinearColor BaseAerial = bCachedDayNightBaseSkyFactors ? DayNightBaseSkyAerialLuminanceFactor : FLinearColor::White;

		// Warm the complete atmospheric response around the horizon, but only by a
		// restrained multiplier so UE's physical Rayleigh/Mie sunset still owns the
		// detailed gradient. Deep-night tint is delayed until the warm band has faded.
		// P33: independent, continuous dawn and dusk windows. A single solar
		// elevation is ambiguous; P32's rising/falling selector resolves that.
		// Sunset and dawn effects fade at deep night (-10 deg) and broad daylight
		// (+12 deg), so 3DGS grading and day/night exposure remain unchanged.
		const float P33TwilightWindow = GaussianSplatComponent->bP24UnifiedAtmosphereLighting
			? SmootherCelestialBlend(-10.0f, -4.0f, SolarElevationDegrees) *
				(1.0f - SmootherCelestialBlend(6.0f, 12.0f, SolarElevationDegrees))
			: 0.0f;
		const float P33WarmWindow = FMath::Clamp(FMath::Max(GoldenHourBlend * 0.65f, SunsetBlend), 0.0f, 1.0f)
			* P33TwilightWindow;
		const float P33DawnWeight = bDayNightSunRising ? P33WarmWindow : 0.0f;
		const float P33DuskWeight = bDayNightSunRising ? 0.0f : P33WarmWindow;
		const float P33SunriseLift = FMath::Lerp(1.0f,
			FMath::Clamp(GaussianSplatComponent->P33DawnSkyBrightness, 1.0f, 2.0f), P33DawnWeight);
		const float P33DuskWarmth = FMath::Clamp(GaussianSplatComponent->P33DuskWarmth, 1.0f, 2.5f);
		const float P33DuskSaturation = FMath::Clamp(GaussianSplatComponent->P33DuskSaturation, 1.0f, 2.0f);
		const float AtmosphereTintStrength = FMath::Clamp(GaussianSplatComponent->TwilightSkyAtmosphereTintStrength, 0.0f, 1.0f);
		// P22: unify the same *authored* dawn/golden/sunset/blue/night controls
		// already used to grade the source 3DGS, including when Unified TOD is ON.
		// Previously Unified TOD disabled the authored nighttime hue entirely.
		const float SkyTimeSync = FMath::Clamp(GaussianSplatComponent->ManagedSkyTimeColorSyncStrength, 0.0f, 1.0f);
		const float NightTintPhase = SmoothCelestialBlend(0.24f, 0.98f, NightBlend) *
			FMath::Clamp(GaussianSplatComponent->NightSkyAtmosphereTintStrength, 0.0f, 1.0f) * SkyTimeSync;
		const FLinearColor NightTint = NormalizeCapturedSkyTint(
			FMath::Lerp(GaussianSplatComponent->RuntimeNightEnvironmentColor,
				GaussianSplatComponent->NightAmbientColor, 0.75f));

		// TimeOfDayGrade is computed earlier using the editor's own phase colours
		// in legacy mode. In Unified TOD explicitly reintroduce those editor
		// colours using the *same* deterministic solar-elevation blend weights.
		FLinearColor AuthoredPhaseSkyTint = TimeOfDayGrade;
		if (bUseUnifiedTOD)
		{
			const float Weights = GoldenHourBlend + SunsetBlend + BlueHourBlend;
			if (Weights > 0.0001f)
			{
				AuthoredPhaseSkyTint =
					ActiveGoldenColor * (GoldenHourBlend / Weights) +
					ActiveHorizonColor * (SunsetBlend / Weights) +
					ActiveBlueColor * (BlueHourBlend / Weights);
				AuthoredPhaseSkyTint.A = 1.0f;
			}
			else
			{
				AuthoredPhaseSkyTint = UnifiedTODState.AmbientColor;
			}
		}
		const float AuthoredSkyPhaseWeight = FMath::Clamp(
			FMath::Max3(GoldenHourBlend, SunsetBlend, BlueHourBlend) * SkyTimeSync, 0.0f, 1.0f);
		const FLinearColor PhaseAtmosphereTint = NormalizeCapturedSkyTint(FMath::Lerp(
			bUseUnifiedTOD ? UnifiedTODState.AmbientColor : TimeOfDayGrade,
			AuthoredPhaseSkyTint, AuthoredSkyPhaseWeight));
		const float AtmospherePhaseStrength = bUseUnifiedTOD
			? FMath::Clamp(FMath::Max(UnifiedTODState.GradeStrength, AuthoredSkyPhaseWeight) * AtmosphereTintStrength * SkyTimeSync, 0.0f, 1.0f)
			: FMath::Clamp(
				PhasePresence * AtmosphereTintStrength * SkyTimeSync * (bDayNightSunRising ? 0.72f : 0.88f), 0.0f, 1.0f);
		// P24: sky and cloud lighting derive from the SAME existing artist-authored
		// phase weights and colors as the 3DGS. Use the native atmospheric scattering
		// solution for a physical zenith-to-horizon gradient, not a screen/camera quad.
		// Moon sky radiance remains independent of a zero solar lux at night.
		// P28: Use distinct dawn and sunset palettes on the actual atmosphere.
		// The Sun elevation controls *when* a phase occurs; the rising/falling
		// flag determines *which palette* it uses. Native sky scattering remains
		// responsible for direction-dependent zenith/horizon gradients.
		// Avoid driving the whole sky toward a single uniform white/pink colour:
		// forward-scattering Mie carries the warm horizon, Rayleigh carries the
		// cooler upper sky. This operates on ORIGINAL authored coefficients only.
		if (GaussianSplatComponent->bP24UnifiedAtmosphereLighting && !bWeatherFogOwnsSkyForDayNight &&
			bP24SavedSkyScattering && P24SkyScatteringOwner.Get() == SkyAtmosphere)
		{
			const float Sync = FMath::Clamp(GaussianSplatComponent->P24ScatteringColorSync, 0.0f, 1.0f);
			const float HorizonArtStrength = FMath::Clamp(GaussianSplatComponent->P32HorizonColorStrength, 0.0f, 1.0f);
			const float GoldenBand = FMath::Clamp(GoldenHourBlend, 0.0f, 1.0f);
			const float HorizonBand = FMath::Clamp(SunsetBlend, 0.0f, 1.0f);
			const float BlueBand = FMath::Clamp(BlueHourBlend, 0.0f, 1.0f);
			const float DarkBand = 1.0f - SmootherCelestialBlend(-18.0f, -6.0f, SolarElevationDegrees);
			const float PhaseBand = FMath::Clamp(FMath::Max3(GoldenBand, HorizonBand, BlueBand), 0.0f, 1.0f);

			const FLinearColor DawnWarm = FMath::Lerp(
				GaussianSplatComponent->DawnGoldenHourAmbientColor,
				GaussianSplatComponent->DawnHorizonAmbientColor,
				HorizonBand / FMath::Max(GoldenBand + HorizonBand, 0.001f));
			const FLinearColor SunsetWarm = FMath::Lerp(
				GaussianSplatComponent->GoldenHourAmbientColor,
				GaussianSplatComponent->TwilightAmbientColor,
				HorizonBand / FMath::Max(GoldenBand + HorizonBand, 0.001f));
			const FLinearColor WarmHue = NormalizeCapturedSkyTint(
				bDayNightSunRising ? DawnWarm : SunsetWarm);
			const FLinearColor CoolHue = NormalizeCapturedSkyTint(
				bDayNightSunRising ? GaussianSplatComponent->DawnBlueHourAmbientColor
				                    : GaussianSplatComponent->BlueHourAmbientColor);
			const FLinearColor NightHue = NormalizeCapturedSkyTint(GaussianSplatComponent->NightAmbientColor);
			// Preserve the baseline Rayleigh coefficient ratios for daylight. Twilight
			// adds a modest cool upper sky, with less saturation in dawn than dusk.
			const float CoolStrength = Sync * FMath::Clamp(BlueBand * 0.32f +
				DarkBand * 0.22f, 0.0f, 0.38f);
			FLinearColor UpperSkyColor = FMath::Lerp(CoolHue, NightHue, DarkBand);
			UpperSkyColor = NormalizeCapturedSkyTint(UpperSkyColor);
			const float BaseRayleighMean = FMath::Max(
				(P24OriginalRayleigh.R + P24OriginalRayleigh.G + P24OriginalRayleigh.B) / 3.0f,
				0.00001f);
			const float UpperSum = FMath::Max(
				UpperSkyColor.R + UpperSkyColor.G + UpperSkyColor.B, 0.02f);
			const FLinearColor TargetRayleigh(
				3.0f * BaseRayleighMean * UpperSkyColor.R / UpperSum,
				3.0f * BaseRayleighMean * UpperSkyColor.G / UpperSum,
				3.0f * BaseRayleighMean * UpperSkyColor.B / UpperSum, 1.0f);
			const FLinearColor NewRayleigh = FMath::Lerp(P24OriginalRayleigh, TargetRayleigh, CoolStrength);

			// Sun-aligned forward scattering naturally gives a colored horizon,
			// rather than multiplying the entire rendered sky by a sunset swatch.
			// P32: the rising palette stays light peach, the setting palette has a
			// stronger reddish warm horizon. We only modulate the native Mie scattering
			// coefficients, not a camera-aligned screen-space colour overlay.
			// P33: use the existing artist-authored sunset hue, not the softer dawn
			// peach. Saturation is around the average scattering energy, avoiding
			// an unbounded increase in the Rayleigh/Mie particle density.
			const float WarmStrength = FMath::Clamp(
				Sync * HorizonArtStrength * (
					GoldenBand * (bDayNightSunRising ? 0.24f : 0.42f) +
					HorizonBand * (bDayNightSunRising ? 0.58f : 0.82f)) *
				FMath::Lerp(1.0f, P33DuskWarmth, P33DuskWeight), 0.0f, 0.98f);
			const float WarmMean = (WarmHue.R + WarmHue.G + WarmHue.B) / 3.0f;
			const float MieSaturation = FMath::Lerp(1.0f, P33DuskSaturation, P33DuskWeight);
			const FLinearColor WarmSaturated(
				FMath::Max(0.015f, WarmMean + (WarmHue.R - WarmMean) * MieSaturation),
				FMath::Max(0.015f, WarmMean + (WarmHue.G - WarmMean) * MieSaturation),
				FMath::Max(0.015f, WarmMean + (WarmHue.B - WarmMean) * MieSaturation), 1.0f);
			const float WarmSum = FMath::Max(WarmSaturated.R + WarmSaturated.G + WarmSaturated.B, 0.02f);
			const float MieMean = FMath::Max(
				(P24OriginalMie.R + P24OriginalMie.G + P24OriginalMie.B) / 3.0f, 0.00001f);
			const FLinearColor WarmMie(
				3.0f * MieMean * WarmSaturated.R / WarmSum,
				3.0f * MieMean * WarmSaturated.G / WarmSum,
				3.0f * MieMean * WarmSaturated.B / WarmSum, 1.0f);
			const FLinearColor NewMie = FMath::Lerp(P24OriginalMie, WarmMie, WarmStrength);
			if (!SkyAtmosphere->RayleighScattering.Equals(NewRayleigh, 0.00001f))
				SkyAtmosphere->SetRayleighScattering(NewRayleigh);
			if (!SkyAtmosphere->MieScattering.Equals(NewMie, 0.00001f))
				SkyAtmosphere->SetMieScattering(NewMie);
		}
		// Preserve luminance when tinting: the authored hue should not make sunset
		// artificially darker (the Lux curve controls direct energy separately).
		const float PhaseLuminance = FMath::Max(0.12f, PhaseAtmosphereTint.GetLuminance());
		const FLinearColor LuminancePreservingPhaseTint(
			FMath::Clamp(PhaseAtmosphereTint.R / PhaseLuminance, 0.10f, 3.0f),
			FMath::Clamp(PhaseAtmosphereTint.G / PhaseLuminance, 0.10f, 3.0f),
			FMath::Clamp(PhaseAtmosphereTint.B / PhaseLuminance, 0.10f, 3.0f), 1.0f);
		// P32: let the existing authored sunrise/sunset colours be visible without
		// flattening the zenith/horizon gradient already produced by SkyAtmosphere.
		const float SkyColorMultiplierStrength = GaussianSplatComponent->bP24UnifiedAtmosphereLighting
			? AtmospherePhaseStrength * FMath::Lerp(0.12f, 0.27f,
				FMath::Clamp(GaussianSplatComponent->P32HorizonColorStrength, 0.0f, 1.0f))
			: AtmospherePhaseStrength;
		FLinearColor SkyTintMultiplier = FMath::Lerp(FLinearColor::White, LuminancePreservingPhaseTint, SkyColorMultiplierStrength);
		const float NightMultiplierStrength = GaussianSplatComponent->bP24UnifiedAtmosphereLighting
			? NightTintPhase * 0.23f : NightTintPhase;
		SkyTintMultiplier.R *= FMath::Lerp(1.0f, NightTint.R, NightMultiplierStrength);
		SkyTintMultiplier.G *= FMath::Lerp(1.0f, NightTint.G, NightMultiplierStrength);
		SkyTintMultiplier.B *= FMath::Lerp(1.0f, NightTint.B, NightMultiplierStrength);
		// P33: saturate the dusk *sky radiance* only, separate from cloud and
		// 3DGS colour grading. Rayleigh still carries a cool upper sky, while
		// the forward Mie response makes the sunset horizon reddish-orange.
		// The sunset palette comes from the existing "일몰 지평선 색상" control.
		const FLinearColor AuthoredDusk = NormalizeCapturedSkyTint(
			GaussianSplatComponent->TwilightAmbientColor);
		const float DuskVividMix = FMath::Clamp(P33DuskWeight * (P33DuskWarmth - 1.0f) * 0.85f, 0.0f, 0.90f);
		const float DuskColorMix = DuskVividMix * P33DuskSaturation;
		const float DuskOrangeRatio = FMath::Clamp(AuthoredDusk.G, 0.18f, 0.75f);
		const float DuskBlueRatio = FMath::Clamp(AuthoredDusk.B, 0.05f, 0.42f);
		SkyTintMultiplier.R *= 1.0f + DuskColorMix * 0.65f;
		SkyTintMultiplier.G *= 1.0f + DuskColorMix * (0.18f + DuskOrangeRatio * 0.24f);
		SkyTintMultiplier.B *= 1.0f - FMath::Clamp(DuskColorMix * (0.37f + 0.12f * (1.0f - DuskBlueRatio)), 0.0f, 0.70f);

		FLinearColor AerialTintMultiplier = FMath::Lerp(
			FLinearColor::White, PhaseAtmosphereTint, AtmospherePhaseStrength * 0.24f);
		const float AerialNightPhase = NightTintPhase * 0.50f;
		AerialTintMultiplier.R *= FMath::Lerp(1.0f, NightTint.R, AerialNightPhase);
		AerialTintMultiplier.G *= FMath::Lerp(1.0f, NightTint.G, AerialNightPhase);
		AerialTintMultiplier.B *= FMath::Lerp(1.0f, NightTint.B, AerialNightPhase);
        // P27: daylight-only sky-luminance guard. Blended out towards night,
        // so midnight moon/sky settings and phase-color ratios stay independent.
        // P28: The SkyAtmosphere sky-view LUT is HDR; in the user's fixed-
        // exposure panorama workflow the old linear 0.12 daylight factor could
        // still clip to pure white. A perceptual shoulder prevents sky-only
        // clipping, leaving the separately-rendered source GS untouched.
        const float P27DayFactor = GaussianSplatComponent->bP24UnifiedAtmosphereLighting
            ? FMath::Lerp(FMath::Pow(FMath::Clamp(
                    GaussianSplatComponent->P27DaySkyLuminance, 0.01f, 1.0f), 1.28f),
                1.0f, RealNightWeight)
            : 1.0f;
		// Twilight has less incident solar radiance than noon, so the hard
		// daylight-only anti-clipping guard must release as sunset/dawn begins.
		// Otherwise it suppresses the colour bands exactly when needed.
		const float P33TwilightSkyFactor = FMath::Lerp(P27DayFactor,
			FMath::Max(P27DayFactor, 0.40f), P33WarmWindow);
		const float P33DuskLightLift = 1.0f + P33DuskWeight * 0.18f * (P33DuskWarmth - 1.0f);
		const float P33SkyBrightness = P33TwilightSkyFactor * P33SunriseLift * P33DuskLightLift;
		const FLinearColor SkyResult(
			BaseSky.R * SkyTintMultiplier.R * SkyLuminanceScale * P33SkyBrightness,
			BaseSky.G * SkyTintMultiplier.G * SkyLuminanceScale * P33SkyBrightness,
			BaseSky.B * SkyTintMultiplier.B * SkyLuminanceScale * P33SkyBrightness, BaseSky.A);
		const FLinearColor AerialResult(
			BaseAerial.R * AerialTintMultiplier.R * AerialLuminanceScale,
			BaseAerial.G * AerialTintMultiplier.G * AerialLuminanceScale,
			BaseAerial.B * AerialTintMultiplier.B * AerialLuminanceScale, BaseAerial.A);
		SkyAtmosphere->SetSkyLuminanceFactor(SkyResult);
		SkyAtmosphere->SetSkyAndAerialPerspectiveLuminanceFactor(AerialResult);
	}

	if (bOwnsWorldDayNight)
	{
		GaussianSplatComponent->RuntimeP24SkyLightingStatus = FString::Printf(
			TEXT("%s / %s / Sun %.1f° %.2f Lux / Moon %.2f Lux / Sky:%s / Color sync %.2f / Clouds via Sun+Moon"),
			GaussianSplatComponent->bP24UnifiedAtmosphereLighting ? TEXT("P24 ON") : TEXT("P23"),
			bDayNightSunRising ? TEXT("Dawn") : TEXT("Sunset"),
			SolarElevationDegrees, FMath::Max(0.0f, AuthoredSun->Intensity),
			FMath::Max(0.0f, MoonDirectionalLightComponent->Intensity),
			SkyAtmosphere ? TEXT("SkyAtmosphere") : TEXT("NONE"),
			GaussianSplatComponent->P24ScatteringColorSync);
	}

	// Prefer the brightest visible SkyLight in this world. Its irradiance SH is the
	// same low-frequency sky lighting Unreal feeds to meshes, so it is a stable color
	// bridge for 3DGS without any GPU readback.
	USkyLightComponent* SkyLight = CachedDayNightSkyLight.Get();
	if (!SkyLight || SkyLight->GetWorld() != World || !SkyLight->IsRegistered() || !SkyLight->IsVisible())
	{
		SkyLight = nullptr;
		float BestSkyLightIntensity = -1.0f;
		for (TObjectIterator<USkyLightComponent> It; It; ++It)
		{
			USkyLightComponent* Candidate = *It;
			if (!Candidate || Candidate->GetWorld() != World || !Candidate->IsRegistered() || !Candidate->IsVisible())
			{
				continue;
			}
			const float CandidateIntensity = FMath::Max(0.0f, Candidate->Intensity);
			if (!SkyLight || CandidateIntensity > BestSkyLightIntensity)
			{
				SkyLight = Candidate;
				BestSkyLightIntensity = CandidateIntensity;
			}
		}
		CachedDayNightSkyLight = SkyLight;
	}

	if (bOwnsWorldDayNight && SkyLight && GaussianSplatComponent->bEnableDayNightSkyLightRealTimeCapture &&
		SkyLight->GetMobility() != EComponentMobility::Static && !SkyLight->IsRealTimeCaptureEnabled())
	{
		SkyLight->SetRealTimeCapture(true);
	}

	// Fallback follows the same sun height that drives SkyAtmosphere: daylight blue,
	// a warm horizon band, then the authored night tint. SkyAtmosphere luminance-factor
	// art direction is folded in only as chroma; brightness is handled by the phase.
	FLinearColor FallbackSkyTint = FLinearColor(0.54f, 0.70f, 1.00f, 1.0f);
	if (bUseUnifiedTOD)
	{
		FallbackSkyTint = UnifiedTODState.AmbientColor;
	}
	else
	{
		FallbackSkyTint = FMath::Lerp(
			FallbackSkyTint, TimeOfDayGrade,
			FMath::Clamp(PhasePresence * (bDayNightSunRising ? 0.62f : 0.78f), 0.0f, 1.0f));
		const float FallbackNightTone = SmoothCelestialBlend(0.30f, 0.995f, NightBlend);
		FallbackSkyTint = FMath::Lerp(FallbackSkyTint, GaussianSplatComponent->RuntimeNightEnvironmentColor, FallbackNightTone);
	}
	FallbackSkyTint = NormalizeCapturedSkyTint(FallbackSkyTint);
	GaussianSplatComponent->RuntimeSkyAmbientColor = FallbackSkyTint;
	GaussianSplatComponent->bRuntimeSkyAmbientColorValid = true;

	// v2828 flicker fix: Real Time Capture is time-sliced across multiple frames.
	// Reading its SH back into 3DGS every editor tick makes GS ambient chroma jump as
	// partial/stale capture states arrive. During automatic TOD, use the deterministic
	// solar-elevation tint above for GS; the SkyLight itself remains in Real Time Capture
	// for UE meshes/reflections. Preserve captured-SH sync only when TOD automation is off.
	if (!GaussianSplatComponent->bEnableAutomaticDayNightCycle &&
		GaussianSplatComponent->bSync3DGSAmbientFromSkyLight && SkyLight)
	{
		const FSHVectorRGB3 Irradiance = SkyLight->GetIrradianceEnvironmentMap();
		if (Irradiance.AreFloatsValid())
		{
			FLinearColor CapturedSky = Irradiance.CalcIntegral();
			const FLinearColor SkyLightColor = SkyLight->GetLightColor();
			CapturedSky.R = FMath::Max(CapturedSky.R, 0.0f) * FMath::Max(SkyLightColor.R, 0.0f);
			CapturedSky.G = FMath::Max(CapturedSky.G, 0.0f) * FMath::Max(SkyLightColor.G, 0.0f);
			CapturedSky.B = FMath::Max(CapturedSky.B, 0.0f) * FMath::Max(SkyLightColor.B, 0.0f);
			if (CapturedSky.GetLuminance() > 1.0e-5f)
			{
				// Keep Unreal's captured sky hue, then apply only a restrained phase art-direction
				// bias. This prevents Real Time Capture from erasing the golden-hour tone while
				// still letting the actual SkyAtmosphere drive local chroma.
				FLinearColor CapturedTint = NormalizeCapturedSkyTint(CapturedSky);
				CapturedTint = FMath::Lerp(
					CapturedTint, NormalizeCapturedSkyTint(TimeOfDayGrade),
					FMath::Clamp(PhasePresence * (bDayNightSunRising ? 0.34f : 0.48f), 0.0f, 1.0f));
				const float CapturedNightTone = SmoothCelestialBlend(0.32f, 0.995f, NightBlend);
				CapturedTint = FMath::Lerp(CapturedTint, NormalizeCapturedSkyTint(GaussianSplatComponent->RuntimeNightEnvironmentColor), CapturedNightTone * 0.45f);
				GaussianSplatComponent->RuntimeSkyAmbientColor = NormalizeCapturedSkyTint(CapturedTint);
				GaussianSplatComponent->bRuntimeSkyAmbientColorValid = true;
			}
		}
	}
}

void AGaussianSplatActor::SyncCinematicPostProcess()
{
	if (!GaussianSplatComponent || !CinematicPostProcessComponent)
	{
		return;
	}

	const bool bOwnsWorldDayNight = FindPrimaryDayNightOwner(GetWorld(), this) == this;
	const bool bEnableFixedExposure = bOwnsWorldDayNight &&
		GaussianSplatComponent->bEnableAutomaticDayNightCycle &&
		GaussianSplatComponent->bLockSceneExposureForDayNight;
	// P44: the experimental P42 DOF path is retired. Keep the original
	// day/night fixed-exposure post process but never force camera blur.
	CinematicPostProcessComponent->bUnbound = true;
	CinematicPostProcessComponent->Priority = 1000.0f;
	CinematicPostProcessComponent->bEnabled = bEnableFixedExposure;
	CinematicPostProcessComponent->BlendWeight = bEnableFixedExposure ? 1.0f : 0.0f;
	FPostProcessSettings& Settings = CinematicPostProcessComponent->Settings;
	Settings.bOverride_DepthOfFieldFocalDistance = false;
	Settings.bOverride_DepthOfFieldFstop = false;
	Settings.bOverride_DepthOfFieldSensorWidth = false;
	if (!bEnableFixedExposure)
	{
		// Do not retain overrides after disabling the owning day/night actor.
		Settings.bOverride_BloomIntensity = false;
		Settings.bOverride_AutoExposureMethod = false;
		Settings.bOverride_AutoExposureApplyPhysicalCameraExposure = false;
		Settings.bOverride_AutoExposureBias = false;
		return;
	}

	// P23: this is the actor-owned PP only, NOT the project PPVolume. Light shaft
	// bloom is disabled on each celestial light separately by SyncDayNightCycle.
	FPostProcessSettings& P23Settings = CinematicPostProcessComponent->Settings;
	P23Settings.bOverride_BloomIntensity = GaussianSplatComponent->bP23OverridePostProcessBloom;
	if (GaussianSplatComponent->bP23OverridePostProcessBloom)
	{
		P23Settings.BloomIntensity = FMath::Clamp(GaussianSplatComponent->P23PostProcessBloomIntensity, 0.0f, 3.0f);
	}

	// Manual + physical-camera off prevents
	// eye adaptation from turning a dark post-sunset atmosphere back into a blue day.
	// Reuse the Settings reference declared above for the same post-process component.
	Settings.bOverride_AutoExposureMethod = true;
	Settings.AutoExposureMethod = AEM_Manual;
	Settings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	Settings.AutoExposureApplyPhysicalCameraExposure = 0u;
	Settings.bOverride_AutoExposureBias = true;
	float ExposureBias = FMath::Clamp(GaussianSplatComponent->DayNightFixedExposureCompensation, -8.0f, 8.0f);
	// P32: sky and 3DGS are in the same HDR view; a manual EV=0 can clip real
	// volumetric clouds even after lowering SkyAtmosphere luminance. Apply a small,
	// configurable daylight exposure shoulder, with NO night/twilight change.
	// This is a whole-scene correction, so keep its range limited and user-visible.
	if (GaussianSplatComponent->bP24UnifiedAtmosphereLighting)
	{
		const float DayGuard = SmootherCelestialBlend(24.0f, 50.0f,
			GaussianSplatComponent->RuntimeSolarElevationDegrees);
		ExposureBias -= DayGuard * FMath::Clamp(GaussianSplatComponent->P32DayExposureGuardEV, 0.0f, 2.5f);
	}
	if (!GaussianSplatComponent->bUseUnifiedPhysicalTimeOfDay && GaussianSplatComponent->bAutoProtectBrightDayExposure)
	{
		const float StartDeg = FMath::Clamp(GaussianSplatComponent->BrightDayExposureStartDegrees, 0.0f, 89.0f);
		const float FullDeg = FMath::Clamp(GaussianSplatComponent->BrightDayExposureFullDegrees, StartDeg + 0.5f, 90.0f);
		const float BrightDayPhase = SmootherCelestialBlend(StartDeg, FullDeg, GaussianSplatComponent->RuntimeSolarElevationDegrees);
		ExposureBias += FMath::Clamp(GaussianSplatComponent->BrightDayExposureCompensation, -4.0f, 0.0f) * BrightDayPhase;
	}
	Settings.AutoExposureBias = FMath::Clamp(ExposureBias, -8.0f, 8.0f);
}

void AGaussianSplatActor::SyncWeatherEffects()
{
	if (!GaussianSplatComponent || !WeatherNiagaraComponent || !WeatherSkyFogComponent)
	{
		return;
	}
	// Weather rendering is independent from the legacy proxy camera/material
	// capture. Publish the direct Snow and dedicated Wet controls every sync so
	// Clear mode also clears any render-thread state left by a previous preset.
	const bool bOwnsWorldWeather = FindPrimaryWeatherOwner(GetWorld(), this) == this;
	const bool bActualRain = GaussianSplatComponent->WeatherMode == ELCCCinematicGSWeatherMode::Rain;
	// Production rain: retire all earlier wet-mesh/test modes and use the latest
	// floor + source-fitted roof/foliage Gaussian layer only. Old serialized flags
	// are reset to prevent a saved diagnostic scene from hiding the world GS.
	PuddleRoofBlueDebugMode = 0;
	PuddleSourceCoverageDebug = 0;
	bPuddleRoofOpaqueDebugView = false;
	bPuddleDebugHideSource = false;
	bPuddleDebugOpaqueBake = false;
	const bool bPuddleAvailable = BakedPuddleGaussianAsset && BakedPuddleGaussianAsset->IsValid();
	// The dedicated wet renderer is the only Rain surface renderer.
	// P06: visibility only, no per-view placement/depth sampling or old wet-mesh mask.
	if (PuddleGaussianComponent)
	{
		const bool bWantPuddle = bActualRain && bPuddleAvailable;
		if (bWantPuddle && PuddleGaussianComponent->GetSplatAsset() != BakedPuddleGaussianAsset.Get())
		{
			PuddleGaussianComponent->SetSplatAsset(BakedPuddleGaussianAsset.Get());
		}
		if (PuddleGaussianComponent->IsVisible() != bWantPuddle)
		{
			PuddleGaussianComponent->SetVisibility(bWantPuddle, false);
		}
		PuddleGaussianComponent->SetHiddenInGame(!bWantPuddle);
	}
	const bool bRainPrecipitationActive =
		bActualRain && GaussianSplatComponent->bRainPrecipitationEnabled;
    const bool bActualSnow = GaussianSplatComponent->WeatherMode == ELCCCinematicGSWeatherMode::Snow;
    // A serialized snow stage can remain attached after an MRQ world is duplicated
    // even when the preset is Clear. Detach it BEFORE any stage selection / upload.
    // Keep the authored bake assets and source-GS component intact so returning
    // to Snow reinstalls the selected stage through the normal transaction below.
    if (!bActualSnow && SnowGaussianComponent && SnowGaussianComponent->GetSplatAsset())
    {
        UGaussianSplatAsset* const DetachedSnowAsset = SnowGaussianComponent->GetSplatAsset();
        SnowGaussianComponent->SetVisibility(false, false);
        SnowGaussianComponent->SetHiddenInGame(true);
        SnowGaussianComponent->SetSplatAsset(nullptr);
        AppliedSnowHeightStage2878 = -1; // force a real rebind on return to Snow
        SnowCacheTrackedStage2880 = -2; // inactive-weather cache retirement pending
        SnowCacheStableTicks2880 = 0;
        bSnowCacheCleanupQueued2880 = false;
        // Existing cache-eviction API only releases buffers when no scene proxy
        // still holds them; never evict the original GS or authored bake data.
        if (DetachedSnowAsset != GaussianSplatComponent->GetSplatAsset())
        {
            DetachedSnowAsset->QueueReleaseUnusedSnowStageGPUCache();
        }
        UE_LOG(LogTemp, Display,
            TEXT("[GSSnowWeatherGate] Detached inactive snow asset; WeatherMode=%d SnowAsset=%s BakesRetained=1"),
            static_cast<int32>(GaussianSplatComponent->WeatherMode), *GetNameSafe(DetachedSnowAsset));
    }
    // A just-retired scene proxy can still hold its RenderData on the render
    // thread. Re-attempt the existing shared-reference-safe eviction after a
    // quiet interval; never evict authored CPU bake data or the base GS.
    if (!bActualSnow && SnowCacheTrackedStage2880 == -2 &&
        !bSnowCacheCleanupQueued2880 && ++SnowCacheStableTicks2880 >= 90)
    {
        for (const FSnowHeightBakeStage2878& Candidate : BakedSnowHeightStages2878)
        {
            UGaussianSplatAsset* const InactiveAsset = Candidate.SnowAsset.Get();
            if (InactiveAsset && InactiveAsset != GaussianSplatComponent->GetSplatAsset())
            {
                InactiveAsset->QueueReleaseUnusedSnowStageGPUCache();
            }
        }
        bSnowCacheCleanupQueued2880 = true;
    }
    // 2.8.79: the SOURCE 3DGS must stay registered through live snow-height edits.
    // Existing four-stage 2.8.78 saves also work; newly generated bakes may have 4..12 stages.
    const int32 AvailableSnowStages2879 = BakedSnowHeightStages2878.Num();
    const bool bHasMultiHeightSnow2878 = !bBuildingSnowHeightStages2878 &&
        AvailableSnowStages2879 >= 4 && AvailableSnowStages2879 <= 12 &&
        SnowBakedMaximumHeightCm2878 > KINDA_SMALL_NUMBER;
    int32 DesiredSnowStage2878 = -1; // legacy single-height asset, unchanged
    const float RequestedSnowHeight2879 = bHasMultiHeightSnow2878
        ? FMath::Clamp(SnowRuntimeHeightCm2878, 0.0f, SnowBakedMaximumHeightCm2878) : 0.0f;
    if (bActualSnow && bHasMultiHeightSnow2878)
    {
        // Use the REAL authored stage heights (not a hardcoded 25% index formula).
        // Independent, non-corresponding Gaussian assets cannot safely morph positions.
        DesiredSnowStage2878 = 0;
        float BestDistanceCm = RequestedSnowHeight2879;
        for (int32 StageIndex = 0; StageIndex < AvailableSnowStages2879; ++StageIndex)
        {
            const float DistanceCm = FMath::Abs(RequestedSnowHeight2879 - BakedSnowHeightStages2878[StageIndex].HeightCm);
            if (DistanceCm <= BestDistanceCm)
            {
                BestDistanceCm = DistanceCm;
                DesiredSnowStage2878 = StageIndex + 1;
            }
        }
        // v2898: put the chosen-stage PostLoad policy on the asset itself.
        // MRQ duplicates this state, so inactive stage assets skip GPU texture
        // creation without trying to discover their owner from Asset::PostLoad.
        for (int32 StageIndex2898 = 0; StageIndex2898 < AvailableSnowStages2879; ++StageIndex2898)
        {
            if (UGaussianSplatAsset* StageAsset2898 = BakedSnowHeightStages2878[StageIndex2898].SnowAsset.Get())
            {
                StageAsset2898->ConfigureSnowHeightStageTexturePolicy2898(
                    true, DesiredSnowStage2878 == StageIndex2898 + 1);
            }
        }

        if (DesiredSnowStage2878 != AppliedSnowHeightStage2878)
        {
            // If the editor has temporarily unregistered either renderer, DEFER
            // the switch instead of claiming success or overwriting the source GS.
            const bool bCanSwitchNow = GaussianSplatComponent->IsRegistered() &&
                SnowGaussianComponent && SnowGaussianComponent->IsRegistered();
            if (!bCanSwitchNow)
            {
                SnowHeightBakeStatus2878 = TEXT("렌더 컴포넌트 등록 대기: 원본 GS는 유지, 눈 전환은 보류");
            }
            else if (DesiredSnowStage2878 == 0)
            {
                // Detach the snow-only render proxy at zero height.  Keeping
                // its previously selected asset bound (although invisible)
                // pins its GPU buffers and defeats inactive-stage eviction.
                // The original GS component and its asset are NEVER touched.
                if (SnowGaussianComponent->GetSplatAsset())
                {
                    SnowGaussianComponent->SetVisibility(false, false);
                    SnowGaussianComponent->SetHiddenInGame(true);
                    SnowGaussianComponent->SetSplatAsset(nullptr);
                }
                GaussianSplatComponent->SnowGaussianGeneratedCount = 0;
                AppliedSnowHeightStage2878 = 0;
                SnowHeightBakeStatus2878 = TEXT("0cm / 추가 눈만 숨김 / 원본 3DGS 유지");
            }
            else
            {
                const FSnowHeightBakeStage2878& StageData = BakedSnowHeightStages2878[DesiredSnowStage2878 - 1];
                UGaussianSplatAsset* const SelectedSnowAsset2898 = StageData.SnowAsset.Get();
                const bool bSelectedTextureReady2898 = SelectedSnowAsset2898 &&
                    SelectedSnowAsset2898->EnsureSnowHeightStageColorTexture2898();
                const UGaussianSplatAsset* SourceAsset = GaussianSplatComponent->GetSplatAsset();
                const bool bValidStage = bSelectedTextureReady2898 &&
                    StageData.SnowAsset && StageData.SnowAsset->IsValid() &&
                    SourceAsset && StageData.SourceCoverageCount == SourceAsset->GetSplatCount() &&
                    StageData.SourceCoverageMask.Num() == (StageData.SourceCoverageCount + 15) / 16 &&
                    StageData.SnowFieldCount == StageData.SnowAsset->GetSplatCount() &&
                    StageData.SnowFieldPacked.Num() == (StageData.SnowFieldCount + 3) / 4;
                if (!bValidStage)
                {
                    SnowHeightBakeStatus2878 = FString::Printf(
                        TEXT("눈높이 %d 단계 데이터 검증 실패: 기존 표시 유지 / 재베이크 필요"),
                        DesiredSnowStage2878);
                    // Avoid hammering SetSplatAsset or dropping the source GS on invalid input.
                    UE_LOG(LogTemp, Warning, TEXT("[Snow2879Switch] %s"), *SnowHeightBakeStatus2878);
                }
                else
                {
                    // One game-thread transaction: asset, source correspondence and
                    // Snow pattern cache are committed together at a single stage edge.
                    UGaussianSplatAsset* const PreviousSnowAsset = SnowGaussianComponent->GetSplatAsset();
                    const TObjectPtr<UGaussianSplatAsset> PreviousBakedAsset = BakedSnowGaussianAsset;
                    const int32 PreviousSnowCount = GaussianSplatComponent->SnowGaussianGeneratedCount;
                    // Stage data must be installed BEFORE SetSplatAsset:
                    // that call creates the new Snow scene proxy already.
                    // Previously the field was assigned afterwards and the
                    // same multi-million-splat proxy was rebuilt a second time.
                    SnowGaussianComponent->SnowBakedFieldPacked2873 = StageData.SnowFieldPacked;
                    SnowGaussianComponent->SnowBakedFieldSplatCount2873 = StageData.SnowFieldCount;
                    SnowGaussianComponent->SnowBakedFieldPatternCm2873 = StageData.SnowFieldPatternCm;
                    SnowGaussianComponent->SnowBakedFieldLocalToWorld2873 = StageData.SnowFieldLocalToWorld;
                    SnowGaussianComponent->SnowBakedFieldSourceAsset2873 = StageData.SnowAsset.Get();
                    BakedSnowGaussianAsset = StageData.SnowAsset;
                    SnowGaussianComponent->SetSplatAsset(StageData.SnowAsset.Get());
                    if (SnowGaussianComponent->GetSplatAsset() != StageData.SnowAsset.Get() ||
                        !SnowGaussianComponent->IsRegistered())
                    {
                        SnowGaussianComponent->SetSplatAsset(PreviousSnowAsset);
                        BakedSnowGaussianAsset = PreviousBakedAsset;
                        GaussianSplatComponent->SnowGaussianGeneratedCount = PreviousSnowCount;
                        // The source mask was never touched before success;
                        // no multi-megabyte backup copy or source proxy reset.
                        // Restore the previous Snow field on the exceptional
                        // rollback path so it matches the restored asset.
                        const FSnowHeightBakeStage2878* PreviousStage = nullptr;
                        for (const FSnowHeightBakeStage2878& Candidate : BakedSnowHeightStages2878)
                        {
                            if (Candidate.SnowAsset.Get() == PreviousSnowAsset)
                            {
                                PreviousStage = &Candidate;
                                break;
                            }
                        }
                        if (PreviousStage)
                        {
                            SnowGaussianComponent->SnowBakedFieldPacked2873 = PreviousStage->SnowFieldPacked;
                            SnowGaussianComponent->SnowBakedFieldSplatCount2873 = PreviousStage->SnowFieldCount;
                            SnowGaussianComponent->SnowBakedFieldPatternCm2873 = PreviousStage->SnowFieldPatternCm;
                            SnowGaussianComponent->SnowBakedFieldLocalToWorld2873 = PreviousStage->SnowFieldLocalToWorld;
                        }
                        else
                        {
                            SnowGaussianComponent->SnowBakedFieldPacked2873.Reset();
                            SnowGaussianComponent->SnowBakedFieldSplatCount2873 = 0;
                            SnowGaussianComponent->SnowBakedFieldPatternCm2873 = 0.0f;
                        }
                        SnowGaussianComponent->SnowBakedFieldSourceAsset2873 = PreviousSnowAsset;
                        SnowGaussianComponent->RefreshSnowCoverageMaskRenderState();
                        SnowHeightBakeStatus2878 = TEXT("눈높이 전환 실패: 이전 Snow 자산 복구 / 원본 GS 유지");
                        UE_LOG(LogTemp, Warning, TEXT("[Snow2879Switch] %s"), *SnowHeightBakeStatus2878);
                    }
                    else
                    {
                        const bool bSourceMaskChanged =
                            GaussianSplatComponent->SnowCoverageMaskSourceAsset.Get() != GaussianSplatComponent->GetSplatAsset() ||
                            GaussianSplatComponent->SnowCoverageMaskSourceCount != StageData.SourceCoverageCount ||
                            GaussianSplatComponent->SnowCoveragePackedMask != StageData.SourceCoverageMask ||
                            !FMath::IsNearlyEqual(GaussianSplatComponent->SnowGaussianBakedPatchThreshold, StageData.SourcePatchThreshold);
                        if (bSourceMaskChanged)
                        {
                            GaussianSplatComponent->SnowCoveragePackedMask = StageData.SourceCoverageMask;
                        }
                        GaussianSplatComponent->SnowCoverageMaskSourceAsset = GaussianSplatComponent->GetSplatAsset();
                        GaussianSplatComponent->SnowCoverageMaskSourceCount = StageData.SourceCoverageCount;
                        GaussianSplatComponent->SnowGaussianBakedPatchThreshold = StageData.SourcePatchThreshold;
                        GaussianSplatComponent->SnowGaussianGeneratedCount = StageData.OriginalGaussianCount;
                        // Snow asset change already refreshed its proxy once.
                        // Refresh the *source* proxy only if its actual baked
                        // coverage or threshold changed between heights.
                        if (bSourceMaskChanged)
                        {
                            GaussianSplatComponent->RefreshSnowCoverageMaskRenderState();
                        }
                        AppliedSnowHeightStage2878 = DesiredSnowStage2878;
                        SnowHeightBakeStatus2878 = FString::Printf(
                            TEXT("요청 %.2fcm → %d/%d 단계 %.2fcm / Snow GS %d / 실제 지오메트리는 단계 전환"),
                            SnowRuntimeHeightCm2878, DesiredSnowStage2878, AvailableSnowStages2879,
                            StageData.HeightCm, StageData.OriginalGaussianCount);
                        UE_LOG(LogTemp, Display, TEXT("[Snow2879Switch] stage=%d/%d height=%.2fcm"),
                            DesiredSnowStage2878, AvailableSnowStages2879, StageData.HeightCm);

                        // v2895 panorama safety: MRQ duplicates the level into a PIE
                        // world, causing every instanced snow-height asset to PostLoad
                        // and create a transient color texture. Only the chosen height
                        // is rendered, so retire GPU resources for the other stages
                        // immediately in PIE. CPU bake payloads/assets stay intact and
                        // an inactive stage recreates its texture if selected later.
                        if (GetWorld() && GetWorld()->WorldType == EWorldType::PIE)
                        {
                            int32 TrimmedStageCount2895 = 0;
                            for (const FSnowHeightBakeStage2878& Candidate : BakedSnowHeightStages2878)
                            {
                                UGaussianSplatAsset* const InactiveAsset = Candidate.SnowAsset.Get();
                                if (InactiveAsset && InactiveAsset != StageData.SnowAsset.Get() &&
                                    InactiveAsset != GaussianSplatComponent->GetSplatAsset())
                                {
                                    InactiveAsset->QueueReleaseUnusedSnowStageGPUCache();
                                    ++TrimmedStageCount2895;
                                }
                            }
                            UE_LOG(LogTemp, Display,
                                TEXT("[Snow2895StageTrim] Active=%d/%d TrimQueued=%d ActiveAsset=%s CPUBakeDataRetained=1"),
                                DesiredSnowStage2878, AvailableSnowStages2879, TrimmedStageCount2895,
                                *GetNameSafe(StageData.SnowAsset.Get()));
                        }
                    }
                }
            }
        }
    }
    // Only retire snow-stage GPU caches after the selection has remained
    // stable for ~90 ticks.  The render-thread job checks shared ownership;
    // assets still used by a proxy cannot be evicted.  CPU bake payloads and
    // the source 3DGS RenderData stay resident for fast re-selection.
    if (bActualSnow && bHasMultiHeightSnow2878 && AppliedSnowHeightStage2878 >= 0)
    {
        if (SnowCacheTrackedStage2880 != AppliedSnowHeightStage2878)
        {
            SnowCacheTrackedStage2880 = AppliedSnowHeightStage2878;
            SnowCacheStableTicks2880 = 0;
            bSnowCacheCleanupQueued2880 = false;
        }
        else if (!bSnowCacheCleanupQueued2880 && ++SnowCacheStableTicks2880 >= 90)
        {
            const UGaussianSplatAsset* ActiveSnowAsset = SnowGaussianComponent
                ? SnowGaussianComponent->GetSplatAsset() : nullptr;
            for (const FSnowHeightBakeStage2878& Candidate : BakedSnowHeightStages2878)
            {
                UGaussianSplatAsset* InactiveAsset = Candidate.SnowAsset.Get();
                if (InactiveAsset && InactiveAsset != ActiveSnowAsset &&
                    InactiveAsset != GaussianSplatComponent->GetSplatAsset())
                {
                    InactiveAsset->QueueReleaseUnusedSnowStageGPUCache();
                }
            }
            bSnowCacheCleanupQueued2880 = true;
        }
    }
    // A requested but invalid/unregistered stage must NEVER be presented as active.
    // Derive render visibility and original-GS color from the last SUCCESSFUL stage.
    const int32 PresentationSnowStage2879 = bHasMultiHeightSnow2878 ? AppliedSnowHeightStage2878 : -1;
    const bool bSnowHeightZero2878 = bHasMultiHeightSnow2878 && PresentationSnowStage2879 == 0;
    const bool bActualDawn = GaussianSplatComponent->WeatherMode == ELCCCinematicGSWeatherMode::Dawn;
	const bool bSnowGaussianOwnsPresentation =
		bActualSnow && !bBuildingSnowHeightStages2878 && !bSnowHeightZero2878 &&
		GaussianSplatComponent->ProxySurfaceEffectMode == ELCCCinematicGSProxySurfaceEffectMode::Snow &&
		GaussianSplatComponent->bUseSnowGaussianLayer &&
		BakedSnowGaussianAsset && BakedSnowGaussianAsset->IsValid();
	// v2838: the original GS splats own the broad snow surface. The baked
	// Gaussian asset provides sparse physical relief only (no legacy overlay).
    // The source-GS snow shade fades with the baked snow stage too. Full height
    // retains the exact 2.8.77 blend; zero height keeps the untouched original.
    // Interpolate the *source GS color* continuously, but only for the stage
    // that was successfully installed. An invalid/deferred asset must not
    // recolor the source for a stage that is not yet on screen.
    const float AppliedSourceHeight2879 = bHasMultiHeightSnow2878 &&
        PresentationSnowStage2879 >= 0 && DesiredSnowStage2878 != PresentationSnowStage2879
        ? (PresentationSnowStage2879 > 0
            ? BakedSnowHeightStages2878[PresentationSnowStage2879 - 1].HeightCm : 0.0f)
        : RequestedSnowHeight2879;
    const float SnowHeightSourceFactor2878 = bHasMultiHeightSnow2878 &&
        PresentationSnowStage2879 >= 0
        ? FMath::Sqrt(FMath::Clamp(
            AppliedSourceHeight2879 / SnowBakedMaximumHeightCm2878, 0.0f, 1.0f)) : 1.0f;
    const float HybridBaseMix = bSnowHeightZero2878 ? 0.0f : bSnowGaussianOwnsPresentation
        ? FMath::Clamp(GaussianSplatComponent->SnowHybridBaseCoverageStrength, 0.0f, 1.0f)
            * SnowHeightSourceFactor2878 : 1.0f;

	if (SnowGaussianComponent)
	{
		// The snow asset is an actor-owned, serializable subobject; this binding also
		// repairs references after MRQ duplicates the editor world.
		if (bSnowGaussianOwnsPresentation && SnowGaussianComponent->GetSplatAsset() != BakedSnowGaussianAsset.Get())
		{
			SnowGaussianComponent->SetSplatAsset(BakedSnowGaussianAsset.Get());
		}
		const bool bShowSnow = bSnowGaussianOwnsPresentation && SnowGaussianComponent->GetSplatAsset() == BakedSnowGaussianAsset.Get();
		// v2896: publish the *actual* baked snow layer visibility. The previous
		// panorama drain looked for the legacy SnowIntermediate overlay, which is
		// deliberately absent for baked relief, so it never ran.
		FGaussianSplatViewExtension::SetBakedSnowGaussianVisible(bShowSnow);
		if (SnowGaussianComponent->IsVisible() != bShowSnow)
		{
			SnowGaussianComponent->SetVisibility(bShowSnow, false);
		}
		// UE's setter already ignores an unchanged hidden state; do not access
		// component implementation flags directly from the actor.
		SnowGaussianComponent->SetHiddenInGame(!bShowSnow);
	}
	const bool bSurfaceEffectAllowed =
		// Rain never uses the retired mesh/legacy splat wet pass.
		(bActualSnow && (bSnowGaussianOwnsPresentation || HybridBaseMix > 0.0001f) &&
		 GaussianSplatComponent->ProxySurfaceEffectMode == ELCCCinematicGSProxySurfaceEffectMode::Snow);
	const ELCCCinematicGSProxySurfaceEffectMode ActiveSurfaceMode = bActualRain
		? ELCCCinematicGSProxySurfaceEffectMode::WetPuddles
		: GaussianSplatComponent->ProxySurfaceEffectMode;
	const FLinearColor SurfaceColor = GaussianSplatComponent->ProxySurfaceEffectColor;
	const FVector4f DirectSettings0(
		bSurfaceEffectAllowed ? static_cast<float>(ActiveSurfaceMode) : 0.0f,
		bSurfaceEffectAllowed ? GaussianSplatComponent->ProxySurfaceEffectStrength : 0.0f,
		// Match the snow bake's proxy slope threshold when source GS owns the
		// snow surface. Wet/puddles retain their original horizontal bias.
		bSnowGaussianOwnsPresentation
			? GaussianSplatComponent->SnowGaussianSlopeThreshold
			: GaussianSplatComponent->ProxySurfaceHorizontalBias,
		(bSnowGaussianOwnsPresentation ? GaussianSplatComponent->SnowGaussianMoundScaleCm
			: GaussianSplatComponent->ProxySurfacePatternScale));
	const FLinearColor DirectSnowSourceColor = bSnowGaussianOwnsPresentation
		? GaussianSplatComponent->SnowGaussianColor : SurfaceColor;
	const FVector4f DirectSettings1(
		DirectSnowSourceColor.R, DirectSnowSourceColor.G, DirectSnowSourceColor.B,
		GaussianSplatComponent->ProxySurfaceSpecularStrength);
	const bool bWetSurfaceMode = bActualRain;
	const float WetGroundFullDistanceCm = FMath::Max(GaussianSplatComponent->ProxyWetGroundFullDistanceCm, 0.0f);
	const float WetGroundMaxDistanceCm = FMath::Max(
		GaussianSplatComponent->ProxyWetGroundMaxDistanceCm, 0.0f);
	const float EncodedWetGroundMaxDistanceCm = GaussianSplatComponent->bPreviewWetGroundMask
		? -WetGroundMaxDistanceCm
		: WetGroundMaxDistanceCm;
	const FVector4f DirectSettings2(
		bWetSurfaceMode ? WetGroundFullDistanceCm : GaussianSplatComponent->ProxySnowThicknessCm,
		bWetSurfaceMode ? EncodedWetGroundMaxDistanceCm :
			(bSnowGaussianOwnsPresentation ? GaussianSplatComponent->SnowGaussianBakedPatchThreshold : GaussianSplatComponent->ProxySnowSplatExpansion),
		(bActualRain || bSnowGaussianOwnsPresentation) ? 0.0f : GaussianSplatComponent->ProxyWetDarkening,
		0.0f); // retired legacy surface puddle path
	const FVector4f DirectSettings3(
		GaussianSplatComponent->ProxyRainRippleStrength,
		GaussianSplatComponent->ProxyRainRippleSizeCm,
		GaussianSplatComponent->ProxyRainRippleSpacingCm,
		GaussianSplatComponent->ProxyRainRippleSpeed);
	const FVector4f DirectSnowTuning(
		FMath::Clamp(GaussianSplatComponent->ProxySnowGroundFillStrength, 0.0f, 1.0f),
		FMath::Clamp(GaussianSplatComponent->ProxySnowStructureRemoval, 0.0f, 1.0f),
		FMath::Clamp(GaussianSplatComponent->ProxySnowStructureSlopeThreshold, 0.0f, 1.0f),
		// Encoded snow-only hybrid state (read only by Snow in CalcViewData):
		// 0 legacy surface overlay, 1+mix/2 hybrid without sky test,
		// 2+mix/2 hybrid with static world-up shelter test.
		bSnowGaussianOwnsPresentation
			? (GaussianSplatComponent->bSnowGaussianSkyExposure ? 2.0f : 1.0f) + 0.5f * HybridBaseMix
			: 0.0f);

	const FVector4f SnowSourceCoverSettings(
		bSnowGaussianOwnsPresentation && GaussianSplatComponent->bSnowSourceCover2872
			? FMath::Clamp(GaussianSplatComponent->SnowSourceCoverStrength2872,0.0f,1.0f) : 0.0f,
		0.0f,0.0f,0.0f);
	const FVector4f RainGrade0 = bActualRain
		? FVector4f(
			FMath::Clamp(GaussianSplatComponent->RainOverlayStrength, 0.0f, 1.0f),
			FMath::Clamp(GaussianSplatComponent->RainOverlayExposureEV, -2.0f, 2.0f),
			FMath::Clamp(GaussianSplatComponent->RainOverlayGamma, 0.5f, 2.0f),
			FMath::Clamp(GaussianSplatComponent->RainOverlaySaturation, 0.0f, 2.0f))
		: FVector4f(0.0f, 0.0f, 1.0f, 1.0f);
	const FLinearColor RainTint = GaussianSplatComponent->RainOverlayTint;
	const FVector4f RainGrade1 = bActualRain
		? FVector4f(RainTint.R, RainTint.G, RainTint.B,
			FMath::Clamp(GaussianSplatComponent->RainOverlayTintStrength, 0.0f, 1.0f))
		: FVector4f(1.0f, 1.0f, 1.0f, 0.0f);
	if (bOwnsWorldWeather)
	{
		ENQUEUE_RENDER_COMMAND(UpdateGaussianDirectWeatherSettings)(
			[DirectSettings0, DirectSettings1, DirectSettings2, DirectSettings3, DirectSnowTuning, SnowSourceCoverSettings, RainGrade0, RainGrade1, bActualRain, bRainPrecipitationActive,
			 WetTestMode = 0, // legacy wet pass always retired from Rain
			 bWetPreviewRipples = false,
			 PuddleDampen = 0.0f,
             PuddleHide = false,
			 PuddleSourceWet = bActualRain && bPuddleAvailable
				 ? FVector2f(FMath::Clamp(PuddleSourceWetStrength, 0.0f, 1.0f), 0.0f)
				 : FVector2f::ZeroVector,
				 // P11: the dedicated transparent water layer now follows the existing rain-ripple UI
				 // (strength / size / spacing / speed). Only film opacity remains an extra water-layer control.
				 PuddleWaterSettings = FVector4f(
					 FMath::Clamp(PuddleWaterFilmOpacity, 0.0f, 0.90f),
					 FMath::Clamp(GaussianSplatComponent->ProxyRainRippleStrength, 0.0f, 2.0f),
					 FMath::Clamp(GaussianSplatComponent->ProxyRainRippleSpacingCm, 10.0f, 500.0f),
					 FMath::Clamp(GaussianSplatComponent->ProxyRainRippleSpeed, 0.0f, 5.0f)),
				 PuddleRippleSizeCm = FMath::Clamp(GaussianSplatComponent->ProxyRainRippleSizeCm, 5.0f, 200.0f),
                 PuddleReflection = FMath::Clamp(PuddleWaterReflectionStrength, 0.0f, 2.0f),
                 PuddleRoofCellCm = FMath::Clamp(PuddleRoofMicroSpacingCm, 12.0f, 25.0f),
                 PuddleRoofFX = FVector4f(bPuddleRoofUseDownflow ? 1.0f : 0.0f,
                                          0.0f,
                                          FMath::Clamp(PuddleRoofFlowStrength, 0.0f, 2.0f),
                                          FMath::Clamp(PuddleRoofFlowSpeed, 0.0f, 3.0f))](FRHICommandListImmediate&)
			{
				FGaussianSplatRenderer::SetWetReceiverTestMode(bActualRain ? WetTestMode : 0, bWetPreviewRipples);
				FGaussianSplatRenderer::SetPuddleDampenStrength(PuddleDampen, PuddleHide, PuddleSourceWet);
				FGaussianSplatRenderer::SetPuddleWaterVisualSettings(PuddleWaterSettings, PuddleRippleSizeCm, PuddleReflection, PuddleRoofCellCm, PuddleRoofFX);
				FGaussianSplatRenderer::SetDirectSurfaceEffectSettings(
					DirectSettings0, DirectSettings1, DirectSettings2, DirectSettings3);
				FGaussianSplatRenderer::SetDirectSnowTuningSettings(DirectSnowTuning);
				FGaussianSplatRenderer::SetSnowSourceCoverSettings(SnowSourceCoverSettings);
				FGaussianSplatRenderer::SetDirectRainOverlaySettings(RainGrade0, RainGrade1);
				FGaussianSplatRenderer::SetDirectRainRuntimeState(bActualRain, bRainPrecipitationActive);
			});
		// Retire any serialized pre-P19 WetMesh and clear its previously published
		// GPU scene buffer once. Do not build or render legacy wet meshes in Rain.
		if (GaussianSplatComponent->WetReceiverTestMode != EGSWetReceiverTestMode::SplatSize)
		{
			GaussianSplatComponent->WetReceiverTestMode = EGSWetReceiverTestMode::SplatSize;
		}
		SyncWetSurfaceMesh();
	}
	UNiagaraSystem* DesiredSystem = nullptr;
	switch (GaussianSplatComponent->WeatherMode)
	{
	case ELCCCinematicGSWeatherMode::Clear: DesiredSystem = GaussianSplatComponent->ClearWeatherNiagara; break;
	case ELCCCinematicGSWeatherMode::Dawn: DesiredSystem = GaussianSplatComponent->DawnWeatherNiagara; break;
	case ELCCCinematicGSWeatherMode::Fog: DesiredSystem = GaussianSplatComponent->FogWeatherNiagara; break;
	case ELCCCinematicGSWeatherMode::Rain:
		DesiredSystem = bRainPrecipitationActive ? GaussianSplatComponent->RainWeatherNiagara : nullptr;
		break;
	case ELCCCinematicGSWeatherMode::Snow: DesiredSystem = GaussianSplatComponent->SnowWeatherNiagara; break;
	}
	const bool bDawnStarSystem = bActualDawn && DesiredSystem &&
		(DesiredSystem->GetName().Equals(TEXT("star"), ESearchCase::IgnoreCase) ||
		 DesiredSystem->GetName().Equals(TEXT("night_star"), ESearchCase::IgnoreCase));
	// One camera-following weather emitter per world. This prevents duplicate rain
	// while still allowing a newly added Rain actor to become the authority over
	// existing Clear actors.
	if (!bOwnsWorldWeather)
	{
		DesiredSystem = nullptr;
	}
	// A separately placed night-star Niagara actor is not part of the GS weather
	// component, but it uses the same After Motion Blur material contract as rain
	// and snow. Detect only that explicitly named system in this world so its
	// particles survive the final GS composite without changing unrelated effects.
	bool bExternalNightStarNiagaraVisible = false;
	if (bOwnsWorldWeather)
	{
		for (TObjectIterator<UNiagaraComponent> It; It; ++It)
		{
			UNiagaraComponent* NiagaraComponent = *It;
			UNiagaraSystem* NiagaraAsset = NiagaraComponent ? NiagaraComponent->GetAsset() : nullptr;
			if (NiagaraComponent && NiagaraAsset &&
				NiagaraComponent != WeatherNiagaraComponent &&
				NiagaraComponent->GetWorld() == GetWorld() &&
				NiagaraComponent->IsRegistered() && NiagaraComponent->IsActive() &&
				NiagaraComponent->IsVisible() &&
				NiagaraAsset->GetName().Equals(TEXT("night_star"), ESearchCase::IgnoreCase))
			{
				bExternalNightStarNiagaraVisible = true;
				break;
			}
		}
	}
	// Rain/snow and the authored Dawn particle effect use After Motion Blur
	// translucency and must remain above the final GS composite. The explicitly
	// named external night-star system uses that same ordering. Other Niagara
	// systems and Clear mode remain untouched.
	if (bOwnsWorldWeather)
	{
		FGaussianSplatViewExtension::SetRainNiagaraCompositeVisible(
			((bActualRain || bActualSnow) && DesiredSystem != nullptr) ||
			bDawnStarSystem ||
			bExternalNightStarNiagaraVisible);
	}
	// Rain-only camera tracking: use the actual rendered FSceneView when available.
	// Other weather systems retain their original actor-relative behavior.
	if (GaussianSplatComponent->WeatherMode == ELCCCinematicGSWeatherMode::Rain)
	{
		// Rain only: follow the rendered camera position, but never inherit the
		// Gaussian actor rotation. The rain system must stay world-vertical.
		WeatherNiagaraComponent->SetAbsolute(true, true, false);
		WeatherNiagaraComponent->SetWorldRotation(FRotator::ZeroRotator);
		FVector WeatherAnchor = GetActorLocation();
		bool bHasWeatherCamera = false;
		UWorld* WeatherWorld = GetWorld();
		if (WeatherWorld && WeatherWorld->Scene)
		{
			if (FGaussianSplatViewExtension* ViewExtension = FGaussianSplatViewExtension::Get())
			{
				bHasWeatherCamera = ViewExtension->GetLatestRenderedViewLocation(WeatherWorld->Scene, WeatherAnchor);
			}
		}
		if (!bHasWeatherCamera && WeatherWorld)
		{
			const bool bGameLikeWorld = WeatherWorld->WorldType == EWorldType::Game ||
				WeatherWorld->WorldType == EWorldType::PIE ||
				WeatherWorld->WorldType == EWorldType::GamePreview;
			if (bGameLikeWorld)
			{
				if (APlayerController* PlayerController = WeatherWorld->GetFirstPlayerController())
				{
					if (PlayerController->PlayerCameraManager)
					{
						WeatherAnchor = PlayerController->PlayerCameraManager->GetCameraLocation();
						bHasWeatherCamera = true;
					}
				}
			}
		}
#if WITH_EDITOR
		if (!bHasWeatherCamera && GCurrentLevelEditingViewportClient)
		{
			WeatherAnchor = GCurrentLevelEditingViewportClient->GetViewLocation();
		}
#endif
		// Keep the emitter origin above the camera so the spawn start plane is not
		// visible in the viewport. Keep it only slightly above the camera; 600 cm was
		// high enough for short-lived rain particles to die before entering view.
		// X/Y still use the existing authored weather offset.
		FVector RainCameraOffset = GaussianSplatComponent->WeatherNiagaraOffset;
		RainCameraOffset.Z = FMath::Max(RainCameraOffset.Z, 200.0);
		WeatherNiagaraComponent->SetWorldLocation(WeatherAnchor + RainCameraOffset);
	}
	else
	{
		// Preserve the original v6 attachment behavior for every non-Rain weather.
		WeatherNiagaraComponent->SetAbsolute(false, false, false);
		WeatherNiagaraComponent->SetRelativeRotation(FRotator::ZeroRotator);
		WeatherNiagaraComponent->SetRelativeLocation(GaussianSplatComponent->WeatherNiagaraOffset);
	}
	WeatherNiagaraComponent->SetRelativeScale3D(FVector(FMath::Max(GaussianSplatComponent->WeatherNiagaraScale, 0.01f)));

	if (GaussianSplatComponent->WeatherMode == ELCCCinematicGSWeatherMode::Rain)
	{
		// Rain only: editor reconstruction can leave the component with Asset=None even
		// while the cached pointer still matches. Rebind and keep Rain alive.
		const bool bNiagaraAssetMismatch =
			WeatherNiagaraComponent->GetAsset() != DesiredSystem ||
			LastWeatherNiagaraSystem.Get() != DesiredSystem;
		if (bNiagaraAssetMismatch)
		{
			WeatherNiagaraComponent->DeactivateImmediate();
			WeatherNiagaraComponent->SetAsset(DesiredSystem);
			LastWeatherNiagaraSystem = DesiredSystem;
			if (DesiredSystem)
			{
				WeatherNiagaraComponent->ReinitializeSystem();
			}
		}
		if (DesiredSystem)
		{
			WeatherNiagaraComponent->SetVisibility(true, true);
			WeatherNiagaraComponent->SetHiddenInGame(false, true);
			WeatherNiagaraComponent->SetComponentTickEnabled(true);
			if (!WeatherNiagaraComponent->IsActive())
			{
				WeatherNiagaraComponent->Activate(true);
			}
		}
		else
		{
			WeatherNiagaraComponent->DeactivateImmediate();
			WeatherNiagaraComponent->SetVisibility(false, true);
			WeatherNiagaraComponent->SetHiddenInGame(true, true);
			WeatherNiagaraComponent->SetComponentTickEnabled(false);
		}
	}
	else
	{
		// Compare the real component asset as well as the cache. Editor reconstruction
		// can reset LastWeatherNiagaraSystem while leaving Rain active on the component.
		const bool bNiagaraAssetMismatch =
			WeatherNiagaraComponent->GetAsset() != DesiredSystem ||
			LastWeatherNiagaraSystem.Get() != DesiredSystem;
		if (bNiagaraAssetMismatch)
		{
			WeatherNiagaraComponent->DeactivateImmediate();
			WeatherNiagaraComponent->SetAsset(DesiredSystem);
			LastWeatherNiagaraSystem = DesiredSystem;
		}
		if (DesiredSystem)
		{
			WeatherNiagaraComponent->SetVisibility(true, true);
			WeatherNiagaraComponent->SetHiddenInGame(false, true);
			WeatherNiagaraComponent->SetComponentTickEnabled(true);
			if (!WeatherNiagaraComponent->IsActive())
			{
				WeatherNiagaraComponent->Activate(true);
			}
		}
		else
		{
			WeatherNiagaraComponent->DeactivateImmediate();
			WeatherNiagaraComponent->SetVisibility(false, true);
			WeatherNiagaraComponent->SetHiddenInGame(true, true);
			WeatherNiagaraComponent->SetComponentTickEnabled(false);
		}
	}

	const uint8 CurrentWeatherMode = static_cast<uint8>(GaussianSplatComponent->WeatherMode);
	// P49: clearing is a continuous increase of optical visibility, without
	// moving an empty camera-centred sphere through otherwise dense fog.
	// Native meshes and GS both receive the same density and zero hard start.
	const float FogBlend = FMath::Clamp(GaussianSplatComponent->FogWeatherStrength, 0.0f, 1.0f);
	const float DenseFogAlpha = FMath::Clamp(GaussianSplatComponent->FogWeatherStrength - 1.0f, 0.0f, 1.0f);
	const float DenseFogProgress = DenseFogAlpha * DenseFogAlpha * (3.0f - 2.0f * DenseFogAlpha);
	const float FogVisibility = FMath::Clamp(GaussianSplatComponent->FogVisibilityDistance, 500.0f, 50000.0f);
	const bool bFogOverlayRequested =
		GaussianSplatComponent->WeatherMode == ELCCCinematicGSWeatherMode::Fog ||
		GaussianSplatComponent->bEnableFogOverlayInOtherWeather;
	const bool bFogOverlayActive = bFogOverlayRequested && FogBlend > 0.001f;
	const bool bDedicatedFogWeatherActive =
		GaussianSplatComponent->WeatherMode == ELCCCinematicGSWeatherMode::Fog &&
		FogBlend > 0.001f;

	// Keep the approved current Fog look as the neutral point (all normalized scales = 1).
	// This lets the existing "03 대기 및 안개" controls drive the panorama-safe
	// fog bridge without changing the current Fog preset at its existing values.
	const float FogDensityProfile = FMath::Clamp(
		(GaussianSplatComponent->AtmosphereStrength / 1.38f) *
		(GaussianSplatComponent->DistanceHaze / 1.55f), 0.0f, 3.0f);
	const float FogPocketProfile = FMath::Clamp(
		GaussianSplatComponent->GroundFogDensity / 1.12f, 0.0f, 3.0f);
	const float FogVariationProfile = FMath::Clamp(
		GaussianSplatComponent->FogDensityVariation, 0.0f, 1.0f);
	const float FogHeightProfile = FMath::Clamp(
		GaussianSplatComponent->FogHeightFalloff / 1.65f, 0.10f, 4.0f);
	const FLinearColor BaseFogColor(
		FMath::Clamp(GaussianSplatComponent->FogColor.R, 0.0f, 1.0f),
		FMath::Clamp(GaussianSplatComponent->FogColor.G, 0.0f, 1.0f),
		FMath::Clamp(GaussianSplatComponent->FogColor.B, 0.0f, 1.0f),
		1.0f);

	// P50: a configurable neutral overcast endpoint for dense daytime fog.
	const FLinearColor DenseFogColor = FMath::Lerp(
		BaseFogColor, GaussianSplatComponent->DenseFogScatteringColor.GetClamped(0.0f, 1.0f),
		DenseFogProgress);

	uint32 FogControlHash = 0u;
	auto AddFogHash = [&FogControlHash](uint32 Value)
	{
		FogControlHash = HashCombine(FogControlHash, Value);
	};
	AddFogHash(GetTypeHash(GaussianSplatComponent->bEnableFogOverlayInOtherWeather));
	AddFogHash(GetTypeHash(DenseFogColor.R));
	AddFogHash(GetTypeHash(DenseFogColor.G));
	AddFogHash(GetTypeHash(DenseFogColor.B));
	AddFogHash(GetTypeHash(GaussianSplatComponent->DenseFogScatteringColor));
	AddFogHash(GetTypeHash(GaussianSplatComponent->AtmosphereStrength));
	AddFogHash(GetTypeHash(GaussianSplatComponent->GroundFogDensity));
	AddFogHash(GetTypeHash(GaussianSplatComponent->DistanceHaze));
	AddFogHash(GetTypeHash(GaussianSplatComponent->FogHeightFalloff));
	AddFogHash(GetTypeHash(GaussianSplatComponent->FogDensityVariation));
	// Native UE fog controls and the GS bridge must update together.
	AddFogHash(GetTypeHash(GaussianSplatComponent->NativeFogDensity));
	AddFogHash(GetTypeHash(GaussianSplatComponent->NativeFogStartDistanceCm));
	AddFogHash(GetTypeHash(GaussianSplatComponent->NativeFogMaxOpacity));
	AddFogHash(GetTypeHash(GaussianSplatComponent->bEnableNativeVolumetricFog));
	// FogBlend saturates at 1; include dense-fog progress to update the fog
	// component and GS shader for every animation frame between 1 and 2.
	AddFogHash(GetTypeHash(DenseFogProgress));

	if (LastSyncedWeatherMode != CurrentWeatherMode ||
		!FMath::IsNearlyEqual(LastSyncedFogWeatherStrength, FogBlend, 0.001f) ||
		!FMath::IsNearlyEqual(LastSyncedFogVisibilityDistance, FogVisibility, 1.0f) ||
		LastSyncedFogControlHash != FogControlHash)
	{
		LastSyncedWeatherMode = CurrentWeatherMode;
		LastSyncedFogWeatherStrength = FogBlend;
		LastSyncedFogVisibilityDistance = FogVisibility;
		LastSyncedFogControlHash = FogControlHash;

		// Materials marked "Is Sky" bypass ExponentialHeightFog. Keep the original
		// v2819 behavior: hide named sky spheres only in the dedicated Fog weather.
		// Cross-weather fog never changes sky visibility.
		if (UWorld* World = GetWorld())
		{
			for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
			{
				AStaticMeshActor* MeshActor = *It;
				const FString ActorName = MeshActor->GetName();
				const FString DisplayLabel =
#if WITH_EDITOR
					MeshActor->GetActorLabel();
#else
					ActorName;
#endif
				if (ActorName.Contains(TEXT("SkySphere"), ESearchCase::IgnoreCase) ||
					DisplayLabel.Contains(TEXT("SkySphere"), ESearchCase::IgnoreCase) ||
					ActorName.Contains(TEXT("Sky_Sphere"), ESearchCase::IgnoreCase) ||
					DisplayLabel.Contains(TEXT("Sky_Sphere"), ESearchCase::IgnoreCase))
				{
					MeshActor->SetActorHiddenInGame(bDedicatedFogWeatherActive);
					if (UStaticMeshComponent* SkyMesh = MeshActor->GetStaticMeshComponent())
					{
						SkyMesh->SetVisibility(!bDedicatedFogWeatherActive, true);
					}
				}
			}

			USkyAtmosphereComponent* SkyAtmosphere = CachedWeatherSkyAtmosphere.Get();
			if (!SkyAtmosphere)
			{
				for (TObjectIterator<USkyAtmosphereComponent> It; It; ++It)
				{
					if (It->GetWorld() == World)
					{
						SkyAtmosphere = *It;
						CachedWeatherSkyAtmosphere = SkyAtmosphere;
						break;
					}
				}
			}
			if (SkyAtmosphere)
			{
				if (!bCachedOriginalSkyAtmosphere)
				{
					bCachedOriginalSkyAtmosphere = true;
					OriginalRayleighScatteringScale = SkyAtmosphere->RayleighScatteringScale;
					OriginalMieScatteringScale = SkyAtmosphere->MieScatteringScale;
					OriginalMieAbsorptionScale = SkyAtmosphere->MieAbsorptionScale;
					OriginalMieAnisotropy = SkyAtmosphere->MieAnisotropy;
					OriginalAerialPerspectiveScale = SkyAtmosphere->AerialPespectiveViewDistanceScale;
					OriginalAerialPerspectiveStartDepth = SkyAtmosphere->AerialPerspectiveStartDepth;
					OriginalHeightFogContribution = SkyAtmosphere->HeightFogContribution;
					OriginalSkyLuminanceFactor = SkyAtmosphere->SkyLuminanceFactor;
					OriginalSkyAerialLuminanceFactor = SkyAtmosphere->SkyAndAerialPerspectiveLuminanceFactor;
				}
				if (bDedicatedFogWeatherActive)
				{
					// Dedicated Fog may own the atmospheric phase. Cross-weather fog never
					// touches SkyAtmosphere, preserving Rain/Snow/Clear/Dawn lighting exactly.
					SkyAtmosphere->SetRayleighScatteringScale(
						FMath::Lerp(OriginalRayleighScatteringScale, 0.05f, FogBlend));
					SkyAtmosphere->SetMieScatteringScale(
						FMath::Lerp(OriginalMieScatteringScale, 8.0f, FogBlend));
					SkyAtmosphere->SetMieAbsorptionScale(
						FMath::Lerp(OriginalMieAbsorptionScale, 1.8f, FogBlend));
					SkyAtmosphere->SetMieAnisotropy(
						FMath::Lerp(OriginalMieAnisotropy, 0.18f, FogBlend));
					SkyAtmosphere->SetSkyLuminanceFactor(
						FMath::Lerp(OriginalSkyLuminanceFactor, FLinearColor(0.16f, 0.20f, 0.27f, 1.0f), FogBlend));
					SkyAtmosphere->SetSkyAndAerialPerspectiveLuminanceFactor(
						FMath::Lerp(OriginalSkyAerialLuminanceFactor, FLinearColor(0.24f, 0.29f, 0.37f, 1.0f), FogBlend));
					SkyAtmosphere->SetAerialPespectiveViewDistanceScale(
						FMath::Lerp(OriginalAerialPerspectiveScale, 3.0f, FogBlend));
					SkyAtmosphere->SetAerialPerspectiveStartDepth(
						FMath::Lerp(OriginalAerialPerspectiveStartDepth, 0.001f, FogBlend));
					SkyAtmosphere->SetHeightFogContribution(1.0f);
				}
				else if (bCachedOriginalSkyAtmosphere)
				{
					SkyAtmosphere->SetRayleighScatteringScale(OriginalRayleighScatteringScale);
					SkyAtmosphere->SetMieScatteringScale(OriginalMieScatteringScale);
					SkyAtmosphere->SetMieAbsorptionScale(OriginalMieAbsorptionScale);
					SkyAtmosphere->SetMieAnisotropy(OriginalMieAnisotropy);
					SkyAtmosphere->SetSkyLuminanceFactor(OriginalSkyLuminanceFactor);
					SkyAtmosphere->SetSkyAndAerialPerspectiveLuminanceFactor(OriginalSkyAerialLuminanceFactor);
					SkyAtmosphere->SetAerialPespectiveViewDistanceScale(OriginalAerialPerspectiveScale);
					SkyAtmosphere->SetAerialPerspectiveStartDepth(OriginalAerialPerspectiveStartDepth);
					SkyAtmosphere->SetHeightFogContribution(OriginalHeightFogContribution);
				}
			}
		}

		// P46: the fog payload is uploaded AFTER the native component is configured.
		// This avoids the separate noisy/proxy GS fog curve that made splats much
		// more opaque than an ordinary mesh at the same world position.

		// P50: the analytic GS bridge models the shared authored scattering RGB.
		// Do not let native fog add a second sky/sun tint absent from that bridge.
		// Resolve renamed directional colour fields once per control update so
		// both legacy Color and UE 5.8 Luminance property names are supported.
		FStructProperty* DirectionalColorProperty = FindFProperty<FStructProperty>(
			WeatherSkyFogComponent->GetClass(), TEXT("DirectionalInscatteringLuminance"));
		if (!DirectionalColorProperty)
		{
			DirectionalColorProperty = FindFProperty<FStructProperty>(
				WeatherSkyFogComponent->GetClass(), TEXT("DirectionalInscatteringColor"));
		}
		FLinearColor* DirectionalColor = DirectionalColorProperty &&
			DirectionalColorProperty->Struct == TBaseStructure<FLinearColor>::Get()
			? DirectionalColorProperty->ContainerPtrToValuePtr<FLinearColor>(WeatherSkyFogComponent.Get()) : nullptr;
		FFloatProperty* SkyLightProperty = FindFProperty<FFloatProperty>(
			WeatherSkyFogComponent->GetClass(), TEXT("SkyLightCaptureAffectsHeightFogStrength"));
		if (bFogOverlayActive)
		{
			if (!bSharedFogScatteringOverride)
			{
				OriginalFogSkyAmbient = WeatherSkyFogComponent->SkyAtmosphereAmbientContributionColorScale;
				if (DirectionalColor) OriginalFogDirectionalScatter = *DirectionalColor;
				if (SkyLightProperty) OriginalFogSkyLightScatter = SkyLightProperty->GetPropertyValue_InContainer(WeatherSkyFogComponent.Get());
				bSharedFogScatteringOverride = true;
			}
            const bool bScatteringChanged =
                WeatherSkyFogComponent->SkyAtmosphereAmbientContributionColorScale != FLinearColor::Black ||
                (DirectionalColor && *DirectionalColor != FLinearColor::Black) ||
                (SkyLightProperty && SkyLightProperty->GetPropertyValue_InContainer(WeatherSkyFogComponent.Get()) != 0.0f);
            WeatherSkyFogComponent->SkyAtmosphereAmbientContributionColorScale = FLinearColor::Black;
            if (DirectionalColor) *DirectionalColor = FLinearColor::Black;
            if (SkyLightProperty) SkyLightProperty->SetPropertyValue_InContainer(WeatherSkyFogComponent.Get(), 0.0f);
            if (bScatteringChanged) WeatherSkyFogComponent->MarkRenderStateDirty();
		}
		else if (bSharedFogScatteringOverride)
		{
			WeatherSkyFogComponent->SkyAtmosphereAmbientContributionColorScale = OriginalFogSkyAmbient;
			if (DirectionalColor) *DirectionalColor = OriginalFogDirectionalScatter;
			if (SkyLightProperty) SkyLightProperty->SetPropertyValue_InContainer(WeatherSkyFogComponent.Get(), OriginalFogSkyLightScatter);
			bSharedFogScatteringOverride = false;
			WeatherSkyFogComponent->MarkRenderStateDirty();
		}

		// P44: exactly one UE ExponentialHeightFogComponent owns all regular
		// scene-geometry fog. No P41 override; no experimental DOF.
		if (bFogOverlayActive)
		{
			const float NativeFogBaseDensity = FMath::Clamp(
				GaussianSplatComponent->NativeFogDensity, 0.0f, 0.2f);
			const float ClearDistanceCm = FMath::Clamp(
				GaussianSplatComponent->NativeFogStartDistanceCm, 0.0f, 1000000.0f);
			// The legacy start-distance property remains serialized and saved in
			// weather profiles. It now lengthens optical visibility continuously:
			// tau(d) = density * d, rather than density * max(d - start, 0).
			// This leaves a little haze on foreground surfaces instead of a
			// perfectly clear circular hole with an opaque edge.
			const float NearAirDensityScale = FogVisibility / (FogVisibility + ClearDistanceCm);
			const float BaselineDensity = FMath::Clamp(
				NativeFogBaseDensity * (6500.0f / FogVisibility) * FogBlend,
				0.0f, 0.2f) * NearAirDensityScale;
			const float BaselineMaxOpacity = FMath::Clamp(
				GaussianSplatComponent->NativeFogMaxOpacity, 0.0f, 0.999f);
			const float BaselineHeightFalloff = FMath::Clamp(0.18f * FogHeightProfile, 0.02f, 0.9f);
			// Interpolate in log-density (equivalently, reciprocal visibility).
			// P48's linear interpolation from ~0.02 to 48 reached very dense fog
			// too early, concentrating the transition in a thin distance band.
			// Keep zero base density valid and the strength-2 endpoint at 48.
			const float LogDensityFloor = 1.0e-6f;
			const float DensityRatio = (48.0f + LogDensityFloor) / (BaselineDensity + LogDensityFloor);
			const float AppliedDensity = FMath::Clamp(
				(BaselineDensity + LogDensityFloor) * FMath::Pow(DensityRatio, DenseFogProgress)
				- LogDensityFloor, 0.0f, 48.0f);
			// Never move a hard start plane/sphere during a clearing shot.
			const float AppliedStartCm = 0.0f;
			const float AppliedMaxOpacity = FMath::Lerp(BaselineMaxOpacity, 1.0f, DenseFogProgress);
			const float AppliedHeightFalloff = FMath::Lerp(BaselineHeightFalloff, 0.001f, DenseFogProgress);
			WeatherSkyFogComponent->SetVisibility(AppliedDensity > 0.00001f);
			WeatherSkyFogComponent->SetFogDensity(AppliedDensity);
			WeatherSkyFogComponent->SetFogHeightFalloff(AppliedHeightFalloff);
			WeatherSkyFogComponent->SetFogInscatteringColor(DenseFogColor);
			WeatherSkyFogComponent->SetStartDistance(AppliedStartCm);
			WeatherSkyFogComponent->SetFogMaxOpacity(AppliedMaxOpacity);
			WeatherSkyFogComponent->SetFogCutoffDistance(0.0f);
			const bool bVolumetric = GaussianSplatComponent->bEnableNativeVolumetricFog;
			WeatherSkyFogComponent->SetVolumetricFog(bVolumetric);
			if (bVolumetric)
			{
				WeatherSkyFogComponent->SetVolumetricFogScatteringDistribution(0.52f);
				WeatherSkyFogComponent->SetVolumetricFogExtinctionScale(
					FMath::Clamp(FogBlend * FogDensityProfile, 0.0f, 4.0f));
				const FLinearColor Albedo(
					FMath::Clamp(DenseFogColor.R * 2.8235f, 0.0f, 1.0f),
					FMath::Clamp(DenseFogColor.G * 2.5350f, 0.0f, 1.0f),
					FMath::Clamp(DenseFogColor.B * 2.2700f, 0.0f, 1.0f), 1.0f);
				WeatherSkyFogComponent->SetVolumetricFogAlbedo(Albedo.ToFColor(false));
				WeatherSkyFogComponent->SetVolumetricFogDistance(
					FMath::Clamp(FogVisibility * 1.75f, 2500.0f, 80000.0f));
			}
		}
		else
		{
			// Preserve all pre-existing non-fog weather behavior when the optional
			// cross-weather overlay is disabled.
			switch (GaussianSplatComponent->WeatherMode)
			{
			case ELCCCinematicGSWeatherMode::Fog:
			case ELCCCinematicGSWeatherMode::Rain:
				WeatherSkyFogComponent->SetVisibility(false);
				WeatherSkyFogComponent->SetVolumetricFog(false);
				WeatherSkyFogComponent->SetFogDensity(0.0f);
				WeatherSkyFogComponent->SetFogMaxOpacity(0.0f);
				break;
			case ELCCCinematicGSWeatherMode::Snow:
				WeatherSkyFogComponent->SetVisibility(true);
				WeatherSkyFogComponent->SetFogDensity(0.010f);
				WeatherSkyFogComponent->SetFogHeightFalloff(0.15f);
				WeatherSkyFogComponent->SetFogInscatteringColor(FLinearColor(0.72f, 0.80f, 0.92f, 1.0f));
				WeatherSkyFogComponent->SetStartDistance(1200.0f);
				WeatherSkyFogComponent->SetFogMaxOpacity(0.55f);
				WeatherSkyFogComponent->SetVolumetricFog(true);
				WeatherSkyFogComponent->SetVolumetricFogExtinctionScale(0.42f);
				WeatherSkyFogComponent->SetVolumetricFogDistance(16000.0f);
				break;
			default:
				WeatherSkyFogComponent->SetVisibility(false);
				WeatherSkyFogComponent->SetVolumetricFog(false);
				break;
			}
		}

		// P46: read the applied UE Height Fog values back from its component.
		// Pack them into the existing 4-vector shader interface to avoid modifying
		// other shader bindings and all unrelated snow/rain/cast-shadow paths.
		const bool bMatchNativeFog = bFogOverlayActive && WeatherSkyFogComponent->IsVisible() &&
			WeatherSkyFogComponent->FogDensity > 0.000001f;
		const FVector4f NativeFogSettings(
			bMatchNativeFog ? 1.0f : 0.0f,
			FMath::Clamp(WeatherSkyFogComponent->FogDensity, 0.0f, 64.0f),
			FMath::Clamp(WeatherSkyFogComponent->FogMaxOpacity, 0.0f, 1.0f),
			FMath::Clamp(WeatherSkyFogComponent->FogHeightFalloff, 0.001f, 2.0f));
		const FVector4f NativeFogColor(
			DenseFogColor.R, DenseFogColor.G, DenseFogColor.B, 1.0f);
		const FVector4f NativeFogProfile0(
			FMath::Clamp(WeatherSkyFogComponent->StartDistance, 0.0f, 1000000.0f),
			static_cast<float>(WeatherSkyFogComponent->GetComponentLocation().Z),
			0.0f, 0.0f);
		// P49: no full-screen veil or moving hard start. The UE component supplies
		// fog density, start, height falloff and maximum opacity.
		const FVector4f NativeFogProfile1(0.0f, 0.0f, 0.0f, 0.0f);
		ENQUEUE_RENDER_COMMAND(UpdateGaussianNativeFogComposite)(
			[NativeFogSettings, NativeFogColor, NativeFogProfile0, NativeFogProfile1](FRHICommandListImmediate& RHICmdList)
			{
				FGaussianSplatRenderer::SetNativeFogCompositeSettings(
					NativeFogSettings, NativeFogColor, NativeFogProfile0, NativeFogProfile1);
			});
	}

}

void AGaussianSplatActor::SyncProxyShadowReceiver()
{
	if (HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_ClassDefaultObject) ||
		!GaussianSplatComponent || !ProxyShadowReceiverComponent)
	{
		return;
	}
	// Camera-projected surface effects must refresh after receiver presentation changes.
	// LightDepth has its own content-based invalidation key below and must not be reset here.
	bHasProxyCaptureState = false;
	// Binding persistence is independent of whether cinematic effects or proxy
	// presentation are currently enabled. OnRegister may have run before asset
	// references were deserialized, so retry as soon as actor synchronization begins.
	if (GaussianSplatComponent->StableBindingState == EStableProxyBindingState::Uninitialized &&
		GaussianSplatComponent->SplatAsset && GaussianSplatComponent->ProxyShadowMesh)
	{
		GaussianSplatComponent->TryLoadStableProxyBindingCache();
	}

	const bool bHasProxyMesh =
		GaussianSplatComponent->bEnableCinematicEffects &&
		GaussianSplatComponent->bUseProxyMeshForShadowCatcher &&
		GaussianSplatComponent->ProxyShadowMesh != nullptr;
	const bool bMeshDebugMode = bHasProxyMesh &&
		(GaussianSplatComponent->LightingPreviewMode == ELCCCinematicGSLightingPreviewMode::ProxyMeshDebug ||
		 static_cast<int32>(ShadowDebugMode) == 73);
	bStableProxyMeshOnlyPresentationActive = bMeshDebugMode && static_cast<int32>(ShadowDebugMode) == 73;
	const bool bCaptureMaskMode = bHasProxyMesh &&
		RequiresProxyShadowCaptures(GaussianSplatComponent->LightingPreviewMode);
	const bool bSurfaceEffectMode = bHasProxyMesh &&
		GaussianSplatComponent->ProxySurfaceEffectMode != ELCCCinematicGSProxySurfaceEffectMode::Disabled &&
		GaussianSplatComponent->ProxySurfaceEffectStrength > 0.001f;
	const bool bSurfaceMaterialMode = bHasProxyMesh && !bSurfaceEffectMode &&
		GaussianSplatComponent->ProxySurfaceLayers.ContainsByPredicate(
			[](const FLCCCinematicGSProxySurfaceLayer& Layer)
			{
				return Layer.bEnabled && Layer.Material != nullptr;
			});
	const int32 StableDebugModeValue = static_cast<int32>(ShadowDebugMode);
	// v2780 critical production fix: v2778 correctly removed Final from the LEGACY
	// camera-AOV capture selector, but SyncProxyShadowReceiver also used that selector
	// to hide the proxy from *every* SceneCapture. Final/mode-02 Stable LightDepth then
	// sampled an empty/fail-open receiver depth and could not create projected cast
	// shadows at all. Keep legacy captures retired, but make the proxy receiver depth-
	// renderable for the sole Stable directional LightDepth authority.
	const bool bStableLightDepthMode = bHasProxyMesh &&
		(StableDebugModeValue == 0 || StableDebugModeValue == 72 ||
		 (StableDebugModeValue >= 74 && StableDebugModeValue <= 78));
	const bool bAnyProxyCaptureVisibility =
		bCaptureMaskMode || bSurfaceEffectMode || bSurfaceMaterialMode || bStableLightDepthMode;

	// Mesh Debug is the only mode that exposes the UE-lit proxy itself. Stable Final
	// keeps it SceneCapture-only, so enabling its depth pass cannot reveal proxy color.
	// Only the explicitly named Actor mode 03 may replace the GS presentation.
	// A stale component-level ProxyMeshDebug selection must not keep the 3DGS hidden
	// after the Actor selector moves back to Final or Stable Proxy Lighting.
	// Keep the attachment parent visible in every mode. ProxyShadowReceiverComponent
	// is a child of the GS component, so hiding the parent also hid the proxy mesh and
	// could leave the hierarchy blank after switching back. Mode 03 suppresses only
	// the custom GS render pass in FLCCCinematicGSModule instead.
	GaussianSplatComponent->SetVisibility(true, false);
	ProxyShadowReceiverComponent->SetVisibility(bMeshDebugMode || bAnyProxyCaptureVisibility, true);
	ProxyShadowReceiverComponent->SetHiddenInGame(!bMeshDebugMode && !bAnyProxyCaptureVisibility);
	ProxyShadowReceiverComponent->SetVisibleInSceneCaptureOnly(!bMeshDebugMode && bAnyProxyCaptureVisibility);
	ProxyShadowReceiverComponent->SetHiddenInSceneCapture(!bMeshDebugMode && !bAnyProxyCaptureVisibility);
	// SceneCapture2D requires the primitive to remain eligible for the main/depth pass;
	// VisibleInSceneCaptureOnly prevents it from leaking into the editor/game camera.
	ProxyShadowReceiverComponent->SetRenderInMainPass(bMeshDebugMode || bAnyProxyCaptureVisibility);
	ProxyShadowReceiverComponent->SetRenderInDepthPass(bMeshDebugMode || bAnyProxyCaptureVisibility);
	ProxyShadowReceiverComponent->SetCastShadow(bMeshDebugMode || bCaptureMaskMode || bStableLightDepthMode);
	ProxyShadowReceiverComponent->bCastHiddenShadow = false;
	ProxyShadowReceiverComponent->SetCollisionEnabled(
		bMeshDebugMode ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
	ProxyShadowReceiverComponent->SetCollisionResponseToAllChannels(
		bMeshDebugMode ? ECollisionResponse::ECR_Block : ECollisionResponse::ECR_Ignore);

	if (!bHasProxyMesh)
	{
		GaussianSplatComponent->SetVisibility(true, false);
		ProxyShadowReceiverComponent->SetStaticMesh(nullptr);
		return;
	}

	if (ProxyShadowReceiverComponent->GetStaticMesh() != GaussianSplatComponent->ProxyShadowMesh)
	{
		ProxyShadowReceiverComponent->SetStaticMesh(GaussianSplatComponent->ProxyShadowMesh);
	}
	ProxyShadowReceiverComponent->SetRelativeTransform(GaussianSplatComponent->GetProxyShadowMeshLocalTransform());

	const int32 LightDepthResolution = 2048; // v2774: 4096 diagnostic removed; keep bounded production memory.
	ProxyLightDepthTarget = ResolveProxyRenderTarget(
		this, ProxyLightDepthTarget, TEXT("ProxyLightDepthTarget"),
		ETextureRenderTargetFormat::RTF_R32f,
		FLinearColor(HALF_WORLD_MAX, HALF_WORLD_MAX, HALF_WORLD_MAX, 1.0f),
		LightDepthResolution, LightDepthResolution);
	if (!ProxyLightDepthTarget)
	{
		return;
	}
	if (ProxyLightDepthTarget->SizeX != LightDepthResolution || ProxyLightDepthTarget->SizeY != LightDepthResolution)
	{
		ProxyLightDepthTarget->ResizeTarget(LightDepthResolution, LightDepthResolution);
	}
	ProxyReceiverLightDepthTarget = ResolveProxyRenderTarget(
		this, ProxyReceiverLightDepthTarget, TEXT("ProxyReceiverLightDepthTarget"),
		ETextureRenderTargetFormat::RTF_R32f,
		FLinearColor(HALF_WORLD_MAX, HALF_WORLD_MAX, HALF_WORLD_MAX, 1.0f),
		LightDepthResolution, LightDepthResolution);
	if (!ProxyReceiverLightDepthTarget)
	{
		return;
	}
	if (ProxyReceiverLightDepthTarget->SizeX != LightDepthResolution || ProxyReceiverLightDepthTarget->SizeY != LightDepthResolution)
	{
		ProxyReceiverLightDepthTarget->ResizeTarget(LightDepthResolution, LightDepthResolution);
	}
	ProxySurfaceDepthTarget = ResolveProxyRenderTarget(
		this, ProxySurfaceDepthTarget, TEXT("ProxySurfaceDepthTarget"),
		ETextureRenderTargetFormat::RTF_R32f,
		FLinearColor(HALF_WORLD_MAX, HALF_WORLD_MAX, HALF_WORLD_MAX, 1.0f),
		16, 16);
	if (!ProxySurfaceDepthTarget)
	{
		return;
	}
	ProxySurfaceMaterialTarget = ResolveProxyRenderTarget(
		this, ProxySurfaceMaterialTarget, TEXT("ProxySurfaceMaterialTarget"),
		ETextureRenderTargetFormat::RTF_RGBA16f,
		FLinearColor(0.0f, 0.0f, 0.0f, 0.0f),
		16, 16);
	if (!ProxySurfaceMaterialTarget)
	{
		return;
	}
	ProxyLightDepthCapture->TextureTarget = ProxyLightDepthTarget;
	ProxyReceiverLightDepthCapture->TextureTarget = ProxyReceiverLightDepthTarget;
	ProxySurfaceDepthCapture->TextureTarget = ProxySurfaceDepthTarget;
	ProxySurfaceMaterialCapture->TextureTarget = ProxySurfaceMaterialTarget;
	ProxyLightDepthCapture->ShowOnlyComponents.Empty();
	ProxyReceiverLightDepthCapture->ShowOnlyComponents.Empty();
	ProxySurfaceDepthCapture->ShowOnlyComponents.Empty();
	ProxySurfaceMaterialCapture->ShowOnlyComponents.Empty();
	// Keep external blockers and the proxy receiver in separate depth targets. Mixing
	// the receiver into the blocker target made both textures identical when no
	// external caster was configured and forced self-shadowing to classify against a
	// single, unstable front-most texel.
	ProxyReceiverLightDepthCapture->ShowOnlyComponent(ProxyShadowReceiverComponent);
	ProxySurfaceDepthCapture->ShowOnlyComponent(ProxyShadowReceiverComponent);
	ProxySurfaceMaterialCapture->ShowOnlyComponent(ProxyShadowReceiverComponent);
	// Original capture pairing: external casters are visible in both captures;
	// the unshadowed capture disables dynamic shadows and AO.
	for (AActor* CasterActor : GaussianSplatComponent->ExternalShadowCasters)
	{
		if (!IsValid(CasterActor) || CasterActor == this || CasterActor->GetWorld() != GetWorld())
		{
			continue;
		}
		TInlineComponentArray<UPrimitiveComponent*> CasterPrimitives(CasterActor);
		for (UPrimitiveComponent* Primitive : CasterPrimitives)
		{
			if (!IsValid(Primitive) || !Primitive->IsRegistered() || !Primitive->CastShadow)
			{
				continue;
			}
			ProxyLightDepthCapture->ShowOnlyComponent(Primitive);
		}
	}

	// Mode 03 is a visual reference for the actual proxy asset. Do not replace all
	// of its material slots with the receiver/default material; doing so turned the
	// entire mesh into an almost-black silhouette in a lit viewport. Overrides from
	// the production receiver path can persist on the component, so clear them when
	// entering the explicit proxy-only presentation.
	if (bStableProxyMeshOnlyPresentationActive)
	{
		ProxyShadowReceiverComponent->EmptyOverrideMaterials();
	}
	else
	{
		UMaterialInterface* ReferenceMaterial = GaussianSplatComponent->ProxyShadowReceiverMaterial
			? GaussianSplatComponent->ProxyShadowReceiverMaterial.Get()
			: UMaterial::GetDefaultMaterial(MD_Surface);
		if (ReferenceMaterial)
		{
			const int32 MaterialCount = FMath::Max(1, ProxyShadowReceiverComponent->GetNumMaterials());
			for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
			{
				ProxyShadowReceiverComponent->SetMaterial(MaterialIndex, ReferenceMaterial);
			}
		}
	}
	// Layer entries are legacy camera-overlay inputs. Direct Rain/Snow must keep the
	// receiver on its neutral/reference material so translucent or masked weather
	// materials cannot change the Stable LightDepth geometry used by self-cast.
	const bool bAllowLegacySurfaceLayerMaterials =
		GaussianSplatComponent->ProxySurfaceEffectMode == ELCCCinematicGSProxySurfaceEffectMode::Disabled;
	if (bAllowLegacySurfaceLayerMaterials)
	{
		for (int32 LayerIndex = 0; LayerIndex < GaussianSplatComponent->ProxySurfaceLayers.Num(); ++LayerIndex)
		{
			const FLCCCinematicGSProxySurfaceLayer& Layer = GaussianSplatComponent->ProxySurfaceLayers[LayerIndex];
			if (Layer.bEnabled && Layer.Material)
			{
				ProxyShadowReceiverComponent->SetMaterial(LayerIndex, Layer.Material);
			}
		}
	}
}
