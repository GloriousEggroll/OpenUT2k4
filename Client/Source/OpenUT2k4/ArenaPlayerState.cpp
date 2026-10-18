#include "ArenaPlayerState.h"
#include "Net/UnrealNetwork.h"

void AArenaPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AArenaPlayerState, Team);
	DOREPLIFETIME(AArenaPlayerState, Kills);
	DOREPLIFETIME(AArenaPlayerState, Deaths);
	DOREPLIFETIME(AArenaPlayerState, Suicides);
	DOREPLIFETIME(AArenaPlayerState, Captures);
	DOREPLIFETIME(AArenaPlayerState, Returns);
	DOREPLIFETIME(AArenaPlayerState, Spree);
	DOREPLIFETIME(AArenaPlayerState, BotSkill);
	DOREPLIFETIME(AArenaPlayerState, Lives);
	DOREPLIFETIME(AArenaPlayerState, bEliminated);
	DOREPLIFETIME(AArenaPlayerState, ObjectiveScore);
	DOREPLIFETIME(AArenaPlayerState, ModeFlags);
	DOREPLIFETIME(AArenaPlayerState, CharacterName);
}

void AArenaPlayerState::Reset()
{
	Super::Reset();
	Kills = Deaths = Suicides = Captures = Returns = Spree = 0;
	MultiKillCount = 0;
	Lives = -1;
	bEliminated = false;
	ObjectiveScore = 0;
	ModeFlags = 0;
	SetScore(0.f);
}
