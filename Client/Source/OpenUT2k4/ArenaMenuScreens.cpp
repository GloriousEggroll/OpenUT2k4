#include "ArenaMenuScreens.h"
#include "ArenaPlayerSettingsTab.h"
#include "OpenUT2k4.h"
#include "ArenaArtHUD.h"
#include "ArenaBranding.h"
#include "ArenaGameRules.h"
#include "ArenaGameSession.h"
#include "ArenaLevelBuilder.h"
#include "ArenaMenuGameMode.h"
#include "ArenaUserSettings.h"
#include "Net/ArenaServerBrowserPage.h"
#if ARENA_HAS_SINGLEPLAYER
#include "ArenaSinglePlayer.h"
#endif

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

using namespace ArenaGUI2K4;
using namespace ArenaGUI2K4Pages;

namespace ArenaMenuScreensLocal
{
	/** UT2k4MainMenu buttons, in its order (TabOrder 0..6) with the stock captions / hints from GUI2K4.int: the
	 *  Animate() targets of MenuIn_OnArrival (WinLeft / WinTop), WinWidth, the off-screen WinLeft of Opened() and the
	 *  Animate() time. WinHeight is 0.075 for all. */
	struct FMainButton
	{
		const TCHAR* Id;
		const TCHAR* LocKey;
		const TCHAR* Caption;
		const TCHAR* Hint;
		float X, Y, W, StartX, AnimTime;
	};
	const FMainButton MainButtons[] = {
		{ TEXT("SinglePlayer"), TEXT("SinglePlayerButton"), TEXT("Single Player"), TEXT("Play the tournament ladder"), 0.315359f, 0.368813f, 0.715022f, 1.0f, 0.35f },
		{ TEXT("JoinGame"), TEXT("MultiplayerButton"), TEXT("Join Game"), TEXT("Find a server on your LAN or the internet and join it"), 0.363246f, 0.449282f, 0.659899f, 1.15f, 0.40f },
		{ TEXT("HostGame"), TEXT("HostButton"), TEXT("Host Game"), TEXT("Start a server for other players to join"), 0.395097f, 0.534027f, 0.627131f, 1.3f, 0.45f },
		{ TEXT("InstantAction"), TEXT("InstantActionButton"), TEXT("Instant Action"), TEXT("Play a quick match against bots"), 0.423640f, 0.618619f, 0.593666f, 1.45f, 0.5f },
		// ModsAndDemosButton: our community page is the UT2004 Content screen (install, downloads, custom content)
		{ TEXT("Community"), TEXT("ModsAndDemosButton"), TEXT("Community"), TEXT("Your UT2004 install and custom maps, models and mods"), 0.433406f, 0.705859f, 0.574135f, 1.6f, 0.55f },
		{ TEXT("Settings"), TEXT("SettingsButton"), TEXT("Settings"), TEXT("Change your display, sound and control settings"), 0.434477f, 0.800327f, 0.580478f, 1.75f, 0.6f },
		{ TEXT("Quit"), TEXT("QuitButton"), TEXT("Exit"), TEXT("Leave the game"), 0.424711f, 0.887567f, 0.526767f, 1.9f, 0.65f },
	};
	/** UT2k4MainMenu.CharShots: the character renders of the main menu */
	const TCHAR* const CharShots[] = { TEXT("2K4Menus.MainMenu.Char01"), TEXT("2K4Menus.MainMenu.char02"), TEXT("2K4Menus.MainMenu.char03") };

	const TCHAR* const SkillNames[8] = {
		TEXT("Novice"), TEXT("Average"), TEXT("Experienced"), TEXT("Skilled"),
		TEXT("Adept"), TEXT("Masterful"), TEXT("Inhuman"), TEXT("Godlike")
	};

	float Ease(double A)
	{
		const float T = float(FMath::Clamp(A, 0.0, 1.0));
		return 1.f - (1.f - T) * (1.f - T);
	}

	/** Strip characters that would break the travel URL. */
	FString SanitizeOption(const FString& In, int32 MaxLen = 24)
	{
		FString Out;
		for (const TCHAR C : In)
		{
			if (C == TCHAR('?') || C == TCHAR('=') || C == TCHAR('#') || C == TCHAR('/') || C == TCHAR('\\') || C == TCHAR('"'))
			{
				continue;
			}
			Out.AppendChar(FChar::IsWhitespace(C) ? TCHAR('_') : C);
		}
		return Out.IsEmpty() ? FString(TEXT("Player")) : Out.Left(MaxLen);
	}

	float Hash01(int32 N)
	{
		uint32 X = uint32(N) * 747796405u + 2891336453u;
		X = ((X >> ((X >> 28u) + 4u)) ^ X) * 277803737u;
		X = (X >> 22u) ^ X;
		return float(X & 0xFFFFFF) / float(0xFFFFFF);
	}

	/** UT2k4MainMenu backdrop: PageBackground (mmbgnd, 1024 x 768 of the texture) covering the screen, the panning
	 *  title bands and the character render. False when the art is missing. */
	bool MainBackdrop(FGUI& G, float CharAlpha, int32 CharShot, double Time)
	{
		const float W = G.ScreenW(), H = G.ScreenH();
		// cover the screen without stretching: crop the 4:3 picture (anchored a little above the middle)
		const float SrcW = 1024.f, SrcH = 768.f;
		float U0 = 0.f, V0 = 0.f, U1 = SrcW, V1 = SrcH;
		if (W / H > SrcW / SrcH)
		{
			const float Vis = SrcW * H / W;
			V0 = (SrcH - Vis) * 0.4f;
			V1 = V0 + Vis;
		}
		else
		{
			const float Vis = SrcH * W / H;
			U0 = (SrcW - Vis) * 0.5f;
			U1 = U0 + Vis;
		}
		if (!G.ImageUV(TEXT("2K4Menus.MainMenu.mmbgnd"), G.Screen(), U0 / 1024.f, V0 / 1024.f, U1 / 1024.f, V1 / 1024.f, FLinearColor::White))
		{
			return false;
		}
		const FRect P = G.Page();
		const float Repeat = W / P.W; // the bands keep the texture's proportions of the 4:3 layout and run across the screen
		const float Lead = (P.X / P.W);
		// iPanHuge (ScrollHuge, PanRate -0.075, alpha 40), iPanBig (ScrollBig, alpha 75), iPanSmall (ScrollSmall, PanRate -0.15)
		struct FBand { const TCHAR* Tex; float Top, Height, Rate, Alpha; };
		const FBand Bands[] = {
			{ TEXT("2K4Menus.MainMenu.ScrollHuge"), 0.001719f, 0.367969f, -0.075f, 40.f / 255.f },
			{ TEXT("2K4Menus.MainMenu.ScrollBig"), 0.081771f, 0.152422f, 0.1f, 75.f / 255.f },
			{ TEXT("2K4Menus.MainMenu.ScrollSmall"), 0.226042f, 0.04f, -0.15f, 1.f },
		};
		for (const FBand& B : Bands)
		{
			const float U = float(FMath::Fmod(Time * B.Rate, 1.0)) - Lead;
			G.ImageUV(B.Tex, FRect(0.f, B.Top * H, W, B.Height * H), -U, 0.f, -U + Repeat, 1.f, FLinearColor(1.f, 1.f, 1.f, B.Alpha * CharAlpha));
		}
		// ImgBkChar: the character render (1024 x 768 of the texture) over the 4:3 page
		const FBox2f Region(FVector2f(0.f, 0.f), FVector2f(1024.f, 768.f));
		G.Image(CharShots[FMath::Abs(CharShot) % UE_ARRAY_COUNT(CharShots)], P, EImgStyle::Scaled, FLinearColor(1.f, 1.f, 1.f, CharAlpha), &Region);
		return true;
	}
}

using namespace ArenaMenuScreensLocal;

// =================================================================================================
// State
// =================================================================================================

FArenaMenuScreens::FArenaMenuScreens(AHUD* InOwner)
	: Owner(InOwner)
{
	CharShot = FMath::RandRange(0, int32(UE_ARRAY_COUNT(CharShots)) - 1);
}

FArenaMenuScreens::~FArenaMenuScreens() = default;

const FArenaRulesEntry* FArenaMenuScreens::GetModeEntry() const
{
	const TArray<const FArenaRulesEntry*> Modes = ArenaGameRulesRegistry::GetModes();
	return Modes.Num() > 0 ? Modes[FMath::Clamp(ModeIndex, 0, Modes.Num() - 1)] : nullptr;
}

const UArenaGameRules* FArenaMenuScreens::GetModeDefaults() const
{
	const FArenaRulesEntry* Entry = GetModeEntry();
	return Entry ? Entry->GetDefaults() : nullptr;
}

FString FArenaMenuScreens::GetModeKey() const
{
	const FArenaRulesEntry* Entry = GetModeEntry();
	return Entry ? FString(Entry->ModeId) : FString(TEXT("DM"));
}

int32 FArenaMenuScreens::FindModeIndex(const FString& ModeId)
{
	const TArray<const FArenaRulesEntry*> Modes = ArenaGameRulesRegistry::GetModes();
	for (int32 i = 0; i < Modes.Num(); ++i)
	{
		if (ModeId.Equals(Modes[i]->ModeId, ESearchCase::IgnoreCase))
		{
			return i;
		}
	}
	return 0;
}

