#include "ArenaAS_Turret.h"

#include "OpenUT2k4.h"
#include "ArenaArt.h"
#include "ArenaAS_Objective.h"
#include "ArenaAssets.h"
#include "ArenaCharacter.h"
#include "ArenaDamage.h"
#include "ArenaFX.h"
#include "ArenaLocalMessages.h"
#include "ArenaGameState.h"
#include "ArenaONS_Vehicle.h"
#include "ArenaPlayerController.h"
#include "ArenaProjectile.h"
#include "ArenaRuntimeContent.h"
#include "ArenaUE2World.h"
#include "Animation/AnimSequence.h"
#include "Camera/CameraTypes.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "UScript/ArenaUScript.h"
#include "UScript/UScriptCore.h"
#include "UScript/UScriptRuntime.h"

int32 AArenaAS_Turret::NumShots = 0;
int32 AArenaAS_Turret::NumDestroyed = 0;

namespace ArenaASTurretLocal
{
	constexpr float S = ArenaUE2World::Scale;

	FString Default(FUSClass* Class, const TCHAR* Key)
	{
		FUSObject* Def = Class ? Class->GetDefaultObject() : nullptr;
		FString Name = Key;
		int32 Index = 0, Bracket = INDEX_NONE;
		if (Name.FindChar(TEXT('['), Bracket))
		{
			Index = FCString::Atoi(*Name.Mid(Bracket + 1));
			Name.LeftInline(Bracket);
		}
		FUSProperty* P = Def && Def->Data ? Def->FindProperty(FName(*Name)) : nullptr;
		if (!P || Index >= P->ArrayDim)
		{
			return FString();
		}
		const uint8* Value = Def->Data + P->Offset + Index * P->ElementSize;
		if (P->Type == EUSType::Byte)
		{
			return FString::FromInt(*Value);
		}
		FString Out;
		P->ExportText(Out, Value, true);
		int32 Quote = INDEX_NONE;
		if (Out.FindChar(TEXT('\''), Quote) && Out.EndsWith(TEXT("'")))
		{
			Out = Out.Mid(Quote + 1, Out.Len() - Quote - 2);
		}
		return Out == TEXT("None") ? FString() : Out;
	}

	FUSClass* FindClass(const FString& Name)
	{
		if (Name.IsEmpty())
		{
			return nullptr;
		}
		if (Name.Contains(TEXT(".")))
		{
			return FUSRuntime::Get().FindClass(Name);
		}
		for (const TCHAR* Pkg : { TEXT("UT2k4Assault"), TEXT("UT2k4AssaultFull") })
		{
			if (FUSClass* C = FUSRuntime::Get().FindClass(FString(Pkg) + TEXT(".") + Name))
			{
				return C;
			}
		}
		return nullptr;
	}

