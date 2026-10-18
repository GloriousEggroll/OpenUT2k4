#include "ArenaArtVehicles.h"
#include "OpenUT2k4.h"
#include "ArenaArt.h"
#include "ArenaAssets.h"
#include "ArenaONS_Vehicle.h"
#include "ArenaCharacter.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/MaterialBillboardComponent.h"
#include "Components/AudioComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture.h"
#include "Animation/AnimSequence.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"
#include "GameFramework/Actor.h"
#include "Sound/SoundAttenuation.h"
#include "UObject/StrongObjectPtr.h"
#include "Misc/Paths.h"

// =================================================================================================
// Table
// =================================================================================================
//
// Coordinates are centimetres in the vehicle's actor space (X forward, Y right, Z up, origin = centre of the
// collision box) unless noted. Imported meshes keep the source game's convention (scale 2.0 baked in), so they are
// used at scale 1. Bone positions were taken from Content/ImportedData/Assets/SkeletalMeshes.json and the exported
// skeletons; MeshOffset moves the mesh so its collision box (fitted to the mesh) is centred on the actor.

namespace ArenaArtVehiclesPrivate
{
	using namespace ArenaArtVehicles;

	TArray<FVehicleArt> BuildTable()
	{
		TArray<FVehicleArt> T;
		T.SetNum(int32(EArenaONS_VehicleType::Num));

		// ---- Wisp: hover scout ----
		{
			FVehicleArt& A = T[int32(EArenaONS_VehicleType::Wisp)];
			A.Mesh = TEXT("SkeletalMeshes/ONSVehicles-A/HoverBike");
			A.MeshOffset = FVector(20.f, 0.f, -25.f);
			A.Skins = { { TEXT("Materials/VMVehicles-TX/HoverBikeGroup_HoverBikeChassisFinalRED"), TEXT("Materials/VMVehicles-TX/HoverBikeGroup_HoverBikeChassisFinalBLUE"),
				TEXT("Textures/VMVehicles-TX/HoverBikeGroup_NewHoverCraftNOcolor") } };
			// Lift fans under the wings, plus the tail exhaust.
			A.Glows = {
				{ FVector(60.f, 150.f, -62.f), 150.f, TEXT("Textures/AW-2004Particles/Weapons_PlasmaFlare"), true },
				{ FVector(60.f, -150.f, -62.f), 150.f, TEXT("Textures/AW-2004Particles/Weapons_PlasmaFlare"), true },
				{ FVector(-190.f, 0.f, 40.f), 70.f, TEXT("Textures/EpicParticles/Flares_SoftFlare"), true },
			};
			A.Wreck = TEXT("StaticMeshes/ONSDeadVehicles-SM/HoverBikeDead");
			A.EngineSound = TEXT("Sounds/ONSVehicleSounds-S/HoverBike_HoverBikeEng01");
			A.StartSound = TEXT("Sounds/ONSVehicleSounds-S/HoverBike_HoverBikeStart01");
			A.StopSound = TEXT("Sounds/ONSVehicleSounds-S/HoverBike_HoverBikeStop01");
			A.ExplodeSound = TEXT("Sounds/ONSVehicleSounds-S/Explosions_VehicleExplosion02");
			A.GunSound[0] = TEXT("Sounds/ONSVehicleSounds-S/HoverBike_HoverBikeFire01");
			A.GunSoundInterval[0] = 0.3f;
			A.bShowDriver = true;
			A.DriverLocation = FVector(-55.f, 0.f, 45.f);
			A.HalfExtent = FVector(175.f, 120.f, 50.f);
			A.SuspensionPoints = { FVector(100.f, 80.f, -45.f), FVector(100.f, -80.f, -45.f), FVector(-100.f, 80.f, -45.f), FVector(-100.f, -80.f, -45.f) };
			A.RestLength = 100.f;
			A.RestCompression = 0.4f;
			A.WheelRadius = 0.f;
			A.CameraDistance = 640.f;
			A.CameraHeight = 200.f;
			A.Muzzles[0] = { FVector(165.f, 30.f, -40.f), FVector(165.f, -30.f, -40.f) };
		}

		// ---- Dustrunner: 4-wheel buggy ----
		{
			FVehicleArt& A = T[int32(EArenaONS_VehicleType::Dustrunner)];
			A.Mesh = TEXT("SkeletalMeshes/ONSVehicles-A/RV");
			A.MeshOffset = FVector(10.f, 0.f, -10.f);
			A.Skins = { { TEXT("Materials/VMVehicles-TX/RVGroup_RVChassisFinalRED"), TEXT("Materials/VMVehicles-TX/RVGroup_RVChassisFinalBLUE"),
				TEXT("Textures/VMVehicles-TX/RVGroup_NEWrvNoCOLOR") } };
			A.Wheels = { { TEXT("tire"), 0, true }, { TEXT("tire03"), 1, true }, { TEXT("tire02"), 2, false }, { TEXT("tire04"), 3, false } };
			{
				FGun G;
				G.Mesh = TEXT("SkeletalMeshes/ONSWeapons-A/RVnewGun");
				G.AttachBone = TEXT("ChainGunAttachment");
				G.Aim = EAim::Driver;
				G.YawBone = TEXT("rvGUNTurret");
				G.PitchBone = TEXT("rvGUNbody");
				G.MuzzleBone = TEXT("RVfirePoint");
				G.ServesGuns = { 0 };
				G.bTeamSkin = false;
				A.Guns.Add(G);
			}
			A.Wreck = TEXT("StaticMeshes/ONSDeadVehicles-SM/RVDead");
			A.EngineSound = TEXT("Sounds/ONSVehicleSounds-S/RV_RVEng01");
			A.StartSound = TEXT("Sounds/ONSVehicleSounds-S/RV_RVStart01");
			A.StopSound = TEXT("Sounds/ONSVehicleSounds-S/RV_RVStop01");
			A.ExplodeSound = TEXT("Sounds/ONSVehicleSounds-S/Explosions_VehicleExplosion01");
			A.GunSound[0] = TEXT("Sounds/ONSVehicleSounds-S/WebLauncher_WebFire");
			A.GunSoundInterval[0] = 0.25f;
			A.bShowDriver = true;
			A.DriverLocation = FVector(14.f, 0.f, 40.f);
			A.HalfExtent = FVector(180.f, 110.f, 40.f);
			// Same order as the wheel bones: front right, front left, rear right, rear left.
			A.SuspensionPoints = { FVector(130.6f, 114.9f, -40.f), FVector(130.6f, -111.7f, -40.f), FVector(-140.5f, 113.6f, -40.f), FVector(-140.3f, -111.7f, -40.f) };
			A.RestLength = 40.f;
			A.RestCompression = 0.45f;
			A.WheelRadius = 49.f;
			A.CameraDistance = 640.f;
			A.CameraHeight = 200.f;
			A.TurretPivot = FVector(-155.f, 0.f, 50.f);
			A.Muzzles[0] = { FVector(-50.f, 10.f, 87.f) };
		}

		// ---- Bulwark: tank ----
		{
			FVehicleArt& A = T[int32(EArenaONS_VehicleType::Bulwark)];
			A.Mesh = TEXT("SkeletalMeshes/ONSVehicles-A/HoverTank");
			A.MeshOffset = FVector(0.f, 0.f, -40.f);
			A.Skins = { { TEXT("Materials/VMVehicles-TX/HoverTankGroup_HoverTankChassisFinalRED"), TEXT("Materials/VMVehicles-TX/HoverTankGroup_HoverTankChassisFinalBLUE"),
				TEXT("Textures/VMVehicles-TX/HoverTankGroup_TankNoColor") } };
			for (const TCHAR* Bone : { TEXT("RollerR1"), TEXT("RollerR2"), TEXT("RollerR3"), TEXT("RollerR4"), TEXT("RollerR5") })
			{
				A.Spinners.Add({ Bone, 57.f, 1.f });
			}
			for (const TCHAR* Bone : { TEXT("RollerL1"), TEXT("RollerL2"), TEXT("RollerL3"), TEXT("RollerL4"), TEXT("RolleL5") })
			{
				A.Spinners.Add({ Bone, 57.f, -1.f });
			}
			{
				FGun G;
				G.Mesh = TEXT("SkeletalMeshes/ONSWeapons-A/HoverTankCannon");
				G.AttachBone = TEXT("TankCannonWeapon");
				G.Aim = EAim::Driver;
				G.YawBone = TEXT("TankTurret");
				G.PitchBone = TEXT("TankBarrel");
				G.MuzzleBone = TEXT("TankBarrel");
				G.MuzzleOffset = FVector(440.f, 0.f, 12.f);
				G.ServesGuns = { 0 };
				A.Guns.Add(G);
			}
			{
				// Coaxial repeater on the turret roof: yaws with the turret, pitches with the barrel.
				FGun G;
				G.Mesh = TEXT("SkeletalMeshes/ONSWeapons-A/TankMachineGun");
				G.AttachBone = TEXT("TankTurret");
				G.ParentGun = 0;
				G.AttachOffset = FVector(0.f, 0.f, 83.2f);
				G.Aim = EAim::Driver;
				G.PitchBone = TEXT("Object02");
				G.MuzzleBone = TEXT("Object02");
				G.MuzzleOffset = FVector(170.f, 0.f, 16.f);
				G.ServesGuns = { 1 };
				A.Guns.Add(G);
			}
			A.Wreck = TEXT("StaticMeshes/ONSDeadVehicles-SM/TankDead");
			A.EngineSound = TEXT("Sounds/ONSVehicleSounds-S/Tank_TankEng01");
			A.StartSound = TEXT("Sounds/ONSVehicleSounds-S/Tank_TankStart01");
			A.StopSound = TEXT("Sounds/ONSVehicleSounds-S/Tank_TankStop01");
			A.ExplodeSound = TEXT("Sounds/ONSVehicleSounds-S/Explosions_VehicleExplosion01");
			A.GunSound[0] = TEXT("Sounds/ONSVehicleSounds-S/Tank_TankFire01");
			A.GunSound[1] = TEXT("Sounds/ONSVehicleSounds-S/Tank_TankMachineGun01");
			A.HalfExtent = FVector(400.f, 265.f, 60.f);
			A.SuspensionPoints = { FVector(300.f, 215.f, -60.f), FVector(300.f, -215.f, -60.f), FVector(0.f, 215.f, -60.f), FVector(0.f, -215.f, -60.f),
				FVector(-300.f, 215.f, -60.f), FVector(-300.f, -215.f, -60.f) };
			A.RestLength = 45.f;
			A.RestCompression = 0.45f;
			A.WheelRadius = 33.f;
			A.CameraDistance = 1150.f;
			A.CameraHeight = 400.f;
			A.TurretPivot = FVector(-145.7f, 0.f, 133.3f);
			A.BarrelPivot = FVector(144.1f, 0.f, 16.3f);
			A.Muzzles[0] = { FVector(440.f, 0.f, 12.f) };
			A.Muzzles[1] = { FVector(26.f, 0.f, 88.f) };
		}

		// ---- Kestrel: gunship ----
		{
			FVehicleArt& A = T[int32(EArenaONS_VehicleType::Kestrel)];
			A.Mesh = TEXT("SkeletalMeshes/ONSVehicles-A/AttackCraft");
			A.MeshOffset = FVector(110.f, 0.f, -10.f);
			A.Skins = { { TEXT("Materials/VMVehicles-TX/AttackCraftGroup_AttackCraftChassisFinalRED"), TEXT("Materials/VMVehicles-TX/AttackCraftGroup_AttackCraftChassisFInalBLUE"),
				TEXT("Textures/VMVehicles-TX/AttackCraftGroup_AttackCraftNoColor") } };
			A.Glows = {
				{ FVector(-185.f, 72.f, 100.f), 120.f, TEXT("Textures/EpicParticles/Flares_SoftFlare"), true },
				{ FVector(-185.f, -72.f, 100.f), 120.f, TEXT("Textures/EpicParticles/Flares_SoftFlare"), true },
			};
			A.Wreck = TEXT("StaticMeshes/ONSDeadVehicles-SM/AttackCraftDead");
			A.EngineSound = TEXT("Sounds/ONSVehicleSounds-S/AttackCraft_AttackCraftEng01");
			A.StartSound = TEXT("Sounds/ONSVehicleSounds-S/AttackCraft_AttackCraftStartUp");
			A.StopSound = TEXT("Sounds/ONSVehicleSounds-S/AttackCraft_AttackCraftShutDown");
			A.ExplodeSound = TEXT("Sounds/ONSVehicleSounds-S/Explosions_VehicleExplosion03");
			A.GunSound[0] = TEXT("Sounds/ONSVehicleSounds-S/AttackCraft_AttackCraftFire01");
			A.GunSoundInterval[0] = 0.15f;
			A.HalfExtent = FVector(270.f, 155.f, 90.f);
			A.SuspensionPoints = { FVector(120.f, 70.f, -80.f), FVector(120.f, -70.f, -80.f), FVector(-160.f, 70.f, -80.f), FVector(-160.f, -70.f, -80.f) };
			A.RestLength = 110.f;
			A.RestCompression = 0.25f;
			A.WheelRadius = 0.f;
			A.CameraDistance = 900.f;
			A.CameraHeight = 280.f;
			A.Muzzles[0] = { FVector(250.f, 22.f, -100.f), FVector(250.f, -22.f, -100.f) };
			A.Muzzles[1] = { FVector(120.f, 150.f, -55.f), FVector(120.f, -150.f, -55.f) };
		}

		// ---- Caravan: transport with a rear gunner ----
		{
			FVehicleArt& A = T[int32(EArenaONS_VehicleType::Caravan)];
			A.Mesh = TEXT("SkeletalMeshes/ONSVehicles-A/PRVchassis");
			A.MeshOffset = FVector(50.f, 0.f, -40.f);
			A.Skins = { { TEXT("Materials/VMVehicles-TX/NEWprvGroup_newPRVredSHAD"), TEXT("Materials/VMVehicles-TX/NEWprvGroup_newPRVshad"),
				TEXT("Textures/VMVehicles-TX/NEWprvGroup_newPRVnoColor") } };
			// Licence tag panel: no material on the mesh (the source game fills it in from script).
			A.SlotMaterials = { { 3, TEXT("Materials/VMVehicles-TX/NEWprvGroup_PRVtag") } };
			A.Wheels = { { TEXT("RightFrontTIRE"), 0, true }, { TEXT("LeftFrontTIRE"), 1, true }, { TEXT("RightRearTIRe"), 2, false }, { TEXT("LeftRearTIRE"), 3, false } };
			{
				FGun G;
				G.Mesh = TEXT("SkeletalMeshes/ONSWeapons-A/PRVrearGUN");
				G.AttachBone = TEXT("Dummy02");
				G.Aim = EAim::Rear;
				G.YawBone = TEXT("REARgunBASE");
				G.PitchBone = TEXT("REARgunTURRET");
				G.MuzzleBone = TEXT("Dummy02");
				G.MuzzleSpread = 24.f;
				G.ServesGuns = { 0, 1 };
				A.Guns.Add(G);
			}
			{
				// Roof gun pod: decoration (the Caravan has one gunner seat).
				FGun G;
				G.Mesh = TEXT("SkeletalMeshes/ONSWeapons-A/PRVsideGun");
				G.AttachBone = TEXT("Dummy01");
				A.Guns.Add(G);
			}
			A.Wreck = TEXT("StaticMeshes/ONSDeadVehicles-SM/newPRVdead");
			A.EngineSound = TEXT("Sounds/ONSVehicleSounds-S/PRV_PRVEng01");
			A.StartSound = TEXT("Sounds/ONSVehicleSounds-S/PRV_PRVStart01");
			A.StopSound = TEXT("Sounds/ONSVehicleSounds-S/PRV_PRVStop01");
			A.ExplodeSound = TEXT("Sounds/ONSVehicleSounds-S/Explosions_VehicleExplosion04");
			A.GunSound[0] = TEXT("Sounds/ONSVehicleSounds-S/PRV_PRVFire02");
			A.GunSound[1] = TEXT("Sounds/ONSVehicleSounds-S/PRV_PRVFire04");
			A.GunSoundInterval[0] = 0.25f;
			A.HalfExtent = FVector(410.f, 230.f, 110.f);
			A.SuspensionPoints = { FVector(286.4f, 205.4f, -78.5f), FVector(286.4f, -205.4f, -78.5f), FVector(-243.6f, 205.4f, -78.5f), FVector(-243.6f, -205.4f, -78.5f) };
			A.RestLength = 45.f;
			A.RestCompression = 0.45f;
			A.WheelRadius = 89.f;
			A.CameraDistance = 1050.f;
			A.CameraHeight = 360.f;
			A.RearTurretPivot = FVector(-267.4f, -8.3f, -20.9f);
			A.RearBarrelPivot = FVector(27.6f, -2.2f, 178.5f);
			A.RearSeatOffset = FVector(0.f, 0.f, 200.f);
			A.Muzzles[0] = { FVector(176.f, 12.f, 52.f), FVector(176.f, -12.f, 52.f) };
			A.Muzzles[1] = { FVector(176.f, 0.f, 60.f) };
		}
		return T;
	}

}

