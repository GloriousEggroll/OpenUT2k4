#include "ArenaRules_CTF.h"
#include "OpenUT2k4.h"
#include "ArenaGameMode.h"
#include "ArenaGameState.h"
#include "ArenaPlayerState.h"
#include "ArenaCharacter.h"
#include "ArenaBotController.h"
#include "ArenaFlag.h"
#include "ArenaHUD.h"
#include "ArenaArtHUD.h"
#include "ArenaLevelBuilder.h"
#include "ArenaNavGraph.h"
#include "ArenaPickup.h"
#include "ArenaUE2World.h"
#include "Engine/World.h"

ARENA_REGISTER_RULES(UArenaRules_CTF, "CTF", "Capture the Flag");

bool UArenaRules_CTF::SupportsLayout(const FArenaLayoutInfo& Layout) const
{
	return Layout.bSupportsCTF;
}

FString UArenaRules_CTF::GetGoalText(int32 Goal) const
{
	return Goal > 0 ? FString::Printf(TEXT("First to %d captures"), Goal) : FString();
}

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------

AArenaFlag* UArenaRules_CTF::SpawnFlag(AArenaLevelBuilder* Builder, const FVector& FloorLocation, int32 Team)
{
	UWorld* World = GetWorld();
	if (!World || (Team != 0 && Team != 1))
	{
		return nullptr;
	}
	const FTransform T(FRotator::ZeroRotator, FloorLocation);
	AArenaFlag* Flag = World->SpawnActorDeferred<AArenaFlag>(AArenaFlag::StaticClass(), T, Builder, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Flag)
	{
		return nullptr;
	}
	Flag->SetTeam(Team);
	Flag->FinishSpawning(T);
	return Flag;
}

bool UArenaRules_CTF::OnLevelBuilt(AArenaLevelBuilder* Builder)
{
	if (!Builder)
	{
		return false;
	}
	// Flag bases: the builder maps the map's flag bases to the TeamBase0/1 anchors.
	for (int32 Team = 0; Team < 2; ++Team)
	{
		FTransform Base;
		if (!Flags[Team] && Builder->GetAnchorPoint(FName(*FString::Printf(TEXT("TeamBase%d"), Team)), Base))
		{
			Flags[Team] = SpawnFlag(Builder, Base.GetLocation(), Team);
		}
	}
	if (!Flags[0] || !Flags[1])
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("Arena: %s has no flag bases, playing Team Deathmatch instead of CTF"), *Builder->GetBuiltLayoutId());
		for (TObjectPtr<AArenaFlag>& Flag : Flags)
		{
			if (Flag)
			{
				Flag->Destroy();
				Flag = nullptr;
			}
		}
		return false;
	}
	return true;
}

void UArenaRules_CTF::InitGameState(AArenaGameState* GS)
{
	if (GS)
	{
		GS->Flags[0] = Flags[0];
		GS->Flags[1] = Flags[1];
	}
}

