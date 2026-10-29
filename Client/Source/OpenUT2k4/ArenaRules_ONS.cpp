#include "ArenaRules_ONS.h"
#include "OpenUT2k4.h"
#include "ArenaONS_PowerNode.h"
#include "ArenaONS_Vehicle.h"
#include "ArenaONS_VehicleFactory.h"
#include "ArenaGameMode.h"
#include "ArenaGameState.h"
#include "ArenaPlayerState.h"
#include "ArenaPlayerController.h"
#include "ArenaCharacter.h"
#include "ArenaBotController.h"
#include "ArenaLevelBuilder.h"
#include "ArenaNavGraph.h"
#include "ArenaHUD.h"
#include "ArenaArtHUD.h"
#include "Camera/PlayerCameraManager.h"
#include "ArenaWeapon.h"
#include "ArenaWeapons.h"
#include "ArenaAssets.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/PlayerController.h"
#include "Components/BoxComponent.h"
#include "Engine/Canvas.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

ARENA_REGISTER_RULES(UArenaRules_ONS, "ONS", "Onslaught");

namespace ArenaONSRules
{
	enum EObjectiveTag : int32
	{
		TagAttack = 7101,
		TagBuild = 7102,
		TagDefend = 7103,
		TagVehicle = 7104
	};

	constexpr float VehicleTripDist = 2200.f;
	constexpr float VehicleSearchDist = 2600.f;
	constexpr float UseReach = 260.f;

