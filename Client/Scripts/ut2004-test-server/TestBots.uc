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

defaultproperties
{
	FriendlyName="OpenUT2k4 Test Bots"
	Description="Keeps MinPlayers bots on an empty test server."
}
