#include "ArenaConvScripts.h"
#include "ArenaConverter.h"
#include "ArenaConvJson.h"
#include "ArenaConvPackage.h"
#include "../OpenUT2k4.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace ArenaConvScriptsLocal
{
	FString IndexPath(const FString& CacheDir)
	{
		return ArenaConvScripts::GetDir(CacheDir) / TEXT("index.json");
	}

	/** A child folder whatever its case. */
	FString FindChild(const FString& Dir, const TCHAR* Child)
	{
		TArray<FString> Dirs;
		IFileManager::Get().FindFiles(Dirs, *(Dir / TEXT("*")), false, true);
		for (const FString& D : Dirs)
		{
			if (D.Equals(Child, ESearchCase::IgnoreCase))
			{
				return Dir / D;
			}
		}
		return FString();
	}

	/** Copies Src to Dst unless an identical-looking copy (same size, not older) is there. */
	bool CopyIfNeeded(const FString& Src, const FString& Dst)
	{
		IFileManager& FM = IFileManager::Get();
		if (FM.FileExists(*Dst) && FM.FileSize(*Dst) == FM.FileSize(*Src) && FM.GetTimeStamp(*Dst) >= FM.GetTimeStamp(*Src))
		{
			return true;
		}
		if (FM.Copy(*Dst, *Src) != COPY_OK)
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("Converter: cannot copy %s to the cache"), *Src);
			return false;
		}
		return true;
	}

	/** True when the package defines script classes (a Class export). */
	bool DefinesClasses(const FString& Path)
	{
		FConvPackage Pkg(Path, nullptr);
		FString Err;
		if (!Pkg.Open(Err))
		{
			return false;
		}
		for (const FConvExport& E : Pkg.Exports)
		{
			if (E.ClassIndex == 0)
			{
				return true;
			}
		}
		return false;
	}
}

FString ArenaConvScripts::GetDir(const FString& CacheDir)
{
	return CacheDir / TEXT("scripts");
}

bool ArenaConvScripts::NeedsUpdate(const FString& CacheDir)
{
	FConvJson Index;
	return !FConvJson::LoadFile(ArenaConvScriptsLocal::IndexPath(CacheDir), Index)
		|| int32(Index.GetNumber(TEXT("version"), 0)) < ArenaConverterVersions::ScriptsVersion;
}

bool ArenaConvScripts::HasScripts(const FString& CacheDir)
{
	return !CacheDir.IsEmpty() && IFileManager::Get().FileExists(*(GetDir(CacheDir) / TEXT("System") / TEXT("Core.u")));
}

bool ArenaConvScripts::CopyAll(const FString& InstallDir, const FString& CacheDir, const TAtomic<bool>* Cancel)
{
	using namespace ArenaConvScriptsLocal;
	IFileManager& FM = IFileManager::Get();
	const FString System = InstallDir.IsEmpty() ? FString() : FindChild(InstallDir, TEXT("System"));
	if (System.IsEmpty() || CacheDir.IsEmpty())
	{
		return false;
	}
	const FString OutSystem = GetDir(CacheDir) / TEXT("System");
	const FString OutMaps = GetDir(CacheDir) / TEXT("Maps");
	FM.MakeDirectory(*OutSystem, true);
	FM.MakeDirectory(*OutMaps, true);
	FConvJson Packages = FConvJson::Array();
	FConvJson Maps = FConvJson::Array();
	int64 Bytes = 0;
	int32 NumPackages = 0, NumMaps = 0;
	TArray<FString> Files;
	FM.FindFiles(Files, *(System / TEXT("*.u")), true, false);
	for (const TCHAR* Ini : { TEXT("Default.ini"), TEXT("DefUser.ini"), TEXT("UT2004.ini"), TEXT("User.ini") })
	{
		if (FM.FileExists(*(System / Ini)))
		{
			Files.Add(Ini);
		}
	}
	Files.Sort();
	for (const FString& F : Files)
	{
		if (Cancel && Cancel->Load())
		{
			return false;
		}
		if (CopyIfNeeded(System / F, OutSystem / F))
		{
			Packages.Add(FConvJson(F));
			Bytes += FM.FileSize(*(OutSystem / F));
			++NumPackages;
		}
	}
	// maps that carry their own script classes (the runtime loads them as packages)
	const FString MapsDir = FindChild(InstallDir, TEXT("Maps"));
	if (!MapsDir.IsEmpty())
	{
		TArray<FString> MapFiles;
		FM.FindFiles(MapFiles, *(MapsDir / TEXT("*.ut2")), true, false);
		MapFiles.Sort();
		for (const FString& M : MapFiles)
		{
			if (Cancel && Cancel->Load())
			{
				return false;
			}
			if (DefinesClasses(MapsDir / M) && CopyIfNeeded(MapsDir / M, OutMaps / M))
			{
				Maps.Add(FConvJson(M));
				Bytes += FM.FileSize(*(OutMaps / M));
				++NumMaps;
			}
		}
	}
	FConvJson Index = FConvJson::Object();
	Index.Set(TEXT("version"), ArenaConverterVersions::ScriptsVersion);
	Index.Set(TEXT("packages"), Packages);
	Index.Set(TEXT("maps"), Maps);
	if (!Index.SaveToFile(IndexPath(CacheDir), 1))
	{
		return false;
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("Converter: script packages in the cache: %d files and %d maps with script classes (%.0f MB, version %d)"),
		NumPackages, NumMaps, double(Bytes) / (1024.0 * 1024.0), ArenaConverterVersions::ScriptsVersion);
	return true;
}
