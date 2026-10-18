#include "ArenaGUI2K4.h"
#include "OpenUT2k4.h"
#include "ArenaArtHUD.h"
#include "ArenaAssets.h"
#include "ArenaContent.h"
#include "ArenaLocalMessages.h"
#include "ArenaRuntimeContent.h"

#include "CanvasItem.h"
#include "Dom/JsonObject.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Input/Events.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundBase.h"
#include "TextureResource.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SLeafWidget.h"

using namespace ArenaGUI2K4;

// =================================================================================================
// Styles (GUI2K4 STY2* class defaults; base values from XInterface.GUIStyles and GUI2K4.GUI2Styles)
// =================================================================================================

namespace ArenaGUI2K4Local
{
	const FColor White(255, 255, 255, 255);

	void Fill5(FColor (&A)[5], FColor C0, FColor C1, FColor C2, FColor C3, FColor C4)
	{
		A[0] = C0; A[1] = C1; A[2] = C2; A[3] = C3; A[4] = C4;
	}
	void Images(FStyle& S, const TCHAR* B, const TCHAR* W, const TCHAR* F, const TCHAR* P, const TCHAR* D)
	{
		S.Images[0] = B; S.Images[1] = W; S.Images[2] = F; S.Images[3] = P; S.Images[4] = D;
	}
	void AllImages(FStyle& S, const TCHAR* Img) { Images(S, Img, Img, Img, Img, Img); }
	void ImgStyleAll(FStyle& S, EImgStyle St) { for (EImgStyle& I : S.ImgStyle) I = St; }
	void Fonts(FStyle& S, int32 Block, const TCHAR* Name) { for (int32 i = 0; i < 5; ++i) S.FontNames[Block * 5 + i] = Name; }
	void Borders(FStyle& S, int32 L, int32 T, int32 R, int32 B) { S.BorderOffsets[0] = L; S.BorderOffsets[1] = T; S.BorderOffsets[2] = R; S.BorderOffsets[3] = B; }

	/** XInterface.GUIStyles defaults */
	FStyle GUIStyles()
	{
		FStyle S;
		Fonts(S, 0, TEXT("UT2SmallFont"));
		Fonts(S, 1, TEXT("UT2MenuFont"));
		Fonts(S, 2, TEXT("UT2LargeFont"));
		Fill5(S.FontColors, FColor(0, 0, 64), FColor(0, 0, 64), FColor(32, 32, 80), FColor(32, 32, 80), FColor(0, 0, 128));
		Fill5(S.FontBKColors, FColor::Black, FColor::Black, FColor::Black, FColor::Black, FColor::Black);
		Fill5(S.ImgColors, White, White, White, White, FColor(128, 128, 128));
		for (int32 i = 0; i < 5; ++i)
		{
			S.RStyles[i] = 1;
			S.ImgStyle[i] = EImgStyle::Stretched;
			S.ImgWidths[i] = -1.f;
			S.ImgHeights[i] = -1.f;
		}
		Borders(S, 10, 10, 10, 10);
		return S;
	}

	/** GUI2K4.GUI2Styles: gold text, white when focused, grey disabled; no border offsets */
	FStyle GUI2Styles()
	{
		FStyle S = GUIStyles();
		Fill5(S.FontColors, FColor(255, 210, 0), FColor(255, 210, 0), FColor(255, 255, 255), FColor(255, 210, 0), FColor(133, 133, 133));
		Borders(S, 0, 0, 0, 0);
		return S;
	}

	TMap<FString, FStyle> BuildStyles()
	{
		TMap<FString, FStyle> M;
		auto Add = [&M](const TCHAR* Key, FStyle S) { S.KeyName = Key; M.Add(FString(Key).ToLower(), S); };

		// STY2SquareButton: NewControls.GradientButton* (stretched)
		FStyle Square = GUI2Styles();
		Images(Square, TEXT("2K4Menus.NewControls.GradientButtonBlurry"), TEXT("2K4Menus.NewControls.GradientButtonWatched"),
			TEXT("2K4Menus.NewControls.GradientButtonFocused"), TEXT("2K4Menus.NewControls.GradientButtonPressed"),
			TEXT("2K4Menus.NewControls.GradientButtonDisabled"));
		Add(TEXT("SquareButton"), Square);

		// STY2RoundButton: Controls.buttonThick_* (ISTY_PartialScaled), small header / header fonts for medium / large
		FStyle Round = GUI2Styles();
		Fonts(Round, 1, TEXT("UT2SmallHeaderFont"));
		Fonts(Round, 2, TEXT("UT2HeaderFont"));
		Images(Round, TEXT("2K4Menus.Controls.buttonThick_b"), TEXT("2K4Menus.Controls.buttonThick_w"), TEXT("2K4Menus.Controls.buttonThick_f"),
			TEXT("2K4Menus.Controls.buttonThick_p"), TEXT("2K4Menus.Controls.buttonThick_d"));
		ImgStyleAll(Round, EImgStyle::PartialScaled);
		Add(TEXT("RoundButton"), Round);
		// STY2RoundScaledButton: same, ISTY_Scaled
		FStyle RoundScaled = Round;
		ImgStyleAll(RoundScaled, EImgStyle::Scaled);
		Add(TEXT("RoundScaledButton"), RoundScaled);

		// STY2SquareMenuButton: square button with the small header font for medium
		FStyle SquareMenu = Square;
		Fonts(SquareMenu, 1, TEXT("UT2SmallHeaderFont"));
		Add(TEXT("SquareMenuButton"), SquareMenu);
		// STY2MidGameButton: UT2MidGameFont for every size
		FStyle MidGame = SquareMenu;
		Fonts(MidGame, 0, TEXT("UT2MidGameFont"));
		Fonts(MidGame, 1, TEXT("UT2MidGameFont"));
		Fonts(MidGame, 2, TEXT("UT2MidGameFont"));
		Add(TEXT("MidGameButton"), MidGame);

		// STY2ListBox: NewControls.SectionHeaderBase (partial scaled), header font large, BorderOffsets 3
		FStyle ListBox = Square;
		Fonts(ListBox, 2, TEXT("UT2HeaderFont"));
		Fill5(ListBox.FontColors, FColor(255, 195, 0), FColor(255, 210, 0), FColor(255, 255, 255), FColor(255, 255, 255), FColor(192, 192, 192));
		AllImages(ListBox, TEXT("2K4Menus.NewControls.SectionHeaderBase"));
		ImgStyleAll(ListBox, EImgStyle::PartialScaled);
		Borders(ListBox, 3, 3, 3, 3);
		Add(TEXT("ListBox"), ListBox);
		// STY2ComboListBox: NewControls.ComboListDropdown, BorderOffsets 5 / 0 / 5 / 0
		FStyle ComboList = ListBox;
		AllImages(ComboList, TEXT("2K4Menus.NewControls.ComboListDropdown"));
		ImgStyleAll(ComboList, EImgStyle::PartialScaled);
		Borders(ComboList, 5, 3, 5, 3);
		Add(TEXT("ComboListBox"), ComboList);

		// STY2TextButton: UT2MainMenuFont, (207,185,103) / (244,237,205), no image
		FStyle TextButton = GUI2Styles();
		Fonts(TextButton, 0, TEXT("UT2MainMenuFont"));
		Fill5(TextButton.FontColors, FColor(207, 185, 103), FColor(244, 237, 205), FColor(244, 237, 205), FColor(244, 237, 205), FColor(244, 237, 205));
		Add(TEXT("TextButton"), TextButton);
		// STY2TextButtonEffect: the caption effect (black) drawn under TextButton captions
		FStyle TextButtonEffect = TextButton;
		Fill5(TextButtonEffect.FontColors, FColor::Black, FColor::Black, FColor::Black, FColor::Black, FColor::Black);
		Add(TEXT("TextButtonEffect"), TextButtonEffect);

		// STY2Footer: NewControls.NewFooter, red text (140,0,0 disabled)
		FStyle Footer = GUI2Styles();
		Fill5(Footer.FontColors, FColor(255, 0, 0), FColor(255, 0, 0), FColor(255, 0, 0), FColor(255, 0, 0), FColor(140, 0, 0));
		AllImages(Footer, TEXT("2K4Menus.NewControls.NewFooter"));
		Add(TEXT("Footer"), Footer);
		// STY2Header (extends Footer): NewControls.NewHeader, UT2DefaultFont, GUI2Styles colours, BorderOffsets 0 / 4 / 0 / 4
		FStyle Header = Footer;
		Header.FontNames[0] = TEXT("UT2DefaultFont");
		Fill5(Header.FontColors, FColor(255, 210, 0), FColor(255, 210, 0), FColor(255, 255, 255), FColor(255, 210, 0), FColor(133, 133, 133));
		AllImages(Header, TEXT("2K4Menus.NewControls.NewHeader"));
		Borders(Header, 0, 4, 0, 4);
		Add(TEXT("Header"), Header);

		// STY2TextLabel: white, (255,240,0) watched, (230,200,0) focused / pressed
		FStyle TextLabel = GUI2Styles();
		Fill5(TextLabel.FontColors, FColor(255, 255, 255), FColor(255, 240, 0), FColor(230, 200, 0), FColor(230, 200, 0), FColor(255, 255, 255));
		Add(TEXT("TextLabel"), TextLabel);
		// STY2DarkTextLabel
		FStyle DarkLabel = TextLabel;
		Fill5(DarkLabel.FontColors, FColor(14, 41, 106), FColor(14, 41, 106), FColor(29, 86, 220), FColor(14, 41, 106), FColor(14, 41, 106));
		Add(TEXT("DarkTextLabel"), DarkLabel);

		// STY2TabButton: gradient buttons, BorderOffsets 4 / 5 / 4 / 5
		FStyle Tab = Square;
		Borders(Tab, 4, 5, 4, 5);
		Add(TEXT("TabButton"), Tab);
		// STY2FooterButton (extends TabButton): NewControls.FooterButton for the normal state, no border offsets
		FStyle FooterButton = Tab;
		FooterButton.Images[0] = TEXT("2K4Menus.NewControls.FooterButton");
		Borders(FooterButton, 0, 0, 0, 0);
		Add(TEXT("FooterButton"), FooterButton);
		// STY2TabBackground: NewControls.NewTabBk
		FStyle TabBk = GUI2Styles();
		AllImages(TabBk, TEXT("2K4Menus.NewControls.NewTabBk"));
		Add(TEXT("TabBackground"), TabBk);

		// STY2NoBackground: no image, header font for large, (255,188,0) text
		FStyle NoBk = GUI2Styles();
		Fonts(NoBk, 2, TEXT("UT2HeaderFont"));
		Fill5(NoBk.FontColors, FColor(255, 188, 0, 220), FColor(255, 188, 0), FColor(255, 188, 0), FColor(255, 188, 0), FColor(32, 32, 32));
		for (uint8& R : NoBk.RStyles) R = 0;
		Add(TEXT("NoBackground"), NoBk);

		// STY2ListSelection (extends XInterface.STY_ListSelection): selected row colours (37,59,127), (255,230,0) text, BorderOffsets 3
		FStyle ListSel = GUIStyles();
		Fonts(ListSel, 2, TEXT("UT2HeaderFont"));
		Fill5(ListSel.FontColors, FColor(255, 188, 0, 220), FColor(255, 230, 0), FColor(255, 230, 0), FColor(255, 230, 0), FColor(187, 159, 0, 140));
		Fill5(ListSel.FontBKColors, FColor(37, 59, 127, 220), FColor(37, 59, 127), FColor(37, 59, 127), FColor(37, 59, 127), FColor(10, 30, 94, 140));
		Fill5(ListSel.ImgColors, FColor(37, 59, 127, 220), FColor(37, 59, 127), FColor(28, 35, 128), FColor(28, 35, 128), FColor(10, 30, 94, 140));
		Borders(ListSel, 3, 3, 3, 3);
		Add(TEXT("ListSelection"), ListSel);
		// STY2ListSectionHeader: (24,24,74,200) band, MediumFont / small header font
		FStyle ListSection = GUI2Styles();
		Fonts(ListSection, 1, TEXT("MediumFont"));
		Fonts(ListSection, 2, TEXT("UT2SmallHeaderFont"));
		Fill5(ListSection.ImgColors, FColor(24, 24, 74, 200), FColor(24, 24, 74, 200), FColor(24, 24, 74, 200), FColor(24, 24, 74, 200), FColor(24, 24, 74, 200));
		Borders(ListSection, 5, 0, 0, 0);
		Add(TEXT("ListSection"), ListSection);

		// STY2EditBox: NewControls.EditBox*, BorderOffsets 5
		FStyle Edit = GUI2Styles();
		Images(Edit, TEXT("2K4Menus.NewControls.EditBoxBlurry"), TEXT("2K4Menus.NewControls.EditBoxWatched"), TEXT("2K4Menus.NewControls.EditBoxFocused"),
			TEXT("2K4Menus.NewControls.EditBoxPressed"), TEXT("2K4Menus.NewControls.EditBoxDisabled"));
		Borders(Edit, 5, 0, 0, 0);
		Add(TEXT("EditBox"), Edit);
		// STY2CheckBox: edit box images; STY2CheckBoxCheck: Controls.checkBoxBall_* (scaled)
		FStyle Check = Edit;
		Borders(Check, 0, 0, 0, 0);
		Add(TEXT("CheckBox"), Check);
		FStyle CheckBall = GUI2Styles();
		Images(CheckBall, TEXT("2K4Menus.Controls.checkBoxBall_b"), TEXT("2K4Menus.Controls.checkBoxBall_w"), TEXT("2K4Menus.Controls.checkBoxBall_f"),
			TEXT("2K4Menus.Controls.checkBoxBall_p"), TEXT("2K4Menus.Controls.checkBoxBall_d"));
		CheckBall.ImgStyle[0] = EImgStyle::Scaled;
		Add(TEXT("CheckBoxCheck"), CheckBall);

		// STY2ComboButton: NewControls.ComboTick*
		FStyle ComboButton = GUI2Styles();
		Images(ComboButton, TEXT("2K4Menus.NewControls.ComboTickBlurry"), TEXT("2K4Menus.NewControls.ComboTickWatched"), TEXT("2K4Menus.NewControls.ComboTickBlurry"),
			TEXT("2K4Menus.NewControls.ComboTickPressed"), TEXT("2K4Menus.NewControls.ComboTickBlurry"));
		Add(TEXT("ComboButton"), ComboButton);

		// STY2SliderBar: NewControls.NewSliderBar (partial scaled); STY2SliderKnob: SliderGrip* scaled at 26 x 13; STY2SliderCaption: UT2MenuFont
		FStyle SliderBar = Square;
		AllImages(SliderBar, TEXT("2K4Menus.NewControls.NewSliderBar"));
		ImgStyleAll(SliderBar, EImgStyle::PartialScaled);
		Add(TEXT("SliderBar"), SliderBar);
		FStyle SliderKnob = Round;
		Images(SliderKnob, TEXT("2K4Menus.NewControls.SliderGripBlurry"), TEXT("2K4Menus.NewControls.SliderGripWatched"), TEXT("2K4Menus.NewControls.SliderGripFocused"),
			TEXT("2K4Menus.NewControls.SliderGripPressed"), TEXT("2K4Menus.NewControls.SliderGripDisabled"));
		ImgStyleAll(SliderKnob, EImgStyle::Scaled);
		for (int32 i = 0; i < 5; ++i) { SliderKnob.ImgWidths[i] = 26.f; SliderKnob.ImgHeights[i] = 13.f; SliderKnob.RStyles[i] = 5; }
		Add(TEXT("SliderKnob"), SliderKnob);
		FStyle SliderCaption = GUI2Styles();
		Fonts(SliderCaption, 0, TEXT("UT2MenuFont"));
		Add(TEXT("SliderCaption"), SliderCaption);

		// STY2ScrollZone (NewScrollZone scaled), STY2VertUpButton / DownButton (Up/DownTick*), STY2VertGrip (ScrollGrip*)
		FStyle Zone = GUI2Styles();
		AllImages(Zone, TEXT("2K4Menus.NewControls.NewScrollZone"));
		ImgStyleAll(Zone, EImgStyle::Scaled);
		Add(TEXT("ScrollZone"), Zone);
		FStyle Up = GUI2Styles();
		Images(Up, TEXT("2K4Menus.NewControls.UpTickBlurry"), TEXT("2K4Menus.NewControls.UpTickWatched"), TEXT("2K4Menus.NewControls.UpTickBlurry"),
			TEXT("2K4Menus.NewControls.UpTickPressed"), TEXT("2K4Menus.NewControls.UpTickDisabled"));
		Add(TEXT("VertUpButton"), Up);
		FStyle Down = GUI2Styles();
		Images(Down, TEXT("2K4Menus.NewControls.DownTickBlurry"), TEXT("2K4Menus.NewControls.DownTickWatched"), TEXT("2K4Menus.NewControls.DownTickBlurry"),
			TEXT("2K4Menus.NewControls.DownTickPressed"), TEXT("2K4Menus.NewControls.DownTickDisabled"));
		Add(TEXT("VertDownButton"), Down);
		FStyle Grip = GUI2Styles();
		Images(Grip, TEXT("2K4Menus.NewControls.ScrollGripBlurry"), TEXT("2K4Menus.NewControls.ScrollGripWatched"), TEXT("2K4Menus.NewControls.ScrollGripBlurry"),
			TEXT("2K4Menus.NewControls.ScrollGripPressed"), TEXT("2K4Menus.NewControls.ScrollGripBlurry"));
		Add(TEXT("VertGrip"), Grip);
		// STY2Spinner: NewControls.Spinner* (scaled)
		FStyle Spinner = GUI2Styles();
		Images(Spinner, TEXT("2K4Menus.NewControls.SpinnerBlurry"), TEXT("2K4Menus.NewControls.SpinnerWatched"), TEXT("2K4Menus.NewControls.SpinnerBlurry"),
			TEXT("2K4Menus.NewControls.SpinnerPressed"), TEXT("2K4Menus.NewControls.SpinnerBlurry"));
		ImgStyleAll(Spinner, EImgStyle::Scaled);
		Add(TEXT("SpinnerButton"), Spinner);

		// STY2ArrowLeft / Right (round button, Controls.arrow*, scaled)
		FStyle ArrowL = Round;
		Images(ArrowL, TEXT("2K4Menus.Controls.arrowLeft_b"), TEXT("2K4Menus.Controls.arrowLeft_w"), TEXT("2K4Menus.Controls.arrowLeft_f"),
			TEXT("2K4Menus.Controls.arrowLeft_p"), TEXT("2K4Menus.Controls.arrowLeft_d"));
		ImgStyleAll(ArrowL, EImgStyle::Scaled);
		Add(TEXT("ArrowLeft"), ArrowL);
		FStyle ArrowR = ArrowL;
		Images(ArrowR, TEXT("2K4Menus.Controls.arrowRight_b"), TEXT("2K4Menus.Controls.arrowRight_w"), TEXT("2K4Menus.Controls.arrowRight_f"),
			TEXT("2K4Menus.Controls.arrowRight_p"), TEXT("2K4Menus.Controls.arrowRight_d"));
		Add(TEXT("ArrowRight"), ArrowR);

		// STY2CloseButton: NewControls.CloseBoxBall*
		FStyle Close = GUI2Styles();
		Images(Close, TEXT("2K4Menus.NewControls.CloseBoxBallBlurry"), TEXT("2K4Menus.NewControls.CloseBoxBallWatched"), TEXT("2K4Menus.NewControls.CloseBoxBallBlurry"),
			TEXT("2K4Menus.NewControls.CloseBoxBallPressed"), TEXT("2K4Menus.NewControls.CloseBoxBallBlurry"));
		Add(TEXT("CloseButton"), Close);

		// STY2MouseOverLabel (tool tips): Controls.editboxSquare_b at (221,221,221,128), text (64,64,80), small font, BorderOffsets 6 / 8 / 6 / 6
		FStyle MouseOver = GUI2Styles();
		Fonts(MouseOver, 1, TEXT("UT2SmallFont"));
		Fonts(MouseOver, 2, TEXT("UT2SmallFont"));
		Fill5(MouseOver.FontColors, FColor(64, 64, 80), FColor(64, 64, 80), FColor(64, 64, 80), FColor(64, 64, 80), FColor(64, 64, 80));
		Fill5(MouseOver.ImgColors, FColor(221, 221, 221, 128), FColor(221, 221, 221, 128), FColor(221, 221, 221, 128), FColor(221, 221, 221, 128), FColor(221, 221, 221, 128));
		Images(MouseOver, TEXT("2K4Menus.Controls.editboxSquare_b"), TEXT("2K4Menus.Controls.editboxSquare_b"), TEXT("2K4Menus.Controls.editboxSquare_b"),
			TEXT("2K4Menus.Controls.editboxSquare_b"), TEXT("2K4Menus.Controls.editboxSquare_d"));
		Borders(MouseOver, 6, 8, 6, 6);
		Add(TEXT("MouseOver"), MouseOver);

		// STY2SectionHeaderTop / Bar (section headers of lists)
		FStyle SecTop = GUI2Styles();
		Images(SecTop, TEXT("2K4Menus.NewControls.SectionHeaderTop"), TEXT("2K4Menus.NewControls.SectionHeaderTopWatched"), TEXT("2K4Menus.NewControls.SectionHeaderTopFocused"),
			TEXT("2K4Menus.NewControls.SectionHeaderTopPressed"), TEXT("2K4Menus.NewControls.SectionHeaderTop"));
		Add(TEXT("SectionHeaderTop"), SecTop);
		FStyle SecBar = GUI2Styles();
		Images(SecBar, TEXT("2K4Menus.NewControls.SectionHeaderBar"), TEXT("2K4Menus.NewControls.SectionHeaderBarWatched"), TEXT("2K4Menus.NewControls.SectionHeaderBarFocused"),
			TEXT("2K4Menus.NewControls.SectionHeaderBarPressed"), TEXT("2K4Menus.NewControls.SectionHeaderBar"));
		Add(TEXT("SectionHeaderBar"), SecBar);

		// STY2SquareBar (round button with Controls.SquareBoxA)
		FStyle SquareBar = Round;
		AllImages(SquareBar, TEXT("2K4Menus.Controls.SquareBoxA"));
		Add(TEXT("SquareBar"), SquareBar);
		return M;
	}