	bool IsA(FUSClass* Class, const TCHAR* Name)
	{
		for (FUSClass* C = Class; C; C = C->GetSuperClass())
		{
			if (C->GetName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	FString Content(const FString& ObjectPath, const TCHAR* Folder)
	{
		FString Pkg, Rest;
		if (ObjectPath.IsEmpty() || !ObjectPath.Split(TEXT("."), &Pkg, &Rest))
		{
			return FString();
		}
		return FString::Printf(TEXT("%s/%s/%s"), Folder, *Pkg, *Rest.Replace(TEXT("."), TEXT("_")));
	}

	FVector Vec(const FString& Text)
	{
		FVector V = FVector::ZeroVector;
		FParse::Value(*Text, TEXT("X="), V.X);
		FParse::Value(*Text, TEXT("Y="), V.Y);
		FParse::Value(*Text, TEXT("Z="), V.Z);
		return V;
	}

	float Num(FUSClass* Class, const TCHAR* Key, float Fallback)
	{
		const FString T = Default(Class, Key);
		return T.IsEmpty() ? Fallback : FCString::Atof(*T);
	}

	bool Bool(FUSClass* Class, const TCHAR* Key)
	{
		return Default(Class, Key).Equals(TEXT("True"), ESearchCase::IgnoreCase);
	}

	float RotDeg(float Units) { return FRotator::NormalizeAxis(Units * 360.f / 65536.f); }
}

FString ArenaASTurret::KillText(const FArenaASTurretDesc& Desc, const FString& Victim)
{
	// "Class'UT2k4Assault.DamTypeSentinelLaser'" -> package UT2k4Assault, section DamTypeSentinelLaser
	FString DamType = Desc.DamageType;
	int32 Quote = INDEX_NONE;
	if (DamType.FindChar(TEXT('\''), Quote))
	{
		DamType = DamType.Mid(Quote + 1).Replace(TEXT("'"), TEXT(""));
	}
	DamType.ReplaceInline(TEXT("\""), TEXT(""));
	FString Text, Pkg, Class;
	if (!DamType.Split(TEXT("."), &Pkg, &Class) || !ArenaLocalMessages::Localize(Pkg, Class, TEXT("DeathString"), Text) || Text.IsEmpty())
	{
		Text = TEXT("%o was killed by %k.");
	}
	Text.ReplaceInline(TEXT("%k"), *Desc.DisplayName);
	Text.ReplaceInline(TEXT("%o"), *Victim);
	return Text;
}

bool ArenaASTurret::DescribeFactory(const FString& FactoryClass, const FString& VehicleClassOverride, FArenaASTurretDesc& Out)
{
	using namespace ArenaASTurretLocal;
	if (!ArenaUScript::EnsureRuntime())
	{
		return false;
	}
	FUSClass* Factory = FindClass(FactoryClass);
	FString VehicleName = VehicleClassOverride;
	if (VehicleName.IsEmpty())
	{
		VehicleName = Default(Factory, TEXT("VehicleClass"));
	}
	if (VehicleName.IsEmpty())
	{
		VehicleName = Default(Factory, TEXT("VehicleClassStr"));
	}
	FUSClass* Vehicle = FindClass(VehicleName);
	if (!Vehicle || !(IsA(Vehicle, TEXT("ASTurret")) || IsA(Vehicle, TEXT("ASVehicle_Sentinel"))))
	{
		return false;
	}
	Out.ClassName = Vehicle->GetName();
	Out.DisplayName = Default(Vehicle, TEXT("VehicleNameString"));
	if (Out.DisplayName.IsEmpty())
	{
		Out.DisplayName = Out.ClassName;
	}
	const FString TeamText = Default(Factory, TEXT("VehicleTeam"));
	Out.Team = TeamText.IsEmpty() ? 1 : FMath::Clamp(FCString::Atoi(*TeamText), 0, 1);
	Out.Health = FMath::Max(50.f, Num(Vehicle, TEXT("Health"), 500.f));
	const bool bSentinel = IsA(Vehicle, TEXT("ASVehicle_Sentinel"));
	Out.bAuto = bSentinel || Bool(Vehicle, TEXT("bAutoTurret"));
	Out.bManned = !Bool(Vehicle, TEXT("bNonHumanControl")) && !bSentinel;
	Out.GunMesh = Content(Default(Vehicle, TEXT("Mesh")), TEXT("SkeletalMeshes"));
	if (FUSClass* Base = FindClass(Default(Vehicle, TEXT("TurretBaseClass"))))
	{
		const FString DT = Default(Base, TEXT("DrawType"));
		Out.bBaseSkeletal = DT == TEXT("2") || DT.Contains(TEXT("DT_Mesh"));
		Out.BaseMesh = Out.bBaseSkeletal ? Content(Default(Base, TEXT("Mesh")), TEXT("SkeletalMeshes")) : Content(Default(Base, TEXT("StaticMesh")), TEXT("StaticMeshes"));
		Out.BaseScale = Num(Base, TEXT("DrawScale"), 1.f);
		Out.BaseOffset = Vec(Default(Base, TEXT("PrePivot"))) * S;
	}
	if (FUSClass* Swivel = FindClass(Default(Vehicle, TEXT("TurretSwivelClass"))))
	{
		Out.bSwivelSkeletal = Default(Swivel, TEXT("DrawType")) == TEXT("2") || Default(Swivel, TEXT("DrawType")).Contains(TEXT("DT_Mesh"));
		Out.SwivelMesh = Out.bSwivelSkeletal ? Content(Default(Swivel, TEXT("Mesh")), TEXT("SkeletalMeshes")) : Content(Default(Swivel, TEXT("StaticMesh")), TEXT("StaticMeshes"));
		Out.SwivelScale = Num(Swivel, TEXT("DrawScale"), 1.f);
		Out.SwivelOffset = Vec(Default(Swivel, TEXT("PrePivot"))) * S;
	}
	Out.bDeploys = bSentinel;
	Out.OpenCloseSound = Default(Vehicle, TEXT("OpenCloseSound"));
	Out.PitchUp = FMath::Clamp(RotDeg(Num(Vehicle, TEXT("PitchUpLimit"), 18000.f)), 5.f, 89.f);
	Out.PitchDown = FMath::Clamp(RotDeg(Num(Vehicle, TEXT("PitchDownLimit"), 49153.f)), -89.f, -5.f);
	Out.EntryRadius = Num(Vehicle, TEXT("EntryRadius"), 120.f) * S;
	const FVector ProjOffset = Vec(Default(Vehicle, TEXT("VehicleProjSpawnOffset")));
	// the weapon: DefaultWeaponClassName -> FireModeClass[0] -> its projectile (TeamProjectileClasses / ProjectileClass)
	FUSClass* Weapon = FindClass(Default(Vehicle, TEXT("DefaultWeaponClassName")));
	FUSClass* Mode = FindClass(Default(Weapon, TEXT("FireModeClass[0]")));
	if (Mode)
	{
		Out.FireInterval = FMath::Max(0.05f, Num(Mode, TEXT("FireRate"), 0.25f));
		Out.bHitscan = Bool(Mode, TEXT("bInstantHit"));
		Out.Spread = Num(Mode, TEXT("Spread"), 0.f);
		Out.AimError = Num(Mode, TEXT("aimerror"), 300.f) * S * 0.25f;
		const FVector ModeOffset = Vec(Default(Mode, TEXT("ProjSpawnOffset")));
		Out.MuzzleOffset = (ProjOffset.IsNearlyZero() ? ModeOffset : ProjOffset) * S;
		const FString Snd = Default(Mode, TEXT("FireSound"));
		Out.FireSound = Snd;
		if (Out.bHitscan)
		{
			Out.Damage = Num(Mode, TEXT("Damage"), 0.f);
			if (Out.Damage <= 0.f)
			{
				Out.Damage = FMath::Max(Num(Mode, TEXT("DamageMax"), 0.f), Num(Mode, TEXT("DamageMin"), 0.f));
			}
			Out.Range = Num(Mode, TEXT("TraceRange"), 10000.f) * S;
			Out.DamageType = Default(Mode, TEXT("DamageType"));
			Out.Momentum = Num(Mode, TEXT("MomentumTransfer"), 0.f) * S;
		}
		FString ProjName = Default(Mode, *FString::Printf(TEXT("TeamProjectileClasses[%d]"), Out.Team));
		if (ProjName.IsEmpty())
		{
			ProjName = Default(Mode, TEXT("ProjectileClass"));
		}
		if (FUSClass* Proj = Out.bHitscan ? nullptr : FindClass(ProjName))
		{
			Out.Damage = Num(Proj, TEXT("Damage"), 20.f);
			Out.DamageRadius = Num(Proj, TEXT("DamageRadius"), 0.f) * S;
			Out.Momentum = Num(Proj, TEXT("MomentumTransfer"), 0.f) * S;
			Out.ProjSpeed = Num(Proj, TEXT("Speed"), 4000.f) * S;
			Out.ProjLifeSpan = Num(Proj, TEXT("LifeSpan"), 3.f);
			Out.DamageType = Default(Proj, TEXT("MyDamageType"));
		}
	}
	// the stock values the scripts compute instead of storing (the minigun's per-bullet damage, the ion cannon's blast)
	if (Out.bHitscan && Out.Damage <= 0.f)
	{
		if (Out.ClassName.Contains(TEXT("Ion")))
		{
			Out.Damage = 200.f;
			Out.DamageRadius = 500.f * S;
		}
		else
		{
			Out.Damage = 16.f;
		}
	}
	Out.Color = Out.Team == 0 ? FLinearColor(1.f, 0.35f, 0.25f) : FLinearColor(0.3f, 0.55f, 1.f);
	if (Out.ClassName.Contains(TEXT("Link")))
	{
		Out.Color = FLinearColor(0.3f, 1.f, 0.3f);
		// FM_LinkTurret_Fire spawns PROJ_LinkTurret_Plasma in script code (no ProjectileClass default)
		if (Out.DamageType.IsEmpty())
		{
			Out.DamageType = Out.bHitscan ? TEXT("UT2k4AssaultFull.DamTypeLinkTurretBeam") : TEXT("UT2k4AssaultFull.DamTypeLinkTurretPlasma");
		}
	}
	Out.SightRange = bSentinel ? 6000.f * S : 9000.f;
	Out.TurnRate = bSentinel ? 140.f : 110.f;
	return true;
}

// =================================================================================================
// AArenaAS_TurretController
// =================================================================================================

int32 AArenaAS_TurretController::TeamOf(const AController* C)
{
	const AArenaAS_TurretController* T = Cast<AArenaAS_TurretController>(C);
	return T ? T->Team : ArenaTeam::None;
}

FString AArenaAS_TurretController::KillText(const AController* C, const FString& Victim, FString& OutKillerName)
{
	const AArenaAS_TurretController* TC = Cast<AArenaAS_TurretController>(C);
	if (!TC)
	{
		return FString();
	}
	if (const AArenaAS_Turret* T = TC->Turret.Get())
	{
		OutKillerName = T->GetTurretDesc().DisplayName;
		return ArenaASTurret::KillText(T->GetTurretDesc(), Victim);
	}
	FArenaASTurretDesc None;
	None.DisplayName = OutKillerName = TEXT("a turret");
	return ArenaASTurret::KillText(None, Victim);
}

// =================================================================================================
// AArenaAS_Turret
// =================================================================================================

AArenaAS_Turret::AArenaAS_Turret()
{
	PrimaryActorTick.bCanEverTick = true;
	Solid = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Solid"));
	Solid->InitCapsuleSize(70.f, 90.f);
	Solid->SetCollisionProfileName(TEXT("Pawn"));
	Solid->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	RootComponent = Solid;
	bReplicates = true;
	AutoPossessAI = EAutoPossessAI::Disabled;
}

void AArenaAS_Turret::InitTurret(const FArenaASTurretDesc& InDesc)
{
	Desc = InDesc;
	Team = Desc.Team;
	Health = Desc.Health;
}

void AArenaAS_Turret::BeginPlay()
{
	Super::BeginPlay();
	HomeTransform = GetActorTransform();
	RestRotation = GetActorRotation();
	AimRotation = FRotator(0.f, RestRotation.Yaw, 0.f);
	BuildModel();
	if (!Desc.FireSound.IsEmpty())
	{
		FireSoundAsset = ArenaRuntimeContent::LoadSoundByObjectPath(Desc.FireSound);
	}
	if (!Desc.OpenCloseSound.IsEmpty())
	{
		OpenCloseSoundAsset = ArenaRuntimeContent::LoadSoundByObjectPath(Desc.OpenCloseSound);
	}
	if (HasAuthority() && GetWorld())
	{
		// the instigator of its own shots (UE2 AutoTurretController): its team, its kills
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		P.ObjectFlags |= RF_Transient;
		AutoController = GetWorld()->SpawnActor<AArenaAS_TurretController>(AArenaAS_TurretController::StaticClass(), GetActorTransform(), P);
		if (AutoController)
		{
			AutoController->Team = Team;
			AutoController->Turret = this;
		}
	}
	if (Desc.bDeploys)
	{
		PlayTurretAnim(TEXT("IdleClosed"), true);
	}
}

void AArenaAS_Turret::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (AutoController)
	{
		AutoController->Destroy();
		AutoController = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

float AArenaAS_Turret::PlayTurretAnim(const TCHAR* Seq, bool bLoop)
{
	float Length = 0.f;
	for (USkeletalMeshComponent* C : { GunComp.Get(), BaseSkel.Get(), SwivelSkel.Get() })
	{
		USkeletalMesh* M = C ? C->GetSkeletalMeshAsset() : nullptr;
		if (!M)
		{
			continue;
		}
		// the mesh's own animation set (the importer names it after the mesh: Animations/<Pkg>/<Mesh>_<Seq>)
		FString Path = C == GunComp ? Desc.GunMesh : (C == BaseSkel ? Desc.BaseMesh : Desc.SwivelMesh);
		Path = Path.Replace(TEXT("SkeletalMeshes/"), TEXT("Animations/")) + TEXT("_") + Seq;
		UAnimSequence* A = ArenaArt::Load<UAnimSequence>(Path);
		if (A && A->GetSkeleton() == M->GetSkeleton())
		{
			C->PlayAnimation(A, bLoop);
			Length = FMath::Max(Length, A->GetPlayLength());
		}
	}
	return Length;
}

void AArenaAS_Turret::SetDeployState(int32 NewState)
{
	if (NewState == DeployState)
	{
		return;
	}
	DeployState = NewState;
	DeployStateTime = GetWorld()->GetTimeSeconds();
	switch (NewState)
	{
	case 1:
	case 3:
	{
		// UE2 ASVehicle_Sentinel: Open / Close with OpenCloseSound; the state ends with the animation
		const float Len = PlayTurretAnim(NewState == 1 ? TEXT("Open") : TEXT("Close"), false);
		DeployEndTime = DeployStateTime + FMath::Max(0.5f, Len);
		static int32 Logged = 0;
		if (Logged++ < 4)
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("AS: %s %s (animation %.2f s%s)"), *Desc.DisplayName, NewState == 1 ? TEXT("opens") : TEXT("closes"), Len,
				OpenCloseSoundAsset ? TEXT(", sound") : TEXT(", no sound"));
		}
		if (OpenCloseSoundAsset)
		{
			UGameplayStatics::PlaySoundAtLocation(this, OpenCloseSoundAsset, GetActorLocation());
		}
		break;
	}
	case 2:
		PlayTurretAnim(TEXT("IdleOpen"), true);
		break;
	default:
		PlayTurretAnim(TEXT("IdleClosed"), true);
		break;
	}
}

void AArenaAS_Turret::BuildModel()
{
	auto MakeStatic = [this](const FString& Path, float Scale, USceneComponent* Parent) -> UStaticMeshComponent*
	{
		UStaticMesh* M = Path.IsEmpty() ? nullptr : ArenaArt::Load<UStaticMesh>(Path);
		if (!M)
		{
			return nullptr;
		}
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
		C->SetStaticMesh(M);
		C->SetupAttachment(Parent);
		C->SetRelativeScale3D(FVector(Scale));
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->RegisterComponent();
		return C;
	};
	// UE2 ASTurret: the base stays, the swivel turns with the yaw, the gun (the turret's own mesh) with yaw and pitch;
	// all three at the turret's location
	if (Desc.bBaseSkeletal)
	{
		if (USkeletalMesh* M = Desc.BaseMesh.IsEmpty() ? nullptr : ArenaArt::Load<USkeletalMesh>(Desc.BaseMesh))
		{
			BaseSkel = NewObject<USkeletalMeshComponent>(this);
			BaseSkel->SetSkeletalMeshAsset(M);
			BaseSkel->SetupAttachment(Solid);
			BaseSkel->SetRelativeLocation(Desc.BaseOffset);
			BaseSkel->SetRelativeScale3D(FVector(Desc.BaseScale));
			BaseSkel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			BaseSkel->RegisterComponent();
		}
	}
	else
	{
		BaseComp = MakeStatic(Desc.BaseMesh, Desc.BaseScale, Solid);
		if (BaseComp)
		{
			BaseComp->SetRelativeLocation(Desc.BaseOffset);
		}
	}
	SwivelRoot = NewObject<USceneComponent>(this, TEXT("SwivelRoot"));
	SwivelRoot->SetupAttachment(Solid);
	SwivelRoot->SetUsingAbsoluteRotation(true);
	SwivelRoot->RegisterComponent();
	if (Desc.bSwivelSkeletal)
	{
		if (USkeletalMesh* M = Desc.SwivelMesh.IsEmpty() ? nullptr : ArenaArt::Load<USkeletalMesh>(Desc.SwivelMesh))
		{
			SwivelSkel = NewObject<USkeletalMeshComponent>(this);
			SwivelSkel->SetSkeletalMeshAsset(M);
			SwivelSkel->SetupAttachment(SwivelRoot);
			SwivelSkel->SetRelativeScale3D(FVector(Desc.SwivelScale));
			SwivelSkel->SetRelativeLocation(Desc.SwivelOffset);
			SwivelSkel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			SwivelSkel->RegisterComponent();
		}
	}
	else
	{
		SwivelStatic = MakeStatic(Desc.SwivelMesh, Desc.SwivelScale, SwivelRoot);
		if (SwivelStatic)
		{
			SwivelStatic->SetRelativeLocation(Desc.SwivelOffset);
		}
	}
	GunRoot = NewObject<USceneComponent>(this, TEXT("GunRoot"));
	GunRoot->SetupAttachment(Solid);
	GunRoot->SetUsingAbsoluteRotation(true);
	GunRoot->RegisterComponent();
	if (USkeletalMesh* M = Desc.GunMesh.IsEmpty() ? nullptr : ArenaArt::Load<USkeletalMesh>(Desc.GunMesh))
	{
		GunComp = NewObject<USkeletalMeshComponent>(this);
		GunComp->SetSkeletalMeshAsset(M);
		GunComp->SetupAttachment(GunRoot);
		GunComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		GunComp->RegisterComponent();
		// the damage capsule around the model
		const FBoxSphereBounds B = M->GetBounds();
		Solid->SetCapsuleSize(FMath::Clamp(float(FMath::Max(B.BoxExtent.X, B.BoxExtent.Y)) * 0.7f, 40.f, 250.f), FMath::Clamp(float(B.BoxExtent.Z), 50.f, 300.f));
	}
	if (!GunComp)
	{
		// no art: a stand-in
		StandIn = NewObject<UStaticMeshComponent>(this);
		StandIn->SetStaticMesh(ArenaAssets::Cylinder());
		StandIn->SetupAttachment(GunRoot);
		StandIn->SetRelativeScale3D(FVector(0.4f, 0.4f, 1.2f));
		StandIn->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		StandIn->RegisterComponent();
	}
	SwivelRoot->SetWorldRotation(FRotator(0.f, AimRotation.Yaw, 0.f));
	GunRoot->SetWorldRotation(AimRotation);
}

void AArenaAS_Turret::SetLinkedObjective(AArenaAS_Objective* InObjective)
{
	LinkedObjective = InObjective;
}

void AArenaAS_Turret::SetTeam(int32 InTeam)
{
	Team = InTeam;
	if (AutoController)
	{
		AutoController->Team = InTeam;
	}
}

bool AArenaAS_Turret::CanBeEnteredBy(const AArenaCharacter* Char) const
{
	return Desc.bManned && !bWrecked && bActiveTurret && !Gunner.IsValid() && Char && Char->IsAlive() && Char->GetController()
		&& Char->GetTeam() == Team && FVector::Dist(Char->GetActorLocation(), GetActorLocation()) < Desc.EntryRadius + 150.f;
}

bool AArenaAS_Turret::EnterTurret(AArenaCharacter* Char)
{
	AController* C = Char ? Char->GetController() : nullptr;
	if (!C || !HasAuthority() || !CanBeEnteredBy(Char))
	{
		return false;
	}
	ArenaONS::HideRider(Char, Solid, FVector(0.f, 0.f, -40.f), GunnerHidden);
	Gunner = Char;
	EnterTime = GetWorld()->GetTimeSeconds();
	C->Possess(this);
	C->SetControlRotation(AimRotation);
	ArenaAssets::PlaySoundAt(this, TEXT("S_WeaponSwitch"), GetActorLocation(), 0.8f, 0.7f);
	const APlayerState* PS = C->PlayerState;
	UE_LOG(LogOpenUT2k4, Log, TEXT("AS: %s manned the %s"), PS ? *PS->GetPlayerName() : TEXT("?"), *Desc.DisplayName);
	return true;
}

AArenaCharacter* AArenaAS_Turret::LeaveTurret(bool bForced)
{
	AArenaCharacter* Char = Gunner.Get();
	if (!Char || !HasAuthority())
	{
		Gunner.Reset();
		return nullptr;
	}
	AController* C = Controller;
	const float Yaw = AimRotation.Yaw;
	// UE2 ExitPositions: beside the turret
	FVector Exit = GetActorLocation() + FRotator(0.f, Yaw, 0.f).RotateVector(FVector(-Desc.EntryRadius * 0.5f, Desc.EntryRadius, 60.f));
	FHitResult Hit;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaASTurretExit), false, this);
	Q.AddIgnoredActor(Char);
	if (GetWorld()->LineTraceSingleByChannel(Hit, GetActorLocation() + FVector(0.f, 0.f, 60.f), Exit, ECC_WorldStatic, Q))
	{
		Exit = Hit.Location - (Exit - GetActorLocation()).GetSafeNormal() * 60.f;
	}
	ArenaONS::RestoreRider(Char, Exit, Yaw, GunnerHidden);
	GunnerHidden.Reset();
	Gunner.Reset();
	if (C)
	{
		C->Possess(Char);
		C->SetControlRotation(FRotator(0.f, Yaw, 0.f));
	}
	return Char;
}