void FArenaMenuScreens::LoadFromSettings()
{
	const UArenaUserSettings* S = UArenaUserSettings::Get();
	ModeIndex = FindModeIndex(S->LastMode);
	LayoutId = S->LastLayout;
	NumBots = FMath::Clamp(S->LastBots, 0, 15);
	BotSkill = FMath::Clamp(S->LastSkill, 0, 7);
	GoalScore = FMath::Clamp(S->LastGoal, 0, 100);
	TimeLimit = FMath::Clamp(S->LastTimeLimit, 0, 60);
	bInstagib = S->bLastInstagib;
	Team = FMath::Clamp(S->LastTeam, 0, 1);
	bOfficialMapsOnly = S->bLastOfficialMapsOnly;
	S->LastMutators.ParseIntoArray(Mutators, TEXT(","), true);
	if (const UArenaGameRules* Defaults = GetModeDefaults())
	{
		int32 Min = 0, Max = 100, Step = 5;
		Defaults->GetGoalRange(Min, Max, Step);
		if (GoalScore > Max || GoalScore < Min)
		{
			GoalScore = Defaults->GetDefaultGoalScore();
		}
	}
	ValidateLayout();
}

bool FArenaMenuScreens::GetTranslocatorSetting() const
{
	const FString Prefix = GetModeKey() + TEXT("=");
	for (const FString& E : UArenaUserSettings::Get()->LastTranslocator)
	{
		if (E.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			return E.RightChop(Prefix.Len()) == TEXT("1");
		}
	}
	const UArenaGameRules* Defaults = GetModeDefaults();
	return Defaults && Defaults->GetDefaultTranslocator();
}

void FArenaMenuScreens::SetTranslocatorSetting(bool bOn)
{
	UArenaUserSettings* S = UArenaUserSettings::Get();
	const FString Prefix = GetModeKey() + TEXT("=");
	S->LastTranslocator.RemoveAll([&Prefix](const FString& E) { return E.StartsWith(Prefix, ESearchCase::IgnoreCase); });
	S->LastTranslocator.Add(Prefix + (bOn ? TEXT("1") : TEXT("0")));
	S->Save();
}

void FArenaMenuScreens::SaveToSettings() const
{
	UArenaUserSettings* S = UArenaUserSettings::Get();
	S->LastMode = GetModeKey();
	S->LastLayout = LayoutId;
	S->LastBots = NumBots;
	S->LastSkill = BotSkill;
	S->LastGoal = GoalScore;
	S->LastTimeLimit = TimeLimit;
	S->bLastInstagib = bInstagib;
	S->LastTeam = Team;
	S->bLastOfficialMapsOnly = bOfficialMapsOnly;
	S->LastMutators = FString::Join(Mutators, TEXT(","));
	S->Save();
}

TArray<int32> FArenaMenuScreens::GetValidLayoutIndices() const
{
	const TArray<FArenaLayoutInfo> Layouts = AArenaLevelBuilder::GetLayouts();
	const FArenaRulesEntry* Entry = GetModeEntry();
	TArray<int32> Result;
	for (int32 i = 0; i < Layouts.Num(); ++i)
	{
		if ((Entry ? Entry->SupportsLayout(Layouts[i]) : Layouts[i].bSupportsDM) && !(bOfficialMapsOnly && Layouts[i].bCustom))
		{
			Result.Add(i);
		}
	}
	return Result;
}

bool FArenaMenuScreens::HasMaps() const
{
	return AArenaLevelBuilder::HasAnyLayouts();
}

void FArenaMenuScreens::ValidateLayout()
{
	const FArenaRulesEntry* Entry = GetModeEntry();
	const UArenaGameRules* Defaults = GetModeDefaults();
	const bool bOfficial = bOfficialMapsOnly;
	LayoutId = AArenaLevelBuilder::ChooseLayout(LayoutId, Defaults ? Defaults->GetDefaultLayout() : FString(),
		[Entry, bOfficial](const FArenaLayoutInfo& L) { return (Entry ? Entry->SupportsLayout(L) : L.bSupportsDM) && !(bOfficial && L.bCustom); });
}

void FArenaMenuScreens::SetMode(int32 NewModeIndex)
{
	const int32 NumModes = FMath::Max(1, ArenaGameRulesRegistry::GetModes().Num());
	int32 OldMin = 0, OldMax = 100, OldStep = 5;
	if (const UArenaGameRules* Old = GetModeDefaults())
	{
		Old->GetGoalRange(OldMin, OldMax, OldStep);
	}
	ModeIndex = FMath::Clamp(NewModeIndex, 0, NumModes - 1);
	if (const UArenaGameRules* New = GetModeDefaults())
	{
		// different goal scales (frags vs captures): start from the new mode's default
		int32 Min = 0, Max = 100, Step = 5;
		New->GetGoalRange(Min, Max, Step);
		if (Min != OldMin || Max != OldMax || Step != OldStep || GoalScore < Min || GoalScore > Max)
		{
			GoalScore = New->GetDefaultGoalScore();
		}
	}
	ValidateLayout();
}

void FArenaMenuScreens::SetPage(EPage NewPage)
{
	if ((Page == EPage::InstantAction || Page == EPage::HostGame) && NewPage != Page)
	{
		SaveToSettings();
	}
	if (Page == EPage::Settings && NewPage != EPage::Settings)
	{
		UArenaUserSettings::Get()->Save();
	}
	Page = NewPage;
	PageOpenedAt = G.Time();
	PendingAction = NAME_None;
	G.ResetInteraction();
	G.SetFocus(0);
	if (NewPage == EPage::InstantAction || NewPage == EPage::HostGame)
	{
		ValidateLayout();
		HostMessage.Reset();
	}
	if (NewPage == EPage::SinglePlayer)
	{
#if ARENA_HAS_SINGLEPLAYER
		if (!SinglePlayer)
		{
			SinglePlayer = MakeUnique<FArenaSinglePlayerPage>(Owner.Get());
		}
		SinglePlayer->OnOpened();
#else
		Page = EPage::Main;
#endif
	}
	if (NewPage == EPage::ServerBrowser)
	{
		if (!Browser)
		{
			Browser = MakeUnique<FArenaServerBrowserPage>();
		}
		Browser->OnOpened();
	}
}

void FArenaMenuScreens::StartMatch(EStart How)
{
	ValidateLayout();
	SaveToSettings();
	AHUD* HUD = Owner.Get();
	if (LayoutId.IsEmpty() || !HUD)
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("No map available for %s, match not started"), *GetModeKey());
		return;
	}
	const UArenaUserSettings* S = UArenaUserSettings::Get();
	FString Options = FString::Printf(TEXT("Mode=%s?Layout=%s?Bots=%d?Skill=%d?Goal=%d?Time=%d?Instagib=%d?Team=%d?Name=%s"),
		*GetModeKey(), *SanitizeOption(LayoutId, 128), NumBots, BotSkill, GoalScore, TimeLimit, bInstagib ? 1 : 0, Team, *SanitizeOption(S->PlayerName));
	Options += FString::Printf(TEXT("?Translocator=%d"), GetTranslocatorSetting() ? 1 : 0);
	// UT2004 syntax: ?Mutator=Package.Class,Package.Class (runs through the UnrealScript runtime)
	TArray<FString> Runnable;
	for (const FString& M : Mutators)
	{
		const FMutatorInfo* Info = GetMutators().FindByPredicate([&M](const FMutatorInfo& I) { return I.ClassName.Equals(M, ESearchCase::IgnoreCase); });
		if (Info && Info->bCanRun)
		{
			Runnable.Add(SanitizeOption(M, 128));
		}
	}
	if (Runnable.Num() > 0)
	{
		Options += TEXT("?Mutator=") + FString::Join(Runnable, TEXT(","));
	}
	// runtime content builds every map into /Game/Maps/Arena; cooked imports are their own levels
	const FString Level = AArenaLevelBuilder::GetLevelForLayout(LayoutId);
	if (How == EStart::Dedicated)
	{
		// UT2K4GamePageMP.StartGame(bAlt): a separate server process; this game stays in the menu
		FString Error;
		const FString Url = Level + TEXT("?") + Options + ArenaHost::BuildServerOptions(false);
		HostMessage = ArenaHost::LaunchDedicated(Url, Error)
			? FString::Printf(TEXT("Dedicated server starting on port %d: join it from Join Game > LAN."), UArenaUserSettings::Get()->HostPort)
			: Error;
		return;
	}
	if (How == EStart::Listen)
	{
		// UT2K4GamePageMP.StartGame: ClientTravel(GameURL $ "?Listen")
		Options += ArenaHost::BuildServerOptions(true);
		ArenaHost::SetListenPort(UArenaUserSettings::Get()->HostPort);
	}
	const int32 PwAt = Options.Find(TEXT("?GamePassword="), ESearchCase::IgnoreCase);
	UE_LOG(LogOpenUT2k4, Log, TEXT("Starting match: %s?%s"), *Level, *(PwAt >= 0 ? Options.Left(PwAt) + TEXT(" (password set)") : Options));
	UGameplayStatics::OpenLevel(HUD, FName(*Level), true, Options);
}

// =================================================================================================
// Frame
// =================================================================================================

void FArenaMenuScreens::Draw(UCanvas* Canvas)
{
	AHUD* HUD = Owner.Get();
	if (!Canvas || !HUD)
	{
		return;
	}
	if (!bLoaded)
	{
		bLoaded = true;
		LoadFromSettings();
	}
	G.Begin(Canvas, HUD->GetOwningPlayerController(), true);
	if (PageOpenedAt < 0.0)
	{
		PageOpenedAt = G.Time();
	}
	RunLegacyTestHooks();
	// the script runs in the first menu of the process (not again when a match returns to the menu)
	if (Automation.Claim())
	{
		for (const TPair<FString, FString>& Cmd : Automation.Tick(G, G.Time(), HUD->GetWorld()))
		{
			ApplyTestCommand(Cmd.Key, Cmd.Value);
		}
	}

#if ARENA_HAS_SINGLEPLAYER
	// back from a Single Player match: straight to the tournament page
	if (!bCheckedSinglePlayerStart)
	{
		bCheckedSinglePlayerStart = true;
		if (Page == EPage::Main && FArenaSinglePlayerPage::ShouldOpenOnMenuStart())
		{
			SetPage(EPage::SinglePlayer);
		}
	}
#endif

	switch (Page)
	{
	case EPage::InstantAction:
	case EPage::HostGame:
		DrawInstantActionPage();
		break;
	case EPage::SinglePlayer:
#if ARENA_HAS_SINGLEPLAYER
		if (!SinglePlayer || SinglePlayer->Draw(G))
#endif
		{
			SetPage(EPage::Main);
		}
		break;
	case EPage::Settings:
		if (Settings.Draw(G))
		{
			SetPage(EPage::Main);
		}
		break;
	case EPage::ServerBrowser:
		if (!Browser || Browser->Draw(G))
		{
			SetPage(EPage::Main);
		}
		break;
	case EPage::Main:
	default:
		DrawMainPage();
		break;
	}
	G.End();
}

