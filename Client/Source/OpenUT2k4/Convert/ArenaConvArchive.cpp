#include "ArenaConvArchive.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "zlib.h"

namespace ArenaConvArchiveLocal
{
	constexpr uint32 UmodMagic = 0x9FE3C5A3u;
	constexpr int64 MaxEntrySize = 2048ll << 20;
	constexpr int64 CopyChunk = 1 << 20;

	/** File name safe for any OS (no separators, no leading dots); empty when nothing usable is left. */
	FString SafeFileName(const FString& InPath)
	{
		FString Name = InPath.Replace(TEXT("\\"), TEXT("/"));
		int32 Slash;
		if (Name.FindLastChar(TEXT('/'), Slash))
		{
			Name = Name.Mid(Slash + 1);
		}
		FString Out;
		for (TCHAR C : Name)
		{
			Out.AppendChar((C < 32 || C == ':' || C == '*' || C == '?' || C == '"' || C == '<' || C == '>' || C == '|') ? TEXT('_') : C);
		}
		while (Out.StartsWith(TEXT(".")))
		{
			Out.RightChopInline(1);
		}
		return Out.TrimStartAndEnd();
	}

	/** Output path for an archive member, or empty when the game does not use it. */
	FString OutPathFor(const FString& OutDir, const FString& MemberPath)
	{
		const FString Name = SafeFileName(MemberPath);
		if (Name.IsEmpty() || FPaths::GetBaseFilename(Name).Equals(TEXT("Manifest"), ESearchCase::IgnoreCase))
		{
			return FString();
		}
		const FString Ext = FPaths::GetExtension(Name, true).ToLower();
		const FString Folder = ArenaConvArchive::FolderForExtension(Ext);
		if (Folder.IsEmpty() && !(Ext == TEXT(".umod") || Ext == TEXT(".ut2mod")))
		{
			return FString();
		}
		return OutDir / (Folder.IsEmpty() ? FString(TEXT("Archives")) : Folder) / Name;
	}

	bool ReadAt(IFileHandle& H, int64 Offset, void* Dest, int64 Count)
	{
		return H.Seek(Offset) && H.Read(static_cast<uint8*>(Dest), Count);
	}

	/** Reads a UE2 compact index from a memory buffer. */
	bool CompactIndex(const TArray<uint8>& B, int64& Pos, int32& Out)
	{
		if (Pos >= B.Num())
		{
			return false;
		}
		const uint8 B0 = B[Pos++];
		int64 V = B0 & 0x3F;
		const bool bNeg = (B0 & 0x80) != 0;
		if (B0 & 0x40)
		{
			int32 Shift = 6;
			for (int32 i = 0; i < 4; ++i)
			{
				if (Pos >= B.Num())
				{
					return false;
				}
				const uint8 Bn = B[Pos++];
				V |= int64(Bn & 0x7F) << Shift;
				Shift += 7;
				if (!(Bn & 0x80))
				{
					break;
				}
			}
		}
		Out = int32(bNeg ? -V : V);
		return true;
	}

	bool U32(const TArray<uint8>& B, int64& Pos, uint32& Out)
	{
		if (Pos + 4 > B.Num())
		{
			return false;
		}
		Out = uint32(B[Pos]) | (uint32(B[Pos + 1]) << 8) | (uint32(B[Pos + 2]) << 16) | (uint32(B[Pos + 3]) << 24);
		Pos += 4;
		return true;
	}

	uint16 Le16(const uint8* P) { return uint16(P[0] | (P[1] << 8)); }
	uint32 Le32(const uint8* P) { return uint32(P[0]) | (uint32(P[1]) << 8) | (uint32(P[2]) << 16) | (uint32(P[3]) << 24); }

