// Frame hitch reporter (ArenaHitchLog.h).

#include "ArenaHitchLog.h"
#include "OpenUT2k4.h"
#include "DynamicRHI.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
#include "RenderTimer.h"
#include "UObject/UObjectGlobals.h"

namespace ArenaHitchLogLocal
{
	static TAutoConsoleVariable<float> CVarHitchMs(
		TEXT("arena.HitchLogMs"), 30.f,
		TEXT("Frames at least this long (ms), and at least twice the recent average, are logged as hitches (0 = off)."),
		ECVF_Default);

	/** At most this many hitch lines per run (the summaries go on). */
	constexpr int32 MaxHitchLines = 300;
	constexpr double SummarySeconds = 30.0;

	double LastFrame = 0.0;
	double AvgMs = 0.f;
	bool bLoading = false;
	int32 SkipFrames = 0;
	int32 HitchLines = 0;
	/** Notes with the frame they were made in: a hitch names those of its frame and the two before (the render
	 *  thread and the GPU run a frame or two behind the game thread). */
	struct FNote
	{
		uint64 Frame;
		FString What;
	};
	TArray<FNote> Notes;

	// the current summary window
	double WindowStart = 0.0;
	int32 WindowFrames = 0;
	double WindowSumMs = 0.0;
	double WindowMaxMs = 0.0;
	int32 WindowHitches = 0;
	TArray<float> WindowTimes;

	FDelegateHandle EndFrameHandle, PreLoadHandle, PostLoadHandle;

	float CyclesMs(uint32 Cycles)
	{
		return float(FPlatformTime::ToMilliseconds(Cycles));
	}

	void FlushSummary(double Now)
	{
		if (WindowFrames > 0)
		{
			WindowTimes.Sort();
			const float P99 = WindowTimes[FMath::Min(WindowTimes.Num() - 1, int32(WindowTimes.Num() * 0.99f))];
			UE_LOG(LogOpenUT2k4, Log, TEXT("Frames (%.0f s): %d frames, average %.1f ms (%.0f fps), 99%% under %.1f ms, worst %.1f ms, %d hitches"),
				Now - WindowStart, WindowFrames, WindowSumMs / WindowFrames, 1000.0 * WindowFrames / FMath::Max(1.0, WindowSumMs), P99, WindowMaxMs, WindowHitches);
		}
		WindowStart = Now;
		WindowFrames = 0;
		WindowSumMs = 0.0;
		WindowMaxMs = 0.0;
		WindowHitches = 0;
		WindowTimes.Reset();
	}

	void OnEndFrame()
	{
		const double Now = FPlatformTime::Seconds();
		const double Prev = LastFrame;
		LastFrame = Now;
		Notes.RemoveAll([](const FNote& N) { return N.Frame + 2 < GFrameCounter; });
		if (Prev <= 0.0 || bLoading)
		{
			return;
		}
		if (SkipFrames > 0)
		{
			// the first frames after a map load still settle (streaming, shader caches)
			--SkipFrames;
			WindowStart = Now;
			return;
		}
		const float Ms = float((Now - Prev) * 1000.0);
		const float Limit = CVarHitchMs.GetValueOnGameThread();
		const bool bHitch = Limit > 0.f && AvgMs > 0.0 && Ms >= Limit && Ms >= 2.0 * AvgMs;
		if (bHitch)
		{
			++WindowHitches;
			if (HitchLines < MaxHitchLines)
			{
				++HitchLines;
				FString What;
				for (const FNote& N : Notes)
				{
					What += What.IsEmpty() ? TEXT("; recent: ") : TEXT(", ");
					What += N.What;
				}
				UE_LOG(LogOpenUT2k4, Log, TEXT("Hitch: frame %.1f ms (average %.1f): game thread %.1f ms, render thread %.1f ms, RHI thread %.1f ms, GPU %.1f ms%s"),
					Ms, AvgMs, CyclesMs(GGameThreadTime), CyclesMs(GRenderThreadTime), CyclesMs(GRHIThreadTime), CyclesMs(RHIGetGPUFrameCycles()), *What);
			}
		}
		else
		{
			// hitches stay out of the average, so a burst of them is still measured against normal frames
			AvgMs = AvgMs <= 0.0 ? Ms : AvgMs + (Ms - AvgMs) * 0.05;
		}
		++WindowFrames;
		WindowSumMs += Ms;
		WindowMaxMs = FMath::Max(WindowMaxMs, double(Ms));
		WindowTimes.Add(Ms);
		if (Now - WindowStart >= SummarySeconds)
		{
			FlushSummary(Now);
		}
	}
}

void ArenaHitchLog::OnModuleStartup()
{
	using namespace ArenaHitchLogLocal;
	if (IsRunningDedicatedServer() || IsRunningCommandlet())
	{
		return;
	}
	EndFrameHandle = FCoreDelegates::OnEndFrame.AddStatic(&OnEndFrame);
	PreLoadHandle = FCoreUObjectDelegates::PreLoadMap.AddLambda([](const FString&)
	{
		bLoading = true;
	});
	PostLoadHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddLambda([](UWorld*)
	{
		bLoading = false;
		SkipFrames = 30;
		AvgMs = 0.0;
		LastFrame = 0.0;
	});
}

void ArenaHitchLog::OnModuleShutdown()
{
	using namespace ArenaHitchLogLocal;
	FCoreDelegates::OnEndFrame.Remove(EndFrameHandle);
	FCoreUObjectDelegates::PreLoadMap.Remove(PreLoadHandle);
	FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadHandle);
}

void ArenaHitchLog::Note(const FString& What)
{
	using namespace ArenaHitchLogLocal;
	if (Notes.Num() < 12 && IsInGameThread())
	{
		Notes.Add({ GFrameCounter, What });
	}
}
