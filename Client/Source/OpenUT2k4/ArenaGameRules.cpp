#include "ArenaGameRules.h"
#include "ArenaAS_Turret.h"
#include "OpenUT2k4.h"
#include "ArenaGameMode.h"
#include "ArenaGameState.h"
#include "ArenaPlayerState.h"
#include "ArenaCharacter.h"
#include "ArenaBotController.h"
#include "ArenaHUD.h"
#include "ArenaLevelBuilder.h"
#include "ArenaPickup.h"
#include "GameFramework/PlayerStart.h"
#include "Engine/Canvas.h"
#include "Engine/World.h"
#include "EngineUtils.h"

// =============================================================================================
// Registry
// =============================================================================================

namespace
{
	TArray<FArenaRulesEntry>& RegistryStorage()
	{
		// Function-local static: constructed on first use, so registration order across translation
		// units (and unity builds) does not matter.
		static TArray<FArenaRulesEntry> Entries;
		return Entries;
	}

	/** Menu order of the known modes; anything else is listed after them alphabetically. */
	int32 KnownModeOrder(const TCHAR* ModeId)
	{
		static const TCHAR* const Known[] = {
			TEXT("DM"), TEXT("TDM"), TEXT("CTF"), TEXT("BR"), TEXT("DOM"), TEXT("LMS"), TEXT("MUT"), TEXT("INV"), TEXT("AS"), TEXT("ONS")
		};
		for (int32 i = 0; i < UE_ARRAY_COUNT(Known); ++i)
		{
			if (FCString::Stricmp(Known[i], ModeId) == 0)
			{
				return i * 10;
			}
		}
		return 1000;
	}
}

void ArenaGameRulesRegistry::Register(const TCHAR* ModeId, const TCHAR* DisplayName, UClass* (*GetClassFn)(), bool (*SupportsLayoutFn)(const FArenaLayoutInfo&))
{
	if (!ModeId || !*ModeId || !GetClassFn)
	{
		return;
	}
	TArray<FArenaRulesEntry>& Entries = RegistryStorage();
	for (const FArenaRulesEntry& E : Entries)
	{
		if (FCString::Stricmp(E.ModeId, ModeId) == 0)
		{
			return; // first registration wins (no logging here: may run during static init)
		}
	}
	FArenaRulesEntry Entry;
	Entry.ModeId = ModeId;
	Entry.DisplayName = DisplayName ? DisplayName : ModeId;
	Entry.GetClassFn = GetClassFn;
	Entry.SupportsLayoutFn = SupportsLayoutFn;
	Entry.Order = KnownModeOrder(ModeId);
	Entries.Add(Entry);
}

TArray<const FArenaRulesEntry*> ArenaGameRulesRegistry::GetModes()
{
	TArray<const FArenaRulesEntry*> Result;
	for (const FArenaRulesEntry& E : RegistryStorage())
	{
		Result.Add(&E);
	}
	Result.StableSort([](const FArenaRulesEntry& A, const FArenaRulesEntry& B)
	{
		if (A.Order != B.Order)
		{
			return A.Order < B.Order;
		}
		return FCString::Stricmp(A.ModeId, B.ModeId) < 0;
	});
	return Result;
}

const FArenaRulesEntry* ArenaGameRulesRegistry::Find(const FString& ModeId)
{
	for (const FArenaRulesEntry& E : RegistryStorage())
	{
		if (ModeId.Equals(E.ModeId, ESearchCase::IgnoreCase))
		{
			return &E;
		}
	}
	return nullptr;
}

FString ArenaGameRulesRegistry::GetDisplayName(const FString& ModeId)
{
	const FArenaRulesEntry* E = Find(ModeId);
	return E ? FString(E->DisplayName) : FString();
}

bool ArenaGameRulesRegistry::SupportsLayout(const FString& ModeId, const FArenaLayoutInfo& Layout)
{
	const FArenaRulesEntry* E = Find(ModeId);
	return E && E->SupportsLayout(Layout);
}

