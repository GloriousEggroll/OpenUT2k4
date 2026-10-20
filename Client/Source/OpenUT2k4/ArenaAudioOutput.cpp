// Output stage on the main submix: stereo fold of the 5.1 mix for stereo devices, and a peak limiter.

#include "ArenaAudioOutput.h"
#include "OpenUT2k4.h"
#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "AudioMixerDevice.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Server/ArenaDedicatedServer.h"
#include "UObject/StrongObjectPtr.h"

#if PLATFORM_LINUX
#include "SDL3/SDL.h"
#endif

namespace ArenaAudioOutputLocal
{
	static TAutoConsoleVariable<int32> CVarFold(
		TEXT("arena.AudioStereoFold"), -1,
		TEXT("Fold the surround mix into the front pair: -1 = when the output device is stereo (default), 0 = never, 1 = always."),
		ECVF_Default);

	static TAutoConsoleVariable<int32> CVarLimiter(
		TEXT("arena.AudioLimiter"), 1,
		TEXT("1 = peak limiter on the main output (keeps overlapping sounds from clipping), 0 = off."),
		ECVF_Default);

	// UE2's effects level: ALAudioSubsystem SoundVolume (Default.ini 0.3) scales every sound, so a sound at volume 1
	// plays at 0.3 and louder requests (announcer 2.0, vehicle guns 1.25 ...) still fit under OpenAL's gain cap of 1;
	// ambient loops use their own levels on that scale (AArenaAmbientSound, AUE2NetVehicle)
	static TAutoConsoleVariable<float> CVarMixLevel(
		TEXT("arena.AudioMixLevel"), 0.3f,
		TEXT("Gain on the whole mix before the limiter: UT2004's default effects volume (SoundVolume 0.3)."),
		ECVF_Default);

	int32 DeviceChannels = 0;
	FDelegateHandle CreatedHandle;
	TStrongObjectPtr<UArenaOutputStagePreset> Preset;

	bool ShouldFold(int32 MixChannels)
	{
		const int32 Mode = CVarFold.GetValueOnAnyThread();
		if (MixChannels <= 2 || Mode == 0)
		{
			return false;
		}
		return Mode > 0 || (DeviceChannels > 0 && DeviceChannels <= 2);
	}

	void Install(Audio::FDeviceId DeviceId)
	{
		FAudioDeviceManager* Manager = FAudioDeviceManager::Get();
		FAudioDeviceHandle Device = Manager ? Manager->GetAudioDevice(DeviceId) : FAudioDeviceHandle();
		if (!Device.IsValid())
		{
			return;
		}
		if (!Preset.IsValid())
		{
			Preset.Reset(NewObject<UArenaOutputStagePreset>(GetTransientPackage(), TEXT("ArenaOutputStage")));
			Preset->Init();
		}
		Audio::FMixerDevice* Mixer = static_cast<Audio::FMixerDevice*>(Device.GetAudioDevice());
		FSoundEffectSubmixInitData InitData;
		InitData.SampleRate = Mixer->GetSampleRate();
		InitData.DeviceID = DeviceId;
		InitData.PresetSettings = nullptr;
		InitData.ParentPresetUniqueId = Preset->GetUniqueID();
		TSoundEffectSubmixPtr Effect = USoundEffectPreset::CreateInstance<FSoundEffectSubmixInitData, FSoundEffectSubmix>(InitData, *Preset.Get());
		Effect->SetEnabled(true);
		Mixer->AddMainSubmixEffect(Effect);
		UE_LOG(LogOpenUT2k4, Log, TEXT("Audio: output stage on device %u (%d mix channels, output device %d channels, stereo fold %s)"),
			DeviceId, Mixer->GetNumDeviceChannels(), DeviceChannels, ShouldFold(Mixer->GetNumDeviceChannels()) ? TEXT("on") : TEXT("off"));
	}
}

int32 ArenaAudioOutput::GetDeviceChannels()
{
	return ArenaAudioOutputLocal::DeviceChannels;
}

void ArenaAudioOutput::OnModuleStartup()
{
	using namespace ArenaAudioOutputLocal;
	if (ArenaDedicatedServer::IsServerMode() || !FApp::CanEverRenderAudio())
	{
		return;
	}
#if PLATFORM_LINUX
	// AudioMixerSDL opens the default device with 6 channels, and SDL then opens the device itself with at least that
	// many, so ask before the engine does: the default device's own format (the sound server's sink, e.g. 2 channels).
	const bool bWasInit = SDL_WasInit(SDL_INIT_AUDIO) != 0;
	if (bWasInit || SDL_InitSubSystem(SDL_INIT_AUDIO))
	{
		SDL_AudioSpec Spec = {};
		int Frames = 0;
		if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &Spec, &Frames))
		{
			DeviceChannels = Spec.channels;
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("Audio: default output device (%hs): %d channels, %d Hz"),
			SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "none", DeviceChannels, Spec.freq);
		if (!bWasInit)
		{
			SDL_QuitSubSystem(SDL_INIT_AUDIO);
		}
	}
#endif
	CreatedHandle = FAudioDeviceManagerDelegates::OnAudioDeviceCreated.AddStatic(&Install);
}

void ArenaAudioOutput::OnModuleShutdown()
{
	using namespace ArenaAudioOutputLocal;
	if (CreatedHandle.IsValid())
	{
		FAudioDeviceManagerDelegates::OnAudioDeviceCreated.Remove(CreatedHandle);
		CreatedHandle.Reset();
	}
	Preset.Reset();
}

