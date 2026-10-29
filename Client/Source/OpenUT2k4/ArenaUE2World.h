#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/Interface.h"
#include "ArenaUE2World.generated.h"

class APawn;
class UWorld;

/**
 * UE2 world behaviour, native: the map actors that make a UT2004 level behave like the original (zones, physics
 * volumes, blocking volumes, movers, triggers and the Tag / Event system, teleporters, emitters, ambient sounds,
 * the 3D sky box) are spawned from map.json as C++ actors with the UE2 property names mapped
 * (ArenaWorldZones.h, ArenaMover.h, ArenaTriggers.h, ArenaEmitters.h, ArenaSkybox.h).
 *
 * Data: every map.json actor carries "properties" (its own UE2 values) and the converter writes the merged UE2
 * class defaults of each world behaviour class to "class_defaults"; FArenaUE2Props merges both, so a native actor
 * reads exactly what UE2 would see (e.g. a WaterVolume's FluidFriction 2.4 from the user's Gameplay.u).
 *
 * Extending (UnrealScript VM, mods): ArenaUE2World::RegisterClass maps a UE2 class name to a native class (the
 * most derived registered class of an actor's class_chain wins), and every native actor implements
 * IArenaUE2Actor (InitFromUE2, Trigger / UnTrigger), so a script class can subclass a native one and call into it.
 */

/** UE2 values of one object: class defaults merged with the instance's own properties (raw UE2 units). */
struct OPENUT2K4_API FArenaUE2Props
{
	TSharedPtr<FJsonObject> Obj;

	/** Defaults first, then Instance on top (either may be null). */
	static FArenaUE2Props Merge(const TSharedPtr<FJsonObject>& Defaults, const TSharedPtr<FJsonObject>& Instance);

	bool Has(const FString& Key) const;
	TSharedPtr<FJsonValue> Value(const FString& Key) const;
	/** Element Index of a UE2 static array ("Key" for 0, "Key[i]" otherwise). */
	TSharedPtr<FJsonValue> Indexed(const FString& Key, int32 Index) const;
	double Num(const FString& Key, double Default = 0.0) const;
	bool Bool(const FString& Key, bool bDefault = false) const;
	/** Strings, names and object paths; "None" and missing give Default. */
	FString Str(const FString& Key, const FString& Default = FString()) const;
	FName Name(const FString& Key) const;
	/** [x, y, z] (raw UE2 units; multiply by ArenaUE2World::Scale for centimetres). */
	FVector Vec(const FString& Key, const FVector& Default = FVector::ZeroVector) const;
	/** {"Pitch","Yaw","Roll"} in UE2 units (65536 per turn) -> degrees. */
	FRotator Rot(const FString& Key, const FRotator& Default = FRotator::ZeroRotator) const;
	/** {"R","G","B","A"}. */
	FColor Color(const FString& Key, const FColor& Default = FColor::White) const;
	/** {"Min","Max"} -> (X = Min, Y = Max); a plain number gives (n, n). */
	FVector2D Range(const FString& Key, const FVector2D& Default = FVector2D::ZeroVector) const;
	/** {"X":{Min,Max},"Y":..,"Z":..} -> Min / Max vectors. */
	void RangeVector(const FString& Key, FVector& OutMin, FVector& OutMax, const FVector& Default = FVector::ZeroVector) const;
	TSharedPtr<FJsonObject> Object(const FString& Key) const;
	const TArray<TSharedPtr<FJsonValue>>& Array(const FString& Key) const;

	static FVector ToVector(const TSharedPtr<FJsonValue>& V, const FVector& Default = FVector::ZeroVector);
	static FRotator ToRotator(const TSharedPtr<FJsonValue>& V);
	static FString ToString(const TSharedPtr<FJsonValue>& V);
};

/** One map.json actor as seen by the native classes. Positions are in centimetres (UE coordinates). */
struct OPENUT2K4_API FArenaUE2ActorDesc
{
	int32 Index = INDEX_NONE;
	FString Name;
	FString Class;
	/** Most derived first ("WaterVolume", "PhysicsVolume", "Volume", "Brush", "Actor", "Object"). */
	TArray<FString> ClassChain;
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	float DrawScale = 1.f;
	FVector DrawScale3D = FVector::OneVector;
	/** Pre pivot in centimetres. */
	FVector PrePivot = FVector::ZeroVector;
	/** "Pkg.Group.Name" or empty. */
	FString StaticMesh;
	TArray<FString> Skins;
	FName Tag;
	FName Event;
	bool bHidden = false;
	float CollisionRadius = 0.f;
	float CollisionHeight = 0.f;
	FArenaUE2Props Props;
	/** The whole map.json actor record. */
	TSharedPtr<FJsonObject> Json;
	/** The map's volume / mover / zone marker for this actor, when the converter wrote one (may be null). */
	TSharedPtr<FJsonObject> Marker;

