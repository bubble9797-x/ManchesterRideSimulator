#include "Core/MRViewportClient.h"

#include "Core/MRPlayerController.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

bool UMRViewportClient::InputChar(FViewport* InViewport, int32 ControllerId, TCHAR Character)
{
	if (const UGameInstance* GI = GetGameInstance())
	{
		if (AMRPlayerController* PC = Cast<AMRPlayerController>(GI->GetFirstLocalPlayerController()))
		{
			if (PC->HandleTextChar(Character)) return true;
		}
	}
	return Super::InputChar(InViewport, ControllerId, Character);
}
