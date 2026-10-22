#include "UE2NetActors.h"
#include "../ArenaHitchLog.h"

#include "../ArenaArt.h"
#include "../ArenaArtFX.h"
#include "../ArenaAssets.h"
#include "../ArenaCharacterAnim.h"
#include "../ArenaContent.h"
#include "../ArenaFX.h"
#include "../ArenaONS_Vehicle.h"
#include "../ArenaPlayerState.h"
#include "../ArenaRagdoll.h"
#include "../ArenaRuntimeContent.h"
#include "../ArenaTypes.h"
#include "../ArenaUE2World.h"
#include "../ArenaWeapons.h"
#include "../ArenaWeaponsExtra.h"
#include "../ArenaWorldZones.h"
#include "../ArenaBR_BallLauncher.h"
#include "../ArenaTranslocator.h"
#include "../UScript/UScriptCore.h"
#include "../UScript/UScriptRuntime.h"
#include "Camera/CameraTypes.h"
#include "Animation/AnimSequence.h"
#include "Components/AudioComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Sound/SoundBase.h"
#include "UE2NetLog.h"
#include "UE2NetModWeapons.h"

bool AUE2NetPawn::bBehindView = true;
int32 AUE2NetProjectile::NumEnded = 0;
double AUE2NetProjectile::SumDistance = 0.0;
int32 AUE2NetPawn::NumCorrections = 0;
double AUE2NetPawn::SumCorrection = 0.0;
float AUE2NetPawn::MaxCorrection = 0.f;

namespace UE2NetActorsLocal
{
	/** UE2 units -> centimetres. */
	constexpr float S = ArenaUE2World::Scale;
	/** UE2 gravity (-950 UU/s^2) in cm/s^2. */
	constexpr float Gravity = -950.f * S;
	/** The player models' feet below the mesh origin (ArenaCharacter.cpp ModelFeetZ). */
	constexpr float ModelFeetZ = -75.5f;

	/** A UT2004 character (System upl player list "Player=" record): mesh and body skin. */
	struct FCharRecord
	{
		FString Mesh;
		FString BodySkin;
		FString FaceSkin;
	};

	/**
	 * Object path ("Pkg.Group.Name") of a skin named without its group, as UPL records name them
	 * ("UT2004ECEPlayerSkins.MSkaarj_Body" is MechSkaarjSkins.MSkaarj_Body), from the content cache's materials /
	 * textures folders; empty when the cache has none.
	 */
	FString FindSkinObjectPath(const FString& Pkg, const FString& Name)
	{
		static TMap<FString, TMap<FString, FString>> Index;
		TMap<FString, FString>* Names = Index.Find(Pkg.ToLower());
		if (!Names)
		{
			Names = &Index.Add(Pkg.ToLower());
			for (const TCHAR* Kind : { TEXT("materials"), TEXT("textures") })
			{
				TArray<FString> Files;
				IFileManager::Get().FindFiles(Files, *(ArenaContent::GetCacheDir() / Kind / Pkg / TEXT("*.json")), true, false);
				for (const FString& F : Files)
				{
					const FString Stem = FPaths::GetBaseFilename(F);
					FString Short = Stem;
					int32 Dot = INDEX_NONE;
					if (Short.FindLastChar(TEXT('.'), Dot))
					{
						Short.RightChopInline(Dot + 1);
					}
					if (!Names->Contains(Short.ToLower()))
					{
						Names->Add(Short.ToLower(), Pkg + TEXT(".") + Stem);
					}
				}
			}
		}
		const FString* Found = Names->Find(Name.ToLower());
		return Found ? *Found : FString();
	}

	/** Character records of the player's install, by lower-case name (read once). */
	const TMap<FString, FCharRecord>& CharRecords()
	{
		static TMap<FString, FCharRecord> Records;
		static bool bLoaded = false;
		if (bLoaded)
		{
			return Records;
		}
		bLoaded = true;
		const FString Install = ArenaContent::FindUT2004Install();
		if (Install.IsEmpty())
		{
			return Records;
		}
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Install / TEXT("System") / TEXT("*.upl")), true, false);
		auto Field = [](const FString& Line, const TCHAR* Key) -> FString
		{
			const int32 At = Line.Find(FString(Key) + TEXT("="), ESearchCase::IgnoreCase);
			if (At == INDEX_NONE)
			{
				return FString();
			}
			FString V = Line.Mid(At + FCString::Strlen(Key) + 1);
			int32 End = INDEX_NONE;
			if (V.StartsWith(TEXT("\"")))
			{
				V.RightChopInline(1);
				V.FindChar(TEXT('"'), End);
			}
			else
			{
				End = V.Find(TEXT(","));
				if (End == INDEX_NONE)
				{
					V.FindChar(TEXT(')'), End);
				}
			}
			return End == INDEX_NONE ? V : V.Left(End);
		};
		for (const FString& F : Files)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *(Install / TEXT("System") / F)))
			{
				continue;
			}
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				if (!Line.StartsWith(TEXT("Player="), ESearchCase::IgnoreCase))
				{
					continue;
				}
				const FString Name = Field(Line, TEXT("DefaultName"));
				FCharRecord R;
				R.Mesh = Field(Line, TEXT("Mesh"));
				R.BodySkin = Field(Line, TEXT("BodySkin"));
				R.FaceSkin = Field(Line, TEXT("FaceSkin"));
				if (!Name.IsEmpty() && !R.Mesh.IsEmpty())
				{
					Records.Add(Name.ToLower(), R);
				}
			}
		}
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: %d character records from the install's player lists"), Records.Num());
		return Records;
	}

	/** Models built for meshes that are not in ArenaArt's pool (strings kept alive for the session). */
	struct FDynModel
	{
		FString Mesh, Anim, Skin;
		ArenaArt::FCharacterModel Model;
	};

	/** Our player model for a UE2 mesh ("HumanMaleA.MercMaleD"), or null. */
	const ArenaArt::FCharacterModel* ModelForMesh(const FString& UE2Mesh)
	{
		FString Pkg, Name;
		if (!UE2Mesh.Split(TEXT("."), &Pkg, &Name))
		{
			return nullptr;
		}
		const FString Path = FString::Printf(TEXT("SkeletalMeshes/%s/%s"), *Pkg, *Name);
		for (const ArenaArt::FCharacterModel& M : ArenaArt::GetCharacterModels())
		{
			if (Path.Equals(M.Mesh, ESearchCase::IgnoreCase))
			{
				return &M;
			}
		}
		// a mesh outside the pool (the 2004 characters: Hellions, ThunderCrash ...): its animation set from the
		// content cache's mesh record ("animation": "HumanMaleA.BipedMaleA")
		static TMap<FString, TUniquePtr<FDynModel>> Dyn;
		if (TUniquePtr<FDynModel>* Found = Dyn.Find(Path.ToLower()))
		{
			return *Found ? &(*Found)->Model : nullptr;
		}
		TUniquePtr<FDynModel>& D = Dyn.Add(Path.ToLower());
		FString Json;
		const FString JsonPath = ArenaContent::GetCacheDir() / TEXT("skeletalmeshes") / Pkg / Name + TEXT(".json");
		if (!FFileHelper::LoadFileToString(Json, *JsonPath))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Root;
		FString AnimSet;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid() || !Root->TryGetStringField(TEXT("animation"), AnimSet))
		{
			return nullptr;
		}
		FString AnimPkg, AnimName;
		if (!AnimSet.Split(TEXT("."), &AnimPkg, &AnimName))
		{
			return nullptr;
		}
		D = MakeUnique<FDynModel>();
		D->Mesh = Path;
		D->Anim = FString::Printf(TEXT("Animations/%s/%s_"), *AnimPkg, *AnimName);
		D->Skin = Name;
		D->Model = { *D->Mesh, *D->Anim, *D->Skin, AnimName.Contains(TEXT("Female")) };
		return &D->Model;
	}

	/** Our weapon class for a UE2 weapon attachment class name ("RocketAttachment"). */
	UClass* WeaponForAttachment(const FString& ClassName)
	{
		struct FMap { const TCHAR* Prefix; UClass* (*Class)(); };
		static const FMap Table[] = {
			{ TEXT("SuperShock"), [] { return AWeap_InstaRifle::StaticClass(); } },
			{ TEXT("ClassicSniper"), [] { return AWeap_MarksmanRifle::StaticClass(); } },
			{ TEXT("Assault"), [] { return AWeap_Carbine::StaticClass(); } },
			{ TEXT("Bio"), [] { return AWeap_GooLauncher::StaticClass(); } },
			{ TEXT("Shock"), [] { return AWeap_PulseRifle::StaticClass(); } },
			{ TEXT("Link"), [] { return AWeap_ArcLinker::StaticClass(); } },
			{ TEXT("Minigun"), [] { return AWeap_RotaryGun::StaticClass(); } },
			{ TEXT("Flak"), [] { return AWeap_ShardCannon::StaticClass(); } },
			{ TEXT("Rocket"), [] { return AWeap_RocketLauncher::StaticClass(); } },
			{ TEXT("Sniper"), [] { return AWeap_RailRifle::StaticClass(); } },
			{ TEXT("Shield"), [] { return AWeap_Gauntlet::StaticClass(); } },
			{ TEXT("Redeemer"), [] { return AWeap_WarheadLauncher::StaticClass(); } },
			{ TEXT("Painter"), [] { return AWeap_OrbitalDesignator::StaticClass(); } },
			{ TEXT("ONSPainter"), [] { return AWeap_OrbitalDesignator::StaticClass(); } },
			{ TEXT("ONSAVRiL"), [] { return AWeap_SeekerLauncher::StaticClass(); } },
			{ TEXT("ONSGrenade"), [] { return AWeap_StickyLauncher::StaticClass(); } },
			{ TEXT("ONSMineLayer"), [] { return AWeap_CrawlerLayer::StaticClass(); } },
			{ TEXT("Ball"), [] { return AArenaBR_BallLauncher::StaticClass(); } },
			{ TEXT("Trans"), [] { return AWeap_Translocator::StaticClass(); } },
		};
		FString Short = ClassName;
		int32 Dot = INDEX_NONE;
		if (Short.FindLastChar(TEXT('.'), Dot))
		{
			Short.RightChopInline(Dot + 1);
		}
		// a stock class by its name; a mod's by its stock ancestor below (BallisticV25's BallisticAttachment is no BallLauncher)
		const bool bStockName = Dot == INDEX_NONE || UE2NetModWeapons::IsStockClass(ClassName);
		for (const FMap& M : Table)
		{
			if (bStockName && Short.StartsWith(M.Prefix, ESearchCase::IgnoreCase))
			{
				return M.Class();
			}
		}
		// a mod's class ("<NewNet package>.NewNet_ShockRifle", "WSUTComp_V6.UTComp_ShockAttachment"): its stock ancestor's
		if (ClassName.Contains(TEXT(".")))
		{
			static TMap<FString, UClass*> Cache;
			if (UClass** Found = Cache.Find(ClassName))
			{
				return *Found;
			}
			Cache.Add(ClassName, nullptr);
			UClass* Result = nullptr;
			auto FromChain = [](FUSClass* C) -> UClass*
			{
				for (; C; C = C->GetSuperClass())
				{
					// only UT2004's own classes are stock relatives: a mod's BallisticWeapon is no BallLauncher
					if (!UE2NetModWeapons::IsStockClass(C->GetPathName()))
					{
						continue;
					}
					const FString Name = C->Name.ToString();
					for (const FMap& M : Table)
					{
						if (Name.StartsWith(M.Prefix, ESearchCase::IgnoreCase))
						{
							return M.Class();
						}
					}
				}
				return nullptr;
			};
			FUSClass* const Class = FUSRuntime::Get().FindClass(ClassName);
			Result = FromChain(Class ? Class->GetSuperClass() : nullptr);
			// a mod weapon that extends Weapon itself (ProSniperRifle_20_BETA3.ProSniperRifle): the stock weapon its
			// pickup, ammo or attachment class comes from (ProSniperPickup extends ClassicSniperRiflePickup)
			FUSObject* Def = Class && !Result ? Class->GetDefaultObject() : nullptr;
			for (const TCHAR* PropName : { TEXT("PickupClass"), TEXT("AmmoClass"), TEXT("AttachmentClass") })
			{
				FUSProperty* Prop = Def && Def->Data && !Result ? Def->FindProperty(PropName) : nullptr;
				if (!Prop || Prop->Type != EUSType::Class)
				{
					continue;
				}
				for (int32 i = 0; i < Prop->ArrayDim && !Result; ++i)
				{
					// the default names the class (its object may not be loaded yet): by path
					const FUSObject* Ref = *reinterpret_cast<FUSObject**>(Def->Data + Prop->Offset + i * Prop->ElementSize);
					Result = Ref ? FromChain(FUSRuntime::Get().FindClass(Ref->GetPathName())) : nullptr;
				}
			}
			// a mod attachment that extends xWeaponAttachment itself (ProSniperAttachment): the weapon of its package that
			// hands it out (Weapon.AttachmentClass)
			bool bAttachment = false;
			for (FUSClass* C = Class; C && !Result && !bAttachment; C = C->GetSuperClass())
			{
				bAttachment = C->Name == TEXT("WeaponAttachment");
			}
			FString Pkg;
			FUSLinker* Linker = bAttachment && ClassName.Split(TEXT("."), &Pkg, nullptr) ? FUSRuntime::Get().LoadPackage(FName(*Pkg)) : nullptr;
			for (int32 Ei = 0; Linker && !Result && Ei < Linker->NumExports(); ++Ei)
			{
				const FString WeaponPath = Pkg + TEXT(".") + Linker->GetExportName(Ei);
				FUSClass* WeaponClass = Linker->IsTopLevelClass(Ei) && WeaponPath != ClassName ? FUSRuntime::Get().FindClass(WeaponPath) : nullptr;
				FUSObject* WeaponDef = WeaponClass ? WeaponClass->GetDefaultObject() : nullptr;
				FUSProperty* Prop = WeaponDef && WeaponDef->Data ? WeaponDef->FindProperty(TEXT("AttachmentClass")) : nullptr;
				const FUSObject* Ref = Prop && Prop->Type == EUSType::Class ? *reinterpret_cast<FUSObject**>(WeaponDef->Data + Prop->Offset) : nullptr;
				if (Ref && Ref->GetPathName().Equals(ClassName, ESearchCase::IgnoreCase))
				{
					Result = WeaponForAttachment(WeaponPath);
				}
			}
			if (Result)
			{
				UE_LOG(LogUE2Net, Log, TEXT("UE2NetActors: mod weapon %s shown as our %s (its nearest stock relative)"), *ClassName, *Result->GetName());
			}
			Cache.Add(ClassName, Result);
			return Result;
		}
		return nullptr;
	}

	int32 CVarInt(const TCHAR* Name, int32 Default)
	{
		const IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name);
		return V ? V->GetInt() : Default;
	}
}

