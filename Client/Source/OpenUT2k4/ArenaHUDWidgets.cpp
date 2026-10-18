#include "ArenaHUDWidgets.h"
#include "ArenaArtHUD.h"
#include "ArenaUserSettings.h"

#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace ArenaHudUTLocal
{
	using namespace ArenaHudUT;

	struct FTexInfo
	{
		const TCHAR* Path;
		float W, H;
	};

	/** Texture of each ETex, relative to /Game/Replacements or /Game/Imported, with the UE2 texture size. */
	FTexInfo TexInfo(ETex Tex)
	{
		switch (Tex)
		{
		case ETex::Hud:
		case ETex::HudPulse:
		case ETex::HudAlertSlow:     return { TEXT("Textures/HudContent/Generic_HUD"), 512.f, 512.f };
		case ETex::GlowPulse:        return { TEXT("Textures/HudContent/Generic_GlowCircle"), 64.f, 64.f };
		case ETex::Links:            return { TEXT("Textures/HudContent/Generic_Links01"), 128.f, 64.f };
		case ETex::DomRingRot:       return { TEXT("Textures/HudContent/Reticles_DomRing"), 128.f, 128.f };
		case ETex::SkinA:            return { TEXT("Textures/InterfaceContent/HUD_SkinA"), 1024.f, 1024.f };
		case ETex::ASObjectiveFinal: return { TEXT("Textures/AS_FX_TX/Icons_ScoreBoard_Objective_Final"), 128.f, 128.f };
		default:                     return { nullptr, 1.f, 1.f };
		}
	}

	const FTexture* ResourceOf(const TCHAR* Path)
	{
		UTexture2D* T = Path ? ArenaArtHUD::GetTexture(Path) : nullptr;
		return T ? T->GetResource() : nullptr;
	}

	void Tile(UCanvas* Canvas, const FTexture* Res, float X, float Y, float W, float H, const FVector2D& UV0, const FVector2D& UV1,
		const FLinearColor& Color, float RotationDeg = 0.f, bool bAdditive = false)
	{
		if (!Canvas || !Res || FMath::Abs(W) <= 0.f || FMath::Abs(H) <= 0.f || Color.A <= 0.f)
		{
			return;
		}
		FCanvasTileItem Item(FVector2D(X, Y), Res, FVector2D(W, H), UV0, UV1, Color);
		Item.BlendMode = bAdditive ? SE_BLEND_Additive : SE_BLEND_Translucent;
		if (RotationDeg != 0.f)
		{
			Item.Rotation = FRotator(0.f, RotationDeg, 0.f);
			Item.PivotPoint = FVector2D(0.5f, 0.5f);
		}
		Canvas->DrawItem(Item);
	}

	/** A sRGB colour scaled in gamma space (like UE2's fixed function modulation), alpha kept. */
	FLinearColor Modulated(const FColor& C, float RGBScale)
	{
		const float S = FMath::Clamp(RGBScale, 0.f, 1.f);
		const FColor Scaled(uint8(FMath::RoundToInt(C.R * S)), uint8(FMath::RoundToInt(C.G * S)), uint8(FMath::RoundToInt(C.B * S)), C.A);
		return FLinearColor(Scaled);
	}

	/** Brightness factor of the animated HUD materials. */
	float TexFade(ETex Tex, float Time)
	{
		switch (Tex)
		{
		case ETex::HudPulse:
		case ETex::Links:        return FadePulse(Time, 0.1f);
		case ETex::HudAlertSlow: return FadePulse(Time, 0.4f);
		default:                 return 1.f;
		}
	}

	/** DigitsBig: HUDContent.Generic.HUD TextureCoords[0..10] = 39 px cells on row 0-38, '-' at 390. */
	FIntBox DigitCoords(int32 Index)
	{
		const int32 X1 = 39 * FMath::Clamp(Index, 0, 10);
		return FIntBox{ X1, 0, X1 + 38, 38 };
	}
}


