// Port of unshield's lib/libunshield.c, file.c, file_group.c, directory.c and helper.c (MIT licence,
// (c) 2003 David Eriksson, see Download/ThirdParty/unshield-LICENSE.txt). Structure offsets and the decoding
// steps follow the original; memory handling, bounds checks and I/O are rewritten for the engine.
#include "ArenaDlCab.h"
#include "ArenaDlIso.h"
#include "Misc/SecureHash.h"

THIRD_PARTY_INCLUDES_START
#include "zlib.h"
THIRD_PARTY_INCLUDES_END

namespace ArenaDlCabLocal
{
	constexpr uint32 CabSignature = 0x28635349; // "ISc("
	constexpr int32 CommonHeaderSize = 20;
	constexpr int32 VolumeHeaderSizeV5 = 40;
	constexpr int32 VolumeHeaderSizeV6 = 64;
	constexpr int32 MaxFileGroupCount = 71;
	constexpr int32 MaxComponentCount = 71;
	constexpr int32 BufferSize = 64 * 1024;

	constexpr uint16 FileSplit = 1;
	constexpr uint16 FileObfuscated = 2;
	constexpr uint16 FileCompressed = 4;
	constexpr uint16 FileInvalid = 8;
	constexpr uint8 LinkPrev = 1;

	FORCEINLINE uint16 Le16(const uint8* P) { return uint16(P[0] | (P[1] << 8)); }
	FORCEINLINE uint32 Le32(const uint8* P) { return uint32(P[0]) | (uint32(P[1]) << 8) | (uint32(P[2]) << 16) | (uint32(P[3]) << 24); }
	FORCEINLINE uint64 Le64(const uint8* P) { return uint64(Le32(P)) | (uint64(Le32(P + 4)) << 32); }

	struct FCommonHeader
	{
		uint32 Signature = 0;
		uint32 Version = 0;
		uint32 VolumeInfo = 0;
		uint32 CabDescriptorOffset = 0;
		uint32 CabDescriptorSize = 0;

		bool Parse(const uint8* P)
		{
			Signature = Le32(P);
			Version = Le32(P + 4);
			VolumeInfo = Le32(P + 8);
			CabDescriptorOffset = Le32(P + 12);
			CabDescriptorSize = Le32(P + 16);
			return Signature == CabSignature;
		}
	};

	struct FVolumeHeader
	{
		uint64 DataOffset = 0;
		uint32 FirstFileIndex = 0;
		uint32 LastFileIndex = 0;
		uint64 FirstFileOffset = 0;
		uint64 FirstFileSizeExpanded = 0;
		uint64 FirstFileSizeCompressed = 0;
		uint64 LastFileOffset = 0;
		uint64 LastFileSizeExpanded = 0;
		uint64 LastFileSizeCompressed = 0;
	};

	/** InstallShield's byte obfuscation (unshield_deobfuscate); Seed is 0 at the start of each file. */
	void Deobfuscate(uint8* Buffer, int64 Size, uint32& Seed)
	{
		uint32 S = Seed;
		for (int64 I = 0; I < Size; ++I, ++S)
		{
			const uint8 X = uint8(Buffer[I] ^ 0xd5);
			const uint8 R = uint8((X >> 2) | (X << 6));
			Buffer[I] = uint8(R - uint8(S % 0x47));
		}
		Seed = S;
	}

	/** One compressed chunk: raw deflate (no zlib header), Source has a spare trailing zero byte like unshield adds. */
	int32 Uncompress(uint8* Dest, uLong& DestLen, uint8* Source, uLong SourceLen)
	{
		z_stream Stream;
		FMemory::Memzero(Stream);
		Stream.next_in = Source;
		Stream.avail_in = uInt(SourceLen);
		Stream.next_out = Dest;
		Stream.avail_out = uInt(DestLen);
		int32 Err = inflateInit2(&Stream, -MAX_WBITS);
		if (Err != Z_OK)
		{
			return Err;
		}
		Err = inflate(&Stream, Z_FINISH);
		if (Err != Z_STREAM_END)
		{
			inflateEnd(&Stream);
			return Err == Z_OK ? Z_BUF_ERROR : Err;
		}
		DestLen = Stream.total_out;
		return inflateEnd(&Stream);
	}
}

using namespace ArenaDlCabLocal;

