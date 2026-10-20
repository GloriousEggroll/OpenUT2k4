#include "ArenaServerBrowserPage.h"
#include "../OpenUT2k4.h"
#include "../ArenaGUI2K4Pages.h"
#include "../ArenaUserSettings.h"
#include "../ArenaGameSession.h"

#if __has_include("UE2NetJoin.h")
#include "UE2NetJoin.h"
#define ARENA_HAS_UE2_JOIN 1
#else
#define ARENA_HAS_UE2_JOIN 0
#endif


using namespace ArenaGUI2K4;
using namespace ArenaGUI2K4Pages;
using namespace ArenaServerBrowser;

namespace ArenaServerBrowserPageLocal
{
	FString LocBrowser(const TCHAR* Section, const TCHAR* Key, const TCHAR* Fallback)
	{
		return Loc(TEXT("GUI2K4"), Section, Key, Fallback);
	}

	/** UT2k4Browser_RulesList: friendly names of the common rules (the server sends the raw keys). */
	FString RuleName(const FString& Key)
	{
		struct FMap { const TCHAR* Key; const TCHAR* LocKey; const TCHAR* Fallback; };
		static const FMap Map[] = {
			{ TEXT("ServerMode"), TEXT("ServerModeString"), TEXT("Server Mode") },
			{ TEXT("AdminName"), TEXT("AdminNameString"), TEXT("Server Admin") },
			{ TEXT("AdminEmail"), TEXT("AdminEmailString"), TEXT("Admin Email") },
			{ TEXT("GamePassword"), TEXT("PasswordString"), TEXT("Requires Password") },
			{ TEXT("GameStats"), TEXT("GameStatsString"), TEXT("Stats") },
			{ TEXT("GameSpeed"), TEXT("GameSpeedString"), TEXT("Game Speed") },
			{ TEXT("Mutator"), TEXT("MutatorString"), TEXT("Mutator") },
			{ TEXT("BalanceTeams"), TEXT("BalanceTeamsString"), TEXT("Bots Balance Teams") },
			{ TEXT("PlayersBalanceTeams"), TEXT("PlayersBalanceTeamsString"), TEXT("Balance Teams") },
			{ TEXT("FriendlyFire"), TEXT("FriendlyFireString"), TEXT("Friendly Fire") },
			{ TEXT("GoalScore"), TEXT("GoalScoreString"), TEXT("Goal Score") },
			{ TEXT("TimeLimit"), TEXT("TimeLimitString"), TEXT("Time Limit") },
			{ TEXT("MinPlayers"), TEXT("MinPlayersString"), TEXT("Minimum Players (bots)") },
			{ TEXT("Translocator"), TEXT("TranslocatorString"), TEXT("Translocator") },
			{ TEXT("WeaponStay"), TEXT("WeaponStayString"), TEXT("Weapons Stay") },
			{ TEXT("MapVoting"), TEXT("MapVotingString"), TEXT("Map Voting") },
			{ TEXT("KickVoting"), TEXT("KickVotingString"), TEXT("Kick Voting") },
		};
		for (const FMap& M : Map)
		{
			if (Key.Equals(M.Key, ESearchCase::IgnoreCase))
			{
				return LocBrowser(TEXT("UT2k4Browser_RulesList"), M.LocKey, M.Fallback);
			}
		}
		return Key;
	}

	FString RuleValue(const FString& Key, const FString& Value)
	{
		if (Value.Equals(TEXT("True"), ESearchCase::IgnoreCase))
		{
			return LocBrowser(TEXT("UT2k4Browser_RulesList"), TEXT("TrueString"), TEXT("Enabled"));
		}
		if (Value.Equals(TEXT("False"), ESearchCase::IgnoreCase))
		{
			return LocBrowser(TEXT("UT2k4Browser_RulesList"), TEXT("FalseString"), TEXT("Disabled"));
		}
		if (Key.Equals(TEXT("ServerMode"), ESearchCase::IgnoreCase))
		{
			return Value.Equals(TEXT("dedicated"), ESearchCase::IgnoreCase) ? LocBrowser(TEXT("UT2k4Browser_RulesList"), TEXT("DedicatedString"), TEXT("Dedicated"))
				: LocBrowser(TEXT("UT2k4Browser_RulesList"), TEXT("NonDedicatedString"), TEXT("Non-Dedicated"));
		}
		return Value;
	}

	/** ServerResponseLine flag bits -> UT2k4Browser_ServersList icons (ServerIcons package of the player's content) */
	TArray<FString> FlagIcons(uint32 Flags)
	{
		TArray<FString> Out;
		if (Flags & 1u) Out.Add(TEXT("ServerIcons.Password"));
		if (Flags & (1u << 4)) Out.Add(TEXT("ServerIcons.Instagib"));
		return Out;
	}

	/** List frame drawn behind the multi-column lists (GUIMultiColumnListBox). */
	void ListBackground(FGUI& G, const FRect& R)
	{
		G.Draw().Rect(R.X, R.Y, R.W, R.H, FLinearColor(0.f, 0.f, 0.02f, 0.55f));
		G.Draw().Outline(R.X, R.Y, R.W, R.H, FArenaDraw::UTColor(FColor(60, 90, 170), 0.8f), 1.f);
	}

	/**
	 * GUIMultiColumnListHeader: one SectionHeaderTop button per column above a List() of the same width.
	 * Returns the clicked column or INDEX_NONE.
	 */
	int32 ColumnHeaders(FGUI& G, uint32 BaseId, const FRect& R, const TArray<FString>& Captions, const TArray<FListColumn>& Columns,
		int32 SortColumn, bool bDescending)
	{
		int32 Clicked = INDEX_NONE;
		float X = R.X;
		for (int32 i = 0; i < Columns.Num() && i < Captions.Num(); ++i)
		{
			const float W = Columns[i].Width * R.W;
			FString Caption = Captions[i];
			if (i == SortColumn)
			{
				Caption += bDescending ? TEXT(" v") : TEXT(" ^");
			}
			if (G.Button(BaseId + uint32(i), FRect(X, R.Y, W, R.H), TEXT("SectionHeaderTop"), Caption, TEXT("Sort by this column"), true,
				EFontScale::Small, ESound::Click, false))
			{
				Clicked = i;
			}
			X += W;
		}
		return Clicked;
	}

	const TArray<FListColumn>& ServerColumns()
	{
		static const TArray<FListColumn> C = { { 0.50f, EAlign::Left }, { 0.26f, EAlign::Left }, { 0.12f, EAlign::Center }, { 0.12f, EAlign::Center } };
		return C;
	}
	const TArray<FListColumn>& RuleColumns()
	{
		static const TArray<FListColumn> C = { { 0.5f, EAlign::Left }, { 0.5f, EAlign::Left } };
		return C;
	}
	const TArray<FListColumn>& PlayerColumns()
	{
		static const TArray<FListColumn> C = { { 0.46f, EAlign::Left }, { 0.18f, EAlign::Center }, { 0.18f, EAlign::Center }, { 0.18f, EAlign::Center } };
		return C;
	}

	/** List rect with room for the scroll bar removed when it will have one (so the headers line up with the cells). */
	float ListContentWidth(FGUI& G, const FRect& R, int32 NumRows, const FString& FontKey)
	{
		const float RowH = G.ListRowHeight(FontKey);
		const int32 Visible = FMath::Max(1, FMath::FloorToInt(R.H / RowH));
		return NumRows > Visible ? R.W - FMath::RoundToFloat(G.L(16.f)) - G.L(2.f) : R.W;
	}