UClass* UE2NetActors::WeaponClassFor(const FString& UE2Class)
{
	return UE2NetActorsLocal::WeaponForAttachment(UE2Class);
}

// =================================================================================================
// AUE2NetPawn
// =================================================================================================

AUE2NetPawn::AUE2NetPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	bUseControllerRotationYaw = false;
	SetReplicates(false);
}

void AUE2NetPawn::BeginPlay()
{
	// ACharacter's, not AArenaCharacter's: no spawn sound, no inventory; the body is built here
	ACharacter::BeginPlay();
	UCapsuleComponent* Capsule = GetCapsuleComponent();
	Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Capsule->SetGenerateOverlapEvents(false);
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->SetComponentTickEnabled(false);
		Move->MovementMode = MOVE_Walking;
	}
	bAllowPlayerModel = true;
	BuildBody();
	for (UStaticMeshComponent* Part : BodyParts)
	{
		if (Part)
		{
			Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Part->SetOwnerNoSee(false);
		}
	}
	if (bCharacterKnown)
	{
		ApplyModel();
	}
	UpdateTeamColors();
}

void AUE2NetPawn::SetCharacter(const FString& CharacterName, int32 PlayerId)
{
	using namespace UE2NetActorsLocal;
	const ArenaArt::FCharacterModel* NewModel = nullptr;
	FString Skin, SkinPkg;
	if (const FCharRecord* R = CharRecords().Find(CharacterName.ToLower()))
	{
		NewModel = ModelForMesh(R->Mesh);
		R->BodySkin.Split(TEXT("."), &SkinPkg, &Skin, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		FaceSkin = R->FaceSkin;
	}
	ModelPlayerId = PlayerId;
	if (bCharacterKnown && NewModel == Model && Skin == CharSkin && SkinPkg == CharSkinPkg)
	{
		return;
	}
	bCharacterKnown = true;
	Model = NewModel;
	CharSkin = Skin;
	CharSkinPkg = SkinPkg;
	if (HasActorBegunPlay())
	{
		bModelApplied = false;
		ApplyModel();
	}
}

void AUE2NetPawn::ApplyModel()
{
	if (bModelApplied || bNetDead || !ArenaArt::IsEnabled())
	{
		return;
	}
	const ArenaArt::FCharacterModel* M = Model ? Model : ArenaArt::PickCharacterModel(ModelPlayerId);
	USkeletalMesh* Mesh = M ? ArenaArt::Load<USkeletalMesh>(M->Mesh) : nullptr;
	if (!Mesh && M != ArenaArt::PickCharacterModel(ModelPlayerId))
	{
		// the character's own mesh is not in the cache: a stock model
		M = ArenaArt::PickCharacterModel(ModelPlayerId);
		Mesh = M ? ArenaArt::Load<USkeletalMesh>(M->Mesh) : nullptr;
	}
	if (!Mesh)
	{
		bModelApplied = true; // the procedural body stays
		return;
	}
	USkeletalMeshComponent* SkelMesh = GetMesh();
	SkelMesh->EmptyOverrideMaterials();
	SkelMesh->SetOverlayMaterial(nullptr);
	BodySkinMaterial = nullptr;
	SkelMesh->SetSkeletalMeshAsset(Mesh);
	SkelMesh->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	SkelMesh->SetAnimInstanceClass(UArenaCharacterAnimInstance::StaticClass());
	UArenaCharacterAnimInstance* Anim = Cast<UArenaCharacterAnimInstance>(SkelMesh->GetAnimInstance());
	if (!Anim || !Anim->LoadAnimSet(M->AnimPrefix))
	{
		UE_LOG(LogUE2Net, Warning, TEXT("UE2NetWorld: player model %s has no usable animations (%s), procedural body"), M->Mesh, M->AnimPrefix);
		SkelMesh->SetAnimInstanceClass(nullptr);
		SkelMesh->SetSkeletalMeshAsset(nullptr);
		SkelMesh->SetVisibility(false);
		for (UStaticMeshComponent* Part : BodyParts)
		{
			if (Part)
			{
				Part->SetHiddenInGame(false);
			}
		}
		bSkeletalBody = false;
		bModelApplied = true;
		return;
	}
	AppliedModel = M;
	SkelMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SkelMesh->SetCanEverAffectNavigation(false);
	SkelMesh->SetGenerateOverlapEvents(false);
	SkelMesh->SetOwnerNoSee(false);
	SkelMesh->SetCastShadow(true);
	SkelMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	SkelMesh->SetVisibility(true);
	for (UStaticMeshComponent* Part : BodyParts)
	{
		if (Part)
		{
			Part->SetHiddenInGame(true);
		}
	}
	bSkeletalBody = true;
	bModelApplied = true;
	BodySkinName = M->BodySkin;
	{
		const FString MeshPath(M->Mesh);
		GibKindValue = uint8(MeshPath.Contains(TEXT("/Bot/")) || MeshPath.Contains(TEXT("/XanRobots/")) ? ArenaRagdoll::EGibKind::Robot
			: (MeshPath.Contains(TEXT("/Aliens/")) ? ArenaRagdoll::EGibKind::OrganicAlt : ArenaRagdoll::EGibKind::Organic));
	}
	AppliedSkinTeam = -100;
	LastTeam = -100;
	UE_LOG(LogUE2Net, Verbose, TEXT("UE2NetWorld: %s uses %s, skin %s.%s"), GetPlayerState() ? *GetPlayerState()->GetPlayerName() : *GetName(), M->Mesh, *CharSkinPkg, *CharSkin);
	if (!WeaponClassName.IsEmpty())
	{
		const FString W = WeaponClassName;
		WeaponClassName.Reset();
		SetNetWeapon(W);
	}
}

void AUE2NetPawn::ApplyCharacterSkin()
{
	using namespace UE2NetActorsLocal;
	// the character's skins (xPawn Skins[0] = UPL BodySkin, Skins[1] = FaceSkin; team games: <skin>_0 / _1 when
	// they exist), else what ApplyBodySkin did (the model's skin, or a team tint)
	USkeletalMeshComponent* SkelMesh = GetMesh();
	if (!bSkeletalBody || CharSkin.IsEmpty() || CharSkinPkg.IsEmpty() || !SkelMesh)
	{
		return;
	}
	const int32 Team = GetTeam();
	const bool bTeam = Team == ArenaTeam::Red || Team == ArenaTeam::Blue;
	FString FacePkg, FaceName;
	FaceSkin.Split(TEXT("."), &FacePkg, &FaceName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	const TPair<FString, FString> Skins[2] = { { CharSkinPkg, CharSkin }, { FacePkg, FaceName } };
	bool bAny = false;
	for (int32 Slot = 0; Slot < 2 && Slot < FMath::Max(1, SkelMesh->GetNumMaterials()); ++Slot)
	{
		if (Skins[Slot].Value.IsEmpty())
		{
			continue;
		}
		FString Path = bTeam ? FindSkinObjectPath(Skins[Slot].Key, FString::Printf(TEXT("%s_%d"), *Skins[Slot].Value, Team)) : FString();
		if (Path.IsEmpty())
		{
			Path = FindSkinObjectPath(Skins[Slot].Key, Skins[Slot].Value);
		}
		UMaterialInterface* Mat = Path.IsEmpty() ? nullptr : ArenaRuntimeContent::LoadMaterialByObjectPath(Path);
		if (Mat)
		{
			SkelMesh->SetMaterial(Slot, Mat);
			bAny = true;
		}
	}
	if (bAny)
	{
		SkelMesh->SetOverlayMaterial(nullptr);
		BodySkinMaterial = nullptr;
	}
}

void AUE2NetPawn::SetNetPosition(const FVector& LocationUU, const FVector& VelocityUU, int32 YawUU, bool bTeleport)
{
	using namespace UE2NetActorsLocal;
	if (bNetDead)
	{
		return; // the ragdoll owns the body now
	}
	NetLocation = LocationUU * S;
	NetVelocity = VelocityUU * S;
	const uint8 OldPhysics = SimPhysics;
	SimPhysics = DeriveSimPhysics(VelocityUU);
	UE_CLOG(SimPhysics != OldPhysics, LogUE2Net, Verbose, TEXT("UE2NetWorld: %s physics %d -> %d (velocity %.0f %.0f %.0f, replicated %d)"),
		*GetName(), OldPhysics, SimPhysics, VelocityUU.X, VelocityUU.Y, VelocityUU.Z, NetPhysics);
	if (bHasNetPosition && !bTeleport)
	{
		const float Err = float(FVector::Dist(GetActorLocation(), NetLocation));
		++NumCorrections;
		SumCorrection += Err;
		MaxCorrection = FMath::Max(MaxCorrection, Err);
	}
	NetTime = FPlatformTime::Seconds();
	NetYaw = float(YawUU & 0xFFFF) * 360.f / 65536.f;
	if (!bHasNetPosition || bTeleport)
	{
		bHasNetPosition = true;
		SmoothYaw = NetYaw;
		SetActorLocationAndRotation(NetLocation, FRotator(0.f, NetYaw, 0.f), false, nullptr, ETeleportType::TeleportPhysics);
	}
}

const TCHAR* AUE2NetPawn::GetAnimPrefix() const
{
	return AppliedModel ? AppliedModel->AnimPrefix : nullptr;
}

void AUE2NetPawn::SetNetVehicle(AActor* Vehicle)
{
	if (NetVehicle.Get() == Vehicle)
	{
		return;
	}
	NetVehicle = Vehicle;
	SetActorHiddenInGame(Vehicle != nullptr);
	if (!Vehicle && bHasNetPosition)
	{
		// out of the vehicle: back to the replicated position (the next PawnPosition corrects it)
		NetTime = FPlatformTime::Seconds();
		NetLocation = GetActorLocation();
	}
}

void AUE2NetPawn::SetNetPhysics(uint8 InPhysics)
{
	NetPhysics = SimPhysics = InPhysics;
}

uint8 AUE2NetPawn::DeriveSimPhysics(const FVector& VelocityUU) const
{
	// UE2 replicates Physics to simulated proxies in the initial bunch only (Actor: bNetInitial): the server never
	// says when the pawn lands, jumps or enters water. Work it out like the UE2 client: in a water volume it swims;
	// a clear vertical speed means falling (UE2 physWalking and our movement keep a walking pawn's Velocity.Z at 0;
	// jumps leave the floor at 300+ UU/s, falls pass 100 within 0.1 s). A small one (the apex of a jump, a pawn
	// stepping down an edge), or none after a fall, means falling only while there is no floor below.
	if (NetPhysics == 4)
	{
		return 4; // flying (ghost / fly cheats): nothing else turns it on or off
	}
	if (const UArenaWorldZoneSubsystem* Zones = UArenaWorldZoneSubsystem::Get(this))
	{
		const AArenaPhysicsVolume* Volume = Zones->HasVolumes() ? Zones->GetPhysicsVolumeAt(NetLocation) : nullptr;
		if (Volume && Volume->bWaterVolume)
		{
			return 3;
		}
	}
	const float SpeedZ = FMath::Abs(VelocityUU.Z);
	if (SpeedZ >= 100.f)
	{
		return 2;
	}
	if (SpeedZ > 1.f || SimPhysics == 2 || SimPhysics == 3)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(UE2NetPawnFloor), false, this);
		const FVector Down(0.f, 0.f, CollisionHalfHeight + 30.f);
		if (!GetWorld()->SweepTestByChannel(NetLocation, NetLocation - Down, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeSphere(15.f), Q))
		{
			return 2;
		}
	}
	return 1;
}