	bool JsonString(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, FString& Out)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonValue> V = Obj->TryGetField(Key);
		if (!V.IsValid() || V->Type != EJson::String)
		{
			return false;
		}
		Out = V->AsString();
		return !Out.IsEmpty();
	}

	bool JsonNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, double& Out)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonValue> V = Obj->TryGetField(Key);
		if (!V.IsValid())
		{
			return false;
		}
		if (V->Type == EJson::Number)
		{
			Out = V->AsNumber();
			return true;
		}
		if (V->Type == EJson::String)
		{
			const FString S = V->AsString();
			if (S.IsNumeric())
			{
				Out = FCString::Atod(*S);
				return true;
			}
		}
		return false;
	}

	/** Team of an exporter marker: explicit team fields first, then names. -1 = neutral / unknown. */
	int32 MarkerTeam(const FArenaRawMarker& M, bool bCore)
	{
		auto Valid = [](double V) { return V == 0.0 || V == 1.0; };
		double N = -1.0;
		if (JsonNumber(M.Json, TEXT("team"), N) && Valid(N))
		{
			return int32(N);
		}
		if (bCore && JsonNumber(M.Json, TEXT("defender_team"), N) && Valid(N))
		{
			return int32(N);
		}
		const int32 PropTeam = M.GetTeam();
		if (PropTeam == 0 || PropTeam == 1)
		{
			return PropTeam;
		}
		FString Name;
		if (JsonString(M.Json, TEXT("team_name"), Name))
		{
			if (Name.Equals(TEXT("red"), ESearchCase::IgnoreCase)) return 0;
			if (Name.Equals(TEXT("blue"), ESearchCase::IgnoreCase)) return 1;
		}
		if (bCore)
		{
			if (M.Class.Contains(TEXT("Red"))) return 0;
			if (M.Class.Contains(TEXT("Blue"))) return 1;
		}
		return -1;
	}

	FString MarkerName(const FArenaRawMarker& M, int32 Fallback)
	{
		FString Name;
		if (JsonString(M.Json, TEXT("actor"), Name) || JsonString(M.Json, TEXT("name"), Name) || M.GetStringProperty(TEXT("Name"), Name))
		{
			return Name;
		}
		return FString::Printf(TEXT("%s_%d"), *M.Class, Fallback);
	}

	bool ChainContains(const FArenaRawMarker& M, const TCHAR* Part)
	{
		if (M.Class.Contains(Part))
		{
			return true;
		}
		for (const FString& C : M.ClassChain)
		{
			if (C.Contains(Part))
			{
				return true;
			}
		}
		return false;
	}

	/** Map an exporter vehicle class (and its parents) onto our roster by role. */
	EArenaONS_VehicleType MapVehicleClass(const TArray<FString>& Names)
	{
		auto Any = [&Names](const TCHAR* Part)
		{
			for (const FString& N : Names)
			{
				if (N.Contains(Part, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
			return false;
		};
		if (Any(TEXT("AttackCraft")) || Any(TEXT("ChopperCraft")) || Any(TEXT("Chopper")) || Any(TEXT("Flyer")))
		{
			return EArenaONS_VehicleType::Kestrel;
		}
		if (Any(TEXT("HoverBike")) || Any(TEXT("HoverCraft")))
		{
			return EArenaONS_VehicleType::Wisp;
		}
		if (Any(TEXT("HoverTank")) || Any(TEXT("TreadCraft")) || Any(TEXT("ShockTank")) || Any(TEXT("MobileAssaultStation")) || Any(TEXT("Artillery")) || Any(TEXT("Tank")))
		{
			return EArenaONS_VehicleType::Bulwark;
		}
		if (Any(TEXT("PRV")) || Any(TEXT("Transport")))
		{
			return EArenaONS_VehicleType::Caravan;
		}
		return EArenaONS_VehicleType::Dustrunner; // ONSRV, WheeledCraft and anything unknown
	}

	uint32 Hash2(uint32 A, uint32 B)
	{
		uint32 H = A * 0x9E3779B1u ^ (B + 0x7F4A7C15u + (A << 6) + (A >> 2));
		H ^= H >> 15;
		H *= 0x2C1B3C6Du;
		H ^= H >> 12;
		return H;
	}

	float Hash01(uint32 A, uint32 B)
	{
		return float(Hash2(A, B) & 0xFFFF) / 65535.f;
	}

	FString TeamName(int32 Team)
	{
		return Team == 0 ? TEXT("Red") : Team == 1 ? TEXT("Blue") : TEXT("Neutral");
	}

	FString ControllerName(const AController* C)
	{
		const APlayerState* PS = C ? C->PlayerState.Get() : nullptr;
		return PS ? PS->GetPlayerName() : FString(TEXT("nobody"));
	}
}
using namespace ArenaONSRules;

// =============================================================================================
// Identity
// =============================================================================================

bool UArenaRules_ONS::SupportsLayout(const FArenaLayoutInfo& Layout) const
{
	return Layout.GameTypePrefix == TEXT("ONS")
		|| Layout.HasFeature(TEXT("raw:power_cores"))
		|| Layout.HasFeature(TEXT("raw:power_nodes"))
		|| Layout.HasFeature(TEXT("FlagBases"));
}

FString UArenaRules_ONS::GetGoalText(int32 Goal) const
{
	return Goal <= 2 ? FString(TEXT("Destroy the enemy core")) : FString::Printf(TEXT("First to %d points (core kill 2, overtime 1)"), Goal);
}

void UArenaRules_ONS::InitRules(AArenaGameMode* InGameMode, const FString& Options)
{
	Super::InitRules(InGameMode, Options);
}

float UArenaRules_ONS::GetKillZ() const
{
	const AArenaLevelBuilder* B = GetLevelBuilder();
	return B ? B->GetKillZ() : -100000.f;
}

// =============================================================================================
// Level setup
// =============================================================================================

bool UArenaRules_ONS::ParseImported(AArenaLevelBuilder* Builder, TArray<FONSNodeDef>& OutNodes, TArray<TPair<FString, FString>>& OutLinks, TArray<FONSFactoryDef>& OutFactories)
{
	auto FloorOf = [Builder](const FVector& L)
	{
		FVector Floor;
		if (Builder->TraceFloor(L + FVector(0.f, 0.f, 40.f), 2500.f, Floor))
		{
			return Floor;
		}
		return L - FVector(0.f, 0.f, 100.f);
	};
	TSet<FString> Seen;
	auto AddDef = [&](const FArenaRawMarker& M, bool bCore, int32 Index)
	{
		FONSNodeDef D;
		D.Name = MarkerName(M, Index);
		if (Seen.Contains(D.Name))
		{
			return;
		}
		Seen.Add(D.Name);
		D.bCore = bCore;
		D.Team = bCore ? MarkerTeam(M, true) : -1;
		D.Location = FloorOf(M.Location);
		D.Yaw = M.Rotation.Yaw;
		OutNodes.Add(D);
	};

	// 1) Dedicated lists (current importer)
	int32 Index = 0;
	for (const FArenaRawMarker& M : Builder->GetRawMarkers(TEXT("power_cores")))
	{
		AddDef(M, true, Index++);
	}
	for (const FArenaRawMarker& M : Builder->GetRawMarkers(TEXT("power_nodes")))
	{
		bool bIsNode = true;
		if (M.Json.IsValid())
		{
			M.Json->TryGetBoolField(TEXT("is_node"), bIsNode);
		}
		const bool bCore = !bIsNode || (ChainContains(M, TEXT("PowerCore")) && !ChainContains(M, TEXT("PowerNode")));
		AddDef(M, bCore, Index++);
	}
	// 2) Generic objective list (older importer)
	if (OutNodes.Num() == 0)
	{
		for (const FArenaRawMarker& M : Builder->GetRawMarkers(TEXT("game_objectives")))
		{
			FString Type;
			JsonString(M.Json, TEXT("objective_type"), Type);
			const bool bCore = Type == TEXT("power_core") || ChainContains(M, TEXT("PowerCore"));
			const bool bNode = Type == TEXT("power_node") || ChainContains(M, TEXT("PowerNode"));
			if (bCore || bNode)
			{
				AddDef(M, bCore && !bNode, Index++);
			}
		}
	}
	// 3) Anchors made by the level builder
	if (OutNodes.Num() == 0)
	{
		for (int32 Team = 0; Team < 2; ++Team)
		{
			FTransform T;
			if (Builder->GetAnchorPoint(FName(*FString::Printf(TEXT("Core%d"), Team)), T) || Builder->GetAnchorPoint(FName(*FString::Printf(TEXT("TeamBase%d"), Team)), T))
			{
				FONSNodeDef D;
				D.Name = FString::Printf(TEXT("Core%d"), Team);
				D.bCore = true;
				D.Team = Team;
				D.Location = T.GetLocation();
				D.Yaw = T.Rotator().Yaw;
				OutNodes.Add(D);
			}
		}
		int32 N = 0;
		for (const FTransform& T : Builder->GetAnchorSeries(TEXT("Node")))
		{
			FONSNodeDef D;
			D.Name = FString::Printf(TEXT("Node%d"), N++);
			D.Location = T.GetLocation();
			D.Yaw = T.Rotator().Yaw;
			OutNodes.Add(D);
		}
		// 4) CTF maps: three generated nodes between the flag bases, snapped to distinct player starts.
		if (N == 0 && OutNodes.Num() == 2)
		{
			const FVector A = OutNodes[0].Location;
			const FVector B = OutNodes[1].Location;
			const float Spacing = FMath::Max(1500.f, FVector::Dist(A, B) * 0.12f);
			TArray<FVector> Used = { A, B };
			FString Prev = OutNodes[0].Name;
			for (const float Alpha : { 0.3f, 0.5f, 0.7f })
			{
				const FVector Want = FMath::Lerp(A, B, Alpha);
				const APlayerStart* Best = nullptr;
				float BestScore = TNumericLimits<float>::Max();
				for (const TObjectPtr<APlayerStart>& S : Builder->GetPlayerStarts())
				{
					if (!S)
					{
						continue;
					}
					const FVector L = S->GetActorLocation();
					if (Used.ContainsByPredicate([&](const FVector& U) { return FVector::Dist(U, L) < Spacing; }))
					{
						continue;
					}
					const float Score = FVector::Dist2D(L, Want) + FMath::Abs(L.Z - Want.Z) * 3.f;
					if (Score < BestScore)
					{
						BestScore = Score;
						Best = S;
					}
				}
				if (Best)
				{
					FONSNodeDef D;
					D.Name = FString::Printf(TEXT("Node%d"), N++);
					D.Location = FloorOf(Best->GetActorLocation());
					Used.Add(D.Location);
					OutNodes.Add(D);
					OutLinks.Add(TPair<FString, FString>(Prev, D.Name));
					Prev = D.Name;
				}
			}
			OutLinks.Add(TPair<FString, FString>(Prev, OutNodes[1].Name));
			LinkSource = TEXT("generated chain");
		}
	}

	// Links: explicit pairs, then a link setup ("Default" preferred), then per-node lists.
	const int32 GeneratedLinks = OutLinks.Num();
	for (const FArenaRawMarker& M : Builder->GetRawMarkers(TEXT("power_links")))
	{
		FString A, B;
		bool bValid = true;
		if (M.Json.IsValid())
		{
			M.Json->TryGetBoolField(TEXT("valid"), bValid);
		}
		if (bValid && (JsonString(M.Json, TEXT("a"), A) || JsonString(M.Json, TEXT("from"), A)) && (JsonString(M.Json, TEXT("b"), B) || JsonString(M.Json, TEXT("to"), B)))
		{
			OutLinks.Add(TPair<FString, FString>(A, B));
		}
	}
	if (OutLinks.Num() > GeneratedLinks)
	{
		LinkSource = TEXT("power_links");
	}
	else
	{
		const TArray<FArenaRawMarker> Setups = Builder->GetRawMarkers(TEXT("power_link_setups"));
		const FArenaRawMarker* Chosen = nullptr;
		for (const FArenaRawMarker& S : Setups)
		{
			FString Name;
			if (!Chosen || (JsonString(S.Json, TEXT("setup_name"), Name) && Name.Equals(TEXT("Default"), ESearchCase::IgnoreCase)))
			{
				Chosen = &S;
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (Chosen && Chosen->Json.IsValid() && Chosen->Json->TryGetArrayField(TEXT("links"), Entries))
		{
			for (const TSharedPtr<FJsonValue>& V : *Entries)
			{
				const TSharedPtr<FJsonObject> O = V.IsValid() ? V->AsObject() : nullptr;
				FString Base;
				const TArray<TSharedPtr<FJsonValue>>* Linked = nullptr;
				if (O.IsValid() && JsonString(O, TEXT("base_node"), Base) && O->TryGetArrayField(TEXT("linked_nodes"), Linked))
				{
					for (const TSharedPtr<FJsonValue>& L : *Linked)
					{
						if (L.IsValid() && L->Type == EJson::String)
						{
							OutLinks.Add(TPair<FString, FString>(Base, L->AsString()));
						}
					}
				}
			}
			if (OutLinks.Num() > 0)
			{
				LinkSource = TEXT("power_link_setups");
			}
		}
	}
	if (OutLinks.Num() == 0)
	{
		for (const TCHAR* List : { TEXT("power_cores"), TEXT("power_nodes") })
		{
			for (const FArenaRawMarker& M : Builder->GetRawMarkers(List))
			{
				const TArray<TSharedPtr<FJsonValue>>* Linked = nullptr;
				if (M.Json.IsValid() && M.Json->TryGetArrayField(TEXT("linked_nodes"), Linked))
				{
					for (const TSharedPtr<FJsonValue>& L : *Linked)
					{
						if (L.IsValid() && L->Type == EJson::String)
						{
							OutLinks.Add(TPair<FString, FString>(MarkerName(M, 0), L->AsString()));
						}
					}
				}
			}
		}
		if (OutLinks.Num() > 0)
		{
			LinkSource = TEXT("linked_nodes");
		}
	}

	// Vehicle factories
	for (const FArenaRawMarker& M : Builder->GetRawMarkers(TEXT("vehicle_factories")))
	{
		TArray<FString> Names;
		FString S;
		if (M.Json.IsValid())
		{
			const TSharedPtr<FJsonObject>* VC = nullptr;
			if (M.Json->TryGetObjectField(TEXT("vehicle_class"), VC) && VC && VC->IsValid())
			{
				if (JsonString(*VC, TEXT("class"), S))
				{
					Names.Add(S);
				}
				const TArray<TSharedPtr<FJsonValue>>* Chain = nullptr;
				if ((*VC)->TryGetArrayField(TEXT("class_chain"), Chain))
				{
					for (const TSharedPtr<FJsonValue>& C : *Chain)
					{
						if (C.IsValid() && C->Type == EJson::String)
						{
							Names.Add(C->AsString());
						}
					}
				}
			}
			else if (JsonString(M.Json, TEXT("vehicle_class"), S))
			{
				Names.Add(S);
			}
			for (const TCHAR* Key : { TEXT("vehicle_class_name"), TEXT("vehicle_class_str") })
			{
				if (JsonString(M.Json, Key, S))
				{
					Names.Add(S);
				}
			}
		}
		if (Names.Num() == 0)
		{
			Names.Add(M.Class); // factory class name (e.g. "ONSTankFactory")
		}
		FONSFactoryDef F;
		F.Type = MapVehicleClass(Names);
		F.SourceClass = Names[0];
		F.Location = M.Location;
		F.Yaw = M.Rotation.Yaw;
		double Respawn = 0.0;
		if (JsonNumber(M.Json, TEXT("respawn_time"), Respawn) && Respawn > 1.0)
		{
			F.RespawnTime = float(Respawn);
		}
		else
		{
			F.RespawnTime = AArenaONS_Vehicle::GetSpec(F.Type).RespawnTime;
		}
		OutFactories.Add(F);
	}
	return true;
}

void UArenaRules_ONS::BuildLinksByProximity(TArray<TPair<int32, int32>>& OutLinks) const
{
	// Minimum spanning tree over all objectives (2D distance) so the network is always connected.
	const int32 N = Nodes.Num();
	if (N < 2)
	{
		return;
	}
	TArray<bool> InTree;
	InTree.Init(false, N);
	InTree[0] = true;
	for (int32 Added = 1; Added < N; ++Added)
	{
		float BestD = TNumericLimits<float>::Max();
		int32 BestA = -1, BestB = -1;
		for (int32 A = 0; A < N; ++A)
		{
			if (!InTree[A])
			{
				continue;
			}
			for (int32 B = 0; B < N; ++B)
			{
				if (InTree[B])
				{
					continue;
				}
				// Avoid core-core links
				if (Nodes[A]->IsCore() && Nodes[B]->IsCore())
				{
					continue;
				}
				const float D = FVector::Dist2D(Nodes[A]->GetActorLocation(), Nodes[B]->GetActorLocation());
				if (D < BestD)
				{
					BestD = D;
					BestA = A;
					BestB = B;
				}
			}
		}
		if (BestB < 0)
		{
			break;
		}
		InTree[BestB] = true;
		OutLinks.Add(TPair<int32, int32>(BestA, BestB));
	}
}

bool UArenaRules_ONS::OnLevelBuilt(AArenaLevelBuilder* Builder)
{
	UWorld* World = GetWorld();
	if (!Builder || !World)
	{
		return false;
	}
	TArray<FONSNodeDef> Defs;
	TArray<TPair<FString, FString>> LinkNames;
	FactoryDefs.Reset();
	const bool bOk = ParseImported(Builder, Defs, LinkNames, FactoryDefs);

	// Exactly one core per team; otherwise the map can't host the mode.
	int32 CoreIdx[2] = { -1, -1 };
	for (int32 i = 0; i < Defs.Num(); ++i)
	{
		if (Defs[i].bCore && (Defs[i].Team == 0 || Defs[i].Team == 1) && CoreIdx[Defs[i].Team] < 0)
		{
			CoreIdx[Defs[i].Team] = i;
		}
	}
	if (!bOk || CoreIdx[0] < 0 || CoreIdx[1] < 0)
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("ONS: map %s has no usable power cores (%d objectives parsed); falling back"), *Builder->GetBuiltLayoutId(), Defs.Num());
		return false;
	}

	// Spawn the objectives: cores first (index 0 / 1), then nodes.
	TArray<int32> Order = { CoreIdx[0], CoreIdx[1] };
	for (int32 i = 0; i < Defs.Num(); ++i)
	{
		if (!Defs[i].bCore)
		{
			Order.Add(i);
		}
	}
	TMap<FString, AArenaONS_PowerNode*> ByName;
	int32 NodeNumber = 1;
	for (int32 OrderIdx = 0; OrderIdx < Order.Num(); ++OrderIdx)
	{
		const FONSNodeDef& D = Defs[Order[OrderIdx]];
		const FTransform T(FRotator(0.f, D.Yaw, 0.f), D.Location);
		AArenaONS_PowerNode* Node = World->SpawnActorDeferred<AArenaONS_PowerNode>(AArenaONS_PowerNode::StaticClass(), T, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Node)
		{
			continue;
		}
		const FString Display = D.bCore ? FString::Printf(TEXT("%s Core"), *TeamName(D.Team)) : FString::Printf(TEXT("Node %d"), NodeNumber++);
		Node->InitNode(this, Nodes.Num(), D.bCore, D.Team, Display);
		Node->UE2SourceName = D.Name; // a UT2004 server's state finds its node by this (Net/UE2NetWorld.h)
		Node->FinishSpawning(T);
		if (D.bCore)
		{
			CoreSide.Add(Nodes.Num(), D.Team);
		}
		Nodes.Add(Node);
		ByName.Add(D.Name, Node);
	}

	// Links
	int32 NumLinks = 0;
	auto Link = [&](AArenaONS_PowerNode* A, AArenaONS_PowerNode* B)
	{
		if (A && B && A != B && !A->IsLinkedTo(B))
		{
			A->AddLink(B);
			B->AddLink(A);
			++NumLinks;
		}
	};
	for (const TPair<FString, FString>& L : LinkNames)
	{
		AArenaONS_PowerNode** A = ByName.Find(L.Key);
		AArenaONS_PowerNode** B = ByName.Find(L.Value);
		if (A && B)
		{
			Link(*A, *B);
		}
	}
	// Connectivity check: every objective must be reachable from core 0.
	auto Connected = [this]()
	{
		TSet<AArenaONS_PowerNode*> Seen;
		TArray<AArenaONS_PowerNode*> Queue = { Nodes[0] };
		Seen.Add(Nodes[0]);
		while (Queue.Num() > 0)
		{
			AArenaONS_PowerNode* N = Queue.Pop();
			for (AArenaONS_PowerNode* L : N->GetLinks())
			{
				if (L && !Seen.Contains(L))
				{
					Seen.Add(L);
					Queue.Add(L);
				}
			}
		}
		return Seen.Num() == Nodes.Num();
	};
	if (NumLinks == 0 || !Connected())
	{
		if (NumLinks > 0)
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("ONS: link setup (%s) leaves the network split; adding proximity links"), *LinkSource);
		}
		TArray<TPair<int32, int32>> Prox;
		BuildLinksByProximity(Prox);
		for (const TPair<int32, int32>& P : Prox)
		{
			Link(Nodes[P.Key], Nodes[P.Value]);
		}
		LinkSource = NumLinks > 0 ? LinkSource + TEXT(" + proximity") : FString(TEXT("proximity"));
	}
	for (AArenaONS_PowerNode* Node : Nodes)
	{
		for (AArenaONS_PowerNode* Other : Node->GetLinks())
		{
			if (Other && Node->GetNodeIndex() < Other->GetNodeIndex())
			{
				FActorSpawnParameters Params;
				Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				if (AArenaONS_PowerLink* LinkActor = World->SpawnActor<AArenaONS_PowerLink>(AArenaONS_PowerLink::StaticClass(), Node->GetActorTransform(), Params))
				{
					LinkActor->InitLink(Node, Other);
					LinkActors.Add(LinkActor);
				}
			}
		}
	}

	// Imported factories: owner = nearest objective.
	for (FONSFactoryDef& F : FactoryDefs)
	{
		if (!F.OwnerName.IsEmpty())
		{
			continue;
		}
		float BestD = TNumericLimits<float>::Max();
		for (const TPair<FString, AArenaONS_PowerNode*>& P : ByName)
		{
			const float D = FVector::Dist(P.Value->GetActorLocation(), F.Location);
			if (D < BestD)
			{
				BestD = D;
				F.OwnerName = P.Key;
			}
		}
	}
	// Keep the owner as a node index in the name for SpawnFactories.
	for (FONSFactoryDef& F : FactoryDefs)
	{
		AArenaONS_PowerNode** Owner = ByName.Find(F.OwnerName);
		F.OwnerName = Owner ? FString::FromInt((*Owner)->GetNodeIndex()) : FString();
	}

	RefreshNetwork();
	UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: network on %s: %d objectives (%d nodes), %d links (%s), %d vehicle bays"),
		*Builder->GetBuiltLayoutId(), Nodes.Num(), Nodes.Num() - 2, NumLinks, *LinkSource, FactoryDefs.Num());
	for (const FONSFactoryDef& F : FactoryDefs)
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("ONS:   bay %s -> %s (owner #%s)"), *F.SourceClass, *AArenaONS_Vehicle::GetTypeName(F.Type), *F.OwnerName);
	}
	LogNetwork();
	return Nodes.Num() >= 2;
}

