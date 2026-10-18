#pragma once

#include "CoreMinimal.h"

/**
 * Incremental SHA-256 (FIPS 180-4), used to verify the downloaded UT2004 disc image against the checksum
 * published by OldUnreal. The engine only offers a one-shot, platform dependent SHA-256
 * (FPlatformMisc::GetSHA256Signature), which cannot hash a 3 GB file in pieces.
 */
class FArenaSha256
{
public:
	FArenaSha256() { Reset(); }

	void Reset();
	void Update(const uint8* Data, int64 Size);
	/** Finishes the hash; the object must be Reset() before it is used again. */
	void Final(uint8 OutDigest[32]);
	/** Finishes the hash and returns it as 64 lower-case hex digits. */
	FString FinalHex();

	/**
	 * Hashes a file in 4 MB pieces. OnProgress(BytesDone, BytesTotal) is called after every piece and may return
	 * false to cancel. Returns an empty string on read errors or cancel.
	 */
	static FString HashFile(const FString& Path, TFunctionRef<bool(int64, int64)> OnProgress);

private:
	void Block(const uint8* Data);

	uint32 State[8];
	uint8 Buffer[64];
	uint64 TotalBytes = 0;
	int32 Buffered = 0;
};