	const TCHAR* const ListFont = TEXT("UT2SmallFont");
}

using namespace ArenaServerBrowserPageLocal;

// =================================================================================================
// Setup
// =================================================================================================

const TArray<FArenaServerBrowserPage::FTabDef>& FArenaServerBrowserPage::Tabs()
{
	static const TArray<FTabDef> Defs = {
		{ TEXT("Favourites"), nullptr, TEXT("Choose a server to join from among your favourites"), nullptr },
		{ TEXT("LAN"), nullptr, TEXT("View the UT2004 servers running on your LAN"), nullptr },
		{ TEXT("DM"), TEXT("DM"), TEXT("Deathmatch servers on the internet"), TEXT("xDeathMatch") },
		{ TEXT("TDM"), TEXT("TDM"), TEXT("Team Deathmatch servers on the internet"), TEXT("xTeamGame") },
		{ TEXT("CTF"), TEXT("CTF"), TEXT("Capture the Flag servers on the internet"), TEXT("xCTFGame") },
		{ TEXT("BR"), TEXT("BR"), TEXT("Bombing Run servers on the internet"), TEXT("xBombingRun") },
		{ TEXT("DOM"), TEXT("DDOM"), TEXT("Double Domination servers on the internet"), TEXT("xDoubleDom") },
		{ TEXT("ONS"), TEXT("ONS"), TEXT("Onslaught servers on the internet"), TEXT("ONSOnslaughtGame") },
		{ TEXT("AS"), TEXT("AS"), TEXT("Assault servers on the internet"), TEXT("ASGameInfo") },
		{ TEXT("INV"), TEXT("INV"), TEXT("Invasion servers on the internet"), TEXT("Invasion") },
		{ TEXT("LMS"), TEXT("LMS"), TEXT("Last Man Standing servers on the internet"), TEXT("xLastManStandingGame") },
		{ TEXT("All"), TEXT("All"), TEXT("Every server on the master servers, mods included"), TEXT("") },
	};
	return Defs;
}

FArenaServerBrowserPage::FArenaServerBrowserPage()
{
	const int32 Num = Tabs().Num();
	for (int32 i = 0; i < Num; ++i)
	{
		Browsers.Add(MakeUnique<FBrowser>());
		Fetched.Add(false);
	}
	const UArenaUserSettings* U = UArenaUserSettings::Get();
	Tab = FMath::Clamp(U ? U->BrowserTab : 2, 0, Num - 1);
}

FArenaServerBrowserPage::~FArenaServerBrowserPage() = default;

FBrowser& FArenaServerBrowserPage::BrowserFor(int32 TabIndex)
{
	return *Browsers[FMath::Clamp(TabIndex, 0, Browsers.Num() - 1)];
}

void FArenaServerBrowserPage::OnOpened()
{
	bOpenIP = false;
	Message.Reset();
	if (!Fetched[Tab])
	{
		Refresh(Tab);
	}
}

void FArenaServerBrowserPage::Refresh(int32 TabIndex)
{
	const FTabDef& T = Tabs()[TabIndex];
	FBrowser& B = BrowserFor(TabIndex);
	Fetched[TabIndex] = true;
	if (FCString::Strcmp(T.Id, TEXT("Favourites")) == 0)
	{
		const UArenaUserSettings* U = UArenaUserSettings::Get();
		B.RefreshAddresses(U ? U->ServerFavourites : TArray<FString>());
	}
	else if (FCString::Strcmp(T.Id, TEXT("LAN")) == 0)
	{
		B.RefreshLAN();
	}
	else
	{
		B.RefreshInternet(T.GameType ? FString(T.GameType) : FString());
	}
	RowsRevision = 0;
}

FString FArenaServerBrowserPage::JoinUnavailableText()
{
	return TEXT("Joining UT2004 servers is not available yet.");
}

// =================================================================================================
// Favourites / join
// =================================================================================================

bool FArenaServerBrowserPage::IsFavourite(const FString& Address) const
{
	const UArenaUserSettings* U = UArenaUserSettings::Get();
	return U && U->ServerFavourites.ContainsByPredicate([&Address](const FString& F) { return F.Equals(Address, ESearchCase::IgnoreCase); });
}

void FArenaServerBrowserPage::AddFavourite(const FString& Address)
{
	UArenaUserSettings* U = UArenaUserSettings::Get();
	FString Host;
	int32 Port = 0;
	if (!U || !ParseAddress(Address, Host, Port))
	{
		return;
	}
	const FString Clean = FString::Printf(TEXT("%s:%d"), *Host, Port);
	if (!IsFavourite(Clean))
	{
		U->ServerFavourites.Add(Clean);
		U->Save();
		Fetched[0] = false; // the Favourites tab fetches again when opened
		StatusOverride = FString::Printf(TEXT("Added %s to the favourites"), *Clean);
		StatusOverrideAt = FPlatformTime::Seconds();
	}
}

void FArenaServerBrowserPage::RemoveFavourite(const FString& Address)
{
	UArenaUserSettings* U = UArenaUserSettings::Get();
	if (!U)
	{
		return;
	}
	const int32 Removed = U->ServerFavourites.RemoveAll([&Address](const FString& F) { return F.Equals(Address, ESearchCase::IgnoreCase); });
	if (Removed > 0)
	{
		U->Save();
		StatusOverride = FString::Printf(TEXT("Removed %s from the favourites"), *Address);
		StatusOverrideAt = FPlatformTime::Seconds();
		if (Tab == 0)
		{
			Refresh(0);
		}
	}
}

void FArenaServerBrowserPage::JoinOrAsk(const ArenaServerBrowser::FServer& S)
{
	// a server whose details name an anti-cheat that never lets us play: ask first (spectate or cancel)
	FString AntiCheat;
#if ARENA_HAS_UE2_JOIN
	for (const TPair<FString, FString>& Rule : S.Rules)
	{
		AntiCheat = ArenaNet::FindSpawnBlockingAntiCheat(Rule.Key + TEXT(" ") + Rule.Value);
		if (!AntiCheat.IsEmpty())
		{
			break;
		}
	}
#endif
	if (AntiCheat.IsEmpty())
	{
		Join(S.Address());
		return;
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("Server browser: %s runs %s, asking to spectate"), *S.Address(), *AntiCheat);
	AntiCheatPrompt = AntiCheat;
	AntiCheatAddress = S.Address();
}

