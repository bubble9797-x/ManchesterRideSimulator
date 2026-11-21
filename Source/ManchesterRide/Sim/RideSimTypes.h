// Manchester Riding Simulator - engine-independent riding simulation types.
//
// Everything in /Sim is plain C++17 with no Unreal dependencies so it can be
// unit-tested outside the editor (see Tests/SimTests) and run identically on
// server and client. Units are SI: metres, seconds, kilograms, newtons, radians.
#pragma once

#include <cstdint>

namespace MRSim
{
	constexpr float Gravity = 9.81f;
	constexpr float AirDensity = 1.225f;
	constexpr float Pi = 3.14159265358979f;

	enum class EVehicleCategory : uint8_t
	{
		EScooter,
		EBike,
		BMX,
		MountainBike,
		RoadBike,
		DownhillBike,
		DirtBike,
		Motorcycle,
	};

	enum class EPowertrain : uint8_t
	{
		Pedal,          // human power only
		PedalAssist,    // UK EAPC style: motor multiplies rider power, cut-off speed
		ElectricThrottle, // e-scooter: hub motor on a throttle
		Combustion,     // geared engine with clutch
	};

	// Matches the physical surface table configured in DefaultEngine.ini
	// (SurfaceType1..8). Keep the order in sync with Config/DefaultEngine.ini.
	enum class ESurface : uint8_t
	{
		Asphalt = 0,
		Concrete,
		Gravel,
		Grass,
		Mud,
		Dirt,
		ForestTrail,
		Cobbles,
		Count
	};

	enum class ETyre : uint8_t
	{
		Road,       // slick/semi-slick: great on tarmac, poor off-road
		Hybrid,     // commuter / scooter
		Knobbly,    // MTB / BMX park
		Motocross,  // full knobbly MX/enduro
		Sport,      // motorcycle road tyre
		Count
	};

	enum class EWheelieAssist : uint8_t
	{
		Casual,     // strong balance hold, cannot loop out
		Balanced,   // partial assistance, loop-out possible when abused
		Realistic,  // no assistance
	};

	struct FSuspensionParams
	{
		float RestLength = 0.35f;   // hub mount -> ground distance at zero compression (m), includes wheel radius
		float Travel = 0.10f;       // usable travel (m)
		float Stiffness = 30000.f;  // N/m
		float CompressionDamping = 2500.f; // N/(m/s)
		float ReboundDamping = 3000.f;     // N/(m/s)
	};

	struct FPowertrainParams
	{
		EPowertrain Type = EPowertrain::Pedal;

		// Human power (pedal and pedal assist)
		float RiderSustainedPowerW = 250.f;
		float RiderPeakForceN = 450.f;     // force cap at very low speed (gearing)

		// Electric
		float MotorPeakPowerW = 500.f;
		float MotorPeakForceN = 120.f;
		float MotorCutoffSpeed = 6.94f;    // m/s (25 km/h UK EAPC limit)
		float BatteryWh = 500.f;
		float MotorEfficiency = 0.85f;
		float RegenFraction = 0.0f;        // fraction of braking energy recovered
		float AssistLevels[4] = { 0.5f, 1.0f, 2.0f, 3.0f }; // eco / tour / sport / turbo

		// Combustion
		float PeakTorqueNm = 20.f;
		float PeakTorqueRpm = 7000.f;
		float IdleRpm = 1600.f;
		float RedlineRpm = 10500.f;
		int   NumGears = 5;
		float GearRatios[6] = { 3.0f, 2.1f, 1.6f, 1.3f, 1.1f, 0.95f };
		float FinalDrive = 3.8f;
		float EngineInertia = 0.06f;       // kg m^2, drives clutch-dump wheelies
		float FuelLitres = 7.0f;
		float BsfcGPerKWh = 320.f;         // brake specific fuel consumption
	};

	struct FWheelieParams
	{
		float RiderLiftTorque = 260.f;      // N*m from weight shift/bar pull when the wheelie button is pressed
		float RiderBalanceTorque = 160.f;   // N*m available to the lean-forward/back axis
		float LoopOutMargin = 0.35f;        // rad past balance point before a loop-out
		float AssistStrength = 1.0f;        // per-vehicle multiplier for the assist controller
		float BunnyHopSpeed = 0.f;          // m/s vertical impulse (0 = cannot hop)
		bool  bCanStoppie = true;
	};

	struct FVehicleParams
	{
		EVehicleCategory Category = EVehicleCategory::EBike;
		ETyre Tyre = ETyre::Hybrid;

		float Mass = 100.f;            // vehicle + rider (kg)
		float Wheelbase = 1.15f;       // m
		float ComHeight = 1.05f;       // m above ground (vehicle+rider)
		float ComFromRear = 0.50f;     // horizontal distance rear contact -> COM (m)
		float PitchRadiusOfGyration = 0.45f; // m, about COM
		float WheelRadiusFront = 0.36f;
		float WheelRadiusRear = 0.36f;

