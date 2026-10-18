#include "ArenaDlIso.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"

namespace ArenaDlIsoLocal
{
	constexpr int64 SectorSize = 2048;

	class FFileRangeReader : public IArenaDlReader
	{
	public:
		FFileRangeReader(IFileHandle* InFile, int64 InBase, int64 InLength) : File(InFile), Base(InBase), Length(InLength) {}

		virtual int64 Size() const override { return Length; }

		virtual bool Read(int64 Offset, void* Dest, int64 Count) override
		{
			if (Offset < 0 || Count < 0 || Offset + Count > Length)
			{
				return false;
			}
			if (Count == 0)
			{
				return true;
			}
			if (Pos != Offset)
			{
				if (!File->Seek(Base + Offset))
				{
					return false;
				}
			}
			if (!File->Read(static_cast<uint8*>(Dest), Count))
			{
				Pos = -1;
				return false;
			}
			Pos = Offset + Count;
			return true;
		}

	private:
		TUniquePtr<IFileHandle> File;
		int64 Base = 0;
		int64 Length = 0;
		/** Current read position relative to Base (skips redundant seeks for sequential reads), -1 = unknown. */
		int64 Pos = -1;
	};

	FORCEINLINE uint32 Le32(const uint8* P) { return uint32(P[0]) | (uint32(P[1]) << 8) | (uint32(P[2]) << 16) | (uint32(P[3]) << 24); }

	/** Directory record file identifier -> name (Joliet: UCS-2 big endian), without the ";1" version suffix. */
	FString DecodeName(const uint8* Id, int32 Len, bool bJoliet)
	{
		FString Name;
		if (bJoliet)
		{
			for (int32 I = 0; I + 1 < Len; I += 2)
			{
				Name.AppendChar(TCHAR((uint16(Id[I]) << 8) | Id[I + 1]));
			}
		}
		else
		{
			for (int32 I = 0; I < Len; ++I)
			{
				Name.AppendChar(TCHAR(Id[I]));
			}
		}
		int32 Semi;
		if (Name.FindLastChar(TEXT(';'), Semi))
		{
			Name.LeftInline(Semi);
		}
		// ISO 9660 level 1 names of files without an extension end in '.'
		if (!bJoliet && Name.EndsWith(TEXT(".")))
		{
			Name.LeftChopInline(1);
		}
		return Name;
	}
}

TUniquePtr<IArenaDlReader> IArenaDlReader::OpenFile(const FString& Path, int64 Base, int64 Length)
{
	IFileHandle* File = FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path);
	if (!File)
	{
		return nullptr;
	}
	const int64 FileSize = File->Size();
	if (Length < 0)
	{
		Length = FileSize - Base;
	}
	if (Base < 0 || Length < 0 || Base + Length > FileSize)
	{
		delete File;
		return nullptr;
	}
	return MakeUnique<ArenaDlIsoLocal::FFileRangeReader>(File, Base, Length);
}

bool FArenaIsoImage::Open(const FString& Path)
{
	using namespace ArenaDlIsoLocal;
	ImagePath = Path;
	Entries.Reset();
	Index.Reset();
	Error.Reset();
	TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
	if (!File)
	{
		Error = FString::Printf(TEXT("Cannot open %s"), *Path);
		return false;
	}
	// volume descriptors start at sector 16: 1 = primary, 2 = supplementary (Joliet when the escape sequence says so), 255 = end
	int64 RootExtent = -1, RootLength = 0;
	int64 JolietExtent = -1, JolietLength = 0;
	uint8 Sector[SectorSize];
	for (int64 S = 16; S < 64; ++S)
	{
		if (!File->Seek(S * SectorSize) || !File->Read(Sector, SectorSize))
		{
			break;
		}
		if (FMemory::Memcmp(Sector + 1, "CD001", 5) != 0)
		{
			break;
		}
		const uint8 Type = Sector[0];
		if (Type == 255)
		{
			break;
		}
		const uint8* Root = Sector + 156;
		if (Type == 1)
		{
			RootExtent = Le32(Root + 2);
			RootLength = Le32(Root + 10);
		}
		else if (Type == 2 && Sector[88] == '%' && Sector[89] == '/' && (Sector[90] == '@' || Sector[90] == 'C' || Sector[90] == 'E'))
		{
			JolietExtent = Le32(Root + 2);
			JolietLength = Le32(Root + 10);
		}
	}
	if (RootExtent < 0 && JolietExtent < 0)
	{
		Error = FString::Printf(TEXT("%s is not an ISO 9660 disc image"), *Path);
		return false;
	}
	bJoliet = JolietExtent >= 0;
	if (!ReadDirectory(*File, bJoliet ? JolietExtent : RootExtent, bJoliet ? JolietLength : RootLength, FString(), 0))
	{
		if (Error.IsEmpty())
		{
			Error = FString::Printf(TEXT("Cannot read the directory tree of %s"), *Path);
		}
		return false;
	}
	for (int32 I = 0; I < Entries.Num(); ++I)
	{
		Index.Add(Entries[I].Path.ToLower(), I);
	}
	return true;
}

