#pragma once

#include "CoreMinimal.h"
#include "ArenaArtVehicles.generated.h"

class AActor;
class USceneComponent;
class UPoseableMeshComponent;
class USkeletalMeshComponent;
class USkeletalMesh;
class UMaterialBillboardComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UAudioComponent;
class USoundBase;
class USoundAttenuation;
class UAnimSequence;
class AArenaCharacter;
enum class EArenaONS_VehicleType : uint8;
struct FArenaONS_VehicleSpec;

/**
 * Optional Onslaught vehicle art, loaded at runtime by soft path through ArenaArt (Replacements first, then the
 * local imported placeholders). Nothing is hard-referenced: when the chassis mesh of a vehicle type is missing (or
 * arena.ImportedArt is 0) the vehicle keeps its procedural model and its procedural tuning.
 *
 * The table in ArenaArtVehicles.cpp maps each of our vehicle types to a chassis skeletal mesh, team skins, wheel /
 * roller / turret bones, gun meshes attached to bones, hover-jet glow sprites, a wreck static mesh and sounds, plus
 * the collision box / suspension layout that fits that mesh. Paths only; every name players see comes from our code.
 */
namespace ArenaArtVehicles
{
	/** Which aim a gun mesh follows. */
	enum class EAim : uint8
	{
		None,    // fixed decoration
		Driver,  // driver turret (TurretYaw / TurretPitch)
		Rear     // passenger turret (RearTurretYaw / RearTurretPitch)
	};

	/** Chassis bone that follows a suspension point (wheel): moves with the spring, spins, optionally steers. */
	struct FWheel
	{
		const TCHAR* Bone = nullptr;
		int32 SuspensionIndex = 0;
		bool bSteer = false;
	};

	/** Chassis bone that only spins with the ground speed of its side (tank rollers). Side: +1 right, -1 left. */
	struct FSpinner
	{
		const TCHAR* Bone = nullptr;
		float Radius = 50.f;
		float Side = 1.f;
	};

	/** A separate skeletal mesh attached to a bone: turrets, gun pods. */
	struct FGun
	{
		const TCHAR* Mesh = nullptr;
		/** Bone on the chassis (ParentGun < 0) or on another gun mesh (ParentGun = its index). */
		const TCHAR* AttachBone = nullptr;
		int32 ParentGun = -1;
		FVector AttachOffset = FVector::ZeroVector;
		EAim Aim = EAim::None;
		const TCHAR* YawBone = nullptr;
		const TCHAR* PitchBone = nullptr;
		/** Muzzle: bone + offset in that bone's reference frame. Used for the vehicle guns listed in ServesGuns. */
		const TCHAR* MuzzleBone = nullptr;
		FVector MuzzleOffset = FVector::ZeroVector;
		/** Spread between alternating shots, along the muzzle bone's Y axis (0 = single muzzle). */
		float MuzzleSpread = 0.f;
		TArray<int32> ServesGuns;
		bool bTeamSkin = true;
	};

	/** Additive glow sprite (hover jets, engine exhausts). Location in actor space. */
	struct FGlow
	{
		FVector Location = FVector::ZeroVector;
		float Size = 100.f;
		const TCHAR* Texture = nullptr;
		/** Scales with thrust (hover / flyer); otherwise constant. */
		bool bThruster = true;
	};

	/**
	 * Team skin for the material slots that show the Red or Blue material. The source game adds a team colour mask
	 * to an uncoloured diffuse in a combiner the importer does not reproduce, and the imported blue masks are empty,
	 * so the skin is rebuilt here as the uncoloured diffuse tinted towards the team colour, plus a faint team-coloured
	 * rim (the same convention as player models without team textures). Without the diffuse texture the slots get
	 * the Red / Blue material as imported.
	 */
	struct FSkin
	{
		const TCHAR* Red = nullptr;
		const TCHAR* Blue = nullptr;
		/** "Textures/<Pkg>/<Name>" uncoloured diffuse. */
		const TCHAR* Diffuse = nullptr;
	};

	struct FVehicleArt
	{
		const TCHAR* Mesh = nullptr;
		/** Mesh placement relative to the vehicle's collision box centre (actor origin). */
		FVector MeshOffset = FVector::ZeroVector;
		TArray<FSkin> Skins;
		/** Chassis material slots that need another material (e.g. slots without one in the source). */
		TArray<TPair<int32, const TCHAR*>> SlotMaterials;
		TArray<FWheel> Wheels;
		TArray<FSpinner> Spinners;
		TArray<FGun> Guns;
		TArray<FGlow> Glows;
		const TCHAR* Wreck = nullptr;

		// ---- sounds ("Sounds/<Pkg>/<Name>") ----
		const TCHAR* EngineSound = nullptr;
		const TCHAR* StartSound = nullptr;
		const TCHAR* StopSound = nullptr;
		const TCHAR* ExplodeSound = nullptr;
		const TCHAR* GunSound[4] = { nullptr, nullptr, nullptr, nullptr };
		/** Minimum seconds between two fire sounds per gun (long samples on rapid-fire guns). */
		float GunSoundInterval[4] = { 0.f, 0.f, 0.f, 0.f };

		// ---- driver ----
		/** Seat the driver's model (Vehicle_Driving pose) instead of hiding it. Location / rotation in actor space. */
		bool bShowDriver = false;
		FVector DriverLocation = FVector::ZeroVector;
		FRotator DriverRotation = FRotator::ZeroRotator;

