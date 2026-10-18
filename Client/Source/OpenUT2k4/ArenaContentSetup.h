#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "ArenaGUI2K4.h"
#include "ArenaContentSetup.generated.h"

struct FArenaDraw;
struct FKeyEvent;
class SArenaSetupTextSink;
class SWidget;

/**
 * First-run "UT2004 Content" screen, drawn like the main menu (UT2004 menu backdrop, framed window, header bar and
 * buttons when the player's converted art is available; the menu's procedural look before that). Shown on the main
 * menu when the game uses runtime content (ArenaContent::UseRuntimeContent()) and the content cache is not ready,
 * and from the main menu's "UT2004 Content" entry (AArenaMenuHUD::OnOpenContentSetup, bound by this file).
 *
 * States (left: friendly text, right: buttons, header chip): no install found / install found / converting (progress,
 * cancel) / done / failed / paused (cancelled) / content ready. Technical details (searched folders, cache folder,
 * converter problems, log file) stay behind "Show details".
 *
 * Choosing the install: detected automatically (ArenaContent::FindUT2004Install), "Browse..." (native folder
 * dialog, ArenaFolderPicker: xdg-desktop-portal / zenity / kdialog on Linux, IFileOpenDialog on Windows,
 * NSOpenPanel on Mac) or "Type a path..." (text field with Tab completion and paste, for systems without a dialog).
 * A chosen folder is checked with CheckInstallFolder, which corrects the usual near misses (the System folder or
 * another subfolder of the install, the folder above it), saved in UArenaUserSettings::UT2004Dir, and the screen
 * offers to convert it. Converting uses FArenaConverter; when done it refreshes the map list
 * (AArenaLevelBuilder::RefreshImportedLayouts) and "Continue" returns to the main menu.
 *
 * Works by swapping the menu player controller's HUD class (AArenaMenuHUD <-> AArenaContentSetupHUD); drawn in the
 * GUI2K4 look (ArenaGUI2K4.h: page background, header, section frames, round buttons, footer) with the same input as
 * the menus (mouse, Up / Down / Enter / Escape and gamepad equivalents); while the path field is edited an invisible
 * Slate widget in the viewport holds keyboard focus and forwards typed characters.
 *
 * -AutoConvert: opens the screen when needed, starts converting right away (honours -ConvertMaps=A,B for
 * quick tests) and continues to the main menu when done (headless tests). More test hooks: README "Content setup".
 */

/**
 * Extension block of the setup screen (e.g. "Download from OldUnreal"): an optional info block drawn in the text
 * column and an optional button. Register at module startup with AArenaContentSetupHUD::RegisterExtension.
 */
struct FArenaContentSetupExtension
{
	/** Unique id (also the button's hit box name suffix). */
	FName Id;
	/** Button label; empty = no button. Evaluated every frame. */
	TFunction<FString()> GetLabel;
	/** Button enabled state (default: enabled). */
	TFunction<bool()> IsEnabled;
	/** Called when the button is chosen. */
	TFunction<void(class AArenaContentSetupHUD* Screen)> OnActivate;
	/**
	 * Draws an info block at (X, Y), at most Width wide, and advances Y; may draw nothing. Called while no conversion
	 * runs. Use AArenaContentSetupHUD::DrawParagraph and ArenaSetupStyle for the screen's look.
	 */
	TFunction<void(const FArenaDraw& D, float X, float& Y, float Width)> DrawInfo;
	/** True while the extension is busy (its own progress replaces the convert buttons' enabled state). */
	TFunction<bool()> IsBusy;
	/**
	 * True while the extension owns the screen (e.g. a notice the player must answer, a download in progress): only
	 * its block, the buttons of extensions in the same group and "Back" are shown. Optional.
	 */
	TFunction<bool()> IsModal;
	/** Header chip text while modal (e.g. "DOWNLOADING"). Optional. */
	TFunction<FString()> GetModalStatus;
	/** Extensions with the same group show their buttons together while one of them is modal (e.g. "I agree" / "Do not agree"). */
	FName Group;
};

/**
 * Colours of the setup screen for extension blocks: the GUI2K4 palette (STY2TextLabel white / gold, the menus' blue).
 */
namespace ArenaSetupStyle
{
	inline FLinearColor White()  { return FLinearColor(FColor(255, 255, 255)); }
	inline FLinearColor Dim()    { return FLinearColor(FColor(190, 205, 235)); }
	inline FLinearColor Accent() { return FLinearColor(FColor(255, 210, 0)); }
	inline FLinearColor Hot()    { return FLinearColor(FColor(255, 150, 40)); }
	inline FLinearColor Good()   { return FLinearColor(FColor(120, 230, 110)); }
	inline FLinearColor Bad()    { return FLinearColor(FColor(255, 90, 80)); }
}

