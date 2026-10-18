#pragma once

#include "CoreMinimal.h"
#include "ArenaGUI2K4.h"

/**
 * GUI2K4 pages shared by the main menu and the in-game menu (ArenaGUI2K4.h): the tabbed page frame
 * (UT2k4MainPage: header, GUITabControl, ButtonFooter), the Settings page (UT2K4SettingsPage) with our settings mapped
 * into its tabs, the data of the Instant Action tabs (game types, map previews, mutators) and the automation runner of
 * the menu test hooks.
 */
namespace ArenaGUI2K4Pages
{
	using ArenaGUI2K4::FGUI;
	using ArenaGUI2K4::FRect;

	// ---- localized arrays (PanelCaption=("A","B",...)) ----
	/** Element Index of a localized array value, or Fallback. */
	OPENUT2K4_API FString LocArray(const TCHAR* Package, const TCHAR* Section, const TCHAR* Key, int32 Index, const FString& Fallback);

	// ---- tabbed page frame ----
	struct FTabbedFrame
	{
		/** Client area of the tab panels (between the tab row and the footer, inside the 4:3 page). */
		FRect Panel;
		FRect Footer;
		bool bTabChanged = false;
	};
	/**
	 * UT2k4MainPage chrome: page background, GUIHeader ("PageCaption | Tab"), the tab row (PageTabs: WinLeft 0.01,
	 * WinWidth 0.98, TabHeight 0.04, docked under the header) and the footer bar with FooterHint (the active tab's hint
	 * for a few seconds after a tab change, like GUITabControl.MyFooter). Draws the background first.
	 */
	OPENUT2K4_API FTabbedFrame TabbedPage(FGUI& G, uint32 Id, const FString& PageCaption, const TArray<FString>& Tabs, const TArray<FString>& Hints,
		int32& Active, double& TabChangedAt);
	/** ButtonFooter buttons (STY2FooterButton, bFixedWidth, right aligned). Returns the index clicked or INDEX_NONE. */
	OPENUT2K4_API int32 FooterButtons(FGUI& G, const FRect& Footer, const TArray<FString>& Captions, const TArray<FString>& Hints,
		const TArray<bool>& Enabled);

	// ---- Settings ----
	struct FSettingsPage
	{
		int32 Tab = 0;
		double TabChangedAt = 0.0;
		/** Draws the page; returns true when the player chose BACK / Esc (settings are saved). */
		OPENUT2K4_API bool Draw(FGUI& G);
	};

	// ---- Instant Action data ----
	struct FGameTypeInfo
	{
		FString ModeId;
		FString Name;
		FString Description;
		/** UT2004Thumbnails shot material (MaterialSequence) */
		FString Screenshot;
	};
	/** Game types in UT2K4Tab_GameTypeBase.GamePos order, names / descriptions localized from the install's cache records. */
	OPENUT2K4_API const TArray<FGameTypeInfo>& GetGameTypes();
	OPENUT2K4_API const FGameTypeInfo* FindGameType(const FString& ModeId);
	/** The map's LevelSummary screenshot material ("DM-Rankin.Screenshot"), empty when unknown. Cached. */
	OPENUT2K4_API FString GetMapScreenshot(const FString& MapName);

	struct FMutatorInfo
	{
		/** "Package.Class" as ?Mutator= takes it */
		FString ClassName;
		FString FriendlyName;
		FString Description;
		FString GroupName;
		bool bCanRun = true;
		FString Reason;
	};
	/**
	 * Mutators of the player's packages: the install's cache records (the "Mutator=" entries of the System .ucl files, like UT2004's
	 * CacheManager) plus Object=(...MetaClass=Engine.Mutator...) entries of .int files in UserContent. Names and
	 * descriptions are localized (Package.int [Class] FriendlyName / Description) with the records' fallbacks.
	 * bCanRun / Reason come from the script runtime (ArenaUScript::CanRun), checked on the first call.
	 */
	OPENUT2K4_API const TArray<FMutatorInfo>& GetMutators();

	// ---- automation (non-shipping menu test hooks) ----
	/**
	 * -AutoMenuScript="<sec>:<cmd>;<sec>:<cmd>..." runs GUI commands at those real-time seconds (since the first menu
	 * frame): Key=<UE key name> (e.g. Down, Enter, Escape, Gamepad_RightShoulder), Click=<x>,<y> (0..1 of the 4:3 page),
	 * Wheel=<n>, Shot=<name> (Saved/Screenshots/menu_<name>.png), Quit, and page commands handled by the caller
	 * (Page=<name>, Tab=<n>, any other Name=Value). Injected input goes straight into the GUI's queue; the real desktop
	 * is never touched.
	 */
	struct OPENUT2K4_API FAutomation
	{
		/** The script runs once per process: true for the first automation that asks (the menu or the match it started in). */
		bool Claim() const;
		/** Runs the commands due at Now; returns the caller commands (Name, Value) to apply. */
		TArray<TPair<FString, FString>> Tick(FGUI& G, double Now, UWorld* World);
		bool bParsed = false;
		double Start = -1.0;
		struct FCmd { float At = 0.f; FString Name; FString Value; bool bDone = false; };
		TArray<FCmd> Cmds;
	};
}
