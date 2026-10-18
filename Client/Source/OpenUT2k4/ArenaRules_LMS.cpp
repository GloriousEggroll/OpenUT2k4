#include "ArenaRules_LMS.h"
#include "OpenUT2k4.h"
#include "ArenaLMS_Beacon.h"
#include "ArenaGameMode.h"
#include "ArenaGameState.h"
#include "ArenaPlayerState.h"
#include "ArenaCharacter.h"
#include "ArenaBotController.h"
#include "ArenaHUD.h"
#include "ArenaArtHUD.h"
#include "ArenaPickup.h"
#include "ArenaWeapon.h"
#include "ArenaWeapons.h"
#include "ArenaAssets.h"
#include "Engine/Canvas.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"

ARENA_REGISTER_RULES(UArenaRules_LMS, "LMS", "Last Man Standing");

namespace ArenaLMS
{
	constexpr float SpawnHealth = 100.f;
	constexpr float SpawnShield = 100.f;
	/** Weapons are topped up to this fraction of their max ammo on spawn. */
	constexpr float SpawnAmmoFraction = 0.75f;
	/** Warn this many seconds before a camper is exposed. */
	constexpr float CampWarnLead = 10.f;
	/** Eliminated humans keep looking at their wreck this long before the camera moves on. */
	constexpr float SpectateSwitchDelay = 2.5f;

	enum EObjectiveTag : int32
	{
		TagHunt = 1,
		TagCamper = 2
	};

	FLinearColor LivesColor() { return FLinearColor(1.f, 0.82f, 0.3f); }
	FLinearColor OutColor() { return FLinearColor(1.f, 0.35f, 0.3f); }

	/** World marker: ring + label + distance when on screen, an arrow at the screen edge otherwise. */
	void DrawWorldMarker(AArenaHUD* HUD, UCanvas* Canvas, const FVector& WorldLoc, const FString& Label, const FLinearColor& Color)
	{
		APlayerController* PC = HUD ? HUD->GetOwningPlayerController() : nullptr;
		if (!PC || !Canvas)
		{
			return;
		}
		const FArenaDraw& D = HUD->GetDraw();
		FVector CamLoc;
		FRotator CamRot;
		PC->GetPlayerViewPoint(CamLoc, CamRot);
		const FVector Dir = WorldLoc - CamLoc;
		const int32 Meters = FMath::RoundToInt(Dir.Size() / 100.f);
		const FString Text = FString::Printf(TEXT("%s  %d m"), *Label, Meters);
		const float Margin = D.S(60.f);

		bool bOnScreen = false;
		FVector2D Screen(0.f, 0.f);
		if (FVector::DotProduct(Dir, CamRot.Vector()) > 10.f)
		{
			const FVector P = Canvas->Project(WorldLoc);
			Screen = FVector2D(P.X, P.Y);
			bOnScreen = Screen.X >= Margin && Screen.X <= D.W() - Margin && Screen.Y >= Margin && Screen.Y <= D.H() - Margin;
		}

		const float Pulse = 0.75f + 0.25f * FMath::Sin(HUD->Now() * 6.f);
		const FLinearColor C = ArenaHudStyle::WithAlpha(Color, Pulse);
		if (bOnScreen)
		{
			D.Arc(Screen.X, Screen.Y, D.S(9.f), D.S(13.f), 0.f, 2.f * PI, C, 24);
			D.Line(Screen.X, Screen.Y - D.S(24.f), Screen.X, Screen.Y - D.S(15.f), C, D.S(2.f));
			D.Text(Text, Screen.X, Screen.Y - D.S(46.f), C, D.S(15.f), EArenaAlign::Center);
			return;
		}

		// Off screen (or behind): arrow on the screen border pointing towards the target
		const FRotationMatrix M(CamRot);
		FVector2D V(FVector::DotProduct(Dir, M.GetUnitAxis(EAxis::Y)), -FVector::DotProduct(Dir, M.GetUnitAxis(EAxis::Z)));
		if (V.SizeSquared() < 1.f)
		{
			V = FVector2D(0.f, 1.f);
		}
		V.Normalize();
		const FVector2D Center(D.W() * 0.5f, D.H() * 0.5f);
		const float HalfW = D.W() * 0.5f - Margin;
		const float HalfH = D.H() * 0.5f - Margin;
		const float T = FMath::Min(FMath::Abs(V.X) > KINDA_SMALL_NUMBER ? HalfW / FMath::Abs(V.X) : BIG_NUMBER,
			FMath::Abs(V.Y) > KINDA_SMALL_NUMBER ? HalfH / FMath::Abs(V.Y) : BIG_NUMBER);
		const FVector2D Pos = Center + V * T;
		const FVector2D Perp(-V.Y, V.X);
		const FVector2D Tip = Pos + V * D.S(16.f);
		const FVector2D A = Pos + Perp * D.S(11.f);
		const FVector2D B = Pos - Perp * D.S(11.f);
		const float Th = D.S(3.f);
		D.Line(Tip.X, Tip.Y, A.X, A.Y, C, Th);
		D.Line(Tip.X, Tip.Y, B.X, B.Y, C, Th);
		D.Line(A.X, A.Y, B.X, B.Y, C, Th);
		const FVector2D TextPos = Pos - V * D.S(34.f);
		D.TextMid(Text, TextPos.X, TextPos.Y, C, D.S(14.f), EArenaAlign::Center);
	}
}

