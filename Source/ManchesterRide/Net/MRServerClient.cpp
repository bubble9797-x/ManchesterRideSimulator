#include "Net/MRServerClient.h"

#include "ManchesterRide.h"
#include "Core/MRGameInstance.h"
#include "Core/MRRiderLook.h"
#include "Core/MRSaveGame.h"
#include "GeoProjection.h"
#include "Net/MRNetworkSettings.h"
#include "Net/MRVersion.h"
#include "Vehicles/MRRideVehicle.h"
#include "World/MRWorldEnvironment.h"

#include "Async/Async.h"
#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Sound/SoundWaveProcedural.h"
#include "VoiceModule.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <wincrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"

#define LOCTEXT_NAMESPACE "MRServer"

using namespace MRNet;

namespace
{
	constexpr int32 VoiceRate = 16000;

	std::string Utf8(const FString& S) { FTCHARToUTF8 C(*S); return std::string(C.Get(), C.Length()); }
	FString FromUtf8(const std::string& S) { FUTF8ToTCHAR C(S.data(), int32(S.size())); return FString(C.Length(), C.Get()); }
	FText T(const std::string& S) { return FText::FromString(FromUtf8(S)); }

	uint32 PackColor(const FLinearColor& C) { const FColor F = C.ToFColor(true); return uint32(F.R) | (uint32(F.G) << 8) | (uint32(F.B) << 16) | (uint32(F.A) << 24); }
	FLinearColor UnpackColor(uint32 V) { return FLinearColor(FColor(uint8(V), uint8(V >> 8), uint8(V >> 16), uint8(V >> 24))); }

	void WriteState(FWriter& W, const FMRNetRideState& S)
	{
		W.F32(float(S.Location.X)); W.F32(float(S.Location.Y)); W.F32(float(S.Location.Z));
		W.U16(S.Yaw); W.U16(uint16(S.GroundPitch)); W.U16(uint16(S.Pitch)); W.U16(uint16(S.Lean)); W.U16(uint16(S.AirYaw));
		W.U16(uint16(S.Speed)); W.U16(uint16(S.VerticalSpeed)); W.U8(uint8(S.Steer));
		W.U8(S.CompFront); W.U8(S.CompRear); W.U8(S.RpmDiv64); W.U8(S.Flags); W.F32(S.Timestamp);
	}
	FMRNetRideState ReadState(FReader& R)
	{
		FMRNetRideState S;
		const float X = R.F32(), Y = R.F32(), Z = R.F32();
		S.Location = FVector(X, Y, Z);
		S.Yaw = R.U16(); S.GroundPitch = int16(R.U16()); S.Pitch = int16(R.U16()); S.Lean = int16(R.U16()); S.AirYaw = int16(R.U16());
		S.Speed = int16(R.U16()); S.VerticalSpeed = int16(R.U16()); S.Steer = int8(R.U8());
		S.CompFront = R.U8(); S.CompRear = R.U8(); S.RpmDiv64 = R.U8(); S.Flags = R.U8(); S.Timestamp = R.F32();
		return S;
	}

	FString LoginFile() { return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Server"), TEXT("login.bin")); }

	// Windows DPAPI: only this Windows user on this PC can decrypt the saved login.
	bool Protect(const TArray<uint8>& In, TArray<uint8>& Out, bool bEncrypt)
	{
		DATA_BLOB A{ DWORD(In.Num()), const_cast<BYTE*>(In.GetData()) }, B{};
		const BOOL bOk = bEncrypt ? CryptProtectData(&A, L"Manchester Ride login", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &B)
			: CryptUnprotectData(&A, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &B);
		if (!bOk) return false;
		Out.SetNumUninitialized(B.cbData);
		FMemory::Memcpy(Out.GetData(), B.pbData, B.cbData);
		LocalFree(B.pbData);
		return true;
	}
}

UMRServerClient* UMRServerClient::Get(const UObject* WorldContext)
{
	const UWorld* W = WorldContext ? WorldContext->GetWorld() : nullptr;
	const UGameInstance* GI = W ? W->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UMRServerClient>() : nullptr;
}

void UMRServerClient::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Link = MakeUnique<FServerLink>();
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UMRServerClient::Tick));
	FString Addr = GetDefault<UMRNetworkSettings>()->DedicatedServer;
	FParse::Value(FCommandLine::Get(), TEXT("Server="), Addr); // -Server=host:port (testing against a local server)
	if (!UMRGameInstance::ParseAddress(Addr, Host, Port)) { Host = TEXT("209.25.141.16"); Port = 38420; }
	LoadSavedLogin();
}

void UMRServerClient::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	if (Link && State == EMRServerState::Online) Link->Close(true);
	CloseDownload(false);
	StopVoice();
	Link.Reset();
	Super::Deinitialize();
}

FString UMRServerClient::GetAddress() const { return FString::Printf(TEXT("%s:%d"), *Host, Port); }

