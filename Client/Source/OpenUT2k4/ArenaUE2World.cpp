#include "ArenaUE2World.h"
#include "OpenUT2k4.h"
#include "ArenaEmitters.h"
#include "ArenaMover.h"
#include "ArenaRuntimeContent.h"
#include "ArenaSkybox.h"
#include "ArenaTriggers.h"
#include "ArenaWorldZones.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "PhysicsEngine/PhysicsSettings.h"

namespace ArenaUE2WorldLocal
{
	const TArray<TSharedPtr<FJsonValue>>& EmptyArray()
	{
		static const TArray<TSharedPtr<FJsonValue>> Empty;
		return Empty;
	}

	TMap<FString, UClass*>& Registry()
	{
		static TMap<FString, UClass*> R;
		return R;
	}

	void RegisterBuiltins()
	{
		static bool bDone = false;
		if (bDone)
		{
			return;
		}
		bDone = true;
		TMap<FString, UClass*>& R = Registry();
		auto Add = [&R](const TCHAR* Name, UClass* Class) { if (!R.Contains(FString(Name).ToLower())) R.Add(FString(Name).ToLower(), Class); };
		Add(TEXT("ZoneInfo"), AArenaZoneInfo::StaticClass());
		Add(TEXT("SkyZoneInfo"), AArenaSkyZoneInfo::StaticClass());
		Add(TEXT("PhysicsVolume"), AArenaPhysicsVolume::StaticClass());
		Add(TEXT("WaterVolume"), AArenaWaterVolume::StaticClass());
		Add(TEXT("LavaVolume"), AArenaLavaVolume::StaticClass());
		Add(TEXT("PressureVolume"), AArenaPressureVolume::StaticClass());
		Add(TEXT("xFallingVolume"), AArenaFallingVolume::StaticClass());
		Add(TEXT("BlockingVolume"), AArenaBlockingVolume::StaticClass());
		Add(TEXT("Mover"), AArenaMover::StaticClass());
		Add(TEXT("Trigger"), AArenaTrigger::StaticClass());
		Add(TEXT("Dispatcher"), AArenaDispatcher::StaticClass());
		Add(TEXT("Counter"), AArenaCounter::StaticClass());
		Add(TEXT("TriggeredCondition"), AArenaTriggeredCondition::StaticClass());
		Add(TEXT("Teleporter"), AArenaTeleporter::StaticClass());
		Add(TEXT("ScriptedTrigger"), AArenaScriptedTrigger::StaticClass());
		Add(TEXT("Emitter"), AArenaEmitter::StaticClass());
		Add(TEXT("xEmitter"), AArenaEmitter::StaticClass());
		Add(TEXT("AmbientSound"), AArenaAmbientSound::StaticClass());
	}