// =============================================================================================
// Identity / lifecycle
// =============================================================================================

FString UArenaRules_LMS::GetGoalText(int32 Goal) const
{
	const int32 Lives = FMath::Max(1, Goal);
	return Lives == 1 ? FString(TEXT("1 life each, last one alive wins")) : FString::Printf(TEXT("%d lives each, last one alive wins"), Lives);
}

void UArenaRules_LMS::InitRules(AArenaGameMode* InGameMode, const FString& Options)
{
	Super::InitRules(InGameMode, Options);
	bAllowAllPickups = UGameplayStatics::GetIntOption(Options, TEXT("LMSPickups"), 0) != 0;
	CampTime = float(UGameplayStatics::GetIntOption(Options, TEXT("CampTime"), 30));
	CampRadius = float(FMath::Max(100, UGameplayStatics::GetIntOption(Options, TEXT("CampRadius"), 600)));
	UE_LOG(LogOpenUT2k4, Log, TEXT("LMS: pickups %s, camp check %.0f s within %.0f cm"), bAllowAllPickups ? TEXT("all") : TEXT("adrenaline only"), CampTime, CampRadius);
}

int32 UArenaRules_LMS::GetStartLives() const
{
	int32 Goal = GetGoalScore();
	if (Goal <= 0 && GameMode)
	{
		Goal = GameMode->GetGoalScore();
	}
	return FMath::Max(1, Goal > 0 ? Goal : GetDefaultGoalScore());
}

TArray<AArenaPlayerState*> UArenaRules_LMS::GetParticipants() const
{
	TArray<AArenaPlayerState*> Result;
	if (const AArenaGameState* GS = GetGameState())
	{
		for (APlayerState* P : GS->PlayerArray)
		{
			AArenaPlayerState* PS = Cast<AArenaPlayerState>(P);
			if (PS && !PS->IsOnlyASpectator() && PS->Lives >= 0)
			{
				Result.Add(PS);
			}
		}
	}
	return Result;
}

AArenaPlayerState* UArenaRules_LMS::PickLeader(const TArray<AArenaPlayerState*>& Players)
{
	AArenaPlayerState* Best = nullptr;
	for (AArenaPlayerState* PS : Players)
	{
		if (!PS)
		{
			continue;
		}
		if (!Best
			|| PS->Lives > Best->Lives
			|| (PS->Lives == Best->Lives && PS->Kills > Best->Kills)
			|| (PS->Lives == Best->Lives && PS->Kills == Best->Kills && PS->Deaths < Best->Deaths))
		{
			Best = PS;
		}
	}
	return Best;
}

void UArenaRules_LMS::OnPlayerJoined(AController* Controller)
{
	AArenaPlayerState* PS = Controller ? Controller->GetPlayerState<AArenaPlayerState>() : nullptr;
	if (!PS)
	{
		return;
	}
	const AArenaGameState* GS = GetGameState();
	int32 Lives = GetStartLives();
	if (GS && GS->Phase != EArenaMatchPhase::Countdown)
	{
		// Late joiner: as many lives as the weakest survivor, so they can't outlast everyone for free
		int32 MinLives = MAX_int32;
		for (const AArenaPlayerState* Other : GetParticipants())
		{
			if (Other != PS && !Other->bEliminated)
			{
				MinLives = FMath::Min(MinLives, Other->Lives);
			}
		}
		if (MinLives != MAX_int32)
		{
			Lives = FMath::Clamp(MinLives, 1, Lives);
		}
	}
	PS->Lives = Lives;
	PS->bEliminated = false;
}

