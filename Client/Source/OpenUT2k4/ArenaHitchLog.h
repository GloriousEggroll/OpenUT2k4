#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"

/**
 * Frame hitch reporter (every build, the shipping log included): a frame much longer than the recent ones is logged
 * with the game thread, render thread, RHI thread and GPU times of that frame ("Hitch: ..."), and every 30 s of play
 * a summary line gives the average and worst frame times and the number of hitches. Map loads are left out.
 *
 * Notes give a hitch a cause: code that does something expensive and rare (spawning a character, building a mesh)
 * calls Note(), and the next hitch line names the notes made during its frame.
 */
namespace ArenaHitchLog
{
	void OnModuleStartup();
	void OnModuleShutdown();

	/** Records what this frame did (shown with the frame's hitch line, if it is one). */
	void Note(const FString& What);

	/** Notes "<What> (<n> ms)" when the scope took at least a millisecond. */
	struct FScopedNote
	{
		const TCHAR* What;
		double Start;
		explicit FScopedNote(const TCHAR* InWhat) : What(InWhat), Start(FPlatformTime::Seconds()) {}
		~FScopedNote()
		{
			const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;
			if (Ms >= 1.0)
			{
				Note(FString::Printf(TEXT("%s (%.0f ms)"), What, Ms));
			}
		}
	};
}