FString UMRServerClient::GetFingerprint() const { return Link ? FromUtf8(Link->GetServerFingerprint()) : FString(); }

UMRSaveGame* UMRServerClient::Profile() const
{
	const UMRGameInstance* GI = Cast<UMRGameInstance>(GetGameInstance());
	return GI ? GI->GetProfile() : nullptr;
}

AMRRideVehicle* UMRServerClient::LocalVehicle() const
{
	const APlayerController* PC = GetGameInstance() ? GetGameInstance()->GetFirstLocalPlayerController() : nullptr;
	return PC ? Cast<AMRRideVehicle>(PC->GetPawn()) : nullptr;
}

// ---------------------------------------------------------------------------
// Saved login
// ---------------------------------------------------------------------------

void UMRServerClient::LoadSavedLogin()
{
	SavedUser.Reset(); SavedToken.Reset();
	TArray<uint8> Enc, Plain;
	if (!FFileHelper::LoadFileToArray(Enc, *LoginFile(), FILEREAD_Silent) || !Protect(Enc, Plain, false)) return;
	const FString S = FromUtf8(std::string((const char*)Plain.GetData(), Plain.Num()));
	TArray<FString> L;
	S.ParseIntoArray(L, TEXT("\n"), false);
	if (L.Num() >= 4 && L[0] == TEXT("MRL1") && L[1] == GetAddress())
	{
		SavedUser = L[2];
		SavedToken = L[3];
	}
}

void UMRServerClient::SaveLogin(const FString& User, const FString& Token)
{
	SavedUser = User;
	SavedToken = Token;
	const std::string S = Utf8(FString::Printf(TEXT("MRL1\n%s\n%s\n%s"), *GetAddress(), *User, *Token));
	TArray<uint8> Plain((const uint8*)S.data(), int32(S.size())), Enc;
	if (Protect(Plain, Enc, true)) FFileHelper::SaveArrayToFile(Enc, *LoginFile());
}

void UMRServerClient::ForgetSavedLogin()
{
	if (State == EMRServerState::Online && !SavedToken.IsEmpty())
	{
		FWriter W; W.U8(CLogout); W.Str(Utf8(SavedToken));
		Link->SendReliable(W.B);
	}
	SavedUser.Reset();
	SavedToken.Reset();
	IFileManager::Get().Delete(*LoginFile(), false, true, true);
}

// ---------------------------------------------------------------------------
// Connect / disconnect
// ---------------------------------------------------------------------------

void UMRServerClient::Connect(EMRLoginMode Mode, const FString& User, const FString& Password, bool bInRemember)
{
	if (IsBusy() || IsOnline()) return;
	ResetSession();
	ErrorText = FText::GetEmpty();
	bNeedLoginForm = false;
	bKeyMismatch = false;
	PendingMode = Mode;
	PendingUser = Mode == EMRLoginMode::Saved ? SavedUser : User.TrimStartAndEnd();
	PendingSecret = Mode == EMRLoginMode::Saved ? SavedToken : Password;
	bRemember = bInRemember;
	if (Mode == EMRLoginMode::Saved && !HasSavedLogin())
	{
		bNeedLoginForm = true;
		Fail(LOCTEXT("NoSaved", "Log in or create an account first."));
		return;
	}
	State = EMRServerState::Connecting;
	StatusText = FText::Format(LOCTEXT("Connecting", "Connecting to {0}..."), FText::FromString(GetAddress()));
	UE_LOG(LogManchesterRide, Log, TEXT("Server: connecting to %s as %s"), *GetAddress(), *PendingUser);
	if (!Link->Start(Utf8(Host), uint16(Port), FPlatformTime::Seconds()))
	{
		Fail(T(Link->GetError()));
	}
}

void UMRServerClient::Disconnect()
{
	if (State == EMRServerState::Offline) return;
	if (Link) Link->Close(true);
	ResetSession();
	CloseDownload(false);
	State = EMRServerState::Offline;
	StatusText = LOCTEXT("Left", "You left the server.");
	UE_LOG(LogManchesterRide, Log, TEXT("Server: disconnected"));
}

void UMRServerClient::TrustNewServerKey()
{
	bTrustNextKey = true;
	bKeyMismatch = false;
	ErrorText = FText::GetEmpty();
}

void UMRServerClient::Fail(const FText& Why)
{
	UE_LOG(LogManchesterRide, Warning, TEXT("Server: %s"), *Why.ToString());
	ErrorText = Why;
	StatusText = FText::GetEmpty();
	if (Link) Link->Close(false);
	ResetSession();
	CloseDownload(false);
	State = EMRServerState::Failed;
}

void UMRServerClient::ResetSession()
{
	for (TPair<uint16, FMRServerRider>& P : Riders) DestroyRider(P.Value);
	Riders.Reset();
	StopVoice();
	MyId = 0;
	bOp = false;
	PingMs = 0;
	PendingSecret.Reset();
}

