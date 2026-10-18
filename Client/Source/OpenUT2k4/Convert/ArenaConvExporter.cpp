#include "ArenaConvExporter.h"
#include "ArenaConvConfig.h"
#if ARENA_CONV_BUILD
#include "Algo/Unique.h"
#include "ArenaConvGltf.h"
#include "ArenaConvImage.h"
#include "Algo/Unique.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

namespace ArenaConvExporterLocal
{
	constexpr int32 ToolVersion = 1;
	/** tool_version of static mesh json files (2: the UE2 collision model as a "collision_model" primitive). */
	constexpr int32 StaticMeshToolVersion = 2;

	/**
	 * Triangles of a static mesh's collision model (a UModel: a BSP of convex hulls whose nodes carry the boundary
	 * polygons), scaled to project units. Unreal winding is irrelevant: the runtime uses double-sided collision.
	 */
	bool ReadCollisionModel(FConvObject* Mo, float Scale, FConvPrim& Out, FString& OutError)
	{
		FConvReader R = Mo->NativeReader();
		R.Skip(25 + 16); // bounding box + sphere
		const int32 Nvec = R.Index();
		if (R.bError || Nvec < 0 || int64(Nvec) * 12 > R.Remaining()) { OutError = TEXT("bad vector count"); return false; }
		R.Skip(int64(Nvec) * 12);
		const int32 Npt = R.Index();
		if (R.bError || Npt < 0 || int64(Npt) * 12 > R.Remaining()) { OutError = TEXT("bad point count"); return false; }
		const float* Pts = Npt ? (const float*)R.Bytes(int64(Npt) * 12) : nullptr;
		const int32 Nn = R.Index();
		if (R.bError || Nn < 0 || Nn > 1000000) { OutError = TEXT("bad node count"); return false; }
		struct FNode { float Plane[4]; int32 VPool, NVerts; };
		TArray<FNode> Nodes;
		Nodes.SetNumUninitialized(Nn);
		for (int32 i = 0; i < Nn && !R.bError; ++i)
		{
			R.Raw(Nodes[i].Plane, 16);
			R.U64();
			R.U8();
			Nodes[i].VPool = R.Index();
			R.Index(); // iSurf
			R.Index(); R.Index(); R.Index(); // iBack, iFront, iPlane
			R.Index(); R.Index(); // iCollisionBound, iRenderBound
			R.Skip(16);
			R.U8(); R.U8();
			Nodes[i].NVerts = R.U8();
			R.Skip(20);
		}
		const int32 Ns = R.Index();
		if (R.bError || Ns < 0 || Ns > 1000000) { OutError = TEXT("bad surface count"); return false; }
		for (int32 i = 0; i < Ns && !R.bError; ++i)
		{
			R.Index(); R.U32(); R.Index(); R.Index(); R.Index(); R.Index(); R.Index(); R.Index();
			R.Skip(16);
			R.F32();
		}
		const int32 Nv = R.Index();
		if (R.bError || Nv < 0 || Nv > 4000000) { OutError = TEXT("bad vertex count"); return false; }
		TArray<int32> Verts;
		Verts.SetNumUninitialized(Nv);
		for (int32 i = 0; i < Nv && !R.bError; ++i)
		{
			Verts[i] = R.Index();
			R.Index();
		}
		if (R.bError) { OutError = R.Error; return false; }
		// some hull polygons (sloped faces of stair hulls...) are not on their node plane; their vertices are still
		// the hull's corners, so they are kept. Most polygons off their plane means the layout was not understood.
		int32 Total = 0, Bad = 0;
		for (const FNode& N : Nodes)
		{
			if (N.NVerts < 3)
			{
				continue;
			}
			if (N.VPool < 0 || N.VPool + N.NVerts > Nv) { OutError = TEXT("node vertex pool out of range"); return false; }
			for (int32 k = 0; k < N.NVerts; ++k)
			{
				const int32 Pi = Verts[N.VPool + k];
				if (Pi < 0 || Pi >= Npt) { OutError = TEXT("vertex index out of range"); return false; }
				const float* P = Pts + int64(Pi) * 3;
				++Total;
				Bad += FMath::Abs(P[0] * N.Plane[0] + P[1] * N.Plane[1] + P[2] * N.Plane[2] - N.Plane[3]) > 2.f ? 1 : 0;
			}
			const uint32 Base = uint32(Out.Positions.Num() / 3);
			for (int32 k = 0; k < N.NVerts; ++k)
			{
				const float* P = Pts + int64(Verts[N.VPool + k]) * 3;
				Out.Positions.Append({ P[0] * Scale, P[1] * Scale, P[2] * Scale });
				Out.Normals.Append({ N.Plane[0], N.Plane[1], N.Plane[2] });
			}
			for (int32 k = 1; k < N.NVerts - 1; ++k)
			{
				Out.Indices.Append({ Base, Base + k + 1, Base + k });
			}
		}
		if (Bad > Total / 4)
		{
			OutError = FString::Printf(TEXT("node vertices are not on their planes (%d/%d): unsupported layout"), Bad, Total);
			return false;
		}
		return Out.Indices.Num() > 0;
	}

	FConvJson Vec3(const float V[3], double Mul = 1.0)
	{
		FConvJson A = FConvJson::Array();
		A.Add(double(V[0]) * Mul);
		A.Add(double(V[1]) * Mul);
		A.Add(double(V[2]) * Mul);
		return A;
	}

	FConvJson PropOrNull(FConvExporter& Ex, const FConvProp* P)
	{
		return P ? Ex.Jsonify(*P) : FConvJson();
	}

	// ---- sound.py / music.py ----
	FString SoundKind(const uint8* D, int64 N)
	{
		if (N >= 12 && FMemory::Memcmp(D, "RIFF", 4) == 0 && FMemory::Memcmp(D + 8, "WAVE", 4) == 0)
		{
			return TEXT("wav");
		}
		if (N >= 4 && FMemory::Memcmp(D, "OggS", 4) == 0)
		{
			return TEXT("ogg");
		}
		if ((N >= 3 && FMemory::Memcmp(D, "ID3", 3) == 0) || (N >= 2 && D[0] == 0xFF && (D[1] == 0xFB || D[1] == 0xF3)))
		{
			return TEXT("mp3");
		}
		return FString();
	}

	uint32 RdU32(const uint8* P) { return P[0] | (P[1] << 8) | (P[2] << 16) | (uint32(P[3]) << 24); }
	uint16 RdU16(const uint8* P) { return P[0] | (P[1] << 8); }

	void WavInfo(const uint8* D, int64 N, FConvJson& Meta)
	{
		int64 Pos = 12;
		FString Codec;
		int32 Channels = 0, Rate = 0, Bits = 0;
		int64 DataBytes = -1;
		while (Pos + 8 <= N)
		{
			const uint8* Cid = D + Pos;
			const uint32 Size = RdU32(D + Pos + 4);
			const int64 Body = Pos + 8;
			if (FMemory::Memcmp(Cid, "fmt ", 4) == 0 && Size >= 16 && Body + 16 <= N)
			{
				const uint16 Tag = RdU16(D + Body);
				Channels = RdU16(D + Body + 2);
				Rate = RdU32(D + Body + 4);
				Bits = RdU16(D + Body + 14);
				static const TMap<int32, FString> Formats = { { 1, TEXT("PCM") }, { 2, TEXT("MS-ADPCM") }, { 3, TEXT("IEEE_FLOAT") }, { 0x11, TEXT("IMA-ADPCM") }, { 0x55, TEXT("MP3") } };
				const FString* F = Formats.Find(Tag);
				Codec = F ? *F : FString::Printf(TEXT("0x%04X"), Tag);
				Meta.Set(TEXT("codec"), Codec);
				Meta.Set(TEXT("format_tag"), int32(Tag));
				Meta.Set(TEXT("channels"), Channels);
				Meta.Set(TEXT("sample_rate"), Rate);
				Meta.Set(TEXT("bits_per_sample"), Bits);
			}
			else if (FMemory::Memcmp(Cid, "data", 4) == 0)
			{
				DataBytes = FMath::Min<int64>(Size, N - Body);
				Meta.Set(TEXT("data_bytes"), DataBytes);
			}
			else if (FMemory::Memcmp(Cid, "smpl", 4) == 0 && Size >= 36 && Body + 36 <= N)
			{
				const uint32 Loops = RdU32(D + Body + 28);
				if (Loops && Size >= 60 && Body + 36 + 16 <= N)
				{
					FConvJson L = FConvJson::Object();
					L.Set(TEXT("start_sample"), RdU32(D + Body + 44));
					L.Set(TEXT("end_sample"), RdU32(D + Body + 48));
					Meta.Set(TEXT("loop"), MoveTemp(L));
				}
			}
			Pos = Body + Size + (Size & 1);
		}
		if (Codec == TEXT("PCM") && DataBytes > 0 && Channels && Bits && Rate)
		{
			const int64 Div = int64(Channels) * Bits / 8;
			if (Div > 0)
			{
				Meta.Set(TEXT("duration"), FConvJson::RoundTo(double(DataBytes / Div) / double(Rate), 4));
			}
		}
	}

