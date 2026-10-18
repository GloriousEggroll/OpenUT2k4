#include "ArenaConverter.h"
#include "ArenaConvConfig.h"
#include "ArenaConvData.h"
#include "ArenaConvFonts.h"
#include "ArenaConvLocalization.h"
#include "ArenaConvScripts.h"
#include "ArenaConvExporter.h"
#include "ArenaConvJson.h"
#include "../ArenaContent.h"
#include "../OpenUT2k4.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

class FArenaConvertRunnable : public FRunnable
{
public:
	FArenaConvertRunnable(FArenaConverter& InOwner, const FArenaConverter::FOptions& InOptions) : Owner(InOwner), Options(InOptions) {}
	virtual uint32 Run() override
	{
		Owner.Run(Options);
		return 0;
	}
	FArenaConverter& Owner;
	FArenaConverter::FOptions Options;
};

namespace ArenaConverterLocal
{
	constexpr int32 StateVersion = 1;

	struct FUnit
	{
		enum class EKind : uint8 { Music, Package, Map };
		EKind Kind;
		FString Name;
		FString File;
		int64 Size = 0;
		int64 MTime = 0;
		FString Key() const
		{
			return FString::Printf(TEXT("%s:%s"), Kind == EKind::Music ? TEXT("music") : Kind == EKind::Package ? TEXT("pkg") : TEXT("map"), *Name.ToLower());
		}
	};

	int64 UnixTime(const FDateTime& T)
	{
		return T == FDateTime::MinValue() ? 0 : (T - FDateTime(1970, 1, 1)).GetTicks() / ETimespan::TicksPerSecond;
	}

	void AppendLog(const FString& Path, const TArray<FString>& Lines)
	{
		if (!Lines.Num())
		{
			return;
		}
		FString Text;
		for (const FString& L : Lines)
		{
			Text += L;
			Text += TEXT("\n");
		}
		FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
	}

	TArray<FString> SplitList(const FString& S)
	{
		TArray<FString> Out;
		S.ParseIntoArray(Out, TEXT(","), true);
		for (FString& X : Out)
		{
			X.TrimStartAndEndInline();
		}
		return Out;
	}
}

using namespace ArenaConverterLocal;

FArenaConverter& FArenaConverter::Get()
{
	static FArenaConverter Instance;
	return Instance;
}

FArenaConverter::~FArenaConverter()
{
	Cancel();
	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	delete Runnable;
	Runnable = nullptr;
}

bool FArenaConverter::Start(const FOptions& Options)
{
	if (bBusy.Load())
	{
		return false;
	}
	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	delete Runnable;
	{
		FScopeLock L(&Lock);
		Progress = FArenaConvertProgress();
		Progress.bRunning = true;
		Progress.Stage = TEXT("Starting");
	}
	bBusy = true;
	Runnable = new FArenaConvertRunnable(*this, Options);
	Thread = FRunnableThread::Create(Runnable, TEXT("ArenaContentConverter"), 0, TPri_BelowNormal);
	if (!Thread)
	{
		bBusy = false;
		FScopeLock L(&Lock);
		Progress.bRunning = false;
		Progress.bFinished = true;
		Progress.FatalError = TEXT("could not start the converter thread");
		return false;
	}
	return true;
}

bool FArenaConverter::RunBlocking(const FOptions& Options)
{
	if (bBusy.Load())
	{
		return false;
	}
	{
		FScopeLock L(&Lock);
		Progress = FArenaConvertProgress();
		Progress.bRunning = true;
	}
	bBusy = true;
	return Run(Options);
}

void FArenaConverter::Shutdown()
{
	FArenaConverter& C = Get();
	C.Cancel();
	if (C.Thread)
	{
		C.Thread->WaitForCompletion();
		delete C.Thread;
		C.Thread = nullptr;
	}
}

void FArenaConverter::Cancel()
{
	bCancel = true;
}

bool FArenaConverter::IsRunning() const
{
	return bBusy.Load();
}

FArenaConvertProgress FArenaConverter::GetProgress() const
{
	FScopeLock L(&Lock);
	return Progress;
}

void FArenaConverter::SetStage(const FString& Stage)
{
	FScopeLock L(&Lock);
	Progress.Stage = Stage;
	Progress.Item.Reset();
	UE_LOG(LogOpenUT2k4, Log, TEXT("Converter: %s"), *Stage);
}

