#include "ArenaSinglePlayer.h"
#include "ArenaSinglePlayerPrivate.h"
#include "ArenaGUI2K4Pages.h"
#include "ArenaGameMode.h"
#include "ArenaGameState.h"
#include "ArenaPlayerState.h"
#include "ArenaMatchStats.h"
#include "ArenaBotController.h"
#include "ArenaLevelBuilder.h"
#include "ArenaUserSettings.h"
#include "ArenaArt.h"
#include "ArenaRuntimeContent.h"
#include "OpenUT2k4.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Containers/Ticker.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Sound/SoundBase.h"
#include "UnrealClient.h"
#include "Misc/Paths.h"

using namespace ArenaGUI2K4;
using namespace ArenaSP;
using namespace ArenaSPUI;

// =================================================================================================
// Session: the match in progress (process lifetime, survives the level change to the match and back)
// =================================================================================================

namespace ArenaSPUI
{
	FSession& Session()
	{
		static FSession S;
		return S;
	}

	FString Caption(const TCHAR* Section, const TCHAR* Key, const TCHAR* Fallback)
	{
		return ArenaSP::Loc(TEXT("GUI2K4"), Section, Key, Fallback).Replace(TEXT("|"), TEXT("\n"));
	}

	FString Lines(const FString& S)
	{
		return S.Replace(TEXT("|"), TEXT("\n"));
	}

	FString MapTitle(const FString& Map)
	{
		FArenaLayoutInfo L;
		if (AArenaLevelBuilder::FindLayout(Map, L) && !L.DisplayName.IsEmpty())
		{
			return L.DisplayName;
		}
		return Map;
	}

	bool HasMap(const FString& Map)
	{
		FArenaLayoutInfo L;
		return !Map.IsEmpty() && AArenaLevelBuilder::FindLayout(Map, L);
	}

	FString MapShot(const FString& Map)
	{
		const FString Mat = ArenaGUI2K4Pages::GetMapScreenshot(Map);
		if (Mat.IsEmpty())
		{
			return FString();
		}
		const FAnimatedImage& Img = FAnimatedImage::Get(Mat);
		return Img.Items.Num() > 0 ? Img.Items[0].Texture : Mat;
	}

	FString PortraitOf(const FString& Character)
	{
		const FPlayerRecord* P = FindPlayer(Character);
		if (!P || P->Portrait.IsEmpty())
		{
			return FString(TEXT("PlayerPictures.cDefault"));
		}
		// custom characters' portraits are often shaders: their base texture
		const FString Path = ResolveObjectPath(P->Portrait);
		const FAnimatedImage& Img = FAnimatedImage::Get(Path);
		return Img.Items.Num() > 0 ? Img.Items[0].Texture : Path;
	}

	FString TeamTitle(const FString& RosterClass)
	{
		const FTeamRoster* T = FindTeam(RosterClass);
		return T && !T->TeamName.IsEmpty() ? T->TeamName : RosterClass;
	}

	FString MatchTitle(const FMatchInfo& M)
	{
		return !M.MenuName.IsEmpty() ? M.MenuName : MapTitle(M.LevelName);
	}

	/** The UE2 mesh of a character as our player model ("HumanMaleA.EgyptMaleB"), for the bot / player model overrides. */
	FString MeshOf(const FString& Character)
	{
		const FPlayerRecord* P = FindPlayer(Character);
		return P ? P->Mesh : FString();
	}

	FString OptionSafe(const FString& S)
	{
		FString R = S;
		for (TCHAR& C : R)
		{
			if (C == TEXT('?') || C == TEXT('=') || C == TEXT(',') || C == TEXT('#') || C == TEXT(' '))
			{
				C = TEXT('_');
			}
		}
		return R;
	}
}

bool ArenaSinglePlayer::HasPendingMatch()
{
	return Session().bActive;
}

bool FArenaSinglePlayerPage::ShouldOpenOnMenuStart()
{
	FSession& S = Session();
	return S.bActive || S.bReturnToPage;
}

