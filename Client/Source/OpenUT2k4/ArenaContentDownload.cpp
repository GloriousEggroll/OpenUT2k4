#include "ArenaContentDownload.h"
#include "OpenUT2k4.h"
#include "ArenaContent.h"
#include "ArenaContentSetup.h"
#include "ArenaHUD.h"
#include "ArenaUserSettings.h"
#include "Download/ArenaDlCab.h"
#include "Download/ArenaDlHttp.h"
#include "Download/ArenaDlIso.h"
#include "Net/UE2NetCDKey.h"
#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace ArenaContentDownloadLocal
{
	const TCHAR* EpicTermsUrl = TEXT("https://legal.epicgames.com/en-US/epicgames/tos");

	/** The UT2004 disc image as published by OldUnreal (install-ut2004.sh: TITLE_PRIMARY / TITLE_BACKUP_DOWNLOAD_SOURCES). */
	constexpr int64 PrimaryIsoSize = 2995322880ll;
	const TCHAR* PrimaryIsoSha256 = TEXT("43e9182ae20bcbc0f6f4588fee6c1b336c261f1465403118a1973b09b1a22541");
	const TCHAR* PrimaryMirrors[] = {
		TEXT("https://files.oldunreal.net/UT2004.ISO"),
		TEXT("https://files2.oldunreal.net/UT2004.ISO"),
		TEXT("https://files3.oldunreal.net/UT2004.ISO"),
	};
	const FArenaDlSource BackupSource{ TEXT("https://archive.org/download/ut-2004/UT2004.ISO"), 3751510016ll,
		TEXT("7ae95242aa23d5e31b353811e1e920a4377fa53cf728b8edcb17006b7f3c4e97") };

	/** Bytes of the groups installed by default / by bFullInstall (measured from the OldUnreal image). */
	constexpr int64 InstallBytes = 6065330000ll;
	constexpr int64 FullInstallBytes = 6089200000ll;

	/**
	 * Cabinet file group -> folder in the install ("" = install root), as in install-ut2004.sh
	 * (step::ut2004_install_files). bOptional groups are only installed with FOptions::bFullInstall.
	 */
	struct FGroupTarget
	{
		const TCHAR* Group;
		const TCHAR* Target;
		bool bOptional;
	};
	const FGroupTarget GroupTargets[] = {
		{ TEXT("All_Animations"), TEXT("Animations"), false },
		{ TEXT("All_Maps"), TEXT("Maps"), false },
		{ TEXT("All_Music"), TEXT("Music"), false },
		{ TEXT("All_StaticMeshes"), TEXT("StaticMeshes"), false },
		{ TEXT("All_Textures"), TEXT("Textures"), false },
		{ TEXT("All_UT2004.EXE"), TEXT("System"), false },
		{ TEXT("English_Sounds_Speech_System_Help"), TEXT(""), false },
		// the backup image has no US_License.int group (the installer adds it only when present)
		{ TEXT("US_License.int"), TEXT("System"), false },
		{ TEXT("All_Benchmark"), TEXT("Benchmark"), true },
		{ TEXT("All_ForceFeedback"), TEXT("ForceFeedback"), true },
		{ TEXT("All_Help"), TEXT("Help"), true },
		{ TEXT("All_KarmaData"), TEXT("KarmaData"), true },
		{ TEXT("All_Web"), TEXT("Web"), true },
		{ TEXT("English_Manual"), TEXT("Manual"), true },
	};

	/** Files the installer removes before copying (Windows executables and helpers). */
	bool IsIgnored(const FString& Group, const FString& Dir, const FString& Name)
	{
		const FString Ext = FPaths::GetExtension(Name).ToLower();
		if (Group == TEXT("All_UT2004.EXE"))
		{
			return Ext == TEXT("exe");
		}
		if (Group == TEXT("English_Sounds_Speech_System_Help"))
		{
			return Dir.Equals(TEXT("System"), ESearchCase::IgnoreCase) && (Ext == TEXT("bat") || Ext == TEXT("dll") || Ext == TEXT("exe"));
		}
		return false;
	}

	/** A relative path from the cabinet is safe to join below the install folder. */
	bool IsSafeRelative(const FString& Rel)
	{
		if (Rel.IsEmpty() || Rel.StartsWith(TEXT("/")) || Rel.Contains(TEXT(":")))
		{
			return false;
		}
		TArray<FString> Parts;
		Rel.ParseIntoArray(Parts, TEXT("/"), true);
		for (const FString& P : Parts)
		{
			if (P == TEXT("..") || P == TEXT("."))
			{
				return false;
			}
		}
		return true;
	}

	FString FormatBytes(double Bytes)
	{
		if (Bytes >= 1e9) return FString::Printf(TEXT("%.2f GB"), Bytes / 1e9);
		if (Bytes >= 1e6) return FString::Printf(TEXT("%.1f MB"), Bytes / 1e6);
		return FString::Printf(TEXT("%.0f KB"), Bytes / 1e3);
	}

	FString FormatTime(double Seconds)
	{
		const int32 S = FMath::Max(0, FMath::RoundToInt(Seconds));
		return S >= 3600 ? FString::Printf(TEXT("%d:%02d:%02d"), S / 3600, (S / 60) % 60, S % 60)
			: S >= 60 ? FString::Printf(TEXT("%d:%02d"), S / 60, S % 60) : FString::Printf(TEXT("%ds"), S);
	}

	/** Free bytes on the volume holding Dir (or its nearest existing parent); -1 if unknown. */
	int64 FreeSpace(const FString& Dir, uint64* OutTotal = nullptr)
	{
		FString D = FPaths::ConvertRelativePathToFull(Dir);
		while (!D.IsEmpty() && !IFileManager::Get().DirectoryExists(*D))
		{
			const FString Parent = FPaths::GetPath(D);
			if (Parent == D)
			{
				break;
			}
			D = Parent;
		}
		uint64 Total = 0, Free = 0;
		if (D.IsEmpty() || !FPlatformMisc::GetDiskTotalAndFreeSpace(D, Total, Free))
		{
			return -1;
		}
		if (OutTotal)
		{
			*OutTotal = Total;
		}
		return int64(Free);
	}

	class FLambdaRunnable : public FRunnable
	{
	public:
		explicit FLambdaRunnable(TFunction<void()> InBody) : Body(MoveTemp(InBody)) {}
		virtual uint32 Run() override
		{
			Body();
			return 0;
		}

	private:
		TFunction<void()> Body;
	};
}

