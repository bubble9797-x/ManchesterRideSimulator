#include "Audio/MRRideAudioComponent.h"

UMRRideAudioComponent::UMRRideAudioComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	NumChannels = 1;
	bAutoActivate = true;
	bAllowSpatialization = true;
}

void UMRRideAudioComponent::SetVoice(EVoice InVoice, int32 InCylinders)
{
	Voice.store(static_cast<uint8>(InVoice));
	Cylinders.store(FMath::Max(1, InCylinders));
}

void UMRRideAudioComponent::SetDriveState(float InRpm, float InThrottle, float InMotorPowerW, float InSpeed)
{
	TargetRpm.store(InRpm);
	TargetThrottle.store(InThrottle);
	TargetMotorPower.store(InMotorPowerW);
	TargetSpeed.store(InSpeed);
}

void UMRRideAudioComponent::SetTyreState(float InRoughness, float InSkid, bool bInGrounded)
{
	TargetRoughness.store(InRoughness);
	TargetSkid.store(InSkid);
	bTargetGrounded.store(bInGrounded);
}

void UMRRideAudioComponent::TriggerThump(float Strength)
{
	PendingThump.store(FMath::Clamp(Strength, 0.f, 1.f));
}

bool UMRRideAudioComponent::Init(int32& SampleRate)
{
	NumChannels = 1;
	Rate = static_cast<float>(SampleRate);
	return true;
}

int32 UMRRideAudioComponent::OnGenerateAudio(float* OutAudio, int32 NumSamples)
{
	const EVoice V = static_cast<EVoice>(Voice.load());
	const float Cyl = static_cast<float>(Cylinders.load());
	const float TRpm = TargetRpm.load(), TThr = TargetThrottle.load(), TPow = TargetMotorPower.load();
	const float TSpd = TargetSpeed.load(), TRough = TargetRoughness.load(), TSkid = TargetSkid.load();
	const float TGround = bTargetGrounded.load() ? 1.f : 0.f;
	const float NewThump = PendingThump.exchange(0.f);
	if (NewThump > 0.f) { Thump = FMath::Max(Thump, NewThump); ThumpPhase = 0.f; }

	const float Smooth = 1.f - FMath::Exp(-1.f / (0.03f * Rate)); // ~30 ms parameter smoothing
	const float Dt = 1.f / Rate;

	for (int32 i = 0; i < NumSamples; ++i)
	{
		Rpm += (TRpm - Rpm) * Smooth;
		Throttle += (TThr - Throttle) * Smooth;
		MotorPower += (TPow - MotorPower) * Smooth;
		Speed += (TSpd - Speed) * Smooth;
		Roughness += (TRough - Roughness) * Smooth;
		Skid += (TSkid - Skid) * Smooth;
		Grounded += (TGround - Grounded) * Smooth;

		// White noise
		Seed = Seed * 1664525u + 1013904223u;
		const float White = static_cast<float>(Seed >> 9) / 4194304.f - 1.f;
		NoiseLP1 += (White - NoiseLP1) * 0.02f;   // rumble
		NoiseLP2 += (White - NoiseLP2) * 0.15f;   // hiss
		NoiseLP3 += (White - NoiseLP3) * 0.005f;  // wind

		float S = 0.f;
		if (V == EVoice::Combustion)
		{
			// Firing frequency of a four-stroke: rpm/60 * cylinders/2
			const float Fire = FMath::Max(Rpm, 300.f) / 60.f * Cyl * 0.5f;
			Phase += Fire * Dt;
			Phase -= FMath::FloorToFloat(Phase);
			const float P = Phase * 2.f * PI;
			const float Load = 0.35f + 0.65f * Throttle;
			// Pulse-rich exhaust: fundamental + harmonics + throttle-scaled noise burst per firing
			const float Pulse = FMath::Sin(P) + 0.55f * FMath::Sin(2.f * P + 0.3f) + 0.3f * FMath::Sin(3.f * P + 1.1f) + 0.15f * FMath::Sin(5.f * P);
			const float Burst = (Phase < 0.25f ? 1.f : 0.25f) * NoiseLP2 * Throttle;
			S += (Pulse * 0.16f + Burst * 0.35f + NoiseLP1 * 0.25f) * Load;
		}
		else if (V == EVoice::Electric)
		{
			const float WhineHz = 120.f + Speed * 95.f;
			WhinePhase += WhineHz * Dt;
			WhinePhase -= FMath::FloorToFloat(WhinePhase);
			const float Amt = FMath::Clamp(MotorPower / 600.f, 0.f, 1.f);
			S += (FMath::Sin(WhinePhase * 2.f * PI) + 0.3f * FMath::Sin(WhinePhase * 6.f * PI)) * 0.05f * (0.15f + Amt);
		}
		else
		{
			// Freewheel ticking for pedal bikes when coasting
			const float TickHz = Speed * 9.f;
			WhinePhase += TickHz * Dt;
			if (WhinePhase >= 1.f) { WhinePhase -= 1.f; }
			S += (WhinePhase < 0.02f ? 0.04f : 0.f) * (1.f - Throttle) * FMath::Clamp(Speed / 3.f, 0.f, 1.f);
		}

		// Tyres: louder on rough surfaces; skids add hiss
		const float SpeedN = FMath::Clamp(Speed / 25.f, 0.f, 1.f);
		S += Grounded * (NoiseLP1 * (0.08f + 0.25f * Roughness) * SpeedN + NoiseLP2 * Skid * 0.35f);
		// Wind rises with the square of speed
		S += NoiseLP3 * 6.f * SpeedN * SpeedN * 0.12f;
		// Suspension / landing thump: decaying 55 Hz sine
		if (Thump > 0.001f)
		{
			ThumpPhase += Dt;
			S += FMath::Sin(ThumpPhase * 55.f * 2.f * PI) * Thump * 0.6f;
			Thump *= 1.f - 12.f * Dt;
		}

		OutAudio[i] = FMath::Clamp(S, -1.f, 1.f);
	}
	return NumSamples;
}
