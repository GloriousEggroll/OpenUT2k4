#include "ArenaWorldZones.h"
#include "OpenUT2k4.h"
#include "ArenaCharacter.h"
#include "ArenaCharacterMovement.h"
#include "ArenaArtVehicles.h"
#include "ArenaDamage.h"
#include "ArenaMover.h"
#include "ArenaProjectile.h"
#include "ArenaRuntimeContent.h"
#include "Camera/CameraActor.h"
#include "Camera/PlayerCameraManager.h"
#include "Misc/CommandLine.h"
#include "Components/AudioComponent.h"
#include "Components/BrushComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/DamageType.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "PhysicsEngine/PhysicsSettings.h"

namespace ArenaWorldZonesLocal
{
	constexpr float UnderWaterTime = 20.f;   // UE2 Pawn.UnderWaterTime
	constexpr float DrownDamage = 5.f;       // UE2 Pawn.TakeDrowningDamage, every BreathTime
	constexpr float BreathTime = 2.f;

	USoundBase* LoadSound(const FString& Path)
	{
		return Path.IsEmpty() ? nullptr : ArenaRuntimeContent::LoadSoundByObjectPath(Path);
	}

	UMovementComponent* MovementOf(AActor* A)
	{
		if (ACharacter* C = Cast<ACharacter>(A))
		{
			return C->GetCharacterMovement();
		}
		return A ? A->FindComponentByClass<UProjectileMovementComponent>() : nullptr;
	}

	/** Per actor: its last water splash (UE2 Pawn.SplashTime, and the sound in its SLOT_Interact). */
	struct FSplash
	{
		double Time = -1.0;
		TWeakObjectPtr<UAudioComponent> Audio;
	};
	TMap<TWeakObjectPtr<AActor>, FSplash> Splashes;

	/**
	 * UE2 PhysicsVolume.PlayEntrySplash / PlayExitSplash: Other.PlaySound(Sound, SLOT_Interact, Other.TransientSoundVolume)
	 * if Other.CanSplash(). Pawns splash only when moving up or down faster than 100 UU/s, at most every 0.15 s
	 * (Pawn.CanSplash; it also wants PHYS_Falling, but our movement is already swimming when the entry is reported):
	 * a pawn swimming at the surface, or moved there by a correction, crosses it again and again, and every crossing
	 * started another splash on top of the others. Projectiles always splash (bReadyToSplash). The sound plays at
	 * Actor.TransientSoundVolume 0.3 with a distance falloff (not at full volume map wide), and replaces the actor's
	 * last splash still playing (one sound per slot).
	 */
	void PlaySplash(AActor* Other, USoundBase* Sound)
	{
		UWorld* World = Other ? Other->GetWorld() : nullptr;
		if (!World || !Sound)
		{
			return;
		}
		if (Splashes.Num() > 64)
		{
			for (auto It = Splashes.CreateIterator(); It; ++It)
			{
				if (!It->Key.IsValid())
				{
					It.RemoveCurrent();
				}
			}
		}
		FSplash& Last = Splashes.FindOrAdd(Other);
		const double Now = World->GetTimeSeconds();
		if (Cast<APawn>(Other))
		{
			const ACharacter* C = Cast<ACharacter>(Other);
			const UCharacterMovementComponent* CM = C ? C->GetCharacterMovement() : nullptr;
			if ((CM && CM->IsMovingOnGround()) || FMath::Abs(Other->GetVelocity().Z) <= 100.f * ArenaUE2World::Scale
				|| (Last.Time >= 0.0 && Now >= Last.Time && Now - Last.Time <= 0.15))
			{
				return;
			}
		}
		Last.Time = Now;
		if (UAudioComponent* Playing = Last.Audio.Get())
		{
			Playing->Stop();
		}
		Last.Audio = UGameplayStatics::SpawnSoundAtLocation(Other, Sound, Other->GetActorLocation(), FRotator::ZeroRotator, 0.3f, 1.f, 0.f,
			ArenaArtVehicles::GetAttenuation());
	}
}

// ---------------------------------------------------------------------------------------------
// FArenaBrushShape
// ---------------------------------------------------------------------------------------------