struct FArenaContentDownload::FShared
{
	mutable FCriticalSection Lock;
	FArenaContentDownloadProgress Progress;
	TAtomic<bool> bCancel{ false };
	TAtomic<bool> bRunning{ false };
	double StartTime = 0.0;

	template <typename F>
	void Update(F&& Fn)
	{
		FScopeLock Guard(&Lock);
		Fn(Progress);
		Progress.ElapsedSeconds = FPlatformTime::Seconds() - StartTime;
	}
};

FArenaContentDownload& FArenaContentDownload::Get()
{
	static FArenaContentDownload Instance;
	static bool bHooked = false;
	if (!bHooked)
	{
		bHooked = true;
		// stop the worker before the engine shuts down (the partial download stays for the next start)
		FCoreDelegates::OnPreExit.AddLambda([]()
		{
			FArenaContentDownload& D = Get();
			D.Cancel();
			if (D.Thread)
			{
				D.Thread->WaitForCompletion();
			}
		});
	}
	return Instance;
}

FArenaContentDownload::~FArenaContentDownload()
{
	if (Thread)
	{
		if (Shared)
		{
			Shared->bCancel = true;
		}
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	delete Runnable;
	Runnable = nullptr;
}

const TCHAR* FArenaContentDownload::GetTermsUrl()
{
	return ArenaContentDownloadLocal::EpicTermsUrl;
}

FString FArenaContentDownload::GetTermsText()
{
	return TEXT("The Epic Games Terms of Service apply to the use and distribution of this game, and they supersede any other end user agreements that may accompany the game.");
}

bool FArenaContentDownload::HasAcceptedTerms()
{
	return !UArenaUserSettings::Get()->AcceptedEpicTermsUtc.IsEmpty();
}

void FArenaContentDownload::AcceptTerms()
{
	UArenaUserSettings* S = UArenaUserSettings::Get();
	S->AcceptedEpicTermsUtc = FDateTime::UtcNow().ToIso8601();
	S->Save();
	UE_LOG(LogOpenUT2k4, Log, TEXT("Download: the player agreed to the Epic Games Terms of Service (%s) at %s"), GetTermsUrl(), *S->AcceptedEpicTermsUtc);
}

FString FArenaContentDownload::GetDefaultInstallDir()
{
	return ArenaContent::GetUserDataDir() / TEXT("UT2004");
}

FString FArenaContentDownload::GetDefaultDownloadDir()
{
	return ArenaContent::GetUserDataDir() / TEXT("Download");
}

int64 FArenaContentDownload::GetDownloadSize()
{
	return ArenaContentDownloadLocal::PrimaryIsoSize;
}

int64 FArenaContentDownload::GetInstallSize(bool bFullInstall)
{
	return bFullInstall ? ArenaContentDownloadLocal::FullInstallBytes : ArenaContentDownloadLocal::InstallBytes;
}

int64 FArenaContentDownload::GetRequiredDiskSpace(bool bFullInstall)
{
	return GetDownloadSize() + GetInstallSize(bFullInstall);
}

bool FArenaContentDownload::IsRunning() const
{
	return Shared.IsValid() && Shared->bRunning;
}

FArenaContentDownloadProgress FArenaContentDownload::GetProgress() const
{
	if (!Shared)
	{
		return FArenaContentDownloadProgress();
	}
	FScopeLock Guard(&Shared->Lock);
	FArenaContentDownloadProgress P = Shared->Progress;
	if (P.bRunning)
	{
		P.ElapsedSeconds = FPlatformTime::Seconds() - Shared->StartTime;
	}
	return P;
}

void FArenaContentDownload::Cancel()
{
	if (Shared)
	{
		Shared->bCancel = true;
	}
}

bool FArenaContentDownload::Start(const FOptions& InOptions, FString* OutError)
{
	auto Fail = [OutError](const FString& E)
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("Download: %s"), *E);
		if (OutError)
		{
			*OutError = E;
		}
		return false;
	};
	if (IsRunning())
	{
		return Fail(TEXT("A download is already running"));
	}
	if (InOptions.IsoPath.IsEmpty() && !HasAcceptedTerms())
	{
		return Fail(TEXT("The Epic Games Terms of Service have not been accepted"));
	}
	if (!InOptions.IsoPath.IsEmpty() && !IFileManager::Get().FileExists(*InOptions.IsoPath))
	{
		return Fail(FString::Printf(TEXT("%s does not exist"), *InOptions.IsoPath));
	}
	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
		delete Runnable;
		Runnable = nullptr;
	}
	FOptions Options = InOptions;
	Options.InstallDir = FPaths::ConvertRelativePathToFull(Options.InstallDir.IsEmpty() ? GetDefaultInstallDir() : Options.InstallDir);
	Options.DownloadDir = FPaths::ConvertRelativePathToFull(Options.DownloadDir.IsEmpty() ? GetDefaultDownloadDir() : Options.DownloadDir);
	if (!Options.IsoPath.IsEmpty())
	{
		Options.IsoPath = FPaths::ConvertRelativePathToFull(Options.IsoPath);
	}
	// the HTTP module must be loaded on the game thread
	FModuleManager::Get().LoadModuleChecked(TEXT("HTTP"));

	Shared = MakeShared<FShared, ESPMode::ThreadSafe>();
	Shared->StartTime = FPlatformTime::Seconds();
	Shared->bRunning = true;
	Shared->Progress.bRunning = true;
	Shared->Progress.Stage = EArenaDownloadStage::Prepare;
	Shared->Progress.StageText = TEXT("Preparing");
	Runnable = new ArenaContentDownloadLocal::FLambdaRunnable([this, Options]() { Run(Options); });
	Thread = FRunnableThread::Create(Runnable, TEXT("ArenaContentDownload"), 0, TPri_BelowNormal);
	if (!Thread)
	{
		Shared->bRunning = false;
		return Fail(TEXT("Cannot start the download thread"));
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("Download: started (install to %s, %s)"), *Options.InstallDir,
		Options.IsoPath.IsEmpty() ? *(TEXT("download to ") + Options.DownloadDir) : *(TEXT("local image ") + Options.IsoPath));
	return true;
}

