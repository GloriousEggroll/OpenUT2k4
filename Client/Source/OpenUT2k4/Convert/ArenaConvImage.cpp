#include "ArenaConvImage.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"

THIRD_PARTY_INCLUDES_START
#include "zlib.h"
THIRD_PARTY_INCLUDES_END

namespace ArenaConvImageLocal
{
	inline void Rgb565(uint32 C, int32 Out[3])
	{
		const int32 R = (C >> 11) & 31, G = (C >> 5) & 63, B = C & 31;
		Out[0] = (R << 3) | (R >> 2);
		Out[1] = (G << 2) | (G >> 4);
		Out[2] = (B << 3) | (B >> 2);
	}

	/** One 4x4 colour block -> 16 RGBA texels. */
	void ColorBlock(const uint8* Blk, bool bForce4, uint8 Out[16][4])
	{
		const uint32 C0 = Blk[0] | (Blk[1] << 8);
		const uint32 C1 = Blk[2] | (Blk[3] << 8);
		const uint32 Bits = Blk[4] | (Blk[5] << 8) | (Blk[6] << 16) | (uint32(Blk[7]) << 24);
		int32 P[4][4];
		Rgb565(C0, P[0]);
		Rgb565(C1, P[1]);
		const bool bFour = bForce4 || C0 > C1;
		for (int32 c = 0; c < 3; ++c)
		{
			P[2][c] = bFour ? (2 * P[0][c] + P[1][c]) / 3 : (P[0][c] + P[1][c]) / 2;
			P[3][c] = bFour ? (P[0][c] + 2 * P[1][c]) / 3 : 0;
		}
		P[0][3] = P[1][3] = P[2][3] = 255;
		P[3][3] = bFour ? 255 : 0;
		for (int32 i = 0; i < 16; ++i)
		{
			const int32 Idx = (Bits >> (2 * i)) & 3;
			for (int32 c = 0; c < 4; ++c)
			{
				Out[i][c] = (uint8)P[Idx][c];
			}
		}
	}

	void PutBlock(FConvImage& Img, int32 Bx, int32 By, const uint8 Px[16][4])
	{
		for (int32 y = 0; y < 4; ++y)
		{
			const int32 Py = By * 4 + y;
			if (Py >= Img.H)
			{
				break;
			}
			for (int32 x = 0; x < 4; ++x)
			{
				const int32 Pxx = Bx * 4 + x;
				if (Pxx >= Img.W)
				{
					break;
				}
				uint8* D = &Img.Pixels[(int64(Py) * Img.W + Pxx) * 4];
				D[0] = Px[y * 4 + x][0];
				D[1] = Px[y * 4 + x][1];
				D[2] = Px[y * 4 + x][2];
				D[3] = Px[y * 4 + x][3];
			}
		}
	}

	uint32 Crc32(const uint8* Data, int64 Len, uint32 Crc = 0)
	{
		return (uint32)crc32(Crc, Data, (uInt)Len);
	}

	void PutBE32(TArray64<uint8>& Out, uint32 V)
	{
		Out.Add(V >> 24);
		Out.Add((V >> 16) & 0xFF);
		Out.Add((V >> 8) & 0xFF);
		Out.Add(V & 0xFF);
	}

	void Chunk(TArray64<uint8>& Out, const char* Tag, const uint8* Data, int64 Len)
	{
		PutBE32(Out, (uint32)Len);
		const int64 TagPos = Out.Num();
		Out.Append((const uint8*)Tag, 4);
		if (Len)
		{
			Out.Append(Data, Len);
		}
		PutBE32(Out, Crc32(Out.GetData() + TagPos, Len + 4));
	}

	// ---- PIL-style separable resampling ----
	struct FCoeffs
	{
		TArray<int32> Min;
		TArray<int32> Count;
		TArray<double> K;
		int32 KSize = 0;
	};

