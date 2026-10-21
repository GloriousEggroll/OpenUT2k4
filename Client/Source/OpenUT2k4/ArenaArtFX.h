#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "ArenaArtFX.generated.h"

class AActor;
class AArenaFXActor;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class USceneComponent;
class UStaticMesh;
class UWorld;

/**
 * Weapon effects built at runtime from the imported effect textures and meshes (the source game's particle
 * emitters were not imported, their textures and some meshes were).
 *
 * Every effect is a handful of components: camera-facing sprite quads, quads stretched along a beam and turned
 * to the viewer, flat rings, and static meshes, each scaling and fading over a short life. Textures are drawn
 * with the generated /Game/Materials/M_ArenaSprite (additive) or M_ArenaSpriteAlpha (translucent) materials,
 * which add flipbooks, tiling and panning; without them the imported master materials are used and flipbook
 * layers are skipped. All assets are soft-loaded through ArenaArt (Replacements, then Imported). When an
 * effect's textures are missing, or with arena.ImportedArt 0, the callers in ArenaFX / ArenaProjectile /
 * ArenaWeapon fall back to their procedural glow shapes.
 *
 * The recipes (effects, projectiles, per-weapon muzzle / beam / impact styles) are tables in ArenaArtFX.cpp.
 */
namespace ArenaArtFX
{
	enum class EBlend : uint8
	{
		Additive,
		Translucent
	};

	enum class EFacing : uint8
	{
		/** Quad turned to the viewer (rolls with Spin). */
		Camera,
		/** Quad stretched between two points (SetBeam), turned around that axis towards the viewer. */
		Beam,
		/** Quad stretched backwards along the parent's forward axis (projectile trails). */
		Trail,
		/** Quad kept at its relative rotation (flat rings / discs; Spin turns it around its normal). */
		Fixed
	};

	/** One textured quad layer. */
	struct FSpriteDesc
	{
		/** "Textures/<Pkg>/<Name>" (soft path, Replacements then Imported). */
		const TCHAR* Texture = nullptr;
		EBlend Blend = EBlend::Additive;
		EFacing Facing = EFacing::Camera;
		FLinearColor Tint = FLinearColor::White;
		float Intensity = 1.f;
		/** Width (along the beam / trail) and height in cm at birth. */
		FVector2D Size = FVector2D(50.f, 50.f);
		/** Size multiplier reached at the end of the life (eased out). */
		float EndScale = 1.f;
		/** Seconds; 0 = lives as long as its owner (no fade). */
		float LifeTime = 0.f;
		float Delay = 0.f;
		float FadeIn = 0.f;
		/** Fade-out curve exponent (1 = linear, >1 stays bright longer, 0 = no fade). */
		float FadePower = 1.f;
		/** Degrees per second around the facing axis. */
		float Spin = 0.f;
		bool bRandomRoll = true;
		/** Flipbook (frames row-major). Frames 0 = Columns * Rows. FrameRate 0 = play once over LifeTime. */
		int32 Columns = 1;
		int32 Rows = 1;
		int32 Frames = 0;
		/** First cell used (a single cell of a sheet: Frames = 1). */
		int32 StartFrame = 0;
		float FrameRate = 0.f;
		/** Beam / trail: cm per texture repeat along the length (0 = stretch once). */
		float TileLength = 0.f;
		/** Texture repeats per second along the length. */
		float Pan = 0.f;
		/** The texture's long axis runs along V instead of U (beam / trail textures drawn vertically). */
		bool bVerticalTexture = false;
		/** Beam: a streak of Size.X travelling from start to end over the life instead of the full span. */
		bool bTravel = false;
		/** Restart when the life ends (ambient layers). */
		bool bLoop = false;
		/** Relative placement (Fixed: rotation of the quad; its normal is the rotated +Z). */
		FVector Offset = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
	};

	/** Static mesh layer of an effect or projectile (keeps the mesh's own imported materials). */
	struct FMeshDesc
	{
		/** "StaticMeshes/<Pkg>/<Name>". */
		const TCHAR* Mesh = nullptr;
		/** Largest dimension in cm (the mesh is fitted by its bounds). */
		float Size = 50.f;
		float EndScale = 1.f;
		FRotator Rotation = FRotator::ZeroRotator;
		FVector Offset = FVector::ZeroVector;
		/** Degrees per second of roll (around the local X axis). */
		float Spin = 0.f;
		/** Fit so the mesh's bounds centre sits on the origin. */
		bool bCenter = true;
	};

