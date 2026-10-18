//=============================================================================
// OUTValidator: the OpenUT2k4 Validator, an opt-in server mutator / server
// actor for stock UT2004 servers (3369, OldUnreal 3374).
// Server/Docs/Validator.md is the admin's guide; this is the implementation.
//
// - Every OpenUT2k4 client says so in its login URL (?OpenUT2k4=<version>).
//   Real UT2004 clients are never challenged.
// - OpenUT2k4 clients get an owner-only OUTValidatorLink with a fresh
//   challenge (random nonce, this server's address, the map, the player id)
//   and sign it with their player key, made silently on the player's machine.
//   A valid signature gives a stable, unforgeable id: the key's fingerprint.
//   Results: Identified, Failed (bad signature, wrong key, replayed or
//   malformed answer) or None (real UT2004 client, or no answer in time).
// - Per server: first-seen date of each key, key bans combined with the
//   connection's IP / subnet / UT2004 id, a whitelist, probation for new
//   keys, signed shared ban lists (imported by the helper script).
// - Conservative, log-only sanity checks (OUTPlayerCheck).
// - Everything is reachable for other mods (the static API below and
//   OUTValidationInfo).
//
// Defaults change nothing for players: log, and mark the result.
//
// Install: ServerActors=OpenUT2k4Validator.OUTValidator (or
// ?Mutator=OpenUT2k4Validator.OUTValidator), settings in
// [OpenUT2k4Validator.OUTValidator] of the server's ini.
//
// Part of OpenUT2k4 (MIT licence). Not affiliated with Epic Games.
//=============================================================================
class OUTValidator extends Mutator
	config;

const VERSION = "1.0";

const RESULT_NONE = 0;
const RESULT_PENDING = 1;
const RESULT_IDENTIFIED = 2;
const RESULT_FAILED = 3;

// ---- policies (Allow: log only; Mark: log and publish the result to every client; Kick) ----
var config string IdentifiedPolicy;
var config string FailedPolicy;
var config string NoResponsePolicy;
// seconds an OpenUT2k4 client has to answer the challenge
var config float ResponseTimeout;
// replicate fingerprints to every client when a result is marked (off: they stay on the server)
var config bool bPublicFingerprints;
// only whitelisted keys may play (OpenUT2k4 clients); real UT2004 clients are not affected
var config bool bWhitelistOnly;

// ---- probation for new keys (0: off) ----
var config int ProbationHours;
// Allow | Flag (log, admin list, marked) | Limited (also no map / kick voting) | Kick
var config string ProbationPolicy;
// at most this many players on probation at once (0: no cap); more are kicked
var config int MaxProbationPlayers;

// ---- bans (local and shared lists) ----
// Kick | Flag
var config string BanPolicy;
var config bool bMatchBanIP;
var config bool bMatchBanCIDR;
var config bool bMatchBanLegacyId;
// check every player's IP / UT2004 id against the ban lists, not only OpenUT2k4 clients
var config bool bApplyBansToAllClients;

// ---- shared ban lists: imported into OUTSharedBans by the helper script (Validator.md); kept here so the
// helper and the OpenUT2k4 dedicated server read the same keys ----
var config array<string> TrustedBanIssuer;
var config array<string> BanListSource;
var config int BanListRefreshMinutes;
// OpenUT2k4 dedicated server only
var config bool bUseKeyAsPlayerId;
// this server's addresses as players dial them (IP or host name, repeatable); behind NAT set it, so relayed
// answers fail instead of being accepted as "address unverified"
var config array<string> PublicAddress;

// ---- sanity checks ----
var config bool bSanityChecks;
var config bool bSanityChecksAllClients;
// Log | Kick
var config string SanityPolicy;
var config float SpeedHackRatio;
var config float MaxWalkSpeedFactor;
var config float SnapAngle;
var config float MaxNewNetPingDT;

// ---- cost ----
// rows of Montgomery multiplication per server tick (a 2048-bit check is 548 rows)
var config int VerifyRowsPerTick;
// check the RSA code against a built-in test vector when the map starts (logs the cost)
var config bool bSelfTest;

struct PendingLogin
{
	var int ID;
	var string Claim;
	var bool bOpenUT2k4;
	var float Time;
};

var OUTCrypto Crypto;
var OUTKeyStore Store;
var OUTSharedBans Shared;
var OUTValidatorRules Rules;
var array<OUTValidationInfo> Infos;
var array<OUTValidatorLink> Links;
var array<OUTRSAJob> Jobs;
var array<OUTPlayerCheck> Checks;
var array<PendingLogin> Pending;
var bool bLinked, bStoreDirty;
var float LastSave, LastLimit;
var int ChallengeCounter;

var OUTRSAJob SelfTestJob;
var float SelfTestMs, SelfTestMaxTickMs;
var int SelfTestTicks;
var string SelfTestKey1, SelfTestKey2, SelfTestSig1, SelfTestSig2;

// =============================================================================
// Startup
// =============================================================================

function PostBeginPlay()
{
	local OUTValidator Other;

	foreach DynamicActors(class'OUTValidator', Other)
	{
		if (Other != self)
		{
			Log("OpenUT2k4Validator: already running (installed twice?), this copy stays idle", 'OUTValidator');
			Destroy();
			return;
		}
	}
	Super.PostBeginPlay();
	// the clients need this package to see OUTValidatorLink / OUTValidationInfo
	AddToPackageMap();
	Crypto = new(None) class'OUTCrypto';
	Store = new(None) class'OUTKeyStore';
	Shared = new(None) class'OUTSharedBans';
	if (Store.ServerSecret == "")
	{
		Store.ServerSecret = Crypto.Sha256String("OUTSecret" $ Level.Year $ Level.Month $ Level.Day $ Level.Hour $ Level.Minute $ Level.Second
			$ Level.Millisecond $ FRand() $ Rand(1000000) $ FRand() $ Level.GetAddressURL());
		Store.SaveConfig();
	}
	Rules = Spawn(class'OUTValidatorRules');
	Rules.Validator = self;
	if (Level.Game.GameRulesModifiers == None)
		Level.Game.GameRulesModifiers = Rules;
	else
		Level.Game.GameRulesModifiers.AddGameRules(Rules);
	Log("OpenUT2k4Validator" @ VERSION $ ": policies identified" @ IdentifiedPolicy $ ", failed" @ FailedPolicy $ ", no response" @ NoResponsePolicy
		$ "; probation" @ ProbationHours @ "h (" $ ProbationPolicy $ ", cap" @ MaxProbationPlayers $ "); bans" @ Store.Bans.Length @ "local +" @ Shared.Bans.Length
		@ "shared (" $ BanPolicy $ ", ip" @ bMatchBanIP $ ", subnet" @ bMatchBanCIDR $ ", legacy id" @ bMatchBanLegacyId $ "); keys" @ Store.Keys.Length
		$ ", whitelist" @ Store.Whitelist.Length $ ", trusted ban list issuers" @ TrustedBanIssuer.Length $ "; sanity checks" @ bSanityChecks @ "(" $ SanityPolicy $ ")", 'OUTValidator');
	if (bSelfTest)
		StartSelfTest();
}

