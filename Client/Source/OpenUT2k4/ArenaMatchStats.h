#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "ArenaMatchStats.generated.h"

class AController;
class APlayerState;

/**
 * Per player counters of a match that the special awards of UT2004's single player use
 * (TeamPlayerReplicationInfo flakcount, combocount, headcount, ranovercount: kills with the flak cannon, adrenaline
 * combos, sniper head shots, road kills). Filled by the character's death and combo code; read when a match ends.
 */
USTRUCT()
struct FArenaPlayerMatchStats
{
	GENERATED_BODY()
	UPROPERTY() int32 FlakKills = 0;
	UPROPERTY() int32 Combos = 0;
	UPROPERTY() int32 HeadShots = 0;
	UPROPERTY() int32 RoadKills = 0;
};

UCLASS()
class OPENUT2K4_API UArenaMatchStats : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/** A kill by Killer (not a suicide) with a weapon of that display name (AArenaWeapon::WeaponName or a cause like "Roadkill"). */
	static void RecordKill(UWorld* World, AController* Killer, AController* Victim, const FString& WeaponName, bool bHeadshot);
	/** An adrenaline combo was started by Who. */
	static void RecordCombo(UWorld* World, AController* Who);
	/** The counters of a player (zero when none). */
	static FArenaPlayerMatchStats Get(const UWorld* World, const APlayerState* Player);

private:
	TMap<TWeakObjectPtr<const APlayerState>, FArenaPlayerMatchStats> Stats;
	FArenaPlayerMatchStats* Find(AController* C);
};