void AUE2NetPawn::SetNetCrouched(bool bCrouched, float CollisionHeightUU)
{
	using namespace UE2NetActorsLocal;
	if (bNetDead)
	{
		return; // the ragdoll owns the mesh
	}
	bIsCrouched = bCrouched;
	if (CollisionHeightUU > 0.f)
	{
		CollisionHalfHeight = CollisionHeightUU * S;
		// the model's feet stay on the floor: the mesh moves up as the cylinder shrinks
		GetMesh()->SetRelativeLocation(FVector(0.f, 0.f, -CollisionHalfHeight - ModelFeetZ));
		BodyRoot->SetRelativeLocation(FVector(0.f, 0.f, -CollisionHalfHeight));
	}
}

void AUE2NetPawn::SetNetWeapon(const FString& AttachmentClass)
{
	if (AttachmentClass == WeaponClassName && (WeaponArt || !bSkeletalBody))
	{
		return;
	}
	WeaponClassName = AttachmentClass;
	UClass* WeaponClass = AttachmentClass.IsEmpty() ? nullptr : UE2NetActorsLocal::WeaponForAttachment(AttachmentClass);
	// a mod attachment: its own mesh, skins and placement where they differ from the stock one (Net/UE2NetModWeapons.h)
	const UE2NetModWeapons::FModAttachment* ModAttachment = WeaponClass ? UE2NetModWeapons::FindAttachment(AttachmentClass) : nullptr;
	WeaponArt = ModAttachment ? &ModAttachment->Art : WeaponClass ? ArenaArt::FindWeaponArt(WeaponClass) : nullptr;
	AnimEvents.bHeavyWeapon = WeaponArt && WeaponArt->bHeavy;
	USkeletalMesh* WeaponMesh = (bSkeletalBody && !bNetDead && WeaponArt && WeaponArt->ThirdPersonMesh) ? ArenaArt::Load<USkeletalMesh>(WeaponArt->ThirdPersonMesh) : nullptr;
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
		ThirdPersonWeapon->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
		ThirdPersonWeapon->RegisterComponent();
	}
	if (ThirdPersonWeapon->GetSkeletalMeshAsset() != WeaponMesh)
	{
		ThirdPersonWeapon->SetSkeletalMeshAsset(WeaponMesh);
	}
	ThirdPersonWeapon->EmptyOverrideMaterials();
	if (ModAttachment)
	{
		const TArray<UMaterialInterface*> Skins = UE2NetModWeapons::LoadSkins(ModAttachment->Skins);
		for (int32 i = 0; i < Skins.Num(); ++i)
		{
			if (Skins[i])
			{
				ThirdPersonWeapon->SetMaterial(i, Skins[i]);
			}
		}
	}
	ThirdPersonWeapon->SetRelativeLocationAndRotation(WeaponArt->HandOffset, WeaponArt->HandRotation);
	ThirdPersonWeapon->SetRelativeScale3D(FVector(WeaponArt->HandScale));
	ThirdPersonWeapon->SetVisibility(true);
	++AnimEvents.SwitchCount;
}

FVector AUE2NetPawn::GetMuzzleLocation() const
{
	FVector Loc;
	if (GetThirdPersonMuzzle(Loc))
	{
		return Loc;
	}
	return GetActorLocation() + FVector(0.f, 0.f, BaseEyeHeight) + GetActorForwardVector() * 30.f;
}

void AUE2NetPawn::NetFired(uint8 FiringMode)
{
	if (bNetDead)
	{
		return;
	}
	++AnimEvents.FireCount;
	// muzzle flash of our weapon effect table
	UClass* WeaponClass = WeaponClassName.IsEmpty() ? nullptr : UE2NetActorsLocal::WeaponForAttachment(WeaponClassName);
	const ArenaArtFX::FWeaponFX* FX = WeaponClass ? ArenaArtFX::FindWeaponFX(WeaponClass, FiringMode) : nullptr;
	if (FX && FX->Muzzle && ThirdPersonWeapon && ThirdPersonWeapon->IsVisible())
	{
		static const FName Tip(TEXT("tip"));
		if (ThirdPersonWeapon->DoesSocketExist(Tip))
		{
			const FVector Offset = ThirdPersonWeapon->GetSocketTransform(Tip, RTS_Component).GetLocation();
			ArenaArtFX::SpawnAttachedEffect(ThirdPersonWeapon, FName(FX->Muzzle), Offset, FX->MuzzleScale * 0.6f, false, false, false);
		}
	}
}

void AUE2NetPawn::NetAnimAction(FName Action)
{
	if (Action.ToString().StartsWith(TEXT("Weapon_Switch")))
	{
		++AnimEvents.SwitchCount;
	}
}

void AUE2NetPawn::NetHit(FName Bone)
{
	if (bNetDead)
	{
		return;
	}
	++AnimEvents.HitCount;
	AnimEvents.bHitHead = Bone == TEXT("head");
	AnimEvents.HitFromDir = GetActorForwardVector();
}

void AUE2NetPawn::NetDie(const FVector& MomentumUU, const FVector& HitLocationUU, bool bGib)
{
	ArenaHitchLog::FScopedNote HitchNote(TEXT("player pawn died"));
	using namespace UE2NetActorsLocal;
	if (bNetDead)
	{
		return;
	}
	bNetDead = true;
	bDead = true;
	DeadTime = FPlatformTime::Seconds();
	const FVector Momentum = MomentumUU * S;
	const FVector Velocity = NetVelocity;
	const FVector HitLoc = HitLocationUU * S;
	const FVector FromHit = (GetActorLocation() - HitLoc).GetSafeNormal2D();
	AnimEvents.DeathPushDir = !Momentum.IsNearlyZero() ? Momentum.GetSafeNormal() : (!FromHit.IsNearlyZero() ? FromHit : -GetActorForwardVector());
	++AnimEvents.DeathCount;
	if (ThirdPersonWeapon)
	{
		ThirdPersonWeapon->SetVisibility(false);
	}
	USkeletalMeshComponent* SkelMesh = GetMesh();
	if (bSkeletalBody && !bGib)
	{
		UPhysicsAsset* Ragdoll = CVarInt(TEXT("arena.Ragdolls"), 1) == 0 ? nullptr
			: (SkelMesh->GetPhysicsAsset() ? SkelMesh->GetPhysicsAsset() : ArenaRagdoll::GetPhysicsAsset(SkelMesh->GetSkeletalMeshAsset()));
		if (Ragdoll)
		{
			SkelMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
			SkelMesh->SetCollisionProfileName(TEXT("Ragdoll"));
			SkelMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
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
			// UE2 TearOffMomentum is the killing blow's momentum (often zero for bullets): a push from the hit side
			const FVector Push = Momentum.IsNearlyZero() ? AnimEvents.DeathPushDir * 350.f : Momentum.GetClampedToMaxSize(1500.f);
			SkelMesh->SetAllPhysicsLinearVelocity(Velocity * 0.8f + Push * 0.6f + FVector(0.f, 0.f, 120.f));
		}
		return; // else the death animation (bDead) holds its last frame
	}
	const bool bArtGibs = bSkeletalBody && ArenaRagdoll::SpawnGibs(GetWorld(), GetActorLocation(), Velocity + Momentum, static_cast<ArenaRagdoll::EGibKind>(GibKindValue));
	if (!bArtGibs)
	{
		ArenaFX::SpawnExplosion(GetWorld(), GetActorLocation(), FLinearColor(0.6f, 0.05f, 0.03f), 60.f);
	}
	SkelMesh->SetVisibility(false);
	for (UStaticMeshComponent* Part : BodyParts)
	{
		if (Part)
		{
			Part->SetVisibility(false);
		}
	}
}

FRotator AUE2NetPawn::GetViewRotation() const
{
	// Pawn.ViewPitch (the pitch's high byte), blended in Tick
	return FRotator(SmoothPitch, SmoothYaw, 0.f);
}

FRotator AUE2NetPawn::GetBaseAimRotation() const
{
	return GetViewRotation();
}

void AUE2NetPawn::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
	APawn::CalcCamera(DeltaTime, OutResult);
	const FRotator View = GetViewRotation();
	const FVector Eye = GetActorLocation() + FVector(0.f, 0.f, bIsCrouched ? CrouchedEyeHeight : BaseEyeHeight);
	if ((!bBehindView || bNetDead) && !NetVehicle.IsValid())
	{
		if (!bNetDead)
		{
			OutResult.Location = Eye;
			OutResult.Rotation = View;
			return;
		}
	}
	// UE2 behind view (Pawn.SpecialCalcView / PlayerController.CalcBehindView): orbit behind the pawn, pulled in by walls
	FRotator ViewRot = View;
	ViewRot.Pitch = FMath::Clamp(FRotator::NormalizeAxis(ViewRot.Pitch), -40.f, 40.f) * 0.5f - 10.f;
	FVector Pivot = GetActorLocation() + FVector(0.f, 0.f, 30.f);
	float Distance = 300.f;
	if (bNetDead && bSkeletalBody && GetMesh()->IsSimulatingPhysics())
	{
		Pivot = GetMesh()->GetComponentLocation() + FVector(0.f, 0.f, 30.f);
	}
	FCollisionQueryParams Q(SCENE_QUERY_STAT(UE2NetBehindView), false, this);
	if (AActor* Vehicle = NetVehicle.Get())
	{
		// UE2 Vehicle.SpecialCalcBehindView: further out, above the vehicle
		const FBoxSphereBounds B = Vehicle->GetRootComponent() ? Vehicle->GetRootComponent()->Bounds : FBoxSphereBounds(Vehicle->GetActorLocation(), FVector(200.f), 200.f);
		Pivot = Vehicle->GetActorLocation() + FVector(0.f, 0.f, 120.f);
		Distance = FMath::Max(600.f, float(B.SphereRadius) * 2.2f);
		Q.AddIgnoredActor(Vehicle);
		if (!bBehindView)
		{
			ViewRot.Pitch = FRotator::NormalizeAxis(View.Pitch);
		}
	}
	FVector Desired = Pivot - ViewRot.Vector() * Distance;
	FHitResult Hit;
	if (GetWorld()->SweepSingleByChannel(Hit, Pivot, Desired, FQuat::Identity, ECC_Camera, FCollisionShape::MakeSphere(12.f), Q))
	{
		Desired = Hit.Location;
	}
	OutResult.Location = Desired;
	OutResult.Rotation = (Pivot - Desired).Rotation();
	OutResult.bUseFirstPersonParameters = false;
}

