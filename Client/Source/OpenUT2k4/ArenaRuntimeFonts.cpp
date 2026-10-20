// Runtime fonts: UFont (offline / bitmap) objects built from the cache's fonts/<Pkg>/<Name>.json (ArenaConvFonts.h).
#include "ArenaRuntimeContent.h"
#include "HAL/PlatformTime.h"
#include "ArenaRuntimeContentPrivate.h"
#include "ArenaContent.h"
#include "OpenUT2k4.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace ArenaRuntimeFontsLocal
{
	/**
	 * Applies a changed sampler setting of a texture that may already be drawn this frame. UpdateResource deletes the
	 * old resource on the render thread at once, ahead of the canvas items queued with it this frame (fonts are built
	 * while the HUD draws), which would then draw a freed resource: the update waits for the next frame.
	 */
	void UpdateResourceNextFrame(UTexture2D* Tex)
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak = TWeakObjectPtr<UTexture2D>(Tex)](float)
		{
			if (UTexture2D* T = Weak.Get())
			{
				T->UpdateResource();
			}
			return false;
		}));
	}

	struct FFontCache
	{
		TMap<FString, TStrongObjectPtr<UFont>> Fonts;
		/** Lower-case object path -> time of the last failed attempt (retried after RetrySeconds). */
		TMap<FString, double> Misses;
		/** Lower-case "<pkg>/<name>.json" -> file, for case-insensitive lookups. */
		TMap<FString, FString> Files;
		double FilesTime = -1.0;
	};
	constexpr double RetrySeconds = 15.0;

	FFontCache& Cache()
	{
		static FFontCache C;
		return C;
	}

	FString FindFontFile(const FString& ObjectPath)
	{
		using namespace ArenaRuntimePrivate;
		FString Pkg, Name;
		if (!ObjectPath.Split(TEXT("."), &Pkg, &Name))
		{
			return FString();
		}
		const FString Root = ArenaContent::GetCacheDir() / TEXT("fonts");
		const FString Exact = Root / CacheSafe(Pkg) / (CacheSafe(Name) + TEXT(".json"));
		if (IFileManager::Get().FileExists(*Exact))
		{
			return Exact;
		}
		// package names differ in case between installs (2K4Fonts / 2k4Fonts): look the file up case-insensitively
		FFontCache& C = Cache();
		const double Now = FPlatformTime::Seconds();
		if (C.FilesTime < 0.0 || Now - C.FilesTime > RetrySeconds)
		{
			C.FilesTime = Now;
			C.Files.Reset();
			TArray<FString> Found;
			IFileManager::Get().FindFilesRecursive(Found, *Root, TEXT("*.json"), true, false);
			for (const FString& F : Found)
			{
				C.Files.Add((FPaths::GetCleanFilename(FPaths::GetPath(F)) + TEXT("/") + FPaths::GetCleanFilename(F)).ToLower(), F);
			}
		}
		const FString* Hit = C.Files.Find((CacheSafe(Pkg) + TEXT("/") + CacheSafe(Name) + TEXT(".json")).ToLower());
		return Hit ? *Hit : FString();
	}

	UFont* BuildFont(const FString& ObjectPath)
	{
		using namespace ArenaRuntimePrivate;
		const FString File = FindFontFile(ObjectPath);
		const TSharedPtr<FJsonObject> J = File.IsEmpty() ? nullptr : LoadJson(File);
		if (!J.IsValid())
		{
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>* Pages = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Chars = nullptr;
		if (!J->TryGetArrayField(TEXT("pages"), Pages) || !J->TryGetArrayField(TEXT("chars"), Chars) || Pages->Num() == 0 || Chars->Num() == 0)
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("RuntimeContent: font %s has no pages or characters"), *ObjectPath);
			return nullptr;
		}
		TArray<UTexture2D*> Textures;
		for (const TSharedPtr<FJsonValue>& P : *Pages)
		{
			const TSharedPtr<FJsonObject>* PO = nullptr;
			FString PagePath;
			if (!P.IsValid() || !P->TryGetObject(PO) || !(*PO)->TryGetStringField(TEXT("path"), PagePath))
			{
				return nullptr;
			}
			UTexture2D* Tex = ArenaRuntimeContent::LoadTextureByObjectPath(PagePath);
			if (!Tex)
			{
				UE_LOG(LogOpenUT2k4, Warning, TEXT("RuntimeContent: font %s: page %s is missing"), *ObjectPath, *PagePath);
				return nullptr;
			}
			// canvas text needs the full-resolution mip (the HUD is not seen by the texture streamer)
			Tex->bForceMiplevelsToBeResident = true;
			Tex->bIgnoreStreamingMipBias = true;
			// bitmap fonts are drawn 1:1 or at whole multiples (FArenaDraw::UTFontScale): nearest keeps them crisp
			if (Tex->Filter != TF_Nearest)
			{
				Tex->Filter = TF_Nearest;
				UpdateResourceNextFrame(Tex);
			}
			Textures.Add(Tex);
		}
		UFont* Font = NewObject<UFont>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UFont::StaticClass(),
			FName(*(TEXT("Font_") + AssetName(ObjectPath)))), RF_Transient);
		Font->FontCacheType = EFontCacheType::Offline;
		Font->Kerning = int32(J->GetNumberField(TEXT("kerning")));
		bool bRemapped = false;
		J->TryGetBoolField(TEXT("remapped"), bRemapped);
		Font->IsRemapped = bRemapped ? 1 : 0;
		Font->Characters.Reserve(Chars->Num());
		for (const TSharedPtr<FJsonValue>& C : *Chars)
		{
			FFontCharacter Ch;
			const TArray<TSharedPtr<FJsonValue>>* E = nullptr;
			if (C.IsValid() && C->TryGetArray(E) && E->Num() >= 5)
			{
				Ch.StartU = int32((*E)[0]->AsNumber());
				Ch.StartV = int32((*E)[1]->AsNumber());
				Ch.USize = int32((*E)[2]->AsNumber());
				Ch.VSize = int32((*E)[3]->AsNumber());
				Ch.TextureIndex = uint8((*E)[4]->AsNumber());
				if (!Textures.IsValidIndex(Ch.TextureIndex))
				{
					Ch = FFontCharacter();
				}
			}
			Font->Characters.Add(Ch);
		}
		const TArray<TSharedPtr<FJsonValue>>* Remap = nullptr;
		if (J->TryGetArrayField(TEXT("remap"), Remap))
		{
			for (const TSharedPtr<FJsonValue>& R : *Remap)
			{
				const TArray<TSharedPtr<FJsonValue>>* E = nullptr;
				if (R.IsValid() && R->TryGetArray(E) && E->Num() >= 2)
				{
					Font->CharRemap.Add(uint16((*E)[0]->AsNumber()), uint16((*E)[1]->AsNumber()));
				}
			}
		}
		for (UTexture2D* Tex : Textures)
		{
			Font->Textures.Add(Tex);
		}
		Font->CacheCharacterCountAndMaxCharHeight();
		ArenaRuntimeContent::KeepAlive(Font, true);
		UE_LOG(LogOpenUT2k4, Log, TEXT("RuntimeContent: font %s (%d characters, %d pages, height %.0f)"), *ObjectPath, Font->Characters.Num(),
			Font->Textures.Num(), Font->GetMaxCharHeight());
		return Font;
	}
}

