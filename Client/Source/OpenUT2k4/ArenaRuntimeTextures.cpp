// Runtime textures: cache PNG -> UTexture2D with a full mip chain, BC1/BC3 compressed on worker threads.
//
// Same settings the editor importer gives its textures (Tools/ImportUT2004.py Importer.import_textures):
// sRGB, world LOD group, wrap addressing, mipmapped, no normal-map detection. The cooked importer lets the
// engine compress to DXT1/DXT5; cooked builds have no texture compressor, so a small block encoder here
// does the same (UT2004 shipped its textures as DXT already, so the quality loss is negligible).
// arena.RuntimeTextureCompression 0 keeps uncompressed BGRA8 (4-8x the memory).

#include "ArenaRuntimeContentPrivate.h"
#include "OpenUT2k4.h"
#include "Async/ParallelFor.h"
#include "Engine/Texture2D.h"
#include "HAL/IConsoleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "TextureResource.h"

namespace ArenaRuntimeTextures
{
	using namespace ArenaRuntimePrivate;

	static TAutoConsoleVariable<int32> CVarCompression(
		TEXT("arena.RuntimeTextureCompression"), 1,
		TEXT("Runtime content textures: 1 = BC1/BC3 block compression (default), 0 = uncompressed BGRA8."),
		ECVF_Default);

	struct FMip
	{
		int32 W = 0, H = 0;
		TArray64<uint8> Data;
	};

	struct FDecoded
	{
		EPixelFormat Format = PF_B8G8R8A8;
		TArray<FMip> Mips;
		bool bValid = false;
	};
	using FDecodedPtr = TSharedPtr<FDecoded, ESPMode::ThreadSafe>;

	// ---- block compression (BC1 colour, BC3 = BC4-style alpha + BC1 colour) -----------------------

	uint16 To565(const float C[3])
	{
		const int32 R = FMath::Clamp(int32(C[0] * 31.f / 255.f + 0.5f), 0, 31);
		const int32 G = FMath::Clamp(int32(C[1] * 63.f / 255.f + 0.5f), 0, 63);
		const int32 B = FMath::Clamp(int32(C[2] * 31.f / 255.f + 0.5f), 0, 31);
		return uint16((R << 11) | (G << 5) | B);
	}
	void From565(uint16 V, int32 Out[3])
	{
		const int32 R = (V >> 11) & 31, G = (V >> 5) & 63, B = V & 31;
		Out[0] = (R << 3) | (R >> 2);
		Out[1] = (G << 2) | (G >> 4);
		Out[2] = (B << 3) | (B >> 2);
	}

