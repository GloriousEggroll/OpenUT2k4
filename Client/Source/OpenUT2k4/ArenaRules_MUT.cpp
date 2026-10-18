#include "ArenaRules_MUT.h"
#include "OpenUT2k4.h"
#include "ArenaMUT_Aura.h"
#include "ArenaGameMode.h"
#include "ArenaGameState.h"
#include "ArenaPlayerState.h"
#include "ArenaCharacter.h"
#include "ArenaBotController.h"
#include "ArenaHUD.h"
#include "ArenaWeapon.h"
#include "ArenaWeapons.h"
#include "ArenaAssets.h"
#include "Engine/Canvas.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/CharacterMovementComponent.h"

ARENA_REGISTER_RULES(UArenaRules_MUT, "MUT", "Mutant");

namespace ArenaMUT
{
	constexpr float DamageScale = 2.f;
	/** Damage the Mutant takes from others. Everyone hunts it, so it needs to soak a lot to last 30-60 s. */
	constexpr float DamageTakenScale = 0.2f;
	/** Health on mutating (above MaxHealth, like a big health pack); shield is filled to MaxShield. */
	constexpr float StartHealth = 199.f;
	constexpr float SpeedScale = 1.3f;
	/** Health per second (up to MaxHealth), then half of it as shield (up to ShieldRegenCap). */
	constexpr float RegenPerSecond = 10.f;
	constexpr float ShieldRegenCap = 100.f;
	constexpr float FeederCheckInterval = 0.5f;
	/** The Bottom Feeder only exists with at least this many players. */
	constexpr int32 MinPlayersForFeeder = 3;

	enum EObjectiveTag : int32
	{
		TagHunt = 1,
		TagChaseMutant = 2
	};

	FLinearColor FeederColor() { return FLinearColor(0.35f, 1.f, 0.45f); }

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

		const float Pulse = 0.75f + 0.25f * FMath::Sin(HUD->Now() * 5.f);
		const FLinearColor C = ArenaHudStyle::WithAlpha(Color, Pulse);
		if (bOnScreen)
		{
			// Diamond + ring around the target
			const float R = D.S(16.f);
			D.Line(Screen.X, Screen.Y - R, Screen.X + R, Screen.Y, C, D.S(2.5f));
			D.Line(Screen.X + R, Screen.Y, Screen.X, Screen.Y + R, C, D.S(2.5f));
			D.Line(Screen.X, Screen.Y + R, Screen.X - R, Screen.Y, C, D.S(2.5f));
			D.Line(Screen.X - R, Screen.Y, Screen.X, Screen.Y - R, C, D.S(2.5f));
			D.Arc(Screen.X, Screen.Y, D.S(4.f), D.S(7.f), 0.f, 2.f * PI, C, 16);
			D.Text(Text, Screen.X, Screen.Y - D.S(44.f), C, D.S(16.f), EArenaAlign::Center);
			return;
		}

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
		const FVector2D Tip = Pos + V * D.S(18.f);
		const FVector2D A = Pos + Perp * D.S(12.f);
		const FVector2D B = Pos - Perp * D.S(12.f);
		const float Th = D.S(3.f);
		D.Line(Tip.X, Tip.Y, A.X, A.Y, C, Th);
		D.Line(Tip.X, Tip.Y, B.X, B.Y, C, Th);
		D.Line(A.X, A.Y, B.X, B.Y, C, Th);
		const FVector2D TextPos = Pos - V * D.S(36.f);
		D.TextMid(Text, TextPos.X, TextPos.Y, C, D.S(15.f), EArenaAlign::Center);
	}
}

// =============================================================================================
// Identity / queries
// =============================================================================================

FString UArenaRules_MUT::GetGoalText(int32 Goal) const
{
	return Goal > 0 ? FString::Printf(TEXT("First to %d points"), Goal) : FString();
}

bool UArenaRules_MUT::HasFlag(const AController* C, int32 Flag)
{
	const AArenaPlayerState* PS = C ? C->GetPlayerState<AArenaPlayerState>() : nullptr;
	return PS && (PS->ModeFlags & Flag) != 0;
}

