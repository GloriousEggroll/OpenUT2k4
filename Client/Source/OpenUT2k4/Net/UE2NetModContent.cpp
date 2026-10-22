#include "UE2NetModContent.h"

#include "../ArenaContent.h"
#include "../ArenaRuntimeContent.h"
#include "../Convert/ArenaConvExporter.h"
#include "../Convert/ArenaConvPackage.h"
#include "../UScript/UScriptRuntime.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Sound/SoundBase.h"
#include "UE2NetLog.h"

namespace UE2NetModContentLocal
{
	/** Cache kind folder of a UE2 class. */
	const TCHAR* KindFolder(const FString& ClassName)
	{
		if (ClassName == TEXT("Sound"))
		{
			return TEXT("sounds");
		}
		if (ClassName == TEXT("StaticMesh"))
		{
			return TEXT("staticmeshes");
		}
		if (ClassName == TEXT("Font"))
		{
			return TEXT("fonts");
		}
		if (ClassName == TEXT("Texture") || ClassName == TEXT("Cubemap"))
		{
			return TEXT("textures");
		}
		if (ClassName == TEXT("SkeletalMesh"))
		{
			return TEXT("skeletalmeshes");
		}
		if (ClassName == TEXT("MeshAnimation"))
		{
			return TEXT("animations");
		}
		// Shader, FinalBlend, Combiner, TexRotator ... (the converter writes them to materials/, their textures to textures/)
		return TEXT("materials");
	}

	/** The package file the script runtime has for this name (downloaded mods first), or empty. */
	FString PackageFile(const FString& Package)
	{
		FUSRuntime& RT = FUSRuntime::Get();
		FUSLinker* const* Found = RT.Linkers.Find(FName(*Package));
		FUSLinker* Linker = Found ? *Found : RT.LoadPackage(FName(*Package));
		return Linker && Linker->Pkg ? Linker->Pkg->PathOnDisk : FString();
	}
}

bool UE2NetModContent::EnsureConverted(const FString& ObjectPath, const FString& ClassName)
{
	using namespace UE2NetModContentLocal;
	static TSet<FString> Tried;
	if (ObjectPath.IsEmpty() || ObjectPath.Equals(TEXT("None")))
	{
		return false;
	}
	// no class: a material of any kind (a texture, or a Shader / FinalBlend / TexEnvMap ...)
	const bool bAnyMaterial = ClassName.IsEmpty();
	const TCHAR* Kind = bAnyMaterial ? TEXT("textures") : KindFolder(ClassName);
	if (!ArenaRuntimeContent::FindCacheFileByObjectPath(Kind, ObjectPath, TEXT(".json")).IsEmpty()
		|| (bAnyMaterial && !ArenaRuntimeContent::FindCacheFileByObjectPath(TEXT("materials"), ObjectPath, TEXT(".json")).IsEmpty()))
	{
		return true;
	}
	const FString Key = ObjectPath.ToLower();
	if (Tried.Contains(Key) || !ArenaContent::UseRuntimeContent() || !ArenaContent::IsCacheReady())
	{
		return false;
	}
	Tried.Add(Key);
	FString Package;
	if (!ObjectPath.Split(TEXT("."), &Package, nullptr))
	{
		return false;
	}
	const FString Install = ArenaContent::FindUT2004Install();
	if (Install.IsEmpty())
	{
		return false;
	}
	// a mod's package (downloaded, or a mod in the UT2004 folder): the install's own packages were converted with the
	// content cache, and what they lack (procedural sounds ...) has no file to convert
	for (const TPair<const TCHAR*, const TCHAR*>& Stock : { TPair<const TCHAR*, const TCHAR*>(TEXT("Sounds"), TEXT(".uax")),
			TPair<const TCHAR*, const TCHAR*>(TEXT("Textures"), TEXT(".utx")), TPair<const TCHAR*, const TCHAR*>(TEXT("StaticMeshes"), TEXT(".usx")),
			TPair<const TCHAR*, const TCHAR*>(TEXT("Animations"), TEXT(".ukx")) })
	{
		if (FPaths::FileExists(Install / Stock.Key / (Package + Stock.Value)))
		{
			return false;
		}
	}
	const FString File = PackageFile(Package);
	if (File.IsEmpty())
	{
		return false;
	}
	const double T0 = FPlatformTime::Seconds();
	FConvPackageManager Mgr(Install);
	if (Mgr.FindFile(Package).IsEmpty())
	{
		// a stock name always keeps the install's file
		Mgr.AddPackageFiles({ File });
	}
	FConvObject* Obj = Mgr.ResolvePath(ObjectPath, ClassName);
	if (!Obj)
	{
		UE_LOG(LogUE2Net, Log, TEXT("UE2NetModContent: %s (%s) not found in its package"), *ObjectPath, *ClassName);
		return false;
	}
	FConvExporter::FOptions Opt;
	Opt.OutDir = ArenaContent::GetCacheDir();
	Opt.bSkipExisting = true;
	FString Result;
	{
		FConvExporter Ex(Mgr, Opt);
		Result = Ex.ExportObject(Obj);
		Ex.Flush();
		for (const FString& E : Ex.TakeNewErrors())
		{
			UE_LOG(LogUE2Net, Warning, TEXT("UE2NetModContent: %s"), *E);
		}
	}
	// a material exports its textures, a mesh its materials
	ArenaRuntimeContent::OnCacheFilesAdded(Kind);
	ArenaRuntimeContent::OnCacheFilesAdded(TEXT("textures"));
	ArenaRuntimeContent::OnCacheFilesAdded(TEXT("materials"));
	UE_LOG(LogUE2Net, Log, TEXT("UE2NetModContent: converted %s (%s) -> %s in %.0f ms"), *ObjectPath, *ClassName, Result.IsEmpty() ? TEXT("nothing") : *Result,
		(FPlatformTime::Seconds() - T0) * 1000.0);
	return !Result.IsEmpty();
}

USoundBase* UE2NetModContent::LoadSound(const FString& ObjectPath)
{
	if (ObjectPath.IsEmpty() || !EnsureConverted(ObjectPath, TEXT("Sound")))
	{
		return nullptr;
	}
	return ArenaRuntimeContent::LoadSoundByObjectPath(ObjectPath);
}
