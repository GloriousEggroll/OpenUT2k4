#include "ArenaSinglePlayer.h"
#include "ArenaSinglePlayerPrivate.h"
#include "ArenaGUI2K4Pages.h"
#include "ArenaHUD.h"
#include "ArenaLevelBuilder.h"
#include "ArenaRuntimeContent.h"
#include "OpenUT2k4.h"
#include "ArenaUserSettings.h"
#include "GameFramework/HUD.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"

using namespace ArenaGUI2K4;
using namespace ArenaSP;
using namespace ArenaSPUI;

namespace ArenaSPDialogsLocal
{
	FLinearColor C(uint8 R, uint8 G, uint8 B, uint8 A = 255)
	{
		return FArenaDraw::UTColor(FColor(R, G, B, A));
	}

	FString Money(int32 V)
	{
		return FProfile::MoneyToString(V);
	}

	/** StrengthLabels of UT2K4SPTab_DetailEnemies ("Weak" .. "Phantom"). */
	FString Strength(int32 Level)
	{
		static const TCHAR* const Fallback[] = { TEXT("Weak"), TEXT("Tough"), TEXT("Strong"), TEXT("Godlike"), TEXT("Phantom") };
		const int32 I = FMath::Clamp(Level < 0 ? 4 : Level, 0, 4);
		return LocArray(TEXT("GUI2K4"), TEXT("UT2K4SPTab_DetailEnemies"), TEXT("StrengthLabels"), I, Fallback[I]);
	}

	/** High score table (SPHighScore): the stock drone entries plus the finished profiles, saved next to the profiles. */
	struct FScore { FString Name; int32 Balance = 0; int32 Matches = 0; int32 Wins = 0; };

	TArray<FScore> LoadScores()
	{
		TArray<FScore> Out;
		for (const FString& S : GetStockArray(TEXT("GUI2K4.SPHighScore"), TEXT("Scores")))
		{
			FScore X;
			X.Name = StructField(S, TEXT("Name"));
			X.Balance = FCString::Atoi(*StructField(S, TEXT("Balance")));
			X.Matches = FCString::Atoi(*StructField(S, TEXT("Matches")));
			X.Wins = FCString::Atoi(*StructField(S, TEXT("Wins")));
			Out.Add(X);
		}
		FString Text;
		if (FFileHelper::LoadFileToString(Text, *(FProfile::GetDir() / TEXT("HighScores.json"))))
		{
			TSharedPtr<FJsonObject> J;
			if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J) && J.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
				if (J->TryGetArrayField(TEXT("Scores"), A))
				{
					for (const TSharedPtr<FJsonValue>& V : *A)
					{
						const TSharedPtr<FJsonObject> O = V->AsObject();
						FScore X;
						X.Name = O->GetStringField(TEXT("Name"));
						X.Balance = int32(O->GetNumberField(TEXT("Balance")));
						X.Matches = int32(O->GetNumberField(TEXT("Matches")));
						X.Wins = int32(O->GetNumberField(TEXT("Wins")));
						Out.Add(X);
					}
				}
			}
		}
		Out.StableSort([](const FScore& A, const FScore& B) { return A.Balance > B.Balance; });
		return Out;
	}

	void AddScore(const FProfile& P)
	{
		FString Text;
		TSharedPtr<FJsonObject> J;
		if (!FFileHelper::LoadFileToString(Text, *(FProfile::GetDir() / TEXT("HighScores.json"))) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J) || !J.IsValid())
		{
			J = MakeShared<FJsonObject>();
		}
		TArray<TSharedPtr<FJsonValue>> A;
		const TArray<TSharedPtr<FJsonValue>>* Old = nullptr;
		if (J->TryGetArrayField(TEXT("Scores"), Old))
		{
			A = *Old;
		}
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("Name"), P.bCheater ? FString(TEXT("*** CHEATER ***")) : P.PlayerName);
		O->SetNumberField(TEXT("Balance"), P.Balance);
		O->SetNumberField(TEXT("Matches"), P.Matches);
		O->SetNumberField(TEXT("Wins"), P.Wins);
		O->SetNumberField(TEXT("Difficulty"), P.BaseDifficulty);
		A.Add(MakeShared<FJsonValueObject>(O));
		J->SetArrayField(TEXT("Scores"), A);
		FString Out;
		FJsonSerializer::Serialize(J.ToSharedRef(), TJsonWriterFactory<>::Create(&Out));
		FFileHelper::SaveStringToFile(Out, *(FProfile::GetDir() / TEXT("HighScores.json")));
	}
}

using namespace ArenaSPDialogsLocal;

bool ArenaSPUI::UnlockCharacter(const FString& MenuTag)
{
	// UT2K4MainPage.UnlockCharacter: the tag list, saved
	UArenaUserSettings* U = UArenaUserSettings::Get();
	if (!U || MenuTag.IsEmpty() || IsUnlocked(MenuTag))
	{
		return false;
	}
	U->UnlockedCharacters = U->UnlockedCharacters.IsEmpty() ? MenuTag : U->UnlockedCharacters + TEXT(";") + MenuTag;
	U->Save();
	UE_LOG(LogOpenUT2k4, Log, TEXT("SinglePlayer: character %s unlocked"), *MenuTag);
	return true;
}

bool ArenaSPUI::IsUnlocked(const FString& MenuTag)
{
	const UArenaUserSettings* U = UArenaUserSettings::Get();
	return U && !MenuTag.IsEmpty() && (TEXT(";") + U->UnlockedCharacters.ToUpper() + TEXT(";")).Contains(TEXT(";") + MenuTag.ToUpper() + TEXT(";"));
}

FRect FArenaSinglePlayerPage::DialogFrame(float L, float T, float W, float H, const FString& Title)
{
	// FloatingWindow / LargeWindow: FloatingFrameBackground NewControls.Display1 (stretched) below a GUIHeader title bar
	const FRect P = G->Page();
	const FRect Win(P.X + L * P.W, T * G->ScreenH(), W * P.W, H * G->ScreenH());
	G->Draw().Rect(0.f, 0.f, G->ScreenW(), G->ScreenH(), FLinearColor(0.f, 0.f, 0.f, 0.35f));
	const FRect Frame = Win.Sub(0.f, 0.02f, 1.f, 0.98f);
	if (!G->Image(TEXT("2K4Menus.NewControls.Display1"), Frame, EImgStyle::Stretched, FLinearColor::White))
	{
		G->FallbackBox(Frame, EMenuState::Blurry, false);
	}
	const FRect TitleR = Win.Sub(0.f, 0.f, 1.f, FMath::Max(0.04375f, G->L(25.f) / FMath::Max(1.f, Win.H)));
	const FStyle& HS = GetStyle(TEXT("Header"));
	if (!G->StyleImage(HS, EMenuState::Blurry, TitleR))
	{
		G->FallbackBox(TitleR, EMenuState::Focused, true);
	}
	if (!Title.IsEmpty())
	{
		G->StyleText(HS, EMenuState::Blurry, EFontScale::Small, Title, TitleR, EAlign::Center);
	}
	return Win;
}

bool FArenaSinglePlayerPage::DrawDialog()
{
	if (Dialogs.Num() == 0)
	{
		return false;
	}
	FDialog& D = Dialogs.Last();
	switch (D.Kind)
	{
	case EDialog::Question: DrawQuestion(D); break;
	case EDialog::Message: DrawMessage(D); break;
	case EDialog::TeamRoles: DrawTeamRoles(D); break;
	case EDialog::Injury: DrawInjury(D); break;
	case EDialog::MapChallenge: DrawMapChallenge(D); break;
	case EDialog::Details: DrawDetails(D); break;
	case EDialog::TeamDetails: DrawTeamDetails(D); break;
	case EDialog::Challenge: DrawChallenge(D); break;
	case EDialog::HighScores: DrawHighScores(D); break;
	case EDialog::Trade:
	case EDialog::UnTrade: DrawTrade(D); break;
	default: CloseDialog(); break;
	}
	return true;
}

// ---- GUI2K4QuestionPage ----

void FArenaSinglePlayerPage::DrawQuestion(FDialog& D)
{
	// imgBack NewControls.Display2 across the screen (0.2979 .. 0.6995), lblQuestion (0.15, 0.45, 0.7 x 0.2), buttons below
	const FRect S = G->Screen();
	const FRect Back(S.X, S.H * 0.297917f, S.W, S.H * 0.401563f);
	G->Draw().Rect(0.f, 0.f, S.W, S.H, FLinearColor(0.f, 0.f, 0.f, 0.4f));
	if (!G->Image(TEXT("2K4Menus.NewControls.Display2"), Back, EImgStyle::Stretched, FLinearColor::White))
	{
		G->FallbackBox(Back, EMenuState::Blurry, false);
	}
	const FRect P = G->Page();
	const FRect Q(P.X + P.W * 0.15f, S.H * 0.36f, P.W * 0.7f, S.H * 0.24f);
	const FFontRef F = G->Font(TEXT("UT2MenuFont"));
	const TArray<FString> Ln = G->Wrap(F, Lines(D.Text), Q.W);
	float Y = Q.Y + FMath::Max(0.f, (Q.H - Ln.Num() * F.Height) * 0.5f);
	for (const FString& L : Ln)
	{
		G->TextIn(F, L, FRect(Q.X, Y, Q.W, F.Height), FLinearColor::White, EAlign::Center);
		Y += F.Height;
	}
	const float BW = P.W * 0.16f, BH = S.H * 0.045f, BY = Back.B() - BH - S.H * 0.025f;
	if (D.bYesNo)
	{
		const bool bYes = G->Button(FGUI::Id(TEXT("SPQYes")), FRect(P.X + P.W * 0.5f - BW - P.W * 0.01f, BY, BW, BH), TEXT("SquareButton"),
			ArenaSP::Loc(TEXT("XInterface"), TEXT("GUIQuestionPage"), TEXT("YesButtonText"), TEXT("YES")), FString());
		const bool bNo = G->Button(FGUI::Id(TEXT("SPQNo")), FRect(P.X + P.W * 0.51f, BY, BW, BH), TEXT("SquareButton"),
			ArenaSP::Loc(TEXT("XInterface"), TEXT("GUIQuestionPage"), TEXT("NoButtonText"), TEXT("NO")), FString());
		if (bYes || bNo || G->BackPressed())
		{
			const EQuestion Q2 = D.Question;
			CloseDialog();
			OnQuestionAnswer(Q2, bYes);
		}
	}
	else if (G->Button(FGUI::Id(TEXT("SPQOk")), FRect(P.X + P.W * 0.5f - BW * 0.5f, BY, BW, BH), TEXT("SquareButton"),
		ArenaSP::Loc(TEXT("XInterface"), TEXT("GUIQuestionPage"), TEXT("OKButtonText"), TEXT("OK")), FString()) || G->BackPressed())
	{
		CloseDialog();
	}
}