void AUE2NetPawn::Tick(float DeltaSeconds)
{
	using namespace UE2NetActorsLocal;
	// ACharacter's tick only: no AArenaCharacter gameplay (combos, regeneration, spawn checks)
	ACharacter::Tick(DeltaSeconds);
	// the player model once the PRI named the character (a stock pick when it does not come)
	if (!bModelApplied && (bCharacterKnown || GetGameTimeSinceCreation() > 1.f))
	{
		ApplyModel();
	}
	const int32 Team = GetTeam();
	if (Team != LastTeam && bModelApplied)
	{
		LastTeam = Team;
		UpdateTeamColors();
		ApplyCharacterSkin();
	}
	// first person through this pawn: hide the body and weapon from the viewer
	const bool bFirstPerson = !bBehindView && !bNetDead && IsViewedInFirstPerson();
	if (bSkeletalBody)
	{
		GetMesh()->SetVisibility(!bFirstPerson && !(bNetDead && !GetMesh()->IsVisible()));
		if (ThirdPersonWeapon)
		{
			ThirdPersonWeapon->SetVisibility(!bFirstPerson && !bNetDead && WeaponArt != nullptr);
		}
	}
	// the view pitch arrives in 256 steps at the net rate: blend towards it like PlayerController.BlendRot does for
	// a spectator's TargetViewRotation (24 per second, a jump of more than 4096 UE2 units snaps)
	const float NetPitch = FRotator::NormalizeAxis(float(ViewPitchByte) * 256.f * 360.f / 65536.f);
	const float PitchErr = FRotator::NormalizeAxis(NetPitch - SmoothPitch);
	SmoothPitch = FMath::Abs(PitchErr) > 22.5f ? NetPitch : FRotator::NormalizeAxis(SmoothPitch + PitchErr * FMath::Min(1.f, DeltaSeconds * 24.f));
	AActor* Vehicle = NetVehicle.Get();
	if (Vehicle && !bNetDead)
	{
		SetActorLocation(Vehicle->GetActorLocation());
		SmoothYaw = NetYaw = Vehicle->GetActorRotation().Yaw;
		return;
	}
	if (bNetDead || !bHasNetPosition)
	{
		return;
	}
	// extrapolate the last PawnPosition along its velocity (at most 0.3 s), falling pawns with gravity, then ease
	// the shown location towards it: smooth between the server's updates, no snapping on small corrections
	const float Age = float(FMath::Min(FPlatformTime::Seconds() - NetTime, 0.3));
	FVector Target = NetLocation + NetVelocity * Age;
	FVector Vel = NetVelocity;
	if (SimPhysics == 2)
	{
		Target.Z += 0.5f * Gravity * Age * Age;
		Vel.Z += Gravity * Age;
	}
	// move along the velocity, then pull the remaining error in (no lag behind a moving target)
	const FVector Predicted = GetActorLocation() + (Age < 0.3f ? Vel * DeltaSeconds : FVector::ZeroVector);
	const FVector Err = Target - Predicted;
	FVector NewLoc;
	if (Err.SizeSquared() > FMath::Square(400.f))
	{
		NewLoc = Target;
	}
	else
	{
		NewLoc = Predicted + Err * FMath::Min(1.f, DeltaSeconds * 10.f);
	}
	SmoothYaw = FMath::FixedTurn(SmoothYaw, NetYaw, FMath::Max(1.f, FMath::Abs(FRotator::NormalizeAxis(NetYaw - SmoothYaw))) * FMath::Min(1.f, DeltaSeconds * 12.f));
	SetActorLocationAndRotation(NewLoc, FRotator(0.f, SmoothYaw, 0.f));
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->Velocity = Vel;
		// UE2 EPhysics: 1 walking, 2 falling, 3 swimming, 4 flying, 11 ladder
		Move->MovementMode = SimPhysics == 2 ? MOVE_Falling : (SimPhysics == 3 ? MOVE_Swimming : (SimPhysics == 4 || SimPhysics == 11 ? MOVE_Flying : MOVE_Walking));
	}
}

// =================================================================================================
// AUE2NetProjectile
// =================================================================================================

AUE2NetProjectile::AUE2NetProjectile()
{
	PrimaryActorTick.bCanEverTick = true;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(Root);
	Light->SetCastShadows(false);
	Light->SetIntensity(2500.f);
	Light->SetAttenuationRadius(300.f);
	InitialLifeSpan = 20.f;
	SetReplicates(false);
}

void AUE2NetProjectile::Setup(FName InStyle, float InScale, const FLinearColor& InGlow)
{
	Style = InStyle;
	Scale = InScale;
	GlowColor = InGlow;
	Light->SetLightColor(InGlow);
	Art = Style.IsNone() ? nullptr : ArenaArtFX::FindProjectileArt(Style);
	int32 Built = 0;
	if (Art)
	{
		for (const ArenaArtFX::FMeshDesc& M : Art->Meshes)
		{
			if (UStaticMeshComponent* C = ArenaArtFX::AddMesh(this, Root, M, Scale))
			{
				Meshes.Add(C);
				Spin.Add(M.Spin);
				++Built;
			}
		}
		for (const ArenaArtFX::FSpriteDesc& D : Art->Sprites)
		{
			UArenaSpriteComponent* C = NewObject<UArenaSpriteComponent>(this);
			C->SetupAttachment(Root);
			if (C->SetupSprite(D, Scale))
			{
				C->RegisterComponent();
				++Built;
			}
			else
			{
				C->DestroyComponent();
			}
		}
		Light->SetIntensity(2500.f * Art->LightScale);
		Light->SetVisibility(Art->LightScale > 0.f);
	}
	if (Built == 0)
	{
		// procedural glow ball (no art for this class)
		Glow = NewObject<UStaticMeshComponent>(this, TEXT("Glow"));
		Glow->SetStaticMesh(ArenaAssets::Sphere());
		Glow->SetupAttachment(Root);
		Glow->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Glow->SetCastShadow(false);
		Glow->SetRelativeScale3D(FVector(0.18f * Scale));
		if (UMaterialInstanceDynamic* MID = ArenaAssets::MakeGlow(this, InGlow, 8.f, 1.f))
		{
			Glow->SetMaterial(0, MID);
		}
		Glow->RegisterComponent();
	}
}

void AUE2NetProjectile::SetLocalSimulation(bool bInTemporary, int32 InMaxBounces, float InDamping, float InLifeSpan)
{
	bTemporary = bInTemporary;
	BouncesLeft = InMaxBounces;
	Damping = InDamping;
	if (InLifeSpan > 0.f)
	{
		SetLifeSpan(InLifeSpan);
	}
}

bool AUE2NetProjectile::HitsPawn(const FVector& From, const FVector& To, FVector& OutHit) const
{
	for (TActorIterator<AUE2NetPawn> It(GetWorld()); It; ++It)
	{
		if (It->IsNetDead() || It->GetNetVehicle())
		{
			continue;
		}
		// the UE2 collision cylinder (radius 25 UU, half height 44 UU) as a capsule test against the segment
		const FVector P = It->GetActorLocation();
		const FVector Closest = FMath::ClosestPointOnSegment(P, From, To);
		const FVector D = Closest - P;
		if (FVector(D.X, D.Y, 0.f).SizeSquared() < FMath::Square(50.f) && FMath::Abs(D.Z) < 88.f)
		{
			OutHit = Closest;
			return true;
		}
	}
	return false;
}

void AUE2NetProjectile::SetNetVelocity(const FVector& VelocityUU)
{
	Velocity = VelocityUU * ArenaUE2World::Scale;
	bStopped = Velocity.IsNearlyZero();
	if (!bStopped)
	{
		SetActorRotation(Velocity.Rotation());
	}
}

void AUE2NetProjectile::SetNetLocation(const FVector& LocationUU)
{
	SetActorLocation(LocationUU * ArenaUE2World::Scale);
}

void AUE2NetProjectile::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	for (int32 i = 0; i < Meshes.Num(); ++i)
	{
		if (Meshes[i] && Spin.IsValidIndex(i) && Spin[i] != 0.f && !bStopped)
		{
			Meshes[i]->AddLocalRotation(FRotator(0.f, 0.f, Spin[i] * DeltaSeconds));
		}
	}
	if (bStopped || bExploded)
	{
		return;
	}
	// UE2 PHYS_Projectile flies straight, PHYS_Falling (flak chunks after a bounce, grenades, bio) with gravity;
	// walls stop it until the server's next word (bounce velocity, or the close when it explodes)
	if (Physics == 2)
	{
		Velocity.Z += UE2NetActorsLocal::Gravity * DeltaSeconds;
	}
	const FVector From = GetActorLocation();
	const FVector To = From + Velocity * DeltaSeconds;
	FHitResult Hit;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(UE2NetProjectile), false, this);
	FVector PawnHit;
	if (bTemporary && HitsPawn(From, To, PawnHit))
	{
		SetActorLocation(PawnHit);
		NetExplode();
		return;
	}
	if (GetWorld()->LineTraceSingleByChannel(Hit, From, To, ECC_WorldStatic, Q))
	{
		SetActorLocation(Hit.Location + Hit.ImpactNormal * 2.f);
		FlightDistance += float(FVector::Dist(From, Hit.Location));
		if (BouncesLeft > 0 && Velocity.SizeSquared() > FMath::Square(100.f))
		{
			// UE2 HitWall: reflect, lose energy (FlakChunk 0.65, Grenade 0.5 ...)
			--BouncesLeft;
			Velocity = Damping * (Velocity - 2.f * FVector::DotProduct(Velocity, Hit.ImpactNormal) * Hit.ImpactNormal);
			Physics = 2;
			return;
		}
		if (bTemporary)
		{
			NetExplode(); // the client side end of a temporary projectile
			return;
		}
		bStopped = true; // the server says when it explodes (channel close)
		return;
	}
	SetActorLocation(To);
	FlightDistance += float(FVector::Dist(From, To));
	if (!Velocity.IsNearlyZero())
	{
		SetActorRotation(Velocity.Rotation());
	}
}

void AUE2NetProjectile::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (EndPlayReason == EEndPlayReason::Destroyed)
	{
		++NumEnded;
		SumDistance += FlightDistance;
	}
	Super::EndPlay(EndPlayReason);
}

// =================================================================================================
// AUE2NetBeam
// =================================================================================================

AUE2NetBeam::AUE2NetBeam()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork; // after the pawns moved this frame
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetReplicates(false);
}

void AUE2NetBeam::Setup(EType InType)
{
	Type = InType;
	if (Type == EType::Link)
	{
		// the link beam as our own weapon draws it (AArenaWeapon::UpdateLiveBeam, Beam.Link)
		if (const ArenaArtFX::FEffectDesc* E = ArenaArtFX::FindEffect(TEXT("Beam.Link")))
		{
			for (const ArenaArtFX::FSpriteDesc& D : E->Sprites)
			{
				UArenaSpriteComponent* C = NewObject<UArenaSpriteComponent>(this);
				C->SetUsingAbsoluteLocation(true);
				C->SetUsingAbsoluteRotation(true);
				C->SetUsingAbsoluteScale(true);
				C->SetupAttachment(GetRootComponent());
				if (C->SetupSprite(D))
				{
					C->RegisterComponent();
					C->SetVisibility(false);
					Sprites.Add(C);
				}
				else
				{
					C->DestroyComponent();
				}
			}
		}
	}
	if (Sprites.Num() == 0)
	{
		// the painter's and the mine layer's beams as our own weapons draw them (ArenaWeaponBeam)
		const bool bPainter = Type == EType::Painter;
		Fallback = ArenaWeaponBeam::Make(this, bPainter ? FLinearColor(1.f, 0.15f, 0.1f) : (Type == EType::MineLaser ? FLinearColor(1.f, 0.3f, 0.2f) : FLinearColor(0.3f, 1.f, 0.3f)),
			bPainter ? TEXT("XWeapons.PainterBeamEffect") : (Type == EType::MineLaser ? TEXT("Onslaught.ONSMineLayerTargetBeamEffect") : nullptr));
	}
}

void AUE2NetBeam::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ArenaWeaponBeam::StopPainter(Look);
	Super::EndPlay(EndPlayReason);
}

void AUE2NetBeam::SetEnds(AUE2NetPawn* From, const FVector& InStart, const FVector& InEnd)
{
	Source = From;
	StartLoc = InStart;
	EndLoc = InEnd;
}

void AUE2NetBeam::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const AUE2NetPawn* From = Source.Get();
	const FVector Start = From && !From->IsHidden() ? From->GetMuzzleLocation() : StartLoc;
	const bool bShow = !EndLoc.IsNearlyZero() && !Start.IsNearlyZero();
	for (UArenaSpriteComponent* C : Sprites)
	{
		if (!C)
		{
			continue;
		}
		if (bShow)
		{
			C->SetBeam(Start, EndLoc);
			C->SetVisibility(true);
			C->SetComponentTickEnabled(true);
		}
		else
		{
			C->SetVisibility(false);
			C->SetComponentTickEnabled(false);
		}
	}
	if (Fallback)
	{
		if (bShow)
		{
			Fallback->SetVisibility(true);
			ArenaWeaponBeam::Place(Fallback, Start, EndLoc, 2.f);
			if (Type == EType::Painter)
			{
				ArenaWeaponBeam::TickPainter(Fallback, Look, TargetState, DeltaSeconds);
			}
		}
		else
		{
			ArenaWeaponBeam::Hide(Fallback);
		}
	}
}

