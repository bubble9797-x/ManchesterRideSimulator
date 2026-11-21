#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "Engine/EngineBaseTypes.h"
#include "MRGameInstance.generated.h"

class UMRSaveGame;
class UNetDriver;

UENUM(BlueprintType)
enum class EMRConnectError : uint8
{
	None,
	BadAddress,        // could not parse or resolve the address
	ServerUnavailable, // no reply: host offline, wrong port, or Playit agent/tunnel down
	Timeout,           // connected, then the host stopped responding
	VersionMismatch,
	ServerFull,
	BadPassword,
	NotOnFriendsList,
	Disconnected,      // host closed the session / connection lost
	PortInUse,         // hosting: UDP port already bound
	Other,
};

UCLASS()
class MANCHESTERRIDE_API UMRGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	virtual void Init() override;
	virtual void Shutdown() override;

	UFUNCTION(BlueprintPure) UMRSaveGame* GetProfile() const { return Profile; }
	void SaveProfile();

	// ---- Sessions ----
	/** Opens the world as a listen server on the profile's port (UDP). */
	void HostSession();
	/** Address forms: "host:port", "1.2.3.4:7777", Playit "name.gl.at.ply.gg:12345". Port defaults to 7777. */
	void JoinByAddress(const FString& Address, const FString& Password);
	void LeaveSession();
	/** Restart solo play in the imported Manchester area (bTestDistrict=false) or the test district. */
	void OpenWorld(bool bTestDistrict);
	bool IsTestDistrictSelected() const { return WorldOption == TEXT("test"); }
	void Reconnect();
	bool IsInMultiplayer() const;
	bool IsHosting() const;
	FString GetLastJoinAddress() const { return LastJoinAddress; }
	bool IsJoining() const { return bJoining; }

	/** Called by the player controller when a client connection reaches gameplay. */
	void NotifyConnectedToServer();

	// ---- Errors for the UI ----
	EMRConnectError GetLastError() const { return LastError; }
	FText GetLastErrorText() const { return LastErrorText; }
	void ClearError() { LastError = EMRConnectError::None; LastErrorText = FText::GetEmpty(); }
	void SetError(EMRConnectError Error, const FText& Text);
	FText GetStatusText() const { return StatusText; }

	// ---- Settings ----
	void ApplyGraphicsPreset(int32 Preset);
	static FText GraphicsPresetName(int32 Preset);

	/** Builds the login URL options this client sends (?name=..?ver=..?veh=..). */
	FString BuildLoginOptions(const FString& Password) const;

	static bool ParseAddress(const FString& In, FString& OutHost, int32& OutPort);

private:
	void HandleNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString);
	void HandleTravelFailure(UWorld* World, ETravelFailure::Type FailureType, const FString& ErrorString);
	void BeginTravelToResolved(const FString& HostPort, const FString& Password);
	void RememberServer(const FString& Address);

	UPROPERTY() TObjectPtr<UMRSaveGame> Profile;

	EMRConnectError LastError = EMRConnectError::None;
	FText LastErrorText;
	FText StatusText;
	FString LastJoinAddress;
	FString WorldOption; // "" = Manchester geo data when installed, "test" = test district
	FString LastJoinPassword;
	bool bJoining = false;
	bool bWasConnected = false;
	int32 ReconnectAttempts = 0;
	FTimerHandle ReconnectTimer;
	FDelegateHandle NetFailureHandle, TravelFailureHandle;
};
