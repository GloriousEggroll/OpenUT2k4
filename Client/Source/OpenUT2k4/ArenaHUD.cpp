#include "ArenaHUD.h"
#include "ArenaTranslocator.h"
#include "OpenUT2k4.h"
#include "ArenaPlayerController.h"
#include "ArenaGameState.h"
#include "ArenaPlayerState.h"
#include "ArenaCharacter.h"
#include "ArenaWeapon.h"
#include "ArenaWeapons.h"
#include "ArenaWeaponsExtra.h"
#include "ArenaWeaponActors.h"
#include "ArenaHUDWidgets.h"
#include "ArenaFlag.h"
#include "ArenaTypes.h"
#include "ArenaUserSettings.h"
#include "ArenaCarryable.h"
#include "ArenaGameRules.h"
#include "ArenaArtHUD.h"
#include "ArenaHudFonts.h"
#include "ArenaLocalMessages.h"
#include "ArenaMidGameMenu.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "TextureResource.h"
#include "CanvasItem.h"
#include "Misc/CommandLine.h"

// =============================================================================================
// FArenaDraw
// =============================================================================================

FArenaDraw::FArenaDraw(UCanvas* InCanvas)
	: Canvas(InCanvas)
{
	Scale = (Canvas && Canvas->SizeY > 0) ? float(Canvas->SizeY) / 1080.f : 1.f;
}

float FArenaDraw::W() const
{
	return Canvas ? float(Canvas->SizeX) : 1920.f;
}

float FArenaDraw::H() const
{
	return Canvas ? float(Canvas->SizeY) : 1080.f;
}

UFont* FArenaDraw::GetFont()
{
	if (!GEngine)
	{
		return nullptr;
	}
	if (UFont* Large = GEngine->GetLargeFont())
	{
		return Large;
	}
	return GEngine->GetMediumFont();
}

namespace ArenaHUDLocal
{
static float HudFontScaleFor(const UFont* Font, float SizePx)
{
	float Base = Font ? Font->GetMaxCharHeight() : 0.f;
	if (Base <= 1.f)
	{
		Base = 24.f;
	}
	return SizePx / Base;
}

/**
 * UE2 colour codes in text (names and chat from UT2004 servers keep them: Net/UE2NetBits.cpp): ESC R G B switches the
 * colour of what follows, as UT2004's Canvas does. Text without ESC is unchanged.
 */
struct FColourRun
{
	FString Text;
	bool bColour = false;
	FColor Colour;
};

static bool HasColourCodes(const FString& InText)
{
	int32 Index = INDEX_NONE;
	return InText.FindChar(TCHAR(0x1B), Index);
}

static FString StripColourCodes(const FString& InText)
{
	if (!HasColourCodes(InText))
	{
		return InText;
	}
	FString Out;
	Out.Reserve(InText.Len());
	for (int32 i = 0; i < InText.Len(); ++i)
	{
		if (InText[i] == 0x1B)
		{
			i += 3;
			continue;
		}
		Out.AppendChar(InText[i]);
	}
	return Out;
}

static TArray<FColourRun> SplitColourRuns(const FString& InText)
{
	TArray<FColourRun> Runs;
	Runs.AddDefaulted();
	for (int32 i = 0; i < InText.Len(); ++i)
	{
		if (InText[i] == 0x1B)
		{
			if (i + 3 < InText.Len())
			{
				FColourRun Run;
				Run.bColour = true;
				Run.Colour = FColor(uint8(InText[i + 1] & 0xFF), uint8(InText[i + 2] & 0xFF), uint8(InText[i + 3] & 0xFF));
				Runs.Add(Run);
			}
			i += 3;
			continue;
		}
		Runs.Last().Text.AppendChar(InText[i]);
	}
	return Runs;
}
}
using namespace ArenaHUDLocal;

FVector2D FArenaDraw::TextSize(const FString& InTextRaw, float SizePx) const
{
	const FString InText = StripColourCodes(InTextRaw);
	UFont* Font = GetFont();
	if (!Canvas || !Font || InText.IsEmpty())
	{
		return FVector2D::ZeroVector;
	}
	const float Sc = HudFontScaleFor(Font, SizePx);
	float XL = 0.f, YL = 0.f;
	Canvas->TextSize(Font, InText, XL, YL, Sc, Sc);
	return FVector2D(XL, YL);
}

float FArenaDraw::Text(const FString& InTextRaw, float X, float Y, const FLinearColor& Color, float SizePx, EArenaAlign Align, bool bShadow) const
{
	const FString InText = StripColourCodes(InTextRaw);
	UFont* Font = GetFont();
	if (!Canvas || !Font || InText.IsEmpty() || Color.A <= 0.f)
	{
		return 0.f;
	}
	const float Sc = HudFontScaleFor(Font, SizePx);
	float XL = 0.f, YL = 0.f;
	Canvas->TextSize(Font, InText, XL, YL, Sc, Sc);

	float DrawX = X;
	if (Align == EArenaAlign::Center)
	{
		DrawX -= XL * 0.5f;
	}
	else if (Align == EArenaAlign::Right)
	{
		DrawX -= XL;
	}

	FCanvasTextItem Item(FVector2D(FMath::RoundToFloat(DrawX), FMath::RoundToFloat(Y)), FText::FromString(InText), Font, Color);
	Item.Scale = FVector2D(Sc, Sc);
	Item.BlendMode = SE_BLEND_Translucent;
	if (bShadow)
	{
		const float Off = FMath::Max(1.f, SizePx / 22.f);
		Item.EnableShadow(FLinearColor(0.f, 0.f, 0.f, 0.75f * Color.A), FVector2D(Off, Off));
	}
	Canvas->DrawItem(Item);
	return XL;
}

float FArenaDraw::TextMid(const FString& InText, float X, float Y, const FLinearColor& Color, float SizePx, EArenaAlign Align, bool bShadow) const
{
	const FVector2D Size = TextSize(InText, SizePx);
	return Text(InText, X, Y - Size.Y * 0.5f, Color, SizePx, Align, bShadow);
}

float FArenaDraw::UTFontScale() const
{
	return float(FMath::Max(1, FMath::RoundToInt(H() / 1080.f)));
}

float FArenaDraw::UTClipX() const
{
	return FMath::Min(W(), H() * 4.f / 3.f) / UTFontScale();
}

FVector2D FArenaDraw::TextSizeUT(int32 FontIndex, const FString& InTextRaw) const
{
	const FString InText = StripColourCodes(InTextRaw);
	const float K = UTFontScale();
	// the font resampled to K times its size (drawn 1:1); metrics are its own
	UFont* Font = ArenaHudFonts::GetScaled(FontIndex, K);
	if (!Font)
	{
		return InText.IsEmpty() ? FVector2D::ZeroVector : TextSize(InText, float(ArenaHudFonts::NominalHeight(FontIndex)) * K);
	}
	// UE2 Canvas metrics: glyph cell widths plus the font's kerning between glyphs, the font's cell height
	float W = 0.f;
	int32 N = 0;
	for (const TCHAR C : InText)
	{
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
	return FVector2D(W, Font->GetMaxCharHeight());
}

float FArenaDraw::TextUT(int32 FontIndex, const FString& InText, float X, float Y, const FLinearColor& Color, EArenaAlign Align, bool bShadow) const
{
	const float K = UTFontScale();
	UFont* Font = ArenaHudFonts::GetScaled(FontIndex, K);
	if (!Font)
	{
		return Text(InText, X, Y, Color, float(ArenaHudFonts::NominalHeight(FontIndex)) * K, Align, bShadow);
	}
	if (!Canvas || InText.IsEmpty() || Color.A <= 0.f)
	{
		return 0.f;
	}
	const FVector2D Size = TextSizeUT(FontIndex, InText);
	float DrawX = X;
	if (Align == EArenaAlign::Center)
	{
		DrawX -= Size.X * 0.5f;
	}
	else if (Align == EArenaAlign::Right)
	{
		DrawX -= Size.X;
	}
	if (HasColourCodes(InText))
	{
		// colour runs: each part in its own colour (the caller's alpha), one after the other with the font's kerning
		float RunX = DrawX;
		FLinearColor RunColor = Color;
		for (const FColourRun& Run : SplitColourRuns(InText))
		{
			if (Run.bColour)
			{
				RunColor = FLinearColor(Run.Colour);
				RunColor.A = Color.A;
			}
			if (Run.Text.IsEmpty())
			{
				continue;
			}
			FCanvasTextItem RunItem(FVector2D(FMath::RoundToFloat(RunX), FMath::RoundToFloat(Y)), FText::FromString(Run.Text), Font, RunColor);
			RunItem.Scale = FVector2D(1.f, 1.f);
			RunItem.BlendMode = SE_BLEND_Translucent;
			if (bShadow)
			{
				RunItem.EnableShadow(FLinearColor(0.f, 0.f, 0.f, 0.75f * Color.A), FVector2D(K, K));
			}
			Canvas->DrawItem(RunItem);
			RunX += TextSizeUT(FontIndex, Run.Text).X + float(Font->Kerning);
		}
		return Size.X;
	}
	FCanvasTextItem Item(FVector2D(FMath::RoundToFloat(DrawX), FMath::RoundToFloat(Y)), FText::FromString(InText), Font, Color);
	Item.Scale = FVector2D(1.f, 1.f);
	Item.BlendMode = SE_BLEND_Translucent;
	if (bShadow)
	{
		Item.EnableShadow(FLinearColor(0.f, 0.f, 0.f, 0.75f * Color.A), FVector2D(K, K));
	}
	Canvas->DrawItem(Item);
	return Size.X;
}

FLinearColor FArenaDraw::UTColor(const FColor& Color, float AlphaScale)
{
	FLinearColor C(Color);
	C.A = FMath::Clamp(float(Color.A) / 255.f * AlphaScale, 0.f, 1.f);
	return C;
}

void FArenaDraw::Rect(float X, float Y, float Width, float Height, const FLinearColor& Color) const
{
	if (!Canvas || Width <= 0.f || Height <= 0.f || Color.A <= 0.f)
	{
		return;
	}
	FCanvasTileItem Tile(FVector2D(X, Y), FVector2D(Width, Height), Color);
	Tile.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Tile);
}