	// ---- fonts (GUI2K4 fntUT2k4* class defaults; FontArrayNames from the .int when localized) ----
	struct FFontDef
	{
		const TCHAR* Key;
		TArray<const TCHAR*> Names;
		bool bFixedSize = false;
		bool bScaled = false;
		int32 NormalXRes = 0;
		int32 FallBackRes = 0;
		/** fntUT2k4MidGame.FontScreenWidth (first entry <= XRes is used) */
		TArray<int32> ScreenWidths;
	};
	const TArray<FFontDef>& FontDefs()
	{
		static const TArray<FFontDef> Defs = {
			// fntUT2k4Small
			{ TEXT("UT2SmallFont"), { TEXT("2K4Fonts.FontSmallText"), TEXT("2K4Fonts.Verdana8"), TEXT("2K4Fonts.Verdana10"), TEXT("2K4Fonts.Verdana12"), TEXT("2K4Fonts.Verdana14") } },
			// fntUT2k4Menu
			{ TEXT("UT2MenuFont"), { TEXT("2K4Fonts.Verdana8"), TEXT("2K4Fonts.Verdana10"), TEXT("2K4Fonts.Verdana12"), TEXT("2K4Fonts.Verdana14"), TEXT("2K4Fonts.Verdana16") } },
			// fntUT2K4Medium
			{ TEXT("MediumFont"), { TEXT("2K4Fonts.Verdana10"), TEXT("2K4Fonts.Verdana12"), TEXT("2K4Fonts.Verdana14"), TEXT("2K4Fonts.Verdana16"), TEXT("2K4Fonts.Verdana18") } },
			// fntUT2k4Large
			{ TEXT("UT2LargeFont"), { TEXT("2K4Fonts.Verdana14"), TEXT("2K4Fonts.Verdana16"), TEXT("2K4Fonts.Verdana18"), TEXT("2K4Fonts.Verdana20"), TEXT("2K4Fonts.Verdana22") } },
			// fntUT2k4Header
			{ TEXT("UT2HeaderFont"), { TEXT("2K4Fonts.Verdana18"), TEXT("2K4Fonts.Verdana20"), TEXT("2K4Fonts.Verdana22"), TEXT("2K4Fonts.Verdana24"), TEXT("2K4Fonts.Verdana26") } },
			// fntUT2k4SmallHeader
			{ TEXT("UT2SmallHeaderFont"), { TEXT("2K4Fonts.Verdana12"), TEXT("2K4Fonts.Verdana14"), TEXT("2K4Fonts.Verdana16"), TEXT("2K4Fonts.Verdana18"), TEXT("2K4Fonts.Verdana20") } },
			// fntUT2k4IRC
			{ TEXT("UT2IRCFont"), { TEXT("2K4Fonts.FontSmallText"), TEXT("2K4Fonts.Verdana8"), TEXT("2K4Fonts.Verdana8"), TEXT("2K4Fonts.Verdana10"), TEXT("2K4Fonts.Verdana10") } },
			// fntUT2k4Default: bFixedSize
			{ TEXT("UT2DefaultFont"), { TEXT("2K4Fonts.Verdana10") }, true },
			// fntUT2k4ServerList: bFixedSize
			{ TEXT("UT2ServerListFont"), { TEXT("2K4Fonts.Verdana8") }, true },
			// fntUT2k4MainMenu: bScaled, NormalXRes 800, FallBackRes 512
			{ TEXT("UT2MainMenuFont"), { TEXT("2K4Fonts.Impact32"), TEXT("2K4Fonts.Verdana24") }, false, true, 800, 512 },
			// not a GUIFont: the HUD's FontArrayNames (XInterface.HudBase) smallest to largest, for big in-game banners
			{ TEXT("HudLargeFont"), { TEXT("UT2003Fonts.FontEurostile17"), TEXT("UT2003Fonts.FontEurostile21"), TEXT("UT2003Fonts.FontEurostile24"),
				TEXT("UT2003Fonts.FontEurostile29"), TEXT("UT2003Fonts.FontEurostile37") } },
			// fntUT2k4MidGame: FontScreenWidth 2048 1600 1280 1024 800 640 600
			{ TEXT("UT2MidGameFont"), { TEXT("UT2003Fonts.FontEurostile17"), TEXT("UT2003Fonts.FontEurostile14"), TEXT("UT2003Fonts.FontEurostile11"),
				TEXT("UT2003Fonts.FontEurostile11"), TEXT("UT2003Fonts.FontEurostile9"), TEXT("UT2003Fonts.FontNeuzeit9"), TEXT("UT2003Fonts.FontSmallText") },
				false, false, 0, 0, { 2048, 1600, 1280, 1024, 800, 640, 600 } },
		};
		return Defs;
	}

	/** Cell heights of the stock fonts (for the engine font fallback before content exists). */
	int32 FontCellHeight(const FString& Name)
	{
		static const TMap<FString, int32> Heights = {
			{ TEXT("2K4Fonts.FontSmallText"), 8 }, { TEXT("2K4Fonts.Verdana8"), 13 }, { TEXT("2K4Fonts.Verdana10"), 16 }, { TEXT("2K4Fonts.Verdana12"), 18 },
			{ TEXT("2K4Fonts.Verdana14"), 23 }, { TEXT("2K4Fonts.Verdana16"), 25 }, { TEXT("2K4Fonts.Verdana18"), 29 }, { TEXT("2K4Fonts.Verdana20"), 32 },
			{ TEXT("2K4Fonts.Verdana22"), 35 }, { TEXT("2K4Fonts.Verdana24"), 38 }, { TEXT("2K4Fonts.Verdana26"), 42 }, { TEXT("2K4Fonts.Impact32"), 53 },
			{ TEXT("UT2003Fonts.FontEurostile17"), 30 }, { TEXT("UT2003Fonts.FontEurostile21"), 37 }, { TEXT("UT2003Fonts.FontEurostile24"), 42 },
			{ TEXT("UT2003Fonts.FontEurostile29"), 50 }, { TEXT("UT2003Fonts.FontEurostile37"), 63 }, { TEXT("UT2003Fonts.FontEurostile14"), 25 }, { TEXT("UT2003Fonts.FontEurostile11"), 20 },
			{ TEXT("UT2003Fonts.FontEurostile9"), 16 }, { TEXT("UT2003Fonts.FontNeuzeit9"), 16 }, { TEXT("UT2003Fonts.FontSmallText"), 8 },
		};
		const int32* H = Heights.Find(Name);
		return H ? *H : 16;
	}

	/** GUIFont.GetFont(XRes) (native in UE2, reconstructed): fixed -> 0, scaled -> 0 (1 at or below FallBackRes), else by width. */
	int32 FontIndexFor(const FFontDef& F, float XRes)
	{
		const int32 Num = F.Names.Num();
		if (F.bFixedSize || Num <= 1)
		{
			return 0;
		}
		if (F.bScaled)
		{
			return XRes <= F.FallBackRes ? 1 : 0;
		}
		if (F.ScreenWidths.Num() > 0)
		{
			for (int32 i = 0; i < F.ScreenWidths.Num() && i < Num; ++i)
			{
				if (F.ScreenWidths[i] <= XRes)
				{
					return i;
				}
			}
			return Num - 1;
		}
		int32 Index = 4;
		if (XRes < 640.f) Index = 0;
		else if (XRes < 800.f) Index = 1;
		else if (XRes < 1024.f) Index = 2;
		else if (XRes < 1280.f) Index = 3;
		return FMath::Clamp(Index, 0, Num - 1);
	}

	/** The sizes of a font's face in the stock content (fonts named <face><size>); a lone font for anything else. */
	TArray<FString> FontFamily(const FString& Name)
	{
		struct FFamily { const TCHAR* Prefix; TArray<int32> Sizes; };
		static const FFamily Families[] = {
			{ TEXT("2K4Fonts.Verdana"), { 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34 } },
			{ TEXT("UT2003Fonts.FontEurostile"), { 8, 9, 11, 12, 14, 17, 21, 24, 29, 37 } },
			{ TEXT("UT2003Fonts.FontNeuzeit"), { 8, 9, 11, 12, 14, 17, 21, 24, 29, 37 } },
		};
		for (const FFamily& F : Families)
		{
			const FString Rest = Name.StartsWith(F.Prefix, ESearchCase::IgnoreCase) ? Name.Mid(FCString::Strlen(F.Prefix)) : FString();
			if (!Rest.IsEmpty() && Rest.IsNumeric())
			{
				TArray<FString> Out;
				for (const int32 Size : F.Sizes)
				{
					Out.Add(FString::Printf(TEXT("%s%d"), F.Prefix, Size));
				}
				return Out;
			}
		}
		return { Name };
	}

	/**
	 * The font to draw a Height pixel line of Name's face with: a size whose cell is within a pixel (4 %) of Height is
	 * drawn as it is; otherwise the smallest larger size is shrunk to Height (the best master), else the largest one is
	 * enlarged. OutHeight = the height to resample to.
	 */
	FString PickFontMaster(const FString& Name, int32 Height, int32& OutHeight)
	{
		FString Best, Above, Largest;
		int32 BestDiff = MAX_int32, AboveCell = MAX_int32, LargestCell = 0;
		for (const FString& Candidate : FontFamily(Name))
		{
			const int32 Cell = ArenaRuntimeContent::GetFontCellHeight(Candidate);
			if (Cell <= 0)
			{
				continue;
			}
			const int32 Diff = FMath::Abs(Cell - Height);
			if (Diff < BestDiff)
			{
				BestDiff = Diff;
				Best = Candidate;
			}
			if (Cell > Height && Cell < AboveCell)
			{
				AboveCell = Cell;
				Above = Candidate;
			}
			if (Cell > LargestCell)
			{
				LargestCell = Cell;
				Largest = Candidate;
			}
		}
		if (!Best.IsEmpty() && BestDiff <= FMath::Max(1, FMath::RoundToInt(Height * 0.04f)))
		{
			OutHeight = ArenaRuntimeContent::GetFontCellHeight(Best);
			return Best;
		}
		OutHeight = Height;
		return !Above.IsEmpty() ? Above : Largest;
	}

