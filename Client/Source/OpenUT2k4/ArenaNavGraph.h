#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaNavGraph.generated.h"

/** Actor tag: actors carrying it are ignored by the nav graph's geometry traces (non-solid mode actors). */
inline FName ArenaNavIgnoreTag()
{
	static const FName Tag(TEXT("ArenaNavIgnore"));
	return Tag;
}

class AArenaLevelBuilder;

UENUM()
enum class EArenaNavLinkType : uint8
{
	Walk,
	Jump,     // needs a (double) jump to reach
	Drop,     // walk off a ledge (one-way)
	JumpPad,  // stand on a jump pad (one-way)
	Lift,     // ride a lift (UE2 mover): wait at the bottom until it is down, stand on it until it is up (one-way)
	Teleport, // walk into a UE2 teleporter (one-way)
	ForcedJump, // UE2 ForcedPaths jump to a JumpDest (JumpDest.DoJump: the bot leaves with a computed velocity, one-way)
	Translocate // UE2 ForcedPaths to a JumpSpot taken with the translocator (JumpSpot.TryTranslocator, one-way)
};

USTRUCT()
struct FArenaNavLink
{
	GENERATED_BODY()
	UPROPERTY() int32 To = INDEX_NONE;
	UPROPERTY() EArenaNavLinkType Type = EArenaNavLinkType::Walk;
	UPROPERTY() float Cost = 0.f;
	// Index into AArenaNavGraph::ScriptedDoors when the link passes a door the map's script opens (closed: no way).
	UPROPERTY() int32 Door = INDEX_NONE;
};

USTRUCT()
struct FArenaNavNode
{
	GENERATED_BODY()
	/** Location at character centre height (floor + capsule half height) */
	UPROPERTY() FVector Location = FVector::ZeroVector;
	UPROPERTY() TArray<FArenaNavLink> Links;
	/** Lift link targets: the lift this node's Lift links ride (null otherwise). */
	UPROPERTY() TWeakObjectPtr<class AArenaMover> Lift;
};

/**
 * Waypoint graph built automatically from world collision (works for any static geometry,
 * including future imported maps). Bots path-find on it with A*.
 * Spawned by AArenaGameMode::StartPlay after the level builder; call Build().
 */
UCLASS()
class OPENUT2K4_API AArenaNavGraph : public AActor
{
	GENERATED_BODY()

public:
	AArenaNavGraph();

	/** Sample the arena bounds, create nodes on walkable floors and link them. */
	/** ExtraPoints: objective spots that must be part of the graph (from UArenaGameRules::GetNavHints). */
	void Build(const FBox& Bounds, AArenaLevelBuilder* Builder, const TArray<FVector>& ExtraPoints = TArray<FVector>());

	int32 FindNearestNode(const FVector& Location, bool bRequireVisible = true) const;
	/**
	 * A big scripted mover (AS-Convoy's crane) came to rest: the map's own navigation points around it that had no floor
	 * when the graph was built get nodes now, linked to the graph like any other node.
	 */
	void RefreshAround(const FBox& Region, const class UPrimitiveComponent* MoverFloor = nullptr);
	/**
	 * A* from Start to End. OutPath receives node locations (excluding the start position, including End).
	 * OutLinkTypes[i] is the link type used to reach OutPath[i]. Returns false if no path.
	 * bDoorsOpen: plan through the doors the map's script opens (ScriptedDoors) even while they are shut.
	 */
	bool FindPath(const FVector& Start, const FVector& End, TArray<FVector>& OutPath, TArray<EArenaNavLinkType>& OutLinkTypes,
		bool bCanTranslocate = false, bool bDoorsOpen = false) const;

	/** A Translocate link's JumpSpot (ArenaTranslocatorNav.cpp): where the beacon goes (JumpSpot.TranslocZOffset above it). */
	struct FTranslocLink
	{
		/** Node indices while the graph is built; the node locations stay valid after unreachable nodes are removed. */
		int32 From = INDEX_NONE;
		int32 To = INDEX_NONE;
		FVector FromLoc = FVector::ZeroVector;
		FVector ToLoc = FVector::ZeroVector;
		/** The JumpSpot's location and collision cylinder (cm), TranslocZOffset (cm). */
		FVector Target = FVector::ZeroVector;
		float Radius = 80.f;
		float Height = 86.f;
		float AimZ = 0.f;
		bool bOnlyTranslocator = false;
	};
	/** The Translocate link from the node at From to the node at To (nearest within 120 cm), or null. */
	const FTranslocLink* FindTranslocLink(const FVector& From, const FVector& To) const;
	int32 NumTranslocLinks() const { return TranslocLinks.Num(); }
	/** Random reachable node location (for wandering). */
	FVector GetRandomNodeLocation(FRandomStream& Rng) const;
	int32 NumNodes() const { return Nodes.Num(); }
	const TArray<FArenaNavNode>& GetNodes() const { return Nodes; }

	/**
	 * Test hook (-NavPadTest, non-shipping): for every jump pad link, plans a path from a node a few links before the
	 * pad to the pad's landing and logs whether the planner routes through the pad. Returns the number that do.
	 */
	int32 RunPadPathTest() const;

	/**
	 * Test hook (-NavCompare=<file>, non-shipping): the file lists UE2 ReachSpecs of the map ("sx sy sz ex ey ez flags
	 * start end" per line, our coordinates). Plans each and logs the ones our graph can't route, or only by a long
	 * detour. Returns the number of specs that fail.
	 */
	int32 RunCompareTest(const FString& File) const;

	/** Draw nodes/links (console: Arena.DebugNav 1) */
	void DebugDraw(float Duration) const;

protected:
	/** Redraws the graph periodically while Arena.DebugNav is enabled. */
	virtual void Tick(float DeltaSeconds) override;

	FIntVector CellOf(const FVector& Location) const;
	/** Line of sight test against world geometry only (pawns / pickups ignored). */
	bool IsClearLine(const FVector& From, const FVector& To) const;

	UPROPERTY() TArray<FArenaNavNode> Nodes;
	/** UE2 navigation points without a floor when the graph was built (on a crane or bridge a mover brings later). */
	TArray<FVector> DeferredPoints;
	/** The build's trace params (level geometry only; doors and mode actors ignored), for RefreshAround. */
	FCollisionQueryParams BuildParams;
	float RefreshTime = -100.f;
	/** Built graphs are cached per map and mode in the content cache (navcache/), keyed by everything the build reads. */
	FString CacheFile;
	bool LoadCache(const FString& File, const TArray<FString>& DoorNames);
	void SaveCache(const FString& File, const TArray<FString>& DoorNames) const;
	/** Doors opened by the map's script / objectives (UE2 BlockedPath): links through them only count while open. */
	TArray<TWeakObjectPtr<class AArenaMover>> ScriptedDoors;
	/** Translocator links (ArenaTranslocatorNav.cpp): ForcedPaths to JumpSpots; nodes are added with the special nodes,
	 *  links after the forced jumps. */
	void CollectTranslocatorSpots(TFunctionRef<int32(const FVector&)> AddNode);
	void AddTranslocatorLinks();
	TArray<FTranslocLink> TranslocLinks;
	/** Spatial hash cell -> node indices for fast nearest queries */
	TMap<FIntVector, TArray<int32>> Cells;
	float CellSize = 600.f;
};
