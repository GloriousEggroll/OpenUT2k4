#pragma once

#include "CoreMinimal.h"

class UStaticMeshComponent;
class UWorld;

/**
 * Runtime map building: turns a converted UT2004 map from the content cache (maps/<Map>/map.json + bsp.gltf,
 * see ArenaContent.h) into level geometry inside the shipped base level /Game/Maps/Arena, the same way
 * Tools/ImportUT2004.py builds the cooked level /Game/Imported/Maps/<Map>:
 *   - BSP (and terrain) split into XY chunks of static meshes with complex-as-simple collision
 *     (NotSolid polygons do not collide; TwoSided / Unlit polygons use those material variants),
 *   - static mesh actors with their transform (pre pivot, draw scale), skins and collision flags;
 *     the UE2 world actors (movers, zones, volumes, triggers, emitters, sky box) come from ArenaUE2World,
 *   - UT2004 lights as unshadowed movable point / spot lights (unitless, gentle falloff),
 *   - sky: sun, sky light, atmosphere, height fog and the unbound post process volume.
 * Meshes, materials and textures come from the runtime asset provider (ArenaRuntimeContent).
 * The marker json part (player starts, pickups, anchors) and the lighting corrections stay with
 * AArenaLevelBuilder::BuildImported / ArenaImportedLighting, which run afterwards as for cooked maps.
 */
namespace ArenaRuntimeMap
{
	/** <cache>/maps/<MapName>. */
	OPENUT2K4_API FString GetMapDir(const FString& MapName);
	/** True if the cache has maps/<MapName>/map.json. */
	OPENUT2K4_API bool HasMap(const FString& MapName);

	/**
	 * Builds the map's geometry, lights and sky into World (call before BeginPlay, e.g. from InitGame).
	 * Blocks while it loads (a few seconds for big maps; logged with timings). False if map.json is unusable.
	 */
	OPENUT2K4_API bool BuildWorld(UWorld* World, const FString& MapName);

	/**
	 * UE2 static mesh collision on a blocking mesh component (map mesh actors, movers): UStaticMesh::LineCheck uses
	 * the mesh's collision model (Model: never drawn, from ArenaRuntimeContent::LoadStaticMeshCollisionModel, may be
	 * null) for extent traces when bModelBox (players: Pawn, WorldStatic probes, WorldDynamic) and for zero extent
	 * ones when bModelLine (Visibility, Camera, Weapon, Projectile), else the colliding sections of Visual; the actor's
	 * bBlockNonZeroExtentTraces / bBlockZeroExtentTraces = false let those through. Physics bodies (Vehicle,
	 * PhysicsBody, Destructible) collide with Visual, or with the model when Visual has no colliding section.
	 * A component left with nothing to block gets no collision.
	 */
	OPENUT2K4_API void ApplyUE2MeshCollision(UStaticMeshComponent* Visual, UStaticMeshComponent* Model, bool bModelBox, bool bModelLine,
		bool bBlockNonZeroExtent, bool bBlockZeroExtent);
}