namespace ArenaSPUI
{
	/** Watches the match of the session: teams and names before the start, sprees, the result at the end, test hooks. */
	bool TickMatch(float DeltaTime)
	{
		FSession& S = Session();
		if (!S.bActive || S.bResultReady || !GEngine)
		{
			return true;
		}
		UWorld* World = nullptr;
		for (const FWorldContext& C : GEngine->GetWorldContexts())
		{
			if (C.WorldType == EWorldType::Game || C.WorldType == EWorldType::PIE)
			{
				World = C.World();
				break;
			}
		}
		AArenaGameMode* GM = World ? World->GetAuthGameMode<AArenaGameMode>() : nullptr;
		AArenaGameState* GS = World ? World->GetGameState<AArenaGameState>() : nullptr;
		if (!GM || !GS)
		{
			return true;
		}
		APlayerController* PC = World->GetFirstPlayerController();
		AArenaPlayerState* Me = PC ? PC->GetPlayerState<AArenaPlayerState>() : nullptr;
		if (!Me)
		{
			return true;
		}
		if (S.MatchWorld != World)
		{
			S.MatchWorld = World;
			S.bSetup = false;
			S.PlayTime = 0.f;
			S.LastSpree = 0;
			S.LastMulti = 0;
			FMemory::Memzero(S.Result.Spree);
			FMemory::Memzero(S.Result.MultiKills);
		}
		// names and teams of the ladder (bots are named by the game mode from ?BotNames=; this enforces the teams)
		if (!S.bSetup && GS->Phase == EArenaMatchPhase::Countdown)
		{
			S.bSetup = true;
			if (!S.PlayerName.IsEmpty() && Me->GetPlayerName() != S.PlayerName)
			{
				GM->ChangeName(PC, S.PlayerName, false);
			}
			if (S.bTeamGame && Me->Team != S.MyTeam)
			{
				GM->SetPlayerTeam(PC, S.MyTeam);
			}
			for (APlayerState* P : GS->PlayerArray)
			{
				AArenaPlayerState* A = Cast<AArenaPlayerState>(P);
				// the character's skill profile (xRosterEntry from the .upl record: Accuracy, ReactionTime, Jumpiness,
				// StrafingAbility; the values the Bot code adds to its skill)
				if (AArenaBotController* Bot = A ? Cast<AArenaBotController>(A->GetOwner()) : nullptr)
				{
					if (const FPlayerRecord* Rec = FindPlayer(A->GetPlayerName()))
					{
						Bot->Accuracy = Rec->Accuracy;
						Bot->ReactionTimeAdjust = Rec->ReactionTime;
						Bot->Jumpiness = Rec->Jumpiness;
						Bot->StrafingAbility = Rec->StrafingAbility;
						Bot->SetAggressiveness(Rec->Aggressiveness);
						Bot->SetCombatStyle(Rec->CombatStyle);
						Bot->SetTactics(Rec->Tactics);
						if (!Rec->FavoriteWeapon.IsEmpty())
						{
							Bot->SetFavoriteWeaponUE2(Rec->FavoriteWeapon);
						}
						UE_LOG(LogOpenUT2k4, Log, TEXT("SinglePlayer: bot %s: accuracy %.2f, reaction %.2f, jumpiness %.2f, strafing %.2f, aggressiveness %.2f, combat style %.2f, tactics %.2f, favorite %s"),
							*A->GetPlayerName(), Rec->Accuracy, Rec->ReactionTime, Rec->Jumpiness, Rec->StrafingAbility, Rec->Aggressiveness, Rec->CombatStyle, Rec->Tactics,
							*Rec->FavoriteWeapon);
					}
				}
				const int32* Team = A ? S.BotTeams.Find(A->GetPlayerName()) : nullptr;
				if (A && Team && S.bTeamGame && A->Team != *Team)
				{
					if (AController* C = Cast<AController>(A->GetOwner()))
					{
						GM->SetPlayerTeam(C, *Team);
					}
				}
			}
			UE_LOG(LogOpenUT2k4, Log, TEXT("SinglePlayer: match set up: %d players, our team %d"), GS->PlayerArray.Num(), Me->Team);
		}
		if (GS->Phase == EArenaMatchPhase::Playing || GS->Phase == EArenaMatchPhase::RoundOver)
		{
			S.PlayTime += DeltaTime;
			// killing sprees (5, 10 .. 30 kills) and multi kills (2 .. 8+ in a row)
			if (Me->Spree > S.LastSpree && Me->Spree % 5 == 0)
			{
				const int32 Level = FMath::Clamp(Me->Spree / 5 - 1, 0, 5);
				S.Result.Spree[Level]++;
			}
			S.LastSpree = Me->Spree;
			if (Me->MultiKillCount > S.LastMulti && Me->MultiKillCount >= 2)
			{
				S.Result.MultiKills[FMath::Clamp(Me->MultiKillCount - 2, 0, 6)]++;
			}
			S.LastMulti = Me->MultiKillCount;
#if !UE_BUILD_SHIPPING
			// -SPAutoWin=<sec> / -SPAutoLose=<sec>: end the match after that much play time (tests)
			float WinAt = -1.f, LoseAt = -1.f;
			const bool bWin = FParse::Value(FCommandLine::Get(), TEXT("SPAutoWin="), WinAt) && S.PlayTime >= WinAt;
			const bool bLose = !bWin && FParse::Value(FCommandLine::Get(), TEXT("SPAutoLose="), LoseAt) && S.PlayTime >= LoseAt;
			if ((bWin || bLose) && GS->Phase == EArenaMatchPhase::Playing)
			{
				if (GS->bTeamGame)
				{
					const int32 Winner = bWin ? Me->Team : 1 - Me->Team;
					GM->SetTeamScore(Winner, FMath::Max(GS->GoalScore, GS->TeamScores[1 - Winner] + 1));
					GM->EndMatch(FString::Printf(TEXT("%s wins! (test)"), Winner == 0 ? TEXT("Red Team") : TEXT("Blue Team")), Winner, nullptr);
				}
				else
				{
					APlayerState* Best = nullptr;
					float Top = 0.f;
					for (APlayerState* P : GS->PlayerArray)
					{
						Top = FMath::Max(Top, P->GetScore());
						if (!bWin && P != Me && !Best)
						{
							Best = P;
						}
					}
					if (bWin)
					{
						Me->SetScore(Top + 1.f);
						Best = Me;
					}
					else if (Best)
					{
						Best->SetScore(FMath::Max(Top, Me->GetScore()) + 1.f);
					}
					GM->EndMatch(FString::Printf(TEXT("%s wins the match! (test)"), Best ? *Best->GetPlayerName() : TEXT("?")), -1, Best);
				}
			}
#endif
		}
		if (GS->Phase == EArenaMatchPhase::Ended)
		{
			FMatchResult& R = S.Result;
			R.bValid = true;
			R.bTeamGame = GS->bTeamGame;
			R.MyTeam = Me->Team >= 0 ? Me->Team : S.MyTeam;
			R.TeamScore[0] = float(GS->TeamScores[0]);
			R.TeamScore[1] = float(GS->TeamScores[1]);
			R.bWon = GS->bTeamGame ? (GS->WinningTeam == R.MyTeam) : (GS->WinningPlayer == Me);
			R.GameTime = S.PlayTime;
			R.Map = S.Map;
			R.GameName = GameTypeName(S.GameType);
			R.Players.Reset();
			for (APlayerState* P : GS->PlayerArray)
			{
				const AArenaPlayerState* A = Cast<AArenaPlayerState>(P);
				if (!A)
				{
					continue;
				}
				FMatchResult::FPlayer X;
				X.Id = A->GetPlayerId();
				X.Name = A->GetPlayerName();
				X.Kills = float(A->Kills);
				X.Deaths = float(A->Deaths);
				X.Score = A->GetScore();
				X.Goals = A->Captures + A->ObjectiveScore;
				X.Team = GS->bTeamGame ? A->Team : -1;
				X.bMe = A == Me;
				const FArenaPlayerMatchStats St = UArenaMatchStats::Get(World, A);
				X.FlakCount = St.FlakKills;
				X.ComboCount = St.Combos;
				X.HeadCount = St.HeadShots;
				X.RanOverCount = St.RoadKills;
#if !UE_BUILD_SHIPPING
				// -SPTestAwards: the player reaches every special award threshold (tests the award path)
				if (X.bMe && FParse::Param(FCommandLine::Get(), TEXT("SPTestAwards")))
				{
					const FProfileDefaults& Def = Get().Defaults;
					X.FlakCount = FMath::Max(X.FlakCount, Def.AwardFlakMonkey);
					X.ComboCount = FMath::Max(X.ComboCount, Def.AwardComboWhore);
					X.HeadCount = FMath::Max(X.HeadCount, Def.AwardHeadHunter);
					X.RanOverCount = FMath::Max(X.RanOverCount, Def.AwardRoadRampage);
				}
#endif
				R.Players.Add(X);
			}
			S.bResultReady = true;
			for (const FMatchResult::FPlayer& X : R.Players)
			{
				UE_LOG(LogOpenUT2k4, Log, TEXT("SinglePlayer: %s: kills %.0f, deaths %.0f, score %.0f, flak %d, combos %d, headshots %d, roadkills %d"), *X.Name,
					X.Kills, X.Deaths, X.Score, X.FlakCount, X.ComboCount, X.HeadCount, X.RanOverCount);
			}
			UE_LOG(LogOpenUT2k4, Log, TEXT("SinglePlayer: match over: %s (team %d, scores %d - %d, %.0f s)"), R.bWon ? TEXT("WON") : TEXT("lost"),
				R.MyTeam, GS->TeamScores[0], GS->TeamScores[1], R.GameTime);
#if !UE_BUILD_SHIPPING
			if (FCString::Stristr(FCommandLine::Get(), TEXT("SPAutoWin=")) || FCString::Stristr(FCommandLine::Get(), TEXT("SPAutoLose=")))
			{
				// tests: back to the menu shortly instead of the 20 s end of match screen
				TWeakObjectPtr<AArenaGameMode> WeakGM = GM;
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakGM](float)
				{
					if (AArenaGameMode* M = WeakGM.Get())
					{
						M->ReturnToMenu();
					}
					return false;
				}), 4.f);
			}
#endif
		}
		return true;
	}

	void EnsureTicker()
	{
		static bool bRegistered = false;
		if (!bRegistered)
		{
			bRegistered = true;
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickMatch), 0.f);
		}
	}
}

// =================================================================================================
// Page
// =================================================================================================

FArenaSinglePlayerPage::FArenaSinglePlayerPage(AHUD* InOwner)
	: Owner(InOwner)
{
}

FArenaSinglePlayerPage::~FArenaSinglePlayerPage() = default;