AArenaPlayerState* UArenaRules_MUT::FindFlagged(const AArenaGameState* GS, int32 Flag)
{
	if (!GS)
	{
		return nullptr;
	}
	for (APlayerState* P : GS->PlayerArray)
	{
		AArenaPlayerState* PS = Cast<AArenaPlayerState>(P);
		if (PS && (PS->ModeFlags & Flag))
		{
			return PS;
		}
	}
	return nullptr;
}

// =============================================================================================
// Mutant status
// =============================================================================================

void UArenaRules_MUT::ApplyMutantBuffs(AArenaCharacter* Pawn, bool bInitial)
{
	if (!Pawn || Pawn->IsDead())
	{
		return;
	}
	if (bInitial)
	{
		Pawn->GiveHealth(ArenaMUT::StartHealth, ArenaMUT::StartHealth);
		Pawn->GiveShield(ArenaTuning::MaxShield, ArenaTuning::MaxShield);
		if (!(GameMode && GameMode->IsInstagib()))
		{
			for (const TSubclassOf<AArenaWeapon>& W : ArenaWeaponList::AllWeapons())
			{
				if (W && !Pawn->FindWeapon(W))
				{
					Pawn->GiveWeapon(W);
				}
			}
			for (AArenaWeapon* W : Pawn->GetWeapons())
			{
				if (W && !W->bInfiniteAmmo && W->Ammo < W->MaxAmmo)
				{
					W->AddAmmo(W->MaxAmmo - W->Ammo);
				}
			}
		}
	}
	if (UCharacterMovementComponent* Move = Pawn->GetCharacterMovement())
	{
		const float ComboScale = Pawn->GetActiveCombo() == EArenaCombo::Speed ? 1.4f : 1.f;
		Move->MaxWalkSpeed = ArenaTuning::GroundSpeed * ArenaMUT::SpeedScale * ComboScale;
	}
}

void UArenaRules_MUT::MakeMutant(AController* Controller)
{
	AArenaPlayerState* PS = Controller ? Controller->GetPlayerState<AArenaPlayerState>() : nullptr;
	AArenaCharacter* Pawn = Controller ? Cast<AArenaCharacter>(Controller->GetPawn()) : nullptr;
	if (!PS || !Pawn || Pawn->IsDead())
	{
		ClearMutant();
		return;
	}
	if (Mutant.Get() != Controller)
	{
		ClearMutant();
	}
	Mutant = Controller;
	PS->ModeFlags |= FlagMutant;
	PS->ModeFlags &= ~FlagBottomFeeder;
	RegenAccum = 0.f;
	ApplyMutantBuffs(Pawn, true);

	if (!Aura.IsValid())
	{
		FActorSpawnParameters Params;
		Params.Owner = Pawn;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		if (AArenaMUT_Aura* NewAura = GetWorld()->SpawnActor<AArenaMUT_Aura>(AArenaMUT_Aura::StaticClass(), Pawn->GetActorLocation(), FRotator::ZeroRotator, Params))
		{
			NewAura->AttachToActor(Pawn, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
			Aura = NewAura;
		}
	}

	ArenaAssets::PlaySoundAt(Pawn, TEXT("S_Pickup_Power"), Pawn->GetActorLocation());
	GameMode->BroadcastCenterMessage(FString::Printf(TEXT("%s has mutated!"), *PS->GetPlayerName()), AArenaMUT_Aura::AuraColor(), 3.f, TEXT("S_Spree"));
	GameMode->SendCenterMessage(Controller, TEXT("You are the MUTANT - every kill scores"), AArenaMUT_Aura::AuraColor(), 3.f);
	UE_LOG(LogOpenUT2k4, Log, TEXT("MUT: %s is now the Mutant"), *PS->GetPlayerName());
}

void UArenaRules_MUT::ClearMutant()
{
	AController* Old = Mutant.Get();
	if (AArenaCharacter* OldPawn = Old ? Cast<AArenaCharacter>(Old->GetPawn()) : nullptr)
	{
		if (OldPawn->IsAlive())
		{
			if (UCharacterMovementComponent* Move = OldPawn->GetCharacterMovement())
			{
				Move->MaxWalkSpeed = ArenaTuning::GroundSpeed;
			}
		}
	}
	if (const AArenaGameState* GS = GetGameState())
	{
		for (APlayerState* P : GS->PlayerArray)
		{
			if (AArenaPlayerState* PS = Cast<AArenaPlayerState>(P))
			{
				PS->ModeFlags &= ~FlagMutant;
			}
		}
	}
	if (AArenaMUT_Aura* OldAura = Aura.Get())
	{
		OldAura->Destroy();
	}
	Aura = nullptr;
	if (Old)
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("MUT: the Mutant (%s) lost the status"), Old->PlayerState ? *Old->PlayerState->GetPlayerName() : TEXT("?"));
	}
	Mutant = nullptr;
}

