#include "TrickTracker.h"
#include "RideSim.h"

#include <cmath>
#include <cstdio>

namespace MRSim
{
	namespace
	{
		constexpr float ComboWindow = 2.0f;   // s between tricks to keep a combo going
		constexpr float MinWheelieTime = 0.5f;

		bool IsBicycle(EVehicleCategory C)
		{
			return C == EVehicleCategory::BMX || C == EVehicleCategory::MountainBike
				|| C == EVehicleCategory::RoadBike || C == EVehicleCategory::DownhillBike;
		}

		std::string Fmt(const char* F, double A, double B = 0.0)
		{
			char Buf[96];
			std::snprintf(Buf, sizeof(Buf), F, A, B);
			return Buf;
		}
	}

	void FTrickTracker::Reset()
	{
		*this = FTrickTracker();
	}

	void FTrickTracker::Update(const FVehicleParams& P, const FRideState& S, const FRiderInput& In, const FStepResult& R, float Dt)
	{
		// ---------------- Bail ----------------
		if (S.bCrashed)
		{
			if (!bWasCrashed)
			{
				if (ComboPts > 0)
				{
					FTrickEvent E; E.Trick = ETrick::Bail; E.Label = "Bail! Combo lost"; E.Points = -ComboPts;
					Pending.push_back(E);
				}
				ComboPts = 0; ComboN = 0;
				bInWheelie = bInStoppie = bInSlide = false;
				WheelieTime = WheelieDistance = StoppieTime = StoppieDistance = SlideTime = 0.f;
			}
			bWasCrashed = true;
			bWasAirborne = false;
			return;
		}
		bWasCrashed = false;

		const float Dist = std::fabs(R.Forward);

		// ---------------- Wheelie / manual ----------------
		const bool bWheelieNow = !S.bAirborne && S.bRearGrounded && !S.bFrontGrounded && S.Pitch > 0.12f;
		if (bWheelieNow)
		{
			bInWheelie = true;
			WheelieTime += Dt;
			WheelieDistance += Dist;
			if (In.Throttle > 0.1f) WheeliePoweredTime += Dt;
			const float Bp = WheelieBalanceAngle(P);
			WheelieAngleScore += Dt * (1.f - std::min(1.f, std::fabs(S.Pitch - Bp) / Bp));
		}
		else if (bInWheelie)
		{
			if (WheelieTime >= MinWheelieTime)
			{
				FTrickEvent E;
				const bool bManual = IsBicycle(P.Category) && WheeliePoweredTime < WheelieTime * 0.3f;
				E.Trick = bManual ? ETrick::Manual : ETrick::Wheelie;
				E.Duration = WheelieTime;
				E.Distance = WheelieDistance;
				const float Quality = WheelieAngleScore / WheelieTime; // 0..1 how close to balance point
				E.Points = static_cast<int>(WheelieDistance * (bManual ? 14.f : 10.f) * (1.f + Quality));
				E.Label = Fmt(bManual ? "Manual %.1f m (%.1f s)" : "Wheelie %.1f m (%.1f s)", WheelieDistance, WheelieTime);
				if (WheelieDistance > BestWheelieDist) BestWheelieDist = WheelieDistance;
				if (WheelieTime > BestWheelieT) BestWheelieT = WheelieTime;
				AddToCombo(E);
			}
			bInWheelie = false;
			WheelieTime = WheelieDistance = WheeliePoweredTime = WheelieAngleScore = 0.f;
		}

		// ---------------- Stoppie ----------------
		const bool bStoppieNow = !S.bAirborne && S.bFrontGrounded && !S.bRearGrounded && S.Pitch < -0.10f;
		if (bStoppieNow)
		{
			bInStoppie = true;
			StoppieTime += Dt;
			StoppieDistance += Dist;
		}
		else if (bInStoppie)
		{
			if (StoppieTime > 0.3f)
			{
				FTrickEvent E; E.Trick = ETrick::Stoppie; E.Duration = StoppieTime; E.Distance = StoppieDistance;
				E.Points = static_cast<int>(StoppieDistance * 25.f + StoppieTime * 20.f);
				E.Label = Fmt("Stoppie %.1f m", StoppieDistance);
				AddToCombo(E);
			}
			bInStoppie = false;
			StoppieTime = StoppieDistance = 0.f;
		}

		// ---------------- Slide ----------------
		const bool bSlideNow = !S.bAirborne && S.bRearSliding && std::fabs(S.LateralSpeed) > 1.f && std::fabs(S.Speed) > 3.f;
		if (bSlideNow)
		{
			bInSlide = true;
			SlideTime += Dt;
		}
		else if (bInSlide)
		{
			if (SlideTime > 0.6f)
			{
				FTrickEvent E; E.Trick = ETrick::Slide; E.Duration = SlideTime;
				E.Points = static_cast<int>(SlideTime * 40.f);
				E.Label = Fmt("Slide %.1f s", SlideTime);
				AddToCombo(E);
			}
			bInSlide = false;
			SlideTime = 0.f;
		}

		// ---------------- Air ----------------
		SinceHopRelease = (bWasHopPrimed && !S.bHopPrimed) ? 0.f : SinceHopRelease + Dt;
		if (S.bAirborne)
		{
			if (!bWasAirborne)
			{
				bHopStarted = SinceHopRelease < 0.2f; // took off right after a hop release
			}
			LastAirTime = S.AirTime;
			LastAirYaw = S.AirYaw;
			LastAirPitch = S.AirPitch;
		}
		else if (bWasAirborne && R.bLanded)
		{
			const int HalfSpins = static_cast<int>(std::floor(std::fabs(LastAirYaw) / Pi + 0.25f));
			const int Flips = static_cast<int>(std::floor(std::fabs(LastAirPitch) / (2.f * Pi) + 0.15f));
			if (LastAirTime > 0.35f || HalfSpins > 0 || Flips > 0)
			{
				FTrickEvent E;
				E.Trick = bHopStarted && LastAirTime < 0.8f && HalfSpins == 0 && Flips == 0 ? ETrick::BunnyHop : ETrick::Air;
				E.Duration = LastAirTime;
				E.Points = static_cast<int>(LastAirTime * 60.f) + HalfSpins * 150 + Flips * 1000;
				std::string L = E.Trick == ETrick::BunnyHop ? "Bunny hop" : Fmt("Air %.1f s", LastAirTime);
				if (HalfSpins > 0) L += " + " + std::to_string(HalfSpins * 180);
				if (Flips > 0) L += LastAirPitch < 0.f ? " + Front flip" : " + Backflip";
				if (Flips > 1) L += " x" + std::to_string(Flips);
				E.Label = L;
				AddToCombo(E);
			}
		}
		bWasAirborne = S.bAirborne;
		bWasHopPrimed = S.bHopPrimed;

		// ---------------- Combo banking ----------------
		const bool bBusy = bInWheelie || bInStoppie || bInSlide || S.bAirborne;
		if (!bBusy && ComboN > 0)
		{
			SinceLastTrick += Dt;
			if (SinceLastTrick > ComboWindow) BankCombo();
		}
	}

	void FTrickTracker::AddToCombo(const FTrickEvent& E)
	{
		Pending.push_back(E);
		ComboPts += E.Points;
		ComboN++;
		SinceLastTrick = 0.f;
	}

	void FTrickTracker::BankCombo()
	{
		Total += ComboPts * ComboN;
		ComboPts = 0;
		ComboN = 0;
		SinceLastTrick = 0.f;
	}

	std::vector<FTrickEvent> FTrickTracker::ConsumeEvents()
	{
		std::vector<FTrickEvent> Out;
		Out.swap(Pending);
		return Out;
	}
}
