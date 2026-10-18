#include "ArenaSinglePlayer.h"
#include "ArenaSinglePlayerPrivate.h"
#include "ArenaGUI2K4Pages.h"
#include "ArenaHUD.h"
#include "ArenaLevelBuilder.h"
#include "ArenaUserSettings.h"

using namespace ArenaGUI2K4;
using namespace ArenaSP;
using namespace ArenaSPUI;

namespace ArenaSPTabsLocal
{
	/** UT2K4LadderButton default size (relative to the tab panel). */
	constexpr float LadderBtnW = 0.083925f;
	constexpr float LadderBtnH = 0.113688f;
	/** UT2K4SPTab_LadderBase.AnimTime */
	constexpr float AnimTime = 1.f;

	FLinearColor C(uint8 R, uint8 G, uint8 B, uint8 A = 255)
	{
		return FArenaDraw::UTColor(FColor(R, G, B, A));
	}

	FRect Lerp(const FRect& A, const FRect& B, float T)
	{
		return FRect(FMath::Lerp(A.X, B.X, T), FMath::Lerp(A.Y, B.Y, T), B.W, B.H);
	}

	/** Button animation of UT2K4SPTab_LadderBase.DoAnimate: button i moves from the first one's spot in AnimTime * i / n. */
	float AnimAlpha(double Since, int32 i, int32 n)
	{
		if (i == 0 || n <= 0)
		{
			return 1.f;
		}
		const float Dur = AnimTime * float(i) / float(n);
		return FMath::Clamp(float(Since) / FMath::Max(0.01f, Dur), 0.f, 1.f);
	}
}

using namespace ArenaSPTabsLocal;

// =================================================================================================
// Profile (UT2K4SPTab_Profile)
// =================================================================================================

void FArenaSinglePlayerPage::DrawProfileTab(const FRect& P)
{
	const TCHAR* Sec = TEXT("UT2K4SPTab_Profile");
	// SPPsbProfiles (0.0232, 0.0465, 0.2708 x 0.803) with SPPlbProfiles (0.0418, 0.1259, 0.2368 x 0.6898)
	G->Section(P.Sub(0.023243f, 0.046522f, 0.270899f, 0.803061f), Caption(Sec, TEXT("SPPsbProfiles.Caption"), TEXT("Profiles")));
	TArray<FListItem> Items;
	for (const FString& N : ProfileNames)
	{
		FListItem It;
		It.Text = N;
		Items.Add(It);
	}
	bool bAct = false;
	const int32 Before = ProfileSel;
	G->List(FGUI::Id(TEXT("SPProfiles")), P.Sub(0.041869f, 0.125936f, 0.236809f, 0.689862f), Items, ProfileSel, bAct,
		Caption(Sec, TEXT("SPPlbProfiles.Hint"), TEXT("Select a profile")));
	(void)Before;
	if (bAct && ProfileNames.IsValidIndex(ProfileSel))
	{
		if (GP && !GP->ProfileName.Equals(ProfileNames[ProfileSel], ESearchCase::IgnoreCase))
		{
			PushQuestion(Caption(Sec, TEXT("OverloadProfile"), TEXT("You already have a profile loaded.||Are you sure you want to load this profile?")),
				EQuestion::LoadOverProfile, true);
		}
		else if (!GP)
		{
			OnQuestionAnswer(EQuestion::LoadOverProfile, true);
		}
	}

	// the selected profile (loaded for its details, like UpdateProfileDesc)
	static TSharedPtr<FProfile> Shown;
	const FString SelName = ProfileNames.IsValidIndex(ProfileSel) ? ProfileNames[ProfileSel] : FString();
	if (GP && GP->ProfileName.Equals(SelName, ESearchCase::IgnoreCase))
	{
		Shown = GP;
	}
	else if (!SelName.IsEmpty() && (!Shown || !Shown->ProfileName.Equals(SelName, ESearchCase::IgnoreCase)))
	{
		Shown = FProfile::Load(SelName);
	}
	else if (SelName.IsEmpty())
	{
		Shown.Reset();
	}
	const bool bLoaded = Shown && GP && Shown->ProfileName.Equals(GP->ProfileName, ESearchCase::IgnoreCase);

	// portrait: SPPimgFaceBorder (0.31, 0.05, 0.28 x 0.8), SPPimgFace (0.324, 0.1069, 0.2519 x 0.686), the blur when not loaded
	G->Section(P.Sub(0.31f, 0.05f, 0.28f, 0.8f), FString(), true);
	const FRect Face = P.Sub(0.324024f, 0.106934f, 0.251954f, 0.686092f);
	if (Shown)
	{
		G->Image(PortraitOf(Shown->PlayerCharacter), Face, EImgStyle::Scaled, FLinearColor(1.f, 1.f, 1.f, bLoaded ? 1.f : 196.f / 255.f));
	}
	if (!bLoaded)
	{
		G->Image(TEXT("InterfaceContent.Menu.pEmptySlot"), Face, EImgStyle::Scaled, FLinearColor::White);
	}
	// team symbol: SPPimgSponsorBorder (0.61, 0.05, 0.37 x 0.2929), SPPimgSponsor (0.6252, 0.1084, 0.3382 x 0.2211)
	G->Section(P.Sub(0.61f, 0.05f, 0.37f, 0.292969f), FString());
	if (Shown && !Shown->TeamSymbolName.IsEmpty())
	{
		G->Image(SymbolTexture(Shown->TeamSymbolName), P.Sub(0.625235f, 0.108424f, 0.338234f, 0.221136f), EImgStyle::Justified, FLinearColor::White);
	}
	// details: SPPsbDetailsBg (0.61, 0.3689, 0.37 x 0.4846), SPPsbDetails (0.6341, 0.4391, 0.3254 x 0.3886)
	G->Section(P.Sub(0.61f, 0.368949f, 0.37f, 0.484686f), Caption(Sec, TEXT("SPPsbDetailsBg.Caption"), TEXT("Details")));
	FString Details = Caption(Sec, TEXT("NoProfileSelected"), TEXT("No profile selected..."));
	if (Shown)
	{
		const FProfile& S = *Shown;
		TArray<FString> Members;
		for (const FString& M : S.PlayerTeam)
		{
			if (!M.IsEmpty())
			{
				Members.Add(M);
			}
		}
		FString Sprees, Multi, Awards;
		for (int32 i = 0; i < 6; ++i)
		{
			if (S.Spree[i] > 0)
			{
				Sprees += TEXT("\n") + ArenaSP::Loc(TEXT("GUI2K4"), Sec, *FString::Printf(TEXT("SpreeLabel[%d]"), i), FString()) + TEXT(" ") + FString::FromInt(S.Spree[i]);
			}
		}
		for (int32 i = 0; i < 7; ++i)
		{
			if (S.MultiKills[i] > 0)
			{
				Multi += TEXT("\n") + ArenaSP::Loc(TEXT("GUI2K4"), Sec, *FString::Printf(TEXT("MultiKillsLabel[%d]"), i), FString()) + TEXT(" ") + FString::FromInt(S.MultiKills[i]);
			}
		}
		const TArray<FString>& AwardNames = Get().Defaults.SpecialAwardNames;
		for (int32 i = 0; i < 6; ++i)
		{
			if (S.SpecialAwards[i] > 0 && AwardNames.IsValidIndex(i))
			{
				Awards += TEXT("\n") + AwardNames[i] + TEXT(": ") + FString::FromInt(S.SpecialAwards[i]);
			}
		}
		Details = Caption(Sec, TEXT("ProfileDetails"), TEXT("Team: %teamname%|Leader: %playername%|Members: %teammembers%||Total game time: %totaltime% minutes|Balance: %credits%||Matches: %matches%|Wins: %wins%|Kills: %kills%|Deaths: %deaths%|Goals: %Goals%|%sprees%|%multikills%|%specialawards%"))
			.Replace(TEXT("%teamname%"), *S.TeamName)
			.Replace(TEXT("%playername%"), *S.PlayerName)
			.Replace(TEXT("%teammembers%"), *FString::Join(Members, TEXT(", ")))
			.Replace(TEXT("%totaltime%"), *FString::FromInt(FMath::RoundToInt(S.TotalTime / 60.f)))
			.Replace(TEXT("%credits%"), *FProfile::MoneyToString(S.Balance))
			.Replace(TEXT("%matches%"), *FString::FromInt(S.Matches))
			.Replace(TEXT("%wins%"), *FString::FromInt(S.Wins))
			.Replace(TEXT("%kills%"), *FString::FromInt(S.Kills))
			.Replace(TEXT("%deaths%"), *FString::FromInt(S.Deaths))
			.Replace(TEXT("%Goals%"), *FString::FromInt(S.Goals))
			.Replace(TEXT("%sprees%"), *Sprees)
			.Replace(TEXT("%multikills%"), *Multi)
			.Replace(TEXT("%specialawards%"), *Awards);
	}
	G->ScrollText(FGUI::Id(TEXT("SPProfileDetails")), P.Sub(0.634102f, 0.439111f, 0.325421f, 0.388687f), Details);

	// DELETE (0.18, 0.9), CREATE NEW (0.4, 0.9), HIGH SCORES (0.62, 0.9), 0.2 x 0.05, small font
	if (G->Button(FGUI::Id(TEXT("SPDelete")), P.Sub(0.18f, 0.9f, 0.2f, 0.05f), TEXT("SquareButton"), Caption(Sec, TEXT("SPPbtnDelete.Caption"), TEXT("DELETE")),
		Caption(Sec, TEXT("SPPbtnDelete.Hint"), TEXT("Delete the selected profile")), ProfileNames.IsValidIndex(ProfileSel), EFontScale::Small))
	{
		PushQuestion(Caption(Sec, TEXT("DeleteProfile"), TEXT("You are about to delete the profile for '%profile%'||Are you sure you want to delete this profile?"))
			.Replace(TEXT("%profile%"), *SelName), EQuestion::DeleteProfile, true);
	}
	if (G->Button(FGUI::Id(TEXT("SPCreate")), P.Sub(0.4f, 0.9f, 0.2f, 0.05f), TEXT("SquareButton"), Caption(Sec, TEXT("SPPbtnCreate.Caption"), TEXT("CREATE NEW")),
		Caption(Sec, TEXT("SPPbtnCreate.Hint"), TEXT("Create a new profile")), true, EFontScale::Small))
	{
		bCreatingProfile = true;
		NewName.Reset();
		RebuildTabs();
		SetActiveTab(ETab::NewProfile);
	}
	if (G->Button(FGUI::Id(TEXT("SPHighScores")), P.Sub(0.62f, 0.9f, 0.2f, 0.05f), TEXT("SquareButton"), Caption(Sec, TEXT("SPPbtnHighScores.Caption"), TEXT("HIGH SCORES")),
		Caption(Sec, TEXT("SPPbtnHighScores.Hint"), TEXT("View the high scores")), true, EFontScale::Small))
	{
		FDialog D;
		D.Kind = EDialog::HighScores;
		PushDialog(D);
	}
}

