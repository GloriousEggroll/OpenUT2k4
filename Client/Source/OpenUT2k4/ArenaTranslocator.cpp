#include "ArenaTranslocator.h"
#include "OpenUT2k4.h"
#include "ArenaArt.h"
#include "ArenaArtFX.h"
#include "ArenaArtHUD.h"
#include "ArenaAssets.h"
#include "ArenaBotController.h"
#include "ArenaCarryable.h"
#include "ArenaCharacter.h"
#include "ArenaFlag.h"
#include "ArenaGameMode.h"
#include "ArenaGameRules.h"
#include "ArenaHUD.h"
#include "ArenaMover.h"
#include "ArenaONS_Vehicle.h"
#include "ArenaPlayerState.h"
#include "ArenaTypes.h"
#include "ArenaWorldZones.h"
#include "CanvasItem.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"
#include "Components/AudioComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Canvas.h"
#include "Engine/OverlapResult.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "UnrealClient.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "TimerManager.h"
#include "Sound/SoundBase.h"

namespace ArenaTranslocatorLocal
{
	using namespace ArenaTranslocator;
	constexpr float UU = 2.f;

	// stock sounds (TransFire.TransFireSound / RecallFireSound, TransRecall.TransFailedSound, TransLauncher.SelectSound,
	// TransBeacon.ImpactSound / AmbientSound)
	const TCHAR* const FireSound = TEXT("Sounds/WeaponSounds/BaseFiringSounds_BTranslocatorFire");
	const TCHAR* const RecallSound = TEXT("Sounds/WeaponSounds/BaseGunTech_BTranslocatorModuleRegeneration");
	const TCHAR* const FailedSound = TEXT("Sounds/WeaponSounds/BaseGunTech_BSeekLost1");
	const TCHAR* const SelectSound = TEXT("Sounds/WeaponSounds/Misc_translocator_change");
	const TCHAR* const ImpactSound = TEXT("Sounds/WeaponSounds/PGrenFloor1_P1GrenFloor1");
	const TCHAR* const AmbientSoundPath = TEXT("Sounds/WeaponSounds/Misc_redeemer_flight");

	void PlaySound2D(const AActor* At, const TCHAR* Path, float Volume = 1.f)
	{
		if (USoundBase* S = At ? ArenaArt::LoadOneShotSound(Path) : nullptr)
		{
			UGameplayStatics::PlaySoundAtLocation(At, S, At->GetActorLocation(), Volume);
		}
	}

	/** ClientPlaySound: only the owning (local human) player hears it. */
	void PlayOwnerSound(const AArenaCharacter* C, const TCHAR* Path)
	{
		if (C && C->IsPlayerControlled() && C->IsLocallyControlled())
		{
			if (USoundBase* S = ArenaArt::LoadOneShotSound(Path))
			{
				UGameplayStatics::PlaySound2D(C, S);
			}
		}
	}

	int32 TeamOf(const AArenaCharacter* C)
	{
		const AArenaPlayerState* PS = C ? C->GetPlayerState<AArenaPlayerState>() : nullptr;
		return PS && PS->Team >= 0 ? PS->Team : 255;
	}

	AArenaGameMode* GameModeOf(const UObject* O)
	{
		const UWorld* W = O ? O->GetWorld() : nullptr;
		return W ? W->GetAuthGameMode<AArenaGameMode>() : nullptr;
	}

	/** UE2 Controller.SameTeamAs. */
	bool SameTeam(const AController* A, const AController* B)
	{
		if (!A || !B)
		{
			return false;
		}
		if (A == B)
		{
			return true;
		}
		const AArenaGameMode* GM = GameModeOf(A);
		return GM && GM->IsTeamGame() && GM->AreOnSameTeam(A, B);
	}

	FLinearColor TeamTrailColor(int32 Team)
	{
		// TransTrail (blue) / TransTrailRed mColorRange
		return Team == 0 ? FLinearColor(FColor(180, 10, 10)) : FLinearColor(FColor(80, 150, 255));
	}

	FLinearColor TeamFlareColor(int32 Team)
	{
		// TransFlareRed / TransFlareBlue mColorRange
		return Team == 0 ? FLinearColor(FColor(200, 50, 50)) : FLinearColor(FColor(50, 50, 200));
	}

	/** Pawns in the world that block translocation / can be telefragged. */
	void ForEachLivePawn(UWorld* World, TFunctionRef<void(AArenaCharacter*)> Fn)
	{
		for (TActorIterator<AArenaCharacter> It(World); It; ++It)
		{
			if (It->IsAlive())
			{
				Fn(*It);
			}
		}
	}

	FString& LastBlocker()
	{
		static FString S;
		return S;
	}

