#include "ArenaMusic.h"
#include "OpenUT2k4.h"
#include "ArenaContent.h"
#include "ArenaRuntimeContent.h"
#include "ArenaGameState.h"
#include "ArenaLevelBuilder.h"
#include "ArenaMenuGameMode.h"
#include "ArenaUserSettings.h"
#include "Components/AudioComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StreamableManager.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundWave.h"
#include "UObject/UObjectGlobals.h"

namespace ArenaMusic
{
	constexpr float FadeInTime = 2.5f;
	constexpr float MapChangeFadeTime = 1.5f;
	constexpr float MatchEndFadeTime = 6.f;
	const TCHAR* const ImportedRoot = TEXT("/Game/Imported/");
	const TCHAR* const ReplacementRoot = TEXT("/Game/Replacements/");
	// Preferred menu tracks (UT2004 names), first match wins
	const TCHAR* const MenuTracks[] = { TEXT("KR-UT2004-Menu"), TEXT("KR-MenuMusic-v2"), TEXT("Menu1"), TEXT("KR-UT2003-Menu") };

	/** Tracks that are not level music: never picked at random for procedural layouts. */
	bool IsSpecialTrack(const FString& Name)
	{
		return Name.Contains(TEXT("Menu")) || Name.Contains(TEXT("Intro")) || Name.Contains(TEXT("Ending")) ||
			Name.Contains(TEXT("The_End")) || Name.Contains(TEXT("Entrance"));
	}

	/** Value of a top-level "key": "string" pair, found by text search (the map json is large and the music
	 *  fields are plain strings at the top level; no need to parse the whole document). */
	bool FindJsonString(const FString& Text, const TCHAR* Key, FString& Out)
	{
		const FString Needle = FString::Printf(TEXT("\"%s\""), Key);
		int32 From = Text.Len();
		// the fields are appended near the end of the file: search backwards
		while (From > 0)
		{
			const int32 Idx = Text.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromEnd, From);
			if (Idx == INDEX_NONE)
			{
				return false;
			}
			From = Idx;
			int32 i = Idx + Needle.Len();
			while (i < Text.Len() && FChar::IsWhitespace(Text[i])) { ++i; }
			if (i >= Text.Len() || Text[i] != TEXT(':'))
			{
				continue; // the key text appeared as a value
			}
			++i;
			while (i < Text.Len() && FChar::IsWhitespace(Text[i])) { ++i; }
			if (i < Text.Len() && Text[i] == TEXT('"'))
			{
				const int32 End = Text.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, i + 1);
				if (End != INDEX_NONE)
				{
					Out = Text.Mid(i + 1, End - i - 1);
					return true;
				}
			}
			return false; // null or not a string
		}
		return false;
	}
}

void UArenaMusicSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	bInitialized = true;
	PreLoadMapHandle = FCoreUObjectDelegates::PreLoadMap.AddUObject(this, &UArenaMusicSubsystem::HandlePreLoadMap);
}

void UArenaMusicSubsystem::Deinitialize()
{
	FCoreUObjectDelegates::PreLoadMap.Remove(PreLoadMapHandle);
	if (LoadHandle.IsValid())
	{
		LoadHandle->CancelHandle();
		LoadHandle.Reset();
	}
	if (Current)
	{
		Current->Stop();
		Current = nullptr;
	}
	for (UAudioComponent* C : FadingOut)
	{
		if (C)
		{
			C->Stop();
		}
	}
	FadingOut.Reset();
	bInitialized = false;
	Super::Deinitialize();
}

TStatId UArenaMusicSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UArenaMusicSubsystem, STATGROUP_Tickables);
}

UWorld* UArenaMusicSubsystem::GetTickableGameObjectWorld() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetWorld() : nullptr;
}

float UArenaMusicSubsystem::GetMusicVolume()
{
	return FMath::Clamp(UArenaUserSettings::Get()->MusicVolume, 0.f, 1.f);
}

