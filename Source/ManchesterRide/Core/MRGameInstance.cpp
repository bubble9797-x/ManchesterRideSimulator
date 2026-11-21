#include "Core/MRGameInstance.h"

#include "ManchesterRide.h"
#include "Core/MRSaveGame.h"
#include "Net/MRNetworkSettings.h"

#include "Async/Async.h"
#include "Engine/Engine.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "GameFramework/GameUserSettings.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/IConsoleManager.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "SocketSubsystem.h"
#include "TimerManager.h"

#define LOCTEXT_NAMESPACE "MRNet"

namespace
{
	// Error codes the server puts in PreLogin's ErrorMessage; the client receives them
	// verbatim through ENetworkFailure::FailureReceived.
	const TCHAR* CodeFull = TEXT("MR_FULL");
	const TCHAR* CodeVersion = TEXT("MR_VERSION");
	const TCHAR* CodePassword = TEXT("MR_PASSWORD");
	const TCHAR* CodeFriends = TEXT("MR_FRIENDS");

	FString ColorToHex(const FLinearColor& C) { return C.ToFColor(true).ToHex(); }
}

void UMRGameInstance::Init()
{
	Super::Init();

	if (UGameplayStatics::DoesSaveGameExist(UMRSaveGame::SlotName, 0))
	{
		Profile = Cast<UMRSaveGame>(UGameplayStatics::LoadGameFromSlot(UMRSaveGame::SlotName, 0));
	}
	if (!Profile)
	{
		Profile = Cast<UMRSaveGame>(UGameplayStatics::CreateSaveGameObject(UMRSaveGame::StaticClass()));
		Profile->HostPort = GetDefault<UMRNetworkSettings>()->DefaultPort;
		Profile->HostMaxPlayers = GetDefault<UMRNetworkSettings>()->DefaultMaxPlayers;
		const FString OsUser = FPlatformProcess::UserName(false);
		if (!OsUser.IsEmpty()) Profile->PlayerName = OsUser.Left(20);
		SaveProfile();
	}

	if (GEngine)
	{
		NetFailureHandle = GEngine->OnNetworkFailure().AddUObject(this, &UMRGameInstance::HandleNetworkFailure);
		TravelFailureHandle = GEngine->OnTravelFailure().AddUObject(this, &UMRGameInstance::HandleTravelFailure);
	}
	if (!IsRunningDedicatedServer())
	{
		ApplyGraphicsPreset(Profile->GraphicsPreset);
	}
}

void UMRGameInstance::Shutdown()
{
	if (GEngine)
	{
		GEngine->OnNetworkFailure().Remove(NetFailureHandle);
		GEngine->OnTravelFailure().Remove(TravelFailureHandle);
	}
	SaveProfile();
	Super::Shutdown();
}

void UMRGameInstance::SaveProfile()
{
	if (Profile)
	{
		UGameplayStatics::SaveGameToSlot(Profile, UMRSaveGame::SlotName, 0);
	}
}

void UMRGameInstance::SetError(EMRConnectError Error, const FText& Text)
{
	LastError = Error;
	LastErrorText = Text;
	UE_LOG(LogManchesterRide, Warning, TEXT("Connection error: %s"), *Text.ToString());
}

// ---------------------------------------------------------------------------
// Hosting / joining
// ---------------------------------------------------------------------------

