#include "ArenaRules_AS.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "ArenaRuntimeMap.h"
#include "OpenUT2k4.h"
#include "ArenaAS_Info.h"
#include "ArenaAS_Objective.h"
#include "ArenaAssets.h"
#include "ArenaBotController.h"
#include "ArenaCharacter.h"
#include "ArenaGameMode.h"
#include "ArenaGameState.h"
#include "ArenaHUD.h"
#include "ArenaArtHUD.h"
#include "ArenaLevelBuilder.h"
#include "ArenaNavGraph.h"
#include "ArenaONS_Vehicle.h"
#include "ArenaPlayerState.h"
#include "ArenaUE2World.h"
#include "ArenaMover.h"
#include "ArenaTriggers.h"
#include "UScript/ArenaUScript.h"
#include "UScript/UScriptCore.h"
#include "UScript/UScriptRuntime.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Canvas.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "Kismet/GameplayStatics.h"

ARENA_REGISTER_RULES(UArenaRules_AS, "AS", "Assault");

namespace ArenaAS
{
	constexpr float TouchRadius = 220.f;
	constexpr float HoldRadius = 420.f;
	constexpr float UseRadius = 170.f;
	constexpr float DefaultHoldTime = 7.f;
	constexpr float DefaultUseTime = 4.f;
	constexpr float DefaultDestroyHealth = 700.f;
	/** Attacker spawn groups closer than this to the objective they attack are not used. */
	constexpr float MinAttackerSpawnDist = 900.f;
	constexpr float SpawnVoidClearance = 300.f;

	/** True if walking SpawnVoidClearance cm from Floor in any of 8 directions (unless a wall comes first)
	 *  reaches a spot with no floor above kill Z. */
	bool IsNearVoid(UWorld* World, const FVector& Floor, float KillZ)
	{
		FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaASVoidCheck), false);
		const FCollisionObjectQueryParams Static(ECC_WorldStatic);
		const FVector Chest = Floor + FVector(0.f, 0.f, 60.f);
		for (int32 k = 0; k < 8; ++k)
		{
			const FVector Dir = FVector(1.f, 0.f, 0.f).RotateAngleAxis(45.f * k, FVector::UpVector);
			float Reach = SpawnVoidClearance;
			FHitResult Wall;
			if (World->LineTraceSingleByObjectType(Wall, Chest, Chest + Dir * Reach, Static, QP))
			{
				Reach = Wall.Distance - 20.f;
			}
			for (float T = 100.f; T <= Reach; T += 100.f)
			{
				const FVector P = Chest + Dir * T;
				FHitResult Hit;
				if (!World->LineTraceSingleByObjectType(Hit, P, FVector(P.X, P.Y, KillZ), Static, QP) || Hit.ImpactPoint.Z < KillZ + 200.f)
				{
					return true;
				}
			}
		}
		return false;
	}
	/** Bots attacking a destroy objective: max distance and assist damage per second while firing. */
	constexpr float BotDestroyRange = 2200.f;
	constexpr float BotAssistDps = 10.f;

	// ---- tolerant json access (raw marker fields may be top level or inside "properties") ----

	static TSharedPtr<FJsonValue> FindIn(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key)
	{
		if (!Obj.IsValid())
		{
			return nullptr;
		}
		if (const TSharedPtr<FJsonValue>* V = Obj->Values.Find(Key))
		{
			return *V;
		}
		for (const auto& Pair : Obj->Values)
		{
			if (FCString::Stricmp(*Pair.Key, Key) == 0)
			{
				return Pair.Value;
			}
		}
		return nullptr;
	}

	static TSharedPtr<FJsonValue> FindField(const FArenaRawMarker& M, std::initializer_list<const TCHAR*> Keys)
	{
		for (const TCHAR* Key : Keys)
		{
			TSharedPtr<FJsonValue> V = FindIn(M.Json, Key);
			if (!V.IsValid() || V->IsNull())
			{
				V = FindIn(M.Properties, Key);
			}
			if (V.IsValid() && !V->IsNull())
			{
				return V;
			}
		}
		return nullptr;
	}

	static bool GetNum(const FArenaRawMarker& M, std::initializer_list<const TCHAR*> Keys, double& Out)
	{
		const TSharedPtr<FJsonValue> V = FindField(M, Keys);
		if (!V.IsValid())
		{
			return false;
		}
		switch (V->Type)
		{
		case EJson::Number: Out = V->AsNumber(); return true;
		case EJson::Boolean: Out = V->AsBool() ? 1.0 : 0.0; return true;
		case EJson::String:
		{
			const FString S = V->AsString();
			if (S.IsNumeric())
			{
				Out = FCString::Atod(*S);
				return true;
			}
			return false;
		}
		default: return false;
		}
	}

	static bool GetBool(const FArenaRawMarker& M, std::initializer_list<const TCHAR*> Keys, bool& Out)
	{
		const TSharedPtr<FJsonValue> V = FindField(M, Keys);
		if (!V.IsValid())
		{
			return false;
		}
		switch (V->Type)
		{
		case EJson::Boolean: Out = V->AsBool(); return true;
		case EJson::Number: Out = V->AsNumber() != 0.0; return true;
		case EJson::String:
		{
			const FString S = V->AsString();
			Out = S.Equals(TEXT("true"), ESearchCase::IgnoreCase) || S == TEXT("1");
			return true;
		}
		default: return false;
		}
	}

	static FString GetStr(const FArenaRawMarker& M, std::initializer_list<const TCHAR*> Keys)
	{
		const TSharedPtr<FJsonValue> V = FindField(M, Keys);
		FString S;
		if (V.IsValid() && V->Type == EJson::String)
		{
			S = V->AsString();
		}
		return S;
	}

	/** All class names of a marker plus its objective type string, lower case, for keyword checks. */
	static FString ClassText(const FArenaRawMarker& M)
	{
		FString T = M.Class;
		for (const FString& C : M.ClassChain)
		{
			T += TEXT(" ") + C;
		}
		T += TEXT(" ") + GetStr(M, { TEXT("objective_type"), TEXT("ObjectiveType"), TEXT("type") });
		return T.ToLower();
	}

	static bool ContainsAny(const FString& Text, std::initializer_list<const TCHAR*> Words)
	{
		for (const TCHAR* W : Words)
		{
			if (Text.Contains(W, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static FString ReachKey(const FVector& A, const FVector& B)
	{
		return FString::Printf(TEXT("%d,%d,%d|%d,%d,%d"), FMath::RoundToInt(A.X / 25.0), FMath::RoundToInt(A.Y / 25.0), FMath::RoundToInt(A.Z / 25.0),
			FMath::RoundToInt(B.X / 25.0), FMath::RoundToInt(B.Y / 25.0), FMath::RoundToInt(B.Z / 25.0));
	}

	static int32 TeamOf(const AController* C)
	{
		const AArenaPlayerState* PS = C ? C->GetPlayerState<AArenaPlayerState>() : nullptr;
		return PS ? PS->Team : ArenaTeam::None;
	}
}

// =============================================================================================
// Identity
// =============================================================================================

bool UArenaRules_AS::SupportsLayout(const FArenaLayoutInfo& Layout) const
{
	return Layout.GameTypePrefix == TEXT("AS") || Layout.HasFeature(TEXT("raw:assault_objectives"));
}

FString UArenaRules_AS::GetGoalText(int32 Goal) const
{
	return Goal > 1 ? FString::Printf(TEXT("First to %d rounds"), Goal) : FString(TEXT("Best attack wins"));
}

// =============================================================================================
// Setup
// =============================================================================================

void UArenaRules_AS::InitRules(AArenaGameMode* InGameMode, const FString& Options)
{
	Super::InitRules(InGameMode, Options);
	const int32 Minutes = UGameplayStatics::GetIntOption(Options, TEXT("Time"), GetDefaultTimeLimit());
	RoundSeconds = Minutes > 0 ? Minutes * 60 : 600;
	DebugStartStage = FMath::Max(0, UGameplayStatics::GetIntOption(Options, TEXT("StartStage"), 0));
	const int32 Override = UGameplayStatics::GetIntOption(Options, TEXT("RoundTime"), 0);
	if (Override > 0)
	{
		RoundSeconds = FMath::Clamp(Override, 20, 3600);
	}
}

FString UArenaRules_AS::MakeDescription(EArenaASObjectiveType Type, int32 InStage, bool bOptional, int32 Salt) const
{
	static const TCHAR* TouchNames[] = { TEXT("Breach Marker"), TEXT("Forward Beacon"), TEXT("Survey Post"), TEXT("Signal Pylon"), TEXT("Landing Pad") };
	static const TCHAR* HoldNames[] = { TEXT("Uplink Zone"), TEXT("Staging Ground"), TEXT("Relay Platform"), TEXT("Choke Point"), TEXT("Drop Zone") };
	static const TCHAR* UseNames[] = { TEXT("Override Console"), TEXT("Gate Terminal"), TEXT("Bridge Controls"), TEXT("Lock Panel"), TEXT("Pump Station") };
	static const TCHAR* DestroyNames[] = { TEXT("Shield Generator"), TEXT("Power Conduit"), TEXT("Defense Core"), TEXT("Coolant Tower"), TEXT("Field Emitter") };
	const int32 Pick = FMath::Abs(Salt * 3 + InStage) % 5;
	switch (Type)
	{
	case EArenaASObjectiveType::Touch:   return FString::Printf(TEXT("Reach the %s"), TouchNames[Pick]);
	case EArenaASObjectiveType::Hold:    return FString::Printf(TEXT("Secure the %s"), HoldNames[Pick]);
	case EArenaASObjectiveType::Use:     return FString::Printf(TEXT("Operate the %s"), UseNames[Pick]);
	case EArenaASObjectiveType::Destroy: return FString::Printf(TEXT("Destroy the %s"), DestroyNames[Pick]);
	}
	return TEXT("Objective");
}

namespace ArenaASLocal
{
	/** Actors of the map's json (<cache>/maps/<Map>/map.json; developer fallback: the project's ImportCache) by name. */
	TMap<FString, TSharedPtr<FJsonObject>> LoadMapActors(const FString& MapName)
	{
		TMap<FString, TSharedPtr<FJsonObject>> Out;
		if (MapName.IsEmpty())
		{
			return Out;
		}
		FString Path = ArenaRuntimeMap::GetMapDir(MapName) / TEXT("map.json");
		if (!FPaths::FileExists(Path))
		{
			Path = FPaths::ProjectDir() / TEXT("ImportCache") / TEXT("maps") / MapName / TEXT("map.json"); // dev fallback (cooked imports)
		}
		FString Text;
		TSharedPtr<FJsonObject> Root;
		if (!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("AS: no map json for %s, objectives keep their stand-in models"), *MapName);
			return Out;
		}
		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		if (Root->TryGetArrayField(TEXT("actors"), Actors))
		{
			for (const TSharedPtr<FJsonValue>& V : *Actors)
			{
				const TSharedPtr<FJsonObject> A = V.IsValid() ? V->AsObject() : nullptr;
				FString Name;
				if (A.IsValid() && A->TryGetStringField(TEXT("name"), Name))
				{
					bool bObjective = false;
					const TArray<TSharedPtr<FJsonValue>>* Chain = nullptr;
					if (A->TryGetArrayField(TEXT("class_chain"), Chain))
					{
						for (const TSharedPtr<FJsonValue>& C : *Chain)
						{
							bObjective |= C->AsString() == TEXT("GameObjective");
						}
					}
					if (bObjective)
					{
						Out.Add(Name, A);
					}
				}
			}
		}
		return Out;
	}

	FVector JsonVector(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const FVector& Default)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Obj.IsValid() && Obj->TryGetArrayField(Field, Arr) && Arr->Num() >= 3)
		{
			return FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		}
		return Default;
	}

	/** The actor's static mesh and world transform (same transform as ArenaRuntimeMap's static mesh actors). */
	bool FindMapActorMesh(const TMap<FString, TSharedPtr<FJsonObject>>& Actors, const FString& Name, FString& OutMesh, FTransform& OutTM, FString& OutDestroyed)
	{
		const TSharedPtr<FJsonObject>* Found = Actors.Find(Name);
		if (!Found || !Found->IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonObject>& A = *Found;
		bool bHidden = false;
		A->TryGetBoolField(TEXT("hidden"), bHidden);
		FString Mesh;
		if (bHidden || !A->TryGetStringField(TEXT("static_mesh"), Mesh) || Mesh.IsEmpty() || Mesh == TEXT("None"))
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* RotObj = nullptr;
		FRotator Rot = FRotator::ZeroRotator;
		if (A->TryGetObjectField(TEXT("rotation"), RotObj))
		{
			Rot = FRotator((*RotObj)->GetNumberField(TEXT("pitch")), (*RotObj)->GetNumberField(TEXT("yaw")), (*RotObj)->GetNumberField(TEXT("roll")));
		}
		double DrawScale = 1.0;
		A->TryGetNumberField(TEXT("draw_scale"), DrawScale);
		const FVector Scale = JsonVector(A, TEXT("draw_scale3d"), FVector(1.0)) * (DrawScale == 0.0 ? 1.0 : DrawScale);
		FVector Loc = JsonVector(A, TEXT("location"), FVector::ZeroVector);
		Loc -= Rot.RotateVector(Scale * JsonVector(A, TEXT("pre_pivot"), FVector::ZeroVector));
		OutMesh = Mesh;
		OutTM = FTransform(Rot, Loc, Scale);
		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (A->TryGetObjectField(TEXT("properties"), Props))
		{
			(*Props)->TryGetStringField(TEXT("DestroyedStaticMesh"), OutDestroyed);
			if (OutDestroyed == TEXT("None") || OutDestroyed == Mesh)
			{
				OutDestroyed.Empty();
			}
		}
		return true;
	}
}

namespace ArenaASLocal
{
	/** A script class default as text ("Class'Pkg.Name'" and "Pkg.Name" are returned as "Pkg.Name"). */
	FString ScriptDefault(FUSClass* Class, const TCHAR* Key)
	{
		FUSObject* Def = Class ? Class->GetDefaultObject() : nullptr;
		FUSProperty* P = Def && Def->Data ? Def->FindProperty(FName(Key)) : nullptr;
		FString Out;
		if (P)
		{
			P->ExportText(Out, Def->Data + P->Offset, true);
		}
		int32 Quote = INDEX_NONE;
		if (Out.FindChar(TEXT('\''), Quote) && Out.EndsWith(TEXT("'")))
		{
			Out = Out.Mid(Quote + 1, Out.Len() - Quote - 2);
		}
		return Out;
	}

	/**
	 * A DestroyVehicleObjective's vehicle: the map's Assault vehicle factory whose VehicleEvent is the objective's Tag,
	 * its VehicleClass's skeletal mesh (from the UScript class defaults) and the factory's transform.
	 */
	bool FindObjectiveVehicle(const FString& MapName, const FString& Tag, FString& OutMesh, FTransform& OutTM)
	{
		if (Tag.IsEmpty() || Tag == TEXT("None") || !ArenaUScript::EnsureRuntime())
		{
			return false;
		}
		FString Text;
		TSharedPtr<FJsonObject> Root;
		const FString Path = ArenaRuntimeMap::GetMapDir(MapName) / TEXT("map.json");
		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		if (!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid()
			|| !Root->TryGetArrayField(TEXT("actors"), Actors))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& V : *Actors)
		{
			const TSharedPtr<FJsonObject> A = V.IsValid() ? V->AsObject() : nullptr;
			const TSharedPtr<FJsonObject>* Props = nullptr;
			FString Class, Event;
			if (!A.IsValid() || !A->TryGetStringField(TEXT("class"), Class) || !Class.Contains(TEXT("Factory")) || !A->TryGetObjectField(TEXT("properties"), Props)
				|| !(*Props)->TryGetStringField(TEXT("VehicleEvent"), Event) || !Event.Equals(Tag, ESearchCase::IgnoreCase))
			{
				continue;
			}
			FUSClass* FactoryClass = nullptr;
			for (const TCHAR* Pkg : { TEXT("UT2k4AssaultFull"), TEXT("UT2k4Assault"), TEXT("Onslaught") })
			{
				FactoryClass = FactoryClass ? FactoryClass : FUSRuntime::Get().FindClass(FString(Pkg) + TEXT(".") + Class);
			}
			FString VehicleClass;
			(*Props)->TryGetStringField(TEXT("VehicleClass"), VehicleClass);
			if (VehicleClass.IsEmpty() || VehicleClass == TEXT("None"))
			{
				VehicleClass = ScriptDefault(FactoryClass, TEXT("VehicleClass"));
			}
			const FString MeshObj = ScriptDefault(VehicleClass.IsEmpty() ? nullptr : FUSRuntime::Get().FindClass(VehicleClass), TEXT("Mesh"));
			FString Pkg, Rest;
			if (!MeshObj.Split(TEXT("."), &Pkg, &Rest))
			{
				UE_LOG(LogOpenUT2k4, Log, TEXT("AS: vehicle of objective %s: factory %s (%s), no mesh for %s"), *Tag, *Class, FactoryClass ? TEXT("class found") : TEXT("class missing"), *VehicleClass);
				return false;
			}
			OutMesh = FString::Printf(TEXT("SkeletalMeshes/%s/%s"), *Pkg, *Rest.Replace(TEXT("."), TEXT("_")));
			const TSharedPtr<FJsonObject>* RotObj = nullptr;
			FRotator Rot = FRotator::ZeroRotator;
			if (A->TryGetObjectField(TEXT("rotation"), RotObj))
			{
				Rot = FRotator((*RotObj)->GetNumberField(TEXT("pitch")), (*RotObj)->GetNumberField(TEXT("yaw")), (*RotObj)->GetNumberField(TEXT("roll")));
			}
			OutTM = FTransform(Rot, JsonVector(A, TEXT("location"), FVector::ZeroVector));
			UE_LOG(LogOpenUT2k4, Log, TEXT("AS: vehicle of objective %s: %s from %s at %s"), *Tag, *OutMesh, *VehicleClass, *OutTM.GetLocation().ToCompactString());
			return true;
		}
		return false;
	}
}

