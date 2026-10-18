#pragma once

#include "CoreMinimal.h"

struct FArenaServerConfig;

/**
 * OPENUT2K4_SERVER_BINARY is 1 when OpenUT2k4Server is built as a Server target (source engine). As a Game target
 * (installed engine) the module is compiled like the game's and the executable's name selects the server mode.
 */
#ifndef OPENUT2K4_SERVER_BINARY
#define OPENUT2K4_SERVER_BINARY 0
#endif

/**
 * The OpenUT2k4 dedicated server (Server/Docs/DedicatedServer.md): the game running headless, without a viewport, window,
 * audio or local player, hosting one match after another with our UE2 server (Net/UE2Server.h) for UT2004 and
 * OpenUT2k4 clients.
 *
 * Started by the OpenUT2k4Server binary, or by the game / editor binary with -OpenUT2k4Server. Configured from
 * UT2004 style ini files and a UCC style command line (Server/ArenaServerConfig.h):
 *
 *   OpenUT2k4Server [server] [DM-Rankin?game=XGame.xDeathMatch?MaxPlayers=8?MinPlayers=4] [ini=<file>] [log=<file>]
 *       -nullrhi -nosound [-ServerConfig=<file>] [-ServerProfile=<dir>] [-ServerStateDir=<dir>] [-port=<n>]
 *       [-multihome=<ip>] [-lanplay] [-ContentCache=<dir>] [-UT2004Dir=<dir>] [-Service[=<name>]] [-NoServerConsole]
 *
 * Admin: commands on stdin and, when enabled, a password protected TCP console ([OpenUT2k4.AdminConsole]):
 * help, status, say, kick, ban, banid, unban, bans, map, nextmap, restartmap, maplist, minplayers, addbot, removebot,
 * quit. Shutdown on SIGTERM / SIGINT (the engine's handler), the Windows service control manager, "quit", or launchd:
 * clients are told the server is going down, the log is flushed.
 */
namespace ArenaDedicatedServer
{
	/** True when this process is a dedicated server (server binary, or -OpenUT2k4Server). */
	OPENUT2K4_API bool IsServerMode();

	/** Called from the game module's StartupModule: reads the configuration and switches the engine to headless. */
	void OnModuleStartup();
	/** Called from the game module's ShutdownModule. */
	void OnModuleShutdown();

	/**
	 * The first match's URL (UArenaServerGameInstance::StartGameInstance). False with OutError when the server cannot
	 * run (no content cache, no maps, bad configuration).
	 */
	bool PrepareFirstMatch(FString& OutUrl, FString& OutError);

	/** The loaded configuration (valid in server mode). */
	OPENUT2K4_API const FArenaServerConfig& GetConfig();

	/** Runs one admin command line ("kick 3", "map DM-Antalus" ...) and returns its reply (several lines). */
	OPENUT2K4_API FString ExecCommand(const FString& Line);

	/** Queues a command from another thread (stdin, admin console); run on the game thread by the server tick. */
	void QueueCommand(const FString& Line, TFunction<void(const FString& Reply)> OnReply);

	/** Exit codes (Server/Package service files: 2 and 3 are not restarted). */
	constexpr int32 ExitConfigError = 2;
	constexpr int32 ExitNoContent = 3;
	constexpr int32 ExitMatchFailed = 4;
}

/** Windows service support (Server/ArenaServerService.cpp; no-ops elsewhere). */
namespace ArenaServerService
{
	/** -Service[=<name>]: connects to the service control manager from a helper thread. */
	void Start();
	/** The engine is shutting down: report SERVICE_STOPPED with the exit code. */
	void Stop(int32 ExitCode);
	bool IsRunningAsService();
}
