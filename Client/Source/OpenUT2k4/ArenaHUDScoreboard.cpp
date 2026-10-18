// UT2004's scoreboards on the canvas: XInterface.ScoreBoardDeathMatch (with the LMS lives column and the Invasion team
// score box of ScoreBoardLMS / ScoreBoardInvasion) and XInterface.ScoreBoardTeamDeathMatch (team games). Layout and font
// rules follow the classes' UpdateScoreBoard / DrawTeam / DrawTitle; the fonts are the HUD fonts (ArenaHudFonts.h) at
// the font index the UE2 rules pick for the canvas (FArenaDraw::UTClipX), the boxes InterfaceContent.Menu.ScoreBox{A,B,C}
// drawn like Canvas.DrawTileStretched. UE2 laid the board out over the full canvas width; wider than 16:9 it keeps a
// 16:9 area in the middle so the rows are not stretched across ultra wide screens.

#include "ArenaHUD.h"
#include "ArenaHUDWidgets.h"
#include "ArenaCharacter.h"
#include "ArenaGameRules.h"
#include "ArenaGameState.h"
#include "ArenaHudFonts.h"
#include "ArenaLocalMessages.h"
#include "ArenaPlayerController.h"
#include "ArenaPlayerState.h"
#include "ArenaUserSettings.h"

#include "Engine/Canvas.h"
#include "Engine/World.h"


namespace ArenaHUDScoreboardLocal
{
	using namespace ArenaHudUT;
	// HudBase FontScreenWidthMedium / FontScreenWidthSmall
	const float FontScreenWidthMedium[9] = { 2048.f, 1600.f, 1280.f, 1024.f, 800.f, 640.f, 512.f, 400.f, 320.f };
	const float FontScreenWidthSmall[9] = { 4096.f, 3200.f, 2560.f, 2048.f, 1600.f, 1280.f, 1024.f, 800.f, 640.f };

	/** ScoreBoard.GetSmallerFontFor */
	int32 SmallerFont(float ClipX, int32 Offset)
	{
		for (int32 i = 0; i < 8 - Offset; ++i)
		{
			if (FontScreenWidthMedium[i] <= ClipX)
			{
				return FMath::Clamp(i + Offset, 0, 8);
			}
		}
		return 8;
	}

	/** ScoreBoard.GetSmallFontFor */
	int32 SmallFont(float ScreenWidth, int32 Offset)
	{
		for (int32 i = 0; i < 8 - Offset; ++i)
		{
			if (FontScreenWidthSmall[i] <= ScreenWidth)
			{
				return FMath::Clamp(i + Offset, 0, 8);
			}
		}
		return 8;
	}

	/** ScoreBoard.HaveHalfFont */
	bool HaveHalfFont(float ClipX, int32 Offset)
	{
		for (int32 i = 0; i < 9 - Offset; ++i)
		{
			if (FontScreenWidthSmall[i] <= ClipX)
			{
				return true;
			}
		}
		return false;
	}

	/** HUD.LargerFontThan */
	int32 LargerFontThan(int32 Font)
	{
		return Font < 7 ? FMath::Max(0, Font - 4) : 5;
	}

	FString UTString(const TCHAR* Package, const TCHAR* Section, const TCHAR* Key, const TCHAR* Fallback)
	{
		FString V;
		return ArenaLocalMessages::Localize(Package, Section, Key, V) ? V : FString(Fallback);
	}

	/** ScoreBoard.FormatTime */
	FString FormatTime(int32 Seconds)
	{
		FString Time;
		if (Seconds > 3600)
		{
			Time = FString::Printf(TEXT("%d:"), Seconds / 3600);
			Seconds %= 3600;
		}
		return Time + FString::Printf(TEXT("%02d:%02d"), Seconds / 60, Seconds % 60);
	}

	const TCHAR* ScoreBox(int32 Team)
	{
		// TeamBoxMaterial: ScoreBoxC red, ScoreBoxB blue; BoxMaterial ScoreBoxA
		return Team == 0 ? TEXT("Textures/InterfaceContent/Menu_ScoreBoxC")
			: (Team == 1 ? TEXT("Textures/InterfaceContent/Menu_ScoreBoxB") : TEXT("Textures/InterfaceContent/Menu_ScoreBoxA"));
	}

	FLinearColor C(const FColor& Color) { return FLinearColor(Color); }