	/** UE2 FindSpot / SetLocation for a pawn cylinder: true when the capsule fits at Loc against world geometry. */
	bool CapsuleFitsWorld(const AArenaCharacter* C, const FVector& Loc)
	{
		const UCapsuleComponent* Cap = C->GetCapsuleComponent();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaTranslocFit), false, C);
		FCollisionObjectQueryParams Obj;
		Obj.AddObjectTypesToQuery(ECC_WorldStatic);
		Obj.AddObjectTypesToQuery(ECC_WorldDynamic);
		const FCollisionShape Shape = FCollisionShape::MakeCapsule(Cap->GetScaledCapsuleRadius(), Cap->GetScaledCapsuleHalfHeight());
		TArray<FOverlapResult> Hits;
		if (!C->GetWorld()->OverlapMultiByObjectType(Hits, Loc, FQuat::Identity, Obj, Shape, Q))
		{
			return true;
		}
		for (const FOverlapResult& H : Hits)
		{
			const UPrimitiveComponent* P = H.GetComponent();
			const AActor* A = H.GetActor();
			// pawns (and what they carry) are telefragged, not in the way
			bool bPawnPart = false;
			for (const AActor* O = A; O && !bPawnPart; O = O->GetOwner())
			{
				bPawnPart = O->IsA<APawn>();
			}
			if (bPawnPart || (A && A->GetAttachParentActor() && A->GetAttachParentActor()->IsA<APawn>()))
			{
				continue;
			}
			// only things that block pawns count (triggers, pickups and the like overlap)
			if (P && P->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block && !Cast<AArenaTransBeacon>(A))
			{
				LastBlocker() = FString::Printf(TEXT("%s.%s"), A ? *A->GetName() : TEXT("?"), *P->GetName());
				return false;
			}
		}
		return true;
	}

	/** UE2 ULevel::FindSpot: nudges a location by up to the extent so the cylinder fits. */
	bool FindSpot(const AArenaCharacter* C, FVector& InOutLoc)
	{
		if (CapsuleFitsWorld(C, InOutLoc))
		{
			return true;
		}
		const float R = C->GetCapsuleComponent()->GetScaledCapsuleRadius();
		const float H = C->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		static const FVector Dirs[] = { FVector(0, 0, 1), FVector(1, 0, 0), FVector(-1, 0, 0), FVector(0, 1, 0), FVector(0, -1, 0),
			FVector(1, 1, 0), FVector(1, -1, 0), FVector(-1, 1, 0), FVector(-1, -1, 0), FVector(0, 0, -1) };
		// UE2 CheckSlice / FindSpot move the cylinder by up to its extent: up first (a spot on the floor), then sideways
		for (const float Up : { 0.25f, 0.5f, 0.75f, 1.f })
		{
			const FVector Try = InOutLoc + FVector(0.f, 0.f, Up * H);
			if (CapsuleFitsWorld(C, Try))
			{
				InOutLoc = Try;
				return true;
			}
		}
		for (const float Up : { 0.f, 0.5f, 1.f })
		{
			for (const FVector& D : Dirs)
			{
				const FVector Try = InOutLoc + FVector(D.X * R, D.Y * R, D.Z * H * 0.5f + Up * H);
				if (CapsuleFitsWorld(C, Try))
				{
					InOutLoc = Try;
					return true;
				}
			}
		}
		return false;
	}

	/**
	 * TransRecall.TransMaterials: Instigator.SetOverlayMaterial(PlayerTransRed / PlayerTrans, 1 s): the panning cell
	 * texture (TexScaler 0.5 -> 2 tiles, TexPanner 0.2 at 45 degrees) drawn additively over the player model.
	 */
	void ApplyTransOverlay(AArenaCharacter* C, int32 Team)
	{
		USkeletalMeshComponent* Mesh = C ? C->GetMesh() : nullptr;
		if (!Mesh || !Mesh->GetSkeletalMeshAsset() || !Mesh->IsVisible())
		{
			return;
		}
		const TCHAR* Tex = Team == 1 ? TEXT("Textures/XGameShaders/PlayerShaders_TransPlayerCell") : TEXT("Textures/XGameShaders/PlayerShaders_TransPlayerCellRed");
		UMaterialInstanceDynamic* MID = ArenaArtFX::MakeMaterial(Mesh, Tex, ArenaArtFX::EBlend::Additive, FLinearColor::White, 1.5f);
		if (!MID)
		{
			return;
		}
		MID->SetScalarParameterValue(TEXT("TileU"), 2.f);
		MID->SetScalarParameterValue(TEXT("TileV"), 2.f);
		MID->SetScalarParameterValue(TEXT("PanU"), 0.2f * 0.7071f);
		MID->SetScalarParameterValue(TEXT("PanV"), 0.2f * 0.7071f);
		UMaterialInterface* Previous = Mesh->GetOverlayMaterial();
		if (Cast<UMaterialInstanceDynamic>(Previous) && Previous->GetName().StartsWith(TEXT("TransOverlay")))
		{
			Previous = nullptr;
		}
		MID->Rename(*MakeUniqueObjectName(Mesh, UMaterialInstanceDynamic::StaticClass(), TEXT("TransOverlay")).ToString());
		Mesh->SetOverlayMaterial(MID);
		TWeakObjectPtr<USkeletalMeshComponent> WeakMesh = Mesh;
		TWeakObjectPtr<UMaterialInterface> WeakPrev = Previous;
		TWeakObjectPtr<UMaterialInstanceDynamic> WeakMID = MID;
		FTimerHandle H;
		C->GetWorldTimerManager().SetTimer(H, FTimerDelegate::CreateLambda([WeakMesh, WeakPrev, WeakMID]()
		{
			if (USkeletalMeshComponent* M = WeakMesh.Get())
			{
				if (M->GetOverlayMaterial() == WeakMID.Get())
				{
					M->SetOverlayMaterial(WeakPrev.Get());
				}
			}
		}), 1.f, false);
		ArenaTranslocator::Stats().Overlays++;
	}

	/** Pawn.SetLocation for the translocating pawn (FarMoveActor with FindSpot): moves it when it fits. */
	bool SetPawnLocation(AArenaCharacter* C, const FVector& Dest)
	{
		FVector Loc = Dest;
		if (!FindSpot(C, Loc))
		{
			return false;
		}
		return C->SetActorLocation(Loc, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

using namespace ArenaTranslocatorLocal;

// =================================================================================================
// Shared helpers
// =================================================================================================

const FString& ArenaTranslocator::TelefraggedName()
{
	static const FString N(TEXT("Telefragged"));
	return N;
}

const FString& ArenaTranslocator::GibbedName()
{
	static const FString N(TEXT("Gibbed"));
	return N;
}

ArenaTranslocator::FStats& ArenaTranslocator::Stats()
{
	static FStats S;
	return S;
}

float ArenaTranslocator::GravityZAt(const UWorld* World, const FVector& Location)
{
	if (!World)
	{
		return -1900.f;
	}
	const UArenaWorldZoneSubsystem* Zones = UArenaWorldZoneSubsystem::Get(World);
	if (const AArenaPhysicsVolume* Vol = Zones && Zones->HasVolumes() ? Zones->GetPhysicsVolumeAt(Location) : nullptr)
	{
		return Vol->GetGravityVector().Z * ArenaTuning::GravityScale;
	}
	return World->GetGravityZ() * ArenaTuning::GravityScale;
}

FVector ArenaTranslocator::AdjustToss(float TSpeed, const FVector& Start, const FVector& End, float GravityZ, bool bNormalize)
{
	// UE2 native Controller.AdjustToss: horizontal speed TSpeed, the vertical part that arrives at End's height
	FVector Dest2D = End - Start;
	Dest2D.Z = 0.f;
	const float Dist2D = Dest2D.Size();
	if (Dist2D < 1.f)
	{
		return FVector(0.f, 0.f, End.Z > Start.Z ? TSpeed : -TSpeed);
	}
	const float TossTime = Dist2D / TSpeed;
	FVector Vel = Dest2D / TossTime;
	Vel.Z = (End.Z - Start.Z) / TossTime - 0.5f * GravityZ * TossTime;
	if (bNormalize)
	{
		Vel = Vel.GetSafeNormal() * TSpeed;
	}
	return Vel;
}

void ArenaTranslocator::DrawCameraOverlay(const FArenaDraw& D, const APlayerController* PC)
{
	const AArenaTransBeacon* B = PC ? Cast<AArenaTransBeacon>(PC->GetViewTarget()) : nullptr;
	UCanvas* Canvas = D.Canvas;
	if (!B || !Canvas)
	{
		return;
	}
	const float W = Canvas->ClipX;
	const float H = Canvas->ClipY;
	// TransLauncher.RenderOverlays: XGameShaders.ZoomFX.TransCamFB (FinalBlend modulate) over the whole screen, then
	// ScreenNoiseFB (darken) brighter the further the beacon is from its owner (DrawColor 20..255 over MaxCamDist)
	auto Tile = [Canvas, W, H](const TCHAR* Path, ESimpleElementBlendMode Blend, const FLinearColor& Color, float UV)
	{
		UTexture2D* Tex = ArenaArtHUD::GetTexture(Path);
		if (!Tex || !Tex->GetResource())
		{
			return false;
		}
		FCanvasTileItem Item(FVector2D::ZeroVector, Tex->GetResource(), FVector2D(W, H), FVector2D::ZeroVector, FVector2D(UV, UV), Color);
		Item.BlendMode = Blend;
		Canvas->DrawItem(Item);
		return true;
	};
	Tile(TEXT("Textures/XGameShaders/ZoomFX_transcamera"), SE_BLEND_Modulate, FLinearColor::White, 1.f);
	const AArenaCharacter* Owner = B->GetBeaconOwner();
	const float Dist = Owner ? FVector::Dist(B->GetActorLocation(), Owner->GetActorLocation()) : 0.f;
	float Clr = Dist > MaxCamDistUU * UU ? 255.f : Dist / (MaxCamDistUU * UU) * 255.f;
	Clr = float(FMath::Clamp(int32(Clr), 20, 255));
	// FB_Darken (dest * (1 - src)): the noise drawn translucent black with its strength as alpha
	const float Noise = Clr / 255.f;
	const float Pan = FMath::Fmod(float(B->GetWorld()->GetTimeSeconds()) * 7.3f, 1.f);
	UTexture2D* NoiseTex = ArenaArtHUD::GetTexture(TEXT("Textures/XGameShaders/ZoomFX_ScreenNoise"));
	if (NoiseTex && NoiseTex->GetResource())
	{
		FCanvasTileItem Item(FVector2D::ZeroVector, NoiseTex->GetResource(), FVector2D(W, H), FVector2D(Pan, 1.f - Pan),
			FVector2D(Pan + W / 128.f * 0.25f, 1.f - Pan + H / 128.f * 0.25f), FLinearColor(Noise, Noise, Noise, 1.f));
		Item.BlendMode = SE_BLEND_Additive;
		Canvas->DrawItem(Item);
		FCanvasTileItem Dark(FVector2D::ZeroVector, GWhiteTexture, FVector2D(W, H), FLinearColor(0.f, 0.f, 0.f, Noise * 0.35f));
		Dark.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Dark);
	}
}

// =================================================================================================
// Weapon (TransLauncher)
// =================================================================================================

AWeap_Translocator::AWeap_Translocator()
{
	WeaponName = TEXT("Translocator");
	Slot = 10;                  // InventoryGroup 10
	MaxAmmo = int32(ArenaTranslocator::AmmoChargeMax);
	InitialAmmo = MaxAmmo;
	AmmoPickupAmount = 0;
	bInfiniteAmmo = false;
	Priority = -1.f;            // AIRating -1: never picked as the best weapon
	WeaponColor = FLinearColor(0.2f, 0.35f, 0.9f);
	BringUpTime = 0.33f;
	PutDownTime = 0.33f;
	for (int32 M = 0; M < 2; ++M)
	{
		FArenaFireMode& F = FireModes[M];
		F.FireType = EArenaFireType::Projectile;
		F.Damage = 0.f;
		F.RefireTime = ArenaTranslocator::FireRate; // TransFire / TransRecall FireRate
		F.AmmoPerShot = 0;
		F.Color = FLinearColor(0.3f, 0.5f, 1.f);
		F.ProjSpawnOffset = ArenaTranslocator::ProjSpawnOffsetUU;
		F.Projectile.Speed = ArenaTranslocator::BeaconSpeedUU * UU;
	}
}

void AWeap_Translocator::GiveTo(AArenaCharacter* NewOwner)
{
	Super::GiveTo(NewOwner);
	// TransLauncher.GiveAmmo: AmmoChargeF = default 6
	SetCharge(ArenaTranslocator::AmmoChargeMax);
}

void AWeap_Translocator::SetCharge(float InCharge)
{
	AmmoChargeF = FMath::Min(InCharge, ArenaTranslocator::AmmoChargeMax);
	// RepAmmo: the HUD's ammo digits
	Ammo = FMath::Max(0, FMath::FloorToInt32(AmmoChargeF));
}

void AWeap_Translocator::BuildViewModel()
{
	// procedural stand-in when the converted first-person mesh is missing: a grip, a disc launcher and the puck
	const FLinearColor Dark(0.12f, 0.13f, 0.16f);
	AddViewPart(ArenaAssets::Cube(), FVector(26.f, 13.f, -15.f), FRotator::ZeroRotator, FVector(0.3f, 0.08f, 0.08f), Dark);
	AddViewPart(ArenaAssets::Cylinder(), FVector(40.f, 13.f, -10.f), FRotator(0.f, 0.f, 0.f), FVector(0.14f, 0.14f, 0.03f), WeaponColor, true);
}

void AWeap_Translocator::Equip()
{
	Super::Equip();
	if (ArenaOwner && ArenaOwner->IsPlayerControlled())
	{
		PlayOwnerSound(ArenaOwner, SelectSound);
	}
	AppliedSkinTeam = -100;
	ApplyTeamSkin();
}

void AWeap_Translocator::Unequip()
{
	// TransLauncher.PutDown: ViewPlayer
	ViewPlayer();
	bPendingFire[0] = bPendingFire[1] = false;
	Super::Unequip();
}

bool AWeap_Translocator::ReadyToFire(uint8 Mode) const
{
	return GetWorldTime() >= NextFireTime;
}

void AWeap_Translocator::StartFire(uint8 Mode)
{
	if (Mode > 1 || !bEquipped || !ArenaOwner || ArenaOwner->IsDead() || bFireHeld[Mode])
	{
		return;
	}
	// bots use BotThrow / Translocate (TransLauncher.BotFire returns false)
	if (!ArenaOwner->IsPlayerControlled())
	{
		return;
	}
	// TransLauncher.StartFire (bPrevWeaponSwitch): fire pressed while alt fire is held switches back to the old weapon
	if (Mode == 0 && bFireHeld[1])
	{
		AArenaWeapon* Old = PrevWeapon.Get();
		if (Old && Old->HasAmmo() && Old->GetArenaOwner() == ArenaOwner)
		{
			StopAllFire();
			ArenaOwner->SwitchToWeapon(Old);
			return;
		}
	}
	bFireHeld[Mode] = true;
	bPendingFire[Mode] = true;
}

void AWeap_Translocator::StopFire(uint8 Mode)
{
	if (Mode > 1)
	{
		return;
	}
	bFireHeld[Mode] = false;
	// TransFire.bWaitForRelease: a press fires once; a press during the refire time still fires when it is up
}

void AWeap_Translocator::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!ArenaOwner)
	{
		return;
	}
	// TransLauncher.Tick: AmmoChargeF += dt x AmmoChargeRate up to AmmoChargeMax; a drained launcher (Bombing Run)
	// gets 1.5 back as soon as someone holds the ball
	if (AmmoChargeF < ArenaTranslocator::AmmoChargeMax)
	{
		AmmoChargeF = FMath::Min(AmmoChargeF + DeltaSeconds * ArenaTranslocator::AmmoChargeRate, ArenaTranslocator::AmmoChargeMax);
		if (AmmoChargeF >= 1.5f)
		{
			bDrained = false;
		}
		else if (bDrained)
		{
			for (TActorIterator<AArenaCarryable> It(GetWorld()); It; ++It)
			{
				if (It->IsCarried())
				{
					bDrained = false;
					AmmoChargeF = 1.5f;
					break;
				}
			}
		}
		Ammo = FMath::Max(0, FMath::FloorToInt32(AmmoChargeF));
	}
	// remember the weapon in use before this one (TransLauncher.StartFire's OldWeapon)
	if (!bEquipped)
	{
		AArenaWeapon* Cur = ArenaOwner->GetCurrentWeapon();
		if (Cur && Cur != this)
		{
			PrevWeapon = Cur;
		}
	}
	if (bEquipped && !ArenaOwner->IsDead())
	{
		for (uint8 M = 0; M < 2; ++M)
		{
			if (bPendingFire[M] && ReadyToFire(M))
			{
				bPendingFire[M] = false;
				const float Now = GetWorldTime();
				bool bFired = false;
				if (M == 0)
				{
					// TransFire.AllowFire: AmmoChargeF >= 1
					if (AmmoChargeF >= 1.f)
					{
						bFired = ThrowOrRecall();
					}
				}
				else
				{
					// TransRecall.AllowFire: AmmoChargeF >= 1, else TransFailedSound when a beacon is out
					if (AmmoChargeF >= 1.f)
					{
						Translocate();
						bFired = true;
					}
					else if (Beacon.IsValid())
					{
						PlayOwnerSound(ArenaOwner, FailedSound);
					}
				}
				if (bFired)
				{
					NextFireTime = FMath::Max(NextFireTime + ArenaTranslocator::FireRate, Now);
				}
			}
		}
		ApplyTeamSkin();
	}
	// the beacon camera ends with the beacon
	if (IsViewingBeacon() && !Beacon.IsValid())
	{
		ViewPlayer();
	}
}