void FArenaContentDownload::Run(FOptions Options)
{
	using namespace ArenaContentDownloadLocal;
	TSharedPtr<FShared, ESPMode::ThreadSafe> S = Shared;
	IFileManager& FM = IFileManager::Get();
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

	auto Finish = [&S](EArenaDownloadStage Stage, const FString& Error)
	{
		S->Update([&](FArenaContentDownloadProgress& P)
		{
			P.Stage = Stage;
			P.bRunning = false;
			P.bSucceeded = Stage == EArenaDownloadStage::Done;
			P.bCancelled = Stage == EArenaDownloadStage::Cancelled;
			P.Error = Error;
			P.StageText = P.bSucceeded ? TEXT("Finished") : P.bCancelled ? TEXT("Cancelled") : TEXT("Failed");
		});
		if (Stage == EArenaDownloadStage::Failed)
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("Download: failed: %s"), *Error);
		}
		else
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("Download: %s"), Stage == EArenaDownloadStage::Done ? TEXT("finished") : TEXT("cancelled (the partial download is kept)"));
		}
		S->bRunning = false;
	};
	auto Cancelled = [&S]() { return bool(S->bCancel); };

	// ---- 1. disk space ----
	const bool bDownload = Options.IsoPath.IsEmpty();
	const FString IsoPath = bDownload ? Options.DownloadDir / TEXT("UT2004.ISO") : Options.IsoPath;
	const int64 InstallSize = GetInstallSize(Options.bFullInstall);
	{
		int64 IsoLeft = 0;
		if (bDownload && !FM.FileExists(*IsoPath))
		{
			IsoLeft = FMath::Max<int64>(0, PrimaryIsoSize - FMath::Max<int64>(0, FM.FileSize(*(IsoPath + TEXT(".part")))));
		}
		uint64 DlTotal = 0, InTotal = 0;
		const int64 DlFree = FreeSpace(Options.DownloadDir, &DlTotal);
		const int64 InFree = Options.bDownloadOnly ? -1 : FreeSpace(Options.InstallDir, &InTotal);
		const bool bSameDisk = DlFree >= 0 && DlFree == InFree && DlTotal == InTotal;
		const int64 Margin = 256ll << 20;
		if (DlFree >= 0 && DlFree < IsoLeft + Margin + (bSameDisk ? InstallSize : 0))
		{
			Finish(EArenaDownloadStage::Failed, FString::Printf(TEXT("Not enough disk space in %s: %s free, %s needed"), *Options.DownloadDir,
				*FormatBytes(DlFree), *FormatBytes(IsoLeft + Margin + (bSameDisk ? InstallSize : 0))));
			return;
		}
		if (!bSameDisk && InFree >= 0 && InFree < InstallSize + Margin)
		{
			Finish(EArenaDownloadStage::Failed, FString::Printf(TEXT("Not enough disk space in %s: %s free, %s needed"), *Options.InstallDir,
				*FormatBytes(InFree), *FormatBytes(InstallSize + Margin)));
			return;
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("Download: disk space ok (download left %s, install %s, free %s)"), *FormatBytes(IsoLeft), *FormatBytes(InstallSize), *FormatBytes(DlFree));
	}

	// ---- 2. download + verify ----
	auto OnProgress = [&S](const FArenaHttpDownload::FProgress& In)
	{
		S->Update([&](FArenaContentDownloadProgress& P)
		{
			const bool bVerify = FCString::Strcmp(In.Phase, TEXT("verify")) == 0;
			P.Stage = bVerify ? EArenaDownloadStage::Verify : EArenaDownloadStage::Download;
			P.StageText = bVerify ? TEXT("Verifying UT2004.ISO") : TEXT("Downloading UT2004.ISO");
			P.BytesDone = In.Done;
			P.BytesTotal = In.Total;
			P.Fraction = In.Total > 0 ? float(double(In.Done) / double(In.Total)) : 0.f;
			P.BytesPerSecond = bVerify ? 0.0 : In.BytesPerSecond;
			P.EtaSeconds = (!bVerify && In.BytesPerSecond > 1.0) ? double(In.Total - In.Done) / In.BytesPerSecond : 0.0;
			if (!bVerify)
			{
				FString Host = In.Url;
				Host.RemoveFromStart(TEXT("https://"));
				Host.Split(TEXT("/"), &Host, nullptr);
				P.Item = Host;
			}
		});
	};
	if (bDownload)
	{
		TArray<FArenaDlSource> Sources;
		for (const TCHAR* Url : PrimaryMirrors)
		{
			Sources.Add({ Url, PrimaryIsoSize, PrimaryIsoSha256 });
		}
		// spread the load over the mirrors like the installer does (shuf), the backup last
		for (int32 I = Sources.Num() - 1; I > 0; --I)
		{
			Sources.Swap(I, FMath::RandRange(0, I));
		}
		Sources.Add(BackupSource);
		FString Error;
		const bool bOk = FArenaHttpDownload::Download(Sources, IsoPath, S->bCancel, OnProgress,
			[&S](const FString& Msg) { S->Update([&](FArenaContentDownloadProgress& P) { P.Item = Msg; }); }, Error);
		if (!bOk)
		{
			Finish(Cancelled() ? EArenaDownloadStage::Cancelled : EArenaDownloadStage::Failed, Error);
			return;
		}
	}
	else
	{
		// a local image: verify it when it is one of the known images, else just try it
		const int64 Size = FM.FileSize(*IsoPath);
		const FArenaDlSource* Known = Size == PrimaryIsoSize ? nullptr : (Size == BackupSource.Size ? &BackupSource : nullptr);
		const FArenaDlSource Primary{ IsoPath, PrimaryIsoSize, PrimaryIsoSha256 };
		if (Size == PrimaryIsoSize)
		{
			Known = &Primary;
		}
		if (Known)
		{
			FString Error;
			if (!FArenaHttpDownload::Verify(IsoPath, *Known, S->bCancel, OnProgress, Error))
			{
				Finish(Cancelled() ? EArenaDownloadStage::Cancelled : EArenaDownloadStage::Failed, Error);
				return;
			}
			UE_LOG(LogOpenUT2k4, Log, TEXT("Download: %s verified (SHA-256 %s)"), *IsoPath, *Known->Sha256);
		}
		else
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("Download: %s is not a known UT2004 image (size %lld); trying it anyway"), *IsoPath, Size);
		}
	}
	if (Options.bDownloadOnly)
	{
		S->Update([&](FArenaContentDownloadProgress& P) { P.InstallDir = FString(); });
		Finish(EArenaDownloadStage::Done, FString());
		return;
	}

	// ---- 3. open the image and the cabinets ----
	S->Update([](FArenaContentDownloadProgress& P)
	{
		P.Stage = EArenaDownloadStage::Open;
		P.StageText = TEXT("Reading the disc image");
		P.Fraction = 0.f;
		P.BytesPerSecond = P.EtaSeconds = 0.0;
	});
	FArenaIsoImage Iso;
	if (!Iso.Open(IsoPath))
	{
		Finish(EArenaDownloadStage::Failed, Iso.GetError());
		return;
	}
	// cabinet volumes live in Disk1..DiskN of the image: index them by file name
	TMap<FString, FString> VolumePaths;
	for (const FArenaIsoImage::FEntry& E : Iso.GetEntries())
	{
		const FString Name = FPaths::GetCleanFilename(E.Path).ToLower();
		if (!E.bDirectory && E.Path.StartsWith(TEXT("Disk"), ESearchCase::IgnoreCase) && (Name.EndsWith(TEXT(".cab")) || Name.EndsWith(TEXT(".hdr"))))
		{
			VolumePaths.Add(Name, E.Path);
		}
	}
	FArenaInstallShieldCab Cab;
	const bool bCabOk = Cab.Open(TEXT("data1.cab"), [&Iso, &VolumePaths](const FString& FileName) -> TUniquePtr<IArenaDlReader>
	{
		const FString* Path = VolumePaths.Find(FileName.ToLower());
		return Path ? Iso.OpenFile(*Path) : nullptr;
	});
	if (!bCabOk)
	{
		Finish(EArenaDownloadStage::Failed, FString::Printf(TEXT("%s does not contain the UT2004 installer cabinets: %s"), *IsoPath, *Cab.GetError()));
		return;
	}

	// ---- 4. the file list ----
	struct FItem
	{
		int32 Index;
		FString Rel;
		uint64 Size;
	};
	TArray<FItem> Items;
	uint64 TotalBytes = 0;
	TSet<FString> Seen;
	for (const FGroupTarget& T : GroupTargets)
	{
		if (T.bOptional && !Options.bFullInstall)
		{
			continue;
		}
		const FArenaInstallShieldCab::FFileGroup* G = Cab.FindFileGroup(T.Group);
		if (!G || G->FirstFile < 0)
		{
			if (!T.bOptional && FCString::Strcmp(T.Group, TEXT("US_License.int")) != 0)
			{
				Finish(EArenaDownloadStage::Failed, FString::Printf(TEXT("The image has no %s file group"), T.Group));
				return;
			}
			continue;
		}
		for (int32 I = G->FirstFile; I <= G->LastFile && I < Cab.NumFiles(); ++I)
		{
			if (!Cab.IsFileValid(I))
			{
				continue;
			}
			const FString Dir = Cab.GetFileDirectory(I);
			const FString Name = Cab.GetFileName(I);
			if (IsIgnored(T.Group, Dir, Name))
			{
				continue;
			}
			FString Rel = FString(T.Target);
			if (!Dir.IsEmpty())
			{
				Rel = Rel.IsEmpty() ? Dir : Rel / Dir;
			}
			Rel = Rel.IsEmpty() ? Name : Rel / Name;
			if (!IsSafeRelative(Rel))
			{
				UE_LOG(LogOpenUT2k4, Warning, TEXT("Download: skipping unsafe path %s"), *Rel);
				continue;
			}
			// later groups overwrite earlier ones in the installer (cp -rf): keep the last copy
			if (Seen.Contains(Rel.ToLower()))
			{
				Items.RemoveAll([&Rel](const FItem& It) { return It.Rel.Equals(Rel, ESearchCase::IgnoreCase); });
			}
			Seen.Add(Rel.ToLower());
			Items.Add({ I, Rel, Cab.GetFileSize(I) });
		}
	}
	for (const FItem& It : Items)
	{
		TotalBytes += It.Size;
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("Download: InstallShield %d cabinets, %d file groups; installing %d files (%s) into %s"), Cab.GetMajorVersion(),
		Cab.GetFileGroups().Num(), Items.Num(), *FormatBytes(double(TotalBytes)), *Options.InstallDir);

	// ---- 5. extract ----
	// build in <dir>.partial and rename when complete; files already there (an interrupted install) are kept when
	// their size matches, since each file is written to .tmp and renamed only when complete and checked (MD5)
	const bool bTargetExists = FM.DirectoryExists(*Options.InstallDir);
	const FString WorkDir = bTargetExists ? Options.InstallDir : Options.InstallDir + TEXT(".partial");
	FM.MakeDirectory(*WorkDir, true);
	const double InstallStart = FPlatformTime::Seconds();
	uint64 DoneBytes = 0;
	int32 DoneFiles = 0;
	for (const FItem& It : Items)
	{
		if (Cancelled())
		{
			Finish(EArenaDownloadStage::Cancelled, TEXT("Cancelled"));
			return;
		}
		const FString Dest = WorkDir / It.Rel;
		S->Update([&](FArenaContentDownloadProgress& P)
		{
			P.Stage = EArenaDownloadStage::Install;
			P.StageText = TEXT("Installing UT2004 files");
			P.Item = It.Rel;
			P.FilesDone = DoneFiles;
			P.FilesTotal = Items.Num();
			P.BytesDone = int64(DoneBytes);
			P.BytesTotal = int64(TotalBytes);
			P.Fraction = TotalBytes ? float(double(DoneBytes) / double(TotalBytes)) : 0.f;
			const double Dt = FPlatformTime::Seconds() - InstallStart;
			P.BytesPerSecond = Dt > 1.0 ? double(DoneBytes) / Dt : 0.0;
			P.EtaSeconds = P.BytesPerSecond > 1.0 ? double(TotalBytes - DoneBytes) / P.BytesPerSecond : 0.0;
		});
		if (!bTargetExists && FM.FileExists(*Dest) && uint64(FM.FileSize(*Dest)) == It.Size)
		{
			DoneBytes += It.Size;
			++DoneFiles;
			continue;
		}
		FM.MakeDirectory(*FPaths::GetPath(Dest), true);
		const FString Tmp = Dest + TEXT(".tmp");
		FString Error;
		bool bOk = false;
		{
			TUniquePtr<IFileHandle> Out(PF.OpenWrite(*Tmp, false, false));
			if (!Out)
			{
				Error = FString::Printf(TEXT("Cannot write %s"), *Tmp);
			}
			else
			{
				bool bWriteFailed = false;
				bOk = Cab.ExtractFile(It.Index, [&](const uint8* Data, int64 Len)
				{
					if (!Out->Write(Data, Len))
					{
						bWriteFailed = true;
						return false;
					}
					DoneBytes += uint64(Len);
					return !Cancelled();
				}, Error);
				if (bWriteFailed)
				{
					Error = FString::Printf(TEXT("Writing %s failed (disk full?)"), *Tmp);
				}
				bOk = bOk && Out->Flush();
			}
		}
		if (!bOk)
		{
			FM.Delete(*Tmp, false, true, true);
			Finish(Cancelled() ? EArenaDownloadStage::Cancelled : EArenaDownloadStage::Failed,
				Cancelled() ? TEXT("Cancelled") : FString::Printf(TEXT("%s: %s"), *It.Rel, *Error));
			return;
		}
		if (!FM.Move(*Dest, *Tmp, true, true))
		{
			Finish(EArenaDownloadStage::Failed, FString::Printf(TEXT("Cannot rename %s"), *Tmp));
			return;
		}
		++DoneFiles;
	}

	// ---- 6. finish: record where it came from, publish the folder, drop the image ----
	{
		TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
		Info->SetStringField(TEXT("source"), bDownload ? TEXT("OldUnreal UT2004.ISO") : FPaths::GetCleanFilename(IsoPath));
		Info->SetNumberField(TEXT("image_size"), double(FM.FileSize(*IsoPath)));
		Info->SetNumberField(TEXT("files"), Items.Num());
		Info->SetNumberField(TEXT("bytes"), double(TotalBytes));
		Info->SetBoolField(TEXT("full_install"), Options.bFullInstall);
		Info->SetStringField(TEXT("installed_utc"), FDateTime::UtcNow().ToIso8601());
		Info->SetStringField(TEXT("terms_of_service"), GetTermsUrl());
		FString Json;
		FJsonSerializer::Serialize(Info, TJsonWriterFactory<>::Create(&Json));
		FFileHelper::SaveStringToFile(Json, *(WorkDir / TEXT("OpenUT2k4-download.json")));
	}
	if (WorkDir != Options.InstallDir && !FM.Move(*Options.InstallDir, *WorkDir, false, true))
	{
		Finish(EArenaDownloadStage::Failed, FString::Printf(TEXT("Cannot rename %s to %s"), *WorkDir, *Options.InstallDir));
		return;
	}
	// the image has no CD key: make one, as the OldUnreal client would on its first start
	UE2NetCDKey::EnsureInstallKey(Options.InstallDir);
	if (bDownload && !Options.bKeepIso)
	{
		FM.Delete(*IsoPath, false, true, true);
		FM.DeleteDirectory(*Options.DownloadDir, false, false); // only if empty
		UE_LOG(LogOpenUT2k4, Log, TEXT("Download: deleted %s"), *IsoPath);
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("Download: installed %d files (%s) into %s in %.1fs"), DoneFiles, *FormatBytes(double(TotalBytes)), *Options.InstallDir,
		FPlatformTime::Seconds() - InstallStart);
	S->Update([&](FArenaContentDownloadProgress& P)
	{
		P.InstallDir = Options.InstallDir;
		P.FilesDone = DoneFiles;
		P.BytesDone = int64(TotalBytes);
		P.Fraction = 1.f;
		P.Item.Reset();
	});
	Finish(EArenaDownloadStage::Done, FString());
}

