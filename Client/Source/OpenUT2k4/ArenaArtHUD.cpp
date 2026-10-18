#include "ArenaArtHUD.h"
#include "ArenaArt.h"
#include "ArenaWeapon.h"
#include "OpenUT2k4.h"

#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"

namespace ArenaArtHUDPrivate
{
	using ArenaArtHUD::FHudSprite;

	/** Applies the streaming override once per texture so canvas draws get the full-resolution mips. */
	void PrepareTexture(UTexture2D* Tex)
	{
		static TSet<TWeakObjectPtr<UTexture2D>> Prepared;
		if (!Tex || Prepared.Contains(Tex))
		{
			return;
		}
		Prepared.Add(Tex);
		// The imported textures are world textures that stream; the HUD is not seen by the streamer.
		Tex->bForceMiplevelsToBeResident = true;
		Tex->bIgnoreStreamingMipBias = true;
	}

	const FTexture* ResourceOf(UTexture2D* Tex)
	{
		return Tex ? Tex->GetResource() : nullptr;
	}

	void Tile(UCanvas* Canvas, const FTexture* Res, float X, float Y, float W, float H, FVector2D UV0, FVector2D UV1,
		const FLinearColor& Tint, bool bAdditive, float RotationDeg = 0.f)
	{
		if (!Canvas || !Res || W <= 0.f || H <= 0.f || Tint.A <= 0.f)
		{
			return;
		}
		// The interface textures carry their shape in alpha (white RGB everywhere), so "additive" glows are
		// drawn alpha blended with a brighter tint; a true additive blend would light up the whole quad.
		const FLinearColor C = bAdditive ? FLinearColor(Tint.R * 1.3f, Tint.G * 1.3f, Tint.B * 1.3f, Tint.A) : Tint;
		FCanvasTileItem Item(FVector2D(X, Y), Res, FVector2D(W, H), UV0, UV1, C);
		Item.BlendMode = SE_BLEND_Translucent;
		if (RotationDeg != 0.f)
		{
			Item.Rotation = FRotator(0.f, RotationDeg, 0.f);
			Item.PivotPoint = FVector2D(0.5f, 0.5f);
		}
		Canvas->DrawItem(Item);
	}

	/** UVs of a source-pixel rectangle, pulled in by half a texel so neighbouring atlas cells do not bleed in. */
	void SpriteUVs(const FHudSprite& S, float SX1, float SY1, float SX2, float SY2, FVector2D& UV0, FVector2D& UV1)
	{
		const float InX = SX2 >= SX1 ? 0.5f : -0.5f;
		const float InY = SY2 >= SY1 ? 0.5f : -0.5f;
		UV0 = FVector2D((SX1 + InX) / S.AtlasW, (SY1 + InY) / S.AtlasH);
		UV1 = FVector2D((SX2 - InX) / S.AtlasW, (SY2 - InY) / S.AtlasH);
	}

	/** Big HUD digits 0-9 and '-' (index 10): horizontal extent of each glyph in the atlas; cells are 38 px tall. */
	struct FGlyph { float X1, X2; };
	const FGlyph Digits[11] = {
		{ 0, 38 }, { 45, 70 }, { 80, 117 }, { 119, 156 }, { 157, 195 }, { 198, 234 },
		{ 236, 272 }, { 275, 311 }, { 314, 350 }, { 353, 389 }, { 403, 425 },
	};
	constexpr float DigitCell = 38.f;
	constexpr float DigitSpacing = 0.04f; // of the height

	float GlyphWidth(int32 Glyph, float Height)
	{
		return (Digits[Glyph].X2 - Digits[Glyph].X1) / DigitCell * Height;
	}

	void NumberGlyphs(int32 Value, TArray<int32, TInlineAllocator<12>>& Out)
	{
		const FString Str = FString::FromInt(Value);
		for (const TCHAR C : Str)
		{
			if (C == TCHAR('-'))
			{
				Out.Add(10);
			}
			else if (C >= TCHAR('0') && C <= TCHAR('9'))
			{
				Out.Add(C - TCHAR('0'));
			}
		}
	}

