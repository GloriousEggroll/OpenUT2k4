#include "ArenaConvLocalization.h"
#include "ArenaConverter.h"
#include "ArenaConvJson.h"
#include "../OpenUT2k4.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace ArenaConvLocalizationLocal
{
	FString Dir(const FString& CacheDir)
	{
		return CacheDir / TEXT("localization");
	}

	FString IndexPath(const FString& CacheDir)
	{
		return Dir(CacheDir) / TEXT("index.json");
	}

	/** The install's System folder, whatever its case. */
	FString SystemDir(const FString& InstallDir)
	{
		TArray<FString> Dirs;
		IFileManager::Get().FindFiles(Dirs, *(InstallDir / TEXT("*")), false, true);
		for (const FString& D : Dirs)
		{
			if (D.Equals(TEXT("System"), ESearchCase::IgnoreCase))
			{
				return InstallDir / D;
			}
		}
		return FString();
	}
}

bool ArenaConvLocalization::NeedsUpdate(const FString& CacheDir)
{
	FConvJson Index;
	return !FConvJson::LoadFile(ArenaConvLocalizationLocal::IndexPath(CacheDir), Index)
		|| int32(Index.GetNumber(TEXT("version"), 0)) < ArenaConverterVersions::LocalizationVersion;
}

int32 ArenaConvLocalization::CopyAll(const FString& InstallDir, const FString& CacheDir)
{
	using namespace ArenaConvLocalizationLocal;
	const FString System = InstallDir.IsEmpty() ? FString() : SystemDir(InstallDir);
	if (System.IsEmpty() || CacheDir.IsEmpty())
	{
		return -1;
	}
	IFileManager& FM = IFileManager::Get();
	const FString Out = Dir(CacheDir);
	FM.MakeDirectory(*Out, true);
	TArray<FString> Files;
	for (const TCHAR* Pattern : { TEXT("*.int"), TEXT("*.ucl"), TEXT("*.upl") })
	{
		TArray<FString> Found;
		FM.FindFiles(Found, *(System / Pattern), true, false);
		Files.Append(Found);
	}
	Files.Sort();
	FConvJson Names = FConvJson::Array();
	int32 Count = 0;
	int32 Copied = 0;
	for (const FString& F : Files)
	{
		const FString Src = System / F;
		const FString Dst = Out / F;
		const FDateTime SrcTime = FM.GetTimeStamp(*Src);
		if (!FM.FileExists(*Dst) || FM.GetTimeStamp(*Dst) < SrcTime || FM.FileSize(*Dst) != FM.FileSize(*Src))
		{
			if (FM.Copy(*Dst, *Src) != COPY_OK)
			{
				UE_LOG(LogOpenUT2k4, Warning, TEXT("Converter: cannot copy %s to the cache"), *Src);
				continue;
			}
			++Copied;
		}
		Names.Add(FConvJson(F));
		++Count;
	}
	FConvJson Index = FConvJson::Object();
	Index.Set(TEXT("version"), ArenaConverterVersions::LocalizationVersion);
	Index.Set(TEXT("files"), Names);
	if (!Index.SaveToFile(IndexPath(CacheDir), 1))
	{
		return -1;
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("Converter: %d localization files in the cache (%d copied, version %d)"), Count, Copied,
		ArenaConverterVersions::LocalizationVersion);
	return Count;
}
