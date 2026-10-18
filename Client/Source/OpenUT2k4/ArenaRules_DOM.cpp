#include "ArenaRules_DOM.h"
#include "OpenUT2k4.h"
#include "ArenaAssets.h"
#include "ArenaGameMode.h"
#include "ArenaGameState.h"
#include "ArenaPlayerState.h"
#include "ArenaCharacter.h"
#include "ArenaBotController.h"
#include "ArenaHUD.h"
#include "ArenaArtHUD.h"
#include "ArenaLevelBuilder.h"
#include "ArenaNavGraph.h"
#include "ArenaPickup.h"
#include "ArenaDOM_ControlPoint.h"
#include "Engine/World.h"
#include "Engine/Canvas.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/PlayerController.h"
#include "Dom/JsonObject.h"

ARENA_REGISTER_RULES(UArenaRules_DOM, "DOM", "Double Domination");

namespace ArenaDOM
{
	static const TCHAR* const PointLabels[2] = { TEXT("A"), TEXT("B") };

	static FString NameOf(const AController* C)
	{
		return (C && C->PlayerState) ? C->PlayerState->GetPlayerName() : FString(TEXT("Someone"));
	}

	static int32 StartTeam(const APlayerStart* S)
	{
		if (!S)
		{
			return -1;
		}
		if (S->PlayerStartTag == FName(TEXT("Red")) || S->ActorHasTag(TEXT("Red")))
		{
			return 0;
		}
		return (S->PlayerStartTag == FName(TEXT("Blue")) || S->ActorHasTag(TEXT("Blue"))) ? 1 : -1;
	}

	/** 0 = A, 1 = B, -1 = unknown, from whatever the importer wrote. */
	static int32 MarkerLetter(const FArenaRawMarker& M)
	{
		auto FromString = [](FString S) -> int32
		{
			S.TrimStartAndEndInline();
			if (S.Equals(TEXT("A"), ESearchCase::IgnoreCase) || S.EndsWith(TEXT("PointA"), ESearchCase::IgnoreCase))
			{
				return 0;
			}
			if (S.Equals(TEXT("B"), ESearchCase::IgnoreCase) || S.EndsWith(TEXT("PointB"), ESearchCase::IgnoreCase))
			{
				return 1;
			}
			return -1;
		};
		FString Str;
		static const TCHAR* const Keys[] = { TEXT("letter"), TEXT("point_name"), TEXT("PointName"), TEXT("name") };
		for (const TCHAR* Key : Keys)
		{
			if ((M.Json.IsValid() && M.Json->TryGetStringField(Key, Str)) || M.GetStringProperty(Key, Str))
			{
				const int32 L = FromString(Str);
				if (L >= 0)
				{
					return L;
				}
			}
		}
		int32 L = FromString(M.Class);
		if (L < 0)
		{
			for (const FString& C : M.ClassChain)
			{
				if ((L = FromString(C)) >= 0)
				{
					break;
				}
			}
		}
		return L;
	}

	static void DrawMarker(AArenaHUD* HUD, UCanvas* Canvas, const FVector& WorldLoc, const FString& Label, const FLinearColor& Color)
	{
		APlayerController* PC = HUD ? HUD->GetOwningPlayerController() : nullptr;
		if (!PC || !Canvas)
		{
			return;
		}
		const FArenaDraw& D = HUD->GetDraw();
		FVector ViewLoc;
		FRotator ViewRot;
		PC->GetPlayerViewPoint(ViewLoc, ViewRot);
		const FVector Dir = WorldLoc - ViewLoc;
		const float Dist = Dir.Size();
		const bool bInFront = FVector::DotProduct(Dir, ViewRot.Vector()) > 1.f;
		const float Margin = D.S(40.f);
		FVector2D P(0.f, 0.f);
		if (bInFront)
		{
			const FVector S = Canvas->Project(WorldLoc);
			P = FVector2D(S.X, S.Y);
		}
		if (!bInFront || P.X < Margin || P.X > D.W() - Margin || P.Y < Margin || P.Y > D.H() - Margin)
		{
			const FVector Local = ViewRot.UnrotateVector(Dir);
			FVector2D V(Local.Y, -Local.Z);
			if (V.IsNearlyZero())
			{
				V = FVector2D(0.f, 1.f);
			}
			V.Normalize();
			const float SX = V.X != 0.f ? (D.W() * 0.5f - Margin) / FMath::Abs(V.X) : 1e6f;
			const float SY = V.Y != 0.f ? (D.H() * 0.5f - Margin) / FMath::Abs(V.Y) : 1e6f;
			P = FVector2D(D.W() * 0.5f, D.H() * 0.5f) + V * FMath::Min(SX, SY);
		}
		const float R = D.S(15.f);
		D.Rect(P.X - R, P.Y - R, R * 2.f, R * 2.f, FLinearColor(0.f, 0.f, 0.f, 0.45f));
		D.Outline(P.X - R, P.Y - R, R * 2.f, R * 2.f, Color, D.S(2.f));
		D.TextMid(Label, P.X, P.Y, Color, D.S(20.f), EArenaAlign::Center);
		D.Text(FString::Printf(TEXT("%dm"), FMath::RoundToInt(Dist / 100.f)), P.X, P.Y + R + D.S(2.f), Color, D.S(13.f), EArenaAlign::Center);
	}
}