	float GlyphsWidth(const TArray<int32, TInlineAllocator<12>>& Glyphs, float Height)
	{
		float W = 0.f;
		for (int32 i = 0; i < Glyphs.Num(); ++i)
		{
			W += GlyphWidth(Glyphs[i], Height) + (i > 0 ? DigitSpacing * Height : 0.f);
		}
		return W;
	}

	float AlignStart(float X, float Width, EArenaAlign Align)
	{
		return Align == EArenaAlign::Center ? X - Width * 0.5f : (Align == EArenaAlign::Right ? X - Width : X);
	}

	// ---- weapons ----
	struct FWeaponIcons
	{
		const TCHAR* ClassName;
		FHudSprite Icon;
		FHudSprite Ammo;
		bool bHasAmmo;
		/** InventoryGroup of the stock weapon it stands in for (the weapon bar slot, HudCDeathmatch.DrawWeaponBar) */
		int32 Group;
	};

	const TArray<FWeaponIcons>& WeaponIconTable()
	{
		using namespace ArenaArtHUD::Sprites;
		// Source: the weapon / ammo class defaults (IconMaterial, IconCoords, InventoryGroup) of the matching source-game
		// weapons (ShieldGun 1, AssaultRifle 2, BioRifle 3, ShockRifle 4 (SuperShockRifle too), LinkGun 5, Minigun 6,
		// FlakCannon 7, RocketLauncher 8, SniperRifle 9, BallLauncher 15 without an icon).
		static const TArray<FWeaponIcons> Table = {
			{ TEXT("Weap_Gauntlet"),       Atlas(169, 39, 241, 77),   FHudSprite(),              false, 1 },
			{ TEXT("Weap_Carbine"),        Atlas(245, 39, 329, 79),   Atlas(336, 82, 382, 125),  true, 2 },
			{ TEXT("Weap_GooLauncher"),    Atlas(179, 127, 241, 175), Atlas(383, 82, 412, 129),  true, 3 },
			{ TEXT("Weap_PulseRifle"),     Atlas(250, 110, 330, 145), Atlas(400, 130, 426, 179), true, 4 },
			{ TEXT("Weap_ArcLinker"),      Atlas(169, 78, 244, 124),  Atlas(413, 82, 457, 125),  true, 5 },
			{ TEXT("Weap_RotaryGun"),      Atlas(246, 80, 332, 106),  Atlas(338, 40, 393, 79),   true, 6 },
			{ TEXT("Weap_ShardCannon"),    Atlas(169, 172, 245, 208), Atlas(394, 40, 457, 81),   true, 7 },
			{ TEXT("Weap_RocketLauncher"), Atlas(253, 146, 333, 181), Atlas(458, 34, 511, 78),   true, 8 },
			{ TEXT("Weap_RailRifle"),      Atlas(246, 182, 331, 210), Atlas(458, 82, 491, 133),  true, 9 },
			{ TEXT("Weap_InstaRifle"),     Atlas(250, 110, 330, 145), Atlas(400, 130, 426, 179), true, 4 },
			// Redeemer 0, Painter 0, ClassicSniperRifle 9, ONSAVRiL 8, ONSGrenadeLauncher 7, ONSMineLayer 3 (IconCoords of the
			// weapons and their ammo classes)
			{ TEXT("Weap_WarheadLauncher"), Atlas(4, 350, 110, 395),  FHudSprite(),              false, 0 },
			{ TEXT("Weap_OrbitalDesignator"), Atlas(0, 407, 118, 442), FHudSprite(),             false, 0 },
			{ TEXT("Weap_MarksmanRifle"),  Atlas(420, 180, 512, 210), Atlas(451, 445, 510, 500), true, 9 },
			{ TEXT("Weap_SeekerLauncher"), Atlas(429, 212, 508, 251), Atlas(455, 295, 494, 341), true, 8 },
			{ TEXT("Weap_StickyLauncher"), Atlas(434, 253, 506, 292), Atlas(460, 343, 488, 392), true, 7 },
			{ TEXT("Weap_CrawlerLayer"),   Atlas(229, 258, 296, 307), Atlas(451, 393, 495, 441), true, 3 },
			{ TEXT("ArenaBR_BallLauncher"), BallGold(),               FHudSprite(),              false, 15 },
		};
		return Table;
	}