USoundAttenuation* ArenaArtVehicles::GetAttenuation()
{
	{
		static TStrongObjectPtr<USoundAttenuation> Att;
		if (!Att.IsValid())
		{
			USoundAttenuation* A = NewObject<USoundAttenuation>(GetTransientPackage(), TEXT("ArenaVehicleAttenuation"));
			A->Attenuation.bAttenuate = true;
			A->Attenuation.bSpatialize = true;
			A->Attenuation.AttenuationShape = EAttenuationShape::Sphere;
			A->Attenuation.AttenuationShapeExtents = FVector(600.f, 0.f, 0.f);
			A->Attenuation.FalloffDistance = 6000.f;
			Att.Reset(A);
		}
		return Att.Get();
	}
}

const ArenaArtVehicles::FVehicleArt* ArenaArtVehicles::Find(EArenaONS_VehicleType Type)
{
	static const TArray<FVehicleArt> Table = ArenaArtVehiclesPrivate::BuildTable();
	return Table.IsValidIndex(int32(Type)) ? &Table[int32(Type)] : nullptr;
}

USkeletalMesh* ArenaArtVehicles::LoadChassis(EArenaONS_VehicleType Type)
{
	const FVehicleArt* Art = Find(Type);
	return (Art && Art->Mesh) ? ArenaArt::Load<USkeletalMesh>(Art->Mesh) : nullptr;
}

