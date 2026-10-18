#pragma once

#include "CoreMinimal.h"
#include "Templates/SubclassOf.h"

class AArenaGameMode;
class AArenaCharacter;
class AArenaWeapon;
class AController;
class UWorld;
class AActor;
class APawn;
enum class EArenaPickupType : uint8;

/**
 * Public API of the UnrealScript runtime (UT2004 bytecode VM, see Docs/UnrealScript.md).
 * Script packages are loaded at runtime from the player's UT2004 install and UserContent; nothing is shipped.
 * Everything here is game-thread only and safe to call when the VM is compiled out (it then reports "not available").
 */
namespace ArenaUScript
{
	/** True when the VM is compiled into this build. */
	OPENUT2K4_API bool IsAvailable();

	/** Initialises the runtime from the UT2004 install (and registers UserContent packages). */
	OPENUT2K4_API bool EnsureRuntime(FString* OutError = nullptr);

	/**
	 * Whether a script class ("Package.Class") or every script class of a package ("Package") can run in our game.
	 * OutReason explains why not (missing package/superclass, unsupported kind of class, unimplemented natives that the
	 * class's gameplay code can reach: only code reachable from the functions the game calls counts, so natives used by
	 * config / menu code such as FillPlayInfo or GetDescriptionText do not matter).
	 */
	OPENUT2K4_API bool CanRun(const FString& PackageOrClass, FString* OutReason = nullptr);

	/** Empty when CanRun() is true. */
	OPENUT2K4_API FString GetUnsupportedReason(const FString& PackageOrClass);

	/** -UScriptCanRunReport: logs CanRun() for every mutator class of the cache's script packages ("UScriptCanRun: ..."). */
	OPENUT2K4_API void LogMutatorSupport();

	/**
	 * Unimplemented natives a script class can reach from the given entry points (function names called on it, e.g. a
	 * HUD's PostRender), as "Class.Function (from Pkg.Class.Function)". Same static analysis as CanRun, other roots.
	 */
	OPENUT2K4_API TArray<FString> FindReachableMissingNatives(const FString& ClassPath, const TArray<FString>& EntryPoints);

	/** Runs the VM self tests (stock Core/Engine/XGame functions with known results). */
	OPENUT2K4_API void RunSelfTests(int32& OutPassed, int32& OutFailed);
}

/**
 * Game hooks of the script runtime: UT2004 mutators and game rules run at the same points as in UT2004.
 * All are no-ops (zero cost) unless the match uses script (?Mutator=Package.Class[,Package.Class]).
 */
namespace ArenaUScriptHooks
{
	/** AArenaGameMode::InitGame, before the level is built: creates the script world and adds ?Mutator= mutators. */
	OPENUT2K4_API void InitGame(AArenaGameMode* GameMode, const FString& Options);
	/** After the level was built: physics volumes pass Mutator.CheckRelevance (e.g. low gravity). */
	OPENUT2K4_API void LevelBuilt(AArenaGameMode* GameMode);
	/** Level builder pickup spawn: CheckRelevance on the UT2004 pickup class; false drops it, a ReplaceWith changes Type/WeaponClass. */
	OPENUT2K4_API bool FilterPickup(UWorld* World, const FVector& Location, EArenaPickupType& Type, TSubclassOf<AArenaWeapon>& WeaponClass);
	/** Starting inventory: UT2004 xPawn.RequiredEquipment / GameInfo.DefaultWeaponName through CheckRelevance. */
	OPENUT2K4_API void ModifyDefaultInventory(AController* Controller, TArray<TSubclassOf<AArenaWeapon>>& Weapons);
	/** After a pawn spawned: CheckRelevance + Mutator.ModifyPlayer. */
	OPENUT2K4_API void PawnSpawned(AArenaCharacter* Pawn);
	/** Damage: GameRules.NetDamage chain. Returns the new damage. */
	OPENUT2K4_API float ModifyDamage(float Damage, AController* Instigator, AArenaCharacter* Victim);
	/** A kill was scored: GameRules.ScoreKill and Mutator.ScoreKill. */
	OPENUT2K4_API void Killed(AController* Killer, AController* Victim, AArenaCharacter* VictimPawn);
	/** Events of a map script actor's game body: 0 Trigger, 1 UnTrigger, 2 Touch, 3 UnTouch. */
	OPENUT2K4_API void ScriptActorEvent(AActor* Body, AActor* Other, APawn* EventInstigator, int32 Kind);
	/** Weapon damage on a map script actor's game body (UE2 TakeDamage). */
	OPENUT2K4_API void ScriptActorDamage(AActor* Body, float Damage, AController* EventInstigator, AActor* DamageCauser);
	/** True when the world runs script. */
	OPENUT2K4_API bool IsActive(const UWorld* World);
}
