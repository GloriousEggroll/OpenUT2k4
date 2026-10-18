#include "ArenaContent.h"
#include "OpenUT2k4.h"
#include "ArenaUserSettings.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace ArenaContentLocal
{
	/** Child folder of Dir whose name matches Name case-insensitively (UT2004 installs mix cases); empty if none. */
	FString FindChildDir(const FString& Dir, const FString& Name)
	{
		FString Found;
		IFileManager::Get().IterateDirectory(*Dir, [&](const TCHAR* Path, bool bIsDir)
		{
			if (bIsDir && FPaths::GetCleanFilename(Path).Equals(Name, ESearchCase::IgnoreCase))
			{
				Found = Path;
				return false;
			}
			return true;
		});
		return Found;
	}

	FString UserDataRoot()
	{
#if PLATFORM_WINDOWS
		const FString Local = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		return Local.IsEmpty() ? FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("OpenUT2k4")) : FPaths::Combine(Local, TEXT("OpenUT2k4"));
#elif PLATFORM_MAC
		return FPaths::Combine(FPlatformProcess::UserHomeDir(), TEXT("Library/Application Support/OpenUT2k4"));
#else
		const FString Xdg = FPlatformMisc::GetEnvironmentVariable(TEXT("XDG_DATA_HOME"));
		return Xdg.IsEmpty() ? FPaths::Combine(FPlatformProcess::UserHomeDir(), TEXT(".local/share/OpenUT2k4")) : FPaths::Combine(Xdg, TEXT("OpenUT2k4"));
#endif
	}
}

namespace ArenaContent
{
	bool UseRuntimeContent()
	{
		static const bool bRuntime = []()
		{
			if (FParse::Param(FCommandLine::Get(), TEXT("RuntimeContent")))
			{
				return true;
			}
			if (FParse::Param(FCommandLine::Get(), TEXT("NoRuntimeContent")))
			{
				return false;
			}
			return !IFileManager::Get().DirectoryExists(*(FPaths::ProjectContentDir() / TEXT("ImportedData")));
		}();
		return bRuntime;
	}

	const FString& GetCacheDir()
	{
		static const FString Dir = []()
		{
			FString Override;
			if (FParse::Value(FCommandLine::Get(), TEXT("ContentCache="), Override) && !Override.IsEmpty())
			{
				return FPaths::ConvertRelativePathToFull(Override);
			}
			return FPaths::Combine(ArenaContentLocal::UserDataRoot(), TEXT("ContentCache"));
		}();
		return Dir;
	}

	FString GetUserDataDir()
	{
		return ArenaContentLocal::UserDataRoot();
	}

	const FString& GetUserContentDir()
	{
		static const FString Dir = []()
		{
			FString Override;
			if (FParse::Value(FCommandLine::Get(), TEXT("UserContent="), Override) && !Override.IsEmpty())
			{
				return FPaths::ConvertRelativePathToFull(Override);
			}
			return FPaths::Combine(ArenaContentLocal::UserDataRoot(), TEXT("UserContent"));
		}();
		return Dir;
	}

	FString GetCacheDataDir()
	{
		return GetCacheDir() / TEXT("data");
	}

	bool IsCacheReady()
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *(GetCacheDir() / TEXT("cache.json"))))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			return false;
		}
		int32 Version = 0;
		bool bComplete = false;
		Root->TryGetNumberField(TEXT("format_version"), Version);
		Root->TryGetBoolField(TEXT("complete"), bComplete);
		return Version == CacheFormatVersion && bComplete;
	}

	bool IsUT2004Install(const FString& Dir)
	{
		return !Dir.IsEmpty() && IFileManager::Get().DirectoryExists(*Dir)
			&& !ArenaContentLocal::FindChildDir(Dir, TEXT("System")).IsEmpty()
			&& !ArenaContentLocal::FindChildDir(Dir, TEXT("Maps")).IsEmpty();
	}

	TArray<FString> GetSearchLocations()
	{
		const FString Home = FPlatformProcess::UserHomeDir();
		TArray<FString> Out;
		// drop-in folder next to the game (packaged: <root>/UT2004, beside the executable's project folder)
		Out.Add(FPaths::ConvertRelativePathToFull(FPaths::RootDir() / TEXT("UT2004")));
		Out.Add(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("UT2004")));
		Out.Add(ArenaContentLocal::UserDataRoot() / TEXT("UT2004"));
