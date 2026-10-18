//=============================================================================
// OUTPlayerCheck: conservative server-side sanity checks of one player, log
// only by default (OUTValidator SanityPolicy). Spawned for OpenUT2k4 clients,
// or for every player with bSanityChecksAllClients. Each check needs several
// bad windows in a row before it reports, and reports at most once a minute.
//
//   time stamps   the client's move time stamps (PlayerController.CurrentTimeStamp)
//                 against server time over 5 s windows: a speed hack runs the
//                 client clock fast
//   walk speed    horizontal speed while walking on non-moving ground, 1 s
//                 windows, against GroundSpeed (speed combos included)
//   NewNet        the PingDT a lag compensated (NewNet) fire mode computed from
//                 the client's time stamp: negative or far above the ping
//   aim           instant hit hits that came right after a view snap larger
//                 than SnapAngle, as a share of all instant hit hits
//
// Part of OpenUT2k4 (MIT licence). Not affiliated with Epic Games.
//=============================================================================
class OUTPlayerCheck extends Info;

var OUTValidator Validator;
var PlayerController PC;
var OUTValidationInfo VInfo;

// time stamps
var float StampWinStart, StampWinStartStamp;
var int StampStrikes;
var float StampRatioMax, StampRatioSum;
var int StampWindows;

// walking speed
var vector LastLoc;
var bool bLastWalking;
var float WalkDist, WalkTime, WalkMaxGround;
var int WalkStrikes;
var float WalkRatioMax;

// NewNet
var float LastPingDT[2];
var int NewNetOdd;

// aim: the view of the last ticks
var rotator RotHist[16];
var float RotTime[16];
var int RotHead;
var int Hits, SnapHits, ReportedHits;

var float LastReport[4];

function Init(OUTValidator V, PlayerController P, OUTValidationInfo VI)
{
	Validator = V;
	PC = P;
	VInfo = VI;
	StampWinStart = -1;
	LastPingDT[0] = -999;
	LastPingDT[1] = -999;
}

function Report(int Kind, string Text)
{
	if (Level.TimeSeconds - LastReport[Kind] < 60 && LastReport[Kind] > 0)
		return;
	LastReport[Kind] = Level.TimeSeconds;
	Validator.SanityReport(PC, VInfo, Text);
}

function Tick(float DeltaTime)
{
	if (PC == None || PC.bDeleteMe)
	{
		Destroy();
		return;
	}
	CheckStamps();
	CheckWalk(DeltaTime);
	CheckNewNet();
	RotHead = (RotHead + 1) & 15;
	RotHist[RotHead] = PC.Rotation;
	RotTime[RotHead] = Level.TimeSeconds;
}

function CheckStamps()
{
	local float DT, DS, Ratio;

	if (PC.Pawn == None || PC.CurrentTimeStamp <= 0)
	{
		StampWinStart = -1;
		return;
	}
	if (StampWinStart < 0)
	{
		StampWinStart = Level.TimeSeconds;
		StampWinStartStamp = PC.CurrentTimeStamp;
		return;
	}
	DT = Level.TimeSeconds - StampWinStart;
	if (DT < 5.0)
		return;
	DS = PC.CurrentTimeStamp - StampWinStartStamp;
	StampWinStart = Level.TimeSeconds;
	StampWinStartStamp = PC.CurrentTimeStamp;
	if (DS <= 0)
		return;
	Ratio = DS / DT;
	StampWindows++;
	StampRatioSum += Ratio;
	StampRatioMax = FMax(StampRatioMax, Ratio);
	if (Ratio > Validator.SpeedHackRatio)
	{
		StampStrikes++;
		if (StampStrikes >= 3)
			Report(0, "time stamps run" @ Ratio @ "times as fast as the server clock for 15 s (speed hack?)");
	}
	else
		StampStrikes = 0;
}