bool FArenaBrushShape::Load(const TSharedPtr<FJsonObject>& Marker)
{
	Planes.Reset();
	Back.Reset();
	Front.Reset();
	Faces.Reset();
	Bounds = FBox(ForceInit);
	if (!Marker.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Brush = nullptr;
	if (Marker->TryGetObjectField(TEXT("brush"), Brush))
	{
		const TArray<TSharedPtr<FJsonValue>>* F = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* P = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* B = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Fr = nullptr;
		if ((*Brush)->TryGetArrayField(TEXT("faces"), F) && (*Brush)->TryGetArrayField(TEXT("planes"), P) && (*Brush)->TryGetArrayField(TEXT("back"), B)
			&& (*Brush)->TryGetArrayField(TEXT("front"), Fr) && F->Num() == P->Num() && P->Num() == B->Num() && B->Num() == Fr->Num())
		{
			for (int32 i = 0; i < P->Num(); ++i)
			{
				const TArray<TSharedPtr<FJsonValue>>& Pl = (*P)[i]->AsArray();
				if (Pl.Num() < 4)
				{
					return false;
				}
				Planes.Add(FPlane(FVector(Pl[0]->AsNumber(), Pl[1]->AsNumber(), Pl[2]->AsNumber()), Pl[3]->AsNumber()));
				Back.Add(int32((*B)[i]->AsNumber()));
				Front.Add(int32((*Fr)[i]->AsNumber()));
				TArray<FVector>& Face = Faces.AddDefaulted_GetRef();
				const TArray<TSharedPtr<FJsonValue>>& Pts = (*F)[i]->AsArray();
				for (int32 k = 0; k + 2 < Pts.Num(); k += 3)
				{
					const FVector V(Pts[k]->AsNumber(), Pts[k + 1]->AsNumber(), Pts[k + 2]->AsNumber());
					Face.Add(V);
					Bounds += V;
				}
			}
			(*Brush)->TryGetBoolField(TEXT("convex"), bConvex);
			return Bounds.IsValid != 0;
		}
	}
	// old caches: the brush bounds as a box
	const TSharedPtr<FJsonObject>* Bw = nullptr;
	if (!Marker->TryGetObjectField(TEXT("bounds_world"), Bw))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Mn = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Mx = nullptr;
	if (!(*Bw)->TryGetArrayField(TEXT("min"), Mn) || !(*Bw)->TryGetArrayField(TEXT("max"), Mx) || Mn->Num() < 3 || Mx->Num() < 3)
	{
		return false;
	}
	const FVector A((*Mn)[0]->AsNumber(), (*Mn)[1]->AsNumber(), (*Mn)[2]->AsNumber());
	const FVector C((*Mx)[0]->AsNumber(), (*Mx)[1]->AsNumber(), (*Mx)[2]->AsNumber());
	Bounds = FBox(A, C);
	const FVector N[6] = { FVector(1, 0, 0), FVector(-1, 0, 0), FVector(0, 1, 0), FVector(0, -1, 0), FVector(0, 0, 1), FVector(0, 0, -1) };
	for (int32 i = 0; i < 6; ++i)
	{
		const FVector P = (N[i].X + N[i].Y + N[i].Z) > 0 ? C : A;
		Planes.Add(FPlane(N[i], FVector::DotProduct(N[i], P)));
		Back.Add(i + 1 < 6 ? i + 1 : -1);
		Front.Add(-1);
		// face: the four box corners on this side
		TArray<FVector>& Face = Faces.AddDefaulted_GetRef();
		const int32 Ax = N[i].X != 0 ? 0 : N[i].Y != 0 ? 1 : 2;
		const int32 U = (Ax + 1) % 3, V = (Ax + 2) % 3;
		const double Fixed = P[Ax];
		const double Us[4] = { A[U], C[U], C[U], A[U] };
		const double Vs[4] = { A[V], A[V], C[V], C[V] };
		for (int32 k = 0; k < 4; ++k)
		{
			FVector Pt;
			Pt[Ax] = Fixed;
			Pt[U] = Us[k];
			Pt[V] = Vs[k];
			Face.Add(Pt);
		}
	}
	bConvex = true;
	return true;
}

bool FArenaBrushShape::Encompasses(const FVector& Point, float Radius) const
{
	if (!IsValid() || !Bounds.ExpandBy(Radius + 1.f).IsInside(Point))
	{
		return false;
	}
	int32 i = 0;
	int32 Guard = 0;
	while (Planes.IsValidIndex(i) && Guard++ < 4096)
	{
		const double D = Planes[i].PlaneDot(Point);
		if (D > Radius)
		{
			if (Front[i] < 0)
			{
				return false;
			}
			i = Front[i];
		}
		else
		{
			if (Back[i] < 0)
			{
				return true;
			}
			i = Back[i];
		}
	}
	return false;
}

UStaticMesh* FArenaBrushShape::BuildMesh(const FString& Name) const
{
	TArray<ArenaRuntimeContent::FMeshSection> Sections;
	ArenaRuntimeContent::FMeshSection& S = Sections.AddDefaulted_GetRef();
	S.Material = TEXT("None");
	S.bCollides = true;
	for (int32 f = 0; f < Faces.Num(); ++f)
	{
		const TArray<FVector>& Face = Faces[f];
		if (Face.Num() < 3)
		{
			continue;
		}
		const FVector3f N = FVector3f(Planes.IsValidIndex(f) ? FVector(Planes[f]) : FVector::UpVector);
		const uint32 Base = uint32(S.Positions.Num());
		for (const FVector& P : Face)
		{
			S.Positions.Add(FVector3f(P));
			S.Normals.Add(N);
			S.UV0.Add(FVector2f::ZeroVector);
		}
		for (int32 k = 1; k + 1 < Face.Num(); ++k)
		{
			S.Indices.Add(Base);
			S.Indices.Add(Base + k + 1);
			S.Indices.Add(Base + k);
		}
	}
	if (S.Indices.Num() == 0)
	{
		return nullptr;
	}
	return ArenaRuntimeContent::BuildStaticMesh(Name, Sections);
}

// ---------------------------------------------------------------------------------------------
// Zones
// ---------------------------------------------------------------------------------------------

AArenaZoneInfo::AArenaZoneInfo()
{
	PrimaryActorTick.bCanEverTick = false;
}

void AArenaZoneInfo::InitFromUE2(const FArenaUE2ActorDesc& Desc)
{
	const FArenaUE2Props& P = Desc.Props;
	UE2Name = Desc.Name;
	Tag = Desc.Tag;
	Event = Desc.Event;
	ZoneTag = P.Name(TEXT("ZoneTag"));
	if (P.Has(TEXT("KillZ")))
	{
		KillZ = float(P.Num(TEXT("KillZ"), -10000.0) * ArenaUE2World::Scale);
	}
	bSoftKillZ = P.Bool(TEXT("bSoftKillZ"));
	bTerrainZone = P.Bool(TEXT("bTerrainZone"));
	bDistanceFog = P.Bool(TEXT("bDistanceFog"));
	bClearToFogColor = P.Bool(TEXT("bClearToFogColor"));
	DistanceFogColor = P.Color(TEXT("DistanceFogColor"), DistanceFogColor);
	DistanceFogStart = float(P.Num(TEXT("DistanceFogStart"), 3000.0) * ArenaUE2World::Scale);
	DistanceFogEnd = float(P.Num(TEXT("DistanceFogEnd"), 8000.0) * ArenaUE2World::Scale);
	LocationName = P.Str(TEXT("LocationName"));
	AmbientBrightness = uint8(P.Num(TEXT("AmbientBrightness"), 0));
	AmbientHue = uint8(P.Num(TEXT("AmbientHue"), 0));
	AmbientSaturation = uint8(P.Num(TEXT("AmbientSaturation"), 255));
	if (UArenaWorldZoneSubsystem* Zones = UArenaWorldZoneSubsystem::Get(this))
	{
		Zones->RegisterZone(this);
	}
}

// ---------------------------------------------------------------------------------------------
// Physics volumes
// ---------------------------------------------------------------------------------------------

AArenaPhysicsVolume::AArenaPhysicsVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// membership is assigned by UArenaWorldZoneSubsystem (UE2 point rules), no overlap queries
	GetBrushComponent()->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	GetBrushComponent()->SetGenerateOverlapEvents(false);
	Priority = 0;
	TerminalVelocity = 2500.f * ArenaUE2World::Scale;
	FluidFriction = 0.3f;
	SetHidden(true);
}

