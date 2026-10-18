#include "ArenaRules_BR.h"
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
#include "ArenaBR_Ball.h"
#include "ArenaBR_Goal.h"
#include "ArenaBR_BallLauncher.h"
#include "ArenaJumpPad.h"
#include "Engine/World.h"
#include "Engine/Canvas.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/PlayerController.h"
#include "Dom/JsonObject.h"

ARENA_REGISTER_RULES(UArenaRules_BR, "BR", "Bombing Run");

namespace ArenaBR
{
	constexpr int32 CarryPoints = 7;
	constexpr int32 ThrowPoints = 3;
	constexpr float HoopLift = 240.f; // hoop centre above the floor when only a floor point is known
	constexpr float DiveMaxDist = 1300.f; // bot jump-in spots: this close to the ring (2D)

	static FString NameOf(const AController* C)
	{
		return (C && C->PlayerState) ? C->PlayerState->GetPlayerName() : FString(TEXT("Someone"));
	}

	/** Team side of a player start (tagged "Red" / "Blue" by the level builder), -1 if neutral. */
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

	/** Team of a raw marker with tolerant field names (-1 if unknown). */
	static int32 MarkerTeam(const FArenaRawMarker& M)
	{
		int32 Team = M.GetTeam();
		if (Team == 0 || Team == 1)
		{
			return Team;
		}
		if (M.Json.IsValid())
		{
			static const TCHAR* const Keys[] = { TEXT("team_index"), TEXT("defender_team"), TEXT("Team"), TEXT("team_num"), TEXT("TeamNum"), TEXT("DefenderTeamIndex") };
			for (const TCHAR* Key : Keys)
			{
				double V = 0.0;
				if (M.Json->TryGetNumberField(Key, V) && (int32(V) == 0 || int32(V) == 1))
				{
					return int32(V);
				}
			}
		}
		return -1;
	}

	static TArray<FArenaRawMarker> GetMarkers(const AArenaLevelBuilder* Builder, std::initializer_list<const TCHAR*> Lists)
	{
		TArray<FArenaRawMarker> Out;
		for (const TCHAR* L : Lists)
		{
			Out.Append(Builder->GetRawMarkers(L));
		}
		return Out;
	}

	/** Floor point under a location on an imported map (or the location itself). */
	static FVector FloorUnder(const AArenaLevelBuilder* Builder, const FVector& Loc, float MaxDrop, bool* bFound = nullptr)
	{
		FVector Floor;
		const bool bOk = Builder->TraceFloor(Loc, MaxDrop, Floor);
		if (bFound)
		{
			*bFound = bOk;
		}
		return bOk ? Floor : Loc;
	}

	/** Is a floor point open enough for a standing character? */
	static bool IsStandable(const UWorld* World, const FVector& Floor)
	{
		const FCollisionShape Capsule = FCollisionShape::MakeCapsule(ArenaTuning::CapsuleRadius, ArenaTuning::CapsuleHalfHeight);
		FCollisionQueryParams Query(SCENE_QUERY_STAT(ArenaBRStandable), false);
		return !World->OverlapBlockingTestByChannel(Floor + FVector(0.f, 0.f, ArenaTuning::CapsuleHalfHeight + 8.f), FQuat::Identity, ECC_Pawn, Capsule, Query);
	}

	/** Screen-space marker for a world location, clamped to the screen edges when off screen. */
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
		FVector2D P;
		bool bClamped = false;
		if (bInFront)
		{
			const FVector S = Canvas->Project(WorldLoc);
			P = FVector2D(S.X, S.Y);
		}
		if (!bInFront || P.X < Margin || P.X > D.W() - Margin || P.Y < Margin || P.Y > D.H() - Margin)
		{
			const FVector Local = ViewRot.UnrotateVector(Dir);
			FVector2D V(Local.Y, -Local.Z);
			if (!bInFront && V.IsNearlyZero())
			{
				V = FVector2D(0.f, 1.f);
			}
			V.Normalize();
			const FVector2D C(D.W() * 0.5f, D.H() * 0.5f);
			const float SX = V.X != 0.f ? (D.W() * 0.5f - Margin) / FMath::Abs(V.X) : 1e6f;
			const float SY = V.Y != 0.f ? (D.H() * 0.5f - Margin) / FMath::Abs(V.Y) : 1e6f;
			P = C + V * FMath::Min(SX, SY);
			bClamped = true;
		}
		const float R = D.S(bClamped ? 9.f : 11.f);
		D.Line(P.X, P.Y - R, P.X + R, P.Y, Color, D.S(2.5f));
		D.Line(P.X + R, P.Y, P.X, P.Y + R, Color, D.S(2.5f));
		D.Line(P.X, P.Y + R, P.X - R, P.Y, Color, D.S(2.5f));
		D.Line(P.X - R, P.Y, P.X, P.Y - R, Color, D.S(2.5f));
		D.Rect(P.X - R * 0.35f, P.Y - R * 0.35f, R * 0.7f, R * 0.7f, Color);
		const FString Text = FString::Printf(TEXT("%s  %dm"), *Label, FMath::RoundToInt(Dist / 100.f));
		D.Text(Text, P.X, P.Y + R + D.S(3.f), Color, D.S(14.f), EArenaAlign::Center);
	}
}

bool UArenaRules_BR::SupportsLayout(const FArenaLayoutInfo& Layout) const
{
	return Layout.HasFeature(TEXT("raw:bomb_spawns")) || Layout.HasFeature(TEXT("raw:bomb_deliveries"))
		|| Layout.GameTypePrefix == TEXT("BR");
}

FString UArenaRules_BR::GetGoalText(int32 Goal) const
{
	return Goal > 0 ? FString::Printf(TEXT("First to %d points"), Goal) : FString();
}

AArenaBR_Ball* UArenaRules_BR::GetBall() const
{
	if (Ball)
	{
		return Ball;
	}
	// HUD on a client: find the local actor
	if (const UWorld* World = GetWorld())
	{
		TActorIterator<AArenaBR_Ball> It(World);
		if (It)
		{
			return *It;
		}
	}
	return nullptr;
}