// ---- UT2K4SP_Message ----

void FArenaSinglePlayerPage::DrawMessage(FDialog& D)
{
	// full screen: the message's picture (screenshots[i], else Controls.thinpipe_f), the announcer's voice over (sounds[i])
	// with the subtitles (SPPsbText 0.02, 0.7817, 0.96 x 0.2062, UT2SmallHeaderFont (255,200,0)), OK when it has been said
	const FString Pic = GetStockValue(TEXT("GUI2K4.UT2K4SP_Message"), TEXT("screenshots"), D.Index);
	const FRect S = G->Screen();
	G->Draw().Rect(0.f, 0.f, S.W, S.H, FLinearColor::Black);
	const FRect P = G->Page();
	const FBox2f Sub(FVector2f(0.f, 0.f), FVector2f(1023.f, 767.f)); // SPMimgBackground X1..Y2: the 4:3 part of the picture
	if (Pic.IsEmpty() || !G->Image(Pic, P, EImgStyle::Scaled, FLinearColor::White, &Sub))
	{
		G->Image(TEXT("2K4Menus.Controls.thinpipe_f"), P, EImgStyle::Scaled, FLinearColor::White);
	}
	float Duration = 0.f;
	if (!D.bSoundPlayed)
	{
		D.bSoundPlayed = true;
		const FString SoundPath = GetStockValue(TEXT("GUI2K4.UT2K4SP_Message"), TEXT("sounds"), D.Index);
		if (USoundBase* Snd = SoundPath.IsEmpty() ? nullptr : ArenaRuntimeContent::LoadSoundByObjectPath(SoundPath))
		{
			if (AHUD* HUD = Owner.Get())
			{
				UGameplayStatics::PlaySound2D(HUD, Snd);
			}
			D.Param1 = FString::SanitizeFloat(Snd->GetDuration());
		}
		D.OpenedAt = G->Time();
	}
	Duration = FCString::Atof(*D.Param1);
	const FString Text = Lines(ArenaSP::Loc(TEXT("GUI2K4"), TEXT("UT2K4SP_Message"), *FString::Printf(TEXT("Messages[%d]"), D.Index), FString()));
	const FRect TextR(P.X + P.W * 0.02f, S.H * 0.781667f, P.W * 0.96f, S.H * 0.20625f);
	G->Draw().Rect(S.X, TextR.Y - S.H * 0.01f, S.W, TextR.H + S.H * 0.02f, FLinearColor(0.f, 0.f, 0.f, 0.55f));
	const FFontRef F = G->Font(TEXT("UT2SmallHeaderFont"));
	// GUISubtitleText: the lines appear in turn (messageslength holds the delays of the voice over)
	TArray<FString> Parts;
	Text.ParseIntoArray(Parts, TEXT("\n"), true);
	TArray<float> Delays;
	{
		TArray<FString> Ds;
		ArenaSP::Loc(TEXT("GUI2K4"), TEXT("UT2K4SP_Message"), *FString::Printf(TEXT("messageslength[%d]"), D.Index), FString()).ParseIntoArray(Ds, TEXT("|"), true);
		for (const FString& X : Ds)
		{
			Delays.Add(FCString::Atof(*X) * 1.1f);
		}
	}
	const float Since = float(G->Time() - D.OpenedAt);
	int32 Shown = 0;
	float T = 0.f;
	for (int32 i = 0; i < Parts.Num(); ++i)
	{
		if (Since >= T)
		{
			Shown = i;
		}
		T += Delays.IsValidIndex(i) ? Delays[i] : 99.f;
	}
	if (Parts.IsValidIndex(Shown))
	{
		const TArray<FString> Ln = G->Wrap(F, Parts[Shown], TextR.W);
		float Y = TextR.B() - Ln.Num() * F.Height;
		for (const FString& L : Ln)
		{
			G->TextIn(F, L, FRect(TextR.X, Y, TextR.W, F.Height), C(255, 200, 0, 225), EAlign::Center, true);
			Y += F.Height;
		}
	}
	const bool bDone = Since >= Duration + 1.f;
	if ((bDone && G->Button(FGUI::Id(TEXT("SPMsgOk")), FRect(P.X + P.W * 0.401563f, S.H * 0.94f, P.W * 0.2f, S.H * 0.045f), TEXT("SquareButton"),
		ArenaSP::Loc(TEXT("GUI2K4"), TEXT("UT2K4SP_Message"), TEXT("SPMbtnOk.Caption"), TEXT("OK")), FString(), true, EFontScale::Small)) || G->BackPressed())
	{
		CloseDialog();
	}
}

// ---- UT2K4SP_TeamRoles ----

void FArenaSinglePlayerPage::DrawTeamRoles(FDialog& D)
{
	if (!GP)
	{
		CloseDialog();
		return;
	}
	FProfile& Pr = *GP;
	const TCHAR* Sec = TEXT("UT2K4SP_TeamRoles");
	const FMatchInfo M = Pr.bIsChallenge ? Pr.ChallengeInfo : Pr.GetMatchInfo(SelLadder, SelRung);
	const int32 MaxMates = Pr.GetNumTeammatesForMatch(M);
	const bool bNoBalanceCheck = Pr.bIsChallenge && Pr.bGotChallenged;
	const TArray<FString>& Positions = Get().Defaults.PositionNames;
	const int32 Inactive = Positions.Num();
	Pr.PlayerTeam.SetNum(Pr.MaxTeamSize);
	// initial roles (drawButtons): the lineup's healthy players keep their role, then the first healthy ones auto-assign
	if (D.Roles.Num() != Pr.MaxTeamSize)
	{
		D.Roles.Init(Inactive, Pr.MaxTeamSize);
		int32 Assigned = 0;
		for (int32 i = 0; i < Pr.MaxTeamSize; ++i)
		{
			const int32 B = Pr.PlayerTeam[i].IsEmpty() ? INDEX_NONE : Pr.FindBot(Pr.PlayerTeam[i]);
			if (B == INDEX_NONE || Pr.BotStats[B].Health < 100 || Assigned >= MaxMates)
			{
				continue;
			}
			const int32 L = Pr.PlayerLineup.IndexOfByKey(i);
			if (L != INDEX_NONE && L < MaxMates)
			{
				const int32 Pos = Positions.IndexOfByPredicate([&](const FString& S) { return Pr.PlayerPositions.IsValidIndex(i) && S.Equals(Pr.PlayerPositions[i]); });
				D.Roles[i] = Pos == INDEX_NONE ? 0 : Pos;
				++Assigned;
			}
		}
		for (int32 i = 0; i < Pr.MaxTeamSize && Assigned < MaxMates; ++i)
		{
			const int32 B = Pr.PlayerTeam[i].IsEmpty() ? INDEX_NONE : Pr.FindBot(Pr.PlayerTeam[i]);
			if (B != INDEX_NONE && Pr.BotStats[B].Health >= 100 && D.Roles[i] == Inactive)
			{
				D.Roles[i] = 0;
				++Assigned;
			}
		}
	}
	const FRect Win = DialogFrame(0.f, 0.06f, 1.f, 0.88f, FString());
	G->Label(Win.Sub(0.089063f, 0.041666f, 0.806251f, 0.065f), Caption(Sec, TEXT("SPTlblTitle.Caption"), TEXT("Team roster")), TEXT("TextLabel"), EAlign::Center,
		EFontScale::Large);
	const FString Info = Caption(Sec, TEXT("SPTlblInfo.Caption"), TEXT("%gametype% in %map% with %teammates% team mates"))
		.Replace(TEXT("%teammates%"), *FString::FromInt(MaxMates)).Replace(TEXT("%gametype%"), *GameTypeName(M.GameType)).Replace(TEXT("%map%"), *MatchTitle(M));
	G->Label(Win.Sub(0.032813f, 0.1f, 0.931251f, 0.05875f), Info, TEXT("TextLabel"), EAlign::Center);
	G->Label(Win.Sub(0.556500f, 0.049900f, 0.414900f, 0.039900f), Caption(Sec, TEXT("SPTlblBalance.Caption"), TEXT("Balance: %balance%")).Replace(TEXT("%balance%"), *Money(Pr.Balance)),
		TEXT("TextLabel"), EAlign::Right, EFontScale::Small);
	// the faces (BaseWidth = window / (1 + team size)), a role combo under each
	const int32 N = Pr.MaxTeamSize;
	const float BaseW = 1.f / (1 + N), Spacing = BaseW / (1 + N);
	float X = Spacing;
	const float Y = 0.1f + 0.05875f;
	int32 Assigned = 0;
	int32 Fee = 0;
	TArray<FString> Items = Positions;
	Items.Add(Caption(Sec, TEXT("InactiveMsg"), TEXT("INACTIVE")));
	FString Hovered;
	for (int32 i = 0; i < N; ++i)
	{
		const FString& Name = Pr.PlayerTeam[i];
		const int32 B = Name.IsEmpty() ? INDEX_NONE : Pr.FindBot(Name);
		const int32 Health = B != INDEX_NONE ? Pr.BotStats[B].Health : 100;
		const FRect Face = Win.Sub(X, Y, BaseW, BaseW * 2.7f * Win.W / FMath::Max(1.f, Win.H) * 0.5f);
		G->Image(TEXT("2K4Menus.Controls.sectionback"), Face, EImgStyle::Scaled, FLinearColor::White);
		const bool bActive = D.Roles.IsValidIndex(i) && D.Roles[i] != Inactive;
		if (!Name.IsEmpty())
		{
			const float A = bActive ? 1.f : 0.5f;
			const FLinearColor Tint = Health < 100 ? FLinearColor(1.f, FMath::Min(1.f, Health * 1.5f / 255.f), FMath::Min(1.f, Health * 1.5f / 255.f), A) : FLinearColor(1.f, 1.f, 1.f, A);
			G->Image(PortraitOf(Name), Face.Inset(Face.W * 0.01f, Face.H * 0.02f, Face.W * 0.01f, Face.H * 0.02f), EImgStyle::Scaled, Tint);
			G->Label(FRect(Face.X, Face.Y + Face.H * 0.02f, Face.W, G->L(20.f)), Name, TEXT("TextLabel"), EAlign::Center, EFontScale::Small);
			if (!bNoBalanceCheck && B != INDEX_NONE)
			{
				// "Minimal fee:|%fee%" on two lines at the bottom right of the portrait
				TArray<FString> FeeLines;
				Caption(Sec, TEXT("MinimalFeeCaption"), TEXT("Minimal fee:|%fee%")).Replace(TEXT("%fee%"), *Money(FMath::RoundToInt(Pr.BotStats[B].Price * Pr.LoserFee)))
					.ParseIntoArray(FeeLines, TEXT("\n"), true);
				const float LH = G->L(17.f);
				float LY = Face.B() - LH * FeeLines.Num() - G->L(4.f);
				for (const FString& FL : FeeLines)
				{
					G->Label(FRect(Face.X, LY, Face.W - G->L(6.f), LH), FL, TEXT("TextLabel"), EAlign::Right, EFontScale::Small);
					LY += LH;
				}
			}
			if (Health < 100)
			{
				G->TextIn(G->Font(TEXT("UT2MenuFont")), Caption(Sec, TEXT("InjuredCaption"), TEXT("INJURED")), FRect(Face.X, Face.Y + Face.H / 3.f, Face.W, Face.H * 0.1f),
					FLinearColor::Red, EAlign::Center);
			}
			const uint32 HId = FGUI::Id(TEXT("SPRoleFace"), i);
			if (G->CustomWidget(HId, Face, true, Name) != EMenuState::Blurry)
			{
				Hovered = Name;
			}
		}
		if (bActive)
		{
			++Assigned;
			if (!bNoBalanceCheck && B != INDEX_NONE)
			{
				Fee += Pr.BotStats[B].Price;
			}
		}
		D.Roles.SetNum(N);
		const bool bCanEnable = !Name.IsEmpty() && Health >= 100;
		int32 Role = D.Roles[i];
		const FRect CR(Face.X - Win.W * 0.005f, Face.B() + G->L(4.f), Face.W + Win.W * 0.01f, FMath::RoundToFloat(G->ScreenH() * 0.032f));
		if (G->Combo(FGUI::Id(TEXT("SPRole"), i), CR, FString(), Role, Items, FString(), 0.f, bCanEnable && (bActive || Assigned < MaxMates || D.Roles[i] != Inactive)))
		{
			D.Roles[i] = Role;
		}
		X += BaseW + Spacing;
	}
	if (!Hovered.IsEmpty())
	{
		D.Param2 = Hovered;
	}
	if (const FPlayerRecord* R = D.Param2.IsEmpty() ? nullptr : FindPlayer(D.Param2))
	{
		FString Det = R->DefaultName;
		Det += TEXT("\n") + Caption(Sec, TEXT("AccuracyCaption"), TEXT("Accuracy:")) + FString::Printf(TEXT(" %d%%"), AccuracyRating(*R));
		Det += TEXT("\n") + Caption(Sec, TEXT("AggressionCaption"), TEXT("Aggressiveness:")) + FString::Printf(TEXT(" %d%%"), AggressivenessRating(*R));
		Det += TEXT("\n") + Caption(Sec, TEXT("AgilityCaption"), TEXT("Agility:")) + FString::Printf(TEXT(" %d%%"), AgilityRating(*R));
		Det += TEXT("\n") + Caption(Sec, TEXT("TacticsCaption"), TEXT("Tactics:")) + FString::Printf(TEXT(" %d%%"), TacticsRating(*R));
		Det += TEXT("\n") + FavoriteWeaponText(*R);
		G->ScrollText(FGUI::Id(TEXT("SPRoleInfo")), Win.Sub(0.032422f, 0.726563f, 0.468750f, 0.212500f), Det);
	}
	// OK when exactly the needed players are assigned and their minimal fee can be paid
	bool bOk = Assigned == MaxMates;
	if (!bNoBalanceCheck && float(Fee) * Pr.LoserFee >= float(Pr.Balance))
	{
		bOk = false;
		G->TextIn(G->Font(TEXT("UT2MenuFont")), Caption(Sec, TEXT("SPTlblNoMoney.Caption"), TEXT("You do not have enough money to pay this team's minimal fee")),
			Win.Sub(0.532813f, 0.758333f, 0.431251f, 0.102500f), C(228, 0, 0), EAlign::Center);
	}
	const bool bClickedOk = G->Button(FGUI::Id(TEXT("SPRolesOk")), Win.Sub(0.764064f, 0.912292f, 0.2f, 0.05f), TEXT("SquareButton"),
		Caption(Sec, TEXT("SPMbtnOk.Caption"), TEXT("OK")), FString(), bOk, EFontScale::Small) || (D.Index == 1 && bOk);
	const bool bCancel = G->Button(FGUI::Id(TEXT("SPRolesCancel")), Win.Sub(0.532813f, 0.912292f, 0.2f, 0.05f), TEXT("SquareButton"),
		Caption(Sec, TEXT("SPTbtnCancel.Caption"), TEXT("CANCEL")), FString(), true, EFontScale::Small) || G->BackPressed();
	D.Index = 0;
	if (bClickedOk)
	{
		// the lineup in slot order with the chosen positions (SetLineup / SetPosition)
		Pr.PlayerLineup.Init(-1, 4);
		Pr.PlayerPositions.SetNum(Pr.MaxTeamSize);
		int32 Pos = 0;
		for (int32 i = 0; i < N; ++i)
		{
			if (D.Roles[i] != Inactive)
			{
				if (Pos >= Pr.PlayerLineup.Num())
				{
					Pr.PlayerLineup.Add(-1);
				}
				Pr.PlayerLineup[Pos++] = i;
				Pr.PlayerPositions[i] = Positions.IsValidIndex(D.Roles[i]) ? Positions[D.Roles[i]] : FString();
			}
		}
		CloseDialog();
		StartMatch();
		return;
	}
	if (bCancel)
	{
		if (Pr.bIsChallenge)
		{
			Pr.bIsChallenge = false;
		}
		CloseDialog();
	}
}

