// Standalone tests for the engine-independent riding simulation.
// Build (Windows, from a "x64 Native Tools" prompt or via Tests/SimTests/build_and_run.bat):
//   cl /std:c++17 /O2 /EHsc /I ..\..\Source\ManchesterRide\Sim SimTests.cpp ..\..\Source\ManchesterRide\Sim\*.cpp
//
// The harness mirrors what AMRRideVehicle does in Unreal: it keeps a body frame,
// samples wheel contacts with vertical rays from the hub mount points, and moves
// the body by the step result (slope-aligned when grounded, ballistic in the air).

#include "RideSim.h"
#include "TrickTracker.h"
#include "VehicleCatalog.h"
#include "GeoProjection.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <functional>
#include <string>

using namespace MRSim;

static int GFailures = 0;
static int GChecks = 0;
#define CHECK(cond, ...) do { ++GChecks; if (!(cond)) { ++GFailures; std::printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct FGround
{
	std::function<float(float)> Height = [](float) { return 0.f; };
	std::function<ESurface(float)> Surface = [](float) { return ESurface::Asphalt; };
};

struct FHarness
{
	const FVehicleParams& P;
	FAssistSettings A;
	FEnvironment Env;
	FGround G;
	FRideState S;
	FTrickTracker Tricks;
	float X = 0.f, Z = 0.f; // rear contact (body origin) at full extension
	float Time = 0.f;
	float MaxPitch = -10.f, MinPitch = 10.f, MaxAirTime = 0.f, MaxLean = 0.f;
	bool  bEverCrashed = false, bEverAirborne = false;
	std::vector<FTrickEvent> Events;

	explicit FHarness(const FVehicleParams& InP) : P(InP)
	{
		FRideSimulator::InitState(P, S);
		Z = G.Height(0.f);
	}

	void MountPositions(float& RX, float& RZ, float& FX, float& FZ) const
	{
		const float Gp = S.GroundPitch;
		const float Wp = FRideSimulator::BodyWorldPitch(S);
		// Base (ground aligned) contact points
		const float BaseRX = X, BaseRZ = Z;
		const float BaseFX = X + std::cos(Gp) * P.Wheelbase, BaseFZ = Z + std::sin(Gp) * P.Wheelbase;
		float PivX, PivZ, CRX, CRZ, CFX, CFZ;
		if (S.Pitch >= 0.f)
		{
			PivX = BaseRX; PivZ = BaseRZ;
			CRX = PivX; CRZ = PivZ;
			CFX = PivX + std::cos(Wp) * P.Wheelbase; CFZ = PivZ + std::sin(Wp) * P.Wheelbase;
		}
		else
		{
			PivX = BaseFX; PivZ = BaseFZ;
			CFX = PivX; CFZ = PivZ;
			CRX = PivX - std::cos(Wp) * P.Wheelbase; CRZ = PivZ - std::sin(Wp) * P.Wheelbase;
		}
		// Mount = contact + body up * rest length
		RX = CRX - std::sin(Wp) * P.Rear.RestLength;  RZ = CRZ + std::cos(Wp) * P.Rear.RestLength;
		FX = CFX - std::sin(Wp) * P.Front.RestLength; FZ = CFZ + std::cos(Wp) * P.Front.RestLength;
	}

	FWheelContact Trace(float MX, float MZ, float MaxDist) const
	{
		FWheelContact C;
		const float H = G.Height(MX);
		const float D = MZ - H;
		C.bHit = D >= -0.5f && D <= MaxDist;
		C.Distance = std::max(0.f, D);
		const float Slope = (G.Height(MX + 0.05f) - G.Height(MX - 0.05f)) / 0.1f;
		C.NormalZ = 1.f / std::sqrt(1.f + Slope * Slope);
		C.Surface = G.Surface(MX);
		return C;
	}

