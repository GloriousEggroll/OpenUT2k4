#include "ArenaGUI2K4Pages.h"
#include "OpenUT2k4.h"
#include "ArenaArtHUD.h"
#include "ArenaContent.h"
#include "ArenaRuntimeMap.h"
#include "ArenaDisplay.h"
#include "ArenaGameRules.h"
#include "ArenaLevelBuilder.h"
#include "ArenaLocalMessages.h"
#include "ArenaRayTracing.h"
#include "ArenaUpscalerSettings.h"
#include "ArenaUserSettings.h"
#include "UScript/ArenaUScript.h"
#include "ArenaPlayerSettingsTab.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

using namespace ArenaGUI2K4;
using namespace ArenaGUI2K4Pages;

namespace ArenaGUI2K4PagesLocal
{
	/** Splits a UE2 struct / array text ("(A=1,B="x,y")" or "("a","b")") at top-level commas; quotes are kept. */
	TArray<FString> SplitTopLevel(const FString& In)
	{
		TArray<FString> Out;
		FString S = In.TrimStartAndEnd();
		if (S.StartsWith(TEXT("(")) && S.EndsWith(TEXT(")")))
		{
			S = S.Mid(1, S.Len() - 2);
		}
		FString Cur;
		bool bQuote = false;
		int32 Depth = 0;
		for (const TCHAR C : S)
		{
			if (C == TEXT('"')) bQuote = !bQuote;
			if (!bQuote && C == TEXT('(')) ++Depth;
			if (!bQuote && C == TEXT(')')) --Depth;
			if (!bQuote && Depth == 0 && C == TEXT(','))
			{
				Out.Add(Cur.TrimStartAndEnd());
				Cur.Reset();
				continue;
			}
			Cur.AppendChar(C);
		}
		if (!Cur.IsEmpty())
		{
			Out.Add(Cur.TrimStartAndEnd());
		}
		return Out;
	}

	FString Unquote(const FString& In)
	{
		FString S = In.TrimStartAndEnd();
		if (S.Len() >= 2 && S.StartsWith(TEXT("\"")) && S.EndsWith(TEXT("\"")))
		{
			S = S.Mid(1, S.Len() - 2);
		}
		return S;
	}

	/** Key=Value pairs of a UE2 struct text; repeated keys keep the first value. */
	TMap<FString, FString> ParseStruct(const FString& In)
	{
		TMap<FString, FString> M;
		for (const FString& Part : SplitTopLevel(In))
		{
			FString K, V;
			if (Part.Split(TEXT("="), &K, &V))
			{
				K = K.TrimStartAndEnd();
				if (!M.Contains(K))
				{
					M.Add(K, Unquote(V));
				}
			}
		}
		return M;
	}

	/** "Pkg.Class.Key" -> Pkg.int [Class] Key */
	FString LocRef(const FString& Ref, const FString& Fallback)
	{
		TArray<FString> P;
		Ref.ParseIntoArray(P, TEXT("."));
		FString V;
		if (P.Num() == 3 && ArenaLocalMessages::Localize(P[0], P[1], P[2], V) && !V.IsEmpty())
		{
			return V;
		}
		return Fallback;
	}

	FString InstallSystemDir()
	{
		const FString Install = ArenaContent::FindUT2004Install();
		return Install.IsEmpty() ? FString() : Install / TEXT("System");
	}

	// ---- settings model: our UArenaUserSettings rows in the UT2K4SettingsPage tabs ----
	enum class EKind : uint8 { Check, Combo, Slider, Spinner, Name, StatsName, StatsPassword };
	enum ETab : int32 { TabDisplay, TabAudio, TabPlayer, TabGame, TabInput, TabHUD, NumTabs };

	struct FRow
	{
		FName Id;
		int32 Tab = TabDisplay;
		/** section index inside the tab (see TabSections) */
		int32 Section = 0;
		EKind Kind = EKind::Spinner;
		/** stock caption (GUI2K4.int section / key) used when it fits our setting */
		const TCHAR* LocSection = nullptr;
		const TCHAR* LocKey = nullptr;
		/** position inside its section (stable sort; Brightness after the display mode rows) */
		int32 Order = 0;
	};