namespace ArenaHudUT
{
	FHudFrame::FHudFrame(const FArenaDraw& InD, int32 InTeamIndex, float InTime)
		: D(&InD)
		, W(InD.W())
		, H(InD.H())
		, TeamIndex(InTeamIndex == 0 ? 0 : 1)
		, Time(InTime)
	{
		HudScale = FMath::Clamp(UArenaUserSettings::Get()->HudScale, 0.5f, 2.f);
		HudRegion(W, H, RegionX0, RegionW);
		// UT2004's ResScaleY = SizeY / 480; a region narrower than 4:3 fits by its width instead
		ResScale = FMath::Max(0.01f, FMath::Min(H / 480.f, RegionW / 640.f));
		TotalScale = HudCanvasScale * ResScale * HudScale;
		FrameW = 640.f * ResScale;
	}

	float FHudFrame::ScreenX(float PosX, EAnchor Anchor) const
	{
		if (Anchor == EAnchor::Auto)
		{
			Anchor = PosX < 0.25f ? EAnchor::Left : (PosX > 0.75f ? EAnchor::Right : EAnchor::Center);
		}
		const float Edge = (1.f - HudCanvasScale) * 0.5f * FrameW;
		switch (Anchor)
		{
		case EAnchor::Left:  return RegionX0 + Edge + PosX * HudCanvasScale * FrameW;
		case EAnchor::Right: return RegionX0 + RegionW - Edge - (1.f - PosX) * HudCanvasScale * FrameW;
		default:             return W * 0.5f + (PosX - 0.5f) * HudCanvasScale * FrameW;
		}
	}

	float FHudFrame::ScreenY(float PosY) const
	{
		return PosY * HudCanvasScale * H + (1.f - HudCanvasScale) * 0.5f * H;
	}

	const TCHAR* TexPath(ETex Tex)
	{
		return ArenaHudUTLocal::TexInfo(Tex).Path;
	}

	bool HasArt()
	{
		return ArenaArtHUD::HasHudArt();
	}

	float FadePulse(float Time, float Period)
	{
		return Period > 0.f ? 0.5f + 0.5f * FMath::Cos(PI * Time / Period) : 1.f;
	}

	int32 UIPixelScale(float CanvasH)
	{
		return FMath::Max(1, FMath::RoundToInt(CanvasH / 1080.f));
	}

	void HudRegion(float W, float H, float& OutX0, float& OutWidth)
	{
		const UArenaUserSettings* S = UArenaUserSettings::Get();
		int32 Mode = S->HudWidth;
		float Percent = S->HudWidthPercent;
#if !UE_BUILD_SHIPPING
		static int32 TestMode = -2;
		static float TestPercent = 100.f;
		if (TestMode == -2)
		{
			TestMode = -1;
			FParse::Value(FCommandLine::Get(), TEXT("HudWidth="), TestMode);
			FParse::Value(FCommandLine::Get(), TEXT("HudWidthPercent="), TestPercent);
		}
		if (TestMode >= 0)
		{
			Mode = TestMode;
			Percent = TestPercent;
		}
#endif
		float RW = W;
		switch (Mode)
		{
		case 1: RW = H * 21.f / 9.f; break;
		case 2: RW = H * 16.f / 9.f; break;
		case 3: RW = W * FMath::Clamp(Percent, 50.f, 100.f) / 100.f; break;
		default: break;
		}
		OutWidth = FMath::RoundToFloat(FMath::Clamp(RW, FMath::Min(W, H * 4.f / 3.f), W));
		OutX0 = FMath::RoundToFloat((W - OutWidth) * 0.5f);
	}