void FArenaSinglePlayerPage::OnOpened()
{
	const FData& D = Get();
	SPCharacters.Reset();
	for (const FPlayerRecord& P : D.Players)
	{
		// the SP characters, and the ones the tournament unlocked (we have no separate character select for them)
		if (P.Menu.Equals(TEXT("SP"), ESearchCase::IgnoreCase) || IsUnlocked(P.Menu))
		{
			SPCharacters.Add(P.DefaultName);
		}
	}
	ProfileNames = FProfile::List();
	FSession& S = Session();
	S.bReturnToPage = false;
	// back from a match: register its result
	if (S.bActive)
	{
		S.bActive = false;
		LoadProfile(S.ProfileName);
		if (GP)
		{
			if (S.bResultReady && S.Result.bValid)
			{
				GP->RegisterGame(S.Result);
			}
			else
			{
				// left before the end: the forfeit fee stays with the team mates
				UE_LOG(LogOpenUT2k4, Log, TEXT("SinglePlayer: the match was left before its end, forfeit fee %d kept"), GP->ForfeitFee);
				GP->ForfeitFee = 0;
				GP->bInLadderGame = false;
				GP->bIsChallenge = false;
			}
			SaveProfile();
			ProcessProfileEvents();
		}
		S.Result = FMatchResult();
		S.bResultReady = false;
	}
	else if (!GP && !S.LastProfile.IsEmpty() && FProfile::Exists(S.LastProfile))
	{
		LoadProfile(S.LastProfile);
	}
	else if (!GP)
	{
		// UT2004 opens on the last profile; the most recent file here
		const FString Last = UArenaUserSettings::Get() ? FString() : FString();
		(void)Last;
	}
	if (ProfileNames.Num() == 0 && !GP)
	{
		bCreatingProfile = true;
	}
	RebuildTabs();
#if !UE_BUILD_SHIPPING
	// test scripts: -SPScript on the first opening, -SPScriptAfterMatch<n> after the n-th match of the process
	{
		static int32 Openings = 0;
		static int32 MatchesSeen = 0;
		FString Text;
		const bool bAfterMatch = Session().MatchesStarted > MatchesSeen;
		if (bAfterMatch)
		{
			MatchesSeen = Session().MatchesStarted;
			FParse::Value(FCommandLine::Get(), *FString::Printf(TEXT("SPScriptAfterMatch%d="), MatchesSeen), Text, false);
		}
		else if (Openings == 0)
		{
			FParse::Value(FCommandLine::Get(), TEXT("SPScript="), Text, false);
		}
		++Openings;
		Script.Reset();
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(";"));
		for (const FString& P : Parts)
		{
			FString At, Cmd, Name, Value;
			if (P.Split(TEXT(":"), &At, &Cmd))
			{
				FScriptCmd C;
				C.At = FCString::Atof(*At);
				if (!Cmd.Split(TEXT("="), &C.Name, &C.Value))
				{
					C.Name = Cmd;
				}
				Script.Add(C);
			}
		}
		ScriptStart = -1.0;
		bScriptPending = Script.Num() > 0;
	}
#endif
}

void FArenaSinglePlayerPage::TickScript()
{
#if !UE_BUILD_SHIPPING
	if (!bScriptPending || !G)
	{
		return;
	}
	if (ScriptStart < 0.0)
	{
		ScriptStart = G->Time();
	}
	const float Now = float(G->Time() - ScriptStart);
	bool bAll = true;
	for (FScriptCmd& C : Script)
	{
		if (C.bDone)
		{
			continue;
		}
		if (Now < C.At)
		{
			bAll = false;
			continue;
		}
		C.bDone = true;
		if (C.Name == TEXT("Shot"))
		{
			const FString Path = FPaths::ScreenShotDir() / FString::Printf(TEXT("sp_%s.png"), *C.Value);
			FScreenshotRequest::RequestScreenshot(Path, false, false);
			UE_LOG(LogOpenUT2k4, Log, TEXT("SinglePlayer: screenshot %s"), *Path);
		}
		else if (C.Name == TEXT("Quit"))
		{
			if (AHUD* HUD = Owner.Get())
			{
				GEngine->Exec(HUD->GetWorld(), TEXT("quit"));
			}
		}
		else if (C.Name == TEXT("Key"))
		{
			FGUI::InjectKey(FKey(*C.Value));
		}
		else if (C.Name == TEXT("Click"))
		{
			FString X, Y;
			if (C.Value.Split(TEXT(","), &X, &Y))
			{
				G->InjectClick(FCString::Atof(*X), FCString::Atof(*Y));
			}
		}
		else
		{
			ApplyTestCommand(C.Name, C.Value);
		}
	}
	bScriptPending = !bAll;
#endif
}

void FArenaSinglePlayerPage::LoadProfile(const FString& Name)
{
	GP = FProfile::Load(Name);
	Session().LastProfile = GP ? Name : FString();
	SelLadder = -1;
	SelRung = -1;
	ShownLadder = -1;
	bInitialDraft = false;
	NewTeam.Reset();
	if (GP)
	{
		const int32 Idx = ProfileNames.IndexOfByPredicate([&Name](const FString& N) { return N.Equals(Name, ESearchCase::IgnoreCase); });
		ProfileSel = FMath::Max(0, Idx);
	}
}

void FArenaSinglePlayerPage::SaveProfile()
{
	if (GP)
	{
		GP->Save();
	}
}

void FArenaSinglePlayerPage::ProcessProfileEvents()
{
	if (!GP)
	{
		return;
	}
	const bool bDetails = GP->bInLadderGame && GP->bShowDetails && GP->bLmdFresh;
	const TArray<FMenuRequest> Requests = GP->HandleSpecialEvents();
	SaveProfile();
	// UT2K4SP_Main.HandleGameProfile opens the details first; the other pages open on top (the last request ends up
	// on top in UT2004's menu stack, so dialogs are queued in reverse here: the first one is shown first)
	TArray<FDialog> Queue;
	if (bDetails)
	{
		FDialog D;
		D.Kind = EDialog::Details;
		Queue.Add(D);
	}
	for (const FMenuRequest& R : Requests)
	{
		FDialog D;
		switch (R.Kind)
		{
		case FMenuRequest::Message:
			D.Kind = EDialog::Message;
			D.Index = R.Index;
			break;
		case FMenuRequest::Draft:
			bInitialDraft = true;
			RebuildTabs();
			SetActiveTab(ETab::TeamManagement);
			continue;
		case FMenuRequest::Injury:
			D.Kind = EDialog::Injury;
			D.Index = R.Index;
			break;
		case FMenuRequest::Challenge:
			D.Kind = EDialog::Challenge;
			D.Param1 = R.Param1;
			D.ChallengeClass = R.Param2;
			D.bChallenged = true;
			break;
		case FMenuRequest::Donation:
			D.Kind = EDialog::Question;
			D.Text = Lines(ArenaSP::LocArray(TEXT("GUI2K4"), TEXT("UT2K4SP_Main"), TEXT("DonationMsg"), FMath::RandRange(0, 1),
				TEXT("You were almost broke, but an anonymous benefactor donated some credits so you can continue your tournament.")));
			break;
		case FMenuRequest::HighScore:
			D.Kind = EDialog::HighScores;
			break;
		case FMenuRequest::Trade:
			D.Kind = EDialog::Trade;
			D.Param1 = R.Param1;
			D.Param2 = R.Param2;
			break;
		case FMenuRequest::UnTrade:
			D.Kind = EDialog::UnTrade;
			D.Param1 = R.Param1;
			D.Param2 = R.Param2;
			break;
		default:
			continue;
		}
		Queue.Add(D);
	}
	for (int32 i = Queue.Num() - 1; i >= 0; --i)
	{
		PushDialog(Queue[i]);
	}
	RebuildTabs();
}

