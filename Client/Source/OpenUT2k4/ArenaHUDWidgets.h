#pragma once

#include "CoreMinimal.h"

struct FArenaDraw;

/**
 * UT2004's data-driven HUD widgets (XInterface.HudBase SpriteWidget / NumericWidget / DigitSet) re-implemented for the
 * canvas HUD, with the stock widget values as tables (ArenaHUDWidgets.cpp). The textures load at runtime from the
 * player's converted content (ArenaArtHUD::GetTexture); nothing is shipped.
 *
 * Widget math (HudBase native DrawSpriteWidget, as re-done in script by HUD_Assault.DrawTeamVS):
 *   TotalScale  = HudCanvasScale (0.95) * ResScale * HudScale
 *   size        = |TextureCoords.X2 - X1 + 1| * TextureScale * TotalScale (same for Y)
 *   screen pos  = Pos * HudCanvasScale * Clip + (1 - HudCanvasScale) / 2 * Clip + Offset * TextureScale * TotalScale,
 *                 then moved by the DrawPivot (EDrawPivot 0..8, HudBase.GetScreenCoords)
 *   colour      = Tints[TeamIndex] (TeamIndex 0 red, 1 blue and also every non-team game), RenderStyle 5 = alpha blend
 * UT2004 used ResScaleX = SizeX / 640 for widths and ResScaleY = SizeY / 480 for heights, which stretches every widget
 * horizontally on wide screens. Here both use the height (no stretching), and the horizontal position keeps the
 * widget's UT2004 anchor: widgets at PosX 0 / 1 (or listed as left / right) stay at that screen edge, the rest keep
 * their distance from the centre, all measured in a 4:3 frame of the screen height (the frame UT2004 was laid out in).
 * At 4:3 this is exactly UT2004's placement.
 */
namespace ArenaHudUT
{
	/** HUD.HudCanvasScale: everything is laid out in the middle 95% of the canvas. */
	constexpr float HudCanvasScale = 0.95f;

	/** Texture of a widget, with the UE2 size of the source texture (TextureCoords are in its texels). */
	enum class ETex : uint8
	{
		None,
		/** HUDContent.Generic.HUD (512 x 512) */
		Hud,
		/** HUDContent.Generic.HUDPulse: the HUD atlas times FadeColors.FastHUDPules (white <-> black, FadePeriod 0.1) */
		HudPulse,
		/** HUDContent.Generic.fbHUDAlertSlow: the HUD atlas times FadeColors.Pulse001 (FadePeriod 0.4) */
		HudAlertSlow,
		/** HUDContent.Generic.fb_Pulse001: GlowCircle (64 x 64) through a TexOscillator (stretch, rate 3, amplitude 0.5) */
		GlowPulse,
		/** HUDContent.Generic.fbLinks: Links01 times FastHUDPules */
		Links,
		/** HUDContent.Reticles.rotDomRing: DomRing (128 x 128) turning (TexRotator, Yaw 26000 per second) */
		DomRingRot,
		/** InterfaceContent.HUD.SkinA (1024 x 1024) */
		SkinA,
		/** AS_FX_TX.Icons.ScoreBoard_Objective_Final (128 x 128) */
		ASObjectiveFinal,
	};

	/** EDrawPivot */
	enum EPivot : uint8
	{
		DP_UpperLeft = 0,
		DP_UpperMiddle,
		DP_UpperRight,
		DP_MiddleRight,
		DP_LowerRight,
		DP_LowerMiddle,
		DP_LowerLeft,
		DP_MiddleLeft,
		DP_MiddleMiddle,
	};

	/** EScaleMode: Scale crops the widget (e.g. the ammo strip shows the ammo left). */
	enum EScaleMode : uint8
	{
		SM_None = 0,
		SM_Up,
		SM_Down,
		SM_Left,
		SM_Right,
	};

	/** Horizontal anchor on wide screens (see the namespace comment). Auto: PosX < 0.25 left, > 0.75 right, else centre. */
	enum class EAnchor : uint8
	{
		Auto,
		Left,
		Center,
		Right,
	};

	struct FIntBox
	{
		int32 X1 = 0, Y1 = 0, X2 = 0, Y2 = 0;
	};

	/** HudBase.SpriteWidget */
	struct FSpriteWidget
	{
		ETex Tex = ETex::Hud;
		FIntBox Coords;
		float TextureScale = 1.f;
		uint8 Pivot = DP_UpperLeft;
		float PosX = 0.f, PosY = 0.f;
		int32 OffsetX = 0, OffsetY = 0;
		uint8 ScaleMode = SM_None;
		float Scale = 1.f;
		FColor Tints[2] = { FColor::White, FColor::White };
		EAnchor Anchor = EAnchor::Auto;
	};