void FArenaServerBrowserPage::Join(const FString& Address)
{
	if (Address.IsEmpty())
	{
		return;
	}
	// never log a password option
	const int32 PwAt = Address.Find(TEXT("?Password="), ESearchCase::IgnoreCase);
	UE_LOG(LogOpenUT2k4, Log, TEXT("Server browser: join %s%s"), *(PwAt >= 0 ? Address.Left(PwAt) : Address), PwAt >= 0 ? TEXT(" (with a password)") : TEXT(""));
	{
		// OpenUT2k4 servers (Host Game, ArenaGameSession.h) are joined with UE5's net driver: listed ones carry
		// ArenaHost::OpenServerFlag; an address not in the list (Open IP) is asked once on its query port
		FString Base = Address;
		int32 Q = INDEX_NONE;
		if (Base.FindChar(TEXT('?'), Q))
		{
			Base.LeftInline(Q);
		}
		bool bKnown = false, bOpenServer = false, bLAN = false;
		for (int32 b = 0; b < Browsers.Num() && !bKnown; ++b)
		{
			if (!Browsers[b])
			{
				continue;
			}
			for (const ArenaServerBrowser::FServer& S : Browsers[b]->GetServers())
			{
				if (S.bHasInfo && (S.Address() == Base || FString::Printf(TEXT("%s:%d"), *S.Host, S.Port) == Base))
				{
					bKnown = true;
					bOpenServer = ArenaHost::IsOpenServer(S.Flags);
					bLAN = Tabs().IsValidIndex(b) && Tabs()[b].Id && FCString::Strcmp(Tabs()[b].Id, TEXT("LAN")) == 0;
					break;
				}
			}
		}
		FString ProbeHost;
		int32 ProbePort = 0;
		if (!bKnown && ParseAddress(Base, ProbeHost, ProbePort))
		{
			bOpenServer = ArenaHost::ProbeOpenServer(ProbeHost, ProbePort);
		}
		if (bOpenServer)
		{
			if (ArenaHost::JoinServer(Address + (bLAN ? TEXT("?LAN") : TEXT(""))))
			{
				StatusOverride = FString::Printf(TEXT("Joining the OpenUT2k4 server %s..."), *Base);
				StatusOverrideAt = FPlatformTime::Seconds();
			}
			else
			{
				Message = FString::Printf(TEXT("Could not join %s."), *Base);
			}
			return;
		}
	}
#if ARENA_HAS_UE2_JOIN
	if (ArenaNet::JoinServer(Address))
	{
		FString Base = Address;
		int32 Q = INDEX_NONE;
		if (Base.FindChar(TEXT('?'), Q))
		{
			Base.LeftInline(Q);
		}
		JoiningAddress = Base;
		bWatchJoin = true;
		StatusOverride = ArenaNet::GetConnectionStatus();
		if (StatusOverride.IsEmpty())
		{
			StatusOverride = FString::Printf(TEXT("Connecting to %s..."), *Address);
		}
		StatusOverrideAt = FPlatformTime::Seconds();
		return;
	}
	const FString Status = ArenaNet::GetConnectionStatus();
	Message = Status.IsEmpty() ? FString::Printf(TEXT("Could not join %s."), *Address) : Status;
#else
	Message = JoinUnavailableText();
#endif
}

// =================================================================================================
// Rows
// =================================================================================================

const FServer* FArenaServerBrowserPage::SelectedServer(const FBrowser& B) const
{
	if (!RowServers.IsValidIndex(SelectedRow))
	{
		return nullptr;
	}
	const int32 Index = RowServers[SelectedRow];
	return B.GetServers().IsValidIndex(Index) ? &B.GetServers()[Index] : nullptr;
}

void FArenaServerBrowserPage::RebuildRows(const FBrowser& B, bool bKeepOrder)
{
	const UArenaUserSettings* U = UArenaUserSettings::Get();
	const bool bHideEmpty = U && U->bBrowserHideEmpty;
	const bool bHideFull = U && U->bBrowserHideFull;
	const TArray<FServer>& Servers = B.GetServers();
	RowServers.Reset();
	for (int32 i = 0; i < Servers.Num(); ++i)
	{
		const FServer& S = Servers[i];
		if (bHideEmpty && S.bHasInfo && S.NumPlayers == 0)
		{
			continue;
		}
		if (bHideFull && S.bHasInfo && S.MaxPlayers > 0 && S.NumPlayers >= S.MaxPlayers)
		{
			continue;
		}
		RowServers.Add(i);
	}
	const ESort SortBy = Sort;
	const bool bDesc = bSortDescending;
	auto Less = [&Servers, SortBy, bDesc](int32 A, int32 B)
	{
		const FServer& SA = Servers[A];
		const FServer& SB = Servers[B];
		// servers that did not answer always go last (UT2004 dropped them from the list)
		const bool bDeadA = SA.State == EServerState::NoResponse, bDeadB = SB.State == EServerState::NoResponse;
		if (bDeadA != bDeadB)
		{
			return bDeadB;
		}
		int32 Cmp = 0;
		switch (SortBy)
		{
		case ESort::Name: Cmp = ArenaServerBrowser::StripColours(SA.Name).Compare(ArenaServerBrowser::StripColours(SB.Name), ESearchCase::IgnoreCase); break;
		case ESort::Map: Cmp = SA.Map.Compare(SB.Map, ESearchCase::IgnoreCase); break;
		case ESort::Players: Cmp = SB.NumPlayers - SA.NumPlayers; break; // most players first
		case ESort::Ping:
		default:
		{
			// unmeasured pings last either way
			const bool bA = SA.Ping >= 0, bB = SB.Ping >= 0;
			if (bA != bB) return bA;
			Cmp = SA.Ping - SB.Ping;
			break;
		}
		}
		return bDesc ? Cmp > 0 : Cmp < 0;
	};
	if (bKeepOrder && RowAddresses.Num() > 0)
	{
		// a server was clicked and the mouse is still on the list: servers already listed keep their rows (the click
		// queries the server again and its new ping must not move the row away before the second click of a double
		// click); new ones go below, sorted among themselves. The list is sorted again once the mouse leaves it.
		TMap<FString, int32> OldRow;
		for (int32 r = 0; r < RowAddresses.Num(); ++r)
		{
			OldRow.Add(RowAddresses[r], r);
		}
		RowServers.StableSort([&Servers, &OldRow, &Less](int32 A, int32 B)
		{
			const int32* RA = OldRow.Find(Servers[A].Address());
			const int32* RB = OldRow.Find(Servers[B].Address());
			if (RA && RB) return *RA < *RB;
			if (RA || RB) return RA != nullptr;
			return Less(A, B);
		});
		bSortDeferred = true;
	}
	else
	{
		RowServers.StableSort(Less);
		bSortDeferred = false;
	}
	Rows.Reset();
	RowAddresses.Reset();
	SelectedRow = -1;
	for (int32 r = 0; r < RowServers.Num(); ++r)
	{
		const FServer& S = Servers[RowServers[r]];
		FListItem It;
		const FString Name = S.Name.IsEmpty() ? S.Address() : S.Name;
		const FString Players = S.bHasInfo ? FString::Printf(TEXT("%d/%d"), S.NumPlayers, S.MaxPlayers) : FString(TEXT("-"));
		FString Ping;
		switch (S.State)
		{
		case EServerState::Responded: Ping = S.Ping >= 0 ? FString::FromInt(S.Ping) : FString(TEXT("?")); break;
		case EServerState::NoResponse: Ping = TEXT("N/A"); break;
		default: Ping = TEXT("..."); break;
		}
		It.Cells = { Name, S.Map, Players, Ping };
		It.Icons = FlagIcons(S.Flags);
		It.bDim = S.State == EServerState::NoResponse;
		Rows.Add(MoveTemp(It));
		RowAddresses.Add(S.Address());
		if (!SelectedAddress.IsEmpty() && S.Address() == SelectedAddress)
		{
			SelectedRow = r;
		}
	}
	RowsRevision = B.GetRevision();
	RowsTab = Tab;
	bRowsHideEmpty = bHideEmpty;
	bRowsHideFull = bHideFull;
}

// =================================================================================================
// Drawing
// =================================================================================================

