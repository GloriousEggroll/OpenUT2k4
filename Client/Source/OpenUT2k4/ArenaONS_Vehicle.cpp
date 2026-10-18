#include "ArenaONS_Vehicle.h"
#include "ArenaAS_Turret.h"
#include "ArenaUE2Effects.h"
#include "ArenaViewShake.h"
#include "ArenaWorldZones.h"
#include "OpenUT2k4.h"
#include "ArenaONS_VehicleFactory.h"
#include "ArenaONS_PowerNode.h"
#include "ArenaRules_ONS.h"
#include "ArenaAssets.h"
#include "ArenaFX.h"
#include "ArenaDamage.h"
#include "ArenaProjectile.h"
#include "ArenaCharacter.h"
#include "ArenaWeapon.h"
#include "ArenaGameMode.h"
#include "ArenaGameState.h"
#include "ArenaPlayerState.h"
#include "ArenaPlayerController.h"
#include "ArenaBotController.h"
#include "ArenaNavGraph.h"
#include "ArenaArt.h"
#include "ArenaArtVehicles.h"
#include "Components/PoseableMeshComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "Camera/CameraActor.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Camera/CameraComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "EngineUtils.h"
#include "CollisionQueryParams.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Net/UnrealNetwork.h"

// =============================================================================================
// Tuning tables
// =============================================================================================

namespace ArenaONSVeh
{
	FArenaProjectileParams MakeProjectile(float Speed, float Damage, float Radius, float Size, const FLinearColor& Color,
		float Gravity = 0.f, float Momentum = 0.f, float LifeSpan = 3.f, FName Sound = NAME_None)
	{
		FArenaProjectileParams P;
		P.Speed = Speed;
		P.MaxSpeed = Speed;
		P.Damage = Damage;
		P.DamageRadius = Radius;
		P.Size = Size;
		P.Color = Color;
		P.GravityScale = Gravity;
		P.Momentum = Momentum;
		P.LifeSpan = LifeSpan;
		P.ExplosionSound = Sound;
		return P;
	}

	TArray<FArenaONS_VehicleSpec> BuildSpecs()
	{
		TArray<FArenaONS_VehicleSpec> Specs;
		Specs.SetNum(int32(EArenaONS_VehicleType::Num));

		// ---- Wisp: light hover scout ----
		{
			FArenaONS_VehicleSpec& S = Specs[int32(EArenaONS_VehicleType::Wisp)];
			S.Name = TEXT("Wisp");
			S.Role = TEXT("Hover scout");
			S.Mode = EArenaONS_DriveMode::Hover;
			S.Mass = 350.f;
			S.HalfExtent = FVector(120.f, 62.f, 22.f);
			S.MaxHealth = 220.f;
			S.SuspensionPoints = { FVector(80.f, 48.f, -18.f), FVector(80.f, -48.f, -18.f), FVector(-80.f, 48.f, -18.f), FVector(-80.f, -48.f, -18.f) };
			S.RestLength = 80.f;
			S.WheelRadius = 0.f;
			S.RestCompression = 0.4f;
			S.DampingRatio = 0.5f;
			S.GravityScale = 1.3f;
			S.MaxSpeed = 2100.f;
			S.ReverseSpeed = 1000.f;
			S.Accel = 2400.f;
			S.LateralGrip = 3.2f;
			S.YawRate = 200.f;
			S.JumpSpeed = 900.f;
			S.CameraDistance = 520.f;
			S.CameraHeight = 170.f;
			S.RespawnTime = 15.f;
			S.BotArriveRadius = 700.f;
			S.RoadkillSpeed = 800.f;
			FArenaONS_GunSpec G;
			G.Name = TEXT("Wisp Plasma");
			G.Projectile = MakeProjectile(5200.f, 24.f, 0.f, 8.f, FLinearColor(0.3f, 1.f, 0.85f), 0.f, 200.f, 2.5f, TEXT("S_Impact"));
			G.Projectile.Style = TEXT("LinkBolt"); // Manta plasma
			G.RefireTime = 0.18f;
			G.HeatPerShot = 0.06f;
			G.CoolRate = 0.4f;
			G.Muzzles = { FVector(140.f, 40.f, -2.f), FVector(140.f, -40.f, -2.f) };
			G.AimCone = 22.f;
			G.Color = G.Projectile.Color;
			G.Sound = TEXT("S_Fire_Link");
			S.Guns.Add(G);
		}

		// ---- Dustrunner: 4-wheel buggy ----
		{
			FArenaONS_VehicleSpec& S = Specs[int32(EArenaONS_VehicleType::Dustrunner)];
			S.Name = TEXT("Dustrunner");
			S.Role = TEXT("Raider buggy");
			S.Mode = EArenaONS_DriveMode::Wheeled;
			S.Mass = 700.f;
			S.HalfExtent = FVector(150.f, 85.f, 28.f);
			S.MaxHealth = 300.f;
			S.SuspensionPoints = { FVector(105.f, 80.f, -20.f), FVector(105.f, -80.f, -20.f), FVector(-105.f, 80.f, -20.f), FVector(-105.f, -80.f, -20.f) };
			S.RestLength = 40.f;
			S.WheelRadius = 38.f;
			S.RestCompression = 0.45f;
			S.DampingRatio = 0.6f;
			S.GravityScale = 1.6f;
			S.MaxSpeed = 2600.f;
			S.ReverseSpeed = 900.f;
			S.Accel = 1700.f;
			S.Brake = 3200.f;
			S.LateralGrip = 7.f;
			S.YawRate = 115.f;
			S.CameraDistance = 560.f;
			S.CameraHeight = 170.f;
			S.RespawnTime = 15.f;
			S.BotArriveRadius = 800.f;
			FArenaONS_GunSpec G;
			G.Name = TEXT("Spark Bolts");
			G.Projectile = MakeProjectile(4200.f, 34.f, 0.f, 10.f, FLinearColor(1.f, 0.75f, 0.2f), 0.f, 300.f, 2.5f, TEXT("S_Impact"));
			G.Projectile.Style = TEXT("KrallBolt"); // yellow energy bolt
			G.RefireTime = 0.32f;
			G.HeatPerShot = 0.11f;
			G.CoolRate = 0.35f;
			G.Muzzles = { FVector(175.f, 30.f, 26.f), FVector(175.f, -30.f, 26.f) };
			G.AimCone = 55.f;
			G.Color = G.Projectile.Color;
			G.Sound = TEXT("S_Fire_Pulse");
			S.Guns.Add(G);
			FArenaONS_GunSpec B;
			B.Name = TEXT("Nitro");
			B.FireMode = 1;
			B.bBoost = true;
			B.RefireTime = 4.f;
			B.HeatPerShot = 1.f;
			B.CoolRate = 0.25f;
			S.Guns.Add(B);
		}

		// ---- Bulwark: heavy tank ----
		{
			FArenaONS_VehicleSpec& S = Specs[int32(EArenaONS_VehicleType::Bulwark)];
			S.Name = TEXT("Bulwark");
			S.Role = TEXT("Siege tank");
			S.Mode = EArenaONS_DriveMode::Tracked;
			S.Mass = 4000.f;
			S.HalfExtent = FVector(240.f, 150.f, 50.f);
			S.MaxHealth = 800.f;
			S.SuspensionPoints = { FVector(170.f, 120.f, -45.f), FVector(170.f, -120.f, -45.f), FVector(0.f, 120.f, -45.f), FVector(0.f, -120.f, -45.f),
				FVector(-170.f, 120.f, -45.f), FVector(-170.f, -120.f, -45.f) };
			S.RestLength = 45.f;
			S.WheelRadius = 25.f;
			S.RestCompression = 0.45f;
			S.DampingRatio = 0.7f;
			S.GravityScale = 1.6f;
			S.MaxSpeed = 1150.f;
			S.ReverseSpeed = 600.f;
			S.Accel = 900.f;
			S.Brake = 2000.f;
			S.LateralGrip = 10.f;
			S.YawRate = 55.f;
			S.CameraDistance = 900.f;
			S.CameraHeight = 300.f;
			S.RespawnTime = 30.f;
			S.TurretRate = 75.f;
			S.TurretMinPitch = -8.f;
			S.TurretMaxPitch = 30.f;
			S.BotArriveRadius = 2600.f;
			S.bBotStaysAtObjective = true;
			S.RoadkillSpeed = 450.f;
			FArenaONS_GunSpec G;
			G.Name = TEXT("Siege Shell");
			G.Shake = ArenaViewShake::Blast(); // ONSHoverTankCannon
			// ONSHoverTank: bEnableProximityViewShake, ViewShakeRadius 600, ViewShakeOffsetMag (0.5, 0, 2), ViewShakeOffsetFreq 7
			S.ProximityShakeRadius = 600.f * 2.f;
			S.ProximityShakeOffsetMag = FVector(0.5f, 0.f, 2.f);
			S.ProximityShakeFreq = 7.f;
			G.bTurret = true;
			G.Projectile = MakeProjectile(5200.f, 230.f, 520.f, 18.f, FLinearColor(1.f, 0.5f, 0.15f), 0.35f, 1200.f, 5.f, TEXT("S_ExplosionBig"));
			G.Projectile.Style = TEXT("Rocket"); // tank shell
			G.RefireTime = 2.4f;
			G.HeatPerShot = 0.95f;
			G.CoolRate = 0.42f;
			G.Muzzles = { FVector(330.f, 0.f, 0.f) };
			G.Color = G.Projectile.Color;
			G.Sound = TEXT("S_Fire_Rocket");
			S.Guns.Add(G);
			FArenaONS_GunSpec C;
			C.Name = TEXT("Coax Repeater");
			C.Shake = ArenaViewShake::AssaultFire(); // ONSTankSecondaryTurret
			C.FireMode = 1;
			C.bTurret = true;
			C.bHitscan = true;
			C.Damage = 15.f;
			C.Range = 8000.f;
			C.Spread = 1.4f;
			C.Momentum = 150.f;
			C.RefireTime = 0.1f;
			C.HeatPerShot = 0.035f;
			C.CoolRate = 0.3f;
			C.BeamWidth = 2.f;
			C.Muzzles = { FVector(120.f, 42.f, -8.f) };
			C.Color = FLinearColor(1.f, 0.9f, 0.5f);
			C.Sound = TEXT("S_Fire_Chaingun");
			S.Guns.Add(C);
		}

		// ---- Kestrel: flying gunship ----
		{
			FArenaONS_VehicleSpec& S = Specs[int32(EArenaONS_VehicleType::Kestrel)];
			S.Name = TEXT("Kestrel");
			S.Role = TEXT("Gunship");
			S.Mode = EArenaONS_DriveMode::Flyer;
			S.Mass = 900.f;
			S.HalfExtent = FVector(180.f, 70.f, 50.f);
			S.MaxHealth = 300.f;
			S.SuspensionPoints = { FVector(100.f, 55.f, -45.f), FVector(100.f, -55.f, -45.f), FVector(-100.f, 55.f, -45.f), FVector(-100.f, -55.f, -45.f) };
			S.RestLength = 35.f;
			S.WheelRadius = 0.f;
			S.RestCompression = 0.5f;
			S.DampingRatio = 0.7f;
			S.GravityScale = 1.3f;
			S.MaxSpeed = 2500.f;
			S.ReverseSpeed = 1100.f;
			S.Accel = 1900.f;
			S.VerticalSpeed = 1100.f;
			S.YawRate = 120.f;
			S.CameraDistance = 700.f;
			S.CameraHeight = 220.f;
			S.RespawnTime = 25.f;
			S.BotArriveRadius = 900.f;
			S.bBotStaysAtObjective = true;
			S.RoadkillSpeed = 1e9f;
			FArenaONS_GunSpec G;
			G.Name = TEXT("Pulse Cannon");
			G.Projectile = MakeProjectile(6500.f, 26.f, 0.f, 9.f, FLinearColor(0.4f, 0.7f, 1.f), 0.f, 200.f, 2.5f, TEXT("S_Impact"));
			G.Projectile.Style = TEXT("IceSkaarjBolt"); // Raptor plasma (blue)
			G.RefireTime = 0.16f;
			G.HeatPerShot = 0.05f;
			G.CoolRate = 0.4f;
			G.Muzzles = { FVector(130.f, 210.f, 0.f), FVector(130.f, -210.f, 0.f) };
			G.AimCone = 30.f;
			G.Color = G.Projectile.Color;
			G.Sound = TEXT("S_Fire_Pulse");
			S.Guns.Add(G);
			FArenaONS_GunSpec R;
			R.Name = TEXT("Hunter Rockets");
			R.FireMode = 1;
			R.Projectile = MakeProjectile(3800.f, 90.f, 280.f, 12.f, FLinearColor(1.f, 0.45f, 0.2f), 0.f, 600.f, 4.f, TEXT("S_Explosion"));
			R.Projectile.Style = TEXT("Rocket");
			R.RefireTime = 0.9f;
			R.HeatPerShot = 0.3f;
			R.CoolRate = 0.3f;
			R.Muzzles = { FVector(70.f, 95.f, -35.f), FVector(70.f, -95.f, -35.f) };
			R.AimCone = 30.f;
			R.Color = R.Projectile.Color;
			R.Sound = TEXT("S_Fire_Rocket");
			S.Guns.Add(R);
		}

		// ---- Caravan: transport with a passenger turret ----
		{
			FArenaONS_VehicleSpec& S = Specs[int32(EArenaONS_VehicleType::Caravan)];
			S.Name = TEXT("Caravan");
			S.Role = TEXT("Armoured transport");
			S.Mode = EArenaONS_DriveMode::Wheeled;
			S.Mass = 1500.f;
			S.HalfExtent = FVector(210.f, 100.f, 50.f);
			S.MaxHealth = 550.f;
			S.SuspensionPoints = { FVector(150.f, 95.f, -40.f), FVector(150.f, -95.f, -40.f), FVector(-150.f, 95.f, -40.f), FVector(-150.f, -95.f, -40.f) };
			S.RestLength = 45.f;
			S.WheelRadius = 48.f;
			S.RestCompression = 0.45f;
			S.DampingRatio = 0.65f;
			S.GravityScale = 1.6f;
			S.MaxSpeed = 2000.f;
			S.ReverseSpeed = 800.f;
			S.Accel = 1200.f;
			S.Brake = 2600.f;
			S.LateralGrip = 7.f;
			S.YawRate = 80.f;
			S.CameraDistance = 750.f;
			S.CameraHeight = 240.f;
			S.RespawnTime = 20.f;
			S.NumSeats = 2;
			S.TurretRate = 140.f;
			S.TurretMinPitch = -10.f;
			S.TurretMaxPitch = 50.f;
			S.BotArriveRadius = 900.f;
			S.RoadkillSpeed = 650.f;
			FArenaONS_GunSpec G;
			G.Name = TEXT("Tail Repeater");
			G.Seat = 1;
			G.bTurret = true;
			G.bHitscan = true;
			G.Damage = 20.f;
			G.Range = 9000.f;
			G.Spread = 1.f;
			G.Momentum = 150.f;
			G.RefireTime = 0.14f;
			G.HeatPerShot = 0.04f;
			G.CoolRate = 0.3f;
			G.BeamWidth = 2.5f;
			G.Muzzles = { FVector(110.f, 12.f, 0.f), FVector(110.f, -12.f, 0.f) };
			G.Color = FLinearColor(0.6f, 1.f, 0.4f);
			G.Sound = TEXT("S_Fire_Chaingun");
			S.Guns.Add(G);
			FArenaONS_GunSpec M;
			M.Name = TEXT("Burst Mortar");
			M.Shake = ArenaViewShake::ShockProjFire(); // ONSPRVSideGun (60, 20, 0) / 1000 / 2
			M.Seat = 1;
			M.FireMode = 1;
			M.bTurret = true;
			M.Projectile = MakeProjectile(3200.f, 75.f, 320.f, 13.f, FLinearColor(0.7f, 1.f, 0.3f), 0.6f, 600.f, 5.f, TEXT("S_Explosion"));
			M.Projectile.Style = TEXT("PulseCore"); // Hellbender sky mine
			M.RefireTime = 1.2f;
			M.HeatPerShot = 0.35f;
			M.CoolRate = 0.3f;
			M.Muzzles = { FVector(110.f, 0.f, 8.f) };
			M.Color = M.Projectile.Color;
			M.Sound = TEXT("S_Fire_Goo");
			S.Guns.Add(M);
		}
		return Specs;
	}

	FLinearColor TeamTint(int32 Team)
	{
		return (Team == 0 || Team == 1) ? ArenaTeam::Color(Team) : FLinearColor(0.8f, 0.8f, 0.75f);
	}

	FString CauserWeaponName(AActor* Causer)
	{
		if (const AArenaWeapon* W = Cast<AArenaWeapon>(Causer))
		{
			return W->WeaponName;
		}
		if (const AArenaProjectile* P = Cast<AArenaProjectile>(Causer))
		{
			return P->GetParams().WeaponName;
		}
		if (const AArenaONS_Vehicle* V = Cast<AArenaONS_Vehicle>(Causer))
		{
			return V->GetVehicleName();
		}
		return FString();
	}

	UArenaRules_ONS* GetRules(const UObject* Context)
	{
		UWorld* World = Context ? Context->GetWorld() : nullptr;
		AArenaGameMode* GM = World ? World->GetAuthGameMode<AArenaGameMode>() : nullptr;
		return GM ? Cast<UArenaRules_ONS>(GM->GetRules()) : nullptr;
	}

	bool IsPlayingPhase(const UObject* Context)
	{
		const UWorld* World = Context ? Context->GetWorld() : nullptr;
		const AArenaGameState* GS = World ? World->GetGameState<AArenaGameState>() : nullptr;
		return !GS || GS->IsPlaying();
	}

	bool CanDrivePhase(const UObject* Context)
	{
		const UWorld* World = Context ? Context->GetWorld() : nullptr;
		const AArenaGameState* GS = World ? World->GetGameState<AArenaGameState>() : nullptr;
		return !GS || GS->IsPlaying() || GS->Phase == EArenaMatchPhase::Countdown;
	}
}
using namespace ArenaONSVeh;

TFunction<bool(const APawn* Seat, AController* Controller, FArenaONS_VehicleInput& OutInput)> GArenaONSExternalInput;