void UArenaRules_MUT::UpdateBottomFeeder()
{
	const AArenaGameState* GS = GetGameState();
	if (!GS)
	{
		return;
	}
	TArray<AArenaPlayerState*> Players;
	for (APlayerState* P : GS->PlayerArray)
	{
		AArenaPlayerState* PS = Cast<AArenaPlayerState>(P);
		if (PS && !PS->IsOnlyASpectator())
		{
			Players.Add(PS);
		}
	}
	AArenaPlayerState* Current = FindFlagged(GS, FlagBottomFeeder);
	AArenaPlayerState* New = nullptr;
	const bool bHasMutant = FindFlagged(GS, FlagMutant) != nullptr;
	if (bHasMutant && Players.Num() >= ArenaMUT::MinPlayersForFeeder)
	{
		float MinScore = BIG_NUMBER;
		for (const AArenaPlayerState* PS : Players)
		{
			if (!(PS->ModeFlags & FlagMutant))
			{
				MinScore = FMath::Min(MinScore, PS->GetScore());
			}
		}
		if (Current && !(Current->ModeFlags & FlagMutant) && Current->GetScore() <= MinScore)
		{
			New = Current; // keep the current one on ties
		}
		else
		{
			for (AArenaPlayerState* PS : Players)
			{
				if ((PS->ModeFlags & FlagMutant) || PS->GetScore() > MinScore)
				{
					continue;
				}
				if (!New || PS->Kills < New->Kills || (PS->Kills == New->Kills && PS->GetPlayerId() < New->GetPlayerId()))
				{
					New = PS;
				}
			}
		}
	}
	if (New == Current)
	{
		return;
	}
	for (AArenaPlayerState* PS : Players)
	{
		PS->ModeFlags &= ~FlagBottomFeeder;
	}
	if (New)
	{
		New->ModeFlags |= FlagBottomFeeder;
		if (GameMode)
		{
			GameMode->SendCenterMessage(Cast<AController>(New->GetOwner()), TEXT("You're the Bottom Feeder - anyone is fair game"), ArenaMUT::FeederColor(), 3.f);
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("MUT: bottom feeder is now %s (score %.0f)"), *New->GetPlayerName(), New->GetScore());
	}
}

// =============================================================================================
// Lifecycle
// =============================================================================================

void UArenaRules_MUT::Tick(float DeltaSeconds)
{
	AArenaGameState* GS = GetGameState();
	if (!GameMode || !GS || !GS->IsPlaying())
	{
		return;
	}

	if (Mutant.IsValid() || Aura.IsValid())
	{
		AController* C = Mutant.Get();
		AArenaCharacter* Pawn = C ? Cast<AArenaCharacter>(C->GetPawn()) : nullptr;
		if (!Pawn || Pawn->IsDead() || !HasFlag(C, FlagMutant))
		{
			ClearMutant();
			UpdateBottomFeeder();
		}
		else
		{
			ApplyMutantBuffs(Pawn, false);
			RegenAccum += DeltaSeconds * ArenaMUT::RegenPerSecond;
			if (RegenAccum >= 1.f)
			{
				const float Whole = FMath::FloorToFloat(RegenAccum);
				RegenAccum -= Whole;
				if (!Pawn->GiveHealth(Whole, ArenaTuning::MaxHealth))
				{
					Pawn->GiveShield(Whole * 0.5f, ArenaMUT::ShieldRegenCap);
				}
			}
		}
	}

	FeederCheckAccum += DeltaSeconds;
	if (FeederCheckAccum >= ArenaMUT::FeederCheckInterval)
	{
		FeederCheckAccum = 0.f;
		UpdateBottomFeeder();
	}
}