const TMap<FString, FString>& UArenaMusicSubsystem::GetTrackList()
{
	if (bTrackListLoaded)
	{
		return Tracks;
	}
	bTrackListLoaded = true;
	const FString Path = AArenaLevelBuilder::GetImportedDataDir() / TEXT("Assets") / TEXT("Music.json");
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		if (ArenaContent::UseRuntimeContent())
		{
			// runtime content without the Music.json table: every song in the content cache
			for (const FString& Name : ArenaRuntimeContent::GetMusicNames())
			{
				FString Asset = Name;
				for (TCHAR& C : Asset)
				{
					if (!FChar::IsAlnum(C) && C != '_' && C != '-') { C = '_'; }
				}
				Tracks.Add(Name, ArenaMusic::ImportedRoot + FString(TEXT("Music/")) + Asset);
			}
			UE_LOG(LogOpenUT2k4, Log, TEXT("Music: %d tracks in the content cache"), Tracks.Num());
			return Tracks;
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("Music: no imported music (%s not found)"), *Path);
		return Tracks;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	const TSharedPtr<FJsonObject>* Assets = nullptr;
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid() || !Root->TryGetObjectField(TEXT("assets"), Assets))
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("Music: cannot read %s"), *Path);
		return Tracks;
	}
	for (const auto& Pair : (*Assets)->Values)
	{
		const TSharedPtr<FJsonObject>* Entry = nullptr;
		FString Imported;
		if (Pair.Value.IsValid() && Pair.Value->TryGetObject(Entry) &&
			((*Entry)->TryGetStringField(TEXT("imported"), Imported) || (*Entry)->TryGetStringField(TEXT("asset"), Imported)) &&
			!ResolveTrackPath(Imported).IsEmpty())
		{
			Tracks.Add(FString(Pair.Key), Imported);
		}
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("Music: %d imported tracks available"), Tracks.Num());
	return Tracks;
}

FString UArenaMusicSubsystem::ResolveTrackPath(const FString& ImportedPath)
{
	if (ImportedPath.IsEmpty())
	{
		return FString();
	}
	FString PackagePath = ImportedPath;
	int32 Dot = INDEX_NONE;
	if (PackagePath.FindLastChar(TEXT('.'), Dot))
	{
		PackagePath.LeftInline(Dot);
	}
	if (PackagePath.StartsWith(ArenaMusic::ImportedRoot))
	{
		const FString Replacement = ArenaMusic::ReplacementRoot + PackagePath.RightChop(FCString::Strlen(ArenaMusic::ImportedRoot));
		if (FPackageName::DoesPackageExist(Replacement))
		{
			return Replacement;
		}
	}
	if (ArenaContent::UseRuntimeContent())
	{
		// runtime content: the "/Game/Imported/Music/<Name>" path names a song of the content cache (see RequestTrack)
		return PackagePath.StartsWith(ArenaMusic::ImportedRoot) &&
			!ArenaRuntimeContent::FindCacheFile(PackagePath.RightChop(FCString::Strlen(ArenaMusic::ImportedRoot)), TEXT(".ogg")).IsEmpty()
			? PackagePath : FString();
	}
	return FPackageName::DoesPackageExist(PackagePath) ? PackagePath : FString();
}

FString UArenaMusicSubsystem::FindMenuTrack()
{
	const TMap<FString, FString>& List = GetTrackList();
	for (const TCHAR* Name : ArenaMusic::MenuTracks)
	{
		for (const TPair<FString, FString>& Pair : List)
		{
			if (Pair.Key.Equals(Name, ESearchCase::IgnoreCase))
			{
				return ResolveTrackPath(Pair.Value);
			}
		}
	}
	return FString();
}

FString UArenaMusicSubsystem::PickRandomTrack()
{
	TArray<FString> Candidates;
	for (const TPair<FString, FString>& Pair : GetTrackList())
	{
		if (!ArenaMusic::IsSpecialTrack(Pair.Key))
		{
			Candidates.Add(Pair.Value);
		}
	}
	if (Candidates.Num() == 0)
	{
		return FString();
	}
	// avoid repeating the track that is playing now
	FString Pick;
	for (int32 Try = 0; Try < 4; ++Try)
	{
		Pick = ResolveTrackPath(Candidates[FMath::RandRange(0, Candidates.Num() - 1)]);
		if (Pick != PlayingPath)
		{
			break;
		}
	}
	return Pick;
}

