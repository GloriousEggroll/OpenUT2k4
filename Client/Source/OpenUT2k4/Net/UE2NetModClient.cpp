#include "UE2NetModClient.h"

#include "../UScript/UScriptCore.h"
#include "../UScript/UScriptRuntime.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "UE2NetConnection.h"
#include "UE2NetLog.h"
#include "UE2NetModContent.h"

namespace UE2NetModClientLocal
{
	FUSClass* ChainClass(const FUE2NetActor& A, const TCHAR* Name)
	{
		for (FUSClass* C = A.Class; C; C = C->GetSuperClass())
		{
			if (C->Name == Name)
			{
				return C;
			}
		}
		return nullptr;
	}

	int32 GetRef(const FUE2NetActor& A, const TCHAR* Key)
	{
		const FUE2NetValue* V = A.Properties.Find(Key);
		return V && V->Kind == FUE2NetValue::EKind::Object && V->Obj.IsDynamic() ? V->Obj.Channel : 0;
	}

	const FUE2NetValue* Param(const TArray<TPair<FName, FUE2NetValue>>& Params, const TCHAR* Name)
	{
		for (const TPair<FName, FUE2NetValue>& P : Params)
		{
			if (P.Key == Name)
			{
				return &P.Value;
			}
		}
		return nullptr;
	}

	/** A class default (config values included, as the script runtime read them) as text, or empty. */
	FString DefaultText(FUSClass* Class, const TCHAR* Key, int32 Index = 0)
	{
		FUSObject* Def = Class ? Class->GetDefaultObject() : nullptr;
		FUSProperty* Prop = Def ? Def->FindProperty(FName(Key)) : nullptr;
		if (!Prop || !Def->Data)
		{
			return FString();
		}
		const uint8* Value = Def->Data + Prop->Offset;
		if (Prop->Type == EUSType::Array)
		{
			// a dynamic array (TArray in the UE2 layout: data pointer, count): element Index
			const FUSScriptArray* Arr = reinterpret_cast<const FUSScriptArray*>(Value);
			if (!Prop->Inner || Index < 0 || Index >= Arr->Num || !Arr->Data)
			{
				return FString();
			}
			Value = Arr->Data + Index * Prop->Inner->ElementSize;
			Prop = Prop->Inner;
		}
		if (Prop->Type == EUSType::Byte)
		{
			return FString::FromInt(*Value);
		}
		FString Out;
		Prop->ExportText(Out, Value, true);
		return Out;
	}

	float DefaultFloat(FUSClass* Class, const TCHAR* Key, float Fallback)
	{
		const FString T = DefaultText(Class, Key);
		return T.IsEmpty() ? Fallback : FCString::Atof(*T);
	}

	bool DefaultBool(FUSClass* Class, const TCHAR* Key, bool bFallback)
	{
		const FString T = DefaultText(Class, Key);
		return T.IsEmpty() ? bFallback : T.Equals(TEXT("True"), ESearchCase::IgnoreCase);
	}

	/** "<package of Class>.<Name>": a class of the same mod package. */
	FUSClass* SiblingClass(FUSClass* Class, const TCHAR* Name)
	{
		if (!Class)
		{
			return nullptr;
		}
		return FUSRuntime::Get().FindClass(Class->GetPackageName().ToString() + TEXT(".") + Name);
	}
}

FUE2NetModClient::FUE2NetModClient(FUE2NetConnection& InConnection, UWorld* InWorld)
	: Connection(InConnection), World(InWorld)
{
}

void FUE2NetModClient::PlayLocal(const FString& SoundPath, float Volume, float Pitch)
{
	++HitSoundEvents;
	USoundBase* Sound = UE2NetModContent::LoadSound(SoundPath);
	UWorld* W = World.Get();
	if (Sound)
	{
		UE_LOG(LogUE2Net, Verbose, TEXT("UE2NetModClient: hit sound %s (volume %.2f, pitch %.2f)"), *SoundPath, Volume, Pitch);
	}
	else if (HitSoundEvents == HitSoundsPlayed + 1)
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetModClient: hit sound %s is not available"), *SoundPath);
	}
	if (!Sound || !W || Volume <= 0.f)
	{
		return;
	}
	++HitSoundsPlayed;
	UGameplayStatics::PlaySound2D(W, Sound, FMath::Clamp(Volume, 0.f, 2.f), FMath::Clamp(Pitch, 0.1f, 4.f));
}