void FArenaDraw::Outline(float X, float Y, float Width, float Height, const FLinearColor& Color, float Thickness) const
{
	const float T = FMath::Max(1.f, Thickness);
	Rect(X, Y, Width, T, Color);
	Rect(X, Y + Height - T, Width, T, Color);
	Rect(X, Y + T, T, Height - 2.f * T, Color);
	Rect(X + Width - T, Y + T, T, Height - 2.f * T, Color);
}

void FArenaDraw::Panel(float X, float Y, float Width, float Height, const FLinearColor& Accent, float Alpha) const
{
	if (ArenaArtHUD::DrawHudPanel(*this, X, Y, Width, Height, Accent, Alpha))
	{
		return;
	}
	Rect(X, Y, Width, Height, FLinearColor(0.02f, 0.025f, 0.04f, 0.62f * Alpha));
	// faint top highlight for depth
	Rect(X, Y, Width, FMath::Max(1.f, Height * 0.45f), FLinearColor(1.f, 1.f, 1.f, 0.025f * Alpha));
	Outline(X, Y, Width, Height, FLinearColor(0.6f, 0.75f, 1.f, 0.16f * Alpha), 1.f);
	if (Accent.A > 0.f)
	{
		Rect(X, Y, FMath::Max(2.f, S(4.f)), Height, FLinearColor(Accent.R, Accent.G, Accent.B, Accent.A * Alpha));
	}
}

void FArenaDraw::Bar(float X, float Y, float Width, float Height, float Fraction, const FLinearColor& Color, float BgAlpha) const
{
	Rect(X, Y, Width, Height, FLinearColor(Color.R * 0.25f, Color.G * 0.25f, Color.B * 0.25f, BgAlpha * Color.A));
	const float F = FMath::Clamp(Fraction, 0.f, 1.f);
	if (F > 0.f)
	{
		Rect(X, Y, Width * F, Height, Color);
		// bright leading edge
		Rect(X + Width * F - FMath::Max(1.f, S(2.f)), Y, FMath::Max(1.f, S(2.f)), Height, FLinearColor(1.f, 1.f, 1.f, 0.5f * Color.A));
	}
}

namespace ArenaHUDLocal
{
static void HudAddQuad(TArray<FCanvasUVTri>& Tris, const FVector2D& A, const FVector2D& B, const FVector2D& C, const FVector2D& Dp, const FLinearColor& Color)
{
	FCanvasUVTri T1;
	T1.V0_Pos = A; T1.V1_Pos = B; T1.V2_Pos = C;
	T1.V0_UV = T1.V1_UV = T1.V2_UV = FVector2D(0.5f, 0.5f);
	T1.V0_Color = T1.V1_Color = T1.V2_Color = Color;
	Tris.Add(T1);

	FCanvasUVTri T2;
	T2.V0_Pos = A; T2.V1_Pos = C; T2.V2_Pos = Dp;
	T2.V0_UV = T2.V1_UV = T2.V2_UV = FVector2D(0.5f, 0.5f);
	T2.V0_Color = T2.V1_Color = T2.V2_Color = Color;
	Tris.Add(T2);
}

static void HudDrawTris(UCanvas* Canvas, const TArray<FCanvasUVTri>& Tris)
{
	if (!Canvas || Tris.Num() == 0 || !Canvas->DefaultTexture)
	{
		return;
	}
	const FTexture* Tex = Canvas->DefaultTexture->GetResource();
	if (!Tex)
	{
		return;
	}
	FCanvasTriangleItem Item(Tris, Tex);
	Item.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Item);
}
}

void FArenaDraw::Line(float X0, float Y0, float X1, float Y1, const FLinearColor& Color, float Thickness) const
{
	const FVector2D A(X0, Y0), B(X1, Y1);
	FVector2D Dir = B - A;
	if (Dir.SizeSquared() < KINDA_SMALL_NUMBER || Color.A <= 0.f)
	{
		return;
	}
	Dir.Normalize();
	const FVector2D N = FVector2D(-Dir.Y, Dir.X) * (FMath::Max(1.f, Thickness) * 0.5f);
	TArray<FCanvasUVTri> Tris;
	HudAddQuad(Tris, A + N, B + N, B - N, A - N, Color);
	HudDrawTris(Canvas, Tris);
}

void FArenaDraw::Arc(float CX, float CY, float InnerRadius, float OuterRadius, float StartAngle, float EndAngle, const FLinearColor& Color, int32 Segments) const
{
	if (Color.A <= 0.f || OuterRadius <= 0.f)
	{
		return;
	}
	Segments = FMath::Clamp(Segments, 1, 128);
	auto P = [CX, CY](float Angle, float R)
	{
		return FVector2D(CX + FMath::Sin(Angle) * R, CY - FMath::Cos(Angle) * R);
	};
	TArray<FCanvasUVTri> Tris;
	Tris.Reserve(Segments * 2);
	for (int32 i = 0; i < Segments; ++i)
	{
		const float A0 = FMath::Lerp(StartAngle, EndAngle, float(i) / Segments);
		const float A1 = FMath::Lerp(StartAngle, EndAngle, float(i + 1) / Segments);
		HudAddQuad(Tris, P(A0, OuterRadius), P(A1, OuterRadius), P(A1, InnerRadius), P(A0, InnerRadius), Color);
	}
	HudDrawTris(Canvas, Tris);
}

void FArenaDraw::VerticalGradient(float X, float Y, float Width, float Height, const FLinearColor& Top, const FLinearColor& Bottom, int32 Steps) const
{
	Steps = FMath::Max(1, Steps);
	const float StepH = Height / Steps;
	for (int32 i = 0; i < Steps; ++i)
	{
		const float Alpha = (i + 0.5f) / Steps;
		Rect(X, Y + i * StepH, Width, StepH + 0.5f, FMath::Lerp(Top, Bottom, Alpha));
	}
}

// =============================================================================================
// AArenaHUD helpers
// =============================================================================================

namespace ArenaHUDLocal
{
	const FLinearColor HudTextWhite(0.94f, 0.96f, 1.f, 1.f);
	const FLinearColor HudTextDim(0.62f, 0.67f, 0.75f, 1.f);
	const FLinearColor HudHealthColor(0.35f, 1.f, 0.55f, 1.f);
	const FLinearColor HudHealthLowColor(1.f, 0.25f, 0.2f, 1.f);
	const FLinearColor HudSuperHealthColor(0.3f, 0.75f, 1.f, 1.f);
	const FLinearColor HudShieldColor(1.f, 0.78f, 0.22f, 1.f);
	const FLinearColor HudAdrenalineColor(0.75f, 0.45f, 1.f, 1.f);
	const FLinearColor HudDoubleDamageColor(0.95f, 0.3f, 1.f, 1.f);
	const FLinearColor HudAccentBlue(0.25f, 0.6f, 1.f, 1.f);

	FLinearColor HudWithAlpha(const FLinearColor& C, float A)
	{
		return FLinearColor(C.R, C.G, C.B, C.A * A);
	}

	FString HudFormatTime(int32 Seconds)
	{
		Seconds = FMath::Max(0, Seconds);
		return FString::Printf(TEXT("%d:%02d"), Seconds / 60, Seconds % 60);
	}

	FString HudComboName(EArenaCombo Combo)
	{
		switch (Combo)
		{
		case EArenaCombo::Speed:     return TEXT("SPEED");
		case EArenaCombo::Booster:   return TEXT("REGEN");
		case EArenaCombo::Invisible: return TEXT("CLOAK");
		case EArenaCombo::Berserk:   return TEXT("BERSERK");
		default:                     return FString();
		}
	}

	int32 HudTeamOf(const APlayerState* PS)
	{
		const AArenaPlayerState* APS = Cast<AArenaPlayerState>(PS);
		return APS ? APS->Team : ArenaTeam::None;
	}

	FLinearColor HudTeamTint(int32 Team, float Brightness = 1.f)
	{
		const FLinearColor C = ArenaTeam::Color(Team);
		return FLinearColor(FMath::Min(1.f, C.R * Brightness), FMath::Min(1.f, C.G * Brightness), FMath::Min(1.f, C.B * Brightness), 1.f);
	}

