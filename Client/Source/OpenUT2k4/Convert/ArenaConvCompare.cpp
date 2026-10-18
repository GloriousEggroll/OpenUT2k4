#include "ArenaConvConfig.h"
#if ARENA_CONV_BUILD
// Structural comparison of a converted cache with a reference cache from Tools/ut2k4_export.py
// (developer tool behind -ConvertCompare=<dir>; see ArenaConverter.h).

#include "ArenaConverter.h"
#include "ArenaConvImage.h"
#include "ArenaConvJson.h"
#include "../OpenUT2k4.h"
#include "Async/ParallelFor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

namespace ArenaConvCompareLocal
{
	struct FDiff
	{
		TArray<FString> Lines;
		int32 Count = 0;
		void Add(const FString& L)
		{
			++Count;
			if (Lines.Num() < 6) Lines.Add(L);
		}
	};

	bool Ignored(const FString& Key)
	{
		static const TSet<FString> Keys = { TEXT("export_stats"), TEXT("mtime"), TEXT("seconds"), TEXT("created"), TEXT("timings_s"), TEXT("note") };
		return Keys.Contains(Key);
	}

	bool NumEq(double A, double B)
	{
		if (FMath::IsNaN(A) && FMath::IsNaN(B)) return true;
		const double D = FMath::Abs(A - B);
		return D <= 2e-3 || D <= 1e-4 * FMath::Max(FMath::Abs(A), FMath::Abs(B));
	}

	FString Short(const FConvJson& V)
	{
		TArray<ANSICHAR> B;
		V.Write(B, -1);
		if (B.Num() > 120) { B.SetNum(120); B.Append("...", 3); }
		B.Add(0);
		return FString(UTF8_TO_TCHAR(B.GetData()));
	}

	void CompareJson(const FConvJson& A, const FConvJson& B, const FString& Path, FDiff& D)
	{
		if (A.IsNumber() && B.IsNumber())
		{
			if (!NumEq(A.AsNumber(), B.AsNumber())) D.Add(FString::Printf(TEXT("%s: %s != %s"), *Path, *Short(A), *Short(B)));
			return;
		}
		if (A.Type != B.Type)
		{
			D.Add(FString::Printf(TEXT("%s: type %s != %s"), *Path, *Short(A), *Short(B)));
			return;
		}
		switch (A.Type)
		{
		case FConvJson::EType::Bool: if (A.B != B.B) D.Add(FString::Printf(TEXT("%s: %d != %d"), *Path, A.B, B.B)); return;
		case FConvJson::EType::String: if (A.S != B.S) D.Add(FString::Printf(TEXT("%s: \"%s\" != \"%s\""), *Path, *A.S.Left(80), *B.S.Left(80))); return;
		case FConvJson::EType::Array:
			if (A.A.Num() != B.A.Num()) { D.Add(FString::Printf(TEXT("%s: %d items != %d"), *Path, A.A.Num(), B.A.Num())); return; }
			if (A.A.Num() == 4 && (Path.EndsWith(TEXT(".rotation")) || Path.EndsWith(TEXT(".rotation_quat"))))
			{
				// quaternions: q and -q are the same rotation (sign of a near-zero w is rounding)
				bool bSame = true, bNeg = true;
				for (int32 i = 0; i < 4; ++i)
				{
					bSame &= NumEq(A.A[i].AsNumber(), B.A[i].AsNumber());
					bNeg &= NumEq(A.A[i].AsNumber(), -B.A[i].AsNumber());
				}
				if (!bSame && !bNeg) D.Add(FString::Printf(TEXT("%s: %s != %s"), *Path, *Short(A), *Short(B)));
				return;
			}
			for (int32 i = 0; i < A.A.Num(); ++i) CompareJson(A.A[i], B.A[i], FString::Printf(TEXT("%s[%d]"), *Path, i), D);
			return;
		case FConvJson::EType::Object:
			for (const TPair<FString, FConvJson>& KV : B.O)
			{
				if (Ignored(KV.Key)) continue;
				const FConvJson* Av = A.Find(KV.Key);
				if (!Av) { D.Add(FString::Printf(TEXT("%s.%s: missing (ref %s)"), *Path, *KV.Key, *Short(KV.Value))); continue; }
				CompareJson(*Av, KV.Value, Path + TEXT(".") + KV.Key, D);
			}
			for (const TPair<FString, FConvJson>& KV : A.O)
			{
				if (!Ignored(KV.Key) && !B.Find(KV.Key) && !KV.Value.IsNull()) D.Add(FString::Printf(TEXT("%s.%s: extra"), *Path, *KV.Key));
			}
			return;
		default: return;
		}
	}