	FCoeffs Precompute(int32 InSize, int32 OutSize, bool bBilinear)
	{
		FCoeffs C;
		const double Scale = double(InSize) / OutSize;
		const double FilterScale = FMath::Max(Scale, 1.0);
		const double Support = (bBilinear ? 1.0 : 0.5) * FilterScale;
		C.KSize = (int32)FMath::CeilToDouble(Support) * 2 + 1;
		C.Min.SetNum(OutSize);
		C.Count.SetNum(OutSize);
		C.K.SetNumZeroed(int64(OutSize) * C.KSize);
		const double SS = 1.0 / FilterScale;
		for (int32 xx = 0; xx < OutSize; ++xx)
		{
			const double Center = (xx + 0.5) * Scale;
			int32 XMin = (int32)(Center - Support + 0.5);
			XMin = FMath::Max(XMin, 0);
			int32 XMax = (int32)(Center + Support + 0.5);
			XMax = FMath::Min(XMax, InSize) - XMin;
			XMax = FMath::Min(XMax, C.KSize);
			double WW = 0.0;
			double* K = &C.K[int64(xx) * C.KSize];
			for (int32 x = 0; x < XMax; ++x)
			{
				const double Arg = (x + XMin - Center + 0.5) * SS;
				double W;
				if (bBilinear)
				{
					const double A = FMath::Abs(Arg);
					W = A < 1.0 ? 1.0 - A : 0.0;
				}
				else
				{
					W = (Arg > -0.5 && Arg <= 0.5) ? 1.0 : 0.0;
				}
				K[x] = W;
				WW += W;
			}
			for (int32 x = 0; x < XMax; ++x)
			{
				K[x] = WW != 0.0 ? K[x] / WW : 0.0;
			}
			C.Min[xx] = XMin;
			C.Count[xx] = XMax;
		}
		return C;
	}

	inline uint8 Clip8(double V)
	{
		const double R = FMath::FloorToDouble(V + 0.5);
		return (uint8)FMath::Clamp(R, 0.0, 255.0);
	}

	/** PIL Image.resize on RGBA (premultiplied as PIL does), 8-bit passes. */
	FConvImage ResizeRGBA(const FConvImage& In, int32 NewW, int32 NewH, bool bBilinear)
	{
		// premultiply (RGBA -> RGBa)
		TArray64<uint8> Src = In.Pixels;
		for (int64 i = 0; i < int64(In.W) * In.H; ++i)
		{
			uint8* P = &Src[i * 4];
			const uint32 A = P[3];
			for (int32 c = 0; c < 3; ++c)
			{
				const uint32 T = P[c] * A + 128;
				P[c] = (uint8)(((T >> 8) + T) >> 8);
			}
		}
		int32 W = In.W, H = In.H;
		if (NewW != W)
		{
			const FCoeffs C = Precompute(W, NewW, bBilinear);
			TArray64<uint8> Dst;
			Dst.SetNumUninitialized(int64(NewW) * H * 4);
			for (int32 y = 0; y < H; ++y)
			{
				for (int32 xx = 0; xx < NewW; ++xx)
				{
					const double* K = &C.K[int64(xx) * C.KSize];
					double Acc[4] = { 0, 0, 0, 0 };
					for (int32 x = 0; x < C.Count[xx]; ++x)
					{
						const uint8* P = &Src[(int64(y) * W + C.Min[xx] + x) * 4];
						for (int32 c = 0; c < 4; ++c)
						{
							Acc[c] += P[c] * K[x];
						}
					}
					uint8* D = &Dst[(int64(y) * NewW + xx) * 4];
					for (int32 c = 0; c < 4; ++c)
					{
						D[c] = Clip8(Acc[c]);
					}
				}
			}
			Src = MoveTemp(Dst);
			W = NewW;
		}
		if (NewH != H)
		{
			const FCoeffs C = Precompute(H, NewH, bBilinear);
			TArray64<uint8> Dst;
			Dst.SetNumUninitialized(int64(W) * NewH * 4);
			for (int32 yy = 0; yy < NewH; ++yy)
			{
				const double* K = &C.K[int64(yy) * C.KSize];
				for (int32 x = 0; x < W; ++x)
				{
					double Acc[4] = { 0, 0, 0, 0 };
					for (int32 y = 0; y < C.Count[yy]; ++y)
					{
						const uint8* P = &Src[(int64(C.Min[yy] + y) * W + x) * 4];
						for (int32 c = 0; c < 4; ++c)
						{
							Acc[c] += P[c] * K[y];
						}
					}
					uint8* D = &Dst[(int64(yy) * W + x) * 4];
					for (int32 c = 0; c < 4; ++c)
					{
						D[c] = Clip8(Acc[c]);
					}
				}
			}
			Src = MoveTemp(Dst);
			H = NewH;
		}
		// unpremultiply (RGBa -> RGBA)
		for (int64 i = 0; i < int64(W) * H; ++i)
		{
			uint8* P = &Src[i * 4];
			const uint32 A = P[3];
			if (A != 255 && A != 0)
			{
				for (int32 c = 0; c < 3; ++c)
				{
					P[c] = (uint8)FMath::Min<uint32>(255u, (255u * P[c]) / A);
				}
			}
		}
		FConvImage Out;
		Out.W = W;
		Out.H = H;
		Out.Channels = 4;
		Out.Pixels = MoveTemp(Src);
		return Out;
	}