	/** Lighter version of a team colour that stays readable as text on dark panels. */
	FLinearColor HudTeamTextColor(int32 Team)
	{
		if (Team != ArenaTeam::Red && Team != ArenaTeam::Blue)
		{
			return HudTextWhite;
		}
		return FMath::Lerp(ArenaTeam::Color(Team), FLinearColor::White, 0.35f);
	}
}

FLinearColor ArenaHudStyle::TextWhite() { return HudTextWhite; }
FLinearColor ArenaHudStyle::TextDim() { return HudTextDim; }
FLinearColor ArenaHudStyle::HealthColor() { return HudHealthColor; }
FLinearColor ArenaHudStyle::ShieldColor() { return HudShieldColor; }
FLinearColor ArenaHudStyle::AccentBlue() { return HudAccentBlue; }
FLinearColor ArenaHudStyle::TeamTint(int32 Team, float Brightness) { return HudTeamTint(Team, Brightness); }
FLinearColor ArenaHudStyle::TeamTextColor(int32 Team) { return HudTeamTextColor(Team); }
FLinearColor ArenaHudStyle::WithAlpha(const FLinearColor& Color, float Alpha) { return HudWithAlpha(Color, Alpha); }
FString ArenaHudStyle::FormatTime(int32 Seconds) { return HudFormatTime(Seconds); }

AArenaHUD::AArenaHUD()
{
}

UArenaGameRules* AArenaHUD::GetRules() const
{
	const AArenaGameState* GS = GetArenaGS();
	return GS ? GS->GetRules() : nullptr;
}

AArenaPlayerController* AArenaHUD::GetArenaPC() const
{
	return Cast<AArenaPlayerController>(PlayerOwner);
}

AArenaGameState* AArenaHUD::GetArenaGS() const
{
	return GetWorld() ? GetWorld()->GetGameState<AArenaGameState>() : nullptr;
}

float AArenaHUD::Now() const
{
	return GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
}

// =============================================================================================
// DrawHUD
// =============================================================================================

void AArenaHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}
	D = FArenaDraw(Canvas);
	UpdateTestPages();

	AArenaPlayerController* PC = GetArenaPC();
	AArenaGameState* GS = GetArenaGS();
	AArenaCharacter* Char = Cast<AArenaCharacter>(GetOwningPawn());
	const bool bAlive = Char && Char->IsAlive();
	const bool bEnded = GS && GS->Phase == EArenaMatchPhase::Ended;
	const bool bScoreboard = (PC && PC->IsScoreboardVisible()) || bTestScoreboard;
	const bool bPaused = (PC && PC->IsPauseMenuOpen()) || bTestPause || bTestSettings;

	// Flying a guided warhead: its scope (RedeemerWarhead.DrawHUD) under the rest of the HUD
	if (const AArenaGuidedWarhead* Warhead = Cast<AArenaGuidedWarhead>(GetOwningPawn()))
	{
		ArenaHudUT::DrawWarheadScope(D, Warhead->GetBankingDeg(), Warhead->GetClimbRate(), Warhead->IsStaticScreen(), GetWorld()->GetTimeSeconds());
	}

	// Looking through a translocator beacon: its camera overlay (TransLauncher.RenderOverlays) under the HUD
	ArenaTranslocator::DrawCameraOverlay(D, PC);

	// UT2004's HUD (ArenaHUDUT.cpp) when its art is in the content; the procedural HUD below otherwise
	bUTHud = ArenaHudUT::HasArt();
	if (bUTHud)
	{
		DrawUTHud(Char, bAlive, bEnded, bScoreboard);
		if (bEnded)
		{
			DrawEndBanner();
		}
		if (bPaused)
		{
			DrawPauseMenu();
		}
		return;
	}

	if (bAlive && !bEnded)
	{
		DrawDamageIndicators(Char);
		if (!bScoreboard)
		{
			DrawCrosshair(Char);
		}
		DrawVitals(Char);
		DrawAmmo(Char);
		DrawWeaponBar(Char);
		DrawPowerups(Char);
		DrawPickupMessage();
	}
	else if (!bEnded && PC && (Char || !GetOwningPawn())) // a non-character pawn (e.g. an Onslaught vehicle) is not dead
	{
		const UArenaGameRules* DeadRules = GetRules();
		if (!DeadRules || DeadRules->ShouldDrawDeadScreen(PC))
		{
			DrawDeadScreen();
		}
	}

	DrawKillFeed();
	DrawScoreInfo();
	DrawTimer();
	if (UArenaGameRules* Rules = GetRules())
	{
		Rules->DrawHUD(this, Canvas);
	}

	if (bScoreboard)
	{
		DrawScoreboard();
	}
	else
	{
		DrawCenterMessages();
		DrawLocalMessages();
	}

	if (bEnded)
	{
		DrawEndBanner();
	}

	if (bPaused)
	{
		DrawPauseMenu();
	}
}

// =============================================================================================
// Vitals / ammo / weapons
// =============================================================================================

void AArenaHUD::DrawVitals(const AArenaCharacter* Char)
{
	const float PW = D.S(340.f);
	const float PH = D.S(112.f);
	const float X = D.S(24.f);
	const float Y = D.H() - D.S(24.f) - PH;
	const int32 Team = HudTeamOf(PlayerOwner ? PlayerOwner->PlayerState : nullptr);
	const FLinearColor Accent = (Team >= 0) ? HudTeamTint(Team) : HudAccentBlue;

	D.Panel(X, Y, PW, PH, Accent);

	// ---- health ----
	const float Health = FMath::Max(0.f, Char->Health);
	FLinearColor HCol = Health > ArenaTuning::MaxHealth ? HudSuperHealthColor : (Health <= 25.f ? HudHealthLowColor : HudHealthColor);
	if (Health <= 25.f)
	{
		const float Pulse = 0.65f + 0.35f * FMath::Sin(Now() * 8.f);
		HCol = HudWithAlpha(HCol, Pulse);
	}
	const float HX = X + D.S(20.f);
	D.Text(TEXT("HEALTH"), HX, Y + D.S(10.f), HudTextDim, D.S(15.f));
	// small medical cross icon
	{
		const float IX = HX + D.S(124.f), IY = Y + D.S(12.f), Sz = D.S(14.f), T = D.S(4.f);
		D.Rect(IX + (Sz - T) * 0.5f, IY, T, Sz, HudWithAlpha(HCol, 0.8f));
		D.Rect(IX, IY + (Sz - T) * 0.5f, Sz, T, HudWithAlpha(HCol, 0.8f));
	}
	D.Text(FString::FromInt(FMath::CeilToInt(Health)), HX, Y + D.S(28.f), HCol, D.S(46.f));
	const float BarW = D.S(140.f);
	const float BarY = Y + D.S(86.f);
	D.Bar(HX, BarY, BarW, D.S(8.f), FMath::Min(Health, ArenaTuning::MaxHealth) / ArenaTuning::MaxHealth,
		Health > ArenaTuning::MaxHealth ? HudHealthColor : HCol);
	if (Health > ArenaTuning::MaxHealth)
	{
		const float Over = (Health - ArenaTuning::MaxHealth) / (ArenaTuning::SuperHealthMax - ArenaTuning::MaxHealth);
		D.Rect(HX, BarY + D.S(1.5f), BarW * FMath::Clamp(Over, 0.f, 1.f), D.S(5.f), HudSuperHealthColor);
	}

	// ---- shield ----
	const float Shield = FMath::Max(0.f, Char->Shield);
	const float SX = X + D.S(184.f);
	const FLinearColor SCol = Shield > 0.f ? HudShieldColor : HudWithAlpha(HudTextDim, 0.6f);
	D.Text(TEXT("SHIELD"), SX, Y + D.S(10.f), HudTextDim, D.S(15.f));
	// small shield icon (stacked bars narrowing downward)
	{
		const float IX = SX + D.S(114.f), IY = Y + D.S(12.f);
		D.Rect(IX, IY, D.S(14.f), D.S(5.f), HudWithAlpha(SCol, 0.8f));
		D.Rect(IX + D.S(2.f), IY + D.S(5.f), D.S(10.f), D.S(5.f), HudWithAlpha(SCol, 0.8f));
		D.Rect(IX + D.S(4.f), IY + D.S(10.f), D.S(6.f), D.S(4.f), HudWithAlpha(SCol, 0.8f));
	}
	D.Text(FString::FromInt(FMath::CeilToInt(Shield)), SX, Y + D.S(28.f), SCol, D.S(46.f));
	D.Bar(SX, BarY, BarW, D.S(8.f), Shield / ArenaTuning::MaxShield, HudShieldColor);

	// ---- adrenaline (panel above) ----
	const float AH = D.S(40.f);
	const float AY = Y - D.S(8.f) - AH;
	const float Adrenaline = FMath::Clamp(Char->Adrenaline, 0.f, ArenaTuning::MaxAdrenaline);
	const EArenaCombo Combo = Char->GetActiveCombo();
	D.Panel(X, AY, PW, AH, HudAdrenalineColor);

	if (Combo != EArenaCombo::None)
	{
		const float Remaining = Char->GetComboRemaining();
		const float Frac = Remaining / AArenaCharacter::GetComboDuration(Char->GetActiveCombo());
		const float Pulse = 0.75f + 0.25f * FMath::Sin(Now() * 5.f);
		D.TextMid(HudComboName(Combo), X + D.S(20.f), AY + AH * 0.5f, HudWithAlpha(HudAdrenalineColor, Pulse), D.S(18.f));
		D.Bar(X + D.S(120.f), AY + AH * 0.5f - D.S(4.f), D.S(150.f), D.S(8.f), Frac, HudAdrenalineColor);
		D.TextMid(FString::Printf(TEXT("%ds"), FMath::CeilToInt(Remaining)), X + PW - D.S(14.f), AY + AH * 0.5f, HudTextWhite, D.S(18.f), EArenaAlign::Right);
	}
	else
	{
		const bool bFull = Adrenaline >= ArenaTuning::MaxAdrenaline;
		const float Pulse = bFull ? (0.55f + 0.45f * (0.5f + 0.5f * FMath::Sin(Now() * 7.f))) : 1.f;
		const FLinearColor ACol = HudWithAlpha(bFull ? FLinearColor(0.95f, 0.75f, 1.f) : HudAdrenalineColor, Pulse);
		D.TextMid(bFull ? TEXT("COMBO READY") : TEXT("ADRENALINE"), X + D.S(20.f), AY + AH * 0.5f, bFull ? ACol : HudTextDim, D.S(15.f));
		D.Bar(X + D.S(140.f), AY + AH * 0.5f - D.S(4.f), D.S(140.f), D.S(8.f), Adrenaline / ArenaTuning::MaxAdrenaline, ACol);
		D.TextMid(FString::FromInt(FMath::FloorToInt(Adrenaline)), X + PW - D.S(14.f), AY + AH * 0.5f, ACol, D.S(18.f), EArenaAlign::Right);
		if (bFull)
		{
			D.Outline(X, AY, PW, AH, HudWithAlpha(HudAdrenalineColor, Pulse * 0.8f), D.S(2.f));
		}
	}
}