UArenaGameRules* ArenaGameRulesRegistry::CreateRules(UObject* Outer, const FString& ModeId)
{
	const FArenaRulesEntry* E = Find(ModeId);
	UClass* Class = E ? E->GetRulesClass() : nullptr;
	if (!Class || !Class->IsChildOf(UArenaGameRules::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
	{
		if (!ModeId.IsEmpty())
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("Game rules: unknown mode '%s', using DM"), *ModeId);
		}
		E = Find(TEXT("DM"));
		Class = E ? E->GetRulesClass() : nullptr;
	}
	if (!Class)
	{
		return nullptr;
	}
	UArenaGameRules* Rules = NewObject<UArenaGameRules>(Outer, Class);
	Rules->SetModeId(E->ModeId);
	return Rules;
}

const UArenaGameRules* FArenaRulesEntry::GetDefaults() const
{
	UClass* Class = GetRulesClass();
	return Class ? Cast<UArenaGameRules>(Class->GetDefaultObject()) : nullptr;
}

bool FArenaRulesEntry::SupportsLayout(const FArenaLayoutInfo& Layout) const
{
	if (SupportsLayoutFn)
	{
		return SupportsLayoutFn(Layout);
	}
	const UArenaGameRules* Defaults = GetDefaults();
	return Defaults && Defaults->SupportsLayout(Layout);
}

// =============================================================================================
// UArenaGameRules - identity / accessors
// =============================================================================================

FString UArenaGameRules::GetModeName() const
{
	FString Id = ModeId;
	if (Id.IsEmpty())
	{
		// Class default object: find our registration by class
		for (const FArenaRulesEntry* E : ArenaGameRulesRegistry::GetModes())
		{
			if (E->GetRulesClass() == GetClass())
			{
				return E->DisplayName;
			}
		}
		return GetClass()->GetName();
	}
	const FString Name = ArenaGameRulesRegistry::GetDisplayName(Id);
	return Name.IsEmpty() ? Id : Name;
}

bool UArenaGameRules::SupportsLayout(const FArenaLayoutInfo& Layout) const
{
	return Layout.bSupportsDM;
}

FString UArenaGameRules::GetGoalText(int32 Goal) const
{
	return Goal > 0 ? FString::Printf(TEXT("First to %d"), Goal) : FString();
}

UWorld* UArenaGameRules::GetWorld() const
{
	if (GameMode)
	{
		return GameMode->GetWorld();
	}
	if (const AActor* OuterActor = Cast<AActor>(GetOuter()))
	{
		return OuterActor->GetWorld();
	}
	return nullptr;
}

AArenaGameState* UArenaGameRules::GetGameState() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetGameState<AArenaGameState>() : nullptr;
}

AArenaLevelBuilder* UArenaGameRules::GetLevelBuilder() const
{
	return GameMode ? GameMode->GetLevelBuilder() : nullptr;
}

AArenaNavGraph* UArenaGameRules::GetNavGraph() const
{
	return GameMode ? GameMode->GetNavGraph() : nullptr;
}

int32 UArenaGameRules::GetGoalScore() const
{
	const AArenaGameState* GS = GetGameState();
	return GS ? GS->GoalScore : 0;
}

int32 UArenaGameRules::GetTeamOf(const AController* Controller)
{
	const AArenaPlayerState* PS = Controller ? Controller->GetPlayerState<AArenaPlayerState>() : nullptr;
	return PS ? PS->Team : ArenaTeam::None;
}

// =============================================================================================
// Lifecycle defaults
// =============================================================================================

void UArenaGameRules::InitRules(AArenaGameMode* InGameMode, const FString& Options)
{
	GameMode = InGameMode;
}

int32 UArenaGameRules::ChooseTeam(AArenaPlayerState* PS, int32 PreferredTeam) const
{
	if (!IsTeamGame())
	{
		return ArenaTeam::None;
	}
	int32 Counts[2] = { 0, 0 };
	if (const AArenaGameState* GS = GetGameState())
	{
		for (APlayerState* Other : GS->PlayerArray)
		{
			const AArenaPlayerState* O = Cast<AArenaPlayerState>(Other);
			if (O && O != PS && (O->Team == 0 || O->Team == 1))
			{
				Counts[O->Team]++;
			}
		}
	}
	if (PreferredTeam == 0 || PreferredTeam == 1)
	{
		return Counts[PreferredTeam] <= Counts[1 - PreferredTeam] ? PreferredTeam : 1 - PreferredTeam;
	}
	return Counts[0] <= Counts[1] ? 0 : 1;
}

// =============================================================================================
// Spawning defaults
// =============================================================================================

