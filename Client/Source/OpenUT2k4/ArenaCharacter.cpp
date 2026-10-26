#include "ArenaCharacter.h"
#include "ArenaMatchStats.h"
#include "ArenaSPData.h"
#include "ArenaViewShake.h"
#include "ArenaRuntimeContent.h"
#include "OpenUT2k4.h"
#include "ArenaAssets.h"
#include "ArenaWeapon.h"
#include "ArenaFlag.h"
#include "ArenaCarryable.h"
#include "ArenaGameMode.h"
#include "ArenaGameRules.h"
#include "ArenaGameState.h"
#include "ArenaPlayerState.h"
#include "ArenaPlayerController.h"
#include "ArenaBotController.h"
#include "ArenaArt.h"
#include "ArenaCharacterAnim.h"
#include "ArenaCharacterMovement.h"
#include "ArenaFX.h"
#include "ArenaRagdoll.h"
#include "ArenaWeapons.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture.h"
#include "Kismet/GameplayStatics.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "HAL/IConsoleManager.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PhysicsVolume.h"
#include "GameFramework/WorldSettings.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/World.h"
#include "Engine/DamageEvents.h"
#include "TimerManager.h"

namespace ArenaCharacterArt
{
	/** The player models stand with their feet this far below the mesh origin (bind pose, ~183 cm tall, facing +X). */
	static constexpr float ModelFeetZ = -75.5f;
	/** Deaths below this health gib instead of ragdolling / playing the death animation. */
	static TAutoConsoleVariable<float> CVarGibHealth(
		TEXT("arena.GibHealth"), -60.f,
		TEXT("Deaths at or below this health gib the body (default -60; a high value gibs every death, for testing)."),
		ECVF_Cheat);

	static TAutoConsoleVariable<int32> CVarRagdolls(
		TEXT("arena.Ragdolls"), 1,
		TEXT("1 = player models ragdoll on death (physics asset built from the skeleton, ArenaRagdoll), 0 = death animations."),
		ECVF_Default);
	static TAutoConsoleVariable<int32> CVarBehindView(
		TEXT("arena.BehindView"), 0,
		TEXT("Debug camera: 1 = third person behind the viewed pawn, 2 = in front of it (to check player models)."),
		ECVF_Cheat);
	static TAutoConsoleVariable<int32> CVarDebugArt(
		TEXT("arena.DebugArt"), 0,
		TEXT("Debug: log player model bone positions and third-person weapon placement."),
		ECVF_Cheat);
	static TAutoConsoleVariable<float> CVarCycleWeapons(
		TEXT("arena.DebugCycleWeapons"), 0.f,
		TEXT("Debug: if > 0, the local player gets every weapon and switches to the next one every N seconds, firing once in between."),
		ECVF_Cheat);
}

AArenaCharacter::AArenaCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UArenaCharacterMovement>(ACharacter::CharacterMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = true;

	UCapsuleComponent* Capsule = GetCapsuleComponent();
	Capsule->InitCapsuleSize(ArenaTuning::CapsuleRadius, ArenaTuning::CapsuleHalfHeight);
	Capsule->SetCollisionResponseToChannel(ECC_Weapon, ECR_Block);
	Capsule->SetCollisionResponseToChannel(ECC_Projectile, ECR_Block);
	Capsule->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);

	bUseControllerRotationYaw = true;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;
	BaseEyeHeight = ArenaTuning::EyeHeight;
	CrouchedEyeHeight = ArenaTuning::CrouchedEyeHeight;
	JumpMaxCount = 2;
	JumpMaxHoldTime = 0.f;

	// UE2 movement parameters: see UArenaCharacterMovement's constructor.
	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->GetNavAgentPropertiesRef().bCanCrouch = true;
	Move->GetNavAgentPropertiesRef().bCanJump = true;
	Move->GetNavAgentPropertiesRef().bCanSwim = true;
	Move->SetCrouchedHalfHeight(ArenaTuning::CrouchedHalfHeight);

	// Player model (optional art, see SetupSkeletalBody). ACharacter keeps the mesh at this offset when crouching.
	GetMesh()->SetVisibility(false);
	GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GetMesh()->SetRelativeLocation(FVector(0.f, 0.f, -ArenaTuning::CapsuleHalfHeight - ArenaCharacterArt::ModelFeetZ));
	GetMesh()->SetRelativeRotation(FRotator::ZeroRotator);

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Capsule);
	Camera->SetRelativeLocation(FVector(0.f, 0.f, ArenaTuning::EyeHeight));
	Camera->bUsePawnControlRotation = true;
	Camera->SetFieldOfView(100.f);

	WeaponRoot = CreateDefaultSubobject<USceneComponent>(TEXT("WeaponRoot"));
	WeaponRoot->SetupAttachment(Camera);

	BodyRoot = CreateDefaultSubobject<USceneComponent>(TEXT("BodyRoot"));
	BodyRoot->SetupAttachment(Capsule);
	BodyRoot->SetRelativeLocation(FVector(0.f, 0.f, -ArenaTuning::CapsuleHalfHeight));

	AutoPossessAI = EAutoPossessAI::Disabled;
	AIControllerClass = AArenaBotController::StaticClass();
}

void AArenaCharacter::BeginPlay()
{
	Super::BeginPlay();
	SpawnTime = GetWorld()->GetTimeSeconds();
	SpawnLocation = GetActorLocation();
	// Subclasses with their own body (Invasion monsters) fill BodyParts before this: no player model for them.
	bAllowPlayerModel = BodyParts.Num() == 0 && !bCustomBody;
	BuildBody();
	UpdateTeamColors();
	ArenaAssets::PlaySoundAt(this, TEXT("S_Respawn"), GetActorLocation(), 0.6f);
}

void AArenaCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	SetupSkeletalBody();
	if (Weapons.Num() == 0)
	{
		GiveDefaultInventory();
	}
	UpdateTeamColors();

	// The local player doesn't see its own body (first person), others do.
	const bool bLocalPlayer = NewController && NewController->IsLocalPlayerController();
	for (UStaticMeshComponent* Part : BodyParts)
	{
		if (Part)
		{
			Part->SetOwnerNoSee(bLocalPlayer);
		}
	}
}

void AArenaCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (CarriedFlag)
	{
		CarriedFlag->Drop(GetController());
		CarriedFlag = nullptr;
	}
	if (CarriedObject)
	{
		CarriedObject->Drop(GetController());
		CarriedObject = nullptr;
	}
	ClearInventory();
	Super::EndPlay(EndPlayReason);
}

bool AArenaCharacter::Use()
{
	if (bDead || !HasAuthority())
	{
		return false;
	}
	AArenaGameMode* GM = GetWorld()->GetAuthGameMode<AArenaGameMode>();
	return GM && GM->PlayerUse(GetController());
}

int32 AArenaCharacter::GetTeam() const
{
	if (const AArenaPlayerState* PS = GetPlayerState<AArenaPlayerState>())
	{
		return PS->Team;
	}
	return ArenaTeam::None;
}

// ---------------------------------------------------------------------------
// Body
// ---------------------------------------------------------------------------

void AArenaCharacter::BuildBody()
{
	if (BodyParts.Num() > 0 || bCustomBody)
	{
		return;
	}

	struct FPart
	{
		UStaticMesh* Mesh;
		FVector Loc;
		FRotator Rot;
		FVector Scale;
		int32 Mat; // 0 = armour (team), 1 = dark suit, 2 = visor glow
	};

	// Original armoured-runner silhouette built from primitives; pivot at the feet.
	const FPart Parts[] = {
		{ ArenaAssets::Cylinder(), FVector(0, -14, 26), FRotator::ZeroRotator, FVector(0.2f, 0.2f, 0.52f), 1 },   // left shin
		{ ArenaAssets::Cylinder(), FVector(0, 14, 26), FRotator::ZeroRotator, FVector(0.2f, 0.2f, 0.52f), 1 },    // right shin
		{ ArenaAssets::Cube(), FVector(6, -14, 8), FRotator::ZeroRotator, FVector(0.34f, 0.2f, 0.16f), 0 },        // left boot
		{ ArenaAssets::Cube(), FVector(6, 14, 8), FRotator::ZeroRotator, FVector(0.34f, 0.2f, 0.16f), 0 },         // right boot
		{ ArenaAssets::Cylinder(), FVector(0, -14, 72), FRotator::ZeroRotator, FVector(0.24f, 0.24f, 0.42f), 0 },  // left thigh
		{ ArenaAssets::Cylinder(), FVector(0, 14, 72), FRotator::ZeroRotator, FVector(0.24f, 0.24f, 0.42f), 0 },   // right thigh
		{ ArenaAssets::Cube(), FVector(0, 0, 98), FRotator::ZeroRotator, FVector(0.3f, 0.48f, 0.18f), 1 },         // hips
		{ ArenaAssets::Cube(), FVector(0, 0, 128), FRotator::ZeroRotator, FVector(0.36f, 0.52f, 0.46f), 0 },       // torso
		{ ArenaAssets::Cube(), FVector(12, 0, 136), FRotator::ZeroRotator, FVector(0.14f, 0.34f, 0.22f), 2 },      // chest light
		{ ArenaAssets::Sphere(), FVector(0, -34, 150), FRotator::ZeroRotator, FVector(0.26f, 0.26f, 0.22f), 0 },   // left shoulder
		{ ArenaAssets::Sphere(), FVector(0, 34, 150), FRotator::ZeroRotator, FVector(0.26f, 0.26f, 0.22f), 0 },    // right shoulder
		{ ArenaAssets::Cylinder(), FVector(0, -38, 118), FRotator::ZeroRotator, FVector(0.15f, 0.15f, 0.5f), 1 },  // left arm
		{ ArenaAssets::Cylinder(), FVector(10, 38, 124), FRotator(-60, 0, 0), FVector(0.15f, 0.15f, 0.5f), 1 },    // right arm (aiming)
		{ ArenaAssets::Cube(), FVector(36, 34, 128), FRotator::ZeroRotator, FVector(0.5f, 0.1f, 0.12f), 1 },       // held weapon
		{ ArenaAssets::Cylinder(), FVector(0, 0, 160), FRotator::ZeroRotator, FVector(0.14f, 0.14f, 0.12f), 1 },   // neck
		{ ArenaAssets::Sphere(), FVector(0, 0, 176), FRotator::ZeroRotator, FVector(0.3f, 0.28f, 0.32f), 0 },      // helmet
		{ ArenaAssets::Cube(), FVector(12, 0, 178), FRotator::ZeroRotator, FVector(0.1f, 0.24f, 0.08f), 2 },       // visor
	};

	for (const FPart& P : Parts)
	{
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
		C->SetStaticMesh(P.Mesh);
		C->SetupAttachment(BodyRoot);
		C->SetRelativeLocation(P.Loc);
		C->SetRelativeRotation(P.Rot);
		C->SetRelativeScale3D(P.Scale);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCanEverAffectNavigation(false);
		C->SetOwnerNoSee(IsLocallyControlled() && IsPlayerControlled());
		// A pawn possessed before BeginPlay (the first player pawn) already shows its player model: keep the
		// procedural body hidden (it stays the gib / no-art fallback).
		C->SetHiddenInGame(bSkeletalBody);
		C->ComponentTags.Add(*FString::FromInt(P.Mat));
		C->RegisterComponent();
		BodyParts.Add(C);
	}
}