bool ArenaArtVehicles::IsAvailable(EArenaONS_VehicleType Type)
{
	return ArenaArt::IsEnabled() && LoadChassis(Type) != nullptr;
}

void ArenaArtVehicles::ApplyToSpec(const FVehicleArt& Art, FArenaONS_VehicleSpec& S)
{
	S.HalfExtent = Art.HalfExtent;
	S.SuspensionPoints = Art.SuspensionPoints;
	S.RestLength = Art.RestLength;
	S.RestCompression = Art.RestCompression;
	S.WheelRadius = Art.WheelRadius;
	S.CameraDistance = Art.CameraDistance;
	S.CameraHeight = Art.CameraHeight;
	for (int32 i = 0; i < S.Guns.Num() && i < 4; ++i)
	{
		if (Art.Muzzles[i].Num() > 0)
		{
			S.Guns[i].Muzzles = Art.Muzzles[i];
		}
	}
}

UMaterialInterface* ArenaArtVehicles::LoadSkin(const FSkin& Skin, int32 Team)
{
	const TCHAR* Path = (Team == 1 && Skin.Blue) ? Skin.Blue : Skin.Red;
	return Path ? ArenaArt::Load<UMaterialInterface>(Path) : nullptr;
}

USoundBase* ArenaArtVehicles::LoadSound(const TCHAR* Path)
{
	return Path ? ArenaArt::Load<USoundBase>(Path) : nullptr;
}

