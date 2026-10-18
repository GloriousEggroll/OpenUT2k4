#pragma once

#include "CoreMinimal.h"

class UWorld;

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

	/** Re-joins the last server (same URL and options). Used after the player set a password or stats and chose retry. */
	OPENUT2K4_API bool RetryLastJoin();
}