	void Step(const FRiderInput& In, float Dt = 1.f / 120.f)
	{
		float RX, RZ, FX, FZ;
		MountPositions(RX, RZ, FX, FZ);
		const FWheelContact CR = Trace(RX, RZ, P.Rear.RestLength + 0.05f);
		const FWheelContact CF = Trace(FX, FZ, P.Front.RestLength + 0.05f);
		// Slope from the ground normals under the wheels that are actually touching
		// (the Unreal pawn does the same with its trace hit normals).
		auto SlopeAt = [this](float SX) { return std::atan((G.Height(SX + 0.05f) - G.Height(SX - 0.05f)) / 0.1f); };
		if (CR.bHit && CF.bHit) Env.SlopePitch = std::atan2(G.Height(FX) - G.Height(RX), std::max(0.1f, FX - RX));
		else if (CR.bHit) Env.SlopePitch = SlopeAt(RX);
		else if (CF.bHit) Env.SlopePitch = SlopeAt(FX);
		const FStepResult R = FRideSimulator::Step(P, A, In, CF, CR, Env, S, Dt);
		if (!S.bAirborne && !R.bLanded)
		{
			X += R.Forward * std::cos(S.GroundPitch);
			Z += R.Forward * std::sin(S.GroundPitch) + R.Up;
		}
		else
		{
			X += R.Forward;
			Z += R.Up;
		}
		// Never sink through the ground (the Unreal pawn uses a swept collision box for this)
		// (origin is the contact point at full extension, so it legitimately sits up to
		// one suspension travel below the surface when compressed)
		const float Floor = std::min(G.Height(X), G.Height(X + P.Wheelbase)) - 1.5f * std::max(P.Front.Travel, P.Rear.Travel);
		if (Z < Floor) { Z = Floor; S.VerticalSpeed = std::max(S.VerticalSpeed, 0.f); }

		Tricks.Update(P, S, In, R, Dt);
		for (auto& E : Tricks.ConsumeEvents()) Events.push_back(E);
		Time += Dt;
		MaxPitch = std::max(MaxPitch, S.Pitch);
		MinPitch = std::min(MinPitch, S.Pitch);
		MaxAirTime = std::max(MaxAirTime, S.AirTime);
		MaxLean = std::max(MaxLean, std::fabs(S.Lean));
		bEverCrashed |= S.bCrashed;
		bEverAirborne |= S.bAirborne;
	}

	void Run(float Seconds, const FRiderInput& In)
	{
		const int N = static_cast<int>(Seconds * 120.f);
		for (int i = 0; i < N; ++i) Step(In);
	}

	// Launch at a given speed by settling at rest then setting speed (like a respawn).
	void Launch(float Speed)
	{
		Run(1.0f, FRiderInput());
		S.Speed = Speed;
	}

	bool HasEvent(ETrick T) const
	{
		for (auto& E : Events) if (E.Trick == T) return true;
		return false;
	}
};

static const FVehicleParams& V(const char* Id)
{
	const FVehicleEntry* E = FVehicleCatalog::Find(Id);
	if (!E) { std::printf("missing vehicle %s\n", Id); std::exit(2); }
	return E->Params;
}

static bool Finite(const FRideState& S)
{
	return std::isfinite(S.Speed) && std::isfinite(S.Pitch) && std::isfinite(S.Lean) && std::isfinite(S.VerticalSpeed);
}

static void TestRest()
{
	std::printf("[rest] e-bike settles on its suspension\n");
	FHarness H(V("ebike_fullsus"));
	H.Run(3.f, FRiderInput());
	const float Sag = H.S.CompressionFront / H.P.Front.Travel;
	CHECK(!H.S.bAirborne && H.S.bFrontGrounded && H.S.bRearGrounded, "grounded F=%d R=%d", H.S.bFrontGrounded, H.S.bRearGrounded);
	CHECK(std::fabs(H.S.Speed) < 0.01f, "speed %.3f", H.S.Speed);
	CHECK(Sag > 0.15f && Sag < 0.40f, "front sag %.2f", Sag);
	CHECK(std::fabs(H.S.VerticalSpeed) < 0.05f, "vz %.3f", H.S.VerticalSpeed);
	CHECK(!H.bEverCrashed, "crashed");
}