bool UArenaRules_DOM::SupportsLayout(const FArenaLayoutInfo& Layout) const
{
	return Layout.HasFeature(TEXT("raw:domination_points")) || Layout.GameTypePrefix == TEXT("DOM");
}

FString UArenaRules_DOM::GetGoalText(int32 Goal) const
{
	return Goal > 0 ? FString::Printf(TEXT("First to %d"), Goal) : FString();
}

float UArenaRules_DOM::ServerTime() const
{
	const AArenaGameState* GS = GetGameState();
	return GS ? GS->GetServerWorldTimeSeconds() : (GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f);
}

AArenaDOM_ControlPoint* UArenaRules_DOM::GetPoint(int32 Index) const
{
	if (Index != 0 && Index != 1)
	{
		return nullptr;
	}
	if (Points[Index])
	{
		return Points[Index];
	}
	if (const UWorld* World = GetWorld())
	{
		for (TActorIterator<AArenaDOM_ControlPoint> It(World); It; ++It)
		{
			if (It->Label == ArenaDOM::PointLabels[Index])
			{
				return *It;
			}
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Level setup
// ---------------------------------------------------------------------------------------------

bool UArenaRules_DOM::FindPositions(AArenaLevelBuilder* Builder, FVector OutPos[2]) const
{
	bool bHave[2] = { false, false };

	// 1. Raw markers (field names are not final)
	if (Builder->IsImportedMap())
	{
		TArray<FArenaRawMarker> Markers = Builder->GetRawMarkers(TEXT("domination_points"));
		Markers.Append(Builder->GetRawMarkers(TEXT("dom_points")));
		for (const FArenaRawMarker& M : Builder->GetRawMarkers(TEXT("game_objectives")))
		{
			if (M.IsA(TEXT("xDomPoint")) || M.IsA(TEXT("DominationPoint")) || M.Class.Contains(TEXT("DomPoint")))
			{
				Markers.Add(M);
			}
		}
		TArray<const FArenaRawMarker*> Unlabelled;
		for (const FArenaRawMarker& M : Markers)
		{
			const int32 L = ArenaDOM::MarkerLetter(M);
			if ((L == 0 || L == 1) && !bHave[L])
			{
				FVector Floor;
				OutPos[L] = Builder->TraceFloor(M.Location, 3000.f, Floor) ? Floor : M.Location;
				bHave[L] = true;
			}
			else
			{
				Unlabelled.Add(&M);
			}
		}
		for (const FArenaRawMarker* M : Unlabelled)
		{
			const int32 L = !bHave[0] ? 0 : (!bHave[1] ? 1 : -1);
			if (L < 0)
			{
				break;
			}
			FVector Floor;
			OutPos[L] = Builder->TraceFloor(M->Location, 3000.f, Floor) ? Floor : M->Location;
			bHave[L] = true;
		}
	}

	// 2. Anchors
	const TCHAR* const AnchorNames[2] = { TEXT("ControlA"), TEXT("ControlB") };
	for (int32 i = 0; i < 2; ++i)
	{
		FTransform T;
		if (!bHave[i] && Builder->GetAnchorPoint(AnchorNames[i], T))
		{
			OutPos[i] = T.GetLocation();
			bHave[i] = true;
		}
	}
	if (bHave[0] && bHave[1])
	{
		return true;
	}

	// 3. Fallback: two pickups (always on walkable floor) to either side of the line between the team spawns
	TArray<FVector> Candidates;
	for (const AArenaPickup* P : Builder->GetPickups())
	{
		if (P)
		{
			Candidates.Add(P->GetActorLocation());
		}
	}
	if (Candidates.Num() < 2)
	{
		for (const APlayerStart* S : Builder->GetPlayerStarts())
		{
			if (S)
			{
				Candidates.Add(S->GetActorLocation());
			}
		}
	}
	if (Candidates.Num() < 2)
	{
		return false;
	}
	FVector Sum[2] = { FVector::ZeroVector, FVector::ZeroVector };
	int32 Num[2] = { 0, 0 };
	for (const APlayerStart* S : Builder->GetPlayerStarts())
	{
		const int32 T = ArenaDOM::StartTeam(S);
		if (T >= 0)
		{
			Sum[T] += S->GetActorLocation();
			Num[T]++;
		}
	}
	FVector Want[2];
	if (Num[0] > 0 && Num[1] > 0)
	{
		const FVector C0 = Sum[0] / Num[0], C1 = Sum[1] / Num[1];
		const FVector Mid = (C0 + C1) * 0.5f;
		const FVector Perp = FVector::CrossProduct((C1 - C0).GetSafeNormal2D(), FVector::UpVector);
		const float D = FVector::Dist2D(C0, C1) * 0.35f;
		Want[0] = Mid + Perp * D;
		Want[1] = Mid - Perp * D;
	}
	else
	{
		const FBox Bounds = Builder->GetArenaBounds();
		const FVector Ext = Bounds.GetExtent();
		const FVector Axis = Ext.X >= Ext.Y ? FVector(Ext.X * 0.5f, 0.f, 0.f) : FVector(0.f, Ext.Y * 0.5f, 0.f);
		Want[0] = Bounds.GetCenter() - Axis;
		Want[1] = Bounds.GetCenter() + Axis;
	}
	for (int32 i = 0; i < 2; ++i)
	{
		if (bHave[i])
		{
			continue;
		}
		float Best = BIG_NUMBER;
		for (const FVector& C : Candidates)
		{
			if (bHave[1 - i] && FVector::Dist(C, OutPos[1 - i]) < 1500.f)
			{
				continue;
			}
			if (FVector::Dist(C, Want[i]) < Best)
			{
				Best = FVector::Dist(C, Want[i]);
				OutPos[i] = C;
			}
		}
		if (Best < BIG_NUMBER)
		{
			FVector Floor;
			if (Builder->TraceFloor(OutPos[i], 400.f, Floor))
			{
				OutPos[i] = Floor;
			}
			bHave[i] = true;
			UE_LOG(LogOpenUT2k4, Log, TEXT("DOM: no marker for point %s, using %s"), ArenaDOM::PointLabels[i], *OutPos[i].ToString());
		}
	}
	return bHave[0] && bHave[1];
}

AArenaDOM_ControlPoint* UArenaRules_DOM::SpawnPoint(const FVector& Floor, const FString& InLabel)
{
	UWorld* World = GetWorld();
	const FTransform T(FRotator::ZeroRotator, Floor);
	AArenaDOM_ControlPoint* P = World->SpawnActorDeferred<AArenaDOM_ControlPoint>(AArenaDOM_ControlPoint::StaticClass(), T, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (P)
	{
		P->Label = InLabel;
		P->FinishSpawning(T);
	}
	return P;
}

bool UArenaRules_DOM::OnLevelBuilt(AArenaLevelBuilder* Builder)
{
	if (!Builder || !GetWorld())
	{
		return false;
	}
	FVector Pos[2];
	if (!FindPositions(Builder, Pos))
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("DOM: %s has no control point positions, playing Team Deathmatch instead"), *Builder->GetBuiltLayoutId());
		return false;
	}
	for (int32 i = 0; i < 2; ++i)
	{
		Points[i] = SpawnPoint(Pos[i], ArenaDOM::PointLabels[i]);
	}
	if (!Points[0] || !Points[1])
	{
		return false;
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("DOM: point A at %s, point B at %s"), *Pos[0].ToString(), *Pos[1].ToString());
	return true;
}

void UArenaRules_DOM::GetNavHints(TArray<FVector>& OutPoints) const
{
	for (const AArenaDOM_ControlPoint* P : Points)
	{
		if (P)
		{
			OutPoints.Add(P->GetFloorLocation() + FVector(0.f, 0.f, 10.f));
		}
	}
}

void UArenaRules_DOM::OnStartPlay()
{
	EnsureReachable();
}

void UArenaRules_DOM::EnsureReachable()
{
	AArenaNavGraph* Nav = GetNavGraph();
	AArenaLevelBuilder* Builder = GetLevelBuilder();
	if (!Nav || !Builder || Nav->NumNodes() == 0)
	{
		return;
	}
	const TArray<FArenaNavNode>& Nodes = Nav->GetNodes();
	// Nodes reachable from every team's spawn area (flood fill over the graph links)
	TArray<int32> ReachCount;
	ReachCount.Init(0, Nodes.Num());
	int32 NumOrigins = 0;
	for (int32 Team = 0; Team < 3; ++Team)
	{
		// Team 2 = any start, only when the level has no team starts
		if (Team == 2 && NumOrigins > 0)
		{
			break;
		}
		const APlayerStart* Origin = nullptr;
		for (const APlayerStart* S : Builder->GetPlayerStarts())
		{
			if (S && (ArenaDOM::StartTeam(S) == Team || Team == 2))
			{
				Origin = S;
				break;
			}
		}
		const int32 StartNode = Origin ? Nav->FindNearestNode(Origin->GetActorLocation(), false) : INDEX_NONE;
		if (StartNode == INDEX_NONE)
		{
			continue;
		}
		NumOrigins++;
		TArray<bool> Seen;
		Seen.Init(false, Nodes.Num());
		TArray<int32> Queue = { StartNode };
		Seen[StartNode] = true;
		for (int32 Head = 0; Head < Queue.Num(); ++Head)
		{
			ReachCount[Queue[Head]]++;
			for (const FArenaNavLink& L : Nodes[Queue[Head]].Links)
			{
				if (Nodes.IsValidIndex(L.To) && !Seen[L.To])
				{
					Seen[L.To] = true;
					Queue.Add(L.To);
				}
			}
		}
	}
	if (NumOrigins == 0)
	{
		return;
	}
	const float Lift = ArenaTuning::CapsuleHalfHeight;
	for (int32 i = 0; i < 2; ++i)
	{
		AArenaDOM_ControlPoint* P = Points[i];
		if (!P)
		{
			continue;
		}
		const FVector Orig = P->GetFloorLocation();
		const int32 Near = Nav->FindNearestNode(Orig + FVector(0.f, 0.f, Lift), false);
		UE_LOG(LogOpenUT2k4, Log, TEXT("DOM: point %s nav check: node %d reached from %d/%d spawns, %.0f away"), *P->Label, Near,
			Near != INDEX_NONE ? ReachCount[Near] : -1, NumOrigins, Near != INDEX_NONE ? FVector::Dist(Nodes[Near].Location, Orig + FVector(0.f, 0.f, Lift)) : -1.f);
		if (Near != INDEX_NONE && ReachCount[Near] == NumOrigins && FVector::Dist(Nodes[Near].Location, Orig + FVector(0.f, 0.f, Lift)) < 300.f)
		{
			continue; // fine as it is
		}
		const AArenaDOM_ControlPoint* Other = Points[1 - i];
		int32 Best = INDEX_NONE;
		float BestDist = BIG_NUMBER;
		for (int32 n = 0; n < Nodes.Num(); ++n)
		{
			if (ReachCount[n] != NumOrigins || (Other && FVector::Dist(Nodes[n].Location, Other->GetFloorLocation()) < 2000.f))
			{
				continue;
			}
			const float D = FVector::Dist(Nodes[n].Location, Orig);
			if (D < BestDist)
			{
				BestDist = D;
				Best = n;
			}
		}
		if (Best != INDEX_NONE)
		{
			FVector Floor = Nodes[Best].Location - FVector(0.f, 0.f, Lift);
			Builder->TraceFloor(Nodes[Best].Location, 400.f, Floor);
			P->SetActorLocation(Floor);
			UE_LOG(LogOpenUT2k4, Log, TEXT("DOM: point %s at %s is not reachable for bots, moved to %s (%.0f away)"), *P->Label, *Orig.ToString(), *Floor.ToString(), BestDist);
		}
	}
}

void UArenaRules_DOM::OnMatchStart()
{
	for (AArenaDOM_ControlPoint* P : Points)
	{
		if (P)
		{
			P->SetEnabled();
			P->SetOwnerTeam(-1, nullptr);
			P->ScoreTime = 0.f;
		}
	}
	HoldTeam = -1;
	bLocked = false;
}

// ---------------------------------------------------------------------------------------------
// Scoring
// ---------------------------------------------------------------------------------------------

void UArenaRules_DOM::ProcessTouches()
{
	for (AArenaDOM_ControlPoint* P : Points)
	{
		if (!P || !P->bEnabled)
		{
			continue;
		}
		AArenaCharacter* Touching[2];
		P->GetTouching(Touching);
		// Claim when exactly one team stands on it (both teams on it: contested, nothing changes)
		for (int32 Team = 0; Team < 2; ++Team)
		{
			if (Touching[Team] && !Touching[1 - Team] && P->OwnerTeam != Team)
			{
				AController* By = Touching[Team]->GetController();
				P->SetOwnerTeam(Team, By);
				if (AArenaPlayerState* PS = By ? By->GetPlayerState<AArenaPlayerState>() : nullptr)
				{
					PS->AddScore(1.f);
				}
				UE_LOG(LogOpenUT2k4, Log, TEXT("DOM: %s claims point %s (%s)"), *ArenaTeam::Name(Team), *P->Label, *ArenaDOM::NameOf(By));
				GameMode->BroadcastCenterMessage(FString::Printf(TEXT("%s takes point %s"), *ArenaDOM::NameOf(By), *P->Label), ArenaTeam::Color(Team), 1.5f, TEXT("S_Flag_Taken"));
				for (AController* Bot : GameMode->GetBots())
				{
					if (AArenaBotController* B = Cast<AArenaBotController>(Bot))
					{
						B->ForceGoalUpdate();
					}
				}
			}
		}
	}
}

void UArenaRules_DOM::Tick(float DeltaSeconds)
{
	const AArenaGameState* GS = GetGameState();
	if (!GS || !GS->IsPlaying() || !GameMode || !Points[0] || !Points[1])
	{
		return;
	}
	const float Now = ServerTime();

	if (bLocked)
	{
		if (Now >= Points[0]->EnableTime)
		{
			bLocked = false;
			Points[0]->SetEnabled();
			Points[1]->SetEnabled();
			UE_LOG(LogOpenUT2k4, Log, TEXT("DOM: points are live again"));
			GameMode->BroadcastCenterMessage(TEXT("Control points are live!"), FLinearColor(0.9f, 0.9f, 0.8f), 1.5f, TEXT("S_Pickup_Power"));
			for (AController* Bot : GameMode->GetBots())
			{
				if (AArenaBotController* B = Cast<AArenaBotController>(Bot))
				{
					B->ForceGoalUpdate();
				}
			}
		}
		return;
	}

	ProcessTouches();

	const int32 OwnerA = Points[0]->OwnerTeam;
	const int32 OwnerB = Points[1]->OwnerTeam;
	if (OwnerA >= 0 && OwnerA == OwnerB)
	{
		if (HoldTeam != OwnerA)
		{
			HoldTeam = OwnerA;
			HoldStart = Now;
			Points[0]->ScoreTime = Points[1]->ScoreTime = Now + HoldTime;
			UE_LOG(LogOpenUT2k4, Log, TEXT("DOM: %s holds both points"), *ArenaTeam::Name(HoldTeam));
			GameMode->BroadcastCenterMessage(FString::Printf(TEXT("%s controls both points!"), *ArenaTeam::Name(HoldTeam).ToUpper()), ArenaTeam::Color(HoldTeam), 2.f, TEXT("S_Countdown"));
		}
		else if (Now - HoldStart >= HoldTime)
		{
			ScorePoint(HoldTeam);
		}
	}
	else if (HoldTeam >= 0)
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("DOM: %s lost the double hold after %.1f s"), *ArenaTeam::Name(HoldTeam), Now - HoldStart);
		HoldTeam = -1;
		Points[0]->ScoreTime = Points[1]->ScoreTime = 0.f;
	}
}