FString UArenaMusicSubsystem::ReadImportedMapTrack(const FString& MapName)
{
	const FString Path = AArenaLevelBuilder::GetImportedDataDir() / (MapName + TEXT(".json"));
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return FString();
	}
	FString Asset, Song;
	if (ArenaMusic::FindJsonString(Text, TEXT("music_asset"), Asset))
	{
		const FString Resolved = ResolveTrackPath(Asset);
		if (!Resolved.IsEmpty())
		{
			return Resolved;
		}
	}
	if (ArenaMusic::FindJsonString(Text, TEXT("music"), Song) && !Song.IsEmpty())
	{
		// the json predates the music_asset field or the asset moved: look the song up in the manifest
		for (const TPair<FString, FString>& Pair : GetTrackList())
		{
			if (Pair.Key.Equals(Song, ESearchCase::IgnoreCase))
			{
				return ResolveTrackPath(Pair.Value);
			}
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("Music: %s wants '%s', which is not imported"), *MapName, *Song);
	}
	return FString();
}

FString UArenaMusicSubsystem::ChooseTrackFor(UWorld* World)
{
	if (GetTrackList().Num() == 0)
	{
		return FString();
	}
	const FString Package = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());

	const AGameModeBase* GM = World->GetAuthGameMode();
	if ((GM && GM->IsA<AArenaMenuGameMode>()) || Package == TEXT("/Game/Maps/MainMenu"))
	{
		const FString Menu = FindMenuTrack();
		UE_LOG(LogOpenUT2k4, Log, TEXT("Music: main menu -> %s"), Menu.IsEmpty() ? TEXT("(no menu track)") : *Menu);
		return Menu;
	}
	if (!World->GetGameState<AArenaGameState>())
	{
		return FString(); // not a match (e.g. an editor preview world)
	}
	// The cooked map level, or the map built at runtime into the base level
	const FString MapName = AArenaLevelBuilder::GetCurrentMapName(World);
	if (!MapName.IsEmpty())
	{
		FString Track = ReadImportedMapTrack(MapName);
		const bool bOwn = !Track.IsEmpty();
		if (!bOwn)
		{
			Track = PickRandomTrack();
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("Music: map %s -> %s%s"), *MapName, *Track, bOwn ? TEXT("") : TEXT(" (no map track, random)"));
		return Track;
	}
	const FString Track = PickRandomTrack();
	UE_LOG(LogOpenUT2k4, Log, TEXT("Music: no map -> %s (random)"), *Track);
	return Track;
}

void UArenaMusicSubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetTickableGameObjectWorld();
	if (!World || !World->IsGameWorld() || World->IsNetMode(NM_DedicatedServer))
	{
		return;
	}
	if (!GEngine || !GEngine->UseSound() || !World->GetAudioDeviceRaw())
	{
		if (!bNoAudioLogged)
		{
			bNoAudioLogged = true;
			UE_LOG(LogOpenUT2k4, Log, TEXT("Music: no audio device, music disabled"));
		}
		return;
	}

	// New world: decide its track once play has begun (the game mode / game state exist then)
	if (DecidedWorld.Get() != World && World->HasBegunPlay())
	{
		DecidedWorld = World;
		bEndFaded = false;
		const FString Wanted = ChooseTrackFor(World);
		WantedPath = Wanted;
		if (Wanted.IsEmpty())
		{
			FadeOutCurrent(ArenaMusic::MapChangeFadeTime);
		}
		else if (Wanted != PlayingPath || !Current || !Current->IsPlaying())
		{
			RequestTrack(Wanted);
		}
	}

	// End of the match: fade the music out
	if (!bEndFaded && Current)
	{
		const AArenaGameState* GS = World->GetGameState<AArenaGameState>();
		if (GS && GS->Phase == EArenaMatchPhase::Ended)
		{
			bEndFaded = true;
			UE_LOG(LogOpenUT2k4, Log, TEXT("Music: match ended, fading out"));
			FadeOutCurrent(ArenaMusic::MatchEndFadeTime);
		}
	}

	// Music volume slider
	const float Vol = GetMusicVolume();
	if (Current && !FMath::IsNearlyEqual(Vol, AppliedVolume, 0.001f))
	{
		AppliedVolume = Vol;
		Current->SetVolumeMultiplier(Vol);
	}

	// Drop finished fade-outs
	FadingOut.RemoveAll([](const TObjectPtr<UAudioComponent>& C)
	{
		if (C && C->IsPlaying())
		{
			return false;
		}
		if (C)
		{
			C->DestroyComponent();
		}
		return true;
	});
}