void UArenaRules_ONS::ComputeNodePoints()
{
	if (bPointsComputed)
	{
		return;
	}
	bPointsComputed = true;
	UWorld* World = GetWorld();
	AArenaLevelBuilder* Builder = GetLevelBuilder();
	if (!World)
	{
		return;
	}
	const FCollisionShape Capsule = FCollisionShape::MakeCapsule(ArenaTuning::CapsuleRadius, ArenaTuning::CapsuleHalfHeight);
	for (AArenaONS_PowerNode* Node : Nodes)
	{
		if (!Node)
		{
			continue;
		}
		FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaONSPoints), false);
		for (AArenaONS_PowerNode* Other : Nodes)
		{
			QP.AddIgnoredActor(Other);
		}
		const FVector Center = Node->GetActorLocation();
		const FVector Eye = Center + FVector(0.f, 0.f, 150.f);
		// Validates a ring point: floor nearby, standing room, line of sight to the node.
		auto Validate = [&](const FVector& Want, float MaxZDiff, bool bNeedLOS, FVector& OutFloor) -> bool
		{
			FHitResult Floor;
			if (!World->LineTraceSingleByChannel(Floor, Want + FVector(0.f, 0.f, 250.f), Want - FVector(0.f, 0.f, 600.f), ECC_Visibility, QP) || Floor.ImpactNormal.Z < 0.7f)
			{
				return false;
			}
			if (FMath::Abs(Floor.ImpactPoint.Z - Center.Z) > MaxZDiff)
			{
				return false;
			}
			const FVector Stand = Floor.ImpactPoint + FVector(0.f, 0.f, ArenaTuning::CapsuleHalfHeight + 6.f);
			if (World->OverlapBlockingTestByChannel(Stand, FQuat::Identity, ECC_Pawn, Capsule, QP))
			{
				return false;
			}
			// Solid footing all around (no spots on the lip of a ledge or bridge).
			for (int32 k = 0; k < 4; ++k)
			{
				const FVector Off = FRotator(0.f, 90.f * k + 45.f, 0.f).Vector() * 170.f;
				FHitResult Around;
				const FVector P = Floor.ImpactPoint + Off;
				if (!World->LineTraceSingleByChannel(Around, P + FVector(0.f, 0.f, 120.f), P - FVector(0.f, 0.f, 200.f), ECC_Visibility, QP))
				{
					return false;
				}
			}
			if (bNeedLOS)
			{
				FHitResult Block;
				if (World->LineTraceSingleByChannel(Block, Eye, Stand, ECC_Visibility, QP))
				{
					return false;
				}
			}
			OutFloor = Floor.ImpactPoint;
			return true;
		};

		const bool bCore = Node->IsCore();
		Node->InnerPoints.Reset();
		Node->OuterPoints.Reset();
		Node->SpawnSpots.Reset();
		if (!bCore)
		{
			for (int32 i = 0; i < 6; ++i)
			{
				const float A = FMath::DegreesToRadians(60.f * i + 15.f);
				FVector F;
				if (Validate(Center + FVector(FMath::Cos(A), FMath::Sin(A), 0.f) * 160.f, 80.f, false, F))
				{
					Node->InnerPoints.Add(F + FVector(0.f, 0.f, 10.f));
				}
			}
		}
		for (const float R : bCore ? TArray<float>{ 700.f, 1000.f } : TArray<float>{ 650.f, 900.f })
		{
			for (int32 i = 0; i < 8; ++i)
			{
				const float A = FMath::DegreesToRadians(45.f * i + (R > 800.f ? 22.5f : 0.f));
				FVector F;
				if (Validate(Center + FVector(FMath::Cos(A), FMath::Sin(A), 0.f) * R, 450.f, true, F))
				{
					Node->OuterPoints.Add(F + FVector(0.f, 0.f, 10.f));
				}
			}
		}
		for (const float R : bCore ? TArray<float>{ 330.f, 440.f, 650.f, 850.f } : TArray<float>{ 330.f, 480.f, 640.f })
		{
			for (int32 i = 0; i < 8 && Node->SpawnSpots.Num() < 12; ++i)
			{
				const float A = FMath::DegreesToRadians(45.f * i + R * 0.1f);
				const FVector Dir(FMath::Cos(A), FMath::Sin(A), 0.f);
				FVector F;
				if (Validate(Center + Dir * R, 450.f, true, F))
				{
					Node->SpawnSpots.Add(FTransform(Dir.Rotation(), F));
				}
			}
		}
		// Cores: the side's level starts nearby are known-good spawns too.
		if (bCore && Builder)
		{
			const FName Tag = Node->GetTeam() == 0 ? FName(TEXT("Red")) : FName(TEXT("Blue"));
			for (const TObjectPtr<APlayerStart>& S : Builder->GetPlayerStarts())
			{
				if (S && S->PlayerStartTag == Tag && FVector::Dist(S->GetActorLocation(), Center) < 3000.f)
				{
					Node->SpawnSpots.Add(FTransform(S->GetActorRotation(), S->GetActorLocation() - FVector(0.f, 0.f, ArenaTuning::CapsuleHalfHeight + 4.f)));
				}
			}
		}

		// Player starts for the spawn spots
		for (const FTransform& T : Node->SpawnSpots)
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			const FVector Loc = T.GetLocation() + FVector(0.f, 0.f, ArenaTuning::CapsuleHalfHeight + 8.f);
			if (APlayerStart* Start = World->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), Loc, FRotator(0.f, T.Rotator().Yaw, 0.f), Params))
			{
				Start->PlayerStartTag = FName(TEXT("ONS"));
				SpawnStarts.Add(Start);
				StartNode.Add(Start, Node->GetNodeIndex());
			}
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: %s at %s: %d build points, %d standoff points, %d spawn spots"), *Node->GetNodeName(),
			*Center.ToCompactString(), Node->InnerPoints.Num(), Node->OuterPoints.Num(), Node->SpawnSpots.Num());
	}
}

void UArenaRules_ONS::GetNavHints(TArray<FVector>& OutPoints) const
{
	const_cast<UArenaRules_ONS*>(this)->ComputeNodePoints();
	for (const AArenaONS_PowerNode* Node : Nodes)
	{
		if (!Node)
		{
			continue;
		}
		for (const FVector& P : Node->InnerPoints)
		{
			OutPoints.Add(P);
		}
		for (const FVector& P : Node->OuterPoints)
		{
			OutPoints.Add(P);
		}
	}
}

void UArenaRules_ONS::SpawnFactories()
{
	UWorld* World = GetWorld();
	if (!World || Factories.Num() > 0)
	{
		return;
	}
	// watching / playing on a UT2004 server: its vehicles are its own (Net/UE2NetWorld.h shows them). Our bays would
	// park vehicles of ours that the server does not have: they block our pawn where the server has nothing (position
	// corrections) and look like vehicles to get in, while the server's Use finds none
	if (World->URL.HasOption(TEXT("UE2Net")))
	{
		return;
	}
	// Imported maps without vehicle factories (e.g. CTF maps): open-sky nav points near each core get bays.
	AArenaNavGraph* Nav = GetNavGraph();
	if (FactoryDefs.Num() == 0 && Nav && Nodes.Num() >= 2)
	{
		const FCollisionShape Box = FCollisionShape::MakeBox(AArenaONS_Vehicle::GetSpec(EArenaONS_VehicleType::Bulwark).HalfExtent + FVector(60.f, 60.f, 0.f));
		const EArenaONS_VehicleType Types[4] = { EArenaONS_VehicleType::Dustrunner, EArenaONS_VehicleType::Wisp, EArenaONS_VehicleType::Bulwark, EArenaONS_VehicleType::Kestrel };
		const float CoreDist = FVector::Dist(Nodes[0]->GetActorLocation(), Nodes[1]->GetActorLocation());
		const float BayRadius = FMath::Max(6000.f, CoreDist * 0.3f);
		for (int32 Team = 0; Team < 2; ++Team)
		{
			const AArenaONS_PowerNode* Core = Nodes[Team];
			const FVector CoreLoc = Core->GetActorLocation();
			// Nav points sorted by distance to the core
			TArray<FVector> Cand;
			for (const FArenaNavNode& N : Nav->GetNodes())
			{
				if (FVector::Dist(N.Location, CoreLoc) < BayRadius)
				{
					Cand.Add(N.Location);
				}
			}
			Cand.Sort([&CoreLoc](const FVector& A, const FVector& B) { return FVector::DistSquared(A, CoreLoc) < FVector::DistSquared(B, CoreLoc); });
			TArray<FVector> Picked;
			for (const FVector& L : Cand)
			{
				if (Picked.Num() >= 4)
				{
					break;
				}
				if (FVector::Dist(L, CoreLoc) < 900.f || Picked.ContainsByPredicate([&L](const FVector& P) { return FVector::Dist(P, L) < 1100.f; }))
				{
					continue;
				}
				FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaONSBaySky), false);
				FHitResult Hit;
				if (World->LineTraceSingleByChannel(Hit, L, L + FVector(0.f, 0.f, 5000.f), ECC_Visibility, QP))
				{
					continue; // indoors
				}
				FHitResult Floor;
				if (!World->LineTraceSingleByChannel(Floor, L, L - FVector(0.f, 0.f, 300.f), ECC_Visibility, QP) || Floor.ImpactNormal.Z < 0.85f)
				{
					continue;
				}
				if (World->OverlapBlockingTestByChannel(Floor.ImpactPoint + FVector(0.f, 0.f, Box.GetExtent().Z + 30.f), FQuat::Identity, ECC_Pawn, Box, QP))
				{
					continue; // cramped
				}
				FONSFactoryDef F;
				F.Type = Types[Picked.Num() % 4];
				F.Location = Floor.ImpactPoint;
				F.Yaw = (Nodes[1 - Team]->GetActorLocation() - L).Rotation().Yaw;
				F.RespawnTime = AArenaONS_Vehicle::GetSpec(F.Type).RespawnTime;
				F.SourceClass = TEXT("generated");
				F.OwnerName = FString::FromInt(Team);
				FactoryDefs.Add(F);
				Picked.Add(L);
			}
			UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: generated %d vehicle bays for the %s core"), Picked.Num(), *TeamName(Team));
		}
	}
	for (const FONSFactoryDef& F : FactoryDefs)
	{
		const int32 OwnerIdx = F.OwnerName.IsEmpty() ? -1 : FCString::Atoi(*F.OwnerName);
		AArenaONS_PowerNode* Owner = Nodes.IsValidIndex(OwnerIdx) ? Nodes[OwnerIdx].Get() : nullptr;
		if (!Owner)
		{
			continue;
		}
		FVector Floor = F.Location;
		FHitResult Hit;
		FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaONSBay), false);
		if (World->LineTraceSingleByChannel(Hit, F.Location + FVector(0.f, 0.f, 150.f), F.Location - FVector(0.f, 0.f, 1500.f), ECC_Visibility, QP))
		{
			Floor = Hit.ImpactPoint;
		}
		const FTransform T(FRotator(0.f, F.Yaw, 0.f), Floor);
		AArenaONS_VehicleFactory* Factory = World->SpawnActorDeferred<AArenaONS_VehicleFactory>(AArenaONS_VehicleFactory::StaticClass(), T, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Factory)
		{
			Factory->InitFactory(F.Type, Owner, F.RespawnTime);
			Factory->FinishSpawning(T);
			Factories.Add(Factory);
		}
	}
}

void UArenaRules_ONS::OnStartPlay()
{
	ComputeNodePoints();
	SpawnFactories();
	RefreshNetwork();
	for (AArenaONS_VehicleFactory* F : Factories)
	{
		if (F)
		{
			F->SpawnVehicle();
		}
	}
}

void UArenaRules_ONS::OnMatchStart()
{
	UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: match started (%d objectives, %d vehicle bays, %d vehicles)"), Nodes.Num(), Factories.Num(), Vehicles.Num());
	// Test harness: -ONSTestCore gives red every node so the blue core is under attack from the start.
	if (ArenaONS::HasDebugFlag(TEXT("ONSTestCore")))
	{
		for (AArenaONS_PowerNode* Node : Nodes)
		{
			if (Node && !Node->IsCore())
			{
				Node->ForceOwner(0);
			}
		}
		RefreshNetwork();
		UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: test harness: all nodes given to Red"));
		LogNetwork();
	}
	if (GameMode)
	{
		GameMode->BroadcastCenterMessage(TEXT("Power up the nodes - bring down the enemy core!"), FLinearColor(1.f, 0.85f, 0.3f), 3.f);
	}
}

