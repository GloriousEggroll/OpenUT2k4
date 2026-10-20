#include "ArenaMidGameMenu.h"
#include "ArenaChat.h"
#include "OpenUT2k4.h"
#include "ArenaGameState.h"
#include "ArenaGUI2K4.h"
#include "ArenaGUI2K4Pages.h"
#include "ArenaPlayerController.h"
#include "ArenaPlayerState.h"
#include "Net/UE2Server.h"
#include "Net/UE2NetJoin.h"
#include "Net/UE2NetMapVote.h"

#include "Containers/Ticker.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "Misc/CommandLine.h"

using namespace ArenaGUI2K4;
using namespace ArenaGUI2K4Pages;

namespace ArenaMidGameMenuLocal
{
	struct FState
	{
		FGUI G;
		FSettingsPage Settings;
		FAutomation Automation;
		int32 Tab = 0;
		double TabChangedAt = 0.0;
		/** Admin tab (hosting for UT2004 clients): selected player / ban rows. */
		int32 AdminSel = -1;
		int32 BanSel = -1;
		/** Map voting tab (a UT2004 server's xVoting): the game type shown, the selected row, open it with the menu. */
		int32 VoteGame = -1;
		int32 VoteSel = -1;
		bool bShowVoteTab = false;
		/** The Map Voting tab's index when the menu was last drawn (INDEX_NONE: none). */
		int32 VoteTabIndex = INDEX_NONE;
		bool bWasOpen = false;
		bool bWasSettings = false;
	};
	FState& State()
	{
		static FState S;
		return S;
	}

	AArenaPlayerController* FindLocalArenaPC()
	{
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* W = Ctx.World();
			if (W && (Ctx.WorldType == EWorldType::Game || Ctx.WorldType == EWorldType::PIE))
			{
				return Cast<AArenaPlayerController>(W->GetFirstPlayerController());
			}
		}
		return nullptr;
	}

	/** -AutoMenuScript in a match: Pause (opens the menu), Settings (its Settings button), Tab=<n>, Resume; keys, clicks
	 *  and shots go through the GUI (ArenaGUI2K4Pages::FAutomation). Runs every frame, also while the menu is closed. */
	struct FTestTicker
	{
		FTestTicker()
		{
#if !UE_BUILD_SHIPPING
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
			{
				static const bool bScript = FString(FCommandLine::Get()).Contains(TEXT("AutoMenuScript="));
				AArenaPlayerController* PC = bScript ? FindLocalArenaPC() : nullptr;
				if (!PC || !PC->GetWorld())
				{
					return true;
				}
				FState& S = State();
				if (!S.Automation.Claim())
				{
					return true;
				}
				for (const TPair<FString, FString>& Cmd : S.Automation.Tick(S.G, PC->GetWorld()->GetRealTimeSeconds(), PC->GetWorld()))
				{
					if (Cmd.Key == TEXT("Pause"))
					{
						PC->SetPauseMenuOpen(true);
					}
					else if (Cmd.Key == TEXT("Resume"))
					{
						PC->SetPauseMenuOpen(false);
					}
					else if (Cmd.Key == TEXT("Settings"))
					{
						PC->SetPauseMenuOpen(true);
						PC->ClickPauseItem(1);
					}
					else if (Cmd.Key == TEXT("AdminKick") || Cmd.Key == TEXT("AdminBan"))
					{
						// hosting test hook: kick / ban a connected player by name (the admin tab's buttons)
						for (const ArenaHost::FUE2HostPlayer& HP : ArenaHost::GetUE2Players())
						{
							if (HP.Name.Equals(Cmd.Value.Replace(TEXT("_"), TEXT(" ")), ESearchCase::IgnoreCase))
							{
								ArenaHost::KickUE2Player(HP.ConnId, Cmd.Key == TEXT("AdminBan"));
							}
						}
					}
					else if (Cmd.Key == TEXT("Talk") || Cmd.Key == TEXT("TeamTalk"))
					{
						// chat test hooks (ArenaChat.h): open the typing line, type ("_" = space), press a key
						ArenaChat::OpenTyping(PC, Cmd.Key == TEXT("TeamTalk"));
					}
					else if (Cmd.Key == TEXT("ChatType"))
					{
						ArenaChat::HandleKey(EKeys::End);
						for (const TCHAR Ch : Cmd.Value.Replace(TEXT("_"), TEXT(" ")))
						{
							ArenaChat::HandleChar(Ch);
						}
					}
					else if (Cmd.Key == TEXT("ChatKey"))
					{
						ArenaChat::HandleKey(FKey(*Cmd.Value));
					}
					else if (Cmd.Key == TEXT("Tab"))
					{
						S.Tab = FCString::Atoi(*Cmd.Value);
						S.Settings.Tab = S.Tab;
						S.G.ResetInteraction();
					}
				}
				return true;
			}));