void AArenaPhysicsVolume::InitFromUE2(const FArenaUE2ActorDesc& Desc)
{
	const FArenaUE2Props& P = Desc.Props;
	constexpr float S = ArenaUE2World::Scale;
	UE2Name = Desc.Name;
	Tag = Desc.Tag;
	Event = Desc.Event;
	Gravity = P.Vec(TEXT("Gravity"), Gravity);
	ZoneVelocity = P.Vec(TEXT("ZoneVelocity"), FVector::ZeroVector) * S;
	GroundFriction = float(P.Num(TEXT("GroundFriction"), GroundFriction));
	TerminalVelocity = float(P.Num(TEXT("TerminalVelocity"), TerminalVelocity / S) * S);
	FluidFriction = float(P.Num(TEXT("FluidFriction"), FluidFriction));
	Priority = int32(P.Num(TEXT("Priority"), Priority));
	bWaterVolume = P.Bool(TEXT("bWaterVolume"), bWaterVolume);
	bPainCausing = P.Bool(TEXT("bPainCausing"), bPainCausing);
	DamagePerSec = float(P.Num(TEXT("DamagePerSec"), DamagePerSec));
	DamageType = P.Str(TEXT("DamageType"), DamageType);
	bDestructive = P.Bool(TEXT("bDestructive"), bDestructive);
	bNoInventory = P.Bool(TEXT("bNoInventory"), bNoInventory);
	bMoveProjectiles = P.Bool(TEXT("bMoveProjectiles"), bMoveProjectiles);
	bBounceVelocity = P.Bool(TEXT("bBounceVelocity"), bBounceVelocity);
	bNeutralZone = P.Bool(TEXT("bNeutralZone"), bNeutralZone);
	bDamagesVehicles = P.Bool(TEXT("bDamagesVehicles"), bDamagesVehicles);
	LocationName = P.Str(TEXT("LocationName"), LocationName);
	if (LocationName == TEXT("unspecified"))
	{
		LocationName.Reset();
	}
	LocationPriority = int32(P.Num(TEXT("LocationPriority"), LocationPriority));
	const FString Entry = P.Str(TEXT("EntrySound"), P.Str(TEXT("EntrySoundName")));
	const FString Exit = P.Str(TEXT("ExitSound"), P.Str(TEXT("ExitSoundName")));
	if (USoundBase* Snd = ArenaWorldZonesLocal::LoadSound(Entry))
	{
		EntrySound = Snd;
	}
	if (USoundBase* Snd = ArenaWorldZonesLocal::LoadSound(Exit))
	{
		ExitSound = Snd;
	}
	ViewFog = P.Vec(TEXT("ViewFog"), ViewFog);
	ViewFlash = P.Vec(TEXT("ViewFlash"), ViewFlash);
	bDistanceFog = P.Bool(TEXT("bDistanceFog"), bDistanceFog);
	DistanceFogColor = P.Color(TEXT("DistanceFogColor"), DistanceFogColor);
	DistanceFogStart = float(P.Num(TEXT("DistanceFogStart"), DistanceFogStart / S) * S);
	DistanceFogEnd = float(P.Num(TEXT("DistanceFogEnd"), DistanceFogEnd / S) * S);
	KExtraLinearDamping = float(P.Num(TEXT("KExtraLinearDamping"), KExtraLinearDamping));
	KExtraAngularDamping = float(P.Num(TEXT("KExtraAngularDamping"), KExtraAngularDamping));
	KBuoyancy = float(P.Num(TEXT("KBuoyancy"), KBuoyancy));
	// kill volumes (no damage over time: instant death on entry)
	for (const FString& C : Desc.ClassChain)
	{
		if (C.Contains(TEXT("Kill"), ESearchCase::IgnoreCase))
		{
			bKillVolume = true;
		}
	}
	Shape.Load(Desc.Marker);
	if (!Shape.IsValid())
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("UE2World: %s has no brush, ignored"), *Desc.Name);
	}
}

void AArenaPhysicsVolume::BeginPlay()
{
	Super::BeginPlay();
	if (UArenaWorldZoneSubsystem* Zones = UArenaWorldZoneSubsystem::Get(this))
	{
		if (Shape.IsValid())
		{
			Zones->RegisterVolume(this);
		}
	}
}

void AArenaPhysicsVolume::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UArenaWorldZoneSubsystem* Zones = UArenaWorldZoneSubsystem::Get(this))
	{
		Zones->UnregisterVolume(this);
	}
	Super::EndPlay(EndPlayReason);
}

float AArenaPhysicsVolume::GetGravityZ() const
{
	return UPhysicsSettings::Get()->DefaultGravityZ * (Gravity.Z / ArenaUE2World::UE2DefaultGravity);
}

FVector AArenaPhysicsVolume::GetGravityVector() const
{
	return Gravity * (UPhysicsSettings::Get()->DefaultGravityZ / ArenaUE2World::UE2DefaultGravity);
}

FVector AArenaPhysicsVolume::ClampToTerminalVelocity(const FVector& Velocity) const
{
	const float Limit = FMath::Abs(TerminalVelocity);
	FVector Out = Velocity;
	if (Out.Z < -Limit)
	{
		Out.Z = -Limit;
	}
	return Out;
}

void AArenaPhysicsVolume::ActorEnteredVolume(AActor* Other)
{
	Super::ActorEnteredVolume(Other);
	if (!Other || !GetWorld() || !GetWorld()->HasBegunPlay())
	{
		return;
	}
	APawn* Pawn = Cast<APawn>(Other);
	if (EntrySound && (Pawn || Other->IsA<AArenaProjectile>()))
	{
		ArenaWorldZonesLocal::PlaySplash(Other, EntrySound);
	}
	if (bDestructive && Other->IsA<AArenaProjectile>())
	{
		Other->Destroy();
		return;
	}
	// UE2 PhysicsVolume.Touch: bMoveProjectiles adds ZoneVelocity once to projectiles entering
	if (bMoveProjectiles && !ZoneVelocity.IsNearlyZero() && !Pawn)
	{
		if (UProjectileMovementComponent* PM = Other->FindComponentByClass<UProjectileMovementComponent>())
		{
			PM->Velocity += ZoneVelocity;
		}
	}
	if (Pawn)
	{
		// UE2 PawnEnteredVolume: player pawns fire Event
		if (Pawn->IsPlayerControlled() && !Event.IsNone())
		{
			if (UArenaEventSubsystem* Events = UArenaEventSubsystem::Get(this))
			{
				Events->TriggerEvent(Event, this, Pawn);
			}
		}
		if (bKillVolume)
		{
			Pawn->FellOutOfWorld(*GetDefault<UDamageType>());
		}
	}
}

void AArenaPhysicsVolume::ActorLeavingVolume(AActor* Other)
{
	Super::ActorLeavingVolume(Other);
	if (Other && ExitSound && Cast<APawn>(Other) && GetWorld() && GetWorld()->HasBegunPlay())
	{
		ArenaWorldZonesLocal::PlaySplash(Other, ExitSound);
	}
}

FString AArenaPhysicsVolume::GetDamageName() const
{
	FString Name = DamageType;
	int32 Dot;
	if (Name.FindLastChar(TEXT('.'), Dot))
	{
		Name = Name.Mid(Dot + 1);
	}
	if (Name.Contains(TEXT("Lava")))
	{
		return TEXT("Lava");
	}
	if (Name.Equals(TEXT("fell"), ESearchCase::IgnoreCase) || Name.IsEmpty())
	{
		return TEXT("Fell");
	}
	if (Name.Contains(TEXT("Depressur")))
	{
		return TEXT("Pressure");
	}
	return Name;
}

void AArenaPhysicsVolume::CausePainTo(AActor* Other)
{
	if (!Other || DamagePerSec == 0.f)
	{
		return;
	}
	if (DamagePerSec > 0.f)
	{
		if (!bDamagesVehicles && !Other->IsA<ACharacter>())
		{
			return;
		}
		FArenaHitInfo Info;
		Info.WeaponName = GetDamageName();
		ArenaDamage::ApplyDamage(Other, DamagePerSec, nullptr, this, Info);
	}
	else if (AArenaCharacter* C = Cast<AArenaCharacter>(Other))
	{
		C->GiveHealth(-DamagePerSec, ArenaTuning::StartHealth);
	}
}

void AArenaPhysicsVolume::Trigger(AActor* Other, APawn* EventInstigator)
{
	if (DamagePerSec != 0.f)
	{
		bPainCausing = !bPainCausing;
	}
}

