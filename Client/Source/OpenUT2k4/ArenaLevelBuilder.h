#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ArenaTypes.h"
#include "Dom/JsonObject.h"
#include "ArenaLevelBuilder.generated.h"

class UStaticMeshComponent;
class AArenaFlag;
class AArenaPickup;
class AArenaJumpPad;
class APlayerStart;
class UStaticMesh;
class UMaterialInterface;
namespace ArenaUE2World { struct FJumpPadInfo; }

/** Menu-facing description of a layout (a UT2004 map from the user's install). */
USTRUCT()
struct FArenaLayoutInfo
{
	GENERATED_BODY()
	/** "Imported:<MapName>" (FindLayout also accepts the bare map name). */
	UPROPERTY() FString Id;
	UPROPERTY() FString DisplayName;
	UPROPERTY() FString Description;
	UPROPERTY() FString Author;
	UPROPERTY() bool bSupportsDM = true;
	UPROPERTY() bool bSupportsCTF = false;
	UPROPERTY() int32 RecommendedPlayers = 6;
	/** Map name, e.g. "BR-Anubis". */
	UPROPERTY() FString MapName;
	/** The map's game type prefix ("DM", "CTF", "BR", "DOM", "AS", "ONS" ...), upper case. */
	UPROPERTY() FString GameTypePrefix;
	/**
	 * Feature tags for UArenaGameRules::SupportsLayout: "FlagBases" (both CTF flag bases) and "raw:<list>"
	 * for every non-empty raw_markers list of the marker json (e.g. "raw:bomb_spawns").
	 */
	UPROPERTY() TArray<FString> Features;
	/** A map from the player's custom content folder (ArenaUserContent), not from the UT2004 install. */
	UPROPERTY() bool bCustom = false;
	/** Texture object path of the map's screenshot (custom maps; ArenaRuntimeContent::LoadTextureByObjectPath), or empty. */
	UPROPERTY() FString Screenshot;
	/** What a custom map plays without (e.g. custom scripts), for the menu; empty when nothing is left out. */
	UPROPERTY() FString Notice;

	bool HasFeature(const FString& Feature) const { return Features.Contains(Feature); }
};

/** One entry of the imported json's "raw_markers" lists (exporter markers, unconverted). */
struct OPENUT2K4_API FArenaRawMarker
{
	/** Exporter class name, e.g. "xBombSpawn". */
	FString Class;
	/** Class and its parents, most derived first. */
	TArray<FString> ClassChain;
	/** World location / rotation in our (UE) coordinates, like every other marker in the json. */
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	/** The marker's "properties" object (may be null). */
	TSharedPtr<FJsonObject> Properties;
	/** The whole marker object. */
	TSharedPtr<FJsonObject> Json;

	/** True if Class or any ClassChain entry equals ClassName (case-insensitive). */
	bool IsA(const FString& ClassName) const;
	/** Team from the properties ("team", "Team", "DefenderTeamIndex", "TeamNum", "TeamIndex"); -1 if none. */
	int32 GetTeam() const;
	/** Property as string / number (case-insensitive key); false if missing. */
	bool GetStringProperty(const FString& Key, FString& Out) const;
	bool GetNumberProperty(const FString& Key, double& Out) const;
};

/**
 * Sets up a match on a UT2004 map converted from the user's install. The map's geometry, lights and sky come
 * either from the cooked level /Game/Imported/Maps/<Map> (developer builds with cooked imports) or are built
 * at runtime into the shipped base level /Game/Maps/Arena from the content cache (ArenaRuntimeMap). Then
 * BuildImported reads the marker json and spawns our player starts, pickups and jump pads, and named anchor
 * points for game modes (mode objectives such as CTF flags are spawned by the game rules).
 * Spawned by AArenaGameMode::InitGame.
 */
UCLASS()
class OPENUT2K4_API AArenaLevelBuilder : public AActor
{
	GENERATED_BODY()

public:
	AArenaLevelBuilder();