void AArenaCharacter::UpdateTeamColors()
{
	ApplyBodySkin();
	if (BodyParts.Num() == 0)
	{
		return;
	}

	FLinearColor Armour;
	const int32 Team = GetTeam();
	if (Team == ArenaTeam::Red || Team == ArenaTeam::Blue)
	{
		Armour = ArenaTeam::Color(Team) * 0.7f;
	}
	else
	{
		// Free-for-all: a stable per-player hue
		const APlayerState* PS = GetPlayerState();
		const int32 Id = PS ? PS->GetPlayerId() : GetUniqueID();
		const float Hue = float((Id * 47) % 360);
		Armour = FLinearColor::MakeFromHSV8(uint8(Hue / 360.f * 255.f), 170, 200);
	}
	Armour.A = 1.f;

	TeamMaterials.Reset();
	UMaterialInstanceDynamic* ArmourMat = ArenaAssets::MakeSurface(this, Armour, 0.35f, 0.6f);
	UMaterialInstanceDynamic* SuitMat = ArenaAssets::MakeSurface(this, FLinearColor(0.04f, 0.04f, 0.05f), 0.5f, 0.2f);
	UMaterialInstanceDynamic* GlowMat = ArenaAssets::MakeSurface(this, Armour, 0.2f, 0.f, Armour * 6.f);
	TeamMaterials.Add(ArmourMat);
	TeamMaterials.Add(SuitMat);
	TeamMaterials.Add(GlowMat);

	for (UStaticMeshComponent* Part : BodyParts)
	{
		if (!Part)
		{
			continue;
		}
		const int32 MatIndex = Part->ComponentTags.Num() > 0 ? FCString::Atoi(*Part->ComponentTags[0].ToString()) : 0;
		Part->SetMaterial(0, TeamMaterials[FMath::Clamp(MatIndex, 0, 2)]);
	}
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void AArenaCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	TickArtDebug(DeltaSeconds);
	TickViewUE2(DeltaSeconds);

	if (bDead)
	{
		return;
	}

	const float Now = GetWorld()->GetTimeSeconds();

	// Falling through the floor at a spawn point: a big drop close to the spawn's XY. Running off the edge of a
	// raised spawn platform (several metres sideways first) is normal and not reported.
	if (!bReportedSpawnFall && Now - SpawnTime < 4.f && GetActorLocation().Z < SpawnLocation.Z - 400.f &&
		FVector::DistSquared2D(GetActorLocation(), SpawnLocation) < FMath::Square(250.f))
	{
		ReportSpawnFall(TEXT("dropped 4 m below spawn point"));
	}

	if (ActiveCombo != EArenaCombo::None)
	{
		// XGame.Combo.Tick: AdrenalineCost (100) drains over Duration, twice as fast while carrying a flag / ball
		const float Drain = DeltaSeconds * (IsCarryingObjective() ? 2.f : 1.f) * ArenaTuning::MaxAdrenaline / GetComboDuration(ActiveCombo);
		Adrenaline = FMath::Max(0.f, Adrenaline - Drain);
		if (ActiveCombo == EArenaCombo::Booster)
		{
			// ComboDefensive.Timer every 0.9 s: +5 health up to 199, then +5 shield
			BoosterAccum += DeltaSeconds;
			while (BoosterAccum >= 0.9f)
			{
				BoosterAccum -= 0.9f;
				GiveHealth(5.f, ArenaTuning::SuperHealthMax);
				if (Health >= ArenaTuning::SuperHealthMax)
				{
					AddShieldStrengthUE2(5);
				}
			}
		}
		if (Adrenaline <= 0.f)
		{
			ActivateCombo(EArenaCombo::None);
		}
	}

	// Combo input (xPlayer.PlayerTick): a key more than 0.35 s after the last one starts a new sequence
	if (TapHistory.Num() > 0 && Now - LastTapHistoryTime > 0.35f)
	{
		TapHistory.Reset();
	}
}

// ---------------------------------------------------------------------------
// Vitals / pickups
// ---------------------------------------------------------------------------

bool AArenaCharacter::GiveHealth(float Amount, float MaxAmount)
{
	if (bDead || Health >= MaxAmount)
	{
		return false;
	}
	Health = FMath::Min(Health + Amount, MaxAmount);
	return true;
}

bool AArenaCharacter::AddShieldStrengthUE2(int32 ShieldAmount)
{
	if (bDead)
	{
		return false;
	}
	// xPawn.AddShieldStrength / CanUseShield: up to ShieldStrengthMax 150; a 50 shield pack only tops the small
	// shield up to 50 (and at least sets the shield to 50).
	const float OldShield = Shield;
	Shield = FMath::Max(0.f, FMath::FloorToFloat(Shield));
	float Add = 0.f;
	if (Shield < ArenaTuning::MaxShield)
	{
		const float Amount = ShieldAmount == 50 ? 50.f - SmallShield : float(ShieldAmount);
		Add = FMath::Min(ArenaTuning::MaxShield, Shield + Amount) - Shield;
	}
	Shield += Add;
	if (ShieldAmount == 50)
	{
		SmallShield = 50.f;
		Shield = FMath::Max(Shield, 50.f);
	}
	return Shield != OldShield;
}

float AArenaCharacter::ShieldAbsorbUE2(float Damage)
{
	// xPawn.ShieldAbsorb: shield above 100 takes everything, the big shield part (above the small shield) 75%,
	// the small shield 50%.
	if (Shield <= 0.f)
	{
		return Damage;
	}
	float Remaining = 0.f;
	if (Shield > 100.f)
	{
		const float Interval = Shield - 100.f;
		if (Interval >= Damage)
		{
			Shield -= Damage;
			return 0.f;
		}
		Shield = 100.f;
		Damage -= Interval;
	}
	if (Shield > SmallShield)
	{
		const float Interval = Shield - SmallShield;
		if (Interval >= 0.75f * Damage)
		{
			Shield -= 0.75f * Damage;
			if (Shield < SmallShield)
			{
				SmallShield = Shield;
			}
			return FMath::FloorToFloat(0.25f * Damage);
		}
		Shield = SmallShield;
		Damage -= Interval;
		Remaining = 0.33f * Interval;
		if (Remaining <= Damage)
		{
			return FMath::FloorToFloat(Damage);
		}
		Damage -= Remaining;
	}
	if (Shield >= 0.5f * Damage)
	{
		Shield -= 0.5f * Damage;
		SmallShield = Shield;
		return FMath::FloorToFloat(Remaining + 0.5f * Damage);
	}
	Damage -= Shield;
	Shield = 0.f;
	SmallShield = 0.f;
	return FMath::FloorToFloat(Damage + Remaining);
}

bool AArenaCharacter::GiveShield(float Amount, float MaxAmount)
{
	if (bDead || Shield >= MaxAmount)
	{
		return false;
	}
	Shield = FMath::Min(Shield + Amount, MaxAmount);
	return true;
}

void AArenaCharacter::AwardAdrenaline(float Amount)
{
	// Controller.AwardAdrenaline: Adrenaline = clamp(Adrenaline + Amount, 0, AdrenalineMax), also during a combo
	if (bDead)
	{
		return;
	}
	if (ActiveCombo != EArenaCombo::None || Amount < 0.f)
	{
		Adrenaline = FMath::Clamp(Adrenaline + Amount, 0.f, ArenaTuning::MaxAdrenaline);
		return;
	}
	GiveAdrenaline(Amount);
}

bool AArenaCharacter::GiveAdrenaline(float Amount)
{
	if (bDead || Adrenaline >= ArenaTuning::MaxAdrenaline || ActiveCombo != EArenaCombo::None)
	{
		return false;
	}
	const bool bWasFull = Adrenaline >= ArenaTuning::MaxAdrenaline;
	Adrenaline = FMath::Min(Adrenaline + Amount, ArenaTuning::MaxAdrenaline);
	if (!bWasFull && Adrenaline >= ArenaTuning::MaxAdrenaline)
	{
		if (AArenaPlayerController* PC = Cast<AArenaPlayerController>(GetController()))
		{
			PC->AddCenterMessage(TEXT("Adrenaline full - combo ready"), FLinearColor(0.4f, 1.f, 0.3f), 2.f);
		}
	}
	return true;
}

void AArenaCharacter::GiveDoubleDamage(float Duration)
{
	// xPawn.EnableUDamage: UDamageTime = max(UDamageTime, now + 30) (a second pack restarts it, doesn't add)
	DoubleDamageEndTime = FMath::Max(DoubleDamageEndTime, GetWorld()->GetTimeSeconds() + Duration);
}

bool AArenaCharacter::HasDoubleDamage() const
{
	return GetWorld() && GetWorld()->GetTimeSeconds() < DoubleDamageEndTime;
}

float AArenaCharacter::GetDoubleDamageRemaining() const
{
	return GetWorld() ? FMath::Max(0.f, DoubleDamageEndTime - GetWorld()->GetTimeSeconds()) : 0.f;
}

float AArenaCharacter::GetDamageScale() const
{
	return HasDoubleDamage() ? 2.f : 1.f;
}

float AArenaCharacter::GetRefireScale() const
{
	// WeaponFire.StartBerserk: FireRate x 0.75
	return ActiveCombo == EArenaCombo::Berserk ? 0.75f : 1.f;
}

bool AArenaCharacter::HasSpawnProtection() const
{
	const AArenaGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AArenaGameMode>() : nullptr;
	const float Protection = GM && GM->GetRules() ? GM->GetRules()->GetSpawnProtectionTime() : ArenaTuning::SpawnProtectionTime;
	return !bFiredSinceSpawn && GetWorld() && GetWorld()->GetTimeSeconds() - SpawnTime < Protection;
}

// ---------------------------------------------------------------------------
// Weapons
// ---------------------------------------------------------------------------

void AArenaCharacter::GiveDefaultInventory()
{
	AArenaGameMode* GM = GetWorld()->GetAuthGameMode<AArenaGameMode>();
	if (!GM)
	{
		return;
	}
	TArray<TSubclassOf<AArenaWeapon>> Defaults;
	GM->GetDefaultInventory(GetController(), Defaults);
	for (const TSubclassOf<AArenaWeapon>& WeaponClass : Defaults)
	{
		GiveWeapon(WeaponClass);
	}
	SwitchToBestWeapon();
}

AArenaWeapon* AArenaCharacter::FindWeapon(TSubclassOf<AArenaWeapon> WeaponClass) const
{
	for (AArenaWeapon* W : Weapons)
	{
		if (W && W->GetClass() == WeaponClass)
		{
			return W;
		}
	}
	return nullptr;
}

bool AArenaCharacter::GiveWeapon(TSubclassOf<AArenaWeapon> WeaponClass)
{
	if (bDead || !WeaponClass)
	{
		return false;
	}

	if (AArenaWeapon* Existing = FindWeapon(WeaponClass))
	{
		if (Existing->bInfiniteAmmo || Existing->Ammo >= Existing->MaxAmmo)
		{
			return false;
		}
		Existing->AddAmmo(Existing->AmmoPickupAmount);
		return true;
	}

	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.Instigator = this;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AArenaWeapon* NewWeapon = GetWorld()->SpawnActor<AArenaWeapon>(WeaponClass, GetActorTransform(), Params);
	if (!NewWeapon)
	{
		return false;
	}
	Weapons.Add(NewWeapon);
	NewWeapon->GiveTo(this);

	if (int32* Pending = PendingAmmo.Find(WeaponClass.Get()))
	{
		NewWeapon->AddAmmo(*Pending);
		PendingAmmo.Remove(WeaponClass.Get());
	}

	Weapons.Sort([](const AArenaWeapon& A, const AArenaWeapon& B) { return A.Slot < B.Slot; });

	// Auto-switch to a better weapon unless we're holding fire
	const bool bHoldingFire = CurrentWeapon && (CurrentWeapon->IsFiring(0) || CurrentWeapon->IsFiring(1));
	if (!CurrentWeapon || (!bHoldingFire && NewWeapon->Priority > CurrentWeapon->Priority))
	{
		SwitchToWeapon(NewWeapon);
	}
	return true;
}

bool AArenaCharacter::GiveAmmo(TSubclassOf<AArenaWeapon> WeaponClass, int32 Amount)
{
	if (bDead || !WeaponClass)
	{
		return false;
	}
	if (AArenaWeapon* Existing = FindWeapon(WeaponClass))
	{
		if (Existing->bInfiniteAmmo || Existing->Ammo >= Existing->MaxAmmo)
		{
			return false;
		}
		const bool bWasEmpty = !Existing->HasAmmo();
		Existing->AddAmmo(Amount);
		if (bWasEmpty && CurrentWeapon && Existing->Priority > CurrentWeapon->Priority)
		{
			SwitchToWeapon(Existing);
		}
		return true;
	}

	const AArenaWeapon* CDO = WeaponClass->GetDefaultObject<AArenaWeapon>();
	int32& Pending = PendingAmmo.FindOrAdd(WeaponClass.Get());
	if (Pending >= CDO->MaxAmmo)
	{
		return false;
	}
	Pending = FMath::Min(Pending + Amount, CDO->MaxAmmo);
	return true;
}