void UMRServerClient::OnLinkUp()
{
	// Remember the server's identity the first time (like SSH); warn if it ever changes.
	UMRSaveGame* P = Profile();
	const std::vector<uint8_t>& Key = Link->GetServerKey();
	const FString Hex = BytesToHex(Key.data(), int32(Key.size()));
	if (P)
	{
		const FString* Known = P->ServerKeys.Find(GetAddress());
		if (Known && *Known != Hex && !bTrustNextKey)
		{
			bKeyMismatch = true;
			Fail(FText::Format(LOCTEXT("KeyChanged", "This server's identity key has changed (now {0}). That's expected if the owner moved or reinstalled the server. If not, someone could be pretending to be it. Ask the owner, then choose Trust the new key."),
				FText::FromString(GetFingerprint())));
			bKeyMismatch = true;
			return;
		}
		if (!Known || *Known != Hex)
		{
			P->ServerKeys.Add(GetAddress(), Hex);
			if (UMRGameInstance* GI = Cast<UMRGameInstance>(GetGameInstance())) GI->SaveProfile();
		}
	}
	bTrustNextKey = false;
	State = EMRServerState::LoggingIn;
	StatusText = PendingMode == EMRLoginMode::Register ? LOCTEXT("Creating", "Creating your account...") : LOCTEXT("LoggingIn", "Logging in...");
	FWriter W;
	W.U8(CLogin);
	W.U8(uint8(PendingMode));
	W.Str(Utf8(PendingUser));
	W.Str(Utf8(PendingSecret));
	W.U32(MR_GAME_BUILD);
	W.Str(MR_GAME_VERSION);
	Link->SendReliable(W.B);
	PendingSecret.Reset(); // the password never stays in memory longer than it has to
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

bool UMRServerClient::Tick(float DeltaTime)
{
	if (!Link || State == EMRServerState::Offline || State == EMRServerState::Failed) return true;
	const double Now = FPlatformTime::Seconds();
	std::vector<std::vector<uint8_t>> Msgs;
	Link->Poll(Now, Msgs);
	if (State == EMRServerState::Connecting && Link->GetState() == FServerLink::EState::Connected) OnLinkUp();
	for (const std::vector<uint8_t>& M : Msgs)
	{
		if (State == EMRServerState::Failed || State == EMRServerState::Offline) return true;
		if (!M.empty()) HandleMessage(M);
	}
	if (Link->GetState() == FServerLink::EState::Failed && State != EMRServerState::Failed)
	{
		Fail(T(Link->GetError()));
		return true;
	}
	if (State == EMRServerState::Online)
	{
		if (const AMRRideVehicle* V = LocalVehicle(); V && Now - LastStateSent >= 1.0 / 30.0)
		{
			LastStateSent = Now;
			FWriter W;
			W.U8(CState);
			WriteState(W, V->GetNetState());
			Link->SendUnreliable(W.B);
		}
		if (Now - LastPing > 1.0)
		{
			LastPing = Now;
			FWriter W; W.U8(CPing); W.U32(uint32(Now * 1000.0));
			Link->SendUnreliable(W.B);
		}
		UpdateRiders();
		TickVoice(Now);
	}
	else if (State == EMRServerState::Updating)
	{
		TickDownload(Now);
	}
	if (Link) Link->Flush(Now);
	return true;
}

void UMRServerClient::AddChat(uint8 Kind, const FString& From, const FString& Text)
{
	FMRChatLine L;
	L.Kind = Kind; L.From = From; L.Text = Text; L.Time = FPlatformTime::Seconds();
	Chat.Add(L);
	UE_LOG(LogManchesterRide, Log, TEXT("Chat: %s%s"), From.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("<%s> "), *From), *Text);
	if (Chat.Num() > 200) Chat.RemoveAt(0, Chat.Num() - 200);
}

void UMRServerClient::SendChat(const FString& Text)
{
	const FString S = Text.TrimStartAndEnd().Left(256);
	if (S.IsEmpty() || !IsOnline()) return;
	FWriter W; W.U8(CChat); W.Str(Utf8(S));
	Link->SendReliable(W.B);
}

void UMRServerClient::SendLook()
{
	const UMRSaveGame* P = Profile();
	if (!P || !IsOnline()) return;
	FWriter W;
	W.U8(CLook);
	W.Str(Utf8(P->VehicleId.ToString()));
	W.U32(PackColor(P->PaintColor)); W.U32(PackColor(P->JacketColor)); W.U32(PackColor(P->HelmetColor));
	W.Str(Utf8(P->RiderLook.Encode()));
	Link->SendReliable(W.B);
}