	// ---- layouts (one per map: marker json + geometry available) ----
	/** All available layouts (used by the menu); empty when no UT2004 content has been converted yet. */
	static TArray<FArenaLayoutInfo> GetLayouts();
	static bool HasAnyLayouts();
	/** Layout info by id or bare map name (case-insensitive); false if unknown. */
	static bool FindLayout(const FString& LayoutId, FArenaLayoutInfo& OutInfo);
	/** Layout id prefix: "Imported:<MapName>". */
	static const TCHAR* ImportedLayoutPrefix() { return TEXT("Imported:"); }
	static FString MakeLayoutId(const FString& MapName);
	/** "Imported:DM-Rankin" -> "DM-Rankin" (bare names pass through). */
	static FString GetMapNameFromLayoutId(const FString& LayoutId);
	/**
	 * Requested if it exists and Supports() it, else Preferred (the mode's default map), else the first
	 * available layout that Supports() accepts. Empty if there is none.
	 */
	static FString ChooseLayout(const FString& Requested, const FString& Preferred, TFunctionRef<bool(const FArenaLayoutInfo&)> Supports);
	/** Forget the cached layout list (re-scanned on the next GetLayouts call, e.g. after a conversion). */
	static void RefreshImportedLayouts();

	// ---- levels and data ----
	/**
	 * Folder of the marker json files <Map>.json (and Assets/ tables): <cache>/data in runtime mode
	 * (dev fallback: Content/ImportedData while a developer cache has no data/), else Content/ImportedData.
	 */
	static FString GetImportedDataDir();
	/** True if GetImportedDataDir()/<MapName>.json exists. */
	static bool HasImportedData(const FString& MapName);
	/** Level to open for a layout: /Game/Imported/Maps/<Map> with cooked imports, else the base level /Game/Maps/Arena. */
	static FString GetLevelForLayout(const FString& LayoutId);
	/** /Game/Maps/Arena: the empty level runtime maps are built into. */
	static FString GetBaseLevel();
	/** Map being played in World (cooked map level or the runtime map built into the base level); empty if none. */
	static FString GetCurrentMapName(const UWorld* World);
	/**
	 * Reads the marker json and spawns our player starts, pickups and jump pads, and maps flag bases / raw
	 * markers to anchors; the geometry must already be in the world. The parsed json stays available through
	 * GetImportedJson(). Returns false if the json is unusable or there is no player start.
	 */
	bool BuildImported(const FString& MapName, bool bTeamGame);

	// ---- data for game rules ----
	/**
	 * Named anchor points (floor location + facing yaw) that modes use to place objectives:
	 *   "TeamBase0" / "TeamBase1"  flag / goal spot of the red (0) / blue (1) side
	 *   "Center"                   neutral middle (ball spawn ...)
	 *   "ControlA" / "ControlB"    two control points (double domination)
	 *   "Objective0".."ObjectiveN" an attack chain from side 0 towards side 1 (assault)
	 *   "Node0".."NodeN"           power node spots between the bases (onslaught)
	 *   "Core0" / "Core1"          power cores (onslaught)
	 *   "VehicleSpawn0".."N"       vehicle factories
	 * Returns an empty array if the anchor does not exist on this map.
	 */
	TArray<FTransform> GetAnchorPoints(FName Name) const;
	/** First transform of an anchor; false if missing. */
	bool GetAnchorPoint(FName Name, FTransform& Out) const;
	/** Prefix0, Prefix1, ... in order, stopping at the first missing index. E.g. GetAnchorSeries(TEXT("Objective")). */
	TArray<FTransform> GetAnchorSeries(const FString& Prefix) const;
	TArray<FName> GetAnchorNames() const;
	/** Add (append to) an anchor. Location is a floor point. Rules may add their own (e.g. derived from raw markers). */
	void AddAnchor(FName Name, const FVector& FloorLocation, float Yaw = 0.f);