	bool IsA(const FString& UE2Class) const;
	/** Inline subobject (emitter, scripted action) by the reference string used in Props, merged with its class defaults. */
	bool GetSubobject(const FString& Ref, FString& OutClass, TArray<FString>& OutChain, FArenaUE2Props& OutProps) const;
	/** Static mesh transform: world = Location + R * (scale * (vertex - PrePivot)). */
	FTransform MeshTransform(const FVector& InLocation, const FRotator& InRotation) const;

	/** Class defaults of the whole map (map.json "class_defaults"). */
	TSharedPtr<FJsonObject> ClassDefaults;
};

UINTERFACE(MinimalAPI)
class UArenaUE2Actor : public UInterface
{
	GENERATED_BODY()
};

/** Implemented by every native UE2 world actor. */
class OPENUT2K4_API IArenaUE2Actor
{
	GENERATED_BODY()

public:
	/** Called once right after spawning (before BeginPlay) with the actor's map data. */
	virtual void InitFromUE2(const FArenaUE2ActorDesc& Desc) {}
	/** Called after every map actor exists (resolve references to other actors by tag / name). */
	virtual void PostInitUE2() {}
	/** UE2 Actor.Tag (events with this name reach the actor) and Actor.Event (what it fires). */
	virtual FName GetUE2Tag() const { return NAME_None; }
	virtual FName GetUE2Event() const { return NAME_None; }
	/** UE2 Actor.Trigger / UnTrigger: an event named like our Tag fired. */
	virtual void Trigger(AActor* Other, APawn* EventInstigator) {}
	virtual void UnTrigger(AActor* Other, APawn* EventInstigator) {}
	/** UE2 Actor.Reset: back to the state at map start (new Assault round). */
	virtual void ResetUE2() {}
};

DECLARE_MULTICAST_DELEGATE_FourParams(FArenaUE2EventDelegate, FName /*Event*/, AActor* /*Other*/, APawn* /*EventInstigator*/, bool /*bTrigger (false: UnTrigger)*/);

/**
 * The UE2 event system (Actor.TriggerEvent / UntriggerEvent): an event name reaches every registered actor whose
 * Tag equals it. Native UE2 actors register themselves; other game code (game rules, objectives) can register any
 * actor implementing IArenaUE2Actor, or listen to every event with OnEvent.
 */
UCLASS()
class OPENUT2K4_API UArenaEventSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	static UArenaEventSubsystem* Get(const UObject* WorldContext);

	/** Actor must implement IArenaUE2Actor. Tag NAME_None registers nothing. */
	void RegisterActor(AActor* Actor, FName Tag);
	void UnregisterActor(AActor* Actor);
	/** UE2 TriggerEvent: Trigger(Other, EventInstigator) on every actor tagged Event (None / empty does nothing). */
	void TriggerEvent(FName Event, AActor* Other, APawn* EventInstigator);
	void UntriggerEvent(FName Event, AActor* Other, APawn* EventInstigator);
	/** Registered actors with this tag. */
	TArray<AActor*> GetTaggedActors(FName Tag) const;
	/** Every event (after the tagged actors were told). */
	FArenaUE2EventDelegate OnEvent;
	/** Development log of every event (-LogUE2Events or arena.LogUE2Events 1). */
	static bool IsLogging();

private:
	TMap<FName, TArray<TWeakObjectPtr<AActor>>> Tagged;
	int32 Depth = 0;
};

namespace ArenaUE2World
{
	/** UE2 unit -> centimetres (the converter's scale). */
	constexpr float Scale = 2.f;

	/**
	 * True in a world that shows a UT2004 server's game (?UE2Net=1, Net/UE2NetWorld.h). Its gameplay is the server's:
	 * pickups, damage, deaths, events, triggers and teleporters are not run locally there (Docs/UE2Net.md "Offline
	 * gameplay in server worlds"). The world is standalone, so HasAuthority() is true there and does not tell.
	 */
	OPENUT2K4_API bool IsUE2NetWorld(const UWorld* World);
	OPENUT2K4_API bool IsUE2NetWorld(const UObject* WorldContext);
	/** UE2 LevelInfo.DefaultGravity of stock maps: our world gravity stands for this value. */
	constexpr float UE2DefaultGravity = -950.f;