FString UMRGameInstance::BuildLoginOptions(const FString& Password) const
{
	const UMRNetworkSettings* NS = GetDefault<UMRNetworkSettings>();
	FString Name = Profile->PlayerName;
	Name.ReplaceInline(TEXT("?"), TEXT(""));
	Name.ReplaceInline(TEXT("="), TEXT(""));
	Name.ReplaceInline(TEXT(" "), TEXT("_"));
	FString Opts = FString::Printf(TEXT("?Name=%s?ver=%d?veh=%s?paint=%s?jacket=%s?helmet=%s?look=%s"),
		*Name.Left(24), NS->ProtocolVersion, *Profile->VehicleId.ToString(),
		*ColorToHex(Profile->PaintColor), *ColorToHex(Profile->JacketColor), *ColorToHex(Profile->HelmetColor), *Profile->RiderLook.Encode());
	if (!Password.IsEmpty())
	{
		// NOTE: Unreal login options travel unencrypted. The password keeps strangers out of a
		// casual session; it is not account security. Documented in docs/PLAYIT_NETWORKING.md.
		Opts += TEXT("?pw=") + FGenericPlatformHttp::UrlEncode(Password);
	}
	return Opts;
}

void UMRGameInstance::HostSession()
{
	ClearError();
	const UMRNetworkSettings* NS = GetDefault<UMRNetworkSettings>();
	const int32 Port = FMath::Clamp(Profile->HostPort, 1024, 65535);
	const int32 MaxPlayers = FMath::Clamp(Profile->HostMaxPlayers, 2, NS->MaxPlayersCeiling);

	// The listen port is taken from FURL's default port. Set it explicitly so the game binds
	// exactly the port the Playit tunnel forwards to (MaxPortCountToTry=0 prevents fallback).
	FURL::UrlConfig.DefaultPort = Port;

	// Host on whatever map we're on now (the test district on Entry until Milestone 4).
	const FString Map = GetWorld() ? UWorld::RemovePIEPrefix(GetWorld()->GetOutermost()->GetName()) : FString(TEXT("/Engine/Maps/Entry"));
	FString Options = FString::Printf(TEXT("listen?MaxPlayers=%d?Vis=%d?Session=%s"), MaxPlayers,
		static_cast<int32>(Profile->HostVisibility), *FGenericPlatformHttp::UrlEncode(Profile->SessionName));
	if (!WorldOption.IsEmpty()) Options += TEXT("?world=") + WorldOption;
	Options += BuildLoginOptions(FString());
	StatusText = FText::Format(LOCTEXT("Hosting", "Hosting on UDP port {0}. Point a Playit.gg UDP tunnel at 127.0.0.1:{0} and share its address."), FText::AsNumber(Port, &FNumberFormattingOptions::DefaultNoGrouping()));
	UE_LOG(LogManchesterRide, Log, TEXT("Hosting: %s %s"), *Map, *Options);
	UGameplayStatics::OpenLevel(this, FName(*Map), true, Options);
}

bool UMRGameInstance::ParseAddress(const FString& In, FString& OutHost, int32& OutPort)
{
	FString S = In.TrimStartAndEnd();
	S.RemoveFromStart(TEXT("udp://"));
	S.RemoveFromStart(TEXT("unreal://"));
	S.RemoveFromEnd(TEXT("/"));
	if (S.IsEmpty() || S.Contains(TEXT(" ")) || S.Contains(TEXT("?")) || S.Contains(TEXT("/"))) return false;

	OutPort = GetDefault<UMRNetworkSettings>()->DefaultPort;
	FString Host = S, PortStr;
	if (S.StartsWith(TEXT("[")))
	{
		// [IPv6]:port
		int32 Close;
		if (!S.FindChar(TEXT(']'), Close)) return false;
		Host = S.Mid(1, Close - 1);
		PortStr = S.Mid(Close + 1);
		PortStr.RemoveFromStart(TEXT(":"));
	}
	else
	{
		int32 Colon;
		if (S.FindLastChar(TEXT(':'), Colon) && S.Find(TEXT(":")) == Colon)
		{
			Host = S.Left(Colon);
			PortStr = S.Mid(Colon + 1);
		}
	}
	if (!PortStr.IsEmpty())
	{
		if (!PortStr.IsNumeric()) return false;
		OutPort = FCString::Atoi(*PortStr);
	}
	if (Host.IsEmpty() || OutPort < 1 || OutPort > 65535) return false;
	OutHost = Host;
	return true;
}

