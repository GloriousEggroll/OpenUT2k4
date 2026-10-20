// UT2004 map voting (xVoting.VotingReplicationInfo) on a UE2 server. See UE2NetMapVote.h.

#include "UE2NetMapVote.h"
#include "UE2NetLog.h"
#include "../UScript/UScriptCore.h"

namespace UE2NetMapVoteLocal
{
	/** xVoting.VotingReplicationInfo or a subclass (mods extend it). */
	bool IsVotingInfo(const FUE2NetActor& Actor)
	{
		for (const FUSClass* C = Actor.Class; C; C = C->GetSuperClass())
		{
			if (C->GetName() == TEXT("VotingReplicationInfo"))
			{
				return true;
			}
		}
		return false;
	}

	const FUE2NetValue* Member(const FUE2NetValue& Struct, const TCHAR* Name)
	{
		for (const TPair<FName, FUE2NetValue>& M : Struct.Members)
		{
			if (M.Key == FName(Name))
			{
				return &M.Value;
			}
		}
		return nullptr;
	}
	int32 MemberInt(const FUE2NetValue& Struct, const TCHAR* Name, int32 Default = 0)
	{
		const FUE2NetValue* V = Member(Struct, Name);
		return V ? V->I : Default;
	}
	FString MemberStr(const FUE2NetValue& Struct, const TCHAR* Name)
	{
		const FUE2NetValue* V = Member(Struct, Name);
		return V ? V->S : FString();
	}
	const FUE2NetValue* Param(const TArray<TPair<FName, FUE2NetValue>>& Params, int32 Index)
	{
		return Params.IsValidIndex(Index) ? &Params[Index].Value : nullptr;
	}
	int32 PropInt(const FUE2NetActor& Actor, const TCHAR* Name, int32 Default = 0)
	{
		const FUE2NetValue* V = Actor.Properties.Find(Name);
		return V ? V->I : Default;
	}
}

using namespace UE2NetMapVoteLocal;

void FUE2NetMapVote::ActorOpened(const FUE2NetActor& Actor)
{
	if (!IsVotingInfo(Actor))
	{
		return;
	}
	if (Channel == Actor.Channel)
	{
		return;
	}
	// every player has one and they are all relevant (ReplicationInfo.bAlwaysRelevant): ours names our controller as
	// its PlayerOwner (only the owner gets a PlayerController replicated)
	const FUE2NetValue* Owner = Actor.Properties.Find(TEXT("PlayerOwner"));
	const int32 OwnerChannel = Owner ? Owner->Obj.Channel : 0;
	const int32 OurController = Connection.FindPlayerControllerChannel();
	if (OwnerChannel ? OwnerChannel != OurController : Channel != 0)
	{
		return;
	}
	Claim(Actor);
}

void FUE2NetMapVote::Claim(const FUE2NetActor& Actor)
{
	Reset();
	Channel = Actor.Channel;
	ActorUpdated(Actor, {});
	UE_LOG(LogUE2Net, Log, TEXT("MapVote: %s on channel %d (map voting %s, %d maps, %d game types)"), *Actor.ClassPath, Channel,
		bMapVote ? TEXT("on") : TEXT("off"), MapCount, PropInt(Actor, TEXT("GameConfigCount")));
}

void FUE2NetMapVote::ActorUpdated(const FUE2NetActor& Actor, const TArray<FString>& Changed)
{
	if (Actor.Channel != Channel)
	{
		return;
	}
	bMapVote = PropInt(Actor, TEXT("bMapVote")) != 0;
	MapCount = PropInt(Actor, TEXT("MapCount"));
	CurrentGameConfig = PropInt(Actor, TEXT("CurrentGameConfig"));
}

void FUE2NetMapVote::ActorClosed(const FUE2NetActor& Actor)
{
	if (Actor.Channel == Channel)
	{
		Reset();
	}
}