	/** Our rows by tab. Unknown ids (rows added later) go to Display / Options as spinners. */
	TArray<FRow> BuildRows()
	{
		TArray<FRow> Rows;
		for (const FName& Id : UArenaUserSettings::GetAdjustableSettingIds())
		{
			FRow R;
			R.Id = Id;
			const FString S = Id.ToString();
			if (S == TEXT("Monitor"))             { R.Tab = TabDisplay; R.Section = 0; R.Kind = EKind::Combo; }
			else if (S == TEXT("WindowMode") || S == TEXT("Resolution")) { R.Tab = TabDisplay; R.Section = 0; R.Kind = EKind::Spinner;
				R.LocSection = TEXT("UT2K4Tab_DetailSettings"); R.LocKey = S == TEXT("Resolution") ? TEXT("VideoResolution.Caption") : nullptr; }
			else if (S.StartsWith(TEXT("HDR")))   { R.Tab = TabDisplay; R.Section = 3; R.Kind = EKind::Spinner; }
			else if (S == TEXT("Brightness"))     { R.Tab = TabDisplay; R.Section = 0; R.Kind = EKind::Slider; R.Order = 10; R.LocSection = TEXT("UT2K4Tab_DetailSettings"); R.LocKey = TEXT("BrightnessSlider.Caption"); }
			else if (S == TEXT("RayTracing"))     { R.Tab = TabDisplay; R.Section = 1; R.Kind = EKind::Combo; }
			else if (S == TEXT("FullRayTracing")) { R.Tab = TabDisplay; R.Section = 1; R.Kind = EKind::Check; }
			else if (S == TEXT("Upscaler"))       { R.Tab = TabDisplay; R.Section = 1; R.Kind = EKind::Combo; }
			else if (S == TEXT("UpscalerQuality")){ R.Tab = TabDisplay; R.Section = 1; R.Kind = EKind::Combo; }
			else if (S == TEXT("FSRSharpness"))   { R.Tab = TabDisplay; R.Section = 1; R.Kind = EKind::Slider; }
			else if (S == TEXT("LowLatency"))     { R.Tab = TabDisplay; R.Section = 1; R.Kind = EKind::Check; }
			else if (S == TEXT("Volume"))         { R.Tab = TabAudio; R.Section = 0; R.Kind = EKind::Slider; }
			else if (S == TEXT("MusicVolume"))    { R.Tab = TabAudio; R.Section = 0; R.Kind = EKind::Slider; R.LocSection = TEXT("UT2K4Tab_AudioSettings"); R.LocKey = TEXT("AudioMusicVolume.Caption"); }
			else if (S == TEXT("FOV"))            { R.Tab = TabPlayer; R.Section = 1; R.Kind = EKind::Slider; R.LocSection = TEXT("UT2K4Tab_PlayerSettings"); R.LocKey = TEXT("PlayerFOV.Caption"); }
			else if (S == TEXT("Sensitivity"))    { R.Tab = TabInput; R.Section = 0; R.Kind = EKind::Spinner; R.LocSection = TEXT("UT2K4Tab_IForceSettings"); R.LocKey = TEXT("InputMouseSensitivity.Caption"); }
			else if (S == TEXT("InvertY"))        { R.Tab = TabInput; R.Section = 0; R.Kind = EKind::Check; R.LocSection = TEXT("UT2K4Tab_IForceSettings"); R.LocKey = TEXT("InputInvertMouse.Caption"); }
			else if (S == TEXT("Crosshair"))      { R.Tab = TabHUD; R.Section = 0; R.Kind = EKind::Combo; R.Order = 5; }
			else if (S.StartsWith(TEXT("HUD")))   { R.Tab = TabHUD; R.Section = 0; R.Kind = EKind::Spinner; } // HUD Scale, HUD Width (hud agent)
			else                                  { R.Tab = TabDisplay; R.Section = 1; R.Kind = EKind::Spinner; }
			Rows.Add(R);
		}
		FRow Name;
		Name.Id = FName(TEXT("PlayerName"));
		Name.Tab = TabPlayer;
		Name.Section = 1;
		Name.Kind = EKind::Name;
		Name.LocSection = TEXT("UT2K4Tab_PlayerSettings");
		Name.LocKey = TEXT("PlayerName.Caption");
		Rows.Insert(Name, 0);
		// Game tab, "Stats" (UT2K4Tab_GameSettings: Track Stats, UserName, Password); stored in UArenaUserSettings only
		const struct { const TCHAR* Id; EKind Kind; const TCHAR* LocKey; } StatsRows[] = {
			{ TEXT("TrackStats"), EKind::Check, TEXT("OnlineTrackStats.Caption") },
			{ TEXT("StatsUsername"), EKind::StatsName, TEXT("OnlineStatsName.Caption") },
			{ TEXT("StatsPassword"), EKind::StatsPassword, TEXT("OnlineStatsPW.Caption") } };
		for (const auto& Sr : StatsRows)
		{
			FRow R;
			R.Id = FName(Sr.Id);
			R.Tab = TabGame;
			R.Section = 0;
			R.Kind = Sr.Kind;
			R.LocSection = TEXT("UT2K4Tab_GameSettings");
			R.LocKey = Sr.LocKey;
			Rows.Add(R);
		}
		Rows.StableSort([](const FRow& A, const FRow& B) { return A.Order < B.Order; });
		return Rows;
	}

	FString RowLabel(const FRow& R)
	{
		const FString S = R.Id.ToString();
		const FString Ours = R.Kind == EKind::Name ? FString(TEXT("Player Name"))
			: S == TEXT("TrackStats") ? FString(TEXT("Track Stats"))
			: S == TEXT("StatsUsername") ? FString(TEXT("UserName"))
			: S == TEXT("StatsPassword") ? FString(TEXT("Password"))
			: UArenaUserSettings::GetSettingLabel(R.Id);
		return R.LocKey ? Loc(TEXT("GUI2K4"), R.LocSection, R.LocKey, Ours) : Ours;
	}

	FString RowHint(const FRow& R)
	{
		const FString S = R.Id.ToString();
		if (S == TEXT("PlayerName")) return TEXT("The name other players see in the match");
		if (S == TEXT("TrackStats")) return Loc(TEXT("GUI2K4"), TEXT("UT2K4Tab_GameSettings"), TEXT("OnlineTrackStats.Hint"),
			TEXT("Enable this option to join the online ranking system."));
		if (S == TEXT("StatsUsername")) return Loc(TEXT("GUI2K4"), TEXT("UT2K4Tab_GameSettings"), TEXT("OnlineStatsName.Hint"),
			TEXT("Please select a name to use for UT Stats!"));
		if (S == TEXT("StatsPassword")) return Loc(TEXT("GUI2K4"), TEXT("UT2K4Tab_GameSettings"), TEXT("OnlineStatsPW.Hint"),
			TEXT("Please select a password that will secure your UT Stats!"));
		if (S == TEXT("Monitor")) return TEXT("Which display the full screen game uses");
		if (S == TEXT("Brightness")) return TEXT("Overall brightness of the game view");
		if (S == TEXT("RayTracing")) return TEXT("Hardware ray traced lighting (Lumen); Auto uses it on a capable graphics card");
		if (S == TEXT("FullRayTracing")) return TEXT("Ray traced lights, shadows and reflections on top of ray traced lighting");
		if (S == TEXT("Upscaler")) return TEXT("How the image is rendered and reconstructed at your screen resolution");
		if (S == TEXT("UpscalerQuality")) return TEXT("Render resolution of the upscaler: higher quality costs performance");
		if (S == TEXT("FSRSharpness")) return TEXT("Sharpening applied after FSR upscaling");
		if (S == TEXT("LowLatency")) return TEXT("Reduces input lag; may need a restart to take effect");
		if (S == TEXT("Volume")) return TEXT("Master volume of all game sound");
		if (S == TEXT("MusicVolume")) return TEXT("Volume of the background music");
		if (S == TEXT("FOV")) return TEXT("Horizontal field of view in degrees");
		if (S == TEXT("Sensitivity")) return TEXT("Mouse look speed in game");
		if (S == TEXT("InvertY")) return TEXT("Moving the mouse forward looks down");
		if (S == TEXT("Crosshair")) return TEXT("The crosshair drawn in the middle of the screen");
		if (S == TEXT("WindowMode")) return TEXT("Full screen, borderless or windowed");
		if (S == TEXT("Resolution")) return TEXT("Screen resolution of the game");
		if (S.StartsWith(TEXT("HDR"))) return TEXT("High dynamic range output for HDR displays");
		return UArenaUserSettings::GetSettingLabel(R.Id);
	}

	int32 NumChoices(FName Id)
	{
		const FString S = Id.ToString();
		if (S == TEXT("Monitor")) return ArenaDisplay::NumChoices();
		if (S == TEXT("RayTracing")) return ArenaRayTracing::NumChoices;
		if (S == TEXT("Upscaler")) return ArenaUpscalerSettings::NumUpscalers;
		if (S == TEXT("UpscalerQuality")) return ArenaUpscalerSettings::NumQualities;
		if (S == TEXT("Crosshair")) return ArenaArtHUD::NumCrosshairs();
		return 0;
	}