#endif
		}
	};
	static FTestTicker GTestTicker;

	/** UT2K4Tab_PlayerLoginControls player list rows: name and score, best first. */
	TArray<FListItem> PlayerRows(const AArenaGameState* GS, int32 Team)
	{
		TArray<FListItem> Rows;
		if (!GS)
		{
			return Rows;
		}
		for (AArenaPlayerState* P : GS->GetSortedPlayers())
		{
			if (!P || (Team >= 0 && P->Team != Team))
			{
				continue;
			}
			Rows.Add({ P->GetPlayerName(), FString::FromInt(FMath::RoundToInt(P->GetScore())) });
		}
		return Rows;
	}
}

namespace ArenaMidGameMenuLocal
{
	/**
	 * The Map Voting tab (xVoting's MapVotingPage): the game types the server offers, the maps of the chosen one with
	 * their votes and play counts, and the vote button (double click / Enter votes too). The server tallies the votes
	 * and counts down (VoteTimeLimit); without votes it picks a map itself when the time is up.
	 */
	void DrawMapVoteTab(FGUI& G, const FRect& Panel, const FUE2NetMapVote& MV)
	{
		FState& S = State();
		const TArray<FUE2NetMapVote::FGameConfig>& Games = MV.GetGameConfigs();
		const TArray<FUE2NetMapVote::FMap>& Maps = MV.GetMaps();
		if (!Games.IsValidIndex(S.VoteGame))
		{
			S.VoteGame = Games.IsValidIndex(MV.GetCurrentGameConfig()) ? MV.GetCurrentGameConfig() : 0;
		}

		// status line: the count down, our vote, or the lists still arriving (one item per server tick)
		FString Status;
		if (Maps.Num() < MV.GetMapCount() || Games.Num() == 0)
		{
			Status = FString::Printf(TEXT("Receiving the map list (%d of %d maps)..."), Maps.Num(), MV.GetMapCount());
		}
		else if (Maps.IsValidIndex(MV.GetMyMap()) && Games.IsValidIndex(MV.GetMyGame()))
		{
			Status = FString::Printf(TEXT("You voted for %s (%s)"), *Maps[MV.GetMyMap()].Name, *Games[MV.GetMyGame()].GameName);
		}
		else
		{
			Status = TEXT("Choose the next map: double click a map or select it and press Vote");
		}
		if (MV.GetSecondsLeft() >= 0)
		{
			Status += FString::Printf(TEXT("  -  voting ends in %d s"), MV.GetSecondsLeft());
		}
		G.Label(Panel.Sub(0.037154f, 0.01f, 0.919753f, 0.05f), Status, TEXT("TextLabel"), EAlign::Center, EFontScale::Small);

		FRect MapsArea = Panel.Sub(0.037154f, 0.07f, 0.919753f, 0.66f);
		if (Games.Num() > 1)
		{
			// MapVotingPage co_GameType: one row per game type
			const FRect GamesR = G.Section(Panel.Sub(0.037154f, 0.07f, 0.28f, 0.66f), TEXT("Game Type"), true);
			TArray<FListItem> Rows;
			for (const FUE2NetMapVote::FGameConfig& Game : Games)
			{
				Rows.Add({ Game.GameName.IsEmpty() ? Game.GameClass : Game.GameName, FString() });
			}
			bool bAct = false;
			if (G.List(FGUI::Id(TEXT("VoteGames")), GamesR, Rows, S.VoteGame, bAct, TEXT("The game type of the next match"), TEXT("UT2SmallHeaderFont")))
			{
				S.VoteSel = -1;
			}
			MapsArea = Panel.Sub(0.33f, 0.07f, 0.626907f, 0.66f);
		}

		// the maps of the game type (its map name prefix), with the votes for them in that game type
		const FString Prefix = Games.IsValidIndex(S.VoteGame) ? Games[S.VoteGame].Prefix : FString();
		// by name, like MapVotingPage's list
		TArray<int32> Order;
		for (int32 i = 0; i < Maps.Num(); ++i)
		{
			Order.Add(i);
		}
		Order.Sort([&Maps](int32 A, int32 B) { return Maps[A].Name < Maps[B].Name; });
		TArray<int32> RowMap;
		TArray<FListItem> Rows;
		for (const int32 i : Order)
		{
			const FUE2NetMapVote::FMap& M = Maps[i];
			if (!M.bEnabled || (!Prefix.IsEmpty() && !M.Name.StartsWith(Prefix, ESearchCase::IgnoreCase)))
			{
				continue;
			}
			int32 Votes = 0;
			for (const FUE2NetMapVote::FVote& V : MV.GetVotes())
			{
				if (V.MapIndex == i && V.GameConfigIndex == S.VoteGame)
				{
					Votes += V.VoteCount;
				}
			}
			FListItem Row;
			Row.Cells = { M.Name, Votes > 0 ? FString::FromInt(Votes) : FString(), FString::FromInt(M.PlayCount) };
			Rows.Add(Row);
			RowMap.Add(i);
		}
		const FRect MapsR = G.Section(MapsArea, TEXT("Maps  (votes, times played)"), true);
		static const TArray<FListColumn> Columns = { { 0.70f, EAlign::Left }, { 0.15f, EAlign::Center }, { 0.15f, EAlign::Center } };
		bool bVote = false;
		G.List(FGUI::Id(TEXT("VoteMaps")), MapsR, Rows, S.VoteSel, bVote, TEXT("Double click a map to vote for it"), TEXT("UT2SmallHeaderFont"), false, &Columns);
		const bool bHasSel = RowMap.IsValidIndex(S.VoteSel);
		if (G.Button(FGUI::Id(TEXT("VoteSubmit")), Panel.Sub(0.375f, 0.765f, 0.25f, 0.06f), TEXT("SquareMenuButton"), TEXT("Vote"),
			TEXT("Vote for the selected map"), bHasSel))
		{
			bVote = true;
		}
		if (bVote && bHasSel)
		{
			ArenaNet::SendMapVote(RowMap[S.VoteSel], S.VoteGame);
		}
	}
}

