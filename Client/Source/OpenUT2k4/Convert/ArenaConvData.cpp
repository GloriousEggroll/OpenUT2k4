#include "ArenaConvData.h"
#include "ArenaConvConfig.h"
#if ARENA_CONV_BUILD
#include "ArenaConvExporter.h"
#include "ArenaConvImage.h"
#include "Async/ParallelFor.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

namespace ArenaConvDataLocal
{
	int32 Cmp(const FString& A, const FString& B) { return FCString::Strcmp(*A, *B); }

	TArray<FString> SortedFiles(const FString& Dir, const TCHAR* Pattern)
	{
		TArray<FString> F;
		IFileManager::Get().FindFiles(F, *(Dir / Pattern), true, false);
		F.Sort([](const FString& A, const FString& B) { return Cmp(A, B) < 0; });
		return F;
	}

	TArray<FString> SortedDirs(const FString& Dir)
	{
		TArray<FString> F;
		IFileManager::Get().FindFiles(F, *(Dir / TEXT("*")), false, true);
		F.Sort([](const FString& A, const FString& B) { return Cmp(A, B) < 0; });
		return F;
	}

	int64 MTime(const FString& Path)
	{
		const FDateTime T = IFileManager::Get().GetTimeStamp(*Path);
		return T == FDateTime::MinValue() ? -1 : (T - FDateTime(1970, 1, 1)).GetTicks() / ETimespan::TicksPerSecond;
	}

	/** Every <kind>/<Pkg>/<File>.json of the cache: (pkg folder, json path). */
	TArray<TPair<FString, FString>> CacheEntries(const FString& CacheDir, const TCHAR* Kind)
	{
		TArray<TPair<FString, FString>> Out;
		const FString Root = CacheDir / Kind;
		for (const FString& Pkg : SortedDirs(Root))
		{
			for (const FString& F : SortedFiles(Root / Pkg, TEXT("*.json")))
			{
				Out.Emplace(Pkg, Root / Pkg / F);
			}
		}
		return Out;
	}

	/** matresolve.py Graph: case-insensitive lookups of material / texture json by object path. */
	struct FGraph
	{
		TMap<FString, FString> Materials, Textures;
		TMap<FString, TSharedPtr<FConvJson>> Docs;

		explicit FGraph(const FString& CacheDir)
		{
			for (const TCHAR* Kind : { TEXT("materials"), TEXT("textures") })
			{
				TMap<FString, FString>& Map = FCString::Strcmp(Kind, TEXT("materials")) == 0 ? Materials : Textures;
				const FString Root = CacheDir / Kind;
				TArray<FString> Pkgs;
				IFileManager::Get().FindFiles(Pkgs, *(Root / TEXT("*")), false, true);
				for (const FString& Pkg : Pkgs)
				{
					TArray<FString> Files;
					IFileManager::Get().FindFiles(Files, *(Root / Pkg / TEXT("*.json")), true, false);
					for (const FString& F : Files)
					{
						Map.Add((Pkg + TEXT(".") + FPaths::GetBaseFilename(F)).ToLower(), Root / Pkg / F);
					}
				}
			}
		}

		static FString Key(const FString& Path)
		{
			int32 Dot;
			const FString Pkg = Path.FindChar(TEXT('.'), Dot) ? Path.Left(Dot) : Path;
			const FString Rest = Path.FindChar(TEXT('.'), Dot) ? Path.Mid(Dot + 1) : FString();
			return (FConvExporter::Safe(Pkg) + TEXT(".") + FConvExporter::Safe(Rest)).ToLower();
		}

		const FConvJson* Doc(const FString& File)
		{
			if (TSharedPtr<FConvJson>* Hit = Docs.Find(File))
			{
				return Hit->Get();
			}
			TSharedPtr<FConvJson> D = MakeShared<FConvJson>();
			if (!FConvJson::LoadFile(File, *D))
			{
				D.Reset();
			}
			Docs.Add(File, D);
			return D.Get();
		}

		/** (doc, json file, is texture) or null doc. */
		const FConvJson* Node(const FConvJson& Ref, bool bTexture, FString& OutFile, bool& bOutIsTexture)
		{
			if (Ref.Type != FConvJson::EType::String || Ref.S.IsEmpty() || Ref.S == TEXT("None"))
			{
				return nullptr;
			}
			const FString K = Key(Ref.S);
			const FString* Mf = bTexture ? nullptr : Materials.Find(K);
			if (Mf)
			{
				OutFile = *Mf;
				bOutIsTexture = false;
				return Doc(*Mf);
			}
			if (const FString* Tf = Textures.Find(K))
			{
				OutFile = *Tf;
				bOutIsTexture = true;
				return Doc(*Tf);
			}
			return nullptr;
		}
	};

	// ---- ImportUT2004.py marker tables ----
	FString MapWeapon(const FString& Cls)
	{
		static const TMap<FString, FString> W = {
			{ TEXT("shockrifle"), TEXT("PulseRifle") }, { TEXT("linkgun"), TEXT("ArcLinker") }, { TEXT("sniperrifle"), TEXT("RailRifle") },
			{ TEXT("classicsniperrifle"), TEXT("MarksmanRifle") }, { TEXT("rocketlauncher"), TEXT("RocketLauncher") }, { TEXT("flakcannon"), TEXT("ShardCannon") },
			{ TEXT("minigun"), TEXT("RotaryGun") }, { TEXT("biorifle"), TEXT("GooLauncher") }, { TEXT("assaultrifle"), TEXT("Carbine") },
			{ TEXT("redeemer"), TEXT("WarheadLauncher") }, { TEXT("painter"), TEXT("OrbitalDesignator") }, { TEXT("onspainter"), TEXT("OrbitalDesignator") },
			{ TEXT("onsavril"), TEXT("SeekerLauncher") }, { TEXT("onsgrenadelauncher"), TEXT("StickyLauncher") }, { TEXT("onsgrenade"), TEXT("StickyLauncher") },
			{ TEXT("onsminelayer"), TEXT("CrawlerLayer") }, { TEXT("translauncher"), TEXT("Translocator") }, { TEXT("transpickup"), TEXT("Translocator") } };
		if (Cls.IsEmpty())
		{
			return FString();
		}
		const FString C = Cls.ToLower();
		if (const FString* Hit = W.Find(C))
		{
			return *Hit;
		}
		if (C.StartsWith(TEXT("ons")) || C.Contains(TEXT("painter")) || C.Contains(TEXT("redeemer")))
		{
			return TEXT("RocketLauncher");
		}
		return FString();
	}