UFont* ArenaRuntimeContent::LoadFontByObjectPath(const FString& ObjectPath)
{
	using namespace ArenaRuntimeFontsLocal;
	check(IsInGameThread());
	if (!IsActive() || ObjectPath.IsEmpty())
	{
		return nullptr;
	}
	FFontCache& C = Cache();
	const FString Key = ObjectPath.ToLower();
	if (const TStrongObjectPtr<UFont>* Found = C.Fonts.Find(Key))
	{
		return Found->Get();
	}
	const double Now = FPlatformTime::Seconds();
	if (const double* Missed = C.Misses.Find(Key))
	{
		if (Now - *Missed < RetrySeconds)
		{
			return nullptr;
		}
	}
	UFont* Font = BuildFont(ObjectPath);
	if (!Font)
	{
		C.Misses.Add(Key, Now);
		return nullptr;
	}
	C.Misses.Remove(Key);
	C.Fonts.Add(Key, TStrongObjectPtr<UFont>(Font));
	return Font;
}

UFont* ArenaRuntimeContent::LoadFont(const FString& RelativePath)
{
	// "Fonts/<Pkg>/<Name>"
	TArray<FString> Parts;
	RelativePath.ParseIntoArray(Parts, TEXT("/"));
	if (Parts.Num() != 3 || !Parts[0].Equals(TEXT("Fonts"), ESearchCase::IgnoreCase))
	{
		return nullptr;
	}
	return LoadFontByObjectPath(Parts[1] + TEXT(".") + Parts[2]);
}