void UArenaRules_DOM::ScorePoint(int32 Team)
{
	const float Now = ServerTime();
	GameMode->AddTeamScore(Team, 1);
	for (AArenaDOM_ControlPoint* P : Points)
	{
		if (AArenaPlayerState* PS = P && P->LastToucher.IsValid() ? P->LastToucher->GetPlayerState<AArenaPlayerState>() : nullptr)
		{
			PS->ObjectiveScore++;
			PS->Captures++;
			PS->AddScore(3.f);
		}
		if (P)
		{
			P->SetDisabled(Now + LockTime);
		}
	}
	HoldTeam = -1;
	bLocked = true;
	const AArenaGameState* GS = GetGameState();
	UE_LOG(LogOpenUT2k4, Log, TEXT("DOM: %s scored - Red %d / Blue %d"), *ArenaTeam::Name(Team), GS ? GS->TeamScores[0] : 0, GS ? GS->TeamScores[1] : 0);
	GameMode->BroadcastCenterMessage(FString::Printf(TEXT("%s SCORES! Points locked for %d seconds"), *ArenaTeam::Name(Team).ToUpper(), int32(LockTime)),
		ArenaTeam::Color(Team), 3.f, TEXT("S_Flag_Captured"));
	GameMode->CheckEndConditions();
}

