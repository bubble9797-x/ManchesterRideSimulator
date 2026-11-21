#pragma once

#include "CoreMinimal.h"
#include "Components/SynthComponent.h"
#include <atomic>
#include "MRRideAudioComponent.generated.h"

/**
 * Procedural vehicle audio synthesised on the audio thread, so the project needs
 * no sound assets to be playable:
 *  - combustion: firing-frequency harmonics + exhaust noise driven by rpm/throttle
 *  - electric: motor whine pitched by speed, scaled by motor power
 *  - all: tyre roll noise (surface dependent), wind, skid hiss, suspension thumps
 * Recorded samples can replace these later (see TODO.md "Audio").
 */
UCLASS(ClassGroup = (Audio), meta = (BlueprintSpawnableComponent))
class MANCHESTERRIDE_API UMRRideAudioComponent : public USynthComponent
{
	GENERATED_BODY()

public:
	UMRRideAudioComponent(const FObjectInitializer& ObjectInitializer);

	enum class EVoice : uint8 { Pedal, Electric, Combustion };

	// Game thread setters (lock-free handoff to the audio thread).
	void SetVoice(EVoice InVoice, int32 InCylinders);
	void SetDriveState(float Rpm, float Throttle, float MotorPowerW, float SpeedMps);
	void SetTyreState(float Roughness, float SkidAmount, bool bGrounded);
	void TriggerThump(float Strength);

protected:
	virtual bool Init(int32& SampleRate) override;
	virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override;

private:
	std::atomic<uint8> Voice{ 0 };
	std::atomic<int32> Cylinders{ 1 };
	std::atomic<float> TargetRpm{ 0.f };
	std::atomic<float> TargetThrottle{ 0.f };
	std::atomic<float> TargetMotorPower{ 0.f };
	std::atomic<float> TargetSpeed{ 0.f };
	std::atomic<float> TargetRoughness{ 0.f };
	std::atomic<float> TargetSkid{ 0.f };
	std::atomic<bool> bTargetGrounded{ true };
	std::atomic<float> PendingThump{ 0.f };

	// Audio-thread state
	float Rate = 48000.f;
	float Rpm = 0.f, Throttle = 0.f, MotorPower = 0.f, Speed = 0.f, Roughness = 0.f, Skid = 0.f, Grounded = 1.f;
	float Phase = 0.f, WhinePhase = 0.f, Thump = 0.f, ThumpPhase = 0.f;
	float NoiseLP1 = 0.f, NoiseLP2 = 0.f, NoiseLP3 = 0.f;
	uint32 Seed = 22222u;
};
