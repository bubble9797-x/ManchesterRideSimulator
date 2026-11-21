#include "World/MRWorldEnvironment.h"
#include "World/MRTestEnvironment.h"

#include "Components/PointLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"

namespace
{
	struct FWeatherLook
	{
		float FogDensity;
		float SunScale;
		float CloudCover;
		float WetRate; // per second toward 1 (0 = drying)
	};

	FWeatherLook LookFor(EMRWeather W)
	{
		switch (W)
		{
		case EMRWeather::Clear:     return { 0.008f, 1.00f, 0.10f, 0.f };
		case EMRWeather::Cloudy:    return { 0.015f, 0.65f, 0.55f, 0.f };
		case EMRWeather::Overcast:  return { 0.025f, 0.35f, 0.90f, 0.f };
		case EMRWeather::LightRain: return { 0.040f, 0.25f, 0.95f, 1.f / 150.f };
		case EMRWeather::HeavyRain: return { 0.070f, 0.15f, 1.00f, 1.f / 45.f };
		case EMRWeather::Fog:       return { 0.300f, 0.30f, 0.80f, 1.f / 900.f };
		}
		return { 0.015f, 1.f, 0.5f, 0.f };
	}
}

AMRWorldEnvironment::AMRWorldEnvironment()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(2.f);

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	Sun = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("Sun"));
	Sun->SetupAttachment(Root);
	Sun->SetMobility(EComponentMobility::Movable);
	Sun->SetIntensity(10.f);
	Sun->SetDynamicShadowCascades(3);
	Sun->bUseTemperature = true;
	Sun->SetTemperature(5900.f);

	// Moonlight / urban sky glow: a dim, cool second light so night streets aren't pitch black.
	Moon = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("Moon"));
	Moon->SetupAttachment(Root);
	Moon->SetMobility(EComponentMobility::Movable);
	Moon->SetIntensity(0.f);
	Moon->SetLightColor(FLinearColor(0.62f, 0.72f, 1.f));
	Moon->SetCastShadows(false);
	Moon->SetAtmosphereSunLight(false); // as an atmosphere light it turns the night sky daytime blue
	Moon->SetRelativeRotation(FRotator(-52.f, 210.f, 0.f));

	// Eye adaptation floor: without it auto-exposure brightens a night street to near-daylight.
	Post = CreateDefaultSubobject<UPostProcessComponent>(TEXT("Post"));
	Post->SetupAttachment(Root);
	Post->bUnbound = true;
	Post->Settings.bOverride_AutoExposureMinBrightness = true;
	Post->Settings.AutoExposureMinBrightness = 0.5f;  // EV100 (extended luminance range is on)
	Post->Settings.bOverride_AutoExposureMaxBrightness = true;
	Post->Settings.AutoExposureMaxBrightness = 16.f;

	// Camera-like finish, so the image reads as filmed rather than rendered: a slightly muted grade
	// (UK daylight is rarely saturated), soft highlights, lens vignette and fringing, fine sensor grain,
	// motion blur, and Lumen at higher quality for softer bounce light and sharper reflections.
	FPostProcessSettings& PP = Post->Settings;
	PP.bOverride_ColorSaturation = true;   PP.ColorSaturation = FVector4(0.9f, 0.9f, 0.9f, 1.f);
	PP.bOverride_ColorContrast = true;     PP.ColorContrast = FVector4(1.06f, 1.06f, 1.06f, 1.f);
	PP.bOverride_ColorGammaShadows = true; PP.ColorGammaShadows = FVector4(1.f, 1.f, 1.02f, 1.f); // cool shadows
	PP.bOverride_ColorGainHighlights = true; PP.ColorGainHighlights = FVector4(1.02f, 1.f, 0.97f, 1.f); // warm highlights
	PP.bOverride_FilmShoulder = true;      PP.FilmShoulder = 0.3f;    // gentler highlight roll-off
	PP.bOverride_FilmToe = true;           PP.FilmToe = 0.5f;
	PP.bOverride_BloomIntensity = true;    PP.BloomIntensity = 0.45f;
	PP.bOverride_BloomThreshold = true;    PP.BloomThreshold = 1.2f;
	PP.bOverride_VignetteIntensity = true; PP.VignetteIntensity = 0.32f;
	PP.bOverride_SceneFringeIntensity = true; PP.SceneFringeIntensity = 0.25f;
	PP.bOverride_FilmGrainIntensity = true; PP.FilmGrainIntensity = 0.08f;
	PP.bOverride_MotionBlurAmount = true;  PP.MotionBlurAmount = 0.35f;
	PP.bOverride_MotionBlurMax = true;     PP.MotionBlurMax = 3.f;
	PP.bOverride_AmbientOcclusionIntensity = true; PP.AmbientOcclusionIntensity = 0.6f;
	PP.bOverride_LumenSceneLightingQuality = true; PP.LumenSceneLightingQuality = 2.f;
	PP.bOverride_LumenFinalGatherQuality = true;   PP.LumenFinalGatherQuality = 1.5f;
	PP.bOverride_LumenReflectionQuality = true;    PP.LumenReflectionQuality = 1.5f;

	Atmosphere = CreateDefaultSubobject<USkyAtmosphereComponent>(TEXT("Atmosphere"));
	Atmosphere->SetupAttachment(Root);

	SkyLight = CreateDefaultSubobject<USkyLightComponent>(TEXT("SkyLight"));
	SkyLight->SetupAttachment(Root);
	SkyLight->SetMobility(EComponentMobility::Movable);
	SkyLight->bRealTimeCapture = true;
	SkyLight->SetIntensity(1.f);

	Fog = CreateDefaultSubobject<UExponentialHeightFogComponent>(TEXT("Fog"));
	Fog->SetupAttachment(Root);
	Fog->SetFogDensity(0.015f);
	Fog->SetVolumetricFog(false); // enabled on High/Ultra via scalability (r.VolumetricFog)

	Clouds = CreateDefaultSubobject<UVolumetricCloudComponent>(TEXT("Clouds"));
	Clouds->SetupAttachment(Root);
}

