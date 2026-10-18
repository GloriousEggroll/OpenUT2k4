#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ArenaTypes.h"
#include "ArenaGameRules.generated.h"

class AArenaGameMode;
class AArenaGameState;
class AArenaPlayerState;
class AArenaCharacter;
class AArenaBotController;
class AArenaHUD;
class AArenaLevelBuilder;
class AArenaNavGraph;
class AArenaWeapon;
class AArenaPickup;
class APlayerStart;
class APlayerState;
class APlayerController;
class UCanvas;
struct FArenaLayoutInfo;
enum class EArenaPickupType : uint8;

// =============================================================================================
// Bot objectives
// =============================================================================================

/** What a bot should do at / with a mode objective. Purely informational for the bot's movement code. */
enum class EArenaBotAction : uint8
{
	Move,     // just go there (e.g. a staging point)
	Attack,   // go to / chase the actor and fight there (enemy carrier, enemy node, enemy base)
	Defend,   // hang around the location and fight intruders
	Collect,  // touch the actor / location (take a flag or ball, touch a control point)
	Deliver,  // carry what we hold to the location (capture point, goal)
	Escort,   // stay near a friendly actor (our carrier, a vehicle)
	Use       // reach the location, then call AArenaBotController::RequestUse()
};

/**
 * One candidate goal for a bot, produced by UArenaGameRules::GetBotObjective(s). The bot compares it
 * with item pickups (pickup scores are roughly 0.05..2, Desire * 2000 / (Dist + 1000)) and keeps the best.
 * Priority >= 0.9 is "important": the bot keeps moving towards it while fighting instead of strafing.
 */
struct FArenaBotObjective
{
	FVector Location = FVector::ZeroVector;
	/** Optional actor (flag, ball, control point, carrier ...). Weak: may disappear. */
	TWeakObjectPtr<AActor> Actor;
	float Priority = 0.f;
	EArenaBotAction Action = EArenaBotAction::Move;
	/** Default UpdateBotObjective: Location follows Actor->GetActorLocation() every tick. */
	bool bTrackActor = false;
	/** When the bot reaches Location (100 cm 2D, 150 cm Z) the goal is re-evaluated (false: stay on it). */
	bool bCompleteOnReach = true;
	/** Free tag for the mode (e.g. which kind of objective this is). */
	int32 Tag = 0;

	FArenaBotObjective() = default;
	FArenaBotObjective(const FVector& InLocation, AActor* InActor, float InPriority, EArenaBotAction InAction, int32 InTag = 0)
		: Location(InLocation), Actor(InActor), Priority(InPriority), Action(InAction), Tag(InTag) {}
};

// =============================================================================================
// UArenaGameRules
// =============================================================================================

/**
 * Pluggable game rules. AArenaGameMode creates exactly one instance per match in InitGame from the
 * ?Mode=<ModeId> option (see ArenaGameRulesRegistry) and delegates every mode-specific decision to it.
 *
 * Every hook is virtual and its default implementation is plain Deathmatch / generic team behaviour,
 * so a mode only overrides what it changes. Server hooks only run on the authority (GetGameMode() is
 * valid there). HUD hooks also run on network clients, where the instance is created locally by
 * AArenaGameState from the replicated ModeId and GetGameMode() returns null: HUD hooks must only use
 * replicated state (game state, player states, replicated actors).
 *
 * See Docs/GameModes.md for the full guide.
 */
UCLASS(Abstract)
class OPENUT2K4_API UArenaGameRules : public UObject
{
	GENERATED_BODY()

public:
	// ------------------------------------------------------------------------------------------
	// Identity and defaults (also called on the class default object by the menu)
	// ------------------------------------------------------------------------------------------