void AArenaHUD::DrawAmmo(const AArenaCharacter* Char)
{
	const AArenaWeapon* Weapon = Char->GetCurrentWeapon();
	if (!Weapon)
	{
		return;
	}
	const float PW = D.S(290.f);
	const float PH = D.S(112.f);
	const float X = D.W() - D.S(24.f) - PW;
	const float Y = D.H() - D.S(24.f) - PH;
	const FLinearColor WCol = FMath::Lerp(Weapon->WeaponColor, FLinearColor::White, 0.25f);

	D.Panel(X, Y, PW, PH, FLinearColor(WCol.R, WCol.G, WCol.B, 1.f));
	D.Text(Weapon->WeaponName.ToUpper(), X + D.S(20.f), Y + D.S(10.f), HudTextDim, D.S(15.f));

	if (Weapon->bInfiniteAmmo)
	{
		D.Text(TEXT("--"), X + PW - D.S(18.f), Y + D.S(28.f), HudTextWhite, D.S(46.f), EArenaAlign::Right);
		D.Bar(X + D.S(20.f), Y + D.S(86.f), PW - D.S(38.f), D.S(8.f), 1.f, WCol);
		return;
	}

	const float Frac = Weapon->GetAmmoFraction();
	const FLinearColor ACol = Weapon->Ammo <= 0 ? HudHealthLowColor : (Frac < 0.2f ? FLinearColor(1.f, 0.6f, 0.25f) : HudTextWhite);
	const FString MaxStr = FString::Printf(TEXT("/ %d"), Weapon->MaxAmmo);
	const float MaxW = D.TextSize(MaxStr, D.S(18.f)).X;
	D.Text(MaxStr, X + PW - D.S(18.f), Y + D.S(54.f), HudTextDim, D.S(18.f), EArenaAlign::Right);
	D.Text(FString::FromInt(Weapon->Ammo), X + PW - D.S(26.f) - MaxW, Y + D.S(28.f), ACol, D.S(46.f), EArenaAlign::Right);
	D.Bar(X + D.S(20.f), Y + D.S(86.f), PW - D.S(38.f), D.S(8.f), Frac, WCol);
}

void AArenaHUD::DrawWeaponBar(const AArenaCharacter* Char)
{
	struct FSlotInfo
	{
		int32 Slot = 0;
		const AArenaWeapon* Owned = nullptr;
		FLinearColor Color = FLinearColor::Gray;
	};

	const AArenaGameState* GS = GetArenaGS();
	const bool bInstagib = GS && GS->bInstagib;
	const AArenaWeapon* Current = Char->GetCurrentWeapon();

	TArray<FSlotInfo> Slots;
	if (bInstagib)
	{
		for (const AArenaWeapon* W : Char->GetWeapons())
		{
			if (W)
			{
				FSlotInfo Info;
				Info.Slot = W->Slot;
				Info.Owned = W;
				Info.Color = W->WeaponColor;
				Slots.Add(Info);
			}
		}
	}
	else
	{
		const TArray<TSubclassOf<AArenaWeapon>> All = ArenaWeaponList::AllWeapons();
		for (int32 SlotIdx = 1; SlotIdx <= 9; ++SlotIdx)
		{
			FSlotInfo Info;
			Info.Slot = SlotIdx;
			for (const AArenaWeapon* W : Char->GetWeapons())
			{
				if (W && W->Slot == SlotIdx)
				{
					Info.Owned = W;
					Info.Color = W->WeaponColor;
					break;
				}
			}
			if (!Info.Owned)
			{
				for (const TSubclassOf<AArenaWeapon>& Cls : All)
				{
					const AArenaWeapon* CDO = Cls ? Cls->GetDefaultObject<AArenaWeapon>() : nullptr;
					if (CDO && CDO->Slot == SlotIdx)
					{
						Info.Color = CDO->WeaponColor;
						break;
					}
				}
			}
			Slots.Add(Info);
		}
		// Extra weapons outside 1..9 (e.g. slot 0) that the player owns
		for (const AArenaWeapon* W : Char->GetWeapons())
		{
			if (W && (W->Slot < 1 || W->Slot > 9))
			{
				FSlotInfo Info;
				Info.Slot = W->Slot;
				Info.Owned = W;
				Info.Color = W->WeaponColor;
				Slots.Add(Info);
			}
		}
	}
	if (Slots.Num() == 0)
	{
		return;
	}

	const float BW = D.S(56.f);
	const float BH = D.S(44.f);
	const float Gap = D.S(6.f);
	const float Total = Slots.Num() * BW + (Slots.Num() - 1) * Gap;
	float X = (D.W() - Total) * 0.5f;
	const float BaseY = D.H() - D.S(24.f) - BH;

	for (const FSlotInfo& Info : Slots)
	{
		const bool bSelected = Info.Owned && Info.Owned == Current;
		const float Y = bSelected ? BaseY - D.S(8.f) : BaseY;
		const FLinearColor Col = FMath::Lerp(Info.Color, FLinearColor::White, 0.2f);

		if (Info.Owned)
		{
			D.Rect(X, Y, BW, BH, bSelected ? FLinearColor(Col.R * 0.35f, Col.G * 0.35f, Col.B * 0.35f, 0.8f) : FLinearColor(0.02f, 0.025f, 0.04f, 0.6f));
			D.Rect(X, Y, BW, D.S(4.f), FLinearColor(Col.R, Col.G, Col.B, bSelected ? 1.f : 0.75f));
			const bool bEmpty = !Info.Owned->bInfiniteAmmo && Info.Owned->Ammo <= 0;
			const FLinearColor NumCol = bEmpty ? HudWithAlpha(HudHealthLowColor, 0.8f) : (bSelected ? HudTextWhite : HudWithAlpha(HudTextWhite, 0.8f));
			const int32 KeyNum = Info.Slot == 10 ? 0 : Info.Slot;
			D.TextMid(FString::FromInt(KeyNum), X + BW * 0.5f, Y + BH * 0.5f, NumCol, D.S(bSelected ? 22.f : 18.f), EArenaAlign::Center);
			const float Frac = Info.Owned->bInfiniteAmmo ? 1.f : Info.Owned->GetAmmoFraction();
			D.Bar(X + D.S(5.f), Y + BH - D.S(8.f), BW - D.S(10.f), D.S(4.f), Frac, FLinearColor(Col.R, Col.G, Col.B, 0.9f), 0.5f);
			D.Outline(X, Y, BW, BH, bSelected ? FLinearColor(1.f, 1.f, 1.f, 0.85f) : FLinearColor(0.6f, 0.75f, 1.f, 0.18f), bSelected ? D.S(2.f) : 1.f);
		}
		else
		{
			D.Rect(X, Y, BW, BH, FLinearColor(0.02f, 0.025f, 0.04f, 0.3f));
			D.Rect(X, Y, BW, D.S(3.f), FLinearColor(Col.R, Col.G, Col.B, 0.18f));
			D.TextMid(FString::FromInt(Info.Slot), X + BW * 0.5f, Y + BH * 0.5f, HudWithAlpha(HudTextDim, 0.35f), D.S(16.f), EArenaAlign::Center, false);
			D.Outline(X, Y, BW, BH, FLinearColor(0.6f, 0.75f, 1.f, 0.07f), 1.f);
		}
		X += BW + Gap;
	}
}

