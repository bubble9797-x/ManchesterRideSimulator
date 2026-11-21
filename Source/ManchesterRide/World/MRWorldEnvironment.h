#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MRWorldEnvironment.generated.h"

class UDirectionalLightComponent;
class USkyAtmosphereComponent;
class USkyLightComponent;
class UExponentialHeightFogComponent;
class UVolumetricCloudComponent;
class ULocalLightComponent;

UENUM(BlueprintType)
enum class EMRWeather : uint8
{
	Clear,
	Cloudy,
	Overcast,
	LightRain,
	HeavyRain,
	Fog,
};

/**
 * Replicated time-of-day and weather. The server owns the clock and weather
 * state; every machine drives its own sun/sky/fog from the replicated values and
 * integrates road wetness locally (it only depends on replicated weather).
 * Wetness feeds the riding simulation's tyre grip (MRSim::SurfaceGrip).
 */
UCLASS()
class MANCHESTERRIDE_API AMRWorldEnvironment : public AActor
{
	GENERATED_BODY()

public:
	AMRWorldEnvironment();

	static AMRWorldEnvironment* Get(const UObject* WorldContext);

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintPure) float GetTimeOfDayHours() const { return TimeOfDayHours; }
	UFUNCTION(BlueprintPure) EMRWeather GetWeather() const { return Weather; }
	UFUNCTION(BlueprintPure) float GetRoadWetness() const { return Wetness; }
	UFUNCTION(BlueprintPure) bool IsNight() const;
	static FText WeatherName(EMRWeather W);

	/** Server only. */
	void SetTimeOfDay(float Hours);
	void SetWeather(EMRWeather NewWeather, bool bLockWeather);
	void SetTimeScale(float GameMinutesPerRealSecond) { TimeScale = GameMinutesPerRealSecond; }

	/** Street lights etc. register here so they switch on at dusk. */
	void RegisterNightLight(ULocalLightComponent* Light, float OnIntensity);

protected:
	UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> Root;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UDirectionalLightComponent> Sun;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UDirectionalLightComponent> Moon;
	UPROPERTY(VisibleAnywhere) TObjectPtr<class UPostProcessComponent> Post;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USkyAtmosphereComponent> Atmosphere;
	UPROPERTY(VisibleAnywhere) TObjectPtr<USkyLightComponent> SkyLight;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UExponentialHeightFogComponent> Fog;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UVolumetricCloudComponent> Clouds;

	UPROPERTY(Replicated) float TimeOfDayHours = 10.f;
	UPROPERTY(Replicated) float TimeScale = 1.f; // game minutes per real second (1 = a day lasts 24 real minutes)
	UPROPERTY(Replicated) EMRWeather Weather = EMRWeather::Cloudy;
	UPROPERTY(Replicated) bool bWeatherLocked = false;

private:
	void ApplyLighting(float DeltaSeconds);
	void ServerUpdateWeather(float DeltaSeconds);

	float Wetness = 0.f;
	float FogDensity = 0.02f, SunScale = 1.f, CloudCover = 0.4f;
	float WeatherTimer = 0.f;
	float SkyRecaptureTimer = 0.f;

	struct FNightLight { TWeakObjectPtr<ULocalLightComponent> Light; float Intensity = 0.f; };
	TArray<FNightLight> NightLights;
	bool bLightsOn = false;
};