	/** Base classes that must not be matched through the chain (too generic, or handled elsewhere). */
	bool IsExcludedChain(const TArray<FString>& Chain)
	{
		for (const FString& C : Chain)
		{
			// LevelInfo is a ZoneInfo; DefaultPhysicsVolume is the level's (world gravity); gameplay triggers of
			// the modes (objectives, vehicle factories) are not world triggers
			if (C.Equals(TEXT("LevelInfo"), ESearchCase::IgnoreCase) || C.Equals(TEXT("DefaultPhysicsVolume"), ESearchCase::IgnoreCase)
				|| C.Equals(TEXT("GameObjective"), ESearchCase::IgnoreCase) || C.Equals(TEXT("SVehicleFactory"), ESearchCase::IgnoreCase)
				|| C.Equals(TEXT("LiftCenter"), ESearchCase::IgnoreCase) || C.Equals(TEXT("LiftExit"), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	TArray<FString> StringArray(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Obj.IsValid() && Obj->TryGetArrayField(Field, Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				FString S;
				Out.Add(V.IsValid() && V->TryGetString(S) ? S : FString());
			}
		}
		return Out;
	}

	FVector JsonVec(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const FVector& Default)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Obj.IsValid() && Obj->TryGetArrayField(Field, Arr) && Arr->Num() >= 3)
		{
			return FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		}
		return Default;
	}

	FRotator JsonRot(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* R = nullptr;
		if (Obj.IsValid() && Obj->TryGetObjectField(Field, R))
		{
			return FRotator((*R)->GetNumberField(TEXT("pitch")), (*R)->GetNumberField(TEXT("yaw")), (*R)->GetNumberField(TEXT("roll")));
		}
		return FRotator::ZeroRotator;
	}

	FName JsonName(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		FString S;
		if (Obj.IsValid() && Obj->TryGetStringField(Field, S) && !S.IsEmpty() && S != TEXT("None"))
		{
			return FName(*S);
		}
		return NAME_None;
	}

	bool ParseDesc(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& ClassDefaults, FArenaUE2ActorDesc& D)
	{
		if (!A.IsValid())
		{
			return false;
		}
		D.Json = A;
		D.ClassDefaults = ClassDefaults;
		D.Index = int32(A->GetNumberField(TEXT("index")));
		A->TryGetStringField(TEXT("name"), D.Name);
		A->TryGetStringField(TEXT("class"), D.Class);
		D.ClassChain = StringArray(A, TEXT("class_chain"));
		D.Location = JsonVec(A, TEXT("location"), FVector::ZeroVector);
		D.Rotation = JsonRot(A, TEXT("rotation"));
		double Ds = 1.0;
		A->TryGetNumberField(TEXT("draw_scale"), Ds);
		D.DrawScale = Ds == 0.0 ? 1.f : float(Ds);
		D.DrawScale3D = JsonVec(A, TEXT("draw_scale3d"), FVector::OneVector);
		D.PrePivot = JsonVec(A, TEXT("pre_pivot"), FVector::ZeroVector);
		FString Mesh;
		if (A->TryGetStringField(TEXT("static_mesh"), Mesh) && Mesh != TEXT("None"))
		{
			D.StaticMesh = Mesh;
		}
		D.Skins = StringArray(A, TEXT("skins"));
		D.Tag = JsonName(A, TEXT("tag"));
		D.Event = JsonName(A, TEXT("event"));
		A->TryGetBoolField(TEXT("hidden"), D.bHidden);
		double V = 0.0;
		if (A->TryGetNumberField(TEXT("collision_radius"), V)) D.CollisionRadius = float(V);
		if (A->TryGetNumberField(TEXT("collision_height"), V)) D.CollisionHeight = float(V);
		const TSharedPtr<FJsonObject>* Props = nullptr;
		TSharedPtr<FJsonObject> Defaults;
		const TSharedPtr<FJsonObject>* Def = nullptr;
		if (ClassDefaults.IsValid() && ClassDefaults->TryGetObjectField(D.Class, Def))
		{
			Defaults = *Def;
		}
		D.Props = FArenaUE2Props::Merge(Defaults, A->TryGetObjectField(TEXT("properties"), Props) ? *Props : nullptr);
		return true;
	}

	TAutoConsoleVariable<int32> CVarLogEvents(TEXT("arena.LogUE2Events"), 0, TEXT("Log every UE2 event (TriggerEvent / UntriggerEvent)."));

	/** Spawned native actors of each world by UE2 name. */
	TMap<TWeakObjectPtr<const UWorld>, TMap<FString, TWeakObjectPtr<AActor>>>& NamedActors()
	{
		static TMap<TWeakObjectPtr<const UWorld>, TMap<FString, TWeakObjectPtr<AActor>>> M;
		return M;
	}
}

// ---------------------------------------------------------------------------------------------
// FArenaUE2Props
// ---------------------------------------------------------------------------------------------

FArenaUE2Props FArenaUE2Props::Merge(const TSharedPtr<FJsonObject>& Defaults, const TSharedPtr<FJsonObject>& Instance)
{
	FArenaUE2Props Out;
	Out.Obj = MakeShared<FJsonObject>();
	if (Defaults.IsValid())
	{
		Out.Obj->Values = Defaults->Values;
	}
	if (Instance.IsValid())
	{
		for (const auto& KV : Instance->Values)
		{
			Out.Obj->Values.Add(KV.Key, KV.Value);
		}
	}
	return Out;
}

bool FArenaUE2Props::Has(const FString& Key) const
{
	return Obj.IsValid() && Obj->HasField(Key);
}

TSharedPtr<FJsonValue> FArenaUE2Props::Value(const FString& Key) const
{
	return Obj.IsValid() ? Obj->TryGetField(Key) : nullptr;
}

TSharedPtr<FJsonValue> FArenaUE2Props::Indexed(const FString& Key, int32 Index) const
{
	return Value(Index == 0 ? Key : FString::Printf(TEXT("%s[%d]"), *Key, Index));
}

double FArenaUE2Props::Num(const FString& Key, double Default) const
{
	const TSharedPtr<FJsonValue> V = Value(Key);
	double Out = Default;
	if (V.IsValid())
	{
		bool b = false;
		if (!V->TryGetNumber(Out))
		{
			Out = V->TryGetBool(b) ? (b ? 1.0 : 0.0) : Default;
		}
	}
	return Out;
}

bool FArenaUE2Props::Bool(const FString& Key, bool bDefault) const
{
	const TSharedPtr<FJsonValue> V = Value(Key);
	if (!V.IsValid())
	{
		return bDefault;
	}
	bool b = bDefault;
	double N = 0.0;
	if (V->TryGetBool(b))
	{
		return b;
	}
	return V->TryGetNumber(N) ? N != 0.0 : bDefault;
}

FString FArenaUE2Props::ToString(const TSharedPtr<FJsonValue>& V)
{
	FString S;
	if (V.IsValid() && V->TryGetString(S) && S != TEXT("None"))
	{
		return S;
	}
	return FString();
}

FString FArenaUE2Props::Str(const FString& Key, const FString& Default) const
{
	const FString S = ToString(Value(Key));
	return S.IsEmpty() ? Default : S;
}

FName FArenaUE2Props::Name(const FString& Key) const
{
	const FString S = Str(Key);
	return S.IsEmpty() ? NAME_None : FName(*S);
}

FVector FArenaUE2Props::ToVector(const TSharedPtr<FJsonValue>& V, const FVector& Default)
{
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (V.IsValid() && V->TryGetArray(Arr) && Arr->Num() >= 3)
	{
		return FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
	}
	const TSharedPtr<FJsonObject>* O = nullptr;
	if (V.IsValid() && V->TryGetObject(O))
	{
		return FVector((*O)->GetNumberField(TEXT("X")), (*O)->GetNumberField(TEXT("Y")), (*O)->GetNumberField(TEXT("Z")));
	}
	return Default;
}

FVector FArenaUE2Props::Vec(const FString& Key, const FVector& Default) const
{
	return ToVector(Value(Key), Default);
}

FRotator FArenaUE2Props::ToRotator(const TSharedPtr<FJsonValue>& V)
{
	const TSharedPtr<FJsonObject>* O = nullptr;
	if (V.IsValid() && V->TryGetObject(O))
	{
		const double K = 360.0 / 65536.0;
		return FRotator((*O)->GetNumberField(TEXT("Pitch")) * K, (*O)->GetNumberField(TEXT("Yaw")) * K, (*O)->GetNumberField(TEXT("Roll")) * K);
	}
	return FRotator::ZeroRotator;
}

FRotator FArenaUE2Props::Rot(const FString& Key, const FRotator& Default) const
{
	const TSharedPtr<FJsonValue> V = Value(Key);
	return V.IsValid() ? ToRotator(V) : Default;
}

FColor FArenaUE2Props::Color(const FString& Key, const FColor& Default) const
{
	const TSharedPtr<FJsonObject> O = Object(Key);
	if (!O.IsValid())
	{
		return Default;
	}
	auto C = [&O](const TCHAR* F, uint8 D) { double V = D; O->TryGetNumberField(F, V); return uint8(FMath::Clamp(int32(V), 0, 255)); };
	return FColor(C(TEXT("R"), Default.R), C(TEXT("G"), Default.G), C(TEXT("B"), Default.B), C(TEXT("A"), Default.A));
}

FVector2D FArenaUE2Props::Range(const FString& Key, const FVector2D& Default) const
{
	const TSharedPtr<FJsonValue> V = Value(Key);
	if (!V.IsValid())
	{
		return Default;
	}
	double N = 0.0;
	if (V->TryGetNumber(N))
	{
		return FVector2D(N, N);
	}
	const TSharedPtr<FJsonObject>* O = nullptr;
	if (V->TryGetObject(O))
	{
		double Mn = Default.X, Mx = Default.Y;
		(*O)->TryGetNumberField(TEXT("Min"), Mn);
		(*O)->TryGetNumberField(TEXT("Max"), Mx);
		return FVector2D(Mn, Mx);
	}
	return Default;
}

void FArenaUE2Props::RangeVector(const FString& Key, FVector& OutMin, FVector& OutMax, const FVector& Default) const
{
	OutMin = OutMax = Default;
	const TSharedPtr<FJsonObject> O = Object(Key);
	if (!O.IsValid())
	{
		return;
	}
	const TCHAR* Axes[3] = { TEXT("X"), TEXT("Y"), TEXT("Z") };
	for (int32 i = 0; i < 3; ++i)
	{
		const TSharedPtr<FJsonObject>* R = nullptr;
		if (O->TryGetObjectField(Axes[i], R))
		{
			double Mn = Default[i], Mx = Default[i];
			(*R)->TryGetNumberField(TEXT("Min"), Mn);
			(*R)->TryGetNumberField(TEXT("Max"), Mx);
			OutMin[i] = Mn;
			OutMax[i] = Mx;
		}
	}
}

TSharedPtr<FJsonObject> FArenaUE2Props::Object(const FString& Key) const
{
	const TSharedPtr<FJsonValue> V = Value(Key);
	const TSharedPtr<FJsonObject>* O = nullptr;
	return V.IsValid() && V->TryGetObject(O) ? *O : nullptr;
}

const TArray<TSharedPtr<FJsonValue>>& FArenaUE2Props::Array(const FString& Key) const
{
	const TSharedPtr<FJsonValue> V = Value(Key);
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	return V.IsValid() && V->TryGetArray(Arr) ? *Arr : ArenaUE2WorldLocal::EmptyArray();
}

// ---------------------------------------------------------------------------------------------
// FArenaUE2ActorDesc
// ---------------------------------------------------------------------------------------------

bool FArenaUE2ActorDesc::IsA(const FString& UE2Class) const
{
	for (const FString& C : ClassChain)
	{
		if (C.Equals(UE2Class, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return Class.Equals(UE2Class, ESearchCase::IgnoreCase);
}

bool FArenaUE2ActorDesc::GetSubobject(const FString& Ref, FString& OutClass, TArray<FString>& OutChain, FArenaUE2Props& OutProps) const
{
	const TSharedPtr<FJsonObject>* Subs = nullptr;
	const TSharedPtr<FJsonObject>* Sub = nullptr;
	if (!Json.IsValid() || !Json->TryGetObjectField(TEXT("subobjects"), Subs) || !(*Subs)->TryGetObjectField(Ref, Sub))
	{
		return false;
	}
	(*Sub)->TryGetStringField(TEXT("class"), OutClass);
	OutChain = ArenaUE2WorldLocal::StringArray(*Sub, TEXT("class_chain"));
	TSharedPtr<FJsonObject> Defaults;
	const TSharedPtr<FJsonObject>* Def = nullptr;
	if (ClassDefaults.IsValid() && ClassDefaults->TryGetObjectField(OutClass, Def))
	{
		Defaults = *Def;
	}
	const TSharedPtr<FJsonObject>* SubProps = nullptr;
	OutProps = FArenaUE2Props::Merge(Defaults, (*Sub)->TryGetObjectField(TEXT("properties"), SubProps) ? *SubProps : nullptr);
	return true;
}

FTransform FArenaUE2ActorDesc::MeshTransform(const FVector& InLocation, const FRotator& InRotation) const
{
	const FVector S = DrawScale3D * DrawScale;
	FVector Loc = InLocation;
	if (!PrePivot.IsNearlyZero(1e-4))
	{
		Loc -= InRotation.RotateVector(S * PrePivot);
	}
	return FTransform(InRotation, Loc, S);
}

// ---------------------------------------------------------------------------------------------
// UArenaEventSubsystem
// ---------------------------------------------------------------------------------------------

UArenaEventSubsystem* UArenaEventSubsystem::Get(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	return World ? World->GetSubsystem<UArenaEventSubsystem>() : nullptr;
}

bool UArenaEventSubsystem::IsLogging()
{
	static const bool bCmd = FParse::Param(FCommandLine::Get(), TEXT("LogUE2Events"));
	return bCmd || ArenaUE2WorldLocal::CVarLogEvents.GetValueOnGameThread() != 0;
}

void UArenaEventSubsystem::RegisterActor(AActor* Actor, FName Tag)
{
	if (!Actor || Tag.IsNone() || !Actor->Implements<UArenaUE2Actor>())
	{
		return;
	}
	Tagged.FindOrAdd(Tag).AddUnique(Actor);
}

void UArenaEventSubsystem::UnregisterActor(AActor* Actor)
{
	for (TPair<FName, TArray<TWeakObjectPtr<AActor>>>& KV : Tagged)
	{
		KV.Value.Remove(Actor);
	}
}

TArray<AActor*> UArenaEventSubsystem::GetTaggedActors(FName Tag) const
{
	TArray<AActor*> Out;
	if (const TArray<TWeakObjectPtr<AActor>>* L = Tagged.Find(Tag))
	{
		for (const TWeakObjectPtr<AActor>& A : *L)
		{
			if (A.IsValid())
			{
				Out.Add(A.Get());
			}
		}
	}
	return Out;
}

void UArenaEventSubsystem::TriggerEvent(FName Event, AActor* Other, APawn* EventInstigator)
{
	if (Event.IsNone() || Depth > 32)
	{
		return;
	}
	if (IsLogging())
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("UE2Event: trigger '%s' by %s (instigator %s), %d receiver(s)"), *Event.ToString(), *GetNameSafe(Other),
			*GetNameSafe(EventInstigator), GetTaggedActors(Event).Num());
	}
	++Depth;
	for (AActor* A : GetTaggedActors(Event))
	{
		if (IArenaUE2Actor* R = Cast<IArenaUE2Actor>(A))
		{
			R->Trigger(Other, EventInstigator);
		}
	}
	OnEvent.Broadcast(Event, Other, EventInstigator, true);
	--Depth;
}

void UArenaEventSubsystem::UntriggerEvent(FName Event, AActor* Other, APawn* EventInstigator)
{
	if (Event.IsNone() || Depth > 32)
	{
		return;
	}
	if (IsLogging())
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("UE2Event: untrigger '%s' by %s"), *Event.ToString(), *GetNameSafe(Other));
	}
	++Depth;
	for (AActor* A : GetTaggedActors(Event))
	{
		if (IArenaUE2Actor* R = Cast<IArenaUE2Actor>(A))
		{
			R->UnTrigger(Other, EventInstigator);
		}
	}
	OnEvent.Broadcast(Event, Other, EventInstigator, false);
	--Depth;
}

