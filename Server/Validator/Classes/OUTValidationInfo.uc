//=============================================================================
// OUTValidationInfo: what the OpenUT2k4 Validator knows about one player.
// One per PlayerController (real UT2004 clients too, with Result = None), so
// other mods can ask the same questions of every player. The public API is in
// OUTValidator (static functions) and documented in Server/Docs/Validator.md.
//
// Server-side fields are never replicated. When the policy for the player's
// result is Mark, the info replicates to every client (PRI, Result,
// ClientVersion, bProbation and, with bPublicFingerprints, the fingerprint).
//
// Part of OpenUT2k4 (MIT licence). Not affiliated with Epic Games.
//=============================================================================
class OUTValidationInfo extends ReplicationInfo;

// Result values (also OUTValidator.RESULT_*)
const RESULT_NONE = 0;
const RESULT_PENDING = 1;
const RESULT_IDENTIFIED = 2;
const RESULT_FAILED = 3;

// replicated when marked
var PlayerReplicationInfo PRI;
var byte Result;
var string ClientVersion;
var bool bProbation;
var string PublicFingerprint;

// server side
var PlayerController Controller;
var bool bOpenUT2k4;
// 32 hex digits: SHA-256 of the player's public key (Identified players only)
var string Fingerprint;
// first time this server saw the key, "YYYY-MM-DD HH" (server local time)
var string FirstSeen;
var int KeyAgeHours;
// the connection's address as the server sees it (never sent by the client)
var string IP;
// the UT2004 CD key hash id (PlayerController.GetPlayerIDHash)
var string LegacyId;
var bool bWhitelisted;
// the address the client signed as dialed ("ip:port[/host]") and whether it matched this server
var string DialedAddress;
var bool bAddressUnverified;
var bool bBanMatch;
var string BanReason;
// why the result is what it is ("bad signature", "no response" ...)
var string Reason;
var string Policy;
// when the answer must have arrived (Level.TimeSeconds)
var float Deadline;
// script time the signature check took (ms) and over how many ticks
var float VerifyMs;
var int VerifyTicks;

replication
{
	reliable if (bNetDirty && Role == ROLE_Authority)
		PRI, Result, ClientVersion, bProbation, PublicFingerprint;
}

function string ResultName()
{
	return class'OUTValidator'.static.ResultName(Result);
}

defaultproperties
{
	RemoteRole=ROLE_None
	bAlwaysRelevant=True
	NetUpdateFrequency=1
}