	static void ApplyPivot(uint8 Pivot, float& X, float& Y, float W, float H)
	{
		switch (Pivot)
		{
		case DP_UpperMiddle:  X -= W * 0.5f; break;
		case DP_UpperRight:   X -= W; break;
		case DP_MiddleRight:  X -= W; Y -= H * 0.5f; break;
		case DP_LowerRight:   X -= W; Y -= H; break;
		case DP_LowerMiddle:  X -= W * 0.5f; Y -= H; break;
		case DP_LowerLeft:    Y -= H; break;
		case DP_MiddleLeft:   Y -= H * 0.5f; break;
		case DP_MiddleMiddle: X -= W * 0.5f; Y -= H * 0.5f; break;
		default: break;
		}
	}

	struct FPlaced
	{
		FRect R;
		float U, V, UL, VL;
	};

	static FPlaced Place(const FHudFrame& F, const FSpriteWidget& Wd)
	{
		const float S = Wd.TextureScale * F.TotalScale;
		FPlaced P;
		P.R.W = FMath::Abs(float(Wd.Coords.X2 - Wd.Coords.X1 + 1)) * S;
		P.R.H = FMath::Abs(float(Wd.Coords.Y2 - Wd.Coords.Y1 + 1)) * S;
		P.R.X = F.ScreenX(Wd.PosX, Wd.Anchor) + float(Wd.OffsetX) * S;
		P.R.Y = F.ScreenY(Wd.PosY) + float(Wd.OffsetY) * S;
		ApplyPivot(Wd.Pivot, P.R.X, P.R.Y, P.R.W, P.R.H);
		P.U = float(Wd.Coords.X1);
		P.V = float(Wd.Coords.Y1);
		P.UL = float(Wd.Coords.X2 - Wd.Coords.X1);
		P.VL = float(Wd.Coords.Y2 - Wd.Coords.Y1);
		const float Sc = FMath::Clamp(Wd.Scale, 0.f, 1.f);
		switch (Wd.ScaleMode)
		{
		case SM_Right: // keeps the left part
			P.R.W *= Sc;
			P.UL *= Sc;
			break;
		case SM_Left: // keeps the right part
			P.R.X += P.R.W * (1.f - Sc);
			P.R.W *= Sc;
			P.U += P.UL * (1.f - Sc);
			P.UL *= Sc;
			break;
		case SM_Up: // keeps the bottom part
			P.R.Y += P.R.H * (1.f - Sc);
			P.R.H *= Sc;
			P.V += P.VL * (1.f - Sc);
			P.VL *= Sc;
			break;
		case SM_Down: // keeps the top part
			P.R.H *= Sc;
			P.VL *= Sc;
			break;
		default:
			break;
		}
		return P;
	}

	FRect SpriteRect(const FHudFrame& F, const FSpriteWidget& W)
	{
		return Place(F, W).R;
	}

	bool DrawTilePath(const FArenaDraw& D, const TCHAR* Path, float TexW, float TexH, float X, float Y, float W, float H,
		float U, float V, float UL, float VL, const FLinearColor& Color, float RotationDeg, bool bAdditive)
	{
		const FTexture* Res = ArenaHudUTLocal::ResourceOf(Path);
		if (!Res || !D.Canvas)
		{
			return false;
		}
		// half a texel in from the region edges so neighbouring atlas cells do not bleed in
		const float InU = UL >= 0.f ? 0.5f : -0.5f;
		const float InV = VL >= 0.f ? 0.5f : -0.5f;
		const FVector2D UV0((U + InU) / TexW, (V + InV) / TexH);
		const FVector2D UV1((U + UL - InU) / TexW, (V + VL - InV) / TexH);
		ArenaHudUTLocal::Tile(D.Canvas, Res, X, Y, W, H, UV0, UV1, Color, RotationDeg, bAdditive);
		return true;
	}