	/** Canvas state for one board: the layout area and the UE2 ClipX the font rules see. */
	struct FBoard
	{
		const FArenaDraw* D = nullptr;
		float X0 = 0.f, W = 0.f, H = 0.f;
		/** ClipX for the font rules (FArenaDraw::UTClipX) */
		float FontClipX = 0.f;
		float Px = 1.f;

		float Height(int32 Font) const { return D->TextSizeUT(Font, TEXT("Test")).Y; }
		float Width(int32 Font, const FString& S) const { return D->TextSizeUT(Font, S).X; }
		void Text(int32 Font, const FString& S, float X, float Y, const FColor& Color) const
		{
			D->TextUT(Font, S, FMath::RoundToFloat(X), FMath::RoundToFloat(Y), C(Color));
		}
		/** The name cut to fit MaxWidth (UpdateScoreBoard: Left(Name, MaxNamePos / XL * Len)). */
		FString Fit(int32 Font, const FString& Name, float MaxWidth) const
		{
			const float XL = Width(Font, Name);
			return XL > MaxWidth ? Name.Left(int32(MaxWidth / XL * Name.Len())) : Name;
		}
	};
}


void AArenaHUD::DrawUTScoreboard()
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDScoreboardLocal;
	const AArenaGameState* GS = GetArenaGS();
	if (!GS)
	{
		return;
	}
	if (GS->IsTeamGame() && GS->ModeId != TEXT("INV"))
	{
		DrawUTScoreboardTeams();
	}
	else
	{
		DrawUTScoreboardDM();
	}
}

namespace ArenaHUDScoreboardLocal
{
	using namespace ArenaHudUT;
	/** ScoreBoardDeathMatch.GetTitleString: skill, game, " in " map. */
	FString TitleString(const AArenaGameState* GS, const FString& Extra)
	{
		const int32 Skill = FMath::Clamp(UArenaUserSettings::Get()->LastSkill, 0, 7);
		const FString Key = FString::Printf(TEXT("SkillLevel[%d]"), Skill);
		static const TCHAR* Ours[8] = { TEXT("NOVICE"), TEXT("AVERAGE"), TEXT("EXPERIENCED"), TEXT("SKILLED"), TEXT("ADEPT"), TEXT("MASTERFUL"), TEXT("INHUMAN"), TEXT("GODLIKE") };
		const FString SkillText = UTString(TEXT("XInterface"), TEXT("ScoreBoardDeathMatch"), *Key, Ours[Skill]);
		return SkillText + TEXT(" ") + GS->GetGameTypeName() + Extra + UTString(TEXT("XInterface"), TEXT("ScoreBoardDeathMatch"), TEXT("MapName"), TEXT(" in ")) + GS->LayoutName;
	}

	/** ScoreBoardDeathMatch.GetDefaultScoreInfoString */
	FString ScoreInfoString(const AArenaGameState* GS, float Elapsed, bool bTeam)
	{
		FString S;
		if (GS->ModeId == TEXT("LMS") && GS->GoalScore != 0)
		{
			// GRI.MaxLives
			S = UTString(TEXT("XInterface"), TEXT("ScoreBoardDeathMatch"), TEXT("MaxLives"), TEXT("MAX LIVES:")) + TEXT(" ") + FString::FromInt(GS->GoalScore);
		}
		else if (GS->GoalScore != 0 && GS->ModeId != TEXT("INV"))
		{
			S = (bTeam ? UTString(TEXT("XInterface"), TEXT("ScoreBoardTeamDeathMatch"), TEXT("FragLimit"), TEXT("SCORE LIMIT:"))
				: UTString(TEXT("XInterface"), TEXT("ScoreBoardDeathMatch"), TEXT("FragLimit"), TEXT("FRAG LIMIT:"))) + TEXT(" ") + FString::FromInt(GS->GoalScore);
		}
		const FString Spacer = TEXT(" ");
		if (GS->TimeLimit != 0)
		{
			S += TEXT(" ") + Spacer + TEXT(" ") + UTString(TEXT("XInterface"), TEXT("ScoreBoardDeathMatch"), TEXT("TimeLimit"), TEXT("REMAINING TIME:")) + FormatTime(GS->RemainingTime);
		}
		else
		{
			S += TEXT(" ") + Spacer + TEXT(" ") + UTString(TEXT("XInterface"), TEXT("ScoreBoardDeathMatch"), TEXT("FooterText"), TEXT("Elapsed Time:")) + TEXT(" ")
				+ FormatTime(int32(Elapsed));
		}
		return S.TrimStart();
	}
}