	/** Copies Size bytes at Offset of In into a new file Dest. */
	bool CopyRange(IFileHandle& In, int64 Offset, int64 Size, const FString& Dest)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		PF.CreateDirectoryTree(*FPaths::GetPath(Dest));
		TUniquePtr<IFileHandle> Out(PF.OpenWrite(*Dest));
		if (!Out || !In.Seek(Offset))
		{
			return false;
		}
		TArray<uint8> Buf;
		Buf.SetNumUninitialized(int32(FMath::Min<int64>(CopyChunk, FMath::Max<int64>(Size, 1))));
		for (int64 Left = Size; Left > 0;)
		{
			const int64 N = FMath::Min<int64>(Left, Buf.Num());
			if (!In.Read(Buf.GetData(), N) || !Out->Write(Buf.GetData(), N))
			{
				return false;
			}
			Left -= N;
		}
		return true;
	}

	/** Inflates a raw deflate stream of CompSize bytes at Offset of In into Dest; checks size and CRC-32. */
	bool InflateRange(IFileHandle& In, int64 Offset, int64 CompSize, int64 Size, uint32 Crc, const FString& Dest)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		PF.CreateDirectoryTree(*FPaths::GetPath(Dest));
		TUniquePtr<IFileHandle> Out(PF.OpenWrite(*Dest));
		if (!Out || !In.Seek(Offset))
		{
			return false;
		}
		z_stream Z;
		FMemory::Memzero(Z);
		if (inflateInit2(&Z, -MAX_WBITS) != Z_OK)
		{
			return false;
		}
		TArray<uint8> InBuf, OutBuf;
		InBuf.SetNumUninitialized(int32(CopyChunk));
		OutBuf.SetNumUninitialized(int32(CopyChunk));
		int64 InLeft = CompSize, Written = 0;
		uLong C = crc32(0L, Z_NULL, 0);
		int Ret = Z_OK;
		bool bOk = true;
		while (bOk && Ret != Z_STREAM_END)
		{
			if (Z.avail_in == 0)
			{
				if (InLeft <= 0)
				{
					bOk = false;
					break;
				}
				const int64 N = FMath::Min<int64>(InLeft, InBuf.Num());
				if (!In.Read(InBuf.GetData(), N))
				{
					bOk = false;
					break;
				}
				InLeft -= N;
				Z.next_in = InBuf.GetData();
				Z.avail_in = uInt(N);
			}
			Z.next_out = OutBuf.GetData();
			Z.avail_out = uInt(OutBuf.Num());
			Ret = inflate(&Z, Z_NO_FLUSH);
			if (Ret != Z_OK && Ret != Z_STREAM_END)
			{
				bOk = false;
				break;
			}
			const int64 Have = OutBuf.Num() - Z.avail_out;
			if (Have > 0)
			{
				if (Written + Have > Size || !Out->Write(OutBuf.GetData(), Have))
				{
					bOk = false;
					break;
				}
				C = crc32(C, OutBuf.GetData(), uInt(Have));
				Written += Have;
			}
		}
		inflateEnd(&Z);
		return bOk && Written == Size && uint32(C) == Crc;
	}
}

using namespace ArenaConvArchiveLocal;

namespace ArenaConvArchive
{
	FString FolderForExtension(const FString& InExt)
	{
		FString Ext = InExt.ToLower();
		if (!Ext.StartsWith(TEXT(".")))
		{
			Ext = TEXT(".") + Ext;
		}
		if (Ext == TEXT(".ut2")) return TEXT("Maps");
		if (Ext == TEXT(".utx")) return TEXT("Textures");
		if (Ext == TEXT(".usx")) return TEXT("StaticMeshes");
		if (Ext == TEXT(".uax")) return TEXT("Sounds");
		if (Ext == TEXT(".ukx")) return TEXT("Animations");
		if (Ext == TEXT(".ogg") || Ext == TEXT(".umx")) return TEXT("Music");
		if (Ext == TEXT(".u") || Ext == TEXT(".ucl") || Ext == TEXT(".int") || Ext == TEXT(".upl")) return TEXT("System");
		if (Ext == TEXT(".ka")) return TEXT("KarmaData");
		return FString();
	}

	bool IsSupportedArchive(const FString& Path)
	{
		const FString Ext = FPaths::GetExtension(Path, true).ToLower();
		return Ext == TEXT(".umod") || Ext == TEXT(".ut2mod") || Ext == TEXT(".zip");
	}

	bool IsUnsupportedArchive(const FString& Path)
	{
		const FString Ext = FPaths::GetExtension(Path, true).ToLower();
		return Ext == TEXT(".7z") || Ext == TEXT(".rar");
	}