void AWeap_Translocator::ApplyTeamSkin()
{
	// TransLauncher.RenderOverlays / TransAttachment.BaseChange: Skins[1] = NEWTranslocatorBLUE for the blue team
	if (!ArenaOwner)
	{
		return;
	}
	const int32 Team = TeamOf(ArenaOwner);
	USkeletalMeshComponent* TP = ArenaOwner->GetThirdPersonWeapon();
	if (Team == AppliedSkinTeam && SkinnedThirdPerson.Get() == TP)
	{
		return;
	}
	if (!BlueSkin)
	{
		BlueSkin = ArenaArt::Load<UMaterialInterface>(TEXT("Materials/WeaponSkins/Skins_NEWTranslocatorBLUE"));
		RedSkin = ArenaArt::Load<UMaterialInterface>(TEXT("Materials/WeaponSkins/Skins_NEWTranslocatorTEX"));
	}
	UMaterialInterface* Skin = Team == 1 ? BlueSkin.Get() : RedSkin.Get();
	if (!Skin)
	{
		AppliedSkinTeam = Team;
		return;
	}
	// the slot of the mesh's NEWTranslocatorTEX skin (UE2 Skins[1]; the imported section order may differ)
	auto ApplyTo = [Skin](USkeletalMeshComponent* M, const TCHAR* MeshName)
	{
		if (!M || !M->GetSkeletalMeshAsset() || !M->GetSkeletalMeshAsset()->GetName().Contains(MeshName))
		{
			return;
		}
		const TArray<FName> Slots = M->GetMaterialSlotNames();
		static int32 Logged = 0;
		if (Logged++ < 2)
		{
			FString Names;
			for (int32 i = 0; i < M->GetNumMaterials(); ++i)
			{
				Names += FString::Printf(TEXT(" [%d] %s / %s"), i, Slots.IsValidIndex(i) ? *Slots[i].ToString() : TEXT("-"),
					M->GetMaterial(i) ? *M->GetMaterial(i)->GetName() : TEXT("-"));
			}
			UE_LOG(LogOpenUT2k4, Log, TEXT("Translocator: %s slots:%s"), *M->GetSkeletalMeshAsset()->GetName(), *Names);
		}
		for (int32 i = 0; i < Slots.Num(); ++i)
		{
			if (Slots[i].ToString().Contains(TEXT("NEWTranslocatorTEX")))
			{
				M->SetMaterial(i, Skin);
				return;
			}
		}
		for (int32 i = 0; i < M->GetNumMaterials(); ++i)
		{
			const UMaterialInterface* Cur = M->GetMaterial(i);
			if (Cur && (Cur->GetName().Contains(TEXT("NEWTranslocatorTEX")) || Cur->GetName().Contains(TEXT("NEWTranslocatorBLUE"))))
			{
				M->SetMaterial(i, Skin);
				return;
			}
		}
		UE_LOG(LogOpenUT2k4, Verbose, TEXT("Translocator: no NEWTranslocatorTEX slot on %s"), *M->GetName());
	};
	ApplyTo(ViewMesh, TEXT("ranslauncher"));
	ApplyTo(TP, TEXT("ranslauncher"));
	SkinnedThirdPerson = TP;
	AppliedSkinTeam = Team;
}

bool AWeap_Translocator::ThrowOrRecall()
{
	if (!ArenaOwner)
	{
		return false;
	}
	UWorld* World = GetWorld();
	if (AArenaTransBeacon* B = Beacon.Get())
	{
		// TransFire.SpawnProjectile with a beacon out: ViewPlayer, then recall (a disrupted beacon can't be recalled)
		ViewPlayer();
		if (B->Disrupted())
		{
			PlayOwnerSound(ArenaOwner, FailedSound);
			return true;
		}
		B->Destroy();
		Beacon.Reset();
		PlaySound2D(ArenaOwner, RecallSound);
		return true;
	}
	// ProjectileFire.DoFireEffect: start at ProjSpawnOffset from the eye (pulled back from walls), TransFire.AdjustAim:
	// the controller's rotation
	SpawnMode = 0;
	FVector Dir;
	const FVector Start = GetProjectileStart(0, Dir);
	FActorSpawnParameters P;
	P.Owner = this;
	P.Instigator = ArenaOwner;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AArenaTransBeacon* NewBeacon = World->SpawnActor<AArenaTransBeacon>(AArenaTransBeacon::StaticClass(), Start, FRotator(0.f, Dir.Rotation().Yaw, 0.f), P);
	if (!NewBeacon)
	{
		return false;
	}
	FVector AimDir = Dir;
	if (AController* C = ArenaOwner->GetController())
	{
		AimDir = C->GetControlRotation().Vector();
	}
	NewBeacon->Launch(this, ArenaOwner, AimDir);
	Beacon = NewBeacon;
	UE_LOG(LogOpenUT2k4, Log, TEXT("Translocator: %s threw the beacon (charge %.2f)"), *ArenaOwner->GetName(), AmmoChargeF);
	PlaySound2D(ArenaOwner, FireSound);
	ArenaTranslocator::Stats().Throws++;
	// TransFire.PlayFiring: the fire animation only for the throw
	if (ViewMesh && Art && bViewShown && Art->FireAnim[0])
	{
		PlayViewAnim(Art->FireAnim[0], false, 0.f, 0.f);
	}
	ArenaOwner->NotifyWeaponFired(0);
	return true;
}