	/** 16 BGRA pixels -> 8 bytes of BC1 colour (always 4-colour mode). */
	void EncodeColorBlock(const uint8 Px[16][4], uint8* Out)
	{
		// principal axis of the block colours (a few power iterations on the covariance)
		float Mean[3] = { 0, 0, 0 };
		for (int32 i = 0; i < 16; ++i) { Mean[0] += Px[i][2]; Mean[1] += Px[i][1]; Mean[2] += Px[i][0]; }
		for (float& M : Mean) { M /= 16.f; }
		float Cov[6] = { 0, 0, 0, 0, 0, 0 };
		for (int32 i = 0; i < 16; ++i)
		{
			const float R = Px[i][2] - Mean[0], G = Px[i][1] - Mean[1], B = Px[i][0] - Mean[2];
			Cov[0] += R * R; Cov[1] += R * G; Cov[2] += R * B; Cov[3] += G * G; Cov[4] += G * B; Cov[5] += B * B;
		}
		float Axis[3] = { 0.577f, 0.577f, 0.577f };
		for (int32 It = 0; It < 4; ++It)
		{
			const float X = Cov[0] * Axis[0] + Cov[1] * Axis[1] + Cov[2] * Axis[2];
			const float Y = Cov[1] * Axis[0] + Cov[3] * Axis[1] + Cov[4] * Axis[2];
			const float Z = Cov[2] * Axis[0] + Cov[4] * Axis[1] + Cov[5] * Axis[2];
			const float L = FMath::Sqrt(X * X + Y * Y + Z * Z);
			if (L < 1e-6f)
			{
				break;
			}
			Axis[0] = X / L; Axis[1] = Y / L; Axis[2] = Z / L;
		}
		float MinT = FLT_MAX, MaxT = -FLT_MAX;
		for (int32 i = 0; i < 16; ++i)
		{
			const float T = (Px[i][2] - Mean[0]) * Axis[0] + (Px[i][1] - Mean[1]) * Axis[1] + (Px[i][0] - Mean[2]) * Axis[2];
			MinT = FMath::Min(MinT, T);
			MaxT = FMath::Max(MaxT, T);
		}
		// inset the end points a little (reduces the error of the interpolated colours)
		const float Inset = (MaxT - MinT) / 32.f;
		MinT += Inset;
		MaxT -= Inset;
		float C0[3], C1[3];
		for (int32 k = 0; k < 3; ++k)
		{
			C0[k] = FMath::Clamp(Mean[k] + Axis[k] * MaxT, 0.f, 255.f);
			C1[k] = FMath::Clamp(Mean[k] + Axis[k] * MinT, 0.f, 255.f);
		}
		uint16 E0 = To565(C0), E1 = To565(C1);
		if (E0 < E1)
		{
			Swap(E0, E1);
		}
		uint32 Indices = 0;
		if (E0 != E1)
		{
			int32 P[4][3];
			From565(E0, P[0]);
			From565(E1, P[1]);
			for (int32 k = 0; k < 3; ++k)
			{
				P[2][k] = (2 * P[0][k] + P[1][k]) / 3;
				P[3][k] = (P[0][k] + 2 * P[1][k]) / 3;
			}
			for (int32 i = 0; i < 16; ++i)
			{
				int32 Best = 0, BestD = INT_MAX;
				for (int32 c = 0; c < 4; ++c)
				{
					const int32 dr = Px[i][2] - P[c][0], dg = Px[i][1] - P[c][1], db = Px[i][0] - P[c][2];
					const int32 D = dr * dr * 3 + dg * dg * 4 + db * db * 2;
					if (D < BestD) { BestD = D; Best = c; }
				}
				Indices |= uint32(Best) << (i * 2);
			}
		}
		Out[0] = uint8(E0 & 0xFF); Out[1] = uint8(E0 >> 8);
		Out[2] = uint8(E1 & 0xFF); Out[3] = uint8(E1 >> 8);
		Out[4] = uint8(Indices); Out[5] = uint8(Indices >> 8); Out[6] = uint8(Indices >> 16); Out[7] = uint8(Indices >> 24);
	}

	/** 16 alpha values -> 8 bytes of BC3 alpha (8-value mode). */
	void EncodeAlphaBlock(const uint8 Px[16][4], uint8* Out)
	{
		int32 A0 = 0, A1 = 255;
		for (int32 i = 0; i < 16; ++i) { A0 = FMath::Max<int32>(A0, Px[i][3]); A1 = FMath::Min<int32>(A1, Px[i][3]); }
		Out[0] = uint8(A0);
		Out[1] = uint8(A1);
		uint64 Bits = 0;
		if (A0 != A1)
		{
			int32 Pal[8];
			Pal[0] = A0; Pal[1] = A1;
			for (int32 k = 1; k < 7; ++k) { Pal[k + 1] = ((7 - k) * A0 + k * A1) / 7; }
			for (int32 i = 0; i < 16; ++i)
			{
				int32 Best = 0, BestD = INT_MAX;
				for (int32 c = 0; c < 8; ++c)
				{
					const int32 D = FMath::Abs(Px[i][3] - Pal[c]);
					if (D < BestD) { BestD = D; Best = c; }
				}
				Bits |= uint64(Best) << (i * 3);
			}
		}
		for (int32 k = 0; k < 6; ++k) { Out[2 + k] = uint8(Bits >> (k * 8)); }
	}