// ---- UT2K4SP_Injury ----

void FArenaSinglePlayerPage::DrawInjury(FDialog& D)
{
	if (!GP || !GP->BotStats.IsValidIndex(D.Index) || GP->BotStats[D.Index].Health >= 100)
	{
		CloseDialog();
		return;
	}
	const TCHAR* Sec = TEXT("UT2K4SP_Injury");
	FBotStats& B = GP->BotStats[D.Index];
	const int32 Cost = GP->TreatmentCost(D.Index);
	const FRect Win = DialogFrame(0.15f, 0.15f, 0.7f, 0.7f, FString());
	G->Label(Win.Sub(0.028750f, 0.051667f, 0.661250f, 0.072500f), Caption(Sec, TEXT("SPLlblTitle.Caption"), TEXT("Injury")), TEXT("TextLabel"), EAlign::Center, EFontScale::Large);
	G->Image(TEXT("2K4Menus.Controls.sectionback"), Win.Sub(0.057750f, 0.136460f, 0.177050f, 0.464580f), EImgStyle::Scaled, FLinearColor::White);
	G->Image(PortraitOf(B.Name), Win.Sub(0.059000f, 0.146500f, 0.174500f, 0.451900f), EImgStyle::Scaled, FLinearColor(1.f, B.Health / 100.f, B.Health / 100.f, 1.f));
	const FString Msg = Caption(Sec, TEXT("SPLlblMessage.Caption"), TEXT("%player% got injured in the last match.|%player%'s health is down to %health%%, treatment of the injuries costs %treatment%.|Balance: %balance%."))
		.Replace(TEXT("%player%"), *B.Name).Replace(TEXT("%health%"), *FString::FromInt(B.Health)).Replace(TEXT("%treatment%"), *Money(Cost)).Replace(TEXT("%balance%"), *Money(GP->Balance));
	G->ScrollText(FGUI::Id(TEXT("SPInjuryMsg")), Win.Sub(0.372500f, 0.176600f, 0.373700f, 0.391200f), Msg, TEXT("UT2MenuFont"), FLinearColor::White);
	if (G->Button(FGUI::Id(TEXT("SPInjuryTreat")), Win.Sub(0.664000f, 0.857200f, 0.2f, 0.06f), TEXT("SquareButton"), Caption(Sec, TEXT("SPMbtnTreat.Caption"), TEXT("TREAT NOW")),
		FString(), Cost <= GP->Balance - GP->MinBalance, EFontScale::Small))
	{
		B.Health = 100;
		GP->Balance -= Cost;
		SaveProfile();
		CloseDialog();
		return;
	}
	if (G->Button(FGUI::Id(TEXT("SPInjuryCancel")), Win.Sub(0.051500f, 0.857200f, 0.2f, 0.06f), TEXT("SquareButton"), Caption(Sec, TEXT("SPMbtnCancel.Caption"), TEXT("CANCEL")),
		FString(), true, EFontScale::Small) || G->BackPressed())
	{
		CloseDialog();
	}
}

// ---- UT2K4SP_MapChallenge ----

void FArenaSinglePlayerPage::DrawMapChallenge(FDialog& D)
{
	if (!GP || SelLadder < 0)
	{
		CloseDialog();
		return;
	}
	const TCHAR* Sec = TEXT("UT2K4SP_MapChallenge");
	const FMatchInfo Cur = GP->GetMatchInfo(SelLadder, SelRung);
	const FMatchInfo Alt = GP->GetAltMatchInfo(SelLadder, SelRung);
	const int32 Cost = int32(Cur.PrizeMoney * GP->MapChallengeCost);
	const FRect Win = DialogFrame(0.1f, 0.2f, 0.8f, 0.6f, FString());
	G->Label(Win.Sub(0.021500f, 0.057800f, 0.762500f, 0.052400f), Caption(Sec, TEXT("SPClblTitle.Caption"), TEXT("Change Arena")), TEXT("TextLabel"), EAlign::Center, EFontScale::Large);
	G->Image(TEXT("2K4Menus.Controls.sectionback"), Win.Sub(0.222200f, 0.144600f, 0.444400f, 0.333300f), EImgStyle::Scaled, FLinearColor::White);
	const FString Shot = MapShot(Alt.LevelName);
	const FRect ShotR = Win.Sub(0.224700f, 0.155800f, 0.441600f, 0.321200f);
	if (Shot.IsEmpty() || !G->ImageNormal(Shot, FString(), 0.f, ShotR))
	{
		G->Draw().Rect(ShotR.X, ShotR.Y, ShotR.W, ShotR.H, FLinearColor::Black);
	}
	const FString Desc = Caption(Sec, TEXT("ChallengeDesc"), TEXT("The current arena for this match is %curmap%.|You can change this arena to %altmap% when you pay %fee%."))
		.Replace(TEXT("%altmap%"), *MapTitle(Alt.LevelName)).Replace(TEXT("%curmap%"), *MapTitle(Cur.LevelName)).Replace(TEXT("%fee%"), *Money(Cost));
	G->ScrollText(FGUI::Id(TEXT("SPMapChalDesc")), Win.Sub(0.042800f, 0.6f, 0.731200f, 0.2f), Desc, TEXT("UT2MenuFont"), FLinearColor::White);
	if (G->Button(FGUI::Id(TEXT("SPMapChalOk")), Win.Sub(0.685000f, 0.854300f, 0.222200f, 0.07f), TEXT("SquareButton"), Caption(Sec, TEXT("SPMbtnOk.Caption"), TEXT("CHANGE")),
		FString(), HasMap(Alt.LevelName), EFontScale::Small))
	{
		GP->Balance -= Cost;
		if (GP->GetSelectedLevel(SelLadder, SelRung) != -1)
		{
			GP->ResetSelectedLevel(SelLadder, SelRung);
		}
		else
		{
			GP->SetSelectedLevel(SelLadder, SelRung, GP->GetAltLevel(SelLadder, SelRung));
		}
		SaveProfile();
		CloseDialog();
		return;
	}
	if (G->Button(FGUI::Id(TEXT("SPMapChalCancel")), Win.Sub(0.037800f, 0.854300f, 0.222200f, 0.07f), TEXT("SquareButton"), Caption(Sec, TEXT("SPCbtnCancel.Caption"), TEXT("CANCEL")),
		FString(), true, EFontScale::Small) || G->BackPressed())
	{
		CloseDialog();
	}
}