static void TestReverse()
{
	std::printf("[reverse] holding reverse from a standstill backs up at walking pace, stays upright\n");
	for (const char* Id : { "ebike_hardtail", "bmx_street", "moto_sport1000", "escooter_commuter" })
	{
		FHarness H(V(Id));
		H.Run(1.f, FRiderInput());
		FRiderInput In; In.Reverse = 1.f;
		H.Run(4.f, In);
		std::printf("    %-18s speed %.2f m/s\n", Id, H.S.Speed);
		CHECK(H.S.Speed < -0.8f && H.S.Speed > -2.2f, "%s reverse speed %.2f", Id, H.S.Speed);
		CHECK(!H.bEverCrashed && Finite(H.S), "%s crashed while reversing", Id);
		H.Run(3.f, FRiderInput()); // let go: rolls to a stop
		CHECK(std::fabs(H.S.Speed) < 0.3f, "%s still moving %.2f after releasing reverse", Id, H.S.Speed);
	}
}

static void TestEBikeAssist()
{
	std::printf("[ebike] pedal assist cuts out at 15.5 mph, battery drains\n");
	FHarness H(V("ebike_commuter"));
	FRiderInput In; In.Throttle = 1.f; In.AssistLevel = 3;
	H.Run(4.f, In);
	const float At4 = H.S.Speed;
	H.Run(26.f, In);
	std::printf("    speed after 4 s %.2f m/s, after 30 s %.2f m/s (%.1f mph), battery %.1f Wh\n", At4, H.S.Speed, H.S.Speed * 2.237f, H.S.BatteryWh);
	CHECK(At4 > 4.5f, "assist should give brisk acceleration, got %.2f", At4);
	CHECK(H.S.Speed > 6.5f && H.S.Speed < 10.f, "cruise %.2f", H.S.Speed);
	CHECK(H.S.BatteryWh < H.P.Powertrain.BatteryWh, "battery not drained");

	FHarness NoAssist(V("ebike_commuter"));
	FRiderInput Eco = In; Eco.AssistLevel = 0;
	NoAssist.Run(4.f, Eco);
	CHECK(NoAssist.S.Speed < At4, "eco %.2f should be slower than turbo %.2f", NoAssist.S.Speed, At4);
}

static void TestBraking()
{
	std::printf("[brakes] stopping distance and surfaces\n");
	auto StopDist = [](const char* Id, ESurface Surf, float Wet, float V0) {
		FHarness H(V(Id));
		H.G.Surface = [Surf](float) { return Surf; };
		H.Env.Wetness = Wet;
		H.Launch(V0);
		const float X0 = H.X;
		FRiderInput In; In.FrontBrake = 1.f; In.RearBrake = 1.f;
		for (int i = 0; i < 120 * 10 && H.S.Speed > 0.05f; ++i) H.Step(In);
		return H.X - X0;
	};
	const float Dry = StopDist("mtb_trail", ESurface::Asphalt, 0.f, 8.f);
	const float Wet = StopDist("mtb_trail", ESurface::Asphalt, 1.f, 8.f);
	const float Grass = StopDist("mtb_trail", ESurface::Grass, 0.f, 8.f);
	const float Mud = StopDist("road_bike", ESurface::Mud, 0.f, 8.f);
	std::printf("    MTB 8 m/s: dry %.2f m, wet %.2f m, grass %.2f m; road bike in mud %.2f m\n", Dry, Wet, Grass, Mud);
	CHECK(Dry > 3.f && Dry < 9.f, "dry %.2f", Dry);
	CHECK(Wet > Dry * 1.1f, "wet %.2f vs dry %.2f", Wet, Dry);
	CHECK(Grass > Dry * 1.2f, "grass %.2f vs dry %.2f", Grass, Dry);
	CHECK(Mud > Grass, "mud on slicks %.2f should be worst", Mud);

	CHECK(SurfaceGrip(ESurface::Asphalt, ETyre::Road, 0.f) > SurfaceGrip(ESurface::Asphalt, ETyre::Road, 1.f), "wet grip");
	CHECK(SurfaceGrip(ESurface::Mud, ETyre::Motocross, 0.f) > SurfaceGrip(ESurface::Mud, ETyre::Road, 0.f), "tyre affinity");
}

