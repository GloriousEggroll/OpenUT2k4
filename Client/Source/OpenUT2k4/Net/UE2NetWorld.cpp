#include "UE2NetWorld.h"
#include "../ArenaHitchLog.h"

#include "../ArenaArt.h"
#include "../ArenaArtFX.h"
#include "../ArenaAS_Info.h"
#include "../ArenaAS_Objective.h"
#include "../ArenaBR_Ball.h"
#include "../ArenaDOM_ControlPoint.h"
#include "../ArenaFlag.h"
#include "../ArenaFX.h"
#include "../ArenaGameState.h"
#include "../ArenaLocalMessages.h"
#include "../ArenaMover.h"
#include "../ArenaONS_PowerNode.h"
#include "../ArenaPickup.h"
#include "../ArenaPlayerController.h"
#include "../ArenaPlayerState.h"
#include "../ArenaRuntimeContent.h"
#include "../ArenaTranslocator.h"
#include "../ArenaTypes.h"
#include "../ArenaUE2World.h"
#include "../ArenaWeapon.h"
#include "../ArenaWeaponActors.h"
#include "../ArenaWeaponsExtra.h"
#include "../UScript/UScriptCore.h"
#include "../UScript/UScriptRuntime.h"
#include "../UScript/UScriptWorld.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/SpectatorPawn.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UE2NetActors.h"
#include "UE2NetModWeapons.h"
#include "UE2NetConnection.h"
#include "UE2NetLog.h"
#include "UE2NetModContent.h"
#include "UE2NetPlay.h"
#include "UE2NetScriptHUD.h"

namespace UE2NetWorldLocal
{
	FUE2NetWorld* GInstance = nullptr;
	constexpr float S = ArenaUE2World::Scale;

	const TCHAR* KindName(FUE2NetWorld::EKind K)
	{
		static const TCHAR* Names[] = { TEXT("unknown"), TEXT("ignored"), TEXT("controller"), TEXT("pri"), TEXT("gri"), TEXT("team"), TEXT("pawn"),
			TEXT("vehicle"), TEXT("attachment"), TEXT("projectile"), TEXT("pickup"), TEXT("mover"), TEXT("flag"), TEXT("dompoint"), TEXT("objective"),
			TEXT("beam"), TEXT("gameobject"), TEXT("weaponpawn"), TEXT("vehicleweapon"), TEXT("heldbeam") };
		static_assert(UE_ARRAY_COUNT(Names) == int32(FUE2NetWorld::EKind::Num), "kind names");
		return Names[int32(K)];
	}

	FVector ToVec(const FVector3f& V) { return FVector(V.X, V.Y, V.Z); }

	/** "Package.Group.Name" -> the content path of a sound ("Sounds/Package/Group_Name"). */
	FString SoundPath(const FString& ObjectPath)
	{
		FString Pkg, Rest;
		if (!ObjectPath.Split(TEXT("."), &Pkg, &Rest))
		{
			return FString();
		}
		return FString::Printf(TEXT("Sounds/%s/%s"), *Pkg, *Rest.Replace(TEXT("."), TEXT("_")));
	}

	USoundBase* LoadSound(const FString& ObjectPath)
	{
		static TMap<FString, TWeakObjectPtr<USoundBase>> Cache;
		static TSet<FString> Missing;
		if (ObjectPath.IsEmpty() || Missing.Contains(ObjectPath))
		{
			return nullptr;
		}
		if (const TWeakObjectPtr<USoundBase>* Found = Cache.Find(ObjectPath))
		{
			if (Found->IsValid())
			{
				return Found->Get();
			}
		}
		USoundBase* Sound = ArenaArt::LoadOneShotSound(SoundPath(ObjectPath));
		if (!Sound)
		{
			// a mod package's sound: converted on demand
			Sound = UE2NetModContent::LoadSound(ObjectPath);
		}
		if (!Sound)
		{
			Missing.Add(ObjectPath);
			return nullptr;
		}
		Cache.Add(ObjectPath, Sound);
		return Sound;
	}

	/**
	 * Distance falloff for a sound the server tells us about, by its UE2 radius (UU; TransientSoundRadius, 300 by
	 * default): full volume inside half the radius, then a logarithmic falloff over 8 radii (UE2 audio fades sounds
	 * well beyond their nominal radius; the server only sends the sounds we can hear).
	 */
	USoundAttenuation* SoundAttenuation(float RadiusUU)
	{
		static TMap<int32, TStrongObjectPtr<USoundAttenuation>> Cache;
		const int32 Key = FMath::Clamp(FMath::RoundToInt(RadiusUU / 50.f), 1, 100) * 50;
		if (TStrongObjectPtr<USoundAttenuation>* Found = Cache.Find(Key))
		{
			return Found->Get();
		}
		USoundAttenuation* A = NewObject<USoundAttenuation>(GetTransientPackage());
		A->Attenuation.bAttenuate = true;
		A->Attenuation.bSpatialize = true;
		A->Attenuation.AttenuationShape = EAttenuationShape::Sphere;
		A->Attenuation.AttenuationShapeExtents = FVector(Key * S * 0.5f, 0.f, 0.f);
		A->Attenuation.FalloffDistance = Key * S * 8.f;
		A->Attenuation.DistanceAlgorithm = EAttenuationDistanceModel::Logarithmic;
		// a source within about a pawn's height of the listener has no meaningful direction (the listener is the eye,
		// sounds come from actor centres): blend it to centred instead of a hard pan picked by a few units of offset
		A->Attenuation.NonSpatializedRadiusStart = 150.f * S;
		A->Attenuation.NonSpatializedRadiusEnd = 60.f * S;
		A->Attenuation.NonSpatializedRadiusMode = ENonSpatializedRadiusSpeakerMapMode::Direct2D;
		Cache.Add(Key, TStrongObjectPtr<USoundAttenuation>(A));
		return A;
	}

	/** Projectile art style for a UE2 projectile class (short name, lower case). */
	bool ProjectileStyle(const TArray<FString>& Chain, FName& OutStyle, float& OutScale, FLinearColor& OutGlow)
	{
		struct FMap { const TCHAR* Class; const TCHAR* Style; float Scale; FLinearColor Glow; };
		static const FMap Table[] = {
			{ TEXT("redeemerprojectile"), TEXT("Rocket"), 2.5f, FLinearColor(1.f, 0.6f, 0.3f) },
			{ TEXT("redeemerwarhead"), TEXT("Rocket"), 2.5f, FLinearColor(1.f, 0.6f, 0.3f) },
			{ TEXT("seekingrocketproj"), TEXT("Rocket"), 1.f, FLinearColor(1.f, 0.6f, 0.3f) },
			{ TEXT("rocketproj"), TEXT("Rocket"), 1.f, FLinearColor(1.f, 0.6f, 0.3f) },
			{ TEXT("onsavrilrocket"), TEXT("Rocket"), 1.2f, FLinearColor(1.f, 0.6f, 0.3f) },
			{ TEXT("flakchunk"), TEXT("FlakChunk"), 1.f, FLinearColor(1.f, 0.7f, 0.2f) },
			{ TEXT("flakshell"), TEXT("FlakShell"), 1.f, FLinearColor(1.f, 0.6f, 0.2f) },
			{ TEXT("shockprojectile"), TEXT("PulseCore"), 1.f, FLinearColor(0.6f, 0.3f, 1.f) },
			{ TEXT("linkprojectile"), TEXT("LinkBolt"), 1.f, FLinearColor(0.3f, 1.f, 0.3f) },
			{ TEXT("bioglob"), TEXT("Goo"), 1.f, FLinearColor(0.4f, 1.f, 0.2f) },
			{ TEXT("onsgrenadeprojectile"), TEXT("Grenade"), 1.f, FLinearColor(1.f, 0.5f, 0.2f) },
			{ TEXT("onsmineprojectile"), TEXT("Grenade"), 1.2f, FLinearColor(1.f, 0.3f, 0.2f) },
			{ TEXT("grenade"), TEXT("Grenade"), 1.f, FLinearColor(1.f, 0.5f, 0.2f) },
			{ TEXT("transbeacon"), TEXT(""), 1.f, FLinearColor(0.3f, 0.6f, 1.f) },
		};
		for (const FMap& M : Table)
		{
			if (Chain.Contains(FString(M.Class)))
			{
				OutStyle = *M.Style ? FName(M.Style) : NAME_None;
				OutScale = M.Scale;
				OutGlow = M.Glow;
				return true;
			}
		}
		// vehicle / mod projectiles: a glow by the name
		OutStyle = NAME_None;
		OutScale = 1.f;
		const FString& N = Chain.Num() ? Chain[0] : FString();
		OutGlow = N.Contains(TEXT("red")) || N.Contains(TEXT("plasma")) ? FLinearColor(1.f, 0.3f, 0.2f)
			: (N.Contains(TEXT("blue")) || N.Contains(TEXT("shock")) ? FLinearColor(0.3f, 0.5f, 1.f) : FLinearColor(1.f, 0.85f, 0.4f));
		return false;
	}

	/** UE2 team name for announcer / message keys. */
	const TCHAR* TeamWord(int32 Team) { return Team == 1 ? TEXT("Blue") : TEXT("Red"); }
}

FUE2NetWorld* FUE2NetWorld::Get()
{
	return UE2NetWorldLocal::GInstance;
}

FUE2NetWorld::FUE2NetWorld(FUE2NetConnection& InConnection, UWorld* InWorld)
	: Connection(&InConnection), World(InWorld)
{
	UE2NetWorldLocal::GInstance = this;
	StartTime = FPlatformTime::Seconds();
	NextSummary = StartTime + 30.0;
	// test hook: -UE2NetSpectateScript="10:next;30:self;40:self" (seconds after the world layer started)
	FString ScriptText;
	if (FParse::Value(FCommandLine::Get(), TEXT("UE2NetSpectateScript="), ScriptText))
	{
		TArray<FString> Steps;
		ScriptText.ParseIntoArray(Steps, TEXT(";"));
		for (const FString& Step : Steps)
		{
			FString T, Cmd;
			if (Step.Split(TEXT(":"), &T, &Cmd))
			{
				Script.Emplace(StartTime + FCString::Atod(*T), Cmd.TrimStartAndEnd().ToLower());
			}
		}
	}
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: showing the server's game state in %s (%d spectate script steps)"), InWorld ? *InWorld->GetMapName() : TEXT("(none)"), Script.Num());
}

FUE2NetWorld::~FUE2NetWorld()
{
	LogSummary();
	for (TPair<int32, FEntity>& P : Entities)
	{
		if (P.Value.bOwned && P.Value.Native.IsValid())
		{
			P.Value.Native->Destroy();
		}
		if (P.Value.PS.IsValid() && P.Value.Channel != OwnPRIChannel)
		{
			P.Value.PS->Destroy();
		}
	}
	for (const TPair<TWeakObjectPtr<AActor>, double>& C : Corpses)
	{
		if (C.Key.IsValid())
		{
			C.Key->Destroy();
		}
	}
	if (UE2NetWorldLocal::GInstance == this)
	{
		UE2NetWorldLocal::GInstance = nullptr;
	}
}

// =================================================================================================
// Values
// =================================================================================================

const FUE2NetValue* FUE2NetWorld::Find(const FUE2NetActor& A, const TCHAR* Key) const
{
	return A.Properties.Find(Key);
}

FString FUE2NetWorld::DefaultText(FUSClass* Class, const TCHAR* Key) const
{
	if (!Class)
	{
		return FString();
	}
	FString Name = Key;
	int32 Index = 0;
	int32 Bracket = INDEX_NONE;
	if (Name.FindChar(TEXT('['), Bracket))
	{
		Index = FCString::Atoi(*Name.Mid(Bracket + 1));
		Name.LeftInline(Bracket);
	}
	FUSObject* Def = Class->GetDefaultObject();
	FUSProperty* Prop = Def ? Def->FindProperty(FName(*Name)) : nullptr;
	if (!Prop || !Def->Data || Index >= Prop->ArrayDim)
	{
		return FString();
	}
	const uint8* Value = Def->Data + Prop->Offset + Index * Prop->ElementSize;
	if (Prop->Type == EUSType::Byte)
	{
		return FString::FromInt(*Value); // the number, not the enum name
	}
	FString Out;
	Prop->ExportText(Out, Value, true);
	// objects export as Class'Package.Name': the path only
	int32 Quote = INDEX_NONE;
	if ((Prop->Type == EUSType::Object || Prop->Type == EUSType::Class) && Out.FindChar(TEXT('\''), Quote) && Out.EndsWith(TEXT("'")))
	{
		Out = Out.Mid(Quote + 1, Out.Len() - Quote - 2);
	}
	return Out;
}

int32 FUE2NetWorld::GetInt(const FUE2NetActor& A, const TCHAR* Key, int32 Default) const
{
	if (const FUE2NetValue* V = Find(A, Key))
	{
		return V->Kind == FUE2NetValue::EKind::Float ? int32(V->F) : V->I;
	}
	const FString D = DefaultText(A.Class, Key);
	return D.IsEmpty() ? Default : FCString::Atoi(*D);
}

float FUE2NetWorld::GetFloat(const FUE2NetActor& A, const TCHAR* Key, float Default) const
{
	if (const FUE2NetValue* V = Find(A, Key))
	{
		return V->Kind == FUE2NetValue::EKind::Float ? V->F : float(V->I);
	}
	const FString D = DefaultText(A.Class, Key);
	return D.IsEmpty() ? Default : FCString::Atof(*D);
}

bool FUE2NetWorld::GetBool(const FUE2NetActor& A, const TCHAR* Key, bool bDefault) const
{
	if (const FUE2NetValue* V = Find(A, Key))
	{
		return V->I != 0;
	}
	const FString D = DefaultText(A.Class, Key);
	return D.IsEmpty() ? bDefault : D.Equals(TEXT("True"), ESearchCase::IgnoreCase);
}

FString FUE2NetWorld::GetStr(const FUE2NetActor& A, const TCHAR* Key) const
{
	if (const FUE2NetValue* V = Find(A, Key))
	{
		return V->Kind == FUE2NetValue::EKind::Name ? V->N.ToString() : V->S;
	}
	return DefaultText(A.Class, Key);
}

FVector FUE2NetWorld::GetVec(const FUE2NetActor& A, const TCHAR* Key, const FVector& Default) const
{
	if (const FUE2NetValue* V = Find(A, Key))
	{
		return UE2NetWorldLocal::ToVec(V->V);
	}
	return Default;
}

int32 FUE2NetWorld::GetRef(const FUE2NetActor& A, const TCHAR* Key) const
{
	const FUE2NetValue* V = Find(A, Key);
	return V && V->Obj.IsDynamic() ? V->Obj.Channel : 0;
}

FString FUE2NetWorld::ObjPath(const FUE2NetValue& V) const
{
	return V.Obj.IsStatic() ? Connection->GetPackageMap().GetObjectPath(V.Obj) : FString();
}

FString FUE2NetWorld::GetObjPath(const FUE2NetActor& A, const TCHAR* Key) const
{
	const FUE2NetValue* V = Find(A, Key);
	return V ? ObjPath(*V) : FString();
}

FString FUE2NetWorld::ClassDefault(const FUE2NetValue& ClassRef, const TCHAR* Key) const
{
	if (!ClassRef.Obj.IsStatic())
	{
		return FString();
	}
	FUSClass* Class = Connection->GetPackageMap().GetClass(ClassRef.Obj);
	return DefaultText(Class, Key);
}

// =================================================================================================
// Lookups
// =================================================================================================

AArenaPlayerController* FUE2NetWorld::GetPC() const
{
	UWorld* W = World.Get();
	return W ? Cast<AArenaPlayerController>(W->GetFirstPlayerController()) : nullptr;
}

AArenaPlayerState* FUE2NetWorld::PSForChannel(int32 PRIChannel)
{
	if (!PRIChannel)
	{
		return nullptr;
	}
	if (PRIChannel == OwnPRIChannel)
	{
		AArenaPlayerController* PC = GetPC();
		return PC ? PC->GetPlayerState<AArenaPlayerState>() : nullptr;
	}
	FEntity* E = FindEntity(PRIChannel);
	return E ? E->PS.Get() : nullptr;
}

int32 FUE2NetWorld::FindChannelOf(const AActor* Actor) const
{
	// a hit on an attached part (a seated driver, a vehicle's turret) counts for the actor it is attached to
	for (int32 Guard = 0; Actor && Guard < 8; ++Guard, Actor = Actor->GetAttachParentActor())
	{
		for (const TPair<int32, FEntity>& P : Entities)
		{
			if (P.Value.Native.Get() == Actor)
			{
				return P.Key;
			}
		}
	}
	return 0;
}

AUE2NetPawn* FUE2NetWorld::PawnForChannel(int32 Channel)
{
	FEntity* E = FindEntity(Channel);
	return E && E->Kind == EKind::Pawn ? Cast<AUE2NetPawn>(E->Native.Get()) : nullptr;
}

AUE2NetPawn* FUE2NetWorld::PawnOfPRI(int32 PRIChannel, bool bAliveOnly)
{
	if (!PRIChannel)
	{
		return nullptr;
	}
	const TMap<int32, FUE2NetActor>& Actors = Connection->GetActors();
	for (TPair<int32, FEntity>& P : Entities)
	{
		if (P.Value.Kind != EKind::Pawn)
		{
			continue;
		}
		const FUE2NetActor* A = Actors.Find(P.Key);
		AUE2NetPawn* Pawn = Cast<AUE2NetPawn>(P.Value.Native.Get());
		if (A && Pawn && GetRef(*A, TEXT("PlayerReplicationInfo")) == PRIChannel && (!bAliveOnly || !Pawn->IsNetDead()))
		{
			return Pawn;
		}
	}
	return nullptr;
}

AArenaCharacter* FUE2NetWorld::CharacterOfPRI(int32 PRIChannel)
{
	// our own PRI while playing: the local pawn (Net/UE2NetPlay.h), there is no AUE2NetPawn for it
	if (PRIChannel && PRIChannel == OwnPRIChannel)
	{
		if (const FUE2NetPlay* Play = FUE2NetPlay::Get())
		{
			if (AArenaCharacter* Local = Play->GetLocalPawn())
			{
				return Local;
			}
		}
	}
	return PawnOfPRI(PRIChannel, true);
}

int32 FUE2NetWorld::TeamIndexOf(int32 TeamInfoChannel) const
{
	if (!TeamInfoChannel)
	{
		return ArenaTeam::None;
	}
	const FUE2NetActor* A = Connection->GetActors().Find(TeamInfoChannel);
	return A ? GetInt(*A, TEXT("TeamIndex"), ArenaTeam::None) : ArenaTeam::None;
}

// =================================================================================================
// Classification
// =================================================================================================