	/** The registered id ("DM", "CTF", ...). Set by the registry when the instance is created. */
	const FString& GetModeId() const { return ModeId; }
	/** Display name; defaults to the registered display name. */
	virtual FString GetModeName() const;
	virtual bool IsTeamGame() const { return false; }
	/** Closest legacy enum value (kept for compatibility, e.g. AArenaGameState::GameType). */
	virtual EArenaGameType GetLegacyGameType() const { return IsTeamGame() ? EArenaGameType::TeamDeathmatch : EArenaGameType::Deathmatch; }
	/** Goal used when ?Goal= is missing (frag / team score / capture limit ...). 0 = none. */
	virtual int32 GetDefaultGoalScore() const { return 25; }
	/** Bots used when ?Bots= is missing. */
	virtual int32 GetDefaultNumBots() const { return 5; }
	/** Minutes used when ?Time= is missing. 0 = none. */
	/** UE2 DeathMatch.TimeLimit 20 (minutes). */
	virtual int32 GetDefaultTimeLimit() const { return 20; }
	/** Preferred map when ?Layout= is missing or not supported (falls back to the first map the mode supports). */
	virtual FString GetDefaultLayout() const { return TEXT("Imported:DM-Rankin"); }
	/** Whether this mode can be played on a layout (menu filter + InitGame validation). */
	virtual bool SupportsLayout(const FArenaLayoutInfo& Layout) const;
	/** Menu label of the Goal row, e.g. "Frag Limit", "Capture Limit". */
	virtual FString GetGoalLabel() const { return TEXT("Frag Limit"); }
	/** Menu range / step of the Goal row. */
	virtual void GetGoalRange(int32& OutMin, int32& OutMax, int32& OutStep) const { OutMin = 0; OutMax = 100; OutStep = 5; }
	/** HUD text under the score panel, e.g. "First to 25". Empty = nothing. */
	virtual FString GetGoalText(int32 Goal) const;
	/** Mode used instead when OnLevelBuilt returns false (e.g. CTF on a map without flag bases). */
	virtual FString GetFallbackModeId() const { return IsTeamGame() ? TEXT("TDM") : TEXT("DM"); }

	// ------------------------------------------------------------------------------------------
	// Match lifecycle (server)
	// ------------------------------------------------------------------------------------------

	/** First call after creation (from AArenaGameMode::InitGame, before the level is built). Parse extra options here. */
	virtual void InitRules(AArenaGameMode* InGameMode, const FString& Options);
	/**
	 * The level builder has finished (map geometry and markers are in the world). Spawn mode actors here (flags, balls,
	 * control points ...) using Builder->GetAnchorPoints() / GetImportedJson(). Return false if the
	 * level cannot host this mode: the game mode then switches to GetFallbackModeId().
	 */
	virtual bool OnLevelBuilt(AArenaLevelBuilder* Builder) { return true; }
	/** The game state exists (InitGameState). Fill mode fields of the game state here. */
	virtual void InitGameState(AArenaGameState* GS) {}
	/** Extra points (e.g. objective spots) the bot nav graph must connect. Called right before it is built. */
	virtual void GetNavHints(TArray<FVector>& OutPoints) const {}
	/** End of StartPlay: nav graph built, bots added. */
	virtual void OnStartPlay() {}
	/** Countdown finished, the first round starts. Called once, before OnRoundStart(1). */
	virtual void OnMatchStart() {}
	/** A round enters the Playing phase (round 1 included). */
	virtual void OnRoundStart(int32 Round) {}
	/** AArenaGameMode::EndRound was called; intermission begins. */
	virtual void OnRoundEnd(int32 Round, int32 WinningTeam) {}
	/** Intermission over, everyone is about to respawn for Round: reset objectives here. */
	virtual void OnRoundReset(int32 Round) {}
	/** Before round Round (>= 2) starts: return true to swap team sides (team 0 spawns at side 1 ...). */
	virtual bool ShouldSwapSides(int32 Round) const { return false; }
	/** AArenaGameMode::EndMatch is ending the match. */
	virtual void OnMatchEnd(const FString& WinnerText, int32 WinningTeam, APlayerState* WinningPlayer) {}
	/** Every game mode tick, all phases (check GS->Phase). */
	virtual void Tick(float DeltaSeconds) {}
	/** A human joined (after team assignment) / a controller left. */
	virtual void OnPlayerJoined(AController* Controller) {}
	virtual void OnPlayerLeft(AController* Controller) {}
	/** Team for a new player or bot (PreferredTeam -1 = any). Default: smaller team, preferred on ties. Non-team modes return ArenaTeam::None. */
	virtual int32 ChooseTeam(AArenaPlayerState* PS, int32 PreferredTeam) const;

	// ------------------------------------------------------------------------------------------
	// Spawning and inventory (server)
	// ------------------------------------------------------------------------------------------