void UArenaRules_LMS::OnPlayerLeft(AController* Controller)
{
	EliminatedAt.Remove(Controller);
	if (GameMode && GameMode->IsPlaying())
	{
		// The leaver may have been one of the last two
		GameMode->CheckEndConditions();
	}
}

void UArenaRules_LMS::OnMatchStart()
{
	const int32 Lives = GetStartLives();
	if (const AArenaGameState* GS = GetGameState())
	{
		for (APlayerState* P : GS->PlayerArray)
		{
			if (AArenaPlayerState* PS = Cast<AArenaPlayerState>(P))
			{
				if (!PS->IsOnlyASpectator())
				{
					PS->Lives = Lives;
					PS->bEliminated = false;
					PS->ModeFlags &= ~FlagExposed;
				}
			}
		}
	}
	CampStates.Reset();
	UE_LOG(LogOpenUT2k4, Log, TEXT("LMS: match started, %d lives each, %d players"), Lives, GetParticipants().Num());
}

void UArenaRules_LMS::OnMatchEnd(const FString& WinnerText, int32 WinningTeam, APlayerState* WinningPlayer)
{
	for (auto& Pair : CampStates)
	{
		if (AArenaLMS_Beacon* Beacon = Pair.Value.Beacon.Get())
		{
			Beacon->Destroy();
		}
		if (AArenaPlayerState* PS = Pair.Value.PlayerState.Get())
		{
			PS->ModeFlags &= ~FlagExposed;
		}
	}
	CampStates.Reset();
	UE_LOG(LogOpenUT2k4, Log, TEXT("LMS: match over: %s"), *WinnerText);
}

// =============================================================================================
// Spawning / inventory
// =============================================================================================

bool UArenaRules_LMS::CanRespawn(AController* Controller) const
{
	const AArenaPlayerState* PS = Controller ? Controller->GetPlayerState<AArenaPlayerState>() : nullptr;
	return !(PS && (PS->bEliminated || PS->Lives == 0));
}

void UArenaRules_LMS::ModifyDefaultInventory(AController* Controller, TArray<TSubclassOf<AArenaWeapon>>& Weapons) const
{
	if (GameMode && GameMode->IsInstagib())
	{
		return; // instagib: the rifle is all you get
	}
	for (const TSubclassOf<AArenaWeapon>& W : ArenaWeaponList::AllWeapons())
	{
		if (W)
		{
			Weapons.AddUnique(W);
		}
	}
}

void UArenaRules_LMS::OnPawnSpawned(AArenaCharacter* Character)
{
	if (!Character)
	{
		return;
	}
	Character->GiveHealth(ArenaLMS::SpawnHealth, ArenaLMS::SpawnHealth);
	Character->GiveShield(ArenaLMS::SpawnShield, ArenaLMS::SpawnShield);
	for (AArenaWeapon* W : Character->GetWeapons())
	{
		if (W && !W->bInfiniteAmmo)
		{
			const int32 Target = FMath::CeilToInt(W->MaxAmmo * ArenaLMS::SpawnAmmoFraction);
			if (W->Ammo < Target)
			{
				W->AddAmmo(Target - W->Ammo);
			}
		}
	}
	Character->SwitchToBestWeapon();
}

bool UArenaRules_LMS::AllowPickup(EArenaPickupType Type, TSubclassOf<AArenaWeapon> WeaponClass) const
{
	if (!Super::AllowPickup(Type, WeaponClass))
	{
		return false;
	}
	return bAllowAllPickups || Type == EArenaPickupType::Adrenaline;
}

// =============================================================================================
// Kills / end of match
// =============================================================================================