	FResult ExtractUmod(const FString& Archive, const FString& OutDir, const TAtomic<bool>* Cancel)
	{
		FResult R;
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		TUniquePtr<IFileHandle> H(PF.OpenRead(*Archive));
		if (!H)
		{
			R.Error = TEXT("cannot be read");
			return R;
		}
		const int64 FileSize = H->Size();
		uint8 Trailer[20];
		if (FileSize < 20 || !ReadAt(*H, FileSize - 20, Trailer, 20) || Le32(Trailer) != UmodMagic)
		{
			R.Error = TEXT("is not a UT2004 umod installer (damaged or a different format)");
			return R;
		}
		const int64 DirOffset = Le32(Trailer + 4);
		const int64 TotalSize = Le32(Trailer + 8);
		if (TotalSize != FileSize || DirOffset <= 0 || DirOffset >= FileSize - 20)
		{
			R.Error = TEXT("is incomplete or damaged (the file table does not match its size)");
			return R;
		}
		TArray<uint8> Dir;
		Dir.SetNumUninitialized(int32(FileSize - 20 - DirOffset));
		if (!ReadAt(*H, DirOffset, Dir.GetData(), Dir.Num()))
		{
			R.Error = TEXT("cannot be read");
			return R;
		}
		int64 Pos = 0;
		int32 Count = 0;
		if (!CompactIndex(Dir, Pos, Count) || Count < 0 || Count > 100000)
		{
			R.Error = TEXT("has a damaged file table");
			return R;
		}
		struct FEntry { FString Name; int64 Offset; int64 Size; };
		TArray<FEntry> Entries;
		for (int32 i = 0; i < Count; ++i)
		{
			int32 Len = 0;
			if (!CompactIndex(Dir, Pos, Len) || Len == 0 || FMath::Abs(Len) > 1024)
			{
				R.Error = TEXT("has a damaged file table");
				return R;
			}
			FString Name;
			if (Len > 0)
			{
				if (Pos + Len > Dir.Num())
				{
					R.Error = TEXT("has a damaged file table");
					return R;
				}
				for (int32 c = 0; c < Len; ++c)
				{
					if (Dir[Pos + c])
					{
						Name.AppendChar(TCHAR(Dir[Pos + c]));
					}
				}
				Pos += Len;
			}
			else
			{
				const int32 N = -Len;
				if (Pos + int64(N) * 2 > Dir.Num())
				{
					R.Error = TEXT("has a damaged file table");
					return R;
				}
				for (int32 c = 0; c < N; ++c)
				{
					const TCHAR Ch = TCHAR(Le16(&Dir[Pos + c * 2]));
					if (Ch)
					{
						Name.AppendChar(Ch);
					}
				}
				Pos += int64(N) * 2;
			}
			uint32 Offset = 0, Size = 0, Flags = 0;
			if (!U32(Dir, Pos, Offset) || !U32(Dir, Pos, Size) || !U32(Dir, Pos, Flags) || int64(Offset) + int64(Size) > DirOffset)
			{
				R.Error = TEXT("has a damaged file table");
				return R;
			}
			Entries.Add({ Name, int64(Offset), int64(Size) });
		}
		for (const FEntry& E : Entries)
		{
			if (Cancel && Cancel->Load())
			{
				R.Error = TEXT("unpacking was cancelled");
				return R;
			}
			const FString Dest = OutPathFor(OutDir, E.Name);
			if (Dest.IsEmpty())
			{
				++R.Skipped;
				continue;
			}
			if (!CopyRange(*H, E.Offset, E.Size, Dest))
			{
				R.Error = FString::Printf(TEXT("could not unpack %s"), *SafeFileName(E.Name));
				return R;
			}
			R.Files.Add(Dest);
		}
		R.bOk = true;
		return R;
	}