	const FWeaponIcons* FindWeapon(const UClass* WeaponClass)
	{
		for (const UClass* C = WeaponClass; C; C = C->GetSuperClass())
		{
			const FString Name = C->GetName();
			for (const FWeaponIcons& E : WeaponIconTable())
			{
				if (Name == E.ClassName)
				{
					return &E;
				}
			}
		}
		return nullptr;
	}

	// ---- crosshairs: texture name, label ----
	struct FCrosshair
	{
		const TCHAR* Texture;
		const TCHAR* Label;
	};
	const FCrosshair Crosshairs[] = {
		{ nullptr,                                         TEXT("Default") },
		{ TEXT("Textures/Crosshairs/HUD_Crosshair_Cross1"),   TEXT("Cross") },
		{ TEXT("Textures/Crosshairs/HUD_Crosshair_Cross2"),   TEXT("Dotted Cross") },
		{ TEXT("Textures/Crosshairs/HUD_Crosshair_Cross3"),   TEXT("Small Cross") },
		{ TEXT("Textures/Crosshairs/HUD_Crosshair_Cross4"),   TEXT("Diagonal") },
		{ TEXT("Textures/Crosshairs/HUD_Crosshair_Cross5"),   TEXT("Long Cross") },
		{ TEXT("Textures/Crosshairs/HUD_Crosshair_Dot"),      TEXT("Dot") },
		{ TEXT("Textures/Crosshairs/HUD_Crosshair_Pointer"),  TEXT("Pointers") },
		{ TEXT("Textures/Crosshairs/HUD_Crosshair_Bracket1"), TEXT("Brackets") },
		{ TEXT("Textures/Crosshairs/HUD_Crosshair_Bracket2"), TEXT("Wide Brackets") },
		{ TEXT("Textures/Crosshairs/HUD_Crosshair_Circle1"),  TEXT("Circle") },
		{ TEXT("Textures/Crosshairs/HUD_Crosshair_Circle2"),  TEXT("Split Circle") },
		{ TEXT("Textures/Crosshairs/HUD_Crosshair_Triad1"),   TEXT("Triangle") },
		{ TEXT("Textures/Crosshairs/HUD_Crosshair_Triad2"),   TEXT("Tri-Blade") },
		{ TEXT("Textures/Crosshairs/HUD_Crosshair_Triad3"),   TEXT("Tri-Tech") },
	};

	// ---- menus ----
	const TCHAR* ButtonTexture(ArenaArtHUD::EButtonState State)
	{
		switch (State)
		{
		case ArenaArtHUD::EButtonState::Hover:    return TEXT("Textures/2K4Menus/Controls_buttonThick_w");
		case ArenaArtHUD::EButtonState::Focused:  return TEXT("Textures/2K4Menus/Controls_buttonThick_f");
		case ArenaArtHUD::EButtonState::Pressed:  return TEXT("Textures/2K4Menus/Controls_buttonThick_p");
		case ArenaArtHUD::EButtonState::Disabled: return TEXT("Textures/2K4Menus/Controls_buttonThick_d");
		default:                                  return TEXT("Textures/2K4Menus/Controls_buttonThick_b");
		}
	}

	const TCHAR* ArrowTexture(bool bRight, ArenaArtHUD::EButtonState State)
	{
		const bool bHot = State == ArenaArtHUD::EButtonState::Focused || State == ArenaArtHUD::EButtonState::Pressed;
		const bool bWatched = State == ArenaArtHUD::EButtonState::Hover;
		if (bRight)
		{
			return bHot ? TEXT("Textures/2K4Menus/Controls_arrowRight_f") : (bWatched ? TEXT("Textures/2K4Menus/Controls_arrowRight_w") : TEXT("Textures/2K4Menus/Controls_arrowRight_b"));
		}
		return bHot ? TEXT("Textures/2K4Menus/Controls_arrowLeft_f") : (bWatched ? TEXT("Textures/2K4Menus/Controls_arrowLeft_w") : TEXT("Textures/2K4Menus/Controls_arrowLeft_b"));
	}

	/** Logo-free character renders for the menu backdrop: path, first column with content (of 1024). */
	struct FMenuRender
	{
		const TCHAR* Texture;
		float ContentLeft;
	};
	const FMenuRender MenuRenders[] = {
		{ TEXT("Textures/2K4Menus/MainMenu_char02"), 352.f },
		{ TEXT("Textures/2K4Menus/MainMenu_Char01"), 221.f },
		{ TEXT("Textures/2K4Menus/MainMenu_char03"), 528.f },
	};
}

