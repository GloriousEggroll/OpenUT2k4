#pragma once

#include "CoreMinimal.h"
#include "ArenaCharacter.h"
#include "ArenaINV_Monster.generated.h"

class UMaterialInterface;
class UAnimSequence;
class USkeletalMesh;
class USoundBase;

/**
 * The Invasion monster roster: the monsters of UT2004's Invasion (SkaarjPack), with our own names. Each one looks
 * like its UT2004 counterpart (vertex mesh, skin, size, animations, sounds: see ArenaINV::GetMonsterDef) and uses
 * one of the behaviours below.
 */
UENUM()
enum class EArenaINVMonsterType : uint8
{
	Gnashling,        // SkaarjPupae: crawling swarmer
	Razorwing,        // Razorfly: small flyer, bites
	Skyray,           // Manta: gliding flyer, stings
	Sputterpod,       // Krall: bolt thrower and staff fighter
	SputterpodElite,  // EliteKrall
	Whirrsting,       // Gasbag: floating, belches fireballs, punches
	Slagbrute,        // Brute: slow heavy, arm rockets, punches
	Slagtitan,        // Behemoth: tougher Brute
	Vaultfang,        // Skaarj: leaps, claws, energy bolts
	FrostFang,        // IceSkaarj
	EmberFang,        // FireSkaarj
	Cindermaw,        // WarLord: boss, rocket volleys
	Count UMETA(Hidden)
};

/** How a monster fights (AArenaINV_MonsterController::UpdateAttack). */
enum class EArenaINVBehavior : uint8
{
	Swarmer,     // sprint and bite
	Leaper,      // stalks, leaps onto the target, rakes on landing; ranged shots between leaps
	Spitter,     // keeps a fighting distance and fires its shots, melee up close
	Brute,       // closes in, winds up a slam; ranged shots on the way
	Flyer,       // orbits above the target firing its shots
	FlyerMelee,  // dives at the target and bites / stings
	Boss         // volleys of shots at range, stomp up close
};

/** A monster's ranged attack (UT2004 projectile defaults, units converted to cm). */
struct FArenaINVShot
{
	float Speed = 0.f;          // 0 = no ranged attack
	float GravityScale = 0.f;
	float Damage = 0.f;
	float DamageRadius = 0.f;
	float Size = 10.f;
	/** Shots per attack (fanned out a little). */
	int32 Count = 1;
	float Interval = 2.f;
	/** ArenaArtFX projectile style. */
	const TCHAR* Style = nullptr;
	const TCHAR* Name = TEXT("");
	/** "Sounds/<Pkg>/<Name>" fire sound. */
	const TCHAR* Sound = nullptr;
};

/** UT2004 look of a monster: vertex mesh, skins and the sequences used for each action. */
struct FArenaINVVisual
{
	/** "SkeletalMeshes/SkaarjPack_rc/<Mesh>"; sequences are "Animations/SkaarjPack_rc/<Mesh>_<Seq>". */
	const TCHAR* Mesh = nullptr;
	const TCHAR* AnimPrefix = nullptr;
	/** Skin overrides per material slot (UT2004 Skins[]): "SkaarjPackSkins/Skins_<Name>" -> material, else texture. */
	const TCHAR* Skins[2] = { nullptr, nullptr };
	/** Actor DrawScale (the mesh transform is baked: at 1 the mesh fits UT2004's collision cylinder). */
	float DrawScale = 1.f;
	float Yaw = 0.f;
	const TCHAR* Idle = nullptr;
	const TCHAR* Walk = nullptr;
	const TCHAR* Run = nullptr;
	/** Flyers: the looping flight sequence. */
	const TCHAR* Fly = nullptr;
	const TCHAR* Leap = nullptr;
	TArray<const TCHAR*> Melee;
	TArray<const TCHAR*> Fire;
	TArray<const TCHAR*> Hit;
	TArray<const TCHAR*> Death;
	/** Ground speed (cm/s) at which Run / Walk play at their own rate. */
	float RunAnimSpeed = 600.f;
	float WalkAnimSpeed = 250.f;
};

/** UT2004 sounds of a monster ("Sounds/SkaarjPack_rc/<Group>_<Name>"). */
struct FArenaINVSounds
{
	TArray<const TCHAR*> Hit;
	TArray<const TCHAR*> Death;
	TArray<const TCHAR*> Challenge;
	const TCHAR* Melee = nullptr;
};

/** Static tuning of one monster type (see ArenaINV::GetMonsterDef). */
struct FArenaINVMonsterDef
{
	const TCHAR* Name = TEXT("");
	EArenaINVBehavior Behavior = EArenaINVBehavior::Swarmer;
	float Health = 100.f;
	float CapsuleRadius = 34.f;
	float CapsuleHalfHeight = 88.f;
	float Speed = 600.f;
	int32 Points = 1;
	bool bFlying = false;
	bool bBoss = false;
	/** Melee reach beyond the capsules (cm). */
	float MeleeReach = 60.f;
	float MeleeDamage = 10.f;
	float MeleeInterval = 1.f;
	const TCHAR* MeleeName = TEXT("Claws");
	FArenaINVShot Shot;
	FArenaINVVisual Visual;
	FArenaINVSounds Sounds;
	/** Procedural stand-in colours (only without the UT2004 art). */
	FLinearColor BodyColor = FLinearColor::Gray;
	FLinearColor GlowColor = FLinearColor::Red;
};