	FString ChoiceText(FName Id, int32 I)
	{
		const FString S = Id.ToString();
		if (S == TEXT("Monitor")) return ArenaDisplay::GetChoiceName(I);
		if (S == TEXT("RayTracing")) return ArenaRayTracing::GetChoiceText(I);
		if (S == TEXT("Upscaler")) return ArenaUpscalerSettings::GetUpscalerText(I);
		if (S == TEXT("UpscalerQuality")) return ArenaUpscalerSettings::GetQualityText(I);
		if (S == TEXT("Crosshair")) return ArenaArtHUD::GetCrosshairName(I);
		return FString();
	}

	int32 GetChoice(FName Id)
	{
		const UArenaUserSettings* U = UArenaUserSettings::Get();
		const FString S = Id.ToString();
		if (S == TEXT("Monitor")) return U->Monitor;
		if (S == TEXT("RayTracing")) return U->RayTracing;
		if (S == TEXT("Upscaler")) return U->Upscaler;
		if (S == TEXT("UpscalerQuality")) return U->UpscalerQuality;
		if (S == TEXT("Crosshair")) return U->Crosshair;
		return 0;
	}

	/** Sets a choice and applies it the way UArenaUserSettings::ChangeSetting does for that row. */
	void SetChoice(FName Id, int32 I)
	{
		UArenaUserSettings* U = UArenaUserSettings::Get();
		const FString S = Id.ToString();
		if (S == TEXT("Monitor")) { U->Monitor = I; U->Save(); ArenaDisplay::Apply(); }
		else if (S == TEXT("RayTracing")) { U->RayTracing = I; U->Save(); ArenaRayTracing::Apply(); }
		else if (S == TEXT("Upscaler")) { U->Upscaler = I; U->Save(); ArenaUpscalerSettings::Apply(); }
		else if (S == TEXT("UpscalerQuality")) { U->UpscalerQuality = I; U->Save(); ArenaUpscalerSettings::Apply(); }
		else if (S == TEXT("Crosshair")) { U->Crosshair = I; U->Save(); }
	}

	bool GetCheck(FName Id)
	{
		const UArenaUserSettings* U = UArenaUserSettings::Get();
		const FString S = Id.ToString();
		if (S == TEXT("InvertY")) return U->bInvertY;
		if (S == TEXT("TrackStats")) return U->bTrackStats;
		if (S == TEXT("FullRayTracing")) return U->FullRayTracing != 0;
		if (S == TEXT("LowLatency")) return U->LowLatency != 0;
		return false;
	}

	bool CheckEnabled(FName Id)
	{
		return Id != FName(TEXT("FullRayTracing")) || ArenaRayTracing::CanUseFull();
	}

	struct FSliderDef { float Min = 0.f, Max = 1.f, Step = 0.05f; };
	FSliderDef SliderDef(FName Id)
	{
		const FString S = Id.ToString();
		if (S == TEXT("Brightness")) return { UArenaUserSettings::MinBrightness, UArenaUserSettings::MaxBrightness, UArenaUserSettings::BrightnessStep };
		if (S == TEXT("FOV")) return { UArenaUserSettings::MinFOV, UArenaUserSettings::MaxFOV, 5.f };
		if (S == TEXT("FSRSharpness")) return { 0.f, 1.f, 0.1f };
		return { 0.f, 1.f, 0.05f };
	}

	float* SliderValue(FName Id)
	{
		UArenaUserSettings* U = UArenaUserSettings::Get();
		const FString S = Id.ToString();
		if (S == TEXT("Brightness")) return &U->Brightness;
		if (S == TEXT("FOV")) return &U->FOV;
		if (S == TEXT("FSRSharpness")) return &U->FSRSharpness;
		if (S == TEXT("Volume")) return &U->Volume;
		if (S == TEXT("MusicVolume")) return &U->MusicVolume;
		return nullptr;
	}

	/** Applies a slider value while it moves (cheap); the release saves. */
	void ApplySlider(FName Id)
	{
		const FString S = Id.ToString();
		if (S == TEXT("Volume")) UArenaUserSettings::Get()->ApplyAudio();
		else if (S == TEXT("FSRSharpness")) ArenaUpscalerSettings::Apply();
	}

	/** DEFAULTS button: the values of UArenaUserSettings' initializers. */
	void ResetRow(const FRow& R)
	{
		UArenaUserSettings* U = UArenaUserSettings::Get();
		const FString S = R.Id.ToString();
		if (S == TEXT("Sensitivity")) U->MouseSensitivity = 1.f;
		else if (S == TEXT("InvertY")) U->bInvertY = false;
		else if (S == TEXT("FOV")) U->FOV = 100.f;
		else if (S == TEXT("Crosshair")) U->Crosshair = 0;
		else if (S == TEXT("Brightness")) U->Brightness = 0.f;
		else if (S == TEXT("Volume")) { U->Volume = 1.f; U->ApplyAudio(); }
		else if (S == TEXT("MusicVolume")) U->MusicVolume = 0.6f;
		else if (S == TEXT("RayTracing")) { SetChoice(R.Id, 0); return; }
		else if (S == TEXT("FullRayTracing")) { if (U->FullRayTracing != 0) U->ChangeSetting(R.Id, 1); return; }
		else if (S == TEXT("Upscaler")) { SetChoice(R.Id, 0); return; }
		else if (S == TEXT("UpscalerQuality")) { SetChoice(R.Id, 0); return; }
		else if (S == TEXT("FSRSharpness")) { U->FSRSharpness = 0.2f; U->Save(); ArenaUpscalerSettings::Apply(); return; }
		else if (S == TEXT("LowLatency")) { if (U->LowLatency != 0) U->ChangeSetting(R.Id, 1); return; }
		else if (S == TEXT("Monitor")) { SetChoice(R.Id, 0); return; }
		else return;
		U->Save();
	}

