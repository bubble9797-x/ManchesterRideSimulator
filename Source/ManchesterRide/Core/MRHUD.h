#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "MRHUD.generated.h"

class AMRPlayerController;
class AMRRideVehicle;
class UTexture2D;

/**
 * Canvas-drawn HUD and menus. Drawing in C++ keeps the project free of binary
 * UMG assets during early milestones; the layout is resolution-independent
 * (scaled from a 1920x1080 reference). A UMG skin can replace it later without
 * changing the menu model in AMRPlayerController.
 */
UCLASS()
class MANCHESTERRIDE_API AMRHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	void DrawMenu(AMRPlayerController* PC);
	void DrawRidingHUD(AMRPlayerController* PC, AMRRideVehicle* V);
	void DrawNameTags(AMRRideVehicle* Self);
	void DrawMinimap(AMRRideVehicle* Self);
	void DrawFullMap(AMRPlayerController* PC);
	void DrawPlayerList();
	void DrawCompetition();
	void DrawSessionInfo(AMRPlayerController* PC);
	void DrawIntro();
	// Dedicated server
	void DrawChat(AMRPlayerController* PC);
	void DrawVoice();
	void DrawServerPlayerList();
	void DrawUpdate();

	void Text(const FString& S, float X, float Y, const FLinearColor& C, float Scale = 1.f, bool bRightAlign = false, bool bCenter = false, UFont* Font = nullptr);
	void Panel(float X, float Y, float W, float H, const FLinearColor& C);
	void Bar(float X, float Y, float W, float H, float Fraction, const FLinearColor& Fill);

	/** Logo for the opening screen, loaded from Content/Brand/Logo.png (a loose file, see DefaultGame.ini). */
	UPROPERTY(Transient) TObjectPtr<UTexture2D> LogoTexture;
	bool bLogoTried = false;

	float S = 1.f; // UI scale relative to 1080p
};