	FString ToRelativeTexture(const FString& ObjectPath)
	{
		return ArenaRuntimeContent::ObjectPathToRelative(TEXT("Textures"), ObjectPath);
	}

	const FTexture* ResourceOf(UTexture2D* T)
	{
		return T ? T->GetResource() : nullptr;
	}

	void TileUV(UCanvas* Canvas, const FTexture* Res, float X, float Y, float W, float H, FVector2D UV0, FVector2D UV1, const FLinearColor& C, bool bAdditive)
	{
		if (!Canvas || !Res || W <= 0.f || H <= 0.f || C.A <= 0.f)
		{
			return;
		}
		FCanvasTileItem Item(FVector2D(X, Y), Res, FVector2D(W, H), UV0, UV1, C);
		Item.BlendMode = bAdditive ? SE_BLEND_Additive : SE_BLEND_Translucent;
		Canvas->DrawItem(Item);
	}

	FLinearColor ToLinear(const FColor& C, float Alpha = 1.f)
	{
		return FArenaDraw::UTColor(C, Alpha);
	}

	/** Width of Text drawn with a UFont at Scale (glyph cells + kerning, like UE2's canvas). */
	float FontTextWidth(UFont* Font, const FString& Text, float Scale)
	{
		float W = 0.f;
		int32 N = 0;
		for (int32 i = 0; i < Text.Len(); ++i)
		{
			const TCHAR C = Text[i];
			if (C == 0x1B)
			{
				i += 3; // UE2 colour code (ESC R G B): no width
				continue;
			}
			const int32 Ch = int32(Font->RemapChar(C));
			if (Font->Characters.IsValidIndex(Ch))
			{
				W += float(Font->Characters[Ch].USize);
				++N;
			}
		}
		if (N > 1)
		{
			W += float(Font->Kerning * (N - 1));
		}
		return W * Scale;
	}

	// ---- material json (for FAnimatedImage) ----
	TSharedPtr<FJsonObject> LoadMaterialJson(const FString& ObjectPath)
	{
		const FString File = ArenaRuntimeContent::FindCacheFileByObjectPath(TEXT("materials"), ObjectPath, TEXT(".json"));
		FString Text;
		if (File.IsEmpty() || !FFileHelper::LoadFileToString(Text, *File))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root))
		{
			return nullptr;
		}
		return Root;
	}

	bool IsTexturePath(const FString& ObjectPath)
	{
		return !ArenaRuntimeContent::FindCacheFileByObjectPath(TEXT("textures"), ObjectPath, TEXT(".png")).IsEmpty();
	}

	/** A material object path to the texture it mostly shows (texture itself, or the converter's base_texture). */
	FString ResolveTexture(const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty() || IsTexturePath(ObjectPath))
		{
			return ObjectPath;
		}
		if (TSharedPtr<FJsonObject> J = LoadMaterialJson(ObjectPath))
		{
			FString Base;
			if (J->TryGetStringField(TEXT("base_texture"), Base))
			{
				return Base;
			}
		}
		return FString();
	}
}

using namespace ArenaGUI2K4Local;

const FStyle& ArenaGUI2K4::GetStyle(const FString& KeyName)
{
	static const TMap<FString, FStyle> Styles = BuildStyles();
	if (const FStyle* S = Styles.Find(KeyName.ToLower()))
	{
		return *S;
	}
	return Styles.FindChecked(TEXT("squarebutton"));
}

FString ArenaGUI2K4::Loc(const TCHAR* Package, const TCHAR* Section, const TCHAR* Key, const FString& Fallback)
{
	FString Value;
	if (ArenaLocalMessages::Localize(Package, Section, Key, Value) && !Value.IsEmpty())
	{
		return Value;
	}
	return Fallback;
}

UTexture2D* ArenaGUI2K4::LoadTex(const FString& ObjectPath)
{
	if (ObjectPath.IsEmpty())
	{
		return nullptr;
	}
	struct FEntry { TWeakObjectPtr<UTexture2D> Tex; double RetryAt = 0.0; };
	static TMap<FString, FEntry> Cache;
	FEntry& E = Cache.FindOrAdd(ObjectPath);
	if (UTexture2D* T = E.Tex.Get())
	{
		return T;
	}
	const double Now = FPlatformTime::Seconds();
	if (Now < E.RetryAt)
	{
		return nullptr;
	}
	// content can appear while the game runs (the setup screen converts it): misses are retried every few seconds
	E.RetryAt = Now + 3.0;
	UTexture2D* T = nullptr;
	if (ArenaRuntimeContent::IsActive())
	{
		T = ArenaRuntimeContent::LoadTextureByObjectPath(ObjectPath);
		if (T)
		{
			T->bForceMiplevelsToBeResident = true;
			T->bIgnoreStreamingMipBias = true;
		}
	}
	if (!T)
	{
		T = ArenaArtHUD::GetTexture(*ToRelativeTexture(ObjectPath));
	}
	E.Tex = T;
	return T;
}

// =================================================================================================
// FAnimatedImage
// =================================================================================================

const FAnimatedImage& FAnimatedImage::Get(const FString& ObjectPath)
{
	static TMap<FString, FAnimatedImage> Cache;
	if (const FAnimatedImage* Found = Cache.Find(ObjectPath))
	{
		return *Found;
	}
	FAnimatedImage Img;
	if (!ObjectPath.IsEmpty() && ArenaRuntimeContent::IsActive())
	{
		if (IsTexturePath(ObjectPath))
		{
			Img.Items.Add({ ObjectPath, 1.f, false });
		}
		else if (TSharedPtr<FJsonObject> J = LoadMaterialJson(ObjectPath))
		{
			FString Class;
			J->TryGetStringField(TEXT("class"), Class);
			const TSharedPtr<FJsonObject>* Props = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Seq = nullptr;
			if (Class == TEXT("MaterialSequence") && J->TryGetObjectField(TEXT("properties"), Props) && (*Props)->TryGetArrayField(TEXT("SequenceItems"), Seq))
			{
				// UE2 MaterialSequence: MSA_ShowMaterial (0) shows an item for Time, MSA_FadeToMaterial (1) fades to it over Time
				for (const TSharedPtr<FJsonValue>& V : *Seq)
				{
					const TSharedPtr<FJsonObject>* It = nullptr;
					if (!V->TryGetObject(It))
					{
						continue;
					}
					FString Mat;
					double Time = 1.0;
					int32 Action = 0;
					(*It)->TryGetStringField(TEXT("Material"), Mat);
					(*It)->TryGetNumberField(TEXT("Time"), Time);
					(*It)->TryGetNumberField(TEXT("Action"), Action);
					const FString Tex = ResolveTexture(Mat);
					if (!Tex.IsEmpty())
					{
						Img.Items.Add({ Tex, float(FMath::Max(0.05, Time)), Action == 1 });
					}
				}
			}
			if (Img.Items.Num() == 0)
			{
				FString Base;
				if (J->TryGetStringField(TEXT("base_texture"), Base) && !Base.IsEmpty())
				{
					Img.Items.Add({ Base, 1.f, false });
				}
			}
		}
	}
	for (const FItem& I : Img.Items)
	{
		Img.TotalTime += I.Time;
	}
	return Cache.Add(ObjectPath, Img);
}

void FAnimatedImage::SamplePlaying(double Now, FString& OutA, FString& OutB, float& OutBlendB) const
{
	if (PlayStart < 0.0 || Now < LastDrawn || Now - LastDrawn > 0.25)
	{
		PlayStart = Now;
	}
	LastDrawn = Now;
	Sample(float(Now - PlayStart), OutA, OutB, OutBlendB);
}

void FAnimatedImage::Sample(float Time, FString& OutA, FString& OutB, float& OutBlendB) const
{
	OutA.Reset();
	OutB.Reset();
	OutBlendB = 0.f;
	if (Items.Num() == 0)
	{
		return;
	}
	if (Items.Num() == 1 || TotalTime <= 0.f)
	{
		OutA = Items[0].Texture;
		return;
	}
	float T = FMath::Fmod(FMath::Max(0.f, Time), TotalTime);
	FString Prev = Items.Last().Texture;
	for (const FItem& I : Items)
	{
		if (T < I.Time)
		{
			if (I.bFade)
			{
				OutA = Prev;
				OutB = I.Texture;
				OutBlendB = FMath::Clamp(T / I.Time, 0.f, 1.f);
			}
			else
			{
				OutA = I.Texture;
			}
			return;
		}
		T -= I.Time;
		Prev = I.Texture;
	}
	OutA = Items.Last().Texture;
}

// =================================================================================================
// Input: pre-processor queue and text focus widget
// =================================================================================================

namespace ArenaGUI2K4Local
{
	struct FGuiEvent
	{
		enum class EType : uint8 { Key, MouseDown, MouseUp, DoubleClick, Wheel, Char };
		EType Type = EType::Key;
		FKey Key;
		float Delta = 0.f;
		TCHAR Char = 0;
		bool bShift = false;
		bool bCtrl = false;
	};
}

namespace ArenaGUI2K4
{
	/** Queues input for the active GUI (and keeps it from the game while a menu is open). */
	class FArenaGUIInput : public IInputProcessor
	{
	public:
		TArray<FGuiEvent> Queue;
		double ActiveUntil = 0.0;
		bool bInternalText = false;
		bool bExternalText = false;
		// gamepad stick / button auto repeat
		FKey HeldPad;
		double HeldSince = 0.0, NextRepeat = 0.0;
		float StickX = 0.f, StickY = 0.f;

		static FArenaGUIInput& Get()
		{
			static TSharedPtr<FArenaGUIInput> Instance;
			if (!Instance.IsValid())
			{
				Instance = MakeShared<FArenaGUIInput>();
				if (FSlateApplication::IsInitialized())
				{
					FSlateApplication::Get().RegisterInputPreProcessor(Instance, 0);
				}
			}
			return *Instance;
		}

		bool IsActive() const { return FPlatformTime::Seconds() < ActiveUntil; }

		static bool IsPassThrough(const FKey& K)
		{
			// console, function keys, the gamepad menu button (toggles the in-game menu) and modifiers stay with the game
			return K == EKeys::Tilde || K == EKeys::Gamepad_Special_Right || K == EKeys::Gamepad_Special_Left || K == EKeys::F1 || K == EKeys::F2
				|| K == EKeys::F3 || K == EKeys::F4 || K == EKeys::F5 || K == EKeys::F6 || K == EKeys::F7 || K == EKeys::F8 || K == EKeys::F9
				|| K == EKeys::F10 || K == EKeys::F11 || K == EKeys::F12 || K.IsModifierKey();
		}

		void Push(const FGuiEvent& E)
		{
			if (Queue.Num() < 256)
			{
				Queue.Add(E);
			}
		}

		virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override
		{
			if (!IsActive() || !HeldPad.IsValid())
			{
				return;
			}
			const double Now = FPlatformTime::Seconds();
			if (Now - HeldSince > 0.4 && Now >= NextRepeat)
			{
				NextRepeat = Now + 0.1;
				FGuiEvent E;
				E.Key = HeldPad;
				Push(E);
			}
		}

		virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override
		{
			const FKey K = InKeyEvent.GetKey();
			if (!IsActive() || bInternalText || bExternalText || IsPassThrough(K))
			{
				return false;
			}
			FGuiEvent E;
			E.Key = K;
			E.bShift = InKeyEvent.IsShiftDown();
			E.bCtrl = InKeyEvent.IsControlDown() || InKeyEvent.IsCommandDown();
			Push(E);
			if (K.IsGamepadKey() && !InKeyEvent.IsRepeat())
			{
				HeldPad = K;
				HeldSince = FPlatformTime::Seconds();
				NextRepeat = 0.0;
			}
			return true;
		}

		virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override
		{
			const FKey K = InKeyEvent.GetKey();
			if (K == HeldPad)
			{
				HeldPad = FKey();
			}
			return IsActive() && !bInternalText && !bExternalText && !IsPassThrough(K);
		}

		virtual bool HandleAnalogInputEvent(FSlateApplication& SlateApp, const FAnalogInputEvent& InEvent) override
		{
			if (!IsActive())
			{
				return false;
			}
			const FKey K = InEvent.GetKey();
			const float V = InEvent.GetAnalogValue();
			auto Edge = [this](float& State, float Value, const FKey& Neg, const FKey& Pos)
			{
				const float Old = State;
				State = Value;
				const int32 OldDir = Old > 0.6f ? 1 : (Old < -0.6f ? -1 : 0);
				const int32 NewDir = Value > 0.6f ? 1 : (Value < -0.6f ? -1 : 0);
				if (NewDir != OldDir)
				{
					if (NewDir != 0)
					{
						FGuiEvent E;
						E.Key = NewDir > 0 ? Pos : Neg;
						Push(E);
						HeldPad = E.Key;
						HeldSince = FPlatformTime::Seconds();
					}
					else if (HeldPad == Pos || HeldPad == Neg)
					{
						HeldPad = FKey();
					}
				}
			};
			if (K == EKeys::Gamepad_LeftY)
			{
				Edge(StickY, V, EKeys::Gamepad_LeftStick_Down, EKeys::Gamepad_LeftStick_Up);
			}
			else if (K == EKeys::Gamepad_LeftX)
			{
				Edge(StickX, V, EKeys::Gamepad_LeftStick_Left, EKeys::Gamepad_LeftStick_Right);
			}
			return true;
		}

		virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
		{
			if (!IsActive())
			{
				return false;
			}
			FGuiEvent E;
			E.Type = FGuiEvent::EType::MouseDown;
			E.Key = MouseEvent.GetEffectingButton();
			Push(E);
			// keep the click for the text field widget while it is edited (it ends editing when clicked elsewhere)
			return true;
		}

		virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
		{
			if (!IsActive())
			{
				return false;
			}
			FGuiEvent E;
			E.Type = FGuiEvent::EType::MouseUp;
			E.Key = MouseEvent.GetEffectingButton();
			Push(E);
			return true;
		}

		virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
		{
			if (!IsActive())
			{
				return false;
			}
			FGuiEvent E;
			E.Type = FGuiEvent::EType::DoubleClick;
			E.Key = MouseEvent.GetEffectingButton();
			Push(E);
			return true;
		}

		virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& InWheelEvent, const FPointerEvent* InGestureEvent) override
		{
			if (!IsActive() || InGestureEvent)
			{
				return false;
			}
			FGuiEvent E;
			E.Type = FGuiEvent::EType::Wheel;
			E.Delta = InWheelEvent.GetWheelDelta();
			Push(E);
			return true;
		}

		virtual const TCHAR* GetDebugName() const override { return TEXT("ArenaGUI2K4"); }
	};

	/** Invisible 1x1 widget that owns keyboard focus while a GUI text field is edited (typed characters, IME, layouts). */
	class SArenaGUITextSink : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SArenaGUITextSink) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&) {}
		virtual bool SupportsKeyboardFocus() const override { return true; }
		virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1.f, 1.f); }
		virtual int32 OnPaint(const FPaintArgs&, const FGeometry&, const FSlateRect&, FSlateWindowElementList&, int32 LayerId, const FWidgetStyle&, bool) const override
		{
			return LayerId;
		}
		virtual FReply OnKeyDown(const FGeometry&, const FKeyEvent& Event) override
		{
			FGuiEvent E;
			E.Key = Event.GetKey();
			E.bCtrl = Event.IsControlDown() || Event.IsCommandDown();
			E.bShift = Event.IsShiftDown();
			FArenaGUIInput::Get().Push(E);
			return FReply::Handled();
		}
		virtual FReply OnKeyUp(const FGeometry&, const FKeyEvent&) override { return FReply::Handled(); }
		virtual FReply OnKeyChar(const FGeometry&, const FCharacterEvent& Event) override
		{
			if (!Event.GetModifierKeys().IsControlDown() && !Event.GetModifierKeys().IsCommandDown())
			{
				FGuiEvent E;
				E.Type = FGuiEvent::EType::Char;
				E.Char = Event.GetCharacter();
				FArenaGUIInput::Get().Push(E);
			}
			return FReply::Handled();
		}
	};
}

