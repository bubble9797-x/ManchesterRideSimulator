#include "VehicleCatalog.h"

#include <cmath>

namespace MRSim
{
	namespace
	{
		// Derive spring rate and damping from a target static sag so every vehicle's
		// suspension is consistent with its mass and weight distribution.
		FSuspensionParams MakeSuspension(float Mass, float LoadShare, float WheelRadius, float Travel, float SagFraction, float DampingRatio)
		{
			FSuspensionParams S;
			const float Load = Mass * Gravity * LoadShare;
			S.Travel = Travel;
			S.RestLength = WheelRadius + Travel;
			S.Stiffness = Load / (SagFraction * Travel);
			const float Critical = 2.f * std::sqrt(S.Stiffness * Mass * LoadShare);
			S.CompressionDamping = Critical * DampingRatio * 0.8f;
			S.ReboundDamping = Critical * DampingRatio * 1.1f;
			return S;
		}

		void FinishGeometry(FVehicleParams& P, float FrontTravel, float RearTravel, float Sag, float Zeta)
		{
			const float FrontShare = P.ComFromRear / P.Wheelbase;
			P.Front = MakeSuspension(P.Mass, FrontShare, P.WheelRadiusFront, FrontTravel, Sag, Zeta);
			P.Rear = MakeSuspension(P.Mass, 1.f - FrontShare, P.WheelRadiusRear, RearTravel, Sag, Zeta);
		}

		FVehicleEntry Bicycle(const char* Id, const char* Name, const char* Desc, EVehicleCategory Cat, ETyre Tyre,
			float Mass, float Wheelbase, float ComH, float ComRear, float WheelR, float TravelF, float TravelR,
			float HopSpeed, float Lift, float CdA)
		{
			FVehicleEntry E{ Id, Name, Desc, {} };
			FVehicleParams& P = E.Params;
			P.Category = Cat;
			P.Tyre = Tyre;
			P.Mass = Mass;
			P.Wheelbase = Wheelbase;
			P.ComHeight = ComH;
			P.ComFromRear = ComRear;
			P.WheelRadiusFront = P.WheelRadiusRear = WheelR;
			P.CdA = CdA;
			P.TopSpeed = 25.f;
			P.MaxBrakeForceFront = Mass * 7.5f;
			P.MaxBrakeForceRear = Mass * 4.5f;
			P.Powertrain.Type = EPowertrain::Pedal;
			P.Wheelie.BunnyHopSpeed = HopSpeed;
			P.Wheelie.RiderLiftTorque = Lift;
			P.Wheelie.RiderBalanceTorque = Lift * 0.6f;
			// Rigid forks/frames still have tyre compliance: model as a tiny travel.
			FinishGeometry(P, TravelF > 0.f ? TravelF : 0.02f, TravelR > 0.f ? TravelR : 0.015f, TravelF > 0.f ? 0.25f : 0.3f, 0.45f);
			return E;
		}

		FVehicleEntry Motor(const char* Id, const char* Name, const char* Desc, EVehicleCategory Cat, ETyre Tyre,
			float Mass, float Wheelbase, float ComH, float ComRear, float WheelRF, float WheelRR, float TravelF, float TravelR,
			float TorqueNm, float PeakRpm, float IdleRpm, float Redline, int Gears, const float* Ratios, float FinalDrive /* primary x secondary */,
			float FuelL, float TopSpeed, float Lift)
		{
			FVehicleEntry E{ Id, Name, Desc, {} };
			FVehicleParams& P = E.Params;
			P.Category = Cat;
			P.Tyre = Tyre;
			P.Mass = Mass;
			P.Wheelbase = Wheelbase;
			P.ComHeight = ComH;
			P.ComFromRear = ComRear;
			P.WheelRadiusFront = WheelRF;
			P.WheelRadiusRear = WheelRR;
			P.PitchRadiusOfGyration = 0.5f;
			P.MaxSteerAngle = 0.6f;
			P.SteerSpeedRef = 9.f;
			P.LeanResponse = 4.5f;
			P.MaxLean = 0.95f;
			P.RollingResistance = 0.015f;
			P.CdA = 0.6f;
			P.TopSpeed = TopSpeed;
			P.MaxBrakeForceFront = Mass * 10.5f;
			P.MaxBrakeForceRear = Mass * 4.f;
			FPowertrainParams& PT = P.Powertrain;
			PT.Type = EPowertrain::Combustion;
			PT.PeakTorqueNm = TorqueNm;
			PT.PeakTorqueRpm = PeakRpm;
			PT.IdleRpm = IdleRpm;
			PT.RedlineRpm = Redline;
			PT.NumGears = Gears;
			for (int i = 0; i < Gears && i < 6; ++i) PT.GearRatios[i] = Ratios[i];
			PT.FinalDrive = FinalDrive;
			PT.FuelLitres = FuelL;
			PT.EngineInertia = 0.02f + TorqueNm * 0.0008f;
			P.Wheelie.RiderLiftTorque = Lift;
			P.Wheelie.RiderBalanceTorque = Lift * 0.8f;
			P.Wheelie.BunnyHopSpeed = Cat == EVehicleCategory::DirtBike ? 1.2f : 0.f;
			P.Wheelie.LoopOutMargin = 0.3f;
			FinishGeometry(P, TravelF, TravelR, 0.3f, 0.5f);
			return E;
		}

		// =====================================================================================
		// Full lineup: hundreds of models across every category, all free.
		//
		// Each model is a real-world type (XC hardtail, enduro, cargo e-bike, 450F, A2 twin...)
		// with realistic mass (bike + 75 kg rider unless noted), geometry, travel, power and
		// gearing, built through the same constructors as the hand-tuned vehicles above so the
		// suspension, brakes and wheelie behaviour stay consistent. Each brand slot applies
		// small, deterministic differences (a little lighter/heavier, livelier/calmer) so
		// same-type models genuinely ride differently. Display names are real production
		// models of that type (tables below); the riding figures are modelled on the class
		// of machine, not taken from manufacturer data. Ids keep the original slot tags so
		// saved profiles stay valid. Every generated vehicle runs through the full SimTests
		// catalogue checks (stable at rest, accelerates, respects its governor, holds a
		// casual-assist wheelie without crashing).
		// =====================================================================================

		struct FBrand { const char* Name; const char* IdTag; float MassF; float PowerF; float ComF; float WbAdd; };

		const FBrand CycleBrands[] = {
			{ "Irk", "irk", 1.00f, 1.00f, 1.00f, 0.00f }, { "Medlock", "medlock", 0.98f, 1.03f, 0.99f, -0.01f },
			{ "Tame Valley", "tame", 1.02f, 0.97f, 1.01f, 0.01f }, { "Kinder", "kinder", 0.97f, 1.05f, 1.00f, 0.00f },
			{ "Holcombe", "holcombe", 1.01f, 1.00f, 0.98f, 0.02f }, { "Etherow", "etherow", 0.99f, 0.98f, 1.02f, -0.01f },
		};
		const FBrand MotorBrands[] = {
			{ "Pendle", "pendle", 1.00f, 1.00f, 1.00f, 0.00f }, { "Roch Works", "roch", 0.97f, 1.05f, 1.01f, -0.01f },
			{ "Ordsall", "ordsall", 1.03f, 0.96f, 0.99f, 0.02f }, { "Goyt", "goyt", 0.99f, 1.02f, 1.00f, 0.01f },
		};

		std::string MakeId(const char* Prefix, const FBrand& B, const char* Model)
		{
			std::string Id = std::string(Prefix) + "_" + B.IdTag + "_";
			for (const char* C = Model; *C; ++C)
			{
				const char Ch = *C;
				if ((Ch >= 'a' && Ch <= 'z') || (Ch >= '0' && Ch <= '9')) Id += Ch;
				else if (Ch >= 'A' && Ch <= 'Z') Id += static_cast<char>(Ch - 'A' + 'a');
				else if (!Id.empty() && Id.back() != '_') Id += '_';
			}
			while (!Id.empty() && Id.back() == '_') Id.pop_back();
			return Id;
		}

		// Pedal bicycle model type.
		struct FCycleModel
		{
			const char* Name; const char* Desc; EVehicleCategory Cat; ETyre Tyre;
			float Mass, Wb, ComH, ComR, WheelR, TF, TR, Hop, Lift, CdA;
		};
		// E-bike model type (UK EAPC unless noted).
		struct FEBikeModel
		{
			const char* Name; const char* Desc; ETyre Tyre;
			float Mass, Wb, ComH, ComR, WheelR, TF, TR, Force, Power, Wh, Hop, Lift;
		};
		struct FScooterModel
		{
			const char* Name; const char* Desc; ETyre Tyre;
			float Mass, Wb, ComH, WheelR, TF, TR, PowerW, ForceN, TopMs, Wh, Lift;
		};
		// Combustion model type: gearing is derived from the target top speed so redline in top gear ~= top speed.
		struct FMotorModel
		{
			const char* Name; const char* Desc; EVehicleCategory Cat; ETyre Tyre;
			float Mass, Wb, ComH, ComR, WheelRF, WheelRR, TF, TR;
			float TorqueNm, PeakRpm, IdleRpm, Redline; int Gears; float TopMs, FuelL, Lift;
		};
		// Electric motorcycle / dirt bike.
		struct FEMotorModel
		{
			const char* Name; const char* Desc; EVehicleCategory Cat; ETyre Tyre;
			float Mass, Wb, ComH, ComR, WheelRF, WheelRR, TF, TR, PowerW, ForceN, TopMs, Wh, Lift;
		};

