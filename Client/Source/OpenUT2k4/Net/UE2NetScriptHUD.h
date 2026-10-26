#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class AHUD;
class UCanvas;
class UWorld;
class FUE2NetConnection;
struct FUE2NetActor;
struct FUE2NetValue;
struct FUSFunction;

/**
 * A modded UT2004 server's own HUD and scoreboard, drawn like the real client draws them: by the mod's client-side
 * UnrealScript, run by the script VM (Docs/UE2Net.md "Mod HUDs and scoreboards").
 *
 *  - Mirror: every replicated actor whose class the script runtime has (stock or downloaded) gets a script object of
 *    that class in a client-side script world, with the class defaults and the received properties on top (Role and
 *    RemoteRole swapped, object references resolved to the mirrored actors or to placeholders of textures, fonts and
 *    classes). The Level (NM_Client, TimeSeconds, GRI, GetLocalPlayerController), GRI.PRIArray, the PlayerController's
 *    GameReplicationInfo, Player (viewport) and ViewTarget are kept like UE2's client keeps them. Only data: mirrored
 *    actors never run their own code, except the local PlayerController's PostBeginPlay (the mods' client settings
 *    objects).
 *  - The HUD: PlayerController.ClientSetHUD from the server is run on the mirrored controller, which spawns the HUD
 *    class and its scoreboard in the script world (their BeginPlay events, timers and Tick run there). Each frame the
 *    HUD's PostRender draws on a script Canvas through the Canvas natives (UScript/UScriptCanvas.h), whose backend is
 *    our UCanvas with UT2004's fonts and the content cache's textures. Textures, fonts and materials only mod packages
 *    have are converted on demand (UE2NetModContent).
 *  - Fallback: stock HUD classes stay drawn by our native HUD (ArenaHUDUT); a mod HUD or scoreboard that fails (script
 *    error, nothing drawn, too slow) falls back to ours for that element. Messages, the kill feed, chat and our
 *    notices are always ours. Anti-cheat packages are never mirrored nor run.
 *
 * Test hooks: -UE2ScriptHUD=0 (off) / all (stock HUD classes too); -UE2ScriptHUDLog (per-frame statistics every 5 s).
 */
class OPENUT2K4_API FUE2NetScriptHUD
{
public:
	FUE2NetScriptHUD(FUE2NetConnection& InConnection, UWorld* InWorld);
	~FUE2NetScriptHUD();

	void ActorOpened(const FUE2NetActor& Actor);
	void ActorUpdated(const FUE2NetActor& Actor, const TArray<FString>& Changed);
	void ActorClosed(const FUE2NetActor& Actor);
	void FunctionCall(const FUE2NetActor& Actor, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params);
	void Tick(float DeltaSeconds);

	struct FDrawResult
	{
		/** The server's HUD drew this frame (our player HUD, crosshair, spectator bar and score pass are left out). */
		bool bHud = false;
		/** The server's scoreboard drew this frame (ours is left out). */
		bool bScoreboard = false;
		/** The server's HUD shows the server's messages (its message lists are fed: FeedMessage). */
		bool bMessages = false;
	};
	/** Draws the script HUD / scoreboard on the HUD's canvas (AArenaHUD::DrawHUD). */
	FDrawResult Draw(AHUD* Hud, UCanvas* Canvas, bool bShowScores);

