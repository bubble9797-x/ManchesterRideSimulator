#include "Vehicles/MRVehicleLibrary.h"
#include "VehicleCatalog.h"

namespace
{
	FMRVehicleInfo ToInfo(const MRSim::FVehicleEntry& E)
	{
		using namespace MRSim;
		FMRVehicleInfo I;
		I.Id = FName(UTF8_TO_TCHAR(E.Id.c_str()));
		I.DisplayName = FText::FromString(UTF8_TO_TCHAR(E.DisplayName.c_str()));
		I.Description = FText::FromString(UTF8_TO_TCHAR(E.Description.c_str()));
		I.Category = FText::FromString(UTF8_TO_TCHAR(FVehicleCatalog::CategoryName(E.Params.Category)));
		I.MassKg = E.Params.Mass;
		I.GovernedTopSpeedMph = E.Params.TopSpeed * 2.23694f;
		switch (E.Params.Powertrain.Type)
		{
		case EPowertrain::Pedal:            I.Powertrain = NSLOCTEXT("MR", "Pedal", "Pedal power"); break;
		case EPowertrain::PedalAssist:      I.Powertrain = NSLOCTEXT("MR", "Assist", "Pedal assist (EAPC)"); I.bHasBattery = true; break;
		case EPowertrain::ElectricThrottle: I.Powertrain = NSLOCTEXT("MR", "Hub", "Electric hub motor"); I.bHasBattery = true; break;
		case EPowertrain::Combustion:       I.Powertrain = NSLOCTEXT("MR", "Engine", "Petrol engine, manual gearbox"); I.bHasFuel = true; I.bHasGears = true; break;
		}
		return I;
	}
}

TArray<FMRVehicleInfo> UMRVehicleLibrary::GetAllVehicles()
{
	TArray<FMRVehicleInfo> Out;
	for (const MRSim::FVehicleEntry& E : MRSim::FVehicleCatalog::All())
	{
		Out.Add(ToInfo(E));
	}
	return Out;
}

bool UMRVehicleLibrary::GetVehicleInfo(FName Id, FMRVehicleInfo& OutInfo)
{
	const MRSim::FVehicleEntry* E = MRSim::FVehicleCatalog::Find(TCHAR_TO_UTF8(*Id.ToString()));
	if (!E) return false;
	OutInfo = ToInfo(*E);
	return true;
}

FName UMRVehicleLibrary::CycleVehicle(FName Current, int32 Direction)
{
	const auto& All = MRSim::FVehicleCatalog::All();
	if (All.empty()) return NAME_None;
	const std::string Cur = TCHAR_TO_UTF8(*Current.ToString());
	int32 Index = 0;
	for (int32 i = 0; i < static_cast<int32>(All.size()); ++i)
	{
		if (All[i].Id == Cur) { Index = i; break; }
	}
	const int32 N = static_cast<int32>(All.size());
	Index = ((Index + Direction) % N + N) % N;
	return FName(UTF8_TO_TCHAR(All[Index].Id.c_str()));
}

FName UMRVehicleLibrary::CycleWithinCategory(FName Current, int32 Direction, int32& OutIndexInCategory, int32& OutCategoryCount)
{
	const auto& All = MRSim::FVehicleCatalog::All();
	OutIndexInCategory = 0; OutCategoryCount = 0;
	if (All.empty()) return NAME_None;
	const std::string Cur = TCHAR_TO_UTF8(*Current.ToString());
	const MRSim::FVehicleEntry* CurE = MRSim::FVehicleCatalog::Find(Cur);
	const MRSim::EVehicleCategory Cat = CurE ? CurE->Params.Category : All[0].Params.Category;
	TArray<int32> Members;
	int32 Pos = 0;
	for (int32 i = 0; i < static_cast<int32>(All.size()); ++i)
	{
		if (All[i].Params.Category != Cat) continue;
		if (All[i].Id == Cur) Pos = Members.Num();
		Members.Add(i);
	}
	const int32 N = Members.Num();
	Pos = ((Pos + Direction) % N + N) % N;
	OutIndexInCategory = Pos; OutCategoryCount = N;
	return FName(UTF8_TO_TCHAR(All[Members[Pos]].Id.c_str()));
}

FName UMRVehicleLibrary::CycleCategory(FName Current, int32 Direction)
{
	const auto& All = MRSim::FVehicleCatalog::All();
	if (All.empty()) return NAME_None;
	const MRSim::FVehicleEntry* CurE = MRSim::FVehicleCatalog::Find(TCHAR_TO_UTF8(*Current.ToString()));
	constexpr int32 NumCats = 8;
	const int32 Cat = CurE ? static_cast<int32>(CurE->Params.Category) : 0;
	for (int32 Step = 1; Step <= NumCats; ++Step)
	{
		const int32 Want = ((Cat + Direction * Step) % NumCats + NumCats) % NumCats;
		for (const MRSim::FVehicleEntry& E : All)
		{
			if (static_cast<int32>(E.Params.Category) == Want) return FName(UTF8_TO_TCHAR(E.Id.c_str())); // first (hand-tuned) model of that category
		}
	}
	return Current;
}

const MRSim::FVehicleParams* UMRVehicleLibrary::FindParams(FName Id)
{
	const MRSim::FVehicleEntry* E = MRSim::FVehicleCatalog::Find(TCHAR_TO_UTF8(*Id.ToString()));
	return E ? &E->Params : nullptr;
}