void FGUI::SetExternalTextInput(bool bActive)
{
	FArenaGUIInput::Get().bExternalText = bActive;
}

void FGUI::InjectKey(const FKey& Key)
{
	FGuiEvent E;
	E.Key = Key;
	FArenaGUIInput::Get().Push(E);
}

void FGUI::InjectClick(float X, float Y, bool bPixels)
{
	InjectedClicks.Add(bPixels ? FVector2D(X, Y) : FVector2D(SafeX + X * SafeW, Y * D.H()));
}

void FGUI::InjectWheel(int32 InDelta)
{
	FGuiEvent E;
	E.Type = FGuiEvent::EType::Wheel;
	E.Delta = float(InDelta);
	FArenaGUIInput::Get().Push(E);
}

// =================================================================================================
// FGUI: frame
// =================================================================================================

FGUI::FGUI()
{
}

FGUI::~FGUI()
{
	EndTextEdit(false);
}

uint32 FGUI::Id(const TCHAR* Name, int32 Index)
{
	return HashCombine(GetTypeHash(FString(Name)), GetTypeHash(Index + 7919)) | 1u;
}

void FGUI::Begin(UCanvas* Canvas, APlayerController* InPC, bool bCaptureInput)
{
	D = FArenaDraw(Canvas);
	PC = InPC;
	const double T = FPlatformTime::Seconds();
	Delta = Now > 0.0 ? float(FMath::Clamp(T - Now, 0.0, 0.25)) : 0.f;
	Now = T;
	bCapture = bCaptureInput;

	const float W = D.W(), H = D.H();
	// 4:3 safe area by height; narrower screens use their whole width
	SafeW = FMath::Min(W, H * 4.f / 3.f);
	SafeX = FMath::RoundToFloat((W - SafeW) * 0.5f);
	LScale = H / 768.f;
	// whole-number scale of pixel details (TextScale()): 1 up to 1439 px high, 2 at 1440, 3 at 2160
	NScale = FMath::Max(1, FMath::FloorToInt(H / 720.f + 0.001f));
	bHasArt = ResourceOf(LoadTex(TEXT("2K4Menus.NewControls.GradientButtonBlurry"))) != nullptr;
	FontsThisFrame.Reset();

	// input of this frame
	bMousePressed = bMouseReleased = bDoubleClick = false;
	Wheel = 0;
	Keys.Reset();
	Chars.Reset();
	bBack = false;
	TabDir = 0;
	NavDir = 0;
	Hint.Reset();
	PrevChain = MoveTemp(FocusChain);
	FocusChain.Reset();
	PrevPopupRect = PopupId != 0 ? PopupRect : FRect();
	LastHot = HotId;
	HotId = 0;

	FArenaGUIInput& In = FArenaGUIInput::Get();
	if (bCapture)
	{
		In.ActiveUntil = T + 0.25;
		In.bInternalText = EditingId != 0;
		float MX = 0.f, MY = 0.f;
		// an injected click keeps its position for its press and release frames
		if (InjectedRelease == 0 && InjectedClicks.Num() == 0 && InPC && InPC->GetMousePosition(MX, MY))
		{
			MousePos = FVector2D(MX, MY);
		}
		for (const FGuiEvent& E : In.Queue)
		{
			switch (E.Type)
			{
			case FGuiEvent::EType::Key:
				Keys.Add(E.Key);
				bShift = E.bShift;
				bCtrl = E.bCtrl;
				break;
			case FGuiEvent::EType::Char:
				Chars.Add(E.Char);
				break;
			case FGuiEvent::EType::MouseDown:
			case FGuiEvent::EType::DoubleClick:
				if (E.Key == EKeys::LeftMouseButton)
				{
					bMouseDown = true;
					bMousePressed = true;
					bDoubleClick |= E.Type == FGuiEvent::EType::DoubleClick;
				}
				break;
			case FGuiEvent::EType::MouseUp:
				if (E.Key == EKeys::LeftMouseButton)
				{
					bMouseDown = false;
					bMouseReleased = true;
				}
				break;
			case FGuiEvent::EType::Wheel:
				Wheel += E.Delta > 0.f ? 1 : (E.Delta < 0.f ? -1 : 0);
				break;
			}
		}
		// injected clicks (automation): press on one frame, release on the next
		if (InjectedRelease > 0)
		{
			bMouseDown = false;
			bMouseReleased = true;
			InjectedRelease = 0;
		}
		else if (InjectedClicks.Num() > 0)
		{
			MousePos = InjectedClicks[0];
			InjectedClicks.RemoveAt(0);
			bMouseDown = true;
			bMousePressed = true;
			InjectedRelease = 1;
		}
	}
	In.Queue.Reset();

	if (!bCapture)
	{
		return;
	}
	// global keys
	if (EditingId == 0 && PopupId == 0)
	{
		if (AnyKey({ EKeys::Escape, EKeys::Gamepad_FaceButton_Right }))
		{
			bBack = true;
		}
		if (AnyKey({ EKeys::PageUp, EKeys::Gamepad_LeftShoulder, EKeys::Gamepad_LeftTrigger }) || (bCtrl && bShift && KeyPressed(EKeys::Tab)))
		{
			TabDir = -1;
		}
		if (AnyKey({ EKeys::PageDown, EKeys::Gamepad_RightShoulder, EKeys::Gamepad_RightTrigger }) || (bCtrl && !bShift && KeyPressed(EKeys::Tab)))
		{
			TabDir = +1;
		}
	}
}

bool FGUI::KeyPressed(const FKey& K) const
{
	return Keys.Contains(K);
}

bool FGUI::AnyKey(std::initializer_list<FKey> InKeys) const
{
	for (const FKey& K : InKeys)
	{
		if (Keys.Contains(K))
		{
			return true;
		}
	}
	return false;
}

void FGUI::ConsumeKey(const FKey& K)
{
	Keys.Remove(K);
}

void FGUI::ResetInteraction()
{
	PopupId = 0;
	PopupResultId = 0;
	CaptureId = 0;
	PressedId = 0;
	EndTextEdit(false);
}

void FGUI::End()
{
	if (!D.Canvas)
	{
		return;
	}
	DrawPopup();

	if (bCapture && EditingId == 0)
	{
		// focus navigation
		int32 Dir = 0;
		if (AnyKey({ EKeys::Up, EKeys::Gamepad_DPad_Up, EKeys::Gamepad_LeftStick_Up }) || (KeyPressed(EKeys::Tab) && bShift && !bCtrl))
		{
			Dir = -1;
		}
		else if (AnyKey({ EKeys::Down, EKeys::Gamepad_DPad_Down, EKeys::Gamepad_LeftStick_Down }) || (KeyPressed(EKeys::Tab) && !bShift && !bCtrl))
		{
			Dir = +1;
		}
		if (Dir != 0 && FocusChain.Num() > 0)
		{
			int32 Cur = FocusChain.IndexOfByPredicate([this](const FWidgetState& W) { return W.Id == Focused; });
			const int32 Num = FocusChain.Num();
			Cur = Cur == INDEX_NONE ? (Dir > 0 ? -1 : 0) : Cur;
			const int32 Next = ((Cur + Dir) % Num + Num) % Num;
			Focused = FocusChain[Next].Id;
			bKeyboardNav = true;
			PlaySound(ESound::Over);
		}
		else if (FocusChain.Num() > 0 && !FocusChain.ContainsByPredicate([this](const FWidgetState& W) { return W.Id == Focused; }))
		{
			// keep something focused for keyboard / gamepad players (UE2 focuses the first control of a page)
			Focused = FocusChain[0].Id;
		}
	}
	if (!bMouseDown)
	{
		PressedId = 0;
		CaptureId = 0;
	}
	if (LastHot != HotId && HotId != 0 && bCapture)
	{
		PlaySound(ESound::Over);
	}

	// tool tip (STY2MouseOverLabel) for the hovered widget after a short delay, like GUIToolTip
	static uint32 TipId = 0;
	static double TipSince = 0.0;
	if (HotId != TipId)
	{
		TipId = HotId;
		TipSince = Now;
	}
	if (bCapture && HotId != 0 && !Hint.IsEmpty() && Now - TipSince > 0.75 && PopupId == 0 && CaptureId == 0)
	{
		const FStyle& S = GetStyle(TEXT("MouseOver"));
		const FFontRef F = StyleFont(S, EMenuState::Blurry, EFontScale::Small);
		const TArray<FString> Lines = Wrap(F, Hint, FMath::Min(SafeW * 0.4f, L(360.f)));
		float TW = 0.f;
		for (const FString& Ln : Lines) TW = FMath::Max(TW, TextSize(F, Ln).X);
		const float PadX = L(S.BorderOffsets[0]), PadY = L(4.f);
		FRect R(MousePos.X + L(14.f), MousePos.Y + L(22.f), TW + PadX * 2.f, Lines.Num() * F.Height + PadY * 2.f);
		R.X = FMath::Min(R.X, D.W() - R.W - 2.f);
		R.Y = FMath::Min(R.Y, D.H() - R.H - 2.f);
		if (!StyleImage(S, EMenuState::Blurry, R))
		{
			D.Rect(R.X, R.Y, R.W, R.H, FLinearColor(0.86f, 0.86f, 0.86f, 0.85f));
		}
		float Y = R.Y + PadY;
		for (const FString& Ln : Lines)
		{
			Text(F, Ln, R.X + PadX, Y, ToLinear(S.FontColors[0]));
			Y += F.Height;
		}
	}
}

// =================================================================================================
// FGUI: metrics, text, images
// =================================================================================================

FRect FGUI::Rel(const FRect& Parent, float Left, float Top, float Width, float Height) const
{
	auto X = [&](float V) { return FMath::Abs(V) <= 2.f ? V * Parent.W : V * LScale; };
	auto Y = [&](float V) { return FMath::Abs(V) <= 2.f ? V * Parent.H : V * LScale; };
	return FRect(Parent.X + X(Left), Parent.Y + Y(Top), X(Width), Y(Height));
}

FFontRef FGUI::Font(const FString& FontKey) const
{
	if (const FFontRef* Cached = FontsThisFrame.Find(FontKey))
	{
		return *Cached;
	}
	const FFontDef* Def = FontDefs().FindByPredicate([&FontKey](const FFontDef& F) { return FontKey.Equals(F.Key, ESearchCase::IgnoreCase); });
	if (!Def)
	{
		Def = &FontDefs()[1];
	}
	// UE2 drew GUI fonts 1:1 and picked bigger ones for wider screens, up to 1280 (pages there are mostly empty space).
	// Here a page is laid out like a 4:3 screen of the height: the font is picked for the virtual resolution of the
	// height / 720 (that size factor, at least 1) and drawn that much larger, resampled to the exact pixel height.
	const float SizeK = FMath::Max(1.f, D.H() / 720.f);
	FString Name;
	float K = SizeK;
	if (Def->bScaled)
	{
		// scaled fonts (the main menu's Impact): page width / NormalXRes
		Name = Def->Names[FontIndexFor(*Def, SafeW)];
		K = SafeW / float(FMath::Max(1, Def->NormalXRes));
	}
	else
	{
		const float VirtualXRes = FMath::Min((D.H() / SizeK) * 4.f / 3.f, SafeW / SizeK);
		Name = Def->Names[FontIndexFor(*Def, VirtualXRes)];
	}
	const int32 StockCell = ArenaRuntimeContent::IsActive() ? ArenaRuntimeContent::GetFontCellHeight(Name) : 0;
	const float BaseCell = float(StockCell > 0 ? StockCell : FontCellHeight(Name));
	const int32 Want = FMath::Max(4, FMath::RoundToInt(BaseCell * K));
	FFontRef R;
	R.Px = K;
	R.Scale = 1.f;
	if (StockCell > 0)
	{
		int32 Height = Want;
		const FString Master = PickFontMaster(Name, Want, Height);
		R.Font = ArenaRuntimeContent::LoadFontAtHeight(Master, Height);
	}
	R.Height = R.Font ? FMath::Max(1.f, float(R.Font->GetMaxCharHeight())) : float(Want);
	FontsThisFrame.Add(FontKey, R);
	return R;
}

FVector2D FGUI::TextSize(const FFontRef& F, const FString& InText) const
{
	if (InText.IsEmpty())
	{
		return FVector2D(0.f, F.Height);
	}
	if (F.Font)
	{
		return FVector2D(FontTextWidth(F.Font, InText, F.Scale), F.Height);
	}
	return FVector2D(D.TextSize(InText, F.Height * 0.8f).X, F.Height);
}

float FGUI::Text(const FFontRef& F, const FString& InText, float X, float Y, const FLinearColor& Color, EAlign Align, bool bShadow) const
{
	if (!D.Canvas || InText.IsEmpty() || Color.A <= 0.f)
	{
		return 0.f;
	}
	const float W = TextSize(F, InText).X;
	float DX = X;
	if (Align == EAlign::Center) DX -= W * 0.5f;
	else if (Align == EAlign::Right) DX -= W;
	if (!F.Font)
	{
		// engine font before content exists, sized like the bitmap font
		D.Text(InText, DX, Y + F.Height * 0.08f, Color, F.Height * 0.8f, EArenaAlign::Left, bShadow);
		return W;
	}
	int32 Esc = INDEX_NONE;
	if (InText.FindChar(TCHAR(0x1B), Esc))
	{
		// UE2 colour codes (names from UT2004 servers): ESC R G B colours what follows
		float RunX = DX;
		FLinearColor RunColor = Color;
		FString Run;
		auto Flush = [&]()
		{
			if (Run.IsEmpty())
			{
				return;
			}
			FCanvasTextItem RunItem(FVector2D(FMath::RoundToFloat(RunX), FMath::RoundToFloat(Y)), FText::FromString(Run), F.Font, RunColor);
			RunItem.Scale = FVector2D(F.Scale, F.Scale);
			RunItem.BlendMode = SE_BLEND_Translucent;
			if (bShadow)
			{
				const float Off = FMath::Max(1.f, FMath::RoundToFloat(F.Px));
				RunItem.EnableShadow(FLinearColor(0.f, 0.f, 0.f, 0.8f * Color.A), FVector2D(Off, Off));
			}
			D.Canvas->DrawItem(RunItem);
			RunX += FontTextWidth(F.Font, Run, F.Scale) + float(F.Font->Kerning) * F.Scale;
			Run.Reset();
		};
		for (int32 i = 0; i < InText.Len(); ++i)
		{
			if (InText[i] == 0x1B)
			{
				Flush();
				if (i + 3 < InText.Len())
				{
					RunColor = FLinearColor(FColor(uint8(InText[i + 1] & 0xFF), uint8(InText[i + 2] & 0xFF), uint8(InText[i + 3] & 0xFF)));
					RunColor.A = Color.A;
				}
				i += 3;
				continue;
			}
			Run.AppendChar(InText[i]);
		}
		Flush();
		return W;
	}
	FCanvasTextItem Item(FVector2D(FMath::RoundToFloat(DX), FMath::RoundToFloat(Y)), FText::FromString(InText), F.Font, Color);
	Item.Scale = FVector2D(F.Scale, F.Scale);
	Item.BlendMode = SE_BLEND_Translucent;
	if (bShadow)
	{
		const float Off = FMath::Max(1.f, FMath::RoundToFloat(F.Px));
		Item.EnableShadow(FLinearColor(0.f, 0.f, 0.f, 0.8f * Color.A), FVector2D(Off, Off));
	}
	D.Canvas->DrawItem(Item);
	return W;
}

