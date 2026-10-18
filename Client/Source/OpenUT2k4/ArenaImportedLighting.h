#pragma once

#include "CoreMinimal.h"

class UWorld;
class FJsonObject;

/**
 * Runtime lighting corrections for imported UT2004 levels (applied by AArenaLevelBuilder::BuildImported, so no
 * re-import is needed).
 *
 * UT2004 lit its levels with baked lightmaps on BSP (lighting applied in gamma space, effectively 2x overbright),
 * unshadowed vertex lighting on static meshes and a per-zone ambient floor (ZoneInfo.AmbientBrightness). None of
 * that exists with Lumen + movable lights, so areas between the converted point lights fall to black. This:
 *  - adds an ambient floor through Lumen skylight leaking, tinted and scaled by the zones' ambient
 *    brightness / hue / saturation (raw_markers.zones),
 *  - switches the level's post process from fixed exposure to clamped histogram auto exposure, so dark interiors
 *    open up a little while bright outdoor maps are held back,
 *  - scales the converted local light intensities.
 * Tunables can be overridden on the command line in non-shipping builds (-ImpLeak=, -ImpLeakAmb=, -ImpLightScale=,
 * -ImpExpMin=, -ImpExpMax=, -ImpBias=, -ImpAuto=0|1, -ImpSkyScale=, -ImpOff) for measurement runs.
 */
namespace ArenaImportedLighting
{
	void Apply(UWorld* World, const TSharedPtr<FJsonObject>& Root);
	/**
	 * Re-tune the level set up by the last Apply for the current tracing mode (ArenaRayTracing): runtime levels
	 * with software Lumen get a weaker ambient fill the more enclosed they are (no distance fields, so nothing
	 * occludes it). Called by Apply and by ArenaRayTracing::Apply when the setting changes. -ImpSwFill= overrides
	 * the scale (non-shipping).
	 */
	void Refresh();
}