UCLASS()
class OPENUT2K4_API AArenaContentSetupHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void DrawHUD() override;
	virtual void NotifyHitBoxClick(FName BoxName) override;
	virtual void NotifyHitBoxBeginCursorOver(FName BoxName) override;

	/** Switches the owner of Menu (or the first local player) to this screen. */
	static void Open(class APlayerController* PC);
	/** True when the game needs a conversion before anything can be played. */
	static bool IsSetupNeeded();

	/** Adds a block / button to the screen (see FArenaContentSetupExtension). Game thread. */
	static void RegisterExtension(const FArenaContentSetupExtension& Extension);
	/**
	 * Uses Dir as the UT2004 install (saved in UArenaUserSettings::UT2004Dir) and starts converting it, e.g. after
	 * an extension provided an install. Returns false if Dir is not a UT2004 install or a conversion runs.
	 */
	bool ContinueWithInstall(const FString& Dir);

	/** Result of CheckInstallFolder. */
	struct FInstallCheck
	{
		/** Install is a UT2004 install (maybe corrected from the folder the player chose). */
		bool bOk = false;
		/** True when Install differs from the chosen folder (a subfolder / the parent was used instead). */
		bool bCorrected = false;
		/** The folder that was checked, cleaned up (quotes, file:// and ~ resolved). */
		FString Chosen;
		FString Install;
		/** Plain-language explanation for the player (what was found, or what is wrong and what to choose). */
		FString Message;
	};
	/**
	 * Checks a folder the player chose or typed (quotes, file:// URLs and ~ are accepted). Corrects near misses:
	 * a subfolder of the install (System, Maps...), the folder above the install (one or two levels).
	 */
	static FInstallCheck CheckInstallFolder(const FString& Chosen);

	/** Word-wrapped text in the screen's style; returns the Y below it. Long words (paths) break anywhere. */
	static float DrawParagraph(const FArenaDraw& D, const FString& Text, float X, float Y, float Width, const FLinearColor& Color,
		float SizeRef, float GapRef = 6.f);

	/** Progress bar in the screen's style (GUIProgressBar look: NewStatusBar / NewStatusFill; flat before content exists). */
	static void DrawProgressBar(const FArenaDraw& D, float X, float Y, float Width, float Height, float Fraction);

	// ---- path field (called by the focus widget while editing) ----
	bool IsEditingPath() const { return bEditingPath; }
	void OnPathKey(const FKeyEvent& Event);
	void OnPathChar(TCHAR Char);

private:
	enum class EView : uint8
	{
		NoInstall,
		InstallFound,
		Ready,
		Converting,
		Done,
		Failed,
		Paused,
		Modal
	};
	enum class EFeedback : uint8
	{
		None,
		Good,
		Warn,
		Bad
	};
	struct FButton
	{
		FName Id;
		FString Label;
		bool bEnabled = true;
	};

	EView GetView() const;
	const FArenaContentSetupExtension* GetModalExtension() const;
	void BuildButtons(TArray<FButton>& Out) const;
	void AddExtensionButtons(TArray<FButton>& Out, FName OnlyGroup = NAME_None) const;
	void SelectButton(FName Id);
	void Activate(FName Id);
	void StartConversion(bool bForce, const FString& Dir = FString());
	void Close();
	void HandleKeys(int32 NumButtons);
	void DrawBody(const FArenaDraw& D, EView View, float X, float Y, float Width, float Bottom);
	void DrawPathField(const FArenaDraw& D, float X, float& Y, float Width);
	void DrawButtons(const FArenaDraw& D, const TArray<FButton>& Buttons, float X, float Y, float Width, float Time);

	// ---- choosing the install ----
	void Browse();
	void BeginEditPath();
	void EndEditPath(bool bAccept);
	/** Applies a folder from the dialog / the path field: checks it, saves it, offers to convert. */
	void UseChosenFolder(const FString& Chosen, const TCHAR* Source);
	void SetFeedback(EFeedback Kind, const FString& Text, const FString& Detail = FString());
	void CompletePath();
	/** Gives the path field keyboard focus (adds the focus widget to the viewport) or returns it to the game view. */
	void FocusTextSink(bool bFocus);

	FString InstallDir;
	TArray<FString> Searched;
	int32 Selection = 0;
	bool bWasRunning = false;
	bool bShowDone = false;
	bool bAutoStarted = false;
	bool bShowDetails = false;
	double FinishedAt = 0.0;

	EFeedback FeedbackKind = EFeedback::None;
	FString FeedbackText;
	FString FeedbackDetail;
	/** The folder dialog could not be shown on this system (Browse then opens the path field). */
	bool bNoDialog = false;

	bool bEditingPath = false;
	FString PathText;
	int32 PathCursor = 0;
	double PathEditStart = 0.0;
	TSharedPtr<SArenaSetupTextSink> TextSink;
	TSharedPtr<SWidget> TextSinkHolder;

	/** GUI2K4 widgets (ArenaGUI2K4.h): buttons, focus, input */
	TUniquePtr<ArenaGUI2K4::FGUI> GUI;
	/** button focus follows Selection when the code picks a button (SelectButton / state changes) */
	int32 FocusedSelection = -1;
};