void AArenaAS_Turret::ResetTurret()
{
	if (Gunner.IsValid())
	{
		LeaveTurret(true);
	}
	bWrecked = false;
	Health = Desc.Health;
	SetActorHiddenInGame(false);
	SetActorEnableCollision(true);
	SetActorTransform(HomeTransform);
	AimRotation = FRotator(0.f, HomeTransform.Rotator().Yaw, 0.f);
	Target.Reset();
	if (Desc.bDeploys)
	{
		DeployState = -1;
		SetDeployState(0);
	}
}

float AArenaAS_Turret::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	if (!HasAuthority() || bWrecked || DamageAmount <= 0.f)
	{
		return 0.f;
	}
	const AArenaGameState* GS = GetWorld()->GetGameState<AArenaGameState>();
	if (GS && !GS->IsPlaying())
	{
		return 0.f;
	}
	if (EventInstigator && ArenaONS::TeamOfController(EventInstigator) == Team)
	{
		return 0.f; // no friendly fire on turrets
	}
	if (AArenaAS_Objective* Obj = LinkedObjective.Get())
	{
		// the objective's vehicle: its health is the objective's
		Obj->ApplyAttackDamage(DamageAmount, EventInstigator);
		Health = Desc.Health * Obj->GetHealthFraction();
		if (Desc.bAuto && EventInstigator && EventInstigator->GetPawn())
		{
			Target = EventInstigator->GetPawn();
		}
		return DamageAmount;
	}
	Health -= DamageAmount;
	if (Desc.bAuto && EventInstigator && EventInstigator->GetPawn())
	{
		Target = EventInstigator->GetPawn(); // shot at: turn on the shooter
	}
	if (Health <= 0.f)
	{
		Wreck(EventInstigator);
	}
	return DamageAmount;
}