	FString MapAmmo(const FString& Cls)
	{
		static const TMap<FString, FString> A = {
			{ TEXT("shock"), TEXT("shockrifle") }, { TEXT("link"), TEXT("linkgun") }, { TEXT("sniper"), TEXT("sniperrifle") },
			{ TEXT("classicsniper"), TEXT("classicsniperrifle") }, { TEXT("rocket"), TEXT("rocketlauncher") }, { TEXT("flak"), TEXT("flakcannon") },
			{ TEXT("minigun"), TEXT("minigun") }, { TEXT("bio"), TEXT("biorifle") }, { TEXT("assault"), TEXT("assaultrifle") },
			{ TEXT("redeemer"), TEXT("redeemer") }, { TEXT("onsavril"), TEXT("onsavril") }, { TEXT("onsgrenade"), TEXT("onsgrenadelauncher") },
			{ TEXT("onsmine"), TEXT("onsminelayer") } };
		const FString C = Cls.ToLower();
		if (!C.EndsWith(TEXT("ammopickup")))
		{
			return FString();
		}
		const FString Prefix = C.LeftChop(10);
		if (const FString* Hit = A.Find(Prefix))
		{
			return MapWeapon(*Hit);
		}
		if (Prefix.StartsWith(TEXT("ons")))
		{
			return TEXT("RocketLauncher");
		}
		return FString();
	}

	/** (EArenaPickupType name, weapon) or empty. */
	TPair<FString, FString> MapPickup(const FString& Cls)
	{
		static const TMap<FString, FString> P = {
			{ TEXT("minihealthpack"), TEXT("HealthVial") }, { TEXT("healthpack"), TEXT("HealthPack") }, { TEXT("healthcharger"), TEXT("HealthPack") },
			{ TEXT("superhealthpack"), TEXT("SuperHealth") }, { TEXT("superhealthcharger"), TEXT("SuperHealth") }, { TEXT("shieldpack"), TEXT("ShieldSmall") },
			{ TEXT("shieldcharger"), TEXT("ShieldSmall") }, { TEXT("supershieldpack"), TEXT("ShieldLarge") }, { TEXT("supershieldcharger"), TEXT("ShieldLarge") },
			{ TEXT("udamagepack"), TEXT("DoubleDamage") }, { TEXT("udamagecharger"), TEXT("DoubleDamage") }, { TEXT("adrenalinepickup"), TEXT("Adrenaline") } };
		if (Cls.IsEmpty())
		{
			return {};
		}
		const FString C = Cls.ToLower();
		if (const FString* Hit = P.Find(C))
		{
			return { *Hit, FString() };
		}
		const FString W = MapAmmo(Cls);
		if (!W.IsEmpty())
		{
			return { TEXT("Ammo"), W };
		}
		if (C.EndsWith(TEXT("pickup")))
		{
			const FString Wp = MapWeapon(Cls.LeftChop(6));
			if (!Wp.IsEmpty())
			{
				return { TEXT("Weapon"), Wp };
			}
		}
		return {};
	}

	FConvJson R1(const FConvJson& V)
	{
		FConvJson A = FConvJson::Array();
		for (const FConvJson& X : V.A)
		{
			A.Add(FConvJson::RoundTo(X.AsNumber(), 1));
		}
		return A;
	}

	FConvJson R1d(double X, double Y, double Z)
	{
		FConvJson A = FConvJson::Array();
		A.Add(FConvJson::RoundTo(X, 1));
		A.Add(FConvJson::RoundTo(Y, 1));
		A.Add(FConvJson::RoundTo(Z, 1));
		return A;
	}

	FString LooseKey(const FString& S)
	{
		FString Out;
		for (TCHAR C : S.ToLower())
		{
			if ((C >= 'a' && C <= 'z') || (C >= '0' && C <= '9'))
			{
				Out.AppendChar(C);
			}
		}
		return Out;
	}

	/** Python str() of a json value (for tag comparisons). */
	FString PyStr(const FConvJson& V)
	{
		if (V.Type == FConvJson::EType::String) return V.S;
		if (V.IsNull()) return TEXT("None");
		TArray<ANSICHAR> B;
		V.Write(B, -1);
		B.Add(0);
		return FString(ANSI_TO_TCHAR(B.GetData()));
	}
}

using namespace ArenaConvDataLocal;