void UArenaRules_CTF::GetNavHints(TArray<FVector>& OutPoints) const
{
	for (int32 Team = 0; Team < 2; ++Team)
	{
		if (const AArenaFlag* Flag = Flags[Team])
		{
			OutPoints.Add(Flag->GetHomeLocation() + FVector(0, 0, 10.f));
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Flag events
// ---------------------------------------------------------------------------------------------

void UArenaRules_CTF::OnFlagTaken(AArenaFlag* Flag, AController* By)
{
	UE_LOG(LogOpenUT2k4, Log, TEXT("CTF: %s flag taken by %s"), *ArenaTeam::Name(Flag->GetTeam()), By && By->PlayerState ? *By->PlayerState->GetPlayerName() : TEXT("-"));
	const APlayerState* PS = By ? By->GetPlayerState<APlayerState>() : nullptr;
	GameMode->BroadcastCenterMessage(FString::Printf(TEXT("%s took the %s flag!"), PS ? *PS->GetPlayerName() : TEXT("Someone"), *ArenaTeam::Name(Flag->GetTeam())),
		ArenaTeam::Color(Flag->GetTeam()), 2.5f, TEXT("S_Flag_Taken"));
}

void UArenaRules_CTF::OnFlagDropped(AArenaFlag* Flag, AController* By)
{
	UE_LOG(LogOpenUT2k4, Log, TEXT("CTF: %s flag dropped by %s"), *ArenaTeam::Name(Flag->GetTeam()), By && By->PlayerState ? *By->PlayerState->GetPlayerName() : TEXT("-"));
	GameMode->BroadcastCenterMessage(FString::Printf(TEXT("The %s flag was dropped"), *ArenaTeam::Name(Flag->GetTeam())), ArenaTeam::Color(Flag->GetTeam()), 2.f);
}

void UArenaRules_CTF::OnFlagReturned(AArenaFlag* Flag, AController* By)
{
	UE_LOG(LogOpenUT2k4, Log, TEXT("CTF: %s flag returned by %s"), *ArenaTeam::Name(Flag->GetTeam()), By && By->PlayerState ? *By->PlayerState->GetPlayerName() : TEXT("-"));
	if (AArenaPlayerState* PS = By ? By->GetPlayerState<AArenaPlayerState>() : nullptr)
	{
		PS->Returns++;
		PS->AddScore(1.f);
	}
	GameMode->BroadcastCenterMessage(FString::Printf(TEXT("The %s flag was returned"), *ArenaTeam::Name(Flag->GetTeam())), ArenaTeam::Color(Flag->GetTeam()), 2.f, TEXT("S_Flag_Returned"));
}

void UArenaRules_CTF::OnFlagCaptured(AArenaFlag* Flag, AController* By)
{
	UE_LOG(LogOpenUT2k4, Log, TEXT("CTF: %s flag captured by %s"), *ArenaTeam::Name(Flag->GetTeam()), By && By->PlayerState ? *By->PlayerState->GetPlayerName() : TEXT("-"));
	AArenaPlayerState* PS = By ? By->GetPlayerState<AArenaPlayerState>() : nullptr;
	const int32 ScoringTeam = 1 - Flag->GetTeam();
	if (GameMode->IsPlaying())
	{
		GameMode->AddTeamScore(ScoringTeam, 1);
	}
	if (PS)
	{
		PS->Captures++;
		PS->AddScore(5.f);
	}
	GameMode->BroadcastCenterMessage(FString::Printf(TEXT("%s captured the %s flag!"), PS ? *PS->GetPlayerName() : TEXT("Someone"), *ArenaTeam::Name(Flag->GetTeam())),
		ArenaTeam::Color(ScoringTeam), 3.f, TEXT("S_Flag_Captured"));
	GameMode->CheckEndConditions();
}

// ---------------------------------------------------------------------------------------------
// Bots
// ---------------------------------------------------------------------------------------------

void UArenaRules_CTF::GetBotObjectives(AArenaBotController* Bot, TArray<FArenaBotObjective>& OutGoals)
{
	AArenaCharacter* P = Bot ? Bot->GetArenaPawn() : nullptr;
	const int32 MyTeam = GetTeamOf(Bot);
	if (!P || (MyTeam != 0 && MyTeam != 1))
	{
		return;
	}
	AArenaFlag* OwnFlag = Flags[MyTeam];
	AArenaFlag* EnemyFlag = Flags[1 - MyTeam];
	const FVector MyLoc = P->GetActorLocation();
	// UT2004 TeamAI.OrderList (Defend, Attack, Attack, Freelance, ...): one bot in four of a team defends the base
	if (GameMode)
	{
		int32 Index = 0, Count = 0;
		for (AController* C : GameMode->GetBots())
		{
			if (C && GetTeamOf(C) == MyTeam)
			{
				Index = C == Bot ? Count : Index;
				++Count;
			}
		}
		Bot->SetDefender(Index % 4 == 0);
	}
	const bool bDefender = Bot->IsDefender();

	auto Add = [&OutGoals](const FVector& Loc, AActor* Actor, float Priority, int32 Tag, EArenaBotAction Action, bool bCompleteOnReach)
	{
		FArenaBotObjective O(Loc, Actor, Priority, Action, Tag);
		O.bCompleteOnReach = bCompleteOnReach;
		OutGoals.Add(O);
	};

	// UE2 SquadAI alternate paths: each trip (out to the enemy flag, home with it) goes via a random AssaultPath of
	// the enemy flag, so attacks and returns don't all take the same way
	FBotRoute& Route = Routes.FindOrAdd(Bot);
	const bool bCarry = P->CarriedFlag != nullptr;
	if (Route.Pawn.Get() != P || Route.bCarry != bCarry)
	{
		Route = FBotRoute();
		Route.Pawn = P;
		Route.bCarry = bCarry;
	}
	if (!Route.bPicked && EnemyFlag && OwnFlag && (bCarry || (!bDefender && EnemyFlag->GetState() == EArenaFlagState::Home)))
	{
		Route.bPicked = true;
		const FVector To = bCarry ? OwnFlag->GetHomeLocation() : EnemyFlag->GetHomeLocation();
		Route.bDone = !PickAltPath(MyLoc, To, EnemyFlag->GetHomeLocation(), bCarry, Bot->GetRandomStream(), Route.Via);
	}

	if (P->CarriedFlag && OwnFlag)
	{
		Add(OwnFlag->GetHomeLocation(), OwnFlag, 100.f, Tag_FlagHome, EArenaBotAction::Deliver, false);
		if (!Route.bDone)
		{
			Add(Route.Via, nullptr, 105.f, Tag_Via, EArenaBotAction::Deliver, true);
		}
		// a hurt carrier picks up health or armour close to its way (Bot.FindInventoryGoal on the way home)
		if (P->Health < 60.f && GameMode && GameMode->GetLevelBuilder())
		{
			AArenaPickup* Best = nullptr;
			float BestDist = 3000.f;
			for (AArenaPickup* Pickup : GameMode->GetLevelBuilder()->GetPickups())
			{
				if (!Pickup || !Pickup->IsAvailable() || Pickup->GetBotDesire(P) <= 0.f || !CanPickUp(P, Pickup))
				{
					continue;
				}
				const EArenaPickupType T = Pickup->GetPickupType();
				if (T != EArenaPickupType::HealthPack && T != EArenaPickupType::SuperHealth && T != EArenaPickupType::ShieldSmall
					&& T != EArenaPickupType::ShieldLarge && T != EArenaPickupType::HealthVial)
				{
					continue;
				}
				const float D = FVector::Dist(MyLoc, Pickup->GetActorLocation());
				if (D < BestDist)
				{
					BestDist = D;
					Best = Pickup;
				}
			}
			if (Best)
			{
				Add(Best->GetActorLocation(), Best, 110.f, Tag_Heal, EArenaBotAction::Collect, true);
			}
		}
	}
	if (OwnFlag)
	{
		const float Dist = FVector::Dist(MyLoc, OwnFlag->GetFlagLocation());
		switch (OwnFlag->GetState())
		{
		case EArenaFlagState::Dropped:
			Add(OwnFlag->GetFlagLocation(), OwnFlag, (bDefender ? 4.f : 1.5f) + (Dist < 2500.f ? 2.5f : 0.f), Tag_FlagLocation, EArenaBotAction::Collect, true);
			break;
		case EArenaFlagState::Carried:
			if (AArenaCharacter* Holder = OwnFlag->GetHolder())
			{
				// CTFSquadAI: the defense goes after the carrier of our flag; the attack keeps attacking, and only
				// attackers that run into the carrier (in sight, or close) turn on it
				const bool bAttackerOnIt = (Bot->GetEnemy() == Holder && Bot->IsEnemyVisible()) || Dist < 2500.f;
				if (bDefender || bAttackerOnIt)
				{
					Add(Holder->GetActorLocation(), Holder, bDefender ? 3.f : 2.f, Tag_Chase, EArenaBotAction::Attack, false);
				}
			}
			break;
		default:
			if (bDefender)
			{
				const FVector Home = OwnFlag->GetHomeLocation();
				FVector DefendSpot = Home;
				const FArenaBotObjective* Current = Bot->GetCurrentObjective();
				if (Current && Current->Tag == Tag_Defend && FVector::Dist2D(Bot->GetGoalLocation(), Home) < 1200.f && !Bot->HasReachedGoal())
				{
					DefendSpot = Bot->GetGoalLocation();
				}
				else
				{
					// A random waypoint near our base (falls back to a random offset)
					FRandomStream& Rng = Bot->GetRandomStream();
					bool bFoundSpot = false;
					if (AArenaNavGraph* Nav = Bot->GetNavGraph(); Nav && Nav->NumNodes() > 0)
					{
						const TArray<FArenaNavNode>& NavNodes = Nav->GetNodes();
						for (int32 Try = 0; Try < 16 && !bFoundSpot; ++Try)
						{
							const FVector& L = NavNodes[Rng.RandHelper(NavNodes.Num())].Location;
							if (FVector::Dist(L, Home) < 1200.f)
							{
								DefendSpot = L;
								bFoundSpot = true;
							}
						}
					}
					if (!bFoundSpot)
					{
						const FVector Off = Rng.GetUnitVector() * Rng.FRandRange(200.f, 700.f);
						DefendSpot = Home + FVector(Off.X, Off.Y, 0.f);
					}
				}
				Add(DefendSpot, OwnFlag, 0.7f, Tag_Defend, EArenaBotAction::Defend, true);
			}
			break;
		}
	}
	if (EnemyFlag && !P->CarriedFlag)
	{
		const float Dist = FVector::Dist(MyLoc, EnemyFlag->GetFlagLocation());
		switch (EnemyFlag->GetState())
		{
		case EArenaFlagState::Home:
			if (!bDefender)
			{
				// UT2004 SquadAI.CheckSquadObjectives: an attacker fights the enemy it sees (FightEnemy) unless the flag
				// is close and in sight (CTFSquadAI goes for it then). Below 0.9 the bot fights instead of running past.
				const bool bFlagInReach = Dist < 2000.f && Bot->LineOfSightTo(EnemyFlag, P->GetPawnViewLocation());
				// a close enemy is a threat to fight; one further away is shot at on the way (strafing)
				const bool bFight = Bot->GetEnemy() && Bot->IsEnemyVisible() && !bFlagInReach
					&& FVector::Dist(Bot->GetEnemy()->GetActorLocation(), MyLoc) < 2000.f;
				Add(EnemyFlag->GetFlagLocation(), EnemyFlag, bFight ? 0.7f : 1.6f, Tag_FlagLocation, EArenaBotAction::Collect, true);
				if (!Route.bDone)
				{
					Add(Route.Via, nullptr, bFight ? 0.75f : 1.65f, Tag_Via, EArenaBotAction::Move, true);
				}
			}
			break;
		case EArenaFlagState::Dropped:
			Add(EnemyFlag->GetFlagLocation(), EnemyFlag, (bDefender && Dist > 2000.f) ? 0.6f : 2.2f, Tag_FlagLocation, EArenaBotAction::Collect, true);
			break;
		case EArenaFlagState::Carried:
			if (!bDefender && EnemyFlag->GetHolder() && EnemyFlag->GetHolder() != P)
			{
				// escorts stay with the carrier (CTFSquadAI: the carrier leads the squad) and fight the threats close to it
				const AArenaCharacter* Holder = EnemyFlag->GetHolder();
				const AArenaCharacter* Threat = Bot->GetEnemy();
				const bool bThreat = Threat && Bot->IsEnemyVisible() && FVector::Dist(Threat->GetActorLocation(), Holder->GetActorLocation()) < 2500.f;
				const bool bClose = FVector::Dist(MyLoc, Holder->GetActorLocation()) < 600.f;
				Add(Holder->GetActorLocation(), EnemyFlag->GetHolder(), (bThreat || bClose) ? 0.7f : 1.3f, Tag_Chase, EArenaBotAction::Escort, false);
			}
			break;
		}
	}
}

bool UArenaRules_CTF::UpdateBotObjective(AArenaBotController* Bot, FArenaBotObjective& Objective)
{
	const AArenaCharacter* P = Bot ? Bot->GetArenaPawn() : nullptr;
	if (!P)
	{
		return false;
	}
	const int32 MyTeam = GetTeamOf(Bot);
	switch (Objective.Tag)
	{
	case Tag_FlagLocation:
		if (const AArenaFlag* Flag = Cast<AArenaFlag>(Objective.Actor.Get()))
		{
			Objective.Location = Flag->GetFlagLocation();
			// Our own flag got returned / enemy flag got picked up by someone else
			return !((Flag->GetTeam() == MyTeam && Flag->IsHome()) || (Flag->GetState() == EArenaFlagState::Carried && Flag->GetHolder() != P));
		}
		return false;
	case Tag_FlagHome:
		return P->CarriedFlag != nullptr;
	case Tag_Via:
		if (FBotRoute* Route = Routes.Find(Bot))
		{
			if (FVector::Dist2D(P->GetActorLocation(), Route->Via) < 400.f && FMath::Abs(P->GetActorLocation().Z - Route->Via.Z) < 300.f)
			{
				Route->bDone = true;
			}
			return !Route->bDone && Route->bCarry == (P->CarriedFlag != nullptr);
		}
		return false;
	case Tag_Heal:
	{
		const AArenaPickup* Pickup = Cast<AArenaPickup>(Objective.Actor.Get());
		return Pickup && Pickup->IsAvailable() && P->CarriedFlag != nullptr;
	}
	case Tag_Chase:
		if (const AArenaCharacter* Target = Cast<AArenaCharacter>(Objective.Actor.Get()))
		{
			Objective.Location = Target->GetActorLocation();
			return !(Target->IsDead() || Target->CarriedFlag == nullptr);
		}
		return false;
	default:
		return true;
	}
}

float UArenaRules_CTF::GetBotEnemyWeight(AArenaBotController* Bot, AArenaCharacter* Candidate) const
{
	const int32 Team = GetTeamOf(Bot);
	const AArenaFlag* OwnFlag = GetFlag(Team);
	// The carrier of our flag is the priority
	return (OwnFlag && OwnFlag->GetHolder() == Candidate) ? 0.4f : 1.f;
}

const AArenaNavGraph* UArenaRules_CTF::GetBotNavGraph() const
{
	return GameMode ? GameMode->GetNavGraph() : nullptr;
}

bool UArenaRules_CTF::PickAltPath(const FVector& From, const FVector& To, const FVector& ObjectiveLoc, bool bReturn, FRandomStream& Rng, FVector& OutVia) const
{
	const TArray<ArenaUE2World::FAssaultPath> Paths = ArenaUE2World::GetAssaultPaths(GetWorld());
	const float Direct = FVector::Dist(From, To);
	TArray<int32> Cands;
	float Total = 0.f;
	for (int32 i = 0; i < Paths.Num(); ++i)
	{
		const ArenaUE2World::FAssaultPath& A = Paths[i];
		if (!A.bHasObjective || FVector::Dist(A.ObjectiveLocation, ObjectiveLoc) > 1500.f || (bReturn ? A.bNoReturn : A.bReturnOnly))
		{
			continue;
		}
		// only where the nav graph goes (a path on geometry our world lacks would strand the bot)
		if (const AArenaNavGraph* Nav = GetBotNavGraph(); Nav && Nav->NumNodes() > 0)
		{
			const int32 N = Nav->FindNearestNode(A.Location, false);
			if (N == INDEX_NONE || FVector::Dist(Nav->GetNodes()[N].Location, A.Location) > 250.f)
			{
				continue;
			}
		}
		const float ToVia = FVector::Dist(From, A.Location);
		if (ToVia < 1000.f || ToVia + FVector::Dist(A.Location, To) > Direct * 1.5f + 3000.f)
		{
			continue;
		}
		Cands.Add(i);
		Total += FMath::Max(A.Priority, 0.05f);
	}
	if (Cands.Num() == 0)
	{
		return false;
	}
	float R = Rng.FRand() * Total;
	for (const int32 i : Cands)
	{
		R -= FMath::Max(Paths[i].Priority, 0.05f);
		if (R <= 0.f)
		{
			OutVia = Paths[i].Location;
			return true;
		}
	}
	OutVia = Paths[Cands.Last()].Location;
	return true;
}

float UArenaRules_CTF::GetBotHuntPriority(AArenaBotController* Bot, AArenaCharacter* Enemy, const FVector& LastSeen) const
{
	const AArenaCharacter* P = Bot ? Bot->GetArenaPawn() : nullptr;
	const int32 Team = GetTeamOf(Bot);
	if (!P || (Team != 0 && Team != 1) || P->CarriedFlag)
	{
		return P && P->CarriedFlag ? 0.f : -1.f;
	}
	const AArenaFlag* OwnFlag = GetFlag(Team);
	const AArenaFlag* EnemyFlag = GetFlag(1 - Team);
	if (Bot->IsDefender() && OwnFlag)
	{
		// defenders go after enemies close to the flag base, above guarding a spot
		return FVector::Dist(LastSeen, OwnFlag->GetHomeLocation()) < 3500.f ? 1.f : 0.f;
	}
	const AArenaCharacter* OurCarrier = EnemyFlag ? EnemyFlag->GetHolder() : nullptr;
	if (OurCarrier && FVector::Dist(LastSeen, OurCarrier->GetActorLocation()) < 3000.f)
	{
		return 1.2f; // escorts deal with the threats to their carrier
	}
	return -1.f;
}

// ---------------------------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------------------------

bool UArenaRules_CTF::DrawScoreInfo(AArenaHUD* HUD, UCanvas* Canvas)
{
	const AArenaGameState* GS = GetGameState();
	if (!HUD || !GS)
	{
		return false;
	}
	if (HUD->IsUTHud())
	{
		// HudCCaptureTheFlag: GRI.FlagState per team (0 home, 2 held by the enemy, 3 dropped) and the bases' direction dots
		uint8 FlagState[2] = { 0, 0 };
		FVector Bases[2];
		const FVector* BasePtr[2] = { nullptr, nullptr };
		for (int32 Team = 0; Team < 2; ++Team)
		{
			if (const AArenaFlag* Flag = GS->Flags[Team])
			{
				FlagState[Team] = Flag->GetState() == EArenaFlagState::Carried ? 2 : (Flag->GetState() == EArenaFlagState::Dropped ? 3 : 0);
				Bases[Team] = Flag->GetHomeLocation();
				BasePtr[Team] = &Bases[Team];
			}
		}
		HUD->DrawUTCaptureTheFlag(FlagState, BasePtr);
		return true;
	}
	const FArenaDraw& D = HUD->GetDraw();
	const float Now = HUD->GetWorld() ? HUD->GetWorld()->GetTimeSeconds() : 0.f;
	HUD->DrawTeamScorePanels(24.f, [&](int32 Team, float X, float Top, float BW)
	{
		using namespace ArenaHudStyle;
		const AArenaFlag* Flag = GS->Flags[Team];
		FString State = TEXT("--");
		FLinearColor SC = TextDim();
		if (Flag)
		{
			switch (Flag->GetState())
			{
			case EArenaFlagState::Home:
				State = TEXT("HOME");
				SC = HealthColor();
				break;
			case EArenaFlagState::Carried:
				State = TEXT("TAKEN");
				SC = WithAlpha(FLinearColor(1.f, 0.4f, 0.3f), 0.6f + 0.4f * FMath::Abs(FMath::Sin(Now * 5.f)));
				break;
			case EArenaFlagState::Dropped:
				State = TEXT("DROPPED");
				SC = ShieldColor();
				break;
			}
		}
		// little flag icon
		const FLinearColor TC = TeamTint(Team);
		const float FX = X + D.S(16.f), FY = Top + D.S(78.f);
		using namespace ArenaArtHUD;
		if (DrawSpriteCentered(D, Sprites::Flag(), X + D.S(26.f), FY + D.S(8.f), D.S(22.f), FMath::Lerp(TC, FLinearColor::White, 0.15f)))
		{
			// state mark next to the flag: "!" while carried, arrow while dropped
			if (Flag && Flag->GetState() == EArenaFlagState::Carried)
			{
				DrawSpriteCentered(D, Sprites::FlagTaken(), X + D.S(50.f), FY + D.S(8.f), D.S(20.f), WithAlpha(FLinearColor::White, SC.A));
			}
			else if (Flag && Flag->GetState() == EArenaFlagState::Dropped)
			{
				DrawSpriteCentered(D, Sprites::FlagDropped(), X + D.S(52.f), FY + D.S(8.f), D.S(18.f), FLinearColor::White);
			}
			D.TextMid(State, X + BW - D.S(10.f), FY + D.S(8.f), SC, D.S(14.f), EArenaAlign::Right);
			return;
		}
		D.Rect(FX, FY, D.S(2.f), D.S(16.f), TextWhite());
		D.Rect(FX + D.S(2.f), FY, D.S(12.f), D.S(8.f), TC);
		D.TextMid(State, X + BW - D.S(12.f), FY + D.S(8.f), SC, D.S(15.f), EArenaAlign::Right);
	}, GetGoalText(GS->GoalScore));
	return true;
}