static void TestCasualWheelie()
{
	std::printf("[wheelie] casual assist: hold the button and ride it out\n");
	for (const char* Id : { "bmx_street", "mtb_trail", "ebike_hardtail", "dirt_mx250", "moto_naked700", "escooter_performance" })
	{
		FHarness H(V(Id));
		H.A.WheelieAssist = EWheelieAssist::Casual;
		H.Launch(5.f);
		FRiderInput In; In.Throttle = 0.35f; In.bWheelie = true;
		int InWheelie = 0, Samples = 0;
		for (int i = 0; i < 120 * 6; ++i)
		{
			H.Step(In);
			if (H.Time > 3.0f) { ++Samples; if (H.Tricks.IsWheelieActive()) ++InWheelie; }
		}
		const float Bp = WheelieBalanceAngle(H.P);
		const float HeldPitch = H.S.Pitch;
		FRiderInput Release; Release.Throttle = 0.2f;
		H.Run(3.f, Release);
		const float Frac = Samples ? static_cast<float>(InWheelie) / Samples : 0.f;
		std::printf("    %-22s balance pt %.2f rad, held at %.2f, in wheelie %.0f%%, crashed=%d, events=%zu\n",
			Id, Bp, HeldPitch, Frac * 100.f, H.bEverCrashed, H.Events.size());
		CHECK(!H.bEverCrashed, "%s crashed in casual mode", Id);
		CHECK(Frac > 0.9f, "%s only %.0f%% wheelie", Id, Frac * 100.f);
		CHECK(H.S.Pitch < 0.05f, "%s nose should come down on release (pitch %.2f)", Id, H.S.Pitch);
		CHECK(H.HasEvent(ETrick::Wheelie), "%s no wheelie scored", Id);
	}
	FHarness B(V("bmx_street"));
	B.Launch(4.f);
	FRiderInput Manual; Manual.bWheelie = true;
	B.Run(2.f, Manual);
	B.Run(2.f, FRiderInput());
	CHECK(B.HasEvent(ETrick::Manual), "unpowered BMX wheelie should score as a manual");
}

static void TestRealisticWheelie()
{
	std::printf("[wheelie] realistic: throttle wheelies on powerful bikes, loop-outs possible\n");
	FHarness Sport(V("moto_sport1000"));
	Sport.A.WheelieAssist = EWheelieAssist::Realistic;
	Sport.A.bAutoClutch = true;
	Sport.Launch(4.f);
	FRiderInput Full; Full.Throttle = 1.f; Full.Balance = 0.5f;
	Sport.Run(5.f, Full);
	std::printf("    RR1000 full throttle: max pitch %.2f rad, crashed=%d, speed %.1f m/s gear %d rpm %.0f\n",
		Sport.MaxPitch, Sport.bEverCrashed, Sport.S.Speed, Sport.S.Gear, Sport.S.EngineRpm);
	CHECK(Sport.MaxPitch > 0.15f, "superbike should lift the front under full throttle");

	FHarness Commuter(V("moto_commuter125"));
	Commuter.A.WheelieAssist = EWheelieAssist::Realistic;
	Commuter.Launch(4.f);
	Commuter.Run(4.f, Full);
	std::printf("    125 commuter full throttle: max pitch %.2f rad\n", Commuter.MaxPitch);
	CHECK(Commuter.MaxPitch < 0.05f, "125 should not power-wheelie without a clutch dump");

	// Clutch dump on the 125: rev it with the clutch in, then drop the clutch.
	FHarness Dump(V("moto_commuter125"));
	Dump.A.WheelieAssist = EWheelieAssist::Realistic;
	Dump.A.bAutoClutch = false;
	Dump.Launch(3.f);
	FRiderInput Rev; Rev.Throttle = 1.f; Rev.Clutch = 1.f; Rev.Balance = 1.f; Rev.bWheelie = true;
	Dump.Run(0.6f, Rev);
	FRiderInput Drop = Rev; Drop.Clutch = 0.f;
	Dump.Run(0.8f, Drop);
	std::printf("    125 clutch dump + weight back: max pitch %.2f rad\n", Dump.MaxPitch);
	CHECK(Dump.MaxPitch > 0.05f, "clutch dump should pop the front");

	FHarness Loop(V("bmx_street"));
	Loop.A.WheelieAssist = EWheelieAssist::Realistic;
	Loop.Launch(4.f);
	FRiderInput Yank; Yank.bWheelie = true; Yank.Balance = 1.f; Yank.Throttle = 1.f;
	Loop.Run(3.f, Yank);
	std::printf("    BMX realistic over-pull: crashed=%d max pitch %.2f\n", Loop.bEverCrashed, Loop.MaxPitch);
	CHECK(Loop.bEverCrashed, "over-pulling a BMX in realistic mode should loop out");

	FHarness Safe(V("bmx_street"));
	Safe.A.WheelieAssist = EWheelieAssist::Casual;
	Safe.Launch(4.f);
	Safe.Run(3.f, Yank);
	CHECK(!Safe.bEverCrashed, "casual mode must prevent loop-outs");
}

