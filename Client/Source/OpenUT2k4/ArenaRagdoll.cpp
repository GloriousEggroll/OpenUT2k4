#include "ArenaRagdoll.h"
#include "OpenUT2k4.h"
#include "ArenaArt.h"
#include "ArenaArtFX.h"
#include "ArenaRuntimeContent.h"
#include "ArenaTypes.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "ReferenceSkeleton.h"
#include "Sound/SoundBase.h"
#include "UObject/StrongObjectPtr.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UnrealClient.h"

namespace ArenaRagdollPrivate
{
	static TAutoConsoleVariable<int32> CVarDebugRagdoll(
		TEXT("arena.DebugRagdoll"), 0,
		TEXT("1 = log the bodies and joints of every physics asset built for a skeleton."),
		ECVF_Default);

	enum class EPart : uint8
	{
		None,
		Pelvis,
		Spine,
		Head,
		UpperArm,
		Forearm,
		Hand,
		Thigh,
		Calf,
		Foot
	};

	EPart Classify(const FString& BoneName)
	{
		const FString N = BoneName.ToLower();
		const TCHAR* const Skip[] = { TEXT("finger"), TEXT("toe"), TEXT("weapon"), TEXT("twist"), TEXT("ponytail"), TEXT("clavicle"),
			TEXT("neck"), TEXT("tail"), TEXT("nub"), TEXT("jaw"), TEXT("eye") };
		for (const TCHAR* S : Skip)
		{
			if (N.Contains(S))
			{
				return EPart::None;
			}
		}
		if (N.Contains(TEXT("pelvis"))) { return EPart::Pelvis; }
		if (N.Contains(TEXT("spine"))) { return EPart::Spine; }
		if (N.Contains(TEXT("upperarm"))) { return EPart::UpperArm; }
		if (N.Contains(TEXT("forearm"))) { return EPart::Forearm; }
		if (N.Contains(TEXT("hand"))) { return EPart::Hand; }
		if (N.Contains(TEXT("thigh"))) { return EPart::Thigh; }
		if (N.Contains(TEXT("calf"))) { return EPart::Calf; }
		if (N.Contains(TEXT("foot"))) { return EPart::Foot; }
		if (N.Contains(TEXT("head"))) { return EPart::Head; }
		return EPart::None;
	}

	/** The part a limb body's capsule runs towards (None = to the bone's farthest descendant). */
	EPart NextInChain(EPart P)
	{
		switch (P)
		{
		case EPart::Pelvis:   return EPart::Spine;
		case EPart::Spine:    return EPart::Spine;
		case EPart::UpperArm: return EPart::Forearm;
		case EPart::Forearm:  return EPart::Hand;
		case EPart::Thigh:    return EPart::Calf;
		case EPart::Calf:     return EPart::Foot;
		default:              return EPart::None;
		}
	}

	/** Limb radius as a share of the skeleton height. */
	float LimbRadius(EPart P)
	{
		switch (P)
		{
		case EPart::UpperArm: return 0.036f;
		case EPart::Forearm:  return 0.030f;
		case EPart::Hand:     return 0.028f;
		case EPart::Thigh:    return 0.050f;
		case EPart::Calf:     return 0.040f;
		case EPart::Foot:     return 0.030f;
		default:              return 0.040f;
		}
	}

	/** Joint limits (swing 1, swing 2, twist; degrees) of the joint between a body and its parent body. */
	FVector JointLimits(EPart P)
	{
		switch (P)
		{
		case EPart::Spine:    return FVector(22.f, 22.f, 15.f);
		case EPart::Head:     return FVector(35.f, 35.f, 30.f);
		case EPart::UpperArm: return FVector(75.f, 75.f, 40.f);
		case EPart::Forearm:  return FVector(70.f, 20.f, 15.f);
		case EPart::Hand:     return FVector(35.f, 35.f, 15.f);
		case EPart::Thigh:    return FVector(55.f, 40.f, 15.f);
		case EPart::Calf:     return FVector(70.f, 15.f, 10.f);
		case EPart::Foot:     return FVector(25.f, 25.f, 10.f);
		default:              return FVector(30.f, 30.f, 20.f);
		}
	}

