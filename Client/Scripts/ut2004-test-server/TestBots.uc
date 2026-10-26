// OpenUT2k4 test helper (Scripts/ut2004-test-server.sh --bots): keeps MinPlayers bots on a dedicated test server even
// while no human is connected, so server queries list players. Our own code, compiled per scratch dir.
class TestBots extends Mutator;

// the scores logged last (Timer)
var string LastScores;
// ?TestAdrenaline=1 in the server URL: human players are kept at full adrenaline (to test combos)
var bool bGiveAdrenaline;

function PostBeginPlay()
{
	Super.PostBeginPlay();
	bGiveAdrenaline = InStr(Caps(Level.GetLocalURL()), "TESTADRENALINE=1") >= 0;
	SetTimer(1.0, true);
}

function Timer()
{
	local DeathMatch DM;
	local Controller C;
	local string Scores;
	// scores and deaths of every player whenever they change (to compare a client's view with the server's)
	for (C = Level.ControllerList; C != None; C = C.NextController)
	{
		if (C.PlayerReplicationInfo != None && !C.PlayerReplicationInfo.bOnlySpectator)
		{
			Scores = Scores $ " " $ C.PlayerReplicationInfo.PlayerName $ "=" $ int(C.PlayerReplicationInfo.Score) $ "/" $ int(C.PlayerReplicationInfo.Deaths);
		}
	}
	if (Level.GRI != None && Level.GRI.Teams[0] != None && Level.GRI.Teams[1] != None)
	{
		Scores = Scores $ " teams=" $ int(Level.GRI.Teams[0].Score) $ "-" $ int(Level.GRI.Teams[1].Score);
	}
	if (bGiveAdrenaline)
	{
		for (C = Level.ControllerList; C != None; C = C.NextController)
		{
			if (PlayerController(C) != None && xPawn(C.Pawn) != None && xPawn(C.Pawn).CurrentCombo == None && C.Adrenaline < C.AdrenalineMax)
			{
				C.Adrenaline = C.AdrenalineMax;
				Log("TestBots adrenaline " $ C.PlayerReplicationInfo.PlayerName $ " " $ int(C.Adrenaline));
			}
		}
	}
	if (Scores != LastScores)
	{
		LastScores = Scores;
		Log("TestBots scores" $ Scores);
	}
	DM = DeathMatch(Level.Game);
	if (DM != None && DM.IsInState('PendingMatch'))
	{
		// stock servers wait for a human before the match starts (and bots only spawn in a running match)
		DM.StartMatch();
		return;
	}
	if (DM != None && DM.NumBots + DM.NumPlayers < DM.MinPlayers)
	{
		DM.AddBot();
	}
}

// stock servers leave bots out of the players query (GameInfo.GetServerPlayers skips bBot); list ours so the
// players pane of a browser has rows to show. The server caches its query replies: bots show after 30-60 s.
function GetServerPlayers(out GameInfo.ServerResponseLine ServerState)
{
	local Controller C;
	local int i;
	Super.GetServerPlayers(ServerState);
	for (C = Level.ControllerList; C != None; C = C.NextController)
	{
		if (C.PlayerReplicationInfo != None && AIController(C) != None)
		{
			i = ServerState.PlayerInfo.Length;
			ServerState.PlayerInfo.Length = i + 1;
			ServerState.PlayerInfo[i].PlayerNum = C.PlayerReplicationInfo.PlayerID;
			ServerState.PlayerInfo[i].PlayerName = C.PlayerReplicationInfo.PlayerName;
			ServerState.PlayerInfo[i].Ping = 0;
			ServerState.PlayerInfo[i].Score = C.PlayerReplicationInfo.Score;
			ServerState.PlayerInfo[i].StatsID = 0;
		}
	}
}

// "Mutate TestSpawn <Package.Class> [distance]" (local test servers only): spawns that actor class (a mod vehicle:
// an Odin, a map's MantaMirage) in front of the sender, to test what the randomizer of a map rarely hands out.
// "Mutate TestDamage <amount> [self]": the nearest vehicle in front of the sender (or with "self" the vehicle it sits
// in) takes that damage (a manual gun turret destroyed: its dead base).
function Mutate(string MutateString, PlayerController Sender)
{
	local array<string> Parts;
	local class<Actor> SpawnClass;
	local Actor A, Best;
	local vector Dir, Loc;
	local float Dist, BestDot, Cosine;

	Split(MutateString, " ", Parts);
	if (Sender != None && Sender.Pawn != None && Parts.Length >= 2 && Parts[0] ~= "TestSpawn")
	{
		SpawnClass = class<Actor>(DynamicLoadObject(Parts[1], class'Class', true));
		Dist = 600;
		if (Parts.Length >= 3)
		{
			Dist = float(Parts[2]);
		}
		Dir = vector(Sender.Pawn.Rotation);
		Dir.Z = 0;
		Loc = Sender.Pawn.Location + Normal(Dir) * Dist + vect(0,0,150);
		A = None;
		if (SpawnClass != None)
		{
			A = Spawn(SpawnClass,,, Loc, Sender.Pawn.Rotation);
		}
		Log("TestBots spawn " $ Parts[1] $ " -> " $ A);
	}
	else if (Sender != None && Sender.Pawn != None && Parts.Length >= 2 && Parts[0] ~= "TestDamage")
	{
		BestDot = 0.7;
		Dir = vector(Sender.Rotation);
		foreach DynamicActors(class'Actor', A)
		{
			if (Vehicle(A) != None && A != Sender.Pawn && VSize(A.Location - Sender.Pawn.Location) < 5000)
			{
				Cosine = Normal(A.Location - Sender.Pawn.Location) Dot Dir;
				if (Cosine > BestDot)
				{
					BestDot = Cosine;
					Best = A;
				}
			}
		}
		if (Parts.Length >= 3 && Parts[2] ~= "self")
		{
			Best = Sender.Pawn; // the vehicle the sender sits in
		}
		if (Best != None)
		{
			Best.TakeDamage(int(Parts[1]), Sender.Pawn, Best.Location, vect(0,0,0), class'DamTypeShockBeam');
		}
		Log("TestBots damage " $ Parts[1] $ " -> " $ Best);
	}
	Super.Mutate(MutateString, Sender);
}

defaultproperties
{
	FriendlyName="OpenUT2k4 Test Bots"
	Description="Keeps MinPlayers bots on an empty test server."
}