static void TestStoppie()
{
	std::printf("[stoppie] hard front brake lifts the rear on a motocross bike\n");
	FHarness H(V("dirt_mx250"));
	H.A.WheelieAssist = EWheelieAssist::Balanced;
	H.A.bABS = true;
	H.Launch(12.f);
	FRiderInput In; In.FrontBrake = 1.f; In.Balance = -1.f;
	for (int i = 0; i < 120 * 4 && H.S.Speed > 0.1f; ++i) H.Step(In);
	std::printf("    min pitch %.2f rad, crashed=%d\n", H.MinPitch, H.bEverCrashed);
	CHECK(H.MinPitch < -0.05f, "expected a stoppie");
}

static void TestJumpAndHop()
{
	std::printf("[air] ramp jump and bunny hop\n");
	FHarness H(V("bmx_street"));
	H.A.WheelieAssist = EWheelieAssist::Casual;
	// Kicker: flat, 1.2 m rise over 2.5 m starting at x = 15, then a drop back to 0
	H.G.Height = [](float X) { return (X > 15.f && X < 17.5f) ? (X - 15.f) * 0.48f : 0.f; };
	H.Launch(8.5f);
	FRiderInput Coast;
	H.Run(5.f, Coast);
	std::printf("    kicker: max air %.2f s, crashed=%d, x=%.1f, events=%zu\n", H.MaxAirTime, H.bEverCrashed, H.X, H.Events.size());
	for (auto& E : H.Events) std::printf("      %s (+%d)\n", E.Label.c_str(), E.Points);
	CHECK(H.MaxAirTime > 0.4f, "should get air off the kicker");
	CHECK(!H.bEverCrashed, "casual landing assist should land it");
	CHECK(H.HasEvent(ETrick::Air), "air should score");

	FHarness Hop(V("bmx_street"));
	Hop.Launch(4.f);
	FRiderInput Press; Press.bHop = true;
	Hop.Run(0.2f, Press);
	Hop.Run(2.f, FRiderInput());
	std::printf("    bunny hop: air %.2f s, crashed=%d\n", Hop.MaxAirTime, Hop.bEverCrashed);
	CHECK(Hop.MaxAirTime > 0.3f, "hop air time %.2f", Hop.MaxAirTime);
	CHECK(!Hop.bEverCrashed, "hop crashed");
	CHECK(Hop.HasEvent(ETrick::BunnyHop), "hop not scored");

	FHarness Scoot(V("escooter_commuter"));
	Scoot.Launch(5.f);
	Scoot.Run(0.2f, Press);
	Scoot.Run(2.f, FRiderInput());
	CHECK(Scoot.MaxAirTime < Hop.MaxAirTime, "scooter hop should be smaller than BMX");

	FHarness Spin(V("bmx_street"));
	Spin.A.WheelieAssist = EWheelieAssist::Casual;
	Spin.G.Height = [](float X) { return (X > 15.f && X < 18.f) ? (X - 15.f) * 0.6f : 0.f; };
	Spin.Launch(10.f);
	FRiderInput Turn; Turn.Steer = 1.f;
	for (int i = 0; i < 120 * 6; ++i) Spin.Step(Spin.S.bAirborne ? Turn : Coast);
	std::printf("    kicker with spin input: events=%zu crashed=%d\n", Spin.Events.size(), Spin.bEverCrashed);
	for (auto& E : Spin.Events) std::printf("      %s (+%d)\n", E.Label.c_str(), E.Points);
}

