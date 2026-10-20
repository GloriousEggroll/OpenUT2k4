#pragma once

#include "CoreMinimal.h"

class UObject;
class UClass;
class USkeletalMesh;
class UAnimSequence;
class USoundBase;
class UTexture;

/**
 * Optional character / weapon art that is loaded at runtime by soft path.
 *
 * Nothing here is hard-referenced: every asset is looked up by path and may be missing, in which case
 * the caller keeps its procedural visuals. For a relative path such as "SkeletalMeshes/HumanMaleA/MercMaleA"
 * the lookup order is
 *   1. /Game/Replacements/<path>   (modern replacement art, committed with the project)
 *   2. /Game/Imported/<path>       (local placeholders converted by Tools/ImportUT2004.py, git-ignored), or with
 *      runtime content (ArenaContent::UseRuntimeContent) the asset built from the user's content cache by
 *      ArenaRuntimeContent::LoadByRelativePath
 *
 * The tables in ArenaArt.cpp map our characters and weapons to those paths. They contain asset paths only;
 * all names shown to players come from our own code.
 */
namespace ArenaArt
{
	/** Generic soft load with the Replacements -> Imported lookup. Results (including misses) are cached. */
	OPENUT2K4_API UObject* LoadArt(UClass* Class, const FString& RelativePath);

	/**
	 * Same for a full "/Game/Imported/<path>[.<Name>]" path as written in the asset tables (Content/ImportedData or
	 * the content cache's data/ folder); works with runtime content too. nullptr for other roots.
	 */
	OPENUT2K4_API UObject* LoadImportedPath(UClass* Class, const FString& ImportedPath);

	template <typename T>
	T* Load(const FString& RelativePath)
	{
		return Cast<T>(LoadArt(T::StaticClass(), RelativePath));
	}

	/** Same, but skips looping sound waves (they would never stop when fired and forgotten). */
	OPENUT2K4_API USoundBase* LoadOneShotSound(const FString& RelativePath);

	/** True unless art loading is switched off (arena.ImportedArt 0). */
	OPENUT2K4_API bool IsEnabled();

	// ---------------------------------------------------------------------------------------------
	// Characters
	// ---------------------------------------------------------------------------------------------

	struct FCharacterModel
	{
		/** "SkeletalMeshes/<Pkg>/<Mesh>" */
		const TCHAR* Mesh;
		/** "Animations/<Pkg>/<AnimSet>_" - sequence names are appended */
		const TCHAR* AnimPrefix;
		/** Body texture base name in Textures/PlayerSkins (team variants are <name>_0 red and <name>_1 blue) */
		const TCHAR* BodySkin;
		bool bFemale;
	};

	/** Pool of player models, interleaved so consecutive players alternate between body types. */
	OPENUT2K4_API const TArray<FCharacterModel>& GetCharacterModels();

	/** Model for a player id (stable for the whole match). */
	OPENUT2K4_API const FCharacterModel* PickCharacterModel(int32 PlayerId);

	/**
	 * Named characters (single player rosters): the player with that name uses the UE2 mesh ("HumanMaleA.EgyptMaleB")
	 * with the character's body skin ("PlayerSkins.EgyptMaleBBodyA"); meshes outside the pool are built from the
	 * converted mesh and its animation set. Set before the match starts (process lifetime); cleared for the next one.
	 */
	OPENUT2K4_API void SetCharacterOverride(const FString& PlayerName, const FString& UE2Mesh, const FString& UE2BodySkin = FString());
	OPENUT2K4_API void ClearCharacterOverrides();
	/** The model of a UE2 mesh and body skin (pool entry, or one built from the converted mesh), or null. */
	OPENUT2K4_API const FCharacterModel* FindCharacterModelForMesh(const FString& UE2Mesh, const FString& UE2BodySkin = FString());
	/** The override model for a player name, or null. */
	OPENUT2K4_API const FCharacterModel* FindCharacterOverride(const FString& PlayerName);
	/**
	 * A body skin texture by its base name (FCharacterModel::BodySkin, plus "_0" / "_1" for the team variants):
	 * Textures/PlayerSkins, else the package the character record named for that skin (DemoPlayerSkins.Jakob_Body ...).
	 */
	OPENUT2K4_API UTexture* LoadBodySkin(const FString& SkinName);

