// Manchester Riding Simulator - single-track (two-wheeler) riding simulation.
//
// Model summary (see docs/ARCHITECTURE.md for the derivation):
//  * Ray-cast spring/damper suspension per wheel gives normal loads.
//  * Longitudinal: drive (pedal / motor / engine+clutch+gears) limited by rear
//    traction, brakes limited by per-wheel traction, drag, rolling resistance, slope.
//  * Pitch: rigid-body rotation about the rear contact (wheelie) or front contact
//    (stoppie) driven by inertial load transfer, gravity and rider weight shift,
//    with an optional balance-assist PD controller.
//  * Lateral: kinematic steering limited by a friction circle, equilibrium lean,
//    and a rear-slip model for controlled slides.
//  * Airborne: ballistic, with rider pitch/spin control and landing checks.
#pragma once

#include "RideSimTypes.h"

namespace MRSim
{
	// Friction coefficient for a tyre on a surface with the given wetness.
	float SurfaceGrip(ESurface Surface, ETyre Tyre, float Wetness);
	// Rolling resistance multiplier for a surface (asphalt = 1).
	float SurfaceRollingMultiplier(ESurface Surface);
	// Static balance-point angle for a wheelie (rad).
	float WheelieBalanceAngle(const FVehicleParams& P);

	class FRideSimulator
	{
	public:
		static void InitState(const FVehicleParams& P, FRideState& S);

		// Advance the simulation by Dt seconds (call with Dt <= 1/60; the Unreal
		// wrapper sub-steps at 120 Hz). Contacts are sampled by the caller at the
		// hub mount points using the current body orientation.
		static FStepResult Step(const FVehicleParams& P, const FAssistSettings& A,
			const FRiderInput& In, const FWheelContact& Front, const FWheelContact& Rear,
			const FEnvironment& Env, FRideState& S, float Dt);

		// Ground pitch the body is currently aligned to (rad); world pitch = this + S.Pitch.
		static float BodyWorldPitch(const FRideState& S) { return S.GroundPitch + S.Pitch; }

	private:
		static float ComputeDriveForce(const FVehicleParams& P, const FAssistSettings& A,
			const FRiderInput& In, FRideState& S, float Dt);
		static void StepPitch(const FVehicleParams& P, const FAssistSettings& A,
			const FRiderInput& In, float LongAccel, FRideState& S, float Dt);
	};
}