static void TestCornering()
{
	std::printf("[corner] lean and yaw follow steering; grip limit\n");
	FHarness H(V("moto_naked700"));
	H.Launch(15.f);
	FRiderInput In; In.Steer = 0.4f; In.Throttle = 0.25f;
	H.Run(3.f, In);
	std::printf("    naked 15 m/s steer 0.4: lean %.2f rad yaw rate %.2f rad/s crashed=%d\n", H.S.Lean, H.S.YawRate, H.bEverCrashed);
	CHECK(H.S.Lean > 0.1f && H.S.YawRate > 0.05f, "should lean into the turn");
	CHECK(!H.bEverCrashed, "crashed in a moderate turn");

	FHarness Wet(V("road_bike"));
	Wet.Env.Wetness = 1.f;
	Wet.G.Surface = [](float) { return ESurface::Cobbles; };
	Wet.A.WheelieAssist = EWheelieAssist::Realistic;
	Wet.Launch(9.f);
	FRiderInput Hard; Hard.Steer = 1.f;
	Wet.Run(2.f, Hard);
	std::printf("    road bike on wet cobbles full lock at 9 m/s: crashed=%d\n", Wet.bEverCrashed);
}

static void TestSlide()
{
	std::printf("[slide] rear brake skid + steer on dirt\n");
	FHarness H(V("dirt_mx250"));
	H.G.Surface = [](float) { return ESurface::Dirt; };
	H.Launch(11.f);
	FRiderInput In; In.RearBrake = 1.f; In.Steer = 0.6f; In.Throttle = 0.f;
	float MaxLat = 0.f;
	for (int i = 0; i < 120 * 2; ++i) { H.Step(In); MaxLat = std::max(MaxLat, std::fabs(H.S.LateralSpeed)); }
	H.Run(3.f, FRiderInput());
	std::printf("    max lateral %.2f m/s, events=%zu\n", MaxLat, H.Events.size());
	CHECK(MaxLat > 1.f, "rear should step out");
	CHECK(H.HasEvent(ETrick::Slide), "slide not scored");
}

