#include "ArenaINV_WaveState.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"
#include "EngineUtils.h"

AArenaINV_WaveState::AArenaINV_WaveState()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(4.f);
	SetCanBeDamaged(false);
	SetHidden(true);
}

void AArenaINV_WaveState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AArenaINV_WaveState, Wave);
	DOREPLIFETIME(AArenaINV_WaveState, NumWaves);
	DOREPLIFETIME(AArenaINV_WaveState, bWaveActive);
	DOREPLIFETIME(AArenaINV_WaveState, Countdown);
	DOREPLIFETIME(AArenaINV_WaveState, MonstersRemaining);
	DOREPLIFETIME(AArenaINV_WaveState, MonstersTotal);
	DOREPLIFETIME(AArenaINV_WaveState, PlayersAlive);
	DOREPLIFETIME(AArenaINV_WaveState, PlayersTotal);
	DOREPLIFETIME(AArenaINV_WaveState, BossHealth);
	DOREPLIFETIME(AArenaINV_WaveState, BossName);
	DOREPLIFETIME(AArenaINV_WaveState, bNextWaveBoss);
}

AArenaINV_WaveState* AArenaINV_WaveState::Get(const UObject* WorldContext)
{
	static TWeakObjectPtr<AArenaINV_WaveState> Cached;
	UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	if (!World)
	{
		return nullptr;
	}
	if (Cached.IsValid() && Cached->GetWorld() == World)
	{
		return Cached.Get();
	}
	TActorIterator<AArenaINV_WaveState> It(World);
	if (It)
	{
		Cached = *It;
		return *It;
	}
	return nullptr;
}