bool UArenaRules_AS::ReadImportedObjectives(AArenaLevelBuilder* Builder, TArray<FObjectiveDef>& OutDefs)
{
	using namespace ArenaAS;

	// The importer writes assault objectives to "assault_objectives"; older files only have "game_objectives".
	TArray<FArenaRawMarker> Markers = Builder->GetRawMarkers(TEXT("assault_objectives"));
	FString ListName = TEXT("assault_objectives");
	if (Markers.Num() == 0)
	{
		Markers = Builder->GetRawMarkers(TEXT("game_objectives"));
		ListName = TEXT("game_objectives");
	}

	struct FPending
	{
		FObjectiveDef Def;
		double OrderKey = 0.0;
	};
	TArray<FPending> Pending;
	TMap<int32, int32> DefenderVotes;

	for (int32 i = 0; i < Markers.Num(); ++i)
	{
		const FArenaRawMarker& M = Markers[i];
		const FString Cls = ClassText(M);
		const FString Name = GetStr(M, { TEXT("actor"), TEXT("name") });
		const FString Label = FString::Printf(TEXT("%s (%s)"), *Name, *M.Class);

		if (ContainsAny(Cls, { TEXT("powernode"), TEXT("powercore") }))
		{
			continue; // Onslaught markers in a shared list
		}
		bool bBotOnly = false;
		GetBool(M, { TEXT("bot_only"), TEXT("bBotOnly") }, bBotOnly);
		// bBotOnlyObjective: a helper UT2004 never shows players. Watching a server it is kept (inactive there), so all
		// of the server's objectives have a counterpart
		const bool bNetWorld = GetWorld() && GetWorld()->URL.HasOption(TEXT("UE2Net"));
		if (bBotOnly && !bNetWorld)
		{
			SkippedNotes.Add(FString::Printf(TEXT("%s: bot-only helper objective"), *Label));
			continue;
		}

		FObjectiveDef Def;
		Def.SourceClass = M.Class;
		Def.SourceActor = Name;
		Def.Yaw = M.Rotation.Yaw;
		if (ContainsAny(Cls, { TEXT("energycore_spawn") }))
		{
			// the energy core's pick-up spot: a reach objective before its delivery point (the core is not carried here)
			Def.Type = EArenaASObjectiveType::Touch;
			SkippedNotes.Add(FString::Printf(TEXT("%s: energy core pick-up played as a reach objective (no carried core)"), *Label));
		}
		else if (ContainsAny(Cls, { TEXT("destroy") }))
		{
			Def.Type = EArenaASObjectiveType::Destroy;
		}
		else if (ContainsAny(Cls, { TEXT("hold") }))
		{
			// Hold objectives that are worked at a device (panels, switches, charges) become Use objectives;
			// the rest are zones to hold. Only the original hint text is inspected, never shown.
			const FString Hint = (GetStr(M, { TEXT("info_attacker"), TEXT("ObjectiveDescription") }) + TEXT(" ")
				+ GetStr(M, { TEXT("description"), TEXT("ObjectiveName") })).ToLower();
			const bool bDevice = ContainsAny(Hint, { TEXT("panel"), TEXT("switch"), TEXT("console"), TEXT("lever"), TEXT("terminal"),
				TEXT("explosive"), TEXT("charge"), TEXT("open"), TEXT("extend"), TEXT("activate"), TEXT("operate"), TEXT("disable") });
			Def.Type = bDevice ? EArenaASObjectiveType::Use : EArenaASObjectiveType::Hold;
		}
		else if (ContainsAny(Cls, { TEXT("useobjective"), TEXT(" use"), TEXT("trigger_asuse") }))
		{
			Def.Type = EArenaASObjectiveType::Use;
		}
		else if (ContainsAny(Cls, { TEXT("proximity"), TEXT("triggered"), TEXT("touch"), TEXT("reach") }))
		{
			Def.Type = EArenaASObjectiveType::Touch;
		}
		else if (ContainsAny(Cls, { TEXT("objective") }))
		{
			Def.Type = EArenaASObjectiveType::Touch;
			SkippedNotes.Add(FString::Printf(TEXT("%s: unknown objective class, played as a reach objective"), *Label));
		}
		else
		{
			SkippedNotes.Add(FString::Printf(TEXT("%s: not an objective"), *Label));
			continue;
		}

		bool bVehicle = false;
		GetBool(M, { TEXT("must_board_vehicle_first"), TEXT("bMustBoardVehicleFirst") }, bVehicle);
		if (bVehicle || ContainsAny(Cls, { TEXT("specialvehicle") }))
		{
			Def.bNeedsVehicle = true;
			SkippedNotes.Add(FString::Printf(TEXT("%s: vehicle requirement dropped (no vehicles)"), *Label));
		}
		if (!GetStr(M, { TEXT("vehicle_path_name") }).IsEmpty())
		{
			Def.bNeedsVehicle = true;
			SkippedNotes.Add(FString::Printf(TEXT("%s: vehicle path ignored"), *Label));
		}

		bool bOptional = false;
		GetBool(M, { TEXT("optional"), TEXT("bOptionalObjective"), TEXT("is_optional") }, bOptional);
		Def.bOptional = bOptional;

		double Num = 0.0;
		switch (Def.Type)
		{
		case EArenaASObjectiveType::Touch:
			Def.Radius = TouchRadius;
			break;
		case EArenaASObjectiveType::Hold:
			Def.Radius = HoldRadius;
			Def.Duration = GetNum(M, { TEXT("hold_time"), TEXT("completion_time"), TEXT("MaxCompletionTime"), TEXT("required_time") }, Num) && Num > 0.0
				? FMath::Clamp(float(Num), 3.f, 20.f) : DefaultHoldTime;
			break;
		case EArenaASObjectiveType::Use:
			Def.Radius = UseRadius;
			Def.Duration = GetNum(M, { TEXT("use_time"), TEXT("completion_time"), TEXT("MaxCompletionTime"), TEXT("required_time") }, Num) && Num > 0.0
				? FMath::Clamp(float(Num), 2.f, 12.f) : DefaultUseTime;
			break;
		case EArenaASObjectiveType::Destroy:
			Def.Radius = 160.f;
			Def.Health = GetNum(M, { TEXT("damage_capacity"), TEXT("DamageCapacity"), TEXT("health"), TEXT("max_health"), TEXT("Health") }, Num) && Num > 0.0
				? FMath::Clamp(float(Num), 250.f, 3000.f) : DefaultDestroyHealth;
			break;
		}

		// Floor below the marker (imported geometry is traceable in OnLevelBuilt)
		FVector Floor;
		Def.Location = Builder->TraceFloor(M.Location, 1500.f, Floor) ? Floor : M.Location;

		FPending P;
		P.Def = Def;
		double Order = 0.0, Priority = 0.0;
		if (GetNum(M, { TEXT("order"), TEXT("stage"), TEXT("Stage") }, Order))
		{
			P.OrderKey = Order;
		}
		else if (GetNum(M, { TEXT("defense_priority"), TEXT("DefensePriority") }, Priority))
		{
			P.OrderKey = 1000.0 - Priority; // highest defense priority first
		}
		else
		{
			P.OrderKey = 2000.0 + i;
		}
		Pending.Add(P);

		double Defender = -1.0;
		if (GetNum(M, { TEXT("defender_team"), TEXT("DefenderTeamIndex"), TEXT("defending_team") }, Defender) && (Defender == 0.0 || Defender == 1.0))
		{
			DefenderVotes.FindOrAdd(int32(Defender))++;
		}
	}

	// Dense stage numbers from the distinct order keys of the required objectives
	TArray<double> Keys;
	for (const FPending& P : Pending)
	{
		if (!P.Def.bOptional)
		{
			Keys.AddUnique(P.OrderKey);
		}
	}
	Keys.Sort();
	for (FPending& P : Pending)
	{
		P.Def.Stage = P.Def.bOptional ? -1 : Keys.IndexOfByKey(P.OrderKey);
		OutDefs.Add(P.Def);
	}

	const int32 Votes0 = DefenderVotes.FindRef(0), Votes1 = DefenderVotes.FindRef(1);
	const int32 DefenderTeam = Votes0 > Votes1 ? 0 : 1;
	AttackSide = 1 - DefenderTeam;

	// What else the map has that this mode does not play
	auto NoteList = [this, Builder](const TCHAR* List, const TCHAR* What)
	{
		const int32 N = Builder->GetRawMarkers(List).Num();
		if (N > 0)
		{
			SkippedNotes.Add(FString::Printf(TEXT("%d %s (%s)"), N, What, List));
		}
	};
	NoteList(TEXT("movers"), TEXT("movers left static"));
	NoteList(TEXT("vehicle_factories"), TEXT("vehicle / turret factories not spawned"));
	NoteList(TEXT("vehicles"), TEXT("vehicles not spawned"));
	NoteList(TEXT("triggers"), TEXT("scripted triggers / sequences ignored"));
	NoteList(TEXT("lifts"), TEXT("lifts ignored"));

	UE_LOG(LogOpenUT2k4, Log, TEXT("AS: read %d objectives from raw_markers.%s (%d stages, defenders = team %d)"),
		OutDefs.Num(), *ListName, Keys.Num(), DefenderTeam);
	return Keys.Num() > 0;
}

void UArenaRules_AS::BuildAnchorObjectives(AArenaLevelBuilder* Builder, TArray<FObjectiveDef>& OutDefs)
{
	const TArray<FTransform> Chain = Builder->GetAnchorSeries(TEXT("Objective"));
	static const EArenaASObjectiveType Cycle[] = { EArenaASObjectiveType::Touch, EArenaASObjectiveType::Hold, EArenaASObjectiveType::Use };
	for (int32 i = 0; i < Chain.Num(); ++i)
	{
		FObjectiveDef Def;
		Def.Location = Chain[i].GetLocation();
		Def.Yaw = Chain[i].Rotator().Yaw;
		Def.Stage = i;
		Def.Type = i == Chain.Num() - 1 ? EArenaASObjectiveType::Destroy : Cycle[i % 3];
		Def.Radius = Def.Type == EArenaASObjectiveType::Hold ? 330.f : Def.Type == EArenaASObjectiveType::Use ? ArenaAS::UseRadius : Def.Type == EArenaASObjectiveType::Touch ? ArenaAS::TouchRadius : 160.f;
		Def.Duration = Def.Type == EArenaASObjectiveType::Hold ? ArenaAS::DefaultHoldTime : ArenaAS::DefaultUseTime;
		Def.Health = ArenaAS::DefaultDestroyHealth;
		Def.SourceClass = FString::Printf(TEXT("Objective%d"), i);
		OutDefs.Add(Def);
	}
	// A bonus objective off the main path
	FTransform Bonus;
	if (Builder->GetAnchorPoint(TEXT("ControlA"), Bonus))
	{
		bool bFree = true;
		for (const FObjectiveDef& D : OutDefs)
		{
			bFree &= FVector::Dist(D.Location, Bonus.GetLocation()) > 600.f;
		}
		if (bFree)
		{
			FObjectiveDef Def;
			Def.Location = Bonus.GetLocation();
			Def.Yaw = Bonus.Rotator().Yaw;
			Def.Stage = -1;
			Def.bOptional = true;
			Def.Type = EArenaASObjectiveType::Use;
			Def.Radius = ArenaAS::UseRadius;
			Def.Duration = 3.f;
			Def.SourceClass = TEXT("ControlA");
			OutDefs.Add(Def);
		}
	}
	AttackSide = 0; // the chain runs from side 0 towards side 1
}

void UArenaRules_AS::SpawnObjectives(const TArray<FObjectiveDef>& Defs)
{
	UWorld* World = GetWorld();
	NumStages = 0;
	VehicleObjectives.Reset();
	for (const FObjectiveDef& D : Defs)
	{
		NumStages = FMath::Max(NumStages, D.Stage + 1);
	}
	const AArenaLevelBuilder* Builder = GetLevelBuilder();
	const FString MapName = Builder ? AArenaLevelBuilder::GetMapNameFromLayoutId(Builder->GetBuiltLayoutId()) : FString();
	const TMap<FString, TSharedPtr<FJsonObject>> MapActors = ArenaASLocal::LoadMapActors(MapName);
	for (int32 i = 0; i < Defs.Num(); ++i)
	{
		const FObjectiveDef& D = Defs[i];
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AArenaAS_Objective* Obj = World->SpawnActor<AArenaAS_Objective>(AArenaAS_Objective::StaticClass(), D.Location, FRotator(0.f, D.Yaw, 0.f), Params);
		if (!Obj)
		{
			continue;
		}
		const float Health = D.Health;
		if (!D.SourceActor.IsEmpty())
		{
			// UT2004's objectives are the map's own things: its mesh (DestroyableObjective_SM) or nothing visible.
			FString Mesh, Destroyed;
			FTransform MeshTM;
			ArenaASLocal::FindMapActorMesh(MapActors, D.SourceActor, Mesh, MeshTM, Destroyed);
			Obj->SetMapSource(Mesh, MeshTM, Destroyed, D.SourceClass.Contains(TEXT("Vehicle")));
			if (const TSharedPtr<FJsonObject>* Rec = MapActors.Find(D.SourceActor))
			{
				FString Tag, Event;
				(*Rec)->TryGetStringField(TEXT("tag"), Tag);
				(*Rec)->TryGetStringField(TEXT("event"), Event);
				Obj->UE2Tag = Tag.IsEmpty() || Tag == TEXT("None") ? NAME_None : FName(*Tag);
				FString VehicleMesh;
				FTransform VehicleTM;
				const bool bRealVehicle = AssaultVehicles.ContainsByPredicate([&Tag](const FAssaultVehicle& AV) { return AV.bTurret && !Tag.IsEmpty() && AV.VehicleEvent.Equals(Tag, ESearchCase::IgnoreCase); });
				if (D.SourceClass.Contains(TEXT("DestroyVehicle")) && !bRealVehicle && ArenaASLocal::FindObjectiveVehicle(MapName, Tag, VehicleMesh, VehicleTM))
				{
					Obj->SetMapVehicle(VehicleMesh, VehicleTM);
				}
				Obj->UE2Event = Event.IsEmpty() || Event == TEXT("None") ? NAME_None : FName(*Event);
				Obj->bCompletedByEvent = D.SourceClass.Contains(TEXT("Triggered"));
				// UT2004 Hold / Use objectives' MoverTag: the mover they work (AS-Convoy's crane extends the boarding platform)
				const TSharedPtr<FJsonObject>* Props = nullptr;
				FString MoverTag;
				if ((*Rec)->TryGetObjectField(TEXT("properties"), Props) && Props && (*Props)->TryGetStringField(TEXT("MoverTag"), MoverTag)
					&& !MoverTag.IsEmpty() && MoverTag != TEXT("None"))
				{
					ObjectiveMoverTags.Add(D.SourceActor, FName(*MoverTag));
				}
			}
		}
		Obj->UE2SourceName = D.SourceActor; // a UT2004 server's state finds its objective by this (Net/UE2NetWorld.h)
		Obj->Setup(D.Type, Objectives.Num(), D.Stage, D.bOptional, MakeDescription(D.Type, D.Stage, D.bOptional, i), D.Radius, D.Duration, Health);
		if (D.bNeedsVehicle)
		{
			VehicleObjectives.Add(Objectives.Num());
		}
		Objectives.Add(Obj);
		// a DestroyVehicleObjective whose vehicle is one of the map's turrets / sentinels: that turret is the target
		for (FAssaultVehicle& AV : AssaultVehicles)
		{
			if (AV.bTurret && Obj->UE2Tag != NAME_None && AV.VehicleEvent.Equals(Obj->UE2Tag.ToString(), ESearchCase::IgnoreCase) && D.SourceClass.Contains(TEXT("DestroyVehicle")))
			{
				AV.Objective = Obj;
				AV.Turret.Health = FMath::Max(AV.Turret.Health, Health);
				Obj->SetLinkedVehicle(nullptr);
			}
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("AS: objective %d stage %d%s %s '%s' at %s (from %s)"), Obj->GetIndex(), D.Stage, D.bOptional ? TEXT(" (bonus)") : TEXT(""),
			*AArenaAS_Objective::TypeLabel(D.Type), *Obj->GetDescription(), *D.Location.ToCompactString(), *D.SourceClass);
	}
}