void UArenaRules_MUT::OnPlayerLeft(AController* Controller)
{
	if (Controller && Mutant.Get() == Controller)
	{
		ClearMutant();
	}
	if (AArenaPlayerState* PS = Controller ? Controller->GetPlayerState<AArenaPlayerState>() : nullptr)
	{
		PS->ModeFlags = 0;
	}
	UpdateBottomFeeder();
}

void UArenaRules_MUT::OnPawnSpawned(AArenaCharacter* Character)
{
	// A fresh pawn is never the Mutant (status is lost on death)
	AArenaPlayerState* PS = Character ? Character->GetPlayerState<AArenaPlayerState>() : nullptr;
	if (PS && (PS->ModeFlags & FlagMutant) && Mutant.Get() != Character->GetController())
	{
		PS->ModeFlags &= ~FlagMutant;
	}
}

void UArenaRules_MUT::OnMatchEnd(const FString& WinnerText, int32 WinningTeam, APlayerState* WinningPlayer)
{
	UE_LOG(LogOpenUT2k4, Log, TEXT("MUT: match over: %s"), *WinnerText);
}

// =============================================================================================
// Combat / scoring
// =============================================================================================

bool UArenaRules_MUT::AreOnSameTeam(const AController* A, const AController* B) const
{
	if (!A || !B || A == B)
	{
		return false;
	}
	const AArenaPlayerState* PA = A->GetPlayerState<AArenaPlayerState>();
	const AArenaPlayerState* PB = B->GetPlayerState<AArenaPlayerState>();
	if (!PA || !PB)
	{
		return false;
	}
	// Before the first mutation it's a plain free-for-all
	if (!FindFlagged(GetGameState(), FlagMutant))
	{
		return false;
	}
	// The Mutant fights everyone, the Bottom Feeder may fight everyone; the rest are allies against the Mutant
	return ((PA->ModeFlags | PB->ModeFlags) & (FlagMutant | FlagBottomFeeder)) == 0;
}

float UArenaRules_MUT::ModifyDamage(float Damage, AController* InstigatorController, AArenaCharacter* Victim) const
{
	float Result = Super::ModifyDamage(Damage, InstigatorController, Victim);
	if (Result > 0.f && Victim && InstigatorController && InstigatorController != Victim->GetController() && HasFlag(InstigatorController, FlagMutant))
	{
		Result *= ArenaMUT::DamageScale;
	}
	else if (Result > 0.f && Victim && InstigatorController && InstigatorController != Victim->GetController() && HasFlag(Victim->GetController(), FlagMutant))
	{
		Result *= ArenaMUT::DamageTakenScale;
	}
	return Result;
}