void FArenaSinglePlayerPage::PushDialog(const FDialog& D)
{
	FDialog Copy = D;
	Copy.OpenedAt = G ? G->Time() : 0.0;
	Dialogs.Add(Copy);
	if (G)
	{
		G->ResetInteraction();
		G->SetFocus(0);
	}
}

void FArenaSinglePlayerPage::PushQuestion(const FString& Text, EQuestion Q, bool bYesNo)
{
	FDialog D;
	D.Kind = EDialog::Question;
	D.Text = Text;
	D.Question = Q;
	D.bYesNo = bYesNo;
	PushDialog(D);
}

void FArenaSinglePlayerPage::CloseDialog()
{
	if (Dialogs.Num() > 0)
	{
		Dialogs.Pop();
	}
	if (G)
	{
		G->ResetInteraction();
		G->SetFocus(0);
	}
}

// ---- tabs ----

void FArenaSinglePlayerPage::RebuildTabs()
{
	const ETab Prev = ActiveTab();
	const int32 OldNum = Tabs.Num();
	Tabs.Reset();
	// UT2K4SP_Main: Profile (or New profile), Tutorials, then with a profile Qualification (or the draft's Team
	// Management in its place), Team Qualification once open, Ladder after the team qualification, Team Management,
	// Additional ladders after the championship
	Tabs.Add(bCreatingProfile ? ETab::NewProfile : ETab::Profile);
	Tabs.Add(ETab::Tutorials);
	ETab Want = Prev;
	if (GP)
	{
		Tabs.Add(bInitialDraft ? ETab::TeamManagement : ETab::Qualification);
		if (GP->GetLadderProgress(LID_TDM) != -1)
		{
			Tabs.Add(ETab::TeamQualification);
		}
		if (GP->CompletedLadder(LID_TDM))
		{
			Tabs.Add(ETab::Ladder);
		}
		if (!bInitialDraft && (GP->GetLadderProgress(LID_TDM) != -1))
		{
			Tabs.Add(ETab::TeamManagement);
		}
		if (GP->bCompleted && Get().CustomLadders.Num() > 0)
		{
			Tabs.Add(ETab::Additional);
		}
		if (OldNum <= 2)
		{
			// a profile was just loaded: the furthest ladder (UpdateTabs' last active tab)
			Want = GP->CompletedLadder(LID_TDM) ? ETab::Ladder : (GP->GetLadderProgress(LID_TDM) != -1 && GP->Balance > GP->MinBalance ? ETab::TeamQualification : ETab::Qualification);
			if (bInitialDraft)
			{
				Want = ETab::TeamManagement;
			}
		}
	}
	SetActiveTab(Want);
}

FArenaSinglePlayerPage::ETab FArenaSinglePlayerPage::ActiveTab() const
{
	return Tabs.IsValidIndex(TabIndex) ? Tabs[TabIndex] : ETab::Profile;
}

void FArenaSinglePlayerPage::SetActiveTab(ETab T)
{
	int32 I = Tabs.IndexOfByKey(T);
	if (I == INDEX_NONE)
	{
		if (T == ETab::NewProfile || T == ETab::Profile)
		{
			I = 0;
		}
		else
		{
			I = Tabs.IsValidIndex(TabIndex) ? TabIndex : 0;
		}
	}
	if (I != TabIndex)
	{
		TabIndex = I;
		TabChangedAt = G ? G->Time() : 0.0;
		ShownLadder = -1;
		if (G)
		{
			G->ResetInteraction();
		}
	}
}

// ---- match selection and start ----

void FArenaSinglePlayerPage::SelectMatch(int32 Ladder, int32 Rung)
{
	SelLadder = Ladder;
	SelRung = Rung;
	if (GP)
	{
		GP->CurrentLadder = Ladder;
		GP->CurrentMenuRung = Rung;
	}
}

void FArenaSinglePlayerPage::SelectNextMatch(int32 Ladder)
{
	if (!GP)
	{
		return;
	}
	const int32 Len = LengthOfLadder(Ladder);
	if (Len <= 0)
	{
		return;
	}
	int32 Offset = FMath::Min(GP->GetLadderProgress(Ladder), Len - 1);
	// UT2K4SPTab_Qualification: a new profile starts on the tutorial
	if (Ladder == LID_DM && GP->TotalTime == 0.f && GP->GetLadderProgress(LID_DM) <= 1)
	{
		Offset = 0;
	}
	if (Offset < 0)
	{
		SelectMatch(Ladder, 0);
		return;
	}
	while (Offset >= 0)
	{
		if (GP->GetMinimalEntryFeeFor(GP->GetMatchInfo(Ladder, Offset)) <= GP->Balance)
		{
			SelectMatch(Ladder, Offset);
			return;
		}
		--Offset;
	}
	SelectMatch(Ladder, 0);
}

void FArenaSinglePlayerPage::PlaySelectedMatch()
{
	if (!GP || SelLadder < 0 || SelRung < 0 || SelRung > GP->GetLadderProgress(SelLadder))
	{
		return;
	}
	const FMatchInfo M = GP->GetMatchInfo(SelLadder, SelRung);
	// UT2K4SP_Main.HandleRequirements
	if (M.Requirements.Contains(TEXT("FULLTEAM")) && !GP->HasFullTeam())
	{
		bInitialDraft = !GP->CompletedLadder(LID_DM);
		RebuildTabs();
		SetActiveTab(ETab::TeamManagement);
		PushQuestion(Caption(TEXT("UT2K4SP_Main"), TEXT("msgFullTeamRequired"), TEXT("A full team is required to play this match.")));
		return;
	}
	GP->bIsChallenge = false;
	if (M.EntryFee > GP->Balance)
	{
		return;
	}
	if (!HasMap(M.LevelName))
	{
		PushQuestion(FString::Printf(TEXT("%s is not in your converted content, so this match cannot be played."), *M.LevelName));
		return;
	}
	const FString Mode = GameTypeToMode(M.GameType);
	if (Mode.IsEmpty())
	{
		PushQuestion(FString::Printf(TEXT("The game type %s is not supported yet."), *GameTypeName(M.GameType)));
		return;
	}
	const int32 Mates = GP->GetNumTeammatesForMatch(M);
	if (Mates > 0)
	{
		const int32 Healthy = GP->NumHealthyTeammates();
		if (Healthy < Mates)
		{
			PushQuestion(Lines(ArenaSP::Loc(TEXT("GUI2K4"), TEXT("UT2K4SPTab_LadderBase"), TEXT("NotEnoughPlayers"),
				TEXT("You do not have enough healthy team mates for this match.|%teammates% healthy team mates are required, you only have %healthy%."))
				.Replace(TEXT("%healthy%"), *FString::FromInt(Healthy)).Replace(TEXT("%teammates%"), *FString::FromInt(Mates))));
			return;
		}
		FDialog D;
		D.Kind = EDialog::TeamRoles;
		PushDialog(D);
		return;
	}
	StartMatch();
}