void AUE2NetProjectile::NetExplode()
{
	if (bExploded)
	{
		return;
	}
	bExploded = true;
	const FVector Loc = GetActorLocation();
	const FVector Normal = Velocity.IsNearlyZero() ? FVector::UpVector : -Velocity.GetSafeNormal();
	const TCHAR* Effect = Art ? (Art->Explosion ? Art->Explosion : Art->Impact) : nullptr;
	if (Effect)
	{
		if (!ArenaArtFX::SpawnEffect(GetWorld(), FName(Effect), Loc, Normal.Rotation(), Scale))
		{
			ArenaFX::SpawnImpact(GetWorld(), Loc, Normal, GlowColor, 30.f * Scale);
		}
	}
	else
	{
		ArenaFX::SpawnImpact(GetWorld(), Loc, Normal, GlowColor, 30.f * Scale);
	}
	Destroy();
}

// =================================================================================================
// AUE2NetPlaceholder
// =================================================================================================

AUE2NetPlaceholder::AUE2NetPlaceholder()
{
	PrimaryActorTick.bCanEverTick = true;
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetGenerateOverlapEvents(false);
	SetReplicates(false);
}

void AUE2NetPlaceholder::Setup(const FString& Label, float RadiusUU, float HeightUU, int32 Team, const FString& StaticMeshPath,
	const FString& SkeletalMeshPath)
{
	UStaticMesh* Art = StaticMeshPath.IsEmpty() ? nullptr : ArenaArt::Load<UStaticMesh>(StaticMeshPath);
	USkeletalMesh* SkelArt = (Art || SkeletalMeshPath.IsEmpty()) ? nullptr : ArenaArt::Load<USkeletalMesh>(SkeletalMeshPath);
	if (SkelArt)
	{
		// the vehicle's own model (no animation: wheels and turrets stay in their bind pose)
		bArtMesh = true;
		Mesh->SetVisibility(false);
		Skel = NewObject<USkeletalMeshComponent>(this, TEXT("Skel"));
		Skel->SetupAttachment(Mesh);
		Skel->SetSkeletalMeshAsset(SkelArt);
		Skel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Skel->SetGenerateOverlapEvents(false);
		Skel->SetUsingAbsoluteScale(true);
		Skel->RegisterComponent();
	}
	else if (Art)
	{
		bArtMesh = true;
		Mesh->SetStaticMesh(Art);
		Mesh->SetWorldScale3D(FVector(1.f));
	}
	else
	{
		Mesh->SetStaticMesh(ArenaAssets::Cube());
		const float R = FMath::Max(10.f, RadiusUU) * ArenaUE2World::Scale;
		const float H = FMath::Max(10.f, HeightUU) * ArenaUE2World::Scale;
		Mesh->SetWorldScale3D(FVector(R * 2.f / 100.f, R * 2.f / 100.f, H * 2.f / 100.f));
	}
	SetTeam(Team);
}

void AUE2NetPlaceholder::SetTeam(int32 Team)
{
	if (bArtMesh)
	{
		return;
	}
	if (UMaterialInstanceDynamic* MID = ArenaAssets::MakeSurface(this, ArenaTeam::Color(Team) * 0.6f, 0.5f, 0.3f))
	{
		Mesh->SetMaterial(0, MID);
	}
}

void AUE2NetPlaceholder::SetNetLocation(const FVector& LocationUU, const FVector& VelocityUU, const FRotator& Rotation)
{
	NetLocation = LocationUU * ArenaUE2World::Scale;
	NetVelocity = VelocityUU * ArenaUE2World::Scale;
	NetTime = FPlatformTime::Seconds();
	if (!bHasNet)
	{
		bHasNet = true;
		SetActorLocation(NetLocation);
	}
	SetActorRotation(Rotation);
}

void AUE2NetPlaceholder::SetNetRigidBody(const FVector& LocationUU, const FQuat& Rotation, const FVector& VelocityUU)
{
	bRigidBody = true;
	NetRotation = Rotation;
	const FVector NewLocation = LocationUU * ArenaUE2World::Scale;
	const double Now = FPlatformTime::Seconds();
	// the replicated ChassisLinVel is not in UU/s (Karma units); the velocity comes from successive positions
	const double Dt = Now - NetTime;
	NetVelocity = (bHasNet && Dt > 0.02 && Dt < 0.5) ? FVector((NewLocation - NetLocation) / Dt).GetClampedToMaxSize(8000.f) : FVector::ZeroVector;
	NetLocation = NewLocation;
	NetTime = Now;
	if (!bHasNet)
	{
		bHasNet = true;
		SetActorLocationAndRotation(NetLocation, NetRotation);
	}
}

void AUE2NetPlaceholder::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bHasNet)
	{
		return;
	}
	const float Age = float(FMath::Min(FPlatformTime::Seconds() - NetTime, 0.3));
	const FVector Target = NetLocation + NetVelocity * Age;
	const FVector Cur = GetActorLocation();
	SetActorLocation((Target - Cur).SizeSquared() > FMath::Square(500.f) ? Target : Cur + (Target - Cur) * FMath::Min(1.f, DeltaSeconds * 10.f));
	if (bRigidBody)
	{
		SetActorRotation(FQuat::Slerp(GetActorQuat(), NetRotation, FMath::Min(1.f, DeltaSeconds * 10.f)));
	}
}

// =================================================================================================
// AUE2NetVehicle
// =================================================================================================

int32 AUE2NetVehicle::NumAimUpdates = 0;
int32 AUE2NetVehicle::NumFlashes = 0;
int32 AUE2NetVehicle::NumSeatsShown = 0;
int32 AUE2NetVehicle::NumWrecks = 0;
int32 AUE2NetVehicle::NumAimChecks = 0;
double AUE2NetVehicle::SumAimError = 0.0;
float AUE2NetVehicle::MaxAimError = 0.f;

namespace UE2NetVehicleLocal
{
	/** Reference pose of a bone in component space. */
	FTransform RefComponentSpace(const UPoseableMeshComponent* Mesh, int32 BoneIndex)
	{
		const USkinnedAsset* Asset = Mesh ? Mesh->GetSkinnedAsset() : nullptr;
		if (!Asset || BoneIndex == INDEX_NONE)
		{
			return FTransform::Identity;
		}
		const FReferenceSkeleton& Ref = Asset->GetRefSkeleton();
		FTransform T = Ref.GetRefBonePose()[BoneIndex];
		for (int32 P = Ref.GetParentIndex(BoneIndex); P != INDEX_NONE; P = Ref.GetParentIndex(P))
		{
			T = T * Ref.GetRefBonePose()[P];
		}
		return T;
	}

	/** Sets a bone's component-space transform (as ArenaArtVehicles' rig: relative to its posed / reference parent). */
	void PoseBone(UPoseableMeshComponent* Mesh, int32 BoneIndex, const FTransform& ComponentSpace, TMap<int32, FTransform>& Posed)
	{
		const USkinnedAsset* Asset = Mesh ? Mesh->GetSkinnedAsset() : nullptr;
		if (!Asset || BoneIndex == INDEX_NONE || !Mesh->BoneSpaceTransforms.IsValidIndex(BoneIndex))
		{
			return;
		}
		const FReferenceSkeleton& Ref = Asset->GetRefSkeleton();
		FTransform ParentCS = FTransform::Identity;
		TArray<int32, TInlineAllocator<8>> Chain;
		for (int32 P = Ref.GetParentIndex(BoneIndex); P != INDEX_NONE; P = Ref.GetParentIndex(P))
		{
			if (const FTransform* Found = Posed.Find(P))
			{
				ParentCS = *Found;
				break;
			}
			Chain.Add(P);
		}
		for (int32 i = Chain.Num() - 1; i >= 0; --i)
		{
			ParentCS = Ref.GetRefBonePose()[Chain[i]] * ParentCS;
		}
		Mesh->BoneSpaceTransforms[BoneIndex] = ComponentSpace.GetRelativeTransform(ParentCS);
		Posed.Add(BoneIndex, ComponentSpace);
	}

	UPoseableMeshComponent* MakePoseable(AActor* Owner, USceneComponent* Parent, USkeletalMesh* Mesh, FName Socket)
	{
		UPoseableMeshComponent* C = NewObject<UPoseableMeshComponent>(Owner);
		C->SetSkinnedAssetAndUpdate(Mesh);
		C->SetupAttachment(Parent, Socket);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCollisionResponseToAllChannels(ECR_Ignore);
		C->SetGenerateOverlapEvents(false);
		C->SetCanEverAffectNavigation(false);
		C->SetCastShadow(true);
		C->bComponentUseFixedSkelBounds = true;
		C->RegisterComponent();
		return C;
	}

	/** UE2 draws an actor at Location + PrePivot along the world axes (not turned with it): the component's offset from its
	 *  attachment frame for the current orientation. */
	void PlacePrePivot(USceneComponent* C, const FVector& PrePivotUU)
	{
		if (!C || PrePivotUU.IsNearlyZero())
		{
			return;
		}
		const USceneComponent* P = C->GetAttachParent();
		const FTransform Frame = P ? P->GetSocketTransform(C->GetAttachSocketName()) : FTransform::Identity;
		C->SetRelativeLocation(Frame.InverseTransformVector(PrePivotUU * ArenaUE2World::Scale));
	}

	FLinearColor TeamFlash(int32 Team)
	{
		return Team == 0 ? FLinearColor(1.f, 0.55f, 0.35f) : (Team == 1 ? FLinearColor(0.45f, 0.65f, 1.f) : FLinearColor(1.f, 0.85f, 0.5f));
	}
}

AUE2NetVehicle::AUE2NetVehicle()
{
	PrimaryActorTick.bCanEverTick = true;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;
	SetReplicates(false);
}

void AUE2NetVehicle::Setup(const FString& InLabel, const FString& SkeletalMeshPath, const FString& StaticMeshPath, const UE2NetModWeapons::FLook& Look,
	const FString& DestroyedMeshPath, float RadiusUU, float HeightUU)
{
	using namespace UE2NetVehicleLocal;
	Label = InLabel;
	ChassisLook = Look;
	DestroyedMesh = DestroyedMeshPath;
	// our art rig when the class's mesh is the mesh of one of our vehicle types: its chassis sits on the UE2 actor
	// origin (the rig places it MeshOffset from the collision box centre). A mod class with skins or a scale of its own
	// (a Manta with other team colours) is drawn with the UE2 mesh and its own look instead.
	if (!SkeletalMeshPath.IsEmpty() && ArenaArt::IsEnabled() && !Look.bOwn)
	{
		for (int32 T = 0; T < int32(EArenaONS_VehicleType::Num) && !bRig; ++T)
		{
			const ArenaArtVehicles::FVehicleArt* Art = ArenaArtVehicles::Find(EArenaONS_VehicleType(T));
			if (!Art || !Art->Mesh || !SkeletalMeshPath.Equals(Art->Mesh, ESearchCase::IgnoreCase))
			{
				continue;
			}
			RigRoot = NewObject<USceneComponent>(this, TEXT("RigRoot"));
			RigRoot->SetupAttachment(Root);
			RigRoot->SetRelativeLocation(-Art->MeshOffset);
			RigRoot->RegisterComponent();
			bRig = Rig.Build(this, RigRoot, EArenaONS_VehicleType(T), ArenaTeam::None);
			if (!bRig)
			{
				RigRoot->DestroyComponent();
				RigRoot = nullptr;
			}
			else if (Rig.Chassis)
			{
				// the rig's wheels get our suspension (offsets per suspension point)
				for (const ArenaArtVehicles::FWheel& W : Art->Wheels)
				{
					FWheelState WS;
					WS.BoneIndex = W.Bone ? Rig.Chassis->GetBoneIndex(FName(W.Bone)) : INDEX_NONE;
					WS.Radius = Art->WheelRadius > 0.f ? Art->WheelRadius : 40.f;
					WS.Travel = FMath::Clamp(WS.Radius * 0.6f, 15.f, 60.f);
					WS.RigSuspension = W.SuspensionIndex;
					if (WS.BoneIndex != INDEX_NONE)
					{
						WheelStates.Add(WS);
					}
				}
			}
		}
	}
	if (!bRig)
	{
		if (USkeletalMesh* M = SkeletalMeshPath.IsEmpty() ? nullptr : ArenaArt::Load<USkeletalMesh>(SkeletalMeshPath))
		{
			Chassis = MakePoseable(this, Root, M, NAME_None);
			Chassis->SetRelativeScale3D(Look.Scale);
		}
		else if (RadiusUU >= 0.f) // (negative: DT_None, nothing drawn)
		{
			BaseMesh = NewObject<UStaticMeshComponent>(this, TEXT("BaseMesh"));
			BaseMesh->SetupAttachment(Root);
			BaseMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			BaseMesh->SetGenerateOverlapEvents(false);
			if (UStaticMesh* SM = StaticMeshPath.IsEmpty() ? nullptr : ArenaArt::Load<UStaticMesh>(StaticMeshPath))
			{
				BaseMesh->SetStaticMesh(SM);
				BaseMesh->SetRelativeScale3D(Look.Scale);
			}
			else
			{
				// no art: a box of the collision size
				BaseMesh->SetStaticMesh(ArenaAssets::Cube());
				const float R = FMath::Max(10.f, RadiusUU) * ArenaUE2World::Scale;
				const float H = FMath::Max(10.f, HeightUU) * ArenaUE2World::Scale;
				BaseMesh->SetRelativeScale3D(FVector(R * 2.f / 100.f, R * 2.f / 100.f, H * 2.f / 100.f));
				if (UMaterialInstanceDynamic* MID = ArenaAssets::MakeSurface(this, FLinearColor(0.4f, 0.4f, 0.4f), 0.5f, 0.3f))
				{
					BaseMesh->SetMaterial(0, MID);
				}
			}
			BaseMesh->RegisterComponent();
		}
	}
	static TSet<FString> Logged;
	if (!Logged.Contains(Label))
	{
		Logged.Add(Label);
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: vehicle %s: %s%s"), *Label, bRig ? TEXT("our art rig, ") : TEXT(""), SkeletalMeshPath.IsEmpty() ? *StaticMeshPath : *SkeletalMeshPath);
	}
}