bool UArenaRules_MUT::ScoreKill(AController* Killer, AController* Victim, bool bHeadshot)
{
	AArenaPlayerState* VictimPS = Victim ? Victim->GetPlayerState<AArenaPlayerState>() : nullptr;
	AArenaPlayerState* KillerPS = Killer ? Killer->GetPlayerState<AArenaPlayerState>() : nullptr;
	const bool bSuicide = !Killer || Killer == Victim;
	const bool bVictimMutant = VictimPS && (VictimPS->ModeFlags & FlagMutant);
	const bool bNoMutant = FindFlagged(GetGameState(), FlagMutant) == nullptr;

	if (VictimPS)
	{
		VictimPS->Deaths++;
		VictimPS->Spree = 0;
		VictimPS->MultiKillCount = 0;
	}

	if (bSuicide || !KillerPS)
	{
		if (bSuicide && VictimPS)
		{
			VictimPS->AddScore(-1.f);
			VictimPS->Suicides++;
		}
		if (bVictimMutant)
		{
			GameMode->BroadcastCenterMessage(TEXT("The Mutant destroyed itself - next kill takes the power"), AArenaMUT_Aura::AuraColor(), 3.f);
			ClearMutant();
		}
		UpdateBottomFeeder();
		return true;
	}

	const bool bKillerMutant = (KillerPS->ModeFlags & FlagMutant) != 0;
	const bool bKillerFeeder = (KillerPS->ModeFlags & FlagBottomFeeder) != 0;
	KillerPS->Kills++;
	KillerPS->Spree++;
	int32 Points = 0;
	if (bKillerMutant)
	{
		Points = 1;
		KillerPS->ObjectiveScore++;
	}
	else if (bVictimMutant || bNoMutant || bKillerFeeder)
	{
		Points = 1;
	}
	KillerPS->AddScore(float(Points));
	if (AArenaCharacter* KillerPawn = Cast<AArenaCharacter>(Killer->GetPawn()))
	{
		KillerPawn->GiveAdrenaline(ArenaTuning::AdrenalinePerKill);
	}

	if (bVictimMutant)
	{
		GameMode->BroadcastCenterMessage(FString::Printf(TEXT("%s brought down the Mutant!"), *KillerPS->GetPlayerName()), FLinearColor(1.f, 0.85f, 0.3f), 2.5f);
		UE_LOG(LogOpenUT2k4, Log, TEXT("MUT: %s killed the Mutant %s"), *KillerPS->GetPlayerName(), *VictimPS->GetPlayerName());
	}
	if (bVictimMutant || bNoMutant)
	{
		MakeMutant(Killer);
	}
	UpdateBottomFeeder();
	return true;
}

void UArenaRules_MUT::OnKilled(AController* Killer, AController* Victim, AArenaCharacter* VictimPawn, const FString& WeaponName, bool bHeadshot)
{
	// Status is lost on death even outside the Playing phase
	if (Victim && Mutant.Get() == Victim)
	{
		ClearMutant();
		UpdateBottomFeeder();
	}
	if (AArenaMUT_Aura* A = Aura.Get())
	{
		if (VictimPawn && A->GetAttachParentActor() == VictimPawn)
		{
			A->Destroy();
			Aura = nullptr;
		}
	}
}

// =============================================================================================
// Bots
// =============================================================================================

void UArenaRules_MUT::GetBotObjectives(AArenaBotController* Bot, TArray<FArenaBotObjective>& OutGoals)
{
	AArenaCharacter* Me = Bot ? Bot->GetArenaPawn() : nullptr;
	if (!Me || !Me->IsAlive())
	{
		return;
	}
	const bool bMeMutant = HasFlag(Bot, FlagMutant);
	const bool bMeFeeder = HasFlag(Bot, FlagBottomFeeder);
	AController* MutantC = Mutant.Get();
	AArenaCharacter* MutantPawn = MutantC ? Cast<AArenaCharacter>(MutantC->GetPawn()) : nullptr;
	if (MutantPawn && MutantPawn->IsDead())
	{
		MutantPawn = nullptr;
	}

	// Nearest valid prey (anyone for the Mutant / Feeder / before the first mutation)
	const FVector MyLoc = Me->GetActorLocation();
	AArenaCharacter* Nearest = nullptr;
	float NearestDist = BIG_NUMBER;
	for (TActorIterator<AArenaCharacter> It(GetWorld()); It; ++It)
	{
		AArenaCharacter* C = *It;
		if (C == Me || !C->IsAlive() || C == MutantPawn)
		{
			continue;
		}
		const float Dist = FVector::Dist(MyLoc, C->GetActorLocation());
		if (Dist < NearestDist)
		{
			NearestDist = Dist;
			Nearest = C;
		}
	}

	if (bMeMutant)
	{
		// Aggressive: always charging the closest victim
		if (Nearest)
		{
			FArenaBotObjective O(Nearest->GetActorLocation(), Nearest, 1.1f, EArenaBotAction::Attack, ArenaMUT::TagHunt);
			O.bTrackActor = true;
			OutGoals.Add(O);
		}
		return;
	}
	if (MutantPawn)
	{
		FArenaBotObjective O(MutantPawn->GetActorLocation(), MutantPawn, bMeFeeder ? 0.7f : 0.85f, EArenaBotAction::Attack, ArenaMUT::TagChaseMutant);
		O.bTrackActor = true;
		OutGoals.Add(O);
	}
	if (Nearest && (!MutantPawn || bMeFeeder))
	{
		FArenaBotObjective O(Nearest->GetActorLocation(), Nearest, 0.6f, EArenaBotAction::Attack, ArenaMUT::TagHunt);
		O.bTrackActor = true;
		OutGoals.Add(O);
	}
}

