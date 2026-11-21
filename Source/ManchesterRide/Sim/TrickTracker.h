// Manchester Riding Simulator - trick detection and scoring (engine independent).
#pragma once

#include "RideSimTypes.h"

#include <string>
#include <vector>

namespace MRSim
{
	enum class ETrick : uint8_t
	{
		Wheelie,   // powered (pedalling / throttle) rear-wheel ride
		Manual,    // un-powered rear-wheel balance (bicycles)
		Stoppie,   // front-wheel ride under braking
		BunnyHop,
		Air,       // jump; spins and flips are added to its label/points
		Slide,     // controlled rear slide
		Bail,      // crash: combo lost
	};

	struct FTrickEvent
	{
		ETrick Trick = ETrick::Wheelie;
		std::string Label;
		float Duration = 0.f;  // s
		float Distance = 0.f;  // m
		int   Points = 0;
	};

	class FTrickTracker
	{
	public:
		void Reset();
		void Update(const FVehicleParams& P, const FRideState& S, const FRiderInput& In, const FStepResult& R, float Dt);

		// Events finished since the last call (for HUD popups / replication to the server).
		std::vector<FTrickEvent> ConsumeEvents();

		// Live values for the HUD
		bool  IsWheelieActive() const { return bInWheelie; }
		float CurrentWheelieTime() const { return WheelieTime; }
		float CurrentWheelieDistance() const { return WheelieDistance; }
		int   ComboPoints() const { return ComboPts; }
		int   ComboCount() const { return ComboN; }
		int   TotalScore() const { return Total; }
		float BestWheelieDistance() const { return BestWheelieDist; }
		float BestWheelieTime() const { return BestWheelieT; }

	private:
		void AddToCombo(const FTrickEvent& E);
		void BankCombo();

		bool  bInWheelie = false;
		float WheelieTime = 0.f, WheelieDistance = 0.f, WheeliePoweredTime = 0.f, WheelieAngleScore = 0.f;
		bool  bInStoppie = false;
		float StoppieTime = 0.f, StoppieDistance = 0.f;
		bool  bInSlide = false;
		float SlideTime = 0.f;
		bool  bWasAirborne = false, bHopStarted = false, bWasHopPrimed = false;
		float SinceHopRelease = 10.f;
		float LastAirTime = 0.f, LastAirYaw = 0.f, LastAirPitch = 0.f;
		bool  bWasCrashed = false;

		int   ComboPts = 0, ComboN = 0, Total = 0;
		float SinceLastTrick = 0.f;
		float BestWheelieDist = 0.f, BestWheelieT = 0.f;
		std::vector<FTrickEvent> Pending;
	};
}