const FArenaONS_VehicleSpec& AArenaONS_Vehicle::GetSpec(EArenaONS_VehicleType Type, bool bArtModel)
{
	static const TArray<FArenaONS_VehicleSpec> Specs = BuildSpecs();
	// Same tuning, with the size / suspension / camera / muzzles fitted to the imported or replacement model.
	static const TArray<FArenaONS_VehicleSpec> ArtSpecs = []()
	{
		TArray<FArenaONS_VehicleSpec> Out = BuildSpecs();
		for (int32 i = 0; i < Out.Num(); ++i)
		{
			if (const ArenaArtVehicles::FVehicleArt* Art = ArenaArtVehicles::Find(EArenaONS_VehicleType(i)))
			{
				ArenaArtVehicles::ApplyToSpec(*Art, Out[i]);
			}
		}
		return Out;
	}();
	const int32 Index = FMath::Clamp(int32(Type), 0, Specs.Num() - 1);
	return bArtModel ? ArtSpecs[Index] : Specs[Index];
}

const FArenaONS_VehicleSpec& AArenaONS_Vehicle::GetSpec(EArenaONS_VehicleType Type)
{
	return GetSpec(Type, ArenaArtVehicles::IsAvailable(Type));
}

bool AArenaONS_Vehicle::UsesArtModel() const
{
	if (ArtModelState < 0)
	{
		ArtModelState = ArenaArtVehicles::IsAvailable(VehicleType) ? 1 : 0;
	}
	return ArtModelState > 0;
}

FString AArenaONS_Vehicle::GetTypeName(EArenaONS_VehicleType Type)
{
	return GetSpec(Type).Name;
}

// =============================================================================================
// Shared helpers
// =============================================================================================

bool ArenaONS::HasDebugFlag(const TCHAR* Flag)
{
	return FParse::Param(FCommandLine::Get(), Flag);
}

int32 ArenaONS::TeamOfController(const AController* C)
{
	const AArenaPlayerState* PS = C ? C->GetPlayerState<AArenaPlayerState>() : nullptr;
	return PS ? PS->Team : AArenaAS_TurretController::TeamOf(C); // (an Assault turret's own controller)
}

AArenaONS_Vehicle* ArenaONS::GetRiddenVehicle(const AArenaCharacter* Char)
{
	if (!Char)
	{
		return nullptr;
	}
	AActor* Parent = Char->GetAttachParentActor();
	if (AArenaONS_Vehicle* V = Cast<AArenaONS_Vehicle>(Parent))
	{
		return V;
	}
	if (AArenaONS_VehicleSeat* S = Cast<AArenaONS_VehicleSeat>(Parent))
	{
		return S->GetVehicle();
	}
	return nullptr;
}

int32 ArenaONS::TeamOfCharacter(const AArenaCharacter* Char)
{
	if (!Char)
	{
		return ArenaTeam::None;
	}
	if (Char->GetController())
	{
		return Char->GetTeam();
	}
	if (const AArenaONS_Vehicle* V = GetRiddenVehicle(Char))
	{
		return V->GetTeam();
	}
	return Char->GetTeam();
}

AArenaONS_Vehicle* ArenaONS::GetControlledVehicle(const AController* C)
{
	APawn* P = C ? C->GetPawn() : nullptr;
	if (AArenaONS_Vehicle* V = Cast<AArenaONS_Vehicle>(P))
	{
		return V;
	}
	if (AArenaONS_VehicleSeat* S = Cast<AArenaONS_VehicleSeat>(P))
	{
		return S->GetVehicle();
	}
	return nullptr;
}

int32 ArenaONS::GetControlledSeat(const AController* C)
{
	APawn* P = C ? C->GetPawn() : nullptr;
	if (Cast<AArenaONS_Vehicle>(P))
	{
		return 0;
	}
	if (const AArenaONS_VehicleSeat* S = Cast<AArenaONS_VehicleSeat>(P))
	{
		return S->GetSeatIndex();
	}
	return -1;
}

void ArenaONS::HideRider(AArenaCharacter* Char, USceneComponent* AttachTo, const FVector& RelLocation, TMap<TWeakObjectPtr<AActor>, bool>& OutHidden)
{
	if (!Char || !AttachTo)
	{
		return;
	}
	Char->StopFire(0);
	Char->StopFire(1);
	if (UCharacterMovementComponent* Move = Char->GetCharacterMovement())
	{
		Move->StopMovementImmediately();
		Move->DisableMovement();
		Move->SetComponentTickEnabled(false);
	}
	OutHidden.Reset();
	TArray<AActor*> Attached;
	Char->GetAttachedActors(Attached, true, true);
	for (AActor* A : Attached)
	{
		if (A)
		{
			OutHidden.Add(A, A->IsHidden());
			A->SetActorHiddenInGame(true);
		}
	}
	Char->SetActorEnableCollision(false);
	Char->SetActorHiddenInGame(true);
	Char->AttachToComponent(AttachTo, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	Char->SetActorRelativeLocation(RelLocation);
	Char->SetActorRelativeRotation(FRotator::ZeroRotator);
}

void ArenaONS::RestoreRider(AArenaCharacter* Char, const FVector& Location, float Yaw, const TMap<TWeakObjectPtr<AActor>, bool>& Hidden)
{
	if (!Char)
	{
		return;
	}
	Char->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	Char->SetActorLocationAndRotation(Location, FRotator(0.f, Yaw, 0.f), false, nullptr, ETeleportType::TeleportPhysics);
	Char->SetActorHiddenInGame(false);
	Char->SetActorEnableCollision(true);
	for (const TPair<TWeakObjectPtr<AActor>, bool>& Pair : Hidden)
	{
		if (AActor* A = Pair.Key.Get())
		{
			A->SetActorHiddenInGame(Pair.Value);
		}
	}
	if (UCharacterMovementComponent* Move = Char->GetCharacterMovement())
	{
		Move->SetComponentTickEnabled(true);
		Move->SetMovementMode(MOVE_Falling);
		Move->Velocity = FVector::ZeroVector;
	}
}

FVector ArenaONS::FindExitLocation(const AArenaONS_Vehicle* Vehicle, const AActor* IgnoreActor)
{
	UWorld* World = Vehicle ? Vehicle->GetWorld() : nullptr;
	if (!World)
	{
		return FVector::ZeroVector;
	}
	const FArenaONS_VehicleSpec& S = Vehicle->GetSpec();
	const FVector Center = Vehicle->GetActorLocation();
	const FRotator YawRot(0.f, Vehicle->GetActorRotation().Yaw, 0.f);
	const FVector E = S.HalfExtent;
	const float HalfHeight = ArenaTuning::CapsuleHalfHeight;
	const float Radius = ArenaTuning::CapsuleRadius;

	FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaONSExit), false, Vehicle);
	Vehicle->AddIgnoredActors(QP);
	if (IgnoreActor)
	{
		QP.AddIgnoredActor(IgnoreActor);
	}

	const TArray<FVector> Offsets = {
		FVector(0.f, -(E.Y + Radius + 60.f), 0.f), FVector(0.f, E.Y + Radius + 60.f, 0.f),
		FVector(-(E.X + Radius + 60.f), 0.f, 0.f), FVector(E.X + Radius + 60.f, 0.f, 0.f),
		FVector(E.X * 0.6f, -(E.Y + Radius + 140.f), 0.f), FVector(-E.X * 0.6f, E.Y + Radius + 140.f, 0.f),
		FVector(0.f, -(E.Y + Radius + 260.f), 0.f), FVector(0.f, E.Y + Radius + 260.f, 0.f),
		FVector(-(E.X + Radius + 220.f), 0.f, 0.f)
	};
	const FCollisionShape Capsule = FCollisionShape::MakeCapsule(Radius, HalfHeight);
	for (const FVector& Off : Offsets)
	{
		FVector P = Center + YawRot.RotateVector(Off);
		// Must not pass through a wall from the vehicle centre.
		FHitResult Wall;
		if (World->LineTraceSingleByChannel(Wall, Center, P, ECC_Visibility, QP))
		{
			continue;
		}
		FHitResult Floor;
		const FVector Top = P + FVector(0.f, 0.f, 120.f);
		if (!World->LineTraceSingleByChannel(Floor, Top, P - FVector(0.f, 0.f, 600.f), ECC_Visibility, QP) || Floor.ImpactNormal.Z < 0.5f)
		{
			continue;
		}
		const FVector Stand = FVector(P.X, P.Y, Floor.ImpactPoint.Z + HalfHeight + 4.f);
		if (World->OverlapBlockingTestByChannel(Stand, FQuat::Identity, ECC_Pawn, Capsule, QP))
		{
			continue;
		}
		return Stand;
	}
	// Above the vehicle as a last resort.
	return Center + FVector(0.f, 0.f, E.Z + HalfHeight + 40.f);
}

void ArenaONS::FireGunShot(AArenaONS_Vehicle* Vehicle, const FArenaONS_GunSpec& Gun, AController* Instigator, const FVector& Muzzle, const FVector& InDir)
{
	UWorld* World = Vehicle ? Vehicle->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}
	const FVector Dir = Gun.Spread > 0.f ? FMath::VRandCone(InDir, FMath::DegreesToRadians(Gun.Spread)) : InDir;
	if (!Gun.bHitscan)
	{
		FArenaProjectileParams P = Gun.Projectile;
		P.WeaponName = Gun.Name;
		const FTransform T(Dir.Rotation(), Muzzle);
		AArenaProjectile* Proj = World->SpawnActorDeferred<AArenaProjectile>(AArenaProjectile::StaticClass(), T, Vehicle, Vehicle,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Proj)
		{
			Proj->Init(P, Instigator, 1.f);
			Proj->FinishSpawning(T);
		}
		return;
	}

	FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaONSGun), false, Vehicle);
	Vehicle->AddIgnoredActors(QP);
	const FVector End = Muzzle + Dir * Gun.Range;
	FHitResult Hit;
	bool bHit = false;
	for (int32 Attempt = 0; Attempt < 6; ++Attempt)
	{
		if (!World->LineTraceSingleByChannel(Hit, Muzzle, End, ECC_Weapon, QP))
		{
			break;
		}
		AActor* HitActor = Hit.GetActor();
		UPrimitiveComponent* HitComp = Hit.GetComponent();
		bool bSolid = true;
		if (HitComp && !Cast<APawn>(HitActor) && !Cast<AArenaProjectile>(HitActor))
		{
			bSolid = HitComp->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block;
		}
		if (bSolid)
		{
			bHit = true;
			break;
		}
		if (HitActor)
		{
			QP.AddIgnoredActor(HitActor);
		}
		else if (HitComp)
		{
			QP.AddIgnoredComponent(HitComp);
		}
		else
		{
			break;
		}
	}
	const FVector HitPoint = bHit ? FVector(Hit.ImpactPoint) : End;
	if (bHit)
	{
		if (AArenaProjectile* Proj = Cast<AArenaProjectile>(Hit.GetActor()))
		{
			Proj->TriggerCombo(Instigator);
		}
		else if (AActor* Victim = Hit.GetActor())
		{
			FArenaHitInfo Info;
			Info.WeaponName = Gun.Name;
			Info.Momentum = Dir * Gun.Momentum;
			ArenaDamage::ApplyDamage(Victim, Gun.Damage, Instigator, Vehicle, Info);
		}
		ArenaFX::SpawnImpact(World, Hit.ImpactPoint, Hit.ImpactNormal, Gun.Color, 10.f, TEXT("Impact.Bullet"));
	}
	ArenaFX::SpawnBeam(World, Muzzle, HitPoint, Gun.Color, Gun.BeamWidth, 0.07f, TEXT("Tracer.Bullet"));
}

// =============================================================================================
// AArenaONS_Vehicle: construction / setup
// =============================================================================================

AArenaONS_Vehicle::AArenaONS_Vehicle()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
	bReplicates = true;
	SetReplicateMovement(true);
	SetCanBeDamaged(true);
	AutoPossessAI = EAutoPossessAI::Disabled;
	AutoPossessPlayer = EAutoReceiveInput::Disabled;
	AIControllerClass = nullptr;
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	Body = CreateDefaultSubobject<UBoxComponent>(TEXT("Body"));
	Body->SetBoxExtent(FVector(150.f, 85.f, 30.f));
	Body->SetCollisionObjectType(ECC_Pawn);
	Body->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Body->SetCollisionResponseToAllChannels(ECR_Block);
	Body->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	Body->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
	Body->SetCanEverAffectNavigation(false);
	Body->SetGenerateOverlapEvents(false);
	Body->SetLinearDamping(0.05f);
	Body->SetAngularDamping(1.2f);
	Body->BodyInstance.bUseCCD = true;
	Body->SetHiddenInGame(true);
	RootComponent = Body;

	VisualRoot = CreateDefaultSubobject<USceneComponent>(TEXT("VisualRoot"));
	VisualRoot->SetupAttachment(Body);

	TurretRoot = CreateDefaultSubobject<USceneComponent>(TEXT("TurretRoot"));
	TurretRoot->SetupAttachment(VisualRoot);
	BarrelRoot = CreateDefaultSubobject<USceneComponent>(TEXT("BarrelRoot"));
	BarrelRoot->SetupAttachment(TurretRoot);
	RearTurretRoot = CreateDefaultSubobject<USceneComponent>(TEXT("RearTurretRoot"));
	RearTurretRoot->SetupAttachment(VisualRoot);
	RearBarrelRoot = CreateDefaultSubobject<USceneComponent>(TEXT("RearBarrelRoot"));
	RearBarrelRoot->SetupAttachment(RearTurretRoot);

	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(Body);
	SpringArm->TargetArmLength = 600.f;
	SpringArm->bUsePawnControlRotation = true;
	SpringArm->bInheritRoll = false;
	SpringArm->bDoCollisionTest = true;
	SpringArm->ProbeSize = 16.f;
	SpringArm->bEnableCameraLag = true;
	SpringArm->CameraLagSpeed = 12.f;
	SpringArm->SetUsingAbsoluteRotation(true);

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm);
	Camera->bUsePawnControlRotation = false;
	Camera->SetFieldOfView(95.f);
}

void AArenaONS_Vehicle::InitVehicle(EArenaONS_VehicleType InType, int32 InTeam, AArenaONS_VehicleFactory* InFactory)
{
	VehicleType = InType;
	Team = InTeam;
	Factory = InFactory;
	ArtModelState = -1;
	const FArenaONS_VehicleSpec& S = GetSpec();
	Health = S.MaxHealth;
	Body->SetBoxExtent(S.HalfExtent);
	SpringArm->TargetArmLength = S.CameraDistance;
	SpringArm->SocketOffset = FVector(0.f, 0.f, S.CameraHeight);
}

void AArenaONS_Vehicle::BeginPlay()
{
	Super::BeginPlay();
	const FArenaONS_VehicleSpec& S = GetSpec();
	Body->SetBoxExtent(S.HalfExtent);
	WheelCompression.Init(S.RestLength, S.SuspensionPoints.Num());
	if (HasAuthority())
	{
		Body->SetSimulatePhysics(true);
		Body->SetMassOverrideInKg(NAME_None, S.Mass, true);
		Body->SetCenterOfMass(FVector(0.f, 0.f, -S.HalfExtent.Z * 0.6f));
		Body->SetEnableGravity(true);

		if (S.NumSeats > 1)
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Params.Owner = this;
			SeatPawn = GetWorld()->SpawnActor<AArenaONS_VehicleSeat>(AArenaONS_VehicleSeat::StaticClass(), GetActorTransform(), Params);
			if (SeatPawn)
			{
				SeatPawn->InitSeat(this, 1);
			}
		}
		if (UArenaRules_ONS* Rules = ArenaONSVeh::GetRules(this))
		{
			Rules->RegisterVehicle(this);
		}
	}
	TurretYaw = 0.f;
	RearTurretYaw = 180.f;
	BuildModel();
	UpdateTeamLook();
}

void AArenaONS_Vehicle::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (SeatPawn)
	{
		SeatPawn->Destroy();
		SeatPawn = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

void AArenaONS_Vehicle::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	if (NewController)
	{
		const int32 T = ArenaONS::TeamOfController(NewController);
		if (T == 0 || T == 1)
		{
			Team = T;
		}
	}
	UpdateTeamLook();
}

void AArenaONS_Vehicle::UnPossessed()
{
	Super::UnPossessed();
}

float AArenaONS_Vehicle::GetHealthFraction() const
{
	const float Max = GetSpec().MaxHealth;
	return Max > 0.f ? FMath::Clamp(Health / Max, 0.f, 1.f) : 0.f;
}

const FArenaONS_GunSpec* AArenaONS_Vehicle::FindGun(int32 Seat, uint8 FireMode, int32* OutIndex) const
{
	const FArenaONS_VehicleSpec& S = GetSpec();
	for (int32 i = 0; i < S.Guns.Num(); ++i)
	{
		if (S.Guns[i].Seat == Seat && S.Guns[i].FireMode == FireMode)
		{
			if (OutIndex)
			{
				*OutIndex = i;
			}
			return &S.Guns[i];
		}
	}
	return nullptr;
}

float AArenaONS_Vehicle::GetGunHeat(int32 Seat, uint8 FireMode) const
{
	int32 Index = INDEX_NONE;
	if (FindGun(Seat, FireMode, &Index) && Index >= 0 && Index < 4)
	{
		return Heat[Index];
	}
	return -1.f;
}

float AArenaONS_Vehicle::GetSpeed() const
{
	return GetVelocity().Size();
}

FVector AArenaONS_Vehicle::GetPawnViewLocation() const
{
	return GetActorLocation() + FVector(0.f, 0.f, GetSpec().HalfExtent.Z + 60.f);
}

FRotator AArenaONS_Vehicle::GetViewRotation() const
{
	if (Controller && Controller->IsA<APlayerController>())
	{
		return Controller->GetControlRotation();
	}
	if (Controller)
	{
		return AIAimRotation;
	}
	return FRotator(-12.f, GetActorRotation().Yaw, 0.f);
}

void AArenaONS_Vehicle::AddIgnoredActors(FCollisionQueryParams& Params) const
{
	Params.AddIgnoredActor(this);
	if (SeatPawn)
	{
		Params.AddIgnoredActor(SeatPawn);
	}
	if (AArenaCharacter* D = DriverChar.Get())
	{
		Params.AddIgnoredActor(D);
	}
	if (SeatPawn && SeatPawn->GetRider())
	{
		Params.AddIgnoredActor(SeatPawn->GetRider());
	}
}

// =============================================================================================
// Seats
// =============================================================================================