// installed as a ServerActor: join the mutator chain (ModifyLogin needs it)
function LinkSelf()
{
	local Mutator M;

	bLinked = true;
	if (Level.Game == None || Level.Game.BaseMutator == None)
		return;
	for (M = Level.Game.BaseMutator; M != None; M = M.NextMutator)
		if (M == self)
			return;
	Level.Game.BaseMutator.AddMutator(self);
}

function GetServerDetails(out GameInfo.ServerResponseLine ServerState)
{
	local int i;

	Super.GetServerDetails(ServerState);
	i = ServerState.ServerInfo.Length;
	ServerState.ServerInfo.Length = i + 1;
	ServerState.ServerInfo[i].Key = "OpenUT2k4Validator";
	ServerState.ServerInfo[i].Value = VERSION;
}

// =============================================================================
// Logins
// =============================================================================

function ModifyLogin(out string Portal, out string Options)
{
	local PendingLogin P;

	Super.ModifyLogin(Portal, Options);
	// GameInfo.Login gives the new player this PlayerID right after this call
	P.ID = Level.Game.CurrentID;
	P.bOpenUT2k4 = Level.Game.HasOption(Options, "OpenUT2k4");
	P.Claim = Left(Level.Game.ParseOption(Options, "OpenUT2k4"), 40);
	P.Time = Level.TimeSeconds;
	Pending[Pending.Length] = P;
}

function ProcessPending()
{
	local int i;
	local Controller C;
	local PlayerController PC;
	local bool bDone;

	for (i = 0; i < Pending.Length; i++)
	{
		bDone = Level.TimeSeconds - Pending[i].Time > 10;
		for (C = Level.ControllerList; C != None && !bDone; C = C.NextController)
		{
			PC = PlayerController(C);
			if (PC != None && PC.PlayerReplicationInfo != None && PC.PlayerReplicationInfo.PlayerID == Pending[i].ID)
			{
				// the connection is set once Login has returned
				if (NetConnection(PC.Player) == None)
					break;
				if (GetInfo(PC) == None)
					PlayerArrived(PC, Pending[i]);
				bDone = true;
			}
		}
		if (bDone)
		{
			Pending.Remove(i, 1);
			i--;
		}
	}
}

function PlayerArrived(PlayerController PC, PendingLogin P)
{
	local OUTValidationInfo VI;
	local OUTValidatorLink Link;
	local OUTPlayerCheck Check;
	local string Nonce, Map;

	VI = Spawn(class'OUTValidationInfo', PC);
	VI.Controller = PC;
	VI.PRI = PC.PlayerReplicationInfo;
	VI.bOpenUT2k4 = P.bOpenUT2k4;
	VI.ClientVersion = P.Claim;
	// the connection's own address: a client is never asked for its IP
	VI.IP = StripPort(PC.GetPlayerNetworkAddress());
	VI.LegacyId = PC.GetPlayerIDHash();
	Infos[Infos.Length] = VI;
	Links[Links.Length] = None;
	Jobs[Jobs.Length] = None;
	Checks[Checks.Length] = None;
	Log("OpenUT2k4Validator: login" @ PlayerName(VI) @ VI.IP @ Eval(VI.bOpenUT2k4, "OpenUT2k4" @ VI.ClientVersion, "UT2004 client"), 'OUTValidator');

	if ((bApplyBansToAllClients || VI.bOpenUT2k4) && CheckBans(VI))
		return;
	if (bSanityChecks && (VI.bOpenUT2k4 || bSanityChecksAllClients))
	{
		Check = Spawn(class'OUTPlayerCheck', PC);
		Check.Init(self, PC, VI);
		Checks[Checks.Length - 1] = Check;
	}
	if (!VI.bOpenUT2k4)
	{
		VI.Result = RESULT_NONE;
		return;
	}
	VI.Result = RESULT_PENDING;
	ChallengeCounter++;
	Nonce = Left(Crypto.Sha256String(Store.ServerSecret $ ChallengeCounter $ Level.Year $ Level.Month $ Level.Day $ Level.Hour $ Level.Minute $ Level.Second
		$ Level.Millisecond $ Level.TimeSeconds $ FRand() $ Rand(1000000) $ VI.IP $ VI.PRI.PlayerID), 32);
	Map = string(Level);
	Map = Left(Map, InStr(Map, "."));
	Link = Spawn(class'OUTValidatorLink', PC);
	Link.Validator = self;
	Link.PC = PC;
	Link.ChallengeText = "OUTV1;" $ Nonce $ ";" $ Level.GetAddressURL() $ ";" $ Map $ ";" $ VI.PRI.PlayerID;
	Links[Links.Length - 1] = Link;
	VI.Reason = "waiting for the answer";
	VI.Deadline = Level.TimeSeconds + ResponseTimeout;
}

function NotifyLogout(Controller Exiting)
{
	local int i;

	Super.NotifyLogout(Exiting);
	for (i = 0; i < Infos.Length; i++)
		if (Infos[i] != None && Infos[i].Controller == Exiting)
			LogSummary(i, "left");
}

function LogSummary(int i, string Why)
{
	if (Checks[i] != None)
		Log("OpenUT2k4Validator:" @ PlayerName(Infos[i]) @ Why $ ":" @ Checks[i].Summary(), 'OUTValidator');
}

