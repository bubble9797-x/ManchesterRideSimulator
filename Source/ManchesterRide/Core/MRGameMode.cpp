#include "Core/MRGameMode.h"

#include "ManchesterRide.h"
#include "Core/MRGameInstance.h"
#include "Core/MRHUD.h"
#include "Core/MRPlayerController.h"
#include "Core/MRPlayerState.h"
#include "Net/MRNetworkSettings.h"
#include "TrickTracker.h"
#include "Vehicles/MRRideVehicle.h"
#include "Vehicles/MRVehicleLibrary.h"
#include "World/MRGeoWorld.h"
#include "World/MRTestEnvironment.h"
#include "World/MRWorldEnvironment.h"

#include "EngineUtils.h"
#include "GameFramework/GameSession.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Kismet/GameplayStatics.h"

namespace
{
	FLinearColor ParseColor(const FString& Hex, const FLinearColor& Fallback)
	{
		if (Hex.Len() < 6) return Fallback;
		return FLinearColor::FromSRGBColor(FColor::FromHex(Hex));
	}
}

AMRGameMode::AMRGameMode()
{
	PrimaryActorTick.bCanEverTick = true;
	DefaultPawnClass = AMRRideVehicle::StaticClass();
	PlayerControllerClass = AMRPlayerController::StaticClass();
	PlayerStateClass = AMRPlayerState::StaticClass();
	GameStateClass = AMRGameState::StaticClass();
	HUDClass = AMRHUD::StaticClass();
	bUseSeamlessTravel = false;
}

void AMRGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);
	const UMRNetworkSettings* NS = GetDefault<UMRNetworkSettings>();
	MaxPlayers = FMath::Clamp(UGameplayStatics::GetIntOption(Options, TEXT("MaxPlayers"), NS->DefaultMaxPlayers), 1, NS->MaxPlayersCeiling);
	Visibility = static_cast<EMRSessionVisibility>(FMath::Clamp(UGameplayStatics::GetIntOption(Options, TEXT("Vis"), 0), 0, 2));
	SessionName = FGenericPlatformHttp::UrlDecode(UGameplayStatics::ParseOption(Options, TEXT("Session")));
	if (SessionName.IsEmpty()) SessionName = TEXT("Manchester Freeride");
	WorldMode = UGameplayStatics::ParseOption(Options, TEXT("world"));
	if (GameSession) GameSession->MaxPlayers = MaxPlayers;
}

AMRTestEnvironment* AMRGameMode::FindTestEnvironment() const
{
	for (TActorIterator<AMRTestEnvironment> It(GetWorld()); It; ++It) return *It;
	return nullptr;
}

void AMRGameMode::StartPlay()
{
	EnsureWorldActors();
	if (AMRGameState* GS = GetGameState<AMRGameState>())
	{
		GS->SessionName = SessionName;
		GS->MaxPlayers = MaxPlayers;
		GS->Visibility = Visibility;
	}
	Super::StartPlay();
}

void AMRGameMode::EnsureWorldActors()
{
	// Called from StartPlay AND from RestartPlayer: in standalone/listen play the local player logs in
	// and is spawned BEFORE StartPlay, so the world has to exist by then or the spawn falls back to the
	// map's PlayerStart instead of the geo world's spawn point.
	if (bWorldActorsSpawned) return;
	bWorldActorsSpawned = true;
	UWorld* World = GetWorld();
	FActorSpawnParameters P;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (!AMRWorldEnvironment::Get(this))
	{
		World->SpawnActor<AMRWorldEnvironment>(AMRWorldEnvironment::StaticClass(), FTransform::Identity, P);
	}
	// Maps built by the geo pipeline tag their ground "MRGround"; otherwise use the test district.
	bool bHasGround = false;
	for (TActorIterator<AActor> It(World); It && !bHasGround; ++It)
	{
		bHasGround = It->ActorHasTag(TEXT("MRGround"));
	}
	if (!bHasGround && !FindTestEnvironment())
	{
		// Real Greater Manchester geometry (Tools/geo_pipeline output) when it is installed and not
		// overridden with ?world=test; otherwise the procedural test district.
		if (WorldMode != TEXT("test") && AMRGeoWorld::HasGeoData())
		{
			GeoWorld = World->SpawnActor<AMRGeoWorld>(AMRGeoWorld::StaticClass(), FTransform::Identity, P);
		}
		else
		{
			World->SpawnActor<AMRTestEnvironment>(AMRTestEnvironment::StaticClass(), FTransform::Identity, P);
		}
	}
}