// =============================================================================================
// Crosshair / damage / powerups
// =============================================================================================

void AArenaHUD::DrawCrosshair(const AArenaCharacter* Char)
{
	const float CX = FMath::RoundToFloat(D.W() * 0.5f);
	const float CY = FMath::RoundToFloat(D.H() * 0.5f);
	const float T = FMath::Max(2.f, FMath::RoundToFloat(D.S(2.f)));
	const float Gap = D.S(6.f);
	const float Len = D.S(9.f);
	const FLinearColor Outline(0.f, 0.f, 0.f, 0.6f);

	FLinearColor Col(0.85f, 1.f, 0.92f, 0.92f);
	if (const AArenaWeapon* W = Char->GetCurrentWeapon())
	{
		Col = FMath::Lerp(FLinearColor(0.9f, 1.f, 0.95f, 0.92f), FLinearColor(W->WeaponColor.R, W->WeaponColor.G, W->WeaponColor.B, 0.92f), 0.2f);
	}

	// four ticks with a gap plus a centre dot (the UT2004 HUD draws the crosshair textures, ArenaHUDUT.cpp)
	{
		auto Bar = [&](float X, float Y, float W, float H)
		{
			D.Rect(X - 1.f, Y - 1.f, W + 2.f, H + 2.f, Outline);
			D.Rect(X, Y, W, H, Col);
		};
		Bar(CX - Gap - Len, CY - T * 0.5f, Len, T);
		Bar(CX + Gap, CY - T * 0.5f, Len, T);
		Bar(CX - T * 0.5f, CY - Gap - Len, T, Len);
		Bar(CX - T * 0.5f, CY + Gap, T, Len);
		Bar(CX - T * 0.5f, CY - T * 0.5f, T, T);
	}

	// hit marker
	if (const AArenaPlayerController* PC = GetArenaPC())
	{
		const float Age = Now() - PC->GetLastHitEnemyTime();
		const float Dur = 0.3f;
		if (Age >= 0.f && Age < Dur)
		{
			const float A = 1.f - Age / Dur;
			const bool bHead = PC->WasLastHitHeadshot();
			const FLinearColor HC = bHead ? FLinearColor(1.f, 0.25f, 0.2f, A) : FLinearColor(1.f, 1.f, 1.f, A);
			const float R0 = D.S(9.f) + Age * D.S(20.f);
			const float R1 = R0 + D.S(bHead ? 12.f : 9.f);
			const float Thick = FMath::Max(2.f, D.S(2.5f));
			for (int32 i = 0; i < 4; ++i)
			{
				const float Ang = PI * 0.25f + i * PI * 0.5f;
				const float SX = FMath::Sin(Ang), SY = -FMath::Cos(Ang);
				D.Line(CX + SX * R0, CY + SY * R0, CX + SX * R1, CY + SY * R1, HC, Thick);
			}
		}
	}
}

void AArenaHUD::DrawDamageIndicators(const AArenaCharacter* Char)
{
	const AArenaPlayerController* PC = GetArenaPC();
	if (!PC)
	{
		return;
	}
	FVector ViewLoc;
	FRotator ViewRot;
	PC->GetPlayerViewPoint(ViewLoc, ViewRot);
	const FRotator YawOnly(0.f, ViewRot.Yaw, 0.f);

	const float CX = D.W() * 0.5f;
	const float CY = D.H() * 0.5f;
	const float T = Now();

	for (const AArenaPlayerController::FDamageIndicator& Ind : PC->GetDamageIndicators())
	{
		const float Age = T - Ind.Time;
		const float Life = 1.5f;
		if (Age < 0.f || Age > Life)
		{
			continue;
		}
		FVector Dir = Ind.From - ViewLoc;
		Dir.Z = 0.f;
		if (Dir.SizeSquared() < 1.f)
		{
			// no direction (environment damage): flash a full ring faintly
			D.Arc(CX, CY, D.S(118.f), D.S(126.f), 0.f, 2.f * PI, FLinearColor(1.f, 0.1f, 0.05f, 0.35f * (1.f - Age / Life)), 48);
			continue;
		}
		const FVector Local = YawOnly.UnrotateVector(Dir);
		const float Angle = FMath::Atan2(Local.Y, Local.X); // 0 = ahead, + = right
		const float Strength = FMath::Clamp(Ind.Damage / 40.f, 0.45f, 1.f);
		const float A = (1.f - Age / Life) * Strength;
		const float Half = 0.38f;
		D.Arc(CX, CY, D.S(112.f), D.S(132.f), Angle - Half, Angle + Half, FLinearColor(1.f, 0.08f, 0.04f, 0.75f * A), 14);
		D.Arc(CX, CY, D.S(132.f), D.S(136.f), Angle - Half * 0.8f, Angle + Half * 0.8f, FLinearColor(1.f, 0.6f, 0.5f, 0.6f * A), 12);
	}
}

void AArenaHUD::DrawPowerups(const AArenaCharacter* Char)
{
	float Y = D.S(104.f);
	const float PW = D.S(260.f);
	const float PH = D.S(36.f);
	const float X = (D.W() - PW) * 0.5f;

	if (Char->HasDoubleDamage())
	{
		const float Remaining = Char->GetDoubleDamageRemaining();
		const float Pulse = Remaining < 5.f ? 0.6f + 0.4f * FMath::Sin(Now() * 10.f) : 1.f;
		D.Panel(X, Y, PW, PH, HudDoubleDamageColor);
		const bool bIcon = ArenaArtHUD::DrawSpriteCentered(D, ArenaArtHUD::Sprites::DamageIcon(), X + D.S(22.f), Y + PH * 0.5f, PH * 0.9f, HudWithAlpha(FLinearColor::White, Pulse));
		D.TextMid(TEXT("DOUBLE DAMAGE"), X + D.S(bIcon ? 44.f : 16.f), Y + PH * 0.5f, HudWithAlpha(HudDoubleDamageColor, Pulse), D.S(17.f));
		D.TextMid(FString::Printf(TEXT("%d"), FMath::CeilToInt(Remaining)), X + PW - D.S(14.f), Y + PH * 0.5f, HudTextWhite, D.S(20.f), EArenaAlign::Right);
		D.Rect(X, Y + PH - D.S(3.f), PW * FMath::Clamp(Remaining / ArenaTuning::DoubleDamageDuration, 0.f, 1.f), D.S(3.f), HudDoubleDamageColor);
		Y += PH + D.S(6.f);
	}

	if (Char->CarriedFlag)
	{
		const int32 FlagTeam = Char->CarriedFlag->GetTeam();
		const FLinearColor FC = HudTeamTextColor(FlagTeam);
		const float Pulse = 0.7f + 0.3f * FMath::Sin(Now() * 6.f);
		D.Panel(X, Y, PW, PH, HudTeamTint(FlagTeam));
		D.TextMid(TEXT("YOU HAVE THE FLAG"), X + PW * 0.5f, Y + PH * 0.5f, HudWithAlpha(FC, Pulse), D.S(17.f), EArenaAlign::Center);
		Y += PH + D.S(6.f);
	}

	if (const AArenaCarryable* Carried = Char->GetCarriedObject())
	{
		const int32 ObjTeam = Carried->GetTeam();
		const FLinearColor FC = (ObjTeam == 0 || ObjTeam == 1) ? HudTeamTextColor(ObjTeam) : FLinearColor(1.f, 0.85f, 0.3f);
		const float Pulse = 0.7f + 0.3f * FMath::Sin(Now() * 6.f);
		D.Panel(X, Y, PW, PH, (ObjTeam == 0 || ObjTeam == 1) ? HudTeamTint(ObjTeam) : FLinearColor(1.f, 0.85f, 0.3f));
		D.TextMid(Carried->GetCarrierHudText(), X + PW * 0.5f, Y + PH * 0.5f, HudWithAlpha(FC, Pulse), D.S(17.f), EArenaAlign::Center);
		Y += PH + D.S(6.f);
	}

	if (Char->HasSpawnProtection())
	{
		D.TextMid(TEXT("SPAWN PROTECTED"), D.W() * 0.5f, D.H() * 0.5f + D.S(60.f), FLinearColor(0.6f, 0.9f, 1.f, 0.7f), D.S(15.f), EArenaAlign::Center);
	}
}

// =============================================================================================
// Kill feed / scores / timer / messages
// =============================================================================================

// DrawKillFeed: ArenaHUDMessages.cpp (UT2004 console kill lines)

