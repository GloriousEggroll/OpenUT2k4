#pragma once

#include "CoreMinimal.h"

/**
 * A UT2004 style ini file: [Section] headers, Key=Value lines, repeated keys for lists (also "Key[n]=Value"),
 * ';' and '#' comments. Section and key names are case-insensitive. Values keep their text (quotes stripped).
 * Read only; the dedicated server never rewrites a user's ini (runtime state such as bans lives in its state dir).
 */
class OPENUT2K4_API FArenaIniFile
{
public:
	bool Load(const FString& Path);
	bool LoadFromString(const FString& Text);
	bool IsLoaded() const { return bLoaded; }
	const FString& GetPath() const { return Path; }

	bool HasSection(const FString& Section) const;
	/** Last value of Key in Section (a later line overrides an earlier one). */
	bool Get(const FString& Section, const FString& Key, FString& Out) const;
	/** Every value of Key in Section, in file order ("Key=" and "Key[n]=" lines). */
	TArray<FString> GetAll(const FString& Section, const FString& Key) const;

	static bool ParseBool(const FString& Value, bool bDefault);

private:
	struct FEntry
	{
		FString Key;
		FString Value;
	};
	/** Lower case section name -> its lines in file order. */
	TMap<FString, TArray<FEntry>> Sections;
	FString Path;
	bool bLoaded = false;
};

/**
 * Everything the dedicated server is configured with (Server/Docs/DedicatedServer.md "Configuration").
 *
 * Layers, later ones win:
 *   1. built-in defaults: the stock UT2004 dedicated server's default values (port 7777, 16 players, 2 spectators,
 *      time limit 20, the stock goal scores per game type, uplink off: OpenUT2k4 keeps it off unless asked)
 *   2. the shipped default ini (<package>/Server/OpenUT2k4Server.ini, or Server/Package/OpenUT2k4Server.ini in a
 *      source tree)
 *   3. the server ini: -ServerConfig=<file> or UCC's ini=<file> (a UT2004.ini works too: same section names)
 *   4. a profile directory, -ServerProfile=<dir>: <dir>/Server.ini, or <dir>/UT2004.ini when it has no Server.ini
 *   5. the command line: the UCC style URL (DM-Rankin?game=XGame.xDeathMatch?MaxPlayers=8...) and switches
 *      (-port=, -multihome=, -lanplay)
 *
 * The map rotation, when no layer gives one, is the UT2004 install's own stock map list for the game type
 * ([XInterface.MapListDeathMatch] ... in its System/UT2004.ini, else Default.ini), read at runtime.
 */
struct OPENUT2K4_API FArenaServerConfig
{
	// ---- [OpenUT2k4.Server] ----
	/** UE2 game class ("XGame.xDeathMatch") as given; ModeId is ours ("DM"). */
	FString GameClass = TEXT("XGame.xDeathMatch");
	FString ModeId = TEXT("DM");
	/** First map; empty: the first of the rotation. */
	FString Map;
	/** Map rotation (map names, no extension). */
	TArray<FString> Maps;
	/** Where the rotation came from (for the log). */
	FString MapsSource;
	/** "Package.Class" of each mutator (UT2004 ?Mutator= syntax). */
	TArray<FString> Mutators;
	/** UT2004 MinPlayers: bots fill the match up to this many players while humans are connected. */
	int32 MinPlayers = 0;
	/** Keep MinPlayers bots playing on an empty server (UT2004 removes them: off). */
	bool bBotsWhenEmpty = false;
	/** Bot skill 0..7 (UT2004 GameDifficulty). */
	int32 BotSkill = 3;
	/** Raw URL options appended last ("?Translocator=1"). */
	FString ExtraOptions;
	/** Engine ticks per second ([IpDrv.TcpNetDriver] NetServerMaxTickRate / LanServerMaxTickRate). */
	int32 TickRate = 30;