	/** Pawn class for a controller; nullptr = AArenaCharacter. */
	virtual UClass* GetPawnClass(AController* Controller) const { return nullptr; }
	/**
	 * Pick a spawn point. Candidates are the level builder's player starts (tags "Red", "Blue", "Any").
	 * May return any actor (its transform is used). Default: PickBestStart with own-side filtering in team games.
	 */
	virtual AActor* ChoosePlayerStart(AController* Player, const TArray<APlayerStart*>& Candidates);
	/** May this controller respawn now/at all? false keeps it dead (spectating). Lives-based modes. */
	virtual bool CanRespawn(AController* Controller) const { return true; }
	/** Seconds after death before the controller may respawn (humans press fire, bots respawn at once). */
	virtual float GetRespawnDelay(AController* Controller) const;
	/** Extra seconds after GetRespawnDelay after which a waiting human is respawned automatically. */
	virtual float GetForcedRespawnDelay(AController* Controller) const;
	/** Called after a pawn was spawned and possessed (default inventory already given). */
	virtual void OnPawnSpawned(AArenaCharacter* Character) {}
	/** Starting weapons. Weapons already holds the defaults (gauntlet + carbine, or the instagib rifle). */
	virtual void ModifyDefaultInventory(AController* Controller, TArray<TSubclassOf<AArenaWeapon>>& Weapons) const {}
	/** false: the level builder spawns no pickups at all. */
	virtual bool AllowPickups() const { return true; }
	/** UE2 DeathMatch.bWeaponStay: weapon pickups stay for players who don't have the weapon (true in every stock mode). */
	virtual bool IsWeaponStay() const { return true; }
	/** UE2 DeathMatch.bDefaultTranslocator: players start with the translocator unless ?Translocator= says otherwise
	 *  (UT2004: CTF and Bombing Run on, everything else off). */
	virtual bool GetDefaultTranslocator() const { return false; }
	/** UE2 DeathMatch.SpawnProtectionTime (2 s; Assault 3 s). */
	virtual float GetSpawnProtectionTime() const { return 2.f; }
	/** UE2 ADR_Kill: adrenaline for a kill (DeathMatch 5; CTF, Bombing Run, Double Domination, Onslaught, Assault 2). */
	virtual float GetKillAdrenaline() const { return 5.f; }
	/** Spawn filter for one pickup (default: instagib keeps adrenaline only). */
	virtual bool AllowPickup(EArenaPickupType Type, TSubclassOf<AArenaWeapon> WeaponClass) const;
	/** Runtime filter: may this character take this pickup now? */
	virtual bool CanPickUp(AArenaCharacter* Character, AArenaPickup* Pickup) const { return true; }

	// ------------------------------------------------------------------------------------------
	// Combat and scoring (server)
	// ------------------------------------------------------------------------------------------

	/** Final damage (default: 0 when not playing / spawn protected / friendly, instagib = 1000). */
	virtual float ModifyDamage(float Damage, AController* Instigator, AArenaCharacter* Victim) const;
	/** Friendly test used for damage, bot targeting and kill scoring. */
	virtual bool AreOnSameTeam(const AController* A, const AController* B) const;
	/**
	 * Score a kill (only called while playing). Return true when handled: the default scoring
	 * (AArenaGameMode::ScoreKillDefault: frags, sprees, multi kills, first blood) is then skipped.
	 */
	virtual bool ScoreKill(AController* Killer, AController* Victim, bool bHeadshot) { return false; }
	/** Default scoring also adds/removes team score per kill (Team Deathmatch). */
	virtual bool KillsAddTeamScore() const { return false; }
	/** After scoring and messages, before the respawn timer is set and end conditions are checked. */
	virtual void OnKilled(AController* Killer, AController* Victim, AArenaCharacter* VictimPawn, const FString& WeaponName, bool bHeadshot) {}
	/** Called after kills / scores / captures: call GameMode->EndMatch(...) (or EndRound) when the mode is won. */
	virtual void CheckEndConditions();
	/** RemainingTime reached 0 (not in overtime). Default: leader wins, tie = overtime. */
	virtual void OnTimeLimitReached();
	/** The player pressed Use (E) or a bot called RequestUse(). Return true if something happened. */
	virtual bool OnPlayerUse(AController* Controller) { return false; }
	/** Where a bot drives one of our vehicles (AArenaONS_Vehicle AI) on a map that is not Onslaught. False: no idea. */
	virtual bool GetVehicleDriveDestination(AController* Driver, const FVector& From, FVector& OutDestination) { return false; }
	/** Something a bot-driven vehicle of Team should shoot when no enemy is in sight (Assault: destroy objectives, turrets). */
	virtual bool GetVehicleSiegeTarget(int32 Team, const FVector& Eye, float Range, AActor*& OutTarget, FVector& OutPoint) { return false; }