// ---- resampled fonts (LoadFontAtHeight) ----

namespace ArenaRuntimeFontsLocal
{
	struct FFontJson
	{
		TSharedPtr<FJsonObject> Json;
		int32 CellHeight = 0;
	};

	/** Parsed font json + its max_height, cached per object path (misses rate limited like the fonts). */
	const FFontJson* GetFontJson(const FString& ObjectPath)
	{
		using namespace ArenaRuntimePrivate;
		static TMap<FString, FFontJson> Jsons;
		static TMap<FString, double> JsonMisses;
		const FString Key = ObjectPath.ToLower();
		if (const FFontJson* Found = Jsons.Find(Key))
		{
			return Found;
		}
		const double Now = FPlatformTime::Seconds();
		if (const double* Missed = JsonMisses.Find(Key))
		{
			if (Now - *Missed < RetrySeconds)
			{
				return nullptr;
			}
		}
		const FString File = FindFontFile(ObjectPath);
		const TSharedPtr<FJsonObject> J = File.IsEmpty() ? nullptr : LoadJson(File);
		if (!J.IsValid())
		{
			JsonMisses.Add(Key, Now);
			return nullptr;
		}
		FFontJson F;
		F.Json = J;
		double MaxH = 0.0;
		if (!J->TryGetNumberField(TEXT("max_height"), MaxH))
		{
			const TArray<TSharedPtr<FJsonValue>>* Chars = nullptr;
			if (J->TryGetArrayField(TEXT("chars"), Chars))
			{
				for (const TSharedPtr<FJsonValue>& C : *Chars)
				{
					const TArray<TSharedPtr<FJsonValue>>* E = nullptr;
					if (C.IsValid() && C->TryGetArray(E) && E->Num() >= 4)
					{
						MaxH = FMath::Max(MaxH, (*E)[3]->AsNumber());
					}
				}
			}
		}
		F.CellHeight = int32(MaxH);
		JsonMisses.Remove(Key);
		return &Jsons.Add(Key, MoveTemp(F));
	}

	/** Catmull-Rom (B 0, C 0.5). */
	float Cubic(float X)
	{
		X = FMath::Abs(X);
		if (X < 1.f)
		{
			return 1.5f * X * X * X - 2.5f * X * X + 1.f;
		}
		if (X < 2.f)
		{
			return -0.5f * X * X * X + 2.5f * X * X - 4.f * X + 2.f;
		}
		return 0.f;
	}

	/** Taps of one output row / column: source indices and normalised weights. */
	struct FTaps
	{
		TArray<int32> Start;
		TArray<int32> Count;
		TArray<int32> Index;
		TArray<float> Weight;
	};

	FTaps MakeTaps(int32 SrcN, int32 DstN, float Q)
	{
		// Q > 1: bicubic interpolation; Q < 1: the cubic stretched to the output pixel (area filter, no aliasing)
		const float Support = Q < 1.f ? 1.f / Q : 1.f;
		FTaps T;
		T.Start.SetNumUninitialized(DstN);
		T.Count.SetNumUninitialized(DstN);
		for (int32 o = 0; o < DstN; ++o)
		{
			const float S = (float(o) + 0.5f) / Q - 0.5f;
			const int32 I0 = FMath::FloorToInt(S - 2.f * Support) + 1;
			const int32 I1 = FMath::FloorToInt(S + 2.f * Support);
			T.Start[o] = T.Index.Num();
			float Sum = 0.f;
			for (int32 i = I0; i <= I1; ++i)
			{
				const float W = Cubic((float(i) - S) / Support);
				if (W == 0.f)
				{
					continue;
				}
				T.Index.Add(FMath::Clamp(i, 0, SrcN - 1));
				T.Weight.Add(W);
				Sum += W;
			}
			T.Count[o] = T.Index.Num() - T.Start[o];
			if (Sum != 0.f)
			{
				for (int32 k = T.Start[o]; k < T.Index.Num(); ++k)
				{
					T.Weight[k] /= Sum;
				}
			}
		}
		return T;
	}