void FArenaServerBrowserPage::DrawServerList(FGUI& G, const FRect& R, FBrowser& B, bool& bOutActivated)
{
	static const TCHAR* Fallback[4] = { TEXT("Server Name"), TEXT("Map"), TEXT("Players"), TEXT("Ping") };
	TArray<FString> Captions;
	for (int32 i = 0; i < 4; ++i)
	{
		// UT2k4Browser_ServersList ColumnHeadings: (icons, "Server Name", "Map", "Players", "Ping")
		Captions.Add(LocArray(TEXT("GUI2K4"), TEXT("UT2k4Browser_ServersList"), TEXT("ColumnHeadings"), i + 1, Fallback[i]));
	}
	const float HeadH = FMath::RoundToFloat(G.ListRowHeight(ListFont) * 1.35f);
	const FRect ListR(R.X, R.Y + HeadH, R.W, R.H - HeadH);
	ListBackground(G, R);
	const float CW = ListContentWidth(G, ListR, Rows.Num(), ListFont);
	const int32 Clicked = ColumnHeaders(G, FGUI::Id(TEXT("BrowserHead")), FRect(R.X, R.Y, CW, HeadH), Captions, ServerColumns(), int32(Sort), bSortDescending);
	if (Clicked != INDEX_NONE)
	{
		const ESort NewSort = ESort(Clicked);
		bSortDescending = NewSort == Sort ? !bSortDescending : false;
		Sort = NewSort;
		RebuildRows(B);
	}
	// after a click on a server its rows stay put until the mouse leaves the list (see RebuildRows)
	bListHeld = bListHeld && ListR.Contains(G.Mouse());
	int32 Sel = SelectedRow;
	if (G.List(FGUI::Id(TEXT("BrowserServers")), ListR, Rows, Sel, bOutActivated, TEXT("Double click a server to join it"), ListFont, false,
		&ServerColumns()))
	{
		bListHeld = true;
		SelectedRow = Sel;
		if (const FServer* S = SelectedServer(B))
		{
			SelectedAddress = S->Address();
		}
		RulesSel = PlayersSel = -1;
	}
	if (Rows.Num() == 0)
	{
		const FFontRef F = G.Font(TEXT("UT2MenuFont"));
		const FString Text = B.IsBusy() ? B.GetStatus() : (Tab == 0 ? FString(TEXT("No favourites yet: select a server and choose ADD FAVORITE, or use OPEN IP."))
			: FString(TEXT("No servers to show.")));
		G.TextIn(F, Text, ListR.Inset(G.L(12.f), 0.f, G.L(12.f), ListR.H * 0.5f), FLinearColor(0.8f, 0.85f, 1.f, 0.9f), EAlign::Center);
	}
}

void FArenaServerBrowserPage::DrawDetails(FGUI& G, const FRect& RulesR, const FRect& PlayersR, const FServer* S)
{
	const float HeadH = FMath::RoundToFloat(G.ListRowHeight(ListFont) * 1.35f);
	// rules (UT2k4Browser_RulesList: Setting, Value)
	{
		TArray<FListItem> Items;
		if (S)
		{
			for (const TPair<FString, FString>& Rule : S->Rules)
			{
				FListItem It;
				It.Cells = { RuleName(Rule.Key), RuleValue(Rule.Key, Rule.Value) };
				Items.Add(MoveTemp(It));
			}
		}
		ListBackground(G, RulesR);
		const FRect ListR(RulesR.X, RulesR.Y + HeadH, RulesR.W, RulesR.H - HeadH);
		const TArray<FString> Captions = {
			LocArray(TEXT("GUI2K4"), TEXT("UT2k4Browser_RulesList"), TEXT("ColumnHeadings"), 0, TEXT("Setting")),
			LocArray(TEXT("GUI2K4"), TEXT("UT2k4Browser_RulesList"), TEXT("ColumnHeadings"), 1, TEXT("Value")) };
		ColumnHeaders(G, FGUI::Id(TEXT("BrowserRulesHead")), FRect(RulesR.X, RulesR.Y, ListContentWidth(G, ListR, Items.Num(), ListFont), HeadH),
			Captions, RuleColumns(), INDEX_NONE, false);
		bool bAct = false;
		G.List(FGUI::Id(TEXT("BrowserRules")), ListR, Items, RulesSel, bAct, TEXT("The settings of the selected server"), ListFont, false, &RuleColumns());
	}
	// players (UT2k4Browser_PlayersList: Name, Score, Team (Rank in UT2004: stats), Ping)
	{
		TArray<FListItem> Items;
		if (S)
		{
			TArray<FPlayer> Sorted = S->Players;
			Sorted.StableSort([](const FPlayer& A, const FPlayer& B) { return A.Score > B.Score; });
			for (const FPlayer& P : Sorted)
			{
				FListItem It;
				const TCHAR* Team = P.Team == 0 ? TEXT("Red") : (P.Team == 1 ? TEXT("Blue") : TEXT(""));
				It.Cells = { P.Name, FString::FromInt(P.Score), Team, FString::FromInt(P.Ping) };
				Items.Add(MoveTemp(It));
			}
		}
		ListBackground(G, PlayersR);
		const FRect ListR(PlayersR.X, PlayersR.Y + HeadH, PlayersR.W, PlayersR.H - HeadH);
		const TArray<FString> Captions = {
			LocArray(TEXT("GUI2K4"), TEXT("UT2k4Browser_PlayersList"), TEXT("ColumnHeadings"), 0, TEXT("Name")),
			LocArray(TEXT("GUI2K4"), TEXT("UT2k4Browser_PlayersList"), TEXT("ColumnHeadings"), 1, TEXT("Score")),
			FString(TEXT("Team")),
			LocArray(TEXT("GUI2K4"), TEXT("UT2k4Browser_PlayersList"), TEXT("ColumnHeadings"), 3, TEXT("Ping")) };
		ColumnHeaders(G, FGUI::Id(TEXT("BrowserPlayersHead")), FRect(PlayersR.X, PlayersR.Y, ListContentWidth(G, ListR, Items.Num(), ListFont), HeadH),
			Captions, PlayerColumns(), INDEX_NONE, false);
		bool bAct = false;
		G.List(FGUI::Id(TEXT("BrowserPlayers")), ListR, Items, PlayersSel, bAct, TEXT("The players on the selected server"), ListFont, false, &PlayerColumns());
		if (S && Items.Num() == 0 && S->State == EServerState::Responded)
		{
			G.TextIn(G.Font(ListFont), S->DetailsAt < 0.0 ? FString(TEXT("Querying...")) : FString(TEXT("No players")),
				ListR.Inset(0.f, 0.f, 0.f, FMath::Max(0.f, ListR.H - G.ListRowHeight(ListFont) * 2.f)), FLinearColor(0.75f, 0.8f, 0.95f, 0.85f), EAlign::Center);
		}
	}
}