void UMRServerClient::HandleMessage(const std::vector<uint8_t>& M)
{
	FReader R(M.data(), M.size(), 1);
	switch (M[0])
	{
	case SLoginResult:
	{
		const uint8 Code = R.U8();
		const FText Msg = T(R.Str());
		if (Code == LoginOK)
		{
			MyId = R.U16();
			MyName = FromUtf8(R.Str());
			const FString Token = FromUtf8(R.Str());
			bOp = R.U8() != 0;
			Motd = FromUtf8(R.Str());
			ServerName = FromUtf8(R.Str());
			MaxPlayers = R.U16();
			if (!Token.IsEmpty() && bRemember) SaveLogin(MyName, Token);
			else if (!bRemember && PendingMode != EMRLoginMode::Saved) { SavedUser.Reset(); SavedToken.Reset(); IFileManager::Get().Delete(*LoginFile(), false, true, true); }
			State = EMRServerState::Online;
			bJustJoined = true;
			StatusText = FText::Format(LOCTEXT("Online", "Online on {0} as {1}"), FText::FromString(ServerName), FText::FromString(MyName));
			UE_LOG(LogManchesterRide, Log, TEXT("Server: logged in as %s (id %d%s)"), *MyName, MyId, bOp ? TEXT(", operator") : TEXT(""));
			AddChat(ChatSystem, FString(), FString::Printf(TEXT("Joined %s. Press T to chat, hold V to talk, Tab for the rider list."), *ServerName));
			if (!Motd.IsEmpty()) AddChat(ChatServer, ServerName, Motd);
			SendLook();
			LastStateSent = LastPing = 0.0;
		}
		else if (Code == LoginNeedUpdate)
		{
			const FString Ver = FromUtf8(R.Str());
			const uint32 Build = R.U32();
			const uint64 Size = R.U64();
			const uint8_t* Sha = R.Raw(32);
			const uint32 Chunk = R.U32();
			if (R.bBad || !Sha || Size == 0 || Chunk == 0) { Fail(LOCTEXT("BadUpdate", "The server offered a broken update.")); return; }
			StartDownload(Ver, Build, Size, Sha, Chunk);
		}
		else
		{
			if (Code == LoginBadToken) { SavedUser.Reset(); SavedToken.Reset(); IFileManager::Get().Delete(*LoginFile(), false, true, true); bNeedLoginForm = true; }
			if (Code == LoginBadPassword || Code == LoginNoAccount || Code == LoginTaken || Code == LoginBadName) bNeedLoginForm = true;
			Fail(Msg);
		}
		break;
	}
	case SPlayer:
	{
		const uint16 Id = R.U16();
		const FString Name = FromUtf8(R.Str());
		const bool bIsOp = R.U8() != 0;
		const FString Veh = FromUtf8(R.Str());
		const uint32 Paint = R.U32(), Jacket = R.U32(), Helmet = R.U32();
		const FString Look = FromUtf8(R.Str());
		if (R.bBad) return;
		if (Id == MyId) { bOp = bIsOp; return; }
		FMRServerRider& Rd = Riders.FindOrAdd(Id);
		Rd.Id = Id; Rd.Name = Name; Rd.bOp = bIsOp;
		if (!Veh.IsEmpty()) { Rd.VehicleId = FName(*Veh); Rd.Paint = UnpackColor(Paint); Rd.Jacket = UnpackColor(Jacket); Rd.Helmet = UnpackColor(Helmet); Rd.LookCode = Look; }
		break;
	}
	case SPlayerLeave:
		if (FMRServerRider* Rd = Riders.Find(R.U16()))
		{
			DestroyRider(*Rd);
			Riders.Remove(Rd->Id);
		}
		break;
	case SStates:
	{
		const int32 N = R.U8();
		for (int32 i = 0; i < N; ++i)
		{
			const uint16 Id = R.U16();
			const FMRNetRideState S = ReadState(R);
			if (R.bBad) break;
			if (FMRServerRider* Rd = Riders.Find(Id))
			{
				Rd->LastState = S;
				Rd->bHasState = true;
				if (AMRRideVehicle* V = Rd->Actor.Get()) V->ReceiveExternalState(S);
			}
		}
		break;
	}
	case SChat:
	{
		const uint8 Kind = R.U8();
		const FString From = FromUtf8(R.Str());
		const FString Text = FromUtf8(R.Str());
		if (!R.bBad) AddChat(Kind, From, Text);
		break;
	}
	case SKick:
		Fail(FText::Format(LOCTEXT("Kicked", "Disconnected by the server: {0}"), T(R.Str())));
		break;
	case SVoice:
	{
		const uint16 From = R.U16();
		R.U16(); // sequence (not needed: late packets are simply played late)
		const size_t Len = R.Left();
		const uint8_t* Data = R.Raw(Len);
		if (FMRServerRider* Rd = Riders.Find(From); Rd && Data && Len > 0) PlayVoice(*Rd, Data, int32(Len));
		break;
	}
	case SPong:
		PingMs = FMath::Clamp(int32(uint32(FPlatformTime::Seconds() * 1000.0) - R.U32()), 0, 9999);
		break;
	case SPings:
	{
		const int32 N = R.U8();
		for (int32 i = 0; i < N; ++i)
		{
			const uint16 Id = R.U16(), Ms = R.U16();
			if (FMRServerRider* Rd = Riders.Find(Id)) Rd->PingMs = Ms;
		}
		break;
	}
	case STeleport:
	{
		const uint8 Mode = R.U8();
		const double A = R.F64(), B = R.F64();
		R.F64();
		const float Yaw = R.F32();
		AMRRideVehicle* V = LocalVehicle();
		if (!V || R.bBad) break;
		FVector2D Where(A, B);
		if (Mode == 1)
		{
			double X, Y;
			MRGeo::LocalToUnreal(MRGeo::ToLocal(MRGeo::FLatLon{ A, B }), X, Y);
			Where = FVector2D(X, Y);
		}
		V->TeleportRider(Where, Yaw >= 0.f ? Yaw : V->GetActorRotation().Yaw);
		break;
	}
	case SEnv:
	{
		const int8 Weather = int8(R.U8());
		const float Hours = R.F32();
		if (AMRWorldEnvironment* Env = AMRWorldEnvironment::Get(GetGameInstance()->GetWorld()))
		{
			if (Weather >= 0 && Weather <= 5) Env->SetWeather(static_cast<EMRWeather>(Weather), true);
			if (Hours >= 0.f) Env->SetTimeOfDay(Hours);
		}
		break;
	}
	case SUpdate:
		// The server already says so in chat; nothing else to do until the next join.
		break;
	case SFileChunk:
	{
		const uint32 Idx = R.U32();
		const size_t Len = R.Left();
		if (const uint8_t* Data = R.Raw(Len)) OnChunk(Idx, Data, int32(Len));
		break;
	}
	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// Other riders
// ---------------------------------------------------------------------------

void UMRServerClient::UpdateRiders()
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World) return;
	for (TPair<uint16, FMRServerRider>& P : Riders)
	{
		FMRServerRider& Rd = P.Value;
		if (!Rd.bHasState || Rd.VehicleId.IsNone()) continue;
		const FString Config = FString::Printf(TEXT("%s|%08x|%08x|%08x|%s"), *Rd.VehicleId.ToString(), PackColor(Rd.Paint), PackColor(Rd.Jacket), PackColor(Rd.Helmet), *Rd.LookCode);
		AMRRideVehicle* V = Rd.Actor.Get();
		if (!V)
		{
			const FTransform Xf(FRotator(0.f, Rd.LastState.GetYawDegrees(), 0.f), FVector(Rd.LastState.Location));
			FActorSpawnParameters SP;
			SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SP.bDeferConstruction = true;
			SP.ObjectFlags |= RF_Transient;
			V = World->SpawnActor<AMRRideVehicle>(AMRRideVehicle::StaticClass(), Xf, SP);
			if (!V) continue;
			V->InitVehicle(Rd.VehicleId, Rd.Paint, Rd.Jacket, Rd.Helmet);
			V->SetRiderLook(FMRRiderLook::Decode(Rd.LookCode));
			V->SetDisplayName(Rd.Name);
			V->FinishSpawning(Xf);
			V->ReceiveExternalState(Rd.LastState);
			Rd.Actor = V;
			Rd.AppliedConfig = Config;
			VoiceComps.Remove(Rd.Id); // re-attach voice to the new rider
			UE_LOG(LogManchesterRide, Log, TEXT("Server: rider %s appeared on a %s at %s"), *Rd.Name, *Rd.VehicleId.ToString(), *V->GetActorLocation().ToCompactString());
		}
		else if (Rd.AppliedConfig != Config)
		{
			V->InitVehicle(Rd.VehicleId, Rd.Paint, Rd.Jacket, Rd.Helmet);
			V->SetRiderLook(FMRRiderLook::Decode(Rd.LookCode));
			Rd.AppliedConfig = Config;
		}
		V->SetDisplayName(Rd.Name);
	}
	// Your own look changed in the garage / character editor: tell everyone.
	static FString LastLook;
	if (const UMRSaveGame* P = Profile())
	{
		const FString Mine = FString::Printf(TEXT("%s|%08x|%08x|%08x|%s"), *P->VehicleId.ToString(), PackColor(P->PaintColor), PackColor(P->JacketColor), PackColor(P->HelmetColor), *P->RiderLook.Encode());
		if (Mine != LastLook) { LastLook = Mine; SendLook(); }
	}
}