using namespace ArenaMidGameMenuLocal;

void ArenaMidGameMenu::DrawPauseMenu(AHUD* HUD, UCanvas* Canvas, AArenaPlayerController* PC, bool bForceSettings)
{
	if (!HUD || !Canvas)
	{
		return;
	}
	FState& S = State();
	FGUI& G = S.G;
	const bool bSettings = bForceSettings || (PC && PC->IsPauseSettingsOpen());
	const bool bOpen = PC && PC->IsPauseMenuOpen();
	G.Begin(Canvas, PC, bOpen);
	if (bOpen && !S.bWasOpen)
	{
		// a new menu: focus the first control again
		G.ResetInteraction();
		G.SetFocus(FGUI::Id(TEXT("MidBtn"), 0)); // Resume
		S.Tab = 0;
	}
	if (bSettings != S.bWasSettings)
	{
		G.ResetInteraction();
		G.SetFocus(0);
	}
	S.bWasOpen = bOpen;
	S.bWasSettings = bSettings;

	if (bSettings)
	{
		if (S.Settings.Draw(G) && PC && PC->IsPauseSettingsOpen())
		{
			// the controller's settings sub page: its last row is Back
			PC->ClickPauseSetting(AArenaPlayerController::GetPauseSettingsRows().Num() - 1, +1);
		}
		G.End();
		return;
	}

	const AArenaGameState* GS = HUD->GetWorld() ? HUD->GetWorld()->GetGameState<AArenaGameState>() : nullptr;
	const FRect P = G.Page();
	const float H = G.ScreenH();
	// UT2K4PlayerLoginMenu (FloatingWindow): WinLeft 0.110313, WinTop 0.057916, 0.779688 x 0.847083;
	// FloatingFrameBackground NewControls.Display1 (stretched) at WinTop 0.02, WinHeight 0.98 of the window
	const FRect Win(P.X + 0.110313f * P.W, 0.057916f * H, 0.779688f * P.W, 0.847083f * H);
	const FRect Frame = Win.Sub(0.f, 0.02f, 1.f, 0.98f);
	if (!G.Image(TEXT("2K4Menus.NewControls.Display1"), Frame, EImgStyle::Stretched, FLinearColor::White))
	{
		G.FallbackBox(Frame, EMenuState::Blurry, false);
	}
	// TitleBar (GUIHeader, WinHeight 0.04375 of the window) with the close box (STY2CloseButton) at its right end
	const FRect Title = Win.Sub(0.f, 0.f, 1.f, 0.04375f);
	const FStyle& HS = GetStyle(TEXT("Header"));
	if (!G.StyleImage(HS, EMenuState::Blurry, Title))
	{
		G.FallbackBox(Title, EMenuState::Focused, true);
	}
	FString Caption = GS ? GS->GetGameTypeName() : FString();
	if (GS && !GS->LayoutName.IsEmpty())
	{
		Caption = GS->LayoutName + TEXT("  -  ") + Caption;
	}
	G.StyleText(HS, EMenuState::Blurry, EFontScale::Small, Caption, Title, EAlign::Center);
	const float CloseSize = Title.H * 0.9f;
	if (G.Button(FGUI::Id(TEXT("MidClose")), FRect(Title.R() - CloseSize - G.L(4.f), Title.Y + (Title.H - CloseSize) * 0.5f, CloseSize, CloseSize),
		TEXT("CloseButton"), FString(), TEXT("Back to the match"), true, EFontScale::Small, ESound::Click, false))
	{
		if (PC) PC->SetPauseMenuOpen(false);
		G.End();
		return;
	}

	// LoginMenuTC: GUITabControl at (0.0125, 0.060215), 0.974999 x 0.044644 of the window, bFillSpace
	const FString GameTab = Loc(TEXT("GUI2K4"), TEXT("UT2K4PlayerLoginMenu"), TEXT("Panels"), FString());
	TArray<FString> Tabs = { LocArray(TEXT("GUI2K4"), TEXT("UT2K4PlayerLoginMenu"), TEXT("Panels"), 0, TEXT("Game")),
		Loc(TEXT("GUI2K4"), TEXT("UT2K4PlayerLoginMenu"), TEXT("SPRulesPanel.Caption"), TEXT("Rules")) };
	// the Panels value is an array of structs: take the Caption of the first one
	{
		FString Raw = Tabs[0];
		int32 At = Raw.Find(TEXT("Caption=\""));
		if (At != INDEX_NONE)
		{
			Raw = Raw.Mid(At + 9);
			int32 End = INDEX_NONE;
			Raw.FindChar(TEXT('"'), End);
			Tabs[0] = End > 0 ? Raw.Left(End) : FString(TEXT("Game"));
		}
	}
	(void)GameTab;
	TArray<FString> Hints = { TEXT("Match controls"), TEXT("The rules of this match") };
	// hosting (Net/UE2Server.h): UT2004's in-game admin, a player list with kick / ban
	int32 AdminTab = INDEX_NONE;
	if (ArenaHost::IsUE2Hosting())
	{
		AdminTab = Tabs.Add(TEXT("Admin"));
		Hints.Add(TEXT("Players on your server: kick or ban"));
	}
	// a UT2004 server's map voting (Net/UE2NetMapVote.h): UT2K4Tab_PlayerLoginControls' Map Voting button opens MapVotingPage
	const FUE2NetMapVote* MapVote = ArenaNet::GetMapVote();
	const FString MapVotingCaption = Loc(TEXT("GUI2K4"), TEXT("UT2K4Tab_PlayerLoginControls"), TEXT("MapVotingButton.Caption"), TEXT("Map Voting"));
	int32 VoteTab = INDEX_NONE;
	if (MapVote)
	{
		VoteTab = Tabs.Add(MapVotingCaption);
		Hints.Add(TEXT("Vote for the next map"));
	}
	S.VoteTabIndex = VoteTab;
	if (S.bShowVoteTab)
	{
		S.bShowVoteTab = false;
		if (VoteTab != INDEX_NONE)
		{
			S.Tab = VoteTab;
			S.VoteSel = -1;
		}
	}
	if (S.Tab >= Tabs.Num())
	{
		S.Tab = 0;
	}
	const FRect TabsR = Win.Sub(0.0125f, 0.060215f, 0.974999f, 0.044644f);
	if (G.Tabs(FGUI::Id(TEXT("MidTabs")), TabsR, Tabs, Hints, S.Tab))
	{
		S.TabChangedAt = G.Time();
	}
	const FRect Panel(Win.X + Win.W * 0.0125f, TabsR.B() + G.L(4.f), Win.W * 0.975f, Win.B() - TabsR.B() - G.L(16.f));

	if (S.Tab == 0)
	{
		const TCHAR* Sec = TEXT("UT2K4Tab_PlayerLoginControls");
		if (GS && GS->IsTeamGame())
		{
			// SBRed (0.0166, 0.083066, 0.470135 x 0.644078), SBBlue (0.512464, 0.083066, 0.46885 x 0.644078), AltSectionBackground
			const FRect Red = G.Section(Panel.Sub(0.0166f, 0.083066f, 0.470135f, 0.644078f), Loc(TEXT("GUI2K4"), Sec, TEXT("SBRed.Caption"), TEXT("Red")), true);
			const FRect Blue = G.Section(Panel.Sub(0.512464f, 0.083066f, 0.46885f, 0.644078f), Loc(TEXT("GUI2K4"), Sec, TEXT("SBBlue.Caption"), TEXT("Blue")), true);
			// team colour tint over the sections (JoinRedButton / JoinBlueButton images: (255,100,100,90) / (0,128,255,90))
			G.Draw().Rect(Red.X, Red.Y, Red.W, Red.H, FArenaDraw::UTColor(FColor(255, 100, 100, 40)));
			G.Draw().Rect(Blue.X, Blue.Y, Blue.W, Blue.H, FArenaDraw::UTColor(FColor(0, 128, 255, 40)));
			int32 SelR = -1, SelB = -1;
			bool bAct = false;
			G.List(FGUI::Id(TEXT("MidRed")), Red, PlayerRows(GS, 0), SelR, bAct, FString(), TEXT("UT2SmallHeaderFont"));
			G.List(FGUI::Id(TEXT("MidBlue")), Blue, PlayerRows(GS, 1), SelB, bAct, FString(), TEXT("UT2SmallHeaderFont"));
		}
		else
		{
			// SBFFA "Players" (0.037154, 0.024639, 0.919753 x 0.701886)
			const FRect All = G.Section(Panel.Sub(0.037154f, 0.024639f, 0.919753f, 0.701886f), Loc(TEXT("GUI2K4"), Sec, TEXT("SBFFA.Caption"), TEXT("Players")), true);
			int32 Sel = -1;
			bool bAct = false;
			const FRect ListR = All.Inset(All.W * 0.2f, 0.f, All.W * 0.2f, 0.f);
			G.List(FGUI::Id(TEXT("MidAll")), ListR, PlayerRows(GS, -1), Sel, bAct, FString(), TEXT("UT2SmallHeaderFont"));
		}
		// buttons on the UT2K4Tab_PlayerLoginControls grid (X 0.025 / 0.375 / 0.725, Y 0.675 / 0.75, 0.2 x 0.05)
		struct FBtn { int32 Item; const TCHAR* LocKey; const TCHAR* Fallback; const TCHAR* Hint; float X, Y; };
		// on a UT2004 server there is no match of ours to restart: the middle button is Map Voting when the server runs it
		const bool bOnServer = HUD->GetWorld() && HUD->GetWorld()->URL.HasOption(TEXT("UE2Net"));
		if (bOnServer && VoteTab != INDEX_NONE
			&& G.Button(FGUI::Id(TEXT("MidVoteBtn")), Panel.Sub(0.375f, 0.765f, 0.25f, 0.06f), TEXT("SquareMenuButton"), MapVotingCaption, TEXT("Vote for the next map")))
		{
			S.Tab = VoteTab;
			S.VoteSel = -1;
		}
		const FBtn Btns[] = {
			{ 0, nullptr, TEXT("Resume"), TEXT("Back to the match"), 0.025f, 0.765f },
			{ 2, nullptr, TEXT("Restart Match"), TEXT("Start this match again from the beginning"), 0.375f, 0.765f },
			{ 3, TEXT("LeaveMPButtonText"), TEXT("Leave Match"), TEXT("Leave the match and return to the main menu"), 0.725f, 0.765f },
			{ 1, TEXT("SettingsButton.Caption"), TEXT("Settings"), TEXT("Change your settings"), 0.025f, 0.855f },
			{ 4, TEXT("QuitGameButton.Caption"), TEXT("Exit Game"), TEXT("Close the game"), 0.725f, 0.855f },
		};
		for (const FBtn& B : Btns)
		{
			if (bOnServer && B.Item == 2)
			{
				continue;
			}
			const FString Cap = B.LocKey ? Loc(TEXT("GUI2K4"), Sec, B.LocKey, B.Fallback) : FString(B.Fallback);
			if (G.Button(FGUI::Id(TEXT("MidBtn"), B.Item), Panel.Sub(B.X, B.Y, 0.25f, 0.06f), TEXT("SquareMenuButton"), Cap, B.Hint) && PC)
			{
				PC->ClickPauseItem(B.Item);
			}
		}
	}
	else if (S.Tab == VoteTab && MapVote)
	{
		DrawMapVoteTab(G, Panel, *MapVote);
	}
	else if (S.Tab == AdminTab)
	{
		// the host's admin page (UT2K4Tab_AdminPlayerList): remote players with kick / ban, and the ban list
		const TArray<ArenaHost::FUE2HostPlayer> Players = ArenaHost::GetUE2Players();
		const FRect PlayersR = G.Section(Panel.Sub(0.037154f, 0.024639f, 0.919753f, 0.46f), TEXT("Players on your server"), true);
		TArray<FListItem> Rows;
		for (const ArenaHost::FUE2HostPlayer& HP : Players)
		{
			Rows.Add({ HP.Name + (HP.bSpectator ? TEXT(" (spectator)") : TEXT("")), FString::Printf(TEXT("%d ms   id %s"), HP.PingMs, *HP.IdShort) });
		}
		bool bAct = false;
		G.List(FGUI::Id(TEXT("AdminPlayers")), PlayersR.Inset(PlayersR.W * 0.05f, G.L(8.f), PlayersR.W * 0.05f, 0.f), Rows, S.AdminSel, bAct, FString(), TEXT("UT2SmallHeaderFont"));
		const bool bPlayer = Players.IsValidIndex(S.AdminSel);
		if (G.Button(FGUI::Id(TEXT("AdminKick")), Panel.Sub(0.2f, 0.5f, 0.25f, 0.06f), TEXT("SquareMenuButton"), TEXT("Kick"), TEXT("Remove the selected player from the match")) && bPlayer)
		{
			ArenaHost::KickUE2Player(Players[S.AdminSel].ConnId, false);
			S.AdminSel = -1;
		}
		if (G.Button(FGUI::Id(TEXT("AdminBan")), Panel.Sub(0.55f, 0.5f, 0.25f, 0.06f), TEXT("SquareMenuButton"), TEXT("Ban"), TEXT("Remove the selected player and keep them out (by player id)")) && bPlayer)
		{
			ArenaHost::KickUE2Player(Players[S.AdminSel].ConnId, true);
			S.AdminSel = -1;
		}
		const TArray<FString> Bans = ArenaHost::GetUE2BanList();
		const FRect BansR = G.Section(Panel.Sub(0.037154f, 0.58f, 0.919753f, 0.28f), TEXT("Banned players"), true);
		TArray<FListItem> BanRows;
		for (const FString& B : Bans)
		{
			BanRows.Add({ B, FString() });
		}
		bool bBanAct = false;
		G.List(FGUI::Id(TEXT("AdminBans")), BansR.Inset(BansR.W * 0.05f, G.L(8.f), BansR.W * 0.05f, 0.f), BanRows, S.BanSel, bBanAct, FString(), TEXT("UT2SmallHeaderFont"));
		if (G.Button(FGUI::Id(TEXT("AdminUnban")), Panel.Sub(0.375f, 0.88f, 0.25f, 0.06f), TEXT("SquareMenuButton"), TEXT("Unban"), TEXT("Lift the selected ban")) && Bans.IsValidIndex(S.BanSel))
		{
			ArenaHost::UnbanUE2(S.BanSel);
			S.BanSel = -1;
		}
	}
	else
	{
		// rules of the match (UT2K4Tab_ServerInfo "Rules")
		const FRect C = G.Section(Panel.Sub(0.037154f, 0.024639f, 0.919753f, 0.9f), Loc(TEXT("GUI2K4"), TEXT("UT2K4Tab_ServerInfo"), TEXT("ServerInfoLabel.Caption"), TEXT("Rules")), true);
		TArray<FListItem> Rows;
		if (GS)
		{
			Rows.Add({ TEXT("Game Type"), GS->GetGameTypeName() });
			Rows.Add({ TEXT("Map"), GS->LayoutName });
			Rows.Add({ TEXT("Goal"), GS->GoalScore > 0 ? FString::FromInt(GS->GoalScore) : FString(TEXT("None")) });
			Rows.Add({ TEXT("Time Limit"), GS->TimeLimit > 0 ? FString::Printf(TEXT("%d min"), FMath::Max(1, GS->TimeLimit / 60)) : FString(TEXT("None")) });
			Rows.Add({ TEXT("Instagib"), GS->bInstagib ? TEXT("On") : TEXT("Off") });
			Rows.Add({ TEXT("Translocator"), GS->bAllowTrans ? TEXT("On") : TEXT("Off") });
			Rows.Add({ TEXT("Players"), FString::FromInt(GS->PlayerArray.Num()) });
		}
		int32 Sel = -1;
		bool bAct = false;
		G.List(FGUI::Id(TEXT("MidRules")), C.Inset(C.W * 0.1f, G.L(8.f), C.W * 0.1f, 0.f), Rows, Sel, bAct, FString(), TEXT("UT2SmallHeaderFont"));
	}
	if (G.BackPressed() && PC)
	{
		PC->SetPauseMenuOpen(false);
	}
	G.End();
}