void FArenaSinglePlayerPage::StartMatch()
{
	AHUD* HUD = Owner.Get();
	if (!GP || !HUD)
	{
		return;
	}
	if (!GP->bIsChallenge)
	{
		GP->CurrentLadder = SelLadder;
		GP->CurrentMenuRung = SelRung;
		GP->Balance -= GP->GetMatchInfo(SelLadder, SelRung).EntryFee;
	}
	GP->StartNewMatch(GP->bIsChallenge ? -1 : SelLadder);
	const FMatchInfo M = GP->GetCurrentMatchInfo();
	const FString Mode = GameTypeToMode(M.GameType);
	const bool bTeam = IsTeamGameType(M.GameType);
	const FString Short = [&M]() { FString S = M.GameType; M.GameType.Split(TEXT("."), nullptr, &S); return S; }();
	const bool bBoss = Short.Equals(TEXT("BossDM"), ESearchCase::IgnoreCase);

	// the bots: UT2004's rosters (team leader first; the deathmatch rosters pick at random)
	FSession& S = Session();
	const int32 Started = S.MatchesStarted;
	S = FSession();
	S.MatchesStarted = Started + 1;
	S.bActive = true;
	S.ProfileName = GP->ProfileName;
	S.LastProfile = GP->ProfileName;
	S.PlayerName = GP->PlayerName;
	S.Map = M.LevelName;
	S.GameType = M.GameType;
	S.bTeamGame = bTeam;
	// the player's team is blue in the stock URL (?Team=1)
	S.MyTeam = 1;
	TArray<FString> Names;
	TArray<int32> Teams;
	TSet<FString> Used;
	Used.Add(GP->PlayerCharacter.ToLower());
	auto AddBot = [&](const FString& Name, int32 Team)
	{
		if (Name.IsEmpty() || Used.Contains(Name.ToLower()))
		{
			return false;
		}
		Used.Add(Name.ToLower());
		Names.Add(Name);
		Teams.Add(Team);
		S.BotTeams.Add(Name, Team);
		return true;
	};
	auto AddRandom = [&](int32 Team, const TArray<FString>& Pool)
	{
		TArray<FString> P = Pool;
		while (P.Num() > 0)
		{
			const int32 I = FMath::RandRange(0, P.Num() - 1);
			if (AddBot(P[I], Team))
			{
				return;
			}
			P.RemoveAt(I);
		}
		// any character with BotUse (xDMRoster.GetRandomPlayer)
		TArray<FString> All;
		for (const FPlayerRecord& R : Get().Players)
		{
			if (R.BotUse > 0 && (R.Menu.IsEmpty() || R.Menu.Equals(TEXT("SP"), ESearchCase::IgnoreCase) || R.Menu.Equals(TEXT("DUP"), ESearchCase::IgnoreCase)))
			{
				All.Add(R.DefaultName);
			}
		}
		while (All.Num() > 0)
		{
			const int32 I = FMath::RandRange(0, All.Num() - 1);
			if (AddBot(All[I], Team))
			{
				return;
			}
			All.RemoveAt(I);
		}
	};
	const FTeamRoster* Enemy = FindTeam(GP->EnemyTeam);
	int32 NumBots = M.NumBots;
	if (bTeam)
	{
		const int32 Mates = GP->GetNumTeammatesForMatch(M);
		for (int32 i = 0; i < Mates && i < GP->PlayerLineup.Num(); ++i)
		{
			const int32 Slot = GP->PlayerLineup[i];
			if (GP->PlayerTeam.IsValidIndex(Slot))
			{
				AddBot(GP->PlayerTeam[Slot], S.MyTeam);
			}
		}
		const TArray<FString> Roster = GP->GetTeamRoster(GP->EnemyTeam);
		const int32 Enemies = NumBots - Mates;
		for (int32 i = 0, Added = 0; Added < Enemies; ++i)
		{
			if (Roster.IsValidIndex(i))
			{
				Added += AddBot(Roster[i], 1 - S.MyTeam) ? 1 : 0;
			}
			else
			{
				AddRandom(1 - S.MyTeam, TArray<FString>());
				++Added;
			}
		}
	}
	else if (bBoss && Enemy)
	{
		// BossDM: the enemy team's leader
		AddBot(!Enemy->TeamLeader.IsEmpty() ? Enemy->TeamLeader : (Enemy->RosterNames.Num() > 0 ? Enemy->RosterNames[0] : FString()), -1);
		NumBots = Names.Num();
	}
	else
	{
		TArray<FString> Pool;
		if (GP->EnemyTeam.EndsWith(TEXT("DMRosterBeatTeam")))
		{
			// "draft your team then defeat them": your own team mates
			for (const FString& N : GP->PlayerTeam)
			{
				if (!N.IsEmpty())
				{
					AddBot(N, -1);
				}
			}
		}
		else if (Enemy && !Enemy->bDMRoster)
		{
			// a team roster in a deathmatch (Head to Head): UT2K4TeamRoster.Initialize(1) takes the team leader
			if (NumBots == 1)
			{
				AddBot(!Enemy->TeamLeader.IsEmpty() ? Enemy->TeamLeader : (Enemy->RosterNames.Num() > 0 ? Enemy->RosterNames[0] : FString()), -1);
			}
			for (const FString& N : GP->GetTeamRoster(GP->EnemyTeam))
			{
				if (Names.Num() >= NumBots)
				{
					break;
				}
				AddBot(N, -1);
			}
		}
		else if (Enemy)
		{
			Pool = Enemy->BotList.Num() > 0 ? Enemy->BotList : Enemy->RosterNames;
		}
		while (Names.Num() < NumBots)
		{
			const int32 Before = Names.Num();
			AddRandom(-1, Pool);
			if (Names.Num() == Before)
			{
				break;
			}
		}
	}
	// character models of the named bots and the player (ArenaArt overrides by player name)
	ArenaArt::ClearCharacterOverrides();
	for (const FString& N : Names)
	{
		ArenaArt::SetCharacterOverride(N, MeshOf(N), FindPlayer(N) ? FindPlayer(N)->BodySkin : FString());
	}
	ArenaArt::SetCharacterOverride(GP->PlayerName, MeshOf(GP->PlayerCharacter), FindPlayer(GP->PlayerCharacter) ? FindPlayer(GP->PlayerCharacter)->BodySkin : FString());
	if (UArenaUserSettings* U = UArenaUserSettings::Get())
	{
		ArenaArt::SetCharacterOverride(U->PlayerName, MeshOf(GP->PlayerCharacter), FindPlayer(GP->PlayerCharacter) ? FindPlayer(GP->PlayerCharacter)->BodySkin : FString());
	}

	// MakeURLFoMatchInfo: the map, goal, bots, game, team, time limit; skill = BaseDifficulty + DifficultyModifier
	const int32 Skill = FMath::Clamp(FMath::RoundToInt(GP->Difficulty), 0, 7);
	FString Options = FString::Printf(TEXT("Mode=%s?Layout=%s?Bots=%d?Skill=%d?Team=%d?Name=%s?SinglePlayer=1"), *Mode, *M.LevelName, Names.Num(), Skill,
		S.MyTeam, *OptionSafe(GP->PlayerName));
	if (M.GoalScore > 0.f)
	{
		Options += FString::Printf(TEXT("?Goal=%d"), FMath::RoundToInt(M.GoalScore));
	}
	const FString RoundTime = UGameplayStatics::ParseOption(M.URLString, TEXT("RoundTimeLimit"));
	if (Mode == TEXT("AS") && !RoundTime.IsEmpty())
	{
		Options += TEXT("?Time=") + RoundTime;
	}
	else if (M.TimeLimit > 0.f)
	{
		Options += FString::Printf(TEXT("?Time=%d"), FMath::RoundToInt(M.TimeLimit));
	}
	TArray<FString> TeamStrs;
	for (int32 T : Teams)
	{
		TeamStrs.Add(FString::FromInt(T));
	}
	TArray<FString> SafeNames;
	for (const FString& N : Names)
	{
		SafeNames.Add(OptionSafe(N));
	}
	if (Names.Num() > 0)
	{
		Options += TEXT("?BotNames=") + FString::Join(SafeNames, TEXT(",")) + TEXT("?BotTeams=") + FString::Join(TeamStrs, TEXT(","));
	}
	if (bTeam)
	{
		// our symbol on our side, the enemy's on theirs
		// as the converted texture path ("Textures/<Pkg>/<Group>_<Name>") so single player symbols work too
		auto SymbolOption = [](const FString& Symbol)
		{
			TArray<FString> Parts;
			SymbolTexture(Symbol).ParseIntoArray(Parts, TEXT("."));
			return Parts.Num() == 3 ? FString::Printf(TEXT("Textures/%s/%s_%s"), *Parts[0], *Parts[1], *Parts[2]) : Symbol;
		};
		const FString Ours = GP->TeamSymbolName.IsEmpty() ? FString() : SymbolOption(GP->TeamSymbolName);
		const FString Theirs = Enemy && !Enemy->TeamSymbolName.IsEmpty() ? SymbolOption(Enemy->TeamSymbolName) : FString();
		if (!Ours.IsEmpty())
		{
			Options += (S.MyTeam == 1 ? TEXT("?BlueTeamSymbol=") : TEXT("?RedTeamSymbol=")) + Ours;
		}
		if (!Theirs.IsEmpty())
		{
			Options += (S.MyTeam == 1 ? TEXT("?RedTeamSymbol=") : TEXT("?BlueTeamSymbol=")) + Theirs;
		}
	}
	// the match info's own URL options (?FirstAttackingTeam=1?ReinforcementsFreq=7 ...)
	FString Extra = M.URLString;
	while (Extra.StartsWith(TEXT("?")))
	{
		Extra.RightChopInline(1);
	}
	if (!Extra.IsEmpty())
	{
		Options += TEXT("?") + Extra;
	}
	GP->Save();
	EnsureTicker();
	const FString Level = AArenaLevelBuilder::GetLevelForLayout(M.LevelName);
	UE_LOG(LogOpenUT2k4, Log, TEXT("SinglePlayer: starting match: %s?%s"), *Level, *Options);
	UGameplayStatics::OpenLevel(HUD, FName(*Level), true, Options);
}