	double R4(double V) { return FConvJson::RoundTo(V, 4); }
}

bool FConvImage::HasAlpha() const
{
	if (Channels != 4)
	{
		return false;
	}
	const int64 N = int64(W) * H;
	for (int64 i = 0; i < N; ++i)
	{
		if (Pixels[i * 4 + 3] != 255)
		{
			return true;
		}
	}
	return false;
}

void FConvImage::StripAlpha()
{
	if (Channels != 4)
	{
		return;
	}
	const int64 N = int64(W) * H;
	for (int64 i = 0; i < N; ++i)
	{
		Pixels[i * 3 + 0] = Pixels[i * 4 + 0];
		Pixels[i * 3 + 1] = Pixels[i * 4 + 1];
		Pixels[i * 3 + 2] = Pixels[i * 4 + 2];
	}
	Pixels.SetNum(N * 3);
	Channels = 3;
}

FConvImage FConvImage::ToRGBA8() const
{
	FConvImage Out;
	Out.W = W;
	Out.H = H;
	Out.Channels = 4;
	const int64 N = int64(W) * H;
	Out.Pixels.SetNumUninitialized(N * 4);
	if (Channels == 4)
	{
		Out.Pixels = Pixels;
	}
	else if (Channels == 3)
	{
		for (int64 i = 0; i < N; ++i)
		{
			Out.Pixels[i * 4 + 0] = Pixels[i * 3 + 0];
			Out.Pixels[i * 4 + 1] = Pixels[i * 3 + 1];
			Out.Pixels[i * 4 + 2] = Pixels[i * 3 + 2];
			Out.Pixels[i * 4 + 3] = 255;
		}
	}
	else
	{
		const uint16* G = (const uint16*)Pixels.GetData();
		uint16 Max = 0;
		for (int64 i = 0; i < N; ++i)
		{
			Max = FMath::Max(Max, G[i]);
		}
		const double Div = Max > 255 ? 65535.0 : 255.0;
		for (int64 i = 0; i < N; ++i)
		{
			const uint8 V = (uint8)FMath::Clamp(int32(G[i] / Div * 255.0), 0, 255);
			Out.Pixels[i * 4 + 0] = Out.Pixels[i * 4 + 1] = Out.Pixels[i * 4 + 2] = V;
			Out.Pixels[i * 4 + 3] = 255;
		}
	}
	return Out;
}

FConvImage FConvImage::ResizeBilinear(int32 NewW, int32 NewH) const
{
	return ArenaConvImageLocal::ResizeRGBA(ToRGBA8(), NewW, NewH, true);
}

namespace ArenaConvImage
{
	using namespace ArenaConvImageLocal;

	FString FormatName(int32 Format)
	{
		static const TCHAR* Names[] = { TEXT("P8"), TEXT("RGBA7"), TEXT("RGB16"), TEXT("DXT1"), TEXT("RGB8"), TEXT("RGBA8"), TEXT("NODATA"),
			TEXT("DXT3"), TEXT("DXT5"), TEXT("L8"), TEXT("G16"), TEXT("RRRGGGBBB") };
		return Format >= 0 && Format <= 11 ? FString(Names[Format]) : FString::Printf(TEXT("UNKNOWN_%d"), Format);
	}

