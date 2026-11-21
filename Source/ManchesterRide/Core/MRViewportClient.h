#pragma once

#include "CoreMinimal.h"
#include "Engine/GameViewportClient.h"
#include "MRViewportClient.generated.h"

/** Passes typed characters to the player controller, for chat and the login screen's text boxes. */
UCLASS()
class MANCHESTERRIDE_API UMRViewportClient : public UGameViewportClient
{
	GENERATED_BODY()

public:
	virtual bool InputChar(FViewport* InViewport, int32 ControllerId, TCHAR Character) override;
};