	/**
	 * BGRA8 page (straight alpha, as the converter wrote it) -> resampled BGRA8 page. Filtering happens in premultiplied
	 * alpha so transparent texels (black) never darken the glyph edges; enlarged alpha gets a contrast curve around 0.5
	 * (the glyph outline stays where it was, the edge ramp stays about a pixel wide instead of growing with Q).
	 */
	TArray64<uint8> ResamplePage(const TArray64<uint8>& Src, int32 SW, int32 SH, int32 DW, int32 DH, float Q)
	{
		TArray<float> Pre;
		Pre.SetNumUninitialized(int64(SW) * SH * 4);
		for (int64 i = 0; i < int64(SW) * SH; ++i)
		{
			const float A = Src[i * 4 + 3] / 255.f;
			Pre[i * 4 + 0] = Src[i * 4 + 0] / 255.f * A;
			Pre[i * 4 + 1] = Src[i * 4 + 1] / 255.f * A;
			Pre[i * 4 + 2] = Src[i * 4 + 2] / 255.f * A;
			Pre[i * 4 + 3] = A;
		}
		const FTaps TX = MakeTaps(SW, DW, Q);
		const FTaps TY = MakeTaps(SH, DH, Q);
		// horizontal pass: SH x DW
		TArray<float> Mid;
		Mid.SetNumZeroed(int64(DW) * SH * 4);
		for (int32 y = 0; y < SH; ++y)
		{
			const float* Row = &Pre[int64(y) * SW * 4];
			float* Out = &Mid[int64(y) * DW * 4];
			for (int32 x = 0; x < DW; ++x)
			{
				float Acc[4] = { 0.f, 0.f, 0.f, 0.f };
				for (int32 k = TX.Start[x], e = TX.Start[x] + TX.Count[x]; k < e; ++k)
				{
					const float* P = Row + int64(TX.Index[k]) * 4;
					const float W = TX.Weight[k];
					Acc[0] += P[0] * W; Acc[1] += P[1] * W; Acc[2] += P[2] * W; Acc[3] += P[3] * W;
				}
				FMemory::Memcpy(Out + int64(x) * 4, Acc, sizeof(Acc));
			}
		}
		const float Gain = Q > 1.f ? FMath::Min(3.f, 1.f + 0.5f * (Q - 1.f)) : 1.f;
		TArray64<uint8> Dst;
		Dst.SetNumZeroed(int64(DW) * DH * 4);
		for (int32 y = 0; y < DH; ++y)
		{
			for (int32 x = 0; x < DW; ++x)
			{
				float Acc[4] = { 0.f, 0.f, 0.f, 0.f };
				for (int32 k = TY.Start[y], e = TY.Start[y] + TY.Count[y]; k < e; ++k)
				{
					const float* P = &Mid[(int64(TY.Index[k]) * DW + x) * 4];
					const float W = TY.Weight[k];
					Acc[0] += P[0] * W; Acc[1] += P[1] * W; Acc[2] += P[2] * W; Acc[3] += P[3] * W;
				}
				const float A = FMath::Clamp(Acc[3], 0.f, 1.f);
				uint8* O = &Dst[(int64(y) * DW + x) * 4];
				if (A <= 0.5f / 255.f)
				{
					continue;
				}
				// straight colour from the filtered premultiplied one, then the sharpened coverage
				for (int32 c = 0; c < 3; ++c)
				{
					O[c] = uint8(FMath::Clamp(FMath::RoundToInt(Acc[c] / A * 255.f), 0, 255));
				}
				const float AS = Gain > 1.f ? FMath::Clamp((A - 0.5f) * Gain + 0.5f, 0.f, 1.f) : A;
				O[3] = uint8(FMath::Clamp(FMath::RoundToInt(AS * 255.f), 0, 255));
			}
		}
		return Dst;
	}