	int64 RequiredBytes(int32 Format, int32 W, int32 H)
	{
		const int64 Npx = int64(W) * H;
		const int64 Blocks = int64(FMath::Max(1, (W + 3) / 4)) * FMath::Max(1, (H + 3) / 4);
		switch ((ETexFormat)Format)
		{
		case ETexFormat::DXT1: return Blocks * 8;
		case ETexFormat::DXT3:
		case ETexFormat::DXT5: return Blocks * 16;
		case ETexFormat::RGBA8: return Npx * 4;
		case ETexFormat::RGB8: return Npx * 3;
		case ETexFormat::P8:
		case ETexFormat::L8: return Npx;
		case ETexFormat::G16:
		case ETexFormat::RGB16: return Npx * 2;
		default: return -1;
		}
	}

	bool Decode(int32 Format, const uint8* Data, int64 Len, int32 W, int32 H, const uint8* Palette, FConvImage& Out, FString& Error)
	{
		const int64 Npx = int64(W) * H;
		Out.W = W;
		Out.H = H;
		Out.Channels = 4;
		const ETexFormat F = (ETexFormat)Format;
		const int32 Bw = FMath::Max(1, (W + 3) / 4), Bh = FMath::Max(1, (H + 3) / 4);
		if (F == ETexFormat::DXT1 || F == ETexFormat::DXT3 || F == ETexFormat::DXT5)
		{
			const int32 BSize = F == ETexFormat::DXT1 ? 8 : 16;
			const int64 Need = int64(Bw) * Bh * BSize;
			if (Len < Need)
			{
				Error = FString::Printf(TEXT("DXT data too short (%lld < %lld)"), Len, Need);
				return false;
			}
			Out.Pixels.SetNumUninitialized(Npx * 4);
			uint8 Px[16][4];
			for (int32 By = 0; By < Bh; ++By)
			{
				for (int32 Bx = 0; Bx < Bw; ++Bx)
				{
					const uint8* B = Data + (int64(By) * Bw + Bx) * BSize;
					if (F == ETexFormat::DXT1)
					{
						ColorBlock(B, false, Px);
					}
					else if (F == ETexFormat::DXT3)
					{
						ColorBlock(B + 8, true, Px);
						for (int32 k = 0; k < 8; ++k)
						{
							Px[2 * k][3] = (B[k] & 0x0F) * 17;
							Px[2 * k + 1][3] = (B[k] >> 4) * 17;
						}
					}
					else
					{
						ColorBlock(B + 8, true, Px);
						const int32 A0 = B[0], A1 = B[1];
						uint64 Bits = 0;
						for (int32 i = 0; i < 6; ++i)
						{
							Bits |= uint64(B[2 + i]) << (8 * i);
						}
						int32 Pal[8];
						Pal[0] = A0;
						Pal[1] = A1;
						if (A0 > A1)
						{
							for (int32 i = 1; i <= 6; ++i)
							{
								Pal[1 + i] = ((7 - i) * A0 + i * A1) / 7;
							}
						}
						else
						{
							for (int32 i = 1; i <= 4; ++i)
							{
								Pal[1 + i] = ((5 - i) * A0 + i * A1) / 5;
							}
							Pal[6] = 0;
							Pal[7] = 255;
						}
						for (int32 i = 0; i < 16; ++i)
						{
							Px[i][3] = (uint8)Pal[(Bits >> (3 * i)) & 7];
						}
					}
					PutBlock(Out, Bx, By, Px);
				}
			}
			return true;
		}
		switch (F)
		{
		case ETexFormat::RGBA8:
			if (Len < Npx * 4) { Error = TEXT("RGBA8 data too short"); return false; }
			Out.Pixels.SetNumUninitialized(Npx * 4);
			for (int64 i = 0; i < Npx; ++i)
			{
				Out.Pixels[i * 4 + 0] = Data[i * 4 + 2];
				Out.Pixels[i * 4 + 1] = Data[i * 4 + 1];
				Out.Pixels[i * 4 + 2] = Data[i * 4 + 0];
				Out.Pixels[i * 4 + 3] = Data[i * 4 + 3];
			}
			return true;
		case ETexFormat::RGB8:
			if (Len < Npx * 3) { Error = TEXT("RGB8 data too short"); return false; }
			Out.Pixels.SetNumUninitialized(Npx * 4);
			for (int64 i = 0; i < Npx; ++i)
			{
				Out.Pixels[i * 4 + 0] = Data[i * 3 + 2];
				Out.Pixels[i * 4 + 1] = Data[i * 3 + 1];
				Out.Pixels[i * 4 + 2] = Data[i * 3 + 0];
				Out.Pixels[i * 4 + 3] = 255;
			}
			return true;
		case ETexFormat::P8:
			if (Len < Npx) { Error = TEXT("P8 data too short"); return false; }
			if (!Palette) { Error = TEXT("P8 texture without palette"); return false; }
			Out.Pixels.SetNumUninitialized(Npx * 4);
			for (int64 i = 0; i < Npx; ++i)
			{
				FMemory::Memcpy(&Out.Pixels[i * 4], Palette + Data[i] * 4, 4);
			}
			return true;
		case ETexFormat::L8:
			if (Len < Npx) { Error = TEXT("L8 data too short"); return false; }
			Out.Pixels.SetNumUninitialized(Npx * 4);
			for (int64 i = 0; i < Npx; ++i)
			{
				Out.Pixels[i * 4 + 0] = Out.Pixels[i * 4 + 1] = Out.Pixels[i * 4 + 2] = Data[i];
				Out.Pixels[i * 4 + 3] = 255;
			}
			return true;
		case ETexFormat::G16:
			if (Len < Npx * 2) { Error = TEXT("G16 data too short"); return false; }
			Out.Channels = 1;
			Out.Pixels.SetNumUninitialized(Npx * 2);
			FMemory::Memcpy(Out.Pixels.GetData(), Data, Npx * 2);
			return true;
		case ETexFormat::RGB16:
			if (Len < Npx * 2) { Error = TEXT("RGB16 data too short"); return false; }
			Out.Pixels.SetNumUninitialized(Npx * 4);
			for (int64 i = 0; i < Npx; ++i)
			{
				int32 C[3];
				Rgb565(Data[i * 2] | (Data[i * 2 + 1] << 8), C);
				Out.Pixels[i * 4 + 0] = (uint8)C[0];
				Out.Pixels[i * 4 + 1] = (uint8)C[1];
				Out.Pixels[i * 4 + 2] = (uint8)C[2];
				Out.Pixels[i * 4 + 3] = 255;
			}
			return true;
		default:
			Error = FString::Printf(TEXT("unsupported texture format %s"), *FormatName(Format));
			return false;
		}
	}