	FString Describe() const;
	/**
	 * A message the server sends our controller (ReceiveLocalizedMessage, ClientMessage, TeamMessage), given to the
	 * server's script HUD the way UE2's PlayerController does (LocalMessage.ClientReceive -> HUD.LocalizedMessage,
	 * HUD.Message): true when that HUD draws it (ours then only plays its sounds), false when ours shows it.
	 */
	bool FeedMessage(int32 Channel, const FString& Function, const TArray<TPair<FName, FUE2NetValue>>& Params);
	// ---- the local player's mod weapons run as script (Net/UE2NetScriptWeapon.h) ----
	enum class EWeaponCommand : uint8 { BringUp, PutDown, StartFire, StopFire };
	struct FScriptWeaponState
	{
		/** Weapon.AmmoAmount(0) / MaxAmmo(0). */
		int32 Ammo = 0;
		int32 MaxAmmo = 0;
		/** What the script last asked the weapon to play (Actor.PlayAnim / LoopAnim / StopAnimating). */
		FName Anim;
		float AnimRate = 1.f;
		bool bAnimLoop = false;
		bool bAnimStopped = false;
		uint32 AnimSerial = 0;
	};
	/** Test hook (development builds, -UE2ScriptVoting): xVoting's VotingReplicationInfo runs its client code in the
	 *  mirror too (its calls to the server dropped), so its script pages open with the server's data. */
	static bool IsScriptVotingTest();
	/** The weapon of Channel (ours) runs its own client code in the mirror. */
	bool IsScriptWeapon(int32 Channel) const;
	/**
	 * Switch our pawn to the weapon of WeaponChannel the way UE2's client does when the weapon runs its own code
	 * (Pawn.PendingWeapon, Weapon.PutDown, then Pawn.ChangedWeapon sends ServerChangedWeapon). False when it does not
	 * run its code here: the caller switches (and tells the server) itself.
	 */
	bool ScriptSwitchWeapon(int32 PawnChannel, int32 WeaponChannel);
	/** PlayerController's own ping on the client (PRI.Ping, a quarter of the ms; the server replicates it to the others
	 *  only): our PRI's mirror gets it, for the mods' scoreboards. */
	void SetOwnPing(uint8 Ping);
	/** Our fire pressed / released in the vehicle of VehicleChannel: its mod weapons' client code (ClientStartFire / ClientStopFire). */
	void VehicleFire(int32 VehicleChannel, bool bStart, bool bAlt);
	/** The weapon our pawn's script holds or is switching to, when that weapon runs its own code (0 otherwise). */
	int32 ScriptPawnWeapon(int32 PawnChannel) const;
	/** Weapon.BringUp / PutDown, ClientStartFire / ClientStopFire(Mode) of the script weapon. */
	void ScriptWeaponCommand(int32 Channel, EWeaponCommand Command, uint8 Mode);
	bool GetScriptWeaponState(int32 Channel, FScriptWeaponState& Out) const;
	/**
	 * The ammo a mod weapon's own code reports for the HUD (Weapon.GetAmmoCount, GetHUDAmmoText overridden by the mod:
	 * Ballistic Weapons' magazine and reserve). False when the weapon's class does not override them.
	 */
	bool GetModWeaponAmmo(int32 Channel, int32& OutCur, int32& OutMax, FString& OutText) const;
	/** The weapon's animation ended: its AnimEnd(0). */
	void ScriptWeaponAnimEnd(int32 Channel);
	/**
	 * A mod weapon our stand-in plays (it has a stock relative): its fire modes' client code runs alongside ours so the fire
	 * timing follows it (ModeTick each frame with bIsFiring as held; ModeDoFire for each shot ours plays). The current
	 * FireRate of mode Mode afterwards, or a negative value.
	 */
	void TickStandInWeapon(int32 Channel, bool bHeld0, bool bHeld1, float DeltaSeconds);
	void StandInFired(int32 Channel, uint8 Mode);
	float GetScriptFireRate(int32 Channel, uint8 Mode) const;
	/** A console command for the mod's code: an exec function of our controller, pawn or weapon in the mirror (BW's
	 *  reload / weapon special binds ...). True when one ran. bModOnly: only exec functions a mod's class declares
	 *  (console commands we do not know: UTComp's "mymenu"). */
	bool ConsoleExec(const FString& Command, bool bModOnly = false);

	// ---- mods' menus: their GUI2K4 / XInterface pages run as script (UScript/UScriptGUI.h) ----
	/** A mod menu is open (it takes the mouse and keys: Net/UE2NetModMenus). */
	bool IsMenuOpen() const;
	/** Class of the top mod menu ("" when none). */
	FString GetMenuName() const;
	/** Draws the open mod menus over everything (AArenaHUD::DrawHUD). True when one is open. */
	bool DrawMenus(AHUD* Hud, UCanvas* Canvas);
	/** UE2 EInputKey / EInputAction into the menus; a typed character; the mouse in canvas pixels. */
	bool MenuKey(uint8 Key, uint8 Action, float Delta = 0.f);
	bool MenuType(uint8 Key, const FString& Unicode);
	void MenuMouse(float X, float Y);
	void CloseMenus();
	/** GUIController.OpenMenu (test hook / console). */
	bool OpenMenu(const FString& MenuClass);
	/** Test hooks: the top menu's component tree; a click on one of its components (object name or caption). */
	FString DumpMenus() const;
	/** Debugging: a property of our controller's mirror as text ("Obj.Prop" follows one object reference). */
	FString DescribePCProperty(const FString& Path) const;
	bool ClickMenu(const FString& NameOrCaption);
	/** The console command a mod bound to a key ("F5" -> "MyMenu"), or "". */
	FString GetModBind(const FString& KeyName) const;
	/** Binds a key to a mod's console command (kept in our per-user ini; empty Command removes it). */
	void SetModBind(const FString& KeyName, const FString& Command);

	/** The mirrored actor of Channel runs client code that shows an attachment of that class ("Pkg.Class"). */
	bool HasClientAttachment(int32 Channel, const FString& ClassPath) const;

	/** The instance of the current connection (game thread), or null. */
	static FUE2NetScriptHUD* Get();

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