void UMRServerClient::DestroyRider(FMRServerRider& Rd)
{
	if (AMRRideVehicle* V = Rd.Actor.Get()) V->Destroy();
	Rd.Actor.Reset();
	if (TObjectPtr<UAudioComponent>* C = VoiceComps.Find(Rd.Id); C && *C) (*C)->Stop();
	VoiceComps.Remove(Rd.Id);
	VoiceWaves.Remove(Rd.Id);
	Rd.Decoder.Reset();
}

// ---------------------------------------------------------------------------
// Voice chat: 16 kHz mono, Opus (the engine's voice codec), relayed by the server
// ---------------------------------------------------------------------------

void UMRServerClient::ApplyVoiceSettings()
{
	const UMRSaveGame* P = Profile();
	if (!P) return;
	if (P->VoiceMode == 0 && VoiceCapture.IsValid()) { VoiceCapture->Shutdown(); VoiceCapture.Reset(); VoiceEncoder.Reset(); bTransmitting = false; }
	for (TPair<int32, TObjectPtr<UAudioComponent>>& C : VoiceComps) if (C.Value) C.Value->Stop();
	VoiceComps.Reset(); // recreated with the new range / volume on the next packet
}

void UMRServerClient::StopVoice()
{
	if (VoiceCapture.IsValid()) { VoiceCapture->Shutdown(); VoiceCapture.Reset(); }
	VoiceEncoder.Reset();
	MicPcm.Reset();
	for (TPair<int32, TObjectPtr<UAudioComponent>>& C : VoiceComps) if (C.Value) C.Value->Stop();
	VoiceComps.Reset();
	VoiceWaves.Reset();
	bTransmitting = false;
}