		const FCycleModel CycleModels[] = {
			// BMX / freestyle
			{ "Park 20", "20-inch park BMX: short rear end, light and snappy.", EVehicleCategory::BMX, ETyre::Knobbly, 85.f, 0.97f, 0.95f, 0.37f, 0.255f, 0, 0, 3.2f, 300.f, 0.5f },
			{ "Street 21", "Street BMX with a longer top tube for stability on big drops.", EVehicleCategory::BMX, ETyre::Knobbly, 87.f, 1.00f, 0.96f, 0.39f, 0.255f, 0, 0, 3.1f, 300.f, 0.5f },
			{ "Race Pro", "Alloy race BMX: stiff, light, fast gate starts.", EVehicleCategory::BMX, ETyre::Hybrid, 83.f, 0.99f, 0.97f, 0.40f, 0.255f, 0, 0, 3.0f, 290.f, 0.46f },
			{ "Flatland", "Flatland BMX: tiny wheelbase, low and balanced for spins and manuals.", EVehicleCategory::BMX, ETyre::Hybrid, 84.f, 0.93f, 0.92f, 0.36f, 0.255f, 0, 0, 3.0f, 290.f, 0.5f },
			{ "Cruiser 24", "24-inch BMX cruiser: rolls over rough ground, still jumps.", EVehicleCategory::BMX, ETyre::Knobbly, 89.f, 1.06f, 0.98f, 0.42f, 0.31f, 0, 0, 2.9f, 290.f, 0.52f },
			{ "Junior 16", "16-inch kids' BMX (lighter rider).", EVehicleCategory::BMX, ETyre::Knobbly, 52.f, 0.82f, 0.78f, 0.31f, 0.205f, 0, 0, 2.6f, 170.f, 0.32f },
			{ "Dirt 26", "26-inch dirt jumper with a 100 mm fork.", EVehicleCategory::BMX, ETyre::Knobbly, 89.f, 1.04f, 0.97f, 0.40f, 0.33f, 0.10f, 0, 3.0f, 290.f, 0.5f },
			{ "Slope 24", "Slopestyle hardtail, 24-inch wheels, steep and playful.", EVehicleCategory::BMX, ETyre::Knobbly, 88.f, 1.01f, 0.96f, 0.39f, 0.31f, 0.10f, 0, 3.0f, 295.f, 0.5f },
			// Mountain bikes
			{ "XC 100 HT", "Cross-country race hardtail, 100 mm fork, 29-inch wheels.", EVehicleCategory::MountainBike, ETyre::Knobbly, 86.f, 1.13f, 1.04f, 0.46f, 0.37f, 0.10f, 0, 2.4f, 240.f, 0.48f },
			{ "XC 100 FS", "Full-suspension XC bike, 100 mm front and rear.", EVehicleCategory::MountainBike, ETyre::Knobbly, 87.f, 1.15f, 1.04f, 0.47f, 0.37f, 0.10f, 0.10f, 2.4f, 240.f, 0.48f },
			{ "Trail 130 HT", "Trail hardtail, 130 mm fork, steel frame feel.", EVehicleCategory::MountainBike, ETyre::Knobbly, 90.f, 1.19f, 1.03f, 0.48f, 0.37f, 0.13f, 0, 2.6f, 250.f, 0.55f },
			{ "Trail 140", "Trail full-suspension, 140 mm, the all-rounder.", EVehicleCategory::MountainBike, ETyre::Knobbly, 92.f, 1.21f, 1.03f, 0.49f, 0.37f, 0.14f, 0.14f, 2.6f, 250.f, 0.55f },
			{ "All-Mountain 150", "150 mm all-mountain bike on 27.5-inch wheels: agile and poppy.", EVehicleCategory::MountainBike, ETyre::Knobbly, 92.f, 1.19f, 1.02f, 0.49f, 0.35f, 0.15f, 0.15f, 2.7f, 255.f, 0.56f },
			{ "Enduro 170", "Enduro race bike, 170 mm, mixed wheels.", EVehicleCategory::MountainBike, ETyre::Knobbly, 94.f, 1.26f, 1.02f, 0.52f, 0.36f, 0.17f, 0.17f, 2.5f, 255.f, 0.58f },
			{ "Fat 4.8", "Fat bike with 4.8-inch tyres: floats over mud and sand.", EVehicleCategory::MountainBike, ETyre::Knobbly, 94.f, 1.17f, 1.04f, 0.47f, 0.38f, 0.02f, 0, 2.2f, 240.f, 0.6f },
			{ "Kids 24 Trail", "24-inch kids' trail bike (lighter rider).", EVehicleCategory::MountainBike, ETyre::Knobbly, 55.f, 0.98f, 0.82f, 0.38f, 0.31f, 0.10f, 0, 2.3f, 170.f, 0.38f },
			// Road / gravel / urban
			{ "Aero Disc", "Aero road race bike, deep wheels, 25 mm slicks.", EVehicleCategory::RoadBike, ETyre::Road, 82.f, 0.99f, 1.00f, 0.42f, 0.34f, 0, 0, 1.8f, 190.f, 0.30f },
			{ "Endurance", "Endurance road bike: taller front, 30 mm tyres, comfy all day.", EVehicleCategory::RoadBike, ETyre::Road, 84.f, 1.01f, 1.02f, 0.43f, 0.345f, 0, 0, 1.8f, 190.f, 0.36f },
			{ "Climber SL", "Featherweight climbing bike.", EVehicleCategory::RoadBike, ETyre::Road, 81.f, 0.98f, 1.00f, 0.42f, 0.34f, 0, 0, 1.9f, 195.f, 0.33f },
			{ "Gravel 40", "Gravel bike with 40 mm tyres: tarmac, towpaths and bridleways.", EVehicleCategory::RoadBike, ETyre::Hybrid, 85.f, 1.03f, 1.01f, 0.44f, 0.35f, 0, 0, 2.0f, 200.f, 0.38f },
			{ "Cyclocross", "Cyclocross race bike, knobbly 33 mm tyres for mud.", EVehicleCategory::RoadBike, ETyre::Knobbly, 84.f, 1.00f, 1.01f, 0.43f, 0.345f, 0, 0, 2.1f, 205.f, 0.38f },
			{ "Fixie", "Single-speed urban fixie: simple, quick, skid-stops.", EVehicleCategory::RoadBike, ETyre::Road, 83.f, 0.97f, 1.00f, 0.42f, 0.34f, 0, 0, 1.9f, 200.f, 0.38f },
			{ "Tourer", "Steel touring bike with racks and panniers.", EVehicleCategory::RoadBike, ETyre::Hybrid, 95.f, 1.06f, 1.03f, 0.45f, 0.35f, 0, 0, 1.4f, 180.f, 0.45f },
			{ "City Hybrid", "Flat-bar hybrid for the commute.", EVehicleCategory::RoadBike, ETyre::Hybrid, 88.f, 1.05f, 1.05f, 0.45f, 0.35f, 0.05f, 0, 1.7f, 200.f, 0.50f },
			// Downhill / freeride
			{ "DH 29", "29-inch World Cup downhill bike, 200 mm.", EVehicleCategory::DownhillBike, ETyre::Knobbly, 97.f, 1.31f, 1.00f, 0.56f, 0.37f, 0.20f, 0.20f, 2.3f, 240.f, 0.6f },
			{ "DH Mullet", "Downhill bike with a 27.5-inch rear: easier to throw around.", EVehicleCategory::DownhillBike, ETyre::Knobbly, 96.f, 1.29f, 1.00f, 0.55f, 0.36f, 0.20f, 0.20f, 2.4f, 245.f, 0.6f },
			{ "Freeride 180", "Freeride bike, 180 mm: big jumps and drops.", EVehicleCategory::DownhillBike, ETyre::Knobbly, 95.f, 1.27f, 1.00f, 0.54f, 0.36f, 0.18f, 0.18f, 2.6f, 250.f, 0.6f },
			{ "Bike Park 160", "Park bike, coil shock, 160 mm: laps all day.", EVehicleCategory::DownhillBike, ETyre::Knobbly, 94.f, 1.25f, 1.01f, 0.53f, 0.36f, 0.16f, 0.16f, 2.6f, 250.f, 0.58f },
			{ "Junior DH", "Downhill bike for younger riders (lighter rider).", EVehicleCategory::DownhillBike, ETyre::Knobbly, 66.f, 1.15f, 0.86f, 0.50f, 0.33f, 0.18f, 0.18f, 2.3f, 190.f, 0.45f },
		};

		const FEBikeModel EBikeModels[] = {
			{ "City Hub-e", "Commuter e-bike with a 40 Nm rear hub motor and rack.", ETyre::Hybrid, 101.f, 1.11f, 1.02f, 0.46f, 0.35f, 0.05f, 0, 130.f, 400.f, 418.f, 1.2f, 225.f },
			{ "Step-Through-e", "Low step-through frame, upright and easy to mount.", ETyre::Hybrid, 102.f, 1.12f, 1.00f, 0.46f, 0.35f, 0.06f, 0, 140.f, 450.f, 500.f, 1.1f, 220.f },
			{ "Trekking-e", "Trekking e-bike, mid-drive, 625 Wh: long days out.", ETyre::Hybrid, 104.f, 1.14f, 1.03f, 0.47f, 0.35f, 0.08f, 0, 200.f, 550.f, 625.f, 1.3f, 235.f },
			{ "Cargo Longtail-e", "Longtail cargo e-bike: heavy, long, very stable.", ETyre::Hybrid, 125.f, 1.40f, 1.00f, 0.60f, 0.31f, 0.06f, 0, 260.f, 650.f, 750.f, 0.8f, 230.f },
			{ "Folder-e", "Folding e-bike with 16-inch wheels.", ETyre::Hybrid, 94.f, 0.98f, 1.00f, 0.42f, 0.205f, 0.02f, 0, 120.f, 380.f, 300.f, 1.1f, 200.f },
			{ "Gravel-e", "Lightweight gravel e-bike with a discreet hub motor.", ETyre::Hybrid, 92.f, 1.03f, 1.01f, 0.44f, 0.35f, 0, 0, 110.f, 350.f, 350.f, 1.6f, 200.f },
			{ "Road-e", "Electric road bike: assist for the hills, natural feel on the flat.", ETyre::Road, 89.f, 1.00f, 1.00f, 0.42f, 0.34f, 0, 0, 100.f, 320.f, 300.f, 1.6f, 195.f },
			{ "Hardtail-e 120", "Hardtail eMTB, 85 Nm mid-drive, 120 mm fork.", ETyre::Knobbly, 103.f, 1.18f, 1.05f, 0.50f, 0.37f, 0.12f, 0, 230.f, 600.f, 625.f, 1.9f, 270.f },
			{ "Trail-e 140", "Full-suspension trail eMTB, 140 mm.", ETyre::Knobbly, 105.f, 1.22f, 1.05f, 0.52f, 0.37f, 0.14f, 0.14f, 240.f, 650.f, 720.f, 2.0f, 275.f },
			{ "Enduro-e 170", "Enduro eMTB, 170 mm, 900 Wh: climbs anything, descends harder.", ETyre::Knobbly, 108.f, 1.27f, 1.05f, 0.54f, 0.36f, 0.17f, 0.17f, 260.f, 720.f, 900.f, 1.9f, 285.f },
			{ "SL-e 150", "Light-assist eMTB (50 Nm): rides like a normal bike with a tailwind.", ETyre::Knobbly, 97.f, 1.21f, 1.04f, 0.50f, 0.37f, 0.15f, 0.15f, 160.f, 450.f, 430.f, 2.3f, 260.f },
			{ "Fat-e", "Electric fat bike with 4-inch tyres.", ETyre::Knobbly, 108.f, 1.18f, 1.05f, 0.48f, 0.38f, 0.08f, 0, 220.f, 600.f, 720.f, 1.5f, 250.f },
		};

