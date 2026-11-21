#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Ticker.h"
#include "Net/MRNetTypes.h"
#include "Net/MRServerLink.h"
#include "MRServerClient.generated.h"

class AMRRideVehicle;
class UAudioComponent;
class USoundWaveProcedural;
class IVoiceCapture;
class IVoiceEncoder;
class IVoiceDecoder;
class IFileHandle;
class UMRSaveGame;

UENUM()
enum class EMRServerState : uint8
{
	Offline,
	Connecting, // handshake with the server
	LoggingIn,  // account check
	Online,
	Updating,   // downloading a newer game from the server
	Failed,     // see GetErrorText()
};

enum class EMRLoginMode : uint8 { Password = 0, Register = 1, Saved = 2 };

struct FMRChatLine
{
	FString From;
	FString Text;
	uint8 Kind = 0; // MRNet::Chat*
	double Time = 0.0;
};

struct FMRServerRider
{
	uint16 Id = 0;
	FString Name;
	bool bOp = false;
	FName VehicleId;
	FLinearColor Paint = FLinearColor::Red, Jacket = FLinearColor::Blue, Helmet = FLinearColor::White;
	FString LookCode;
	FString AppliedConfig;               // what the puppet was last built with
	TWeakObjectPtr<AMRRideVehicle> Actor;
	FMRNetRideState LastState;
	bool bHasState = false;
	int32 PingMs = 0;
	double LastVoiceTime = -100.0;
	TSharedPtr<IVoiceDecoder> Decoder;
};

/**
 * The game's connection to the Manchester Ride dedicated server (Server/, Go): account login (remembered,
 * encrypted with Windows DPAPI), other riders as puppets, text chat, voice chat and game updates.
 * Runs alongside solo free roam: the world stays local, other riders are drawn from the server's states.
 */
UCLASS()
class MANCHESTERRIDE_API UMRServerClient : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	static UMRServerClient* Get(const UObject* WorldContext);

	void Connect(EMRLoginMode Mode, const FString& User, const FString& Password, bool bRemember = true);
	void Disconnect();
	bool HasSavedLogin() const { return !SavedUser.IsEmpty() && !SavedToken.IsEmpty(); }
	const FString& GetSavedUser() const { return SavedUser; }
	/** Log out on this PC: forgets the saved login (and tells the server to forget it too). */
	void ForgetSavedLogin();
	/** After a "server key changed" warning: accept the new key and join anyway. */
	void TrustNewServerKey();
	bool IsKeyMismatch() const { return bKeyMismatch; }
	bool NeedsLoginForm() const { return bNeedLoginForm; }
	FString GetAddress() const;

	void SendChat(const FString& Text);
	void SetPushToTalk(bool bDown) { bPushToTalk = bDown; }
	/** Call after changing voice settings in the profile. */
	void ApplyVoiceSettings();

	EMRServerState GetState() const { return State; }
	bool IsOnline() const { return State == EMRServerState::Online; }
	bool IsBusy() const { return State == EMRServerState::Connecting || State == EMRServerState::LoggingIn || State == EMRServerState::Updating; }
	FText GetStatusText() const { return StatusText; }
	FText GetErrorText() const { return ErrorText; }
	const TArray<FMRChatLine>& GetChat() const { return Chat; }
	const TMap<uint16, FMRServerRider>& GetRiders() const { return Riders; }
	const FString& GetMyName() const { return MyName; }
	bool IsOp() const { return bOp; }
	const FString& GetServerName() const { return ServerName; }
	const FString& GetMotd() const { return Motd; }
	int32 GetMaxPlayers() const { return MaxPlayers; }
	int32 GetPingMs() const { return PingMs; }
	bool IsTransmitting() const { return bTransmitting; }
	bool IsRiderTalking(const FMRServerRider& R) const { return FPlatformTime::Seconds() - R.LastVoiceTime < 0.35; }
	FString GetFingerprint() const;
	/** True once, right after a successful login (the menu closes itself). */
	bool ConsumeJustJoined() { const bool b = bJustJoined; bJustJoined = false; return b; }

	// Update download
	int64 GetUpdateReceived() const { return UpdReceivedBytes; }
	int64 GetUpdateSize() const { return UpdSize; }
	const FString& GetUpdateVersion() const { return UpdVersion; }
	double GetUpdateRate() const { return UpdRate; }
	bool IsVerifyingUpdate() const { return bUpdVerifying; }

private:
	bool Tick(float DeltaTime);
	void OnLinkUp();
	void HandleMessage(const std::vector<uint8_t>& M);
	void Fail(const FText& Why);
	void ResetSession();
	void AddChat(uint8 Kind, const FString& From, const FString& Text);
	UMRSaveGame* Profile() const;
	AMRRideVehicle* LocalVehicle() const;
	void SendLook();
	void UpdateRiders();
	void DestroyRider(FMRServerRider& R);
	void TickVoice(double Now);
	void PlayVoice(FMRServerRider& R, const uint8_t* Data, int32 Len);
	void StopVoice();
	void StartDownload(const FString& Version, uint32 Build, uint64 Size, const uint8_t* Sha, uint32 ChunkSize);
	void TickDownload(double Now);
	void OnChunk(uint32 Index, const uint8_t* Data, int32 Len);
	void FinishDownload();
	void CloseDownload(bool bDelete);
	void LoadSavedLogin();
	void SaveLogin(const FString& User, const FString& Token);

	FTSTicker::FDelegateHandle TickHandle;
	TUniquePtr<MRNet::FServerLink> Link;
	EMRServerState State = EMRServerState::Offline;
	FText StatusText, ErrorText;
	FString Host;
	int32 Port = 0;

	// login
	EMRLoginMode PendingMode = EMRLoginMode::Saved;
	FString PendingUser, PendingSecret;
	bool bRemember = true;
	FString SavedUser, SavedToken, SavedAddress;
	bool bKeyMismatch = false, bTrustNextKey = false, bNeedLoginForm = false, bJustJoined = false;
	FString PendingKeyHex;

	// session
	uint16 MyId = 0;
	FString MyName, ServerName, Motd;
	bool bOp = false;
	int32 MaxPlayers = 0;
	int32 PingMs = 0;
	double LastStateSent = 0.0, LastPing = 0.0;
	TMap<uint16, FMRServerRider> Riders;
	TArray<FMRChatLine> Chat;

	// voice
	TSharedPtr<IVoiceCapture> VoiceCapture;
	TSharedPtr<IVoiceEncoder> VoiceEncoder;
	TArray<uint8> MicPcm;
	uint16 VoiceSeq = 0;
	bool bPushToTalk = false, bTransmitting = false;
	double LastLoudTime = -100.0;
	UPROPERTY(Transient) TMap<int32, TObjectPtr<UAudioComponent>> VoiceComps;
	UPROPERTY(Transient) TMap<int32, TObjectPtr<USoundWaveProcedural>> VoiceWaves;

	// update
	FString UpdVersion, UpdPath;
	uint32 UpdBuild = 0, UpdChunk = 1024, UpdChunks = 0, UpdHave = 0, UpdNextReq = 0;
	int64 UpdSize = 0, UpdReceivedBytes = 0;
	uint8 UpdSha[32] = {};
	TBitArray<> UpdGot;
	TMap<uint32, double> UpdOutstanding;
	double UpdWindow = 64.0, UpdStart = 0.0, UpdRate = 0.0, UpdRateT = 0.0;
	int64 UpdRateBytes = 0;
	bool bUpdVerifying = false;
	TUniquePtr<IFileHandle> UpdFile;
};
