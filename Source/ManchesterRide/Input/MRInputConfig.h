#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "InputCoreTypes.h"
#include "MRInputConfig.generated.h"

class UInputAction;
class UInputMappingContext;
struct FMRKeyBinding;

/**
 * Builds every Enhanced Input action and mapping context in C++ at startup.
 * No .uasset input files are needed and every binding is rebindable at runtime
 * (overrides are stored in the save game and re-applied by Rebuild()).
 *
 * Default layout (Xbox naming; PlayStation equivalents in docs/CONTROLS.md):
 *   RT throttle/pedal  LT front brake  LB rear brake  RB WHEELIE (dedicated, rebindable)
 *   Left stick X steer, Y lean back/forward (wheelie balance)  Right stick camera
 *   A bunny hop  B clutch  Y gear up  X gear down  D-pad up/down assist level
 *   D-pad left respawn  View camera mode  Menu pause/menu
 */
UCLASS()
class MANCHESTERRIDE_API UMRInputConfig : public UObject
{
	GENERATED_BODY()

public:
	static UMRInputConfig* Get();

	/** Re-create mapping contexts, applying user overrides. */
	void Rebuild(const TArray<FMRKeyBinding>& Overrides, bool bInvertLookY);

	UPROPERTY() TObjectPtr<UInputMappingContext> RidingContext;
	UPROPERTY() TObjectPtr<UInputMappingContext> MenuContext;

	// Riding
	UPROPERTY() TObjectPtr<UInputAction> Throttle;
	UPROPERTY() TObjectPtr<UInputAction> FrontBrake;
	UPROPERTY() TObjectPtr<UInputAction> RearBrake;
	UPROPERTY() TObjectPtr<UInputAction> Steer;
	UPROPERTY() TObjectPtr<UInputAction> Balance;
	UPROPERTY() TObjectPtr<UInputAction> Wheelie;
	UPROPERTY() TObjectPtr<UInputAction> Hop;
	UPROPERTY() TObjectPtr<UInputAction> Clutch;
	UPROPERTY() TObjectPtr<UInputAction> GearUp;
	UPROPERTY() TObjectPtr<UInputAction> GearDown;
	UPROPERTY() TObjectPtr<UInputAction> AssistUp;
	UPROPERTY() TObjectPtr<UInputAction> AssistDown;
	UPROPERTY() TObjectPtr<UInputAction> Look;     // mouse delta (pixels)
	UPROPERTY() TObjectPtr<UInputAction> LookPad;  // right stick (-1..1, scaled per second)
	UPROPERTY() TObjectPtr<UInputAction> CameraToggle;
	UPROPERTY() TObjectPtr<UInputAction> Respawn;
	UPROPERTY() TObjectPtr<UInputAction> OpenMap;
	UPROPERTY() TObjectPtr<UInputAction> OpenMenu;
	UPROPERTY() TObjectPtr<UInputAction> PlayerList;

	// Menu navigation
	UPROPERTY() TObjectPtr<UInputAction> MenuUp;
	UPROPERTY() TObjectPtr<UInputAction> MenuDown;
	UPROPERTY() TObjectPtr<UInputAction> MenuLeft;
	UPROPERTY() TObjectPtr<UInputAction> MenuRight;
	UPROPERTY() TObjectPtr<UInputAction> MenuConfirm;
	UPROPERTY() TObjectPtr<UInputAction> MenuBack;

	/** Rebindable riding actions in display order (for the controls menu). */
	struct FBindable { FName Name; FText Label; UInputAction* Action; FKey DefaultKeyboard; FKey DefaultGamepad; };
	const TArray<FBindable>& GetBindables() const { return Bindables; }
	FKey GetKeyboardKey(FName Action) const;
	FKey GetGamepadKey(FName Action) const;

private:
	void CreateActions();
	TArray<FBindable> Bindables;
	TMap<FName, FKey> ActiveKeyboard;
	TMap<FName, FKey> ActiveGamepad;
};