// ---- UT2K4SP_Details (Last Match Played, Opponent Teams, Other Tournament Matches) ----

void FArenaSinglePlayerPage::DrawDetails(FDialog& D)
{
	if (!GP)
	{
		CloseDialog();
		return;
	}
	FProfile& Pr = *GP;
	const TCHAR* Sec = TEXT("UT2K4SP_Details");
	TArray<FString> DTabs, Hints;
	for (int32 i = 0; i < 3; ++i)
	{
		static const TCHAR* const F[] = { TEXT("Last Match Played"), TEXT("Opponent Teams"), TEXT("Other Tournament Matches") };
		DTabs.Add(LocArray(TEXT("GUI2K4"), Sec, TEXT("PanelCaption"), i, F[i]));
		Hints.Add(LocArray(TEXT("GUI2K4"), Sec, TEXT("PanelHint"), i, FString()));
	}
	const ArenaGUI2K4Pages::FTabbedFrame Frame = ArenaGUI2K4Pages::TabbedPage(*G, FGUI::Id(TEXT("SPDetailTabs")), Caption(Sec, TEXT("PageCaption"), TEXT("Tournament details")),
		DTabs, Hints, D.Tab, D.TabChangedAt);
	const FRect& P = Frame.Panel;
	const FLinearColor White = FLinearColor::White;
	if (D.Tab == 0)
	{
		// UT2K4SPTab_DetailMatch
		const TCHAR* M = TEXT("UT2K4SPTab_DetailMatch");
		G->Label(P.Sub(0.05f, 0.f, 0.9f, 0.0825f), Caption(M, TEXT("MatchTitleCaption"), TEXT("%gametype% in %map%")).Replace(TEXT("%gametype%"), *Pr.LmdGameType)
			.Replace(TEXT("%map%"), *MapTitle(Pr.LmdMap)), TEXT("NoBackground"), EAlign::Center, EFontScale::Large);
		TArray<FListItem> Own, Enemy;
		TArray<FListColumn> Cols = { { 0.37f, EAlign::Left }, { 0.2f, EAlign::Left }, { 0.2f, EAlign::Left }, { 0.23f, EAlign::Left } };
		TArray<FPlayerMatchDetails> Sorted = Pr.PlayerMatchDetails;
		Sorted.StableSort([](const FPlayerMatchDetails& A, const FPlayerMatchDetails& B) { return A.Score > B.Score; });
		TArray<FString> Heads;
		for (int32 i = 0; i < 4; ++i)
		{
			static const TCHAR* const F[] = { TEXT("Name"), TEXT("Score"), TEXT("Kills"), TEXT("Deaths") };
			Heads.Add(ArenaSP::Loc(TEXT("GUI2K4"), M, *FString::Printf(TEXT("ColumnHeadings[%d]"), i), F[i]));
		}
		// GUIMultiColumnListBox: the column headings above the rows
		auto HeadedList = [&](uint32 Id, const FRect& LR, const TArray<FListItem>& Rows)
		{
			const float HH = G->ListRowHeight(TEXT("UT2MenuFont"));
			G->Draw().Rect(LR.X, LR.Y, LR.W, HH, C(24, 24, 74, 200));
			float X = LR.X;
			for (int32 i = 0; i < Heads.Num(); ++i)
			{
				G->TextIn(G->Font(TEXT("UT2MenuFont")), Heads[i], FRect(X + G->L(3.f), LR.Y, LR.W * Cols[i].Width, HH), C(255, 255, 255), EAlign::Left);
				X += LR.W * Cols[i].Width;
			}
			int32 Sel = -1;
			bool bAct = false;
			G->List(Id, FRect(LR.X, LR.Y + HH, LR.W, LR.H - HH), Rows, Sel, bAct, FString(), TEXT("UT2MenuFont"), false, &Cols);
		};
		for (const FPlayerMatchDetails& X : Sorted)
		{
			FListItem It;
			It.Cells = { X.Name, FString::FromInt(int32(X.Score)), FString::FromInt(int32(X.Kills)), FString::FromInt(int32(X.Deaths)) };
			(!Pr.bLmdTeamGame || X.Team == Pr.LmdMyTeam ? Own : Enemy).Add(It);
		}
		if (Pr.bLmdTeamGame)
		{
			G->Section(P.Sub(0.05f, 0.075f, 0.431888f, 0.34375f), Pr.TeamName);
			HeadedList(FGUI::Id(TEXT("SPDetOwn")), P.Sub(0.067857f, 0.133333f, 0.397500f, 0.254000f), Own);
			G->Section(P.Sub(0.533418f, 0.075f, 0.431888f, 0.34375f), TeamTitle(Pr.LmdEnemyTeam));
			HeadedList(FGUI::Id(TEXT("SPDetEnemy")), P.Sub(0.551200f, 0.133333f, 0.397500f, 0.254000f), Enemy);
		}
		else
		{
			G->Section(P.Sub(0.287245f, 0.075f, 0.431888f, 0.34375f), Pr.TeamName);
			HeadedList(FGUI::Id(TEXT("SPDetOwn")), P.Sub(0.305102f, 0.133333f, 0.397500f, 0.254000f), Own);
		}
		// the story of the match (FillData)
		FString T = Caption(M, Pr.bLmdWon ? TEXT("msgWon") : TEXT("msgLost"), Pr.bLmdWon
			? TEXT("After %gametime% minutes you won the %gametype% against %enemies% enemies in %map%.|The prize money for this match was %PrizeMoney%%BonusMoney%. %team_payment%")
			: TEXT("After %gametime% minutes you lost the %gametype% against %enemies% enemies in %map%%BonusMoney%. %team_payment%"));
		T = T.Replace(TEXT("%gametype%"), *Pr.LmdGameType).Replace(TEXT("%map%"), *MapTitle(Pr.LmdMap));
		T = T.Replace(TEXT("%gametime%"), *FString::Printf(TEXT("%.2f"), Pr.LmdGameTime / 60.f)).Replace(TEXT("%PrizeMoney%"), *Money(Pr.LmdPrizeMoney));
		if (Pr.LmdTotalBonusMoney > 0)
		{
			T = T.Replace(TEXT("%BonusMoney%"), *Caption(M, Pr.bLmdWon ? TEXT("msgBonusMoneyWon") : TEXT("msgBonusMoneyLost"), Pr.bLmdWon ? TEXT(" additionally you won %BonusMoney% in bonus money")
				: TEXT(", however you won %BonusMoney% in bonus money")).Replace(TEXT("%BonusMoney%"), *Money(Pr.LmdTotalBonusMoney)));
		}
		else
		{
			T = T.Replace(TEXT("%BonusMoney%"), TEXT(""));
		}
		T = T.Replace(TEXT("%enemies%"), *FString::FromInt(Pr.bLmdTeamGame ? Enemy.Num() : FMath::Max(0, Own.Num() - 1)));
		T = T.Replace(TEXT("%team_payment%"), Pr.PayCheck.Num() > 0 ? *Caption(M, TEXT("msgTeamPayment"), TEXT("After paying your team mates %balance_change%.")) : TEXT(""));
		const FString BC = Caption(M, Pr.LmdBalanceChange > 0 ? TEXT("msgBalanceChangeUp") : TEXT("msgBalanceChangeDown"),
			Pr.LmdBalanceChange > 0 ? TEXT("your balance increased by %balance_absolute%") : TEXT("your balance decreased by %balance_absolute%"))
			.Replace(TEXT("%balance_absolute%"), *Money(FMath::Abs(Pr.LmdBalanceChange)));
		T = T.Replace(TEXT("%balance_change%"), *BC);
		if (Pr.LmdInjury > -1 && Pr.BotStats.IsValidIndex(Pr.LmdInjury))
		{
			T += TEXT("\n\n") + Caption(M, TEXT("msgInjury"), TEXT("During the last match %player% got injured, health dropped to %health%%. Treatment of the injuries costs %treatment%"))
				.Replace(TEXT("%player%"), *Pr.BotStats[Pr.LmdInjury].Name).Replace(TEXT("%health%"), *FString::FromInt(Pr.LmdInjuryHealth))
				.Replace(TEXT("%treatment%"), *Money(Pr.LmdInjuryTreatment));
		}
		for (const FPlayerMatchDetails& X : Pr.PlayerMatchDetails)
		{
			if (X.SpecialAwards.Num() > 0)
			{
				T += TEXT("\n") + Caption(M, TEXT("msgSpecialAwards"), TEXT("%player% earned the following special awards:|        %awards%")).Replace(TEXT("%player%"), *X.Name)
					.Replace(TEXT("%awards%"), *FString::Join(X.SpecialAwards, TEXT(", ")));
			}
		}
		if (Pr.PayCheck.Num() > 0)
		{
			T += TEXT("\n\n") + Caption(M, TEXT("msgPayCheck"), TEXT("Pay check overview:"));
			for (const FPayCheck& Pc : Pr.PayCheck)
			{
				T += TEXT("\n") + Caption(M, TEXT("msgEarns"), TEXT("    %player% earned %payment%."))
					.Replace(TEXT("%player%"), Pr.BotStats.IsValidIndex(Pc.BotId) ? *Pr.BotStats[Pc.BotId].Name : TEXT("?")).Replace(TEXT("%payment%"), *Money(Pc.Payment));
			}
		}
		FString Bonus;
		const FProfileDefaults& Def = Get().Defaults;
		for (int32 i = 0; i < 6; ++i)
		{
			if (Pr.LmdSpree[i] > 0)
			{
				Bonus += TEXT("\n") + Caption(M, TEXT("msgBonusOverviewItem"), TEXT("    %bonuscount% x %bonusname% %bonusmoney%"))
					.Replace(TEXT("%bonusname%"), *ArenaSP::Loc(TEXT("GUI2K4"), TEXT("UT2K4SPTab_Profile"), *FString::Printf(TEXT("SpreeLabel[%d]"), i), FString()))
					.Replace(TEXT("%bonuscount%"), *FString::FromInt(Pr.LmdSpree[i])).Replace(TEXT("%bonusmoney%"), *Money(Def.SpreeBonus[i]));
			}
		}
		for (int32 i = 0; i < 7; ++i)
		{
			if (Pr.LmdMultiKills[i] > 0)
			{
				Bonus += TEXT("\n") + Caption(M, TEXT("msgBonusOverviewItem"), TEXT("    %bonuscount% x %bonusname% %bonusmoney%"))
					.Replace(TEXT("%bonusname%"), *ArenaSP::Loc(TEXT("GUI2K4"), TEXT("UT2K4SPTab_Profile"), *FString::Printf(TEXT("MultiKillsLabel[%d]"), i), FString()))
					.Replace(TEXT("%bonuscount%"), *FString::FromInt(Pr.LmdMultiKills[i])).Replace(TEXT("%bonusmoney%"), *Money(Def.MultiKillBonus[i]));
			}
		}
		if (!Bonus.IsEmpty())
		{
			T += TEXT("\n\n") + Caption(M, TEXT("msgBonusOverview"), TEXT("You won the following bonuses:")) + Bonus;
		}
		G->Section(P.Sub(0.05f, 0.566666f, 0.899999f, 0.425f), Caption(M, TEXT("SPDMimgDetailsBg.Caption"), TEXT("Details")));
		G->ScrollText(FGUI::Id(TEXT("SPDetText")), P.Sub(0.072800f, 0.636800f, 0.852900f, 0.330200f), T, TEXT("UT2MenuFont"), White);
		// challenges after a team game (SPLcbChallenges)
		G->Section(P.Sub(0.281200f, 0.422600f, 0.443700f, 0.140300f), Caption(M, TEXT("SPLsbgChallengeBg.Caption"), TEXT("Challenges")), true);
		TArray<FString> Items = { Caption(M, TEXT("msgSelectChal"), TEXT("Select a challenge")) };
		TArray<FString> Classes = { FString() };
		for (const FChallengeInfo& Ch : Get().Challenges)
		{
			if (!Ch.Class.EndsWith(TEXT("BloodRites")) || Pr.CompletedLadder(LID_TDM))
			{
				Items.Add(Ch.Name);
				Classes.Add(Ch.Class);
			}
		}
		int32 CSel = 0;
		const FRect CR = P.Sub(0.294900f, 0.478200f, 0.417300f, 0.0486f);
		if (G->Combo(FGUI::Id(TEXT("SPDetChal")), FRect(CR.X, CR.Y, CR.W, FMath::RoundToFloat(G->ScreenH() * 0.032f)), FString(), CSel, Items,
			Caption(M, TEXT("SPLcbChallenges.Hint"), TEXT("Challenge another team")), 0.f, Pr.bLmdTeamGame) && CSel > 0)
		{
			FDialog Ch;
			Ch.Kind = EDialog::Challenge;
			Ch.ChallengeClass = Classes[CSel];
			Ch.Param1 = Pr.LmdEnemyTeam;
			PushDialog(Ch);
			return;
		}
	}
	else if (D.Tab == 1)
	{
		// UT2K4SPTab_DetailEnemies: the team list and the selected team's symbol and description
		const TCHAR* M = TEXT("UT2K4SPTab_DetailEnemies");
		TArray<int32> Ids;
		for (int32 i = 0; i < Pr.TeamStats.Num(); ++i)
		{
			if (Pr.TeamStats[i].Matches > 0 || Pr.TeamStats[i].Level >= 0)
			{
				const FTeamRoster* T = FindTeam(Pr.TeamStats[i].Name);
				if (T && !T->bPhantom)
				{
					Ids.Add(i);
				}
			}
		}
		TArray<FListItem> Items;
		TArray<FListColumn> Cols = { { 0.3f, EAlign::Left }, { 0.15f, EAlign::Left }, { 0.15f, EAlign::Left }, { 0.15f, EAlign::Left }, { 0.25f, EAlign::Left } };
		for (int32 Id : Ids)
		{
			const FTeamStats& T = Pr.TeamStats[Id];
			FListItem It;
			It.Cells = { TeamTitle(T.Name), FString::FromInt(T.Matches), FString::FromInt(T.Won), FString::Printf(TEXT("%.2f"), T.Rating), Strength(T.Level) };
			Items.Add(It);
		}
		D.Index = FMath::Clamp(D.Index, 0, FMath::Max(0, Items.Num() - 1));
		G->Section(P.Sub(0.082700f, 0.036400f, 0.820700f, 0.551900f), FString(), true);
		G->Section(P.Sub(0.079900f, 0.616500f, 0.827400f, 0.356200f), Caption(M, TEXT("SPPsbgFoesBG.Caption"), TEXT("Enemy Teams")));
		bool bAct = false;
		const FRect LR = P.Sub(0.097800f, 0.675400f, 0.791800f, 0.267800f);
		const float HH = G->ListRowHeight(TEXT("UT2MenuFont"));
		G->Draw().Rect(LR.X, LR.Y, LR.W, HH, C(24, 24, 74, 200));
		float HX = LR.X;
		for (int32 i = 0; i < Cols.Num(); ++i)
		{
			static const TCHAR* const F[] = { TEXT("Name"), TEXT("Matches"), TEXT("They won"), TEXT("Rating"), TEXT("Strength") };
			G->TextIn(G->Font(TEXT("UT2MenuFont")), ArenaSP::Loc(TEXT("GUI2K4"), M, *FString::Printf(TEXT("ColumnHeadings[%d]"), i), F[i]),
				FRect(HX + G->L(3.f), LR.Y, LR.W * Cols[i].Width, HH), FLinearColor::White, EAlign::Left);
			HX += LR.W * Cols[i].Width;
		}
		G->List(FGUI::Id(TEXT("SPDetFoes")), FRect(LR.X, LR.Y + HH, LR.W, LR.H - HH), Items, D.Index, bAct, FString(), TEXT("UT2MenuFont"), false, &Cols);
		if (Ids.IsValidIndex(D.Index))
		{
			const FTeamRoster* T = FindTeam(Pr.TeamStats[Ids[D.Index]].Name);
			if (T)
			{
				G->Image(SymbolTexture(T->TeamSymbolName), P.Sub(0.101200f, 0.105100f, 0.318800f, 0.417000f), EImgStyle::Justified, White);
				FString Desc = T->TeamName + TEXT("\n\n") + Lines(T->TeamDescription) + TEXT("\n\n") + Caption(M, TEXT("msgMembers"), TEXT("Members:")) + TEXT(" ")
					+ FString::Join(Pr.GetTeamRoster(T->Class), TEXT(", "));
				G->ScrollText(FGUI::Id(TEXT("SPDetFoeDesc")), P.Sub(0.431100f, 0.108900f, 0.456200f, 0.412400f), Desc, TEXT("UT2MenuFont"), White);
			}
		}
	}
	else
	{
		// UT2K4SPTab_DetailPhantom: the other teams' matches played meanwhile
		const TCHAR* M = TEXT("UT2K4SPTab_DetailPhantom");
		G->Section(P.Sub(0.1f, 0.1f, 0.8f, 0.8f), Caption(M, TEXT("SPDPimgMatchesBg.Caption"), TEXT("Other Tournament Matches Played During Your Match")));
		FString T;
		for (const FPhantomMatch& Ph : Pr.PhantomMatches)
		{
			const FString T1 = Pr.TeamStats.IsValidIndex(Ph.Team1) ? TeamTitle(Pr.TeamStats[Ph.Team1].Name) : FString();
			const FString T2 = Pr.TeamStats.IsValidIndex(Ph.Team2) ? TeamTitle(Pr.TeamStats[Ph.Team2].Name) : FString();
			const FMatchInfo MI = Pr.GetMatchInfo(Ph.LadderId, Ph.MatchId);
			T += T1 + TEXT(" ") + Caption(M, TEXT("VS"), TEXT("vs")) + TEXT(" ") + T2 + TEXT("\n");
			T += TEXT("    ") + Caption(M, TEXT("msgMatchInfo"), TEXT("%gametype% in %map%")).Replace(TEXT("%gametype%"), *GameTypeName(MI.GameType)).Replace(TEXT("%map%"), *MapTitle(MI.LevelName)) + TEXT("\n");
			T += TEXT("    ") + Caption(M, TEXT("msgGameTime"), TEXT("Game time: %gametime% minutes")).Replace(TEXT("%gametime%"), *FString::Printf(TEXT("%.2f"), Ph.GameTime / 60.f)) + TEXT("\n");
			T += TEXT("    ") + Caption(M, TEXT("msgScore"), TEXT("Score: %team1% - %team2%")).Replace(TEXT("%team1%"), *FString::FromInt(int32(Ph.ScoreTeam1)))
				.Replace(TEXT("%team2%"), *FString::FromInt(int32(Ph.ScoreTeam2))) + TEXT("\n\n");
		}
		G->ScrollText(FGUI::Id(TEXT("SPDetPhantom")), P.Sub(0.117800f, 0.179600f, 0.764300f, 0.686000f), T, TEXT("UT2MenuFont"), White);
	}
	// footer: "Show after a match" check box, CLOSE
	bool bShow = Pr.bShowDetails;
	const FRect& F = Frame.Footer;
	if (G->CheckBox(FGUI::Id(TEXT("SPDetShow")), FRect(G->Page().X + G->Page().W * 0.00625f, F.Y + F.H * 0.15f, G->Page().W * 0.289063f, F.H * 0.7f),
		Caption(Sec, TEXT("SPDcbAlwaysShow.Caption"), TEXT("Show after a match")), bShow, FString(), 0.8f))
	{
		Pr.bShowDetails = bShow;
		SaveProfile();
	}
	const int32 Clicked = ArenaGUI2K4Pages::FooterButtons(*G, F, { Caption(Sec, TEXT("SPDbtnClose.Caption"), TEXT("CLOSE")) },
		{ Caption(Sec, TEXT("SPDbtnClose.Hint"), TEXT("Return to the ladder")) }, {});
	if (Clicked == 0 || G->BackPressed())
	{
		Pr.bLmdFresh = false;
		SaveProfile();
		CloseDialog();
	}
}