	bool EncodePng(const FConvImage& Image, TArray64<uint8>& Out, int32 Level)
	{
		const int32 Ch = Image.Channels;
		const int32 Depth = Image.Is16() ? 16 : 8;
		const int32 ColorType = Ch == 1 ? 0 : Ch == 3 ? 2 : 6;
		const int64 RowBytes = int64(Image.W) * Ch * (Depth / 8);
		TArray64<uint8> Raw;
		Raw.SetNumUninitialized((RowBytes + 1) * Image.H);
		for (int32 y = 0; y < Image.H; ++y)
		{
			uint8* Row = &Raw[(RowBytes + 1) * y];
			Row[0] = 0; // filter: none
			const uint8* Src = Image.Pixels.GetData() + RowBytes * y;
			if (Depth == 16)
			{
				for (int64 i = 0; i < RowBytes; i += 2)
				{
					Row[1 + i] = Src[i + 1]; // big endian
					Row[2 + i] = Src[i];
				}
			}
			else
			{
				FMemory::Memcpy(Row + 1, Src, RowBytes);
			}
		}
		uLongf DestLen = compressBound((uLong)Raw.Num());
		TArray64<uint8> Z;
		Z.SetNumUninitialized(DestLen);
		if (compress2(Z.GetData(), &DestLen, Raw.GetData(), (uLong)Raw.Num(), Level) != Z_OK)
		{
			return false;
		}
		Out.Reset(int64(DestLen) + 128);
		static const uint8 Sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
		Out.Append(Sig, 8);
		uint8 Ihdr[13];
		Ihdr[0] = Image.W >> 24; Ihdr[1] = (Image.W >> 16) & 0xFF; Ihdr[2] = (Image.W >> 8) & 0xFF; Ihdr[3] = Image.W & 0xFF;
		Ihdr[4] = Image.H >> 24; Ihdr[5] = (Image.H >> 16) & 0xFF; Ihdr[6] = (Image.H >> 8) & 0xFF; Ihdr[7] = Image.H & 0xFF;
		Ihdr[8] = (uint8)Depth;
		Ihdr[9] = (uint8)ColorType;
		Ihdr[10] = Ihdr[11] = Ihdr[12] = 0;
		Chunk(Out, "IHDR", Ihdr, 13);
		Chunk(Out, "IDAT", Z.GetData(), DestLen);
		Chunk(Out, "IEND", nullptr, 0);
		return true;
	}