void UMRGameInstance::JoinByAddress(const FString& Address, const FString& Password)
{
	ClearError();
	FString Host;
	int32 Port = 0;
	if (!ParseAddress(Address, Host, Port))
	{
		SetError(EMRConnectError::BadAddress, FText::Format(LOCTEXT("BadAddress",
			"\"{0}\" is not a valid address. Use host:port, e.g. the address shown on the host's Playit.gg tunnel."), FText::FromString(Address)));
		return;
	}
	LastJoinAddress = Address.TrimStartAndEnd();
	LastJoinPassword = Password;
	bJoining = true;
	StatusText = FText::Format(LOCTEXT("Resolving", "Looking up {0}..."), FText::FromString(Host));

	// Resolve off the game thread so a slow DNS lookup can't freeze the menu.
	TWeakObjectPtr<UMRGameInstance> WeakThis(this);
	Async(EAsyncExecution::ThreadPool, [WeakThis, Host, Port, Password]()
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		FAddressInfoResult Res = Sockets ? Sockets->GetAddressInfo(*Host, nullptr, EAddressInfoFlags::Default, NAME_None, ESocketType::SOCKTYPE_Datagram)
			: FAddressInfoResult(*Host, nullptr);
		FString Resolved;
		if (Res.ReturnCode == SE_NO_ERROR && Res.Results.Num() > 0)
		{
			Resolved = Res.Results[0].Address->ToString(false);
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Host, Port, Password, Resolved]()
		{
			UMRGameInstance* GI = WeakThis.Get();
			if (!GI) return;
			if (Resolved.IsEmpty())
			{
				GI->bJoining = false;
				GI->SetError(EMRConnectError::BadAddress, FText::Format(LOCTEXT("NoResolve",
					"Could not find \"{0}\". Check the address for typos (Playit addresses look like name.gl.at.ply.gg:12345)."), FText::FromString(Host)));
				return;
			}
			const bool bV6 = Resolved.Contains(TEXT(":"));
			GI->BeginTravelToResolved(bV6 ? FString::Printf(TEXT("[%s]:%d"), *Resolved, Port) : FString::Printf(TEXT("%s:%d"), *Resolved, Port), Password);
		});
	});
}

void UMRGameInstance::BeginTravelToResolved(const FString& HostPort, const FString& Password)
{
	APlayerController* PC = GetFirstLocalPlayerController();
	if (!PC)
	{
		bJoining = false;
		SetError(EMRConnectError::Other, LOCTEXT("NoPC", "No local player to travel with."));
		return;
	}
	StatusText = FText::Format(LOCTEXT("Connecting", "Connecting to {0} (UDP)..."), FText::FromString(LastJoinAddress));
	bWasConnected = false;
	const FString URL = HostPort + BuildLoginOptions(Password);
	UE_LOG(LogManchesterRide, Log, TEXT("Joining %s"), *HostPort);
	PC->ClientTravel(URL, TRAVEL_Absolute);
}

void UMRGameInstance::NotifyConnectedToServer()
{
	bJoining = false;
	bWasConnected = true;
	ReconnectAttempts = 0;
	StatusText = FText::Format(LOCTEXT("Connected", "Connected to {0}"), FText::FromString(LastJoinAddress));
	ClearError();
	RememberServer(LastJoinAddress);
}

void UMRGameInstance::RememberServer(const FString& Address)
{
	if (Address.IsEmpty()) return;
	Profile->RecentServers.RemoveAll([&](const FMRServerEntry& E) { return E.Address == Address; });
	FMRServerEntry Entry;
	Entry.Address = Address;
	Entry.Name = Address;
	Entry.LastJoined = FDateTime::Now();
	Profile->RecentServers.Insert(Entry, 0);
	if (Profile->RecentServers.Num() > 10) Profile->RecentServers.SetNum(10);
	SaveProfile();
}

