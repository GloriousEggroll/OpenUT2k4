// Runtime sounds and music: cache wav (PCM 8/16 bit) and ogg vorbis -> UArenaRuntimeSoundWave.

#include "ArenaRuntimeAudio.h"
#include "ArenaRuntimeContentPrivate.h"
#include "ArenaContent.h"
#include "ArenaRuntimeContent.h"
#include "OpenUT2k4.h"
#include "AudioDecompress.h"
#include "Interfaces/IAudioFormat.h"
#include "Decoders/VorbisAudioInfo.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Sound/SoundGenerator.h"

namespace ArenaRuntimeAudio
{
	static TAutoConsoleVariable<int32> CVarAudioDebug(
		TEXT("arena.RuntimeAudioDebug"), 0,
		TEXT("1 = log the peak level of the first second of every runtime sound / music play (checks that decoding works)."),
		ECVF_Default);

	/** Peak meter for arena.RuntimeAudioDebug (audio render thread). */
	struct FPeakProbe
	{
		FString Name;
		int64 Samples = 0;
		int64 Limit = 0;
		float Peak = 0.f;
		bool bDone = false;
		void Init(const FString& InName, int32 SampleRate, int32 Channels)
		{
			Name = InName;
			Limit = int64(SampleRate) * Channels;
			bDone = CVarAudioDebug.GetValueOnAnyThread() == 0;
		}
		void Add(const float* Out, int32 Num)
		{
			if (bDone)
			{
				return;
			}
			for (int32 i = 0; i < Num; ++i)
			{
				Peak = FMath::Max(Peak, FMath::Abs(Out[i]));
			}
			Samples += Num;
			if (Samples >= Limit)
			{
				bDone = true;
				UE_LOG(LogOpenUT2k4, Log, TEXT("RuntimeAudio: %s first second peak %.3f"), *Name, Peak);
			}
		}
	};

	using FDataPtr = TSharedPtr<const FArenaRuntimeAudioData, ESPMode::ThreadSafe>;

	/** Plays shared PCM; loops over [LoopStart, LoopEnd) when looping. */
	class FPcmGenerator : public ISoundGenerator
	{
	public:
		FPcmGenerator(FDataPtr InData, bool bInLooping, const FString& Name, float InRate = 1.f, float InGain = 1.f)
			: Data(MoveTemp(InData)), bLooping(bInLooping), Rate(FMath::Clamp(InRate, 0.25f, 4.f)), Gain(FMath::Max(0.f, InGain))
		{
			Probe.Init(Name, Data->SampleRate, Data->NumChannels);
		}

		virtual int32 OnGenerateAudio(float* Out, int32 NumSamples) override
		{
			if (!FMath::IsNearlyEqual(Rate, 1.f) || !FMath::IsNearlyEqual(Gain, 1.f))
			{
				return GenerateResampled(Out, NumSamples);
			}
			const int32 Ch = Data->NumChannels;
			const int32 Frames = Data->Pcm.Num() / Ch;
			const int32 LoopEnd = Data->LoopEnd > Data->LoopStart ? FMath::Min(Data->LoopEnd, Frames) : Frames;
			int32 Written = 0;
			while (Written < NumSamples)
			{
				const int32 End = bLooping ? LoopEnd : Frames;
				if (Frame >= End)
				{
					if (!bLooping || End <= Data->LoopStart)
					{
						bFinished = true;
						break;
					}
					Frame = Data->LoopStart;
				}
				const int32 Count = FMath::Min((NumSamples - Written) / Ch, End - Frame);
				if (Count <= 0)
				{
					break;
				}
				const int16* Src = Data->Pcm.GetData() + int64(Frame) * Ch;
				for (int32 i = 0; i < Count * Ch; ++i)
				{
					Out[Written + i] = Src[i] / 32768.f;
				}
				Written += Count * Ch;
				Frame += Count;
			}
			// zero the rest of the buffer (end of a one-shot)
			for (int32 i = Written; i < NumSamples; ++i)
			{
				Out[i] = 0.f;
			}
			Probe.Add(Out, NumSamples);
			return NumSamples;
		}