		const FScooterModel ScooterModels[] = {
			{ "Rental 250", "UK trial-style rental scooter: 250 W, 15.5 mph.", ETyre::Hybrid, 98.f, 0.90f, 1.10f, 0.125f, 0.012f, 0.010f, 500.f, 140.f, 6.94f, 400.f, 105.f },
			{ "Fold 300", "Light folding commuter, 8.5-inch tyres.", ETyre::Hybrid, 88.f, 0.84f, 1.10f, 0.11f, 0.010f, 0.010f, 550.f, 150.f, 6.94f, 250.f, 105.f },
			{ "Commuter 500", "500 W commuter with dual brakes and front suspension.", ETyre::Hybrid, 95.f, 0.90f, 1.08f, 0.125f, 0.03f, 0.010f, 900.f, 220.f, 8.3f, 470.f, 115.f },
			{ "Cruiser 10", "10-inch pneumatic tyres and dual springs: comfy on cobbles.", ETyre::Hybrid, 100.f, 0.95f, 1.07f, 0.13f, 0.04f, 0.04f, 1000.f, 250.f, 9.7f, 600.f, 125.f },
			{ "Dual 1600", "Dual-motor 1600 W performance scooter (private land).", ETyre::Hybrid, 106.f, 1.00f, 1.06f, 0.135f, 0.05f, 0.05f, 1800.f, 400.f, 12.f, 900.f, 150.f },
			{ "Offroad 3000", "Off-road dual-motor scooter, 11-inch knobblies (private land).", ETyre::Knobbly, 115.f, 1.08f, 1.05f, 0.145f, 0.07f, 0.06f, 3000.f, 600.f, 14.f, 1300.f, 170.f },
			{ "Street Trick", "Kick-style trick deck with a small hub motor: built for hops.", ETyre::Hybrid, 84.f, 0.80f, 1.08f, 0.10f, 0.008f, 0.008f, 450.f, 130.f, 6.94f, 200.f, 120.f },
			{ "Long Range 1000", "Long-range commuter: 1 kWh battery, 50-mile range.", ETyre::Hybrid, 104.f, 0.96f, 1.08f, 0.13f, 0.03f, 0.02f, 1100.f, 260.f, 8.3f, 1000.f, 120.f },
		};

		const FMotorModel DirtModels[] = {
			{ "50 Mini", "50cc kids' minibike with an automatic clutch (lighter rider).", EVehicleCategory::DirtBike, ETyre::Motocross, 82.f, 0.96f, 0.72f, 0.45f, 0.25f, 0.23f, 0.20f, 0.18f, 4.f, 9000.f, 2500.f, 11000.f, 3, 15.f, 2.3f, 190.f },
			{ "65 2T", "65cc two-stroke youth motocrosser (lighter rider).", EVehicleCategory::DirtBike, ETyre::Motocross, 105.f, 1.13f, 0.80f, 0.53f, 0.28f, 0.25f, 0.22f, 0.22f, 9.f, 11000.f, 2500.f, 13000.f, 6, 20.f, 3.5f, 230.f },
			{ "85 2T", "85cc big-wheel youth two-stroke (lighter rider).", EVehicleCategory::DirtBike, ETyre::Motocross, 125.f, 1.26f, 0.88f, 0.60f, 0.33f, 0.30f, 0.26f, 0.26f, 15.f, 11000.f, 2200.f, 12500.f, 6, 24.f, 5.f, 280.f },
			{ "125 2T", "125cc two-stroke motocrosser: screaming, light, all about the powerband.", EVehicleCategory::DirtBike, ETyre::Motocross, 165.f, 1.46f, 0.97f, 0.69f, 0.36f, 0.33f, 0.30f, 0.30f, 24.f, 10500.f, 2200.f, 12500.f, 6, 28.f, 7.f, 350.f },
			{ "250 2T", "250cc two-stroke: violent hit, wheelies everywhere.", EVehicleCategory::DirtBike, ETyre::Motocross, 175.f, 1.48f, 0.98f, 0.70f, 0.36f, 0.33f, 0.30f, 0.30f, 40.f, 8000.f, 1800.f, 10000.f, 5, 31.f, 7.5f, 365.f },
			{ "250F", "250cc four-stroke motocrosser.", EVehicleCategory::DirtBike, ETyre::Motocross, 180.f, 1.48f, 0.98f, 0.70f, 0.36f, 0.33f, 0.30f, 0.30f, 27.f, 9500.f, 2000.f, 13500.f, 5, 30.f, 6.3f, 360.f },
			{ "350F", "350cc four-stroke: 250 agility with more torque.", EVehicleCategory::DirtBike, ETyre::Motocross, 182.f, 1.48f, 0.98f, 0.70f, 0.36f, 0.33f, 0.30f, 0.30f, 37.f, 8500.f, 1800.f, 12500.f, 5, 32.f, 6.5f, 365.f },
			{ "450F", "450cc four-stroke: the big one. Hooks up and pulls in any gear.", EVehicleCategory::DirtBike, ETyre::Motocross, 186.f, 1.49f, 0.99f, 0.71f, 0.36f, 0.33f, 0.31f, 0.31f, 50.f, 7500.f, 1700.f, 11500.f, 5, 34.f, 6.5f, 375.f },
			{ "500 Enduro", "500cc four-stroke enduro with lights, 6 gears and a big tank.", EVehicleCategory::DirtBike, ETyre::Motocross, 192.f, 1.49f, 1.00f, 0.72f, 0.36f, 0.33f, 0.30f, 0.29f, 52.f, 7000.f, 1600.f, 10500.f, 6, 36.f, 9.f, 370.f },
			{ "250 Trail", "Air-cooled 250 trail bike: tractor-like and unkillable.", EVehicleCategory::DirtBike, ETyre::Motocross, 200.f, 1.40f, 0.96f, 0.65f, 0.34f, 0.31f, 0.24f, 0.22f, 21.f, 6500.f, 1400.f, 9500.f, 6, 30.f, 8.f, 340.f },
			{ "Pit 140", "140cc pit bike: tiny, loud and hilarious.", EVehicleCategory::DirtBike, ETyre::Motocross, 135.f, 1.12f, 0.82f, 0.52f, 0.27f, 0.25f, 0.20f, 0.18f, 11.f, 8000.f, 1800.f, 10000.f, 4, 24.f, 4.f, 290.f },
			{ "Trials 300", "Trials bike: no seat, super-low gearing, climbs walls.", EVehicleCategory::DirtBike, ETyre::Motocross, 150.f, 1.33f, 0.98f, 0.62f, 0.34f, 0.32f, 0.17f, 0.17f, 30.f, 5500.f, 1500.f, 9000.f, 6, 24.f, 3.f, 380.f },
		};