void ArenaMidGameMenu::DrawEndOfMatch(AHUD* HUD, UCanvas* Canvas, const AArenaGameState* GS)
{
	if (!HUD || !Canvas || !GS)
	{
		return;
	}
	// UT2004 ends a match with the winner announced in the HUD's large message font (gold, or the winning team's
	// colour) over the scoreboard; no window. Drawn with a separate draw-only GUI pass.
	static FGUI G;
	G.Begin(Canvas, HUD->GetOwningPlayerController(), false);
	const float W = G.ScreenW(), H = G.ScreenH();
	FColor Color(255, 255, 0);
	if (GS->IsTeamGame() && GS->WinningTeam == 0) Color = FColor(255, 64, 64);
	if (GS->IsTeamGame() && GS->WinningTeam == 1) Color = FColor(64, 160, 255);
	const FString Winner = GS->WinnerText.IsEmpty() ? FString(TEXT("Match over")) : GS->WinnerText;
	const FFontRef Big = G.Font(TEXT("HudLargeFont"));
	// under the scoreboard rows, just above the HUD's own "you won / lost" line at the bottom
	G.Text(Big, Winner, W * 0.5f, H * 0.87f - Big.Height, FArenaDraw::UTColor(Color), EAlign::Center, true);
	G.End();
}

void ArenaMidGameMenu::OpenMapVote(AArenaPlayerController* PC)
{
	if (PC)
	{
		State().bShowVoteTab = true;
		PC->SetPauseMenuOpen(true);
	}
}

void ArenaMidGameMenu::CloseMapVote(AArenaPlayerController* PC)
{
	// the server chose the map (CloseWindow): close the menu if it shows the vote
	FState& S = State();
	S.bShowVoteTab = false;
	if (PC && PC->IsPauseMenuOpen() && !PC->IsPauseSettingsOpen() && S.VoteTabIndex != INDEX_NONE && S.Tab == S.VoteTabIndex)
	{
		PC->SetPauseMenuOpen(false);
	}
}