AMRWorldEnvironment* AMRWorldEnvironment::Get(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	if (!World) return nullptr;
	for (TActorIterator<AMRWorldEnvironment> It(World); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

void AMRWorldEnvironment::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AMRWorldEnvironment, TimeOfDayHours);
	DOREPLIFETIME(AMRWorldEnvironment, TimeScale);
	DOREPLIFETIME(AMRWorldEnvironment, Weather);
	DOREPLIFETIME(AMRWorldEnvironment, bWeatherLocked);
}

void AMRWorldEnvironment::BeginPlay()
{
	Super::BeginPlay();
	Sun->SetAtmosphereSunLight(true);
	// Engine sample cloud material (ships with UE5). Optional: clouds are skipped if it is missing.
	if (UMaterialInterface* CloudMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst.m_SimpleVolumetricCloud_Inst")))
	{
		Clouds->SetMaterial(CloudMat);
	}
	else
	{
		Clouds->SetVisibility(false);
	}
	const FWeatherLook L = LookFor(Weather);
	FogDensity = L.FogDensity;
	SunScale = L.SunScale;
	CloudCover = L.CloudCover;

	for (TActorIterator<AMRTestEnvironment> It(GetWorld()); It; ++It)
	{
		for (UPointLightComponent* Light : It->TakePendingNightLights())
		{
			RegisterNightLight(Light, 8000.f);
		}
	}
	ApplyLighting(0.f);
}

bool AMRWorldEnvironment::IsNight() const
{
	return TimeOfDayHours < 7.0f || TimeOfDayHours > 17.5f;
}

FText AMRWorldEnvironment::WeatherName(EMRWeather W)
{
	switch (W)
	{
	case EMRWeather::Clear:     return NSLOCTEXT("MR", "Clear", "Clear");
	case EMRWeather::Cloudy:    return NSLOCTEXT("MR", "Cloudy", "Cloudy");
	case EMRWeather::Overcast:  return NSLOCTEXT("MR", "Overcast", "Overcast");
	case EMRWeather::LightRain: return NSLOCTEXT("MR", "LightRain", "Light rain");
	case EMRWeather::HeavyRain: return NSLOCTEXT("MR", "HeavyRain", "Heavy rain");
	case EMRWeather::Fog:       return NSLOCTEXT("MR", "Fog", "Fog");
	}
	return FText::GetEmpty();
}

void AMRWorldEnvironment::SetTimeOfDay(float Hours)
{
	TimeOfDayHours = FMath::Fmod(FMath::Max(0.f, Hours), 24.f);
}

void AMRWorldEnvironment::SetWeather(EMRWeather NewWeather, bool bLockWeather)
{
	Weather = NewWeather;
	bWeatherLocked = bLockWeather;
	WeatherTimer = 0.f;
}

void AMRWorldEnvironment::RegisterNightLight(ULocalLightComponent* Light, float OnIntensity)
{
	if (!Light) return;
	NightLights.Add({ Light, OnIntensity });
	Light->SetIntensity(bLightsOn ? OnIntensity : 0.f);
	Light->SetVisibility(bLightsOn);
}

void AMRWorldEnvironment::ServerUpdateWeather(float DeltaSeconds)
{
	TimeOfDayHours = FMath::Fmod(TimeOfDayHours + DeltaSeconds * TimeScale / 60.f, 24.f);
	if (bWeatherLocked) return;
	WeatherTimer += DeltaSeconds;
	if (WeatherTimer > 300.f) // re-roll every 5 real minutes; Manchester-appropriate odds
	{
		WeatherTimer = 0.f;
		const float R = FMath::FRand();
		Weather = R < 0.12f ? EMRWeather::Clear : R < 0.40f ? EMRWeather::Cloudy : R < 0.62f ? EMRWeather::Overcast
			: R < 0.84f ? EMRWeather::LightRain : R < 0.95f ? EMRWeather::HeavyRain : EMRWeather::Fog;
	}
}

void AMRWorldEnvironment::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (HasAuthority())
	{
		ServerUpdateWeather(DeltaSeconds);
	}
	else
	{
		// Clients advance the clock between (infrequent) replication updates.
		TimeOfDayHours = FMath::Fmod(TimeOfDayHours + DeltaSeconds * TimeScale / 60.f, 24.f);
	}

	const FWeatherLook L = LookFor(Weather);
	const float Blend = FMath::Clamp(DeltaSeconds * 0.2f, 0.f, 1.f); // ~5 s weather transitions
	FogDensity = FMath::Lerp(FogDensity, L.FogDensity, Blend);
	SunScale = FMath::Lerp(SunScale, L.SunScale, Blend);
	CloudCover = FMath::Lerp(CloudCover, L.CloudCover, Blend);
	if (L.WetRate > 0.f)
	{
		Wetness = FMath::Min(1.f, Wetness + L.WetRate * DeltaSeconds);
	}
	else
	{
		Wetness = FMath::Max(0.f, Wetness - DeltaSeconds / (SunScale > 0.6f ? 240.f : 600.f));
	}
	ApplyLighting(DeltaSeconds);
}

