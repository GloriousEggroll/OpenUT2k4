#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "ArenaServerGameInstance.generated.h"

/**
 * The game instance of the dedicated server (Server/ArenaDedicatedServer.h; set only in server mode, the game keeps
 * the engine's default). Instead of the default map it opens the server's first match, built from its configuration;
 * no local player exists.
 */
UCLASS()
class OPENUT2K4_API UArenaServerGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	virtual void StartGameInstance() override;
};