struct FArenaInstallShieldCab::FFileDescriptor
{
	uint32 NameOffset = 0;
	uint32 DirectoryIndex = 0;
	uint16 Flags = 0;
	uint64 ExpandedSize = 0;
	uint64 CompressedSize = 0;
	uint64 DataOffset = 0;
	uint8 Md5[16] = {};
	int32 Volume = 0;
	uint32 LinkPrevious = 0;
	uint32 LinkNext = 0;
	uint8 LinkFlags = 0;
};

struct FArenaInstallShieldCab::FHeader
{
	TArray<uint8> Data;
	int32 MajorVersion = 0;
	FCommonHeader Common;
	uint32 FileTableOffset = 0;
	uint32 DirectoryCount = 0;
	uint32 FileCount = 0;
	uint32 FileTableOffset2 = 0;
	TArray<uint32> FileTable;
	TArray<TUniquePtr<FFileDescriptor>> Descriptors;

	/** Pointer to Need bytes at Offset into the header data, or null when out of range. */
	const uint8* At(uint64 Offset, uint64 Need) const
	{
		return (Offset + Need <= uint64(Data.Num())) ? Data.GetData() + Offset : nullptr;
	}
	/** Cab descriptor relative pointer (unshield_header_get_buffer); null for offset 0. */
	const uint8* Desc(uint32 Offset, uint64 Need) const
	{
		return Offset ? At(uint64(Common.CabDescriptorOffset) + Offset, Need) : nullptr;
	}
};

/** Sequential reader of one file's stored bytes across volumes (unshield's UnshieldReader). */
struct FArenaInstallShieldCab::FReader
{
	FArenaInstallShieldCab& Cab;
	int32 Index;
	FFileDescriptor& Fd;
	int32 Volume = 0;
	TUniquePtr<IArenaDlReader> VolumeFile;
	int64 VolumePos = 0;
	FVolumeHeader VolumeHeader;
	uint64 VolumeBytesLeft = 0;
	uint32 ObfuscationOffset = 0;

	FReader(FArenaInstallShieldCab& InCab, int32 InIndex, FFileDescriptor& InFd) : Cab(InCab), Index(InIndex), Fd(InFd) {}

