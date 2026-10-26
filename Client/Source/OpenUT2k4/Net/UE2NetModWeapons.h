#pragma once

#include "CoreMinimal.h"
#include "../ArenaArt.h"
#include "../ArenaWeaponHand.h"

class UStaticMesh;
class UMaterialInterface;

/**
 * Mod weapons of a UT2004 server shown with the mod's own assets (Docs/UE2Net.md, "Mod weapons").
 *
 * A mod weapon plays as our stand-in for its nearest stock relative (UE2NetActors::WeaponClassFor). Where the mod's
 * class defaults name other assets or values than the stock relative's (the class the stand-in reproduces), those
 * are used: the first-person mesh (its animation set and skins), view placement (PlayerViewOffset / PlayerViewPivot /
 * DrawScale / DisplayFOV), each fire mode's FireSound and FireRate, the weapon bar icon (IconCoords on the stock HUD
 * atlas or the mod's own texture), the third-person attachment (mesh, skins, RelativeLocation / RelativeRotation /
 * DrawScale) and the pickup's StaticMesh. Assets come from the downloaded package, converted into the content cache with the other net cache
 * content (ArenaConvUserContent exports a script package's own textures, meshes and sounds). Every item falls back to
 * the stand-in's own on its own (not in the package, not converted, not loadable); Notes says which and why.
 *
 * Only class defaults are read: nothing of the mod's code runs (script-driven weapons are the next step: a weapon's
 * FireMode / attachment objects running in the script VM for client-side effects, keyed by the same UE2 class).
 */
namespace UE2NetModWeapons
{
	struct FModWeapon
	{
		/** The server's weapon class ("ProSniperRifle_20_BETA3.ProSniperRifle"). */
		FString UE2Class;
		/** Our stand-in and the stock UE2 class it reproduces ("UTClassic.ClassicSniperRifle"). */
		UClass* Native = nullptr;
		FString StockClass;
		/** The stand-in's art with the mod's own assets where they differ and load. */
		ArenaArt::FWeaponArt Art;
		bool bView = false;
		ArenaWeaponHand::FViewParams View;
		/** First-person Skins (UE2 material paths; empty: the mesh's own). */
		TArray<FString> Skins;
		/** The attachment's Skins for the third-person mesh (a weapon without a stock relative). */
		TArray<FString> ThirdPersonSkins;
		/** Fire modes' FireRate (seconds) where the mod's differs; < 0: the stand-in's. */
		float FireRate[2] = { -1.f, -1.f };
		/** IconCoords where they differ, on IconTexture (the mod's IconMaterial, an art path; empty: the stock HUD
		 *  atlas HUDContent.Generic.HUD). */
		bool bIcon = false;
		FVector4f IconCoords = FVector4f(0.f);
		FString IconTexture;
		/** Fire modes' FlashEmitterClass where it differs ("Pkg.Class"), SelectSound ("Pkg.Group.Name"). */
		FString FlashEmitterClass[2];
		FString SelectSound;
		/** What is the mod's and what fell back, for the log. */
		TArray<FString> Notes;

		// storage for Art's strings
		FString FirstPersonMesh, AnimPrefix, SelectAnim, PutDownAnim, IdleAnim, FireAnim[2], FireSound[2];
	};

	struct FModAttachment
	{
		FString UE2Class;
		/** The stand-in's art with the attachment's mesh and placement where they differ and load. */
		ArenaArt::FWeaponArt Art;
		TArray<FString> Skins;
		TArray<FString> Notes;
		FString ThirdPersonMesh;
	};

	/** The mod weapon's description, built once per class; null for stock weapons and classes without a stand-in. */
	OPENUT2K4_API const FModWeapon* FindWeapon(const FString& UE2Class);
	/** The same for a mod weapon attachment (other players' third-person weapon). */
	OPENUT2K4_API const FModAttachment* FindAttachment(const FString& UE2Class);
	/** A mod pickup's StaticMesh where it differs from its stock ancestor's (null: keep ours). */
	OPENUT2K4_API UStaticMesh* FindPickupMesh(const FString& UE2Class);
	/** A mod pickup's PrePivot less its nearest stock ancestor's (UE2 units; zero for stock classes): where UE2 draws its
	 *  mesh against the stock pickup's place (BallisticV25.AP_6Magnum: 70 UU up). */
	OPENUT2K4_API FVector PickupPrePivotDelta(const FString& UE2Class);
	/** A class of UT2004's own script packages ("XWeapons.ShockRifle"), not a mod's. */
	OPENUT2K4_API bool IsStockClass(const FString& ClassPath);
	/** Loads the materials of a Skins list (null entries: the mesh's own), converting a mod's that are not in the cache yet. */
	OPENUT2K4_API TArray<UMaterialInterface*> LoadSkins(const TArray<FString>& Skins);

	/** How a vehicle or vehicle weapon class is drawn, from its class defaults (mod vehicles: Docs/UE2Net.md "Mod vehicles"). */
	struct FLook
	{
		/** Actor.Skins (UE2 material paths; empty: the mesh's own). */
		TArray<FString> Skins;
		/** ONSVehicle / ONSWeapon RedSkin / BlueSkin: Skins[0] of team 0 / 1. */
		FString RedSkin, BlueSkin;
		/** DrawScale x DrawScale3D. */
		FVector Scale = FVector::OneVector;
		/** PrePivot (UE2 units): UE2 draws the actor at Location + PrePivot, along the world axes. */
		FVector PrePivot = FVector::ZeroVector;
		/** A mod class drawn differently from its nearest stock ancestor (its own skins or scale). */
		bool bOwn = false;
	};
	/** The look of a class ("Pkg.Class"), read once per class. */
	OPENUT2K4_API const FLook& FindLook(const FString& ClassPath);

	/**
	 * An actor a mod class's client-side code spawns and attaches to one of its bones (OdinIonTurret's PostNetBeginPlay:
	 * Spawn(class'IonTurretAttachment') then AttachToBone(TurretAttachment, PitchBone), the visible ion cannon on an
	 * invisible dummy gun). Found without running the code: an instance variable of the class whose type is a client-only
	 * (RemoteRole ROLE_None) actor class with a mesh, passed to AttachToBone with a bone name in the class's script.
	 */
	struct FClientAttachment
	{
		/** The spawned class ("OdinV2Omni.IonTurretAttachment"). */
		FString UE2Class;
		/** Art path of its mesh ("SkeletalMeshes/Pkg/Name" or "StaticMeshes/Pkg/Name"). */
		FString Mesh;
		bool bSkeletal = true;
		FName Bone;
		/** DrawScale x DrawScale3D; RelativeLocation (UE2 units) / RelativeRotation on the bone. */
		FVector Scale = FVector::OneVector;
		FVector Offset = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		/** Skins, and per team when the class has RedSkins / BlueSkins arrays (its SetTeam). */
		TArray<FString> Skins, RedSkins, BlueSkins;
	};
	/** "Animations/Pkg/Set_" of a skeletal mesh's animation set (the converter's record), empty when it has none. */
	OPENUT2K4_API FString AnimPrefixForMesh(const FString& MeshObjectPath);
	/** "Kind/Pkg/Group_Name": the content cache's art path of a UE2 object ("SkeletalMeshes", "Pkg.Group.Name"). */
	OPENUT2K4_API FString ArtPathOf(const TCHAR* Kind, const FString& ObjectPath);

	/** The client-side attachments of a class (vehicle weapons), found once per class. */
	OPENUT2K4_API const TArray<FClientAttachment>& FindClientAttachments(const FString& ClassPath);
}