bool FArenaSinglePlayerPage::CreateProfile()
{
	FString Name = NewName.TrimStartAndEnd();
	// UT2K4SPTab_ProfileNew: names of letters, digits and underscores
	FString Clean;
	for (TCHAR C : Name)
	{
		if (FChar::IsAlnum(C) || C == TEXT('_'))
		{
			Clean.AppendChar(C);
		}
	}
	if (Clean.IsEmpty())
	{
		return false;
	}
	if (FProfile::Exists(Clean))
	{
		PushQuestion(Caption(TEXT("UT2K4SPTab_ProfileNew"), TEXT("ErrorProfileExists"), TEXT("Profile with name '%profile%' already exists!")).Replace(TEXT("%profile%"), *Clean));
		return false;
	}
	const FData& D = Get();
	const FString Char = SPCharacters.IsValidIndex(NewCharIndex) ? SPCharacters[NewCharIndex] : D.Defaults.DefaultCharacter;
	const FString Symbol = D.TeamSymbols.IsValidIndex(NewSymbolIndex) ? D.TeamSymbols[NewSymbolIndex] : FString();
	const FString Team = NewTeamName.TrimStartAndEnd().IsEmpty() ? D.Defaults.DefaultTeamName : NewTeamName.TrimStartAndEnd();
	TSharedPtr<FProfile> P = FProfile::Create(Clean, Char, Team, Symbol, NewDifficulty);
	if (!P || !P->Save())
	{
		PushQuestion(Caption(TEXT("UT2K4SPTab_ProfileNew"), TEXT("ErrorCantCreateProfile"), TEXT("Profile creation failed.")));
		return false;
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("SinglePlayer: created profile %s (%s, team %s, difficulty %d, %d free agents)"), *Clean, *Char, *Team, NewDifficulty,
		P->BotStats.Num());
	ProfileNames = FProfile::List();
	bCreatingProfile = false;
	Tabs.Reset();
	LoadProfile(Clean);
	// the intro movie of UT2004 (MOV-UT2004-Intro) is a scripted cinematic map: not played; the welcome message follows
	ProcessProfileEvents();
	return true;
}

// =================================================================================================
// Frame
// =================================================================================================

