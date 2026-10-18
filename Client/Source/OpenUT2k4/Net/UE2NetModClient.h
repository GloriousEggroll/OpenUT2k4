#pragma once

#include "CoreMinimal.h"

class FUE2NetConnection;
class UWorld;
struct FUE2NetActor;
struct FUE2NetValue;
struct FUSFunction;
struct FUSClass;

/**
 * Client-side parts of common server mods, done the way their client code does them (Docs/UE2Net.md "Mod
 * client-side code"). Their settings come from the mods' own classes (class defaults and the player's config, read
 * through the script runtime), their sounds are converted from the mods' packages on demand (UE2NetModContent):
 *
 *  - hit sounds: UTPlus (UTPlusHitSoundTag.HitSoundRepInfo, replicated to its owner: PlayHitSound with
 *    UTPlusSettings' sound, volume and damage pitch) and UTComp (BS_xPlayer.ReceiveHitSound / ReceiveHit ->
 *    PlayEnemyHitSound / PlayTeammateHitSound with UTComp_Settings' EnemySound / FriendlySound, HitSoundVolume,
 *    CPMA style pitch)
 *  - ready-up: UTComp's warmup (UTComp_PRI.Ready / NotReady, bIsReady), instead of the stock ServerSetReadyToStart
 *  - warmup state for the HUD (UTComp_Warmup, BS_xPlayer.NotifyEndWarmup)
 */
class OPENUT2K4_API FUE2NetModClient
{
public:
	FUE2NetModClient(FUE2NetConnection& InConnection, UWorld* InWorld);

	void ActorUpdated(const FUE2NetActor& A, const TArray<FString>& Changed, bool bOpened, int32 OwnPRIChannel);
	/** True when the call was handled here. */
	bool FunctionCall(const FUE2NetActor& A, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params, int32 OwnPRIChannel);

	/** The mod's ready-up call when the server has one (UTComp_PRI.Ready / NotReady). False: use the stock way. */
	bool SetReady(bool bReady, int32 OwnPRIChannel);
	/** "Warmup: 2 of 4 ready" style line while a mod's warmup runs, else empty. */
	FString DescribeWarmup() const;
	FString Describe() const;

private:
	FUE2NetConnection& Connection;
	TWeakObjectPtr<UWorld> World;

	TMap<int32, int32> LastUTPlusHit;
	double NextUTCompHitSound = 0.0;
	int32 HitSoundsPlayed = 0;
	int32 HitSoundEvents = 0;
	int32 ReadyCalls = 0;
	bool bWarmup = false;
	bool bWarmupEnded = false;

	/** Plays a sound object of a mod package as UE2's ViewTarget.PlaySound does for the local player (2D). */
	void PlayLocal(const FString& SoundPath, float Volume, float Pitch);
	int32 FindLinkedInfo(int32 OwnPRIChannel, const TCHAR* ClassName) const;
	int32 TeamOfPawnChannel(int32 PawnChannel) const;
};