void AArenaCharacter::SwitchToWeapon(AArenaWeapon* Weapon)
{
	if (!Weapon || Weapon == CurrentWeapon || bDead)
	{
		return;
	}
	float PutDown = 0.f;
	if (CurrentWeapon)
	{
		PutDown = CurrentWeapon->PutDownTime;
		CurrentWeapon->Unequip();
		++AnimEvents.SwitchCount;
	}
	CurrentWeapon = Weapon;
	// UE2: the old weapon's PutDownTime, then the new one's BringUpTime before it can fire
	CurrentWeapon->EquipDelay = PutDown;
	CurrentWeapon->Equip();
	UpdateThirdPersonWeapon();
	if (IsPlayerControlled())
	{
		ArenaAssets::PlaySoundAt(this, TEXT("S_WeaponSwitch"), GetActorLocation(), 0.5f);
	}
}

void AArenaCharacter::SwitchToSlot(int32 Slot)
{
	// UE2 InventoryGroup: several weapons can share a group (rail / marksman rifle 9, rockets / seeker 8, ...); pressing
	// the key again cycles through them (Pawn.NextWeapon within the group)
	TArray<AArenaWeapon*> InSlot;
	for (AArenaWeapon* W : Weapons)
	{
		if (W && W->Slot == Slot && W->HasAmmo())
		{
			InSlot.Add(W);
		}
	}
	if (InSlot.Num() == 0)
	{
		return;
	}
	const int32 Cur = InSlot.IndexOfByKey(CurrentWeapon);
	SwitchToWeapon(InSlot[Cur == INDEX_NONE ? 0 : (Cur + 1) % InSlot.Num()]);
}

void AArenaCharacter::NextWeapon()
{
	if (Weapons.Num() < 2)
	{
		return;
	}
	const int32 Start = FMath::Max(0, Weapons.IndexOfByKey(CurrentWeapon));
	for (int32 i = 1; i < Weapons.Num(); ++i)
	{
		AArenaWeapon* W = Weapons[(Start + i) % Weapons.Num()];
		if (W && W->HasAmmo())
		{
			SwitchToWeapon(W);
			return;
		}
	}
}

void AArenaCharacter::PrevWeapon()
{
	if (Weapons.Num() < 2)
	{
		return;
	}
	const int32 Start = FMath::Max(0, Weapons.IndexOfByKey(CurrentWeapon));
	for (int32 i = 1; i < Weapons.Num(); ++i)
	{
		AArenaWeapon* W = Weapons[(Start - i + Weapons.Num()) % Weapons.Num()];
		if (W && W->HasAmmo())
		{
			SwitchToWeapon(W);
			return;
		}
	}
}

void AArenaCharacter::SwitchToBestWeapon()
{
	AArenaWeapon* Best = nullptr;
	for (AArenaWeapon* W : Weapons)
	{
		if (W && W->HasAmmo() && (!Best || W->Priority > Best->Priority))
		{
			Best = W;
		}
	}
	if (Best)
	{
		SwitchToWeapon(Best);
	}
}

bool AArenaCharacter::RemoveWeapon(AArenaWeapon* Weapon)
{
	if (!Weapon || Weapons.Remove(Weapon) == 0)
	{
		return false;
	}
	if (CurrentWeapon == Weapon)
	{
		Weapon->Unequip();
		CurrentWeapon = nullptr;
		SwitchToBestWeapon();
		UpdateThirdPersonWeapon();
	}
	return true;
}

void AArenaCharacter::StartFire(uint8 Mode)
{
	if (bDead || !CurrentWeapon)
	{
		return;
	}
	const AArenaGameState* GS = GetWorld()->GetGameState<AArenaGameState>();
	if (GS && !GS->IsPlaying())
	{
		return;
	}
	bFiredSinceSpawn = true;
	CurrentWeapon->StartFire(Mode);
}

void AArenaCharacter::StopFire(uint8 Mode)
{
	if (CurrentWeapon)
	{
		CurrentWeapon->StopFire(Mode);
	}
}

void AArenaCharacter::ClearInventory()
{
	for (AArenaWeapon* W : Weapons)
	{
		if (W)
		{
			W->StopAllFire();
			W->Destroy();
		}
	}
	Weapons.Reset();
	CurrentWeapon = nullptr;
	UpdateThirdPersonWeapon();
}

// ---------------------------------------------------------------------------
// Movement: UT2004 rules (XGame.xPawn, UnrealGame.UnrealPlayer, Engine.Pawn / PlayerController / PlayerInput)
// ---------------------------------------------------------------------------

namespace ArenaMoveUE2
{
	/** UE2 EDoubleClickDir. */
	enum : uint8 { DClickNone = 0, DClickLeft = 1, DClickRight = 2, DClickForward = 3, DClickBack = 4, DClickActive = 5, DClickDone = 6 };

	uint8 FromDir(EArenaDir Dir)
	{
		switch (Dir)
		{
		case EArenaDir::Forward: return DClickForward;
		case EArenaDir::Back:    return DClickBack;
		case EArenaDir::Left:    return DClickLeft;
		default:                 return DClickRight;
		}
	}
}

UArenaCharacterMovement* AArenaCharacter::GetArenaMovement() const
{
	return Cast<UArenaCharacterMovement>(GetCharacterMovement());
}

void AArenaCharacter::OnDirectionTap(EArenaDir Dir)
{
	if (bDead)
	{
		return;
	}
	const float Now = GetWorld()->GetTimeSeconds();

	// Combo input when adrenaline is full
	if (Adrenaline >= ArenaTuning::MaxAdrenaline && ActiveCombo == EArenaCombo::None)
	{
		if (Now - LastTapHistoryTime > 0.35f)
		{
			TapHistory.Reset();
		}
		TapHistory.Add(Dir);
		LastTapHistoryTime = Now;
		if (TapHistory.Num() > 4)
		{
			TapHistory.RemoveAt(0);
		}
		if (TapHistory.Num() == 4)
		{
			using D = EArenaDir;
			auto Match = [this](D A, D B, D C, D E) { return TapHistory[0] == A && TapHistory[1] == B && TapHistory[2] == C && TapHistory[3] == E; };
			EArenaCombo Combo = EArenaCombo::None;
			if (Match(D::Forward, D::Forward, D::Forward, D::Forward)) Combo = EArenaCombo::Speed;
			else if (Match(D::Back, D::Back, D::Back, D::Back)) Combo = EArenaCombo::Booster;
			else if (Match(D::Right, D::Right, D::Left, D::Left)) Combo = EArenaCombo::Invisible;
			else if (Match(D::Forward, D::Forward, D::Back, D::Back)) Combo = EArenaCombo::Berserk;
			if (Combo != EArenaCombo::None)
			{
				TapHistory.Reset();
				ActivateCombo(Combo);
				// don't also dodge on this tap
				DoubleClickDir = DoubleClickDir == ArenaMoveUE2::DClickActive || DoubleClickDir == ArenaMoveUE2::DClickDone ? DoubleClickDir : ArenaMoveUE2::DClickNone;
				PendingTapEdges = 0;
				return;
			}
		}
	}

	// UE2 PlayerInput bEdge<Dir>: evaluated by CheckForDoubleClickMove before the next physics step.
	PendingTapEdges |= uint8(1u << ArenaMoveUE2::FromDir(Dir));
}

void AArenaCharacter::OnDodgeKey()
{
	if (!bDead)
	{
		bDodgeKeyPending = true;
	}
}

uint8 AArenaCharacter::CheckForDoubleClickMove(float DeltaTime)
{
	using namespace ArenaMoveUE2;
	const uint8 Edges = PendingTapEdges;
	PendingTapEdges = 0;
	const bool bDodgeKey = bDodgeKeyPending;
	bDodgeKeyPending = false;

	uint8 Move = DoubleClickDir == DClickActive ? DClickActive : DClickNone;
	const UCharacterMovementComponent* CM = GetCharacterMovement();
	const float DoubleClickTime = ArenaTuning::DoubleTapTime;

	if (DoubleClickDir == DClickActive)
	{
		if (CM && CM->IsMovingOnGround())
		{
			DoubleClickTimer = 0.f;
			DoubleClickDir = DClickDone;
		}
	}
	else if (DoubleClickDir != DClickDone)
	{
		const uint8 OldDoubleClick = DoubleClickDir;
		DoubleClickDir = DClickNone;
		if (Edges & (1u << DClickForward))      DoubleClickDir = DClickForward;
		else if (Edges & (1u << DClickBack))    DoubleClickDir = DClickBack;
		else if (Edges & (1u << DClickLeft))    DoubleClickDir = DClickLeft;
		else if (Edges & (1u << DClickRight))   DoubleClickDir = DClickRight;

		if (DoubleClickDir == DClickNone)
		{
			DoubleClickDir = OldDoubleClick;
		}
		else if (DoubleClickDir != OldDoubleClick)
		{
			DoubleClickTimer = DoubleClickTime + 0.5f * DeltaTime;
		}
		else
		{
			Move = DoubleClickDir;
		}

		// Dodge key (no UT2004 equivalent): dodge now towards the held movement keys, same states as a double tap.
		if (bDodgeKey && Move == DClickNone)
		{
			const FVector Input = GetPendingMovementInputVector().IsNearlyZero() ? GetLastMovementInputVector() : GetPendingMovementInputVector();
			const FRotator Yaw(0.f, GetMovementYaw(), 0.f);
			const float F = FVector::DotProduct(Input, Yaw.Vector());
			const float R = FVector::DotProduct(Input, FRotationMatrix(Yaw).GetUnitAxis(EAxis::Y));
			uint8 Held = DClickNone;
			if (FMath::Abs(F) > UE_KINDA_SMALL_NUMBER || FMath::Abs(R) > UE_KINDA_SMALL_NUMBER)
			{
				Held = FMath::Abs(F) >= FMath::Abs(R) ? (F > 0.f ? DClickForward : DClickBack) : (R > 0.f ? DClickRight : DClickLeft);
			}
			if (Held != DClickNone)
			{
				DoubleClickDir = Held;
				DoubleClickTimer = DoubleClickTime;
				Move = Held;
			}
		}
	}

	if (DoubleClickDir == DClickDone)
	{
		DoubleClickTimer = FMath::Min(DoubleClickTimer - DeltaTime, 0.f);
		if (DoubleClickTimer < -ArenaTuning::DodgeCooldown)
		{
			DoubleClickDir = DClickNone;
			DoubleClickTimer = DoubleClickTime;
		}
	}
	else if (DoubleClickDir != DClickNone && DoubleClickDir != DClickActive)
	{
		DoubleClickTimer -= DeltaTime;
		if (DoubleClickTimer < 0.f)
		{
			DoubleClickDir = DClickNone;
			DoubleClickTimer = DoubleClickTime;
		}
	}
	return Move;
}