void AWeap_Translocator::DestroyBeacon()
{
	if (AArenaTransBeacon* B = Beacon.Get())
	{
		B->bNoAI = true;
		B->Destroy();
	}
	Beacon.Reset();
}

void AWeap_Translocator::ReduceAmmo()
{
	// TransLauncher.ReduceAmmo
	bDrained = false;
	SetCharge(AmmoChargeF - 1.f);
	if (AArenaBotController* Bot = ArenaOwner ? Cast<AArenaBotController>(ArenaOwner->GetController()) : nullptr)
	{
		Bot->NotifyTranslocated();
	}
}

void AWeap_Translocator::DrainCharges()
{
	// TransLauncher.DrainCharges
	AmmoChargeF = -1.f;
	Ammo = 0;
	bDrained = true;
	if (AArenaBotController* Bot = ArenaOwner ? Cast<AArenaBotController>(ArenaOwner->GetController()) : nullptr)
	{
		Bot->NotifyTranslocatorDrained();
	}
}

bool AWeap_Translocator::Translocate()
{
	// TransRecall.DoFireEffect / ModeDoFire
	AArenaTransBeacon* B = Beacon.Get();
	if (!ArenaOwner || !B)
	{
		UE_LOG(LogOpenUT2k4, Verbose, TEXT("Translocator: alt fire without a beacon"));
		return false;
	}
	if (B->Disrupted())
	{
		// Translocate: "translocate_gib"; ModeDoFire: ViewPlayer, Instigator.gibbedBy(TransBeacon.Disruptor)
		Beacon.Reset();
		ViewPlayer();
		AArenaCharacter* Disruptor = B->GetDisruptor();
		B->Destroy();
		ArenaTranslocator::Stats().Gibs++;
		UE_LOG(LogOpenUT2k4, Log, TEXT("Translocator: %s translocated to a disrupted beacon (disrupted by %s)"), *ArenaOwner->GetName(),
			Disruptor ? *Disruptor->GetName() : TEXT("nobody"));
		GibbedBy(ArenaOwner, Disruptor);
		return false;
	}
	const bool bMoved = DoTranslocate(B);
	// always: TransBeacon.Destroy, TransBeacon = None, ViewPlayer
	if (Beacon.Get() == B)
	{
		B->Destroy();
		Beacon.Reset();
	}
	ViewPlayer();
	return bMoved;
}

void AWeap_Translocator::GibbedBy(AArenaCharacter* Victim, AArenaCharacter* Other)
{
	// UnrealPawn.gibbedBy: a pawn holding the translocator kills with its damage type (XWeapons.DamTypeTeleFrag), other
	// pawns with Engine.DamTypeTelefragged, nobody with Engine.Gibbed
	if (!Victim || Victim->IsDead())
	{
		return;
	}
	if (Other)
	{
		const bool bTrans = Cast<AWeap_Translocator>(Other->GetCurrentWeapon()) != nullptr;
		AController* Killer = Other->GetController();
		Victim->DieGibbed(Killer, bTrans ? GetDefault<AWeap_Translocator>()->WeaponName : ArenaTranslocator::TelefraggedName());
	}
	else
	{
		Victim->DieGibbed(nullptr, ArenaTranslocator::GibbedName());
	}
}

bool AWeap_Translocator::DoTranslocate(AArenaTransBeacon* B)
{
	AArenaCharacter* Me = ArenaOwner;
	UWorld* World = GetWorld();
	const float MyHeight = Me->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const float MyRadius = Me->GetCapsuleComponent()->GetScaledCapsuleRadius();
	const FVector BeaconLoc = B->GetActorLocation();

	// Dest: the beacon raised by the pawn's height (resting) or half of it (in flight), unless that is blocked
	FVector Dest = BeaconLoc + FVector(0.f, 0.f, (B->IsResting() ? 1.f : 0.5f) * MyHeight);
	{
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaTranslocDest), false, B);
		Q.AddIgnoredActor(Me);
		if (World->LineTraceSingleByChannel(Hit, BeaconLoc, Dest, ECC_Pawn, Q))
		{
			Dest = BeaconLoc;
		}
	}
	B->SetActorEnableCollision(false);

	// a carried flag / ball is dropped with half the pawn's velocity (GameObject.Drop(0.5 x Velocity))
	const FVector MyVel = Me->GetVelocity();
	if (AArenaFlag* Flag = Me->CarriedFlag)
	{
		Flag->Drop(Me->GetController());
	}
	if (AArenaCarryable* Obj = Me->GetCarriedObject())
	{
		Obj->Throw(Me->GetActorLocation(), MyVel * 0.5f, Me->GetController());
	}
	const FVector PrevLocation = Me->GetActorLocation();

	// pawns at the destination: team mates and spawn-protected players make it fail, enemies move the spot a little
	bool bFailedTransloc = false;
	AArenaGameMode* GM = GameModeOf(this);
	const float SpawnProtection = GM && GM->GetRules() ? GM->GetRules()->GetSpawnProtectionTime() : ArenaTuning::SpawnProtectionTime;
	const float Now = World->GetTimeSeconds();
	ForEachLivePawn(World, [&](AArenaCharacter* Other)
	{
		if (bFailedTransloc || Other == Me)
		{
			return;
		}
		FVector Diff = Dest - Other->GetActorLocation();
		const float DiffZ = Diff.Z;
		Diff.Z = 0.f;
		const float OtherHeight = Other->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const float OtherRadius = Other->GetCapsuleComponent()->GetScaledCapsuleRadius();
		if (FMath::Abs(DiffZ) < OtherHeight + 2.f * MyHeight && Diff.Size() < OtherRadius + MyRadius + 8.f * UU)
		{
			if (SameTeam(Other->GetController(), Me->GetController()) || Now - Other->GetSpawnTime() < SpawnProtection)
			{
				bFailedTransloc = true;
				return;
			}
			const FVector NewDest = DiffZ > 1.5f * OtherHeight ? Dest + FVector(0.f, 0.f, 0.7f * OtherHeight)
				: Dest + 0.5f * OtherRadius * Diff.GetSafeNormal();
			FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaTranslocFast), false);
			Q.AddIgnoredActor(Me);
			if (!World->LineTraceTestByChannel(NewDest, Dest, ECC_WorldStatic, Q))
			{
				Dest = NewDest;
			}
		}
	});

	// AttemptTranslocation / TranslocSucceeded: Dest, then just behind a flying beacon, the beacon itself and four
	// spots one radius around Dest; then no blocking actor may lie between the new location and Dest
	bool bMoved = false;
	if (!bFailedTransloc)
	{
		const FVector OldLocation = Me->GetActorLocation();
		auto TryAt = [&](const FVector& L) { return SetPawnLocation(Me, L); };
		AArenaBotController* Bot = Cast<AArenaBotController>(Me->GetController());
		bool bSet = TryAt(Dest);
		if (!bSet && Bot)
		{
			// TransRecall.BotTranslocation: a bot preparing a move goes to its RealTranslocationTarget
			FVector BotDest;
			if (Bot->GetRealTranslocationTarget(BotDest))
			{
				bSet = TryAt(BotDest);
			}
		}
		if (!bSet && !B->IsResting())
		{
			bSet = TryAt(BeaconLoc - MyRadius * B->GetVelocity().GetSafeNormal());
		}
		if (!bSet && !Dest.Equals(BeaconLoc))
		{
			bSet = TryAt(BeaconLoc);
		}
		static const FVector Corners[] = { FVector(1, 1, 0), FVector(1, -1, 0), FVector(-1, 1, 0), FVector(-1, -1, 0) };
		for (int32 i = 0; i < 4 && !bSet; ++i)
		{
			bSet = TryAt(Dest + MyRadius * Corners[i]);
		}
		if (bSet)
		{
			FHitResult Hit;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaTranslocBack), false, Me);
			Q.AddIgnoredActor(B);
			const bool bBlocked = World->LineTraceSingleByChannel(Hit, Me->GetActorLocation(), Dest, ECC_Pawn, Q) && Hit.GetActor()
				&& !Cast<APawn>(Hit.GetActor());
			if (bBlocked && (Hit.Distance > 1.f))
			{
				Me->SetActorLocation(OldLocation, false, nullptr, ETeleportType::TeleportPhysics);
			}
			else
			{
				bMoved = true;
			}
		}
	}

	if (bMoved)
	{
		ReduceAmmo();
		NumTranslocations++;
		ArenaTranslocator::Stats().Translocations++;
		const int32 Team = TeamOf(Me);
		// xPawn.DoTranslocateOut at the old spot, PlayTeleportEffect at the new one
		AArenaTransEffect::Spawn(World, PrevLocation, Team, true);
		AArenaTransEffect::Spawn(World, Me->GetActorLocation() + FVector(0.f, 0.f, 0.75f * MyHeight), Team, false);
		ApplyTransOverlay(Me, Team);
		// the horizontal velocity is kept up to GroundSpeed; bots fall straight down at 150 UU/s
		UCharacterMovementComponent* CMC = Me->GetCharacterMovement();
		FVector Vel = MyVel;
		FVector Vel2D(Vel.X, Vel.Y, 0.f);
		Vel2D = Vel2D.GetSafeNormal() * FMath::Min(CMC ? CMC->MaxWalkSpeed : ArenaTuning::GroundSpeed, Vel2D.Size());
		Vel = FVector(Vel2D.X, Vel2D.Y, Vel.Z);
		const bool bWater = CMC && CMC->IsSwimming();
		if (!bWater && CMC)
		{
			if (Cast<AArenaBotController>(Me->GetController()))
			{
				Vel = FVector(0.f, 0.f, -150.f * UU);
			}
			CMC->SetMovementMode(MOVE_Falling);
		}
		if (CMC)
		{
			CMC->Velocity = Vel;
		}
		// telefrags: enemies the pawn now overlaps (Pawn.EncroachedBy -> gibbedBy the translocating pawn)
		const FVector MyLoc = Me->GetActorLocation();
		ForEachLivePawn(World, [&](AArenaCharacter* Other)
		{
			if (Other == Me)
			{
				return;
			}
			const FVector D = Other->GetActorLocation() - MyLoc;
			const float OtherHeight = Other->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			const float OtherRadius = Other->GetCapsuleComponent()->GetScaledCapsuleRadius();
			if (FMath::Abs(D.Z) < OtherHeight + MyHeight && FVector(D.X, D.Y, 0.f).Size() < OtherRadius + MyRadius)
			{
				ArenaTranslocator::Stats().Telefrags++;
				UE_LOG(LogOpenUT2k4, Log, TEXT("Translocator: %s telefragged %s"), *Me->GetName(), *Other->GetName());
				GibbedBy(Other, Me);
			}
		});
		UE_LOG(LogOpenUT2k4, Log, TEXT("Translocator: %s%s translocated %.0f cm (charge %.2f)"), Cast<AArenaBotController>(Me->GetController()) ? TEXT("bot ") : TEXT(""),
			*Me->GetName(), FVector::Dist(PrevLocation, MyLoc), AmmoChargeF);
	}
	else
	{
		ArenaTranslocator::Stats().Failed++;
		PlayOwnerSound(Me, FailedSound);
		UE_LOG(LogOpenUT2k4, Log, TEXT("Translocator: %s could not translocate to %s (beacon %s%s) (%s)"), *Me->GetName(), *Dest.ToCompactString(),
			*BeaconLoc.ToCompactString(), B->IsResting() ? TEXT(" resting") : TEXT(""),
			bFailedTransloc ? TEXT("a team mate or spawn protected player is there") : *(TEXT("no room: ") + LastBlocker()));
	}
	return bMoved;
}