void FArenaMenuScreens::ApplyTestCommand(const FString& Name, const FString& Value)
{
	if (Name == TEXT("Page"))
	{
		bQuitPage = Value == TEXT("Quit");
		EPage To = EPage::Main;
		if (Value == TEXT("InstantAction")) To = EPage::InstantAction;
		else if (Value == TEXT("Settings")) To = EPage::Settings;
		else if (Value == TEXT("ServerBrowser")) To = EPage::ServerBrowser;
		else if (Value == TEXT("HostGame")) To = EPage::HostGame;
		else if (Value == TEXT("SinglePlayer")) To = EPage::SinglePlayer;
		SetPage(To);
		if (Value == TEXT("Quit"))
		{
			// as after the Exit button: the menu has animated out
			bQuitPage = true;
			PendingAction = FName(TEXT("QuitShown"));
			ActionAt = G.Time() - 1.0;
		}
	}
	else if (Name == TEXT("Tab"))
	{
		if (Page == EPage::Settings)
		{
			Settings.Tab = FCString::Atoi(*Value);
		}
		else if (Page == EPage::ServerBrowser && Browser)
		{
			Browser->ApplyTestCommand(TEXT("BrowserTab"), Value);
		}
		else if (Page == EPage::HostGame)
		{
			HostTab = FCString::Atoi(*Value);
		}
		else
		{
			IATab = FCString::Atoi(*Value);
		}
		G.ResetInteraction();
	}
	else if (Name == TEXT("Mode"))
	{
		SetMode(FindModeIndex(Value));
	}
	else if (Name == TEXT("Layout"))
	{
		LayoutId = Value;
		ValidateLayout();
	}
	else if (Name == TEXT("Mutator"))
	{
		Mutators.AddUnique(Value);
	}
	else if (Name == TEXT("HostStart"))
	{
		// test hook: LISTEN (1) / DEDICATED (2) from the Host Game page
		StartMatch(Value == TEXT("2") ? EStart::Dedicated : EStart::Listen);
	}
	else if (Name == TEXT("HostPort") || Name == TEXT("HostName") || Name == TEXT("HostPassword"))
	{
		UArenaUserSettings* S = UArenaUserSettings::Get();
		if (Name == TEXT("HostPort")) S->HostPort = FMath::Clamp(FCString::Atoi(*Value), 1, 65534);
		else if (Name == TEXT("HostName")) S->HostServerName = Value.Replace(TEXT("_"), TEXT(" "));
		else S->HostPassword = Value;
	}
	else if (Name.StartsWith(TEXT("Player")))
	{
		// Settings > Player test hooks (ArenaPlayerSettingsTab.h)
		ArenaPlayerSettingsTab::ApplyTestCommand(Name, Value);
	}
	else if (Name.StartsWith(TEXT("SP")))
	{
#if ARENA_HAS_SINGLEPLAYER
		if (!SinglePlayer)
		{
			SinglePlayer = MakeUnique<FArenaSinglePlayerPage>(Owner.Get());
		}
		SinglePlayer->ApplyTestCommand(Name, Value);
#endif
	}
	else if (Name.StartsWith(TEXT("Browser")))
	{
		if (!Browser)
		{
			Browser = MakeUnique<FArenaServerBrowserPage>();
		}
		Browser->ApplyTestCommand(Name, Value);
	}
}

void FArenaMenuScreens::RunLegacyTestHooks()
{
#if !UE_BUILD_SHIPPING
	AHUD* HUD = Owner.Get();
	UWorld* World = HUD ? HUD->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}
	const TCHAR* Cmd = FCommandLine::Get();
	// -AutoMenuPage=InstantAction|Settings|ServerBrowser|Quit opens that page once (-AutoMenuTab=<n> picks a tab; -AutoMenuMode /
	// -AutoMenuLayout; the server browser also takes -AutoBrowserTab=<n or id>, -AutoBrowserSelect=<row or address>,
	// -AutoBrowserSort=<column>, -AutoBrowserOpenIP=<address>, see FArenaServerBrowserPage::ApplyTestCommand)
	static bool bAutoPage = false;
	FString Value;
	if (!bAutoPage && FParse::Value(Cmd, TEXT("AutoMenuPage="), Value))
	{
		bAutoPage = true;
		FString V2;
		if (FParse::Value(Cmd, TEXT("AutoMenuMode="), V2))
		{
			ModeIndex = FindModeIndex(V2);
		}
		if (FParse::Value(Cmd, TEXT("AutoMenuLayout="), V2, false))
		{
			LayoutId = V2;
		}
		ValidateLayout();
		// server browser options first, so opening the page fetches the chosen tab only
		for (const TCHAR* Key : { TEXT("BrowserTab"), TEXT("BrowserSort"), TEXT("BrowserOpenIP") })
		{
			if (FParse::Value(Cmd, *(FString(TEXT("Auto")) + Key + TEXT("=")), V2, false))
			{
				ApplyTestCommand(Key, V2);
			}
		}
		ApplyTestCommand(TEXT("Page"), Value);
		if (FParse::Value(Cmd, TEXT("AutoMenuTab="), V2))
		{
			ApplyTestCommand(TEXT("Tab"), V2);
		}
	}
	// -AutoMenuShot=<sec>: screenshot to Saved/Screenshots/menu.png, then quit 2 s later
	static int32 ShotState = 0;
	float ShotAt = -1.f;
	if (ShotState < 2 && FParse::Value(Cmd, TEXT("AutoMenuShot="), ShotAt))
	{
		const float Now = World->GetRealTimeSeconds();
		if (ShotState == 0 && Now >= ShotAt)
		{
			ShotState = 1;
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Screenshots/menu.png"), false, false);
		}
		else if (ShotState == 1 && Now >= ShotAt + 2.f)
		{
			ShotState = 2;
			GEngine->Exec(World, TEXT("quit"));
		}
	}
	// -AutoMenuStart=<sec> starts the Instant Action match (-AutoMenuMode=<ModeId>, -AutoMenuLayout=<layout id>)
	static bool bAutoStarted = false;
	float StartAt = -1.f;
	if (!bAutoStarted && FParse::Value(Cmd, TEXT("AutoMenuStart="), StartAt) && World->GetRealTimeSeconds() >= StartAt)
	{
		bAutoStarted = true;
		if (FParse::Value(Cmd, TEXT("AutoMenuMode="), Value))
		{
			ModeIndex = FindModeIndex(Value);
		}
		if (FParse::Value(Cmd, TEXT("AutoMenuLayout="), Value, false))
		{
			LayoutId = Value;
		}
		StartMatch();
	}
#endif
}

// =================================================================================================
// Backdrop
// =================================================================================================

void FArenaMenuScreens::DrawProceduralBackground(const FArenaDraw& D, float Time)
{
	// deep blue gradient with a slow horizon glow and drifting particles (stand-in before any content exists)
	const float W = D.W(), H = D.H();
	D.VerticalGradient(0.f, 0.f, W, H, FLinearColor(0.012f, 0.03f, 0.09f, 1.f), FLinearColor(0.003f, 0.006f, 0.02f, 1.f), 32);
	const float HorizonY = H * 0.62f;
	D.VerticalGradient(0.f, HorizonY - H * 0.18f, W, H * 0.18f, FLinearColor(0.1f, 0.3f, 0.8f, 0.f), FLinearColor(0.15f, 0.4f, 1.f, 0.12f + 0.04f * FMath::Sin(Time * 0.7f)), 16);
	D.Rect(0.f, HorizonY, W, FMath::Max(1.f, D.S(2.f)), FLinearColor(0.35f, 0.65f, 1.f, 0.35f));
	const int32 NumH = 14;
	const float Scroll = FMath::Frac(Time * 0.25f);
	for (int32 i = 0; i < NumH; ++i)
	{
		const float T = (i + Scroll) / NumH;
		const float Y = HorizonY + (H - HorizonY) * T * T;
		D.Rect(0.f, Y, W, FMath::Max(1.f, D.S(1.5f) * (0.5f + T)), FLinearColor(0.25f, 0.55f, 1.f, 0.05f + 0.25f * T));
	}
	for (int32 i = -12; i <= 12; ++i)
	{
		D.Line(W * 0.5f + i * W * 0.03f, HorizonY, W * 0.5f + i * W * 0.16f, H, FLinearColor(0.25f, 0.55f, 1.f, 0.12f), FMath::Max(1.f, D.S(1.5f)));
	}
	for (int32 i = 0; i < 60; ++i)
	{
		const float Speed = 0.01f + 0.03f * Hash01(i * 3 + 1);
		const float PX = FMath::Frac(Hash01(i * 3) + Time * Speed) * W;
		const float PY = HorizonY * (0.05f + 0.9f * Hash01(i * 3 + 2)) + FMath::Sin(Time * 0.8f + i) * D.S(6.f);
		const float Sz = FMath::Max(1.f, D.S(1.5f + 2.5f * Hash01(i * 7 + 5)));
		const float Tw = 0.25f + 0.35f * (0.5f + 0.5f * FMath::Sin(Time * (1.f + Hash01(i)) * 2.f + i));
		D.Rect(PX, PY, Sz, Sz, (i % 5 == 0) ? FLinearColor(1.f, 0.55f, 0.2f, Tw) : FLinearColor(0.5f, 0.75f, 1.f, Tw));
	}
	D.VerticalGradient(0.f, 0.f, W, H * 0.2f, FLinearColor(0.f, 0.f, 0.f, 0.5f), FLinearColor(0.f, 0.f, 0.f, 0.f), 10);
	D.VerticalGradient(0.f, H * 0.85f, W, H * 0.15f, FLinearColor(0.f, 0.f, 0.f, 0.f), FLinearColor(0.f, 0.f, 0.f, 0.6f), 10);
}

