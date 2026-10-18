#pragma once

#include "CoreMinimal.h"
#include "ArenaHUD.h"

class UTexture2D;
class UClass;

/**
 * Optional interface art for the canvas HUD and the menus, loaded at runtime by soft path through ArenaArt
 * (/Game/Replacements/<path> first, then /Game/Imported/<path>; nothing is hard-referenced).
 *
 * Every draw function returns false and draws nothing when its texture is missing or arena.ImportedArt is 0,
 * so callers keep their procedural look as the fallback.
 *
 * Atlas regions are given in pixels of the source atlas (AtlasW x AtlasH) and turned into UVs, so a replacement
 * texture of a different resolution keeps working as long as it keeps the layout. The regions of the HUD atlas
 * come from the source game's HUD class defaults (widget TextureCoords), see README "Interface art".
 */
namespace ArenaArtHUD
{
	/**
	 * A rectangle of a texture, in source pixels. X1 > X2 mirrors the image.
	 * The draw functions' bAdditive flag brightens the tint for glows; everything is alpha blended because these
	 * textures keep their shape in the alpha channel.
	 */
	struct FHudSprite
	{
		const TCHAR* Texture = nullptr;
		float X1 = 0.f, Y1 = 0.f, X2 = 0.f, Y2 = 0.f;
		float AtlasW = 1.f, AtlasH = 1.f;

		float Width() const { return FMath::Abs(X2 - X1); }
		float Height() const { return FMath::Abs(Y2 - Y1); }
		/** Width / height */
		float Aspect() const { return Height() > 0.f ? Width() / Height() : 1.f; }
	};

	// ---- texture paths (relative to /Game/Replacements or /Game/Imported) ----
	namespace Tex
	{
		inline const TCHAR* HudAtlas()      { return TEXT("Textures/HudContent/Generic_HUD"); }
		inline const TCHAR* HudSkin()       { return TEXT("Textures/InterfaceContent/HUD_SkinA"); }
		inline const TCHAR* NodeIcons()     { return TEXT("Textures/ONSInterface-TX/NewHUDicons"); }
		inline const TCHAR* MapBorder()     { return TEXT("Textures/ONSInterface-TX/MapBorderTEX"); }
		inline const TCHAR* DomRing()       { return TEXT("Textures/HudContent/Reticles_DomRing"); }
		inline const TCHAR* GlowCircle()    { return TEXT("Textures/HudContent/Generic_GlowCircle"); }
		inline const TCHAR* ScoreBoxNeutral() { return TEXT("Textures/InterfaceContent/Menu_ScoreBoxA"); }
		inline const TCHAR* ScoreBoxBlue()  { return TEXT("Textures/InterfaceContent/Menu_ScoreBoxB"); }
		inline const TCHAR* ScoreBoxRed()   { return TEXT("Textures/InterfaceContent/Menu_ScoreBoxC"); }
		inline const TCHAR* MenuBackground(){ return TEXT("Textures/2K4Menus/MainMenu_mmbgnd"); }
		/** Game logo of the player's own UT2004 data (1024x512, artwork in the top 410 rows). Never shipped. */
		inline const TCHAR* MenuLogo()      { return TEXT("Textures/2K4Menus/MainMenu_2K4Logo"); }
		inline const TCHAR* Scanlines()     { return TEXT("Textures/2K4Menus/BkRenders_Scanlines"); }
		inline const TCHAR* MenuPanel()     { return TEXT("Textures/2K4Menus/NewControls_Display99"); }
		inline const TCHAR* MenuHeader()    { return TEXT("Textures/2K4Menus/NewControls_NewHeader"); }
	}

	// ---- named regions of the HUD atlases ----
	namespace Sprites
	{
		inline FHudSprite Atlas(float X1, float Y1, float X2, float Y2) { return FHudSprite{ Tex::HudAtlas(), X1, Y1, X2, Y2, 512.f, 512.f }; }
		inline FHudSprite Skin(float X1, float Y1, float X2, float Y2)  { return FHudSprite{ Tex::HudSkin(), X1, Y1, X2, Y2, 1024.f, 1024.f }; }
		inline FHudSprite Node(float X1, float Y1, float X2, float Y2)  { return FHudSprite{ Tex::NodeIcons(), X1, Y1, X2, Y2, 128.f, 128.f }; }