// =============================================================================
// Answers
// =============================================================================

function LinkResponse(OUTValidatorLink Link, string Text, string Error)
{
	local int i, Semi;
	local OUTValidationInfo VI;
	local string KeyHex, SigHex, Dialed;
	local array<byte> Modulus, Sig, Digest;
	local OUTRSAJob Job;

	for (i = 0; i < Links.Length; i++)
		if (Links[i] == Link)
			break;
	if (i >= Links.Length || Infos[i] == None || Infos[i].Result != RESULT_PENDING)
		return;
	VI = Infos[i];
	if (Error != "")
	{
		Finish(i, RESULT_FAILED, Error);
		return;
	}
	if (Left(Text, 6) != "OUTR2;")
	{
		Finish(i, RESULT_FAILED, Eval(Left(Text, 4) == "OUTR", "old answer format (without the dialed address)", "malformed answer"));
		return;
	}
	Text = Mid(Text, 6);
	if (Left(Text, 6) == "NOKEY;")
	{
		Finish(i, RESULT_NONE, "the client did not sign (" $ Left(Mid(Text, 6), 100) $ ")");
		return;
	}
	// "<dialed ip:port[/host]>;<public key>;<signature>"
	Semi = InStr(Text, ";");
	if (Semi < 0 || Semi > 120)
	{
		Finish(i, RESULT_FAILED, "malformed answer");
		return;
	}
	Dialed = Left(Text, Semi);
	Text = Mid(Text, Semi + 1);
	Semi = InStr(Text, ";");
	if (Semi < 0)
	{
		Finish(i, RESULT_FAILED, "malformed answer");
		return;
	}
	KeyHex = Left(Text, Semi);
	SigHex = Mid(Text, Semi + 1);
	VI.DialedAddress = Left(Repl(Repl(Dialed, "|", ""), "\"", ""), 120);
	KeyHex = class'OUTCrypto'.static.NormalizeHex(KeyHex);
	if (Len(KeyHex) != 512)
	{
		Finish(i, RESULT_FAILED, "the public key is not RSA-2048");
		return;
	}
	if (!class'OUTCrypto'.static.HexToBytes(KeyHex, Modulus) || !class'OUTCrypto'.static.HexToBytes(SigHex, Sig))
	{
		Finish(i, RESULT_FAILED, "malformed answer");
		return;
	}
	VI.Fingerprint = Left(Crypto.Sha256String(KeyHex), 32);
	// the signed message is this connection's own challenge plus the address the client dialed: an answer to any
	// other challenge fails (replay), and the dialed address must be this server (relay; checked in Identified)
	Crypto.Sha256StringBytes("OUT2K4RESP2;" $ Link.ChallengeText $ ";" $ Dialed, Digest);
	Job = new(None) class'OUTRSAJob';
	if (!Job.Init(Modulus, Sig, Digest))
	{
		Finish(i, RESULT_FAILED, Job.FailReason);
		return;
	}
	Jobs[i] = Job;
	VI.Reason = "checking the signature";
}

function RunJobs()
{
	local int i, Budget, Before, R;
	local float T;

	Budget = Max(VerifyRowsPerTick, 8);
	if (SelfTestJob != None)
	{
		T = 0;
		Clock(T);
		Before = SelfTestJob.RowsDone;
		R = SelfTestJob.Step(Budget);
		UnClock(T);
		Budget -= SelfTestJob.RowsDone - Before;
		SelfTestMs += T;
		SelfTestMaxTickMs = FMax(SelfTestMaxTickMs, T);
		SelfTestTicks++;
		if (R != 0)
			FinishSelfTest(R == 1);
	}
	for (i = 0; i < Jobs.Length && Budget > 0; i++)
	{
		if (Jobs[i] == None)
			continue;
		T = 0;
		Clock(T);
		Before = Jobs[i].RowsDone;
		R = Jobs[i].Step(Budget);
		UnClock(T);
		Budget -= Jobs[i].RowsDone - Before;
		Infos[i].VerifyMs += T;
		Infos[i].VerifyTicks++;
		if (R == 1)
			Identified(i);
		else if (R == 2)
			Finish(i, RESULT_FAILED, Jobs[i].FailReason);
	}
}

// Relay defence: "" when the address the client signed as dialed is this server (or cannot be told apart behind
// NAT: VI.bAddressUnverified), otherwise why not.
function string CheckDialed(OUTValidationInfo VI)
{
	local string Addr, Host, IP, Own, OwnPort, P;
	local int i, Colon, Port;

	Addr = VI.DialedAddress;
	i = InStr(Addr, "/");
	if (i >= 0)
	{
		Host = Mid(Addr, i + 1);
		Addr = Left(Addr, i);
	}
	Colon = -1;
	for (i = 0; i < Len(Addr); i++)
		if (Mid(Addr, i, 1) == ":")
			Colon = i;
	if (Colon <= 0)
		return "malformed dialed address";
	IP = Left(Addr, Colon);
	Port = int(Mid(Addr, Colon + 1));
	Own = Level.GetAddressURL();
	i = InStr(Own, ":");
	OwnPort = Mid(Own, i + 1);
	Own = Left(Own, i);
	if (Port != int(OwnPort))
		return "relayed: the client dialed" @ VI.DialedAddress $ ", not this server's port" @ OwnPort;
	for (i = 0; i < PublicAddress.Length; i++)
	{
		P = PublicAddress[i];
		if (InStr(P, ":") >= 0)
			P = Left(P, InStr(P, ":"));
		if (P ~= IP || (Host != "" && P ~= Host))
			return "";
	}
	if (IP == Own || IP == "127.0.0.1")
		return "";
	if (PublicAddress.Length > 0)
		return "relayed: the client dialed" @ VI.DialedAddress $ ", which is not one of this server's addresses (PublicAddress=)";
	VI.bAddressUnverified = true;
	return "";
}