void AArenaHUD::DrawScoreInfo()
{
	const AArenaGameState* GS = GetArenaGS();
	const APlayerState* MyPS = PlayerOwner ? PlayerOwner->PlayerState : nullptr;
	if (!GS || !MyPS)
	{
		return;
	}

	const float Top = D.S(24.f);
	const float Right = D.W() - D.S(24.f);
	ScoreInfoBottom = Top;

	UArenaGameRules* Rules = GetRules();
	if (Rules && Rules->DrawScoreInfo(this, Canvas))
	{
		return;
	}
	const FString GoalText = Rules ? Rules->GetGoalText(GS->GoalScore)
		: (GS->GoalScore > 0 ? FString::Printf(TEXT("First to %d"), GS->GoalScore) : FString());

	if (!GS->IsTeamGame())
	{
		const float PW = D.S(230.f);
		const float PH = D.S(100.f);
		const float X = Right - PW;
		D.Panel(X, Top, PW, PH, HudAccentBlue);

		const TArray<AArenaPlayerState*> Sorted = GS->GetSortedPlayers();
		const int32 MyScore = FMath::RoundToInt(MyPS->GetScore());
		const int32 Rank = FMath::Max(1, GS->GetRank(MyPS));

		int32 Spread = 0;
		if (Rank == 1)
		{
			int32 Best = MIN_int32;
			for (const AArenaPlayerState* P : Sorted)
			{
				if (P && P != MyPS)
				{
					Best = FMath::Max(Best, FMath::RoundToInt(P->GetScore()));
				}
			}
			Spread = (Best == MIN_int32) ? 0 : MyScore - Best;
		}
		else if (Sorted.Num() > 0 && Sorted[0])
		{
			Spread = MyScore - FMath::RoundToInt(Sorted[0]->GetScore());
		}

		D.Text(TEXT("FRAGS"), X + D.S(18.f), Top + D.S(10.f), HudTextDim, D.S(14.f));
		if (!ArenaArtHUD::DrawNumber(D, MyScore, X + D.S(18.f), Top + D.S(54.f), D.S(44.f), EArenaAlign::Left, HudTextWhite))
		{
			D.Text(FString::FromInt(MyScore), X + D.S(18.f), Top + D.S(26.f), HudTextWhite, D.S(48.f));
		}

		const FString RankStr = FString::Printf(TEXT("%d / %d"), Rank, FMath::Max(1, Sorted.Num()));
		D.Text(TEXT("RANK"), Right - D.S(16.f), Top + D.S(10.f), HudTextDim, D.S(14.f), EArenaAlign::Right);
		D.Text(RankStr, Right - D.S(16.f), Top + D.S(26.f), Rank == 1 ? FLinearColor(1.f, 0.85f, 0.3f) : HudTextWhite, D.S(22.f), EArenaAlign::Right);

		const FString SpreadStr = Spread > 0 ? FString::Printf(TEXT("+%d"), Spread) : FString::FromInt(Spread);
		const FLinearColor SpreadCol = Spread > 0 ? HudHealthColor : (Spread < 0 ? FLinearColor(1.f, 0.45f, 0.35f) : HudTextWhite);
		D.Text(TEXT("SPREAD"), Right - D.S(16.f), Top + D.S(54.f), HudTextDim, D.S(14.f), EArenaAlign::Right);
		D.Text(SpreadStr, Right - D.S(16.f), Top + D.S(70.f), SpreadCol, D.S(20.f), EArenaAlign::Right);

		ScoreInfoBottom = Top + PH;
		if (!GoalText.IsEmpty())
		{
			D.Text(GoalText, Right, Top + PH + D.S(6.f), HudTextDim, D.S(14.f), EArenaAlign::Right);
			ScoreInfoBottom += D.S(22.f);
		}
		return;
	}

	// ---- team games ----
	DrawTeamScorePanels(0.f, [](int32, float, float, float) {}, GoalText);
}

void AArenaHUD::DrawTeamScorePanels(float ExtraHeight, TFunctionRef<void(int32 Team, float X, float Top, float Width)> DrawExtra, const FString& GoalText)
{
	const AArenaGameState* GS = GetArenaGS();
	const APlayerState* MyPS = PlayerOwner ? PlayerOwner->PlayerState : nullptr;
	if (!GS)
	{
		return;
	}
	const float Top = D.S(24.f);
	const float Right = D.W() - D.S(24.f);
	const int32 MyTeam = HudTeamOf(MyPS);
	const float BW = D.S(128.f);
	const float BH = D.S(80.f + ExtraHeight);
	const float Gap = D.S(8.f);
	const float StartX = Right - (BW * 2.f + Gap);

	for (int32 Team = 0; Team < 2; ++Team)
	{
		const float X = StartX + Team * (BW + Gap);
		const FLinearColor TC = HudTeamTint(Team);
		D.Rect(X, Top, BW, BH, FLinearColor(TC.R * 0.18f, TC.G * 0.18f, TC.B * 0.18f, 0.7f));
		D.Rect(X, Top, BW, D.S(4.f), TC);
		D.Outline(X, Top, BW, BH, Team == MyTeam ? FLinearColor(1.f, 1.f, 1.f, 0.6f) : FLinearColor(TC.R, TC.G, TC.B, 0.3f), Team == MyTeam ? D.S(2.f) : 1.f);

		D.Text(ArenaTeam::Name(Team).ToUpper(), X + BW * 0.5f, Top + D.S(10.f), HudTeamTextColor(Team), D.S(14.f), EArenaAlign::Center);
		D.Text(FString::FromInt(GS->TeamScores[Team]), X + BW * 0.5f, Top + D.S(26.f), HudTextWhite, D.S(42.f), EArenaAlign::Center);

		if (ExtraHeight > 0.f)
		{
			DrawExtra(Team, X, Top, BW);
		}
	}

	ScoreInfoBottom = Top + BH;
	if (!GoalText.IsEmpty())
	{
		D.Text(GoalText, Right, Top + BH + D.S(6.f), HudTextDim, D.S(14.f), EArenaAlign::Right);
		ScoreInfoBottom += D.S(22.f);
	}
}

void AArenaHUD::DrawTimer()
{
	const AArenaGameState* GS = GetArenaGS();
	if (!GS)
	{
		return;
	}
	const float PW = D.S(180.f);
	const float PH = D.S(54.f);
	const float X = (D.W() - PW) * 0.5f;
	const float Y = D.S(18.f);

	FString Main;
	FLinearColor Col = HudTextWhite;
	bool bClock = false;
	switch (GS->Phase)
	{
	case EArenaMatchPhase::Countdown:
		Main = GS->Round > 1 ? FString::Printf(TEXT("ROUND %d"), GS->Round) : FString(TEXT("WARMUP"));
		Col = HudShieldColor;
		break;
	case EArenaMatchPhase::RoundOver:
		Main = TEXT("ROUND OVER");
		Col = HudShieldColor;
		break;
	case EArenaMatchPhase::Playing:
		if (GS->bOvertime)
		{
			Main = TEXT("OVERTIME");
			Col = HudWithAlpha(FLinearColor(1.f, 0.55f, 0.15f), 0.7f + 0.3f * FMath::Sin(Now() * 4.f));
		}
		else if (GS->TimeLimit > 0)
		{
			Main = HudFormatTime(GS->RemainingTime);
			bClock = true;
			if (GS->RemainingTime <= 30)
			{
				Col = FLinearColor(1.f, 0.4f, 0.3f);
			}
		}
		else
		{
			Main = TEXT("NO LIMIT");
			Col = HudTextDim;
		}
		break;
	case EArenaMatchPhase::Ended:
		Main = TEXT("FINAL");
		Col = HudShieldColor;
		break;
	}

	FString Sub = GS->GetGameTypeName();
	if (GS->bInstagib)
	{
		Sub += TEXT("  |  INSTAGIB");
	}
	if (!GS->LayoutName.IsEmpty())
	{
		Sub += TEXT("  |  ") + GS->LayoutName;
	}

	D.Panel(X, Y, PW, PH);
	D.TextMid(Main, X + PW * 0.5f, Y + PH * 0.5f, Col, D.S(Main.Len() > 6 ? 26.f : 34.f), EArenaAlign::Center);
	D.Text(Sub, D.W() * 0.5f, Y + PH + D.S(4.f), HudTextDim, D.S(14.f), EArenaAlign::Center);
}

void AArenaHUD::DrawCenterMessages()
{
	const AArenaPlayerController* PC = GetArenaPC();
	if (!PC)
	{
		return;
	}
	const float T = Now();
	float Y = D.H() * 0.24f;
	for (const FArenaCenterMessage& M : PC->GetCenterMessages())
	{
		const float Age = T - M.Time;
		const float FadeIn = FMath::Clamp(Age / 0.15f, 0.f, 1.f);
		const float FadeOut = FMath::Clamp((M.Duration - Age) / 0.5f, 0.f, 1.f);
		const float A = FadeIn * FadeOut;
		if (A <= 0.f)
		{
			continue;
		}
		const float Pop = 1.f + 0.2f * (1.f - FadeIn);
		D.Text(M.Text, D.W() * 0.5f, Y, HudWithAlpha(M.Color, A), D.S(32.f) * Pop, EArenaAlign::Center);
		Y += D.S(44.f);
	}
}

void AArenaHUD::DrawPickupMessage()
{
	const AArenaPlayerController* PC = GetArenaPC();
	if (!PC)
	{
		return;
	}
	float Age = 0.f;
	const FString Msg = PC->GetPickupMessage(Age);
	const float Life = 2.5f;
	if (Msg.IsEmpty() || Age < 0.f || Age > Life)
	{
		return;
	}
	const float A = FMath::Clamp((Life - Age) / 0.6f, 0.f, 1.f);
	const float Y = D.H() - D.S(24.f) - D.S(44.f) - D.S(52.f);
	const FVector2D Size = D.TextSize(Msg, D.S(20.f));
	const float PW = Size.X + D.S(32.f);
	const float PH = D.S(32.f);
	D.Panel((D.W() - PW) * 0.5f, Y, PW, PH, FLinearColor::Transparent, A);
	D.TextMid(Msg, D.W() * 0.5f, Y + PH * 0.5f, FLinearColor(0.75f, 1.f, 0.8f, A), D.S(20.f), EArenaAlign::Center);
}