bool AWeap_Translocator::IsViewingBeacon() const
{
	const APlayerController* PC = ArenaOwner ? Cast<APlayerController>(ArenaOwner->GetController()) : nullptr;
	return PC && Beacon.IsValid() && PC->GetViewTarget() == Beacon.Get();
}

void AWeap_Translocator::ViewCamera()
{
	APlayerController* PC = ArenaOwner ? Cast<APlayerController>(ArenaOwner->GetController()) : nullptr;
	AArenaTransBeacon* B = Beacon.Get();
	if (PC && B)
	{
		PC->SetViewTarget(B);
		B->SetCameraView(true);
	}
}

void AWeap_Translocator::ViewPlayer()
{
	APlayerController* PC = ArenaOwner ? Cast<APlayerController>(ArenaOwner->GetController()) : nullptr;
	if (PC && Cast<AArenaTransBeacon>(PC->GetViewTarget()))
	{
		if (AArenaTransBeacon* B = Cast<AArenaTransBeacon>(PC->GetViewTarget()))
		{
			B->SetCameraView(false);
		}
		PC->SetViewTarget(ArenaOwner);
	}
}

void AWeap_Translocator::Reselect()
{
	// TransLauncher.Reselect: no beacon or already looking through it -> the player's view, else the beacon's
	if (!Beacon.IsValid() || IsViewingBeacon())
	{
		ViewPlayer();
	}
	else
	{
		ViewCamera();
	}
}

float AWeap_Translocator::GetBotRating(float DistanceToEnemy) const
{
	// TransLauncher.GetAIRating: 10 while a bot prepares a translocation, else AIRating -1
	return bBotPreparing ? 10.f : -1.f;
}

AArenaTransBeacon* AWeap_Translocator::BotThrow(const FVector& TargetLoc, AActor* MonitorTarget, const FVector& InMonitorLoc, float InMonitorRadius,
	float InMonitorHeight, bool bGameObjectTarget)
{
	// TransLauncher.FireHack(0): an old beacon goes (no AI callbacks), the new one starts at the pawn and is thrown with
	// AdjustToss(TransBeacon.Speed, its location, the target, false)
	if (!ArenaOwner || !bEquipped)
	{
		return nullptr;
	}
	DestroyBeacon();
	UWorld* World = GetWorld();
	FActorSpawnParameters P;
	P.Owner = this;
	P.Instigator = ArenaOwner;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const FVector Start = ArenaOwner->GetActorLocation();
	AArenaTransBeacon* NewBeacon = World->SpawnActor<AArenaTransBeacon>(AArenaTransBeacon::StaticClass(), Start, FRotator::ZeroRotator, P);
	if (!NewBeacon)
	{
		return nullptr;
	}
	const FVector Vel = ArenaTranslocator::AdjustToss(ArenaTranslocator::BeaconSpeedUU * UU, Start, TargetLoc, ArenaTranslocator::GravityZAt(World, Start), false);
	NewBeacon->Launch(this, ArenaOwner, Vel.GetSafeNormal(), Vel);
	Beacon = NewBeacon;
	NewBeacon->SetTranslocationTarget(MonitorTarget, InMonitorLoc, InMonitorRadius, InMonitorHeight, bGameObjectTarget, bBotPreparing);
	PlaySound2D(ArenaOwner, FireSound);
	ArenaOwner->NotifyWeaponFired(0);
	ArenaTranslocator::Stats().BotThrows++;
	ArenaTranslocator::Stats().Throws++;
	return NewBeacon;
}

void AWeap_Translocator::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// TransLauncher.Destroyed: the beacon goes with the launcher
	ViewPlayer();
	DestroyBeacon();
	Super::EndPlay(EndPlayReason);
}

// =================================================================================================
// Beacon (TransBeacon)
// =================================================================================================

AArenaTransBeacon::AArenaTransBeacon()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	Collision->InitSphereRadius(ArenaTranslocator::BeaconRadiusUU * UU);
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Collision->SetCollisionObjectType(ECC_WorldDynamic);
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Collision->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	// bProjTarget: shots hit it (weapon traces, projectiles, radius damage)
	Collision->SetCollisionResponseToChannel(ECC_Weapon, ECR_Block);
	Collision->SetCollisionResponseToChannel(ECC_Projectile, ECR_Block);
	Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Collision->SetGenerateOverlapEvents(false);
	Collision->SetCanEverAffectNavigation(false);
	Collision->SetShouldUpdatePhysicsVolume(false);
	RootComponent = Collision;
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(Collision);
	Light->SetIntensity(300.f);
	Light->SetAttenuationRadius(140.f);
	Light->SetCastShadows(false);
	SetCanBeDamaged(true);
	// weapon traces only hit actors carrying the shootable tag (ArenaWeaponActors)
	Tags.Add(FName(TEXT("ArenaShootable")));
	Tags.Add(ArenaNavIgnoreTag());
}