AArenaBR_Goal* UArenaRules_BR::GetGoal(int32 Team) const
{
	if (Team != 0 && Team != 1)
	{
		return nullptr;
	}
	if (Goals[Team])
	{
		return Goals[Team];
	}
	if (const UWorld* World = GetWorld())
	{
		for (TActorIterator<AArenaBR_Goal> It(World); It; ++It)
		{
			if (It->GetTeam() == Team)
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

bool UArenaRules_BR::FindPositions(AArenaLevelBuilder* Builder, FVector& OutCenter, FVector OutFloor[2], FVector OutHoop[2]) const
{
	using namespace ArenaBR;
	const UWorld* World = GetWorld();
	bool bHaveGoal[2] = { false, false };
	bool bHaveCenter = false;

	// 1. Raw markers (field names are not final: tolerant parsing)
	if (Builder->IsImportedMap())
	{
		TArray<FArenaRawMarker> Unassigned;
		for (const FArenaRawMarker& M : GetMarkers(Builder, { TEXT("bomb_deliveries"), TEXT("bomb_goals"), TEXT("ball_goals") }))
		{
			const int32 Team = MarkerTeam(M);
			if ((Team == 0 || Team == 1) && !bHaveGoal[Team])
			{
				OutHoop[Team] = M.Location;
				OutFloor[Team] = FloorUnder(Builder, M.Location, 3000.f);
				bHaveGoal[Team] = true;
			}
			else if (Team < 0)
			{
				Unassigned.Add(M);
			}
		}
		// Goals without a team: give them to the nearer team's player starts
		for (const FArenaRawMarker& M : Unassigned)
		{
			float Best[2] = { BIG_NUMBER, BIG_NUMBER };
			for (const APlayerStart* S : Builder->GetPlayerStarts())
			{
				const int32 T = StartTeam(S);
				if (T >= 0)
				{
					Best[T] = FMath::Min(Best[T], FVector::Dist(S->GetActorLocation(), M.Location));
				}
			}
			const int32 T = Best[0] <= Best[1] ? 0 : 1;
			const int32 Pick = !bHaveGoal[T] ? T : (!bHaveGoal[1 - T] ? 1 - T : -1);
			if (Pick >= 0)
			{
				OutHoop[Pick] = M.Location;
				OutFloor[Pick] = FloorUnder(Builder, M.Location, 3000.f);
				bHaveGoal[Pick] = true;
			}
		}
		const TArray<FArenaRawMarker> Spawns = GetMarkers(Builder, { TEXT("bomb_spawns"), TEXT("ball_spawns") });
		if (Spawns.Num() > 0)
		{
			OutCenter = FloorUnder(Builder, Spawns[0].Location, 3000.f);
			bHaveCenter = true;
		}
	}

	// 2. Anchors the builder mapped from markers
	for (int32 Team = 0; Team < 2; ++Team)
	{
		FTransform T;
		if (!bHaveGoal[Team] && Builder->GetAnchorPoint(FName(*FString::Printf(TEXT("TeamBase%d"), Team)), T))
		{
			OutFloor[Team] = T.GetLocation();
			OutHoop[Team] = OutFloor[Team] + FVector(0.f, 0.f, HoopLift);
			FTransform G;
			if (Builder->GetAnchorPoint(FName(*FString::Printf(TEXT("Goal%d"), Team)), G))
			{
				OutHoop[Team] = G.GetLocation();
			}
			bHaveGoal[Team] = true;
		}
	}
	FTransform C;
	if (!bHaveCenter && Builder->GetAnchorPoint(TEXT("Center"), C))
	{
		OutCenter = C.GetLocation();
		bHaveCenter = true;
	}

	// 3. Fallback: the team spawn areas (maps without BR markers)
	if (!bHaveGoal[0] || !bHaveGoal[1])
	{
		FVector Sum[2] = { FVector::ZeroVector, FVector::ZeroVector };
		int32 Num[2] = { 0, 0 };
		for (const APlayerStart* S : Builder->GetPlayerStarts())
		{
			const int32 T = StartTeam(S);
			if (T >= 0)
			{
				Sum[T] += S->GetActorLocation();
				Num[T]++;
			}
		}
		for (int32 Team = 0; Team < 2; ++Team)
		{
			if (bHaveGoal[Team] || Num[Team] == 0)
			{
				continue;
			}
			const FVector Centroid = Sum[Team] / Num[Team];
			bool bFloor = false;
			FVector Floor = FloorUnder(Builder, Centroid + FVector(0.f, 0.f, 200.f), 1200.f, &bFloor);
			if (!bFloor || !IsStandable(World, Floor))
			{
				// Nearest start to the centroid
				float BestDist = BIG_NUMBER;
				for (const APlayerStart* S : Builder->GetPlayerStarts())
				{
					const int32 T = StartTeam(S);
					if (T == Team && FVector::Dist(S->GetActorLocation(), Centroid) < BestDist)
					{
						BestDist = FVector::Dist(S->GetActorLocation(), Centroid);
						Floor = FloorUnder(Builder, S->GetActorLocation(), 400.f);
					}
				}
			}
			OutFloor[Team] = Floor;
			OutHoop[Team] = Floor + FVector(0.f, 0.f, HoopLift);
			bHaveGoal[Team] = true;
			UE_LOG(LogOpenUT2k4, Log, TEXT("BR: no goal marker for %s, using its spawn area %s"), *ArenaTeam::Name(Team), *Floor.ToString());
		}
	}
	if (!bHaveGoal[0] || !bHaveGoal[1])
	{
		return false;
	}
	if (!bHaveCenter)
	{
		const FVector Mid = (OutFloor[0] + OutFloor[1]) * 0.5f;
		bool bFloor = false;
		FVector Floor = FloorUnder(Builder, Mid + FVector(0.f, 0.f, 300.f), 2000.f, &bFloor);
		if (!bFloor || !IsStandable(World, Floor))
		{
			// Closest pickup (always on a walkable floor) to the midpoint, else the closest start
			float BestDist = BIG_NUMBER;
			for (const AArenaPickup* P : Builder->GetPickups())
			{
				if (P && FVector::Dist(P->GetActorLocation(), Mid) < BestDist)
				{
					BestDist = FVector::Dist(P->GetActorLocation(), Mid);
					Floor = FloorUnder(Builder, P->GetActorLocation(), 400.f);
				}
			}
			if (BestDist == BIG_NUMBER)
			{
				for (const APlayerStart* S : Builder->GetPlayerStarts())
				{
					if (S && FVector::Dist(S->GetActorLocation(), Mid) < BestDist)
					{
						BestDist = FVector::Dist(S->GetActorLocation(), Mid);
						Floor = FloorUnder(Builder, S->GetActorLocation(), 400.f);
					}
				}
			}
		}
		OutCenter = Floor;
		bHaveCenter = true;
		UE_LOG(LogOpenUT2k4, Log, TEXT("BR: no ball spawn marker, using %s"), *OutCenter.ToString());
	}
	// Keep hoops at a sensible height above their floor
	for (int32 Team = 0; Team < 2; ++Team)
	{
		if (OutHoop[Team].Z < OutFloor[Team].Z + HoopLift)
		{
			OutHoop[Team].Z = OutFloor[Team].Z + HoopLift;
		}
	}
	return true;
}

bool UArenaRules_BR::OnLevelBuilt(AArenaLevelBuilder* Builder)
{
	UWorld* World = GetWorld();
	if (!Builder || !World)
	{
		return false;
	}
	FVector Center = FVector::ZeroVector;
	FVector Floor[2], Hoop[2];
	if (!FindPositions(Builder, Center, Floor, Hoop))
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("BR: %s has no usable goal positions, playing Team Deathmatch instead"), *Builder->GetBuiltLayoutId());
		return false;
	}

	for (int32 Team = 0; Team < 2; ++Team)
	{
		const float Yaw = (Center - Floor[Team]).Rotation().Yaw;
		const FTransform T(FRotator(0.f, Yaw, 0.f), Floor[Team]);
		AArenaBR_Goal* Goal = World->SpawnActorDeferred<AArenaBR_Goal>(AArenaBR_Goal::StaticClass(), T, Builder, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Goal)
		{
			Goal->Setup(Team, Floor[Team], Hoop[Team], Yaw);
			Goal->FinishSpawning(T);
			Goals[Team] = Goal;
		}
	}
	const FTransform BT(FRotator::ZeroRotator, Center);
	Ball = World->SpawnActorDeferred<AArenaBR_Ball>(AArenaBR_Ball::StaticClass(), BT, Builder, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Ball)
	{
		Ball->FinishSpawning(BT);
	}
	if (!Ball || !Goals[0] || !Goals[1])
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("BR: failed to spawn the ball / goals"));
		return false;
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("BR: ball at %s, goals Red %s (hoop %s) / Blue %s (hoop %s)"), *Center.ToString(),
		*Floor[0].ToString(), *Hoop[0].ToString(), *Floor[1].ToString(), *Hoop[1].ToString());
	return true;
}