// =============================================================================================
// Scoreboard
// =============================================================================================

float AArenaHUD::DrawScoreTable(const TArray<AArenaPlayerState*>& Players, float X, float Y, float Width, const FString& Title,
	const FLinearColor& Accent, int32 TeamScore, int32 Team)
{
	const bool bArt = ArenaArtHUD::HasHudArt();
	const float StartY = Y;
	const float TitleH = D.S(44.f);
	const float HeaderH = D.S(26.f);
	const float RowH = D.S(30.f);
	const APlayerState* MyPS = PlayerOwner ? PlayerOwner->PlayerState : nullptr;

	// title band
	if (!bArt || !ArenaArtHUD::DrawScoreBox(D, X, Y, Width, TitleH, Team, 1.f))
	{
		D.Rect(X, Y, Width, TitleH, FLinearColor(Accent.R * 0.3f, Accent.G * 0.3f, Accent.B * 0.3f, 0.85f));
		D.Rect(X, Y, Width, D.S(3.f), Accent);
	}
	else
	{
		D.Rect(X + D.S(3.f), Y + D.S(3.f), Width - D.S(6.f), TitleH - D.S(6.f), FLinearColor(Accent.R * 0.3f, Accent.G * 0.3f, Accent.B * 0.3f, 0.6f));
	}
	D.TextMid(Title, X + D.S(14.f), Y + TitleH * 0.5f, HudTextWhite, D.S(22.f));
	if (TeamScore >= 0)
	{
		D.TextMid(FString::FromInt(TeamScore), X + Width - D.S(14.f), Y + TitleH * 0.5f, HudTextWhite, D.S(30.f), EArenaAlign::Right);
	}
	Y += TitleH;

	// column layout (right-aligned numeric columns)
	const float ColPing = X + Width - D.S(14.f);
	const float ColDeaths = ColPing - D.S(76.f);
	const float ColKills = ColDeaths - D.S(70.f);
	const float ColScore = ColKills - D.S(70.f);
	const float HeaderSize = D.S(13.f);

	D.Rect(X, Y, Width, HeaderH, FLinearColor(0.02f, 0.025f, 0.04f, 0.8f));
	const float HY = Y + HeaderH * 0.5f;
	D.TextMid(TEXT("NAME"), X + D.S(14.f), HY, HudTextDim, HeaderSize);
	D.TextMid(TEXT("SCORE"), ColScore, HY, HudTextDim, HeaderSize, EArenaAlign::Right);
	D.TextMid(TEXT("KILLS"), ColKills, HY, HudTextDim, HeaderSize, EArenaAlign::Right);
	D.TextMid(TEXT("DEATHS"), ColDeaths, HY, HudTextDim, HeaderSize, EArenaAlign::Right);
	D.TextMid(TEXT("PING"), ColPing, HY, HudTextDim, HeaderSize, EArenaAlign::Right);
	Y += HeaderH;

	const int32 MaxRows = FMath::Max(1, FMath::FloorToInt((D.H() - Y - D.S(40.f)) / RowH));
	int32 Row = 0;
	for (const AArenaPlayerState* P : Players)
	{
		if (!P)
		{
			continue;
		}
		if (Row >= MaxRows)
		{
			break;
		}
		const bool bMe = (P == MyPS);
		D.Rect(X, Y, Width, RowH, bMe ? FLinearColor(Accent.R, Accent.G, Accent.B, 0.22f)
			: FLinearColor(0.02f, 0.025f, 0.04f, (Row % 2) ? 0.62f : 0.5f));
		if (bArt)
		{
			ArenaArtHUD::DrawScoreBox(D, X, Y, Width, RowH, bMe ? Team : ArenaTeam::None, bMe ? 0.9f : ((Row % 2) ? 0.55f : 0.4f));
		}
		if (bMe)
		{
			D.Rect(X, Y, D.S(3.f), RowH, FLinearColor(1.f, 1.f, 1.f, 0.85f));
		}
		const float MY = Y + RowH * 0.5f;
		const FLinearColor NameCol = bMe ? FLinearColor(1.f, 0.9f, 0.45f) : HudTextWhite;
		FString Name = P->GetPlayerName();
		if (Name.Len() > 22)
		{
			Name = Name.Left(21) + TEXT("~");
		}
		const float NameW = D.TextMid(FString::Printf(TEXT("%d. %s"), Row + 1, *Name), X + D.S(14.f), MY, NameCol, D.S(17.f));
		if (const UArenaGameRules* Rules = GetRules())
		{
			const FString Extra = Rules->GetScoreboardExtra(P);
			if (!Extra.IsEmpty())
			{
				D.TextMid(Extra, X + D.S(24.f) + NameW, MY, HudTextDim, D.S(14.f));
			}
		}
		D.TextMid(FString::FromInt(FMath::RoundToInt(P->GetScore())), ColScore, MY, HudTextWhite, D.S(17.f), EArenaAlign::Right);
		D.TextMid(FString::FromInt(P->Kills), ColKills, MY, HudTextDim, D.S(17.f), EArenaAlign::Right);
		D.TextMid(FString::FromInt(P->Deaths), ColDeaths, MY, HudTextDim, D.S(17.f), EArenaAlign::Right);
		const bool bBot = P->IsABot() || P->BotSkill >= 0.f;
		const FString PingStr = bBot ? FString(TEXT("BOT")) : FString::FromInt(FMath::RoundToInt(P->GetPingInMilliseconds()));
		D.TextMid(PingStr, ColPing, MY, bBot ? HudWithAlpha(HudTextDim, 0.7f) : HudTextDim, D.S(15.f), EArenaAlign::Right);
		Y += RowH;
		++Row;
	}
	if (Row == 0)
	{
		D.Rect(X, Y, Width, RowH, FLinearColor(0.02f, 0.025f, 0.04f, 0.5f));
		D.TextMid(TEXT("No players"), X + D.S(14.f), Y + RowH * 0.5f, HudTextDim, D.S(15.f));
		Y += RowH;
	}
	if (!bArt)
	{
		D.Outline(X, StartY, Width, Y - StartY, FLinearColor(0.6f, 0.75f, 1.f, 0.16f), 1.f);
	}
	return Y - StartY;
}

void AArenaHUD::DrawScoreboard()
{
	const AArenaGameState* GS = GetArenaGS();
	if (!GS)
	{
		return;
	}
	const bool bEnded = GS->Phase == EArenaMatchPhase::Ended;

	// dim the world a little behind the board
	D.Rect(0.f, 0.f, D.W(), D.H(), FLinearColor(0.f, 0.f, 0.f, 0.35f));

	float Y = bEnded ? D.S(230.f) : D.S(120.f);
	const float CX = D.W() * 0.5f;

	// framed window behind header and tables (interface art)
	{
		const TArray<AArenaPlayerState*> AllPlayers = GS->GetSortedPlayers();
		int32 Rows = 0;
		float TablesW = 0.f;
		if (!GS->IsTeamGame())
		{
			Rows = AllPlayers.Num();
			TablesW = FMath::Min(D.S(760.f), D.W() - D.S(48.f));
		}
		else
		{
			int32 Counts[2] = { 0, 0 };
			for (const AArenaPlayerState* P : AllPlayers)
			{
				if (P && (P->Team == 0 || P->Team == 1))
				{
					++Counts[P->Team];
				}
			}
			Rows = FMath::Max(Counts[0], Counts[1]);
			const float Gap = D.S(24.f);
			TablesW = FMath::Min(D.S(560.f), (D.W() - D.S(48.f) - Gap) * 0.5f) * 2.f + Gap;
		}
		const float TableTop = Y + D.S(66.f);
		const float MaxRows = FMath::Max(1.f, FMath::FloorToFloat((D.H() - TableTop - D.S(70.f) - D.S(40.f)) / D.S(30.f)));
		const float Bottom = FMath::Min(D.H() - D.S(44.f), TableTop + D.S(70.f) + FMath::Min(float(FMath::Max(1, Rows)), MaxRows) * D.S(30.f) + D.S(26.f));
		const float PW = FMath::Min(D.W() - D.S(8.f), TablesW + D.S(70.f));
		ArenaArtHUD::DrawMenuPanel(D, CX - PW * 0.5f, Y - D.S(26.f), PW, Bottom - (Y - D.S(26.f)), 0.9f);
	}

	// header
	FString Title = GS->GetGameTypeName();
	if (!GS->LayoutName.IsEmpty())
	{
		Title += FString::Printf(TEXT("  -  %s"), *GS->LayoutName);
	}
	D.Text(Title, CX, Y, HudTextWhite, D.S(28.f), EArenaAlign::Center);
	Y += D.S(36.f);
	FString Info;
	if (GS->GoalScore > 0)
	{
		Info += FString::Printf(TEXT("Goal %d"), GS->GoalScore);
	}
	if (GS->TimeLimit > 0)
	{
		Info += FString::Printf(TEXT("%sTime limit %d min"), Info.IsEmpty() ? TEXT("") : TEXT("   |   "), GS->TimeLimit / 60);
	}
	if (GS->bInstagib)
	{
		Info += FString::Printf(TEXT("%sInstagib"), Info.IsEmpty() ? TEXT("") : TEXT("   |   "));
	}
	const UArenaGameRules* Rules = GetRules();
	if (Rules)
	{
		const FString Extra = Rules->GetScoreboardHeaderExtra(GS);
		if (!Extra.IsEmpty())
		{
			Info += FString::Printf(TEXT("%s%s"), Info.IsEmpty() ? TEXT("") : TEXT("   |   "), *Extra);
		}
	}
	if (!Info.IsEmpty())
	{
		D.Text(Info, CX, Y, HudTextDim, D.S(15.f), EArenaAlign::Center);
	}
	Y += D.S(30.f);

	const TArray<AArenaPlayerState*> Sorted = GS->GetSortedPlayers();
	if (!GS->IsTeamGame())
	{
		const float Width = FMath::Min(D.S(760.f), D.W() - D.S(48.f));
		DrawScoreTable(Sorted, CX - Width * 0.5f, Y, Width, TEXT("Players"), HudAccentBlue, -1, ArenaTeam::None);
	}
	else
	{
		TArray<AArenaPlayerState*> TeamPlayers[2];
		for (AArenaPlayerState* P : Sorted)
		{
			if (P && (P->Team == 0 || P->Team == 1))
			{
				TeamPlayers[P->Team].Add(P);
			}
		}
		const float Gap = D.S(24.f);
		const float Width = FMath::Min(D.S(560.f), (D.W() - D.S(48.f) - Gap) * 0.5f);
		const float StartX = CX - Width - Gap * 0.5f;
		for (int32 Team = 0; Team < 2; ++Team)
		{
			DrawScoreTable(TeamPlayers[Team], StartX + Team * (Width + Gap), Y, Width,
				FString::Printf(TEXT("%s Team"), *ArenaTeam::Name(Team)), HudTeamTint(Team), GS->TeamScores[Team], Team);
		}
	}

	if (!bEnded)
	{
		D.Text(TEXT("Release TAB to close"), CX, D.H() - D.S(36.f), HudWithAlpha(HudTextDim, 0.7f), D.S(13.f), EArenaAlign::Center);
	}
}