void AArenaAS_Turret::Wreck(AController* Killer)
{
	bWrecked = true;
	Health = 0.f;
	++NumDestroyed;
	UWorld* World = GetWorld();
	if (Gunner.IsValid())
	{
		AArenaCharacter* Char = LeaveTurret(true);
		if (Char && Char->IsAlive())
		{
			FArenaHitInfo Info;
			Info.WeaponName = Desc.DisplayName;
			Char->ReceiveArenaDamage(Desc.Health * 0.25f, Killer, this, Info);
		}
	}
	ArenaFX::SpawnExplosion(World, GetActorLocation() + FVector(0.f, 0.f, 60.f), FLinearColor(1.f, 0.5f, 0.15f), 450.f, TEXT("Explosion.Rocket"));
	ArenaAssets::PlaySoundAt(this, TEXT("S_ExplosionBig"), GetActorLocation(), 1.2f, 0.9f);
	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
	const APlayerState* KPS = Killer ? Killer->PlayerState.Get() : nullptr;
	UE_LOG(LogOpenUT2k4, Log, TEXT("AS: %s (team %d) destroyed by %s"), *Desc.DisplayName, Team, KPS ? *KPS->GetPlayerName() : TEXT("nobody"));
}

bool AArenaAS_Turret::CanSee(const AActor* InTarget, FVector& OutAimPoint) const
{
	if (!InTarget)
	{
		return false;
	}
	const FVector From = GunRoot ? GunRoot->GetComponentLocation() : GetActorLocation();
	OutAimPoint = InTarget->GetActorLocation() + FVector(0.f, 0.f, 20.f);
	if (FVector::DistSquared(From, OutAimPoint) > FMath::Square(Desc.SightRange))
	{
		return false;
	}
	FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaASTurretSight), false, this);
	Q.AddIgnoredActor(InTarget);
	if (const APawn* P = Cast<APawn>(InTarget))
	{
		TArray<AActor*> Attached;
		P->GetAttachedActors(Attached);
		Q.AddIgnoredActors(Attached);
	}
	return !GetWorld()->LineTraceTestByChannel(From, OutAimPoint, ECC_Visibility, Q);
}

