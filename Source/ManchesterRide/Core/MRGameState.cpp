#include "Core/MRGameState.h"
#include "Net/UnrealNetwork.h"
#include "World/MRPhotoLayer.h"
#include "World/MRGeoWorld.h"
#include "TimerManager.h"

void AMRGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AMRGameState, SessionName);
	DOREPLIFETIME(AMRGameState, MaxPlayers);
	DOREPLIFETIME(AMRGameState, Visibility);
	DOREPLIFETIME(AMRGameState, Competition);
	DOREPLIFETIME(AMRGameState, CompetitionEndTime);
	DOREPLIFETIME(AMRGameState, LastWinner);
	DOREPLIFETIME(AMRGameState, bPhotoMap);
	DOREPLIFETIME(AMRGameState, PhotoMapToken);
}

void AMRGameState::SetPhotoMap(bool bOn, const FString& Token)
{
	if (!HasAuthority()) return;
	const bool bEffective = bOn && !Token.IsEmpty() && AMRPhotoLayer::IsAvailable();
	bPhotoMap = bEffective;
	PhotoMapToken = bEffective ? Token : FString();
	OnRep_PhotoMap(); // apply on the host / in solo play
}

void AMRGameState::OnRep_PhotoMap()
{
	// On a joining client the replicated geo world can arrive after this; retry until it exists.
	if (bPhotoMap && !AMRGeoWorld::Find(this))
	{
		GetWorldTimerManager().SetTimer(PhotoRetry, this, &AMRGameState::OnRep_PhotoMap, 0.5f, false);
		return;
	}
	AMRPhotoLayer::Apply(this, bPhotoMap, PhotoMapToken);
}

float AMRGameState::GetCompetitionTimeLeft() const
{
	if (Competition == EMRCompetition::None) return 0.f;
	return FMath::Max(0.f, static_cast<float>(CompetitionEndTime - GetServerWorldTimeSeconds()));
}

FText AMRGameState::CompetitionName(EMRCompetition C)
{
	switch (C)
	{
	case EMRCompetition::LongestWheelie: return NSLOCTEXT("MR", "CompWheelie", "Longest wheelie");
	case EMRCompetition::TrickScore:     return NSLOCTEXT("MR", "CompTricks", "Trick score");
	default:                             return NSLOCTEXT("MR", "CompNone", "Freeride");
	}
}