FUE2NetWorld::EKind FUE2NetWorld::Classify(const FEntity& E) const
{
	if (Has(E, TEXT("playercontroller")))
	{
		return EKind::Controller;
	}
	if (Has(E, TEXT("playerreplicationinfo")))
	{
		return EKind::PRI;
	}
	if (Has(E, TEXT("gamereplicationinfo")))
	{
		return EKind::GRI;
	}
	if (Has(E, TEXT("teaminfo")))
	{
		return EKind::Team;
	}
	if (Has(E, TEXT("onsweaponpawn")))
	{
		return EKind::WeaponPawn; // a gunner seat of a vehicle, or a stationary turret
	}
	if (Has(E, TEXT("onsweapon")))
	{
		return EKind::VehicleWeapon; // a vehicle's or turret's gun (its own mesh on a bone of the vehicle)
	}
	if (Has(E, TEXT("vehicle")))
	{
		return EKind::Vehicle;
	}
	if (Has(E, TEXT("redeemerwarhead")))
	{
		return EKind::Projectile; // a guided Redeemer: a pawn its player flies, shown as the missile (ours is flown by Net/UE2NetPlay)
	}
	if (Has(E, TEXT("timestamp_pawn")))
	{
		return EKind::Ignored; // UTComp's NewNet clock (Net/UE2NetNewNet.h): no body, never shown
	}
	if (Has(E, TEXT("pawn")))
	{
		return EKind::Pawn;
	}
	if (Has(E, TEXT("weaponattachment")) || Has(E, TEXT("inventoryattachment")))
	{
		return EKind::Attachment;
	}
	if (Has(E, TEXT("projectile")))
	{
		return EKind::Projectile;
	}
	if (Has(E, TEXT("pickup")))
	{
		return EKind::Pickup;
	}
	if (Has(E, TEXT("mover")))
	{
		return EKind::Mover;
	}
	if (Has(E, TEXT("ctfflag")))
	{
		return EKind::Flag;
	}
	if (Has(E, TEXT("gameobject")))
	{
		return EKind::GameObject; // the bombing run ball
	}
	if (Has(E, TEXT("xdompoint")) || Has(E, TEXT("dominationpoint")))
	{
		return EKind::DomPoint;
	}
	if (Has(E, TEXT("gameobjective")))
	{
		return EKind::Objective;
	}
	if (Has(E, TEXT("shockbeameffect")) || Has(E, TEXT("newlightningbolt")) || Has(E, TEXT("shockcombo")) || HasLike(E, TEXT("wallhiteffect"))
		|| Has(E, TEXT("redeemerexplosion")) || Has(E, TEXT("smallredeemerexplosion")))
	{
		return EKind::Beam; // shot effects (beams, hit effects, blasts) spawned once by the server
	}
	if (E.ClassName.StartsWith(TEXT("NewTransEffect")) || E.ClassName.StartsWith(TEXT("NewTransDeres")))
	{
		return EKind::Beam; // xPawn.PlayTeleportEffect / DoTranslocateOut: the translocation flashes
	}
	if (Has(E, TEXT("linkbeameffect")) || Has(E, TEXT("painterbeameffect")) || Has(E, TEXT("onsminelayertargetbeameffect")))
	{
		return EKind::HeldBeam;
	}
	// known engine / game infrastructure without anything to show (inventory is replicated to its owner only)
	static const TCHAR* Known[] = { TEXT("levelinfo"), TEXT("inventory"), TEXT("voicechatreplicationinfo"), TEXT("playerchatmanager"),
		TEXT("squadai"), TEXT("zoneinfo"), TEXT("xemitter"), TEXT("emitter"), TEXT("xpickupbase"), TEXT("navigationpoint"), TEXT("hud"),
		TEXT("scoreboard"), TEXT("replicationinfo"), TEXT("projector"), TEXT("decoration") };
	for (const TCHAR* K : Known)
	{
		if (Has(E, K))
		{
			return EKind::Ignored;
		}
	}
	return EKind::Unknown;
}

// =================================================================================================
// Connection events
// =================================================================================================

void FUE2NetWorld::ActorOpened(const FUE2NetActor& A)
{
	if (A.bBroken || !A.Class || !World.IsValid())
	{
		return;
	}
	FEntity& E = Entities.FindOrAdd(A.Channel);
	E = FEntity();
	E.Channel = A.Channel;
	E.Class = A.Class;
	E.ClassPath = A.ClassPath;
	E.ClassName = A.Class->GetName();
	for (FUSClass* C = A.Class; C; C = C->GetSuperClass())
	{
		E.Chain.Add(C->GetName().ToLower());
	}
	E.Kind = Classify(E);
	++KindOpened[int32(E.Kind)];
	if (E.Kind == EKind::Unknown)
	{
		++UnknownClasses.FindOrAdd(E.ClassPath);
	}
	TArray<FString> All;
	A.Properties.GetKeys(All);
	Apply(E, A, All, true);
}

void FUE2NetWorld::ActorUpdated(const FUE2NetActor& A, const TArray<FString>& Changed)
{
	if (FEntity* E = Entities.Find(A.Channel))
	{
		Apply(*E, A, Changed, false);
	}
}

void FUE2NetWorld::DropNativeActor(int32 Channel)
{
	FEntity* E = Entities.Find(Channel);
	if (E && E->bOwned && E->Native.IsValid())
	{
		E->Native->Destroy();
	}
	if (E)
	{
		E->Native.Reset();
		E->bOwned = false;
	}
}

void FUE2NetWorld::ActorClosed(const FUE2NetActor& A)
{
	FEntity E;
	if (!Entities.RemoveAndCopyValue(A.Channel, E))
	{
		return;
	}
	NetObjectives.Remove(A.Channel);
	AActor* Native = E.Native.Get();
	switch (E.Kind)
	{
	case EKind::Pawn:
		if (AUE2NetPawn* Pawn = Cast<AUE2NetPawn>(Native))
		{
			if (Pawn->IsNetDead())
			{
				// torn off: the body stays a few seconds (UE2 keeps it until its LifeSpan)
				Corpses.Emplace(Pawn, FPlatformTime::Seconds() + 8.0);
				Native = nullptr;
			}
		}
		break;
	case EKind::Projectile:
		if (AUE2NetProjectile* Proj = Cast<AUE2NetProjectile>(Native))
		{
			// bNetTemporary projectiles: the channel closes right after the initial bunch, the client flies them on
			// until they hit something; the others are closed when they explode
			if (Has(E, TEXT("shockprojectile")))
			{
				// a shock combo may come with its close: the core's explosion waits for it (SpawnBeam)
				bool bCombo = false;
				for (const TPair<FVector, double>& C : RecentCombos)
				{
					bCombo |= FPlatformTime::Seconds() - C.Value < 0.5 && FVector::Dist(C.Key, Proj->GetActorLocation()) < 250.f * UE2NetWorldLocal::S;
				}
				if (bCombo)
				{
					Proj->Destroy();
				}
				else
				{
					PendingShockCores.Emplace(Proj, FPlatformTime::Seconds() + 0.1);
				}
			}
			else if (!Proj->IsTemporary())
			{
				Proj->NetExplode();
			}
			Native = nullptr;
		}
		break;
	case EKind::Attachment:
		if (AUE2NetPawn* Pawn = PawnForChannel(E.PawnChannel))
		{
			Pawn->SetNetWeapon(FString());
		}
		break;
	case EKind::Vehicle:
	case EKind::WeaponPawn:
		if (AUE2NetPawn* Pawn = PawnForChannel(E.PawnChannel))
		{
			Pawn->SetNetVehicle(nullptr);
		}
		if (E.Kind == EKind::WeaponPawn && !E.bOwned)
		{
			if (AUE2NetVehicle* V = VehicleForChannel(E.BaseChannel))
			{
				V->SetSeat(E.Channel, nullptr, false, FVector::ZeroVector, FRotator::ZeroRotator, 0);
			}
		}
		if (Native && E.bOwned)
		{
			// gunners of this vehicle leave it with it
			for (TPair<int32, FEntity>& P : Entities)
			{
				AUE2NetPawn* Pawn = P.Value.Kind == EKind::Pawn ? Cast<AUE2NetPawn>(P.Value.Native.Get()) : nullptr;
				if (Pawn && Pawn->GetNetVehicle() == Native)
				{
					Pawn->SetNetVehicle(nullptr);
				}
			}
		}
		break;
	case EKind::VehicleWeapon:
		if (AUE2NetVehicle* V = Cast<AUE2NetVehicle>(Native))
		{
			V->RemoveWeapon(E.Channel);
		}
		break;
	case EKind::HeldBeam:
		if (const FUE2NetPlay* Play = FUE2NetPlay::Get(); Play && Has(E, TEXT("painterbeameffect")) && Play->IsOwnPawnChannel(GetRef(A, TEXT("Instigator"))))
		{
			// our PainterBeamEffect is gone (Cancelled's 0.4 s timer): our beam goes too
			AArenaCharacter* Own = Play->GetLocalPawn();
			if (AArenaWeapon* Weapon = Own ? Own->FindWeapon(AWeap_OrbitalDesignator::StaticClass()) : nullptr)
			{
				Weapon->NetState.bPainterBeamOpen = false;
				Weapon->NetState.bPainterBeamGone = true;
			}
		}
		break;
	case EKind::PRI:
		if (E.PS.IsValid() && E.Channel != OwnPRIChannel)
		{
			E.PS->Destroy();
		}
		break;
	default:
		break;
	}
	if (E.bOwned && Native)
	{
		Native->Destroy();
	}
}

void FUE2NetWorld::Apply(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	if (bInitial || Changed.Contains(TEXT("Role")) || Changed.Contains(TEXT("RemoteRole")))
	{
		// the client's roles: UE2's server swaps them before replicating (SimulatedProxy, AutonomousProxy for what we own)
		E.Role = uint8(GetInt(A, TEXT("Role"), 2));
		E.RemoteRole = uint8(GetInt(A, TEXT("RemoteRole"), 4));
	}
	switch (E.Kind)
	{
	case EKind::Controller:
		if (Changed.Contains(TEXT("PlayerReplicationInfo")) && A.Channel == Connection->FindPlayerControllerChannel())
		{
			const int32 NewOwn = GetRef(A, TEXT("PlayerReplicationInfo"));
			if (NewOwn && NewOwn != OwnPRIChannel)
			{
				OwnPRIChannel = NewOwn;
				// our own PRI stands for the local player state (a spectator): no second entry on the scoreboard
				if (FEntity* PRI = FindEntity(NewOwn))
				{
					if (PRI->PS.IsValid())
					{
						PRI->PS->Destroy();
						PRI->PS.Reset();
					}
				}
			}
		}
		break;
	case EKind::PRI: ApplyPRI(E, A, Changed, bInitial); break;
	case EKind::GRI: ApplyGRI(E, A, Changed, bInitial); break;
	case EKind::Team: ApplyTeam(E, A, Changed, bInitial); break;
	case EKind::Pawn: ApplyPawn(E, A, Changed, bInitial); break;
	case EKind::Attachment: ApplyAttachment(E, A, Changed, bInitial); break;
	case EKind::Projectile: ApplyProjectile(E, A, Changed, bInitial); break;
	case EKind::Pickup: ApplyPickup(E, A, Changed, bInitial); break;
	case EKind::Mover: ApplyMover(E, A, Changed, bInitial); break;
	case EKind::Flag: ApplyFlag(E, A, Changed, bInitial); break;
	case EKind::DomPoint: ApplyDomPoint(E, A, Changed, bInitial); break;
	case EKind::Vehicle: ApplyVehicle(E, A, Changed, bInitial); break;
	case EKind::WeaponPawn: ApplyWeaponPawn(E, A, Changed, bInitial); break;
	case EKind::VehicleWeapon: ApplyVehicleWeapon(E, A, Changed, bInitial); break;
	case EKind::Objective: ApplyObjective(E, A, Changed, bInitial); break;
	case EKind::GameObject: ApplyGameObject(E, A, Changed, bInitial); break;
	case EKind::Beam:
		if (bInitial)
		{
			SpawnBeam(E, A);
		}
		break;
	case EKind::HeldBeam: ApplyHeldBeam(E, A, Changed, bInitial); break;
	case EKind::Unknown: ApplyDecoration(E, A, Changed, bInitial); break;
	default:
		break;
	}
}

// =================================================================================================
// Replication infos
// =================================================================================================

void FUE2NetWorld::ApplyPRI(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	UWorld* W = World.Get();
	if (!W)
	{
		return;
	}
	AArenaPlayerState* PS = nullptr;
	if (A.Channel == OwnPRIChannel)
	{
		PS = PSForChannel(A.Channel);
	}
	else
	{
		PS = E.PS.Get();
		if (!PS)
		{
			FActorSpawnParameters P;
			P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			P.ObjectFlags |= RF_Transient;
			PS = W->SpawnActor<AArenaPlayerState>(AArenaPlayerState::StaticClass(), FTransform::Identity, P);
			E.PS = PS;
			bInitial = true;
		}
	}
	if (!PS)
	{
		return;
	}
	auto Changes = [&](const TCHAR* Key) { return bInitial || Changed.Contains(Key); };
	if (A.Channel != OwnPRIChannel)
	{
		if (Changes(TEXT("PlayerName")))
		{
			PS->SetPlayerName(GetStr(A, TEXT("PlayerName")));
		}
		if (Changes(TEXT("PlayerID")))
		{
			PS->SetPlayerId(GetInt(A, TEXT("PlayerID")));
		}
		if (Changes(TEXT("bBot")))
		{
			PS->SetIsABot(GetBool(A, TEXT("bBot")));
		}
		if (Changes(TEXT("bIsSpectator")) || Changes(TEXT("bOnlySpectator")))
		{
			PS->SetIsSpectator(GetBool(A, TEXT("bIsSpectator")));
			PS->SetIsOnlyASpectator(GetBool(A, TEXT("bOnlySpectator")));
		}
	}
	if (Changes(TEXT("Score")))
	{
		PS->SetScore(GetFloat(A, TEXT("Score")));
		PS->Kills = FMath::Max(0, FMath::RoundToInt(PS->GetScore()));
	}
	if (Changes(TEXT("Deaths")))
	{
		PS->Deaths = FMath::RoundToInt(GetFloat(A, TEXT("Deaths")));
	}
	if (Changes(TEXT("Team")))
	{
		PS->Team = TeamIndexOf(GetRef(A, TEXT("Team")));
	}
	if (Changes(TEXT("bOutOfLives")))
	{
		PS->bEliminated = GetBool(A, TEXT("bOutOfLives"));
	}
	if (Changes(TEXT("Ping")))
	{
		// UE2 PRI.Ping is the ping / 4 in a byte, like UE5's compressed ping
		PS->SetCompressedPing(uint8(GetInt(A, TEXT("Ping"))));
	}
	if (Changes(TEXT("NumLives")))
	{
		PS->Lives = GetInt(A, TEXT("NumLives"), -1);
	}
	if (Changes(TEXT("CharacterName")) || Changes(TEXT("PlayerID")))
	{
		if (AUE2NetPawn* Pawn = PawnOfPRI(A.Channel, false))
		{
			Pawn->SetCharacter(GetStr(A, TEXT("CharacterName")), GetInt(A, TEXT("PlayerID")));
		}
	}
}

void FUE2NetWorld::ApplyTeam(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	UWorld* W = World.Get();
	AArenaGameState* GS = W ? W->GetGameState<AArenaGameState>() : nullptr;
	const int32 Index = GetInt(A, TEXT("TeamIndex"), -1);
	if (GS && (Index == 0 || Index == 1))
	{
		GS->TeamScores[Index] = FMath::RoundToInt(GetFloat(A, TEXT("Score")));
	}
	if (bInitial || Changed.Contains(TEXT("TeamIndex")))
	{
		// PRIs that arrived before their team
		const TMap<int32, FUE2NetActor>& Actors = Connection->GetActors();
		for (TPair<int32, FEntity>& P : Entities)
		{
			const FUE2NetActor* PRI = P.Value.Kind == EKind::PRI ? Actors.Find(P.Key) : nullptr;
			AArenaPlayerState* PS = PRI ? PSForChannel(P.Key) : nullptr;
			if (PS && GetRef(*PRI, TEXT("Team")) == A.Channel)
			{
				PS->Team = Index;
			}
		}
	}
}

void FUE2NetWorld::ApplyGRI(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	GRIChannel = A.Channel;
	UWorld* W = World.Get();
	AArenaGameState* GS = W ? W->GetGameState<AArenaGameState>() : nullptr;
	if (!GS)
	{
		return;
	}
	auto Changes = [&](const TCHAR* Key) { return bInitial || Changed.Contains(Key); };
	if (Changes(TEXT("ServerName")))
	{
		ServerName = GetStr(A, TEXT("ServerName"));
	}
	if (Changes(TEXT("GoalScore")))
	{
		GS->GoalScore = GetInt(A, TEXT("GoalScore"));
	}
	if (Changes(TEXT("TimeLimit")))
	{
		GS->TimeLimit = GetInt(A, TEXT("TimeLimit")) * 60;
	}
	if (Changes(TEXT("RemainingTime")))
	{
		RemainingTime = GetInt(A, TEXT("RemainingTime"));
	}
	if (Changed.Contains(TEXT("RemainingMinute")) && GetInt(A, TEXT("RemainingMinute")) != 0)
	{
		RemainingTime = GetInt(A, TEXT("RemainingMinute"));
	}
	GS->RemainingTime = RemainingTime;
	bStopCountDown = GetBool(A, TEXT("bStopCountDown"), true);
	bMatchHasBegun = GetBool(A, TEXT("bMatchHasBegun"));
	if (Changes(TEXT("Winner")))
	{
		const int32 Winner = GetRef(A, TEXT("Winner"));
		if (Winner)
		{
			const FEntity* WE = FindEntity(Winner);
			if (WE && WE->Kind == EKind::Team)
			{
				GS->WinningTeam = TeamIndexOf(Winner);
			}
			else
			{
				GS->WinningPlayer = PSForChannel(Winner);
			}
			bGameEnded = true;
		}
	}
	if (Has(E, TEXT("asgamereplicationinfo")))
	{
		bAssault = true;
		if (Changes(TEXT("ElapsedTime")))
		{
			ElapsedTime = GetInt(A, TEXT("ElapsedTime"));
		}
		RoundTimeLimit = GetInt(A, TEXT("RoundTimeLimit"));
		// ObjectiveProgress: objectives completed this round (0 after a reset)
		RoundStartTime = GetInt(A, TEXT("RoundStartTime"));
		GS->Round = FMath::Max(1, GetInt(A, TEXT("CurrentRound"), 1));
		const int32 Attackers = GetBool(A, TEXT("bTeamZeroIsAttacking"), true) ? 0 : 1;
		for (TActorIterator<AArenaAS_Info> It(W); It; ++It)
		{
			It->AttackingTeam = Attackers;
			It->CurrentStage = GetInt(A, TEXT("ObjectiveProgress"));
		}
		if (RoundTimeLimit > 0)
		{
			GS->TimeLimit = RoundTimeLimit;
			GS->RemainingTime = RemainingTime = FMath::Max(0, RoundTimeLimit - (ElapsedTime - RoundStartTime));
		}
	}
	const EArenaMatchPhase Phase = bGameEnded ? EArenaMatchPhase::Ended : (bMatchHasBegun ? EArenaMatchPhase::Playing : EArenaMatchPhase::Countdown);
	if (GS->Phase != Phase)
	{
		GS->Phase = Phase;
		GS->CountdownRemaining = 0;
	}
}

// =================================================================================================
// Pawns and weapons
// =================================================================================================

