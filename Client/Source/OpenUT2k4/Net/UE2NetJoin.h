#pragma once

#include "CoreMinimal.h"

class UWorld;
class FUE2NetMapVote;

/**
 * Joining stock UT2004 (UE2, build 3369/3374) servers from OpenUT2k4. See Docs/UE2Net.md.
 *
 * The UE2 protocol is implemented as a standalone UDP client (FUE2NetConnection in UE2NetConnection.h), not through
 * UE5's net driver.
 */
namespace ArenaNet
{
	/**
	 * Starts joining a UT2004 server. Address is "host:port" (port defaults to 7777) or "host".
	 * Returns false when the address cannot be parsed or a connection could not be started. Game thread only.
	 */
	OPENUT2K4_API bool JoinServer(const FString& Address);

	/** Closes the current UE2 server connection, if any (sends a clean close). */
	OPENUT2K4_API void LeaveServer();

	/** True while a UE2 server connection exists (connecting or connected). */
	OPENUT2K4_API bool IsConnectedToServer();

	/**
	 * Text for the HUD's spectator bar while the world shows a UT2004 server's map (until the game state drives the
	 * world): "Connected to <server> - receiving game state ...". Empty when World is not such a map.
	 */
	OPENUT2K4_API FString GetSpectatorBarText(const UWorld* World);
	/**
	 * The name of an anti-cheat that never lets OpenUT2k4 play when Text (a package name, a server rule key or value)
	 * names it ("AntiTCC"), else empty. Such a server holds new players until the anti-cheat's client check answers,
	 * and OpenUT2k4 never runs, answers or imitates anti-cheat: on those servers we can only spectate.
	 */
	OPENUT2K4_API FString FindSpawnBlockingAntiCheat(const FString& Text);
	/** A server mod's status line for the playing HUD (UTComp's warmup: "Warmup: 2 of 4 ready ..."), else empty. */
	OPENUT2K4_API FString GetModStatusText(const UWorld* World);

	/**
	 * While joining needs package downloads or their conversion (before the map loads): true, with the status text
	 * ("Downloading R3tex.utx (2 of 3): ...") and the fraction done (0..1). False otherwise.
	 */
	OPENUT2K4_API bool GetJoinProgress(FString& OutText, float& OutFraction);

	/** Human readable connection status ("Connecting to ...", "Connected to ..."); when idle, why the last connection failed (or empty). */
	OPENUT2K4_API FString GetConnectionStatus();

	/** The server's FAILCODE of the last rejected join ("NEEDSTATS", "LOCALBAN", "NEEDPW", ...), empty otherwise. */
	OPENUT2K4_API FString GetLastFailCode();

	/** True when the last join was rejected because the server requires stats (NEEDSTATS): show GetStatsPrompt(). */
	OPENUT2K4_API bool NeedsStatsPrompt();

	/** UT2004's UT2K4StatsPrompt texts (from GUI2K4.int). For the NEEDSTATS dialog: YES opens the stats settings and retries. */
	OPENUT2K4_API bool GetStatsPrompt(FString& OutHeader, FString& OutDescription, FString& OutYes, FString& OutNo);

	/**
	 * While the server changes maps (from its ClientTravel until the next map is up): true, with a title ("Loading
	 * DM-Rankin"), what happens now (reconnecting, downloading or converting packages, or why the next map cannot be
	 * shown) and the fraction done (0..1, or -1 without a progress bar). The HUD covers the old map with it.
	 */
	OPENUT2K4_API bool GetMapChangeStatus(const UWorld* World, FString& OutTitle, FString& OutDetail, float& OutFraction);
	/** The server's map voting (xVoting) when it runs one, else null. Game thread only; valid until the next tick. */
	OPENUT2K4_API const FUE2NetMapVote* GetMapVote();
	/** Votes for a map (index into GetMapVote()->GetMaps()) and game type (GetGameConfigs()). */
	OPENUT2K4_API bool SendMapVote(int32 MapIndex, int32 GameIndex);
	/** True once when the server asked to open the map voting window (end of the match). */
	OPENUT2K4_API bool ConsumeMapVoteOpenRequest();
	/** True once when the server closed map voting (a map was chosen). */
	OPENUT2K4_API bool ConsumeMapVoteCloseRequest();
	/** Re-joins the last server (same URL and options). Used after the player set a password or stats and chose retry. */
	OPENUT2K4_API bool RetryLastJoin();
}