bool UArenaRules_AS::OnLevelBuilt(AArenaLevelBuilder* Builder)
{
	if (!Builder || !GetWorld())
	{
		return false;
	}
	// developer hook: -DumpUScriptDefaults=Pkg.Class,Pkg.Class logs every non-empty class default (UT2004 data lookups)
	FString DumpList;
	if (FParse::Value(FCommandLine::Get(), TEXT("DumpUScriptDefaults="), DumpList, false) && ArenaUScript::EnsureRuntime())
	{
		TArray<FString> Names;
		DumpList.ParseIntoArray(Names, TEXT(","));
		for (const FString& N : Names)
		{
			FUSClass* C = FUSRuntime::Get().FindClass(N);
			FUSObject* Def = C ? C->GetDefaultObject() : nullptr;
			if (!Def || !Def->Data)
			{
				UE_LOG(LogOpenUT2k4, Log, TEXT("UScriptDefaults %s: not found"), *N);
				continue;
			}
			for (FUSProperty* P : C->AllProps)
			{
				for (int32 i = 0; i < P->ArrayDim; ++i)
				{
					FString V;
					P->ExportText(V, Def->Data + P->Offset + i * P->ElementSize, true);
					if (!V.IsEmpty() && V != TEXT("0") && V != TEXT("0.000000") && V != TEXT("None") && V != TEXT("False") && V != TEXT("()") && V != TEXT("\"\""))
					{
						UE_LOG(LogOpenUT2k4, Log, TEXT("UScriptDefaults %s %s%s = %s"), *N, *P->GetName(), P->ArrayDim > 1 ? *FString::Printf(TEXT("[%d]"), i) : TEXT(""), *V.Left(200));
					}
				}
			}
		}
	}
	// the map's Assault vehicle factories (turrets, sentinels, vehicles); a server's are its own when watching one
	if (!GetWorld()->URL.HasOption(TEXT("UE2Net")))
	{
		ReadAssaultVehicles(AArenaLevelBuilder::GetMapNameFromLayoutId(Builder->GetBuiltLayoutId()));
	}
	TArray<FObjectiveDef> Defs;
	if (!ReadImportedObjectives(Builder, Defs))
	{
		// No usable assault markers: fall back to the builder's Objective anchors (other game objectives), if any
		Defs.Reset();
		BuildAnchorObjectives(Builder, Defs);
		for (FObjectiveDef& D : Defs)
		{
			FVector Floor;
			if (Builder->TraceFloor(D.Location, 1500.f, Floor))
			{
				D.Location = Floor;
			}
		}
	}

	int32 NumRequired = 0;
	for (const FObjectiveDef& D : Defs)
	{
		NumRequired += D.bOptional ? 0 : 1;
	}
	if (NumRequired == 0)
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("AS: no objectives on %s"), *Builder->GetBuiltLayoutId());
		return false;
	}
	SpawnObjectives(Defs);
	if (UArenaEventSubsystem* Events = UArenaEventSubsystem::Get(GetWorld()))
	{
		Events->OnEvent.Remove(UE2EventHandle);
		UE2EventHandle = Events->OnEvent.AddUObject(this, &UArenaRules_AS::OnUE2Event);
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Info = GetWorld()->SpawnActor<AArenaAS_Info>(AArenaAS_Info::StaticClass(), FTransform::Identity, Params);
	if (Info)
	{
		Info->NumStages = NumStages;
	}
	for (const FString& Note : SkippedNotes)
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("AS: skipped: %s"), *Note);
	}
	CurrentStage = 0;
	ApplyRolesAndStage();
	return true;
}

void UArenaRules_AS::InitGameState(AArenaGameState* GS)
{
	if (GS)
	{
		GS->TimeLimit = RoundSeconds;
		GS->RemainingTime = RoundSeconds;
		if (GS->GoalScore <= 0)
		{
			GS->GoalScore = 1;
		}
	}
}

void UArenaRules_AS::GetNavHints(TArray<FVector>& OutPoints) const
{
	for (const AArenaAS_Objective* Obj : Objectives)
	{
		if (!Obj)
		{
			continue;
		}
		const FVector L = Obj->GetActorLocation();
		if (Obj->GetType() == EArenaASObjectiveType::Destroy)
		{
			for (int32 i = 0; i < 4; ++i)
			{
				const float A = PI * 0.5f * i;
				OutPoints.Add(L + FVector(FMath::Cos(A) * 260.f, FMath::Sin(A) * 260.f, 10.f));
			}
		}
		else
		{
			OutPoints.Add(L + FVector(0.f, 0.f, 10.f));
		}
	}
}

void UArenaRules_AS::OnStartPlay()
{
	EnsureSpawnGroups();
	ApplyRolesAndStage();
}

// =============================================================================================
// Roles, stages, rounds
// =============================================================================================

int32 UArenaRules_AS::GetAttackingTeam() const
{
	const AArenaGameState* GS = GetGameState();
	const bool bSwapped = GS && GS->bSidesSwapped;
	return bSwapped ? 1 - AttackSide : AttackSide;
}

AArenaAS_Info* UArenaRules_AS::FindInfo() const
{
	if (Info)
	{
		return Info;
	}
	if (!HudInfo.IsValid() && GetWorld())
	{
		TActorIterator<AArenaAS_Info> It(GetWorld());
		if (It)
		{
			HudInfo = *It;
		}
	}
	return HudInfo.Get();
}

void UArenaRules_AS::ApplyRolesAndStage()
{
	const int32 Attack = GetAttackingTeam();
	for (AArenaAS_Objective* Obj : Objectives)
	{
		if (Obj)
		{
			Obj->SetRoles(Attack, 1 - Attack);
		}
	}
	SetStage(CurrentStage);
	if (Info)
	{
		Info->AttackingTeam = Attack;
		Info->NumStages = NumStages;
		Info->PairRound = PairRound;
		Info->PairNumber = PairNumber;
		Info->FirstAttackTeam = PairRound == 1 ? FirstAttackTeam : -1;
		Info->bFirstSucceeded = bFirstSucceeded;
		Info->FirstTime = FirstTime;
		Info->FirstStages = FirstStages;
		Info->TimeToBeat = (PairRound == 1 && bFirstSucceeded) ? FirstTime : -1.f;
		Info->bOvertimePair = bOvertimePair;
	}
}

void UArenaRules_AS::SetStage(int32 NewStage)
{
	CurrentStage = FMath::Clamp(NewStage, 0, FMath::Max(0, NumStages - 1));
	for (AArenaAS_Objective* Obj : Objectives)
	{
		if (!Obj)
		{
			continue;
		}
		const bool bActive = !Obj->IsCompleted() && (Obj->IsOptional() || Obj->GetStage() == CurrentStage);
		Obj->SetActive(bActive);
	}
	if (Info)
	{
		Info->CurrentStage = CurrentStage;
	}
	if (GameMode)
	{
		for (AController* C : GameMode->GetBots())
		{
			if (AArenaBotController* Bot = Cast<AArenaBotController>(C))
			{
				Bot->ForceGoalUpdate();
			}
		}
	}
}

FVector UArenaRules_AS::GetStageLocation(int32 InStage) const
{
	for (const AArenaAS_Objective* Obj : Objectives)
	{
		if (Obj && !Obj->IsOptional() && Obj->GetStage() == InStage)
		{
			return Obj->GetActorLocation();
		}
	}
	return FVector::ZeroVector;
}

void UArenaRules_AS::OnRoundStart(int32 Round)
{
	RoundStartTime = GetWorld()->GetTimeSeconds();
	if (AssaultVehicles.Num() > 0 && !AssaultVehicles.ContainsByPredicate([](const FAssaultVehicle& AV) { return AV.Pawn.IsValid(); }))
	{
		ResetAssaultVehicles();
	}
	bAttackFinished = false;
	if (DebugStartStage > 0 && CurrentStage == 0 && NumStages > 1)
	{
		const int32 Skip = FMath::Min(DebugStartStage, NumStages - 1);
		for (AArenaAS_Objective* Obj : Objectives)
		{
			if (Obj && !Obj->IsOptional() && Obj->GetStage() < Skip)
			{
				Obj->ForceCompleted();
			}
		}
		CurrentStage = Skip;
		UE_LOG(LogOpenUT2k4, Log, TEXT("AS: testing - starting at stage %d"), Skip);
	}
	ApplyRolesAndStage();
	const int32 Attack = GetAttackingTeam();
	UE_LOG(LogOpenUT2k4, Log, TEXT("AS: round %d started - %s team attacks (pair %d, attack %d/2), time limit %d s%s"),
		Round, *ArenaTeam::Name(Attack), PairNumber, PairRound + 1, GetGameState() ? GetGameState()->TimeLimit : 0,
		(PairRound == 1 && bFirstSucceeded) ? *FString::Printf(TEXT(", time to beat %.1f s"), FirstTime) : TEXT(""));
	if (GameMode)
	{
		GameMode->BroadcastTeamMessage(Attack, PairRound == 1 && bFirstSucceeded
			? FString::Printf(TEXT("Attack! Beat %s"), *ArenaHudStyle::FormatTime(FMath::CeilToInt(FirstTime)))
			: FString(TEXT("Attack! Complete the objectives")), ArenaTeam::Color(Attack), 3.f);
		GameMode->BroadcastTeamMessage(1 - Attack, TEXT("Defend the objectives!"), ArenaTeam::Color(1 - Attack), 3.f);
	}
}

void UArenaRules_AS::OnRoundReset(int32 Round)
{
	// doors, lifts, triggers and scripted sequences back to their map start state
	ArenaUE2World::ResetWorldActors(GetWorld());
	for (FSpawnGroup& G : SpawnGroups)
	{
		G.bEnabledNow = G.bInitial;
	}
	bSpawnsToggled = false;
	for (AArenaAS_Objective* Obj : Objectives)
	{
		if (Obj)
		{
			Obj->ResetObjective();
		}
	}
	FiringBots.Reset();
	BotAttackTime.Reset();
	CurrentStage = 0;
	bAttackFinished = false;
	ResetAssaultVehicles();
	if (AArenaGameState* GS = GetGameState())
	{
		GS->TimeLimit = (PairRound == 1 && bFirstSucceeded) ? FMath::Max(5, FMath::CeilToInt(FirstTime)) : RoundSeconds;
		GS->RemainingTime = GS->TimeLimit;
	}
	ApplyRolesAndStage();
}

void UArenaRules_AS::Tick(float DeltaSeconds)
{
	AArenaGameState* GS = GetGameState();
	if (!GS || !GameMode)
	{
		return;
	}
	if (GS->IsPlaying() && !bAttackFinished)
	{
		const float Now = GetWorld()->GetTimeSeconds();
		TArray<AArenaAS_Objective*> Snapshot;
		for (AArenaAS_Objective* Obj : Objectives)
		{
			if (Obj)
			{
				Snapshot.Add(Obj);
			}
		}
		for (AArenaAS_Objective* Obj : Snapshot)
		{
			if (bAttackFinished)
			{
				break;
			}
			if (Obj->ServerTick(DeltaSeconds, Now))
			{
				OnObjectiveCompleted(Obj);
			}
		}
		UpdateBotDestroyFire(DeltaSeconds);
		TickAssaultVehicles(Now);
		UpdateMoverStall(Now);

		// periodic status for logs / dedicated-server diagnostics
		StatusAccum += DeltaSeconds;
		if (StatusAccum >= 10.f)
		{
			StatusAccum = 0.f;
			const int32 Attack = GetAttackingTeam();
			for (const AArenaAS_Objective* Obj : Objectives)
			{
				if (!Obj || !Obj->IsActive() || Obj->IsOptional())
				{
					continue;
				}
				float Nearest = BIG_NUMBER;
				FVector NearestLoc = FVector::ZeroVector;
				for (TActorIterator<AArenaCharacter> It(GetWorld()); It; ++It)
				{
					if (It->IsAlive() && It->GetTeam() == Attack && Cast<AArenaBotController>(It->GetController()))
					{
						const float D = FVector::Dist(It->GetActorLocation(), Obj->GetActorLocation());
						if (D < Nearest)
						{
							Nearest = D;
							NearestLoc = It->GetActorLocation();
						}
					}
				}
				for (TActorIterator<AArenaONS_Vehicle> It(GetWorld()); It; ++It)
				{
					// attackers flying (AS-MotherShip) are in their vehicles, not their characters
					const float D = FVector::Dist(It->GetActorLocation(), Obj->GetActorLocation());
					if (It->HasDriver() && It->GetTeam() == Attack && D < Nearest)
					{
						Nearest = D;
						NearestLoc = It->GetActorLocation();
					}
				}
				// path check from the nearest attacker (stall diagnostics: is the objective reachable from where they stand)
				FString PathNote;
				if (AArenaNavGraph* Nav = GetNavGraph(); Nav && Nearest < BIG_NUMBER)
				{
					TArray<FVector> Path;
					TArray<EArenaNavLinkType> Links;
					const bool bPath = Nav->FindPath(NearestLoc, Obj->GetActorLocation() + FVector(0.f, 0.f, 90.f), Path, Links);
					PathNote = bPath && Path.Num() > 0 ? FString::Printf(TEXT(", path %d nodes ending %.0f cm away"), Path.Num(), FVector::Dist(Path.Last(), Obj->GetActorLocation() + FVector(0.f, 0.f, 90.f)))
						: FString(TEXT(", no path"));
				}
				UE_LOG(LogOpenUT2k4, Log, TEXT("AS: status: objective %d %s work %.0f%% - nearest attacker %.0f cm (at %s)%s"), Obj->GetIndex(),
					*AArenaAS_Objective::TypeLabel(Obj->GetType()), Obj->GetWorkFraction() * 100.f, Nearest, *NearestLoc.ToCompactString(), *PathNote);
			}
		}
	}
	else if (FiringBots.Num() > 0)
	{
		TArray<TWeakObjectPtr<AArenaBotController>> Bots = FiringBots.Array();
		for (const TWeakObjectPtr<AArenaBotController>& B : Bots)
		{
			StopBotFire(B.Get());
		}
		FiringBots.Reset();
	}
}

void UArenaRules_AS::OnUE2Event(FName Event, AActor* Other, APawn* EventInstigator, bool bTrigger)
{
	if (!bTrigger || Event.IsNone())
	{
		return;
	}
	// UE2 PlayerSpawnManager.Trigger: the map's script moves the spawns forward by toggling its managers
	for (FSpawnGroup& G : SpawnGroups)
	{
		if (G.Tag == Event)
		{
			G.bEnabledNow = !G.bEnabledNow;
			bSpawnsToggled = true;
			UE_LOG(LogOpenUT2k4, Log, TEXT("AS: spawn %s %s by the map's event"), *G.Name, G.bEnabledNow ? TEXT("enabled") : TEXT("disabled"));
		}
	}
	for (AArenaAS_Objective* Obj : Objectives)
	{
		if (Obj && Obj->bCompletedByEvent && Obj->UE2Tag == Event)
		{
			Obj->CompleteByEvent();
		}
	}
}

void UArenaRules_AS::OnObjectiveCompleted(AArenaAS_Objective* Obj)
{
	const int32 Attack = GetAttackingTeam();
	// UE2 GameObjective.DisableObjective: TriggerEvent(Event) opens doors, starts movers and scripted sequences
	if (UArenaEventSubsystem* Events = UArenaEventSubsystem::Get(GetWorld()))
	{
		APawn* By = nullptr;
		for (const TWeakObjectPtr<AController>& C : Obj->GetContributors())
		{
			if (C.IsValid() && C->GetPawn())
			{
				By = C->GetPawn();
				break;
			}
		}
		Events->TriggerEvent(Obj->UE2Event, Obj, By);
		if (const FName* MoverTag = ObjectiveMoverTags.Find(Obj->UE2SourceName))
		{
			Events->TriggerEvent(*MoverTag, Obj, By);
		}
	}
	const float Elapsed = GetWorld()->GetTimeSeconds() - RoundStartTime;
	TArray<FString> Names;
	for (const TWeakObjectPtr<AController>& C : Obj->GetContributors())
	{
		if (AController* Ctrl = C.Get())
		{
			if (AArenaPlayerState* PS = Ctrl->GetPlayerState<AArenaPlayerState>())
			{
				PS->ObjectiveScore++;
				PS->AddScore(Obj->IsOptional() ? 5.f : 10.f);
				Names.Add(PS->GetPlayerName());
			}
		}
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("AS: objective %d completed (%s '%s', stage %d%s) by %s team [%s] at %.1f s"),
		Obj->GetIndex(), *AArenaAS_Objective::TypeLabel(Obj->GetType()), *Obj->GetDescription(), Obj->GetStage(),
		Obj->IsOptional() ? TEXT(", bonus") : TEXT(""), *ArenaTeam::Name(Attack), *FString::Join(Names, TEXT(", ")), Elapsed);

	if (Obj->IsOptional())
	{
		GameMode->BroadcastCenterMessage(FString::Printf(TEXT("Bonus objective complete: %s"), *Obj->GetDescription()),
			ArenaTeam::Color(Attack), 2.5f, TEXT("S_Pickup_Power"));
		return;
	}

	bool bStageDone = true;
	for (const AArenaAS_Objective* Other : Objectives)
	{
		if (Other && !Other->IsOptional() && Other->GetStage() == CurrentStage && !Other->IsCompleted())
		{
			bStageDone = false;
		}
	}
	if (!bStageDone)
	{
		GameMode->BroadcastCenterMessage(FString::Printf(TEXT("%s - done"), *Obj->GetDescription()), ArenaTeam::Color(Attack), 2.f, TEXT("S_Flag_Taken"));
		return;
	}
	if (CurrentStage >= NumStages - 1)
	{
		FinishAttack(true);
		return;
	}
	SetStage(CurrentStage + 1);
	UE_LOG(LogOpenUT2k4, Log, TEXT("AS: stage %d/%d reached at %.1f s; attacker spawns move forward"), CurrentStage + 1, NumStages, Elapsed);
	FString Next;
	for (const AArenaAS_Objective* Other : Objectives)
	{
		if (Other && !Other->IsOptional() && Other->GetStage() == CurrentStage)
		{
			Next = Other->GetDescription();
			break;
		}
	}
	GameMode->BroadcastCenterMessage(FString::Printf(TEXT("Objective complete! Next: %s"), *Next), ArenaTeam::Color(Attack), 2.5f, TEXT("S_Flag_Taken"));
}

float UArenaRules_AS::GetAttackProgressScore() const
{
	float Partial = 0.f;
	int32 Done = 0;
	for (const AArenaAS_Objective* Obj : Objectives)
	{
		if (!Obj || Obj->IsOptional())
		{
			continue;
		}
		if (Obj->IsCompleted())
		{
			++Done;
		}
		else if (Obj->GetStage() == CurrentStage)
		{
			Partial = FMath::Max(Partial, Obj->GetWorkFraction() * 0.99f);
		}
	}
	// Bonus objectives count a little (only to break exact ties)
	float Bonus = 0.f;
	for (const AArenaAS_Objective* Obj : Objectives)
	{
		if (Obj && Obj->IsOptional() && Obj->IsCompleted())
		{
			Bonus += 0.001f;
		}
	}
	return Done + Partial + Bonus;
}

