#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "RideSimTypes.h"
#include "MRVehicleLibrary.generated.h"

/** Blueprint/UI-facing description of a catalogue vehicle. There is intentionally no price or unlock field. */
USTRUCT(BlueprintType)
struct FMRVehicleInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) FName Id;
	UPROPERTY(BlueprintReadOnly) FText DisplayName;
	UPROPERTY(BlueprintReadOnly) FText Category;
	UPROPERTY(BlueprintReadOnly) FText Description;
	UPROPERTY(BlueprintReadOnly) float MassKg = 0.f;          // vehicle + rider
	UPROPERTY(BlueprintReadOnly) float GovernedTopSpeedMph = 0.f;
	UPROPERTY(BlueprintReadOnly) FText Powertrain;
	UPROPERTY(BlueprintReadOnly) bool bHasBattery = false;
	UPROPERTY(BlueprintReadOnly) bool bHasFuel = false;
	UPROPERTY(BlueprintReadOnly) bool bHasGears = false;
};

UCLASS()
class MANCHESTERRIDE_API UMRVehicleLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Every vehicle in the game. All of them are free and selectable immediately. */
	UFUNCTION(BlueprintPure, Category = "Vehicles")
	static TArray<FMRVehicleInfo> GetAllVehicles();

	UFUNCTION(BlueprintPure, Category = "Vehicles")
	static bool GetVehicleInfo(FName Id, FMRVehicleInfo& OutInfo);

	UFUNCTION(BlueprintPure, Category = "Vehicles")
	static FName GetDefaultVehicleId() { return TEXT("ebike_hardtail"); }

	/** Returns the next/previous vehicle id in catalogue order (wraps). */
	UFUNCTION(BlueprintPure, Category = "Vehicles")
	static FName CycleVehicle(FName Current, int32 Direction);
	/** Next/previous vehicle in the same category (garage list); also reports position within it. */
	static FName CycleWithinCategory(FName Current, int32 Direction, int32& OutIndexInCategory, int32& OutCategoryCount);
	/** Jump to the first vehicle of the next/previous category. */
	static FName CycleCategory(FName Current, int32 Direction);

	static const MRSim::FVehicleParams* FindParams(FName Id);
	static bool IsValidVehicle(FName Id) { return FindParams(Id) != nullptr; }
};
