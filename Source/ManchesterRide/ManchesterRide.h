#pragma once

#include "CoreMinimal.h"

DECLARE_LOG_CATEGORY_EXTERN(LogManchesterRide, Log, All);

// The simulation (Source/ManchesterRide/Sim) works in metres; Unreal in centimetres.
namespace MRUnits
{
	constexpr float MetresToUU = 100.f;
	constexpr float UUToMetres = 0.01f;
}
