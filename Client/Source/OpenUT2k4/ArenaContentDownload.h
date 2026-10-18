#pragma once

#include "CoreMinimal.h"

/**
 * "Download UT2004 from OldUnreal": gets the UT2004 game data for players who do not have an install, the same way
 * OldUnreal's official installers do (https://github.com/OldUnreal/FullGameInstallers, Linux/install-ut2004.sh),
 * but without the OldUnreal patch or any executables, since only the game data is converted.
 *
 *  1. The Epic Games Terms of Service apply to the game; the player must agree first (GetTermsText / GetTermsUrl,
 *     AcceptTerms stores the agreement in UArenaUserSettings::AcceptedEpicTermsUtc). Start() refuses otherwise.
 *  2. Downloads UT2004.ISO (about 2.8 GB) from the OldUnreal mirrors (shuffled), with archive.org as backup:
 *     streamed to disk, resumable after a cancel or restart, size + SHA-256 checked (Download/ArenaDlHttp).
 *  3. Reads the InstallShield cabinets (Disk1..5/data*.cab, data1.hdr) straight out of the ISO
 *     (Download/ArenaDlIso, Download/ArenaDlCab, a port of unshield) and writes the file groups the converter
 *     needs: Animations, Maps, Music, StaticMeshes, Textures, Sounds, Speech, System (no .exe/.dll/.bat), Help
 *     (FOptions::bFullInstall adds the rest of what OldUnreal installs: Benchmark, ForceFeedback, KarmaData, Web,
 *     Manual). About 6.1 GB.
 *  4. Installs into GetDefaultInstallDir() (= ArenaContent::GetUserDataDir()/UT2004, one of the places
 *     ArenaContent::FindUT2004Install searches), building it in <dir>.partial and renaming it when complete, so a
 *     half-finished install is never picked up. Deletes the ISO unless bKeepIso (a user-supplied IsoPath is
 *     never deleted).
 *
 * The setup screen (AArenaContentSetupHUD) shows this as an extension block registered in ArenaContentDownload.cpp:
 * button "Download UT2004 from OldUnreal" -> Terms of Service notice + "I agree" -> progress / cancel -> on
 * success AArenaContentSetupHUD::ContinueWithInstall(InstallDir) starts the conversion.
 *
 * Test hook (non-shipping builds): -DownloadUT2004 [-AcceptEpicTerms] [-DownloadTo=<install dir>]
 * [-DownloadDir=<folder for UT2004.ISO>] [-ISO=<local UT2004.ISO, skips the download>] [-KeepISO] [-FullInstall]
 * [-DownloadOnly] [-DownloadNoQuit]: runs headless at startup, logs progress and quits when finished.
 */

enum class EArenaDownloadStage : uint8
{
	Idle,
	/** Checking free disk space. */
	Prepare,
	Download,
	/** SHA-256 of the image. */
	Verify,
	/** Reading the disc image and the cabinet headers. */
	Open,
	/** Extracting the files. */
	Install,
	Done,
	Failed,
	Cancelled,
};

struct FArenaContentDownloadProgress
{
	EArenaDownloadStage Stage = EArenaDownloadStage::Idle;
	/** Short human readable stage ("Downloading UT2004.ISO", "Installing files"...). */
	FString StageText;
	/** Current item (mirror host, file being written) or the latest notable event. */
	FString Item;
	/** Progress of the current stage (bytes for Download / Verify / Install). */
	int64 BytesDone = 0;
	int64 BytesTotal = 0;
	/** Download speed (Download stage) and estimated time left for the stage, 0 when unknown. */
	double BytesPerSecond = 0.0;
	double EtaSeconds = 0.0;
	/** 0..1 for the current stage. */
	float Fraction = 0.f;
	int32 FilesDone = 0;
	int32 FilesTotal = 0;
	double ElapsedSeconds = 0.0;
	bool bRunning = false;
	bool bSucceeded = false;
	bool bCancelled = false;
	/** Why it failed (Stage == Failed). */
	FString Error;
	/** The install folder (valid when bSucceeded). */
	FString InstallDir;
};

class OPENUT2K4_API FArenaContentDownload
{
public:
	struct FOptions
	{
		/** Where UT2004 is installed; empty = GetDefaultInstallDir(). */
		FString InstallDir;
		/** Use this local UT2004.ISO instead of downloading (never deleted). */
		FString IsoPath;
		/** Folder for the downloaded UT2004.ISO (and its .part while downloading); empty = GetDefaultDownloadDir(). */
		FString DownloadDir;
		/** Keep the downloaded ISO after installing. */
		bool bKeepIso = false;
		/** Also install the groups the converter does not need (Benchmark, ForceFeedback, KarmaData, Web, Manual). */
		bool bFullInstall = false;
		/** Stop after the verified download (tests). */
		bool bDownloadOnly = false;
	};

	static FArenaContentDownload& Get();

	/** Starts on a background thread. False (and OutError) if already running, terms not accepted or bad options. Game thread. */
	bool Start(const FOptions& Options, FString* OutError = nullptr);
	/** Requests a stop; the partial download is kept and resumed by the next Start. */
	void Cancel();
	bool IsRunning() const;
	/** Thread-safe snapshot. */
	FArenaContentDownloadProgress GetProgress() const;

	// ---- Epic Games Terms of Service ----
	static const TCHAR* GetTermsUrl();
	/** The notice shown before downloading (same wording as OldUnreal's installer), without the URL. */
	static FString GetTermsText();
	static bool HasAcceptedTerms();
	/** Records the player's agreement (UArenaUserSettings::AcceptedEpicTermsUtc) and saves. */
	static void AcceptTerms();

	// ---- locations and sizes ----
	static FString GetDefaultInstallDir();
	static FString GetDefaultDownloadDir();
	/** Size of the disc image download (bytes). */
	static int64 GetDownloadSize();
	/** Approximate size of the installed files (bytes). */
	static int64 GetInstallSize(bool bFullInstall = false);
	/** Free space needed for a fresh download + install (image and install exist at the same time). */
	static int64 GetRequiredDiskSpace(bool bFullInstall = false);

	~FArenaContentDownload();

private:
	FArenaContentDownload() = default;
	void Run(FOptions Options);

	struct FShared;
	TSharedPtr<FShared, ESPMode::ThreadSafe> Shared;
	class FRunnableThread* Thread = nullptr;
	class FRunnable* Runnable = nullptr;
};