		virtual bool IsFinished() const override { return bFinished; }

	private:
		/** Procedural sounds: linear-interpolated playback at Rate (pitch), scaled by Gain (volume). */
		int32 GenerateResampled(float* Out, int32 NumSamples)
		{
			const int32 Ch = Data->NumChannels;
			const int32 Frames = Data->Pcm.Num() / Ch;
			const int32 LoopEnd = Data->LoopEnd > Data->LoopStart ? FMath::Min(Data->LoopEnd, Frames) : Frames;
			const int32 End = bLooping ? LoopEnd : Frames;
			const float Scale = Gain / 32768.f;
			int32 Written = 0;
			while (Written + Ch <= NumSamples)
			{
				if (Pos >= End - 1)
				{
					if (!bLooping || End - Data->LoopStart < 2)
					{
						bFinished = true;
						break;
					}
					Pos = Data->LoopStart + FMath::Fmod(Pos - Data->LoopStart, double(End - Data->LoopStart - 1));
				}
				const int32 I = int32(Pos);
				const float T = float(Pos - I);
				const int16* A = Data->Pcm.GetData() + int64(I) * Ch;
				const int16* B = A + Ch;
				for (int32 c = 0; c < Ch; ++c)
				{
					Out[Written + c] = FMath::Clamp((A[c] + (B[c] - A[c]) * T) * Scale, -1.f, 1.f);
				}
				Written += Ch;
				Pos += Rate;
			}
			for (int32 i = Written; i < NumSamples; ++i)
			{
				Out[i] = 0.f;
			}
			Probe.Add(Out, NumSamples);
			return NumSamples;
		}

		FDataPtr Data;
		bool bLooping;
		float Rate = 1.f;
		float Gain = 1.f;
		int32 Frame = 0;
		double Pos = 0.0;
		bool bFinished = false;
		FPeakProbe Probe;
	};

	/** Decodes the shared ogg file progressively (each playing instance has its own decoder). */
	class FVorbisGenerator : public ISoundGenerator
	{
	public:
		FVorbisGenerator(FDataPtr InData, bool bInLooping, const FString& Name) : Data(MoveTemp(InData)), bLooping(bInLooping)
		{
			Probe.Init(Name, Data->SampleRate, Data->NumChannels);
			FSoundQualityInfo Info;
			bReady = Decoder.ReadCompressedInfo(Data->Ogg.GetData(), Data->Ogg.Num(), &Info);
			bFinished = !bReady;
		}

		virtual int32 OnGenerateAudio(float* Out, int32 NumSamples) override
		{
			if (!bReady || bFinished)
			{
				FMemory::Memzero(Out, NumSamples * sizeof(float));
				bFinished = true;
				return NumSamples;
			}
			Scratch.SetNumUninitialized(NumSamples);
			const bool bEnded = Decoder.ReadCompressedData(reinterpret_cast<uint8*>(Scratch.GetData()), bLooping, NumSamples * sizeof(int16));
			for (int32 i = 0; i < NumSamples; ++i)
			{
				Out[i] = Scratch[i] / 32768.f;
			}
			if (bEnded && !bLooping)
			{
				bFinished = true;
			}
			Probe.Add(Out, NumSamples);
			return NumSamples;
		}

		virtual bool IsFinished() const override { return bFinished; }

	private:
		FDataPtr Data;
		bool bLooping;
		FVorbisAudioInfo Decoder;
		TArray<int16> Scratch;
		bool bReady = false;
		bool bFinished = false;
		FPeakProbe Probe;
	};

	uint32 ReadU32(const uint8* P) { return uint32(P[0]) | (uint32(P[1]) << 8) | (uint32(P[2]) << 16) | (uint32(P[3]) << 24); }
	uint16 ReadU16(const uint8* P) { return uint16(P[0] | (P[1] << 8)); }