void FArenaSinglePlayerPage::DrawTeamDetails(FDialog& D)
{
	// UT2K4SP_DetailsTeam
	const FTeamRoster* T = FindTeam(D.Param1);
	const FRect Win = DialogFrame(0.2f, 0.2f, 0.6f, 0.6f, FString());
	if (T)
	{
		G->Label(Win.Sub(0.027400f, 0.068300f, 0.567400f, 0.053700f), T->TeamName, TEXT("TextLabel"), EAlign::Center, EFontScale::Large);
		G->Image(SymbolTexture(T->TeamSymbolName), Win.Sub(0.031200f, 0.162400f, 0.224900f, 0.412400f), EImgStyle::Justified, FLinearColor::White);
		G->ScrollText(FGUI::Id(TEXT("SPTeamDesc")), Win.Sub(0.425000f, 0.166600f, 0.524900f, 0.6f), Lines(T->TeamDescription) + TEXT("\n\n")
			+ FString::Join(GP ? GP->GetTeamRoster(T->Class) : T->RosterNames, TEXT(", ")), TEXT("UT2MenuFont"), FLinearColor::White);
	}
	if (G->Button(FGUI::Id(TEXT("SPTeamDetOk")), Win.Sub(0.337800f, 0.865600f, 0.3f, 0.07f), TEXT("SquareButton"), Caption(TEXT("UT2K4SP_DetailsTeam"), TEXT("SPDTbtnOk.Caption"), TEXT("CLOSE")),
		FString(), true, EFontScale::Small) || G->BackPressed())
	{
		CloseDialog();
	}
}