int32 FUE2NetModClient::FindLinkedInfo(int32 OwnPRIChannel, const TCHAR* ClassName) const
{
	using namespace UE2NetModClientLocal;
	const TMap<int32, FUE2NetActor>& Actors = Connection.GetActors();
	const FUE2NetActor* PRI = OwnPRIChannel ? Actors.Find(OwnPRIChannel) : nullptr;
	int32 Ch = PRI ? GetRef(*PRI, TEXT("CustomReplicationInfo")) : 0;
	for (int32 Guard = 0; Ch && Guard < 32; ++Guard)
	{
		const FUE2NetActor* L = Actors.Find(Ch);
		if (!L)
		{
			break;
		}
		if (ChainClass(*L, ClassName))
		{
			return Ch;
		}
		Ch = GetRef(*L, TEXT("NextReplicationInfo"));
	}
	// some mods keep it elsewhere: an actor of that class owned by us is replicated with bNetOwner values
	return 0;
}

int32 FUE2NetModClient::TeamOfPawnChannel(int32 PawnChannel) const
{
	using namespace UE2NetModClientLocal;
	const TMap<int32, FUE2NetActor>& Actors = Connection.GetActors();
	const FUE2NetActor* P = PawnChannel ? Actors.Find(PawnChannel) : nullptr;
	const FUE2NetActor* PRI = P ? Actors.Find(GetRef(*P, TEXT("PlayerReplicationInfo"))) : nullptr;
	const FUE2NetActor* Team = PRI ? Actors.Find(GetRef(*PRI, TEXT("Team"))) : nullptr;
	const FUE2NetValue* Index = Team ? Team->Properties.Find(TEXT("TeamIndex")) : nullptr;
	return Index ? Index->I : 255;
}

void FUE2NetModClient::ActorUpdated(const FUE2NetActor& A, const TArray<FString>& Changed, bool bOpened, int32 OwnPRIChannel)
{
	using namespace UE2NetModClientLocal;
	if (A.bBroken || !A.Class)
	{
		return;
	}
	if (Changed.Contains(TEXT("HitSoundRepInfo")))
	{
		FUSClass* TagClass = ChainClass(A, TEXT("UTPlusHitSoundTag"));
		const FUE2NetValue* V = TagClass ? A.Properties.Find(TEXT("HitSoundRepInfo")) : nullptr;
		if (V)
		{
			int32 HitType = 0, HitData = 0;
			for (const TPair<FName, FUE2NetValue>& M : V->Members)
			{
				HitType = M.Key == TEXT("HitType") ? M.Value.I : HitType;
				HitData = M.Key == TEXT("HitData") ? M.Value.I : HitData;
			}
			const int32 Packed = HitType << 8 | HitData;
			const int32* Found = LastUTPlusHit.Find(A.Channel);
			const int32 Last = Found ? *Found : 0;
			LastUTPlusHit.Add(A.Channel, Packed);
			// UTPlusHitSoundTag.PostNetReceive: a changed HitSoundRepInfo plays (never the initial value); IsViewTarget:
			// our own tag (Keeper is our PRI)
			const int32 Keeper = GetRef(A, TEXT("Keeper"));
			// (LastHitSoundInfo starts zero: the first change plays too)
			if (!bOpened && Last != Packed && HitType != 0 && (Keeper == 0 || Keeper == OwnPRIChannel))
			{
				// UTPlusSettings (client config, the class defaults when the player has none): the sound per hit type,
				// its volume and the damage pitch
				FUSClass* Settings = SiblingClass(TagClass, TEXT("UTPlusSettings"));
				static const TCHAR* TypeKeys[] = { nullptr, TEXT("HitSoundEnemy"), TEXT("HitSoundTeam"), TEXT("HitSoundVehicle"), TEXT("HitSoundEmptyVehicle"), TEXT("HitSoundObjective") };
				const TCHAR* Key = HitType < UE_ARRAY_COUNT(TypeKeys) ? TypeKeys[HitType] : nullptr;
				const int32 SoundIndex = Key ? FCString::Atoi(*DefaultText(Settings, Key)) : -1;
				FString Name = SoundIndex >= 0 ? DefaultText(Settings, TEXT("HitSoundNames"), SoundIndex) : FString();
				const float Volume = DefaultFloat(Settings, TEXT("HitSoundVolume"), 1.f);
				const float Pitch = DefaultBool(Settings, TEXT("bHitSoundDamagePitch"), true) ? float(HitData & 0x1F) * 0.0613f + 0.1f : 1.f;
				if (!Name.IsEmpty())
				{
					// UTPlusHitSoundTag.LoadHitSound: unqualified names are in the tag package's HitSounds group
					if (!Name.Contains(TEXT(".")))
					{
						Name = TagClass->GetPackageName().ToString() + TEXT(".HitSounds.") + Name;
					}
					PlayLocal(Name, Volume, Pitch);
				}
			}
		}
	}
	if (Changed.Contains(TEXT("bInWarmup")) && ChainClass(A, TEXT("UTComp_Warmup")))
	{
		const FUE2NetValue* V = A.Properties.Find(TEXT("bInWarmup"));
		bWarmup = V && V->I != 0;
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetModClient: UTComp warmup %s"), bWarmup ? TEXT("running") : TEXT("over"));
	}
}

