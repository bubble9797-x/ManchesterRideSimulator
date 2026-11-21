#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Core/MRGameState.h"
#include "Core/MRSaveGame.h"
#include "MRGameMode.generated.h"

class AMRPlayerState;
class AMRTestEnvironment;
class AMRGeoWorld;

/**
 * Free-roam game mode. There is no vehicle ownership, purchasing or unlocking:
 * any catalogue vehicle id a client asks for is spawned (unknown ids fall back
 * to the default vehicle).
 */
UCLASS()
class MANCHESTERRIDE_API AMRGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AMRGameMode();

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void StartPlay() override;
	virtual void PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage) override;
	virtual FString InitNewPlayer(APlayerController* NewPlayerController, const FUniqueNetIdRepl& UniqueId, const FString& Options, const FString& Portal) override;
	virtual void RestartPlayer(AController* NewPlayer) override;
	virtual APawn* SpawnDefaultPawnAtTransform_Implementation(AController* NewPlayer, const FTransform& SpawnTransform) override;
	virtual void Tick(float DeltaSeconds) override;

	/** Swap the controller's vehicle in place (garage "ride this"). Always allowed: every vehicle is free. */
	void ChangeVehicle(AController* Controller, FName VehicleId, FLinearColor Paint, FLinearColor Jacket, FLinearColor Helmet);

	/** Server-side trick validation + scoring. */
	void ScoreTrick(AMRPlayerState* PS, uint8 TrickType, int32 Points, float Distance, float Duration);

	void StartCompetition(EMRCompetition Type, float DurationSeconds);

	int32 GetMaxPlayers() const { return MaxPlayers; }

private:
	FTransform ChooseSpawnTransform(AController* Controller) const;
	/** Spawns the environment and the geo world (or test district) once; safe to call before StartPlay. */
	void EnsureWorldActors();
	bool bWorldActorsSpawned = false;
	AMRTestEnvironment* FindTestEnvironment() const;
	void EndCompetition();

	int32 MaxPlayers = 8;
	EMRSessionVisibility Visibility = EMRSessionVisibility::Public;
	FString SessionName;
	int32 SpawnCounter = 0;
	FString WorldMode; // "test" forces the test district
	UPROPERTY() TObjectPtr<AMRGeoWorld> GeoWorld;
};