void UArenaRules_AS::FinishAttack(bool bSuccess)
{
	AArenaGameState* GS = GetGameState();
	if (!GS || !GameMode || bAttackFinished)
	{
		return;
	}
	bAttackFinished = true;
	const float Elapsed = GetWorld()->GetTimeSeconds() - RoundStartTime;
	const float Progress = GetAttackProgressScore();
	const int32 StagesDone = bSuccess ? NumStages : CurrentStage;
	const int32 Attack = GetAttackingTeam();
	const int32 Defend = 1 - Attack;
	using ArenaHudStyle::FormatTime;

	UE_LOG(LogOpenUT2k4, Log, TEXT("AS: attack of %s team %s after %.1f s (stages %d/%d, progress %.2f)"),
		*ArenaTeam::Name(Attack), bSuccess ? TEXT("SUCCEEDED") : TEXT("failed"), Elapsed, StagesDone, NumStages, Progress);

	if (PairRound == 0)
	{
		FirstAttackTeam = Attack;
		bFirstSucceeded = bSuccess;
		FirstTime = Elapsed;
		FirstProgress = Progress;
		FirstStages = StagesDone;
		PairRound = 1;
		const FString Msg = bSuccess
			? FString::Printf(TEXT("%s Team completed the assault in %s! Sides swap."), *ArenaTeam::Name(Attack), *FormatTime(FMath::CeilToInt(Elapsed)))
			: FString::Printf(TEXT("%s Team held the line (%d/%d objectives lost). Sides swap."), *ArenaTeam::Name(Defend), StagesDone, NumStages);
		if (Info)
		{
			Info->FirstAttackTeam = Attack;
			Info->bFirstSucceeded = bSuccess;
			Info->FirstTime = Elapsed;
			Info->FirstStages = StagesDone;
		}
		GameMode->EndRound(bSuccess ? Attack : Defend, Msg, 7.f);
		return;
	}

	// Second attack of the pair: compare with the first one
	const int32 First = FirstAttackTeam;
	int32 Winner = -1;
	FString Why;
	if (bSuccess)
	{
		if (!bFirstSucceeded || Elapsed < FirstTime)
		{
			Winner = Attack;
			Why = bFirstSucceeded
				? FString::Printf(TEXT("%s Team beat the time (%s vs %s)!"), *ArenaTeam::Name(Attack), *FormatTime(FMath::CeilToInt(Elapsed)), *FormatTime(FMath::CeilToInt(FirstTime)))
				: FString::Printf(TEXT("%s Team completed the assault!"), *ArenaTeam::Name(Attack));
		}
		else
		{
			Winner = First;
			Why = FString::Printf(TEXT("%s Team's time stands!"), *ArenaTeam::Name(First));
		}
	}
	else if (bFirstSucceeded)
	{
		Winner = First;
		Why = FString::Printf(TEXT("%s Team failed to beat the time!"), *ArenaTeam::Name(Attack));
	}
	else if (Progress > FirstProgress + 0.0005f)
	{
		Winner = Attack;
		Why = FString::Printf(TEXT("Both attacks failed - %s Team got further!"), *ArenaTeam::Name(Attack));
	}
	else if (FirstProgress > Progress + 0.0005f)
	{
		Winner = First;
		Why = FString::Printf(TEXT("Both attacks failed - %s Team got further!"), *ArenaTeam::Name(First));
	}

	PairRound = 0;
	FirstAttackTeam = -1;
	bFirstSucceeded = false;
	FirstTime = 0.f;
	FirstProgress = 0.f;
	FirstStages = 0;

	const int32 Goal = FMath::Max(1, GS->GoalScore);
	if (Winner == 0 || Winner == 1)
	{
		GameMode->AddTeamScore(Winner, 1);
		UE_LOG(LogOpenUT2k4, Log, TEXT("AS: pair %d won by %s team - %s (score %d:%d)"), PairNumber, *ArenaTeam::Name(Winner), *Why, GS->TeamScores[0], GS->TeamScores[1]);
		if (GS->TeamScores[Winner] >= Goal)
		{
			GameMode->EndMatch(FString::Printf(TEXT("%s Team wins the assault! %s"), *ArenaTeam::Name(Winner), *Why), Winner, nullptr);
			return;
		}
		++PairNumber;
		bOvertimePair = false;
		GameMode->EndRound(Winner, Why, 8.f);
		return;
	}

	UE_LOG(LogOpenUT2k4, Log, TEXT("AS: pair %d is a draw"), PairNumber);
	if (!bOvertimeUsed)
	{
		bOvertimeUsed = true;
		bOvertimePair = true;
		++PairNumber;
		GameMode->EndRound(-1, TEXT("Draw! Overtime: both teams attack again"), 8.f);
		return;
	}
	GameMode->EndMatch(TEXT("The assault ends in a draw"), -1, nullptr);
}

void UArenaRules_AS::OnTimeLimitReached()
{
	const AArenaGameState* GS = GetGameState();
	if (GS && GS->IsPlaying() && !bAttackFinished)
	{
		FinishAttack(false);
	}
}

bool UArenaRules_AS::OnPlayerUse(AController* Controller)
{
	const AArenaGameState* GS = GetGameState();
	if (!GS || !GS->IsPlaying())
	{
		return false;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	for (AArenaAS_Objective* Obj : Objectives)
	{
		if (Obj && Obj->NotifyUse(Controller, Now))
		{
			return true;
		}
	}
	// the map's turrets and vehicles: get in
	AArenaCharacter* Char = Controller ? Cast<AArenaCharacter>(Controller->GetPawn()) : nullptr;
	if (!Char)
	{
		return false;
	}
	if (FireUseTrigger(Char->GetActorLocation(), Controller))
	{
		return true;
	}
	APawn* Best = nullptr;
	float BestDist = TNumericLimits<float>::Max();
	for (const FAssaultVehicle& AV : AssaultVehicles)
	{
		APawn* P = AV.Pawn.Get();
		const float D = P ? float(FVector::Dist(P->GetActorLocation(), Char->GetActorLocation())) : BestDist;
		if (D >= BestDist)
		{
			continue;
		}
		if (const AArenaAS_Turret* T = Cast<AArenaAS_Turret>(P); T && T->CanBeEnteredBy(Char))
		{
			Best = P;
			BestDist = D;
		}
		else if (const AArenaONS_Vehicle* V = Cast<AArenaONS_Vehicle>(P); V && V->CanBeEnteredBy(Char) && D < V->GetSpec().HalfExtent.X + 300.f)
		{
			Best = P;
			BestDist = D;
		}
	}
	if (AArenaAS_Turret* T = Cast<AArenaAS_Turret>(Best))
	{
		return T->EnterTurret(Char);
	}
	if (AArenaONS_Vehicle* V = Cast<AArenaONS_Vehicle>(Best))
	{
		return V->EnterSeat(Char, V->GetFreeSeat());
	}
	return false;
}

// =============================================================================================
// Spawning
// =============================================================================================

bool UArenaRules_AS::IsReachable(const FVector& From, const FVector& To, bool bDoorsOpen)
{
	AArenaNavGraph* Nav = GetNavGraph();
	if (!Nav || Nav->NumNodes() == 0)
	{
		return true;
	}
	const FString Key = ArenaAS::ReachKey(From, To) + (bDoorsOpen ? TEXT("/open") : TEXT(""));
	if (const bool* Cached = ReachCache.Find(Key))
	{
		return *Cached;
	}
	TArray<FVector> Path;
	TArray<EArenaNavLinkType> Links;
	const FVector Goal = To + FVector(0.f, 0.f, 90.f);
	// the path must really end at the objective (not on a floor below / beside it)
	const bool bOk = Nav->FindPath(From, Goal, Path, Links, false, bDoorsOpen) && Path.Num() > 0
		&& FVector::Dist2D(Path.Last(), Goal) < 350.f && FMath::Abs(Path.Last().Z - Goal.Z) < 200.f;
	ReachCache.Add(Key, bOk);
	return bOk;
}

void UArenaRules_AS::EnsureSpawnGroups()
{
	// the groups come from the map's starts and managers; the stage table needs the nav graph, which may still be
	// building when the first player spawns (it then spawns at its managers' starts already)
	if (!bSpawnGroupsBuilt && GetLevelBuilder() && GetLevelBuilder()->GetPlayerStarts().Num() > 0)
	{
		bSpawnGroupsBuilt = true;
		BuildImportedSpawnGroups();
		for (FSpawnGroup& G : SpawnGroups)
		{
			FVector Sum = FVector::ZeroVector;
			int32 N = 0;
			for (const TWeakObjectPtr<APlayerStart>& S : G.Starts)
			{
				if (S.IsValid())
				{
					Sum += S->GetActorLocation();
					++N;
				}
			}
			G.Centroid = N > 0 ? Sum / N : FVector::ZeroVector;
		}
		SpawnGroups.RemoveAll([](const FSpawnGroup& G) { return G.Starts.Num() == 0; });
	}
	if (!bSpawnGroupsBuilt || bSpawnStagesAssigned || !GetNavGraph())
	{
		return;
	}
	bSpawnStagesAssigned = true;
	DropUnreachableStages();
	AssignStageSpawnGroups();
}

void UArenaRules_AS::BuildImportedSpawnGroups()
{
	using namespace ArenaAS;
	AArenaLevelBuilder* Builder = GetLevelBuilder();
	const TArray<TObjectPtr<APlayerStart>>& Starts = Builder->GetPlayerStarts();

	// spawn managers: start group -> attacker (0) / defender (1), enabled at round start
	struct FManager
	{
		int32 Role = -1;
		bool bEnabled = false;
		FString Tag;
		bool bFighter = false;
	};
	TMap<int32, FManager> Managers;
	for (const FArenaRawMarker& M : Builder->GetRawMarkers(TEXT("spawn_managers")))
	{
		double Group = 0.0, Role = -1.0;
		// PlayerStartTeam defaults to 0 (unset in the map) and AssaultTeam to 0 (the attackers), bEnabled to true
		GetNum(M, { TEXT("player_start_team"), TEXT("PlayerStartTeam") }, Group);
		if (!GetNum(M, { TEXT("assault_team"), TEXT("AssaultTeam") }, Role))
		{
			Role = 0.0;
		}
		bool bEnabled = true;
		GetBool(M, { TEXT("enabled"), TEXT("bEnabled") }, bEnabled);
		FManager& F = Managers.FindOrAdd(int32(Group));
		F.bFighter |= GetStr(M, { TEXT("ForcedPawnClass"), TEXT("forced_pawn_class") }).Contains(TEXT("SpaceFighter"));
		if (F.Role < 0)
		{
			F.Role = (Role == 0.0 || Role == 1.0) ? int32(Role) : -1;
			F.Tag = GetStr(M, { TEXT("tag") });
		}
		F.bEnabled |= bEnabled;
	}

	TMap<int32, int32> GroupIndex;
	int32 Matched = 0;
	for (const FArenaRawMarker& M : Builder->GetRawMarkers(TEXT("player_starts")))
	{
		bool bEnabled = true;
		GetBool(M, { TEXT("enabled"), TEXT("bEnabled") }, bEnabled);
		double GroupNum = 0.0;
		GetNum(M, { TEXT("team"), TEXT("TeamNumber"), TEXT("player_start_team") }, GroupNum);
		const int32 Group = int32(GroupNum);

		// our start for this marker: same spot (ours sits on the floor below it)
		APlayerStart* Best = nullptr;
		float BestD = 90.f;
		for (APlayerStart* S : Starts)
		{
			if (!S)
			{
				continue;
			}
			const FVector L = S->GetActorLocation();
			const float D = FVector::Dist2D(L, M.Location);
			if (D < BestD && FMath::Abs(L.Z - M.Location.Z) < 400.f)
			{
				BestD = D;
				Best = S;
			}
		}
		if (!Best)
		{
			// starts our level has none for: the ones in space where players spawn in a fighter (AS-MotherShip) get one
			const FManager* FM = Managers.Find(Group);
			if (FM && FM->bFighter && GetWorld())
			{
				FActorSpawnParameters SP;
				SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				Best = GetWorld()->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), M.Location, FRotator(0.f, M.Rotation.Yaw, 0.f), SP);
				if (Best)
				{
					FighterStarts.Add(Best);
				}
			}
			if (!Best)
			{
				continue;
			}
		}
		++Matched;
		int32* Idx = GroupIndex.Find(Group);
		if (!Idx)
		{
			FSpawnGroup G;
			const FManager* F = Managers.Find(Group);
			G.Role = F ? F->Role : -1;
			G.bInitial = F ? F->bEnabled : false;
			G.Name = FString::Printf(TEXT("group %d%s"), Group, F && !F->Tag.IsEmpty() ? *FString::Printf(TEXT(" '%s'"), *F->Tag) : TEXT(""));
			G.bSpawnInFighter = F && F->bFighter;
			G.Tag = F && !F->Tag.IsEmpty() && F->Tag != TEXT("None") ? FName(*F->Tag) : NAME_None;
			G.bEnabledNow = G.bInitial;
			Idx = &GroupIndex.Add(Group, SpawnGroups.Add(G));
		}
		SpawnGroups[*Idx].Starts.AddUnique(Best);
	}

	if (SpawnGroups.Num() == 0)
	{
		FSpawnGroup All;
		All.Name = TEXT("all starts");
		for (APlayerStart* S : Starts)
		{
			if (S)
			{
				All.Starts.Add(S);
			}
		}
		SpawnGroups.Add(All);
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("AS: %d spawn groups from %d matched starts (%d spawn managers)"), SpawnGroups.Num(), Matched, Managers.Num());
}

void UArenaRules_AS::DropUnreachableStages()
{
	bool bHasAttackerRole = false;
	for (const FSpawnGroup& G : SpawnGroups)
	{
		bHasAttackerRole |= G.Role == 0;
	}
	auto Reachable = [&](const FVector& Target)
	{
		for (const FSpawnGroup& G : SpawnGroups)
		{
			if (!(G.Role == 0 || (G.Role == -1 && !bHasAttackerRole)))
			{
				continue;
			}
			for (const TWeakObjectPtr<APlayerStart>& S : G.Starts)
			{
				if (S.IsValid())
				{
					if (IsReachable(S->GetActorLocation(), Target, true))
					{
						return true;
					}
					break;
				}
			}
		}
		return false;
	};

	// The nav graph is built with the movers at rest. Watching a UT2004 server every objective is the server's (none is
	// dropped); with the map's movers running natively (doors, lifts, the AS-Convoy crane and trucks, driven by the
	// UE2 events objectives fire) an objective the paths do not reach at rest is reached through them, as in UT2004
	const bool bNetWorld = GetWorld() && GetWorld()->URL.HasOption(TEXT("UE2Net"));
	const bool bNativeMovers = ArenaUE2World::HasNativeMovers(GetWorld());
	TArray<AArenaAS_Objective*> Keep, Dropped;
	for (AArenaAS_Objective* Obj : Objectives)
	{
		if (!Obj)
		{
			continue;
		}
		if (Reachable(Obj->GetActorLocation()))
		{
			Keep.Add(Obj);
		}
		else if (bNetWorld || (VehicleObjectives.Contains(Obj->GetIndex()) ? HasAssaultVehicles(true) : bNativeMovers))
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("AS: objective %d (stage %d, '%s') is not reachable with the movers at rest: kept (%s)"),
				Obj->GetIndex(), Obj->GetStage(), *Obj->GetDescription(), bNetWorld ? TEXT("the server's state drives it") : TEXT("the map's movers run"));
			Keep.Add(Obj);
		}
		else
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("AS: dropped objective %d (stage %d, '%s'): no attacker spawn can reach it (closed movers / scripted path)"),
				Obj->GetIndex(), Obj->GetStage(), *Obj->GetDescription());
			SkippedNotes.Add(FString::Printf(TEXT("objective %d unreachable without scripts/movers"), Obj->GetIndex()));
			Dropped.Add(Obj);
		}
	}
	int32 NumRequired = 0;
	for (const AArenaAS_Objective* Obj : Keep)
	{
		NumRequired += Obj->IsOptional() ? 0 : 1;
	}
	if (NumRequired == 0)
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("AS: no reachable objectives; keeping the original chain"));
		Keep.Reset();
		for (AArenaAS_Objective* Obj : Objectives)
		{
			if (Obj)
			{
				Keep.Add(Obj);
			}
		}
		Dropped.Reset();
	}
	for (AArenaAS_Objective* Obj : Dropped)
	{
		Obj->Destroy();
	}
	// renumber: dense stages in the old order, indices = array position
	TArray<int32> OldStages;
	for (const AArenaAS_Objective* Obj : Keep)
	{
		if (!Obj->IsOptional())
		{
			OldStages.AddUnique(Obj->GetStage());
		}
	}
	OldStages.Sort();
	Objectives.Reset();
	for (AArenaAS_Objective* Obj : Keep)
	{
		const int32 NewStage = Obj->IsOptional() ? -1 : OldStages.IndexOfByKey(Obj->GetStage());
		Obj->SetStageAndIndex(NewStage, Objectives.Num());
		Objectives.Add(Obj);
	}
	NumStages = OldStages.Num();
	CurrentStage = 0;
	if (Info)
	{
		Info->NumStages = NumStages;
	}
	SetStage(0);
	UE_LOG(LogOpenUT2k4, Log, TEXT("AS: %d playable objectives in %d stages"), Objectives.Num(), NumStages);
}

