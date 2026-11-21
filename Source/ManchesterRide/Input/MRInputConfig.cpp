#include "Input/MRInputConfig.h"

#include "Core/MRSaveGame.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"

namespace
{
	UInputAction* MakeAction(UObject* Outer, const TCHAR* Name, EInputActionValueType Type)
	{
		UInputAction* A = NewObject<UInputAction>(Outer, FName(Name));
		A->ValueType = Type;
		return A;
	}

	void AddNegate(UInputMappingContext* Ctx, FEnhancedActionKeyMapping& M, bool bX = true, bool bY = true)
	{
		UInputModifierNegate* Neg = NewObject<UInputModifierNegate>(Ctx);
		Neg->bX = bX;
		Neg->bY = bY;
		Neg->bZ = false;
		M.Modifiers.Add(Neg);
	}

	void AddDeadZone(UInputMappingContext* Ctx, FEnhancedActionKeyMapping& M, float Lower = 0.12f)
	{
		UInputModifierDeadZone* DZ = NewObject<UInputModifierDeadZone>(Ctx);
		DZ->LowerThreshold = Lower;
		DZ->UpperThreshold = 1.f;
		M.Modifiers.Add(DZ);
	}
}

UMRInputConfig* UMRInputConfig::Get()
{
	static UMRInputConfig* Instance = nullptr; // rooted below, lives for the process
	if (!Instance)
	{
		Instance = NewObject<UMRInputConfig>(GetTransientPackage(), TEXT("MRInputConfig"));
		Instance->AddToRoot();
		Instance->CreateActions();
		Instance->Rebuild({}, false);
	}
	return Instance;
}

