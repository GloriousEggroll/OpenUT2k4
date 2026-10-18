#pragma once

#include "CoreMinimal.h"

class FUE2NetConnection;
struct FUE2NetActor;
struct FUE2NetValue;
struct FUSFunction;

/**
 * "NewNet" (UTComp's lag compensated weapons, also in its forks, such as WSUTComp) as a UE2 client speaks it.
 * See Docs/UE2Net.md "NewNet". Worked out from the mods' scripts (the packages carry their source):
 *
 *  - the server keeps a time stamp and tells every client the current one: a NewNet_TimeStamp actor with a float
 *    ClientTimeStamp (UTComp 1.7 and its forks), or a TimeStamp_Pawn whose replicated rotation is a counter that
 *    goes up by one each server tick (UTComp 1.8+ / WSUTComp: yaw byte = counter % 256), plus the client's own DT
 *    since that counter last changed
 *  - a NewNet weapon's client fire (NewNet_ClientStartFire) sends NewNet_ServerStartFire(Mode, <the last stamp
 *    received>[, DT][, avgPing][, R, V][, bBelievesHit, A]) instead of ServerStartFire; R is the view (yaw, pitch),
 *    V the eye location, bBelievesHit / A what the client's own trace along the view hit (a pawn or vehicle).
 *    The server turns the stamp into how long ago the client saw the world (PingDT) and traces against the
 *    players' positions of that time (NewNet_PawnCollisionCopy)
 *  - NewNet_OldServerStartFire(Mode, stamp[, DT]) when the weapon starts firing while not ready (exclusive modes)
 *  - NewNet_PRI.Ping / Pong: the client measures its round trip (PredictedPing; avgPing above)
 *
 * The parameters are filled by name from the weapon class's own declaration, so the forks' variants all work.
 */
class OPENUT2K4_API FUE2NetNewNet
{
public:
	explicit FUE2NetNewNet(FUE2NetConnection& InConnection);

	void ActorUpdated(const FUE2NetActor& A, const TArray<FString>& Changed, double GameTime);
	void ActorClosed(const FUE2NetActor& A);
	/** True when the call was NewNet's (Pong). */
	bool FunctionCall(const FUE2NetActor& A, FUSFunction* Function, double GameTime);
	/** Every frame (game seconds): DT of the pawn stamp, pings. */
	void Tick(double GameTime, float GameDelta, int32 OwnPRIChannel);

	/** The weapon channel's class fires the NewNet way (it has a replicated NewNet_ServerStartFire). */
	bool IsNewNetWeapon(int32 WeaponChannel) const;
	/** The weapon's OldServerStartFire exists (it starts firing while not ready that way). */
	bool HasOldStartFire(int32 WeaponChannel) const;

	struct FFire
	{
		uint8 Mode = 0;
		/** View (UE2 units, 65536 per turn) and eye location (UU). */
		int32 Yaw = 0;
		int32 Pitch = 0;
		FVector EyeUU = FVector::ZeroVector;
		/** The client's own trace along the view hit a pawn or vehicle: its channel (0: none). */
		int32 HitChannel = 0;
		bool bOld = false;
	};
	/** Sends the weapon's NewNet start fire. False when the weapon is not a NewNet one (send ServerStartFire then). */
	bool SendStartFire(int32 WeaponChannel, const FFire& Fire);

	/** A time stamp from the server was seen (NewNet is running there). */
	bool HasStamp() const { return bHaveFloatStamp || bHavePawnStamp; }
	FString Describe() const;

private:
	FUE2NetConnection& Connection;

	// UTComp 1.7 style: NewNet_TimeStamp.ClientTimeStamp (float), the last value received
	int32 FloatStampChannel = 0;
	bool bHaveFloatStamp = false;
	float FloatStamp = 0.f;
	// UTComp 1.8+: TimeStamp_Pawn (rotation counter) and the client's DT since it last changed
	int32 PawnStampChannel = 0;
	bool bHavePawnStamp = false;
	int32 PawnStamp = 0;
	float PawnStampDT = 0.f;

	// NewNet_PRI ping (PredictedPing)
	int32 PingChannel = 0;
	double PingSendTime = 0.0;
	bool bPingReceived = true;
	int32 NumPings = 0;
	float PredictedPing = 0.f;
	/** NewNet_PRI.PingTweenTime (UTComp 1.8+) or 0 (float-stamp forks: const 0.5, a 10 sample average). */
	float PingTweenTime = 0.f;

	// statistics
	int32 NumFires = 0;
	int32 NumOldFires = 0;
	int32 NumBelievedHits = 0;
	int32 NumPongs = 0;
	TSet<FName> WarnedParams;

	int32 FindOwnNewNetPRI(int32 OwnPRIChannel) const;
};