// ---------------------------------------------------------------------------------------------
// ArenaUE2World
// ---------------------------------------------------------------------------------------------

void ArenaUE2World::RegisterClass(const FString& UE2Class, UClass* NativeClass)
{
	ArenaUE2WorldLocal::RegisterBuiltins();
	if (NativeClass && NativeClass->ImplementsInterface(UArenaUE2Actor::StaticClass()))
	{
		ArenaUE2WorldLocal::Registry().Add(UE2Class.ToLower(), NativeClass);
	}
}

UClass* ArenaUE2World::FindClass(const TArray<FString>& ClassChain)
{
	using namespace ArenaUE2WorldLocal;
	RegisterBuiltins();
	if (IsExcludedChain(ClassChain))
	{
		return nullptr;
	}
	for (const FString& C : ClassChain)
	{
		if (UClass** Found = Registry().Find(C.ToLower()))
		{
			return *Found;
		}
	}
	return nullptr;
}

TSet<int32> ArenaUE2World::GetHandledActorIndices(const TSharedPtr<FJsonObject>& Doc)
{
	TSet<int32> Out;
	const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
	if (!Doc.IsValid() || !Doc->TryGetArrayField(TEXT("actors"), Actors))
	{
		return Out;
	}
	for (const TSharedPtr<FJsonValue>& V : *Actors)
	{
		const TSharedPtr<FJsonObject> A = V->AsObject();
		UClass* C = A.IsValid() ? FindClass(ArenaUE2WorldLocal::StringArray(A, TEXT("class_chain"))) : nullptr;
		if (C && C->IsChildOf(AArenaMover::StaticClass()))
		{
			Out.Add(int32(A->GetNumberField(TEXT("index"))));
		}
	}
	return Out;
}