	/**
	 * A sound an effect plays where it spawns. Sound may be a UT2004 ProceduralSound (base sound + pitch / volume
	 * modification and variance): with runtime content the provider renders those per play from the cache json; the
	 * cooked imports have no procedural sounds, so there CookedBase is played with the same settings (UT2004's values).
	 */
	struct FEffectSound
	{
		const TCHAR* Sound = nullptr;
		const TCHAR* CookedBase = nullptr;
		float PitchModification = 0.f;
		float PitchVariance = 0.f;
		float VolumeModification = 0.f;
		float VolumeVariance = 0.f;
	};

	/** A short-lived effect: layers plus an optional point light. */
	struct FEffectDesc
	{
		const TCHAR* Name = nullptr;
		float LifeTime = 0.4f;
		/** Sizes are authored for this radius (ArenaFX::SpawnExplosion / SpawnImpact scale by Radius / RefRadius); 0 = fixed size. */
		float RefRadius = 0.f;
		TArray<FSpriteDesc> Sprites;
		TArray<FMeshDesc> Meshes;
		FLinearColor LightColor = FLinearColor::White;
		float LightIntensity = 0.f;
		float LightRadius = 0.f;
		/** One of these is played at random (a UT2004 SoundGroup) by SpawnEffect. */
		TArray<FEffectSound> Sounds;
	};

	/** What a projectile looks like in flight and what it leaves behind. */
	struct FProjectileArt
	{
		const TCHAR* Style = nullptr;
		/** Visual sizes are authored for this FArenaProjectileParams::Size and scaled with it. */
		float RefSize = 10.f;
		TArray<FMeshDesc> Meshes;
		/** Vertex-animated mesh (e.g. goo), "SkeletalMeshes/<Pkg>/<Name>" with its material and looping sequence. */
		const TCHAR* SkelMesh = nullptr;
		const TCHAR* SkelMaterial = nullptr;
		const TCHAR* FlyAnim = nullptr;
		const TCHAR* StuckAnim = nullptr;
		float SkelSize = 0.f;
		/** Persistent sprites (LifeTime 0) attached to the projectile: glows, coronas, trails. */
		TArray<FSpriteDesc> Sprites;
		/** Effect names: radial explosion, direct hit / fizzle. */
		const TCHAR* Explosion = nullptr;
		const TCHAR* Impact = nullptr;
		/** Multiplier for the projectile's point light (0 = off). */
		float LightScale = 1.f;
	};

	/** Effect styles of one weapon fire mode. */
	struct FWeaponFX
	{
		/** C++ class name without the prefix, e.g. "Weap_Carbine". */
		const TCHAR* ClassName = nullptr;
		uint8 Mode = 0;
		const TCHAR* Muzzle = nullptr;
		const TCHAR* Beam = nullptr;
		const TCHAR* Impact = nullptr;
		/** Muzzle flash scale for this mode. */
		float MuzzleScale = 1.f;
	};

	// ---- tables (ArenaArtFX.cpp) ----
	OPENUT2K4_API const FEffectDesc* FindEffect(FName Name);
	OPENUT2K4_API const FProjectileArt* FindProjectileArt(FName Style);
	/** Walks up the class hierarchy. nullptr if none. */
	OPENUT2K4_API const FWeaponFX* FindWeaponFX(const UClass* WeaponClass, uint8 Mode);

	// ---- materials ----
	/**
	 * Effect material for a texture: an instance of M_ArenaSprite / M_ArenaSpriteAlpha, or of the imported master
	 * when those were not generated. nullptr when the texture or every parent is missing, or art is disabled.
	 */
	OPENUT2K4_API UMaterialInstanceDynamic* MakeMaterial(UObject* Outer, const TCHAR* Texture, EBlend Blend, const FLinearColor& Tint, float Intensity);
	/** True when the instance comes from the generated sprite materials (flipbook, tiling, opacity). */
	OPENUT2K4_API bool IsSpriteMaterial(const UMaterialInterface* Material);
	/** Engine plane, 100 cm square, normal +Z. */
	OPENUT2K4_API UStaticMesh* QuadMesh();