void UArenaRules_ONS::OnRoundEnd(int32 Round, int32 WinningTeam)
{
	for (const TWeakObjectPtr<AArenaONS_Vehicle>& V : Vehicles)
	{
		if (V.IsValid())
		{
			V->EjectAll();
		}
	}
	SiegeBots.Reset();
}

void UArenaRules_ONS::OnRoundReset(int32 Round)
{
	ResetNetwork();
}

void UArenaRules_ONS::ResetNetwork()
{
	bCoreDestroyedThisRound = false;
	for (AArenaONS_PowerNode* Node : Nodes)
	{
		if (!Node)
		{
			continue;
		}
		if (Node->IsCore())
		{
			const int32* Side = CoreSide.Find(Node->GetNodeIndex());
			Node->ResetNode(GetCoreTeamForSide(Side ? *Side : 0));
		}
		else
		{
			Node->ResetNode(ArenaTeam::None);
		}
	}
	for (const TWeakObjectPtr<AArenaONS_Vehicle>& V : Vehicles)
	{
		if (V.IsValid())
		{
			V->EjectAll();
			V->Destroy();
		}
	}
	Vehicles.Reset();
	for (AArenaONS_VehicleFactory* F : Factories)
	{
		if (F)
		{
			F->ResetFactory();
		}
	}
	Reservations.Reset();
	PendingEntries.Reset();
	RefreshNetwork();
}

int32 UArenaRules_ONS::GetCoreTeamForSide(int32 Side) const
{
	const AArenaGameState* GS = GetGameState();
	const bool bSwapped = GS && GS->bSidesSwapped;
	return bSwapped ? 1 - Side : Side;
}

void UArenaRules_ONS::OnMatchEnd(const FString& WinnerText, int32 WinningTeam, APlayerState* WinningPlayer)
{
	UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: match over: %s (nodes built %d, destroyed %d, vehicle entries %d)"), *WinnerText, NumNodesBuilt, NumNodesDestroyed, NumVehicleEntries);
	SiegeBots.Reset();
}

void UArenaRules_ONS::OnPlayerLeft(AController* Controller)
{
	if (AArenaONS_Vehicle* V = ArenaONS::GetControlledVehicle(Controller))
	{
		const int32 Seat = ArenaONS::GetControlledSeat(Controller);
		if (Seat >= 0)
		{
			if (AArenaCharacter* C = V->LeaveSeat(Seat, true))
			{
				C->Destroy();
			}
		}
	}
}

// =============================================================================================
// Network
// =============================================================================================

AArenaONS_PowerNode* UArenaRules_ONS::GetCore(int32 Team) const
{
	for (AArenaONS_PowerNode* Node : Nodes)
	{
		if (Node && Node->IsCore() && Node->GetTeam() == Team)
		{
			return Node;
		}
	}
	return nullptr;
}

bool UArenaRules_ONS::IsNodeAttackableBy(const AArenaONS_PowerNode* Node, int32 Team) const
{
	if (!Node || (Team != 0 && Team != 1) || Node->IsNeutral() || Node->GetTeam() == Team)
	{
		return false;
	}
	for (const AArenaONS_PowerNode* L : Node->GetLinks())
	{
		if (L && L->IsOwnedBy(Team))
		{
			return true;
		}
	}
	return false;
}

bool UArenaRules_ONS::IsNodeBuildableBy(const AArenaONS_PowerNode* Node, int32 Team) const
{
	if (!Node || Node->IsCore() || !Node->IsNeutral() || (Team != 0 && Team != 1))
	{
		return false;
	}
	for (const AArenaONS_PowerNode* L : Node->GetLinks())
	{
		if (L && L->IsOwnedBy(Team))
		{
			return true;
		}
	}
	return false;
}

int32 UArenaRules_ONS::GetFrontDistance(const AArenaONS_PowerNode* Node, int32 Team) const
{
	if (!Node || !Node->IsOwnedBy(Team))
	{
		return 0;
	}
	TMap<const AArenaONS_PowerNode*, int32> Dist;
	TArray<const AArenaONS_PowerNode*> Queue = { Node };
	Dist.Add(Node, 0);
	for (int32 Head = 0; Head < Queue.Num(); ++Head)
	{
		const AArenaONS_PowerNode* N = Queue[Head];
		const int32 D = Dist[N];
		for (const AArenaONS_PowerNode* L : N->GetLinks())
		{
			if (!L || Dist.Contains(L))
			{
				continue;
			}
			if (!L->IsOwnedBy(Team))
			{
				return D + 1;
			}
			Dist.Add(L, D + 1);
			Queue.Add(L);
		}
	}
	return 99;
}

void UArenaRules_ONS::RefreshNetwork()
{
	for (AArenaONS_PowerNode* Node : Nodes)
	{
		if (Node)
		{
			Node->SetNetworkFlags(IsNodeAttackableBy(Node, 0), IsNodeAttackableBy(Node, 1), IsNodeBuildableBy(Node, 0), IsNodeBuildableBy(Node, 1));
		}
	}
}

void UArenaRules_ONS::NetClearLinks()
{
	for (AArenaONS_PowerNode* Node : Nodes)
	{
		if (Node)
		{
			Node->ClearLinks();
		}
	}
	for (AArenaONS_PowerLink* L : LinkActors)
	{
		if (L)
		{
			L->Destroy();
		}
	}
	LinkActors.Reset();
}

void UArenaRules_ONS::NetAddLink(AArenaONS_PowerNode* A, AArenaONS_PowerNode* B)
{
	UWorld* World = GetWorld();
	if (!World || !A || !B || A == B || A->IsLinkedTo(B))
	{
		return;
	}
	A->AddLink(B);
	B->AddLink(A);
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AArenaONS_PowerNode* From = A->GetNodeIndex() < B->GetNodeIndex() ? A : B;
	if (AArenaONS_PowerLink* LinkActor = World->SpawnActor<AArenaONS_PowerLink>(AArenaONS_PowerLink::StaticClass(), From->GetActorTransform(), Params))
	{
		LinkActor->InitLink(From, From == A ? B : A);
		LinkActors.Add(LinkActor);
	}
}

void UArenaRules_ONS::NetRemoveLink(AArenaONS_PowerNode* A, AArenaONS_PowerNode* B)
{
	if (!A || !B)
	{
		return;
	}
	A->RemoveLink(B);
	B->RemoveLink(A);
	for (int32 i = LinkActors.Num() - 1; i >= 0; --i)
	{
		AArenaONS_PowerLink* L = LinkActors[i];
		if (!L || (L->GetA() == A && L->GetB() == B) || (L->GetA() == B && L->GetB() == A))
		{
			if (L)
			{
				L->Destroy();
			}
			LinkActors.RemoveAt(i);
		}
	}
}

void UArenaRules_ONS::LogNetwork() const
{
	FString Line;
	for (const AArenaONS_PowerNode* Node : Nodes)
	{
		if (!Node)
		{
			continue;
		}
		const TCHAR* State = Node->IsNeutral() ? TEXT("neutral") : Node->IsConstructing() ? TEXT("building") : TEXT("active");
		FString Links;
		for (const AArenaONS_PowerNode* L : Node->GetLinks())
		{
			Links += FString::Printf(TEXT("%d,"), L ? L->GetNodeIndex() : -1);
		}
		Line += FString::Printf(TEXT(" [%d %s %s %s %.0f/%.0f links %s]"), Node->GetNodeIndex(), *Node->GetNodeName(), *TeamName(Node->GetTeam()), State,
			Node->GetHealth(), Node->GetMaxHealth(), *Links);
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: network%s"), *Line);
}

void UArenaRules_ONS::NotifyConstructionStarted(AArenaONS_PowerNode* Node, AController* By)
{
	UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: %s team started building %s (%s)"), *TeamName(Node->GetTeam()), *Node->GetNodeName(), *ControllerName(By));
	RefreshNetwork();
	if (GameMode)
	{
		GameMode->BroadcastTeamMessage(Node->GetTeam(), FString::Printf(TEXT("Building %s"), *Node->GetNodeName()), ArenaTeam::Color(Node->GetTeam()), 2.f);
	}
}

void UArenaRules_ONS::NotifyNodeBuilt(AArenaONS_PowerNode* Node, AController* By)
{
	++NumNodesBuilt;
	UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: node built: %s now powered by %s (builder %s)"), *Node->GetNodeName(), *TeamName(Node->GetTeam()), *ControllerName(By));
	if (AArenaPlayerState* PS = By ? By->GetPlayerState<AArenaPlayerState>() : nullptr)
	{
		PS->ObjectiveScore++;
		PS->AddScore(5.f);
	}
	RefreshNetwork();
	if (GameMode)
	{
		GameMode->BroadcastCenterMessage(FString::Printf(TEXT("%s team powered %s"), *TeamName(Node->GetTeam()), *Node->GetNodeName()), ArenaTeam::Color(Node->GetTeam()), 2.5f, TEXT("S_Flag_Returned"));
	}
}

void UArenaRules_ONS::NotifyNodeDestroyed(AArenaONS_PowerNode* Node, int32 OldTeam, AController* By)
{
	++NumNodesDestroyed;
	UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: node destroyed: %s (%s) by %s"), *Node->GetNodeName(), *TeamName(OldTeam), *ControllerName(By));
	if (AArenaPlayerState* PS = By ? By->GetPlayerState<AArenaPlayerState>() : nullptr)
	{
		PS->ObjectiveScore++;
		PS->AddScore(5.f);
	}
	RefreshNetwork();
	if (GameMode)
	{
		const int32 Attacker = OldTeam == 0 ? 1 : 0;
		GameMode->BroadcastCenterMessage(FString::Printf(TEXT("%s team's %s was destroyed"), *TeamName(OldTeam), *Node->GetNodeName()), ArenaTeam::Color(Attacker), 2.5f, TEXT("S_Flag_Taken"));
	}
}

void UArenaRules_ONS::NotifyCoreDamaged(AArenaONS_PowerNode* Core, float Damage, AController* By)
{
	UWorld* World = GetWorld();
	static TMap<TWeakObjectPtr<AArenaONS_PowerNode>, TPair<float, float>> LogState; // next log time, accumulated
	TPair<float, float>& State = LogState.FindOrAdd(Core, TPair<float, float>(0.f, 0.f));
	State.Value += Damage;
	const float Now = World ? World->GetTimeSeconds() : 0.f;
	if (Now >= State.Key)
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: core damaged: %s took %.0f (health %.0f/%.0f), last hit by %s"), *Core->GetNodeName(), State.Value,
			Core->GetHealth(), Core->GetMaxHealth(), *ControllerName(By));
		if (GameMode && State.Key > 0.f && Now - State.Key > 8.f)
		{
			GameMode->BroadcastTeamMessage(Core->GetTeam(), TEXT("Your core is under attack!"), FLinearColor(1.f, 0.3f, 0.2f), 2.f);
		}
		State = TPair<float, float>(Now + 2.f, 0.f);
	}
}

void UArenaRules_ONS::NotifyCoreDestroyed(AArenaONS_PowerNode* Core, AController* By)
{
	if (bCoreDestroyedThisRound || !GameMode)
	{
		return;
	}
	bCoreDestroyedThisRound = true;
	const int32 Winner = Core->GetTeam() == 0 ? 1 : 0;
	UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: CORE DESTROYED: %s by %s - %s team wins the round"), *Core->GetNodeName(), *ControllerName(By), *TeamName(Winner));
	if (AArenaPlayerState* PS = By ? By->GetPlayerState<AArenaPlayerState>() : nullptr)
	{
		PS->ObjectiveScore++;
		PS->AddScore(10.f);
	}
	// UT2004 ONSOnslaughtGame.MainCoreDestroyed: a core kill is worth 2 points, 1 in overtime.
	const AArenaGameState* ScoreGS = GetGameState();
	GameMode->AddTeamScore(Winner, (ScoreGS && ScoreGS->bOvertime) ? 1 : 2);
	EndWithCoreDestroyed(Winner, Core);
}

void UArenaRules_ONS::EndWithCoreDestroyed(int32 WinnerTeam, AArenaONS_PowerNode* Core)
{
	const AArenaGameState* GS = GetGameState();
	if (!GS || !GameMode)
	{
		return;
	}
	const int32 Goal = FMath::Max(1, GS->GoalScore);
	if (GS->TeamScores[WinnerTeam] >= Goal)
	{
		GameMode->EndMatch(FString::Printf(TEXT("%s Team wins!"), *TeamName(WinnerTeam)), WinnerTeam, nullptr);
	}
	else
	{
		GameMode->EndRound(WinnerTeam, Core ? FString::Printf(TEXT("%s team destroyed the %s!"), *TeamName(WinnerTeam), *Core->GetNodeName())
			: FString::Printf(TEXT("%s team takes the round"), *TeamName(WinnerTeam)), 7.f);
	}
}