function Identified(int i)
{
	local OUTValidationInfo VI;
	local int k, Now;
	local array<string> F;
	local bool bFound;
	local string Relay;

	Relay = CheckDialed(Infos[i]);
	if (Relay != "")
	{
		Finish(i, RESULT_FAILED, Relay);
		return;
	}

	VI = Infos[i];
	Now = HoursNow();
	for (k = 0; k < Store.Keys.Length; k++)
	{
		if (Left(Store.Keys[k], 32) != VI.Fingerprint)
			continue;
		SplitBar(Store.Keys[k], F);
		VI.FirstSeen = F[1];
		Store.Keys[k] = VI.Fingerprint $ "|" $ F[1] $ "|" $ DateNow() $ "|" $ CleanName(VI.PRI.PlayerName);
		bFound = true;
		break;
	}
	if (!bFound)
	{
		VI.FirstSeen = DateNow();
		Store.Keys[Store.Keys.Length] = VI.Fingerprint $ "|" $ VI.FirstSeen $ "|" $ VI.FirstSeen $ "|" $ CleanName(VI.PRI.PlayerName);
	}
	bStoreDirty = true;
	VI.KeyAgeHours = Max(0, Now - DateToHours(VI.FirstSeen));
	VI.bWhitelisted = InList(Store.Whitelist, VI.Fingerprint);
	if (bPublicFingerprints)
		VI.PublicFingerprint = VI.Fingerprint;
	Finish(i, RESULT_IDENTIFIED, "");
}

function Finish(int i, byte Result, string Reason)
{
	local OUTValidationInfo VI;
	local string Policy, Cost;
	local int k, OnProbation;

	VI = Infos[i];
	VI.Result = Result;
	VI.Reason = Reason;
	Jobs[i] = None;
	if (VI.VerifyTicks > 0)
		Cost = ", verified in" @ VI.VerifyMs @ "ms of script time over" @ VI.VerifyTicks @ "ticks";
	if (Result == RESULT_IDENTIFIED)
		Cost = Cost $ Eval(VI.bAddressUnverified, ", address unverified (dialed " $ VI.DialedAddress $ "; set PublicAddress= behind NAT)", ", dialed " $ VI.DialedAddress);
	if (Result == RESULT_IDENTIFIED)
		Policy = IdentifiedPolicy;
	else if (Result == RESULT_FAILED)
		Policy = FailedPolicy;
	else
		Policy = NoResponsePolicy;
	VI.Policy = Policy;
	Log("OpenUT2k4Validator:" @ PlayerName(VI) @ VI.IP @ ResultName(Result) $ Eval(VI.Fingerprint != "" && Result == RESULT_IDENTIFIED, " key " $ VI.Fingerprint
		$ " first seen " $ VI.FirstSeen $ " (" $ VI.KeyAgeHours $ " h)", "") $ Eval(Reason != "", ": " $ Reason, "") $ Cost $ "; policy" @ Policy, 'OUTValidator');

	if (Result == RESULT_IDENTIFIED)
	{
		if (CheckBans(VI))
			return;
		if (bWhitelistOnly && !VI.bWhitelisted)
		{
			KickPlayer(VI, "This server only admits whitelisted players.");
			return;
		}
		if (ProbationHours > 0 && !VI.bWhitelisted && VI.KeyAgeHours < ProbationHours)
		{
			for (k = 0; k < Infos.Length; k++)
				if (Infos[k] != None && Infos[k] != VI && Infos[k].bProbation && Infos[k].Controller != None)
					OnProbation++;
			VI.bProbation = true;
			Log("OpenUT2k4Validator:" @ PlayerName(VI) @ "is on probation (key first seen" @ VI.KeyAgeHours @ "h ago, probation" @ ProbationHours @ "h):" @ ProbationPolicy, 'OUTValidator');
			if (ProbationPolicy ~= "Kick")
			{
				KickPlayer(VI, "This server does not admit new player keys yet (probation " $ ProbationHours $ " hours).");
				return;
			}
			if (MaxProbationPlayers > 0 && OnProbation >= MaxProbationPlayers)
			{
				KickPlayer(VI, "Too many new players on this server right now; try again later.");
				return;
			}
			if (ProbationPolicy ~= "Flag" || ProbationPolicy ~= "Limited")
				MarkInfo(VI);
		}
	}
	if (Policy ~= "Kick")
		KickPlayer(VI, "This server does not admit this player (" $ ResultName(Result) $ ").");
	else if (Policy ~= "Mark" || Policy ~= "Flag")
		MarkInfo(VI);
}

function MarkInfo(OUTValidationInfo VI)
{
	VI.RemoteRole = ROLE_SimulatedProxy;
	VI.NetUpdateTime = Level.TimeSeconds - 1;
}

// =============================================================================
// Bans
// =============================================================================

// True when the player matched a ban and was kicked.
function bool CheckBans(OUTValidationInfo VI)
{
	local string Reason;

	if (!FindBan(VI.Fingerprint, VI.IP, VI.LegacyId, Reason))
		return false;
	VI.bBanMatch = true;
	VI.BanReason = Reason;
	Log("OpenUT2k4Validator:" @ PlayerName(VI) @ VI.IP @ "matches a ban:" @ Reason $ "; policy" @ BanPolicy, 'OUTValidator');
	if (BanPolicy ~= "Kick")
		return KickPlayer(VI, "You are banned from this server.");
	MarkInfo(VI);
	return false;
}

function bool FindBan(string Fingerprint, string IP, string LegacyId, out string Reason)
{
	return FindBanIn(Store.Bans, "local", Fingerprint, IP, LegacyId, Reason) || FindBanIn(Shared.Bans, "shared", Fingerprint, IP, LegacyId, Reason);
}

// Ban entries: <fingerprint>|<ip>|<cidr>|<legacy id>|<date>|<reason>|<issuer>
function bool FindBanIn(array<string> List, string ListName, string Fingerprint, string IP, string LegacyId, out string Reason)
{
	local int i;
	local array<string> F;
	local string How;

	for (i = 0; i < List.Length; i++)
	{
		SplitBar(List[i], F);
		F.Length = 7;
		How = "";
		if (Fingerprint != "" && F[0] != "" && F[0] ~= Fingerprint)
			How = "key";
		else if (bMatchBanIP && IP != "" && F[1] != "" && F[1] == IP)
			How = "IP";
		else if (bMatchBanCIDR && IP != "" && F[2] != "" && InCIDR(IP, F[2]))
			How = "subnet" @ F[2];
		else if (bMatchBanLegacyId && LegacyId != "" && F[3] != "" && F[3] ~= LegacyId)
			How = "UT2004 id";
		if (How == "")
			continue;
		Reason = ListName @ "ban" @ i @ "(" $ How $ ")" $ Eval(F[5] != "", ": " $ F[5], "") $ Eval(F[6] != "", " by " $ F[6], "") $ Eval(F[4] != "", ", " $ F[4], "");
		return true;
	}
	return false;
}