// =================================================================================================
// Main menu (UT2k4MainMenu)
// =================================================================================================

void FArenaMenuScreens::DrawMainPage()
{
	const FRect P = G.Page();
	const float H = G.ScreenH();
	const double T = G.Time() - PageOpenedAt;
	const bool bLeaving = !PendingAction.IsNone();
	const double TL = bLeaving ? G.Time() - ActionAt : 0.0;
	// Opened(): components off screen; Timer() after 0.5 s: logo, symbol and corner view slide in (0.35 s);
	// MenuIn_OnArrival: the character fades in (0.75 s) and the buttons slide in from the right
	const float In = Ease((T - 0.5) / 0.35);
	const float Out = bLeaving ? Ease(TL / 0.35) : 0.f;
	const float K = In * (1.f - Out);
	const float CharAlpha = FMath::Clamp(float((T - 0.85) / 0.75), 0.f, 1.f) * (1.f - Out);
	const float Time = float(G.Time());

	if (!MainBackdrop(G, CharAlpha, CharShot, G.Time()))
	{
		DrawProceduralBackground(G.Draw(), Time);
	}

	// ImgUT2Logo: 2K4Menus.MainMenu.2K4Logo at (0.007226, 0.016926), 0.638868 x 0.392579, from WinTop -0.392579
	{
		const float Y = FMath::Lerp(-0.392579f, 0.016926f, K) * H;
		const FRect LogoR(P.X + 0.007226f * P.W, Y, 0.638868f * P.W, 0.392579f * H);
		// ISTY_Scaled into a 4:3 rectangle stretched the 2:1 logo a little; we keep its proportions (left aligned)
		const FRect Fit(LogoR.X, LogoR.Y, FMath::Min(LogoR.W, LogoR.H * 2.f), FMath::Min(LogoR.H, LogoR.W * 0.5f));
		if (!G.Image(TEXT("2K4Menus.MainMenu.2K4Logo"), Fit, EImgStyle::Scaled, FLinearColor::White))
		{
			// our title before content exists
			const FFontRef F = G.Font(TEXT("UT2MainMenuFont"));
			G.Text(F, ArenaBranding::GameTitle(), LogoR.X + LogoR.W * 0.5f, LogoR.Y + LogoR.H * 0.3f, FArenaDraw::UTColor(FColor(244, 237, 205)), EAlign::Center, true);
		}
	}

	// buttons (STY2TextButton captions with the TextButtonEffect shadow) and their highlight bars (MainMenu.highlight)
	const bool bContent = static_cast<bool>(AArenaMenuHUD::OnOpenContentSetup());
	const FStyle& S = GetStyle(TEXT("TextButton"));
	const FStyle& Effect = GetStyle(TEXT("TextButtonEffect"));
	const bool bReady = !bLeaving && !bQuitPage && T > 0.85;
	const float BtnH = 0.075f * H;
	for (int32 i = 0; i < int32(UE_ARRAY_COUNT(MainButtons)); ++i)
	{
		const FMainButton& B = MainButtons[i];
		const FString BtnId = B.Id;
		bool bEnabled = true;
		if (BtnId == TEXT("InstantAction") || BtnId == TEXT("HostGame"))
		{
			bEnabled = HasMaps();
		}
		else if (BtnId == TEXT("SinglePlayer"))
		{
			bEnabled = ARENA_HAS_SINGLEPLAYER && HasMaps();
		}
		else if (BtnId == TEXT("Community"))
		{
			bEnabled = bContent;
		}
		const float KB = Ease((T - 0.85) / B.AnimTime) * (1.f - Out);
		const FRect R(P.X + FMath::Lerp(B.StartX, B.X, KB) * P.W, B.Y * H, B.W * P.W, BtnH);
		const uint32 Id = FGUI::Id(B.Id);
		const FString Caption = Loc(TEXT("GUI2K4"), TEXT("UT2k4MainMenu"), *(FString(B.LocKey) + TEXT(".Caption")), B.Caption);
		const FString BtnHint = Loc(TEXT("GUI2K4"), TEXT("UT2k4MainMenu"), *(FString(B.LocKey) + TEXT(".Hint")), B.Hint);
		const EMenuState State = bReady ? G.CustomWidget(Id, R, bEnabled, BtnHint, true) : EMenuState::Blurry;
		const FFontRef F = G.StyleFont(S, EMenuState::Focused, EFontScale::Small);
		const float CW = G.TextSize(F, Caption).X;
		// highlight: Tag 200 while focused, fading by 784 per second; drawn from the screen's left edge to the caption end + 16
		float& Tag = Highlight.FindOrAdd(i);
		Tag = State == EMenuState::Focused ? 200.f : FMath::Max(0.f, Tag - 784.f * G.DeltaTime());
		if (Tag > 0.f)
		{
			const FRect HR(0.f, R.Y, R.X + CW + G.L(16.f), R.H);
			G.Image(TEXT("2K4Menus.MainMenu.highlight"), HR, EImgStyle::PartialScaled, FLinearColor(1.f, 1.f, 1.f, Tag / 255.f));
		}
		const float Alpha = bEnabled ? 1.f : 0.35f;
		const float TY = R.Y + (R.H - F.Height) * 0.5f;
		const float Off = FMath::Max(1.f, FMath::RoundToFloat(G.L(2.f)));
		G.Text(F, Caption, R.X + Off, TY + Off, FArenaDraw::UTColor(Effect.FontColors[0], Alpha * 0.9f));
		G.Text(F, Caption, R.X, TY, FArenaDraw::UTColor(S.FontColors[int32(State == EMenuState::Disabled ? EMenuState::Blurry : State)], Alpha));
		if (bReady && bEnabled && G.WasClicked(Id, R))
		{
			G.PlaySound(ESound::Click);
			PendingAction = FName(B.Id);
			ActionAt = G.Time();
		}
	}

	// ImgTV: the corner view (ULogo.MainMenu.CornerMenuFinal: the action1..3 sequence), X1 0 Y1 1 X2 512 Y2 511,
	// at (-0.000977, 0.332292), 0.5 x 0.668619, sliding up from 1.668619; drawn over everything
	{
		const FRect TV(P.X - 0.000977f * P.W, FMath::Lerp(1.668619f, 0.332292f, K) * H, 0.5f * P.W, 0.668619f * H);
		const FAnimatedImage& Seq = FAnimatedImage::Get(TEXT("2K4Menus.MainMenu.ActionFader"));
		FString A, B;
		float Blend = 0.f;
		Seq.Sample(Time, A, B, Blend);
		const FBox2f Region(FVector2f(0.f, 1.f), FVector2f(512.f, 511.f));
		const float Side = FMath::Min(TV.W, TV.H);
		const FRect Sq(TV.X, TV.Y, Side, Side);
		if (!A.IsEmpty())
		{
			G.Image(A, Sq, EImgStyle::Scaled, FLinearColor(1.f, 1.f, 1.f, 1.f - Blend), &Region);
		}
		if (!B.IsEmpty() && Blend > 0.f)
		{
			G.Image(B, Sq, EImgStyle::Scaled, FLinearColor(1.f, 1.f, 1.f, Blend), &Region);
		}
	}

	// empty state: nothing to play until UT2004 content is converted
	if (!HasMaps() && bReady)
	{
		const FFontRef F = G.Font(TEXT("UT2SmallHeaderFont"));
		const float X = P.X + 0.43f * P.W, Y = 0.50f * H;
		G.Text(F, TEXT("No maps yet: the game uses the maps of your own UT2004 install."), X, Y, FArenaDraw::UTColor(FColor(255, 210, 0)), EAlign::Left, true);
		G.Text(G.Font(TEXT("UT2MenuFont")), bContent ? TEXT("Choose UT2004 Content to find and convert it.")
			: TEXT("Start the game with -UT2004Dir=<folder> or set UT2004Dir in Game.ini, then restart."), X, Y + F.Height * 1.2f, FLinearColor::White, EAlign::Left, true);
	}

	// our disclaimer and version (not part of UT2004's menu)
	{
		const FFontRef F = G.Font(TEXT("UT2SmallFont"));
		const float Y = H - F.Height - G.L(4.f);
		G.Text(F, ArenaBranding::Disclaimer(), G.ScreenW() * 0.5f, Y, FLinearColor(0.75f, 0.78f, 0.85f, 0.6f), EAlign::Center, true);
		G.Text(F, ArenaBranding::VersionTag(), G.ScreenW() - G.L(8.f), Y, FLinearColor(0.75f, 0.78f, 0.85f, 0.5f), EAlign::Right, true);
	}

	// MoveOn() after the out animation
	if (bLeaving && TL >= 0.36)
	{
		const FName Action = PendingAction;
		PendingAction = NAME_None;
		if (Action == TEXT("InstantAction"))
		{
			SetPage(EPage::InstantAction);
		}
		else if (Action == TEXT("Settings"))
		{
			SetPage(EPage::Settings);
		}
		else if (Action == TEXT("JoinGame"))
		{
			SetPage(EPage::ServerBrowser);
		}
		else if (Action == TEXT("HostGame"))
		{
			SetPage(EPage::HostGame);
		}
		else if (Action == TEXT("SinglePlayer"))
		{
			SetPage(EPage::SinglePlayer);
		}
		else if (Action == TEXT("Quit"))
		{
			bQuitPage = true;
			PendingAction = FName(TEXT("QuitShown"));
			ActionAt = G.Time() - 1.0;
		}
		else if (Action == TEXT("Community"))
		{
			PageOpenedAt = G.Time() - 0.5;
			if (AArenaMenuHUD::OnOpenContentSetup())
			{
				AArenaMenuHUD::OnOpenContentSetup()(Cast<AArenaMenuHUD>(Owner.Get()));
			}
			return;
		}
		else if (Action == TEXT("QuitShown"))
		{
			PendingAction = Action;
		}
	}
	if (bQuitPage)
	{
		DrawQuitPage();
	}
	else if (!bLeaving && G.BackPressed())
	{
		// UT2k4MainMenu.CanClose: Esc opens the quit page
		G.PlaySound(ESound::Click);
		PendingAction = FName(TEXT("Quit"));
		ActionAt = G.Time();
	}
}