// =================================================================================================
// New profile (UT2K4SPTab_ProfileNew)
// =================================================================================================

void FArenaSinglePlayerPage::DrawNewProfileTab(const FRect& P)
{
	const TCHAR* Sec = TEXT("UT2K4SPTab_ProfileNew");
	const FData& D = Get();
	// defaults (ResetValues): the default character, its name, the default team name, difficulty and symbol
	static bool bInit = false;
	if (NewName.IsEmpty() && !bInit)
	{
		bInit = true;
		// the character chosen in Settings > Player when it is one of the tournament's, else the stock default (Jakob)
		const FString Chosen = UArenaUserSettings::Get()->PlayerCharacter;
		NewCharIndex = SPCharacters.IndexOfByPredicate([&Chosen](const FString& C) { return C.Equals(Chosen, ESearchCase::IgnoreCase); });
		if (NewCharIndex == INDEX_NONE)
		{
			NewCharIndex = FMath::Max(0, SPCharacters.IndexOfByPredicate([&D](const FString& C) { return C.Equals(D.Defaults.DefaultCharacter, ESearchCase::IgnoreCase); }));
		}
		NewName = SPCharacters.IsValidIndex(NewCharIndex) ? SPCharacters[NewCharIndex].Replace(TEXT("."), TEXT("_")) : FString(TEXT("Player"));
		NewTeamName = D.Defaults.DefaultTeamName.IsEmpty() ? FString(TEXT("Team")) : D.Defaults.DefaultTeamName;
		NewDifficulty = D.Defaults.DefaultDifficulty;
		NewSymbolIndex = FMath::Max(0, D.TeamSymbols.IndexOfByPredicate([](const FString& S) { return S.Equals(TEXT("TeamSymbols_UT2004.Design1"), ESearchCase::IgnoreCase); }));
	}
	if (NewName.IsEmpty() && SPCharacters.IsValidIndex(NewCharIndex) && !G->IsEditingText())
	{
		NewName = SPCharacters[NewCharIndex].Replace(TEXT("."), TEXT("_"));
	}
	// SPNimgPortraitBack (0.0304, 0.1014, 0.2341 x 0.7699), SPNclPlayerSkins (0.0482, 0.1919, 0.1996 x 0.5), arrows at 0.7337
	G->Section(P.Sub(0.030428f, 0.101485f, 0.234172f, 0.769958f), Caption(Sec, TEXT("SPNimgPortraitBack.Caption"), TEXT("Portrait")));
	const FString Char = SPCharacters.IsValidIndex(NewCharIndex) ? SPCharacters[NewCharIndex] : FString();
	const FRect Portrait = P.Sub(0.048207f, 0.191921f, 0.199566f, 0.5f);
	G->Image(TEXT("2K4Menus.Controls.sectionback"), Portrait, EImgStyle::Stretched, FLinearColor::White);
	G->Image(PortraitOf(Char), Portrait.Inset(G->L(3.f), G->L(3.f), G->L(3.f), G->L(3.f)), EImgStyle::Scaled, FLinearColor::White);
	G->Label(FRect(Portrait.X, Portrait.B() + G->L(2.f), Portrait.W, G->L(22.f)), Char, TEXT("TextLabel"), EAlign::Center);
	const int32 NumChars = FMath::Max(1, SPCharacters.Num());
	int32 CharStep = 0;
	if (G->Button(FGUI::Id(TEXT("SPPrevSkin")), P.Sub(0.060867f, 0.733708f, 0.048750f, 0.079861f), TEXT("ArrowLeft"), FString(),
		Caption(Sec, TEXT("SPNbtnPrevSkin.Hint"), TEXT("Selects a new appearance for your character"))))
	{
		CharStep = -1;
	}
	if (G->Button(FGUI::Id(TEXT("SPNextSkin")), P.Sub(0.178054f, 0.733708f, 0.048750f, 0.079861f), TEXT("ArrowRight"), FString(),
		Caption(Sec, TEXT("SPNbtnNextSkin.Hint"), TEXT("Selects a new appearance for your character"))))
	{
		CharStep = 1;
	}
	if (CharStep != 0)
	{
		const FString Prev = Char.Replace(TEXT("."), TEXT("_"));
		NewCharIndex = (NewCharIndex + CharStep + NumChars) % NumChars;
		if (NewName == Prev && SPCharacters.IsValidIndex(NewCharIndex))
		{
			NewName = SPCharacters[NewCharIndex].Replace(TEXT("."), TEXT("_"));
		}
	}
	// SPNimgSponsorBack (0.2738, 0.1014, 0.2532 x 0.7699), SPNilSponsor (0.3044, 0.1892, 0.1855 x 0.5063)
	G->Section(P.Sub(0.273865f, 0.101485f, 0.253223f, 0.769958f), Caption(Sec, TEXT("SPNimgSponsorBack.Caption"), TEXT("Team symbol")));
	const FString Symbol = D.TeamSymbols.IsValidIndex(NewSymbolIndex) ? D.TeamSymbols[NewSymbolIndex] : FString();
	if (!Symbol.IsEmpty())
	{
		G->Image(SymbolTexture(Symbol), P.Sub(0.304414f, 0.189271f, 0.185561f, 0.506366f), EImgStyle::Justified, FLinearColor::White);
	}
	const int32 NumSymbols = FMath::Max(1, D.TeamSymbols.Num());
	if (G->Button(FGUI::Id(TEXT("SPPrevSponsor")), P.Sub(0.311525f, 0.733708f, 0.048750f, 0.079861f), TEXT("ArrowLeft"), FString(),
		Caption(Sec, TEXT("SPNbtnPrevSponsor.Hint"), TEXT("Selects a new symbol for your team"))))
	{
		NewSymbolIndex = (NewSymbolIndex - 1 + NumSymbols) % NumSymbols;
	}
	if (G->Button(FGUI::Id(TEXT("SPNextSponsor")), P.Sub(0.433336f, 0.733708f, 0.054968f, 0.079861f), TEXT("ArrowRight"), FString(),
		Caption(Sec, TEXT("SPNbtnNextSponsor.Hint"), TEXT("Selects a new symbol for your team"))))
	{
		NewSymbolIndex = (NewSymbolIndex + 1) % NumSymbols;
	}
	// SPNimgEditBg (0.5362, 0.1014, 0.4251 x 0.7699): name (0.286), team (0.428), difficulty (0.5688), 0.3449 wide
	G->Section(P.Sub(0.536203f, 0.101485f, 0.425176f, 0.769958f), Caption(Sec, TEXT("SPNimgEditBg.Caption"), TEXT("Profile")));
	const float RowH = FMath::RoundToFloat(G->ScreenH() * 0.035f);
	const FRect NameR = P.Sub(0.572227f, 0.286001f, 0.344922f, 0.f);
	const FRect TeamR = P.Sub(0.572227f, 0.428001f, 0.344922f, 0.f);
	const FRect DiffR = P.Sub(0.572227f, 0.568803f, 0.344922f, 0.f);
	G->EditBox(FGUI::Id(TEXT("SPName")), FRect(NameR.X, NameR.Y, NameR.W, RowH), Caption(Sec, TEXT("SPNmeName.Caption"), TEXT("Player Name: ")), NewName,
		Caption(Sec, TEXT("SPNmeName.Hint"), TEXT("Your character's name")), 0.45f, 16);
	G->EditBox(FGUI::Id(TEXT("SPTeamName")), FRect(TeamR.X, TeamR.Y, TeamR.W, RowH), Caption(Sec, TEXT("SPNmeTeam.Caption"), TEXT("Team Name: ")), NewTeamName,
		Caption(Sec, TEXT("SPNmeTeam.Hint"), TEXT("The name of your team")), 0.45f, 24);
	TArray<FString> Levels;
	for (int32 i = 0; i < 8; ++i)
	{
		const FString L = D.Defaults.DifficultyLevels.IsValidIndex(i) ? D.Defaults.DifficultyLevels[i] : FString();
		Levels.Add(L.IsEmpty() ? FString::FromInt(i) : L);
	}
	G->Combo(FGUI::Id(TEXT("SPDifficulty")), FRect(DiffR.X, DiffR.Y, DiffR.W, RowH), Caption(Sec, TEXT("SPNmcDifficulty.Caption"), TEXT("Difficulty: ")),
		NewDifficulty, Levels, Caption(Sec, TEXT("SPNmcDifficulty.Hint"), TEXT("Customize your challenge")), 0.45f);
}