	struct FSectionDef
	{
		/** GUI2K4.int section / key of the stock caption, our fallback */
		const TCHAR* LocSection;
		const TCHAR* LocKey;
		const TCHAR* Fallback;
		float L, T, W, H;
		bool bAlt = false;
	};
	/** UT2K4Tab_* section backgrounds (WinLeft, WinTop, WinWidth, WinHeight of the panel) */
	TArray<FSectionDef> TabSections(int32 Tab)
	{
		switch (Tab)
		{
		case TabDisplay: return {
			// sbSection1 "Resolution", sbSection2 "Options", sbSection3 "Gamma Test" (UT2K4Tab_DetailSettings)
			{ TEXT("UT2K4Tab_DetailSettings"), TEXT("sbSection1.Caption"), TEXT("Resolution"), 0.000948f, 0.012761f, 0.491849f, 0.440729f },
			// (Options keeps its left / width; its lower part holds our HDR group)
			{ TEXT("UT2K4Tab_DetailSettings"), TEXT("sbSection2.Caption"), TEXT("Options"), 0.495826f, 0.012761f, 0.502751f, 0.62f },
			{ TEXT("UT2K4Tab_DetailSettings"), TEXT("sbSection3.Caption"), TEXT("Gamma Test"), 0.011132f, 0.476061f, 0.462891f, 0.511261f },
			{ nullptr, nullptr, TEXT("HDR"), 0.495826f, 0.65f, 0.502751f, 0.337989f } };
		case TabAudio: return {
			// AudioBK1 "Sound Effects"
			{ TEXT("UT2K4Tab_AudioSettings"), TEXT("AudioBK1.Caption"), TEXT("Sound Effects"), 0.004063f, 0.017393f, 0.987773f, 0.45f } };
		case TabPlayer: return {
			// PlayerBK1 "3D View", PlayerBK2 "Misc."
			{ TEXT("UT2K4Tab_PlayerSettings"), TEXT("PlayerBK1.Caption"), TEXT("Character"), 0.004063f, 0.017969f, 0.446758f, 0.963631f },
			{ TEXT("UT2K4Tab_PlayerSettings"), TEXT("PlayerBK2.Caption"), TEXT("Misc."), 0.463047f, 0.017969f, 0.531719f, 0.573006f } };
		case TabGame: return {
			// GameBK3 "Stats" (the tab's other sections have no settings of ours yet)
			{ TEXT("UT2K4Tab_GameSettings"), TEXT("GameBK3.Caption"), TEXT("Stats"), 0.014649f, 0.033853f, 0.449609f, 0.36f } };
		case TabInput: return {
			// InputBK1 "Options"
			{ TEXT("UT2K4Tab_IForceSettings"), TEXT("InputBK1.Caption"), TEXT("Options"), 0.021641f, 0.028176f, 0.381328f, 0.655039f } };
		case TabHUD: return {
			// GameBK "Options", GameBK1 "Visuals" (UT2K4Tab_HudSettings)
			{ TEXT("UT2K4Tab_HudSettings"), TEXT("GameBK.Caption"), TEXT("Options"), 0.031797f, 0.057604f, 0.448633f, 0.901485f },
			{ TEXT("UT2K4Tab_HudSettings"), TEXT("GameBK1.Caption"), TEXT("Visuals"), 0.517578f, 0.060208f, 0.448633f, 0.901485f } };
		default: return {};
		}
	}
}

using namespace ArenaGUI2K4PagesLocal;

// =================================================================================================
// Localization, page frame
// =================================================================================================

FString ArenaGUI2K4Pages::LocArray(const TCHAR* Package, const TCHAR* Section, const TCHAR* Key, int32 Index, const FString& Fallback)
{
	FString Raw;
	if (!ArenaLocalMessages::Localize(Package, Section, Key, Raw))
	{
		return Fallback;
	}
	const TArray<FString> Parts = SplitTopLevel(Raw);
	return Parts.IsValidIndex(Index) && !Unquote(Parts[Index]).IsEmpty() ? Unquote(Parts[Index]) : Fallback;
}

FTabbedFrame ArenaGUI2K4Pages::TabbedPage(FGUI& G, uint32 Id, const FString& PageCaption, const TArray<FString>& Tabs, const TArray<FString>& Hints,
	int32& Active, double& TabChangedAt)
{
	FTabbedFrame F;
	G.PageBackground();
	const FString TabCaption = Tabs.IsValidIndex(Active) ? Tabs[Active] : FString();
	// UT2K4GamePageBase.InternalOnChange: t_Header.SetCaption(PageCaption @ "|" @ Tab.Caption)
	const FRect Header = G.Header(TabCaption.IsEmpty() ? PageCaption : PageCaption + TEXT(" | ") + TabCaption);
	const FRect Page = G.Page();
	// PageTabs: WinLeft 0.01, WinWidth 0.98, TabHeight 0.04, docked under the header
	const FRect TabsR(Page.X + Page.W * 0.01f, Header.B() + G.L(2.f), Page.W * 0.98f, FMath::RoundToFloat(Page.H * 0.04f));
	if (G.Tabs(Id, TabsR, Tabs, Hints, Active))
	{
		F.bTabChanged = true;
		TabChangedAt = G.Time();
	}
	// ButtonFooter at the bottom (WinHeight 0.04); the tab's hint shows in it for a while after a change
	const FString FooterText = (G.Time() - TabChangedAt < 4.0 && Hints.IsValidIndex(Active)) ? Hints[Active] : FString();
	F.Footer = G.Footer(FooterText);
	F.Panel = FRect(Page.X + Page.W * 0.01f, TabsR.B() + G.L(4.f), Page.W * 0.98f, F.Footer.Y - TabsR.B() - G.L(10.f));
	return F;
}

int32 ArenaGUI2K4Pages::FooterButtons(FGUI& G, const FRect& Footer, const TArray<FString>& Captions, const TArray<FString>& Hints, const TArray<bool>& Enabled)
{
	// ButtonFooter: bFixedWidth (longest caption), Padding 0.14, Margin 0.005, Alignment right, bFullHeight.
	// The buttons always stay inside the 4:3 page: when the fixed width does not fit (long captions such as REMOVE
	// FAVORITE), each button takes its own caption's width (ButtonFooter without bFixedWidth), then the small font,
	// and as a last resort the widths are scaled down to the page.
	const FStyle& S = GetStyle(TEXT("FooterButton"));
	const FRect Page = G.Page();
	const float Avail = Page.W * 0.995f;
	const int32 Num = Captions.Num();
	if (Num == 0)
	{
		return INDEX_NONE;
	}
	EFontScale Scale = EFontScale::Medium;
	TArray<float> Widths;
	auto Measure = [&](EFontScale InScale, bool bFixed) -> float
	{
		const FFontRef F = G.StyleFont(S, EMenuState::Blurry, InScale);
		Widths.Reset();
		float Longest = 0.f;
		for (const FString& C : Captions)
		{
			const float W = G.TextSize(F, C).X * 1.14f + G.L(24.f);
			Widths.Add(W);
			Longest = FMath::Max(Longest, W);
		}
		float Total = 0.f;
		for (float& W : Widths)
		{
			W = bFixed ? FMath::Max(Longest, Page.W * 0.12f) : FMath::Max(W, Page.W * 0.08f);
			Total += W;
		}
		return Total;
	};
	float Total = Measure(EFontScale::Medium, true);
	if (Total > Avail)
	{
		Total = Measure(EFontScale::Medium, false);
	}
	if (Total > Avail)
	{
		Scale = EFontScale::Small;
		Total = Measure(EFontScale::Small, false);
	}
	if (Total > Avail)
	{
		for (float& W : Widths)
		{
			W *= Avail / Total;
		}
		Total = Avail;
	}
	float X = Page.R() - Page.W * 0.0025f - Total;
	int32 Clicked = INDEX_NONE;
	for (int32 i = 0; i < Num; ++i)
	{
		const bool bEnabled = !Enabled.IsValidIndex(i) || Enabled[i];
		if (G.Button(FGUI::Id(TEXT("FooterBtn"), i), FRect(X, Footer.Y, Widths[i], Footer.H), TEXT("FooterButton"), Captions[i],
			Hints.IsValidIndex(i) ? Hints[i] : FString(), bEnabled, Scale))
		{
			Clicked = i;
		}
		X += Widths[i];
	}
	return Clicked;
}