/** ScoreBoardDeathMatch.DrawTitle (shared by both boards) */
static void DrawUTScoreTitle(const ArenaHUDScoreboardLocal::FBoard& B, AArenaHUD* HUD, const AArenaGameState* GS, int32 Font, float HeaderOffsetY, float PlayerAreaY, float PlayerBoxSizeY,
	bool bTeam, const FString& TitleExtra)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDScoreboardLocal;
	const APlayerController* PC = HUD->GetOwningPlayerController();
	const AArenaPlayerState* MyPS = PC ? PC->GetPlayerState<AArenaPlayerState>() : nullptr;
	const FString Title = TitleString(GS, TitleExtra);
	const float YL = B.Height(Font);
	const float Elapsed = HUD->GetWorld() ? HUD->GetWorld()->GetTimeSeconds() : 0.f;
	FString Info = ScoreInfoString(GS, Elapsed, bTeam);
	const FColor Gold(255, 255, 0, 255);
	if (GS->Phase == EArenaMatchPhase::Ended)
	{
		const bool bWon = GS->IsTeamGame() ? (MyPS && MyPS->Team == GS->WinningTeam) : (MyPS && GS->WinningPlayer == MyPS);
		Info = bWon ? UTString(TEXT("XInterface"), TEXT("HudBase"), TEXT("YouveWonTheMatch"), TEXT("You've won the match!"))
			: UTString(TEXT("XInterface"), TEXT("HudBase"), TEXT("YouveLostTheMatch"), TEXT("You've lost the match."));
	}
	else
	{
		const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		const AArenaCharacter* Char = Cast<AArenaCharacter>(Pawn);
		const bool bDead = MyPS && !MyPS->IsSpectator() && (!Pawn || (Char && !Char->IsAlive()));
		if (bDead)
		{
			const FString Restart = MyPS->bEliminated
				? UTString(TEXT("XInterface"), TEXT("ScoreBoardDeathMatch"), TEXT("OutFireText"), TEXT("   You are out. Fire to watch the others."))
				: UTString(TEXT("XInterface"), TEXT("ScoreBoardDeathMatch"), TEXT("Restart"), TEXT("   You died. Press Fire to respawn!"));
			if (B.H - HeaderOffsetY - PlayerAreaY >= 2.5f * YL)
			{
				const float XL = B.Width(Font, Restart);
				B.Text(Font, Restart, B.X0 + 0.5f * (B.W - XL), B.H - 2.5f * YL, Gold);
			}
			else
			{
				Info = Restart;
			}
		}
	}
	const float TitleXL = B.Width(Font, Title);
	B.Text(Font, Title, B.X0 + 0.5f * (B.W - TitleXL), (HeaderOffsetY - PlayerBoxSizeY - YL) * 0.5f, Gold);
	const float InfoXL = B.Width(Font, Info);
	B.Text(Font, Info, B.X0 + 0.5f * (B.W - InfoXL), B.H - 1.5f * YL, Gold);
}

static ArenaHUDScoreboardLocal::FBoard MakeBoard(const FArenaDraw& D)
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDScoreboardLocal;
	FBoard B;
	B.D = &D;
	B.H = D.H();
	// never wider than the HUD region (Settings > HUD Width)
	float RX0, RW;
	ArenaHudUT::HudRegion(D.W(), D.H(), RX0, RW);
	B.W = FMath::Min3(D.W(), D.H() * 16.f / 9.f, RW);
	B.X0 = FMath::RoundToFloat((D.W() - B.W) * 0.5f);
	B.FontClipX = D.UTClipX();
	B.Px = D.UTFontScale();
	return B;
}

