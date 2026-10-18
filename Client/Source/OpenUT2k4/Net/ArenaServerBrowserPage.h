#pragma once

#include "CoreMinimal.h"
#include "../ArenaGUI2K4.h"
#include "ArenaServerBrowser.h"

/**
 * The Server Browser page (UT2K4ServerBrowser look) of the front end menus (FArenaMenuScreens, opened by "Join Game"):
 * tabs Favourites, LAN and one per game type (Internet servers of the master server list filtered by game type, plus
 * All), the server list (Server Name, Map, Players, Ping; click a heading to sort), the Rules and Players panes of the
 * selected server, the empty / full filters, and the footer: BACK, OPEN IP, ADD / REMOVE FAVOURITE, REFRESH, JOIN.
 * JOIN (double click / Enter) calls ArenaNet::JoinServer (Net/UE2NetJoin.h); without that client a message says so.
 * Favourites and filters persist in UArenaUserSettings. Each tab keeps its own list until refreshed; a tab is fetched
 * the first time it is opened.
 */
class OPENUT2K4_API FArenaServerBrowserPage
{
public:
	FArenaServerBrowserPage();
	~FArenaServerBrowserPage();

	/** Draws and runs the page for one frame; true when the player chose BACK / Esc. */
	bool Draw(ArenaGUI2K4::FGUI& G);
	/** The page was opened (from the main menu). */
	void OnOpened();

	/**
	 * Test hooks (menu automation, -AutoMenuScript commands and -AutoBrowser* options): BrowserTab=<index or caption
	 * id: Favourites, LAN, DM, TDM, CTF, BR, DOM, ONS, AS, INV, LMS, All>, BrowserSelect=<row index or address>,
	 * BrowserSort=<Name|Map|Players|Ping>, BrowserOpenIP=<address> (opens the Open IP box with it), BrowserRefresh=1,
	 * BrowserAddFavourite=<address>, BrowserRemoveFavourite=<address>, BrowserJoin=<address> (calls the join hook).
	 */
	void ApplyTestCommand(const FString& Name, const FString& Value);

	/** The "Joining is not available" message (and its address) of the last JOIN. */
	static FString JoinUnavailableText();

private:
	struct FTabDef
	{
		const TCHAR* Id;
		const TCHAR* Caption;
		const TCHAR* Hint;
		/** UE2 game class name for the master server filter ("" = all game types) */
		const TCHAR* GameType;
	};
	enum class ESort : uint8 { Name, Map, Players, Ping };

	static const TArray<FTabDef>& Tabs();
	ArenaServerBrowser::FBrowser& BrowserFor(int32 TabIndex);
	void Refresh(int32 TabIndex);
	void RebuildRows(const ArenaServerBrowser::FBrowser& B);
	void DrawServerList(ArenaGUI2K4::FGUI& G, const ArenaGUI2K4::FRect& R, ArenaServerBrowser::FBrowser& B, bool& bOutActivated);
	void DrawDetails(ArenaGUI2K4::FGUI& G, const ArenaGUI2K4::FRect& RulesR, const ArenaGUI2K4::FRect& PlayersR, const ArenaServerBrowser::FServer* S);
	void DrawOpenIP(ArenaGUI2K4::FGUI& G);
	void DrawMessage(ArenaGUI2K4::FGUI& G);
	/** UT2K4GetPassword: the server rejected the join with NEEDPW / WRONGPW. */
	void DrawPassword(ArenaGUI2K4::FGUI& G);
	/** UT2K4StatsPrompt: the server rejected the join with NEEDSTATS. */
	void DrawStatsPrompt(ArenaGUI2K4::FGUI& G);
	/** After a join started: when the client gives up, show the right dialog for its fail code. */
	void WatchJoin();
	void Join(const FString& Address);
	void AddFavourite(const FString& Address);
	void RemoveFavourite(const FString& Address);
	bool IsFavourite(const FString& Address) const;
	const ArenaServerBrowser::FServer* SelectedServer(const ArenaServerBrowser::FBrowser& B) const;

	int32 Tab = 2;
	double TabChangedAt = 0.0;
	TArray<TUniquePtr<ArenaServerBrowser::FBrowser>> Browsers;
	TArray<bool> Fetched;

	// server list view (rebuilt when the list, sorting or filters change)
	TArray<int32> RowServers;
	TArray<ArenaGUI2K4::FListItem> Rows;
	uint32 RowsRevision = 0;
	int32 RowsTab = -1;
	ESort Sort = ESort::Ping;
	bool bSortDescending = false;
	bool bRowsHideEmpty = false;
	bool bRowsHideFull = false;
	int32 SelectedRow = -1;
	/** The selected server's address (kept across re-sorting / refreshes) */
	FString SelectedAddress;

	int32 RulesSel = -1;
	int32 PlayersSel = -1;

	// Open IP box (UT2K4Browser_OpenIP)
	bool bOpenIP = false;
	FString OpenIPText;
	// message box (join result)
	FString Message;
	/** The address of the join in progress (without options), watched for a rejection */
	FString JoiningAddress;
	bool bWatchJoin = false;
	bool bPasswordBox = false;
	bool bWrongPassword = false;
	FString PasswordText;
	bool bStatsPrompt = false;
	FString StatusOverride;
	double StatusOverrideAt = -1.0;
};