void UMRGameInstance::LeaveSession()
{
	bWasConnected = false;
	bJoining = false;
	ReconnectAttempts = GetDefault<UMRNetworkSettings>()->AutoReconnectAttempts; // no auto-reconnect after a deliberate leave
	StatusText = FText::GetEmpty();
	FURL::UrlConfig.DefaultPort = GetDefault<UMRNetworkSettings>()->DefaultPort;
	UGameplayStatics::OpenLevel(this, FName(TEXT("/Engine/Maps/Entry")), true, (WorldOption.IsEmpty() ? FString() : TEXT("?world=") + WorldOption) + BuildLoginOptions(FString()));
}

void UMRGameInstance::OpenWorld(bool bTestDistrict)
{
	WorldOption = bTestDistrict ? TEXT("test") : FString();
	LeaveSession();
}

void UMRGameInstance::Reconnect()
{
	if (!LastJoinAddress.IsEmpty())
	{
		JoinByAddress(LastJoinAddress, LastJoinPassword);
	}
}

bool UMRGameInstance::IsInMultiplayer() const
{
	const UWorld* W = GetWorld();
	return W && W->GetNetMode() != NM_Standalone;
}

bool UMRGameInstance::IsHosting() const
{
	const UWorld* W = GetWorld();
	return W && (W->GetNetMode() == NM_ListenServer || W->GetNetMode() == NM_DedicatedServer);
}

// ---------------------------------------------------------------------------
// Failure handling -> clear, actionable messages
// ---------------------------------------------------------------------------

void UMRGameInstance::HandleNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString)
{
	const bool bWasInSession = bWasConnected;
	bJoining = false;
	bWasConnected = false;
	UE_LOG(LogManchesterRide, Warning, TEXT("Network failure %s: %s"), ENetworkFailure::ToString(FailureType), *ErrorString);

	bool bTryReconnect = false;
	switch (FailureType)
	{
	case ENetworkFailure::FailureReceived:
		if (ErrorString.Contains(CodeFull))
			SetError(EMRConnectError::ServerFull, LOCTEXT("Full", "The session is full. Ask the host to raise the player limit, or try again later."));
		else if (ErrorString.Contains(CodeVersion))
			SetError(EMRConnectError::VersionMismatch, FText::Format(LOCTEXT("Ver", "Version mismatch: {0}. Everyone needs the same game build."), FText::FromString(ErrorString)));
		else if (ErrorString.Contains(CodePassword))
			SetError(EMRConnectError::BadPassword, LOCTEXT("Pw", "Incorrect password for this private session."));
		else if (ErrorString.Contains(CodeFriends))
			SetError(EMRConnectError::NotOnFriendsList, LOCTEXT("Friends", "This is a friends-only session and your player name is not on the host's friends list."));
		else
			SetError(EMRConnectError::Disconnected, FText::Format(LOCTEXT("Kicked", "Disconnected by the host: {0}"), FText::FromString(ErrorString)));
		break;

	case ENetworkFailure::OutdatedClient:
	case ENetworkFailure::OutdatedServer:
		SetError(EMRConnectError::VersionMismatch, LOCTEXT("Outdated", "Version mismatch between your game and the host's. Everyone needs the same game build."));
		break;

	case ENetworkFailure::ConnectionTimeout:
		if (bWasInSession)
		{
			SetError(EMRConnectError::Timeout, LOCTEXT("TimedOut", "Lost contact with the host (timed out). Reconnecting..."));
			bTryReconnect = true;
		}
		else
		{
			SetError(EMRConnectError::ServerUnavailable, LOCTEXT("Unavailable",
				"Server not responding. Check: the address and port are right, the host is in a session, and the host's Playit.gg agent shows the UDP tunnel as online."));
		}
		break;

	case ENetworkFailure::ConnectionLost:
	case ENetworkFailure::PendingConnectionFailure:
		if (bWasInSession)
		{
			SetError(EMRConnectError::Disconnected, LOCTEXT("Lost", "Disconnected from the host. Reconnecting..."));
			bTryReconnect = true;
		}
		else
		{
			SetError(EMRConnectError::ServerUnavailable, LOCTEXT("ConnFail", "Could not connect to the server. Is the host online and the Playit.gg tunnel running?"));
		}
		break;

	case ENetworkFailure::NetDriverListenFailure:
		SetError(EMRConnectError::PortInUse, FText::Format(LOCTEXT("Port", "Could not open UDP port {0} for hosting. Close anything else using it or pick another port (and update the Playit tunnel)."),
			FText::AsNumber(Profile->HostPort, &FNumberFormattingOptions::DefaultNoGrouping())));
		break;

	default:
		SetError(EMRConnectError::Other, FText::Format(LOCTEXT("Other", "Network error: {0}"), FText::FromString(ErrorString)));
		break;
	}

	if (bTryReconnect && ReconnectAttempts < GetDefault<UMRNetworkSettings>()->AutoReconnectAttempts && !LastJoinAddress.IsEmpty())
	{
		++ReconnectAttempts;
		const float Delay = 2.f * ReconnectAttempts;
		StatusText = FText::Format(LOCTEXT("Retry", "Reconnecting in {0}s (attempt {1})"), FText::AsNumber(Delay), FText::AsNumber(ReconnectAttempts));
		GetTimerManager().SetTimer(ReconnectTimer, this, &UMRGameInstance::Reconnect, Delay, false);
	}
}