	/** glTF: structure without buffers / views / byte offsets; accessors compared by count, type and min/max. */
	void CompareGltf(const FConvJson& A, const FConvJson& B, FDiff& D)
	{
		auto Acc = [](const FConvJson& Doc, const FConvJson& Idx) -> const FConvJson*
		{
			const int32 I = (int32)Idx.AsNumber();
			const FConvJson& L = Doc.Get(TEXT("accessors"));
			return L.A.IsValidIndex(I) ? &L.A[I] : nullptr;
		};
		auto AccJson = [](const FConvJson* X)
		{
			FConvJson O = FConvJson::Object();
			if (X)
			{
				O.Set(TEXT("count"), X->Get(TEXT("count")));
				O.Set(TEXT("type"), X->Get(TEXT("type")));
				O.Set(TEXT("componentType"), X->Get(TEXT("componentType")));
				if (X->Has(TEXT("min"))) { O.Set(TEXT("min"), X->Get(TEXT("min"))); O.Set(TEXT("max"), X->Get(TEXT("max"))); }
			}
			return O;
		};
		auto Normalize = [&](const FConvJson& Doc)
		{
			FConvJson N = FConvJson::Object();
			N.Set(TEXT("asset"), Doc.Get(TEXT("asset")));
			N.Set(TEXT("scenes"), Doc.Get(TEXT("scenes")));
			N.Set(TEXT("nodes"), Doc.Get(TEXT("nodes")));
			N.Set(TEXT("materials"), Doc.Get(TEXT("materials")));
			FConvJson Meshes = FConvJson::Array();
			for (const FConvJson& M : Doc.Get(TEXT("meshes")).A)
			{
				FConvJson Mj = FConvJson::Object();
				Mj.Set(TEXT("name"), M.Get(TEXT("name")));
				FConvJson Prims = FConvJson::Array();
				for (const FConvJson& P : M.Get(TEXT("primitives")).A)
				{
					FConvJson Pj = FConvJson::Object();
					FConvJson Attrs = FConvJson::Object();
					for (const TPair<FString, FConvJson>& KV : P.Get(TEXT("attributes")).O) Attrs.Set(KV.Key, AccJson(Acc(Doc, KV.Value)));
					Pj.Set(TEXT("attributes"), Attrs);
					Pj.Set(TEXT("indices"), AccJson(Acc(Doc, P.Get(TEXT("indices")))));
					Pj.Set(TEXT("material"), P.Get(TEXT("material")));
					Pj.Set(TEXT("extras"), P.Get(TEXT("extras")));
					Prims.Add(Pj);
				}
				Mj.Set(TEXT("primitives"), Prims);
				Meshes.Add(Mj);
			}
			N.Set(TEXT("meshes"), Meshes);
			FConvJson Skins = FConvJson::Array();
			for (const FConvJson& S : Doc.Get(TEXT("skins")).A)
			{
				FConvJson Sj = FConvJson::Object();
				Sj.Set(TEXT("joints"), S.Get(TEXT("joints")));
				Sj.Set(TEXT("skeleton"), S.Get(TEXT("skeleton")));
				Sj.Set(TEXT("ibm"), AccJson(Acc(Doc, S.Get(TEXT("inverseBindMatrices")))));
				Skins.Add(Sj);
			}
			N.Set(TEXT("skins"), Skins);
			FConvJson Anims = FConvJson::Array();
			for (const FConvJson& An : Doc.Get(TEXT("animations")).A)
			{
				FConvJson Aj = FConvJson::Object();
				Aj.Set(TEXT("name"), An.Get(TEXT("name")));
				Aj.Set(TEXT("extras"), An.Get(TEXT("extras")));
				Aj.Set(TEXT("channels"), An.Get(TEXT("channels")));
				FConvJson Samplers = FConvJson::Array();
				for (const FConvJson& S : An.Get(TEXT("samplers")).A)
				{
					FConvJson Sj = FConvJson::Object();
					Sj.Set(TEXT("input"), AccJson(Acc(Doc, S.Get(TEXT("input")))));
					Sj.Set(TEXT("output"), AccJson(Acc(Doc, S.Get(TEXT("output")))));
					Samplers.Add(Sj);
				}
				Aj.Set(TEXT("samplers"), Samplers);
				Anims.Add(Aj);
			}
			N.Set(TEXT("animations"), Anims);
			return N;
		};
		CompareJson(Normalize(A), Normalize(B), TEXT("gltf"), D);
	}