void AArenaCharacter::PreMovementUpdate(float DeltaTime)
{
	using namespace ArenaMoveUE2;
	UArenaCharacterMovement* CM = GetArenaMovement();
	if (bDead || !CM || !GetController())
	{
		PendingTapEdges = 0;
		bDodgeKeyPending = false;
		bPendingJump = false;
		return;
	}
	const bool bPlayer = IsPlayerControlled();
	CM->bUE2Walking = bPlayer && bWalkHeld;

	if (const FUE2StepInput* Replay = StepInputOverride)
	{
		// net play replay (UE2 MoveAutonomous with bUpdating): the saved move's inputs, no input processing
		CM->bUE2Walking = Replay->bRun;
		LastStepInput = *Replay;
		if (Replay->DoubleClickMove == DClickActive && CM->IsFalling())
		{
			DoubleClickDir = DClickActive;
		}
		else if (Replay->DoubleClickMove != DClickNone && Replay->DoubleClickMove < DClickActive)
		{
			static const EArenaDir ByDClick[5] = { EArenaDir::Forward, EArenaDir::Left, EArenaDir::Right, EArenaDir::Forward, EArenaDir::Back };
			if (Dodge(ByDClick[Replay->DoubleClickMove]))
			{
				DoubleClickDir = DClickActive;
			}
		}
		if (Replay->bDoubleJump)
		{
			DoDoubleJumpUE2();
		}
		else if (Replay->bPressedJump)
		{
			DoJumpUE2(true);
		}
		if (!CM->IsFalling())
		{
			if (Replay->bDuck && !bIsCrouched && !CM->bWantsToCrouch)
			{
				Crouch();
			}
			else if (!Replay->bDuck && (bIsCrouched || CM->bWantsToCrouch))
			{
				UnCrouch();
			}
		}
		return;
	}
	LastStepInput = FUE2StepInput();
	LastStepInput.bRun = bPlayer && bWalkHeld;
	LastStepInput.bDuck = bPlayer && bDuckHeld;

	if (bPlayer)
	{
		if (CM->IsSwimming())
		{
			// UE2 PlayerSwimming: no dodges; Jump / Duck are aUp +- (swim up / down).
			PendingTapEdges = 0;
			bDodgeKeyPending = false;
			bPendingJump = false;
			const float Up = (bJumpHeld ? 1.f : 0.f) - (bDuckHeld ? 1.f : 0.f);
			if (Up != 0.f)
			{
				AddMovementInput(FVector::UpVector, Up);
			}
			return;
		}

		// UE2 PlayerController.PlayerWalking.PlayerMove: CheckForDoubleClickMove(1.1 * DeltaTime / TimeDilation).
		const AWorldSettings* WS = GetWorld()->GetWorldSettings();
		const float Dilation = WS ? FMath::Max(WS->TimeDilation, 0.01f) : 1.f;
		const uint8 DoubleClickMove = CheckForDoubleClickMove(1.1f * DeltaTime / Dilation);
		LastStepInput.DoubleClickMove = DoubleClickMove;

		// UnrealPlayer.PlayerWalking.ProcessMove
		if (DoubleClickMove == DClickActive && CM->IsFalling())
		{
			DoubleClickDir = DClickActive;
		}
		else if (DoubleClickMove != DClickNone && DoubleClickMove < DClickActive)
		{
			static const EArenaDir ByDClick[5] = { EArenaDir::Forward, EArenaDir::Left, EArenaDir::Right, EArenaDir::Forward, EArenaDir::Back };
			if (Dodge(ByDClick[DoubleClickMove]))
			{
				DoubleClickDir = DClickActive;
			}
		}
	}

	// PlayerController.PlayerWalking.ProcessMove: jump, then crouch (only when not falling).
	if (bPendingJump)
	{
		bPendingJump = false;
		LastStepInput.bPressedJump = true;
		const int32 DoubleJumpsBefore = MoveRecord.DoubleJumps;
		DoJumpUE2();
		LastStepInput.bDoubleJump = MoveRecord.DoubleJumps != DoubleJumpsBefore;
	}
	if (bPlayer && !CM->IsFalling())
	{
		if (bDuckHeld)
		{
			if (!bIsCrouched && !CM->bWantsToCrouch)
			{
				Crouch();
			}
		}
		else if (bIsCrouched || CM->bWantsToCrouch)
		{
			UnCrouch();
		}
	}
}

void AArenaCharacter::Jump()
{
	if (bDead)
	{
		return;
	}
	bPendingJump = true;
	bJumpHeld = true;
}

void AArenaCharacter::StopJumping()
{
	bJumpHeld = false;
}

bool AArenaCharacter::CanDoubleJumpUE2() const
{
	const UCharacterMovementComponent* CM = GetCharacterMovement();
	return MultiJumpRemaining > 0 && CM && CM->IsFalling();
}

bool AArenaCharacter::DoDoubleJumpUE2()
{
	UCharacterMovementComponent* CM = GetCharacterMovement();
	if (bDead || !CM || bIsCrouched || CM->bWantsToCrouch)
	{
		return false;
	}
	if (!IsPlayerControlled())
	{
		MultiJumpRemaining -= 1;
	}
	CM->Velocity.Z = CM->JumpZVelocity + MultiJumpBoost;
	CM->SetMovementMode(MOVE_Falling);
	++JumpCurrentCount;
	RecordTakeoff();
	++MoveRecord.DoubleJumps;
	ArenaAssets::PlaySoundAt(this, TEXT("S_Jump"), GetActorLocation(), 0.35f, 1.15f);
	return true;
}

bool AArenaCharacter::DoJumpUE2(bool bUpdating)
{
	UArenaCharacterMovement* CM = GetArenaMovement();
	if (bDead || !CM)
	{
		return false;
	}
	// xPawn.DoJump: the double jump needs |Velocity.Z| < 100 UU/s (near the apex of a jump or dodge).
	if (!bUpdating && CanDoubleJumpUE2() && FMath::Abs(CM->Velocity.Z) < ArenaTuning::DoubleJumpMaxVelZ && IsLocallyControlled())
	{
		DoDoubleJumpUE2();
		MultiJumpRemaining -= 1;
		return true;
	}
	// Pawn.DoJump (from the ground, or off a ladder)
	if (!bIsCrouched && !CM->bWantsToCrouch && (CM->IsMovingOnGround() || CM->IsOnLadder()))
	{
		CM->Velocity.Z = CM->bUE2Walking ? ArenaTuning::JumpZ : CM->JumpZVelocity;
		// A moving base (lift) adds its vertical speed.
		CM->Velocity.Z += CM->GetBaseVelocityUE2().Z;
		CM->SetMovementMode(MOVE_Falling);
		JumpCurrentCount = 1;
		RecordTakeoff();
		++MoveRecord.Jumps;
		ArenaAssets::PlaySoundAt(this, TEXT("S_Jump"), GetActorLocation(), 0.3f, 1.f);
		return true;
	}
	return false;
}

bool AArenaCharacter::Dodge(EArenaDir DodgeDir)
{
	// xPawn.Dodge
	UCharacterMovementComponent* CM = GetCharacterMovement();
	if (bDead || !CM || bIsCrouched || CM->bWantsToCrouch || !(CM->IsMovingOnGround() || CM->IsFalling()))
	{
		return false;
	}
	const FRotator TurnRot(0.f, GetMovementYaw(), 0.f);
	const FVector X = TurnRot.Vector();
	const FVector Y = FRotationMatrix(TurnRot).GetUnitAxis(EAxis::Y);
	FVector Dir, Cross;
	switch (DodgeDir)
	{
	case EArenaDir::Forward: Dir = X;  Cross = Y; break;
	case EArenaDir::Back:    Dir = -X; Cross = Y; break;
	case EArenaDir::Left:    Dir = -Y; Cross = X; break;
	default:                 Dir = Y;  Cross = X; break;
	}
	const bool bWall = CM->IsFalling();
	if (bWall && (!bCanWallDodge || !HasWallDodgeSupport(Dir)))
	{
		return false;
	}
	if (!IsPlayerControlled())
	{
		Cross = FVector::ZeroVector;
	}
	return PerformDodgeUE2(Dir, Cross, bWall);
}

bool AArenaCharacter::DodgeWorld(const FVector& InWorldDir)
{
	UCharacterMovementComponent* CM = GetCharacterMovement();
	if (bDead || !CM || bIsCrouched || CM->bWantsToCrouch || !(CM->IsMovingOnGround() || CM->IsFalling()))
	{
		return false;
	}
	// AI dodges have no double-click state; keep them from chaining every frame.
	if (GetWorld()->GetTimeSeconds() - LastDodgeTime < ArenaTuning::DodgeCooldown)
	{
		return false;
	}
	const FVector Dir = InWorldDir.GetSafeNormal2D();
	if (Dir.IsNearlyZero())
	{
		return false;
	}
	const bool bWall = CM->IsFalling();
	if (bWall && (!bCanWallDodge || !HasWallDodgeSupport(Dir)))
	{
		return false;
	}
	return PerformDodgeUE2(Dir, FVector::ZeroVector, bWall);
}

bool AArenaCharacter::HasWallDodgeSupport(const FVector& Dir) const
{
	// xPawn.Dodge: Trace(TraceStart + Away * 32, TraceStart, extent 1) from the feet one collision radius out, on
	// the side away from the dodge; it must hit level geometry or a mover.
	const UCapsuleComponent* Cap = GetCapsuleComponent();
	const FVector Away = -Dir.GetSafeNormal2D();
	const FVector Start = GetActorLocation() - FVector(0.f, 0.f, Cap->GetScaledCapsuleHalfHeight()) + Away * Cap->GetScaledCapsuleRadius();
	const FVector End = Start + Away * ArenaTuning::WallDodgeTraceDist;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaWallDodge), false, this);
	FCollisionResponseParams RP(Cap->GetCollisionResponseToChannels());
	RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
	RP.CollisionResponse.SetResponse(ECC_Projectile, ECR_Ignore);
	RP.CollisionResponse.SetResponse(ECC_PhysicsBody, ECR_Ignore);
	FHitResult Hit;
	if (!GetWorld()->SweepSingleByChannel(Hit, Start, End, FQuat::Identity, Cap->GetCollisionObjectType(),
		FCollisionShape::MakeBox(FVector(2.f)), Q, RP))
	{
		return false;
	}
	return !Cast<APawn>(Hit.GetActor());
}

bool AArenaCharacter::PerformDodgeUE2(const FVector& Dir, const FVector& Cross, bool bWallDodge)
{
	UCharacterMovementComponent* CM = GetCharacterMovement();
	if (!CM)
	{
		return false;
	}
	FVector& Velocity = CM->Velocity;
	if (bWallDodge)
	{
		TakeFallingDamageUE2();
		if (bDead)
		{
			return false;
		}
		if (Velocity.Z < -DodgeSpeedZ * 0.5f)
		{
			Velocity.Z += DodgeSpeedZ * 0.5f;
		}
	}
	const float VelocityZ = Velocity.Z;
	// GroundSpeed: the pawn's current ground speed (speed combo / mutators included)
	Velocity = DodgeSpeedFactor * CM->MaxWalkSpeed * Dir + FVector::DotProduct(Velocity, Cross) * Cross;
	if (!bCanDodgeDoubleJump)
	{
		MultiJumpRemaining = 0;
	}
	// xPawn.PerformDodge tests the new (horizontal) velocity here, so without bCanBoostDodge the lift is always DodgeSpeedZ.
	if (bCanBoostDodge || Velocity.Z < -200.f)
	{
		Velocity.Z = VelocityZ + DodgeSpeedZ;
	}
	else
	{
		Velocity.Z = DodgeSpeedZ;
	}
	CM->SetMovementMode(MOVE_Falling);
	RecordTakeoff();
	++(bWallDodge ? MoveRecord.WallDodges : MoveRecord.Dodges);

	LastDodgeTime = GetWorld()->GetTimeSeconds();
	++AnimEvents.DodgeCount;
	AnimEvents.DodgeDir = Dir;
	ArenaAssets::PlaySoundAt(this, TEXT("S_Dodge"), GetActorLocation(), 0.7f);
	return true;
}

void AArenaCharacter::TakeFallingDamageUE2()
{
	// Pawn.TakeFallingDamage: hurt when falling faster than MaxFallSpeed (water takes 100 UU/s off).
	const UCharacterMovementComponent* CM = GetCharacterMovement();
	if (!CM || bDead)
	{
		return;
	}
	const float VelZ = CM->Velocity.Z;
	if (VelZ >= -MaxFallSpeed)
	{
		return;
	}
	// Pawn.TakeFallingDamage: Controller.DamageShake(int(FMin(1, -Velocity.Z / MaxFallSpeed))): 1 beyond MaxFallSpeed
	ArenaViewShake::DamageShake(GetController(), 1);
	float EffectiveSpeed = VelZ;
	if (CM->GetPhysicsVolume() && CM->GetPhysicsVolume()->bWaterVolume)
	{
		EffectiveSpeed = FMath::Min(0.f, EffectiveSpeed + 200.f);
	}
	if (EffectiveSpeed < -MaxFallSpeed)
	{
		const int32 Damage = int32(-100.f * (EffectiveSpeed + MaxFallSpeed) / MaxFallSpeed);
		if (Damage > 0)
		{
			FArenaHitInfo Info;
			Info.WeaponName = TEXT("Fell");
			ReceiveArenaDamage(float(Damage), nullptr, nullptr, Info);
		}
	}
}