void UArenaRules_ONS::CheckEndConditions()
{
	const AArenaGameState* GS = GetGameState();
	if (!GameMode || !GS || !GS->IsPlaying())
	{
		return;
	}
	if (GS->bOvertime && GS->TeamScores[0] != GS->TeamScores[1])
	{
		const int32 W = GS->TeamScores[0] > GS->TeamScores[1] ? 0 : 1;
		GameMode->EndMatch(FString::Printf(TEXT("%s Team wins!"), *TeamName(W)), W, nullptr);
	}
}

void UArenaRules_ONS::OnTimeLimitReached()
{
	AArenaGameState* GS = GetGameState();
	if (!GS || !GameMode)
	{
		return;
	}
	// Time out: the stronger core takes the round, then the team with more nodes.
	const AArenaONS_PowerNode* C0 = GetCore(0);
	const AArenaONS_PowerNode* C1 = GetCore(1);
	const float H0 = C0 ? C0->GetHealthFraction() : 0.f;
	const float H1 = C1 ? C1->GetHealthFraction() : 0.f;
	int32 Winner = -1;
	if (FMath::Abs(H0 - H1) > 0.01f)
	{
		Winner = H0 > H1 ? 0 : 1;
	}
	else
	{
		int32 Count[2] = { 0, 0 };
		for (const AArenaONS_PowerNode* N : Nodes)
		{
			if (N && !N->IsCore() && N->IsActive() && (N->GetTeam() == 0 || N->GetTeam() == 1))
			{
				Count[N->GetTeam()]++;
			}
		}
		if (Count[0] != Count[1])
		{
			Winner = Count[0] > Count[1] ? 0 : 1;
		}
	}
	if (Winner < 0)
	{
		Super::OnTimeLimitReached(); // tie: overtime
		return;
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: time limit: %s team takes the round (core health %.0f%% / %.0f%%)"), *TeamName(Winner), H0 * 100.f, H1 * 100.f);
	GameMode->AddTeamScore(Winner, 1);
	EndWithCoreDestroyed(Winner, nullptr);
}

// =============================================================================================
// Tick
// =============================================================================================

void UArenaRules_ONS::Tick(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	if (!World || !GameMode)
	{
		return;
	}
	const float Now = World->GetTimeSeconds();
	ProcessVehicleEntries();
	if (Now >= NextStatusTime)
	{
		NextStatusTime = Now + 1.f;
		RefreshNetwork();
		Vehicles.RemoveAll([](const TWeakObjectPtr<AArenaONS_Vehicle>& V) { return !V.IsValid(); });
		for (auto It = Reservations.CreateIterator(); It; ++It)
		{
			if (!It->Key.IsValid() || Now > It->Value.Value)
			{
				It.RemoveCurrent();
			}
		}
	}
	TickBotSiege();
	TickDebugView();
	if (Now >= NextNetworkLog)
	{
		NextNetworkLog = Now + 20.f;
		LogNetwork();
	}

	// Debug: put the local player into a vehicle driven by the AI (-ONSAutoDrive).
	static float NextAutoDrive = 0.f;
	if (ArenaONS::HasDebugFlag(TEXT("ONSAutoDrive")) && Now >= NextAutoDrive && GameMode->IsPlaying())
	{
		NextAutoDrive = Now + 3.f;
		APlayerController* PC = World->GetFirstPlayerController();
		AArenaCharacter* Char = PC ? Cast<AArenaCharacter>(PC->GetPawn()) : nullptr;
		if (Char && Char->IsAlive())
		{
			AArenaONS_Vehicle* Best = nullptr;
			float BestD = 6000.f;
			for (const TWeakObjectPtr<AArenaONS_Vehicle>& V : Vehicles)
			{
				if (V.IsValid() && V->GetFreeSeat() == 0 && V->CanBeEnteredBy(Char))
				{
					const float D = FVector::Dist(V->GetActorLocation(), Char->GetActorLocation());
					if (D < BestD)
					{
						BestD = D;
						Best = V.Get();
					}
				}
			}
			if (Best)
			{
				UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: debug autodrive: local player takes %s"), *Best->GetVehicleName());
				Best->EnterSeat(Char, 0);
				++NumVehicleEntries;
			}
		}
	}
}

void UArenaRules_ONS::RegisterVehicle(AArenaONS_Vehicle* Vehicle)
{
	if (Vehicle)
	{
		Vehicles.AddUnique(Vehicle);
	}
}

void UArenaRules_ONS::QueueVehicleEntry(AArenaCharacter* Char, AArenaONS_Vehicle* Vehicle)
{
	for (const FPendingEntry& E : PendingEntries)
	{
		if (E.Char.Get() == Char)
		{
			return;
		}
	}
	FPendingEntry E;
	E.Char = Char;
	E.Vehicle = Vehicle;
	PendingEntries.Add(E);
}

void UArenaRules_ONS::ProcessVehicleEntries()
{
	TArray<FPendingEntry> Entries = MoveTemp(PendingEntries);
	PendingEntries.Reset();
	for (const FPendingEntry& E : Entries)
	{
		AArenaCharacter* Char = E.Char.Get();
		AArenaONS_Vehicle* V = E.Vehicle.Get();
		if (!Char || !V || !Char->IsAlive() || !Char->GetController() || !V->CanBeEnteredBy(Char))
		{
			continue;
		}
		const int32 Seat = V->GetFreeSeat();
		if (Seat >= 0 && V->EnterSeat(Char, Seat))
		{
			++NumVehicleEntries;
		}
	}
}

void UArenaRules_ONS::NotifyLeftVehicle(AController* C, AArenaONS_Vehicle* Vehicle, bool bGaveUp)
{
	UWorld* World = GetWorld();
	if (C && World && Cast<AArenaBotController>(C))
	{
		NoVehicleUntil.Add(C, World->GetTimeSeconds() + (bGaveUp ? 25.f : 10.f));
		if (AArenaBotController* Bot = Cast<AArenaBotController>(C))
		{
			Bot->ForceGoalUpdate();
		}
	}
	Reservations.Remove(Vehicle);
}

void UArenaRules_ONS::TickBotSiege()
{
	UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.f;
	for (auto It = SiegeBots.CreateIterator(); It; ++It)
	{
		AArenaBotController* Bot = It->Key.Get();
		if (!Bot)
		{
			It.RemoveCurrent();
			continue;
		}
		if (Now - It->Value > 0.3f)
		{
			AArenaCharacter* P = Bot->GetArenaPawn();
			if (P && P->IsAlive() && !Bot->IsEnemyVisible())
			{
				P->StopFire(0);
			}
			It.RemoveCurrent();
		}
	}
}

void UArenaRules_ONS::TickDebugView()
{
	if (!ArenaONS::HasDebugFlag(TEXT("ONSViewVehicle")))
	{
		return;
	}
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return;
	}
	const float Now = World->GetTimeSeconds();
	AActor* Target = DebugViewTarget.Get();
	AArenaONS_Vehicle* TV = Cast<AArenaONS_Vehicle>(Target);
	if (!TV || !TV->HasDriver() || TV->IsDestroyedVehicle())
	{
		Target = nullptr;
		if (Now >= NextDebugViewTime)
		{
			NextDebugViewTime = Now + 0.5f;
			for (const TWeakObjectPtr<AArenaONS_Vehicle>& V : Vehicles)
			{
				if (V.IsValid() && V->HasDriver() && Cast<AArenaBotController>(V->GetController()))
				{
					Target = V.Get();
					UE_LOG(LogOpenUT2k4, Log, TEXT("ONS: debug view follows %s driven by %s"), *V->GetVehicleName(), *ControllerName(V->GetController()));
					break;
				}
			}
			DebugViewTarget = Target;
		}
	}
	if (Target && PC->GetViewTarget() != Target)
	{
		PC->SetViewTarget(Target);
	}
}

// =============================================================================================
// Spawning / use
// =============================================================================================

AActor* UArenaRules_ONS::ChoosePlayerStart(AController* Player, const TArray<APlayerStart*>& Candidates)
{
	const int32 Team = GetTeamOf(Player);
	UWorld* World = GetWorld();
	if ((Team != 0 && Team != 1) || SpawnStarts.Num() == 0 || !World)
	{
		return Super::ChoosePlayerStart(Player, Candidates);
	}
	// Owned objectives that have spawn spots
	TArray<AArenaONS_PowerNode*> Owned;
	for (AArenaONS_PowerNode* Node : Nodes)
	{
		if (Node && Node->IsOwnedBy(Team) && Node->SpawnSpots.Num() > 0)
		{
			Owned.Add(Node);
		}
	}
	if (Owned.Num() == 0)
	{
		return Super::ChoosePlayerStart(Player, Candidates);
	}
	const AArenaONS_PowerNode* EnemyCore = GetCore(1 - Team);
	auto FrontKey = [&](const AArenaONS_PowerNode* N)
	{
		const float ToEnemy = EnemyCore ? FVector::Dist(N->GetActorLocation(), EnemyCore->GetActorLocation()) : 0.f;
		return GetFrontDistance(N, Team) * 1000000.f + ToEnemy;
	};
	Owned.Sort([&](const AArenaONS_PowerNode& A, const AArenaONS_PowerNode& B) { return FrontKey(&A) < FrontKey(&B); });

	AArenaONS_PowerNode* Chosen = Owned[0]; // humans: owned node nearest the front
	if (AArenaBotController* Bot = Cast<AArenaBotController>(Player))
	{
		const float R = Bot->GetRandomStream().FRand();
		AArenaONS_PowerNode* Core = GetCore(Team);
		if (Bot->IsDefender() && R < 0.45f && Core && Core->SpawnSpots.Num() > 0)
		{
			Chosen = Core;
		}
		else if (R > 0.8f)
		{
			Chosen = Owned[Bot->GetRandomStream().RandRange(0, Owned.Num() - 1)];
		}
	}

	// Free start of that node, preferring ones away from enemies.
	APlayerStart* Best = nullptr;
	float BestScore = -TNumericLimits<float>::Max();
	for (APlayerStart* S : SpawnStarts)
	{
		const int32* Idx = S ? StartNode.Find(S) : nullptr;
		if (!Idx || *Idx != Chosen->GetNodeIndex())
		{
			continue;
		}
		const FVector Loc = S->GetActorLocation();
		float MinEnemy = 5000.f;
		bool bBlocked = false;
		for (TActorIterator<AArenaCharacter> It(World); It; ++It)
		{
			if (!It->IsAlive() || !It->GetActorEnableCollision())
			{
				continue;
			}
			const float D = FVector::Dist(It->GetActorLocation(), Loc);
			if (D < 90.f)
			{
				bBlocked = true;
				break;
			}
			if (It->GetTeam() != Team)
			{
				MinEnemy = FMath::Min(MinEnemy, D);
			}
		}
		if (!bBlocked)
		{
			// Parked vehicles and other pawns
			FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaONSSpawnFree), false, S);
			bBlocked = World->OverlapBlockingTestByChannel(Loc, FQuat::Identity, ECC_Pawn,
				FCollisionShape::MakeCapsule(ArenaTuning::CapsuleRadius + 10.f, ArenaTuning::CapsuleHalfHeight), QP);
		}
		if (bBlocked)
		{
			continue;
		}
		const float Score = MinEnemy + FMath::FRand() * 600.f;
		if (Score > BestScore)
		{
			BestScore = Score;
			Best = S;
		}
	}
	return Best ? Best : Super::ChoosePlayerStart(Player, Candidates);
}

void UArenaRules_ONS::ModifyDefaultInventory(AController* Controller, TArray<TSubclassOf<AArenaWeapon>>& Weapons) const
{
	if (!GameMode || !GameMode->IsInstagib())
	{
		Weapons.AddUnique(AWeap_ArcLinker::StaticClass()); // builds and repairs nodes with its beam
	}
}

