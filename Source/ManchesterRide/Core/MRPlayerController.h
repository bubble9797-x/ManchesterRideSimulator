#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Core/MRGameState.h"
#include "Core/MRRiderLook.h"
#include "MRPlayerController.generated.h"

class AMRRideVehicle;
class UMRGameInstance;
class UMRSaveGame;
struct FInputActionValue;

UENUM()
enum class EMRMenuScreen : uint8
{
	None,       // riding
	Main,
	Multiplayer,
	Host,
	Join,
	Browser,
	Garage,
	Rider,      // character editor
	Settings,
	Controls,
	Credits,
	Server,         // the Manchester Ride dedicated server: join / account / voice
	ServerLogin,
	ServerRegister,
};

/** One row in the canvas menu (drawn by AMRHUD). Left/Right adjust, Confirm activates. */
struct FMRMenuItem
{
	FText Label;
	FText Value;          // for text boxes: the grey hint shown while empty
	TFunction<void()> OnConfirm;
	TFunction<void(int32)> OnAdjust;
	bool bEnabled = true;
	FString* EditText = nullptr; // set: a text box you type into while it's selected
	bool bPassword = false;
	int32 MaxLen = 64;
};

class UMRServerClient;

UCLASS()
class MANCHESTERRIDE_API AMRPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AMRPlayerController();

	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float DeltaTime) override;
	virtual bool InputKey(const FInputKeyParams& Params) override;

	// ---- Menu model for the HUD ----
	EMRMenuScreen GetMenuScreen() const { return Screen; }
	bool IsMenuOpen() const { return Screen != EMRMenuScreen::None; }
	const TArray<FMRMenuItem>& GetMenuItems() const { return Items; }
	int32 GetMenuSelection() const { return Selected; }
	FText GetMenuTitle() const { return Title; }
	FText GetMenuFooter() const { return Footer; }
	bool IsAwaitingKey() const { return bAwaitingKey; }
	bool IsPlayerListHeld() const { return bPlayerListHeld; }

	// ---- Typing: chat (T / Enter / '/') and the login screen's text boxes ----
	/** A typed character from UMRViewportClient. True if it was used. */
	bool HandleTextChar(TCHAR C);
	bool IsChatOpen() const { return bChatOpen; }
	const FString& GetChatDraft() const { return ChatDraft; }
	bool IsEditingText() const;
	void OpenChat(const FString& Prefill);
	void CloseChat();
	UMRServerClient* GetServer() const;

	// Full-screen map (M)
	bool IsMapOpen() const { return bMapOpen; }
	FVector2D GetMapCentre() const { return MapCentre; }
	float GetMapCmPerPixel() const { return MapCmPerPixel; }
	void CloseMap();

	// Waypoint sat-nav: set on the full map, routed along the road network (blue line on both maps).
	bool HasWaypoint() const { return bHasWaypoint; }
	FVector2D GetWaypoint() const { return Waypoint; }
	const TArray<FVector2f>& GetRoute() const { return Route; }
	int32 GetRouteProgressSeg() const { return RouteSeg; }       // route segment you are on (draw from here)
	FVector2f GetRouteProgressPoint() const { return RouteHere; }
	float GetRouteRemainingCm() const { return RouteRemainingCm; }
	bool IsRouting() const { return bRouting; }
	bool IsRouteFailed() const { return bRouteFailed; }
	float GetArrivedTime() const { return ArrivedTime; }        // world seconds when you last arrived (for the HUD message)
	void SetWaypoint(const FVector2D& P);
	void ClearWaypoint();
	/** Jumps straight to the waypoint (the nearest road to it), then clears it. */
	void TeleportToWaypoint();
	UFUNCTION(Exec) void MRWaypoint(float LatOrX, float LonOrY);

	void OpenMenu(EMRMenuScreen NewScreen);
	void CloseMenu();

	AMRRideVehicle* GetVehicle() const;
	UMRSaveGame* GetProfile() const;
	UMRGameInstance* GetMRGameInstance() const;

	// ---- Console commands (` key): handy for testing and for pasting addresses ----
	UFUNCTION(Exec) void MRHost();
	UFUNCTION(Exec) void MRJoin(const FString& Address, const FString& Password);
	UFUNCTION(Exec) void MRLeave();
	UFUNCTION(Exec) void MRVehicle(const FString& VehicleId);
	UFUNCTION(Exec) void MRListVehicles();
	UFUNCTION(Exec) void MRName(const FString& Name);
	UFUNCTION(Exec) void MRPassword(const FString& Password);
	UFUNCTION(Exec) void MRFriend(const FString& Name);
	UFUNCTION(Exec) void MRPlayit(const FString& PublicAddress);
	UFUNCTION(Exec) void MRPort(int32 Port);
	UFUNCTION(Exec) void MRMaxPlayers(int32 Count);
	UFUNCTION(Exec) void MRGraphics(int32 Preset);
	UFUNCTION(Exec) void MRAssist(const FString& Mode);
	UFUNCTION(Exec) void MRWeather(int32 Weather);
	UFUNCTION(Exec) void MRTime(float Hours);
	UFUNCTION(Exec) void MRBind(const FString& Action, const FString& Key);
	UFUNCTION(Exec) void MRCompetition(const FString& Type, float Seconds);

