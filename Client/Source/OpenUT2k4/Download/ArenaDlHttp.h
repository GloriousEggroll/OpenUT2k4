#pragma once

#include "CoreMinimal.h"

/** One download location of a file with its expected size and SHA-256 (lower-case hex). */
struct FArenaDlSource
{
	FString Url;
	int64 Size = 0;
	FString Sha256;
};

/**
 * Resumable, verified download of one large file with the engine HTTP module (libcurl on Linux, WinHttp on Windows,
 * NSURLSession on Mac), for the UT2004 disc image.
 *
 *  - Streams to <Dest>.part on disk in HTTP Range requests of ChunkSize bytes; nothing is kept in memory.
 *  - Resumes an existing .part (after a cancel, a crash or a restart). <Dest>.part.json remembers which file
 *    (size + SHA-256) the part belongs to, so a part of a different file is never continued.
 *  - Mirrors are tried in the given order (callers shuffle equal mirrors); after repeated errors the next one is
 *    used, and the download continues from the same position, since all mirrors of a group serve the same file.
 *    A server that ignores Range is handled (its leading bytes are skipped).
 *  - When complete, the size and SHA-256 are checked and the part is renamed to Dest; a bad file is deleted and
 *    the next source is tried.
 *
 * Blocking: call from a worker thread (never the game thread). Cancel through the flag.
 */
class FArenaHttpDownload
{
public:
	struct FProgress
	{
		/** "download" or "verify" */
		const TCHAR* Phase = TEXT("download");
		int64 Done = 0;
		int64 Total = 0;
		/** Average over the last few seconds (download phase). */
		double BytesPerSecond = 0.0;
		FString Url;
	};

	/** Bytes per HTTP request. */
	static constexpr int64 ChunkSize = 256ll << 20;

	/**
	 * Downloads one of Sources to DestPath. OnProgress is called about 4 times a second, OnMessage for notable events
	 * (mirror switch, retry). Returns false with OutError on failure or cancel (the .part is kept for resuming,
	 * unless it was found to be bad).
	 */
	static bool Download(const TArray<FArenaDlSource>& Sources, const FString& DestPath, const TAtomic<bool>& bCancel,
		TFunction<void(const FProgress&)> OnProgress, TFunction<void(const FString&)> OnMessage, FString& OutError);

	/** Checks Path against Source's size and SHA-256 (hashing reports "verify" progress). */
	static bool Verify(const FString& Path, const FArenaDlSource& Source, const TAtomic<bool>& bCancel,
		TFunction<void(const FProgress&)> OnProgress, FString& OutError);
};