UAnimSequence* ArenaArtVehicles::FindDrivingAnim(const USkeletalMesh* CharacterMesh)
{
	if (!CharacterMesh)
	{
		return nullptr;
	}
	const FString MeshPath = CharacterMesh->GetPathName();
	for (const ArenaArt::FCharacterModel& M : ArenaArt::GetCharacterModels())
	{
		// "/Game/Imported/SkeletalMeshes/HumanMaleA/MercMaleA.MercMaleA" ends with "<Mesh>.<Name>"; meshes built from
		// the runtime content cache have transient paths, so compare with the loaded model as well
		if (MeshPath.Contains(FString(M.Mesh) + TEXT(".")) ||
			(CharacterMesh->GetName().StartsWith(FPaths::GetCleanFilename(M.Mesh)) && ArenaArt::Load<USkeletalMesh>(M.Mesh) == CharacterMesh))
		{
			UAnimSequence* Seq = ArenaArt::Load<UAnimSequence>(FString(M.AnimPrefix) + TEXT("Vehicle_Driving"));
			if (Seq && Seq->GetSkeleton() == CharacterMesh->GetSkeleton())
			{
				return Seq;
			}
			return nullptr;
		}
	}
	return nullptr;
}

// =================================================================================================
// Rig
// =================================================================================================

