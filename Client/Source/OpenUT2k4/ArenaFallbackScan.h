#pragma once

#include "CoreMinimal.h"

class UWorld;

/**
 * Development check (non-shipping), -LogProceduralShapes: finds the procedural stand-in shapes (ArenaAssets' cube,
 * sphere, cylinder and cone meshes) that are visible in the world, i.e. places where art is missing and a
 * fallback is drawn. The world is scanned twice a second; every distinct (owner class, component) that shows a shape
 * is logged once when first seen ("FallbackScan: new ...") and all of them with counts at exit (or with the console
 * command arena.FallbackScanReport).
 */
namespace ArenaFallbackScan
{
	void Tick(UWorld* World);
	void Report();
}