void UArenaMusicSubsystem::RequestTrack(const FString& Path)
{
	if (LoadHandle.IsValid())
	{
		LoadHandle->CancelHandle();
		LoadHandle.Reset();
	}
	if (ArenaContent::UseRuntimeContent() && Path.StartsWith(ArenaMusic::ImportedRoot))
	{
		// built from the content cache (the ogg is streamed while playing, so this is quick)
		if (USoundBase* Built = ArenaRuntimeContent::LoadMusic(FPackageName::GetShortName(Path)))
		{
			StartPlaying(Built, Path);
		}
		else
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("Music: cannot build %s from the content cache"), *Path);
		}
		return;
	}
	const FSoftObjectPath ObjectPath(Path + TEXT(".") + FPackageName::GetShortName(Path));
	if (USoundBase* Loaded = Cast<USoundBase>(ObjectPath.ResolveObject()))
	{
		StartPlaying(Loaded, Path);
		return;
	}
	LoadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(ObjectPath,
		FStreamableDelegate::CreateUObject(this, &UArenaMusicSubsystem::OnTrackLoaded, Path));
	if (!LoadHandle.IsValid())
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("Music: cannot load %s"), *Path);
	}
}

void UArenaMusicSubsystem::OnTrackLoaded(FString Path)
{
	USoundBase* Sound = LoadHandle.IsValid() ? Cast<USoundBase>(LoadHandle->GetLoadedAsset()) : nullptr;
	LoadHandle.Reset();
	if (Path != WantedPath)
	{
		return; // a newer request replaced this one
	}
	if (!Sound)
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("Music: %s is not a sound asset"), *Path);
		return;
	}
	StartPlaying(Sound, Path);
}

void UArenaMusicSubsystem::StartPlaying(USoundBase* Sound, const FString& Path)
{
	UWorld* World = GetTickableGameObjectWorld();
	if (!World)
	{
		return;
	}
	FadeOutCurrent(ArenaMusic::MapChangeFadeTime);

	if (USoundWave* Wave = Cast<USoundWave>(Sound))
	{
		Wave->bLooping = true; // music always loops (the importer sets it too)
	}
	// Persistent 2D component: survives the level transition so the old track can fade under the loading screen
	UAudioComponent* Comp = UGameplayStatics::CreateSound2D(World, Sound, 1.f, 1.f, 0.f, nullptr, true, false);
	if (!Comp)
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("Music: audio playback unavailable"));
		return;
	}
	AppliedVolume = GetMusicVolume();
	Comp->SetVolumeMultiplier(AppliedVolume);
	Comp->bIsMusic = true;
	Comp->bIsUISound = true; // keeps playing while the game is paused
	Comp->FadeIn(ArenaMusic::FadeInTime, 1.f);
	Current = Comp;
	PlayingPath = Path;
	UE_LOG(LogOpenUT2k4, Log, TEXT("Music: playing %s (%.1fs, volume %.2f)"), *Path, Sound->GetDuration(), AppliedVolume);
}

void UArenaMusicSubsystem::FadeOutCurrent(float Seconds)
{
	if (Current)
	{
		if (Current->IsPlaying())
		{
			Current->FadeOut(Seconds, 0.f);
			FadingOut.Add(Current);
		}
		else
		{
			Current->DestroyComponent();
		}
		Current = nullptr;
	}
	PlayingPath.Reset();
}

void UArenaMusicSubsystem::HandlePreLoadMap(const FString& MapName)
{
	// Leaving the map: start the fade now; the new map's track fades in once it has begun play
	if (Current)
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("Music: loading %s, fading out"), *MapName);
	}
	FadeOutCurrent(ArenaMusic::MapChangeFadeTime);
	DecidedWorld.Reset();
}