	bool LoadPng(const FString& Path, FConvImage& Out, bool& bOutHasAlphaChannel)
	{
		TArray64<uint8> File;
		if (!FFileHelper::LoadFileToArray(File, *Path, FILEREAD_Silent) || File.Num() < 33)
		{
			return false;
		}
		const int32 Depth = File[24];
		const int32 ColorType = File[25];
		bOutHasAlphaChannel = ColorType == 4 || ColorType == 6;
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> W = Module.CreateImageWrapper(EImageFormat::PNG);
		if (!W.IsValid() || !W->SetCompressed(File.GetData(), File.Num()))
		{
			return false;
		}
		Out.W = (int32)W->GetWidth();
		Out.H = (int32)W->GetHeight();
		TArray64<uint8> Raw;
		if (ColorType == 0 && Depth == 16)
		{
			if (!W->GetRaw(ERGBFormat::Gray, 16, Raw))
			{
				return false;
			}
			Out.Channels = 1;
			Out.Pixels = MoveTemp(Raw);
			return true;
		}
		if (!W->GetRaw(ERGBFormat::RGBA, 8, Raw))
		{
			return false;
		}
		Out.Channels = 4;
		Out.Pixels = MoveTemp(Raw);
		return true;
	}

	FConvJson Stats(const FConvImage& Image, bool bHasAlphaChannel)
	{
		constexpr int32 Sample = 128;
		FConvImage Rgba = Image.ToRGBA8();
		const int32 W = Image.W, H = Image.H;
		if (FMath::Max(W, H) > Sample)
		{
			const double F = double(FMath::Max(W, H)) / Sample;
			Rgba = ResizeRGBA(Rgba, FMath::Max(1, int32(W / F)), FMath::Max(1, int32(H / F)), false);
		}
		const int64 N = int64(Rgba.W) * Rgba.H;
		double Mean[3] = { 0, 0, 0 }, LumSum = 0, LumSq = 0, Chroma = 0, Black = 0, White = 0, Alpha = 0;
		TArray<double> Lums;
		Lums.SetNumUninitialized(N);
		for (int64 i = 0; i < N; ++i)
		{
			const uint8* P = &Rgba.Pixels[i * 4];
			const double R = P[0] / 255.0, G = P[1] / 255.0, B = P[2] / 255.0;
			const double L = R * 0.2126 + G * 0.7152 + B * 0.0722;
			Lums[i] = L;
			Mean[0] += R;
			Mean[1] += G;
			Mean[2] += B;
			LumSum += L;
			Chroma += FMath::Max3(R, G, B) - FMath::Min3(R, G, B);
			Black += L < 0.04 ? 1 : 0;
			White += L > 0.94 ? 1 : 0;
			Alpha += P[3] / 255.0;
		}
		const double Inv = N ? 1.0 / N : 0.0;
		const double LumMean = LumSum * Inv;
		for (int64 i = 0; i < N; ++i)
		{
			LumSq += (Lums[i] - LumMean) * (Lums[i] - LumMean);
		}
		FConvJson S = FConvJson::Object();
		S.Set(TEXT("w"), W);
		S.Set(TEXT("h"), H);
		FConvJson M = FConvJson::Array();
		for (int32 c = 0; c < 3; ++c)
		{
			M.Add(R4(Mean[c] * Inv));
		}
		S.Set(TEXT("mean"), MoveTemp(M));
		S.Set(TEXT("lum"), R4(LumMean));
		S.Set(TEXT("std"), R4(FMath::Sqrt(LumSq * Inv)));
		S.Set(TEXT("chroma"), R4(Chroma * Inv));
		S.Set(TEXT("black"), R4(Black * Inv));
		S.Set(TEXT("white"), R4(White * Inv));
		S.Set(TEXT("has_alpha"), bHasAlphaChannel);
		S.Set(TEXT("alpha"), R4(Alpha * Inv));
		return S;
	}
}