	/**
	 * xPawn Skins[1] = the character record's FaceSkin (UE2 object path, team variant <name>_<Team> first for red / blue)
	 * on meshes whose face slot has no material of its own (UE2 None: the ECE Skaarj, Hellions, Xan robots ...); meshes
	 * with their own face material keep it.
	 */
	OPENUT2K4_API void ApplyFaceSkin(class USkeletalMeshComponent* Comp, const FString& UE2FaceSkin, int32 Team);

	// ---------------------------------------------------------------------------------------------
	// Weapons
	// ---------------------------------------------------------------------------------------------

	struct FWeaponArt
	{
		/** C++ class name without the prefix, e.g. "Weap_Carbine". */
		const TCHAR* ClassName = nullptr;
		/** First-person mesh (arms included), "SkeletalMeshes/<Pkg>/<Mesh>". */
		const TCHAR* FirstPersonMesh = nullptr;
		/** First-person animation prefix, "Animations/<Pkg>/<Set>_". */
		const TCHAR* AnimPrefix = nullptr;
		const TCHAR* IdleAnim = TEXT("Idle");
		const TCHAR* SelectAnim = TEXT("Select");
		const TCHAR* PutDownAnim = TEXT("PutDown");
		/** One-shot per shot, per fire mode (nullptr = none). */
		const TCHAR* FireAnim[2] = { TEXT("Fire"), TEXT("AltFire") };
		/** Looped (or held on its last frame) while the button is held: charging, shields, beams, spinning barrels. */
		const TCHAR* HoldAnim[2] = { nullptr, nullptr };
		bool bLoopHold[2] = { false, false };
		/** Socket on the first-person mesh used for the muzzle (flash and beam start). */
		const TCHAR* MuzzleSocket = TEXT("tip");
		/** Third-person mesh shown in the character's right hand. */
		const TCHAR* ThirdPersonMesh = nullptr;
		/** Fire sounds, "Sounds/<Pkg>/<Name>" (nullptr = our synthesized sound). */
		const TCHAR* FireSound[2] = { nullptr, nullptr };
		/** Minimum seconds between two fire sounds (rapid-fire weapons with long samples). */
		float SoundInterval[2] = { 0.f, 0.f };
		/**
		 * View model placement relative to the camera (cm / degrees), scale, and the field of view it is drawn with
		 * (horizontal degrees for a 4:3 screen; widened for the actual aspect ratio at runtime).
		 */
		FVector ViewOffset = FVector::ZeroVector;
		FRotator ViewRotation = FRotator::ZeroRotator;
		float ViewScale = 1.f;
		float ViewFOV = 60.f;
		/** Third-person mesh placement relative to the hand socket. */
		FVector HandOffset = FVector::ZeroVector;
		FRotator HandRotation = FRotator::ZeroRotator;
		float HandScale = 0.4f;

		/**
		 * Placement in the source game's units: view offset (units), view pivot and hand rotation (65536 = 360 deg),
		 * draw scales and display FOV. One unit is 2 cm in the imported content.
		 */
		void SetViewFromSource(const FVector& Offset, const FIntVector& Pivot, float DrawScale, float DisplayFOV);
		void SetHandFromSource(const FVector& Offset, const FIntVector& Rotation, float DrawScale);
		/** Two-handed heavy weapon: characters use the heavy idle / fire poses. */
		bool bHeavy = false;
	};

	/** Art mapping for a weapon class (walks up the class hierarchy). nullptr if none. */
	OPENUT2K4_API const FWeaponArt* FindWeaponArt(const UClass* WeaponClass);
	OPENUT2K4_API const TArray<FWeaponArt>& GetWeaponArtTable();
}
