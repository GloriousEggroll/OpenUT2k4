#pragma once

#include "CoreMinimal.h"
#include "ArenaConvJson.h"

/**
 * Texture pixel decoding (Tools/ut2k4/texture.py), PNG writing (png.py) and the texture statistics of
 * Tools/ut2k4_materials.py (texture_stats.json), for the content converter.
 */
struct FConvImage
{
	int32 W = 0;
	int32 H = 0;
	/** 4 = RGBA8, 3 = RGB8, 1 = 16-bit gray (G16 height maps). */
	int32 Channels = 4;
	TArray64<uint8> Pixels;

	bool Is16() const { return Channels == 1; }
	/** Any alpha below 255 (RGBA only). */
	bool HasAlpha() const;
	/** RGBA -> RGB (drops alpha). */
	void StripAlpha();
	/** Any channel layout -> RGBA8 (16-bit gray scaled like PIL: /65535 when the max is above 255, else /255). */
	FConvImage ToRGBA8() const;
	/** Bilinear resize (PIL BILINEAR, used by the Combiner bake). RGBA8 only. */
	FConvImage ResizeBilinear(int32 NewW, int32 NewH) const;
};

namespace ArenaConvImage
{
	enum class ETexFormat : int32 { P8 = 0, RGBA7 = 1, RGB16 = 2, DXT1 = 3, RGB8 = 4, RGBA8 = 5, NODATA = 6, DXT3 = 7, DXT5 = 8, L8 = 9, G16 = 10, RRRGGGBBB = 11 };
	FString FormatName(int32 Format);

	/** Bytes the top mip needs for a format; -1 for unsupported formats. */
	int64 RequiredBytes(int32 Format, int32 W, int32 H);

	/**
	 * Decodes the top mip. Palette is 256 RGBA entries (P8 only, index 0 already cleared for masked textures).
	 * Returns false when the format is unsupported or the data too short (Error says which).
	 */
	bool Decode(int32 Format, const uint8* Data, int64 Len, int32 W, int32 H, const uint8* Palette, FConvImage& Out, FString& Error);

	/** PNG bytes (zlib level Level, filter none like the Python writer). */
	bool EncodePng(const FConvImage& Image, TArray64<uint8>& Out, int32 Level = 6);
	/** Reads a PNG (any format) into RGBA8 / 16-bit gray via the engine's image wrapper. */
	bool LoadPng(const FString& Path, FConvImage& Out, bool& bOutHasAlphaChannel);

	/**
	 * texture_stats.json entry of an image (ut2k4_materials.py _stats_of): sampled at <= 128 px with PIL's BOX
	 * filter on premultiplied RGBA; w/h are the full size. bHasAlphaChannel is the PNG's own alpha channel.
	 */
	FConvJson Stats(const FConvImage& Image, bool bHasAlphaChannel);
}