bool FArenaSinglePlayerPage::Draw(FGUI& InG)
{
	G = &InG;
	if (Tabs.Num() == 0)
	{
		RebuildTabs();
	}
	TickScript();
	const FData& D = Get();
	FString PageCaption = Caption(TEXT("UT2K4SP_Main"), TEXT("PageCaption"), TEXT("Tournament"));
	if (GP)
	{
		PageCaption = GP->PlayerName + Caption(TEXT("UT2K4SP_Main"), TEXT("LoadedPageCaption"), TEXT("'s Tournament"));
	}
	if (Dialogs.Num() > 0)
	{
		G->PageBackground();
		G->Header(PageCaption);
		G->Footer(FString());
		DrawDialog();
		return false;
	}
	// tab captions: UT2K4SP_Main.PanelCaption / PanelHint
	TArray<FString> Captions, Hints;
	for (ETab T : Tabs)
	{
		const int32 I = int32(T);
		static const TCHAR* const Fallback[] = { TEXT("Profile"), TEXT("New profile"), TEXT("Tutorials"), TEXT("Qualification"), TEXT("Team Qualification"),
			TEXT("Ladder"), TEXT("Team Management"), TEXT("Additional") };
		Captions.Add(ArenaSP::LocArray(TEXT("GUI2K4"), TEXT("UT2K4SP_Main"), TEXT("PanelCaption"), I, Fallback[I]));
		Hints.Add(ArenaSP::LocArray(TEXT("GUI2K4"), TEXT("UT2K4SP_Main"), TEXT("PanelHint"), I, FString()));
	}
	int32 Active = TabIndex;
	const ArenaGUI2K4Pages::FTabbedFrame Frame = ArenaGUI2K4Pages::TabbedPage(*G, FGUI::Id(TEXT("SPTabs")), PageCaption, Captions, Hints, Active, TabChangedAt);
	if (Frame.bTabChanged || Active != TabIndex)
	{
		TabIndex = Active;
		ShownLadder = -1;
		G->ResetInteraction();
	}
	const FRect& P = Frame.Panel;
	if (!D.bValid)
	{
		G->TextIn(G->Font(TEXT("UT2HeaderFont")), FString::Printf(TEXT("Single player data not available: %s"), *D.Error), P, FLinearColor(1.f, 0.4f, 0.4f),
			EAlign::Center);
	}
	const ETab Tab = ActiveTab();
	switch (Tab)
	{
	case ETab::Profile: DrawProfileTab(P); break;
	case ETab::NewProfile: DrawNewProfileTab(P); break;
	case ETab::Tutorials: DrawTutorialsTab(P); break;
	case ETab::Qualification: DrawSingleLadderTab(P, LID_DM); break;
	case ETab::TeamQualification: DrawSingleLadderTab(P, LID_TDM); break;
	case ETab::Ladder: DrawLadderTab(P); break;
	case ETab::TeamManagement: DrawTeamTab(P); break;
	case ETab::Additional: DrawAdditionalTab(P); break;
	}
	if (Dialogs.Num() > 0)
	{
		return false;
	}

	// footer: DETAILS (with a played match), PLAY / LOAD / CREATE / DONE, BACK / CANCEL (UT2K4SP_Main buttons)
	const TCHAR* B = TEXT("UT2K4SPTab_Base");
	FString PlayCaption = Caption(B, TEXT("CaptionPlay"), TEXT("PLAY"));
	FString BackCaption = Caption(B, TEXT("CaptionBack"), TEXT("BACK"));
	bool bPlayEnabled = false;
	if (Tab == ETab::Profile)
	{
		PlayCaption = Caption(B, TEXT("CaptionLoad"), TEXT("LOAD"));
		bPlayEnabled = ProfileNames.IsValidIndex(ProfileSel);
	}
	else if (Tab == ETab::NewProfile)
	{
		PlayCaption = Caption(B, TEXT("CaptionCreate"), TEXT("CREATE"));
		BackCaption = Caption(B, TEXT("CaptionCancel"), TEXT("CANCEL"));
		bPlayEnabled = !NewName.TrimStartAndEnd().IsEmpty() && !NewTeamName.TrimStartAndEnd().IsEmpty();
	}
	else if (Tab == ETab::TeamManagement && bInitialDraft)
	{
		PlayCaption = Caption(B, TEXT("CaptionDone"), TEXT("DONE"));
		BackCaption = Caption(B, TEXT("CaptionCancel"), TEXT("CANCEL"));
		bPlayEnabled = GP && GP->HasFullTeam();
	}
	else if (GP && SelLadder >= 0 && Tab != ETab::Tutorials)
	{
		const FMatchInfo M = GP->GetMatchInfo(SelLadder, SelRung);
		const int32 MinFee = GP->GetMinimalEntryFeeFor(M);
		bPlayEnabled = SelRung <= GP->GetLadderProgress(SelLadder) && MinFee <= GP->Balance && MinFee >= 0;
	}
	TArray<FString> Btns, BtnHints;
	TArray<bool> Enabled;
	const bool bDetails = GP && !GP->LmdGameType.IsEmpty() && Tab != ETab::NewProfile;
	if (bDetails)
	{
		Btns.Add(Caption(TEXT("UT2K4SP_Main"), TEXT("SPbtnDetails.Caption"), TEXT("DETAILS")));
		BtnHints.Add(Caption(TEXT("UT2K4SP_Main"), TEXT("SPbtnDetails.Hint"), TEXT("Show profile details")));
		Enabled.Add(true);
	}
	Btns.Add(PlayCaption);
	BtnHints.Add(Caption(TEXT("UT2K4SP_Main"), TEXT("SPbtnPlay.Hint"), TEXT("Continue this tournament")));
	Enabled.Add(bPlayEnabled);
	Btns.Add(BackCaption);
	BtnHints.Add(Caption(TEXT("UT2K4SP_Main"), TEXT("SPbtnBack.Hint"), TEXT("Return to Previous Menu")));
	Enabled.Add(true);
	const int32 Clicked = ArenaGUI2K4Pages::FooterButtons(*G, Frame.Footer, Btns, BtnHints, Enabled);
	const int32 PlayIndex = bDetails ? 1 : 0;
	if (bDetails && Clicked == 0)
	{
		FDialog Dlg;
		Dlg.Kind = EDialog::Details;
		PushDialog(Dlg);
		return false;
	}
	if (Clicked == PlayIndex)
	{
		switch (Tab)
		{
		case ETab::Profile:
			if (ProfileNames.IsValidIndex(ProfileSel))
			{
				if (GP && !GP->ProfileName.Equals(ProfileNames[ProfileSel], ESearchCase::IgnoreCase))
				{
					PushQuestion(Caption(TEXT("UT2K4SPTab_Profile"), TEXT("OverloadProfile"), TEXT("You already have a profile loaded.||Are you sure you want to load this profile?")),
						EQuestion::LoadOverProfile, true);
				}
				else if (!GP)
				{
					OnQuestionAnswer(EQuestion::LoadOverProfile, true);
				}
			}
			break;
		case ETab::NewProfile:
			CreateProfile();
			break;
		case ETab::TeamManagement:
			if (bInitialDraft)
			{
				// DONE: back to the qualification and play the draft match
				bInitialDraft = false;
				NewTeam.Reset();
				SaveProfile();
				RebuildTabs();
				SetActiveTab(ETab::Qualification);
				PlaySelectedMatch();
			}
			else
			{
				PlaySelectedMatch();
			}
			break;
		default:
			PlaySelectedMatch();
			break;
		}
	}
	const int32 BackIndex = PlayIndex + 1;
	if (Clicked == BackIndex || (G->BackPressed() && Dialogs.Num() == 0))
	{
		if (Tab == ETab::NewProfile && ProfileNames.Num() > 0)
		{
			bCreatingProfile = false;
			RebuildTabs();
			return false;
		}
		if (Tab == ETab::TeamManagement && bInitialDraft && GP)
		{
			// cancel the draft: release everyone hired (refunded)
			for (int32 i = GP->MaxTeamSize - 1; i >= 0; --i)
			{
				if (GP->PlayerTeam.IsValidIndex(i) && !GP->PlayerTeam[i].IsEmpty())
				{
					const FString N = GP->PlayerTeam[i];
					if (GP->ReleaseTeammate(N) && NewTeam.ContainsByPredicate([&N](const FString& X) { return X.Equals(N, ESearchCase::IgnoreCase); }))
					{
						GP->Balance += GP->GetBotPrice(GP->FindBot(N));
					}
				}
			}
			NewTeam.Reset();
			bInitialDraft = false;
			SaveProfile();
			RebuildTabs();
			SetActiveTab(ETab::Qualification);
			return false;
		}
		SaveProfile();
		bCreatingProfile = false;
		Tabs.Reset();
		return true;
	}
	return false;
}

void FArenaSinglePlayerPage::OnQuestionAnswer(EQuestion Q, bool bYes)
{
	if (!bYes)
	{
		return;
	}
	switch (Q)
	{
	case EQuestion::DeleteProfile:
		if (ProfileNames.IsValidIndex(ProfileSel))
		{
			const FString Name = ProfileNames[ProfileSel];
			if (GP && GP->ProfileName.Equals(Name, ESearchCase::IgnoreCase))
			{
				GP.Reset();
				Session().LastProfile.Reset();
			}
			FProfile::Delete(Name);
			UE_LOG(LogOpenUT2k4, Log, TEXT("SinglePlayer: deleted profile %s"), *Name);
			ProfileNames = FProfile::List();
			ProfileSel = FMath::Clamp(ProfileSel, 0, FMath::Max(0, ProfileNames.Num() - 1));
			Tabs.Reset();
			RebuildTabs();
		}
		break;
	case EQuestion::LoadOverProfile:
		if (ProfileNames.IsValidIndex(ProfileSel))
		{
			SaveProfile();
			LoadProfile(ProfileNames[ProfileSel]);
			if (GP)
			{
				GP->bInLadderGame = true;
				GP->bIsChallenge = false;
				Tabs.Reset();
				ProcessProfileEvents();
			}
		}
		break;
	default:
		break;
	}
}

// =================================================================================================
// Test hooks
// =================================================================================================