		const FMotorModel MotoModels[] = {
			{ "125 Naked", "125cc learner naked: light and frugal.", EVehicleCategory::Motorcycle, ETyre::Hybrid, 212.f, 1.32f, 0.80f, 0.61f, 0.29f, 0.30f, 0.12f, 0.11f, 12.f, 8000.f, 1400.f, 10500.f, 6, 32.f, 11.f, 380.f },
			{ "125 Sport", "125cc faired sports learner bike.", EVehicleCategory::Motorcycle, ETyre::Sport, 215.f, 1.34f, 0.80f, 0.62f, 0.30f, 0.30f, 0.12f, 0.12f, 12.f, 8500.f, 1400.f, 10500.f, 6, 34.f, 11.f, 380.f },
			{ "300 Twin Sport", "300cc parallel-twin sports bike.", EVehicleCategory::Motorcycle, ETyre::Sport, 240.f, 1.37f, 0.79f, 0.64f, 0.30f, 0.31f, 0.13f, 0.12f, 27.f, 10000.f, 1300.f, 13000.f, 6, 48.f, 14.f, 395.f },
			{ "400 Single", "400cc single-cylinder naked: punchy and narrow.", EVehicleCategory::Motorcycle, ETyre::Sport, 235.f, 1.36f, 0.80f, 0.63f, 0.30f, 0.31f, 0.14f, 0.13f, 38.f, 7000.f, 1300.f, 10000.f, 6, 46.f, 13.f, 400.f },
			{ "500 A2 Twin", "A2-licence 500cc twin: the do-everything bike.", EVehicleCategory::Motorcycle, ETyre::Hybrid, 265.f, 1.42f, 0.81f, 0.67f, 0.30f, 0.31f, 0.14f, 0.13f, 43.f, 6500.f, 1300.f, 9500.f, 6, 50.f, 16.f, 400.f },
			{ "650 Twin", "650cc parallel-twin roadster.", EVehicleCategory::Motorcycle, ETyre::Sport, 265.f, 1.41f, 0.80f, 0.66f, 0.31f, 0.32f, 0.13f, 0.13f, 64.f, 6500.f, 1300.f, 10000.f, 6, 57.f, 15.f, 415.f },
			{ "700 Adventure", "Mid-size adventure bike with 21-inch front wheel.", EVehicleCategory::Motorcycle, ETyre::Hybrid, 285.f, 1.51f, 0.85f, 0.71f, 0.36f, 0.33f, 0.20f, 0.19f, 70.f, 6500.f, 1300.f, 9500.f, 6, 55.f, 18.f, 405.f },
			{ "800 Triple", "800cc triple naked: wheelies on the throttle in second.", EVehicleCategory::Motorcycle, ETyre::Sport, 265.f, 1.41f, 0.79f, 0.66f, 0.31f, 0.32f, 0.13f, 0.13f, 82.f, 9000.f, 1300.f, 12500.f, 6, 66.f, 15.f, 425.f },
			{ "900 Sport Tourer", "Sport tourer with luggage: fast, comfy, long legs.", EVehicleCategory::Motorcycle, ETyre::Sport, 305.f, 1.50f, 0.82f, 0.71f, 0.31f, 0.32f, 0.14f, 0.14f, 90.f, 8000.f, 1200.f, 11000.f, 6, 68.f, 19.f, 405.f },
			{ "1000 Superbike", "Inline-four 1000cc superbike: track weapon.", EVehicleCategory::Motorcycle, ETyre::Sport, 277.f, 1.41f, 0.78f, 0.67f, 0.31f, 0.32f, 0.12f, 0.12f, 113.f, 11000.f, 1300.f, 14000.f, 6, 82.f, 17.f, 420.f },
			{ "1200 Cruiser", "Big V-twin cruiser: long, low, lazy torque.", EVehicleCategory::Motorcycle, ETyre::Hybrid, 335.f, 1.62f, 0.70f, 0.74f, 0.33f, 0.33f, 0.12f, 0.09f, 105.f, 3500.f, 900.f, 6000.f, 5, 56.f, 18.f, 380.f },
			{ "1300 Hyper Naked", "1300cc hyper-naked: 140 Nm, wheelie control strongly advised.", EVehicleCategory::Motorcycle, ETyre::Sport, 285.f, 1.48f, 0.80f, 0.69f, 0.31f, 0.32f, 0.13f, 0.13f, 140.f, 9000.f, 1200.f, 11000.f, 6, 78.f, 18.f, 435.f },
			{ "1100 Retro", "Retro roadster, air-cooled twin, round headlight.", EVehicleCategory::Motorcycle, ETyre::Hybrid, 290.f, 1.48f, 0.81f, 0.70f, 0.32f, 0.32f, 0.12f, 0.11f, 95.f, 4500.f, 1000.f, 7500.f, 6, 60.f, 15.f, 395.f },
			{ "Supermoto 690", "Single-cylinder supermoto: road tyres on a dirt-bike chassis.", EVehicleCategory::Motorcycle, ETyre::Sport, 235.f, 1.48f, 0.92f, 0.70f, 0.30f, 0.30f, 0.24f, 0.22f, 73.f, 6500.f, 1500.f, 9500.f, 6, 55.f, 13.f, 420.f },
		};

		const FEMotorModel EMotorModels[] = {
			{ "E-Dirt Mini", "Electric kids' dirt bike (lighter rider).", EVehicleCategory::DirtBike, ETyre::Motocross, 70.f, 0.95f, 0.75f, 0.45f, 0.24f, 0.22f, 0.15f, 0.15f, 1200.f, 300.f, 9.f, 300.f, 180.f },
			{ "E-Dirt 250e", "Silent electric motocrosser with instant torque.", EVehicleCategory::DirtBike, ETyre::Motocross, 175.f, 1.47f, 0.98f, 0.70f, 0.36f, 0.33f, 0.30f, 0.30f, 18000.f, 2600.f, 26.f, 3800.f, 370.f },
			{ "E-Moto Street", "Electric naked motorcycle: no gears, huge shove.", EVehicleCategory::Motorcycle, ETyre::Sport, 290.f, 1.45f, 0.80f, 0.68f, 0.31f, 0.32f, 0.13f, 0.13f, 70000.f, 6000.f, 55.f, 15000.f, 420.f },
			{ "E-Moto Commuter", "125-equivalent electric commuter motorcycle.", EVehicleCategory::Motorcycle, ETyre::Hybrid, 200.f, 1.30f, 0.80f, 0.60f, 0.29f, 0.29f, 0.11f, 0.10f, 11000.f, 1700.f, 25.f, 4000.f, 370.f },
		};