	bool OpenVolume(int32 NewVolume, FString& OutError)
	{
		VolumeFile = Cab.OpenVolume(Cab.VolumeName(NewVolume, TEXT("cab")));
		if (!VolumeFile)
		{
			OutError = FString::Printf(TEXT("Cannot open cabinet volume %s"), *Cab.VolumeName(NewVolume, TEXT("cab")));
			return false;
		}
		uint8 Tmp[VolumeHeaderSizeV6];
		FCommonHeader Common;
		if (!VolumeFile->Read(0, Tmp, CommonHeaderSize) || !Common.Parse(Tmp))
		{
			OutError = FString::Printf(TEXT("%s is not an InstallShield cabinet"), *Cab.VolumeName(NewVolume, TEXT("cab")));
			return false;
		}
		VolumeHeader = FVolumeHeader();
		const int32 Major = Cab.Header->MajorVersion;
		if (Major <= 5)
		{
			if (!VolumeFile->Read(CommonHeaderSize, Tmp, VolumeHeaderSizeV5))
			{
				OutError = TEXT("Cannot read a cabinet volume header");
				return false;
			}
			const uint8* P = Tmp;
			VolumeHeader.DataOffset = Le32(P);
			VolumeHeader.FirstFileIndex = Le32(P + 8);
			VolumeHeader.LastFileIndex = Le32(P + 12);
			VolumeHeader.FirstFileOffset = Le32(P + 16);
			VolumeHeader.FirstFileSizeExpanded = Le32(P + 20);
			VolumeHeader.FirstFileSizeCompressed = Le32(P + 24);
			VolumeHeader.LastFileOffset = Le32(P + 28);
			VolumeHeader.LastFileSizeExpanded = Le32(P + 32);
			VolumeHeader.LastFileSizeCompressed = Le32(P + 36);
			if (VolumeHeader.LastFileOffset == 0)
			{
				VolumeHeader.LastFileOffset = MAX_int32;
			}
		}
		else
		{
			if (!VolumeFile->Read(CommonHeaderSize, Tmp, VolumeHeaderSizeV6))
			{
				OutError = TEXT("Cannot read a cabinet volume header");
				return false;
			}
			const uint8* P = Tmp;
			auto Wide = [P](int32 Off) { return uint64(Le32(P + Off)) | (uint64(Le32(P + Off + 4)) << 32); };
			VolumeHeader.DataOffset = Wide(0);
			VolumeHeader.FirstFileIndex = Le32(P + 8);
			VolumeHeader.LastFileIndex = Le32(P + 12);
			VolumeHeader.FirstFileOffset = Wide(16);
			VolumeHeader.FirstFileSizeExpanded = Wide(24);
			VolumeHeader.FirstFileSizeCompressed = Wide(32);
			VolumeHeader.LastFileOffset = Wide(40);
			VolumeHeader.LastFileSizeExpanded = Wide(48);
			VolumeHeader.LastFileSizeCompressed = Wide(56);
		}

		// InstallShield 5 does not flag split files: detect them from the volume header
		if (Major == 5)
		{
			if (uint32(Index) < Cab.Header->FileCount - 1 && uint32(Index) == VolumeHeader.LastFileIndex
				&& VolumeHeader.LastFileSizeCompressed != Fd.CompressedSize)
			{
				Fd.Flags |= FileSplit;
			}
			else if (Index > 0 && uint32(Index) == VolumeHeader.FirstFileIndex && VolumeHeader.FirstFileSizeCompressed != Fd.CompressedSize)
			{
				Fd.Flags |= FileSplit;
			}
		}

		uint64 DataOffset = 0, LeftExpanded = 0, LeftCompressed = 0;
		if (Fd.Flags & FileSplit)
		{
			if (uint32(Index) == VolumeHeader.LastFileIndex && VolumeHeader.LastFileOffset != 0x7FFFFFFF)
			{
				DataOffset = VolumeHeader.LastFileOffset;
				LeftExpanded = VolumeHeader.LastFileSizeExpanded;
				LeftCompressed = VolumeHeader.LastFileSizeCompressed;
			}
			else if (uint32(Index) == VolumeHeader.FirstFileIndex)
			{
				DataOffset = VolumeHeader.FirstFileOffset;
				LeftExpanded = VolumeHeader.FirstFileSizeExpanded;
				LeftCompressed = VolumeHeader.FirstFileSizeCompressed;
			}
			else
			{
				Volume = NewVolume;
				VolumeBytesLeft = 0;
				return true;
			}
		}
		else
		{
			DataOffset = Fd.DataOffset;
			LeftExpanded = Fd.ExpandedSize;
			LeftCompressed = Fd.CompressedSize;
		}
		VolumeBytesLeft = (Fd.Flags & FileCompressed) ? LeftCompressed : LeftExpanded;
		VolumePos = int64(DataOffset);
		Volume = NewVolume;
		return true;
	}

	bool Read(uint8* Buffer, int64 Size, FString& OutError)
	{
		uint8* P = Buffer;
		int64 BytesLeft = Size;
		for (;;)
		{
			const int64 ToRead = FMath::Min<int64>(BytesLeft, int64(VolumeBytesLeft));
			if (ToRead == 0)
			{
				OutError = FString::Printf(TEXT("Unexpected end of data in cabinet volume %d"), Volume);
				return false;
			}
			if (!VolumeFile->Read(VolumePos, P, ToRead))
			{
				OutError = FString::Printf(TEXT("Cannot read %lld bytes from cabinet volume %d"), ToRead, Volume);
				return false;
			}
			VolumePos += ToRead;
			BytesLeft -= ToRead;
			VolumeBytesLeft -= uint64(ToRead);
			if (BytesLeft == 0)
			{
				break;
			}
			P += ToRead;
			// the rest of the file continues in the next volume
			if (!OpenVolume(Volume + 1, OutError))
			{
				return false;
			}
		}
		if (Fd.Flags & FileObfuscated)
		{
			Deobfuscate(Buffer, Size, ObfuscationOffset);
		}
		return true;
	}
};

FArenaInstallShieldCab::FArenaInstallShieldCab() = default;
FArenaInstallShieldCab::~FArenaInstallShieldCab() = default;

FString FArenaInstallShieldCab::VolumeName(int32 Volume, const TCHAR* Suffix) const
{
	return FString::Printf(TEXT("%s%d.%s"), *Prefix, Volume, Suffix);
}

