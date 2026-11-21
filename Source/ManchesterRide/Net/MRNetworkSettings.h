#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MRNetworkSettings.generated.h"

/** Network tuning read from Config/DefaultGame.ini [/Script/ManchesterRide.MRNetworkSettings]. */
UCLASS(Config = Game)
class MANCHESTERRIDE_API UMRNetworkSettings : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(Config) int32 ProtocolVersion = 1;
	UPROPERTY(Config) int32 DefaultPort = 7777;
	UPROPERTY(Config) int32 DefaultMaxPlayers = 8;
	UPROPERTY(Config) int32 MaxPlayersCeiling = 16;
	UPROPERTY(Config) float ClientSendRate = 30.f;
	UPROPERTY(Config) float InterpolationDelay = 0.12f;
	UPROPERTY(Config) int32 AutoReconnectAttempts = 3;
	/** The Manchester Ride dedicated server the Multiplayer screen joins (host:port, a Playit.gg UDP tunnel address). */
	UPROPERTY(Config) FString DedicatedServer = TEXT("209.25.141.16:38420");
};