static void TestWholeCatalogue()
{
	std::printf("[catalogue] every vehicle: stable at rest, accelerates, no NaNs\n");
	std::printf("    %-22s %-17s %6s %8s %8s %6s\n", "id", "category", "mass", "v@10s", "v@40s", "sag");
	size_t Index = 0;
	for (const FVehicleEntry& E : FVehicleCatalog::All())
	{
		const bool bTuned = Index++ < 17; // print the hand-tuned reference vehicles; the generated lineup only on failure
		FHarness H(E.Params);
		H.Run(2.f, FRiderInput());
		const float Sag = E.Params.Front.Travel > 0.f ? H.S.CompressionFront / E.Params.Front.Travel : 0.f;
		const bool bRestOk = !H.S.bAirborne && std::fabs(H.S.Speed) < 0.01f;
		FRiderInput In; In.Throttle = 1.f; In.AssistLevel = 3;
		H.A.bAutoShift = true;
		H.Run(10.f, In);
		const float V10 = H.S.Speed;
		H.Run(30.f, In);
		const bool bBad = !bRestOk || !Finite(H.S) || H.S.Speed <= 4.f || H.S.Speed > E.Params.TopSpeed + 0.5f || H.bEverCrashed;
		if (bTuned || bBad)
			std::printf("    %-22s %-17s %6.0f %6.1f mph %6.1f mph %5.2f%s\n", E.Id.c_str(), FVehicleCatalog::CategoryName(E.Params.Category),
				E.Params.Mass, V10 * 2.237f, H.S.Speed * 2.237f, Sag, H.bEverCrashed ? "  CRASHED" : "");
		CHECK(bRestOk, "%s not stable at rest", E.Id.c_str());
		CHECK(Finite(H.S), "%s NaN state", E.Id.c_str());
		CHECK(H.S.Speed > 4.f, "%s too slow %.2f", E.Id.c_str(), H.S.Speed);
		CHECK(H.S.Speed <= E.Params.TopSpeed + 0.5f, "%s exceeds governor", E.Id.c_str());
		CHECK(!H.bEverCrashed, "%s crashed while accelerating in a straight line", E.Id.c_str());
	}
}

static void TestLineup()
{
	std::printf("[lineup] hundreds of vehicles, unique ids/names, every one holds a casual wheelie\n");
	const auto& All = FVehicleCatalog::All();
	int PerCat[8] = {};
	std::vector<std::string> Ids, Names;
	for (const FVehicleEntry& E : All)
	{
		Ids.push_back(E.Id); Names.push_back(E.DisplayName);
		++PerCat[static_cast<int>(E.Params.Category)];
	}
	std::sort(Ids.begin(), Ids.end()); std::sort(Names.begin(), Names.end());
	CHECK(std::adjacent_find(Ids.begin(), Ids.end()) == Ids.end(), "duplicate vehicle id");
	CHECK(std::adjacent_find(Names.begin(), Names.end()) == Names.end(), "duplicate vehicle name");
	CHECK(All.size() >= 400, "only %zu vehicles", All.size());
	std::printf("    %zu vehicles:", All.size());
	for (int c = 0; c < 8; ++c)
	{
		std::printf(" %s %d%s", FVehicleCatalog::CategoryName(static_cast<EVehicleCategory>(c)), PerCat[c], c < 7 ? "," : "\n");
		CHECK(PerCat[c] >= 20, "category %d has only %d vehicles", c, PerCat[c]);
	}

	int Failed = 0;
	for (const FVehicleEntry& E : All)
	{
		FHarness H(E.Params);
		H.A.WheelieAssist = EWheelieAssist::Casual;
		H.Launch(std::min(5.f, E.Params.TopSpeed * 0.6f));
		FRiderInput In; In.Throttle = 0.35f; In.bWheelie = true;
		int InWheelie = 0, Samples = 0;
		for (int i = 0; i < 120 * 6; ++i)
		{
			H.Step(In);
			if (H.Time > 3.0f) { ++Samples; if (H.Tricks.IsWheelieActive()) ++InWheelie; }
		}
		FRiderInput Release; Release.Throttle = 0.2f;
		H.Run(3.f, Release);
		const float Frac = Samples ? static_cast<float>(InWheelie) / Samples : 0.f;
		const bool Ok = !H.bEverCrashed && Frac > 0.9f && H.S.Pitch < 0.05f && Finite(H.S);
		if (!Ok)
		{
			++Failed;
			std::printf("    FAIL %-34s wheelie %3.0f%% crashed=%d pitch after release %.2f\n", E.Id.c_str(), Frac * 100.f, H.bEverCrashed, H.S.Pitch);
		}
	}
	CHECK(Failed == 0, "%d vehicles failed the casual wheelie check", Failed);
}