// =================================================================================================
// Tutorials (UT2K4SPTab_Tutorials)
// =================================================================================================

void FArenaSinglePlayerPage::DrawTutorialsTab(const FRect& P)
{
	// UT2K4SPTab_Tutorials.Tutorials (class defaults): GameClass, Preview, PreviewLocked, Map. The tutorial maps are
	// scripted (voice overs, scripted sequences, a guided match) and need gameplay we do not run yet, so they are shown
	// locked with a note instead of starting a plain match.
	static TArray<FString> Tutorials = GetStockArray(TEXT("GUI2K4.UT2K4SPTab_Tutorials"), TEXT("Tutorials"));
	const int32 Num = Tutorials.Num();
	if (Num == 0)
	{
		G->TextIn(G->Font(TEXT("UT2HeaderFont")), TEXT("No tutorials found in GUI2K4.u"), P, FLinearColor::White, EAlign::Center);
		return;
	}
	const int32 Rows = FMath::Max(1, FMath::RoundToInt(FMath::Sqrt(float(Num))));
	const int32 Cols = FMath::CeilToInt(float(Num) / float(Rows));
	const float ColW = P.W / Cols, RowH = (P.H * 0.9f) / Rows;
	const float Cell = FMath::Min(ColW, RowH) * 0.9f;
	for (int32 i = 0; i < Num; ++i)
	{
		const FString GameClass = StructField(Tutorials[i], TEXT("GameClass"));
		const FString Locked = StructField(Tutorials[i], TEXT("PreviewLocked"));
		float X = P.X + (i % Cols) * ColW + (ColW - Cell) * 0.5f;
		const float Y = P.Y + (i / Cols) * RowH + (RowH - Cell) * 0.5f;
		if ((i / Cols) + 1 == Rows && Rows * Cols > Num)
		{
			X += ColW * (Rows * Cols - Num) * 0.5f;
		}
		const FRect R(X, Y, Cell, Cell);
		const FRect Inner = G->Section(R, GameTypeName(GameClass), true);
		const FString Pic = !Locked.IsEmpty() ? Locked : StructField(Tutorials[i], TEXT("Preview"));
		const FBox2f Sub(FVector2f(0.f, 0.f), FVector2f(1023.f, 767.f));
		G->Image(Pic, Inner, EImgStyle::Scaled, FLinearColor(0.6f, 0.6f, 0.6f, 1.f), &Sub);
		G->CustomWidget(FGUI::Id(TEXT("SPTut"), i), R, false, Caption(TEXT("UT2K4SPTab_Tutorials"), TEXT("StartTutorial"), TEXT("Click to play tutorial for"))
			+ TEXT(" ") + GameTypeName(GameClass));
	}
	G->TextIn(G->Font(TEXT("UT2MenuFont")), TEXT("The scripted tutorials are not supported yet."), FRect(P.X, P.B() - P.H * 0.08f, P.W, P.H * 0.06f),
		C(255, 200, 0), EAlign::Center, true);
}

// =================================================================================================
// Ladders
// =================================================================================================

bool FArenaSinglePlayerPage::DrawLadderButton(const FRect& R, int32 Ladder, int32 Rung, uint32 Id)
{
	const int32 Progress = GP ? GP->GetLadderProgress(Ladder) : -1;
	const bool bReach = Progress >= 0 && Rung <= Progress;
	const bool bSel = SelLadder == Ladder && SelRung == Rung;
	const FMatchInfo M = GP ? GP->GetMatchInfo(Ladder, Rung) : FMatchInfo();
	const EMenuState St = G->CustomWidget(Id, R, bReach, MatchTitle(M));
	// UT2K4LadderVButton.SetState: LadderButton (open / out of reach), LadderButtonHi (completed), LadderButtonActive (selected)
	FString Frame = TEXT("InterfaceContent.SPMenu.LadderButton");
	if (bSel)
	{
		Frame += TEXT("Active");
	}
	else
	{
		if (bReach && Rung < Progress)
		{
			Frame += TEXT("Hi");
		}
		if (St == EMenuState::Watched || St == EMenuState::Focused)
		{
			Frame += TEXT("Over");
		}
		else if (St == EMenuState::Pressed)
		{
			Frame += TEXT("Pressed");
		}
	}
	// GUIGFXButton: the style image first, the graphic inside its border
	if (!G->Image(Frame, R, EImgStyle::Stretched, FLinearColor::White))
	{
		G->FallbackBox(R, St, true);
	}
	const FRect Inner = R.Inset(G->L(5.f), G->L(5.f), G->L(5.f), G->L(5.f));
	if (!bReach)
	{
		G->Image(TEXT("SinglePlayerThumbs.Grey"), Inner, EImgStyle::Scaled, FLinearColor::White);
	}
	else
	{
		const FString Shot = MapShot(M.LevelName);
		if (Shot.IsEmpty() || !G->ImageNormal(Shot, FString(), 0.f, Inner))
		{
			G->Draw().Rect(Inner.X, Inner.Y, Inner.W, Inner.H, FLinearColor::Black);
		}
	}
	return bReach && G->WasClicked(Id, R);
}