using namespace ArenaArtHUDPrivate;

// =================================================================================================
// Textures
// =================================================================================================

UTexture2D* ArenaArtHUD::GetTexture(const TCHAR* Path)
{
	if (!Path || !*Path)
	{
		return nullptr;
	}
	UTexture2D* Tex = ArenaArt::Load<UTexture2D>(Path);
	PrepareTexture(Tex);
	return Tex;
}

bool ArenaArtHUD::HasHudArt()
{
	return ResourceOf(GetTexture(Tex::HudAtlas())) != nullptr;
}

bool ArenaArtHUD::HasMenuArt()
{
	return ResourceOf(GetTexture(Tex::MenuPanel())) != nullptr && ResourceOf(GetTexture(ButtonTexture(EButtonState::Normal))) != nullptr;
}

// =================================================================================================
// Drawing
// =================================================================================================

bool ArenaArtHUD::DrawSprite(const FArenaDraw& D, const FHudSprite& S, float X, float Y, float W, float H, const FLinearColor& Tint,
	bool bAdditive, float RotationDeg)
{
	const FTexture* Res = ResourceOf(GetTexture(S.Texture));
	if (!Res || !D.Canvas)
	{
		return false;
	}
	FVector2D UV0, UV1;
	SpriteUVs(S, S.X1, S.Y1, S.X2, S.Y2, UV0, UV1);
	Tile(D.Canvas, Res, X, Y, W, H, UV0, UV1, Tint, bAdditive, RotationDeg);
	return true;
}

bool ArenaArtHUD::DrawSpriteCentered(const FArenaDraw& D, const FHudSprite& S, float CX, float CY, float Height, const FLinearColor& Tint,
	bool bAdditive, float RotationDeg)
{
	const float W = Height * S.Aspect();
	return DrawSprite(D, S, CX - W * 0.5f, CY - Height * 0.5f, W, Height, Tint, bAdditive, RotationDeg);
}

bool ArenaArtHUD::DrawSpriteFraction(const FArenaDraw& D, const FHudSprite& S, float X, float Y, float W, float H, float Fraction, const FLinearColor& Tint)
{
	const FTexture* Res = ResourceOf(GetTexture(S.Texture));
	if (!Res || !D.Canvas)
	{
		return false;
	}
	const float F = FMath::Clamp(Fraction, 0.f, 1.f);
	if (F <= 0.f)
	{
		return true;
	}
	FVector2D UV0, UV1;
	SpriteUVs(S, S.X1, S.Y1, S.X1 + (S.X2 - S.X1) * F, S.Y2, UV0, UV1);
	Tile(D.Canvas, Res, X, Y, W * F, H, UV0, UV1, Tint, false);
	return true;
}

bool ArenaArtHUD::DrawNineSlice(const FArenaDraw& D, const FHudSprite& S, float SrcMargin, float X, float Y, float W, float H, float ScreenMargin,
	const FLinearColor& Tint, bool bAdditive)
{
	const FTexture* Res = ResourceOf(GetTexture(S.Texture));
	if (!Res || !D.Canvas)
	{
		return false;
	}
	if (W <= 0.f || H <= 0.f)
	{
		return true;
	}
	const float M = FMath::Min(ScreenMargin, FMath::Min(W, H) * 0.5f);
	const float SM = FMath::Min(SrcMargin, FMath::Min(S.Width(), S.Height()) * 0.5f);
	const float SX[4] = { S.X1, S.X1 + SM, S.X2 - SM, S.X2 };
	const float SY[4] = { S.Y1, S.Y1 + SM, S.Y2 - SM, S.Y2 };
	const float DX[4] = { X, X + M, X + W - M, X + W };
	const float DY[4] = { Y, Y + M, Y + H - M, Y + H };
	for (int32 Row = 0; Row < 3; ++Row)
	{
		for (int32 Col = 0; Col < 3; ++Col)
		{
			const float PW = DX[Col + 1] - DX[Col];
			const float PH = DY[Row + 1] - DY[Row];
			if (PW <= 0.f || PH <= 0.f)
			{
				continue;
			}
			// Exact UVs inside the box (only the outer edges are pulled in) so the slices join without seams
			FVector2D UV0((SX[Col] + (Col == 0 ? 0.5f : 0.f)) / S.AtlasW, (SY[Row] + (Row == 0 ? 0.5f : 0.f)) / S.AtlasH);
			FVector2D UV1((SX[Col + 1] - (Col == 2 ? 0.5f : 0.f)) / S.AtlasW, (SY[Row + 1] - (Row == 2 ? 0.5f : 0.f)) / S.AtlasH);
			Tile(D.Canvas, Res, DX[Col], DY[Row], PW, PH, UV0, UV1, Tint, bAdditive);
		}
	}
	return true;
}