namespace
{
	UPoseableMeshComponent* MakePoseable(AActor* Owner, USceneComponent* Parent, USkeletalMesh* Mesh, FName Socket, const FVector& Offset)
	{
		UPoseableMeshComponent* C = NewObject<UPoseableMeshComponent>(Owner);
		C->SetSkinnedAssetAndUpdate(Mesh);
		C->SetupAttachment(Parent, Socket);
		C->SetRelativeLocation(Offset);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCollisionResponseToAllChannels(ECR_Ignore);
		C->SetGenerateOverlapEvents(false);
		C->SetCanEverAffectNavigation(false);
		C->SetCastShadow(true);
		C->bComponentUseFixedSkelBounds = true;
		C->RegisterComponent();
		return C;
	}

	int32 BoneIndexOf(const UPoseableMeshComponent* C, const TCHAR* Bone)
	{
		return (C && Bone) ? C->GetBoneIndex(FName(Bone)) : INDEX_NONE;
	}
}

bool FArenaVehicleArtRig::Build(AActor* Owner, USceneComponent* Parent, EArenaONS_VehicleType Type, int32 Team)
{
	using namespace ArenaArtVehicles;
	const FVehicleArt* A = Find(Type);
	USkeletalMesh* Mesh = LoadChassis(Type);
	if (!Owner || !Parent || !A || !Mesh || !ArenaArt::IsEnabled())
	{
		return false;
	}
	Art = A;
	Chassis = MakePoseable(Owner, Parent, Mesh, NAME_None, A->MeshOffset);
	for (const TPair<int32, const TCHAR*>& SM : A->SlotMaterials)
	{
		if (UMaterialInterface* M = ArenaArt::Load<UMaterialInterface>(SM.Value))
		{
			Chassis->SetMaterial(SM.Key, M);
		}
	}

	WheelBones.Reset();
	for (const FWheel& W : A->Wheels)
	{
		WheelBones.Add(BoneIndexOf(Chassis, W.Bone));
	}
	SpinnerBones.Reset();
	for (const FSpinner& S : A->Spinners)
	{
		SpinnerBones.Add(BoneIndexOf(Chassis, S.Bone));
	}

	Guns.Reset();
	GunYawBones.Reset();
	GunPitchBones.Reset();
	for (const FGun& G : A->Guns)
	{
		USkeletalMesh* GunMesh = ArenaArt::Load<USkeletalMesh>(G.Mesh);
		USceneComponent* GunParent = Chassis;
		if (G.ParentGun >= 0)
		{
			GunParent = Guns.IsValidIndex(G.ParentGun) ? Guns[G.ParentGun].Get() : nullptr;
		}
		UPoseableMeshComponent* GC = (GunMesh && GunParent) ? MakePoseable(Owner, GunParent, GunMesh, G.AttachBone ? FName(G.AttachBone) : NAME_None, G.AttachOffset) : nullptr;
		Guns.Add(GC);
		GunYawBones.Add(BoneIndexOf(GC, G.YawBone));
		GunPitchBones.Add(BoneIndexOf(GC, G.PitchBone));
	}

	// Glow sprites (unlit additive master with an imported flare texture).
	Glows.Reset();
	GlowMaterials.Reset();
	if (UMaterialInterface* Additive = ArenaArt::Load<UMaterialInterface>(TEXT("Materials/Masters/M_ImportedAdditive")))
	{
		for (const FGlow& G : A->Glows)
		{
			UTexture* Tex = G.Texture ? ArenaArt::Load<UTexture>(G.Texture) : nullptr;
			if (!Tex)
			{
				continue;
			}
			UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Additive, Owner);
			MID->SetTextureParameterValue(TEXT("Base"), Tex);
			MID->SetScalarParameterValue(TEXT("Intensity"), 1.f);
			UMaterialBillboardComponent* B = NewObject<UMaterialBillboardComponent>(Owner);
			B->SetupAttachment(Parent);
			B->SetRelativeLocation(G.Location);
			B->AddElement(MID, nullptr, false, G.Size * 0.5f, G.Size * 0.5f, nullptr);
			B->SetCastShadow(false);
			B->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			B->RegisterComponent();
			Glows.Add(B);
			GlowMaterials.Add(MID);
		}
	}

	// Engine loop (restarted while running; most engine samples are not flagged as looping).
	if (USoundBase* EngineSnd = LoadSound(A->EngineSound))
	{
		Engine = NewObject<UAudioComponent>(Owner);
		Engine->SetupAttachment(Parent);
		Engine->bAutoActivate = false;
		Engine->bStopWhenOwnerDestroyed = true;
		Engine->SetSound(EngineSnd);
		Engine->AttenuationSettings = GetAttenuation();
		Engine->RegisterComponent();
	}

	SkinTeam = -2;
	SetTeam(Team);
	UpdatePose(FArenaVehicleArtPose());
	return true;
}