void UArenaRules_AS::AssignStageSpawnGroups()
{
	AttackerGroupForStage.Init(INDEX_NONE, NumStages);
	DefenderGroupForStage.Init(INDEX_NONE, NumStages);
	if (SpawnGroups.Num() == 0 || NumStages == 0)
	{
		return;
	}
	bool bHasAttackerRole = false, bHasDefenderRole = false;
	for (const FSpawnGroup& G : SpawnGroups)
	{
		bHasAttackerRole |= G.Role == 0;
		bHasDefenderRole |= G.Role == 1;
	}
	auto FirstStart = [](const FSpawnGroup& G)
	{
		for (const TWeakObjectPtr<APlayerStart>& S : G.Starts)
		{
			if (S.IsValid())
			{
				return S->GetActorLocation();
			}
		}
		return G.Centroid;
	};
	const FVector Final = GetStageLocation(NumStages - 1);

	for (int32 Stage = 0; Stage < NumStages; ++Stage)
	{
		const FVector Obj = GetStageLocation(Stage);

		// ---- attackers ----
		const int32 Prev = Stage > 0 ? AttackerGroupForStage[Stage - 1] : INDEX_NONE;
		const float PrevDist = Prev != INDEX_NONE ? FVector::Dist(SpawnGroups[Prev].Centroid, Obj) : BIG_NUMBER;
		auto PickAttackers = [&](bool bDoorsOpen) -> int32
		{
			int32 Pick = INDEX_NONE;
			float BestScore = BIG_NUMBER;
			for (int32 g = 0; g < SpawnGroups.Num(); ++g)
			{
				const FSpawnGroup& G = SpawnGroups[g];
				if (!(G.Role == 0 || (G.Role == -1 && !bHasAttackerRole)))
				{
					continue;
				}
				const float D = FVector::Dist(G.Centroid, Obj);
				if (!IsReachable(FirstStart(G), Obj, bDoorsOpen))
				{
					continue;
				}
				float Score;
				if (Stage == 0)
				{
					// home spawn: initial groups first; without spawn managers, the group farthest from the final objective
					Score = (G.bInitial ? 0.f : 1.0e7f) + (bHasAttackerRole ? D : -FVector::Dist(G.Centroid, Final));
					if (!G.bInitial && D < ArenaAS::MinAttackerSpawnDist)
					{
						continue;
					}
				}
				else
				{
					// advance: the closest group that is not on top of the objective and not further back than before
					if (g != Prev && (D < ArenaAS::MinAttackerSpawnDist || D > PrevDist + 1.f))
					{
						continue;
					}
					Score = D;
				}
				if (Score < BestScore)
				{
					BestScore = Score;
					Pick = g;
				}
			}
			return Pick;
		};
		int32 Best = PickAttackers(false);
		const int32 BestOpen = Stage > 0 ? PickAttackers(true) : INDEX_NONE;
		if (BestOpen != INDEX_NONE && BestOpen != Best && (Best == INDEX_NONE || (Best == Prev
			&& FVector::Dist(SpawnGroups[BestOpen].Centroid, Obj) * 3.f < FVector::Dist(SpawnGroups[Best].Centroid, Obj))))
		{
			// a forward spawn behind doors the map's script opens, when nothing reachable now is within three times its
			// distance (AS-Convoy's 'Spawnt'; AS-Glacier's hangar stays shut until its stage is done)
			Best = BestOpen;
		}
		if (Best == INDEX_NONE)
		{
			Best = Prev;
		}
		if (Best == INDEX_NONE)
		{
			// nothing reachable: any attacker-capable group, nearest to the objective
			for (int32 g = 0; g < SpawnGroups.Num(); ++g)
			{
				if (SpawnGroups[g].Role != 1 && (Best == INDEX_NONE || FVector::Dist(SpawnGroups[g].Centroid, Obj) < FVector::Dist(SpawnGroups[Best].Centroid, Obj)))
				{
					Best = g;
				}
			}
		}
		AttackerGroupForStage[Stage] = Best;

		// ---- defenders: the nearest reachable defender group ----
		int32 BestDef = INDEX_NONE, AnyDef = INDEX_NONE;
		float BestDefDist = BIG_NUMBER, AnyDefDist = BIG_NUMBER;
		for (int32 g = 0; g < SpawnGroups.Num(); ++g)
		{
			const FSpawnGroup& G = SpawnGroups[g];
			if (g == Best || !(G.Role == 1 || (G.Role == -1 && !bHasDefenderRole)))
			{
				continue;
			}
			const float D = FVector::Dist(G.Centroid, Obj);
			if (D < AnyDefDist)
			{
				AnyDefDist = D;
				AnyDef = g;
			}
			if (D < BestDefDist && IsReachable(FirstStart(G), Obj))
			{
				BestDefDist = D;
				BestDef = g;
			}
		}
		DefenderGroupForStage[Stage] = BestDef != INDEX_NONE ? BestDef : AnyDef;

		UE_LOG(LogOpenUT2k4, Log, TEXT("AS: stage %d spawns: attackers %s, defenders %s"), Stage,
			Best != INDEX_NONE ? *FString::Printf(TEXT("'%s' (%.0f cm away)"), *SpawnGroups[Best].Name, FVector::Dist(SpawnGroups[Best].Centroid, Obj)) : TEXT("default"),
			DefenderGroupForStage[Stage] != INDEX_NONE ? *FString::Printf(TEXT("'%s' (%.0f cm away)"), *SpawnGroups[DefenderGroupForStage[Stage]].Name, FVector::Dist(SpawnGroups[DefenderGroupForStage[Stage]].Centroid, Obj)) : TEXT("default"));
	}
}

AActor* UArenaRules_AS::ChoosePlayerStart(AController* Player, const TArray<APlayerStart*>& Candidates)
{
	EnsureSpawnGroups();
	// a player whose side isn't set yet (the local player at round start, sometimes) starts with the attackers: any
	// other start may stand in a trigger meant for one side (AS-MotherShip's 'Corereached' in the core)
	const int32 PlayerTeam = GetTeamOf(Player);
	const int32 Team = (PlayerTeam == 0 || PlayerTeam == 1) ? PlayerTeam : GetAttackingTeam();
	if (Cast<APlayerController>(Player))
	{
		UE_LOG(LogOpenUT2k4, Verbose, TEXT("AS: start for %s (team %d, attackers %d, %d groups, stage %d)"), *GetNameSafe(Player), PlayerTeam, GetAttackingTeam(), SpawnGroups.Num(), CurrentStage);
	}
	bool bManaged = false;
	for (const FSpawnGroup& G : SpawnGroups)
	{
		bManaged |= !G.Tag.IsNone() && G.Role != -1;
	}
	if ((Team == 0 || Team == 1) && bManaged && (bSpawnsToggled || CurrentStage == 0))
	{
		// UT2004 ASGameInfo: players spawn at the starts of the PlayerSpawnManagers that are enabled for their side
		// (bEnabled at round start, then toggled by the map's script). Until the script toggles one, later stages use
		// the stage table below (our script runtime doesn't run every map's spawn sequence yet)
		const int32 Role = Team == GetAttackingTeam() ? 0 : 1;
		TArray<APlayerStart*> Enabled;
		for (const FSpawnGroup& G : SpawnGroups)
		{
			if (G.bEnabledNow && G.Role == Role && !G.Tag.IsNone())
			{
				for (const TWeakObjectPtr<APlayerStart>& S : G.Starts)
				{
					if (S.IsValid())
					{
						Enabled.Add(S.Get());
					}
				}
			}
		}
		if (Enabled.Num() > 0)
		{
			if (AActor* Start = PickBestStart(Player, Enabled, -1))
			{
				return Start;
			}
		}
	}
	if ((Team == 0 || Team == 1) && NumStages > 0)
	{
		const int32 Stage = FMath::Clamp(CurrentStage, 0, NumStages - 1);
		const TArray<int32>& Table = Team == GetAttackingTeam() ? AttackerGroupForStage : DefenderGroupForStage;
		const int32 G = Table.IsValidIndex(Stage) ? Table[Stage] : INDEX_NONE;
		if (SpawnGroups.IsValidIndex(G))
		{
			TArray<APlayerStart*> Starts;
			for (const TWeakObjectPtr<APlayerStart>& S : SpawnGroups[G].Starts)
			{
				if (S.IsValid())
				{
					Starts.Add(S.Get());
				}
			}
			if (AActor* Start = PickBestStart(Player, Starts, -1))
			{
				return Start;
			}
		}
	}
	return Super::ChoosePlayerStart(Player, Candidates);
}

// =============================================================================================
// Bots
// =============================================================================================

void UArenaRules_AS::GetBotObjectives(AArenaBotController* Bot, TArray<FArenaBotObjective>& OutGoals)
{
	GetBotVehicleObjectives(Bot, OutGoals);
	if (Stall.Kind != 0 && Bot && Bot->GetArenaPawn() && GetTeamOf(Bot) == GetAttackingTeam())
	{
		// the door / mover in the attackers' way (UpdateMoverStall): the two attackers nearest to it see to it, at a
		// priority just under the objectives', so the rest keep pushing instead of crowding one mover
		const float MyD = float(FVector::Dist(Bot->GetArenaPawn()->GetActorLocation(), Stall.GoalLocation));
		int32 Closer = 0;
		for (AController* C : GameMode->GetBots())
		{
			const AArenaBotController* Other = Cast<AArenaBotController>(C);
			if (Other && Other != Bot && Other->GetArenaPawn() && GetTeamOf(Other) == GetAttackingTeam()
				&& FVector::Dist(Other->GetArenaPawn()->GetActorLocation(), Stall.GoalLocation) < MyD)
			{
				++Closer;
			}
		}
		if (Closer < 2)
		{
			FArenaBotObjective O(Stall.GoalLocation, Stall.Opener.Get(), 1.3f, EArenaBotAction::Move, Tag_Mover);
			O.bCompleteOnReach = false;
			OutGoals.Add(O);
		}
	}
	AArenaCharacter* P = Bot ? Bot->GetArenaPawn() : nullptr;
	const int32 Team = GetTeamOf(Bot);
	if (!P || (Team != 0 && Team != 1))
	{
		return;
	}
	const bool bAttacker = Team == GetAttackingTeam();
	const FVector MyLoc = P->GetActorLocation();
	FRandomStream& Rng = Bot->GetRandomStream();
	AArenaNavGraph* Nav = GetNavGraph();
	auto Snap = [Nav](const FVector& Floor, float MaxMove) -> FVector
	{
		const FVector Want = Floor + FVector(0.f, 0.f, 90.f);
		if (Nav)
		{
			const int32 Node = Nav->FindNearestNode(Want, false);
			if (Node != INDEX_NONE && Nav->GetNodes().IsValidIndex(Node) && FVector::Dist(Nav->GetNodes()[Node].Location, Want) < MaxMove)
			{
				return Nav->GetNodes()[Node].Location;
			}
		}
		return Want;
	};

	for (AArenaAS_Objective* Obj : Objectives)
	{
		if (!Obj || !Obj->IsActive())
		{
			continue;
		}
		const FVector L = Obj->GetActorLocation();
		const float Dist = FVector::Dist(MyLoc, L);
		const int32 Idx = Obj->GetIndex();
		if (bAttacker)
		{
			const float Pri = Obj->IsOptional() ? (Dist < 2500.f ? 0.9f : 0.3f) : 1.4f;
			FArenaBotObjective O;
			O.Actor = Obj;
			O.Priority = Pri;
			O.Tag = Tag_Attack + Idx;
			switch (Obj->GetType())
			{
			case EArenaASObjectiveType::Touch:
				O.Location = L + FVector(0.f, 0.f, 90.f);
				O.Action = EArenaBotAction::Collect;
				O.bCompleteOnReach = true;
				break;
			case EArenaASObjectiveType::Hold:
			{
				const FVector2D Off = FVector2D(Rng.FRandRange(-1.f, 1.f), Rng.FRandRange(-1.f, 1.f)).GetSafeNormal() * Rng.FRandRange(0.f, Obj->GetRadius() * 0.5f);
				O.Location = L + FVector(Off.X, Off.Y, 90.f);
				O.Action = EArenaBotAction::Defend;
				O.bCompleteOnReach = false;
				break;
			}
			case EArenaASObjectiveType::Use:
				O.Location = L + FVector(0.f, 0.f, 90.f);
				O.Action = EArenaBotAction::Use;
				O.bCompleteOnReach = false;
				break;
			case EArenaASObjectiveType::Destroy:
			{
				FVector Dir = (MyLoc - L).GetSafeNormal2D();
				if (Dir.IsNearlyZero())
				{
					Dir = FVector(1.f, 0.f, 0.f);
				}
				Dir = Dir.RotateAngleAxis(Rng.FRandRange(-35.f, 35.f), FVector::UpVector);
				O.Location = Snap(L + Dir * Rng.FRandRange(450.f, 750.f), 500.f);
				O.Action = EArenaBotAction::Attack;
				O.bCompleteOnReach = false;
				break;
			}
			}
			OutGoals.Add(O);
		}
		else if (!Obj->IsOptional())
		{
			// Defenders: guard the current objective; rush it when attackers work on it or are close
			bool bThreat = Obj->IsBeingWorked() || (Obj->GetType() != EArenaASObjectiveType::Destroy && Obj->GetProgress() > 0.02f);
			if (!bThreat)
			{
				for (TActorIterator<AArenaCharacter> It(GetWorld()); It; ++It)
				{
					if (It->IsAlive() && It->GetTeam() == 1 - Team && FVector::Dist(It->GetActorLocation(), L) < 1300.f)
					{
						bThreat = true;
						break;
					}
				}
			}
			FArenaBotObjective O;
			O.Actor = Obj;
			if (bThreat)
			{
				const float R = Obj->GetType() == EArenaASObjectiveType::Destroy ? 260.f : Obj->GetRadius() * 0.4f;
				const FVector2D Off = FVector2D(Rng.FRandRange(-1.f, 1.f), Rng.FRandRange(-1.f, 1.f)).GetSafeNormal() * R;
				O.Location = L + FVector(Off.X, Off.Y, 90.f);
				O.Priority = Dist < 3000.f ? 1.6f : 1.1f;
				O.Action = EArenaBotAction::Attack;
				O.bCompleteOnReach = false;
				O.Tag = Tag_Intercept + Idx;
			}
			else
			{
				const float A = Rng.FRandRange(0.f, 2.f * PI);
				const float R = Rng.FRandRange(300.f, 900.f);
				O.Location = Snap(L + FVector(FMath::Cos(A) * R, FMath::Sin(A) * R, 0.f), 450.f);
				O.Priority = Dist > 2000.f ? 1.2f : 0.85f;
				O.Action = EArenaBotAction::Defend;
				O.bCompleteOnReach = true;
				O.Tag = Tag_Guard + Idx;
			}
			OutGoals.Add(O);
		}
	}
}

bool UArenaRules_AS::UpdateBotObjective(AArenaBotController* Bot, FArenaBotObjective& Objective)
{
	const int32 Base = (Objective.Tag / 1000) * 1000;
	const int32 Idx = Objective.Tag - Base;
	if (Base == Tag_Mover)
	{
		AArenaCharacter* P = Bot ? Bot->GetArenaPawn() : nullptr;
		if (!P || Stall.Kind == 0)
		{
			return false;
		}
		Objective.Location = Stall.GoalLocation;
		const float D = float(FVector::Dist(P->GetActorLocation(), Stall.GoalLocation));
		if (Stall.Kind == 2 && D < 250.f)
		{
			FireUseTrigger(P->GetActorLocation(), Bot);
		}
		else if (Stall.Kind == 3 && D < 2500.f && Stall.Opener.IsValid())
		{
			Bot->SetShootTarget(Stall.Opener.Get(), 1.5f);
		}
		return true;
	}
	if (Base == Tag_Vehicle || Base == Tag_Turret)
	{
		// board the vehicle / man the turret once there (UE2 bots use them the same way)
		AArenaCharacter* P = Bot ? Bot->GetArenaPawn() : nullptr;
		APawn* Target = AssaultVehicles.IsValidIndex(Idx) ? AssaultVehicles[Idx].Pawn.Get() : nullptr;
		if (!P || !Target)
		{
			return false;
		}
		Objective.Location = Target->GetActorLocation();
		if (AArenaAS_Turret* T = Cast<AArenaAS_Turret>(Target))
		{
			if (!T->CanBeEnteredBy(P) && FVector::Dist(P->GetActorLocation(), T->GetActorLocation()) < T->GetTurretDesc().EntryRadius + 150.f)
			{
				return false; // taken / destroyed
			}
			if (T->IsManned() || T->IsDestroyedTurret())
			{
				return false;
			}
			if (T->CanBeEnteredBy(P))
			{
				T->EnterTurret(P);
				return false;
			}
			return true;
		}
		AArenaONS_Vehicle* V = Cast<AArenaONS_Vehicle>(Target);
		if (!V || !V->CanBeEnteredBy(P))
		{
			return false;
		}
		if (FVector::Dist(P->GetActorLocation(), V->GetActorLocation()) < V->GetSpec().HalfExtent.X + 250.f)
		{
			V->EnterSeat(P, V->GetFreeSeat());
			return false;
		}
		return true;
	}
	if (Base != Tag_Attack && Base != Tag_Guard && Base != Tag_Intercept)
	{
		return Super::UpdateBotObjective(Bot, Objective);
	}
	AArenaAS_Objective* Obj = Objectives.IsValidIndex(Idx) ? Objectives[Idx].Get() : nullptr;
	AArenaCharacter* P = Bot ? Bot->GetArenaPawn() : nullptr;
	if (!Obj || !Obj->IsActive() || !P)
	{
		StopBotFire(Bot);
		return false;
	}
	const bool bAttacker = GetTeamOf(Bot) == GetAttackingTeam();
	if (bAttacker != (Base == Tag_Attack))
	{
		return false; // sides changed
	}
	if (Base == Tag_Attack)
	{
		if (Obj->GetType() == EArenaASObjectiveType::Use && Obj->IsInUseRange(P))
		{
			Bot->RequestUse();
		}
		else if (Obj->GetType() == EArenaASObjectiveType::Destroy)
		{
			BotAttackTime.Add(Bot, GetWorld()->GetTimeSeconds());
		}
	}
	else if (Base == Tag_Guard && Obj->IsBeingWorked())
	{
		return false; // go and intercept
	}
	return true;
}

void UArenaRules_AS::StopBotFire(AArenaBotController* Bot)
{
	if (!Bot || !FiringBots.Contains(Bot))
	{
		return;
	}
	FiringBots.Remove(Bot);
	if (AArenaCharacter* P = Bot->GetArenaPawn())
	{
		P->StopFire(0);
	}
}