void UArenaRules_LMS::OnKilled(AController* Killer, AController* Victim, AArenaCharacter* VictimPawn, const FString& WeaponName, bool bHeadshot)
{
	// The beacon goes down with its carrier
	if (VictimPawn)
	{
		if (FCampState* State = CampStates.Find(VictimPawn))
		{
			if (AArenaLMS_Beacon* Beacon = State->Beacon.Get())
			{
				Beacon->Destroy();
			}
			if (AArenaPlayerState* CampPS = State->PlayerState.Get())
			{
				CampPS->ModeFlags &= ~FlagExposed;
			}
			CampStates.Remove(VictimPawn);
		}
	}

	AArenaGameState* GS = GetGameState();
	if (!GameMode || !GS || !GS->IsPlaying())
	{
		return;
	}
	AArenaPlayerState* PS = Victim ? Victim->GetPlayerState<AArenaPlayerState>() : (VictimPawn ? VictimPawn->GetPlayerState<AArenaPlayerState>() : nullptr);
	if (!PS || PS->bEliminated)
	{
		return;
	}
	if (PS->Lives < 0)
	{
		PS->Lives = GetStartLives();
	}
	PS->Lives = FMath::Max(0, PS->Lives - 1);

	if (PS->Lives > 0)
	{
		if (Victim)
		{
			GameMode->SendCenterMessage(Victim, PS->Lives == 1 ? FString(TEXT("Last life - make it count")) : FString::Printf(TEXT("%d lives left"), PS->Lives),
				ArenaLMS::LivesColor(), 2.5f);
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("LMS: %s lost a life (%d left)"), *PS->GetPlayerName(), PS->Lives);
		return;
	}

	PS->bEliminated = true;
	if (Victim)
	{
		GameMode->CancelRespawn(Victim);
		EliminatedAt.Add(Victim, GetWorld()->GetTimeSeconds());
	}
	int32 Remaining = 0;
	for (const AArenaPlayerState* Other : GetParticipants())
	{
		if (!Other->bEliminated)
		{
			Remaining++;
		}
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("LMS: %s eliminated, %d remaining"), *PS->GetPlayerName(), Remaining);
	if (Remaining > 1)
	{
		GameMode->BroadcastCenterMessage(FString::Printf(TEXT("%s is out!  %d remain"), *PS->GetPlayerName(), Remaining), ArenaLMS::OutColor(), 3.f, TEXT("S_Death"));
	}
}

void UArenaRules_LMS::CheckEndConditions()
{
	AArenaGameState* GS = GetGameState();
	if (!GameMode || !GS || !GS->IsPlaying())
	{
		return;
	}
	const TArray<AArenaPlayerState*> Parts = GetParticipants();
	if (Parts.Num() < 2)
	{
		return;
	}
	TArray<AArenaPlayerState*> Alive = Parts.FilterByPredicate([](const AArenaPlayerState* PS) { return !PS->bEliminated; });
	if (Alive.Num() > 1)
	{
		return;
	}
	AArenaPlayerState* Winner = Alive.Num() == 1 ? Alive[0] : PickLeader(Parts);
	if (!Winner)
	{
		return;
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("LMS: winner %s (last standing, %d lives left, %d kills)"), *Winner->GetPlayerName(), Winner->Lives, Winner->Kills);
	GameMode->EndMatch(FString::Printf(TEXT("%s is the last one standing!"), *Winner->GetPlayerName()), -1, Winner);
}

void UArenaRules_LMS::OnTimeLimitReached()
{
	AArenaGameState* GS = GetGameState();
	if (!GameMode || !GS)
	{
		return;
	}
	TArray<AArenaPlayerState*> Parts = GetParticipants();
	TArray<AArenaPlayerState*> Alive = Parts.FilterByPredicate([](const AArenaPlayerState* PS) { return !PS->bEliminated; });
	AArenaPlayerState* Winner = PickLeader(Alive.Num() > 0 ? Alive : Parts);
	if (!Winner)
	{
		GameMode->EndMatch(TEXT("Time! Nobody survived."), -1, nullptr);
		return;
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("LMS: time limit, winner %s (%d lives, %d kills)"), *Winner->GetPlayerName(), Winner->Lives, Winner->Kills);
	GameMode->EndMatch(FString::Printf(TEXT("Time! %s outlasted the rest"), *Winner->GetPlayerName()), -1, Winner);
}

// =============================================================================================
// Tick: anti-camping, spectators
// =============================================================================================