	void CompareFile(const FString& Conv, const FString& Ref, const FString& Rel, FDiff& D)
	{
		IFileManager& FM = IFileManager::Get();
		if (!FM.FileExists(*Conv))
		{
			D.Add(TEXT("missing in converted output"));
			return;
		}
		const FString Ext = FPaths::GetExtension(Rel).ToLower();
		if (Ext == TEXT("json"))
		{
			FConvJson A, B;
			FString E;
			if (!FConvJson::LoadFile(Conv, A, &E)) { D.Add(TEXT("unreadable: ") + E); return; }
			if (!FConvJson::LoadFile(Ref, B, &E)) { D.Add(TEXT("reference unreadable: ") + E); return; }
			CompareJson(A, B, TEXT("$"), D);
		}
		else if (Ext == TEXT("gltf"))
		{
			FConvJson A, B;
			if (!FConvJson::LoadFile(Conv, A) || !FConvJson::LoadFile(Ref, B)) { D.Add(TEXT("unreadable gltf")); return; }
			CompareGltf(A, B, D);
		}
		else if (Ext == TEXT("png"))
		{
			FConvImage A, B;
			bool bA = false, bB = false;
			if (!ArenaConvImage::LoadPng(Conv, A, bA) || !ArenaConvImage::LoadPng(Ref, B, bB)) { D.Add(TEXT("unreadable png")); return; }
			if (A.W != B.W || A.H != B.H || A.Channels != B.Channels || bA != bB)
			{
				D.Add(FString::Printf(TEXT("png %dx%d ch%d alpha%d != %dx%d ch%d alpha%d"), A.W, A.H, A.Channels, bA, B.W, B.H, B.Channels, bB));
				return;
			}
			int32 MaxDiff = 0;
			int64 NDiff = 0;
			for (int64 i = 0; i < A.Pixels.Num(); ++i)
			{
				const int32 Df = FMath::Abs(int32(A.Pixels[i]) - int32(B.Pixels[i]));
				if (Df) { ++NDiff; MaxDiff = FMath::Max(MaxDiff, Df); }
			}
			if (NDiff) D.Add(FString::Printf(TEXT("png pixels: %lld bytes differ, max %d"), NDiff, MaxDiff));
		}
		else if (Ext == TEXT("bin"))
		{
			const int64 Sa = FM.FileSize(*Conv), Sb = FM.FileSize(*Ref);
			if (Sa != Sb) D.Add(FString::Printf(TEXT("bin size %lld != %lld"), Sa, Sb));
		}
		else
		{
			// wav / ogg / other payloads are copied: byte identical
			TArray64<uint8> A, B;
			FFileHelper::LoadFileToArray(A, *Conv);
			FFileHelper::LoadFileToArray(B, *Ref);
			if (A.Num() != B.Num() || FMemory::Memcmp(A.GetData(), B.GetData(), A.Num()) != 0)
			{
				D.Add(FString::Printf(TEXT("bytes differ (%lld vs %lld)"), A.Num(), B.Num()));
			}
		}
	}
}