void UMRInputConfig::CreateActions()
{
	Throttle     = MakeAction(this, TEXT("IA_Throttle"), EInputActionValueType::Axis1D);
	FrontBrake   = MakeAction(this, TEXT("IA_FrontBrake"), EInputActionValueType::Axis1D);
	RearBrake    = MakeAction(this, TEXT("IA_RearBrake"), EInputActionValueType::Axis1D);
	Steer        = MakeAction(this, TEXT("IA_Steer"), EInputActionValueType::Axis1D);
	Balance      = MakeAction(this, TEXT("IA_Balance"), EInputActionValueType::Axis1D);
	Wheelie      = MakeAction(this, TEXT("IA_Wheelie"), EInputActionValueType::Boolean);
	Hop          = MakeAction(this, TEXT("IA_Hop"), EInputActionValueType::Boolean);
	Clutch       = MakeAction(this, TEXT("IA_Clutch"), EInputActionValueType::Axis1D);
	GearUp       = MakeAction(this, TEXT("IA_GearUp"), EInputActionValueType::Boolean);
	GearDown     = MakeAction(this, TEXT("IA_GearDown"), EInputActionValueType::Boolean);
	AssistUp     = MakeAction(this, TEXT("IA_AssistUp"), EInputActionValueType::Boolean);
	AssistDown   = MakeAction(this, TEXT("IA_AssistDown"), EInputActionValueType::Boolean);
	Look         = MakeAction(this, TEXT("IA_Look"), EInputActionValueType::Axis2D);
	LookPad      = MakeAction(this, TEXT("IA_LookPad"), EInputActionValueType::Axis2D);
	CameraToggle = MakeAction(this, TEXT("IA_Camera"), EInputActionValueType::Boolean);
	Respawn      = MakeAction(this, TEXT("IA_Respawn"), EInputActionValueType::Boolean);
	OpenMenu     = MakeAction(this, TEXT("IA_Menu"), EInputActionValueType::Boolean);
	PlayerList   = MakeAction(this, TEXT("IA_PlayerList"), EInputActionValueType::Boolean);
	OpenMap      = MakeAction(this, TEXT("IA_Map"), EInputActionValueType::Boolean);

	MenuUp       = MakeAction(this, TEXT("IA_MenuUp"), EInputActionValueType::Boolean);
	MenuDown     = MakeAction(this, TEXT("IA_MenuDown"), EInputActionValueType::Boolean);
	MenuLeft     = MakeAction(this, TEXT("IA_MenuLeft"), EInputActionValueType::Boolean);
	MenuRight    = MakeAction(this, TEXT("IA_MenuRight"), EInputActionValueType::Boolean);
	MenuConfirm  = MakeAction(this, TEXT("IA_MenuConfirm"), EInputActionValueType::Boolean);
	MenuBack     = MakeAction(this, TEXT("IA_MenuBack"), EInputActionValueType::Boolean);

	// Rebindable single-key actions (axes like steer/balance/look keep their pairs; see docs/CONTROLS.md)
	Bindables = {
		{ TEXT("Throttle"),   NSLOCTEXT("MR", "BThrottle", "Throttle / pedal"),     Throttle,     EKeys::W,           EKeys::Gamepad_RightTriggerAxis },
		{ TEXT("FrontBrake"), NSLOCTEXT("MR", "BFront", "Front brake"),             FrontBrake,   EKeys::S,           EKeys::Gamepad_LeftTriggerAxis },
		{ TEXT("RearBrake"),  NSLOCTEXT("MR", "BRear", "Rear brake"),               RearBrake,    EKeys::X,           EKeys::Gamepad_LeftShoulder },
		{ TEXT("Wheelie"),    NSLOCTEXT("MR", "BWheelie", "Wheelie"),               Wheelie,      EKeys::LeftShift,   EKeys::Gamepad_RightShoulder },
		{ TEXT("Hop"),        NSLOCTEXT("MR", "BHop", "Bunny hop / jump"),          Hop,          EKeys::SpaceBar,    EKeys::Gamepad_FaceButton_Bottom },
		{ TEXT("Clutch"),     NSLOCTEXT("MR", "BClutch", "Clutch (hold)"),          Clutch,       EKeys::LeftControl, EKeys::Gamepad_FaceButton_Right },
		{ TEXT("GearUp"),     NSLOCTEXT("MR", "BGearUp", "Gear up"),                GearUp,       EKeys::E,           EKeys::Gamepad_FaceButton_Top },
		{ TEXT("GearDown"),   NSLOCTEXT("MR", "BGearDown", "Gear down"),            GearDown,     EKeys::Q,           EKeys::Gamepad_FaceButton_Left },
		{ TEXT("AssistUp"),   NSLOCTEXT("MR", "BAssistUp", "E-bike assist up"),     AssistUp,     EKeys::PageUp,      EKeys::Gamepad_DPad_Up },
		{ TEXT("AssistDown"), NSLOCTEXT("MR", "BAssistDown", "E-bike assist down"), AssistDown,   EKeys::PageDown,    EKeys::Gamepad_DPad_Down },
		{ TEXT("Camera"),     NSLOCTEXT("MR", "BCamera", "Camera (1st / 3rd)"),     CameraToggle, EKeys::C,           EKeys::Gamepad_Special_Left },
		{ TEXT("Respawn"),    NSLOCTEXT("MR", "BRespawn", "Respawn"),               Respawn,      EKeys::R,           EKeys::Gamepad_DPad_Left },
		{ TEXT("PlayerList"), NSLOCTEXT("MR", "BPlayers", "Player list"),           PlayerList,   EKeys::Tab,         EKeys::Gamepad_DPad_Right },
		{ TEXT("Map"),        NSLOCTEXT("MR", "BMap", "Map (full screen)"),           OpenMap,      EKeys::M,           EKeys::Gamepad_RightThumbstick },
	};
}

FKey UMRInputConfig::GetKeyboardKey(FName Action) const
{
	const FKey* K = ActiveKeyboard.Find(Action);
	return K ? *K : EKeys::Invalid;
}

FKey UMRInputConfig::GetGamepadKey(FName Action) const
{
	const FKey* K = ActiveGamepad.Find(Action);
	return K ? *K : EKeys::Invalid;
}