void FGUI::TextIn(const FFontRef& F, const FString& InText, const FRect& R, const FLinearColor& Color, EAlign Align, bool bShadow) const
{
	FString S = InText;
	if (TextSize(F, S).X > R.W && S.Len() > 3)
	{
		while (S.Len() > 1 && TextSize(F, S + TEXT("...")).X > R.W)
		{
			S.LeftChopInline(1);
		}
		S += TEXT("...");
	}
	const float Y = R.Y + (R.H - F.Height) * 0.5f;
	const float X = Align == EAlign::Left ? R.X : (Align == EAlign::Center ? R.X + R.W * 0.5f : R.R());
	Text(F, S, X, Y, Color, Align, bShadow);
}

TArray<FString> FGUI::Wrap(const FFontRef& F, const FString& InText, float Width) const
{
	TArray<FString> Lines;
	TArray<FString> Paras;
	InText.ParseIntoArray(Paras, TEXT("\n"), false);
	for (const FString& P : Paras)
	{
		TArray<FString> Words;
		P.ParseIntoArray(Words, TEXT(" "), true);
		FString Cur;
		for (FString Word : Words)
		{
			const FString Try = Cur.IsEmpty() ? Word : Cur + TEXT(" ") + Word;
			if (TextSize(F, Try).X <= Width)
			{
				Cur = Try;
				continue;
			}
			if (!Cur.IsEmpty())
			{
				Lines.Add(Cur);
				Cur.Reset();
			}
			while (TextSize(F, Word).X > Width && Word.Len() > 1)
			{
				int32 N = Word.Len() - 1;
				while (N > 1 && TextSize(F, Word.Left(N)).X > Width)
				{
					N = FMath::Max(1, N - FMath::Max(1, N / 10));
				}
				Lines.Add(Word.Left(N));
				Word.RightChopInline(N);
			}
			Cur = Word;
		}
		Lines.Add(Cur);
	}
	return Lines;
}

FFontRef FGUI::StyleFont(const FStyle& S, EMenuState State, EFontScale Scale) const
{
	FString Key = S.Font(Scale, State);
	if (Key.IsEmpty())
	{
		Key = S.Font(Scale, EMenuState::Blurry);
	}
	return Font(Key.IsEmpty() ? FString(TEXT("UT2MenuFont")) : Key);
}

void FGUI::StyleText(const FStyle& S, EMenuState State, EFontScale Scale, const FString& InText, const FRect& R, EAlign Align, float Alpha, bool bShadow) const
{
	const FFontRef F = StyleFont(S, State, Scale);
	const FRect In = R.Inset(L(S.BorderOffsets[0]), 0.f, L(S.BorderOffsets[2]), 0.f);
	TextIn(F, InText, In, ToLinear(S.FontColors[int32(State)], Alpha), Align, bShadow);
}

bool FGUI::ImageNormal(const FString& A, const FString& B, float BlendB, const FRect& R) const
{
	const FTexture* ResA = ResourceOf(LoadTex(A));
	if (!ResA || !D.Canvas)
	{
		return false;
	}
	if (R.W <= 0.f || R.H <= 0.f)
	{
		return true;
	}
	const FTexture* ResB = B.IsEmpty() ? nullptr : ResourceOf(LoadTex(B));
	const float WB = ResB ? FMath::Clamp(BlendB, 0.f, 1.f) : 0.f;
	// A opaque, weighted (1 - b); B added on top weighted b: A (1 - b) + B b without any texture alpha
	const float WA = 1.f - WB;
	FCanvasTileItem ItemA(FVector2D(R.X, R.Y), ResA, FVector2D(R.W, R.H), FVector2D(0.f, 0.f), FVector2D(1.f, 1.f), FLinearColor(WA, WA, WA, 1.f));
	ItemA.BlendMode = SE_BLEND_Opaque;
	D.Canvas->DrawItem(ItemA);
	if (ResB && WB > 0.f)
	{
		FCanvasTileItem ItemB(FVector2D(R.X, R.Y), ResB, FVector2D(R.W, R.H), FVector2D(0.f, 0.f), FVector2D(1.f, 1.f), FLinearColor(WB, WB, WB, 1.f));
		ItemB.BlendMode = SE_BLEND_Additive;
		D.Canvas->DrawItem(ItemB);
	}
	return true;
}

bool FGUI::Image(const FString& ObjectPath, const FRect& R, EImgStyle Style, const FLinearColor& Color, const FBox2f* SubRect, bool bAdditive) const
{
	UTexture2D* Tex = LoadTex(ObjectPath);
	const FTexture* Res = ResourceOf(Tex);
	if (!Res || !D.Canvas)
	{
		return false;
	}
	if (R.W <= 0.f || R.H <= 0.f || Color.A <= 0.f)
	{
		return true;
	}
	const float TW = float(FMath::Max(1, Tex->GetSizeX())), TH = float(FMath::Max(1, Tex->GetSizeY()));
	// UE2 material sizes: textures exported at full size; the texture's own pixels are the UE2 "material" pixels
	float SX1 = 0.f, SY1 = 0.f, SX2 = TW, SY2 = TH;
	if (SubRect && SubRect->Min.X >= 0.f && SubRect->Max.X > SubRect->Min.X)
	{
		SX1 = SubRect->Min.X; SY1 = SubRect->Min.Y; SX2 = SubRect->Max.X; SY2 = SubRect->Max.Y;
	}
	const float SW = SX2 - SX1, SH = SY2 - SY1;
	auto UV = [TW, TH](float X, float Y) { return FVector2D(X / TW, Y / TH); };
	UCanvas* C = D.Canvas;
	switch (Style)
	{
	case EImgStyle::Scaled:
	case EImgStyle::Bound:
		TileUV(C, Res, R.X, R.Y, R.W, R.H, UV(SX1, SY1), UV(SX2, SY2), Color, bAdditive);
		break;
	case EImgStyle::Justified:
	{
		const float S = FMath::Min(R.W / SW, R.H / SH);
		const float W = SW * S, H = SH * S;
		TileUV(C, Res, R.X + (R.W - W) * 0.5f, R.Y + (R.H - H) * 0.5f, W, H, UV(SX1, SY1), UV(SX2, SY2), Color, bAdditive);
		break;
	}
	case EImgStyle::Normal:
	{
		const float W = FMath::Min(R.W, L(SW)), H = FMath::Min(R.H, L(SH));
		TileUV(C, Res, R.X, R.Y, W, H, UV(SX1, SY1), UV(SX1 + W / LScale, SY1 + H / LScale), Color, bAdditive);
		break;
	}
	case EImgStyle::Tiled:
		TileUV(C, Res, R.X, R.Y, R.W, R.H, UV(SX1, SY1), UV(SX1 + R.W / L(SW) * SW, SY1 + R.H / L(SH) * SH), Color, bAdditive);
		break;
	case EImgStyle::PartialScaled:
	{
		// Canvas.DrawTilePartialStretched: the left and right halves keep their width (scaled with the page), the
		// middle column stretches; vertically the image is scaled to the height
		const float Cap = FMath::Min(L(SW * 0.5f), R.W * 0.5f);
		const float SrcCap = SW * 0.5f;
		TileUV(C, Res, R.X, R.Y, Cap, R.H, UV(SX1 + 0.5f, SY1), UV(SX1 + SrcCap, SY2), Color, bAdditive);
		if (R.W > Cap * 2.f)
		{
			TileUV(C, Res, R.X + Cap, R.Y, R.W - Cap * 2.f, R.H, UV(SX1 + SrcCap - 0.5f, SY1), UV(SX1 + SrcCap + 0.5f, SY2), Color, bAdditive);
		}
		TileUV(C, Res, R.R() - Cap, R.Y, Cap, R.H, UV(SX2 - SrcCap, SY1), UV(SX2 - 0.5f, SY2), Color, bAdditive);
		break;
	}
	case EImgStyle::Stretched:
	default:
	{
		// Canvas.DrawTileStretched: the four corners (half the image each) keep their size (scaled with the page),
		// the centre lines stretch, so borders stay crisp at any size
		const float CW = FMath::Min(L(SW * 0.5f), R.W * 0.5f);
		const float CH = FMath::Min(L(SH * 0.5f), R.H * 0.5f);
		const float SCW = SW * 0.5f, SCH = SH * 0.5f;
		const float DX[4] = { R.X, R.X + CW, R.R() - CW, R.R() };
		const float DY[4] = { R.Y, R.Y + CH, R.B() - CH, R.B() };
		const float UX[4] = { SX1 + 0.25f, SX1 + SCW - 0.5f, SX1 + SCW + 0.5f, SX2 - 0.25f };
		const float VY[4] = { SY1 + 0.25f, SY1 + SCH - 0.5f, SY1 + SCH + 0.5f, SY2 - 0.25f };
		for (int32 Row = 0; Row < 3; ++Row)
		{
			for (int32 Col = 0; Col < 3; ++Col)
			{
				const float PW = DX[Col + 1] - DX[Col], PH = DY[Row + 1] - DY[Row];
				if (PW > 0.01f && PH > 0.01f)
				{
					TileUV(C, Res, DX[Col], DY[Row], PW, PH, UV(UX[Col], VY[Row]), UV(UX[Col + 1], VY[Row + 1]), Color, bAdditive);
				}
			}
		}
		break;
	}
	}
	return true;
}

bool FGUI::ImageUV(const FString& ObjectPath, const FRect& R, float U0, float V0, float U1, float V1, const FLinearColor& Color, bool bAdditive) const
{
	const FTexture* Res = ResourceOf(LoadTex(ObjectPath));
	if (!Res || !D.Canvas)
	{
		return false;
	}
	TileUV(D.Canvas, Res, R.X, R.Y, R.W, R.H, FVector2D(U0, V0), FVector2D(U1, V1), Color, bAdditive);
	return true;
}

bool FGUI::StyleImage(const FStyle& S, EMenuState State, const FRect& R, float Alpha) const
{
	const int32 I = int32(State);
	if (S.Images[I].IsEmpty())
	{
		return false;
	}
	FRect Box = R;
	if (S.ImgWidths[I] > 0.f && S.ImgHeights[I] > 0.f)
	{
		// fixed image size (slider knob, scroll grip), centred
		const float W = FMath::Min(R.W, L(S.ImgWidths[I])), H = FMath::Min(R.H, L(S.ImgHeights[I]));
		Box = FRect(R.X + (R.W - W) * 0.5f, R.Y + (R.H - H) * 0.5f, W, H);
	}
	return Image(S.Images[I], Box, S.ImgStyle[I], ToLinear(S.ImgColors[I], Alpha), nullptr, S.RStyles[I] == 3);
}

void FGUI::FallbackBox(const FRect& R, EMenuState State, bool bButton, float Alpha) const
{
	// flat stand-in in the GUI2K4 palette (deep blue boxes, light blue frame, bright when focused)
	const bool bHot = State == EMenuState::Focused || State == EMenuState::Pressed;
	const bool bWatched = State == EMenuState::Watched;
	FLinearColor Fill = FArenaDraw::UTColor(FColor(23, 40, 100), 0.85f * Alpha);
	if (bButton && bHot) Fill = FArenaDraw::UTColor(FColor(40, 110, 200), 0.95f * Alpha);
	else if (bButton && bWatched) Fill = FArenaDraw::UTColor(FColor(30, 75, 160), 0.9f * Alpha);
	else if (State == EMenuState::Disabled) Fill = FArenaDraw::UTColor(FColor(30, 34, 52), 0.7f * Alpha);
	D.Rect(R.X, R.Y, R.W, R.H, Fill);
	D.Outline(R.X, R.Y, R.W, R.H, FArenaDraw::UTColor(bHot ? FColor(150, 220, 255) : FColor(80, 140, 220), Alpha), FMath::Max(1.f, FMath::RoundToFloat(L(1.f))));
}

// =================================================================================================
// FGUI: page furniture
// =================================================================================================

void FGUI::PageBackground(float Alpha) const
{
	const float W = D.W(), H = D.H();
	// UT2k4MainPage.PageBackground: BkRenders.Bgndtile (X1..Y2 0,0,4,768), the dark blue gradient, across the screen
	const FBox2f Strip(FVector2f(0.f, 0.f), FVector2f(4.f, 768.f));
	if (!Image(TEXT("2K4Menus.BkRenders.Bgndtile"), Screen(), EImgStyle::Scaled, FLinearColor(1.f, 1.f, 1.f, Alpha), &Strip))
	{
		D.VerticalGradient(0.f, 0.f, W, H, FArenaDraw::UTColor(FColor(0, 8, 40), Alpha), FArenaDraw::UTColor(FColor(0, 40, 110), Alpha), 24);
	}
	// BkChar: BkRenders.Char01 (1024 x 768) over the page, scaled
	const FBox2f Full(FVector2f(0.f, 0.f), FVector2f(1024.f, 768.f));
	Image(TEXT("2K4Menus.BkRenders.Char01"), Page(), EImgStyle::Scaled, FLinearColor(1.f, 1.f, 1.f, Alpha), &Full);
	// PageScanLine: BkRenders.Scanlines (32 x 32) tiled, alpha 32
	const FBox2f Lines(FVector2f(0.f, 0.f), FVector2f(32.f, 32.f));
	Image(TEXT("2K4Menus.BkRenders.Scanlines"), Screen(), EImgStyle::Tiled, FArenaDraw::UTColor(FColor(255, 255, 255, 32), Alpha), &Lines);
}

FRect FGUI::Header(const FString& Caption) const
{
	// GUIHeader: WinHeight 25 (UE2 pixels), full width, STY2Header (NewHeader), caption centred (GUIBorder Justification 1)
	const FStyle& S = GetStyle(TEXT("Header"));
	const FRect R(0.f, 0.f, D.W(), FMath::RoundToFloat(L(25.f)));
	if (!StyleImage(S, EMenuState::Blurry, R))
	{
		D.VerticalGradient(R.X, R.Y, R.W, R.H, FArenaDraw::UTColor(FColor(40, 90, 190)), FArenaDraw::UTColor(FColor(12, 30, 90)), 6);
		D.Rect(R.X, R.B() - 1.f, R.W, FMath::Max(1.f, L(1.f)), FArenaDraw::UTColor(FColor(110, 170, 255)));
	}
	StyleText(S, EMenuState::Blurry, EFontScale::Small, Caption, R.Inset(0.f, L(S.BorderOffsets[1]) * 0.f, 0.f, 0.f), EAlign::Center);
	return R;
}

FRect FGUI::Footer(const FString& InHint) const
{
	// ButtonFooter: WinHeight 0.04, at the bottom, STY2Footer (NewFooter); the status text left (Justification 0, TextIndent 5)
	const FStyle& S = GetStyle(TEXT("Footer"));
	const float H = FMath::RoundToFloat(D.H() * 0.04f);
	const FRect R(0.f, D.H() - H, D.W(), H);
	if (!StyleImage(S, EMenuState::Blurry, R))
	{
		D.VerticalGradient(R.X, R.Y, R.W, R.H, FArenaDraw::UTColor(FColor(12, 30, 90)), FArenaDraw::UTColor(FColor(6, 16, 56)), 6);
		D.Rect(R.X, R.Y, R.W, FMath::Max(1.f, L(1.f)), FArenaDraw::UTColor(FColor(80, 140, 220)));
	}
	if (!InHint.IsEmpty())
	{
		StyleText(S, EMenuState::Blurry, EFontScale::Small, InHint, FRect(SafeX + L(5.f), R.Y, SafeW * 0.55f, R.H), EAlign::Left);
	}
	return R;
}