	// ------------------------------------------------------------------------------------------
	// Bots (server)
	// ------------------------------------------------------------------------------------------

	/** Simple form: the single best objective for this bot right now. Return false for none. */
	virtual bool GetBotObjective(AArenaBotController* Bot, FArenaBotObjective& OutGoal) { return false; }
	/** All candidate objectives (each competes with pickups). Default: GetBotObjective. Override for several candidates. */
	virtual void GetBotObjectives(AArenaBotController* Bot, TArray<FArenaBotObjective>& OutGoals);
	/**
	 * Called every bot tick while an objective is the bot's goal. Update Objective.Location (moving targets)
	 * and return false when it became pointless (the bot then re-evaluates at once). Default: tracks Actor
	 * when bTrackActor, false when the actor is gone.
	 */
	virtual bool UpdateBotObjective(AArenaBotController* Bot, FArenaBotObjective& Objective);
	/** Enemy selection weight (bot target score is distance * this; < 1 = preferred target, e.g. a carrier). */
	virtual float GetBotEnemyWeight(AArenaBotController* Bot, AArenaCharacter* Candidate) const { return 1.f; }
	/**
	 * Goal priority of hunting a known enemy out of sight at LastSeen (UE2 Bot state Hunting), compared with the
	 * objectives' priorities; 0 = don't hunt, < 0 = the default (0.3 + 0.05 x skill: below any objective).
	 */
	/**
	 * Nav graph: movers that the map's script or objectives open (no player trigger) are passages, not walls. Assault
	 * opens its doors stage by stage; elsewhere such doors may stay shut, so the default is false.
	 */
	virtual bool AreScriptedDoorsPassable() const { return false; }
	virtual float GetBotHuntPriority(AArenaBotController* Bot, AArenaCharacter* Enemy, const FVector& LastSeen) const { return -1.f; }

	// ------------------------------------------------------------------------------------------
	// HUD (clients too; use replicated state only)
	// ------------------------------------------------------------------------------------------

	/** Draw the top-right score panel. Return true when drawn (the default panel is skipped). */
	virtual bool DrawScoreInfo(AArenaHUD* HUD, UCanvas* Canvas) { return false; }
	/** Extra HUD drawing after the standard HUD. Default: AArenaGameState::ModeStatusLines under the score panel. */
	virtual void DrawHUD(AArenaHUD* HUD, UCanvas* Canvas);
	/** Short text after a player's name on the scoreboard (e.g. "3 lives", "OUT"). */
	virtual FString GetScoreboardExtra(const AArenaPlayerState* Player) const { return FString(); }
	/** Extra text for the scoreboard header line (e.g. "Round 2 / 4"). */
	virtual FString GetScoreboardHeaderExtra(const AArenaGameState* GS) const { return FString(); }
	/** Text on the dead screen. Default "Press FIRE to respawn". */
	virtual FString GetDeadScreenMessage(const AArenaPlayerState* Player) const;
	/**
	 * Whether the HUD draws the dead screen (red tint + message) for this player while it has no live character.
	 * Only asked when the default HUD would draw it. Return false e.g. for eliminated players who are spectating.
	 */
	virtual bool ShouldDrawDeadScreen(APlayerController* PC) const { return true; }

	// ------------------------------------------------------------------------------------------
	// Accessors
	// ------------------------------------------------------------------------------------------

	/** Authority only (null on network clients). */
	AArenaGameMode* GetGameMode() const { return GameMode; }
	AArenaGameState* GetGameState() const;
	AArenaLevelBuilder* GetLevelBuilder() const;
	AArenaNavGraph* GetNavGraph() const;
	/** Current goal (AArenaGameState::GoalScore). */
	int32 GetGoalScore() const;
	virtual UWorld* GetWorld() const override;

