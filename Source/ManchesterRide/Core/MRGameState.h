#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "Core/MRSaveGame.h"
#include "MRGameState.generated.h"

UENUM(BlueprintType)
enum class EMRCompetition : uint8
{
	None,
	LongestWheelie, // best single wheelie/manual distance in the window
	TrickScore,     // most trick points in the window
};

UCLASS()
class MANCHESTERRIDE_API AMRGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(Replicated, BlueprintReadOnly) FString SessionName;
	UPROPERTY(Replicated, BlueprintReadOnly) int32 MaxPlayers = 8;
	UPROPERTY(Replicated, BlueprintReadOnly) EMRSessionVisibility Visibility = EMRSessionVisibility::Public;

	UPROPERTY(Replicated, BlueprintReadOnly) EMRCompetition Competition = EMRCompetition::None;
	UPROPERTY(Replicated, BlueprintReadOnly) double CompetitionEndTime = 0.0; // server world time
	UPROPERTY(Replicated, BlueprintReadOnly) FString LastWinner;

	/**
	 * Photoreal map is a SESSION setting: everyone rides the same surface (riding is owner-simulated,
	 * so a player on a different ground model would appear offset for everyone else). The host's
	 * choice and the host's Cesium ion token are replicated; clients use them and only the host needs
	 * a token. Server only: SetPhotoMap.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_PhotoMap, BlueprintReadOnly) bool bPhotoMap = false;
	UPROPERTY(ReplicatedUsing = OnRep_PhotoMap) FString PhotoMapToken;
	void SetPhotoMap(bool bOn, const FString& Token);
	UFUNCTION() void OnRep_PhotoMap();
	FTimerHandle PhotoRetry;

	float GetCompetitionTimeLeft() const;
	static FText CompetitionName(EMRCompetition C);
};
