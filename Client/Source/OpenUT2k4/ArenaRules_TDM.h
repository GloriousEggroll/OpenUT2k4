#pragma once

#include "CoreMinimal.h"
#include "ArenaGameRules.h"
#include "ArenaRules_TDM.generated.h"

/** Team Deathmatch: two teams, every frag scores a team point, first team to the goal wins. */
UCLASS()
class OPENUT2K4_API UArenaRules_TDM : public UArenaGameRules
{
	GENERATED_BODY()

public:
	virtual bool IsTeamGame() const override { return true; }
	virtual EArenaGameType GetLegacyGameType() const override { return EArenaGameType::TeamDeathmatch; }
	virtual int32 GetDefaultGoalScore() const override { return 60; }
	virtual bool KillsAddTeamScore() const override { return true; }
};