void AArenaCharacter::AddVelocityUE2(const FVector& NewVelocity)
{
	UCharacterMovementComponent* CM = GetCharacterMovement();
	if (bDead || !CM || NewVelocity.IsNearlyZero() || CM->MovementMode == MOVE_None)
	{
		return;
	}
	if (CM->IsMovingOnGround())
	{
		CM->SetMovementMode(MOVE_Falling);
	}
	FVector Add = NewVelocity;
	// Pawn.AddVelocity: rising faster than 380 UU/s already halves further lift.
	if (CM->Velocity.Z > 760.f && Add.Z > 0.f)
	{
		Add.Z *= 0.5f;
	}
	MoveRecord.PushVelocityBefore = CM->Velocity;
	CM->Velocity += Add;
	MoveRecord.PushVelocityAfter = CM->Velocity;
	++MoveRecord.Pushes;
}

void AArenaCharacter::TakeMomentumUE2(const FVector& Momentum, bool bSelf)
{
	// Pawn.TakeDamage: Momentum is MomentumTransfer / Mass (here in cm/s). A walking pawn gets at least 40% of it
	// upwards (DamageType.bExtraMomentumZ), self inflicted momentum is scaled by 0.6, then Pawn.AddVelocity.
	if (bDead || Momentum.IsNearlyZero())
	{
		return;
	}
	FVector Push = Momentum;
	const UCharacterMovementComponent* CM = GetCharacterMovement();
	if (CM && CM->IsMovingOnGround())
	{
		Push.Z = FMath::Max(Push.Z, 0.4f * Push.Size());
	}
	if (bSelf)
	{
		Push *= 0.6f;
	}
	AddVelocityUE2(Push);
}

void AArenaCharacter::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);
	UCharacterMovementComponent* CM = GetCharacterMovement();
	const float ImpactSpeed = -CM->Velocity.Z;

	// Pawn.Landed
	TakeFallingDamageUE2();
	// xPawn.Landed
	MultiJumpRemaining = GetMaxMultiJump();
	// Pawn.Landed: landing faster than 200 UU/s dips the view (PlayerController.bLandingShake)
	if (ImpactSpeed > 400.f && IsPlayerControlled())
	{
		bViewJustLanded = true;
	}
	ViewOldZ = GetActorLocation().Z;
	if (ImpactSpeed > 600.f)
	{
		ArenaAssets::PlaySoundAt(this, TEXT("S_Land"), GetActorLocation(), FMath::Clamp(ImpactSpeed / 2000.f, 0.3f, 1.f));
	}
	// UnrealPlayer.PlayerWalking.NotifyLanded: landing from a dodge stops you (10% of the speed is kept) and the
	// next dodge waits for the double-click timer.
	if (IsPlayerControlled())
	{
		if (DoubleClickDir == ArenaMoveUE2::DClickActive)
		{
			DoubleClickDir = ArenaMoveUE2::DClickDone;
			DoubleClickTimer = 0.f;
			CM->Velocity.X *= 0.1f;
			CM->Velocity.Y *= 0.1f;
		}
		else
		{
			DoubleClickDir = ArenaMoveUE2::DClickNone;
		}
	}
	MoveRecord.LandLocation = GetActorLocation() - FVector(0.f, 0.f, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	MoveRecord.LandVelocity = FVector(CM->Velocity.X, CM->Velocity.Y, 0.f);
	MoveRecord.LandImpactVelocity = FVector(CM->Velocity.X, CM->Velocity.Y, -ImpactSpeed);
}

void AArenaCharacter::OnStartCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
	Super::OnStartCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);
	// Pawn.StartCrouch: the eye keeps its world height, then smooths down to the crouched eye height.
	ViewEyeHeight += ScaledHalfHeightAdjust;
	ViewOldZ -= ScaledHalfHeightAdjust;
	ViewBaseEyeHeight = ArenaTuning::CrouchedEyeHeight;
}

void AArenaCharacter::OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
	Super::OnEndCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);
	ViewEyeHeight -= ScaledHalfHeightAdjust;
	ViewOldZ += ScaledHalfHeightAdjust;
	ViewBaseEyeHeight = ArenaTuning::EyeHeight;
}

void AArenaCharacter::TickViewUE2(float DeltaSeconds)
{
	if (!Camera)
	{
		return;
	}
	const UCharacterMovementComponent* CM = GetCharacterMovement();
	const float Z = GetActorLocation().Z;
	if (!bViewInit)
	{
		bViewInit = true;
		ViewOldZ = Z;
		ViewEyeHeight = ViewBaseEyeHeight;
	}
	if (!IsPlayerControlled() || bDead || !CM)
	{
		ViewOldZ = Z;
		Camera->SetRelativeLocation(FVector(0.f, 0.f, ViewEyeHeight));
		return;
	}
	const float Dt = FMath::Max(DeltaSeconds, 0.f);
	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

	// Pawn.CheckBob (run before the eye update in UE2's PlayerMove)
	const FVector Y = FRotationMatrix(FRotator(0.f, GetMovementYaw(), 0.f)).GetUnitAxis(EAxis::Y);
	if (bViewJustLanded)
	{
		ViewBobTime = 0.f;
		ViewWalkBob = FVector::ZeroVector;
	}
	else if (CM->IsMovingOnGround())
	{
		const float Speed2D = CM->Velocity.Size();
		ViewBobTime += Speed2D < 20.f ? 0.2f * Dt : Dt * (0.3f + 0.7f * Speed2D / FMath::Max(1.f, CM->MaxWalkSpeed));
		ViewWalkBob = Y * ArenaTuning::ViewBob * Speed2D * FMath::Sin(8.f * ViewBobTime);
		ViewAppliedBob *= 1.f - FMath::Min(1.f, 16.f * Dt);
		ViewWalkBob.Z = ViewAppliedBob;
		if (Speed2D > 20.f)
		{
			ViewWalkBob.Z += 0.75f * ArenaTuning::ViewBob * Speed2D * FMath::Sin(16.f * ViewBobTime);
		}
	}
	else if (CM->IsSwimming())
	{
		ViewBobTime += Dt;
		const float Speed2D = CM->Velocity.Size2D();
		ViewWalkBob = Y * ArenaTuning::ViewBob * 0.5f * Speed2D * FMath::Sin(4.f * ViewBobTime);
		ViewWalkBob.Z = ArenaTuning::ViewBob * 1.5f * Speed2D * FMath::Sin(8.f * ViewBobTime);
	}
	else
	{
		ViewBobTime = 0.f;
		ViewWalkBob *= 1.f - FMath::Min(1.f, 8.f * Dt);
	}

	// Pawn.UpdateEyeHeight (UE2 units doubled: 15 UU = 30 cm, 35 UU = 70 cm, ...)
	const float MaxEyeHeight = HalfHeight + 70.f;
	if (FMath::Abs(Z - ViewOldZ) > 30.f)
	{
		bViewJustLanded = false;
		bViewLandRecovery = false;
	}
	if (!bViewJustLanded)
	{
		const float Smooth = FMath::Min(0.9f, 10.f * Dt);
		ViewLandBob *= 1.f - Smooth;
		if (CM->IsMovingOnGround())
		{
			// stairs / steps: keep the eye's world height and smooth towards the base eye height
			ViewEyeHeight = FMath::Clamp((ViewEyeHeight - Z + ViewOldZ) * (1.f - Smooth) + ViewBaseEyeHeight * Smooth, -0.5f * HalfHeight, MaxEyeHeight);
		}
		else
		{
			ViewEyeHeight = FMath::Min(ViewEyeHeight * (1.f - Smooth) + ViewBaseEyeHeight * Smooth, MaxEyeHeight);
		}
	}
	else if (bViewLandRecovery)
	{
		const float Smooth = FMath::Min(0.9f, 10.f * Dt);
		ViewEyeHeight = FMath::Min(ViewEyeHeight * (1.f - 0.6f * Smooth) + ViewBaseEyeHeight * 0.6f * Smooth, ViewBaseEyeHeight);
		ViewLandBob *= 1.f - Smooth;
		if (ViewEyeHeight >= ViewBaseEyeHeight - 2.f)
		{
			bViewJustLanded = false;
			bViewLandRecovery = false;
			ViewEyeHeight = ViewBaseEyeHeight;
		}
	}
	else
	{
		const float Smooth = FMath::Min(0.65f, 10.f * Dt);
		const float OldEye = ViewEyeHeight;
		ViewEyeHeight = FMath::Min(ViewEyeHeight * (1.f - 1.5f * Smooth), MaxEyeHeight);
		ViewLandBob += 0.03f * (OldEye - ViewEyeHeight);
		if (ViewEyeHeight < 0.25f * ViewBaseEyeHeight + 2.f || ViewLandBob > 6.f)
		{
			bViewLandRecovery = true;
			ViewEyeHeight = 0.25f * ViewBaseEyeHeight + 2.f;
		}
	}
	ViewOldZ = Z;

	// Pawn.EyePosition = EyeHeight + WalkBob (the camera follows the pawn's yaw: Y is its local right)
	Camera->SetRelativeLocation(FVector(0.f, FVector::DotProduct(ViewWalkBob, Y), ViewEyeHeight + ViewWalkBob.Z));
}

void AArenaCharacter::RecordTakeoff()
{
	MoveRecord.TakeoffLocation = GetActorLocation() - FVector(0.f, 0.f, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	MoveRecord.TakeoffVelocity = GetCharacterMovement()->Velocity;
	++MoveRecord.Takeoffs;
}

float AArenaCharacter::GetComboRemaining() const
{
	// remaining time at the current drain rate (UE2 combos end when the adrenaline runs out)
	return ActiveCombo == EArenaCombo::None ? 0.f : Adrenaline / ArenaTuning::MaxAdrenaline * GetComboDuration(ActiveCombo);
}

float AArenaCharacter::GetComboDuration(EArenaCombo Combo)
{
	// XGame.ComboSpeed Duration 16; ComboBerserk / ComboDefensive / ComboInvis: Combo default 30
	return Combo == EArenaCombo::Speed ? 16.f : 30.f;
}

void AArenaCharacter::ActivateCombo(EArenaCombo Combo)
{
	ActiveCombo = Combo;
	BoosterAccum = Combo == EArenaCombo::Booster ? 0.9f : 0.f;   // ComboDefensive.StartEffect calls Timer() at once
	if (Combo != EArenaCombo::None)
	{
		ComboEndTime = GetWorld()->GetTimeSeconds() + GetComboDuration(Combo);
		UArenaMatchStats::RecordCombo(GetWorld(), GetController());
		ArenaAssets::PlaySoundAt(this, TEXT("S_Combo"), GetActorLocation());
		if (AArenaPlayerController* PC = Cast<AArenaPlayerController>(GetController()))
		{
			static const TCHAR* Names[] = { TEXT(""), TEXT("Speed"), TEXT("Booster"), TEXT("Invisible"), TEXT("Berserk") };
			PC->AddCenterMessage(FString::Printf(TEXT("%s!"), Names[static_cast<int32>(Combo)]), FLinearColor(0.4f, 1.f, 0.3f), 2.f);
		}
	}
	else
	{
		Adrenaline = 0.f;
	}
	UpdateMovementForCombo();
}

void AArenaCharacter::UpdateMovementForCombo()
{
	UCharacterMovementComponent* Move = GetCharacterMovement();
	const bool bSpeed = ActiveCombo == EArenaCombo::Speed;
	// XGame.ComboSpeed: GroundSpeed / AirSpeed / WaterSpeed / AirControl x1.4, JumpZ x1.5
	Move->MaxWalkSpeed = ArenaTuning::GroundSpeed * (bSpeed ? 1.4f : 1.f);
	Move->MaxSwimSpeed = ArenaTuning::WaterSpeed * (bSpeed ? 1.4f : 1.f);
	Move->JumpZVelocity = ArenaTuning::JumpZ * (bSpeed ? 1.5f : 1.f);
	Move->AirControl = ArenaTuning::AirControl * (bSpeed ? 1.4f : 1.f);

	const bool bInvisible = ActiveCombo == EArenaCombo::Invisible;
	for (UStaticMeshComponent* Part : BodyParts)
	{
		if (Part)
		{
			Part->SetVisibility(!bInvisible);
		}
	}
	if (bSkeletalBody)
	{
		GetMesh()->SetVisibility(!bInvisible);
		UpdateThirdPersonWeapon();
	}
}

// ---------------------------------------------------------------------------
// Damage / death
// ---------------------------------------------------------------------------

FVector AArenaCharacter::GetHeadLocation() const
{
	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	// a scaled head's centre moves up with it (Pawn.IsHeadShot: HeadHeight 6 UU x HeadScale above the bone)
	return GetActorLocation() + FVector(0.f, 0.f, HalfHeight * 0.82f + 12.f * (HeadScale - 1.f));
}

bool AArenaCharacter::CheckShieldReflect(const FVector& HitLocation, FVector& OutRefNormal, int32 AmmoDrain)
{
	AWeap_Gauntlet* ShieldGun = Cast<AWeap_Gauntlet>(CurrentWeapon);
	return ShieldGun && Health > 0.f && ShieldGun->CheckReflect(HitLocation, OutRefNormal, AmmoDrain);
}

bool AArenaCharacter::IsHeadshotUE2(const FVector& HitLocation, const FVector& RayDir) const
{
	// Pawn.IsHeadShot: distance from the head centre to the segment HitLocation + Ray x 2 x (CollisionHeight +
	// CollisionRadius), compared with HeadRadius 9 UU (18 cm)
	float Radius = 0.f, HalfHeight = 0.f;
	GetCapsuleComponent()->GetScaledCapsuleSize(Radius, HalfHeight);
	const FVector Head = GetHeadLocation();
	const FVector M = RayDir.GetSafeNormal() * (2.f * HalfHeight + 2.f * Radius);
	FVector Diff = Head - HitLocation;
	const float T = FVector::DotProduct(M, Diff);
	if (T > 0.f)
	{
		const float DotMM = FVector::DotProduct(M, M);
		Diff -= (T < DotMM ? T / DotMM : 1.f) * M;
	}
	return Diff.Size() < 18.f * HeadScale;
}

bool AArenaCharacter::IsHeadshot(const FVector& HitLocation) const
{
	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	return HitLocation.Z > GetActorLocation().Z + HalfHeight * 0.62f;
}

float AArenaCharacter::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	FArenaHitInfo Info;
	Info.WeaponName = DamageCauser ? DamageCauser->GetName() : FString();
	return ReceiveArenaDamage(DamageAmount, EventInstigator, DamageCauser, Info);
}