// ---- challenges: UT2K4SP_CGManoEMano (Head to Head) and UT2K4SP_CGBloodRites ----

void FArenaSinglePlayerPage::DrawChallenge(FDialog& D)
{
	if (!GP)
	{
		CloseDialog();
		return;
	}
	FProfile& Pr = *GP;
	const bool bBlood = D.ChallengeClass.EndsWith(TEXT("BloodRites"));
	const FString Cls = bBlood ? TEXT("GUI2K4.UT2K4SP_CGBloodRites") : TEXT("GUI2K4.UT2K4SP_CGManoEMano");
	const TCHAR* Sec = bBlood ? TEXT("UT2K4SP_CGBloodRites") : TEXT("UT2K4SP_CGManoEMano");
	const FChallengeInfo* Info = Get().Challenges.FindByPredicate([&D](const FChallengeInfo& I) { return I.Class.Equals(D.ChallengeClass, ESearchCase::IgnoreCase); });
	if (!Info)
	{
		CloseDialog();
		return;
	}
	// set up once (HandleParameters): the teams you can challenge, two arenas, bets / penalties
	if (D.Teams.Num() == 0 && D.Maps.Num() == 0)
	{
		D.bChallenged = D.bChallenged || !D.Param2.IsEmpty();
		const int32 Healthy = Pr.NumHealthyTeammates();
		for (const FTeamStats& T : Pr.TeamStats)
		{
			const FTeamRoster* R = FindTeam(T.Name);
			if (T.Matches <= 0 || !R || R->bPhantom)
			{
				continue;
			}
			if (!bBlood && T.Level >= 3)
			{
				continue; // never a strong team head to head
			}
			if (bBlood && !D.bChallenged && Pr.GetTeamRoster(T.Name).Num() <= 5)
			{
				continue;
			}
			D.Teams.Add(T.Name);
		}
		if (!D.Param1.IsEmpty() && !D.Teams.Contains(D.Param1))
		{
			D.Teams.Insert(D.Param1, 0);
		}
		D.TeamIndex = FMath::Max(0, D.Teams.IndexOfByKey(D.Param1));
		// arenas: deathmatch maps of the stock content (bloodrites: big maps, team game)
		TArray<FString> Pool;
		if (bBlood)
		{
			for (const FArenaLayoutInfo& L : AArenaLevelBuilder::GetLayouts())
			{
				if (!L.bCustom && L.MapName.StartsWith(TEXT("DM-")) && !L.MapName.StartsWith(TEXT("DM-1on1")) && L.RecommendedPlayers >= 7)
				{
					Pool.Add(L.MapName);
				}
			}
		}
		else
		{
			for (const FString& M : GetStockArray(Cls, TEXT("ChalMaps")))
			{
				if (HasMap(M))
				{
					Pool.Add(M);
				}
			}
			if (Pool.Num() == 0)
			{
				for (const FArenaLayoutInfo& L : AArenaLevelBuilder::GetLayouts())
				{
					if (!L.bCustom && L.MapName.StartsWith(TEXT("DM-1on1")))
					{
						Pool.Add(L.MapName);
					}
				}
			}
		}
		while (D.Maps.Num() < 2 && Pool.Num() > 0)
		{
			const int32 I = FMath::RandRange(0, Pool.Num() - 1);
			D.Maps.Add(Pool[I]);
			Pool.RemoveAt(I);
		}
		const int32 MinimalBet = FMath::Max(1, FCString::Atoi(*GetStockValue(Cls, TEXT("MinimalBet"))));
		if (bBlood)
		{
			Pr.ChallengeInfo = FMatchInfo();
			Pr.ChallengeInfo.NumBots = FMath::Min(Healthy * 2 + 1, 7);
			if (D.bChallenged)
			{
				// they want one of your team mates
				TArray<FString> Mates;
				for (const FString& N : Pr.PlayerTeam)
				{
					if (!N.IsEmpty())
					{
						Mates.Add(N);
					}
				}
				D.Param2 = Mates.Num() > 0 ? Mates[FMath::RandRange(0, Mates.Num() - 1)] : FString();
				const float Mult = FMath::Max(0.1f, FCString::Atof(*GetStockValue(Cls, TEXT("ChalledFeeMultiply"))));
				const int32 B = Pr.FindBot(D.Param2);
				D.CancelPenalty = B != INDEX_NONE ? int32(Pr.BotStats[B].Price * Mult) : 0;
				D.bInstagib = FMath::RandBool();
			}
		}
		else
		{
			D.MinBet = MinimalBet;
			if (D.bChallenged)
			{
				D.MinBet = MinimalBet + FMath::RandRange(0, MinimalBet - 1);
				D.CancelPenalty = int32(D.MinBet * 1.5f);
			}
			D.Bet = D.MinBet;
		}
		if (Pr.Balance - Pr.MinBalance < FMath::Max(D.MinBet, D.CancelPenalty) || D.Teams.Num() == 0 || D.Maps.Num() == 0)
		{
			CloseDialog();
			PushQuestion(Caption(TEXT("UT2K4SP_CGManoEMano"), TEXT("NotEnoughCash"), TEXT("You do not have enough money to pay the minimal bet for this challenge.")));
			return;
		}
	}
	const FString EnemyTeam = D.Teams.IsValidIndex(D.TeamIndex) ? D.Teams[D.TeamIndex] : FString();
	const FTeamRoster* ET = FindTeam(EnemyTeam);
	const FRect Win = DialogFrame(0.05f, bBlood ? 0.05f : 0.1f, 0.9f, bBlood ? 0.9f : 0.8f, FString());
	G->Label(Win.Sub(0.067800f, 0.026100f, 0.862500f, 0.077500f), Caption(Sec, TEXT("SPClblTitle.Caption"), *Info->Name), TEXT("TextLabel"), EAlign::Center, EFontScale::Large);
	G->Label(Win.Sub(0.532300f, 0.044f, 0.449900f, 0.041200f), Caption(TEXT("UT2K4SPTab_LadderBase"), TEXT("BalanceLabel"), TEXT("Balance:")) + TEXT(" ") + Money(Pr.Balance),
		TEXT("TextLabel"), EAlign::Right, EFontScale::Small);
	// the two arenas (SPCimgMap1/2, SPCcbMap1/2)
	for (int32 i = 0; i < 2; ++i)
	{
		const float L = i == 0 ? 0.020800f : 0.534700f;
		const FRect Bg = Win.Sub(L, 0.111300f, 0.444400f, 0.333300f);
		G->Image(TEXT("2K4Menus.Controls.sectionback"), Bg, EImgStyle::Scaled, FLinearColor::White);
		const FString Map = D.Maps.IsValidIndex(i) ? D.Maps[i] : FString();
		const FRect Shot = Bg.Inset(Bg.W * 0.006f, Bg.H * 0.03f, Bg.W * 0.006f, Bg.H * 0.03f);
		const FString Pic = MapShot(Map);
		if (Pic.IsEmpty() || !G->ImageNormal(Pic, FString(), 0.f, Shot))
		{
			G->TextIn(G->Font(TEXT("UT2HeaderFont")), Caption(Sec, TEXT("NoPreview.Caption"), TEXT("No Preview Available")), Shot, C(247, 255, 0), EAlign::Center);
		}
		const FRect Btn = Win.Sub(L, 0.452900f, 0.444400f, 0.0625f);
		if (G->Button(FGUI::Id(TEXT("SPChalMap"), i), Btn, D.MapSel == i ? TEXT("TabButton") : TEXT("SquareButton"), MapTitle(Map), FString(), !Map.IsEmpty(), EFontScale::Small))
		{
			D.MapSel = i;
		}
		if (D.MapSel == i)
		{
			G->Draw().Outline(Bg.X, Bg.Y, Bg.W, Bg.H, C(255, 200, 0), FMath::Max(1.f, G->L(2.f)));
		}
	}
	// the enemy team (locked when they challenged you)
	TArray<FString> TeamItems;
	for (const FString& T : D.Teams)
	{
		const FTeamRoster* R = FindTeam(T);
		TeamItems.Add((R ? R->TeamName : T) + TEXT(" (") + Strength(R ? R->TeamLevel : 0) + TEXT(")"));
	}
	const float RowH = FMath::RoundToFloat(G->ScreenH() * 0.032f);
	const FRect TeamR = bBlood ? Win.Sub(0.280500f, 0.528100f, 0.696500f, 0.f) : Win.Sub(0.024300f, 0.536400f, 0.952700f, 0.f);
	G->Combo(FGUI::Id(TEXT("SPChalTeam")), FRect(TeamR.X, TeamR.Y, TeamR.W, RowH), Caption(Sec, TEXT("SPCcbEnemyTeam.Caption"), TEXT("Challenge team:")), D.TeamIndex, TeamItems,
		Caption(Sec, TEXT("SPCcbEnemyTeam.Hint"), TEXT("Challenge this team for a match")), 0.35f, !D.bChallenged);
	FString Desc;
	FMatchInfo& CI = Pr.ChallengeInfo;
	const FString MapName = D.Maps.IsValidIndex(D.MapSel) ? D.Maps[D.MapSel] : FString();
	if (bBlood)
	{
		// the team mate you challenge for (bloodrites trades): one of theirs, or one of yours when they challenged you
		TArray<FString> Chars = D.bChallenged ? TArray<FString>({ D.Param2 }) : Pr.GetTeamRoster(EnemyTeam);
		D.CharIndex = Chars.Num() > 0 ? (D.CharIndex % Chars.Num() + Chars.Num()) % Chars.Num() : 0;
		const FString Char = Chars.IsValidIndex(D.CharIndex) ? Chars[D.CharIndex] : FString();
		G->Label(Win.Sub(0.047200f, 0.530800f, 0.197900f, 0.043700f), Caption(Sec, TEXT("CGBlblSelChar.Caption"), TEXT("Team Mate")), TEXT("TextLabel"), EAlign::Center);
		G->Image(PortraitOf(Char), Win.Sub(0.088400f, 0.576200f, 0.118400f, 0.268700f), EImgStyle::Scaled, FLinearColor::White);
		G->Label(Win.Sub(0.037200f, 0.847500f, 0.222900f, 0.043700f), Char, TEXT("TextLabel"), EAlign::Center);
		if (G->Button(FGUI::Id(TEXT("SPChalPrev")), Win.Sub(0.042500f, 0.673300f, 0.048700f, 0.073700f), TEXT("ArrowLeft"), FString(),
			Caption(Sec, TEXT("CGBbtnPrevChar.Hint"), TEXT("Select the team mate you want")), !D.bChallenged))
		{
			--D.CharIndex;
		}
		if (G->Button(FGUI::Id(TEXT("SPChalNext")), Win.Sub(0.203400f, 0.673300f, 0.048700f, 0.073700f), TEXT("ArrowRight"), FString(),
			Caption(Sec, TEXT("CGBbtnNextChar.Hint"), TEXT("Select the team mate you want")), !D.bChallenged))
		{
			++D.CharIndex;
		}
		const FRect IR = Win.Sub(0.280500f, 0.586400f, 0.696500f, 0.f);
		bool bInsta = D.bInstagib;
		if (G->CheckBox(FGUI::Id(TEXT("SPChalInsta")), FRect(IR.X, IR.Y, IR.W, RowH), Caption(Sec, TEXT("SPCcbInstaGib.Caption"), TEXT("InstaGib Game")), bInsta,
			Caption(Sec, TEXT("SPCcbInstaGib.Hint"), TEXT("In this an InstaGib Match")), 0.8f, !D.bChallenged))
		{
			D.bInstagib = bInsta;
		}
		const FString GameType = GetStockValue(Cls, TEXT("ChalGameType")).IsEmpty() ? FString(TEXT("xGame.xTeamGame")) : GetStockValue(Cls, TEXT("ChalGameType"));
		CI.LevelName = MapName;
		CI.GameType = GameType;
		CI.DifficultyModifier = (ET ? ET->TeamLevel : 0) / 1.2f;
		CI.PrizeMoney = 0;
		CI.GoalScore = float(FCString::Atoi(*GetStockValue(Cls, D.bInstagib ? TEXT("InstaGoalScore") : TEXT("NormalGoalScore"))));
		const int32 B = Char.IsEmpty() ? INDEX_NONE : Pr.GetBotPosition(Char, true);
		CI.EntryFee = D.bChallenged || B == INDEX_NONE ? 0 : int32(Pr.BotStats[B].Price * Info->FeeMultiply);
		CI.URLString = GetStockValue(Cls, TEXT("DefaultUrl")) + (D.bInstagib ? FString(TEXT("?Instagib=1")) : FString());
		CI.SpecialEvent = FString::Printf(TEXT("%s %s %s"), D.bChallenged ? TEXT("UNTRADE") : TEXT("TRADE"), *EnemyTeam, *Char);
		D.Param2 = D.bChallenged ? D.Param2 : FString();
		Desc = Caption(Sec, D.bChallenged ? TEXT("msgChallenged") : TEXT("msgChallenge"), TEXT(""))
			.Replace(TEXT("%map%"), *MapTitle(MapName)).Replace(TEXT("%enemyteam%"), ET ? *ET->TeamName : *EnemyTeam).Replace(TEXT("%selectedchar%"), *Char)
			.Replace(TEXT("%entryfee%"), *Money(CI.EntryFee)).Replace(TEXT("%gametype%"), *((D.bInstagib ? Caption(Sec, TEXT("msgInstaGib"), TEXT("InstaGib")) + TEXT(" ") : FString()) + GameTypeName(GameType)))
			.Replace(TEXT("%penalty%"), *Money(D.CancelPenalty));
		D.Text = Char;
	}
	else
	{
		// Head to Head: a 1 on 1 against the team leader, the bet decides the prize
		D.MaxBet = FMath::Min(Pr.Balance, D.MinBet * ((ET ? ET->TeamLevel : 0) + 1) * 2);
		const int32 Step = FMath::Max(1, (D.MaxBet - D.MinBet) / 20);
		const FRect BR = Win.Sub(0.024300f, 0.590600f, 0.952700f, 0.f);
		const int32 Dir = G->Spinner(FGUI::Id(TEXT("SPChalBet")), FRect(BR.X, BR.Y, BR.W * 0.6f, RowH), Caption(Sec, TEXT("SPCneBet.Caption"), TEXT("Challenge bet:")), Money(D.Bet),
			Caption(Sec, TEXT("SPCneBet.Hint"), TEXT("The bet for this challenge")), 0.5f, !D.bChallenged || true);
		D.Bet = FMath::Clamp(D.Bet + Dir * Step, D.MinBet, FMath::Max(D.MinBet, D.MaxBet));
		G->Label(FRect(BR.X + BR.W * 0.62f, BR.Y, BR.W * 0.38f, RowH), Caption(Sec, TEXT("msgMinMaxBet"), TEXT("minimum: %min%, maximum: %max%")).Replace(TEXT("%min%"), *Money(D.MinBet))
			.Replace(TEXT("%max%"), *Money(D.MaxBet)), TEXT("TextLabel"), EAlign::Left, EFontScale::Small);
		// per map type settings (ChallengeSettings: Acronym, GoalScore, GameType)
		FString GameType = TEXT("xGame.xDeathMatch");
		int32 Goal = 10;
		const FString Prefix = MapName.Left(MapName.Find(TEXT("-")));
		for (const FString& S : GetStockArray(Cls, TEXT("ChallengeSettings")))
		{
			if (StructField(S, TEXT("Acronym")).Equals(Prefix, ESearchCase::IgnoreCase))
			{
				GameType = StructField(S, TEXT("GameType"));
				Goal = FCString::Atoi(*StructField(S, TEXT("GoalScore")));
			}
		}
		CI.LevelName = MapName;
		CI.NumBots = 1;
		CI.DifficultyModifier = (ET ? ET->TeamLevel : 0) / 1.2f + (MapName.Equals(TEXT("CTF-1on1-Joust"), ESearchCase::IgnoreCase) ? 2.f : 0.f);
		CI.PrizeMoney = D.Bet * 2;
		CI.GameType = GameType;
		CI.GoalScore = float(Goal);
		CI.EntryFee = D.Bet;
		CI.URLString = GetStockValue(Cls, TEXT("DefaultUrl"));
		CI.SpecialEvent.Reset();
		const FString Leader = ET ? (!ET->TeamLeader.IsEmpty() ? ET->TeamLeader : (ET->RosterNames.Num() > 0 ? ET->RosterNames[0] : FString())) : FString();
		Desc = Caption(Sec, D.bChallenged ? TEXT("msgChallenged") : TEXT("msgChallenge"), TEXT(""))
			.Replace(TEXT("%map%"), *MapTitle(MapName)).Replace(TEXT("%teamname%"), *(TeamItems.IsValidIndex(D.TeamIndex) ? TeamItems[D.TeamIndex] : FString()))
			.Replace(TEXT("%teamleader%"), *Leader).Replace(TEXT("%prizemoney%"), *Money(CI.PrizeMoney)).Replace(TEXT("%entryfee%"), *Money(CI.EntryFee))
			.Replace(TEXT("%gametype%"), *GameTypeName(GameType)).Replace(TEXT("%penalty%"), *Money(D.CancelPenalty));
	}
	if (MapName.IsEmpty())
	{
		Desc = Caption(Sec, TEXT("SelectAMap"), TEXT("Please select an area for the challenge."));
	}
	const bool bCanPay = D.bChallenged || Pr.Balance > Pr.GetMinimalEntryFeeFor(CI) + Pr.MinBalance;
	if (!bCanPay && !MapName.IsEmpty())
	{
		Desc += TEXT("\n") + Caption(TEXT("UT2K4SP_CGManoEMano"), TEXT("NotEnoughCash"), TEXT("You do not have enough money to pay the minimal bet for this challenge."));
	}
	if (GameTypeToMode(CI.GameType).IsEmpty() && !MapName.IsEmpty())
	{
		Desc += TEXT("\n(") + GameTypeName(CI.GameType) + TEXT(" is not supported yet.)");
	}
	G->ScrollText(FGUI::Id(TEXT("SPChalDesc")), bBlood ? Win.Sub(0.282200f, 0.657900f, 0.689500f, 0.243600f) : Win.Sub(0.019700f, 0.674600f, 0.958300f, 0.217300f),
		Desc, TEXT("UT2MenuFont"), FLinearColor::White);
	const bool bOk = G->Button(FGUI::Id(TEXT("SPChalOk")), bBlood ? Win.Sub(0.741300f, 0.912700f, 0.222200f, 0.05f) : Win.Sub(0.710000f, 0.904300f, 0.222200f, 0.05f), TEXT("SquareButton"),
		Caption(Sec, TEXT("SPMbtnOk.Caption"), TEXT("ACCEPT")), FString(), !MapName.IsEmpty() && bCanPay && !GameTypeToMode(CI.GameType).IsEmpty() && HasMap(MapName), EFontScale::Small);
	const bool bCancel = G->Button(FGUI::Id(TEXT("SPChalCancel")), bBlood ? Win.Sub(0.037800f, 0.912700f, 0.222200f, 0.05f) : Win.Sub(0.050300f, 0.904300f, 0.222200f, 0.05f), TEXT("SquareButton"),
		Caption(Sec, TEXT("SPCbtnCancel.Caption"), TEXT("REFUSE")), FString(), true, EFontScale::Small) || G->BackPressed();
	if (bOk)
	{
		// StartChallenge: the entry fee is paid now, the match is a challenge (CurrentLadder -1)
		Pr.EnemyTeam = EnemyTeam;
		CI.EnemyTeamName = EnemyTeam;
		Pr.bGotChallenged = D.bChallenged;
		Pr.ChallengeClass = D.ChallengeClass;
		Pr.ChallengeVariable = bBlood ? D.Text : Money(D.Bet);
		Pr.SpecialEvent = bBlood && D.bChallenged ? CI.SpecialEvent : FString();
		Pr.bIsChallenge = true;
		Pr.Balance -= CI.EntryFee;
		CloseDialog();
		const int32 Mates = Pr.GetNumTeammatesForMatch(CI);
		if (Mates > 0)
		{
			if (Pr.NumHealthyTeammates() < Mates)
			{
				Pr.Balance += CI.EntryFee;
				Pr.bIsChallenge = false;
				PushQuestion(Caption(TEXT("UT2K4SPTab_LadderBase"), TEXT("NotEnoughPlayers"), TEXT("You do not have enough healthy team mates for this match.|%teammates% healthy team mates are required, you only have %healthy%."))
					.Replace(TEXT("%healthy%"), *FString::FromInt(Pr.NumHealthyTeammates())).Replace(TEXT("%teammates%"), *FString::FromInt(Mates)));
				return;
			}
			FDialog R;
			R.Kind = EDialog::TeamRoles;
			PushDialog(R);
		}
		else
		{
			StartMatch();
		}
		return;
	}
	if (bCancel)
	{
		// refusing a challenge costs the penalty
		if (D.CancelPenalty > 0)
		{
			Pr.Balance -= FMath::Min(D.CancelPenalty, FMath::Max(0, Pr.Balance - Pr.MinBalance));
			SaveProfile();
		}
		CloseDialog();
	}
}