void FArenaSinglePlayerPage::DrawSingleLadderTab(const FRect& P, int32 Ladder)
{
	if (!GP)
	{
		return;
	}
	const FLadder* L = FindLadder(Ladder);
	const int32 N = L ? L->Matches.Num() : 0;
	if (N == 0)
	{
		return;
	}
	if (ShownLadder != Ladder)
	{
		ShownLadder = Ladder;
		LadderShownAt = G->Time();
		if (SelLadder != Ladder)
		{
			SelectNextMatch(Ladder);
		}
	}
	const double Since = G->Time() - LadderShownAt;
	// UT2K4SPTab_SingleLadder: LadderTop 0.05, LadderLeft 0.05, LadderHeight 0.8; the last match on top
	const float Top = 0.05f, Left = 0.05f, Height = 0.8f;
	const float Spacing = N > 1 ? Height / (N - 1) - LadderBtnH : 0.f;
	const int32 Progress = GP->GetLadderProgress(Ladder);
	TArray<FRect> Rects;
	Rects.SetNum(N);
	float Cur = Top;
	for (int32 i = N - 1; i >= 0; --i)
	{
		Rects[i] = P.Sub(Left, Cur, LadderBtnW, LadderBtnH);
		Cur += LadderBtnH + Spacing;
	}
	for (int32 i = N - 1; i >= 0; --i)
	{
		const float A = AnimAlpha(Since, i, N);
		// connection bar under the button to the previous match (BarVertical / Hi when reachable)
		if (i > 0 && A >= 1.f)
		{
			const float BarW = P.W * 0.003906f;
			const FRect Bar(Rects[i].X + Rects[i].W * 0.5f - BarW, Rects[i].B(), BarW, Rects[i - 1].Y - Rects[i].B());
			G->Image(Progress >= i ? TEXT("InterfaceContent.SPMenu.BarVerticalHi") : TEXT("InterfaceContent.SPMenu.BarVertical"), Bar, EImgStyle::Scaled,
				FLinearColor::White);
		}
	}
	for (int32 i = 0; i < N; ++i)
	{
		const FRect R = Lerp(Rects[0], Rects[i], AnimAlpha(Since, i, N));
		if (DrawLadderButton(R, Ladder, i, FGUI::Id(TEXT("SPRung"), Ladder * 100 + i)))
		{
			SelectMatch(Ladder, i);
		}
		// the labels slide in from the left (UT2K4SPTab_SingleLadder.CreateLabels / AnimateLabels)
		const float LabelL = (Left + LadderBtnW) * 1.05f;
		FRect LabelR = P.Sub(LabelL, Rects[i].Y > 0.f ? (Rects[i].Y - P.Y) / P.H : 0.f, 0.5f - LabelL - Left, LadderBtnH * 0.95f);
		const float LA = AnimAlpha(Since, i, N);
		LabelR.X = FMath::Lerp(P.X - LabelR.W, LabelR.X, LA);
		const FString Label = L->EntryLabels.IsValidIndex(i) ? L->EntryLabels[i] : FString();
		if (!Label.IsEmpty())
		{
			const FFontRef F = G->StyleFont(GetStyle(TEXT("NoBackground")), EMenuState::Blurry, EFontScale::Medium);
			const TArray<FString> Ln = G->Wrap(F, Label, LabelR.W);
			float Y = LabelR.Y + (LabelR.H - Ln.Num() * F.Height) * 0.5f;
			for (const FString& S : Ln)
			{
				G->Text(F, S, LabelR.X + 1.f, Y + 1.f, FLinearColor(0.f, 0.f, 0.f, 0.5f));
				G->Text(F, S, LabelR.X, Y, FArenaDraw::UTColor(GetStyle(TEXT("NoBackground")).FontColors[0]));
				Y += F.Height;
			}
		}
	}
	// double click plays it (onMatchDblClick)
	DrawMatchInfo(P, false);
}

void FArenaSinglePlayerPage::DrawMatchInfo(const FRect& P, bool bLadderLayout)
{
	const TCHAR* Sec = TEXT("UT2K4SPTab_LadderBase");
	// balance (SPLlblBalance, top right)
	G->Label(P.Sub(0.546250f, 0.004918f, 0.449999f, 0.041250f), Caption(Sec, TEXT("BalanceLabel"), TEXT("Balance:")) + TEXT(" ") + FProfile::MoneyToString(GP->Balance),
		TEXT("TextLabel"), EAlign::Right);
	if (SelLadder < 0 || SelRung < 0)
	{
		return;
	}
	const FMatchInfo M = GP->GetMatchInfo(SelLadder, SelRung);
	struct FLayout { float Match[4], Shot[4], Fee[4], Price[4], NoMoney[4], Detail[4], Text[4], Change[4]; };
	// UT2K4SPTab_LadderBase defaults / UT2K4SPTab_Ladder.InitComponent positions
	static const FLayout Base = {
		{ 0.514900f, 0.074300f, 0.469900f, 0.376600f }, { 0.529000f, 0.128400f, 0.443200f, 0.286900f }, { 0.524900f, 0.340400f, 0.449900f, 0.041200f },
		{ 0.524900f, 0.374000f, 0.449900f, 0.041200f }, { 0.530100f, 0.128900f, 0.442300f, 0.208400f }, { 0.514900f, 0.473400f, 0.469900f, 0.449900f },
		{ 0.531300f, 0.532100f, 0.439700f, 0.333700f }, { 0.565400f, 0.868200f, 0.371500f, 0.047600f } };
	static const FLayout Ladder = {
		{ 0.077117f, 0.415000f, 0.377143f, 0.303750f }, { 0.091122f, 0.469105f, 0.350408f, 0.214082f }, { 0.102500f, 0.608750f, 0.318750f, 0.041250f },
		{ 0.102500f, 0.644307f, 0.318750f, 0.041250f }, { 0.092296f, 0.465720f, 0.349362f, 0.214796f }, { 0.465510f, 0.415000f, 0.451250f, 0.303750f },
		{ 0.482015f, 0.472461f, 0.419515f, 0.188469f }, { 0.516377f, 0.664488f, 0.350765f, 0.046352f } };
	const FLayout& Lo = bLadderLayout ? Ladder : Base;
	auto R = [&P](const float (&V)[4]) { return P.Sub(V[0], V[1], V[2], V[3]); };
	G->Section(R(Lo.Match), Caption(Sec, TEXT("SPPsbgMatch.Caption"), TEXT("Selected match")), true);
	const FString Shot = MapShot(M.LevelName);
	const FRect ShotR = R(Lo.Shot);
	if (Shot.IsEmpty() || !G->ImageNormal(Shot, FString(), 0.f, ShotR))
	{
		G->Draw().Rect(ShotR.X, ShotR.Y, ShotR.W, ShotR.H, FLinearColor::Black);
	}
	const int32 MinFee = GP->GetMinimalEntryFeeFor(M);
	if (MinFee > GP->Balance || MinFee < 0)
	{
		// lblNoMoney: not enough healthy team mates, or not enough money
		FString Msg = Caption(Sec, TEXT("SPLlblNoMoney.Caption"), TEXT("You do not have enough money to enter this match."));
		const int32 Mates = GP->GetNumTeammatesForMatch(M);
		if (Mates > 0 && GP->NumHealthyTeammates() < Mates)
		{
			Msg = Caption(Sec, TEXT("NotEnoughPlayers"), TEXT("You do not have enough healthy team mates for this match.|%teammates% healthy team mates are required, you only have %healthy%."))
				.Replace(TEXT("%healthy%"), *FString::FromInt(GP->NumHealthyTeammates())).Replace(TEXT("%teammates%"), *FString::FromInt(Mates));
		}
		const FRect NM = R(Lo.NoMoney);
		G->Draw().Rect(NM.X, NM.Y, NM.W, NM.H, FLinearColor(0.f, 0.f, 0.f, 0.6f));
		const FFontRef F = G->Font(TEXT("UT2MenuFont"));
		const TArray<FString> Ln = G->Wrap(F, Msg, NM.W - G->L(8.f));
		float Y = NM.Y + (NM.H - Ln.Num() * F.Height) * 0.5f;
		for (const FString& S : Ln)
		{
			G->TextIn(F, S, FRect(NM.X, Y, NM.W, F.Height), C(255, 96, 96), EAlign::Center);
			Y += F.Height;
		}
	}
	if (!HasMap(M.LevelName))
	{
		G->TextIn(G->Font(TEXT("UT2MenuFont")), FString::Printf(TEXT("%s is not in the converted content"), *M.LevelName), ShotR, C(255, 96, 96), EAlign::Center, true);
	}
	if (M.EntryFee != 0)
	{
		G->TextIn(G->Font(TEXT("UT2MenuFont")), Caption(Sec, TEXT("EntryFee"), TEXT("Entry fee:")) + TEXT(" ") + FProfile::MoneyToString(M.EntryFee), R(Lo.Fee),
			M.EntryFee <= GP->Balance ? C(185, 207, 103) : C(255, 96, 96), EAlign::Center, true);
	}
	if (M.PrizeMoney != 0)
	{
		G->TextIn(G->Font(TEXT("UT2MenuFont")), Caption(Sec, TEXT("PrizeMoney"), TEXT("Prize money:")) + TEXT(" ") + FProfile::MoneyToString(M.PrizeMoney), R(Lo.Price),
			C(207, 185, 103), EAlign::Center, true);
	}
	// details: the match name (MenuName or the map's title), the map description and the game
	const FRect DetailC = G->Section(R(Lo.Detail), MatchTitle(M), true);
	(void)DetailC;
	FArenaLayoutInfo Info;
	FString Desc;
	if (AArenaLevelBuilder::FindLayout(M.LevelName, Info))
	{
		Desc = Info.Description;
	}
	const FString Game = GameTypeName(M.GameType);
	FString Head = Game;
	if (!GP->bIsChallenge && !M.EnemyTeamName.IsEmpty() && !M.EnemyTeamName.StartsWith(TEXT(";")) && !M.EnemyTeamName.Contains(TEXT("DMRoster")))
	{
		Head += TEXT(" - ") + TeamTitle(M.EnemyTeamName);
	}
	G->ScrollText(FGUI::Id(TEXT("SPMatchDesc")), R(Lo.Text), Head + TEXT("\n\n") + Lines(Desc));
	// CHANGE ARENA (MapChallenge) when the rung has alternative arenas
	if (GP->HasAltLevel(SelLadder, SelRung) && SelRung <= GP->GetLadderProgress(SelLadder))
	{
		const int32 MinFeeAll = GP->GetMinimalEntryFeeFor(M, true);
		const bool bAfford = float(M.PrizeMoney) * GP->MapChallengeCost < float(GP->Balance - MinFeeAll);
		if (G->Button(FGUI::Id(TEXT("SPChangeArena")), R(Lo.Change), TEXT("SquareButton"), Caption(Sec, TEXT("SPLbtnChallengeMap.Caption"), TEXT("CHANGE ARENA")),
			Caption(Sec, TEXT("SPLbtnChallengeMap.Hint"), TEXT("Select an alternative arena for this match")), bAfford, EFontScale::Small))
		{
			FDialog D;
			D.Kind = EDialog::MapChallenge;
			PushDialog(D);
		}
	}
}