	void OggInfo(const uint8* D, int64 N, FConvJson& Meta)
	{
		Meta.Set(TEXT("codec"), FConvJson());
		TArray<TArray<uint8>> Packets;
		TArray<uint8> Cur;
		int64 LastGranule = 0;
		int64 Pos = 0;
		while (Pos + 27 <= N)
		{
			if (FMemory::Memcmp(D + Pos, "OggS", 4) != 0)
			{
				int64 Next = -1;
				for (int64 i = Pos + 1; i + 4 <= N; ++i)
				{
					if (FMemory::Memcmp(D + i, "OggS", 4) == 0)
					{
						Next = i;
						break;
					}
				}
				if (Next < 0)
				{
					break;
				}
				Pos = Next;
				continue;
			}
			int64 Granule;
			FMemory::Memcpy(&Granule, D + Pos + 6, 8);
			const int32 NSeg = D[Pos + 26];
			if (Pos + 27 + NSeg > N)
			{
				break;
			}
			const uint8* Segs = D + Pos + 27;
			const int64 Body = Pos + 27 + NSeg;
			int64 Size = 0;
			for (int32 i = 0; i < NSeg; ++i)
			{
				Size += Segs[i];
			}
			if (Granule >= 0)
			{
				LastGranule = FMath::Max(LastGranule, Granule);
			}
			if (Packets.Num() < 2)
			{
				int64 Off = 0;
				for (int32 i = 0; i < NSeg; ++i)
				{
					const int32 S = Segs[i];
					const int64 Avail = FMath::Clamp<int64>(N - (Body + Off), 0, S);
					Cur.Append(D + Body + Off, Avail);
					Off += S;
					if (S < 255)
					{
						Packets.Add(MoveTemp(Cur));
						Cur.Reset();
						if (Packets.Num() >= 2)
						{
							break;
						}
					}
				}
			}
			Pos = Body + Size;
		}
		if (Packets.Num() && Packets[0].Num() >= 7 + 21 && FMemory::Memcmp(Packets[0].GetData(), "\x01vorbis", 7) == 0)
		{
			const uint8* P = Packets[0].GetData();
			const int32 Ch = P[11];
			const uint32 Rate = RdU32(P + 12);
			const int32 Nom = (int32)RdU32(P + 20);
			Meta.Set(TEXT("codec"), TEXT("vorbis"));
			Meta.Set(TEXT("channels"), Ch);
			Meta.Set(TEXT("sample_rate"), Rate);
			Meta.Set(TEXT("bitrate_nominal"), Nom > 0 ? FConvJson(Nom) : FConvJson());
			if (Rate)
			{
				Meta.Set(TEXT("duration"), FConvJson::RoundTo(double(LastGranule) / double(Rate), 3));
			}
		}
		else if (Packets.Num() && Packets[0].Num() >= 10 && FMemory::Memcmp(Packets[0].GetData(), "OpusHead", 8) == 0)
		{
			Meta.Set(TEXT("codec"), TEXT("opus"));
			Meta.Set(TEXT("channels"), int32(Packets[0][9]));
			Meta.Set(TEXT("sample_rate"), 48000);
			Meta.Set(TEXT("duration"), FConvJson::RoundTo(double(LastGranule) / 48000.0, 3));
		}
		if (Packets.Num() > 1 && Packets[1].Num() >= 11 && FMemory::Memcmp(Packets[1].GetData(), "\x03vorbis", 7) == 0)
		{
			const TArray<uint8>& P = Packets[1];
			auto Utf8 = [](const uint8* S, int64 L) { return FString(FUTF8ToTCHAR((const ANSICHAR*)S, (int32)L)); };
			const uint32 Vl = RdU32(P.GetData() + 7);
			if (11 + int64(Vl) <= P.Num())
			{
				Meta.Set(TEXT("vendor"), Utf8(P.GetData() + 11, Vl));
				int64 Q = 11 + Vl;
				if (Q + 4 <= P.Num())
				{
					const uint32 Count = RdU32(P.GetData() + Q);
					Q += 4;
					FConvJson Comments = FConvJson::Object();
					bool bOk = true;
					for (uint32 i = 0; i < FMath::Min<uint32>(Count, 256); ++i)
					{
						if (Q + 4 > P.Num())
						{
							bOk = false;
							break;
						}
						const uint32 Ln = RdU32(P.GetData() + Q);
						Q += 4;
						const FString Kv = Utf8(P.GetData() + Q, FMath::Min<int64>(Ln, P.Num() - Q));
						Q += Ln;
						int32 Eq;
						if (Kv.FindChar(TEXT('='), Eq))
						{
							const FString K = Kv.Left(Eq).ToUpper();
							if (!Comments.Has(K))
							{
								Comments.Set(K, Kv.Mid(Eq + 1));
							}
						}
					}
					if (bOk)
					{
						Meta.Set(TEXT("comments"), MoveTemp(Comments));
					}
				}
			}
		}
	}

	struct FTexSpec
	{
		TSharedPtr<FConvPackageData, ESPMode::ThreadSafe> Data;
		int64 Off = 0;
		int64 Len = 0;
		int32 W = 0;
		int32 H = 0;
		int32 Format = 0;
		int32 MipCount = 0;
		TArray<uint8> Palette;
		bool bFlat = false;
		uint8 Flat[3] = { 128, 128, 128 };
		bool bMasked = false, bAlphaTexture = false, bTwoSided = false;
		/** Mip that is decoded (> 0 when the top mips were stripped), -1 before PrepareTexture found one. */
		int32 MipUsed = 0;
		/** Set when no mip has pixel data at all. */
		bool bNoPixels = false;
	};

	enum class ETexErr : uint8 { None, Texture, Other };

	bool ReadPalette(FConvObject* PalObj, TArray<uint8>& Out, ETexErr& Err, FString& Msg)
	{
		FConvReader R = PalObj->NativeReader();
		const int32 N = R.Index();
		if (R.bError)
		{
			Err = ETexErr::Other;
			Msg = R.Error;
			return false;
		}
		if (!(N > 0 && N <= 256))
		{
			Err = ETexErr::Texture;
			Msg = FString::Printf(TEXT("bad palette size %d"), N);
			return false;
		}
		const uint8* P = R.Bytes(int64(N) * 4);
		if (!P)
		{
			Err = ETexErr::Other;
			Msg = R.Error;
			return false;
		}
		Out.SetNumZeroed(1024);
		FMemory::Memcpy(Out.GetData(), P, N * 4);
		return true;
	}

	/** texture.py read_mips + decode_texture validation (the decode itself runs as a task). */
	bool PrepareTexture(FConvObject* Obj, FTexSpec& Spec, ETexErr& Err, FString& Msg)
	{
		const FConvProps& P = Obj->Props();
		const FConvProp* Fmt = ConvFindProp(P, TEXT("Format"));
		Spec.Format = Fmt ? Fmt->Int() : 0;
		const FConvProp* M = ConvFindProp(P, TEXT("bMasked"));
		const FConvProp* A = ConvFindProp(P, TEXT("bAlphaTexture"));
		const FConvProp* T = ConvFindProp(P, TEXT("bTwoSided"));
		Spec.bMasked = M && M->Truthy();
		Spec.bAlphaTexture = A && A->Truthy();
		Spec.bTwoSided = T && T->Truthy();
		FConvReader R = Obj->NativeReader();
		const int32 N = R.Index();
		if (R.bError)
		{
			Err = ETexErr::Other;
			Msg = R.Error;
			return false;
		}
		if (N < 0 || N > 32)
		{
			Err = ETexErr::Texture;
			Msg = FString::Printf(TEXT("bad mip count %d"), N);
			return false;
		}
		const bool bSkip = Obj->Pkg->Version > 61;
		int64 MipOff = 0, MipLen = 0;
		int32 MipW = 0, MipH = 0;
		Spec.MipUsed = -1;
		for (int32 i = 0; i < N; ++i)
		{
			int64 Off = 0, Len = 0;
			R.LazyBytesSpan(bSkip, Off, Len);
			const int32 Us = R.I32(), Vs = R.I32();
			R.U8();
			R.U8();
			if (R.bError)
			{
				Err = ETexErr::Other;
				Msg = R.Error;
				return false;
			}
			if (i == 0)
			{
				Spec.Off = Off;
				Spec.Len = Len;
				Spec.W = Us;
				Spec.H = Vs;
			}
			else if (Spec.MipUsed < 0 && Us > 0 && Vs > 0)
			{
				// UT2004 ships many textures whose top mip(s) were stripped (length 0) to save space: the game shows
				// the first mip that has data, and so do we
				const int64 NeedI = ArenaConvImage::RequiredBytes(Spec.Format, Us, Vs);
				if (NeedI > 0 && Len >= NeedI)
				{
					Spec.MipUsed = i;
					MipOff = Off; MipLen = Len; MipW = Us; MipH = Vs;
				}
			}
			if (i == 0 && Us > 0 && Vs > 0)
			{
				const int64 Need0 = ArenaConvImage::RequiredBytes(Spec.Format, Us, Vs);
				if (Need0 > 0 && Len >= Need0)
				{
					Spec.MipUsed = 0;
				}
			}
		}
		if (Spec.MipUsed > 0)
		{
			Spec.Off = MipOff;
			Spec.Len = MipLen;
			Spec.W = MipW;
			Spec.H = MipH;
		}
		Spec.MipCount = N;
		if (N == 0)
		{
			Err = ETexErr::Texture;
			Msg = TEXT("texture has no mipmaps");
			Spec.bNoPixels = true;
			return false;
		}
		if (Spec.W <= 0 || Spec.H <= 0)
		{
			Err = ETexErr::Texture;
			Msg = FString::Printf(TEXT("bad size %dx%d"), Spec.W, Spec.H);
			return false;
		}
		const int64 Npx = int64(Spec.W) * Spec.H;
		const ArenaConvImage::ETexFormat F = (ArenaConvImage::ETexFormat)Spec.Format;
		const int64 Need = ArenaConvImage::RequiredBytes(Spec.Format, Spec.W, Spec.H);
		if (Need < 0)
		{
			Err = ETexErr::Texture;
			Msg = FString::Printf(TEXT("unsupported texture format %s"), *ArenaConvImage::FormatName(Spec.Format));
			return false;
		}
		if (Spec.Len < Need)
		{
			const bool bValueError = F == ArenaConvImage::ETexFormat::L8 || F == ArenaConvImage::ETexFormat::RGB16;
			Err = bValueError ? ETexErr::Other : ETexErr::Texture;
			switch (F)
			{
			case ArenaConvImage::ETexFormat::DXT1:
			case ArenaConvImage::ETexFormat::DXT3:
			case ArenaConvImage::ETexFormat::DXT5: Msg = FString::Printf(TEXT("DXT data too short (%lld < %lld)"), Spec.Len, Need); break;
			case ArenaConvImage::ETexFormat::RGBA8: Msg = TEXT("RGBA8 data too short"); break;
			case ArenaConvImage::ETexFormat::RGB8: Msg = TEXT("RGB8 data too short"); break;
			case ArenaConvImage::ETexFormat::P8: Msg = TEXT("P8 data too short"); break;
			case ArenaConvImage::ETexFormat::G16: Msg = TEXT("G16 data too short"); break;
			default: Msg = FString::Printf(TEXT("buffer is smaller than requested size (%lld < %lld)"), Spec.Len, Need); break;
			}
			Spec.bNoPixels = true; // no mip carries pixels: the package only kept the texture's settings
			return false;
		}
		if (F == ArenaConvImage::ETexFormat::P8)
		{
			const FConvProp* PalRef = ConvFindProp(P, TEXT("Palette"));
			FConvObject* PalObj = PalRef && PalRef->IsRef() && PalRef->Truthy() ? PalRef->Ref.Load() : nullptr;
			if (!PalObj)
			{
				Err = ETexErr::Texture;
				Msg = TEXT("P8 texture without palette");
				return false;
			}
			if (!ReadPalette(PalObj, Spec.Palette, Err, Msg))
			{
				return false;
			}
			if (Spec.bMasked)
			{
				FMemory::Memzero(Spec.Palette.GetData(), 4);
			}
		}
		(void)Npx;
		Spec.Data = Obj->Pkg->Data;
		return true;
	}