void AUE2NetVehicle::SetWheels(const TArray<FNetWheel>& InWheels)
{
	if (bRig || !Chassis)
	{
		return;
	}
	WheelStates.Reset();
	for (const FNetWheel& W : InWheels)
	{
		FWheelState WS;
		WS.BoneIndex = W.Bone.IsNone() ? INDEX_NONE : Chassis->GetBoneIndex(W.Bone);
		WS.Radius = FMath::Max(10.f, W.RadiusUU * ArenaUE2World::Scale);
		WS.Travel = FMath::Clamp(WS.Radius * 0.6f, 15.f, 60.f);
		WS.SteerSign = W.SteerType == 1 ? 1 : (W.SteerType == 2 ? -1 : 0);
		if (WS.BoneIndex != INDEX_NONE)
		{
			WheelStates.Add(WS);
		}
	}
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: vehicle %s: %d of %d wheels found on its mesh"), *Label, WheelStates.Num(), InWheels.Num());
}

void AUE2NetVehicle::UpdateSuspension(UPoseableMeshComponent* C, float DeltaSeconds)
{
	UWorld* World = GetWorld();
	if (!C || !World)
	{
		return;
	}
	const FTransform CT = C->GetComponentTransform();
	const FVector Up = C->GetUpVector();
	FCollisionQueryParams Q(SCENE_QUERY_STAT(UE2NetVehicleWheel), false, this);
	for (FWheelState& W : WheelStates)
	{
		const FVector Center = CT.TransformPosition(UE2NetVehicleLocal::RefComponentSpace(C, W.BoneIndex).GetLocation());
		FHitResult Hit;
		float Target = -W.Travel * 0.5f; // in the air: hanging
		if (World->LineTraceSingleByChannel(Hit, Center + Up * W.Travel, Center - Up * (W.Travel + W.Radius), ECC_WorldStatic, Q))
		{
			Target = FMath::Clamp(float(FVector::DotProduct(Hit.ImpactPoint + Up * W.Radius - Center, Up)), -W.Travel, W.Travel);
		}
		W.Offset = FMath::FInterpTo(W.Offset, Target, DeltaSeconds, 15.f);
	}
}

void AUE2NetVehicle::ApplySkins(UMeshComponent* C, const UE2NetModWeapons::FLook& Look)
{
	if (!C)
	{
		return;
	}
	TArray<FString> Skins = Look.Skins;
	const FString& TeamSkin = (Team == 1 && !Look.BlueSkin.IsEmpty()) ? Look.BlueSkin : Look.RedSkin;
	if (!TeamSkin.IsEmpty())
	{
		if (Skins.Num() == 0)
		{
			Skins.Add(FString());
		}
		Skins[0] = TeamSkin;
	}
	const TArray<UMaterialInterface*> Materials = UE2NetModWeapons::LoadSkins(Skins);
	for (int32 i = 0; i < Materials.Num() && i < C->GetNumMaterials(); ++i)
	{
		if (Materials[i])
		{
			C->SetMaterial(i, Materials[i]);
		}
	}
}

void AUE2NetVehicle::SetTeam(int32 InTeam)
{
	if (InTeam == Team)
	{
		return;
	}
	Team = InTeam;
	if (bRig)
	{
		Rig.SetTeam(Team);
	}
	else
	{
		ApplySkins(Chassis ? static_cast<UMeshComponent*>(Chassis) : BaseMesh.Get(), ChassisLook);
	}
	for (TPair<int32, FGun>& P : Guns)
	{
		if (P.Value.RigGun == INDEX_NONE)
		{
			ApplySkins(P.Value.Comp.Get(), P.Value.Look);
		}
		for (int32 i = 0; i < P.Value.Extras.Num() && i < P.Value.ExtraDefs.Num(); ++i)
		{
			ApplyExtraSkins(P.Value.Extras[i].Get(), P.Value.ExtraDefs[i]);
		}
	}
}

void AUE2NetVehicle::SetBodyHidden(bool bInHidden)
{
	if (bInHidden == bBodyHidden)
	{
		return;
	}
	bBodyHidden = bInHidden;
	// the guns on its bones stay as they are (their own bHidden)
	for (USceneComponent* C : TArray<USceneComponent*>{ RigRoot, Chassis, BaseMesh })
	{
		if (C && !bDestroyed)
		{
			C->SetVisibility(!bBodyHidden, C == RigRoot);
		}
	}
}

void AUE2NetVehicle::SetWeaponHidden(int32 Key, bool bInHidden)
{
	FGun* G = Guns.Find(Key);
	if (!G || G->bHidden == bInHidden)
	{
		return;
	}
	G->bHidden = bInHidden;
	if (UPoseableMeshComponent* C = G->Comp.Get(); C && !bDestroyed)
	{
		C->SetVisibility(!bInHidden, true); // (with what is attached to it)
	}
}

void AUE2NetVehicle::SetNetRigidBody(const FVector& LocationUU, const FQuat& Rotation, const FVector& VelocityUU)
{
	bRigidBody = true;
	NetRotation = Rotation;
	NetLocation = LocationUU * ArenaUE2World::Scale;
	NetVelocity = (VelocityUU * ArenaUE2World::Scale).GetClampedToMaxSize(12000.f);
	NetTime = FPlatformTime::Seconds();
	if (!bHasNet)
	{
		bHasNet = true;
		SetActorLocationAndRotation(NetLocation, NetRotation);
		LastYaw = GetActorRotation().Yaw;
	}
}

void AUE2NetVehicle::SetNetLocation(const FVector& LocationUU, const FVector& VelocityUU, const FRotator& Rotation, bool bTeleport)
{
	NetLocation = LocationUU * ArenaUE2World::Scale;
	NetVelocity = VelocityUU * ArenaUE2World::Scale;
	NetRotation = Rotation.Quaternion();
	NetTime = FPlatformTime::Seconds();
	if (!bHasNet || bTeleport)
	{
		bHasNet = true;
		SetActorLocationAndRotation(NetLocation, NetRotation);
		LastYaw = GetActorRotation().Yaw;
	}
}

void AUE2NetVehicle::SetAmbientSound(USoundBase* Sound, float Volume)
{
	AmbientSound = Sound;
	AmbientVolume = FMath::Clamp(Volume, 0.05f, 1.5f);
	if (!Sound)
	{
		if (Ambient)
		{
			Ambient->Stop();
		}
		return;
	}
	if (!Ambient)
	{
		Ambient = NewObject<UAudioComponent>(this, TEXT("Ambient"));
		Ambient->SetupAttachment(Root);
		Ambient->bAutoActivate = false;
		Ambient->bStopWhenOwnerDestroyed = true;
		Ambient->AttenuationSettings = ArenaArtVehicles::GetAttenuation();
		Ambient->RegisterComponent();
	}
	Ambient->SetVolumeMultiplier(AmbientVolume);
	if (Ambient->Sound != Sound)
	{
		Ambient->SetSound(Sound);
	}
	if (!bDestroyed && !Ambient->IsPlaying())
	{
		Ambient->Play();
	}
}

USceneComponent* AUE2NetVehicle::GetChassisComponent() const
{
	if (bRig && Rig.Chassis)
	{
		return Rig.Chassis;
	}
	if (Chassis)
	{
		return Chassis;
	}
	return BaseMesh ? static_cast<USceneComponent*>(BaseMesh) : Root.Get();
}

UPoseableMeshComponent* AUE2NetVehicle::GetGunComponent(const FGun& G) const
{
	if (G.RigGun != INDEX_NONE)
	{
		return Rig.Guns.IsValidIndex(G.RigGun) ? Rig.Guns[G.RigGun].Get() : nullptr;
	}
	return G.Comp.Get();
}

void AUE2NetVehicle::SetWeapon(int32 Key, const FString& MeshPath, FName AttachBone, FName YawBone, FName PitchBone, FName FireBone, float FireOffsetUU,
	const UE2NetModWeapons::FLook& Look)
{
	using namespace UE2NetVehicleLocal;
	if (const FGun* Old = Guns.Find(Key); Old && (Old->Comp.IsValid() || Old->RigGun != INDEX_NONE))
	{
		return;
	}
	FGun G;
	G.FireBone = FireBone;
	G.FireOffset = FireOffsetUU * ArenaUE2World::Scale;
	G.Look = Look;
	// our rig's gun of the same mesh: aimed through the rig's driver / rear turret slot; a fixed rig gun (the rig has it
	// as decoration) is replaced by our own aimed copy
	if (bRig && Rig.Art)
	{
		for (int32 g = 0; g < Rig.Art->Guns.Num() && g < Rig.Guns.Num(); ++g)
		{
			const ArenaArtVehicles::FGun& RG = Rig.Art->Guns[g];
			if (RigGunsUsed.Contains(g) || !Rig.Guns[g] || !RG.Mesh || !MeshPath.Equals(RG.Mesh, ESearchCase::IgnoreCase))
			{
				continue;
			}
			RigGunsUsed.Add(g);
			if (RG.Aim != ArenaArtVehicles::EAim::None)
			{
				G.RigGun = g;
				G.RigAim = RG.Aim == ArenaArtVehicles::EAim::Rear ? 2 : 1;
				G.YawBone = Rig.Guns[g]->GetBoneIndex(RG.YawBone ? FName(RG.YawBone) : YawBone);
				G.Yaw = G.RigAim == 2 ? 180.f : 0.f;
				Guns.Add(Key, G);
				return;
			}
			Rig.Guns[g]->SetVisibility(false);
			break;
		}
	}
	USkeletalMesh* M = MeshPath.IsEmpty() ? nullptr : ArenaArt::Load<USkeletalMesh>(MeshPath);
	if (!M)
	{
		Guns.Add(Key, G); // no art: the aim and flashes still work from the chassis
		return;
	}
	USceneComponent* Parent = GetChassisComponent();
	const USkinnedMeshComponent* ParentSkin = Cast<USkinnedMeshComponent>(Parent);
	const FName Socket = (ParentSkin && !AttachBone.IsNone() && ParentSkin->GetBoneIndex(AttachBone) != INDEX_NONE) ? AttachBone : NAME_None;
	UPoseableMeshComponent* C = MakePoseable(this, Parent, M, Socket);
	// UE2 attachments keep their own DrawScale (the chassis's scale moves the bone, not the gun's size)
	C->SetUsingAbsoluteScale(true);
	C->SetRelativeScale3D(Look.Scale);
	GunComps.Add(C);
	G.Comp = C;
	G.YawBone = YawBone.IsNone() ? INDEX_NONE : C->GetBoneIndex(YawBone);
	G.PitchBone = PitchBone.IsNone() ? INDEX_NONE : C->GetBoneIndex(PitchBone);
	Guns.Add(Key, G);
	ApplySkins(C, Look);
	if (bDestroyed)
	{
		C->SetVisibility(false);
	}
}

void AUE2NetVehicle::RemoveWeapon(int32 Key)
{
	FGun G;
	if (!Guns.RemoveAndCopyValue(Key, G))
	{
		return;
	}
	if (G.RigGun != INDEX_NONE)
	{
		RigGunsUsed.Remove(G.RigGun);
	}
	else if (UPoseableMeshComponent* C = G.Comp.Get())
	{
		GunComps.Remove(C);
		C->DestroyComponent();
	}
	for (const TWeakObjectPtr<UMeshComponent>& X : G.Extras)
	{
		if (X.IsValid())
		{
			X->DestroyComponent();
		}
	}
}