AArenaCharacter* AArenaONS_Vehicle::GetSeatCharacter(int32 Seat) const
{
	if (Seat == 0)
	{
		return DriverChar.Get();
	}
	return (Seat == 1 && SeatPawn) ? SeatPawn->GetRider() : nullptr;
}

AController* AArenaONS_Vehicle::GetSeatController(int32 Seat) const
{
	if (Seat == 0)
	{
		return GetController();
	}
	return (Seat == 1 && SeatPawn) ? SeatPawn->GetController() : nullptr;
}

bool AArenaONS_Vehicle::IsEmpty() const
{
	return !DriverChar.IsValid() && !(SeatPawn && SeatPawn->GetRider());
}

int32 AArenaONS_Vehicle::GetFreeSeat() const
{
	if (!DriverChar.IsValid())
	{
		return 0;
	}
	if (GetSpec().NumSeats > 1 && SeatPawn && !SeatPawn->GetRider())
	{
		return 1;
	}
	return -1;
}

bool AArenaONS_Vehicle::CanBeEnteredBy(const AArenaCharacter* Char) const
{
	if (bWrecked || !Char || !Char->IsAlive() || !Char->GetController())
	{
		return false;
	}
	const int32 T = Char->GetTeam();
	if (T != Team)
	{
		// Enemies may only steal a vehicle that was already driven and is empty.
		if (bLocked || !IsEmpty())
		{
			return false;
		}
	}
	return GetFreeSeat() >= 0;
}

bool AArenaONS_Vehicle::EnterSeat(AArenaCharacter* Char, int32 Seat)
{
	AController* C = Char ? Char->GetController() : nullptr;
	UWorld* World = GetWorld();
	if (!HasAuthority() || !C || !World || bWrecked || !Char->IsAlive())
	{
		return false;
	}
	const float Now = World->GetTimeSeconds();
	const float Yaw = GetActorRotation().Yaw;
	if (Seat == 0)
	{
		if (DriverChar.IsValid())
		{
			return false;
		}
		ArenaONS::HideRider(Char, Body, FVector(0.f, 0.f, -40.f), DriverAttachedHidden);
		DriverChar = Char;
		ReplicatedDriver = Char;
		const int32 T = ArenaONS::TeamOfController(C);
		if (T == 0 || T == 1)
		{
			Team = T;
		}
		bLocked = false;
		bWasDriven = true;
		EnterTime = Now;
		AIPath.Reset();
		AINextDestTime = 0.f;
		AINextRepathTime = 0.f;
		AIStuckCount = 0;
		AIStuckTime = 0.f;
		AIDriveStartTime = Now;
		AILastPos = GetActorLocation();
		AIProgressCheckTime = Now + 8.f;
		AIAimRotation = FRotator(-5.f, Yaw, 0.f);
		C->Possess(this);
		C->SetControlRotation(FRotator(-10.f, Yaw, 0.f));
		bDebugAutoDrive = Cast<APlayerController>(C) && ArenaONS::HasDebugFlag(TEXT("ONSAutoDrive"));
	}
	else if (Seat == 1 && SeatPawn && !SeatPawn->GetRider())
	{
		ArenaONS::HideRider(Char, SeatPawn->Root, FVector(0.f, 0.f, -60.f), SeatPawn->RiderAttachedHidden);
		SeatPawn->Rider = Char;
		SeatPawn->EnterTime = Now;
		if (IsEmpty())
		{
			Team = ArenaONS::TeamOfController(C);
		}
		bLocked = false;
		C->Possess(SeatPawn);
		C->SetControlRotation(FRotator(-5.f, Yaw + 180.f, 0.f));
	}
	else
	{
		return false;
	}
	UpdateTeamLook();
	ArenaAssets::PlaySoundAt(this, TEXT("S_WeaponSwitch"), GetActorLocation(), 0.8f, 0.7f);
	if (Seat == 0 && ArtRig.IsValid())
	{
		PlayArtSound(ArtRig.Art->StartSound, GetActorLocation(), 0.9f);
	}
	const APlayerState* PS = C->PlayerState;
	UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: %s entered %s (seat %d, team %d)"), PS ? *PS->GetPlayerName() : TEXT("?"), *GetVehicleName(), Seat, Team);
	return true;
}

AArenaCharacter* AArenaONS_Vehicle::LeaveSeat(int32 Seat, bool bForced)
{
	if (!HasAuthority())
	{
		return nullptr;
	}
	AArenaCharacter* Char = GetSeatCharacter(Seat);
	AController* C = GetSeatController(Seat);
	if (!Char)
	{
		return nullptr;
	}
	const FVector Exit = ArenaONS::FindExitLocation(this, Char);
	const float Yaw = C ? C->GetControlRotation().Yaw : GetActorRotation().Yaw;
	const FVector Vel = Body ? Body->GetPhysicsLinearVelocity() : FVector::ZeroVector;

	if (Seat == 0)
	{
		if (ArtRig.IsValid() && !bWrecked)
		{
			PlayArtSound(ArtRig.Art->StopSound, GetActorLocation(), 0.9f);
		}
		DriverChar.Reset();
		ReplicatedDriver = nullptr;
		ArenaONS::RestoreRider(Char, Exit, Yaw, DriverAttachedHidden);
		DriverAttachedHidden.Reset();
	}
	else if (SeatPawn)
	{
		SeatPawn->Rider.Reset();
		ArenaONS::RestoreRider(Char, Exit, Yaw, SeatPawn->RiderAttachedHidden);
		SeatPawn->RiderAttachedHidden.Reset();
	}
	if (C)
	{
		C->Possess(Char);
		C->SetControlRotation(FRotator(0.f, Yaw, 0.f));
	}
	if (!bForced)
	{
		if (UCharacterMovementComponent* Move = Char->GetCharacterMovement())
		{
			Move->Velocity = Vel * 0.4f;
		}
	}
	const APlayerState* PS = C ? C->PlayerState.Get() : nullptr;
	UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: %s left %s (seat %d%s)"), PS ? *PS->GetPlayerName() : TEXT("?"), *GetVehicleName(), Seat, bForced ? TEXT(", forced") : TEXT(""));
	if (UArenaRules_ONS* Rules = ArenaONSVeh::GetRules(this))
	{
		Rules->NotifyLeftVehicle(C, this, AIStuckCount >= 5);
	}
	return Char;
}

void AArenaONS_Vehicle::EjectAll()
{
	for (int32 Seat = GetSpec().NumSeats - 1; Seat >= 0; --Seat)
	{
		LeaveSeat(Seat, true);
	}
}

// =============================================================================================
// Damage
// =============================================================================================

float AArenaONS_Vehicle::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	if (!HasAuthority() || bWrecked || DamageAmount <= 0.f || !ArenaONSVeh::IsPlayingPhase(this))
	{
		return 0.f;
	}
	const bool bSelf = EventInstigator && (EventInstigator == GetController() || (SeatPawn && EventInstigator == SeatPawn->GetController()));
	const int32 AttackerTeam = ArenaONS::TeamOfController(EventInstigator);
	if (!bSelf && AttackerTeam >= 0 && AttackerTeam == Team && (bWasDriven || !IsEmpty()))
	{
		return 0.f; // no friendly fire on vehicles
	}
	const float Dmg = DamageAmount * (bSelf ? 0.5f : 1.f);
	Health -= Dmg;
	if (!bSelf && EventInstigator)
	{
		LastDamager = EventInstigator;
		LastDamageWeapon = ArenaONSVeh::CauserWeaponName(DamageCauser);
		LastDamageTime = GetWorld()->GetTimeSeconds();
		if (AArenaPlayerController* PC = Cast<AArenaPlayerController>(EventInstigator))
		{
			PC->NotifyHitEnemy(Dmg, false);
		}
	}
	for (int32 Seat = 0; Seat < GetSpec().NumSeats; ++Seat)
	{
		if (AArenaPlayerController* PC = Cast<AArenaPlayerController>(GetSeatController(Seat)))
		{
			const FVector From = DamageCauser ? DamageCauser->GetActorLocation() : GetActorLocation();
			PC->NotifyTookDamage(From, Dmg);
		}
	}
	if (Health <= 0.f)
	{
		AController* Killer = bSelf ? EventInstigator : (LastDamager.IsValid() ? LastDamager.Get() : EventInstigator);
		FString Weapon = ArenaONSVeh::CauserWeaponName(DamageCauser);
		Explode(Killer, Weapon);
	}
	return Dmg;
}

void AArenaONS_Vehicle::FellOutOfWorld(const UDamageType& DmgType)
{
	if (HasAuthority() && !bWrecked)
	{
		Explode(LastDamager.Get(), TEXT("Fall"));
	}
}

void AArenaONS_Vehicle::Explode(AController* Killer, const FString& WeaponName)
{
	if (bWrecked || !HasAuthority())
	{
		return;
	}
	bWrecked = true;
	Health = 0.f;
	UWorld* World = GetWorld();
	const FVector Loc = GetActorLocation();
	const APlayerState* KPS = Killer ? Killer->PlayerState.Get() : nullptr;
	UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: vehicle %s (team %d) destroyed by %s [%s] at %s%s"), *GetVehicleName(), Team, KPS ? *KPS->GetPlayerName() : TEXT("nobody"), *WeaponName,
		*Loc.ToCompactString(), HasDriver() ? TEXT(" (driven)") : TEXT(""));

	const FString KillName = WeaponName.IsEmpty() ? FString::Printf(TEXT("%s wreck"), *GetVehicleName()) : WeaponName;
	for (int32 Seat = GetSpec().NumSeats - 1; Seat >= 0; --Seat)
	{
		AController* C = GetSeatController(Seat);
		if (AArenaCharacter* Char = LeaveSeat(Seat, true))
		{
			FArenaHitInfo Info;
			Info.WeaponName = KillName;
			Info.Momentum = FVector(0.f, 0.f, 600.f);
			AController* KillCredit = Killer ? Killer : C;
			Char->ReceiveArenaDamage(10000.f, KillCredit, this, Info);
			if (Char->IsAlive())
			{
				Char->Suicide();
			}
		}
	}

	// ONSVehicle.ClientVehicleExplosion: view shake within ExplosionRadius (300 UU) x 2.5 (ShakeRotMag ... defaults)
	ArenaViewShake::ShakeViewersNear(World, Loc, 300.f * 2.f, 2.5f, ArenaViewShake::Blast());
	// ONSVehicle.ClientVehicleExplosion: DestructionEffectClass (ONSHoverBike / ONSRV: ONSSmallVehicleExplosionEffect;
	// ONSHoverTank / ONSAttackCraft / ONSPRV: ONSVehicleExplosionEffect) through ArenaUE2Effects, else our own
	const bool bSmall = GetVehicleType() == EArenaONS_VehicleType::Wisp || GetVehicleType() == EArenaONS_VehicleType::Dustrunner;
	if (!ArenaUE2Effects::SpawnEmitter(World, bSmall ? TEXT("Onslaught.ONSSmallVehicleExplosionEffect") : TEXT("Onslaught.ONSVehicleExplosionEffect"), Loc, GetActorRotation()))
	{
		ArenaFX::SpawnExplosion(World, Loc, FLinearColor(1.f, 0.45f, 0.1f), 550.f, TEXT("Explosion.Rocket"));
		ArenaFX::SpawnExplosion(World, Loc + FVector(0.f, 0.f, 60.f), FLinearColor(1.f, 0.85f, 0.5f), 300.f, TEXT("Explosion.Grenade"));
	}
	if (ArtRig.IsValid() && ArtRig.Art->ExplodeSound && ArenaArtVehicles::LoadSound(ArtRig.Art->ExplodeSound))
	{
		PlayArtSound(ArtRig.Art->ExplodeSound, Loc, 1.f);
	}
	else
	{
		ArenaAssets::PlaySoundAt(this, TEXT("S_ExplosionBig"), Loc, 1.4f, 0.85f);
	}
	if (ArtRig.IsValid())
	{
		AArenaONS_VehicleWreck::SpawnFor(this);
	}
	ArenaDamage::ApplyRadialDamage(World, Loc, 80.f, 500.f, 900.f, Killer, this, KillName, 0.5f, this);
	if (AArenaONS_VehicleFactory* F = Factory.Get())
	{
		F->NotifyVehicleDestroyed(this);
	}
	Destroy();
}

// =============================================================================================
// Tick
// =============================================================================================

void AArenaONS_Vehicle::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bWrecked)
	{
		return;
	}
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();
	// ONSVehicle proximity view shake (bOnlyViewShakeIfDriven: only while someone drives it)
	if (GetSpec().ProximityShakeRadius > 0.f && HasDriver())
	{
		const FArenaONS_VehicleSpec& PS = GetSpec();
		ArenaViewShake::ProximityShake(World, this, GetActorLocation(), PS.ProximityShakeRadius, PS.ProximityShakeOffsetMag, PS.ProximityShakeFreq);
	}

	if (HasAuthority())
	{
		// Riders whose controller vanished (player left)
		if (DriverChar.IsValid() && !GetController())
		{
			DriverChar->Destroy();
			DriverChar.Reset();
			ReplicatedDriver = nullptr;
		}

		const FRotator PrevAim = Input.AimRotation;
		Input = FArenaONS_VehicleInput();
		Input.AimRotation = PrevAim;
		AController* C = GetController();
		if (C && ArenaONSVeh::CanDrivePhase(this))
		{
			APlayerController* PC = Cast<APlayerController>(C);
			if (!PC && GArenaONSExternalInput && GArenaONSExternalInput(this, C, Input))
			{
				// a remote player's input (hosting for UT2004 clients); the aim point from the view
				ConsumeMovementInputVector();
				Input.AimPoint = TraceAimPoint(Camera ? Camera->GetComponentLocation() : GetPawnViewLocation(), Input.AimRotation);
			}
			else if (PC && !bDebugAutoDrive)
			{
				GatherPlayerInput(PC, DeltaSeconds);
			}
			else
			{
				GatherAIInput(C, DeltaSeconds);
			}
		}
		else
		{
			ConsumeMovementInputVector();
		}

		SimulatePhysics(DeltaSeconds);
		UpdateWeapons(DeltaSeconds);
		UpdateRoadkill(DeltaSeconds);
		CheckFlipped(DeltaSeconds);

		if (Input.bUse && C && Now - EnterTime > 0.6f)
		{
			LeaveSeat(0, false);
		}

		// Kill Z
		if (UArenaRules_ONS* Rules = ArenaONSVeh::GetRules(this))
		{
			if (GetActorLocation().Z < Rules->GetKillZ())
			{
				Explode(LastDamager.Get(), TEXT("Fall"));
				return;
			}
		}
	}
	UpdateVisuals(DeltaSeconds);
	TickArtShowcase();

	// Local camera FOV follows the user setting.
	if (AArenaPlayerController* LPC = Cast<AArenaPlayerController>(GetController()))
	{
		if (LPC->IsLocalController() && Camera)
		{
			Camera->SetFieldOfView(LPC->GetDesiredFOV());
		}
	}
}

FVector AArenaONS_Vehicle::TraceAimPoint(const FVector& ViewLoc, const FRotator& ViewRot) const
{
	UWorld* World = GetWorld();
	const FVector Dir = ViewRot.Vector();
	const FVector End = ViewLoc + Dir * 20000.f;
	FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaONSAim), false, this);
	AddIgnoredActors(QP);
	FHitResult Hit;
	if (World && World->LineTraceSingleByChannel(Hit, ViewLoc, End, ECC_Weapon, QP))
	{
		// Ignore hits closer than the vehicle itself (camera inside geometry)
		if (FVector::DotProduct(FVector(Hit.ImpactPoint) - GetActorLocation(), Dir) > 0.f)
		{
			return Hit.ImpactPoint;
		}
	}
	return End;
}

void AArenaONS_Vehicle::GatherPlayerInput(APlayerController* PC, float DeltaSeconds)
{
	const FVector Move = ConsumeMovementInputVector();
	if (const AArenaPlayerController* APC = Cast<AArenaPlayerController>(PC))
	{
		if (APC->IsPauseMenuOpen())
		{
			return;
		}
	}
	const FRotator Ctrl = PC->GetControlRotation();
	const FRotator YawRot(0.f, Ctrl.Yaw, 0.f);
	const FRotationMatrix M(YawRot);
	Input.Throttle = FMath::Clamp(FVector::DotProduct(Move, M.GetUnitAxis(EAxis::X)), -1.f, 1.f);
	Input.Steer = FMath::Clamp(FVector::DotProduct(Move, M.GetUnitAxis(EAxis::Y)), -1.f, 1.f);
	Input.bFire = PC->IsInputKeyDown(EKeys::LeftMouseButton) || PC->IsInputKeyDown(EKeys::Gamepad_RightTrigger);
	Input.bAltFire = PC->IsInputKeyDown(EKeys::RightMouseButton) || PC->IsInputKeyDown(EKeys::Gamepad_LeftTrigger);
	Input.bJump = PC->IsInputKeyDown(EKeys::SpaceBar) || PC->IsInputKeyDown(EKeys::Gamepad_FaceButton_Bottom);
	const bool bDown = PC->IsInputKeyDown(EKeys::C) || PC->IsInputKeyDown(EKeys::LeftControl) || PC->IsInputKeyDown(EKeys::Gamepad_RightThumbstick);
	Input.Rise = (Input.bJump ? 1.f : 0.f) - (bDown ? 1.f : 0.f);
	Input.bUse = PC->WasInputKeyJustPressed(EKeys::E) || PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Left);
	Input.DesiredYaw = Ctrl.Yaw;
	Input.bHasDesiredYaw = true;
	Input.AimRotation = Ctrl;
	const FVector ViewLoc = Camera ? Camera->GetComponentLocation() : GetPawnViewLocation();
	Input.AimPoint = TraceAimPoint(ViewLoc, Ctrl);
}

// =============================================================================================
// Physics
// =============================================================================================

