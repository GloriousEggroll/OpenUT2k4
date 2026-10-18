#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "ArenaHUD.h"

class UCanvas;
class UFont;
class UTexture2D;
class APlayerController;

/**
 * Canvas reimplementation of UT2004's GUI2K4 look (XInterface / GUI2K4 GUIComponent, GUIStyles and GUIFont), used by
 * the main menu, Instant Action, Settings, the mid-game menu and the content setup screen.
 *
 * Data: every style (STY2*), font (fntUT2k4*) and layout value is a table entry in ArenaGUI2K4.cpp / the page files,
 * commented with the class default it was read from (the player's own GUI2K4.u / XInterface.u, read once while this
 * was written; nothing of UT2004 is shipped). Textures load at runtime from the player's converted content
 * (2K4Menus.utx ...); without them every element falls back to flat procedural drawing in the same layout, so the
 * first-run setup screen works before any content exists. Captions come from the install's System .int files at runtime
 * (Loc()) with our own fallback wording.
 *
 * Layout: UE2 pages use relative coordinates (WinLeft/WinTop/WinWidth/WinHeight) of a 4:3 screen. Pages are laid out
 * in a centred 4:3 "safe area" sized by the screen height (FGUI::Page()); backgrounds, header and footer bars extend
 * to the full width. Values > 2 are UE2 absolute pixels of the 1024x768 reference and scale with the height (L()).
 * Text: a GUIFont picks its font like GUIFont.GetFont(ResX) for the virtual resolution of the page (the screen height
 * over 720 = the text size factor, at least 1); that font's height times the factor is the size drawn. The closest
 * size of the same face in the content (Verdana 8-34, Eurostile / Neuzeit 8-37) is resampled to exactly that height
 * and drawn 1 texel = 1 pixel with bilinear filtering (ArenaRuntimeContent::LoadFontAtHeight), so text is smooth at
 * any resolution; up to 720 lines it is the stock font 1:1, like UT2004.
 *
 * Input: an input pre-processor queues mouse buttons, wheel and keys while a GUI is active (consumed, so the game does
 * not see them); the mouse position comes from the player controller. Widgets are immediate mode with retained
 * focus / hover / pressed state (UE2 menu states Blurry, Watched, Focused, Pressed, Disabled). Hit areas are the
 * drawn rectangles. Keyboard / gamepad: Up/Down (or Tab) move the focus, Left/Right change the focused option or tab,
 * Enter / A activate, Esc / B go back, shoulder buttons / PageUp/PageDown switch tabs, mouse wheel scrolls lists.
 */
namespace ArenaGUI2K4
{
	/** XInterface.GUI eMenuState */
	enum class EMenuState : uint8 { Blurry, Watched, Focused, Pressed, Disabled };
	/** XInterface.GUI eImgStyle */
	enum class EImgStyle : uint8 { Normal, Stretched, Scaled, Bound, Justified, PartialScaled, Tiled };
	/** XInterface.GUI eFontScale (index into GUIStyles.FontNames blocks of 5) */
	enum class EFontScale : uint8 { Small, Medium, Large };
	enum class EAlign : uint8 { Left, Center, Right };
	/** XInterface.GUIComponent OnClickSound (UT2K4GUIController sounds) */
	enum class ESound : uint8 { None, Click, Edit, Up, Down, Drag, Fade, Over };

	/** A GUIStyles subclass (STY2*): per menu state images, colours and fonts. */
	struct FStyle
	{
		FString KeyName;
		/** UE2 object paths, e.g. "2K4Menus.NewControls.GradientButtonBlurry"; empty = no image */
		FString Images[5];
		EImgStyle ImgStyle[5];
		/** [0..4] small, [5..9] medium, [10..14] large, per menu state */
		FString FontNames[15];
		FColor FontColors[5];
		FColor FontBKColors[5];
		FColor ImgColors[5];
		/** EMenuRenderStyle: 1 normal, 3 translucent (additive), 5 alpha */
		uint8 RStyles[5];
		float ImgWidths[5];
		float ImgHeights[5];
		/** left, top, right, bottom padding of the caption, UE2 pixels */
		int32 BorderOffsets[4];

		const FString& Font(EFontScale Scale, EMenuState State) const { return FontNames[int32(Scale) * 5 + int32(State)]; }
	};
	/** The style registered under KeyName (UT2K4GUIController.DefaultStyleNames); unknown names give "SquareButton". */
	OPENUT2K4_API const FStyle& GetStyle(const FString& KeyName);