void UArenaRules_LMS::Tick(float DeltaSeconds)
{
	AArenaGameState* GS = GetGameState();
	if (!GameMode || !GS)
	{
		return;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	if (GS->IsPlaying())
	{
		UpdateCamping(Now);
	}
	if (GS->Phase != EArenaMatchPhase::Ended)
	{
		UpdateSpectators(Now);
	}

	// Status line for logs / spectators (also replicated for clients)
	StatusAccum += DeltaSeconds;
	if (StatusAccum >= 1.f && GS->IsPlaying())
	{
		StatusAccum = 0.f;
		const TArray<AArenaPlayerState*> Parts = GetParticipants();
		int32 Alive = 0;
		for (const AArenaPlayerState* PS : Parts)
		{
			Alive += PS->bEliminated ? 0 : 1;
		}
		GameMode->SetModeStatusLines({ FString::Printf(TEXT("%d / %d players left"), Alive, Parts.Num()) });
	}
}

void UArenaRules_LMS::SetExposed(AArenaCharacter* Character, AArenaPlayerState* PS, bool bExposed)
{
	FCampState* State = CampStates.Find(Character);
	if (!State)
	{
		return;
	}
	State->bExposed = bExposed;
	if (bExposed)
	{
		if (PS)
		{
			PS->ModeFlags |= FlagExposed;
		}
		if (!State->Beacon.IsValid() && Character)
		{
			FActorSpawnParameters Params;
			Params.Owner = Character;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AArenaLMS_Beacon* Beacon = GetWorld()->SpawnActor<AArenaLMS_Beacon>(AArenaLMS_Beacon::StaticClass(), Character->GetActorLocation(), FRotator::ZeroRotator, Params);
			if (Beacon)
			{
				Beacon->AttachToActor(Character, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
				State->Beacon = Beacon;
			}
		}
		if (Character)
		{
			ArenaAssets::PlaySoundAt(Character, TEXT("S_Flag_Taken"), Character->GetActorLocation());
			GameMode->SendCenterMessage(Character->GetController(), TEXT("You've been EXPOSED - get moving!"), AArenaLMS_Beacon::BeaconColor(), 3.f);
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("LMS: %s exposed for camping"), PS ? *PS->GetPlayerName() : TEXT("?"));
	}
	else
	{
		if (PS)
		{
			PS->ModeFlags &= ~FlagExposed;
		}
		if (AArenaLMS_Beacon* Beacon = State->Beacon.Get())
		{
			Beacon->Destroy();
		}
		State->Beacon = nullptr;
		if (Character)
		{
			GameMode->SendCenterMessage(Character->GetController(), TEXT("Back in the shadows"), ArenaHudStyle::TextWhite(), 2.f);
		}
	}
}

void UArenaRules_LMS::UpdateCamping(float Now)
{
	// Drop entries of dead / destroyed pawns
	for (auto It = CampStates.CreateIterator(); It; ++It)
	{
		AArenaCharacter* C = It.Key().Get();
		if (!IsValid(C) || C->IsDead())
		{
			if (AArenaLMS_Beacon* Beacon = It.Value().Beacon.Get())
			{
				Beacon->Destroy();
			}
			if (AArenaPlayerState* PS = It.Value().PlayerState.Get())
			{
				PS->ModeFlags &= ~FlagExposed;
			}
			It.RemoveCurrent();
		}
	}

	if (CampTime <= 0.f)
	{
		return;
	}

	for (TActorIterator<AArenaCharacter> It(GetWorld()); It; ++It)
	{
		AArenaCharacter* C = *It;
		AArenaPlayerState* PS = C ? C->GetPlayerState<AArenaPlayerState>() : nullptr;
		if (!C || !C->IsAlive() || !PS)
		{
			continue;
		}
		const FVector Loc = C->GetActorLocation();
		FCampState* State = CampStates.Find(C);
		if (!State)
		{
			FCampState NewState;
			NewState.Anchor = Loc;
			NewState.AnchorTime = Now;
			NewState.PlayerState = PS;
			State = &CampStates.Add(C, NewState);
			continue;
		}
		if (FVector::Dist(Loc, State->Anchor) > CampRadius)
		{
			if (State->bExposed)
			{
				SetExposed(C, PS, false);
			}
			State->Anchor = Loc;
			State->AnchorTime = Now;
			State->bWarned = false;
			continue;
		}
		const float Stayed = Now - State->AnchorTime;
		if (!State->bWarned && !State->bExposed && Stayed > CampTime - ArenaLMS::CampWarnLead)
		{
			State->bWarned = true;
			GameMode->SendCenterMessage(C->GetController(), TEXT("Keep moving or you'll be exposed"), AArenaLMS_Beacon::BeaconColor(), 2.5f);
		}
		if (!State->bExposed && Stayed > CampTime)
		{
			SetExposed(C, PS, true);
		}
	}
}

AArenaCharacter* UArenaRules_LMS::FindNextAlive(const AArenaCharacter* After) const
{
	TArray<AArenaCharacter*> Alive;
	for (TActorIterator<AArenaCharacter> It(GetWorld()); It; ++It)
	{
		if (It->IsAlive() && It->GetPlayerState())
		{
			Alive.Add(*It);
		}
	}
	if (Alive.Num() == 0)
	{
		return nullptr;
	}
	Alive.Sort([](const AArenaCharacter& A, const AArenaCharacter& B) { return A.GetPlayerState()->GetPlayerId() < B.GetPlayerState()->GetPlayerId(); });
	const int32 Index = After ? Alive.IndexOfByKey(After) : INDEX_NONE;
	return Alive[(Index + 1) % Alive.Num()];
}

void UArenaRules_LMS::UpdateSpectators(float Now)
{
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		const AArenaPlayerState* PS = PC ? PC->GetPlayerState<AArenaPlayerState>() : nullptr;
		if (!PS || !PS->bEliminated || PC->GetPawn())
		{
			continue;
		}
		AArenaCharacter* Current = Cast<AArenaCharacter>(PC->GetViewTarget());
		const bool bWatchingSomeone = Current && Current->IsAlive();
		const bool bCycle = PC->IsLocalController()
			&& (PC->WasInputKeyJustPressed(EKeys::LeftMouseButton) || PC->WasInputKeyJustPressed(EKeys::Gamepad_RightTrigger));
		const float* When = EliminatedAt.Find(PC);
		const bool bDelayOver = !When || Now - *When > ArenaLMS::SpectateSwitchDelay;
		if (bCycle || (!bWatchingSomeone && bDelayOver))
		{
			if (AArenaCharacter* Next = FindNextAlive(bWatchingSomeone ? Current : nullptr))
			{
				if (Next != Current)
				{
					PC->SetViewTarget(Next);
				}
			}
		}
	}
}

// =============================================================================================
// Bots
// =============================================================================================

void UArenaRules_LMS::GetBotObjectives(AArenaBotController* Bot, TArray<FArenaBotObjective>& OutGoals)
{
	AArenaCharacter* Me = Bot ? Bot->GetArenaPawn() : nullptr;
	if (!Me || !Me->IsAlive())
	{
		return;
	}
	const FVector MyLoc = Me->GetActorLocation();
	AArenaCharacter* NearestCamper = nullptr;
	AArenaCharacter* Nearest = nullptr;
	float CamperDist = BIG_NUMBER;
	float NearestDist = BIG_NUMBER;
	for (TActorIterator<AArenaCharacter> It(GetWorld()); It; ++It)
	{
		AArenaCharacter* C = *It;
		if (C == Me || !C->IsAlive())
		{
			continue;
		}
		const AArenaPlayerState* PS = C->GetPlayerState<AArenaPlayerState>();
		const float Dist = FVector::Dist(MyLoc, C->GetActorLocation());
		if (PS && (PS->ModeFlags & FlagExposed) && Dist < CamperDist)
		{
			CamperDist = Dist;
			NearestCamper = C;
		}
		if (Dist < NearestDist)
		{
			NearestDist = Dist;
			Nearest = C;
		}
	}
	if (NearestCamper)
	{
		FArenaBotObjective O(NearestCamper->GetActorLocation(), NearestCamper, 1.0f, EArenaBotAction::Attack, ArenaLMS::TagCamper);
		O.bTrackActor = true;
		OutGoals.Add(O);
	}
	if (Nearest && Nearest != NearestCamper)
	{
		// Always on the hunt: beats wandering and most item pickups, but not an exposed camper
		FArenaBotObjective O(Nearest->GetActorLocation(), Nearest, 0.6f, EArenaBotAction::Attack, ArenaLMS::TagHunt);
		O.bTrackActor = true;
		OutGoals.Add(O);
	}
}

bool UArenaRules_LMS::UpdateBotObjective(AArenaBotController* Bot, FArenaBotObjective& Objective)
{
	const AArenaCharacter* Target = Cast<AArenaCharacter>(Objective.Actor.Get());
	if (!IsValid(Target) || Target->IsDead())
	{
		return false;
	}
	if (Objective.Tag == ArenaLMS::TagCamper)
	{
		const AArenaPlayerState* PS = Target->GetPlayerState<AArenaPlayerState>();
		if (!PS || !(PS->ModeFlags & FlagExposed))
		{
			return false;
		}
	}
	return Super::UpdateBotObjective(Bot, Objective);
}

float UArenaRules_LMS::GetBotEnemyWeight(AArenaBotController* Bot, AArenaCharacter* Candidate) const
{
	const AArenaPlayerState* PS = Candidate ? Candidate->GetPlayerState<AArenaPlayerState>() : nullptr;
	return (PS && (PS->ModeFlags & FlagExposed)) ? 0.5f : 1.f;
}

// =============================================================================================
// HUD
// =============================================================================================

void UArenaRules_LMS::DrawHUD(AArenaHUD* HUD, UCanvas* Canvas)
{
	const AArenaGameState* GS = GetGameState();
	APlayerController* PC = HUD ? HUD->GetOwningPlayerController() : nullptr;
	const AArenaPlayerState* MyPS = PC ? PC->GetPlayerState<AArenaPlayerState>() : nullptr;
	if (!GS || !MyPS || !Canvas || HUD->IsUTHud())
	{
		// the UT2004 HUD shows lives and players left in its rank / spread widgets (HudLMS, AArenaHUD::DrawUTScorePass)
		return;
	}
	const FArenaDraw& D = HUD->GetDraw();

	int32 Total = 0;
	int32 Alive = 0;
	for (APlayerState* P : GS->PlayerArray)
	{
		const AArenaPlayerState* PS = Cast<AArenaPlayerState>(P);
		if (PS && !PS->IsOnlyASpectator() && PS->Lives >= 0)
		{
			Total++;
			Alive += PS->bEliminated ? 0 : 1;
		}
	}

	// ---- panel under the score: lives + players left ----
	const float Right = D.W() - D.S(24.f);
	const float PW = D.S(230.f);
	const float PH = D.S(74.f);
	const float X = Right - PW;
	const float Top = HUD->GetScoreInfoBottom() + D.S(8.f);
	const bool bOut = MyPS->bEliminated;
	D.Panel(X, Top, PW, PH, bOut ? ArenaLMS::OutColor() : ArenaLMS::LivesColor());

	D.Text(TEXT("LIVES"), X + D.S(18.f), Top + D.S(8.f), ArenaHudStyle::TextDim(), D.S(13.f));
	if (bOut)
	{
		D.Text(TEXT("OUT"), X + D.S(18.f), Top + D.S(24.f), ArenaLMS::OutColor(), D.S(34.f));
	}
	else
	{
		const int32 Lives = FMath::Max(0, MyPS->Lives);
		D.Text(FString::FromInt(Lives), X + D.S(18.f), Top + D.S(24.f), Lives <= 1 ? ArenaLMS::OutColor() : ArenaHudStyle::TextWhite(), D.S(34.f));
		// pips
		const int32 MaxPips = FMath::Clamp(FMath::Max(GS->GoalScore, Lives), 1, 10);
		for (int32 i = 0; i < MaxPips; ++i)
		{
			const float PX = X + D.S(62.f) + i * D.S(10.f);
			const FLinearColor PipCol = i < Lives ? ArenaLMS::LivesColor() : ArenaHudStyle::WithAlpha(ArenaHudStyle::TextDim(), 0.35f);
			if (!ArenaArtHUD::DrawSpriteCentered(D, ArenaArtHUD::Sprites::HealthIcon(), PX + D.S(3.5f), Top + D.S(45.f), D.S(11.f), PipCol))
			{
				D.Rect(PX, Top + D.S(38.f), D.S(7.f), D.S(14.f), PipCol);
			}
		}
	}
	D.Text(TEXT("PLAYERS LEFT"), Right - D.S(16.f), Top + D.S(8.f), ArenaHudStyle::TextDim(), D.S(13.f), EArenaAlign::Right);
	D.Text(FString::Printf(TEXT("%d / %d"), Alive, Total), Right - D.S(16.f), Top + D.S(26.f), ArenaHudStyle::TextWhite(), D.S(26.f), EArenaAlign::Right);

	// ---- exposed campers ----
	const APawn* MyPawn = PC->GetPawn();
	for (APlayerState* P : GS->PlayerArray)
	{
		const AArenaPlayerState* PS = Cast<AArenaPlayerState>(P);
		const AArenaCharacter* C = PS ? Cast<AArenaCharacter>(PS->GetPawn()) : nullptr;
		if (!PS || PS == MyPS || !(PS->ModeFlags & FlagExposed) || !C || C->IsDead() || C == MyPawn || C == PC->GetViewTarget())
		{
			continue;
		}
		ArenaLMS::DrawWorldMarker(HUD, Canvas, C->GetActorLocation() + FVector(0.f, 0.f, 120.f), FString::Printf(TEXT("CAMPER %s"), *PS->GetPlayerName()),
			AArenaLMS_Beacon::BeaconColor());
	}

	// ---- own warnings / spectating ----
	if (!bOut && (MyPS->ModeFlags & FlagExposed))
	{
		const float Pulse = 0.6f + 0.4f * FMath::Sin(HUD->Now() * 7.f);
		D.Text(TEXT("EXPOSED - KEEP MOVING"), D.W() * 0.5f, D.S(150.f), ArenaHudStyle::WithAlpha(AArenaLMS_Beacon::BeaconColor(), Pulse), D.S(26.f), EArenaAlign::Center);
	}
	if (bOut && GS->Phase != EArenaMatchPhase::Ended)
	{
		const AArenaCharacter* Watched = Cast<AArenaCharacter>(PC->GetViewTarget());
		const APlayerState* WatchedPS = Watched ? Watched->GetPlayerState() : nullptr;
		if (Watched && Watched->IsAlive() && WatchedPS)
		{
			const AArenaPlayerState* WPS = Cast<AArenaPlayerState>(WatchedPS);
			const FString Line = FString::Printf(TEXT("Watching %s  (%d %s)  -  FIRE for next"), *WatchedPS->GetPlayerName(), WPS ? WPS->Lives : 0,
				(WPS && WPS->Lives == 1) ? TEXT("life") : TEXT("lives"));
			D.Text(Line, D.W() * 0.5f, D.H() * 0.80f, ArenaHudStyle::TextWhite(), D.S(22.f), EArenaAlign::Center);
		}
	}
}

FString UArenaRules_LMS::GetScoreboardExtra(const AArenaPlayerState* Player) const
{
	if (!Player || Player->Lives < 0)
	{
		return FString();
	}
	if (Player->bEliminated)
	{
		return TEXT("OUT");
	}
	FString Text = Player->Lives == 1 ? FString(TEXT("1 life")) : FString::Printf(TEXT("%d lives"), Player->Lives);
	if (Player->ModeFlags & FlagExposed)
	{
		Text += TEXT("  EXPOSED");
	}
	return Text;
}

FString UArenaRules_LMS::GetScoreboardHeaderExtra(const AArenaGameState* GS) const
{
	if (!GS)
	{
		return FString();
	}
	int32 Alive = 0;
	for (APlayerState* P : GS->PlayerArray)
	{
		const AArenaPlayerState* PS = Cast<AArenaPlayerState>(P);
		if (PS && !PS->IsOnlyASpectator() && PS->Lives >= 0 && !PS->bEliminated)
		{
			Alive++;
		}
	}
	return FString::Printf(TEXT("%d left standing"), Alive);
}

bool UArenaRules_LMS::ShouldDrawDeadScreen(APlayerController* PC) const
{
	// Eliminated players who are watching a survivor get the spectator view, not the death tint
	const AArenaPlayerState* PS = PC ? PC->GetPlayerState<AArenaPlayerState>() : nullptr;
	if (PS && PS->bEliminated && !PC->GetPawn())
	{
		const AArenaCharacter* Watched = Cast<AArenaCharacter>(PC->GetViewTarget());
		if (Watched && Watched->IsAlive())
		{
			return false;
		}
	}
	return Super::ShouldDrawDeadScreen(PC);
}

FString UArenaRules_LMS::GetDeadScreenMessage(const AArenaPlayerState* Player) const
{
	if (Player && Player->bEliminated)
	{
		return TEXT("Eliminated  -  FIRE to watch another survivor");
	}
	if (Player && Player->Lives > 0)
	{
		return Player->Lives == 1 ? FString(TEXT("1 life left  -  press FIRE to respawn"))
			: FString::Printf(TEXT("%d lives left  -  press FIRE to respawn"), Player->Lives);
	}
	return Super::GetDeadScreenMessage(Player);
}