		float MaxSteerAngle = 0.55f;   // rad at walking pace
		float SteerSpeedRef = 7.f;     // m/s at which steering authority has halved
		float LeanResponse = 6.f;      // 1/s, how quickly the rider reaches equilibrium lean
		float MaxLean = 0.85f;         // rad

		float TyreGrip = 1.0f;         // multiplier on surface friction
		float RollingResistance = 0.008f;
		float CdA = 0.55f;             // drag area (m^2)
		float MaxBrakeForceFront = 900.f;
		float MaxBrakeForceRear = 500.f;
		float TopSpeed = 12.f;         // m/s hard governor (scooters etc.)

		FSuspensionParams Front;
		FSuspensionParams Rear;
		FPowertrainParams Powertrain;
		FWheelieParams Wheelie;
	};

	// Per-frame rider input, already filtered by the input layer. All axes 0..1 or -1..1.
	struct FRiderInput
	{
		float Throttle = 0.f;      // 0..1  (pedal effort on bicycles)
		float FrontBrake = 0.f;    // 0..1
		float RearBrake = 0.f;     // 0..1
		float Reverse = 0.f;       // 0..1 back up at walking pace (only when stopped; see MRPlayerController)
		float Steer = 0.f;         // -1..1 (right positive)
		float Balance = 0.f;       // -1..1 (+ = lean back / nose up, - = lean forward)
		float Clutch = 0.f;        // 0..1 pulled (combustion only; ignored if auto clutch)
		bool  bWheelie = false;    // dedicated wheelie button held
		bool  bHop = false;        // bunny hop button held (fires on release)
		int   GearRequest = 0;     // -1 down, +1 up, 0 none (edge-triggered by caller)
		int   AssistLevel = 2;     // e-bike assist index 0..3
	};

	struct FWheelContact
	{
		bool  bHit = false;
		float Distance = 0.f;     // distance from hub mount point down to ground (m)
		float NormalZ = 1.f;      // world up component of the ground normal
		ESurface Surface = ESurface::Asphalt;
	};

	struct FEnvironment
	{
		float Wetness = 0.f;      // 0 dry .. 1 standing water
		// rad, +ve = uphill along the heading. With both wheels down: pitch of the line
		// through the two contact points. With one wheel down: from that hit's normal.
		float SlopePitch = 0.f;
	};

	struct FAssistSettings
	{
		EWheelieAssist WheelieAssist = EWheelieAssist::Casual;
		bool  bAutoClutch = true;
		bool  bAutoShift = false;
		bool  bLandingAssist = true;
		bool  bABS = true;           // prevents front-wheel lock under hard braking
		float SteerSensitivity = 1.f;
	};

	// Full simulation state. Only a compact subset is replicated (see MRNetTypes.h).
	struct FRideState
	{
		float Speed = 0.f;          // forward speed along heading (m/s)
		float LongAccel = 0.f;      // last longitudinal acceleration (m/s^2), drives load transfer
		float LateralSpeed = 0.f;   // sideways slide speed (m/s, right positive)
		float VerticalSpeed = 0.f;  // m/s
		float YawRate = 0.f;        // rad/s
		float Steer = 0.f;          // current steer angle (rad)
		float Lean = 0.f;           // roll (rad, right positive)
		float Pitch = 0.f;          // wheelie (+) / stoppie (-) pitch relative to ground (rad)
		float PitchRate = 0.f;
		float GroundPitch = 0.f;    // slope the body is aligned to (held while airborne)
		float AirPitch = 0.f;       // accumulated flip rotation while airborne (scoring)
		float AirYaw = 0.f;         // visual spin while airborne (heading is kept)

		float CompressionFront = 0.f;
		float CompressionRear = 0.f;
		float LoadFront = 0.f;      // N
		float LoadRear = 0.f;

		bool  bFrontGrounded = false;
		bool  bRearGrounded = false;
		bool  bAirborne = false;
		bool  bCrashed = false;
		bool  bRearSliding = false;
		bool  bFrontLocked = false;
		bool  bHopPrimed = false;
		float AirTime = 0.f;
		float CrashTimer = 0.f;

		// Powertrain
		float BatteryWh = 0.f;
		float FuelLitres = 0.f;
		float EngineRpm = 0.f;
		int   Gear = 1;
		float ClutchEngage = 1.f;   // 0 = pulled in, 1 = fully engaged
		float DriveForce = 0.f;     // last applied (N), for audio/FX
		float MotorPowerW = 0.f;
	};

	struct FStepResult
	{
		// Motion in the bike's local frame for this step (m / rad).
		float Forward = 0.f;
		float Right = 0.f;
		float Up = 0.f;
		float Yaw = 0.f;
		bool  bLanded = false;
		bool  bCrashedThisStep = false;
		float LandingImpactSpeed = 0.f;
	};
}