bool FArenaConverter::Run(FOptions Options)
{
#if ARENA_CONV_BUILD
	const double T0 = FPlatformTime::Seconds();
	bCancel = false;
	auto Finish = [&](bool bOk, const FString& Fatal)
	{
		FScopeLock L(&Lock);
		Progress.bRunning = false;
		Progress.bFinished = true;
		Progress.bSucceeded = bOk;
		Progress.bCancelled = bCancel.Load();
		Progress.FatalError = Fatal;
		Progress.ElapsedSeconds = FPlatformTime::Seconds() - T0;
		if (!Fatal.IsEmpty())
		{
			UE_LOG(LogOpenUT2k4, Error, TEXT("Converter: %s"), *Fatal);
		}
		bBusy = false;
		return bOk;
	};
	if (!ArenaContent::IsUT2004Install(Options.InstallDir))
	{
		return Finish(false, FString::Printf(TEXT("No UT2004 install at \"%s\" (it needs System and Maps folders)."), *Options.InstallDir));
	}
	const FString Out = FPaths::ConvertRelativePathToFull(Options.CacheDir.IsEmpty() ? ArenaContent::GetCacheDir() : Options.CacheDir);
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.CreateDirectoryTree(*Out))
	{
		return Finish(false, FString::Printf(TEXT("Cannot create the content cache folder \"%s\"."), *Out));
	}
	// A cache written in another format (ArenaContent::CacheFormatVersion) must not keep any of its old files: every
	// asset is converted again instead of skipping the files that exist. "full_rewrite" in the resume state carries
	// that over to a resumed run, so an interrupted rewrite never mixes old and new files.
	bool bFormatChanged = false;
	bool bResumeRewrite = false;
	{
		FConvJson OldCache;
		if (FConvJson::LoadFile(Out / TEXT("cache.json"), OldCache)
			&& int32(OldCache.GetNumber(TEXT("format_version"), 0)) != ArenaContent::CacheFormatVersion)
		{
			bFormatChanged = true;
		}
		FConvJson OldState;
		if (FConvJson::LoadFile(Out / TEXT("convert_state.json"), OldState))
		{
			if (int32(OldState.GetNumber(TEXT("cache_format"), 0)) != ArenaContent::CacheFormatVersion)
			{
				bFormatChanged = true;
			}
			bResumeRewrite = OldState.GetNumber(TEXT("full_rewrite"), 0) != 0;
		}
	}
	const bool bFreshForce = Options.bForce || bFormatChanged;
	const bool bRewrite = bFreshForce || bResumeRewrite;
	if (bFormatChanged)
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("Converter: the cache was written in another format, converting everything again"));
	}
	// the cache is incomplete from now on until cache.json is written again
	PF.DeleteFile(*(Out / TEXT("cache.json")));
	const FString ErrLog = Out / TEXT("convert_errors.log");
	const FString StatePath = Out / TEXT("convert_state.json");
	const FString NotesLog = Out / TEXT("convert_notes.log");
	const FString Install = FPaths::ConvertRelativePathToFull(Options.InstallDir);
	UE_LOG(LogOpenUT2k4, Log, TEXT("Converter: %s -> %s (%s)"), *Install, *Out,
		Options.Maps.Num() ? *FString::Join(Options.Maps, TEXT(",")) : TEXT("whole install"));

	SetStage(TEXT("Reading the UT2004 install"));
	FConvPackageManager Mgr(Install);
	FConvExporter::FOptions ExOpt;
	ExOpt.OutDir = Out;
	ExOpt.PngLevel = Options.PngLevel;
	ExOpt.bSkipExisting = !bRewrite;
	ExOpt.bBackgroundTasks = !IsInGameThread();
	FConvExporter Ex(Mgr, ExOpt);
	Ex.CancelFlag = &bCancel;

	// ---- units of work ----
	TArray<FUnit> Units;
	for (const FString& M : Ex.MusicFiles())
	{
		FUnit U{ FUnit::EKind::Music, FPaths::GetBaseFilename(M), M };
		Units.Add(U);
	}
	TArray<FString> MapNames = Options.Maps;
	if (MapNames.Num() == 0)
	{
		for (const TPair<FString, FString>& KV : Mgr.PackageFiles())
		{
			const FString Ext = FPaths::GetExtension(KV.Value, true).ToLower();
			if (Ext == TEXT(".utx") || Ext == TEXT(".usx") || Ext == TEXT(".uax") || Ext == TEXT(".ukx") || Ext == TEXT(".u"))
			{
				Units.Add({ FUnit::EKind::Package, FPaths::GetBaseFilename(KV.Value), KV.Value });
			}
		}
		TArray<FString> MapFiles;
		for (const TPair<FString, FString>& KV : Mgr.PackageFiles(TEXT(".ut2")))
		{
			MapFiles.Add(KV.Value);
		}
		MapFiles.Sort([](const FString& A, const FString& B) { return FCString::Strcmp(*A, *B) < 0; });
		for (const FString& F : MapFiles)
		{
			MapNames.Add(FPaths::GetBaseFilename(F));
		}
	}
	for (const FString& M : MapNames)
	{
		FString File = Mgr.FindFile(M);
		Units.Add({ FUnit::EKind::Map, File.IsEmpty() ? M : FPaths::GetBaseFilename(File), File.IsEmpty() ? M : File });
	}
	int64 TotalSize = 0;
	for (FUnit& U : Units)
	{
		const FFileStatData St = PF.GetStatData(*U.File);
		U.Size = FMath::Max<int64>(St.FileSize, 1) + (U.Kind == FUnit::EKind::Map ? (4ll << 20) : 0);
		U.MTime = UnixTime(St.ModificationTime);
		TotalSize += U.Size;
	}
	{
		FScopeLock L(&Lock);
		Progress.UnitsTotal = Units.Num();
	}

	// ---- resume state ----
	FConvJson State;
	if (bFreshForce || !FConvJson::LoadFile(StatePath, State) || State.GetNumber(TEXT("version"), 0) != StateVersion
		|| State.GetString(TEXT("install")) != Install)
	{
		// a fresh run: start the logs over (a resumed run appends to them)
		PF.DeleteFile(*ErrLog);
		PF.DeleteFile(*NotesLog);
		State = FConvJson::Object();
		State.Set(TEXT("version"), StateVersion);
		State.Set(TEXT("install"), Install);
		State.Set(TEXT("cache_format"), ArenaContent::CacheFormatVersion);
		State.Set(TEXT("full_rewrite"), bRewrite ? 1 : 0);
		State.Set(TEXT("units"), FConvJson::Object());
	}
	TArray<FString> PendingDone;
	auto Commit = [&]()
	{
		Ex.Flush();
		FConvJson& Done = *State.Find(TEXT("units"));
		for (const FString& K : PendingDone)
		{
			for (const FUnit& U : Units)
			{
				if (U.Key() == K)
				{
					Done.Set(K, U.MTime);
					break;
				}
			}
		}
		PendingDone.Reset();
		State.SaveToFile(StatePath, -1);
	};

	int64 SizeDone = 0;
	const TCHAR* LastStage = nullptr;
	for (int32 i = 0; i < Units.Num(); ++i)
	{
		if (bCancel.Load())
		{
			break;
		}
		const FUnit& U = Units[i];
		const TCHAR* Stage = U.Kind == FUnit::EKind::Music ? TEXT("Music") : U.Kind == FUnit::EKind::Package ? TEXT("Textures, meshes and sounds") : TEXT("Maps");
		if (Stage != LastStage)
		{
			SetStage(Stage);
			LastStage = Stage;
		}
		{
			FScopeLock L(&Lock);
			Progress.Item = U.Name;
			Progress.UnitsDone = i;
			Progress.Fraction = 0.9f * float(double(SizeDone) / double(FMath::Max<int64>(TotalSize, 1)));
			Progress.ElapsedSeconds = FPlatformTime::Seconds() - T0;
		}
		const FConvJson& Done = State.Get(TEXT("units"));
		const FConvJson* Prev = Done.Find(U.Key());
		const bool bSkip = !bFreshForce && Prev && Prev->IsNumber() && int64(Prev->AsNumber()) == U.MTime && U.Kind != FUnit::EKind::Map;
		if (!bSkip)
		{
			switch (U.Kind)
			{
			case FUnit::EKind::Music: Ex.ExportMusicFile(U.File); break;
			case FUnit::EKind::Package: Ex.ExportPackage(U.Name); break;
			case FUnit::EKind::Map: Ex.ExportMap(U.Kind == FUnit::EKind::Map && Mgr.FindFile(U.Name).IsEmpty() ? U.File : U.Name); break;
			}
			// release every open package between units (bounded memory, like the Python exporter)
			Mgr.CloseAll();
		}
		TArray<FString> NewNotes = Ex.TakeNewNotes();
		for (FString& N : NewNotes)
		{
			N = U.Name + TEXT("\t") + N;
		}
		AppendLog(NotesLog, NewNotes);
		TArray<FString> NewErrors = Ex.TakeNewErrors();
		for (FString& E : NewErrors)
		{
			E = U.Name + TEXT("\t") + E;
		}
		AppendLog(ErrLog, NewErrors);
		{
			FScopeLock L(&Lock);
			Progress.ErrorCount += NewErrors.Num();
			for (const FString& E : NewErrors)
			{
				Progress.RecentErrors.Add(E);
			}
			while (Progress.RecentErrors.Num() > 8)
			{
				Progress.RecentErrors.RemoveAt(0);
			}
		}
		if (!bCancel.Load())
		{
			PendingDone.Add(U.Key());
		}
		SizeDone += U.Size;
		if (PendingDone.Num() >= 16)
		{
			Commit();
		}
	}
	// the HUD fonts (glyph tables; their pages are textures): also brings a cache converted before fonts were exported
	// up to date, and a -ConvertMaps run gets them too
	if (!bCancel.Load())
	{
		SetStage(TEXT("Fonts"));
		ArenaConvFonts::ExportAll(Mgr, Ex, Out, &bCancel);
	}
	// the localization files the game reads its texts from (ArenaConvLocalization.h)
	if (!bCancel.Load())
	{
		SetStage(TEXT("Language files"));
		ArenaConvLocalization::CopyAll(Install, Out);
	}
	// the script packages the UnrealScript runtime loads (ArenaConvScripts.h)
	if (!bCancel.Load())
	{
		SetStage(TEXT("Script packages"));
		ArenaConvScripts::CopyAll(Install, Out, &bCancel);
	}
	SetStage(TEXT("Finishing textures"));
	Commit();
	AppendLog(ErrLog, Ex.TakeNewErrors());
	AppendLog(NotesLog, Ex.TakeNewNotes());
	if (bCancel.Load())
	{
		return Finish(false, FString());
	}

	// ---- cache-side material support and game data ----
	SetStage(TEXT("Material data"));
	{
		FScopeLock L(&Lock);
		Progress.Fraction = 0.92f;
	}
	ArenaConvData::FResult DataResult;
	TMap<FString, FConvJson> NewStats = Ex.TakeTextureStats();
	ArenaConvData::BakeCombiners(Out, NewStats, DataResult);
	ArenaConvData::WriteTextureStats(Out, NewStats, DataResult, &bCancel);
	if (bCancel.Load())
	{
		return Finish(false, FString());
	}
	SetStage(TEXT("Game data"));
	{
		FScopeLock L(&Lock);
		Progress.Fraction = 0.97f;
	}
	ArenaConvData::WriteGameData(Out, DataResult);
	AppendLog(ErrLog, DataResult.Errors);

	// ---- cache.json (last) ----
	FConvJson Cache = FConvJson::Object();
	Cache.Set(TEXT("format_version"), ArenaContent::CacheFormatVersion);
	Cache.Set(TEXT("complete"), true);
	Cache.Set(TEXT("partial_maps"), Options.Maps.Num() > 0);
	FConvJson Src = FConvJson::Object();
	Src.Set(TEXT("path"), Install);
	Src.Set(TEXT("packages"), Mgr.FileIndex.Num());
	Cache.Set(TEXT("source_install"), Src);
	Cache.Set(TEXT("created"), FDateTime::UtcNow().ToIso8601());
	Cache.Set(TEXT("seconds"), FConvJson::RoundTo(FPlatformTime::Seconds() - T0, 1));
	FConvJson Counts = FConvJson::Object();
	for (const TPair<FString, int32>& KV : Ex.Counts)
	{
		Counts.Set(KV.Key, KV.Value);
	}
	for (const TPair<FString, int32>& KV : DataResult.Counts)
	{
		Counts.Set(KV.Key, KV.Value);
	}
	Cache.Set(TEXT("counts"), Counts);
	{
		FScopeLock L(&Lock);
		Cache.Set(TEXT("errors"), Progress.ErrorCount + DataResult.Errors.Num());
		Progress.Fraction = 1.f;
		Progress.UnitsDone = Units.Num();
	}
	if (!Cache.SaveToFile(Out / TEXT("cache.json"), 1))
	{
		return Finish(false, TEXT("Cannot write cache.json."));
	}
	TArray<ANSICHAR> CountsText;
	Counts.Write(CountsText, -1);
	CountsText.Add(0);
	UE_LOG(LogOpenUT2k4, Log, TEXT("Converter: done in %.1fs, %s"), FPlatformTime::Seconds() - T0, ANSI_TO_TCHAR(CountsText.GetData()));
	return Finish(true, FString());