	/** RIFF/WAVE with PCM 8/16/24/32 bit or float32 -> int16 interleaved. */
	bool ParseWav(const TArray<uint8>& Bytes, FArenaRuntimeAudioData& Out)
	{
		if (Bytes.Num() < 12 || FMemory::Memcmp(Bytes.GetData(), "RIFF", 4) != 0 || FMemory::Memcmp(Bytes.GetData() + 8, "WAVE", 4) != 0)
		{
			return false;
		}
		int32 Format = 0, Channels = 0, Rate = 0, Bits = 0;
		const uint8* PcmData = nullptr;
		int64 PcmBytes = 0;
		int64 Pos = 12;
		while (Pos + 8 <= Bytes.Num())
		{
			const uint8* Chunk = Bytes.GetData() + Pos;
			const int64 Size = ReadU32(Chunk + 4);
			const int64 Avail = FMath::Min<int64>(Size, Bytes.Num() - Pos - 8);
			if (FMemory::Memcmp(Chunk, "fmt ", 4) == 0 && Avail >= 16)
			{
				Format = ReadU16(Chunk + 8);
				Channels = ReadU16(Chunk + 10);
				Rate = int32(ReadU32(Chunk + 12));
				Bits = ReadU16(Chunk + 22);
				if (Format == 0xFFFE && Avail >= 26)
				{
					Format = ReadU16(Chunk + 32); // WAVE_FORMAT_EXTENSIBLE: sub format
				}
			}
			else if (FMemory::Memcmp(Chunk, "data", 4) == 0)
			{
				PcmData = Chunk + 8;
				PcmBytes = Avail;
			}
			Pos += 8 + Size + (Size & 1);
		}
		if (!PcmData || Channels <= 0 || Rate <= 0 || !(Format == 1 || Format == 3))
		{
			return false;
		}
		const int32 BytesPerSample = Bits / 8;
		if (BytesPerSample <= 0 || (Format == 3 && Bits != 32))
		{
			return false;
		}
		const int64 NumSamples = PcmBytes / BytesPerSample / Channels * Channels;
		Out.Pcm.SetNumUninitialized(NumSamples);
		for (int64 i = 0; i < NumSamples; ++i)
		{
			const uint8* S = PcmData + i * BytesPerSample;
			int32 V = 0;
			if (Format == 3)
			{
				float F;
				FMemory::Memcpy(&F, S, 4);
				V = int32(FMath::Clamp(F, -1.f, 1.f) * 32767.f);
			}
			else switch (Bits)
			{
			case 8: V = (int32(S[0]) - 128) << 8; break;
			case 16: V = int16(ReadU16(S)); break;
			case 24: V = int32((uint32(S[0]) << 8) | (uint32(S[1]) << 16) | (uint32(S[2]) << 24)) >> 16; break;
			case 32: V = int32(ReadU32(S)) >> 16; break;
			default: return false;
			}
			Out.Pcm[i] = int16(FMath::Clamp(V, -32768, 32767));
		}
		Out.NumChannels = Channels;
		Out.SampleRate = Rate;
		return true;
	}

	UArenaRuntimeSoundWave* MakeWave(const FString& Name, FDataPtr Data, float Duration, bool bLooping)
	{
		UObject* Outer = ArenaRuntimePrivate::AssetOuter();
		UArenaRuntimeSoundWave* Wave = NewObject<UArenaRuntimeSoundWave>(Outer,
			MakeUniqueObjectName(Outer, UArenaRuntimeSoundWave::StaticClass(), FName(*ArenaRuntimePrivate::AssetName(Name))), RF_Transient);
		Wave->SetData(MoveTemp(Data), Duration, bLooping);
		++ArenaRuntimePrivate::Stats().Sounds;
		return Wave;
	}
}

UArenaRuntimeSoundWave::UArenaRuntimeSoundWave(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bProcedural = true;
	// a procedural one-shot that is virtualized and realized again would start over: just stop it
	VirtualizationMode = EVirtualizationMode::Disabled;
}