	/** BGRA8 mip -> BC1 / BC3 (blocks padded by edge clamping for mips smaller than 4x4). */
	TArray64<uint8> Compress(const FMip& Mip, bool bAlpha)
	{
		const int32 BW = FMath::Max(1, (Mip.W + 3) / 4), BH = FMath::Max(1, (Mip.H + 3) / 4);
		const int32 BlockBytes = bAlpha ? 16 : 8;
		TArray64<uint8> Out;
		Out.SetNumUninitialized(int64(BW) * BH * BlockBytes);
		uint8 Px[16][4];
		for (int32 by = 0; by < BH; ++by)
		{
			for (int32 bx = 0; bx < BW; ++bx)
			{
				for (int32 y = 0; y < 4; ++y)
				{
					for (int32 x = 0; x < 4; ++x)
					{
						const int32 sx = FMath::Min(bx * 4 + x, Mip.W - 1), sy = FMath::Min(by * 4 + y, Mip.H - 1);
						FMemory::Memcpy(Px[y * 4 + x], &Mip.Data[(int64(sy) * Mip.W + sx) * 4], 4);
					}
				}
				uint8* Dst = &Out[(int64(by) * BW + bx) * BlockBytes];
				if (bAlpha)
				{
					EncodeAlphaBlock(Px, Dst);
					EncodeColorBlock(Px, Dst + 8);
				}
				else
				{
					EncodeColorBlock(Px, Dst);
				}
			}
		}
		return Out;
	}

	/** 2x2 box filter (odd edges clamp). */
	FMip Downsample(const FMip& Src)
	{
		FMip Dst;
		Dst.W = FMath::Max(1, Src.W / 2);
		Dst.H = FMath::Max(1, Src.H / 2);
		Dst.Data.SetNumUninitialized(int64(Dst.W) * Dst.H * 4);
		for (int32 y = 0; y < Dst.H; ++y)
		{
			const int32 y0 = FMath::Min(y * 2, Src.H - 1), y1 = FMath::Min(y * 2 + 1, Src.H - 1);
			for (int32 x = 0; x < Dst.W; ++x)
			{
				const int32 x0 = FMath::Min(x * 2, Src.W - 1), x1 = FMath::Min(x * 2 + 1, Src.W - 1);
				const uint8* P00 = &Src.Data[(int64(y0) * Src.W + x0) * 4];
				const uint8* P01 = &Src.Data[(int64(y0) * Src.W + x1) * 4];
				const uint8* P10 = &Src.Data[(int64(y1) * Src.W + x0) * 4];
				const uint8* P11 = &Src.Data[(int64(y1) * Src.W + x1) * 4];
				uint8* D = &Dst.Data[(int64(y) * Dst.W + x) * 4];
				for (int32 c = 0; c < 4; ++c)
				{
					D[c] = uint8((P00[c] + P01[c] + P10[c] + P11[c] + 2) / 4);
				}
			}
		}
		return Dst;
	}

	FDecodedPtr Decode(const FString& ObjectPath)
	{
		FDecodedPtr Out = MakeShared<FDecoded, ESPMode::ThreadSafe>();
		const FString Base = FindByObjectPath(TEXT("textures"), ObjectPath);
		if (Base.IsEmpty())
		{
			return Out;
		}
		const TSharedPtr<FJsonObject> Meta = LoadJson(Base + TEXT(".json"));
		FString PngName;
		if (!Meta.IsValid() || !Meta->TryGetStringField(TEXT("png"), PngName))
		{
			return Out;
		}
		TArray64<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *(FPaths::GetPath(Base) / PngName)))
		{
			return Out;
		}
		static IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
		FMip Top;
		if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()) || !Wrapper->GetRaw(ERGBFormat::BGRA, 8, Top.Data))
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("RuntimeContent: cannot decode %s"), *PngName);
			return Out;
		}
		Top.W = int32(Wrapper->GetWidth());
		Top.H = int32(Wrapper->GetHeight());
		if (Top.W <= 0 || Top.H <= 0 || Top.Data.Num() != int64(Top.W) * Top.H * 4)
		{
			return Out;
		}
		bool bHasAlpha = false;
		Meta->TryGetBoolField(TEXT("has_alpha"), bHasAlpha);
		if (!bHasAlpha)
		{
			// trust the pixels over the metadata (procedural / baked textures)
			for (int64 i = 3; i < Top.Data.Num(); i += 4)
			{
				if (Top.Data[i] != 255) { bHasAlpha = true; break; }
			}
		}
		Out->Mips.Add(MoveTemp(Top));
		while (Out->Mips.Last().W > 1 || Out->Mips.Last().H > 1)
		{
			FMip Next = Downsample(Out->Mips.Last());
			Out->Mips.Add(MoveTemp(Next));
		}
		const bool bBlockAligned = Out->Mips[0].W % 4 == 0 && Out->Mips[0].H % 4 == 0;
		if (CVarCompression.GetValueOnAnyThread() != 0 && bBlockAligned)
		{
			Out->Format = bHasAlpha ? PF_DXT5 : PF_DXT1;
			for (FMip& M : Out->Mips)
			{
				M.Data = Compress(M, bHasAlpha);
			}
		}
		Out->bValid = true;
		return Out;
	}

	/** Decoded data waiting for the game thread (filled by PrefetchTextures). */
	FCriticalSection PendingLock;
	TMap<FString, FDecodedPtr> Pending;

	FDecodedPtr TakePending(const FString& Key)
	{
		FScopeLock Lock(&PendingLock);
		FDecodedPtr Out;
		Pending.RemoveAndCopyValue(Key, Out);
		return Out;
	}
}