AArenaWaterVolume::AArenaWaterVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bWaterVolume = true;
	FluidFriction = 2.4f;
	bDistanceFog = true;
	DistanceFogColor = FColor(32, 64, 128, 64);
	DistanceFogStart = 8.f * ArenaUE2World::Scale;
	DistanceFogEnd = 2000.f * ArenaUE2World::Scale;
	KExtraLinearDamping = 2.5f;
	KExtraAngularDamping = 0.4f;
	LocationName = TEXT("under water");
}

AArenaLavaVolume::AArenaLavaVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DamagePerSec = 40.f;
	DamageType = TEXT("Engine.FellLava");
	FluidFriction = 4.f;
	ViewFog = FVector(0.5859375f, 0.1953125f, 0.078125f);
	bPainCausing = true;
	bDestructive = true;
	bNoInventory = true;
	KExtraLinearDamping = 0.8f;
	KExtraAngularDamping = 0.1f;
	LocationName = TEXT("in lava");
}

AArenaPressureVolume::AArenaPressureVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DamageType = TEXT("GamePlay.Depressurized");
}

AArenaFallingVolume::AArenaFallingVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DamagePerSec = 100.f;
	DamageType = TEXT("Engine.fell");
	ViewFog = FVector(0.5859375f, 0.1953125f, 0.078125f);
	bPainCausing = true;
	bDestructive = true;
	bNoInventory = true;
	LocationName = TEXT("in air");
}

// ---------------------------------------------------------------------------------------------
// Blocking volumes
// ---------------------------------------------------------------------------------------------

AArenaBlockingVolume::AArenaBlockingVolume()
{
	PrimaryActorTick.bCanEverTick = false;
	Collision = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Collision"));
	RootComponent = Collision;
	Collision->SetMobility(EComponentMobility::Static);
	// the collision mesh is built in world space
	Collision->SetUsingAbsoluteLocation(true);
	Collision->SetUsingAbsoluteRotation(true);
	Collision->SetUsingAbsoluteScale(true);
	Collision->SetVisibility(false);
	Collision->SetHiddenInGame(true);
	Collision->SetCastShadow(false);
	Collision->SetGenerateOverlapEvents(false);
	Collision->SetCanEverAffectNavigation(false);
	Collision->bAffectDistanceFieldLighting = false;
	Collision->bVisibleInRayTracing = false;
	SetHidden(true);
}

bool AArenaBlockingVolume::BlocksOnlyVehicles() const
{
	if (!bClassBlocker)
	{
		return false;
	}
	for (const FString& C : BlockedClasses)
	{
		if (!C.Contains(TEXT("Vehicle")) && !C.Contains(TEXT("ONS")) && !C.Contains(TEXT("SVehicle")))
		{
			return false;
		}
	}
	return BlockedClasses.Num() > 0;
}

void AArenaBlockingVolume::InitFromUE2(const FArenaUE2ActorDesc& Desc)
{
	const FArenaUE2Props& P = Desc.Props;
	UE2Name = Desc.Name;
	Tag = Desc.Tag;
	bClassBlocker = P.Bool(TEXT("bClassBlocker"));
	for (int32 i = 0; i < 16; ++i)
	{
		const FString C = FArenaUE2Props::ToString(P.Indexed(TEXT("BlockedClasses"), i));
		if (!C.IsEmpty())
		{
			BlockedClasses.Add(C);
		}
	}
	for (const TSharedPtr<FJsonValue>& V : P.Array(TEXT("BlockedClasses")))
	{
		const FString C = FArenaUE2Props::ToString(V);
		if (!C.IsEmpty())
		{
			BlockedClasses.AddUnique(C);
		}
	}
	bBlockZeroExtentTraces = P.Bool(TEXT("bBlockZeroExtentTraces"), false);
	bBlockNonZeroExtentTraces = P.Bool(TEXT("bBlockNonZeroExtentTraces"), true);
	bBlockActors = P.Bool(TEXT("bBlockActors"), true) && P.Bool(TEXT("bCollideActors"), true);
	bBlockKarma = P.Bool(TEXT("bBlockKarma"), true);
	Shape.Load(Desc.Marker);
	if (Shape.IsValid() && (bBlockActors || bBlockKarma))
	{
		if (UStaticMesh* Mesh = Shape.BuildMesh(TEXT("UE2_") + Desc.Name))
		{
			Collision->SetStaticMesh(Mesh);
		}
	}
	ApplyCollision();
}

void AArenaBlockingVolume::ApplyCollision()
{
	if (!Collision->GetStaticMesh())
	{
		Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}
	// The mesh is in world space but Collision is the root component, so spawning the actor at the UE2 brush location
	// moved it by that location a second time: keep the component at the origin.
	if (!Collision->GetComponentTransform().Equals(FTransform::Identity))
	{
		Collision->SetWorldTransform(FTransform::Identity);
	}
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	// WorldDynamic object: object-type line of sight queries (bots: ECC_WorldStatic objects) see through it
	Collision->SetCollisionObjectType(ECC_WorldDynamic);
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	const bool bVehiclesOnly = BlocksOnlyVehicles();
	if (bBlockActors && bBlockNonZeroExtentTraces)
	{
		if (!bVehiclesOnly)
		{
			Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
			// floor / nav sweeps of the level builder and the nav graph
			Collision->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
		}
		Collision->SetCollisionResponseToChannel(ECC_Vehicle, ECR_Block);
	}
	if (bBlockKarma)
	{
		Collision->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
		Collision->SetCollisionResponseToChannel(ECC_Vehicle, ECR_Block);
		Collision->SetCollisionResponseToChannel(ECC_Destructible, ECR_Block);
	}
	if (bBlockZeroExtentTraces)
	{
		// sight and shots (zero extent traces)
		Collision->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		Collision->SetCollisionResponseToChannel(ECC_Weapon, ECR_Block);
		Collision->ComponentTags.AddUnique(ARENA_SHOT_BLOCKER_TAG);
	}
}

// ---------------------------------------------------------------------------------------------
// UArenaWorldZoneSubsystem
// ---------------------------------------------------------------------------------------------

UArenaWorldZoneSubsystem* UArenaWorldZoneSubsystem::Get(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	return World ? World->GetSubsystem<UArenaWorldZoneSubsystem>() : nullptr;
}

TStatId UArenaWorldZoneSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UArenaWorldZoneSubsystem, STATGROUP_Tickables);
}