void FArenaServerBrowserPage::DrawOpenIP(FGUI& G)
{
	// UT2k4Browser_OpenIP: a band across the screen with the address field, OK and CANCEL
	const FRect P = G.Page();
	const float W = G.ScreenW(), H = G.ScreenH();
	G.Draw().Rect(0.f, 0.f, W, H, FLinearColor(0.f, 0.f, 0.f, 0.55f));
	const FRect Band(0.f, 0.33f * H, W, 0.3f * H);
	if (!G.Image(TEXT("2K4Menus.NewControls.Display2"), Band, EImgStyle::Stretched, FLinearColor::White))
	{
		G.FallbackBox(Band, EMenuState::Blurry, false);
	}
	const FFontRef F = G.Font(TEXT("UT2HeaderFont"));
	G.TextIn(F, LocBrowser(TEXT("UT2k4Browser_OpenIP"), TEXT("IPDesc.Caption"), TEXT("Enter New IP Address")),
		FRect(P.X, Band.Y + 0.04f * H, P.W, G.L(32.f)), FArenaDraw::UTColor(FColor(253, 237, 244)), EAlign::Center, true);
	const uint32 EditId = FGUI::Id(TEXT("OpenIPEdit"));
	if (G.GetFocus() == 0)
	{
		G.SetFocus(EditId);
	}
	G.EditBox(EditId, FRect(P.X + 0.15f * P.W, Band.Y + 0.12f * H, 0.7f * P.W, 0.04f * H),
		LocBrowser(TEXT("UT2k4Browser_OpenIP"), TEXT("IpEntryBox.Caption"), TEXT("IP Address: ")), OpenIPText,
		LocBrowser(TEXT("UT2k4Browser_OpenIP"), TEXT("EditBoxHint"), TEXT("Enter the server address: IP or host name, and the port after a colon")), 0.25f, 64);
	const float BY = Band.Y + 0.21f * H, BH = 0.05f * H, BW = 0.2f * P.W;
	FString Host;
	int32 Port = 0;
	const bool bValid = ParseAddress(OpenIPText, Host, Port);
	if (G.Button(FGUI::Id(TEXT("OpenIPOk")), FRect(P.X + 0.1f * P.W, BY, BW, BH), TEXT("RoundButton"), TEXT("OK"),
		LocBrowser(TEXT("UT2k4Browser_OpenIP"), TEXT("OKButtonHint"), TEXT("Connect to this address")), bValid))
	{
		bOpenIP = false;
		G.SetFocus(0);
		Join(FString::Printf(TEXT("%s:%d"), *Host, Port));
		return;
	}
	if (G.Button(FGUI::Id(TEXT("OpenIPFav")), FRect(P.X + 0.4f * P.W, BY, BW, BH), TEXT("RoundButton"),
		LocBrowser(TEXT("UT2k4Browser_ServerListPageFavorites"), TEXT("AddFavoriteCaption"), TEXT("ADD FAVORITE")), TEXT("Add this address to the favourites"), bValid))
	{
		AddFavourite(OpenIPText);
		bOpenIP = false;
		G.SetFocus(0);
		return;
	}
	if (G.Button(FGUI::Id(TEXT("OpenIPCancel")), FRect(P.X + 0.7f * P.W, BY, BW, BH), TEXT("RoundButton"), TEXT("CANCEL"),
		LocBrowser(TEXT("UT2k4Browser_OpenIP"), TEXT("CancelButtonHint"), TEXT("Close without connecting")), true) || G.BackPressed())
	{
		bOpenIP = false;
		G.SetFocus(0);
	}
}

void FArenaServerBrowserPage::WatchJoin()
{
#if ARENA_HAS_UE2_JOIN
	if (!bWatchJoin || ArenaNet::IsConnectedToServer())
	{
		return;
	}
	bWatchJoin = false;
	const FString Code = ArenaNet::GetLastFailCode();
	if (Code == TEXT("NEEDPW") || Code == TEXT("WRONGPW"))
	{
		bPasswordBox = true;
		bWrongPassword = Code == TEXT("WRONGPW");
		PasswordText.Reset();
	}
	else if (Code == TEXT("NEEDSTATS") || ArenaNet::NeedsStatsPrompt())
	{
		bStatsPrompt = true;
	}
	else
	{
		const FString Status = ArenaNet::GetConnectionStatus();
		if (!Code.IsEmpty() || !Status.IsEmpty())
		{
			Message = Status.IsEmpty() ? FString::Printf(TEXT("Could not join %s (%s)."), *JoiningAddress, *Code) : Status;
		}
	}
#endif
}

void FArenaServerBrowserPage::DrawPassword(FGUI& G)
{
	// UT2K4GetPassword: the question, the password field, SUBMIT / CANCEL
	const FRect P = G.Page();
	const float W = G.ScreenW(), H = G.ScreenH();
	G.Draw().Rect(0.f, 0.f, W, H, FLinearColor(0.f, 0.f, 0.f, 0.55f));
	const FRect Band(0.f, 0.33f * H, W, 0.3f * H);
	if (!G.Image(TEXT("2K4Menus.NewControls.Display2"), Band, EImgStyle::Stretched, FLinearColor::White))
	{
		G.FallbackBox(Band, EMenuState::Blurry, false);
	}
	const FString Question = bWrongPassword
		? LocBrowser(TEXT("UT2K4GetPassword"), TEXT("IncorrectPassword"), TEXT("Incorrect password specified.  Please try again."))
		: LocBrowser(TEXT("UT2K4GetPassword"), TEXT("GetPassLabel.Caption"), TEXT("A password is required to play on this server."));
	G.TextIn(G.Font(TEXT("UT2HeaderFont")), Question, FRect(P.X, Band.Y + 0.04f * H, P.W, G.L(32.f)), FArenaDraw::UTColor(FColor(253, 237, 244)),
		EAlign::Center, true);
	const uint32 EditId = FGUI::Id(TEXT("JoinPassword"));
	if (G.GetFocus() == 0)
	{
		G.SetFocus(EditId);
	}
	// the GUI's edit box has no masked mode: the password is shown while typed (it is never logged or saved)
	FString Shown = PasswordText;
	G.EditBox(EditId, FRect(P.X + 0.15f * P.W, Band.Y + 0.12f * H, 0.7f * P.W, 0.04f * H),
		LocBrowser(TEXT("UT2K4GetPassword"), TEXT("GetPassPW.Caption"), TEXT("Server Password")), Shown, TEXT("The password of this server"), 0.3f, 32);
	PasswordText = Shown;
	const float BY = Band.Y + 0.21f * H, BH = 0.05f * H, BW = 0.2f * P.W;
	FString Clean;
	for (const TCHAR C : PasswordText)
	{
		if (C != TEXT('?') && C != TEXT('#') && C != TEXT('/') && !FChar::IsWhitespace(C))
		{
			Clean.AppendChar(C);
		}
	}
	if (G.Button(FGUI::Id(TEXT("JoinPasswordOk")), FRect(P.X + 0.2f * P.W, BY, BW, BH), TEXT("RoundButton"),
		LocBrowser(TEXT("UT2K4GetPassword"), TEXT("GetPassRetry.Caption"), TEXT("SUBMIT")), TEXT("Join with this password"), !Clean.IsEmpty()))
	{
		bPasswordBox = false;
		G.SetFocus(0);
		Join(JoiningAddress + TEXT("?Password=") + Clean);
		PasswordText.Reset();
		return;
	}
	if (G.Button(FGUI::Id(TEXT("JoinPasswordCancel")), FRect(P.X + 0.6f * P.W, BY, BW, BH), TEXT("RoundButton"),
		LocBrowser(TEXT("UT2K4GetPassword"), TEXT("GetPassFail.Caption"), TEXT("CANCEL")), TEXT("Do not join this server"), true) || G.BackPressed())
	{
		bPasswordBox = false;
		PasswordText.Reset();
		G.SetFocus(0);
	}
}