float AArenaCharacter::ReceiveArenaDamage(float Damage, AController* InstigatorController, AActor* DamageCauser, const FArenaHitInfo& Info)
{
	if (bDead || Damage <= 0.f || bNetProxy)
	{
		return 0.f;
	}

	AArenaGameMode* GM = GetWorld()->GetAuthGameMode<AArenaGameMode>();
	const bool bSelfDamage = InstigatorController && InstigatorController == GetController();
	// UE2 Pawn.TakeDamage: the current weapon adjusts the damage first (ShieldGun alt fire)
	FVector Momentum = Info.Momentum;
	const bool bFell = Info.WeaponName == TEXT("Fell");
	const bool bWorldDamage = bFell || Info.WeaponName == TEXT("Drowned") || (!InstigatorController && !DamageCauser);
	// ShieldGun.AdjustPlayerDamage skips damage armour doesn't stop (except falls) and the owner's own shield blast
	const bool bOwnShieldBlast = bSelfDamage && DamageCauser == CurrentWeapon;
	if (CurrentWeapon && (bFell || !bWorldDamage) && !bOwnShieldBlast)
	{
		Damage = CurrentWeapon->AdjustPlayerDamage(Damage, InstigatorController, Info.HitLocation, Momentum, bFell);
	}
	// UnrealGame.DeathMatch.ReduceDamage: offline, a human player's self damage is halved at skill (difficulty) 3 and below.
	if (bSelfDamage && GM && IsPlayerControlled() && GM->GetBotSkill() <= 3 && GetNetMode() == NM_Standalone)
	{
		Damage = FMath::FloorToFloat(Damage * 0.5f);
	}
	if (GM)
	{
		Damage = GM->ModifyDamage(Damage, InstigatorController, this);
	}

	// Momentum is applied even when damage is cancelled (e.g. team mates pushing, self boosts)
	TakeMomentumUE2(Momentum, bSelfDamage);

	if (Damage <= 0.f)
	{
		return 0.f;
	}

	// UE2 damage is whole numbers; armour (xPawn.ShieldAbsorb) stops weapon damage, not falls / drowning / world damage
	Damage = FMath::FloorToFloat(Damage);
	if (!bWorldDamage && Damage > 0.f)
	{
		Damage = ShieldAbsorbUE2(Damage);
	}

	Health -= Damage;
	LastHitTime = GetWorld()->GetTimeSeconds();
	LastHitMomentum = Info.Momentum;
	if (InstigatorController && InstigatorController != GetController())
	{
		LastDamager = InstigatorController;
	}
	bLastHitHeadshot = Info.bHeadshot;

	const FVector FromLocation = DamageCauser ? DamageCauser->GetActorLocation()
		: (InstigatorController && InstigatorController->GetPawn() ? InstigatorController->GetPawn()->GetActorLocation() : GetActorLocation());

	++AnimEvents.HitCount;
	AnimEvents.bHitHead = Info.bHeadshot;
	{
		FVector From = (FromLocation - GetActorLocation()).GetSafeNormal2D();
		if (From.IsNearlyZero())
		{
			From = Info.Momentum.IsNearlyZero() ? GetActorForwardVector() : -Info.Momentum.GetSafeNormal2D();
		}
		AnimEvents.HitFromDir = From;
	}

	if (AArenaPlayerController* PC = Cast<AArenaPlayerController>(GetController()))
	{
		PC->NotifyTookDamage(FromLocation, Damage);
	}
	else if (AArenaBotController* Bot = Cast<AArenaBotController>(GetController()))
	{
		Bot->NotifyTookDamage(InstigatorController, FromLocation);
	}

	if (Health > 0.f)
	{
		// Pawn.TakeDamage -> Controller.NotifyTakeHit -> UnrealPlayer.DamageShake(actualDamage) (survived hits only)
		ArenaViewShake::DamageShake(GetController(), int32(Damage));
	}
	if (Health <= 0.f)
	{
		Die(InstigatorController, DamageCauser, Info.WeaponName);
	}
	else if (LastHitTime - LastHurtSoundTime > 0.3f)
	{
		LastHurtSoundTime = LastHitTime;
		ArenaAssets::PlaySoundAt(this, TEXT("S_Hurt"), GetActorLocation(), 0.8f);
	}
	return Damage;
}

void AArenaCharacter::DieGibbed(AController* Killer, const FString& WeaponName)
{
	if (!bDead)
	{
		Health = FMath::Min(Health, -1000.f);
		bLastHitHeadshot = false;
		Die(Killer, nullptr, WeaponName);
	}
}

void AArenaCharacter::Suicide()
{
	if (!bDead)
	{
		Health = 0.f;
		Die(GetController(), nullptr, TEXT("Suicide"));
	}
}

void AArenaCharacter::ReportSpawnFall(const TCHAR* Reason)
{
	bReportedSpawnFall = true;
	UWorld* World = GetWorld();
	FHitResult Floor;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(SpawnFallDiag), true, this);
	const bool bFloor = World->LineTraceSingleByChannel(Floor, SpawnLocation, SpawnLocation - FVector(0.f, 0.f, 400.f), ECC_Pawn, Q);
	const bool bOverlap = World->OverlapBlockingTestByChannel(SpawnLocation, FQuat::Identity, ECC_Pawn,
		FCollisionShape::MakeCapsule(ArenaTuning::CapsuleRadius, ArenaTuning::CapsuleHalfHeight), Q);
	const APlayerState* PS = GetPlayerState();
	UE_LOG(LogOpenUT2k4, Warning, TEXT("SpawnFall: %s %s (%s) %.2fs after spawn. Spawn=%s Now=%s Vel=%s Mode=%d FloorUnderSpawn=%s(%s dist=%.0f) SpawnOverlapsGeometry=%d"),
		PS ? *PS->GetPlayerName() : TEXT("?"), Reason, IsPlayerControlled() ? TEXT("human") : TEXT("bot"),
		World->GetTimeSeconds() - SpawnTime, *SpawnLocation.ToString(), *GetActorLocation().ToString(), *GetVelocity().ToString(),
		int32(GetCharacterMovement()->MovementMode.GetValue()),
		bFloor ? TEXT("yes") : TEXT("NO"), bFloor && Floor.GetComponent() ? *Floor.GetComponent()->GetName() : TEXT("-"),
		bFloor ? Floor.Distance : -1.f, bOverlap ? 1 : 0);
}

void AArenaCharacter::FellOutOfWorld(const UDamageType& DmgType)
{
	if (!bReportedSpawnFall && GetWorld()->GetTimeSeconds() - SpawnTime < 6.f)
	{
		ReportSpawnFall(TEXT("fell out of the world"));
	}
	if (!bDead)
	{
		Health = 0.f;
		// Credit the last person who hit us recently (knocked off a ledge)
		AController* Killer = (LastDamager.IsValid() && GetWorld()->GetTimeSeconds() - LastHitTime < 5.f) ? LastDamager.Get() : GetController();
		Die(Killer, nullptr, TEXT("the void"));
	}
	SetActorHiddenInGame(true);
	SetLifeSpan(0.5f);
}