namespace ArenaConvData
{
	FString AssetName(const FString& Name)
	{
		FString Out = Name;
		for (TCHAR& C : Out.GetCharArray())
		{
			if (C == 0) break;
			if (!((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '_' || C == '-'))
			{
				C = '_';
			}
		}
		return Out;
	}

	FString ImportedPath(const TCHAR* Kind, const FString& ObjectPath)
	{
		int32 Dot;
		FString Pkg = ObjectPath, Rest = ObjectPath;
		if (ObjectPath.FindChar(TEXT('.'), Dot))
		{
			Pkg = ObjectPath.Left(Dot);
			Rest = ObjectPath.Mid(Dot + 1);
		}
		return FString::Printf(TEXT("/Game/Imported/%s/%s/%s"), Kind, *AssetName(Pkg), *AssetName(Rest));
	}

	void SortKeys(FConvJson& J)
	{
		if (J.Type == FConvJson::EType::Object)
		{
			J.O.Sort([](const TPair<FString, FConvJson>& A, const TPair<FString, FConvJson>& B) { return Cmp(A.Key, B.Key) < 0; });
			for (TPair<FString, FConvJson>& KV : J.O) SortKeys(KV.Value);
		}
		else if (J.Type == FConvJson::EType::Array)
		{
			for (FConvJson& V : J.A) SortKeys(V);
		}
	}

	// -----------------------------------------------------------------------------------------
	// bake

	void BakeCombiners(const FString& CacheDir, TMap<FString, FConvJson>& NewStats, FResult& Result, const TSet<FString>* OnlyPackages)
	{
		FGraph G(CacheDir);
		int32 N = 0;
		for (const TPair<FString, FString>& E : CacheEntries(CacheDir, TEXT("materials")))
		{
			if (OnlyPackages && !OnlyPackages->Contains(E.Key.ToLower()))
			{
				continue;
			}
			FConvJson Doc;
			if (!FConvJson::LoadFile(E.Value, Doc) || Doc.GetString(TEXT("class")) != TEXT("Combiner"))
			{
				continue;
			}
			const FConvJson& Props = Doc.Get(TEXT("properties"));
			const FConvJson& OpJ = Props.Get(TEXT("CombineOperation"));
			const int32 Op = OpJ.Truthy() ? (int32)OpJ.AsNumber() : 0;
			if (Op != 3 && Op != 6)
			{
				continue;
			}
			const FString DocPath = Doc.GetString(TEXT("path"));
			auto TexNode = [&](const FConvJson& Ref, FString& File, bool& bTex) -> const FConvJson*
			{
				const FConvJson* Nd = G.Node(Ref, false, File, bTex);
				if (Nd && !bTex && Nd->GetString(TEXT("path")).ToLower() == DocPath.ToLower())
				{
					Nd = G.Node(Ref, true, File, bTex);
				}
				return Nd;
			};
			FString F1, F2;
			bool bT1 = false, bT2 = false;
			const FConvJson* T1 = TexNode(Props.Get(TEXT("Material1")), F1, bT1);
			if (!T1) continue;
			const FConvJson T1Copy = *T1;
			const FConvJson* T2 = TexNode(Props.Get(TEXT("Material2")), F2, bT2);
			if (!T2 || !bT1 || !bT2) continue;
			const FConvJson T2Copy = *T2;
			const FString Png1 = T1Copy.GetString(TEXT("png")).IsEmpty() ? FString() : FPaths::GetPath(F1) / T1Copy.GetString(TEXT("png"));
			const FString Png2 = T2Copy.GetString(TEXT("png")).IsEmpty() ? FString() : FPaths::GetPath(F2) / T2Copy.GetString(TEXT("png"));
			if (Png1.IsEmpty() || Png2.IsEmpty() || !IFileManager::Get().FileExists(*Png1) || !IFileManager::Get().FileExists(*Png2))
			{
				continue;
			}
			const FString Path = DocPath + TEXT("_Baked");
			int32 Dot;
			if (!Path.FindChar(TEXT('.'), Dot)) continue;
			const FString Dir = CacheDir / TEXT("textures") / FConvExporter::Safe(Path.Left(Dot));
			const FString Png = Dir / (FConvExporter::Safe(Path.Mid(Dot + 1)) + TEXT(".png"));
			const FString Js = FPaths::ChangeExtension(Png, TEXT(".json"));
			const int64 SrcM = FMath::Max(MTime(Png1), MTime(Png2));
			if (IFileManager::Get().FileExists(*Png) && IFileManager::Get().FileExists(*Js) && MTime(Png) >= SrcM)
			{
				continue;
			}
			FConvImage A, B;
			bool bA = false, bB = false;
			if (!ArenaConvImage::LoadPng(Png1, A, bA) || !ArenaConvImage::LoadPng(Png2, B, bB))
			{
				Result.Errors.Add(FString::Printf(TEXT("bake %s: cannot read the source PNGs"), *Path));
				continue;
			}
			A = A.ToRGBA8();
			B = B.ToRGBA8();
			const int32 W = FMath::Max(A.W, B.W), H = FMath::Max(A.H, B.H);
			if (A.W != W || A.H != H) A = A.ResizeBilinear(W, H);
			if (B.W != W || B.H != H) B = B.ResizeBilinear(W, H);
			FConvImage OutImg = B;
			for (int64 i = 0; i < int64(W) * H; ++i)
			{
				for (int32 c = 0; c < 3; ++c)
				{
					OutImg.Pixels[i * 4 + c] = (uint8)FMath::Min(255, int32(A.Pixels[i * 4 + c]) + int32(B.Pixels[i * 4 + c]));
				}
			}
			const bool bHasAlpha = T2Copy.Get(TEXT("has_alpha")).Truthy();
			if (!bHasAlpha)
			{
				OutImg.StripAlpha();
			}
			TArray64<uint8> Bytes;
			if (!ArenaConvImage::EncodePng(OutImg, Bytes, 6) || !ConvWriteFile(Png, Bytes.GetData(), Bytes.Num()))
			{
				Result.Errors.Add(FString::Printf(TEXT("bake %s: cannot write"), *Path));
				continue;
			}
			FConvJson Meta = FConvJson::Object();
			Meta.Set(TEXT("type"), TEXT("Texture"));
			Meta.Set(TEXT("class"), TEXT("Texture"));
			Meta.Set(TEXT("path"), Path);
			const FConvJson& Sc = T2Copy.Get(TEXT("scale"));
			Meta.Set(TEXT("scale"), Sc.IsNull() ? FConvJson(2.0) : Sc);
			Meta.Set(TEXT("png"), FPaths::GetCleanFilename(Png));
			Meta.Set(TEXT("format"), TEXT("RGBA8"));
			Meta.Set(TEXT("width"), W);
			Meta.Set(TEXT("height"), H);
			Meta.Set(TEXT("mip_count"), 0);
			Meta.Set(TEXT("bMasked"), T2Copy.Get(TEXT("bMasked")).Truthy());
			Meta.Set(TEXT("bAlphaTexture"), false);
			Meta.Set(TEXT("bTwoSided"), T2Copy.Get(TEXT("bTwoSided")).Truthy());
			Meta.Set(TEXT("has_alpha"), bHasAlpha);
			Meta.Set(TEXT("png_bit_depth"), 8);
			Meta.Set(TEXT("procedural_fallback"), FConvJson());
			FConvJson Baked = FConvJson::Object();
			Baked.Set(TEXT("combiner"), DocPath);
			Baked.Set(TEXT("operation"), Op == 3 ? TEXT("add") : TEXT("add_mask_mod"));
			Baked.Set(TEXT("material1"), T1Copy.GetString(TEXT("path")));
			Baked.Set(TEXT("material2"), T2Copy.GetString(TEXT("path")));
			Baked.Set(TEXT("note"), TEXT("Material1 + Material2 (saturating add); Mask ignored"));
			Meta.Set(TEXT("baked"), Baked);
			FConvJson Hints = T2Copy.Get(TEXT("material_hints")).Type == FConvJson::EType::Object ? T2Copy.Get(TEXT("material_hints")) : FConvJson::Object();
			Hints.Set(TEXT("blend_mode"), TEXT("opaque"));
			Meta.Set(TEXT("material_hints"), Hints);
			Meta.SaveToFile(Js, 1);
			NewStats.Remove(Path);
			++N;
		}
		Result.Counts.Add(TEXT("baked_textures"), N);
	}

	// -----------------------------------------------------------------------------------------
	// texture_stats.json

	void WriteTextureStats(const FString& CacheDir, const TMap<FString, FConvJson>& NewStats, FResult& Result, const TAtomic<bool>* Cancel)
	{
		const FString StatsFile = CacheDir / TEXT("texture_stats.json");
		FConvJson Old;
		TMap<FString, const FConvJson*> OldByKey;
		if (FConvJson::LoadFile(StatsFile, Old))
		{
			for (const TPair<FString, FConvJson>& KV : Old.Get(TEXT("textures")).O)
			{
				OldByKey.Add(KV.Key, &KV.Value);
			}
		}
		struct FTodo { FString Key, Png; int64 MTime; };
		TArray<FTodo> Todo;
		FConvJson Out = FConvJson::Object();
		FConvJson& Tex = Out.Set(TEXT("version"), 1), &Textures = Out.Set(TEXT("textures"), FConvJson::Object());
		(void)Tex;
		for (const TPair<FString, FString>& E : CacheEntries(CacheDir, TEXT("textures")))
		{
			FConvJson Doc;
			if (!FConvJson::LoadFile(E.Value, Doc)) continue;
			const FString PngName = Doc.GetString(TEXT("png"));
			if (PngName.IsEmpty()) continue;
			const FString Png = FPaths::GetPath(E.Value) / PngName;
			const int64 Mt = MTime(Png);
			if (Mt < 0) continue;
			FString Key = Doc.GetString(TEXT("path"));
			if (Key.IsEmpty()) Key = E.Key + TEXT(".") + FPaths::GetBaseFilename(E.Value);
			if (const FConvJson* N = NewStats.Find(Key))
			{
				FConvJson S = *N;
				S.Set(TEXT("mtime"), Mt);
				Textures.O.Emplace(Key, MoveTemp(S));
				continue;
			}
			const FConvJson* const* O = OldByKey.Find(Key);
			if (O && (int64)(*O)->GetNumber(TEXT("mtime"), -1) == Mt && (*O)->GetNumber(TEXT("v"), 0) == 1)
			{
				Textures.O.Emplace(Key, **O);
				continue;
			}
			Todo.Add({ Key, Png, Mt });
		}
		TArray<FConvJson> Computed;
		Computed.SetNum(Todo.Num());
		ParallelFor(Todo.Num(), [&](int32 i)
		{
			if (Cancel && Cancel->Load()) return;
			FConvImage Img;
			bool bAlpha = false;
			FConvJson S;
			if (ArenaConvImage::LoadPng(Todo[i].Png, Img, bAlpha))
			{
				S = ArenaConvImage::Stats(Img, bAlpha);
			}
			else
			{
				S = FConvJson::Object();
				S.Set(TEXT("error"), TEXT("cannot read PNG"));
			}
			S.Set(TEXT("mtime"), Todo[i].MTime);
			S.Set(TEXT("v"), 1);
			Computed[i] = MoveTemp(S);
		});
		for (int32 i = 0; i < Todo.Num(); ++i)
		{
			Textures.O.Emplace(Todo[i].Key, MoveTemp(Computed[i]));
		}
		Out.SaveToFile(StatsFile, -1);
		Result.Counts.Add(TEXT("texture_stats"), Textures.O.Num());
		Result.Counts.Add(TEXT("texture_stats_sampled"), Todo.Num());
	}

	void MergeTextureStats(const FString& CacheDir, const TMap<FString, FConvJson>& NewStats, const TSet<FString>& RemovedPackages, FResult& Result)
	{
		const FString StatsFile = CacheDir / TEXT("texture_stats.json");
		FConvJson Doc;
		if (!FConvJson::LoadFile(StatsFile, Doc) || Doc.Get(TEXT("textures")).Type != FConvJson::EType::Object)
		{
			Doc = FConvJson::Object();
			Doc.Set(TEXT("version"), 1);
			Doc.Set(TEXT("textures"), FConvJson::Object());
		}
		FConvJson& Textures = *Doc.Find(TEXT("textures"));
		int32 Removed = 0;
		if (RemovedPackages.Num())
		{
			Removed = Textures.O.RemoveAll([&RemovedPackages](const TPair<FString, FConvJson>& KV)
			{
				int32 Dot;
				return RemovedPackages.Contains((KV.Key.FindChar(TEXT('.'), Dot) ? KV.Key.Left(Dot) : KV.Key).ToLower());
			});
		}
		for (const TPair<FString, FConvJson>& KV : NewStats)
		{
			int32 Dot;
			if (!KV.Key.FindChar(TEXT('.'), Dot))
			{
				continue;
			}
			const FString Png = CacheDir / TEXT("textures") / FConvExporter::Safe(KV.Key.Left(Dot)) / (FConvExporter::Safe(KV.Key.Mid(Dot + 1)) + TEXT(".png"));
			FConvJson S = KV.Value;
			S.Set(TEXT("mtime"), MTime(Png));
			Textures.Set(KV.Key, MoveTemp(S));
		}
		Doc.SaveToFile(StatsFile, -1);
		Result.Counts.Add(TEXT("texture_stats_merged"), NewStats.Num());
		Result.Counts.Add(TEXT("texture_stats_removed"), Removed);
	}

	// -----------------------------------------------------------------------------------------
	// markers (ImportUT2004.py slim_markers)

	FConvJson SlimMarkers(const FConvJson& Doc, const FConvJson& MusicAssets)
	{
		const FConvJson& M = Doc.Get(TEXT("markers"));
		const FConvJson& Summary = Doc.Get(TEXT("summary"));
		const FConvJson& LevelInfo = Doc.Get(TEXT("level_info"));
		const FString Name = Doc.GetString(TEXT("map"));
		FString Prefix = Doc.GetString(TEXT("game_type_prefix"));
		if (Prefix.IsEmpty())
		{
			int32 Dash;
			Prefix = Name.FindChar(TEXT('-'), Dash) ? Name.Left(Dash) : Name;
		}
		Prefix = Prefix.ToUpper();
		auto List = [&M](const TCHAR* K) -> const TArray<FConvJson>& { return M.Get(K).A; };
		auto Enabled = [](const FConvJson& S) { const FConvJson* E = S.Find(TEXT("enabled")); return !E || E->Truthy(); };

		// starts
		TSet<int64> Teams;
		for (const FConvJson& S : List(TEXT("player_starts")))
		{
			if (Enabled(S)) Teams.Add(S.Has(TEXT("team")) ? (int64)S.Get(TEXT("team")).AsNumber() : 0);
		}
		const bool bTeamStarts = Teams.Contains(0) && Teams.Contains(1);
		FConvJson Starts = FConvJson::Array();
		for (const FConvJson& S : List(TEXT("player_starts")))
		{
			if (!Enabled(S)) continue;
			FConvJson E = FConvJson::Object();
			E.Set(TEXT("location"), R1(S.Get(TEXT("location"))));
			E.Set(TEXT("yaw"), FConvJson::RoundTo(S.Get(TEXT("rotation")).Get(TEXT("yaw")).AsNumber(), 2));
			E.Set(TEXT("team"), bTeamStarts ? (int32)S.Get(TEXT("team")).AsNumber() : -1);
			Starts.Add(E);
		}

		FConvJson Pickups = FConvJson::Array();
		TMap<FString, int32> Skipped;
		TArray<FString> SkippedOrder;
		auto Skip = [&](const FString& What)
		{
			if (!Skipped.Contains(What)) SkippedOrder.Add(What);
			Skipped.FindOrAdd(What)++;
		};
		auto AddPickup = [&](const FString& Type, const FString& Weapon, const FConvJson& Loc, const FString& Source)
		{
			FConvJson P = FConvJson::Object();
			P.Set(TEXT("type"), Type);
			P.Set(TEXT("weapon"), FConvJson::StrOrNull(Weapon));
			P.Set(TEXT("location"), Loc);
			P.Set(TEXT("source"), Source);
			Pickups.Add(P);
		};
		for (const FConvJson& Wb : List(TEXT("weapon_bases")))
		{
			const FString Cls = Wb.Get(TEXT("weapon_class")).GetString(TEXT("class"));
			const FString W = MapWeapon(Cls);
			if (!W.IsEmpty()) AddPickup(TEXT("Weapon"), W, R1(Wb.Get(TEXT("location"))), Wb.GetString(TEXT("class")));
			else Skip(Cls.IsEmpty() ? Wb.GetString(TEXT("class")) : Cls);
		}
		for (const FConvJson& Lk : List(TEXT("weapon_lockers")))
		{
			TArray<FString> Ws;
			for (const FConvJson& X : Lk.Get(TEXT("weapons")).A)
			{
				const FString W = MapWeapon(X.Get(TEXT("weapon_class")).GetString(TEXT("class")));
				if (!W.IsEmpty()) Ws.Add(W);
			}
			const FConvJson& Loc = Lk.Get(TEXT("location"));
			const double Yaw = FMath::DegreesToRadians(Lk.Get(TEXT("rotation")).Get(TEXT("yaw")).AsNumber());
			const int32 N = Ws.Num();
			for (int32 i = 0; i < N; ++i)
			{
				const double Off = (i - (N - 1) * 0.5) * 70.0;
				AddPickup(TEXT("Weapon"), Ws[i], R1d(Loc.A[0].AsNumber() - FMath::Sin(Yaw) * Off, Loc.A[1].AsNumber() + FMath::Cos(Yaw) * Off, Loc.A[2].AsNumber()),
					Lk.GetString(TEXT("class")));
			}
		}
		for (const FConvJson& Pb : List(TEXT("pickup_bases")))
		{
			TArray<FString> Classes;
			if (Pb.Get(TEXT("powerup")).Truthy()) Classes.Add(Pb.Get(TEXT("powerup")).GetString(TEXT("class")));
			for (const FConvJson& C : Pb.Get(TEXT("pickup_classes")).A)
			{
				if (C.Truthy()) Classes.Add(C.GetString(TEXT("class")));
			}
			Classes.Add(Pb.GetString(TEXT("class")));
			TPair<FString, FString> Hit;
			for (const FString& C : Classes)
			{
				Hit = MapPickup(C);
				if (!Hit.Key.IsEmpty()) break;
			}
			if (!Hit.Key.IsEmpty()) AddPickup(Hit.Key, Hit.Value, R1(Pb.Get(TEXT("location"))), Pb.GetString(TEXT("class")));
			else Skip(Pb.GetString(TEXT("class")));
		}
		for (const FConvJson& P : List(TEXT("pickups")))
		{
			const TPair<FString, FString> Hit = MapPickup(P.GetString(TEXT("class")));
			if (!Hit.Key.IsEmpty()) AddPickup(Hit.Key, Hit.Value, R1(P.Get(TEXT("location"))), P.GetString(TEXT("class")));
			else Skip(P.GetString(TEXT("class")));
		}

		FConvJson Pads = FConvJson::Array();
		auto AddPad = [&](const FConvJson& Loc, const FConvJson& Target, const FString& Source)
		{
			FConvJson P = FConvJson::Object();
			P.Set(TEXT("location"), Loc);
			P.Set(TEXT("target"), Target);
			P.Set(TEXT("source"), Source);
			Pads.Add(P);
		};
		for (const FConvJson& Jp : List(TEXT("jump_pads")))
		{
			if (Jp.Get(TEXT("jump_target_location")).Truthy())
			{
				AddPad(R1(Jp.Get(TEXT("location"))), R1(Jp.Get(TEXT("jump_target_location"))), Jp.GetString(TEXT("class")));
			}
		}
		// lifts become jump pads to the upper exit
		const TArray<FConvJson>& Lifts = List(TEXT("lifts"));
		auto Xy2 = [](const FConvJson& A, const FConvJson& B)
		{
			return FMath::Square(A.A[0].AsNumber() - B.A[0].AsNumber()) + FMath::Square(A.A[1].AsNumber() - B.A[1].AsNumber());
		};
		for (const FConvJson& Mv : List(TEXT("movers")))
		{
			const TArray<FConvJson>& Keys = Mv.Get(TEXT("key_pos")).A;
			if (!Keys.Num()) continue;
			const FConvJson* Top = &Keys[0];
			for (const FConvJson& K : Keys) if (K.A[2].AsNumber() > Top->A[2].AsNumber()) Top = &K;
			if (Top->A[2].AsNumber() < 120.0) continue;
			const FConvJson* Actor = nullptr;
			for (const FConvJson& A : Doc.Get(TEXT("actors")).A)
			{
				if (A.Get(TEXT("index")).AsNumber() == Mv.Get(TEXT("actor_index")).AsNumber()) { Actor = &A; break; }
			}
			const FString Tag = Actor ? PyStr(Actor->Get(TEXT("tag"))) : FString(TEXT("None"));
			TArray<const FConvJson*> Centers;
			for (const FConvJson& L : Lifts)
			{
				if (L.GetString(TEXT("class")) == TEXT("LiftCenter") && PyStr(L.Get(TEXT("lift_tag"))) == Tag) Centers.Add(&L);
			}
			if (!Centers.Num()) continue;
			const FConvJson& Base = Mv.Get(TEXT("location"));
			const FConvJson* Center = Centers[0];
			for (const FConvJson* C : Centers) if (Xy2(C->Get(TEXT("location")), Base) < Xy2(Center->Get(TEXT("location")), Base)) Center = C;
			const double TopZ = Base.A[2].AsNumber() + Top->A[2].AsNumber();
			TArray<const FConvJson*> Exits;
			for (const FConvJson& L : Lifts)
			{
				if (L.GetString(TEXT("class")) == TEXT("LiftExit") && PyStr(L.Get(TEXT("lift_tag"))) == Tag
					&& L.Get(TEXT("location")).A[2].AsNumber() > Base.A[2].AsNumber() + Top->A[2].AsNumber() * 0.5)
				{
					Exits.Add(&L);
				}
			}
			if (!Exits.Num())
			{
				for (const FConvJson& L : Lifts)
				{
					if (L.GetString(TEXT("class")) == TEXT("LiftExit") && FMath::Abs(L.Get(TEXT("location")).A[2].AsNumber() - TopZ) < 400.0) Exits.Add(&L);
				}
			}
			if (!Exits.Num()) continue;
			const FConvJson& C = Center->Get(TEXT("location"));
			const FConvJson* Exit = Exits[0];
			for (const FConvJson* E : Exits) if (Xy2(E->Get(TEXT("location")), C) < Xy2(Exit->Get(TEXT("location")), C)) Exit = E;
			AddPad(R1(C), R1(Exit->Get(TEXT("location"))), TEXT("Lift:") + Mv.GetString(TEXT("actor")));
		}

		FConvJson Flags = FConvJson::Array();
		for (const FConvJson& Fb : List(TEXT("flag_bases")))
		{
			const FConvJson& T = Fb.Get(TEXT("team"));
			if (T.IsNumber() && (T.AsNumber() == 0 || T.AsNumber() == 1))
			{
				FConvJson F = FConvJson::Object();
				F.Set(TEXT("team"), (int32)T.AsNumber());
				F.Set(TEXT("location"), R1(Fb.Get(TEXT("location"))));
				Flags.Add(F);
			}
		}

		// playable bounds
		TArray<const FConvJson*> Pts;
		for (const FConvJson& S : Starts.A) Pts.Add(&S.Get(TEXT("location")));
		for (const FConvJson& P : Pickups.A) Pts.Add(&P.Get(TEXT("location")));
		for (const TCHAR* K : { TEXT("path_nodes"), TEXT("inventory_spots"), TEXT("jump_spots"), TEXT("other_nav_points"), TEXT("lifts") })
		{
			for (const FConvJson& X : List(K)) Pts.Add(&X.Get(TEXT("location")));
		}
		for (const FConvJson& P : Pads.A) { Pts.Add(&P.Get(TEXT("location"))); Pts.Add(&P.Get(TEXT("target"))); }
		for (const FConvJson& F : Flags.A) Pts.Add(&F.Get(TEXT("location")));
		FConvJson Bounds;
		Pts.RemoveAll([](const FConvJson* P) { return P->A.Num() < 3; });
		if (Pts.Num())
		{
			double Mn[3], Mx[3];
			for (int32 c = 0; c < 3; ++c)
			{
				Mn[c] = Mx[c] = Pts[0]->A[c].AsNumber();
				for (const FConvJson* P : Pts)
				{
					Mn[c] = FMath::Min(Mn[c], P->A[c].AsNumber());
					Mx[c] = FMath::Max(Mx[c], P->A[c].AsNumber());
				}
			}
			Bounds = FConvJson::Object();
			Bounds.Set(TEXT("min"), R1d(Mn[0], Mn[1], Mn[2]));
			Bounds.Set(TEXT("max"), R1d(Mx[0], Mx[1], Mx[2]));
		}

		// music
		FConvJson Music, MusicAsset;
		{
			FString Song = LevelInfo.GetString(TEXT("song")).TrimStartAndEnd();
			if (Song.EndsWith(TEXT(".ogg"), ESearchCase::IgnoreCase)) Song.LeftChopInline(4);
			if (!Song.IsEmpty() && Song.ToLower() != TEXT("none"))
			{
				Music = Song;
				const FConvJson* Hit = nullptr;
				for (const TPair<FString, FConvJson>& KV : MusicAssets.O) if (KV.Key.ToLower() == Song.ToLower()) { Hit = &KV.Value; break; }
				if (!Hit)
				{
					for (const TPair<FString, FConvJson>& KV : MusicAssets.O) if (LooseKey(KV.Key) == LooseKey(Song)) { Hit = &KV.Value; break; }
				}
				if (Hit)
				{
					const FConvJson& Imp = Hit->Get(TEXT("imported"));
					MusicAsset = Imp.Truthy() ? Imp : Hit->Get(TEXT("asset"));
				}
			}
		}

		const FConvJson& KillZ = Doc.Get(TEXT("kill_z"));
		FConvJson Ideal = FConvJson::Array();
		const FConvJson& Ip = Summary.Get(TEXT("ideal_player_count"));
		for (const FConvJson& X : Ip.A) if (X.IsNumber()) Ideal.Add(X);
		FString Title = Summary.GetString(TEXT("title"));
		if (Title.IsEmpty()) Title = LevelInfo.GetString(TEXT("title"));
		if (Title.IsEmpty()) Title = Name;
		FString Author = Summary.GetString(TEXT("author"));
		if (Author.IsEmpty()) Author = LevelInfo.GetString(TEXT("author"));

		FConvJson Out = FConvJson::Object();
		Out.Set(TEXT("schema"), TEXT("openut2k4-imported-map"));
		Out.Set(TEXT("schema_version"), 1);
		Out.Set(TEXT("map"), Name);
		Out.Set(TEXT("level"), FString::Printf(TEXT("/Game/Imported/Maps/%s"), *Name));
		Out.Set(TEXT("title"), Title);
		Out.Set(TEXT("author"), Author);
		Out.Set(TEXT("description"), Summary.GetString(TEXT("description")));
		Out.Set(TEXT("game_type_prefix"), Prefix);
		Out.Set(TEXT("recommended_players"), Ideal);
		Out.Set(TEXT("kill_z"), KillZ.Type == FConvJson::EType::Object ? KillZ.Get(TEXT("z")) : FConvJson());
		Out.Set(TEXT("play_bounds"), Bounds);
		Out.Set(TEXT("player_starts"), Starts);
		Out.Set(TEXT("pickups"), Pickups);
		Out.Set(TEXT("jump_pads"), Pads);
		Out.Set(TEXT("flag_bases"), Flags);
		Out.Set(TEXT("music"), Music);
		Out.Set(TEXT("music_asset"), MusicAsset);
		FConvJson Sk = FConvJson::Object();
		for (const FString& K : SkippedOrder) Sk.Set(K, Skipped[K]);
		Out.Set(TEXT("skipped"), Sk);
		Out.Set(TEXT("note"), TEXT("Local placeholder data converted from the user's own UT2004 install. Not for distribution."));
		Out.Set(TEXT("raw_markers_schema_version"), Doc.Get(TEXT("schema_version")));
		Out.Set(TEXT("raw_markers"), M);
		return Out;
	}

	// -----------------------------------------------------------------------------------------
	// asset tables + markers

	void WriteGameData(const FString& CacheDir, FResult& Result, const TArray<FString>* OnlyMaps)
	{
		const FString DataDir = CacheDir / TEXT("data");
		const FString AssetsDir = DataDir / TEXT("Assets");
		auto Manifest = [&](const TCHAR* Kind, FConvJson Assets)
		{
			FConvJson Doc = FConvJson::Object();
			Doc.Set(TEXT("schema"), TEXT("openut2k4-imported-assets"));
			Doc.Set(TEXT("kind"), Kind);
			Result.Counts.Add(FString::Printf(TEXT("data_%s"), Kind), Assets.O.Num());
			Doc.Set(TEXT("assets"), MoveTemp(Assets));
			Doc.Set(TEXT("note"), TEXT("Local placeholder assets converted from the user's own UT2004 install. 'asset' is the path to use (a /Game/Replacements override when one exists), 'imported' the generated one."));
			SortKeys(Doc);
			if (!Doc.SaveToFile(AssetsDir / (FString(Kind) + TEXT(".json")), 1))
			{
				Result.Errors.Add(FString::Printf(TEXT("data: cannot write Assets/%s.json"), Kind));
			}
		};

		// Music
		FConvJson MusicAssets = FConvJson::Object();
		for (const FString& F : SortedFiles(CacheDir / TEXT("music"), TEXT("*.json")))
		{
			FConvJson M;
			if (!FConvJson::LoadFile(CacheDir / TEXT("music") / F, M)) continue;
			const FString Name = M.GetString(TEXT("name"));
			if (Name.IsEmpty() || !IFileManager::Get().FileExists(*(CacheDir / TEXT("music") / M.GetString(TEXT("file"))))) continue;
			const FString Dst = TEXT("/Game/Imported/Music/") + AssetName(Name);
			FConvJson E = FConvJson::Object();
			E.Set(TEXT("asset"), Dst);
			E.Set(TEXT("imported"), Dst);
			E.Set(TEXT("duration"), M.Get(TEXT("duration")));
			E.Set(TEXT("channels"), M.Get(TEXT("channels")));
			E.Set(TEXT("sample_rate"), M.Get(TEXT("sample_rate")));
			E.Set(TEXT("title"), M.Get(TEXT("comments")).Get(TEXT("TITLE")));
			E.Set(TEXT("source_file"), M.Get(TEXT("source_file")));
			MusicAssets.Set(Name, E);
		}
		Manifest(TEXT("Music"), MusicAssets);
		auto WriteMap = [&](const FString& MapJson) -> bool
		{
			FConvJson Doc;
			FString Err;
			if (!FConvJson::LoadFile(MapJson, Doc, &Err))
			{
				if (IFileManager::Get().FileExists(*MapJson)) Result.Errors.Add(FString::Printf(TEXT("data: %s: %s"), *MapJson, *Err));
				return false;
			}
			const FConvJson Slim = SlimMarkers(Doc, MusicAssets);
			if (!Slim.SaveToFile(DataDir / (Doc.GetString(TEXT("map")) + TEXT(".json")), 1))
			{
				Result.Errors.Add(FString::Printf(TEXT("data: cannot write %s.json"), *Doc.GetString(TEXT("map"))));
				return false;
			}
			return true;
		};
		if (OnlyMaps)
		{
			// incremental (custom content): the music table and the listed maps only
			int32 NMaps = 0;
			for (const FString& Map : *OnlyMaps)
			{
				NMaps += WriteMap(CacheDir / TEXT("maps") / FConvExporter::Safe(Map) / TEXT("map.json")) ? 1 : 0;
			}
			Result.Counts.Add(TEXT("data_maps"), NMaps);
			return;
		}

		// Sounds
		FConvJson Sounds = FConvJson::Object();
		for (const TPair<FString, FString>& E : CacheEntries(CacheDir, TEXT("sounds")))
		{
			FConvJson M;
			if (!FConvJson::LoadFile(E.Value, M) || M.GetString(TEXT("type")) != TEXT("Sound") || !M.Get(TEXT("file")).Truthy()) continue;
			if (!IFileManager::Get().FileExists(*(FPaths::GetPath(E.Value) / M.GetString(TEXT("file"))))) continue;
			const FString Dst = ImportedPath(TEXT("Sounds"), M.GetString(TEXT("path")));
			FConvJson S = FConvJson::Object();
			S.Set(TEXT("asset"), Dst);
			S.Set(TEXT("imported"), Dst);
			S.Set(TEXT("duration"), M.Get(TEXT("duration")));
			S.Set(TEXT("loop"), M.Get(TEXT("loop")));
			S.Set(TEXT("container"), M.Get(TEXT("container")));
			Sounds.Set(M.GetString(TEXT("path")), S);
		}
		Manifest(TEXT("Sounds"), Sounds);

		// Skeletal meshes and animations (ImportUT2004.py SkeletalImporter.run, minus the editor import)
		struct FEntry { FString Json; FConvJson Meta; };
		TMap<FString, FEntry> Meshes, AllMeshes, Anims;
		TArray<FEntry> Verts;
		for (const TPair<FString, FString>& E : CacheEntries(CacheDir, TEXT("skeletalmeshes")))
		{
			FConvJson M;
			if (!FConvJson::LoadFile(E.Value, M)) continue;
			if (M.GetString(TEXT("type")) == TEXT("SkeletalMesh") && IFileManager::Get().FileExists(*(FPaths::GetPath(E.Value) / M.GetString(TEXT("gltf")))))
			{
				Meshes.Add(M.GetString(TEXT("path")), { E.Value, M });
			}
		}
		for (const TPair<FString, FString>& E : CacheEntries(CacheDir, TEXT("animations")))
		{
			FConvJson M;
			if (FConvJson::LoadFile(E.Value, M) && M.GetString(TEXT("type")) == TEXT("MeshAnimation")) Anims.Add(M.GetString(TEXT("path")), { E.Value, M });
		}
		for (const TPair<FString, FString>& E : CacheEntries(CacheDir, TEXT("vertmeshes")))
		{
			FConvJson M;
			if (FConvJson::LoadFile(E.Value, M) && M.GetString(TEXT("type")) == TEXT("VertMesh")) Verts.Add({ E.Value, M });
		}
		FConvJson MeshTable = FConvJson::Object(), AnimTable = FConvJson::Object();
		TSet<FString> Done;
		TMap<FString, FString> MeshSkeleton;
		auto MeshEntry = [&](const FConvJson& Meta, const FString& Skeleton)
		{
			MeshSkeleton.Add(Meta.GetString(TEXT("path")), Skeleton);
			const FString Dst = ImportedPath(TEXT("SkeletalMeshes"), Meta.GetString(TEXT("path")));
			FConvJson E = FConvJson::Object();
			E.Set(TEXT("asset"), Dst);
			E.Set(TEXT("imported"), Dst);
			E.Set(TEXT("skeleton"), Skeleton);
			E.Set(TEXT("type"), Meta.Get(TEXT("type")));
			E.Set(TEXT("animation"), Meta.Get(TEXT("animation")));
			FConvJson Sockets = FConvJson::Array();
			for (const FConvJson& S : Meta.Get(TEXT("sockets")).A) Sockets.Add(S.Get(TEXT("name")));
			E.Set(TEXT("sockets"), Sockets);
			E.Set(TEXT("bounds"), Meta.Get(TEXT("bounds")));
			E.Set(TEXT("materials"), Meta.Get(TEXT("materials")));
			MeshTable.Set(Meta.GetString(TEXT("path")), E);
		};
		auto AnimEntry = [&](const FConvJson& Meta, const FString& Skeleton, const FString& SetName)
		{
			const FString Path = Meta.GetString(TEXT("path"));
			int32 Dot;
			const FString Pkg = Path.FindChar(TEXT('.'), Dot) ? Path.Left(Dot) : Path;
			FConvJson Seqs = FConvJson::Object();
			for (const FConvJson& S : Meta.Get(TEXT("sequences")).A)
			{
				const FString A = FString::Printf(TEXT("/Game/Imported/Animations/%s/%s_%s"), *AssetName(Pkg), *SetName, *AssetName(S.GetString(TEXT("name"))));
				FConvJson E = FConvJson::Object();
				E.Set(TEXT("asset"), A);
				E.Set(TEXT("imported"), A);
				E.Set(TEXT("num_frames"), S.Get(TEXT("num_frames")));
				E.Set(TEXT("rate"), S.Get(TEXT("rate")));
				E.Set(TEXT("groups"), S.Get(TEXT("groups")));
				E.Set(TEXT("notifies"), S.Get(TEXT("notifies")));
				Seqs.Set(S.GetString(TEXT("name")), E);
			}
			FConvJson E = FConvJson::Object();
			E.Set(TEXT("skeleton"), Skeleton);
			E.Set(TEXT("representative_mesh"), Meta.Get(TEXT("representative_mesh")));
			E.Set(TEXT("set_name"), SetName);
			E.Set(TEXT("type"), Meta.Get(TEXT("type")));
			E.Set(TEXT("sequences"), Seqs);
			AnimTable.Set(Path, E);
		};
		TArray<FString> AnimPaths;
		Anims.GetKeys(AnimPaths);
		AnimPaths.Sort([](const FString& A, const FString& B) { return Cmp(A, B) < 0; });
		TArray<FString> MeshPaths;
		Meshes.GetKeys(MeshPaths);
		MeshPaths.Sort([](const FString& A, const FString& B) { return Cmp(A, B) < 0; });
		for (const FString& APath : AnimPaths)
		{
			const FConvJson& AMeta = Anims[APath].Meta;
			const FString Rep = AMeta.GetString(TEXT("representative_mesh"));
			if (Rep.IsEmpty() || !AMeta.Get(TEXT("gltf")).Truthy()) continue;
			const FEntry* RepE = Meshes.Find(Rep);
			if (!RepE)
			{
				Result.Errors.Add(FString::Printf(TEXT("data: animation %s: representative mesh %s not in the cache"), *APath, *Rep));
				continue;
			}
			// a representative mesh already imported with another set's skeleton keeps that skeleton
			const FString* Prev = MeshSkeleton.Find(Rep);
			const FString Skel = Prev ? *Prev : ImportedPath(TEXT("SkeletalMeshes"), Rep) + TEXT("_Skeleton");
			if (!Done.Contains(Rep))
			{
				MeshEntry(RepE->Meta, Skel);
				Done.Add(Rep);
			}
			for (const FString& MPath : MeshPaths)
			{
				if (Done.Contains(MPath) || Meshes[MPath].Meta.GetString(TEXT("animation")).ToLower() != APath.ToLower()) continue;
				MeshEntry(Meshes[MPath].Meta, Skel);
				Done.Add(MPath);
			}
			int32 Dot;
			AnimEntry(AMeta, Skel, AssetName(APath.FindChar(TEXT('.'), Dot) ? APath.Mid(Dot + 1) : APath));
		}
		for (const FString& MPath : MeshPaths)
		{
			if (Done.Contains(MPath)) continue;
			MeshEntry(Meshes[MPath].Meta, ImportedPath(TEXT("SkeletalMeshes"), MPath) + TEXT("_Skeleton"));
		}
		for (const FEntry& V : Verts)
		{
			const FString Path = V.Meta.GetString(TEXT("path"));
			const FString Skel = ImportedPath(TEXT("SkeletalMeshes"), Path) + TEXT("_Skeleton");
			MeshEntry(V.Meta, Skel);
			if (V.Meta.Get(TEXT("sequences")).Truthy())
			{
				int32 Dot;
				AnimEntry(V.Meta, Skel, AssetName(Path.FindChar(TEXT('.'), Dot) ? Path.Mid(Dot + 1) : Path));
			}
		}
		Manifest(TEXT("SkeletalMeshes"), MeshTable);
		Manifest(TEXT("Animations"), AnimTable);

		// maps
		int32 NMaps = 0;
		for (const FString& MapDir : SortedDirs(CacheDir / TEXT("maps")))
		{
			NMaps += WriteMap(CacheDir / TEXT("maps") / MapDir / TEXT("map.json")) ? 1 : 0;
		}
		Result.Counts.Add(TEXT("data_maps"), NMaps);
	}
}

#endif // ARENA_CONV_BUILD