bool ArenaUE2World::HasNativeMovers(const UWorld* World)
{
	if (!World)
	{
		return false;
	}
	TActorIterator<AArenaMover> It(const_cast<UWorld*>(World));
	return bool(It);
}

void ArenaUE2World::ResetWorldActors(UWorld* World)
{
	if (!World)
	{
		return;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (IArenaUE2Actor* U = Cast<IArenaUE2Actor>(*It))
		{
			U->ResetUE2();
		}
	}
}

namespace ArenaUE2WorldLocal
{
	TMap<TWeakObjectPtr<const UWorld>, TArray<ArenaUE2World::FJumpPadInfo>>& JumpPads()
	{
		static TMap<TWeakObjectPtr<const UWorld>, TArray<ArenaUE2World::FJumpPadInfo>> M;
		return M;
	}
	TMap<TWeakObjectPtr<const UWorld>, TArray<ArenaUE2World::FForcedPath>>& ForcedPaths()
	{
		static TMap<TWeakObjectPtr<const UWorld>, TArray<ArenaUE2World::FForcedPath>> M;
		return M;
	}
	TMap<TWeakObjectPtr<const UWorld>, TArray<FString>>& NavDoors()
	{
		static TMap<TWeakObjectPtr<const UWorld>, TArray<FString>> M;
		return M;
	}
	TMap<TWeakObjectPtr<const UWorld>, TArray<ArenaUE2World::FAssaultPath>>& AssaultPaths()
	{
		static TMap<TWeakObjectPtr<const UWorld>, TArray<ArenaUE2World::FAssaultPath>> M;
		return M;
	}
}