// ============================================================================================================
// Setup screen block (AArenaContentSetupHUD extension) and the headless test hook
// ============================================================================================================
namespace ArenaContentDownloadLocal
{
	const FLinearColor White = ArenaSetupStyle::White();
	const FLinearColor Dim = ArenaSetupStyle::Dim();
	const FLinearColor Accent = ArenaSetupStyle::Accent();
	const FLinearColor Hot = ArenaSetupStyle::Hot();
	const FLinearColor Good = ArenaSetupStyle::Good();
	const FLinearColor Bad = ArenaSetupStyle::Bad();

	/** Game-thread UI state of the setup screen block. */
	struct FScreenState
	{
		/** The Terms of Service notice is shown and waits for "I agree". */
		bool bShowTerms = false;
		/** A download was started from the screen (its result is shown, and on success the conversion starts). */
		bool bStartedHere = false;
		bool bHandedOver = false;
		TWeakObjectPtr<AArenaContentSetupHUD> Screen;
		FString LastStartError;
		/** FindUT2004Install, refreshed every 2 s (it scans folders). */
		bool bHasInstall = false;
		double InstallCheckedAt = -100.0;

		bool HasInstall()
		{
			const double Now = FPlatformTime::Seconds();
			if (Now - InstallCheckedAt > 2.0)
			{
				InstallCheckedAt = Now;
				bHasInstall = !ArenaContent::FindUT2004Install().IsEmpty();
			}
			return bHasInstall;
		}
	};
	FScreenState GScreen;

