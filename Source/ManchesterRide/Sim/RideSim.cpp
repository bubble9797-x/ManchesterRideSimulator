#include "RideSim.h"

#include <algorithm>
#include <cmath>

namespace MRSim
{
	namespace
	{
		inline float Clamp(float V, float Lo, float Hi) { return V < Lo ? Lo : (V > Hi ? Hi : V); }
		inline float Sign(float V) { return V > 0.f ? 1.f : (V < 0.f ? -1.f : 0.f); }
		inline float MoveTowards(float Cur, float Target, float MaxDelta)
		{
			const float D = Target - Cur;
			return std::fabs(D) <= MaxDelta ? Target : Cur + Sign(D) * MaxDelta;
		}
		inline float WrapPi(float A)
		{
			while (A > Pi) A -= 2.f * Pi;
			while (A < -Pi) A += 2.f * Pi;
			return A;
		}

		constexpr int NumSurf = static_cast<int>(ESurface::Count);
		//                                   Asph  Conc  Grav  Grass Mud   Dirt  Forest Cobbles
		const float kBaseGrip[NumSurf]   = { 1.00f, 0.95f, 0.60f, 0.55f, 0.35f, 0.70f, 0.65f, 0.75f };
		const float kWetMul[NumSurf]     = { 0.70f, 0.72f, 0.90f, 0.60f, 0.75f, 0.70f, 0.75f, 0.50f };
		const float kRolling[NumSurf]    = { 1.00f, 1.05f, 2.50f, 3.50f, 6.00f, 2.20f, 2.60f, 1.60f };
		const float kTyre[static_cast<int>(ETyre::Count)][NumSurf] = {
			/* Road      */ { 1.05f, 1.00f, 0.60f, 0.55f, 0.40f, 0.60f, 0.55f, 0.95f },
			/* Hybrid    */ { 1.00f, 1.00f, 0.80f, 0.75f, 0.60f, 0.80f, 0.75f, 0.95f },
			/* Knobbly   */ { 0.92f, 0.92f, 1.00f, 1.00f, 1.00f, 1.05f, 1.05f, 0.90f },
			/* Motocross */ { 0.85f, 0.85f, 1.10f, 1.10f, 1.25f, 1.15f, 1.10f, 0.85f },
			/* Sport     */ { 1.12f, 1.05f, 0.55f, 0.50f, 0.35f, 0.55f, 0.50f, 0.95f },
		};

		struct FAssistTuning
		{
			float PitchGain;          // strength of the wheelie/stoppie balance controller
			bool  bNoLoopOut;         // clamp pitch instead of crashing
			float LowsideFactor;      // lateral demand / grip ratio that causes a lowside
			float LandingPitchLimit;  // rad mismatch tolerated on landing
			float LandingImpactMul;   // multiplier on the vehicle's landing impact tolerance
			float LandingYawLimit;    // rad of unfinished spin tolerated on landing
		};

		FAssistTuning GetTuning(EWheelieAssist Mode)
		{
			switch (Mode)
			{
			case EWheelieAssist::Casual:    return { 1.00f, true,  1e9f, 0.90f, 1.60f, 1.00f };
			case EWheelieAssist::Balanced:  return { 0.45f, false, 2.20f, 0.60f, 1.25f, 0.60f };
			case EWheelieAssist::Realistic:
			default:                        return { 0.00f, false, 1.60f, 0.45f, 1.00f, 0.35f };
			}
		}

		// Vertical speed into the ground (m/s) a rider can absorb on landing.
		float LandingImpactTolerance(EVehicleCategory C)
		{
			switch (C)
			{
			case EVehicleCategory::BMX:          return 7.5f;
			case EVehicleCategory::MountainBike: return 7.0f;
			case EVehicleCategory::DownhillBike: return 9.0f;
			case EVehicleCategory::RoadBike:     return 4.0f;
			case EVehicleCategory::EBike:        return 6.0f;
			case EVehicleCategory::EScooter:     return 3.5f;
			case EVehicleCategory::DirtBike:     return 11.0f;
			case EVehicleCategory::Motorcycle:   return 6.0f;
			}
			return 6.f;
		}