FString FArenaInstallShieldCab::GetString(const uint8* Ptr) const
{
	if (!Ptr)
	{
		return FString();
	}
	const uint8* End = Header->Data.GetData() + Header->Data.Num();
	FString Out;
	if (Header->MajorVersion >= 17)
	{
		// UTF-16 little endian
		TArray<UTF16CHAR> Chars;
		for (const uint8* P = Ptr; P + 1 < End; P += 2)
		{
			const UTF16CHAR C = UTF16CHAR(Le16(P));
			if (C == 0)
			{
				break;
			}
			Chars.Add(C);
		}
		Out = FString(Chars.Num(), Chars.GetData());
	}
	else
	{
		// 8-bit names (ASCII in practice; extended characters kept as Latin-1)
		for (const uint8* P = Ptr; P < End && *P; ++P)
		{
			Out.AppendChar(TCHAR(*P));
		}
	}
	return Out;
}

bool FArenaInstallShieldCab::Open(const FString& FirstCabName, FOpenVolume InOpenVolume)
{
	OpenVolume = MoveTemp(InOpenVolume);
	FileGroups.Reset();
	Header.Reset();
	Error.Reset();

	// "data1.cab" -> "data": the name up to the first '.' or digit (unshield_create_filename_pattern)
	Prefix = FirstCabName;
	for (int32 I = 0; I < Prefix.Len(); ++I)
	{
		if (Prefix[I] == TEXT('.') || FChar::IsDigit(Prefix[I]))
		{
			Prefix.LeftInline(I);
			break;
		}
	}

	TUniquePtr<IArenaDlReader> File = OpenVolume(VolumeName(1, TEXT("hdr")));
	if (!File)
	{
		File = OpenVolume(VolumeName(1, TEXT("cab")));
	}
	if (!File)
	{
		Error = FString::Printf(TEXT("Cannot find %s or %s"), *VolumeName(1, TEXT("hdr")), *VolumeName(1, TEXT("cab")));
		return false;
	}
	TUniquePtr<FHeader> H = MakeUnique<FHeader>();
	// a .cab's header is at its start; cap the read for cabinets without a separate .hdr
	const int64 HeaderSize = FMath::Min<int64>(File->Size(), 256 << 20);
	if (HeaderSize < CommonHeaderSize)
	{
		Error = TEXT("Cabinet header too small");
		return false;
	}
	H->Data.SetNumUninitialized(HeaderSize);
	if (!File->Read(0, H->Data.GetData(), HeaderSize) || !H->Common.Parse(H->Data.GetData()))
	{
		Error = TEXT("Invalid InstallShield cabinet header");
		return false;
	}
	const uint32 V = H->Common.Version;
	if (V >> 24 == 1)
	{
		H->MajorVersion = (V >> 12) & 0xf;
	}
	else if (V >> 24 == 2 || V >> 24 == 4)
	{
		H->MajorVersion = int32(V & 0xffff);
		if (H->MajorVersion != 0)
		{
			H->MajorVersion /= 100;
		}
	}
	if (!H->Common.CabDescriptorSize)
	{
		Error = TEXT("The cabinet has no descriptor");
		return false;
	}
	const uint8* Cd = H->At(H->Common.CabDescriptorOffset, 0x3e + 4 * (MaxFileGroupCount + MaxComponentCount));
	if (!Cd)
	{
		Error = TEXT("Truncated cabinet descriptor");
		return false;
	}
	H->FileTableOffset = Le32(Cd + 0x0c);
	H->DirectoryCount = Le32(Cd + 0x1c);
	H->FileCount = Le32(Cd + 0x28);
	H->FileTableOffset2 = Le32(Cd + 0x2c);
	const uint32 Count = H->DirectoryCount + H->FileCount;
	const uint8* Ft = H->At(uint64(H->Common.CabDescriptorOffset) + H->FileTableOffset, uint64(Count) * 4);
	if (!Ft || Count > 10000000)
	{
		Error = TEXT("Truncated cabinet file table");
		return false;
	}
	H->FileTable.SetNumUninitialized(Count);
	for (uint32 I = 0; I < Count; ++I)
	{
		H->FileTable[I] = Le32(Ft + I * 4);
	}
	H->Descriptors.SetNum(H->FileCount);
	Header = MoveTemp(H);

	// file groups: MaxFileGroupCount hash buckets of linked lists
	for (int32 I = 0; I < MaxFileGroupCount; ++I)
	{
		uint32 Next = Le32(Cd + 0x3e + I * 4);
		int32 Guard = 0;
		while (Next && ++Guard < 10000)
		{
			const uint8* L = Header->Desc(Next, 12);
			if (!L)
			{
				break;
			}
			const uint32 DescriptorOffset = Le32(L + 4);
			Next = Le32(L + 8);
			const int32 Skip = Header->MajorVersion <= 5 ? 0x48 : 0x12;
			const uint8* G = Header->Desc(DescriptorOffset, 4 + Skip + 8);
			if (!G)
			{
				continue;
			}
			FFileGroup Group;
			Group.Name = GetString(Header->Desc(Le32(G), 1));
			Group.FirstFile = int32(Le32(G + 4 + Skip));
			Group.LastFile = int32(Le32(G + 8 + Skip));
			FileGroups.Add(Group);
		}
	}
	return true;
}