	/** A screen rectangle in pixels. */
	struct FRect
	{
		float X = 0.f, Y = 0.f, W = 0.f, H = 0.f;
		FRect() = default;
		FRect(float InX, float InY, float InW, float InH) : X(InX), Y(InY), W(InW), H(InH) {}
		float R() const { return X + W; }
		float B() const { return Y + H; }
		bool Contains(const FVector2D& P) const { return P.X >= X && P.X < X + W && P.Y >= Y && P.Y < Y + H; }
		FRect Inset(float L, float T, float Rt, float Bt) const { return FRect(X + L, Y + T, FMath::Max(0.f, W - L - Rt), FMath::Max(0.f, H - T - Bt)); }
		/** UE2 bound + scaled child: relative values inside this rectangle. */
		FRect Sub(float RL, float RT, float RW, float RH) const { return FRect(X + RL * W, Y + RT * H, RW * W, RH * H); }
	};

	/** A resolved bitmap font at the current resolution. */
	struct FFontRef
	{
		UFont* Font = nullptr;
		/** Draw scale: 1 (the font is resampled to the size it is drawn at, ArenaRuntimeContent::LoadFontAtHeight) */
		float Scale = 1.f;
		/** Line height in pixels (cell height * Scale); also the size of the engine-font fallback */
		float Height = 16.f;
		/** Size of the text relative to UE2's (1 = the stock font 1:1): shadow offsets and other pixel details scale with it */
		float Px = 1.f;
	};

	/** A string of the install's System/<Package>.int ([Section] Key=...), or Fallback (our own wording). */
	OPENUT2K4_API FString Loc(const TCHAR* Package, const TCHAR* Section, const TCHAR* Key, const FString& Fallback);

	/** "Pkg.Group.Name" -> the converted texture (cached; null when missing). */
	OPENUT2K4_API UTexture2D* LoadTex(const FString& ObjectPath);

	/**
	 * A material of the player's content resolved to the textures it shows over time: MaterialSequence items (with
	 * cross fades), Shader/FinalBlend/Combiner/TexModifier chains followed to their base texture. Used for the game type
	 * and map preview shots and the main menu's corner view.
	 */
	struct OPENUT2K4_API FAnimatedImage
	{
		struct FItem { FString Texture; float Time = 1.f; bool bFade = false; };
		TArray<FItem> Items;
		float TotalTime = 0.f;
		bool IsValid() const { return Items.Num() > 0; }
		/** Builds from a material / texture object path (empty on failure). Cached by path. */
		static const FAnimatedImage& Get(const FString& ObjectPath);
		/** Texture(s) to show at Time: A, and B with BlendB (0..1) when cross fading. */
		void Sample(float Time, FString& OutA, FString& OutB, float& OutBlendB) const;
		/**
		 * Like UE2 drawing a MaterialSequence: its clock runs only while it is drawn and starts at its first item, so
		 * a map preview opens on its first shot (CTF-Face3: the asteroid, not the tower interior that follows).
		 * Not drawn for a moment (another map selected): it starts over. Now is the GUI time.
		 */
		void SamplePlaying(double Now, FString& OutA, FString& OutB, float& OutBlendB) const;
	private:
		mutable double PlayStart = -1.0;
		mutable double LastDrawn = -1.0;
	};

	/** A list row: text columns, optional section header (not selectable), disabled look. */
	struct FListItem
	{
		FString Text;
		FString Right;
		bool bSection = false;
		bool bDisabled = false;
		/** Multi-column rows (GUIMultiColumnList): one text per column of List()'s Columns; Text / Right are then unused. */
		TArray<FString> Cells;
		/** Drawn dimmed but still selectable (e.g. a server that did not answer). */
		bool bDim = false;
		/** Small icons ("Pkg.Name" textures) drawn at the start of the first column of a multi-column row. */
		TArray<FString> Icons;
	};

	/** A column of a multi-column List(): width relative to the row, cell alignment. */
	struct FListColumn
	{
		float Width = 1.f;
		EAlign Align = EAlign::Left;
	};

	struct FTabResult
	{
		bool bChanged = false;
	};

	/**
	 * GUI context (one per HUD): frame metrics, input, focus, widgets and drawing. Begin() at the start of DrawHUD,
	 * widgets in tab order, End() at the end.
	 */
	class OPENUT2K4_API FGUI
	{
	public:
		FGUI();
		~FGUI();

