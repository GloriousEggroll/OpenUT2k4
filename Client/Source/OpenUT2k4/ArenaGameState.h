#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "ArenaTypes.h"
#include "ArenaGameState.generated.h"

class AArenaFlag;
class AArenaPlayerState;
class UArenaGameRules;

UCLASS()
class OPENUT2K4_API AArenaGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	AArenaGameState();

	/** Legacy enum (compatibility). The active mode is ModeId. */
	UPROPERTY(Replicated, BlueprintReadOnly) EArenaGameType GameType = EArenaGameType::Deathmatch;
	/** Registered mode id of the match ("DM", "CTF", ...). */
	UPROPERTY(Replicated, BlueprintReadOnly) FString ModeId;
	/** Display name of the mode ("Capture the Flag"). */
	UPROPERTY(Replicated, BlueprintReadOnly) FString ModeName;
	/** From UArenaGameRules::IsTeamGame. */
	UPROPERTY(Replicated, BlueprintReadOnly) bool bTeamGame = false;
	/** Current round, 1-based (AArenaGameMode::EndRound / StartNextRound). */
	UPROPERTY(Replicated, BlueprintReadOnly) int32 Round = 1;
	/** Teams play on swapped sides (team 0 spawns at the blue base ...). */
	UPROPERTY(Replicated, BlueprintReadOnly) bool bSidesSwapped = false;
	/** Free-form mode status for the HUD (drawn by the default UArenaGameRules::DrawHUD), e.g. "Ball: carried by Vexa". */
	UPROPERTY(Replicated, BlueprintReadOnly) TArray<FString> ModeStatusLines;
	UPROPERTY(Replicated, BlueprintReadOnly) EArenaMatchPhase Phase = EArenaMatchPhase::Countdown;
	UPROPERTY(Replicated, BlueprintReadOnly) bool bInstagib = false;
	/** UE2 DeathMatch.bAllowTrans: the translocator is in the default inventory (shown in the in-game Rules tab). */
	UPROPERTY(Replicated, BlueprintReadOnly) bool bAllowTrans = false;
	/** Frag limit (DM/TDM) or capture limit (CTF). 0 = none */
	UPROPERTY(Replicated, BlueprintReadOnly) int32 GoalScore = 25;
	/** Seconds; 0 = no time limit */
	UPROPERTY(Replicated, BlueprintReadOnly) int32 TimeLimit = 0;
	UPROPERTY(Replicated, BlueprintReadOnly) int32 RemainingTime = 0;
	UPROPERTY(Replicated, BlueprintReadOnly) int32 CountdownRemaining = 0;
	UPROPERTY(Replicated, BlueprintReadOnly) bool bOvertime = false;
	UPROPERTY(Replicated) int32 TeamScores[2] = { 0, 0 };
	UPROPERTY(Replicated, BlueprintReadOnly) FString LayoutName;
	/** Set at match end, e.g. "Red Team wins!" */
	UPROPERTY(Replicated, BlueprintReadOnly) FString WinnerText;
	UPROPERTY(Replicated, BlueprintReadOnly) int32 WinningTeam = -1;
	UPROPERTY(Replicated, BlueprintReadOnly) TObjectPtr<APlayerState> WinningPlayer;

	/** Red = 0, Blue = 1. Only valid in CTF. */
	UPROPERTY(Replicated) TObjectPtr<AArenaFlag> Flags[2];
	/**
	 * Team symbol textures of the match (red, blue), paths relative to /Game/Replacements or /Game/Imported
	 * (ArenaArtItems::PickTeamSymbols: random per match as in UT2004, ?RedTeamSymbol= / ?BlueTeamSymbol=).
	 * Drawn on every flag of the team and in the HUD team scores. Empty outside team games.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_TeamSymbols) FString TeamSymbols[2];
	/** Team symbol path for a team (empty for no team / none picked). */
	const FString& GetTeamSymbol(int32 Team) const;

	bool IsTeamGame() const { return bTeamGame; }
	bool IsPlaying() const { return Phase == EArenaMatchPhase::Playing; }

	/** Players sorted for the scoreboard (score desc, deaths asc). */
	TArray<AArenaPlayerState*> GetSortedPlayers() const;
	/** Rank of a player (1-based) in DM, and the spread to the leader / next. */
	int32 GetRank(const APlayerState* PS) const;

	/** Mode display name, prefixed with "Instagib " when instagib is on. */
	FString GetGameTypeName() const;
	/**
	 * The rules of this match: the game mode's instance on the server, a locally created instance (from
	 * ModeId) on network clients, used for HUD hooks only. May be null on clients before ModeId replicated.
	 */
	UArenaGameRules* GetRules() const;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	/** Re-applies the symbols to the flags (clients: the flags may be built before the symbols arrive). */
	UFUNCTION() void OnRep_TeamSymbols();

	/** Client-side rules instance for HUD hooks (see GetRules). */
	UPROPERTY(Transient) mutable TObjectPtr<UArenaGameRules> ClientRules;
};