void FUE2NetWorld::ApplyPawn(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	using namespace UE2NetWorldLocal;
	UWorld* W = World.Get();
	if (!W)
	{
		return;
	}
	if (const FUE2NetPlay* Play = FUE2NetPlay::Get(); Play && Play->IsOwnPawnChannel(A.Channel))
	{
		return; // our own pawn: predicted locally (Net/UE2NetPlay.h)
	}
	AUE2NetPawn* Pawn = Cast<AUE2NetPawn>(E.Native.Get());
	const FUE2NetValue* PawnPos = Find(A, TEXT("PawnPosition"));
	auto MemberVec = [](const FUE2NetValue* V, const TCHAR* Name, FVector& Out, FIntVector* OutRot = nullptr)
	{
		if (!V)
		{
			return false;
		}
		for (const TPair<FName, FUE2NetValue>& M : V->Members)
		{
			if (M.Key == Name)
			{
				Out = ToVec(M.Value.V);
				if (OutRot)
				{
					*OutRot = M.Value.R;
				}
				return true;
			}
		}
		return false;
	};
	if (!Pawn)
	{
		FVector Loc = ToVec(A.SpawnLocation);
		MemberVec(PawnPos, TEXT("Location"), Loc);
		if (const FUE2NetValue* L = Find(A, TEXT("Location")))
		{
			Loc = ToVec(L->V);
		}
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		P.ObjectFlags |= RF_Transient;
		ArenaHitchLog::FScopedNote HitchNote(TEXT("player pawn spawned"));
		Pawn = W->SpawnActor<AUE2NetPawn>(AUE2NetPawn::StaticClass(), Loc * S, FRotator(0.f, A.SpawnRotation.Y * 360.f / 65536.f, 0.f), P);
		if (!Pawn)
		{
			return;
		}
		E.Native = Pawn;
		E.bOwned = true;
		bInitial = true;
	}
	auto Changes = [&](const TCHAR* Key) { return bInitial || Changed.Contains(Key); };
	ON_SCOPE_EXIT
	{
		if (PendingViewChannel == A.Channel)
		{
			FUE2NetValue Target;
			Target.Kind = FUE2NetValue::EKind::Object;
			Target.Obj.Channel = A.Channel;
			SetViewTargetFromServer(Target);
		}
	};
	if (Changes(TEXT("PlayerReplicationInfo")))
	{
		const int32 PRIChannel = GetRef(A, TEXT("PlayerReplicationInfo"));
		AArenaPlayerState* PS = PSForChannel(PRIChannel);
		if (PS && Pawn->GetPlayerState() != PS)
		{
			Pawn->SetPlayerState(PS);
		}
		if (const FUE2NetActor* PRI = PRIChannel ? Connection->GetActors().Find(PRIChannel) : nullptr)
		{
			ArenaHitchLog::FScopedNote HitchNote(TEXT("player pawn character set"));
			Pawn->SetCharacter(GetStr(*PRI, TEXT("CharacterName")), GetInt(*PRI, TEXT("PlayerID")));
		}
	}
	if (Changes(TEXT("Physics")))
	{
		Pawn->SetNetPhysics(uint8(GetInt(A, TEXT("Physics"), 1)));
	}
	if (Changes(TEXT("bIsCrouched")) || Changes(TEXT("CollisionHeight")))
	{
		Pawn->SetNetCrouched(GetBool(A, TEXT("bIsCrouched")), GetFloat(A, TEXT("CollisionHeight"), 44.f));
	}
	if (Changes(TEXT("ViewPitch")))
	{
		Pawn->SetNetViewPitch(uint8(GetInt(A, TEXT("ViewPitch"))));
	}
	if (Changes(TEXT("Health")))
	{
		Pawn->Health = float(GetInt(A, TEXT("Health"), 100));
	}
	if (Changed.Contains(TEXT("ShieldStrength")))
	{
		Pawn->Shield = GetFloat(A, TEXT("ShieldStrength"));
	}
	if (Changed.Contains(TEXT("OverlayMaterial")) || Changed.Contains(TEXT("OverlayTimer")))
	{
		// Actor.OverlayMaterial / OverlayTimer (the server's SetOverlayMaterial)
		const FString Overlay = GetObjPath(A, TEXT("OverlayMaterial"));
		UMaterialInterface* M = Overlay.IsEmpty() || Overlay.Equals(TEXT("None")) ? nullptr : UE2NetModWeapons::LoadSkins({ Overlay })[0];
		Pawn->SetNetOverlay(M, GetFloat(A, TEXT("OverlayTimer")));
		static TSet<FString> Logged;
		if (M && !Logged.Contains(Overlay))
		{
			Logged.Add(Overlay);
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: pawn overlay %s for %.1f s"), *Overlay, GetFloat(A, TEXT("OverlayTimer")));
		}
	}
	if (Changes(TEXT("bFrozen")) && A.Class && A.Class->GetDefaultObject() && A.Class->GetDefaultObject()->FindProperty(FName(TEXT("bFrozen"))))
	{
		// Freeze Tag (WS3SPN Freon_Pawn, L7's freeze tag and its relatives): the replicated bFrozen; the client Tick stops
		// the animation and adds the class's FrostMaterial to the skins (Combiner CO_Add)
		static TMap<FUSClass*, TStrongObjectPtr<UMaterialInterface>> Ice;
		TStrongObjectPtr<UMaterialInterface>& M = Ice.FindOrAdd(A.Class);
		if (!M.IsValid())
		{
			M.Reset(ArenaRuntimeContent::CreateAdditiveLayer(DefaultText(A.Class, TEXT("FrostMaterial")), FLinearColor(0.55f, 0.65f, 0.8f), GetTransientPackage()));
		}
		const bool bFrozen = GetBool(A, TEXT("bFrozen"));
		if (bFrozen != Pawn->IsNetFrozen())
		{
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: %s %s (%s)"), *E.ClassName, bFrozen ? TEXT("frozen") : TEXT("thawed"), *DefaultText(A.Class, TEXT("FrostMaterial")));
		}
		Pawn->SetNetFrozen(bFrozen, M.Get());
	}
	if (Changes(TEXT("PawnPosition")) && PawnPos)
	{
		FVector Loc, Vel;
		FIntVector Rot = FIntVector::ZeroValue;
		if (MemberVec(PawnPos, TEXT("Location"), Loc) && MemberVec(PawnPos, TEXT("Velocity"), Vel))
		{
			FVector Dummy;
			MemberVec(PawnPos, TEXT("Rotation"), Dummy, &Rot);
			Pawn->SetNetPosition(Loc, Vel, Rot.Y, bInitial);
		}
	}
	else if (Changed.Contains(TEXT("Location")) || (bInitial && !PawnPos))
	{
		// plain Location (teleports, initial bunches without PawnPosition)
		const FVector Loc = GetVec(A, TEXT("Location"), ToVec(A.SpawnLocation));
		const FUE2NetValue* R = Find(A, TEXT("Rotation"));
		Pawn->SetNetPosition(Loc, GetVec(A, TEXT("Velocity")), R ? R->R.Y : A.SpawnRotation.Y, true);
	}
	if (!bInitial && Changed.Contains(TEXT("AnimAction")))
	{
		Pawn->NetAnimAction(FName(*GetStr(A, TEXT("AnimAction"))));
	}
	if (Changes(TEXT("HitFxTicker")))
	{
		const int32 Ticker = GetInt(A, TEXT("HitFxTicker"));
		if (E.LastHitFxTicker != INDEX_NONE && Ticker != E.LastHitFxTicker)
		{
			FName Bone;
			if (const FUE2NetValue* Fx = Find(A, *FString::Printf(TEXT("HitFx[%d]"), FMath::Max(0, (Ticker - 1) & 7))))
			{
				for (const TPair<FName, FUE2NetValue>& M : Fx->Members)
				{
					if (M.Key == TEXT("Bone"))
					{
						Bone = M.Value.N;
					}
				}
			}
			Pawn->NetHit(Bone);
		}
		E.LastHitFxTicker = Ticker;
	}
	if (Changes(TEXT("bTearOff")) && GetBool(A, TEXT("bTearOff")) && !E.bTornOff)
	{
		E.bTornOff = true;
		// xPawn.PlayDying: gibs for damage types that always gib (or a big overkill), else the ragdoll
		bool bGib = GetInt(A, TEXT("Health"), 0) <= -60;
		if (const FUE2NetValue* Dam = Find(A, TEXT("HitDamageType")))
		{
			bGib |= ClassDefault(*Dam, TEXT("bAlwaysGibs")).Equals(TEXT("True"), ESearchCase::IgnoreCase);
		}
		Pawn->NetDie(GetVec(A, TEXT("TearOffMomentum")), GetVec(A, TEXT("TakeHitLocation"), Pawn->GetActorLocation() / S), bGib);
	}
}

void FUE2NetWorld::ApplyAttachment(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	using namespace UE2NetWorldLocal;
	int32 PawnChannel = GetRef(A, TEXT("Instigator"));
	if (!PawnChannel)
	{
		PawnChannel = GetRef(A, TEXT("Base"));
	}
	if (PawnChannel)
	{
		E.PawnChannel = PawnChannel;
	}
	AUE2NetPawn* Pawn = PawnForChannel(E.PawnChannel);
	// our own weapon's attachment: the server sends it no FlashCount (WeaponAttachment replicates it !bNetOwner, the
	// owner plays its shots itself) but the bullet hits below
	const FUE2NetPlay* Play = FUE2NetPlay::Get();
	AActor* OwnPawn = Play && Play->IsOwnPawnChannel(E.PawnChannel) ? Play->GetLocalPawn() : nullptr;
	if (!Pawn && !OwnPawn)
	{
		return; // the pawn may open later: Tick retries
	}
	auto Changes = [&](const TCHAR* Key) { return bInitial || Changed.Contains(Key); };
	if (Pawn)
	{
		Pawn->SetNetWeapon(E.ClassPath);
		const uint8 Mode = uint8(GetInt(A, TEXT("FiringMode")));
		if (Changes(TEXT("FlashCount")))
		{
			const int32 Flash = GetInt(A, TEXT("FlashCount"));
			if (E.LastFlashCount != INDEX_NONE && Flash != E.LastFlashCount && Flash != 0)
			{
				Pawn->NetFired(Mode);
			}
			E.LastFlashCount = Flash;
		}
	}
	// instant hits of the bullet weapons (xWeaponAttachment.mHitLocation / SpawnHitCount); beam weapons replicate
	// their own beam effect actors (Beam kind)
	const bool bBullets = Has(E, TEXT("assaultattachment")) || Has(E, TEXT("minigunattachment"));
	if (bBullets && !bInitial && (Changed.Contains(TEXT("SpawnHitCount")) || Changed.Contains(TEXT("mHitLocation"))))
	{
		const int32 Hits = GetInt(A, TEXT("SpawnHitCount"));
		if (Hits != E.LastHitCount)
		{
			E.LastHitCount = Hits;
			UWorld* W = World.Get();
			const FVector End = GetVec(A, TEXT("mHitLocation")) * S;
			if (W && !End.IsNearlyZero())
			{
				const FVector Start = Pawn ? Pawn->GetMuzzleLocation() : OwnPawn->GetActorLocation();
				const FVector Normal = (Start - End).GetSafeNormal();
				if (Pawn)
				{
					ArenaFX::SpawnBeam(W, Start, End, FLinearColor(1.f, 0.85f, 0.5f), 1.5f, 0.06f, TEXT("Tracer.Bullet"));
				}
				// our own shots: the hit effect only, the local weapon drew the tracer along our view when it fired
				// (AArenaWeapon::PlayNetFireEffects)
				ArenaFX::SpawnImpact(W, End, Normal, FLinearColor(1.f, 0.8f, 0.4f), 8.f, TEXT("Impact.Bullet"));
			}
		}
	}
}

void FUE2NetWorld::SpawnBeam(FEntity& E, const FUE2NetActor& A)
{
	using namespace UE2NetWorldLocal;
	UWorld* W = World.Get();
	const FVector Start = ToVec(A.SpawnLocation) * S;
	const FVector End = GetVec(A, TEXT("mSpawnVecA")) * S;
	if (W && Has(E, TEXT("shockcombo")))
	{
		ArenaFX::SpawnExplosion(W, Start, FLinearColor(0.6f, 0.3f, 1.f), 250.f, TEXT("Explosion.ShockCombo"));
		// ShockProjectile.SuperExplosion: the core goes without its own explosion (closed now or in a moment)
		RecentCombos.Emplace(Start, FPlatformTime::Seconds());
		for (int32 i = PendingShockCores.Num() - 1; i >= 0; --i)
		{
			AUE2NetProjectile* Core = PendingShockCores[i].Key.Get();
			if (!Core || FVector::Dist(Core->GetActorLocation(), Start) < 250.f * S)
			{
				if (Core)
				{
					Core->Destroy();
				}
				PendingShockCores.RemoveAt(i);
			}
		}
		return;
	}
	if (W && (E.ClassName.StartsWith(TEXT("NewTransEffect")) || E.ClassName.StartsWith(TEXT("NewTransDeres"))))
	{
		// TransEffects where a pawn arrives, TransOutEffect where it left (the team colour is in the class)
		AArenaTransEffect::Spawn(W, Start, E.ClassName.Contains(TEXT("Blue")) ? 1 : 0, E.ClassName.StartsWith(TEXT("NewTransDeres")));
		return;
	}
	if (W && Has(E, TEXT("smallredeemerexplosion")))
	{
		// RedeemerProjectile / RedeemerWarhead shot down or blown up at the launcher: SmallRedeemerExplosion
		ArenaFX::SpawnExplosion(W, Start, FLinearColor(1.f, 0.5f, 0.2f), 600.f, TEXT("Explosion.Rocket"));
		return;
	}
	if (W && Has(E, TEXT("redeemerexplosion")))
	{
		// RedeemerProjectile.BlowUp: ExplosionEffectClass; the nuke our own warhead shell shows (stock emitter, else a
		// procedural blast)
		if (!AArenaWarheadShell::SpawnNukeEffects(W, Start, FVector::ZeroVector))
		{
			ArenaFX::SpawnExplosion(W, Start, FLinearColor(1.f, 0.55f, 0.2f), 1200.f, TEXT("Explosion.Rocket"));
		}
		return;
	}
	if (W && HasLike(E, TEXT("wallhiteffect")))
	{
		// ClassicSniperFire.DoTrace: SniperWallHitEffect at the hit, turned along -X of the shot (mods' copies too:
		// ProSniperRifle's ProSniperWallHitEffect, IK_Stealth's Stealth_WallHitEffect)
		const FRotator Rot(A.SpawnRotation.X * 360.f / 65536.f, A.SpawnRotation.Y * 360.f / 65536.f, 0.f);
		ArenaFX::SpawnImpact(W, Start, Rot.Vector(), FLinearColor(1.f, 0.8f, 0.4f), 12.f, TEXT("Impact.Bullet"));
		return;
	}
	if (!W || End.IsNearlyZero())
	{
		return;
	}
	const FVector Normal = GetVec(A, TEXT("HitNormal"), (Start - End).GetSafeNormal());
	// our own shock beams leave our first-person weapon (ShockBeamEffect of a first-person Instigator starts at its
	// weapon's GetEffectStart); the server starts them at our eye. NewLightningBolt already starts at the server's
	// first-person effect location (SniperFire ArcEnd)
	FVector From = Start;
	const FUE2NetPlay* Play = FUE2NetPlay::Get();
	const AUE2NetLocalPawn* Own = Play && Play->IsOwnPawnChannel(GetRef(A, TEXT("Instigator"))) ? Play->GetLocalPawn() : nullptr;
	if (AArenaWeapon* Weapon = Own ? Own->GetCurrentWeapon() : nullptr)
	{
		From = Weapon->GetEffectStart();
	}
	if (Has(E, TEXT("newlightningbolt")))
	{
		ArenaFX::SpawnBeam(W, From, End, FLinearColor(0.5f, 0.6f, 1.f), 3.f, 0.4f, TEXT("Beam.Lightning"));
		ArenaFX::SpawnImpact(W, End, Normal, FLinearColor(0.5f, 0.6f, 1.f), 20.f, TEXT("Impact.Lightning"));
	}
	else if (E.ClassName.Contains(TEXT("SuperShock")) || E.ClassName.Contains(TEXT("Insta")))
	{
		ArenaFX::SpawnBeam(W, From, End, FLinearColor(1.f, 0.2f, 0.2f), 4.f, 0.3f, TEXT("Beam.Insta"));
		ArenaFX::SpawnImpact(W, End, Normal, FLinearColor(1.f, 0.2f, 0.2f), 25.f, TEXT("Impact.Insta"));
	}
	else
	{
		ArenaFX::SpawnBeam(W, From, End, FLinearColor(0.6f, 0.3f, 1.f), 4.f, 0.3f, TEXT("Beam.Shock"));
		ArenaFX::SpawnImpact(W, End, Normal, FLinearColor(0.6f, 0.3f, 1.f), 25.f, TEXT("Impact.ShockBeam"));
	}
}

void FUE2NetWorld::ApplyHeldBeam(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	using namespace UE2NetWorldLocal;
	UWorld* W = World.Get();
	const int32 Inst = GetRef(A, TEXT("Instigator"));
	const FUE2NetPlay* Play = FUE2NetPlay::Get();
	const bool bPainter = Has(E, TEXT("painterbeameffect"));
	if (Play && Play->IsOwnPawnChannel(Inst))
	{
		// ours: our weapon draws it while we hold fire and while the server's effect fades; the painter's mark state is
		// the server's (TargetState)
		AArenaCharacter* Own = Play->GetLocalPawn();
		AArenaWeapon* Weapon = Own && bPainter ? Own->FindWeapon(AWeap_OrbitalDesignator::StaticClass()) : nullptr;
		if (Weapon)
		{
			Weapon->NetState.PainterTargetState = uint8(GetInt(A, TEXT("TargetState")));
			Weapon->NetState.bPainterBeamOpen = true;
			if (Weapon != Own->GetCurrentWeapon() && Changed.Contains(TEXT("TargetState")) && Weapon->NetState.PainterTargetState == 2)
			{
				// the strike put the painter down (ClientSwitchToBestWeapon) before its beam's Aquired: its AquiredSound
				if (USoundBase* Snd = ArenaArt::LoadOneShotSound(TEXT("Sounds/WeaponSounds/TAGRifle_TAGTargetAquired")))
				{
					UGameplayStatics::PlaySoundAtLocation(Own, Snd, Own->GetActorLocation());
				}
			}
		}
		return;
	}
	if (!W)
	{
		return;
	}
	AUE2NetBeam* Beam = Cast<AUE2NetBeam>(E.Native.Get());
	if (!Beam)
	{
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		P.ObjectFlags |= RF_Transient;
		Beam = W->SpawnActor<AUE2NetBeam>(AUE2NetBeam::StaticClass(), ToVec(A.SpawnLocation) * S, FRotator::ZeroRotator, P);
		if (!Beam)
		{
			return;
		}
		Beam->Setup(bPainter ? AUE2NetBeam::EType::Painter : (Has(E, TEXT("linkbeameffect")) ? AUE2NetBeam::EType::Link : AUE2NetBeam::EType::MineLaser));
		E.Native = Beam;
		E.bOwned = true;
	}
	// LinkBeamEffect: StartEffect / EndEffect; PainterBeamEffect: EndEffect, TargetState; ONSMineLayerTargetBeamEffect:
	// EndEffect, bCancel (it fades out). The beam leaves the firing pawn's weapon as we show it.
	const bool bCancelled = GetBool(A, TEXT("bCancel"));
	Beam->SetEnds(PawnForChannel(Inst), GetVec(A, TEXT("StartEffect"), ToVec(A.SpawnLocation)) * S, bCancelled ? FVector::ZeroVector : GetVec(A, TEXT("EndEffect")) * S);
	Beam->SetTargetState(uint8(GetInt(A, TEXT("TargetState"))));
}

// =================================================================================================
// Projectiles, pickups, movers, objectives
// =================================================================================================

void FUE2NetWorld::ApplyProjectile(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	using namespace UE2NetWorldLocal;
	UWorld* W = World.Get();
	AUE2NetProjectile* Proj = Cast<AUE2NetProjectile>(E.Native.Get());
	if (!Proj)
	{
		const FUE2NetPlay* Play = FUE2NetPlay::Get();
		if (!W || (Play && Play->IsOwnPawnChannel(A.Channel)) || GetBool(A, TEXT("bHidden")))
		{
			return; // our own guided warhead (flown by Net/UE2NetPlay), or one already blown up
		}
		const FVector Loc = GetVec(A, TEXT("Location"), ToVec(A.SpawnLocation)) * S;
		const FRotator Rot(A.SpawnRotation.X * 360.f / 65536.f, A.SpawnRotation.Y * 360.f / 65536.f, 0.f);
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		P.ObjectFlags |= RF_Transient;
		Proj = W->SpawnActor<AUE2NetProjectile>(AUE2NetProjectile::StaticClass(), Loc, Rot, P);
		if (!Proj)
		{
			return;
		}
		FName Style;
		float Scale = 1.f;
		FLinearColor Glow;
		ProjectileStyle(E.Chain, Style, Scale, Glow);
		Proj->Setup(Style, Scale, Glow);
		// UE2 class defaults: bNetTemporary, LifeSpan; bouncing for flak chunks and grenades
		const bool bBounceChunk = Has(E, TEXT("flakchunk"));
		const bool bGrenade = Has(E, TEXT("grenade")) || Has(E, TEXT("onsgrenadeprojectile"));
		Proj->SetLocalSimulation(GetBool(A, TEXT("bNetTemporary")), bBounceChunk ? 2 : (bGrenade ? 8 : 0), bBounceChunk ? 0.65f : 0.5f,
			GetFloat(A, TEXT("LifeSpan"), 0.f));
		E.Native = Proj;
		E.bOwned = true;
		bInitial = true;
		// Velocity is in the initial bunch; without it the class default Speed along the spawn rotation
		if (!Find(A, TEXT("Velocity")))
		{
			Proj->SetNetVelocity(Rot.Vector() * GetFloat(A, TEXT("Speed"), 0.f));
		}
	}
	auto Changes = [&](const TCHAR* Key) { return bInitial || Changed.Contains(Key); };
	if (Changes(TEXT("Physics")))
	{
		Proj->SetNetPhysics(uint8(GetInt(A, TEXT("Physics"), 6)));
	}
	if (!bInitial && Changed.Contains(TEXT("Location")))
	{
		Proj->SetNetLocation(GetVec(A, TEXT("Location")));
	}
	if (Find(A, TEXT("Velocity")) && Changes(TEXT("Velocity")))
	{
		Proj->SetNetVelocity(GetVec(A, TEXT("Velocity")));
	}
}