// =================================================================================================
// Settings page
// =================================================================================================

bool FSettingsPage::Draw(FGUI& G)
{
	static const TArray<FRow> AllRows = BuildRows();
	// tabs that have rows (UT2K4SettingsPage.PanelCaption: Display, Audio, Player, Game, Input, Weapons, HUD)
	struct FTabDef { int32 Tab; int32 LocIndex; const TCHAR* Fallback; const TCHAR* HintFallback; };
	static const FTabDef TabDefs[] = {
		{ TabDisplay, 0, TEXT("Display"), TEXT("Display, lighting and upscaling options") },
		{ TabAudio, 1, TEXT("Audio"), TEXT("Volume levels") },
		{ TabPlayer, 2, TEXT("Player"), TEXT("Your name and view") },
		{ TabGame, 3, TEXT("Game"), TEXT("Gameplay and stats options") },
		{ TabInput, 4, TEXT("Input"), TEXT("Mouse options") },
		{ TabHUD, 6, TEXT("HUD"), TEXT("Crosshair") },
	};
	TArray<int32> TabIds;
	TArray<FString> Captions, Hints;
	for (const FTabDef& T : TabDefs)
	{
		if (AllRows.ContainsByPredicate([&T](const FRow& R) { return R.Tab == T.Tab; }))
		{
			TabIds.Add(T.Tab);
			Captions.Add(LocArray(TEXT("GUI2K4"), TEXT("UT2K4SettingsPage"), TEXT("PanelCaption"), T.LocIndex, T.Fallback));
			Hints.Add(LocArray(TEXT("GUI2K4"), TEXT("UT2K4SettingsPage"), TEXT("PanelHint"), T.LocIndex, T.HintFallback));
		}
	}
	if (bOpenStatsTab)
	{
		bOpenStatsTab = false;
		Tab = FMath::Max(0, TabIds.IndexOfByKey(int32(TabGame)));
	}
	Tab = FMath::Clamp(Tab, 0, FMath::Max(0, TabIds.Num() - 1));
	const FString PageCaption = Loc(TEXT("GUI2K4"), TEXT("UT2K4SettingsPage"), TEXT("SettingHeader.Caption"), TEXT("Settings"));
	const FTabbedFrame Frame = TabbedPage(G, FGUI::Id(TEXT("SettingsTabs")), PageCaption, Captions, Hints, Tab, TabChangedAt);
	if (Frame.bTabChanged)
	{
		G.ResetInteraction();
	}
	const int32 TabId = TabIds.IsValidIndex(Tab) ? TabIds[Tab] : TabDisplay;
	const TArray<FSectionDef> Sections = TabId == TabPlayer ? TArray<FSectionDef>() : TabSections(TabId);
	UArenaUserSettings* U = UArenaUserSettings::Get();
	// the Player tab is its own page (UT2K4Tab_PlayerSettings: character, 3D view, Misc., Biography)
	const bool bPlayerModal = TabId == TabPlayer && ArenaPlayerSettingsTab::IsModalOpen();
	if (TabId == TabPlayer)
	{
		ArenaPlayerSettingsTab::Draw(G, Frame.Panel);
	}

	// section backgrounds, then the rows each section manages (AutoPosition: stacked from the top of the client area)
	TArray<FRect> Clients;
	for (const FSectionDef& Sd : Sections)
	{
		const FRect R = Frame.Panel.Sub(Sd.L, Sd.T, Sd.W, Sd.H);
		Clients.Add(G.Section(R, Sd.LocSection ? Loc(TEXT("GUI2K4"), Sd.LocSection, Sd.LocKey, Sd.Fallback) : FString(Sd.Fallback), Sd.bAlt));
	}
	// GUIMenuOption standard height 0.03 of the page; rows spaced like a section's auto positioning
	const float RowH = FMath::RoundToFloat(G.ScreenH() * 0.03f);
	const float RowStep = FMath::RoundToFloat(G.ScreenH() * 0.045f);
	TArray<float> NextY;
	for (const FRect& C : Clients)
	{
		NextY.Add(C.Y + G.L(6.f));
	}
	int32 RowIndex = 0;
	for (const FRow& R : AllRows)
	{
		if (R.Tab != TabId)
		{
			continue;
		}
		const int32 Sec = Clients.IsValidIndex(R.Section) ? R.Section : 0;
		if (!Clients.IsValidIndex(Sec))
		{
			continue;
		}
		const FRect& C = Clients[Sec];
		const FRect Box(C.X + C.W * 0.05f, NextY[Sec], C.W * 0.9f, RowH);
		NextY[Sec] += RowStep;
		const uint32 Id = FGUI::Id(*R.Id.ToString(), 100 + RowIndex++);
		const FString Label = RowLabel(R);
		const FString HintText = RowHint(R);
		switch (R.Kind)
		{
		case EKind::Name:
		{
			FString Name = U->PlayerName;
			if (G.EditBox(Id, Box, Label, Name, HintText, 0.5f, 24))
			{
				U->PlayerName = Name;
				U->Save();
			}
			break;
		}
		case EKind::StatsName:
		case EKind::StatsPassword:
		{
			const bool bPassword = R.Kind == EKind::StatsPassword;
			FString Text = bPassword ? U->StatsPassword : U->StatsUsername;
			if (G.EditBox(Id, Box, Label, Text, HintText, 0.5f, UArenaUserSettings::MaxStatsTextLen, bPassword))
			{
				(bPassword ? U->StatsPassword : U->StatsUsername) = UArenaUserSettings::CleanStatsText(Text);
				U->Save();
			}
			break;
		}
		case EKind::Check:
		{
			bool b = GetCheck(R.Id);
			if (G.CheckBox(Id, Box, Label, b, HintText, 0.8f, CheckEnabled(R.Id)))
			{
				if (R.Id == FName(TEXT("TrackStats")))
				{
					U->bTrackStats = !U->bTrackStats;
					U->Save();
				}
				else
				{
					U->ChangeSetting(R.Id, 1);
				}
			}
			break;
		}
		case EKind::Combo:
		{
			TArray<FString> Items;
			for (int32 i = 0; i < NumChoices(R.Id); ++i)
			{
				Items.Add(ChoiceText(R.Id, i));
			}
			int32 Index = GetChoice(R.Id);
			if (R.Id == FName(TEXT("UpscalerQuality")) && U->Upscaler == ArenaUpscalerSettings::UpscalerOff)
			{
				// the quality has no effect without an upscaler: show "Native" like the old row did
				TArray<FString> Native = { U->GetSettingValueText(R.Id) };
				int32 Zero = 0;
				G.Combo(Id, Box, Label, Zero, Native, HintText, 0.5f, false);
				break;
			}
			if (G.Combo(Id, Box, Label, Index, Items, HintText, 0.5f, Items.Num() > 1))
			{
				SetChoice(R.Id, Index);
			}
			break;
		}
		case EKind::Slider:
		{
			float* V = SliderValue(R.Id);
			if (!V)
			{
				break;
			}
			const FSliderDef Def = SliderDef(R.Id);
			bool bReleased = false;
			if (G.Slider(Id, Box, Label, *V, Def.Min, Def.Max, Def.Step, U->GetSettingValueText(R.Id), HintText, bReleased, 0.5f))
			{
				ApplySlider(R.Id);
			}
			if (bReleased)
			{
				U->Save();
			}
			break;
		}
		case EKind::Spinner:
		default:
		{
			const int32 Dir = G.Spinner(Id, Box, Label, U->GetSettingValueText(R.Id), HintText, 0.5f);
			if (Dir != 0)
			{
				U->ChangeSetting(R.Id, Dir);
			}
			break;
		}
		}
	}

	// tab extras
	if (TabId == TabDisplay && Clients.IsValidIndex(2))
	{
		// GammaBar: 2K4Menus.Controls.Gamma test image in the Gamma Test section
		const FRect C = Clients[2];
		const FRect Img = C.Inset(C.W * 0.04f, C.H * 0.04f, C.W * 0.04f, C.H * 0.04f);
		if (!G.Image(TEXT("2K4Menus.Controls.Gamma"), Img, EImgStyle::Scaled, FLinearColor::White))
		{
			for (int32 i = 0; i < 16; ++i)
			{
				const float V = float(i) / 15.f;
				G.Draw().Rect(Img.X + Img.W * i / 16.f, Img.Y, Img.W / 16.f + 1.f, Img.H, FLinearColor(V, V, V, 1.f));
			}
		}
	}
	if (TabId == TabGame && Clients.IsValidIndex(0) && U->bTrackStats && !U->HasValidStatsLogin())
	{
		// UT2K4Tab_GameSettings.InvalidWarning under the Stats section
		const FRect C = Clients[0];
		const FString Warn = Loc(TEXT("GUI2K4"), TEXT("UT2K4Tab_GameSettings"), TEXT("InvalidWarning.Caption"),
			TEXT("Your stats username or password is invalid.  Your username must be at least 4 characters long, and your password must be at least 6 characters long."));
		const FFontRef F = G.Font(TEXT("UT2SmallFont"));
		float Y = NextY[0] + G.L(4.f);
		for (const FString& Line : G.Wrap(F, Warn, C.W * 0.9f))
		{
			G.TextIn(F, Line, FRect(C.X + C.W * 0.05f, Y, C.W * 0.9f, F.Height), FLinearColor(1.f, 0.35f, 0.3f, 1.f), EAlign::Left);
			Y += F.Height;
		}
	}
	if (TabId == TabPlayer && Clients.IsValidIndex(0))
	{
		// "3D View" section: no model viewer; the player character art of the menus instead
		const FRect C = Clients[0];
		const FBox2f Shot(FVector2f(230.f, 0.f), FVector2f(900.f, 1024.f));
		G.Image(TEXT("2K4Menus.MainMenu.Char01"), C.Inset(G.L(8.f), G.L(8.f), G.L(8.f), G.L(8.f)), EImgStyle::Justified, FLinearColor::White, &Shot);
	}
	if (TabId == TabHUD && Clients.IsValidIndex(1))
	{
		// crosshair preview in "Visuals"
		const FRect C = Clients[1];
		const float Size = FMath::Min(C.W, C.H) * 0.35f;
		G.Draw().Rect(C.X + (C.W - Size * 1.6f) * 0.5f, C.Y + (C.H - Size * 1.6f) * 0.3f, Size * 1.6f, Size * 1.6f, FLinearColor(0.f, 0.f, 0.f, 0.45f));
		if (!ArenaArtHUD::DrawCrosshair(G.Draw(), U->Crosshair, C.X + C.W * 0.5f, C.Y + (C.H - Size * 1.6f) * 0.3f + Size * 0.8f, Size,
			FLinearColor(0.9f, 1.f, 0.95f, 1.f), false))
		{
			const float CX = C.X + C.W * 0.5f, CY = C.Y + (C.H - Size * 1.6f) * 0.3f + Size * 0.8f;
			G.Draw().Rect(CX - Size * 0.4f, CY - 1.f, Size * 0.8f, 2.f, FLinearColor::White);
			G.Draw().Rect(CX - 1.f, CY - Size * 0.4f, 2.f, Size * 0.8f, FLinearColor::White);
		}
	}

	// footer: BACK, DEFAULTS (UT2K4Settings_Footer)
	const TArray<FString> Btns = {
		Loc(TEXT("GUI2K4"), TEXT("UT2K4Settings_Footer"), TEXT("BackB.Caption"), TEXT("BACK")),
		Loc(TEXT("GUI2K4"), TEXT("UT2K4Settings_Footer"), TEXT("DefaultB.Caption"), TEXT("DEFAULTS")) };
	const TArray<FString> BtnHints = { TEXT("Return to the previous menu"), TEXT("Reset the settings of this tab to their default values") };
	const int32 Clicked = FooterButtons(G, Frame.Footer, Btns, BtnHints, {});
	if (Clicked == 1)
	{
		if (TabId == TabPlayer)
		{
			ArenaPlayerSettingsTab::ResetDefaults();
		}
		for (const FRow& R : AllRows)
		{
			if (R.Tab == TabId)
			{
				ResetRow(R);
			}
		}
	}
	if (Clicked == 0 || (G.BackPressed() && !bPlayerModal))
	{
		U->Save();
		return true;
	}
	return false;
}