	UPhysicsAsset* BuildPhysicsAsset(USkeletalMesh* Mesh)
	{
		const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
		const int32 Num = Ref.GetNum();
		if (Num < 6)
		{
			return nullptr;
		}
		// Component-space reference pose
		TArray<FTransform> CS;
		CS.SetNum(Num);
		const TArray<FTransform>& Local = Ref.GetRefBonePose();
		for (int32 i = 0; i < Num; ++i)
		{
			const int32 Parent = Ref.GetParentIndex(i);
			CS[i] = Parent >= 0 ? Local[i] * CS[Parent] : Local[i];
		}

		TArray<EPart> Part;
		Part.Init(EPart::None, Num);
		float MinZ = TNumericLimits<float>::Max(), MaxZ = -TNumericLimits<float>::Max();
		int32 Count = 0;
		int32 PelvisIndex = INDEX_NONE;
		for (int32 i = 0; i < Num; ++i)
		{
			Part[i] = Classify(Ref.GetBoneName(i).ToString());
			if (Part[i] == EPart::Pelvis && PelvisIndex != INDEX_NONE)
			{
				Part[i] = EPart::None; // one pelvis
			}
			if (Part[i] != EPart::None)
			{
				++Count;
				PelvisIndex = Part[i] == EPart::Pelvis ? i : PelvisIndex;
				MinZ = FMath::Min(MinZ, float(CS[i].GetLocation().Z));
				MaxZ = FMath::Max(MaxZ, float(CS[i].GetLocation().Z));
			}
		}
		if (PelvisIndex == INDEX_NONE || Count < 6)
		{
			return nullptr;
		}
		// Skeleton height: feet to head bone plus a head's worth.
		const float H = FMath::Max(20.f, (MaxZ - MinZ) * 1.08f);

		auto Pos = [&CS](int32 i) { return CS[i].GetLocation(); };
		auto IsAncestor = [&Ref](int32 Ancestor, int32 Bone)
		{
			for (int32 b = Ref.GetParentIndex(Bone); b >= 0; b = Ref.GetParentIndex(b))
			{
				if (b == Ancestor)
				{
					return true;
				}
			}
			return false;
		};

		// Thin the spine: keep a spine bone only when it is far enough from the previous torso body.
		for (int32 i = 0; i < Num; ++i)
		{
			if (Part[i] != EPart::Spine)
			{
				continue;
			}
			int32 Prev = Ref.GetParentIndex(i);
			while (Prev >= 0 && Part[Prev] != EPart::Spine && Part[Prev] != EPart::Pelvis)
			{
				Prev = Ref.GetParentIndex(Prev);
			}
			if (Prev >= 0 && FVector::Dist(Pos(i), Pos(Prev)) < 0.08f * H)
			{
				// merge into the previous one unless that one is the pelvis (then this one replaces nothing: drop it)
				Part[i] = EPart::None;
			}
		}
		// Only body bones whose ancestors chain up to the pelvis (or the pelvis' own ancestors, e.g. a thigh under bip01)
		TArray<int32> Bodies;
		for (int32 i = 0; i < Num; ++i)
		{
			if (Part[i] != EPart::None)
			{
				Bodies.Add(i);
			}
		}

		// Widths across hips and shoulders from the left / right limb roots.
		auto FindPair = [&](EPart P, FVector& OutL, FVector& OutR) -> bool
		{
			int32 L = INDEX_NONE, R = INDEX_NONE;
			for (int32 b : Bodies)
			{
				if (Part[b] != P)
				{
					continue;
				}
				const FString N = Ref.GetBoneName(b).ToString().ToLower();
				const bool bLeft = N.Contains(TEXT("-l-")) || N.Contains(TEXT(" l ")) || N.Contains(TEXT("_l_")) || N.Contains(TEXT("left")) || N.EndsWith(TEXT("_l"));
				(bLeft ? L : R) = b;
			}
			if (L == INDEX_NONE || R == INDEX_NONE)
			{
				return false;
			}
			OutL = Pos(L);
			OutR = Pos(R);
			return true;
		};
		FVector HipL = Pos(PelvisIndex), HipR = HipL, ShoulderL = HipL + FVector(0.f, 0.f, 0.3f * H), ShoulderR = ShoulderL;
		const bool bHips = FindPair(EPart::Thigh, HipL, HipR);
		const bool bShoulders = FindPair(EPart::UpperArm, ShoulderL, ShoulderR);
		const float HipWidth = bHips ? FVector::Dist(HipL, HipR) : 0.16f * H;
		const float ShoulderWidth = bShoulders ? FVector::Dist(ShoulderL, ShoulderR) : 0.22f * H;
		const FVector HipAxis = bHips ? (HipL - HipR).GetSafeNormal() : FVector::RightVector;
		const FVector ShoulderAxis = bShoulders ? (ShoulderL - ShoulderR).GetSafeNormal() : HipAxis;

		// Segment end of a body: the next body of its chain below it, else the farthest descendant.
		auto SegmentEnd = [&](int32 b, bool& bOutFound) -> FVector
		{
			bOutFound = false;
			const EPart Next = NextInChain(Part[b]);
			float BestDepth = TNumericLimits<float>::Max();
			FVector End = Pos(b);
			if (Next != EPart::None)
			{
				for (int32 c : Bodies)
				{
					if (c != b && Part[c] == Next && IsAncestor(b, c))
					{
						const float D = FVector::Dist(Pos(b), Pos(c));
						if (D < BestDepth)
						{
							BestDepth = D;
							End = Pos(c);
							bOutFound = true;
						}
					}
				}
				if (Part[b] == EPart::Spine && !bOutFound)
				{
					// chest: up to the head
					for (int32 c : Bodies)
					{
						if (Part[c] == EPart::Head && IsAncestor(b, c))
						{
							End = Pos(c);
							bOutFound = true;
						}
					}
				}
			}
			if (!bOutFound)
			{
				float Far = 0.f;
				for (int32 c = b + 1; c < Num; ++c)
				{
					if (IsAncestor(b, c) && !Ref.GetBoneName(c).ToString().ToLower().Contains(TEXT("weapon")))
					{
						const float D = FVector::Dist(Pos(b), Pos(c));
						if (D > Far)
						{
							Far = D;
							End = Pos(c);
						}
					}
				}
				bOutFound = Far > 0.02f * H;
			}
			return End;
		};

		UPhysicsAsset* PA = NewObject<UPhysicsAsset>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UPhysicsAsset::StaticClass(),
			FName(*(Mesh->GetName() + TEXT("_Ragdoll")))), RF_Transient);
		const bool bDebug = CVarDebugRagdoll.GetValueOnGameThread() != 0;

		for (int32 b : Bodies)
		{
			USkeletalBodySetup* BS = NewObject<USkeletalBodySetup>(PA);
			BS->BoneName = Ref.GetBoneName(b);
			BS->PhysicsType = PhysType_Default;
			BS->CollisionTraceFlag = CTF_UseSimpleAsComplex;
			BS->DefaultInstance.LinearDamping = 0.1f;
			BS->DefaultInstance.AngularDamping = 1.f;
			BS->DefaultInstance.SetUseCCD(true);

			const FTransform& BoneTM = CS[b];
			bool bFound = false;
			const FVector A = Pos(b);
			FVector B = SegmentEnd(b, bFound);
			const EPart P = Part[b];
			if (P == EPart::Head)
			{
				// Sphere above the neck along the head bone's direction from its parent.
				const int32 Parent = Ref.GetParentIndex(b);
				FVector Up = Parent >= 0 ? (A - Pos(Parent)).GetSafeNormal() : FVector::UpVector;
				if (Up.IsNearlyZero())
				{
					Up = FVector::UpVector;
				}
				FKSphereElem Sphere(0.065f * H);
				Sphere.Center = BoneTM.InverseTransformPosition(A + Up * 0.045f * H);
				BS->AggGeom.SphereElems.Add(Sphere);
			}
			else if (P == EPart::Pelvis || P == EPart::Spine)
			{
				// Torso: a capsule across the body (hips for the pelvis, blending to the shoulders up the spine).
				if (!bFound)
				{
					B = A + FVector::UpVector * 0.12f * H;
				}
				const float Seg = FVector::Dist(A, B);
				const float Radius = FMath::Clamp(Seg * 0.5f, 0.05f * H, 0.085f * H);
				const float Height01 = FMath::Clamp(float(A.Z - HipL.Z) / FMath::Max(1.f, float(ShoulderL.Z - HipL.Z)), 0.f, 1.f);
				const float Width = P == EPart::Pelvis ? HipWidth * 1.1f : FMath::Lerp(HipWidth * 1.1f, ShoulderWidth * 0.8f, Height01);
				const FVector Axis = (P == EPart::Pelvis ? HipAxis : FMath::Lerp(HipAxis, ShoulderAxis, Height01)).GetSafeNormal();
				FKSphylElem Capsule(Radius, FMath::Max(1.f, Width - Radius));
				Capsule.Center = BoneTM.InverseTransformPosition(FMath::Lerp(A, B, P == EPart::Pelvis ? 0.35f : 0.5f));
				Capsule.Rotation = FQuat::FindBetweenNormals(FVector::UpVector, BoneTM.InverseTransformVectorNoScale(Axis).GetSafeNormal()).Rotator();
				BS->AggGeom.SphylElems.Add(Capsule);
			}
			else
			{
				// Limb: a capsule along the segment (terminal parts: towards their farthest descendant, or along the bone).
				if (!bFound)
				{
					B = A + BoneTM.GetUnitAxis(EAxis::X) * 0.07f * H;
				}
				const float Seg = FMath::Max(1.f, float(FVector::Dist(A, B)));
				const float Radius = FMath::Min(LimbRadius(P) * H, Seg * 0.45f);
				FKSphylElem Capsule(Radius, FMath::Max(1.f, Seg - Radius));
				Capsule.Center = BoneTM.InverseTransformPosition((A + B) * 0.5f);
				Capsule.Rotation = FQuat::FindBetweenNormals(FVector::UpVector, BoneTM.InverseTransformVectorNoScale((B - A).GetSafeNormal()).GetSafeNormal()).Rotator();
				BS->AggGeom.SphylElems.Add(Capsule);
			}
			PA->SkeletalBodySetups.Add(BS);
		}

		// Joints: each body to the nearest ancestor body.
		TArray<int32> ParentBody;
		ParentBody.Init(INDEX_NONE, Bodies.Num());
		for (int32 k = 0; k < Bodies.Num(); ++k)
		{
			const int32 b = Bodies[k];
			for (int32 p = Ref.GetParentIndex(b); p >= 0 && ParentBody[k] == INDEX_NONE; p = Ref.GetParentIndex(p))
			{
				ParentBody[k] = Bodies.IndexOfByKey(p);
			}
			if (ParentBody[k] == INDEX_NONE)
			{
				continue; // root body (pelvis), or a body above it (thighs under the root in some rigs get the pelvis below)
			}
			const int32 p = Bodies[ParentBody[k]];
			UPhysicsConstraintTemplate* T = NewObject<UPhysicsConstraintTemplate>(PA);
			FConstraintInstance& CI = T->DefaultInstance;
			CI.JointName = Ref.GetBoneName(b);
			CI.ConstraintBone1 = Ref.GetBoneName(b);
			CI.ConstraintBone2 = Ref.GetBoneName(p);
			CI.SetRefFrame(EConstraintFrame::Frame1, FTransform::Identity);
			CI.SetRefFrame(EConstraintFrame::Frame2, CS[b].GetRelativeTransform(CS[p]));
			CI.SetLinearXLimit(LCM_Locked, 0.f);
			CI.SetLinearYLimit(LCM_Locked, 0.f);
			CI.SetLinearZLimit(LCM_Locked, 0.f);
			const FVector Limits = JointLimits(Part[b]);
			CI.SetAngularSwing1Limit(ACM_Limited, Limits.X);
			CI.SetAngularSwing2Limit(ACM_Limited, Limits.Y);
			CI.SetAngularTwistLimit(ACM_Limited, Limits.Z);
			CI.SetDisableCollision(true);
			PA->ConstraintSetup.Add(T);
		}
		// Thighs attached to a bone above the pelvis (bip01 rigs parent them to the pelvis, others to the root): joint
		// them to the pelvis instead so the legs stay on.
		const int32 PelvisBody = Bodies.IndexOfByKey(PelvisIndex);
		for (int32 k = 0; k < Bodies.Num(); ++k)
		{
			if (ParentBody[k] == INDEX_NONE && k != PelvisBody && PelvisBody != INDEX_NONE)
			{
				const int32 b = Bodies[k];
				UPhysicsConstraintTemplate* T = NewObject<UPhysicsConstraintTemplate>(PA);
				FConstraintInstance& CI = T->DefaultInstance;
				CI.JointName = Ref.GetBoneName(b);
				CI.ConstraintBone1 = Ref.GetBoneName(b);
				CI.ConstraintBone2 = Ref.GetBoneName(PelvisIndex);
				CI.SetRefFrame(EConstraintFrame::Frame1, FTransform::Identity);
				CI.SetRefFrame(EConstraintFrame::Frame2, CS[b].GetRelativeTransform(CS[PelvisIndex]));
				const FVector Limits = JointLimits(Part[b]);
				CI.SetAngularSwing1Limit(ACM_Limited, Limits.X);
				CI.SetAngularSwing2Limit(ACM_Limited, Limits.Y);
				CI.SetAngularTwistLimit(ACM_Limited, Limits.Z);
				CI.SetDisableCollision(true);
				PA->ConstraintSetup.Add(T);
				ParentBody[k] = PelvisBody;
			}
		}

		PA->UpdateBodySetupIndexMap();
		PA->UpdateBoundsBodiesArray();
		// No collision between siblings and grandparents (shoulders vs chest vs head, thighs vs belly).
		for (int32 i = 0; i < Bodies.Num(); ++i)
		{
			for (int32 j = i + 1; j < Bodies.Num(); ++j)
			{
				const int32 Pi = ParentBody[i], Pj = ParentBody[j];
				const bool bSiblings = Pi != INDEX_NONE && Pi == Pj;
				const bool bGrand = (Pi != INDEX_NONE && ParentBody[Pi] == j) || (Pj != INDEX_NONE && ParentBody[Pj] == i);
				const bool bTorso = (Part[Bodies[i]] == EPart::Pelvis || Part[Bodies[i]] == EPart::Spine) && (Part[Bodies[j]] == EPart::Pelvis || Part[Bodies[j]] == EPart::Spine);
				if (bSiblings || bGrand || bTorso)
				{
					PA->DisableCollision(i, j);
				}
			}
		}

		UE_LOG(LogOpenUT2k4, Log, TEXT("Ragdoll: physics asset for %s - %d bodies, %d joints (height %.0f)"), *Mesh->GetName(), PA->SkeletalBodySetups.Num(), PA->ConstraintSetup.Num(), H);
		if (bDebug)
		{
			for (int32 k = 0; k < Bodies.Num(); ++k)
			{
				const USkeletalBodySetup* BS = PA->SkeletalBodySetups[k];
				const FKAggregateGeom& G = BS->AggGeom;
				UE_LOG(LogOpenUT2k4, Log, TEXT("Ragdoll:   %-22s part %d parent %s %s"), *BS->BoneName.ToString(), int32(Part[Bodies[k]]),
					ParentBody[k] != INDEX_NONE ? *Ref.GetBoneName(Bodies[ParentBody[k]]).ToString() : TEXT("-"),
					G.SphylElems.Num() ? *FString::Printf(TEXT("capsule r %.1f len %.1f"), G.SphylElems[0].Radius, G.SphylElems[0].Length)
						: *FString::Printf(TEXT("sphere r %.1f"), G.SphereElems.Num() ? G.SphereElems[0].Radius : 0.f));
			}
		}
		return PA;
	}

	struct FGibPart
	{
		const TCHAR* Organic;
		const TCHAR* Robot;
		/** Largest dimension in cm. */
		float Size;
	};
	// xPawn.SpawnGibs: GibCountTorso 2, GibCountHead 2, GibCountForearm 2 (EGT_UpperArm pieces), GibCountUpperArm 2
	// (EGT_Forearm pieces). The calves and hands are the extra pieces of a severed limb; a whole-body gib uses these.
	const FGibPart GibParts[] = {
		{ TEXT("StaticMeshes/XEffects/GibOrganicTorso"),    TEXT("StaticMeshes/XEffects/GibBotTorso"),    44.f },
		{ TEXT("StaticMeshes/XEffects/GibOrganicTorso"),    TEXT("StaticMeshes/XEffects/GibBotTorso"),    40.f },
		{ TEXT("StaticMeshes/XEffects/GibOrganicHead"),     TEXT("StaticMeshes/XEffects/GibBotHead"),     26.f },
		{ TEXT("StaticMeshes/XEffects/GibOrganicHead"),     TEXT("StaticMeshes/XEffects/GibBotHead"),     22.f },
		{ TEXT("StaticMeshes/XEffects/GibOrganicUpperarm"), TEXT("StaticMeshes/XEffects/GibBotUpperArm"), 34.f },
		{ TEXT("StaticMeshes/XEffects/GibOrganicUpperarm"), TEXT("StaticMeshes/XEffects/GibBotUpperArm"), 34.f },
		{ TEXT("StaticMeshes/XEffects/GibOrganicForearm"),  TEXT("StaticMeshes/XEffects/GibBotForearm"),  30.f },
		{ TEXT("StaticMeshes/XEffects/GibOrganicForearm"),  TEXT("StaticMeshes/XEffects/GibBotForearm"),  30.f },
	};
	/** xPawnGibGroup.GibSounds */
	const TCHAR* const GibSounds[] = {
		TEXT("Sounds/PlayerSounds/Final_Giblets1"), TEXT("Sounds/PlayerSounds/Final_Giblets2"), TEXT("Sounds/PlayerSounds/Final_Giblets3") };
}