void FUE2NetWorld::ApplyPickup(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	using namespace UE2NetWorldLocal;
	UWorld* W = World.Get();
	if (!W)
	{
		return;
	}
	AArenaPickup* Pickup = Cast<AArenaPickup>(E.Native.Get());
	if (!Pickup && bInitial)
	{
		// the map's pickups and the items on pickup bases are spawned actors in UE2: match ours by location
		const FVector Loc = GetVec(A, TEXT("Location"), ToVec(A.SpawnLocation)) * S;
		float Best = FMath::Square(160.f);
		for (TActorIterator<AArenaPickup> It(W); It; ++It)
		{
			const float D = FVector::DistSquared(It->GetActorLocation(), Loc);
			if (D < Best)
			{
				Best = D;
				Pickup = *It;
			}
		}
		if (Pickup)
		{
			E.Native = Pickup;
			++PickupsMatched;
			// a mod's pickup in place of the stock one (ProSniperPickup for ClassicSniperRiflePickup): its own mesh
			if (UStaticMesh* ModMesh = UE2NetModWeapons::FindPickupMesh(E.ClassPath))
			{
				Pickup->SetNetItemMesh(ModMesh);
			}
		}
		else
		{
			++PickupsUnmatched; // dropped weapons, mod pickups
		}
	}
	if (!Pickup)
	{
		// not one of the map's: drawn as its static mesh where the server has it (a dropped weapon, a mutator's pickup)
		ApplyDecoration(E, A, Changed, bInitial);
		return;
	}
	if (Pickup && (bInitial || Changed.Contains(TEXT("bHidden"))))
	{
		Pickup->SetNetAvailable(!GetBool(A, TEXT("bHidden")));
	}
}

bool FUE2NetWorld::RetireClientAttachmentStandIn(int32 Channel, const FString& ClassPath)
{
	const FEntity* E = Channel ? Entities.Find(Channel) : nullptr;
	AUE2NetVehicle* V = E && E->Kind == EKind::VehicleWeapon ? Cast<AUE2NetVehicle>(E->Native.Get()) : nullptr;
	if (!V)
	{
		return false;
	}
	if (V->RemoveWeaponAttachment(Channel, ClassPath))
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: %s on %s is shown by the mod's client code: the class-defaults stand-in goes"), *ClassPath, *E->ClassName);
	}
	return true;
}

USceneComponent* FUE2NetWorld::GetChannelComponent(int32 Channel) const
{
	const FEntity* E = Channel ? Entities.Find(Channel) : nullptr;
	if (E && E->Kind == EKind::Attachment)
	{
		// a weapon attachment: the third-person weapon of its pawn (ours: our pawn's)
		const FEntity* PE = E->PawnChannel ? Entities.Find(E->PawnChannel) : nullptr;
		AArenaCharacter* Pawn = PE ? Cast<AArenaCharacter>(PE->Native.Get()) : nullptr;
		if (const FUE2NetPlay* Play = FUE2NetPlay::Get(); !Pawn && Play && Play->IsOwnPawnChannel(E->PawnChannel))
		{
			Pawn = Cast<AArenaCharacter>(Play->GetLocalPawn());
		}
		return Pawn ? (Pawn->GetThirdPersonWeapon() ? static_cast<USceneComponent*>(Pawn->GetThirdPersonWeapon()) : Pawn->GetMesh()) : nullptr;
	}
	AActor* A = E ? E->Native.Get() : nullptr;
	if (!A)
	{
		// our own weapon (inventory, not a world entity) run by the mod's script: its first-person view model
		const FUE2NetPlay* Play = FUE2NetPlay::Get();
		return Play ? Play->GetScriptWeaponComponent(Channel) : nullptr;
	}
	if (AUE2NetVehicle* V = Cast<AUE2NetVehicle>(A))
	{
		if (E->Kind == EKind::VehicleWeapon)
		{
			return V->GetWeaponComponent(Channel);
		}
		return V->GetBodyComponent();
	}
	if (ACharacter* C = Cast<ACharacter>(A))
	{
		return C->GetMesh();
	}
	return A->GetRootComponent();
}

void FUE2NetWorld::ApplyDecoration(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	using namespace UE2NetWorldLocal;
	UWorld* W = World.Get();
	auto Changes = [&](const TCHAR* Key) { return bInitial || Changed.Contains(Key); };
	// (an actor placed in the map is drawn with the converted map)
	if (!W || A.bStatic || !(Changes(TEXT("DrawType")) || Changes(TEXT("StaticMesh")) || Changes(TEXT("bHidden")) || Changes(TEXT("Base")) || Changes(TEXT("AttachmentBone"))
		|| Changes(TEXT("RelativeLocation")) || Changes(TEXT("RelativeRotation")) || Changes(TEXT("Location")) || Changes(TEXT("Rotation"))
		|| Changes(TEXT("DrawScale")) || Changes(TEXT("DrawScale3D"))))
	{
		return;
	}
	// what the actor draws: DT_StaticMesh only (meshes and sprites of unknown classes need their code)
	FString MeshPath = GetObjPath(A, TEXT("StaticMesh"));
	if (MeshPath.IsEmpty() && !Find(A, TEXT("StaticMesh")))
	{
		MeshPath = DefaultText(A.Class, TEXT("StaticMesh"));
	}
	const bool bShow = GetInt(A, TEXT("DrawType"), 1) == 8 && !MeshPath.IsEmpty() && !MeshPath.Equals(TEXT("None")) && !GetBool(A, TEXT("bHidden"));
	AActor* Deco = E.Native.Get();
	if (!bShow)
	{
		if (Deco)
		{
			Deco->SetActorHiddenInGame(true);
		}
		return;
	}
	if (!Deco)
	{
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		P.ObjectFlags |= RF_Transient;
		Deco = W->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, P);
		if (!Deco)
		{
			return;
		}
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(Deco, TEXT("Mesh"));
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetGenerateOverlapEvents(false);
		C->SetCanEverAffectNavigation(false);
		Deco->SetRootComponent(C);
		C->RegisterComponent();
		E.Native = Deco;
		E.bOwned = true;
	}
	UStaticMeshComponent* C = Cast<UStaticMeshComponent>(Deco->GetRootComponent());
	if (!C)
	{
		return;
	}
	UE2NetModContent::EnsureConverted(MeshPath, TEXT("StaticMesh"));
	UStaticMesh* Mesh = ArenaRuntimeContent::LoadStaticMeshByObjectPath(MeshPath);
	if (C->GetStaticMesh() != Mesh)
	{
		C->SetStaticMesh(Mesh);
		const TArray<UMaterialInterface*> Skins = UE2NetModWeapons::LoadSkins(UE2NetModWeapons::FindLook(E.ClassPath).Skins);
		for (int32 i = 0; i < Skins.Num() && i < C->GetNumMaterials(); ++i)
		{
			if (Skins[i])
			{
				C->SetMaterial(i, Skins[i]);
			}
		}
	}
	Deco->SetActorHiddenInGame(Mesh == nullptr);
	auto Rot = [this, &A](const TCHAR* Key)
	{
		const FUE2NetValue* V = Find(A, Key);
		return V ? FRotator(V->R.X * 360.f / 65536.f, V->R.Y * 360.f / 65536.f, V->R.Z * 360.f / 65536.f) : DefaultRotator(A.Class, Key);
	};
	const FVector Scale3D = Find(A, TEXT("DrawScale3D")) ? GetVec(A, TEXT("DrawScale3D")) : DefaultVector(A.Class, TEXT("DrawScale3D"));
	C->SetUsingAbsoluteScale(true);
	C->SetRelativeScale3D((Scale3D.IsNearlyZero() ? FVector::OneVector : Scale3D) * GetFloat(A, TEXT("DrawScale"), 1.f));
	// on its Base (a player's bone: AttachToBone), else where it is
	const FEntity* BaseE = FindEntity(GetRef(A, TEXT("Base")));
	AActor* BaseActor = BaseE ? BaseE->Native.Get() : nullptr;
	if (BaseActor)
	{
		USceneComponent* Parent = BaseActor->GetRootComponent();
		if (ACharacter* Ch = Cast<ACharacter>(BaseActor); Ch && Ch->GetMesh())
		{
			Parent = Ch->GetMesh();
		}
		FName Socket = NAME_None;
		const FString Bone = GetStr(A, TEXT("AttachmentBone"));
		if (!Bone.IsEmpty() && Bone != TEXT("None") && Parent && Parent->DoesSocketExist(FName(*Bone)))
		{
			Socket = FName(*Bone);
		}
		if (C->GetAttachParent() != Parent || C->GetAttachSocketName() != Socket)
		{
			Deco->AttachToComponent(Parent, FAttachmentTransformRules::KeepRelativeTransform, Socket);
		}
		C->SetRelativeLocationAndRotation(GetVec(A, TEXT("RelativeLocation")) * S, Rot(TEXT("RelativeRotation")));
	}
	else
	{
		if (C->GetAttachParent())
		{
			Deco->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		}
		Deco->SetActorLocationAndRotation(GetVec(A, TEXT("Location"), ToVec(A.SpawnLocation)) * S, Rot(TEXT("Rotation")));
	}
	const FVector PrePivot = (Find(A, TEXT("PrePivot")) ? GetVec(A, TEXT("PrePivot")) : DefaultVector(A.Class, TEXT("PrePivot"))) * S;
	if (!PrePivot.IsNearlyZero())
	{
		PrePivoted.Add(A.Channel, { C, C->GetRelativeLocation(), PrePivot });
	}
	else
	{
		PrePivoted.Remove(A.Channel);
	}
	static TSet<FUSClass*> Logged;
	if (bInitial && !Logged.Contains(A.Class))
	{
		Logged.Add(A.Class);
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: %s drawn as its static mesh %s (%s)%s"), *E.ClassPath, *MeshPath, Mesh ? TEXT("loaded") : TEXT("not converted"),
			BaseActor ? *FString::Printf(TEXT(", on %s"), *GetStr(A, TEXT("AttachmentBone"))) : TEXT(""));
	}
}

void FUE2NetWorld::ApplyMover(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	using namespace UE2NetWorldLocal;
	AArenaMover* Mover = Cast<AArenaMover>(E.Native.Get());
	if (!Mover && bInitial && A.bStatic)
	{
		FString Path = Connection->GetPackageMap().GetObjectPath(A.Archetype);
		int32 Dot = INDEX_NONE;
		if (Path.FindLastChar(TEXT('.'), Dot))
		{
			Path.RightChopInline(Dot + 1);
		}
		Mover = Cast<AArenaMover>(ArenaUE2World::FindActorByName(World.Get(), Path));
		E.Native = Mover;
		if (!Mover)
		{
			UE_LOG(LogUE2Net, Verbose, TEXT("UE2NetWorld: no native mover %s"), *Path);
		}
	}
	if (!Mover)
	{
		return;
	}
	if (Changed.Contains(TEXT("SimInterpolate")))
	{
		// Mover.SimInterpolate: X = 100 * PhysAlpha, Y = 100 * PhysRate, Z = 256 * PrevKeyNum + KeyNum
		const FVector Sim = GetVec(A, TEXT("SimInterpolate"));
		const int32 Keys = FMath::RoundToInt(Sim.Z);
		Mover->SetNetMove(Keys & 255, Keys >> 8, float(Sim.X) / 100.f, float(Sim.Y) / 100.f);
	}
	else if (Changed.Contains(TEXT("RealPosition")) || Changed.Contains(TEXT("RealRotation")))
	{
		const FUE2NetValue* R = Find(A, TEXT("RealRotation"));
		const FRotator Rot = R ? FRotator(R->R.X * 360.f / 65536.f, R->R.Y * 360.f / 65536.f, R->R.Z * 360.f / 65536.f) : Mover->BaseRot;
		Mover->SetNetRest(GetVec(A, TEXT("RealPosition"), Mover->BasePos / S) * S, Rot);
	}
}

void FUE2NetWorld::ApplyFlag(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	using namespace UE2NetWorldLocal;
	UWorld* W = World.Get();
	if (!W)
	{
		return;
	}
	AArenaFlag* Flag = Cast<AArenaFlag>(E.Native.Get());
	if (!Flag)
	{
		int32 Team = TeamIndexOf(GetRef(A, TEXT("Team")));
		if (Team != 0 && Team != 1)
		{
			Team = Has(E, TEXT("xblueflag")) || E.ClassName.Contains(TEXT("Blue")) ? 1 : 0;
		}
		for (TActorIterator<AArenaFlag> It(W); It; ++It)
		{
			if (It->GetTeam() == Team)
			{
				Flag = *It;
				break;
			}
		}
		E.Native = Flag;
		if (!Flag)
		{
			return;
		}
	}
	// GameObject: bHome / bHeld and the holder's PRI; the holder pawn may not be relevant (then the flag is shown
	// where the server says it is)
	const int32 HolderPRI = GetRef(A, TEXT("HolderPRI"));
	AArenaCharacter* Holder = GetBool(A, TEXT("bHeld")) ? (HolderPRI ? CharacterOfPRI(HolderPRI) : PawnForChannel(GetRef(A, TEXT("Holder")))) : nullptr;
	if (!Holder && GetBool(A, TEXT("bHeld")))
	{
		Holder = PawnForChannel(GetRef(A, TEXT("Base")));
	}
	if (Holder)
	{
		Flag->SetNetState(EArenaFlagState::Carried, Holder, FVector::ZeroVector);
	}
	else if (GetBool(A, TEXT("bHome"), true))
	{
		Flag->SetNetState(EArenaFlagState::Home, nullptr, FVector::ZeroVector);
	}
	else
	{
		const FVector Loc = GetVec(A, TEXT("Location"), ToVec(A.SpawnLocation)) * S;
		if (!Loc.IsNearlyZero())
		{
			Flag->SetNetState(EArenaFlagState::Dropped, nullptr, Loc - FVector(0.f, 0.f, 40.f));
		}
	}
}

void FUE2NetWorld::ApplyDomPoint(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	using namespace UE2NetWorldLocal;
	UWorld* W = World.Get();
	if (!W)
	{
		return;
	}
	AArenaDOM_ControlPoint* Point = Cast<AArenaDOM_ControlPoint>(E.Native.Get());
	if (!Point)
	{
		// xDomPointA / xDomPointB: by the letter, else the nearest point
		const FString Letter = E.ClassName.Right(1).ToUpper();
		const FVector Loc = GetVec(A, TEXT("Location"), ToVec(A.SpawnLocation)) * S;
		float Best = TNumericLimits<float>::Max();
		for (TActorIterator<AArenaDOM_ControlPoint> It(W); It; ++It)
		{
			if (It->Label.Equals(Letter, ESearchCase::IgnoreCase))
			{
				Point = *It;
				break;
			}
			const float D = Loc.IsNearlyZero() ? 0.f : FVector::DistSquared(It->GetActorLocation(), Loc);
			if (D < Best)
			{
				Best = D;
				Point = *It;
			}
		}
		E.Native = Point;
		if (!Point)
		{
			return;
		}
	}
	// DominationPoint: ControllingTeam (a TeamInfo), bControllable (false while locked after a score)
	if (!GetBool(A, TEXT("bControllable"), true))
	{
		if (Point->bEnabled)
		{
			Point->SetDisabled(W->GetTimeSeconds() + 10.f);
		}
		return;
	}
	if (!Point->bEnabled)
	{
		Point->SetEnabled();
	}
	const int32 Team = TeamIndexOf(GetRef(A, TEXT("ControllingTeam")));
	if (Point->OwnerTeam != Team)
	{
		Point->SetOwnerTeam(Team, nullptr);
	}
}

void FUE2NetWorld::ApplyPlaceholder(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	using namespace UE2NetWorldLocal;
	UWorld* W = World.Get();
	if (!W)
	{
		return;
	}
	AUE2NetPlaceholder* Ph = Cast<AUE2NetPlaceholder>(E.Native.Get());
	const FVector Loc = GetVec(A, TEXT("Location"), ToVec(A.SpawnLocation));
	if (!Ph)
	{
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		P.ObjectFlags |= RF_Transient;
		Ph = W->SpawnActor<AUE2NetPlaceholder>(AUE2NetPlaceholder::StaticClass(), Loc * S, FRotator::ZeroRotator, P);
		if (!Ph)
		{
			return;
		}
		// the class's own static mesh when it has one (StaticMesh default, e.g. the ball), else a box of its size
		FString MeshPath = GetObjPath(A, TEXT("StaticMesh"));
		if (MeshPath.IsEmpty())
		{
			MeshPath = DefaultText(A.Class, TEXT("StaticMesh"));
		}
		FString Art;
		FString Pkg, Rest;
		if (MeshPath.Split(TEXT("."), &Pkg, &Rest) && !MeshPath.Equals(TEXT("None")))
		{
			Art = FString::Printf(TEXT("StaticMeshes/%s/%s"), *Pkg, *Rest.Replace(TEXT("."), TEXT("_")));
		}
		// vehicles: the class's skeletal mesh (Actor.Mesh default, e.g. ONSVehicles-A.HoverBike)
		FString SkelArt;
		const FString SkelPath = DefaultText(A.Class, TEXT("Mesh"));
		if (SkelPath.Split(TEXT("."), &Pkg, &Rest) && !SkelPath.Equals(TEXT("None")))
		{
			SkelArt = FString::Printf(TEXT("SkeletalMeshes/%s/%s"), *Pkg, *Rest.Replace(TEXT("."), TEXT("_")));
		}
		Ph->Setup(E.ClassName, GetFloat(A, TEXT("CollisionRadius"), 40.f), GetFloat(A, TEXT("CollisionHeight"), 40.f), ArenaTeam::None, Art, SkelArt);
		E.Native = Ph;
		E.bOwned = true;
		bInitial = true;
	}
	if (bInitial || Changed.Contains(TEXT("Team")))
	{
		const int32 TeamRef = GetRef(A, TEXT("Team"));
		Ph->SetTeam(TeamRef ? TeamIndexOf(TeamRef) : GetInt(A, TEXT("Team"), ArenaTeam::None));
	}
	// Vehicle.Driver: that pawn rides along hidden (the camera follows the vehicle)
	if (E.Kind == EKind::Vehicle && (bInitial || Changed.Contains(TEXT("Driver"))))
	{
		const int32 Driver = GetRef(A, TEXT("Driver"));
		if (AUE2NetPawn* Old = PawnForChannel(E.PawnChannel); Old && E.PawnChannel != Driver)
		{
			Old->SetNetVehicle(nullptr);
		}
		E.PawnChannel = Driver;
		if (AUE2NetPawn* Pawn = PawnForChannel(Driver))
		{
			Pawn->SetNetVehicle(Ph);
		}
	}
	// vehicles: the KRigidBody state struct (HoverState, CarState, CopState, TreadState ...: ChassisPosition,
	// ChassisQuaternion, ChassisLinVel); other objects: Location / Rotation
	for (const FString& Key : Changed)
	{
		const FUE2NetValue* V = Find(A, *Key);
		if (!V || V->Kind != FUE2NetValue::EKind::Struct)
		{
			continue;
		}
		const FUE2NetValue* Pos = nullptr;
		const FUE2NetValue* Quat = nullptr;
		const FUE2NetValue* LinVel = nullptr;
		for (const TPair<FName, FUE2NetValue>& M : V->Members)
		{
			if (M.Key == TEXT("ChassisPosition")) Pos = &M.Value;
			else if (M.Key == TEXT("ChassisQuaternion")) Quat = &M.Value;
			else if (M.Key == TEXT("ChassisLinVel")) LinVel = &M.Value;
		}
		if (Pos && Quat && Quat->Members.Num() == 4)
		{
			const FQuat Q(Quat->Members[0].Value.F, Quat->Members[1].Value.F, Quat->Members[2].Value.F, Quat->Members[3].Value.F);
			Ph->SetNetRigidBody(ToVec(Pos->V), Q.GetNormalized(), LinVel ? ToVec(LinVel->V) : FVector::ZeroVector);
			return;
		}
	}
	FVector Vel = GetVec(A, TEXT("Velocity"));
	FRotator Rot = FRotator::ZeroRotator;
	if (const FUE2NetValue* R = Find(A, TEXT("Rotation")))
	{
		Rot = FRotator(R->R.X * 360.f / 65536.f, R->R.Y * 360.f / 65536.f, R->R.Z * 360.f / 65536.f);
	}
	else
	{
		Rot = FRotator(A.SpawnRotation.X * 360.f / 65536.f, A.SpawnRotation.Y * 360.f / 65536.f, A.SpawnRotation.Z * 360.f / 65536.f);
	}
	if (bInitial || Changed.Contains(TEXT("Location")) || Changed.Contains(TEXT("Velocity")) || Changed.Contains(TEXT("Rotation")))
	{
		Ph->SetNetLocation(Loc, Vel, Rot);
	}
}