#else
	FScopeLock L(&Lock);
	Progress.bRunning = false;
	Progress.bFinished = true;
	Progress.FatalError = TEXT("The content converter is not part of this build.");
	bBusy = false;
	return false;
#endif
}

// ---------------------------------------------------------------------------------------------
// -ConvertOnly

namespace ArenaConverterLocal
{
	void RunCommandLineConversion()
	{
		const TCHAR* Cmd = FCommandLine::Get();
#if ARENA_CONV_BUILD
		// -ConvertFontsOnly: only export the install's Font objects into the cache (ArenaConvFonts), then quit
		if (FParse::Param(Cmd, TEXT("ConvertFontsOnly")))
		{
			FString Dir;
			if (!FParse::Value(Cmd, TEXT("UT2004Dir="), Dir))
			{
				Dir = ArenaContent::FindUT2004Install();
			}
			FString OutDir;
			if (!FParse::Value(Cmd, TEXT("ConvertTo="), OutDir))
			{
				OutDir = ArenaContent::GetCacheDir();
			}
			OutDir = FPaths::ConvertRelativePathToFull(OutDir);
			int32 Count = -1;
			if (ArenaContent::IsUT2004Install(Dir))
			{
				FConvPackageManager Mgr(FPaths::ConvertRelativePathToFull(Dir));
				FConvExporter::FOptions ExOpt;
				ExOpt.OutDir = OutDir;
				ExOpt.bSkipExisting = true;
				FConvExporter Ex(Mgr, ExOpt);
				Count = ArenaConvFonts::ExportAll(Mgr, Ex, OutDir, nullptr);
				ArenaConvLocalization::CopyAll(FPaths::ConvertRelativePathToFull(Dir), OutDir);
				for (const FString& E : Ex.TakeNewErrors())
				{
					UE_LOG(LogOpenUT2k4, Warning, TEXT("ConvertFontsOnly: %s"), *E);
				}
			}
			UE_LOG(LogOpenUT2k4, Display, TEXT("ConvertFontsOnly: %d fonts in %s"), Count, *OutDir);
			FPlatformMisc::RequestExitWithStatus(true, Count >= 0 ? 0 : 1);
			return;
		}
#endif
		if (!FParse::Param(Cmd, TEXT("ConvertOnly")))
		{
			return;
		}
		FArenaConverter::FOptions Opt;
		FString Dir;
		if (!FParse::Value(Cmd, TEXT("UT2004Dir="), Dir))
		{
			Dir = ArenaContent::FindUT2004Install();
		}
		Opt.InstallDir = Dir;
		FParse::Value(Cmd, TEXT("ConvertTo="), Opt.CacheDir);
		FString Maps;
		if (FParse::Value(Cmd, TEXT("ConvertMaps="), Maps, false))
		{
			Opt.Maps = SplitList(Maps);
		}
		Opt.bForce = FParse::Param(Cmd, TEXT("ConvertForce"));
		FParse::Value(Cmd, TEXT("ConvertPngLevel="), Opt.PngLevel);
		// -ConvertCompareOnly: no conversion, only -ConvertCompare of an existing output folder
		const bool bOk = FParse::Param(Cmd, TEXT("ConvertCompareOnly")) || FArenaConverter::Get().RunBlocking(Opt);
		const FArenaConvertProgress P = FArenaConverter::Get().GetProgress();
		UE_LOG(LogOpenUT2k4, Display, TEXT("ConvertOnly: %s in %.1fs, %d errors%s%s"), bOk ? TEXT("succeeded") : TEXT("FAILED"), P.ElapsedSeconds,
			P.ErrorCount, P.FatalError.IsEmpty() ? TEXT("") : TEXT(": "), *P.FatalError);
		FString Ref;
		if (bOk && FParse::Value(Cmd, TEXT("ConvertCompare="), Ref))
		{
			FString RefData;
			if (!FParse::Value(Cmd, TEXT("ConvertCompareData="), RefData))
			{
				RefData = FPaths::ProjectContentDir() / TEXT("ImportedData");
			}
			const FString OutDir = Opt.CacheDir.IsEmpty() ? ArenaContent::GetCacheDir() : Opt.CacheDir;
			FArenaConverter::CompareCaches(FPaths::ConvertRelativePathToFull(OutDir), FPaths::ConvertRelativePathToFull(Ref),
				FPaths::ConvertRelativePathToFull(RefData), Opt.Maps);
		}
		FPlatformMisc::RequestExitWithStatus(true, bOk ? 0 : 1);
	}

	struct FRegisterCommandLine
	{
		FRegisterCommandLine()
		{
			FCoreDelegates::OnFEngineLoopInitComplete.AddStatic(&RunCommandLineConversion);
			// quitting mid-conversion: stop the worker before the engine shuts down (the cache stays resumable)
			FCoreDelegates::OnPreExit.AddStatic(&FArenaConverter::Shutdown);
		}
	};
	static FRegisterCommandLine GRegisterCommandLine;
}