void UArenaRules_BR::GetNavHints(TArray<FVector>& OutPoints) const
{
	if (Ball)
	{
		OutPoints.Add(Ball->GetHomeLocation() + FVector(0.f, 0.f, 10.f));
	}
	for (const AArenaBR_Goal* Goal : Goals)
	{
		if (Goal)
		{
			OutPoints.Add(Goal->GetFloorLocation() + FVector(0.f, 0.f, 10.f));
		}
	}
}

void UArenaRules_BR::OnMatchStart()
{
	if (Ball)
	{
		Ball->SendHome();
		Ball->ClearThrowInfo();
	}
	// -BotDebug=<sec>: can bots path between the ball and both goals?
	AArenaNavGraph* Nav = GameMode ? GameMode->GetNavGraph() : nullptr;
	if (Nav && Ball && Goals[0] && Goals[1] && FCString::Strifind(FCommandLine::Get(), TEXT("-BotDebug=")))
	{
		UpdateGoalSpots();
		const FVector Pts[3] = { Ball->GetHomeLocation(), ApproachSpot[0], ApproachSpot[1] };
		const TCHAR* Names[3] = { TEXT("ball"), TEXT("red goal"), TEXT("blue goal") };
		for (int32 i = 0; i < 3; ++i)
		{
			for (int32 j = 0; j < 3; ++j)
			{
				TArray<FVector> Path;
				TArray<EArenaNavLinkType> Links;
				if (i != j)
				{
					const bool bOk = Nav->FindPath(Pts[i], Pts[j], Path, Links);
					UE_LOG(LogOpenUT2k4, Log, TEXT("BotDebug: path %s -> %s: %s (%d points)"), Names[i], Names[j], bOk ? TEXT("yes") : TEXT("NO"), Path.Num());
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Scoring
// ---------------------------------------------------------------------------------------------

void UArenaRules_BR::Tick(float DeltaSeconds)
{
	const AArenaGameState* GS = GetGameState();
	if (!GS || !GS->IsPlaying() || !Ball || !GameMode)
	{
		return;
	}
	// Carry-in: the carrier entered the enemy hoop
	AArenaCharacter* Carrier = Ball->GetCarrier();
	if (Ball->IsCarried() && Carrier && Carrier->IsAlive())
	{
		const int32 Team = Carrier->GetTeam();
		const AArenaBR_Goal* EnemyGoal = GetGoal(1 - Team);
		if ((Team == 0 || Team == 1) && EnemyGoal && EnemyGoal->IsCharacterInside(Carrier->GetActorLocation()))
		{
			ScoreGoal(Team, Carrier->GetController(), ArenaBR::CarryPoints, true);
		}
	}
}

void UArenaRules_BR::OnBallFlightSegment(AArenaBR_Ball* InBall, const FVector& From, const FVector& To)
{
	const AArenaGameState* GS = GetGameState();
	if (!GS || !GS->IsPlaying() || !InBall)
	{
		return;
	}
	const int32 Team = InBall->GetLastThrowerTeam();
	const AArenaBR_Goal* EnemyGoal = GetGoal(1 - Team);
	if ((Team == 0 || Team == 1) && EnemyGoal && EnemyGoal->SegmentPassesThrough(From, To))
	{
		ScoreGoal(Team, InBall->GetLastThrower(), ArenaBR::ThrowPoints, false);
	}
}

void UArenaRules_BR::ScoreGoal(int32 ScoringTeam, AController* Scorer, int32 Points, bool bCarried)
{
	const float Now = GetWorld()->GetTimeSeconds();
	if (Now < ScoreLockUntil || !GameMode)
	{
		return;
	}
	ScoreLockUntil = Now + 1.f;
	AArenaCharacter* Carrier = Ball ? Ball->GetCarrier() : nullptr;

	GameMode->AddTeamScore(ScoringTeam, Points);
	if (AArenaPlayerState* PS = Scorer ? Scorer->GetPlayerState<AArenaPlayerState>() : nullptr)
	{
		PS->Captures++;
		PS->ObjectiveScore += Points;
		PS->AddScore(bCarried ? 5.f : 3.f);
	}
	if (AArenaBR_Goal* Goal = GetGoal(1 - ScoringTeam))
	{
		Goal->PlayScoreEffect();
	}
	const AArenaGameState* GS = GetGameState();
	UE_LOG(LogOpenUT2k4, Log, TEXT("BR: %s scored %d (%s) by %s - Red %d / Blue %d"), *ArenaTeam::Name(ScoringTeam), Points,
		bCarried ? TEXT("carried in") : TEXT("thrown through"), *ArenaBR::NameOf(Scorer), GS ? GS->TeamScores[0] : 0, GS ? GS->TeamScores[1] : 0);
	GameMode->BroadcastCenterMessage(bCarried
		? FString::Printf(TEXT("%s dives through the hoop! +%d %s"), *ArenaBR::NameOf(Scorer), Points, *ArenaTeam::Name(ScoringTeam).ToUpper())
		: FString::Printf(TEXT("%s sinks a long shot! +%d %s"), *ArenaBR::NameOf(Scorer), Points, *ArenaTeam::Name(ScoringTeam).ToUpper()),
		ArenaTeam::Color(ScoringTeam), 3.f, TEXT("S_Flag_Captured"));

	if (Ball)
	{
		Ball->SendHome();
		Ball->ClearThrowInfo();
	}
	if (bCarried && Carrier && Carrier->IsAlive())
	{
		// Self-sacrifice: the carrier goes up with the goal (no suicide penalty, see ScoreKill)
		AArenaBR_BallLauncher::RemoveFrom(Carrier);
		SacrificeController = Carrier->GetController();
		ArenaAssets::PlaySoundAt(Carrier, TEXT("S_Explosion"), Carrier->GetActorLocation());
		Carrier->Suicide();
		SacrificeController.Reset();
	}
	GameMode->CheckEndConditions();
}

bool UArenaRules_BR::ScoreKill(AController* Killer, AController* Victim, bool bHeadshot)
{
	if (Victim && SacrificeController.Get() == Victim)
	{
		if (AArenaPlayerState* PS = Victim->GetPlayerState<AArenaPlayerState>())
		{
			PS->Deaths++;
		}
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------------------------
// Ball events
// ---------------------------------------------------------------------------------------------

void UArenaRules_BR::OnBallPickedUp(AArenaBR_Ball* InBall, AArenaCharacter* By)
{
	if (!GameMode || !By)
	{
		return;
	}
	AController* C = By->GetController();
	const int32 Team = By->GetTeam();
	CarrierSince = GetWorld()->GetTimeSeconds();
	CarrierScoringRand = FMath::FRand();
	const bool bPass = InBall && InBall->GetLastThrowerTeam() == Team && InBall->GetCarryState() == EArenaCarryState::Carried && InBall->GetLastThrower() && InBall->GetLastThrower() != C;
	UE_LOG(LogOpenUT2k4, Log, TEXT("BR: ball %s by %s (%s)"), bPass ? TEXT("caught") : TEXT("taken"), *ArenaBR::NameOf(C), *ArenaTeam::Name(Team));
	if (bPass)
	{
		if (AArenaPlayerState* PS = InBall->GetLastThrower()->GetPlayerState<AArenaPlayerState>())
		{
			PS->AddScore(1.f);
		}
		GameMode->BroadcastTeamMessage(Team, FString::Printf(TEXT("%s caught the pass"), *ArenaBR::NameOf(C)), ArenaTeam::Color(Team), 1.5f);
	}
	else
	{
		GameMode->BroadcastCenterMessage(FString::Printf(TEXT("%s has the ball!"), *ArenaBR::NameOf(C)), ArenaTeam::Color(Team), 2.f, TEXT("S_Flag_Taken"));
	}
	if (InBall)
	{
		InBall->ClearThrowInfo();
	}
	AArenaBotController::ReportNoise(By, By->GetActorLocation(), 1.f);
	// Carrier changed: everyone re-thinks
	for (AController* Bot : GameMode->GetBots())
	{
		if (AArenaBotController* B = Cast<AArenaBotController>(Bot))
		{
			B->ForceGoalUpdate();
		}
	}
}

void UArenaRules_BR::OnBallDropped(AArenaBR_Ball* InBall, AController* By, bool bThrown)
{
	if (!GameMode)
	{
		return;
	}
	if (!bThrown)
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("BR: ball dropped (%s)"), *ArenaBR::NameOf(By));
		GameMode->BroadcastCenterMessage(TEXT("The ball is loose!"), FLinearColor(1.f, 0.8f, 0.3f), 1.5f);
	}
	for (AController* Bot : GameMode->GetBots())
	{
		if (AArenaBotController* B = Cast<AArenaBotController>(Bot))
		{
			B->ForceGoalUpdate();
		}
	}
}

void UArenaRules_BR::OnBallReturned(AArenaBR_Ball* InBall)
{
	if (!GameMode)
	{
		return;
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("BR: ball reset to the centre"));
	GameMode->BroadcastCenterMessage(TEXT("The ball is back at the centre"), FLinearColor(1.f, 0.8f, 0.3f), 2.f, TEXT("S_Flag_Returned"));
}

// ---------------------------------------------------------------------------------------------
// Bots (after UT2004's BombingRunSquadAI: attackers go for the ball, the carrier heads for the enemy goal and
// jumps through it, throws from close range or passes to a teammate ahead, defenders guard their goal, the enemy
// carrier is everybody's target; fighting itself is the normal bot combat)
// ---------------------------------------------------------------------------------------------

void UArenaRules_BR::UpdateGoalSpots()
{
	if (bGoalSpotsDone || !GameMode)
	{
		return;
	}
	const AArenaNavGraph* Nav = GameMode->GetNavGraph();
	if (!Nav || Nav->NumNodes() == 0)
	{
		return;
	}
	bGoalSpotsDone = true;
	const TArray<FArenaNavNode>& Nodes = Nav->GetNodes();
	for (int32 Team = 0; Team < 2; ++Team)
	{
		const AArenaBR_Goal* Goal = Goals[Team];
		if (!Goal)
		{
			continue;
		}
		const FVector Hoop = Goal->GetHoopCenter();
		ApproachSpot[Team] = Goal->GetFloorLocation();
		bDiveGoal[Team] = false;
		bPadGoal[Team] = false;
		// A jump pad that throws its rider through the ring (BR-Colossus): carriers just take the pad
		if (const AArenaLevelBuilder* Builder = GameMode->GetLevelBuilder())
		{
			float BestPad = 700.f;
			for (const AArenaJumpPad* Pad : Builder->GetJumpPads())
			{
				const float D = Pad ? FVector::Dist(Pad->GetTarget(), Hoop) : BIG_NUMBER;
				const int32 N = D < BestPad ? Nav->FindNearestNode(Pad->GetActorLocation() + FVector(0.f, 0.f, 90.f), false) : INDEX_NONE;
				if (N != INDEX_NONE && FVector::Dist2D(Nodes[N].Location, Pad->GetActorLocation()) < 150.f)
				{
					BestPad = D;
					ApproachSpot[Team] = Nodes[N].Location;
					bDiveGoal[Team] = true;
					bPadGoal[Team] = true;
				}
			}
		}
		// A node on a floor about level with the ring, near it, with a clear line to it: carriers run and jump in
		// from there (UT2004's xBombDelivery is a JumpSpot over a pit on maps like BR-Colossus).
		float BestScore = bPadGoal[Team] ? -1.f : BIG_NUMBER;
		for (const FArenaNavNode& N : Nodes)
		{
			const float D2 = FVector::Dist2D(N.Location, Hoop);
			const float Dz = N.Location.Z - Hoop.Z;
			if (D2 > ArenaBR::DiveMaxDist || Dz < -280.f || Dz > 120.f)
			{
				continue;
			}
			const float Score = D2 + 2.f * FMath::Abs(Dz + 60.f);
			if (Score >= BestScore)
			{
				continue;
			}
			const FVector Dir = (Hoop - N.Location).GetSafeNormal();
			FHitResult Hit;
			FCollisionQueryParams Query(SCENE_QUERY_STAT(ArenaBRDiveLine), false);
			if (GetWorld()->LineTraceSingleByObjectType(Hit, N.Location, Hoop - Dir * 60.f, FCollisionObjectQueryParams(ECC_WorldStatic), Query))
			{
				continue;
			}
			BestScore = Score;
			ApproachSpot[Team] = N.Location;
			bDiveGoal[Team] = true;
		}
		if (!bDiveGoal[Team] && !Goal->IsElevated())
		{
			// walk-in goal: the floor under the ring
			const int32 N = Nav->FindNearestNode(Goal->GetFloorLocation(), true);
			ApproachSpot[Team] = N != INDEX_NONE ? Nodes[N].Location : Goal->GetFloorLocation();
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("BR: bots score at the %s goal by %s from %s (ring %s)"), *ArenaTeam::Name(Team),
			bPadGoal[Team] ? TEXT("taking the jump pad") : bDiveGoal[Team] ? TEXT("jumping in") : (Goal->IsElevated() ? TEXT("throwing") : TEXT("walking in")), *ApproachSpot[Team].ToCompactString(), *Hoop.ToCompactString());
	}
}

FVector UArenaRules_BR::PickDefendSpot(AArenaBotController* Bot, int32 Team) const
{
	const FVector Base = ApproachSpot[Team];
	AArenaNavGraph* Nav = Bot->GetNavGraph();
	if (!Nav || Nav->NumNodes() == 0)
	{
		return Base;
	}
	FRandomStream& Rng = Bot->GetRandomStream();
	const TArray<FArenaNavNode>& Nodes = Nav->GetNodes();
	for (int32 Try = 0; Try < 64; ++Try)
	{
		const FVector& L = Nodes[Rng.RandHelper(Nodes.Num())].Location;
		const FVector Hoop = Goals[Team] ? Goals[Team]->GetHoopCenter() : Base;
		if (FVector::Dist2D(L, Base) < 1800.f && FMath::Abs(L.Z - Base.Z) < 500.f && FVector::Dist2D(L, Hoop) > 600.f)
		{
			// not on or next to a jump pad (a defender would bounce off it, BR-Colossus' goal pads)
			bool bNearPad = false;
			if (GameMode && GameMode->GetLevelBuilder())
			{
				for (const AArenaJumpPad* Pad : GameMode->GetLevelBuilder()->GetJumpPads())
				{
					bNearPad |= Pad && FVector::Dist2D(Pad->GetTriggerCenter(), L) < Pad->GetTriggerRadius() + 250.f && FMath::Abs(Pad->GetTriggerCenter().Z - L.Z) < 400.f;
				}
			}
			if (!bNearPad)
			{
				return L;
			}
		}
	}
	return Base;
}

FVector UArenaRules_BR::GetEscortSpot(const AArenaCharacter* Carrier) const
{
	// UT2004: teammates head for the carrier's route a few nodes ahead (RouteCache[2..3]), or the carrier itself
	if (const AArenaBotController* CB = Cast<AArenaBotController>(Carrier->GetController()))
	{
		const TArray<FVector>& Route = CB->GetPathPoints();
		if (Route.Num() > 0)
		{
			return Route[FMath::Min(3, Route.Num() - 1)];
		}
	}
	return Carrier->GetActorLocation();
}

void UArenaRules_BR::GetBotObjectives(AArenaBotController* Bot, TArray<FArenaBotObjective>& OutGoals)
{
	AArenaCharacter* P = Bot ? Bot->GetArenaPawn() : nullptr;
	const int32 MyTeam = GetTeamOf(Bot);
	if (!P || !Ball || (MyTeam != 0 && MyTeam != 1) || !Goals[0] || !Goals[1])
	{
		return;
	}
	UpdateGoalSpots();
	AArenaBR_Goal* OwnGoal = Goals[MyTeam];
	AArenaBR_Goal* EnemyGoal = Goals[1 - MyTeam];
	const bool bDefender = Bot->IsDefender();
	const FVector MyLoc = P->GetActorLocation();

	auto Add = [&OutGoals](const FVector& Loc, AActor* Actor, float Priority, int32 Tag, EArenaBotAction Action, bool bCompleteOnReach, bool bTrack)
	{
		FArenaBotObjective O(Loc, Actor, Priority, Action, Tag);
		O.bCompleteOnReach = bCompleteOnReach;
		O.bTrackActor = bTrack;
		OutGoals.Add(O);
	};

	// Carrier: the enemy goal, nothing else matters (OrdersForBombCarrier)
	if (P->GetCarriedObject() == Ball)
	{
		Add(ApproachSpot[1 - MyTeam], EnemyGoal, 100.f, Tag_Deliver, EArenaBotAction::Deliver, false, false);
		return;
	}

	bool bDefend = bDefender;
	AArenaCharacter* Carrier = Ball->IsCarried() ? Ball->GetCarrier() : nullptr;
	if (Carrier && Carrier->GetTeam() == MyTeam)
	{
		// Our carrier: attackers run ahead on its route and fight for it, defenders stay home
		if (!bDefender)
		{
			Add(GetEscortSpot(Carrier), Carrier, 1.5f, Tag_Escort, EArenaBotAction::Escort, false, false);
		}
	}
	else if (Carrier)
	{
		// Enemy carrier: attackers intercept it; defenders when they see it or it nears our goal
		// (seen, heard or told by the squad: the carrier is the known enemy)
		const bool bSee = Bot->GetEnemy() == Carrier;
		const bool bNearHome = FVector::Dist(Carrier->GetActorLocation(), ApproachSpot[MyTeam]) < 8000.f;
		if (!bDefender || bSee || bNearHome)
		{
			Add(Carrier->GetActorLocation(), Carrier, bDefender ? 3.f : 2.5f, Tag_Attack, EArenaBotAction::Attack, false, true);
			bDefend = false;
		}
	}
	else
	{
		// Loose or home ball: attackers go for it; defenders when it is close or in sight (GoPickupBomb)
		const FVector BallLoc = Ball->GetObjectLocation();
		const float Dist = FVector::Dist(MyLoc, BallLoc);
		const bool bSeeBall = Dist < 6000.f && Bot->LineOfSightTo(Ball, P->GetPawnViewLocation());
		if (!bDefender)
		{
			Add(BallLoc, Ball, 2.5f, Tag_Ball, EArenaBotAction::Collect, false, false);
		}
		else if (Dist < 2500.f || (bSeeBall && !Ball->IsHome() && Dist < 4500.f))
		{
			Add(BallLoc, Ball, 2.2f, Tag_Ball, EArenaBotAction::Collect, false, false);
			bDefend = false;
		}
	}

	if (bDefend && OwnGoal)
	{
		FVector Spot;
		const FArenaBotObjective* Current = Bot->GetCurrentObjective();
		if (Current && Current->Tag == Tag_Defend && !Bot->HasReachedGoal())
		{
			Spot = Bot->GetGoalLocation();
		}
		else
		{
			Spot = PickDefendSpot(Bot, MyTeam);
		}
		// below 0.9: a defender that sees an enemy fights it like in DM (approaches, strafes) instead of only passing by
		Add(Spot, OwnGoal, 0.85f, Tag_Defend, EArenaBotAction::Defend, true, false);
	}
}

bool UArenaRules_BR::UpdateBotObjective(AArenaBotController* Bot, FArenaBotObjective& Objective)
{
	AArenaCharacter* P = Bot ? Bot->GetArenaPawn() : nullptr;
	if (!P || !Ball)
	{
		return false;
	}
	const int32 MyTeam = GetTeamOf(Bot);
	switch (Objective.Tag)
	{
	case Tag_Deliver:
		if (P->GetCarriedObject() != Ball)
		{
			return false;
		}
		BotCarrierThink(Bot, P);
		return P->GetCarriedObject() == Ball;
	case Tag_Ball:
		if (Ball->IsCarried())
		{
			return false;
		}
		Objective.Location = Ball->GetObjectLocation();
		return true;
	case Tag_Escort:
	{
		const AArenaCharacter* Carrier = Ball->IsCarried() ? Ball->GetCarrier() : nullptr;
		if (!Carrier || Carrier->GetTeam() != MyTeam || Carrier == P)
		{
			return false;
		}
		Objective.Location = GetEscortSpot(Carrier);
		return true;
	}
	case Tag_Attack:
	{
		const AArenaCharacter* Target = Cast<AArenaCharacter>(Objective.Actor.Get());
		if (!Target || Target->IsDead() || Target->GetCarriedObject() != Ball)
		{
			return false;
		}
		Objective.Location = Target->GetActorLocation();
		return true;
	}
	default:
		return true;
	}
}

void UArenaRules_BR::BotCarrierThink(AArenaBotController* Bot, AArenaCharacter* P)
{
	const float Now = GetWorld()->GetTimeSeconds();
	float& Next = NextBotThinkTime.FindOrAdd(Bot);
	if (Now < Next || Bot->IsDirectMoving())
	{
		return;
	}
	Next = Now + 0.25f;

	AArenaBR_BallLauncher* Launcher = Cast<AArenaBR_BallLauncher>(P->FindWeapon(AArenaBR_BallLauncher::StaticClass()));
	const int32 MyTeam = P->GetTeam();
	const AArenaBR_Goal* EnemyGoal = GetGoal(1 - MyTeam);
	if (!Launcher || !EnemyGoal || (MyTeam != 0 && MyTeam != 1))
	{
		return;
	}
	const FVector Loc = P->GetActorLocation();
	const FVector Eye = P->GetPawnViewLocation();
	const FVector Hoop = EnemyGoal->GetHoopCenter();
	const float HoopDist = FVector::Dist(Eye, Hoop);
	const FVector Approach = ApproachSpot[1 - MyTeam];
	const bool bDive = bDiveGoal[1 - MyTeam];
	auto ClearLine = [this, P](const FVector& From, const FVector& To, const AActor* Ignore2)
	{
		FHitResult Hit;
		FCollisionQueryParams Query(SCENE_QUERY_STAT(ArenaBRBotLine), false, P);
		if (Ignore2)
		{
			Query.AddIgnoredActor(Ignore2);
		}
		return !GetWorld()->LineTraceSingleByObjectType(Hit, From, To, FCollisionObjectQueryParams(ECC_WorldStatic), Query);
	};
	const FVector HoopFront = Hoop - (Hoop - Eye).GetSafeNormal() * 60.f;

	// 1. At the jump-in spot (or close to the ring at its height with a clear run): jump through the ring (7 points)
	if (bDive)
	{
		const bool bAtSpot = FVector::Dist2D(Loc, Approach) < 220.f && FMath::Abs(Loc.Z - Approach.Z) < 150.f;
		const bool bClose = FVector::Dist2D(Loc, Hoop) < ArenaBR::DiveMaxDist && Loc.Z - Hoop.Z > -280.f && Loc.Z - Hoop.Z < 150.f && ClearLine(Loc, HoopFront, nullptr);
		if (bPadGoal[1 - MyTeam] && FVector::Dist2D(Loc, Approach) < 600.f && P->GetVelocity().Z > 300.f)
		{
			// on the pad: steer through the ring while it throws us
			Bot->SetDirectMove(Hoop, 2.5f, false);
			UE_LOG(LogOpenUT2k4, Log, TEXT("BR: %s takes the jump pad to the hoop"), *ArenaBR::NameOf(Bot));
			return;
		}
		if ((bAtSpot && !bPadGoal[1 - MyTeam]) || bClose)
		{
			const float Time = FMath::Clamp(FVector::Dist(Loc, Hoop) / 700.f + 0.6f, 0.8f, 2.5f);
			Bot->SetDirectMove(Hoop, Time, FVector::Dist2D(Loc, Hoop) > 150.f);
			UE_LOG(LogOpenUT2k4, Log, TEXT("BR: %s jumps for the hoop from %.0f"), *ArenaBR::NameOf(Bot), FVector::Dist(Loc, Hoop));
			return;
		}
	}

	const AArenaCharacter* Enemy = Bot->GetEnemy();
	const bool bEnemyClose = Enemy && Bot->IsEnemyVisible() && FVector::Dist(Enemy->GetActorLocation(), Loc) < 1600.f;
	const bool bHurtRecently = Now - P->LastHitTime < 1.5f;
	const bool bThreatened = (bEnemyClose && P->Health < 60.f) || (bHurtRecently && P->Health < 40.f);

	// 2. Shot at the ring (3 points): when it can't be walked or jumped into, when this carrier prefers shooting
	//    (UT2004 PreferShootScore: a random pick per possession), or when about to lose the ball
	const bool bPreferShoot = !bDive && EnemyGoal->IsElevated();
	if (HoopDist < 2600.f && HoopDist > 350.f && (bPreferShoot || CarrierScoringRand < 0.3f || (bThreatened && HoopDist < 2000.f)) && ClearLine(Eye, HoopFront, nullptr))
	{
		if (Launcher->ThrowAt(Hoop))
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("BR: %s shoots at the hoop from %.0f (at %s)"), *ArenaBR::NameOf(Bot), HoopDist, *Loc.ToCompactString());
			return;
		}
	}

	// 3. Pass (TryPassTo): away from both bases, to a healthy teammate ahead on our way and in sight; anywhere when
	//    threatened
	const float MyGoalDist = FVector::Dist(Loc, Approach);
	const bool bAwayFromBases = MyGoalDist > 3500.f && FVector::Dist(Loc, ApproachSpot[MyTeam]) > 3500.f;
	if ((bAwayFromBases || bThreatened) && Now - CarrierSince > 1.5f && Now >= NextPassTime[MyTeam])
	{
		const TArray<FVector>& Route = Bot->GetPathPoints();
		FVector Ahead = Route.Num() > 0 ? Route[FMath::Min(2, Route.Num() - 1)] - Loc : Approach - Loc;
		Ahead = Ahead.GetSafeNormal2D();
		AArenaCharacter* Best = nullptr;
		float BestScore = BIG_NUMBER;
		for (TActorIterator<AArenaCharacter> It(GetWorld()); It; ++It)
		{
			AArenaCharacter* C = *It;
			if (C == P || !C->IsAlive() || C->GetTeam() != MyTeam || C->GetCarriedObject())
			{
				continue;
			}
			const FVector To = C->GetActorLocation() - Loc;
			const float Dist = To.Size();
			if (Dist < 500.f || Dist > 6000.f || C->Health + C->Shield < FMath::Min(60.f, P->Health))
			{
				continue;
			}
			const float GoalDist = FVector::Dist(C->GetActorLocation(), Approach);
			const bool bAhead = FVector::DotProduct(To.GetSafeNormal2D(), Ahead) > 0.5f && GoalDist < MyGoalDist - 600.f;
			const bool bRescue = bThreatened && Dist < 3500.f && GoalDist < MyGoalDist + 1500.f;
			if (!(bAhead || bRescue) || !ClearLine(Eye, C->GetActorLocation(), C))
			{
				continue;
			}
			// no enemy close to the ball's way (it would catch it)
			bool bCovered = false;
			for (TActorIterator<AArenaCharacter> E(GetWorld()); E && !bCovered; ++E)
			{
				bCovered = E->IsAlive() && E->GetTeam() != MyTeam
					&& FMath::PointDistToSegment(E->GetActorLocation(), Eye, C->GetActorLocation()) < 400.f;
			}
			if (bCovered)
			{
				continue;
			}
			const float Score = GoalDist + Dist * 0.3f;
			if (Score < BestScore)
			{
				BestScore = Score;
				Best = C;
			}
		}
		if (Best && Launcher->PassTo(Best))
		{
			NextPassTime[MyTeam] = Now + 2.5f;
			UE_LOG(LogOpenUT2k4, Log, TEXT("BR: %s passes to %s"), *ArenaBR::NameOf(Bot), *ArenaBR::NameOf(Best->GetController()));
			if (AArenaBotController* Receiver = Cast<AArenaBotController>(Best->GetController()))
			{
				Receiver->ForceGoalUpdate();
			}
		}
	}
}

float UArenaRules_BR::GetBotEnemyWeight(AArenaBotController* Bot, AArenaCharacter* Candidate) const
{
	// UT2004 ModifyThreat: a visible ball carrier is the main threat, all the more when close
	if (Ball && Candidate && Ball->IsCarried() && Ball->GetCarrier() == Candidate)
	{
		const APawn* P = Bot ? Bot->GetPawn() : nullptr;
		return (P && FVector::Dist(P->GetActorLocation(), Candidate->GetActorLocation()) < 3000.f) ? 0.15f : 0.4f;
	}
	return 1.f;
}

float UArenaRules_BR::GetBotHuntPriority(AArenaBotController* Bot, AArenaCharacter* Enemy, const FVector& LastSeen) const
{
	const AArenaCharacter* P = Bot ? Bot->GetArenaPawn() : nullptr;
	const int32 Team = GetTeamOf(Bot);
	if (!P || !Ball || (Team != 0 && Team != 1))
	{
		return -1.f;
	}
	const AArenaCharacter* Carrier = Ball->IsCarried() ? Ball->GetCarrier() : nullptr;
	if (Carrier == P || Carrier == Enemy)
	{
		return 0.f; // the carrier goes for the goal; the enemy carrier is chased as an objective
	}
	if (Bot->IsDefender())
	{
		// defenders (LostContact(4)) go after what comes near their goal, above guarding a spot
		return FVector::Dist(LastSeen, ApproachSpot[Team]) < 4000.f ? 1.2f : 0.f;
	}
	if (Carrier && Carrier->GetTeam() == Team)
	{
		// escorts fight for the carrier: enemies near it before the spot ahead of it
		return FVector::Dist(LastSeen, Carrier->GetActorLocation()) < 3000.f ? 1.8f : 0.5f;
	}
	// a loose ball comes first; hunting fills the gaps
	return 0.6f + 0.05f * Bot->GetSkill();
}

// ---------------------------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------------------------

bool UArenaRules_BR::DrawScoreInfo(AArenaHUD* HUD, UCanvas* Canvas)
{
	const AArenaGameState* GS = GetGameState();
	if (!HUD || !GS)
	{
		return false;
	}
	const FArenaDraw& D = HUD->GetDraw();
	const AArenaBR_Ball* TheBall = GetBall();
	const AArenaCharacter* Carrier = TheBall && TheBall->IsCarried() ? TheBall->GetCarrier() : nullptr;
	const float Now = HUD->Now();
	if (HUD->IsUTHud())
	{
		// HudCBombingRun: the ball's state as GRI.FlagState of the viewer's team (0 home, 1 we have it, 2 they have it, 3 loose)
		const APlayerController* PC = HUD->GetOwningPlayerController();
		const AArenaPlayerState* MyPS = PC ? PC->GetPlayerState<AArenaPlayerState>() : nullptr;
		const int32 MyTeam = MyPS ? MyPS->Team : 0;
		uint8 State = 0;
		if (TheBall && TheBall->IsCarried())
		{
			State = (Carrier && Carrier->GetTeam() == (MyTeam == 1 ? 1 : 0)) ? 1 : 2;
		}
		else if (TheBall && !TheBall->IsHome())
		{
			State = 3;
		}
		// the dot points at the enemy goal while holding the ball launcher, else at the ball
		FVector Target = FVector::ZeroVector;
		const FVector* TargetPtr = nullptr;
		const AArenaCharacter* Me = PC ? Cast<AArenaCharacter>(PC->GetPawn()) : nullptr;
		if (Me && Me->GetCarriedObject() == TheBall && (MyTeam == 0 || MyTeam == 1))
		{
			if (const AArenaBR_Goal* Goal = GetGoal(1 - MyTeam))
			{
				Target = Goal->GetHoopCenter();
				TargetPtr = &Target;
			}
			if (const AArenaBR_BallLauncher* L = Cast<AArenaBR_BallLauncher>(Me->GetCurrentWeapon()))
			{
				if (const AArenaCharacter* Locked = L->GetLockedTarget())
				{
					HUD->DrawUTTargeting(Locked->GetActorLocation());
				}
			}
		}
		else if (TheBall)
		{
			Target = Carrier ? Carrier->GetActorLocation() : TheBall->GetObjectLocation();
			TargetPtr = &Target;
		}
		HUD->DrawUTBombingRun(State, TargetPtr);
		return true;
	}
	HUD->DrawTeamScorePanels(24.f, [&](int32 Team, float X, float Top, float BW)
	{
		using namespace ArenaHudStyle;
		const bool bHas = Carrier && Carrier->GetTeam() == Team;
		const float Y = Top + D.S(86.f);
		// small ball icon
		if (!ArenaArtHUD::DrawSpriteCentered(D, bHas ? ArenaArtHUD::Sprites::BallGold() : ArenaArtHUD::Sprites::BallSilver(), X + D.S(22.f), Y, D.S(22.f),
			bHas ? FLinearColor::White : FLinearColor(1.f, 1.f, 1.f, 0.35f)))
		{
			D.Rect(X + D.S(12.f), Y - D.S(6.f), D.S(12.f), D.S(12.f), bHas ? FLinearColor(1.f, 0.78f, 0.25f) : WithAlpha(TextDim(), 0.4f));
		}
		D.TextMid(bHas ? TEXT("HAS BALL") : TEXT("--"), X + BW - D.S(12.f), Y, bHas ? WithAlpha(FLinearColor(1.f, 0.85f, 0.4f), 0.6f + 0.4f * FMath::Abs(FMath::Sin(Now * 5.f))) : TextDim(),
			D.S(15.f), EArenaAlign::Right);
	}, GetGoalText(GS->GoalScore));
	return true;
}

void UArenaRules_BR::DrawHUD(AArenaHUD* HUD, UCanvas* Canvas)
{
	Super::DrawHUD(HUD, Canvas);
	const AArenaGameState* GS = GetGameState();
	AArenaBR_Ball* TheBall = GetBall();
	if (!HUD || !GS || !TheBall || !Canvas || HUD->IsUTHud())
	{
		// the UT2004 HUD shows the ball in the score pass (DrawScoreInfo)
		return;
	}
	using namespace ArenaHudStyle;
	const FArenaDraw& D = HUD->GetDraw();
	const FLinearColor BallCol(1.f, 0.8f, 0.3f);

	// Ball state line under the score panel
	FString State;
	FLinearColor StateCol = TextWhite();
	const AArenaCharacter* Carrier = TheBall->IsCarried() ? TheBall->GetCarrier() : nullptr;
	switch (TheBall->GetCarryState())
	{
	case EArenaCarryState::Home:
		State = TEXT("BALL AT CENTRE");
		break;
	case EArenaCarryState::Carried:
		State = FString::Printf(TEXT("BALL: %s"), Carrier ? *ArenaTeam::Name(Carrier->GetTeam()).ToUpper() : TEXT("HELD"));
		StateCol = Carrier ? TeamTextColor(Carrier->GetTeam()) : TextWhite();
		break;
	case EArenaCarryState::Dropped:
		State = FString::Printf(TEXT("BALL LOOSE - RESET IN %d"), FMath::CeilToInt(TheBall->GetReturnCountdown()));
		StateCol = BallCol;
		break;
	case EArenaCarryState::InFlight:
		State = TEXT("BALL IN THE AIR");
		StateCol = BallCol;
		break;
	}
	D.Text(State, D.W() - D.S(24.f), HUD->GetScoreInfoBottom() + D.S(4.f), StateCol, D.S(16.f), EArenaAlign::Right);

	if (!GS->IsPlaying() && GS->Phase != EArenaMatchPhase::Countdown)
	{
		return;
	}
	const APlayerController* PC = HUD->GetOwningPlayerController();
	const AArenaCharacter* Me = PC ? Cast<AArenaCharacter>(PC->GetPawn()) : nullptr;
	const int32 MyTeam = Me ? Me->GetTeam() : -1;

	if (Me && Me->GetCarriedObject() == TheBall)
	{
		// Carrier: show the hoop to attack and the locked teammate
		if (const AArenaBR_Goal* Target = GetGoal(1 - MyTeam))
		{
			ArenaBR::DrawMarker(HUD, Canvas, Target->GetHoopCenter(), TEXT("ENEMY HOOP"), TeamTextColor(1 - MyTeam));
		}
		if (const AArenaBR_BallLauncher* L = Cast<AArenaBR_BallLauncher>(Me->GetCurrentWeapon()))
		{
			if (const AArenaCharacter* Locked = L->GetLockedTarget())
			{
				const APlayerState* PS = Locked->GetPlayerState();
				ArenaBR::DrawMarker(HUD, Canvas, Locked->GetActorLocation() + FVector(0.f, 0.f, 110.f),
					FString::Printf(TEXT("PASS: %s"), PS ? *PS->GetPlayerName() : TEXT("?")), FLinearColor(0.4f, 1.f, 0.5f));
			}
		}
		return;
	}
	// Everybody else: where is the ball?
	FString Label = TEXT("BALL");
	FLinearColor Col = BallCol;
	if (Carrier)
	{
		const bool bFriendly = Carrier->GetTeam() == MyTeam;
		Label = bFriendly ? TEXT("ESCORT") : TEXT("STOP CARRIER");
		Col = TeamTextColor(Carrier->GetTeam());
	}
	ArenaBR::DrawMarker(HUD, Canvas, TheBall->GetObjectLocation() + FVector(0.f, 0.f, Carrier ? 120.f : 40.f), Label, Col);
}
