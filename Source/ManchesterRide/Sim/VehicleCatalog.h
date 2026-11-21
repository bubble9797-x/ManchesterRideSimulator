// Manchester Riding Simulator - built-in vehicle catalogue.
//
// EVERY vehicle in this catalogue is free and available from the first launch.
// There is deliberately no price, unlock flag or ownership concept anywhere in
// the vehicle data model (see docs/ARCHITECTURE.md "Free vehicles policy").
#pragma once

#include "RideSimTypes.h"

#include <string>
#include <vector>

namespace MRSim
{
	struct FVehicleEntry
	{
		std::string Id;           // stable id used in saves and replication
		std::string DisplayName;
		std::string Description;
		FVehicleParams Params;
	};

	class FVehicleCatalog
	{
	public:
		static const std::vector<FVehicleEntry>& All();
		static const FVehicleEntry* Find(const std::string& Id);
		static const char* CategoryName(EVehicleCategory C);
	};
}