void UMRServerClient::TickVoice(double Now)
{
	const UMRSaveGame* P = Profile();
	if (!P || P->VoiceMode == 0 || !FVoiceModule::IsAvailable()) { bTransmitting = false; return; }
	if (!VoiceCapture.IsValid())
	{
		static double LastTry = -100.0;
		if (Now - LastTry < 5.0) return;
		LastTry = Now;
		VoiceCapture = FVoiceModule::Get().CreateVoiceCapture(FString(), VoiceRate, 1);
		VoiceEncoder = FVoiceModule::Get().CreateVoiceEncoder(VoiceRate, 1, EAudioEncodeHint::VoiceEncode_Voice);
		if (!VoiceCapture.IsValid() || !VoiceEncoder.IsValid())
		{
			VoiceCapture.Reset(); VoiceEncoder.Reset();
			static bool bWarned = false;
			if (!bWarned) { bWarned = true; AddChat(ChatError, FString(), TEXT("No microphone found (or Windows is blocking it: Settings > Privacy > Microphone). Voice chat: listening only.")); }
			return;
		}
	}
	const bool bWant = P->VoiceMode == 2 || bPushToTalk;
	if (bWant && !VoiceCapture->IsCapturing()) VoiceCapture->Start();
	else if (!bWant && VoiceCapture->IsCapturing()) { VoiceCapture->Stop(); MicPcm.Reset(); }
	if (!bWant) { bTransmitting = false; return; }

	uint32 Avail = 0;
	if (VoiceCapture->GetCaptureState(Avail) == EVoiceCaptureState::Ok && Avail > 0)
	{
		const int32 Old = MicPcm.Num();
		MicPcm.SetNumUninitialized(Old + int32(Avail));
		uint32 Got = 0;
		VoiceCapture->GetVoiceData(MicPcm.GetData() + Old, Avail, Got);
		MicPcm.SetNum(Old + int32(Got));
		// Mic volume, and a simple loudness gate for open mic so silence isn't sent.
		int16* S16 = reinterpret_cast<int16*>(MicPcm.GetData() + Old);
		double Sum = 0.0;
		for (uint32 i = 0; i < Got / 2; ++i)
		{
			const int32 V = FMath::Clamp(FMath::RoundToInt(S16[i] * P->MicVolume), -32768, 32767);
			S16[i] = int16(V);
			Sum += double(V) * V;
		}
		const double Rms = Got >= 2 ? FMath::Sqrt(Sum / (Got / 2)) : 0.0;
		if (Rms > 500.0 || P->VoiceMode == 1) LastLoudTime = Now;
	}
	const int32 FrameBytes = VoiceRate / 50 * 2; // 20 ms
	if (MicPcm.Num() < FrameBytes) { bTransmitting = Now - LastLoudTime < 0.4; return; }
	uint8 Out[1024];
	uint32 OutSize = sizeof(Out);
	const int32 Remainder = VoiceEncoder->Encode(MicPcm.GetData(), MicPcm.Num(), Out, OutSize);
	MicPcm.RemoveAt(0, MicPcm.Num() - FMath::Clamp(Remainder, 0, MicPcm.Num()), EAllowShrinking::No);
	bTransmitting = Now - LastLoudTime < 0.4;
	if (OutSize > 0 && OutSize <= 1000 && bTransmitting)
	{
		FWriter W; W.U8(CVoice); W.U16(VoiceSeq++); W.Bytes(Out, OutSize);
		Link->SendNow(W.B);
	}
}