bool ArenaArtHUD::DrawTexture(const FArenaDraw& D, const TCHAR* Path, float X, float Y, float W, float H, const FLinearColor& Tint,
	float U0, float V0, float U1, float V1, bool bAdditive)
{
	const FTexture* Res = ResourceOf(GetTexture(Path));
	if (!Res || !D.Canvas)
	{
		return false;
	}
	Tile(D.Canvas, Res, X, Y, W, H, FVector2D(U0, V0), FVector2D(U1, V1), Tint, bAdditive);
	return true;
}

float ArenaArtHUD::NumberWidth(int32 Value, float Height)
{
	TArray<int32, TInlineAllocator<12>> Glyphs;
	NumberGlyphs(Value, Glyphs);
	return GlyphsWidth(Glyphs, Height);
}

bool ArenaArtHUD::DrawNumber(const FArenaDraw& D, int32 Value, float X, float CY, float Height, EArenaAlign Align, const FLinearColor& Tint)
{
	const FTexture* Res = ResourceOf(GetTexture(Tex::HudAtlas()));
	if (!Res || !D.Canvas)
	{
		return false;
	}
	TArray<int32, TInlineAllocator<12>> Glyphs;
	NumberGlyphs(Value, Glyphs);
	float PX = AlignStart(X, GlyphsWidth(Glyphs, Height), Align);
	const float Y = CY - Height * 0.5f;
	const FHudSprite Atlas = Sprites::Atlas(0, 0, 1, 1);
	const FLinearColor Shadow(0.f, 0.f, 0.f, 0.5f * Tint.A);
	// the glyphs are mid-grey chrome; lift them so white tints read as white
	const FLinearColor Bright(Tint.R * 1.35f, Tint.G * 1.35f, Tint.B * 1.35f, Tint.A);
	const float ShadowOff = FMath::Max(1.f, Height * 0.04f);
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		float GX = PX;
		for (const int32 G : Glyphs)
		{
			const float GW = GlyphWidth(G, Height);
			FVector2D UV0, UV1;
			SpriteUVs(Atlas, Digits[G].X1, 0.f, Digits[G].X2, DigitCell, UV0, UV1);
			if (Pass == 0)
			{
				Tile(D.Canvas, Res, FMath::RoundToFloat(GX + ShadowOff), FMath::RoundToFloat(Y + ShadowOff), GW, Height, UV0, UV1, Shadow, false);
			}
			else
			{
				Tile(D.Canvas, Res, FMath::RoundToFloat(GX), FMath::RoundToFloat(Y), GW, Height, UV0, UV1, Bright, false);
			}
			GX += GW + DigitSpacing * Height;
		}
	}
	return true;
}

float ArenaArtHUD::ClockWidth(int32 Seconds, float Height)
{
	Seconds = FMath::Max(0, Seconds);
	const int32 Minutes = Seconds / 60;
	const float Colon = Height * 0.3f;
	const int32 Secs = Seconds % 60;
	return NumberWidth(Minutes, Height) + Colon + NumberWidth(Secs / 10, Height) + DigitSpacing * Height + NumberWidth(Secs % 10, Height);
}