int32 FArenaInstallShieldCab::GetMajorVersion() const
{
	return Header ? Header->MajorVersion : 0;
}

const FArenaInstallShieldCab::FFileGroup* FArenaInstallShieldCab::FindFileGroup(const FString& Name) const
{
	return FileGroups.FindByPredicate([&Name](const FFileGroup& G) { return G.Name == Name; });
}

int32 FArenaInstallShieldCab::NumFiles() const
{
	return Header ? int32(Header->FileCount) : 0;
}

FArenaInstallShieldCab::FFileDescriptor* FArenaInstallShieldCab::GetDescriptor(int32 Index)
{
	if (!Header || Index < 0 || uint32(Index) >= Header->FileCount)
	{
		return nullptr;
	}
	if (Header->Descriptors[Index])
	{
		return Header->Descriptors[Index].Get();
	}
	TUniquePtr<FFileDescriptor> Fd = MakeUnique<FFileDescriptor>();
	const uint64 TableBase = uint64(Header->Common.CabDescriptorOffset) + Header->FileTableOffset;
	if (Header->MajorVersion <= 5)
	{
		const uint8* P = Header->At(TableBase + Header->FileTable[Header->DirectoryCount + Index], 0x3a);
		if (!P)
		{
			return nullptr;
		}
		Fd->Volume = 1;
		Fd->NameOffset = Le32(P);
		Fd->DirectoryIndex = Le16(P + 4);
		Fd->Flags = Le16(P + 8);
		Fd->ExpandedSize = Le32(P + 10);
		Fd->CompressedSize = Le32(P + 14);
		Fd->DataOffset = Le32(P + 38);
		if (Header->MajorVersion == 5)
		{
			FMemory::Memcpy(Fd->Md5, P + 42, 16);
		}
	}
	else
	{
		const uint8* P = Header->At(TableBase + Header->FileTableOffset2 + uint64(Index) * 0x57, 0x57);
		if (!P)
		{
			return nullptr;
		}
		Fd->Flags = Le16(P);
		Fd->ExpandedSize = Le64(P + 2);
		Fd->CompressedSize = Le64(P + 10);
		Fd->DataOffset = Le64(P + 18);
		FMemory::Memcpy(Fd->Md5, P + 26, 16);
		Fd->NameOffset = Le32(P + 58);
		Fd->DirectoryIndex = Le16(P + 62);
		Fd->LinkPrevious = Le32(P + 76);
		Fd->LinkNext = Le32(P + 80);
		Fd->LinkFlags = P[84];
		Fd->Volume = Le16(P + 85);
	}
	if (Fd->DirectoryIndex >= Header->DirectoryCount)
	{
		Fd->Flags |= FileInvalid;
	}
	Header->Descriptors[Index] = MoveTemp(Fd);
	return Header->Descriptors[Index].Get();
}

bool FArenaInstallShieldCab::IsFileValid(int32 Index)
{
	const FFileDescriptor* Fd = GetDescriptor(Index);
	return Fd && !(Fd->Flags & FileInvalid) && Fd->NameOffset && Fd->DataOffset;
}

FString FArenaInstallShieldCab::GetFileName(int32 Index)
{
	const FFileDescriptor* Fd = GetDescriptor(Index);
	if (!Fd)
	{
		return FString();
	}
	return GetString(Header->At(uint64(Header->Common.CabDescriptorOffset) + Header->FileTableOffset + Fd->NameOffset, 1));
}

FString FArenaInstallShieldCab::GetFileDirectory(int32 Index)
{
	const FFileDescriptor* Fd = GetDescriptor(Index);
	if (!Fd || Fd->DirectoryIndex >= Header->DirectoryCount)
	{
		return FString();
	}
	FString Dir = GetString(Header->At(uint64(Header->Common.CabDescriptorOffset) + Header->FileTableOffset + Header->FileTable[Fd->DirectoryIndex], 1));
	Dir.ReplaceInline(TEXT("\\"), TEXT("/"));
	return Dir;
}