static void TestDeterminism()
{
	std::printf("[determinism] identical inputs give identical state (needed for net replay)\n");
	auto Run = []() {
		FHarness H(V("ebike_fullsus"));
		H.G.Height = [](float X) { return 0.3f * std::sin(X * 0.4f); };
		for (int i = 0; i < 1200; ++i)
		{
			FRiderInput In; In.Throttle = 0.6f + 0.4f * std::sin(i * 0.01f); In.Steer = std::sin(i * 0.02f) * 0.5f; In.bWheelie = (i / 200) % 2 == 0;
			H.Step(In);
		}
		return H.S;
	};
	const FRideState A = Run(), B = Run();
	CHECK(A.Speed == B.Speed && A.Pitch == B.Pitch && A.Lean == B.Lean && A.BatteryWh == B.BatteryWh, "non-deterministic");
}

static void TestGeo()
{
	std::printf("[geo] C++ projection matches the pipeline (Tools/geo_pipeline/geo.mjs)\n");
	const MRGeo::FEastNorth O = MRGeo::ToLocal({ MRGeo::OriginLat, MRGeo::OriginLon });
	CHECK(std::fabs(O.East) < 1e-6 && std::fabs(O.North) < 1e-6, "origin %.6f %.6f", O.East, O.North);
	// Reference printed by `node test_geo.mjs`: Piccadilly Gardens east=522.4793 north=181.4402
	const MRGeo::FEastNorth P = MRGeo::ToLocal({ 53.48099, -2.23697 });
	CHECK(std::fabs(P.East - 522.4793) < 0.001 && std::fabs(P.North - 181.4402) < 0.001, "Piccadilly %.4f %.4f", P.East, P.North);
	const MRGeo::FEastNorth W = MRGeo::ToLocal({ 53.54490, -2.63130 }); // Wigan
	const MRGeo::FLatLon Back = MRGeo::ToLatLon(W);
	CHECK(std::fabs(Back.Lat - 53.54490) < 1e-7 && std::fabs(Back.Lon + 2.63130) < 1e-7, "round trip %.8f %.8f", Back.Lat, Back.Lon);
	double UX, UY;
	MRGeo::LocalToUnreal(P, UX, UY);
	CHECK(UX > 0 && UY < 0, "Unreal axes: east = +X, north = -Y");
}

static void DebugTrace(const char* Id, bool bKicker)
{
	FHarness H(V(Id));
	FRiderInput In;
	if (bKicker)
	{
		H.G.Height = [](float X) { return (X > 15.f && X < 17.5f) ? (X - 15.f) * 0.48f : 0.f; };
		H.Launch(8.5f);
	}
	for (int i = 0; i < 120 * 6; ++i)
	{
		if (i == 240 && !bKicker) In.Throttle = 1.f;
		H.Step(In);
		if (i % (bKicker ? 6 : 24) == 0)
			std::printf("t=%.2f x=%.2f v=%.2f vz=%.2f z=%.3f cF=%.3f cR=%.3f NF=%.0f NR=%.0f air=%d pitch=%.2f rpm=%.0f gear=%d clutch=%.2f F=%.0f\n",
				H.Time, H.X, H.S.Speed, H.S.VerticalSpeed, H.Z, H.S.CompressionFront, H.S.CompressionRear, H.S.LoadFront, H.S.LoadRear,
				H.S.bAirborne, H.S.Pitch, H.S.EngineRpm, H.S.Gear, H.S.ClutchEngage, H.S.DriveForce);
	}
}

int main(int Argc, char** Argv)
{
	if (Argc > 2 && std::string(Argv[1]) == "trace") { DebugTrace(Argv[2], Argc > 3 && std::string(Argv[3]) == "kicker"); return 0; }
	TestRest();
	TestEBikeAssist();
	TestReverse();
	TestBraking();
	TestCasualWheelie();
	TestRealisticWheelie();
	TestStoppie();
	TestJumpAndHop();
	TestCornering();
	TestSlide();
	TestWholeCatalogue();
	TestLineup();
	TestDeterminism();
	TestGeo();
	std::printf("\n%d checks, %d failures\n", GChecks, GFailures);
	return GFailures == 0 ? 0 : 1;
}
