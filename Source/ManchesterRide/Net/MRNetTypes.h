#pragma once

#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"
#include "RideSimTypes.h"
#include "MRNetTypes.generated.h"

// Bit flags packed into FMRNetRideState::Flags (kept outside the USTRUCT for UHT).
namespace EMRNetFlags
{
	enum : uint8
	{
		Airborne = 1 << 0,
		Crashed = 1 << 1,
		FrontGrounded = 1 << 2,
		RearGrounded = 1 << 3,
		Sliding = 1 << 4,
		OffRoad = 1 << 5, // rider stance: standing on the pegs off-road
	};
}

/**
 * Compact ride state sent owner -> server (unreliable RPC, ~30 Hz) and
 * server -> other clients (replicated property, skip owner).
 * About 32 bytes per update. We deliberately do NOT replicate every physics
 * quantity: remote riders only need pose, speed and a few visual flags.
 */
USTRUCT()
struct FMRNetRideState
{
	GENERATED_BODY()

	UPROPERTY() FVector_NetQuantize10 Location = FVector::ZeroVector; // actor location (cm, 0.1 cm precision)
	UPROPERTY() uint16 Yaw = 0;          // 0..65535 -> 0..360 deg
	UPROPERTY() int16 GroundPitch = 0;   // rad * 5000
	UPROPERTY() int16 Pitch = 0;         // rad * 5000 (wheelie +, stoppie -)
	UPROPERTY() int16 Lean = 0;          // rad * 5000
	UPROPERTY() int16 AirYaw = 0;        // rad * 2000 (visual spin)
	UPROPERTY() int16 Speed = 0;         // cm/s
	UPROPERTY() int16 VerticalSpeed = 0; // cm/s
	UPROPERTY() int8 Steer = 0;          // rad * 100
	UPROPERTY() uint8 CompFront = 0;     // compression, 1/255 of 1.5 x travel
	UPROPERTY() uint8 CompRear = 0;
	UPROPERTY() uint8 RpmDiv64 = 0;      // engine rpm / 64 (audio)
	UPROPERTY() uint8 Flags = 0;         // EMRNetFlags bits
	UPROPERTY() float Timestamp = 0.f;   // sender's world time (s)


	static int16 PackAngle(float Rad, float Scale) { return static_cast<int16>(FMath::Clamp(FMath::RoundToInt(Rad * Scale), -32767, 32767)); }
	static float UnpackAngle(int16 V, float Scale) { return static_cast<float>(V) / Scale; }

	float GetYawDegrees() const { return static_cast<float>(Yaw) * (360.f / 65536.f); }
	void SetYawDegrees(float Deg) { Yaw = static_cast<uint16>(FMath::RoundToInt(FRotator::ClampAxis(Deg) * (65536.f / 360.f)) & 0xFFFF); }
};

/** Last client-reported values the server keeps per rider for validation. */
struct FMRServerMoveCheck
{
	FVector LastLocation = FVector::ZeroVector;
	float LastTime = -1.f;
	int32 Rejections = 0;
};