void AUE2NetVehicle::SetWeaponAttachments(int32 Key, const TArray<UE2NetModWeapons::FClientAttachment>& Attachments)
{
	FGun* G = Guns.Find(Key);
	UPoseableMeshComponent* Gun = G ? G->Comp.Get() : nullptr;
	if (!Gun || G->ExtraDefs.Num() > 0)
	{
		return;
	}
	for (const UE2NetModWeapons::FClientAttachment& A : Attachments)
	{
		UMeshComponent* C = nullptr;
		if (A.bSkeletal)
		{
			USkeletalMesh* M = ArenaArt::Load<USkeletalMesh>(A.Mesh);
			C = M ? UE2NetVehicleLocal::MakePoseable(this, Gun, M, Gun->GetBoneIndex(A.Bone) != INDEX_NONE ? A.Bone : NAME_None) : nullptr;
		}
		else if (UStaticMesh* M = ArenaArt::Load<UStaticMesh>(A.Mesh))
		{
			UStaticMeshComponent* S = NewObject<UStaticMeshComponent>(this);
			S->SetStaticMesh(M);
			S->SetupAttachment(Gun, Gun->GetBoneIndex(A.Bone) != INDEX_NONE ? A.Bone : NAME_None);
			S->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			S->SetGenerateOverlapEvents(false);
			S->RegisterComponent();
			C = S;
		}
		if (!C)
		{
			continue;
		}
		C->SetUsingAbsoluteScale(true);
		C->SetRelativeScale3D(A.Scale);
		C->SetRelativeLocationAndRotation(A.Offset * ArenaUE2World::Scale, A.Rotation);
		C->SetVisibility(!G->bHidden && !bDestroyed);
		ApplyExtraSkins(C, A);
		G->Extras.Add(C);
		G->ExtraDefs.Add(A);
	}
}

void AUE2NetVehicle::ApplyExtraSkins(UMeshComponent* C, const UE2NetModWeapons::FClientAttachment& A)
{
	const TArray<FString>& TeamSkins = Team == 1 ? A.BlueSkins : A.RedSkins;
	const TArray<UMaterialInterface*> Materials = UE2NetModWeapons::LoadSkins((Team == 0 || Team == 1) && TeamSkins.Num() ? TeamSkins : A.Skins);
	for (int32 i = 0; C && i < Materials.Num() && i < C->GetNumMaterials(); ++i)
	{
		if (Materials[i])
		{
			C->SetMaterial(i, Materials[i]);
		}
	}
}

void AUE2NetVehicle::SetWeaponLimits(int32 Key, float PitchMin, float PitchMax, bool bLimitYaw, float YawMin, float YawMax)
{
	if (FGun* G = Guns.Find(Key))
	{
		G->PitchMin = FMath::Min(PitchMin, PitchMax);
		G->PitchMax = FMath::Max(PitchMin, PitchMax);
		G->bLimitYaw = bLimitYaw && G->RigAim == 0;
		G->YawMin = YawMin;
		G->YawMax = YawMax;
	}
}

void AUE2NetVehicle::SetWeaponAim(int32 Key, const FVector& TargetUU)
{
	if (FGun* G = Guns.Find(Key))
	{
		G->Target = TargetUU * ArenaUE2World::Scale;
		G->bHasTarget = !TargetUU.IsNearlyZero();
		++NumAimUpdates;
	}
}

bool AUE2NetVehicle::AimAngles(const UPoseableMeshComponent* C, int32 YawBone, const FVector& WorldTarget, float& OutYaw, float& OutPitch) const
{
	if (!C)
	{
		return false;
	}
	const FTransform CT = C->GetComponentTransform();
	const FVector Pivot = CT.TransformPosition(UE2NetVehicleLocal::RefComponentSpace(C, YawBone).GetLocation());
	const FVector Local = CT.InverseTransformVectorNoScale(WorldTarget - Pivot);
	if (Local.SizeSquared() < 1.f)
	{
		return false;
	}
	OutYaw = FMath::RadiansToDegrees(FMath::Atan2(Local.Y, Local.X));
	OutPitch = FMath::RadiansToDegrees(FMath::Atan2(Local.Z, FVector2D(Local.X, Local.Y).Size()));
	return true;
}

void AUE2NetVehicle::PoseGun(UPoseableMeshComponent* C, int32 YawBone, int32 PitchBone, float Yaw, float Pitch) const
{
	using namespace UE2NetVehicleLocal;
	if (!C || (YawBone == INDEX_NONE && PitchBone == INDEX_NONE))
	{
		return;
	}
	// ArenaArtVehicles' rig convention: yaw about the component's Z at the yaw bone, pitch on the pitch bone
	TMap<int32, FTransform> Posed;
	const FQuat YawQ = FRotator(0.f, YawBone != INDEX_NONE ? Yaw : 0.f, 0.f).Quaternion();
	FVector YawPivot = FVector::ZeroVector;
	if (YawBone != INDEX_NONE)
	{
		// one bone for both (the Manta's and Raptor's gun barrels): yaw and pitch on it
		FTransform CS = RefComponentSpace(C, YawBone);
		YawPivot = CS.GetLocation();
		CS.SetRotation((PitchBone == YawBone ? FRotator(Pitch, Yaw, 0.f).Quaternion() : YawQ) * CS.GetRotation());
		PoseBone(C, YawBone, CS, Posed);
	}
	if (PitchBone != INDEX_NONE && PitchBone != YawBone)
	{
		FTransform CS = RefComponentSpace(C, PitchBone);
		const FQuat Full = FRotator(Pitch, YawBone != INDEX_NONE ? Yaw : 0.f, 0.f).Quaternion();
		CS.SetRotation(Full * CS.GetRotation());
		CS.SetLocation(YawPivot + YawQ.RotateVector(CS.GetLocation() - YawPivot));
		PoseBone(C, PitchBone, CS, Posed);
	}
	if (Posed.Num() > 0)
	{
		C->MarkRefreshTransformDirty();
		C->RefreshBoneTransforms();
	}
}

FVector AUE2NetVehicle::GetMuzzle(const FGun& G, FVector& OutDir) const
{
	const UPoseableMeshComponent* C = GetGunComponent(G);
	const USceneComponent* At = C ? static_cast<const USceneComponent*>(C) : GetChassisComponent();
	FVector Loc = At ? At->GetComponentLocation() : GetActorLocation();
	if (C && !G.FireBone.IsNone() && C->GetBoneIndex(G.FireBone) != INDEX_NONE)
	{
		Loc = C->GetSocketLocation(G.FireBone);
	}
	OutDir = G.bHasTarget ? (G.Target - Loc).GetSafeNormal() : (At ? At->GetForwardVector() : GetActorForwardVector());
	if (OutDir.IsNearlyZero())
	{
		OutDir = GetActorForwardVector();
	}
	return Loc + OutDir * G.FireOffset;
}

void AUE2NetVehicle::WeaponFired(int32 Key, const FVector& HitUU, bool bInstantHit, bool bHasHit)
{
	using namespace UE2NetVehicleLocal;
	FGun* G = Guns.Find(Key);
	UWorld* World = GetWorld();
	if (!G || !World || bDestroyed)
	{
		return;
	}
	++NumFlashes;
	++G->Shots;
	FVector Dir;
	const FVector Muzzle = GetMuzzle(*G, Dir);
	const FLinearColor Color = TeamFlash(Team);
	ArenaFX::SpawnImpact(World, Muzzle, Dir, Color, bInstantHit ? 14.f : 34.f, bInstantHit ? TEXT("Muzzle.Minigun") : TEXT("Muzzle.Rocket"));
	if (bInstantHit && bHasHit)
	{
		const FVector Hit = HitUU * ArenaUE2World::Scale;
		ArenaFX::SpawnBeam(World, Muzzle, Hit, Color, 2.f, 0.07f, TEXT("Tracer.Bullet"));
		ArenaFX::SpawnImpact(World, Hit, (Muzzle - Hit).GetSafeNormal(), Color, 12.f, TEXT("Impact.Bullet"));
	}
}

void AUE2NetVehicle::WeaponHit(int32 Key, const FVector& HitUU)
{
	UWorld* World = GetWorld();
	const FGun* G = Guns.Find(Key);
	if (!World || !G || HitUU.IsNearlyZero())
	{
		return;
	}
	FVector Dir;
	const FVector Muzzle = GetMuzzle(*G, Dir);
	const FVector Hit = HitUU * ArenaUE2World::Scale;
	ArenaFX::SpawnImpact(World, Hit, (Muzzle - Hit).GetSafeNormal(), UE2NetVehicleLocal::TeamFlash(Team), 14.f, TEXT("Impact.Bullet"));
}

void AUE2NetVehicle::SetSeat(int32 Key, AUE2NetPawn* Pawn, bool bShow, const FVector& DrivePosUU, const FRotator& DriveRot, int32 WeaponKey)
{
	if (!Pawn)
	{
		FSeat Old;
		if (Seats.RemoveAndCopyValue(Key, Old) && Old.Body.IsValid())
		{
			Old.Body->SetVisibility(false);
			SeatBodies.Remove(Old.Body.Get());
			Old.Body->DestroyComponent();
		}
		return;
	}
	FSeat& S = Seats.FindOrAdd(Key);
	if (S.Pawn.Get() != Pawn)
	{
		S.ShownMesh.Reset();
	}
	S.Pawn = Pawn;
	S.bShow = bShow;
	S.Pos = DrivePosUU * ArenaUE2World::Scale;
	S.Rot = DriveRot;
	S.WeaponKey = WeaponKey;
	UpdateSeat(S);
}

void AUE2NetVehicle::UpdateSeat(FSeat& S)
{
	AUE2NetPawn* P = S.Pawn.Get();
	USkeletalMesh* BodyMesh = P && P->GetMesh() ? P->GetMesh()->GetSkeletalMeshAsset() : nullptr;
	const bool bVisible = S.bShow && P && !P->IsNetDead() && !bDestroyed && BodyMesh;
	USkeletalMeshComponent* Body = S.Body.Get();
	if (!bVisible)
	{
		if (Body)
		{
			Body->SetVisibility(false);
		}
		return;
	}
	USceneComponent* Parent = Root;
	if (const FGun* G = S.WeaponKey ? Guns.Find(S.WeaponKey) : nullptr)
	{
		if (UPoseableMeshComponent* C = GetGunComponent(*G))
		{
			Parent = C;
		}
	}
	if (!Body)
	{
		Body = NewObject<USkeletalMeshComponent>(this);
		Body->SetupAttachment(Parent);
		Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Body->SetGenerateOverlapEvents(false);
		Body->SetCanEverAffectNavigation(false);
		Body->RegisterComponent();
		SeatBodies.Add(Body);
		S.Body = Body;
	}
	else if (Body->GetAttachParent() != Parent)
	{
		Body->AttachToComponent(Parent, FAttachmentTransformRules::KeepRelativeTransform);
	}
	Body->SetRelativeLocationAndRotation(S.Pos, S.Rot);
	if (S.ShownMesh.Get() != BodyMesh)
	{
		S.ShownMesh = BodyMesh;
		Body->SetSkeletalMeshAsset(BodyMesh);
		const USkeletalMeshComponent* Src = P->GetMesh();
		for (int32 i = 0; i < Src->GetNumMaterials(); ++i)
		{
			Body->SetMaterial(i, Src->GetMaterial(i));
		}
		UAnimSequence* Seq = ArenaArtVehicles::FindDrivingAnim(BodyMesh);
		if (!Seq && P->GetAnimPrefix())
		{
			// models outside our pool (the 2004 characters): their own animation set's Vehicle_Driving
			Seq = ArenaArt::Load<UAnimSequence>(FString(P->GetAnimPrefix()) + TEXT("Vehicle_Driving"));
			if (Seq && Seq->GetSkeleton() != BodyMesh->GetSkeleton())
			{
				Seq = nullptr;
			}
		}
		// UE2 (Vehicle.AttachDriver -> PlayAnim(DriveAnim)) leaves the model in its last pose when the mesh has no
		// DriveAnim: the idle stance, which is what is shown then
		for (const TCHAR* Fallback : { TEXT("Idle_Rest"), TEXT("Idle_Rifle"), TEXT("Crouch") })
		{
			if (Seq || !P->GetAnimPrefix())
			{
				break;
			}
			Seq = ArenaArt::Load<UAnimSequence>(FString(P->GetAnimPrefix()) + Fallback);
			if (Seq && Seq->GetSkeleton() != BodyMesh->GetSkeleton())
			{
				Seq = nullptr;
			}
		}
		S.bNoPose = Seq == nullptr; // no animation at all for this model: not drawn (a reference pose would show)
		if (Seq)
		{
			Body->PlayAnimation(Seq, true);
		}
		++NumSeatsShown;
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: %s seat: %s (%s) at %s (vehicle at %s), driving animation %s"), *Label, *BodyMesh->GetName(),
			P->GetPlayerState() ? *P->GetPlayerState()->GetPlayerName() : TEXT("?"), *Body->GetComponentLocation().ToCompactString(),
			*GetActorLocation().ToCompactString(), Seq ? *Seq->GetName() : TEXT("none"));
	}
	Body->SetVisibility(!S.bNoPose);
}

