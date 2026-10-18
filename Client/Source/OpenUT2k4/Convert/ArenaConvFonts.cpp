#include "ArenaConvFonts.h"
#include "ArenaConvConfig.h"
#include "ArenaConverter.h"
#include "ArenaConvExporter.h"
#include "ArenaConvJson.h"
#include "ArenaConvPackage.h"
#include "../OpenUT2k4.h"
#include "Misc/Paths.h"

namespace ArenaConvFontsLocal
{
	FString IndexPath(const FString& CacheDir)
	{
		return CacheDir / TEXT("fonts") / TEXT("index.json");
	}
}

bool ArenaConvFonts::NeedsUpdate(const FString& CacheDir)
{
	FConvJson Index;
	return !FConvJson::LoadFile(ArenaConvFontsLocal::IndexPath(CacheDir), Index)
		|| int32(Index.GetNumber(TEXT("version"), 0)) < ArenaConverterVersions::FontsVersion;
}

#if ARENA_CONV_BUILD

// ---------------------------------------------------------------------------------------------
// One Font object (UE2 UFont::Serialize after the tagged properties, package version 128):
//   TArray<FFontCharacter> Characters   (compact count, then INT StartU, StartV, USize, VSize, BYTE TextureIndex)
//   TArray<UTexture*> Textures          (compact count, compact object references)
//   INT Kerning
//   TMap<WORD, WORD> CharRemap          (compact count, then WORD key, WORD value pairs)
//   UBOOL IsRemapped                    (INT)
// Older packages may end after the textures; the trailing fields are read only when present.

FString FConvExporter::ExportFont(FConvObject* Obj)
{
	const FString Path = OutPath(TEXT("fonts"), Obj, TEXT(".json"));
	FConvReader R = Obj->NativeReader();
	if (R.bError)
	{
		Error(FString::Printf(TEXT("%s (Font)"), *Obj->Path()), Obj->LoadError().IsEmpty() ? R.Error : Obj->LoadError());
		return FString();
	}
	struct FChar
	{
		int32 U = 0, V = 0, W = 0, H = 0;
		uint8 Page = 0;
	};
	const int32 NumChars = R.Index();
	if (R.bError || NumChars < 0 || NumChars > 65536)
	{
		Error(FString::Printf(TEXT("%s (Font)"), *Obj->Path()), FString::Printf(TEXT("bad character count %d"), NumChars));
		return FString();
	}
	TArray<FChar> Chars;
	Chars.SetNum(NumChars);
	int32 MaxHeight = 0;
	for (FChar& C : Chars)
	{
		C.U = R.I32();
		C.V = R.I32();
		C.W = R.I32();
		C.H = R.I32();
		C.Page = R.U8();
		MaxHeight = FMath::Max(MaxHeight, C.H);
	}
	const int32 NumPages = R.Index();
	if (R.bError || NumPages < 0 || NumPages > 256)
	{
		Error(FString::Printf(TEXT("%s (Font)"), *Obj->Path()), FString::Printf(TEXT("bad texture count %d %s"), NumPages, *R.Error));
		return FString();
	}
	TArray<int32> PageRefs;
	for (int32 i = 0; i < NumPages; ++i)
	{
		PageRefs.Add(R.Index());
	}
	int32 Kerning = 0;
	bool bRemapped = false;
	TArray<TPair<uint16, uint16>> Remap;
	if (!R.bError && R.Remaining() >= 4)
	{
		Kerning = R.I32();
	}
	if (!R.bError && R.Remaining() >= 1)
	{
		const int32 NumRemap = R.Index();
		for (int32 i = 0; i < NumRemap && !R.bError; ++i)
		{
			const uint16 From = R.U16();
			const uint16 To = R.U16();
			Remap.Emplace(From, To);
		}
	}
	if (!R.bError && R.Remaining() >= 4)
	{
		bRemapped = R.I32() != 0;
	}
	if (R.bError)
	{
		Error(FString::Printf(TEXT("%s (Font)"), *Obj->Path()), R.Error);
		return FString();
	}

	// the pages first: they are what the font depends on (and they are reused when fresh)
	FConvJson Pages = FConvJson::Array();
	for (const int32 Ref : PageRefs)
	{
		const FConvRef PageRef = Obj->Pkg->MakeRef(Ref);
		FConvJson Page = FConvJson::Object();
		Page.Set(TEXT("path"), PageRef.IsSet() ? FConvJson(Canon(PageRef)) : FConvJson());
		Page.Set(TEXT("file"), FConvJson::StrOrNull(ExportRef(PageRef)));
		FConvObject* Tex = PageRef.Load();
		const FIntPoint Size = Tex ? TextureSize(Tex) : FIntPoint(0, 0);
		Page.Set(TEXT("width"), Size.X);
		Page.Set(TEXT("height"), Size.Y);
		Pages.Add(MoveTemp(Page));
	}
	if (Fresh(Obj, Path, Path, ArenaConverterVersions::FontsVersion))
	{
		return Rel(Path);
	}

	FConvJson Meta = FConvJson::Object();
	Meta.Set(TEXT("type"), TEXT("Font"));
	Meta.Set(TEXT("path"), Obj->Path());
	Meta.Set(TEXT("tool_version"), ArenaConverterVersions::FontsVersion);
	Meta.Set(TEXT("kerning"), Kerning);
	Meta.Set(TEXT("remapped"), bRemapped);
	Meta.Set(TEXT("max_height"), MaxHeight);
	Meta.Set(TEXT("pages"), MoveTemp(Pages));
	FConvJson CharsJson = FConvJson::Array();
	CharsJson.A.Reserve(Chars.Num());
	for (const FChar& C : Chars)
	{
		FConvJson E = FConvJson::Array();
		E.Add(C.U);
		E.Add(C.V);
		E.Add(C.W);
		E.Add(C.H);
		E.Add(int32(C.Page));
		CharsJson.Add(MoveTemp(E));
	}
	Meta.Set(TEXT("chars"), MoveTemp(CharsJson));
	if (Remap.Num())
	{
		FConvJson RemapJson = FConvJson::Array();
		for (const TPair<uint16, uint16>& KV : Remap)
		{
			FConvJson E = FConvJson::Array();
			E.Add(int32(KV.Key));
			E.Add(int32(KV.Value));
			RemapJson.Add(MoveTemp(E));
		}
		Meta.Set(TEXT("remap"), MoveTemp(RemapJson));
	}
	if (!WriteJson(Path, Meta))
	{
		return FString();
	}
	Counts.FindOrAdd(TEXT("fonts"))++;
	return Rel(Path);
}