		// Rider control authority in the air (rad/s): pitch for flips, yaw for spins.
		void AirControlRates(EVehicleCategory C, float& OutPitch, float& OutSpin)
		{
			switch (C)
			{
			case EVehicleCategory::BMX:          OutPitch = 4.5f; OutSpin = 4.5f; return;
			case EVehicleCategory::MountainBike:
			case EVehicleCategory::DownhillBike: OutPitch = 3.0f; OutSpin = 3.2f; return;
			case EVehicleCategory::DirtBike:     OutPitch = 1.8f; OutSpin = 1.4f; return;
			case EVehicleCategory::EBike:        OutPitch = 2.2f; OutSpin = 2.0f; return;
			case EVehicleCategory::EScooter:     OutPitch = 1.6f; OutSpin = 2.0f; return;
			default:                             OutPitch = 1.2f; OutSpin = 0.8f; return;
			}
		}

		float SpringForce(const FSuspensionParams& Sp, const FWheelContact& C, float PrevComp, float Dt, float& OutComp)
		{
			if (!C.bHit || C.Distance >= Sp.RestLength)
			{
				OutComp = 0.f;
				return 0.f;
			}
			OutComp = std::min(Sp.RestLength - C.Distance, Sp.Travel * 1.5f);
			const float Vel = (OutComp - PrevComp) / Dt;
			const float Damp = Vel >= 0.f ? Sp.CompressionDamping : Sp.ReboundDamping;
			float F = Sp.Stiffness * OutComp + Damp * Vel;
			if (OutComp > Sp.Travel)
			{
				F += Sp.Stiffness * 8.f * (OutComp - Sp.Travel); // bump stop
			}
			return std::max(0.f, F) * std::max(0.2f, C.NormalZ);
		}
	}

	float SurfaceGrip(ESurface Surface, ETyre Tyre, float Wetness)
	{
		const int S = static_cast<int>(Surface);
		const int T = static_cast<int>(Tyre);
		const float Wet = 1.f - Clamp(Wetness, 0.f, 1.f) * (1.f - kWetMul[S]);
		return kBaseGrip[S] * kTyre[T][S] * Wet;
	}

	float SurfaceRollingMultiplier(ESurface Surface)
	{
		return kRolling[static_cast<int>(Surface)];
	}

	float WheelieBalanceAngle(const FVehicleParams& P)
	{
		return std::atan2(P.ComFromRear, P.ComHeight);
	}

	void FRideSimulator::InitState(const FVehicleParams& P, FRideState& S)
	{
		S = FRideState();
		S.BatteryWh = P.Powertrain.BatteryWh;
		S.FuelLitres = P.Powertrain.FuelLitres;
		S.EngineRpm = P.Powertrain.Type == EPowertrain::Combustion ? P.Powertrain.IdleRpm : 0.f;
		S.Gear = 1;
		S.ClutchEngage = P.Powertrain.Type == EPowertrain::Combustion ? 0.f : 1.f;
	}

