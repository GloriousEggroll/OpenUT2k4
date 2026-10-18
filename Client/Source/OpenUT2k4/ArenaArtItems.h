#pragma once

#include "CoreMinimal.h"

class AActor;
class UClass;
class UMaterialInterface;
class USceneComponent;
class UStaticMesh;
class UStaticMeshComponent;
enum class EArenaPickupType : uint8;

/**
 * Imported pickup and game-object art (tables in ArenaArtItems.cpp), loaded by soft path through ArenaArt
 * (Replacements, then Imported). Every caller keeps its procedural model when a mesh is missing or
 * arena.ImportedArt is 0.
 */
namespace ArenaArtItems
{
	/** A static mesh fitted by its bounds. */
	struct FItemMesh
	{
		/** "StaticMeshes/<Pkg>/<Name>"; Fallback is tried when it is missing. */
		const TCHAR* Mesh = nullptr;
		const TCHAR* Fallback = nullptr;
		/** Largest dimension in cm. */
		float Size = 50.f;
		FRotator Rotation = FRotator::ZeroRotator;
		FVector Offset = FVector::ZeroVector;
		/** Bounds bottom on the origin (bases, pads) instead of the bounds centre. */
		bool bBottom = false;
	};

	struct FPickupArt
	{
		FItemMesh Item;
		/** Pickup base / charger under the item (Mesh nullptr = none). */
		FItemMesh Base;
		/** Show the base when the map does not say whether the item had one (procedural layouts). */
		bool bBaseByDefault = false;
	};

	/** Art for a pickup (weapon / ammo by weapon class, walking up the hierarchy). nullptr if none. */
	OPENUT2K4_API const FPickupArt* FindPickupArt(EArenaPickupType Type, const UClass* WeaponClass);

	/** Loads Mesh (or Fallback). nullptr when both are missing or art is disabled. */
	OPENUT2K4_API UStaticMesh* LoadMesh(const FItemMesh& M);
	/** Adds the mesh under Parent, fitted and placed. nullptr when the mesh is missing. */
	OPENUT2K4_API UStaticMeshComponent* AddItemMesh(AActor* Owner, USceneComponent* Parent, const FItemMesh& M, float ScaleMul = 1.f, bool bShadow = false);
	/** Fits an existing component (largest dimension = Size, centred or standing on the origin). */
	OPENUT2K4_API void FitItemMesh(UStaticMeshComponent* Comp, const FItemMesh& M, float ScaleMul = 1.f);
	/** Soft-loaded material ("Materials/<Pkg>/<Name>"), nullptr if missing. */
	OPENUT2K4_API UMaterialInterface* LoadMaterial(const TCHAR* Path);

	// ---- game objects (paths; see ArenaArtItems.cpp) ----

	/** CTF: waving flag (vertex-animated mesh), its team materials and loop, and the flag base. */
	struct FFlagArt
	{
		const TCHAR* Mesh;
		const TCHAR* Anim;
		const TCHAR* TeamMaterial[2];
		const TCHAR* TeamMaterialFallback[2];
		/** Height of the flag (pole) in cm. */
		float Height;
		FItemMesh Base;
		const TCHAR* BaseMaterial[2];
		/** Team symbol drawn on the cloth (red, blue). UT2004 sets it at runtime (CTFFlag.UpdateForTeam: the flag
		 *  shader's Combiner adds a TexScaler whose Material is the team symbol), so the flag material alone has none. */
		const TCHAR* Symbol[2];
		/** Cloth UV -> symbol UV of that TexScaler: uv' = uv * SymbolScale + SymbolOffset (symbol texture clamped). */
		float SymbolScale;
		FVector2f SymbolOffset;
	};
	OPENUT2K4_API const FFlagArt& GetFlagArt();

	/**
	 * Team symbols (CTF flags, HUD team scores). UT2004 picks them at random per match from the non single-player
	 * entries of its symbol list (CacheManager.GetTeamSymbolList(.., bNoSinglePlayer): every texture listed in
	 * TeamSymbols_UT2003.int / TeamSymbols_UT2004.int without Description="SP"), red from index 1 on, blue from
	 * index 10 on (TeamGame.GetRandomTeamSymbol), and takes ?RedTeamSymbol= / ?BlueTeamSymbol= overrides.
	 * Entries are texture paths relative to /Game/Replacements or /Game/Imported ("Textures/<Pkg>/<Group>_<Name>").
	 */
	OPENUT2K4_API const TArray<FString>& GetTeamSymbolList();
	/** URL value ("TeamSymbols_UT2003.sym05", "TeamSymbols_UT2004.Team.Phoenix", "Phoenix" or a texture path) -> list entry; empty if unknown. */
	OPENUT2K4_API FString ResolveTeamSymbol(const FString& Value);
	/** Symbols of a match: the URL overrides, else random as UT2004 did; red and blue are never the same. */
	OPENUT2K4_API void PickTeamSymbols(const FString& Options, FString OutSymbols[2]);

	/** Bombing Run: ball, goal hoop (+ team pulse material for slot 1), ball spawn pad. */
	struct FBombingRunArt
	{
		FItemMesh Ball;
		FItemMesh Goal;
		const TCHAR* GoalPulseMaterial[2];
		/** Goal hoop: mesh width relative to the hoop radius. */
		float GoalWidthPerRadius;
		FItemMesh SpawnPad;
	};
	OPENUT2K4_API const FBombingRunArt& GetBombingRunArt();

	/** Domination: floor pad, letter emblems (A, B) and ring, materials per state (0 red, 1 blue, 2 neutral). */
	struct FDominationArt
	{
		FItemMesh Pad;
		FItemMesh Letter[2];
		FItemMesh Ring;
		const TCHAR* LetterMaterial[3];
		const TCHAR* RingMaterial[3];
	};
	OPENUT2K4_API const FDominationArt& GetDominationArt();

	/** Onslaught: node base and energy frame (skeletal, team materials for sections 1..3), core and core shield. */
	struct FOnslaughtArt
	{
		FItemMesh NodeBase;
		const TCHAR* NodeFrame;
		float NodeFrameHeight;
		const TCHAR* NodeFrameMaterials[2][3];
		FItemMesh CoreBase;
		FItemMesh Core;
		FItemMesh CoreShield;
		const TCHAR* CoreShieldMaterial[2];
		/** Node shield dome (same mesh as the core shield, smaller). */
		FItemMesh NodeShield;
		/** Energy column above an owned core (team materials as the shield). */
		FItemMesh CoreEnergy;
		/** Soft flare on the node / core heart, tinted with the team colour. */
		const TCHAR* Flare;
		/** Link beam textures: soft beam and a tiled energy bolt (tinted with the link colour). */
		const TCHAR* LinkBeam;
		const TCHAR* LinkBolt;
	};
	OPENUT2K4_API const FOnslaughtArt& GetOnslaughtArt();
}
