#include "ArenaRules_INV.h"
#include "OpenUT2k4.h"
#include "ArenaINV_Monster.h"
#include "ArenaINV_MonsterController.h"
#include "ArenaINV_WaveState.h"
#include "ArenaGameMode.h"
#include "ArenaGameState.h"
#include "ArenaPlayerState.h"
#include "ArenaPlayerController.h"
#include "ArenaBotController.h"
#include "ArenaCharacter.h"
#include "ArenaLevelBuilder.h"
#include "ArenaNavGraph.h"
#include "ArenaHUD.h"
#include "ArenaArtHUD.h"
#include "ArenaHudFonts.h"
#include "ArenaWeapons.h"
#include "ArenaFX.h"
#include "ArenaAssets.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/PlayerStart.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "Engine/Canvas.h"
#include "EngineUtils.h"

ARENA_REGISTER_RULES(UArenaRules_INV, "INV", "Invasion");

namespace ArenaINVRules
{
	constexpr float FirstWaveDelay = 6.f;
	constexpr float SpawnInterval = 0.35f;
	constexpr float SpectateDelay = 2.5f;
	constexpr float StragglerTime = 16.f;
	constexpr float MinSpawnDistance = 1300.f;

	const FLinearColor WaveColor(0.55f, 1.f, 0.25f);
	const FLinearColor BossColor(1.f, 0.35f, 0.1f);
	const FLinearColor DangerColor(1.f, 0.3f, 0.25f);

	/** Line of sight from a player's eye to a point (world geometry only). */
	bool IsPointVisible(UWorld* World, const FVector& Eye, const FVector& Point)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(INVSpawnVis), false);
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		RP.CollisionResponse.SetResponse(ECC_PhysicsBody, ECR_Ignore);
		return !World->LineTraceTestByChannel(Eye, Point, ECC_Visibility, Q, RP);
	}

	/** True if a capsule of this size fits at Center. */
	bool CapsuleFits(UWorld* World, const FVector& Center, float Radius, float HalfHeight)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(INVSpawnFit), false);
		return !World->OverlapBlockingTestByChannel(Center, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Radius, HalfHeight), Q);
	}

	int32 PointsFor(EArenaINVMonsterType Type)
	{
		return ArenaINV::GetMonsterDef(Type).Points;
	}
}

// =============================================================================================
// Identity / options
// =============================================================================================

FString UArenaRules_INV::GetGoalText(int32 Goal) const
{
	return Goal > 0 ? FString::Printf(TEXT("Survive %d waves"), Goal) : FString();
}

int32 UArenaRules_INV::GetNumWaves() const
{
	const int32 Goal = GetGoalScore();
	return Goal > 0 ? Goal : GetDefaultGoalScore();
}

bool UArenaRules_INV::IsMonsterController(const AController* C)
{
	return C && C->IsA<AArenaINV_MonsterController>();
}

void UArenaRules_INV::InitRules(AArenaGameMode* InGameMode, const FString& Options)
{
	Super::InitRules(InGameMode, Options);
	WaveDelay = float(FMath::Clamp(UGameplayStatics::GetIntOption(Options, TEXT("WaveDelay"), 12), 3, 90));
	StartWaveOption = FMath::Max(1, UGameplayStatics::GetIntOption(Options, TEXT("StartWave"), 1));
}

// =============================================================================================
// Level / spawn candidates
// =============================================================================================