	/** HudBase.NumericWidget (drawn with DigitsBig) */
	struct FNumericWidget
	{
		int32 Value = 0;
		float TextureScale = 1.f;
		uint8 Pivot = DP_UpperLeft;
		float PosX = 0.f, PosY = 0.f;
		int32 OffsetX = 0, OffsetY = 0;
		int32 MinDigitCount = 0;
		bool bPadWithZeroes = false;
		FColor Tints[2] = { FColor::White, FColor::White };
		EAnchor Anchor = EAnchor::Auto;
		/** HudCTeamDeathMatch.totalLinks uses DigitsBigPulse (the digits through fbHUDAlertSlow) */
		bool bPulseDigits = false;
	};

	/** A SpriteWidget in the argument order of the defaultproperties (ScaleMode SM_Right, Scale 1). */
	inline FSpriteWidget Spr(ETex Tex, int32 X1, int32 Y1, int32 X2, int32 Y2, float TS, uint8 Pivot, float PosX, float PosY, int32 OffX, int32 OffY,
		const FColor& T0 = FColor::White, const FColor& T1 = FColor::White, EAnchor Anchor = EAnchor::Auto)
	{
		FSpriteWidget W;
		W.Tex = Tex;
		W.Coords = FIntBox{ X1, Y1, X2, Y2 };
		W.TextureScale = TS;
		W.Pivot = Pivot;
		W.PosX = PosX;
		W.PosY = PosY;
		W.OffsetX = OffX;
		W.OffsetY = OffY;
		W.ScaleMode = SM_Right;
		W.Scale = 1.f;
		W.Tints[0] = T0;
		W.Tints[1] = T1;
		W.Anchor = Anchor;
		return W;
	}

	/** A NumericWidget in the argument order of the defaultproperties. */
	inline FNumericWidget Num(float TS, uint8 Pivot, float PosX, float PosY, int32 OffX, int32 OffY, int32 MinDigits = 0, bool bPad = false,
		EAnchor Anchor = EAnchor::Auto)
	{
		FNumericWidget N;
		N.TextureScale = TS;
		N.Pivot = Pivot;
		N.PosX = PosX;
		N.PosY = PosY;
		N.OffsetX = OffX;
		N.OffsetY = OffY;
		N.MinDigitCount = MinDigits;
		N.bPadWithZeroes = bPad;
		N.Anchor = Anchor;
		return N;
	}

	/** Frame values for one canvas: scale, team, time. */
	struct OPENUT2K4_API FHudFrame
	{
		const FArenaDraw* D = nullptr;
		float W = 0.f, H = 0.f;
		/** HUD.HudScale (Settings > HUD Scale, UT2004 default 1) */
		float HudScale = 1.f;
		/** H / 480 (UT2004's ResScaleY), used for both axes */
		float ResScale = 1.f;
		/** HudCanvasScale * ResScale * HudScale */
		float TotalScale = 1.f;
		/** Width of the 4:3 frame of the screen height */
		float FrameW = 0.f;
		/** HudCDeathmatch.TeamIndex: 0 red team, 1 blue team or no team */
		int32 TeamIndex = 1;
		float Time = 0.f;
		/** The HUD region (Settings > HUD Width, HudRegion): edge anchored widgets sit at its edges */
		float RegionX0 = 0.f, RegionW = 0.f;
		/** HudColorRed / HudColorBlue: the team tints of the boxes and the weapon bar (HudMutant swaps in MutantHudTint) */
		FColor HudRed = FColor(200, 0, 0, 255);
		FColor HudBlue = FColor(50, 64, 200, 255);

		FHudFrame() = default;
		FHudFrame(const FArenaDraw& InD, int32 InTeamIndex, float InTime);

		/** HudBase.GetScreenCoords placement of a canvas fraction (no pivot), with the wide screen anchor. */
		float ScreenX(float PosX, EAnchor Anchor = EAnchor::Auto) const;
		float ScreenY(float PosY) const;
		/** UT2004 sizes that were "fraction of ClipX" (radar, icons): the same fraction of the 4:3 frame width. */
		float ClipX43() const { return FrameW; }
	};

	/** Screen rectangle a widget would cover (after pivot and scale mode). */
	struct FRect
	{
		float X = 0.f, Y = 0.f, W = 0.f, H = 0.f;
	};