void AArenaONS_Vehicle::SimulatePhysics(float DeltaSeconds)
{
	UBoxComponent* B = Body;
	UWorld* World = GetWorld();
	if (!B || !World || !B->IsSimulatingPhysics() || DeltaSeconds <= 0.f)
	{
		return;
	}
	const FArenaONS_VehicleSpec& S = GetSpec();
	const float Now = World->GetTimeSeconds();
	const FTransform T = B->GetComponentTransform();
	const FVector Up = T.GetUnitAxis(EAxis::Z);
	const FVector Fwd = T.GetUnitAxis(EAxis::X);
	const float Mass = FMath::Max(1.f, B->GetMass());
	const FVector Vel = B->GetPhysicsLinearVelocity();
	const float WorldG = World->GetGravityZ(); // negative
	const bool bDriven = GetController() != nullptr;
	const bool bFlyer = S.Mode == EArenaONS_DriveMode::Flyer;
	const bool bHover = S.Mode == EArenaONS_DriveMode::Hover;

	// ---- gravity / lift ----
	// UE2 Karma: every body falls with its PhysicsVolume's full Gravity vector x KarmaParams.KActorGravScale (the
	// sideways / upward parts of tubes and wind volumes too). The physics scene applies WorldG; we add the difference.
	const UArenaWorldZoneSubsystem* Zones = UArenaWorldZoneSubsystem::Get(this);
	const FVector LocalG = Zones && Zones->HasVolumes() ? Zones->GetGravityAt(GetActorLocation()) : FVector(0.f, 0.f, WorldG);
	if (bFlyer && bDriven)
	{
		// a flown craft's lift cancels the vertical part (ONSChopperCraft uplift); a volume's sideways pull still acts
		B->AddForce(FVector(LocalG.X * S.GravityScale, LocalG.Y * S.GravityScale, -WorldG), NAME_None, true);
	}
	else
	{
		B->AddForce(LocalG * S.GravityScale - FVector(0.f, 0.f, WorldG), NAME_None, true);
	}
	// UE2 PhysicsVolume Karma settings (approximation: the Karma code is native): KExtraLinearDamping /
	// KExtraAngularDamping slow bodies inside (water 2.5 / 0.4, lava 0.8 / 0.1 per second), and in water volumes the
	// vehicle's KBuoyancy x the volume's KBuoyancy of gravity pushes up (0 for the stock vehicles)
	if (const AArenaPhysicsVolume* KV = Zones && Zones->HasVolumes() ? Zones->GetPhysicsVolumeAt(GetActorLocation()) : nullptr)
	{
		if (KV->KExtraLinearDamping > 0.f)
		{
			B->AddForce(-Vel * KV->KExtraLinearDamping, NAME_None, true);
		}
		if (KV->KExtraAngularDamping > 0.f)
		{
			B->AddTorqueInRadians(-B->GetPhysicsAngularVelocityInRadians() * KV->KExtraAngularDamping, NAME_None, true);
		}
		if (KV->bWaterVolume && S.KBuoyancy > 0.f)
		{
			B->AddForce(-LocalG * S.KBuoyancy * KV->KBuoyancy, NAME_None, true);
		}
	}

	// ---- suspension / thrusters ----
	const int32 N = S.SuspensionPoints.Num();
	const float GEff = FMath::Abs(WorldG * S.GravityScale);
	const float K = GEff / FMath::Max(1.f, S.RestCompression * S.RestLength);
	const float Damp = 2.f * S.DampingRatio * FMath::Sqrt(K);
	const float PointMass = Mass / FMath::Max(1, N);
	const float RayLen = S.RestLength + S.WheelRadius + (bHover ? S.RestLength * 0.5f : 8.f);
	FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaONSSuspension), false, this);
	AddIgnoredActors(QP);
	int32 Grounded = 0;
	FVector NormalSum = FVector::ZeroVector;
	FVector SupportSum = FVector::ZeroVector;
	if (WheelCompression.Num() != N)
	{
		WheelCompression.Init(S.RestLength, N);
	}
	for (int32 i = 0; i < N; ++i)
	{
		const FVector P = T.TransformPosition(S.SuspensionPoints[i]);
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, P, P - Up * RayLen, ECC_Visibility, QP) && !Cast<APawn>(Hit.GetActor()))
		{
			const float SpringLen = Hit.Distance - S.WheelRadius;
			const float X = S.RestLength - SpringLen;
			WheelCompression[i] = FMath::Clamp(SpringLen, 0.f, S.RestLength);
			const float PointVelUp = FVector::DotProduct(B->GetPhysicsLinearVelocityAtPoint(P), Up);
			float A = K * X - Damp * PointVelUp;
			if (X > S.RestLength * 0.9f)
			{
				A += K * 4.f * (X - S.RestLength * 0.9f); // bump stop
			}
			if (A > 0.f)
			{
				B->AddForceAtLocation(Up * A * PointMass, P);
			}
			if (X > -S.RestLength * 0.25f)
			{
				++Grounded;
				NormalSum += Hit.ImpactNormal;
				SupportSum += P;
			}
		}
		else
		{
			WheelCompression[i] = S.RestLength;
		}
	}
	GroundedFraction = N > 0 ? float(Grounded) / float(N) : 0.f;
	AvgGroundNormal = Grounded > 0 ? NormalSum.GetSafeNormal() : FVector::UpVector;

	// ---- drive ----
	FVector AngVel = B->GetPhysicsAngularVelocityInDegrees();
	const FVector GroundUp = AvgGroundNormal;
	const FVector FwdG = (Fwd - GroundUp * FVector::DotProduct(Fwd, GroundUp)).GetSafeNormal();
	const FVector RightG = FVector::CrossProduct(GroundUp, FwdG);
	const float Speed = FVector::DotProduct(Vel, FwdG);
	const float Lat = FVector::DotProduct(Vel, RightG);
	const float CurYaw = T.Rotator().Yaw;
	const FRotator HullYaw(0.f, CurYaw, 0.f);
	const FVector FwdFlat = HullYaw.Vector();
	const FVector RightFlat = FRotationMatrix(HullYaw).GetUnitAxis(EAxis::Y);

	float UprightK = 4.f;
	float UprightD = 2.f;

	auto YawTowards = [&](float TargetYaw, float Gain)
	{
		const float Delta = FMath::FindDeltaAngleDegrees(CurYaw, TargetYaw);
		const float TargetRate = FMath::Clamp(Delta * Gain, -S.YawRate, S.YawRate);
		AngVel.Z = FMath::FInterpTo(AngVel.Z, TargetRate, DeltaSeconds, 10.f);
	};

	// UE2 ONSWheeledCraft / ONSHoverCraft StopThreshold (100 UU/s): with no throttle and slower than that on the ground the
	// brakes hold the vehicle (bHandbrake / MinBrakeFriction 4): static friction cancels the creep along the ground and
	// gravity's pull down a slope, up to the friction limit, so a parked or idle vehicle stays where it is
	bool bHold = false;
	{
		const bool bNoInput = !bDriven || (FMath::Abs(Input.Throttle) < 0.05f && FMath::Abs(Input.Steer) < 0.05f && !Input.bJump && FMath::Abs(Input.Rise) < 0.05f);
		const FVector PlaneN = bHover ? FVector::UpVector : GroundUp;
		const FVector VelT = Vel - PlaneN * FVector::DotProduct(Vel, PlaneN);
		if ((S.Mode != EArenaONS_DriveMode::Flyer || !bDriven) && bNoInput && Grounded > 0 && GroundedFraction >= 0.5f && VelT.Size() < 100.f * 2.f)
		{
			bHold = true;
			const FVector GTot = LocalG * S.GravityScale; // the body's whole gravity (scene WorldG + our difference)
			const FVector GT = GTot - PlaneN * FVector::DotProduct(GTot, PlaneN);
			// where the brakes caught it: a stiff spring back to that spot takes out what the per-step velocity cancel leaves
			// (hover thrust along a tilted hull on a slope)
			if (!bHoldAnchorSet)
			{
				HoldAnchor = GetActorLocation();
				bHoldAnchorSet = true;
			}
			FVector Back = HoldAnchor - GetActorLocation();
			Back -= PlaneN * FVector::DotProduct(Back, PlaneN);
			FVector Hold = -VelT / FMath::Max(DeltaSeconds, 0.001f) - GT + Back * 25.f;
			Hold = Hold.GetClampedToMaxSize(float(GTot.Size()) * 1.2f);
			B->AddForce(Hold, NAME_None, true);
			if (!(bHover && bDriven))
			{
				AngVel.Z = FMath::FInterpTo(AngVel.Z, 0.f, DeltaSeconds, 12.f);
			}
		}
	}

	if (!bHold)
	{
		bHoldAnchorSet = false;
	}

	switch (S.Mode)
	{
	case EArenaONS_DriveMode::Wheeled:
	case EArenaONS_DriveMode::Tracked:
	{
		if (Grounded > 0 && bHold)
		{
			UprightK = 3.f;
			UprightD = 1.5f;
		}
		else if (Grounded > 0)
		{
			const float G = GroundedFraction;
			const float Thr = Input.Throttle;
			float Acc = 0.f;
			float GripMul = 1.f;
			if (Thr > 0.05f)
			{
				if (Speed < -60.f)
				{
					Acc = S.Brake * Thr;
				}
				else if (Speed < S.MaxSpeed * Thr)
				{
					Acc = S.Accel * Thr * (1.f - 0.6f * FMath::Clamp(Speed / S.MaxSpeed, 0.f, 1.f));
				}
			}
			else if (Thr < -0.05f)
			{
				if (Speed > 60.f)
				{
					Acc = -S.Brake * -Thr;
				}
				else if (Speed > -S.ReverseSpeed)
				{
					Acc = -S.Accel * 0.7f * -Thr;
				}
			}
			else
			{
				Acc = FMath::Clamp(-Speed * (bDriven ? 0.8f : 3.f), -900.f, 900.f);
			}
			if (Input.bJump && S.Mode == EArenaONS_DriveMode::Wheeled)
			{
				// Handbrake: slide
				Acc = FMath::Clamp(-Speed * 2.f, -S.Brake, S.Brake);
				GripMul = 0.25f;
			}
			const float LatAcc = FMath::Clamp(-Lat * S.LateralGrip * GripMul, -4500.f, 4500.f);
			B->AddForce((FwdG * Acc + RightG * LatAcc) * G, NAME_None, true);

			const float SpeedFactor = S.Mode == EArenaONS_DriveMode::Tracked ? 1.f : FMath::Clamp(Speed / 450.f, -1.f, 1.f);
			const float TargetYawRate = Input.Steer * S.YawRate * SpeedFactor;
			const float CurRate = FVector::DotProduct(AngVel, Up);
			const float NewRate = FMath::FInterpTo(CurRate, TargetYawRate, DeltaSeconds, 6.f);
			AngVel += Up * (NewRate - CurRate) * G;
			UprightK = 3.f;
			UprightD = 1.5f;
		}
		else
		{
			UprightK = 5.f;
			UprightD = 2.5f;
		}
		break;
	}
	case EArenaONS_DriveMode::Hover:
	{
		const float Control = Grounded > 0 ? 1.f : 0.35f;
		FVector DesiredVel = FVector::ZeroVector;
		if (bDriven)
		{
			const float Fw = Input.Throttle >= 0.f ? Input.Throttle * S.MaxSpeed : Input.Throttle * S.ReverseSpeed;
			DesiredVel = FwdFlat * Fw + RightFlat * Input.Steer * S.MaxSpeed * 0.7f;
		}
		const FVector VelH(Vel.X, Vel.Y, 0.f);
		FVector Acc = (DesiredVel - VelH) * (bDriven ? S.LateralGrip : 1.5f);
		Acc = Acc.GetClampedToMaxSize(S.Accel) * Control;
		if (!bHold)
		{
			B->AddForce(Acc, NAME_None, true);
		}
		if (bDriven && Input.bHasDesiredYaw)
		{
			YawTowards(Input.DesiredYaw, 5.f);
		}
		else
		{
			AngVel.Z = FMath::FInterpTo(AngVel.Z, 0.f, DeltaSeconds, 4.f);
		}
		if (bDriven && Input.bJump && Grounded > 0 && Now - LastJumpTime > 1.2f)
		{
			LastJumpTime = Now;
			B->AddImpulse(FVector(0.f, 0.f, S.JumpSpeed), NAME_None, true);
			ArenaAssets::PlaySoundAt(this, TEXT("S_Jump"), GetActorLocation(), 0.8f, 0.7f);
		}
		UprightK = 18.f;
		UprightD = 6.f;
		break;
	}
	case EArenaONS_DriveMode::Flyer:
	{
		if (bDriven)
		{
			const float Fw = Input.Throttle >= 0.f ? Input.Throttle * S.MaxSpeed : Input.Throttle * S.ReverseSpeed;
			const FVector DesiredVel = FwdFlat * Fw + RightFlat * Input.Steer * S.MaxSpeed * 0.6f + FVector(0.f, 0.f, Input.Rise * S.VerticalSpeed);
			FVector Acc = (DesiredVel - Vel) * 1.6f;
			FVector AccH(Acc.X, Acc.Y, 0.f);
			AccH = AccH.GetClampedToMaxSize(S.Accel);
			const float AccZ = FMath::Clamp(Acc.Z, -1600.f, 1600.f);
			B->AddForce(FVector(AccH.X, AccH.Y, AccZ), NAME_None, true);
			if (Input.bHasDesiredYaw)
			{
				YawTowards(Input.DesiredYaw, 4.f);
			}
			UprightK = 14.f;
			UprightD = 5.f;
		}
		else
		{
			AngVel.Z = FMath::FInterpTo(AngVel.Z, 0.f, DeltaSeconds, 3.f);
			UprightK = 5.f;
			UprightD = 2.f;
		}
		break;
	}
	}
	// Partly over a drop-off: pull the body back over the supported side (empty vehicles and hover craft).
	if (Grounded > 0 && Grounded < N && (!bDriven || bHover) && !bHold)
	{
		FVector Pull = SupportSum / float(Grounded) - T.GetLocation();
		Pull.Z = 0.f;
		if (!Pull.IsNearlyZero())
		{
			B->AddForce(Pull.GetSafeNormal() * (bDriven ? 700.f : 1100.f), NAME_None, true);
		}
	}
	// Empty and resting on something: parking brake so nothing drifts off.
	if (!bDriven && Grounded > 0 && S.Mode != EArenaONS_DriveMode::Hover && !bHold)
	{
		const FVector VelH(Vel.X, Vel.Y, 0.f);
		B->AddForce((-VelH * 4.f).GetClampedToMaxSize(2500.f), NAME_None, true);
		AngVel.Z = FMath::FInterpTo(AngVel.Z, 0.f, DeltaSeconds, 6.f);
	}
	B->SetPhysicsAngularVelocityInDegrees(AngVel);

	// ---- keep upright (roll / pitch spring towards world up) ----
	{
		const FVector Axis = FVector::CrossProduct(Up, FVector::UpVector);
		const FVector AngRad = B->GetPhysicsAngularVelocityInRadians();
		const FVector AngPerp = AngRad - Up * FVector::DotProduct(AngRad, Up);
		B->AddTorqueInRadians(Axis * UprightK - AngPerp * UprightD, NAME_None, true);
	}
}

void AArenaONS_Vehicle::CheckFlipped(float DeltaSeconds)
{
	const FVector Up = GetActorUpVector();
	if (Up.Z < 0.3f && GetVelocity().Size() < 300.f)
	{
		FlippedTime += DeltaSeconds;
		if (FlippedTime > 2.5f)
		{
			FlippedTime = 0.f;
			const FVector Loc = GetActorLocation() + FVector(0.f, 0.f, GetSpec().HalfExtent.Z + 120.f);
			SetActorLocationAndRotation(Loc, FRotator(0.f, GetActorRotation().Yaw, 0.f), false, nullptr, ETeleportType::TeleportPhysics);
			Body->SetPhysicsLinearVelocity(FVector::ZeroVector);
			Body->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
		}
	}
	else
	{
		FlippedTime = 0.f;
	}
}

// =============================================================================================
// Weapons
// =============================================================================================

FVector AArenaONS_Vehicle::GetTurretPivot(bool bPassengerTurret) const
{
	const USceneComponent* C = bPassengerTurret ? RearBarrelRoot.Get() : BarrelRoot.Get();
	return C ? C->GetComponentLocation() : GetActorLocation();
}

void AArenaONS_Vehicle::UpdateTurretAim(const FRotator& AimRotation, float DeltaSeconds, bool bPassengerTurret)
{
	const FArenaONS_VehicleSpec& S = GetSpec();
	// Desired aim in hull space
	const FTransform HullT = VisualRoot ? VisualRoot->GetComponentTransform() : GetActorTransform();
	const FVector LocalDir = HullT.InverseTransformVectorNoScale(AimRotation.Vector());
	const FRotator LocalRot = LocalDir.Rotation();
	float& Yaw = bPassengerTurret ? RearTurretYaw : TurretYaw;
	float& Pitch = bPassengerTurret ? RearTurretPitch : TurretPitch;
	const float MaxStep = S.TurretRate * DeltaSeconds;
	Yaw = FRotator::NormalizeAxis(Yaw + FMath::Clamp(FMath::FindDeltaAngleDegrees(Yaw, LocalRot.Yaw), -MaxStep, MaxStep));
	const float TargetPitch = FMath::Clamp(LocalRot.Pitch, S.TurretMinPitch, S.TurretMaxPitch);
	Pitch = Pitch + FMath::Clamp(TargetPitch - Pitch, -MaxStep, MaxStep);
}

