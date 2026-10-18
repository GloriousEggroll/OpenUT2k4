#pragma once

#include "CoreMinimal.h"

class AController;

/**
 * UT2004 weapon hand and small weapons (Engine.Weapon RenderOverlays / GetEffectStart, ProjectileFire.DoFireEffect):
 * the first person view model's offset, pivot and mirroring for Right, Left, Center and Hidden, from the stock
 * weapons' class defaults (PlayerViewOffset, SmallViewOffset, CenteredOffsetY, CenteredRoll, CenteredYaw,
 * PlayerViewPivot, DisplayFOV) read at runtime from the player's packages.
 */
namespace ArenaWeaponHand
{
	/** UE2 Handedness of a controller: the local player's Settings > Player choice (-1 left, 0 center, 1 right,
	 *  2 hidden), 0 for bots (Bot.ChangedWeapon: SetHand(0)), 1 for everybody else. */
	OPENUT2K4_API float GetHandedness(const AController* Controller);
	/** PlayerController.bSmallWeapons of the local player. */
	OPENUT2K4_API bool UseSmallWeapons(const AController* Controller);

	/** The stock first person values of one of our weapon classes (bValid false when unknown). */
	struct FViewParams
	{
		bool bValid = false;
		FVector Offset = FVector::ZeroVector;      // PlayerViewOffset (UU)
		FVector SmallOffset = FVector::ZeroVector; // SmallViewOffset (UU; PlayerViewOffset when unset)
		float CenteredOffsetY = 0.f;
		int32 CenteredRoll = 0;
		int32 CenteredYaw = 0;
		float DisplayFOV = 60.f;
	};
	OPENUT2K4_API const FViewParams& GetViewParams(const UClass* WeaponClass);

	/** UE2 class of one of our weapons ("XWeapons.FlakCannon"), empty when unknown. */
	OPENUT2K4_API FString UE2WeaponClass(const UClass* WeaponClass);
}