UPhysicsAsset* ArenaRagdoll::GetPhysicsAsset(USkeletalMesh* Mesh)
{
	using namespace ArenaRagdollPrivate;
	if (!Mesh)
	{
		return nullptr;
	}
	static TMap<TWeakObjectPtr<USkeletalMesh>, TStrongObjectPtr<UPhysicsAsset>> Cache;
	if (const TStrongObjectPtr<UPhysicsAsset>* Found = Cache.Find(Mesh))
	{
		return Found->Get();
	}
	UPhysicsAsset* PA = BuildPhysicsAsset(Mesh);
	if (!PA)
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("Ragdoll: %s has no biped skeleton, no ragdoll"), *Mesh->GetName());
	}
	Cache.Add(Mesh, TStrongObjectPtr<UPhysicsAsset>(PA));
	return PA;
}

bool ArenaRagdoll::SpawnGibs(UWorld* World, const FVector& Location, const FVector& Velocity, EGibKind Kind, float SizeScale)
{
	using namespace ArenaRagdollPrivate;
	if (!World || !ArenaArt::IsEnabled())
	{
		return false;
	}
	const bool bRobot = Kind == EGibKind::Robot;
	// Green blood: the organic gib skin swapped for its green variant (xAlienGibGroup uses GibOrganicGreen*).
	UTexture2D* GreenSkin = Kind == EGibKind::OrganicAlt ? ArenaArt::Load<UTexture2D>(TEXT("Textures/XEffects/GibOrganicGreen")) : nullptr;
	int32 Spawned = 0;
	for (const FGibPart& G : GibParts)
	{
		UStaticMesh* Mesh = ArenaArt::Load<UStaticMesh>(bRobot ? G.Robot : G.Organic);
		if (!Mesh)
		{
			continue;
		}
		const FVector Start = Location + FMath::VRand() * 18.f * SizeScale;
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AArenaGib* Gib = World->SpawnActor<AArenaGib>(AArenaGib::StaticClass(), FTransform(FMath::VRand().Rotation(), Start), Params);
		if (!Gib)
		{
			continue;
		}
		const FBoxSphereBounds B = Mesh->GetBounds();
		const float Largest = FMath::Max(1.f, float(B.BoxExtent.GetMax()) * 2.f);
		const float Scale = G.Size * SizeScale / Largest;
		Gib->Mesh->SetStaticMesh(Mesh);
		Gib->Mesh->SetRelativeScale3D(FVector(Scale));
		Gib->Mesh->SetRelativeLocation(-B.Origin * Scale);
		Gib->Body->SetSphereRadius(FMath::Max(3.f, G.Size * SizeScale * 0.3f));
		if (GreenSkin)
		{
			if (UMaterialInstanceDynamic* MID = ArenaRuntimeContent::CreateMaterialInstance(Gib->Mesh->GetMaterial(0), Gib->Mesh))
			{
				MID->SetTextureParameterValue(TEXT("Base"), GreenSkin);
				Gib->Mesh->SetMaterial(0, MID);
			}
		}
		Gib->Body->SetSimulatePhysics(true);
		// Gib.SpawnGiblet: the body's velocity plus a random push (GibPerterbation), tumbling.
		Gib->Body->SetPhysicsLinearVelocity(Velocity * 0.7f + FMath::VRand() * FMath::FRandRange(220.f, 520.f) + FVector(0.f, 0.f, FMath::FRandRange(150.f, 380.f)));
		Gib->Body->SetPhysicsAngularVelocityInDegrees(FMath::VRand() * FMath::FRandRange(240.f, 720.f));
		Gib->SetLifeSpan(FMath::FRandRange(6.f, 9.f));
		++Spawned;
	}
	if (Spawned == 0)
	{
		return false;
	}
	// BloodExplosion / BotSparks burst and the gib sound.
	const FLinearColor Tint = Kind == EGibKind::OrganicAlt ? FLinearColor(0.35f, 1.f, 0.25f) : FLinearColor::White;
	ArenaArtFX::SpawnEffect(World, bRobot ? FName(TEXT("Gib.Sparks")) : FName(TEXT("Gib.Blood")), Location, FRotator::ZeroRotator, SizeScale, Tint);
	if (USoundBase* Sound = ArenaArt::LoadOneShotSound(GibSounds[FMath::RandRange(0, UE_ARRAY_COUNT(GibSounds) - 1)]))
	{
		UGameplayStatics::PlaySoundAtLocation(World, Sound, Location, 1.f);
	}
	return true;
}