bool AArenaONS_Vehicle::TryFireGun(int32 GunIndex, AController* InInstigator, const FVector& AimPoint)
{
	const FArenaONS_VehicleSpec& S = GetSpec();
	UWorld* World = GetWorld();
	if (!World || !S.Guns.IsValidIndex(GunIndex) || GunIndex >= 4 || !ArenaONSVeh::IsPlayingPhase(this))
	{
		return false;
	}
	const float Now = World->GetTimeSeconds();
	const FArenaONS_GunSpec& Gun = S.Guns[GunIndex];
	if (Now < NextFireTime[GunIndex] || bOverheated[GunIndex])
	{
		return false;
	}
	NextFireTime[GunIndex] = Now + Gun.RefireTime;
	Heat[GunIndex] = FMath::Min(1.f, Heat[GunIndex] + Gun.HeatPerShot);
	if (Heat[GunIndex] >= 0.999f)
	{
		bOverheated[GunIndex] = true;
	}

	if (Gun.bBoost)
	{
		if (GroundedFraction > 0.f || GetSpec().Mode == EArenaONS_DriveMode::Hover)
		{
			Body->AddImpulse(GetActorForwardVector() * 1500.f + FVector(0.f, 0.f, 120.f), NAME_None, true);
			ArenaFX::SpawnExplosion(World, GetActorLocation() - GetActorForwardVector() * (S.HalfExtent.X + 30.f), FLinearColor(0.3f, 0.6f, 1.f), 120.f, TEXT("Explosion.Grenade"));
			ArenaAssets::PlaySoundAt(this, TEXT("S_Combo"), GetActorLocation(), 0.8f, 1.3f);
		}
		return true;
	}

	FTransform BaseT = VisualRoot ? VisualRoot->GetComponentTransform() : GetActorTransform();
	if (Gun.bTurret)
	{
		const USceneComponent* Barrel = Gun.Seat == 1 ? RearBarrelRoot.Get() : BarrelRoot.Get();
		if (Barrel)
		{
			BaseT = Barrel->GetComponentTransform();
		}
	}
	const int32 NumMuzzles = FMath::Max(1, Gun.Muzzles.Num());
	const FVector LocalMuzzle = Gun.Muzzles.Num() > 0 ? Gun.Muzzles[MuzzleIndex[GunIndex] % NumMuzzles] : FVector(S.HalfExtent.X + 40.f, 0.f, 0.f);
	FVector Muzzle = BaseT.TransformPosition(LocalMuzzle);
	// The art model's gun meshes know where their barrels are.
	ArtRig.GetMuzzle(GunIndex, MuzzleIndex[GunIndex], Muzzle);
	MuzzleIndex[GunIndex]++;
	const FVector BaseFwd = BaseT.GetUnitAxis(EAxis::X);
	FVector Dir = (AimPoint - Muzzle).GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		Dir = BaseFwd;
	}
	const float Cone = Gun.bTurret ? 5.f : Gun.AimCone;
	if (FVector::DotProduct(Dir, BaseFwd) < FMath::Cos(FMath::DegreesToRadians(Cone)))
	{
		Dir = BaseFwd;
	}
	ArenaONS::FireGunShot(this, Gun, InInstigator, Muzzle, Dir);
	ArenaViewShake::WeaponShakeView(InInstigator, Gun.Shake);
	ArenaFX::SpawnImpact(World, Muzzle, Dir, Gun.Color, Gun.bTurret && !Gun.bHitscan ? 40.f : 12.f, Gun.bHitscan ? TEXT("Muzzle.Minigun") : TEXT("Muzzle.Rocket"));
	const TCHAR* ArtSound = ArtRig.IsValid() ? ArtRig.Art->GunSound[GunIndex] : nullptr;
	if (ArtSound && ArenaArtVehicles::LoadSound(ArtSound))
	{
		if (Now - LastGunSoundTime[GunIndex] >= ArtRig.Art->GunSoundInterval[GunIndex])
		{
			LastGunSoundTime[GunIndex] = Now;
			PlayArtSound(ArtSound, Muzzle, 0.9f);
		}
	}
	else if (!Gun.Sound.IsNone())
	{
		ArenaAssets::PlaySoundAt(this, Gun.Sound, Muzzle, 0.8f, Gun.bTurret && !Gun.bHitscan ? 0.7f : 1.f);
	}
	if (Gun.bTurret && !Gun.bHitscan && Body)
	{
		Body->AddImpulse(-BaseFwd * 120.f, NAME_None, true); // recoil
	}
	return true;
}

void AArenaONS_Vehicle::UpdateWeapons(float DeltaSeconds)
{
	const FArenaONS_VehicleSpec& S = GetSpec();
	for (int32 i = 0; i < S.Guns.Num() && i < 4; ++i)
	{
		Heat[i] = FMath::Max(0.f, Heat[i] - S.Guns[i].CoolRate * DeltaSeconds);
		if (bOverheated[i] && Heat[i] < 0.3f)
		{
			bOverheated[i] = false;
		}
	}
	AController* C = GetController();
	if (!C)
	{
		return;
	}
	// Turret follows the aim
	bool bHasTurret = false;
	for (const FArenaONS_GunSpec& G : S.Guns)
	{
		bHasTurret |= G.bTurret && G.Seat == 0;
	}
	if (ArtRig.IsValid())
	{
		for (const ArenaArtVehicles::FGun& AG : ArtRig.Art->Guns)
		{
			bHasTurret |= AG.Aim == ArenaArtVehicles::EAim::Driver;
		}
	}
	if (bHasTurret)
	{
		UpdateTurretAim(Input.AimRotation, DeltaSeconds, false);
	}
	for (int32 i = 0; i < S.Guns.Num() && i < 4; ++i)
	{
		const FArenaONS_GunSpec& G = S.Guns[i];
		if (G.Seat != 0)
		{
			continue;
		}
		const bool bPressed = G.FireMode == 0 ? Input.bFire : Input.bAltFire;
		if (bPressed)
		{
			TryFireGun(i, C, Input.AimPoint);
		}
	}
}

void AArenaONS_Vehicle::UpdateRoadkill(float DeltaSeconds)
{
	AController* C = GetController();
	UWorld* World = GetWorld();
	const FArenaONS_VehicleSpec& S = GetSpec();
	if (!C || !World || !ArenaONSVeh::IsPlayingPhase(this))
	{
		return;
	}
	const FVector Vel = GetVelocity();
	const float Speed = Vel.Size();
	if (Speed < S.RoadkillSpeed)
	{
		return;
	}
	const FVector Ext = S.HalfExtent + FVector(40.f, 30.f, 60.f);
	FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaONSRoadkill), false, this);
	AddIgnoredActors(QP);
	TArray<FOverlapResult> Overlaps;
	World->OverlapMultiByObjectType(Overlaps, GetActorLocation() + Vel.GetSafeNormal() * 40.f, GetActorQuat(), FCollisionObjectQueryParams(ECC_Pawn), FCollisionShape::MakeBox(Ext), QP);
	for (const FOverlapResult& O : Overlaps)
	{
		AArenaCharacter* Char = Cast<AArenaCharacter>(O.GetActor());
		if (!Char || !Char->IsAlive() || !Char->GetActorEnableCollision())
		{
			continue;
		}
		const FVector To = Char->GetActorLocation() - GetActorLocation();
		const float Closing = FVector::DotProduct(Vel - Char->GetVelocity(), To.GetSafeNormal());
		if (Closing < S.RoadkillSpeed * 0.7f)
		{
			continue;
		}
		AController* VictimC = Char->GetController();
		UWorld* W = World;
		const AArenaGameMode* GM = W->GetAuthGameMode<AArenaGameMode>();
		if (GM && VictimC && GM->AreOnSameTeam(C, VictimC))
		{
			// Nudge team mates out of the way
			Char->LaunchCharacter((To.GetSafeNormal2D() + FVector(0.f, 0.f, 0.4f)) * 500.f, true, true);
			continue;
		}
		FArenaHitInfo Info;
		Info.WeaponName = TEXT("Roadkill");
		Info.Momentum = Vel * 0.8f + FVector(0.f, 0.f, 400.f);
		ArenaDamage::ApplyDamage(Char, 1000.f, C, this, Info);
		const APlayerState* PS = C->PlayerState;
		UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: roadkill by %s in %s"), PS ? *PS->GetPlayerName() : TEXT("?"), *GetVehicleName());
	}
}

// =============================================================================================
// AI driving
// =============================================================================================

bool AArenaONS_Vehicle::FindAITarget(AActor*& OutTarget, FVector& OutAimPoint) const
{
	UWorld* World = GetWorld();
	OutTarget = nullptr;
	if (!World)
	{
		return false;
	}
	const FArenaONS_VehicleSpec& S = GetSpec();
	const float Range = S.Mode == EArenaONS_DriveMode::Flyer ? 7000.f : 5500.f;
	const FVector Eye = GetActorLocation() + FVector(0.f, 0.f, S.HalfExtent.Z + 50.f);
	FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaONSAISight), false, this);
	AddIgnoredActors(QP);

	float BestScore = TNumericLimits<float>::Max();
	FVector BestPoint = FVector::ZeroVector;
	AActor* Best = nullptr;
	for (TActorIterator<AArenaCharacter> It(World); It; ++It)
	{
		AArenaCharacter* Char = *It;
		if (!Char || !Char->IsAlive())
		{
			continue;
		}
		const int32 T = ArenaONS::TeamOfCharacter(Char);
		if (T < 0 || T == Team)
		{
			continue;
		}
		AArenaONS_Vehicle* Ridden = ArenaONS::GetRiddenVehicle(Char);
		AActor* TargetActor = Ridden ? static_cast<AActor*>(Ridden) : static_cast<AActor*>(Char);
		const FVector Point = TargetActor->GetActorLocation();
		const float Dist = FVector::Dist(Eye, Point);
		if (Dist > Range)
		{
			continue;
		}
		const float Score = Dist * (Ridden ? 0.8f : 1.f);
		if (Score >= BestScore)
		{
			continue;
		}
		FCollisionQueryParams Q2 = QP;
		Q2.AddIgnoredActor(TargetActor);
		Q2.AddIgnoredActor(Char);
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, Eye, Point, ECC_Visibility, Q2))
		{
			continue;
		}
		BestScore = Score;
		BestPoint = Point;
		Best = TargetActor;
	}
	if (!Best)
	{
		// Siege: an attackable node / core in range
		if (UArenaRules_ONS* Rules = ArenaONSVeh::GetRules(this))
		{
			for (AArenaONS_PowerNode* Node : Rules->GetNodes())
			{
				if (!Node || !Node->IsAttackableBy(Team))
				{
					continue;
				}
				const FVector Point = Node->GetAimPoint();
				const float Dist = FVector::Dist(Eye, Point);
				if (Dist > Range || Dist >= BestScore)
				{
					continue;
				}
				FCollisionQueryParams Q2 = QP;
				Q2.AddIgnoredActor(Node);
				FHitResult Hit;
				if (World->LineTraceSingleByChannel(Hit, Eye, Point, ECC_Visibility, Q2))
				{
					continue;
				}
				BestScore = Dist;
				BestPoint = Point;
				Best = Node;
			}
		}
	}
	if (!Best && !ArenaONSVeh::GetRules(this))
	{
		// other modes (Assault): the mode's targets for vehicles
		const AArenaGameMode* GM = World->GetAuthGameMode<AArenaGameMode>();
		UArenaGameRules* ModeRules = GM ? GM->GetRules() : nullptr;
		AActor* Siege = nullptr;
		FVector SiegePoint;
		if (ModeRules && ModeRules->GetVehicleSiegeTarget(Team, Eye, Range, Siege, SiegePoint))
		{
			Best = Siege;
			BestPoint = SiegePoint;
		}
	}
	if (!Best)
	{
		return false;
	}
	// Lead moving targets with the primary gun's projectile speed.
	const FArenaONS_GunSpec* Gun = FindGun(0, 0);
	if (Gun && !Gun->bHitscan && Gun->Projectile.Speed > 0.f)
	{
		const float Time = FVector::Dist(Eye, BestPoint) / Gun->Projectile.Speed;
		BestPoint += Best->GetVelocity() * FMath::Min(Time, 1.5f);
		if (Gun->Projectile.GravityScale > 0.f)
		{
			BestPoint.Z += 0.5f * FMath::Abs(World->GetGravityZ()) * Gun->Projectile.GravityScale * Time * Time;
		}
	}
	OutTarget = Best;
	OutAimPoint = BestPoint;
	return true;
}

void AArenaONS_Vehicle::RepathAI(const FVector& Destination)
{
	AIPath.Reset();
	AIPathIndex = 0;
	UWorld* World = GetWorld();
	AArenaGameMode* GM = World ? World->GetAuthGameMode<AArenaGameMode>() : nullptr;
	AArenaNavGraph* Nav = GM ? GM->GetNavGraph() : nullptr;
	if (!Nav || GetSpec().Mode == EArenaONS_DriveMode::Flyer)
	{
		return;
	}
	TArray<EArenaNavLinkType> Links;
	TArray<FVector> Path;
	if (Nav->FindPath(GetActorLocation(), Destination, Path, Links))
	{
		AIPath = Path;
	}
}

FVector AArenaONS_Vehicle::GetAISteerTarget()
{
	const FVector Loc = GetActorLocation();
	const float Now = GetWorld()->GetTimeSeconds();
	const float LookAhead = FMath::Max(GetSpec().Mode == EArenaONS_DriveMode::Hover ? 380.f : 480.f, GetVelocity().Size2D() * 0.35f);
	while (AIPath.IsValidIndex(AIPathIndex) && AIPathIndex < AIPath.Num() - 1 && FVector::Dist2D(Loc, AIPath[AIPathIndex]) < LookAhead)
	{
		++AIPathIndex;
	}
	// Orbit guard: no progress towards the current point for a while -> skip it (or repath at the end).
	if (AIPath.IsValidIndex(AIPathIndex))
	{
		const float D = FVector::Dist2D(Loc, AIPath[AIPathIndex]);
		if (AIPathIndex != AIOrbitIndex || D < AIOrbitBestDist - 150.f)
		{
			AIOrbitIndex = AIPathIndex;
			AIOrbitBestDist = D;
			AIOrbitTime = Now;
		}
		else if (Now - AIOrbitTime > 3.f)
		{
			AIOrbitTime = Now;
			if (AIPathIndex < AIPath.Num() - 1)
			{
				++AIPathIndex;
			}
			else
			{
				AINextRepathTime = 0.f;
			}
		}
	}
	if (AIPath.IsValidIndex(AIPathIndex))
	{
		return AIPath[AIPathIndex];
	}
	return AIDestination;
}