void AMRGameMode::PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
	Super::PreLogin(Options, Address, UniqueId, ErrorMessage);
	if (!ErrorMessage.IsEmpty()) return;

	const int32 Expected = GetDefault<UMRNetworkSettings>()->ProtocolVersion;
	const int32 ClientVer = UGameplayStatics::GetIntOption(Options, TEXT("ver"), -1);
	if (ClientVer != Expected)
	{
		ErrorMessage = FString::Printf(TEXT("MR_VERSION host protocol %d, yours %d"), Expected, ClientVer);
		return;
	}
	if (GetNumPlayers() >= MaxPlayers)
	{
		ErrorMessage = TEXT("MR_FULL");
		return;
	}

	const UMRGameInstance* GI = GetGameInstance<UMRGameInstance>();
	const UMRSaveGame* Profile = GI ? GI->GetProfile() : nullptr;
	if (Visibility == EMRSessionVisibility::Private && Profile && !Profile->HostPassword.IsEmpty())
	{
		const FString Given = FGenericPlatformHttp::UrlDecode(UGameplayStatics::ParseOption(Options, TEXT("pw")));
		if (Given != Profile->HostPassword)
		{
			ErrorMessage = TEXT("MR_PASSWORD");
			return;
		}
	}
	if (Visibility == EMRSessionVisibility::FriendsOnly && Profile)
	{
		const FString Name = UGameplayStatics::ParseOption(Options, TEXT("Name"));
		const bool bFriend = Profile->FriendsList.ContainsByPredicate([&](const FString& F) { return F.Equals(Name, ESearchCase::IgnoreCase); });
		if (!bFriend)
		{
			ErrorMessage = TEXT("MR_FRIENDS");
			return;
		}
	}
	UE_LOG(LogManchesterRide, Log, TEXT("PreLogin accepted from %s"), *Address);
}

FString AMRGameMode::InitNewPlayer(APlayerController* NewPlayerController, const FUniqueNetIdRepl& UniqueId, const FString& Options, const FString& Portal)
{
	const FString Result = Super::InitNewPlayer(NewPlayerController, UniqueId, Options, Portal); // handles ?Name=
	if (AMRPlayerState* PS = NewPlayerController ? NewPlayerController->GetPlayerState<AMRPlayerState>() : nullptr)
	{
		const FName Veh(*UGameplayStatics::ParseOption(Options, TEXT("veh")));
		PS->VehicleId = UMRVehicleLibrary::IsValidVehicle(Veh) ? Veh : UMRVehicleLibrary::GetDefaultVehicleId();
		PS->PaintColor = ParseColor(UGameplayStatics::ParseOption(Options, TEXT("paint")), FLinearColor(0.75f, 0.08f, 0.08f));
		PS->JacketColor = ParseColor(UGameplayStatics::ParseOption(Options, TEXT("jacket")), FLinearColor(0.04f, 0.1f, 0.35f));
		PS->HelmetColor = ParseColor(UGameplayStatics::ParseOption(Options, TEXT("helmet")), FLinearColor(0.92f, 0.92f, 0.92f));
		PS->RiderLook = FMRRiderLook::Decode(UGameplayStatics::ParseOption(Options, TEXT("look"))); // sanitised
	}
	return Result;
}

FTransform AMRGameMode::ChooseSpawnTransform(AController* Controller) const
{
	if (GeoWorld)
	{
		return GeoWorld->GetSpawnTransform(SpawnCounter);
	}
	if (const AMRTestEnvironment* Env = FindTestEnvironment())
	{
		return Env->GetSpawnTransform(SpawnCounter);
	}
	if (AActor* Start = const_cast<AMRGameMode*>(this)->FindPlayerStart(Controller))
	{
		return Start->GetActorTransform();
	}
	return FTransform(FVector(0, 0, 300.f));
}

void AMRGameMode::RestartPlayer(AController* NewPlayer)
{
	if (!NewPlayer || NewPlayer->IsPendingKillPending()) return;
	EnsureWorldActors();
	const FTransform Spawn = ChooseSpawnTransform(NewPlayer);
	UE_LOG(LogManchesterRide, Log, TEXT("Spawning player %d at %s (%s)"), SpawnCounter, *Spawn.GetLocation().ToCompactString(), GeoWorld ? TEXT("geo world") : TEXT("no geo world"));
	RestartPlayerAtTransform(NewPlayer, Spawn);
	++SpawnCounter;
}

APawn* AMRGameMode::SpawnDefaultPawnAtTransform_Implementation(AController* NewPlayer, const FTransform& SpawnTransform)
{
	FActorSpawnParameters P;
	P.Instigator = GetInstigator();
	P.ObjectFlags |= RF_Transient;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	P.bDeferConstruction = true;
	AMRRideVehicle* V = GetWorld()->SpawnActor<AMRRideVehicle>(AMRRideVehicle::StaticClass(), SpawnTransform, P);
	if (!V) return nullptr;
	if (const AMRPlayerState* PS = NewPlayer ? NewPlayer->GetPlayerState<AMRPlayerState>() : nullptr)
	{
		V->InitVehicle(PS->VehicleId, PS->PaintColor, PS->JacketColor, PS->HelmetColor);
		V->SetRiderLook(PS->RiderLook);
	}
	V->FinishSpawning(SpawnTransform);
	return V;
}