// =================================================================================================
// Instant Action data
// =================================================================================================

const TArray<FGameTypeInfo>& ArenaGUI2K4Pages::GetGameTypes()
{
	static TArray<FGameTypeInfo> Types;
	static bool bLocalized = false;
	if (Types.Num() > 0 && (bLocalized || !ArenaLocalMessages::HasLocalization(TEXT("XGame"))))
	{
		return Types;
	}
	Types.Reset();
	// UT2K4Tab_GameTypeBase.GamePos order; the stock game class of each of our modes (its cache record has the
	// localized name, description and the UT2004Thumbnails shot)
	struct FMap { const TCHAR* Mode; const TCHAR* GameClass; const TCHAR* Shots; };
	static const FMap Map[] = {
		{ TEXT("AS"), TEXT("UT2k4Assault.ASGameInfo"), TEXT("UT2004Thumbnails.Shaders.AssaultShots") },
		{ TEXT("ONS"), TEXT("Onslaught.ONSOnslaughtGame"), TEXT("UT2004Thumbnails.Shaders.OnslaughtShots") },
		{ TEXT("DM"), TEXT("XGame.xDeathMatch"), TEXT("UT2004Thumbnails.Shaders.DMShots") },
		{ TEXT("CTF"), TEXT("XGame.xCTFGame"), TEXT("UT2004Thumbnails.Shaders.CTFShots") },
		{ TEXT("TDM"), TEXT("XGame.xTeamGame"), TEXT("UT2004Thumbnails.Shaders.TDMShots") },
		{ TEXT("DOM"), TEXT("XGame.xDoubleDom"), TEXT("UT2004Thumbnails.Shaders.DOMShots") },
		{ TEXT("BR"), TEXT("XGame.xBombingRun"), TEXT("UT2004Thumbnails.Shaders.BRShots") },
		{ TEXT("MUT"), TEXT("BonusPack.xMutantGame"), TEXT("UT2004Thumbnails.Shaders.MutantShots") },
		{ TEXT("INV"), TEXT("SkaarjPack.Invasion"), TEXT("UT2004Thumbnails.Shaders.InvasionShots") },
		{ TEXT("LMS"), TEXT("BonusPack.xLastManStandingGame"), TEXT("UT2004Thumbnails.Shaders.LMSShots") },
	};
	TArray<const FArenaRulesEntry*> Modes = ArenaGameRulesRegistry::GetModes();
	auto AddMode = [&](const FArenaRulesEntry* E, const FMap* M)
	{
		FGameTypeInfo T;
		T.ModeId = E->ModeId;
		T.Name = E->DisplayName;
		if (M)
		{
			FString Pkg, Class;
			FString(M->GameClass).Split(TEXT("."), &Pkg, &Class);
			FString V;
			if (ArenaLocalMessages::Localize(Pkg, Class, TEXT("GameName"), V) && !V.IsEmpty()) { T.Name = V; bLocalized = true; }
			if (ArenaLocalMessages::Localize(Pkg, Class, TEXT("Description"), V) && !V.IsEmpty()) { T.Description = V; }
			T.Screenshot = M->Shots;
		}
		Types.Add(T);
	};
	for (const FMap& M : Map)
	{
		if (const FArenaRulesEntry* const* E = Modes.FindByPredicate([&M](const FArenaRulesEntry* R) { return FString(R->ModeId).Equals(M.Mode, ESearchCase::IgnoreCase); }))
		{
			AddMode(*E, &M);
		}
	}
	for (const FArenaRulesEntry* E : Modes)
	{
		if (!Types.ContainsByPredicate([E](const FGameTypeInfo& T) { return T.ModeId.Equals(E->ModeId, ESearchCase::IgnoreCase); }))
		{
			AddMode(E, nullptr);
		}
	}
	return Types;
}

