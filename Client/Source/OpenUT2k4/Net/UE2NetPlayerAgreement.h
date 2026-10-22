#pragma once

#include "CoreMinimal.h"
#include "UE2NetConnection.h"

struct FUSFunction;

/**
 * A server's player agreement (DruidsPlayerAgreement: PlayerAgreementManager) on a UE2 server: our side of the manager
 * the server spawns for us.
 *
 * Until we answer, the server holds us as a spectator (PlayerAgreementManager.setup: bOnlySpectator, state
 * Spectating), so Fire does not let us join. It calls PlayerAgreement (the stock client opens the mod's
 * PlayerAgreementPage and calls showPage back), then addToAgreement once per line (an empty line is a paragraph
 * break) and doneAdding(YesButtonEnableTime): the page's "I agree!" button becomes usable that many seconds later.
 * The answer goes back with yes() (we may play: PlayerWaiting, Fire joins) or no() (the server kicks us); the server
 * kicks us too when its Timeout runs out without an answer. ForceCloseMenu closes the page. Nothing of the mod's
 * code runs here: the calls are decoded from the class metadata and answered natively; the mid-game menu draws the
 * page (ArenaMidGameMenu).
 */
class FUE2NetPlayerAgreement
{
public:
	explicit FUE2NetPlayerAgreement(FUE2NetConnection& InConnection) : Connection(InConnection) {}

	void ActorClosed(const FUE2NetActor& Actor);
	/** Handles the manager's client calls; returns true when the call was one of them. */
	bool FunctionCall(const FUE2NetActor& Actor, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params);

	/** The page is up: the server waits for our answer. */
	bool IsOpen() const { return Channel != 0 && bOpen; }
	/** The agreement's lines (empty: a paragraph break). */
	const TArray<FString>& GetLines() const { return Lines; }
	/** Seconds until "I agree!" may be pressed (0: now), or -1 while the text is still coming. */
	int32 GetSecondsUntilYes() const;
	/** Answers: yes (we may play) or no (the server kicks us). */
	bool Answer(bool bYes);

private:
	void Reset();

	FUE2NetConnection& Connection;
	int32 Channel = 0;
	bool bOpen = false;
	TArray<FString> Lines;
	double YesAt = -1.0;
};