void AArenaCharacter::Die(AController* Killer, AActor* DamageCauser, const FString& WeaponName)
{
	if (bDead)
	{
		return;
	}
	bDead = true;
	Health = FMath::Min(Health, 0.f);
	ActiveCombo = EArenaCombo::None;

	if (CarriedFlag)
	{
		CarriedFlag->Drop(GetController());
		CarriedFlag = nullptr;
	}
	if (CarriedObject)
	{
		CarriedObject->Drop(Killer);
		CarriedObject = nullptr;
	}

	// a player flying a guided warhead still owns this body (UE2 keeps its PlayerReplicationInfo)
	AController* MyController = GetController() ? GetController() : RemoteController.Get();
	if (AArenaGameMode* GM = GetWorld()->GetAuthGameMode<AArenaGameMode>())
	{
		UArenaMatchStats::RecordKill(GetWorld(), Killer, MyController, WeaponName, bLastHitHeadshot);
		GM->Killed(Killer, MyController, this, WeaponName, bLastHitHeadshot);
	}

	ArenaAssets::PlaySoundAt(this, TEXT("S_Death"), GetActorLocation());
	ClearInventory();

	// Which way the killing blow pushes the body (drives the death animation / ragdoll impulse)
	const bool bRecentHit = GetWorld()->GetTimeSeconds() - LastHitTime < 0.5f;
	AnimEvents.DeathPushDir = bRecentHit ? -AnimEvents.HitFromDir : -GetActorForwardVector();
	++AnimEvents.DeathCount;

	// Collision: stop blocking players and shots
	UCapsuleComponent* Capsule = GetCapsuleComponent();
	Capsule->SetCollisionResponseToAllChannels(ECR_Ignore);
	Capsule->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Capsule->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);

	const FVector Velocity = GetVelocity();
	USkeletalMeshComponent* SkelMesh = GetMesh();
	const bool bGib = Health <= ArenaCharacterArt::CVarGibHealth.GetValueOnGameThread();
	// The killing blow's push is already in the velocity (TakeMomentumUE2 runs before the damage).
	const FVector KillMomentum = FVector::ZeroVector;

	if (HandleDeathBody(bGib, Velocity + KillMomentum))
	{
		// subclass body (Invasion monsters)
	}
	else if (bSkeletalBody && !bGib)
	{
		UCharacterMovementComponent* Move = GetCharacterMovement();
		// The model's own physics asset (a replacement model), else one built from its skeleton.
		UPhysicsAsset* Ragdoll = ArenaCharacterArt::CVarRagdolls.GetValueOnGameThread() == 0 ? nullptr
			: (SkelMesh->GetPhysicsAsset() ? SkelMesh->GetPhysicsAsset() : ArenaRagdoll::GetPhysicsAsset(SkelMesh->GetSkeletalMeshAsset()));
		if (Ragdoll)
		{
			// Karma-style ragdoll: the bodies take over from the animated pose with the body's velocity plus the
			// killing momentum, the hit bone gets an extra kick; it settles and is removed with the pawn.
			Move->DisableMovement();
			Move->StopMovementImmediately();
			Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			SkelMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
			SkelMesh->SetCollisionProfileName(TEXT("Ragdoll"));
			SkelMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
			SkelMesh->SetCollisionResponseToChannel(ECC_Weapon, ECR_Ignore);
			SkelMesh->SetCollisionResponseToChannel(ECC_Projectile, ECR_Ignore);
			SkelMesh->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
			SkelMesh->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Ignore);
			SkelMesh->SetCollisionEnabled(ECollisionEnabled::PhysicsOnly);
			if (SkelMesh->GetPhysicsAsset() != Ragdoll)
			{
				SkelMesh->SetPhysicsAsset(Ragdoll, true);
			}
			SkelMesh->SetAllBodiesSimulatePhysics(true);
			SkelMesh->SetSimulatePhysics(true);
			SkelMesh->WakeAllRigidBodies();
			SkelMesh->bPauseAnims = true;
			const FVector Push = KillMomentum.IsNearlyZero() ? AnimEvents.DeathPushDir * 350.f : KillMomentum;
			SkelMesh->SetAllPhysicsLinearVelocity(Velocity * 0.8f + Push * 0.6f + FVector(0.f, 0.f, 120.f));
			// UT2004 xPawn: the hit bone takes the damage impulse (head for headshots, else the chest / pelvis)
			const FName HitBone = SkelMesh->GetBoneIndex(bLastHitHeadshot ? FName(TEXT("bip01-head")) : FName(TEXT("bip01-spine2"))) != INDEX_NONE
				? (bLastHitHeadshot ? FName(TEXT("bip01-head")) : FName(TEXT("bip01-spine2"))) : NAME_None;
			if (!HitBone.IsNone())
			{
				SkelMesh->AddImpulse(Push * 0.5f, HitBone, true);
			}
			UE_LOG(LogOpenUT2k4, Verbose, TEXT("%s ragdolls (push %s)"), *GetName(), *Push.ToCompactString());
		}
		else
		{
			// Death animation, holding its last frame. The capsule keeps falling to the floor on its own.
			Move->bRunPhysicsWithNoController = true;
			Move->Velocity.X *= 0.35f;
			Move->Velocity.Y *= 0.35f;
			if (!Move->IsFalling())
			{
				Move->SetMovementMode(MOVE_Falling);
			}
		}
	}
	else
	{
		GetCharacterMovement()->DisableMovement();
		GetCharacterMovement()->StopMovementImmediately();

		// Gibs: UT2004's gib meshes for the model's species. Without them (no art) the procedural body breaks apart.
		const bool bArtGibs = bSkeletalBody && ArenaRagdoll::SpawnGibs(GetWorld(), GetActorLocation(), Velocity + KillMomentum, static_cast<ArenaRagdoll::EGibKind>(GibKindValue));
		if (bSkeletalBody)
		{
			SkelMesh->SetVisibility(false);
			if (!bArtGibs)
			{
				ArenaFX::SpawnExplosion(GetWorld(), GetActorLocation(), FLinearColor(0.6f, 0.05f, 0.03f), 60.f);
			}
		}
		for (UStaticMeshComponent* Part : BodyParts)
		{
			if (!Part)
			{
				continue;
			}
			if (bArtGibs)
			{
				Part->SetVisibility(false);
				continue;
			}
			Part->SetHiddenInGame(false);
			Part->SetVisibility(true);
			Part->SetOwnerNoSee(false);
			Part->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
			Part->SetCollisionProfileName(TEXT("PhysicsActor"));
			Part->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
			Part->SetCollisionResponseToChannel(ECC_Weapon, ECR_Ignore);
			Part->SetCollisionResponseToChannel(ECC_Projectile, ECR_Ignore);
			Part->SetCollisionEnabled(ECollisionEnabled::PhysicsOnly);
			Part->SetSimulatePhysics(true);
			Part->SetPhysicsLinearVelocity(Velocity * 0.5f + FMath::VRand() * 250.f + FVector(0, 0, 200.f));
			Part->SetPhysicsAngularVelocityInDegrees(FMath::VRand() * 360.f);
		}
		if (!bArtGibs)
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("%s: procedural body breaks apart (%s)"), *GetName(), bSkeletalBody ? TEXT("no gib art") : TEXT("no player model art"));
		}
	}
	if (ThirdPersonWeapon)
	{
		ThirdPersonWeapon->SetVisibility(false);
	}
	if (OffhandWeapon)
	{
		OffhandWeapon->SetVisibility(false);
	}

	ArenaRagdoll::DebugNotifyDeath(this, WeaponName + (bGib ? TEXT(", gibbed") : TEXT("")));
	DetachFromControllerPendingDestroy();
	if (APlayerController* PC = Cast<APlayerController>(MyController))
	{
		PC->SetViewTarget(this);
	}
	SetLifeSpan(6.f);
}

// ---------------------------------------------------------------------------
// Player model (optional imported / replacement art)
// ---------------------------------------------------------------------------

void AArenaCharacter::SetupSkeletalBody()
{
	if (!bAllowPlayerModel || bSkeletalBody || bDead)
	{
		return;
	}
	const APlayerState* PS = GetPlayerState();
	// a named character (single player rosters) first, else the pool's pick for the player id
	const ArenaArt::FCharacterModel* Model = PS ? ArenaArt::FindCharacterOverride(PS->GetPlayerName()) : nullptr;
	if (!Model)
	{
		// the player's chosen character (?Character=, Settings > Player): its mesh and skin from the .upl record
		const AArenaPlayerState* APS = Cast<AArenaPlayerState>(PS);
		if (const ArenaSP::FPlayerRecord* Rec = APS && !APS->CharacterName.IsEmpty() ? ArenaSP::FindPlayer(APS->CharacterName) : nullptr)
		{
			Model = ArenaArt::FindCharacterModelForMesh(Rec->Mesh, Rec->BodySkin);
		}
	}
	if (!Model)
	{
		Model = ArenaArt::PickCharacterModel(PS ? PS->GetPlayerId() : int32(GetUniqueID()));
	}
	USkeletalMesh* Mesh = Model ? ArenaArt::Load<USkeletalMesh>(Model->Mesh) : nullptr;
	if (!Mesh)
	{
		return; // keep the procedural body
	}

	USkeletalMeshComponent* SkelMesh = GetMesh();
	SkelMesh->SetSkeletalMeshAsset(Mesh);
	SkelMesh->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	SkelMesh->SetAnimInstanceClass(UArenaCharacterAnimInstance::StaticClass());
	UArenaCharacterAnimInstance* Anim = Cast<UArenaCharacterAnimInstance>(SkelMesh->GetAnimInstance());
	if (!Anim || !Anim->LoadAnimSet(Model->AnimPrefix))
	{
		// A model without its animations would T-pose: stay procedural.
		UE_LOG(LogOpenUT2k4, Warning, TEXT("Player model %s has no usable animations, using the procedural body"), Model->Mesh);
		SkelMesh->SetAnimInstanceClass(nullptr);
		SkelMesh->SetSkeletalMeshAsset(nullptr);
		return;
	}

	SkelMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SkelMesh->SetCanEverAffectNavigation(false);
	SkelMesh->SetGenerateOverlapEvents(false);
	// The first-person owner never sees its own body; everybody else (and spectators through other pawns) does.
	SkelMesh->SetOwnerNoSee(true);
	SkelMesh->SetCastShadow(true);
	SkelMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	SkelMesh->SetVisibility(!IsInvisible());

	for (UStaticMeshComponent* Part : BodyParts)
	{
		if (Part)
		{
			Part->SetHiddenInGame(true);
		}
	}
	bSkeletalBody = true;
	BodySkinName = Model->BodySkin;
	// xBotGibGroup for the robots, xAlienGibGroup (green) for the aliens, xPawnGibGroup otherwise.
	{
		const FString MeshPath(Model->Mesh);
		GibKindValue = uint8(MeshPath.Contains(TEXT("/Bot/")) ? ArenaRagdoll::EGibKind::Robot
			: (MeshPath.Contains(TEXT("/Aliens/")) ? ArenaRagdoll::EGibKind::OrganicAlt : ArenaRagdoll::EGibKind::Organic));
	}
	AppliedSkinTeam = -100;
	ApplyBodySkin();
	UpdateThirdPersonWeapon();
	UE_LOG(LogOpenUT2k4, Log, TEXT("%s uses player model %s"), PS ? *PS->GetPlayerName() : *GetName(), *Mesh->GetPathName());
	if (ArenaCharacterArt::CVarDebugArt.GetValueOnGameThread() != 0)
	{
		SkelMesh->RefreshBoneTransforms();
		auto CS = [SkelMesh](const TCHAR* Bone) { return SkelMesh->GetBoneLocation(FName(Bone), EBoneSpaces::ComponentSpace).ToCompactString(); };
		UE_LOG(LogOpenUT2k4, Log, TEXT("ArtDebug %s: head %s ltoe %s lfoot %s rhand %s weapon %s"), *Mesh->GetName(), *CS(TEXT("bip01-head")), *CS(TEXT("bip01-l-toe0")),
			*CS(TEXT("bip01-l-foot")), *CS(TEXT("bip01-r-hand")), *CS(TEXT("bone_weapon")));
	}
}

void AArenaCharacter::ApplyBodySkin()
{
	if (!bSkeletalBody)
	{
		return;
	}
	const int32 Team = GetTeam();
	if (Team == AppliedSkinTeam)
	{
		return;
	}
	AppliedSkinTeam = Team;

	USkeletalMeshComponent* SkelMesh = GetMesh();
	// Body material slot: the one named like the body skin, else slot 0.
	int32 BodySlot = 0;
	for (int32 i = 0; i < SkelMesh->GetNumMaterials(); ++i)
	{
		const UMaterialInterface* M = SkelMesh->GetSkeletalMeshAsset() && SkelMesh->GetSkeletalMeshAsset()->GetMaterials().IsValidIndex(i)
			? SkelMesh->GetSkeletalMeshAsset()->GetMaterials()[i].MaterialInterface.Get() : nullptr;
		if (M && M->GetName().Contains(TEXT("Body")))
		{
			BodySlot = i;
			break;
		}
	}
	SkelMesh->SetMaterial(BodySlot, nullptr); // back to the mesh's own material
	SkelMesh->SetOverlayMaterial(nullptr);
	BodySkinMaterial = nullptr;

	// xPawn Skins[1]: the character record's face skin on meshes without a face material of their own (UE2 None)
	{
		const APlayerState* OwnerPS = GetPlayerState();
		const AArenaPlayerState* OwnerAPS = Cast<AArenaPlayerState>(OwnerPS);
		const ArenaSP::FPlayerRecord* Rec = OwnerAPS && !OwnerAPS->CharacterName.IsEmpty() ? ArenaSP::FindPlayer(OwnerAPS->CharacterName)
			: (OwnerPS && ArenaArt::FindCharacterOverride(OwnerPS->GetPlayerName()) ? ArenaSP::FindPlayer(OwnerPS->GetPlayerName()) : nullptr);
		if (Rec)
		{
			ArenaArt::ApplyFaceSkin(SkelMesh, Rec->FaceSkin, Team);
		}
	}

	if (Team != ArenaTeam::Red && Team != ArenaTeam::Blue)
	{
		// free for all: the character's own body skin when it is another one than the mesh's (named characters), else
		// the model's default skin
		{
			const APlayerState* OwnerPS = GetPlayerState();
			const AArenaPlayerState* OwnerAPS = Cast<AArenaPlayerState>(OwnerPS);
			if (OwnerPS && (ArenaArt::FindCharacterOverride(OwnerPS->GetPlayerName()) || (OwnerAPS && !OwnerAPS->CharacterName.IsEmpty())))
			{
				if (UTexture* Own = ArenaArt::LoadBodySkin(BodySkinName))
				{
					BodySkinMaterial = ArenaRuntimeContent::CreateMaterialInstance(SkelMesh->GetMaterial(BodySlot), SkelMesh);
					if (BodySkinMaterial)
					{
						SkelMesh->SetMaterial(BodySlot, BodySkinMaterial);
						BodySkinMaterial->SetTextureParameterValue(TEXT("Base"), Own);
					}
				}
			}
		}
		return;
	}
	// Team skins: <skin>_0 (red) / <skin>_1 (blue) as the "Base" texture of the imported material.
	UTexture* TeamTexture = ArenaArt::LoadBodySkin(FString::Printf(TEXT("%s_%d"), *BodySkinName, Team));
	if (TeamTexture)
	{
		// Always a new instance: with runtime content the mesh's own material is already a shared dynamic instance,
		// which CreateDynamicMaterialInstance would hand back (and this would re-skin every player using it).
		BodySkinMaterial = ArenaRuntimeContent::CreateMaterialInstance(SkelMesh->GetMaterial(BodySlot), SkelMesh);
		if (BodySkinMaterial)
		{
			SkelMesh->SetMaterial(BodySlot, BodySkinMaterial);
		}
		if (BodySkinMaterial)
		{
			BodySkinMaterial->SetTextureParameterValue(TEXT("Base"), TeamTexture);
			return;
		}
	}
	// No team texture (e.g. a replacement model): a faint team-coloured rim.
	if (UMaterialInstanceDynamic* Tint = ArenaAssets::MakeGlow(this, ArenaTeam::Color(Team), 0.6f, 0.25f))
	{
		SkelMesh->SetOverlayMaterial(Tint);
	}
}