void UMRGameInstance::HandleTravelFailure(UWorld* World, ETravelFailure::Type FailureType, const FString& ErrorString)
{
	bJoining = false;
	if (FailureType == ETravelFailure::PackageMissing || FailureType == ETravelFailure::PackageVersion)
	{
		SetError(EMRConnectError::VersionMismatch, FText::Format(LOCTEXT("Package", "The host is on a map or build you don't have ({0}). Update the game."), FText::FromString(ErrorString)));
	}
	else
	{
		SetError(EMRConnectError::Other, FText::Format(LOCTEXT("Travel", "Travel failed: {0}"), FText::FromString(ErrorString)));
	}
}

// ---------------------------------------------------------------------------
// Graphics presets tuned for an RTX 3060 / Ryzen 5 5600G class PC
// ---------------------------------------------------------------------------

FText UMRGameInstance::GraphicsPresetName(int32 Preset)
{
	switch (Preset)
	{
	case 0: return LOCTEXT("Low", "Low");
	case 1: return LOCTEXT("Medium", "Medium");
	case 2: return LOCTEXT("High", "High");
	default: return LOCTEXT("Ultra", "Ultra");
	}
}

void UMRGameInstance::ApplyGraphicsPreset(int32 Preset)
{
	Preset = FMath::Clamp(Preset, 0, 3);
	if (Profile) Profile->GraphicsPreset = Preset;
	UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr;
	if (!S) return;
	// Engine scalability levels 0..3 = Low/Medium/High/Epic. With UE5 defaults, Lumen GI and
	// reflections are only active at High and above, which is what keeps Low/Medium fast.
	S->SetOverallScalabilityLevel(Preset);
	S->SetFrameRateLimit(Preset == 3 ? 0.f : 60.f);
	S->SetVSyncEnabled(false);
	S->ApplySettings(false);

	auto SetCVar = [](const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name)) V->Set(Value, ECVF_SetByGameSetting);
	};
	SetCVar(TEXT("r.VolumetricFog"), Preset >= 2 ? 1 : 0);
	SetCVar(TEXT("r.Shadow.Virtual.Enable"), Preset >= 2 ? 1 : 0);
	SetCVar(TEXT("r.ScreenPercentage"), Preset == 0 ? 75 : 100);
	SetCVar(TEXT("foliage.DensityScale"), Preset == 0 ? 0 : 1);
}

#undef LOCTEXT_NAMESPACE