void FArenaVehicleArtRig::SetTeam(int32 Team)
{
	using namespace ArenaArtVehicles;
	if (!IsValid() || SkinTeam == Team)
	{
		return;
	}
	SkinTeam = Team;
	const FLinearColor TeamCol = (Team == 0 || Team == 1) ? ArenaTeam::Color(Team) : FLinearColor(0.6f, 0.6f, 0.6f);

	// Rebuilt skins (once): uncoloured diffuse, tinted per team.
	AActor* Owner = Chassis->GetOwner();
	if (SkinMaterials.Num() != Art->Skins.Num())
	{
		SkinMaterials.Reset();
		UMaterialInterface* Opaque = ArenaArt::Load<UMaterialInterface>(TEXT("Materials/Masters/M_ImportedOpaque"));
		for (const FSkin& S : Art->Skins)
		{
			UTexture* Diffuse = S.Diffuse ? ArenaArt::Load<UTexture>(S.Diffuse) : nullptr;
			UMaterialInstanceDynamic* Base = nullptr;
			if (Opaque && Diffuse)
			{
				Base = UMaterialInstanceDynamic::Create(Opaque, Owner);
				Base->SetTextureParameterValue(TEXT("Base"), Diffuse);
				Base->SetScalarParameterValue(TEXT("Roughness"), 0.55f);
				Base->SetScalarParameterValue(TEXT("Specular"), 0.45f);
			}
			SkinMaterials.Add(Base);
		}
	}
	const FLinearColor DiffuseTint = Team == 0 ? FLinearColor(1.f, 0.56f, 0.5f) : Team == 1 ? FLinearColor(0.5f, 0.64f, 1.f) : FLinearColor::White;
	for (UMaterialInstanceDynamic* M : SkinMaterials)
	{
		if (M)
		{
			M->SetVectorParameterValue(TEXT("Tint"), DiffuseTint);
		}
	}
	if (Team == 0 || Team == 1)
	{
		if (!SkinRim)
		{
			SkinRim = ArenaAssets::MakeGlow(Owner, TeamCol, 0.3f, 0.12f);
		}
		else
		{
			SkinRim->SetVectorParameterValue(TEXT("Color"), TeamCol);
		}
	}

	auto Apply = [this, Team](UPoseableMeshComponent* C)
	{
		const USkeletalMesh* Mesh = C ? Cast<USkeletalMesh>(C->GetSkinnedAsset()) : nullptr;
		if (!Mesh)
		{
			return;
		}
		const TArray<FSkeletalMaterial>& Slots = Mesh->GetMaterials();
		for (int32 i = 0; i < Slots.Num(); ++i)
		{
			const UMaterialInterface* Orig = Slots[i].MaterialInterface;
			const FString OrigName = Orig ? Orig->GetName() : FString();
			for (int32 k = 0; k < Art->Skins.Num(); ++k)
			{
				const FSkin& S = Art->Skins[k];
				if (OrigName != FPaths::GetBaseFilename(S.Red) && !(S.Blue && OrigName == FPaths::GetBaseFilename(S.Blue)))
				{
					continue;
				}
				if (UMaterialInstanceDynamic* Base = SkinMaterials.IsValidIndex(k) ? SkinMaterials[k].Get() : nullptr)
				{
					C->SetMaterial(i, Base);
					C->SetOverlayMaterial((Team == 0 || Team == 1) ? SkinRim.Get() : nullptr, true, i);
				}
				else if (UMaterialInterface* M = LoadSkin(S, Team))
				{
					C->SetMaterial(i, M);
				}
			}
		}
	};
	Apply(Chassis);
	for (int32 g = 0; g < Guns.Num(); ++g)
	{
		if (Art->Guns.IsValidIndex(g) && Art->Guns[g].bTeamSkin)
		{
			Apply(Guns[g]);
		}
	}
	const FLinearColor Tint = (Team == 0 || Team == 1) ? TeamCol : FLinearColor(0.6f, 0.8f, 1.f);
	for (UMaterialInstanceDynamic* MID : GlowMaterials)
	{
		if (MID)
		{
			MID->SetVectorParameterValue(TEXT("Tint"), Tint);
		}
	}
}