AActor* UArenaGameRules::ChoosePlayerStart(AController* Player, const TArray<APlayerStart*>& Candidates)
{
	const int32 Team = GetTeamOf(Player);
	int32 Side = ArenaTeam::None;
	if (IsTeamGame() && (Team == 0 || Team == 1))
	{
		Side = GameMode ? GameMode->GetTeamSide(Team) : Team;
	}
	return PickBestStart(Player, Candidates, Side);
}

AActor* UArenaGameRules::PickBestStart(AController* Player, const TArray<APlayerStart*>& InCandidates, int32 SideFilter) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	const int32 Team = GetTeamOf(Player);
	const bool bTeamGame = IsTeamGame();

	TArray<APlayerStart*> Candidates = InCandidates;
	// Prefer the given side's starts
	if (SideFilter == 0 || SideFilter == 1)
	{
		const FName Tag = SideFilter == 0 ? FName(TEXT("Red")) : FName(TEXT("Blue"));
		TArray<APlayerStart*> SideStarts = Candidates.FilterByPredicate([&](const APlayerStart* S) { return S && S->PlayerStartTag == Tag; });
		if (SideStarts.Num() > 0)
		{
			Candidates = MoveTemp(SideStarts);
		}
	}

	APlayerStart* Best = nullptr;
	float BestScore = -BIG_NUMBER;
	for (APlayerStart* Start : Candidates)
	{
		if (!Start)
		{
			continue;
		}
		const FVector Loc = Start->GetActorLocation();
		float NearestEnemy = 100000.f;
		bool bBlocked = false;
		for (TActorIterator<AArenaCharacter> It(World); It; ++It)
		{
			AArenaCharacter* C = *It;
			if (!C->IsAlive())
			{
				continue;
			}
			const float Dist = FVector::Dist(C->GetActorLocation(), Loc);
			if (Dist < 120.f)
			{
				bBlocked = true;
			}
			if (!(bTeamGame && C->GetTeam() == Team))
			{
				NearestEnemy = FMath::Min(NearestEnemy, Dist);
			}
		}
		float Score = FMath::Min(NearestEnemy, 3000.f) + FMath::FRandRange(0.f, 1200.f);
		if (bBlocked)
		{
			Score -= 100000.f;
		}
		if (Score > BestScore)
		{
			BestScore = Score;
			Best = Start;
		}
	}
	return Best;
}

float UArenaGameRules::GetRespawnDelay(AController* Controller) const
{
	// Humans may respawn after a short delay (fire); bots respawn on their own
	const bool bIsBot = Cast<AArenaBotController>(Controller) != nullptr;
	return bIsBot ? FMath::FRandRange(1.5f, 2.5f) : 1.0f;
}

float UArenaGameRules::GetForcedRespawnDelay(AController* Controller) const
{
	const bool bIsBot = Cast<AArenaBotController>(Controller) != nullptr;
	return bIsBot ? 0.f : 4.f;
}

bool UArenaGameRules::AllowPickup(EArenaPickupType Type, TSubclassOf<AArenaWeapon> WeaponClass) const
{
	if (GameMode && GameMode->IsInstagib() && Type != EArenaPickupType::Adrenaline)
	{
		return false;
	}
	return true;
}

// =============================================================================================
// Combat / scoring defaults
// =============================================================================================

float UArenaGameRules::ModifyDamage(float Damage, AController* InstigatorController, AArenaCharacter* Victim) const
{
	if (!GameMode || !GameMode->IsPlaying() || !Victim)
	{
		return 0.f;
	}
	const bool bSelf = InstigatorController && InstigatorController == Victim->GetController();
	// DeathMatch.ReduceDamage: spawn protection stops weapon damage from others (not self damage or the world)
	if (Victim->HasSpawnProtection() && InstigatorController && !bSelf)
	{
		return 0.f;
	}
	if (!bSelf && AreOnSameTeam(InstigatorController, Victim->GetController()))
	{
		return 0.f;
	}
	if (GameMode->IsInstagib())
	{
		return bSelf ? 0.f : 1000.f;
	}
	return Damage;
}