bool UArenaRules_INV::OnLevelBuilt(AArenaLevelBuilder* Builder)
{
	UWorld* World = GetWorld();
	if (!Builder || !World)
	{
		return false;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	WaveState = World->SpawnActor<AArenaINV_WaveState>(AArenaINV_WaveState::StaticClass(), FTransform::Identity, Params);

	SpawnFloors.Reset();
	// Path nodes are the best hidden spots (the map geometry is traceable already).
	for (const FArenaRawMarker& M : Builder->GetRawMarkers(TEXT("path_nodes")))
	{
		FVector Floor;
		if (Builder->TraceFloor(M.Location + FVector(0.f, 0.f, 40.f), 400.f, Floor))
		{
			SpawnFloors.Add(Floor);
		}
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("Invasion: level %s - %d path node spawn spots"), *Builder->GetBuiltLayoutId(), SpawnFloors.Num());
	return true;
}

void UArenaRules_INV::GetNavHints(TArray<FVector>& OutPoints) const
{
	// A sample of the spawn spots so monsters appearing there are on the graph
	const int32 Step = FMath::Max(1, SpawnFloors.Num() / 48);
	for (int32 i = 0; i < SpawnFloors.Num(); i += Step)
	{
		OutPoints.Add(SpawnFloors[i] + FVector(0.f, 0.f, 10.f));
	}
}

void UArenaRules_INV::GatherSpawnCandidates()
{
	bCandidatesGathered = true;
	AArenaLevelBuilder* Builder = GetLevelBuilder();
	if (Builder)
	{
		for (APlayerStart* Start : Builder->GetPlayerStarts())
		{
			FVector Floor;
			if (Start && Builder->TraceFloor(Start->GetActorLocation(), 400.f, Floor))
			{
				SpawnFloors.Add(Floor);
			}
		}
	}
	// Nav graph nodes: extra variety (and the fallback on maps with few markers)
	if (AArenaNavGraph* Nav = GetNavGraph())
	{
		const TArray<FArenaNavNode>& Nodes = Nav->GetNodes();
		const int32 Step = FMath::Max(1, Nodes.Num() / 120);
		for (int32 i = 0; i < Nodes.Num(); i += Step)
		{
			SpawnFloors.Add(Nodes[i].Location - FVector(0.f, 0.f, ArenaTuning::CapsuleHalfHeight));
		}
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("Invasion: %d monster spawn candidates"), SpawnFloors.Num());
}

void UArenaRules_INV::InitGameState(AArenaGameState* GS)
{
	if (WaveState)
	{
		WaveState->NumWaves = GetNumWaves();
	}
}

void UArenaRules_INV::OnStartPlay()
{
	GatherSpawnCandidates();
	UpdateWaveState();
}

void UArenaRules_INV::OnMatchStart()
{
	Phase = EWavePhase::Countdown;
	Wave = FMath::Clamp(StartWaveOption, 1, GetNumWaves()) - 1;
	NextWaveTime = GetWorld()->GetTimeSeconds() + ArenaINVRules::FirstWaveDelay;
	if (GameMode)
	{
		GameMode->BroadcastCenterMessage(TEXT("Hold the arena - the swarm is coming"), ArenaINVRules::WaveColor, 3.f);
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("Invasion: match started, %d waves"), GetNumWaves());
}

void UArenaRules_INV::OnMatchEnd(const FString& WinnerText, int32 WinningTeam, APlayerState* WinningPlayer)
{
	Phase = EWavePhase::Done;
	UE_LOG(LogOpenUT2k4, Log, TEXT("Invasion: match over after wave %d (%d monsters slain) - %s"), Wave, TotalKills, *WinnerText);
	UpdateWaveState();
}

void UArenaRules_INV::OnPlayerJoined(AController* Controller)
{
	if (AArenaPlayerState* PS = Controller ? Controller->GetPlayerState<AArenaPlayerState>() : nullptr)
	{
		PS->Lives = 1;
		PS->bEliminated = false;
	}
}

// =============================================================================================
// Team helpers
// =============================================================================================

void UArenaRules_INV::GetTeamControllers(TArray<AController*>& Out) const
{
	const AArenaGameState* GS = GetGameState();
	if (!GS)
	{
		return;
	}
	for (APlayerState* PS : GS->PlayerArray)
	{
		AController* C = PS ? Cast<AController>(PS->GetOwner()) : nullptr;
		if (C && !IsMonsterController(C) && !PS->IsOnlyASpectator())
		{
			Out.Add(C);
		}
	}
}

int32 UArenaRules_INV::CountAlivePlayers(int32* OutTotal) const
{
	TArray<AController*> Team;
	GetTeamControllers(Team);
	int32 Alive = 0;
	for (AController* C : Team)
	{
		const AArenaCharacter* P = Cast<AArenaCharacter>(C->GetPawn());
		if (P && P->IsAlive())
		{
			++Alive;
		}
	}
	if (OutTotal)
	{
		*OutTotal = Team.Num();
	}
	return Alive;
}

TArray<FVector> UArenaRules_INV::GetLivingPlayerEyes() const
{
	TArray<FVector> Eyes;
	TArray<AController*> Team;
	GetTeamControllers(Team);
	for (AController* C : Team)
	{
		const AArenaCharacter* P = Cast<AArenaCharacter>(C->GetPawn());
		if (P && P->IsAlive())
		{
			Eyes.Add(P->GetPawnViewLocation());
		}
	}
	return Eyes;
}

// =============================================================================================
// Waves
// =============================================================================================

void UArenaRules_INV::BuildWaveQueue(int32 InWave)
{
	SpawnQueue.Reset();
	TArray<AController*> Team;
	GetTeamControllers(Team);
	const int32 NumPlayers = FMath::Max(1, Team.Num());
	FRandomStream Rng(InWave * 1013 + FMath::Rand());

	// UT2004's wave table (Invasion.Waves): which monsters a wave has and how many may be alive.
	TArray<EArenaINVMonsterType> Types;
	int32 WaveMax = 12;
	ArenaINV::GetWave(InWave, Types, WaveMax);
	if (Types.Num() == 0)
	{
		Types.Add(EArenaINVMonsterType::Gnashling);
	}
	const bool bBossOnly = Types.Num() == 1 && ArenaINV::GetMonsterDef(Types[0]).bBoss;
	int32 Count = FMath::RoundToInt(5.f + 1.6f * InWave + 1.2f * (NumPlayers - 1));
	if (bBossOnly)
	{
		Count = FMath::Clamp(1 + InWave / 8 + NumPlayers / 3, 1, 6);
	}
	Count = FMath::Clamp(Count, 1, 70);
	// Bosses are rare in mixed waves (at most one per four monsters)
	int32 Bosses = 0;
	for (int32 i = 0; i < Count; ++i)
	{
		EArenaINVMonsterType Pick = Types[Rng.RandRange(0, Types.Num() - 1)];
		if (!bBossOnly && ArenaINV::GetMonsterDef(Pick).bBoss)
		{
			if (Bosses >= FMath::Max(1, Count / 8))
			{
				Pick = Types[0];
			}
			else
			{
				++Bosses;
			}
		}
		SpawnQueue.Add(Pick);
	}
	// Every type of the wave shows up at least once
	for (int32 i = 0; i < Types.Num() && SpawnQueue.Num() >= Types.Num(); ++i)
	{
		if (!SpawnQueue.Contains(Types[i]))
		{
			SpawnQueue[Rng.RandRange(0, SpawnQueue.Num() - 1)] = Types[i];
		}
	}
	MaxAlive = FMath::Clamp(FMath::Min(WaveMax, 4 + InWave / 2 + NumPlayers), bBossOnly ? 1 : 6, 18);
}

void UArenaRules_INV::StartWave(int32 NewWave)
{
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();

	// Anyone still down comes back now
	RespawnFallenPlayers();

	Wave = NewWave;
	Phase = EWavePhase::Active;
	BuildWaveQueue(Wave);
	WaveTotal = SpawnQueue.Num();
	NextSpawnTime = Now;
	LastKillTime = Now;
	WaveStartTime = Now;
	DeathTimes.Reset();

	TArray<AController*> Team;
	GetTeamControllers(Team);
	for (AController* C : Team)
	{
		if (AArenaPlayerState* PS = C->GetPlayerState<AArenaPlayerState>())
		{
			PS->Lives = 1;
			PS->bEliminated = false;
		}
	}

	const bool bBoss = SpawnQueue.Contains(EArenaINVMonsterType::Cindermaw);
	static_assert(int32(EArenaINVMonsterType::Count) == 12, "update the boss check");
	if (GameMode)
	{
		GameMode->BroadcastCenterMessage(bBoss ? FString::Printf(TEXT("Wave %d - a Cindermaw approaches!"), Wave) : FString::Printf(TEXT("Wave %d"), Wave),
			bBoss ? ArenaINVRules::BossColor : ArenaINVRules::WaveColor, 3.f, bBoss ? TEXT("S_Flag_Taken") : TEXT("S_Spree"));
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("Invasion: wave %d / %d started - %d monsters (max %d alive)%s"), Wave, GetNumWaves(), WaveTotal, MaxAlive, bBoss ? TEXT(", boss wave") : TEXT(""));
}

void UArenaRules_INV::FinishWave()
{
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();
	UE_LOG(LogOpenUT2k4, Log, TEXT("Invasion: wave %d cleared in %.0f s"), Wave, Now - WaveStartTime);

	if (Wave >= GetNumWaves())
	{
		Phase = EWavePhase::Done;
		APlayerState* Best = nullptr;
		if (const AArenaGameState* GS = GetGameState())
		{
			const TArray<AArenaPlayerState*> Sorted = GS->GetSortedPlayers();
			Best = Sorted.Num() > 0 ? Sorted[0] : nullptr;
		}
		if (GameMode)
		{
			GameMode->EndMatch(FString::Printf(TEXT("Invasion repelled! The team survived all %d waves"), Wave), -1, Best);
		}
		return;
	}

	Phase = EWavePhase::Countdown;
	NextWaveTime = Now + WaveDelay;
	LastCountdownSecond = -1.f;
	if (GameMode)
	{
		GameMode->BroadcastCenterMessage(FString::Printf(TEXT("Wave %d cleared!"), Wave), ArenaINVRules::WaveColor, 3.f, TEXT("S_Flag_Captured"));
	}
	RespawnFallenPlayers();
}

void UArenaRules_INV::RespawnFallenPlayers()
{
	if (!GameMode)
	{
		return;
	}
	TArray<AController*> Team;
	GetTeamControllers(Team);
	for (AController* C : Team)
	{
		AArenaPlayerState* PS = C->GetPlayerState<AArenaPlayerState>();
		if (PS)
		{
			PS->Lives = 1;
			PS->bEliminated = false;
		}
		const AArenaCharacter* P = Cast<AArenaCharacter>(C->GetPawn());
		if (!P || P->IsDead())
		{
			GameMode->RestartPlayer(C);
		}
	}
}

bool UArenaRules_INV::PickSpawnPoint(EArenaINVMonsterType Type, FVector& OutFloor, float& OutYaw)
{
	UWorld* World = GetWorld();
	AArenaLevelBuilder* Builder = GetLevelBuilder();
	const FArenaINVMonsterDef& Def = ArenaINV::GetMonsterDef(Type);
	const TArray<FVector> Eyes = GetLivingPlayerEyes();
	FRandomStream Rng(FMath::Rand());

	auto MinPlayerDist = [&Eyes](const FVector& P)
	{
		float Best = 1.0e9f;
		for (const FVector& E : Eyes)
		{
			Best = FMath::Min(Best, FVector::Dist(E, P));
		}
		return Best;
	};
	auto Visible = [&Eyes, World, &Def](const FVector& Floor)
	{
		const FVector Body = Floor + FVector(0.f, 0.f, Def.CapsuleHalfHeight);
		for (const FVector& E : Eyes)
		{
			if (FVector::DistSquared(E, Body) < FMath::Square(9000.f) && ArenaINVRules::IsPointVisible(World, E, Body))
			{
				return true;
			}
		}
		return false;
	};
	auto Facing = [&Eyes](const FVector& P)
	{
		// Face the nearest player
		float Best = 1.0e9f;
		FVector To = FVector::ForwardVector;
		for (const FVector& E : Eyes)
		{
			const float D = FVector::DistSquared(E, P);
			if (D < Best)
			{
				Best = D;
				To = E - P;
			}
		}
		return To.Rotation().Yaw;
	};
	auto Validate = [&](const FVector& Floor, FVector& OutCenterFloor) -> bool
	{
		FVector Found = Floor;
		if (Builder)
		{
			if (!Builder->TraceFloor(Floor + FVector(0.f, 0.f, 80.f), 300.f, Found))
			{
				return false;
			}
		}
		const float Lift = Def.bFlying ? 220.f : 0.f;
		const FVector Center = Found + FVector(0.f, 0.f, Def.CapsuleHalfHeight + 6.f + Lift);
		if (!ArenaINVRules::CapsuleFits(World, Center, Def.CapsuleRadius, Def.CapsuleHalfHeight))
		{
			// flyers: try at ground height instead
			if (!Def.bFlying || !ArenaINVRules::CapsuleFits(World, Found + FVector(0.f, 0.f, Def.CapsuleHalfHeight + 6.f), Def.CapsuleRadius, Def.CapsuleHalfHeight))
			{
				return false;
			}
			OutCenterFloor = Found;
			return true;
		}
		OutCenterFloor = Found + FVector(0.f, 0.f, Lift);
		return true;
	};

	// ---- far, hidden path nodes, player starts and nav nodes ----
	if (SpawnFloors.Num() == 0)
	{
		return false;
	}
	float BestScore = -1.0e9f;
	FVector BestFloor = FVector::ZeroVector;
	bool bFound = false;
	const int32 Samples = FMath::Min(SpawnFloors.Num(), 28);
	for (int32 i = 0; i < Samples; ++i)
	{
		const FVector& C = SpawnFloors[Rng.RandRange(0, SpawnFloors.Num() - 1)];
		const float Dist = MinPlayerDist(C);
		if (Dist < ArenaINVRules::MinSpawnDistance && Eyes.Num() > 0)
		{
			continue;
		}
		float Score = FMath::Min(Dist, 4500.f) - FMath::Max(0.f, Dist - 4500.f) * 0.3f + Rng.FRandRange(0.f, 800.f);
		if (Visible(C))
		{
			Score -= 5000.f;
		}
		if (Score <= BestScore)
		{
			continue;
		}
		FVector Floor;
		if (Validate(C, Floor))
		{
			BestScore = Score;
			BestFloor = Floor;
			bFound = true;
		}
	}
	if (bFound)
	{
		OutFloor = BestFloor;
		OutYaw = Facing(BestFloor);
	}
	return bFound;
}

bool UArenaRules_INV::TrySpawnMonster(EArenaINVMonsterType Type)
{
	UWorld* World = GetWorld();
	AArenaGameState* GS = GetGameState();
	FVector Floor;
	float Yaw = 0.f;
	if (!PickSpawnPoint(Type, Floor, Yaw))
	{
		return false;
	}
	const FArenaINVMonsterDef& Def = ArenaINV::GetMonsterDef(Type);
	const FTransform SpawnTM(FRotator(0.f, Yaw, 0.f), Floor + FVector(0.f, 0.f, Def.CapsuleHalfHeight + 6.f));

	TArray<AController*> Team;
	GetTeamControllers(Team);
	const int32 NumPlayers = FMath::Max(1, Team.Num());
	const float Skill = GameMode ? float(GameMode->GetBotSkill()) : 3.f;
	const float HealthScale = (1.f + 0.07f * (Wave - 1)) * (1.f + 0.08f * FMath::Max(0, NumPlayers - 4));
	const float DamageScale = (1.f + 0.04f * (Wave - 1)) * FMath::Lerp(0.45f, 1.05f, Skill / 7.f);

	// PickSpawnPoint checked that this type's capsule fits; the spawn-time test would use the default (player-sized)
	// capsule, which small monsters standing on the floor always fail.
	AArenaINV_Monster* Monster = World->SpawnActorDeferred<AArenaINV_Monster>(AArenaINV_Monster::StaticClass(), SpawnTM, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Monster)
	{
		return false;
	}
	Monster->InitMonster(Type, HealthScale, DamageScale);
	Monster->FinishSpawning(SpawnTM);
	if (!IsValid(Monster))
	{
		return false;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AArenaINV_MonsterController* Ctrl = World->SpawnActor<AArenaINV_MonsterController>(AArenaINV_MonsterController::StaticClass(), SpawnTM, Params);
	if (!Ctrl)
	{
		Monster->Destroy();
		return false;
	}
	// Hidden player state: gives kill messages a name ("Taken out by Slagbrute") but stays out of the
	// scoreboard / player list (inactive player states are not part of GameState->PlayerArray).
	Ctrl->InitPlayerState();
	if (AArenaPlayerState* PS = Ctrl->GetPlayerState<AArenaPlayerState>())
	{
		PS->SetIsInactive(true);
		PS->SetIsABot(true);
		PS->SetPlayerName(Def.Name);
		if (GS)
		{
			GS->RemovePlayerState(PS);
		}
	}
	Ctrl->Possess(Monster);
	Monsters.Add(Monster);
	++MonsterSerial;

	ArenaFX::SpawnExplosion(World, SpawnTM.GetLocation(), Def.GlowColor, Def.CapsuleHalfHeight * 1.5f, TEXT("Explosion.ShockCombo"));
	UE_LOG(LogOpenUT2k4, Log, TEXT("Invasion: spawned %s #%d (wave %d, hp %.0f) at %s"), Def.Name, MonsterSerial, Wave, Monster->Health, *SpawnTM.GetLocation().ToCompactString());
	return true;
}

void UArenaRules_INV::RecallStragglers(float Now)
{
	// Monsters that got lost (no movement, no hits) for a while reappear at a fresh spawn spot.
	const bool bWaveStalled = Now - LastKillTime > 40.f;
	for (const TWeakObjectPtr<AArenaINV_Monster>& Weak : Monsters)
	{
		AArenaINV_Monster* M = Weak.Get();
		AArenaINV_MonsterController* Ctrl = M ? Cast<AArenaINV_MonsterController>(M->GetController()) : nullptr;
		if (!Ctrl || M->IsDead())
		{
			continue;
		}
		const float Idle = Now - FMath::Max(Ctrl->GetLastProgressTime(), Ctrl->GetLastAttackTime());
		const float Quiet = Now - Ctrl->GetLastAttackTime();
		if (Idle > ArenaINVRules::StragglerTime || (bWaveStalled && Quiet > 30.f))
		{
			FVector Floor;
			float Yaw = 0.f;
			if (PickSpawnPoint(M->GetMonsterType(), Floor, Yaw))
			{
				const FVector Dest = Floor + FVector(0.f, 0.f, M->GetDef().CapsuleHalfHeight + 6.f);
				if (M->TeleportTo(Dest, FRotator(0.f, Yaw, 0.f)))
				{
					UE_LOG(LogOpenUT2k4, Log, TEXT("Invasion: recalled stuck %s to %s"), M->GetDef().Name, *Dest.ToCompactString());
					ArenaFX::SpawnExplosion(GetWorld(), Dest, M->GetDef().GlowColor, 120.f, TEXT("Explosion.ShockCombo"));
				}
			}
			Ctrl->ResetAfterTeleport();
		}
	}
	if (bWaveStalled)
	{
		LastKillTime = Now - 10.f; // re-check in 30 s, not every tick
	}
}

void UArenaRules_INV::UpdateSpectators(float Now)
{
	TArray<AController*> Team;
	GetTeamControllers(Team);
	TArray<APawn*> Alive;
	for (AController* C : Team)
	{
		AArenaCharacter* P = Cast<AArenaCharacter>(C->GetPawn());
		if (P && P->IsAlive())
		{
			Alive.Add(P);
		}
	}
	if (Alive.Num() == 0)
	{
		return;
	}
	for (AController* C : Team)
	{
		APlayerController* PC = Cast<APlayerController>(C);
		const AArenaPlayerState* PS = C->GetPlayerState<AArenaPlayerState>();
		if (!PC || !PS || !PS->bEliminated)
		{
			continue;
		}
		const float* DiedAt = DeathTimes.Find(C);
		if (DiedAt && Now - *DiedAt < ArenaINVRules::SpectateDelay)
		{
			continue;
		}
		const AArenaCharacter* View = Cast<AArenaCharacter>(PC->GetViewTarget());
		if (View && View->IsAlive() && !View->IsA<AArenaINV_Monster>())
		{
			continue;
		}
		APawn* Follow = Alive[FMath::RandRange(0, Alive.Num() - 1)];
		PC->SetViewTargetWithBlend(Follow, 0.4f);
	}
}

void UArenaRules_INV::Tick(float DeltaSeconds)
{
	AArenaGameState* GS = GetGameState();
	UWorld* World = GetWorld();
	if (!GS || !World || !GameMode)
	{
		return;
	}
	const float Now = World->GetTimeSeconds();
	ArenaINV::TickDebugShowcase(World);
	if (ArenaINV::IsShowcase())
	{
		return;
	}

	if (GS->Phase == EArenaMatchPhase::Playing)
	{
		switch (Phase)
		{
		case EWavePhase::Countdown:
		{
			const float Left = NextWaveTime - Now;
			const float Sec = FMath::CeilToFloat(Left);
			if (Sec != LastCountdownSecond && Sec >= 1.f && Sec <= 3.f)
			{
				GameMode->BroadcastCenterMessage(FString::Printf(TEXT("Wave %d in %d"), Wave + 1, int32(Sec)), FLinearColor::White, 1.f, TEXT("S_Countdown"));
			}
			LastCountdownSecond = Sec;
			if (Left <= 0.f)
			{
				StartWave(Wave + 1);
			}
			break;
		}
		case EWavePhase::Active:
		{
			Monsters.RemoveAll([](const TWeakObjectPtr<AArenaINV_Monster>& M) { return !M.IsValid() || M->IsDead(); });
			while (SpawnQueue.Num() > 0 && Monsters.Num() < MaxAlive && Now >= NextSpawnTime)
			{
				if (TrySpawnMonster(SpawnQueue[0]))
				{
					SpawnQueue.RemoveAt(0);
					NextSpawnTime = Now + ArenaINVRules::SpawnInterval;
				}
				else
				{
					// No room for this one right now (big monsters need a big free spot): try the next in line first.
					static int32 Reported = 0;
					if (Reported++ < 20)
					{
						UE_LOG(LogOpenUT2k4, Log, TEXT("Invasion: no spawn spot for %s yet"), ArenaINV::GetMonsterDef(SpawnQueue[0]).Name);
					}
					if (SpawnQueue.Num() > 1)
					{
						const EArenaINVMonsterType Waiting = SpawnQueue[0];
						SpawnQueue.RemoveAt(0);
						SpawnQueue.Add(Waiting);
					}
					NextSpawnTime = Now + 0.25f;
				}
				break; // one per tick at most
			}
			if (SpawnQueue.Num() == 0 && Monsters.Num() == 0)
			{
				FinishWave();
				break;
			}
			int32 Total = 0;
			if (CountAlivePlayers(&Total) == 0 && Total > 0)
			{
				CheckEndConditions();
				break;
			}
			RecallStragglers(Now);
			UpdateSpectators(Now);
			break;
		}
		default:
			break;
		}
	}
	UpdateWaveState();
}

void UArenaRules_INV::UpdateWaveState()
{
	if (!WaveState)
	{
		return;
	}
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	int32 Alive = 0;
	float BossFrac = -1.f;
	FString BossName;
	for (const TWeakObjectPtr<AArenaINV_Monster>& Weak : Monsters)
	{
		const AArenaINV_Monster* M = Weak.Get();
		if (M && M->IsAlive())
		{
			++Alive;
			if (M->IsBoss() && BossFrac < 0.f)
			{
				BossFrac = FMath::Clamp(M->Health / FMath::Max(1.f, M->GetMaxHealth()), 0.f, 1.f);
				BossName = M->GetDef().Name;
			}
		}
	}
	int32 Total = 0;
	WaveState->Wave = Wave;
	WaveState->NumWaves = GetNumWaves();
	WaveState->bWaveActive = Phase == EWavePhase::Active;
	WaveState->Countdown = Phase == EWavePhase::Countdown ? FMath::Max(0, FMath::CeilToInt(NextWaveTime - Now)) : 0;
	WaveState->MonstersRemaining = Phase == EWavePhase::Active ? Alive + SpawnQueue.Num() : 0;
	WaveState->MonstersTotal = WaveTotal;
	WaveState->PlayersAlive = CountAlivePlayers(&Total);
	WaveState->PlayersTotal = Total;
	WaveState->BossHealth = BossFrac;
	WaveState->BossName = BossName;
	{
		TArray<EArenaINVMonsterType> NextTypes;
		int32 NextMax = 0;
		ArenaINV::GetWave(Wave + 1, NextTypes, NextMax);
		WaveState->bNextWaveBoss = Phase == EWavePhase::Countdown && NextTypes.Contains(EArenaINVMonsterType::Cindermaw);
	}
}

// =============================================================================================
// Spawning / inventory
// =============================================================================================

AActor* UArenaRules_INV::ChoosePlayerStart(AController* Player, const TArray<APlayerStart*>& Candidates)
{
	UWorld* World = GetWorld();
	APlayerStart* Best = nullptr;
	float BestScore = -1.0e9f;
	for (APlayerStart* Start : Candidates)
	{
		if (!Start)
		{
			continue;
		}
		const FVector Loc = Start->GetActorLocation();
		float NearestMonster = 6000.f;
		bool bBlocked = false;
		for (TActorIterator<AArenaCharacter> It(World); It; ++It)
		{
			if (!It->IsAlive())
			{
				continue;
			}
			const float Dist = FVector::Dist(It->GetActorLocation(), Loc);
			if (Dist < 120.f)
			{
				bBlocked = true;
			}
			if (It->IsA<AArenaINV_Monster>())
			{
				NearestMonster = FMath::Min(NearestMonster, Dist);
			}
		}
		float Score = NearestMonster + FMath::FRandRange(0.f, 900.f) - (bBlocked ? 100000.f : 0.f);
		if (Score > BestScore)
		{
			BestScore = Score;
			Best = Start;
		}
	}
	return Best;
}

bool UArenaRules_INV::CanRespawn(AController* Controller) const
{
	if (IsMonsterController(Controller))
	{
		return false;
	}
	if (Phase == EWavePhase::Done)
	{
		return false;
	}
	if (Phase == EWavePhase::Active)
	{
		const AArenaPlayerState* PS = Controller ? Controller->GetPlayerState<AArenaPlayerState>() : nullptr;
		return PS && !PS->bEliminated;
	}
	return true;
}

float UArenaRules_INV::GetForcedRespawnDelay(AController* Controller) const
{
	return Cast<AArenaBotController>(Controller) ? 0.f : 2.f;
}

void UArenaRules_INV::ModifyDefaultInventory(AController* Controller, TArray<TSubclassOf<AArenaWeapon>>& Weapons) const
{
	if (IsMonsterController(Controller))
	{
		Weapons.Empty(); // monsters fight with their own attacks
	}
	else if (GameMode && !GameMode->IsInstagib())
	{
		Weapons.AddUnique(AWeap_PulseRifle::StaticClass()); // the team starts a little better armed against swarms
	}
}

bool UArenaRules_INV::CanPickUp(AArenaCharacter* Character, AArenaPickup* Pickup) const
{
	return Character && !Character->IsA<AArenaINV_Monster>();
}

// =============================================================================================
// Combat / scoring
// =============================================================================================

bool UArenaRules_INV::AreOnSameTeam(const AController* A, const AController* B) const
{
	if (!A || !B)
	{
		return false;
	}
	return IsMonsterController(A) == IsMonsterController(B);
}

float UArenaRules_INV::ModifyDamage(float Damage, AController* InstigatorController, AArenaCharacter* Victim) const
{
	if (!GameMode || !GameMode->IsPlaying() || !Victim)
	{
		return 0.f;
	}
	const bool bInstigatorMonster = IsMonsterController(InstigatorController);
	const bool bSelf = InstigatorController && InstigatorController == Victim->GetController();

	if (Victim->IsA<AArenaINV_Monster>())
	{
		if (bInstigatorMonster)
		{
			return 0.f; // no infighting (momentum still applies)
		}
		if (InstigatorController && GameMode->IsInstagib())
		{
			return 200.f;
		}
		return Damage;
	}

	// Victim is on the human side
	if (Victim->HasSpawnProtection())
	{
		return 0.f;
	}
	if (bInstigatorMonster || !InstigatorController)
	{
		return Damage;
	}
	if (bSelf)
	{
		return GameMode->IsInstagib() ? 0.f : Damage;
	}
	return 0.f; // team mates
}

bool UArenaRules_INV::ScoreKill(AController* Killer, AController* Victim, bool bHeadshot)
{
	if (IsMonsterController(Victim))
	{
		const AArenaINV_Monster* M = Victim ? Cast<AArenaINV_Monster>(Victim->GetPawn()) : nullptr;
		AArenaPlayerState* KillerPS = Killer ? Killer->GetPlayerState<AArenaPlayerState>() : nullptr;
		if (M && KillerPS && !IsMonsterController(Killer))
		{
			const int32 Points = ArenaINVRules::PointsFor(M->GetMonsterType());
			KillerPS->Kills++;
			KillerPS->AddScore(float(Points));
			if (GameMode)
			{
				GameMode->AddTeamScore(0, Points);
			}
			if (AArenaCharacter* KillerPawn = Cast<AArenaCharacter>(Killer->GetPawn()))
			{
				KillerPawn->GiveAdrenaline(ArenaTuning::AdrenalinePerKill);
			}
			if (M->IsBoss() && GameMode)
			{
				GameMode->BroadcastCenterMessage(FString::Printf(TEXT("%s toppled the %s!"), *KillerPS->GetPlayerName(), M->GetDef().Name),
					ArenaINVRules::BossColor, 3.f, TEXT("S_Spree"));
			}
		}
		return true;
	}

	// A team member went down
	if (AArenaPlayerState* VictimPS = Victim ? Victim->GetPlayerState<AArenaPlayerState>() : nullptr)
	{
		VictimPS->Deaths++;
		VictimPS->Spree = 0;
		if (!Killer || Killer == Victim)
		{
			VictimPS->Suicides++;
		}
	}
	return true;
}

void UArenaRules_INV::OnKilled(AController* Killer, AController* Victim, AArenaCharacter* VictimPawn, const FString& WeaponName, bool bHeadshot)
{
	UWorld* World = GetWorld();
	const float Now = World->GetTimeSeconds();
	if (AArenaINV_Monster* M = Cast<AArenaINV_Monster>(VictimPawn))
	{
		++TotalKills;
		LastKillTime = Now;
		Monsters.Remove(M);
		const APlayerState* KPS = Killer ? Killer->GetPlayerState<APlayerState>() : nullptr;
		UE_LOG(LogOpenUT2k4, Log, TEXT("Invasion: %s slain by %s [%s] - wave %d, %d remaining"), M->GetDef().Name,
			KPS ? *KPS->GetPlayerName() : TEXT("?"), *WeaponName, Wave, Monsters.Num() + SpawnQueue.Num());
		return;
	}

	AArenaPlayerState* PS = Victim ? Victim->GetPlayerState<AArenaPlayerState>() : nullptr;
	const APlayerState* KPS = Killer ? Killer->GetPlayerState<APlayerState>() : nullptr;
	UE_LOG(LogOpenUT2k4, Log, TEXT("Invasion: player %s killed by %s [%s] on wave %d"), PS ? *PS->GetPlayerName() : TEXT("?"),
		KPS ? *KPS->GetPlayerName() : TEXT("?"), *WeaponName, Wave);
	if (Phase != EWavePhase::Active || !PS)
	{
		return; // between waves: normal respawn
	}
	PS->Lives = 0;
	PS->bEliminated = true;
	DeathTimes.Add(Victim, Now);
	if (GameMode)
	{
		GameMode->CancelRespawn(Victim);
		int32 Total = 0;
		const int32 Alive = CountAlivePlayers(&Total);
		if (Alive == 1)
		{
			GameMode->BroadcastCenterMessage(TEXT("Last one standing!"), ArenaINVRules::DangerColor, 2.5f, TEXT("S_Countdown"));
		}
		GameMode->SendCenterMessage(Victim, TEXT("You are down - you return when the wave is cleared"), ArenaINVRules::DangerColor, 4.f);
	}
}

void UArenaRules_INV::CheckEndConditions()
{
	AArenaGameState* GS = GetGameState();
	if (!GameMode || !GS || !GS->IsPlaying() || Phase != EWavePhase::Active)
	{
		return;
	}
	int32 Total = 0;
	if (CountAlivePlayers(&Total) == 0 && Total > 0)
	{
		Phase = EWavePhase::Done;
		UE_LOG(LogOpenUT2k4, Log, TEXT("Invasion: team wiped out on wave %d"), Wave);
		GameMode->EndMatch(FString::Printf(TEXT("The swarm overran the team on wave %d"), Wave), -1, nullptr);
	}
}

void UArenaRules_INV::OnTimeLimitReached()
{
	if (GameMode && Phase != EWavePhase::Done)
	{
		Phase = EWavePhase::Done;
		GameMode->EndMatch(FString::Printf(TEXT("Time ran out on wave %d"), Wave), -1, nullptr);
	}
}

// =============================================================================================
// Bots
// =============================================================================================

bool UArenaRules_INV::GetBotObjective(AArenaBotController* Bot, FArenaBotObjective& OutGoal)
{
	if (Phase != EWavePhase::Active || !Bot)
	{
		return false; // between waves: collect items
	}
	const AArenaCharacter* P = Bot->GetArenaPawn();
	if (!P)
	{
		return false;
	}
	AArenaINV_Monster* Nearest = nullptr;
	float BestDist = TNumericLimits<float>::Max();
	for (const TWeakObjectPtr<AArenaINV_Monster>& Weak : Monsters)
	{
		AArenaINV_Monster* M = Weak.Get();
		if (M && M->IsAlive())
		{
			const float D = FVector::DistSquared(M->GetActorLocation(), P->GetActorLocation());
			if (D < BestDist)
			{
				BestDist = D;
				Nearest = M;
			}
		}
	}
	if (!Nearest)
	{
		return false;
	}
	OutGoal = FArenaBotObjective(Nearest->GetActorLocation(), Nearest, 0.55f, EArenaBotAction::Attack);
	OutGoal.bTrackActor = true;
	return true;
}

float UArenaRules_INV::GetBotEnemyWeight(AArenaBotController* Bot, AArenaCharacter* Candidate) const
{
	if (const AArenaINV_Monster* M = Cast<AArenaINV_Monster>(Candidate))
	{
		// Stingers and spitters chip from range: deal with them; the titan is worth focusing too
		switch (M->GetBehavior())
		{
		case EArenaINVBehavior::Boss:    return 0.8f;
		case EArenaINVBehavior::Flyer:   return 0.85f;
		case EArenaINVBehavior::Spitter: return 0.9f;
		default: return 1.f;
		}
	}
	return 1.f;
}

// =============================================================================================
// HUD
// =============================================================================================

bool UArenaRules_INV::DrawScoreInfo(AArenaHUD* HUD, UCanvas* Canvas)
{
	const AArenaGameState* GS = GetGameState();
	const AArenaINV_WaveState* WS = AArenaINV_WaveState::Get(HUD);
	if (!HUD || !GS || !WS)
	{
		return false;
	}
	if (HUD->IsUTHud())
	{
		// HudInvasion: the monster radar instead of the team scores
		TArray<FVector> RadarDots;
		for (TActorIterator<AArenaINV_Monster> It(HUD->GetWorld()); It; ++It)
		{
			if (It->IsAlive())
			{
				RadarDots.Add(It->GetActorLocation());
			}
		}
		HUD->DrawUTInvasionRadar(RadarDots);
		return true;
	}
	const FArenaDraw& D = HUD->GetDraw();
	const float PW = D.S(270.f);
	const float PH = D.S(124.f);
	const float Right = D.W() - D.S(24.f);
	const float X = Right - PW;
	const float Top = D.S(24.f);
	const FLinearColor Accent = WS->bWaveActive ? ArenaINVRules::WaveColor : ArenaHudStyle::AccentBlue();
	D.Panel(X, Top, PW, PH, Accent);

	const float L = X + D.S(18.f);
	const float R = Right - D.S(16.f);
	D.Text(TEXT("WAVE"), L, Top + D.S(10.f), ArenaHudStyle::TextDim(), D.S(14.f));
	D.Text(FString::Printf(TEXT("%d / %d"), FMath::Max(WS->Wave, WS->bWaveActive ? 1 : WS->Wave), WS->NumWaves), L, Top + D.S(26.f), ArenaHudStyle::TextWhite(), D.S(36.f));

	if (WS->bWaveActive)
	{
		const float LabelW = D.Text(TEXT("MONSTERS"), R, Top + D.S(10.f), ArenaHudStyle::TextDim(), D.S(14.f), EArenaAlign::Right);
		ArenaArtHUD::DrawSpriteCentered(D, ArenaArtHUD::Sprites::Skull(), R - LabelW - D.S(14.f), Top + D.S(18.f), D.S(20.f), FLinearColor(1.f, 1.f, 1.f, 0.85f));
		D.Text(FString::FromInt(WS->MonstersRemaining), R, Top + D.S(26.f), ArenaINVRules::DangerColor, D.S(36.f), EArenaAlign::Right);
		const float Frac = WS->MonstersTotal > 0 ? 1.f - float(WS->MonstersRemaining) / float(WS->MonstersTotal) : 0.f;
		D.Bar(L, Top + D.S(72.f), PW - D.S(34.f), D.S(6.f), Frac, ArenaINVRules::WaveColor);
	}
	else
	{
		D.Text(TEXT("NEXT WAVE"), R, Top + D.S(10.f), ArenaHudStyle::TextDim(), D.S(14.f), EArenaAlign::Right);
		D.Text(GS->IsPlaying() ? FString::Printf(TEXT("%d s"), WS->Countdown) : FString(TEXT("--")), R, Top + D.S(26.f),
			WS->bNextWaveBoss ? ArenaINVRules::BossColor : ArenaHudStyle::ShieldColor(), D.S(36.f), EArenaAlign::Right);
	}

	// Team lives: one pip per team member, lit while alive
	const float RowY = Top + D.S(86.f);
	D.Text(FString::Printf(TEXT("TEAM %d / %d"), WS->PlayersAlive, WS->PlayersTotal), L, RowY, ArenaHudStyle::TextDim(), D.S(14.f));
	const float PipSize = D.S(10.f);
	float PipX = L + D.S(96.f);
	for (int32 i = 0; i < WS->PlayersTotal && i < 12; ++i)
	{
		const bool bAlive = i < WS->PlayersAlive;
		D.Rect(PipX, RowY + D.S(3.f), PipSize, PipSize, bAlive ? ArenaHudStyle::HealthColor() : ArenaHudStyle::WithAlpha(ArenaINVRules::DangerColor, 0.5f));
		PipX += PipSize + D.S(4.f);
	}
	D.Text(FString::Printf(TEXT("%d pts"), GS->TeamScores[0]), R, RowY, ArenaHudStyle::TextWhite(), D.S(14.f), EArenaAlign::Right);
	return true;
}

void UArenaRules_INV::DrawHUD(AArenaHUD* HUD, UCanvas* Canvas)
{
	const AArenaGameState* GS = GetGameState();
	const AArenaINV_WaveState* WS = AArenaINV_WaveState::Get(HUD);
	if (!HUD || !GS || !WS || GS->Phase == EArenaMatchPhase::Ended)
	{
		return;
	}
	const FArenaDraw& D = HUD->GetDraw();
	const float CX = D.W() * 0.5f;
	if (HUD->IsUTHud())
	{
		// the countdown between waves in the style of UT2004's critical event messages (0,160,255, font size 1, 83 % height)
		if (GS->IsPlaying() && !WS->bWaveActive && WS->Countdown > 0)
		{
			const int32 Font = ArenaHudFonts::FontSizeIndex(D.UTClipX(), 1);
			const FString Text = FString::Printf(TEXT("Wave %d in %d"), WS->Wave + 1, WS->Countdown);
			const FVector2D S = D.TextSizeUT(Font, Text);
			D.TextUT(Font, Text, CX - S.X * 0.5f, HUD->GetUTFrame().ScreenY(0.83f) - S.Y * 1.5f, FArenaDraw::UTColor(FColor(0, 160, 255, 255)));
		}
		return;
	}

	// Countdown between waves
	if (GS->IsPlaying() && !WS->bWaveActive && WS->Countdown > 0)
	{
		// Ends just above the centre message area (which starts at 24 % of the height)
		const float SubY = D.H() * 0.24f - D.S(26.f);
		const float Y = SubY - D.S(42.f);
		D.Text(FString::Printf(TEXT("WAVE %d IN %d"), WS->Wave + 1, WS->Countdown), CX, Y, ArenaHudStyle::TextWhite(), D.S(34.f), EArenaAlign::Center);
		const FString Sub = WS->bNextWaveBoss ? FString(TEXT("Boss wave - a Cindermaw is coming")) : FString(TEXT("Grab weapons and armour while you can"));
		D.Text(Sub, CX, SubY, WS->bNextWaveBoss ? ArenaINVRules::BossColor : ArenaHudStyle::TextDim(), D.S(16.f), EArenaAlign::Center);
	}

	// Boss health bar under the timer
	if (WS->BossHealth >= 0.f)
	{
		const float BW = D.S(520.f);
		const float BX = CX - BW * 0.5f;
		const float BY = D.S(100.f);
		D.Panel(BX - D.S(8.f), BY - D.S(6.f), BW + D.S(16.f), D.S(40.f), ArenaINVRules::BossColor);
		D.Text(WS->BossName.ToUpper(), CX, BY - D.S(2.f), ArenaINVRules::BossColor, D.S(13.f), EArenaAlign::Center);
		D.Bar(BX, BY + D.S(16.f), BW, D.S(10.f), WS->BossHealth, ArenaINVRules::BossColor);
	}
}

FString UArenaRules_INV::GetScoreboardExtra(const AArenaPlayerState* Player) const
{
	return Player && Player->bEliminated ? FString(TEXT("DOWN")) : FString();
}

FString UArenaRules_INV::GetScoreboardHeaderExtra(const AArenaGameState* GS) const
{
	const AArenaINV_WaveState* WS = AArenaINV_WaveState::Get(GS);
	if (!WS || !GS)
	{
		return FString();
	}
	return FString::Printf(TEXT("Wave %d / %d   |   Team points %d"), WS->Wave, WS->NumWaves, GS->TeamScores[0]);
}

FString UArenaRules_INV::GetDeadScreenMessage(const AArenaPlayerState* Player) const
{
	if (Player && Player->bEliminated)
	{
		return TEXT("Down for this wave - back when it is cleared");
	}
	return TEXT("Press FIRE to respawn");
}