		// ---- frame ----
		/** bCaptureInput: consume mouse / keys this frame (menus); false = draw only. */
		void Begin(UCanvas* Canvas, APlayerController* PC, bool bCaptureInput = true);
		void End();
		const FArenaDraw& Draw() const { return D; }
		UCanvas* GetCanvas() const { return D.Canvas; }
		APlayerController* GetPC() const { return PC.Get(); }
		/** Real time in seconds (menu animations). */
		double Time() const { return Now; }
		float DeltaTime() const { return Delta; }

		// ---- metrics ----
		float ScreenW() const { return D.W(); }
		float ScreenH() const { return D.H(); }
		/** The centred 4:3 page area (full height). */
		FRect Page() const { return FRect(SafeX, 0.f, SafeW, D.H()); }
		FRect Screen() const { return FRect(0.f, 0.f, D.W(), D.H()); }
		/** UE2 pixels of the 1024x768 reference to screen pixels. */
		float L(float Ue2Pixels = 1.f) const { return Ue2Pixels * LScale; }
		/** Whole-number scale of the bitmap fonts. */
		int32 TextScale() const { return NScale; }
		/** UE2 component rectangle: values <= 2 are relative to Parent, larger values absolute UE2 pixels. */
		FRect Rel(const FRect& Parent, float Left, float Top, float Width, float Height) const;

		// ---- text ----
		/** GUIFont by key name ("UT2MenuFont", "UT2HeaderFont", ...) for the current resolution. */
		FFontRef Font(const FString& FontKey) const;
		FVector2D TextSize(const FFontRef& F, const FString& Text) const;
		/** Top-left at X, Y (pixel snapped). Returns the width. */
		float Text(const FFontRef& F, const FString& Text, float X, float Y, const FLinearColor& Color, EAlign Align = EAlign::Left,
			bool bShadow = false) const;
		/** Text vertically centred in R, clipped by trimming with "..." when wider than R. */
		void TextIn(const FFontRef& F, const FString& Text, const FRect& R, const FLinearColor& Color, EAlign Align, bool bShadow = false) const;
		/** Word wrapped lines of Text inside Width. */
		TArray<FString> Wrap(const FFontRef& F, const FString& Text, float Width) const;
		/** Style caption: the style's font / colour for State inside R minus BorderOffsets. */
		void StyleText(const FStyle& S, EMenuState State, EFontScale Scale, const FString& Text, const FRect& R, EAlign Align,
			float Alpha = 1.f, bool bShadow = false) const;
		FFontRef StyleFont(const FStyle& S, EMenuState State, EFontScale Scale) const;

		// ---- images ----
		/** A texture drawn into R with a UE2 image style. SubRect = source pixels (X1, Y1, X2, Y2), all <0 = whole. False when missing. */
		bool Image(const FString& ObjectPath, const FRect& R, EImgStyle Style, const FLinearColor& Color, const FBox2f* SubRect = nullptr,
			bool bAdditive = false) const;
		/**
		 * UE2 GUIImage with MSTY_Normal (map / game type previews): A scaled into R, opaque (the texture's alpha is not
		 * used: DM-Injector's DXT3 shots have alpha 0), cross faded to B by BlendB. False when A is missing.
		 */
		bool ImageNormal(const FString& A, const FString& B, float BlendB, const FRect& R) const;
		/** A texture with explicit UVs (0..1 of the texture; beyond 1 tiles), e.g. panning bands and cropped backdrops. */
		bool ImageUV(const FString& ObjectPath, const FRect& R, float U0, float V0, float U1, float V1, const FLinearColor& Color, bool bAdditive = false) const;
		/** The style's image for State (ImgColors tint). False when the style has no image or it is missing. */
		bool StyleImage(const FStyle& S, EMenuState State, const FRect& R, float Alpha = 1.f) const;
		/** True when the 2K4 menu textures are available (otherwise the procedural fallback is drawn). */
		bool HasArt() const { return bHasArt; }
		/** Procedural stand-in for a style image (no content yet). */
		void FallbackBox(const FRect& R, EMenuState State, bool bButton, float Alpha = 1.f) const;

		// ---- backgrounds ----
		/** UT2k4MainPage backdrop: Bgndtile, BkRenders.Char01, scanlines (full screen). */
		void PageBackground(float Alpha = 1.f) const;
		/** GUIHeader (STY2Header, NewHeader) across the screen at the top; returns its rectangle. */
		FRect Header(const FString& Caption) const;
		/** ButtonFooter (STY2Footer, NewFooter) across the screen at the bottom with the status hint; returns its rectangle. */
		FRect Footer(const FString& Hint) const;

		// ---- widgets ----
		/** Widget id from a name and an index. */
		static uint32 Id(const TCHAR* Name, int32 Index = 0);