void FArenaMenuScreens::DrawQuitPage()
{
	// UT2K4QuitPage (BlackoutWindow / MessageWindow): a band across the screen (WinTop 0.3, WinHeight 0.38) with the
	// Display2 frame, the question in UT2HeaderFont (253,237,244) at 0.426, YES at (0.164, 0.516) and NO at (0.611, 0.516)
	const FRect P = G.Page();
	const float W = G.ScreenW(), H = G.ScreenH();
	G.Draw().Rect(0.f, 0.f, W, H, FLinearColor(0.f, 0.f, 0.f, 0.55f));
	const FRect Band(0.f, 0.3f * H, W, 0.38f * H);
	if (!G.Image(TEXT("2K4Menus.NewControls.Display2"), Band, EImgStyle::Stretched, FLinearColor::White))
	{
		G.FallbackBox(Band, EMenuState::Blurry, false);
	}
	const FFontRef F = G.Font(TEXT("UT2HeaderFont"));
	G.TextIn(F, Loc(TEXT("GUI2K4"), TEXT("UT2K4QuitPage"), TEXT("cQuitDesc.Caption"), TEXT("Do you really want to leave?")),
		FRect(P.X, 0.426042f * H, P.W, G.L(32.f)), FArenaDraw::UTColor(FColor(253, 237, 244)), EAlign::Center, true);
	const float BH = 0.05f * H;
	if (G.Button(FGUI::Id(TEXT("QuitYes")), FRect(P.X + 0.164063f * P.W, 0.515625f * H, 0.2f * P.W, BH), TEXT("RoundButton"),
		Loc(TEXT("GUI2K4"), TEXT("UT2K4QuitPage"), TEXT("cYesButton.Caption"), TEXT("YES")), FString()))
	{
		if (AHUD* HUD = Owner.Get())
		{
			UKismetSystemLibrary::QuitGame(HUD, HUD->GetOwningPlayerController(), EQuitPreference::Quit, false);
		}
	}
	const uint32 NoId = FGUI::Id(TEXT("QuitNo"));
	if (G.GetFocus() == 0)
	{
		G.SetFocus(NoId);
	}
	if (G.Button(NoId, FRect(P.X + 0.610937f * P.W, 0.515625f * H, 0.2f * P.W, BH), TEXT("RoundButton"),
		Loc(TEXT("GUI2K4"), TEXT("UT2K4QuitPage"), TEXT("cNoButton.Caption"), TEXT("NO")), FString()) || G.BackPressed())
	{
		// MainReopened: the menu animates in again without the initial delay
		bQuitPage = false;
		PendingAction = NAME_None;
		PageOpenedAt = G.Time() - 0.5;
		G.SetFocus(0);
	}
}

// =================================================================================================
// Instant Action (UT2K4GamePageSP)
// =================================================================================================

void FArenaMenuScreens::DrawInstantActionPage()
{
	static const TCHAR* TabFallback[6] = { TEXT("Game Type"), TEXT("Map"), TEXT("Rules"), TEXT("Mutators"), TEXT("Bots"), TEXT("Server") };
	static const TCHAR* HintFallback[6] = { TEXT("Choose what kind of match to play"), TEXT("Pick the map and see its preview"),
		TEXT("Score and time limits of the match"), TEXT("Choose mutators that change the rules"), TEXT("Number and skill of the bots"),
		TEXT("Name, players, password and network of the server") };
	const bool bHost = Page == EPage::HostGame;
	const TCHAR* PageClass = bHost ? TEXT("UT2K4GamePageMP") : TEXT("UT2K4GamePageBase");
	// UT2K4GamePageMP inserts Server Rules (PanelCaption[5]) as tab 3
	TArray<int32> Order = { 0, 1, 2, 3, 4 };
	if (bHost)
	{
		Order.Insert(5, 3);
	}
	TArray<FString> Tabs, Hints;
	for (const int32 i : Order)
	{
		Tabs.Add(LocArray(TEXT("GUI2K4"), PageClass, TEXT("PanelCaption"), i, TabFallback[i]));
		Hints.Add(LocArray(TEXT("GUI2K4"), PageClass, TEXT("PanelHint"), i, HintFallback[i]));
	}
	int32& Tab = bHost ? HostTab : IATab;
	double& TabChangedAt = bHost ? HostTabChangedAt : IATabChangedAt;
	Tab = FMath::Clamp(Tab, 0, Tabs.Num() - 1);
	const FString Caption = bHost ? Loc(TEXT("GUI2K4"), TEXT("UT2K4GamePageMP"), TEXT("PageCaption"), TEXT("Host Game"))
		: Loc(TEXT("GUI2K4"), TEXT("UT2K4GamePageSP"), TEXT("PageCaption"), TEXT("Instant Action"));
	const FTabbedFrame Frame = TabbedPage(G, FGUI::Id(bHost ? TEXT("HostTabs") : TEXT("IATabs")), Caption, Tabs, Hints, Tab, TabChangedAt);
	// the tab panels set IATab when a list is activated (double click: next tab); mirror it on the host page
	const int32 IABefore = IATab;
	switch (Order[Tab])
	{
	case 0: DrawGameTypeTab(Frame.Panel); break;
	case 1: DrawMapTab(Frame.Panel); break;
	case 2: DrawRulesTab(Frame.Panel); break;
	case 3: DrawMutatorTab(Frame.Panel); break;
	case 5: DrawServerRulesTab(Frame.Panel); break;
	default: DrawBotTab(Frame.Panel); break;
	}
	if (bHost && IATab != IABefore)
	{
		HostTab = Order.IndexOfByKey(IATab);
		HostTabChangedAt = G.Time();
		IATab = IABefore;
	}
	if (bHost)
	{
		// UT2K4GamePageMP footer: LISTEN, DEDICATED, BACK
		const TArray<FString> Btns = {
			Loc(TEXT("GUI2K4"), TEXT("UT2K4GamePageMP"), TEXT("MPFooter.PrimaryCaption"), TEXT("LISTEN")),
			Loc(TEXT("GUI2K4"), TEXT("UT2K4GamePageMP"), TEXT("MPFooter.SecondaryCaption"), TEXT("DEDICATED")),
			Loc(TEXT("GUI2K4"), TEXT("UT2K4GameFooter"), TEXT("GameBackButton.Caption"), TEXT("BACK")) };
		const TArray<FString> BtnHints = {
			Loc(TEXT("GUI2K4"), TEXT("UT2K4GamePageMP"), TEXT("MPFooter.PrimaryHint"), TEXT("Play on a server that runs in this game")),
			Loc(TEXT("GUI2K4"), TEXT("UT2K4GamePageMP"), TEXT("MPFooter.SecondaryHint"), TEXT("Run the server as a separate program")),
			TEXT("Return to the main menu") };
		const int32 Clicked = FooterButtons(G, Frame.Footer, Btns, BtnHints, { !LayoutId.IsEmpty(), !LayoutId.IsEmpty(), true });
		if (!HostMessage.IsEmpty())
		{
			G.TextIn(G.Font(TEXT("UT2SmallFont")), HostMessage, FRect(G.Page().X + G.L(8.f), Frame.Footer.Y - G.L(22.f), G.Page().W * 0.9f, G.L(20.f)),
				FArenaDraw::UTColor(FColor(255, 210, 0)), EAlign::Left, true);
		}
		if (Clicked == 0)
		{
			StartMatch(EStart::Listen);
		}
		else if (Clicked == 1)
		{
			StartMatch(EStart::Dedicated);
		}
		else if (Clicked == 2 || G.BackPressed())
		{
			SetPage(EPage::Main);
		}
		return;
	}
	// UT2K4GameFooter: PLAY, SPECTATE (not available: disabled like its default MenuState), BACK
	const TArray<FString> Btns = {
		Loc(TEXT("GUI2K4"), TEXT("UT2K4GamePageSP"), TEXT("SPFooter.PrimaryCaption"), TEXT("PLAY")),
		Loc(TEXT("GUI2K4"), TEXT("UT2K4GamePageSP"), TEXT("SPFooter.SecondaryCaption"), TEXT("SPECTATE")),
		Loc(TEXT("GUI2K4"), TEXT("UT2K4GameFooter"), TEXT("GameBackButton.Caption"), TEXT("BACK")) };
	const TArray<FString> BtnHints = { TEXT("Start the match with these settings"), TEXT("Watching a match is not available yet"),
		TEXT("Return to the main menu") };
	const int32 Clicked = FooterButtons(G, Frame.Footer, Btns, BtnHints, { !LayoutId.IsEmpty(), false, true });
	if (Clicked == 0)
	{
		StartMatch();
	}
	else if (Clicked == 2 || G.BackPressed())
	{
		SetPage(EPage::Main);
	}
}

