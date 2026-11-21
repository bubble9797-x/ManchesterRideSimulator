#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "Core/MRRiderLook.h"
#include "MRPlayerState.generated.h"

UCLASS()
class MANCHESTERRIDE_API AMRPlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(Replicated, BlueprintReadOnly) FName VehicleId;
	UPROPERTY(Replicated, BlueprintReadOnly) FLinearColor PaintColor = FLinearColor::Red;
	UPROPERTY(Replicated) FMRRiderLook RiderLook;
	UPROPERTY(Replicated, BlueprintReadOnly) FLinearColor JacketColor = FLinearColor::Blue;
	UPROPERTY(Replicated, BlueprintReadOnly) FLinearColor HelmetColor = FLinearColor::White;

	UPROPERTY(Replicated, BlueprintReadOnly) int32 TrickScore = 0;
	UPROPERTY(Replicated, BlueprintReadOnly) float BestWheelieDistance = 0.f;
	UPROPERTY(Replicated, BlueprintReadOnly) float BestWheelieTime = 0.f;
	UPROPERTY(Replicated, BlueprintReadOnly) float CompetitionScore = 0.f;
};