void AMRWorldEnvironment::ApplyLighting(float DeltaSeconds)
{
	// Simple solar model for 53.5 N: peak elevation ~60 deg (summer) - use 40 as a year-round average.
	const float DayPhase = (TimeOfDayHours - 6.f) / 12.f; // 0 at 06:00, 1 at 18:00
	const float ElevationDeg = FMath::Sin(DayPhase * PI) * 40.f;
	const float AzimuthDeg = 90.f + (TimeOfDayHours / 24.f) * 360.f;
	Sun->SetWorldRotation(FRotator(-ElevationDeg, AzimuthDeg, 0.f));

	const float DayFactor = FMath::Clamp((ElevationDeg + 6.f) / 16.f, 0.f, 1.f); // civil twilight fade
	Sun->SetIntensity(10.f * SunScale * DayFactor);
	Sun->SetTemperature(FMath::Lerp(3200.f, 5900.f, FMath::Clamp(ElevationDeg / 25.f, 0.f, 1.f)));
	SkyLight->SetIntensity(FMath::Lerp(0.15f, 1.0f, DayFactor) * FMath::Lerp(0.7f, 1.f, SunScale));
	Moon->SetIntensity(0.12f * (1.f - DayFactor) * FMath::Lerp(0.6f, 1.f, SunScale));
	Fog->SetFogDensity(FogDensity);

	const bool bWantLights = DayFactor < 0.35f || (SunScale < 0.2f && DayFactor < 0.6f);
	if (bWantLights != bLightsOn)
	{
		bLightsOn = bWantLights;
		for (const FNightLight& N : NightLights)
		{
			if (ULocalLightComponent* Light = N.Light.Get())
			{
				Light->SetIntensity(bLightsOn ? N.Intensity : 0.f);
				Light->SetVisibility(bLightsOn);
			}
		}
	}
}