	void StartFromScreen()
	{
		FArenaContentDownload::FOptions Opt;
		FString Error;
		GScreen.bStartedHere = FArenaContentDownload::Get().Start(Opt, &Error);
		GScreen.bHandedOver = false;
		GScreen.LastStartError = GScreen.bStartedHere ? FString() : Error;
	}

	/** The block in the setup screen's text column (see FArenaContentSetupExtension::DrawInfo). */
	void DrawBlock(const FArenaDraw& D, float X, float& Y, float Width)
	{
		auto Para = [&](const FString& Text, const FLinearColor& Color, float Size = 19.f, float Gap = 10.f)
		{
			Y = AArenaContentSetupHUD::DrawParagraph(D, Text, X, Y, Width, Color, Size, Gap);
		};
		FArenaContentDownload& Dl = FArenaContentDownload::Get();
		const FArenaContentDownloadProgress P = Dl.GetProgress();
		if (Dl.IsRunning())
		{
			Para(TEXT("Downloading UT2004 from OldUnreal"), White, 30.f, 12.f);
			Para(TEXT("When the download is installed, the game converts it right away."), Dim, 17.f, 18.f);
			Para(P.StageText, Accent, 20.f, 8.f);
			const float BarH = D.S(22.f);
			AArenaContentSetupHUD::DrawProgressBar(D, X, Y, Width, BarH, P.Fraction);
			Y += BarH + D.S(10.f);
			FString Stats = FString::Printf(TEXT("%d%%   -   %s of %s"), FMath::RoundToInt(P.Fraction * 100.f), *FormatBytes(double(P.BytesDone)), *FormatBytes(double(P.BytesTotal)));
			if (P.Stage == EArenaDownloadStage::Download && P.BytesPerSecond > 0.0)
			{
				Stats += FString::Printf(TEXT("   -   %s/s, about %s left"), *FormatBytes(P.BytesPerSecond), *FormatTime(P.EtaSeconds));
			}
			if (P.Stage == EArenaDownloadStage::Install)
			{
				Stats += FString::Printf(TEXT("   -   %d of %d files"), P.FilesDone, P.FilesTotal);
			}
			Para(Stats, White, 18.f, 8.f);
			if (!P.Item.IsEmpty())
			{
				Para(P.Item, Dim, 15.f, 10.f);
			}
			Para(TEXT("You can cancel at any time: the download continues where it stopped next time."), Dim, 16.f, 10.f);
			return;
		}
		if (GScreen.bShowTerms)
		{
			Para(TEXT("Terms of Service"), Hot, 30.f, 12.f);
			Para(TEXT("The Epic Games Terms of Service apply to the use and distribution of this game, and they supersede any other end user agreements that may accompany the game."),
				White, 19.f, 10.f);
			Para(TEXT("You may read the Terms of Service at this URL:"), White, 19.f, 4.f);
			Para(FArenaContentDownload::GetTermsUrl(), Accent, 19.f, 16.f);
			Para(FString::Printf(TEXT("The game data (%s) is downloaded from OldUnreal (files.oldunreal.net) and installed to:"),
				*FormatBytes(double(FArenaContentDownload::GetDownloadSize()))), Dim, 17.f, 4.f);
			Para(FArenaContentDownload::GetDefaultInstallDir(), White, 17.f, 8.f);
			Para(FString::Printf(TEXT("It needs about %s of free disk space while installing (%s afterwards)."),
				*FormatBytes(double(FArenaContentDownload::GetRequiredDiskSpace())), *FormatBytes(double(FArenaContentDownload::GetInstallSize()))), Dim, 17.f, 12.f);
			Para(TEXT("Choose \"I agree - download\" to accept the Terms of Service and start, or \"Do not agree\" to go back."), White, 17.f, 10.f);
			return;
		}
		if (GScreen.bStartedHere && P.Stage == EArenaDownloadStage::Done)
		{
			Para(TEXT("UT2004 was downloaded and installed."), Good, 19.f, 4.f);
			Para(P.InstallDir, Dim, 15.f, 14.f);
			return;
		}
		if (GScreen.bStartedHere && (P.Stage == EArenaDownloadStage::Failed || P.Stage == EArenaDownloadStage::Cancelled))
		{
			if (P.Stage == EArenaDownloadStage::Failed)
			{
				Para(TEXT("The download did not finish."), Bad, 20.f, 4.f);
				Para(P.Error, White, 16.f, 6.f);
				Para(TEXT("Check your internet connection and free disk space, then choose Resume download."), Dim, 16.f, 14.f);
			}
			else
			{
				Para(TEXT("Download paused. Resume download continues where it stopped."), Hot, 18.f, 14.f);
			}
			return;
		}
		if (!GScreen.LastStartError.IsEmpty())
		{
			Para(GScreen.LastStartError, Bad, 17.f, 12.f);
		}
		if (!GScreen.HasInstall())
		{
			// separator from the screen's own text
			D.Rect(X, Y, Width, FMath::Max(1.f, D.S(1.f)), FLinearColor(Accent.R, Accent.G, Accent.B, 0.35f));
			Y += D.S(14.f);
			Para(TEXT("No copy of UT2004?"), White, 21.f, 4.f);
			Para(FString::Printf(TEXT("OldUnreal hosts the full game for free. The download is %s and needs about %s of disk space once installed."),
				*FormatBytes(double(FArenaContentDownload::GetDownloadSize())), *FormatBytes(double(FArenaContentDownload::GetInstallSize()))), Dim, 17.f, 14.f);
		}
	}