bool FUE2NetMapVote::FunctionCall(const FUE2NetActor& Actor, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params)
{
	if (!Function)
	{
		return false;
	}
	if (Actor.Channel != Channel)
	{
		// the first ReceiveGameConfig comes in the bunch that opens the channel, before ActorOpened; the server calls
		// only the owner's, so a call says this one is ours
		if (!IsVotingInfo(Actor))
		{
			return false;
		}
		Claim(Actor);
	}
	const FName Name = Function->Name;
	if (Name == TEXT("ReceiveGameConfig"))
	{
		// MapVoteGameConfigLite: GameClass, Prefix, GameName
		if (const FUE2NetValue* P = Param(Params, 0))
		{
			GameConfigs.Add({ MemberStr(*P, TEXT("GameClass")), MemberStr(*P, TEXT("Prefix")), MemberStr(*P, TEXT("GameName")) });
		}
		Reply();
		return true;
	}
	if (Name == TEXT("ReceiveMapInfo"))
	{
		// MapVoteMapList: MapName, PlayCount, Sequence, bEnabled
		if (const FUE2NetValue* P = Param(Params, 0))
		{
			const FUE2NetValue* Enabled = Member(*P, TEXT("bEnabled"));
			Maps.Add({ MemberStr(*P, TEXT("MapName")), MemberInt(*P, TEXT("PlayCount")), MemberInt(*P, TEXT("Sequence")), !Enabled || Enabled->I != 0 });
		}
		Reply();
		return true;
	}
	if (Name == TEXT("ReceiveMapVoteCount"))
	{
		// MapVoteScore (MapIndex, GameConfigIndex, VoteCount), bReply: VotingReplicationInfo.ReceiveMapVoteCount
		if (const FUE2NetValue* P = Param(Params, 0))
		{
			const FVote V{ MemberInt(*P, TEXT("MapIndex"), -1), MemberInt(*P, TEXT("GameConfigIndex"), -1), MemberInt(*P, TEXT("VoteCount")) };
			const int32 Found = Votes.IndexOfByPredicate([&](const FVote& O) { return O.MapIndex == V.MapIndex && O.GameConfigIndex == V.GameConfigIndex; });
			if (Found == INDEX_NONE)
			{
				if (V.VoteCount > 0)
				{
					Votes.Add(V);
				}
			}
			else if (V.VoteCount <= 0)
			{
				Votes.RemoveAt(Found);
			}
			else
			{
				Votes[Found].VoteCount = V.VoteCount;
			}
		}
		const FUE2NetValue* bReply = Param(Params, 1);
		if (bReply && bReply->I != 0)
		{
			Reply();
		}
		return true;
	}
	if (Name == TEXT("OpenWindow"))
	{
		UE_LOG(LogUE2Net, Log, TEXT("MapVote: the server opens map voting (%d of %d maps, %d game types)"), Maps.Num(), MapCount, GameConfigs.Num());
		bOpenRequest = true;
		bCloseRequest = false;
		return true;
	}
	if (Name == TEXT("CloseWindow"))
	{
		bCloseRequest = true;
		bOpenRequest = false;
		CountDown = -1;
		return true;
	}
	if (Name == TEXT("PlayCountDown"))
	{
		if (const FUE2NetValue* P = Param(Params, 0))
		{
			CountDown = P->I;
			CountDownAt = FPlatformTime::Seconds();
		}
		return true;
	}
	return false;
}

int32 FUE2NetMapVote::GetSecondsLeft() const
{
	if (CountDown < 0)
	{
		return -1;
	}
	return FMath::Max(0, CountDown - int32(FPlatformTime::Seconds() - CountDownAt));
}

bool FUE2NetMapVote::ConsumeOpenRequest()
{
	const bool b = bOpenRequest;
	bOpenRequest = false;
	return b;
}

bool FUE2NetMapVote::ConsumeCloseRequest()
{
	const bool b = bCloseRequest;
	bCloseRequest = false;
	return b;
}

bool FUE2NetMapVote::SendVote(int32 MapIndex, int32 GameIndex)
{
	if (!IsAvailable() || !Maps.IsValidIndex(MapIndex) || !GameConfigs.IsValidIndex(GameIndex))
	{
		return false;
	}
	FUE2NetValue M, G;
	M.Kind = FUE2NetValue::EKind::Int;
	M.I = MapIndex;
	G.Kind = FUE2NetValue::EKind::Int;
	G.I = GameIndex;
	if (!Connection.SendFunction(Channel, TEXT("SendMapVote"), { M, G }))
	{
		return false;
	}
	MyMap = MapIndex;
	MyGame = GameIndex;
	UE_LOG(LogUE2Net, Log, TEXT("MapVote: voted for %s (%s)"), *Maps[MapIndex].Name, *GameConfigs[GameIndex].GameName);
	return true;
}

void FUE2NetMapVote::Reset()
{
	Channel = 0;
	bMapVote = false;
	MapCount = 0;
	CurrentGameConfig = 0;
	Maps.Reset();
	GameConfigs.Reset();
	Votes.Reset();
	MyMap = MyGame = CountDown = -1;
	bOpenRequest = bCloseRequest = false;
}

void FUE2NetMapVote::Reply()
{
	// the server sends the next list item only after this (VotingReplicationInfo.bWaitingForReply)
	Connection.SendFunction(Channel, TEXT("ReplicationReply"), {});
}