void AUE2NetVehicle::SetDestroyed(bool bInDestroyed, bool bDisintegrate)
{
	if (!bInDestroyed || bDestroyed)
	{
		return;
	}
	bDestroyed = true;
	UWorld* World = GetWorld();
	const FVector Loc = GetActorLocation();
	// as our own vehicles' explosion (AArenaONS_Vehicle::Explode)
	ArenaFX::SpawnExplosion(World, Loc, FLinearColor(1.f, 0.45f, 0.1f), bDisintegrate ? 800.f : 550.f, TEXT("Explosion.Rocket"));
	ArenaFX::SpawnExplosion(World, Loc + FVector(0.f, 0.f, 60.f), FLinearColor(1.f, 0.85f, 0.5f), 300.f, TEXT("Explosion.Grenade"));
	if (bRig && Rig.Art && Rig.Art->ExplodeSound)
	{
		if (USoundBase* Snd = ArenaArtVehicles::LoadSound(Rig.Art->ExplodeSound))
		{
			UGameplayStatics::PlaySoundAtLocation(this, Snd, Loc, 1.f, 1.f, 0.f, ArenaArtVehicles::GetAttenuation());
		}
	}
	else
	{
		ArenaAssets::PlaySoundAt(this, TEXT("S_ExplosionBig"), Loc, 1.4f, 0.85f);
	}
	if (Ambient)
	{
		Ambient->Stop();
	}
	if (bRig)
	{
		Rig.UpdateEngine(false, 0.f);
	}
	if (RigRoot)
	{
		RigRoot->SetVisibility(false, true);
	}
	for (USceneComponent* C : TArray<USceneComponent*>{ Chassis, BaseMesh })
	{
		if (C)
		{
			C->SetVisibility(false, true);
		}
	}
	for (USceneComponent* C : GunComps)
	{
		if (C)
		{
			C->SetVisibility(false, true);
		}
	}
	for (USkeletalMeshComponent* B : SeatBodies)
	{
		if (B)
		{
			B->SetVisibility(false);
		}
	}
	if (bDisintegrate)
	{
		return;
	}
	// UE2 ONSVehicle: the DestroyedVehicleMesh, still moved by its rigid body
	UStaticMesh* WreckMesh = DestroyedMesh.IsEmpty() ? nullptr : ArenaRuntimeContent::LoadStaticMeshByObjectPath(DestroyedMesh);
	if (!WreckMesh && bRig && Rig.Art && Rig.Art->Wreck)
	{
		WreckMesh = ArenaArt::Load<UStaticMesh>(Rig.Art->Wreck);
	}
	if (WreckMesh)
	{
		Wreck = NewObject<UStaticMeshComponent>(this, TEXT("Wreck"));
		Wreck->SetupAttachment(Root);
		Wreck->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Wreck->SetGenerateOverlapEvents(false);
		Wreck->SetStaticMesh(WreckMesh);
		Wreck->SetRelativeScale3D(ChassisLook.Scale);
		Wreck->RegisterComponent();
		++NumWrecks;
	}
}

void AUE2NetVehicle::NetExplosion()
{
	if (UWorld* World = GetWorld())
	{
		ArenaFX::SpawnExplosion(World, GetActorLocation() + FVector(0.f, 0.f, 40.f), FLinearColor(1.f, 0.6f, 0.2f), 220.f, TEXT("Explosion.Grenade"));
	}
}

void AUE2NetVehicle::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Ambient)
	{
		Ambient->Stop();
	}
	Super::EndPlay(EndPlayReason);
}

void AUE2NetVehicle::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bHasNet)
	{
		// extrapolate the last state along its velocity (at most 0.3 s) and ease the shown transform towards it
		const float Age = float(FMath::Min(FPlatformTime::Seconds() - NetTime, 0.3));
		const FVector Target = NetLocation + NetVelocity * Age;
		const FVector Cur = GetActorLocation() + NetVelocity * DeltaSeconds;
		const FVector NewLoc = (Target - Cur).SizeSquared() > FMath::Square(500.f) ? Target : Cur + (Target - Cur) * FMath::Min(1.f, DeltaSeconds * 10.f);
		SetActorLocationAndRotation(NewLoc, FQuat::Slerp(GetActorQuat(), NetRotation, FMath::Min(1.f, DeltaSeconds * 10.f)));
	}
	for (USceneComponent* C : TArray<USceneComponent*>{ Chassis, BaseMesh, Wreck })
	{
		UE2NetVehicleLocal::PlacePrePivot(C, ChassisLook.PrePivot);
	}
	for (const TPair<int32, FGun>& P : Guns)
	{
		if (P.Value.RigGun == INDEX_NONE)
		{
			UE2NetVehicleLocal::PlacePrePivot(P.Value.Comp.Get(), P.Value.Look.PrePivot);
		}
	}
	const float Dt = FMath::Max(DeltaSeconds, 1e-3f);
	const FRotator R = GetActorRotation();
	YawRate = FMath::FInterpTo(YawRate, FRotator::NormalizeAxis(R.Yaw - LastYaw) / Dt, DeltaSeconds, 8.f);
	LastYaw = R.Yaw;
	const float Forward = float(FVector::DotProduct(NetVelocity, GetActorForwardVector()));
	// steering from the turn the hull makes (no steering input is replicated to us): tan(angle) = wheelbase * yaw rate /
	// speed, a 3 m wheelbase
	const float TargetSteer = FMath::Abs(Forward) > 100.f
		? FMath::Clamp(FMath::RadiansToDegrees(FMath::Atan(300.f * FMath::DegreesToRadians(YawRate) / Forward)), -30.f, 30.f) : 0.f;
	SteerDegrees = FMath::FInterpTo(SteerDegrees, TargetSteer, DeltaSeconds, 6.f);
	const float SpinRadius = WheelStates.Num() > 0 ? WheelStates[0].Radius : 50.f;
	WheelSpin = FMath::Fmod(WheelSpin + Forward * DeltaSeconds / (SpinRadius * 2.f * PI) * 360.f, 360.f);
	if (!bDestroyed && WheelStates.Num() > 0)
	{
		UpdateSuspension(bRig ? Rig.Chassis.Get() : Chassis.Get(), DeltaSeconds);
	}
	if (!bRig && Chassis && !bDestroyed && WheelStates.Num() > 0)
	{
		// UE2 SVehicleWheel bones: roll about their Y axis, steer about Z, move with the suspension
		TMap<int32, FTransform> Posed;
		for (const FWheelState& W : WheelStates)
		{
			FTransform CS = UE2NetVehicleLocal::RefComponentSpace(Chassis, W.BoneIndex);
			const FQuat Delta = FRotator(0.f, SteerDegrees * W.SteerSign, 0.f).Quaternion() * FRotator(-WheelSpin, 0.f, 0.f).Quaternion();
			CS.SetRotation(Delta * CS.GetRotation());
			CS.AddToTranslation(FVector(0.f, 0.f, W.Offset));
			UE2NetVehicleLocal::PoseBone(Chassis, W.BoneIndex, CS, Posed);
		}
		Chassis->MarkRefreshTransformDirty();
		Chassis->RefreshBoneTransforms();
	}

	// weapons: turn towards the replicated aim (UE2 turrets turn at a limited rate)
	for (TPair<int32, FGun>& P : Guns)
	{
		FGun& G = P.Value;
		UPoseableMeshComponent* C = GetGunComponent(G);
		float TYaw = 0.f, TPitch = 0.f;
		if (G.bHasTarget && C && AimAngles(C, G.YawBone, G.Target, TYaw, TPitch))
		{
			if (G.RigGun != INDEX_NONE && G.RigAim == 2)
			{
				TYaw = FRotator::ClampAxis(TYaw); // the rig's rear slot rests at 180
			}
			const float RawYaw = TYaw, RawPitch = TPitch;
			if (G.bLimitYaw)
			{
				TYaw = FMath::Clamp(FRotator::NormalizeAxis(TYaw), G.YawMin, G.YawMax);
			}
			TPitch = FMath::Clamp(TPitch, G.PitchMin, G.PitchMax);
			G.bClamped = FMath::Abs(FRotator::NormalizeAxis(RawYaw - TYaw)) > 1.f || FMath::Abs(RawPitch - TPitch) > 1.f;
			G.Yaw += FRotator::NormalizeAxis(TYaw - G.Yaw) * FMath::Min(1.f, DeltaSeconds * 8.f);
			G.Pitch += (TPitch - G.Pitch) * FMath::Min(1.f, DeltaSeconds * 8.f);
			G.bSettled = FMath::Abs(FRotator::NormalizeAxis(TYaw - G.Yaw)) < 3.f && FMath::Abs(TPitch - G.Pitch) < 3.f;
		}
		if (G.RigGun == INDEX_NONE && C && !bDestroyed)
		{
			PoseGun(C, G.YawBone, G.PitchBone, G.Yaw, G.Pitch);
		}
	}

	// aim check (test evidence): once the turret has settled on a held aim point, the barrel bone's X axis should point
	// at it; logged per gun for the first checks, averaged in the world summary
	const double NowCheck = FPlatformTime::Seconds();
	if (NowCheck >= NextAimCheck && !bDestroyed)
	{
		NextAimCheck = NowCheck + 2.0;
		for (const TPair<int32, FGun>& P : Guns)
		{
			const FGun& G = P.Value;
			const UPoseableMeshComponent* C = GetGunComponent(G);
			const int32 Bone = G.PitchBone != INDEX_NONE ? G.PitchBone : G.YawBone;
			if (!G.bHasTarget || !C || Bone == INDEX_NONE || G.bClamped || !G.bSettled)
			{
				continue; // only aim points within the gun's limits, once the barrel has caught up
			}
			const FTransform BT = C->GetBoneTransform(Bone);
			const FVector ToTarget = (G.Target - BT.GetLocation()).GetSafeNormal();
			if (ToTarget.IsNearlyZero() || FVector::Dist(G.Target, BT.GetLocation()) < 300.f)
			{
				continue;
			}
			const float Err = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(float(FVector::DotProduct(BT.GetUnitAxis(EAxis::X), ToTarget)), -1.f, 1.f)));
			++NumAimChecks;
			SumAimError += Err;
			MaxAimError = FMath::Max(MaxAimError, Err);
			if (NumAimChecks <= 12)
			{
				UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: aim check %s gun %s: barrel X axis %.1f deg off the aim point %.0f m away (yaw %.0f, pitch %.0f)"), *Label,
					*C->GetSkinnedAsset()->GetName(), Err, FVector::Dist(G.Target, BT.GetLocation()) / 100.f, G.Yaw, G.Pitch);
			}
		}
	}

	if (bRig && !bDestroyed)
	{
		FArenaVehicleArtPose Pose;
		Pose.SteerDegrees = SteerDegrees;
		Pose.WheelSpin = WheelSpin;
		for (const FWheelState& W : WheelStates)
		{
			if (W.RigSuspension >= 0)
			{
				if (Pose.WheelOffsets.Num() <= W.RigSuspension)
				{
					Pose.WheelOffsets.SetNumZeroed(W.RigSuspension + 1);
				}
				Pose.WheelOffsets[W.RigSuspension] = W.Offset;
			}
		}
		Pose.ForwardSpeed = Forward;
		Pose.YawRate = YawRate;
		Pose.Thrust = bDriving ? FMath::Clamp(0.2f + float(NetVelocity.Size()) / 2500.f, 0.f, 1.f) : 0.f;
		Pose.DeltaSeconds = DeltaSeconds;
		Pose.Time = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
		for (const TPair<int32, FGun>& P : Guns)
		{
			if (P.Value.RigAim == 1)
			{
				Pose.TurretYaw = P.Value.Yaw;
				Pose.TurretPitch = P.Value.Pitch;
			}
			else if (P.Value.RigAim == 2)
			{
				Pose.RearTurretYaw = P.Value.Yaw;
				Pose.RearTurretPitch = P.Value.Pitch;
			}
		}
		Rig.UpdatePose(Pose);
	}
	for (TPair<int32, FSeat>& P : Seats)
	{
		UpdateSeat(P.Value);
	}
	if (Ambient && AmbientSound.IsValid() && !bDestroyed && !Ambient->IsPlaying())
	{
		Ambient->Play(); // engine samples are often not flagged as looping
	}
}