function int IPToInt(string IP)
{
	local int i, Dot, V, R;

	R = 0;
	for (i = 0; i < 4; i++)
	{
		Dot = InStr(IP, ".");
		if (Dot < 0)
		{
			if (i != 3)
				return 0;
			V = int(IP);
		}
		else
		{
			V = int(Left(IP, Dot));
			IP = Mid(IP, Dot + 1);
		}
		R = (R << 8) | (V & 255);
	}
	return R;
}

function bool InCIDR(string IP, string CIDR)
{
	local int Slash, Bits, Mask;

	Slash = InStr(CIDR, "/");
	if (Slash < 0)
		return IP == CIDR;
	Bits = int(Mid(CIDR, Slash + 1));
	if (Bits <= 0 || Bits > 32)
		return false;
	if (Bits == 32)
		Mask = -1;
	else
		Mask = ~((1 << (32 - Bits)) - 1);
	return (IPToInt(IP) & Mask) == (IPToInt(Left(CIDR, Slash)) & Mask);
}

function bool KickPlayer(OUTValidationInfo VI, string Reason)
{
	local PlayerController PC;

	PC = VI.Controller;
	if (PC == None || PC.bDeleteMe)
		return true;
	if (PC.PlayerReplicationInfo != None && PC.PlayerReplicationInfo.bAdmin)
	{
		Log("OpenUT2k4Validator: not kicking admin" @ PlayerName(VI) @ "(" $ Reason $ ")", 'OUTValidator');
		return false;
	}
	Log("OpenUT2k4Validator: kicking" @ PlayerName(VI) @ VI.IP $ ":" @ Reason, 'OUTValidator');
	PC.ClientNetworkMessage("AC_Kicked", Reason);
	if (PC.Pawn != None)
		PC.Pawn.Destroy();
	if (PC != None)
		PC.Destroy();
	return true;
}

// =============================================================================
// Ticks
// =============================================================================

function Tick(float DeltaTime)
{
	local int i;
	local OUTValidationInfo VI;

	if (!bLinked)
		LinkSelf();
	if (Pending.Length > 0)
		ProcessPending();
	for (i = 0; i < Infos.Length; i++)
	{
		VI = Infos[i];
		if (VI == None || VI.Controller == None || VI.Controller.bDeleteMe)
		{
			RemoveInfo(i);
			i--;
			continue;
		}
		if (VI.Result == RESULT_PENDING && Jobs[i] == None && Level.TimeSeconds > VI.Deadline)
			Finish(i, RESULT_NONE, "no answer in" @ int(ResponseTimeout) @ "s");
	}
	RunJobs();
	if (Level.TimeSeconds - LastLimit > 2)
	{
		LastLimit = Level.TimeSeconds;
		if (ProbationPolicy ~= "Limited")
			for (i = 0; i < Infos.Length; i++)
				if (Infos[i].bProbation)
					LimitVoting(Infos[i].Controller);
	}
	if (bStoreDirty && Level.TimeSeconds - LastSave > 20)
		SaveStore();
}

function RemoveInfo(int i)
{
	if (Checks[i] != None)
		Checks[i].Destroy();
	if (Links[i] != None)
		Links[i].Destroy();
	if (Infos[i] != None)
		Infos[i].Destroy();
	Infos.Remove(i, 1);
	Links.Remove(i, 1);
	Jobs.Remove(i, 1);
	Checks.Remove(i, 1);
}

// Limited probation: the player's map / kick vote interface (xVoting) loses its handler.
function LimitVoting(PlayerController PC)
{
	local ReplicationInfo RI;

	if (PC == None)
		return;
	foreach DynamicActors(class'ReplicationInfo', RI)
		if (RI.Owner == PC && RI.IsA('VotingReplicationInfo') && RI.GetPropertyText("VH") != "None")
			RI.SetPropertyText("VH", "None");
}

function SaveStore()
{
	bStoreDirty = false;
	LastSave = Level.TimeSeconds;
	Store.SaveConfig();
}

function ServerTraveling(string URL, bool bItems)
{
	if (bStoreDirty)
		SaveStore();
	Super.ServerTraveling(URL, bItems);
}

function Destroyed()
{
	if (bStoreDirty && Store != None)
		SaveStore();
	Super.Destroyed();
}

// =============================================================================
// Sanity checks
// =============================================================================

function NotifyInstantHit(PlayerController PC)
{
	local int i;

	for (i = 0; i < Infos.Length; i++)
		if (Infos[i] != None && Infos[i].Controller == PC && Checks[i] != None)
			Checks[i].NotifyHit();
}

function SanityReport(PlayerController PC, OUTValidationInfo VI, string Text)
{
	Log("OpenUT2k4Validator: sanity:" @ PlayerName(VI) @ VI.IP @ Eval(VI.Fingerprint != "", "key " $ VI.Fingerprint, "") @ Text, 'OUTValidator');
	if (SanityPolicy ~= "Kick")
		KickPlayer(VI, "Kicked by the server's sanity checks.");
}

// =============================================================================
// Self-test and benchmark (built-in RSA-2048 test vector, public data only)
// =============================================================================

function OUTRSAJob MakeSelfTestJob()
{
	local array<byte> Modulus, Sig, Digest;
	local OUTRSAJob Job;

	class'OUTCrypto'.static.HexToBytes(SelfTestKey1 $ SelfTestKey2, Modulus);
	class'OUTCrypto'.static.HexToBytes(SelfTestSig1 $ SelfTestSig2, Sig);
	Crypto.Sha256StringBytes("OpenUT2k4 validator self-test", Digest);
	Job = new(None) class'OUTRSAJob';
	Job.Init(Modulus, Sig, Digest);
	return Job;
}