bool FArenaIsoImage::ReadDirectory(IFileHandle& File, int64 Extent, int64 Length, const FString& Prefix, int32 Depth)
{
	using namespace ArenaDlIsoLocal;
	if (Depth > 16 || Length <= 0 || Length > (64 << 20))
	{
		return Depth <= 16;
	}
	TArray<uint8> Data;
	Data.SetNumUninitialized(Length);
	if (!File.Seek(Extent * SectorSize) || !File.Read(Data.GetData(), Length))
	{
		return false;
	}
	struct FSub { FString Path; int64 Extent; int64 Length; };
	TArray<FSub> Subs;
	int64 Pos = 0;
	while (Pos < Length)
	{
		const int32 RecLen = Data[Pos];
		if (RecLen == 0)
		{
			// records never cross a sector boundary: the rest of this sector is padding
			Pos = (Pos / SectorSize + 1) * SectorSize;
			continue;
		}
		if (RecLen < 34 || Pos + RecLen > Length)
		{
			Error = TEXT("Corrupt directory record in the disc image");
			return false;
		}
		const uint8* Rec = Data.GetData() + Pos;
		const int64 RecExtent = Le32(Rec + 2);
		const int64 RecSize = Le32(Rec + 10);
		const uint8 Flags = Rec[25];
		const int32 IdLen = Rec[32];
		Pos += RecLen;
		if (IdLen == 1 && (Rec[33] == 0 || Rec[33] == 1))
		{
			continue; // "." and ".."
		}
		if (33 + IdLen > RecLen)
		{
			Error = TEXT("Corrupt directory record in the disc image");
			return false;
		}
		if ((Flags & 0x80) != 0 || Rec[26] != 0 || Rec[27] != 0)
		{
			Error = TEXT("The disc image uses multi-extent or interleaved files, which are not supported");
			return false;
		}
		const FString Name = DecodeName(Rec + 33, IdLen, bJoliet);
		FEntry E;
		E.Path = Prefix.IsEmpty() ? Name : Prefix / Name;
		E.Offset = RecExtent * SectorSize;
		E.Size = RecSize;
		E.bDirectory = (Flags & 0x02) != 0;
		Entries.Add(E);
		if (E.bDirectory)
		{
			Subs.Add({ E.Path, RecExtent, RecSize });
		}
	}
	for (const FSub& S : Subs)
	{
		if (!ReadDirectory(File, S.Extent, S.Length, S.Path, Depth + 1))
		{
			return false;
		}
	}
	return true;
}

const FArenaIsoImage::FEntry* FArenaIsoImage::Find(const FString& Path) const
{
	const int32* I = Index.Find(Path.Replace(TEXT("\\"), TEXT("/")).ToLower());
	return I ? &Entries[*I] : nullptr;
}

TUniquePtr<IArenaDlReader> FArenaIsoImage::OpenFile(const FString& Path) const
{
	const FEntry* E = Find(Path);
	if (!E || E->bDirectory)
	{
		return nullptr;
	}
	return IArenaDlReader::OpenFile(ImagePath, E->Offset, E->Size);
}