void FArenaMenuScreens::DrawServerRulesTab(const FRect& Panel)
{
	// UT2K4Tab_ServerRulesPanel: the server (bMPOnly) rules in a list background (0.000505, 0.014733, 0.996997 x
	// 0.853123), LAN Server below; our rows are the AGameSession / ArenaGameSession settings
	const TCHAR* Sec = TEXT("UT2K4Tab_ServerRulesPanel");
	UArenaUserSettings* S = UArenaUserSettings::Get();
	const FRect C = G.Section(Panel.Sub(0.02f, 0.02f, 0.96f, 0.83f), LocArray(TEXT("GUI2K4"), TEXT("UT2K4GamePageMP"), TEXT("PanelCaption"), 5, TEXT("Server Rules")));
	const float RowH = G.L(30.f);
	const float W = FMath::Min(C.W * 0.8f, G.L(620.f));
	float Y = C.Y + G.L(16.f);
	auto Row = [&]() { const FRect R(C.X + (C.W - W) * 0.5f, Y, W, RowH - G.L(6.f)); Y += RowH; return R; };
	bool bChanged = false;

	FString Name = S->HostServerName;
	if (G.EditBox(FGUI::Id(TEXT("HostName")), Row(), TEXT("Server Name"), Name, TEXT("The name other players see in the server browser"), 0.4f, 48))
	{
		S->HostServerName = Name.TrimStartAndEnd();
		bChanged = true;
	}
	if (const int32 Step = G.Spinner(FGUI::Id(TEXT("HostMaxPlayers")), Row(), TEXT("Max Players"), FString::FromInt(S->HostMaxPlayers),
		TEXT("How many players can join (bots not counted)"), 0.4f))
	{
		S->HostMaxPlayers = FMath::Clamp(S->HostMaxPlayers + Step, 1, 32);
		bChanged = true;
	}
	if (const int32 Step = G.Spinner(FGUI::Id(TEXT("HostMaxSpectators")), Row(), TEXT("Max Spectators"), FString::FromInt(S->HostMaxSpectators),
		TEXT("How many spectators can join"), 0.4f))
	{
		S->HostMaxSpectators = FMath::Clamp(S->HostMaxSpectators + Step, 0, 32);
		bChanged = true;
	}
	FString Password = S->HostPassword;
	if (G.EditBox(FGUI::Id(TEXT("HostPassword")), Row(), TEXT("Game Password"), Password, TEXT("Players need this password to join; empty for an open server"), 0.4f, 24))
	{
		S->HostPassword = Password.TrimStartAndEnd();
		bChanged = true;
	}
	if (const int32 Step = G.Spinner(FGUI::Id(TEXT("HostPort")), Row(), TEXT("Port"), FString::FromInt(S->HostPort),
		TEXT("UDP game port; server queries use the next port"), 0.4f))
	{
		S->HostPort = FMath::Clamp(S->HostPort + Step, 1024, 65000);
		bChanged = true;
	}
	bool bLAN = S->bHostLANServer;
	if (G.CheckBox(FGUI::Id(TEXT("HostLAN")), Row(), Loc(TEXT("GUI2K4"), Sec, TEXT("LANServer.Caption"), TEXT("LAN Server")), bLAN,
		TEXT("Lifts the bandwidth limit per player; for LAN games only"), 0.8f))
	{
		S->bHostLANServer = bLAN;
		bChanged = true;
	}
	bool bAdvertise = S->bHostAdvertise;
	if (G.CheckBox(FGUI::Id(TEXT("HostAdvertise")), Row(), TEXT("Show in Server Browsers"), bAdvertise,
		TEXT("Answer LAN and server queries so the server appears in the Join Game LAN tab"), 0.8f))
	{
		S->bHostAdvertise = bAdvertise;
		bChanged = true;
	}
	if (bChanged)
	{
		S->Save();
	}
	// what the players need to join
	const FString Note = FString::Printf(TEXT("Players join with %s: Join Game > LAN, or Open IP <your address>:%d. Only %s players can join, not UT2004."),
		ArenaBranding::GameTitle(), S->HostPort, ArenaBranding::GameTitle());
	G.ScrollText(FGUI::Id(TEXT("HostNote")), FRect(C.X + G.L(16.f), Y + G.L(8.f), C.W - G.L(32.f), C.B() - Y - G.L(12.f)), Note);
}

void FArenaMenuScreens::DrawGameTypeTab(const FRect& Panel)
{
	// UT2K4Tab_GameTypeBase: GameTypeLeftGroup (0.02375, 0.043125, 0.4825 x 0.94149) with the UT2004Games list
	// (0.045599, 0.144225, 0.438457 x 0.796982, FontScale large), GameTypeRightGroup "Preview" (0.513243, 0.043125,
	// 0.464649 x 0.94149) with GameTypePreview (0.539224, 0.142826, 0.411862 x 0.316545) and Bk1 (AltSectionBackground,
	// 0.535622, 0.478553, 0.41903 x 0.474455) around the description (0.56527, 0.556774, 0.362056 x 0.325716)
	const TArray<FGameTypeInfo>& Types = GetGameTypes();
	const FRect GTC = G.Section(Panel.Sub(0.02375f, 0.043125f, 0.4825f, 0.94149f), Loc(TEXT("GUI2K4"), TEXT("UT2K4Tab_GameTypeBase"), TEXT("GameTypeLeftGroup.Caption"), TEXT("Game Types")));
	G.Section(Panel.Sub(0.513243f, 0.043125f, 0.464649f, 0.94149f), Loc(TEXT("GUI2K4"), TEXT("UT2K4Tab_GameTypeBase"), TEXT("GameTypeRightGroup.Caption"), TEXT("Preview")));
	const FRect GTDesc = G.Section(Panel.Sub(0.535622f, 0.478553f, 0.41903f, 0.474455f), FString(), true);

	TArray<FListItem> Items;
	Items.Add({ Loc(TEXT("GUI2K4"), TEXT("UT2K4Tab_GameTypeBase"), TEXT("EpicGameCaption"), TEXT("Game Types")), FString(), true });
	int32 Sel = 0;
	const FString Current = GetModeKey();
	for (int32 i = 0; i < Types.Num(); ++i)
	{
		Items.Add({ Types[i].Name });
		if (Types[i].ModeId.Equals(Current, ESearchCase::IgnoreCase))
		{
			Sel = i + 1;
		}
	}
	bool bActivated = false;
	if (G.List(FGUI::Id(TEXT("GameTypes")), GTC.Inset(0.f, GTC.H * 0.025f, 0.f, GTC.H * 0.025f) , Items, Sel, bActivated,
		TEXT("Choose the game type to play"), TEXT("UT2HeaderFont"), true) && Types.IsValidIndex(Sel - 1))
	{
		SetMode(FindModeIndex(Types[Sel - 1].ModeId));
	}
	if (bActivated)
	{
		IATab = 1;
		IATabChangedAt = G.Time();
	}
	const FGameTypeInfo* Info = FindGameType(Current);
	const FRect Shot = Panel.Sub(0.539224f, 0.142826f, 0.411862f, 0.316545f);
	bool bShot = false;
	if (Info && !Info->Screenshot.IsEmpty())
	{
		const FAnimatedImage& Img = FAnimatedImage::Get(Info->Screenshot);
		FString A, B;
		float Blend = 0.f;
		Img.SamplePlaying(G.Time(), A, B, Blend);
		bShot = !A.IsEmpty() && G.ImageNormal(A, B, Blend, Shot);
	}
	if (!bShot)
	{
		// NoPreview: UT2HeaderFont, (247,255,0)
		G.TextIn(G.Font(TEXT("UT2HeaderFont")), Loc(TEXT("GUI2K4"), TEXT("UT2K4Tab_GameTypeBase"), TEXT("NoPreview.Caption"), TEXT("No preview")),
			Shot, FArenaDraw::UTColor(FColor(247, 255, 0)), EAlign::Center);
	}
	const FString Desc = Info && !Info->Description.IsEmpty() ? Info->Description : FString(TEXT(""));
	G.ScrollText(FGUI::Id(TEXT("GameDesc")), GTDesc.Inset(G.L(8.f), G.L(4.f), G.L(8.f), G.L(4.f)), Desc);
}

