#pragma once

#include "CoreMinimal.h"
#include "ArenaGUI2K4.h"
#include "ArenaSPProfile.h"

class AHUD;

/**
 * UT2004's single player tournament (GUI2K4 UT2K4SP_Main and its UT2K4SPTab_* tabs and UT2K4SP_* dialogs) in the
 * GUI2K4 look of ArenaGUI2K4.h: profiles, qualification, team qualification with the draft, the tournament ladder,
 * the championship, the additional (Onslaught) ladder, team management, team roles, injuries, arena changes,
 * challenges and the match details. Rules and data: ArenaSPProfile.h / ArenaSPData.h (read from the player's own
 * UT2004 packages at runtime). Matches start through the normal Instant Action launch path (?Mode=...?Layout=...)
 * with the ladder's bots, teams and goals; when the match ends its result is registered and the menu reopens here.
 *
 * Test hooks (non-shipping): see README "Single player tournament".
 */
class OPENUT2K4_API FArenaSinglePlayerPage
{
public:
	explicit FArenaSinglePlayerPage(AHUD* InOwner);
	~FArenaSinglePlayerPage();

	/** Called when the main menu opens the page. */
	void OnOpened();
	/** Draws the whole page (background, header, tabs, footer, dialogs). True = BACK to the main menu. */
	bool Draw(ArenaGUI2K4::FGUI& G);
	/** -AutoMenuScript commands starting with "SP" (see README). */
	void ApplyTestCommand(const FString& Name, const FString& Value);
	/** True after a single player match returned to the menu (the menu then opens this page directly). */
	static bool ShouldOpenOnMenuStart();

	// ---- implementation ----
	enum class ETab : uint8 { Profile, NewProfile, Tutorials, Qualification, TeamQualification, Ladder, TeamManagement, Additional };
	enum class EDialog : uint8 { None, Question, Message, TeamRoles, Injury, MapChallenge, Details, TeamDetails, Challenge, HighScores, Trade, UnTrade };
	enum class EQuestion : uint8 { None, DeleteProfile, LoadOverProfile, ForfeitChallenge };

	struct FDialog
	{
		EDialog Kind = EDialog::None;
		FString Text;
		FString Param1;
		FString Param2;
		int32 Index = 0;
		EQuestion Question = EQuestion::None;
		bool bYesNo = false;
		double OpenedAt = 0.0;
		bool bSoundPlayed = false;
		// team roles: the role per team slot (index into the positions, -1 inactive)
		TArray<int32> Roles;
		// challenges
		FString ChallengeClass;
		int32 TeamIndex = 0;
		TArray<FString> Teams;
		TArray<FString> Maps;
		int32 MapSel = -1;
		int32 Bet = 0;
		int32 MinBet = 0;
		int32 MaxBet = 0;
		int32 CancelPenalty = 0;
		int32 CharIndex = 0;
		bool bInstagib = false;
		bool bChallenged = false;
		int32 Tab = 0;
		double TabChangedAt = 0.0;
	};

private:
	friend struct FArenaSPDraw;
	void RebuildTabs();
	void SetActiveTab(ETab T);
	ETab ActiveTab() const;
	void LoadProfile(const FString& Name);
	void SaveProfile();
	void ProcessProfileEvents();
	void ProcessMenuRequests(const TArray<ArenaSP::FMenuRequest>& Requests);
	void SelectMatch(int32 Ladder, int32 Rung);
	void SelectNextMatch(int32 Ladder);
	void PushDialog(const FDialog& D);
	void PushQuestion(const FString& Text, EQuestion Q = EQuestion::None, bool bYesNo = false);
	void OnQuestionAnswer(EQuestion Q, bool bYes);
	/** PLAY on a ladder: requirements, team roles, then StartMatch. */
	void PlaySelectedMatch();
	void StartMatch();
	bool CreateProfile();

	void DrawProfileTab(const ArenaGUI2K4::FRect& P);
	void DrawNewProfileTab(const ArenaGUI2K4::FRect& P);
	void DrawTutorialsTab(const ArenaGUI2K4::FRect& P);
	void DrawSingleLadderTab(const ArenaGUI2K4::FRect& P, int32 Ladder);
	void DrawLadderTab(const ArenaGUI2K4::FRect& P);
	void DrawTeamTab(const ArenaGUI2K4::FRect& P);
	void DrawAdditionalTab(const ArenaGUI2K4::FRect& P);
	/** UT2K4SPTab_LadderBase match panel (selected match, details, balance, arena change). */
	void DrawMatchInfo(const ArenaGUI2K4::FRect& P, bool bLadderLayout);
	/** A ladder button (UT2K4LadderVButton / HButton); true when clicked. */
	bool DrawLadderButton(const ArenaGUI2K4::FRect& R, int32 Ladder, int32 Rung, uint32 Id);

	/** Draws the top dialog; returns false when none is open. */
	bool DrawDialog();
	void DrawQuestion(FDialog& D);
	void DrawMessage(FDialog& D);
	void DrawTeamRoles(FDialog& D);
	void DrawInjury(FDialog& D);
	void DrawMapChallenge(FDialog& D);
	void DrawDetails(FDialog& D);
	void DrawTeamDetails(FDialog& D);
	void DrawChallenge(FDialog& D);
	void DrawHighScores(FDialog& D);
	void DrawTrade(FDialog& D);
	void CloseDialog();
	/** A LargeWindow frame (FloatingWindow: title bar, Display1 background); returns the window rect. */
	ArenaGUI2K4::FRect DialogFrame(float L, float T, float W, float H, const FString& Title);

	TWeakObjectPtr<AHUD> Owner;
	ArenaGUI2K4::FGUI* G = nullptr;
	TSharedPtr<ArenaSP::FProfile> GP;
	TArray<ETab> Tabs;
	int32 TabIndex = 0;
	double TabChangedAt = 0.0;
	/** Profile tab replaced by New Profile; Qualification by the team draft. */
	bool bCreatingProfile = false;
	bool bInitialDraft = false;
	TArray<FString> NewTeam;
	TArray<FDialog> Dialogs;
	TArray<FString> PendingNotes;

	// selection
	int32 SelLadder = -1;
	int32 SelRung = -1;
	int32 ShownLadder = -1;
	int32 AdditionalIndex = 0;
	double LadderShownAt = 0.0;
	int32 ProfileSel = 0;
	TArray<FString> ProfileNames;

	// new profile
	FString NewName;
	FString NewTeamName;
	int32 NewCharIndex = 0;
	int32 NewSymbolIndex = 0;
	int32 NewDifficulty = 2;
	TArray<FString> SPCharacters;

	// team management
	int32 FreeAgentSel = -1;
	int32 FreeAgentScroll = 0;
	FString ActiveBot;

	// automation (-SPScript / -SPScriptAfterMatch<n>, see README)
	void TickScript();
	struct FScriptCmd { float At = 0.f; FString Name; FString Value; bool bDone = false; };
	TArray<FScriptCmd> Script;
	double ScriptStart = -1.0;
	bool bScriptPending = false;
};

/** Match session state shared between the menu and the match (process lifetime). */
namespace ArenaSinglePlayer
{
	/** True while a single player match is being played or its result waits for the menu. */
	OPENUT2K4_API bool HasPendingMatch();
}