	bool DrawTile(const FArenaDraw& D, ETex Tex, float X, float Y, float W, float H, float U, float V, float UL, float VL,
		const FLinearColor& Color, float Time)
	{
		const ArenaHudUTLocal::FTexInfo Info = ArenaHudUTLocal::TexInfo(Tex);
		if (!Info.Path)
		{
			return false;
		}
		FLinearColor C = Color;
		const float Fade = ArenaHudUTLocal::TexFade(Tex, Time);
		if (Fade < 1.f)
		{
			FColor S = C.ToFColorSRGB();
			C = ArenaHudUTLocal::Modulated(S, Fade);
			C.A = Color.A;
		}
		if (Tex == ETex::GlowPulse)
		{
			// TexScaler 0.5 about the centre, then a TexOscillator stretching the UVs (rate 3, amplitude 0.5)
			const float Stretch = 1.f + 0.5f * FMath::Sin(2.f * PI * 3.f * Time);
			const float CU = U + UL * 0.5f, CV = V + VL * 0.5f;
			return DrawTilePath(D, Info.Path, Info.W, Info.H, X, Y, W, H, CU - UL * 0.25f * Stretch, CV - VL * 0.25f * Stretch,
				UL * 0.5f * Stretch, VL * 0.5f * Stretch, C);
		}
		const float Rot = Tex == ETex::DomRingRot ? FMath::Fmod(Time * 26000.f / 65536.f * 360.f, 360.f) : 0.f;
		return DrawTilePath(D, Info.Path, Info.W, Info.H, X, Y, W, H, U, V, UL, VL, C, Rot);
	}

	FRect DrawSprite(const FHudFrame& F, const FSpriteWidget& Wd)
	{
		if (!F.D || Wd.Tex == ETex::None)
		{
			return FRect();
		}
		const FPlaced P = Place(F, Wd);
		if (P.R.W <= 0.f || P.R.H <= 0.f)
		{
			return P.R;
		}
		const FColor& Tint = Wd.Tints[F.TeamIndex];
		if (!DrawTile(*F.D, Wd.Tex, P.R.X, P.R.Y, P.R.W, P.R.H, P.U, P.V, P.UL, P.VL, FLinearColor(Tint), F.Time))
		{
			return FRect();
		}
		return P.R;
	}

	FRect DrawNumeric(const FHudFrame& F, const FNumericWidget& N)
	{
		if (!F.D)
		{
			return FRect();
		}
		TArray<int32, TInlineAllocator<12>> Glyphs;
		const FString Str = FString::FromInt(FMath::Abs(N.Value));
		if (N.Value < 0)
		{
			Glyphs.Add(10);
		}
		int32 Pad = FMath::Max(0, N.MinDigitCount - Str.Len() - (N.Value < 0 ? 1 : 0));
		for (; Pad > 0; --Pad)
		{
			Glyphs.Add(N.bPadWithZeroes ? 0 : -1);
		}
		for (const TCHAR C : Str)
		{
			Glyphs.Add(C - TCHAR('0'));
		}
		const float S = N.TextureScale * F.TotalScale;
		const float CellW = 39.f * S;
		const float CellH = 39.f * S;
		FRect R;
		R.W = CellW * Glyphs.Num();
		R.H = CellH;
		R.X = F.ScreenX(N.PosX, N.Anchor) + float(N.OffsetX) * S;
		R.Y = F.ScreenY(N.PosY) + float(N.OffsetY) * S;
		ApplyPivot(N.Pivot, R.X, R.Y, R.W, R.H);
		const FColor& Tint = N.Tints[F.TeamIndex];
		const ETex Tex = N.bPulseDigits ? ETex::HudAlertSlow : ETex::Hud;
		float X = R.X;
		for (const int32 G : Glyphs)
		{
			if (G >= 0)
			{
				const FIntBox B = ArenaHudUTLocal::DigitCoords(G);
				if (!DrawTile(*F.D, Tex, X, R.Y, CellW, CellH, float(B.X1), float(B.Y1), float(B.X2 - B.X1), float(B.Y2 - B.Y1), FLinearColor(Tint), F.Time))
				{
					return FRect();
				}
			}
			X += CellW;
		}
		return R;
	}