bool FUE2NetModClient::FunctionCall(const FUE2NetActor& A, FUSFunction* Function, const TArray<TPair<FName, FUE2NetValue>>& Params, int32 OwnPRIChannel)
{
	using namespace UE2NetModClientLocal;
	if (!Function)
	{
		return false;
	}
	const FName Name = Function->Name;
	FUSClass* PCClass = ChainClass(A, TEXT("BS_xPlayer"));
	if (!PCClass)
	{
		return false;
	}
	if (Name == TEXT("ReceiveHitSound") || Name == TEXT("ReceiveHit"))
	{
		// BS_xPlayer.ReceiveHitSound(Damage, iTeam: 1 enemy, 2 teammate) / ReceiveHit(DamageType, Damage, Injured,
		// instigatedBy) -> PlayEnemyHitSound / PlayTeammateHitSound(Damage)
		const FUE2NetValue* Damage = Param(Params, TEXT("Damage"));
		int32 Kind = 0;
		if (Name == TEXT("ReceiveHitSound"))
		{
			const FUE2NetValue* Team = Param(Params, TEXT("iTeam"));
			Kind = Team ? Team->I : 0;
		}
		else
		{
			const FUE2NetValue* Injured = Param(Params, TEXT("Injured"));
			const int32 InjuredCh = Injured && Injured->Obj.IsDynamic() ? Injured->Obj.Channel : 0;
			const FUE2NetActor* PRI = OwnPRIChannel ? Connection.GetActors().Find(OwnPRIChannel) : nullptr;
			const FUE2NetActor* OwnTeam = PRI ? Connection.GetActors().Find(GetRef(*PRI, TEXT("Team"))) : nullptr;
			const FUE2NetValue* OwnIndex = OwnTeam ? OwnTeam->Properties.Find(TEXT("TeamIndex")) : nullptr;
			const int32 InjuredTeam = TeamOfPawnChannel(InjuredCh);
			const FUE2NetActor* InjuredActor = InjuredCh ? Connection.GetActors().Find(InjuredCh) : nullptr;
			const bool bSelf = InjuredActor && GetRef(*InjuredActor, TEXT("PlayerReplicationInfo")) == OwnPRIChannel && OwnPRIChannel != 0;
			Kind = bSelf ? 0 : (InjuredTeam == 255 || !OwnIndex || InjuredTeam != OwnIndex->I) ? 1 : 2;
		}
		const double Now = World.IsValid() ? World->GetTimeSeconds() : 0.0;
		FUSClass* Settings = SiblingClass(PCClass, TEXT("UTComp_Settings"));
		if (Kind != 0 && DefaultBool(Settings, TEXT("bEnableHitSounds"), true) && Now >= NextUTCompHitSound)
		{
			NextUTCompHitSound = Now + 0.05; // HITSOUNDTWEENTIME
			float Pitch = 1.f;
			const int32 Dmg = Damage ? Damage->I : 0;
			if (DefaultBool(Settings, TEXT("bCPMAStyleHitsounds"), false) && Dmg > 0)
			{
				Pitch = DefaultFloat(Settings, TEXT("CPMAPitchModifier"), 1.f) * 30.f / float(Dmg);
			}
			// UTComp_Settings.CheckSettings points the sound at the settings' own package; a default without the package
			// ("Sounds.HitSound") is a group of it
			FString Path = DefaultText(Settings, Kind == 1 ? TEXT("EnemySound") : TEXT("FriendlySound"));
			int32 Dots = 0;
			for (TCHAR Ch : Path)
			{
				Dots += Ch == TEXT('.') ? 1 : 0;
			}
			if (Settings && !Path.IsEmpty() && Dots < 2)
			{
				Path = Settings->GetPackageName().ToString() + TEXT(".") + Path;
			}
			PlayLocal(Path, DefaultFloat(Settings, TEXT("HitSoundVolume"), 1.f), Pitch);
		}
		return true;
	}
	if (Name == TEXT("NotifyEndWarmup"))
	{
		bWarmup = false;
		bWarmupEnded = true;
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetModClient: UTComp warmup ended"));
		return true;
	}
	return false;
}