void AMRGameMode::ChangeVehicle(AController* Controller, FName VehicleId, FLinearColor Paint, FLinearColor Jacket, FLinearColor Helmet)
{
	if (!Controller) return;
	AMRPlayerState* PS = Controller->GetPlayerState<AMRPlayerState>();
	if (PS)
	{
		PS->VehicleId = UMRVehicleLibrary::IsValidVehicle(VehicleId) ? VehicleId : UMRVehicleLibrary::GetDefaultVehicleId();
		PS->PaintColor = Paint;
		PS->JacketColor = Jacket;
		PS->HelmetColor = Helmet;
	}
	FTransform Where = ChooseSpawnTransform(Controller);
	if (APawn* Old = Controller->GetPawn())
	{
		Where = FTransform(FRotator(0.f, Old->GetActorRotation().Yaw, 0.f), Old->GetActorLocation() + FVector(0, 0, 40.f));
		Controller->UnPossess();
		Old->Destroy();
	}
	RestartPlayerAtTransform(Controller, Where);
}

void AMRGameMode::ScoreTrick(AMRPlayerState* PS, uint8 TrickType, int32 Points, float Distance, float Duration)
{
	if (!PS) return;
	using MRSim::ETrick;
	const ETrick T = static_cast<ETrick>(TrickType);
	// Sanity caps: clients compute scores, the server rejects anything implausible.
	int32 Cap = 0;
	switch (T)
	{
	case ETrick::Wheelie:
	case ETrick::Manual:   Cap = FMath::CeilToInt(Distance * 30.f) + 50; if (Distance > 5000.f || Duration > 600.f) Cap = 0; break;
	case ETrick::Stoppie:  Cap = FMath::CeilToInt(Distance * 50.f + Duration * 40.f) + 50; if (Distance > 300.f) Cap = 0; break;
	case ETrick::BunnyHop:
	case ETrick::Air:      Cap = FMath::CeilToInt(Duration * 80.f) + 3 * 1000 + 6 * 150; if (Duration > 20.f) Cap = 0; break;
	case ETrick::Slide:    Cap = FMath::CeilToInt(Duration * 50.f) + 20; if (Duration > 60.f) Cap = 0; break;
	default: break;
	}
	if (Points <= 0 || Points > Cap)
	{
		UE_LOG(LogManchesterRide, Warning, TEXT("Rejected trick from %s: type %d points %d cap %d"), *PS->GetPlayerName(), TrickType, Points, Cap);
		return;
	}
	PS->TrickScore += Points;
	if ((T == ETrick::Wheelie || T == ETrick::Manual) && Distance > PS->BestWheelieDistance)
	{
		PS->BestWheelieDistance = Distance;
		PS->BestWheelieTime = Duration;
	}

	AMRGameState* GS = GetGameState<AMRGameState>();
	if (GS && GS->Competition != EMRCompetition::None && GS->GetCompetitionTimeLeft() > 0.f)
	{
		if (GS->Competition == EMRCompetition::LongestWheelie && (T == ETrick::Wheelie || T == ETrick::Manual))
		{
			PS->CompetitionScore = FMath::Max(PS->CompetitionScore, Distance);
		}
		else if (GS->Competition == EMRCompetition::TrickScore)
		{
			PS->CompetitionScore += Points;
		}
	}
}

void AMRGameMode::StartCompetition(EMRCompetition Type, float DurationSeconds)
{
	AMRGameState* GS = GetGameState<AMRGameState>();
	if (!GS) return;
	for (APlayerState* P : GS->PlayerArray)
	{
		if (AMRPlayerState* MP = Cast<AMRPlayerState>(P)) MP->CompetitionScore = 0.f;
	}
	GS->Competition = Type;
	GS->CompetitionEndTime = GS->GetServerWorldTimeSeconds() + DurationSeconds;
}

void AMRGameMode::EndCompetition()
{
	AMRGameState* GS = GetGameState<AMRGameState>();
	if (!GS) return;
	const AMRPlayerState* Best = nullptr;
	for (APlayerState* P : GS->PlayerArray)
	{
		const AMRPlayerState* MP = Cast<AMRPlayerState>(P);
		if (MP && MP->CompetitionScore > 0.f && (!Best || MP->CompetitionScore > Best->CompetitionScore)) Best = MP;
	}
	GS->LastWinner = Best ? FString::Printf(TEXT("%s won %s (%.1f)"), *Best->GetPlayerName(), *AMRGameState::CompetitionName(GS->Competition).ToString(), Best->CompetitionScore)
		: TEXT("No entries");
	GS->Competition = EMRCompetition::None;
}

void AMRGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const AMRGameState* GS = GetGameState<AMRGameState>();
	if (GS && GS->Competition != EMRCompetition::None && GS->GetCompetitionTimeLeft() <= 0.f)
	{
		EndCompetition();
	}
}