void UArenaRuntimeSoundWave::SetData(TSharedPtr<const FArenaRuntimeAudioData, ESPMode::ThreadSafe> InData, float InDuration, bool bInLooping)
{
	Data = MoveTemp(InData);
	NumChannels = Data->NumChannels;
	SetSampleRate(Data->SampleRate);
	Duration = InDuration;
	TotalSamples = InDuration * Data->SampleRate;
	bLooping = bInLooping;
	if (bInLooping)
	{
		VirtualizationMode = EVirtualizationMode::Restart;
	}
}

ISoundGeneratorPtr UArenaRuntimeSoundWave::CreateSoundGenerator(const FSoundGeneratorInitParams& InParams)
{
	using namespace ArenaRuntimeAudio;
	if (!Data.IsValid())
	{
		return nullptr;
	}
	++ArenaRuntimePrivate::Stats().SoundPlays;
	UE_LOG(LogOpenUT2k4, Verbose, TEXT("RuntimeContent: playing %s"), *GetName());
	if (Data->Ogg.Num() > 0)
	{
		return MakeShared<FVorbisGenerator, ESPMode::ThreadSafe>(Data, bLooping != 0, GetName());
	}
	if (bProceduralSound)
	{
		// UE2 UProceduralSound: modification +- a random share of the variance, in percent, per play.
		const float PitchPct = PitchModification + FMath::FRandRange(-1.f, 1.f) * FMath::Abs(PitchVariance);
		const float VolumePct = VolumeModification + FMath::FRandRange(-1.f, 1.f) * FMath::Abs(VolumeVariance);
		return MakeShared<FPcmGenerator, ESPMode::ThreadSafe>(Data, bLooping != 0, GetName(), (100.f + PitchPct) / 100.f, (100.f + VolumePct) / 100.f);
	}
	return MakeShared<FPcmGenerator, ESPMode::ThreadSafe>(Data, bLooping != 0, GetName());
}

void UArenaRuntimeSoundWave::SetProcedural(float InPitchModification, float InPitchVariance, float InVolumeModification, float InVolumeVariance)
{
	PitchModification = InPitchModification;
	PitchVariance = InPitchVariance;
	VolumeModification = InVolumeModification;
	VolumeVariance = InVolumeVariance;
	bProceduralSound = Data.IsValid() && Data->Ogg.Num() == 0;
	// the lowest pitch plays the longest
	const float MinRate = FMath::Max(0.25f, (100.f + PitchModification - FMath::Abs(PitchVariance)) / 100.f);
	Duration = Duration / MinRate;
	TotalSamples = Duration * SampleRate;
}