bool UArenaRules_MUT::UpdateBotObjective(AArenaBotController* Bot, FArenaBotObjective& Objective)
{
	const AArenaCharacter* Target = Cast<AArenaCharacter>(Objective.Actor.Get());
	if (!IsValid(Target) || Target->IsDead())
	{
		return false;
	}
	const bool bMutantExists = Mutant.IsValid();
	if (Objective.Tag == ArenaMUT::TagChaseMutant && !HasFlag(Target->GetController(), FlagMutant))
	{
		return false;
	}
	if (Objective.Tag == ArenaMUT::TagHunt && bMutantExists && !HasFlag(Bot, FlagMutant) && !HasFlag(Bot, FlagBottomFeeder))
	{
		return false; // a Mutant appeared: go after it instead
	}
	if (Objective.Tag == ArenaMUT::TagHunt && HasFlag(Target->GetController(), FlagMutant) && !HasFlag(Bot, FlagMutant))
	{
		return false; // prey turned into the Mutant: re-evaluate (chase objective)
	}
	return Super::UpdateBotObjective(Bot, Objective);
}

float UArenaRules_MUT::GetBotEnemyWeight(AArenaBotController* Bot, AArenaCharacter* Candidate) const
{
	if (!Candidate || HasFlag(Bot, FlagMutant))
	{
		return 1.f;
	}
	return HasFlag(Candidate->GetController(), FlagMutant) ? 0.25f : 1.f;
}

// =============================================================================================
// HUD
// =============================================================================================