	/**
	 * Native class for a UE2 class name (Class must implement IArenaUE2Actor). Later registrations of the same
	 * name replace earlier ones. Built-in classes are registered on first use.
	 */
	OPENUT2K4_API void RegisterClass(const FString& UE2Class, UClass* NativeClass);
	/** Native class for an actor's class chain (most derived registered entry), or null. */
	OPENUT2K4_API UClass* FindClass(const TArray<FString>& ClassChain);

	/**
	 * Spawns the native UE2 world actors of a map (runtime maps: called by ArenaRuntimeMap::BuildWorld). Doc is the
	 * parsed map.json, MapDir its folder (bsptree.bin). Returns the map.json indices of actors that were taken
	 * over (e.g. movers: the map builder must not also place them as static meshes).
	 */
	OPENUT2K4_API TSet<int32> SpawnWorldActors(UWorld* World, const TSharedPtr<FJsonObject>& Doc, const FString& MapDir);

	/** Indices of the actors SpawnWorldActors would take over (so the map builder can skip them before spawning). */
	OPENUT2K4_API TSet<int32> GetHandledActorIndices(const TSharedPtr<FJsonObject>& Doc);

	/** True when the world's movers are native (lifts really move; the converted "Lift:" jump pads are not needed). */
	OPENUT2K4_API bool HasNativeMovers(const UWorld* World);

	/** Calls ResetUE2 on every native UE2 actor of the world (new rounds). */
	OPENUT2K4_API void ResetWorldActors(UWorld* World);

	/** Actor by its map.json name ("Mover3") among the spawned native actors, or null. */
	OPENUT2K4_API AActor* FindActorByName(const UWorld* World, const FString& UE2Name);

	/** A UE2 JumpPad of the map (recorded by SpawnWorldActors; the jump pads themselves are AArenaLevelBuilder's). */
	struct FJumpPadInfo
	{
		FString Name;
		/** UE2 actor location (the trigger cylinder's centre), cm. */
		FVector Location = FVector::ZeroVector;
		/** CollisionRadius / CollisionHeight in cm (UE2 defaults when the map has none). */
		float Radius = 80.f;
		float HalfHeight = 86.f;
		/** UE2 JumpVelocity in cm/s (zero when the map has none). */
		FVector JumpVelocity = FVector::ZeroVector;
	};
	/** The map's UE2 jump pad closest to Location within MaxDist (cm), or null. */
	OPENUT2K4_API const FJumpPadInfo* FindJumpPad(const UWorld* World, const FVector& Location, float MaxDist = 50.f);

	/** A UE2 NavigationPoint.ForcedPaths entry (a path the map's author forced, e.g. a long jump to a JumpSpot), cm. */
	struct FForcedPath
	{
		FVector From = FVector::ZeroVector;
		FVector To = FVector::ZeroVector;
		/** The target is a JumpDest (JumpSpot, xBombDelivery ...): bots jump there (JumpDest.DoJump). */
		bool bToJumpDest = false;
		/** The target JumpSpot's translocator settings (JumpSpot.bOnlyTranslocator / bNoLowGrav, TranslocZOffset in cm). */
		bool bOnlyTranslocator = false;
		bool bNoLowGrav = false;
		float TranslocZOffset = 0.f;
		/** The target's collision cylinder (cm; UE2 NavigationPoint defaults 40 x 43 UU when the map has none). */
		float TargetRadius = 80.f;
		float TargetHeight = 86.f;
	};
	/** The map's ForcedPaths (recorded by SpawnWorldActors), empty if none. */
	OPENUT2K4_API TArray<FForcedPath> GetForcedPaths(const UWorld* World);

	/** A UE2 AssaultPath (alternate route the squad AI picks to reach an objective, CTF flag bases too), cm. */
	struct FAssaultPath
	{
		FVector Location = FVector::ZeroVector;
		/** Location of the GameObjective whose Tag is the path's ObjectiveTag (zero when none was found). */
		FVector ObjectiveLocation = FVector::ZeroVector;
		bool bHasObjective = false;
		/** Only on the way back from the objective (a flag carrier going home) / never on the way back. */
		bool bReturnOnly = false;
		bool bNoReturn = false;
		/** AssaultPath.Priority: the selection weight. */
		float Priority = 0.5f;
	};
	/** The map's AssaultPaths (recorded by SpawnWorldActors), empty if none. */
	OPENUT2K4_API TArray<FAssaultPath> GetAssaultPaths(const UWorld* World);
	/** Movers the map's UE2 Door / AutoDoor navigation points name as their MyDoor (doors bots go through). */
	OPENUT2K4_API TArray<FString> GetNavDoorMovers(const UWorld* World);
}