USoundBase* ArenaRuntimePrivate::BuildSound(const FString& ObjectPath)
{
	using namespace ArenaRuntimeAudio;
	FScopeUs Timer(Stats().SoundUs);
	const FString Base = FindByObjectPath(TEXT("sounds"), ObjectPath);
	const TSharedPtr<FJsonObject> Meta = Base.IsEmpty() ? nullptr : LoadJson(Base + TEXT(".json"));
	FString File;
	if (!Meta.IsValid())
	{
		return nullptr;
	}
	if (!Meta->TryGetStringField(TEXT("file"), File) || File.IsEmpty())
	{
		// ProceduralSound: the base sound with UT2004's pitch / volume modification and variance, applied per play.
		FString BaseSound;
		if (!Meta->TryGetStringField(TEXT("base_sound"), BaseSound) || BaseSound.IsEmpty() || BaseSound.Equals(ObjectPath, ESearchCase::IgnoreCase))
		{
			return nullptr;
		}
		const UArenaRuntimeSoundWave* BaseWave = Cast<UArenaRuntimeSoundWave>(ArenaRuntimeContent::LoadSoundByObjectPath(BaseSound));
		if (!BaseWave || !BaseWave->GetData().IsValid())
		{
			return nullptr;
		}
		double PitchMod = 0.0, PitchVar = 0.0, VolMod = 0.0, VolVar = 0.0;
		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (Meta->TryGetObjectField(TEXT("properties"), Props) && Props->IsValid())
		{
			(*Props)->TryGetNumberField(TEXT("PitchModification"), PitchMod);
			(*Props)->TryGetNumberField(TEXT("PitchVariance"), PitchVar);
			(*Props)->TryGetNumberField(TEXT("VolumeModification"), VolMod);
			(*Props)->TryGetNumberField(TEXT("VolumeVariance"), VolVar);
		}
		UArenaRuntimeSoundWave* Wave = MakeWave(ObjectPath, BaseWave->GetData(), BaseWave->Duration, BaseWave->bLooping != 0);
		Wave->SetProcedural(float(PitchMod), float(PitchVar), float(VolMod), float(VolVar));
		UE_LOG(LogOpenUT2k4, Log, TEXT("RuntimeContent: procedural sound %s = %s pitch %+.0f%% +-%.0f%%, volume %+.0f%% +-%.0f%%"),
			*ObjectPath, *BaseSound, PitchMod, FMath::Abs(PitchVar), VolMod, FMath::Abs(VolVar));
		return Wave;
	}
	const FString Path = FPaths::GetPath(Base) / File;
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		return nullptr;
	}
	TSharedPtr<FArenaRuntimeAudioData, ESPMode::ThreadSafe> Data = MakeShared<FArenaRuntimeAudioData, ESPMode::ThreadSafe>();
	float Duration = 0.f;
	if (File.EndsWith(TEXT(".ogg"), ESearchCase::IgnoreCase))
	{
		FVorbisAudioInfo Probe;
		FSoundQualityInfo Info;
		if (!Probe.ReadCompressedInfo(Bytes.GetData(), Bytes.Num(), &Info))
		{
			return nullptr;
		}
		Data->NumChannels = Info.NumChannels;
		Data->SampleRate = Info.SampleRate;
		Duration = Info.Duration;
		Data->Ogg = MoveTemp(Bytes);
	}
	else if (!ParseWav(Bytes, *Data))
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("RuntimeContent: unsupported wav %s"), *Path);
		return nullptr;
	}
	else
	{
		Duration = float(Data->Pcm.Num() / Data->NumChannels) / Data->SampleRate;
	}
	bool bLoop = false;
	const TSharedPtr<FJsonObject>* Loop = nullptr;
	if (Meta->TryGetObjectField(TEXT("loop"), Loop) && Loop->IsValid())
	{
		bLoop = true;
		int32 Start = 0, End = 0;
		(*Loop)->TryGetNumberField(TEXT("start_sample"), Start);
		(*Loop)->TryGetNumberField(TEXT("end_sample"), End);
		Data->LoopStart = FMath::Max(0, Start);
		Data->LoopEnd = End > Start ? End + 1 : 0;
	}
	return MakeWave(ObjectPath, Data, Duration, bLoop);
}

USoundBase* ArenaRuntimePrivate::BuildMusic(const FString& Name)
{
	using namespace ArenaRuntimeAudio;
	FScopeUs Timer(Stats().SoundUs);
	// asset names ("Music/<AssetName>") and UT2004 song names both resolve through the cache index
	const FString Base = FindByAssetName(TEXT("music"), FString(), AssetName(Name));
	const FString Path = Base.IsEmpty() ? ArenaContent::GetCacheDir() / TEXT("music") / (Name + TEXT(".ogg")) : Base + TEXT(".ogg");
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		return nullptr;
	}
	TSharedPtr<FArenaRuntimeAudioData, ESPMode::ThreadSafe> Data = MakeShared<FArenaRuntimeAudioData, ESPMode::ThreadSafe>();
	FVorbisAudioInfo Probe;
	FSoundQualityInfo Info;
	if (!Probe.ReadCompressedInfo(Bytes.GetData(), Bytes.Num(), &Info))
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("RuntimeContent: cannot read music %s"), *Path);
		return nullptr;
	}
	Data->NumChannels = Info.NumChannels;
	Data->SampleRate = Info.SampleRate;
	Data->Ogg = MoveTemp(Bytes);
	return MakeWave(TEXT("Music_") + Name, Data, Info.Duration, true);
}
