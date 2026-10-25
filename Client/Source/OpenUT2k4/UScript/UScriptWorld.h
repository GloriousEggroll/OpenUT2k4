#pragma once

#include "CoreMinimal.h"
#include "UScriptCore.h"

/**
 * A set of live script object instances (one per map, plus a transient world for tests and non-world objects).
 * Owns every instance it creates; destroying the world frees them. Actor support (spawning, ticking, timers,
 * the UE5 bridge) lives in FUSGameWorld (UScriptBridge.h).
 */
class FUSWorld
{
public:
	FUSWorld();
	virtual ~FUSWorld();
	FUSWorld(const FUSWorld&) = delete;
	FUSWorld& operator=(const FUSWorld&) = delete;

	/** Creates an instance: memory initialised from the class defaults. Does not call any event. */
	FUSObject* ConstructObject(FUSClass* Class, FUSObject* Outer = nullptr, FName Name = NAME_None);
	/** Marks an object dead; references are cleared by CleanupDestroyed. */
	void MarkPendingKill(FUSObject* Obj);
	/** UE2 CleanupDestroyed: clears references to dead objects and frees them. */
	void CleanupDestroyed();
	/** Every live instance. */
	const TArray<FUSObject*>& GetObjects() const { return Objects; }

	/** Spawn an actor (overridden by the game world; the base version only constructs it). */
	virtual FUSObject* SpawnActor(FUSClass* Class, FUSObject* Owner, FName Tag, const FUSVector* Location, const FUSRotator* Rotation, FUSObject* Spawner);
	/** Destroy an actor (Destroyed event + pending kill). */
	virtual bool DestroyActor(FUSObject* Actor);

	/** The game world (FUSGameWorld) when this world is one; other worlds (Transient, the UE2 client mirror) return null. */
	virtual class FUSGameWorld* AsGameWorld() { return nullptr; }
	/** LevelInfo.GetLocalPlayerController of worlds that are not game worlds (the UE2 client mirror). */
	virtual FUSObject* GetLocalPlayerController() { return nullptr; }
	/** The UE world sounds, traces and effects of a world that is not a game world go to (the UE2 client mirror's map). */
	virtual class UWorld* GetPresentationWorld() { return nullptr; }
	/** Where an actor of such a world is shown (UE coordinates), when it has a native counterpart. */
	virtual bool GetPresentationLocation(FUSObject* Actor, FVector& OutLocation) { return false; }
	/** A bone (or attach socket) of what shows an actor of such a world, world space (UE coordinates). */
	virtual bool GetPresentationBone(FUSObject* Actor, FName Bone, FTransform& OutTransform) { return false; }

	/** Seconds since the world started (Level.TimeSeconds). */
	float TimeSeconds = 0.f;

	/** World for objects created outside a map (tests, static calls). */
	static FUSWorld& Transient();

protected:
	TArray<FUSObject*> Objects;
	int32 PendingKillCount = 0;
	TMap<FName, int32> NameCounters;
};