AArenaONS_Vehicle* UArenaRules_ONS::FindUsableVehicle(AArenaCharacter* Char, float MaxDist) const
{
	if (!Char)
	{
		return nullptr;
	}
	AArenaONS_Vehicle* Best = nullptr;
	float BestD = MaxDist;
	for (const TWeakObjectPtr<AArenaONS_Vehicle>& WV : Vehicles)
	{
		AArenaONS_Vehicle* V = WV.Get();
		if (!V || !V->CanBeEnteredBy(Char))
		{
			continue;
		}
		// Distance from the character to the vehicle box
		const FVector Local = V->GetActorTransform().InverseTransformPositionNoScale(Char->GetActorLocation());
		const FVector E = V->GetSpec().HalfExtent;
		const FVector Clamped(FMath::Clamp(Local.X, -E.X, E.X), FMath::Clamp(Local.Y, -E.Y, E.Y), FMath::Clamp(Local.Z, -E.Z, E.Z));
		const float D = FVector::Dist(Local, Clamped);
		if (D < BestD)
		{
			BestD = D;
			Best = V;
		}
	}
	return Best;
}

bool UArenaRules_ONS::OnPlayerUse(AController* Controller)
{
	APawn* P = Controller ? Controller->GetPawn() : nullptr;
	if (AArenaCharacter* Char = Cast<AArenaCharacter>(P))
	{
		if (AArenaONS_Vehicle* V = FindUsableVehicle(Char, UseReach))
		{
			const int32 Seat = V->GetFreeSeat();
			if (Seat >= 0 && V->EnterSeat(Char, Seat))
			{
				++NumVehicleEntries;
				return true;
			}
		}
		return false;
	}
	if (AArenaONS_Vehicle* V = ArenaONS::GetControlledVehicle(Controller))
	{
		return V->LeaveSeat(ArenaONS::GetControlledSeat(Controller), false) != nullptr;
	}
	return false;
}

// =============================================================================================
// Bots
// =============================================================================================

bool UArenaRules_ONS::ChooseNodeObjective(int32 Team, const FVector& Location, uint32 Seed, bool bDefender, FArenaBotObjective& Out) const
{
	if (Team != 0 && Team != 1)
	{
		return false;
	}
	const AArenaONS_PowerNode* BestNode = nullptr;
	float BestScore = -1.f;
	int32 BestTag = 0;
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		const AArenaONS_PowerNode* N = Nodes[i];
		if (!N)
		{
			continue;
		}
		float Base = -1.f;
		int32 Tag = 0;
		if (N->IsAttackableBy(Team))
		{
			Base = N->IsCore() ? 1.35f : 1.1f;
			Tag = TagAttack;
		}
		else if (N->IsBuildableBy(Team))
		{
			Base = 1.15f;
			Tag = TagBuild;
		}
		else if (N->GetTeam() == Team && !N->IsNeutral() && N->IsAttackableBy(1 - Team))
		{
			Base = N->IsCore() ? 0.85f : (N->IsConstructing() ? 0.8f : 0.65f);
			if (bDefender)
			{
				Base *= 1.7f;
			}
			Tag = TagDefend;
		}
		else if (bDefender && N->IsCore() && N->GetTeam() == Team)
		{
			Base = 0.4f;
			Tag = TagDefend;
		}
		if (Base < 0.f)
		{
			continue;
		}
		const float Dist = FVector::Dist(Location, N->GetActorLocation());
		const float Score = Base + Hash01(Seed, uint32(i)) * 0.3f - FMath::Min(Dist / 25000.f, 0.5f);
		if (Score > BestScore)
		{
			BestScore = Score;
			BestNode = N;
			BestTag = Tag;
		}
	}
	if (!BestNode)
	{
		return false;
	}
	auto Nearest = [&Location](const TArray<FVector>& Points, const FVector& Fallback)
	{
		float BestD = TNumericLimits<float>::Max();
		FVector Best = Fallback;
		for (const FVector& P : Points)
		{
			const float D = FVector::DistSquared(P, Location);
			if (D < BestD)
			{
				BestD = D;
				Best = P;
			}
		}
		return Best;
	};
	const FVector NodeLoc = BestNode->GetActorLocation();
	const FVector Away = NodeLoc + (Location - NodeLoc).GetSafeNormal2D() * (BestNode->IsCore() ? 800.f : 600.f);
	AArenaONS_PowerNode* NodeActor = const_cast<AArenaONS_PowerNode*>(BestNode);
	switch (BestTag)
	{
	case TagAttack:
		Out = FArenaBotObjective(Nearest(BestNode->OuterPoints, Away), NodeActor, 1.f, EArenaBotAction::Attack, TagAttack);
		Out.bCompleteOnReach = false;
		break;
	case TagBuild:
		Out = FArenaBotObjective(Nearest(BestNode->InnerPoints, NodeLoc + FVector(0.f, 0.f, 10.f)), NodeActor, 1.05f, EArenaBotAction::Collect, TagBuild);
		Out.bCompleteOnReach = false;
		break;
	default:
	{
		const TArray<FVector>& Pts = BestNode->OuterPoints;
		const FVector Spot = Pts.Num() > 0 ? Pts[Hash2(Seed, uint32(BestNode->GetNodeIndex())) % uint32(Pts.Num())] : Away;
		Out = FArenaBotObjective(Spot, NodeActor, bDefender ? 0.95f : 0.75f, EArenaBotAction::Defend, TagDefend);
		Out.bCompleteOnReach = true;
		break;
	}
	}
	return true;
}

bool UArenaRules_ONS::GetDriveDestination(AController* Driver, const FVector& From, FVector& OutDest, AArenaONS_PowerNode*& OutNode, bool& bOutAttack)
{
	const int32 Team = ArenaONS::TeamOfController(Driver);
	const APlayerState* PS = Driver ? Driver->PlayerState.Get() : nullptr;
	const uint32 Seed = PS ? uint32(PS->GetPlayerId()) : 0u;
	FArenaBotObjective O;
	if (!ChooseNodeObjective(Team, From, Seed, false, O))
	{
		return false;
	}
	OutNode = Cast<AArenaONS_PowerNode>(O.Actor.Get());
	bOutAttack = O.Tag == TagAttack;
	OutDest = O.Location;
	return true;
}

AArenaONS_Vehicle* UArenaRules_ONS::FindVehicleForBot(AArenaBotController* Bot, const FVector& Location, float MaxDist, const FVector& Destination) const
{
	AArenaCharacter* Char = Bot ? Bot->GetArenaPawn() : nullptr;
	UWorld* World = GetWorld();
	if (!Char || !World)
	{
		return nullptr;
	}
	const float Now = World->GetTimeSeconds();
	AArenaONS_Vehicle* Best = nullptr;
	float BestD = MaxDist;
	for (const TWeakObjectPtr<AArenaONS_Vehicle>& WV : Vehicles)
	{
		AArenaONS_Vehicle* V = WV.Get();
		if (!V || V->IsDestroyedVehicle() || V->GetFreeSeat() != 0 || !V->CanBeEnteredBy(Char) || V->GetHealthFraction() < 0.3f)
		{
			continue;
		}
		if (const TPair<TWeakObjectPtr<AController>, float>* R = Reservations.Find(V))
		{
			if (R->Key.IsValid() && R->Key.Get() != Bot && Now < R->Value)
			{
				continue;
			}
		}
		const FVector VL = V->GetActorLocation();
		if (FMath::Abs(VL.Z - Location.Z) > 400.f)
		{
			continue;
		}
		// Only worth it if the vehicle still has a real trip ahead of it.
		if (!Destination.IsZero() && FVector::Dist(VL, Destination) < V->GetSpec().BotArriveRadius + 1200.f)
		{
			continue;
		}
		const float D = FVector::Dist(VL, Location);
		if (D < BestD)
		{
			BestD = D;
			Best = V;
		}
	}
	return Best;
}

void UArenaRules_ONS::GetBotObjectives(AArenaBotController* Bot, TArray<FArenaBotObjective>& OutGoals)
{
	AArenaCharacter* P = Bot ? Bot->GetArenaPawn() : nullptr;
	UWorld* World = GetWorld();
	if (!P || !World)
	{
		return;
	}
	const int32 Team = Bot->GetTeam();
	const APlayerState* PS = Bot->PlayerState;
	const uint32 Seed = PS ? uint32(PS->GetPlayerId()) : 0u;
	FArenaBotObjective O;
	if (!ChooseNodeObjective(Team, P->GetActorLocation(), Seed, Bot->IsDefender(), O))
	{
		return;
	}
	OutGoals.Add(O);

	// Far away: take a vehicle if one is close.
	const float Now = World->GetTimeSeconds();
	const float* Until = NoVehicleUntil.Find(Bot);
	if (FVector::Dist(P->GetActorLocation(), O.Location) > VehicleTripDist && (!Until || Now >= *Until))
	{
		if (AArenaONS_Vehicle* V = FindVehicleForBot(Bot, P->GetActorLocation(), VehicleSearchDist, O.Location))
		{
			FArenaBotObjective VO(V->GetActorLocation(), V, O.Priority + 0.3f, EArenaBotAction::Use, TagVehicle);
			VO.bTrackActor = true;
			VO.bCompleteOnReach = false;
			OutGoals.Add(VO);
			Reservations.Add(V, TPair<TWeakObjectPtr<AController>, float>(Bot, Now + 6.f));
		}
	}
}

bool UArenaRules_ONS::UpdateBotObjective(AArenaBotController* Bot, FArenaBotObjective& Objective)
{
	AArenaCharacter* P = Bot ? Bot->GetArenaPawn() : nullptr;
	UWorld* World = GetWorld();
	if (!P || !World)
	{
		return false;
	}
	const int32 Team = Bot->GetTeam();
	const float Now = World->GetTimeSeconds();
	switch (Objective.Tag)
	{
	case TagVehicle:
	{
		AArenaONS_Vehicle* V = Cast<AArenaONS_Vehicle>(Objective.Actor.Get());
		if (!V || !V->CanBeEnteredBy(P) || V->GetFreeSeat() != 0)
		{
			return false;
		}
		Objective.Location = V->GetActorLocation();
		Reservations.Add(V, TPair<TWeakObjectPtr<AController>, float>(Bot, Now + 3.f));
		const FVector Local = V->GetActorTransform().InverseTransformPositionNoScale(P->GetActorLocation());
		const FVector E = V->GetSpec().HalfExtent;
		const FVector Clamped(FMath::Clamp(Local.X, -E.X, E.X), FMath::Clamp(Local.Y, -E.Y, E.Y), FMath::Clamp(Local.Z, -E.Z, E.Z));
		if (FVector::Dist(Local, Clamped) < UseReach)
		{
			QueueVehicleEntry(P, V);
		}
		return true;
	}
	case TagBuild:
	{
		const AArenaONS_PowerNode* Node = Cast<AArenaONS_PowerNode>(Objective.Actor.Get());
		return Node && Node->IsBuildableBy(Team);
	}
	case TagDefend:
	{
		const AArenaONS_PowerNode* Node = Cast<AArenaONS_PowerNode>(Objective.Actor.Get());
		return Node && Node->GetTeam() == Team && !Node->IsNeutral();
	}
	case TagAttack:
	{
		AArenaONS_PowerNode* Node = Cast<AArenaONS_PowerNode>(Objective.Actor.Get());
		const bool bSieging = SiegeBots.Contains(Bot);
		if (!Node || !Node->IsAttackableBy(Team))
		{
			if (bSieging)
			{
				if (!Bot->IsEnemyVisible())
				{
					P->StopFire(0);
				}
				SiegeBots.Remove(Bot);
			}
			return false;
		}
		const FVector Eye = P->GetPawnViewLocation();
		const FVector Aim = Node->GetAimPoint();
		const float Dist = FVector::Dist(Eye, Aim);
		AArenaWeapon* W = P->GetCurrentWeapon();
		const bool bUsable = W && W->HasAmmoFor(0) && W->FireModes[0].FireType != EArenaFireType::Melee && W->FireModes[0].FireType != EArenaFireType::Zoom;
		if (!Bot->IsEnemyVisible() && Dist < 2600.f && bUsable)
		{
			FCollisionQueryParams QP(SCENE_QUERY_STAT(ArenaONSSiege), false, P);
			QP.AddIgnoredActor(Node);
			FHitResult Hit;
			if (!World->LineTraceSingleByChannel(Hit, Eye, Aim, ECC_Visibility, QP))
			{
				FRotator R = (Aim - Eye).Rotation();
				R.Roll = 0.f;
				Bot->SetControlRotation(R);
				if (!W->IsFiring(0))
				{
					P->StartFire(0);
				}
				SiegeBots.Add(Bot, Now);
				if (Dist < 1700.f)
				{
					Objective.Location = P->GetActorLocation(); // hold position and shoot
				}
				return true;
			}
		}
		if (bSieging && !Bot->IsEnemyVisible())
		{
			P->StopFire(0);
			SiegeBots.Remove(Bot);
		}
		return true;
	}
	default:
		return Super::UpdateBotObjective(Bot, Objective);
	}
}