TArray<FString> ArenaUE2World::GetNavDoorMovers(const UWorld* World)
{
	const TArray<FString>* Doors = ArenaUE2WorldLocal::NavDoors().Find(World);
	return Doors ? *Doors : TArray<FString>();
}

TArray<ArenaUE2World::FAssaultPath> ArenaUE2World::GetAssaultPaths(const UWorld* World)
{
	const TArray<FAssaultPath>* Paths = ArenaUE2WorldLocal::AssaultPaths().Find(World);
	return Paths ? *Paths : TArray<FAssaultPath>();
}

TArray<ArenaUE2World::FForcedPath> ArenaUE2World::GetForcedPaths(const UWorld* World)
{
	const TArray<FForcedPath>* Paths = ArenaUE2WorldLocal::ForcedPaths().Find(World);
	return Paths ? *Paths : TArray<FForcedPath>();
}

const ArenaUE2World::FJumpPadInfo* ArenaUE2World::FindJumpPad(const UWorld* World, const FVector& Location, float MaxDist)
{
	const TArray<FJumpPadInfo>* Pads = ArenaUE2WorldLocal::JumpPads().Find(World);
	const FJumpPadInfo* Best = nullptr;
	if (!Pads)
	{
		return Best;
	}
	double BestDist = FMath::Square(double(MaxDist));
	for (const FJumpPadInfo& P : *Pads)
	{
		const double D = FVector::DistSquared(P.Location, Location);
		if (D <= BestDist)
		{
			BestDist = D;
			Best = &P;
		}
	}
	return Best;
}

AActor* ArenaUE2World::FindActorByName(const UWorld* World, const FString& UE2Name)
{
	const TMap<FString, TWeakObjectPtr<AActor>>* M = ArenaUE2WorldLocal::NamedActors().Find(World);
	const TWeakObjectPtr<AActor>* A = M ? M->Find(UE2Name) : nullptr;
	return A ? A->Get() : nullptr;
}