void FArenaServerBrowserPage::DrawStatsPrompt(FGUI& G)
{
	FString Header = TEXT("Stats required"), Desc = TEXT("This server requires stats to be enabled."), Yes = TEXT("YES"), No = TEXT("NO");
#if ARENA_HAS_UE2_JOIN
	ArenaNet::GetStatsPrompt(Header, Desc, Yes, No);
#endif
	const FRect P = G.Page();
	const float W = G.ScreenW(), H = G.ScreenH();
	G.Draw().Rect(0.f, 0.f, W, H, FLinearColor(0.f, 0.f, 0.f, 0.55f));
	const FRect Band(0.f, 0.3f * H, W, 0.38f * H);
	if (!G.Image(TEXT("2K4Menus.NewControls.Display2"), Band, EImgStyle::Stretched, FLinearColor::White))
	{
		G.FallbackBox(Band, EMenuState::Blurry, false);
	}
	G.TextIn(G.Font(TEXT("UT2HeaderFont")), Header, FRect(P.X, Band.Y + 0.04f * H, P.W, G.L(32.f)), FArenaDraw::UTColor(FColor(253, 237, 244)),
		EAlign::Center, true);
	const FFontRef F = G.Font(TEXT("UT2MenuFont"));
	float Y = Band.Y + 0.1f * H;
	for (const FString& Line : G.Wrap(F, Desc, P.W * 0.8f))
	{
		G.TextIn(F, Line, FRect(P.X + P.W * 0.1f, Y, P.W * 0.8f, F.Height), FLinearColor::White, EAlign::Center);
		Y += F.Height;
	}
	// UT2K4StatsPrompt: YES opens the stats settings (Settings, Game tab); closing them joins again when the account is set
	const float BY = Band.B() - 0.08f * H, BH = 0.05f * H, BW = 0.2f * P.W;
	if (G.Button(FGUI::Id(TEXT("StatsYes")), FRect(P.X + 0.2f * P.W, BY, BW, BH), TEXT("RoundButton"), Yes,
		TEXT("Turn on Track Stats and set your stats username and password, then join again")))
	{
		bStatsPrompt = false;
		bStatsSettings = true;
		StatsSettings.bOpenStatsTab = true;
		G.ResetInteraction();
		G.SetFocus(0);
		return;
	}
	if (G.Button(FGUI::Id(TEXT("StatsNo")), FRect(P.X + 0.6f * P.W, BY, BW, BH), TEXT("RoundButton"), No, TEXT("Do not join this server")) || G.BackPressed())
	{
		bStatsPrompt = false;
		G.SetFocus(0);
	}
}

void FArenaServerBrowserPage::DrawAntiCheatPrompt(FGUI& G)
{
	const FRect P = G.Page();
	const float W = G.ScreenW(), H = G.ScreenH();
	G.Draw().Rect(0.f, 0.f, W, H, FLinearColor(0.f, 0.f, 0.f, 0.55f));
	const FRect Band(0.f, 0.3f * H, W, 0.38f * H);
	if (!G.Image(TEXT("2K4Menus.NewControls.Display2"), Band, EImgStyle::Stretched, FLinearColor::White))
	{
		G.FallbackBox(Band, EMenuState::Blurry, false);
	}
	G.TextIn(G.Font(TEXT("UT2HeaderFont")), FString::Printf(TEXT("This server runs %s"), *AntiCheatPrompt),
		FRect(P.X, Band.Y + 0.04f * H, P.W, G.L(32.f)), FArenaDraw::UTColor(FColor(253, 237, 244)), EAlign::Center, true);
	const FString Desc = FString::Printf(TEXT("%s is an anti-cheat that holds every new player until its own check runs in the player's game. ")
		TEXT("OpenUT2k4 does not run anti-cheat software, so %s will never let you play on this server.\n\n")
		TEXT("You can join as a spectator and watch the match."), *AntiCheatPrompt, *AntiCheatPrompt);
	const FFontRef F = G.Font(TEXT("UT2MenuFont"));
	float Y = Band.Y + 0.1f * H;
	for (const FString& Line : G.Wrap(F, Desc, P.W * 0.8f))
	{
		G.TextIn(F, Line, FRect(P.X + P.W * 0.1f, Y, P.W * 0.8f, F.Height), FLinearColor::White, EAlign::Center);
		Y += F.Height;
	}
	const float BY = Band.B() - 0.08f * H, BH = 0.05f * H, BW = 0.2f * P.W;
	if (G.Button(FGUI::Id(TEXT("AntiCheatSpectate")), FRect(P.X + 0.2f * P.W, BY, BW, BH), TEXT("RoundButton"), TEXT("SPECTATE"),
		TEXT("Join this server as a spectator")))
	{
		const FString Address = AntiCheatAddress;
		AntiCheatPrompt.Reset();
		AntiCheatAddress.Reset();
		G.SetFocus(0);
		Join(Address + TEXT("?SpectatorOnly=1"));
		return;
	}
	if (G.Button(FGUI::Id(TEXT("AntiCheatCancel")), FRect(P.X + 0.6f * P.W, BY, BW, BH), TEXT("RoundButton"), TEXT("CANCEL"),
		TEXT("Do not join this server")) || G.BackPressed())
	{
		AntiCheatPrompt.Reset();
		AntiCheatAddress.Reset();
		G.SetFocus(0);
	}
}

void FArenaServerBrowserPage::DrawMessage(FGUI& G)
{
	const FRect P = G.Page();
	const float W = G.ScreenW(), H = G.ScreenH();
	G.Draw().Rect(0.f, 0.f, W, H, FLinearColor(0.f, 0.f, 0.f, 0.55f));
	const FRect Band(0.f, 0.36f * H, W, 0.24f * H);
	if (!G.Image(TEXT("2K4Menus.NewControls.Display2"), Band, EImgStyle::Stretched, FLinearColor::White))
	{
		G.FallbackBox(Band, EMenuState::Blurry, false);
	}
	G.TextIn(G.Font(TEXT("UT2HeaderFont")), Message, FRect(P.X, Band.Y + 0.06f * H, P.W, G.L(32.f)), FArenaDraw::UTColor(FColor(253, 237, 244)),
		EAlign::Center, true);
	const uint32 OkId = FGUI::Id(TEXT("BrowserMsgOk"));
	if (G.GetFocus() == 0)
	{
		G.SetFocus(OkId);
	}
	if (G.Button(OkId, FRect(P.X + 0.4f * P.W, Band.Y + 0.14f * H, 0.2f * P.W, 0.05f * H), TEXT("RoundButton"), TEXT("OK"), FString()) || G.BackPressed())
	{
		Message.Reset();
		G.SetFocus(0);
	}
}