void UMRServerClient::PlayVoice(FMRServerRider& Rd, const uint8_t* Data, int32 Len)
{
	const UMRSaveGame* P = Profile();
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!P || !World || !FVoiceModule::IsAvailable() || P->VoiceVolume <= 0.f) return;
	Rd.LastVoiceTime = FPlatformTime::Seconds();
	if (!Rd.Decoder.IsValid()) Rd.Decoder = FVoiceModule::Get().CreateVoiceDecoder(VoiceRate, 1);
	if (!Rd.Decoder.IsValid()) return;

	TObjectPtr<USoundWaveProcedural>& Wave = VoiceWaves.FindOrAdd(Rd.Id);
	if (!Wave)
	{
		Wave = NewObject<USoundWaveProcedural>(this);
		Wave->SetSampleRate(VoiceRate);
		Wave->NumChannels = 1;
		Wave->Duration = INDEFINITELY_LOOPING_DURATION;
		Wave->SoundGroup = SOUNDGROUP_Voice;
		Wave->bLooping = false;
	}
	TObjectPtr<UAudioComponent>& Comp = VoiceComps.FindOrAdd(Rd.Id);
	if (!Comp || !IsValid(Comp) || Comp->GetWorld() != World)
	{
		AMRRideVehicle* V = Rd.Actor.Get();
		if (P->VoiceRange == 1 && V)
		{
			Comp = UGameplayStatics::SpawnSoundAttached(Wave, V->GetRootComponent(), NAME_None, FVector(0, 0, 160.f), EAttachLocation::KeepRelativeOffset, false, P->VoiceVolume, 1.f, 0.f, nullptr, nullptr, false);
			if (Comp)
			{
				Comp->bOverrideAttenuation = true;
				Comp->AttenuationOverrides.bAttenuate = true;
				Comp->AttenuationOverrides.bSpatialize = true;
				Comp->AttenuationOverrides.AttenuationShapeExtents = FVector(1500.f, 0, 0); // full volume within 15 m
				Comp->AttenuationOverrides.FalloffDistance = 13500.f;                     // silent beyond 150 m
			}
		}
		else
		{
			Comp = UGameplayStatics::SpawnSound2D(World, Wave, P->VoiceVolume, 1.f, 0.f, nullptr, false, false);
		}
		if (!Comp) return;
	}
	uint8 Pcm[48 * 1024];
	uint32 PcmSize = sizeof(Pcm);
	Rd.Decoder->Decode(Data, uint32(Len), Pcm, PcmSize);
	if (PcmSize > 0)
	{
		if (Wave->GetAvailableAudioByteCount() > VoiceRate * 2) Wave->ResetAudio(); // >1 s behind: catch up
		Wave->QueueAudio(Pcm, int32(PcmSize));
	}
	if (!Comp->IsPlaying()) Comp->Play();
}

// ---------------------------------------------------------------------------
// Game update, downloaded from the server over the same link
// ---------------------------------------------------------------------------

void UMRServerClient::StartDownload(const FString& Version, uint32 Build, uint64 Size, const uint8_t* Sha, uint32 ChunkSize)
{
	CloseDownload(false);
	UpdVersion = Version;
	UpdBuild = Build;
	UpdSize = int64(Size);
	UpdChunk = ChunkSize;
	UpdChunks = uint32((Size + ChunkSize - 1) / ChunkSize);
	FMemory::Memcpy(UpdSha, Sha, 32);
	UpdPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Update"), FString::Printf(TEXT("ManchesterRide-Setup-%u.exe"), Build)));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(UpdPath), true);
	UpdFile.Reset(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*UpdPath, false, false));
	if (!UpdFile)
	{
		Fail(FText::Format(LOCTEXT("NoWrite", "Couldn't save the update to {0}."), FText::FromString(UpdPath)));
		return;
	}
	UpdGot.Init(false, int32(UpdChunks));
	UpdOutstanding.Reset();
	UpdHave = UpdNextReq = 0;
	UpdReceivedBytes = UpdRateBytes = 0;
	UpdWindow = 64.0;
	UpdStart = UpdRateT = FPlatformTime::Seconds();
	UpdRate = 0.0;
	bUpdVerifying = false;
	State = EMRServerState::Updating;
	StatusText = FText::Format(LOCTEXT("Updating", "Downloading game version {0} from the server..."), FText::FromString(Version));
	UE_LOG(LogManchesterRide, Log, TEXT("Server: downloading update %s (build %u, %lld bytes) to %s"), *Version, Build, UpdSize, *UpdPath);
}