void AArenaONS_Vehicle::GatherAIInput(AController* C, float DeltaSeconds)
{
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();
	const FArenaONS_VehicleSpec& S = GetSpec();
	UArenaRules_ONS* Rules = ArenaONSVeh::GetRules(this);
	const FVector Loc = GetActorLocation();
	const float CurYaw = GetActorRotation().Yaw;
	const bool bPlayerAutopilot = Cast<APlayerController>(C) != nullptr;
	if (bPlayerAutopilot)
	{
		ConsumeMovementInputVector();
	}

	// ---- destination ----
	if (Now >= AINextDestTime)
	{
		AINextDestTime = Now + 2.f;
		FVector Dest = Loc;
		AArenaONS_PowerNode* Node = nullptr;
		bool bAttack = false;
		bool bHasDest = Rules && Rules->GetDriveDestination(C, Loc, Dest, Node, bAttack);
		if (!Rules)
		{
			// other modes with our vehicles (Assault maps): the mode's destination for the driver
			const AArenaGameMode* GM = World->GetAuthGameMode<AArenaGameMode>();
			UArenaGameRules* ModeRules = GM ? GM->GetRules() : nullptr;
			bHasDest = ModeRules && ModeRules->GetVehicleDriveDestination(C, Loc, Dest);
		}
		if (bHasDest)
		{
			if (FVector::DistSquared(Dest, AIDestination) > FMath::Square(300.f))
			{
				AIDestination = Dest;
				AIPath.Reset();
				AINextRepathTime = 0.f;
			}
		}
		AITargetNode = Node;
		bAIAttackNode = bAttack;
	}
	if (Now >= AINextRepathTime && S.Mode != EArenaONS_DriveMode::Flyer)
	{
		AINextRepathTime = Now + 5.f;
		RepathAI(AIDestination);
	}

	// ---- target / aim ----
	if (Now >= AINextTargetTime)
	{
		AINextTargetTime = Now + 0.3f;
		AActor* Target = nullptr;
		FVector Aim;
		if (FindAITarget(Target, Aim))
		{
			AITarget = Target;
			AITargetAim = Aim;
		}
		else
		{
			AITarget.Reset();
		}
	}
	const bool bHasTarget = AITarget.IsValid();
	// a flyer outside Onslaught (AS-MotherShip's space fighters) never gets out: there is nowhere to walk to
	const bool bSpaceFlyer = !Rules && S.Mode == EArenaONS_DriveMode::Flyer;
	const FVector Eye = GetPawnViewLocation();
	const FVector AimPoint = bHasTarget ? AITargetAim : Eye + FRotator(-4.f, CurYaw, 0.f).Vector() * 3000.f;
	const FRotator DesiredAim = (AimPoint - Eye).Rotation();
	AIAimRotation = FMath::RInterpTo(AIAimRotation, DesiredAim, DeltaSeconds, 5.f);
	Input.AimRotation = AIAimRotation;
	Input.AimPoint = Eye + AIAimRotation.Vector() * FVector::Dist(Eye, AimPoint);
	C->SetControlRotation(AIAimRotation);

	// ---- arrival / staying ----
	const float DistToDest = FVector::Dist2D(Loc, AIDestination);
	AArenaONS_PowerNode* Node = AITargetNode.Get();
	const bool bStay = S.bBotStaysAtObjective && bAIAttackNode && Node && Node->IsAttackableBy(Team);
	const float StayRange = S.Mode == EArenaONS_DriveMode::Flyer ? 2500.f : 3200.f;
	bool bHold = false;
	if (bStay && Node && FVector::Dist(Loc, Node->GetActorLocation()) < StayRange)
	{
		if (Now >= AINextSightTime)
		{
			AINextSightTime = Now + 0.5f;
			FCollisionQueryParams SQ(SCENE_QUERY_STAT(ArenaONSNodeSight), false, this);
			AddIgnoredActors(SQ);
			SQ.AddIgnoredActor(Node);
			FHitResult Block;
			bAINodeVisible = !World->LineTraceSingleByChannel(Block, GetPawnViewLocation(), Node->GetAimPoint(), ECC_Visibility, SQ);
		}
		bHold = bAINodeVisible;
	}
	else if (DistToDest < S.BotArriveRadius)
	{
		// Arrived: get out (gunships land first). The debug autopilot exits too, to exercise the exit code.
		if (S.Mode != EArenaONS_DriveMode::Flyer || (GroundedFraction > 0.f && !bSpaceFlyer))
		{
			Input.bUse = true;
		}
	}
	// Bail out of a dying vehicle near the objective
	if (!bPlayerAutopilot && !bSpaceFlyer && GetHealthFraction() < 0.15f && DistToDest < 3000.f)
	{
		Input.bUse = true;
	}

	// ---- steering ----
	const FVector SteerTarget = (S.Mode == EArenaONS_DriveMode::Flyer || bHold) ? (bHold && Node ? Node->GetActorLocation() : AIDestination) : GetAISteerTarget();
	const float TargetYaw = (SteerTarget - Loc).Rotation().Yaw;
	const float Delta = FMath::FindDeltaAngleDegrees(CurYaw, TargetYaw);
	const float AbsDelta = FMath::Abs(Delta);
	const float Speed2D = GetVelocity().Size2D();
	const bool bReversing = Now < AIReverseUntil;

	switch (S.Mode)
	{
	case EArenaONS_DriveMode::Wheeled:
	{
		Input.Steer = FMath::Clamp(Delta / 30.f, -1.f, 1.f);
		Input.Throttle = AbsDelta > 100.f ? 0.5f : (AbsDelta > 45.f ? 0.75f : 1.f);
		// Too fast to make the turn: brake.
		const float TurnRadius = Speed2D / FMath::DegreesToRadians(FMath::Max(10.f, S.YawRate));
		if (AbsDelta > 35.f && FVector::Dist2D(Loc, SteerTarget) < TurnRadius * 1.6f)
		{
			Input.Throttle = Speed2D > 500.f ? -0.7f : 0.45f;
		}
		if (DistToDest < S.BotArriveRadius * 2.f)
		{
			Input.Throttle *= 0.6f;
		}
		break;
	}
	case EArenaONS_DriveMode::Tracked:
		Input.Steer = FMath::Clamp(Delta / 20.f, -1.f, 1.f);
		Input.Throttle = AbsDelta < 35.f ? 1.f : 0.15f;
		break;
	case EArenaONS_DriveMode::Hover:
		Input.DesiredYaw = TargetYaw;
		Input.bHasDesiredYaw = true;
		Input.Throttle = AbsDelta < 50.f ? 1.f : 0.35f;
		break;
	case EArenaONS_DriveMode::Flyer:
	{
		Input.DesiredYaw = bHasTarget && bHold ? DesiredAim.Yaw : TargetYaw;
		Input.bHasDesiredYaw = true;
		if (!Rules)
		{
			// Other modes (Assault: AS-MotherShip's space fighters): fly straight at the destination in 3D (the mode gives
			// a spot with a clear shot at its target), climb only over what is ahead, never land; close to it, slow down,
			// level with the target and turn to face it so the guns bear (UT2004's space fighter bots strafe and fire)
			FCollisionQueryParams QS(SCENE_QUERY_STAT(ArenaONSSpace), false, this);
			AddIgnoredActors(QS);
			const float Dist3D = FVector::Dist(Loc, AIDestination);
			float WantZ = AIDestination.Z;
			Input.Throttle = FMath::Clamp(Dist3D / 1500.f, 0.15f, 1.f) * (AbsDelta < 45.f ? 1.f : 0.3f);
			FHitResult Block;
			const FVector Ahead3D = Loc + FRotator(0.f, CurYaw, 0.f).Vector() * 1600.f;
			if (World->LineTraceSingleByChannel(Block, Loc, Ahead3D, ECC_Visibility, QS))
			{
				WantZ = Loc.Z + 800.f;
				Input.Throttle *= 0.3f;
			}
			if (Dist3D < 1800.f && bHasTarget)
			{
				Input.DesiredYaw = DesiredAim.Yaw;
				Input.Throttle = FMath::Abs(FMath::FindDeltaAngleDegrees(CurYaw, DesiredAim.Yaw)) < 20.f ? 0.15f : 0.f;
				WantZ = FMath::Lerp(AIDestination.Z, AITargetAim.Z, 0.5f);
				// strafe side to side while the guns bear (no sitting still under the defenders' turrets)
				Input.Steer = FMath::Sin(Now * 0.9f + float(GetUniqueID() % 17u)) > 0.f ? 0.8f : -0.8f;
			}
			Input.Rise = FMath::Clamp((WantZ - Loc.Z) / 300.f, -1.f, 1.f);
			break;
		}
		const float Dist2D = FVector::Dist2D(Loc, SteerTarget);
		Input.Throttle = FMath::Clamp(Dist2D / 1500.f, 0.f, 1.f) * (AbsDelta < 45.f ? 1.f : 0.3f);
		// Altitude: stay well above the ground, climb over obstacles, land at the destination.
		float GroundZ = Loc.Z - 2000.f;
		FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaONSAlt), false, this);
		AddIgnoredActors(QP);
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, Loc, Loc - FVector(0.f, 0.f, 5000.f), ECC_Visibility, QP))
		{
			GroundZ = Hit.ImpactPoint.Z;
		}
		float DesiredZ = FMath::Max(GroundZ + 750.f, AIDestination.Z + 600.f);
		const FVector Ahead = Loc + FRotator(0.f, CurYaw, 0.f).Vector() * 1600.f;
		if (World->LineTraceSingleByChannel(Hit, Loc, Ahead, ECC_Visibility, QP))
		{
			DesiredZ = Loc.Z + 800.f;
			Input.Throttle *= 0.3f;
		}
		if (!bHold && DistToDest < S.BotArriveRadius * 1.5f)
		{
			// Land only where the ground is at the objective's level (never into a chasm).
			if (GroundZ > AIDestination.Z - 250.f)
			{
				DesiredZ = GroundZ;
				Input.Throttle = FMath::Min(Input.Throttle, 0.25f);
			}
			else
			{
				DesiredZ = AIDestination.Z + 450.f;
			}
		}
		if (bHold)
		{
			Input.Throttle = 0.f;
			DesiredZ = FMath::Max(DesiredZ, GroundZ + 900.f);
		}
		Input.Rise = FMath::Clamp((DesiredZ - Loc.Z) / 300.f, -1.f, 1.f);
		break;
	}
	}
	if (bHold && S.Mode != EArenaONS_DriveMode::Flyer)
	{
		// Turn to face it but don't advance (tank turret does the aiming)
		Input.Throttle = 0.f;
		if (S.Mode == EArenaONS_DriveMode::Hover)
		{
			Input.DesiredYaw = DesiredAim.Yaw;
		}
	}

	// ---- edge guard: don't drive off ledges / into chasms ----
	if (S.Mode != EArenaONS_DriveMode::Flyer && !bReversing && Input.Throttle > 0.f)
	{
		const FVector Vel = GetVelocity();
		const FVector Dir = Vel.Size2D() > 250.f ? Vel.GetSafeNormal2D() : GetActorForwardVector().GetSafeNormal2D();
		const FVector Side(-Dir.Y, Dir.X, 0.f);
		const float Near = S.HalfExtent.X + 110.f;
		const float Far = S.HalfExtent.X + FMath::Clamp(Vel.Size2D() * 0.5f, 200.f, 900.f);
		FCollisionQueryParams EQ(SCENE_QUERY_STAT(ArenaONSEdge), false, this);
		AddIgnoredActors(EQ);
		auto HasFloor = [&](const FVector& Probe)
		{
			FHitResult Floor;
			return World->LineTraceSingleByChannel(Floor, Probe + FVector(0.f, 0.f, 150.f), Probe - FVector(0.f, 0.f, 420.f), ECC_Visibility, EQ);
		};
		bool bNearEdge = false;
		bool bFarEdge = false;
		for (const float SideMul : { -1.f, 0.f, 1.f })
		{
			const FVector Lateral = Side * SideMul * S.HalfExtent.Y;
			bNearEdge |= !HasFloor(Loc + Dir * Near + Lateral);
			bFarEdge |= !HasFloor(Loc + Dir * Far + Lateral);
		}
		// Hover craft: keep off the walls (and out of wall openings).
		if (S.Mode == EArenaONS_DriveMode::Hover)
		{
			for (const float SideMul : { -1.f, 1.f })
			{
				FHitResult Wall;
				if (World->LineTraceSingleByChannel(Wall, Loc, Loc + Side * SideMul * (S.HalfExtent.Y + 130.f), ECC_Visibility, EQ))
				{
					Input.Steer = FMath::Clamp(Input.Steer - SideMul * 0.7f, -1.f, 1.f);
				}
			}
		}
		// Side drop-offs (narrow bridges): steer / strafe away from the open side.
		for (const float SideMul : { -1.f, 1.f })
		{
			const FVector SideProbe = Loc + Side * SideMul * (S.HalfExtent.Y + 140.f) + Dir * S.HalfExtent.X * 0.5f;
			if (!HasFloor(SideProbe))
			{
				Input.Steer = FMath::Clamp(Input.Steer - SideMul * 0.8f, -1.f, 1.f);
				if (S.Mode == EArenaONS_DriveMode::Hover)
				{
					Input.Throttle = FMath::Min(Input.Throttle, 0.6f);
				}
			}
		}
		if (bNearEdge)
		{
			// Right at the edge: back off and turn away.
			Input.Throttle = -1.f;
			Input.bJump = false;
			if (Speed2D < 250.f)
			{
				AIReverseUntil = Now + 1.f;
				AIReverseSteer = Delta > 0.f ? -1.f : 1.f;
				AIPath.Reset();
				AINextRepathTime = Now + 1.1f;
			}
		}
		else if (bFarEdge)
		{
			Input.Throttle = Speed2D > 400.f ? -0.8f : 0.3f;
			Input.bJump = false;
		}
	}

	// ---- stuck handling (ground) ----
	if (S.Mode != EArenaONS_DriveMode::Flyer)
	{
		if (bReversing)
		{
			Input.Throttle = -1.f;
			Input.Steer = AIReverseSteer;
			if (S.Mode == EArenaONS_DriveMode::Hover)
			{
				Input.Steer = AIReverseSteer;
				Input.bHasDesiredYaw = false;
			}
		}
		else if (FMath::Abs(Input.Throttle) > 0.3f && Speed2D < 140.f && Now - EnterTime > 2.f)
		{
			AIStuckTime += DeltaSeconds;
			if (AIStuckTime > 1.6f)
			{
				AIStuckTime = 0.f;
				++AIStuckCount;
				AIReverseUntil = Now + 1.3f;
				AIReverseSteer = Delta > 0.f ? -1.f : 1.f;
				AINextRepathTime = Now + 1.4f;
				AIPath.Reset();
			}
		}
		else
		{
			AIStuckTime = FMath::Max(0.f, AIStuckTime - DeltaSeconds);
		}
	}
	if (Now >= AIProgressCheckTime)
	{
		AIProgressCheckTime = Now + 8.f;
		if (bSpaceFlyer && FVector::Dist(Loc, AIDestination) < 1800.f)
		{
			AIStuckCount = 0; // holding at its firing spot
		}
		else if (!bHold && FVector::Dist(Loc, AILastPos) < 400.f)
		{
			AIStuckCount += S.Mode == EArenaONS_DriveMode::Flyer ? 2 : 1;
		}
		AILastPos = Loc;
	}
	if (AIStuckCount >= 5 && bSpaceFlyer)
	{
		// hung up on the hull: climb clear for a while and pick the destination again
		AIStuckCount = 0;
		AISpaceClimbUntil = Now + 2.5f;
		AINextDestTime = 0.f;
	}
	else if (AIStuckCount >= 5)
	{
		Input.bUse = true; // give up and walk
	}
	if (bSpaceFlyer && Now < AISpaceClimbUntil)
	{
		Input.Rise = 1.f;
		Input.Throttle = -0.3f;
	}

	// ---- shooting ----
	if (bHasTarget)
	{
		const FVector ToAim = (AimPoint - Loc).GetSafeNormal();
		const FVector HullFwd = GetActorForwardVector();
		const float HullAngle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(ToAim, HullFwd), -1.f, 1.f)));
		const float AimDist = FVector::Dist(Loc, AimPoint);
		for (int32 i = 0; i < S.Guns.Num(); ++i)
		{
			const FArenaONS_GunSpec& G = S.Guns[i];
			if (G.Seat != 0)
			{
				continue;
			}
			bool bShoot = false;
			if (G.bBoost)
			{
				bShoot = false;
			}
			else if (G.bTurret)
			{
				const FVector BarrelFwd = BarrelRoot ? BarrelRoot->GetForwardVector() : HullFwd;
				const FVector ToFromBarrel = (AimPoint - GetTurretPivot(false)).GetSafeNormal();
				const float Ang = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(ToFromBarrel, BarrelFwd), -1.f, 1.f)));
				bShoot = Ang < (G.bHitscan ? 6.f : 4.f) && (G.bHitscan ? AimDist < 4500.f && !Cast<AArenaONS_PowerNode>(AITarget.Get()) : AimDist > 700.f);
			}
			else
			{
				bShoot = HullAngle < G.AimCone + 4.f && AimDist < (G.FireMode == 0 ? 5000.f : 6000.f);
				if (G.FireMode == 1)
				{
					bShoot = bShoot && AimDist > 600.f;
				}
			}
			if (bShoot)
			{
				(G.FireMode == 0 ? Input.bFire : Input.bAltFire) = true;
			}
		}
	}
	// Nitro on long straights
	if (const FArenaONS_GunSpec* Boost = FindGun(0, 1))
	{
		if (Boost->bBoost && !bReversing && AbsDelta < 8.f && DistToDest > 3000.f && Speed2D > 700.f && FMath::FRand() < 0.02f)
		{
			Input.bAltFire = true;
		}
	}

	if (Now >= AINextLogTime)
	{
		AINextLogTime = Now + 6.f;
		const APlayerState* PS = C->PlayerState;
		UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: %s driving %s: dist %.0f to %s, speed %.0f, path %d/%d, stuck %d%s%s"),
			PS ? *PS->GetPlayerName() : TEXT("?"), *GetVehicleName(), DistToDest, Node ? *Node->GetNodeName() : TEXT("(none)"), GetVelocity().Size(),
			AIPathIndex, AIPath.Num(), AIStuckCount, bHasTarget ? TEXT(", shooting") : TEXT(""), bHold ? TEXT(", holding") : TEXT(""));
	}
}

// =============================================================================================
// Visuals
// =============================================================================================

UStaticMeshComponent* AArenaONS_Vehicle::AddPart(USceneComponent* Parent, UStaticMesh* Mesh, const FVector& Loc, const FRotator& Rot, const FVector& SizeCm, UMaterialInterface* Material, bool bShadow)
{
	if (!Mesh || !Parent)
	{
		return nullptr;
	}
	UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(this);
	Part->SetMobility(EComponentMobility::Movable);
	Part->SetStaticMesh(Mesh);
	Part->SetupAttachment(Parent);
	Part->SetRelativeTransform(FTransform(Rot, Loc, SizeCm / 100.f));
	Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Part->SetCollisionResponseToAllChannels(ECR_Ignore);
	Part->SetGenerateOverlapEvents(false);
	Part->SetCanEverAffectNavigation(false);
	Part->SetCastShadow(bShadow);
	if (Material)
	{
		Part->SetMaterial(0, Material);
	}
	Part->RegisterComponent();
	Parts.Add(Part);
	return Part;
}

