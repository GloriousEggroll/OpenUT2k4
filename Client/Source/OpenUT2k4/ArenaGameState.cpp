#include "ArenaGameState.h"
#include "ArenaPlayerState.h"
#include "ArenaFlag.h"
#include "ArenaGameMode.h"
#include "ArenaGameRules.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"

AArenaGameState::AArenaGameState()
{
	Flags[0] = nullptr;
	Flags[1] = nullptr;
}

void AArenaGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AArenaGameState, GameType);
	DOREPLIFETIME(AArenaGameState, Phase);
	DOREPLIFETIME(AArenaGameState, bInstagib);
	DOREPLIFETIME(AArenaGameState, bAllowTrans);
	DOREPLIFETIME(AArenaGameState, GoalScore);
	DOREPLIFETIME(AArenaGameState, TimeLimit);
	DOREPLIFETIME(AArenaGameState, RemainingTime);
	DOREPLIFETIME(AArenaGameState, CountdownRemaining);
	DOREPLIFETIME(AArenaGameState, bOvertime);
	DOREPLIFETIME(AArenaGameState, TeamScores);
	DOREPLIFETIME(AArenaGameState, LayoutName);
	DOREPLIFETIME(AArenaGameState, WinnerText);
	DOREPLIFETIME(AArenaGameState, WinningTeam);
	DOREPLIFETIME(AArenaGameState, WinningPlayer);
	DOREPLIFETIME(AArenaGameState, Flags);
	DOREPLIFETIME(AArenaGameState, TeamSymbols);
	DOREPLIFETIME(AArenaGameState, ModeId);
	DOREPLIFETIME(AArenaGameState, ModeName);
	DOREPLIFETIME(AArenaGameState, bTeamGame);
	DOREPLIFETIME(AArenaGameState, Round);
	DOREPLIFETIME(AArenaGameState, bSidesSwapped);
	DOREPLIFETIME(AArenaGameState, ModeStatusLines);
}

TArray<AArenaPlayerState*> AArenaGameState::GetSortedPlayers() const
{
	TArray<AArenaPlayerState*> Result;
	for (APlayerState* PS : PlayerArray)
	{
		if (AArenaPlayerState* APS = Cast<AArenaPlayerState>(PS))
		{
			if (!APS->IsOnlyASpectator())
			{
				Result.Add(APS);
			}
		}
	}
	Result.Sort([](const AArenaPlayerState& A, const AArenaPlayerState& B)
	{
		if (A.GetScore() != B.GetScore())
		{
			return A.GetScore() > B.GetScore();
		}
		if (A.Deaths != B.Deaths)
		{
			return A.Deaths < B.Deaths;
		}
		return A.GetPlayerName() < B.GetPlayerName();
	});
	return Result;
}

int32 AArenaGameState::GetRank(const APlayerState* PS) const
{
	const TArray<AArenaPlayerState*> Sorted = GetSortedPlayers();
	for (int32 i = 0; i < Sorted.Num(); ++i)
	{
		if (Sorted[i] == PS)
		{
			// Ties share a rank
			int32 Rank = i;
			while (Rank > 0 && Sorted[Rank - 1]->GetScore() == Sorted[i]->GetScore())
			{
				--Rank;
			}
			return Rank + 1;
		}
	}
	return 0;
}

FString AArenaGameState::GetGameTypeName() const
{
	FString Name = ModeName;
	if (Name.IsEmpty())
	{
		Name = ArenaGameRulesRegistry::GetDisplayName(ModeId);
	}
	if (Name.IsEmpty())
	{
		switch (GameType)
		{
		case EArenaGameType::TeamDeathmatch: Name = TEXT("Team Deathmatch"); break;
		case EArenaGameType::CaptureTheFlag: Name = TEXT("Capture the Flag"); break;
		default:                             Name = TEXT("Deathmatch"); break;
		}
	}
	if (bInstagib)
	{
		Name = TEXT("Instagib ") + Name;
	}
	return Name;
}

UArenaGameRules* AArenaGameState::GetRules() const
{
	const UWorld* World = GetWorld();
	if (const AArenaGameMode* GM = World ? World->GetAuthGameMode<AArenaGameMode>() : nullptr)
	{
		return GM->GetRules();
	}
	if (ModeId.IsEmpty())
	{
		return nullptr;
	}
	if (!ClientRules || !ClientRules->GetModeId().Equals(ModeId, ESearchCase::IgnoreCase))
	{
		ClientRules = ArenaGameRulesRegistry::CreateRules(const_cast<AArenaGameState*>(this), ModeId);
	}
	return ClientRules;
}

const FString& AArenaGameState::GetTeamSymbol(int32 Team) const
{
	static const FString None;
	return (Team == 0 || Team == 1) ? TeamSymbols[Team] : None;
}

void AArenaGameState::OnRep_TeamSymbols()
{
	for (TActorIterator<AArenaFlag> It(GetWorld()); It; ++It)
	{
		It->RefreshTeamSymbol();
	}
}
