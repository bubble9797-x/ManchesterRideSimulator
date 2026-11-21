#include "Core/MRPlayerState.h"
#include "Net/UnrealNetwork.h"

void AMRPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AMRPlayerState, VehicleId);
	DOREPLIFETIME(AMRPlayerState, PaintColor);
	DOREPLIFETIME(AMRPlayerState, RiderLook);
	DOREPLIFETIME(AMRPlayerState, JacketColor);
	DOREPLIFETIME(AMRPlayerState, HelmetColor);
	DOREPLIFETIME(AMRPlayerState, TrickScore);
	DOREPLIFETIME(AMRPlayerState, BestWheelieDistance);
	DOREPLIFETIME(AMRPlayerState, BestWheelieTime);
	DOREPLIFETIME(AMRPlayerState, CompetitionScore);
}