protected:
	UFUNCTION(Server, Reliable) void ServerChangeVehicle(FName VehicleId, FLinearColor Paint, FLinearColor Jacket, FLinearColor Helmet);
	UFUNCTION(Server, Reliable) void ServerSetRiderLook(FMRRiderLook Look, FLinearColor Jacket, FLinearColor Helmet);
	UFUNCTION(Server, Reliable) void ServerStartCompetition(EMRCompetition Type, float Seconds);
	UFUNCTION(Server, Reliable) void ServerSetEnvironment(int32 Weather, float Hours);

private:
	// Input handlers
	void OnGearUp(const FInputActionValue& V);
	void OnGearDown(const FInputActionValue& V);
	void OnAssistUp(const FInputActionValue& V);
	void OnAssistDown(const FInputActionValue& V);
	void OnCamera(const FInputActionValue& V);
	void OnRespawn(const FInputActionValue& V);
	void OnOpenMenu(const FInputActionValue& V);
	void OnOpenMap(const FInputActionValue& V);
	void UpdateMapInput(float DeltaTime);
	bool bMapOpen = false;
	FVector2D MapCentre = FVector2D::ZeroVector;
	float MapCmPerPixel = 0.f;
	uint64 MapOpenedFrame = 0;
	void UpdateRoute();
	void RequestRoute();
	bool bHasWaypoint = false;
	FVector2D Waypoint = FVector2D::ZeroVector;
	TArray<FVector2f> Route;
	int32 RouteSeg = 0;
	FVector2f RouteHere = FVector2f::ZeroVector;
	float RouteRemainingCm = 0.f;
	bool bRouting = false;
	bool bRouteFailed = false;
	int32 RouteRequestId = 0;
	double LastRouteRequest = -100.0;
	float ArrivedTime = -100.f;
	void OnMenuUp(const FInputActionValue& V);
	void OnMenuDown(const FInputActionValue& V);
	void OnMenuLeft(const FInputActionValue& V);
	void OnMenuRight(const FInputActionValue& V);
	void OnMenuConfirm(const FInputActionValue& V);
	void OnMenuBack(const FInputActionValue& V);

	void BindAllActions();
	void ApplyInputContexts();
	void UpdateRidingInput(float DeltaTime);
	void ApplyProfileToVehicle();
	void RebuildMenu();
	template <typename FAddFn, typename FInfoFn, typename FGoFn> void BuildServerMenu(FAddFn& Add, FInfoFn& Info, FGoFn& Go);
	void MoveSelection(int32 Dir);
	void ApplyCustomisation(bool bRespawnVehicle);
	void SaveProfile();

	EMRMenuScreen Screen = EMRMenuScreen::None;
	TArray<EMRMenuScreen> ScreenStack;
	TArray<FMRMenuItem> Items;
	FText Title, Footer;
	int32 Selected = 0;
	bool bAwaitingKey = false;
	FName RebindAction;
	bool bPlayerListHeld = false;

	FString JoinAddressDraft;
	FString JoinPasswordDraft;

	bool bChatOpen = false;
	FString ChatDraft;
	TCHAR SkipChar = 0;   // the key that opened chat also arrives as a character: don't type it
	FString LoginName, LoginPassword, RegName, RegPassword, RegPassword2;
	bool bRememberLogin = true;
	FText LocalFormError;
	double LastMenuRefresh = 0.0;
	bool bPhotoTokenPasteFailed = false;
	bool bReversing = false;
	FName GarageVehicle;

	TWeakObjectPtr<AMRRideVehicle> LastVehicle;
	float SmoothedSteer = 0.f;
	bool bInputBound = false;
	static bool bShownStartMenu;
};