// =================================================================================================
// Vehicles, gunner seats and vehicle weapons
// =================================================================================================

FVector FUE2NetWorld::DefaultVector(FUSClass* Class, const TCHAR* Key) const
{
	const FString T = DefaultText(Class, Key);
	FVector V = FVector::ZeroVector;
	FParse::Value(*T, TEXT("X="), V.X);
	FParse::Value(*T, TEXT("Y="), V.Y);
	FParse::Value(*T, TEXT("Z="), V.Z);
	return V;
}

FRotator FUE2NetWorld::DefaultRotator(FUSClass* Class, const TCHAR* Key) const
{
	const FString T = DefaultText(Class, Key);
	int32 P = 0, Y = 0, R = 0;
	FParse::Value(*T, TEXT("Pitch="), P);
	FParse::Value(*T, TEXT("Yaw="), Y);
	FParse::Value(*T, TEXT("Roll="), R);
	return FRotator(P * 360.f / 65536.f, Y * 360.f / 65536.f, R * 360.f / 65536.f);
}

AUE2NetVehicle* FUE2NetWorld::VehicleForChannel(int32 Channel, int32 Depth)
{
	FEntity* E = FindEntity(Channel);
	if (!E || Depth > 2)
	{
		return nullptr;
	}
	if (AUE2NetVehicle* V = Cast<AUE2NetVehicle>(E->Native.Get()))
	{
		return V;
	}
	if (E->Kind == EKind::WeaponPawn && E->BaseChannel && E->BaseChannel != Channel)
	{
		return VehicleForChannel(E->BaseChannel, Depth + 1);
	}
	return nullptr;
}

AUE2NetVehicle* FUE2NetWorld::SpawnVehicle(FEntity& E, const FUE2NetActor& A)
{
	using namespace UE2NetWorldLocal;
	UWorld* W = World.Get();
	if (!W)
	{
		return nullptr;
	}
	const FVector Loc = GetVec(A, TEXT("Location"), ToVec(A.SpawnLocation));
	const FUE2NetValue* RotV = Find(A, TEXT("Rotation"));
	const FIntVector R = RotV ? RotV->R : FIntVector(A.SpawnRotation.X, A.SpawnRotation.Y, A.SpawnRotation.Z);
	const FRotator Rot(R.X * 360.f / 65536.f, R.Y * 360.f / 65536.f, R.Z * 360.f / 65536.f);
	FActorSpawnParameters P;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	P.ObjectFlags |= RF_Transient;
	AUE2NetVehicle* V = W->SpawnActor<AUE2NetVehicle>(AUE2NetVehicle::StaticClass(), Loc * S, Rot, P);
	if (!V)
	{
		return nullptr;
	}
	// the class's own art (Actor.Mesh, else its StaticMesh when drawn as one), skins and wreck
	auto Content = [](const FString& ObjectPath, const TCHAR* Folder)
	{
		FString Pkg, Rest;
		if (ObjectPath.IsEmpty() || ObjectPath.Equals(TEXT("None")) || !ObjectPath.Split(TEXT("."), &Pkg, &Rest))
		{
			return FString();
		}
		return FString::Printf(TEXT("%s/%s/%s"), Folder, *Pkg, *Rest.Replace(TEXT("."), TEXT("_")));
	};
	const int32 DrawType = GetInt(A, TEXT("DrawType"), 2);
	FString StaticMesh = GetObjPath(A, TEXT("StaticMesh"));
	if (StaticMesh.IsEmpty())
	{
		StaticMesh = DefaultText(A.Class, TEXT("StaticMesh"));
	}
	const FString Mesh = DrawType == 2 ? Content(DefaultText(A.Class, TEXT("Mesh")), TEXT("SkeletalMeshes")) : FString();
	const FString SM = DrawType == 8 ? Content(StaticMesh, TEXT("StaticMeshes")) : FString();
	const float Radius = DrawType == 0 && Mesh.IsEmpty() && SM.IsEmpty() ? -1.f : GetFloat(A, TEXT("CollisionRadius"), 40.f);
	// a mod's mesh that is not in the cache yet (a mod installed in the UT2004 folder, not downloaded)
	if (DrawType == 2)
	{
		UE2NetModContent::EnsureConverted(DefaultText(A.Class, TEXT("Mesh")), TEXT("SkeletalMesh"));
	}
	else if (DrawType == 8)
	{
		UE2NetModContent::EnsureConverted(StaticMesh, TEXT("StaticMesh"));
	}
	V->Setup(E.ClassName, Mesh, SM, UE2NetModWeapons::FindLook(E.ClassPath), DefaultText(A.Class, TEXT("DestroyedVehicleMesh")), Radius,
		GetFloat(A, TEXT("CollisionHeight"), 40.f));
	V->SetNetLocation(Loc, GetVec(A, TEXT("Velocity")), Rot, true);
	// SVehicle.Wheels: the class's wheel subobjects (BoneName, WheelRadius, SteerType) from the UScript defaults
	TArray<AUE2NetVehicle::FNetWheel> Wheels;
	FUSObject* Def = A.Class ? A.Class->GetDefaultObject() : nullptr;
	FUSProperty* WheelsProp = Def && Def->Data ? Def->FindProperty(FName(TEXT("Wheels"))) : nullptr;
	if (WheelsProp && WheelsProp->Type == EUSType::Array && WheelsProp->Inner && WheelsProp->Inner->Type == EUSType::Object)
	{
		const FUSScriptArray* Arr = reinterpret_cast<const FUSScriptArray*>(Def->Data + WheelsProp->Offset);
		for (int32 i = 0; i < Arr->Num && Arr->Data; ++i)
		{
			FUSObject* WheelObj = reinterpret_cast<FUSObject* const*>(Arr->Data)[i];
			if (WheelObj && !WheelObj->Data && WheelObj->Class)
			{
				// the wheel subobjects (defaultproperties "Begin Object Class=SVehicleWheel") are shells: read their saved
				// properties from the package into a transient copy (cached by path)
				static TMap<FString, FUSObject*> Loaded;
				const FString Path = WheelObj->GetPathName();
				FUSObject*& Copy = Loaded.FindOrAdd(Path);
				if (!Copy)
				{
					FString Pkg, Rel;
					Path.Split(TEXT("."), &Pkg, &Rel);
					FUSLinker* const* Linker = FUSRuntime::Get().Linkers.Find(FName(*Pkg.ToLower()));
					if (!Linker)
					{
						Linker = FUSRuntime::Get().Linkers.Find(FName(*Pkg));
					}
					const int32 Ei = Linker && *Linker ? (*Linker)->FindExport(Rel, WheelObj->Class->GetName()) : INDEX_NONE;
					Copy = FUSWorld::Transient().ConstructObject(WheelObj->Class);
					if (Ei == INDEX_NONE || !Copy || !(*Linker)->LoadInstanceProperties(Ei, Copy))
					{
						UE_LOG(LogUE2Net, Verbose, TEXT("UE2NetWorld: wheel %s: saved properties not read"), *Path);
					}
				}
				WheelObj = Copy;
			}
			auto Text = [WheelObj](const TCHAR* Key)
			{
				FUSProperty* P = WheelObj && WheelObj->Data ? WheelObj->FindProperty(FName(Key)) : nullptr;
				FString Out;
				if (P)
				{
					if (P->Type == EUSType::Byte)
					{
						return FString::FromInt(*(WheelObj->Data + P->Offset));
					}
					P->ExportText(Out, WheelObj->Data + P->Offset, true);
				}
				return Out;
			};
			AUE2NetVehicle::FNetWheel Wheel;
			const FString Bone = Text(TEXT("BoneName"));
			Wheel.Bone = Bone.IsEmpty() || Bone == TEXT("None") ? FName() : FName(*Bone);
			Wheel.RadiusUU = FCString::Atof(*Text(TEXT("WheelRadius")));
			Wheel.SteerType = uint8(FCString::Atoi(*Text(TEXT("SteerType"))));
			if (!Wheel.Bone.IsNone())
			{
				Wheels.Add(Wheel);
			}
		}
	}
	if (Wheels.Num() > 0)
	{
		V->SetWheels(Wheels);
	}
	E.Native = V;
	E.bOwned = true;
	return V;
}

void FUE2NetWorld::LinkSeat(FEntity& E, const FUE2NetActor& A)
{
	AUE2NetPawn* Pawn = PawnForChannel(E.PawnChannel);
	const bool bTurretSeat = E.Kind == EKind::WeaponPawn;
	AUE2NetVehicle* V = bTurretSeat && !E.bOwned ? VehicleForChannel(E.BaseChannel) : Cast<AUE2NetVehicle>(E.Native.Get());
	if (!Pawn || !V)
	{
		return;
	}
	if (Pawn->GetNetVehicle() != V)
	{
		Pawn->SetNetVehicle(V);
	}
	// Vehicle.bDrawDriverInTP: the driver / gunner is drawn in the seat (DrivePos / DriveRot; a gunner on its gun)
	static TSet<FUSClass*> Logged;
	if (!Logged.Contains(A.Class))
	{
		Logged.Add(A.Class);
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: seat of %s: bDrawDriverInTP %d, DrivePos %s, DriveRot %s, on gun %d"), *E.ClassName, GetBool(A, TEXT("bDrawDriverInTP")),
			*DefaultVector(A.Class, TEXT("DrivePos")).ToCompactString(), *DefaultRotator(A.Class, TEXT("DriveRot")).ToCompactString(), bTurretSeat ? GetRef(A, TEXT("Gun")) : 0);
	}
	V->SetSeat(E.Channel, Pawn, GetBool(A, TEXT("bDrawDriverInTP")), DefaultVector(A.Class, TEXT("DrivePos")), DefaultRotator(A.Class, TEXT("DriveRot")),
		bTurretSeat ? GetRef(A, TEXT("Gun")) : 0);
}

void FUE2NetWorld::ApplyVehicle(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	using namespace UE2NetWorldLocal;
	AUE2NetVehicle* V = Cast<AUE2NetVehicle>(E.Native.Get());
	if (!V)
	{
		V = SpawnVehicle(E, A);
		if (!V)
		{
			return;
		}
		bInitial = true;
	}
	auto Changes = [&](const TCHAR* Key) { return bInitial || Changed.Contains(Key); };
	if (Changes(TEXT("Team")))
	{
		const int32 TeamRef = GetRef(A, TEXT("Team"));
		V->SetTeam(TeamRef ? TeamIndexOf(TeamRef) : GetInt(A, TEXT("Team"), ArenaTeam::None));
	}
	if (Changes(TEXT("bDriving")))
	{
		V->SetDriving(GetBool(A, TEXT("bDriving")));
	}
	if (Changes(TEXT("bHidden")))
	{
		// a stationary turret (ONSStationaryWeaponPawn) is hidden behind its gun until destroyed: then its dead base shows
		V->SetBodyHidden(GetBool(A, TEXT("bHidden")));
	}
	if (Changes(TEXT("AmbientSound")) || Changes(TEXT("SoundVolume")))
	{
		// the engine loop (Actor.AmbientSound; SoundVolume 0..255, 128 = normal)
		FString Path = GetObjPath(A, TEXT("AmbientSound"));
		if (!Find(A, TEXT("AmbientSound")))
		{
			Path = DefaultText(A.Class, TEXT("AmbientSound"));
		}
		V->SetAmbientSound(Path.Equals(TEXT("None")) ? nullptr : LoadSound(Path), GetInt(A, TEXT("SoundVolume"), 128) / 160.f);
	}
	// Vehicle.Driver (for a stationary turret: its gunner)
	if (Changes(TEXT("Driver")))
	{
		const int32 Driver = GetRef(A, TEXT("Driver"));
		if (E.PawnChannel && E.PawnChannel != Driver)
		{
			if (AUE2NetPawn* Old = PawnForChannel(E.PawnChannel); Old && Old->GetNetVehicle() == V)
			{
				Old->SetNetVehicle(nullptr);
			}
			V->SetSeat(E.Channel, nullptr, false, FVector::ZeroVector, FRotator::ZeroRotator, 0);
		}
		E.PawnChannel = Driver;
		LinkSeat(E, A);
	}
	// destroyed: ONSVehicle.bDestroyAppearance (the wreck, still moved by its rigid body), bDisintegrateVehicle
	if (Changes(TEXT("bDestroyAppearance")) || Changes(TEXT("bVehicleDestroyed")) || Changes(TEXT("bDisintegrateVehicle")))
	{
		const bool bDisintegrate = GetBool(A, TEXT("bDisintegrateVehicle"));
		if (GetBool(A, TEXT("bDestroyAppearance")) || bDisintegrate)
		{
			V->SetDestroyed(true, bDisintegrate);
		}
	}
	if (Changed.Contains(TEXT("ExplosionCount")))
	{
		const int32 Count = GetInt(A, TEXT("ExplosionCount"));
		if (E.LastExplosionCount != INDEX_NONE && Count != E.LastExplosionCount)
		{
			V->NetExplosion();
		}
		E.LastExplosionCount = Count;
	}
	// the KRigidBody state struct (HoverState, CarState, CopState, TreadState ...: ChassisPosition, ChassisQuaternion,
	// ChassisLinVel). ONSVehicle packs ChassisLinVel as 10 x the velocity (KUpdateState unpacks it with 0.1): checked
	// against successive positions in captures (the ratio is 0.08 .. 0.11)
	TArray<FString> Keys;
	if (bInitial)
	{
		A.Properties.GetKeys(Keys);
	}
	for (const FString& Key : bInitial ? Keys : Changed)
	{
		const FUE2NetValue* St = Find(A, *Key);
		if (!St || St->Kind != FUE2NetValue::EKind::Struct)
		{
			continue;
		}
		const FUE2NetValue* Pos = nullptr;
		const FUE2NetValue* Quat = nullptr;
		const FUE2NetValue* LinVel = nullptr;
		for (const TPair<FName, FUE2NetValue>& M : St->Members)
		{
			if (M.Key == TEXT("ChassisPosition")) Pos = &M.Value;
			else if (M.Key == TEXT("ChassisQuaternion")) Quat = &M.Value;
			else if (M.Key == TEXT("ChassisLinVel")) LinVel = &M.Value;
		}
		if (Pos && Quat && Quat->Members.Num() == 4)
		{
			const FQuat Q(Quat->Members[0].Value.F, Quat->Members[1].Value.F, Quat->Members[2].Value.F, Quat->Members[3].Value.F);
			V->SetNetRigidBody(ToVec(Pos->V), Q.GetNormalized(), LinVel ? ToVec(LinVel->V) * 0.1f : FVector::ZeroVector);
			return;
		}
	}
	if (!bInitial && (Changed.Contains(TEXT("Location")) || Changed.Contains(TEXT("Velocity")) || Changed.Contains(TEXT("Rotation"))))
	{
		const FUE2NetValue* R = Find(A, TEXT("Rotation"));
		const FRotator Rot = R ? FRotator(R->R.X * 360.f / 65536.f, R->R.Y * 360.f / 65536.f, R->R.Z * 360.f / 65536.f) : V->GetActorRotation();
		V->SetNetLocation(GetVec(A, TEXT("Location"), V->GetActorLocation() / S), GetVec(A, TEXT("Velocity")), Rot, false);
	}
}

void FUE2NetWorld::ApplyWeaponPawn(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	// ONSWeaponPawn: a gunner seat of VehicleBase; without a base it is a stationary turret (a vehicle of its own)
	const int32 Base = GetRef(A, TEXT("VehicleBase"));
	if (E.bOwned || (!Base && Has(E, TEXT("onsstationaryweaponpawn"))))
	{
		E.BaseChannel = 0;
		ApplyVehicle(E, A, Changed, bInitial);
		return;
	}
	if (Base)
	{
		E.BaseChannel = Base;
	}
	if (bInitial || Changed.Contains(TEXT("Driver")) || Changed.Contains(TEXT("VehicleBase")) || Changed.Contains(TEXT("Gun")))
	{
		const int32 Driver = GetRef(A, TEXT("Driver"));
		if (E.PawnChannel && E.PawnChannel != Driver)
		{
			if (AUE2NetPawn* Old = PawnForChannel(E.PawnChannel); Old && Old->GetNetVehicle())
			{
				Old->SetNetVehicle(nullptr);
			}
			if (AUE2NetVehicle* V = VehicleForChannel(E.BaseChannel))
			{
				V->SetSeat(E.Channel, nullptr, false, FVector::ZeroVector, FRotator::ZeroRotator, 0);
			}
		}
		E.PawnChannel = Driver;
		LinkSeat(E, A);
	}
}

void FUE2NetWorld::ApplyVehicleWeapon(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	using namespace UE2NetWorldLocal;
	// ONSWeapon: attached to its vehicle (Base); a stationary turret's gun belongs to the weapon pawn naming it
	if (const int32 Base = GetRef(A, TEXT("Base")))
	{
		E.BaseChannel = Base;
	}
	if (!E.BaseChannel)
	{
		const TMap<int32, FUE2NetActor>& Actors = Connection->GetActors();
		for (const TPair<int32, FEntity>& P : Entities)
		{
			const FUE2NetActor* WP = P.Value.Kind == EKind::WeaponPawn ? Actors.Find(P.Key) : nullptr;
			if (WP && GetRef(*WP, TEXT("Gun")) == A.Channel)
			{
				E.BaseChannel = P.Key;
				break;
			}
		}
	}
	AUE2NetVehicle* V = Cast<AUE2NetVehicle>(E.Native.Get());
	if (!V)
	{
		V = VehicleForChannel(E.BaseChannel);
		if (!V)
		{
			return; // the vehicle opens later: Tick retries
		}
		E.Native = V;
		E.bOwned = false;
		bInitial = true;
	}
	if (!V->HasWeapon(A.Channel))
	{
		FString MeshPath;
		const FString MeshObj = DefaultText(A.Class, TEXT("Mesh"));
		FString Pkg, Rest;
		if (!MeshObj.Equals(TEXT("None")) && MeshObj.Split(TEXT("."), &Pkg, &Rest))
		{
			MeshPath = FString::Printf(TEXT("SkeletalMeshes/%s/%s"), *Pkg, *Rest.Replace(TEXT("."), TEXT("_")));
		}
		auto NameOf = [this, &A](const TCHAR* Key) { const FString N = GetStr(A, Key); return N.IsEmpty() || N == TEXT("None") ? FName() : FName(*N); };
		UE2NetModContent::EnsureConverted(MeshObj, TEXT("SkeletalMesh"));
		V->SetWeapon(A.Channel, MeshPath, NameOf(TEXT("AttachmentBone")), NameOf(TEXT("YawBone")), NameOf(TEXT("PitchBone")), NameOf(TEXT("WeaponFireAttachmentBone")),
			GetFloat(A, TEXT("WeaponFireOffset")), UE2NetModWeapons::FindLook(E.ClassPath));
		// (the attachments the mod's own client code already shows are left to it: Net/UE2NetScriptHUD)
		TArray<UE2NetModWeapons::FClientAttachment> StandIns = UE2NetModWeapons::FindClientAttachments(E.ClassPath);
		if (const FUE2NetScriptHUD* SH = FUE2NetScriptHUD::Get())
		{
			StandIns.RemoveAll([&](const UE2NetModWeapons::FClientAttachment& At) { return SH->HasClientAttachment(A.Channel, At.UE2Class); });
		}
		V->SetWeaponAttachments(A.Channel, StandIns);
		// ONSWeapon turning limits (UE2 rotation units): PitchUpLimit / PitchDownLimit, YawStartConstraint ..
		// YawEndConstraint when bLimitYaw
		auto Deg = [](float Units) { return FRotator::NormalizeAxis(Units * 360.f / 65536.f); };
		const float PitchUp = Deg(GetFloat(A, TEXT("PitchUpLimit"), 16384.f));
		const float PitchDown = Deg(GetFloat(A, TEXT("PitchDownLimit"), 49152.f));
		// a yaw range is given by distinct constraints (the Manta's guns: -45 .. 45)
		const bool bLimitYaw = GetBool(A, TEXT("bLimitYaw")) || FMath::Abs(GetFloat(A, TEXT("YawStartConstraint")) - GetFloat(A, TEXT("YawEndConstraint"))) > 1.f;
		V->SetWeaponLimits(A.Channel, PitchDown, PitchUp, bLimitYaw, Deg(GetFloat(A, TEXT("YawStartConstraint"))), Deg(GetFloat(A, TEXT("YawEndConstraint"))));
		static TSet<FUSClass*> LoggedLimits;
		if (!LoggedLimits.Contains(A.Class))
		{
			LoggedLimits.Add(A.Class);
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: weapon %s: pitch %.0f .. %.0f deg, yaw %s (%.0f .. %.0f), bones yaw %s pitch %s fire %s"), *E.ClassName, PitchDown, PitchUp,
				bLimitYaw ? TEXT("limited") : TEXT("free"), Deg(GetFloat(A, TEXT("YawStartConstraint"))), Deg(GetFloat(A, TEXT("YawEndConstraint"))), *GetStr(A, TEXT("YawBone")),
				*GetStr(A, TEXT("PitchBone")), *GetStr(A, TEXT("WeaponFireAttachmentBone")));
		}
		bInitial = true;
	}
	auto Changes = [&](const TCHAR* Key) { return bInitial || Changed.Contains(Key); };
	if (Changes(TEXT("bHidden")))
	{
		V->SetWeaponHidden(A.Channel, GetBool(A, TEXT("bHidden")));
	}
	const FUE2NetValue* Hit = Find(A, TEXT("CurrentHitLocation"));
	if (Hit && Changes(TEXT("CurrentHitLocation")))
	{
		V->SetWeaponAim(A.Channel, ToVec(Hit->V));
	}
	if (Changes(TEXT("FlashCount")))
	{
		const int32 Flash = GetInt(A, TEXT("FlashCount"));
		if (E.LastFlashCount != INDEX_NONE && Flash != E.LastFlashCount && Flash != 0)
		{
			V->WeaponFired(A.Channel, Hit ? ToVec(Hit->V) : FVector::ZeroVector, GetBool(A, TEXT("bInstantFire")), Hit != nullptr);
		}
		E.LastFlashCount = Flash;
	}
	if (Changes(TEXT("HitCount")))
	{
		const int32 Count = GetInt(A, TEXT("HitCount"));
		if (E.LastHitCount != INDEX_NONE && Count != E.LastHitCount)
		{
			V->WeaponHit(A.Channel, GetVec(A, TEXT("LastHitLocation")));
		}
		E.LastHitCount = Count;
	}
}