		// Real model names, one row per model type above (same order), one column per brand slot.
		// Brand and model names are trademarks of their respective owners (see Credits).
		const char* const CycleNames[][6] = {
			{ "Mongoose Legion L100", "Haro Downtown", "WeThePeople Arcade", "Kink Curb", "Cult Gateway", "Subrosa Altus" },                       // Park 20
			{ "WeThePeople Crysis", "Kink Gap", "Cult Control", "Sunday Primer", "Fiend Type O", "Haro Leucadia" },                                 // Street 21
			{ "Chase Element", "Meybo Holeshot", "Supercross Envy", "Haro Race Lite Pro", "GT Speed Series", "Redline Flight" },                   // Race Pro
			{ "Haro Master", "Ares A-Class", "Mongoose Legion L20", "Kink Launch", "Sunday Blueprint", "Fit Series One" },                         // Flatland
			{ "SE Bikes Quad Ripper", "Haro Downtown 24", "Sunday Model C", "Redline Proline 24", "GT Pro Series 24", "Mongoose Title 24" },      // Cruiser 24
			{ "Haro Downtown 16", "Kink Kicker 16", "Sunday Primer 16", "Cult Juvenile 16", "Fit Misfit 16", "WeThePeople Seed 16" },              // Junior 16
			{ "NS Bikes Movement 2", "Kona Shonky", "Commencal Absolut", "Dartmoor Two6Player", "Marin Alcatraz", "Canyon Stitched 360" },         // Dirt 26
			{ "Dartmoor Gamer 24", "NS Bikes Zircus", "Specialized P.Slope", "Trek Ticket S", "Canyon Stitched 720", "Kona Shonky DL" },          // Slope 24
			{ "Specialized Epic HT", "Trek Procaliber", "Scott Scale", "Canyon Exceed", "Cannondale F-Si", "Giant XTC Advanced" },                  // XC 100 HT
			{ "Specialized Epic", "Trek Supercaliber", "Scott Spark RC", "Canyon Lux", "Cannondale Scalpel", "Giant Anthem" },                      // XC 100 FS
			{ "Cotic BFe", "Ragley Big Al", "Kona Honzo", "Nukeproof Scout", "Orange Crush", "Marin San Quentin" },                                // Trail 130 HT
			{ "Trek Fuel EX", "Specialized Stumpjumper", "Santa Cruz Hightower", "Giant Trance", "Canyon Spectral", "YT Jeffsy" },                 // Trail 140
			{ "Santa Cruz Bronson", "Orange Five", "Mondraker Foxy", "Transition Patrol", "Nukeproof Reactor", "Whyte T-160" },                    // All-Mountain 150
			{ "Santa Cruz Megatower", "Specialized Enduro", "Trek Slash", "Canyon Strive", "YT Capra", "Nukeproof Mega" },                        // Enduro 170
			{ "Surly Wednesday", "Salsa Mukluk", "Trek Farley", "Specialized Fatboy", "Kona Wo", "On-One Fatty" },                                // Fat 4.8
			{ "Islabikes Creig 24", "Frog MTB 69", "Trek Roscoe 24", "Specialized Riprock 24", "Woom OFF 5", "Commencal Ramones 24" },             // Kids 24 Trail
			{ "Trek Madone", "Canyon Aeroad", "Cervelo S5", "Giant Propel", "Ribble Ultra SL R", "Specialized Tarmac SL8" },                       // Aero Disc
			{ "Specialized Roubaix", "Trek Domane", "Canyon Endurace", "Giant Defy", "Cannondale Synapse", "Boardman SLR Endurance" },             // Endurance
			{ "Specialized Aethos", "Trek Emonda", "Canyon Ultimate", "Scott Addict", "Cervelo R5", "BMC Teammachine" },                          // Climber SL
			{ "Specialized Diverge", "Canyon Grizl", "Cannondale Topstone", "Trek Checkpoint", "Giant Revolt", "Ribble Gravel AL" },               // Gravel 40
			{ "Trek Boone", "Cannondale SuperX", "Ridley X-Night", "Canyon Inflite", "Specialized Crux", "Giant TCX" },                            // Cyclocross
			{ "Charge Plug", "Cinelli Vigorelli", "Fuji Track", "State Bicycle Co. Core-Line", "Mango Point R", "Genesis Day One" },              // Fixie
			{ "Surly Long Haul Trucker", "Dawes Galaxy", "Kona Sutra", "Genesis Tour de Fer", "Ridgeback Panorama", "Thorn Sherpa" },             // Tourer
			{ "Specialized Sirrus", "Trek FX", "Cannondale Quick", "Giant Escape", "Boardman HYB 8.6", "Ridgeback Speed" },                        // City Hybrid
			{ "Santa Cruz V10", "Specialized Demo", "Trek Session", "YT Tues", "Commencal Supreme DH", "Canyon Sender" },                          // DH 29
			{ "Mondraker Summum", "Pivot Phoenix", "Scott Gambler", "Nukeproof Dissent", "GT Fury", "Norco Aurum" },                               // DH Mullet
			{ "Commencal Clash", "Norco Shore", "Transition TR11", "Canyon Torque", "Propain Spindrift", "Banshee Darkside" },                    // Freeride 180
			{ "Commencal Meta SX", "Marin Alpine Trail", "Kona Process X", "Ibis HD6", "Rocky Mountain Slayer", "GT Force" },                      // Bike Park 160
			{ "Commencal Supreme 24", "Norco Fluid FS 24", "Trek Fuel EX Jr", "Specialized Status 24", "Canyon Neuron Young Hero", "Marin Rift Zone Jr" }, // Junior DH
		};
		const char* const EBikeNames[][6] = {
			{ "Raleigh Array", "Carrera Crossfuse", "Ribble Hybrid AL e", "Boardman HYB 8.8E", "Pendleton Somerby", "Volt Pulse" },               // City Hub-e
			{ "Raleigh Motus Tour Lowstep", "Gazelle Orange C7", "Batavus Finez E-go", "Kalkhoff Image", "Specialized Turbo Como", "Electra Townie Go!" }, // Step-Through-e
			{ "Riese & Muller Charger", "Kalkhoff Endeavour", "Cube Kathmandu Hybrid", "Trek Allant+", "Gazelle Medeo", "Specialized Turbo Vado" },  // Trekking-e
			{ "Tern GSD", "Riese & Muller Multicharger", "Yuba Spicy Curry", "Xtracycle Swoop", "Benno Boost", "Specialized Globe Haul LT" },     // Cargo Longtail-e
			{ "Brompton Electric C Line", "Tern Vektron", "GoCycle G4", "Hummingbird Electric", "Raleigh Stow-E-Way", "MiRider One" },            // Folder-e
			{ "Ribble Gravel AL e", "Orbea Gain", "Cube Nuroad Hybrid", "Boardman ADV 8.9E", "Canyon Grail:ON", "Bianchi Impulso E-Allroad" },   // Gravel-e
			{ "Ribble Endurance SL e", "Pinarello Nytro", "Specialized Turbo Creo SL", "Cannondale SuperSix EVO Neo", "BMC Roadmachine AMP", "Wilier Cento1 Hybrid" }, // Road-e
			{ "Whyte E-405", "Cube Reaction Hybrid", "Trek Powerfly", "Haibike AllTrack", "Canyon Grand Canyon:ON", "Specialized Turbo Tero" },  // Hardtail-e 120
			{ "Specialized Turbo Levo", "Trek Rail", "Giant Trance X E+", "Canyon Spectral:ON", "Cube Stereo Hybrid 140", "Whyte E-150" },        // Trail-e 140
			{ "Specialized Turbo Kenevo", "Orbea Wild", "Santa Cruz Bullit", "YT Decoy", "Commencal Meta Power SX", "Nukeproof Megawatt" },     // Enduro-e 170
			{ "Specialized Turbo Levo SL", "Trek Fuel EXe", "Orbea Rise", "Santa Cruz Heckler SL", "Scott Lumen eRIDE", "Pivot Shuttle SL" },     // SL-e 150
			{ "Haibike FatSix", "Rad Power RadRover", "Felt LEBOWSKe", "Himiway Cruiser", "Aventon Aventure", "Lectric XP 3.0" },                 // Fat-e
		};
		const char* const ScooterNames[][4] = {
			{ "Voi V5", "Lime Gen 4", "TIER 5", "Bird Three" },                                                       // Rental 250
			{ "Xiaomi Electric Scooter 4 Lite", "Pure Air3", "Segway Ninebot E2", "Hiboy S2" },                        // Fold 300
			{ "Segway Ninebot Max G2", "Pure Advance", "Xiaomi Electric Scooter 4 Pro", "Unagi Model One" },           // Commuter 500
			{ "Apollo City", "NIU KQi3 Max", "Inokim Quick 4", "Pure Advance Flex" },                                  // Cruiser 10
			{ "Kaabo Mantis 10", "Apollo Phantom", "Varla Eagle One", "Dualtron Mini" },                               // Dual 1600
			{ "Kaabo Wolf Warrior 11", "Dualtron Thunder 3", "Nami Burn-E 2", "Segway Ninebot GT2" },                  // Offroad 3000
			{ "Razor E Prime III", "Micro Merlin", "Gotrax GXL V2", "Hover-1 Alpha" },                                 // Street Trick
			{ "Segway Ninebot Max G30", "Xiaomi Electric Scooter 4 Ultra", "NIU KQi Air", "Apollo Explore" },          // Long Range 1000
		};
		const char* const DirtNames[][4] = {
			{ "KTM 50 SX", "Yamaha PW50", "Husqvarna TC 50", "GasGas MC 50" },
			{ "KTM 65 SX", "Yamaha YZ65", "Husqvarna TC 65", "Kawasaki KX65" },
			{ "KTM 85 SX", "Yamaha YZ85", "Kawasaki KX85", "Husqvarna TC 85" },
			{ "KTM 125 SX", "Yamaha YZ125", "Husqvarna TC 125", "GasGas MC 125" },
			{ "KTM 250 SX", "Yamaha YZ250", "Husqvarna TC 250", "GasGas MC 250" },
			{ "Honda CRF250R", "Yamaha YZ250F", "Kawasaki KX250", "Suzuki RM-Z250" },
			{ "KTM 350 SX-F", "Husqvarna FC 350", "GasGas MC 350F", "Beta 350 RR" },
			{ "Honda CRF450R", "Yamaha YZ450F", "Kawasaki KX450", "KTM 450 SX-F" },
			{ "KTM 500 EXC-F", "Husqvarna FE 501", "Beta 480 RR", "Sherco 500 SEF Factory" },
			{ "Honda CRF250L", "Yamaha XT250", "Kawasaki KLX230", "Suzuki DR-Z250" },
			{ "Kawasaki KLX140R", "Yamaha TT-R125", "YCF Pilot 150", "Honda CRF110F" },
			{ "GasGas TXT Racing 300", "Beta EVO 300", "Montesa Cota 301RR", "Sherco ST 300" },
		};
		const char* const MotoNames[][4] = {
			{ "Honda CB125R", "Yamaha MT-125", "KTM 125 Duke", "Suzuki GSX-S125" },
			{ "Yamaha YZF-R125", "Honda CBR125R", "KTM RC 125", "Aprilia RS 125" },
			{ "Yamaha YZF-R3", "Kawasaki Ninja 400", "Aprilia RS 457", "KTM RC 390" },
			{ "KTM 390 Duke", "Husqvarna Svartpilen 401", "Triumph Speed 400", "Honda CB300R" },
			{ "Honda CB500 Hornet", "Kawasaki Z500", "Honda NX500", "CFMoto 450NK" },
			{ "Kawasaki Z650", "Honda CB650R", "Royal Enfield Interceptor 650", "Suzuki SV650" },
			{ "Yamaha Tenere 700", "KTM 790 Adventure", "Suzuki V-Strom 800DE", "Honda Transalp XL750" },
			{ "Triumph Street Triple 765", "Yamaha MT-09", "MV Agusta Brutale 800", "Triumph Trident 660" },
			{ "Yamaha Tracer 9", "Kawasaki Ninja 1000SX", "Suzuki GSX-S1000GT", "BMW F 900 XR" },
			{ "Honda CBR1000RR-R Fireblade", "Yamaha YZF-R1", "Kawasaki Ninja ZX-10R", "BMW S 1000 RR" },
			{ "Harley-Davidson Sportster S", "Triumph Bonneville Bobber", "Indian Scout", "Honda Rebel 1100" },
			{ "KTM 1390 Super Duke R", "Kawasaki Z H2", "Ducati Streetfighter V4", "Aprilia Tuono V4" },
			{ "Triumph Thruxton RS", "BMW R nineT", "Kawasaki Z900RS", "Ducati Scrambler 1100" },
			{ "KTM 690 SMC R", "Husqvarna 701 Supermoto", "Ducati Hypermotard 950", "Suzuki DR-Z400SM" },
		};
		const char* const EMotorNames[][4] = {
			{ "KTM SX-E 3", "Husqvarna EE 3", "GasGas MC-E 3", "Oset 16.0 Racing" },
			{ "CAKE Kalk OR", "Alta Redshift MXR", "KTM Freeride E-XC", "Sur-Ron Storm Bee" }, // Stark Varg has its own exact entries
			{ "Zero SR/F", "Energica Eva Ribelle", "LiveWire One", "Zero SR/S" },
			{ "Super Soco TC Max", "Horwin CR6", "Maeving RM1", "Zero FXE" },
		};
		static_assert(sizeof(CycleNames) / sizeof(CycleNames[0]) == sizeof(CycleModels) / sizeof(CycleModels[0]), "one name row per cycle model");
		static_assert(sizeof(EBikeNames) / sizeof(EBikeNames[0]) == sizeof(EBikeModels) / sizeof(EBikeModels[0]), "one name row per e-bike model");
		static_assert(sizeof(ScooterNames) / sizeof(ScooterNames[0]) == sizeof(ScooterModels) / sizeof(ScooterModels[0]), "one name row per scooter model");
		static_assert(sizeof(DirtNames) / sizeof(DirtNames[0]) == sizeof(DirtModels) / sizeof(DirtModels[0]), "one name row per dirt model");
		static_assert(sizeof(MotoNames) / sizeof(MotoNames[0]) == sizeof(MotoModels) / sizeof(MotoModels[0]), "one name row per road model");
		static_assert(sizeof(EMotorNames) / sizeof(EMotorNames[0]) == sizeof(EMotorModels) / sizeof(EMotorModels[0]), "one name row per electric motor model");
		static_assert(sizeof(CycleBrands) / sizeof(CycleBrands[0]) == 6 && sizeof(MotorBrands) / sizeof(MotorBrands[0]) == 4, "name tables assume 6 cycle and 4 motor brand slots");