void AArenaTransBeacon::Launch(AWeap_Translocator* InLauncher, AArenaCharacter* InOwner, const FVector& Dir, const FVector& InVelocity)
{
	Launcher = InLauncher;
	OwnerPawn = InOwner;
	OwnerController = InOwner ? InOwner->GetController() : nullptr;
	Team = TeamOf(InOwner);
	TeamColor = TeamTrailColor(Team);
	// TransBeacon.PostBeginPlay: Velocity = Speed x Vector(Rotation) (bHighBeaconTrajectory off), rotation yaw only
	Velocity = InVelocity.IsNearlyZero() ? Dir.GetSafeNormal() * ArenaTranslocator::BeaconSpeedUU * UU : InVelocity;
	SetActorRotation(FRotator(0.f, Dir.Rotation().Yaw, 0.f));
	LaunchLocation = GetActorLocation();
	LaunchVelocity = Velocity;
	LaunchTime = LastMoveTime = GetWorld()->GetTimeSeconds();
	bCanHitOwner = false;
	if (InOwner)
	{
		// Projectile.Instigator: no touch with the owner until the first bounce
		Collision->IgnoreActorWhenMoving(InOwner, true);
	}
	// the puck: WeaponStaticMesh.NEWTranslocatorPUCK, DrawScale 0.35, PrePivot (0, 0, 25) UU
	if (UStaticMesh* M = ArenaArt::Load<UStaticMesh>(TEXT("StaticMeshes/WeaponStaticMesh/NEWTranslocatorPUCK")))
	{
		Mesh = NewObject<UStaticMeshComponent>(this, TEXT("Puck"));
		Mesh->SetStaticMesh(M);
		Mesh->SetupAttachment(Collision);
		Mesh->SetRelativeScale3D(FVector(0.35f));
		Mesh->SetRelativeLocation(FVector(0.f, 0.f, -25.f * UU * 0.35f));
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetCastShadow(true);
		Mesh->RegisterComponent();
	}
	else
	{
		Mesh = NewObject<UStaticMeshComponent>(this, TEXT("Puck"));
		Mesh->SetStaticMesh(ArenaAssets::Cylinder());
		Mesh->SetupAttachment(Collision);
		Mesh->SetRelativeScale3D(FVector(0.4f, 0.4f, 0.1f));
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		if (UMaterialInstanceDynamic* MID = ArenaAssets::MakeGlow(this, TeamColor, 3.f, 1.f))
		{
			Mesh->SetMaterial(0, MID);
		}
		Mesh->RegisterComponent();
	}
	Light->SetLightColor(TeamColor);
	// AmbientSound redeemer_flight, SoundVolume 250, SoundRadius 7 (heard close by; ViewBeaconVolume 40 in the camera)
	if (USoundBase* S = ArenaArt::LoadOneShotSound(AmbientSoundPath))
	{
		Ambient = UGameplayStatics::SpawnSoundAttached(S, Collision, NAME_None, FVector::ZeroVector, EAttachLocation::KeepRelativeOffset, true,
			250.f / 255.f * 0.5f, 1.f, 0.f, nullptr, nullptr, true);
	}
	UE_LOG(LogOpenUT2k4, Verbose, TEXT("Translocator: beacon thrown at %s, %.0f cm/s"), *Velocity.ToCompactString(), Velocity.Size());
}

void AArenaTransBeacon::SetCameraView(bool bViewing)
{
	if (Ambient)
	{
		Ambient->SetVolumeMultiplier(bViewing ? 40.f / 255.f : 250.f / 255.f * 0.5f);
	}
}

void AArenaTransBeacon::SetTranslocationTarget(AActor* Target, const FVector& TargetLoc, float Radius, float HalfHeight, bool bGameObject, bool bPreparingMove)
{
	MonitorActor = Target;
	MonitorLoc = TargetLoc;
	MonitorRadius = Radius;
	MonitorHeight = HalfHeight;
	bMonitorGameObject = bGameObject;
	bMonitorPreparing = bPreparingMove;
	bMonitoring = true;
}

void AArenaTransBeacon::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();

	// TransBeacon.Timer every 0.3 s: sparks once disrupted (the flare goes)
	if (Now >= NextSparkCheck)
	{
		NextSparkCheck = Now + 0.3f;
		if (Disrupted() && !bSparks)
		{
			bSparks = true;
			ArenaArtFX::FSpriteDesc S;
			S.Texture = TEXT("Textures/XEffects/Skins_pcl_ball");
			S.Tint = FLinearColor(1.f, 0.8f, 0.4f);
			S.Intensity = 3.f;
			S.Size = FVector2D(40.f, 40.f);
			S.LifeTime = 0.3f;
			S.bLoop = true;
			SparkGlow = NewObject<UArenaSpriteComponent>(this);
			SparkGlow->SetupAttachment(Collision);
			SparkGlow->SetRelativeLocation(FVector(0.f, 0.f, 10.f));
			if (SparkGlow->SetupSprite(S))
			{
				SparkGlow->RegisterComponent();
			}
			if (Flare)
			{
				Flare->DestroyComponent();
				Flare = nullptr;
			}
			PlaySound2D(this, TEXT("Sounds/WeaponSounds/BaseImpactAndExplosions_BElectric1"), 0.5f);
		}
	}

	// the owner died or left: the launcher went with it
	if (!Launcher.IsValid() && !bMonitoring)
	{
		Destroy();
		return;
	}

	if (!bResting)
	{
		// PHYS_Falling in the physics volume at the beacon, sub-stepped from where the last tick left it (a beacon spawned
		// during this frame's tick group starts at its spawn time)
		float Remaining = FMath::Clamp(Now - LastMoveTime, 0.f, 0.1f);
		LastMoveTime = Now - Remaining;
		int32 Guard = 0;
		while (Remaining > KINDA_SMALL_NUMBER && !bResting && Guard++ < 8)
		{
			const float Dt = FMath::Min(Remaining, 1.f / 60.f);
			Remaining -= Dt;
			// UE2 physFalling: the move uses the average of the old and new velocity
			const FVector OldVelocity = Velocity;
			Velocity.Z += ArenaTranslocator::GravityZAt(World, GetActorLocation()) * Dt;
			Velocity.Z = FMath::Max(Velocity.Z, -ArenaWorldZones::GetTerminalVelocityAt(World, GetActorLocation())); // PhysicsVolume.TerminalVelocity
			FHitResult Hit;
			const FVector From = GetActorLocation();
			SetActorLocation(From + 0.5f * (OldVelocity + Velocity) * Dt, true, &Hit);
			LastMoveTime += Dt;
			if (Hit.bBlockingHit)
			{
				AActor* Wall = Hit.GetActor();
				const bool bWorld = !Wall || !Cast<AArenaMover>(Wall);
				if (Hit.bStartPenetrating)
				{
					SetActorLocation(From + Hit.Normal * (Hit.PenetrationDepth + 0.5f), false);
				}
				HitWall(Hit.ImpactNormal, Wall, bWorld);
				if (IsActorBeingDestroyed())
				{
					return;
				}
			}
		}
		if (!bResting && Now >= NextPuffTime)
		{
			NextPuffTime = Now + 0.05f; // TransTrail mRegenRange 20 per second
			SpawnTrailPuff();
		}
	}
	else
	{
		// a mover pushing into it destroys it (TransBeacon.EncroachedBy)
		TArray<FOverlapResult> Ov;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaBeaconMover), false, this);
		if (World->OverlapMultiByObjectType(Ov, GetActorLocation(), FQuat::Identity, FCollisionObjectQueryParams(ECC_WorldDynamic),
			FCollisionShape::MakeSphere(Collision->GetScaledSphereRadius() * 0.8f), Q))
		{
			for (const FOverlapResult& O : Ov)
			{
				if (Cast<AArenaMover>(O.GetActor()))
				{
					Destroy();
					return;
				}
			}
		}
		// no floor any more (a lift moved away): fall again
		FHitResult Floor;
		if (!World->SweepSingleByObjectType(Floor, GetActorLocation(), GetActorLocation() - FVector(0.f, 0.f, 4.f), FQuat::Identity,
			FCollisionObjectQueryParams(FCollisionObjectQueryParams::AllStaticObjects), FCollisionShape::MakeSphere(Collision->GetScaledSphereRadius()), Q))
		{
			bResting = false;
		}
	}
	UpdateFlare(DeltaSeconds);

	// Projectile.Touch -> ProcessTouch with pawns
	{
		TArray<FOverlapResult> Ov;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaBeaconTouch), false, this);
		TSet<TWeakObjectPtr<AActor>> Now2;
		if (World->OverlapMultiByChannel(Ov, GetActorLocation(), FQuat::Identity, ECC_Pawn, FCollisionShape::MakeSphere(Collision->GetScaledSphereRadius()), Q))
		{
			for (const FOverlapResult& O : Ov)
			{
				APawn* P = Cast<APawn>(O.GetActor());
				if (!P || (Cast<AArenaCharacter>(P) && Cast<AArenaCharacter>(P)->IsDead()))
				{
					continue;
				}
				Now2.Add(P);
				if (!TouchingPawns.Contains(P))
				{
					TouchPawn(P);
					if (IsActorBeingDestroyed())
					{
						return;
					}
				}
			}
		}
		TouchingPawns = MoveTemp(Now2);
	}

	if (bMonitoring)
	{
		MonitorTick();
	}
}