void FArenaSinglePlayerPage::DrawLadderTab(const FRect& P)
{
	if (!GP)
	{
		return;
	}
	const TCHAR* Sec = TEXT("UT2K4SPTab_Ladder");
	// the four ladder selectors (CreateLadderButtons): CTF, BR, DOM, AS with their LadderShots pictures and progress
	struct FSel { int32 Ladder; float Top, Left; const TCHAR* Shot; const TCHAR* MoneyShot; };
	static const FSel Sels[4] = {
		{ LID_CTF, 0.041666f, 0.0125f, TEXT("LadderShots.CTFShot"), TEXT("LadderShots.CTFMoneyShot") },
		{ LID_BR, 0.125f, 0.13125f, TEXT("LadderShots.BRShot"), TEXT("LadderShots.BRMoneyShot") },
		{ LID_DOM, 0.041666f, 0.875001f, TEXT("LadderShots.DOMShot"), TEXT("LadderShots.DOMMoneyShot") },
		{ LID_AS, 0.125f, 0.756251f, TEXT("LadderShots.TeamDMShot"), TEXT("LadderShots.TeamDMMoneyShot") } };
	// lock symbols and bars of the championship (SPLimgChamp1..4, SPLimgChampBar1..4) and ladder labels (SPLlblLadder1..4)
	static const float Locks[4][4] = { { 0.4376f, 0.0643f, 0.0628f, 0.0873f }, { 0.4376f, 0.1502f, 0.0628f, 0.0873f }, { 0.5001f, 0.0643f, 0.0628f, 0.0873f },
		{ 0.5001f, 0.1502f, 0.0628f, 0.0873f } };
	static const float Bars[4][4] = { { 0.1242f, 0.1057f, 0.3130f, 0.0039f }, { 0.2410f, 0.1942f, 0.1975f, 0.0039f }, { 0.5630f, 0.1057f, 0.3130f, 0.0039f },
		{ 0.5630f, 0.1942f, 0.1955f, 0.0039f } };
	static const float Labels[4][4] = { { 0.1184f, 0.0608f, 0.3474f, 0.0431f }, { 0.2428f, 0.1491f, 0.2231f, 0.0431f }, { 0.5440f, 0.0608f, 0.3462f, 0.0431f },
		{ 0.5378f, 0.1491f, 0.2218f, 0.0431f } };
	int32 FirstOpen = -1;
	for (int32 i = 0; i < 4; ++i)
	{
		const FSel& S = Sels[i];
		const bool bDone = GP->CompletedLadder(S.Ladder);
		const bool bOpen = GP->GetLadderProgress(S.Ladder) > -1;
		if (bOpen && !bDone && FirstOpen < 0)
		{
			FirstOpen = S.Ladder;
		}
		G->Image(bDone ? TEXT("InterfaceContent.SPMenu.BarHorizontalHi") : TEXT("InterfaceContent.SPMenu.BarHorizontal"),
			P.Sub(Bars[i][0], Bars[i][1], Bars[i][2], Bars[i][3]), EImgStyle::Scaled, FLinearColor::White);
		G->Image(FString::Printf(TEXT("InterfaceContent.SPMenu.Lock%d%s"), i + 1, bDone ? TEXT("Hi") : TEXT("")),
			P.Sub(Locks[i][0], Locks[i][1], Locks[i][2], Locks[i][3]), EImgStyle::Scaled, FLinearColor::White);
		G->TextIn(G->StyleFont(GetStyle(TEXT("NoBackground")), EMenuState::Blurry, EFontScale::Medium), LadderDescription(S.Ladder).ToUpper(),
			P.Sub(Labels[i][0], Labels[i][1], Labels[i][2], Labels[i][3]), C(14, 41, 106), EAlign::Center);
		const FRect Box = P.Sub(S.Left, S.Top, 0.1125f, 0.13125f);
		const uint32 Id = FGUI::Id(TEXT("SPLadderSel"), i);
		const EMenuState St = G->CustomWidget(Id, Box, true, LadderDescription(S.Ladder), true);
		G->Image(bDone ? TEXT("2K4Menus.Controls.thinpipe_f") : TEXT("2K4Menus.Controls.thinpipe_b"), Box, EImgStyle::Stretched,
			St == EMenuState::Blurry ? FLinearColor::White : FLinearColor(1.f, 1.f, 0.7f, 1.f));
		const FRect Pic = FRect(Box.X + P.W * 0.01f, Box.Y + P.H * 0.01f, Box.W - P.W * 0.02f, Box.H * 0.64f);
		const FBox2f Sub(FVector2f(0.f, 0.f), FVector2f(1023.f, 767.f)); // the 4:3 part of the 1024 x 1024 LadderShots (X1..Y2)
		G->Image(bDone ? S.MoneyShot : S.Shot, Pic, EImgStyle::Scaled, bOpen ? FLinearColor::White : FLinearColor(0.35f, 0.35f, 0.35f, 1.f), &Sub);
		const int32 Len = LengthOfLadder(S.Ladder);
		const float Frac = Len > 0 ? FMath::Clamp(float(GP->GetLadderProgress(S.Ladder)) / Len, 0.f, 1.f) : 0.f;
		const FRect Prog(Pic.X, Pic.B() + P.H * 0.013333f, Pic.W, Box.H * 0.2f);
		// GUIProgressBar: dark bar, filled with the ladder's progress
		G->Draw().Rect(Prog.X, Prog.Y, Prog.W, Prog.H, FLinearColor(0.f, 0.f, 0.f, 0.6f));
		G->Draw().Rect(Prog.X, Prog.Y, Prog.W * Frac, Prog.H, C(255, 200, 0));
		if (G->WasClicked(Id, Box))
		{
			ShownLadder = S.Ladder;
			LadderShownAt = G->Time();
			SelectNextMatch(S.Ladder);
		}
	}
	// the championship button (SPLbtnChampFinal) once the championship is open
	const bool bChamp = GP->GetLadderProgress(LID_CHAMP) >= 0;
	if (bChamp)
	{
		const FRect CB = P.Sub(0.4586f, 0.0943f, LadderBtnW, LadderBtnH);
		const uint32 Id = FGUI::Id(TEXT("SPChampFinal"));
		const EMenuState St = G->CustomWidget(Id, CB, true, ArenaSP::Loc(TEXT("XGame"), TEXT("UT2K4GameProfile"), TEXT("msgChampionship"), TEXT("Championship")));
		const FString Pic = GP->CompletedLadder(LID_CHAMP) ? FString(TEXT("PlayerPictures.aDOM"))
			: MapShot(GP->GetMatchInfo(LID_CHAMP, GP->GetLadderProgress(LID_CHAMP)).LevelName);
		G->ImageNormal(Pic, FString(), 0.f, CB.Inset(G->L(5.f), G->L(5.f), G->L(5.f), G->L(5.f)));
		G->Image(St == EMenuState::Blurry ? TEXT("InterfaceContent.SPMenu.LadderButton") : TEXT("InterfaceContent.SPMenu.LadderButtonOver"), CB,
			EImgStyle::Stretched, FLinearColor::White);
		if (G->WasClicked(Id, CB))
		{
			ShownLadder = LID_CHAMP;
			LadderShownAt = G->Time();
			SelectNextMatch(LID_CHAMP);
		}
	}
	if (ShownLadder < 0)
	{
		ShownLadder = bChamp ? LID_CHAMP : (FirstOpen >= 0 ? FirstOpen : LID_DOM);
		LadderShownAt = G->Time();
		SelectNextMatch(ShownLadder);
	}
	// challenges (SPLsbgChallengeBg / SPLcbChallenges)
	G->Section(P.Sub(0.2812f, 0.2594f, 0.4437f, 0.1403f), Caption(Sec, TEXT("SPLsbgChallengeBg.Caption"), TEXT("Challenges")), true);
	{
		TArray<FString> Items = { Caption(Sec, TEXT("msgSelectChal"), TEXT("Select a challenge")) };
		for (const FChallengeInfo& Ch : Get().Challenges)
		{
			Items.Add(Ch.Name);
		}
		int32 Sel = 0;
		const FRect CR = P.Sub(0.2949f, 0.3150f, 0.4173f, 0.0486f);
		if (G->Combo(FGUI::Id(TEXT("SPChallenges")), FRect(CR.X, CR.Y, CR.W, FMath::Min(CR.H, G->ScreenH() * 0.035f)), FString(), Sel, Items,
			Caption(Sec, TEXT("SPLcbChallenges.Hint"), TEXT("Challenge another team")), 0.f) && Sel > 0 && Get().Challenges.IsValidIndex(Sel - 1))
		{
			FDialog D;
			D.Kind = EDialog::Challenge;
			D.ChallengeClass = Get().Challenges[Sel - 1].Class;
			PushDialog(D);
		}
	}
	// the selected ladder (SPLimgLadderBg) with its horizontal buttons (CreateHButtons at 0.655 below the panel top)
	const FString LadderName = ShownLadder == LID_CHAMP ? ArenaSP::Loc(TEXT("XGame"), TEXT("UT2K4GameProfile"), TEXT("msgChampionship"), TEXT("Championship"))
		: LadderDescription(ShownLadder);
	G->Section(P.Sub(0.0149f, 0.7387f, 0.97f, 0.2187f), LadderName);
	const int32 N = LengthOfLadder(ShownLadder);
	if (N > 0)
	{
		const bool bCh = ShownLadder == LID_CHAMP;
		const float Left = bCh ? 0.25f : 0.05f, Width = bCh ? 0.81f * 0.5f : 0.81f, Top = 0.805f;
		const float Spacing = N > 1 ? Width / (N - 1) - LadderBtnW : 0.f;
		const double Since = G->Time() - LadderShownAt;
		const int32 Progress = GP->GetLadderProgress(ShownLadder);
		TArray<FRect> Rects;
		for (int32 i = 0; i < N; ++i)
		{
			Rects.Add(P.Sub(Left + i * (LadderBtnW + Spacing), Top, LadderBtnW, LadderBtnH));
		}
		for (int32 i = 1; i < N; ++i)
		{
			if (AnimAlpha(Since, i, N) >= 1.f)
			{
				const float BarH = P.H * 0.003906f;
				const FRect Bar(Rects[i - 1].R(), Rects[i].Y + Rects[i].H * 0.5f - BarH, Rects[i].X - Rects[i - 1].R(), BarH);
				G->Image(Progress >= i ? TEXT("InterfaceContent.SPMenu.BarHorizontalHi") : TEXT("InterfaceContent.SPMenu.BarHorizontal"), Bar, EImgStyle::Scaled,
					FLinearColor::White);
			}
		}
		for (int32 i = 0; i < N; ++i)
		{
			if (DrawLadderButton(Lerp(Rects[0], Rects[i], AnimAlpha(Since, i, N)), ShownLadder, i, FGUI::Id(TEXT("SPRung"), ShownLadder * 100 + i)))
			{
				SelectMatch(ShownLadder, i);
			}
		}
	}
	DrawMatchInfo(P, true);
}

