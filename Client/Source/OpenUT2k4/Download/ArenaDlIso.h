#pragma once

#include "CoreMinimal.h"

class IFileHandle;

/**
 * Random access to a byte range: a plain file, or one file inside a disc image (FArenaIsoImage::OpenFile).
 * Not thread safe; each reader owns its own file handle.
 */
class IArenaDlReader
{
public:
	virtual ~IArenaDlReader() = default;
	virtual int64 Size() const = 0;
	/** Reads Count bytes at Offset (relative to the start of this range). False on errors or reads past the end. */
	virtual bool Read(int64 Offset, void* Dest, int64 Count) = 0;

	/** Reader over [Base, Base + Length) of the file at Path (Length < 0 = to the end of the file). Null if it cannot be opened. */
	static TUniquePtr<IArenaDlReader> OpenFile(const FString& Path, int64 Base = 0, int64 Length = -1);
};

/**
 * Minimal ISO 9660 reader (with the Joliet extension for long / mixed-case names) that reads files straight out of
 * a disc image without extracting it. Enough for the UT2004 DVD image OldUnreal hosts: plain ISO 9660 + Joliet,
 * 2048-byte sectors, no UDF-only content, no Rock Ridge. Multi-extent files (over 4 GB) are not supported;
 * interleaved files are rejected.
 */
class FArenaIsoImage
{
public:
	struct FEntry
	{
		/** Path inside the image with '/' separators and no ";1" version suffix, e.g. "Disk1/data1.cab". */
		FString Path;
		int64 Offset = 0;
		int64 Size = 0;
		bool bDirectory = false;
	};

	/** Reads the volume descriptors and the whole directory tree. False (and Error set) if Path is not a usable image. */
	bool Open(const FString& Path);

	const FString& GetError() const { return Error; }
	const FString& GetPath() const { return ImagePath; }
	/** Every file and directory of the image. */
	const TArray<FEntry>& GetEntries() const { return Entries; }
	/** Case-insensitive lookup of a path ("Disk1/data1.hdr"). */
	const FEntry* Find(const FString& Path) const;
	/** Reader for one file of the image; null if it does not exist. */
	TUniquePtr<IArenaDlReader> OpenFile(const FString& Path) const;
	/** True if the image's names came from the Joliet tree. */
	bool UsesJoliet() const { return bJoliet; }

private:
	bool ReadDirectory(IFileHandle& File, int64 Extent, int64 Length, const FString& Prefix, int32 Depth);

	FString ImagePath;
	FString Error;
	TArray<FEntry> Entries;
	TMap<FString, int32> Index;
	bool bJoliet = false;
};
