#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundWave.h"
#include "ArenaRuntimeAudio.generated.h"

/** Sample data shared by every playing instance of a runtime sound. */
struct FArenaRuntimeAudioData
{
	/** Decoded interleaved PCM (wav sounds), or empty for streamed ogg. */
	TArray<int16> Pcm;
	/** Whole ogg file (music): every playing instance decodes it on the audio render thread. */
	TArray<uint8> Ogg;
	int32 NumChannels = 1;
	int32 SampleRate = 22050;
	/** Loop region in frames (wav loop points from the cache json; whole sound otherwise). */
	int32 LoopStart = 0;
	int32 LoopEnd = 0;
};

/**
 * Sound wave built at runtime from the content cache (ArenaRuntimeContent::LoadSound / LoadMusic).
 *
 * Cooked builds cannot compress audio, so instead of compressed platform data the wave is procedural and
 * hands every playing instance its own sound generator reading the shared samples (any number of
 * overlapping plays, looping by the wave's loop region). Game code treats it as any USoundWave.
 */
UCLASS(Transient)
class UArenaRuntimeSoundWave : public USoundWave
{
	GENERATED_BODY()

public:
	UArenaRuntimeSoundWave(const FObjectInitializer& ObjectInitializer);

	void SetData(TSharedPtr<const FArenaRuntimeAudioData, ESPMode::ThreadSafe> InData, float InDuration, bool bInLooping);
	TSharedPtr<const FArenaRuntimeAudioData, ESPMode::ThreadSafe> GetData() const { return Data; }

	/**
	 * UT2004 ProceduralSound settings (percent, as in the cache json "properties"): every play renders the base sound
	 * at pitch (100 + PitchModification +- PitchVariance) % and volume (100 + VolumeModification +- VolumeVariance) %,
	 * the variances picked at random per play (UE2 UProceduralSound). Applied by the play's sound generator.
	 */
	void SetProcedural(float InPitchModification, float InPitchVariance, float InVolumeModification, float InVolumeVariance);

	virtual ISoundGeneratorPtr CreateSoundGenerator(const FSoundGeneratorInitParams& InParams) override;
	virtual bool IsPlayable() const override { return Data.IsValid(); }

private:
	TSharedPtr<const FArenaRuntimeAudioData, ESPMode::ThreadSafe> Data;
	float PitchModification = 0.f;
	float PitchVariance = 0.f;
	float VolumeModification = 0.f;
	float VolumeVariance = 0.f;
	bool bProceduralSound = false;
};
