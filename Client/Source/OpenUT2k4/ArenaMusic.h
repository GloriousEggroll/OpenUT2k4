#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "UObject/SoftObjectPath.h"
#include "ArenaMusic.generated.h"

class UAudioComponent;
class USoundBase;
struct FStreamableHandle;

/**
 * Background music. Plays one looping track per map and cross-fades on map changes:
 *  - imported maps: the map's own track ("music_asset" / "music" in Content/ImportedData/<Map>.json,
 *    taken from the UT2004 LevelInfo.Song by Tools/ut2k4_map_music.py)
 *  - procedural layouts: a random imported track
 *  - main menu: a menu track (KR-UT2004-Menu, ...) when one is imported
 * Tracks are loaded by soft path; /Game/Replacements/<path> wins over /Game/Imported/<path>. When no imported
 * music exists (Content/ImportedData/Assets/Music.json missing or its assets not present) nothing plays.
 * Fades out when the match ends. Volume: the master volume (UArenaUserSettings::Volume, applied to the audio
 * device) times UArenaUserSettings::MusicVolume. Self-contained: no game mode / HUD hooks are needed.
 */
UCLASS()
class OPENUT2K4_API UArenaMusicSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return !IsTemplate() && bInitialized; }
	virtual bool IsTickableWhenPaused() const override { return true; }
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
	virtual UWorld* GetTickableGameObjectWorld() const override;

	/** Imported music asset paths (/Game/Imported/Music/<Name>) keyed by UT2004 name, from Music.json. */
	const TMap<FString, FString>& GetTrackList();

	/** Resolve an imported asset path to the path to load (replacement first). Empty if neither exists. */
	static FString ResolveTrackPath(const FString& ImportedPath);

	/** The music volume the component plays at (MusicVolume setting, 0..1). */
	static float GetMusicVolume();

private:
	/** Decide which track the current world wants. Empty = silence. */
	FString ChooseTrackFor(UWorld* World);
	FString FindMenuTrack();
	FString PickRandomTrack();
	FString ReadImportedMapTrack(const FString& MapName);

	void RequestTrack(const FString& Path);
	void OnTrackLoaded(FString Path);
	void StartPlaying(USoundBase* Sound, const FString& Path);
	void FadeOutCurrent(float Seconds);

	void HandlePreLoadMap(const FString& MapName);

	bool bInitialized = false;
	bool bTrackListLoaded = false;
	bool bNoAudioLogged = false;
	TMap<FString, FString> Tracks;

	TWeakObjectPtr<UWorld> DecidedWorld;
	FString WantedPath;     // track the current world wants (resolved path), empty for silence
	FString PlayingPath;    // track of Current
	bool bEndFaded = false; // faded out for the end of the match
	float AppliedVolume = -1.f;

	UPROPERTY() TObjectPtr<UAudioComponent> Current;
	/** Previous tracks still fading out (destroyed when done). */
	UPROPERTY() TArray<TObjectPtr<UAudioComponent>> FadingOut;
	TSharedPtr<FStreamableHandle> LoadHandle;
	FDelegateHandle PreLoadMapHandle;
};
