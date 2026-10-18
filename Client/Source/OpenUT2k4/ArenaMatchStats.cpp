#include "ArenaMatchStats.h"
#include "ArenaWeapons.h"
#include "ArenaWeaponsExtra.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerState.h"

FArenaPlayerMatchStats* UArenaMatchStats::Find(AController* C)
{
	const APlayerState* PS = C ? C->PlayerState : nullptr;
	return PS ? &Stats.FindOrAdd(PS) : nullptr;
}

void UArenaMatchStats::RecordKill(UWorld* World, AController* Killer, AController* Victim, const FString& WeaponName, bool bHeadshot)
{
	UArenaMatchStats* S = World ? World->GetSubsystem<UArenaMatchStats>() : nullptr;
	if (!S || !Killer || Killer == Victim)
	{
		return;
	}
	FArenaPlayerMatchStats* P = S->Find(Killer);
	if (!P)
	{
		return;
	}
	// DamTypeFlakChunk / DamTypeFlakShell, DamTypeSniperHeadShot / DamTypeClassicHeadshot, DamTypeRoadkill (IncrementKills)
	if (WeaponName.Equals(GetDefault<AWeap_ShardCannon>()->WeaponName, ESearchCase::IgnoreCase))
	{
		P->FlakKills++;
	}
	if (bHeadshot && (WeaponName.Equals(GetDefault<AWeap_RailRifle>()->WeaponName, ESearchCase::IgnoreCase)
		|| WeaponName.Equals(GetDefault<AWeap_MarksmanRifle>()->WeaponName, ESearchCase::IgnoreCase)))
	{
		P->HeadShots++;
	}
	if (WeaponName.Equals(TEXT("Roadkill"), ESearchCase::IgnoreCase))
	{
		P->RoadKills++;
	}
}

void UArenaMatchStats::RecordCombo(UWorld* World, AController* Who)
{
	UArenaMatchStats* S = World ? World->GetSubsystem<UArenaMatchStats>() : nullptr;
	if (FArenaPlayerMatchStats* P = S ? S->Find(Who) : nullptr)
	{
		P->Combos++;
	}
}

FArenaPlayerMatchStats UArenaMatchStats::Get(const UWorld* World, const APlayerState* Player)
{
	const UArenaMatchStats* S = World ? World->GetSubsystem<UArenaMatchStats>() : nullptr;
	const FArenaPlayerMatchStats* P = S && Player ? S->Stats.Find(Player) : nullptr;
	return P ? *P : FArenaPlayerMatchStats();
}