void AArenaTransBeacon::HitWall(const FVector& HitNormal, AActor* Wall, bool bWorld)
{
	// TransBeacon.HitWall
	bCanHitOwner = true;
	if (AArenaCharacter* O = OwnerPawn.Get())
	{
		Collision->IgnoreActorWhenMoving(O, false);
	}
	++NumBounces;
	Velocity = 0.3f * (FVector::DotProduct(Velocity, HitNormal) * HitNormal * -2.f + Velocity);
	float Speed = Velocity.Size();
	if (Speed < 100.f * UU)
	{
		// resting on a flag base is not allowed: it keeps hopping and gets disrupted (TouchingActors(CTFBase))
		for (TActorIterator<AArenaFlag> It(GetWorld()); It; ++It)
		{
			const FVector Home = It->GetHomeLocation();
			if (FVector::Dist2D(Home, GetActorLocation()) < 60.f * UU + Collision->GetScaledSphereRadius()
				&& FMath::Abs(Home.Z - GetActorLocation().Z) < 60.f * UU + Collision->GetScaledSphereRadius())
			{
				Speed = 90.f * UU;
				Velocity = Speed * Velocity.GetSafeNormal();
				Disruption += 5.f;
				if (!Disruptor.IsValid())
				{
					Disruptor = OwnerPawn;
				}
				break;
			}
		}
	}
	if (Speed < 20.f * UU && bWorld && HitNormal.Z >= 0.7f)
	{
		PlaySound2D(this, ImpactSound, 0.8f);
		bResting = true;
		Velocity = FVector::ZeroVector;
		if (!Flare && !Disrupted())
		{
			ArenaArtFX::FSpriteDesc S;
			S.Texture = TEXT("Textures/XEffects/Skins_pcl_ball");
			S.Tint = TeamFlareColor(Team);
			S.Intensity = 2.5f;
			S.Size = FVector2D(6.f * UU * 2.f, 6.f * UU * 2.f);
			Flare = NewObject<UArenaSpriteComponent>(this);
			Flare->SetupAttachment(Collision);
			Flare->SetRelativeLocation(FVector(0.f, 0.f, 10.f * UU * 0.5f));
			if (Flare->SetupSprite(S))
			{
				Flare->RegisterComponent();
			}
			else
			{
				Flare = nullptr;
			}
		}
	}
	if (bMonitoring)
	{
		MonitorHitWall(HitNormal);
	}
}

void AArenaTransBeacon::TouchPawn(APawn* Other)
{
	// MonitoringThrow.Touch: not the owner; a touched enemy bot may make the owner translocate (into it)
	AArenaCharacter* OwnerC = OwnerPawn.Get();
	if (bMonitoring && Other == OwnerC)
	{
		return;
	}
	// TransBeacon.ProcessTouch: the owner walking over the resting beacon picks it up
	if (Other == OwnerC && bResting)
	{
		Destroy();
		return;
	}
	if (Other != OwnerC || bCanHitOwner)
	{
		if (!Cast<AArenaONS_Vehicle>(Other))
		{
			const FVector Vel2D(Velocity.X, Velocity.Y, 0.f);
			if (Vel2D.Size() < 200.f * UU)
			{
				return;
			}
		}
		HitWall(-Velocity.GetSafeNormal(), Other, false);
	}
	if (bMonitoring && OwnerC)
	{
		AArenaBotController* TouchedBot = Cast<AArenaBotController>(Other->GetController());
		if (!TouchedBot || SameTeam(Other->GetController(), OwnerC->GetController()))
		{
			EndMonitoring(false, 0.f);
			return;
		}
		// Bot(P.Controller).ProficientWithWeapon() && 2 + 8 x FRand < Bot(P.Controller).Skill
		FRandomStream& R = TouchedBot->GetRandomStream();
		const bool bProficient = TouchedBot->GetSkill() > 2.f + R.FRand() * 4.f;
		if (bProficient && 2.f + 8.f * R.FRand() < TouchedBot->GetSkill())
		{
			BotTranslocate();
		}
	}
}

void AArenaTransBeacon::MonitorTick()
{
	// MonitoringThrow.Tick
	AArenaCharacter* OwnerC = OwnerPawn.Get();
	AArenaBotController* Bot = OwnerC ? Cast<AArenaBotController>(OwnerC->GetController()) : nullptr;
	if (!Bot || !Bot->IsTranslocationTargetValid(this))
	{
		EndMonitoring(false, 0.f);
		return;
	}
	if (AActor* A = MonitorActor.Get())
	{
		MonitorLoc = A->GetActorLocation();
	}
	FVector Dist = GetActorLocation() - MonitorLoc;
	const float ZDiff = Dist.Z;
	Dist.Z = 0.f;
	const float Dist2D = Dist.Size();
	if (Dist2D < MonitorRadius)
	{
		if (ZDiff > -0.9f * MonitorHeight)
		{
			BotTranslocate();
		}
		return;
	}
	FVector Dir = MonitorLoc - OwnerC->GetActorLocation();
	Dir.Z = 0.f;
	if (FVector::DotProduct(Dist, Dir) > 0.f)
	{
		// went past the target
		if (bMonitorPreparing)
		{
			EndMonitoring(true, 400.f);
			return;
		}
		if (bMonitorGameObject && ZDiff > 0.f && Dist2D < FMath::Min(400.f * UU, FVector::Dist(OwnerC->GetActorLocation(), MonitorLoc) - 250.f * UU))
		{
			FHitResult Hit;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaBeaconDown), false, this);
			if (GetWorld()->LineTraceSingleByChannel(Hit, GetActorLocation(), GetActorLocation() - FVector(0.f, 0.f, 100.f * UU), ECC_WorldStatic, Q)
				&& Hit.ImpactNormal.Z > 0.7f)
			{
				BotTranslocate();
				return;
			}
		}
		EndMonitoring(false, 0.f);
	}
}

void AArenaTransBeacon::MonitorHitWall(const FVector& HitNormal)
{
	// MonitoringThrow.HitWall
	AArenaCharacter* OwnerC = OwnerPawn.Get();
	if (!OwnerC)
	{
		return;
	}
	if (bMonitorGameObject && HitNormal.Z > 0.7f
		&& FVector::Dist(GetActorLocation(), MonitorLoc) < FMath::Min(400.f * UU, FVector::Dist(OwnerC->GetActorLocation(), MonitorLoc)))
	{
		BotTranslocate();
		return;
	}
	if (bResting)
	{
		EndMonitoring(bMonitorPreparing, bMonitorPreparing ? 900.f : 0.f);
	}
}

void AArenaTransBeacon::BotTranslocate()
{
	// MonitoringThrow.BotTranslocate: TransRecall.DoFireEffect, then EndMonitoring
	bMonitoring = false;
	AWeap_Translocator* L = Launcher.Get();
	AArenaCharacter* OwnerC = OwnerPawn.Get();
	AArenaBotController* Bot = OwnerC ? Cast<AArenaBotController>(OwnerC->GetController()) : nullptr;
	if (Bot && !Bot->IsSafeTranslocation(GetActorLocation()))
	{
		Bot->OnBeaconMonitorEnd(this, false, 0.f, false);
		return;
	}
	if (L && OwnerC && OwnerC->GetCurrentWeapon() == L)
	{
		const bool bMoved = L->Translocate();
		if (bMoved)
		{
			ArenaTranslocator::Stats().BotTranslocations++;
		}
		if (Bot)
		{
			Bot->OnBeaconMonitorEnd(this, !bMoved, 0.f, bMoved);
		}
		return;
	}
	if (Bot)
	{
		Bot->OnBeaconMonitorEnd(this, false, 0.f, false);
	}
}

void AArenaTransBeacon::EndMonitoring(bool bFailedMove, float FearCost)
{
	// MonitoringThrow.EndState: the bot's TranslocationTarget / bPreparingMove are cleared, it picks its best weapon
	if (!bMonitoring)
	{
		return;
	}
	bMonitoring = false;
	AArenaCharacter* OwnerC = OwnerPawn.Get();
	if (AArenaBotController* Bot = OwnerC && !bNoAI ? Cast<AArenaBotController>(OwnerC->GetController()) : nullptr)
	{
		Bot->OnBeaconMonitorEnd(this, bFailedMove, FearCost, false);
	}
}

float AArenaTransBeacon::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	// TransBeacon.TakeDamage: in team games the owner's team (the owner too) can't disrupt it
	if (DamageAmount <= 0.f)
	{
		return 0.f;
	}
	const AArenaGameMode* GM = GameModeOf(this);
	const AController* OwnerC = OwnerController.Get();
	if (GM && GM->IsTeamGame() && EventInstigator && EventInstigator->GetPlayerState<APlayerState>()
		&& (!OwnerC || SameTeam(EventInstigator, OwnerC)))
	{
		return 0.f;
	}
	Disruption += DamageAmount;
	Disruptor = EventInstigator ? Cast<AArenaCharacter>(EventInstigator->GetPawn()) : nullptr;
	UE_LOG(LogOpenUT2k4, Log, TEXT("Translocator: beacon of %s took %.0f damage (disruption %.0f of %.0f)"), OwnerPawn.IsValid() ? *OwnerPawn->GetName() : TEXT("?"),
		DamageAmount, Disruption, ArenaTranslocator::DisruptionThreshold);
	return DamageAmount;
}

void AArenaTransBeacon::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
	// PlayerController.PlayerCalcView with the beacon as ViewTarget: its location, the controller's rotation
	OutResult.Location = GetActorLocation();
	const AController* C = OwnerController.Get();
	OutResult.Rotation = C ? C->GetControlRotation() : GetActorRotation();
	const AArenaCharacter* O = OwnerPawn.Get();
	OutResult.FOV = O && O->Camera ? O->Camera->FieldOfView : 90.f;
	if (O && O->Camera)
	{
		OutResult.AspectRatio = O->Camera->AspectRatio;
		OutResult.bConstrainAspectRatio = false;
	}
}

