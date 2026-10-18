#pragma once

#include "CoreMinimal.h"

class IArenaDlReader;

/**
 * InstallShield cabinet reader (data1.hdr + data1.cab ... dataN.cab, InstallShield 5 and 6 - 2xxx formats).
 *
 * A C++ port of the parts of unshield (https://github.com/twogood/unshield, MIT licence, (c) 2003 David Eriksson,
 * see Download/ThirdParty/unshield-LICENSE.txt) that list file groups and extract files: header / file
 * descriptor parsing, multi-volume (split) files, obfuscation, the chunked raw-deflate compression and the MD5
 * check. Not ported: the "old" compression (-O), files stored outside the cabinet, component listing.
 * Volumes are read through a callback, so the cabinets can be read straight out of a disc image.
 */
class FArenaInstallShieldCab
{
public:
	/** Opens a volume or header by file name ("data1.hdr", "data3.cab"); null if it does not exist. */
	using FOpenVolume = TFunction<TUniquePtr<IArenaDlReader>(const FString& FileName)>;

	struct FFileGroup
	{
		FString Name;
		int32 FirstFile = -1;
		int32 LastFile = -1;
	};

	FArenaInstallShieldCab();
	~FArenaInstallShieldCab();

	/**
	 * Reads the header of the cabinet set whose first file is FirstCabName ("data1.cab"): <prefix>1.hdr when it
	 * exists, else the header inside <prefix>1.cab. False (and GetError()) on failure.
	 */
	bool Open(const FString& FirstCabName, FOpenVolume InOpenVolume);

	const FString& GetError() const { return Error; }
	int32 GetMajorVersion() const;
	const TArray<FFileGroup>& GetFileGroups() const { return FileGroups; }
	const FFileGroup* FindFileGroup(const FString& Name) const;

	int32 NumFiles() const;
	/** False for invalid / empty entries (unshield_file_is_valid); those are skipped when extracting. */
	bool IsFileValid(int32 Index);
	FString GetFileName(int32 Index);
	/** The file's directory inside its group, with '/' separators ("" for the group root). */
	FString GetFileDirectory(int32 Index);
	uint64 GetFileSize(int32 Index);

	/**
	 * Decompresses file Index and passes the data to Sink in pieces of up to 64 KB (Sink returns false to stop).
	 * Checks the expanded size and, for InstallShield 6+, the MD5. False (and OutError) on failure.
	 */
	bool ExtractFile(int32 Index, TFunctionRef<bool(const uint8*, int64)> Sink, FString& OutError);

private:
	struct FHeader;
	struct FFileDescriptor;
	struct FReader;

	FFileDescriptor* GetDescriptor(int32 Index);
	FString GetString(const uint8* Ptr) const;
	FString VolumeName(int32 Volume, const TCHAR* Suffix) const;

	TUniquePtr<FHeader> Header;
	TArray<FFileGroup> FileGroups;
	FOpenVolume OpenVolume;
	FString Prefix;
	FString Error;
};