void UMRServerClient::TickDownload(double Now)
{
	if (!UpdFile || bUpdVerifying) return;
	const double Timeout = FMath::Max(0.4, Link->GetRttMs() * 4.0 / 1000.0);
	std::vector<uint32_t> Req;
	for (TPair<uint32, double>& O : UpdOutstanding)
	{
		if (Now - O.Value > Timeout) { Req.push_back(O.Key); O.Value = Now; UpdWindow = FMath::Max(16.0, UpdWindow * 0.85); }
	}
	while (UpdOutstanding.Num() < int32(UpdWindow) && UpdNextReq < UpdChunks)
	{
		if (!UpdGot[int32(UpdNextReq)]) { UpdOutstanding.Add(UpdNextReq, Now); Req.push_back(UpdNextReq); }
		++UpdNextReq;
	}
	for (size_t i = 0; i < Req.size(); i += 200)
	{
		const size_t N = FMath::Min<size_t>(200, Req.size() - i);
		FWriter W; W.U8(CFileReq); W.U16(uint16(N));
		for (size_t j = 0; j < N; ++j) W.U32(Req[i + j]);
		Link->SendNow(W.B);
	}
	if (Now - UpdRateT > 1.0)
	{
		UpdRate = UpdRateBytes / (Now - UpdRateT);
		UpdRateBytes = 0;
		UpdRateT = Now;
	}
}

void UMRServerClient::OnChunk(uint32 Index, const uint8_t* Data, int32 Len)
{
	if (State != EMRServerState::Updating || !UpdFile || Index >= UpdChunks || UpdGot[int32(Index)]) return;
	const int64 Off = int64(Index) * UpdChunk;
	const int64 Expect = FMath::Min<int64>(UpdChunk, UpdSize - Off);
	if (Len != Expect) return;
	if (!UpdFile->Seek(Off) || !UpdFile->Write(Data, Len)) { Fail(LOCTEXT("DiskFull", "Couldn't write the update (is the disk full?).")); return; }
	UpdGot[int32(Index)] = true;
	UpdOutstanding.Remove(Index);
	++UpdHave;
	UpdReceivedBytes += Len;
	UpdRateBytes += Len;
	UpdWindow = FMath::Min(1024.0, UpdWindow + 0.125);
	if (UpdHave == UpdChunks) FinishDownload();
}

void UMRServerClient::FinishDownload()
{
	UpdFile.Reset(); // closes it
	bUpdVerifying = true;
	StatusText = LOCTEXT("Verifying", "Checking the download...");
	if (Link) Link->Close(true); // done with the server; it gets the new version next time
	const FString Path = UpdPath;
	uint8 Want[32];
	FMemory::Memcpy(Want, UpdSha, 32);
	TWeakObjectPtr<UMRServerClient> WeakThis(this);
	Async(EAsyncExecution::Thread, [WeakThis, Path, Want]()
	{
		bool bOk = false;
		if (TUniquePtr<FArchive> Ar = TUniquePtr<FArchive>(IFileManager::Get().CreateFileReader(*Path)))
		{
			FSha256 H;
			TArray<uint8> Buf;
			Buf.SetNumUninitialized(4 << 20);
			int64 Left = Ar->TotalSize();
			while (Left > 0)
			{
				const int64 N = FMath::Min<int64>(Left, Buf.Num());
				Ar->Serialize(Buf.GetData(), N);
				H.Update(Buf.GetData(), size_t(N));
				Left -= N;
			}
			uint8 Got[32];
			H.End(Got);
			bOk = FMemory::Memcmp(Got, Want, 32) == 0;
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Path, bOk]()
		{
			UMRServerClient* Self = WeakThis.Get();
			if (!Self) return;
			Self->bUpdVerifying = false;
			if (!bOk)
			{
				IFileManager::Get().Delete(*Path, false, true, true);
				Self->Fail(LOCTEXT("BadDownload", "The update didn't download correctly (it failed its check). Try joining again."));
				return;
			}
			UE_LOG(LogManchesterRide, Log, TEXT("Server: update verified, starting %s /update"), *Path);
			if (GIsEditor)
			{
				Self->State = EMRServerState::Offline;
				Self->StatusText = FText::Format(LOCTEXT("EditorUpdate", "Update downloaded and checked ({0}). The editor doesn't install it."), FText::FromString(Path));
				return;
			}
			// The installer waits for the game to close, updates it in place and starts it again (rejoining the server).
			FProcHandle Proc = FPlatformProcess::CreateProc(*Path, TEXT("/update"), true, false, false, nullptr, 0, nullptr, nullptr);
			if (!Proc.IsValid())
			{
				Self->Fail(FText::Format(LOCTEXT("NoRun", "Couldn't start the installer. Run it yourself: {0}"), FText::FromString(Path)));
				return;
			}
			Self->State = EMRServerState::Offline;
			Self->StatusText = LOCTEXT("Restarting", "Installing the update. The game will restart...");
			FPlatformMisc::RequestExit(false);
		});
	});
}

void UMRServerClient::CloseDownload(bool bDelete)
{
	if (UpdFile)
	{
		UpdFile.Reset();
		if (bDelete || !bUpdVerifying) IFileManager::Get().Delete(*UpdPath, false, true, true);
	}
	UpdOutstanding.Reset();
}

#undef LOCTEXT_NAMESPACE