const FGameTypeInfo* ArenaGUI2K4Pages::FindGameType(const FString& ModeId)
{
	return GetGameTypes().FindByPredicate([&ModeId](const FGameTypeInfo& T) { return T.ModeId.Equals(ModeId, ESearchCase::IgnoreCase); });
}

FString ArenaGUI2K4Pages::GetMapScreenshot(const FString& MapName)
{
	static TMap<FString, FString> Cache;
	if (const FString* Found = Cache.Find(MapName))
	{
		return *Found;
	}
	FString Result;
	// maps/<Map>/map.json carries the LevelInfo's Screenshot material ("Screenshot": "DM-Rankin.Screenshot")
	const FString File = ArenaRuntimeMap::GetMapDir(MapName) / TEXT("map.json");
	FString Text;
	if (!MapName.IsEmpty() && FFileHelper::LoadFileToString(Text, *File))
	{
		const int32 At = Text.Find(TEXT("\"Screenshot\": \""));
		if (At != INDEX_NONE)
		{
			const int32 Start = At + 15;
			const int32 End = Text.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
			if (End > Start)
			{
				Result = Text.Mid(Start, End - Start);
			}
		}
	}
	Cache.Add(MapName, Result);
	return Result;
}

const TArray<FMutatorInfo>& ArenaGUI2K4Pages::GetMutators()
{
	static TArray<FMutatorInfo> List;
	static bool bBuilt = false;
	if (bBuilt)
	{
		return List;
	}
	bBuilt = true;
	const FString System = InstallSystemDir();
	// the content cache's copies first (ArenaConvScripts: <cache>/scripts/System; ArenaConvLocalization: <cache>/localization)
	const FString CacheScripts = ArenaContent::GetCacheDir() / TEXT("scripts") / TEXT("System");
	const FString CacheLoc = ArenaContent::GetCacheDir() / TEXT("localization");
	TSet<FString> Seen;
	auto PackageExists = [&System, &CacheScripts](const FString& Pkg)
	{
		if (IFileManager::Get().FileExists(*(CacheScripts / Pkg + TEXT(".u"))))
		{
			return true;
		}
		if (!System.IsEmpty() && IFileManager::Get().FileExists(*(System / Pkg + TEXT(".u"))))
		{
			return true;
		}
		TArray<FString> Found;
		const FString UC = ArenaContent::GetUserContentDir();
		if (!UC.IsEmpty() && IFileManager::Get().DirectoryExists(*UC))
		{
			IFileManager::Get().FindFilesRecursive(Found, *UC, *(Pkg + TEXT(".u")), true, false);
		}
		return Found.Num() > 0;
	};
	auto AddRecord = [&](const TMap<FString, FString>& R)
	{
		const FString* Class = R.Find(TEXT("ClassName"));
		if (!Class || Class->IsEmpty() || Seen.Contains(Class->ToLower()))
		{
			return;
		}
		FString Pkg, Name;
		if (!Class->Split(TEXT("."), &Pkg, &Name) || !PackageExists(Pkg))
		{
			return;
		}
		Seen.Add(Class->ToLower());
		FMutatorInfo M;
		M.ClassName = *Class;
		const FString* FB = R.Find(TEXT("FallbackName"));
		const FString* FD = R.Find(TEXT("FallbackDesc"));
		const FString* FN = R.Find(TEXT("FriendlyName"));
		const FString* DS = R.Find(TEXT("Description"));
		M.FriendlyName = FN ? LocRef(*FN, FB ? *FB : Name) : (FB ? *FB : Name);
		M.Description = DS ? LocRef(*DS, FD ? *FD : FString()) : (FD ? *FD : FString());
		if (const FString* GN = R.Find(TEXT("GroupName")))
		{
			M.GroupName = *GN;
		}
		// localized name / description from the package's .int when the record points elsewhere
		FString V;
		if (ArenaLocalMessages::Localize(Pkg, Name, TEXT("FriendlyName"), V) && !V.IsEmpty()) M.FriendlyName = V;
		if (ArenaLocalMessages::Localize(Pkg, Name, TEXT("Description"), V) && !V.IsEmpty()) M.Description = V;
		List.Add(M);
	};
	auto ScanUcl = [&](const FString& File)
	{
		TArray<FString> Lines;
		FFileHelper::LoadFileToStringArray(Lines, *File);
		for (const FString& L : Lines)
		{
			if (L.StartsWith(TEXT("Mutator=")))
			{
				AddRecord(ParseStruct(L.Mid(8)));
			}
		}
	};
	auto ScanInt = [&](const FString& File)
	{
		TArray<FString> Lines;
		FFileHelper::LoadFileToStringArray(Lines, *File);
		for (const FString& L : Lines)
		{
			if (L.StartsWith(TEXT("Object=")) && L.Contains(TEXT("MetaClass=Engine.Mutator")))
			{
				TMap<FString, FString> R = ParseStruct(L.Mid(7));
				TMap<FString, FString> Rec;
				if (const FString* N = R.Find(TEXT("Name"))) Rec.Add(TEXT("ClassName"), *N);
				if (const FString* D = R.Find(TEXT("Description"))) Rec.Add(TEXT("FallbackDesc"), *D);
				AddRecord(Rec);
			}
		}
	};
	{
		TArray<FString> Files;
		FString UclDir = CacheLoc;
		IFileManager::Get().FindFiles(Files, *(UclDir / TEXT("*.ucl")), true, false);
		if (Files.Num() == 0 && !System.IsEmpty())
		{
			UclDir = System;
			IFileManager::Get().FindFiles(Files, *(UclDir / TEXT("*.ucl")), true, false);
		}
		Files.Sort();
		for (const FString& F : Files)
		{
			ScanUcl(UclDir / F);
		}
	}
	const FString UC = ArenaContent::GetUserContentDir();
	if (!UC.IsEmpty() && IFileManager::Get().DirectoryExists(*UC))
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *UC, TEXT("*.ucl"), true, false);
		for (const FString& F : Files) ScanUcl(F);
		Files.Reset();
		IFileManager::Get().FindFilesRecursive(Files, *UC, TEXT("*.int"), true, false);
		for (const FString& F : Files) ScanInt(F);
	}
	// which ones the script runtime can run
	const bool bVM = ArenaUScript::IsAvailable() && ArenaUScript::EnsureRuntime();
	for (FMutatorInfo& M : List)
	{
		if (!bVM)
		{
			M.bCanRun = false;
			M.Reason = TEXT("The UnrealScript runtime is not available in this build.");
			continue;
		}
		FString Reason;
		M.bCanRun = ArenaUScript::CanRun(M.ClassName, &Reason);
		M.Reason = Reason;
		UE_LOG(LogOpenUT2k4, Verbose, TEXT("Menus: mutator %s: %s"), *M.ClassName, Reason.IsEmpty() ? TEXT("ok") : *Reason);
	}
	List.Sort([](const FMutatorInfo& A, const FMutatorInfo& B) { return A.FriendlyName < B.FriendlyName; });
	UE_LOG(LogOpenUT2k4, Log, TEXT("Menus: %d mutators found (%d can run)"), List.Num(),
		List.FilterByPredicate([](const FMutatorInfo& M) { return M.bCanRun; }).Num());
	return List;
}