		FVehicleEntry ElectricMotor(const FEMotorModel& M, const FBrand& B, const std::string& Id, const std::string& Name)
		{
			const float Ratios[] = { 1.f };
			FVehicleEntry E = Motor("tmp", "tmp", "tmp", M.Cat, M.Tyre, M.Mass * B.MassF, M.Wb + B.WbAdd, M.ComH * B.ComF, M.ComR + B.WbAdd * 0.5f,
				M.WheelRF, M.WheelRR, M.TF, M.TR, 10.f, 5000.f, 1000.f, 9000.f, 1, Ratios, 5.f, 0.f, M.TopMs, M.Lift);
			E.Id = Id; E.DisplayName = Name; E.Description = M.Desc;
			FPowertrainParams& PT = E.Params.Powertrain;
			PT = FPowertrainParams();
			PT.Type = EPowertrain::ElectricThrottle;
			PT.MotorPeakPowerW = M.PowerW * B.PowerF;
			PT.MotorPeakForceN = M.ForceN * B.PowerF;
			PT.MotorCutoffSpeed = M.TopMs;
			PT.BatteryWh = M.Wh;
			PT.RegenFraction = 0.25f;
			E.Params.Wheelie.BunnyHopSpeed = M.Cat == EVehicleCategory::DirtBike ? 1.2f : 0.f;
			return E;
		}

		void AddLineup(std::vector<FVehicleEntry>& V)
		{
			// Six-speed and five-speed ratio sets (scaled per model); fewer gears use the first N of a wider set.
			const float Ratios6[] = { 2.70f, 1.98f, 1.59f, 1.34f, 1.16f, 1.02f };
			const float Ratios5[] = { 2.40f, 1.85f, 1.50f, 1.27f, 1.10f };
			const float Ratios4[] = { 2.50f, 1.70f, 1.30f, 1.05f };
			const float Ratios3[] = { 2.40f, 1.55f, 1.10f };

			constexpr size_t NumCycleBrands = sizeof(CycleBrands) / sizeof(CycleBrands[0]);
			for (size_t bi = 0; bi < NumCycleBrands; ++bi)
			{
				const FBrand& B = CycleBrands[bi];
				for (size_t mi = 0; mi < sizeof(CycleModels) / sizeof(CycleModels[0]); ++mi)
				{
					const FCycleModel& M = CycleModels[mi];
					FVehicleEntry E = Bicycle("tmp", "tmp", M.Desc, M.Cat, M.Tyre, M.Mass * B.MassF, M.Wb + B.WbAdd, M.ComH * B.ComF,
						M.ComR + B.WbAdd * 0.5f, M.WheelR, M.TF, M.TR, M.Hop * (2.f - B.MassF), M.Lift * B.PowerF, M.CdA);
					E.Id = MakeId("cyc", B, M.Name); E.DisplayName = CycleNames[mi][bi];
					V.push_back(E);
				}
				for (size_t mi = 0; mi < sizeof(EBikeModels) / sizeof(EBikeModels[0]); ++mi)
				{
					const FEBikeModel& M = EBikeModels[mi];
					FVehicleEntry E = Bicycle("tmp", "tmp", M.Desc, EVehicleCategory::EBike, M.Tyre, M.Mass * B.MassF, M.Wb + B.WbAdd, M.ComH * B.ComF,
						M.ComR + B.WbAdd * 0.5f, M.WheelR, M.TF, M.TR, M.Hop, M.Lift * B.PowerF, 0.55f);
					FPowertrainParams& PT = E.Params.Powertrain;
					PT.Type = EPowertrain::PedalAssist;
					PT.MotorPeakForceN = M.Force * B.PowerF;
					PT.MotorPeakPowerW = M.Power * B.PowerF;
					PT.MotorCutoffSpeed = 6.94f; // UK EAPC 15.5 mph
					PT.BatteryWh = M.Wh;
					E.Params.Wheelie.AssistStrength = 1.1f;
					E.Id = MakeId("eb", B, M.Name); E.DisplayName = EBikeNames[mi][bi];
					V.push_back(E);
				}
			}
			for (int bi = 0; bi < 4; ++bi)
			{
				const FBrand& B = CycleBrands[bi];
				for (size_t mi = 0; mi < sizeof(ScooterModels) / sizeof(ScooterModels[0]); ++mi)
				{
					const FScooterModel& M = ScooterModels[mi];
					FVehicleEntry E{ MakeId("esc", B, M.Name), ScooterNames[mi][bi], M.Desc, {} };
					FVehicleParams& P = E.Params;
					P.Category = EVehicleCategory::EScooter; P.Tyre = M.Tyre;
					P.Mass = M.Mass * B.MassF; P.Wheelbase = M.Wb + B.WbAdd; P.ComHeight = M.ComH * B.ComF; P.ComFromRear = P.Wheelbase * 0.46f;
					P.WheelRadiusFront = P.WheelRadiusRear = M.WheelR;
					P.PitchRadiusOfGyration = 0.42f;
					P.MaxSteerAngle = 0.45f; P.SteerSpeedRef = 5.f; P.LeanResponse = 5.f; P.MaxLean = 0.62f;
					P.RollingResistance = 0.012f; P.CdA = 0.6f; P.TopSpeed = M.TopMs;
					P.MaxBrakeForceFront = P.Mass * 5.5f; P.MaxBrakeForceRear = P.Mass * 4.f;
					P.Powertrain.Type = EPowertrain::ElectricThrottle;
					P.Powertrain.MotorPeakPowerW = M.PowerW * B.PowerF; P.Powertrain.MotorPeakForceN = M.ForceN * B.PowerF;
					P.Powertrain.MotorCutoffSpeed = M.TopMs; P.Powertrain.BatteryWh = M.Wh; P.Powertrain.RegenFraction = 0.15f;
					P.Wheelie.RiderLiftTorque = M.Lift; P.Wheelie.RiderBalanceTorque = M.Lift * 0.62f;
					P.Wheelie.BunnyHopSpeed = 1.1f; P.Wheelie.LoopOutMargin = 0.26f; P.Wheelie.AssistStrength = 0.85f;
					FinishGeometry(P, M.TF, M.TR, M.TF > 0.02f ? 0.25f : 0.3f, 0.48f);
					V.push_back(E);
				}
			}
			for (size_t bi = 0; bi < sizeof(MotorBrands) / sizeof(MotorBrands[0]); ++bi)
			{
				const FBrand& B = MotorBrands[bi];
				auto AddMotors = [&](const FMotorModel* List, size_t Count, const char* const (*Names)[4])
				{
					for (size_t mi = 0; mi < Count; ++mi)
					{
						const FMotorModel& M = List[mi];
						const float* Base = M.Gears >= 6 ? Ratios6 : (M.Gears == 5 ? Ratios5 : (M.Gears == 4 ? Ratios4 : Ratios3));
						const float Top = Base[M.Gears - 1];
						// Overall reduction so redline in top gear lands just above the target top speed.
						const float FinalDrive = (M.Redline * 2.f * Pi / 60.f) * M.WheelRR / (Top * M.TopMs * 1.03f);
						FVehicleEntry E = Motor("tmp", "tmp", M.Desc, M.Cat, M.Tyre, M.Mass * B.MassF, M.Wb + B.WbAdd, M.ComH * B.ComF,
							M.ComR + B.WbAdd * 0.5f, M.WheelRF, M.WheelRR, M.TF, M.TR, M.TorqueNm * B.PowerF, M.PeakRpm, M.IdleRpm, M.Redline,
							M.Gears, Base, FinalDrive, M.FuelL, M.TopMs, M.Lift);
						E.Id = MakeId(M.Cat == EVehicleCategory::DirtBike ? "dirt" : "moto", B, M.Name);
						E.DisplayName = Names[mi][bi];
						V.push_back(E);
					}
				};
				AddMotors(DirtModels, sizeof(DirtModels) / sizeof(DirtModels[0]), DirtNames);
				AddMotors(MotoModels, sizeof(MotoModels) / sizeof(MotoModels[0]), MotoNames);
				for (size_t mi = 0; mi < sizeof(EMotorModels) / sizeof(EMotorModels[0]); ++mi)
				{
					const FEMotorModel& M = EMotorModels[mi];
					V.push_back(ElectricMotor(M, B, MakeId(M.Cat == EVehicleCategory::DirtBike ? "dirt" : "moto", B, M.Name), EMotorNames[mi][bi]));
				}
			}
		}