bool UArenaWorldZoneSubsystem::LoadBspTree(const FString& File, const TSharedPtr<FJsonObject>& TreeJson)
{
	Nodes.Reset();
	ZoneActorNames.Reset();
	ZoneIsSky.Reset();
	TArray<uint8> Data;
	if (!FFileHelper::LoadFileToArray(Data, *File) || Data.Num() < 12 || FMemory::Memcmp(Data.GetData(), "AZT1", 4) != 0)
	{
		return false;
	}
	int32 Counts[2];
	FMemory::Memcpy(Counts, Data.GetData() + 4, 8);
	if (Counts[0] <= 0 || int64(Counts[0]) * 28 + 12 > Data.Num())
	{
		return false;
	}
	Nodes.SetNum(Counts[0]);
	const uint8* P = Data.GetData() + 12;
	for (int32 i = 0; i < Counts[0]; ++i, P += 28)
	{
		float Pl[4];
		int32 Ch[2];
		FMemory::Memcpy(Pl, P, 16);
		FMemory::Memcpy(Ch, P + 16, 8);
		FNode& N = Nodes[i];
		N.Plane = FPlane(Pl[0], Pl[1], Pl[2], Pl[3]);
		N.Back = Ch[0];
		N.Front = Ch[1];
		N.Zone[0] = P[24];
		N.Zone[1] = P[25];
	}
	const TArray<TSharedPtr<FJsonValue>>* Zs = nullptr;
	if (TreeJson.IsValid() && TreeJson->TryGetArrayField(TEXT("zones"), Zs))
	{
		for (const TSharedPtr<FJsonValue>& V : *Zs)
		{
			const TSharedPtr<FJsonObject> Z = V->AsObject();
			FString Name;
			bool bSky = false;
			if (Z.IsValid())
			{
				Z->TryGetStringField(TEXT("actor"), Name);
				Z->TryGetBoolField(TEXT("sky"), bSky);
			}
			ZoneActorNames.Add(Name);
			ZoneIsSky.Add(bSky);
		}
	}
	ZoneActors.SetNum(ZoneActorNames.Num());
	for (TActorIterator<AArenaZoneInfo> It(GetWorld()); It; ++It)
	{
		RegisterZone(*It);
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("UE2World: BSP zone tree %d nodes, %d zones"), Nodes.Num(), ZoneActorNames.Num());
	return true;
}

int32 UArenaWorldZoneSubsystem::GetZoneNumberAt(const FVector& Point) const
{
	if (Nodes.Num() == 0)
	{
		return INDEX_NONE;
	}
	int32 i = 0, Parent = 0, Guard = 0;
	bool bFront = false;
	while (Nodes.IsValidIndex(i) && Guard++ < 100000)
	{
		Parent = i;
		bFront = Nodes[i].Plane.PlaneDot(Point) >= 0.0;
		i = bFront ? Nodes[i].Front : Nodes[i].Back;
	}
	return Nodes[Parent].Zone[bFront ? 1 : 0];
}

AArenaZoneInfo* UArenaWorldZoneSubsystem::GetZone(int32 ZoneNumber) const
{
	return ZoneActors.IsValidIndex(ZoneNumber) ? ZoneActors[ZoneNumber].Get() : nullptr;
}

AArenaZoneInfo* UArenaWorldZoneSubsystem::GetZoneAt(const FVector& Point) const
{
	return GetZone(GetZoneNumberAt(Point));
}

bool UArenaWorldZoneSubsystem::IsInSkyZone(const FVector& Point) const
{
	const int32 Z = GetZoneNumberAt(Point);
	return ZoneIsSky.IsValidIndex(Z) && ZoneIsSky[Z];
}

void UArenaWorldZoneSubsystem::RegisterZone(AArenaZoneInfo* Zone)
{
	if (!Zone)
	{
		return;
	}
	for (int32 i = 0; i < ZoneActorNames.Num(); ++i)
	{
		if (ZoneActorNames[i] == Zone->UE2Name)
		{
			ZoneActors[i] = Zone;
			Zone->ZoneNumber = i;
		}
	}
}

void UArenaWorldZoneSubsystem::RegisterVolume(AArenaPhysicsVolume* Volume)
{
	Volumes.AddUnique(Volume);
}

void UArenaWorldZoneSubsystem::UnregisterVolume(AArenaPhysicsVolume* Volume)
{
	Volumes.Remove(Volume);
}

AArenaPhysicsVolume* UArenaWorldZoneSubsystem::GetPhysicsVolumeAt(const FVector& Point) const
{
	AArenaPhysicsVolume* Best = nullptr;
	for (const TWeakObjectPtr<AArenaPhysicsVolume>& W : Volumes)
	{
		AArenaPhysicsVolume* V = W.Get();
		if (V && (!Best || V->Priority > Best->Priority) && V->EncompassesUE2(Point))
		{
			Best = V;
		}
	}
	return Best;
}

float UArenaWorldZoneSubsystem::GetGravityZAt(const FVector& Point) const
{
	if (AArenaPhysicsVolume* V = GetPhysicsVolumeAt(Point))
	{
		return V->GetGravityZ();
	}
	return GetWorld() ? GetWorld()->GetGravityZ() : UPhysicsSettings::Get()->DefaultGravityZ;
}

FVector UArenaWorldZoneSubsystem::GetGravityAt(const FVector& Point) const
{
	if (AArenaPhysicsVolume* V = GetPhysicsVolumeAt(Point))
	{
		return V->GetGravityVector();
	}
	return FVector(0.f, 0.f, GetWorld() ? GetWorld()->GetGravityZ() : UPhysicsSettings::Get()->DefaultGravityZ);
}

FString UArenaWorldZoneSubsystem::GetLocationNameAt(const FVector& Point) const
{
	AArenaPhysicsVolume* V = GetPhysicsVolumeAt(Point);
	if (V && !V->LocationName.IsEmpty())
	{
		return V->LocationName;
	}
	AArenaZoneInfo* Z = GetZoneAt(Point);
	return Z ? Z->LocationName : FString();
}

bool UArenaWorldZoneSubsystem::IsHazardAt(const FVector& Point) const
{
	AArenaPhysicsVolume* V = GetPhysicsVolumeAt(Point);
	return V && (V->bKillVolume || (V->bPainCausing && V->DamagePerSec > 0.f));
}

void UArenaWorldZoneSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	SpawnHandle = InWorld.AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateUObject(this, &UArenaWorldZoneSubsystem::OnActorSpawned));
	for (TActorIterator<AActor> It(&InWorld); It; ++It)
	{
		OnActorSpawned(*It);
	}
}

void UArenaWorldZoneSubsystem::Deinitialize()
{
	if (UWorld* W = GetWorld())
	{
		W->RemoveOnActorSpawnedHandler(SpawnHandle);
	}
	Tracked.Reset();
	Super::Deinitialize();
}

void UArenaWorldZoneSubsystem::OnActorSpawned(AActor* Actor)
{
	if (Actor && (Actor->IsA<APawn>() || Actor->IsA<AArenaProjectile>() || Actor->FindComponentByClass<UProjectileMovementComponent>()))
	{
		TrackActor(Actor);
	}
}

void UArenaWorldZoneSubsystem::TrackActor(AActor* Actor)
{
	if (Volumes.Num() == 0 && ZoneActors.Num() == 0)
	{
		// nothing to track on maps without zones / volumes (still track for later registration)
	}
	Tracked.FindOrAdd(Actor);
}

void UArenaWorldZoneSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	UWorld* World = GetWorld();
	if (!World || !World->HasBegunPlay())
	{
		return;
	}
#if !UE_BUILD_SHIPPING
	TickTestHooks(DeltaTime);
#endif
	if (Volumes.Num() == 0 && Nodes.Num() == 0)
	{
		return;
	}
	for (auto It = Tracked.CreateIterator(); It; ++It)
	{
		AActor* A = It->Key.Get();
		if (!IsValid(A))
		{
			It.RemoveCurrent();
			continue;
		}
		UpdateActor(A, It->Value, DeltaTime);
	}
	UpdateView(DeltaTime);
}