	/** The whole parsed marker json <Map>.json; null before BuildImported. */
	TSharedPtr<FJsonObject> GetImportedJson() const { return ImportedJson; }
	bool IsImportedMap() const { return ImportedJson.IsValid(); }
	/** Entries of ImportedJson.raw_markers.<ListName> (e.g. "bomb_spawns"); empty if missing. */
	TArray<FArenaRawMarker> GetRawMarkers(const FString& ListName) const;
	/** Names of the lists in raw_markers (empty if none). */
	TArray<FString> GetRawMarkerListNames() const;
	/** [x, y, z] array field -> vector. */
	static bool ReadJsonVector(const TSharedPtr<FJsonObject>& Obj, const FString& Field, FVector& Out);
	/** Rotation field: [pitch, yaw, roll] array, {"pitch","yaw","roll"} object, or a plain number (yaw). */
	static bool ReadJsonRotator(const TSharedPtr<FJsonObject>& Obj, const FString& Field, FRotator& Out);
	/** Parse one raw marker object. */
	static bool ParseRawMarker(const TSharedPtr<FJsonObject>& Obj, FArenaRawMarker& Out);

	/**
	 * Floor below a point: traces down (from 60 cm above, MaxDrop deep) against all static level
	 * geometry, ignoring pawns and gameplay actors.
	 */
	bool TraceFloor(const FVector& Location, float MaxDrop, FVector& OutFloor) const;

	/**
	 * Development check (non-shipping): warns about anchors without floor right below or blocked by geometry.
	 * Called by the game mode in StartPlay (-ValidateAnchors).
	 */
	void ValidateAnchors() const;

	/** Spawn a player start (rules may add their own spawn spots). Team -1 = any. */
	APlayerStart* AddSpawnPoint(const FVector& FloorLocation, float Yaw, int32 Team);

	const TArray<TObjectPtr<AArenaPickup>>& GetPickups() const { return Pickups; }
	const TArray<TObjectPtr<AArenaJumpPad>>& GetJumpPads() const { return JumpPads; }
	const TArray<TObjectPtr<APlayerStart>>& GetPlayerStarts() const { return PlayerStarts; }
	/** Axis aligned bounds of all generated geometry (used by the nav graph). */
	FBox GetArenaBounds() const { return ArenaBounds; }
	/** Z below which characters are killed. */
	float GetKillZ() const { return KillZ; }
	FString GetBuiltLayoutId() const { return BuiltLayoutId; }

protected:
	APlayerStart* AddPlayerStart(const FVector& Location, float Yaw, int32 Team = -1);
	AArenaPickup* AddPickup(const FVector& FloorLocation, int32 PickupType, TSubclassOf<class AArenaWeapon> WeaponClass = nullptr);
	/** UE2Pad: the map's UE2 jump pad (trigger cylinder, JumpVelocity), null for generated pads. */
	AArenaJumpPad* AddJumpPad(const FVector& FloorLocation, const FVector& Target, float ApexHeight = 400.f, const ArenaUE2World::FJumpPadInfo* UE2Pad = nullptr);
	/** Asks the game rules whether a pickup may be placed (instagib, modes without pickups). */
	bool IsPickupAllowed(int32 PickupType, TSubclassOf<class AArenaWeapon> WeaponClass) const;
	/** Anchors from flag bases and raw markers. */
	void AddImportedAnchors();
	/** Floor below a marker (level geometry only); returns Location - FallbackDrop if nothing is found. */
	FVector FindFloorBelow(const FVector& Location, float FallbackDrop, bool* bOutFound = nullptr) const;
	/** Bounds of the level's colliding static geometry. */
	FBox ComputeStaticGeometryBounds() const;

	UPROPERTY() TArray<TObjectPtr<AArenaPickup>> Pickups;
	UPROPERTY() TArray<TObjectPtr<AArenaJumpPad>> JumpPads;
	UPROPERTY() TArray<TObjectPtr<APlayerStart>> PlayerStarts;
	UPROPERTY() TArray<TObjectPtr<AActor>> SpawnedActors;
	FBox ArenaBounds = FBox(ForceInit);
	float KillZ = -2000.f;
	FString BuiltLayoutId;
	UPROPERTY() TObjectPtr<USceneComponent> BuilderRoot;
	TMap<FName, TArray<FTransform>> Anchors;
	TSharedPtr<FJsonObject> ImportedJson;
};