FRect FGUI::Section(const FRect& R, const FString& Caption, bool bAlt) const
{
	// GUISectionBackground: HeaderBase NewControls.Display98, ImageOffset 16 / 44 / 16 / 18, caption TextLabel small top left;
	// AltSectionBackground: Display99, ImageOffset 16 / 32 / 16 / 32, caption centred at AltCaptionOffset 40 / 8 / 40 / 25
	const FString Tex = bAlt ? TEXT("2K4Menus.NewControls.Display99") : TEXT("2K4Menus.NewControls.Display98");
	if (!Image(Tex, R, EImgStyle::Stretched, FLinearColor::White))
	{
		D.Rect(R.X, R.Y, R.W, R.H, FArenaDraw::UTColor(FColor(30, 20, 90), 0.55f));
		D.Outline(R.X, R.Y, R.W, R.H, FArenaDraw::UTColor(FColor(90, 160, 255)), FMath::Max(1.f, FMath::RoundToFloat(L(2.f))));
		if (!bAlt)
		{
			D.Rect(R.X, R.Y, R.W, FMath::Min(R.H, L(36.f)), FArenaDraw::UTColor(FColor(60, 130, 230), 0.5f));
		}
	}
	const FStyle& S = GetStyle(TEXT("TextLabel"));
	if (!Caption.IsEmpty())
	{
		if (bAlt)
		{
			StyleText(S, EMenuState::Blurry, EFontScale::Medium, Caption, FRect(R.X + L(40.f), R.Y + L(8.f), R.W - L(80.f), L(17.f)), EAlign::Center);
		}
		else
		{
			StyleText(S, EMenuState::Blurry, EFontScale::Small, Caption, FRect(R.X + L(20.f), R.Y + L(6.f), R.W - L(40.f), L(26.f)), EAlign::Left);
		}
	}
	return bAlt ? R.Inset(L(16.f), L(32.f), L(16.f), L(32.f)) : R.Inset(L(16.f), L(44.f), L(16.f), L(18.f));
}

void FGUI::Label(const FRect& R, const FString& InText, const FString& StyleKey, EAlign Align, EFontScale Scale, EMenuState State) const
{
	const FStyle& S = GetStyle(StyleKey);
	const FFontRef F = StyleFont(S, State, Scale);
	TextIn(F, InText, R, ToLinear(S.FontColors[int32(State)]), Align);
}

void FGUI::ScrollText(uint32 InId, const FRect& R, const FString& InText, const FString& FontKey, const FLinearColor& Color)
{
	const FFontRef F = Font(FontKey);
	const TArray<FString> Lines = Wrap(F, InText, R.W - L(6.f));
	const int32 Visible = FMath::Max(1, FMath::FloorToInt(R.H / F.Height));
	float& First = Scrolls.FindOrAdd(InId);
	if (bCapture && Hovered(R) && Wheel != 0)
	{
		First -= Wheel * 2.f;
	}
	First = FMath::Clamp(First, 0.f, float(FMath::Max(0, Lines.Num() - Visible)));
	const FLinearColor C = Color.A > 0.f ? Color : ToLinear(GetStyle(TEXT("NoBackground")).FontColors[0]);
	float Y = R.Y;
	for (int32 i = int32(First); i < Lines.Num() && i < int32(First) + Visible; ++i)
	{
		Text(F, Lines[i], R.X + L(3.f), Y, C);
		Y += F.Height;
	}
}

// =================================================================================================
// FGUI: interaction
// =================================================================================================

bool FGUI::Hovered(const FRect& R) const
{
	if (!bCapture || !R.Contains(MousePos))
	{
		return false;
	}
	// an open combo list covers what is below it
	return !(PrevPopupRect.W > 0.f && PrevPopupRect.Contains(MousePos));
}

EMenuState FGUI::Interact(uint32 InId, const FRect& R, bool bEnabled, bool bFocusable, const FString& InHint, bool bFocusOnHover)
{
	const bool bHover = Hovered(R) && (CaptureId == 0 || CaptureId == InId);
	if (bEnabled && bFocusable)
	{
		FocusChain.Add({ InId, R, bEnabled });
	}
	if (bHover)
	{
		HotId = InId;
		if (!InHint.IsEmpty())
		{
			Hint = InHint;
		}
	}
	if (!bEnabled)
	{
		return EMenuState::Disabled;
	}
	if (bHover && bFocusOnHover && LastHot != InId && bFocusable)
	{
		Focused = InId;
	}
	if (bHover && bMousePressed)
	{
		PressedId = InId;
		if (bFocusable)
		{
			Focused = InId;
		}
		if (PopupId != 0 && PopupId != InId)
		{
			const_cast<FGUI*>(this)->PopupId = 0;
		}
	}
	if (Focused == InId && Hint.IsEmpty())
	{
		Hint = InHint;
	}
	if (PressedId == InId && bMouseDown && bHover)
	{
		return EMenuState::Pressed;
	}
	if (Focused == InId)
	{
		return EMenuState::Focused;
	}
	return bHover ? EMenuState::Watched : EMenuState::Blurry;
}

void FGUI::PlaySound(ESound Sound) const
{
	// UT2K4GUIController: MouseOverSound, ClickSound, EditSound, UpSound, DownSound, DragSound, FadeSound (2K4MenuSounds.Generic)
	const TCHAR* Path = nullptr;
	switch (Sound)
	{
	case ESound::Click: Path = TEXT("2K4MenuSounds.Generic.msfxMouseClick"); break;
	case ESound::Edit:  Path = TEXT("2K4MenuSounds.Generic.msfxEdit"); break;
	case ESound::Up:    Path = TEXT("2K4MenuSounds.Generic.msfxUp"); break;
	case ESound::Down:  Path = TEXT("2K4MenuSounds.Generic.msfxDown"); break;
	case ESound::Drag:  Path = TEXT("2K4MenuSounds.Generic.msfxDrag"); break;
	case ESound::Fade:  Path = TEXT("2K4MenuSounds.Generic.msfxFade"); break;
	case ESound::Over:  Path = TEXT("2K4MenuSounds.Generic.msfxMouseOver"); break;
	default: return;
	}
	APlayerController* P = PC.Get();
	if (!P)
	{
		return;
	}
	USoundBase* Snd = ArenaRuntimeContent::IsActive() ? ArenaRuntimeContent::LoadSoundByObjectPath(Path) : nullptr;
	if (Snd)
	{
		UGameplayStatics::PlaySound2D(P, Snd, Sound == ESound::Over ? 0.5f : 0.8f);
	}
	else if (Sound != ESound::Over)
	{
		ArenaAssets::PlaySound2D(P, Sound == ESound::Click ? FName(TEXT("S_MenuSelect")) : FName(TEXT("S_MenuMove")), 0.6f);
	}
}

float FGUI::CaptionWidthFor(const FRect& R, const FString& Caption, float CaptionWidth) const
{
	// GUIMenuOption.bAutoSizeCaption (default True): a caption longer than CaptionWidth widens it
	if (Caption.IsEmpty() || R.W <= 0.f)
	{
		return CaptionWidth;
	}
	const FFontRef F = StyleFont(GetStyle(TEXT("TextLabel")), EMenuState::Blurry, EFontScale::Medium);
	const float Needed = (TextSize(F, Caption).X + L(10.f)) / R.W;
	return FMath::Clamp(FMath::Max(CaptionWidth, Needed), CaptionWidth, FMath::Max(CaptionWidth, 0.7f));
}

bool FGUI::WasClicked(uint32 InId, const FRect& R)
{
	if (bMouseReleased && PressedId == InId && Hovered(R))
	{
		return true;
	}
	if (Focused == InId && AnyKey({ EKeys::Enter, EKeys::SpaceBar, EKeys::Gamepad_FaceButton_Bottom }))
	{
		ConsumeKey(EKeys::Enter);
		ConsumeKey(EKeys::SpaceBar);
		ConsumeKey(EKeys::Gamepad_FaceButton_Bottom);
		return true;
	}
	return false;
}

void FGUI::CaptionLabel(const FRect& R, const FString& Caption, EMenuState State) const
{
	// GUIMenuOption label: LabelStyleName "TextLabel", medium font, left justified, follows the option's state
	if (Caption.IsEmpty())
	{
		return;
	}
	const FStyle& S = GetStyle(TEXT("TextLabel"));
	const FFontRef F = StyleFont(S, State, EFontScale::Medium);
	TextIn(F, Caption, R, ToLinear(S.FontColors[int32(State)]), EAlign::Left);
}

bool FGUI::Button(uint32 InId, const FRect& R, const FString& StyleKey, const FString& Caption, const FString& InHint, bool bEnabled,
	EFontScale Scale, ESound Sound, bool bFocusable)
{
	const EMenuState State = Interact(InId, R, bEnabled, bFocusable, InHint);
	const FStyle& S = GetStyle(StyleKey);
	if (!StyleImage(S, State, R) && !S.Images[0].IsEmpty())
	{
		FallbackBox(R, State, true);
	}
	if (!Caption.IsEmpty())
	{
		StyleText(S, State, Scale, Caption, R, EAlign::Center);
	}
	bool bClicked = false;
	if (bEnabled)
	{
		if (bMouseReleased && PressedId == InId && Hovered(R))
		{
			bClicked = true;
		}
		if (Focused == InId && AnyKey({ EKeys::Enter, EKeys::SpaceBar, EKeys::Gamepad_FaceButton_Bottom }))
		{
			ConsumeKey(EKeys::Enter);
			ConsumeKey(EKeys::SpaceBar);
			ConsumeKey(EKeys::Gamepad_FaceButton_Bottom);
			bClicked = true;
		}
	}
	if (bClicked)
	{
		PlaySound(Sound);
	}
	return bClicked;
}

bool FGUI::CheckBox(uint32 InId, const FRect& R, const FString& Caption, bool& bValue, const FString& InHint, float CaptionWidth, bool bEnabled)
{
	CaptionWidth = CaptionWidthFor(R, Caption, CaptionWidth);
	const EMenuState State = Interact(InId, R, bEnabled, true, InHint);
	CaptionLabel(FRect(R.X, R.Y, R.W * CaptionWidth, R.H), Caption, State);
	// GUICheckBoxButton (bSquare): a square at the right end; STY2CheckBox frame, Controls.checkBoxX_* when checked
	const float Size = R.H;
	const FRect Box(R.R() - Size, R.Y, Size, Size);
	const FStyle& S = GetStyle(TEXT("CheckBox"));
	if (!StyleImage(S, State, Box))
	{
		FallbackBox(Box, State, false);
	}
	if (bValue)
	{
		static const TCHAR* Checked[5] = { TEXT("2K4Menus.Controls.checkBoxX_b"), TEXT("2K4Menus.Controls.checkBoxX_w"), TEXT("2K4Menus.Controls.checkBoxX_f"),
			TEXT("2K4Menus.Controls.checkBoxX_p"), TEXT("2K4Menus.Controls.checkBoxX_d") };
		if (!Image(Checked[int32(State)], Box, EImgStyle::Scaled, FLinearColor::White))
		{
			const float I = Size * 0.25f;
			D.Line(Box.X + I, Box.Y + I, Box.R() - I, Box.B() - I, FLinearColor(1.f, 0.85f, 0.2f, 1.f), FMath::Max(2.f, Size * 0.12f));
			D.Line(Box.R() - I, Box.Y + I, Box.X + I, Box.B() - I, FLinearColor(1.f, 0.85f, 0.2f, 1.f), FMath::Max(2.f, Size * 0.12f));
		}
	}
	bool bChanged = false;
	if (bEnabled)
	{
		if ((bMouseReleased && PressedId == InId && Hovered(R))
			|| (Focused == InId && AnyKey({ EKeys::Enter, EKeys::SpaceBar, EKeys::Gamepad_FaceButton_Bottom, EKeys::Left, EKeys::Right,
				EKeys::Gamepad_DPad_Left, EKeys::Gamepad_DPad_Right, EKeys::Gamepad_LeftStick_Left, EKeys::Gamepad_LeftStick_Right })))
		{
			bValue = !bValue;
			bChanged = true;
			PlaySound(ESound::Click);
		}
	}
	return bChanged;
}

bool FGUI::Combo(uint32 InId, const FRect& R, const FString& Caption, int32& Index, const TArray<FString>& Items, const FString& InHint,
	float CaptionWidth, bool bEnabled)
{
	CaptionWidth = CaptionWidthFor(R, Caption, CaptionWidth);
	const EMenuState State = Interact(InId, R, bEnabled, true, InHint);
	const FRect Comp = Caption.IsEmpty() ? R : FRect(R.X + R.W * CaptionWidth, R.Y, R.W * (1.f - CaptionWidth), R.H);
	CaptionLabel(FRect(R.X, R.Y, R.W * CaptionWidth, R.H), Caption, State);
	// GUIComboBox: GUIEditBox (STY2EditBox) with the value, ShowList button (STY2ComboButton, square at the right)
	const FStyle& Edit = GetStyle(TEXT("EditBox"));
	if (!StyleImage(Edit, State, Comp))
	{
		FallbackBox(Comp, State, false);
	}
	const FRect Btn(Comp.R() - Comp.H, Comp.Y, Comp.H, Comp.H);
	const FStyle& BS = GetStyle(TEXT("ComboButton"));
	const EMenuState BtnState = !bEnabled ? EMenuState::Disabled : (PopupId == InId ? EMenuState::Pressed : (Hovered(Btn) ? EMenuState::Watched : EMenuState::Blurry));
	if (!StyleImage(BS, BtnState, Btn))
	{
		FallbackBox(Btn.Inset(2.f, 2.f, 2.f, 2.f), BtnState, true);
		D.TextMid(TEXT("v"), Btn.X + Btn.W * 0.5f, Btn.Y + Btn.H * 0.5f, FLinearColor::White, Btn.H * 0.6f, EArenaAlign::Center);
	}
	const FString Value = Items.IsValidIndex(Index) ? Items[Index] : FString();
	StyleText(Edit, State == EMenuState::Disabled ? EMenuState::Disabled : EMenuState::Blurry, EFontScale::Medium, Value,
		FRect(Comp.X, Comp.Y, Comp.W - Comp.H, Comp.H), EAlign::Left);

	bool bChanged = false;
	if (PopupResultId == InId)
	{
		PopupResultId = 0;
		if (Items.IsValidIndex(PopupResult) && PopupResult != Index)
		{
			Index = PopupResult;
			bChanged = true;
		}
	}
	if (!bEnabled || Items.Num() == 0)
	{
		return bChanged;
	}
	auto OpenList = [&]()
	{
		PopupId = InId;
		PopupItems = Items;
		PopupHover = FMath::Clamp(Index, 0, Items.Num() - 1);
		const int32 Visible = FMath::Min(8, Items.Num()); // GUIComboBox.MaxVisibleItems
		const FFontRef F = StyleFont(GetStyle(TEXT("ComboListBox")), EMenuState::Blurry, EFontScale::Medium);
		const float RowH = FMath::RoundToFloat(F.Height * 1.1f);
		PopupRect = FRect(Comp.X, Comp.B(), Comp.W, RowH * Visible + L(6.f));
		if (PopupRect.B() > D.H())
		{
			PopupRect.Y = Comp.Y - PopupRect.H;
		}
		PopupScroll = FMath::Clamp(PopupHover - Visible / 2, 0, FMath::Max(0, Items.Num() - Visible));
		PlaySound(ESound::Click);
	};
	if (PopupId != InId)
	{
		if (bMouseReleased && PressedId == InId && Hovered(Comp))
		{
			OpenList();
		}
		else if (Focused == InId && AnyKey({ EKeys::Enter, EKeys::SpaceBar, EKeys::Gamepad_FaceButton_Bottom }))
		{
			ConsumeKey(EKeys::Enter);
			ConsumeKey(EKeys::SpaceBar);
			ConsumeKey(EKeys::Gamepad_FaceButton_Bottom);
			OpenList();
		}
		else if (Focused == InId)
		{
			int32 Dir = 0;
			if (AnyKey({ EKeys::Left, EKeys::Gamepad_DPad_Left, EKeys::Gamepad_LeftStick_Left })) Dir = -1;
			if (AnyKey({ EKeys::Right, EKeys::Gamepad_DPad_Right, EKeys::Gamepad_LeftStick_Right })) Dir = +1;
			if (Dir != 0)
			{
				const int32 New = FMath::Clamp(Index + Dir, 0, Items.Num() - 1);
				if (New != Index)
				{
					Index = New;
					bChanged = true;
					PlaySound(ESound::Click);
				}
			}
		}
	}
	else
	{
		// the open list takes the keys
		PopupItems = Items;
	}
	return bChanged;
}