void UArenaWorldZoneSubsystem::UpdateActor(AActor* A, FTracked& T, float DeltaTime)
{
	using namespace ArenaWorldZonesLocal;
	const FVector Loc = A->GetActorLocation();
	AArenaPhysicsVolume* Vol = GetPhysicsVolumeAt(Loc);
	AArenaPhysicsVolume* Old = T.Volume.Get();
	APawn* Pawn = Cast<APawn>(A);
	AArenaCharacter* Char = Cast<AArenaCharacter>(A);

	// physics volume of the movement component (gravity, water, terminal velocity, fluid friction)
	UMovementComponent* Move = MovementOf(A);
	USceneComponent* Updated = Move ? Move->UpdatedComponent.Get() : nullptr;
	if (Updated)
	{
		if (Updated->GetShouldUpdatePhysicsVolume())
		{
			Updated->SetShouldUpdatePhysicsVolume(false);
		}
		APhysicsVolume* Want = Vol ? static_cast<APhysicsVolume*>(Vol) : A->GetWorld()->GetDefaultPhysicsVolume();
		if (Updated->GetPhysicsVolume() != Want)
		{
			// notifies the volumes (ActorEnteredVolume / ActorLeavingVolume) and the movement component (swimming)
			Updated->SetPhysicsVolume(Want, true);
		}
	}
	else if (Vol != Old)
	{
		if (Old)
		{
			Old->ActorLeavingVolume(A);
		}
		if (Vol)
		{
			Vol->ActorEnteredVolume(A);
		}
	}
	if (Vol != Old)
	{
#if !UE_BUILD_SHIPPING
		if (Pawn && FParse::Param(FCommandLine::Get(), TEXT("UE2Report")))
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("UE2Report: %s volume %s -> %s at %s velocity %s"), *A->GetName(), Old ? *Old->UE2Name : TEXT("none"),
				Vol ? *Vol->UE2Name : TEXT("none"), *Loc.ToCompactString(), *A->GetVelocity().ToCompactString());
		}
#endif
		T.PainTime = 0.5f;
		T.Volume = Vol;
	}
	if (!IsValid(A))
	{
		return;
	}

	// zone changes and per-zone KillZ
	if (Nodes.Num() > 0 && Pawn)
	{
		AArenaZoneInfo* Zone = GetZoneAt(Loc);
		if (Zone != T.Zone.Get())
		{
			if (AArenaZoneInfo* OldZone = T.Zone.Get())
			{
				OldZone->ActorLeaving(A);
			}
			if (Zone)
			{
				Zone->ActorEntered(A);
			}
			T.Zone = Zone;
		}
		if (Zone && Loc.Z < Zone->KillZ && !(Char && Char->IsDead()))
		{
			const UCharacterMovementComponent* CM = Char ? Char->GetCharacterMovement() : nullptr;
			if (!Zone->bSoftKillZ || !CM || CM->IsFalling())
			{
				A->FellOutOfWorld(*GetDefault<UDamageType>());
				return;
			}
		}
	}
	if (Char && Char->IsDead())
	{
		return;
	}

	if (Vol)
	{
		// pain (UE2 PainTimer: once a second)
		if (Vol->bPainCausing && Vol->DamagePerSec != 0.f && !Vol->bKillVolume && (Pawn || Vol->bDestructive))
		{
			T.PainTime += DeltaTime;
			while (T.PainTime >= 1.f && IsValid(A))
			{
				T.PainTime -= 1.f;
				Vol->CausePainTo(A);
			}
			if (!IsValid(A))
			{
				return;
			}
		}
		// zone velocity (conveyor / wind): characters move with it in UArenaCharacterMovement (UE2 physWalking /
		// physFalling / physSwimming add it to the move), projectiles get it once on entry (ActorEnteredVolume);
		// other pawns (vehicles) are pushed here
		if (!Vol->ZoneVelocity.IsNearlyZero() && Pawn && !Vol->bBounceVelocity && !Cast<UArenaCharacterMovement>(Move))
		{
			A->AddActorWorldOffset(Vol->ZoneVelocity * DeltaTime, true);
		}
		// the horizontal part of the volume's gravity for falling projectiles (the engine applies GetGravityZ)
		if (Vol->HasLateralGravity())
		{
			if (UProjectileMovementComponent* PM = Cast<UProjectileMovementComponent>(Move))
			{
				if (PM->ProjectileGravityScale != 0.f && PM->UpdatedComponent)
				{
					const FVector G = Vol->GetGravityVector() * PM->ProjectileGravityScale;
					PM->Velocity += FVector(G.X, G.Y, 0.f) * DeltaTime;
				}
			}
		}
	}
	// falling projectiles (UE2 PHYS_Falling: grenades, bio globs, flak shells and chunks): physFalling's NewFallVelocity
	// slows them in water (FluidFriction) and limits the fall to the volume's TerminalVelocity (else the level's
	// DefaultPhysicsVolume's); UE2 PHYS_Projectile (rockets, gravity scale 0) has neither
	if (UProjectileMovementComponent* PM = Cast<UProjectileMovementComponent>(Move))
	{
		if (PM->ProjectileGravityScale != 0.f && PM->UpdatedComponent && !PM->Velocity.IsZero())
		{
			if (Vol && Vol->bWaterVolume)
			{
				PM->Velocity *= 1.f - FMath::Min(Vol->FluidFriction * DeltaTime, 1.f);
			}
			const float Limit = ArenaWorldZones::GetTerminalVelocityAt(A->GetWorld(), Loc);
			if (PM->Velocity.Z < -Limit)
			{
				PM->Velocity.Z = -Limit;
			}
		}
	}

	// breathing
	if (Char)
	{
		AArenaPhysicsVolume* HeadVol = GetPhysicsVolumeAt(Char->GetPawnViewLocation());
		const bool bHeadIn = HeadVol && HeadVol->bWaterVolume;
		if (bHeadIn)
		{
			T.UnderWaterTime += DeltaTime;
			if (T.UnderWaterTime > UnderWaterTime)
			{
				T.NextBreathDamage -= DeltaTime;
				if (T.NextBreathDamage <= 0.f)
				{
					T.NextBreathDamage = BreathTime;
					FArenaHitInfo Info;
					Info.WeaponName = TEXT("Drowned");
					ArenaDamage::ApplyDamage(Char, DrownDamage, nullptr, HeadVol, Info);
				}
			}
		}
		else
		{
			T.UnderWaterTime = 0.f;
			T.NextBreathDamage = 0.f;
		}
		T.bHeadInWater = bHeadIn;
	}
}

void UArenaWorldZoneSubsystem::UpdateView(float DeltaTime)
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager)
	{
		return;
	}
	const FVector Cam = PC->PlayerCameraManager->GetCameraLocation();
	AArenaPhysicsVolume* V = GetPhysicsVolumeAt(Cam);
	const bool bWater = V && V->bWaterVolume;
	const bool bFog = V && (bWater || V->bDistanceFog || !V->ViewFog.IsNearlyZero());
	const float Target = bFog ? 1.f : 0.f;