// =================================================================================================
// Game objectives (ONS power nodes, Assault objectives), the bombing run ball
// =================================================================================================

FString FUE2NetWorld::MapActorName(const FUE2NetActor& A) const
{
	if (!A.bStatic)
	{
		return FString();
	}
	FString Path = Connection->GetPackageMap().GetObjectPath(A.Archetype);
	int32 Dot = INDEX_NONE;
	if (Path.FindLastChar(TEXT('.'), Dot))
	{
		Path.RightChopInline(Dot + 1);
	}
	return Path;
}

void FUE2NetWorld::ApplyObjective(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	if (Has(E, TEXT("onspowercore")))
	{
		ApplyPowerNode(E, A, Changed, bInitial);
	}
	else
	{
		ApplyAssaultObjective(E, A, Changed, bInitial);
	}
}

void FUE2NetWorld::ApplyPowerNode(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	UWorld* W = World.Get();
	AArenaONS_PowerNode* Node = Cast<AArenaONS_PowerNode>(E.Native.Get());
	if (!Node)
	{
		// our node / core made from the same map actor (UArenaRules_ONS records its name)
		const FString Name = MapActorName(A);
		for (TActorIterator<AArenaONS_PowerNode> It(W); It && !Name.IsEmpty(); ++It)
		{
			if (It->UE2SourceName.Equals(Name, ESearchCase::IgnoreCase))
			{
				Node = *It;
				break;
			}
		}
		if (!Node)
		{
			return;
		}
		E.Native = Node;
		++NodesMatched;
		bInitial = true;
	}
	if (!bInitial && Changed.Num() == 0)
	{
		return;
	}
	// ONSPowerCore: CoreStage 0 active, 2 constructing, 1 / 4 / others destroyed / neutral; DefenderTeamIndex the
	// owner (2 / 255 none); Health of DamageCapacity (construction counts it up); bPoweredByRed / Blue: linked to that
	// team's network (it can attack or build it); bUnderAttack
	const int32 Stage = GetInt(A, TEXT("CoreStage"), 4);
	const int32 Defender = GetInt(A, TEXT("DefenderTeamIndex"), 255);
	const int32 Team = Defender == 0 || Defender == 1 ? Defender : ArenaTeam::None;
	const float Health = float(GetInt(A, TEXT("Health")));
	const float MaxHealth = GetFloat(A, TEXT("DamageCapacity"), Node->GetMaxHealth());
	EArenaONS_NodeState State = EArenaONS_NodeState::Neutral;
	if (Node->IsCore())
	{
		State = EArenaONS_NodeState::Active;
	}
	else if (Stage == 0 && Team != ArenaTeam::None)
	{
		State = EArenaONS_NodeState::Active;
	}
	else if (Stage == 2 && Team != ArenaTeam::None)
	{
		State = EArenaONS_NodeState::Constructing;
	}
	const bool bDamaged = GetBool(A, TEXT("bUnderAttack")) || (E.LastHealth != INDEX_NONE && Health < float(E.LastHealth) && State == EArenaONS_NodeState::Active);
	const EArenaONS_NodeState OldState = Node->GetState();
	const int32 OldTeam = Node->GetTeam();
	Node->SetNetState(State, Node->IsCore() && Team == ArenaTeam::None ? Node->GetTeam() : Team, Health, MaxHealth, bDamaged);
	E.LastHealth = int32(Health);
	const bool bRed = GetBool(A, TEXT("bPoweredByRed"));
	const bool bBlue = GetBool(A, TEXT("bPoweredByBlue"));
	const bool bNeutral = State == EArenaONS_NodeState::Neutral;
	Node->SetNetworkFlags(bRed && !(Team == 0 && !bNeutral), bBlue && !(Team == 1 && !bNeutral), bRed && bNeutral, bBlue && bNeutral);
	++NodeUpdates;
	if (OldState != Node->GetState() || OldTeam != Node->GetTeam() || bInitial)
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: power %s %s: stage %d team %d health %.0f / %.0f -> %s"), Node->IsCore() ? TEXT("core") : TEXT("node"),
			*Node->UE2SourceName, Stage, Defender, Health, MaxHealth, *UEnum::GetValueAsString(Node->GetState()));
	}
}

void FUE2NetWorld::ApplyAssaultObjective(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	UWorld* W = World.Get();
	AArenaAS_Objective* Obj = Cast<AArenaAS_Objective>(E.Native.Get());
	if (!Obj)
	{
		const FString Name = MapActorName(A);
		for (TActorIterator<AArenaAS_Objective> It(W); It && !Name.IsEmpty(); ++It)
		{
			if (It->UE2SourceName.Equals(Name, ESearchCase::IgnoreCase))
			{
				Obj = *It;
				break;
			}
		}
		if (!Obj)
		{
			return; // not one of ours (a bombing run goal ...)
		}
		E.Native = Obj;
		++ObjectivesMatched;
		bInitial = true;
	}
	if (!bInitial && Changed.Num() == 0)
	{
		return;
	}
	// GameObjective: bActive, bDisabled (completed), DefenderTeamIndex, bIsCritical; DestroyableObjective: Health of
	// DamageCapacity, bIsUnderAttack; HoldObjective: bIsHeld (an attacker is working on it; its progress itself is not
	// replicated)
	FNetObjective& N = NetObjectives.FindOrAdd(A.Channel);
	N.Obj = Obj;
	N.bDisabled = GetBool(A, TEXT("bDisabled"));
	N.bActive = GetBool(A, TEXT("bActive"), true);
	N.bDestroyable = Has(E, TEXT("destroyableobjective"));
	// the map's DamageCapacity is not replicated (the class default is, 100 for most): the objective's full health is the
	// highest the server reported (it starts full) or our map-derived one
	const bool bGotHealth = Find(A, TEXT("Health")) != nullptr;
	N.Health = N.bDestroyable && bGotHealth ? float(GetInt(A, TEXT("Health"))) : 0.f;
	N.MaxHealth = N.bDestroyable ? FMath::Max3(N.MaxHealth, N.Health, bGotHealth && N.MaxHealth > 0.f ? 0.f : Obj->GetMaxHealth()) : 0.f;
	if (N.bDestroyable && !bGotHealth)
	{
		N.Health = N.MaxHealth; // not sent: still the map's full health
	}
	N.bWorked = GetBool(A, TEXT("bIsCritical")) || GetBool(A, TEXT("bIsUnderAttack"));
	const bool bHeld = GetBool(A, TEXT("bIsHeld"));
	if (bHeld && N.HeldSince <= 0.0)
	{
		N.HeldSince = FPlatformTime::Seconds();
	}
	else if (!bHeld)
	{
		N.HeldSince = 0.0;
	}
	N.Defender = GetInt(A, TEXT("DefenderTeamIndex"), -1);
	++ObjectiveUpdates;
	RefreshAssaultObjectives();
}

void FUE2NetWorld::RefreshAssaultObjectives()
{
	// UT2004 works on the objectives of the highest DefensePriority that are active and not disabled (several at once
	// when they share it); ours are staged by that priority (UArenaRules_AS), so the current ones are the active
	// required objectives of the lowest stage, plus active bonus objectives
	int32 MinStage = MAX_int32;
	for (const TPair<int32, FNetObjective>& P : NetObjectives)
	{
		const AArenaAS_Objective* Obj = P.Value.Obj.Get();
		if (Obj && P.Value.bActive && !P.Value.bDisabled && !Obj->IsOptional() && Obj->GetStage() >= 0)
		{
			MinStage = FMath::Min(MinStage, Obj->GetStage());
		}
	}
	const double Now = FPlatformTime::Seconds();
	for (TPair<int32, FNetObjective>& P : NetObjectives)
	{
		FNetObjective& N = P.Value;
		AArenaAS_Objective* Obj = N.Obj.Get();
		if (!Obj)
		{
			continue;
		}
		const bool bCurrent = N.bActive && !N.bDisabled && (Obj->IsOptional() || Obj->GetStage() < 0 || MinStage == MAX_int32 || Obj->GetStage() == MinStage);
		// hold / use progress: counted locally while the server says it is held, over the objective's time
		float Progress = N.bDisabled ? 1.f : 0.f;
		if (!N.bDisabled && N.HeldSince > 0.0)
		{
			Progress = FMath::Clamp(float(Now - N.HeldSince) / FMath::Max(1.f, Obj->GetDuration()), 0.f, 0.99f);
		}
		const bool bWasActive = Obj->IsActive();
		const bool bWasCompleted = Obj->IsCompleted();
		Obj->SetNetState(bCurrent, N.bDisabled, N.Health, N.MaxHealth, Progress, N.bWorked || N.HeldSince > 0.0, N.Defender);
		if (!N.bLogged || bWasActive != Obj->IsActive() || bWasCompleted != Obj->IsCompleted())
		{
			N.bLogged = true;
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: objective %s (%s, stage %d): %s%s, health %.0f / %.0f"), *Obj->UE2SourceName, *Obj->GetDescription(), Obj->GetStage(),
				Obj->IsCompleted() ? TEXT("completed") : (Obj->IsActive() ? TEXT("current") : (N.bActive ? TEXT("waiting") : TEXT("inactive"))),
				N.HeldSince > 0.0 ? TEXT(", held") : (N.bWorked ? TEXT(", under attack") : TEXT("")), N.Health, N.MaxHealth);
		}
	}
}

void FUE2NetWorld::ApplyGameObject(FEntity& E, const FUE2NetActor& A, const TArray<FString>& Changed, bool bInitial)
{
	using namespace UE2NetWorldLocal;
	UWorld* W = World.Get();
	AArenaBR_Ball* Ball = Cast<AArenaBR_Ball>(E.Native.Get());
	if (!Ball && Has(E, TEXT("xbombflag")))
	{
		TActorIterator<AArenaBR_Ball> It(W);
		Ball = It ? *It : nullptr;
		if (Ball)
		{
			E.Native = Ball;
			E.bOwned = false;
			bInitial = true;
		}
	}
	if (!Ball)
	{
		ApplyPlaceholder(E, A, Changed, bInitial); // no native object for it: its mesh at the replicated location
		return;
	}
	// GameObject: bHeld + HolderPRI (the holder's pawn may not be relevant: then where the server says), bHome;
	// thrown: falling with a velocity; else resting where it dropped
	const bool bHeld = GetBool(A, TEXT("bHeld"));
	const int32 HolderPRI = GetRef(A, TEXT("HolderPRI"));
	AArenaCharacter* Holder = bHeld ? (HolderPRI ? CharacterOfPRI(HolderPRI) : nullptr) : nullptr;
	if (bHeld && !Holder)
	{
		Holder = PawnForChannel(GetRef(A, TEXT("Base")));
	}
	if (!bInitial && Changed.Num() == 0 && !(Holder && Ball->GetCarrier() != Holder))
	{
		return; // nothing new (the flight is simulated by the ball between updates)
	}
	const FVector Loc = GetVec(A, TEXT("Location"), ToVec(A.SpawnLocation)) * S;
	const FVector Vel = GetVec(A, TEXT("Velocity")) * S;
	const EArenaCarryState Old = Ball->GetCarryState();
	if (Holder)
	{
		Ball->SetNetState(EArenaCarryState::Carried, Holder, FVector::ZeroVector, FVector::ZeroVector);
	}
	else if (GetBool(A, TEXT("bHome"), true))
	{
		Ball->SetNetState(EArenaCarryState::Home, nullptr, FVector::ZeroVector, FVector::ZeroVector);
	}
	else if (GetInt(A, TEXT("Physics")) == 2 && Vel.SizeSquared() > FMath::Square(60.f))
	{
		Ball->SetNetState(EArenaCarryState::InFlight, nullptr, Loc, Vel);
	}
	else if (!Loc.IsNearlyZero())
	{
		Ball->SetNetState(EArenaCarryState::Dropped, nullptr, Loc - FVector(0.f, 0.f, GetFloat(A, TEXT("CollisionHeight"), 20.f) * S), FVector::ZeroVector);
	}
	// its look as the server has it: loose, its own StaticMesh (E_Pickups.BombBall.FullBomb) at its DrawScale; carried,
	// our ball model on the carrier (UE2 draws BombEffectMesh on the holder's spine)
	if (Holder)
	{
		Ball->SetNetMesh(nullptr, 1.f);
	}
	else
	{
		FString MeshPath = GetObjPath(A, TEXT("StaticMesh"));
		if (MeshPath.IsEmpty() && !Find(A, TEXT("StaticMesh")))
		{
			MeshPath = DefaultText(A.Class, TEXT("StaticMesh"));
		}
		UStaticMesh* Loose = MeshPath.IsEmpty() ? nullptr : ArenaRuntimeContent::LoadStaticMeshByObjectPath(MeshPath);
		Ball->SetNetMesh(Loose, GetFloat(A, TEXT("DrawScale"), 1.f));
		static bool bLogged = false;
		if (!bLogged)
		{
			bLogged = true;
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: loose ball mesh %s (%s), draw scale %.2f"), *MeshPath, Loose ? TEXT("loaded") : TEXT("missing"), GetFloat(A, TEXT("DrawScale"), 1.f));
		}
	}
	++BallUpdates;
	if (Old != Ball->GetCarryState())
	{
		static const TCHAR* Names[] = { TEXT("home"), TEXT("carried"), TEXT("dropped"), TEXT("thrown") };
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: ball %s%s"), Names[FMath::Clamp(int32(Ball->GetCarryState()), 0, 3)],
			Holder && Holder->GetPlayerState() ? *(FString(TEXT(" by ")) + Holder->GetPlayerState()->GetPlayerName()) : TEXT(""));
	}
}

