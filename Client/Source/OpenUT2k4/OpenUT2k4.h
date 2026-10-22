#pragma once

#include "CoreMinimal.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOpenUT2k4, Log, All);

// Custom collision channels (see DefaultEngine.ini)
#define ECC_Weapon     ECC_GameTraceChannel1
#define ECC_Projectile ECC_GameTraceChannel2

// Component tag of world geometry that stops shots but not players (UE2 zero extent traces hit it, pawns use other
// collision): terrain's true surface, meshes with bBlockNonZeroExtentTraces=False, BlockingVolumes with
// bBlockZeroExtentTraces=True. AArenaWeapon::WeaponTrace stops at it although it doesn't block pawns.
#define ARENA_SHOT_BLOCKER_TAG TEXT("ArenaShotBlocker")
