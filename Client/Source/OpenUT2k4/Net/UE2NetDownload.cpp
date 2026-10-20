#include "UE2NetDownload.h"

#include "../ArenaContent.h"
#include "../Convert/ArenaConverter.h"
#include "../ArenaLevelBuilder.h"
#include "../ArenaRuntimeContent.h"
#include "../ArenaUserContent.h"
#include "../Convert/ArenaConvJson.h"
#include "../Convert/ArenaConvPackage.h"
#include "../Convert/ArenaConvUserContent.h"
#include "../UScript/ArenaUScript.h"
#include "../UScript/UScriptRuntime.h"
#include "Async/Async.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UE2NetConnection.h"
#include "UE2NetLog.h"
#include "UE2NetPackageMap.h"

namespace UE2NetDownloadLocal
{
	/** UE2 EPackageFlags: the server may send the package to clients. */
	constexpr uint32 PKG_AllowDownload = 0x0001;
	/** Seconds without any data before a download is given up. */
	constexpr double StallTimeout = 30.0;
	/** Seconds the redirect has to answer at all (status line or data): a dead redirect host is given up quickly. */
	constexpr double RedirectConnectTimeout = 10.0;
	/** Seconds without data from a redirect that already answered before the request is given up. */
	constexpr double RedirectStallTimeout = 20.0;
	/** Seconds the server has to answer a file request at all (data or a refusal: normally one round trip). */
	constexpr double ReplyTimeout = 10.0;
	/** File requests in a row without any answer before the file channel is given up for this join. */
	constexpr int32 MaxNoReplyInRow = 3;
	/** Seconds of measured transfer before the remaining time is projected against the time limit. */
	constexpr double RateWarmup = 15.0;
	/** NETSPEED asked for while a file channel download runs (the server caps it with MaxClientRate). */
	constexpr int32 DownloadNetSpeed = 1000000;
	/** Largest uz2 chunk accepted (ucc writes 32768-byte chunks). */
	constexpr int32 MaxUz2Chunk = 1 << 20;

	const TCHAR* const AllowedExtensions[] = { TEXT("u"), TEXT("ut2"), TEXT("utx"), TEXT("usx"), TEXT("ukx"), TEXT("uax") };

	bool IsScriptFile(const FString& FileName)
	{
		return FPaths::GetExtension(FileName).Equals(TEXT("u"), ESearchCase::IgnoreCase);
	}

	FString IndexPath()
	{
		return UE2NetDownload::GetNetCacheDir() / TEXT("index.json");
	}

	/** Folder of one GUID: <net cache>/scripts/<GUID> for code packages, <net cache>/packages/<GUID> for the rest. */
	FString GuidDir(const FGuid& Guid, bool bScript)
	{
		return UE2NetDownload::GetNetCacheDir() / (bScript ? TEXT("scripts") : TEXT("packages")) / UE2GuidToString(Guid);
	}

	FString FormatMB(int64 Bytes)
	{
		return FString::Printf(TEXT("%.1f"), double(Bytes) / (1024.0 * 1024.0));
	}

	bool IsUnder(const FString& Path, const FString& Dir)
	{
		const FString P = FPaths::ConvertRelativePathToFull(Path);
		const FString D = FPaths::ConvertRelativePathToFull(Dir);
		return FPaths::IsUnderDirectory(P, D) && !P.Contains(TEXT(".."));
	}

	/** Records a stored or used package in index.json (GUID -> name, file, size, generations, times). */
	void TouchIndex(const FGuid& Guid, const FString& Name, const FString& File, int64 Size, int32 Generations, bool bNew)
	{
		FConvJson Index;
		if (!FConvJson::LoadFile(IndexPath(), Index) || Index.Type != FConvJson::EType::Object)
		{
			Index = FConvJson::Object();
			Index.Set(TEXT("version"), 1);
		}
		FConvJson Packages = Index.Get(TEXT("packages"));
		if (Packages.Type != FConvJson::EType::Object)
		{
			Packages = FConvJson::Object();
		}
		const FString Key = UE2GuidToString(Guid);
		FConvJson Rec = Packages.Get(Key);
		if (Rec.Type != FConvJson::EType::Object)
		{
			Rec = FConvJson::Object();
		}
		const FString Now = FDateTime::UtcNow().ToIso8601();
		Rec.Set(TEXT("name"), Name);
		Rec.Set(TEXT("file"), File.Mid(UE2NetDownload::GetNetCacheDir().Len() + 1));
		Rec.Set(TEXT("size"), Size);
		Rec.Set(TEXT("generations"), Generations);
		if (bNew || !Rec.Has(TEXT("downloaded")))
		{
			Rec.Set(TEXT("downloaded"), Now);
		}
		Rec.Set(TEXT("last_used"), Now);
		Packages.Set(Key, Rec);
		Index.Set(TEXT("packages"), Packages);
		IFileManager::Get().MakeDirectory(*UE2NetDownload::GetNetCacheDir(), true);
		Index.SaveToFile(IndexPath(), 1);
	}

	/** Package files of the custom content folder (found by the converter, never converted by the download pass). */
	TArray<FString> UserContentPackages()
	{
		TArray<FString> Out;
		const FString UserDir = ArenaContent::GetUserContentDir();
		if (!UserDir.IsEmpty() && IFileManager::Get().DirectoryExists(*UserDir))
		{
			for (const TCHAR* Ext : { TEXT("*.u"), TEXT("*.ut2"), TEXT("*.utx"), TEXT("*.usx"), TEXT("*.uax"), TEXT("*.ukx") })
			{
				IFileManager::Get().FindFilesRecursive(Out, *UserDir, Ext, true, false, false);
			}
		}
		return Out;
	}
}

using namespace UE2NetDownloadLocal;

// =================================================================================================
// Net cache, names, uz2
// =================================================================================================

namespace UE2NetDownload
{
	FString GetNetCacheDir()
	{
		static const FString Dir = []()
		{
			FString Override;
			if (FParse::Value(FCommandLine::Get(), TEXT("UE2NetCache="), Override) && !Override.IsEmpty())
			{
				return FPaths::ConvertRelativePathToFull(Override);
			}
			return FPaths::ConvertRelativePathToFull(ArenaContent::GetUserDataDir() / TEXT("NetCache"));
		}();
		return Dir;
	}

	FString FindCachedPackage(const FGuid& Guid)
	{
		if (!Guid.IsValid())
		{
			return FString();
		}
		for (const bool bScript : { true, false })
		{
			const FString Dir = GuidDir(Guid, bScript);
			TArray<FString> Files;
			IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*")), true, false);
			for (const FString& F : Files)
			{
				const FString Path = Dir / F;
				const FString Ext = FPaths::GetExtension(F);
				bool bAllowed = false;
				for (const TCHAR* A : AllowedExtensions)
				{
					bAllowed |= Ext.Equals(A, ESearchCase::IgnoreCase);
				}
				FUE2PackageHeader Header;
				if (bAllowed && UE2ReadPackageHeader(Path, Header) && Header.Guid == Guid)
				{
					return Path;
				}
			}
		}
		return FString();
	}

	FString& JoinNotice()
	{
		static FString Notice;
		return Notice;
	}

	const FString& GetJoinNotice()
	{
		return JoinNotice();
	}