// ---------------------------------------------------------------------------------------------
// Bots
// ---------------------------------------------------------------------------------------------

void UArenaRules_DOM::GetBotObjectives(AArenaBotController* Bot, TArray<FArenaBotObjective>& OutGoals)
{
	AArenaCharacter* P = Bot ? Bot->GetArenaPawn() : nullptr;
	const int32 MyTeam = GetTeamOf(Bot);
	if (!P || !GameMode || (MyTeam != 0 && MyTeam != 1) || !Points[0] || !Points[1])
	{
		return;
	}
	// Split each team's bots between A and B by their order in the bot list
	int32 Index = 0;
	for (AController* Other : GameMode->GetBots())
	{
		if (Other == Bot)
		{
			break;
		}
		if (GetTeamOf(Other) == MyTeam)
		{
			Index++;
		}
	}
	const int32 Preferred = Index % 2;
	const bool bDefender = Bot->IsDefender();
	const int32 EnemyTeam = 1 - MyTeam;
	const bool bEnemyHoldsBoth = Points[0]->OwnerTeam == EnemyTeam && Points[1]->OwnerTeam == EnemyTeam;
	const FVector MyLoc = P->GetActorLocation();

	for (int32 i = 0; i < 2; ++i)
	{
		AArenaDOM_ControlPoint* Pt = Points[i];
		const bool bMine = i == Preferred;
		const FVector Loc = Pt->GetFloorLocation();
		const float Dist = FVector::Dist(MyLoc, Loc);
		if (!Pt->bEnabled)
		{
			// Locked after a score: be there when it opens
			FArenaBotObjective O(Loc, Pt, bMine ? 0.8f : 0.25f, EArenaBotAction::Defend, Tag_Hold);
			O.bCompleteOnReach = false;
			OutGoals.Add(O);
		}
		else if (Pt->OwnerTeam != MyTeam)
		{
			float Priority = bMine ? 2.2f : 1.2f;
			if (bEnemyHoldsBoth)
			{
				Priority += 1.5f; // they are about to score
			}
			if (Dist < 1500.f)
			{
				Priority += 0.5f;
			}
			FArenaBotObjective O(Loc, Pt, Priority, EArenaBotAction::Collect, Tag_Capture);
			O.bCompleteOnReach = false;
			OutGoals.Add(O);
		}
		else
		{
			// Ours: guard it from a spot nearby
			FVector Spot = Loc;
			const FArenaBotObjective* Current = Bot->GetCurrentObjective();
			if (Current && Current->Tag == Tag_Hold && Current->Actor.Get() == Pt && !Bot->HasReachedGoal())
			{
				Spot = Bot->GetGoalLocation();
			}
			else if (AArenaNavGraph* Nav = Bot->GetNavGraph(); Nav && Nav->NumNodes() > 0)
			{
				FRandomStream& Rng = Bot->GetRandomStream();
				const TArray<FArenaNavNode>& Nodes = Nav->GetNodes();
				for (int32 Try = 0; Try < 16; ++Try)
				{
					const FVector& L = Nodes[Rng.RandHelper(Nodes.Num())].Location;
					if (FVector::Dist(L, Loc) < 700.f)
					{
						Spot = L;
						break;
					}
				}
			}
			const float Priority = bMine ? (bDefender ? 1.2f : 0.85f) : 0.3f;
			FArenaBotObjective O(Spot, Pt, Priority, EArenaBotAction::Defend, Tag_Hold);
			OutGoals.Add(O);
		}
	}
}