	// ---- building ----
	/** Location of the local viewer (first local player's camera), or false. */
	OPENUT2K4_API bool GetViewLocation(const UWorld* World, FVector& OutLocation);
	/** Adds a static mesh layer under Parent, fitted to Desc.Size. nullptr if the mesh is missing. */
	OPENUT2K4_API UStaticMeshComponent* AddMesh(AActor* Owner, USceneComponent* Parent, const FMeshDesc& Desc, float Scale = 1.f);
	/** Fits a static mesh component's relative scale / location so the mesh's largest dimension is Size cm. */
	OPENUT2K4_API void FitMesh(UStaticMeshComponent* Comp, float Size, const FRotator& Rotation, const FVector& Offset, bool bCenter);

	/**
	 * Spawns a named effect at a location (Rotation's X = surface normal / forward) and returns the effect actor,
	 * or nullptr when the effect is unknown or none of its layers could be built (the caller then draws its
	 * procedural version). Scale multiplies sizes; Tint (if not white) multiplies every layer's tint.
	 */
	OPENUT2K4_API AArenaFXActor* SpawnEffect(UWorld* World, FName Name, const FVector& Location, const FRotator& Rotation, float Scale = 1.f,
		const FLinearColor& Tint = FLinearColor::White);
	/** Same, stretched along Start..End (beam layers span it, other layers sit at the end). */
	OPENUT2K4_API AArenaFXActor* SpawnBeamEffect(UWorld* World, FName Name, const FVector& Start, const FVector& End, float Scale = 1.f);
	/** Same, attached to a component (muzzle flashes). bFirstPerson draws it with the first-person view model. */
	OPENUT2K4_API AArenaFXActor* SpawnAttachedEffect(USceneComponent* AttachTo, FName Name, const FVector& RelativeOffset, float Scale,
		bool bOnlyOwnerSee, bool bOwnerNoSee, bool bFirstPerson);

	/** Debug: arena.DebugFXGallery (called by the game mode every tick). */
	OPENUT2K4_API void TickDebugGallery(UWorld* World);
}

/**
 * Textured quad that faces the viewer, spans a beam or trails its parent, and animates its own size, fade,
 * spin and flipbook frame (see ArenaArtFX::FSpriteDesc). Ticks itself; cheap enough for dozens at once.
 */
UCLASS(ClassGroup = Rendering)
class OPENUT2K4_API UArenaSpriteComponent : public UStaticMeshComponent
{
	GENERATED_BODY()

public:
	UArenaSpriteComponent();

	/** Configure before or after registering. Returns false when the texture / material is unavailable. */
	bool SetupSprite(const ArenaArtFX::FSpriteDesc& InDesc, float InScale = 1.f, const FLinearColor& TintMul = FLinearColor::White);
	/** Beam span in world space (Beam facing). */
	void SetBeam(const FVector& InStart, const FVector& InEnd);
	/** Multiplies the layer's intensity (pulses, team / state dimming). */
	void SetIntensityScale(float InScale);
	void SetTint(const FLinearColor& InTint);
	/** Multiplies the quad's height (a beam's width). */
	void SetWidthScale(float InScale) { WidthScale = InScale; }
	/** Restart the life cycle (looping layers driven by their owner). */
	void Restart();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void OnRegister() override;

	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> SpriteMaterial;

protected:
	void UpdateSprite(float DeltaTime);

	ArenaArtFX::FSpriteDesc Desc;
	float Scale = 1.f;
	float Age = 0.f;
	float Roll = 0.f;
	float IntensityScale = 1.f;
	float WidthScale = 1.f;
	float BaseIntensity = 1.f;
	bool bSpriteMaterial = false;
	bool bConfigured = false;
	FVector BeamStart = FVector::ZeroVector;
	FVector BeamEnd = FVector::ZeroVector;
	float LastFrame = -1.f;
	float LastFade = -1.f;
};
