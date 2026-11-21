#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "InputCoreTypes.h"
#include "Core/MRRiderLook.h"
#include "MRSaveGame.generated.h"

UENUM(BlueprintType)
enum class EMRSessionVisibility : uint8
{
	Public,      // anyone with the address can join
	Private,     // password required
	FriendsOnly, // player name must be on the host's friends list (see docs: names are not verified identities)
};

USTRUCT(BlueprintType)
struct FMRServerEntry
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite) FString Name;
	UPROPERTY(BlueprintReadWrite) FString Address; // e.g. "sample-words.gl.at.ply.gg:12345" or "203.0.113.5:7777"
	UPROPERTY(BlueprintReadWrite) FDateTime LastJoined;
};

/** A user key binding override: Action name -> keyboard key and gamepad key. */
USTRUCT(BlueprintType)
struct FMRKeyBinding
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite) FName Action;
	UPROPERTY(BlueprintReadWrite) FKey Keyboard;
	UPROPERTY(BlueprintReadWrite) FKey Gamepad;
};

/** Everything persistent about the local player. One slot: "Profile". */
UCLASS()
class MANCHESTERRIDE_API UMRSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	static constexpr const TCHAR* SlotName = TEXT("Profile");

	// Profile & appearance
	UPROPERTY() FString PlayerName = TEXT("Rider");
	UPROPERTY() FName VehicleId = TEXT("ebike_hardtail");
	UPROPERTY() FLinearColor PaintColor = FLinearColor(0.75f, 0.08f, 0.08f);
	UPROPERTY() FLinearColor JacketColor = FLinearColor(0.04f, 0.10f, 0.35f);
	UPROPERTY() FLinearColor HelmetColor = FLinearColor(0.92f, 0.92f, 0.92f);
	UPROPERTY() FMRRiderLook RiderLook; // character editor (Main menu > Rider)
	UPROPERTY() TMap<FName, FLinearColor> PaintPerVehicle; // remembers your colour for each vehicle

	// Riding assists & sensitivity
	UPROPERTY() uint8 WheelieAssist = 0;   // MRSim::EWheelieAssist
	UPROPERTY() bool bAutoClutch = true;
	UPROPERTY() bool bAutoShift = false;
	UPROPERTY() bool bLandingAssist = true;
	UPROPERTY() bool bABS = true;
	UPROPERTY() float SteerSensitivity = 1.f;
	UPROPERTY() float BrakeSensitivity = 1.f;
	UPROPERTY() float CameraSensitivity = 1.f;
	UPROPERTY() bool bInvertLookY = false;
	UPROPERTY() bool bVibration = true;
	/** Photoreal map: Google Photorealistic 3D Tiles streamed through Cesium ion (needs the Cesium plugin). */
	UPROPERTY() bool bPhotoMap = false;
	/** The player's own Cesium ion access token, pasted in Settings. Stored only in the local profile. */
	UPROPERTY() FString PhotoMapToken;
	UPROPERTY() bool bFirstPerson = false;
	UPROPERTY() int32 AssistLevel = 2;
	UPROPERTY() TArray<FMRKeyBinding> KeyBindings;

	// Graphics (0 Low, 1 Medium, 2 High, 3 Ultra)
	UPROPERTY() int32 GraphicsPreset = 2;

	// Multiplayer
	UPROPERTY() FString SessionName = TEXT("Manchester Freeride");
	UPROPERTY() int32 HostPort = 7777;
	UPROPERTY() int32 HostMaxPlayers = 8;
	UPROPERTY() EMRSessionVisibility HostVisibility = EMRSessionVisibility::Public;
	UPROPERTY() FString HostPassword;
	UPROPERTY() TArray<FString> FriendsList;
	UPROPERTY() FString PlayitPublicAddress; // pasted from the Playit dashboard, shown to share
	UPROPERTY() TArray<FMRServerEntry> Favourites;
	UPROPERTY() TArray<FMRServerEntry> RecentServers;

	// Dedicated server: voice and chat (the login itself is kept encrypted in Saved/Server/login.bin, not here)
	UPROPERTY() uint8 VoiceMode = 1;      // 0 off, 1 push-to-talk (hold V), 2 open mic
	UPROPERTY() uint8 VoiceRange = 0;     // 0 everyone on the server, 1 only riders near you
	UPROPERTY() float VoiceVolume = 1.f;
	UPROPERTY() float MicVolume = 1.f;
	/** Server identity keys seen before ("host:port" -> hex). Warns if a server's key ever changes. */
	UPROPERTY() TMap<FString, FString> ServerKeys;

	// Records
	UPROPERTY() float BestWheelieDistance = 0.f;
	UPROPERTY() float BestWheelieTime = 0.f;
	UPROPERTY() int32 BestTrickScore = 0;
	UPROPERTY() double TotalDistanceKm = 0.0;
};