void FGUI::DrawPopup()
{
	if (PopupId == 0)
	{
		return;
	}
	const int32 Num = PopupItems.Num();
	if (Num == 0)
	{
		PopupId = 0;
		return;
	}
	const FStyle& S = GetStyle(TEXT("ComboListBox"));
	const FFontRef F = StyleFont(S, EMenuState::Blurry, EFontScale::Medium);
	const float RowH = FMath::RoundToFloat(F.Height * 1.1f);
	const int32 Visible = FMath::Min(8, Num);
	if (!StyleImage(S, EMenuState::Blurry, PopupRect))
	{
		D.Rect(PopupRect.X, PopupRect.Y, PopupRect.W, PopupRect.H, FArenaDraw::UTColor(FColor(10, 24, 70), 0.97f));
		D.Outline(PopupRect.X, PopupRect.Y, PopupRect.W, PopupRect.H, FArenaDraw::UTColor(FColor(90, 160, 255)), 1.f);
	}
	// keys
	if (AnyKey({ EKeys::Up, EKeys::Gamepad_DPad_Up, EKeys::Gamepad_LeftStick_Up })) PopupHover = FMath::Max(0, PopupHover - 1);
	if (AnyKey({ EKeys::Down, EKeys::Gamepad_DPad_Down, EKeys::Gamepad_LeftStick_Down })) PopupHover = FMath::Min(Num - 1, PopupHover + 1);
	Keys.Remove(EKeys::Up); Keys.Remove(EKeys::Down); Keys.Remove(EKeys::Gamepad_DPad_Up); Keys.Remove(EKeys::Gamepad_DPad_Down);
	Keys.Remove(EKeys::Gamepad_LeftStick_Up); Keys.Remove(EKeys::Gamepad_LeftStick_Down); Keys.Remove(EKeys::Tab);
	if (PopupRect.Contains(MousePos) && Wheel != 0)
	{
		PopupScroll -= Wheel;
	}
	if (PopupHover < PopupScroll) PopupScroll = PopupHover;
	if (PopupHover >= PopupScroll + Visible) PopupScroll = PopupHover - Visible + 1;
	PopupScroll = FMath::Clamp(PopupScroll, 0, FMath::Max(0, Num - Visible));

	const FStyle& Sel = GetStyle(TEXT("ListSelection"));
	const float Top = PopupRect.Y + L(3.f);
	for (int32 i = 0; i < Visible; ++i)
	{
		const int32 Item = PopupScroll + i;
		const FRect Row(PopupRect.X + L(3.f), Top + i * RowH, PopupRect.W - L(6.f), RowH);
		if (Row.Contains(MousePos) && (MousePos != FVector2D::ZeroVector))
		{
			PopupHover = Item;
		}
		const bool bSel = Item == PopupHover;
		if (bSel)
		{
			D.Rect(Row.X, Row.Y, Row.W, Row.H, ToLinear(Sel.ImgColors[int32(EMenuState::Focused)]));
		}
		TextIn(F, PopupItems[Item], Row.Inset(L(S.BorderOffsets[0]), 0.f, 0.f, 0.f), ToLinear(bSel ? Sel.FontColors[2] : S.FontColors[0]), EAlign::Left);
	}
	bool bClose = false;
	if (bMousePressed && !PopupRect.Contains(MousePos))
	{
		bClose = true; // clicking outside closes (the click still reaches what is below)
	}
	else if (bMouseReleased && PopupRect.Contains(MousePos) && PressedId != PopupId)
	{
		PopupResult = PopupHover;
		PopupResultId = PopupId;
		bClose = true;
		PlaySound(ESound::Click);
	}
	if (AnyKey({ EKeys::Enter, EKeys::SpaceBar, EKeys::Gamepad_FaceButton_Bottom }))
	{
		PopupResult = PopupHover;
		PopupResultId = PopupId;
		bClose = true;
		Keys.Remove(EKeys::Enter);
		PlaySound(ESound::Click);
	}
	if (AnyKey({ EKeys::Escape, EKeys::Gamepad_FaceButton_Right }))
	{
		bClose = true;
		Keys.Remove(EKeys::Escape);
		Keys.Remove(EKeys::Gamepad_FaceButton_Right);
	}
	if (bClose)
	{
		PopupId = 0;
	}
}

bool FGUI::Slider(uint32 InId, const FRect& R, const FString& Caption, float& Value, float Min, float Max, float Step, const FString& ValueText,
	const FString& InHint, bool& bOutReleased, float CaptionWidth, bool bEnabled)
{
	CaptionWidth = CaptionWidthFor(R, Caption, CaptionWidth);
	bOutReleased = false;
	const EMenuState State = Interact(InId, R, bEnabled, true, InHint);
	CaptionLabel(FRect(R.X, R.Y, R.W * CaptionWidth, R.H), Caption, State);
	const FRect Comp(R.X + R.W * CaptionWidth, R.Y, R.W * (1.f - CaptionWidth), R.H);
	// GUISlider: STY2SliderBar (NewSliderBar) across, FillImage SliderFill* up to the value, STY2SliderKnob 26 x 13,
	// the value as caption (STY2SliderCaption) in the middle of the bar
	const FRect Bar = Comp.Inset(0.f, R.H * 0.2f, 0.f, R.H * 0.2f);
	const float Frac = Max > Min ? FMath::Clamp((Value - Min) / (Max - Min), 0.f, 1.f) : 0.f;
	if (!StyleImage(GetStyle(TEXT("SliderBar")), State, Bar))
	{
		D.Rect(Bar.X, Bar.Y, Bar.W, Bar.H, FArenaDraw::UTColor(FColor(10, 40, 100), 0.9f));
	}
	static const TCHAR* Fills[5] = { TEXT("2K4Menus.NewControls.SliderFillBlurry"), TEXT("2K4Menus.NewControls.SliderFillWatched"),
		TEXT("2K4Menus.NewControls.SliderFillFocused"), TEXT("2K4Menus.NewControls.SliderFillPressed"), TEXT("2K4Menus.NewControls.SliderFillDisabled") };
	const FRect Fill(Bar.X, Bar.Y, Bar.W * Frac, Bar.H);
	if (Fill.W > 0.5f && !Image(Fills[int32(State)], Fill, EImgStyle::Stretched, FLinearColor::White))
	{
		D.Rect(Fill.X, Fill.Y, Fill.W, Fill.H, FArenaDraw::UTColor(FColor(230, 170, 30), 0.9f));
	}
	const FStyle& Knob = GetStyle(TEXT("SliderKnob"));
	const float KW = L(26.f), KH = FMath::Min(R.H, L(13.f) * 1.3f);
	const FRect KR(Bar.X + Bar.W * Frac - KW * 0.5f, R.Y + (R.H - KH) * 0.5f, KW, KH);
	if (!StyleImage(Knob, State, KR))
	{
		FallbackBox(KR, State, true);
	}
	if (!ValueText.IsEmpty())
	{
		const FStyle& Cap = GetStyle(TEXT("SliderCaption"));
		const FFontRef F = StyleFont(Cap, EMenuState::Blurry, EFontScale::Small);
		TextIn(F, ValueText, Bar, FLinearColor::White, EAlign::Center, true);
	}
	if (!bEnabled)
	{
		return false;
	}
	bool bChanged = false;
	auto SetFrac = [&](float NewFrac)
	{
		float V = Min + FMath::Clamp(NewFrac, 0.f, 1.f) * (Max - Min);
		if (Step > 0.f)
		{
			V = Min + FMath::RoundToFloat((V - Min) / Step) * Step;
		}
		V = FMath::Clamp(V, Min, Max);
		if (!FMath::IsNearlyEqual(V, Value, Step > 0.f ? Step * 0.01f : 0.0001f))
		{
			Value = V;
			bChanged = true;
		}
	};
	if (bMousePressed && Hovered(Comp))
	{
		CaptureId = InId;
		PlaySound(ESound::Drag);
	}
	if (CaptureId == InId)
	{
		SetFrac((MousePos.X - Bar.X) / FMath::Max(1.f, Bar.W));
		if (!bMouseDown)
		{
			bOutReleased = true;
			CaptureId = 0;
		}
	}
	if (Focused == InId)
	{
		int32 Dir = 0;
		if (AnyKey({ EKeys::Left, EKeys::Gamepad_DPad_Left, EKeys::Gamepad_LeftStick_Left })) Dir = -1;
		if (AnyKey({ EKeys::Right, EKeys::Gamepad_DPad_Right, EKeys::Gamepad_LeftStick_Right })) Dir = +1;
		if (Dir != 0)
		{
			const float StepV = Step > 0.f ? Step : (Max - Min) / 20.f;
			const float Old = Value;
			Value = FMath::Clamp(Value + Dir * StepV, Min, Max);
			bChanged = !FMath::IsNearlyEqual(Old, Value);
			bOutReleased = bChanged;
			PlaySound(ESound::Click);
		}
	}
	return bChanged || bOutReleased;
}

int32 FGUI::Spinner(uint32 InId, const FRect& R, const FString& Caption, const FString& ValueText, const FString& InHint, float CaptionWidth, bool bEnabled)
{
	CaptionWidth = CaptionWidthFor(R, Caption, CaptionWidth);
	const EMenuState State = Interact(InId, R, bEnabled, true, InHint);
	CaptionLabel(FRect(R.X, R.Y, R.W * CaptionWidth, R.H), Caption, State);
	const FRect Comp(R.X + R.W * CaptionWidth, R.Y, R.W * (1.f - CaptionWidth), R.H);
	// GUINumericEdit: GUIEditBox + GUISpinnerButton (STY2Spinner: + over -) at the right
	const FStyle& Edit = GetStyle(TEXT("EditBox"));
	if (!StyleImage(Edit, State, Comp))
	{
		FallbackBox(Comp, State, false);
	}
	const float SW = Comp.H * 0.85f;
	const FRect Spin(Comp.R() - SW, Comp.Y, SW, Comp.H);
	const EMenuState SpinState = !bEnabled ? EMenuState::Disabled : (Hovered(Spin) ? (bMouseDown ? EMenuState::Pressed : EMenuState::Watched) : EMenuState::Blurry);
	if (!StyleImage(GetStyle(TEXT("SpinnerButton")), SpinState, Spin))
	{
		FallbackBox(Spin, SpinState, true);
		D.TextMid(TEXT("+"), Spin.X + Spin.W * 0.5f, Spin.Y + Spin.H * 0.27f, FLinearColor(1.f, 0.7f, 0.2f), Spin.H * 0.45f, EArenaAlign::Center);
		D.TextMid(TEXT("-"), Spin.X + Spin.W * 0.5f, Spin.Y + Spin.H * 0.73f, FLinearColor(1.f, 0.7f, 0.2f), Spin.H * 0.45f, EArenaAlign::Center);
	}
	StyleText(Edit, State == EMenuState::Disabled ? EMenuState::Disabled : EMenuState::Blurry, EFontScale::Medium, ValueText,
		FRect(Comp.X, Comp.Y, Comp.W - SW, Comp.H), EAlign::Left);
	if (!bEnabled)
	{
		return 0;
	}
	int32 Dir = 0;
	if (bMouseReleased && PressedId == InId && Hovered(Spin))
	{
		Dir = MousePos.Y < Spin.Y + Spin.H * 0.5f ? +1 : -1;
	}
	else if (bMouseReleased && PressedId == InId && Hovered(Comp))
	{
		Dir = +1;
	}
	if (Focused == InId)
	{
		if (AnyKey({ EKeys::Left, EKeys::Gamepad_DPad_Left, EKeys::Gamepad_LeftStick_Left })) Dir = -1;
		if (AnyKey({ EKeys::Right, EKeys::Gamepad_DPad_Right, EKeys::Gamepad_LeftStick_Right, EKeys::Enter, EKeys::Gamepad_FaceButton_Bottom })) Dir = +1;
		ConsumeKey(EKeys::Enter);
	}
	if (Hovered(R) && Wheel != 0 && Focused == InId)
	{
		Dir = Wheel > 0 ? +1 : -1;
	}
	if (Dir != 0)
	{
		PlaySound(Dir > 0 ? ESound::Up : ESound::Down);
	}
	return Dir;
}

void FGUI::BeginTextEdit(uint32 InId, const FString& Value, int32 MaxLen)
{
	if (EditingId == InId)
	{
		return;
	}
	EndTextEdit(true);
	EditingId = InId;
	EditText = Value;
	EditMaxLen = MaxLen;
	EditStart = Now;
	FArenaGUIInput::Get().bInternalText = true;
	if (FSlateApplication::IsInitialized() && GEngine && GEngine->GameViewport)
	{
		if (!TextSink.IsValid())
		{
			TextSink = SNew(SArenaGUITextSink);
			TextSinkHolder = SNew(SBox).HAlign(HAlign_Left).VAlign(VAlign_Top).WidthOverride(1.f).HeightOverride(1.f)[TextSink.ToSharedRef()];
			GEngine->GameViewport->AddViewportWidgetContent(TextSinkHolder.ToSharedRef(), 1000);
		}
		FSlateApplication::Get().SetAllUserFocus(TextSink, EFocusCause::SetDirectly);
	}
}

void FGUI::EndTextEdit(bool bCommit)
{
	if (EditingId == 0)
	{
		return;
	}
	if (bCommit)
	{
		CommittedId = EditingId;
	}
	EditingId = 0;
	FArenaGUIInput::Get().bInternalText = false;
	if (TextSink.IsValid() && FSlateApplication::IsInitialized() && GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(TextSinkHolder.ToSharedRef());
		FSlateApplication::Get().SetAllUserFocusToGameViewport();
	}
	TextSink.Reset();
	TextSinkHolder.Reset();
}