bool FUE2NetModClient::SetReady(bool bReady, int32 OwnPRIChannel)
{
	// BS_xPlayer.Ready / NotReady: UTComp_PRI.Ready() / NotReady(), then BroadcastReady(bReady) on the controller
	const int32 UTCompPRI = FindLinkedInfo(OwnPRIChannel, TEXT("UTComp_PRI"));
	if (!UTCompPRI)
	{
		return false;
	}
	++ReadyCalls;
	Connection.SendFunction(UTCompPRI, FName(bReady ? TEXT("Ready") : TEXT("NotReady")), {});
	const int32 PC = Connection.FindPlayerControllerChannel();
	if (PC && Connection.FindReplicatedFunction(PC, FName(TEXT("BroadcastReady"))))
	{
		FUE2NetValue B;
		B.Kind = FUE2NetValue::EKind::Bool;
		B.I = bReady ? 1 : 0;
		Connection.SendFunction(PC, FName(TEXT("BroadcastReady")), { B });
	}
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetModClient: UTComp %s"), bReady ? TEXT("Ready") : TEXT("NotReady"));
	return true;
}

FString FUE2NetModClient::DescribeWarmup() const
{
	if (!bWarmup)
	{
		return FString();
	}
	// UTComp_PRI.bIsReady of every player
	int32 Players = 0, Ready = 0;
	for (const TPair<int32, FUE2NetActor>& KV : Connection.GetActors())
	{
		if (UE2NetModClientLocal::ChainClass(KV.Value, TEXT("UTComp_PRI")))
		{
			++Players;
			const FUE2NetValue* V = KV.Value.Properties.Find(TEXT("bIsReady"));
			Ready += V && V->I != 0 ? 1 : 0;
		}
	}
	return FString::Printf(TEXT("Warmup: %d of %d ready (type Ready / NotReady)"), Ready, Players);
}

FString FUE2NetModClient::Describe() const
{
	return FString::Printf(TEXT("mod client: hit sounds %d of %d played, ready calls %d, warmup %s"), HitSoundsPlayed, HitSoundEvents, ReadyCalls,
		bWarmup ? TEXT("running") : (bWarmupEnded ? TEXT("ended") : TEXT("none")));
}