	struct FRegister
	{
		FRegister()
		{
			FArenaContentSetupExtension Main;
			Main.Id = FName(TEXT("OldUnrealDownload"));
			Main.GetLabel = []() -> FString
			{
				if (FArenaContentDownload::Get().IsRunning())
				{
					return TEXT("Cancel download");
				}
				if (GScreen.bShowTerms)
				{
					return TEXT("I agree - download");
				}
				const FArenaContentDownloadProgress P = FArenaContentDownload::Get().GetProgress();
				if (GScreen.bStartedHere && P.Stage == EArenaDownloadStage::Done)
				{
					return FString();
				}
				if (GScreen.bStartedHere && (P.Stage == EArenaDownloadStage::Failed || P.Stage == EArenaDownloadStage::Cancelled))
				{
					return TEXT("Resume download");
				}
				return GScreen.HasInstall() ? FString() : FString(TEXT("Download UT2004 from OldUnreal"));
			};
			Main.OnActivate = [](AArenaContentSetupHUD* Screen)
			{
				GScreen.Screen = Screen;
				if (FArenaContentDownload::Get().IsRunning())
				{
					FArenaContentDownload::Get().Cancel();
				}
				else if (GScreen.bShowTerms)
				{
					// explicit agreement: the second activation
					GScreen.bShowTerms = false;
					FArenaContentDownload::AcceptTerms();
					StartFromScreen();
				}
				else
				{
					// the first activation only shows the notice
					GScreen.bShowTerms = true;
					GScreen.LastStartError.Reset();
				}
			};
			Main.DrawInfo = [](const FArenaDraw& D, float X, float& Y, float Width) { DrawBlock(D, X, Y, Width); };
			Main.IsBusy = []() { return FArenaContentDownload::Get().IsRunning(); };
			// the notice and the download own the screen (only their buttons and Back)
			Main.IsModal = []() { return GScreen.bShowTerms || FArenaContentDownload::Get().IsRunning(); };
			Main.GetModalStatus = []() -> FString
			{
				if (FArenaContentDownload::Get().IsRunning())
				{
					return FString::Printf(TEXT("DOWNLOADING  %d%%"), FMath::RoundToInt(FArenaContentDownload::Get().GetProgress().Fraction * 100.f));
				}
				return TEXT("TERMS OF SERVICE");
			};
			Main.Group = FName(TEXT("OldUnreal"));
			AArenaContentSetupHUD::RegisterExtension(Main);

			FArenaContentSetupExtension Decline;
			Decline.Id = FName(TEXT("OldUnrealDecline"));
			Decline.GetLabel = []() -> FString { return GScreen.bShowTerms ? FString(TEXT("Do not agree")) : FString(); };
			Decline.OnActivate = [](AArenaContentSetupHUD*) { GScreen.bShowTerms = false; };
			Decline.Group = FName(TEXT("OldUnreal"));
			AArenaContentSetupHUD::RegisterExtension(Decline);

			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FRegister::Tick), 0.25f);
		}

		bool bTestStarted = false;
		double NextLog = 0.0;

		bool Tick(float)
		{
			FArenaContentDownload& Dl = FArenaContentDownload::Get();
			// hand a finished download to the setup screen, which converts it
			if (GScreen.bStartedHere && !GScreen.bHandedOver && !Dl.IsRunning())
			{
				const FArenaContentDownloadProgress P = Dl.GetProgress();
				if (P.Stage == EArenaDownloadStage::Done && !P.InstallDir.IsEmpty())
				{
					GScreen.bHandedOver = true;
					GScreen.InstallCheckedAt = -100.0;
					if (AArenaContentSetupHUD* Screen = GScreen.Screen.Get())
					{
						Screen->ContinueWithInstall(P.InstallDir);
					}
					else
					{
						UArenaUserSettings* Settings = UArenaUserSettings::Get();
						Settings->UT2004Dir = P.InstallDir;
						Settings->Save();
					}
				}
			}
#if !UE_BUILD_SHIPPING
			TickTestHook(Dl);
#endif
			return true;
		}