bool FArenaServerBrowserPage::DrawJoinProgress(FGUI& G, const FRect& Band)
{
#if ARENA_HAS_UE2_JOIN
	FString JoinText;
	float JoinFraction = 0.f;
	if (!ArenaNet::GetJoinProgress(JoinText, JoinFraction))
	{
		return false;
	}
	G.BeginOverlay(Band);
	G.Draw().Rect(Band.X, Band.Y, Band.W, Band.H, FLinearColor(0.f, 0.f, 0.f, 0.75f));
	// the file on the first line; the totals and notes (" | " parts) on the second
	FString Line1 = JoinText, Line2;
	JoinText.Split(TEXT(" | "), &Line1, &Line2);
	Line2.ReplaceInline(TEXT(" | "), TEXT(" - "));
	G.TextIn(G.Font(TEXT("UT2MenuFont")), Line1, FRect(Band.X + G.L(12.f), Band.Y + G.L(6.f), Band.W - G.L(24.f), G.L(28.f)),
		FLinearColor(1.f, 0.85f, 0.3f, 1.f), EAlign::Center, true);
	if (!Line2.IsEmpty())
	{
		G.TextIn(G.Font(TEXT("UT2SmallFont")), Line2, FRect(Band.X + G.L(12.f), Band.Y + G.L(32.f), Band.W - G.L(24.f), G.L(24.f)),
			FLinearColor(0.85f, 0.85f, 0.9f, 1.f), EAlign::Center, true);
	}
	const FRect Bar(Band.X + Band.W * 0.1f, Band.Y + G.L(60.f), Band.W * 0.8f, G.L(14.f));
	G.Draw().Rect(Bar.X, Bar.Y, Bar.W, Bar.H, FLinearColor(0.15f, 0.15f, 0.25f, 1.f));
	G.Draw().Rect(Bar.X, Bar.Y, Bar.W * FMath::Clamp(JoinFraction, 0.f, 1.f), Bar.H, FLinearColor(0.95f, 0.75f, 0.2f, 1.f));
	// UT2004's download dialog has a Cancel button: stop downloading and leave the server
	const float CW = G.L(160.f);
	if (G.Button(FGUI::Id(TEXT("JoinDownloadCancel")), FRect(Band.X + (Band.W - CW) * 0.5f, Band.Y + G.L(82.f), CW, G.L(34.f)), TEXT("RoundButton"),
		TEXT("CANCEL"), TEXT("Stop downloading and leave the server"), true))
	{
		ArenaNet::LeaveServer();
	}
	G.EndOverlay();
	return true;
#else
	return false;
#endif
}

bool FArenaServerBrowserPage::Draw(FGUI& G)
{
	if (bStatsSettings)
	{
		if (StatsSettings.Draw(G))
		{
			bStatsSettings = false;
			G.ResetInteraction();
			G.SetFocus(0);
			const UArenaUserSettings* U = UArenaUserSettings::Get();
#if ARENA_HAS_UE2_JOIN
			if (U->bTrackStats && U->HasValidStatsLogin() && ArenaNet::RetryLastJoin())
			{
				bWatchJoin = true;
			}
			else
#endif
			{
				// UT2K4SettingsPage.InvalidStats
				Message = !U->bTrackStats ? FString(TEXT("Track Stats is off: this server can't be joined."))
					: Loc(TEXT("GUI2K4"), TEXT("UT2K4SettingsPage"), TEXT("InvalidStats"), TEXT("Invalid Stats Info"));
			}
		}
		return false;
	}
	const TArray<FTabDef>& Defs = Tabs();
	TArray<FString> Captions, Hints;
	for (int32 i = 0; i < Defs.Num(); ++i)
	{
		if (i == 0)
		{
			Captions.Add(LocArray(TEXT("GUI2K4"), TEXT("UT2k4ServerBrowser"), TEXT("PanelCaption"), 2, TEXT("Favorites")));
		}
		else if (i == 1)
		{
			Captions.Add(LocArray(TEXT("GUI2K4"), TEXT("UT2k4ServerBrowser"), TEXT("PanelCaption"), 3, TEXT("LAN")));
		}
		else
		{
			Captions.Add(Defs[i].Caption);
		}
		Hints.Add(Defs[i].Hint);
	}
	for (TUniquePtr<FBrowser>& B : Browsers)
	{
		B->Tick();
	}
	const int32 OldTab = Tab;
	WatchJoin();
	const bool bModal = bOpenIP || !Message.IsEmpty() || bPasswordBox || bStatsPrompt || !AntiCheatPrompt.IsEmpty();
	const FString PageCaption = LocBrowser(TEXT("UT2k4ServerBrowser"), TEXT("ServerBrowserHeader.Caption"), TEXT("Server Browser"));
	const FTabbedFrame Frame = TabbedPage(G, FGUI::Id(TEXT("BrowserTabs")), PageCaption, Captions, Hints, Tab, TabChangedAt);
	if (Tab != OldTab)
	{
		if (bModal)
		{
			Tab = OldTab;
		}
		else
		{
			G.ResetInteraction();
			SelectedRow = -1;
			SelectedAddress.Reset();
			RowsRevision = 0;
			if (UArenaUserSettings* U = UArenaUserSettings::Get())
			{
				U->BrowserTab = Tab;
				U->Save();
			}
			if (!Fetched[Tab])
			{
				Refresh(Tab);
			}
		}
	}
	FBrowser& B = BrowserFor(Tab);
	UArenaUserSettings* U = UArenaUserSettings::Get();
	if (RowsTab != Tab || bRowsHideEmpty != U->bBrowserHideEmpty || bRowsHideFull != U->bBrowserHideFull)
	{
		RebuildRows(B);
	}
	else if (RowsRevision != B.GetRevision() || (bSortDeferred && !bListHeld))
	{
		// new answers (pings, details): rows stay put after a click on the list while the mouse is on it
		RebuildRows(B, bListHeld);
	}

	const FRect& Panel = Frame.Panel;
	// filter row: No Empty Servers, No Full Servers (Ut2K4_FilterEdit), status at the right
	const float RowH = FMath::RoundToFloat(G.ScreenH() * 0.03f);
	{
		bool bEmpty = U->bBrowserHideEmpty, bFull = U->bBrowserHideFull;
		if (G.CheckBox(FGUI::Id(TEXT("BrowserNoEmpty")), FRect(Panel.X, Panel.Y, Panel.W * 0.3f, RowH),
			LocBrowser(TEXT("Ut2K4_FilterEdit"), TEXT("ckEmpty.Caption"), TEXT("No Empty Servers")), bEmpty, TEXT("Hide servers without players"), 0.85f))
		{
			U->bBrowserHideEmpty = bEmpty;
			U->Save();
		}
		if (G.CheckBox(FGUI::Id(TEXT("BrowserNoFull")), FRect(Panel.X + Panel.W * 0.32f, Panel.Y, Panel.W * 0.3f, RowH),
			LocBrowser(TEXT("Ut2K4_FilterEdit"), TEXT("ckFull.Caption"), TEXT("No Full Servers")), bFull, TEXT("Hide servers that are full"), 0.85f))
		{
			U->bBrowserHideFull = bFull;
			U->Save();
		}
		FString Status = B.GetStatus();
		if (StatusOverrideAt > 0.0 && FPlatformTime::Seconds() - StatusOverrideAt < 6.0)
		{
			Status = StatusOverride;
		}
#if ARENA_HAS_UE2_JOIN
		// while joining (logging in, downloading packages, converting them) the connection's own status
		if (ArenaNet::IsConnectedToServer())
		{
			const FString NetStatus = ArenaNet::GetConnectionStatus();
			if (!NetStatus.IsEmpty())
			{
				Status = NetStatus;
			}
		}
#endif
		else if (RowServers.Num() != B.GetServers().Num() && !B.IsBusy())
		{
			Status += FString::Printf(TEXT(" (%d shown)"), RowServers.Num());
		}
		G.TextIn(G.Font(TEXT("UT2MenuFont")), Status, FRect(Panel.X + Panel.W * 0.64f, Panel.Y, Panel.W * 0.36f, RowH), FLinearColor(1.f, 0.85f, 0.3f, 1.f),
			EAlign::Right, true);
	}
	// server list (top 60 %), rules and players side by side below
	const float Gap = G.L(6.f);
	const float Top = Panel.Y + RowH + Gap;
	const float Avail = Panel.B() - Top;
	const FRect ListR(Panel.X, Top, Panel.W, FMath::RoundToFloat(Avail * 0.6f));
	bool bActivated = false;
	DrawServerList(G, ListR, B, bActivated);
	const FServer* Sel = SelectedServer(B);
	B.WantDetails(Sel ? RowServers[SelectedRow] : INDEX_NONE);
	const float DY = ListR.B() + Gap;
	const float DH = Panel.B() - DY;
	DrawDetails(G, FRect(Panel.X, DY, Panel.W * 0.5f - Gap * 0.5f, DH), FRect(Panel.X + Panel.W * 0.5f + Gap * 0.5f, DY, Panel.W * 0.5f - Gap * 0.5f, DH), Sel);
	// joining a server that needs package downloads: their progress over the list (Net/UE2NetDownload.h)
	DrawJoinProgress(G, FRect(ListR.X, ListR.Y + ListR.H * 0.3f, ListR.W, G.L(124.f)));

	// footer: BACK, OPEN IP, ADD / REMOVE FAVORITE, REFRESH, JOIN (UT2k4Browser_Footer)
	const bool bFavTab = Tab == 0;
	const FString SelAddress = Sel ? (Sel->Host.IsEmpty() ? Sel->Address() : FString::Printf(TEXT("%s:%d"), *Sel->Host, Sel->Port)) : FString();
	const bool bSelFavourite = Sel && (IsFavourite(SelAddress) || IsFavourite(Sel->Address()));
	const TArray<FString> Btns = {
		LocBrowser(TEXT("UT2k4Browser_Footer"), TEXT("BrowserBack.Caption"), TEXT("BACK")),
		TEXT("OPEN IP"),
		(bFavTab || bSelFavourite) ? LocBrowser(TEXT("UT2k4Browser_ServerListPageFavorites"), TEXT("RemoveFavoriteCaption"), TEXT("REMOVE FAVORITE"))
			: LocBrowser(TEXT("UT2k4Browser_ServerListPageFavorites"), TEXT("AddFavoriteCaption"), TEXT("ADD FAVORITE")),
		LocBrowser(TEXT("UT2k4Browser_Footer"), TEXT("BrowserRefresh.Caption"), TEXT("REFRESH")),
		LocBrowser(TEXT("UT2k4Browser_Footer"), TEXT("BrowserJoin.Caption"), TEXT("JOIN")) };
	const TArray<FString> BtnHints = {
		LocBrowser(TEXT("UT2k4Browser_Footer"), TEXT("BrowserBack.Hint"), TEXT("Return to the previous menu")),
		TEXT("Connect to a server by its address"),
		(bFavTab || bSelFavourite) ? FString(TEXT("Remove the selected server from the favourites")) : FString(TEXT("Add the selected server to the favourites")),
		TEXT("Query the servers again"),
		TEXT("Join the selected server") };
	const int32 Clicked = bModal ? INDEX_NONE : FooterButtons(G, Frame.Footer, Btns, BtnHints, { true, true, Sel != nullptr, true, Sel != nullptr && !Sel->Ip.IsEmpty() });

	if (bOpenIP)
	{
		DrawOpenIP(G);
		return false;
	}
	if (!Message.IsEmpty())
	{
		DrawMessage(G);
		return false;
	}
	if (bPasswordBox)
	{
		DrawPassword(G);
		return false;
	}
	if (bStatsPrompt)
	{
		DrawStatsPrompt(G);
		return false;
	}
	if (!AntiCheatPrompt.IsEmpty())
	{
		DrawAntiCheatPrompt(G);
		return false;
	}
	if (Clicked == 1)
	{
		bOpenIP = true;
		G.ResetInteraction();
		G.SetFocus(0);
	}
	else if (Clicked == 2 && Sel)
	{
		if (bFavTab || bSelFavourite)
		{
			RemoveFavourite(IsFavourite(SelAddress) ? SelAddress : Sel->Address());
		}
		else
		{
			AddFavourite(SelAddress);
		}
	}
	else if (Clicked == 3)
	{
		Refresh(Tab);
	}
	else if ((Clicked == 4 || bActivated) && Sel && !Sel->Ip.IsEmpty())
	{
		JoinOrAsk(*Sel);
	}
	else if (Clicked == 0 || G.BackPressed())
	{
		return true;
	}
	return false;
}