function CheckWalk(float DeltaTime)
{
	local Pawn P;
	local vector D;
	local float Dist, Speed, Limit;
	local bool bWalking;

	P = PC.Pawn;
	bWalking = P != None && Vehicle(P) == None && P.Physics == PHYS_Walking && (P.Base == None || Mover(P.Base) == None);
	if (bWalking && bLastWalking)
	{
		D = P.Location - LastLoc;
		D.Z = 0;
		Dist = VSize(D);
		// teleporters, translocation, respawns
		if (Dist < 600)
		{
			WalkDist += Dist;
			WalkTime += DeltaTime;
			WalkMaxGround = FMax(WalkMaxGround, P.GroundSpeed);
		}
	}
	if (P != None)
		LastLoc = P.Location;
	bLastWalking = bWalking;
	if (WalkTime < 1.0)
		return;
	Speed = WalkDist / WalkTime;
	Limit = WalkMaxGround * Validator.MaxWalkSpeedFactor + 50;
	if (WalkMaxGround > 0)
		WalkRatioMax = FMax(WalkRatioMax, Speed / WalkMaxGround);
	if (WalkMaxGround > 0 && Speed > Limit)
	{
		WalkStrikes++;
		if (WalkStrikes >= 3)
			Report(1, "walks at" @ int(Speed) @ "UU/s for 3 s, ground speed" @ int(WalkMaxGround) @ "(speed hack?)");
	}
	else
		WalkStrikes = 0;
	WalkDist = 0;
	WalkTime = 0;
	WalkMaxGround = 0;
}

function CheckNewNet()
{
	local Weapon W;
	local WeaponFire FM;
	local int i;
	local string S;
	local float PingDT, Ping;

	if (PC.Pawn == None || PC.Pawn.Weapon == None)
		return;
	W = PC.Pawn.Weapon;
	for (i = 0; i < 2; i++)
	{
		FM = W.GetFireMode(i);
		if (FM == None)
			continue;
		S = FM.GetPropertyText("PingDT");
		if (S == "")
			continue;
		PingDT = float(S);
		if (PingDT == LastPingDT[i])
			continue;
		LastPingDT[i] = PingDT;
		Ping = 0.004 * PC.PlayerReplicationInfo.Ping;
		if (PingDT < -0.05 || PingDT > Validator.MaxNewNetPingDT || PingDT > Ping + 0.75)
		{
			NewNetOdd++;
			if (NewNetOdd >= 3)
				Report(2, "NewNet shot rewound" @ PingDT @ "s with a ping of" @ int(Ping * 1000) @ "ms (forged time stamp?)");
		}
	}
}

// From OUTValidatorRules: this player's instant hit damaged another player.
function NotifyHit()
{
	local int i, Idx, D, MaxD;
	local rotator Now;

	Now = PC.Rotation;
	for (i = 1; i < 16; i++)
	{
		Idx = (RotHead - i) & 15;
		if (Level.TimeSeconds - RotTime[Idx] > 0.12)
			break;
		D = AngleDiff(Now.Yaw, RotHist[Idx].Yaw) + AngleDiff(Now.Pitch, RotHist[Idx].Pitch);
		MaxD = Max(MaxD, D);
	}
	Hits++;
	if (MaxD * 360.0 / 65536.0 > Validator.SnapAngle)
		SnapHits++;
	if (Hits >= 20 && Hits - ReportedHits >= 20 && SnapHits * 2 > Hits)
	{
		ReportedHits = Hits;
		Report(3, SnapHits @ "of" @ Hits @ "instant hit hits came right after a view snap of more than" @ int(Validator.SnapAngle) @ "degrees (aim bot?)");
	}
}

static final function int AngleDiff(int A, int B)
{
	local int D;

	D = (A - B) & 65535;
	if (D > 32768)
		D = 65536 - D;
	return D;
}

function string Summary()
{
	local string S;

	if (StampWindows > 0)
		S = "time stamp rate avg" @ (StampRatioSum / StampWindows) @ "max" @ StampRatioMax $ ", ";
	S = S $ "walk speed max" @ WalkRatioMax @ "x ground speed, instant hits" @ Hits @ "(" $ SnapHits @ "after snaps)";
	return S;
}

defaultproperties
{
	RemoteRole=ROLE_None
	bHidden=True
}