void AArenaCharacter::UpdateThirdPersonWeapon()
{
	// a net mod weapon's art (its stand-in's, or a script weapon's own attachment: Net/UE2NetModWeapons.h), else ours
	const ArenaArt::FWeaponArt* Art = CurrentWeapon ? (CurrentWeapon->GetArt() ? CurrentWeapon->GetArt() : ArenaArt::FindWeaponArt(CurrentWeapon->GetClass())) : nullptr;
	AnimEvents.bHeavyWeapon = Art && Art->bHeavy;

	USkeletalMesh* WeaponMesh = (bSkeletalBody && !bDead && Art && Art->ThirdPersonMesh) ? ArenaArt::Load<USkeletalMesh>(Art->ThirdPersonMesh) : nullptr;
	// UE2 AssaultRifle.AttachToPawn (bDualMode): a second attachment on 'bip01 l hand', rolled 180 degrees at
	// (40, -3, -7) UU, both at DrawScale 0.3
	static const FName OffhandBone(TEXT("bip01-l-hand")); // imported name of UE2 'bip01 l hand'
	const AWeap_Carbine* DualRifle = Cast<AWeap_Carbine>(CurrentWeapon);
	const bool bOffhand = WeaponMesh && DualRifle && DualRifle->IsDualMode() && GetMesh() && GetMesh()->DoesSocketExist(OffhandBone);
	if (bOffhand && !OffhandWeapon)
	{
		OffhandWeapon = NewObject<USkeletalMeshComponent>(this, TEXT("OffhandWeapon"));
		OffhandWeapon->SetupAttachment(GetMesh(), OffhandBone);
		OffhandWeapon->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		OffhandWeapon->SetGenerateOverlapEvents(false);
		OffhandWeapon->SetCanEverAffectNavigation(false);
		OffhandWeapon->SetOwnerNoSee(GetMesh()->bOwnerNoSee);
		OffhandWeapon->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
		OffhandWeapon->RegisterComponent();
	}
	if (OffhandWeapon)
	{
		if (bOffhand)
		{
			if (OffhandWeapon->GetSkeletalMeshAsset() != WeaponMesh)
			{
				OffhandWeapon->SetSkeletalMeshAsset(WeaponMesh);
			}
			OffhandWeapon->SetRelativeLocationAndRotation(FVector(80.f, -6.f, -14.f), FRotator(0.f, 0.f, 180.f));
			OffhandWeapon->SetRelativeScale3D(FVector(Art->HandScale));
		}
		OffhandWeapon->SetVisibility(bOffhand && !IsInvisible());
	}
	if (!WeaponMesh)
	{
		if (ThirdPersonWeapon)
		{
			ThirdPersonWeapon->SetVisibility(false);
		}
		return;
	}
	if (!ThirdPersonWeapon)
	{
		ThirdPersonWeapon = NewObject<USkeletalMeshComponent>(this, TEXT("ThirdPersonWeapon"));
		ThirdPersonWeapon->SetupAttachment(GetMesh(), TEXT("righthand"));
		ThirdPersonWeapon->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		ThirdPersonWeapon->SetGenerateOverlapEvents(false);
		ThirdPersonWeapon->SetCanEverAffectNavigation(false);
		ThirdPersonWeapon->SetOwnerNoSee(GetMesh()->bOwnerNoSee);
		ThirdPersonWeapon->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
		ThirdPersonWeapon->RegisterComponent();
	}
	if (ThirdPersonWeapon->GetSkeletalMeshAsset() != WeaponMesh)
	{
		ThirdPersonWeapon->SetSkeletalMeshAsset(WeaponMesh);
	}
	ThirdPersonWeapon->EmptyOverrideMaterials();
	const TArray<UMaterialInterface*>& TPSkins = CurrentWeapon->GetNetStandIn().ThirdPersonSkins;
	for (int32 i = 0; i < TPSkins.Num(); ++i)
	{
		if (TPSkins[i])
		{
			ThirdPersonWeapon->SetMaterial(i, TPSkins[i]);
		}
	}
	ThirdPersonWeapon->SetRelativeLocationAndRotation(Art->HandOffset, Art->HandRotation);
	ThirdPersonWeapon->SetRelativeScale3D(FVector(Art->HandScale));
	ThirdPersonWeapon->SetVisibility(!IsInvisible());
	if (ArenaCharacterArt::CVarDebugArt.GetValueOnGameThread() != 0)
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("ArtDebug %s holds %s: socket %s world %s (actor %s) vis %d"), *GetName(), *WeaponMesh->GetName(),
			GetMesh()->DoesSocketExist(TEXT("righthand")) ? TEXT("ok") : TEXT("MISSING"), *ThirdPersonWeapon->GetComponentLocation().ToCompactString(),
			*GetActorLocation().ToCompactString(), ThirdPersonWeapon->IsVisible() ? 1 : 0);
	}
}

void AArenaCharacter::NotifyWeaponFired(uint8 Mode)
{
	++AnimEvents.FireCount;
}

bool AArenaCharacter::GetThirdPersonMuzzle(FVector& OutLocation) const
{
	if (!ThirdPersonWeapon || !ThirdPersonWeapon->IsVisible() || !ThirdPersonWeapon->GetSkeletalMeshAsset())
	{
		return false;
	}
	static const FName Tip(TEXT("tip"));
	if (ThirdPersonWeapon->DoesSocketExist(Tip))
	{
		OutLocation = ThirdPersonWeapon->GetSocketLocation(Tip);
		return true;
	}
	return false;
}

bool AArenaCharacter::IsViewedInFirstPerson() const
{
	if (ArenaCharacterArt::CVarBehindView.GetValueOnGameThread() != 0)
	{
		return false;
	}
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PC = It->Get();
		if (PC && PC->IsLocalController() && PC->GetViewTarget() == this)
		{
			return true;
		}
	}
	return false;
}

void AArenaCharacter::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
	Super::CalcCamera(DeltaTime, OutResult);

	const int32 Behind = ArenaCharacterArt::CVarBehindView.GetValueOnGameThread();
	if (Behind == 0)
	{
		return;
	}
	// Debug third-person camera (arena.BehindView): orbit around the pawn, pulled in by walls.
	FRotator ViewRot = OutResult.Rotation;
	ViewRot.Pitch = FMath::Clamp(FRotator::NormalizeAxis(ViewRot.Pitch), -40.f, 40.f) * 0.5f - 8.f;
	if (Behind == 2)
	{
		ViewRot.Yaw += 160.f;
	}
	const FVector Pivot = GetActorLocation() + FVector(0.f, 0.f, 30.f);
	FVector Desired = Pivot - ViewRot.Vector() * 260.f;
	FHitResult Hit;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaBehindView), false, this);
	if (GetWorld()->SweepSingleByChannel(Hit, Pivot, Desired, FQuat::Identity, ECC_Camera, FCollisionShape::MakeSphere(12.f), Q))
	{
		Desired = Hit.Location;
	}
	OutResult.Location = Desired;
	OutResult.Rotation = (Pivot - Desired).Rotation();
	OutResult.bUseFirstPersonParameters = false;
}

void AArenaCharacter::TickArtDebug(float DeltaSeconds)
{
	// Behind view shows the body to its owner too.
	const int32 Behind = ArenaCharacterArt::CVarBehindView.GetValueOnGameThread();
	if (Behind != AppliedBehindView)
	{
		AppliedBehindView = Behind;
		if (bSkeletalBody)
		{
			GetMesh()->SetOwnerNoSee(Behind == 0);
			if (ThirdPersonWeapon)
			{
				ThirdPersonWeapon->SetOwnerNoSee(Behind == 0);
			}
			if (OffhandWeapon)
			{
				OffhandWeapon->SetOwnerNoSee(Behind == 0);
			}
		}
	}

	if (ArenaCharacterArt::CVarDebugArt.GetValueOnGameThread() >= 2 && bSkeletalBody)
	{
		// Bone check: the head / foot positions move with the animation (ref pose would stay constant)
		const float T = GetWorld()->GetTimeSeconds();
		if (FMath::FloorToInt(T * 2.f) != FMath::FloorToInt((T - DeltaSeconds) * 2.f))
		{
			USkeletalMeshComponent* SkelMesh = GetMesh();
			auto CS = [SkelMesh](const TCHAR* Bone) { return SkelMesh->GetBoneLocation(FName(Bone), EBoneSpaces::ComponentSpace).ToCompactString(); };
			UE_LOG(LogOpenUT2k4, Log, TEXT("ArtBones %s t=%.1f dead=%d mode=%d crouch=%d speed=%.0f vz=%.0f rendered=%d head %s lfoot %s rfoot %s"), *GetName(), T, bDead ? 1 : 0,
				int32(GetCharacterMovement()->MovementMode.GetValue()), bIsCrouched ? 1 : 0, GetVelocity().Size2D(), GetVelocity().Z,
				SkelMesh->WasRecentlyRendered(0.2f) ? 1 : 0, *CS(TEXT("bip01-head")), *CS(TEXT("bip01-l-foot")), *CS(TEXT("bip01-r-foot")));
		}
	}

	const float Cycle = ArenaCharacterArt::CVarCycleWeapons.GetValueOnGameThread();
	if (Cycle <= 0.f || bDead || !IsLocallyControlled() || !IsPlayerControlled())
	{
		return;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	if (Now < DebugNextWeaponTime)
	{
		// Fire once halfway through, release shortly after
		const float Into = Cycle - (DebugNextWeaponTime - Now);
		if (CurrentWeapon && Into > Cycle * 0.5f && Into < Cycle * 0.5f + DeltaSeconds * 1.5f)
		{
			StartFire(0);
		}
		else if (CurrentWeapon && Into > Cycle * 0.5f + 0.35f && CurrentWeapon->IsFiring(0))
		{
			StopFire(0);
		}
		return;
	}
	DebugNextWeaponTime = Now + Cycle;
	for (const TSubclassOf<AArenaWeapon>& WeaponClass : ArenaWeaponList::AllWeapons())
	{
		if (!FindWeapon(WeaponClass))
		{
			GiveWeapon(WeaponClass);
		}
	}
	if (Weapons.Num() > 0)
	{
		StopFire(0);
		const int32 Index = (Weapons.IndexOfByKey(CurrentWeapon) + 1) % Weapons.Num();
		SwitchToWeapon(Weapons[Index]);
		UE_LOG(LogOpenUT2k4, Log, TEXT("DebugCycleWeapons: %s"), CurrentWeapon ? *CurrentWeapon->WeaponName : TEXT("-"));
	}
}