	UTexture2D* MakePageTexture(const TArray64<uint8>& Pixels, int32 W, int32 H, const FString& Name)
	{
		UTexture2D* Tex = NewObject<UTexture2D>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(),
			FName(*Name)), RF_Transient);
		FTexturePlatformData* PD = new FTexturePlatformData();
		PD->SizeX = W;
		PD->SizeY = H;
		PD->PixelFormat = PF_B8G8R8A8;
		FTexture2DMipMap* Mip = new FTexture2DMipMap(W, H, 1);
		PD->Mips.Add(Mip);
		Mip->BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(Mip->BulkData.Realloc(Pixels.Num()), Pixels.GetData(), Pixels.Num());
		Mip->BulkData.Unlock();
		Tex->SetPlatformData(PD);
		Tex->SRGB = true;
		Tex->LODGroup = TEXTUREGROUP_UI;
		Tex->NeverStream = true;
		Tex->Filter = TF_Bilinear;
		Tex->AddressX = TA_Clamp;
		Tex->AddressY = TA_Clamp;
		Tex->UpdateResource();
		return Tex;
	}

	UFont* BuildFontAtHeight(const FString& ObjectPath, int32 PixelHeight)
	{
		using namespace ArenaRuntimePrivate;
		const FFontJson* FJ = GetFontJson(ObjectPath);
		if (!FJ || FJ->CellHeight <= 0)
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>& J = FJ->Json;
		const float Q = PixelHeight > 0 ? float(PixelHeight) / float(FJ->CellHeight) : 1.f;
		const TArray<TSharedPtr<FJsonValue>>* Pages = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Chars = nullptr;
		if (!J->TryGetArrayField(TEXT("pages"), Pages) || !J->TryGetArrayField(TEXT("chars"), Chars) || Pages->Num() == 0 || Chars->Num() == 0)
		{
			return nullptr;
		}
		static IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const auto Scaled = [Q](int32 V) { return FMath::RoundToInt(float(V) * Q); };
		TArray<UTexture2D*> Textures;
		TArray<FIntPoint> SrcSizes;
		for (const TSharedPtr<FJsonValue>& P : *Pages)
		{
			const TSharedPtr<FJsonObject>* PO = nullptr;
			FString File;
			if (!P.IsValid() || !P->TryGetObject(PO) || !(*PO)->TryGetStringField(TEXT("file"), File))
			{
				return nullptr;
			}
			TArray64<uint8> Bytes;
			const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
			TArray64<uint8> Raw;
			if (!FFileHelper::LoadFileToArray(Bytes, *(ArenaContent::GetCacheDir() / File)) || !Wrapper.IsValid()
				|| !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()) || !Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw))
			{
				UE_LOG(LogOpenUT2k4, Warning, TEXT("RuntimeContent: font %s: cannot read page %s"), *ObjectPath, *File);
				return nullptr;
			}
			const int32 SW = int32(Wrapper->GetWidth()), SH = int32(Wrapper->GetHeight());
			if (SW <= 0 || SH <= 0 || Raw.Num() != int64(SW) * SH * 4)
			{
				return nullptr;
			}
			const int32 DW = FMath::Max(1, Scaled(SW)), DH = FMath::Max(1, Scaled(SH));
			const TArray64<uint8> Out = ResamplePage(Raw, SW, SH, DW, DH, Q);
			Textures.Add(MakePageTexture(Out, DW, DH, FString::Printf(TEXT("FontPage_%s_%d_%d"), *AssetName(ObjectPath), PixelHeight, Textures.Num())));
			SrcSizes.Add(FIntPoint(SW, SH));
		}
		UFont* Font = NewObject<UFont>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UFont::StaticClass(),
			FName(*FString::Printf(TEXT("Font_%s_%d"), *AssetName(ObjectPath), PixelHeight))), RF_Transient);
		Font->FontCacheType = EFontCacheType::Offline;
		Font->Kerning = Scaled(int32(J->GetNumberField(TEXT("kerning"))));
		bool bRemapped = false;
		J->TryGetBoolField(TEXT("remapped"), bRemapped);
		Font->IsRemapped = bRemapped ? 1 : 0;
		Font->Characters.Reserve(Chars->Num());
		for (const TSharedPtr<FJsonValue>& C : *Chars)
		{
			FFontCharacter Ch;
			const TArray<TSharedPtr<FJsonValue>>* E = nullptr;
			if (C.IsValid() && C->TryGetArray(E) && E->Num() >= 5)
			{
				const int32 U = int32((*E)[0]->AsNumber()), V = int32((*E)[1]->AsNumber());
				const int32 US = int32((*E)[2]->AsNumber()), VS = int32((*E)[3]->AsNumber());
				Ch.TextureIndex = uint8((*E)[4]->AsNumber());
				// cell edges rounded separately: neighbouring cells stay adjacent, never overlap
				Ch.StartU = Scaled(U);
				Ch.StartV = Scaled(V);
				Ch.USize = Scaled(U + US) - Ch.StartU;
				Ch.VSize = Scaled(V + VS) - Ch.StartV;
				if (!Textures.IsValidIndex(Ch.TextureIndex))
				{
					Ch = FFontCharacter();
				}
			}
			Font->Characters.Add(Ch);
		}
		const TArray<TSharedPtr<FJsonValue>>* Remap = nullptr;
		if (J->TryGetArrayField(TEXT("remap"), Remap))
		{
			for (const TSharedPtr<FJsonValue>& R : *Remap)
			{
				const TArray<TSharedPtr<FJsonValue>>* E = nullptr;
				if (R.IsValid() && R->TryGetArray(E) && E->Num() >= 2)
				{
					Font->CharRemap.Add(uint16((*E)[0]->AsNumber()), uint16((*E)[1]->AsNumber()));
				}
			}
		}
		for (UTexture2D* Tex : Textures)
		{
			Font->Textures.Add(Tex);
		}
		Font->CacheCharacterCountAndMaxCharHeight();
		UE_LOG(LogOpenUT2k4, Log, TEXT("RuntimeContent: font %s at %d px (x%.3f of %d px, %d pages)"), *ObjectPath, PixelHeight, Q,
			FJ->CellHeight, Textures.Num());
		return Font;
	}
}