	// ---- drawing ----
	/** True when the HUD atlas is available (the UT2004 HUD is drawn; otherwise callers use their procedural look). */
	OPENUT2K4_API bool HasArt();
	/** HudBase.DrawSpriteWidget. Returns the drawn rectangle (zero size when the texture is missing). */
	OPENUT2K4_API FRect DrawSprite(const FHudFrame& F, const FSpriteWidget& W);
	/** Same placement as DrawSprite, without drawing. */
	OPENUT2K4_API FRect SpriteRect(const FHudFrame& F, const FSpriteWidget& W);
	/** HudBase.DrawNumericWidget with DigitsBig (HUDContent.Generic.HUD digits 0-9 and '-', 39 x 39 cells). */
	OPENUT2K4_API FRect DrawNumeric(const FHudFrame& F, const FNumericWidget& W);
	/** Canvas.DrawTile of a texture region (texels of the UE2 texture) at a screen rectangle, alpha blended. */
	OPENUT2K4_API bool DrawTile(const FArenaDraw& D, ETex Tex, float X, float Y, float W, float H, float U, float V, float UL, float VL,
		const FLinearColor& Color, float Time = 0.f);
	/** Same for any texture by path (UE2 size given for the texel coordinates). */
	OPENUT2K4_API bool DrawTilePath(const FArenaDraw& D, const TCHAR* Path, float TexW, float TexH, float X, float Y, float W, float H,
		float U, float V, float UL, float VL, const FLinearColor& Color, float RotationDeg = 0.f, bool bAdditive = false);
	/** Texture path of an ETex (relative to /Game/Replacements or /Game/Imported), nullptr for None. */
	OPENUT2K4_API const TCHAR* TexPath(ETex Tex);
	/**
	 * Canvas.DrawTileStretched: the texture's four quarters at the corners (drawn at Pixel scale, UE2 drew them 1:1;
	 * bigger screens use a whole multiple, see UIPixelScale), the middle stretched.
	 */
	OPENUT2K4_API bool DrawTileStretched(const FArenaDraw& D, const TCHAR* Path, float X, float Y, float W, float H, const FLinearColor& Color);
	/**
	 * The centred part of a W x H screen the edge anchored HUD widgets use (Settings > HUD Width): the full width, the
	 * width of a 21:9 or 16:9 screen of that height (never more than W), or a percentage of W. Non-shipping test override:
	 * -HudWidth=<0..3> [-HudWidthPercent=<50..100>].
	 */
	OPENUT2K4_API void HudRegion(float W, float H, float& OutX0, float& OutWidth);
	/** Whole multiple used for things UE2 drew 1:1 in pixels (bitmap fonts, DrawTileStretched corners): Max(1, floor(H / 1080)). */
	OPENUT2K4_API int32 UIPixelScale(float CanvasH);

	/** HUD colours of HudCDeathmatch / HUD defaults (sRGB). */
	namespace Colors
	{
		inline const FColor HudRed(200, 0, 0, 255);      // HudColorRed / HudColorTeam[0]
		inline const FColor HudBlue(50, 64, 200, 255);   // HudColorBlue / HudColorTeam[1]
		inline const FColor HudBlack(0, 0, 0, 255);      // HudColorBlack
		inline const FColor HighLight(255, 160, 0, 255); // HudColorHighLight
		inline const FColor HighLightAS(255, 192, 32, 255); // HUD_Assault HudColorHighLight
		inline const FColor Normal(255, 255, 255, 255);  // HudColorNormal
		inline const FColor White(255, 255, 255, 255);
		inline const FColor Red(255, 0, 0, 255);
		inline const FColor Green(0, 255, 0, 255);
		inline const FColor Gold(255, 255, 0, 255);
		inline const FColor Cyan(0, 255, 255, 255);
		inline const FColor Turq(0, 128, 255, 255);
		inline const FColor Blue(0, 0, 255, 255);
		inline const FColor Gray(200, 200, 200, 255);
		inline FColor Team(int32 TeamIndex) { return TeamIndex == 0 ? HudRed : HudBlue; }
	}

	/** FadeColor value 0..1 (Color1 white at 1 -> Color2 black at 0) of a FadePeriod at Time. */
	OPENUT2K4_API float FadePulse(float Time, float Period);

	/**
	 * HudBase.Draw2DLocationDot: a small arrow on a circle around (OffsetX, OffsetY) (canvas fractions) pointing to
	 * WorldDir (relative yaw in radians, 0 = straight ahead). Generic_HUD 340,432 78 x 78 region, 24 px at 1600 wide.
	 */
	OPENUT2K4_API void Draw2DLocationDot(const FHudFrame& F, float RelYawRad, float OffsetX, float OffsetY, float ScaleX, float ScaleY,
		const FColor& Color, ETex Tex = ETex::Hud);

	/**
	 * RedeemerWarhead.DrawHUD: the guided warhead's scope (2K4Hud.ZoomFX: RedeemerOuterEdge / InnerScope quadrants,
	 * the outer scope ring turned with the warhead's roll, the altitude strips panned by the climb rate) or, after the
	 * explosion, full-screen static (XGameShaders.ZoomFX.ScreenNoise). Laid out like UE2's for a wide canvas.
	 */
	OPENUT2K4_API void DrawWarheadScope(const FArenaDraw& D, float RollDeg, float ClimbRateCm, bool bStaticScreen, float Time);
}