/**
 * Invasion monster. Subclasses AArenaCharacter so the existing pipeline treats it like any other
 * combatant: weapon traces / projectiles hit its capsule (ECC_Weapon / ECC_Projectile block), damage goes
 * through ArenaDamage -> ReceiveArenaDamage -> rules ModifyDamage, deaths go through AArenaGameMode::Killed
 * and the bots' enemy selection (TActorIterator<AArenaCharacter> + AreOnSameTeam) picks it up without
 * changes. It carries no weapons and its behaviour lives in AArenaINV_MonsterController.
 *
 * Body: the UT2004 monster's vertex mesh on ACharacter's mesh component, placed like UE2 places it (mesh origin at
 * the collision cylinder's centre, DrawScale), playing its sequences by movement (idle / walk / run / fly) and by
 * action (melee, fire, leap, hit reactions, death held on the last frame). Without the art (or arena.ImportedArt 0)
 * a procedural stand-in body is built instead.
 */
UCLASS(NotPlaceable)
class OPENUT2K4_API AArenaINV_Monster : public AArenaCharacter
{
	GENERATED_BODY()

public:
	AArenaINV_Monster();

	/** Call before FinishSpawning (SpawnActorDeferred). */
	void InitMonster(EArenaINVMonsterType InType, float InHealthScale, float InDamageScale);

	EArenaINVMonsterType GetMonsterType() const { return MonsterType; }
	const FArenaINVMonsterDef& GetDef() const;
	EArenaINVBehavior GetBehavior() const { return GetDef().Behavior; }
	float GetMaxHealth() const { return MaxHealth; }
	float GetMonsterDamageScale() const { return DamageScale; }
	bool IsFlyer() const;
	bool IsBoss() const;

	/** Visual cue while charging an attack (slam wind-up, volley charge): 0..1 glow boost (procedural body only). */
	void SetChargeGlow(float Alpha);

	/** Action animations, called by the controller. */
	enum class EAction : uint8 { Melee, Fire, Leap };
	void PlayAction(EAction Action);
	/** One of the type's sounds at the monster (Challenge when it spots a target, Melee on a hit). */
	void PlayMonsterSound(const TArray<const TCHAR*>& Sounds, float Volume = 1.f) const;
	void PlayMonsterSound(const TCHAR* Sound, float Volume = 1.f) const;

	/** Location of the feet (capsule bottom). */
	FVector GetFeetLocation() const;

	/** Showcase (TickDebugShowcase): plays a sequence by role. 0 idle, 1 walk, 2 run, 3 melee, 4 fire, 5 hit, 6 death. */
	void DebugPlayRole(int32 Role);

	/** True when the UT2004 mesh is shown (else the procedural stand-in). */
	bool HasImportedBody() const { return bUsingImportedMesh; }

protected:
	virtual void BeginPlay() override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool HandleDeathBody(bool bGib, const FVector& Velocity) override;

	/** Creates the procedural body into BodyParts (so AArenaCharacter::BuildBody skips the humanoid). */
	void BuildMonsterBody();
	/** AArenaCharacter recolours BodyParts with team materials; put ours back. */
	void ApplyMonsterMaterials();
	/** The UT2004 mesh, skins and animations. False when the art is missing. */
	bool SetupImportedBody();
	/** Movement / action animation (imported body). */
	void TickAnimation(float DeltaSeconds);
	UAnimSequence* LoadSequence(const TCHAR* Name) const;
	UAnimSequence* PickSequence(const TArray<const TCHAR*>& Names) const;
	/** Plays Seq unless it is already playing (bLoop) / restarts it (one-shots). */
	void PlaySequence(UAnimSequence* Seq, bool bLoop, float Rate = 1.f);

	/** Add one shape part (pivot at the feet, relative to BodyRoot). MatIndex: 0 body, 1 dark, 2 glow, 3 accent. */
	UStaticMeshComponent* AddPart(UStaticMesh* Mesh, const FVector& Loc, const FVector& SizeCm, int32 MatIndex, const FRotator& Rot = FRotator::ZeroRotator);

	EArenaINVMonsterType MonsterType = EArenaINVMonsterType::Gnashling;
	float HealthScale = 1.f;
	float DamageScale = 1.f;
	float MaxHealth = 100.f;
	bool bConfigured = false;
	float AnimTime = 0.f;
	float ChargeGlow = 0.f;
	bool bDeathHandled = false;
	bool bUsingImportedMesh = false;

	// imported body animation state
	UPROPERTY() TObjectPtr<UAnimSequence> CurrentSeq;
	bool bCurrentLoops = false;
	float OneShotEnd = 0.f;
	int32 SeenHitCount = 0;
	float LastHitSoundTime = -100.f;

	UPROPERTY() TArray<TObjectPtr<UMaterialInterface>> PartMaterials;
	UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> MonsterMats;
	/** Parts that bob / spin (rotors, legs) with their base transforms. */
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> AnimParts;
	TArray<FVector> AnimBaseLoc;
	TArray<FRotator> AnimBaseRot;
	TArray<FVector> AnimBaseScale;
	TArray<int32> AnimKind;
};

namespace ArenaINV
{
	OPENUT2K4_API const FArenaINVMonsterDef& GetMonsterDef(EArenaINVMonsterType Type);

	/** UT2004 Invasion's default waves (Invasion.Waves, 16 of them): the types in a wave and its max monsters alive. */
	OPENUT2K4_API void GetWave(int32 Wave, TArray<EArenaINVMonsterType>& OutTypes, int32& OutMaxMonsters);

	/**
	 * Test hook (non-shipping), -INVShowcase: 3 s into the match every monster type stands in a row in front of the
	 * first player start (no AI), a camera frames them in turn (one per 2.5 s, for -AutoScreenshot) while they play
	 * their melee / fire / hit / walk sequences, and from then on the death sequences. No waves start. Use with ?Bots=0.
	 */
	OPENUT2K4_API void TickDebugShowcase(UWorld* World);
	/** True with -INVShowcase (the rules then start no waves). */
	OPENUT2K4_API bool IsShowcase();
}