#if PLATFORM_WINDOWS
		Out.Add(TEXT("C:/UT2004"));
		Out.Add(TEXT("C:/Program Files/Unreal Tournament 2004"));
		Out.Add(TEXT("C:/Program Files (x86)/Unreal Tournament 2004"));
		Out.Add(TEXT("C:/GOG Games/Unreal Tournament 2004"));
		Out.Add(TEXT("C:/Program Files (x86)/Steam/steamapps/common/Unreal Tournament 2004"));
#elif PLATFORM_MAC
		Out.Add(TEXT("/Applications/Unreal Tournament 2004"));
		Out.Add(Home / TEXT("Applications/Unreal Tournament 2004"));
#else
		Out.Add(Home / TEXT("ut2004"));
		Out.Add(Home / TEXT("UT2004"));
		Out.Add(Home / TEXT(".local/share/ut2004"));
		Out.Add(Home / TEXT(".local/share/Steam/steamapps/common/Unreal Tournament 2004"));
		Out.Add(Home / TEXT("Games/ut2004"));
		Out.Add(Home / TEXT("Games/Unreal Tournament 2004"));
		Out.Add(TEXT("/usr/local/games/ut2004"));
		Out.Add(TEXT("/opt/ut2004"));
#endif
		return Out;
	}

	FString FindUT2004Install()
	{
		FString Dir;
		if (FParse::Value(FCommandLine::Get(), TEXT("UT2004Dir="), Dir) && IsUT2004Install(Dir))
		{
			return FPaths::ConvertRelativePathToFull(Dir);
		}
		Dir = FPlatformMisc::GetEnvironmentVariable(TEXT("OPENUT2K4_UT2004_DIR"));
		if (IsUT2004Install(Dir))
		{
			return FPaths::ConvertRelativePathToFull(Dir);
		}
		Dir = UArenaUserSettings::Get()->UT2004Dir;
		if (IsUT2004Install(Dir))
		{
			return FPaths::ConvertRelativePathToFull(Dir);
		}
		// the install the content cache was converted from (cache.json "source_install.path"): the same install the
		// setup screen and converter already chose, so net play and conversion use one install
		{
			const FString CacheJson = GetCacheDir() / TEXT("cache.json");
			FString Json;
			if (FFileHelper::LoadFileToString(Json, *CacheJson))
			{
				TSharedPtr<FJsonObject> Root;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
				const TSharedPtr<FJsonObject>* Source = nullptr;
				if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid()
					&& Root->TryGetObjectField(TEXT("source_install"), Source) && Source && (*Source).IsValid())
				{
					const FString Path = (*Source)->GetStringField(TEXT("path"));
					if (IsUT2004Install(Path))
					{
						return FPaths::ConvertRelativePathToFull(Path);
					}
				}
			}
		}
		for (const FString& Candidate : GetSearchLocations())
		{
			if (IsUT2004Install(Candidate))
			{
				return Candidate;
			}
		}
		static bool bWarnedNotFound = false;
		if (!bWarnedNotFound)
		{
			bWarnedNotFound = true;
			UE_LOG(LogOpenUT2k4, Warning, TEXT("UT2004 install not found"));
		}
		return FString();
	}
}

// ---------------------------------------------------------------------------------------------
// Localization files
// ---------------------------------------------------------------------------------------------

FString ArenaContent::FindLocalizationFile(const FString& FileName)
{
	// lower-case file name -> path, per folder (scanned once; the cache's copies win over the install)
	static TMap<FString, TMap<FString, FString>> Listings;
	auto Lookup = [](const FString& Dir, const FString& Name) -> FString
	{
		if (Dir.IsEmpty())
		{
			return FString();
		}
		TMap<FString, FString>* L = Listings.Find(Dir);
		// an empty folder is looked at again (the startup content pass may be copying the files right now)
		if (!L || L->Num() == 0)
		{
			L = &Listings.FindOrAdd(Dir);
			TArray<FString> Files;
			IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.int")), true, false);
			for (const FString& F : Files)
			{
				L->Add(F.ToLower(), Dir / F);
			}
		}
		const FString* P = L->Find(Name.ToLower());
		return P ? *P : FString();
	};
	FString Path = Lookup(GetCacheDir() / TEXT("localization"), FileName);
	if (Path.IsEmpty())
	{
		static const FString Install = FindUT2004Install();
		if (!Install.IsEmpty())
		{
			// System/ may be spelled differently on case-sensitive file systems
			TArray<FString> Dirs;
			IFileManager::Get().FindFiles(Dirs, *(Install / TEXT("*")), false, true);
			for (const FString& D : Dirs)
			{
				if (D.Equals(TEXT("System"), ESearchCase::IgnoreCase))
				{
					Path = Lookup(Install / D, FileName);
					break;
				}
			}
		}
	}
	return Path;
}