void ArenaRuntimePrivate::PrefetchTextures(const TArray<FString>& ObjectPaths)
{
	using namespace ArenaRuntimeTextures;
	TArray<FString> Todo;
	{
		FScopeLock Lock(&PendingLock);
		for (const FString& P : ObjectPaths)
		{
			if (!Pending.Contains(P.ToLower()))
			{
				Todo.AddUnique(P);
			}
		}
	}
	FScopeUs Timer(Stats().TextureUs);
	FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper")); // load on the game thread, not in a worker
	ParallelFor(Todo.Num(), [&](int32 i)
	{
		FDecodedPtr D = Decode(Todo[i]);
		FScopeLock Lock(&PendingLock);
		Pending.Add(Todo[i].ToLower(), D);
	});
}

void ArenaRuntimePrivate::TrimTextures()
{
	using namespace ArenaRuntimeTextures;
	FScopeLock Lock(&PendingLock);
	Pending.Reset();
}

UTexture2D* ArenaRuntimePrivate::BuildTexture(const FString& ObjectPath)
{
	using namespace ArenaRuntimeTextures;
	FDecodedPtr D = TakePending(ObjectPath.ToLower());
	{
		FScopeUs Timer(Stats().TextureUs);
		if (!D.IsValid())
		{
			D = Decode(ObjectPath);
		}
		if (!D->bValid || D->Mips.Num() == 0)
		{
			return nullptr;
		}
		UObject* Outer = AssetOuter();
		UTexture2D* Tex = NewObject<UTexture2D>(Outer, MakeUniqueObjectName(Outer, UTexture2D::StaticClass(), FName(*AssetName(ObjectPath))), RF_Transient);
		FTexturePlatformData* PD = new FTexturePlatformData();
		PD->SizeX = D->Mips[0].W;
		PD->SizeY = D->Mips[0].H;
		PD->PixelFormat = D->Format;
		int64 Bytes = 0;
		for (FMip& M : D->Mips)
		{
			FTexture2DMipMap* Mip = new FTexture2DMipMap(M.W, M.H, 1);
			PD->Mips.Add(Mip);
			Mip->BulkData.Lock(LOCK_READ_WRITE);
			void* Dst = Mip->BulkData.Realloc(M.Data.Num());
			FMemory::Memcpy(Dst, M.Data.GetData(), M.Data.Num());
			Mip->BulkData.Unlock();
			Bytes += M.Data.Num();
		}
		Tex->SetPlatformData(PD);
		Tex->SRGB = true;
		Tex->LODGroup = TEXTUREGROUP_World;
		Tex->NeverStream = true;
		Tex->AddressX = TA_Wrap;
		Tex->AddressY = TA_Wrap;
		Tex->UpdateResource();
		++Stats().Textures;
		Stats().TextureBytes += Bytes;
		return Tex;
	}
}
