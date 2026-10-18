#pragma once

#include "CoreMinimal.h"

class FUE2NetConnection;

/**
 * UT2004 package downloads while joining a server (Docs/UE2Net.md "Downloads").
 *
 * After WELCOME, the packages the player lacks (USES entries without a local file of the same GUID) are downloaded
 * before JOIN, in the order of the server's download managers (DLMGR):
 *   - IpDrv.HTTPDownload: GET <redirect URL>/<file name>.uz2 (or the plain file without COMPRESSION=1);
 *   - Engine.ChannelDownload: a file channel on the game connection (FUE2NetConnection::OpenFileChannel), also used
 *     when the redirect fails.
 * Every file is checked (an Unreal package, the USES GUID, the USES size when not compressed, size caps) and stored in
 * the per-user net cache, indexed by GUID:
 *   <net cache>/scripts/<GUID>/<Package>.u           code packages: decoded by the UnrealScript loader, never run
 *   <net cache>/packages/<GUID>/<Package>.<ext>      maps (.ut2) and asset packages (.utx .usx .ukx .uax)
 *   <net cache>/index.json                           GUID -> name, file, size, generations, last use
 * The package map then finds them by GUID (FindCachedPackage). Maps and asset packages are converted into the content
 * cache by the custom content converter (ArenaConvUserContent::Run with its state in <content cache>/netcache), so a
 * downloaded map loads like any converted map.
 *
 * Safety: only package files are downloaded (.u .ut2 .utx .usx .ukx .uax; UT2004 never sends native code and neither
 * are .so / .dll files accepted), server-provided names are checked (no paths, no "..", a fixed character set, the
 * file name must be the package name), files are written only below the net cache, and sizes are capped
 * (MaxPackageBytes; a redirect copy is judged by its GUID, the file channel's by the USES size too).
 *
 * Game thread only (the conversion runs on a worker thread).
 */
namespace UE2NetDownload
{
	/**
	 * Largest package file accepted (compressed or not): a sanity cap only. UT2004 has no limit, and maps of 30 to
	 * 150 MB are common.
	 */
	constexpr int64 MaxPackageBytes = 1ll << 30;

	/** GetUserDataDir()/NetCache, or -UE2NetCache=<dir>. */
	OPENUT2K4_API FString GetNetCacheDir();

	/** The downloaded file of a package GUID (empty when not in the net cache, or its header has another GUID). */
	OPENUT2K4_API FString FindCachedPackage(const FGuid& Guid);

	/**
	 * Names (lower case) of UT2004's own packages, never downloaded (a server's other version of one means the install
	 * is missing or another version). From the files UT2004's installer installed: the install's System/Manifest.ini (and
	 * any other System ini manifest of an Epic installer: a [Setup] section with MasterProduct=UT2004), whose File=
	 * lines list the retail, Bonus Pack and ECE files. Mods the player put into the UT2004 folder are not in it. Without a
	 * manifest (no install, or none found): the install's package folders and what the content cache was converted from,
	 * without custom content and downloads (their converter states).
	 * OutSource (optional) says which rule was used.
	 */
	OPENUT2K4_API TSet<FString> GetStockPackageNames(FString* OutSource = nullptr);

	/** Package names (lower case) of the File= lines of an installer manifest (Manifest.ini format); empty if none. */
	OPENUT2K4_API TSet<FString> ReadManifestPackages(const FString& ManifestPath);

	/**
	 * A non-blocking notice about the current join (stock packages missing / another version), for the connecting
	 * screen and the spectator bar; empty when there is nothing to say. Cleared by ClearJoinNotice (a new join).
	 */
	OPENUT2K4_API const FString& GetJoinNotice();
	OPENUT2K4_API void ClearJoinNotice();

	/**
	 * Seconds a whole download pass may take before the join is cancelled (default 1800, -UE2NetDownloadTimeLimit=).
	 * Many servers cap the file channel at 10 to 20 KB/s and some redirects hold stale versions, so a 25 MB map can
	 * take 20 minutes, as it would in UT2004.
	 */
	OPENUT2K4_API double GetDownloadTimeLimit();

	/** True when a server-provided file name may be stored: "<PackageName>.<allowed extension>", no paths. */
	OPENUT2K4_API bool IsSafePackageFileName(const FString& FileName, const FString& PackageName);