AActor* AArenaAS_Turret::FindTarget() const
{
	AActor* Best = nullptr;
	float BestDist = TNumericLimits<float>::Max();
	FVector Aim;
	for (TActorIterator<AArenaCharacter> It(GetWorld()); It; ++It)
	{
		AArenaCharacter* C = *It;
		if (!C || !C->IsAlive() || C->IsHidden() || C->GetTeam() == Team || (C->GetTeam() != 0 && C->GetTeam() != 1))
		{
			continue;
		}
		const float D = float(FVector::DistSquared(C->GetActorLocation(), GetActorLocation()));
		if (D < BestDist && CanSee(C, Aim))
		{
			BestDist = D;
			Best = C;
		}
	}
	for (TActorIterator<AArenaONS_Vehicle> It(GetWorld()); It; ++It)
	{
		AArenaONS_Vehicle* V = *It;
		if (!V || V->IsEmpty() || V->GetTeam() == Team)
		{
			continue;
		}
		const float D = float(FVector::DistSquared(V->GetActorLocation(), GetActorLocation()));
		if (D < BestDist && CanSee(V, Aim))
		{
			BestDist = D;
			Best = V;
		}
	}
	return Best;
}

void AArenaAS_Turret::AimAt(const FRotator& Desired, float DeltaSeconds)
{
	const float Step = Desc.TurnRate * DeltaSeconds;
	AimRotation.Yaw = FMath::FixedTurn(AimRotation.Yaw, Desired.Yaw, Step);
	AimRotation.Pitch = FMath::Clamp(FMath::FixedTurn(AimRotation.Pitch, FRotator::NormalizeAxis(Desired.Pitch), Step), Desc.PitchDown, Desc.PitchUp);
	AimRotation.Roll = 0.f;
}