	/** Default spawn choice: free start far from enemies, optionally only the given side's starts ("Red" / "Blue"). */
	AActor* PickBestStart(AController* Player, const TArray<APlayerStart*>& Candidates, int32 SideFilter) const;
	/** Team of a controller (ArenaTeam::None if unknown). */
	static int32 GetTeamOf(const AController* Controller);

	/** Set by the registry / game state (do not call). */
	void SetModeId(const FString& InModeId) { ModeId = InModeId; }

protected:
	UPROPERTY() TObjectPtr<AArenaGameMode> GameMode;
	FString ModeId;
};

// =============================================================================================
// Registry
// =============================================================================================

/** One registered mode. */
struct OPENUT2K4_API FArenaRulesEntry
{
	const TCHAR* ModeId = nullptr;
	const TCHAR* DisplayName = nullptr;
	/** Returns the rules class (resolved lazily: StaticClass() must not run during static init). */
	UClass* (*GetClassFn)() = nullptr;
	/** Optional layout filter; null = the class default object's SupportsLayout(). */
	bool (*SupportsLayoutFn)(const FArenaLayoutInfo&) = nullptr;
	/** Menu order (lower first). */
	int32 Order = 1000;

	UClass* GetRulesClass() const { return GetClassFn ? GetClassFn() : nullptr; }
	/** Class default object, for metadata queries (IsTeamGame, GetDefaultGoalScore ...). */
	const UArenaGameRules* GetDefaults() const;
	bool SupportsLayout(const FArenaLayoutInfo& Layout) const;
};

/**
 * Static registry of game rules classes. Modes register themselves at static-init time with
 * ARENA_REGISTER_RULES in their .cpp, so adding a mode never touches shared files.
 * Storage is a function-local static (safe with unity builds and any static-init order).
 */
namespace ArenaGameRulesRegistry
{
	OPENUT2K4_API void Register(const TCHAR* ModeId, const TCHAR* DisplayName, UClass* (*GetClassFn)(), bool (*SupportsLayoutFn)(const FArenaLayoutInfo&) = nullptr);
	/** All modes in menu order: DM, TDM, CTF, BR, DOM, LMS, MUT, INV, AS, ONS, then others alphabetically. */
	OPENUT2K4_API TArray<const FArenaRulesEntry*> GetModes();
	/** Case-insensitive lookup; null if unknown. */
	OPENUT2K4_API const FArenaRulesEntry* Find(const FString& ModeId);
	/** Display name of a mode id ("" if unknown). */
	OPENUT2K4_API FString GetDisplayName(const FString& ModeId);
	OPENUT2K4_API bool SupportsLayout(const FString& ModeId, const FArenaLayoutInfo& Layout);
	/** New rules instance for ModeId (unknown ids fall back to "DM"). Outer is the game mode or game state. */
	OPENUT2K4_API UArenaGameRules* CreateRules(UObject* Outer, const FString& ModeId);

	struct FAutoRegister
	{
		FAutoRegister(const TCHAR* ModeId, const TCHAR* DisplayName, UClass* (*GetClassFn)(), bool (*SupportsLayoutFn)(const FArenaLayoutInfo&) = nullptr)
		{
			Register(ModeId, DisplayName, GetClassFn, SupportsLayoutFn);
		}
	};
}

/**
 * Register a rules class. Put exactly one in the mode's .cpp (file scope):
 *   ARENA_REGISTER_RULES(UArenaRules_BR, "BR", "Bombing Run");
 * ModeId is what ?Mode= takes (case-insensitive). The class's SupportsLayout() filters layouts.
 */
#define ARENA_REGISTER_RULES(RulesClass, ModeIdString, DisplayNameString) \
	static ArenaGameRulesRegistry::FAutoRegister GArenaRulesAutoRegister_##RulesClass(TEXT(ModeIdString), TEXT(DisplayNameString), &RulesClass::StaticClass)

/** Same with an explicit layout filter function bool(const FArenaLayoutInfo&). */
#define ARENA_REGISTER_RULES_WITH_LAYOUTS(RulesClass, ModeIdString, DisplayNameString, SupportsLayoutFunction) \
	static ArenaGameRulesRegistry::FAutoRegister GArenaRulesAutoRegister_##RulesClass(TEXT(ModeIdString), TEXT(DisplayNameString), &RulesClass::StaticClass, SupportsLayoutFunction)