		/** GUIButton with a style; returns true when clicked (released over it, or Enter while focused). */
		bool Button(uint32 Id, const FRect& R, const FString& StyleKey, const FString& Caption, const FString& Hint, bool bEnabled = true,
			EFontScale Scale = EFontScale::Medium, ESound Sound = ESound::Click, bool bFocusable = true);
		/** moCheckBox: caption left, check box right (CaptionWidth of the width). */
		bool CheckBox(uint32 Id, const FRect& R, const FString& Caption, bool& bValue, const FString& Hint, float CaptionWidth = 0.8f, bool bEnabled = true);
		/** moComboBox: caption, edit field with the value, drop-down button and list. Returns true when Index changed. */
		bool Combo(uint32 Id, const FRect& R, const FString& Caption, int32& Index, const TArray<FString>& Items, const FString& Hint,
			float CaptionWidth = 0.5f, bool bEnabled = true);
		/**
		 * moSlider: caption, bar with the filled part, knob and the value text. Returns true while the value changes
		 * (drag, click, Left/Right); bOutReleased = the change is complete (mouse released / key step).
		 */
		bool Slider(uint32 Id, const FRect& R, const FString& Caption, float& Value, float Min, float Max, float Step, const FString& ValueText,
			const FString& Hint, bool& bOutReleased, float CaptionWidth = 0.5f, bool bEnabled = true);
		/** moNumericEdit / spinner: caption, value field and +/- spinner. Returns -1 / +1 when stepped, 0 otherwise. */
		int32 Spinner(uint32 Id, const FRect& R, const FString& Caption, const FString& ValueText, const FString& Hint, float CaptionWidth = 0.5f,
			bool bEnabled = true);
		/** moEditBox: caption and a text field (typing through a focus widget). Returns true when the text was committed (Enter / focus lost). */
		bool EditBox(uint32 Id, const FRect& R, const FString& Caption, FString& Value, const FString& Hint, float CaptionWidth = 0.5f,
			int32 MaxLen = 24);
		/**
		 * GUIListBox (+ GUIVertScrollBar): rows in the list style, selection in STY2ListSelection, wheel / drag scrolling.
		 * Returns true when Selected changed; bOutActivated on double click / Enter.
		 */
		bool List(uint32 Id, const FRect& R, const TArray<FListItem>& Items, int32& Selected, bool& bOutActivated, const FString& Hint,
			const FString& FontKey = TEXT("UT2MenuFont"), bool bLargeRows = false, const TArray<FListColumn>* Columns = nullptr);
		/** Row height of List() with that font (column headers drawn above a list line up with it). */
		float ListRowHeight(const FString& FontKey = TEXT("UT2MenuFont"), bool bLargeRows = false) const;
		/** GUITabControl: tab buttons centred in R (TabButton style, NewTabBk strip). Returns true when Active changed. */
		bool Tabs(uint32 Id, const FRect& R, const TArray<FString>& Captions, const TArray<FString>& Hints, int32& Active);
		/** GUISectionBackground (Display98) / AltSectionBackground (Display99) with caption; returns the client area. */
		FRect Section(const FRect& R, const FString& Caption, bool bAlt = false) const;
		/** GUILabel / TextLabel style text. */
		void Label(const FRect& R, const FString& Text, const FString& StyleKey = TEXT("TextLabel"), EAlign Align = EAlign::Left,
			EFontScale Scale = EFontScale::Medium, EMenuState State = EMenuState::Blurry) const;
		/** GUIScrollTextBox: word wrapped text (STY2NoBackground colour unless Color is given), scrolls with the wheel when hovered. */
		void ScrollText(uint32 Id, const FRect& R, const FString& Text, const FString& FontKey = TEXT("UT2MenuFont"),
			const FLinearColor& Color = FLinearColor::Transparent);

		/** A custom drawn widget: registers it (hover, focus chain, hint) and returns its menu state. */
		EMenuState CustomWidget(uint32 Id, const FRect& R, bool bEnabled, const FString& InHint, bool bFocusOnHover = false)
		{
			return Interact(Id, R, bEnabled, true, InHint, bFocusOnHover);
		}
		/** True when a custom widget was clicked (released over it) or activated with Enter / A while focused. */
		bool WasClicked(uint32 Id, const FRect& R);