int32 ArenaConvFonts::ExportAll(FConvPackageManager& Mgr, FConvExporter& Ex, const FString& CacheDir, const TAtomic<bool>* Cancel)
{
	FConvJson Fonts = FConvJson::Array();
	int32 Opened = 0;
	for (const TPair<FString, FString>& KV : Mgr.PackageFiles())
	{
		if (Cancel && Cancel->Load())
		{
			Mgr.CloseAll();
			return -1;
		}
		const FString Ext = FPaths::GetExtension(KV.Value, true).ToLower();
		// asset and script packages; maps and music carry no HUD fonts
		if (Ext == TEXT(".ut2") || Ext == TEXT(".umx") || Ext == TEXT(".ogg"))
		{
			continue;
		}
		const FString Name = FPaths::GetBaseFilename(KV.Value);
		if (Mgr.ExtraPackages.Contains(Name.ToLower()))
		{
			continue;
		}
		FConvPackage* Pkg = Mgr.LoadPackage(Name);
		if (!Pkg)
		{
			continue;
		}
		for (const int32 Ei : Pkg->ExportsOfClass({ TEXT("Font") }))
		{
			FConvObject* Obj = Pkg->GetObject(Ei);
			const FString File = Obj ? Ex.ExportObject(Obj) : FString();
			if (!File.IsEmpty())
			{
				Fonts.Add(Obj->Path());
			}
		}
		if (++Opened % 64 == 0)
		{
			Mgr.CloseAll();
		}
	}
	Mgr.CloseAll();
	Ex.Flush();
	FConvJson Index = FConvJson::Object();
	Index.Set(TEXT("version"), ArenaConverterVersions::FontsVersion);
	Index.Set(TEXT("fonts"), Fonts);
	if (!Index.SaveToFile(ArenaConvFontsLocal::IndexPath(CacheDir), 1))
	{
		Ex.Error(ArenaConvFontsLocal::IndexPath(CacheDir), TEXT("cannot write"));
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("Converter: %d fonts in the cache (fonts version %d)"), Fonts.Num(), ArenaConverterVersions::FontsVersion);
	return Fonts.Num();
}

#else

int32 ArenaConvFonts::ExportAll(FConvPackageManager&, FConvExporter&, const FString&, const TAtomic<bool>*)
{
	return -1;
}

#endif // ARENA_CONV_BUILD