	void ClearJoinNotice()
	{
		JoinNotice().Reset();
	}

	FString GetMapProblem(const FString& MapName)
	{
		// the converter's state of downloaded maps, then of the custom content folder
		for (const TCHAR* StateDir : { TEXT("netcache"), TEXT("usercontent") })
		{
			FConvJson State;
			if (!FConvJson::LoadFile(ArenaContent::GetCacheDir() / StateDir / TEXT("state.json"), State) || State.Type != FConvJson::EType::Object)
			{
				continue;
			}
			for (const TPair<FString, FConvJson>& KV : State.Get(TEXT("maps")).O)
			{
				if (!KV.Key.Equals(MapName, ESearchCase::IgnoreCase) || KV.Value.GetString(TEXT("status")) == TEXT("ok"))
				{
					continue;
				}
				TArray<FString> Problems;
				for (const FConvJson& P : KV.Value.Get(TEXT("problems")).A)
				{
					if (P.Type == FConvJson::EType::String && !P.S.IsEmpty())
					{
						Problems.Add(P.S);
					}
				}
				return Problems.Num() ? FString::Join(Problems, TEXT(" ")) : FString(TEXT("The map could not be converted."));
			}
		}
		return FString();
	}

	TSet<FString> ReadManifestPackages(const FString& ManifestPath)
	{
		TSet<FString> Names;
		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *ManifestPath))
		{
			return Names;
		}
		// "File=Maps\DM-Rankin.ut2" lines (any section); only package files count
		for (FString Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (!Line.StartsWith(TEXT("File="), ESearchCase::IgnoreCase))
			{
				continue;
			}
			FString Rel = Line.Mid(5).Replace(TEXT("\\"), TEXT("/"));
			const FString Ext = FPaths::GetExtension(Rel);
			for (const TCHAR* A : AllowedExtensions)
			{
				if (Ext.Equals(A, ESearchCase::IgnoreCase))
				{
					Names.Add(FPaths::GetBaseFilename(Rel).ToLower());
					break;
				}
			}
		}
		return Names;
	}

	TSet<FString> GetStockPackageNames(FString* OutSource)
	{
		TSet<FString> Names;
		IFileManager& FM = IFileManager::Get();
		// the installer's manifests: exactly the files UT2004 (with the Bonus Pack / ECE) installed
		const FString InstallDir = ArenaContent::FindUT2004Install();
		if (!InstallDir.IsEmpty())
		{
			TArray<FString> Inis;
			FM.FindFiles(Inis, *(InstallDir / TEXT("System") / TEXT("*.ini")), true, false);
			TArray<FString> Used;
			for (const FString& Ini : Inis)
			{
				const FString Path = InstallDir / TEXT("System") / Ini;
				bool bManifest = Ini.Equals(TEXT("Manifest.ini"), ESearchCase::IgnoreCase);
				if (!bManifest)
				{
					FString Text;
					// other installer manifests of Epic's products (MasterProduct=UT2004 in [Setup]); small files only
					if (FM.FileSize(*Path) < 4 * 1024 * 1024 && FFileHelper::LoadFileToString(Text, *Path))
					{
						bManifest = Text.Contains(TEXT("[Setup]")) && Text.Contains(TEXT("MasterProduct=UT2004")) && Text.Contains(TEXT("File="));
					}
				}
				if (bManifest)
				{
					const TSet<FString> FromManifest = ReadManifestPackages(Path);
					if (FromManifest.Num())
					{
						Names.Append(FromManifest);
						Used.Add(Ini);
					}
				}
			}
			if (Names.Num())
			{
				if (OutSource)
				{
					*OutSource = FString::Printf(TEXT("installer manifest (%s, %d packages)"), *FString::Join(Used, TEXT(", ")), Names.Num());
				}
				return Names;
			}
		}
		if (OutSource)
		{
			*OutSource = InstallDir.IsEmpty() ? FString(TEXT("content cache (no install found)")) : FString(TEXT("install folders and content cache (no installer manifest)"));
		}
		auto AddFolderNames = [&FM, &Names](const FString& Dir)
		{
			TArray<FString> Sub;
			FM.FindFiles(Sub, *(Dir / TEXT("*")), false, true);
			for (const FString& S : Sub)
			{
				Names.Add(S.ToLower());
			}
		};
		auto AddFileNames = [&FM, &Names](const FString& Dir, const TCHAR* Wildcard)
		{
			TArray<FString> Files;
			FM.FindFiles(Files, *(Dir / Wildcard), true, false);
			for (const FString& F : Files)
			{
				Names.Add(FPaths::GetBaseFilename(F).ToLower());
			}
		};
		// what the content cache was converted from
		const FString Cache = ArenaContent::GetCacheDir();
		for (const TCHAR* Kind : { TEXT("textures"), TEXT("materials"), TEXT("sounds"), TEXT("staticmeshes"), TEXT("skeletalmeshes"),
			TEXT("animations"), TEXT("vertmeshes"), TEXT("maps") })
		{
			AddFolderNames(Cache / Kind);
		}
		AddFileNames(Cache / TEXT("scripts") / TEXT("System"), TEXT("*.u"));
		AddFileNames(Cache / TEXT("scripts") / TEXT("Maps"), TEXT("*.ut2"));
		// the install's package folders
		const FString Install = ArenaContent::FindUT2004Install();
		if (!Install.IsEmpty())
		{
			const TPair<const TCHAR*, const TCHAR*> Folders[] = { { TEXT("System"), TEXT("*.u") }, { TEXT("Maps"), TEXT("*.ut2") },
				{ TEXT("Textures"), TEXT("*.utx") }, { TEXT("Sounds"), TEXT("*.uax") }, { TEXT("StaticMeshes"), TEXT("*.usx") },
				{ TEXT("Animations"), TEXT("*.ukx") } };
			for (const TPair<const TCHAR*, const TCHAR*>& F : Folders)
			{
				AddFileNames(Install / F.Key, F.Value);
			}
		}
		// custom content and earlier downloads were converted into the same cache: they are not stock
		for (const TCHAR* State : { TEXT("usercontent"), TEXT("netcache") })
		{
			FConvJson J;
			if (FConvJson::LoadFile(ArenaConvUserContent::StatePath(Cache, State), J))
			{
				for (const TCHAR* Key : { TEXT("packages"), TEXT("maps") })
				{
					for (const TPair<FString, FConvJson>& KV : J.Get(Key).O)
					{
						Names.Remove(KV.Key.ToLower());
					}
				}
			}
		}
		return Names;
	}

	double GetDownloadTimeLimit()
	{
		double Limit = 1800.0;
		FParse::Value(FCommandLine::Get(), TEXT("UE2NetDownloadTimeLimit="), Limit);
		return FMath::Max(10.0, Limit);
	}

	bool IsSafePackageFileName(const FString& FileName, const FString& PackageName)
	{
		if (FileName.IsEmpty() || FileName.Len() > 128 || PackageName.IsEmpty() || PackageName.Len() > 120)
		{
			return false;
		}
		for (const TCHAR C : FileName)
		{
			// any printable ASCII UT2004 package names use ("][", "{V][M}", "(v4Fix)", "R!P", spaces...), but never path
			// separators, drive colons, wildcards, quotes or controls
			const bool bOk = C >= 0x20 && C <= 0x7E && C != TEXT('/') && C != TEXT('\\') && C != TEXT(':') && C != TEXT('*')
				&& C != TEXT('?') && C != TEXT('"') && C != TEXT('<') && C != TEXT('>') && C != TEXT('|');
			if (!bOk)
			{
				return false;
			}
		}
		if (FileName.StartsWith(TEXT(".")) || FileName.StartsWith(TEXT(" ")) || FileName.EndsWith(TEXT(" ")) || FileName.Contains(TEXT("..")))
		{
			return false;
		}
		// exactly "<PackageName>.<ext>" with a package extension
		int32 Dot = INDEX_NONE;
		if (!FileName.FindLastChar(TEXT('.'), Dot) || !FileName.Left(Dot).Equals(PackageName, ESearchCase::IgnoreCase))
		{
			return false;
		}
		const FString Ext = FileName.Mid(Dot + 1);
		for (const TCHAR* A : AllowedExtensions)
		{
			if (Ext.Equals(A, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool DecompressUz2(const uint8* Data, int64 Size, int64 MaxOut, TArray<uint8>& Out, FString& OutError)
	{
		Out.Reset();
		int64 Pos = 0;
		auto I32 = [Data](int64 At) { return int32(uint32(Data[At]) | (uint32(Data[At + 1]) << 8) | (uint32(Data[At + 2]) << 16) | (uint32(Data[At + 3]) << 24)); };
		// the chunk headers declare the unpacked size: check it against the cap before unpacking anything
		int64 Declared = 0;
		for (int64 Scan = 0; Scan + 8 <= Size; )
		{
			const int32 C = I32(Scan), U = I32(Scan + 4);
			if (C <= 0 || U <= 0)
			{
				break; // reported by the unpacking loop
			}
			Declared += U;
			Scan += 8 + int64(C);
		}
		if (Declared > MaxOut)
		{
			OutError = FString::Printf(TEXT("the .uz2 file declares %s MB unpacked, more than the %s MB limit"), *FormatMB(Declared), *FormatMB(MaxOut));
			return false;
		}
		Out.Reserve(Declared);
		while (Pos < Size)
		{
			if (Size - Pos < 8)
			{
				OutError = TEXT("truncated .uz2 chunk header");
				return false;
			}
			const int32 Compressed = I32(Pos);
			const int32 Uncompressed = I32(Pos + 4);
			Pos += 8;
			if (Compressed <= 0 || Uncompressed <= 0 || Uncompressed > MaxUz2Chunk || Compressed > Size - Pos)
			{
				OutError = FString::Printf(TEXT("bad .uz2 chunk at byte %lld (sizes %d / %d)"), Pos - 8, Compressed, Uncompressed);
				return false;
			}
			if (int64(Out.Num()) + Uncompressed > MaxOut)
			{
				OutError = FString::Printf(TEXT("the unpacked file is larger than %s MB"), *FormatMB(MaxOut));
				return false;
			}
			const int64 At = Out.Num();
			Out.AddUninitialized(Uncompressed);
			if (!FCompression::UncompressMemory(NAME_Zlib, Out.GetData() + At, Uncompressed, Data + Pos, Compressed))
			{
				OutError = FString::Printf(TEXT("zlib error in the .uz2 chunk at byte %lld"), Pos - 8);
				return false;
			}
			Pos += Compressed;
		}
		if (Out.Num() == 0)
		{
			OutError = TEXT("empty .uz2 file");
			return false;
		}
		return true;
	}
}

// =================================================================================================
// Downloader
// =================================================================================================

struct FUE2NetDownloader::FHttpState
{
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request;
	bool bDone = false;
	bool bOk = false;
	int32 Status = 0;
	/** The redirect answered (a status line or data). */
	bool bResponded = false;
	int64 Received = 0;
	bool bTooBig = false;
	TArray<uint8> Content;
	FString Url;
};

struct FUE2NetDownloader::FConvertJob
{
	TAtomic<bool> bDone { false };
	TAtomic<bool> bCancel { false };
	FCriticalSection Lock;
	FString Stage;
	FString Item;
	float Fraction = 0.f;
	ArenaConvUserContent::FStats Stats;
	bool bOk = false;
};

FUE2NetDownloader::FUE2NetDownloader(FUE2NetConnection& InConnection)
	: Connection(InConnection)
{
}

FUE2NetDownloader::~FUE2NetDownloader()
{
	if (Http && Http->Request && !Http->bDone)
	{
		Http->Request->OnProcessRequestComplete().Unbind();
		Http->Request->OnRequestProgress64().Unbind();
		Http->Request->CancelRequest();
	}
	if (FileChannel)
	{
		Connection.CloseFileChannel(FileChannel);
	}
	Connection.OnFileData = nullptr;
	if (Convert)
	{
		// the worker keeps its own reference; it stops at the next check and its result is dropped
		Convert->bCancel = true;
	}
}

bool FUE2NetDownloader::Start()
{
	// download managers in the server's order of preference
	for (const FString& Line : Connection.GetDownloadManagers())
	{
		FString Class, Params;
		int32 Compression = 0;
		FParse::Value(*Line, TEXT("CLASS="), Class);
		FParse::Value(*Line, TEXT("PARAMS="), Params);
		FParse::Value(*Line, TEXT("COMPRESSION="), Compression);
		if (Class.Equals(TEXT("IpDrv.HTTPDownload"), ESearchCase::IgnoreCase) && RedirectURL.IsEmpty())
		{
			// only plain http(s) redirects
			if (Params.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase) || Params.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase))
			{
				RedirectURL = Params;
				bRedirectCompressed = Compression != 0;
			}
			else if (!Params.IsEmpty())
			{
				UE_LOG(LogUE2Net, Warning, TEXT("Download redirect \"%s\" is not an http(s) URL: not used"), *Params);
			}
		}
		else if (Class.Equals(TEXT("Engine.ChannelDownload"), ESearchCase::IgnoreCase))
		{
			bChannelAllowed = Params.Equals(TEXT("Enabled"), ESearchCase::IgnoreCase);
			bChannelCompressed = Compression != 0;
		}
	}

	FUE2PackageMap& Map = Connection.GetPackageMap();
	FString StockSource;
	const TSet<FString> Stock = UE2NetDownload::GetStockPackageNames(&StockSource);
	UE_LOG(LogUE2Net, Log, TEXT("UT2004 packages (never downloaded) from the %s"), *StockSource);
	UE2NetDownload::ClearJoinNotice();
	TArray<FString> CodeMismatch;
	for (const FUE2PackageInfo& Info : Map.GetList())
	{
		if (Info.bGuidMatch)
		{
			continue;
		}
		if (Stock.Contains(Info.Name.ToLower()))
		{
			// UT2004's own packages are never downloaded (servers do not offer them, and they come from the install):
			// missing means the install was not found, another GUID means another version
			StockMissing.Add(Info.Name);
			Failed.Add(Info.Name);
			const bool bCode = Info.FileName.IsEmpty() || IsScriptFile(Info.FileName) || IsScriptFile(Info.LocalPath);
			if (Info.bFound && bCode)
			{
				CodeMismatch.Add(Info.Name);
			}
			continue;
		}
		if (!(Info.PackageFlags & PKG_AllowDownload))
		{
			UE_LOG(LogUE2Net, Log, TEXT("Package %s is missing and the server does not allow downloading it"), *Info.Name);
			Failed.Add(Info.Name);
			continue;
		}
		if (!UE2NetDownload::IsSafePackageFileName(Info.FileName, Info.Name))
		{
			UE_LOG(LogUE2Net, Warning, TEXT("Package %s: the server's file name \"%s\" is not a safe package file name: not downloaded"), *Info.Name, *Info.FileName);
			Failed.Add(Info.Name);
			continue;
		}
		if (Info.FileSize <= 0 || Info.FileSize > UE2NetDownload::MaxPackageBytes)
		{
			UE_LOG(LogUE2Net, Warning, TEXT("Package %s: size %d is outside the download limit (%s MB): not downloaded"), *Info.Name, Info.FileSize,
				*FormatMB(UE2NetDownload::MaxPackageBytes));
			Failed.Add(Info.Name);
			continue;
		}
		if (Info.bFound)
		{
			// UT2004 clients treat another build of a package as missing too. A stock package of another build is never
			// offered by servers; a mod or map with the same name as one of ours is downloaded into the net cache.
			UE_LOG(LogUE2Net, Log, TEXT("Package %s: the local copy (%s) is another version; downloading the server's"), *Info.Name, *Info.LocalPath);
		}
		FItem Item;
		Item.Name = Info.Name;
		Item.FileName = Info.FileName;
		Item.Guid = Info.Guid;
		Item.Size = Info.FileSize;
		Item.Generation = Info.RemoteGeneration;
		BytesTotal += Item.Size;
		Queue.Add(MoveTemp(Item));
	}
	if (CodeMismatch.Num() > 0)
	{
		// the server's game code is another build: nothing it replicates would decode (UT2004 refuses this as well)
		Abort(FString::Printf(TEXT("Version mismatch: the server's %s %s another version than your UT2004 install's (the server runs another patch)"),
			*FString::Join(CodeMismatch, TEXT(", ")), CodeMismatch.Num() == 1 ? TEXT("is") : TEXT("are")));
		return true;
	}
	if (StockMissing.Num() > 0)
	{
		TArray<FString> Shown(StockMissing.GetData(), FMath::Min(8, StockMissing.Num()));
		UE_LOG(LogUE2Net, Warning, TEXT("%d UT2004 packages of the server are not in your UT2004 install, or are another version (install not found or version mismatch): %s%s. They are not downloaded; objects in them are left out."),
			StockMissing.Num(), *FString::Join(Shown, TEXT(", ")), StockMissing.Num() > Shown.Num() ? TEXT(", ...") : TEXT(""));
		UE2NetDownload::JoinNotice() = FString::Printf(TEXT("%d UT2004 %s missing or another version (install not found or mismatch)"),
			StockMissing.Num(), StockMissing.Num() == 1 ? TEXT("package") : TEXT("packages"));
	}
	if (Queue.Num() > 0 && RedirectURL.IsEmpty() && !bChannelAllowed)
	{
		UE_LOG(LogUE2Net, Warning, TEXT("%d missing packages, but the server offers no download method (AllowDownloads off, no redirect)"), Queue.Num());
		for (const FItem& I : Queue)
		{
			Failed.Add(I.Name);
		}
		Queue.Reset();
	}
	if (Queue.Num() > 0)
	{
		UE_LOG(LogUE2Net, Log, TEXT("Downloading %d packages (%s MB) before joining: redirect %s, file channel %s"), Queue.Num(), *FormatMB(BytesTotal),
			RedirectURL.IsEmpty() ? TEXT("none") : *RedirectURL, bChannelAllowed ? TEXT("allowed") : TEXT("off"));
		Connection.OnFileData = [this](int32 Channel, const TArray<uint8>& Data, bool bClose) { OnFileData(Channel, Data, bClose); };
		Phase = EPhase::Downloading;
		PassStart = LastTick = FPlatformTime::Seconds();
		StartNext(PassStart);
		return true;
	}
	// nothing to download; packages that came from the net cache earlier may still need their conversion, and their
	// code the script runtime
	RegisterScriptPackages();
	StartConvert();
	return Phase != EPhase::Done && Phase != EPhase::Idle;
}

void FUE2NetDownloader::RegisterScriptPackages()
{
	// code packages: the script runtime resolves imports between packages by name
	TArray<FString> Scripts;
	for (const FUE2PackageInfo& Info : Connection.GetPackageMap().GetList())
	{
		if (Info.bGuidMatch && IsScriptFile(Info.LocalPath) && IsUnder(Info.LocalPath, UE2NetDownload::GetNetCacheDir()))
		{
			Scripts.Add(Info.LocalPath);
		}
	}
	if (Scripts.Num() && ArenaUScript::EnsureRuntime())
	{
		FUSRuntime& RT = FUSRuntime::Get();
		RT.AddPackageFiles(Scripts);
		// open them now, so a package of the same name in the UT2004 folder (a mod of another version) is not
		// the one the runtime finds by name
		for (const FString& Path : Scripts)
		{
			FUSLinker* L = RT.LoadPackageFile(Path);
			if (L && L->Pkg && !FPaths::IsSamePath(L->Pkg->PathOnDisk, Path))
			{
				UE_LOG(LogUE2Net, Warning, TEXT("Script package %s was already loaded from %s: the downloaded version is not used until a restart"),
					*FPaths::GetBaseFilename(Path), *L->Pkg->PathOnDisk);
			}
		}
	}
}

void FUE2NetDownloader::StartNext(double Now)
{
	while (true)
	{
		Method = EMethod::None;
		CurrentBytes = 0;
		if (Current == INDEX_NONE)
		{
			Current = 0;
		}
		if (!Queue.IsValidIndex(Current))
		{
			if (bRaisedNetSpeed)
			{
				bRaisedNetSpeed = false;
				Connection.SetNetSpeed(Connection.GetSettings().NetSpeed);
			}
			Connection.OnFileData = nullptr;
			// the package map finds the new files by GUID
			Connection.GetPackageMap().MatchLocalPackages();
			RegisterScriptPackages();
			StartConvert();
			return;
		}
		FItem& Item = Queue[Current];
		ItemStart = Now;
		LastData = Now;
		if (!Item.bTriedHttp && !RedirectURL.IsEmpty() && !bRedirectDead && !FParse::Param(FCommandLine::Get(), TEXT("UE2NetNoRedirect")))
		{
			Item.bTriedHttp = true;
			if (StartHttp(Item))
			{
				return;
			}
		}
		if (!Item.bTriedChannel && bChannelAllowed && NoReplyInRow < MaxNoReplyInRow)
		{
			Item.bTriedChannel = true;
			if (StartChannel(Item))
			{
				return;
			}
		}
		UE_LOG(LogUE2Net, Warning, TEXT("Could not download %s: it is skipped"), *Item.FileName);
		Failed.Add(Item.Name);
		BytesDone += Item.Size;
		++Current;
	}
}

bool FUE2NetDownloader::StartHttp(FItem& Item)
{
	FString Url = RedirectURL;
	if (!Url.EndsWith(TEXT("/")))
	{
		Url += TEXT("/");
	}
	Url += FGenericPlatformHttp::UrlEncode(Item.FileName);
	if (bRedirectCompressed)
	{
		Url += TEXT(".uz2");
	}
	Http = MakeShared<FHttpState, ESPMode::ThreadSafe>();
	Http->Url = Url;
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(Url);
	Req->SetVerb(TEXT("GET"));
	Req->SetHeader(TEXT("User-Agent"), TEXT("OpenUT2k4"));
	Req->SetActivityTimeout(RedirectStallTimeout);
	const TWeakPtr<FHttpState, ESPMode::ThreadSafe> Weak = Http;
	// a redirect may hold another build of the package (its GUID decides below): only the sanity cap applies here
	const int64 Cap = UE2NetDownload::MaxPackageBytes;
	Req->OnRequestProgress64().BindLambda([Weak, Cap](FHttpRequestPtr R, uint64, uint64 Received)
	{
		if (const TSharedPtr<FHttpState, ESPMode::ThreadSafe> S = Weak.Pin())
		{
			S->Received = int64(Received);
			S->bResponded |= Received > 0;
			if (S->Received > Cap && !S->bTooBig)
			{
				S->bTooBig = true;
				if (R)
				{
					R->CancelRequest();
				}
			}
		}
	});
	Req->OnStatusCodeReceived().BindLambda([Weak](FHttpRequestPtr, int32)
	{
		if (const TSharedPtr<FHttpState, ESPMode::ThreadSafe> S = Weak.Pin())
		{
			S->bResponded = true;
		}
	});
	Req->OnProcessRequestComplete().BindLambda([Weak](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
	{
		if (const TSharedPtr<FHttpState, ESPMode::ThreadSafe> S = Weak.Pin())
		{
			S->bDone = true;
			S->Status = Response ? Response->GetResponseCode() : 0;
			S->bResponded |= S->Status != 0;
			S->bOk = bConnected && Response && S->Status == 200 && !S->bTooBig;
			if (S->bOk)
			{
				S->Content = Response->GetContent();
			}
		}
	});
	Http->Request = Req;
	if (!Req->ProcessRequest())
	{
		UE_LOG(LogUE2Net, Warning, TEXT("Download %s: the HTTP request could not be started"), *Url);
		Http.Reset();
		return false;
	}
	Method = EMethod::Http;
	UE_LOG(LogUE2Net, Log, TEXT("Downloading %s (%d of %d, %s MB) from the redirect: %s"), *Item.FileName, Current + 1, Queue.Num(), *FormatMB(Item.Size), *Url);
	return true;
}

bool FUE2NetDownloader::StartChannel(FItem& Item)
{
	ChannelBytes.Reset();
	bChannelDone = false;
	bGotReply = false;
	FileChannel = Connection.OpenFileChannel(Item.Guid);
	if (!FileChannel)
	{
		return false;
	}
	if (!bRaisedNetSpeed)
	{
		bRaisedNetSpeed = true;
		Connection.SetNetSpeed(DownloadNetSpeed);
	}
	Method = EMethod::Channel;
	UE_LOG(LogUE2Net, Log, TEXT("Downloading %s (%d of %d, %s MB) from the server (file channel %d)"), *Item.FileName, Current + 1, Queue.Num(), *FormatMB(Item.Size), FileChannel);
	return true;
}

void FUE2NetDownloader::OnFileData(int32 Channel, const TArray<uint8>& Data, bool bClose)
{
	if (Channel != FileChannel || Method != EMethod::Channel || !Queue.IsValidIndex(Current))
	{
		return;
	}
	LastData = FPlatformTime::Seconds();
	bGotReply = true;
	NoReplyInRow = 0;
	ReceivedBytes += Data.Num();
	const int64 Cap = FMath::Min<int64>(UE2NetDownload::MaxPackageBytes, bChannelCompressed ? Queue[Current].Size + Queue[Current].Size / 8 + 65536 : Queue[Current].Size);
	if (int64(ChannelBytes.Num()) + Data.Num() > Cap)
	{
		// more than the server announced: stop
		Connection.CloseFileChannel(FileChannel);
		FileChannel = 0;
		ChannelBytes.Reset();
		bChannelDone = true;
		UE_LOG(LogUE2Net, Warning, TEXT("Download %s: the server sends more than its announced size; cancelled"), *Queue[Current].FileName);
		return;
	}
	ChannelBytes.Append(Data);
	CurrentBytes = ChannelBytes.Num();
	if (bClose)
	{
		// the channel is closed by the server's last bunch (an empty close: refused)
		FileChannel = 0;
		bChannelDone = true;
	}
}

bool FUE2NetDownloader::Finish(FItem& Item, TArray<uint8>&& Bytes, bool bCompressed, FString& OutError)
{
	TArray<uint8> File;
	if (bCompressed)
	{
		if (!UE2NetDownload::DecompressUz2(Bytes.GetData(), Bytes.Num(), UE2NetDownload::MaxPackageBytes, File, OutError))
		{
			return false;
		}
	}
	else
	{
		File = MoveTemp(Bytes);
	}
	const bool bSizeDiffers = File.Num() != Item.Size;
	if (bSizeDiffers && Method == EMethod::Channel)
	{
		// the server's own file: exactly its announced size
		OutError = FString::Printf(TEXT("%d bytes instead of the server's %lld"), File.Num(), Item.Size);
		return false;
	}
	FUE2PackageHeader Header;
	if (!UE2ParsePackageHeader(File.GetData(), File.Num(), Header, &OutError))
	{
		return false;
	}
	if (Header.Guid != Item.Guid)
	{
		OutError = FString::Printf(TEXT("another version of the package: GUID %s and %d bytes, the server's is GUID %s and %lld bytes"),
			*UE2GuidToString(Header.Guid), File.Num(), *UE2GuidToString(Item.Guid), Item.Size);
		return false;
	}
	if (bSizeDiffers)
	{
		// same GUID (the same package build), other size: UT2004 only compares GUIDs
		UE_LOG(LogUE2Net, Warning, TEXT("Download %s: %d bytes, the server announced %lld; the GUID matches, so it is used"), *Item.FileName, File.Num(), Item.Size);
	}
	if (Header.Generations.Num() < 1 || (Item.Generation > 0 && Header.Generations.Num() < Item.Generation))
	{
		OutError = FString::Printf(TEXT("%d generations, the server has %d"), Header.Generations.Num(), Item.Generation);
		return false;
	}
	// <net cache>/<scripts|packages>/<GUID>/<FileName>: the name was checked (no paths), the folder is the GUID's
	const FString Dir = GuidDir(Item.Guid, IsScriptFile(Item.FileName));
	const FString Path = Dir / Item.FileName;
	if (!IsUnder(Path, UE2NetDownload::GetNetCacheDir()) || FPaths::GetCleanFilename(Path) != Item.FileName)
	{
		OutError = TEXT("unsafe file name");
		return false;
	}
	IFileManager::Get().MakeDirectory(*Dir, true);
	const FString Temp = Path + TEXT(".part");
	if (!FFileHelper::SaveArrayToFile(File, *Temp) || !IFileManager::Get().Move(*Path, *Temp, true, true))
	{
		IFileManager::Get().Delete(*Temp, false, true, true);
		OutError = FString::Printf(TEXT("cannot write %s"), *Path);
		return false;
	}
	TouchIndex(Item.Guid, Item.Name, Path, File.Num(), Header.Generations.Num(), true);
	UE_LOG(LogUE2Net, Log, TEXT("Downloaded %s: GUID %s matches, %d generations (server %d), %d bytes -> %s"), *Item.FileName, *UE2GuidToString(Header.Guid),
		Header.Generations.Num(), Item.Generation, File.Num(), *Path);
	return true;
}

void FUE2NetDownloader::MarkRedirectDead(const FString& Why)
{
	if (bRedirectDead)
	{
		return;
	}
	bRedirectDead = true;
	RedirectNote = bChannelAllowed ? TEXT("redirect not answering: using the server (slower)")
		: TEXT("redirect not answering, and the server allows no downloads");
	UE_LOG(LogUE2Net, Warning, TEXT("Download redirect %s: %s; the rest of this pass %s"), *RedirectURL, *Why,
		bChannelAllowed ? TEXT("uses the server's file channel") : TEXT("cannot be downloaded (the server does not allow downloads)"));
}

void FUE2NetDownloader::FailCurrent(const FString& Reason, double Now)
{
	FItem& Item = Queue[Current];
	UE_LOG(LogUE2Net, Warning, TEXT("Download %s (%s) failed: %s"), *Item.FileName, Method == EMethod::Http ? TEXT("redirect") : TEXT("file channel"), *Reason);
	Http.Reset();
	if (FileChannel)
	{
		Connection.CloseFileChannel(FileChannel);
		FileChannel = 0;
	}
	Method = EMethod::None;
	// the next method of the same file (the redirect failed: the file channel), else the next file
	StartNext(Now);
}

void FUE2NetDownloader::Abort(const FString& Reason)
{
	UE_LOG(LogUE2Net, Warning, TEXT("Join cancelled: %s"), *Reason);
	if (Http && Http->Request && !Http->bDone)
	{
		Http->Request->OnProcessRequestComplete().Unbind();
		Http->Request->OnRequestProgress64().Unbind();
		Http->Request->CancelRequest();
	}
	Http.Reset();
	Connection.OnFileData = nullptr;
	FileChannel = 0;
	Method = EMethod::None;
	Phase = EPhase::Aborted;
	// the join reports the close reason (UE2NetJoin's OnClosed)
	Connection.Close(Reason);
}

void FUE2NetDownloader::Tick(double Now)
{
	if (Phase == EPhase::Downloading && Queue.IsValidIndex(Current))
	{
		// the whole pass has a time limit; with a measured rate, a pass that cannot make it ends early
		const double Limit = UE2NetDownload::GetDownloadTimeLimit();
		// the projection uses the file channel's own rate, and only for what will come over the channel: with a redirect,
		// the next files try it first (the redirect is not projected; the overall limit still applies)
		if (Method == EMethod::Channel && bGotReply)
		{
			ReceiveSeconds += FMath::Clamp(Now - LastTick, 0.0, 1.0);
		}
		LastTick = Now;
		const bool bRedirectUsable = !RedirectURL.IsEmpty() && !bRedirectDead && !FParse::Param(FCommandLine::Get(), TEXT("UE2NetNoRedirect"));
		int64 Left = FMath::Max<int64>(0, Queue[Current].Size - CurrentBytes);
		if (!bRedirectUsable)
		{
			for (int32 i = Current + 1; i < Queue.Num(); ++i)
			{
				Left += Queue[i].Size;
			}
		}
		const double Rate = ReceiveSeconds > 0.0 ? double(ReceivedBytes) / ReceiveSeconds : 0.0;
		const bool bTooSlow = Method == EMethod::Channel && ReceiveSeconds > RateWarmup && Rate > 0.0 && (Now - PassStart) + double(Left) / Rate > Limit;
		if (Now - PassStart > Limit || bTooSlow)
		{
			Abort(bTooSlow
				? FString::Printf(TEXT("downloading the server's packages would take too long (%s MB left over the file channel at %.1f KB/s, limit %.0f min; the server limits the download speed)"),
					*FormatMB(Left), Rate / 1024.0, Limit / 60.0)
				: FString::Printf(TEXT("downloading the server's packages took longer than %.0f min"), Limit / 60.0));
			return;
		}
		FItem& Item = Queue[Current];
		if (Method == EMethod::Http && Http)
		{
			if (Http->Received != CurrentBytes)
			{
				CurrentBytes = Http->Received;
				LastData = Now;
			}
			if (Http->bDone)
			{
				const TSharedPtr<FHttpState, ESPMode::ThreadSafe> S = Http;
				Http.Reset();
				FString Err;
				if (!S->bOk)
				{
					if (!S->bTooBig && S->Status == 0)
					{
						// no HTTP answer at all (host down, refused, reset): the redirect is not tried again this pass
						MarkRedirectDead(TEXT("it could not be reached"));
					}
					FailCurrent(S->bTooBig ? FString(TEXT("larger than announced")) : FString::Printf(TEXT("HTTP status %d from %s"), S->Status, *S->Url), Now);
					return;
				}
				CurrentBytes = 0;
				if (!Finish(Item, MoveTemp(S->Content), bRedirectCompressed, Err))
				{
					FailCurrent(Err, Now);
					return;
				}
				Downloaded.Add(Item.Name);
				BytesDone += Item.Size;
				++Current;
				StartNext(Now);
				return;
			}
		}
		else if (Method == EMethod::Channel)
		{
			if (bChannelDone)
			{
				bChannelDone = false;
				FString Err;
				if (ChannelBytes.Num() == 0)
				{
					FailCurrent(TEXT("the server refused to send it (download not allowed, or not its file)"), Now);
					return;
				}
				if (!Finish(Item, MoveTemp(ChannelBytes), bChannelCompressed, Err))
				{
					ChannelBytes.Reset();
					FailCurrent(Err, Now);
					return;
				}
				ChannelBytes.Reset();
				Downloaded.Add(Item.Name);
				BytesDone += Item.Size;
				++Current;
				StartNext(Now);
				return;
			}
		}
		if (Phase == EPhase::Downloading && Method == EMethod::Channel && !bGotReply && Now - ItemStart > ReplyTimeout)
		{
			// no answer at all to the request (neither data nor a refusal)
			if (++NoReplyInRow >= MaxNoReplyInRow)
			{
				UE_LOG(LogUE2Net, Warning, TEXT("The server did not answer %d file requests in a row: no more file channel downloads for this join"), NoReplyInRow);
			}
			FailCurrent(FString::Printf(TEXT("no answer from the server in %.0f s"), ReplyTimeout), Now);
			return;
		}
		if (Phase == EPhase::Downloading && Method == EMethod::Http && Http && !Http->bResponded && Now - ItemStart > RedirectConnectTimeout)
		{
			// UT2004 gives a redirect that does not answer up and uses the server itself for the rest of the pass
			Http->Request->OnProcessRequestComplete().Unbind();
			Http->Request->CancelRequest();
			MarkRedirectDead(FString::Printf(TEXT("it did not answer in %.0f s"), RedirectConnectTimeout));
			FailCurrent(FString::Printf(TEXT("no answer from the redirect in %.0f s"), RedirectConnectTimeout), Now);
			return;
		}
		const double Stall = Method == EMethod::Http ? RedirectStallTimeout : StallTimeout;
		if (Phase == EPhase::Downloading && Method != EMethod::None && Now - LastData > Stall)
		{
			if (Method == EMethod::Http)
			{
				MarkRedirectDead(FString::Printf(TEXT("it sent no data for %.0f s"), Stall));
			}
			if (Http && Http->Request)
			{
				Http->Request->OnProcessRequestComplete().Unbind();
				Http->Request->CancelRequest();
			}
			FailCurrent(FString::Printf(TEXT("no data for %.0f s"), Stall), Now);
		}
		return;
	}
	if (Phase == EPhase::Converting && Convert && Convert->bDone)
	{
		const TSharedPtr<FConvertJob, ESPMode::ThreadSafe> Job = Convert;
		Convert.Reset();
		ConvertedMaps = Job->Stats.ConvertedMaps;
		UE_LOG(LogUE2Net, Log, TEXT("Downloaded content converted in %.1fs: %s%d maps converted (%s), %d unchanged, %d cannot be played, %d errors%s%s"), Job->Stats.Seconds,
			Job->bOk ? TEXT("") : TEXT("FAILED, "), Job->Stats.ConvertedMaps.Num(), *FString::Join(Job->Stats.ConvertedMaps, TEXT(", ")), Job->Stats.UnchangedMaps,
			Job->Stats.UnplayableMaps, Job->Stats.Errors, Job->Stats.FatalError.IsEmpty() ? TEXT("") : TEXT(": "), *Job->Stats.FatalError);
		if (Job->Stats.ConvertedMaps.Num() || Job->Stats.RemovedMaps.Num())
		{
			ArenaRuntimeContent::ResetCacheIndex();
		}
		AArenaLevelBuilder::RefreshImportedLayouts();
		Phase = EPhase::Done;
	}
}

void FUE2NetDownloader::StartConvert()
{
	Phase = EPhase::Done;
	// maps and asset packages from the net cache are converted into the content cache like custom content
	const FString NetDir = UE2NetDownload::GetNetCacheDir();
	TMap<FString, FString> Chosen; // lower package name -> file
	TSet<FString> ServerNames;
	bool bNeeded = false;
	for (const FUE2PackageInfo& Info : Connection.GetPackageMap().GetList())
	{
		if (Info.bGuidMatch && IsUnder(Info.LocalPath, NetDir))
		{
			Chosen.Add(Info.Name.ToLower(), Info.LocalPath);
			ServerNames.Add(Info.Name.ToLower());
			TouchIndex(Info.Guid, Info.Name, Info.LocalPath, IFileManager::Get().FileSize(*Info.LocalPath), Info.LocalGeneration, false);
			bNeeded |= !IsScriptFile(Info.LocalPath);
		}
	}
	if (!bNeeded)
	{
		return;
	}
	if (!ArenaContent::UseRuntimeContent() || !ArenaContent::IsCacheReady())
	{
		UE_LOG(LogUE2Net, Log, TEXT("Downloaded maps / packages are not converted: no runtime content cache"));
		return;
	}
	const FString Install = ArenaContent::FindUT2004Install();
	if (Install.IsEmpty())
	{
		UE_LOG(LogUE2Net, Warning, TEXT("Downloaded maps / packages cannot be converted without a UT2004 install (the stock packages they use)"));
		return;
	}
	if (FArenaConverter::Get().IsRunning() || ArenaUserContent::IsScanning())
	{
		UE_LOG(LogUE2Net, Warning, TEXT("The content converter is busy: downloaded maps are converted on the next join"));
		return;
	}
	// every other version-free package of the net cache too (one file per name, the most recently used), so maps
	// converted for other servers stay in the content cache
	FConvJson Index;
	if (FConvJson::LoadFile(IndexPath(), Index))
	{
		TMap<FString, FString> Newest; // lower name -> last_used
		for (const TPair<FString, FConvJson>& KV : Index.Get(TEXT("packages")).O)
		{
			const FString Name = KV.Value.GetString(TEXT("name")).ToLower();
			const FString File = NetDir / KV.Value.GetString(TEXT("file"));
			const FString Used = KV.Value.GetString(TEXT("last_used"));
			if (Name.IsEmpty() || ServerNames.Contains(Name) || !IsUnder(File, NetDir) || !IFileManager::Get().FileExists(*File))
			{
				continue;
			}
			const FString* Prev = Newest.Find(Name);
			if (!Prev || Used > *Prev)
			{
				Newest.Add(Name, Used);
				Chosen.FindOrAdd(Name) = File;
			}
		}
	}
	ArenaConvUserContent::FOptions Opt;
	Opt.InstallDir = Install;
	Opt.CacheDir = ArenaContent::GetCacheDir();
	Opt.UserDir = NetDir;
	Opt.StateDirName = TEXT("netcache");
	Chosen.GenerateValueArray(Opt.Files);
	Opt.Files.Sort();
	Opt.ReferencePackages = UserContentPackages();
	Opt.bMaintenance = false;

	Convert = MakeShared<FConvertJob, ESPMode::ThreadSafe>();
	Phase = EPhase::Converting;
	UE_LOG(LogUE2Net, Log, TEXT("Converting downloaded content (%d net cache packages) into %s"), Opt.Files.Num(), *Opt.CacheDir);
	const TSharedPtr<FConvertJob, ESPMode::ThreadSafe> Job = Convert;
	Async(EAsyncExecution::Thread, [Job, Opt]()
	{
		ArenaConvUserContent::FStats Stats;
		const bool bOk = ArenaConvUserContent::Run(Opt, [&Job](const ArenaConvUserContent::FProgress& P)
		{
			FScopeLock L(&Job->Lock);
			Job->Stage = P.Stage;
			Job->Item = P.Item;
			Job->Fraction = P.Fraction;
		}, &Job->bCancel, Stats);
		{
			FScopeLock L(&Job->Lock);
			Job->Stats = Stats;
			Job->bOk = bOk;
		}
		Job->bDone = true;
	});
}

FString FUE2NetDownloader::DescribeStatus() const
{
	if (Phase == EPhase::Downloading && Queue.IsValidIndex(Current))
	{
		const FItem& Item = Queue[Current];
		FString Speed;
		if (Method == EMethod::Channel && ReceiveSeconds > 3.0 && ReceivedBytes > 0)
		{
			// file channel: the rate the server allows, and the time this file still needs
			const double Rate = double(ReceivedBytes) / ReceiveSeconds;
			const int32 Secs = FMath::RoundToInt(double(FMath::Max<int64>(0, Item.Size - CurrentBytes)) / Rate);
			Speed = FString::Printf(TEXT(", %.0f KB/s, about %d:%02d left"), Rate / 1024.0, Secs / 60, Secs % 60);
		}
		FString Waiting;
		if (Method == EMethod::Http)
		{
			const double Elapsed = FPlatformTime::Seconds() - ItemStart;
			if (CurrentBytes == 0 && Elapsed > 2.0)
			{
				Waiting = FString::Printf(TEXT(" | waiting for the redirect (%.0f s)"), Elapsed);
			}
			else if (Elapsed > 1.0 && CurrentBytes > 0)
			{
				Speed = FString::Printf(TEXT(", %.0f KB/s"), double(CurrentBytes) / Elapsed / 1024.0);
			}
		}
		// " | " parts: the download band shows them on a second line
		const FString Total = Queue.Num() > 1 ? FString::Printf(TEXT(" | %s of %s MB in all"), *FormatMB(BytesDone + CurrentBytes), *FormatMB(BytesTotal)) : FString();
		return FString::Printf(TEXT("Downloading %s (%d of %d): %s / %s MB%s%s%s%s%s"), *Item.FileName, Current + 1, Queue.Num(), *FormatMB(CurrentBytes),
			*FormatMB(Item.Size), *Speed, *Total, *Waiting, RedirectNote.IsEmpty() ? TEXT("") : TEXT(" | "), *RedirectNote);
	}
	if (Phase == EPhase::Converting && Convert)
	{
		FScopeLock L(&Convert->Lock);
		return FString::Printf(TEXT("Converting downloaded content: %s%s%s (%d%%)"), *Convert->Stage, Convert->Item.IsEmpty() ? TEXT("") : TEXT(" "), *Convert->Item,
			FMath::RoundToInt(Convert->Fraction * 100.f));
	}
	return FString();
}

float FUE2NetDownloader::GetProgress() const
{
	if (Phase == EPhase::Done)
	{
		return 1.f;
	}
	if (Phase == EPhase::Converting && Convert)
	{
		FScopeLock L(&Convert->Lock);
		return 0.8f + 0.2f * Convert->Fraction;
	}
	return BytesTotal > 0 ? 0.8f * float(double(BytesDone + CurrentBytes) / double(BytesTotal)) : 0.f;
}

// =================================================================================================
// -UE2NetDownloadSelfTest: file name checks and .uz2 round trip (logs "UE2NetDownloadSelfTest: n passed, m failed")
// =================================================================================================

#if !UE_BUILD_SHIPPING
#include "Misc/DelayedAutoRegister.h"

namespace UE2NetDownloadLocal
{
	void RunSelfTest()
	{
		int32 Passed = 0, Failed = 0;
		auto Check = [&](bool bOk, const TCHAR* What)
		{
			(bOk ? Passed : Failed)++;
			if (!bOk)
			{
				UE_LOG(LogUE2Net, Error, TEXT("UE2NetDownloadSelfTest: FAILED %s"), What);
			}
		};
		using UE2NetDownload::IsSafePackageFileName;
		Check(IsSafePackageFileName(TEXT("3SPNv3141.u"), TEXT("3SPNv3141")), TEXT("plain script package"));
		Check(IsSafePackageFileName(TEXT("DM-CBP2-Griffin.ut2"), TEXT("DM-CBP2-Griffin")), TEXT("map"));
		Check(IsSafePackageFileName(TEXT("DM-[EFG]Arena.ut2"), TEXT("DM-[EFG]Arena")), TEXT("brackets"));
		Check(IsSafePackageFileName(TEXT("VCTF-Arcade][Blocktastic]-Rev2.ut2"), TEXT("VCTF-Arcade][Blocktastic]-Rev2")), TEXT("reversed brackets"));
		Check(IsSafePackageFileName(TEXT("VCTF-ShockSlap-{V][M}.ut2"), TEXT("VCTF-ShockSlap-{V][M}")), TEXT("braces"));
		Check(IsSafePackageFileName(TEXT("AS-Gasoline(v4Fix).ut2"), TEXT("AS-Gasoline(v4Fix)")), TEXT("parentheses"));
		Check(IsSafePackageFileName(TEXT("VCTF4-R!P-Castle.ut2"), TEXT("VCTF4-R!P-Castle")), TEXT("exclamation"));
		Check(IsSafePackageFileName(TEXT("DM-My Map.ut2"), TEXT("DM-My Map")), TEXT("space"));
		Check(!IsSafePackageFileName(TEXT("A|B.u"), TEXT("A|B")), TEXT("pipe"));
		Check(!IsSafePackageFileName(TEXT(" A.u"), TEXT(" A")), TEXT("leading space"));
		Check(!IsSafePackageFileName(TEXT("../System/Core.u"), TEXT("Core")), TEXT("parent path"));
		Check(!IsSafePackageFileName(TEXT("..\\Core.u"), TEXT("Core")), TEXT("backslash path"));
		Check(!IsSafePackageFileName(TEXT("/etc/passwd.u"), TEXT("passwd")), TEXT("absolute path"));
		Check(!IsSafePackageFileName(TEXT("C:Core.u"), TEXT("C:Core")), TEXT("drive colon"));
		Check(!IsSafePackageFileName(TEXT("Evil.so"), TEXT("Evil")), TEXT("native library"));
		Check(!IsSafePackageFileName(TEXT("Evil.dll"), TEXT("Evil")), TEXT("windows library"));
		Check(!IsSafePackageFileName(TEXT("Other.u"), TEXT("Evil")), TEXT("name mismatch"));
		Check(!IsSafePackageFileName(TEXT(".u"), TEXT("")), TEXT("empty name"));
		Check(!IsSafePackageFileName(TEXT("A..u"), TEXT("A.")), TEXT("double dot"));
		Check(!IsSafePackageFileName(TEXT("Map.ut2.uz2"), TEXT("Map.ut2")), TEXT("compressed extension"));

		// .uz2: two chunks written the way ucc compress does, unpacked again
		TArray<uint8> Plain;
		for (int32 i = 0; i < 40000; ++i)
		{
			Plain.Add(uint8((i * 7) ^ (i >> 5)));
		}
		TArray<uint8> Uz2;
		for (int32 At = 0; At < Plain.Num(); At += 32768)
		{
			const int32 N = FMath::Min(32768, Plain.Num() - At);
			int32 CSize = FCompression::CompressMemoryBound(NAME_Zlib, N);
			TArray<uint8> C;
			C.SetNumUninitialized(CSize);
			FCompression::CompressMemory(NAME_Zlib, C.GetData(), CSize, Plain.GetData() + At, N);
			const int32 Head[2] = { CSize, N };
			Uz2.Append(reinterpret_cast<const uint8*>(Head), 8);
			Uz2.Append(C.GetData(), CSize);
		}
		TArray<uint8> Out;
		FString Err;
		Check(UE2NetDownload::DecompressUz2(Uz2.GetData(), Uz2.Num(), 1 << 20, Out, Err) && Out == Plain, TEXT("uz2 round trip"));
		Check(!UE2NetDownload::DecompressUz2(Uz2.GetData(), Uz2.Num(), 1000, Out, Err), TEXT("uz2 size cap"));
		Check(!UE2NetDownload::DecompressUz2(Uz2.GetData(), Uz2.Num() - 3, 1 << 20, Out, Err), TEXT("uz2 truncated"));
		UE_LOG(LogUE2Net, Display, TEXT("UE2NetDownloadSelfTest: %d passed, %d failed"), Passed, Failed);
	}

	FDelayedAutoRegisterHelper GSelfTest(EDelayedRegisterRunPhase::EndOfEngineInit, []()
	{
		if (FParse::Param(FCommandLine::Get(), TEXT("UE2NetDownloadSelfTest")))
		{
			RunSelfTest();
		}
	});
}
#endif
