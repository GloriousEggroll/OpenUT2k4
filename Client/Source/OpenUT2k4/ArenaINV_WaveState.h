#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaINV_WaveState.generated.h"

/**
 * Replicated Invasion status for the HUD (the rules object only exists on the server).
 * Spawned by UArenaRules_INV::OnLevelBuilt and updated by its Tick.
 */
UCLASS(NotPlaceable)
class OPENUT2K4_API AArenaINV_WaveState : public AActor
{
	GENERATED_BODY()

public:
	AArenaINV_WaveState();

	/** Current wave (0 before the first one). */
	UPROPERTY(Replicated) int32 Wave = 0;
	UPROPERTY(Replicated) int32 NumWaves = 16;
	/** Monsters are attacking (false during the countdown between waves). */
	UPROPERTY(Replicated) bool bWaveActive = false;
	/** Seconds until the next wave (countdown). */
	UPROPERTY(Replicated) int32 Countdown = 0;
	/** Alive + still to spawn. */
	UPROPERTY(Replicated) int32 MonstersRemaining = 0;
	UPROPERTY(Replicated) int32 MonstersTotal = 0;
	UPROPERTY(Replicated) int32 PlayersAlive = 0;
	UPROPERTY(Replicated) int32 PlayersTotal = 0;
	/** Boss health 0..1 (< 0 = no boss alive) and its name. */
	UPROPERTY(Replicated) float BossHealth = -1.f;
	UPROPERTY(Replicated) FString BossName;
	/** Next wave contains a boss (shown during the countdown). */
	UPROPERTY(Replicated) bool bNextWaveBoss = false;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** The world's wave state (cached; null if none). Works on clients. */
	static AArenaINV_WaveState* Get(const UObject* WorldContext);
};
