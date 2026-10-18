#pragma once

#include "CoreMinimal.h"

class FUE2NetConnection;
struct FUE2NetActor;

/**
 * The client side of the OpenUT2k4 Validator (Server/Docs/Validator.md, Docs/UE2Net.md "OpenUT2k4 Validator").
 *
 * - Honest identification: every login URL carries ?OpenUT2k4=<version>. Our client never claims to be the stock
 *   client and never runs, answers or imitates any third-party anti-cheat.
 * - The player key: an RSA-2048 key made silently on first start (in the background, no prompt) and kept in the
 *   player's profile (<Saved>/Identity/PlayerKey.txt, mode 0600 where the OS has it). A missing or damaged file is
 *   replaced by a new key. It proves "the same player as before" to servers that run the validator; it says nothing
 *   about the build.
 * - Answering: a validator challenge (the OpenUT2k4Validator.OUTValidatorLink actor of a stock server running the
 *   mutator, or an OUTVALIDATE control message of our own server) is signed with the player key:
 *   "OUTR2;<dialed ip:port[/host]>;<public key hex>;<signature of OUT2K4RESP2;<challenge>;<dialed>>", sent in parts.
 *   The dialed address is the one we really sent to (a relaying server cannot pass another server's challenge on
 *   unnoticed), and a challenge that names another port than the one we dialed is refused. Nothing else is signed.
 *
 * Test switches (not in Shipping): -UE2PlayerKey=<file> (another key file), -UE2ValidatorSelfTest (crypto test
 * vectors at startup), -UE2ValidatorTest=badsig|wrongkey|nokey|silent|record=<file>|replay=<file>|dialport=<n>|relaydialed=<ip:port> (wrong answers
 * for the validator's tests).
 */
namespace UE2NetValidator
{
	/** "?OpenUT2k4=<version>" for the login URL. */
	OPENUT2K4_API FString GetLoginOption();
	/** Loads the player key, or makes one in the background. Safe to call often. */
	OPENUT2K4_API void EnsurePlayerKey();
	/** The player key's fingerprint (32 hex digits), empty while it is being made. */
	OPENUT2K4_API FString GetFingerprint();
	/** Where the player key lives. */
	OPENUT2K4_API FString GetKeyPath();

	// ---- hooks of FUE2NetConnection ----
	/** An actor was opened or updated: answers an OUTValidatorLink's ChallengeText. */
	void ActorChanged(FUE2NetConnection& Connection, const FUE2NetActor& Actor);
	/** "OUTVALIDATE ..." control message (our own server's native validator). */
	void ReceivedControl(FUE2NetConnection& Connection, const TCHAR* Args);
	/** Answers a challenge that waited for the key. */
	void Tick(FUE2NetConnection& Connection);
	/** The connection is going away. */
	void ConnectionClosed(FUE2NetConnection& Connection);
}