// =================================================================================================
// Automation
// =================================================================================================

bool FAutomation::Claim() const
{
	static const FAutomation* Owner = this;
	return Owner == this;
}

TArray<TPair<FString, FString>> FAutomation::Tick(FGUI& G, double Now, UWorld* World)
{
	TArray<TPair<FString, FString>> Out;
#if !UE_BUILD_SHIPPING
	if (!bParsed)
	{
		bParsed = true;
		FString Script;
		if (FParse::Value(FCommandLine::Get(), TEXT("AutoMenuScript="), Script, false))
		{
			TArray<FString> Parts;
			Script.ParseIntoArray(Parts, TEXT(";"), true);
			for (const FString& P : Parts)
			{
				FString T, Cmd;
				if (!P.Split(TEXT(":"), &T, &Cmd))
				{
					continue;
				}
				FCmd C;
				C.At = FCString::Atof(*T);
				if (!Cmd.Split(TEXT("="), &C.Name, &C.Value))
				{
					C.Name = Cmd;
				}
				C.Name.TrimStartAndEndInline();
				Cmds.Add(C);
			}
			UE_LOG(LogOpenUT2k4, Log, TEXT("AutoMenuScript: %d commands"), Cmds.Num());
		}
	}
	if (Cmds.Num() == 0)
	{
		return Out;
	}
	if (Start < 0.0)
	{
		Start = Now;
	}
	const double T = Now - Start;
	for (FCmd& C : Cmds)
	{
		if (C.bDone || T < C.At)
		{
			continue;
		}
		C.bDone = true;
		UE_LOG(LogOpenUT2k4, Log, TEXT("AutoMenuScript %.1fs: %s=%s"), T, *C.Name, *C.Value);
		if (C.Name == TEXT("Key"))
		{
			FGUI::InjectKey(FKey(*C.Value));
		}
		else if (C.Name == TEXT("Click"))
		{
			FString X, Y;
			if (C.Value.Split(TEXT(","), &X, &Y))
			{
				G.InjectClick(FCString::Atof(*X), FCString::Atof(*Y));
			}
		}
		else if (C.Name == TEXT("Type"))
		{
			FGUI::InjectText(C.Value);
		}
		else if (C.Name == TEXT("Wheel"))
		{
			G.InjectWheel(FCString::Atoi(*C.Value));
		}
		else if (C.Name == TEXT("Shot"))
		{
			const FString File = FPaths::ScreenShotDir() / FString::Printf(TEXT("menu_%s.png"), *C.Value);
			FScreenshotRequest::RequestScreenshot(File, false, false);
			UE_LOG(LogOpenUT2k4, Log, TEXT("AutoMenuScript: screenshot %s"), *File);
		}
		else if (C.Name == TEXT("Quit"))
		{
			if (GEngine && World)
			{
				GEngine->Exec(World, TEXT("quit"));
			}
		}
		else
		{
			Out.Add(TPair<FString, FString>(C.Name, C.Value));
		}
		// one command per frame so injected input is processed in order
		break;
	}
#endif
	return Out;
}