		// ---- state / navigation ----
		bool IsFocused(uint32 Id) const { return Focused == Id; }
		void SetFocus(uint32 Id) { Focused = Id; }
		uint32 GetFocus() const { return Focused; }
		/** The hint of the hovered / focused widget this frame (for the footer). */
		const FString& GetHint() const { return Hint; }
		/** Esc / gamepad B pressed this frame (and no popup or text field took it). */
		bool BackPressed() const { return bBack; }
		/** Previous / next tab requested (shoulder buttons, PageUp / PageDown, Ctrl+Tab). */
		int32 TabStep() const { return TabDir; }
		/** Closes an open combo list / text field (page changes). */
		void ResetInteraction();
		/** True while a text field is edited (keys go to the text). */
		bool IsEditingText() const { return EditingId != 0; }
		/** Lets another text sink (the content setup path field) receive keys: the GUI then does not consume them. */
		static void SetExternalTextInput(bool bActive);
		/** Mouse position in screen pixels. */
		FVector2D Mouse() const { return MousePos; }

		/** Plays a UT2K4GUIController sound. */
		void PlaySound(ESound Sound) const;

		// ---- automation (non-shipping test hooks; never touches the real desktop) ----
		/** Injects a key press into the GUI's input queue. */
		static void InjectKey(const FKey& Key);
		/** Injects a click at a page-relative position (0..1 of the 4:3 page) or screen pixels (bPixels). */
		void InjectClick(float X, float Y, bool bPixels = false);
		void InjectWheel(int32 Delta);

	private:
		struct FWidgetState
		{
			uint32 Id = 0;
			FRect Rect;
			bool bEnabled = true;
		};
		/** Registers a focusable / hoverable widget; returns its menu state. */
		EMenuState Interact(uint32 Id, const FRect& R, bool bEnabled, bool bFocusable, const FString& InHint, bool bFocusOnHover = false);
		bool Hovered(const FRect& R) const;
		bool KeyPressed(const FKey& K) const;
		bool AnyKey(std::initializer_list<FKey> Keys) const;
		void ConsumeKey(const FKey& K);
		void DrawPopup();
		void CaptionLabel(const FRect& R, const FString& Caption, EMenuState State) const;
		float CaptionWidthFor(const FRect& R, const FString& Caption, float CaptionWidth) const;
		void BeginTextEdit(uint32 Id, const FString& Value, int32 MaxLen);
		void EndTextEdit(bool bCommit);

		FArenaDraw D;
		TWeakObjectPtr<APlayerController> PC;
		double Now = 0.0;
		float Delta = 0.f;
		float SafeX = 0.f, SafeW = 1.f, LScale = 1.f;
		int32 NScale = 1;
		bool bHasArt = false;
		bool bCapture = true;

		// input of this frame
		FVector2D MousePos = FVector2D::ZeroVector;
		bool bMouseDown = false;
		bool bMousePressed = false;
		bool bMouseReleased = false;
		bool bDoubleClick = false;
		int32 Wheel = 0;
		TArray<FKey> Keys;
		TArray<TCHAR> Chars;
		bool bShift = false;
		bool bCtrl = false;

		// retained state
		uint32 Focused = 0;
		uint32 HotId = 0;
		uint32 PressedId = 0;
		uint32 CaptureId = 0;
		uint32 LastHot = 0;
		TArray<FWidgetState> FocusChain;
		TArray<FWidgetState> PrevChain;
		int32 NavDir = 0;
		FString Hint;
		bool bBack = false;
		int32 TabDir = 0;
		bool bKeyboardNav = false;

		// popup (combo list)
		uint32 PopupId = 0;
		FRect PopupRect;
		TArray<FString> PopupItems;
		int32 PopupHover = -1;
		int32 PopupScroll = 0;
		int32 PopupResult = -1;
		uint32 PopupResultId = 0;
		FRect PrevPopupRect;

		// scrolling lists: id -> first visible row
		TMap<uint32, float> Scrolls;
		TMap<uint32, double> LastClickTime;
		TMap<uint32, int32> KnownSelection;

		// text editing
		uint32 EditingId = 0;
		FString EditText;
		int32 EditMaxLen = 24;
		bool bEditCommitted = false;
		uint32 CommittedId = 0;
		double EditStart = 0.0;
		TSharedPtr<class SArenaGUITextSink> TextSink;
		TSharedPtr<class SWidget> TextSinkHolder;

		/** Font() results of this frame by font key (cleared in Begin()) */
		mutable TMap<FString, FFontRef> FontsThisFrame;

		// injected (automation)
		TArray<FVector2D> InjectedClicks;
		bool bInjectedDown = false;
		int32 InjectedRelease = 0;

		friend class SArenaGUITextSink;
		friend class FArenaGUIInput;
	};
}