FVector AArenaAS_Turret::GetMuzzle() const
{
	const FVector Pivot = GunRoot ? GunRoot->GetComponentLocation() : GetActorLocation();
	return Pivot + AimRotation.RotateVector(Desc.MuzzleOffset);
}

void AArenaAS_Turret::Fire(AController* InstigatorController)
{
	UWorld* World = GetWorld();
	const FVector Muzzle = GetMuzzle();
	FVector Dir = AimRotation.Vector();
	if (Desc.Spread > 0.f)
	{
		Dir = FMath::VRandCone(Dir, Desc.Spread);
	}
	++NumShots;
	if (FireSoundAsset)
	{
		UGameplayStatics::PlaySoundAtLocation(this, FireSoundAsset, Muzzle, 0.8f);
	}
	ArenaFX::SpawnImpact(World, Muzzle, Dir, Desc.Color, Desc.bHitscan ? 14.f : 28.f, Desc.bHitscan ? TEXT("Muzzle.Minigun") : TEXT("Muzzle.Rocket"));
	if (!Desc.bHitscan)
	{
		FArenaProjectileParams P;
		P.Speed = Desc.ProjSpeed;
		P.MaxSpeed = Desc.ProjSpeed;
		P.Damage = Desc.Damage;
		P.DamageRadius = Desc.DamageRadius;
		P.Momentum = Desc.Momentum;
		P.LifeSpan = Desc.ProjLifeSpan;
		P.Size = 14.f;
		P.Color = Desc.Color;
		P.WeaponName = Desc.DisplayName;
		const FTransform T(Dir.Rotation(), Muzzle);
		if (AArenaProjectile* Proj = World->SpawnActorDeferred<AArenaProjectile>(AArenaProjectile::StaticClass(), T, this, this, ESpawnActorCollisionHandlingMethod::AlwaysSpawn))
		{
			Proj->Init(P, InstigatorController, 1.f);
			Proj->FinishSpawning(T);
		}
		return;
	}
	FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaASTurretShot), false, this);
	if (Gunner.IsValid())
	{
		Q.AddIgnoredActor(Gunner.Get());
	}
	const FVector End = Muzzle + Dir * Desc.Range;
	FHitResult Hit;
	const bool bHit = World->LineTraceSingleByChannel(Hit, Muzzle, End, ECC_Weapon, Q);
	const FVector HitPoint = bHit ? FVector(Hit.ImpactPoint) : End;
	if (bHit)
	{
		FArenaHitInfo Info;
		Info.WeaponName = Desc.DisplayName;
		Info.Momentum = Dir * Desc.Momentum;
		if (Desc.DamageRadius > 0.f)
		{
			ArenaDamage::ApplyRadialDamage(World, HitPoint, Desc.Damage, Desc.DamageRadius, Desc.Momentum, InstigatorController, this, Desc.DisplayName, 0.f, this);
			ArenaFX::SpawnExplosion(World, HitPoint, Desc.Color, Desc.DamageRadius, TEXT("Explosion.ShockCombo"));
		}
		else if (AActor* Victim = Hit.GetActor())
		{
			ArenaDamage::ApplyDamage(Victim, Desc.Damage, InstigatorController, this, Info);
		}
		ArenaFX::SpawnImpact(World, Hit.ImpactPoint, Hit.ImpactNormal, Desc.Color, 10.f, TEXT("Impact.Bullet"));
	}
	ArenaFX::SpawnBeam(World, Muzzle, HitPoint, Desc.Color, Desc.DamageRadius > 0.f ? 12.f : 2.f, Desc.DamageRadius > 0.f ? 0.5f : 0.07f,
		Desc.DamageRadius > 0.f ? TEXT("Beam.Shock") : TEXT("Tracer.Bullet"));
}