int32 ArenaRuntimeContent::GetFontCellHeight(const FString& ObjectPath)
{
	check(IsInGameThread());
	if (!IsActive() || ObjectPath.IsEmpty())
	{
		return 0;
	}
	const ArenaRuntimeFontsLocal::FFontJson* F = ArenaRuntimeFontsLocal::GetFontJson(ObjectPath);
	return F ? F->CellHeight : 0;
}

UFont* ArenaRuntimeContent::LoadFontAtHeight(const FString& ObjectPath, int32 PixelHeight)
{
	using namespace ArenaRuntimeFontsLocal;
	check(IsInGameThread());
	if (!IsActive() || ObjectPath.IsEmpty())
	{
		return nullptr;
	}
	struct FVariant
	{
		TStrongObjectPtr<UFont> Font;
		uint64 LastUsed = 0;
	};
	static TMap<FString, FVariant> Variants;
	static TMap<FString, double> VariantMisses;
	static uint64 Clock = 0;
	const int32 Cell = GetFontCellHeight(ObjectPath);
	if (Cell <= 0)
	{
		return nullptr;
	}
	const int32 Height = PixelHeight > 0 ? FMath::Clamp(PixelHeight, 4, 512) : Cell;
	const FString Key = FString::Printf(TEXT("%s@%d"), *ObjectPath.ToLower(), Height);
	++Clock;
	if (FVariant* Found = Variants.Find(Key))
	{
		Found->LastUsed = Clock;
		return Found->Font.Get();
	}
	const double Now = FPlatformTime::Seconds();
	if (const double* Missed = VariantMisses.Find(Key))
	{
		if (Now - *Missed < RetrySeconds)
		{
			return nullptr;
		}
	}
	UFont* Font = BuildFontAtHeight(ObjectPath, Height);
	if (!Font)
	{
		VariantMisses.Add(Key, Now);
		return nullptr;
	}
	// a window being resized makes a new size every frame: keep the most recently used few dozen
	constexpr int32 MaxVariants = 48;
	if (Variants.Num() >= MaxVariants)
	{
		FString Oldest;
		uint64 OldestUse = MAX_uint64;
		for (const TPair<FString, FVariant>& V : Variants)
		{
			if (V.Value.LastUsed < OldestUse)
			{
				OldestUse = V.Value.LastUsed;
				Oldest = V.Key;
			}
		}
		Variants.Remove(Oldest);
	}
	FVariant& V = Variants.Add(Key);
	V.Font = TStrongObjectPtr<UFont>(Font);
	V.LastUsed = Clock;
	return Font;
}