void UArenaRules_MUT::DrawHUD(AArenaHUD* HUD, UCanvas* Canvas)
{
	const AArenaGameState* GS = GetGameState();
	APlayerController* PC = HUD ? HUD->GetOwningPlayerController() : nullptr;
	const AArenaPlayerState* MyPS = PC ? PC->GetPlayerState<AArenaPlayerState>() : nullptr;
	if (!GS || !MyPS || !Canvas)
	{
		return;
	}
	const FArenaDraw& D = HUD->GetDraw();
	const AArenaPlayerState* MutantPS = FindFlagged(GS, FlagMutant);
	const AArenaPlayerState* FeederPS = FindFlagged(GS, FlagBottomFeeder);
	if (HUD->IsUTHud())
	{
		FVector MutantLoc;
		const FVector* MutantLocPtr = nullptr;
		const APawn* MutantPawn = MutantPS && MutantPS != MyPS ? MutantPS->GetPawn() : nullptr;
		if (MutantPawn)
		{
			MutantLoc = MutantPawn->GetActorLocation();
			MutantLocPtr = &MutantLoc;
		}
		HUD->DrawUTMutant(MutantPS && MutantPS != MyPS ? MutantPS->GetPlayerName() : FString(), MutantLocPtr, FeederPS == MyPS);
		return;
	}
	const FLinearColor MutCol = AArenaMUT_Aura::AuraColor();

	// ---- panel under the score: who is the Mutant / Bottom Feeder ----
	const float Right = D.W() - D.S(24.f);
	const float PW = D.S(230.f);
	const float PH = D.S(84.f);
	const float X = Right - PW;
	const float Top = HUD->GetScoreInfoBottom() + D.S(8.f);
	D.Panel(X, Top, PW, PH, MutCol);
	D.Text(TEXT("MUTANT"), X + D.S(18.f), Top + D.S(8.f), ArenaHudStyle::TextDim(), D.S(13.f));
	D.Text(MutantPS ? (MutantPS == MyPS ? FString(TEXT("YOU")) : MutantPS->GetPlayerName()) : FString(TEXT("none - first kill mutates")),
		X + D.S(18.f), Top + D.S(22.f), MutantPS ? MutCol : ArenaHudStyle::TextDim(), D.S(MutantPS ? 20.f : 15.f));
	D.Text(TEXT("BOTTOM FEEDER"), X + D.S(18.f), Top + D.S(46.f), ArenaHudStyle::TextDim(), D.S(13.f));
	D.Text(FeederPS ? (FeederPS == MyPS ? FString(TEXT("YOU")) : FeederPS->GetPlayerName()) : FString(TEXT("-")),
		X + D.S(18.f), Top + D.S(60.f), FeederPS ? ArenaMUT::FeederColor() : ArenaHudStyle::TextDim(), D.S(16.f));

	// ---- Mutant locator ----
	if (MutantPS && MutantPS != MyPS)
	{
		const AArenaCharacter* MutantPawn = Cast<AArenaCharacter>(MutantPS->GetPawn());
		if (MutantPawn && MutantPawn->IsAlive() && MutantPawn != PC->GetViewTarget())
		{
			ArenaMUT::DrawWorldMarker(HUD, Canvas, MutantPawn->GetActorLocation() + FVector(0.f, 0.f, 130.f), TEXT("MUTANT"), MutCol);
		}
	}

	// ---- role banner ----
	if (GS->Phase == EArenaMatchPhase::Ended)
	{
		return;
	}
	const float Pulse = 0.65f + 0.35f * FMath::Sin(HUD->Now() * 4.f);
	if (MyPS == MutantPS)
	{
		D.Text(TEXT("YOU ARE THE MUTANT"), D.W() * 0.5f, D.S(150.f), ArenaHudStyle::WithAlpha(MutCol, Pulse), D.S(26.f), EArenaAlign::Center);
	}
	else if (MyPS == FeederPS)
	{
		D.Text(TEXT("BOTTOM FEEDER - anyone is fair game"), D.W() * 0.5f, D.S(150.f), ArenaHudStyle::WithAlpha(ArenaMUT::FeederColor(), Pulse), D.S(22.f), EArenaAlign::Center);
	}
	else if (MutantPS)
	{
		D.Text(TEXT("Hunt the Mutant"), D.W() * 0.5f, D.S(150.f), ArenaHudStyle::WithAlpha(ArenaHudStyle::TextWhite(), 0.7f), D.S(18.f), EArenaAlign::Center);
	}
}

FString UArenaRules_MUT::GetScoreboardExtra(const AArenaPlayerState* Player) const
{
	if (!Player)
	{
		return FString();
	}
	FString Role;
	if (Player->ModeFlags & FlagMutant)
	{
		Role = TEXT("MUTANT");
	}
	else if (Player->ModeFlags & FlagBottomFeeder)
	{
		Role = TEXT("FEEDER");
	}
	if (Player->ObjectiveScore > 0)
	{
		const FString Kills = FString::Printf(TEXT("%d as mutant"), Player->ObjectiveScore);
		Role = Role.IsEmpty() ? Kills : Role + TEXT("  ") + Kills;
	}
	return Role;
}

FString UArenaRules_MUT::GetScoreboardHeaderExtra(const AArenaGameState* GS) const
{
	const AArenaPlayerState* MutantPS = FindFlagged(GS, FlagMutant);
	return MutantPS ? FString::Printf(TEXT("Mutant: %s"), *MutantPS->GetPlayerName()) : FString(TEXT("No Mutant yet"));
}