void FArenaMenuScreens::DrawMapTab(const FRect& Panel)
{
	// UT2K4Tab_MainSP: SelectionGroup (0.016993, 0.018125, 0.482149 x 0.60333) with AvailableMaps (0.045671, 0.169272,
	// 0.422481 x 0.44987), OptionsGroup (0.018008, 0.64258, 0.482149 x 0.351772) with FilterCheck, PreviewGroup
	// (0.515743, 0.018125, 0.470899 x 0.974305): MapPreviewImage (0.562668, 0.107691, 0.372002 x 0.35748), MapAuthorLabel
	// (0.405278), RecommendedPlayers (0.474166), ScrollSection "Map Desc" (AltSectionBackground 0.546118, 0.525219,
	// 0.409888 x 0.437814) with MapDescription (0.561065, 0.628421, 0.379993 x 0.26841)
	const TCHAR* Sec = TEXT("UT2K4Tab_MainSP");
	const FRect MapC = G.Section(Panel.Sub(0.016993f, 0.018125f, 0.482149f, 0.60333f), Loc(TEXT("GUI2K4"), Sec, TEXT("SelectionGroup.Caption"), TEXT("Maps")));
	const FRect Opt = G.Section(Panel.Sub(0.018008f, 0.64258f, 0.482149f, 0.351772f), Loc(TEXT("GUI2K4"), Sec, TEXT("OptionsGroup.Caption"), TEXT("Options")));
	G.Section(Panel.Sub(0.515743f, 0.018125f, 0.470899f, 0.974305f), Loc(TEXT("GUI2K4"), Sec, TEXT("PreviewGroup.Caption"), TEXT("Preview")));
	const FRect MapDescC = G.Section(Panel.Sub(0.546118f, 0.525219f, 0.409888f, 0.437814f), Loc(TEXT("GUI2K4"), Sec, TEXT("ScrollSection.Caption"), TEXT("Description")), true);

	const TArray<FArenaLayoutInfo> Layouts = AArenaLevelBuilder::GetLayouts();
	const TArray<int32> Valid = GetValidLayoutIndices();
	TArray<FListItem> Items;
	int32 Sel = -1;
	for (int32 i = 0; i < Valid.Num(); ++i)
	{
		const FArenaLayoutInfo& L = Layouts[Valid[i]];
		FListItem It;
		It.Text = L.MapName.IsEmpty() ? L.Id : L.MapName;
		if (L.bCustom)
		{
			It.Right = L.Notice.IsEmpty() ? TEXT("custom") : TEXT("custom*");
		}
		Items.Add(It);
		if (L.Id == LayoutId)
		{
			Sel = i;
		}
	}
	bool bActivated = false;
	if (G.List(FGUI::Id(TEXT("Maps")), MapC, Items, Sel, bActivated,
		Loc(TEXT("GUI2K4"), Sec, TEXT("AvailableMaps.Hint"), TEXT("Choose a map; double click to play it"))) && Valid.IsValidIndex(Sel))
	{
		LayoutId = Layouts[Valid[Sel]].Id;
	}
	if (bActivated && !LayoutId.IsEmpty())
	{
		StartMatch();
		return;
	}
	if (G.CheckBox(FGUI::Id(TEXT("OfficialOnly")), FRect(Opt.X + Opt.W * 0.05f, Opt.Y + G.L(8.f), Opt.W * 0.9f, FMath::RoundToFloat(G.ScreenH() * 0.03f)),
		Loc(TEXT("GUI2K4"), Sec, TEXT("FilterCheck.Caption"), TEXT("Only UT2004 Maps")), bOfficialMapsOnly, TEXT("Hide the custom maps of your UserContent folder"), 0.8f))
	{
		ValidateLayout();
	}

	FArenaLayoutInfo L;
	const FRect Shot = Panel.Sub(0.562668f, 0.107691f, 0.372002f, 0.35748f);
	if (!AArenaLevelBuilder::FindLayout(LayoutId, L))
	{
		G.TextIn(G.Font(TEXT("UT2HeaderFont")), TEXT("No map for this game type"), Shot, FArenaDraw::UTColor(FColor(247, 255, 0)), EAlign::Center);
		return;
	}
	bool bShot = false;
	const FString Material = !L.Screenshot.IsEmpty() ? L.Screenshot : GetMapScreenshot(L.MapName);
	if (!Material.IsEmpty())
	{
		const FAnimatedImage& Img = FAnimatedImage::Get(Material);
		FString A, B;
		float Blend = 0.f;
		Img.SamplePlaying(G.Time(), A, B, Blend);
		if (A.IsEmpty() && !L.Screenshot.IsEmpty())
		{
			A = L.Screenshot;
		}
		bShot = !A.IsEmpty() && G.ImageNormal(A, B, Blend, Shot);
	}
	if (!bShot)
	{
		G.TextIn(G.Font(TEXT("UT2HeaderFont")), Loc(TEXT("GUI2K4"), Sec, TEXT("NoPreview.Caption"), TEXT("No preview")), Shot,
			FArenaDraw::UTColor(FColor(247, 255, 0)), EAlign::Center);
	}
	const FString Author = L.Author.IsEmpty() ? FString() : Loc(TEXT("GUI2K4"), TEXT("UT2K4Tab_MainBase"), TEXT("AuthorText"), TEXT("Author")) + TEXT(": ") + L.Author;
	// MapAuthorLabel (0.405278) and RecommendedPlayers (0.474166) overlap the preview at our image proportions: placed under it
	G.Label(Panel.Sub(0.522265f, 0.468f, 0.453285f, 0.032552f), Author, TEXT("TextLabel"), EAlign::Center);
	G.Label(Panel.Sub(0.521288f, 0.4935f, 0.445313f, 0.032552f),
		FString::Printf(TEXT("%d %s"), L.RecommendedPlayers, *Loc(TEXT("GUI2K4"), TEXT("UT2K4Tab_MainBase"), TEXT("PlayerText"), TEXT("players"))),
		TEXT("TextLabel"), EAlign::Center);
	FString Desc = L.Description.IsEmpty() ? Loc(TEXT("GUI2K4"), TEXT("UT2K4Tab_MainBase"), TEXT("MessageNoInfo"), TEXT("No description.")) : L.Description;
	if (L.bCustom && !L.Notice.IsEmpty())
	{
		Desc += TEXT("\n\n* ") + L.Notice;
	}
	G.ScrollText(FGUI::Id(TEXT("MapDesc")), MapDescC.Inset(G.L(8.f), G.L(4.f), G.L(8.f), G.L(4.f)), Desc);
}

void FArenaMenuScreens::DrawRulesTab(const FRect& Panel)
{
	// UT2K4Tab_RulesBase: the rules of the game type (PlayInfo groups in UT2004; our match options here)
	const FRect C = G.Section(Panel.Sub(0.02f, 0.02f, 0.96f, 0.96f), Loc(TEXT("GUI2K4"), TEXT("UT2K4PlayerLoginMenu"), TEXT("SPRulesPanel.Caption"), TEXT("Rules")));
	const UArenaGameRules* Defaults = GetModeDefaults();
	const float RowH = FMath::RoundToFloat(G.ScreenH() * 0.03f), Step = FMath::RoundToFloat(G.ScreenH() * 0.045f);
	float Y = C.Y + G.L(8.f);
	const float X = C.X + C.W * 0.05f, W = C.W * 0.45f;
	auto Row = [&]() { const FRect R(X, Y, W, RowH); Y += Step; return R; };

	int32 Min = 0, Max = 100, GStep = 5;
	if (Defaults)
	{
		Defaults->GetGoalRange(Min, Max, GStep);
	}
	GStep = FMath::Max(1, GStep);
	const FString GoalLabel = Defaults ? Defaults->GetGoalLabel() : FString(TEXT("Frag Limit"));
	int32 Dir = G.Spinner(FGUI::Id(TEXT("RuleGoal")), Row(), GoalLabel, GoalScore == 0 ? FString(TEXT("None")) : FString::FromInt(GoalScore),
		TEXT("Score that wins the match (none = play until the time limit)"), 0.6f);
	if (Dir != 0)
	{
		GoalScore = FMath::Clamp((GoalScore / GStep) * GStep + Dir * GStep, Min, Max);
	}
	Dir = G.Spinner(FGUI::Id(TEXT("RuleTime")), Row(), TEXT("Time Limit"), TimeLimit == 0 ? FString(TEXT("None")) : FString::Printf(TEXT("%d min"), TimeLimit),
		TEXT("Length of the match in minutes (none = no limit)"), 0.6f);
	if (Dir != 0)
	{
		TimeLimit = FMath::Clamp((TimeLimit / 5) * 5 + Dir * 5, 0, 60);
	}
	G.CheckBox(FGUI::Id(TEXT("RuleInstagib")), Row(), TEXT("Instagib"), bInstagib, TEXT("Everyone has a one-shot-kill rifle and nothing else"), 0.8f);
	{
		// DeathMatch PlayInfo "bAllowTrans" (Rules group): kept per game mode like UT2004's per game class setting
		bool bTrans = GetTranslocatorSetting();
		const bool bWas = bTrans;
		G.CheckBox(FGUI::Id(TEXT("RuleTranslocator")), Row(), TEXT("Translocator"), bTrans, TEXT("Players start with a translocator"), 0.8f);
		if (bTrans != bWas)
		{
			SetTranslocatorSetting(bTrans);
		}
	}
	{
		// GameInfo PlayInfo "bWeaponShouldViewShake" (Rules group, "Weapons Shake View"): UArenaUserSettings::bWeaponViewShake
		bool bShake = UArenaUserSettings::Get()->bWeaponViewShake;
		const bool bWasShake = bShake;
		G.CheckBox(FGUI::Id(TEXT("RuleWeaponShake")), Row(), TEXT("Weapons Shake View"), bShake, TEXT("Firing a weapon shakes your view"), 0.8f);
		if (bShake != bWasShake)
		{
			UArenaUserSettings::Get()->bWeaponViewShake = bShake;
			UArenaUserSettings::Get()->Save();
		}
	}
	if (Defaults && Defaults->IsTeamGame())
	{
		TArray<FString> Teams = { TEXT("Red"), TEXT("Blue") };
		G.Combo(FGUI::Id(TEXT("RuleTeam")), Row(), TEXT("Your Team"), Team, Teams, TEXT("The team you play on"), 0.6f);
	}
}

