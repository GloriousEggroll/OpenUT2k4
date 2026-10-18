#pragma once

#include "CoreMinimal.h"
#include "UScriptCore.h"

/**
 * Canvas bridge of the script runtime: UE2's Canvas natives (Engine.Canvas DrawText / DrawTile / StrLen ...), the
 * HUD widget natives (XInterface.HudBase DrawSpriteWidget / DrawNumericWidget), HUD.DrawCanvasLine and
 * Material.MaterialUSize / VSize, implemented in UScriptNativesCanvas.cpp on top of a drawing backend the game
 * provides while a script HUD draws (Net/UE2NetScriptHUD: our UCanvas with UT2004's fonts and textures).
 *
 * Canvas state (Font, CurX / CurY, OrgX / OrgY, ClipX / ClipY, DrawColor, Style, ColorModulate ...) lives in the
 * script Canvas object like in UE2; the natives do UE2's layout (wrapping, clipping, the cursor) and hand the backend
 * screen rectangles in pixels. Without a backend every native still consumes its parameters and returns zero sizes.
 */
class IUSCanvasBackend
{
public:
	virtual ~IUSCanvasBackend() = default;

	/**
	 * Draws a region of a material (U, V, UL, VL in texels of its UE2 size; UL / VL may be negative: mirrored) at a
	 * screen rectangle. Style is ERenderStyle (STY_Normal 1, Masked 2, Translucent 3, Modulated 4, Alpha 5, Additive 6).
	 * Color is the final colour (DrawColor with ColorModulate applied), sRGB bytes.
	 */
	virtual void DrawTile(FUSObject* Material, float X, float Y, float XL, float YL, float U, float V, float UL, float VL, const FColor& Color, uint8 Style) = 0;
	/** Draws a line of text (UE2 colour codes ESC R G B allowed) with its top left at X, Y, scaled by ScaleX / ScaleY. */
	virtual void DrawText(FUSObject* Font, const FString& Text, float X, float Y, float ScaleX, float ScaleY, const FColor& Color, uint8 Style) = 0;
	/** Size of a line of text in pixels (colour codes take no room). */
	virtual void TextSize(FUSObject* Font, const FString& Text, float ScaleX, float ScaleY, float& OutXL, float& OutYL) = 0;
	/** UE2 size (USize, VSize) of a material; false when unknown. */
	virtual bool MaterialSize(FUSObject* Material, int32& OutU, int32& OutV) = 0;
	/** UE2 world location (UE2 units) to canvas pixels; Z > 0 when in front of the camera. */
	virtual FUSVector WorldToScreen(const FUSVector& World) = 0;
	/** The camera (UE2 units and rotation). */
	virtual void GetCamera(FUSVector& OutLocation, FUSRotator& OutRotation) = 0;
	/** A 1 pixel line. */
	virtual void DrawLine(float X1, float Y1, float X2, float Y2, const FColor& Color) = 0;
};

namespace USCanvas
{
	/** The backend for the natives (null: draw nothing). Set it only while script draws, game thread. */
	void SetBackend(IUSCanvasBackend* Backend);
	IUSCanvasBackend* GetBackend();
	/** Draw calls and text calls since the last reset (statistics). */
	int32 GetDrawCount();
	void ResetDrawCount();
}
