#pragma once

#include "CoreMinimal.h"

class FArenaIniFile;
class FUE2ServerConnection;

/**
 * The OpenUT2k4 Validator built into our own UE2 server (Server/Docs/Validator.md "OpenUT2k4 dedicated server"): the
 * same checks, policies, ini keys ([OpenUT2k4Validator.OUTValidator]) and state file format as the UnrealScript
 * mutator for stock servers (Server/Validator), without needing the mutator. Off by default; the dedicated server turns
 * it on when its ini installs the mutator (ServerActors= or Mutators= OpenUT2k4Validator.OUTValidator).
 *
 * Differences to the mutator: the challenge goes over the control channel ("OUTVALIDATE CHALLENGE=..." after JOIN,
 * answered with "OUTVALIDATE PART= TOTAL= DATA=" lines), only to clients whose login URL has ?OpenUT2k4=; signed shared
 * ban lists are fetched (files, http and https) and checked here; "Mark" results show in the admin console only (no
 * replicated info actor); probation "Limited" acts as "Flag" (no voting on our server); no sanity checks yet.
 */
struct OPENUT2K4_API FUE2ValidatorSettings
{
	bool bEnabled = false;
	FString IdentifiedPolicy = TEXT("Mark");
	FString FailedPolicy = TEXT("Mark");
	FString NoResponsePolicy = TEXT("Mark");
	float ResponseTimeout = 30.f;
	bool bWhitelistOnly = false;
	int32 ProbationHours = 0;
	FString ProbationPolicy = TEXT("Flag");
	int32 MaxProbationPlayers = 0;
	FString BanPolicy = TEXT("Kick");
	bool bMatchBanIP = true;
	bool bMatchBanCIDR = false;
	bool bMatchBanLegacyId = true;
	bool bApplyBansToAllClients = true;
	/** Native only: an Identified player's fingerprint becomes the player id that kick / ban / the ban list use. */
	bool bUseKeyAsPlayerId = false;
	/** "<64 hex Ed25519 public key> <name>": issuers whose signed ban lists are accepted. */
	TArray<FString> TrustedBanIssuers;
	/** Signed ban lists to import: files or http(s) URLs. */
	TArray<FString> BanListSources;
	int32 BanListRefreshMinutes = 60;
	/**
	 * This server's addresses as players dial them (IP or host name, repeatable). The address an answer was signed
	 * for must be one of them, the bind / multihome IP, a local interface or loopback. Unset and none of those (NAT):
	 * accepted when the port matches, marked "address unverified".
	 */
	TArray<FString> PublicAddresses;

	/** Reads [OpenUT2k4Validator.OUTValidator] (keys the ini does not have keep their value). */
	void ApplyIni(const FArenaIniFile& Ini);
};

namespace UE2ServerValidator
{
	/** "OpenUT2k4Validator.OUTValidator": installing this mutator / server actor turns the native validator on. */
	OPENUT2K4_API bool IsValidatorClass(const FString& ClassPath);
	/** Settings and the state folder (ValidatorKeys.ini, ValidatorShared.ini). Loads the state. */
	OPENUT2K4_API void Configure(const FUE2ValidatorSettings& Settings, const FString& StateDir);
	OPENUT2K4_API bool IsEnabled();

	// ---- hooks of the UE2 server ----
	/** At LOGIN: bans by IP / subnet / UT2004 id for every client (LOCALBAN). */
	void CheckLogin(FUE2ServerConnection& Conn, const FString& Url, FString& OutFailCode, FString& OutFailText);
	/** After JOIN: the challenge for OpenUT2k4 clients. */
	void PlayerJoined(FUE2ServerConnection& Conn);
	void ReceivedControl(FUE2ServerConnection& Conn, const TCHAR* Args);
	void PlayerLeft(FUE2ServerConnection& Conn);
	/** The host's "ban" of a player: also a combined validator ban (key, IP, UT2004 id). */
	void PlayerBanned(FUE2ServerConnection& Conn, const FString& Reason);
	void Tick(double Now);

	/** "validator <command>" of the admin console: status, ban, unban, whitelist, unwhitelist, export, import. */
	OPENUT2K4_API FString AdminCommand(const FString& Args);

	// ---- API for other code (Server/Docs/Validator.md "API") ----
	struct FPlayerStatus
	{
		int32 ConnId = 0;
		FString Name;
		bool bOpenUT2k4 = false;
		/** None, Pending, Identified, Failed. */
		FString Result;
		FString Fingerprint;
		FString FirstSeen;
		int32 KeyAgeHours = -1;
		/** The connection's address as the server sees it. */
		FString IP;
		FString LegacyId;
		/** The address the client signed as dialed ("ip:port[/host]"). */
		FString DialedAddress;
		/** The dialed address could not be matched to this server (NAT without PublicAddress=): relays not excluded. */
		bool bAddressUnverified = false;
		bool bProbation = false;
		bool bWhitelisted = false;
		bool bBanMatch = false;
		FString Reason;
	};
	OPENUT2K4_API bool GetStatus(int32 ConnId, FPlayerStatus& Out);
	OPENUT2K4_API TArray<FPlayerStatus> GetAllStatus();
	/** Does a fingerprint and / or IP / UT2004 id match the local or shared ban lists (empty fields are not compared)? */
	OPENUT2K4_API bool MatchesBanList(const FString& Fingerprint, const FString& IP, const FString& LegacyId, FString* OutReason = nullptr);
	/** Parses and checks a signed ban list against the trusted issuers. Entries are "fp|ip|cidr|legacy|date|reason|issuer". */
	OPENUT2K4_API bool ParseSignedBanList(const FString& Text, const TArray<FString>& TrustedIssuers, FString& OutIssuer, TArray<FString>& OutEntries, FString& OutError);
}