	bool DrawTileStretched(const FArenaDraw& D, const TCHAR* Path, float X, float Y, float W, float H, const FLinearColor& Color)
	{
		UTexture2D* T = Path ? ArenaArtHUD::GetTexture(Path) : nullptr;
		const FTexture* Res = T ? T->GetResource() : nullptr;
		if (!Res || !D.Canvas || W <= 0.f || H <= 0.f)
		{
			return Res != nullptr;
		}
		const float TW = float(FMath::Max(1, T->GetSizeX()));
		const float TH = float(FMath::Max(1, T->GetSizeY()));
		const float Px = float(UIPixelScale(D.H()));
		// corners: a quarter of the texture each, at most half the box
		const float CW = FMath::Min(TW * 0.5f * Px, W * 0.5f);
		const float CH = FMath::Min(TH * 0.5f * Px, H * 0.5f);
		const float CU = CW / Px / TW, CV = CH / Px / TH;
		const float DX[4] = { X, X + CW, X + W - CW, X + W };
		const float DY[4] = { Y, Y + CH, Y + H - CH, Y + H };
		const float SU[4] = { 0.f, CU, 1.f - CU, 1.f };
		const float SV[4] = { 0.f, CV, 1.f - CV, 1.f };
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
				// the middle row / column stretch the texture's centre texels
				const float U0 = Col == 1 ? 0.5f - 0.5f / TW : SU[Col];
				const float U1 = Col == 1 ? 0.5f + 0.5f / TW : SU[Col + 1];
				const float V0 = Row == 1 ? 0.5f - 0.5f / TH : SV[Row];
				const float V1 = Row == 1 ? 0.5f + 0.5f / TH : SV[Row + 1];
				ArenaHudUTLocal::Tile(D.Canvas, Res, DX[Col], DY[Row], PW, PH, FVector2D(U0, V0), FVector2D(U1, V1), Color);
			}
		}
		return true;
	}

	void Draw2DLocationDot(const FHudFrame& F, float RelYawRad, float OffsetX, float OffsetY, float ScaleX, float ScaleY, const FColor& Color, ETex Tex)
	{
		if (!F.D)
		{
			return;
		}
		// SetPos(OffsetX * ClipX + ScaleX * ClipX * Sin(Angle), OffsetY * ClipY - ScaleY * ClipY * Cos(Angle));
		// DrawTile(LocationDot, 24 * ClipX * HudScale / 1600, ..., 340, 432, 78, 78). ClipX fractions use the 4:3 frame.
		const float X = F.W * 0.5f + (OffsetX - 0.5f) * F.FrameW + ScaleX * F.FrameW * FMath::Sin(RelYawRad);
		const float Y = OffsetY * F.H - ScaleY * F.H * FMath::Cos(RelYawRad);
		const float Size = 24.f * F.FrameW * F.HudScale / 1600.f;
		DrawTile(*F.D, Tex, X, Y, Size, Size, 340.f, 432.f, 78.f, 78.f, FLinearColor(Color), F.Time);
	}
}

