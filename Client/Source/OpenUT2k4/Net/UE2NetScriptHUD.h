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
	/** The mirrored actor of Channel runs client code that shows an attachment of that class ("Pkg.Class"). */
	bool HasClientAttachment(int32 Channel, const FString& ClassPath) const;

	/** The instance of the current connection (game thread), or null. */
	static FUE2NetScriptHUD* Get();

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