FRotator AArenaAS_Turret::GetViewRotation() const
{
	if (Controller && Controller->IsA<APlayerController>())
	{
		return Controller->GetControlRotation();
	}
	return AimRotation;
}

FVector AArenaAS_Turret::GetPawnViewLocation() const
{
	return (GunRoot ? GunRoot->GetComponentLocation() : GetActorLocation()) + FRotator(0.f, AimRotation.Yaw, 0.f).RotateVector(Desc.CameraOffset);
}

void AArenaAS_Turret::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
	const FRotator View = GetViewRotation();
	const FVector Pivot = (GunRoot ? GunRoot->GetComponentLocation() : GetActorLocation()) + FVector(0.f, 0.f, 120.f);
	FVector Desired = Pivot - View.Vector() * 420.f;
	FHitResult Hit;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaASTurretCam), false, this);
	if (GetWorld()->SweepSingleByChannel(Hit, Pivot, Desired, FQuat::Identity, ECC_Camera, FCollisionShape::MakeSphere(12.f), Q))
	{
		Desired = Hit.Location;
	}
	OutResult.Location = Desired;
	OutResult.Rotation = View;
	OutResult.FOV = 90.f;
}

void AArenaAS_Turret::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UWorld* World = GetWorld();
	if (!World || bWrecked)
	{
		return;
	}
	if (const AArenaAS_Objective* Obj = LinkedObjective.Get(); Obj && Obj->IsCompleted() && HasAuthority())
	{
		Wreck(nullptr); // destroyed through its objective
		return;
	}
	const float Now = World->GetTimeSeconds();
	const AArenaGameState* GS = World->GetGameState<AArenaGameState>();
	const bool bPlaying = !GS || GS->IsPlaying();
	APlayerController* PC = Cast<APlayerController>(Controller);
	if (HasAuthority() && Gunner.IsValid() && (!Gunner->IsAlive() || !Controller))
	{
		Gunner.Reset();
	}
	if (HasAuthority() && PC && Gunner.IsValid())
	{
		// the gunner: aim with the view, fire with fire, get out with use
		const AArenaPlayerController* APC = Cast<AArenaPlayerController>(PC);
		if (!(APC && APC->IsPauseMenuOpen()))
		{
			AimAt(PC->GetControlRotation(), DeltaSeconds * 4.f);
			const bool bFire = PC->IsInputKeyDown(EKeys::LeftMouseButton) || PC->IsInputKeyDown(EKeys::Gamepad_RightTrigger);
			if (bFire && bPlaying && Now >= NextFireTime)
			{
				NextFireTime = Now + Desc.FireInterval;
				Fire(PC);
			}
			if ((PC->WasInputKeyJustPressed(EKeys::E) || PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Left)) && Now - EnterTime > 0.5f)
			{
				LeaveTurret(false);
			}
		}
	}
	else if (HasAuthority() && bPlaying && bActiveTurret && (Gunner.IsValid() || Desc.bAuto))
	{
		// a bot gunner or the turret's own controller (UE2 TurretController / ASSentinelController): the nearest seen enemy
		if (Now >= NextTargetTime)
		{
			NextTargetTime = Now + 0.4f;
			AActor* Current = Target.Get();
			FVector Aim;
			const bool bKeep = Current && CanSee(Current, Aim) && !(Cast<AArenaCharacter>(Current) && !Cast<AArenaCharacter>(Current)->IsAlive());
			Target = bKeep ? Current : FindTarget();
			if (Target.IsValid())
			{
				CanSee(Target.Get(), TargetAim);
				TargetAim += FMath::VRand() * Desc.AimError * FMath::FRand();
			}
		}
		if (Desc.bDeploys)
		{
			// UE2 ASVehicle_Sentinel: open when an enemy shows up, close a while after the last one
			if (Target.IsValid())
			{
				LastTargetTime = Now;
				if (DeployState == 0 || DeployState == 3)
				{
					SetDeployState(1);
				}
			}
			else if (DeployState == 2 && Now - LastTargetTime > 5.f)
			{
				SetDeployState(3);
			}
			if ((DeployState == 1 || DeployState == 3) && Now >= DeployEndTime)
			{
				SetDeployState(DeployState == 1 ? 2 : 0);
			}
		}
		const bool bCanFire = !Desc.bDeploys || DeployState == 2;
		if (AActor* T = bCanFire ? Target.Get() : nullptr)
		{
			// lead moving targets for projectiles
			FVector AimPoint = TargetAim;
			if (!Desc.bHitscan && Desc.ProjSpeed > 0.f)
			{
				AimPoint += T->GetVelocity() * (FVector::Dist(GetMuzzle(), AimPoint) / Desc.ProjSpeed);
			}
			const FRotator Desired = (AimPoint - (GunRoot ? GunRoot->GetComponentLocation() : GetActorLocation())).Rotation();
			AimAt(Desired, DeltaSeconds);
			const float Off = FMath::Abs(FRotator::NormalizeAxis(Desired.Yaw - AimRotation.Yaw)) + FMath::Abs(FRotator::NormalizeAxis(Desired.Pitch - AimRotation.Pitch));
			if (Off < 6.f && Now >= NextFireTime)
			{
				NextFireTime = Now + Desc.FireInterval;
				Fire(Gunner.IsValid() ? Controller.Get() : AutoController.Get());
				if (Desc.bDeploys)
				{
					PlayTurretAnim(TEXT("Fire"), false);
				}
			}
		}
		else
		{
			// idle: slowly back to the rest direction
			AimAt(FRotator(0.f, RestRotation.Yaw, 0.f), DeltaSeconds * 0.3f);
		}
	}
	if (SwivelRoot)
	{
		SwivelRoot->SetWorldRotation(FRotator(0.f, AimRotation.Yaw, 0.f));
	}
	if (GunRoot)
	{
		GunRoot->SetWorldRotation(AimRotation);
	}
}