function StartSelfTest()
{
	if (Crypto.Sha256String("abc") != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
	{
		Log("OpenUT2k4Validator: SELF-TEST FAILED: SHA-256 gives wrong results on this server", 'OUTValidator');
		return;
	}
	SelfTestJob = MakeSelfTestJob();
	SelfTestMs = 0;
	SelfTestMaxTickMs = 0;
	SelfTestTicks = 0;
}

function FinishSelfTest(bool bValid)
{
	local array<byte> Modulus, Sig, Digest;
	local OUTRSAJob Bad;

	// the same check of a tampered signature must fail
	class'OUTCrypto'.static.HexToBytes(SelfTestKey1 $ SelfTestKey2, Modulus);
	class'OUTCrypto'.static.HexToBytes(SelfTestSig1 $ SelfTestSig2, Sig);
	Sig[100] = Sig[100] ^ 1;
	Crypto.Sha256StringBytes("OpenUT2k4 validator self-test", Digest);
	Bad = new(None) class'OUTRSAJob';
	Bad.Init(Modulus, Sig, Digest);
	while (Bad.Step(10000) == 0)
		continue;
	if (bValid && Bad.Result == 2)
		Log("OpenUT2k4Validator: self-test passed: SHA-256 and RSA-2048 verify (" $ SelfTestJob.TotalRows() @ "rows) took" @ SelfTestMs
			@ "ms of script time over" @ SelfTestTicks @ "ticks, at most" @ SelfTestMaxTickMs @ "ms per tick (VerifyRowsPerTick" @ VerifyRowsPerTick $ ")", 'OUTValidator');
	else
		Log("OpenUT2k4Validator: SELF-TEST FAILED (valid signature" @ bValid $ ", tampered signature rejected" @ (Bad.Result == 2) $ ")", 'OUTValidator');
	SelfTestJob = None;
}

// =============================================================================
// Admin commands: mutate validator <command> (logged in admins)
// =============================================================================

function Mutate(string MutateString, PlayerController Sender)
{
	local string Cmd, Arg;
	local int Sp;

	if (Left(MutateString, 9) ~= "validator")
	{
		Arg = Mid(MutateString, 10);
		Sp = InStr(Arg, " ");
		if (Sp >= 0)
		{
			Cmd = Left(Arg, Sp);
			Arg = Mid(Arg, Sp + 1);
		}
		else
		{
			Cmd = Arg;
			Arg = "";
		}
		if (Sender != None && Sender.PlayerReplicationInfo != None && Sender.PlayerReplicationInfo.bAdmin)
			AdminCommand(Sender, Locs(Cmd), Arg);
		else if (Sender != None)
			Sender.ClientMessage("OpenUT2k4 Validator" @ VERSION $ ": log in as admin for its commands");
	}
	Super.Mutate(MutateString, Sender);
}

function AdminCommand(PlayerController Sender, string Cmd, string Arg)
{
	local int i;
	local OUTValidationInfo VI;
	local string Reason, Entry;
	local float T;
	local OUTRSAJob Job;

	if (Cmd == "status" || Cmd == "")
	{
		Sender.ClientMessage("OpenUT2k4 Validator" @ VERSION $ ":" @ Infos.Length @ "players, keys" @ Store.Keys.Length $ ", bans" @ Store.Bans.Length
			@ "local +" @ Shared.Bans.Length @ "shared, whitelist" @ Store.Whitelist.Length);
		for (i = 0; i < Infos.Length; i++)
		{
			VI = Infos[i];
			if (VI != None)
				Sender.ClientMessage(PlayerName(VI) @ VI.IP @ ResultName(VI.Result) @ Eval(VI.Fingerprint != "", Left(VI.Fingerprint, 12) $ " first seen " $ VI.FirstSeen, "")
					$ Eval(VI.bProbation, " PROBATION", "") $ Eval(VI.bAddressUnverified, " (address unverified)", "") $ Eval(VI.bBanMatch, " BAN MATCH", "") $ Eval(VI.bWhitelisted, " whitelisted", ""));
		}
		Sender.ClientMessage("commands: status | ban <player|fingerprint|ip|cidr> [reason] | unban <fingerprint|ip|cidr> | whitelist <player|fingerprint> | unwhitelist <fingerprint> | bench");
	}
	else if (Cmd == "ban")
	{
		SplitFirst(Arg, Entry, Reason);
		VI = FindInfoByName(Entry);
		if (VI != None)
		{
			Store.Bans[Store.Bans.Length] = VI.Fingerprint $ "|" $ VI.IP $ "||" $ VI.LegacyId $ "|" $ DateNow() $ "|" $ CleanName(Reason) $ "|" $ CleanName(Sender.PlayerReplicationInfo.PlayerName);
			SaveStore();
			Sender.ClientMessage("banned" @ PlayerName(VI) @ Eval(VI.Fingerprint != "", "key " $ VI.Fingerprint, "(no key)") @ "IP" @ VI.IP);
			KickPlayer(VI, "You are banned from this server.");
		}
		else if (Len(Entry) == 32 && class'OUTCrypto'.static.NormalizeHex(Entry) != "")
		{
			Store.Bans[Store.Bans.Length] = Locs(Entry) $ "||||" $ DateNow() $ "|" $ CleanName(Reason) $ "|" $ CleanName(Sender.PlayerReplicationInfo.PlayerName);
			SaveStore();
			Sender.ClientMessage("banned key" @ Entry);
		}
		else if (InStr(Entry, ".") > 0)
		{
			if (InStr(Entry, "/") > 0)
				Store.Bans[Store.Bans.Length] = "||" $ Entry $ "||" $ DateNow() $ "|" $ CleanName(Reason) $ "|" $ CleanName(Sender.PlayerReplicationInfo.PlayerName);
			else
				Store.Bans[Store.Bans.Length] = "|" $ Entry $ "|||" $ DateNow() $ "|" $ CleanName(Reason) $ "|" $ CleanName(Sender.PlayerReplicationInfo.PlayerName);
			SaveStore();
			Sender.ClientMessage("banned" @ Entry);
		}
		else
			Sender.ClientMessage("no such player, key, IP or subnet:" @ Entry);
	}
	else if (Cmd == "unban")
	{
		for (i = Store.Bans.Length - 1; i >= 0; i--)
		{
			if (Arg != "" && (Left(Store.Bans[i], Len(Arg) + 1) ~= (Arg $ "|") || InStr(Store.Bans[i], "|" $ Arg $ "|") >= 0))
			{
				Sender.ClientMessage("removed ban" @ Store.Bans[i]);
				Store.Bans.Remove(i, 1);
			}
		}
		SaveStore();
	}
	else if (Cmd == "whitelist")
	{
		VI = FindInfoByName(Arg);
		if (VI != None && VI.Fingerprint != "")
			Entry = VI.Fingerprint $ "|" $ CleanName(VI.PRI.PlayerName);
		else if (Len(Arg) == 32)
			Entry = Locs(Arg) $ "|";
		if (Entry == "")
			Sender.ClientMessage("no identified player or key:" @ Arg);
		else
		{
			Store.Whitelist[Store.Whitelist.Length] = Entry;
			SaveStore();
			if (VI != None)
				VI.bWhitelisted = true;
			Sender.ClientMessage("whitelisted" @ Entry);
		}
	}
	else if (Cmd == "unwhitelist")
	{
		for (i = Store.Whitelist.Length - 1; i >= 0; i--)
			if (Arg != "" && Left(Store.Whitelist[i], Len(Arg)) ~= Arg)
				Store.Whitelist.Remove(i, 1);
		SaveStore();
		Sender.ClientMessage("removed" @ Arg @ "from the whitelist");
	}
	else if (Cmd == "bench")
	{
		Job = MakeSelfTestJob();
		T = 0;
		Clock(T);
		while (Job.Step(100000) == 0)
			continue;
		UnClock(T);
		Sender.ClientMessage("one RSA-2048 verify:" @ T @ "ms of script time (valid" @ (Job.Result == 1) $ ")");
		Log("OpenUT2k4Validator: bench: one RSA-2048 verify took" @ T @ "ms of script time", 'OUTValidator');
	}
}

function SplitFirst(string In, out string First, out string Rest)
{
	local int i;

	i = InStr(In, " ");
	if (i < 0)
	{
		First = In;
		Rest = "";
	}
	else
	{
		First = Left(In, i);
		Rest = Mid(In, i + 1);
	}
}

function OUTValidationInfo FindInfoByName(string Name)
{
	local int i;

	if (Name == "")
		return None;
	for (i = 0; i < Infos.Length; i++)
		if (Infos[i] != None && Infos[i].PRI != None && (Infos[i].PRI.PlayerName ~= Name || (Infos[i].Fingerprint != "" && Infos[i].Fingerprint ~= Name)))
			return Infos[i];
	return None;
}

// =============================================================================
// Helpers
// =============================================================================

static final function string ResultName(byte R)
{
	switch (R)
	{
		case RESULT_PENDING: return "Pending";
		case RESULT_IDENTIFIED: return "Identified";
		case RESULT_FAILED: return "Failed";
	}
	return "None";
}

static final function string StripPort(string Address)
{
	local int i;

	i = InStr(Address, ":");
	if (i >= 0)
		return Left(Address, i);
	return Address;
}

static final function string CleanName(string S)
{
	S = Repl(S, "|", "");
	S = Repl(S, "\"", "");
	return Left(S, 40);
}

static final function SplitBar(string S, out array<string> Out)
{
	local int i;

	Out.Length = 0;
	while (true)
	{
		i = InStr(S, "|");
		if (i < 0)
		{
			Out[Out.Length] = S;
			return;
		}
		Out[Out.Length] = Left(S, i);
		S = Mid(S, i + 1);
	}
}

static final function bool InList(array<string> List, string Fingerprint)
{
	local int i;

	if (Fingerprint == "")
		return false;
	for (i = 0; i < List.Length; i++)
		if (Left(List[i], 32) ~= Fingerprint)
			return true;
	return false;
}

function string PlayerName(OUTValidationInfo VI)
{
	if (VI != None && VI.PRI != None)
		return "\"" $ VI.PRI.PlayerName $ "\"";
	return "(unknown)";
}

static final function string Pad2(int V)
{
	if (V < 10)
		return "0" $ V;
	return string(V);
}

function string DateNow()
{
	return Level.Year $ "-" $ Pad2(Level.Month) $ "-" $ Pad2(Level.Day) @ Pad2(Level.Hour);
}

// days since 1970-01-01 (proleptic Gregorian)
static final function int DaysFromCivil(int Y, int M, int D)
{
	local int Era, Yoe, Doy, Doe, Mp;

	if (M <= 2)
		Y -= 1;
	Era = Y / 400;
	Yoe = Y - Era * 400;
	if (M > 2)
		Mp = M - 3;
	else
		Mp = M + 9;
	Doy = (153 * Mp + 2) / 5 + D - 1;
	Doe = Yoe * 365 + Yoe / 4 - Yoe / 100 + Doy;
	return Era * 146097 + Doe - 719468;
}

// "YYYY-MM-DD HH" to hours since 1970
static final function int DateToHours(string S)
{
	return DaysFromCivil(int(Left(S, 4)), int(Mid(S, 5, 2)), int(Mid(S, 8, 2))) * 24 + int(Mid(S, 11, 2));
}

function int HoursNow()
{
	return DaysFromCivil(Level.Year, Level.Month, Level.Day) * 24 + Level.Hour;
}

// =============================================================================
// API for other mods (Server/Docs/Validator.md "API for other mods").
// All functions are static: call them as class'OpenUT2k4Validator.OUTValidator'.static.GetResult(C).
// Mods without a compile-time dependency use the OUTValidationInfo actors instead:
// foreach DynamicActors(class'ReplicationInfo', RI) if (RI.IsA('OUTValidationInfo')) ... RI.GetPropertyText("Fingerprint")
// =============================================================================

static function OUTValidator GetValidator(Actor Context)
{
	local OUTValidator V;

	if (Context == None)
		return None;
	foreach Context.DynamicActors(class'OUTValidator', V)
		if (!V.bDeleteMe)
			return V;
	return None;
}

// The player's OUTValidationInfo (None when the validator did not see the login).
static function OUTValidationInfo GetInfo(Controller C)
{
	local OUTValidationInfo VI;

	if (C == None)
		return None;
	foreach C.DynamicActors(class'OUTValidationInfo', VI)
		if (VI.Controller == C)
			return VI;
	return None;
}

// RESULT_NONE (UT2004 client, no answer, unknown), RESULT_PENDING, RESULT_IDENTIFIED, RESULT_FAILED
static function byte GetResult(Controller C)
{
	local OUTValidationInfo VI;

	VI = GetInfo(C);
	if (VI == None)
		return RESULT_NONE;
	return VI.Result;
}

// 32 hex digits for Identified players, "" otherwise
static function string GetFingerprint(Controller C)
{
	local OUTValidationInfo VI;

	VI = GetInfo(C);
	if (VI == None || VI.Result != RESULT_IDENTIFIED)
		return "";
	return VI.Fingerprint;
}

// "YYYY-MM-DD HH" when this server first saw the player's key, "" when not identified
static function string GetFirstSeen(Controller C)
{
	local OUTValidationInfo VI;

	VI = GetInfo(C);
	if (VI == None || VI.Result != RESULT_IDENTIFIED)
		return "";
	return VI.FirstSeen;
}

// hours since the first sighting, -1 when not identified
static function int GetKeyAgeHours(Controller C)
{
	local OUTValidationInfo VI;

	VI = GetInfo(C);
	if (VI == None || VI.Result != RESULT_IDENTIFIED)
		return -1;
	return VI.KeyAgeHours;
}

// the connection's IP as the server sees it
static function string GetIP(Controller C)
{
	local OUTValidationInfo VI;

	VI = GetInfo(C);
	if (VI != None)
		return VI.IP;
	if (PlayerController(C) != None)
		return StripPort(PlayerController(C).GetPlayerNetworkAddress());
	return "";
}

static function bool IsOnProbation(Controller C)
{
	local OUTValidationInfo VI;

	VI = GetInfo(C);
	return VI != None && VI.bProbation;
}

// the address the client signed as dialed ("ip:port[/host]"), "" when none
static function string GetDialedAddress(Controller C)
{
	local OUTValidationInfo VI;

	VI = GetInfo(C);
	if (VI == None)
		return "";
	return VI.DialedAddress;
}

// false when an Identified player's dialed address could not be matched to this server (NAT without PublicAddress=)
static function bool IsAddressVerified(Controller C)
{
	local OUTValidationInfo VI;

	VI = GetInfo(C);
	return VI != None && VI.Result == RESULT_IDENTIFIED && !VI.bAddressUnverified;
}

static function bool IsOpenUT2k4(Controller C)
{
	local OUTValidationInfo VI;

	VI = GetInfo(C);
	return VI != None && VI.bOpenUT2k4;
}

// Does a fingerprint and / or IP (and / or UT2004 id) match the local or shared ban lists? Empty fields are
// not compared. IP and subnet matching follow the validator's bMatchBanIP / bMatchBanCIDR settings.
static function bool MatchesBanList(Actor Context, string Fingerprint, string IP, optional string LegacyId, optional out string Reason)
{
	local OUTValidator V;

	V = GetValidator(Context);
	if (V == None)
		return false;
	return V.FindBan(Fingerprint, StripPort(IP), LegacyId, Reason);
}

defaultproperties
{
	FriendlyName="OpenUT2k4 Validator"
	Description="Identifies OpenUT2k4 players by their player key, key bans, probation for new keys, server-side sanity checks."
	GroupName="OpenUT2k4Validator"
	IdentifiedPolicy="Mark"
	FailedPolicy="Mark"
	NoResponsePolicy="Mark"
	ResponseTimeout=30.0
	bPublicFingerprints=False
	bWhitelistOnly=False
	ProbationHours=0
	ProbationPolicy="Flag"
	MaxProbationPlayers=0
	BanPolicy="Kick"
	bMatchBanIP=True
	bMatchBanCIDR=False
	bMatchBanLegacyId=True
	bApplyBansToAllClients=True
	bSanityChecks=True
	bSanityChecksAllClients=False
	SanityPolicy="Log"
	SpeedHackRatio=1.25
	MaxWalkSpeedFactor=1.6
	SnapAngle=60.0
	MaxNewNetPingDT=1.0
	VerifyRowsPerTick=64
	bSelfTest=True
	BanListRefreshMinutes=60
	SelfTestKey1="a3f21f1123c9ee0488873bfa0d02cbd18e9294718b1ceed9aa981f58edb763d76270ffe8199bf3a0d316b845be0df06dc5e433c59736f6427f9a8dc010975d4c21d3ea2ef1114dbdd7c10e8e0ba5a3af58d782d8ad9f747d5fc3444e9514ab6d3c2a99f94b29761ae6150473b560ec9a60576d59949c80ba443ae79b2631f315"
	SelfTestKey2="0dfcf7119e3ade1e1c7dc4a9b2f95485671abe04f50a9e4b6cb0eadce148ae1fe4209d95600710a985619a4e2bea81a41007d879fa7a296ee9dd50b239b34f692e1c09993bbe6756cea8cc8638b65fa1d1f842bb19bbad1b28dbfcd439af00d3399bac48017c3786b050a54e9d4a3c4e01336932043e2fe007470d63d78d4743"
	SelfTestSig1="7581da2f3a94430cce23471a696c36e7c862fcf60cf1fcf4614960865a6977a35f53b0f9af1a7eb1e68e6aca7c874f49b7e0a977bd2632b1700c0227e4fcc4922ebe3364fac19ad10710f352628e473167cad834865d59946fa55d99bded7a3c63b81baffe23c9853492e535762c64d025cf67785804c84d49d31f5cdca7fd7d"
	SelfTestSig2="0a78ef1c92c2a6870d41833a879a1f3d220dc2bf17fcd70360934e2416befaaca1ac59fa9f1a6978543e95578a517aac2b2177c0bd532c0c92bf743f0a32e58f42675af57428d7f5c952f17d41b21e56c6cb9064e2b326abbfffc7762f10c9646fa3f9ab536c1cf81538d3f17cb8fc2ba82a302826ea45a02e92b257020f4ccd"
}