bool FUE2NetWorld::ShowScriptedMessage(const FString& ClassPath, FUSClass* Class, int32 Switch, int32 PRI1, int32 PRI2, const FUE2NetValue* Optional)
{
	AArenaPlayerController* PC = GetPC();
	FUSObject* Def = Class ? Class->GetDefaultObject() : nullptr;
	if (!PC || !Def)
	{
		return false;
	}
	// script stand-ins for the related PRIs / team (transient objects of the server's classes with the names set)
	auto SetStr = [](FUSObject* O, const TCHAR* Key, const FString& V)
	{
		FUSProperty* P = O ? O->FindProperty(FName(Key)) : nullptr;
		if (P && P->Type == EUSType::Str && O->Data)
		{
			*reinterpret_cast<FString*>(O->Data + P->Offset) = V;
		}
	};
	auto SetNum = [](FUSObject* O, const TCHAR* Key, int32 V)
	{
		FUSProperty* P = O ? O->FindProperty(FName(Key)) : nullptr;
		if (P && O->Data && P->Type == EUSType::Int)
		{
			*reinterpret_cast<int32*>(O->Data + P->Offset) = V;
		}
		else if (P && O->Data && P->Type == EUSType::Byte)
		{
			*(O->Data + P->Offset) = uint8(V);
		}
	};
	auto SetObj = [](FUSObject* O, const TCHAR* Key, FUSObject* V)
	{
		FUSProperty* P = O ? O->FindProperty(FName(Key)) : nullptr;
		if (P && O->Data && P->Type == EUSType::Object)
		{
			*reinterpret_cast<FUSObject**>(O->Data + P->Offset) = V;
		}
	};
	static TMap<int32, FUSObject*> ScriptObjects; // per channel (kept: the transient script world owns them)
	TFunction<FUSObject*(int32)> ScriptActor = [&](int32 Channel) -> FUSObject*
	{
		FEntity* E = FindEntity(Channel);
		const FUE2NetActor* A = Channel ? Connection->GetActors().Find(Channel) : nullptr;
		if (!E || !A || !E->Class)
		{
			return nullptr;
		}
		FUSObject*& O = ScriptObjects.FindOrAdd(Channel);
		if (!O || O->Class != E->Class)
		{
			O = FUSWorld::Transient().ConstructObject(E->Class);
		}
		if (E->Kind == EKind::PRI)
		{
			const AArenaPlayerState* PS = PSForChannel(Channel);
			SetStr(O, TEXT("PlayerName"), PS ? PS->GetPlayerName() : GetStr(*A, TEXT("PlayerName")));
			SetNum(O, TEXT("PlayerID"), GetInt(*A, TEXT("PlayerID")));
			const int32 TeamChannel = GetRef(*A, TEXT("Team"));
			SetObj(O, TEXT("Team"), TeamChannel && TeamChannel != Channel ? ScriptActor(TeamChannel) : nullptr);
		}
		else if (E->Kind == EKind::Team)
		{
			SetNum(O, TEXT("TeamIndex"), GetInt(*A, TEXT("TeamIndex")));
		}
		return O;
	};
	FUSObject* Related1 = ScriptActor(PRI1);
	FUSObject* Related2 = ScriptActor(PRI2);
	FUSObject* OptObj = nullptr;
	if (Optional && Optional->Obj.IsDynamic())
	{
		OptObj = ScriptActor(Optional->Obj.Channel);
	}
	else if (Optional && Optional->Obj.IsStatic())
	{
		const FString Path = ObjPath(*Optional);
		OptObj = Path.IsEmpty() ? nullptr : FUSRuntime::Get().FindObject(Path);
	}
	FString Text;
	if (FUSFunction* F = USVM::FindFunction(Def, FName(TEXT("GetString"))))
	{
		FUSParms Parms(F);
		Parms.Int(Switch).Object(Related1).Object(Related2).Object(OptObj);
		Parms.Call(Def);
		Text = Parms.ReturnStr();
	}
	Text.TrimStartAndEndInline();
	if (Text.IsEmpty())
	{
		return false;
	}
	// the class's look: LocalMessage defaults, GetColor / GetFontSize / GetLifeTime per switch where it has them
	ArenaLocalMessages::FStyle Style = {};
	Style.SourceClass = nullptr;
	const FString Color = DefaultText(Class, TEXT("DrawColor"));
	int32 R = 255, G = 255, B = 255, Alpha = 255;
	FParse::Value(*Color, TEXT("R="), R);
	FParse::Value(*Color, TEXT("G="), G);
	FParse::Value(*Color, TEXT("B="), B);
	FParse::Value(*Color, TEXT("A="), Alpha);
	Style.DrawColor = FColor(uint8(R), uint8(G), uint8(B), uint8(Alpha));
	if (FUSFunction* F = USVM::FindFunction(Def, FName(TEXT("GetColor"))))
	{
		FUSParms Parms(F);
		Parms.Int(Switch).Object(Related1).Object(Related2);
		Parms.Call(Def);
		if (const uint8* C = Parms.ReturnAddr())
		{
			Style.DrawColor = FColor(C[2], C[1], C[0], C[3]); // UE2 Color: B, G, R, A
		}
	}
	int32 FontSize = FCString::Atoi(*DefaultText(Class, TEXT("FontSize")));
	if (FUSFunction* F = USVM::FindFunction(Def, FName(TEXT("GetFontSize"))))
	{
		FUSParms Parms(F);
		Parms.Int(Switch).Object(Related1).Object(Related2).Object(nullptr);
		Parms.Call(Def);
		FontSize = Parms.ReturnInt();
	}
	Style.FontSize = FontSize;
	Style.PosX = DefaultText(Class, TEXT("PosX")).IsEmpty() ? 0.5f : FCString::Atof(*DefaultText(Class, TEXT("PosX")));
	Style.PosY = DefaultText(Class, TEXT("PosY")).IsEmpty() ? 0.7f : FCString::Atof(*DefaultText(Class, TEXT("PosY")));
	Style.DrawPivot = uint8(FCString::Atoi(*DefaultText(Class, TEXT("DrawPivot"))));
	Style.StackMode = ArenaLocalMessages::EStackMode(FMath::Clamp(FCString::Atoi(*DefaultText(Class, TEXT("StackMode"))), 0, 2));
	Style.Lifetime = FMath::Max(1.f, FCString::Atof(*DefaultText(Class, TEXT("Lifetime"))));
	if (FUSFunction* F = USVM::FindFunction(Def, FName(TEXT("GetLifeTime"))))
	{
		FUSParms Parms(F);
		Parms.Int(Switch);
		Parms.Call(Def);
		// LocalMessage.GetLifeTime returns a float (an int return in a mod's override is read as one too)
		const FUSProperty* Ret = F->ReturnProperty;
		Style.Lifetime = FMath::Clamp(Ret && Ret->Type == EUSType::Int ? float(Parms.ReturnInt()) : Parms.ReturnFloat(), 1.f, 30.f);
	}
	auto DefBool = [&](const TCHAR* Key) { return DefaultText(Class, Key).Equals(TEXT("True"), ESearchCase::IgnoreCase); };
	Style.bFadeMessage = DefBool(TEXT("bFadeMessage"));
	Style.bIsUnique = DefBool(TEXT("bIsUnique"));
	Style.bIsSpecial = DefBool(TEXT("bIsSpecial"));
	const bool bConsole = DefBool(TEXT("bIsConsoleMessage"));
	// HUD.LocalizedMessage: bIsSpecial ones are drawn as local messages, the others go to the console lines
	if (Style.bIsSpecial)
	{
		PC->AddCustomLocalMessage(Text, Style, FontSize, FName(*ClassPath));
	}
	if (!Style.bIsSpecial || bConsole)
	{
		PC->AddConsoleLine(Text, Style.bIsSpecial ? FColor(255, 255, 255) : Style.DrawColor);
	}
	++ScriptedMessages;
	int32& Count = ScriptedMessageClasses.FindOrAdd(ClassPath);
	if (Count++ < 3)
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: message %s switch %d (script GetString%s): %s [colour %s, font %d, pos %.2f %.2f, life %.1f, unique %d, fade %d, stack %d]"),
			*ClassPath, Switch, Style.bIsSpecial ? TEXT(", local message") : TEXT(", console"), *Text, *Style.DrawColor.ToString(), FontSize, Style.PosX, Style.PosY,
			Style.Lifetime, Style.bIsUnique, Style.bFadeMessage, int32(Style.StackMode));
	}
	// the class's announcement for this switch (ONSOnslaughtMessage and others: AnnouncerSounds[Switch] names or sounds)
	const FString Announce = DefaultText(Class, *FString::Printf(TEXT("AnnouncerSounds[%d]"), Switch));
	if (!Announce.IsEmpty() && !Announce.Equals(TEXT("None")))
	{
		if (USoundBase* Snd = Announce.Contains(TEXT(".")) ? UE2NetWorldLocal::LoadSound(Announce) : nullptr)
		{
			UGameplayStatics::PlaySound2D(PC, Snd);
		}
		else
		{
			PlayAnnouncer(Announce);
		}
	}
	return true;
}

// =================================================================================================
// Function calls on our PlayerController
// =================================================================================================

void FUE2NetWorld::FunctionCall(const FUE2NetActor& A, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params)
{
	if (!Function || A.Channel != Connection->FindPlayerControllerChannel())
	{
		return;
	}
	auto Param = [&Params](const TCHAR* Name) -> const FUE2NetValue*
	{
		for (const TPair<FName, FUE2NetValue>& P : Params)
		{
			if (P.Key == Name)
			{
				return &P.Value;
			}
		}
		return nullptr;
	};
	const FString Name = Function->Name.ToString();
	AArenaPlayerController* PC = GetPC();
	static bool bLoggedNoPC = false;
	if (!PC && !bLoggedNoPC)
	{
		bLoggedNoPC = true;
		UWorld* W = World.Get();
		UE_LOG(LogUE2Net, Warning, TEXT("UE2NetWorld: no local player controller for %s (world %s, first PC %s)"), *Name, W ? *W->GetName() : TEXT("none"),
			W && W->GetFirstPlayerController() ? *W->GetFirstPlayerController()->GetClass()->GetName() : TEXT("none"));
	}
	// the server's script HUD shows its messages itself (ours then only plays their sounds): Net/UE2NetScriptHUD.h
	const bool bMessage = Name == TEXT("ReceiveLocalizedMessage") || Name == TEXT("ClientMessage") || Name == TEXT("TeamMessage");
	FUE2NetScriptHUD* ScriptHud = bMessage ? FUE2NetScriptHUD::Get() : nullptr;
	TGuardValue<int32> Hold(AArenaPlayerController::MessageDisplayHold,
		AArenaPlayerController::MessageDisplayHold + (ScriptHud && ScriptHud->FeedMessage(A.Channel, Name, Params) ? 1 : 0));
	if (Name == TEXT("ReceiveLocalizedMessage"))
	{
		ReceiveLocalizedMessage(Params);
	}
	else if (Name == TEXT("ClientSetViewTarget"))
	{
		if (const FUE2NetValue* T = Param(TEXT("A")))
		{
			SetViewTargetFromServer(*T);
		}
	}
	else if (Name == TEXT("ClientSetBehindView"))
	{
		if (const FUE2NetValue* B = Param(TEXT("B")))
		{
			AUE2NetPawn::bBehindView = B->I != 0;
		}
	}
	else if (Name == TEXT("ClientHearSound"))
	{
		HearSound(Params, false);
	}
	else if (Name == TEXT("ClientPlaySound"))
	{
		HearSound(Params, true);
	}
	else if (Name == TEXT("TeamMessage") || Name == TEXT("ClientMessage"))
	{
		const FUE2NetValue* Text = Param(TEXT("S"));
		if (PC && Text && !Text->S.IsEmpty())
		{
			FString Line = Text->S;
			FColor Color(255, 255, 255);
			if (Name == TEXT("TeamMessage"))
			{
				const FUE2NetValue* PRI = Param(TEXT("PRI"));
				const AArenaPlayerState* From = PRI && PRI->Obj.IsDynamic() ? PSForChannel(PRI->Obj.Channel) : nullptr;
				const FUE2NetValue* Type = Param(TEXT("Type"));
				const bool bTeam = Type && Type->N == TEXT("TeamSay");
				if (From)
				{
					Line = From->GetPlayerName() + TEXT(": ") + Line;
				}
				// UT2004 chat colours: team chat in the team colour, normal chat green-white
				Color = bTeam && From && (From->Team == 0 || From->Team == 1) ? ArenaTeam::Color(From->Team).ToFColor(true) : FColor(200, 255, 200);
			}
			PC->AddConsoleLine(Line, Color);
			++MessagesShown;
		}
	}
	else if (Name == TEXT("PlayStartupMessage"))
	{
		if (const FUE2NetValue* Stage = Param(TEXT("StartupStage")))
		{
			// stages 5 / 6 (the match has begun) are announced by our HUD when the GRI's match phase changes
			if (PC && Stage->I < 5)
			{
				PC->AddLocalMessage(EArenaLocalMessage::Startup, Stage->I);
				++MessagesShown;
			}
		}
	}
	else if (Name == TEXT("ClientGameEnded") || Name == TEXT("GameHasEnded"))
	{
		bGameEnded = true;
		if (UWorld* W = World.Get())
		{
			if (AArenaGameState* GS = W->GetGameState<AArenaGameState>())
			{
				GS->Phase = EArenaMatchPhase::Ended;
			}
		}
	}
	else if (Name == TEXT("ClientReset") || Name == TEXT("ClientRestart"))
	{
		bGameEnded = false;
	}
}

void FUE2NetWorld::ReceiveLocalizedMessage(const TArray<TPair<FName, FUE2NetValue>>& Params)
{
	AArenaPlayerController* PC = GetPC();
	if (!PC)
	{
		return;
	}
	const FUE2NetValue* Message = nullptr;
	int32 Switch = 0;
	int32 PRI1 = 0, PRI2 = 0;
	const FUE2NetValue* Optional = nullptr;
	for (const TPair<FName, FUE2NetValue>& P : Params)
	{
		if (P.Key == TEXT("Message")) Message = &P.Value;
		else if (P.Key == TEXT("Switch")) Switch = P.Value.I;
		else if (P.Key == TEXT("RelatedPRI_1")) PRI1 = P.Value.Obj.Channel;
		else if (P.Key == TEXT("RelatedPRI_2")) PRI2 = P.Value.Obj.Channel;
		else if (P.Key == TEXT("OptionalObject")) Optional = &P.Value;
	}
	const FString ClassPath = Message ? ObjPath(*Message) : FString();
	FString Class = ClassPath;
	int32 Dot = INDEX_NONE;
	if (Class.FindLastChar(TEXT('.'), Dot))
	{
		Class.RightChopInline(Dot + 1);
	}
	AArenaPlayerState* Name1 = PSForChannel(PRI1);
	AArenaPlayerState* Name2 = PSForChannel(PRI2);
	const AUE2NetPawn* Viewed = Cast<AUE2NetPawn>(PC->GetViewTarget());
	const bool bAboutViewed = Viewed && Name1 && Viewed->GetPlayerState() == Name1;
	++MessagesShown;
	if (Class == TEXT("xDeathMessage") || Class == TEXT("DeathMessage"))
	{
		// xDeathMessage.GetString: the damage type's DeathString (or the suicide text) with %k / %o
		FString DamPkg, DamClass;
		const FString DamPath = Optional ? ObjPath(*Optional) : FString();
		DamPath.Split(TEXT("."), &DamPkg, &DamClass);
		const bool bSuicide = !Name1 || Name1 == Name2;
		FString Text;
		if (bSuicide)
		{
			const FUE2NetActor* VictimPRI = PRI2 ? Connection->GetActors().Find(PRI2) : nullptr;
			const bool bFemale = VictimPRI && GetBool(*VictimPRI, TEXT("bIsFemale"));
			if (DamClass.IsEmpty() || !ArenaLocalMessages::Localize(DamPkg, DamClass, bFemale ? TEXT("FemaleSuicide") : TEXT("MaleSuicide"), Text))
			{
				Text = bFemale ? TEXT("%o killed herself.") : TEXT("%o killed himself.");
			}
		}
		else if (DamClass.IsEmpty() || !ArenaLocalMessages::Localize(DamPkg, DamClass, TEXT("DeathString"), Text))
		{
			Text = TEXT("%o was killed by %k.");
		}
		Text.ReplaceInline(TEXT("%k"), Name1 ? *Name1->GetPlayerName() : TEXT(""));
		Text.ReplaceInline(TEXT("%o"), Name2 ? *Name2->GetPlayerName() : TEXT("someone"));
		PC->AddConsoleLine(Text, FColor(0, 0, 0, 0));
		++KillLines;
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: kill: %s"), *Text);
		return;
	}
	struct FMap { const TCHAR* Class; EArenaLocalMessage Kind; };
	static const FMap Table[] = {
		{ TEXT("FirstBloodMessage"), EArenaLocalMessage::FirstBlood }, { TEXT("KillingSpreeMessage"), EArenaLocalMessage::KillingSpree },
		{ TEXT("MultiKillMessage"), EArenaLocalMessage::MultiKill }, { TEXT("SpecialKillMessage"), EArenaLocalMessage::SpecialKill },
		{ TEXT("xKillerMessagePlus"), EArenaLocalMessage::Killer }, { TEXT("xVictimMessage"), EArenaLocalMessage::Victim },
		{ TEXT("TimerMessage"), EArenaLocalMessage::Timer }, { TEXT("StartupMessage"), EArenaLocalMessage::Startup },
	};
	for (const FMap& M : Table)
	{
		if (Class == M.Class)
		{
			if (M.Kind == EArenaLocalMessage::Startup && Switch >= 5)
			{
				return; // announced by our HUD on the match phase change
			}
			PC->AddLocalMessage(M.Kind, Switch, Name1, Name2);
			// the reward announcer (UT2004 plays it for the player concerned; we also for the followed player)
			if (M.Kind == EArenaLocalMessage::FirstBlood)
			{
				PlayAnnouncer(TEXT("first_blood"));
			}
			else if (M.Kind == EArenaLocalMessage::KillingSpree && !Name2 && bAboutViewed)
			{
				static const TCHAR* Spree[] = { TEXT("killing_spree"), TEXT("rampage"), TEXT("dominating"), TEXT("unstoppable"), TEXT("godlike"), TEXT("wicked_sick") };
				PlayAnnouncer(Spree[FMath::Clamp(Switch, 0, 5)]);
			}
			else if (M.Kind == EArenaLocalMessage::MultiKill)
			{
				static const TCHAR* Multi[] = { TEXT("Double_Kill"), TEXT("MultiKill"), TEXT("MegaKill"), TEXT("UltraKill"), TEXT("MonsterKill"), TEXT("LudicrousKill"), TEXT("HolyShit") };
				PlayAnnouncer(Multi[FMath::Clamp(Switch - 1, 0, 6)]);
			}
			else if (M.Kind == EArenaLocalMessage::SpecialKill && Switch == 0)
			{
				PlayAnnouncer(TEXT("Headshot"));
			}
			return;
		}
	}
	if (Class == TEXT("CTFMessage") || Class == TEXT("xCTFMessage"))
	{
		// UnrealGame.CTFMessage: OptionalObject is the flag's TeamInfo. 0 captured, 1 returned (by a player),
		// 2 dropped, 3 / 5 returned (auto), 4 / 6 taken
		const int32 FlagTeam = Optional && Optional->Obj.IsDynamic() ? TeamIndexOf(Optional->Obj.Channel) : 0;
		const FString T = UE2NetWorldLocal::TeamWord(FlagTeam);
		FString Key, Fallback, Announce;
		bool bName = Name1 != nullptr;
		switch (Switch)
		{
		case 0: Key = TEXT("Capture") + T; Fallback = FString::Printf(TEXT(" captured the %s flag!"), *T.ToLower());
			Announce = FString(UE2NetWorldLocal::TeamWord(1 - FlagTeam)) + TEXT("_Team_Scores"); break;
		case 1: Key = TEXT("Return") + T; Fallback = FString::Printf(TEXT(" returned the %s flag!"), *T.ToLower()); Announce = T + TEXT("_Flag_Returned"); break;
		case 2: Key = TEXT("Dropped") + T; Fallback = FString::Printf(TEXT(" dropped the %s flag!"), *T.ToLower()); Announce = T + TEXT("_Flag_Dropped"); break;
		case 3: case 5: Key = TEXT("Returned") + T; Fallback = FString::Printf(TEXT("The %s flag was returned!"), *T.ToLower()); Announce = T + TEXT("_Flag_Returned"); bName = false; break;
		default: Key = TEXT("Has") + T; Fallback = FString::Printf(TEXT(" took the %s flag!"), *T.ToLower()); Announce = T + TEXT("_Flag_Taken"); break;
		}
		FString Text;
		if (!ArenaLocalMessages::Localize(TEXT("UnrealGame"), TEXT("CTFMessage"), Key, Text))
		{
			Text = Fallback;
		}
		if (bName)
		{
			Text = Name1->GetPlayerName() + TEXT(" ") + Text.TrimStart();
		}
		PC->AddCenterMessage(Text.TrimStartAndEnd(), ArenaTeam::Color(FlagTeam), 3.f);
		PlayAnnouncer(Announce);
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: CTF message %d: %s"), Switch, *Text);
		return;
	}
	// ONS, Assault and mod message classes: the class's own GetString through the UScript VM
	if (Message && Message->Obj.IsStatic() && ShowScriptedMessage(ClassPath, Connection->GetPackageMap().GetClass(Message->Obj), Switch, PRI1, PRI2, Optional))
	{
		return;
	}
	--MessagesShown;
	int32& Count = UnhandledMessages.FindOrAdd(ClassPath.IsEmpty() ? FString(TEXT("(none)")) : ClassPath);
	if (Count++ == 0)
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: local message class %s (switch %d) is not shown"), *ClassPath, Switch);
	}
}

void FUE2NetWorld::PlayAnnouncer(const FString& Name)
{
	AArenaPlayerController* PC = GetPC();
	if (!PC || Name.IsEmpty())
	{
		return;
	}
	for (const TCHAR* Pkg : { TEXT("AnnouncerMale2K4"), TEXT("AnnouncerFemale2K4") })
	{
		if (USoundBase* Sound = ArenaArt::LoadOneShotSound(FString::Printf(TEXT("Sounds/%s/Generic_%s"), Pkg, *Name)))
		{
			UGameplayStatics::PlaySound2D(PC, Sound);
			return;
		}
	}
}