FTransform FArenaVehicleArtRig::GetRefComponentSpace(const UPoseableMeshComponent* Mesh, int32 BoneIndex) const
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

void FArenaVehicleArtRig::PoseBone(UPoseableMeshComponent* Mesh, int32 BoneIndex, const FTransform& ComponentSpace, TMap<int32, FTransform>& Posed) const
{
	const USkinnedAsset* Asset = Mesh ? Mesh->GetSkinnedAsset() : nullptr;
	if (!Asset || BoneIndex == INDEX_NONE || !Mesh->BoneSpaceTransforms.IsValidIndex(BoneIndex))
	{
		return;
	}
	const FReferenceSkeleton& Ref = Asset->GetRefSkeleton();
	// Current component-space transform of the parent: posed this frame, or reference pose under posed ancestors.
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

void FArenaVehicleArtRig::UpdatePose(const FArenaVehicleArtPose& Pose)
{
	using namespace ArenaArtVehicles;
	if (!IsValid())
	{
		return;
	}

	// ---- chassis: wheels and rollers ----
	{
		TMap<int32, FTransform> Posed;
		for (int32 i = 0; i < Art->Wheels.Num() && i < WheelBones.Num(); ++i)
		{
			const int32 Bone = WheelBones[i];
			if (Bone == INDEX_NONE)
			{
				continue;
			}
			const FWheel& W = Art->Wheels[i];
			FTransform CS = GetRefComponentSpace(Chassis, Bone);
			const float Offset = Pose.WheelOffsets.IsValidIndex(W.SuspensionIndex) ? Pose.WheelOffsets[W.SuspensionIndex] : 0.f;
			const FQuat Delta = FRotator(0.f, W.bSteer ? Pose.SteerDegrees : 0.f, 0.f).Quaternion() * FRotator(-Pose.WheelSpin, 0.f, 0.f).Quaternion();
			CS.SetRotation(Delta * CS.GetRotation());
			CS.AddToTranslation(FVector(0.f, 0.f, Offset));
			PoseBone(Chassis, Bone, CS, Posed);
		}
		if (Art->Spinners.Num() > 0)
		{
			// Each side rolls with its own ground speed (pivot turns spin the sides in opposite directions).
			const float YawRad = FMath::DegreesToRadians(Pose.YawRate);
			for (int32 Side = 0; Side < 2; ++Side)
			{
				const float SideSign = Side == 0 ? 1.f : -1.f;
				const float Radius = Art->Spinners[0].Radius;
				const float V = Pose.ForwardSpeed - SideSign * YawRad * 215.f;
				RollerSpin[Side] = FMath::Fmod(RollerSpin[Side] + V * Pose.DeltaSeconds / (Radius * 2.f * PI) * 360.f, 360.f);
			}
			for (int32 i = 0; i < Art->Spinners.Num() && i < SpinnerBones.Num(); ++i)
			{
				const int32 Bone = SpinnerBones[i];
				if (Bone == INDEX_NONE)
				{
					continue;
				}
				FTransform CS = GetRefComponentSpace(Chassis, Bone);
				const float Spin = RollerSpin[Art->Spinners[i].Side > 0.f ? 0 : 1];
				CS.SetRotation(FRotator(-Spin, 0.f, 0.f).Quaternion() * CS.GetRotation());
				PoseBone(Chassis, Bone, CS, Posed);
			}
		}
		if (Posed.Num() > 0)
		{
			Chassis->MarkRefreshTransformDirty();
			Chassis->RefreshBoneTransforms();
		}
	}

	// ---- guns: yaw / pitch bones follow the aim ----
	for (int32 g = 0; g < Guns.Num() && g < Art->Guns.Num(); ++g)
	{
		UPoseableMeshComponent* GC = Guns[g];
		const FGun& G = Art->Guns[g];
		if (!GC || G.Aim == EAim::None)
		{
			continue;
		}
		const float Yaw = G.Aim == EAim::Rear ? Pose.RearTurretYaw : Pose.TurretYaw;
		const float Pitch = G.Aim == EAim::Rear ? Pose.RearTurretPitch : Pose.TurretPitch;
		TMap<int32, FTransform> Posed;
		const int32 YawBone = GunYawBones[g];
		const int32 PitchBone = GunPitchBones[g];
		const FQuat YawQ = FRotator(0.f, YawBone != INDEX_NONE ? Yaw : 0.f, 0.f).Quaternion();
		FVector YawPivot = FVector::ZeroVector;
		if (YawBone != INDEX_NONE)
		{
			FTransform CS = GetRefComponentSpace(GC, YawBone);
			YawPivot = CS.GetLocation();
			CS.SetRotation(YawQ * CS.GetRotation());
			PoseBone(GC, YawBone, CS, Posed);
		}
		if (PitchBone != INDEX_NONE)
		{
			FTransform CS = GetRefComponentSpace(GC, PitchBone);
			const FQuat Full = FRotator(Pitch, YawBone != INDEX_NONE ? Yaw : 0.f, 0.f).Quaternion();
			CS.SetRotation(Full * CS.GetRotation());
			CS.SetLocation(YawPivot + YawQ.RotateVector(CS.GetLocation() - YawPivot));
			PoseBone(GC, PitchBone, CS, Posed);
		}
		if (Posed.Num() > 0)
		{
			GC->MarkRefreshTransformDirty();
			GC->RefreshBoneTransforms();
		}
	}

	// ---- glow sprites ----
	for (int32 i = 0; i < GlowMaterials.Num(); ++i)
	{
		if (UMaterialInstanceDynamic* MID = GlowMaterials[i])
		{
			const bool bThruster = Art->Glows.IsValidIndex(i) ? Art->Glows[i].bThruster : true;
			const float Flicker = 0.9f + 0.1f * FMath::Sin(Pose.Time * 31.f + i * 1.7f);
			MID->SetScalarParameterValue(TEXT("Intensity"), bThruster ? (0.35f + 1.4f * Pose.Thrust) * Flicker : 1.2f);
		}
	}
}

void FArenaVehicleArtRig::UpdateEngine(bool bRunning, float Load)
{
	if (!Engine)
	{
		return;
	}
	if (bRunning)
	{
		Engine->SetPitchMultiplier(0.85f + 0.4f * FMath::Clamp(Load, 0.f, 1.f));
		Engine->SetVolumeMultiplier(0.55f);
		if (!Engine->IsPlaying())
		{
			Engine->Play();
		}
	}
	else if (Engine->IsPlaying())
	{
		Engine->Stop();
	}
}

bool FArenaVehicleArtRig::SetDriver(const AArenaCharacter* Char)
{
	if (!IsValid() || !Art->bShowDriver)
	{
		return false;
	}
	if (SeatedChar.Get() == Char && (Char == nullptr || (Driver && Driver->IsVisible())))
	{
		return Char != nullptr && Driver && Driver->IsVisible();
	}
	SeatedChar = Char;
	const USkeletalMeshComponent* Body = Char ? Char->GetMesh() : nullptr;
	USkeletalMesh* BodyMesh = Body ? Body->GetSkeletalMeshAsset() : nullptr;
	UAnimSequence* Seq = ArenaArtVehicles::FindDrivingAnim(BodyMesh);
	if (!BodyMesh || !Seq)
	{
		if (Driver)
		{
			Driver->SetVisibility(false);
		}
		return false;
	}
	if (!Driver)
	{
		AActor* Owner = Chassis->GetOwner();
		Driver = NewObject<USkeletalMeshComponent>(Owner);
		Driver->SetupAttachment(Chassis->GetAttachParent());
		Driver->SetRelativeLocationAndRotation(Art->DriverLocation, Art->DriverRotation);
		Driver->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Driver->SetGenerateOverlapEvents(false);
		Driver->SetCanEverAffectNavigation(false);
		Driver->RegisterComponent();
	}
	Driver->SetSkeletalMeshAsset(BodyMesh);
	for (int32 i = 0; i < Body->GetNumMaterials(); ++i)
	{
		Driver->SetMaterial(i, Body->GetMaterial(i));
	}
	Driver->PlayAnimation(Seq, true);
	Driver->SetVisibility(true);
	return true;
}

bool FArenaVehicleArtRig::GetMuzzle(int32 GunIndex, int32 Shot, FVector& OutLocation) const
{
	if (!IsValid())
	{
		return false;
	}
	for (int32 g = 0; g < Guns.Num() && g < Art->Guns.Num(); ++g)
	{
		const ArenaArtVehicles::FGun& G = Art->Guns[g];
		const UPoseableMeshComponent* GC = Guns[g];
		if (!GC || !G.MuzzleBone || !G.ServesGuns.Contains(GunIndex))
		{
			continue;
		}
		const FName Bone(G.MuzzleBone);
		if (GC->GetBoneIndex(Bone) == INDEX_NONE)
		{
			return false;
		}
		const FTransform T = GC->GetSocketTransform(Bone, RTS_World);
		FVector Local = G.MuzzleOffset;
		if (G.MuzzleSpread > 0.f)
		{
			Local.Y += (Shot % 2 == 0 ? 0.5f : -0.5f) * G.MuzzleSpread;
		}
		OutLocation = T.TransformPosition(Local);
		return true;
	}
	return false;
}

FTransform FArenaVehicleArtRig::GetChassisTransform() const
{
	return Chassis ? Chassis->GetComponentTransform() : FTransform::Identity;
}