using namespace ArenaConvCompareLocal;

int32 FArenaConverter::CompareCaches(const FString& Converted, const FString& Reference, const FString& ReferenceData, const TArray<FString>& Maps)
{
	const double T0 = FPlatformTime::Seconds();
	IFileManager& FM = IFileManager::Get();
	// (rel path in converted, abs reference path)
	TArray<TPair<FString, FString>> Pairs;
	TSet<FString> Seen;
	auto AddPair = [&](const FString& RelConv, const FString& AbsRef)
	{
		if (!Seen.Contains(RelConv) && FM.FileExists(*AbsRef))
		{
			Seen.Add(RelConv);
			Pairs.Emplace(RelConv, AbsRef);
		}
	};
	auto AddCacheFile = [&](const FString& Rel)
	{
		AddPair(Rel, Reference / Rel);
		const FString Ext = FPaths::GetExtension(Rel).ToLower();
		const FString Base = FPaths::ChangeExtension(Rel, TEXT(""));
		if (Ext != TEXT("json")) AddPair(Base + TEXT(".json"), Reference / Base + TEXT(".json"));
		if (Ext == TEXT("gltf")) AddPair(Base + TEXT(".bin"), Reference / Base + TEXT(".bin"));
	};
	if (Maps.Num())
	{
		for (const FString& M : Maps)
		{
			AddCacheFile(TEXT("maps") / M / TEXT("map.json"));
			AddCacheFile(TEXT("maps") / M / TEXT("bsp.gltf"));
			AddPair(TEXT("data") / M + TEXT(".json"), ReferenceData / M + TEXT(".json"));
			FConvJson Doc;
			if (FConvJson::LoadFile(Reference / TEXT("maps") / M / TEXT("map.json"), Doc))
			{
				for (const TPair<FString, FConvJson>& Kind : Doc.Get(TEXT("dependencies")).O)
				{
					for (const FConvJson& E : Kind.Value.A) AddCacheFile(E.GetString(TEXT("file")));
				}
			}
		}
	}
	else
	{
		TArray<FString> Files;
		for (const TCHAR* Kind : { TEXT("textures"), TEXT("materials"), TEXT("staticmeshes"), TEXT("sounds"), TEXT("skeletalmeshes"),
			TEXT("animations"), TEXT("vertmeshes"), TEXT("music"), TEXT("maps") })
		{
			Files.Reset();
			FM.FindFilesRecursive(Files, *(Reference / Kind), TEXT("*"), true, false);
			for (const FString& F : Files)
			{
				FString Rel = F;
				FPaths::MakePathRelativeTo(Rel, *(Reference + TEXT("/")));
				AddPair(Rel, F);
			}
		}
		Files.Reset();
		FM.FindFiles(Files, *(ReferenceData / TEXT("*.json")), true, false);
		for (const FString& F : Files) AddPair(TEXT("data") / F, ReferenceData / F);
	}
	for (const TCHAR* A : { TEXT("Music"), TEXT("Sounds"), TEXT("SkeletalMeshes"), TEXT("Animations") })
	{
		Pairs.Emplace(FString::Printf(TEXT("data/Assets/%s.json"), A), ReferenceData / TEXT("Assets") / FString(A) + TEXT(".json"));
	}

	TArray<FDiff> Diffs;
	Diffs.SetNum(Pairs.Num());
	ParallelFor(Pairs.Num(), [&](int32 i)
	{
		const FString& Rel = Pairs[i].Key;
		const FString Conv = Converted / Rel;
		if (Rel.StartsWith(TEXT("data/Assets/")))
		{
			// tables: entries of the reference that exist in the converted table (subset runs convert fewer assets)
			FConvJson A, B;
			if (!FConvJson::LoadFile(Conv, A)) { Diffs[i].Add(TEXT("missing in converted output")); return; }
			if (!FConvJson::LoadFile(Pairs[i].Value, B)) return;
			const FConvJson& Aa = A.Get(TEXT("assets"));
			int32 Missing = 0;
			for (const TPair<FString, FConvJson>& KV : B.Get(TEXT("assets")).O)
			{
				const FConvJson* Av = Aa.Find(KV.Key);
				if (!Av) { ++Missing; continue; }
				CompareJson(*Av, KV.Value, KV.Key, Diffs[i]);
			}
			if (Missing && !Maps.Num()) Diffs[i].Add(FString::Printf(TEXT("%d reference entries missing (%d converted)"), Missing, Aa.O.Num()));
			return;
		}
		if (Rel.StartsWith(TEXT("data/")))
		{
			FConvJson A, B;
			if (!FConvJson::LoadFile(Conv, A)) { Diffs[i].Add(TEXT("missing in converted output")); return; }
			if (!FConvJson::LoadFile(Pairs[i].Value, B)) return;
			A.Remove(TEXT("schema"));
			B.Remove(TEXT("schema"));
			CompareJson(A, B, TEXT("$"), Diffs[i]);
			return;
		}
		CompareFile(Conv, Pairs[i].Value, Rel, Diffs[i]);
	});

	TMap<FString, TPair<int32, int32>> PerKind; // kind -> (compared, mismatched)
	TArray<FString> Report;
	int32 Bad = 0;
	for (int32 i = 0; i < Pairs.Num(); ++i)
	{
		const FString& Rel = Pairs[i].Key;
		int32 Slash;
		FString Kind = Rel.FindChar(TEXT('/'), Slash) ? Rel.Left(Slash) : Rel;
		Kind += TEXT(" .") + FPaths::GetExtension(Rel);
		TPair<int32, int32>& C = PerKind.FindOrAdd(Kind);
		++C.Key;
		if (Diffs[i].Count)
		{
			++C.Value;
			++Bad;
			Report.Add(FString::Printf(TEXT("DIFF %s (%d)"), *Rel, Diffs[i].Count));
			for (const FString& L : Diffs[i].Lines) Report.Add(TEXT("    ") + L);
		}
	}
	TArray<FString> Summary;
	Summary.Add(FString::Printf(TEXT("Compare %s with %s (data: %s): %d files, %d differ, %.1fs"), *Converted, *Reference, *ReferenceData, Pairs.Num(), Bad,
		FPlatformTime::Seconds() - T0));
	TArray<FString> Kinds;
	PerKind.GetKeys(Kinds);
	Kinds.Sort();
	for (const FString& K : Kinds)
	{
		Summary.Add(FString::Printf(TEXT("  %-28s %6d compared, %6d differ"), *K, PerKind[K].Key, PerKind[K].Value));
	}
	for (const FString& L : Summary) UE_LOG(LogOpenUT2k4, Display, TEXT("%s"), *L);
	Summary.Append(Report);
	FFileHelper::SaveStringArrayToFile(Summary, *(Converted / TEXT("compare_report.txt")));
	UE_LOG(LogOpenUT2k4, Display, TEXT("Compare report: %s"), *(Converted / TEXT("compare_report.txt")));
	return Bad;
}

#else
#include "ArenaConverter.h"
int32 FArenaConverter::CompareCaches(const FString&, const FString&, const FString&, const TArray<FString>&) { return -1; }
#endif // ARENA_CONV_BUILD