#if !UE_BUILD_SHIPPING
	if (LastViewVolume.Get() != V && FParse::Param(FCommandLine::Get(), TEXT("UE2Report")))
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("UE2Report: view %s now in %s (fog %d)"), *Cam.ToCompactString(), V ? *V->UE2Name : TEXT("no volume"), bFog ? 1 : 0);
	}
	LastViewVolume = V;
#endif
	// UE2 switches immediately: blend quickly to avoid a hard pop
	ViewBlend = FMath::FInterpConstantTo(ViewBlend, Target, DeltaTime, 8.f);
	if (ViewBlend <= 0.f && !ViewPP.IsValid())
	{
		return;
	}
	APostProcessVolume* PP = ViewPP.Get();
	if (!PP)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		PP = World->SpawnActor<APostProcessVolume>(APostProcessVolume::StaticClass(), FTransform::Identity, Params);
		if (!PP)
		{
			return;
		}
		PP->bUnbound = true;
		PP->Priority = 50.f;
		ViewPP = PP;
	}
	PP->BlendWeight = ViewBlend;
	if (bFog && V)
	{
		FPostProcessSettings& S = PP->Settings;
		// fog colour of the volume (UE2 DistanceFogColor / ViewFog) as a tint and a depth haze
		FLinearColor Fog = V->bDistanceFog || bWater ? FLinearColor(V->DistanceFogColor) : FLinearColor(V->ViewFog.X, V->ViewFog.Y, V->ViewFog.Z);
		if (!V->ViewFog.IsNearlyZero())
		{
			Fog = FLinearColor(V->ViewFog.X, V->ViewFog.Y, V->ViewFog.Z);
		}
		const float Lum = FMath::Max3(Fog.R, Fog.G, Fog.B);
		const FLinearColor Tint = Lum > 0.f ? FLinearColor(0.35f + 0.65f * Fog.R / Lum, 0.35f + 0.65f * Fog.G / Lum, 0.35f + 0.65f * Fog.B / Lum) : FLinearColor::White;
		S.bOverride_SceneColorTint = 1;
		S.SceneColorTint = Tint;
		S.bOverride_VignetteIntensity = 1;
		S.VignetteIntensity = 0.6f;
		FogColor = Fog;
		FogStart = V->bDistanceFog || bWater ? V->DistanceFogStart : 0.f;
		FogEnd = V->bDistanceFog || bWater ? FMath::Max(V->DistanceFogEnd, 200.f) : 3000.f;
	}
	// the level's height fog turns into the volume's distance fog
	AExponentialHeightFog* FogActor = HeightFog.Get();
	if (!FogActor)
	{
		TActorIterator<AExponentialHeightFog> It(World);
		FogActor = It ? *It : nullptr;
		HeightFog = FogActor;
	}
	UExponentialHeightFogComponent* FogComp = FogActor ? FogActor->GetComponent() : nullptr;
	if (FogComp && ViewBlend > 0.f && !bFogApplied)
	{
		// remember the fog as it is now (the level's, or the zone distance fog of UArenaZoneFogSubsystem)
		SavedFogDensity = FogComp->FogDensity;
		SavedFogFalloff = FogComp->FogHeightFalloff;
		SavedFogStart = FogComp->StartDistance;
		SavedFogColor = FogComp->FogInscatteringLuminance;
		SavedFogMaxOpacity = FogComp->FogMaxOpacity;
	}
	if (FogComp && (ViewBlend > 0.f || bFogApplied))
	{
		UExponentialHeightFogComponent* C = FogComp;
		bFogApplied = ViewBlend > 0.f;
		// thick enough to hide everything beyond FogEnd
		const float Dense = FMath::Clamp(80.f / FMath::Max(FogEnd - FogStart, 100.f), 0.002f, 0.5f);
		const float B = ViewBlend;
		C->SetFogDensity(FMath::Lerp(SavedFogDensity, Dense, B));
		C->SetFogHeightFalloff(FMath::Lerp(SavedFogFalloff, 0.001f, B));
		C->SetStartDistance(FMath::Lerp(SavedFogStart, FogStart, B));
		C->SetFogInscatteringColor(FMath::Lerp(SavedFogColor, FogColor * 0.5f, B));
		C->SetFogMaxOpacity(FMath::Lerp(SavedFogMaxOpacity, 1.f, B));
	}
}

