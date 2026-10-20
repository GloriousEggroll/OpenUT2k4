// Shipping log file (ArenaShippingLog.h): this module's UE_LOG lines when the engine has no logging of its own.

#include "ArenaShippingLog.h"

#if NO_LOGGING

#include "CoreGlobals.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Templates/UniquePtr.h"

namespace ArenaShippingLogLocal
{
	/** A runaway message cannot fill the disk: writing stops at this size (the next run starts a new file). */
	constexpr int64 MaxBytes = 64ll * 1024 * 1024;

	struct FLogFile
	{
		FCriticalSection Lock;
		TUniquePtr<IFileHandle> Handle;
		bool bOpened = false;
		bool bFull = false;
		int64 Bytes = 0;
	};

	FLogFile& File()
	{
		static FLogFile F;
		return F;
	}

	const TCHAR* VerbosityName(ELogVerbosity::Type Verbosity)
	{
		switch (Verbosity & ELogVerbosity::VerbosityMask)
		{
		case ELogVerbosity::Fatal: return TEXT("Fatal: ");
		case ELogVerbosity::Error: return TEXT("Error: ");
		case ELogVerbosity::Warning: return TEXT("Warning: ");
		case ELogVerbosity::Display: return TEXT("Display: ");
		default: return TEXT("");
		}
	}

	void WriteLine(FLogFile& F, const FString& Line)
	{
		// every line goes to the file at once (one write call), so the last lines before a crash are there: the kernel
		// keeps written data when the process dies. No sync to the disk: IFileHandle::Flush / FArchive::Flush is an
		// fdatasync on Linux, several milliseconds per line on the game thread (a burst of lines was a visible hitch)
		const FTCHARToUTF8 Utf8(*(Line + TEXT("\n")));
		F.Handle->Write(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		F.Bytes += Utf8.Length();
	}

	/** Opens <Saved>/Logs/<Name>.log, keeping the previous run's as <Name>-backup.log. */
	void Open(FLogFile& F)
	{
		F.bOpened = true;
		const FString Name = IsRunningDedicatedServer() ? TEXT("OpenUT2k4Server") : TEXT("OpenUT2k4");
		const FString Dir = FPaths::ProjectLogDir();
		const FString Path = Dir / (Name + TEXT(".log"));
		IFileManager& FM = IFileManager::Get();
		FM.MakeDirectory(*Dir, true);
		if (FM.FileExists(*Path))
		{
			FM.Move(*(Dir / (Name + TEXT("-backup.log"))), *Path, true, true);
		}
		F.Handle.Reset(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Path, false, true));
		if (F.Handle)
		{
			FString Version;
			if (GConfig)
			{
				GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("ProjectVersion"), Version, GGameIni);
			}
			WriteLine(F, FString::Printf(TEXT("Log file open, %s (OpenUT2k4 %s, %s, engine %s)"), *FDateTime::Now().ToString(),
				*Version, LexToString(FApp::GetBuildConfiguration()), FApp::GetBuildVersion()));
		}
	}
}

void ArenaShippingLog::Write(const TCHAR* Category, ELogVerbosity::Type Verbosity, const FString& Message)
{
	using namespace ArenaShippingLogLocal;
	FLogFile& F = File();
	FScopeLock Lock(&F.Lock);
	if (!F.bOpened)
	{
		Open(F);
	}
	if (!F.Handle || F.bFull)
	{
		return;
	}
	// the engine's format: [time][frame]Category: Verbosity: message
	const FDateTime Now = FDateTime::Now();
	WriteLine(F, FString::Printf(TEXT("[%s:%03d][%3llu]%s: %s%s"), *Now.ToString(TEXT("%Y.%m.%d-%H.%M.%S")), Now.GetMillisecond(),
		GFrameCounter % 1000, Category, VerbosityName(Verbosity), *Message));
	if (F.Bytes >= MaxBytes)
	{
		WriteLine(F, TEXT("Log file full: no more lines are written this run"));
		F.bFull = true;
	}
}

#endif // NO_LOGGING