float UArenaRules_ONS::GetBotEnemyWeight(AArenaBotController* Bot, AArenaCharacter* Candidate) const
{
	if (Candidate && !Candidate->GetController())
	{
		// Hidden vehicle riders stand in for their vehicle: enemies shoot at it, team mates ignore it.
		if (const AArenaONS_Vehicle* V = ArenaONS::GetRiddenVehicle(Candidate))
		{
			return V->GetTeam() == Bot->GetTeam() ? TNumericLimits<float>::Max() : 0.75f;
		}
	}
	return 1.f;
}

// =============================================================================================
// HUD
// =============================================================================================

bool UArenaRules_ONS::DrawScoreInfo(AArenaHUD* HUD, UCanvas* Canvas)
{
	const AArenaGameState* GS = GetGameState();
	UWorld* World = GetWorld();
	if (!HUD || !GS || !World)
	{
		return false;
	}
	if (HUD->IsUTHud())
	{
		// ONSHUDOnslaught: our node states as ONSPowerCore.CoreStage (0 active, 2 constructing, 4 neutral)
		const APlayerController* PC = HUD->GetOwningPlayerController();
		const AArenaPlayerState* MyPS = PC ? PC->GetPlayerState<AArenaPlayerState>() : nullptr;
		const int32 MyTeam = MyPS ? MyPS->Team : -1;
		TArray<AArenaONS_PowerNode*> All;
		for (TActorIterator<AArenaONS_PowerNode> It(World); It; ++It)
		{
			// hidden: a node a UT2004 server's link setup leaves out (CoreStage 255; the stock HUD hides it too)
			if (!It->IsHidden())
			{
				All.Add(*It);
			}
		}
		All.Sort([](const AArenaONS_PowerNode& A, const AArenaONS_PowerNode& B) { return A.GetNodeIndex() < B.GetNodeIndex(); });
		TArray<AArenaHUD::FUTPowerNode> HudNodes;
		TArray<TPair<int32, int32>> HudLinks;
		int32 Num = 0;
		const float T = HUD->Now();
		for (const AArenaONS_PowerNode* N : All)
		{
			AArenaHUD::FUTPowerNode U;
			U.Location = N->GetActorLocation();
			U.Team = (N->GetTeam() == 0 || N->GetTeam() == 1) && !N->IsNeutral() ? N->GetTeam() : 255;
			if (N->IsCore())
			{
				U.Team = N->GetTeam();
			}
			U.Stage = N->IsActive() ? 0 : (N->IsConstructing() ? 2 : 4);
			U.bFinalCore = N->IsCore();
			U.bUnderAttack = N->GetLastDamageTime() > 0.f && T - N->GetLastDamageTime() < 1.f;
			U.bAttackable = MyTeam >= 0 && (N->IsAttackableBy(MyTeam) || N->IsBuildableBy(MyTeam));
			U.NodeNum = N->IsCore() ? 0 : ++Num;
			U.Health = N->IsConstructing() ? N->GetBuildProgress() * N->GetMaxHealth() : N->GetHealth();
			U.MaxHealth = N->GetMaxHealth();
			HudNodes.Add(U);
		}
		for (int32 i = 0; i < All.Num(); ++i)
		{
			for (const AArenaONS_PowerNode* L : All[i]->GetLinks())
			{
				const int32 j = All.IndexOfByKey(L);
				if (j > i)
				{
					HudLinks.Add(TPair<int32, int32>(i, j));
				}
			}
		}
		HUD->DrawUTOnslaught(HudNodes, HudLinks, MyTeam);
		return true;
	}
	AArenaONS_PowerNode* Cores[2] = { nullptr, nullptr };
	for (TActorIterator<AArenaONS_PowerNode> It(World); It; ++It)
	{
		if (It->IsCore() && (It->GetTeam() == 0 || It->GetTeam() == 1))
		{
			Cores[It->GetTeam()] = *It;
		}
	}
	const FArenaDraw& D = HUD->GetDraw();
	const float Now = HUD->Now();
	HUD->DrawTeamScorePanels(30.f, [&](int32 Team, float X, float Top, float W)
	{
		const AArenaONS_PowerNode* Core = Cores[Team];
		const float Frac = Core ? Core->GetHealthFraction() : 0.f;
		const bool bHit = Core && Now - Core->GetLastDamageTime() < 0.6f;
		const FLinearColor Col = bHit && FMath::Fmod(Now * 8.f, 2.f) < 1.f ? FLinearColor::White : ArenaHudStyle::TeamTint(Team);
		const float RowY = Top + D.S(80.f);
		if (!ArenaArtHUD::DrawSpriteCentered(D, ArenaArtHUD::Sprites::Core(), X + D.S(28.f), RowY + D.S(11.f), D.S(24.f), FLinearColor(Col.R, Col.G, Col.B, 1.f)))
		{
			D.Text(TEXT("CORE"), X + D.S(10.f), RowY + D.S(2.f), ArenaHudStyle::TextDim(), D.S(12.f));
		}
		D.Bar(X + D.S(52.f), RowY + D.S(6.f), W - D.S(62.f), D.S(10.f), Frac, Col);
	}, GetGoalText(GS->GoalScore));
	return true;
}

void UArenaRules_ONS::DrawNodeHealth(AArenaHUD* HUD, UCanvas* Canvas)
{
	UWorld* World = GetWorld();
	APlayerController* PC = HUD ? HUD->PlayerOwner : nullptr;
	if (!World || !PC || !PC->PlayerCameraManager || !ArenaArtHUD::HasHudArt())
	{
		return;
	}
	const FArenaDraw& D = HUD->GetDraw();
	const FVector Cam = PC->PlayerCameraManager->GetCameraLocation();
	const FVector CamFwd = PC->PlayerCameraManager->GetCameraRotation().Vector();
	const float Now = HUD->Now();
	for (TActorIterator<AArenaONS_PowerNode> It(World); It; ++It)
	{
		const AArenaONS_PowerNode* N = *It;
		if (N->GetState() == EArenaONS_NodeState::Neutral)
		{
			continue;
		}
		const FVector Top = N->GetActorLocation() + FVector(0.f, 0.f, N->IsCore() ? 800.f : 480.f);
		const float Dist = FVector::Dist(Cam, Top);
		if (Dist > 6000.f || FVector::DotProduct((Top - Cam).GetSafeNormal(), CamFwd) < 0.5f)
		{
			continue;
		}
		// only nodes in sight (the node itself may block the trace)
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(ONSNodeHealthSight), false, PC->GetPawn());
		Q.AddIgnoredActor(N);
		if (World->LineTraceSingleByChannel(Hit, Cam, Top, ECC_Visibility, Q))
		{
			continue;
		}
		const FVector P = Canvas->Project(Top);
		if (P.Z <= 0.f)
		{
			continue;
		}
		// RechargeBar meter in a small HUD box, team tinted; flashes white while taking damage
		const float Scale = FMath::Clamp(1800.f / FMath::Max(1.f, Dist), 0.45f, 1.f);
		const float W = D.S(150.f) * Scale;
		const float H = D.S(30.f) * Scale;
		const float X = P.X - W * 0.5f;
		const float Y = P.Y - H * 0.5f;
		const int32 Team = N->GetTeam();
		const bool bFlash = Now - N->GetLastDamageTime() < 0.4f && FMath::Fmod(Now * 10.f, 2.f) < 1.f;
		const FLinearColor Tint = bFlash ? FLinearColor::White : ((Team == 0 || Team == 1) ? ArenaHudStyle::TeamTint(Team) : FLinearColor(0.7f, 0.7f, 0.7f));
		ArenaArtHUD::DrawSprite(D, ArenaArtHUD::Sprites::Box(), X - D.S(6.f) * Scale, Y - D.S(5.f) * Scale, W + D.S(12.f) * Scale, H + D.S(10.f) * Scale,
			FLinearColor(Tint.R, Tint.G, Tint.B, 0.8f));
		ArenaArtHUD::DrawSpriteFraction(D, ArenaArtHUD::Sprites::Meter(), X, Y, W, H, N->GetHealthFraction(), FLinearColor::White);
	}
}