		// ---- tuning that fits the mesh (overrides the procedural spec) ----
		FVector HalfExtent = FVector(150.f, 85.f, 30.f);
		TArray<FVector> SuspensionPoints;
		float RestLength = 40.f;
		float RestCompression = 0.45f;
		float WheelRadius = 0.f;
		float CameraDistance = 600.f;
		float CameraHeight = 180.f;
		/** Turret pivots (actor space for the turret roots, turret-root space for the barrel roots). */
		FVector TurretPivot = FVector::ZeroVector;
		FVector BarrelPivot = FVector::ZeroVector;
		FVector RearTurretPivot = FVector::ZeroVector;
		FVector RearBarrelPivot = FVector::ZeroVector;
		/** Passenger seat pawn (camera anchor) relative to the rear turret root. */
		FVector RearSeatOffset = FVector(0.f, 0.f, 40.f);
		/** Fallback muzzles per gun index (same spaces as FArenaONS_GunSpec::Muzzles); empty = keep the spec's. */
		TArray<FVector> Muzzles[4];
	};

	/** Table entry for a vehicle type (whether or not its assets exist). */
	OPENUT2K4_API const FVehicleArt* Find(EArenaONS_VehicleType Type);

	/** True when art is enabled and the chassis mesh of this type loads. */
	OPENUT2K4_API bool IsAvailable(EArenaONS_VehicleType Type);

	/** Overwrite the size / suspension / camera / muzzle tuning of a spec with the art's. */
	OPENUT2K4_API void ApplyToSpec(const FVehicleArt& Art, FArenaONS_VehicleSpec& Spec);

	/** Chassis mesh of a type (nullptr if unavailable). */
	OPENUT2K4_API USkeletalMesh* LoadChassis(EArenaONS_VehicleType Type);

	/** Material for a skin slot of the given team. */
	OPENUT2K4_API UMaterialInterface* LoadSkin(const FSkin& Skin, int32 Team);

	/** Distance falloff shared by the vehicle sounds. */
	OPENUT2K4_API USoundAttenuation* GetAttenuation();

	/** One-shot sound by relative path (nullptr if missing). */
	OPENUT2K4_API USoundBase* LoadSound(const TCHAR* Path);

	/** "Vehicle_Driving" sequence for a player model mesh (nullptr if the model has none). */
	OPENUT2K4_API UAnimSequence* FindDrivingAnim(const USkeletalMesh* CharacterMesh);
}

/** Per-frame inputs of the bone rig. */
struct FArenaVehicleArtPose
{
	/** Per suspension point: wheel centre offset along the hull's up axis from the reference pose (cm). */
	TArray<float> WheelOffsets;
	float SteerDegrees = 0.f;
	/** Accumulated wheel rotation (degrees). */
	float WheelSpin = 0.f;
	/** Forward speed (cm/s) and yaw rate (deg/s) of the hull, for rollers. */
	float ForwardSpeed = 0.f;
	float YawRate = 0.f;
	float TurretYaw = 0.f;
	float TurretPitch = 0.f;
	float RearTurretYaw = 180.f;
	float RearTurretPitch = 0.f;
	/** 0..1 thruster output. */
	float Thrust = 0.f;
	float DeltaSeconds = 0.f;
	/** World time (sprite flicker). */
	float Time = 0.f;
};

/**
 * Runtime model built from an FVehicleArt: a poseable chassis, gun meshes on its bones, glow sprites, the engine
 * sound and an optional seated driver. Bones are posed directly (no animation blueprint needed).
 */
USTRUCT()
struct OPENUT2K4_API FArenaVehicleArtRig
{
	GENERATED_BODY()

	UPROPERTY() TObjectPtr<UPoseableMeshComponent> Chassis;
	UPROPERTY() TArray<TObjectPtr<UPoseableMeshComponent>> Guns;
	UPROPERTY() TArray<TObjectPtr<UMaterialBillboardComponent>> Glows;
	UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> GlowMaterials;
	/** Per skin: tinted diffuse material; shared team rim overlay. */
	UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> SkinMaterials;
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> SkinRim;
	UPROPERTY() TObjectPtr<UAudioComponent> Engine;
	UPROPERTY() TObjectPtr<USkeletalMeshComponent> Driver;

	const ArenaArtVehicles::FVehicleArt* Art = nullptr;

	bool IsValid() const { return Chassis != nullptr && Art != nullptr; }

	/** Builds the components under Parent (the vehicle's visual root). False (and nothing built) if the mesh is missing. */
	bool Build(AActor* Owner, USceneComponent* Parent, EArenaONS_VehicleType Type, int32 Team);
	void SetTeam(int32 Team);
	void UpdatePose(const FArenaVehicleArtPose& Pose);
	/** Engine loop on / off and pitch (0..1 load). */
	void UpdateEngine(bool bRunning, float Load);
	/** Seat (or clear, with nullptr) the driver's model. True if a model is shown. */
	bool SetDriver(const AArenaCharacter* Char);
	/** World muzzle of a vehicle gun from the gun meshes, if one serves it. Shot alternates between barrels. */
	bool GetMuzzle(int32 GunIndex, int32 Shot, FVector& OutLocation) const;
	/** Current world transform of the chassis mesh (for the wreck). */
	FTransform GetChassisTransform() const;

private:
	void PoseBone(UPoseableMeshComponent* Mesh, int32 BoneIndex, const FTransform& ComponentSpace, TMap<int32, FTransform>& Posed) const;
	FTransform GetRefComponentSpace(const UPoseableMeshComponent* Mesh, int32 BoneIndex) const;

	int32 SkinTeam = -2;
	TWeakObjectPtr<const AArenaCharacter> SeatedChar;
	TArray<int32> WheelBones;
	TArray<int32> SpinnerBones;
	TArray<int32> GunYawBones;
	TArray<int32> GunPitchBones;
	float RollerSpin[2] = { 0.f, 0.f };
};
