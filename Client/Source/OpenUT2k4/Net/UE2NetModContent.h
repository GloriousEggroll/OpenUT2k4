#pragma once

#include "CoreMinimal.h"

class USoundBase;

/**
 * Resources used only by a mod's classes (hit sounds in a mod's .u, HUD textures...): the content converter converts
 * maps and what they use, so these are converted on demand, one object at a time, into the content cache the first
 * time the game needs them (Docs/UE2Net.md "Mod client-side code"). The package comes from the script runtime (a
 * downloaded or installed .u) or the net cache; a package with the name of a stock one is never converted over it.
 */
namespace UE2NetModContent
{
	/** The file the script runtime has for a package name (a downloaded or installed mod first), or empty. */
	OPENUT2K4_API FString PackageFile(const FString& Package);
	/** "Package.Group.Name" of a Sound: the content cache's sound, converted from its package first when missing. */
	OPENUT2K4_API USoundBase* LoadSound(const FString& ObjectPath);
	/** Converts the object (Sound / Texture / Material / Font / StaticMesh / SkeletalMesh ...; ClassName its UE2 class, empty for
	 *  a material of any kind) into the content cache when it is not there yet. */
	OPENUT2K4_API bool EnsureConverted(const FString& ObjectPath, const FString& ClassName);
}