void FArenaSinglePlayerPage::ApplyTestCommand(const FString& Name, const FString& Value)
{
#if !UE_BUILD_SHIPPING
	UE_LOG(LogOpenUT2k4, Log, TEXT("SinglePlayer: test command %s=%s"), *Name, *Value);
	if (Name == TEXT("SPCreate"))
	{
		// SPCreate=<name>[,<character>[,<difficulty>]]
		TArray<FString> P;
		Value.ParseIntoArray(P, TEXT(","));
		NewName = P.IsValidIndex(0) ? P[0] : FString(TEXT("Tester"));
		NewTeamName = Get().Defaults.DefaultTeamName.IsEmpty() ? FString(TEXT("Team")) : Get().Defaults.DefaultTeamName;
		if (P.IsValidIndex(1))
		{
			NewCharIndex = FMath::Max(0, SPCharacters.IndexOfByPredicate([&P](const FString& C) { return C.Equals(P[1], ESearchCase::IgnoreCase); }));
		}
		NewDifficulty = P.IsValidIndex(2) ? FCString::Atoi(*P[2]) : NewDifficulty;
		NewSymbolIndex = FMath::Max(0, Get().TeamSymbols.IndexOfByPredicate([](const FString& S) { return S.Equals(TEXT("TeamSymbols_UT2004.Design1"), ESearchCase::IgnoreCase); }));
		bCreatingProfile = true;
		CreateProfile();
	}
	else if (Name == TEXT("SPLoad"))
	{
		ProfileNames = FProfile::List();
		ProfileSel = FMath::Max(0, ProfileNames.IndexOfByPredicate([&Value](const FString& N) { return N.Equals(Value, ESearchCase::IgnoreCase); }));
		OnQuestionAnswer(EQuestion::LoadOverProfile, true);
	}
	else if (Name == TEXT("SPDelete"))
	{
		ProfileNames = FProfile::List();
		ProfileSel = ProfileNames.IndexOfByPredicate([&Value](const FString& N) { return N.Equals(Value, ESearchCase::IgnoreCase); });
		if (ProfileSel != INDEX_NONE)
		{
			OnQuestionAnswer(EQuestion::DeleteProfile, true);
		}
	}
	else if (Name == TEXT("SPTab"))
	{
		static const TCHAR* const Names[] = { TEXT("Profile"), TEXT("NewProfile"), TEXT("Tutorials"), TEXT("Qualification"), TEXT("TeamQualification"),
			TEXT("Ladder"), TEXT("TeamManagement"), TEXT("Additional") };
		for (int32 i = 0; i < UE_ARRAY_COUNT(Names); ++i)
		{
			if (Value.Equals(Names[i], ESearchCase::IgnoreCase))
			{
				if (ETab(i) == ETab::NewProfile)
				{
					bCreatingProfile = true;
					RebuildTabs();
				}
				SetActiveTab(ETab(i));
			}
		}
	}
	else if (Name == TEXT("SPCloseDialogs"))
	{
		Dialogs.Reset();
	}
	else if (Name == TEXT("SPCloseDialog"))
	{
		CloseDialog();
	}
	else if (Name == TEXT("SPSelect"))
	{
		// SPSelect=<ladder>,<rung>
		TArray<FString> P;
		Value.ParseIntoArray(P, TEXT(","));
		if (P.Num() == 2)
		{
			SelectMatch(FCString::Atoi(*P[0]), FCString::Atoi(*P[1]));
			ShownLadder = SelLadder;
		}
	}
	else if (Name == TEXT("SPPlay"))
	{
		Dialogs.Reset();
		PlaySelectedMatch();
	}
	else if (Name == TEXT("SPAutoFill") && GP)
	{
		// hire the cheapest free agents until the team is full (like AUTO FILL)
		TArray<int32> Free;
		for (int32 i = 0; i < GP->BotStats.Num(); ++i)
		{
			if (GP->BotStats[i].bFreeAgent && !GP->BotStats[i].Name.Equals(GP->PlayerCharacter, ESearchCase::IgnoreCase) && FindPlayer(GP->BotStats[i].Name))
			{
				Free.Add(i);
			}
		}
		Free.Sort([this](int32 A, int32 B) { return GP->BotStats[A].Price < GP->BotStats[B].Price; });
		for (int32 Id : Free)
		{
			if (GP->HasFullTeam())
			{
				break;
			}
			const FString N = GP->BotStats[Id].Name;
			if (GP->Balance - GP->MinBalance > GP->BotStats[Id].Price && GP->AddTeammate(N))
			{
				GP->Balance -= GP->BotStats[Id].Price;
				NewTeam.Add(N);
			}
		}
		SaveProfile();
	}
	else if (Name == TEXT("SPDone"))
	{
		if (bInitialDraft && GP && GP->HasFullTeam())
		{
			bInitialDraft = false;
			NewTeam.Reset();
			SaveProfile();
			RebuildTabs();
			SetActiveTab(ETab::Qualification);
		}
	}
	else if (Name == TEXT("SPOk"))
	{
		// confirms the top dialog (team roles OK, message OK)
		if (Dialogs.Num() > 0)
		{
			FDialog& D = Dialogs.Last();
			if (D.Kind == EDialog::TeamRoles)
			{
				D.Index = 1; // DrawTeamRoles reads it as "OK pressed"
			}
			else
			{
				CloseDialog();
			}
		}
	}
	else if (Name == TEXT("SPDialog"))
	{
		// open a dialog for screenshots: Message:<n>, Details, TeamRoles, Injury:<bot>, MapChallenge, HighScores, Challenge:<class>
		FString Kind = Value, Arg;
		Value.Split(TEXT(":"), &Kind, &Arg);
		FDialog D;
		if (Kind == TEXT("Message")) { D.Kind = EDialog::Message; D.Index = FCString::Atoi(*Arg); }
		else if (Kind == TEXT("Details")) { D.Kind = EDialog::Details; D.Tab = FCString::Atoi(*Arg); }
		else if (Kind == TEXT("TeamRoles")) { D.Kind = EDialog::TeamRoles; }
		else if (Kind == TEXT("Injury")) { D.Kind = EDialog::Injury; D.Index = GP ? GP->FindBot(Arg) : 0; }
		else if (Kind == TEXT("MapChallenge")) { D.Kind = EDialog::MapChallenge; }
		else if (Kind == TEXT("HighScores")) { D.Kind = EDialog::HighScores; }
		else if (Kind == TEXT("TeamDetails")) { D.Kind = EDialog::TeamDetails; D.Param1 = Arg; }
		else if (Kind == TEXT("Challenge")) { D.Kind = EDialog::Challenge; D.ChallengeClass = Arg; }
		else { return; }
		PushDialog(D);
	}
	else if (Name == TEXT("SPCheat") && GP)
	{
		// SPCheat=Ladder,<id>,<progress> | Balance,<n> | Completed
		TArray<FString> P;
		Value.ParseIntoArray(P, TEXT(","));
		if (P.Num() >= 3 && P[0] == TEXT("Ladder") && GP->LadderProgress.IsValidIndex(FCString::Atoi(*P[1])))
		{
			GP->LadderProgress[FCString::Atoi(*P[1])] = FCString::Atoi(*P[2]);
		}
		else if (P.Num() >= 2 && P[0] == TEXT("Balance"))
		{
			GP->Balance = FCString::Atoi(*P[1]);
		}
		else if (P.Num() >= 3 && P[0] == TEXT("Health") && GP->FindBot(P[1]) != INDEX_NONE)
		{
			GP->BotStats[GP->FindBot(P[1])].Health = FMath::Clamp(FCString::Atoi(*P[2]), 0, 100);
		}
		else if (P.Num() >= 2 && P[0] == TEXT("Final"))
		{
			GP->FinalEnemyTeam = P[1];
		}
		else if (P.Num() >= 1 && P[0] == TEXT("Completed"))
		{
			GP->bCompleted = true;
		}
		SaveProfile();
		Tabs.Reset();
		RebuildTabs();
	}
#endif
}