void AArenaONS_Vehicle::BuildModel()
{
	if (bModelBuilt)
	{
		return;
	}
	bModelBuilt = true;
	if (UsesArtModel() && BuildArtModel())
	{
		BuiltTeam = Team;
		return;
	}
	UStaticMesh* Cube = ArenaAssets::Cube();
	UStaticMesh* Cyl = ArenaAssets::Cylinder();
	UStaticMesh* Sphere = ArenaAssets::Sphere();
	UStaticMesh* Cone = ArenaAssets::Cone();
	const FLinearColor TeamCol = TeamTint(Team);
	const FRotator AlongX(-90.f, 0.f, 0.f); // cylinder / cone axis -> +X
	const FRotator AlongY(0.f, 0.f, 90.f);  // cylinder axis -> Y (wheels)

	auto Paint = [this, &TeamCol]() { return ArenaAssets::MakeSurface(this, FMath::Lerp(FLinearColor(0.42f, 0.43f, 0.45f), TeamCol, 0.4f), 0.45f, 0.5f); };
	auto Metal = [this]() { return ArenaAssets::MakeSurface(this, FLinearColor(0.12f, 0.12f, 0.13f), 0.4f, 0.85f); };
	auto Rubber = [this]() { return ArenaAssets::MakeSurface(this, FLinearColor(0.03f, 0.03f, 0.03f), 0.9f, 0.f); };
	auto Glass = [this]() { return ArenaAssets::MakeGlow(this, FLinearColor(0.35f, 0.7f, 1.f), 1.5f, 0.55f); };
	auto Glow = [this, &TeamCol](float I) { return ArenaAssets::MakeGlow(this, TeamCol, I); };

	auto P = [&](USceneComponent* Parent, UStaticMesh* Mesh, const FVector& L, const FRotator& R, const FVector& Sz, bool bPaint)
	{
		UStaticMeshComponent* C = AddPart(Parent, Mesh, L, R, Sz, bPaint ? Paint() : Metal());
		if (C && bPaint)
		{
			TeamPaintParts.Add(C);
		}
		return C;
	};
	auto G = [&](USceneComponent* Parent, UStaticMesh* Mesh, const FVector& L, const FRotator& R, const FVector& Sz, float I)
	{
		UStaticMeshComponent* C = AddPart(Parent, Mesh, L, R, Sz, Glow(I), false);
		if (C)
		{
			TeamGlowParts.Add(C);
		}
		return C;
	};
	auto Wheel = [&](const FVector& At, float Diameter, float Width)
	{
		USceneComponent* W = NewObject<USceneComponent>(this);
		W->SetupAttachment(VisualRoot);
		W->SetRelativeLocation(At);
		W->RegisterComponent();
		WheelRoots.Add(W);
		USceneComponent* Spin = NewObject<USceneComponent>(this);
		Spin->SetupAttachment(W);
		Spin->RegisterComponent();
		AddPart(Spin, Cyl, FVector::ZeroVector, AlongY, FVector(Diameter, Diameter, Width), Rubber());
		AddPart(Spin, Cube, FVector::ZeroVector, AlongY, FVector(Diameter * 0.75f, Diameter * 0.12f, Width + 2.f), Metal());
		UStaticMeshComponent* Hub = AddPart(Spin, Cyl, FVector::ZeroVector, AlongY, FVector(Diameter * 0.4f, Diameter * 0.4f, Width + 4.f), Glow(4.f), false);
		TeamGlowParts.Add(Hub);
	};

	switch (VehicleType)
	{
	case EArenaONS_VehicleType::Wisp:
	{
		P(VisualRoot, Cube, FVector(0.f, 0.f, 0.f), FRotator::ZeroRotator, FVector(210.f, 70.f, 24.f), true);
		P(VisualRoot, Cone, FVector(138.f, 0.f, 0.f), AlongX, FVector(66.f, 46.f, 80.f), true);
		for (const float SY : { -1.f, 1.f })
		{
			P(VisualRoot, Cyl, FVector(0.f, SY * 60.f, -6.f), AlongX, FVector(34.f, 34.f, 230.f), false);
			P(VisualRoot, Cone, FVector(128.f, SY * 60.f, -6.f), AlongX, FVector(34.f, 34.f, 36.f), false);
			G(VisualRoot, Cube, FVector(0.f, SY * 60.f, 12.f), FRotator::ZeroRotator, FVector(170.f, 6.f, 4.f), 8.f);
			P(VisualRoot, Cube, FVector(-95.f, SY * 26.f, 28.f), FRotator(0.f, 0.f, SY * -22.f), FVector(50.f, 6.f, 40.f), true);
			AddPart(VisualRoot, Cyl, FVector(120.f, SY * 40.f, -2.f), AlongX, FVector(10.f, 10.f, 50.f), Metal());
		}
		AddPart(VisualRoot, Sphere, FVector(-5.f, 0.f, 18.f), FRotator::ZeroRotator, FVector(80.f, 48.f, 32.f), Glass(), false);
		for (const FVector& Pt : GetSpec().SuspensionPoints)
		{
			UStaticMeshComponent* T = G(VisualRoot, Cyl, Pt + FVector(0.f, 0.f, -8.f), FRotator::ZeroRotator, FVector(40.f, 40.f, 4.f), 10.f);
			ThrusterParts.Add(T);
		}
		G(VisualRoot, Cyl, FVector(-112.f, 0.f, 2.f), FRotator(90.f, 0.f, 0.f), FVector(30.f, 30.f, 4.f), 12.f);
		break;
	}
	case EArenaONS_VehicleType::Dustrunner:
	{
		AddPart(VisualRoot, Cube, FVector(0.f, 0.f, -10.f), FRotator::ZeroRotator, FVector(290.f, 140.f, 18.f), Metal());
		P(VisualRoot, Cube, FVector(80.f, 0.f, 10.f), FRotator(-6.f, 0.f, 0.f), FVector(120.f, 150.f, 34.f), true);
		P(VisualRoot, Cube, FVector(-105.f, 0.f, 8.f), FRotator::ZeroRotator, FVector(80.f, 150.f, 30.f), true);
		AddPart(VisualRoot, Cube, FVector(-15.f, 0.f, 18.f), FRotator::ZeroRotator, FVector(50.f, 50.f, 30.f), Rubber());
		for (const float SX : { -1.f, 1.f })
		{
			for (const float SY : { -1.f, 1.f })
			{
				AddPart(VisualRoot, Cyl, FVector(SX * 35.f - 15.f, SY * 58.f, 52.f), FRotator::ZeroRotator, FVector(7.f, 7.f, 70.f), Metal());
			}
			AddPart(VisualRoot, Cyl, FVector(SX * 35.f - 15.f, 0.f, 88.f), AlongY, FVector(7.f, 7.f, 122.f), Metal());
		}
		for (const float SY : { -1.f, 1.f })
		{
			AddPart(VisualRoot, Cyl, FVector(-15.f, SY * 58.f, 88.f), AlongX, FVector(7.f, 7.f, 76.f), Metal());
			G(VisualRoot, Cube, FVector(0.f, SY * 76.f, 14.f), FRotator::ZeroRotator, FVector(270.f, 6.f, 5.f), 8.f);
			AddPart(VisualRoot, Sphere, FVector(150.f, SY * 50.f, 10.f), FRotator::ZeroRotator, FVector(16.f), ArenaAssets::MakeGlow(this, FLinearColor(1.f, 0.95f, 0.8f), 12.f), false);
			AddPart(VisualRoot, Cyl, FVector(150.f, SY * 30.f, 26.f), AlongX, FVector(9.f, 9.f, 60.f), Metal());
		}
		for (const FVector& Pt : GetSpec().SuspensionPoints)
		{
			Wheel(Pt + FVector(0.f, FMath::Sign(Pt.Y) * 8.f, -GetSpec().RestLength * 0.55f), 76.f, 34.f);
		}
		break;
	}
	case EArenaONS_VehicleType::Bulwark:
	{
		P(VisualRoot, Cube, FVector(0.f, 0.f, 0.f), FRotator::ZeroRotator, FVector(420.f, 250.f, 70.f), true);
		P(VisualRoot, Cube, FVector(215.f, 0.f, -8.f), FRotator(-28.f, 0.f, 0.f), FVector(80.f, 240.f, 50.f), true);
		P(VisualRoot, Cube, FVector(-205.f, 0.f, 0.f), FRotator::ZeroRotator, FVector(40.f, 220.f, 60.f), false);
		for (const float SY : { -1.f, 1.f })
		{
			AddPart(VisualRoot, Cube, FVector(0.f, SY * 150.f, -18.f), FRotator::ZeroRotator, FVector(490.f, 70.f, 80.f), Rubber());
			G(VisualRoot, Cube, FVector(0.f, SY * 186.f, 2.f), FRotator::ZeroRotator, FVector(440.f, 4.f, 6.f), 7.f);
			for (int32 i = 0; i < 5; ++i)
			{
				AddPart(VisualRoot, Cyl, FVector(-180.f + i * 90.f, SY * 187.f, -32.f), AlongY, FVector(46.f, 46.f, 8.f), Metal());
			}
			P(VisualRoot, Cube, FVector(0.f, SY * 150.f, 28.f), FRotator::ZeroRotator, FVector(470.f, 76.f, 12.f), true);
		}
		TurretRoot->SetRelativeLocation(FVector(-20.f, 0.f, 40.f));
		G(TurretRoot, Cyl, FVector(0.f, 0.f, 0.f), FRotator::ZeroRotator, FVector(236.f, 236.f, 5.f), 6.f);
		P(TurretRoot, Cube, FVector(0.f, 0.f, 26.f), FRotator::ZeroRotator, FVector(190.f, 170.f, 52.f), true);
		P(TurretRoot, Sphere, FVector(-10.f, 0.f, 50.f), FRotator::ZeroRotator, FVector(200.f, 170.f, 70.f), true);
		AddPart(TurretRoot, Cyl, FVector(-30.f, 40.f, 86.f), FRotator::ZeroRotator, FVector(40.f, 40.f, 12.f), Metal());
		BarrelRoot->SetRelativeLocation(FVector(80.f, 0.f, 36.f));
		AddPart(BarrelRoot, Cyl, FVector(160.f, 0.f, 0.f), AlongX, FVector(24.f, 24.f, 300.f), Metal());
		AddPart(BarrelRoot, Cyl, FVector(305.f, 0.f, 0.f), AlongX, FVector(38.f, 38.f, 34.f), Metal());
		G(BarrelRoot, Cyl, FVector(250.f, 0.f, 0.f), AlongX, FVector(28.f, 28.f, 8.f), 8.f);
		AddPart(BarrelRoot, Cyl, FVector(70.f, 42.f, -8.f), AlongX, FVector(9.f, 9.f, 90.f), Metal());
		break;
	}
	case EArenaONS_VehicleType::Kestrel:
	{
		P(VisualRoot, Sphere, FVector(0.f, 0.f, 0.f), FRotator::ZeroRotator, FVector(360.f, 120.f, 100.f), true);
		AddPart(VisualRoot, Sphere, FVector(95.f, 0.f, 28.f), FRotator::ZeroRotator, FVector(120.f, 80.f, 58.f), Glass(), false);
		P(VisualRoot, Cube, FVector(-20.f, 0.f, 0.f), FRotator::ZeroRotator, FVector(90.f, 420.f, 12.f), true);
		for (const float SY : { -1.f, 1.f })
		{
			AddPart(VisualRoot, Cyl, FVector(-10.f, SY * 210.f, 0.f), AlongX, FVector(32.f, 32.f, 130.f), Metal());
			AddPart(VisualRoot, Cone, FVector(65.f, SY * 210.f, 0.f), AlongX, FVector(32.f, 32.f, 30.f), Metal());
			UStaticMeshComponent* E = G(VisualRoot, Cyl, FVector(-78.f, SY * 210.f, 0.f), AlongX, FVector(28.f, 28.f, 6.f), 12.f);
			ThrusterParts.Add(E);
			AddPart(VisualRoot, Cyl, FVector(0.f, SY * 60.f, -62.f), AlongX, FVector(10.f, 10.f, 220.f), Metal());
			for (const float SX : { -1.f, 1.f })
			{
				AddPart(VisualRoot, Cube, FVector(SX * 60.f, SY * 55.f, -45.f), FRotator::ZeroRotator, FVector(6.f, 6.f, 34.f), Metal());
			}
			AddPart(VisualRoot, Cyl, FVector(55.f, SY * 95.f, -35.f), AlongX, FVector(22.f, 22.f, 70.f), Metal());
			G(VisualRoot, Cube, FVector(-20.f, SY * 150.f, 7.f), FRotator::ZeroRotator, FVector(60.f, 80.f, 2.f), 6.f);
		}
		P(VisualRoot, Cyl, FVector(-230.f, 0.f, 15.f), AlongX, FVector(40.f, 40.f, 200.f), false);
		P(VisualRoot, Cube, FVector(-310.f, 0.f, 62.f), FRotator(0.f, 0.f, 0.f), FVector(60.f, 8.f, 90.f), true);
		P(VisualRoot, Cube, FVector(-310.f, 0.f, 20.f), FRotator::ZeroRotator, FVector(40.f, 150.f, 8.f), true);
		UStaticMeshComponent* Main = G(VisualRoot, Cyl, FVector(-182.f, 0.f, 10.f), AlongX, FVector(60.f, 60.f, 8.f), 12.f);
		ThrusterParts.Add(Main);
		break;
	}
	case EArenaONS_VehicleType::Caravan:
	{
		P(VisualRoot, Cube, FVector(0.f, 0.f, 0.f), FRotator::ZeroRotator, FVector(400.f, 190.f, 60.f), true);
		P(VisualRoot, Cube, FVector(120.f, 0.f, 60.f), FRotator::ZeroRotator, FVector(130.f, 180.f, 66.f), true);
		AddPart(VisualRoot, Cube, FVector(187.f, 0.f, 66.f), FRotator(-20.f, 0.f, 0.f), FVector(8.f, 160.f, 44.f), Glass(), false);
		AddPart(VisualRoot, Cube, FVector(-60.f, 0.f, 36.f), FRotator::ZeroRotator, FVector(250.f, 170.f, 12.f), Metal());
		for (const float SY : { -1.f, 1.f })
		{
			P(VisualRoot, Cube, FVector(-60.f, SY * 90.f, 56.f), FRotator::ZeroRotator, FVector(250.f, 10.f, 30.f), true);
			G(VisualRoot, Cube, FVector(0.f, SY * 96.f, 10.f), FRotator::ZeroRotator, FVector(380.f, 4.f, 6.f), 8.f);
			AddPart(VisualRoot, Sphere, FVector(200.f, SY * 70.f, 10.f), FRotator::ZeroRotator, FVector(18.f), ArenaAssets::MakeGlow(this, FLinearColor(1.f, 0.95f, 0.8f), 12.f), false);
		}
		for (const FVector& Pt : GetSpec().SuspensionPoints)
		{
			Wheel(Pt + FVector(0.f, FMath::Sign(Pt.Y) * 10.f, -GetSpec().RestLength * 0.55f), 96.f, 40.f);
		}
		RearTurretRoot->SetRelativeLocation(FVector(-110.f, 0.f, 42.f));
		AddPart(RearTurretRoot, Cyl, FVector(0.f, 0.f, 10.f), FRotator::ZeroRotator, FVector(110.f, 110.f, 26.f), Metal());
		P(RearTurretRoot, Sphere, FVector(0.f, 0.f, 36.f), FRotator::ZeroRotator, FVector(90.f, 90.f, 60.f), true);
		G(RearTurretRoot, Cyl, FVector(0.f, 0.f, 24.f), FRotator::ZeroRotator, FVector(116.f, 116.f, 4.f), 6.f);
		RearBarrelRoot->SetRelativeLocation(FVector(0.f, 0.f, 46.f));
		for (const float SY : { -1.f, 1.f })
		{
			AddPart(RearBarrelRoot, Cyl, FVector(60.f, SY * 12.f, 0.f), AlongX, FVector(10.f, 10.f, 100.f), Metal());
		}
		AddPart(RearBarrelRoot, Cyl, FVector(55.f, 0.f, 10.f), AlongX, FVector(18.f, 18.f, 90.f), Metal());
		break;
	}
	default:
		break;
	}
	BuiltTeam = Team;
}

void AArenaONS_Vehicle::UpdateTeamLook()
{
	if (!bModelBuilt || BuiltTeam == Team)
	{
		return;
	}
	BuiltTeam = Team;
	if (ArtRig.IsValid())
	{
		ArtRig.SetTeam(Team);
		return;
	}
	const FLinearColor TeamCol = TeamTint(Team);
	for (UStaticMeshComponent* C : TeamGlowParts)
	{
		if (UMaterialInstanceDynamic* MID = C ? Cast<UMaterialInstanceDynamic>(C->GetMaterial(0)) : nullptr)
		{
			MID->SetVectorParameterValue(TEXT("Color"), TeamCol);
		}
	}
	for (UStaticMeshComponent* C : TeamPaintParts)
	{
		if (UMaterialInstanceDynamic* MID = C ? Cast<UMaterialInstanceDynamic>(C->GetMaterial(0)) : nullptr)
		{
			MID->SetVectorParameterValue(TEXT("Color"), FMath::Lerp(FLinearColor(0.42f, 0.43f, 0.45f), TeamCol, 0.4f));
		}
	}
}

void AArenaONS_Vehicle::UpdateVisuals(float DeltaSeconds)
{
	UpdateTeamLook();
	const FArenaONS_VehicleSpec& S = GetSpec();
	const FVector Vel = GetVelocity();
	const FTransform T = GetActorTransform();
	const FVector LocalVel = T.InverseTransformVectorNoScale(Vel);

	// Lean for hover / flyer visuals
	if (S.Mode == EArenaONS_DriveMode::Hover || S.Mode == EArenaONS_DriveMode::Flyer)
	{
		const float Roll = FMath::Clamp(LocalVel.Y / S.MaxSpeed, -1.f, 1.f) * 14.f;
		const float Pitch = FMath::Clamp(-LocalVel.X / S.MaxSpeed, -1.f, 1.f) * (S.Mode == EArenaONS_DriveMode::Flyer ? 10.f : 4.f);
		const FRotator Cur = VisualRoot->GetRelativeRotation();
		VisualRoot->SetRelativeRotation(FMath::RInterpTo(Cur, FRotator(Pitch, 0.f, Roll), DeltaSeconds, 5.f));
		const float Thrust = HasDriver() ? 1.f : 0.35f;
		for (int32 i = 0; i < ThrusterParts.Num(); ++i)
		{
			if (UStaticMeshComponent* C = ThrusterParts[i])
			{
				const float Flicker = 0.85f + 0.15f * FMath::Sin(GetWorld()->GetTimeSeconds() * 30.f + i);
				const FVector Base = C->GetRelativeScale3D();
				C->SetRelativeScale3D(FVector(Base.X, Base.Y, 0.04f + 0.04f * Thrust * Flicker));
			}
		}
	}

	// Wheels: follow the suspension, spin with speed, front wheels steer
	if (WheelRoots.Num() > 0 && S.WheelRadius > 0.f)
	{
		WheelSpin = FMath::Fmod(WheelSpin + LocalVel.X * DeltaSeconds / (S.WheelRadius * 2.f * PI) * 360.f, 360.f);
		SteerVisual = FMath::FInterpTo(SteerVisual, Input.Steer * 28.f, DeltaSeconds, 8.f);
		for (int32 i = 0; i < WheelRoots.Num() && i < S.SuspensionPoints.Num(); ++i)
		{
			USceneComponent* W = WheelRoots[i];
			if (!W)
			{
				continue;
			}
			const FVector Pt = S.SuspensionPoints[i];
			const float Comp = WheelCompression.IsValidIndex(i) ? WheelCompression[i] : S.RestLength;
			W->SetRelativeLocation(FVector(Pt.X, Pt.Y + FMath::Sign(Pt.Y) * 8.f, Pt.Z - Comp));
			W->SetRelativeRotation(FRotator(0.f, Pt.X > 0.f ? SteerVisual : 0.f, 0.f));
			if (USceneComponent* Spin = W->GetNumChildrenComponents() > 0 ? W->GetChildComponent(0) : nullptr)
			{
				Spin->SetRelativeRotation(FRotator(-WheelSpin, 0.f, 0.f));
			}
		}
	}

	// Turret aims: the server owns them, clients follow the replicated values.
	if (HasAuthority())
	{
		RepTurretAim[0] = FRotator::CompressAxisToShort(TurretYaw);
		RepTurretAim[1] = FRotator::CompressAxisToShort(TurretPitch);
		RepTurretAim[2] = FRotator::CompressAxisToShort(RearTurretYaw);
		RepTurretAim[3] = FRotator::CompressAxisToShort(RearTurretPitch);
	}
	else
	{
		auto Follow = [DeltaSeconds](float& Angle, uint16 Packed)
		{
			const float Target = FRotator::NormalizeAxis(FRotator::DecompressAxisFromShort(Packed));
			Angle = FRotator::NormalizeAxis(Angle + FMath::FindDeltaAngleDegrees(Angle, Target) * FMath::Clamp(DeltaSeconds * 15.f, 0.f, 1.f));
		};
		Follow(TurretYaw, RepTurretAim[0]);
		Follow(TurretPitch, RepTurretAim[1]);
		Follow(RearTurretYaw, RepTurretAim[2]);
		Follow(RearTurretPitch, RepTurretAim[3]);
	}

	if (ArtRig.IsValid())
	{
		FArenaVehicleArtPose Pose;
		Pose.DeltaSeconds = DeltaSeconds;
		Pose.Time = GetWorld()->GetTimeSeconds();
		if (S.WheelRadius > 0.f)
		{
			WheelSpin = FMath::Fmod(WheelSpin + LocalVel.X * DeltaSeconds / (S.WheelRadius * 2.f * PI) * 360.f, 360.f);
		}
		SteerVisual = FMath::FInterpTo(SteerVisual, Input.Steer * 28.f, DeltaSeconds, 8.f);
		Pose.WheelSpin = WheelSpin;
		Pose.SteerDegrees = SteerVisual;
		// Wheel centre relative to its reference pose: the art spec puts the reference at rest compression.
		const float RestSpring = S.RestLength * (1.f - S.RestCompression);
		Pose.WheelOffsets.SetNum(S.SuspensionPoints.Num());
		for (int32 i = 0; i < S.SuspensionPoints.Num(); ++i)
		{
			const float Comp = WheelCompression.IsValidIndex(i) ? WheelCompression[i] : RestSpring;
			Pose.WheelOffsets[i] = RestSpring - Comp;
		}
		Pose.ForwardSpeed = LocalVel.X;
		Pose.YawRate = Body ? Body->GetPhysicsAngularVelocityInDegrees().Z : 0.f;
		Pose.TurretYaw = TurretYaw;
		Pose.TurretPitch = TurretPitch;
		Pose.RearTurretYaw = RearTurretYaw;
		Pose.RearTurretPitch = RearTurretPitch;
		Pose.Thrust = HasDriver() || ReplicatedDriver ? FMath::Clamp(0.6f + 0.4f * LocalVel.Size() / FMath::Max(1.f, S.MaxSpeed), 0.f, 1.f) : 0.f;
		ArtRig.UpdatePose(Pose);

		const bool bDriven = HasDriver() || ReplicatedDriver != nullptr;
		ArtRig.UpdateEngine(bDriven, LocalVel.Size() / FMath::Max(1.f, S.MaxSpeed));
		ArtRig.SetDriver(bDriven ? (ReplicatedDriver ? ReplicatedDriver.Get() : DriverChar.Get()) : nullptr);
	}

	// Turrets
	if (TurretRoot)
	{
		TurretRoot->SetRelativeRotation(FRotator(0.f, TurretYaw, 0.f));
	}
	if (BarrelRoot)
	{
		BarrelRoot->SetRelativeRotation(FRotator(TurretPitch, 0.f, 0.f));
	}
	if (RearTurretRoot)
	{
		RearTurretRoot->SetRelativeRotation(FRotator(0.f, RearTurretYaw, 0.f));
	}
	if (RearBarrelRoot)
	{
		RearBarrelRoot->SetRelativeRotation(FRotator(RearTurretPitch, 0.f, 0.f));
	}
}