	bool IsProcedural(FConvObject* Obj)
	{
		if (Obj->IsA(TEXT("FractalTexture"), TEXT("ScriptedTexture")))
		{
			return true;
		}
		for (const FString& C : Obj->ClassChain())
		{
			for (const TCHAR* K : { TEXT("FireTexture"), TEXT("WaterTexture"), TEXT("WetTexture"), TEXT("WaveTexture"), TEXT("IceTexture"), TEXT("FluidTexture") })
			{
				if (C.Equals(K, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
		}
		return false;
	}

	int64 FileMTime(const FString& Path)
	{
		const FDateTime T = IFileManager::Get().GetTimeStamp(*Path);
		return T == FDateTime::MinValue() ? -1 : (T - FDateTime(1970, 1, 1)).GetTicks() / ETimespan::TicksPerSecond;
	}

	// material graph helpers (texture.py)
	const TCHAR* ShaderBlend(int32 Ob)
	{
		switch (Ob) { case 0: return TEXT("opaque"); case 1: return TEXT("masked"); case 2: return TEXT("modulate"); case 3: return TEXT("additive");
		case 4: return TEXT("invisible"); case 5: return TEXT("additive"); case 6: return TEXT("modulate"); default: return TEXT("opaque"); }
	}
	FConvJson ShaderOutputName(int32 Ob)
	{
		static const TCHAR* N[] = { TEXT("Normal"), TEXT("Masked"), TEXT("Modulate"), TEXT("Translucent"), TEXT("Invisible"), TEXT("Brighten"), TEXT("Darken") };
		return Ob >= 0 && Ob <= 6 ? FConvJson(N[Ob]) : FConvJson(Ob);
	}
	const TCHAR* FbToBlend(int32 Fb)
	{
		switch (Fb) { case 0: return TEXT("opaque"); case 1: return TEXT("modulate"); case 2: return TEXT("translucent"); case 3: return TEXT("translucent");
		case 4: return TEXT("additive"); case 5: return TEXT("modulate"); case 6: return TEXT("additive"); case 7: return TEXT("invisible"); default: return TEXT("opaque"); }
	}
	FConvJson FbName(int32 Fb)
	{
		static const TCHAR* N[] = { TEXT("Overwrite"), TEXT("Modulate"), TEXT("AlphaBlend"), TEXT("AlphaModulate"), TEXT("Translucent"), TEXT("Darken"), TEXT("Brighten"), TEXT("Invisible") };
		return Fb >= 0 && Fb <= 7 ? FConvJson(N[Fb]) : FConvJson(Fb);
	}
}

using namespace ArenaConvExporterLocal;

FConvExporter::FConvExporter(FConvPackageManager& InManager, const FOptions& InOptions)
	: Mgr(InManager), Opt(InOptions)
{
	for (const TCHAR* K : { TEXT("textures"), TEXT("materials"), TEXT("staticmeshes"), TEXT("sounds"), TEXT("maps"), TEXT("skipped"),
		TEXT("skeletalmeshes"), TEXT("animations"), TEXT("vertmeshes"), TEXT("music") })
	{
		Counts.Add(K, 0);
	}
}

FConvExporter::~FConvExporter()
{
	Flush();
	delete AnimMeshes;
}

// ---------------------------------------------------------------------------------------------
// tasks

void FConvExporter::Launch(int64 Bytes, TUniqueFunction<void()> Work)
{
	while (PendingBytes.Load() > Opt.MaxPendingBytes && Tasks.Num())
	{
		// wait for the oldest task, drop finished ones
		Tasks[0].Wait();
		Tasks.RemoveAll([](const UE::Tasks::FTask& T) { return T.IsCompleted(); });
	}
	PendingBytes += Bytes;
	Tasks.Add(UE::Tasks::Launch(TEXT("ArenaConvert"), [this, Bytes, Work = MoveTemp(Work)]() mutable
	{
		Work();
		PendingBytes -= Bytes;
	}, Opt.bBackgroundTasks ? UE::Tasks::ETaskPriority::BackgroundNormal : UE::Tasks::ETaskPriority::Normal));
	if (Tasks.Num() > 256)
	{
		Tasks.RemoveAll([](const UE::Tasks::FTask& T) { return T.IsCompleted(); });
	}
}

void FConvExporter::Flush()
{
	for (UE::Tasks::FTask& T : Tasks)
	{
		T.Wait();
	}
	Tasks.Reset();
	FScopeLock Lock(&TaskLock);
	for (FString& E : TaskErrors)
	{
		Errors.Add(MoveTemp(E));
	}
	TaskErrors.Reset();
}

TMap<FString, FConvJson> FConvExporter::TakeTextureStats()
{
	FScopeLock Lock(&TaskLock);
	TMap<FString, FConvJson> Out = MoveTemp(TextureStats);
	TextureStats.Reset();
	return Out;
}

// ---------------------------------------------------------------------------------------------
// logging, paths, json

void FConvExporter::Error(const FString& What, const FString& Message)
{
	Errors.Add(FString::Printf(TEXT("%s: %s"), *What, *Message));
}

void FConvExporter::Note_(const FString& Message)
{
	Notes.Add(Message);
}

TArray<FString> FConvExporter::TakeNewNotes()
{
	TArray<FString> Out;
	{
		FScopeLock Lock(&TaskLock);
		Notes.Append(MoveTemp(TaskNotes));
		TaskNotes.Reset();
	}
	for (int32 i = NotesTaken; i < Notes.Num(); ++i)
	{
		Out.Add(Notes[i]);
	}
	NotesTaken = Notes.Num();
	return Out;
}

TArray<FString> FConvExporter::TakeNewErrors()
{
	{
		FScopeLock Lock(&TaskLock);
		for (FString& E : TaskErrors)
		{
			Errors.Add(MoveTemp(E));
		}
		TaskErrors.Reset();
	}
	TArray<FString> Out;
	for (int32 i = ErrorsTaken; i < Errors.Num(); ++i)
	{
		Out.Add(Errors[i]);
	}
	ErrorsTaken = Errors.Num();
	return Out;
}

FString FConvExporter::Safe(const FString& Name)
{
	FString Out = Name;
	for (TCHAR& C : Out.GetCharArray())
	{
		if (C == 0)
		{
			break;
		}
		const bool bOk = (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '.' || C == '_' || C == '-';
		if (!bOk)
		{
			C = '_';
		}
	}
	return Out;
}

FString FConvExporter::OutPath(const TCHAR* Kind, FConvObject* Obj, const TCHAR* Ext)
{
	const FString Full = Obj->Path();
	int32 Dot;
	const FString RelName = Full.FindChar(TEXT('.'), Dot) ? Full.Mid(Dot + 1) : Full;
	return Opt.OutDir / Kind / Safe(Obj->Pkg->Name) / (Safe(RelName) + Ext);
}

FString FConvExporter::Rel(const FString& Path) const
{
	FString R = Path;
	if (R.StartsWith(Opt.OutDir))
	{
		R.RightChopInline(Opt.OutDir.Len());
		R.RemoveFromStart(TEXT("/"));
	}
	return R;
}

bool FConvExporter::WriteJson(const FString& Path, const FConvJson& Data)
{
	if (!Data.SaveToFile(Path, 1))
	{
		Error(Path, TEXT("cannot write"));
		return false;
	}
	return true;
}

bool FConvExporter::Fresh(FConvObject* Obj, const FString& MainPath, const FString& InMetaPath, int32 Version)
{
	if (!Opt.bSkipExisting)
	{
		return false;
	}
	const FString MetaPath = InMetaPath.IsEmpty() ? FPaths::ChangeExtension(MainPath, TEXT(".json")) : InMetaPath;
	IFileManager& FM = IFileManager::Get();
	const FDateTime Mt = FM.GetTimeStamp(*MetaPath);
	if (Mt == FDateTime::MinValue())
	{
		return false;
	}
	if (MainPath != MetaPath && !FM.FileExists(*MainPath))
	{
		return false;
	}
	if (Mt < FM.GetTimeStamp(*Obj->Pkg->PathOnDisk))
	{
		return false;
	}
	FConvJson Meta;
	if (!FConvJson::LoadFile(MetaPath, Meta))
	{
		return false;
	}
	return Meta.GetNumber(TEXT("tool_version"), 1) >= Version;
}

FString FConvExporter::Canon(const FConvRef& Ref)
{
	if (!Ref.IsSet())
	{
		return FString();
	}
	const FString Key = FString::Printf(TEXT("%s|%d"), *Ref.Pkg->PathOnDisk, Ref.Index);
	if (const FString* Hit = CanonCache.Find(Key))
	{
		return *Hit;
	}
	FConvPackage* P = nullptr;
	int32 Ei = 0;
	FString Out = Ref.Pkg->ResolveRef(Ref.Index, P, Ei) ? P->ExportPath(Ei) : Ref.Path();
	CanonCache.Add(Key, Out);
	return Out;
}

FConvJson FConvExporter::Jsonify(const FConvProp& V)
{
	switch (V.Kind)
	{
	case FConvProp::EKind::None: return FConvJson();
	case FConvProp::EKind::Bool: return FConvJson(V.B);
	case FConvProp::EKind::Int: return FConvJson(V.I);
	case FConvProp::EKind::Float:
		if (!FMath::IsFinite(V.F))
		{
			return FConvJson();
		}
		return FConvJson(FConvJson::RoundTo(V.F, 6));
	case FConvProp::EKind::Str: return FConvJson(V.S);
	case FConvProp::EKind::Ref: return V.Ref.IsSet() ? FConvJson(Canon(V.Ref)) : FConvJson();
	case FConvProp::EKind::Vector:
	{
		FConvJson A = FConvJson::Array();
		for (int32 i = 0; i < 3; ++i)
		{
			A.Add(FConvJson::RoundTo(V.V[i], 5));
		}
		return A;
	}
	case FConvProp::EKind::Rotator:
	{
		FConvJson O = FConvJson::Object();
		O.Set(TEXT("Pitch"), V.R[0]);
		O.Set(TEXT("Yaw"), V.R[1]);
		O.Set(TEXT("Roll"), V.R[2]);
		return O;
	}
	case FConvProp::EKind::Struct: return JsonifyProps(V.M);
	case FConvProp::EKind::Array:
	{
		FConvJson A = FConvJson::Array();
		A.A.Reserve(V.A.Num());
		for (const FConvProp& E : V.A)
		{
			A.Add(Jsonify(E));
		}
		return A;
	}
	}
	return FConvJson();
}

FConvJson FConvExporter::JsonifyProps(const FConvProps& Props)
{
	FConvJson O = FConvJson::Object();
	O.O.Reserve(Props.Num());
	for (const TPair<FString, FConvProp>& KV : Props)
	{
		O.O.Emplace(KV.Key, Jsonify(KV.Value));
	}
	return O;
}

void FConvExporter::CollectRefs(const FConvProp& V, TArray<FConvRef>& Out) const
{
	if (V.Kind == FConvProp::EKind::Ref)
	{
		if (V.Ref.IsSet())
		{
			Out.Add(V.Ref);
		}
	}
	else if (V.Kind == FConvProp::EKind::Struct)
	{
		CollectRefs(V.M, Out);
	}
	else if (V.Kind == FConvProp::EKind::Array)
	{
		for (const FConvProp& E : V.A)
		{
			CollectRefs(E, Out);
		}
	}
}

void FConvExporter::CollectRefs(const FConvProps& Props, TArray<FConvRef>& Out) const
{
	for (const TPair<FString, FConvProp>& KV : Props)
	{
		CollectRefs(KV.Value, Out);
	}
}

// ---------------------------------------------------------------------------------------------
// dependency tracking

void FConvExporter::Note(const FString& Key)
{
	const TSet<FString>* Deps = ObjDeps.Find(Key);
	for (TSet<FString>& C : Collect)
	{
		C.Add(Key);
		if (Deps)
		{
			C.Append(*Deps);
		}
	}
}

void FConvExporter::BeginCollect()
{
	Collect.AddDefaulted();
}

FConvJson FConvExporter::EndCollect()
{
	TSet<FString> Keys = Collect.Pop();
	// transitive closure: objects in a reference cycle (AnimNext chains) recorded their dependencies only
	// partly while the cycle was being exported; the map needs all of them
	TArray<FString> Work = Keys.Array();
	while (Work.Num())
	{
		const FString K = Work.Pop(EAllowShrinking::No);
		if (const TSet<FString>* Deps = ObjDeps.Find(K))
		{
			for (const FString& Dk : *Deps)
			{
				bool bAlready = false;
				Keys.Add(Dk, &bAlready);
				if (!bAlready)
				{
					Work.Add(Dk);
				}
			}
		}
	}
	TArray<FString> Sorted = Keys.Array();
	Sorted.Sort([](const FString& A, const FString& B) { return FCString::Strcmp(*A, *B) < 0; });
	FConvJson Out = FConvJson::Object();
	for (const FString& K : Sorted)
	{
		const FString* F = Done.Find(K);
		if (!F || F->IsEmpty())
		{
			continue;
		}
		int32 Slash;
		const FString Kind = F->FindChar(TEXT('/'), Slash) ? F->Left(Slash) : *F;
		FConvJson* List = Out.Find(Kind);
		if (!List)
		{
			List = &Out.Set(Kind, FConvJson::Array());
		}
		FConvJson E = FConvJson::Object();
		const FString* P = Paths.Find(K);
		E.Set(TEXT("path"), P ? *P : K);
		E.Set(TEXT("file"), *F);
		List->Add(MoveTemp(E));
	}
	return Out;
}

FString FConvExporter::FileFor(const FString& Path, const FString& Kind) const
{
	if (const TArray<FString>* Keys = ByPath.Find(Path.ToLower()))
	{
		for (const FString& K : *Keys)
		{
			const FString* F = Done.Find(K);
			if (F && !F->IsEmpty() && (Kind.IsEmpty() || F->StartsWith(Kind + TEXT("/"))))
			{
				return *F;
			}
		}
	}
	return FString();
}

// ---------------------------------------------------------------------------------------------
// dispatch

FString FConvExporter::KindOf(FConvObject* Obj)
{
	if (Obj->IsA(TEXT("Texture")) && !Obj->IsA(TEXT("Cubemap"))) return TEXT("textures");
	if (Obj->IsA(TEXT("Material"))) return TEXT("materials");
	if (Obj->IsA(TEXT("StaticMesh"))) return TEXT("staticmeshes");
	if (Obj->IsA(TEXT("Sound")) && !Obj->IsA(TEXT("SoundGroup"))) return TEXT("sounds");
	if (Obj->IsA(TEXT("SoundGroup"))) return TEXT("soundgroup");
	if (Obj->IsA(TEXT("SkeletalMesh"))) return TEXT("skeletalmeshes");
	if (Obj->IsA(TEXT("MeshAnimation"))) return TEXT("animations");
	if (Obj->IsA(TEXT("VertMesh"))) return TEXT("vertmeshes");
	if (Obj->IsA(TEXT("Font"))) return TEXT("fonts");
	return FString();
}

FString FConvExporter::ExportRef(const FConvRef& Ref)
{
	if (!Ref.IsSet())
	{
		return FString();
	}
	if (Ref.Index < 0)
	{
		const FString Cn = Ref.ClassName();
		if (Cn == TEXT("Class") || Cn == TEXT("Package") || Cn.EndsWith(TEXT("Property")) || Cn == TEXT("Function"))
		{
			return FString();
		}
	}
	FConvObject* Obj = Ref.Load();
	if (!Obj)
	{
		const FString Key = Ref.Path().ToLower();
		if (!Done.Contains(Key))
		{
			Done.Add(Key, FString());
			// a reference the original data cannot satisfy either (the retail game leaves it empty too): a note, not an error
			const FString Path = Ref.Path();
			int32 Dot;
			const FString PkgName = Path.FindChar(TEXT('.'), Dot) ? Path.Left(Dot) : Path;
			Note_(FString::Printf(TEXT("dangling reference %s (%s) from %s: %s"), *Path, *Ref.ClassName(), *Ref.Pkg->Name,
				Mgr.FindFile(PkgName).IsEmpty() ? *FString::Printf(TEXT("package %s is not part of this install"), *PkgName)
					: *FString::Printf(TEXT("package %s has no such object"), *PkgName)));
		}
		return FString();
	}
	return ExportObject(Obj);
}

FString FConvExporter::ExportObject(FConvObject* Obj)
{
	const FString ObjPath = Obj->Path();
	const FString Key = ObjPath.ToLower() + TEXT("|") + Obj->ClassName().ToLower();
	if (const FString* Hit = Done.Find(Key))
	{
		const FString Result = *Hit;
		Note(Key);
		return Result;
	}
	ByPath.FindOrAdd(ObjPath.ToLower()).Add(Key);
	Done.Add(Key, FString()); // recursion guard
	Paths.Add(Key, ObjPath);
	const FString Kind = KindOf(Obj);
	if (Kind.IsEmpty())
	{
		return FString();
	}
	Collect.AddDefaulted();
	const FString Result = ExportKind(Obj, Kind);
	ObjDeps.Add(Key, Collect.Pop());
	Done.Add(Key, Result);
	Note(Key);
	return Result;
}

FString FConvExporter::ExportKind(FConvObject* Obj, const FString& Kind)
{
	CurrentItem = Obj->Path();
	const int32 NErr = Errors.Num();
	FString Result;
	if (Kind == TEXT("textures")) Result = ExportTexture(Obj);
	else if (Kind == TEXT("materials")) Result = ExportMaterial(Obj);
	else if (Kind == TEXT("staticmeshes")) Result = ExportStaticMesh(Obj);
	else if (Kind == TEXT("sounds")) Result = ExportSound(Obj);
	else if (Kind == TEXT("skeletalmeshes")) Result = ExportSkeletalMesh(Obj);
	else if (Kind == TEXT("animations")) Result = ExportAnimation(Obj);
	else if (Kind == TEXT("vertmeshes")) Result = ExportVertMesh(Obj);
	else if (Kind == TEXT("fonts")) Result = ExportFont(Obj);
	else if (Kind == TEXT("soundgroup"))
	{
		if (const FConvProp* S = ConvFindProp(Obj->Props(), TEXT("Sounds")))
		{
			if (S->IsArray())
			{
				for (const FConvProp& E : S->A)
				{
					if (E.IsRef())
					{
						ExportRef(E.Ref);
					}
				}
			}
		}
	}
	(void)NErr;
	return Result;
}

// ---------------------------------------------------------------------------------------------
// material graph (texture.py resolve_material)

FIntPoint FConvExporter::TextureSize(FConvObject* Tex)
{
	const FConvProps& P = Tex->Props();
	const FConvProp* U = ConvFindProp(P, TEXT("USize"));
	const FConvProp* V = ConvFindProp(P, TEXT("VSize"));
	int32 Us = U ? U->Int() : 0, Vs = V ? V->Int() : 0;
	if (!(U && U->Truthy()) || !(V && V->Truthy()))
	{
		FConvReader R = Tex->NativeReader();
		const int32 N = R.Index();
		if (!R.bError && N > 0 && N <= 32)
		{
			int64 Off, Len;
			R.LazyBytesSpan(Tex->Pkg->Version > 61, Off, Len);
			const int32 W = R.I32(), H = R.I32();
			if (!R.bError)
			{
				Us = W;
				Vs = H;
			}
		}
	}
	return FIntPoint(Us ? Us : 256, Vs ? Vs : 256);
}

FConvExporter::FMatResolve FConvExporter::ResolveMaterial(FConvObject* Root)
{
	FMatResolve Out;
	TSet<FString> Seen;
	auto RefLoad = [](const FConvProp* V) -> FConvObject*
	{
		return V && V->IsRef() && V->Ref.IsSet() ? V->Ref.Load() : nullptr;
	};
	auto Child = [&](FConvObject* Cur, const FString& Key) -> FConvObject*
	{
		int32 Hash;
		if (Key.FindChar(TEXT('#'), Hash))
		{
			const FConvProp* Arr = ConvFindProp(Cur->Props(), Key.Left(Hash));
			const int32 I = FCString::Atoi(*Key.Mid(Hash + 1));
			if (Arr && Arr->IsArray() && Arr->A.Num() > I && I >= 0)
			{
				const FConvProp* V = &Arr->A[I];
				if (V->IsStruct())
				{
					V = V->Find(TEXT("Material"));
				}
				return RefLoad(V);
			}
			return nullptr;
		}
		return RefLoad(ConvFindProp(Cur->Props(), Key));
	};
	auto PathOf = [](const FConvProp* V) -> FConvJson { return V && V->IsRef() ? FConvJson(V->Ref.Path()) : FConvJson(); };

	TFunction<FConvObject*(FConvObject*, int32)> Visit = [&](FConvObject* Cur, int32 D) -> FConvObject*
	{
		if (!Cur || D > 24)
		{
			return nullptr;
		}
		const FString CurPath = Cur->Path();
		if (Seen.Contains(CurPath))
		{
			return nullptr;
		}
		Seen.Add(CurPath);
		FConvJson C = FConvJson::Object();
		C.Set(TEXT("class"), Cur->ClassName());
		C.Set(TEXT("path"), CurPath);
		Out.Chain.Add(MoveTemp(C));
		const FConvProps& P = Cur->Props();
		auto Get = [&P](const TCHAR* K) { return ConvFindProp(P, K); };
		auto Truthy = [&P](const TCHAR* K) { const FConvProp* V = ConvFindProp(P, K); return V && V->Truthy(); };
		if (Cur->IsA(TEXT("Texture")) && !Cur->IsA(TEXT("Cubemap")))
		{
			if (Out.BlendMode.IsEmpty())
			{
				if (Truthy(TEXT("bMasked")))
				{
					Out.BlendMode = TEXT("masked");
				}
				else if (Truthy(TEXT("bAlphaTexture")))
				{
					Out.BlendMode = TEXT("translucent");
				}
			}
			if (Truthy(TEXT("bTwoSided")))
			{
				Out.bTwoSided = true;
			}
			if (Out.Detail.IsNull() && Truthy(TEXT("Detail")))
			{
				Out.Detail = PathOf(Get(TEXT("Detail")));
			}
			return Cur;
		}
		TArray<FString> Children;
		if (Cur->IsA(TEXT("Shader")))
		{
			const FConvProp* ObP = Get(TEXT("OutputBlending"));
			const int32 Ob = ObP ? ObP->Int() : 0;
			if (Out.BlendMode.IsEmpty())
			{
				FString Bm = ShaderBlend(Ob);
				if (Ob == 0 && Truthy(TEXT("Opacity")))
				{
					Bm = TEXT("translucent");
				}
				Out.BlendMode = Bm;
				Out.ShaderOutputBlending = ShaderOutputName(Ob);
				Out.bHasShaderOutput = true;
			}
			if (Truthy(TEXT("TwoSided")))
			{
				Out.bTwoSided = true;
			}
			const TPair<const TCHAR*, FConvJson*> Pairs[] = { { TEXT("SelfIllumination"), &Out.Emissive }, { TEXT("Opacity"), &Out.Opacity },
				{ TEXT("Specular"), &Out.Specular }, { TEXT("Detail"), &Out.Detail } };
			for (const auto& Pr : Pairs)
			{
				const FConvProp* V = Get(Pr.Key);
				if (V && V->Truthy() && Pr.Value->IsNull())
				{
					*Pr.Value = PathOf(V);
				}
			}
			if (Truthy(TEXT("SelfIllumination")) && !Truthy(TEXT("Diffuse")))
			{
				Out.bUnlit = true;
			}
		}
		else if (Cur->IsA(TEXT("FinalBlend")))
		{
			const FConvProp* FbP = Get(TEXT("FrameBufferBlending"));
			const int32 Fb = FbP ? FbP->Int() : 0;
			if (Out.BlendMode.IsEmpty())
			{
				Out.FrameBufferBlending = FbName(Fb);
				Out.bHasFrameBuffer = true;
				FString Bm = FbToBlend(Fb);
				if (Bm == TEXT("opaque") && Truthy(TEXT("AlphaTest")))
				{
					Bm = TEXT("masked");
				}
				Out.BlendMode = Bm;
				if (Truthy(TEXT("AlphaTest")))
				{
					const FConvProp* Ar = Get(TEXT("AlphaRef"));
					Out.AlphaRef = Ar ? Jsonify(*Ar) : FConvJson(0);
					Out.bHasAlphaRef = true;
				}
			}
			if (Truthy(TEXT("TwoSided")))
			{
				Out.bTwoSided = true;
			}
		}
		else if (Cur->IsA(TEXT("ConstantColor"), TEXT("ConstantMaterial")))
		{
			const FConvProp* Col = Get(TEXT("Color"));
			if (Col && Col->IsStruct() && !Out.bHasConstantColor)
			{
				FConvJson CC = FConvJson::Array();
				const FConvProp* R = Col->Find(TEXT("R")); const FConvProp* G = Col->Find(TEXT("G"));
				const FConvProp* B = Col->Find(TEXT("B")); const FConvProp* A = Col->Find(TEXT("A"));
				CC.Add(R ? Jsonify(*R) : FConvJson(0));
				CC.Add(G ? Jsonify(*G) : FConvJson(0));
				CC.Add(B ? Jsonify(*B) : FConvJson(0));
				CC.Add(A ? Jsonify(*A) : FConvJson(255));
				Out.ConstantColor = MoveTemp(CC);
				Out.bHasConstantColor = true;
			}
		}
		// material_children
		if (Cur->IsA(TEXT("Shader")))
		{
			Children = { TEXT("Diffuse"), TEXT("SelfIllumination"), TEXT("Specular"), TEXT("Opacity"), TEXT("FallbackMaterial") };
		}
		else if (Cur->IsA(TEXT("Combiner")))
		{
			Children = { TEXT("Material1"), TEXT("Material2"), TEXT("Mask"), TEXT("FallbackMaterial") };
		}
		else if (Cur->IsA(TEXT("MaterialSwitch")))
		{
			const FConvProp* Cp = Get(TEXT("Current"));
			Children = { FString::Printf(TEXT("Materials#%d"), Cp ? Cp->Int() : 0), TEXT("FallbackMaterial") };
		}
		else if (Cur->IsA(TEXT("MaterialSequence")))
		{
			Children = { TEXT("SequenceItems#0"), TEXT("FallbackMaterial") };
		}
		else if (Cur->IsA(TEXT("Modifier")))
		{
			Children = { TEXT("Material"), TEXT("FallbackMaterial") };
		}
		else if (Cur->IsA(TEXT("Cubemap")))
		{
			Children = { Get(TEXT("Faces[0]")) ? TEXT("Faces[0]") : TEXT("Faces"), TEXT("FallbackMaterial") };
		}
		else
		{
			Children = { TEXT("FallbackMaterial") };
		}
		for (const FString& K : Children)
		{
			if (FConvObject* Found = Visit(Child(Cur, K), D + 1))
			{
				return Found;
			}
		}
		return nullptr;
	};
	Out.BaseTexture = Visit(Root, 0);
	if (Out.BlendMode.IsEmpty())
	{
		Out.BlendMode = TEXT("opaque");
	}
	return Out;
}

// ---------------------------------------------------------------------------------------------
// textures

FString FConvExporter::ExportTexture(FConvObject* Obj)
{
	const FString PngPath = OutPath(TEXT("textures"), Obj, TEXT(".png"));
	const FConvProps& P = Obj->Props();
	auto ExportFollow = [this, &P]()
	{
		for (const TCHAR* K : { TEXT("Detail"), TEXT("AnimNext") })
		{
			const FConvProp* V = ConvFindProp(P, K);
			if (V && V->IsRef() && V->Truthy())
			{
				ExportRef(V->Ref);
			}
		}
	};
	if (Fresh(Obj, PngPath))
	{
		ExportFollow();
		return Rel(PngPath);
	}
	FTexSpec Spec;
	ETexErr Err = ETexErr::None;
	FString Msg;
	FConvJson Procedural;
	FString FormatName;
	int32 W = 0, H = 0, MipCount = 0;
	bool bMasked = false, bAlphaTex = false, bTwoSided = false;
	if (PrepareTexture(Obj, Spec, Err, Msg))
	{
		FormatName = ArenaConvImage::FormatName(Spec.Format);
		W = Spec.W; H = Spec.H; MipCount = Spec.MipCount;
		bMasked = Spec.bMasked; bAlphaTex = Spec.bAlphaTexture; bTwoSided = Spec.bTwoSided;
	}
	else
	{
		const bool bProcedural = IsProcedural(Obj);
		const bool bStripped = Spec.bNoPixels && !bProcedural;
		if (Err != ETexErr::Texture || (!bProcedural && !bStripped))
		{
			Error(FString::Printf(TEXT("%s (%s)"), *Obj->Path(), *Obj->ClassName()), Msg);
			return FString();
		}
		// procedural textures keep no pixels: their source texture, else a flat image in the palette's mean colour
		bool bFromSource = false;
		for (const TCHAR* K : { TEXT("SourceTexture"), TEXT("GlassTexture") })
		{
			const FConvProp* Ref = ConvFindProp(P, K);
			FConvObject* Src = Ref && Ref->IsRef() && Ref->Truthy() ? Ref->Ref.Load() : nullptr;
			if (Src && Src->IsA(TEXT("Texture")))
			{
				FTexSpec SrcSpec;
				ETexErr SrcErr = ETexErr::None;
				FString SrcMsg;
				if (PrepareTexture(Src, SrcSpec, SrcErr, SrcMsg))
				{
					Spec = MoveTemp(SrcSpec);
					FormatName = ArenaConvImage::FormatName(Spec.Format);
					W = Spec.W; H = Spec.H; MipCount = Spec.MipCount;
					bMasked = Spec.bMasked; bAlphaTex = Spec.bAlphaTexture; bTwoSided = Spec.bTwoSided;
					Procedural = FConvJson::Object();
					Procedural.Set(TEXT("from"), K);
					Procedural.Set(TEXT("texture"), Src->Path());
					Procedural.Set(TEXT("reason"), Msg);
					bFromSource = true;
					break;
				}
				if (SrcErr == ETexErr::Other)
				{
					Error(FString::Printf(TEXT("%s (%s)"), *Obj->Path(), *Obj->ClassName()), SrcMsg);
					return FString();
				}
			}
		}
		if (!bFromSource)
		{
			const FConvProp* Fmt = ConvFindProp(P, TEXT("Format"));
			FormatName = ArenaConvImage::FormatName(Fmt ? Fmt->Int() : 0);
			const FConvProp* Bm = ConvFindProp(P, TEXT("bMasked")); bMasked = Bm && Bm->Truthy();
			const FConvProp* Ba = ConvFindProp(P, TEXT("bAlphaTexture")); bAlphaTex = Ba && Ba->Truthy();
			const FConvProp* Bt = ConvFindProp(P, TEXT("bTwoSided")); bTwoSided = Bt && Bt->Truthy();
			const FConvProp* Us = Obj->GetProp(TEXT("USize"));
			const FConvProp* Vs = Obj->GetProp(TEXT("VSize"));
			W = FMath::Max(1, FMath::Min(Us && Us->Truthy() ? Us->Int() : 64, 512));
			H = FMath::Max(1, FMath::Min(Vs && Vs->Truthy() ? Vs->Int() : 64, 512));
			MipCount = 0;
			Spec = FTexSpec();
			Spec.bFlat = true;
			Spec.W = W;
			Spec.H = H;
			const FConvProp* PalRef = ConvFindProp(P, TEXT("Palette"));
			FConvObject* PalObj = PalRef && PalRef->IsRef() && PalRef->Truthy() ? PalRef->Ref.Load() : nullptr;
			// stripped textures (no mip has pixels; the retail game draws them with its default texture): a flat
			// stand-in in MipZero, the average colour UE2 stores with every texture, else the palette's mean colour
			const FConvProp* MipZero = bStripped ? ConvFindProp(P, TEXT("MipZero")) : nullptr;
			if (MipZero && MipZero->IsStruct())
			{
				Spec.Flat[0] = (uint8)MipZero->Get(TEXT("R")).Int();
				Spec.Flat[1] = (uint8)MipZero->Get(TEXT("G")).Int();
				Spec.Flat[2] = (uint8)MipZero->Get(TEXT("B")).Int();
			}
			else if (PalObj)
			{
				TArray<uint8> Pal;
				ETexErr PE;
				FString PM;
				if (ReadPalette(PalObj, Pal, PE, PM))
				{
					for (int32 c = 0; c < 3; ++c)
					{
						double Sum = 0;
						for (int32 i = 0; i < 256; ++i)
						{
							Sum += Pal[i * 4 + c];
						}
						Spec.Flat[c] = (uint8)int32(Sum / 256.0);
					}
				}
			}
			Procedural = FConvJson::Object();
			Procedural.Set(TEXT("from"), (MipZero && MipZero->IsStruct()) ? TEXT("mip_zero_color") : PalObj ? TEXT("palette_mean") : TEXT("grey"));
			Procedural.Set(TEXT("reason"), Msg);
			if (bStripped)
			{
				Procedural.Set(TEXT("stripped"), true);
				Note_(FString::Printf(TEXT("%s (%s): no pixel data in the package (%s); flat %s stand-in written"), *Obj->Path(), *Obj->ClassName(),
					*Msg, *Procedural.GetString(TEXT("from"))));
			}
		}
	}

	const FMatResolve Hints = ResolveMaterial(Obj);
	auto RefCanon = [this, &P](const TCHAR* K) -> FConvJson
	{
		const FConvProp* V = ConvFindProp(P, K);
		return V && V->Truthy() && V->IsRef() ? FConvJson(Canon(V->Ref)) : FConvJson();
	};
	auto Raw = [this, &P](const TCHAR* K) -> FConvJson
	{
		const FConvProp* V = ConvFindProp(P, K);
		return V ? Jsonify(*V) : FConvJson();
	};
	FConvJson Meta = FConvJson::Object();
	Meta.Set(TEXT("type"), TEXT("Texture"));
	Meta.Set(TEXT("class"), Obj->ClassName());
	Meta.Set(TEXT("path"), Obj->Path());
	Meta.Set(TEXT("scale"), Opt.Scale);
	Meta.Set(TEXT("png"), FPaths::GetCleanFilename(PngPath));
	Meta.Set(TEXT("format"), FormatName);
	Meta.Set(TEXT("width"), W);
	Meta.Set(TEXT("height"), H);
	Meta.Set(TEXT("mip_count"), MipCount);
	if (Spec.MipUsed > 0 && !Spec.bFlat)
	{
		// the top mips were stripped from the package; width / height are those of this mip
		Meta.Set(TEXT("mip_used"), Spec.MipUsed);
	}
	Meta.Set(TEXT("bMasked"), bMasked);
	Meta.Set(TEXT("bAlphaTexture"), bAlphaTex);
	Meta.Set(TEXT("bTwoSided"), bTwoSided);
	Meta.Set(TEXT("has_alpha"), false);      // filled in by the task
	Meta.Set(TEXT("png_bit_depth"), 8);      // filled in by the task
	Meta.Set(TEXT("procedural_fallback"), Procedural);
	FConvJson MH = FConvJson::Object();
	MH.Set(TEXT("blend_mode"), Hints.BlendMode);
	MH.Set(TEXT("two_sided"), Hints.bTwoSided);
	MH.Set(TEXT("detail_texture"), RefCanon(TEXT("Detail")));
	MH.Set(TEXT("detail_scale"), Raw(TEXT("DetailScale")));
	MH.Set(TEXT("u_clamp_mode"), Raw(TEXT("UClampMode")));
	MH.Set(TEXT("v_clamp_mode"), Raw(TEXT("VClampMode")));
	MH.Set(TEXT("lod_set"), Raw(TEXT("LODSet")));
	MH.Set(TEXT("palette"), RefCanon(TEXT("Palette")));
	MH.Set(TEXT("anim_next"), RefCanon(TEXT("AnimNext")));
	MH.Set(TEXT("max_frame_rate"), Raw(TEXT("MaxFrameRate")));
	MH.Set(TEXT("surface_type"), Raw(TEXT("SurfaceType")));
	Meta.Set(TEXT("material_hints"), MoveTemp(MH));

	const FString ObjPath = Obj->Path();
	const int64 Bytes = int64(W) * H * 4 * 2;
	const int32 Level = Opt.PngLevel;
	Launch(Bytes, [this, Spec = MoveTemp(Spec), Meta = MoveTemp(Meta), PngPath, ObjPath, Level]() mutable
	{
		FConvImage Img;
		FString DecodeError;
		if (Spec.bFlat)
		{
			Img.W = Spec.W;
			Img.H = Spec.H;
			Img.Channels = 4;
			Img.Pixels.SetNumUninitialized(int64(Img.W) * Img.H * 4);
			for (int64 i = 0; i < int64(Img.W) * Img.H; ++i)
			{
				Img.Pixels[i * 4 + 0] = Spec.Flat[0];
				Img.Pixels[i * 4 + 1] = Spec.Flat[1];
				Img.Pixels[i * 4 + 2] = Spec.Flat[2];
				Img.Pixels[i * 4 + 3] = 255;
			}
		}
		else if (!ArenaConvImage::Decode(Spec.Format, Spec.Data->Data + Spec.Off, Spec.Len, Spec.W, Spec.H,
			Spec.Palette.Num() ? Spec.Palette.GetData() : nullptr, Img, DecodeError))
		{
			FScopeLock Lock(&TaskLock);
			TaskErrors.Add(FString::Printf(TEXT("%s: %s"), *ObjPath, *DecodeError));
			return;
		}
		Spec.Data.Reset();
		const bool bAlpha = Img.HasAlpha();
		if (!Img.Is16() && !bAlpha)
		{
			Img.StripAlpha();
		}
		TArray64<uint8> Png;
		if (!ArenaConvImage::EncodePng(Img, Png, Level) || !ConvWriteFile(PngPath, Png.GetData(), Png.Num()))
		{
			FScopeLock Lock(&TaskLock);
			TaskErrors.Add(FString::Printf(TEXT("%s: cannot write %s"), *ObjPath, *PngPath));
			return;
		}
		Meta.Set(TEXT("has_alpha"), bAlpha);
		Meta.Set(TEXT("png_bit_depth"), Img.Is16() ? 16 : 8);
		Meta.SaveToFile(FPaths::ChangeExtension(PngPath, TEXT(".json")), 1);
		FConvJson St = ArenaConvImage::Stats(Img, Img.Channels == 4);
		St.Set(TEXT("mtime"), FileMTime(PngPath));
		St.Set(TEXT("v"), 1);
		FScopeLock Lock(&TaskLock);
		TextureStats.Add(ObjPath, MoveTemp(St));
	});
	Counts[TEXT("textures")]++;
	ExportFollow();
	return Rel(PngPath);
}

// ---------------------------------------------------------------------------------------------
// materials

FString FConvExporter::ExportMaterial(FConvObject* Obj)
{
	const FString Path = OutPath(TEXT("materials"), Obj, TEXT(".json"));
	TArray<FConvRef> Refs;
	CollectRefs(Obj->Props(), Refs);
	for (const FConvRef& R : Refs)
	{
		ExportRef(R);
	}
	const FMatResolve Res = ResolveMaterial(Obj);
	FString BaseFile;
	FConvJson Size;
	if (Res.BaseTexture)
	{
		BaseFile = ExportObject(Res.BaseTexture);
		const FIntPoint S = TextureSize(Res.BaseTexture);
		Size = FConvJson::Array();
		Size.Add(S.X);
		Size.Add(S.Y);
	}
	FConvJson Meta = FConvJson::Object();
	Meta.Set(TEXT("type"), TEXT("Material"));
	Meta.Set(TEXT("class"), Obj->ClassName());
	FConvJson Chain = FConvJson::Array();
	for (const FString& C : Obj->ClassChain())
	{
		Chain.Add(C);
	}
	Meta.Set(TEXT("class_chain"), MoveTemp(Chain));
	Meta.Set(TEXT("path"), Obj->Path());
	Meta.Set(TEXT("scale"), Opt.Scale);
	Meta.Set(TEXT("base_texture"), Res.BaseTexture ? FConvJson(Res.BaseTexture->Path()) : FConvJson());
	Meta.Set(TEXT("base_texture_file"), FConvJson::StrOrNull(BaseFile));
	Meta.Set(TEXT("base_texture_size"), Size);
	Meta.Set(TEXT("blend_mode"), Res.BlendMode);
	Meta.Set(TEXT("two_sided"), Res.bTwoSided);
	Meta.Set(TEXT("unlit"), Res.bUnlit);
	Meta.Set(TEXT("emissive"), Res.Emissive);
	Meta.Set(TEXT("opacity"), Res.Opacity);
	Meta.Set(TEXT("specular"), Res.Specular);
	Meta.Set(TEXT("detail"), Res.Detail);
	Meta.Set(TEXT("resolve_chain"), Res.Chain);
	Meta.Set(TEXT("properties"), JsonifyProps(Obj->Props()));
	if (Res.bHasShaderOutput) Meta.Set(TEXT("shader_output_blending"), Res.ShaderOutputBlending);
	if (Res.bHasFrameBuffer) Meta.Set(TEXT("frame_buffer_blending"), Res.FrameBufferBlending);
	if (Res.bHasAlphaRef) Meta.Set(TEXT("alpha_ref"), Res.AlphaRef);
	if (Res.bHasConstantColor) Meta.Set(TEXT("constant_color"), Res.ConstantColor);
	WriteJson(Path, Meta);
	Counts[TEXT("materials")]++;
	return Rel(Path);
}

// ---------------------------------------------------------------------------------------------
// static meshes (staticmesh.py)

FString FConvExporter::ExportStaticMesh(FConvObject* Obj)
{
	const FString GPath = OutPath(TEXT("staticmeshes"), Obj, TEXT(".gltf"));
	const FConvProps& P = Obj->Props();
	const FConvProp* Mats = ConvFindProp(P, TEXT("Materials"));
	auto MatRef = [Mats](int32 i) -> const FConvProp*
	{
		if (!Mats || !Mats->IsArray() || i >= Mats->A.Num())
		{
			return nullptr;
		}
		const FConvProp& M = Mats->A[i];
		return M.IsStruct() ? M.Find(TEXT("Material")) : &M;
	};
	auto SectionCollision = [Mats](int32 i)
	{
		if (Mats && Mats->IsArray() && i < Mats->A.Num() && Mats->A[i].IsStruct())
		{
			const FConvProp* E = Mats->A[i].Find(TEXT("EnableCollision"));
			return E && E->Truthy();
		}
		return false;
	};
	if (Fresh(Obj, GPath, FString(), StaticMeshToolVersion))
	{
		if (Mats && Mats->IsArray())
		{
			for (int32 i = 0; i < Mats->A.Num(); ++i)
			{
				const FConvProp* R = MatRef(i);
				if (R && R->IsRef() && R->Truthy())
				{
					ExportRef(R->Ref);
				}
			}
		}
		return Rel(GPath);
	}
	const FString What = FString::Printf(TEXT("%s (%s)"), *Obj->Path(), *Obj->ClassName());
	if (Obj->Pkg->Version < 112)
	{
		Error(What, FString::Printf(TEXT("StaticMesh version %d not supported"), Obj->Pkg->Version));
		return FString();
	}
	FConvReader R = Obj->NativeReader();
	float BMin[3], BMax[3];
	R.Vector(BMin);
	R.Vector(BMax);
	R.U8();
	R.Skip(16);
	const int32 NSec = R.Index();
	if (R.bError || !(NSec >= 0 && NSec < 4096))
	{
		Error(What, R.bError ? R.Error : FString::Printf(TEXT("bad section count %d"), NSec));
		return FString();
	}
	struct FSec { int32 FirstIndex, NumFaces; };
	TArray<FSec> Sections;
	for (int32 i = 0; i < NSec; ++i)
	{
		R.I32();
		const int32 FirstIndex = R.U16();
		R.U16();
		R.U16();
		R.U16();
		const int32 NumFaces = R.U16();
		Sections.Add({ FirstIndex, NumFaces });
	}
	R.Skip(25);
	const int32 Nv = R.Index();
	if (R.bError || Nv < 0 || int64(Nv) * 24 > R.Remaining())
	{
		Error(What, R.bError ? R.Error : FString::Printf(TEXT("bad vertex count %d"), Nv));
		return FString();
	}
	const float* VData = (const float*)R.Bytes(int64(Nv) * 24);
	R.I32();
	const int32 Nc = R.Index();
	R.Skip(int64(Nc) * 4);
	R.I32();
	const int32 Na = R.Index();
	R.Skip(int64(Na) * 4);
	R.I32();
	const int32 NUvs = R.Index();
	TArray<TPair<const float*, int32>> Uvs;
	for (int32 i = 0; i < NUvs && !R.bError; ++i)
	{
		const int32 C = R.Index();
		const float* Uv = (const float*)R.Bytes(int64(C) * 8);
		R.I32();
		R.I32();
		Uvs.Emplace(Uv, C);
	}
	const int32 Ni = R.Index();
	const uint8* IData = R.Bytes(int64(Ni) * 2);
	R.I32();
	const int32 Ni2 = R.Index();
	R.Skip(int64(Ni2) * 2);
	R.I32();
	if (R.bError || !VData || (Ni > 0 && !IData))
	{
		Error(What, R.Error);
		return FString();
	}
	FConvReader R2 = R;
	int32 CollisionRef = R2.Index();
	const bool bHasCollisionRef = !R2.bError;
	if (!bHasCollisionRef)
	{
		CollisionRef = 0;
	}
	TArray<uint32> Indices;
	Indices.SetNumUninitialized(FMath::Max(Ni, 0));
	for (int32 i = 0; i < Ni; ++i)
	{
		Indices[i] = IData[i * 2] | (IData[i * 2 + 1] << 8);
	}

	// section materials
	TArray<FString> MatPaths;
	for (int32 i = 0; i < NSec; ++i)
	{
		const FConvProp* MR = MatRef(i);
		if (MR && MR->IsRef() && MR->Truthy())
		{
			ExportRef(MR->Ref);
			MatPaths.Add(Canon(MR->Ref));
		}
		else
		{
			MatPaths.Add(TEXT("None"));
		}
	}
	// primitives with section-local vertex sets
	TArray<FConvPrim> Prims;
	const float S = (float)Opt.Scale;
	for (int32 Si = 0; Si < NSec; ++Si)
	{
		const int32 A = Sections[Si].FirstIndex;
		const int32 B = A + 3 * Sections[Si].NumFaces;
		if (B > Indices.Num())
		{
			Error(What, FString::Printf(TEXT("section %d indices out of range (%d > %d)"), Si, B, Indices.Num()));
			return FString();
		}
		if (B == A)
		{
			continue;
		}
		TArray<uint32> Used;
		for (int32 k = A; k < B; ++k)
		{
			if (Indices[k] >= uint32(Nv))
			{
				Error(What, FString::Printf(TEXT("section %d vertex index out of range"), Si));
				return FString();
			}
			Used.Add(Indices[k]);
		}
		Used.Sort();
		Used.SetNum(Algo::Unique(Used));
		TMap<uint32, uint32> Remap;
		Remap.Reserve(Used.Num());
		for (int32 k = 0; k < Used.Num(); ++k)
		{
			Remap.Add(Used[k], k);
		}
		FConvPrim Prim;
		const bool bUv = Uvs.Num() && Uvs[0].Value == Nv && Uvs[0].Key;
		for (uint32 U : Used)
		{
			const float* V = VData + int64(U) * 6;
			Prim.Positions.Add(V[0] * S);
			Prim.Positions.Add(V[1] * S);
			Prim.Positions.Add(V[2] * S);
			Prim.Normals.Add(V[3]);
			Prim.Normals.Add(V[4]);
			Prim.Normals.Add(V[5]);
			if (bUv)
			{
				Prim.UVs.Add(Uvs[0].Key[U * 2]);
				Prim.UVs.Add(Uvs[0].Key[U * 2 + 1]);
			}
		}
		for (int32 k = A; k < B; ++k)
		{
			Prim.Indices.Add(Remap[Indices[k]]);
		}
		Prim.Material = MatPaths[Si];
		Prim.Extras = FConvJson::Object();
		Prim.Extras.Set(TEXT("section"), Si);
		Prim.Extras.Set(TEXT("enable_collision"), SectionCollision(Si));
		Prims.Add(MoveTemp(Prim));
	}
	// UE2 collides non-zero extent traces (players) with the collision model when UseSimpleBoxCollision, zero extent
	// ones (shots) when UseSimpleLineCollision, else with the collision-enabled sections (UStaticMesh::LineCheck)
	auto BoolProp = [&P](const TCHAR* K, bool bDefault)
	{
		const FConvProp* V = ConvFindProp(P, K);
		return V ? V->Truthy() : bDefault;
	};
	const bool bModelBox = BoolProp(TEXT("UseSimpleBoxCollision"), true), bModelLine = BoolProp(TEXT("UseSimpleLineCollision"), false);
	int32 ModelTris = 0;
	if (CollisionRef && (bModelBox || bModelLine))
	{
		FConvObject* Mo = Obj->Pkg->MakeRef(CollisionRef).Load();
		FConvPrim Col;
		FString ColError;
		if (Mo && Mo->ClassName() == TEXT("Model") && ReadCollisionModel(Mo, S, Col, ColError))
		{
			ModelTris = Col.Indices.Num() / 3;
			Col.Extras = FConvJson::Object();
			Col.Extras.Set(TEXT("collision_model"), true);
			Col.Extras.Set(TEXT("box"), bModelBox);
			Col.Extras.Set(TEXT("line"), bModelLine);
			Prims.Add(MoveTemp(Col));
		}
		else if (!ColError.IsEmpty())
		{
			Error(What, FString::Printf(TEXT("collision model: %s"), *ColError));
		}
	}
	FConvGltf Gb(Opt.Scale);
	Gb.AddPrimitiveList(Obj->Name(), Prims);
	FConvJson Extra = FConvJson::Object();
	Extra.Set(TEXT("source"), Obj->Path());
	Gb.Write(GPath, Extra);

	FConvJson KarmaProps;
	if (const FConvProp* K = ConvFindProp(P, TEXT("KarmaProps")))
	{
		if (K->IsRef() && K->Truthy())
		{
			if (FConvObject* Ko = K->Ref.Load())
			{
				KarmaProps = JsonifyProps(Ko->Props());
			}
		}
	}
	FConvJson ColModel;
	if (CollisionRef)
	{
		ColModel = Obj->Pkg->RefPath(CollisionRef);
	}
	FConvJson Meta = FConvJson::Object();
	Meta.Set(TEXT("type"), TEXT("StaticMesh"));
	Meta.Set(TEXT("tool_version"), StaticMeshToolVersion);
	Meta.Set(TEXT("path"), Obj->Path());
	Meta.Set(TEXT("scale"), Opt.Scale);
	Meta.Set(TEXT("gltf"), FPaths::GetCleanFilename(GPath));
	Meta.Set(TEXT("vertex_count"), Nv);
	Meta.Set(TEXT("triangle_count"), Ni / 3);
	Meta.Set(TEXT("uv_channels"), Uvs.Num());
	Meta.Set(TEXT("has_vertex_colors"), Nc != 0);
	FConvJson Bounds = FConvJson::Object();
	Bounds.Set(TEXT("min"), Vec3(BMin, Opt.Scale));
	Bounds.Set(TEXT("max"), Vec3(BMax, Opt.Scale));
	Meta.Set(TEXT("bounds"), MoveTemp(Bounds));
	FConvJson Secs = FConvJson::Array();
	for (int32 i = 0; i < NSec; ++i)
	{
		FConvJson Sj = FConvJson::Object();
		Sj.Set(TEXT("index"), i);
		Sj.Set(TEXT("material"), MatPaths[i]);
		Sj.Set(TEXT("triangles"), Sections[i].NumFaces);
		Sj.Set(TEXT("enable_collision"), SectionCollision(i));
		Secs.Add(MoveTemp(Sj));
	}
	Meta.Set(TEXT("sections"), MoveTemp(Secs));
	FConvJson Col = FConvJson::Object();
	Col.Set(TEXT("UseSimpleLineCollision"), BoolProp(TEXT("UseSimpleLineCollision"), false));
	Col.Set(TEXT("UseSimpleBoxCollision"), BoolProp(TEXT("UseSimpleBoxCollision"), true));
	Col.Set(TEXT("UseSimpleKarmaCollision"), BoolProp(TEXT("UseSimpleKarmaCollision"), true));
	Col.Set(TEXT("UseVertexColor"), BoolProp(TEXT("UseVertexColor"), false));
	Col.Set(TEXT("collision_model"), ColModel);
	Col.Set(TEXT("collision_model_triangles"), ModelTris);
	Col.Set(TEXT("karma_props"), KarmaProps);
	Col.Set(TEXT("note"), TEXT("karma_props values are raw UE2 units (multiply lengths by scale)"));
	Meta.Set(TEXT("collision"), MoveTemp(Col));
	WriteJson(FPaths::ChangeExtension(GPath, TEXT(".json")), Meta);
	Counts[TEXT("staticmeshes")]++;
	return Rel(GPath);
}

// ---------------------------------------------------------------------------------------------
// sounds (sound.py)

FString FConvExporter::ExportSound(FConvObject* Obj)
{
	const FString MetaPath = OutPath(TEXT("sounds"), Obj, TEXT(".json"));
	const FConvProp* Base = ConvFindProp(Obj->Props(), TEXT("BaseSound"));
	if (Opt.bSkipExisting && Fresh(Obj, MetaPath, MetaPath))
	{
		FConvJson Prev;
		if (FConvJson::LoadFile(MetaPath, Prev))
		{
			const FString File = Prev.GetString(TEXT("file"));
			const FString Main = File.IsEmpty() ? MetaPath : FPaths::GetPath(MetaPath) / File;
			if (IFileManager::Get().FileExists(*Main))
			{
				if (Base && Base->IsRef() && Base->Truthy())
				{
					ExportRef(Base->Ref);
				}
				return Rel(Main);
			}
		}
	}
	const FString What = FString::Printf(TEXT("%s (%s)"), *Obj->Path(), *Obj->ClassName());
	FString Fmt = TEXT("None");
	int64 Off = 0, Len = 0;
	if (!Obj->IsA(TEXT("ProceduralSound")))
	{
		FConvReader R = Obj->NativeReader();
		const int32 FmtIdx = R.Index();
		if (R.bError || !Obj->Pkg->NameAt(FmtIdx, Fmt))
		{
			Error(What, R.bError ? R.Error : FString::Printf(TEXT("name index %d out of range"), FmtIdx));
			return FString();
		}
		if (Obj->Pkg->Licensee >= 2)
		{
			R.F32();
		}
		if (!R.LazyBytesSpan(Obj->Pkg->Version > 61, Off, Len))
		{
			Error(What, R.Error);
			return FString();
		}
	}
	// ProceduralSound has no samples of its own: it plays BaseSound with the pitch / volume settings of its
	// properties. Its native data is not a USound payload (reading it as one only worked by accident).
	const uint8* Data = Obj->Pkg->Buf() + Off;
	FConvJson Meta = FConvJson::Object();
	Meta.Set(TEXT("type"), TEXT("Sound"));
	Meta.Set(TEXT("class"), Obj->ClassName());
	Meta.Set(TEXT("path"), Obj->Path());
	Meta.Set(TEXT("scale"), Opt.Scale);
	Meta.Set(TEXT("format_name"), Fmt);
	Meta.Set(TEXT("bytes"), Len);
	Meta.Set(TEXT("properties"), JsonifyProps(Obj->Props()));
	if (Len == 0)
	{
		Meta.Set(TEXT("file"), FConvJson());
		Meta.Set(TEXT("container"), FConvJson());
		const bool bBase = Base && Base->IsRef() && Base->Truthy();
		Meta.Set(TEXT("base_sound"), bBase ? FConvJson(Canon(Base->Ref)) : FConvJson());
		Meta.Set(TEXT("base_sound_file"), bBase ? FConvJson::StrOrNull(ExportRef(Base->Ref)) : FConvJson());
		WriteJson(MetaPath, Meta);
		Counts[TEXT("sounds")]++;
		return Rel(MetaPath);
	}
	FString Kind = SoundKind(Data, Len);
	if (Kind.IsEmpty())
	{
		Kind = Safe(Fmt.ToLower());
	}
	if (Kind.IsEmpty())
	{
		Kind = TEXT("bin");
	}
	const FString Path = OutPath(TEXT("sounds"), Obj, *(TEXT(".") + Kind));
	Meta.Set(TEXT("file"), FPaths::GetCleanFilename(Path));
	Meta.Set(TEXT("container"), Kind);
	if (Kind == TEXT("wav"))
	{
		WavInfo(Data, Len, Meta);
	}
	TSharedPtr<FConvPackageData, ESPMode::ThreadSafe> Keep = Obj->Pkg->Data;
	const FString JsonPath = FPaths::ChangeExtension(Path, TEXT(".json"));
	Launch(Len, [this, Keep, Data, Len, Path, JsonPath, Meta = MoveTemp(Meta)]()
	{
		if (!ConvWriteFile(Path, Data, Len) || !Meta.SaveToFile(JsonPath, 1))
		{
			FScopeLock Lock(&TaskLock);
			TaskErrors.Add(FString::Printf(TEXT("%s: cannot write"), *Path));
		}
	});
	Counts[TEXT("sounds")]++;
	return Rel(Path);
}

// ---------------------------------------------------------------------------------------------
// music (music.py)

TArray<FString> FConvExporter::MusicFiles() const
{
	TArray<FString> Out;
	FString MusicDir;
	IFileManager::Get().IterateDirectory(*Mgr.GameDir, [&](const TCHAR* P, bool bDir)
	{
		if (bDir && FPaths::GetCleanFilename(P).Equals(TEXT("Music"), ESearchCase::IgnoreCase))
		{
			MusicDir = P;
			return false;
		}
		return true;
	});
	if (MusicDir.IsEmpty())
	{
		return Out;
	}
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(MusicDir / TEXT("*")), true, false);
	Files.Sort([](const FString& A, const FString& B) { return FCString::Strcmp(*A, *B) < 0; });
	for (const FString& F : Files)
	{
		if (F.EndsWith(TEXT(".ogg"), ESearchCase::IgnoreCase))
		{
			Out.Add(MusicDir / F);
		}
	}
	return Out;
}

FString FConvExporter::ExportMusicFile(const FString& Path)
{
	const FString Name = FPaths::GetBaseFilename(Path);
	const FString Dir = Opt.OutDir / TEXT("music");
	const FString Dst = Dir / (Safe(Name) + TEXT(".ogg"));
	const FString MetaPath = Dir / (Safe(Name) + TEXT(".json"));
	IFileManager& FM = IFileManager::Get();
	if (Opt.bSkipExisting && FM.FileExists(*Dst) && FM.FileExists(*MetaPath) && FM.GetTimeStamp(*MetaPath) >= FM.GetTimeStamp(*Path))
	{
		return Rel(Dst);
	}
	TArray64<uint8> Data;
	if (!FFileHelper::LoadFileToArray(Data, *Path))
	{
		Error(FString::Printf(TEXT("music %s"), *Path), TEXT("cannot read"));
		return FString();
	}
	FConvJson Meta = FConvJson::Object();
	Meta.Set(TEXT("type"), TEXT("Music"));
	Meta.Set(TEXT("name"), Name);
	Meta.Set(TEXT("source_file"), FPaths::GetCleanFilename(Path));
	Meta.Set(TEXT("file"), FPaths::GetCleanFilename(Dst));
	Meta.Set(TEXT("bytes"), Data.Num());
	Meta.Set(TEXT("tool_version"), ToolVersion);
	Meta.Set(TEXT("container"), TEXT("ogg"));
	OggInfo(Data.GetData(), Data.Num(), Meta);
	const int64 Bytes = Data.Num();
	Launch(Bytes, [this, Data = MoveTemp(Data), Dst, MetaPath, Meta = MoveTemp(Meta)]()
	{
		if (!ConvWriteFile(Dst, Data.GetData(), Data.Num()) || !Meta.SaveToFile(MetaPath, 1))
		{
			FScopeLock Lock(&TaskLock);
			TaskErrors.Add(FString::Printf(TEXT("music %s: cannot write"), *Dst));
		}
	});
	Counts[TEXT("music")]++;
	return Rel(Dst);
}

// ---------------------------------------------------------------------------------------------
// whole packages

void FConvExporter::ExportPackage(const FString& Name)
{
	FConvPackage* Pkg = Mgr.LoadPackage(Name);
	if (!Pkg)
	{
		Error(FString::Printf(TEXT("package %s"), *Name), TEXT("not found"));
		return;
	}
	static const TSet<FString> Skip = { TEXT("Package"), TEXT("Class"), TEXT("Palette"), TEXT("Polys"), TEXT("Model"), TEXT("ReachSpec"),
		TEXT("Function"), TEXT("State"), TEXT("Enum"), TEXT("Const"), TEXT("Struct"), TEXT("TextBuffer"), TEXT("Field") };
	for (int32 i = 0; i < Pkg->Exports.Num(); ++i)
	{
		if (IsCancelled())
		{
			return;
		}
		const FString Cn = Pkg->ExportClassName(i);
		if (Skip.Contains(Cn) || Cn.EndsWith(TEXT("Property")))
		{
			continue;
		}
		FConvObject* Obj = Pkg->GetObject(i);
		if (!Obj || KindOf(Obj).IsEmpty())
		{
			continue;
		}
		ExportObject(Obj);
	}
}

#endif // ARENA_CONV_BUILD