#if !UE_BUILD_SHIPPING
		void TickTestHook(FArenaContentDownload& Dl)
		{
			const TCHAR* Cmd = FCommandLine::Get();
			// screenshots of the setup screen block: -DownloadShowTerms opens the Terms of Service notice
			if (!bTestStarted && FParse::Param(Cmd, TEXT("DownloadShowTerms")))
			{
				bTestStarted = true;
				GScreen.bShowTerms = true;
			}
			if (!FParse::Param(Cmd, TEXT("DownloadUT2004")))
			{
				return;
			}
			if (!bTestStarted)
			{
				bTestStarted = true;
				FArenaContentDownload::FOptions Opt;
				FParse::Value(Cmd, TEXT("DownloadTo="), Opt.InstallDir);
				FParse::Value(Cmd, TEXT("DownloadDir="), Opt.DownloadDir);
				FParse::Value(Cmd, TEXT("ISO="), Opt.IsoPath);
				Opt.bKeepIso = FParse::Param(Cmd, TEXT("KeepISO"));
				Opt.bFullInstall = FParse::Param(Cmd, TEXT("FullInstall"));
				Opt.bDownloadOnly = FParse::Param(Cmd, TEXT("DownloadOnly"));
				UE_LOG(LogOpenUT2k4, Log, TEXT("DownloadUT2004: %s You may read the Terms of Service at this URL: %s"), *FArenaContentDownload::GetTermsText(),
					FArenaContentDownload::GetTermsUrl());
				if (Opt.IsoPath.IsEmpty() && !FArenaContentDownload::HasAcceptedTerms())
				{
					if (!FParse::Param(Cmd, TEXT("AcceptEpicTerms")))
					{
						UE_LOG(LogOpenUT2k4, Error, TEXT("DownloadUT2004: add -AcceptEpicTerms to agree to the Terms of Service"));
						FPlatformMisc::RequestExit(false, TEXT("DownloadUT2004"));
						return;
					}
					FArenaContentDownload::AcceptTerms();
				}
				if (!Dl.Start(Opt))
				{
					FPlatformMisc::RequestExit(false, TEXT("DownloadUT2004"));
				}
				return;
			}
			const FArenaContentDownloadProgress P = Dl.GetProgress();
			const double Now = FPlatformTime::Seconds();
			if (Dl.IsRunning())
			{
				if (Now >= NextLog)
				{
					NextLog = Now + 5.0;
					UE_LOG(LogOpenUT2k4, Log, TEXT("DownloadUT2004: %s %.1f%% %lld/%lld %.1f MB/s eta %.0fs files %d/%d elapsed %.0fs  %s"), *P.StageText, P.Fraction * 100.f,
						P.BytesDone, P.BytesTotal, P.BytesPerSecond / 1e6, P.EtaSeconds, P.FilesDone, P.FilesTotal, P.ElapsedSeconds, *P.Item);
				}
				return;
			}
			if (NextLog >= 0.0)
			{
				NextLog = -1.0;
				UE_LOG(LogOpenUT2k4, Log, TEXT("DownloadUT2004: result %s in %.1fs%s%s"), P.bSucceeded ? TEXT("SUCCESS") : P.bCancelled ? TEXT("CANCELLED") : TEXT("FAILED"),
					P.ElapsedSeconds, P.Error.IsEmpty() ? TEXT("") : TEXT(": "), *P.Error);
				if (P.bSucceeded && !P.InstallDir.IsEmpty())
				{
					UE_LOG(LogOpenUT2k4, Log, TEXT("DownloadUT2004: IsUT2004Install(%s) = %d"), *P.InstallDir, ArenaContent::IsUT2004Install(P.InstallDir) ? 1 : 0);
				}
				if (!FParse::Param(Cmd, TEXT("DownloadNoQuit")))
				{
					FPlatformMisc::RequestExit(false, TEXT("DownloadUT2004"));
				}
			}
		}
#endif
	};
	static FRegister GRegister;
}