void AArenaONS_Vehicle::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AArenaONS_Vehicle, VehicleType);
	DOREPLIFETIME(AArenaONS_Vehicle, Team);
	DOREPLIFETIME(AArenaONS_Vehicle, Health);
	DOREPLIFETIME(AArenaONS_Vehicle, Heat);
	DOREPLIFETIME(AArenaONS_Vehicle, bLocked);
	DOREPLIFETIME(AArenaONS_Vehicle, SeatPawn);
	DOREPLIFETIME(AArenaONS_Vehicle, ReplicatedDriver);
	DOREPLIFETIME(AArenaONS_Vehicle, RepTurretAim);
}

// =============================================================================================
// Imported / replacement model
// =============================================================================================

bool AArenaONS_Vehicle::BuildArtModel()
{
	if (!ArtRig.Build(this, VisualRoot, VehicleType, Team))
	{
		return false;
	}
	const ArenaArtVehicles::FVehicleArt& Art = *ArtRig.Art;
	// Turret / barrel roots carry the gameplay aim (muzzle fallbacks, AI eye); put them where the model's pivots are.
	TurretRoot->SetRelativeLocation(Art.TurretPivot);
	BarrelRoot->SetRelativeLocation(Art.BarrelPivot);
	RearTurretRoot->SetRelativeLocation(Art.RearTurretPivot);
	RearBarrelRoot->SetRelativeLocation(Art.RearBarrelPivot);
	static bool bLogged[int32(EArenaONS_VehicleType::Num)] = {};
	if (!bLogged[int32(VehicleType)])
	{
		bLogged[int32(VehicleType)] = true;
		UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: %s uses model %s (%d gun meshes, %d glows)"), *GetVehicleName(), Art.Mesh, ArtRig.Guns.Num(), ArtRig.Glows.Num());
		for (int32 i = 0; i < ArtRig.Chassis->GetNumMaterials(); ++i)
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("ONS:   slot %d material %s"), i, *GetPathNameSafe(ArtRig.Chassis->GetMaterial(i)));
		}
		// Placement check: wheel bones vs. the suspension rest positions, gun attach points vs. the turret roots.
		const FTransform VisT = VisualRoot->GetComponentTransform();
		const FArenaONS_VehicleSpec& S = GetSpec();
		for (const ArenaArtVehicles::FWheel& W : Art.Wheels)
		{
			const FVector B = VisT.InverseTransformPosition(ArtRig.Chassis->GetSocketLocation(W.Bone));
			const FVector P = S.SuspensionPoints.IsValidIndex(W.SuspensionIndex) ? S.SuspensionPoints[W.SuspensionIndex] : FVector::ZeroVector;
			UE_LOG(LogOpenUT2k4, Log, TEXT("ONS:   wheel %s at %s, suspension rest %s"), W.Bone, *B.ToCompactString(),
				*(P - FVector(0.f, 0.f, S.RestLength * (1.f - S.RestCompression))).ToCompactString());
		}
		for (int32 g = 0; g < ArtRig.Guns.Num(); ++g)
		{
			if (UPoseableMeshComponent* GC = ArtRig.Guns[g])
			{
				UE_LOG(LogOpenUT2k4, Log, TEXT("ONS:   gun %s at %s (turret root %s, rear turret root %s)"), *GetNameSafe(GC->GetSkinnedAsset()),
					*VisT.InverseTransformPosition(GC->GetComponentLocation()).ToCompactString(), *TurretRoot->GetRelativeLocation().ToCompactString(),
					*RearTurretRoot->GetRelativeLocation().ToCompactString());
			}
		}
		for (int32 i = 0; i < S.Guns.Num() && i < 4; ++i)
		{
			FVector M;
			if (ArtRig.GetMuzzle(i, 0, M))
			{
				const USceneComponent* BaseC = S.Guns[i].bTurret ? (S.Guns[i].Seat == 1 ? RearBarrelRoot.Get() : BarrelRoot.Get()) : VisualRoot.Get();
				const FVector Spec = S.Guns[i].Muzzles.Num() > 0 ? BaseC->GetComponentTransform().TransformPosition(S.Guns[i].Muzzles[0]) : FVector::ZeroVector;
				UE_LOG(LogOpenUT2k4, Log, TEXT("ONS:   gun %d muzzle %s (spec fallback %s)"), i, *VisT.InverseTransformPosition(M).ToCompactString(),
					*VisT.InverseTransformPosition(Spec).ToCompactString());
			}
		}
	}
	return true;
}

void AArenaONS_Vehicle::TickArtShowcase()
{
	// Test harness: -ONSArtShowcase frames each vehicle type in turn (8 s each: rear quarter, then front quarter),
	// preferring one that is being driven, so -AutoScreenshot can check models, skins, wheels and turrets.
	if (!ArenaONS::HasDebugFlag(TEXT("ONSArtShowcase")))
	{
		return;
	}
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return;
	}
	static TWeakObjectPtr<ACameraActor> Cam;
	static TWeakObjectPtr<AArenaONS_Vehicle> Shown;
	static float StartTime = -1.f;
	const float Now = World->GetTimeSeconds();
	if (StartTime < 0.f)
	{
		StartTime = Now;
	}
	const int32 Slot = int32((Now - StartTime) / 8.f);
	const EArenaONS_VehicleType Wanted = EArenaONS_VehicleType(Slot % int32(EArenaONS_VehicleType::Num));
	if (VehicleType != Wanted)
	{
		return;
	}
	// Alternate teams every full cycle; prefer a driven vehicle.
	const int32 WantedTeam = (Slot / int32(EArenaONS_VehicleType::Num)) % 2;
	auto Score = [WantedTeam](const AArenaONS_Vehicle* V) { return (V->GetTeam() == WantedTeam ? 2 : 0) + (V->HasDriver() ? 1 : 0); };
	AArenaONS_Vehicle* Cur = Shown.Get();
	const bool bCurOk = Cur && Cur->GetVehicleType() == Wanted && !Cur->IsDestroyedVehicle();
	if (!bCurOk || Score(this) > Score(Cur))
	{
		Shown = this;
		UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: showcase %s (team %d, %s, art %d) at %s"), *GetVehicleName(), Team, HasDriver() ? TEXT("driven") : TEXT("parked"),
			UsesArtModel() ? 1 : 0, *GetActorLocation().ToCompactString());
	}
	if (Shown.Get() != this)
	{
		return;
	}
	if (!Cam.IsValid())
	{
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Cam = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), FTransform::Identity, P);
	}
	if (ACameraActor* C = Cam.Get())
	{
		const FArenaONS_VehicleSpec& S = GetSpec();
		const bool bFront = FMath::Fmod(Now - StartTime, 8.f) >= 4.f;
		// Eye = focus - view direction * distance: view yaw = hull yaw looks from behind.
		const float Dist = S.HalfExtent.Size() * 2.6f + 250.f;
		const float Yaw = GetActorRotation().Yaw + (bFront ? 215.f : 35.f);
		const FVector Focus = GetActorLocation();
		const FVector Eye = Focus + FRotator(-18.f, Yaw, 0.f).Vector() * -Dist;
		C->SetActorLocationAndRotation(Eye, (Focus - Eye).Rotation());
		if (PC->GetViewTarget() != C)
		{
			PC->SetViewTarget(C);
		}
	}
}

void AArenaONS_Vehicle::PlayArtSound(const TCHAR* Path, const FVector& Location, float Volume)
{
	if (USoundBase* Snd = ArenaArtVehicles::LoadSound(Path))
	{
		UGameplayStatics::PlaySoundAtLocation(this, Snd, Location, Volume, 1.f, 0.f, ArenaArtVehicles::GetAttenuation());
	}
}

// =============================================================================================
// AArenaONS_VehicleWreck
// =============================================================================================

AArenaONS_VehicleWreck::AArenaONS_VehicleWreck()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	SetReplicateMovement(true);
	InitialLifeSpan = 14.f;

	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	Box->SetBoxExtent(FVector(150.f, 85.f, 30.f));
	Box->SetCollisionObjectType(ECC_PhysicsBody);
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Box->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	Box->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	Box->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
	Box->SetCanEverAffectNavigation(false);
	Box->SetHiddenInGame(true);
	RootComponent = Box;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Box);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCanEverAffectNavigation(false);
}

AArenaONS_VehicleWreck* AArenaONS_VehicleWreck::SpawnFor(AArenaONS_Vehicle* Vehicle)
{
	UWorld* World = Vehicle ? Vehicle->GetWorld() : nullptr;
	const ArenaArtVehicles::FVehicleArt* Art = Vehicle ? ArenaArtVehicles::Find(Vehicle->GetVehicleType()) : nullptr;
	if (!World || !Vehicle->HasAuthority() || !Art || !Art->Wreck || !ArenaArt::Load<UStaticMesh>(Art->Wreck))
	{
		return nullptr;
	}
	const FTransform T = Vehicle->GetActorTransform();
	AArenaONS_VehicleWreck* W = World->SpawnActorDeferred<AArenaONS_VehicleWreck>(AArenaONS_VehicleWreck::StaticClass(), T, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!W)
	{
		return nullptr;
	}
	W->VehicleType = Vehicle->GetVehicleType();
	W->LaunchVelocity = Vehicle->GetVelocity() * 0.5f + FVector(FMath::FRandRange(-150.f, 150.f), FMath::FRandRange(-150.f, 150.f), 450.f);
	W->FinishSpawning(T);
	return W;
}

void AArenaONS_VehicleWreck::BeginPlay()
{
	Super::BeginPlay();
	BuildWreck();
	if (HasAuthority())
	{
		Box->SetSimulatePhysics(true);
		Box->SetMassOverrideInKg(NAME_None, 2000.f, true);
		Box->SetPhysicsLinearVelocity(LaunchVelocity);
		Box->SetPhysicsAngularVelocityInDegrees(FVector(FMath::FRandRange(-60.f, 60.f), FMath::FRandRange(-40.f, 40.f), FMath::FRandRange(-90.f, 90.f)));
	}
}

void AArenaONS_VehicleWreck::BuildWreck()
{
	const ArenaArtVehicles::FVehicleArt* Art = ArenaArtVehicles::Find(VehicleType);
	UStaticMesh* WreckMesh = (Art && Art->Wreck) ? ArenaArt::Load<UStaticMesh>(Art->Wreck) : nullptr;
	if (!Art || !WreckMesh)
	{
		return;
	}
	Box->SetBoxExtent(Art->HalfExtent);
	Mesh->SetStaticMesh(WreckMesh);
	Mesh->SetRelativeLocation(Art->MeshOffset);
}

void AArenaONS_VehicleWreck::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AArenaONS_VehicleWreck, VehicleType);
}

// =============================================================================================
// AArenaONS_VehicleSeat
// =============================================================================================

AArenaONS_VehicleSeat::AArenaONS_VehicleSeat()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	SetCanBeDamaged(false);
	AutoPossessAI = EAutoPossessAI::Disabled;
	AutoPossessPlayer = EAutoReceiveInput::Disabled;
	AIControllerClass = nullptr;
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;
	SetActorEnableCollision(false);

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(Root);
	SpringArm->TargetArmLength = 420.f;
	SpringArm->SocketOffset = FVector(0.f, 0.f, 120.f);
	SpringArm->bUsePawnControlRotation = true;
	SpringArm->bInheritRoll = false;
	SpringArm->bDoCollisionTest = true;
	SpringArm->bEnableCameraLag = true;
	SpringArm->CameraLagSpeed = 15.f;
	SpringArm->SetUsingAbsoluteRotation(true);

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm);
	Camera->SetFieldOfView(95.f);
}

void AArenaONS_VehicleSeat::InitSeat(AArenaONS_Vehicle* InVehicle, int32 InSeatIndex)
{
	Vehicle = InVehicle;
	SeatIndex = InSeatIndex;
	if (Vehicle && Vehicle->RearTurretRoot)
	{
		AttachToComponent(Vehicle->RearTurretRoot, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		const ArenaArtVehicles::FVehicleArt* Art = Vehicle->UsesArtModel() ? ArenaArtVehicles::Find(Vehicle->GetVehicleType()) : nullptr;
		SetActorRelativeLocation(Art ? Art->RearSeatOffset : FVector(0.f, 0.f, 40.f));
	}
}

FVector AArenaONS_VehicleSeat::GetPawnViewLocation() const
{
	return GetActorLocation() + FVector(0.f, 0.f, 60.f);
}

FRotator AArenaONS_VehicleSeat::GetViewRotation() const
{
	if (Controller && Controller->IsA<APlayerController>())
	{
		return Controller->GetControlRotation();
	}
	return AIAim;
}

void AArenaONS_VehicleSeat::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority() || !Vehicle || Vehicle->IsDestroyedVehicle())
	{
		return;
	}
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();
	AController* C = GetController();
	if (Rider.IsValid() && !C)
	{
		Rider->Destroy();
		Rider.Reset();
		return;
	}
	if (!C)
	{
		return;
	}

	FRotator Aim;
	FVector AimPoint;
	bool bFire = false;
	bool bAlt = false;
	bool bUse = false;
	FArenaONS_VehicleInput RemoteInput;
	if (!Cast<APlayerController>(C) && GArenaONSExternalInput && GArenaONSExternalInput(this, C, RemoteInput))
	{
		// a remote gunner (hosting for UT2004 clients)
		ConsumeMovementInputVector();
		Aim = RemoteInput.AimRotation;
		AimPoint = Vehicle->TraceAimPoint(Camera ? Camera->GetComponentLocation() : GetPawnViewLocation(), Aim);
		bFire = RemoteInput.bFire;
		bAlt = RemoteInput.bAltFire;
		bUse = RemoteInput.bUse;
	}
	else if (APlayerController* PC = Cast<APlayerController>(C))
	{
		ConsumeMovementInputVector();
		const AArenaPlayerController* APC = Cast<AArenaPlayerController>(PC);
		const bool bBlocked = APC && APC->IsPauseMenuOpen();
		Aim = PC->GetControlRotation();
		AimPoint = Vehicle->TraceAimPoint(Camera ? Camera->GetComponentLocation() : GetPawnViewLocation(), Aim);
		if (!bBlocked)
		{
			bFire = PC->IsInputKeyDown(EKeys::LeftMouseButton) || PC->IsInputKeyDown(EKeys::Gamepad_RightTrigger);
			bAlt = PC->IsInputKeyDown(EKeys::RightMouseButton) || PC->IsInputKeyDown(EKeys::Gamepad_LeftTrigger);
			bUse = PC->WasInputKeyJustPressed(EKeys::E) || PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Left);
		}
	}
	else
	{
		// Bot gunner: shoot whatever the vehicle AI would shoot.
		if (Now >= AINextTargetTime)
		{
			AINextTargetTime = Now + 0.3f;
			AActor* T = nullptr;
			FVector P;
			if (Vehicle->FindAITarget(T, P))
			{
				AITarget = T;
				AITargetAim = P;
			}
			else
			{
				AITarget.Reset();
			}
		}
		const FVector Eye = Vehicle->GetTurretPivot(true);
		const FVector Target = AITarget.IsValid() ? AITargetAim : Eye + Vehicle->GetActorForwardVector() * -2000.f;
		AIAim = FMath::RInterpTo(AIAim, (Target - Eye).Rotation(), DeltaSeconds, 6.f);
		Aim = AIAim;
		AimPoint = Target;
		C->SetControlRotation(AIAim);
		bFire = AITarget.IsValid();
		bAlt = AITarget.IsValid() && FVector::Dist(Eye, Target) > 1200.f && FMath::FRand() < 0.05f;
		// Leave with the driver
		bUse = !Vehicle->HasDriver() && Now - EnterTime > 2.f;
	}

	Vehicle->UpdateTurretAim(Aim, DeltaSeconds, true);
	const FArenaONS_VehicleSpec& S = Vehicle->GetSpec();
	for (int32 i = 0; i < S.Guns.Num(); ++i)
	{
		const FArenaONS_GunSpec& G = S.Guns[i];
		if (G.Seat == SeatIndex && ((G.FireMode == 0 && bFire) || (G.FireMode == 1 && bAlt)))
		{
			Vehicle->TryFireGun(i, C, AimPoint);
		}
	}
	if (bUse && Now - EnterTime > 0.6f)
	{
		Vehicle->LeaveSeat(SeatIndex, false);
	}
	if (AArenaPlayerController* LPC = Cast<AArenaPlayerController>(C))
	{
		if (LPC->IsLocalController() && Camera)
		{
			Camera->SetFieldOfView(LPC->GetDesiredFOV());
		}
	}
}

void AArenaONS_VehicleSeat::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AArenaONS_VehicleSeat, Vehicle);
}