// =================================================================================================
// Test hooks
// =================================================================================================

void FArenaServerBrowserPage::ApplyTestCommand(const FString& Name, const FString& Value)
{
	if (Name == TEXT("BrowserTab"))
	{
		const TArray<FTabDef>& Defs = Tabs();
		int32 NewTab = Value.IsNumeric() ? FCString::Atoi(*Value) : Defs.IndexOfByPredicate([&Value](const FTabDef& T) { return Value.Equals(T.Id, ESearchCase::IgnoreCase); });
		if (Defs.IsValidIndex(NewTab))
		{
			Tab = NewTab;
			SelectedRow = -1;
			SelectedAddress.Reset();
			RowsRevision = 0;
			if (!Fetched[Tab])
			{
				Refresh(Tab);
			}
		}
	}
	else if (Name == TEXT("BrowserRefresh"))
	{
		Refresh(Tab);
	}
	else if (Name == TEXT("BrowserSort"))
	{
		static const TCHAR* Names[] = { TEXT("Name"), TEXT("Map"), TEXT("Players"), TEXT("Ping") };
		for (int32 i = 0; i < 4; ++i)
		{
			if (Value.Equals(Names[i], ESearchCase::IgnoreCase))
			{
				Sort = ESort(i);
				RowsRevision = 0;
			}
		}
	}
	else if (Name == TEXT("BrowserSelect"))
	{
		FBrowser& B = BrowserFor(Tab);
		RebuildRows(B);
		if (Value.IsNumeric())
		{
			const int32 Row = FCString::Atoi(*Value);
			if (RowServers.IsValidIndex(Row))
			{
				SelectedRow = Row;
				SelectedAddress = B.GetServers()[RowServers[Row]].Address();
			}
		}
		else
		{
			SelectedAddress = Value;
			RebuildRows(B);
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("Server browser test: select %s -> row %d of %d (first: %s)"), *Value, SelectedRow, Rows.Num(), RowServers.Num() > 0 ? *B.GetServers()[RowServers[0]].Address() : TEXT("-"));
	}
	else if (Name == TEXT("BrowserOpenIP"))
	{
		bOpenIP = true;
		OpenIPText = Value;
	}
	else if (Name == TEXT("BrowserAddFavourite"))
	{
		AddFavourite(Value);
	}
	else if (Name == TEXT("BrowserRemoveFavourite"))
	{
		RemoveFavourite(Value);
	}
	else if (Name == TEXT("BrowserJoin"))
	{
		Join(Value);
	}
	else if (Name == TEXT("BrowserJoinSelected"))
	{
		// as the JOIN button: the selected server, asking first when it runs an anti-cheat we can only spectate on
		for (const ArenaServerBrowser::FServer& S : BrowserFor(Tab).GetServers())
		{
			if (S.Address() == SelectedAddress)
			{
				JoinOrAsk(S);
				break;
			}
		}
	}
}
