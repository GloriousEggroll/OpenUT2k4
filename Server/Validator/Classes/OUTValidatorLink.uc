//=============================================================================
// OUTValidatorLink: the challenge an OpenUT2k4 client answers. Spawned by
// OUTValidator for players whose login URL has ?OpenUT2k4=, owned by their
// PlayerController and replicated to that player only. Stock UT2004 clients
// never get one.
//
// Protocol (Server/Docs/Validator.md, Client/Docs/UE2Net.md):
//   server -> owner   ChallengeText  "OUTV1;<nonce>;<server address>;<map>;<player id>"
//   owner  -> server  ServerResponse(Part, Total, Data), Data parts joined:
//                     "OUTR2;<dialed ip:port[/host]>;<public key hex>;<signature hex>"
//                     (signature of "OUT2K4RESP2;" $ ChallengeText $ ";" $ dialed,
//                     RSA-2048 e=3 PKCS#1 v1.5 SHA-256) or "OUTR2;NOKEY;<reason>"
//
// Part of OpenUT2k4 (MIT licence). Not affiliated with Epic Games.
//=============================================================================
class OUTValidatorLink extends Info;

var string ChallengeText;

// server side
var OUTValidator Validator;
var PlayerController PC;
var array<string> Parts;
var int NumParts, Received;
var bool bComplete;

replication
{
	reliable if (bNetOwner && Role == ROLE_Authority)
		ChallengeText;
	reliable if (Role < ROLE_Authority)
		ServerResponse;
}

function ServerResponse(int Part, int Total, string Data)
{
	if (bComplete || Validator == None)
		return;
	if (Total < 1 || Total > 16 || Part < 0 || Part >= Total || Len(Data) > 400 || (NumParts != 0 && Total != NumParts))
	{
		bComplete = true;
		Validator.LinkResponse(self, "", "malformed response");
		return;
	}
	if (NumParts == 0)
	{
		NumParts = Total;
		Parts.Length = Total;
	}
	if (Parts[Part] == "")
		Received++;
	Parts[Part] = Data;
	if (Received < NumParts)
		return;
	bComplete = true;
	Validator.LinkResponse(self, JoinParts(), "");
}

function string JoinParts()
{
	local int i;
	local string S;

	for (i = 0; i < Parts.Length; i++)
		S = S $ Parts[i];
	return S;
}

defaultproperties
{
	RemoteRole=ROLE_SimulatedProxy
	bOnlyRelevantToOwner=True
	bAlwaysRelevant=False
	bHidden=True
	NetUpdateFrequency=2
	NetPriority=1.5
}