void UMRInputConfig::Rebuild(const TArray<FMRKeyBinding>& Overrides, bool bInvertLookY)
{
	RidingContext = NewObject<UInputMappingContext>(this); // unique names: Rebuild() can run many times
	MenuContext = NewObject<UInputMappingContext>(this);
	UInputMappingContext* R = RidingContext;
	ActiveKeyboard.Reset();
	ActiveGamepad.Reset();

	for (const FBindable& B : Bindables)
	{
		FKey Kb = B.DefaultKeyboard, Pad = B.DefaultGamepad;
		for (const FMRKeyBinding& O : Overrides)
		{
			if (O.Action == B.Name)
			{
				if (O.Keyboard.IsValid()) Kb = O.Keyboard;
				if (O.Gamepad.IsValid()) Pad = O.Gamepad;
			}
		}
		ActiveKeyboard.Add(B.Name, Kb);
		ActiveGamepad.Add(B.Name, Pad);
		R->MapKey(B.Action, Kb);
		FEnhancedActionKeyMapping& PadMap = R->MapKey(B.Action, Pad);
		if (Pad.IsAnalog()) AddDeadZone(R, PadMap, 0.05f);
	}

	// Paired / analog axes
	{
		FEnhancedActionKeyMapping& M = R->MapKey(Steer, EKeys::Gamepad_LeftX);
		AddDeadZone(R, M, 0.12f);
		R->MapKey(Steer, EKeys::D);
		FEnhancedActionKeyMapping& L = R->MapKey(Steer, EKeys::A);
		AddNegate(R, L);
	}
	{
		// Pulling back on the stick (stick Y negative) = lean back = nose up.
		FEnhancedActionKeyMapping& M = R->MapKey(Balance, EKeys::Gamepad_LeftY);
		AddDeadZone(R, M, 0.15f);
		AddNegate(R, M);
		R->MapKey(Balance, EKeys::Down);
		FEnhancedActionKeyMapping& F = R->MapKey(Balance, EKeys::Up);
		AddNegate(R, F);
	}
	{
		FEnhancedActionKeyMapping& Mouse = R->MapKey(Look, EKeys::Mouse2D);
		if (bInvertLookY) AddNegate(R, Mouse, false, true);
		FEnhancedActionKeyMapping& Pad = R->MapKey(LookPad, EKeys::Gamepad_Right2D);
		AddDeadZone(R, Pad, 0.15f);
		if (bInvertLookY) AddNegate(R, Pad, false, true);
	}
	R->MapKey(OpenMenu, EKeys::Escape);
	R->MapKey(OpenMenu, EKeys::Gamepad_Special_Right);

	// Menu context (keyboard arrows/enter/escape, d-pad, left stick, A/B)
	UInputMappingContext* Mn = MenuContext;
	Mn->MapKey(MenuUp, EKeys::Up);            Mn->MapKey(MenuUp, EKeys::Gamepad_DPad_Up);       Mn->MapKey(MenuUp, EKeys::W);
	Mn->MapKey(MenuDown, EKeys::Down);        Mn->MapKey(MenuDown, EKeys::Gamepad_DPad_Down);   Mn->MapKey(MenuDown, EKeys::S);
	Mn->MapKey(MenuLeft, EKeys::Left);        Mn->MapKey(MenuLeft, EKeys::Gamepad_DPad_Left);   Mn->MapKey(MenuLeft, EKeys::A);
	Mn->MapKey(MenuRight, EKeys::Right);      Mn->MapKey(MenuRight, EKeys::Gamepad_DPad_Right); Mn->MapKey(MenuRight, EKeys::D);
	Mn->MapKey(MenuConfirm, EKeys::Enter);    Mn->MapKey(MenuConfirm, EKeys::Gamepad_FaceButton_Bottom); Mn->MapKey(MenuConfirm, EKeys::SpaceBar);
	Mn->MapKey(MenuBack, EKeys::Escape);      Mn->MapKey(MenuBack, EKeys::Gamepad_FaceButton_Right);     Mn->MapKey(MenuBack, EKeys::BackSpace);
	Mn->MapKey(MenuBack, EKeys::Gamepad_Special_Right);
}