bool ArenaArtHUD::DrawClock(const FArenaDraw& D, int32 Seconds, float X, float CY, float Height, EArenaAlign Align, const FLinearColor& Tint)
{
	if (!HasHudArt())
	{
		return false;
	}
	Seconds = FMath::Max(0, Seconds);
	const int32 Minutes = Seconds / 60;
	const int32 Secs = Seconds % 60;
	const float Colon = Height * 0.3f;
	const float MinW = NumberWidth(Minutes, Height);
	const float Total = ClockWidth(Seconds, Height);
	const float PX = AlignStart(X, Total, Align);
	DrawNumber(D, Minutes, PX, CY, Height, EArenaAlign::Left, Tint);
	const FHudSprite C = Sprites::DigitColon();
	const float CH = Height * 0.55f;
	DrawSprite(D, C, PX + MinW + (Colon - CH * C.Aspect()) * 0.5f, CY - CH * 0.5f, CH * C.Aspect(), CH, Tint);
	float SX = PX + MinW + Colon;
	DrawNumber(D, Secs / 10, SX, CY, Height, EArenaAlign::Left, Tint);
	SX += NumberWidth(Secs / 10, Height) + DigitSpacing * Height;
	DrawNumber(D, Secs % 10, SX, CY, Height, EArenaAlign::Left, Tint);
	return true;
}

bool ArenaArtHUD::DrawHudPanel(const FArenaDraw& D, float X, float Y, float W, float H, const FLinearColor& Accent, float Alpha)
{
	if (!HasHudArt() || Alpha <= 0.f)
	{
		return HasHudArt();
	}
	const FLinearColor Steel(0.42f, 0.52f, 0.72f, 1.f);
	const FLinearColor Base = Accent.A > 0.f ? FMath::Lerp(FLinearColor(Accent.R, Accent.G, Accent.B, 1.f), Steel, 0.3f) : Steel;
	const float M = FMath::Max(3.f, D.S(9.f));
	// dark underlay for readable text on bright scenes
	D.Rect(X + M * 0.3f, Y + M * 0.3f, W - M * 0.6f, H - M * 0.6f, FLinearColor(0.f, 0.f, 0.02f, 0.42f * Alpha));
	DrawNineSlice(D, Sprites::Box(), 9.f, X, Y, W, H, M, FLinearColor(Base.R, Base.G, Base.B, 0.95f * Alpha));
	return true;
}

bool ArenaArtHUD::DrawScoreBox(const FArenaDraw& D, float X, float Y, float W, float H, int32 Team, float Alpha)
{
	const TCHAR* Path = Team == 0 ? Tex::ScoreBoxRed() : (Team == 1 ? Tex::ScoreBoxBlue() : Tex::ScoreBoxNeutral());
	const FHudSprite S{ Path, 0.f, 0.f, 64.f, 64.f, 64.f, 64.f };
	return DrawNineSlice(D, S, 10.f, X, Y, W, H, FMath::Max(3.f, FMath::Min(D.S(10.f), H * 0.3f)), FLinearColor(1.f, 1.f, 1.f, Alpha));
}

// =================================================================================================
// Weapons
// =================================================================================================

const ArenaArtHUD::FHudSprite* ArenaArtHUD::GetWeaponIcon(const UClass* WeaponClass)
{
	const FWeaponIcons* E = FindWeapon(WeaponClass);
	return E ? &E->Icon : nullptr;
}

int32 ArenaArtHUD::GetWeaponGroup(const UClass* WeaponClass)
{
	const FWeaponIcons* E = FindWeapon(WeaponClass);
	return E ? E->Group : 0;
}

const ArenaArtHUD::FHudSprite* ArenaArtHUD::GetAmmoIcon(const UClass* WeaponClass)
{
	const FWeaponIcons* E = FindWeapon(WeaponClass);
	return (E && E->bHasAmmo) ? &E->Ammo : nullptr;
}