void AArenaHUD::DrawUTScoreboardDM()
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDScoreboardLocal;
	const AArenaGameState* GS = GetArenaGS();
	const AArenaPlayerState* OwnerPRI = PlayerOwner ? PlayerOwner->GetPlayerState<AArenaPlayerState>() : nullptr;
	const FBoard B = MakeBoard(D);
	const bool bLives = GS->ModeId == TEXT("LMS");
	const bool bInvasion = GS->ModeId == TEXT("INV");

	TArray<AArenaPlayerState*> PRIArray = GS->GetSortedPlayers();
	if (bLives)
	{
		// ScoreBoardLMS.InOrder: players still in first
		PRIArray.StableSort([](const AArenaPlayerState& A, const AArenaPlayerState& Bp) { return !A.bEliminated && Bp.bEliminated; });
	}
	int32 PlayerCount = FMath::Min(PRIArray.Num(), 32);
	const int32 OwnerOffset = OwnerPRI ? PRIArray.IndexOfByKey(OwnerPRI) : INDEX_NONE;
	const float ClipX = B.FontClipX;
	const float ClipY = B.H;

	int32 Font = ArenaHudFonts::MediumFontIndex(ClipX);
	float YL = B.Height(Font);
	int32 BoxSpaceY = int32(0.25f * YL);
	int32 PlayerBoxSizeY = int32(1.5f * YL);
	int32 HeadFoot = int32(5.f * YL);
	const int32 MessageFoot = int32(1.5f * HeadFoot);
	int32 FontReduction = 0;
	auto Fits = [&](float Foot) { return float(PlayerCount) <= (ClipY - Foot) / float(PlayerBoxSizeY + BoxSpaceY); };
	if (!Fits(1.5f * HeadFoot))
	{
		BoxSpaceY = int32(0.125f * YL);
		PlayerBoxSizeY = int32(1.25f * YL);
		if (!Fits(1.5f * HeadFoot))
		{
			PlayerBoxSizeY = int32(1.125f * YL);
			if (!Fits(1.5f * HeadFoot))
			{
				for (int32 Step = 0; Step < 3; ++Step)
				{
					++FontReduction;
					Font = SmallerFont(ClipX, FontReduction);
					YL = B.Height(Font);
					BoxSpaceY = int32(0.125f * YL);
					PlayerBoxSizeY = int32(1.125f * YL);
					HeadFoot = int32(5.f * YL);
					if (Fits(float(HeadFoot)) || (Step == 1 && ClipY < 768.f * B.Px))
					{
						break;
					}
				}
			}
		}
	}
	PlayerCount = FMath::Min(PlayerCount, int32((ClipY - HeadFoot) / (PlayerBoxSizeY + BoxSpaceY)));
	if (OwnerOffset >= PlayerCount && PlayerCount > 0)
	{
		--PlayerCount;
	}
	const float MaxScaling = FontReduction > 2 ? 3.f : 2.125f;
	if (PlayerCount > 0)
	{
		PlayerBoxSizeY = int32(FMath::Clamp((1.f + (ClipY - 0.67f * MessageFoot)) / PlayerCount - BoxSpaceY, float(PlayerBoxSizeY), MaxScaling * YL));
	}
	const int32 HeaderOffsetY = int32(3.f * YL);
	int32 BoxWidth = int32(0.9375f * B.W);
	const int32 BoxXPos = int32(B.X0 + 0.5f * (B.W - BoxWidth));
	BoxWidth = int32(B.W - 2 * (BoxXPos - B.X0));
	const int32 NameXPos = int32(BoxXPos + 0.0625f * BoxWidth);
	const int32 ScoreXPos = int32(BoxXPos + 0.5f * BoxWidth);
	const int32 DeathsXPos = int32(BoxXPos + 0.6875f * BoxWidth);
	const int32 Row = PlayerBoxSizeY + BoxSpaceY;

	// the rows: ScoreBoxA in white * 0.5 (the colour's alpha halves too)
	const FLinearColor RowColor = C(FColor(127, 127, 127, 127));
	for (int32 i = 0; i < PlayerCount; ++i)
	{
		DrawTileStretched(D, ScoreBox(-1), float(BoxXPos), float(HeaderOffsetY + Row * i), float(BoxWidth), float(PlayerBoxSizeY), RowColor);
	}
	if (bInvasion)
	{
		// ScoreBoardInvasion: "Team Score    N" in a blue box above the list
		const FString TeamScore = UTString(TEXT("SkaarjPack"), TEXT("ScoreBoardInvasion"), TEXT("TeamScoreString"), TEXT("Team Score")) + TEXT("    ") + FString::FromInt(GS->TeamScores[0]);
		const float XL = B.Width(Font, TeamScore);
		DrawTileStretched(D, ScoreBox(-1), float(BoxXPos), HeaderOffsetY - 2.75f * YL, XL + 0.125f * BoxWidth, 1.25f * YL, C(FColor(0, 0, 255, 255)));
		B.Text(Font, TeamScore, float(NameXPos), HeaderOffsetY - 2.5f * YL, FColor::White);
	}
	FString TitleExtra;
	if (bInvasion)
	{
		TitleExtra = TEXT(" ") + UTString(TEXT("SkaarjPack"), TEXT("ScoreBoardInvasion"), TEXT("WaveString"), TEXT("Wave")) + TEXT(" ") + FString::FromInt(GS->Round);
	}
	DrawUTScoreTitle(B, this, GS, Font, float(HeaderOffsetY), float((PlayerCount + 1) * Row), float(PlayerBoxSizeY), false, TitleExtra);

	const FString PlayerText = UTString(TEXT("XInterface"), TEXT("ScoreBoardDeathMatch"), TEXT("PlayerText"), TEXT("PLAYER"));
	const FString PointsText = UTString(TEXT("XInterface"), TEXT("ScoreBoardDeathMatch"), TEXT("PointsText"), TEXT("SCORE"));
	const FString DeathsText = bLives ? UTString(TEXT("BonusPack"), TEXT("ScoreBoardLMS"), TEXT("LivesText"), TEXT("LIVES"))
		: UTString(TEXT("XInterface"), TEXT("ScoreBoardDeathMatch"), TEXT("DeathsText"), TEXT("DEATHS"));
	const FString OutText = UTString(TEXT("XInterface"), TEXT("ScoreBoardDeathMatch"), TEXT("OutText"), TEXT("OUT"));
	const float TitleYPos = HeaderOffsetY - 1.25f * YL;
	B.Text(Font, PlayerText, float(NameXPos), TitleYPos, FColor::White);
	B.Text(Font, PointsText, ScoreXPos - 0.5f * B.Width(Font, PointsText), TitleYPos, FColor::White);
	B.Text(Font, DeathsText, DeathsXPos - 0.5f * B.Width(Font, DeathsText), TitleYPos, FColor::White);
	if (PlayerCount <= 0)
	{
		return;
	}
	// long names use a smaller font, then get cut
	const float MaxNamePos = 0.9f * (ScoreXPos - NameXPos);
	int32 NameFont = Font;
	float Longest = 0.f;
	FString LongestName;
	for (int32 i = 0; i < PlayerCount || (i == OwnerOffset); ++i)
	{
		if (!PRIArray.IsValidIndex(i))
		{
			break;
		}
		const float XL = B.Width(Font, PRIArray[i]->GetPlayerName());
		if (XL > FMath::Max(Longest, MaxNamePos))
		{
			Longest = XL;
			LongestName = PRIArray[i]->GetPlayerName();
		}
	}
	if (Longest > 0.f)
	{
		for (int32 Step = 1; Step <= 3; ++Step)
		{
			NameFont = SmallerFont(ClipX, FontReduction + Step);
			if (B.Width(NameFont, LongestName) <= MaxNamePos)
			{
				break;
			}
		}
	}
	const float NameYL = B.Height(NameFont);
	const float BoxTextOffsetY = HeaderOffsetY + 0.5f * (PlayerBoxSizeY - YL);
	auto LivesOf = [&](const AArenaPlayerState* P) { return FString::FromInt(FMath::Max(0, P->Lives)); };
	for (int32 i = 0; i < PlayerCount; ++i)
	{
		if (i == OwnerOffset)
		{
			continue;
		}
		const AArenaPlayerState* P = PRIArray[i];
		const FColor RowTint = (bLives && P->bEliminated) ? FColor(191, 191, 191, 255) : FColor::White; // GrayedOut 0.75
		B.Text(NameFont, B.Fit(NameFont, P->GetPlayerName(), MaxNamePos), float(NameXPos), Row * i + BoxTextOffsetY + 0.5f * (YL - NameYL), RowTint);
		B.Text(Font, FString::FromInt(FMath::RoundToInt(P->GetScore())), float(ScoreXPos), Row * i + BoxTextOffsetY, RowTint);
		if (bLives && P->bEliminated)
		{
			B.Text(Font, OutText, DeathsXPos - 0.5f * B.Width(Font, OutText), Row * i + BoxTextOffsetY, RowTint);
		}
		else
		{
			B.Text(Font, bLives ? LivesOf(P) : FString::FromInt(P->Deaths), float(DeathsXPos), Row * i + BoxTextOffsetY, RowTint);
		}
	}
	if (OwnerOffset != INDEX_NONE)
	{
		float OwnerPos;
		if (OwnerOffset >= PlayerCount)
		{
			// the local player below the list in a turquoise box
			OwnerPos = float(Row * PlayerCount) + BoxTextOffsetY;
			DrawTileStretched(D, ScoreBox(-1), float(BoxXPos), float(HeaderOffsetY + Row * PlayerCount), float(BoxWidth), float(PlayerBoxSizeY),
				C(FColor(0, 64, 127, 127)));
		}
		else
		{
			OwnerPos = float(Row * OwnerOffset) + BoxTextOffsetY;
		}
		const AArenaPlayerState* P = PRIArray[OwnerOffset];
		const FColor Gold(255, 255, 0, 255);
		B.Text(NameFont, B.Fit(NameFont, P->GetPlayerName(), MaxNamePos), float(NameXPos), OwnerPos + 0.5f * (YL - NameYL), Gold);
		B.Text(Font, FString::FromInt(FMath::RoundToInt(P->GetScore())), float(ScoreXPos), OwnerPos, Gold);
		if (bLives && P->bEliminated)
		{
			B.Text(Font, OutText, DeathsXPos - 0.5f * B.Width(Font, OutText), OwnerPos, Gold);
		}
		else
		{
			B.Text(Font, bLives ? LivesOf(P) : FString::FromInt(P->Deaths), float(DeathsXPos), OwnerPos, Gold);
		}
	}
	// Level.NetMode == NM_Standalone: no NET column
}

