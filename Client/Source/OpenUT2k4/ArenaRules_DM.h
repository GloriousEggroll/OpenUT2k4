#pragma once

#include "CoreMinimal.h"
#include "ArenaGameRules.h"
#include "ArenaRules_DM.generated.h"

/** Deathmatch: every player for themselves, first to the frag limit. All behaviour is the UArenaGameRules default. */
UCLASS()
class OPENUT2K4_API UArenaRules_DM : public UArenaGameRules
{
	GENERATED_BODY()

public:
	virtual EArenaGameType GetLegacyGameType() const override { return EArenaGameType::Deathmatch; }
};