void UArenaWorldZoneSubsystem::TickTestHooks(float DeltaTime)
{
	UWorld* World = GetWorld();
	const TCHAR* Cmd = FCommandLine::Get();
	FString Views;
	if (FParse::Value(Cmd, TEXT("UE2View="), Views, false))
	{
		TArray<FString> List;
		Views.ParseIntoArray(List, TEXT(";"));
		float Every = 5.f;
		FParse::Value(Cmd, TEXT("UE2ViewEvery="), Every);
		TestViewTime += DeltaTime;
		const int32 Index = List.Num() > 0 ? FMath::Min(int32(TestViewTime / FMath::Max(Every, 0.1f)), List.Num() - 1) : 0;
		TArray<FString> F;
		if (List.IsValidIndex(Index))
		{
			List[Index].ParseIntoArray(F, TEXT(","));
		}
		if (F.Num() >= 5)
		{
			const FVector Loc(FCString::Atod(*F[0]), FCString::Atod(*F[1]), FCString::Atod(*F[2]));
			const FRotator Rot(FCString::Atof(*F[3]), FCString::Atof(*F[4]), 0.f);
			ACameraActor* Cam = TestCamera.Get();
			if (!Cam)
			{
				Cam = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), Loc, Rot);
				TestCamera = Cam;
			}
			if (Cam)
			{
				Cam->SetActorLocationAndRotation(Loc, Rot);
				if (APlayerController* PC = World->GetFirstPlayerController())
				{
					if (PC->GetViewTarget() != Cam)
					{
						PC->SetViewTarget(Cam);
					}
				}
			}
		}
	}
	// -UE2PlaceOnMover=<Mover name>[,<seconds>]: put the first bot on that mover's resting top (lift checks)
	FString PlaceOn;
	if (FParse::Value(Cmd, TEXT("UE2PlaceOnMover="), PlaceOn, false))
	{
		static TSet<FString> Done;
		FString Name = PlaceOn, When = TEXT("5");
		PlaceOn.Split(TEXT(","), &Name, &When);
		const FString Key = GetWorld()->GetName() + Name;
		if (!Done.Contains(Key) && World->GetTimeSeconds() > FCString::Atof(*When))
		{
			Done.Add(Key);
			AArenaMover* M = Cast<AArenaMover>(ArenaUE2World::FindActorByName(World, Name));
			for (TActorIterator<ACharacter> It(World); It && M; ++It)
			{
				if (!It->IsPlayerControlled() && It->GetController())
				{
					const FVector Top = M->GetKeyLocation(M->KeyNum);
					It->TeleportTo(Top + FVector(0, 0, It->GetSimpleCollisionHalfHeight() + 5.f), It->GetActorRotation());
					UE_LOG(LogOpenUT2k4, Log, TEXT("UE2Report: placed %s on %s at %s"), *It->GetName(), *Name, *Top.ToCompactString());
					if (APlayerController* PC = World->GetFirstPlayerController())
					{
						PC->SetViewTarget(*It);
					}
					// keep it there (no AI) to see the lift work
					if (AController* Ctrl = It->GetController())
					{
						Ctrl->SetActorTickEnabled(false);
					}
					break;
				}
			}
		}
	}
	// -UE2PlaceBotAt=x|y|z|seconds: put the first bot there (water / lava checks), brain off
	FString PlaceAt;
	if (FParse::Value(Cmd, TEXT("UE2PlaceBotAt="), PlaceAt, false))
	{
		static TSet<FString> DoneAt;
		TArray<FString> F;
		PlaceAt.ParseIntoArray(F, TEXT("|"));
		const FString Key = GetWorld()->GetName();
		if (F.Num() >= 4 && !DoneAt.Contains(Key) && World->GetTimeSeconds() > FCString::Atof(*F[3]))
		{
			DoneAt.Add(Key);
			for (TActorIterator<ACharacter> It(World); It; ++It)
			{
				if (!It->IsPlayerControlled() && It->GetController())
				{
					const FVector At(FCString::Atod(*F[0]), FCString::Atod(*F[1]), FCString::Atod(*F[2]));
					It->TeleportTo(At, It->GetActorRotation());
					It->GetCharacterMovement()->StopMovementImmediately();
					It->GetController()->SetActorTickEnabled(false);
					if (APlayerController* PC = World->GetFirstPlayerController())
					{
						PC->SetViewTarget(*It);
					}
					UE_LOG(LogOpenUT2k4, Log, TEXT("UE2Report: placed %s at %s"), *It->GetName(), *At.ToCompactString());
					break;
				}
			}
		}
	}
	if (FParse::Param(Cmd, TEXT("UE2Report")))
	{
		// jump apexes (low gravity checks) and a periodic state line per character
		for (TActorIterator<ACharacter> It(World); It; ++It)
		{
			UCharacterMovementComponent* CM = It->GetCharacterMovement();
			if (!CM)
			{
				continue;
			}
			FJumpTrack& J = Jumps.FindOrAdd(*It);
			const float Z = float(It->GetActorLocation().Z);
			if (CM->IsFalling())
			{
				if (!J.bAir)
				{
					J.bAir = true;
					J.StartZ = J.MaxZ = Z;
				}
				J.MaxZ = FMath::Max(J.MaxZ, Z);
			}
			else if (J.bAir)
			{
				J.bAir = false;
				if (J.MaxZ - J.StartZ > 20.f)
				{
					UE_LOG(LogOpenUT2k4, Log, TEXT("UE2Report: %s apex +%.0f cm (gravity %.0f, volume %s)"), *It->GetName(), J.MaxZ - J.StartZ, CM->GetGravityZ(),
						*GetNameSafe(CM->GetPhysicsVolume()));
				}
			}
		}
		ReportTime += DeltaTime;
		if (ReportTime > 3.f)
		{
			ReportTime = 0.f;
			for (TActorIterator<ACharacter> It(World); It; ++It)
			{
				UCharacterMovementComponent* CM = It->GetCharacterMovement();
				const AArenaZoneInfo* Z = GetZoneAt(It->GetActorLocation());
				const UObject* BaseObj = It->GetMovementBaseObject();
				// what the pawn would run into along its volume's gravity / its velocity within 1 m (stuck pawns)
				const AArenaPhysicsVolume* PV = CM ? Cast<AArenaPhysicsVolume>(CM->GetPhysicsVolume()) : nullptr;
				const FVector Dir = PV && PV->HasLateralGravity() ? PV->GetGravityVector().GetSafeNormal() : It->GetVelocity().GetSafeNormal();
				FString Ahead = TEXT("-");
				FHitResult Hit;
				FCollisionQueryParams Q(SCENE_QUERY_STAT(UE2ReportAhead), false, *It);
				if (!Dir.IsNearlyZero() && World->SweepSingleByChannel(Hit, It->GetActorLocation(), It->GetActorLocation() + Dir * 100.f, FQuat::Identity, ECC_Pawn,
					FCollisionShape::MakeCapsule(It->GetSimpleCollisionRadius(), It->GetSimpleCollisionHalfHeight()), Q))
				{
					const UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Hit.GetComponent());
					Ahead = FString::Printf(TEXT("%s/%s (%s) at %.0f cm, normal %s"), *GetNameSafe(Hit.GetActor()), *GetNameSafe(Hit.GetComponent()),
						SMC ? *GetNameSafe(SMC->GetStaticMesh()) : TEXT("-"), Hit.Distance, *Hit.ImpactNormal.ToCompactString());
				}
				UE_LOG(LogOpenUT2k4, Log, TEXT("UE2Report: %s at %s mode %d gravity %.0f volume %s zone %s base %s velocity %s ahead %s"), *It->GetName(), *It->GetActorLocation().ToCompactString(),
					CM ? int32(CM->MovementMode) : -1, CM ? CM->GetGravityZ() : 0.f, CM ? *GetNameSafe(CM->GetPhysicsVolume()) : TEXT("-"),
					Z ? *Z->UE2Name : TEXT("-"), BaseObj ? *GetNameSafe(Cast<UActorComponent>(BaseObj) ? Cast<UActorComponent>(BaseObj)->GetOwner() : nullptr) : TEXT("-"),
					*It->GetVelocity().ToCompactString(), *Ahead);
			}
		}
	}
}

float ArenaWorldZones::GetTerminalVelocityAt(const UWorld* World, const FVector& Location)
{
	const UArenaWorldZoneSubsystem* Zones = World ? World->GetSubsystem<UArenaWorldZoneSubsystem>() : nullptr;
	if (const AArenaPhysicsVolume* V = Zones && Zones->HasVolumes() ? Zones->GetPhysicsVolumeAt(Location) : nullptr)
	{
		return FMath::Abs(V->TerminalVelocity);
	}
	const APhysicsVolume* D = World ? World->GetDefaultPhysicsVolume() : nullptr;
	return D ? FMath::Abs(D->TerminalVelocity) : 2500.f * ArenaUE2World::Scale;
}

FVector ArenaWorldZones::NewFallVelocity(const UWorld* World, const FVector& Location, const FVector& Velocity, float GravityScale, float DeltaTime)
{
	const UArenaWorldZoneSubsystem* Zones = World ? World->GetSubsystem<UArenaWorldZoneSubsystem>() : nullptr;
	const AArenaPhysicsVolume* V = Zones && Zones->HasVolumes() ? Zones->GetPhysicsVolumeAt(Location) : nullptr;
	FVector Out = Velocity;
	if (V && V->bWaterVolume)
	{
		Out *= 1.f - FMath::Min(V->FluidFriction * DeltaTime, 1.f);
	}
	const FVector G = V ? V->GetGravityVector() : FVector(0.f, 0.f, World ? World->GetGravityZ() : -1900.f);
	Out += G * GravityScale * DeltaTime;
	const float Limit = GetTerminalVelocityAt(World, Location);
	if (Out.Z < -Limit)
	{
		Out.Z = -Limit;
	}
	return Out;
}