	float FRideSimulator::ComputeDriveForce(const FVehicleParams& P, const FAssistSettings& A,
		const FRiderInput& In, FRideState& S, float Dt)
	{
		const FPowertrainParams& PT = P.Powertrain;
		const float V = std::max(S.Speed, 0.f);
		const float VEff = std::max(V, 0.5f);
		const float Throttle = Clamp(In.Throttle, 0.f, 1.f);
		const float Governor = Clamp(P.TopSpeed - V, 0.f, 1.f);
		S.MotorPowerW = 0.f;

		auto RiderForce = [&]() { return std::min(PT.RiderPeakForceN, PT.RiderSustainedPowerW / VEff) * Throttle; };
		auto DrainBattery = [&](float ForceN)
		{
			S.MotorPowerW = ForceN * V;
			S.BatteryWh = std::max(0.f, S.BatteryWh - (S.MotorPowerW / PT.MotorEfficiency) * Dt / 3600.f);
		};

		switch (PT.Type)
		{
		case EPowertrain::Pedal:
			return RiderForce() * Governor;

		case EPowertrain::PedalAssist:
		{
			const float FRider = RiderForce();
			const float Mult = PT.AssistLevels[std::min(3, std::max(0, In.AssistLevel))];
			const float Cutoff = Clamp(PT.MotorCutoffSpeed - V, 0.f, 1.f); // taper over the last 1 m/s
			float FMotor = 0.f;
			if (S.BatteryWh > 0.f)
			{
				FMotor = std::min({ PT.MotorPeakForceN, FRider * Mult, PT.MotorPeakPowerW / VEff }) * Cutoff;
				DrainBattery(FMotor);
			}
			return (FRider + FMotor) * Governor;
		}

		case EPowertrain::ElectricThrottle:
		{
			if (S.BatteryWh <= 0.f) return 0.f;
			const float Cutoff = Clamp(PT.MotorCutoffSpeed - V, 0.f, 1.f);
			const float F = Throttle * std::min(PT.MotorPeakForceN, PT.MotorPeakPowerW / VEff) * Cutoff * Governor;
			DrainBattery(F);
			return F;
		}

		case EPowertrain::Combustion:
		{
			// Gear selection
			if (In.GearRequest > 0 && S.Gear < PT.NumGears) S.Gear++;
			if (In.GearRequest < 0 && S.Gear > 1) S.Gear--;

			const float RpmToRad = 2.f * Pi / 60.f;
			const float Ratio = PT.GearRatios[S.Gear - 1] * PT.FinalDrive;
			const float WheelOmega = S.Speed / P.WheelRadiusRear;
			const float LockedOmega = WheelOmega * Ratio;

			// Clutch: the manual lever always overrides. The auto clutch behaves like a
			// centrifugal clutch: it bites progressively as ENGINE rpm rises above idle.
			float EngageTarget = 1.f - Clamp(In.Clutch, 0.f, 1.f);
			if (A.bAutoClutch && In.Clutch < 0.05f)
			{
				EngageTarget = Clamp((S.EngineRpm - PT.IdleRpm * 1.15f) / 1800.f, 0.f, 1.f);
			}
			S.ClutchEngage = MoveTowards(S.ClutchEngage, EngageTarget, 6.f * Dt); // full dump in ~0.17 s

			float Omega = S.EngineRpm * RpmToRad;
			const float IdleOmega = PT.IdleRpm * RpmToRad;
			const float Redline = PT.RedlineRpm * RpmToRad;
			auto TorqueAt = [&](float Rpm)
			{
				const float X = (Rpm - PT.PeakTorqueRpm) / (PT.RedlineRpm - PT.IdleRpm);
				return PT.PeakTorqueNm * Clamp(1.f - 1.6f * X * X, 0.35f, 1.f);
			};
			// Idle governor keeps the engine running; the rev limiter cuts torque at redline.
			const float IdleT = Clamp((IdleOmega - Omega) * 0.02f, 0.f, 0.25f) * PT.PeakTorqueNm;
			const float EngineT = (S.FuelLitres > 0.f && Omega < Redline) ? std::max(Throttle * TorqueAt(S.EngineRpm), IdleT) : 0.f;
			const float FrictionT = PT.PeakTorqueNm * (0.02f + 0.08f * (Omega / Redline)) * (Throttle < 0.02f ? 2.f : 1.f);

			// Stick/slip friction clutch: lock if the torque needed to hold sync is within capacity.
			const float Capacity = S.ClutchEngage * PT.PeakTorqueNm * 2.5f;
			const float Slip = Omega - LockedOmega;
			const float SyncT = Slip * PT.EngineInertia / Dt + (EngineT - FrictionT);
			float ClutchT;
			if (std::fabs(SyncT) <= Capacity && LockedOmega > IdleOmega * 0.7f)
			{
				Omega = LockedOmega; // locked: net engine torque reaches the road (negative = engine braking)
				ClutchT = EngineT - FrictionT;
			}
			else
			{
				ClutchT = Sign(Slip) * Capacity;
				Omega += (EngineT - FrictionT - ClutchT) / PT.EngineInertia * Dt;
				Omega = Clamp(Omega, IdleOmega * 0.7f, Redline * 1.02f);
			}
			S.EngineRpm = Omega / RpmToRad;

			if (A.bAutoShift && S.ClutchEngage > 0.9f)
			{
				if (S.EngineRpm > PT.RedlineRpm * 0.9f && S.Gear < PT.NumGears) S.Gear++;
				else if (S.EngineRpm < PT.PeakTorqueRpm * 0.55f && S.Gear > 1) S.Gear--;
			}

			const float PowerW = std::max(0.f, ClutchT * Omega);
			S.MotorPowerW = PowerW;
			S.FuelLitres = std::max(0.f, S.FuelLitres - (PowerW / 1000.f) * PT.BsfcGPerKWh / 3600.f * Dt / 740.f);

			const float F = ClutchT * Ratio * 0.9f / P.WheelRadiusRear;
			return F > 0.f ? F * Governor : F;
		}
		}
		return 0.f;
	}

