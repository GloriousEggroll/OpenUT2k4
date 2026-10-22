#include "ArenaConvUserContent.h"
#include "ArenaConvConfig.h"
#include "ArenaConvArchive.h"
#include "ArenaConvJson.h"
#include "ArenaConverter.h"
#include "ArenaConvFonts.h"
#include "ArenaConvLocalization.h"
#include "ArenaConvScripts.h"
#include "../ArenaContent.h"
#include "../OpenUT2k4.h"
#if ARENA_CONV_BUILD
#include "ArenaConvData.h"
#include "ArenaConvExporter.h"
#include "ArenaConvPackage.h"
#endif
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace ArenaConvUserContentLocal
{
	const TCHAR* const KindFolders[] = { TEXT("textures"), TEXT("materials"), TEXT("staticmeshes"), TEXT("sounds"), TEXT("skeletalmeshes"),
		TEXT("animations"), TEXT("vertmeshes") };

	int64 UnixTime(const FDateTime& T)
	{
		return T == FDateTime::MinValue() ? 0 : (T - FDateTime(1970, 1, 1)).GetTicks() / ETimespan::TicksPerSecond;
	}

	FString FileMd5(const FString& Path)
	{
		TUniquePtr<IFileHandle> H(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
		if (!H)
		{
			return FString();
		}
		FMD5 Md5;
		TArray<uint8> Buf;
		Buf.SetNumUninitialized(1 << 20);
		for (int64 Left = H->Size(); Left > 0;)
		{
			const int64 N = FMath::Min<int64>(Left, Buf.Num());
			if (!H->Read(Buf.GetData(), N))
			{
				return FString();
			}
			Md5.Update(Buf.GetData(), uint64(N));
			Left -= N;
		}
		uint8 Digest[16];
		Md5.Final(Digest);
		return BytesToHex(Digest, 16).ToLower();
	}

	/** Every file below Dir (any depth), skipping hidden folders and files; sorted, absolute. */
	TArray<FString> ListFiles(const FString& Dir)
	{
		TArray<FString> Out;
		if (!IFileManager::Get().DirectoryExists(*Dir))
		{
			return Out;
		}
		IFileManager::Get().IterateDirectoryRecursively(*Dir, [&](const TCHAR* P, bool bDir)
		{
			if (bDir)
			{
				return true;
			}
			const FString Path = P;
			const FString Rel = Path.Mid(Dir.Len());
			if (Rel.Contains(TEXT("/.")) || Rel.Contains(TEXT("\\.")))
			{
				return true;
			}
			Out.Add(FPaths::ConvertRelativePathToFull(Path));
			return true;
		});
		Out.Sort();
		return Out;
	}

	/** Files that matter for HasChanges / the scan: usable content and archives. */
	bool IsRelevant(const FString& Path)
	{
		const FString Ext = FPaths::GetExtension(Path, true);
		return !ArenaConvArchive::FolderForExtension(Ext).IsEmpty() || ArenaConvArchive::IsSupportedArchive(Path) || ArenaConvArchive::IsUnsupportedArchive(Path);
	}

	/** Size + mtime fingerprint of the folder's relevant files (cheap). */
	FConvJson QuickFingerprint(const FString& UserDir)
	{
		FConvJson Out = FConvJson::Object();
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		for (const FString& F : ListFiles(UserDir))
		{
			if (!IsRelevant(F))
			{
				continue;
			}
			const FFileStatData St = PF.GetStatData(*F);
			FConvJson E = FConvJson::Array();
			E.Add(St.FileSize);
			E.Add(UnixTime(St.ModificationTime));
			Out.Set(F.Mid(UserDir.Len()), E);
		}
		return Out;
	}

	bool SameJson(const FConvJson& A, const FConvJson& B)
	{
		TArray<ANSICHAR> TA, TB;
		A.Write(TA, -1);
		B.Write(TB, -1);
		return TA == TB;
	}

	/** Unquotes a UE2 ini / record value. */
	FString Unquote(FString V)
	{
		V.TrimStartAndEndInline();
		if (V.Len() >= 2 && V.StartsWith(TEXT("\"")) && V.EndsWith(TEXT("\"")))
		{
			V = V.Mid(1, V.Len() - 2);
		}
		return V;
	}

	/** [LevelSummary] Title / Description of a map's .int file. */
	void ReadInt(const FString& Path, TMap<FString, FString>& Out)
	{
		TArray<FString> Lines;
		FFileHelper::LoadFileToStringArray(Lines, *Path);
		FString Section;
		for (FString L : Lines)
		{
			L.TrimStartAndEndInline();
			if (L.StartsWith(TEXT("[")))
			{
				Section = L;
				continue;
			}
			int32 Eq;
			if (Section.Equals(TEXT("[LevelSummary]"), ESearchCase::IgnoreCase) && L.FindChar(TEXT('='), Eq))
			{
				const FString Key = L.Left(Eq).TrimStartAndEnd().ToLower();
				if (Key == TEXT("title") || Key == TEXT("description") || Key == TEXT("author"))
				{
					Out.Add(Key, Unquote(L.Mid(Eq + 1)));
				}
			}
		}
	}

	/** Map=(MapName=..,Author=..,PlayerCountMin=..) records of a .ucl file -> lower map name -> key -> value. */
	void ReadUcl(const FString& Path, TMap<FString, TMap<FString, FString>>& Out)
	{
		TArray<FString> Lines;
		FFileHelper::LoadFileToStringArray(Lines, *Path);
		for (FString L : Lines)
		{
			L.TrimStartAndEndInline();
			if (!L.StartsWith(TEXT("Map=("), ESearchCase::IgnoreCase))
			{
				continue;
			}
			TMap<FString, FString> Rec;
			int32 i = 5;
			while (i < L.Len() && L[i] != TEXT(')'))
			{
				FString Key;
				while (i < L.Len() && L[i] != TEXT('=') && L[i] != TEXT(')'))
				{
					Key.AppendChar(L[i++]);
				}
				if (i >= L.Len() || L[i] != TEXT('='))
				{
					break;
				}
				++i;
				FString Val;
				if (i < L.Len() && L[i] == TEXT('"'))
				{
					++i;
					while (i < L.Len() && L[i] != TEXT('"'))
					{
						Val.AppendChar(L[i++]);
					}
					++i;
				}
				else
				{
					while (i < L.Len() && L[i] != TEXT(',') && L[i] != TEXT(')'))
					{
						Val.AppendChar(L[i++]);
					}
				}
				Rec.Add(Key.TrimStartAndEnd().ToLower(), Val);
				if (i < L.Len() && L[i] == TEXT(','))
				{
					++i;
				}
			}
			if (const FString* Name = Rec.Find(TEXT("mapname")))
			{
				Out.Add(Name->ToLower(), MoveTemp(Rec));
			}
		}
	}

	/** Deletes <kind>/<Name> (any case) for every asset kind. */
	void RemoveAssetFolders(const FString& CacheDir, const FString& Name)
	{
		IFileManager& FM = IFileManager::Get();
		const FString Want = Name.ToLower();
		for (const TCHAR* Kind : KindFolders)
		{
			TArray<FString> Dirs;
			FM.FindFiles(Dirs, *(CacheDir / Kind / TEXT("*")), false, true);
			for (const FString& D : Dirs)
			{
				if (D.ToLower() == Want)
				{
					FM.DeleteDirectory(*(CacheDir / Kind / D), false, true);
				}
			}
		}
	}

	void RemoveMapOutputs(const FString& CacheDir, const FString& Map)
	{
		IFileManager& FM = IFileManager::Get();
		FM.DeleteDirectory(*(CacheDir / TEXT("maps") / Map), false, true);
		FM.Delete(*(CacheDir / TEXT("data") / (Map + TEXT(".json"))), false, true, true);
		RemoveAssetFolders(CacheDir, Map);
	}

	FString JoinNames(const TArray<FString>& Names, int32 Max = 6)
	{
		TArray<FString> Shown;
		for (int32 i = 0; i < Names.Num() && i < Max; ++i)
		{
			Shown.Add(Names[i]);
		}
		FString S = FString::Join(Shown, TEXT(", "));
		if (Names.Num() > Max)
		{
			S += FString::Printf(TEXT(" and %d more"), Names.Num() - Max);
		}
		return S;
	}

	/** schema_version of a map.json from its first bytes (the exporter writes it near the top); 0 if unknown. */
	int32 MapJsonSchema(const FString& Path)
	{
		TUniquePtr<IFileHandle> H(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
		if (!H)
		{
			return 0;
		}
		ANSICHAR Buf[1025];
		const int64 N = FMath::Min<int64>(H->Size(), 1024);
		if (!H->Read(reinterpret_cast<uint8*>(Buf), N))
		{
			return 0;
		}
		Buf[N] = 0;
		const ANSICHAR* P = FCStringAnsi::Strstr(Buf, "\"schema_version\"");
		if (!P)
		{
			return 0;
		}
		P += 16;
		while (*P == ' ' || *P == ':')
		{
			++P;
		}
		return FCStringAnsi::Atoi(P);
	}

	/**
	 * Cached maps (stock or custom) written with an older map schema than ArenaConverterVersions::MapSchemaVersion,
	 * except those in Skip (lower-case; maps that could not be updated before).
	 */
	TArray<FString> OutdatedMaps(const FString& CacheDir, const TSet<FString>& Skip)
	{
		TArray<FString> Dirs, Out;
		IFileManager::Get().FindFiles(Dirs, *(CacheDir / TEXT("maps") / TEXT("*")), false, true);
		Dirs.Sort();
		for (const FString& D : Dirs)
		{
			const FString Json = CacheDir / TEXT("maps") / D / TEXT("map.json");
			if (!Skip.Contains(D.ToLower()) && IFileManager::Get().FileExists(*Json) && MapJsonSchema(Json) < ArenaConverterVersions::MapSchemaVersion)
			{
				Out.Add(D);
			}
		}
		return Out;
	}

	TSet<FString> SkipSet(const FConvJson& State)
	{
		TSet<FString> Out;
		for (const TPair<FString, FConvJson>& KV : State.Get(TEXT("schema_failed")).O)
		{
			if (int32(KV.Value.AsNumber()) >= ArenaConverterVersions::MapSchemaVersion)
			{
				Out.Add(KV.Key.ToLower());
			}
		}
		return Out;
	}

	bool IsPackageExt(const FString& Ext)
	{
		return Ext == TEXT(".ut2") || Ext == TEXT(".utx") || Ext == TEXT(".usx") || Ext == TEXT(".uax") || Ext == TEXT(".ukx") || Ext == TEXT(".u");
	}
}

using namespace ArenaConvUserContentLocal;

namespace ArenaConvUserContent
{
	FString StatePath(const FString& CacheDir)
	{
		return StatePath(CacheDir, TEXT("usercontent"));
	}

	FString StatePath(const FString& CacheDir, const FString& StateDirName)
	{
		return CacheDir / StateDirName / TEXT("state.json");
	}

	bool HasChanges(const FOptions& Options)
	{
		FConvJson State;
		const FConvJson Now = QuickFingerprint(FPaths::ConvertRelativePathToFull(Options.UserDir));
		const bool bHasState = FConvJson::LoadFile(StatePath(Options.CacheDir), State);
		// converted maps of an older map schema are updated by the next pass
		if (OutdatedMaps(Options.CacheDir, bHasState ? SkipSet(State) : TSet<FString>()).Num())
		{
			return true;
		}
		// a cache converted before the HUD fonts were exported (ArenaConvFonts) gets them by the next pass
		if (ArenaConvFonts::NeedsUpdate(Options.CacheDir))
		{
			return true;
		}
		// the same for the localization files (ArenaConvLocalization), when an install is there to copy them from
		if (ArenaConvLocalization::NeedsUpdate(Options.CacheDir) && ArenaContent::IsUT2004Install(Options.InstallDir))
		{
			return true;
		}
		// and for the script packages (ArenaConvScripts)
		if (ArenaConvScripts::NeedsUpdate(Options.CacheDir) && ArenaContent::IsUT2004Install(Options.InstallDir))
		{
			return true;
		}
		if (!bHasState)
		{
			return Now.Num() > 0;
		}
		if (State.GetNumber(TEXT("version"), 0) != StateVersion
			|| State.GetString(TEXT("install")) != FPaths::ConvertRelativePathToFull(Options.InstallDir)
			|| State.GetString(TEXT("user_dir")) != FPaths::ConvertRelativePathToFull(Options.UserDir))
		{
			return true;
		}
		return !SameJson(State.Get(TEXT("fingerprint")), Now);
	}

#if ARENA_CONV_BUILD
	bool Run(const FOptions& Options, TFunctionRef<void(const FProgress&)> OnProgress, const TAtomic<bool>* Cancel, FStats& S)
	{
		const double T0 = FPlatformTime::Seconds();
		double TStage = T0;
		auto EndStage = [&](const TCHAR* Name)
		{
			const double Now = FPlatformTime::Seconds();
			S.StageSeconds.Emplace(Name, Now - TStage);
			TStage = Now;
		};
		FProgress P;
		auto Report = [&](const FString& Stage, const FString& Item, float Fraction)
		{
			P.Stage = Stage;
			P.Item = Item;
			P.Fraction = Fraction;
			OnProgress(P);
		};
		auto Cancelled = [Cancel]() { return Cancel && Cancel->Load(); };
		IFileManager& FM = IFileManager::Get();
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

		const FString Out = FPaths::ConvertRelativePathToFull(Options.CacheDir);
		const FString UserDir = FPaths::ConvertRelativePathToFull(Options.UserDir);
		const FString Install = FPaths::ConvertRelativePathToFull(Options.InstallDir);
		const FString UcDir = Out / Options.StateDirName;
		const FString UnpackRoot = UcDir / TEXT("unpacked");
		PF.CreateDirectoryTree(*UcDir);

		FConvJson Prev;
		if (!FConvJson::LoadFile(StatePath(Out, Options.StateDirName), Prev) || Prev.Type != FConvJson::EType::Object)
		{
			Prev = FConvJson::Object();
		}
		// a different install or state layout: everything is converted again (the old entries are still removed)
		const bool bReuse = !Options.bForce && Prev.GetNumber(TEXT("version"), 0) == StateVersion && Prev.GetString(TEXT("install")) == Install;

		FConvJson State = FConvJson::Object();
		State.Set(TEXT("version"), StateVersion);
		State.Set(TEXT("install"), Install);
		State.Set(TEXT("user_dir"), UserDir);
		FConvJson Problems = FConvJson::Array();
		auto Problem = [&Problems](const FString& File, const FString& Reason, const TCHAR* Severity = TEXT("error"))
		{
			FConvJson E = FConvJson::Object();
			E.Set(TEXT("file"), File);
			E.Set(TEXT("reason"), Reason);
			E.Set(TEXT("severity"), Severity);
			Problems.Add(E);
			UE_LOG(LogOpenUT2k4, Log, TEXT("Custom content: %s: %s"), *File, *Reason);
		};

		// ---- 1. list the folder, unpack archives ----
		Report(TEXT("Looking for custom content"), FString(), 0.f);
		const FConvJson Fingerprint = QuickFingerprint(UserDir);
		// an explicit file list (downloaded packages) replaces the folder listing
		const TArray<FString> AllFiles = Options.Files.Num() ? Options.Files : ListFiles(UserDir);
		EndStage(TEXT("scan"));
		TArray<FString> Loose, Archives;
		for (const FString& F : AllFiles)
		{
			if (ArenaConvArchive::IsSupportedArchive(F))
			{
				Archives.Add(F);
			}
			else if (ArenaConvArchive::IsUnsupportedArchive(F))
			{
				Problem(FPaths::GetCleanFilename(F), TEXT("is a 7-Zip / RAR archive, which the game cannot open: unpack it into the custom content folder"));
			}
			else if (!ArenaConvArchive::FolderForExtension(FPaths::GetExtension(F, true)).IsEmpty())
			{
				Loose.Add(F);
			}
		}
		const FConvJson& PrevFiles = Prev.Get(TEXT("files"));
		auto Fingerprint1 = [&](const FString& Path, FConvJson& Entry) -> FString
		{
			const FFileStatData St = PF.GetStatData(*Path);
			const int64 Size = St.FileSize, MTime = UnixTime(St.ModificationTime);
			const FConvJson& Old = PrevFiles.Get(Path);
			FString Md5;
			if (Old.Type == FConvJson::EType::Object && int64(Old.GetNumber(TEXT("size"), -1)) == Size && int64(Old.GetNumber(TEXT("mtime"), -1)) == MTime)
			{
				Md5 = Old.GetString(TEXT("md5"));
			}
			if (Md5.IsEmpty())
			{
				Md5 = FileMd5(Path);
			}
			Entry = FConvJson::Object();
			Entry.Set(TEXT("size"), Size);
			Entry.Set(TEXT("mtime"), MTime);
			Entry.Set(TEXT("md5"), Md5);
			return Md5;
		};

		FConvJson Files = FConvJson::Object();
		FConvJson ArchiveState = FConvJson::Object();
		TArray<FString> Unpacked;
		TMap<FString, FString> SourceOf; // unpacked file -> archive name
		TSet<FString> KeepUnpackDirs;
		for (int32 i = 0; i < Archives.Num(); ++i)
		{
			if (Cancelled())
			{
				break;
			}
			const FString& A = Archives[i];
			const FString AName = FPaths::GetCleanFilename(A);
			Report(TEXT("Unpacking archives"), AName, 0.1f * float(i) / float(FMath::Max(1, Archives.Num())));
			FConvJson FileEntry;
			const FString Md5 = Fingerprint1(A, FileEntry);
			Files.Set(A, FileEntry);
			const FString Dir = UnpackRoot / FConvExporter::Safe(AName.Replace(TEXT("."), TEXT("_")));
			KeepUnpackDirs.Add(FPaths::GetCleanFilename(Dir));
			const FConvJson& Old = Prev.Get(TEXT("archives")).Get(A);
			FConvJson Rec = FConvJson::Object();
			Rec.Set(TEXT("md5"), Md5);
			TArray<FString> Got;
			bool bReused = false;
			if (Old.Type == FConvJson::EType::Object && Old.GetString(TEXT("md5")) == Md5 && Old.Get(TEXT("ok")).Truthy())
			{
				bReused = true;
				for (const FConvJson& F : Old.Get(TEXT("files")).A)
				{
					if (!FM.FileExists(*F.S))
					{
						bReused = false;
						break;
					}
					Got.Add(F.S);
				}
			}
			if (!bReused)
			{
				Got.Reset();
				FM.DeleteDirectory(*Dir, false, true);
				const ArenaConvArchive::FResult R = ArenaConvArchive::Extract(A, Dir, Cancel);
				if (!R.bOk)
				{
					FM.DeleteDirectory(*Dir, false, true);
					Rec.Set(TEXT("ok"), false);
					Rec.Set(TEXT("error"), R.Error);
					Problem(AName, R.Error);
					ArchiveState.Set(A, Rec);
					continue;
				}
				Got = R.Files;
				UE_LOG(LogOpenUT2k4, Log, TEXT("Custom content: unpacked %s (%d files, %d skipped)"), *AName, R.Files.Num(), R.Skipped);
			}
			Rec.Set(TEXT("ok"), true);
			FConvJson FL = FConvJson::Array();
			for (const FString& F : Got)
			{
				FL.Add(F);
				Unpacked.Add(F);
				SourceOf.Add(F, AName);
			}
			Rec.Set(TEXT("files"), FL);
			if (Got.Num() == 0)
			{
				Problem(AName, TEXT("contains no maps or packages the game can use"), TEXT("info"));
			}
			ArchiveState.Set(A, Rec);
		}
		// unpacked folders of archives that are gone
		{
			TArray<FString> Dirs;
			FM.FindFiles(Dirs, *(UnpackRoot / TEXT("*")), false, true);
			for (const FString& D : Dirs)
			{
				if (!KeepUnpackDirs.Contains(D))
				{
					FM.DeleteDirectory(*(UnpackRoot / D), false, true);
				}
			}
		}
		EndStage(TEXT("unpack"));
		if (Cancelled())
		{
			S.bCancelled = true;
			return false;
		}

		// ---- 2. fingerprints, duplicates ----
		Report(TEXT("Checking custom content"), FString(), 0.1f);
		struct FUserFile
		{
			FString Path;
			FString Name;     // base name
			FString Ext;      // lower, with dot
			FString Md5;
			FString Source;   // "" or archive name
		};
		TMap<FString, FUserFile> ByFile; // lower clean file name -> file
		TArray<FString> Order;
		for (const FString& F : Loose)
		{
			Order.Add(F);
		}
		Order.Append(Unpacked);
		for (const FString& F : Order)
		{
			const FString Key = FPaths::GetCleanFilename(F).ToLower();
			const FString* Src = SourceOf.Find(F);
			if (const FUserFile* Dup = ByFile.Find(Key))
			{
				Problem(Src ? FString::Printf(TEXT("%s (in %s)"), *FPaths::GetCleanFilename(F), **Src) : F.Mid(UserDir.Len() + 1),
					FString::Printf(TEXT("is a second copy of %s; the first one (%s) is used"), *FPaths::GetCleanFilename(F),
						Dup->Source.IsEmpty() ? *Dup->Path.Mid(UserDir.Len() + 1) : *FString::Printf(TEXT("in %s"), *Dup->Source)), TEXT("info"));
				continue;
			}
			FUserFile U;
			U.Path = F;
			U.Name = FPaths::GetBaseFilename(F);
			U.Ext = FPaths::GetExtension(F, true).ToLower();
			U.Source = Src ? *Src : FString();
			FConvJson Entry;
			U.Md5 = Fingerprint1(F, Entry);
			Files.Set(F, Entry);
			ByFile.Add(Key, U);
		}
		State.Set(TEXT("files"), Files);
		State.Set(TEXT("archives"), ArchiveState);

		// ---- 3. index next to the install, dependencies ----
		FConvPackageManager Mgr(Install);
		// packages the caller provides from elsewhere count as stock: used, never converted or removed by this state
		Mgr.AddPackageFiles(Options.ReferencePackages);
		TSet<FString> StockNames;
		for (const TPair<FString, FString>& KV : Mgr.FileIndex)
		{
			StockNames.Add(KV.Key);
		}
		TArray<FString> PkgFiles;
		TMap<FString, const FUserFile*> UserPkgs; // lower package name -> file
		TMap<FString, FString> MetaInt, MetaUcl;  // lower map -> file
		TArray<const FUserFile*> Music;
		TMap<FString, TMap<FString, FString>> UclRecords;
		FConvJson ScriptPackages = FConvJson::Object();
		for (const TPair<FString, FUserFile>& KV : ByFile)
		{
			const FUserFile& U = KV.Value;
			if (IsPackageExt(U.Ext))
			{
				PkgFiles.Add(U.Path);
			}
			else if (U.Ext == TEXT(".int"))
			{
				MetaInt.Add(U.Name.ToLower(), U.Path);
			}
			else if (U.Ext == TEXT(".ucl"))
			{
				ReadUcl(U.Path, UclRecords);
				MetaUcl.Add(U.Name.ToLower(), U.Path);
			}
			else if (U.Ext == TEXT(".ogg"))
			{
				Music.Add(&U);
			}
			else if (U.Ext == TEXT(".umx"))
			{
				Problem(FPaths::GetCleanFilename(U.Path), TEXT("is Unreal Tournament (1999) tracker music (.umx), which UT2004 maps do not use"), TEXT("info"));
			}
			else if (U.Ext == TEXT(".upl"))
			{
				Problem(FPaths::GetCleanFilename(U.Path), TEXT("is a character list (.upl); custom characters are not supported yet"), TEXT("info"));
			}
		}
		PkgFiles.Sort();
		TArray<FString> Shadowed;
		Mgr.AddPackageFiles(PkgFiles, &Shadowed);
		TSet<FString> ShadowedMaps;
		for (const FString& F : Shadowed)
		{
			const FString Name = FPaths::GetBaseFilename(F);
			const FString StockFile = Mgr.FindFile(Name);
			const bool bSame = PF.FileSize(*F) == PF.FileSize(*StockFile);
			const bool bMap = FPaths::GetExtension(F, true).ToLower() == TEXT(".ut2");
			if (bMap)
			{
				ShadowedMaps.Add(Name.ToLower());
			}
			if (bSame)
			{
				Problem(FPaths::GetCleanFilename(F), TEXT("is already part of UT2004 (the same file); ignored"), TEXT("info"));
			}
			else if (bMap)
			{
				Problem(FPaths::GetCleanFilename(F), FString::Printf(TEXT("has the same name as the UT2004 map %s; rename the file (and only the file) to play it"), *Name));
			}
			else
			{
				Problem(FPaths::GetCleanFilename(F), FString::Printf(TEXT("has the same name as a UT2004 package; UT2004's own %s is used"), *FPaths::GetCleanFilename(StockFile)), TEXT("info"));
			}
		}
		for (const FString& F : PkgFiles)
		{
			const FString Key = FPaths::GetBaseFilename(F).ToLower();
			if (Mgr.ExtraPackages.Contains(Key))
			{
				UserPkgs.Add(Key, &ByFile[FPaths::GetCleanFilename(F).ToLower()]);
			}
		}

		struct FPkgInfo
		{
			TArray<FString> Imports;  // lower top-level packages
			/** Lower top-level packages this package imports classes from (actor / object classes it needs to work). */
			TSet<FString> ClassImports;
			FString Error;
			bool bScript = false;
			TMap<FString, int32> ScriptClasses; // maps only
		};
		TMap<FString, FPkgInfo> Infos;
		TArray<FString> UserPkgNames;
		UserPkgs.GetKeys(UserPkgNames);
		UserPkgNames.Sort();
		for (int32 i = 0; i < UserPkgNames.Num(); ++i)
		{
			const FString& Key = UserPkgNames[i];
			const FUserFile& U = *UserPkgs[Key];
			FPkgInfo Info;
			Info.bScript = U.Ext == TEXT(".u");
			FConvPackage* Pkg = Mgr.LoadPackage(Key);
			if (!Pkg)
			{
				Info.Error = TEXT("cannot be read (damaged, or not a UT2004 package)");
				Infos.Add(Key, Info);
				continue;
			}
			auto TopOf = [Pkg](int32 ImportIdx) -> FString
			{
				int32 Idx = ImportIdx;
				for (int32 Guard = 0; Guard < 64 && Pkg->Imports.IsValidIndex(Idx); ++Guard)
				{
					const int32 Outer = Pkg->Imports[Idx].PackageIndex;
					if (Outer == 0)
					{
						return Pkg->Imports[Idx].Name;
					}
					if (Outer > 0)
					{
						return FString();
					}
					Idx = -Outer - 1;
				}
				return FString();
			};
			for (int32 Ii = 0; Ii < Pkg->Imports.Num(); ++Ii)
			{
				const FConvImport& Im = Pkg->Imports[Ii];
				if (Im.PackageIndex == 0 && !Im.Name.IsEmpty())
				{
					Info.Imports.AddUnique(Im.Name.ToLower());
				}
				else if (Im.ClassName == TEXT("Class"))
				{
					const FString Top = TopOf(Ii);
					if (!Top.IsEmpty())
					{
						Info.ClassImports.Add(Top.ToLower());
					}
				}
			}
			if (U.Ext == TEXT(".ut2"))
			{
				for (int32 Ei = 0; Ei < Pkg->Exports.Num(); ++Ei)
				{
					const int32 Ci = Pkg->Exports[Ei].ClassIndex;
					if (Ci > 0)
					{
						// an object whose class is defined inside the map (embedded script)
						Info.ScriptClasses.FindOrAdd(FString::Printf(TEXT("%s.%s"), *U.Name, *Pkg->RefName(Ci)))++;
					}
					else if (Ci < 0)
					{
						const FString Top = TopOf(-Ci - 1);
						const FUserFile* const* Owner = UserPkgs.Find(Top.ToLower());
						if (Owner && (*Owner)->Ext == TEXT(".u"))
						{
							Info.ScriptClasses.FindOrAdd(FString::Printf(TEXT("%s.%s"), *Top, *Pkg->RefName(Ci)))++;
						}
					}
				}
			}
			Infos.Add(Key, Info);
			if (Info.bScript)
			{
				FConvJson SP = FConvJson::Object();
				SP.Set(TEXT("name"), U.Name);
				SP.Set(TEXT("file"), U.Path);
				const FString* Int = MetaInt.Find(Key);
				SP.Set(TEXT("int"), Int ? FConvJson(*Int) : FConvJson());
				ScriptPackages.Set(U.Name, SP);
			}
		}
		Mgr.CloseAll();
		State.Set(TEXT("script_packages"), ScriptPackages);
		EndStage(TEXT("analyse"));

		// ---- 4. decide what to convert, remove what is gone ----
		const FConvJson& PrevMaps = Prev.Get(TEXT("maps"));
		const FConvJson& PrevPkgs = Prev.Get(TEXT("packages"));
		FConvJson Maps = FConvJson::Object();
		FConvJson Packages = FConvJson::Object();
		struct FTodo { FString Name; FString Signature; TArray<FString> UserDeps; };
		TArray<FTodo> Todo;
		TSet<FString> ChangedPkgs; // lower names whose cache folders must go
		for (const FString& Key : UserPkgNames)
		{
			const FUserFile& U = *UserPkgs[Key];
			if (U.Ext == TEXT(".ut2"))
			{
				continue;
			}
			FConvJson Rec = FConvJson::Object();
			Rec.Set(TEXT("name"), U.Name);
			Rec.Set(TEXT("file"), U.Path);
			Rec.Set(TEXT("md5"), U.Md5);
			if (!U.Source.IsEmpty())
			{
				Rec.Set(TEXT("archive"), U.Source);
			}
			const FConvJson* Old = PrevPkgs.Find(U.Name);
			if (!bReuse || !Old || Old->GetString(TEXT("md5")) != U.Md5)
			{
				ChangedPkgs.Add(Key);
			}
			if (!Infos[Key].Error.IsEmpty())
			{
				Problem(FPaths::GetCleanFilename(U.Path), Infos[Key].Error);
			}
			Packages.Set(U.Name, Rec);
		}
		for (const TPair<FString, FConvJson>& KV : PrevPkgs.O)
		{
			if (!UserPkgs.Contains(KV.Key.ToLower()))
			{
				ChangedPkgs.Add(KV.Key.ToLower());
			}
		}

		TArray<FString> MapKeys;
		for (const FString& Key : UserPkgNames)
		{
			if (UserPkgs[Key]->Ext == TEXT(".ut2"))
			{
				MapKeys.Add(Key);
			}
		}
		auto MapRecord = [&](const FUserFile& U) -> FConvJson
		{
			FConvJson Rec = FConvJson::Object();
			Rec.Set(TEXT("name"), U.Name);
			Rec.Set(TEXT("file"), U.Path);
			if (!U.Source.IsEmpty())
			{
				Rec.Set(TEXT("archive"), U.Source);
			}
			return Rec;
		};
		for (const FString& Key : MapKeys)
		{
			const FUserFile& U = *UserPkgs[Key];
			const FPkgInfo& Info = Infos[Key];
			FConvJson Rec = MapRecord(U);
			if (!Info.Error.IsEmpty())
			{
				Rec.Set(TEXT("status"), TEXT("unplayable"));
				FConvJson Pr = FConvJson::Array();
				Pr.Add(FString::Printf(TEXT("The map file %s"), *Info.Error));
				Rec.Set(TEXT("problems"), Pr);
				Maps.Set(U.Name, Rec);
				continue;
			}
			// custom packages it needs (transitively) and what is missing
			TArray<FString> UserDeps, Missing, Unreadable;
			TArray<FString> Stack = Info.Imports;
			TSet<FString> Seen;
			Seen.Add(Key);
			while (Stack.Num())
			{
				const FString D = Stack.Pop();
				if (Seen.Contains(D))
				{
					continue;
				}
				Seen.Add(D);
				if (Mgr.FindFile(D).IsEmpty())
				{
					Missing.Add(D);
					continue;
				}
				if (const FPkgInfo* DI = Infos.Find(D))
				{
					UserDeps.Add(D);
					if (!DI->Error.IsEmpty())
					{
						Unreadable.Add(FPaths::GetCleanFilename(UserPkgs[D]->Path));
					}
					Stack.Append(DI->Imports);
				}
			}
			UserDeps.Sort();
			Missing.Sort();
			// imports keep the names as the map wrote them: report them that way
			TArray<FString> MissingNames;
			{
				FConvPackage* Pkg = Mgr.LoadPackage(Key);
				for (const FString& M : Missing)
				{
					FString Shown = M;
					if (Pkg)
					{
						for (const FConvImport& Im : Pkg->Imports)
						{
							if (Im.PackageIndex == 0 && Im.Name.ToLower() == M)
							{
								Shown = Im.Name;
								break;
							}
						}
					}
					MissingNames.Add(Shown);
				}
				Mgr.CloseAll();
			}
			FConvJson Deps = FConvJson::Array();
			FString Signature = FString::Printf(TEXT("v%d.%d|%s"), StateVersion, ArenaConverterVersions::MapSchemaVersion, *U.Md5);
			for (const FString& D : UserDeps)
			{
				Deps.Add(UserPkgs[D]->Name);
				Signature += TEXT("|") + D + TEXT("=") + UserPkgs[D]->Md5;
			}
			for (const TMap<FString, FString>* Meta : { &MetaInt, &MetaUcl })
			{
				if (const FString* F = Meta->Find(Key))
				{
					Signature += TEXT("|meta=") + ByFile[FPaths::GetCleanFilename(*F).ToLower()].Md5;
				}
			}
			Rec.Set(TEXT("uses"), Deps);
			Rec.Set(TEXT("signature"), Signature);
			FConvJson Scripts = FConvJson::Array();
			TArray<FString> ClassNames;
			Info.ScriptClasses.GetKeys(ClassNames);
			ClassNames.Sort();
			for (const FString& C : ClassNames)
			{
				FConvJson E = FConvJson::Object();
				E.Set(TEXT("class"), C);
				E.Set(TEXT("count"), Info.ScriptClasses[C]);
				Scripts.Add(E);
			}
			Rec.Set(TEXT("script_classes"), Scripts);
			if (ShadowedMaps.Contains(Key))
			{
				continue; // reported above; the UT2004 map of that name is the one in the cache
			}
			// A missing package the map (or a custom package it uses) takes classes from: those objects cannot exist, so
			// the map cannot be played. Packages it only takes sounds, textures or meshes from are left out instead: UT2004
			// itself plays such a map on a server that has the package and just lacks those parts on clients without it
			// (e.g. a map with a few UT99 AmbOutside sounds).
			TArray<FString> MissingParts;
			bool bMissingCode = false;
			for (int32 i = 0; i < Missing.Num(); ++i)
			{
				bool bCode = Info.ClassImports.Contains(Missing[i]);
				for (const FString& D : UserDeps)
				{
					const FPkgInfo* DI = Infos.Find(D);
					bCode |= DI && DI->ClassImports.Contains(Missing[i]);
				}
				bMissingCode |= bCode;
				MissingParts.Add(MissingNames[i]);
			}
			if (!bMissingCode && !Unreadable.Num() && MissingParts.Num())
			{
				FConvJson M = FConvJson::Array();
				for (const FString& N : MissingNames)
				{
					M.Add(N);
				}
				Rec.Set(TEXT("missing"), M);
				FConvJson Pr = FConvJson::Array();
				Pr.Add(FString::Printf(TEXT("Missing %s: %s. The sounds, textures or meshes the map uses from %s are left out."),
					MissingParts.Num() == 1 ? TEXT("package") : TEXT("packages"), *JoinNames(MissingParts), MissingParts.Num() == 1 ? TEXT("it") : TEXT("them")));
				Rec.Set(TEXT("problems"), Pr);
				// converted again once the package turns up
				Signature += TEXT("|missing=") + FString::Join(Missing, TEXT(","));
				Rec.Set(TEXT("signature"), Signature);
			}
			else if (Missing.Num() || Unreadable.Num())
			{
				FConvJson M = FConvJson::Array();
				for (const FString& N : MissingNames)
				{
					M.Add(N);
				}
				Rec.Set(TEXT("missing"), M);
				Rec.Set(TEXT("status"), TEXT("unplayable"));
				FConvJson Pr = FConvJson::Array();
				if (Missing.Num())
				{
					Pr.Add(FString::Printf(TEXT("Missing %s: %s. Put %s in the custom content folder."), Missing.Num() == 1 ? TEXT("package") : TEXT("packages"),
						*JoinNames(MissingNames), Missing.Num() == 1 ? TEXT("it") : TEXT("them")));
				}
				if (Unreadable.Num())
				{
					Pr.Add(FString::Printf(TEXT("Damaged %s: %s."), Unreadable.Num() == 1 ? TEXT("package") : TEXT("packages"), *JoinNames(Unreadable)));
				}
				Rec.Set(TEXT("problems"), Pr);
				Maps.Set(U.Name, Rec);
				continue;
			}
			bool bChangedDep = false;
			for (const FString& D : UserDeps)
			{
				bChangedDep |= ChangedPkgs.Contains(D);
			}
			const FConvJson* Old = PrevMaps.Find(U.Name);
			const bool bUnchanged = bReuse && Old && !bChangedDep && Old->GetString(TEXT("signature")) == Signature
				&& Old->GetString(TEXT("status")) == TEXT("ok")
				&& (Old->GetNumber(TEXT("errors"), 0) == 0 || Old->GetNumber(TEXT("error_retry"), 0) >= ErrorRetryVersion)
				&& MapJsonSchema(Out / TEXT("maps") / FConvExporter::Safe(U.Name) / TEXT("map.json")) >= ArenaConverterVersions::MapSchemaVersion
				&& FM.FileExists(*(Out / TEXT("data") / (U.Name + TEXT(".json"))));
			if (bUnchanged)
			{
				Maps.Set(U.Name, *Old);
				++S.UnchangedMaps;
				continue;
			}
			Maps.Set(U.Name, Rec);
			Todo.Add({ U.Name, Signature, UserDeps });
		}

		// removals: changed / deleted packages, maps that are gone, re-converted or now unplayable
		TSet<FString> RemovedPkgs;
		for (const FString& K : ChangedPkgs)
		{
			if (!StockNames.Contains(K))
			{
				RemoveAssetFolders(Out, K);
				RemovedPkgs.Add(K);
			}
		}
		for (const TPair<FString, FConvJson>& KV : PrevMaps.O)
		{
			const FConvJson* Now = Maps.Find(KV.Key);
			const bool bKeep = Now && Now->GetString(TEXT("status")) == TEXT("ok") && !Todo.ContainsByPredicate([&KV](const FTodo& T) { return T.Name == KV.Key; });
			if (!bKeep && !StockNames.Contains(KV.Key.ToLower()))
			{
				RemoveMapOutputs(Out, KV.Key);
				RemovedPkgs.Add(KV.Key.ToLower());
				if (!Now)
				{
					S.RemovedMaps.Add(KV.Key);
				}
			}
		}
		for (const FTodo& T : Todo)
		{
			RemoveMapOutputs(Out, T.Name);
			RemovedPkgs.Add(T.Name.ToLower());
		}
		// music: gone or changed custom songs
		TSet<FString> StockMusic;
		{
			FConvExporter Probe(Mgr, FConvExporter::FOptions());
			for (const FString& F : Probe.MusicFiles())
			{
				StockMusic.Add(FPaths::GetBaseFilename(F).ToLower());
			}
		}
		FConvJson MusicState = FConvJson::Object();
		TArray<const FUserFile*> MusicTodo;
		for (const FUserFile* M : Music)
		{
			if (StockMusic.Contains(M->Name.ToLower()))
			{
				Problem(FPaths::GetCleanFilename(M->Path), TEXT("has the same name as a UT2004 song; UT2004's song is used"), TEXT("info"));
				continue;
			}
			const FConvJson* Old = Prev.Get(TEXT("music")).Find(M->Name);
			if (!bReuse || !Old || Old->S != M->Md5 || !FM.FileExists(*(Out / TEXT("music") / (FConvExporter::Safe(M->Name) + TEXT(".ogg")))))
			{
				MusicTodo.Add(M);
			}
			MusicState.Set(M->Name, M->Md5);
		}
		for (const TPair<FString, FConvJson>& KV : Prev.Get(TEXT("music")).O)
		{
			const FConvJson* Now = MusicState.Find(KV.Key);
			if ((!Now || Now->S != KV.Value.S) && !StockMusic.Contains(KV.Key.ToLower()))
			{
				FM.Delete(*(Out / TEXT("music") / (FConvExporter::Safe(KV.Key) + TEXT(".ogg"))), false, true, true);
				FM.Delete(*(Out / TEXT("music") / (FConvExporter::Safe(KV.Key) + TEXT(".json"))), false, true, true);
			}
		}
		State.Set(TEXT("music"), MusicState);

		// ---- 5. convert ----
		FConvExporter::FOptions ExOpt;
		ExOpt.OutDir = Out;
		ExOpt.bSkipExisting = true;
		ExOpt.bBackgroundTasks = !IsInGameThread();
		FConvExporter Ex(Mgr, ExOpt);
		Ex.CancelFlag = const_cast<TAtomic<bool>*>(Cancel);
		const FString ErrLog = UcDir / TEXT("errors.log");
		FString ErrTextPre;
		// converted maps (stock) of an older map schema: export them again with the current exporter
		TSet<FString> UserMapNames;
		for (const FString& K : MapKeys)
		{
			UserMapNames.Add(FConvExporter::Safe(UserPkgs[K]->Name).ToLower());
		}
		for (const TPair<FString, FConvJson>& KV : PrevMaps.O)
		{
			UserMapNames.Add(FConvExporter::Safe(KV.Key).ToLower());
		}
		TSet<FString> Skip = SkipSet(Prev);
		Skip.Append(UserMapNames);
		const TArray<FString> Outdated = Options.bMaintenance ? OutdatedMaps(Out, Skip) : TArray<FString>();
		FConvJson SchemaFailed = FConvJson::Object();
		for (const TPair<FString, FConvJson>& KV : Prev.Get(TEXT("schema_failed")).O)
		{
			if (int32(KV.Value.AsNumber()) >= ArenaConverterVersions::MapSchemaVersion)
			{
				SchemaFailed.Set(KV.Key, KV.Value);
			}
		}
		P.Total = Todo.Num() + MusicTodo.Num() + Outdated.Num();
		TArray<FString> UpdatedMaps;
		for (int32 i = 0; i < Outdated.Num(); ++i)
		{
			if (Cancelled())
			{
				break;
			}
			const FString& M = Outdated[i];
			Report(TEXT("Updating converted maps..."), M, 0.2f + 0.7f * float(i) / float(FMath::Max(1, Outdated.Num() + Todo.Num())));
			const FString Src = Mgr.FindFile(M);
			const bool bOk = !Src.IsEmpty() && !Mgr.ExtraPackages.Contains(M.ToLower()) && Ex.ExportMap(M);
			Mgr.CloseAll();
			for (const FString& E : Ex.TakeNewErrors())
			{
				ErrTextPre += M + TEXT("\t") + E + TEXT("\n");
			}
			Ex.TakeNewNotes();
			if (bOk && MapJsonSchema(Out / TEXT("maps") / M / TEXT("map.json")) >= ArenaConverterVersions::MapSchemaVersion)
			{
				UpdatedMaps.Add(M);
			}
			else
			{
				// not in the install any more, or the export failed: do not try again on every start
				SchemaFailed.Set(M, ArenaConverterVersions::MapSchemaVersion);
				UE_LOG(LogOpenUT2k4, Warning, TEXT("Custom content: could not update %s to map schema %d"), *M, ArenaConverterVersions::MapSchemaVersion);
			}
			++P.Done;
		}
		Ex.Flush();
		S.UpdatedMaps = UpdatedMaps;
		// HUD fonts missing from an older cache (ArenaConverterVersions::FontsVersion): export only those
		if (!Cancelled() && Options.bMaintenance && ArenaConvFonts::NeedsUpdate(Out))
		{
			Report(TEXT("Updating fonts"), FString(), 0.2f);
			ArenaConvFonts::ExportAll(Mgr, Ex, Out, Cancel);
			for (const FString& E : Ex.TakeNewErrors())
			{
				ErrTextPre += TEXT("fonts\t") + E + TEXT("\n");
			}
			Ex.TakeNewNotes();
		}
		// localization files missing from an older cache (ArenaConverterVersions::LocalizationVersion): copy only those
		if (!Cancelled() && Options.bMaintenance && ArenaConvLocalization::NeedsUpdate(Out) && ArenaContent::IsUT2004Install(Options.InstallDir))
		{
			Report(TEXT("Updating language files"), FString(), 0.2f);
			ArenaConvLocalization::CopyAll(FPaths::ConvertRelativePathToFull(Options.InstallDir), Out);
		}
		// script packages missing from an older cache (ArenaConverterVersions::ScriptsVersion): copy only those
		if (!Cancelled() && Options.bMaintenance && ArenaConvScripts::NeedsUpdate(Out) && ArenaContent::IsUT2004Install(Options.InstallDir))
		{
			Report(TEXT("Updating script packages"), FString(), 0.2f);
			ArenaConvScripts::CopyAll(FPaths::ConvertRelativePathToFull(Options.InstallDir), Out, Cancel);
		}
		State.Set(TEXT("schema_failed"), SchemaFailed);
		if (UpdatedMaps.Num())
		{
			UE_LOG(LogOpenUT2k4, Log, TEXT("Custom content: updated %d converted maps to map schema %d"), UpdatedMaps.Num(), ArenaConverterVersions::MapSchemaVersion);
		}
		for (const FUserFile* M : MusicTodo)
		{
			Report(TEXT("Converting custom music"), M->Name, 0.2f);
			Ex.ExportMusicFile(M->Path);
			++P.Done;
		}
		FString ErrText = ErrTextPre;
		TArray<FString> OkMaps;
		for (int32 i = 0; i < Todo.Num(); ++i)
		{
			if (Cancelled())
			{
				break;
			}
			const FTodo& T = Todo[i];
			Report(TEXT("Converting custom maps"), T.Name, 0.2f + 0.7f * float(Outdated.Num() + i) / float(FMath::Max(1, Outdated.Num() + Todo.Num())));
			const double TM = FPlatformTime::Seconds();
			const bool bExported = Ex.ExportMap(T.Name);
			Mgr.CloseAll();
			Ex.Flush();
			const TArray<FString> Errors = Ex.TakeNewErrors();
			Ex.TakeNewNotes();
			for (const FString& E : Errors)
			{
				ErrText += T.Name + TEXT("\t") + E + TEXT("\n");
			}
			S.Errors += Errors.Num();
			FConvJson& Rec = *Maps.Find(T.Name);
			Rec.Set(TEXT("seconds"), FConvJson::RoundTo(FPlatformTime::Seconds() - TM, 2));
			Rec.Set(TEXT("converted"), FDateTime::UtcNow().ToIso8601());
			Rec.Set(TEXT("errors"), Errors.Num());
			Rec.Set(TEXT("error_retry"), ErrorRetryVersion);
			const FString MapJson = Out / TEXT("maps") / FConvExporter::Safe(T.Name) / TEXT("map.json");
			FConvJson Doc;
			// notes from the scan (missing packages whose parts are left out) stay with the map
			FConvJson Pr = Rec.Get(TEXT("problems")).Type == FConvJson::EType::Array ? Rec.Get(TEXT("problems")) : FConvJson::Array();
			if (!bExported || !FConvJson::LoadFile(MapJson, Doc))
			{
				Rec.Set(TEXT("status"), TEXT("unplayable"));
				Pr.Add(FString::Printf(TEXT("Could not be converted%s%s"), Errors.Num() ? TEXT(": ") : TEXT("."), Errors.Num() ? *Errors[0].Replace(TEXT("\t"), TEXT(" ")) : TEXT("")));
				Rec.Set(TEXT("problems"), Pr);
				RemoveMapOutputs(Out, T.Name);
				++S.UnplayableMaps;
				continue;
			}
			// metadata: LevelInfo / LevelSummary, then the .int and .ucl files
			const FConvJson& Sum = Doc.Get(TEXT("summary"));
			FString Title = Sum.GetString(TEXT("title")), Author = Sum.GetString(TEXT("author")), Desc = Sum.GetString(TEXT("description"));
			int32 PMin = 0, PMax = 0;
			const FConvJson& Ideal = Sum.Get(TEXT("ideal_player_count"));
			if (Ideal.Type == FConvJson::EType::Array && Ideal.A.Num() == 2)
			{
				PMin = int32(Ideal.A[0].AsNumber());
				PMax = int32(Ideal.A[1].AsNumber());
			}
			if (const TMap<FString, FString>* Ucl = UclRecords.Find(T.Name.ToLower()))
			{
				if (const FString* V = Ucl->Find(TEXT("author")); V && !V->IsEmpty()) Author = *V;
				if (const FString* V = Ucl->Find(TEXT("fallbackname")); V && !V->IsEmpty()) Title = *V;
				if (const FString* V = Ucl->Find(TEXT("fallbackdesc")); V && !V->IsEmpty()) Desc = *V;
				if (const FString* V = Ucl->Find(TEXT("playercountmin")); V && FCString::Atoi(**V) > 0) PMin = FCString::Atoi(**V);
				if (const FString* V = Ucl->Find(TEXT("playercountmax")); V && FCString::Atoi(**V) > 0) PMax = FCString::Atoi(**V);
			}
			if (const FString* IntFile = MetaInt.Find(T.Name.ToLower()))
			{
				TMap<FString, FString> Int;
				ReadInt(*IntFile, Int);
				if (const FString* V = Int.Find(TEXT("title")); V && !V->IsEmpty()) Title = *V;
				if (const FString* V = Int.Find(TEXT("description")); V && !V->IsEmpty()) Desc = *V;
				if (const FString* V = Int.Find(TEXT("author")); V && !V->IsEmpty()) Author = *V;
			}
			Rec.Set(TEXT("title"), Title.IsEmpty() ? T.Name : Title);
			Rec.Set(TEXT("author"), Author);
			Rec.Set(TEXT("description"), Desc);
			FConvJson Players = FConvJson::Array();
			Players.Add(PMin);
			Players.Add(PMax);
			Rec.Set(TEXT("players"), Players);
			// screenshot: the LevelInfo Screenshot material's first texture
			const FString Shot = Sum.GetString(TEXT("screenshot"));
			FString ShotTex;
			int32 Dot;
			if (Shot.FindChar(TEXT('.'), Dot))
			{
				const FString Pkg = FConvExporter::Safe(Shot.Left(Dot)), Rest = FConvExporter::Safe(Shot.Mid(Dot + 1));
				FConvJson M;
				if (FM.FileExists(*(Out / TEXT("textures") / Pkg / (Rest + TEXT(".png")))))
				{
					ShotTex = Shot;
				}
				else if (FConvJson::LoadFile(Out / TEXT("materials") / Pkg / (Rest + TEXT(".json")), M) && FM.FileExists(*(Out / M.GetString(TEXT("base_texture_file")))))
				{
					ShotTex = M.GetString(TEXT("base_texture"));
				}
			}
			Rec.Set(TEXT("screenshot"), FConvJson::StrOrNull(ShotTex));
			Rec.Set(TEXT("game_type_prefix"), Doc.Get(TEXT("game_type_prefix")));
			const int32 Starts = Doc.Get(TEXT("markers")).Get(TEXT("player_starts")).Num();
			Rec.Set(TEXT("player_starts"), Starts);
			if (Starts == 0)
			{
				Rec.Set(TEXT("status"), TEXT("unplayable"));
				Pr.Add(TEXT("The map has no player starts, so nobody can spawn in it."));
				Rec.Set(TEXT("problems"), Pr);
				RemoveMapOutputs(Out, T.Name);
				++S.UnplayableMaps;
				continue;
			}
			if (Errors.Num())
			{
				Pr.Add(FString::Printf(TEXT("%d %s could not be converted (details in %s)."), Errors.Num(), Errors.Num() == 1 ? TEXT("part") : TEXT("parts"), *ErrLog));
			}
			Rec.Set(TEXT("problems"), Pr);
			Rec.Set(TEXT("status"), TEXT("ok"));
			OkMaps.Add(T.Name);
			S.ConvertedMaps.Add(T.Name);
			UE_LOG(LogOpenUT2k4, Log, TEXT("Custom content: converted %s in %.2fs (%d errors)"), *T.Name, FPlatformTime::Seconds() - TM, Errors.Num());
		}
		Ex.Flush();
		EndStage(TEXT("convert"));
		if (Cancelled())
		{
			// what was converted stays; the next scan converts the rest (state not saved: those maps count as new)
			for (const FString& M : OkMaps)
			{
				RemoveMapOutputs(Out, M);
			}
			if (UpdatedMaps.Num())
			{
				// updated stock maps are done (their schema is current): keep their marker files in step
				ArenaConvData::FResult DR;
				ArenaConvData::WriteGameData(Out, DR, &UpdatedMaps);
			}
			S.bCancelled = true;
			return false;
		}
		if (!ErrText.IsEmpty())
		{
			FFileHelper::SaveStringToFile(ErrText, *ErrLog, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
		}

		// ---- 6. cache-side data ----
		Report(TEXT("Updating the map list"), FString(), 0.92f);
		ArenaConvData::FResult DR;
		TMap<FString, FConvJson> NewStats = Ex.TakeTextureStats();
		if (OkMaps.Num() || UpdatedMaps.Num())
		{
			TSet<FString> Touched;
			for (const FString& M : UpdatedMaps)
			{
				Touched.Add(M.ToLower());
			}
			for (const FString& M : OkMaps)
			{
				Touched.Add(FConvExporter::Safe(M).ToLower());
				for (const FTodo& T : Todo)
				{
					if (T.Name == M)
					{
						for (const FString& D : T.UserDeps)
						{
							Touched.Add(FConvExporter::Safe(UserPkgs[D]->Name).ToLower());
						}
					}
				}
			}
			ArenaConvData::BakeCombiners(Out, NewStats, DR, &Touched);
		}
		if (NewStats.Num() || RemovedPkgs.Num())
		{
			ArenaConvData::MergeTextureStats(Out, NewStats, RemovedPkgs, DR);
		}
		if (OkMaps.Num() || MusicTodo.Num() || UpdatedMaps.Num())
		{
			TArray<FString> DataMaps = OkMaps;
			DataMaps.Append(UpdatedMaps);
			ArenaConvData::WriteGameData(Out, DR, &DataMaps);
		}
		for (const FString& E : DR.Errors)
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("Custom content: %s"), *E);
		}
		EndStage(TEXT("data"));

		// ---- 7. state ----
		int32 Unplayable = 0;
		for (const TPair<FString, FConvJson>& KV : Maps.O)
		{
			Unplayable += KV.Value.GetString(TEXT("status")) != TEXT("ok") ? 1 : 0;
		}
		S.UnplayableMaps = Unplayable;
		State.Set(TEXT("maps"), Maps);
		State.Set(TEXT("packages"), Packages);
		State.Set(TEXT("problems"), Problems);
		State.Set(TEXT("fingerprint"), Fingerprint);
		FConvJson Last = FConvJson::Object();
		Last.Set(TEXT("time"), FDateTime::UtcNow().ToIso8601());
		Last.Set(TEXT("seconds"), FConvJson::RoundTo(FPlatformTime::Seconds() - T0, 2));
		FConvJson Conv = FConvJson::Array();
		for (const FString& M : S.ConvertedMaps) Conv.Add(M);
		Last.Set(TEXT("converted"), Conv);
		FConvJson Rem = FConvJson::Array();
		for (const FString& M : S.RemovedMaps) Rem.Add(M);
		Last.Set(TEXT("removed"), Rem);
		Last.Set(TEXT("unchanged"), S.UnchangedMaps);
		FConvJson Upd = FConvJson::Array();
		for (const FString& M : S.UpdatedMaps) Upd.Add(M);
		Last.Set(TEXT("updated"), Upd);
		FConvJson Stages = FConvJson::Object();
		for (const TPair<FString, double>& KV : S.StageSeconds)
		{
			Stages.Set(KV.Key, FConvJson::RoundTo(KV.Value, 3));
		}
		Last.Set(TEXT("stages"), Stages);
		State.Set(TEXT("last_scan"), Last);
		if (!State.SaveToFile(StatePath(Out, Options.StateDirName), 1))
		{
			S.FatalError = TEXT("Cannot write the custom content list in the content cache.");
			return false;
		}
		S.Seconds = FPlatformTime::Seconds() - T0;
		S.bOk = true;
		Report(TEXT("Done"), FString(), 1.f);
		return true;
	}
#else
	bool Run(const FOptions& Options, TFunctionRef<void(const FProgress&)> OnProgress, const TAtomic<bool>* Cancel, FStats& S)
	{
		S.FatalError = TEXT("The content converter is not part of this build.");
		return false;
	}
#endif
}