void UArenaRules_AS::UpdateBotDestroyFire(float DeltaSeconds)
{
	if (!GameMode)
	{
		return;
	}
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();
	for (AController* C : GameMode->GetBots())
	{
		AArenaBotController* Bot = Cast<AArenaBotController>(C);
		if (!Bot)
		{
			continue;
		}
		const FArenaBotObjective* Cur = Bot->GetCurrentObjective();
		const float* Asked = BotAttackTime.Find(Bot);
		AArenaAS_Objective* Obj = nullptr;
		if (Cur && Asked && Now - *Asked < 0.5f && Cur->Tag >= Tag_Attack && Cur->Tag < Tag_Attack + 1000)
		{
			const int32 Idx = Cur->Tag - Tag_Attack;
			Obj = Objectives.IsValidIndex(Idx) ? Objectives[Idx].Get() : nullptr;
		}
		AArenaCharacter* P = Bot->GetArenaPawn();
		if (!Obj || !P || !P->IsAlive() || !Obj->IsActive() || Obj->GetType() != EArenaASObjectiveType::Destroy || Bot->IsEnemyVisible())
		{
			StopBotFire(Bot);
			continue;
		}
		const FVector Eye = P->GetPawnViewLocation();
		const FVector Target = Obj->GetMarkerLocation();
		if (FVector::Dist(Eye, Target) > ArenaAS::BotDestroyRange)
		{
			StopBotFire(Bot);
			continue;
		}
		FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaASBotAim), false, P);
		FHitResult Hit;
		const bool bBlocked = World->LineTraceSingleByChannel(Hit, Eye, Target, ECC_Visibility, QP) && Hit.GetActor() != Obj;
		if (bBlocked)
		{
			StopBotFire(Bot);
			continue;
		}
		// Turn towards it (the bot's own aim smoothing does the turning) and fire once roughly on target
		Bot->NotifyTookDamage(nullptr, Target);
		const FVector Fwd = Bot->GetControlRotation().Vector();
		const bool bOnTarget = FVector::DotProduct(Fwd, (Target - Eye).GetSafeNormal()) > FMath::Cos(FMath::DegreesToRadians(12.f));
		if (bOnTarget)
		{
			if (!FiringBots.Contains(Bot))
			{
				P->StartFire(0);
				FiringBots.Add(Bot);
			}
			// Assist damage: bot aim at a static target is crude, so their fire is backed by a small steady rate
			Obj->ApplyAttackDamage(ArenaAS::BotAssistDps * DeltaSeconds, Bot);
			if (Obj->IsCompleted())
			{
				StopBotFire(Bot);
			}
		}
	}
}

float UArenaRules_AS::GetBotEnemyWeight(AArenaBotController* Bot, AArenaCharacter* Candidate) const
{
	if (!Candidate)
	{
		return 1.f;
	}
	for (const AArenaAS_Objective* Obj : Objectives)
	{
		if (Obj && Obj->IsActive() && !Obj->IsOptional() && FVector::Dist(Obj->GetActorLocation(), Candidate->GetActorLocation()) < 1500.f)
		{
			return 0.6f; // whoever is at the objective matters most
		}
	}
	return 1.f;
}

// =============================================================================================
// HUD
// =============================================================================================

void UArenaRules_AS::GatherObjectives(TArray<AArenaAS_Objective*>& Out) const
{
	if (Objectives.Num() > 0)
	{
		for (AArenaAS_Objective* Obj : Objectives)
		{
			if (Obj)
			{
				Out.Add(Obj);
			}
		}
		return;
	}
	// network client: find the replicated actors
	UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.f;
	if (World && (Now - HudRefreshTime > 1.f || HudObjectives.Num() == 0))
	{
		HudRefreshTime = Now;
		HudObjectives.Reset();
		for (TActorIterator<AArenaAS_Objective> It(World); It; ++It)
		{
			HudObjectives.Add(*It);
		}
	}
	for (const TWeakObjectPtr<AArenaAS_Objective>& W : HudObjectives)
	{
		if (W.IsValid())
		{
			Out.Add(W.Get());
		}
	}
	Out.Sort([](const AArenaAS_Objective& A, const AArenaAS_Objective& B) { return A.GetIndex() < B.GetIndex(); });
}

bool UArenaRules_AS::DrawScoreInfo(AArenaHUD* HUD, UCanvas* Canvas)
{
	const AArenaGameState* GS = GetGameState();
	const AArenaAS_Info* I = FindInfo();
	if (!HUD || !GS)
	{
		return false;
	}
	if (HUD->IsUTHud())
	{
		// HUD_Assault: no team scores, timer or adrenaline; the round time pod top left (no reinforcement waves here)
		HUD->DrawUTAssault(GS->RemainingTime, -1, GS->TimeLimit > 0);
		return true;
	}
	const FArenaDraw& D = HUD->GetDraw();
	const int32 Attack = I ? I->AttackingTeam : GetAttackingTeam();
	HUD->DrawTeamScorePanels(24.f, [&](int32 Team, float X, float Top, float BW)
	{
		using namespace ArenaHudStyle;
		const bool bAtt = Team == Attack;
		const float Y = Top + D.S(86.f);
		D.TextMid(bAtt ? TEXT("ATTACKING") : TEXT("DEFENDING"), X + D.S(12.f), Y, bAtt ? TeamTextColor(Team) : TextDim(), D.S(14.f));
		if (I && I->PairRound == 1 && I->FirstAttackTeam == Team)
		{
			const FString T = I->bFirstSucceeded ? FormatTime(FMath::CeilToInt(I->FirstTime)) : FString::Printf(TEXT("%d/%d"), I->FirstStages, I->NumStages);
			D.TextMid(T, X + BW - D.S(12.f), Y, TextWhite(), D.S(14.f), EArenaAlign::Right);
		}
	}, GetGoalText(GS->GoalScore));
	return true;
}

void UArenaRules_AS::DrawHUD(AArenaHUD* HUD, UCanvas* Canvas)
{
	Super::DrawHUD(HUD, Canvas);
	const AArenaGameState* GS = GetGameState();
	const AArenaAS_Info* I = FindInfo();
	if (!HUD || !Canvas || !GS || GS->Phase == EArenaMatchPhase::Ended)
	{
		return;
	}
	using namespace ArenaHudStyle;
	const FArenaDraw& D = HUD->GetDraw();
	APlayerController* PC = HUD->GetOwningPlayerController();
	const AArenaPlayerState* MyPS = PC ? PC->GetPlayerState<AArenaPlayerState>() : nullptr;
	const int32 MyTeam = MyPS ? MyPS->Team : ArenaTeam::None;
	const int32 Attack = I ? I->AttackingTeam : GetAttackingTeam();
	const int32 Stage = I ? I->CurrentStage : CurrentStage;
	const int32 Stages = I ? FMath::Max(1, I->NumStages) : FMath::Max(1, NumStages);
	const float Now = HUD->Now();

	TArray<AArenaAS_Objective*> All;
	GatherObjectives(All);
	if (HUD->IsUTHud())
	{
		// HUD_Assault.DrawObjectives: the active objectives' type icons where they are visible, with their progress bar
		for (const AArenaAS_Objective* Obj : All)
		{
			if (Obj && Obj->IsActive())
			{
				const float Progress = Obj->GetType() == EArenaASObjectiveType::Destroy ? Obj->GetHealthFraction()
					: (Obj->GetWorkFraction() > 0.f ? 1.f - Obj->GetWorkFraction() : 0.f);
				HUD->DrawUTObjectiveMarker(Obj->GetMarkerLocation(), int32(Obj->GetType()), Obj->GetDefendingTeam(), Progress, Obj->GetIndex());
			}
		}
		return;
	}
	TArray<AArenaAS_Objective*> Current, Bonus;
	for (AArenaAS_Objective* Obj : All)
	{
		if (Obj->IsActive())
		{
			(Obj->IsOptional() ? Bonus : Current).Add(Obj);
		}
	}

	// ---- objective panel under the timer ----
	const float PW = D.S(600.f);
	const float PH = D.S(96.f);
	const float PX = (D.W() - PW) * 0.5f;
	const float PY = D.S(94.f);
	const bool bMeAttacking = MyTeam == Attack;
	const int32 RoleTeam = (MyTeam == 0 || MyTeam == 1) ? MyTeam : Attack;
	D.Panel(PX, PY, PW, PH, TeamTint(RoleTeam));

	FString Role;
	if (MyTeam == 0 || MyTeam == 1)
	{
		Role = bMeAttacking ? TEXT("YOU ARE ATTACKING") : TEXT("YOU ARE DEFENDING");
	}
	else
	{
		Role = FString::Printf(TEXT("%s ATTACKS"), *ArenaTeam::Name(Attack).ToUpper());
	}
	FString RoundText = FString::Printf(TEXT("ROUND %d"), GS->Round);
	if (I && I->bOvertimePair)
	{
		RoundText += TEXT("  OVERTIME");
	}
	D.Text(RoundText, PX + D.S(14.f), PY + D.S(8.f), TextDim(), D.S(15.f));
	D.Text(Role, PX + PW * 0.5f, PY + D.S(8.f), TeamTextColor(RoleTeam), D.S(15.f), EArenaAlign::Center);
	FString Beat;
	FLinearColor BeatColor = TextWhite();
	if (I && I->PairRound == 1)
	{
		Beat = I->TimeToBeat >= 0.f ? FString::Printf(TEXT("TIME TO BEAT %s"), *FormatTime(FMath::CeilToInt(I->TimeToBeat))) : FString(TEXT("COMPLETE TO WIN"));
		BeatColor = FLinearColor(1.f, 0.85f, 0.3f);
	}
	else
	{
		Beat = TEXT("SET THE TIME");
		BeatColor = TextDim();
	}
	D.Text(Beat, PX + PW - D.S(14.f), PY + D.S(8.f), BeatColor, D.S(15.f), EArenaAlign::Right);

	if (Current.Num() > 0)
	{
		const AArenaAS_Objective* Main = Current[0];
		// objective type icon at the left of the panel
		{
			using namespace ArenaArtHUD;
			FHudSprite Icon = Sprites::NodeTarget();
			switch (Main->GetType())
			{
			case EArenaASObjectiveType::Destroy: Icon = Sprites::NodeAttack(); break;
			case EArenaASObjectiveType::Hold:    Icon = Sprites::DomRing(); break;
			case EArenaASObjectiveType::Use:     Icon = Sprites::NodeLocked(); break;
			default: break;
			}
			DrawSpriteCentered(D, Icon, PX + D.S(46.f), PY + D.S(46.f), D.S(34.f), WithAlpha(TeamTextColor(RoleTeam), 0.9f));
		}
		const FString Line = FString::Printf(TEXT("%s  %s%s"), *AArenaAS_Objective::TypeLabel(Main->GetType()), *Main->GetDescription(),
			Current.Num() > 1 ? *FString::Printf(TEXT("  (+%d)"), Current.Num() - 1) : TEXT(""));
		D.Text(Line, PX + PW * 0.5f, PY + D.S(30.f), TextWhite(), D.S(24.f), EArenaAlign::Center);

		// progress / health
		float Frac = 0.f;
		FString BarText;
		FLinearColor BarColor = TeamTint(Attack);
		switch (Main->GetType())
		{
		case EArenaASObjectiveType::Destroy:
			Frac = Main->GetHealthFraction();
			BarText = FString::Printf(TEXT("INTEGRITY %d%%"), FMath::RoundToInt(Frac * 100.f));
			BarColor = TeamTint(Main->GetDefendingTeam());
			break;
		case EArenaASObjectiveType::Hold:
			Frac = Main->GetProgress();
			BarText = Main->IsContested() ? FString(TEXT("CONTESTED")) : FString::Printf(TEXT("SECURED %d%%"), FMath::RoundToInt(Frac * 100.f));
			break;
		case EArenaASObjectiveType::Use:
			Frac = Main->GetProgress();
			BarText = (bMeAttacking && Frac <= 0.f) ? FString(TEXT("HOLD USE AT THE CONSOLE")) : FString::Printf(TEXT("OVERRIDE %d%%"), FMath::RoundToInt(Frac * 100.f));
			break;
		case EArenaASObjectiveType::Touch:
			BarText = bMeAttacking ? TEXT("GET THERE") : TEXT("KEEP THEM OUT");
			break;
		}
		const float BX = PX + D.S(110.f), BY = PY + D.S(68.f), BW = PW - D.S(220.f), BH = D.S(14.f);
		if (Main->GetType() != EArenaASObjectiveType::Touch)
		{
			D.Bar(BX, BY, BW, BH, Frac, Main->IsContested() ? WithAlpha(FLinearColor(1.f, 0.7f, 0.2f), 0.6f + 0.4f * FMath::Sin(Now * 8.f)) : BarColor);
		}
		D.TextMid(BarText, PX + PW * 0.5f, BY + BH * 0.5f, TextWhite(), D.S(13.f), EArenaAlign::Center);
		D.TextMid(FString::Printf(TEXT("%d / %d"), FMath::Min(Stage + 1, Stages), Stages), PX + D.S(14.f), BY + BH * 0.5f, TextDim(), D.S(15.f));
	}
	else
	{
		D.Text(GS->Phase == EArenaMatchPhase::RoundOver ? TEXT("Round over") : TEXT("Stand by"), PX + PW * 0.5f, PY + D.S(34.f), TextDim(), D.S(22.f), EArenaAlign::Center);
	}
	float BonusY = PY + PH + D.S(4.f);
	for (const AArenaAS_Objective* B : Bonus)
	{
		D.Text(FString::Printf(TEXT("BONUS  %s  %s"), *AArenaAS_Objective::TypeLabel(B->GetType()), *B->GetDescription()), PX + PW * 0.5f, BonusY,
			WithAlpha(FLinearColor(0.7f, 0.9f, 1.f), 0.85f), D.S(14.f), EArenaAlign::Center);
		BonusY += D.S(18.f);
	}

	// ---- world markers / edge arrows ----
	if (!PC)
	{
		return;
	}
	FVector CamLoc;
	FRotator CamRot;
	PC->GetPlayerViewPoint(CamLoc, CamRot);
	const FVector CamFwd = CamRot.Vector();
	const FVector CamRight = FRotationMatrix(CamRot).GetScaledAxis(EAxis::Y);
	const FVector CamUp = FRotationMatrix(CamRot).GetScaledAxis(EAxis::Z);
	const float W = D.W(), H = D.H();
	const float Margin = D.S(48.f);

	auto DrawObjectiveMarker = [&](const AArenaAS_Objective* Obj, bool bBonus)
	{
		const FVector WorldLoc = Obj->GetMarkerLocation() + FVector(0.f, 0.f, 60.f);
		const FVector ToObj = WorldLoc - CamLoc;
		const float Dist = ToObj.Size();
		const FLinearColor Col = bBonus ? FLinearColor(0.6f, 0.9f, 1.f) : (bMeAttacking || MyTeam < 0 ? TeamTextColor(Attack) : TeamTextColor(1 - Attack));
		const FString DistText = FString::Printf(TEXT("%dm"), FMath::RoundToInt(Dist / 100.f));
		if (FVector::Dist2D(WorldLoc, CamLoc) < Obj->GetRadius() + 60.f && FMath::Abs(ToObj.Z) < 400.f)
		{
			return; // standing on it
		}
		const bool bInFront = FVector::DotProduct(ToObj, CamFwd) > 1.f;
		FVector2D Screen(0.f, 0.f);
		bool bOnScreen = false;
		if (bInFront)
		{
			const FVector P = Canvas->Project(WorldLoc);
			Screen = FVector2D(P.X, P.Y);
			bOnScreen = Screen.X > Margin && Screen.X < W - Margin && Screen.Y > Margin && Screen.Y < H - Margin;
		}
		const float Pulse = 0.75f + 0.25f * FMath::Sin(Now * 4.f);
		if (bOnScreen)
		{
			const float R = D.S(bBonus ? 9.f : 13.f);
			const float X = Screen.X, Y = Screen.Y;
			const FLinearColor C = WithAlpha(Col, Pulse);
			if (!ArenaArtHUD::DrawSpriteCentered(D, ArenaArtHUD::Sprites::NodeTarget(), X, Y, R * 2.4f, C))
			{
				D.Line(X, Y - R, X + R, Y, C, D.S(2.5f));
				D.Line(X + R, Y, X, Y + R, C, D.S(2.5f));
				D.Line(X, Y + R, X - R, Y, C, D.S(2.5f));
				D.Line(X - R, Y, X, Y - R, C, D.S(2.5f));
			}
			D.Text(AArenaAS_Objective::TypeLabel(Obj->GetType()), X, Y - R - D.S(20.f), C, D.S(13.f), EArenaAlign::Center);
			D.Text(DistText, X, Y + R + D.S(3.f), WithAlpha(TextWhite(), 0.9f), D.S(13.f), EArenaAlign::Center);
			// health bar over destroy objectives
			if (Obj->GetType() == EArenaASObjectiveType::Destroy)
			{
				const float BW = D.S(90.f), BH = D.S(7.f);
				D.Bar(X - BW * 0.5f, Y + R + D.S(20.f), BW, BH, Obj->GetHealthFraction(), TeamTint(Obj->GetDefendingTeam()));
			}
			else if (Obj->GetProgress() > 0.f)
			{
				const float BW = D.S(70.f), BH = D.S(5.f);
				D.Bar(X - BW * 0.5f, Y + R + D.S(20.f), BW, BH, Obj->GetProgress(), TeamTint(Obj->GetAttackingTeam()));
			}
		}
		else
		{
			// arrow at the screen edge pointing towards the objective
			FVector2D Dir(FVector::DotProduct(ToObj, CamRight), -FVector::DotProduct(ToObj, CamUp));
			if (bInFront)
			{
				Dir = Screen - FVector2D(W * 0.5f, H * 0.5f);
			}
			if (Dir.IsNearlyZero())
			{
				Dir = FVector2D(0.f, 1.f);
			}
			Dir.Normalize();
			const FVector2D Centre(W * 0.5f, H * 0.5f);
			const float EX = W * 0.5f - Margin, EY = H * 0.5f - Margin;
			const float T = FMath::Min(FMath::Abs(Dir.X) > KINDA_SMALL_NUMBER ? EX / FMath::Abs(Dir.X) : BIG_NUMBER, FMath::Abs(Dir.Y) > KINDA_SMALL_NUMBER ? EY / FMath::Abs(Dir.Y) : BIG_NUMBER);
			const FVector2D Tip = Centre + Dir * T;
			const FVector2D Side(-Dir.Y, Dir.X);
			const float L = D.S(bBonus ? 14.f : 20.f);
			const FVector2D A = Tip - Dir * L + Side * L * 0.6f;
			const FVector2D B = Tip - Dir * L - Side * L * 0.6f;
			const FLinearColor C = WithAlpha(Col, Pulse);
			D.Line(Tip.X, Tip.Y, A.X, A.Y, C, D.S(3.f));
			D.Line(Tip.X, Tip.Y, B.X, B.Y, C, D.S(3.f));
			D.Line(A.X, A.Y, B.X, B.Y, C, D.S(2.f));
			const FVector2D TextPos = Tip - Dir * (L + D.S(18.f));
			D.TextMid(DistText, TextPos.X, TextPos.Y, WithAlpha(TextWhite(), 0.85f), D.S(13.f), EArenaAlign::Center);
		}
	};
	for (const AArenaAS_Objective* Obj : Current)
	{
		DrawObjectiveMarker(Obj, false);
	}
	for (const AArenaAS_Objective* Obj : Bonus)
	{
		DrawObjectiveMarker(Obj, true);
	}
}

