#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundEffectSubmix.h"
#include "ArenaAudioOutput.generated.h"

/**
 * Output stage on the main submix (installed from code, ArenaAudioOutput::OnModuleStartup):
 *
 * - Stereo fold: the Linux mixer (AudioMixerSDL) always renders 5.1 and leaves the downmix to the system. When the
 *   output device is stereo, the stage folds centre and rear channels into the front pair itself, so sounds behind the
 *   listener keep their side whatever the sound server does with surround streams, and the limiter sees the
 *   final stereo signal.
 * - Mix level: UT2004's default effects volume (ALAudio SoundVolume 0.3) on the whole mix, the headroom the game's
 *   levels were made for (sounds are normalized to full scale; announcer and vehicle guns play above volume 1).
 * - Limiter: a 5 ms lookahead peak limiter at -1 dBFS (50 ms hold) keeps what still overlaps from clipping (the mixer clamps at 0 dBFS, which distorts). UT2004 on OpenAL Soft (OldUnreal) has the same
 *   protection: OpenAL Soft's output limiter is on by default.
 */
USTRUCT()
struct FArenaOutputStageSettings
{
	GENERATED_BODY()

	/** Limiter threshold, dBFS. */
	UPROPERTY()
	float ThresholdDb = -1.f;
};

class FArenaOutputStage : public FSoundEffectSubmix
{
public:
	virtual void Init(const FSoundEffectSubmixInitData& InData) override;
	virtual void OnPresetChanged() override;
	virtual void OnProcessAudio(const FSoundEffectSubmixInputData& InData, FSoundEffectSubmixOutputData& OutData) override;

private:
	void SetupLimiter(int32 NumChannels);
	void Limit(float* Buffer, int32 Frames);

	/**
	 * Brick-wall lookahead limiter: the gain each frame needs (ceiling / peak) goes through a sliding minimum over the
	 * lookahead window plus a hold time, a slow release, and a moving average over the lookahead window, applied to the
	 * signal delayed by the window. The average of minimums that all include a peak never exceeds that peak's gain, so no frame passes the
	 * ceiling, and the gain still ramps down smoothly over the window before the peak (no clicks).
	 */
	float SampleRate = 48000.f;
	float Ceiling = 0.891f;
	float ReleaseCoef = 0.f;
	int32 Window = 0;
	int32 Hold = 0;				// frames a gain reduction is held (sliding minimum length, >= Window)
	int32 LimiterChannels = 0;
	int64 Frame = 0;
	TArray<float> Delay;		// Window frames of input, interleaved
	TArray<float> MinValue;		// monotonic deque of (gain, frame) over the last Hold + 1 frames
	TArray<int64> MinFrame;
	int32 MinHead = 0;
	int32 MinCount = 0;
	TArray<float> Held;			// the last Window released minimums, for the moving average
	double HeldSum = 0.0;
	float Released = 1.f;
};

UCLASS(MinimalAPI)
class UArenaOutputStagePreset : public USoundEffectSubmixPreset
{
	GENERATED_BODY()

public:
	EFFECT_PRESET_METHODS(ArenaOutputStage)

	UPROPERTY()
	FArenaOutputStageSettings Settings;
};

namespace ArenaAudioOutput
{
	/** Probes the output device's channel count (before the engine opens it) and installs the stage on every new audio device. */
	void OnModuleStartup();
	void OnModuleShutdown();

	/** Channels of the system's default output device (0 = unknown, no fold). */
	int32 GetDeviceChannels();
}