void FArenaMenuScreens::DrawMutatorTab(const FRect& Panel)
{
	// (lists and text boxes are placed in the client area of the section that manages them, GUISectionBackground.ManageComponent)
	// UT2K4Tab_MutatorBase: AvailBackground (0.025156, 0.036614, 0.380859 x 0.547697) with IAMutatorAvailList (0.026108,
	// 0.144937, 0.378955 x 0.501446), ActiveBackground (0.586876, ...) with IAMutatorSelectedList (0.584376, ...), the
	// Add / Add All / Remove / Remove All buttons at X 0.425 (0.145 x 0.05; Y 0.194114, 0.259218, 0.360259, 0.424322) and
	// DescriptionBackground (0.025976, 0.610678, 0.942969 x 0.291796) with IAMutatorScroll (0.028333, 0.648595, 0.938254 x 0.244296)
	const TCHAR* Sec = TEXT("UT2K4Tab_MutatorBase");
	const FRect AvailC = G.Section(Panel.Sub(0.025156f, 0.036614f, 0.380859f, 0.547697f), Loc(TEXT("GUI2K4"), Sec, TEXT("AvailBackground.Caption"), TEXT("Available")));
	const FRect ActiveC = G.Section(Panel.Sub(0.586876f, 0.036614f, 0.380859f, 0.547697f), Loc(TEXT("GUI2K4"), Sec, TEXT("ActiveBackground.Caption"), TEXT("Active")));
	const FRect DescC = G.Section(Panel.Sub(0.025976f, 0.610678f, 0.942969f, 0.291796f), Loc(TEXT("GUI2K4"), Sec, TEXT("DescriptionBackground.Caption"), TEXT("Details")));

	const TArray<FMutatorInfo>& All = GetMutators();
	auto Find = [&All](const FString& Class) { return All.FindByPredicate([&Class](const FMutatorInfo& M) { return M.ClassName.Equals(Class, ESearchCase::IgnoreCase); }); };
	Mutators.RemoveAll([&Find](const FString& C) { return Find(C) == nullptr; });
	TArray<const FMutatorInfo*> Avail, Active;
	for (const FMutatorInfo& M : All)
	{
		(Mutators.Contains(M.ClassName) ? Active : Avail).Add(&M);
	}
	TArray<FListItem> AvailItems, ActiveItems;
	for (const FMutatorInfo* M : Avail) AvailItems.Add({ M->FriendlyName, FString(), false, !M->bCanRun });
	for (const FMutatorInfo* M : Active) ActiveItems.Add({ M->FriendlyName });

	bool bAct = false;
	auto AddSel = [&]()
	{
		if (!Avail.IsValidIndex(AvailSel))
		{
			return;
		}
		const FMutatorInfo* M = Avail[AvailSel];
		if (!M->bCanRun)
		{
			MutatorMessage = M->Reason.IsEmpty() ? FString(TEXT("This mutator cannot run in this game yet.")) : M->Reason;
			return;
		}
		// GroupConflictText: one mutator per group
		if (!M->GroupName.IsEmpty() && Active.ContainsByPredicate([M](const FMutatorInfo* A) { return A->GroupName.Equals(M->GroupName, ESearchCase::IgnoreCase); }))
		{
			MutatorMessage = Loc(TEXT("GUI2K4"), TEXT("UT2K4Tab_MutatorBase"), TEXT("GroupConflictText"), TEXT("Only one mutator of a group can be active."));
			return;
		}
		Mutators.Add(M->ClassName);
		MutatorMessage.Reset();
	};
	if (G.List(FGUI::Id(TEXT("MutAvail")), AvailC, AvailItems, AvailSel, bAct,
		Loc(TEXT("GUI2K4"), Sec, TEXT("IAMutatorAvailList.Hint"), TEXT("The mutators you can add"))))
	{
		bDescActive = false;
		MutatorMessage.Reset();
	}
	if (bAct) AddSel();
	if (G.List(FGUI::Id(TEXT("MutActive")), ActiveC, ActiveItems, ActiveSel, bAct,
		Loc(TEXT("GUI2K4"), Sec, TEXT("IAMutatorSelectedList.Hint"), TEXT("The mutators of the match"))))
	{
		bDescActive = true;
	}
	if (bAct && Active.IsValidIndex(ActiveSel))
	{
		Mutators.Remove(Active[ActiveSel]->ClassName);
	}
	const float BX = 0.425f, BW = 0.145f, BH = 0.05f;
	if (G.Button(FGUI::Id(TEXT("MutAdd")), Panel.Sub(BX, 0.194114f, BW, BH), TEXT("SquareButton"), Loc(TEXT("GUI2K4"), Sec, TEXT("IAMutatorAdd.Caption"), TEXT("Add")),
		Loc(TEXT("GUI2K4"), Sec, TEXT("IAMutatorAdd.Hint"), TEXT("Add the selected mutator")), Avail.IsValidIndex(AvailSel), EFontScale::Medium, ESound::Up))
	{
		AddSel();
	}
	if (G.Button(FGUI::Id(TEXT("MutAll")), Panel.Sub(BX, 0.259218f, BW, BH), TEXT("SquareButton"), Loc(TEXT("GUI2K4"), Sec, TEXT("IAMutatorAll.Caption"), TEXT("Add All")),
		Loc(TEXT("GUI2K4"), Sec, TEXT("IAMutatorAll.Hint"), TEXT("Add every mutator")), Avail.Num() > 0, EFontScale::Medium, ESound::Up))
	{
		for (const FMutatorInfo* M : Avail)
		{
			const bool bConflict = !M->GroupName.IsEmpty() && Mutators.ContainsByPredicate([&](const FString& C)
			{
				const FMutatorInfo* A = Find(C);
				return A && A->GroupName.Equals(M->GroupName, ESearchCase::IgnoreCase);
			});
			if (M->bCanRun && !bConflict)
			{
				Mutators.Add(M->ClassName);
			}
		}
	}
	if (G.Button(FGUI::Id(TEXT("MutRemove")), Panel.Sub(BX, 0.360259f, BW, BH), TEXT("SquareButton"), Loc(TEXT("GUI2K4"), Sec, TEXT("IAMutatorRemove.Caption"), TEXT("Remove")),
		Loc(TEXT("GUI2K4"), Sec, TEXT("IAMutatorRemove.Hint"), TEXT("Remove the selected mutator")), Active.IsValidIndex(ActiveSel), EFontScale::Medium, ESound::Down))
	{
		Mutators.Remove(Active[ActiveSel]->ClassName);
	}
	if (G.Button(FGUI::Id(TEXT("MutClear")), Panel.Sub(BX, 0.424322f, BW, BH), TEXT("SquareButton"), Loc(TEXT("GUI2K4"), Sec, TEXT("IAMutatorClear.Caption"), TEXT("Remove All")),
		Loc(TEXT("GUI2K4"), Sec, TEXT("IAMutatorClear.Hint"), TEXT("Remove every mutator")), Active.Num() > 0, EFontScale::Medium, ESound::Down))
	{
		Mutators.Reset();
	}
	// description of the selected mutator
	const FMutatorInfo* Shown = bDescActive ? (Active.IsValidIndex(ActiveSel) ? Active[ActiveSel] : nullptr) : (Avail.IsValidIndex(AvailSel) ? Avail[AvailSel] : nullptr);
	FString Desc;
	if (All.Num() == 0)
	{
		Desc = TEXT("No mutators were found in your UT2004 install.");
	}
	else if (Shown)
	{
		Desc = Shown->Description;
		if (!Shown->bCanRun)
		{
			Desc += TEXT("\n\nNot available: ") + (Shown->Reason.IsEmpty() ? FString(TEXT("this mutator cannot run in this game yet.")) : Shown->Reason);
		}
	}
	if (!MutatorMessage.IsEmpty())
	{
		Desc = MutatorMessage + TEXT("\n\n") + Desc;
	}
	G.ScrollText(FGUI::Id(TEXT("MutDesc")), DescC, Desc);
}

void FArenaMenuScreens::DrawBotTab(const FRect& Panel)
{
	// UT2K4Tab_BotConfigBase lays out red / blue rosters with character portraits; our bots are configured by number and skill
	const FRect C = G.Section(Panel.Sub(0.011758f, 0.008334f, 0.358731f, 0.576876f),
		TEXT("Bots"));
	const float RowH = FMath::RoundToFloat(G.ScreenH() * 0.03f), Step = FMath::RoundToFloat(G.ScreenH() * 0.045f);
	float Y = C.Y + G.L(8.f);
	const float X = C.X + C.W * 0.05f, W = C.W * 0.9f;
	int32 Dir = G.Spinner(FGUI::Id(TEXT("BotNum")), FRect(X, Y, W, RowH), TEXT("Number of Bots"), NumBots == 0 ? FString(TEXT("None")) : FString::FromInt(NumBots),
		TEXT("How many bots join the match"), 0.6f);
	if (Dir != 0)
	{
		NumBots = FMath::Clamp(NumBots + Dir, 0, 15);
	}
	Y += Step;
	TArray<FString> Skills;
	for (const TCHAR* S : SkillNames)
	{
		Skills.Add(S);
	}
	G.Combo(FGUI::Id(TEXT("BotSkill")), FRect(X, Y, W, RowH), TEXT("Bot Skill"), BotSkill, Skills, TEXT("How well the bots play"), 0.45f);
	// right side: the description of the match line-up
	const FRect Info = G.Section(Panel.Sub(0.392777f, 0.03782f, 0.595f, 0.512104f), FString(), true);
	const UArenaGameRules* Defaults = GetModeDefaults();
	FString Text = FString::Printf(TEXT("%d %s at %s skill."), NumBots, NumBots == 1 ? TEXT("bot") : TEXT("bots"), SkillNames[FMath::Clamp(BotSkill, 0, 7)]);
	if (Defaults && Defaults->IsTeamGame())
	{
		Text += FString::Printf(TEXT(" The bots fill both teams; you play on the %s team."), Team == 0 ? TEXT("red") : TEXT("blue"));
	}
	G.ScrollText(FGUI::Id(TEXT("BotInfo")), Info.Inset(G.L(10.f), G.L(6.f), G.L(10.f), 0.f), Text);
}