bool UArenaRules_DOM::UpdateBotObjective(AArenaBotController* Bot, FArenaBotObjective& Objective)
{
	const AArenaDOM_ControlPoint* Pt = Cast<AArenaDOM_ControlPoint>(Objective.Actor.Get());
	if (!Pt)
	{
		return false;
	}
	const int32 MyTeam = GetTeamOf(Bot);
	switch (Objective.Tag)
	{
	case Tag_Capture:
		return Pt->bEnabled && Pt->OwnerTeam != MyTeam;
	case Tag_Hold:
		// A locked point opened, or the point we guard was taken: re-think (capture it)
		if (!Pt->bEnabled)
		{
			return true;
		}
		return Pt->OwnerTeam == MyTeam;
	default:
		return true;
	}
}

float UArenaRules_DOM::GetBotEnemyWeight(AArenaBotController* Bot, AArenaCharacter* Candidate) const
{
	if (!Candidate)
	{
		return 1.f;
	}
	// Enemies standing on / next to a point matter most
	for (const AArenaDOM_ControlPoint* P : Points)
	{
		if (P && FVector::Dist(Candidate->GetActorLocation(), P->GetFloorLocation()) < 700.f)
		{
			return 0.6f;
		}
	}
	return 1.f;
}

// ---------------------------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------------------------