void FArenaOutputStage::Init(const FSoundEffectSubmixInitData& InData)
{
	SampleRate = InData.SampleRate > 0.f ? InData.SampleRate : 48000.f;
	SetupLimiter(2);
}

void FArenaOutputStage::OnPresetChanged()
{
	GET_EFFECT_SETTINGS(ArenaOutputStage);
	Ceiling = FMath::Pow(10.f, FMath::Clamp(Settings.ThresholdDb, -12.f, 0.f) / 20.f);
}

void FArenaOutputStage::SetupLimiter(int32 NumChannels)
{
	LimiterChannels = NumChannels;
	Window = FMath::Max(1, FMath::RoundToInt(SampleRate * 0.005f));
	// a reduction is held 50 ms before it is given back: longer than half a cycle of the lowest ambience drones (20 Hz),
	// so the gain does not follow single waveform peaks (which distorts the bass)
	Hold = Window + FMath::RoundToInt(SampleRate * 0.05f);
	ReleaseCoef = 1.f - FMath::Exp(-1.f / (SampleRate * 0.15f));
	Frame = 0;
	Delay.SetNumZeroed(Window * NumChannels);
	MinValue.SetNumZeroed(Hold + 1);
	MinFrame.SetNumZeroed(Hold + 1);
	MinHead = MinCount = 0;
	Held.Init(1.f, Window);
	HeldSum = Window;
	Released = 1.f;
}

void FArenaOutputStage::Limit(float* Buffer, int32 Frames)
{
	const int32 Ch = LimiterChannels;
	for (int32 F = 0; F < Frames; ++F, ++Frame)
	{
		float* X = Buffer + F * Ch;
		float Peak = 0.f;
		for (int32 c = 0; c < Ch; ++c)
		{
			Peak = FMath::Max(Peak, FMath::Abs(X[c]));
		}
		const float Need = Peak > Ceiling ? Ceiling / Peak : 1.f;
		// sliding minimum of Need over the last Hold + 1 frames (monotonic deque in a ring): every average below then
		// includes the frame leaving the delay line
		const int32 Cap = Hold + 1;
		while (MinCount > 0 && MinValue[(MinHead + MinCount - 1) % Cap] >= Need)
		{
			--MinCount;
		}
		const int32 Tail = (MinHead + MinCount) % Cap;
		MinValue[Tail] = Need;
		MinFrame[Tail] = Frame;
		++MinCount;
		if (MinFrame[MinHead] < Frame - Hold)
		{
			MinHead = (MinHead + 1) % Cap;
			--MinCount;
		}
		// gain reduction is taken at once, given back slowly
		const float Min = MinValue[MinHead];
		Released = Min < Released ? Min : Released + (Min - Released) * ReleaseCoef;
		// moving average over the window, applied to the frame that entered the delay line Window frames ago
		const int32 Slot = int32(Frame % Window);
		HeldSum += Released - Held[Slot];
		Held[Slot] = Released;
		const float Gain = FMath::Min(1.f, float(HeldSum / Window));
		float* D = Delay.GetData() + Slot * Ch;
		for (int32 c = 0; c < Ch; ++c)
		{
			const float In = X[c];
			X[c] = D[c] * Gain;
			D[c] = In;
		}
	}
}

void FArenaOutputStage::OnProcessAudio(const FSoundEffectSubmixInputData& InData, FSoundEffectSubmixOutputData& OutData)
{
	const int32 Ch = InData.NumChannels;
	const int32 Frames = InData.NumFrames;
	const float* In = InData.AudioBuffer->GetData();
	float* Out = OutData.AudioBuffer->GetData();
	const int32 Num = Frames * Ch;
	if (Ch <= 0 || OutData.NumChannels != Ch || OutData.AudioBuffer->Num() < Num)
	{
		return;
	}
	// stereo fold, EAudioMixerChannel order: FL FR FC LFE BL BR [FLC FRC BC SL SR ...]; the LFE send is left out as
	// in common stereo downmixes (UE only sends to it on request)
	if (ArenaAudioOutputLocal::ShouldFold(Ch))
	{
		constexpr float Half = 0.70710678f;
		for (int32 F = 0; F < Frames; ++F)
		{
			const float* S = In + F * Ch;
			float* D = Out + F * Ch;
			float L = S[0] + (Ch > 2 ? S[2] * Half : 0.f);
			float R = S[1] + (Ch > 2 ? S[2] * Half : 0.f);
			if (Ch > 5)
			{
				L += S[4] * Half;
				R += S[5] * Half;
			}
			if (Ch > 10)
			{
				L += S[9] * Half;
				R += S[10] * Half;
			}
			FMemory::Memzero(D, Ch * sizeof(float));
			D[0] = L;
			D[1] = R;
		}
	}
	else if (Out != In)
	{
		FMemory::Memcpy(Out, In, Num * sizeof(float));
	}
	const float Gain = ArenaAudioOutputLocal::CVarMixLevel.GetValueOnAnyThread();
	if (Gain != 1.f)
	{
		for (int32 i = 0; i < Num; ++i)
		{
			Out[i] *= Gain;
		}
	}
	if (ArenaAudioOutputLocal::CVarLimiter.GetValueOnAnyThread() == 0)
	{
		return;
	}
	if (LimiterChannels != Ch)
	{
		SetupLimiter(Ch);
	}
	Limit(Out, Frames);
}