// ---- UT2K4SP_HighScores ----

void FArenaSinglePlayerPage::DrawHighScores(FDialog& D)
{
	if (!D.bSoundPlayed)
	{
		D.bSoundPlayed = true;
		if (GP && GP->bCompleted && !GP->bCheater && D.Param1.IsEmpty())
		{
			AddScore(*GP);
			// UT2K4SP_Main.DoHighScore: making the high score list unlocks the final team's champion
			const TArray<FScore> All = LoadScores();
			const int32 Rank = All.IndexOfByPredicate([this](const FScore& X) { return X.Name == GP->PlayerName && X.Balance == GP->Balance; });
			const int32 MaxEntries = FMath::Max(1, FCString::Atoi(*GetStockValue(TEXT("GUI2K4.SPHighScore"), TEXT("MaxEntries"))));
			if (Rank != INDEX_NONE && Rank < MaxEntries)
			{
				FString Tag;
				if (GP->FinalEnemyTeam.Equals(TEXT("xGame.TeamThundercrash"), ESearchCase::IgnoreCase)) { Tag = TEXT("MALCOLM"); }
				else if (GP->FinalEnemyTeam.Equals(TEXT("xGame.TeamIronSkull"), ESearchCase::IgnoreCase)) { Tag = TEXT("CLANLORD"); }
				else if (GP->FinalEnemyTeam.Equals(TEXT("xGame.TeamCorrupt"), ESearchCase::IgnoreCase)) { Tag = TEXT("XAN"); }
				if (!Tag.IsEmpty() && UnlockCharacter(Tag))
				{
					const FPlayerRecord* R = Get().Players.FindByPredicate([&Tag](const FPlayerRecord& P) { return P.Menu.Equals(Tag, ESearchCase::IgnoreCase); });
					D.Text = FString::Printf(TEXT("%s unlocked."), R ? *R->DefaultName : *Tag);
				}
			}
		}
	}
	const TArray<FScore> Scores = LoadScores();
	const FRect S = G->Screen();
	G->Draw().Rect(0.f, 0.f, S.W, S.H, FLinearColor::Black);
	G->Image(TEXT("2K4Menus.Controls.background_anim"), G->Page(), EImgStyle::Scaled, FLinearColor::White);
	const FRect P = G->Page();
	const FFontRef F = G->Font(TEXT("UT2HeaderFont"));
	const TCHAR* Sec = TEXT("UT2K4SP_HighScores");
	float Y = S.H * 0.08f;
	const float Cols[4] = { 0.1f, 0.42f, 0.61f, 0.82f };
	for (int32 c = 0; c < 4; ++c)
	{
		static const TCHAR* const Fb[] = { TEXT("name"), TEXT("balance"), TEXT("matches"), TEXT("wins") };
		G->Text(F, ArenaSP::Loc(TEXT("GUI2K4"), Sec, *FString::Printf(TEXT("ColumnHeaders[%d]"), c), Fb[c]).ToUpper(), P.X + P.W * Cols[c], Y, C(255, 200, 0));
	}
	Y += F.Height * 1.5f;
	const int32 MaxEntries = FMath::Max(1, FCString::Atoi(*GetStockValue(TEXT("GUI2K4.SPHighScore"), TEXT("MaxEntries"))));
	for (int32 i = 0; i < Scores.Num() && i < MaxEntries && Y < S.H * 0.9f; ++i)
	{
		const bool bMine = GP && Scores[i].Name == GP->PlayerName;
		const FLinearColor Col = bMine ? C(255, 255, 0) : FLinearColor::White;
		const FFontRef SF = G->Font(TEXT("UT2MenuFont"));
		G->Text(SF, FString::Printf(TEXT("%d. %s"), i + 1, *Scores[i].Name), P.X + P.W * 0.1f, Y, Col);
		G->Text(SF, Money(Scores[i].Balance), P.X + P.W * Cols[1], Y, Col);
		G->Text(SF, FString::FromInt(Scores[i].Matches), P.X + P.W * Cols[2], Y, Col);
		G->Text(SF, FString::FromInt(Scores[i].Wins), P.X + P.W * Cols[3], Y, Col);
		Y += SF.Height * 1.15f;
	}
	if (!D.Text.IsEmpty())
	{
		G->TextIn(F, D.Text, FRect(P.X, S.H * 0.87f, P.W, S.H * 0.05f), C(255, 255, 255), EAlign::Center);
	}
	const FString Exit = Caption(Sec, TEXT("ClickToExit"), TEXT("C L I C K   T O   E X I T"));
	G->TextIn(F, Exit, FRect(P.X, S.H * 0.93f, P.W, S.H * 0.05f), C(255, 200, 0, uint8(128 + 127 * FMath::Sin(G->Time() * 3.0))), EAlign::Center);
	const uint32 Id = FGUI::Id(TEXT("SPScoresExit"));
	G->CustomWidget(Id, S, true, FString());
	if (G->WasClicked(Id, S) || G->BackPressed())
	{
		CloseDialog();
	}
}