	void FRideSimulator::StepPitch(const FVehicleParams& P, const FAssistSettings& A,
		const FRiderInput& In, float LongAccel, FRideState& S, float Dt)
	{
		const FAssistTuning T = GetTuning(A.WheelieAssist);
		const float M = P.Mass;
		const float Ar = P.ComFromRear;
		const float Bf = P.Wheelbase - P.ComFromRear;
		const float H = P.ComHeight;
		const float K2 = P.PitchRadiusOfGyration * P.PitchRadiusOfGyration;
		const float Gain = T.PitchGain * P.Wheelie.AssistStrength;

		if (S.bAirborne)
		{
			float AirPitchRate, AirSpinRate;
			AirControlRates(P.Category, AirPitchRate, AirSpinRate);
			float TargetRate = Clamp(In.Balance, -1.f, 1.f) * AirPitchRate;
			if (A.bLandingAssist && std::fabs(In.Balance) < 0.1f)
			{
				// Nose follows the trajectory so landings are clean.
				const float Traj = Clamp(std::atan2(S.VerticalSpeed, std::max(std::fabs(S.Speed), 1.f)), -0.6f, 0.6f);
				const float World = S.GroundPitch + S.Pitch;
				TargetRate = Clamp((Traj - WrapPi(World)) * 3.f, -AirPitchRate, AirPitchRate);
			}
			S.PitchRate = MoveTowards(S.PitchRate, TargetRate, 10.f * Dt);
			S.Pitch += S.PitchRate * Dt;
			S.AirPitch += S.PitchRate * Dt;
			return;
		}

		// Rider weight shift (+ = nose up)
		float Rider = Clamp(In.Balance, -1.f, 1.f) * P.Wheelie.RiderBalanceTorque;
		if (In.bWheelie && S.bRearGrounded)
		{
			Rider += P.Wheelie.RiderLiftTorque;
		}

		if (S.Pitch > 0.f || (S.bRearGrounded && S.Pitch == 0.f))
		{
			// --- Wheelie side: rotation about the rear contact ---
			const float Th = S.Pitch;
			const float IRear = M * (K2 + Ar * Ar + H * H);
			float Tau = M * LongAccel * (Ar * std::sin(Th) + H * std::cos(Th))
				- M * Gravity * (Ar * std::cos(Th) - H * std::sin(Th))
				+ Rider;

			// Balance assist: a PD controller toward a target just under the balance point.
			// It must act from Th = 0 so the button alone can start a wheelie in casual mode.
			const float Bp = WheelieBalanceAngle(P);
			const bool bHolding = (In.bWheelie || In.Balance > 0.2f) && S.bRearGrounded;
			if (Gain > 0.f && (Th > 0.03f || bHolding))
			{
				const float Target = bHolding ? Bp * 0.82f + Clamp(In.Balance, -1.f, 1.f) * 0.12f : 0.f;
				Tau += Gain * IRear * (14.f * (Target - Th) - 5.f * S.PitchRate);
			}

			if (Th <= 0.f && Tau <= 0.f)
			{
				S.Pitch = 0.f;
				S.PitchRate = 0.f;
			}
			else
			{
				S.PitchRate += (Tau / IRear - 1.5f * S.PitchRate) * Dt;
				S.Pitch += S.PitchRate * Dt;

				if (S.Pitch < 0.f)
				{
					S.Pitch = 0.f;
					S.PitchRate = 0.f;
				}
				const float LoopOut = Bp + P.Wheelie.LoopOutMargin;
				if (S.Pitch > LoopOut)
				{
					if (T.bNoLoopOut)
					{
						S.Pitch = Bp + 0.05f;
						S.PitchRate = std::min(S.PitchRate, 0.f);
					}
					else
					{
						S.bCrashed = true;
					}
				}
				return;
			}
		}

		// --- Stoppie side: rotation about the front contact (Psi = -Pitch >= 0) ---
		if (!P.Wheelie.bCanStoppie || !S.bFrontGrounded)
		{
			S.Pitch = std::max(S.Pitch, 0.f);
			S.PitchRate = 0.f;
			return;
		}
		const float Psi = -S.Pitch;
		const float IFront = M * (K2 + Bf * Bf + H * H);
		float TauF = -M * LongAccel * (Bf * std::sin(Psi) + H * std::cos(Psi))
			- M * Gravity * (Bf * std::cos(Psi) - H * std::sin(Psi))
			- Rider;
		if (Psi <= 0.f && TauF <= 0.f)
		{
			S.Pitch = 0.f;
			S.PitchRate = 0.f;
			return;
		}
		float PsiRate = -S.PitchRate;
		if (Gain > 0.f && Psi > 0.22f)
		{
			TauF += Gain * IFront * (14.f * (0.22f - Psi) - 5.f * PsiRate);
		}
		PsiRate += (TauF / IFront - 1.5f * PsiRate) * Dt;
		float NewPsi = Psi + PsiRate * Dt;
		if (NewPsi < 0.f)
		{
			NewPsi = 0.f;
			PsiRate = 0.f;
		}
		const float Endo = std::atan2(Bf, H) + P.Wheelie.LoopOutMargin;
		if (NewPsi > Endo)
		{
			if (T.bNoLoopOut)
			{
				NewPsi = Endo - 0.3f;
				PsiRate = std::min(PsiRate, 0.f);
			}
			else
			{
				S.bCrashed = true;
			}
		}
		S.Pitch = -NewPsi;
		S.PitchRate = -PsiRate;
	}