bool UArenaRules_DOM::DrawScoreInfo(AArenaHUD* HUD, UCanvas* Canvas)
{
	if (!HUD || !HUD->IsUTHud())
	{
		return false;
	}
	// HudCDoubleDomination.UpdateTeamHud: 3 disabled, 0 neutral, 1 the viewer's team (red for spectators), 2 the other
	const APlayerController* PC = HUD->GetOwningPlayerController();
	const AArenaPlayerState* MyPS = PC ? PC->GetPlayerState<AArenaPlayerState>() : nullptr;
	const int32 MyTeam = (MyPS && (MyPS->Team == 0 || MyPS->Team == 1)) ? MyPS->Team : 0;
	uint8 State[2] = { 0, 0 };
	FVector Loc[2];
	const FVector* LocPtr[2] = { nullptr, nullptr };
	for (int32 i = 0; i < 2; ++i)
	{
		if (const AArenaDOM_ControlPoint* P = GetPoint(i))
		{
			State[i] = !P->bEnabled ? 3 : ((P->OwnerTeam == 0 || P->OwnerTeam == 1) ? (P->OwnerTeam == MyTeam ? 1 : 2) : 0);
			Loc[i] = P->GetActorLocation();
			LocPtr[i] = &Loc[i];
		}
	}
	HUD->DrawUTDoubleDomination(State, LocPtr);
	return true;
}