bool FGUI::EditBox(uint32 InId, const FRect& R, const FString& Caption, FString& Value, const FString& InHint, float CaptionWidth, int32 MaxLen)
{
	CaptionWidth = CaptionWidthFor(R, Caption, CaptionWidth);
	const bool bEditing = EditingId == InId;
	const EMenuState State = bEditing ? EMenuState::Focused : Interact(InId, R, true, true, InHint);
	if (bEditing)
	{
		FocusChain.Add({ InId, R, true });
	}
	CaptionLabel(FRect(R.X, R.Y, R.W * CaptionWidth, R.H), Caption, State);
	const FRect Comp(R.X + R.W * CaptionWidth, R.Y, R.W * (1.f - CaptionWidth), R.H);
	const FStyle& Edit = GetStyle(TEXT("EditBox"));
	if (!StyleImage(Edit, bEditing ? EMenuState::Pressed : State, Comp))
	{
		FallbackBox(Comp, State, false);
	}
	const FFontRef F = StyleFont(Edit, EMenuState::Blurry, EFontScale::Medium);
	const FRect TR = Comp.Inset(L(Edit.BorderOffsets[0]), 0.f, L(4.f), 0.f);
	const FString& Shown = bEditing ? EditText : Value;
	TextIn(F, Shown, TR, ToLinear(Edit.FontColors[int32(bEditing ? EMenuState::Focused : EMenuState::Blurry)]), EAlign::Left);
	if (bEditing && FMath::Fmod(Now - EditStart, 1.0) < 0.6)
	{
		const float CX = TR.X + FMath::Min(TR.W, TextSize(F, EditText).X) + 1.f;
		D.Rect(CX, TR.Y + TR.H * 0.18f, FMath::Max(1.f, L(1.5f)), TR.H * 0.64f, FLinearColor::White);
	}
	bool bCommitted = false;
	if (CommittedId == InId)
	{
		CommittedId = 0;
		const FString Clean = EditText.TrimStartAndEnd();
		if (!Clean.IsEmpty() && Clean != Value)
		{
			Value = Clean;
			bCommitted = true;
		}
	}
	if (!bEditing)
	{
		if ((bMouseReleased && PressedId == InId && Hovered(R)) || (Focused == InId && AnyKey({ EKeys::Enter, EKeys::Gamepad_FaceButton_Bottom })))
		{
			ConsumeKey(EKeys::Enter);
			BeginTextEdit(InId, Value, MaxLen);
			PlaySound(ESound::Edit);
		}
		return bCommitted;
	}
	// editing: characters and keys arrive through the focus widget
	EditStart = Chars.Num() > 0 || Keys.Num() > 0 ? Now : EditStart;
	for (const TCHAR C : Chars)
	{
		if (C >= 32 && C != 127 && EditText.Len() < EditMaxLen)
		{
			EditText.AppendChar(C);
		}
	}
	if (KeyPressed(EKeys::BackSpace) && EditText.Len() > 0)
	{
		EditText.LeftChopInline(1);
	}
	if (bCtrl && KeyPressed(EKeys::V))
	{
		FString Clip;
		FPlatformApplicationMisc::ClipboardPaste(Clip);
		Clip.ReplaceInline(TEXT("\r"), TEXT(""));
		Clip.ReplaceInline(TEXT("\n"), TEXT(""));
		EditText = (EditText + Clip).Left(EditMaxLen);
	}
	if (AnyKey({ EKeys::Enter, EKeys::Tab, EKeys::Gamepad_FaceButton_Bottom }) || (bMousePressed && !Hovered(R)))
	{
		EndTextEdit(true);
		PlaySound(ESound::Click);
	}
	else if (AnyKey({ EKeys::Escape, EKeys::Gamepad_FaceButton_Right }))
	{
		EndTextEdit(false);
	}
	Keys.Reset();
	return bCommitted;
}

float FGUI::ListRowHeight(const FString& FontKey, bool bLargeRows) const
{
	return FMath::RoundToFloat(Font(FontKey).Height * (bLargeRows ? 1.15f : 1.05f));
}

bool FGUI::List(uint32 InId, const FRect& R, const TArray<FListItem>& Items, int32& Selected, bool& bOutActivated, const FString& InHint,
	const FString& FontKey, bool bLargeRows, const TArray<FListColumn>* Columns)
{
	bOutActivated = false;
	const EMenuState State = Interact(InId, R, true, true, InHint);
	const FStyle& S = GetStyle(TEXT("NoBackground"));
	const FStyle& Sel = GetStyle(TEXT("ListSelection"));
	const FStyle& SecS = GetStyle(TEXT("ListSection"));
	const FFontRef F = Font(FontKey);
	const float RowH = FMath::RoundToFloat(F.Height * (bLargeRows ? 1.15f : 1.05f));
	const int32 Num = Items.Num();
	const int32 Visible = FMath::Max(1, FMath::FloorToInt(R.H / RowH));
	const bool bScrollBar = Num > Visible;
	// GUIVertScrollBar at the right: up / down ticks, NewScrollZone, ScrollGrip
	const float SBW = FMath::RoundToFloat(L(16.f));
	const FRect Rows = bScrollBar ? FRect(R.X, R.Y, R.W - SBW - L(2.f), R.H) : R;
	float& First = Scrolls.FindOrAdd(InId);
	const float MaxFirst = float(FMath::Max(0, Num - Visible));

	bool bChanged = false;
	auto Select = [&](int32 NewSel)
	{
		NewSel = FMath::Clamp(NewSel, 0, Num - 1);
		// skip section headers
		while (Items.IsValidIndex(NewSel) && Items[NewSel].bSection && NewSel < Num - 1) ++NewSel;
		if (NewSel != Selected && Items.IsValidIndex(NewSel) && !Items[NewSel].bSection)
		{
			Selected = NewSel;
			bChanged = true;
		}
	};
	if (Num > 0)
	{
		if (Focused == InId)
		{
			int32 Dir = 0;
			if (AnyKey({ EKeys::Up, EKeys::Gamepad_DPad_Up, EKeys::Gamepad_LeftStick_Up })) Dir = -1;
			if (AnyKey({ EKeys::Down, EKeys::Gamepad_DPad_Down, EKeys::Gamepad_LeftStick_Down })) Dir = +1;
			if (Dir != 0)
			{
				int32 N = FMath::Clamp(Selected + Dir, 0, Num - 1);
				while (Items.IsValidIndex(N) && Items[N].bSection && N > 0 && N < Num - 1) N += Dir;
				// leave the list at its ends (focus moves on, like UE2's list wraps to the next control)
				if (N != Selected && Items.IsValidIndex(N) && !Items[N].bSection)
				{
					Select(N);
					ConsumeKey(EKeys::Up); ConsumeKey(EKeys::Down); ConsumeKey(EKeys::Gamepad_DPad_Up); ConsumeKey(EKeys::Gamepad_DPad_Down);
					ConsumeKey(EKeys::Gamepad_LeftStick_Up); ConsumeKey(EKeys::Gamepad_LeftStick_Down);
					PlaySound(ESound::Click);
				}
			}
			if (AnyKey({ EKeys::Enter, EKeys::Gamepad_FaceButton_Bottom }))
			{
				ConsumeKey(EKeys::Enter);
				ConsumeKey(EKeys::Gamepad_FaceButton_Bottom);
				bOutActivated = true;
			}
			if (KeyPressed(EKeys::Home)) Select(0);
			if (KeyPressed(EKeys::End)) Select(Num - 1);
		}
		if (Hovered(R) && Wheel != 0)
		{
			First -= Wheel * 3.f;
		}
		if (Hovered(Rows) && bMousePressed)
		{
			const int32 Row = int32(First) + FMath::FloorToInt((MousePos.Y - Rows.Y) / RowH);
			if (Items.IsValidIndex(Row) && !Items[Row].bSection && !Items[Row].bDisabled)
			{
				if (Row != Selected)
				{
					Selected = Row;
					bChanged = true;
					PlaySound(ESound::Click);
				}
				const double* Last = LastClickTime.Find(InId);
				if (bDoubleClick || (Last && Now - *Last < 0.35))
				{
					bOutActivated = true;
				}
				LastClickTime.Add(InId, Now);
			}
		}
		// a selection set from outside (page opened, game type changed) is scrolled into view once
		int32& Known = KnownSelection.FindOrAdd(InId);
		if (Known != Selected)
		{
			Known = Selected;
			bChanged |= false;
			if (Selected >= 0 && (Selected < First || Selected >= First + Visible))
			{
				First = float(FMath::Max(0, Selected - Visible / 2));
			}
		}
		if (bChanged && Selected >= 0)
		{
			// keep the selection visible
			if (Selected < First) First = float(Selected);
			if (Selected >= First + Visible) First = float(Selected - Visible + 1);
			KnownSelection.Add(InId, Selected);
		}
	}
	// scrollbar dragging
	const uint32 GripId = InId ^ 0x5a5a5a5au;
	if (bScrollBar)
	{
		const FRect Bar(R.R() - SBW, R.Y, SBW, R.H);
		const FRect UpB(Bar.X, Bar.Y, SBW, SBW), DownB(Bar.X, Bar.B() - SBW, SBW, SBW);
		const FRect Zone(Bar.X, Bar.Y + SBW, SBW, FMath::Max(1.f, Bar.H - SBW * 2.f));
		const float GripH = FMath::Max(L(12.f), Zone.H * float(Visible) / float(Num));
		const float GripY = Zone.Y + (Zone.H - GripH) * (MaxFirst > 0.f ? First / MaxFirst : 0.f);
		const FRect GripR(Bar.X, GripY, SBW, GripH);
		if (bMousePressed && Hovered(GripR)) { CaptureId = GripId; }
		if (CaptureId == GripId && bMouseDown)
		{
			First = (MousePos.Y - Zone.Y - GripH * 0.5f) / FMath::Max(1.f, Zone.H - GripH) * MaxFirst;
		}
		if (bMouseReleased && Hovered(UpB)) { First -= 1.f; PlaySound(ESound::Click); }
		if (bMouseReleased && Hovered(DownB)) { First += 1.f; PlaySound(ESound::Click); }
		if (bMousePressed && Hovered(Zone) && !Hovered(GripR)) { First += MousePos.Y < GripY ? -Visible : Visible; }
		First = FMath::Clamp(First, 0.f, MaxFirst);
		const float GripY2 = Zone.Y + (Zone.H - GripH) * (MaxFirst > 0.f ? First / MaxFirst : 0.f);
		if (!StyleImage(GetStyle(TEXT("ScrollZone")), EMenuState::Blurry, Zone))
		{
			D.Rect(Zone.X, Zone.Y, Zone.W, Zone.H, FArenaDraw::UTColor(FColor(10, 30, 80), 0.8f));
		}
		auto St = [this](const FRect& B) { return Hovered(B) ? (bMouseDown ? EMenuState::Pressed : EMenuState::Watched) : EMenuState::Blurry; };
		if (!StyleImage(GetStyle(TEXT("VertUpButton")), St(UpB), UpB)) FallbackBox(UpB, St(UpB), true);
		if (!StyleImage(GetStyle(TEXT("VertDownButton")), St(DownB), DownB)) FallbackBox(DownB, St(DownB), true);
		const FRect G2(Bar.X, GripY2, SBW, GripH);
		const EMenuState GS = CaptureId == GripId ? EMenuState::Pressed : St(G2);
		if (!StyleImage(GetStyle(TEXT("VertGrip")), GS, G2)) FallbackBox(G2, GS, true);
	}
	First = FMath::Clamp(First, 0.f, MaxFirst);

	// rows
	const int32 Start = FMath::FloorToInt(First);
	for (int32 i = 0; i < Visible && Start + i < Num; ++i)
	{
		const FListItem& It = Items[Start + i];
		const FRect Row(Rows.X, Rows.Y + i * RowH, Rows.W, RowH);
		const FRect TextR = Row.Inset(L(4.f), 0.f, L(4.f), 0.f);
		if (It.bSection)
		{
			// STY2ListSectionHeader band
			D.Rect(Row.X, Row.Y, Row.W, Row.H, ToLinear(SecS.ImgColors[0]));
			TextIn(F, It.Text, TextR.Inset(L(SecS.BorderOffsets[0]), 0.f, 0.f, 0.f), ToLinear(SecS.FontColors[0]), EAlign::Left);
			continue;
		}
		const bool bSel = (Start + i) == Selected;
		FLinearColor TextC = ToLinear(S.FontColors[0]);
		if (bSel)
		{
			const EMenuState SS = Focused == InId ? EMenuState::Focused : EMenuState::Blurry;
			D.Rect(Row.X, Row.Y, Row.W, Row.H, ToLinear(Sel.ImgColors[int32(SS)]));
			TextC = ToLinear(Sel.FontColors[int32(SS)]);
		}
		else if (Hovered(Row))
		{
			TextC = ToLinear(S.FontColors[1]);
		}
		if (It.bDisabled || (It.bDim && !bSel))
		{
			TextC = FLinearColor(0.55f, 0.55f, 0.6f, 0.9f);
		}
		if (Columns && It.Cells.Num() > 0)
		{
			// GUIMultiColumnList: each cell clipped to its column
			float CX = Row.X;
			for (int32 c = 0; c < Columns->Num() && c < It.Cells.Num(); ++c)
			{
				const float CW = (*Columns)[c].Width * Row.W;
				float IconsW = 0.f;
				if (c == 0)
				{
					const float IS = FMath::Max(4.f, Row.H - 2.f);
					for (const FString& Icon : It.Icons)
					{
						if (IconsW + IS <= CW && Image(Icon, FRect(CX + L(2.f) + IconsW, Row.Y + 1.f, IS, IS), EImgStyle::Scaled, FLinearColor::White))
						{
							IconsW += IS + 1.f;
						}
					}
				}
				TextIn(F, It.Cells[c], FRect(CX + L(4.f) + IconsW, Row.Y, FMath::Max(0.f, CW - L(8.f) - IconsW), Row.H), TextC, (*Columns)[c].Align);
				CX += CW;
			}
		}
		else if (!It.Right.IsEmpty())
		{
			const float RW = TextSize(F, It.Right).X;
			TextIn(F, It.Text, FRect(TextR.X, TextR.Y, TextR.W - RW - L(8.f), TextR.H), TextC, EAlign::Left);
			TextIn(F, It.Right, TextR, TextC, EAlign::Right);
		}
		else
		{
			TextIn(F, It.Text, TextR, TextC, EAlign::Left);
		}
	}
	(void)State;
	return bChanged;
}

bool FGUI::Tabs(uint32 InId, const FRect& R, const TArray<FString>& Captions, const TArray<FString>& Hints, int32& Active)
{
	const int32 Num = Captions.Num();
	if (Num == 0)
	{
		return false;
	}
	// GUITabControl: BackgroundStyleName "TabBackground" (NewTabBk) behind the buttons
	if (!StyleImage(GetStyle(TEXT("TabBackground")), EMenuState::Blurry, R))
	{
		D.Rect(R.X, R.Y, R.W, R.H, FArenaDraw::UTColor(FColor(16, 36, 96), 0.85f));
	}
	const FStyle& S = GetStyle(TEXT("TabButton"));
	const FFontRef F = StyleFont(S, EMenuState::Blurry, EFontScale::Medium);
	TArray<float> Widths;
	float Total = 0.f;
	const float Pad = L(24.f);
	for (const FString& C : Captions)
	{
		const float W = FMath::Max(L(70.f), TextSize(F, C).X + Pad * 2.f);
		Widths.Add(W);
		Total += W;
	}
	const float Gap = L(2.f);
	Total += Gap * (Num - 1);
	const float Scale = Total > R.W ? R.W / Total : 1.f;
	float X = R.X + (R.W - Total * Scale) * 0.5f;
	bool bChanged = false;
	// the tab row is one focus stop; Left / Right switch tabs while it has focus
	const EMenuState RowState = Interact(InId, R, true, true, Hints.IsValidIndex(Active) ? Hints[Active] : FString());
	for (int32 i = 0; i < Num; ++i)
	{
		const FRect TR(X, R.Y, Widths[i] * Scale, R.H);
		const bool bHover = Hovered(TR);
		EMenuState St = i == Active ? EMenuState::Focused : (bHover ? EMenuState::Watched : EMenuState::Blurry);
		if (i == Active && RowState == EMenuState::Focused && bKeyboardNav)
		{
			St = EMenuState::Pressed;
		}
		if (bHover && Hints.IsValidIndex(i))
		{
			Hint = Hints[i];
		}
		if (!StyleImage(S, St, TR))
		{
			FallbackBox(TR, St, true);
		}
		StyleText(S, St, EFontScale::Medium, Captions[i], TR, EAlign::Center);
		if (bMousePressed && bHover && i != Active)
		{
			Active = i;
			bChanged = true;
			PlaySound(ESound::Edit);
		}
		X += (Widths[i] + Gap) * Scale;
	}
	int32 Dir = TabDir;
	if (Focused == InId)
	{
		if (AnyKey({ EKeys::Left, EKeys::Gamepad_DPad_Left, EKeys::Gamepad_LeftStick_Left })) Dir = -1;
		if (AnyKey({ EKeys::Right, EKeys::Gamepad_DPad_Right, EKeys::Gamepad_LeftStick_Right })) Dir = +1;
	}
	if (Dir != 0)
	{
		Active = ((Active + Dir) % Num + Num) % Num;
		bChanged = true;
		TabDir = 0;
		PlaySound(ESound::Edit);
	}
	if (bChanged)
	{
		PopupId = 0;
	}
	return bChanged;
}