		/** Health / shield / ammo box (HudBorderHealth) */
		inline FHudSprite Box()            { return Atlas(0, 110, 166, 163); }
		/** Low-value alert outline (HudHealthALERT) */
		inline FHudSprite BoxAlert()       { return Atlas(125, 458, 291, 511); }
		/** Small outline frame */
		inline FHudSprite SmallFrame()     { return Atlas(127, 400, 220, 452); }
		/** Rounded capsule behind counters (AdrenalineBackground / TimerBackground) */
		inline FHudSprite Capsule()        { return Atlas(168, 211, 334, 255); }
		/** Dark disc at the end of a capsule (AdrenalineBackgroundDisc) */
		inline FHudSprite Disc()           { return Atlas(119, 258, 173, 313); }
		inline FHudSprite HealthIcon()     { return Atlas(74, 165, 123, 216); }
		inline FHudSprite ShieldIcon()     { return Atlas(0, 248, 68, 310); }
		inline FHudSprite AdrenalineIcon() { return Atlas(113, 38, 165, 106); }
		inline FHudSprite DamageIcon()     { return Atlas(0, 164, 73, 246); }
		inline FHudSprite TimerIcon()      { return Atlas(150, 356, 184, 389); }
		inline FHudSprite DigitColon()     { return Atlas(495, 91, 503, 112); }
		/** Weapon bar slot (BarBorder) and its ammo strip (BarBorderAmmoIndicator) */
		inline FHudSprite WeaponSlot()     { return Atlas(0, 39, 94, 93); }
		inline FHudSprite WeaponAmmoStrip(){ return Atlas(0, 94, 94, 109); }
		/** Segmented red-to-green meter (RechargeBar) */
		inline FHudSprite Meter()          { return Atlas(0, 313, 170, 348); }
		/** Flag (NewFlagWidgets), "taken" mark (FlagHeldWidgets), "dropped" arrow (FlagDownWidgets) */
		inline FHudSprite Flag()           { return Atlas(336, 129, 397, 170); }
		inline FHudSprite FlagTaken()      { return Atlas(341, 211, 366, 271); }
		inline FHudSprite FlagDropped()    { return Atlas(77, 271, 116, 311); }
		/** Power core (CoreWidgets) */
		inline FHudSprite Core()           { return Atlas(365, 233, 432, 313); }
		/** Domination letters */
		inline FHudSprite LetterA()        { return Atlas(2, 451, 60, 511); }
		inline FHudSprite LetterB()        { return Atlas(62, 451, 126, 511); }
		inline FHudSprite Versus()         { return Atlas(435, 0, 508, 31); }
		/** Bombing Run ball (held / at home) */
		inline FHudSprite BallGold()       { return Skin(622, 538, 738, 660); }
		inline FHudSprite BallSilver()     { return Skin(742, 538, 858, 660); }
		inline FHudSprite Skull()          { return Skin(952, 449, 1017, 524); }
		/** Onslaught node map icons */
		inline FHudSprite NodeDisc()       { return Node(0, 0, 32, 32); }
		inline FHudSprite NodeBuilding()   { return Node(32, 0, 64, 32); }
		inline FHudSprite NodeLocked()     { return Node(0, 32, 32, 64); }
		inline FHudSprite NodeTarget()     { return Node(32, 32, 64, 64); }
		inline FHudSprite NodeCore()       { return Node(64, 0, 128, 64); }
		inline FHudSprite NodeAttack()     { return Node(0, 64, 64, 128); }
		inline FHudSprite NodeArrows()     { return Node(64, 64, 128, 128); }
		/** Whole-texture sprites */
		inline FHudSprite DomRing()        { return FHudSprite{ Tex::DomRing(), 0, 0, 128, 128, 128.f, 128.f }; }
		inline FHudSprite Glow()           { return FHudSprite{ Tex::GlowCircle(), 0, 0, 64, 64, 64.f, 64.f }; }
	}

	/** Texture by relative path (cached; misses are cached too). Its mips are kept resident for crisp canvas drawing. */
	OPENUT2K4_API UTexture2D* GetTexture(const TCHAR* Path);
	/** True when the main HUD atlas is available (and art is enabled). */
	OPENUT2K4_API bool HasHudArt();
	/** True when the menu panel / button textures are available. */
	OPENUT2K4_API bool HasMenuArt();

	// ---- drawing (screen pixels) ----
	OPENUT2K4_API bool DrawSprite(const FArenaDraw& D, const FHudSprite& S, float X, float Y, float W, float H, const FLinearColor& Tint,
		bool bAdditive = false, float RotationDeg = 0.f);
	/** Sprite with its aspect ratio kept, centred on (CX, CY), Height pixels tall. Returns false when missing. */
	OPENUT2K4_API bool DrawSpriteCentered(const FArenaDraw& D, const FHudSprite& S, float CX, float CY, float Height, const FLinearColor& Tint,
		bool bAdditive = false, float RotationDeg = 0.f);
	/** Left part (Fraction 0..1) of a sprite, e.g. a meter. */
	OPENUT2K4_API bool DrawSpriteFraction(const FArenaDraw& D, const FHudSprite& S, float X, float Y, float W, float H, float Fraction, const FLinearColor& Tint);
	/** Nine-slice: corners of SrcMargin source pixels are drawn ScreenMargin pixels wide, the rest stretches. */
	OPENUT2K4_API bool DrawNineSlice(const FArenaDraw& D, const FHudSprite& S, float SrcMargin, float X, float Y, float W, float H, float ScreenMargin,
		const FLinearColor& Tint, bool bAdditive = false);
	/** Whole texture (or a UV range of it; UVs beyond 0..1 tile). */
	OPENUT2K4_API bool DrawTexture(const FArenaDraw& D, const TCHAR* Path, float X, float Y, float W, float H, const FLinearColor& Tint,
		float U0 = 0.f, float V0 = 0.f, float U1 = 1.f, float V1 = 1.f, bool bAdditive = false);

