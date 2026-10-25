#pragma once

#include "CoreMinimal.h"

struct FUSObject;

/**
 * What script asks an actor to show that has no value of its own to keep it in (UE2 keeps it in the mesh instance):
 * the animation it plays (Actor.PlayAnim / LoopAnim / StopAnimating). The natives record it here; whatever draws the
 * actor (the UE2 client mirror's script actors, Net/UE2NetScriptVisuals.h) plays it. Bone attachments, draw type, static
 * mesh and relative placement are the actor's own properties (AttachToBone sets Base and AttachmentBone, as UE2 does).
 */
namespace USPresentation
{
	struct FAnim
	{
		FName Sequence;
		float Rate = 1.f;
		bool bLoop = false;
		bool bStopped = false;
		/** Counts up with every command (the same sequence played again restarts it). */
		uint32 Serial = 0;
	};
	OPENUT2K4_API const FAnim* FindAnim(const FUSObject* Actor);
	OPENUT2K4_API void SetAnim(const FUSObject* Actor, FName Sequence, float Rate, bool bLoop, bool bStopped);
	OPENUT2K4_API void Forget(const FUSObject* Actor);
}