uint64 FArenaInstallShieldCab::GetFileSize(int32 Index)
{
	const FFileDescriptor* Fd = GetDescriptor(Index);
	return Fd ? Fd->ExpandedSize : 0;
}

bool FArenaInstallShieldCab::ExtractFile(int32 Index, TFunctionRef<bool(const uint8*, int64)> Sink, FString& OutError)
{
	FFileDescriptor* Fd = GetDescriptor(Index);
	if (!Fd || (Fd->Flags & FileInvalid) || Fd->DataOffset == 0)
	{
		OutError = FString::Printf(TEXT("File %d is not valid"), Index);
		return false;
	}
	// identical files are stored once; later copies link to the stored one
	int32 Guard = 0;
	while ((Fd->LinkFlags & LinkPrev) && ++Guard < 64)
	{
		Index = int32(Fd->LinkPrevious);
		Fd = GetDescriptor(Index);
		if (!Fd || (Fd->Flags & FileInvalid) || Fd->DataOffset == 0)
		{
			OutError = FString::Printf(TEXT("Linked file %d is not valid"), Index);
			return false;
		}
	}

	FReader Reader(*this, Index, *Fd);
	for (;;)
	{
		if (!Reader.OpenVolume(Fd->Volume, OutError))
		{
			return false;
		}
		// InstallShield 5: the descriptor's volume may be too early; move on until the file is inside it
		if (Header->MajorVersion <= 5 && uint32(Index) > Reader.VolumeHeader.LastFileIndex && Fd->Volume < 1000)
		{
			++Fd->Volume;
			continue;
		}
		break;
	}
	if (Reader.VolumeFile->Size() == int64(Fd->DataOffset))
	{
		OutError = FString::Printf(TEXT("File %d is not inside the cabinet"), Index);
		return false;
	}

	TArray<uint8> In, Out;
	In.SetNumUninitialized(BufferSize + 1);
	Out.SetNumUninitialized(BufferSize);
	FMD5 Md5;
	uint64 BytesLeft = (Fd->Flags & FileCompressed) ? Fd->CompressedSize : Fd->ExpandedSize;
	uint64 Written = 0;
	while (BytesLeft > 0)
	{
		uLong ToWrite = BufferSize;
		if (Fd->Flags & FileCompressed)
		{
			uint8 SizeBytes[2];
			if (!Reader.Read(SizeBytes, 2, OutError))
			{
				return false;
			}
			const uint16 ChunkSize = Le16(SizeBytes);
			if (ChunkSize == 0 || uint64(ChunkSize) + 2 > BytesLeft)
			{
				OutError = FString::Printf(TEXT("Bad compressed chunk in file %d"), Index);
				return false;
			}
			if (!Reader.Read(In.GetData(), ChunkSize, OutError))
			{
				return false;
			}
			// a trailing zero byte keeps inflate happy (as in unshield)
			In[ChunkSize] = 0;
			const int32 Result = Uncompress(Out.GetData(), ToWrite, In.GetData(), ChunkSize + 1);
			if (Result != Z_OK)
			{
				OutError = FString::Printf(TEXT("Decompression failed (zlib %d) in file %d"), Result, Index);
				return false;
			}
			BytesLeft -= 2 + uint64(ChunkSize);
		}
		else
		{
			ToWrite = uLong(FMath::Min<uint64>(BytesLeft, BufferSize));
			if (!Reader.Read(Out.GetData(), ToWrite, OutError))
			{
				return false;
			}
			BytesLeft -= ToWrite;
		}
		Md5.Update(Out.GetData(), ToWrite);
		if (!Sink(Out.GetData(), int64(ToWrite)))
		{
			OutError = TEXT("Cancelled");
			return false;
		}
		Written += ToWrite;
	}
	if (Written != Fd->ExpandedSize)
	{
		OutError = FString::Printf(TEXT("File %d: expected %llu bytes, got %llu"), Index, Fd->ExpandedSize, Written);
		return false;
	}
	if (Header->MajorVersion >= 6)
	{
		uint8 Digest[16];
		Md5.Final(Digest);
		if (FMemory::Memcmp(Digest, Fd->Md5, 16) != 0)
		{
			OutError = FString::Printf(TEXT("MD5 mismatch in file %d"), Index);
			return false;
		}
	}
	return true;
}