	FResult ExtractZip(const FString& Archive, const FString& OutDir, const TAtomic<bool>* Cancel)
	{
		FResult R;
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		TUniquePtr<IFileHandle> H(PF.OpenRead(*Archive));
		if (!H)
		{
			R.Error = TEXT("cannot be read");
			return R;
		}
		const int64 FileSize = H->Size();
		// end of central directory record: within the last 64 KB + 22 bytes (comment)
		const int64 TailLen = FMath::Min<int64>(FileSize, 65536 + 22);
		TArray<uint8> Tail;
		Tail.SetNumUninitialized(int32(TailLen));
		if (TailLen < 22 || !ReadAt(*H, FileSize - TailLen, Tail.GetData(), TailLen))
		{
			R.Error = TEXT("is not a zip file");
			return R;
		}
		int32 Eocd = -1;
		for (int32 i = int32(TailLen) - 22; i >= 0; --i)
		{
			if (Le32(&Tail[i]) == 0x06054b50u)
			{
				Eocd = i;
				break;
			}
		}
		if (Eocd < 0)
		{
			R.Error = TEXT("is not a zip file (or it is damaged)");
			return R;
		}
		const uint8* E = &Tail[Eocd];
		const int32 NumEntries = Le16(E + 10);
		const int64 CdSize = Le32(E + 12);
		const int64 CdOffset = Le32(E + 16);
		if (Le32(E + 16) == 0xFFFFFFFFu || Le16(E + 10) == 0xFFFF)
		{
			R.Error = TEXT("is a zip64 archive (over 4 GB), which is not supported: unpack it into the folder instead");
			return R;
		}
		if (CdOffset + CdSize > FileSize)
		{
			R.Error = TEXT("is incomplete or damaged");
			return R;
		}
		TArray<uint8> Cd;
		Cd.SetNumUninitialized(int32(CdSize));
		if (!ReadAt(*H, CdOffset, Cd.GetData(), CdSize))
		{
			R.Error = TEXT("cannot be read");
			return R;
		}
		int64 Pos = 0;
		TArray<FString> Nested;
		for (int32 i = 0; i < NumEntries; ++i)
		{
			if (Cancel && Cancel->Load())
			{
				R.Error = TEXT("unpacking was cancelled");
				return R;
			}
			if (Pos + 46 > Cd.Num() || Le32(&Cd[Pos]) != 0x02014b50u)
			{
				R.Error = TEXT("has a damaged file table");
				return R;
			}
			const uint8* C = &Cd[Pos];
			const uint16 Flags = Le16(C + 8);
			const uint16 Method = Le16(C + 10);
			const uint32 Crc = Le32(C + 16);
			const int64 CompSize = Le32(C + 20);
			const int64 Size = Le32(C + 24);
			const int32 NameLen = Le16(C + 28), ExtraLen = Le16(C + 30), CommentLen = Le16(C + 32);
			const int64 LocalOffset = Le32(C + 42);
			if (Pos + 46 + NameLen > Cd.Num())
			{
				R.Error = TEXT("has a damaged file table");
				return R;
			}
			FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(C + 46), NameLen);
			const FString Name(Conv.Length(), Conv.Get());
			Pos += 46 + NameLen + ExtraLen + CommentLen;
			if (Name.EndsWith(TEXT("/")))
			{
				continue; // folder
			}
			const FString Dest = OutPathFor(OutDir, Name);
			if (Dest.IsEmpty())
			{
				++R.Skipped;
				continue;
			}
			if (Flags & 1)
			{
				R.Error = TEXT("is password protected");
				return R;
			}
			if (CompSize == 0xFFFFFFFFll || Size == 0xFFFFFFFFll || Size > MaxEntrySize)
			{
				R.Error = FString::Printf(TEXT("contains a file that is too large (%s)"), *SafeFileName(Name));
				return R;
			}
			uint8 Local[30];
			if (!ReadAt(*H, LocalOffset, Local, 30) || Le32(Local) != 0x04034b50u)
			{
				R.Error = TEXT("is damaged");
				return R;
			}
			const int64 DataOffset = LocalOffset + 30 + Le16(Local + 26) + Le16(Local + 28);
			bool bOk = false;
			if (Method == 0)
			{
				bOk = CompSize == Size && CopyRange(*H, DataOffset, Size, Dest);
			}
			else if (Method == 8)
			{
				bOk = InflateRange(*H, DataOffset, CompSize, Size, Crc, Dest);
			}
			else
			{
				R.Error = FString::Printf(TEXT("uses a compression method the game cannot read (%d): unpack it into the folder instead"), int32(Method));
				return R;
			}
			if (!bOk)
			{
				R.Error = FString::Printf(TEXT("could not unpack %s (damaged?)"), *SafeFileName(Name));
				return R;
			}
			const FString Ext = FPaths::GetExtension(Dest, true).ToLower();
			if (Ext == TEXT(".umod") || Ext == TEXT(".ut2mod"))
			{
				Nested.Add(Dest);
			}
			else
			{
				R.Files.Add(Dest);
			}
		}
		// umod installers inside the zip
		for (const FString& N : Nested)
		{
			FResult Inner = ExtractUmod(N, OutDir, Cancel);
			PF.DeleteFile(*N);
			if (!Inner.bOk)
			{
				R.Error = FString::Printf(TEXT("contains %s, which %s"), *FPaths::GetCleanFilename(N), *Inner.Error);
				return R;
			}
			R.Files.Append(Inner.Files);
			R.Skipped += Inner.Skipped;
		}
		R.bOk = true;
		return R;
	}

	FResult Extract(const FString& Archive, const FString& OutDir, const TAtomic<bool>* Cancel)
	{
		const FString Ext = FPaths::GetExtension(Archive, true).ToLower();
		if (Ext == TEXT(".zip"))
		{
			return ExtractZip(Archive, OutDir, Cancel);
		}
		if (Ext == TEXT(".umod") || Ext == TEXT(".ut2mod"))
		{
			return ExtractUmod(Archive, OutDir, Cancel);
		}
		FResult R;
		R.Error = TEXT("is not an archive the game can unpack");
		return R;
	}
}