void UArenaRules_ONS::DrawHUD(AArenaHUD* HUD, UCanvas* Canvas)
{
	UWorld* World = GetWorld();
	if (!HUD || !Canvas || !World)
	{
		return;
	}
	if (HUD->IsUTHud())
	{
		// UT2004: the vehicle's health box next to the driver's (HudCDeathmatch.DrawHudPassA in a vehicle); the node map and
		// health bars are part of the score pass (DrawScoreInfo)
		APawn* OwnPawn = HUD->GetOwningPawn();
		const AArenaONS_Vehicle* V = Cast<AArenaONS_Vehicle>(OwnPawn);
		if (const AArenaONS_VehicleSeat* Seat = Cast<AArenaONS_VehicleSeat>(OwnPawn))
		{
			V = Seat->GetVehicle();
		}
		if (V)
		{
			HUD->DrawUTVehicleHud(V->GetDriverCharacter(), FMath::CeilToInt(V->GetHealth()), V->GetHealthFraction() > 0.f ? V->GetHealth() / V->GetHealthFraction() : 1.f);
		}
		return;
	}
	DrawNodeHealth(HUD, Canvas);
	const FArenaDraw& D = HUD->GetDraw();
	const float Now = HUD->Now();
	APlayerController* PC = HUD->PlayerOwner;
	const AArenaPlayerState* MyPS = PC ? PC->GetPlayerState<AArenaPlayerState>() : nullptr;
	const int32 MyTeam = MyPS ? MyPS->Team : -1;

	// ---- power network map (under the score panel) ----
	TArray<AArenaONS_PowerNode*> AllNodes;
	AArenaONS_PowerNode* Cores[2] = { nullptr, nullptr };
	for (TActorIterator<AArenaONS_PowerNode> It(World); It; ++It)
	{
		if (It->IsHidden())
		{
			continue; // left out of a UT2004 server's link setup
		}
		AllNodes.Add(*It);
		if (It->IsCore() && (It->GetTeam() == 0 || It->GetTeam() == 1))
		{
			Cores[It->GetTeam()] = *It;
		}
	}
	if (AllNodes.Num() > 0)
	{
		const float PW = D.S(300.f);
		const float PH = D.S(150.f);
		const float PX = D.W() - PW - D.S(20.f);
		const float PY = HUD->GetScoreInfoBottom() + D.S(10.f);
		D.Panel(PX, PY, PW, PH, ArenaHudStyle::AccentBlue(), 0.85f);
		const bool bArt = ArenaArtHUD::HasHudArt();
		if (bArt)
		{
			const ArenaArtHUD::FHudSprite Border{ ArenaArtHUD::Tex::MapBorder(), 0.f, 0.f, 256.f, 256.f, 256.f, 256.f };
			ArenaArtHUD::DrawNineSlice(D, Border, 28.f, PX, PY, PW, PH, D.S(22.f), FLinearColor(0.7f, 0.85f, 1.f, 0.8f));
		}
		D.Text(TEXT("POWER GRID"), PX + D.S(12.f), PY + D.S(6.f), ArenaHudStyle::TextDim(), D.S(12.f));

		// Axis from the red core to the blue core is horizontal.
		FVector2D Axis(1.f, 0.f);
		FVector2D Mid(0.f, 0.f);
		if (Cores[0] && Cores[1])
		{
			const FVector A = Cores[0]->GetActorLocation();
			const FVector B = Cores[1]->GetActorLocation();
			Axis = FVector2D(B.X - A.X, B.Y - A.Y).GetSafeNormal();
			Mid = FVector2D((A.X + B.X) * 0.5f, (A.Y + B.Y) * 0.5f);
		}
		const FVector2D Perp(-Axis.Y, Axis.X);
		float MaxU = 1.f, MaxV = 1.f;
		TMap<const AArenaONS_PowerNode*, FVector2D> UV;
		for (const AArenaONS_PowerNode* N : AllNodes)
		{
			const FVector2D P(N->GetActorLocation().X - Mid.X, N->GetActorLocation().Y - Mid.Y);
			const FVector2D Q(FVector2D::DotProduct(P, Axis), FVector2D::DotProduct(P, Perp));
			UV.Add(N, Q);
			MaxU = FMath::Max(MaxU, FMath::Abs(Q.X));
			MaxV = FMath::Max(MaxV, FMath::Abs(Q.Y));
		}
		const float InnerW = PW - D.S(50.f);
		const float InnerH = PH - D.S(52.f);
		const float CX = PX + PW * 0.5f;
		const float CY = PY + D.S(24.f) + InnerH * 0.5f + D.S(6.f);
		auto ToScreen = [&](const AArenaONS_PowerNode* N)
		{
			const FVector2D Q = UV.FindRef(N);
			return FVector2D(CX + Q.X / MaxU * InnerW * 0.5f, CY - Q.Y / FMath::Max(MaxV, MaxU * 0.35f) * InnerH * 0.5f);
		};
		auto NodeColor = [](const AArenaONS_PowerNode* N)
		{
			if (N->IsNeutral())
			{
				return FLinearColor(0.45f, 0.45f, 0.5f);
			}
			const FLinearColor C = ArenaHudStyle::TeamTint(N->GetTeam());
			return N->IsConstructing() ? C * 0.55f : C;
		};
		for (const AArenaONS_PowerNode* N : AllNodes)
		{
			for (const AArenaONS_PowerNode* L : N->GetLinks())
			{
				if (L && N->GetNodeIndex() < L->GetNodeIndex())
				{
					const FVector2D A = ToScreen(N);
					const FVector2D B = ToScreen(L);
					const bool bPowered = N->IsActive() && L->IsActive() && N->GetTeam() == L->GetTeam();
					const FLinearColor C = bPowered ? ArenaHudStyle::TeamTint(N->GetTeam()) : FLinearColor(0.35f, 0.35f, 0.4f, 0.8f);
					D.Line(A.X, A.Y, B.X, B.Y, C, D.S(bPowered ? 3.f : 2.f));
				}
			}
		}
		for (const AArenaONS_PowerNode* N : AllNodes)
		{
			const FVector2D S = ToScreen(N);
			const float R = D.S(N->IsCore() ? 10.f : 7.f);
			const bool bFlash = Now - N->GetLastDamageTime() < 0.6f && FMath::Fmod(Now * 8.f, 2.f) < 1.f;
			const bool bTarget = MyTeam >= 0 && (N->IsAttackableBy(MyTeam) || N->IsBuildableBy(MyTeam));
			if (bArt)
			{
				// node icons: core, disc (rings while building), target brackets, flash when hit
				using namespace ArenaArtHUD;
				const FLinearColor C = NodeColor(N);
				if (N->IsCore())
				{
					DrawSpriteCentered(D, Sprites::NodeCore(), S.X, S.Y, R * 2.8f, FLinearColor(C.R, C.G, C.B, 1.f));
				}
				else
				{
					DrawSpriteCentered(D, N->IsConstructing() ? Sprites::NodeBuilding() : Sprites::NodeDisc(), S.X, S.Y, R * 2.4f, FLinearColor(C.R, C.G, C.B, 1.f));
				}
				if (bTarget)
				{
					const float Pulse = 0.5f + 0.5f * FMath::Sin(Now * 5.f);
					DrawSpriteCentered(D, Sprites::NodeTarget(), S.X, S.Y, R * 2.f + D.S(14.f), FLinearColor(1.f, 0.9f, 0.3f, 0.5f + 0.5f * Pulse));
				}
				if (bFlash)
				{
					DrawSpriteCentered(D, Sprites::NodeAttack(), S.X, S.Y, R * 3.2f, FLinearColor(1.f, 0.95f, 0.8f, 0.9f), true);
				}
			}
			else
			{
				if (bTarget)
				{
					const float Pulse = 0.5f + 0.5f * FMath::Sin(Now * 5.f);
					D.Outline(S.X - R - D.S(4.f), S.Y - R - D.S(4.f), 2.f * R + D.S(8.f), 2.f * R + D.S(8.f), FLinearColor(1.f, 0.9f, 0.3f, 0.4f + 0.6f * Pulse), D.S(2.f));
				}
				D.Rect(S.X - R, S.Y - R, 2.f * R, 2.f * R, bFlash ? FLinearColor::White : NodeColor(N));
			}
			if (N->IsConstructing())
			{
				D.Bar(S.X - D.S(12.f), S.Y + R + D.S(3.f), D.S(24.f), D.S(4.f), N->GetBuildProgress(), ArenaHudStyle::TeamTint(N->GetTeam()));
			}
			else if (!N->IsNeutral() && N->GetHealthFraction() < 0.999f)
			{
				D.Bar(S.X - D.S(12.f), S.Y + R + D.S(3.f), D.S(24.f), D.S(4.f), N->GetHealthFraction(), ArenaHudStyle::HealthColor());
			}
		}
		D.Text(bArt ? TEXT("Yellow brackets: your team can attack / build it") : TEXT("Yellow frame: your team can attack / build it"), PX + D.S(12.f), PY + PH - D.S(20.f), ArenaHudStyle::TextDim(), D.S(11.f));
	}

	// ---- vehicle HUD (driving, riding, or spectating a vehicle) ----
	APawn* OwnPawn = HUD->GetOwningPawn();
	AActor* ViewTarget = PC ? PC->GetViewTarget() : nullptr;
	AArenaONS_Vehicle* V = nullptr;
	int32 Seat = 0;
	if (AArenaONS_Vehicle* OV = Cast<AArenaONS_Vehicle>(OwnPawn))
	{
		V = OV;
	}
	else if (AArenaONS_VehicleSeat* OS = Cast<AArenaONS_VehicleSeat>(OwnPawn))
	{
		V = OS->GetVehicle();
		Seat = OS->GetSeatIndex();
	}
	else if (AArenaONS_Vehicle* TV = Cast<AArenaONS_Vehicle>(ViewTarget))
	{
		V = TV;
	}
	if (V)
	{
		const FArenaONS_VehicleSpec& S = V->GetSpec();
		const FLinearColor TeamCol = ArenaHudStyle::TeamTint(V->GetTeam());
		const float PW = D.S(380.f);
		const float PH = D.S(150.f);
		const float X = D.S(30.f);
		// Above the normal vitals panel when we only spectate a vehicle.
		const float Y = D.H() - PH - D.S(Cast<AArenaCharacter>(OwnPawn) ? 215.f : 30.f);
		D.Panel(X, Y, PW, PH, TeamCol);
		D.Text(FString::Printf(TEXT("%s%s"), *S.Name, Seat == 1 ? TEXT("  (gunner)") : TEXT("")), X + D.S(16.f), Y + D.S(8.f), ArenaHudStyle::TextWhite(), D.S(24.f));
		D.Text(S.Role, X + PW - D.S(16.f), Y + D.S(14.f), ArenaHudStyle::TextDim(), D.S(14.f), EArenaAlign::Right);
		const float HF = V->GetHealthFraction();
		const FLinearColor HC = HF < 0.3f ? FLinearColor(1.f, 0.25f, 0.2f) : ArenaHudStyle::HealthColor();
		D.Text(TEXT("ARMOUR"), X + D.S(16.f), Y + D.S(44.f), ArenaHudStyle::TextDim(), D.S(12.f));
		D.Bar(X + D.S(90.f), Y + D.S(46.f), PW - D.S(170.f), D.S(12.f), HF, HC);
		D.Text(FString::Printf(TEXT("%d"), FMath::RoundToInt(V->GetHealth())), X + PW - D.S(16.f), Y + D.S(42.f), ArenaHudStyle::TextWhite(), D.S(16.f), EArenaAlign::Right);
		float RowY = Y + D.S(68.f);
		for (uint8 Mode = 0; Mode < 2; ++Mode)
		{
			const FArenaONS_GunSpec* G = V->FindGun(Seat, Mode);
			if (!G)
			{
				continue;
			}
			const float H = FMath::Clamp(V->GetGunHeat(Seat, Mode), 0.f, 1.f);
			const FLinearColor C = H > 0.85f ? FLinearColor(1.f, 0.3f, 0.15f) : FLinearColor(1.f, 0.75f, 0.25f);
			D.Text(FString::Printf(TEXT("%s %s"), Mode == 0 ? TEXT("FIRE") : TEXT("ALT"), *G->Name), X + D.S(16.f), RowY, ArenaHudStyle::TextWhite(), D.S(13.f));
			D.Bar(X + D.S(200.f), RowY + D.S(3.f), PW - D.S(216.f), D.S(9.f), G->bBoost ? 1.f - H : H, G->bBoost ? ArenaHudStyle::AccentBlue() : C);
			RowY += D.S(22.f);
		}
		const int32 Kmh = FMath::RoundToInt(V->GetVelocity().Size() * 0.036f);
		FString Hint = TEXT("[E] leave");
		switch (S.Mode)
		{
		case EArenaONS_DriveMode::Hover: Hint += TEXT("   [Space] hop   [A/D] strafe"); break;
		case EArenaONS_DriveMode::Flyer: Hint += TEXT("   [Space] climb   [C] descend"); break;
		case EArenaONS_DriveMode::Wheeled: Hint += Seat == 0 ? TEXT("   [Space] handbrake") : TEXT(""); break;
		default: break;
		}
		D.Text(FString::Printf(TEXT("%d km/h"), Kmh), X + D.S(16.f), Y + PH - D.S(26.f), ArenaHudStyle::TextWhite(), D.S(14.f));
		D.Text(Hint, X + PW - D.S(16.f), Y + PH - D.S(24.f), ArenaHudStyle::TextDim(), D.S(12.f), EArenaAlign::Right);

		// Crosshair
		const float CX = D.W() * 0.5f;
		const float CY = D.H() * 0.5f;
		const FLinearColor XC(0.9f, 1.f, 0.9f, 0.85f);
		D.Line(CX - D.S(14.f), CY, CX - D.S(5.f), CY, XC, D.S(2.f));
		D.Line(CX + D.S(5.f), CY, CX + D.S(14.f), CY, XC, D.S(2.f));
		D.Line(CX, CY - D.S(14.f), CX, CY - D.S(5.f), XC, D.S(2.f));
		D.Line(CX, CY + D.S(5.f), CX, CY + D.S(14.f), XC, D.S(2.f));
		// Turret aim marker
		const FArenaONS_GunSpec* Main = V->FindGun(Seat, 0);
		if (Main && Main->bTurret)
		{
			const USceneComponent* Barrel = Seat == 1 ? V->RearBarrelRoot.Get() : V->BarrelRoot.Get();
			if (Barrel)
			{
				const FVector P = Canvas->Project(Barrel->GetComponentLocation() + Barrel->GetForwardVector() * 4000.f);
				if (P.Z > 0.f)
				{
					D.Arc(P.X, P.Y, D.S(9.f), D.S(12.f), 0.f, 2.f * PI, FLinearColor(1.f, 0.6f, 0.2f, 0.9f), 20);
				}
			}
		}
	}
	else if (AArenaCharacter* Char = Cast<AArenaCharacter>(OwnPawn))
	{
		// On foot: prompt for a nearby vehicle
		if (Char->IsAlive())
		{
			for (TActorIterator<AArenaONS_Vehicle> It(World); It; ++It)
			{
				AArenaONS_Vehicle* Veh = *It;
				if (FVector::Dist(Veh->GetActorLocation(), Char->GetActorLocation()) > Veh->GetSpec().HalfExtent.X + 300.f || !Veh->CanBeEnteredBy(Char))
				{
					continue;
				}
				const int32 FreeSeat = Veh->GetFreeSeat();
				const FString Text = FString::Printf(TEXT("[E]  %s %s"), FreeSeat == 0 ? TEXT("Drive") : TEXT("Man the gun of"), *Veh->GetVehicleName());
				const FVector2D Size = D.TextSize(Text, D.S(22.f));
				const float BX = (D.W() - Size.X) * 0.5f - D.S(16.f);
				const float BY = D.H() * 0.68f;
				D.Panel(BX, BY, Size.X + D.S(32.f), D.S(40.f), ArenaHudStyle::TeamTint(Veh->GetTeam()));
				D.TextMid(Text, D.W() * 0.5f, BY + D.S(20.f), ArenaHudStyle::TextWhite(), D.S(22.f), EArenaAlign::Center);
				break;
			}
		}
	}
}

FString UArenaRules_ONS::GetScoreboardExtra(const AArenaPlayerState* Player) const
{
	if (Player && Player->ObjectiveScore > 0)
	{
		return FString::Printf(TEXT("%d obj"), Player->ObjectiveScore);
	}
	return FString();
}

FString UArenaRules_ONS::GetDeadScreenMessage(const AArenaPlayerState* Player) const
{
	const APawn* P = Player ? Player->GetPawn() : nullptr;
	if (Cast<AArenaONS_Vehicle>(P) || Cast<AArenaONS_VehicleSeat>(P))
	{
		return FString();
	}
	return TEXT("Press FIRE to respawn at the front line");
}