AArenaGib::AArenaGib()
{
	PrimaryActorTick.bCanEverTick = false;
	Body = CreateDefaultSubobject<USphereComponent>(TEXT("Body"));
	RootComponent = Body;
	Body->SetMobility(EComponentMobility::Movable);
	Body->InitSphereRadius(8.f);
	Body->SetCollisionProfileName(TEXT("PhysicsActor"));
	Body->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	Body->SetCollisionResponseToChannel(ECC_Weapon, ECR_Ignore);
	Body->SetCollisionResponseToChannel(ECC_Projectile, ECR_Ignore);
	Body->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	Body->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Ignore);
	Body->SetCollisionEnabled(ECollisionEnabled::PhysicsOnly);
	Body->SetCanEverAffectNavigation(false);
	Body->SetGenerateOverlapEvents(false);
	Body->SetHiddenInGame(true);
	Body->BodyInstance.bUseCCD = true;
	Body->BodyInstance.LinearDamping = 0.2f;
	Body->BodyInstance.AngularDamping = 0.6f;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Body);
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCanEverAffectNavigation(false);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCastShadow(true);
}

void ArenaRagdoll::DebugNotifyDeath(AActor* Body, const FString& Label)
{
#if !UE_BUILD_SHIPPING
	static int32 Remaining = -1;
	static bool bBusy = false;
	static int32 Serial = 0;
	if (Remaining < 0)
	{
		Remaining = 0;
		if (!FParse::Value(FCommandLine::Get(), TEXT("AutoDeathCam="), Remaining) && FParse::Param(FCommandLine::Get(), TEXT("AutoDeathCam")))
		{
			Remaining = 6;
		}
	}
	UWorld* World = Body ? Body->GetWorld() : nullptr;
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (Remaining <= 0 || bBusy || !PC || PC->GetPawn() == Body)
	{
		return;
	}
	--Remaining;
	bBusy = true;
	const int32 Index = ++Serial;
	UE_LOG(LogOpenUT2k4, Log, TEXT("AutoDeathCam %d: %s (%s)"), Index, *Body->GetName(), *Label);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const FVector Focus0 = Body->GetActorLocation();
	const FVector Dir = FRotator(-25.f, Body->GetActorRotation().Yaw + 70.f, 0.f).Vector();
	ACameraActor* Cam = World->SpawnActor<ACameraActor>(Focus0 - Dir * 420.f, Dir.Rotation(), Params);
	if (!Cam)
	{
		bBusy = false;
		return;
	}
	Cam->GetCameraComponent()->SetFieldOfView(75.f);
	Cam->GetCameraComponent()->bConstrainAspectRatio = false;
	if (APawn* Own = PC->GetPawn())
	{
		Own->SetActorHiddenInGame(true);
	}
	PC->SetViewTarget(Cam);

	TWeakObjectPtr<AActor> WeakBody(Body);
	TWeakObjectPtr<ACameraActor> WeakCam(Cam);
	TSharedRef<float> Elapsed = MakeShared<float>(0.f);
	TSharedRef<int32> Step = MakeShared<int32>(0);
	TSharedRef<FVector> LastFocus = MakeShared<FVector>(Focus0);
	TSharedRef<FTimerHandle> Handle = MakeShared<FTimerHandle>();
	const float Interval = 0.05f;
	World->GetTimerManager().SetTimer(*Handle, FTimerDelegate::CreateLambda([WeakBody, WeakCam, Elapsed, Step, LastFocus, Handle, Dir, Index, World, Interval]()
	{
		*Elapsed += Interval;
		// follow the body: the ragdoll's bounds, else the actor
		if (AActor* B = WeakBody.Get())
		{
			FVector Focus = B->GetActorLocation();
			if (const USkeletalMeshComponent* Skel = B->FindComponentByClass<USkeletalMeshComponent>())
			{
				if (Skel->IsSimulatingPhysics() || Skel->IsVisible())
				{
					Focus = Skel->Bounds.Origin;
				}
			}
			*LastFocus = Focus;
		}
		if (ACameraActor* C = WeakCam.Get())
		{
			C->SetActorLocationAndRotation(*LastFocus - Dir * 420.f, Dir.Rotation());
			// keep the view when the local player dies / respawns meanwhile
			APlayerController* P = World->GetFirstPlayerController();
			if (P && P->GetViewTarget() != C)
			{
				P->SetViewTarget(C);
			}
		}
		const float Shots[] = { 0.05f, 0.5f, 1.5f, 4.f };
		if (*Step < int32(UE_ARRAY_COUNT(Shots)) && *Elapsed >= Shots[*Step])
		{
			const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Screenshots") / FString::Printf(TEXT("death_%02d_%d.png"), Index, *Step));
			FScreenshotRequest::RequestScreenshot(File, false, false);
			UE_LOG(LogOpenUT2k4, Log, TEXT("AutoDeathCam %d: shot %s"), Index, *File);
			++*Step;
		}
		if (*Step >= int32(UE_ARRAY_COUNT(Shots)) && *Elapsed > Shots[UE_ARRAY_COUNT(Shots) - 1] + 0.3f)
		{
			World->GetTimerManager().ClearTimer(*Handle);
			if (APlayerController* P = World->GetFirstPlayerController())
			{
				if (APawn* Own = P->GetPawn())
				{
					Own->SetActorHiddenInGame(false);
					P->SetViewTarget(Own);
				}
			}
			if (ACameraActor* C = WeakCam.Get())
			{
				C->Destroy();
			}
			bBusy = false;
		}
	}), Interval, true);
#endif
}