	/** Width of a number drawn with the HUD digits at the given glyph height. */
	OPENUT2K4_API float NumberWidth(int32 Value, float Height);
	/** Number with the big HUD digits, vertically centred on CY. Returns false when the atlas is missing. */
	OPENUT2K4_API bool DrawNumber(const FArenaDraw& D, int32 Value, float X, float CY, float Height, EArenaAlign Align, const FLinearColor& Tint);
	/** "m:ss" with the HUD digits. */
	OPENUT2K4_API bool DrawClock(const FArenaDraw& D, int32 Seconds, float X, float CY, float Height, EArenaAlign Align, const FLinearColor& Tint);
	OPENUT2K4_API float ClockWidth(int32 Seconds, float Height);

	/** Translucent HUD box tinted by Accent (no accent: neutral steel). Used by FArenaDraw::Panel when available. */
	OPENUT2K4_API bool DrawHudPanel(const FArenaDraw& D, float X, float Y, float W, float H, const FLinearColor& Accent, float Alpha = 1.f);
	/** Scoreboard row / band: Team 0 red, 1 blue, anything else neutral. */
	OPENUT2K4_API bool DrawScoreBox(const FArenaDraw& D, float X, float Y, float W, float H, int32 Team, float Alpha = 1.f);

	// ---- weapons ----
	/** HUD icon of a weapon class (walks up the class hierarchy), nullptr if none. */
	OPENUT2K4_API const FHudSprite* GetWeaponIcon(const UClass* WeaponClass);
	/** HUD icon of the ammo a weapon class uses, nullptr if none. */
	OPENUT2K4_API const FHudSprite* GetAmmoIcon(const UClass* WeaponClass);
	/** InventoryGroup of the stock weapon a weapon class stands in for (1..9 on the weapon bar, 0 if unknown). */
	OPENUT2K4_API int32 GetWeaponGroup(const UClass* WeaponClass);
	/** Weapon icon by the weapon's display name (kill feed), nullptr if unknown. */
	OPENUT2K4_API const FHudSprite* FindWeaponIconByName(const FString& WeaponName);

	// ---- crosshairs ----
	/** Number of crosshair choices: 0 is the built-in procedural crosshair, 1.. are textures. */
	OPENUT2K4_API int32 NumCrosshairs();
	OPENUT2K4_API FString GetCrosshairName(int32 Index);
	/** Texture path of crosshair Index (>= 1), empty for 0. */
	OPENUT2K4_API FString GetCrosshairTexture(int32 Index);
	/**
	 * Draws crosshair Index (>= 1) centred on (CX, CY). False for 0 or when its texture is missing.
	 * bPixelSnap rounds the size to whole multiples of the 64 px texture (crisp thin lines; at least 64 px).
	 */
	OPENUT2K4_API bool DrawCrosshair(const FArenaDraw& D, int32 Index, float CX, float CY, float Size, const FLinearColor& Color, bool bPixelSnap = true);

	// ---- menus ----
	enum class EButtonState : uint8
	{
		Normal,
		Hover,
		Focused,
		Pressed,
		Disabled
	};
	/** Full-screen menu backdrop (scene render, character render picked by Variant, scanlines). */
	OPENUT2K4_API bool DrawMenuBackground(const FArenaDraw& D, float Time, int32 Variant);
	/** Framed menu window. */
	OPENUT2K4_API bool DrawMenuPanel(const FArenaDraw& D, float X, float Y, float W, float H, float Alpha = 1.f);
	/** Header bar of a menu window. */
	OPENUT2K4_API bool DrawMenuHeader(const FArenaDraw& D, float X, float Y, float W, float H, float Alpha = 1.f);
	OPENUT2K4_API bool DrawMenuButton(const FArenaDraw& D, float X, float Y, float W, float H, EButtonState State, float Alpha = 1.f);
	/** Left / right value arrow centred on (CX, CY). */
	OPENUT2K4_API bool DrawMenuArrow(const FArenaDraw& D, bool bRight, float CX, float CY, float Size, EButtonState State);
}