void FUE2NetWorld::HearSound(const TArray<TPair<FName, FUE2NetValue>>& Params, bool b2D)
{
	using namespace UE2NetWorldLocal;
	UWorld* W = World.Get();
	if (!W)
	{
		return;
	}
	const FUE2NetValue* Sound = nullptr;
	int32 SourceChannel = 0;
	int32 Id = 0;
	FVector Location = FVector::ZeroVector;
	FVector Parameters(1000.f, 300.f, 1000.f);
	for (const TPair<FName, FUE2NetValue>& P : Params)
	{
		if (P.Key == TEXT("S") || P.Key == TEXT("ASound")) Sound = &P.Value;
		else if (P.Key == TEXT("Actor")) SourceChannel = P.Value.Obj.Channel;
		else if (P.Key == TEXT("Id")) Id = P.Value.I;
		else if (P.Key == TEXT("SoundLocation")) Location = ToVec(P.Value.V) * S;
		else if (P.Key == TEXT("Parameters")) Parameters = ToVec(P.Value.V);
	}
	const double Now = FPlatformTime::Seconds();
	if (Id != 0)
	{
		// the server sends some sounds twice (once per slot)
		if (const double* Last = RecentSoundIds.Find(Id); Last && Now - *Last < 0.25)
		{
			return;
		}
		RecentSoundIds.Add(Id, Now);
	}
	USoundBase* S2 = Sound ? LoadSound(ObjPath(*Sound)) : nullptr;
	if (!S2)
	{
		return;
	}
	// Parameters = (Volume * 1000, Radius, Pitch * 1000), confirmed from captures: landing sounds (xPawn: volume
	// FMin(1, ...)) top out at 1000, Actor.TransientSoundVolume 0.3 (pickups, switches) arrives as 300, ONS vehicle guns'
	// SoundVolume 160 / 255 as 627; LightningGunChargeUp (pitch 1.1) as 1100; radii are TransientSoundRadius (300 ...)
	// UE2 plays through OpenAL at Volume * SoundVolume (0.3, the output stage's mix level), capped at 1 (AL_MAX_GAIN):
	// louder requests (vehicle guns' 1.25 ...) play louder than volume 1, up to 1 / 0.3
	const float Volume = FMath::Clamp(float(Parameters.X) / 1000.f, 0.02f, 1.f / 0.3f);
	const float Pitch = Parameters.Z > 0.f ? FMath::Clamp(float(Parameters.Z) / 1000.f, 0.5f, 2.f) : 1.f;
	const float Radius = Parameters.Y > 0.f ? float(Parameters.Y) : 300.f;
	++SoundsPlayed;
	// UE2 hears the sounds of the view target (our own pawn, or the player we spectate in first person) at the
	// listener: not spatialized, so the pawn's own voice, pickups and landings are centred
	const FUE2NetPlay* Play = FUE2NetPlay::Get();
	if (SourceChannel != 0 && ((Play && Play->IsOwnPawnChannel(SourceChannel)) || SourceChannel == FollowPawn))
	{
		b2D = true;
	}
	if (b2D)
	{
		UGameplayStatics::PlaySound2D(W, S2, Volume, Pitch);
		return;
	}
	if (FEntity* Src = FindEntity(SourceChannel))
	{
		if (AActor* N = Src->Native.Get())
		{
			Location = N->GetActorLocation();
		}
	}
	UGameplayStatics::PlaySoundAtLocation(W, S2, Location, FRotator::ZeroRotator, Volume, Pitch, 0.f, SoundAttenuation(Radius));
}

// =================================================================================================
// Spectating
// =================================================================================================

void FUE2NetWorld::SetViewTargetFromServer(const FUE2NetValue& Target)
{
	AArenaPlayerController* PC = GetPC();
	if (!PC)
	{
		return;
	}
	if (Target.Obj.IsDynamic())
	{
		if (AUE2NetPawn* Pawn = PawnForChannel(Target.Obj.Channel))
		{
			const FUE2NetActor* A = Connection->GetActors().Find(Target.Obj.Channel);
			FollowPawn = Target.Obj.Channel;
			FollowPRI = A ? GetRef(*A, TEXT("PlayerReplicationInfo")) : 0;
			bFreeCamera = false;
			bWantFreeCamera = false;
			PendingViewChannel = 0;
			PC->SetViewTarget(Pawn);
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: following %s"), Pawn->GetPlayerState() ? *Pawn->GetPlayerState()->GetPlayerName() : *Pawn->GetName());
			return;
		}
		if (Target.Obj.Channel == Connection->FindPlayerControllerChannel())
		{
			// ServerViewNextPlayer cycled round to our own view: the free camera
			bWantFreeCamera = true;
		}
		else if (!Connection->GetActors().Contains(Target.Obj.Channel))
		{
			// the pawn becomes relevant to us only now: its channel opens right after this call
			PendingViewChannel = Target.Obj.Channel;
			return;
		}
	}
	// None: our own view (ServerViewSelf), or a view target that is not replicated to us (yet): the server sends
	// references to actors without an open channel as None. UE2 clients then ask again (ServerVerifyViewTarget); by
	// then the new target's pawn is relevant to us. Keep following the current player meanwhile.
	if (!bWantFreeCamera)
	{
		const double Now = FPlatformTime::Seconds();
		if (Now - LastVerifyTime > 0.5 && Now - LastViewRequest < 10.0)
		{
			LastVerifyTime = Now;
			if (const int32 Channel = Connection->FindPlayerControllerChannel())
			{
				Connection->SendFunction(Channel, TEXT("ServerVerifyViewTarget"), {});
			}
		}
	}
	if (bWantFreeCamera || !FollowPRI)
	{
		bFreeCamera = true;
		FollowPRI = 0;
		FollowPawn = 0;
		AActor* Cam = PC->GetSpectatorPawn() ? static_cast<AActor*>(PC->GetSpectatorPawn()) : static_cast<AActor*>(PC);
		if (PC->GetViewTarget() != Cam)
		{
			if (APawn* SP = PC->GetSpectatorPawn())
			{
				// start the free camera where the view was
				FVector Loc;
				FRotator Rot;
				PC->GetPlayerViewPoint(Loc, Rot);
				SP->SetActorLocation(Loc);
				PC->SetControlRotation(Rot);
			}
			PC->SetViewTarget(Cam);
		}
	}
}

bool FUE2NetWorld::SpectatorFire(APlayerController* InPC, bool bAltFire)
{
	AArenaPlayerController* PC = Cast<AArenaPlayerController>(InPC);
	if (!PC || PC->GetWorld() != World.Get() || PC->IsPauseMenuOpen())
	{
		return false;
	}
	const int32 Channel = Connection->FindPlayerControllerChannel();
	if (!Channel)
	{
		return true;
	}
	LastViewRequest = FPlatformTime::Seconds();
	if (bAltFire && !bFreeCamera)
	{
		// free camera: ServerViewSelf (the server answers ClientSetViewTarget(None))
		bWantFreeCamera = true;
		Connection->SendFunction(Channel, TEXT("ServerViewSelf"), {});
		FUE2NetValue None;
		SetViewTargetFromServer(None);
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: free camera (ServerViewSelf)"));
		return true;
	}
	bWantFreeCamera = false;
	Connection->SendFunction(Channel, TEXT("ServerViewNextPlayer"), {});
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: next player (ServerViewNextPlayer)"));
	return true;
}

FString FUE2NetWorld::GetViewedPlayerName() const
{
	const AArenaPlayerController* PC = GetPC();
	const APawn* Pawn = PC ? Cast<APawn>(PC->GetViewTarget()) : nullptr;
	const APlayerState* PS = Pawn ? Pawn->GetPlayerState() : nullptr;
	return PS ? PS->GetPlayerName() : FString();
}

// =================================================================================================
// Tick
// =================================================================================================

void FUE2NetWorld::Tick(float DeltaSeconds)
{
	UWorld* W = World.Get();
	if (!W)
	{
		return;
	}
	for (auto It = PrePivoted.CreateIterator(); It; ++It)
	{
		USceneComponent* C = It.Value().Comp.Get();
		if (!C)
		{
			It.RemoveCurrent();
			continue;
		}
		const USceneComponent* P = C->GetAttachParent();
		const FTransform Frame = P ? P->GetSocketTransform(C->GetAttachSocketName()) : FTransform::Identity;
		C->SetRelativeLocation(It.Value().Relative + Frame.InverseTransformVector(It.Value().PrePivot));
	}
	const double Now = FPlatformTime::Seconds();
	for (int32 i = PendingShockCores.Num() - 1; i >= 0; --i)
	{
		if (Now >= PendingShockCores[i].Value)
		{
			if (AUE2NetProjectile* Core = PendingShockCores[i].Key.Get())
			{
				Core->NetExplode();
			}
			PendingShockCores.RemoveAt(i);
		}
	}
	RecentCombos.RemoveAll([Now](const TPair<FVector, double>& C) { return Now - C.Value > 0.5; });
	AArenaGameState* GS = W->GetGameState<AArenaGameState>();
	// UE2 GameReplicationInfo.Timer on clients: the remaining time counts down locally between RemainingMinute updates
	if (Now >= NextSecond)
	{
		NextSecond = Now + 1.0;
		if (!bStopCountDown && RemainingTime > 0)
		{
			--RemainingTime;
		}
		if (bAssault && bMatchHasBegun && !bGameEnded)
		{
			++ElapsedTime;
			if (RoundTimeLimit > 0)
			{
				RemainingTime = FMath::Max(0, RoundTimeLimit - (ElapsedTime - RoundStartTime));
			}
		}
		if (GS && GRIChannel)
		{
			GS->RemainingTime = RemainingTime;
		}
		// late links: pawns whose PRI arrived after them, attachments whose pawn arrived after them
		const TMap<int32, FUE2NetActor>& Actors = Connection->GetActors();
		for (TPair<int32, FEntity>& P : Entities)
		{
			const FUE2NetActor* A = Actors.Find(P.Key);
			if (!A)
			{
				continue;
			}
			if (P.Value.Kind == EKind::Pawn)
			{
				AUE2NetPawn* Pawn = Cast<AUE2NetPawn>(P.Value.Native.Get());
				AArenaPlayerState* PS = PSForChannel(GetRef(*A, TEXT("PlayerReplicationInfo")));
				if (Pawn && PS && Pawn->GetPlayerState() != PS)
				{
					Pawn->SetPlayerState(PS);
					if (const FUE2NetActor* PRI = Actors.Find(GetRef(*A, TEXT("PlayerReplicationInfo"))))
					{
						Pawn->SetCharacter(GetStr(*PRI, TEXT("CharacterName")), GetInt(*PRI, TEXT("PlayerID")));
					}
				}
			}
			else if (P.Value.Kind == EKind::Attachment || P.Value.Kind == EKind::Flag || P.Value.Kind == EKind::DomPoint || P.Value.Kind == EKind::GameObject
				|| P.Value.Kind == EKind::Objective)
			{
				Apply(P.Value, *A, {}, false);
			}
			else if (P.Value.Kind == EKind::VehicleWeapon && !P.Value.Native.IsValid())
			{
				Apply(P.Value, *A, {}, false); // its vehicle opened after it
			}
			else if (P.Value.Kind == EKind::Vehicle || P.Value.Kind == EKind::WeaponPawn)
			{
				LinkSeat(P.Value, *A); // drivers / gunners whose pawn opened after the vehicle
			}
		}
		for (int32 i = Corpses.Num() - 1; i >= 0; --i)
		{
			if (!Corpses[i].Key.IsValid() || Now >= Corpses[i].Value)
			{
				if (Corpses[i].Key.IsValid())
				{
					Corpses[i].Key->Destroy();
				}
				Corpses.RemoveAt(i);
			}
		}
		for (auto It = RecentSoundIds.CreateIterator(); It; ++It)
		{
			if (Now - It.Value() > 2.0)
			{
				It.RemoveCurrent();
			}
		}
	}
	// Assault hold progress while the server says an objective is held
	for (const TPair<int32, FNetObjective>& P : NetObjectives)
	{
		if (P.Value.HeldSince > 0.0)
		{
			RefreshAssaultObjectives();
			break;
		}
	}
	// following a player: their new pawn after a respawn (playing: until we have a pawn of our own again)
	AArenaPlayerController* PC = GetPC();
	if (const FUE2NetPlay* Play = FUE2NetPlay::Get(); Play && Play->GetLocalPawn() && FollowPRI)
	{
		FollowPRI = 0;
		FollowPawn = 0;
	}
	if (PC && !bFreeCamera && FollowPRI)
	{
		AUE2NetPawn* Current = Cast<AUE2NetPawn>(PC->GetViewTarget());
		if (!Current || Current->IsNetDead())
		{
			if (AUE2NetPawn* Pawn = PawnOfPRI(FollowPRI, true))
			{
				PC->SetViewTarget(Pawn);
			}
		}
	}
	// test hook script
	while (Script.Num() > 0 && Now >= Script[0].Key)
	{
		const FString Cmd = Script[0].Value;
		Script.RemoveAt(0);
		if (PC)
		{
			UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: spectate script: %s"), *Cmd);
			if (Cmd == TEXT("ball"))
			{
				// test view of the bombing run ball: its carrier, else the free camera looking at it
				TActorIterator<AArenaBR_Ball> It(W);
				if (AArenaBR_Ball* Ball = It ? *It : nullptr)
				{
					if (AArenaCharacter* Carrier = Ball->GetCarrier())
					{
						PC->SetViewTarget(Carrier);
					}
					else if (APawn* SP = PC->GetSpectatorPawn())
					{
						const FVector At = Ball->GetObjectLocation();
						SP->SetActorLocation(At + FVector(-350.f, -250.f, 220.f));
						PC->SetControlRotation((At - SP->GetActorLocation()).Rotation());
						PC->SetViewTarget(SP);
						bFreeCamera = true;
						FollowPRI = 0;
					}
					UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: spectate script: ball view (%s)"), Ball->GetCarrier() ? TEXT("carrier") : TEXT("free camera"));
				}
			}
			else if (Cmd == TEXT("vehicle") || Cmd.StartsWith(TEXT("vehicle=")))
			{
				// test view of the next vehicle class not viewed yet (parked ones first), or of a class ("vehicle=odinv2omni"):
				// the free camera in front of it
				static TSet<FString> Viewed;
				const FString Only = Cmd.Mid(8);
				AActor* Pick = nullptr;
				FString PickClass;
				for (int32 Pass = 0; Pass < 2 && !Pick; ++Pass)
				{
					for (const TPair<int32, FEntity>& P : Entities)
					{
						// (vehicles, and stationary turrets: weapon pawns with a vehicle actor of their own)
						AUE2NetVehicle* V = P.Value.Kind == EKind::Vehicle || (P.Value.Kind == EKind::WeaponPawn && P.Value.bOwned)
							? Cast<AUE2NetVehicle>(P.Value.Native.Get()) : nullptr;
						if (V && (Only.IsEmpty() ? !Viewed.Contains(P.Value.ClassName) : P.Value.ClassName.Equals(Only, ESearchCase::IgnoreCase)) && (Pass == 1 || V->GetVelocity().Size() < 50.f) && !V->IsDestroyedAppearance())
						{
							Pick = V;
							PickClass = P.Value.ClassName;
							break;
						}
					}
				}
				AActor* Cam = PC->GetSpectatorPawn() ? static_cast<AActor*>(PC->GetSpectatorPawn()) : static_cast<AActor*>(PC);
				if (Pick)
				{
					Viewed.Add(PickClass);
					FVector Origin, Extent;
					Pick->GetActorBounds(false, Origin, Extent);
					const float Dist = FMath::Clamp(float(Extent.Size()) * 1.6f, 300.f, 2500.f);
					const FVector Dir = (Pick->GetActorForwardVector() * 0.8f + Pick->GetActorRightVector() * 0.6f).GetSafeNormal();
					Cam->SetActorLocation(Origin + Dir * Dist + FVector(0.f, 0.f, Dist * 0.35f));
					PC->SetControlRotation((Origin - Cam->GetActorLocation()).Rotation());
					PC->SetViewTarget(Cam);
					bFreeCamera = true;
					FollowPRI = 0;
				}
				UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: spectate script: vehicle view %s"), Pick ? *PickClass : TEXT("(none left)"));
			}
			else if (Cmd == TEXT("frozen"))
			{
				// test view of a frozen Freeze Tag pawn (the next one each time): the free camera beside it
				static int32 Next = 0;
				TArray<AUE2NetPawn*> Frozen;
				for (const TPair<int32, FEntity>& P : Entities)
				{
					if (AUE2NetPawn* Pawn = P.Value.Kind == EKind::Pawn ? Cast<AUE2NetPawn>(P.Value.Native.Get()) : nullptr; Pawn && Pawn->IsNetFrozen())
					{
						Frozen.Add(Pawn);
					}
				}
				AActor* Cam = PC->GetSpectatorPawn() ? static_cast<AActor*>(PC->GetSpectatorPawn()) : static_cast<AActor*>(PC);
				if (Frozen.Num() > 0)
				{
					AUE2NetPawn* Pick = Frozen[Next++ % Frozen.Num()];
					const FVector At = Pick->GetActorLocation();
					const FVector Dir = (Pick->GetActorForwardVector() * 0.9f + Pick->GetActorRightVector() * 0.4f).GetSafeNormal();
					Cam->SetActorLocation(At + Dir * 260.f + FVector(0.f, 0.f, 60.f));
					PC->SetControlRotation((At - Cam->GetActorLocation()).Rotation());
					PC->SetViewTarget(Cam);
					bFreeCamera = true;
					FollowPRI = 0;
				}
				UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: spectate script: frozen view (%d frozen)"), Frozen.Num());
			}
			else if (Cmd == TEXT("scores") || Cmd == TEXT("noscores"))
			{
				PC->SetScoreboardHeld(Cmd == TEXT("scores"));
				// the server's view, for comparing with the server log
				for (const TPair<int32, FEntity>& P : Entities)
				{
					if (const AArenaPlayerState* PS = P.Value.Kind == EKind::PRI ? P.Value.PS.Get() : nullptr)
					{
						UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld: score %s %.0f deaths %d team %d"), *PS->GetPlayerName(), PS->GetScore(), PS->Deaths, PS->Team);
					}
				}
			}
			else
			{
				SpectatorFire(PC, Cmd == TEXT("self") || Cmd == TEXT("alt"));
			}
		}
	}
	if (Now >= NextSummary)
	{
		NextSummary = Now + 30.0;
		LogSummary();
	}
}

void FUE2NetWorld::LogSummary()
{
	using namespace UE2NetWorldLocal;
	FMemory::Memzero(KindLive, sizeof(KindLive));
	for (const TPair<int32, FEntity>& P : Entities)
	{
		++KindLive[int32(P.Value.Kind)];
	}
	FString Kinds;
	for (int32 K = 0; K < int32(EKind::Num); ++K)
	{
		if (KindOpened[K] > 0)
		{
			Kinds += FString::Printf(TEXT(" %s %d/%d"), KindName(EKind(K)), KindLive[K], KindOpened[K]);
		}
	}
	TArray<FString> Unknown;
	for (const TPair<FString, int32>& U : UnknownClasses)
	{
		Unknown.Add(FString::Printf(TEXT("%s x%d"), *U.Key, U.Value));
	}
	TArray<FString> Msgs;
	for (const TPair<FString, int32>& U : UnhandledMessages)
	{
		Msgs.Add(FString::Printf(TEXT("%s x%d"), *U.Key, U.Value));
	}
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld summary (%.0f s): live/opened%s; projectiles flown %d (average %.0f cm); pawn updates %d (shown vs received: average %.1f cm, max %.0f cm); pickups matched %d, unmatched %d; messages %d, kill lines %d, sounds %d; unknown classes (ignored): %s; messages not shown: %s"),
		FPlatformTime::Seconds() - StartTime, *Kinds, AUE2NetProjectile::NumEnded, AUE2NetProjectile::NumEnded ? AUE2NetProjectile::SumDistance / AUE2NetProjectile::NumEnded : 0.0,
		AUE2NetPawn::NumCorrections, AUE2NetPawn::NumCorrections ? AUE2NetPawn::SumCorrection / AUE2NetPawn::NumCorrections : 0.0, AUE2NetPawn::MaxCorrection, PickupsMatched, PickupsUnmatched, MessagesShown, KillLines, SoundsPlayed,
		Unknown.Num() ? *FString::Join(Unknown, TEXT(", ")) : TEXT("none"), Msgs.Num() ? *FString::Join(Msgs, TEXT(", ")) : TEXT("none"));
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetWorld summary (game objects): power nodes matched %d (updates %d), objectives matched %d (updates %d), ball updates %d; vehicle aim updates %d (aim check: %d, barrel off the aim point by %.1f deg on average, max %.0f), weapon flashes %d, seats shown %d, wrecks %d; script messages %d"),
		NodesMatched, NodeUpdates, ObjectivesMatched, ObjectiveUpdates, BallUpdates, AUE2NetVehicle::NumAimUpdates, AUE2NetVehicle::NumAimChecks,
		AUE2NetVehicle::NumAimChecks ? AUE2NetVehicle::SumAimError / AUE2NetVehicle::NumAimChecks : 0.0, AUE2NetVehicle::MaxAimError, AUE2NetVehicle::NumFlashes, AUE2NetVehicle::NumSeatsShown,
		AUE2NetVehicle::NumWrecks, ScriptedMessages);
}

namespace ArenaNet
{
	bool HandleSpectatorFire(APlayerController* PC, bool bAltFire)
	{
		if (HandleNetPlayInput(PC, int32(bAltFire ? FUE2NetPlay::EInput::AltFire : FUE2NetPlay::EInput::Fire)))
		{
			return true; // playing on the server: fire / respawn (Net/UE2NetPlay.h)
		}
		FUE2NetWorld* W = FUE2NetWorld::Get();
		return W && PC && PC->GetWorld() == W->GetWorld() && W->SpectatorFire(PC, bAltFire);
	}
}
