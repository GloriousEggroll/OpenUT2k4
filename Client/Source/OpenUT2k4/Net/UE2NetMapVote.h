#pragma once

#include "CoreMinimal.h"
#include "UE2NetConnection.h"

struct FUSFunction;

/**
 * UT2004 map voting (xVoting) on a UE2 server: our side of the player's xVoting.VotingReplicationInfo.
 *
 * The server sends the game types (ReceiveGameConfig), the map list (ReceiveMapInfo) and the vote counts
 * (ReceiveMapVoteCount) one call per tick, each waiting for our ReplicationReply. At the end of a match it calls
 * OpenWindow (the stock client opens MapVotingPage) and counts down (PlayCountDown at 60 / 30 / 20 / 10 s left); a vote
 * goes back with SendMapVote(MapIndex, GameConfigIndex). The mid-game menu draws the vote page (ArenaMidGameMenu).
 * Nothing of xVoting's code runs here: the calls are decoded from the class metadata and answered natively.
 */
class FUE2NetMapVote
{
public:
	struct FMap
	{
		FString Name;
		int32 PlayCount = 0;
		int32 Sequence = 0;
		bool bEnabled = true;
	};
	struct FGameConfig
	{
		FString GameClass;
		FString Prefix;
		FString GameName;
	};
	struct FVote
	{
		int32 MapIndex = -1;
		int32 GameConfigIndex = -1;
		int32 VoteCount = 0;
	};

	explicit FUE2NetMapVote(FUE2NetConnection& InConnection) : Connection(InConnection) {}

	void ActorOpened(const FUE2NetActor& Actor);
	void ActorUpdated(const FUE2NetActor& Actor, const TArray<FString>& Changed);
	void ActorClosed(const FUE2NetActor& Actor);
	/** Handles the VotingReplicationInfo's client calls; returns true when the call was one of them. */
	bool FunctionCall(const FUE2NetActor& Actor, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params);

	/** The server runs map voting (bMapVote) and its VotingReplicationInfo reached us. */
	bool IsAvailable() const { return Channel != 0 && bMapVote; }
	const TArray<FMap>& GetMaps() const { return Maps; }
	const TArray<FGameConfig>& GetGameConfigs() const { return GameConfigs; }
	const TArray<FVote>& GetVotes() const { return Votes; }
	/** The number of maps the server announced (MapCount): the list is complete when GetMaps() has them all. */
	int32 GetMapCount() const { return MapCount; }
	int32 GetCurrentGameConfig() const { return CurrentGameConfig; }
	int32 GetMyMap() const { return MyMap; }
	int32 GetMyGame() const { return MyGame; }
	/** Seconds left to vote (from the server's last count down call), or -1 when no count down runs. */
	int32 GetSecondsLeft() const;
	/** True once after the server asked to open the voting window (OpenWindow), false after CloseWindow. */
	bool ConsumeOpenRequest();
	bool ConsumeCloseRequest();

	/** Votes for MapIndex (into GetMaps()) with the game type GameIndex (into GetGameConfigs()). */
	bool SendVote(int32 MapIndex, int32 GameIndex);

private:
	/** Makes Actor (a VotingReplicationInfo) the one we answer. */
	void Claim(const FUE2NetActor& Actor);
	void Reset();
	void Reply();

	FUE2NetConnection& Connection;
	int32 Channel = 0;
	bool bMapVote = false;
	int32 MapCount = 0;
	int32 CurrentGameConfig = 0;
	TArray<FMap> Maps;
	TArray<FGameConfig> GameConfigs;
	TArray<FVote> Votes;
	int32 MyMap = -1;
	int32 MyGame = -1;
	int32 CountDown = -1;
	double CountDownAt = 0.0;
	bool bOpenRequest = false;
	bool bCloseRequest = false;
};
