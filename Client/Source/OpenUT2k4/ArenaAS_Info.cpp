#include "ArenaAS_Info.h"
#include "Net/UnrealNetwork.h"

AArenaAS_Info::AArenaAS_Info()
{
	bReplicates = true;
	bAlwaysRelevant = true;
}

void AArenaAS_Info::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AArenaAS_Info, AttackingTeam);
	DOREPLIFETIME(AArenaAS_Info, CurrentStage);
	DOREPLIFETIME(AArenaAS_Info, NumStages);
	DOREPLIFETIME(AArenaAS_Info, PairRound);
	DOREPLIFETIME(AArenaAS_Info, PairNumber);
	DOREPLIFETIME(AArenaAS_Info, TimeToBeat);
	DOREPLIFETIME(AArenaAS_Info, FirstAttackTeam);
	DOREPLIFETIME(AArenaAS_Info, bFirstSucceeded);
	DOREPLIFETIME(AArenaAS_Info, FirstTime);
	DOREPLIFETIME(AArenaAS_Info, FirstStages);
	DOREPLIFETIME(AArenaAS_Info, bOvertimePair);
}