TSet<int32> ArenaUE2World::SpawnWorldActors(UWorld* World, const TSharedPtr<FJsonObject>& Doc, const FString& MapDir)
{
	using namespace ArenaUE2WorldLocal;
	TSet<int32> Handled;
	if (!World || !Doc.IsValid())
	{
		return Handled;
	}
	const double T0 = FPlatformTime::Seconds();
	const TSharedPtr<FJsonObject>* DefaultsPtr = nullptr;
	const TSharedPtr<FJsonObject> ClassDefaults = Doc->TryGetObjectField(TEXT("class_defaults"), DefaultsPtr) ? *DefaultsPtr : nullptr;
	const TSharedPtr<FJsonObject>* MarkersPtr = nullptr;
	const TSharedPtr<FJsonObject> Markers = Doc->TryGetObjectField(TEXT("markers"), MarkersPtr) ? *MarkersPtr : nullptr;

	// markers by actor index (volumes carry the brush geometry)
	TMap<int32, TSharedPtr<FJsonObject>> MarkerByIndex;
	for (const TCHAR* List : { TEXT("volumes"), TEXT("zones"), TEXT("movers"), TEXT("teleporters") })
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Markers.IsValid() && Markers->TryGetArrayField(List, Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				const TSharedPtr<FJsonObject> M = V->AsObject();
				double Idx = -1;
				if (M.IsValid() && M->TryGetNumberField(TEXT("actor_index"), Idx))
				{
					MarkerByIndex.FindOrAdd(int32(Idx), M);
				}
			}
		}
	}

	// zones: BSP tree
	UArenaWorldZoneSubsystem* Zones = World->GetSubsystem<UArenaWorldZoneSubsystem>();
	const TSharedPtr<FJsonObject>* Bsp = nullptr;
	const TSharedPtr<FJsonObject>* Tree = nullptr;
	if (Zones && Doc->TryGetObjectField(TEXT("bsp"), Bsp) && (*Bsp)->TryGetObjectField(TEXT("tree"), Tree))
	{
		FString File;
		(*Tree)->TryGetStringField(TEXT("file"), File);
		Zones->LoadBspTree(MapDir / File, *Tree);
	}

	// level gravity (LevelInfo.DefaultGravity relative to the stock -950)
	const TSharedPtr<FJsonObject>* Level = nullptr;
	double DefaultGravity = UE2DefaultGravity;
	if (Doc->TryGetObjectField(TEXT("level_info"), Level) && (*Level)->TryGetNumberField(TEXT("default_gravity"), DefaultGravity)
		&& DefaultGravity < 0.0 && !FMath::IsNearlyEqual(DefaultGravity, double(UE2DefaultGravity), 1.0))
	{
		if (AWorldSettings* WS = World->GetWorldSettings())
		{
			WS->bWorldGravitySet = true;
			WS->WorldGravityZ = UPhysicsSettings::Get()->DefaultGravityZ * float(DefaultGravity / UE2DefaultGravity);
			UE_LOG(LogOpenUT2k4, Log, TEXT("UE2World: level gravity %.0f (UE2 %.0f)"), WS->WorldGravityZ, DefaultGravity);
		}
	}

	// ---- spawn ----
	const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
	if (!Doc->TryGetArrayField(TEXT("actors"), Actors))
	{
		return Handled;
	}
	TMap<FString, TWeakObjectPtr<AActor>>& Named = NamedActors().FindOrAdd(World);
	Named.Reset();
	TArray<FJumpPadInfo>& PadInfos = JumpPads().FindOrAdd(World);
	PadInfos.Reset();
	for (auto It = JumpPads().CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}
	// navigation points and their ForcedPaths (resolved after the loop)
	struct FNavPt
	{
		FVector Location;
		bool bJumpDest = false;
		bool bOnlyTranslocator = false;
		bool bNoLowGrav = false;
		float TranslocZOffset = 0.f;
		float Radius = 80.f;
		float Height = 86.f;
	};
	TMap<FString, FNavPt> NavPts;
	TArray<TPair<FString, FString>> Forced;
	for (auto It = ForcedPaths().CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}
	ForcedPaths().FindOrAdd(World).Reset();
	for (auto It = AssaultPaths().CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}
	TArray<FAssaultPath>& APaths = AssaultPaths().FindOrAdd(World);
	APaths.Reset();
	for (auto It = NavDoors().CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}
	TArray<FString>& DoorMovers = NavDoors().FindOrAdd(World);
	DoorMovers.Reset();
	TArray<FName> APathTags;
	TMap<FName, FVector> ObjectiveByTag;
	TArray<AActor*> Spawned;
	TMap<FString, int32> Counts;
	FVector SkyLocation = FVector::ZeroVector;
	bool bSkyZone = false;
	for (const TSharedPtr<FJsonValue>& V : *Actors)
	{
		FArenaUE2ActorDesc D;
		if (!ParseDesc(V->AsObject(), ClassDefaults, D))
		{
			continue;
		}
		if (D.IsA(TEXT("NavigationPoint")))
		{
			FNavPt& Pt = NavPts.Add(D.Name, { D.Location, D.IsA(TEXT("JumpDest")) });
			Pt.bOnlyTranslocator = D.Props.Bool(TEXT("bOnlyTranslocator"));
			Pt.bNoLowGrav = D.Props.Bool(TEXT("bNoLowGrav"));
			Pt.TranslocZOffset = float(D.Props.Num(TEXT("TranslocZOffset"))) * Scale;
			Pt.Radius = D.CollisionRadius > 0.f ? D.CollisionRadius : Pt.Radius;
			Pt.Height = D.CollisionHeight > 0.f ? D.CollisionHeight : Pt.Height;
			for (int32 i = 0; i < 16; ++i)
			{
				const FString Target = FArenaUE2Props::ToString(D.Props.Indexed(TEXT("ForcedPaths"), i));
				if (!Target.IsEmpty() && Target != TEXT("None"))
				{
					Forced.Emplace(D.Name, Target);
				}
			}
		}
		// AssaultPaths (alternate routes of the squad AI) and the objectives they lead to
		if (D.IsA(TEXT("AssaultPath")))
		{
			FAssaultPath& A = APaths.AddDefaulted_GetRef();
			A.Location = D.Location;
			A.bReturnOnly = D.Props.Bool(TEXT("bReturnOnly"));
			A.bNoReturn = D.Props.Bool(TEXT("bNoReturn"));
			A.Priority = float(D.Props.Num(TEXT("Priority"), 0.5));
			APathTags.Add(D.Props.Name(TEXT("ObjectiveTag")));
		}
		if (D.IsA(TEXT("Door")))
		{
			FString MyDoor = D.Props.Str(TEXT("MyDoor"));
			int32 DoorDot = INDEX_NONE;
			if (MyDoor.FindLastChar(TEXT('.'), DoorDot))
			{
				MyDoor.RightChopInline(DoorDot + 1);
			}
			if (!MyDoor.IsEmpty() && MyDoor != TEXT("None"))
			{
				DoorMovers.AddUnique(MyDoor);
			}
		}
		if (D.IsA(TEXT("GameObjective")) && D.Tag != NAME_None)
		{
			ObjectiveByTag.Add(D.Tag, D.Location);
		}
		// jump pads: their UE2 collision cylinder and JumpVelocity for AArenaLevelBuilder's pads
		if (D.IsA(TEXT("JumpPad")))
		{
			FJumpPadInfo& P = JumpPads().FindOrAdd(World).AddDefaulted_GetRef();
			P.Name = D.Name;
			P.Location = D.Location;
			P.Radius = D.CollisionRadius > 0.f ? D.CollisionRadius : P.Radius;
			P.HalfHeight = D.CollisionHeight > 0.f ? D.CollisionHeight : P.HalfHeight;
			P.JumpVelocity = D.Props.Vec(TEXT("JumpVelocity"), FVector::ZeroVector) * Scale;
		}
		// the level's DefaultPhysicsVolume: TerminalVelocity and gravity of everything outside volumes. Its gravity
		// starts as LevelInfo.DefaultGravity (above); a Gravity saved with the map's volume wins, as in UE2 where
		// pawns outside other volumes use PhysicsVolume.Gravity (BR-Skyline: -600).
		if (D.IsA(TEXT("DefaultPhysicsVolume")))
		{
			if (APhysicsVolume* DPV = World->GetDefaultPhysicsVolume())
			{
				DPV->TerminalVelocity = float(D.Props.Num(TEXT("TerminalVelocity"), 2500.0)) * Scale;
			}
			const FVector G = D.Props.Vec(TEXT("Gravity"), FVector::ZeroVector);
			AWorldSettings* WS = World->GetWorldSettings();
			if (WS && D.Props.Has(TEXT("Gravity")) && G.Z < 0.f && !FMath::IsNearlyEqual(G.Z, double(UE2DefaultGravity), 1.0))
			{
				WS->bWorldGravitySet = true;
				WS->WorldGravityZ = UPhysicsSettings::Get()->DefaultGravityZ * float(G.Z / UE2DefaultGravity);
				UE_LOG(LogOpenUT2k4, Log, TEXT("UE2World: level gravity %.0f (UE2 %.0f, DefaultPhysicsVolume)"), WS->WorldGravityZ, G.Z);
			}
		}
		UClass* NativeClass = FindClass(D.ClassChain);
		const FString Ambient = D.Props.Str(TEXT("AmbientSound"));
		if (!NativeClass && Ambient.IsEmpty())
		{
			continue;
		}
		if (const TSharedPtr<FJsonObject>* M = MarkerByIndex.Find(D.Index))
		{
			D.Marker = *M;
		}
		AActor* Actor = nullptr;
		if (NativeClass)
		{
			const FTransform Xf(D.Rotation, D.Location);
			Actor = World->SpawnActorDeferred<AActor>(NativeClass, Xf, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (!Actor)
			{
				continue;
			}
			if (IArenaUE2Actor* U = Cast<IArenaUE2Actor>(Actor))
			{
				U->InitFromUE2(D);
			}
			Actor->FinishSpawning(Xf);
			Spawned.Add(Actor);
			Named.Add(D.Name, Actor);
			Counts.FindOrAdd(NativeClass->GetName())++;
			if (Actor->IsA<AArenaMover>())
			{
				Handled.Add(D.Index);
			}
			if (Actor->IsA<AArenaSkyZoneInfo>())
			{
				bSkyZone = true;
				SkyLocation = D.Location;
			}
		}
		// ambient sound of any other actor (movers, static meshes, emitters ...): an attached sound
		if (!Ambient.IsEmpty() && !(Actor && Actor->IsA<AArenaAmbientSound>()))
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			if (AArenaAmbientSound* S = World->SpawnActor<AArenaAmbientSound>(AArenaAmbientSound::StaticClass(), D.Location, D.Rotation, Params))
			{
				S->InitFromUE2(D);
				if (Actor)
				{
					S->AttachToActor(Actor, FAttachmentTransformRules::KeepWorldTransform);
				}
				Counts.FindOrAdd(TEXT("AttachedAmbientSound"))++;
			}
		}
	}

	// register tags, then let everything resolve its references
	UArenaEventSubsystem* Events = World->GetSubsystem<UArenaEventSubsystem>();
	for (AActor* A : Spawned)
	{
		if (IArenaUE2Actor* U = Cast<IArenaUE2Actor>(A))
		{
			if (Events)
			{
				Events->RegisterActor(A, U->GetUE2Tag());
			}
		}
	}
	for (AActor* A : Spawned)
	{
		if (IArenaUE2Actor* U = Cast<IArenaUE2Actor>(A))
		{
			U->PostInitUE2();
		}
	}

	for (int32 i = 0; i < APaths.Num(); ++i)
	{
		if (const FVector* O = ObjectiveByTag.Find(APathTags[i]))
		{
			APaths[i].ObjectiveLocation = *O;
			APaths[i].bHasObjective = true;
		}
	}
	{
		TArray<FForcedPath>& Paths = ForcedPaths().FindOrAdd(World);
		for (const TPair<FString, FString>& F : Forced)
		{
			// object references may come as "Map.Name"
			FString TargetName = F.Value;
			int32 Dot = INDEX_NONE;
			if (TargetName.FindLastChar(TEXT('.'), Dot))
			{
				TargetName.RightChopInline(Dot + 1);
			}
			const FNavPt* A = NavPts.Find(F.Key);
			const FNavPt* B = NavPts.Find(TargetName);
			if (A && B)
			{
				ArenaUE2World::FForcedPath& Path = Paths.Add_GetRef({ A->Location, B->Location, B->bJumpDest });
				Path.bOnlyTranslocator = B->bOnlyTranslocator;
				Path.bNoLowGrav = B->bNoLowGrav;
				Path.TranslocZOffset = B->TranslocZOffset;
				Path.TargetRadius = B->Radius;
				Path.TargetHeight = B->Height;
			}
		}
	}

	// collision of the meshes the native actors built (blocking volumes) or use (movers)
	TArray<UStaticMesh*> Cook;
	for (AActor* A : Spawned)
	{
		TInlineComponentArray<UStaticMeshComponent*> Comps(A);
		for (UStaticMeshComponent* C : Comps)
		{
			if (C->GetStaticMesh())
			{
				Cook.AddUnique(C->GetStaticMesh());
			}
		}
	}
	ArenaRuntimeContent::CookCollision(Cook);
	int32 Blocking = 0, BlockingOk = 0;
	for (AActor* A : Spawned)
	{
		if (AArenaBlockingVolume* B = Cast<AArenaBlockingVolume>(A))
		{
			B->ApplyCollision();
			++Blocking;
			const UBodySetup* Body = B->Collision->GetStaticMesh() ? B->Collision->GetStaticMesh()->GetBodySetup() : nullptr;
			BlockingOk += Body && Body->TriMeshGeometries.Num() > 0 ? 1 : 0;
		}
	}
	if (Blocking > 0)
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("UE2World: %d of %d blocking volumes have collision"), BlockingOk, Blocking);
	}

	// sky box: the sky zone's primitives move into it
	int32 SkyPrims = 0;
	if (bSkyZone && Zones && Zones->HasBspTree())
	{
		if (AArenaSkybox* Sky = AArenaSkybox::Spawn(World, SkyLocation))
		{
			static const FName SkyTag(TEXT("ArenaSkyZone"));
			static const FName MapMeshTag(TEXT("ArenaMapMesh"));
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* A = *It;
				const bool bTagged = A->ActorHasTag(SkyTag);
				const bool bPlaced = A->ActorHasTag(MapMeshTag) || A->Implements<UArenaUE2Actor>();
				if (!bTagged && !(bPlaced && Zones->IsInSkyZone(A->GetActorLocation())))
				{
					continue;
				}
				if (A->IsA<AArenaZoneInfo>() || A->IsA<AArenaPhysicsVolume>() || A->IsA<AArenaBlockingVolume>())
				{
					continue;
				}
				TInlineComponentArray<UPrimitiveComponent*> Prims(A);
				for (UPrimitiveComponent* P : Prims)
				{
					Sky->AddSkyPrimitive(P);
					++SkyPrims;
				}
				// emitters create their particle components in BeginPlay: they register themselves later
				if (AArenaEmitter* E = Cast<AArenaEmitter>(A))
				{
					E->Tags.Add(SkyTag);
				}
			}
		}
	}

	FString Summary;
	Counts.KeySort([](const FString& A, const FString& B) { return A < B; });
	for (const TPair<FString, int32>& C : Counts)
	{
		Summary += FString::Printf(TEXT("%s%s %d"), Summary.IsEmpty() ? TEXT("") : TEXT(", "), *C.Key, C.Value);
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("UE2World: %d native actors (%s), zones %s, sky box %s (%d primitives), %.3f s"), Spawned.Num(), *Summary,
		Zones && Zones->HasBspTree() ? TEXT("from BSP") : TEXT("none"), bSkyZone ? TEXT("yes") : TEXT("no"), SkyPrims, FPlatformTime::Seconds() - T0);
	return Handled;
}