void AArenaHUD::DrawUTScoreboardTeams()
{
	using namespace ArenaHudUT;
	using namespace ArenaHUDScoreboardLocal;
	const AArenaGameState* GS = GetArenaGS();
	const AArenaPlayerState* OwnerPRI = PlayerOwner ? PlayerOwner->GetPlayerState<AArenaPlayerState>() : nullptr;
	const FBoard B = MakeBoard(D);
	const float ClipX = B.FontClipX;
	const float ClipY = B.H;

	TArray<AArenaPlayerState*> TeamPRI[2];
	int32 OwnerOffset[2] = { -1, -1 };
	for (AArenaPlayerState* P : GS->GetSortedPlayers())
	{
		if (P && (P->Team == 0 || P->Team == 1) && TeamPRI[P->Team].Num() < 32)
		{
			if (P == OwnerPRI)
			{
				OwnerOffset[P->Team] = TeamPRI[P->Team].Num();
			}
			TeamPRI[P->Team].Add(P);
		}
	}
	int32 Count[2] = { TeamPRI[0].Num(), TeamPRI[1].Num() };
	int32 MaxPlayerCount = FMath::Max(Count[0], Count[1]);

	int32 Font = ArenaHudFonts::MediumFontIndex(ClipX);
	float YL = B.Height(Font);
	// IconSize = FMax(2 * YL, 64 * ClipX / 1024) (in screen pixels: the 4:3 frame width)
	const float FrameW = FMath::Min(D.W(), D.H() * 4.f / 3.f);
	const float IconSize = FMath::Max(2.f * YL, 64.f * FrameW / 1024.f);
	int32 FontReduction = 0;
	int32 BoxSpaceY = int32(0.25f * YL);
	int32 PlayerBoxSizeY = HaveHalfFont(ClipX, FontReduction) ? int32(2.125f * YL) : int32(1.75f * YL);
	int32 HeadFoot = int32(4.f * YL + IconSize);
	const float MessageFoot = 1.5f * HeadFoot;
	auto Fits = [&](float Foot) { return float(MaxPlayerCount) <= (ClipY - Foot) / float(PlayerBoxSizeY + BoxSpaceY); };
	if (!Fits(1.5f * HeadFoot))
	{
		BoxSpaceY = int32(0.125f * YL);
		if (!Fits(1.5f * HeadFoot))
		{
			for (int32 Step = 0; Step < 4; ++Step)
			{
				++FontReduction;
				Font = SmallerFont(ClipX, FontReduction);
				YL = B.Height(Font);
				BoxSpaceY = int32(0.125f * YL);
				PlayerBoxSizeY = HaveHalfFont(ClipX, FontReduction) ? int32(2.125f * YL) : int32(1.75f * YL);
				HeadFoot = int32(4.f * YL + IconSize);
				if (Fits(Step < 2 ? 1.5f * HeadFoot : float(HeadFoot)) || (Step == 2 && ClipY < 600.f * B.Px))
				{
					break;
				}
			}
		}
	}
	MaxPlayerCount = FMath::Min(MaxPlayerCount, int32(1.f + (ClipY - HeadFoot) / (PlayerBoxSizeY + BoxSpaceY)));
	const float MaxScaling = FontReduction > 2 ? 3.f : 2.125f;
	if (MaxPlayerCount > 0)
	{
		PlayerBoxSizeY = int32(FMath::Clamp((1.f + (ClipY - 0.67f * MessageFoot)) / MaxPlayerCount - BoxSpaceY, float(PlayerBoxSizeY), MaxScaling * YL));
	}
	for (int32 T = 0; T < 2; ++T)
	{
		Count[T] = FMath::Min(Count[T], MaxPlayerCount);
		if (OwnerOffset[T] >= Count[T] && Count[T] > 0)
		{
			--Count[T];
		}
	}
	const int32 HeaderOffsetY = int32(1.5f * YL + IconSize);
	const int32 Row = PlayerBoxSizeY + BoxSpaceY;
	DrawUTScoreTitle(B, this, GS, Font, float(HeaderOffsetY), float((MaxPlayerCount + 1) * Row), float(PlayerBoxSizeY), true, FString());

	const FString OutText = UTString(TEXT("XInterface"), TEXT("ScoreBoardDeathMatch"), TEXT("OutText"), TEXT("OUT"));
	const int32 MainFont = Font;
	for (int32 TeamNum = 0; TeamNum < 2; ++TeamNum)
	{
		// ---- ScoreBoardTeamDeathMatch.DrawTeam ----
		const TArray<AArenaPlayerState*>& PRIArray = TeamPRI[TeamNum];
		const int32 PlayerCount = Count[TeamNum];
		const int32 Owner = OwnerOffset[TeamNum];
		int32 BoxWidth = int32(0.47f * B.W);
		int32 BoxXPos = int32(0.5f * (0.5f * B.W - BoxWidth));
		BoxWidth = int32(0.5f * B.W - 2 * BoxXPos);
		BoxXPos = int32(B.X0 + BoxXPos + TeamNum * 0.5f * B.W);
		const int32 NameXPos = int32(BoxXPos + 0.05f * BoxWidth);
		const int32 ScoreXPos = int32(BoxXPos + 0.55f * BoxWidth);
		const bool bHaveHalfFont = HaveHalfFont(ClipX, FontReduction);
		DrawTileStretched(D, ScoreBox(TeamNum), float(BoxXPos), float(HeaderOffsetY), float(BoxWidth), float(PlayerCount * Row), FLinearColor::White);
		// the team symbol and score above the list (IconScale = ClipX / 4096)
		const float IconScale = FrameW / 4096.f;
		const FString& Symbol = GS->GetTeamSymbol(TeamNum);
		const float SymbolUSize = 256.f, SymbolVSize = 256.f;
		int32 ScoreYPos = int32(HeaderOffsetY - SymbolVSize * IconScale - BoxSpaceY);
		DrawTileStretched(D, ScoreBox(-1), float(BoxXPos), float(ScoreYPos - BoxSpaceY), float(BoxWidth), float(HeaderOffsetY + BoxSpaceY - ScoreYPos),
			C(FColor(191, 191, 191, 191)));
		const FColor TeamColor = TeamNum == 0 ? FColor(255, 0, 0, 255) : FColor(0, 0, 255, 255);
		const float TeamCenterX = B.X0 + (0.25f + 0.5f * TeamNum) * B.W;
		if (!Symbol.IsEmpty())
		{
			DrawTilePath(D, *Symbol, 256.f, 256.f, TeamCenterX - (SymbolUSize + 32.f) * IconScale, float(ScoreYPos), SymbolUSize * IconScale, SymbolVSize * IconScale,
				0.f, 0.f, 256.f, 256.f, C(TeamColor));
		}
		const int32 ScoreFont = LargerFontThan(MainFont);
		const float ScoreYL = B.Height(ScoreFont);
		ScoreYPos = int32(ScoreYPos + 0.5f * SymbolVSize * IconScale - 0.5f * ScoreYL);
		B.Text(ScoreFont, FString::FromInt(GS->TeamScores[TeamNum]), TeamCenterX + 32.f * IconScale, float(ScoreYPos), TeamColor);
		if (PlayerCount <= 0)
		{
			continue;
		}
		// separators between the rows: InterfaceContent.Menu.ButtonBob in the team colour
		const float ScoreBackScale = FrameW / 1024.f;
		const int32 LastLine = Owner >= PlayerCount ? PlayerCount + 1 : PlayerCount;
		for (int32 i = 1; i < LastLine; ++i)
		{
			DrawTileStretched(D, TEXT("Textures/InterfaceContent/Menu_ButtonBob"), float(NameXPos), HeaderOffsetY + Row * i - 0.5f * BoxSpaceY, 0.9f * BoxWidth,
				ScoreBackScale * 3.f, C(TeamNum == 0 ? FColor(255, 0, 0, 255) : FColor(0, 0, 255, 255)));
		}
		const float MaxNamePos = 0.95f * (ScoreXPos - NameXPos);
		int32 NameFont = MainFont;
		float Longest = 0.f;
		FString LongestName;
		for (int32 i = 0; i < PlayerCount; ++i)
		{
			const float XL = B.Width(MainFont, PRIArray[i]->GetPlayerName());
			if (XL > FMath::Max(Longest, MaxNamePos))
			{
				Longest = XL;
				LongestName = PRIArray[i]->GetPlayerName();
			}
		}
		if (Longest > 0.f)
		{
			for (int32 Step = 1; Step <= 3; ++Step)
			{
				NameFont = SmallerFont(ClipX, FontReduction + Step);
				if (B.Width(NameFont, LongestName) <= MaxNamePos)
				{
					break;
				}
			}
		}
		float NameY = 0.f;
		if (D.W() < 512.f)
		{
			NameY = 0.5f * YL;
		}
		else if (!bHaveHalfFont)
		{
			NameY = 0.125f * YL;
		}
		const float BoxTextOffsetY = HeaderOffsetY + 0.5f * PlayerBoxSizeY - 0.5f * YL;
		for (int32 i = 0; i < PlayerCount; ++i)
		{
			if (i == Owner)
			{
				continue;
			}
			// the owner's team leaves a line under each name (orders / location in UT2004)
			const float NY = Owner == -1 ? Row * i + BoxTextOffsetY : Row * i + BoxTextOffsetY - 0.5f * YL + NameY;
			B.Text(NameFont, B.Fit(NameFont, PRIArray[i]->GetPlayerName(), MaxNamePos), float(NameXPos), NY, FColor::White);
			B.Text(MainFont, PRIArray[i]->bEliminated ? OutText : FString::FromInt(FMath::RoundToInt(PRIArray[i]->GetScore())), float(ScoreXPos),
				Row * i + BoxTextOffsetY, FColor::White);
		}
		if (Owner >= 0)
		{
			float OwnerPos;
			if (Owner >= PlayerCount)
			{
				OwnerPos = float(Row * PlayerCount) + BoxTextOffsetY;
				DrawTileStretched(D, ScoreBox(TeamNum), float(BoxXPos), float(HeaderOffsetY + Row * PlayerCount), float(BoxWidth), float(PlayerBoxSizeY), FLinearColor::White);
			}
			else
			{
				OwnerPos = float(Row * Owner) + BoxTextOffsetY;
			}
			const AArenaPlayerState* P = PRIArray[Owner];
			const FColor Gold(255, 255, 0, 255);
			B.Text(NameFont, B.Fit(NameFont, P->GetPlayerName(), MaxNamePos), float(NameXPos), OwnerPos - 0.5f * YL + NameY, Gold);
			B.Text(MainFont, P->bEliminated ? OutText : FString::FromInt(FMath::RoundToInt(P->GetScore())), float(ScoreXPos), OwnerPos, Gold);
		}
		// flag carriers: a flag left of the name (UT2004 drew MenuEffects.ScoreBoard.ScoreboardU_FB, a logo, here the HUD flag)
		const float FlagIconScale = FrameW / 1024.f;
		for (int32 i = 0; i < PlayerCount || i == Owner; ++i)
		{
			if (!PRIArray.IsValidIndex(i))
			{
				break;
			}
			const AArenaCharacter* Char = Cast<AArenaCharacter>(PRIArray[i]->GetPawn());
			if (!Char || !Char->IsCarryingObjective())
			{
				continue;
			}
			const float RowY = (i == Owner && Owner >= PlayerCount) ? float(Row * PlayerCount) + BoxTextOffsetY : float(Row * i) + BoxTextOffsetY;
			const int32 OtherTeam = 1 - TeamNum;
			DrawTile(D, ETex::Hud, NameXPos - 48.f * FlagIconScale, RowY - 16.f * FlagIconScale + 0.5f * YL - 16.f * FlagIconScale, 48.f * FlagIconScale,
				32.f * FlagIconScale, 336.f, 129.f, 61.f, 41.f, C(Colors::Team(OtherTeam)));
		}
	}
}