// ---- UT2K4SP_CGBRTrade / UT2K4SP_CGBRUnTrade ----

void FArenaSinglePlayerPage::DrawTrade(FDialog& D)
{
	if (!GP)
	{
		CloseDialog();
		return;
	}
	FProfile& Pr = *GP;
	const bool bTrade = D.Kind == EDialog::Trade;
	const TCHAR* Sec = bTrade ? TEXT("UT2K4SP_CGBRTrade") : TEXT("UT2K4SP_CGBRUnTrade");
	const FRect Win = DialogFrame(0.1f, 0.25f, 0.8f, 0.5f, FString());
	G->Label(Win.Sub(0.024000f, 0.067800f, 0.568700f, 0.046200f), Caption(TEXT("UT2K4SP_CGBRUnTrade"), TEXT("SPClblTitle.Caption"), TEXT("Bloodrites")), TEXT("TextLabel"),
		EAlign::Center, EFontScale::Large);
	G->Image(TEXT("2K4Menus.Controls.sectionback"), Win.Sub(0.057200f, 0.157500f, 0.121600f, 0.326200f * 2.f), EImgStyle::Scaled, FLinearColor::White);
	G->Image(PortraitOf(D.Param2), Win.Sub(0.060900f, 0.172500f, 0.116600f, 0.312500f * 2.f), EImgStyle::Scaled, FLinearColor::White);
	const FString Team = TeamTitle(D.Param1);
	FString Msg = Caption(Sec, TEXT("Message"), bTrade ? TEXT("You have won the challenge against %enemyteam%. %player% will now become part of your team.")
		: TEXT("Because you lost the challenge against %enemyteam% you have to give up your team mate %player%.|%player% now belongs to the %enemyteam%"))
		.Replace(TEXT("%enemyteam%"), *Team).Replace(TEXT("%player%"), *D.Param2);
	FString Fire;
	if (bTrade && Pr.HasFullTeam())
	{
		// a full team: choose who leaves (refunded)
		TArray<FString> Mates;
		for (const FString& N : Pr.PlayerTeam)
		{
			if (!N.IsEmpty())
			{
				Mates.Add(N);
			}
		}
		D.CharIndex = Mates.Num() > 0 ? (D.CharIndex % Mates.Num() + Mates.Num()) % Mates.Num() : 0;
		Fire = Mates.IsValidIndex(D.CharIndex) ? Mates[D.CharIndex] : FString();
		const int32 B = Pr.FindBot(Fire);
		Msg += TEXT("\n") + Caption(Sec, TEXT("FullTeamWarning"), TEXT("However your team is already at capacity. You will have to fire one of your team mates first. You will be refunded for the loss of the selected player."));
		Msg += TEXT("\n") + Caption(Sec, TEXT("FireInfo"), TEXT("You have selected to fire %fireplayer%, the refund will be %refund%.")).Replace(TEXT("%fireplayer%"), *Fire)
			.Replace(TEXT("%refund%"), *Money(B != INDEX_NONE ? Pr.BotStats[B].Price : 0));
		G->Image(PortraitOf(Fire), Win.Sub(0.807213f, 0.149584f, 0.124694f, 0.324999f * 2.f), EImgStyle::Scaled, FLinearColor(1.f, 0.6f, 0.6f, 1.f));
		if (G->Button(FGUI::Id(TEXT("SPTradePrev")), Win.Sub(0.805000f, 0.806600f, 0.048700f, 0.0612f * 2.f), TEXT("ArrowLeft"), FString(), FString()))
		{
			--D.CharIndex;
		}
		if (G->Button(FGUI::Id(TEXT("SPTradeNext")), Win.Sub(0.898400f, 0.806600f, 0.048700f, 0.0612f * 2.f), TEXT("ArrowRight"), FString(), FString()))
		{
			++D.CharIndex;
		}
	}
	G->ScrollText(FGUI::Id(TEXT("SPTradeMsg")), Win.Sub(0.25f, 0.166400f, 0.52f, 0.6f), Msg, TEXT("UT2MenuFont"), FLinearColor::White);
	if (G->Button(FGUI::Id(TEXT("SPTradeOk")), Win.Sub(0.310000f, 0.821000f, 0.222200f, 0.1f), TEXT("SquareButton"), Caption(TEXT("UT2K4SP_CGBRUnTrade"), TEXT("SPMbtnOk.Caption"), TEXT("OK")),
		FString(), true, EFontScale::Small) || G->BackPressed())
	{
		if (bTrade)
		{
			if (!Fire.IsEmpty() && Pr.ReleaseTeammate(Fire))
			{
				const int32 B = Pr.FindBot(Fire);
				if (B != INDEX_NONE)
				{
					Pr.Balance += Pr.BotStats[B].Price;
				}
			}
			Pr.AddTeammate(D.Param2);
		}
		SaveProfile();
		CloseDialog();
	}
}