		// Named light electric motos and big-wheel scooters, from the makers' published figures (power,
		// battery, kerb weight, top speed, wheel sizes). Mass includes a 75 kg rider. Ids carry style
		// keywords for the visuals: "lemx" = Sur-Ron-style light e-moto frame, "dual" / "offroad" scooters.
		void AddNamedElectric(std::vector<FVehicleEntry>& V)
		{
			const FBrand Exact{ "", "", 1.f, 1.f, 1.f, 0.f };
			struct FNamedMoto { const char* Id; const char* Name; FEMotorModel M; };
			const FNamedMoto Motos[] = {
				{ "dirt_lemx_surron_lightbeex", "Sur-Ron Light Bee X", { "", "Light electric dirt bike: 6 kW peak, 60 V 40 Ah battery, 57 kg, 19-inch wheels. Off-road / private land.",
					EVehicleCategory::DirtBike, ETyre::Motocross, 132.f, 1.26f, 0.95f, 0.60f, 0.31f, 0.31f, 0.20f, 0.22f, 6000.f, 800.f, 20.8f, 2400.f, 300.f } },
				{ "dirt_lemx_surron_ultrabee", "Sur-Ron Ultra Bee", { "", "Bigger Sur-Ron: 12.5 kW peak, 74 V 55 Ah battery, 85 kg, 21/18-inch wheels. Off-road / private land.",
					EVehicleCategory::DirtBike, ETyre::Motocross, 160.f, 1.355f, 0.98f, 0.65f, 0.34f, 0.32f, 0.24f, 0.25f, 12500.f, 1375.f, 25.f, 4070.f, 340.f } },
				{ "dirt_lemx_talaria_stingrmx4", "Talaria Sting R MX4", { "", "Light electric dirt bike: 8 kW peak, 60 V 45 Ah battery, 66 kg, 19-inch wheels. Off-road / private land.",
					EVehicleCategory::DirtBike, ETyre::Motocross, 141.f, 1.29f, 0.96f, 0.62f, 0.31f, 0.31f, 0.21f, 0.22f, 8000.f, 1100.f, 23.6f, 2700.f, 310.f } },
				{ "dirt_lemx_talaria_xxx", "Talaria XXX", { "", "Entry Talaria: 6 kW peak, 60 V 38 Ah battery, 55 kg, 19-inch wheels. Off-road / private land.",
					EVehicleCategory::DirtBike, ETyre::Motocross, 130.f, 1.25f, 0.95f, 0.60f, 0.30f, 0.30f, 0.19f, 0.20f, 6000.f, 750.f, 20.8f, 2280.f, 290.f } },
				{ "dirt_lemx_segway_x260", "Segway Dirt eBike X260", { "", "Light electric dirt bike: 5 kW peak, 74 V 32 Ah battery, 56 kg, 19-inch wheels. Off-road / private land.",
					EVehicleCategory::DirtBike, ETyre::Motocross, 131.f, 1.24f, 0.95f, 0.60f, 0.31f, 0.31f, 0.20f, 0.20f, 5000.f, 800.f, 20.8f, 2370.f, 290.f } },
				{ "dirt_lemx_tuttio_soleil01", "Tuttio Soleil01", { "", "Budget mini electric dirt bike: 2 kW, 48 V 21 Ah battery, about 48 kg. Off-road / private land.",
					EVehicleCategory::DirtBike, ETyre::Motocross, 123.f, 1.05f, 0.90f, 0.50f, 0.24f, 0.22f, 0.12f, 0.10f, 2000.f, 450.f, 13.4f, 1010.f, 220.f } },
				{ "dirt_varg_stark_vargmx", "Stark Varg MX", { "", "Full-size electric motocrosser: 60 kW (80 hp) peak, 6.5 kWh battery, 118 kg, 938 Nm at the wheel.",
					EVehicleCategory::DirtBike, ETyre::Motocross, 193.f, 1.49f, 0.98f, 0.71f, 0.355f, 0.33f, 0.31f, 0.30f, 60000.f, 2840.f, 33.f, 6500.f, 380.f } },
				{ "dirt_varg_stark_vargex", "Stark Varg EX", { "", "Enduro Stark Varg with lights and plate: 60 kW peak, 6.5 kWh battery, 120 kg.",
					EVehicleCategory::DirtBike, ETyre::Motocross, 195.f, 1.49f, 1.00f, 0.71f, 0.355f, 0.33f, 0.30f, 0.29f, 60000.f, 2840.f, 33.f, 6500.f, 370.f } },
			};
			for (const FNamedMoto& N : Motos)
			{
				FVehicleEntry E = ElectricMotor(N.M, Exact, N.Id, N.Name);
				E.Params.Wheelie.BunnyHopSpeed = 1.3f; // light enough to hop
				V.push_back(E);
			}

			struct FNamedScooter { const char* Id; const char* Name; FScooterModel M; };
			const FNamedScooter Scooters[] = {
				{ "esc_kukirin_g2pro", "KuKirin G2 Pro", { "", "600 W commuter, 48 V 15 Ah, 45 km/h, 9-inch tyres, front suspension. Private land in the UK.",
					ETyre::Hybrid, 97.f, 0.95f, 1.08f, 0.115f, 0.03f, 0.02f, 900.f, 220.f, 12.5f, 720.f, 115.f } },
				{ "esc_kukirin_g2max", "KuKirin G2 Max", { "", "1000 W long-range commuter, 48 V 20 Ah, 55 km/h, 10-inch tyres. Private land in the UK.",
					ETyre::Hybrid, 99.f, 0.98f, 1.08f, 0.127f, 0.04f, 0.03f, 1400.f, 300.f, 15.3f, 960.f, 120.f } },
				{ "esc_offroad_kukirin_g3", "KuKirin G3", { "", "1200 W, 52 V 18 Ah, 50 km/h, 10-inch off-road tyres, dual springs. Private land in the UK.",
					ETyre::Knobbly, 102.f, 1.00f, 1.07f, 0.13f, 0.05f, 0.04f, 1600.f, 330.f, 13.9f, 936.f, 130.f } },
				{ "esc_dual_kukirin_g3pro", "KuKirin G3 Pro", { "", "Dual 1200 W motors, 52 V 23.4 Ah, 65 km/h, 10-inch off-road tyres. Private land in the UK.",
					ETyre::Knobbly, 104.f, 1.02f, 1.07f, 0.13f, 0.05f, 0.04f, 2400.f, 600.f, 18.f, 1217.f, 145.f } },
				{ "esc_offroad_kukirin_g4", "KuKirin G4", { "", "2000 W, 60 V 20 Ah, about 60 km/h, 12-inch off-road tyres, hydraulic brakes. Private land in the UK.",
					ETyre::Knobbly, 112.f, 1.08f, 1.06f, 0.15f, 0.07f, 0.06f, 2000.f, 450.f, 16.7f, 1200.f, 155.f } },
				{ "esc_dual_kukirin_g4max", "KuKirin G4 Max", { "", "Dual 1600 W motors, 60 V 35.2 Ah, 86 km/h claimed, 12-inch off-road tyres, 64.5 kg. Private land in the UK.",
					ETyre::Knobbly, 140.f, 1.15f, 1.05f, 0.155f, 0.08f, 0.07f, 3200.f, 800.f, 24.f, 2112.f, 175.f } },
			};
			for (const FNamedScooter& N : Scooters)
			{
				const FScooterModel& M = N.M;
				FVehicleEntry E{ N.Id, N.Name, M.Desc, {} };
				FVehicleParams& P = E.Params;
				P.Category = EVehicleCategory::EScooter; P.Tyre = M.Tyre;
				P.Mass = M.Mass; P.Wheelbase = M.Wb; P.ComHeight = M.ComH; P.ComFromRear = P.Wheelbase * 0.46f;
				P.WheelRadiusFront = P.WheelRadiusRear = M.WheelR;
				P.PitchRadiusOfGyration = 0.42f;
				P.MaxSteerAngle = 0.45f; P.SteerSpeedRef = 5.f; P.LeanResponse = 5.f; P.MaxLean = 0.62f;
				P.RollingResistance = 0.012f; P.CdA = 0.6f; P.TopSpeed = M.TopMs;
				P.MaxBrakeForceFront = P.Mass * 5.5f; P.MaxBrakeForceRear = P.Mass * 4.f;
				P.Powertrain.Type = EPowertrain::ElectricThrottle;
				P.Powertrain.MotorPeakPowerW = M.PowerW; P.Powertrain.MotorPeakForceN = M.ForceN;
				P.Powertrain.MotorCutoffSpeed = M.TopMs; P.Powertrain.BatteryWh = M.Wh; P.Powertrain.RegenFraction = 0.15f;
				P.Wheelie.RiderLiftTorque = M.Lift; P.Wheelie.RiderBalanceTorque = M.Lift * 0.62f;
				P.Wheelie.BunnyHopSpeed = 1.1f; P.Wheelie.LoopOutMargin = 0.26f; P.Wheelie.AssistStrength = 0.85f;
				FinishGeometry(P, M.TF, M.TR, M.TF > 0.02f ? 0.25f : 0.3f, 0.48f);
				V.push_back(E);
			}
		}