const ArenaArtHUD::FHudSprite* ArenaArtHUD::FindWeaponIconByName(const FString& WeaponName)
{
	if (WeaponName.IsEmpty())
	{
		return nullptr;
	}
	static TMap<FString, const FHudSprite*> ByName;
	static bool bBuilt = false;
	if (!bBuilt)
	{
		bBuilt = true;
		for (const FWeaponIcons& E : WeaponIconTable())
		{
			// Display names come from the weapon defaults, so a renamed weapon keeps its icon.
			if (const UClass* Cls = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/OpenUT2k4.%s"), E.ClassName)))
			{
				if (const AArenaWeapon* CDO = Cls->GetDefaultObject<AArenaWeapon>())
				{
					ByName.Add(CDO->WeaponName.ToLower(), &E.Icon);
				}
			}
		}
	}
	const FHudSprite* const* Found = ByName.Find(WeaponName.ToLower());
	return Found ? *Found : nullptr;
}

// =================================================================================================
// Crosshairs
// =================================================================================================

int32 ArenaArtHUD::NumCrosshairs()
{
	return UE_ARRAY_COUNT(Crosshairs);
}

FString ArenaArtHUD::GetCrosshairTexture(int32 Index)
{
	return (Index > 0 && Index < NumCrosshairs() && Crosshairs[Index].Texture) ? FString(Crosshairs[Index].Texture) : FString();
}

FString ArenaArtHUD::GetCrosshairName(int32 Index)
{
	return (Index >= 0 && Index < NumCrosshairs()) ? FString(Crosshairs[Index].Label) : FString(Crosshairs[0].Label);
}

bool ArenaArtHUD::DrawCrosshair(const FArenaDraw& D, int32 Index, float CX, float CY, float Size, const FLinearColor& Color, bool bPixelSnap)
{
	if (Index <= 0 || Index >= NumCrosshairs())
	{
		return false;
	}
	const TCHAR* Path = Crosshairs[Index].Texture;
	if (!ResourceOf(GetTexture(Path)))
	{
		return false;
	}
	// Whole multiples of the texture size keep the thin lines crisp (no minification blur)
	const float Px = bPixelSnap ? FMath::Max(1.f, FMath::FloorToFloat(Size / 64.f + 0.25f)) * 64.f : Size;
	const float X = FMath::RoundToFloat(CX - Px * 0.5f);
	const float Y = FMath::RoundToFloat(CY - Px * 0.5f);
	DrawTexture(D, Path, X + 1.f, Y + 1.f, Px, Px, FLinearColor(0.f, 0.f, 0.f, 0.35f * Color.A));
	return DrawTexture(D, Path, X, Y, Px, Px, Color);
}

// =================================================================================================
// Menus
// =================================================================================================

bool ArenaArtHUD::DrawMenuBackground(const FArenaDraw& D, float Time, int32 Variant)
{
	UTexture2D* Bg = GetTexture(Tex::MenuBackground());
	if (!ResourceOf(Bg))
	{
		return false;
	}
	const float W = D.W();
	const float H = D.H();
	D.Rect(0.f, 0.f, W, H, FLinearColor(0.f, 0.f, 0.f, 1.f));

	// Scene render: the 1024 x 768 picture in the top of the texture. The outer columns carry wall lettering,
	// so only the middle is used, cropped to the screen aspect with a slow drift.
	const float SrcU0 = 0.15f, SrcU1 = 0.85f, SrcV0 = 0.f, SrcV1 = 0.75f;
	const float SrcW = (SrcU1 - SrcU0) * 1024.f, SrcH = (SrcV1 - SrcV0) * 1024.f;
	const float ScreenAspect = W / FMath::Max(1.f, H);
	float CropW = SrcW, CropH = SrcW / ScreenAspect;
	if (CropH > SrcH)
	{
		CropH = SrcH;
		CropW = SrcH * ScreenAspect;
	}
	const float Drift = 0.5f + 0.5f * FMath::Sin(Time * 0.05f);
	const float CU0 = SrcU0 + (SrcW - CropW) / 1024.f * 0.5f;
	const float CV0 = SrcV0 + (SrcH - CropH) / 1024.f * Drift;
	DrawTexture(D, Tex::MenuBackground(), 0.f, 0.f, W, H, FLinearColor(0.55f, 0.6f, 0.7f, 1.f), CU0, CV0, CU0 + CropW / 1024.f, CV0 + CropH / 1024.f);

	// Character render on the right
	const FMenuRender& R = MenuRenders[FMath::Abs(Variant) % UE_ARRAY_COUNT(MenuRenders)];
	if (ResourceOf(GetTexture(R.Texture)))
	{
		const float RH = H * 0.92f;
		const float Scale = RH / 780.f;
		const float RW = (1024.f - R.ContentLeft) * Scale;
		const float RX = W - RW - D.S(10.f);
		D.VerticalGradient(0.f, 0.f, W, H, FLinearColor(0.f, 0.02f, 0.06f, 0.25f), FLinearColor(0.f, 0.f, 0.f, 0.55f), 12);
		DrawTexture(D, R.Texture, RX, H - RH, RW, RH, FLinearColor(0.85f, 0.88f, 0.95f, 0.9f), R.ContentLeft / 1024.f, 0.f, 1.f, 780.f / 1024.f);
	}
	else
	{
		D.VerticalGradient(0.f, 0.f, W, H, FLinearColor(0.f, 0.02f, 0.06f, 0.25f), FLinearColor(0.f, 0.f, 0.f, 0.55f), 12);
	}

	// Scanlines, scrolling slowly
	const float Lines = H / FMath::Max(2.f, D.S(4.f));
	const float Scroll = FMath::Frac(Time * 0.05f);
	DrawTexture(D, Tex::Scanlines(), 0.f, 0.f, W, H, FLinearColor(0.4f, 0.6f, 1.f, 0.05f), 0.f, Scroll, W / H * Lines / 16.f, Scroll + Lines / 16.f, true);

	// vignette top and bottom
	D.VerticalGradient(0.f, 0.f, W, H * 0.22f, FLinearColor(0.f, 0.f, 0.f, 0.65f), FLinearColor(0.f, 0.f, 0.f, 0.f), 10);
	D.VerticalGradient(0.f, H * 0.82f, W, H * 0.18f, FLinearColor(0.f, 0.f, 0.f, 0.f), FLinearColor(0.f, 0.f, 0.f, 0.7f), 10);
	return true;
}

bool ArenaArtHUD::DrawMenuPanel(const FArenaDraw& D, float X, float Y, float W, float H, float Alpha)
{
	const FHudSprite S{ Tex::MenuPanel(), 0.f, 0.f, 128.f, 128.f, 128.f, 128.f };
	if (!ResourceOf(GetTexture(S.Texture)))
	{
		return false;
	}
	// darken what is behind the window a little more than the frame itself does
	D.Rect(X + D.S(6.f), Y + D.S(6.f), W - D.S(12.f), H - D.S(12.f), FLinearColor(0.f, 0.f, 0.02f, 0.45f * Alpha));
	return DrawNineSlice(D, S, 40.f, X, Y, W, H, FMath::Min(D.S(40.f), FMath::Min(W, H) * 0.25f), FLinearColor(1.f, 1.f, 1.f, 0.95f * Alpha));
}

bool ArenaArtHUD::DrawMenuHeader(const FArenaDraw& D, float X, float Y, float W, float H, float Alpha)
{
	const FHudSprite S{ Tex::MenuHeader(), 0.f, 0.f, 128.f, 32.f, 128.f, 32.f };
	if (!ResourceOf(GetTexture(S.Texture)))
	{
		return false;
	}
	return DrawNineSlice(D, S, 15.f, X, Y, W, H, FMath::Min(H * 0.5f, D.S(30.f)), FLinearColor(1.f, 1.f, 1.f, Alpha));
}

bool ArenaArtHUD::DrawMenuButton(const FArenaDraw& D, float X, float Y, float W, float H, EButtonState State, float Alpha)
{
	const FHudSprite S{ ButtonTexture(State), 0.f, 0.f, 64.f, 64.f, 64.f, 64.f };
	if (!ResourceOf(GetTexture(S.Texture)))
	{
		return false;
	}
	return DrawNineSlice(D, S, 8.f, X, Y, W, H, FMath::Max(2.f, D.S(6.f)), FLinearColor(1.f, 1.f, 1.f, Alpha));
}

bool ArenaArtHUD::DrawMenuArrow(const FArenaDraw& D, bool bRight, float CX, float CY, float Size, EButtonState State)
{
	const TCHAR* Path = ArrowTexture(bRight, State);
	if (!ResourceOf(GetTexture(Path)))
	{
		return false;
	}
	return DrawTexture(D, Path, FMath::RoundToFloat(CX - Size * 0.5f), FMath::RoundToFloat(CY - Size * 0.5f), Size, Size,
		FLinearColor(1.f, 1.f, 1.f, State == EButtonState::Disabled ? 0.35f : 1.f));
}