// =============================================================================================
// Death / end / pause
// =============================================================================================

void AArenaHUD::DrawDeadScreen()
{
	const AArenaGameState* GS = GetArenaGS();
	// red vignette from top and bottom edges
	D.VerticalGradient(0.f, 0.f, D.W(), D.H() * 0.35f, FLinearColor(0.35f, 0.f, 0.f, 0.45f), FLinearColor(0.35f, 0.f, 0.f, 0.f), 16);
	D.VerticalGradient(0.f, D.H() * 0.65f, D.W(), D.H() * 0.35f, FLinearColor(0.35f, 0.f, 0.f, 0.f), FLinearColor(0.35f, 0.f, 0.f, 0.45f), 16);

	if (GS && GS->Phase == EArenaMatchPhase::Countdown)
	{
		return;
	}
	const float Pulse = 0.65f + 0.35f * FMath::Sin(Now() * 3.f);
	const float Y = D.H() * 0.62f;
	const UArenaGameRules* Rules = GetRules();
	const FString Msg = Rules ? Rules->GetDeadScreenMessage(PlayerOwner ? PlayerOwner->GetPlayerState<AArenaPlayerState>() : nullptr)
		: FString(TEXT("Press FIRE to respawn"));
	if (Msg.IsEmpty())
	{
		return;
	}
	const FVector2D Size = D.TextSize(Msg, D.S(28.f));
	const float PW = Size.X + D.S(48.f);
	const float PH = D.S(50.f);
	D.Panel((D.W() - PW) * 0.5f, Y, PW, PH);
	D.TextMid(Msg, D.W() * 0.5f, Y + PH * 0.5f, FLinearColor(1.f, 1.f, 1.f, Pulse), D.S(28.f), EArenaAlign::Center);
}

void AArenaHUD::DrawEndBanner()
{
	// GUI2K4 look (ArenaMidGameMenu.cpp, menus)
	ArenaMidGameMenu::DrawEndOfMatch(this, Canvas, GetArenaGS());
}

void AArenaHUD::DrawPauseMenu()
{
	// UT2K4PlayerLoginMenu / UT2K4SettingsPage in the GUI2K4 look (ArenaMidGameMenu.cpp, menus); hit areas and input
	// are handled by the menu GUI itself
	AArenaPlayerController* PC = GetArenaPC();
	ArenaMidGameMenu::DrawPauseMenu(this, Canvas, PC, bTestSettings || (PC && PC->IsPauseSettingsOpen()));
}

void AArenaHUD::DrawPauseSettings()
{
	ArenaMidGameMenu::DrawPauseMenu(this, Canvas, GetArenaPC(), true);
}

// =============================================================================================
// Test hook
// =============================================================================================

void AArenaHUD::UpdateTestPages()
{
	bTestScoreboard = bTestPause = bTestSettings = false;
#if !UE_BUILD_SHIPPING
	// -AutoHudShow=Scoreboard:20-24,Pause:26-28,Settings:30-33 forces those pages on during the given world-time
	// windows (seconds), for -AutoScreenshot checks of the scoreboard and the pause / settings pages; Kill:<a>-<b> kills
	// the local player in that window (the UT2004 HUD's death bar).
	static FString Spec;
	static bool bParsed = false;
	if (!bParsed)
	{
		bParsed = true;
		FParse::Value(FCommandLine::Get(), TEXT("AutoHudShow="), Spec, false);
	}
	if (Spec.IsEmpty())
	{
		return;
	}
	const float T = Now();
	TArray<FString> Parts;
	Spec.ParseIntoArray(Parts, TEXT(","));
	for (const FString& Part : Parts)
	{
		FString Name, Range, From, To;
		if (!Part.Split(TEXT(":"), &Name, &Range) || !Range.Split(TEXT("-"), &From, &To))
		{
			continue;
		}
		if (T < FCString::Atof(*From) || T > FCString::Atof(*To))
		{
			continue;
		}
		bTestScoreboard |= Name.Equals(TEXT("Scoreboard"), ESearchCase::IgnoreCase);
		bTestPause |= Name.Equals(TEXT("Pause"), ESearchCase::IgnoreCase);
		bTestSettings |= Name.Equals(TEXT("Settings"), ESearchCase::IgnoreCase);
		// Kill:<from>-<to>: the local player dies (once a second) in that window, for the spectating / death bar
		if (Name.Equals(TEXT("Kill"), ESearchCase::IgnoreCase) && GetWorld() && GetWorld()->GetAuthGameMode())
		{
			static float LastKill = -10.f;
			AArenaCharacter* Char = Cast<AArenaCharacter>(GetOwningPawn());
			if (Char && Char->IsAlive() && T - LastKill > 1.f)
			{
				LastKill = T;
				Char->Suicide();
			}
		}
	}
#endif
}

// =============================================================================================
// Hit boxes
// =============================================================================================

namespace ArenaHUDLocal
{
static int32 HudParsePauseIndex(FName BoxName)
{
	const FString Str = BoxName.ToString();
	if (Str.StartsWith(TEXT("Pause")))
	{
		return FCString::Atoi(*Str.RightChop(5));
	}
	return INDEX_NONE;
}

/** "PSetL3" / "PSetR3" / "PSetB4" -> row index and direction (-1 left, +1 right / back). */
static bool HudParsePauseSetting(FName BoxName, int32& OutRow, int32& OutDir)
{
	const FString Str = BoxName.ToString();
	if (!Str.StartsWith(TEXT("PSet")) || Str.Len() < 6)
	{
		return false;
	}
	const TCHAR Kind = Str[4];
	OutDir = (Kind == TCHAR('L')) ? -1 : +1;
	OutRow = FCString::Atoi(*Str.RightChop(5));
	return true;
}
}

void AArenaHUD::NotifyHitBoxClick(FName BoxName)
{
	Super::NotifyHitBoxClick(BoxName);
	AArenaPlayerController* PC = GetArenaPC();
	if (!PC)
	{
		return;
	}
	int32 Row = 0, Dir = 0;
	if (HudParsePauseSetting(BoxName, Row, Dir))
	{
		PC->ClickPauseSetting(Row, Dir);
		return;
	}
	const int32 Index = HudParsePauseIndex(BoxName);
	if (Index != INDEX_NONE)
	{
		PC->ClickPauseItem(Index);
	}
}

void AArenaHUD::NotifyHitBoxBeginCursorOver(FName BoxName)
{
	Super::NotifyHitBoxBeginCursorOver(BoxName);
	AArenaPlayerController* PC = GetArenaPC();
	if (!PC)
	{
		return;
	}
	int32 Row = 0, Dir = 0;
	if (HudParsePauseSetting(BoxName, Row, Dir))
	{
		PC->SetPauseSettingsSelection(Row);
		return;
	}
	const int32 Index = HudParsePauseIndex(BoxName);
	if (Index != INDEX_NONE)
	{
		PC->SetPauseMenuSelection(Index);
	}
}