void UArenaRules_DOM::DrawHUD(AArenaHUD* HUD, UCanvas* Canvas)
{
	Super::DrawHUD(HUD, Canvas);
	const AArenaGameState* GS = GetGameState();
	if (!HUD || !GS || !Canvas || HUD->IsUTHud())
	{
		return;
	}
	using namespace ArenaHudStyle;
	const FArenaDraw& D = HUD->GetDraw();
	const float Now = GS->GetServerWorldTimeSeconds();
	const float Right = D.W() - D.S(24.f);
	const float BoxW = D.S(60.f), BoxH = D.S(46.f), Gap = D.S(8.f);
	float Y = HUD->GetScoreInfoBottom() + D.S(6.f);

	AArenaDOM_ControlPoint* Pts[2] = { GetPoint(0), GetPoint(1) };
	for (int32 i = 0; i < 2; ++i)
	{
		const AArenaDOM_ControlPoint* P = Pts[i];
		const float X = Right - (2 - i) * BoxW - (1 - i) * Gap;
		FLinearColor Col = FLinearColor(0.85f, 0.85f, 0.8f);
		FString Sub = TEXT("FREE");
		if (P && !P->bEnabled)
		{
			Col = FLinearColor(0.35f, 0.35f, 0.38f);
			Sub = TEXT("LOCKED");
		}
		else if (P && (P->OwnerTeam == 0 || P->OwnerTeam == 1))
		{
			Col = TeamTint(P->OwnerTeam);
			Sub = ArenaTeam::Name(P->OwnerTeam).ToUpper();
		}
		if (ArenaArtHUD::HasHudArt())
		{
			// point box: owner-tinted HUD box, spinning ring and the point letter
			using namespace ArenaArtHUD;
			const bool bOwned = P && P->bEnabled && (P->OwnerTeam == 0 || P->OwnerTeam == 1);
			DrawHudPanel(D, X, Y, BoxW, BoxH, bOwned ? Col : FLinearColor::Transparent);
			const float LCX = X + BoxW * 0.5f, LCY = Y + D.S(17.f);
			DrawSpriteCentered(D, Sprites::DomRing(), LCX, LCY, D.S(30.f), WithAlpha(Col, 0.8f), true, bOwned ? FMath::Fmod(Now * 90.f, 360.f) : 0.f);
			DrawSpriteCentered(D, i == 0 ? Sprites::LetterA() : Sprites::LetterB(), LCX, LCY, D.S(17.f), bOwned ? FMath::Lerp(Col, FLinearColor::White, 0.45f) : Col);
			D.Text(Sub, LCX, Y + D.S(31.f), WithAlpha(TextWhite(), 0.85f), D.S(11.f), EArenaAlign::Center);
			continue;
		}
		D.Rect(X, Y, BoxW, BoxH, FLinearColor(Col.R * 0.25f, Col.G * 0.25f, Col.B * 0.25f, 0.75f));
		D.Rect(X, Y, BoxW, D.S(4.f), Col);
		D.Outline(X, Y, BoxW, BoxH, WithAlpha(Col, 0.7f), D.S(1.5f));
		D.Text(ArenaDOM::PointLabels[i], X + BoxW * 0.5f, Y + D.S(5.f), TextWhite(), D.S(22.f), EArenaAlign::Center);
		D.Text(Sub, X + BoxW * 0.5f, Y + D.S(29.f), WithAlpha(TextWhite(), 0.8f), D.S(11.f), EArenaAlign::Center);
	}
	Y += BoxH + D.S(6.f);

	// Countdown line: holding both / locked after a score
	const AArenaDOM_ControlPoint* A = Pts[0];
	const AArenaDOM_ControlPoint* B = Pts[1];
	FString Big;
	FLinearColor BigCol = TextWhite();
	if (A && !A->bEnabled && A->EnableTime > 0.f)
	{
		const int32 Secs = FMath::Max(0, FMath::CeilToInt(A->EnableTime - Now));
		D.Text(FString::Printf(TEXT("POINTS LOCKED  %d"), Secs), Right, Y, TextDim(), D.S(16.f), EArenaAlign::Right);
		Big = FString::Printf(TEXT("POINTS REOPEN IN %d"), Secs);
	}
	else if (A && B && A->ScoreTime > 0.f && A->OwnerTeam >= 0 && A->OwnerTeam == B->OwnerTeam)
	{
		const float Left = FMath::Max(0.f, A->ScoreTime - Now);
		const int32 Team = A->OwnerTeam;
		D.Text(FString::Printf(TEXT("%s SCORES IN %d"), *ArenaTeam::Name(Team).ToUpper(), FMath::CeilToInt(Left)), Right, Y, TeamTextColor(Team), D.S(16.f), EArenaAlign::Right);
		D.Bar(Right - BoxW * 2.f - Gap, Y + D.S(22.f), BoxW * 2.f + Gap, D.S(5.f), 1.f - Left / HoldTime, TeamTint(Team));
		Big = FString::Printf(TEXT("%s SCORES IN %d"), *ArenaTeam::Name(Team).ToUpper(), FMath::CeilToInt(Left));
		BigCol = TeamTextColor(Team);
	}
	if (!Big.IsEmpty() && GS->IsPlaying())
	{
		D.Text(Big, D.W() * 0.5f, D.S(100.f), BigCol, D.S(26.f), EArenaAlign::Center);
	}

	// World markers for both points
	if (GS->IsPlaying() || GS->Phase == EArenaMatchPhase::Countdown)
	{
		for (int32 i = 0; i < 2; ++i)
		{
			if (const AArenaDOM_ControlPoint* P = Pts[i])
			{
				const FLinearColor Col = !P->bEnabled ? FLinearColor(0.5f, 0.5f, 0.55f) : ((P->OwnerTeam == 0 || P->OwnerTeam == 1) ? TeamTextColor(P->OwnerTeam) : FLinearColor(0.95f, 0.95f, 0.9f));
				ArenaDOM::DrawMarker(HUD, Canvas, P->GetFloorLocation() + FVector(0.f, 0.f, 430.f), ArenaDOM::PointLabels[i], Col);
			}
		}
	}
}