FString UArenaRules_AS::GetScoreboardExtra(const AArenaPlayerState* Player) const
{
	return Player && Player->ObjectiveScore > 0 ? FString::Printf(TEXT("%d obj"), Player->ObjectiveScore) : FString();
}

FString UArenaRules_AS::GetScoreboardHeaderExtra(const AArenaGameState* GS) const
{
	const AArenaAS_Info* I = FindInfo();
	const int32 Attack = I ? I->AttackingTeam : GetAttackingTeam();
	return FString::Printf(TEXT("Round %d - %s attacking"), GS ? GS->Round : 1, *ArenaTeam::Name(Attack));
}

// =============================================================================================
// Assault vehicles and turrets (the map's ASVehicleFactory actors)
// =============================================================================================

namespace ArenaASLocal
{
	/** Our vehicle type for an ONS vehicle class an Assault factory names (the closest one for the others). */
	bool VehicleTypeFor(const FString& ClassPath, EArenaONS_VehicleType& Out)
	{
		const FString C = ClassPath.ToLower();
		struct FMap { const TCHAR* Part; EArenaONS_VehicleType Type; };
		static const FMap Table[] = {
			{ TEXT("hoverbike"), EArenaONS_VehicleType::Wisp }, { TEXT("dualattackcraft"), EArenaONS_VehicleType::Kestrel },
			{ TEXT("attackcraft"), EArenaONS_VehicleType::Kestrel }, { TEXT("harrier"), EArenaONS_VehicleType::Kestrel },
			{ TEXT("prv"), EArenaONS_VehicleType::Caravan }, { TEXT("bender"), EArenaONS_VehicleType::Caravan },
			{ TEXT("onsrv"), EArenaONS_VehicleType::Dustrunner }, { TEXT("camoonsrv"), EArenaONS_VehicleType::Dustrunner },
			{ TEXT("hovertank"), EArenaONS_VehicleType::Bulwark }, { TEXT("shocktank"), EArenaONS_VehicleType::Bulwark },
			{ TEXT("artillery"), EArenaONS_VehicleType::Bulwark }, { TEXT("camost"), EArenaONS_VehicleType::Bulwark },
			{ TEXT("hellspawn"), EArenaONS_VehicleType::Bulwark },
		};
		for (const FMap& M : Table)
		{
			if (C.Contains(M.Part))
			{
				Out = M.Type;
				return true;
			}
		}
		return false;
	}
}

void UArenaRules_AS::ReadAssaultVehicles(const FString& MapName)
{
	AssaultVehicles.Reset();
	UseTriggers.Reset();
	FString Text;
	TSharedPtr<FJsonObject> Root;
	const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
	if (!FFileHelper::LoadFileToString(Text, *(ArenaRuntimeMap::GetMapDir(MapName) / TEXT("map.json"))) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root)
		|| !Root.IsValid() || !Root->TryGetArrayField(TEXT("actors"), Actors))
	{
		return;
	}
	const bool bScripts = ArenaUScript::EnsureRuntime();
	int32 NumTurrets = 0, NumVehicles = 0, NumSkipped = 0;
	for (const TSharedPtr<FJsonValue>& V : *Actors)
	{
		const TSharedPtr<FJsonObject> A = V.IsValid() ? V->AsObject() : nullptr;
		FString Class, Name;
		if (A.IsValid() && A->TryGetStringField(TEXT("class"), Class) && Class.Contains(TEXT("UseTrigger")))
		{
			FString Event;
			FUseTrigger U;
			U.Location = ArenaASLocal::JsonVector(A, TEXT("location"), FVector::ZeroVector);
			double Radius = 0.0;
			U.Radius = A->TryGetNumberField(TEXT("collision_radius"), Radius) && Radius > 0.0 ? FMath::Max(120.f, float(Radius) * 2.f + 60.f) : 150.f;
			if (A->TryGetStringField(TEXT("event"), Event) && !Event.IsEmpty() && Event != TEXT("None"))
			{
				U.Event = FName(*Event);
				UseTriggers.Add(U);
			}
			continue;
		}
		if (!A.IsValid() || Class.IsEmpty() || !Class.Contains(TEXT("VehicleFactory")) || !Class.Contains(TEXT("AS")))
		{
			continue;
		}
		A->TryGetStringField(TEXT("name"), Name);
		const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
		const TSharedPtr<FJsonObject> Props = A->TryGetObjectField(TEXT("properties"), PropsPtr) ? *PropsPtr : MakeShared<FJsonObject>();
		FString VehicleClass, Event;
		Props->TryGetStringField(TEXT("VehicleClass"), VehicleClass);
		Props->TryGetStringField(TEXT("VehicleEvent"), Event);
		const TSharedPtr<FJsonObject>* RotObj = nullptr;
		FRotator Rot = FRotator::ZeroRotator;
		if (A->TryGetObjectField(TEXT("rotation"), RotObj))
		{
			Rot = FRotator((*RotObj)->GetNumberField(TEXT("pitch")), (*RotObj)->GetNumberField(TEXT("yaw")), (*RotObj)->GetNumberField(TEXT("roll")));
		}
		FAssaultVehicle AV;
		AV.Transform = FTransform(Rot, ArenaASLocal::JsonVector(A, TEXT("location"), FVector::ZeroVector));
		AV.VehicleEvent = Event == TEXT("None") ? FString() : Event;
		AV.Source = Name;
		// VehicleTeam: 1 = the defenders (the stock factories' default for turrets and sentinels)
		double TeamNum = -1.0;
		bool bTeam = Props->TryGetNumberField(TEXT("VehicleTeam"), TeamNum);
		if (bScripts && ArenaASTurret::DescribeFactory(Class, VehicleClass, AV.Turret))
		{
			AV.bTurret = true;
			AV.RelTeam = bTeam ? int32(TeamNum) : AV.Turret.Team;
			UE_LOG(LogOpenUT2k4, Log, TEXT("AS: turret %s (%s), damage type %s: \"%s\""), *AV.Turret.DisplayName, *Class, *AV.Turret.DamageType,
				*ArenaASTurret::KillText(AV.Turret, TEXT("Someone")));
			AssaultVehicles.Add(AV);
			++NumTurrets;
			continue;
		}
		if (VehicleClass.IsEmpty() && bScripts)
		{
			if (FUSClass* FC = FUSRuntime::Get().FindClass(FString(TEXT("UT2k4Assault.")) + Class))
			{
				VehicleClass = ArenaASLocal::ScriptDefault(FC, TEXT("VehicleClass"));
				if (!bTeam)
				{
					const FString T = ArenaASLocal::ScriptDefault(FC, TEXT("VehicleTeam"));
					TeamNum = T.IsEmpty() ? 0.0 : FCString::Atod(*T);
					bTeam = !T.IsEmpty();
				}
			}
		}
		if (ArenaASLocal::VehicleTypeFor(VehicleClass, AV.Type))
		{
			AV.bTurret = false;
			// Defensive factories are the defenders'; the others belong to the attackers unless the map says otherwise
			AV.RelTeam = bTeam ? int32(TeamNum) : (Class.Contains(TEXT("Defensive")) ? 1 : 0);
			AV.Transform.AddToTranslation(FVector(0.f, 0.f, AArenaONS_Vehicle::GetSpec(AV.Type).HalfExtent.Z + 30.f));
			AssaultVehicles.Add(AV);
			++NumVehicles;
		}
		else
		{
			++NumSkipped;
			SkippedNotes.Add(FString::Printf(TEXT("%s (%s %s): no vehicle of ours"), *Name, *Class, *VehicleClass));
		}
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("AS: %d turrets / sentinels and %d vehicles from the map's vehicle factories (%d skipped)"), NumTurrets, NumVehicles, NumSkipped);
}

bool UArenaRules_AS::HasAssaultVehicles(bool bForAttackers) const
{
	for (const FSpawnGroup& G : SpawnGroups)
	{
		if (G.bSpawnInFighter && (G.Role == 0) == bForAttackers)
		{
			return true; // players spawn in fighters there
		}
	}
	for (const FAssaultVehicle& AV : AssaultVehicles)
	{
		if (!AV.bTurret && (AV.RelTeam == 0) == bForAttackers)
		{
			return true;
		}
	}
	return false;
}

void UArenaRules_AS::SpawnAssaultVehicle(int32 Index)
{
	UWorld* World = GetWorld();
	if (!World || !AssaultVehicles.IsValidIndex(Index))
	{
		return;
	}
	FAssaultVehicle& AV = AssaultVehicles[Index];
	const int32 Attack = GetAttackingTeam();
	const int32 Team = AV.RelTeam == 1 ? 1 - Attack : Attack;
	if (AV.bTurret)
	{
		FArenaASTurretDesc D = AV.Turret;
		D.Team = Team;
		AArenaAS_Turret* T = World->SpawnActorDeferred<AArenaAS_Turret>(AArenaAS_Turret::StaticClass(), AV.Transform, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (T)
		{
			T->InitTurret(D);
			T->FinishSpawning(AV.Transform);
			T->SetLinkedObjective(AV.Objective.Get());
			AV.Pawn = T;
		}
		return;
	}
	AArenaONS_Vehicle* V = World->SpawnActorDeferred<AArenaONS_Vehicle>(AArenaONS_Vehicle::StaticClass(), AV.Transform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
	if (V)
	{
		V->InitVehicle(AV.Type, Team, nullptr);
		V->FinishSpawning(AV.Transform);
		AV.Pawn = V;
		UE_LOG(LogOpenUT2k4, Log, TEXT("AS: %s for team %d at %s (from %s)"), *V->GetVehicleName(), Team, *AV.Transform.GetLocation().ToCompactString(), *AV.Source);
	}
}

void UArenaRules_AS::ResetAssaultVehicles()
{
	for (int32 i = 0; i < AssaultVehicles.Num(); ++i)
	{
		FAssaultVehicle& AV = AssaultVehicles[i];
		if (AArenaAS_Turret* T = Cast<AArenaAS_Turret>(AV.Pawn.Get()))
		{
			T->LeaveTurret(true);
			T->Destroy();
		}
		else if (AArenaONS_Vehicle* V = Cast<AArenaONS_Vehicle>(AV.Pawn.Get()))
		{
			V->EjectAll();
			V->Destroy();
		}
		AV.Pawn.Reset();
		AV.RespawnAt = 0.f;
		SpawnAssaultVehicle(i);
	}
}

void UArenaRules_AS::TickAssaultVehicles(float Now)
{
	const int32 Attack = GetAttackingTeam();
	for (int32 i = 0; i < AssaultVehicles.Num(); ++i)
	{
		FAssaultVehicle& AV = AssaultVehicles[i];
		APawn* P = AV.Pawn.Get();
		if (!AV.bTurret)
		{
			// destroyed vehicles come back after a while (UE2 ASVehicleFactory RespawnTime)
			if (!P)
			{
				if (AV.RespawnAt <= 0.f)
				{
					AV.RespawnAt = Now + 30.f;
				}
				else if (Now >= AV.RespawnAt)
				{
					AV.RespawnAt = 0.f;
					SpawnAssaultVehicle(i);
				}
			}
			continue;
		}
		// bots leave a turret once no current objective of their team is near it
		AArenaAS_Turret* T = Cast<AArenaAS_Turret>(P);
		if (T && T->IsManned() && T->GetController() && !T->GetController()->IsA<APlayerController>())
		{
			bool bNeeded = false;
			for (const AArenaAS_Objective* Obj : Objectives)
			{
				bNeeded |= Obj && Obj->IsActive() && FVector::Dist(Obj->GetActorLocation(), T->GetActorLocation()) < 6000.f;
			}
			if (!bNeeded)
			{
				T->LeaveTurret(false);
			}
		}
		if (T && !T->IsDestroyedTurret())
		{
			const int32 Team = AV.RelTeam == 1 ? 1 - Attack : Attack;
			if (T->GetTeam() != Team && !T->IsManned())
			{
				T->SetTeam(Team);
			}
		}
	}
}

void UArenaRules_AS::GetBotVehicleObjectives(AArenaBotController* Bot, TArray<FArenaBotObjective>& OutGoals)
{
	AArenaCharacter* P = Bot ? Bot->GetArenaPawn() : nullptr;
	const int32 Team = GetTeamOf(Bot);
	if (!P || (Team != 0 && Team != 1) || AssaultVehicles.Num() == 0)
	{
		return;
	}
	const bool bAttacker = Team == GetAttackingTeam();
	const FVector MyLoc = P->GetActorLocation();
	// attackers: the vehicles their current objectives need (UE2 bMustBoardVehicleFirst)
	bool bNeedVehicle = false;
	for (const AArenaAS_Objective* Obj : Objectives)
	{
		bNeedVehicle |= bAttacker && Obj && Obj->IsActive() && VehicleObjectives.Contains(Obj->GetIndex());
	}
	for (int32 i = 0; i < AssaultVehicles.Num(); ++i)
	{
		const FAssaultVehicle& AV = AssaultVehicles[i];
		APawn* Pawn = AV.Pawn.Get();
		if (!Pawn)
		{
			continue;
		}
		const float Dist = float(FVector::Dist(MyLoc, Pawn->GetActorLocation()));
		if (const AArenaAS_Turret* T = Cast<AArenaAS_Turret>(Pawn))
		{
			// defenders man the turrets near their current objectives
			if (bAttacker || !T->GetTurretDesc().bManned || T->IsManned() || T->IsDestroyedTurret() || T->GetTeam() != Team || Dist > 3500.f)
			{
				continue;
			}
			bool bNear = false;
			for (const AArenaAS_Objective* Obj : Objectives)
			{
				bNear |= Obj && Obj->IsActive() && FVector::Dist(Obj->GetActorLocation(), T->GetActorLocation()) < 5000.f;
			}
			if (bNear && Bot->GetRandomStream().FRand() < 0.5f)
			{
				OutGoals.Add(FArenaBotObjective(T->GetActorLocation(), const_cast<AArenaAS_Turret*>(T), 1.3f, EArenaBotAction::Move, Tag_Turret + i));
				OutGoals.Last().bCompleteOnReach = false;
			}
			continue;
		}
		const AArenaONS_Vehicle* V = Cast<AArenaONS_Vehicle>(Pawn);
		if (V && V->CanBeEnteredBy(P) && V->GetTeam() == Team && (bNeedVehicle || (bAttacker && Dist < 2500.f)) && Dist < 12000.f)
		{
			OutGoals.Add(FArenaBotObjective(V->GetActorLocation(), const_cast<AArenaONS_Vehicle*>(V), bNeedVehicle ? 2.2f : 0.8f, EArenaBotAction::Move, Tag_Vehicle + i));
			OutGoals.Last().bCompleteOnReach = false;
		}
	}
}

bool UArenaRules_AS::GetVehicleSiegeTarget(int32 Team, const FVector& Eye, float Range, AActor*& OutTarget, FVector& OutPoint)
{
	UWorld* World = GetWorld();
	// objectives are big, fixed targets: UT2004's vehicle and space fighter weapons engage them from further away than
	// enemies (AS-MotherShip's fighters circle the shield generator about 70 m out)
	float Best = Range * 2.5f;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaASSiege), false);
	auto Consider = [&](AActor* Actor, const FVector& Point)
	{
		const float D = float(FVector::Dist(Eye, Point));
		if (D >= Best || (!Cast<AArenaAS_Objective>(Actor) && D >= Range))
		{
			return;
		}
		FCollisionQueryParams Q2 = Q;
		Q2.AddIgnoredActor(Actor);
		FHitResult Block;
		if (World->LineTraceSingleByChannel(Block, Eye, Point, ECC_Visibility, Q2))
		{
			// a hit on the objective's own mesh, right next to the aim point, still counts
			if (!Cast<AArenaAS_Objective>(Actor) || FVector::Dist(Block.ImpactPoint, Point) > 500.f)
			{
#if !UE_BUILD_SHIPPING
				static double LastSiegeLog = -100.0;
				if (Cast<AArenaAS_Objective>(Actor) && World->GetTimeSeconds() - LastSiegeLog > 10.0 && FParse::Param(FCommandLine::Get(), TEXT("BotDebug")) == false
					&& FCString::Strifind(FCommandLine::Get(), TEXT("-BotDebug=")))
				{
					LastSiegeLog = World->GetTimeSeconds();
					UE_LOG(LogOpenUT2k4, Log, TEXT("AS: vehicle siege on %s from %.0f cm blocked by %s at %s (%.0f cm short)"), *Actor->GetName(), D,
						*GetNameSafe(Block.GetActor()), *Block.ImpactPoint.ToCompactString(), FVector::Dist(Block.ImpactPoint, Point));
				}
#endif
				return;
			}
		}
		Best = D;
		OutTarget = Actor;
		OutPoint = Point;
	};
	if (Team == GetAttackingTeam())
	{
		for (AArenaAS_Objective* Obj : Objectives)
		{
			if (Obj && Obj->IsActive() && Obj->GetType() == EArenaASObjectiveType::Destroy)
			{
				AActor* Vehicle = nullptr;
				for (const FAssaultVehicle& AV : AssaultVehicles)
				{
					Vehicle = AV.Objective.Get() == Obj ? AV.Pawn.Get() : Vehicle;
				}
				Consider(Vehicle ? Vehicle : static_cast<AActor*>(Obj), Vehicle ? Vehicle->GetActorLocation() + FVector(0.f, 0.f, 40.f) : Obj->GetMarkerLocation());
			}
		}
	}
	for (const FAssaultVehicle& AV : AssaultVehicles)
	{
		const AArenaAS_Turret* T = Cast<AArenaAS_Turret>(AV.Pawn.Get());
		if (T && !T->IsDestroyedTurret() && T->GetTeam() != Team)
		{
			Consider(const_cast<AArenaAS_Turret*>(T), T->GetActorLocation() + FVector(0.f, 0.f, 40.f));
		}
	}
	return OutTarget != nullptr;
}