void FArenaSinglePlayerPage::DrawAdditionalTab(const FRect& P)
{
	const FData& D = Get();
	if (!GP || D.CustomLadders.Num() == 0)
	{
		return;
	}
	// UT2K4SPTab_ExtraLadder: a tab per additional ladder (CLOnslaught ...), each a single ladder page
	TArray<FString> Names, Hints;
	for (const FLadder& L : D.CustomLadders)
	{
		Names.Add(L.Name);
		Hints.Add(FString());
		GP->EnableLadder(L.Id); // RegisterCustomLadder
	}
	const FRect TabsR(P.X, P.Y, P.W, FMath::RoundToFloat(P.H * 0.0399f / 0.72f * 0.72f));
	G->Tabs(FGUI::Id(TEXT("SPExtraTabs")), FRect(TabsR.X, TabsR.Y, TabsR.W, FMath::RoundToFloat(G->ScreenH() * 0.035f)), Names, Hints, AdditionalIndex);
	AdditionalIndex = FMath::Clamp(AdditionalIndex, 0, D.CustomLadders.Num() - 1);
	const float Off = G->ScreenH() * 0.04f;
	const FRect Inner(P.X, P.Y + Off, P.W, P.H - Off);
	DrawSingleLadderTab(Inner, D.CustomLadders[AdditionalIndex].Id);
	if (Dialogs.Num() == 0)
	{
		const FString Mode = GameTypeToMode(D.CustomLadders[AdditionalIndex].Matches[0].GameType);
		if (Mode.IsEmpty())
		{
			G->TextIn(G->Font(TEXT("UT2MenuFont")), TEXT("This game type is not supported yet."), FRect(P.X, P.B() - P.H * 0.06f, P.W * 0.5f, P.H * 0.05f),
				C(255, 200, 0), EAlign::Center);
		}
	}
}

// =================================================================================================
// Team management (UT2K4SPTab_TeamManagement)
// =================================================================================================