bool UArenaGameRules::AreOnSameTeam(const AController* A, const AController* B) const
{
	if (!IsTeamGame() || !A || !B)
	{
		return false;
	}
	const AArenaPlayerState* PA = A->GetPlayerState<AArenaPlayerState>();
	const AArenaPlayerState* PB = B->GetPlayerState<AArenaPlayerState>();
	// automatic turrets (Assault) have a team but no player state
	const int32 TA = PA ? PA->Team : AArenaAS_TurretController::TeamOf(A);
	const int32 TB = PB ? PB->Team : AArenaAS_TurretController::TeamOf(B);
	return TA >= 0 && TA == TB;
}

void UArenaGameRules::CheckEndConditions()
{
	AArenaGameState* GS = GetGameState();
	if (!GameMode || !GS || !GS->IsPlaying())
	{
		return;
	}
	const int32 Goal = GS->GoalScore;

	if (IsTeamGame())
	{
		for (int32 Team = 0; Team < 2; ++Team)
		{
			if ((Goal > 0 && GS->TeamScores[Team] >= Goal) || (GS->bOvertime && GS->TeamScores[Team] > GS->TeamScores[1 - Team]))
			{
				GameMode->EndMatch(FString::Printf(TEXT("%s Team wins the match!"), *ArenaTeam::Name(Team)), Team, nullptr);
				return;
			}
		}
	}
	else
	{
		const TArray<AArenaPlayerState*> Sorted = GS->GetSortedPlayers();
		if (Sorted.Num() > 0)
		{
			const bool bLeaderAlone = Sorted.Num() == 1 || Sorted[0]->GetScore() > Sorted[1]->GetScore();
			if ((Goal > 0 && Sorted[0]->GetScore() >= Goal) || (GS->bOvertime && bLeaderAlone))
			{
				GameMode->EndMatch(FString::Printf(TEXT("%s wins the match!"), *Sorted[0]->GetPlayerName()), -1, Sorted[0]);
			}
		}
	}
}

void UArenaGameRules::OnTimeLimitReached()
{
	AArenaGameState* GS = GetGameState();
	if (!GameMode || !GS)
	{
		return;
	}
	bool bTie = false;
	if (IsTeamGame())
	{
		bTie = GS->TeamScores[0] == GS->TeamScores[1];
	}
	else
	{
		const TArray<AArenaPlayerState*> Sorted = GS->GetSortedPlayers();
		bTie = Sorted.Num() > 1 && Sorted[0]->GetScore() == Sorted[1]->GetScore();
	}

	if (bTie)
	{
		GS->bOvertime = true;
		GameMode->BroadcastCenterMessage(TEXT("Overtime! Next score wins."), FLinearColor(1.f, 0.6f, 0.1f), 3.f, TEXT("S_Countdown"));
		return;
	}

	GS->bOvertime = true; // forces CheckEndConditions to pick the leader
	GameMode->CheckEndConditions();
}

// =============================================================================================
// Bots
// =============================================================================================

void UArenaGameRules::GetBotObjectives(AArenaBotController* Bot, TArray<FArenaBotObjective>& OutGoals)
{
	FArenaBotObjective Goal;
	if (GetBotObjective(Bot, Goal))
	{
		OutGoals.Add(Goal);
	}
}

bool UArenaGameRules::UpdateBotObjective(AArenaBotController* Bot, FArenaBotObjective& Objective)
{
	if (Objective.bTrackActor)
	{
		const AActor* Actor = Objective.Actor.Get();
		if (!IsValid(Actor))
		{
			return false;
		}
		Objective.Location = Actor->GetActorLocation();
	}
	return true;
}

// =============================================================================================
// HUD
// =============================================================================================

void UArenaGameRules::DrawHUD(AArenaHUD* HUD, UCanvas* Canvas)
{
	const AArenaGameState* GS = GetGameState();
	if (!HUD || !GS || GS->ModeStatusLines.Num() == 0 || HUD->IsUTHud())
	{
		return;
	}
	const FArenaDraw& D = HUD->GetDraw();
	const float Right = D.W() - D.S(24.f);
	float Y = HUD->GetScoreInfoBottom() + D.S(28.f);
	for (const FString& Line : GS->ModeStatusLines)
	{
		D.Text(Line, Right, Y, ArenaHudStyle::TextWhite(), D.S(16.f), EArenaAlign::Right);
		Y += D.S(22.f);
	}
}

FString UArenaGameRules::GetDeadScreenMessage(const AArenaPlayerState* Player) const
{
	return TEXT("Press FIRE to respawn");
}