void ArenaHudUT::DrawWarheadScope(const FArenaDraw& D, float RollDeg, float ClimbRateCm, bool bStaticScreen, float Time)
{
	using ArenaArtHUD::FHudSprite;
	if (!D.Canvas)
	{
		return;
	}
	const float SX = D.W();
	const float SY = D.H();
	const FLinearColor White(1.f, 1.f, 1.f, 1.f);
	if (bStaticScreen)
	{
		// ScreenNoiseFB: the noise texture tiled over the screen, jumping every frame
		const FHudSprite Noise{ TEXT("Textures/XGameShaders/ZoomFX_ScreenNoise"), 0.f, 0.f, 512.f, 512.f, 512.f, 512.f };
		const float J = FMath::Frac(Time * 37.f) * 512.f;
		const FHudSprite N2{ Noise.Texture, J, J * 0.7f, J + 512.f, J * 0.7f + 512.f, 512.f, 512.f };
		if (!ArenaArtHUD::DrawSprite(D, N2, 0.f, 0.f, SX, SY, White))
		{
			D.Rect(0.f, 0.f, SX, SY, FLinearColor(0.25f, 0.25f, 0.25f, 1.f));
		}
		return;
	}
	// ClipX >= ClipY: offset = ClipX / ClipY; four mirrored quadrants of each 512 x 512 corner texture
	const float Off = SX / FMath::Max(1.f, SY);
	// UE2 draws U from 512 x (1 - offset): the part left of U = 0 is the clamped texture edge; here that part is
	// filled black (the edge texture is black outside the scope) and the corner texture covers U 0..512
	const float QW = SX * 0.5f;
	const float QH = SY * 0.5f;
	const float Side = QW * (1.f - 1.f / FMath::Max(Off, 1.f)); // screen width of the clamped part of a quadrant
	auto Quadrants = [&](const TCHAR* Tex, bool bFillSides)
	{
		const FHudSprite TL{ Tex, 0.f, 0.f, 512.f, 512.f, 512.f, 512.f };
		const FHudSprite TR{ Tex, 512.f, 0.f, 0.f, 512.f, 512.f, 512.f };
		const FHudSprite BL{ Tex, 0.f, 512.f, 512.f, 0.f, 512.f, 512.f };
		const FHudSprite BR{ Tex, 512.f, 512.f, 0.f, 0.f, 512.f, 512.f };
		ArenaArtHUD::DrawSprite(D, TL, Side, 0.f, QW - Side, QH, White);
		ArenaArtHUD::DrawSprite(D, TR, QW, 0.f, QW - Side, QH, White);
		ArenaArtHUD::DrawSprite(D, BL, Side, QH, QW - Side, QH, White);
		ArenaArtHUD::DrawSprite(D, BR, QW, QH, QW - Side, QH, White);
		if (bFillSides && Side > 0.f)
		{
			D.Rect(0.f, 0.f, Side, SY, FLinearColor::Black);
			D.Rect(SX - Side, 0.f, Side, SY, FLinearColor::Black);
		}
	};
	Quadrants(TEXT("Textures/2K4Hud/ZoomFX_RedeemerOuterEdge"), true);
	Quadrants(TEXT("Textures/2K4Hud/ZoomFX_RedeemerInnerScope"), false);
	// the outer scope (a TexRotator turned by the warhead's roll), centred
	const FHudSprite Outer{ TEXT("Textures/2K4Hud/ZoomFX_RedeemerOuterScope"), 0.f, 0.f, 1024.f, 1024.f, 1024.f, 1024.f };
	ArenaArtHUD::DrawSprite(D, Outer, (SX - SY) * 0.5f, 0.f, SY, SY, White, false, RollDeg);
	// altitude strips (VariableTexPanner: PanRate = clamp(Velocity.Z x 0.00175, -10, 10) per second)
	const float Pan = FMath::Frac(Time * FMath::Clamp(ClimbRateCm * 0.5f * 0.00175f, -10.f, 10.f)) * 128.f;
	const FHudSprite AltL{ TEXT("Textures/2K4Hud/ZoomFX_RDM_Altitude"), 0.f, Pan, 128.f, Pan + 128.f, 128.f, 128.f };
	const FHudSprite AltR{ AltL.Texture, 128.f, Pan, 0.f, Pan + 128.f, 128.f, 128.f };
	const float StripW = SX / (8.f * Off);
	const float StripH = SY / 8.f;
	const float Y = SY * 451.f / 1024.f;
	const float XL = (512.f * (Off - 1.f) + 383.f) * SX / (1024.f * Off);
	const float XR = (512.f * (Off - 1.f) + 383.f + 2.f * (512.f - 383.f) - 128.f) * SX / (1024.f * Off);
	ArenaArtHUD::DrawSprite(D, AltL, XL, Y, StripW, StripH, White);
	ArenaArtHUD::DrawSprite(D, AltR, XR, Y, StripW, StripH, White);
}