void FArenaSinglePlayerPage::DrawTeamTab(const FRect& P)
{
	if (!GP)
	{
		return;
	}
	const TCHAR* Sec = TEXT("UT2K4SPTab_TeamManagement");
	FProfile& Pr = *GP;
	// editing is locked until the team qualification is won, except during the draft (LockManagementTools)
	const bool bLocked = !bInitialDraft && !Pr.CompletedLadder(LID_TDM);
	Pr.PlayerTeam.SetNum(Pr.MaxTeamSize);

	// your team (SPLsbgTeamBg, AltSection 0.0061, 0.0166, 0.3187 x 0.9687) with the portraits (CreateTeamButtons(2, 3, 0.04))
	const float TL = 0.006158f, TT = 0.016667f, TW = 0.318750f, TH = 0.968750f;
	G->Section(P.Sub(TL, TT, TW, TH), Caption(Sec, TEXT("SPLsbgTeamBg.Caption"), TEXT("Your team")), true);
	{
		const int32 Cols = 2, Rows = 3;
		const float RowH = TH / Rows * 0.87f, ColW = TW / Cols;
		float CellW = ColW * 0.9f, CellH = RowH * 0.8f;
		const float X0 = FMath::Min(CellW * 2.55f, CellH);
		CellW = X0 / 2.55f;
		CellH = X0;
		const float SpH = (RowH - CellH) / 2.f + 0.005f, SpW = (ColW - CellW) / 2.f - 0.005f;
		int32 TotalTreat = 0;
		for (int32 i = 0; i < Pr.MaxTeamSize; ++i)
		{
			float X = (i % Cols) * ColW + SpW + TL / 2.f;
			const float Y = (i / Cols) * RowH + SpH + 0.04f + TT;
			if ((i / Cols) + 1 == Rows && Rows * Cols > Pr.MaxTeamSize)
			{
				X += ColW * (Rows * Cols - Pr.MaxTeamSize) / 2.f;
			}
			G->Image(TEXT("2K4Menus.Controls.sectionback"), P.Sub(X - 0.00125f, Y - 0.005f, CellW + 0.0025f, CellH + 0.009375f), EImgStyle::Scaled, FLinearColor::White);
			const FRect Cell = P.Sub(X, Y, CellW, CellH);
			const FString& Name = Pr.PlayerTeam[i];
			const int32 B = Name.IsEmpty() ? INDEX_NONE : Pr.GetBotPosition(Name, true);
			const bool bNew = NewTeam.ContainsByPredicate([&Name](const FString& N) { return !N.IsEmpty() && N.Equals(Name, ESearchCase::IgnoreCase); });
			const float Alpha = Name.IsEmpty() || bNew ? 0.5f : 1.f;
			G->Image(Name.IsEmpty() ? TEXT("PlayerPictures.cDefault") : PortraitOf(Name), Cell, EImgStyle::Scaled, FLinearColor(1.f, 1.f, 1.f, Alpha));
			if (ActiveBot.Equals(Name, ESearchCase::IgnoreCase) && !Name.IsEmpty())
			{
				G->Draw().Outline(Cell.X, Cell.Y, Cell.W, Cell.H, C(255, 200, 0), FMath::Max(1.f, G->L(2.f)));
			}
			// health bar (bottom to top, red to green)
			FString Hint;
			if (B != INDEX_NONE)
			{
				const int32 H = Pr.BotStats[B].Health;
				TotalTreat += Pr.TreatmentCost(B);
				const FRect Bar = P.Sub(X + CellW, Y, 0.015f, CellH);
				G->Draw().Rect(Bar.X, Bar.Y, Bar.W, Bar.H, FLinearColor(0.f, 0.f, 0.f, 0.5f));
				const float F = H / 100.f;
				G->Draw().Rect(Bar.X, Bar.B() - Bar.H * F, Bar.W, Bar.H * F, FArenaDraw::UTColor(FColor(uint8(255 * (1.f - F)), uint8(255 * F), 0)));
				Hint = Caption(Sec, TEXT("HealthHint"), TEXT("%player%'s health is at %health%%")).Replace(TEXT("%player%"), *Name).Replace(TEXT("%health%"), *FString::FromInt(H));
				G->Label(FRect(Cell.X, Cell.B() - G->L(18.f), Cell.W, G->L(18.f)), Name, TEXT("TextLabel"), EAlign::Center, EFontScale::Small);
			}
			const uint32 Id = FGUI::Id(TEXT("SPTeamSlot"), i);
			G->CustomWidget(Id, Cell, !Name.IsEmpty(), Hint);
			if (G->WasClicked(Id, Cell) && !Name.IsEmpty())
			{
				ActiveBot = Name;
				FreeAgentSel = -1;
			}
		}
		// overview (SPLsbgOverviewBg): balance, treatment
		G->Section(P.Sub(0.337372f, 0.208333f, 0.212628f, 0.468750f), FString(), true);
		G->Label(P.Sub(0.355168f, 0.271250f, 0.179689f, 0.034141f), Caption(Sec, TEXT("SPTMedBalance.Caption"), TEXT("Balance:")), TEXT("TextLabel"), EAlign::Left,
			EFontScale::Small);
		G->Label(P.Sub(0.355168f, 0.310f, 0.179689f, 0.034141f), FProfile::MoneyToString(Pr.Balance), TEXT("TextLabel"), EAlign::Right, EFontScale::Small);
		const int32 Active = ActiveBot.IsEmpty() ? INDEX_NONE : Pr.FindBot(ActiveBot);
		const int32 Treat = Active != INDEX_NONE ? Pr.TreatmentCost(Active) : 0;
		if (G->Button(FGUI::Id(TEXT("SPTreat")), P.Sub(0.352551f, 0.521590f, 0.184876f, 0.05f), TEXT("SquareButton"), Caption(Sec, TEXT("SPPbtnTreat.Caption"), TEXT("TREAT INJURIES")),
			Caption(Sec, TEXT("SPPbtnTreat.Hint"), TEXT("Treat the injuries of the current player")), Treat > 0 && Treat < Pr.Balance - Pr.MinBalance, EFontScale::Small))
		{
			Pr.BotStats[Active].Health = 100;
			Pr.Balance -= Treat;
			SaveProfile();
		}
		if (G->Button(FGUI::Id(TEXT("SPTreatAll")), P.Sub(0.352551f, 0.570370f, 0.184876f, 0.05f), TEXT("SquareButton"), Caption(Sec, TEXT("SPPbtnTreatAll.Caption"), TEXT("TREAT ALL")),
			TotalTreat > 0 ? FProfile::MoneyToString(TotalTreat) : FString(), TotalTreat > 0 && TotalTreat < Pr.Balance - Pr.MinBalance, EFontScale::Small))
		{
			for (FBotStats& Bs : Pr.BotStats)
			{
				Bs.Health = 100;
			}
			Pr.Balance -= TotalTreat;
			SaveProfile();
		}
	}

	// free agents (FillFreeAgentsList: free BotStats with a character record, not yourself)
	TArray<FString> Free;
	for (const FBotStats& B : Pr.BotStats)
	{
		if (B.bFreeAgent && !B.Name.Equals(Pr.PlayerCharacter, ESearchCase::IgnoreCase) && FindPlayer(B.Name))
		{
			Free.Add(B.Name);
		}
	}
	const bool bFreeSel = Free.IsValidIndex(FreeAgentSel);
	const FString Selected = bFreeSel ? Free[FreeAgentSel] : ActiveBot;
	const bool bSelIsMate = !Selected.IsEmpty() && Pr.IsTeammate(Selected);
	const int32 SelId = Selected.IsEmpty() ? INDEX_NONE : Pr.GetBotPosition(Selected, true);
	const int32 SelPrice = SelId != INDEX_NONE ? Pr.BotStats[SelId].Price : 0;

	// HIRE / RELEASE / AUTO FILL
	const bool bCanHire = !bLocked && bFreeSel && !Pr.HasFullTeam() && Pr.Balance - Pr.MinBalance >= SelPrice;
	if (G->Button(FGUI::Id(TEXT("SPHire")), P.Sub(0.341122f, 0.033334f, 0.206250f, 0.05f), TEXT("SquareButton"), Caption(Sec, TEXT("SPPbtnHire.Caption"), TEXT("HIRE")),
		Caption(Sec, TEXT("SPPbtnHire.Hint"), TEXT("Hire the selected player")), bCanHire, EFontScale::Small))
	{
		if (Pr.AddTeammate(Selected))
		{
			NewTeam.AddUnique(Selected);
			Pr.Balance -= SelPrice;
			ActiveBot = Selected;
			FreeAgentSel = -1;
			SaveProfile();
		}
	}
	if (G->Button(FGUI::Id(TEXT("SPFire")), P.Sub(0.341122f, 0.091667f, 0.206250f, 0.05f), TEXT("SquareButton"), Caption(Sec, TEXT("SPPbtnFire.Caption"), TEXT("RELEASE")),
		Caption(Sec, TEXT("SPPbtnFire.Hint"), TEXT("Fire this team mate")), !bLocked && bSelIsMate, EFontScale::Small))
	{
		if (Pr.ReleaseTeammate(Selected))
		{
			// refunded when hired in this session (FireBot's NewTeam list); the released player is healed
			if (NewTeam.ContainsByPredicate([&Selected](const FString& N) { return N.Equals(Selected, ESearchCase::IgnoreCase); }))
			{
				Pr.Balance += SelPrice;
				NewTeam.RemoveAll([&Selected](const FString& N) { return N.Equals(Selected, ESearchCase::IgnoreCase); });
			}
			Pr.BotStats[SelId].Health = 100;
			ActiveBot.Reset();
			SaveProfile();
		}
	}
	if (G->Button(FGUI::Id(TEXT("SPAutoFill")), P.Sub(0.341122f, 0.15f, 0.206250f, 0.05f), TEXT("SquareButton"), Caption(Sec, TEXT("SPPbtnAutoMake.Caption"), TEXT("AUTO FILL")),
		Caption(Sec, TEXT("SPPbtnAutoMake.Hint"), TEXT("Automatically fill your team")), !bLocked && !Pr.HasFullTeam(), EFontScale::Small))
	{
		ApplyTestCommand(TEXT("SPAutoFill"), FString());
#if UE_BUILD_SHIPPING
		// (shipping builds have no test commands: same rule inline)
		for (int32 i = 0; i < Free.Num() && !Pr.HasFullTeam(); ++i)
		{
			const int32 Id = Pr.FindBot(Free[i % Free.Num()]);
			if (Id != INDEX_NONE && Pr.Balance - Pr.MinBalance > Pr.BotStats[Id].Price && Pr.AddTeammate(Free[i]))
			{
				Pr.Balance -= Pr.BotStats[Id].Price;
				NewTeam.Add(Free[i]);
			}
		}
		SaveProfile();
#endif
	}

	// selection (SPLsbgSelectionBg): portrait, details (fee, health, ratings, favorite weapon), description
	G->Section(P.Sub(0.5625f, 0.016667f, 0.425f, 0.6625f), Caption(Sec, TEXT("SPLsbgSelectionBg.Caption"), TEXT("Selection")));
	G->Image(TEXT("2K4Menus.Controls.sectionback"), P.Sub(0.589063f, 0.081250f, 0.094922f, 0.3f), EImgStyle::Scaled, FLinearColor::White);
	if (const FPlayerRecord* R = Selected.IsEmpty() ? nullptr : FindPlayer(Selected))
	{
		G->Image(R->Portrait, P.Sub(0.590345f, 0.088159f, 0.092579f, 0.289943f), EImgStyle::Scaled, FLinearColor::White);
		FString Det = Caption(Sec, TEXT("FeeCaption"), TEXT("Fee:")) + TEXT(" ") + FProfile::MoneyToString(SelPrice);
		const int32 Treat = Pr.TreatmentCost(SelId);
		if (Treat > 0)
		{
			Det += TEXT("\n") + Caption(Sec, TEXT("HealthCaption"), TEXT("Health:")) + FString::Printf(TEXT(" %d%%\n     (%s)"), Pr.BotStats[SelId].Health,
				*FProfile::MoneyToString(Treat));
		}
		else
		{
			Det += TEXT("\n");
		}
		Det += TEXT("\n") + Caption(Sec, TEXT("AccuracyCaption"), TEXT("Accuracy:")) + FString::Printf(TEXT(" %d%%"), AccuracyRating(*R));
		Det += TEXT("\n") + Caption(Sec, TEXT("AggressionCaption"), TEXT("Aggressiveness:")) + FString::Printf(TEXT(" %d%%"), AggressivenessRating(*R));
		Det += TEXT("\n") + Caption(Sec, TEXT("AgilityCaption"), TEXT("Agility:")) + FString::Printf(TEXT(" %d%%"), AgilityRating(*R));
		Det += TEXT("\n") + Caption(Sec, TEXT("TacticsCaption"), TEXT("Tactics:")) + FString::Printf(TEXT(" %d%%"), TacticsRating(*R));
		Det += TEXT("\n") + FavoriteWeaponText(*R);
		G->ScrollText(FGUI::Id(TEXT("SPSelDetails")), P.Sub(0.689063f, 0.081250f, 0.278321f, 0.280859f), Det);
		G->ScrollText(FGUI::Id(TEXT("SPSelDesc")), P.Sub(0.585231f, 0.380157f, 0.382031f, 0.274766f), Lines(CharacterText(*R)));
	}

	// free agents (SPLsbgFreeAgentBg 0.3348, 0.6999, 0.6627 x 0.2874), the list (0.3861, 0.7627, 0.5615 x 0.2044), arrows
	G->Section(P.Sub(0.334800f, 0.699900f, 0.662700f, 0.287400f), Caption(Sec, TEXT("SPLsbgFreeAgentBg.Caption"), TEXT("Free agents")));
	const FRect ListR = P.Sub(0.386122f, 0.762721f, 0.561505f, 0.204490f);
	const float ItemW = ListR.H * 0.45f;
	const int32 PerPage = FMath::Max(1, FMath::FloorToInt(ListR.W / ItemW));
	const int32 MaxScroll = FMath::Max(0, Free.Num() - PerPage);
	if (G->Button(FGUI::Id(TEXT("SPAgentsLeft")), P.Sub(0.343750f, 0.806100f, 0.05f, 0.09f), TEXT("ArrowLeft"), FString(), FString(), FreeAgentScroll > 0))
	{
		FreeAgentScroll = FMath::Max(0, FreeAgentScroll - PerPage);
	}
	if (G->Button(FGUI::Id(TEXT("SPAgentsRight")), P.Sub(0.938750f, 0.806100f, 0.05f, 0.09f), TEXT("ArrowRight"), FString(), FString(), FreeAgentScroll < MaxScroll))
	{
		FreeAgentScroll = FMath::Min(MaxScroll, FreeAgentScroll + PerPage);
	}
	FreeAgentScroll = FMath::Clamp(FreeAgentScroll, 0, MaxScroll);
	const float Gap = (ListR.W - PerPage * ItemW) / FMath::Max(1, PerPage);

	for (int32 k = 0; k < PerPage && FreeAgentScroll + k < Free.Num(); ++k)
	{
		const int32 i = FreeAgentScroll + k;
		const FRect Cell(ListR.X + k * (ItemW + Gap) + Gap * 0.5f, ListR.Y, ItemW, ListR.H);
		const int32 Id = Pr.FindBot(Free[i]);
		const bool bAfford = Id != INDEX_NONE && Pr.Balance - Pr.MinBalance >= Pr.BotStats[Id].Price;
		const FLinearColor Tint(1.f, 1.f, 1.f, bLocked || !bAfford ? 0.5f : 1.f);
		if (!G->Image(PortraitOf(Free[i]), Cell, EImgStyle::Scaled, Tint))
		{
			// characters of packages that are not converted (custom character packs): the empty portrait
			G->Image(TEXT("PlayerPictures.cDefault"), Cell, EImgStyle::Scaled, Tint);
		}
		G->Label(FRect(Cell.X - Gap * 0.5f, Cell.B() - G->L(16.f), Cell.W + Gap, G->L(16.f)), Free[i], TEXT("TextLabel"), EAlign::Center, EFontScale::Small);
		if (FreeAgentSel == i)
		{
			G->Draw().Outline(Cell.X, Cell.Y, Cell.W, Cell.H, C(255, 200, 0), FMath::Max(1.f, G->L(2.f)));
		}
		const uint32 WId = FGUI::Id(TEXT("SPAgent"), i);
		G->CustomWidget(WId, Cell, true, Free[i] + TEXT(" - ") + (Id != INDEX_NONE ? FProfile::MoneyToString(Pr.BotStats[Id].Price) : FString()));
		if (G->WasClicked(WId, Cell))
		{
			FreeAgentSel = i;
			ActiveBot.Reset();
		}
	}
}