void UArenaRules_AS::OnPawnSpawned(AArenaCharacter* Character)
{
	Super::OnPawnSpawned(Character);
	// UE2 PlayerSpawnManager ForcedPawnClass (AS-MotherShip): the attackers start their space stages in a fighter; ours
	// is our gunship (Kestrel), spawned around them where they appear
	AController* C = Character ? Character->GetController() : nullptr;
	const int32 Team = GetTeamOf(C);
	if (!C || (Team != 0 && Team != 1) || NumStages <= 0)
	{
		return;
	}
	// the group of the start the pawn appeared at (PlayerSpawnManager ForcedPawnClass)
	int32 G = INDEX_NONE;
	float BestD = 300.f;
	for (int32 g = 0; g < SpawnGroups.Num(); ++g)
	{
		for (const TWeakObjectPtr<APlayerStart>& S : SpawnGroups[g].Starts)
		{
			const float D = S.IsValid() ? float(FVector::Dist(S->GetActorLocation(), Character->GetActorLocation())) : BIG_NUMBER;
			if (D < BestD)
			{
				BestD = D;
				G = g;
			}
		}
	}
	if (!SpawnGroups.IsValidIndex(G) || !SpawnGroups[G].bSpawnInFighter)
	{
		return;
	}
	SpawnedFighters.RemoveAll([](const TWeakObjectPtr<AArenaONS_Vehicle>& V) { return !V.IsValid(); });
	for (const TWeakObjectPtr<AArenaONS_Vehicle>& V : SpawnedFighters)
	{
		if (V->IsEmpty() && !V->IsActorBeingDestroyed())
		{
			V->Destroy(); // abandoned fighters go when their pilots respawn (UE2 destroys them as well)
		}
	}
	const FTransform T(FRotator(0.f, Character->GetActorRotation().Yaw, 0.f), Character->GetActorLocation() + FVector(0.f, 0.f, 150.f));
	AArenaONS_Vehicle* V = GetWorld()->SpawnActorDeferred<AArenaONS_Vehicle>(AArenaONS_Vehicle::StaticClass(), T, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
	if (!V)
	{
		return;
	}
	V->InitVehicle(EArenaONS_VehicleType::Kestrel, Team, nullptr);
	V->FinishSpawning(T);
	SpawnedFighters.Add(V);
	V->EnterSeat(Character, 0);
}

bool UArenaRules_AS::GetVehicleDriveDestination(AController* Driver, const FVector& From, FVector& OutDestination)
{
	const int32 Team = ArenaONS::TeamOfController(Driver);
	const bool bAttacker = Team == GetAttackingTeam();
	float Best = TNumericLimits<float>::Max();
	bool bFound = false;
	for (const AArenaAS_Objective* Obj : Objectives)
	{
		if (!Obj || !Obj->IsActive() || (Obj->IsOptional() && !bAttacker))
		{
			continue;
		}
		// attackers go for the objectives that need a vehicle first, then the nearest
		const float D = float(FVector::Dist(From, Obj->GetActorLocation())) - (VehicleObjectives.Contains(Obj->GetIndex()) ? 100000.f : 0.f);
		if (D < Best)
		{
			Best = D;
			OutDestination = Obj->GetMarkerLocation();
			bFound = true;
			// a flyer attacking a destroy objective goes where it can see it (UT2004 space fighters strafe the objective
			// from the open, not from behind the hull)
			const AArenaONS_Vehicle* Flyer = Driver ? Cast<AArenaONS_Vehicle>(Driver->GetPawn()) : nullptr;
			FVector Spot;
			if (bAttacker && Flyer && Flyer->GetSpec().Mode == EArenaONS_DriveMode::Flyer && Obj->GetType() == EArenaASObjectiveType::Destroy
				&& FindSiegeSpot(Obj, From, Flyer, Spot))
			{
				OutDestination = Spot;
			}
		}
	}
	return bFound;
}

bool UArenaRules_AS::FindSiegeSpot(const AArenaAS_Objective* Obj, const FVector& From, const AActor* Flyer, FVector& OutSpot)
{
	if (!Obj)
	{
		return false;
	}
	UWorld* World = GetWorld();
	const FVector Target = Obj->GetMarkerLocation();
	FCollisionQueryParams Q(SCENE_QUERY_STAT(ArenaASSiegeSpot), false);
	Q.AddIgnoredActor(Obj);
	FSiegeGraph* Graph = SiegeSpots.Find(Obj->GetIndex());
	if (!Graph)
	{
		// computed once per objective: spots in the open around it with a clear shot at it, approach points on wider
		// shells, and every point's flight distance to a spot along straight clear lines between points (the objective
		// may sit behind the hull from where the fighters come in: AS-MotherShip's shield generator)
		const double StartTime = FPlatformTime::Seconds();
		Graph = &SiegeSpots.Add(Obj->GetIndex());
		TArray<FVector>& Points = Graph->Points;
		for (const float R : { 2000.f, 3000.f, 4200.f, 6500.f, 10000.f, 15000.f, 20000.f, 35000.f })
		{
			// siege spots within the guns' range and within 30 degrees of level with the target (the fighter's guns fire
			// within 30 degrees of its nose, and it flies level)
			const bool bSiegeShell = R <= 5000.f;
			const FCollisionShape Ball = FCollisionShape::MakeSphere(bSiegeShell ? 400.f : 800.f);
			for (int32 Pitch = -60; Pitch <= 90; Pitch += (bSiegeShell ? 15 : 30))
			{
				if (bSiegeShell && FMath::Abs(Pitch) > 30)
				{
					continue;
				}
				for (int32 Yaw = 0; Yaw < 360; Yaw += (Pitch == 90 ? 360 : 30))
				{
					const FVector P = Target + FRotator(float(Pitch), float(Yaw), 0.f).Vector() * R;
					if (World->OverlapBlockingTestByChannel(P, FQuat::Identity, ECC_Visibility, Ball, Q))
					{
						continue;
					}
					if (bSiegeShell)
					{
						FHitResult Hit;
						if (World->LineTraceSingleByChannel(Hit, P, Target, ECC_Visibility, Q) && FVector::Dist(Hit.ImpactPoint, Target) > 500.f)
						{
							continue;
						}
						++Graph->NumSiege;
					}
					Points.Add(P);
				}
			}
		}
		// Dijkstra from the spots over clear lines (no edge longer than 40000 cm)
		const int32 N = Points.Num();
		Graph->CostToSpot.Init(TNumericLimits<float>::Max(), N);
		TArray<uint8> Done;
		Done.Init(0, N);
		TArray<int8> Clear; // -1 unknown, 0 blocked, 1 clear (traced on demand)
		Clear.Init(-1, N * N);
		for (int32 i = 0; i < Graph->NumSiege; ++i)
		{
			Graph->CostToSpot[i] = 0.f;
		}
		for (;;)
		{
			int32 Cur = INDEX_NONE;
			for (int32 i = 0; i < N; ++i)
			{
				if (!Done[i] && Graph->CostToSpot[i] < TNumericLimits<float>::Max() && (Cur == INDEX_NONE || Graph->CostToSpot[i] < Graph->CostToSpot[Cur]))
				{
					Cur = i;
				}
			}
			if (Cur == INDEX_NONE)
			{
				break;
			}
			Done[Cur] = 1;
			for (int32 j = Graph->NumSiege; j < N; ++j)
			{
				const float D = float(FVector::Dist(Points[Cur], Points[j]));
				if (Done[j] || D > 40000.f || Graph->CostToSpot[Cur] + D >= Graph->CostToSpot[j])
				{
					continue;
				}
				int8& C = Clear[FMath::Min(Cur, j) * N + FMath::Max(Cur, j)];
				if (C < 0)
				{
					C = World->LineTraceTestByChannel(Points[Cur], Points[j], ECC_Visibility, Q) ? 0 : 1;
				}
				if (C)
				{
					Graph->CostToSpot[j] = Graph->CostToSpot[Cur] + D;
				}
			}
		}
		int32 Linked = 0;
		for (int32 i = Graph->NumSiege; i < N; ++i)
		{
			Linked += Graph->CostToSpot[i] < TNumericLimits<float>::Max() ? 1 : 0;
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("AS: %d siege spot(s) with a clear shot at objective %d, %d of %d approach points lead to one (%.0f ms)"),
			Graph->NumSiege, Obj->GetIndex(), Linked, N - Graph->NumSiege, (FPlatformTime::Seconds() - StartTime) * 1000.0);
	}
	// the point in a straight clear line from the flyer with the shortest flight to a spot (a spot itself when one is in
	// sight); with none in sight, the nearest spot (the flyer's own climb-out handles being hung up)
	float Best = TNumericLimits<float>::Max(), BestAny = TNumericLimits<float>::Max();
	FVector Pick = FVector::ZeroVector, Any = FVector::ZeroVector;
	FCollisionQueryParams QF(SCENE_QUERY_STAT(ArenaASSiegeWay), false, Flyer);
	QF.AddIgnoredActor(Obj);
	TArray<int32> Order;
	for (int32 i = 0; i < Graph->Points.Num(); ++i)
	{
		if (Graph->CostToSpot[i] < TNumericLimits<float>::Max())
		{
			Order.Add(i);
		}
	}
	Order.Sort([&](int32 A, int32 B)
	{
		return FVector::Dist(Graph->Points[A], From) + Graph->CostToSpot[A] < FVector::Dist(Graph->Points[B], From) + Graph->CostToSpot[B];
	});
	for (const int32 i : Order)
	{
		const FVector& P = Graph->Points[i];
		const float D = float(FVector::Dist(P, From));
		if (i >= Graph->NumSiege && D < 1500.f)
		{
			continue; // an approach point the flyer has reached: on to the next
		}
		if (i < Graph->NumSiege && D < BestAny)
		{
			BestAny = D;
			Any = P;
		}
		if (D + Graph->CostToSpot[i] < Best && !World->LineTraceTestByChannel(From, P, ECC_Visibility, QF))
		{
			Best = D + Graph->CostToSpot[i];
			Pick = P;
			break; // sorted by total: the first clear one is the best
		}
	}
	OutSpot = !Pick.IsZero() ? Pick : Any;
	return Graph->NumSiege > 0;
}

// =============================================================================================
// Movers in the attackers' way
// =============================================================================================

bool UArenaRules_AS::FireUseTrigger(const FVector& At, AController* By)
{
	for (const FUseTrigger& U : UseTriggers)
	{
		if (FVector::Dist(U.Location, At) < U.Radius)
		{
			if (UArenaEventSubsystem* Events = UArenaEventSubsystem::Get(this))
			{
				Events->TriggerEvent(U.Event, nullptr, By ? By->GetPawn() : nullptr);
				UE_LOG(LogOpenUT2k4, Log, TEXT("AS: UseTrigger at %s used: event %s"), *U.Location.ToCompactString(), *U.Event.ToString());
				return true;
			}
		}
	}
	return false;
}

void UArenaRules_AS::UpdateMoverStall(float Now)
{
	UWorld* World = GetWorld();
	static float NextCheck = 0.f;
	if (Now < NextCheck)
	{
		return;
	}
	NextCheck = Now + 2.f;
	const int32 Attack = GetAttackingTeam();
	// the current required objective nearest to an attacker
	const AArenaAS_Objective* Target = nullptr;
	float Nearest = TNumericLimits<float>::Max();
	FVector NearestLoc = FVector::ZeroVector;
	for (const AArenaAS_Objective* Obj : Objectives)
	{
		if (!Obj || !Obj->IsActive() || Obj->IsOptional())
		{
			continue;
		}
		for (TActorIterator<AArenaCharacter> It(World); It; ++It)
		{
			// bots only: a player standing still is not a stall of the bots
			if (It->IsAlive() && It->GetTeam() == Attack && !It->IsHidden() && Cast<AArenaBotController>(It->GetController()))
			{
				const float D = float(FVector::Dist(It->GetActorLocation(), Obj->GetActorLocation()));
				if (D < Nearest)
				{
					Nearest = D;
					NearestLoc = It->GetActorLocation();
					Target = Obj;
				}
			}
		}
	}
	if (!Target)
	{
		Stall = FMoverStall();
		return;
	}
	if (Stall.ObjectiveIndex != Target->GetIndex())
	{
		Stall = FMoverStall();
		Stall.ObjectiveIndex = Target->GetIndex();
		Stall.LastProgress = Now;
	}
	if (Nearest < Stall.BestDist - 300.f)
	{
		Stall.BestDist = Nearest;
		Stall.LastProgress = Now;
	}
	// solved: the mover opened (or moved), or they got going again
	if (Stall.Kind != 0)
	{
		const AArenaMover* M = Stall.Mover.Get();
		if (!M || !M->IsClosed() || Now - Stall.LastProgress < 1.f || Now - Stall.Since > 60.f)
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("AS: the way to objective %d is %s"), Stall.ObjectiveIndex, M && !M->IsClosed() ? TEXT("open") : TEXT("no longer blocked (or given up)"));
			const int32 Index = Stall.ObjectiveIndex;
			Stall = FMoverStall();
			Stall.ObjectiveIndex = Index;
			Stall.BestDist = Nearest;
			Stall.LastProgress = Now;
		}
		return;
	}
	if (Now - Stall.LastProgress < 20.f || Nearest < 600.f)
	{
		return;
	}
	// the attackers' path: the first closed mover on it
	AArenaNavGraph* Nav = GetNavGraph();
	TArray<FVector> Path;
	TArray<EArenaNavLinkType> Links;
	if (!Nav || !Nav->FindPath(NearestLoc, Target->GetActorLocation() + FVector(0.f, 0.f, 90.f), Path, Links, false, true))
	{
		Path = { Target->GetActorLocation() + FVector(0.f, 0.f, 90.f) };
	}
	Path.Insert(NearestLoc, 0);
	AArenaMover* Blocking = nullptr;
	FVector Before = NearestLoc;
	for (int32 i = 0; i + 1 < Path.Num() && !Blocking; ++i)
	{
		for (TActorIterator<AArenaMover> It(World); It; ++It)
		{
			if (!It->IsClosed() || It->GetUE2Tag() == NAME_None)
			{
				continue;
			}
			const FBox B = It->GetComponentsBoundingBox(true).ExpandBy(60.f);
			if (B.IsValid && FMath::LineBoxIntersection(B, Path[i], Path[i + 1], Path[i + 1] - Path[i]))
			{
				Blocking = *It;
				Before = Path[i];
				break;
			}
		}
	}
	if (!Blocking)
	{
		Stall.LastProgress = Now; // nothing we can do here: look again later
		return;
	}
	// what opens it: actors whose Event is the mover's Tag (one step back through dispatchers / scripted triggers)
	const FName MoverTag = Blocking->GetUE2Tag();
	TArray<FName> Wanted = { MoverTag };
	AActor* Opener = nullptr;
	int32 Kind = 0;
	FVector Goal = Before;
	for (int32 Depth = 0; Depth < 2 && !Opener; ++Depth)
	{
		TArray<FName> Next;
		for (TActorIterator<AActor> It(World); It && !Opener; ++It)
		{
			IArenaUE2Actor* U = Cast<IArenaUE2Actor>(*It);
			if (!U || !Wanted.Contains(U->GetUE2Event()))
			{
				continue;
			}
			if (const AArenaTrigger* T = Cast<AArenaTrigger>(*It))
			{
				Opener = *It;
				Kind = T->TriggerType == 4 ? 3 : 1;
				Goal = It->GetActorLocation();
			}
			else if (U->GetUE2Tag() != NAME_None)
			{
				Next.Add(U->GetUE2Tag());
			}
		}
		for (const FUseTrigger& UT : UseTriggers)
		{
			if (!Opener && Wanted.Contains(UT.Event))
			{
				Kind = 2;
				Goal = UT.Location;
				Opener = Blocking;
			}
		}
		Wanted = Next;
	}
	if (Kind == 0)
	{
		Kind = 4; // opened by the map's scripts (an objective, a sequence): wait there for it
	}
	Stall.Kind = Kind;
	Stall.Mover = Blocking;
	Stall.Opener = Opener;
	Stall.GoalLocation = Goal;
	Stall.Since = Now;
	static const TCHAR* Names[] = { TEXT(""), TEXT("touch its trigger"), TEXT("use its switch"), TEXT("shoot its trigger"), TEXT("wait for it") };
	UE_LOG(LogOpenUT2k4, Log, TEXT("AS: attackers stuck %.0f cm from objective %d behind mover %s (tag %s): %s at %s"), Nearest, Stall.ObjectiveIndex,
		*Blocking->GetName(), *MoverTag.ToString(), Names[Kind], *Goal.ToCompactString());
}