		std::vector<FVehicleEntry> Build()
		{
			std::vector<FVehicleEntry> V;

			// ---------------- Electric scooters ----------------
			{
				FVehicleEntry E{ "escooter_commuter", "Ducati Pro-II Evo",
					"Folding commuter e-scooter. 350 W hub motor, 15.5 mph cap, regenerative braking.", {} };
				FVehicleParams& P = E.Params;
				P.Category = EVehicleCategory::EScooter; P.Tyre = ETyre::Hybrid;
				P.Mass = 92.f; P.Wheelbase = 0.86f; P.ComHeight = 1.10f; P.ComFromRear = 0.40f;
				P.WheelRadiusFront = P.WheelRadiusRear = 0.11f;
				P.PitchRadiusOfGyration = 0.42f;
				P.MaxSteerAngle = 0.45f; P.SteerSpeedRef = 5.f; P.LeanResponse = 5.f; P.MaxLean = 0.6f;
				P.TyreGrip = 0.92f; P.RollingResistance = 0.012f; P.CdA = 0.6f; P.TopSpeed = 7.0f;
				P.MaxBrakeForceFront = 420.f; P.MaxBrakeForceRear = 300.f;
				P.Powertrain.Type = EPowertrain::ElectricThrottle;
				P.Powertrain.MotorPeakPowerW = 600.f; P.Powertrain.MotorPeakForceN = 160.f;
				P.Powertrain.MotorCutoffSpeed = 6.94f; P.Powertrain.BatteryWh = 280.f; P.Powertrain.RegenFraction = 0.15f;
				P.Wheelie.RiderLiftTorque = 110.f; P.Wheelie.RiderBalanceTorque = 70.f;
				P.Wheelie.BunnyHopSpeed = 1.0f; P.Wheelie.LoopOutMargin = 0.25f; P.Wheelie.AssistStrength = 0.8f;
				FinishGeometry(P, 0.012f, 0.010f, 0.3f, 0.5f); // tyre-only suspension
				V.push_back(E);
			}
			{
				FVehicleEntry E{ "escooter_performance", "Inokim OXO",
					"Dual-motor, dual-suspension performance scooter with big pneumatic tyres. Private land use in the UK.", {} };
				FVehicleParams& P = E.Params;
				P.Category = EVehicleCategory::EScooter; P.Tyre = ETyre::Knobbly;
				P.Mass = 110.f; P.Wheelbase = 1.05f; P.ComHeight = 1.05f; P.ComFromRear = 0.48f;
				P.WheelRadiusFront = P.WheelRadiusRear = 0.14f;
				P.MaxSteerAngle = 0.45f; P.SteerSpeedRef = 6.f; P.LeanResponse = 5.f; P.MaxLean = 0.65f;
				P.RollingResistance = 0.014f; P.CdA = 0.62f; P.TopSpeed = 13.f;
				P.MaxBrakeForceFront = 700.f; P.MaxBrakeForceRear = 500.f;
				P.Powertrain.Type = EPowertrain::ElectricThrottle;
				P.Powertrain.MotorPeakPowerW = 2400.f; P.Powertrain.MotorPeakForceN = 520.f;
				P.Powertrain.MotorCutoffSpeed = 13.f; P.Powertrain.BatteryWh = 1100.f; P.Powertrain.RegenFraction = 0.2f;
				P.Wheelie.RiderLiftTorque = 160.f; P.Wheelie.RiderBalanceTorque = 100.f;
				P.Wheelie.BunnyHopSpeed = 1.3f; P.Wheelie.LoopOutMargin = 0.28f;
				FinishGeometry(P, 0.06f, 0.05f, 0.25f, 0.45f);
				V.push_back(E);
			}

			// ---------------- Electric bikes (UK EAPC: 250 W nominal, 15.5 mph assist cut-off) ----------------
			auto EBike = [&](const char* Id, const char* Name, const char* Desc, ETyre Tyre, float Mass, float Wb,
				float ComH, float ComR, float WheelR, float TF, float TR, float PeakForce, float PeakPower, float Wh, float Hop, float Lift)
			{
				FVehicleEntry E = Bicycle(Id, Name, Desc, EVehicleCategory::EBike, Tyre, Mass, Wb, ComH, ComR, WheelR, TF, TR, Hop, Lift, 0.55f);
				FPowertrainParams& PT = E.Params.Powertrain;
				PT.Type = EPowertrain::PedalAssist;
				PT.MotorPeakForceN = PeakForce;
				PT.MotorPeakPowerW = PeakPower;
				PT.MotorCutoffSpeed = 6.94f;
				PT.BatteryWh = Wh;
				E.Params.Wheelie.AssistStrength = 1.1f;
				V.push_back(E);
			};
			EBike("ebike_commuter", "Raleigh Motus Grand Tour",
				"Step-through commuter e-bike with hub motor, mudguards and hybrid tyres.",
				ETyre::Hybrid, 106.f, 1.12f, 1.02f, 0.47f, 0.35f, 0.05f, 0.f, 140.f, 450.f, 418.f, 1.3f, 230.f);
			EBike("ebike_hardtail", "Merida eBIG.NINE",
				"Hardtail electric mountain bike, 85 Nm mid-drive, 120 mm fork.",
				ETyre::Knobbly, 103.f, 1.18f, 1.05f, 0.50f, 0.37f, 0.12f, 0.f, 230.f, 600.f, 625.f, 1.9f, 270.f);
			EBike("ebike_fullsus", "Merida eONE-SIXTY",
				"Full-suspension eMTB, 150 mm travel front and rear, 750 Wh battery.",
				ETyre::Knobbly, 106.f, 1.24f, 1.06f, 0.53f, 0.37f, 0.15f, 0.15f, 250.f, 700.f, 750.f, 2.0f, 280.f);

			// ---------------- Traditional bicycles ----------------
			V.push_back(Bicycle("bmx_street", "Mongoose Legion L500",
				"20-inch street/park BMX. Light, short and eager to lift.",
				EVehicleCategory::BMX, ETyre::Knobbly, 86.f, 0.98f, 0.95f, 0.38f, 0.255f, 0.f, 0.f, 3.2f, 300.f, 0.5f));
			V.push_back(Bicycle("mtb_trail", "Orange Stage 4",
				"Trail mountain bike, 130 mm travel, 29-inch wheels.",
				EVehicleCategory::MountainBike, ETyre::Knobbly, 92.f, 1.20f, 1.03f, 0.48f, 0.37f, 0.13f, 0.13f, 2.6f, 250.f, 0.55f));
			V.push_back(Bicycle("road_bike", "Pinarello Dogma F",
				"Carbon road bike with 25 mm slick tyres. Fast on tarmac, nervous on grass.",
				EVehicleCategory::RoadBike, ETyre::Road, 83.f, 0.99f, 1.00f, 0.42f, 0.34f, 0.f, 0.f, 1.8f, 190.f, 0.32f));
			V.push_back(Bicycle("dh_bike", "Atherton AM.200",
				"Downhill race bike, 200 mm travel, slack geometry. Heavy to pedal, unstoppable downhill.",
				EVehicleCategory::DownhillBike, ETyre::Knobbly, 97.f, 1.30f, 1.00f, 0.55f, 0.36f, 0.20f, 0.20f, 2.3f, 240.f, 0.6f));
			V.push_back(Bicycle("freestyle_dj", "Specialized P.3",
				"26-inch dirt jump / freestyle hardtail with short travel fork.",
				EVehicleCategory::BMX, ETyre::Knobbly, 89.f, 1.04f, 0.97f, 0.40f, 0.33f, 0.10f, 0.f, 3.0f, 290.f, 0.5f));

			// ---------------- Dirt bikes ----------------
			{
				const float Mx[] = { 2.33f, 1.88f, 1.56f, 1.33f, 1.14f };
				V.push_back(Motor("dirt_mx250", "Husqvarna FC 250",
					"250cc four-stroke motocross bike. Revvy, light, 5 gears, 300 mm travel.",
					EVehicleCategory::DirtBike, ETyre::Motocross, 180.f, 1.48f, 0.98f, 0.70f, 0.36f, 0.33f, 0.30f, 0.30f,
					27.f, 9500.f, 2000.f, 13500.f, 5, Mx, 11.5f, 6.3f, 30.f, 360.f));
				const float En[] = { 2.79f, 1.99f, 1.53f, 1.22f, 1.00f, 0.84f };
				V.push_back(Motor("dirt_enduro300", "KTM 300 EXC",
					"300cc two-stroke enduro. Torquey, 6 gears, lights and plates.",
					EVehicleCategory::DirtBike, ETyre::Motocross, 186.f, 1.48f, 1.00f, 0.71f, 0.36f, 0.33f, 0.30f, 0.29f,
					38.f, 7500.f, 1600.f, 10000.f, 6, En, 8.6f, 9.0f, 34.f, 360.f));
				const float Tr[] = { 2.80f, 1.80f, 1.35f, 1.10f, 0.92f };
				V.push_back(Motor("dirt_trail125", "Honda CRF125F",
					"Air-cooled 125cc trail bike. Forgiving, light, ideal first dirt bike.",
					EVehicleCategory::DirtBike, ETyre::Motocross, 175.f, 1.30f, 0.95f, 0.60f, 0.33f, 0.30f, 0.20f, 0.18f,
					10.f, 6500.f, 1500.f, 9500.f, 5, Tr, 11.9f, 6.0f, 25.f, 330.f));
			}

			// ---------------- Motorcycles ----------------
			{
				const float Nk[] = { 2.67f, 2.06f, 1.67f, 1.40f, 1.21f, 1.07f };
				V.push_back(Motor("moto_naked700", "Yamaha MT-07",
					"Twin-cylinder naked roadster. Punchy midrange, upright and wheelie-happy.",
					EVehicleCategory::Motorcycle, ETyre::Sport, 265.f, 1.40f, 0.80f, 0.66f, 0.31f, 0.32f, 0.13f, 0.13f,
					68.f, 6500.f, 1300.f, 10000.f, 6, Nk, 5.2f, 14.f, 58.f, 420.f));
				const float Sp[] = { 2.60f, 2.05f, 1.75f, 1.52f, 1.36f, 1.25f };
				V.push_back(Motor("moto_sport1000", "Suzuki GSX-R1000",
					"Inline-four superbike. Brutal top end, quick steering, wants to wheelie in 1st and 2nd.",
					EVehicleCategory::Motorcycle, ETyre::Sport, 278.f, 1.41f, 0.78f, 0.67f, 0.31f, 0.32f, 0.12f, 0.12f,
					113.f, 11000.f, 1300.f, 14000.f, 6, Sp, 4.4f, 17.f, 82.f, 420.f));
				const float Av[] = { 2.80f, 2.00f, 1.55f, 1.27f, 1.08f, 0.93f };
				V.push_back(Motor("moto_adventure", "BMW R 1250 GS",
					"Big adventure tourer. Heavy, tall, long-travel suspension, happy on gravel.",
					EVehicleCategory::Motorcycle, ETyre::Hybrid, 330.f, 1.55f, 0.85f, 0.74f, 0.37f, 0.35f, 0.21f, 0.21f,
					125.f, 6250.f, 1100.f, 9000.f, 6, Av, 4.6f, 20.f, 60.f, 400.f));
				const float Cm[] = { 3.08f, 1.88f, 1.40f, 1.13f, 0.96f };
				V.push_back(Motor("moto_commuter125", "Honda CB125F",
					"Learner-legal 125cc commuter. Light, frugal and very forgiving.",
					EVehicleCategory::Motorcycle, ETyre::Hybrid, 215.f, 1.30f, 0.80f, 0.60f, 0.29f, 0.29f, 0.12f, 0.10f,
					11.f, 7000.f, 1400.f, 10000.f, 5, Cm, 9.5f, 11.f, 30.f, 380.f));
			}
			AddLineup(V);
			AddNamedElectric(V);
			return V;
		}
	}

	const std::vector<FVehicleEntry>& FVehicleCatalog::All()
	{
		static const std::vector<FVehicleEntry> Entries = Build();
		return Entries;
	}

	const FVehicleEntry* FVehicleCatalog::Find(const std::string& Id)
	{
		for (const FVehicleEntry& E : All())
		{
			if (E.Id == Id) return &E;
		}
		return nullptr;
	}

	const char* FVehicleCatalog::CategoryName(EVehicleCategory C)
	{
		switch (C)
		{
		case EVehicleCategory::EScooter:     return "Electric scooter";
		case EVehicleCategory::EBike:        return "Electric bike";
		case EVehicleCategory::BMX:          return "BMX / Freestyle";
		case EVehicleCategory::MountainBike: return "Mountain bike";
		case EVehicleCategory::RoadBike:     return "Road bike";
		case EVehicleCategory::DownhillBike: return "Downhill bike";
		case EVehicleCategory::DirtBike:     return "Dirt bike";
		case EVehicleCategory::Motorcycle:   return "Motorcycle";
		}
		return "Vehicle";
	}
}