	FStepResult FRideSimulator::Step(const FVehicleParams& P, const FAssistSettings& A,
		const FRiderInput& In, const FWheelContact& Front, const FWheelContact& Rear,
		const FEnvironment& Env, FRideState& S, float Dt)
	{
		FStepResult R;
		const FAssistTuning T = GetTuning(A.WheelieAssist);
		const float M = P.Mass;

		// ---------------- Crashed: slide to a halt; the pawn handles respawn ----------------
		if (S.bCrashed)
		{
			S.CrashTimer += Dt;
			S.Speed = MoveTowards(S.Speed, 0.f, 8.f * Dt);
			S.LateralSpeed = MoveTowards(S.LateralSpeed, 0.f, 8.f * Dt);
			S.Lean = MoveTowards(S.Lean, (S.Lean >= 0.f ? 1.f : -1.f) * 1.3f, 3.f * Dt);
			S.Pitch = MoveTowards(S.Pitch, 0.f, 2.f * Dt);
			S.YawRate = 0.f;
			const bool bAnyContact = Front.bHit || Rear.bHit;
			S.VerticalSpeed = bAnyContact ? 0.f : S.VerticalSpeed - Gravity * Dt;
			R.Forward = S.Speed * Dt;
			R.Right = S.LateralSpeed * Dt;
			R.Up = S.VerticalSpeed * Dt;
			return R;
		}

		// ---------------- Suspension ----------------
		float CompF = 0.f, CompR = 0.f;
		const float SpringF = SpringForce(P.Front, Front, S.CompressionFront, Dt, CompF);
		const float SpringR = SpringForce(P.Rear, Rear, S.CompressionRear, Dt, CompR);
		const bool bGF = Front.bHit && CompF > 0.f;
		const bool bGR = Rear.bHit && CompR > 0.f;
		const bool bGrounded = bGF || bGR;
		S.CompressionFront = CompF;
		S.CompressionRear = CompR;

		const float MuF = bGF ? SurfaceGrip(Front.Surface, P.Tyre, Env.Wetness) * P.TyreGrip : 0.f;
		const float MuR = bGR ? SurfaceGrip(Rear.Surface, P.Tyre, Env.Wetness) * P.TyreGrip : 0.f;

		// ---------------- Take-off / landing transitions ----------------
		const bool bWasAirborne = S.bAirborne;
		if (!bGrounded && !bWasAirborne)
		{
			// Leaving the ground: convert slope-aligned speed into a ballistic velocity.
			S.VerticalSpeed += S.Speed * std::sin(S.GroundPitch);
			S.Speed *= std::cos(S.GroundPitch);
			S.AirTime = 0.f;
			S.AirPitch = 0.f;
			S.AirYaw = 0.f;
		}
		else if (bGrounded && bWasAirborne)
		{
			const float Slope = Env.SlopePitch;
			const float Rel = WrapPi(S.GroundPitch + S.Pitch - Slope);
			const float Impact = -(S.VerticalSpeed * std::cos(Slope) - S.Speed * std::sin(Slope));
			S.Speed = S.Speed * std::cos(Slope) + S.VerticalSpeed * std::sin(Slope);
			S.VerticalSpeed = -std::max(0.f, Impact) * 0.5f; // the rest is absorbed by tyres/rider
			R.bLanded = true;
			R.LandingImpactSpeed = std::max(0.f, Impact);

			const float YawErr = WrapPi(S.AirYaw);
			const bool bBad = R.LandingImpactSpeed > LandingImpactTolerance(P.Category) * T.LandingImpactMul
				|| Rel > T.LandingPitchLimit * 1.3f      // rear-first landings are more forgiving
				|| Rel < -T.LandingPitchLimit
				|| std::fabs(YawErr) > T.LandingYawLimit
				|| std::fabs(S.Lean) > 0.9f;
			if (bBad)
			{
				S.bCrashed = true;
				R.bCrashedThisStep = true;
			}
			S.GroundPitch = Slope;
			S.Pitch = std::max(0.f, Rel);
			S.PitchRate = 0.f;
			S.AirYaw = 0.f;
		}
		S.bAirborne = !bGrounded;
		S.bFrontGrounded = bGF;
		S.bRearGrounded = bGR;
		if (S.bCrashed)
		{
			return R;
		}

		if (bGrounded)
		{
			// Fast follow so the bike rotates onto ramps rather than being shoved up by the springs.
			S.GroundPitch += (Env.SlopePitch - S.GroundPitch) * std::min(1.f, 30.f * Dt);
		}

		// ---------------- Normal loads with longitudinal load transfer ----------------
		const float NTotal = SpringF + SpringR;
		float NF, NR;
		if (bGF && bGR)
		{
			NF = NTotal * (P.ComFromRear / P.Wheelbase) - M * S.LongAccel * P.ComHeight / P.Wheelbase;
			NF = Clamp(NF, 0.f, NTotal);
			NR = NTotal - NF;
		}
		else
		{
			NF = bGF ? NTotal : 0.f;
			NR = bGR ? NTotal : 0.f;
		}
		S.LoadFront = NF;
		S.LoadRear = NR;

		// ---------------- Longitudinal ----------------
		const float VAbs0 = std::fabs(S.Speed);
		const float MuMax = std::max(0.05f, std::max(MuF, MuR));
		const float LatUse = Clamp(std::fabs(S.Speed * S.YawRate) / (MuMax * Gravity), 0.f, 0.95f);
		const float LongScale = std::sqrt(1.f - LatUse * LatUse);

		float Drive = ComputeDriveForce(P, A, In, S, Dt);
		const float RearLimit = MuR * NR * LongScale;
		const float FrontLimit = MuF * NF * LongScale;
		S.bRearSliding = false;
		S.bFrontLocked = false;
		// Reverse: rider walks/paddles the bike backwards (motorbikes: paddling with the feet), capped at
		// a walking pace. Only while on the ground and not pedalling forwards.
		if (In.Reverse > 0.f && bGR && In.Throttle < 0.05f)
		{
			const float MaxBack = P.Category == EVehicleCategory::Motorcycle || P.Category == EVehicleCategory::DirtBike ? 1.6f : 1.9f;
			const float Want = -MaxBack * Clamp(In.Reverse, 0.f, 1.f);
			Drive = S.Speed > Want ? -M * 1.6f * Clamp(In.Reverse, 0.f, 1.f) : 0.f;
		}
		if (!bGR)
		{
			Drive = 0.f;
		}
		else if (std::fabs(Drive) > RearLimit)
		{
			Drive = Sign(Drive) * RearLimit * 0.92f;
			S.bRearSliding = true; // wheelspin
		}

		float BrakeF = bGF ? Clamp(In.FrontBrake, 0.f, 1.f) * P.MaxBrakeForceFront : 0.f;
		float BrakeR = bGR ? Clamp(In.RearBrake, 0.f, 1.f) * P.MaxBrakeForceRear : 0.f;
		if (BrakeF > FrontLimit)
		{
			if (A.bABS) BrakeF = FrontLimit;
			else { BrakeF = FrontLimit * 0.8f; S.bFrontLocked = true; }
		}
		if (BrakeR > RearLimit)
		{
			BrakeR = RearLimit * 0.8f;
			S.bRearSliding = true; // rear skid - used for controlled slides
		}
		if (P.Powertrain.RegenFraction > 0.f && BrakeR > 0.f)
		{
			S.BatteryWh = std::min(P.Powertrain.BatteryWh, S.BatteryWh + BrakeR * VAbs0 * P.Powertrain.RegenFraction * Dt / 3600.f);
		}

		const float Drag = 0.5f * AirDensity * P.CdA * S.Speed * VAbs0;
		const float SurfRoll = 0.5f * ((bGF ? SurfaceRollingMultiplier(Front.Surface) : 0.f) + (bGR ? SurfaceRollingMultiplier(Rear.Surface) : 0.f))
			* ((bGF && bGR) ? 1.f : 2.f);
		const float Rolling = bGrounded ? P.RollingResistance * M * Gravity * SurfRoll : 0.f;
		const float SlopeForce = bGrounded ? -M * Gravity * std::sin(S.GroundPitch) : 0.f;

		float V = S.Speed + (Drive - Drag + SlopeForce) / M * Dt;
		// Rolling backwards with nothing held: the rider plants a foot / grabs a brake and stops.
		const float FootStop = (bGrounded && S.Speed < 0.f && In.Reverse <= 0.f) ? M * 3.f : 0.f;
		const float ResistDv = (BrakeF + BrakeR + Rolling + FootStop) / M * Dt; // always opposes motion
		V = std::fabs(V) <= ResistDv ? 0.f : V - Sign(V) * ResistDv;
		// Filtered (~40 ms) so load transfer and pitch don't react to single-step spikes.
		const float RawAccel = (V - S.Speed) / Dt;
		S.LongAccel += (RawAccel - S.LongAccel) * std::min(1.f, Dt / 0.04f);
		S.Speed = V;
		S.DriveForce = Drive;
		const float VAbs = std::fabs(S.Speed);

		// ---------------- Pitch (wheelie / stoppie / air) ----------------
		StepPitch(P, A, In, bGrounded ? S.LongAccel : 0.f, S, Dt);
		if (S.bCrashed)
		{
			R.bCrashedThisStep = true;
			return R;
		}

		// ---------------- Steering, lean and slides ----------------
		const float Sens = A.SteerSensitivity;
		const float SteerIn = Clamp(In.Steer * Sens, -1.f, 1.f);
		if (bGrounded)
		{
			const float SpeedRatio = VAbs / P.SteerSpeedRef;
			const float MaxSteer = P.MaxSteerAngle / (1.f + SpeedRatio * SpeedRatio);
			float SteerTarget = SteerIn * MaxSteer;
			if (S.bFrontLocked) SteerTarget *= 0.15f;
			S.Steer = MoveTowards(S.Steer, SteerTarget, 3.5f * Dt);

			float YawKin = 0.f;
			if (bGF)
			{
				YawKin = S.Speed * std::tan(S.Steer) / P.Wheelbase;
			}
			else
			{
				// Front wheel up: steer by leaning the bike on the rear wheel.
				YawKin = SteerIn * Gravity * std::tan(0.35f) / std::max(VAbs, 2.f) * Sign(S.Speed);
			}

			const float MuAvg = (bGF && bGR) ? 0.5f * (MuF + MuR) : MuMax;
			const float LongUse = Clamp(std::fabs(S.LongAccel) / (std::max(0.05f, MuAvg) * Gravity), 0.f, 0.95f);
			const float LatMax = MuAvg * Gravity * std::sqrt(1.f - LongUse * LongUse);
			const float LatDemand = std::fabs(S.Speed * YawKin);

			float YawRate = YawKin;
			if (LatDemand > LatMax && VAbs > 0.5f)
			{
				YawRate = Sign(YawKin) * LatMax / VAbs; // tyres saturate: bike runs wide
				if (LatDemand > LatMax * T.LowsideFactor)
				{
					S.bCrashed = true;
					R.bCrashedThisStep = true;
					return R;
				}
			}

			if (S.bRearSliding && VAbs > 2.f && bGF)
			{
				// Rear steps out: extra yaw and a sideways component (controlled slide).
				YawRate += SteerIn * 1.4f * std::min(1.f, VAbs / 6.f);
				const float SlideTarget = -Sign(YawRate) * std::min(VAbs * 0.35f, std::fabs(YawRate) * VAbs * 0.25f);
				S.LateralSpeed = MoveTowards(S.LateralSpeed, SlideTarget, 6.f * Dt);
			}
			else
			{
				S.LateralSpeed = MoveTowards(S.LateralSpeed, 0.f, MuAvg * Gravity * 0.8f * Dt);
			}
			S.YawRate = YawRate;

			float LeanEq = Clamp(std::atan(S.Speed * YawRate / Gravity), -P.MaxLean, P.MaxLean);
			if (S.bRearSliding) LeanEq *= 0.7f;
			S.Lean += (LeanEq - S.Lean) * std::min(1.f, P.LeanResponse * Dt);
		}
		else
		{
			float AirPitchRate, AirSpinRate;
			AirControlRates(P.Category, AirPitchRate, AirSpinRate);
			S.YawRate = 0.f; // heading (and velocity) is kept; spins are visual + scored
			S.AirYaw += SteerIn * AirSpinRate * Dt;
			S.Lean = MoveTowards(S.Lean, SteerIn * 0.25f, 1.f * Dt);
			S.LateralSpeed = MoveTowards(S.LateralSpeed, 0.f, 0.5f * Dt);
			S.AirTime += Dt;
			if (A.bLandingAssist && std::fabs(In.Steer) < 0.1f && T.bNoLoopOut)
			{
				// Casual: finish a nearly-complete spin automatically.
				const float Err = WrapPi(S.AirYaw);
				S.AirYaw -= Clamp(Err * 3.f, -AirSpinRate, AirSpinRate) * Dt;
			}
		}

		// ---------------- Vertical ----------------
		if (bGrounded)
		{
			S.VerticalSpeed += ((SpringF + SpringR) / M - Gravity * std::cos(S.GroundPitch)) * Dt;
			const float OverF = CompF - P.Front.Travel;
			const float OverR = CompR - P.Rear.Travel;
			const float Over = std::max(OverF, OverR);
			if (Over > 0.f)
			{
				R.Up += Over; // positional bump-stop correction
				S.VerticalSpeed = std::max(S.VerticalSpeed, 0.f);
			}
			if (In.bHop)
			{
				S.bHopPrimed = true;
			}
			else if (S.bHopPrimed)
			{
				S.bHopPrimed = false;
				if (P.Wheelie.BunnyHopSpeed > 0.f)
				{
					S.VerticalSpeed = std::max(S.VerticalSpeed, 0.f) + P.Wheelie.BunnyHopSpeed;
					S.PitchRate += 1.2f; // front comes up first
				}
			}
		}
		else
		{
			S.VerticalSpeed -= Gravity * Dt;
			if (!In.bHop) S.bHopPrimed = false;
		}

		R.Forward = S.Speed * Dt;
		R.Right = S.LateralSpeed * Dt;
		R.Up += S.VerticalSpeed * Dt;
		R.Yaw = S.YawRate * Dt;
		return R;
	}
}