	/**
	 * Unpacks a .uz2 file (UT2004 "ucc compress"): a sequence of chunks, each an int32 compressed size, an int32
	 * uncompressed size (at most 32768 in files from ucc) and a zlib stream. Fails above MaxOut bytes.
	 */
	OPENUT2K4_API bool DecompressUz2(const uint8* Data, int64 Size, int64 MaxOut, TArray<uint8>& Out, FString& OutError);
}

/** The download and conversion pass of one connection, between WELCOME and JOIN. */
class FUE2NetDownloader
{
public:
	explicit FUE2NetDownloader(FUE2NetConnection& InConnection);
	~FUE2NetDownloader();

	/**
	 * Call after the package map has matched the local packages. Queues the downloadable missing packages, and a
	 * conversion when the server's map or asset packages come from the net cache. False when there is nothing to do.
	 */
	bool Start();
	void Tick(double Now);
	bool IsDone() const { return Phase == EPhase::Done; }
	/** "Downloading R3tex.utx (2 of 3): 1.2 / 4.0 MB (HTTP)" / "Converting DM-Foo..." */
	FString DescribeStatus() const;
	/** 0..1 over the whole pass. */
	float GetProgress() const;

	/** Packages stored this pass, and those that could not be downloaded (they are SKIPped). */
	TArray<FString> Downloaded;
	TArray<FString> Failed;
	/** Stock packages the server uses that the player lacks or has in another version (never downloaded; SKIPped). */
	TArray<FString> StockMissing;
	/** Maps converted by this pass (from the converter). */
	TArray<FString> ConvertedMaps;

private:
	/** Aborted: the join was cancelled (connection closed); never reports IsDone, so the join does not go on. */
	enum class EPhase : uint8 { Idle, Downloading, Converting, Done, Aborted };
	enum class EMethod : uint8 { None, Http, Channel };

	struct FItem
	{
		FString Name;
		FString FileName;
		FGuid Guid;
		int64 Size = 0;
		int32 Generation = 0;
		bool bTriedHttp = false;
		bool bTriedChannel = false;
	};

	void StartNext(double Now);
	bool StartHttp(FItem& Item);
	bool StartChannel(FItem& Item);
	void OnFileData(int32 Channel, const TArray<uint8>& Data, bool bClose);
	/** Checks and stores the received bytes; false (with the reason logged) when the file is not acceptable. */
	bool Finish(FItem& Item, TArray<uint8>&& Bytes, bool bCompressed, FString& OutError);
	void FailCurrent(const FString& Reason, double Now);
	void StartConvert();
	/** Cancels the join (closes the connection with Reason, shown as the join's error). */
	void Abort(const FString& Reason);

	FUE2NetConnection& Connection;
	EPhase Phase = EPhase::Idle;
	TArray<FItem> Queue;
	int32 Current = INDEX_NONE;
	EMethod Method = EMethod::None;
	/** The DLMGR entries: HTTP redirect URL (empty: none), its COMPRESSION, and whether the channel is offered. */
	FString RedirectURL;
	bool bRedirectCompressed = true;
	bool bChannelAllowed = false;
	bool bChannelCompressed = false;

	struct FHttpState;
	TSharedPtr<FHttpState, ESPMode::ThreadSafe> Http;
	int32 FileChannel = 0;
	TArray<uint8> ChannelBytes;
	bool bChannelDone = false;
	bool bRaisedNetSpeed = false;
	int64 BytesDone = 0;
	int64 BytesTotal = 0;
	int64 CurrentBytes = 0;
	double ItemStart = 0.0;
	double LastData = 0.0;
	/** The current request got any answer (data or a refusal). */
	bool bGotReply = false;
	/** File requests in a row the server did not answer at all. */
	int32 NoReplyInRow = 0;
	double PassStart = 0.0;
	/** Seconds spent receiving file data and the bytes received (download rate estimate). */
	/** File channel only (the redirect is fast and has no projection). */
	double ReceiveSeconds = 0.0;
	int64 ReceivedBytes = 0;
	double LastTick = 0.0;

	struct FConvertJob;
	TSharedPtr<FConvertJob, ESPMode::ThreadSafe> Convert;
};