void AArenaTransBeacon::SpawnTrailPuff()
{
	// TransTrail (xEmitter, TransTrailT, additive): puffs of 9 UU growing 6 UU/s over 1.5 s in the team colour
	if (Puffs.Num() < 30)
	{
		UArenaSpriteComponent* P = NewObject<UArenaSpriteComponent>(this);
		ArenaArtFX::FSpriteDesc S;
		S.Texture = TEXT("Textures/XEffects/Skins_TransTrailT");
		S.Tint = TeamColor;
		S.Intensity = 1.2f;
		S.Size = FVector2D(18.f * UU, 18.f * UU);
		S.EndScale = (9.f + 6.f * 1.5f) / 9.f;
		S.LifeTime = 1.5f;
		S.FadePower = 1.f;
		if (!P->SetupSprite(S))
		{
			return;
		}
		P->SetUsingAbsoluteLocation(true);
		P->SetUsingAbsoluteRotation(true);
		P->SetupAttachment(Collision);
		P->RegisterComponent();
		Puffs.Add(P);
	}
	UArenaSpriteComponent* P = Puffs[NextPuff % Puffs.Num()];
	NextPuff++;
	if (P)
	{
		P->SetWorldLocation(GetActorLocation());
		P->Restart();
	}
}

void AArenaTransBeacon::UpdateFlare(float DeltaSeconds)
{
	if (Light)
	{
		Light->SetIntensity(Disrupted() ? 0.f : 300.f);
	}
}

void AArenaTransBeacon::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (bMonitoring)
	{
		// MonitoringThrow.Destroyed: the bot's translocation state is cleared
		bMonitoring = false;
		AArenaCharacter* OwnerC = OwnerPawn.Get();
		if (AArenaBotController* Bot = OwnerC && !bNoAI ? Cast<AArenaBotController>(OwnerC->GetController()) : nullptr)
		{
			Bot->OnBeaconMonitorEnd(this, false, 0.f, false);
		}
	}
	// the camera falls back to the owner
	if (UWorld* World = GetWorld())
	{
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = It->Get();
			if (PC && PC->GetViewTarget() == this && PC->GetPawn())
			{
				PC->SetViewTarget(PC->GetPawn());
			}
		}
	}
	Super::EndPlay(EndPlayReason);
}

// =================================================================================================
// Effects
// =================================================================================================

AArenaTransEffect::AArenaTransEffect()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	InitialLifeSpan = 1.2f;
}

void AArenaTransEffect::Spawn(UWorld* World, const FVector& Location, int32 Team, bool bOut)
{
	if (!World || World->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	FActorSpawnParameters P;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AArenaTransEffect* FX = World->SpawnActor<AArenaTransEffect>(AArenaTransEffect::StaticClass(), Location, FRotator::ZeroRotator, P);
	if (!FX)
	{
		return;
	}
	// xPawn.TransEffects (NewTransEffect / NewTransEffectBlue) and TransOutEffect (NewTransDeresRed / Blue): a team ring
	// flash where the pawn arrives, a fading column where it left
	const FLinearColor Col = Team == 1 ? FLinearColor(0.3f, 0.5f, 1.f) : FLinearColor(1.f, 0.25f, 0.2f);
	auto Add = [FX](const ArenaArtFX::FSpriteDesc& S)
	{
		UArenaSpriteComponent* C = NewObject<UArenaSpriteComponent>(FX);
		C->SetupAttachment(FX->GetRootComponent());
		if (C->SetupSprite(S))
		{
			C->RegisterComponent();
		}
	};
	ArenaArtFX::FSpriteDesc Ring;
	Ring.Texture = Team == 1 ? TEXT("Textures/XGameShaders/Trans_TransRingEnergy") : TEXT("Textures/xGameShadersB/TransB_TransRingEnergyRed");
	Ring.Facing = ArenaArtFX::EFacing::Fixed;
	Ring.Tint = Col;
	Ring.Intensity = 2.f;
	Ring.Size = FVector2D(bOut ? 120.f : 160.f, bOut ? 120.f : 160.f);
	Ring.EndScale = bOut ? 0.3f : 1.8f;
	Ring.LifeTime = 0.6f;
	Add(Ring);
	ArenaArtFX::FSpriteDesc Glow;
	Glow.Texture = TEXT("Textures/XEffects/Skins_pcl_ball");
	Glow.Tint = Col;
	Glow.Intensity = 2.f;
	Glow.Size = FVector2D(bOut ? 140.f : 200.f, bOut ? 220.f : 260.f);
	Glow.EndScale = bOut ? 0.4f : 0.2f;
	Glow.LifeTime = bOut ? 0.8f : 0.5f;
	Add(Glow);
}

// =================================================================================================
// Screenshot hook
// =================================================================================================

bool UArenaTranslocShotSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
#if UE_BUILD_SHIPPING
	return false;
#else
	return FParse::Param(FCommandLine::Get(), TEXT("TranslocShots"));
#endif
}

bool UArenaTranslocShotSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UArenaTranslocShotSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UArenaTranslocShotSubsystem, STATGROUP_Tickables);
}

void UArenaTranslocShotSubsystem::Tick(float DeltaTime)
{
#if !UE_BUILD_SHIPPING
	UWorld* World = GetWorld();
	AArenaGameMode* GM = World ? World->GetAuthGameMode<AArenaGameMode>() : nullptr;
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	AArenaCharacter* C = PC ? Cast<AArenaCharacter>(PC->GetPawn()) : nullptr;
	if (!GM || !GM->IsPlaying() || (!C && Step < 9))
	{
		return;
	}
	const float Now = World->GetTimeSeconds();
	if (Start < 0.f)
	{
		Start = Now;
	}
	const float T = Now - Start;
	auto Shot = [](const TCHAR* Name)
	{
		FScreenshotRequest::RequestScreenshot(FString::Printf(TEXT("transloc_%s"), Name), false, false);
		UE_LOG(LogOpenUT2k4, Log, TEXT("TranslocShots: %s"), Name);
	};
	AWeap_Translocator* W = C ? Cast<AWeap_Translocator>(C->FindWeapon(AWeap_Translocator::StaticClass())) : nullptr;
	static const float Times[] = { 3.f, 4.5f, 5.f, 5.3f, 7.f, 7.2f, 8.2f, 8.5f, 9.6f, 10.f, 10.7f, 11.f, 11.6f, 11.8f, 13.f, 13.4f, 14.f };
	if (Step >= UE_ARRAY_COUNT(Times) || T < Times[Step])
	{
		return;
	}
	switch (Step)
	{
	case 0:
		C->GiveWeapon(AWeap_Translocator::StaticClass());
		if (AArenaWeapon* NewW = C->FindWeapon(AWeap_Translocator::StaticClass()))
		{
			C->SwitchToWeapon(NewW);
		}
		break;
	case 1: Shot(TEXT("firstperson")); break;
	case 2:
		PC->SetControlRotation(FRotator(-12.f, PC->GetControlRotation().Yaw, 0.f));
		C->StartFire(0);
		C->StopFire(0);
		break;
	case 3: Shot(TEXT("beacon_flight")); break;
	case 4: Shot(TEXT("beacon_rest")); break;
	case 5: if (W) { W->Reselect(); } break;
	case 6: Shot(TEXT("beacon_camera")); break;
	case 7:
		if (W) { W->Reselect(); }
		if (IConsoleVariable* CV = IConsoleManager::Get().FindConsoleVariable(TEXT("arena.BehindView")))
		{
			CV->Set(1, ECVF_SetByCode);
		}
		break;
	case 8: Shot(TEXT("thirdperson")); break;
	case 9:
		if (C)
		{
			C->StartFire(1);
			C->StopFire(1);
		}
		break;
	case 10: Shot(TEXT("after_translocate")); break;
	case 11:
		// a camera in front of the player, to the right: the third-person translocator in the hand and the overlay
		if (C)
		{
			const FRotator Yaw(0.f, C->GetActorRotation().Yaw, 0.f);
			const FVector Eye = C->GetActorLocation() + Yaw.RotateVector(FVector(170.f, 110.f, 40.f));
			FActorSpawnParameters SP;
			SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			if (ACameraActor* Cam = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), Eye, (C->GetActorLocation() + FVector(0.f, 0.f, 10.f) - Eye).Rotation(), SP))
			{
				PC->SetViewTarget(Cam);
			}
		}
		break;
	case 12: Shot(TEXT("thirdperson_front")); break;
	case 13:
		if (C)
		{
			C->StartFire(0);
			C->StopFire(0);
		}
		break;
	case 14:
		if (AArenaTransBeacon* B = W ? W->GetBeacon() : nullptr)
		{
			const FVector At = B->GetActorLocation();
			FActorSpawnParameters SP;
			SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			if (ACameraActor* Cam = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), At + FVector(120.f, 0.f, 70.f), (At - (At + FVector(120.f, 0.f, 70.f))).Rotation(), SP))
			{
				PC->SetViewTarget(Cam);
			}
		}
		break;
	case 15: Shot(TEXT("beacon_closeup")); break;
	case 16:
		UE_LOG(LogOpenUT2k4, Log, TEXT("TranslocShots: done (%d throws, %d translocations)"), ArenaTranslocator::Stats().Throws, ArenaTranslocator::Stats().Translocations);
		FPlatformMisc::RequestExit(false);
		break;
	}
	++Step;
#endif
}