	// ---- [URL] / [IpDrv.MasterServerUplink] / [Engine.GameInfo] / [Engine.AccessControl] ----
	int32 Port = 7777;
	/** -multihome=<ip>: bind the game port to one address. */
	FString BindAddress;
	bool bLANServer = false;
	/** Register with the OldUnreal master servers (DoUplink). Off by default. */
	bool bUplink = false;
	int32 MaxPlayers = 16;
	int32 MaxSpectators = 2;
	/** -1: the game type's own default. */
	int32 TimeLimit = -1;
	int32 GoalScore = -1;
	FString GamePassword;
	FString AdminPassword;
	/** "id|name" entries from the ini ([Engine.AccessControl] BannedIDs=<id> <name>). */
	TArray<FString> BannedIDs;

	// ---- [Engine.GameReplicationInfo] ----
	FString ServerName = TEXT("OpenUT2k4 Server");
	FString ShortName = TEXT("OpenUT2k4");
	FString AdminName;
	FString AdminEmail;
	/** MessageOfTheDay or MOTDLine1..4. */
	TArray<FString> MOTD;

	// ---- [Engine.GameEngine] ----
	TArray<FString> ServerActors;
	TArray<FString> ServerPackages;
	/**
	 * [OpenUT2k4.Server] UseServerPackages=True: the ServerPackages above replace the UT2004 install's list in the clients'
	 * package list (USES). Off by default: a UT2004.ini's list often names mods the clients would need to download.
	 */
	bool bUseServerPackages = false;

	// ---- [OpenUT2k4.AdminConsole]: line based TCP admin console ----
	bool bAdminConsole = false;
	FString AdminConsoleAddress = TEXT("127.0.0.1");
	int32 AdminConsolePort = 7780;

	// ---- [OpenUT2k4.Paths] ----
	FString ContentCache;
	FString UT2004Dir;
	FString UserContent;

	/** Files that were read, in order (for the log and "status"). */
	TArray<FString> LoadedFiles;
	/** Problems found while loading (unknown game type, unsupported server actors ...). */
	TArray<FString> Warnings;

	/**
	 * Builds the configuration from the layers above. CommandLine is the process command line. False (with OutError)
	 * when a file that was asked for explicitly cannot be read.
	 */
	bool Load(const TCHAR* CommandLine, FString& OutError);

	/** Applies one ini file (any layer). */
	void ApplyIni(const FArenaIniFile& Ini);
	/** Applies UCC style URL options ("DM-Rankin?game=XGame.xDeathMatch?MaxPlayers=8"). */
	void ApplyURL(const FString& Url);

	/** Fills Maps from the install's stock map list when no layer gave one; keeps only maps the content cache has. */
	void ResolveMaps();

	/** The level and options our game opens: "/Game/Maps/Arena?Mode=DM?Layout=Imported:DM-Rankin?...". */
	FString BuildMatchURL(const FString& MapName) const;

	/** UE2 game class (short or full) -> our mode id; empty when unknown. */
	static FString ModeIdFromGameClass(const FString& GameClass);
	/** Our mode id -> the stock game class ("XGame.xDeathMatch"). */
	static FString GameClassFromModeId(const FString& ModeId);
	/** The stock map list section of a game class in UT2004.ini / Default.ini. */
	static FString StockMapListSection(const FString& ModeId);
	/** Option-encodes a value (% ? = # & space quotes slashes as %XX), as ArenaHost::BuildServerOptions does. */
	static FString EncodeOption(const FString& In);

	/** The shipped default ini (empty when not found). */
	static FString FindDefaultIni();
	/** A "?Key=Value" option of a URL, decoded; empty when missing. */
	static FString UrlOption(const FString& Url, const FString& Key);

private:
	/** The game section of the configured game class ("XGame.xDeathMatch"), for per-game TimeLimit / GoalScore. */
	void ApplyGameSection(const FArenaIniFile& Ini);
	/** The ini layers in the order they were read. */
	TArray<FArenaIniFile> Layers;
};
