#include "ArenaConvConfig.h"
#if ARENA_CONV_BUILD
// Map (.ut2) export: map.json with actors and gameplay markers, plus bsp.gltf
// (port of Tools/ut2k4/level.py and bsp.py).

#include "ArenaConvExporter.h"
#include "ArenaConverter.h"
#include "ArenaConvGltf.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace ArenaConvLevelLocal
{
	// 4: bsp.tree (bsptree.bin: BSP planes + zones for point -> zone queries), "sky_zone" BSP primitives,
	//    volume "brush" (world-space faces / planes), actor "subobjects" (emitters, scripted actions), "class_defaults"
	// 6: "collision_only" BSP primitives (invisible solid surfaces: invisible walls and floors)
	// 7: static meshes re-exported with their "collision_model" primitive
	// 8: sky (FakeBackdrop) surfaces as "collision_only" + "fake_backdrop" primitives
	constexpr int32 SchemaVersion = ArenaConverterVersions::MapSchemaVersion;
	constexpr int32 DT_StaticMesh = 8;

	enum : uint32
	{
		PF_Invisible = 0x00000001, PF_Masked = 0x00000002, PF_Translucent = 0x00000004, PF_NotSolid = 0x00000008,
		PF_Environment = 0x00000010, PF_Semisolid = 0x00000020, PF_Modulated = 0x00000040, PF_FakeBackdrop = 0x00000080,
		PF_TwoSided = 0x00000100, PF_NoSmooth = 0x00000800, PF_AlphaTexture = 0x00001000, PF_Flat = 0x00004000,
		PF_NoMerge = 0x00010000, PF_NoZTest = 0x00020000, PF_Additive = 0x00040000, PF_SpecialLit = 0x00100000,
		PF_Wireframe = 0x00200000, PF_Unlit = 0x00400000, PF_Portal = 0x04000000, PF_Mirrored = 0x08000000, PF_Flag29 = 0x20000000,
	};
	constexpr uint32 SkipFlags = PF_Invisible | PF_Portal | PF_FakeBackdrop;
	/**
	 * Invisible and sky (FakeBackdrop) surfaces still bound solid space in UE2 (invisible walls / floors, the sky
	 * players and projectiles run into): exported as collision only.
	 */
	constexpr uint32 NoCollisionFlags = PF_Portal | PF_NotSolid;
	constexpr uint32 RenderFlags = PF_Masked | PF_Translucent | PF_Modulated | PF_TwoSided | PF_AlphaTexture | PF_Additive | PF_Unlit
		| PF_Environment | PF_Mirrored | PF_NotSolid;

	FConvJson FlagNames(uint32 Flags)
	{
		static const TPair<uint32, const TCHAR*> Names[] = {
			{ PF_Invisible, TEXT("Invisible") }, { PF_Masked, TEXT("Masked") }, { PF_Translucent, TEXT("Translucent") }, { PF_NotSolid, TEXT("NotSolid") },
			{ PF_Environment, TEXT("Environment") }, { PF_Semisolid, TEXT("Semisolid") }, { PF_Modulated, TEXT("Modulated") },
			{ PF_FakeBackdrop, TEXT("FakeBackdrop") }, { PF_TwoSided, TEXT("TwoSided") }, { PF_NoSmooth, TEXT("NoSmooth") },
			{ PF_AlphaTexture, TEXT("AlphaTexture") }, { PF_Flat, TEXT("Flat") }, { PF_NoMerge, TEXT("NoMerge") }, { PF_NoZTest, TEXT("NoZTest") },
			{ PF_Additive, TEXT("Additive") }, { PF_SpecialLit, TEXT("SpecialLit") }, { PF_Wireframe, TEXT("Wireframe") }, { PF_Unlit, TEXT("Unlit") },
			{ PF_Portal, TEXT("Portal") }, { PF_Mirrored, TEXT("Mirrored") }, { PF_Flag29, TEXT("Flag29") } };
		FConvJson A = FConvJson::Array();
		for (const auto& N : Names)
		{
			if (Flags & N.Key) A.Add(N.Value);
		}
		return A;
	}

	const TSet<FString>& SkipProps()
	{
		static const TSet<FString> S = { TEXT("Level"), TEXT("Region"), TEXT("PhysicsVolume"), TEXT("PathList"), TEXT("StaticMeshInstance"),
			TEXT("nextNavigationPoint"), TEXT("Touching"), TEXT("UpstreamPaths"), TEXT("NeededJump"), TEXT("myMarker"), TEXT("XLevel"),
			TEXT("ColLocation"), TEXT("Brush"), TEXT("Layers"), TEXT("DecoLayers"), TEXT("TerrainMap"), TEXT("Sectors"), TEXT("Vertices"),
			TEXT("ManualExcludes") };
		return S;
	}

	FString BaseKey(const FString& K)
	{
		int32 B;
		return K.FindChar(TEXT('['), B) ? K.Left(B) : K;
	}

	const TSet<FString>& NoWalk()
	{
		static const TSet<FString> S = { TEXT("model"), TEXT("polys"), TEXT("reachspec"), TEXT("level"), TEXT("staticmeshinstance"),
			TEXT("levelsummary"), TEXT("class"), TEXT("package"), TEXT("textbuffer"), TEXT("palette") };
		return S;
	}

	FConvJson RotToDeg(const int32 R[3])
	{
		FConvJson O = FConvJson::Object();
		const TCHAR* K[3] = { TEXT("pitch"), TEXT("yaw"), TEXT("roll") };
		for (int32 i = 0; i < 3; ++i)
		{
			double D = double(((int64(R[i]) % 65536) + 65536) % 65536) * 360.0 / 65536.0;
			if (D > 180.0) D -= 360.0;
			O.Set(K[i], FConvJson::RoundTo(D, 5));
		}
		return O;
	}

	FConvJson HsvColor(int32 H, int32 Sat)
	{
		double C[3];
		if (H < 86) { C[0] = (85 - H) / 85.0; C[1] = H / 85.0; C[2] = 0.0; }
		else if (H < 171) { C[0] = 0.0; C[1] = (170 - H) / 85.0; C[2] = (H - 85) / 85.0; }
		else { C[0] = (H - 170) / 85.0; C[1] = 0.0; C[2] = (255 - H) / 84.0; }
		const double S = Sat / 255.0;
		double Rgb[3];
		for (int32 i = 0; i < 3; ++i) Rgb[i] = C[i] + S * (1.0 - C[i]);
		double M = FMath::Max3(Rgb[0], Rgb[1], Rgb[2]);
		if (M == 0) M = 1.0;
		FConvJson A = FConvJson::Array();
		for (int32 i = 0; i < 3; ++i) A.Add(FConvJson::RoundTo(Rgb[i] / M, 4));
		return A;
	}

	/** Python str() of a scalar value for _s(). */
	FString PyStr(const FConvProp& V)
	{
		switch (V.Kind)
		{
		case FConvProp::EKind::Str: return V.S;
		case FConvProp::EKind::Int: return FString::Printf(TEXT("%d"), V.I);
		case FConvProp::EKind::Bool: return V.B ? TEXT("True") : TEXT("False");
		case FConvProp::EKind::Float:
		{
			TArray<ANSICHAR> Buf;
			FConvJson::AppendDouble(Buf, V.F);
			Buf.Add(0);
			return FString(ANSI_TO_TCHAR(Buf.GetData()));
		}
		default: return TEXT("?");
		}
	}

	/** level.py _s: name/str -> str, null for empty / 'None'. */
	FConvJson S_(const FConvProp* V)
	{
		if (!V || V->IsNone()) return FConvJson();
		if (V->IsRef()) return V->Ref.IsSet() ? FConvJson(V->Ref.Name()) : FConvJson();
		const FString S = PyStr(*V);
		return (S.IsEmpty() || S == TEXT("None")) ? FConvJson() : FConvJson(S);
	}

	FConvJson Names_(const FConvProp* V)
	{
		FConvJson Out = FConvJson::Array();
		if (!V || V->IsNone()) return Out;
		if (V->IsArray())
		{
			for (const FConvProp& X : V->A)
			{
				FConvJson N = S_(&X);
				if (N.Truthy()) Out.Add(N);
			}
		}
		else
		{
			FConvJson N = S_(V);
			if (N.Truthy()) Out.Add(N);
		}
		return Out;
	}

	FString ObjectiveType(const TArray<FString>& Chain)
	{
		static const TPair<const TCHAR*, const TCHAR*> Keys[] = {
			{ TEXT("onspowernode"), TEXT("power_node") }, { TEXT("onspowercore"), TEXT("power_core") },
			{ TEXT("destroyvehicleobjective"), TEXT("destroy_vehicle") }, { TEXT("holdobjective"), TEXT("hold") },
			{ TEXT("proximityobjective"), TEXT("proximity") }, { TEXT("useobjective"), TEXT("use") },
			{ TEXT("triggeredobjective"), TEXT("triggered") }, { TEXT("destroyableobjective"), TEXT("destroyable") },
			{ TEXT("lockedobjective"), TEXT("locked") } };
		for (const auto& K : Keys)
		{
			for (const FString& C : Chain) if (C.ToLower() == K.Key) return K.Value;
		}
		return TEXT("other");
	}

	FString PickupKind(const TArray<FString>& Chain)
	{
		TSet<FString> C;
		for (const FString& X : Chain) C.Add(X.ToLower());
		if (C.Contains(TEXT("tournamenthealth")) || C.Contains(TEXT("healthpack"))) return TEXT("health");
		if (C.Contains(TEXT("shieldpickup")) || C.Contains(TEXT("shieldpack")) || C.Contains(TEXT("supershieldpack"))) return TEXT("shield");
		if (C.Contains(TEXT("adrenalinepickup"))) return TEXT("adrenaline");
		if (C.Contains(TEXT("udamagepack"))) return TEXT("udamage");
		if (C.Contains(TEXT("ammo"))) return TEXT("ammo");
		if (C.Contains(TEXT("weaponpickup"))) return TEXT("weapon");
		return TEXT("other");
	}

	FConvJson TeamName(int32 Team)
	{
		if (Team == 0) return FConvJson(TEXT("red"));
		if (Team == 1) return FConvJson(TEXT("blue"));
		if (Team == 255) return FConvJson(TEXT("neutral"));
		return FConvJson(FString::Printf(TEXT("%d"), Team));
	}

	/** Rows are the X, Y, Z axes of an Unreal rotator (level.py _rot_matrix). */
	void RotRows(const int32 R[3], double M[3][3])
	{
		const double p = R[0] * 2.0 * PI / 65536.0, y = R[1] * 2.0 * PI / 65536.0, r = R[2] * 2.0 * PI / 65536.0;
		const double sp = FMath::Sin(p), cp = FMath::Cos(p), sy = FMath::Sin(y), cy = FMath::Cos(y), sr = FMath::Sin(r), cr = FMath::Cos(r);
		M[0][0] = cp * cy; M[0][1] = cp * sy; M[0][2] = sp;
		M[1][0] = sr * sp * cy - cr * sy; M[1][1] = sr * sp * sy + cr * cy; M[1][2] = -sr * cp;
		M[2][0] = -(cr * sp * cy + sr * sy); M[2][1] = cy * sr - cr * sp * sy; M[2][2] = cr * cp;
	}

	/** Sorted (ordinal) property keys whose base name is Base. */
	TArray<const TPair<FString, FConvProp>*> SortedIndexed(const FConvProps& P, const TCHAR* Base)
	{
		TArray<const TPair<FString, FConvProp>*> Out;
		for (const TPair<FString, FConvProp>& KV : P)
		{
			if (BaseKey(KV.Key) == Base) Out.Add(&KV);
		}
		Out.Sort([](const TPair<FString, FConvProp>& A, const TPair<FString, FConvProp>& B) { return FCString::Strcmp(*A.Key, *B.Key) < 0; });
		return Out;
	}

	int32 CmpStr(const FString& A, const FString& B) { return FCString::Strcmp(*A, *B); }
}

using namespace ArenaConvLevelLocal;

/** level.py MapExporter */
class FConvMapExporter
{
public:
	FConvMapExporter(FConvExporter& InEx, const FString& InName) : Ex(InEx), Mgr(InEx.Mgr), Name(InName), S(InEx.Opt.Scale) {}

	bool Run();

private:
	FConvExporter& Ex;
	FConvPackageManager& Mgr;
	FString Name;
	double S;
	TMap<FString, FIntPoint> SizeCache;
	/** event graph: string outputs of ScriptedSequence actions per trigger marker (raw property strings) */
	TMap<FString, TArray<FString>> ActionOuts;

	// ---- helpers ----
	FConvJson Sv(const float V[3]) const
	{
		FConvJson A = FConvJson::Array();
		for (int32 i = 0; i < 3; ++i) A.Add(FConvJson::RoundTo(double(V[i]) * S, 4));
		return A;
	}
	FConvJson SvD(double X, double Y, double Z) const
	{
		FConvJson A = FConvJson::Array();
		A.Add(FConvJson::RoundTo(X * S, 4)); A.Add(FConvJson::RoundTo(Y * S, 4)); A.Add(FConvJson::RoundTo(Z * S, 4));
		return A;
	}
	FConvJson SvProp(const FConvProp* V) const
	{
		if (!V) return FConvJson();
		if (V->Kind == FConvProp::EKind::Vector) return Sv(V->V);
		if (V->IsStruct())
		{
			// a struct-shaped vector: X Y Z members
			const FConvProp* X = V->Find(TEXT("X")); const FConvProp* Y = V->Find(TEXT("Y")); const FConvProp* Z = V->Find(TEXT("Z"));
			if (X && Y && Z) return SvD(X->Num(), Y->Num(), Z->Num());
		}
		return FConvJson();
	}
	const FConvProp* G(FConvObject* O, const TCHAR* K) { return O->GetProp(K); }
	FConvJson GJ(FConvObject* O, const TCHAR* K, FConvJson Default = FConvJson())
	{
		const FConvProp* V = O->GetProp(K);
		return V ? Ex.Jsonify(*V) : Default;
	}
	bool GB(FConvObject* O, const TCHAR* K, bool bDefault)
	{
		const FConvProp* V = O->GetProp(K);
		return V ? V->Truthy() : bDefault;
	}
	double GN(FConvObject* O, const TCHAR* K, double Default)
	{
		const FConvProp* V = O->GetProp(K);
		return V ? V->Num(Default) : Default;
	}
	FConvJson RefJ(const FConvProp* V) { return V && V->IsRef() && V->Ref.IsSet() ? FConvJson(Ex.Canon(V->Ref)) : FConvJson(); }
	FConvJson PropsJ(FConvObject* O)
	{
		FConvJson Out = FConvJson::Object();
		for (const TPair<FString, FConvProp>& KV : O->Props())
		{
			if (!SkipProps().Contains(BaseKey(KV.Key))) Out.O.Emplace(KV.Key, Ex.Jsonify(KV.Value));
		}
		return Out;
	}
	FConvJson ClassRefInfo(const FConvProp* V)
	{
		if (!V || !V->IsRef() || !V->Ref.IsSet()) return FConvJson();
		FConvJson O = FConvJson::Object();
		O.Set(TEXT("class"), V->Ref.Name());
		O.Set(TEXT("path"), Ex.Canon(V->Ref));
		FConvJson C = FConvJson::Array();
		for (const FString& X : Mgr.ClassChain(V->Ref.Pkg, V->Ref.Index)) C.Add(X);
		O.Set(TEXT("class_chain"), C);
		return O;
	}
	FConvJson ActorLocation(const FConvProp* V)
	{
		if (!V || !V->IsRef() || !V->Ref.IsSet()) return FConvJson();
		FConvObject* O = V->Ref.Load();
		if (!O) return FConvJson();
		const FConvProp* L = O->Prop(TEXT("Location"));
		return L ? SvProp(L) : SvD(0, 0, 0);
	}
	void Dep(const FConvRef& R) { Ex.ExportRef(R); }
	void WalkRefs(FConvObject* Obj, int32 Depth, TSet<FString>& Seen);
	FIntPoint TexSize(const FString& MatPath);
	bool ReadLevel(FConvPackage* Pkg, TArray<int32>& OutActors, int32& OutModel);
	FConvJson ActorRecord(FConvObject* O, int32 Idx);
	/** Adds O's class to ClassDefaultsJson (merged UE2 class default properties) for world behaviour classes. */
	void NoteDefaults(FConvObject* O);
	FConvJson ClassDefaultsJson = FConvJson::Object();
	FConvJson Base(const FConvJson& Rec);
	void Marker(FConvObject* O, const FConvJson& Rec, FConvJson& M);
	void ExtraMarker(FConvObject* O, const FConvJson& Rec, FConvJson& M);
	void PostMarkers(FConvJson& M);
	FConvJson ObjectiveFields(FConvObject* O, const FConvJson& Rec);
	FConvJson CoreFields(FConvObject* O, const FConvJson& Rec);
	FConvJson Volume(FConvObject* O, const FConvJson& Rec);
	FConvJson Terrain(FConvObject* O, const FConvJson& Rec);
	FConvJson LevelInfo(FConvPackage* Pkg, const TArray<FConvObject*>& Objs, FConvJson& OutLevel, FConvJson& OutKillZ);
	FConvJson ExportBsp(FConvPackage* Pkg, int32 ModelIdx, const FString& OutDir);
};

void FConvMapExporter::WalkRefs(FConvObject* Obj, int32 Depth, TSet<FString>& Seen)
{
	TArray<FConvRef> Refs;
	Ex.CollectRefs(Obj->Props(), Refs);
	for (const FConvRef& R : Refs)
	{
		if (R.Index < 0)
		{
			const FString Cn = R.ClassName();
			if (Cn == TEXT("Class") || Cn == TEXT("Package")) continue;
			Dep(R);
			continue;
		}
		if (R.Pkg != Obj->Pkg && R.Index > 0)
		{
			Dep(R);
			continue;
		}
		const FString Key = FString::Printf(TEXT("%s|%d"), *R.Pkg->PathOnDisk, R.Index);
		if (Seen.Contains(Key)) continue;
		Seen.Add(Key);
		if (NoWalk().Contains(R.ClassName().ToLower())) continue;
		FConvObject* O = R.Load();
		if (!O) continue;
		if (!Ex.KindOf(O).IsEmpty())
		{
			Ex.ExportObject(O);
		}
		else if (Depth < 3 && !O->IsA(TEXT("Actor")))
		{
			WalkRefs(O, Depth + 1, Seen);
		}
	}
}

FIntPoint FConvMapExporter::TexSize(const FString& MatPath)
{
	if (const FIntPoint* Hit = SizeCache.Find(MatPath)) return *Hit;
	FIntPoint Out(256, 256);
	if (!MatPath.IsEmpty() && MatPath != TEXT("None"))
	{
		if (FConvObject* O = Mgr.ResolvePath(MatPath))
		{
			const FConvExporter::FMatResolve R = Ex.ResolveMaterial(O);
			if (R.BaseTexture) Out = Ex.TextureSize(R.BaseTexture);
		}
	}
	SizeCache.Add(MatPath, Out);
	return Out;
}

bool FConvMapExporter::ReadLevel(FConvPackage* Pkg, TArray<int32>& OutActors, int32& OutModel)
{
	OutModel = 0;
	bool bHaveActors = false;
	const TArray<int32> Lv = Pkg->ExportsOfClass({ TEXT("Level") });
	if (Lv.Num())
	{
		FConvObject* L = Pkg->GetObject(Lv[0]);
		FConvReader R = L->NativeReader();
		const int32 N = R.I32();
		R.I32();
		if (!R.bError && N >= 0 && N <= R.Remaining())
		{
			TArray<int32> A;
			for (int32 i = 0; i < N; ++i) A.Add(R.Index());
			for (int32 i = 0; i < 4; ++i) R.FString_();
			const int32 No = R.Index();
			for (int32 i = 0; i < No && !R.bError; ++i) R.FString_();
			R.I32();
			R.I32();
			int32 Model = R.Index();
			if (!R.bError)
			{
				if (Model <= 0 || Model - 1 >= Pkg->Exports.Num() || Pkg->ExportClassName(Model - 1) != TEXT("Model")) Model = 0;
				OutActors = A;
				OutModel = Model;
				bHaveActors = true;
			}
		}
		if (!bHaveActors)
		{
			Ex.Error(FString::Printf(TEXT("%s level header"), *Pkg->Name), R.bError ? R.Error : FString(TEXT("bad actor count")));
		}
	}
	if (!bHaveActors)
	{
		OutActors.Reset();
		for (int32 i = 0; i < Pkg->Exports.Num(); ++i)
		{
			if (Pkg->GetObject(i)->IsA(TEXT("Actor"))) OutActors.Add(i + 1);
		}
	}
	if (!OutModel)
	{
		int32 Best = -1;
		for (int32 I : Pkg->ExportsOfClass({ TEXT("Model") }))
		{
			if (Best < 0 || Pkg->Exports[I].Size > Pkg->Exports[Best].Size) Best = I;
		}
		OutModel = Best >= 0 ? Best + 1 : 0;
	}
	OutActors.RemoveAll([](int32 A) { return A <= 0; });
	return true;
}

FConvJson FConvMapExporter::ActorRecord(FConvObject* O, int32 Idx)
{
	const FConvProps& P = O->Props();
	FConvJson Rec = FConvJson::Object();
	Rec.Set(TEXT("index"), Idx);
	Rec.Set(TEXT("name"), O->Name());
	Rec.Set(TEXT("path"), O->Path());
	Rec.Set(TEXT("class"), O->ClassName());
	FConvJson Chain = FConvJson::Array();
	for (const FString& C : O->ClassChain()) Chain.Add(C);
	Rec.Set(TEXT("class_chain"), Chain);
	const FConvProp* Loc = O->Prop(TEXT("Location"));
	Rec.Set(TEXT("location"), Loc ? SvProp(Loc) : SvD(0, 0, 0));
	const FConvProp* Rot = O->Prop(TEXT("Rotation"));
	int32 R[3] = { 0, 0, 0 };
	if (Rot && Rot->Kind == FConvProp::EKind::Rotator) { R[0] = Rot->R[0]; R[1] = Rot->R[1]; R[2] = Rot->R[2]; }
	Rec.Set(TEXT("rotation"), RotToDeg(R));
	FConvJson Raw = FConvJson::Array();
	Raw.Add(R[0]); Raw.Add(R[1]); Raw.Add(R[2]);
	Rec.Set(TEXT("rotation_raw"), Raw);
	Rec.Set(TEXT("draw_scale"), GJ(O, TEXT("DrawScale"), FConvJson(1.0)));
	const FConvProp* Ds3 = G(O, TEXT("DrawScale3D"));
	FConvJson Ds = FConvJson::Array();
	if (Ds3 && Ds3->Kind == FConvProp::EKind::Vector)
	{
		for (int32 i = 0; i < 3; ++i) Ds.Add(FConvJson::RoundTo(Ds3->V[i], 6));
	}
	else
	{
		Ds.Add(1.0); Ds.Add(1.0); Ds.Add(1.0);
	}
	Rec.Set(TEXT("draw_scale3d"), Ds);
	const FConvProp* Pp = O->Prop(TEXT("PrePivot"));
	Rec.Set(TEXT("pre_pivot"), Pp ? SvProp(Pp) : SvD(0, 0, 0));
	const FConvProp* DtP = G(O, TEXT("DrawType"));
	const int32 DrawType = DtP ? DtP->Int() : 1;
	Rec.Set(TEXT("draw_type"), DtP ? Ex.Jsonify(*DtP) : FConvJson(1));
	Rec.Set(TEXT("static_mesh"), FConvJson());
	Rec.Set(TEXT("tag"), GJ(O, TEXT("Tag"), FConvJson(O->ClassName())));
	Rec.Set(TEXT("event"), GJ(O, TEXT("Event"), FConvJson(TEXT("None"))));
	Rec.Set(TEXT("hidden"), GB(O, TEXT("bHidden"), false));
	Rec.Set(TEXT("collision_radius"), FConvJson::RoundTo(GN(O, TEXT("CollisionRadius"), 0.0) * S, 4));
	Rec.Set(TEXT("collision_height"), FConvJson::RoundTo(GN(O, TEXT("CollisionHeight"), 0.0) * S, 4));
	const FConvProp* B = O->Prop(TEXT("Base"));
	Rec.Set(TEXT("base"), B && B->Truthy() ? RefJ(B) : FConvJson());
	if (DrawType == DT_StaticMesh || O->IsA(TEXT("StaticMeshActor"), TEXT("Mover")))
	{
		const FConvProp* Sm = G(O, TEXT("StaticMesh"));
		if (Sm && Sm->Truthy() && Sm->IsRef()) Rec.Set(TEXT("static_mesh"), Ex.Canon(Sm->Ref));
	}
	const FConvProp* Skins = O->Prop(TEXT("Skins"));
	if (Skins && Skins->IsArray() && Skins->A.Num())
	{
		FConvJson Sk = FConvJson::Array();
		for (const FConvProp& X : Skins->A) Sk.Add(X.IsRef() && X.Ref.IsSet() ? FConvJson(Ex.Canon(X.Ref)) : FConvJson());
		Rec.Set(TEXT("skins"), Sk);
	}
	FConvJson Props = FConvJson::Object();
	for (const TPair<FString, FConvProp>& KV : P)
	{
		if (!SkipProps().Contains(BaseKey(KV.Key))) Props.O.Emplace(KV.Key, Ex.Jsonify(KV.Value));
	}
	Rec.Set(TEXT("properties"), Props);
	// inline subobjects the runtime needs (Emitter.Emitters: SpriteEmitter / MeshEmitter / BeamEmitter / SparkEmitter;
	// ScriptedSequence.Actions: ACTION_*), keyed by the reference string used in "properties"
	FConvJson Subs = FConvJson::Object();
	for (const TPair<FString, FConvProp>& KV : P)
	{
		const FString Key = BaseKey(KV.Key);
		if (Key != TEXT("Emitters") && Key != TEXT("Actions")) continue;
		TArray<const FConvProp*> Refs;
		if (KV.Value.IsArray()) { for (const FConvProp& X : KV.Value.A) Refs.Add(&X); }
		else Refs.Add(&KV.Value);
		for (const FConvProp* X : Refs)
		{
			if (!X->IsRef() || !X->Ref.IsSet()) continue;
			FConvObject* So = X->Ref.Load();
			if (!So || So->IsA(TEXT("Actor"))) continue;
			const FConvJson RefKey = Ex.Jsonify(*X);
			if (RefKey.Type != FConvJson::EType::String) continue;
			FConvJson Sub = FConvJson::Object();
			Sub.Set(TEXT("name"), So->Name());
			Sub.Set(TEXT("class"), So->ClassName());
			FConvJson SChain = FConvJson::Array();
			for (const FString& C : So->ClassChain()) SChain.Add(C);
			Sub.Set(TEXT("class_chain"), SChain);
			Sub.Set(TEXT("properties"), PropsJ(So));
			Subs.Set(RefKey.S, Sub);
			NoteDefaults(So);
		}
	}
	if (Subs.O.Num() > 0) Rec.Set(TEXT("subobjects"), Subs);
	NoteDefaults(O);
	return Rec;
}

void FConvMapExporter::NoteDefaults(FConvObject* O)
{
	// world behaviour classes only (static meshes, lights, brushes and path nodes need no defaults)
	if (!O->IsA(TEXT("Volume"), TEXT("ZoneInfo"), TEXT("Mover"), TEXT("Triggers")) && !O->IsA(TEXT("Teleporter"), TEXT("Emitter"), TEXT("xEmitter"), TEXT("Keypoint"))
		&& !O->IsA(TEXT("ParticleEmitter"), TEXT("ScriptedAction"), TEXT("ScriptedSequence"), TEXT("Info")))
	{
		return;
	}
	const FString Cn = O->ClassName();
	if (ClassDefaultsJson.Has(Cn)) return;
	FConvJson Out = FConvJson::Object();
	for (const TPair<FString, FConvProp>& KV : Mgr.ClassDefaults(O->Pkg, O->Export().ClassIndex))
	{
		if (!SkipProps().Contains(BaseKey(KV.Key))) Out.O.Emplace(KV.Key, Ex.Jsonify(KV.Value));
	}
	ClassDefaultsJson.Set(Cn, Out);
}

FConvJson FConvMapExporter::Base(const FConvJson& Rec)
{
	FConvJson D = FConvJson::Object();
	D.Set(TEXT("actor"), Rec.Get(TEXT("name")));
	D.Set(TEXT("actor_index"), Rec.Get(TEXT("index")));
	D.Set(TEXT("class"), Rec.Get(TEXT("class")));
	D.Set(TEXT("location"), Rec.Get(TEXT("location")));
	D.Set(TEXT("rotation"), Rec.Get(TEXT("rotation")));
	return D;
}

static FConvJson RecS(const FConvJson& V)
{
	// _s() of a json value that came from a name / str
	if (V.Type != FConvJson::EType::String) return V.IsNull() ? FConvJson() : V;
	return (V.S.IsEmpty() || V.S == TEXT("None")) ? FConvJson() : V;
}

void FConvMapExporter::Marker(FConvObject* O, const FConvJson& Rec, FConvJson& M)
{
	const FConvProps& P = O->Props();
	auto Add = [&M](const TCHAR* Kind, FConvJson D) { M.Find(Kind)->Add(MoveTemp(D)); };
	if (O->IsA(TEXT("PlayerStart")))
	{
		FConvJson D = Base(Rec);
		D.Set(TEXT("team"), GJ(O, TEXT("TeamNumber"), FConvJson(0)));
		D.Set(TEXT("enabled"), GB(O, TEXT("bEnabled"), true));
		D.Set(TEXT("primary"), GB(O, TEXT("bPrimaryStart"), true));
		D.Set(TEXT("tag"), RecS(Rec.Get(TEXT("tag"))));
		D.Set(TEXT("event"), S_(G(O, TEXT("Event"))));
		Add(TEXT("player_starts"), D);
	}
	else if (O->IsA(TEXT("xWeaponBase")))
	{
		const FConvProp* Wt = ConvFindProp(P, TEXT("WeaponType"));
		FConvJson D = Base(Rec);
		D.Set(TEXT("weapon_class"), Wt && Wt->Truthy() ? ClassRefInfo(Wt) : FConvJson());
		D.Set(TEXT("weapon_class_property"), TEXT("WeaponType"));
		Add(TEXT("weapon_bases"), D);
	}
	else if (O->IsA(TEXT("WeaponLocker")))
	{
		FConvJson Ws = FConvJson::Array();
		const FConvProp* W = ConvFindProp(P, TEXT("Weapons"));
		if (W && W->IsArray())
		{
			for (const FConvProp& X : W->A)
			{
				if (!X.IsStruct()) continue;
				FConvJson E = FConvJson::Object();
				E.Set(TEXT("weapon_class"), ClassRefInfo(X.Find(TEXT("WeaponClass"))));
				const FConvProp* Ea = X.Find(TEXT("ExtraAmmo"));
				E.Set(TEXT("extra_ammo"), Ea ? Ex.Jsonify(*Ea) : FConvJson(0));
				Ws.Add(E);
			}
		}
		FConvJson D = Base(Rec);
		D.Set(TEXT("weapons"), Ws);
		D.Set(TEXT("weapon_class_property"), TEXT("Weapons[].WeaponClass"));
		Add(TEXT("weapon_lockers"), D);
	}
	else if (O->IsA(TEXT("WildCardBase")))
	{
		FConvJson Cls = FConvJson::Array();
		for (const auto* KV : SortedIndexed(P, TEXT("PickupClasses")))
		{
			if (KV->Value.IsRef() && KV->Value.Ref.IsSet()) Cls.Add(ClassRefInfo(&KV->Value));
		}
		FConvJson D = Base(Rec);
		D.Set(TEXT("powerup"), FConvJson());
		D.Set(TEXT("kind"), TEXT("wildcard"));
		D.Set(TEXT("pickup_classes"), Cls);
		D.Set(TEXT("powerup_property"), TEXT("PickupClasses[]"));
		Add(TEXT("pickup_bases"), D);
	}
	else if (O->IsA(TEXT("xPickUpBase")))
	{
		const FConvProp* Pu = G(O, TEXT("PowerUp"));
		FConvJson Info = Pu && Pu->Truthy() ? ClassRefInfo(Pu) : FConvJson();
		FString Kind = TEXT("none");
		if (Info.Truthy())
		{
			TArray<FString> Chain;
			for (const FConvJson& C : Info.Get(TEXT("class_chain")).A) Chain.Add(C.S);
			Kind = PickupKind(Chain);
		}
		FConvJson D = Base(Rec);
		D.Set(TEXT("powerup"), Info);
		D.Set(TEXT("kind"), Kind);
		D.Set(TEXT("powerup_property"), TEXT("PowerUp"));
		Add(TEXT("pickup_bases"), D);
	}
	else if (O->IsA(TEXT("Pickup")))
	{
		const FConvProp* It = G(O, TEXT("InventoryType"));
		FConvJson D = Base(Rec);
		D.Set(TEXT("kind"), PickupKind(O->ClassChain()));
		D.Set(TEXT("inventory_type"), It && It->Truthy() ? RefJ(It) : FConvJson());
		Add(TEXT("pickups"), D);
	}
	else if (O->IsA(TEXT("JumpPad")))
	{
		const FConvProp* Tgt = ConvFindProp(P, TEXT("JumpTarget"));
		const FConvProp* Jv = G(O, TEXT("JumpVelocity"));
		float V[3] = { 0, 0, 0 };
		if (Jv && Jv->Kind == FConvProp::EKind::Vector) { V[0] = Jv->V[0]; V[1] = Jv->V[1]; V[2] = Jv->V[2]; }
		FConvJson D = Base(Rec);
		const bool bTgt = Tgt && Tgt->Truthy();
		D.Set(TEXT("jump_target"), bTgt && Tgt->IsRef() ? FConvJson(Tgt->Ref.Name()) : FConvJson());
		D.Set(TEXT("jump_target_location"), bTgt ? ActorLocation(Tgt) : FConvJson());
		D.Set(TEXT("jump_velocity"), Sv(V));
		FConvJson Raw = FConvJson::Array();
		for (int32 i = 0; i < 3; ++i) Raw.Add(FConvJson::RoundTo(V[i], 6));
		D.Set(TEXT("jump_velocity_raw"), Raw);
		D.Set(TEXT("jump_z_modifier"), GJ(O, TEXT("JumpZModifier"), FConvJson(1.0)));
		Add(TEXT("jump_pads"), D);
	}
	else if (O->IsA(TEXT("Teleporter")))
	{
		FConvJson D = Base(Rec);
		D.Set(TEXT("url"), GJ(O, TEXT("URL"), FConvJson(TEXT(""))));
		D.Set(TEXT("tag"), Rec.Get(TEXT("tag")));
		D.Set(TEXT("enabled"), GB(O, TEXT("bEnabled"), true));
		Add(TEXT("teleporters"), D);
	}
	else if (O->IsA(TEXT("CTFBase")))
	{
		const FString Cn = O->ClassName().ToLower();
		FConvJson D = Base(Rec);
		D.Set(TEXT("team"), Cn.Contains(TEXT("red")) ? FConvJson(0) : Cn.Contains(TEXT("blue")) ? FConvJson(1) : GJ(O, TEXT("DefenderTeamIndex"), FConvJson(0)));
		Add(TEXT("flag_bases"), D);
	}
	else if (O->IsA(TEXT("xBombSpawn")))
	{
		Add(TEXT("bomb_spawns"), Base(Rec));
	}
	else if (O->IsA(TEXT("xBombDelivery")))
	{
		FConvJson D = Base(Rec);
		D.Set(TEXT("team"), G(O, TEXT("Team")) ? GJ(O, TEXT("Team")) : GJ(O, TEXT("DefenderTeamIndex"), FConvJson(0)));
		D.Set(TEXT("touchdown_difficulty"), GJ(O, TEXT("TouchDownDifficulty")));
		D.Set(TEXT("tag"), S_(G(O, TEXT("Tag"))));
		D.Set(TEXT("event"), S_(G(O, TEXT("Event"))));
		Add(TEXT("bomb_deliveries"), D);
	}
	else if (O->IsA(TEXT("xDomPoint"), TEXT("DominationPoint")))
	{
		const FString Cn = O->ClassName().ToLower();
		FConvJson Letter;
		if (Cn.EndsWith(TEXT("a")) && Cn.Contains(TEXT("dompoint"))) Letter = TEXT("A");
		else if (Cn.EndsWith(TEXT("b")) && Cn.Contains(TEXT("dompoint"))) Letter = TEXT("B");
		FConvJson D = Base(Rec);
		D.Set(TEXT("point_name"), GJ(O, TEXT("PointName"), FConvJson(TEXT(""))));
		D.Set(TEXT("letter"), Letter);
		D.Set(TEXT("primary_team"), GJ(O, TEXT("PrimaryTeam")));
		D.Set(TEXT("defender_team"), GJ(O, TEXT("DefenderTeamIndex"), FConvJson(255)));
		D.Set(TEXT("controllable"), GB(O, TEXT("bControllable"), true));
		D.Set(TEXT("control_event"), S_(G(O, TEXT("ControlEvent"))));
		D.Set(TEXT("tag"), S_(G(O, TEXT("Tag"))));
		D.Set(TEXT("event"), S_(G(O, TEXT("Event"))));
		Add(TEXT("domination_points"), D);
	}
	else if (O->IsA(TEXT("GameObjective")))
	{
		FConvJson D = Base(Rec);
		D.Set(TEXT("defender_team"), GJ(O, TEXT("DefenderTeamIndex"), FConvJson(0)));
		D.Set(TEXT("objective_name"), GJ(O, TEXT("ObjectiveName"), FConvJson(TEXT(""))));
		for (TPair<FString, FConvJson>& KV : ObjectiveFields(O, Rec).O) D.Set(KV.Key, KV.Value);
		Add(TEXT("game_objectives"), D);
	}
	else if (O->IsA(TEXT("LiftCenter"), TEXT("LiftExit")))
	{
		FConvJson D = Base(Rec);
		D.Set(TEXT("lift_tag"), GJ(O, TEXT("LiftTag"), FConvJson(TEXT("None"))));
		Add(TEXT("lifts"), D);
	}
	else if (O->IsA(TEXT("InventorySpot"))) Add(TEXT("inventory_spots"), Base(Rec));
	else if (O->IsA(TEXT("JumpSpot"), TEXT("JumpDest"))) Add(TEXT("jump_spots"), Base(Rec));
	else if (O->IsA(TEXT("PathNode"))) Add(TEXT("path_nodes"), Base(Rec));
	else if (O->IsA(TEXT("NavigationPoint"))) Add(TEXT("other_nav_points"), Base(Rec));
	else if (O->IsA(TEXT("Light")))
	{
		const int32 Hue = (int32)GN(O, TEXT("LightHue"), 0), Sat = (int32)GN(O, TEXT("LightSaturation"), 255);
		const double Rad = GN(O, TEXT("LightRadius"), 64.0);
		const FConvProp* Lt = G(O, TEXT("LightType"));
		const FConvProp* Le = G(O, TEXT("LightEffect"));
		static const TCHAR* Types[] = { TEXT("None"), TEXT("Steady"), TEXT("Pulse"), TEXT("Blink"), TEXT("Flicker"), TEXT("Strobe"), TEXT("BackdropLight"),
			TEXT("SubtlePulse"), TEXT("TexturePaletteOnce"), TEXT("TexturePaletteLoop"), TEXT("FadeOut") };
		static const TCHAR* Effects[] = { TEXT("None"), TEXT("TorchWaver"), TEXT("FireWaver"), TEXT("WateryShimmer"), TEXT("Searchlight"), TEXT("SlowWave"),
			TEXT("FastWave"), TEXT("CloudCast"), TEXT("StaticSpot"), TEXT("Shock"), TEXT("Disco"), TEXT("Warp"), TEXT("Spotlight"), TEXT("NonIncidence"),
			TEXT("Shell"), TEXT("OmniBumpMap"), TEXT("Interference"), TEXT("Cylinder"), TEXT("Rotor"), TEXT("Negative"), TEXT("Sunlight"), TEXT("QuadraticNonIncidence") };
		const int32 LtV = Lt ? Lt->Int() : 1, LeV = Le ? Le->Int() : 0;
		FConvJson D = Base(Rec);
		D.Set(TEXT("light_type"), LtV >= 0 && LtV <= 10 ? FConvJson(Types[LtV]) : FConvJson(LtV));
		D.Set(TEXT("light_effect"), LeV >= 0 && LeV <= 21 ? FConvJson(Effects[LeV]) : FConvJson(LeV));
		D.Set(TEXT("brightness"), GJ(O, TEXT("LightBrightness"), FConvJson(64.0)));
		D.Set(TEXT("hue"), GJ(O, TEXT("LightHue"), FConvJson(0)));
		D.Set(TEXT("saturation"), GJ(O, TEXT("LightSaturation"), FConvJson(255)));
		D.Set(TEXT("color_rgb"), HsvColor(Hue, Sat));
		D.Set(TEXT("radius"), GJ(O, TEXT("LightRadius"), FConvJson(64.0)));
		D.Set(TEXT("radius_world"), FConvJson::RoundTo(FConvJson::RoundTo(25.0 * (Rad + 1.0) * S, 3), 6));
		D.Set(TEXT("cone"), GJ(O, TEXT("LightCone"), FConvJson(128)));
		D.Set(TEXT("dynamic"), GB(O, TEXT("bDynamicLight"), false));
		D.Set(TEXT("directional"), GB(O, TEXT("bDirectional"), false) || O->IsA(TEXT("Spotlight"), TEXT("Sunlight")));
		D.Set(TEXT("sunlight"), O->IsA(TEXT("Sunlight")));
		Add(TEXT("lights"), D);
	}
	else if (O->IsA(TEXT("ZoneInfo")))
	{
		if (O->IsA(TEXT("LevelInfo"))) return;
		const FConvProp* Kz = ConvFindProp(P, TEXT("KillZ"));
		FConvJson D = Base(Rec);
		D.Set(TEXT("sky_zone"), O->IsA(TEXT("SkyZoneInfo")));
		D.Set(TEXT("zone_tag"), GJ(O, TEXT("ZoneTag"), FConvJson(TEXT("None"))));
		D.Set(TEXT("kill_z"), Kz ? FConvJson(FConvJson::RoundTo(Kz->Num() * S, 4)) : FConvJson());
		D.Set(TEXT("soft_kill_z"), GB(O, TEXT("bSoftKillZ"), false));
		D.Set(TEXT("distance_fog"), GB(O, TEXT("bDistanceFog"), false));
		D.Set(TEXT("fog_color"), GJ(O, TEXT("DistanceFogColor")));
		D.Set(TEXT("fog_start"), GJ(O, TEXT("DistanceFogStart")));
		D.Set(TEXT("fog_end"), GJ(O, TEXT("DistanceFogEnd")));
		D.Set(TEXT("ambient_brightness"), GJ(O, TEXT("AmbientBrightness"), FConvJson(0)));
		D.Set(TEXT("ambient_hue"), GJ(O, TEXT("AmbientHue"), FConvJson(0)));
		D.Set(TEXT("ambient_saturation"), GJ(O, TEXT("AmbientSaturation"), FConvJson(255)));
		D.Set(TEXT("terrain_zone"), GB(O, TEXT("bTerrainZone"), false));
		Add(TEXT("zones"), D);
	}
	else if (O->IsA(TEXT("Volume"))) Add(TEXT("volumes"), Volume(O, Rec));
	else if (O->IsA(TEXT("TerrainInfo"))) Add(TEXT("terrains"), Terrain(O, Rec));
	else if (O->IsA(TEXT("AmbientSound")))
	{
		const FConvProp* Snd = G(O, TEXT("AmbientSound"));
		FConvJson D = Base(Rec);
		D.Set(TEXT("sound"), Snd && Snd->Truthy() ? RefJ(Snd) : FConvJson());
		D.Set(TEXT("radius"), GJ(O, TEXT("SoundRadius")));
		D.Set(TEXT("volume"), GJ(O, TEXT("SoundVolume")));
		Add(TEXT("ambient_sounds"), D);
	}
	if (O->IsA(TEXT("Mover")))
	{
		static const TCHAR* Events[] = { TEXT("OpeningEvent"), TEXT("OpenedEvent"), TEXT("ClosingEvent"), TEXT("ClosedEvent"), TEXT("LoopEvent"),
			TEXT("BumpEvent"), TEXT("PlayerBumpEvent"), TEXT("ReturnGroup") };
		FConvJson Te = FConvJson::Object();
		for (const TCHAR* K : Events)
		{
			FConvJson V = S_(G(O, K));
			if (V.Truthy()) Te.Set(K, V);
		}
		FConvJson Kp = FConvJson::Array(), Kr = FConvJson::Array();
		for (const auto* KV : SortedIndexed(P, TEXT("KeyPos"))) Kp.Add(SvProp(&KV->Value));
		for (const auto* KV : SortedIndexed(P, TEXT("KeyRot")))
		{
			int32 R[3] = { 0, 0, 0 };
			if (KV->Value.Kind == FConvProp::EKind::Rotator) { R[0] = KV->Value.R[0]; R[1] = KV->Value.R[1]; R[2] = KV->Value.R[2]; }
			Kr.Add(RotToDeg(R));
		}
		FConvJson D = Base(Rec);
		D.Set(TEXT("tag"), RecS(Rec.Get(TEXT("tag"))));
		D.Set(TEXT("event"), S_(G(O, TEXT("Event"))));
		D.Set(TEXT("trigger_events"), Te);
		D.Set(TEXT("bump_type"), GJ(O, TEXT("BumpType")));
		D.Set(TEXT("stay_open_time"), GJ(O, TEXT("StayOpenTime")));
		D.Set(TEXT("trigger_once_only"), GB(O, TEXT("bTriggerOnceOnly"), false));
		D.Set(TEXT("damage_triggered"), GB(O, TEXT("bDamageTriggered"), false));
		D.Set(TEXT("static_mesh"), Rec.Get(TEXT("static_mesh")));
		D.Set(TEXT("key_num"), GJ(O, TEXT("NumKeys"), FConvJson(0)));
		D.Set(TEXT("key_pos"), Kp);
		D.Set(TEXT("key_rot"), Kr);
		D.Set(TEXT("move_time"), GJ(O, TEXT("MoveTime"), FConvJson(1.0)));
		D.Set(TEXT("initial_state"), GJ(O, TEXT("InitialState")));
		Add(TEXT("movers"), D);
	}
}

FConvJson FConvMapExporter::ObjectiveFields(FConvObject* O, const FConvJson& Rec)
{
	FConvJson D = FConvJson::Object();
	D.Set(TEXT("objective_type"), ObjectiveType(O->ClassChain()));
	D.Set(TEXT("tag"), RecS(Rec.Get(TEXT("tag"))));
	D.Set(TEXT("event"), S_(G(O, TEXT("Event"))));
	D.Set(TEXT("defense_priority"), GJ(O, TEXT("DefensePriority"), FConvJson(0)));
	D.Set(TEXT("objective_priority"), GJ(O, TEXT("ObjectivePriority"), FConvJson(0)));
	D.Set(TEXT("description"), GJ(O, TEXT("ObjectiveDescription"), FConvJson(TEXT(""))));
	D.Set(TEXT("info_attacker"), GJ(O, TEXT("Objective_Info_Attacker"), FConvJson(TEXT(""))));
	D.Set(TEXT("info_defender"), GJ(O, TEXT("Objective_Info_Defender"), FConvJson(TEXT(""))));
	D.Set(TEXT("destruction_message"), GJ(O, TEXT("DestructionMessage"), FConvJson(TEXT(""))));
	D.Set(TEXT("use_description"), GJ(O, TEXT("UseDescription")));
	D.Set(TEXT("score"), GJ(O, TEXT("Score"), FConvJson(5)));
	D.Set(TEXT("optional"), GB(O, TEXT("bOptionalObjective"), false));
	D.Set(TEXT("initially_active"), GB(O, TEXT("bInitiallyActive"), true));
	D.Set(TEXT("bot_only"), GB(O, TEXT("bBotOnlyObjective"), false));
	D.Set(TEXT("team_controlled"), GB(O, TEXT("bTeamControlled"), false));
	D.Set(TEXT("trigger_once_only"), GB(O, TEXT("bTriggerOnceOnly"), false));
	D.Set(TEXT("must_board_vehicle_first"), GB(O, TEXT("bMustBoardVehicleFirst"), false));
	D.Set(TEXT("defense_script_tags"), S_(G(O, TEXT("DefenseScriptTags"))));
	D.Set(TEXT("area_volume_tag"), S_(G(O, TEXT("AreaVolumeTag"))));
	D.Set(TEXT("critical_volume_tag"), S_(G(O, TEXT("CriticalObjectiveVolumeTag"))));
	D.Set(TEXT("physical_actors_tag"), S_(G(O, TEXT("PhysicalObjectiveActorsTag"))));
	D.Set(TEXT("vehicle_path_name"), S_(G(O, TEXT("VehiclePathName"))));
	D.Set(TEXT("end_camera_tag"), S_(G(O, TEXT("EndCameraTag"))));
	D.Set(TEXT("icon"), RefJ(G(O, TEXT("ObjectiveTypeIcon"))));
	FConvJson Ann = FConvJson::Object();
	for (const TCHAR* K : { TEXT("Announcer_ObjectiveInfo"), TEXT("Announcer_DisabledObjective"), TEXT("Announcer_DefendObjective") })
	{
		FConvJson V = RefJ(G(O, K));
		if (V.Truthy()) Ann.Set(K, V);
	}
	D.Set(TEXT("announcer"), Ann);
	if (O->IsA(TEXT("DestroyableObjective")))
	{
		D.Set(TEXT("damage_capacity"), GJ(O, TEXT("DamageCapacity"), FConvJson(100)));
		D.Set(TEXT("health"), GJ(O, TEXT("Health")));
		D.Set(TEXT("can_defender_damage"), GB(O, TEXT("bCanDefenderDamage"), false));
		D.Set(TEXT("damage_event"), S_(G(O, TEXT("TakeDamageEvent"))));
		D.Set(TEXT("damage_event_threshold"), GJ(O, TEXT("DamageEventThreshold")));
		D.Set(TEXT("link_heal_mult"), GJ(O, TEXT("LinkHealMult")));
		D.Set(TEXT("constraint_pawn_class"), RefJ(G(O, TEXT("ConstraintPawnClass"))));
	}
	if (O->IsA(TEXT("ProximityObjective")))
	{
		D.Set(TEXT("constraint_pawn_class"), RefJ(G(O, TEXT("ConstraintPawnClass"))));
	}
	if (O->IsA(TEXT("DestroyVehicleObjective")))
	{
		FConvJson V = S_(G(O, TEXT("VehicleFactoryName")));
		D.Set(TEXT("vehicle_factory_tag"), V.Truthy() ? V : S_(G(O, TEXT("VehicleFactoryTag"))));
	}
	return D;
}

FConvJson FConvMapExporter::CoreFields(FConvObject* O, const FConvJson& Rec)
{
	const FConvProp* TeamP = G(O, TEXT("DefenderTeamIndex"));
	const int32 Team = TeamP ? TeamP->Int() : 0;
	const bool bNeutral = GB(O, TEXT("bStartNeutral"), false) || O->IsA(TEXT("ONSPowerNodeNeutral"));
	const bool bNode = O->IsA(TEXT("ONSPowerNode"));
	const bool bN = bNeutral && bNode;
	FConvJson D = Base(Rec);
	D.Set(TEXT("is_node"), bNode);
	D.Set(TEXT("final_core"), GB(O, TEXT("bFinalCore"), false));
	D.Set(TEXT("team"), bN ? FConvJson(255) : (TeamP ? Ex.Jsonify(*TeamP) : FConvJson(0)));
	D.Set(TEXT("team_name"), TeamName(bN ? 255 : Team));
	D.Set(TEXT("start_neutral"), bNeutral);
	D.Set(TEXT("node_num"), GJ(O, TEXT("NodeNum")));
	D.Set(TEXT("linked_nodes"), Names_(G(O, TEXT("LinkedNodes"))));
	D.Set(TEXT("construction_time"), GJ(O, TEXT("ConstructionTime")));
	D.Set(TEXT("tag"), RecS(Rec.Get(TEXT("tag"))));
	D.Set(TEXT("event"), S_(G(O, TEXT("Event"))));
	D.Set(TEXT("destroyed_event"), S_(G(O, TEXT("DestroyedEvent"))));
	D.Set(TEXT("constructed_event"), S_(G(O, TEXT("ConstructedEvent"))));
	D.Set(TEXT("red_activation_event"), S_(G(O, TEXT("RedActivationEventName"))));
	D.Set(TEXT("blue_activation_event"), S_(G(O, TEXT("BlueActivationEventName"))));
	D.Set(TEXT("area_volume_tag"), S_(G(O, TEXT("AreaVolumeTag"))));
	D.Set(TEXT("defense_script_tags"), S_(G(O, TEXT("DefenseScriptTags"))));
	D.Set(TEXT("properties"), PropsJ(O));
	return D;
}

void FConvMapExporter::ExtraMarker(FConvObject* O, const FConvJson& Rec, FConvJson& M)
{
	auto Add = [&M](const TCHAR* Kind, FConvJson D) { M.Find(Kind)->Add(MoveTemp(D)); };
	auto ObjectiveTags = [&]()
	{
		FConvJson Out = FConvJson::Array();
		for (const auto* KV : SortedIndexed(O->Props(), TEXT("ObjectiveTag")))
		{
			FConvJson N = S_(&KV->Value);
			if (N.Truthy()) Out.Add(N);
		}
		return Out;
	};
	if (O->IsA(TEXT("SVehicleFactory")))
	{
		const bool bOns = O->IsA(TEXT("ONSVehicleFactory"));
		const bool bAssault = O->IsA(TEXT("ASVehicleFactory"));
		const FConvProp* Vc = G(O, TEXT("VehicleClass"));
		const bool bVc = Vc && Vc->IsRef() && Vc->Ref.IsSet();
		FConvJson Team, Respawn;
		if (bAssault)
		{
			Team = GJ(O, TEXT("VehicleTeam"), FConvJson(1));
			Respawn = GJ(O, TEXT("RespawnDelay"), FConvJson(3.0));
		}
		else
		{
			Team = GJ(O, TEXT("TeamNum"), FConvJson(0));
			Respawn = GJ(O, TEXT("RespawnTime"), FConvJson(15.0));
		}
		FConvJson D = Base(Rec);
		D.Set(TEXT("kind"), bOns ? TEXT("onslaught") : bAssault ? TEXT("assault") : TEXT("other"));
		D.Set(TEXT("vehicle_class"), bVc ? ClassRefInfo(Vc) : FConvJson());
		D.Set(TEXT("vehicle_class_name"), bVc ? FConvJson(Vc->Ref.Name()) : S_(G(O, TEXT("VehicleClassStr"))));
		D.Set(TEXT("vehicle_class_str"), S_(G(O, TEXT("VehicleClassStr"))));
		D.Set(TEXT("team"), Team);
		D.Set(TEXT("team_name"), TeamName(Team.Type == FConvJson::EType::Int ? (int32)Team.I : -999999));
		if (Team.Type != FConvJson::EType::Int)
		{
			TArray<ANSICHAR> Buf;
			Team.Write(Buf, -1);
			Buf.Add(0);
			D.Set(TEXT("team_name"), FString(ANSI_TO_TCHAR(Buf.GetData())));
		}
		D.Set(TEXT("respawn_time"), Respawn);
		D.Set(TEXT("max_vehicle_count"), GJ(O, TEXT("MaxVehicleCount"), FConvJson(1)));
		D.Set(TEXT("tag"), RecS(Rec.Get(TEXT("tag"))));
		D.Set(TEXT("event"), S_(G(O, TEXT("Event"))));
		if (bOns)
		{
			D.Set(TEXT("active"), GB(O, TEXT("bActive"), false));
			D.Set(TEXT("never_activate"), GB(O, TEXT("bNeverActivate"), false));
			D.Set(TEXT("reverse_blue_team_direction"), GB(O, TEXT("bReverseBlueTeamDirection"), false));
			D.Set(TEXT("pre_spawn"), GB(O, TEXT("bPreSpawn"), false));
		}
		if (bAssault)
		{
			D.Set(TEXT("enabled"), GB(O, TEXT("bEnabled"), true));
			D.Set(TEXT("vehicle_health"), GJ(O, TEXT("VehicleHealth")));
			D.Set(TEXT("respawn_when_destroyed"), GB(O, TEXT("bRespawnWhenDestroyed"), false));
			D.Set(TEXT("team_locked"), GB(O, TEXT("bVehicleTeamLock"), false));
			D.Set(TEXT("key_vehicle"), GB(O, TEXT("bKeyVehicle"), false));
			D.Set(TEXT("triggered_function"), GJ(O, TEXT("TriggeredFunction")));
			D.Set(TEXT("vehicle_tag"), S_(G(O, TEXT("VehicleTag"))));
			D.Set(TEXT("vehicle_event"), S_(G(O, TEXT("VehicleEvent"))));
			D.Set(TEXT("vehicle_possessed_event"), S_(G(O, TEXT("VehiclePossessedEvent"))));
			D.Set(TEXT("next_factory_tag"), S_(G(O, TEXT("NextFactoryTag"))));
			D.Set(TEXT("objective_tags"), ObjectiveTags());
		}
		D.Set(TEXT("properties"), PropsJ(O));
		Add(TEXT("vehicle_factories"), D);
	}
	else if (O->IsA(TEXT("Vehicle")) || O->IsA(TEXT("ONSWeaponPawn")))
	{
		FConvJson D = Base(Rec);
		D.Set(TEXT("team"), GJ(O, TEXT("Team")));
		FConvJson Chain = FConvJson::Array();
		for (const FString& C : O->ClassChain()) Chain.Add(C);
		D.Set(TEXT("class_chain"), Chain);
		D.Set(TEXT("tag"), RecS(Rec.Get(TEXT("tag"))));
		D.Set(TEXT("properties"), PropsJ(O));
		Add(TEXT("vehicles"), D);
	}
	if (O->IsA(TEXT("ONSPowerCore")))
	{
		Add(O->IsA(TEXT("ONSPowerNode")) ? TEXT("power_nodes") : TEXT("power_cores"), CoreFields(O, Rec));
	}
	if (O->IsA(TEXT("ONSPowerLinkOfficialSetup"), TEXT("ONSPowerLinkCustomSetup"), TEXT("ONSPowerLinkSetup")))
	{
		FConvJson Links = FConvJson::Array();
		const FConvProp* Ls = G(O, TEXT("LinkSetups"));
		if (Ls && Ls->IsArray())
		{
			for (const FConvProp& St : Ls->A)
			{
				if (!St.IsStruct()) continue;
				FConvJson L = FConvJson::Object();
				L.Set(TEXT("base_node"), S_(St.Find(TEXT("BaseNode"))));
				L.Set(TEXT("linked_nodes"), Names_(St.Find(TEXT("LinkedNodes"))));
				Links.Add(L);
			}
		}
		FConvJson D = Base(Rec);
		D.Set(TEXT("setup_name"), GJ(O, TEXT("SetupName"), O->IsA(TEXT("ONSPowerLinkOfficialSetup")) ? FConvJson(TEXT("Default")) : FConvJson()));
		D.Set(TEXT("custom"), O->IsA(TEXT("ONSPowerLinkCustomSetup")));
		D.Set(TEXT("links"), Links);
		Add(TEXT("power_link_setups"), D);
	}
	if (O->IsA(TEXT("PlayerSpawnManager")))
	{
		FConvJson D = Base(Rec);
		D.Set(TEXT("tag"), RecS(Rec.Get(TEXT("tag"))));
		D.Set(TEXT("enabled"), GB(O, TEXT("bEnabled"), true));
		D.Set(TEXT("player_start_team"), GJ(O, TEXT("PlayerStartTeam"), FConvJson(0)));
		D.Set(TEXT("assault_team"), GJ(O, TEXT("AssaultTeam"), FConvJson(0)));
		D.Set(TEXT("allow_teleporting"), GB(O, TEXT("bAllowTeleporting"), true));
		D.Set(TEXT("disabled_vehicle_factories_tags"), Names_(G(O, TEXT("DisabledVehicleFactoriesTag"))));
		D.Set(TEXT("forced_pawn_class"), RefJ(G(O, TEXT("ForcedPawnClass"))));
		D.Set(TEXT("override_pawn_class"), GJ(O, TEXT("OverridePawnClass")));
		Add(TEXT("spawn_managers"), D);
	}
	if (O->IsA(TEXT("InvasionPoint")) || O->ClassName().ToLower().Contains(TEXT("invasion")))
	{
		FConvJson D = Base(Rec);
		D.Set(TEXT("tag"), RecS(Rec.Get(TEXT("tag"))));
		D.Set(TEXT("properties"), PropsJ(O));
		Add(TEXT("invasion_spawns"), D);
	}
	if (O->IsA(TEXT("Monster")))
	{
		FConvJson D = Base(Rec);
		D.Set(TEXT("tag"), RecS(Rec.Get(TEXT("tag"))));
		FConvJson Chain = FConvJson::Array();
		for (const FString& C : O->ClassChain()) Chain.Add(C);
		D.Set(TEXT("class_chain"), Chain);
		Add(TEXT("monsters"), D);
	}
	const FString Cn = O->ClassName().ToLower();
	if (O->IsA(TEXT("Triggers"), TEXT("ScriptedTrigger"), TEXT("DestroyableTrigger"), TEXT("ASCriticalObjectiveVolume"))
		|| (Cn.Contains(TEXT("trigger")) && !O->IsA(TEXT("NavigationPoint"))))
	{
		FConvJson D = Base(Rec);
		D.Set(TEXT("tag"), RecS(Rec.Get(TEXT("tag"))));
		D.Set(TEXT("event"), S_(G(O, TEXT("Event"))));
		FConvJson Chain = FConvJson::Array();
		for (const FString& C : O->ClassChain()) Chain.Add(C);
		D.Set(TEXT("class_chain"), Chain);
		D.Set(TEXT("initially_active"), GB(O, TEXT("bInitiallyActive"), true));
		D.Set(TEXT("trigger_type"), GJ(O, TEXT("TriggerType")));
		D.Set(TEXT("initial_state"), S_(G(O, TEXT("InitialState"))));
		D.Set(TEXT("message"), GJ(O, TEXT("Message")));
		D.Set(TEXT("objective_tags"), ObjectiveTags());
		D.Set(TEXT("team"), G(O, TEXT("Team")) ? GJ(O, TEXT("Team")) : GJ(O, TEXT("TeamNum")));
		D.Set(TEXT("health"), GJ(O, TEXT("Health")));
		D.Set(TEXT("collision_radius"), Rec.Get(TEXT("collision_radius")));
		D.Set(TEXT("collision_height"), Rec.Get(TEXT("collision_height")));
		D.Set(TEXT("properties"), PropsJ(O));
		if (O->IsA(TEXT("ScriptedSequence")))
		{
			FConvJson Acts = FConvJson::Array();
			TArray<FString> Outs;
			const FConvProp* A = G(O, TEXT("Actions"));
			if (A && A->IsArray())
			{
				for (const FConvProp& R : A->A)
				{
					if (!R.IsRef() || !R.Ref.IsSet()) continue;
					FConvObject* Ao = R.Ref.Load();
					if (!Ao) continue;
					FConvJson Aj = FConvJson::Object();
					Aj.Set(TEXT("class"), Ao->ClassName());
					Aj.Set(TEXT("properties"), Ex.JsonifyProps(Ao->Props()));
					Acts.Add(Aj);
					for (const TPair<FString, FConvProp>& KV : Ao->Props())
					{
						const FString Kl = KV.Key.ToLower();
						if (KV.Value.Kind == FConvProp::EKind::Str && !KV.Value.S.IsEmpty() && KV.Value.S != TEXT("None")
							&& (Kl.Contains(TEXT("event")) || Kl.Contains(TEXT("tag"))))
						{
							Outs.Add(KV.Value.S);
						}
					}
				}
			}
			D.Set(TEXT("actions"), Acts);
			ActionOuts.Add(Rec.GetString(TEXT("name")), Outs);
		}
		Add(TEXT("triggers"), D);
	}
}

void FConvMapExporter::PostMarkers(FConvJson& M)
{
	// --- Onslaught links
	TArray<FConvJson*> Cores;
	for (FConvJson& C : M.Find(TEXT("power_cores"))->A) Cores.Add(&C);
	for (FConvJson& C : M.Find(TEXT("power_nodes"))->A) Cores.Add(&C);
	TSet<FString> Names;
	for (FConvJson* C : Cores) Names.Add(C->GetString(TEXT("actor")));
	TSet<TPair<FString, FString>> Pairs;
	FConvJson Source;
	auto AddPair = [&Pairs](const FString& A, const FString& B)
	{
		Pairs.Add(CmpStr(A, B) <= 0 ? TPair<FString, FString>(A, B) : TPair<FString, FString>(B, A));
	};
	auto AddSetup = [&](const FConvJson& Setup)
	{
		for (const FConvJson& L : Setup.Get(TEXT("links")).A)
		{
			for (const FConvJson& N : L.Get(TEXT("linked_nodes")).A)
			{
				const FConvJson& Bn = L.Get(TEXT("base_node"));
				if (Bn.Truthy() && N.Truthy()) AddPair(Bn.S, N.S);
			}
		}
	};
	FConvJson& Setups = *M.Find(TEXT("power_link_setups"));
	const FConvJson* Default = nullptr;
	for (const FConvJson& Setup : Setups.A)
	{
		const FConvJson& Sn = Setup.Get(TEXT("setup_name"));
		FString Name_ = Sn.Truthy() && Sn.Type == FConvJson::EType::String ? Sn.S : FString(TEXT("Default"));
		if (!Setup.Get(TEXT("custom")).B && Name_.ToLower() == TEXT("default")) { Default = &Setup; break; }
	}
	if (Default)
	{
		Source = FString::Printf(TEXT("ONSPowerLinkOfficialSetup:%s"), *Default->GetString(TEXT("actor")));
		AddSetup(*Default);
	}
	if (!Pairs.Num())
	{
		for (FConvJson* C : Cores)
		{
			for (const FConvJson& N : C->Get(TEXT("linked_nodes")).A) AddPair(C->GetString(TEXT("actor")), N.S);
		}
		if (Pairs.Num()) Source = TEXT("LinkedNodes");
	}
	if (!Pairs.Num() && Setups.A.Num())
	{
		const FConvJson& S0 = Setups.A[0];
		Source = FString::Printf(TEXT("%s:%s"), *S0.GetString(TEXT("class")), *S0.GetString(TEXT("actor")));
		AddSetup(S0);
	}
	TArray<TPair<FString, FString>> Sorted = Pairs.Array();
	Sorted.Sort([](const TPair<FString, FString>& A, const TPair<FString, FString>& B)
	{
		const int32 C = CmpStr(A.Key, B.Key);
		return C != 0 ? C < 0 : CmpStr(A.Value, B.Value) < 0;
	});
	for (const auto& P : Sorted)
	{
		FConvJson L = FConvJson::Object();
		L.Set(TEXT("a"), P.Key);
		L.Set(TEXT("b"), P.Value);
		L.Set(TEXT("source"), Source);
		L.Set(TEXT("valid"), Names.Contains(P.Key) && Names.Contains(P.Value));
		M.Find(TEXT("power_links"))->Add(L);
	}
	for (FConvJson& F : M.Find(TEXT("vehicle_factories"))->A)
	{
		if (F.GetString(TEXT("kind")) != TEXT("onslaught") || !Cores.Num()) continue;
		const FConvJson& Fl = F.Get(TEXT("location"));
		FConvJson* Best = nullptr;
		double BestD = 0;
		for (FConvJson* C : Cores)
		{
			const FConvJson& Cl = C->Get(TEXT("location"));
			double D = 0;
			for (int32 i = 0; i < 3; ++i) D += FMath::Square(Cl.A[i].AsNumber() - Fl.A[i].AsNumber());
			if (!Best || D < BestD) { Best = C; BestD = D; }
		}
		F.Set(TEXT("nearest_power_node"), Best->Get(TEXT("actor")));
		F.Set(TEXT("nearest_power_node_distance"), FConvJson::RoundTo(FMath::Sqrt(BestD), 2));
	}
	// --- Assault stage order
	TArray<const FConvJson*> Objs;
	for (const FConvJson& O : M.Find(TEXT("game_objectives"))->A)
	{
		const FString T = O.GetString(TEXT("objective_type"));
		if (T != TEXT("power_core") && T != TEXT("power_node")) Objs.Add(&O);
	}
	auto Prio = [](const FConvJson& O) { const FConvJson& P = O.Get(TEXT("defense_priority")); return P.Truthy() ? P.AsNumber() : 0.0; };
	TArray<double> Prios;
	for (const FConvJson* O : Objs) if (!O->Get(TEXT("optional")).Truthy()) Prios.AddUnique(Prio(*O));
	Prios.Sort([](double A, double B) { return A > B; });
	FConvJson& Ao = *M.Find(TEXT("assault_objectives"));
	for (const FConvJson* O : Objs)
	{
		FConvJson A = *O;
		const int32 Idx = Prios.IndexOfByKey(Prio(*O));
		A.Set(TEXT("order"), Idx != INDEX_NONE ? FConvJson(Idx + 1) : FConvJson());
		A.Set(TEXT("order_note"), TEXT("stage rank by DefensePriority (highest first); optional objectives have no stage"));
		Ao.Add(A);
	}
	Algo::StableSort(Ao.A, [](const FConvJson& A, const FConvJson& B)
	{
		const bool An = A.Get(TEXT("order")).IsNull(), Bn = B.Get(TEXT("order")).IsNull();
		if (An != Bn) return !An;
		const double Ao_ = An ? 0 : A.Get(TEXT("order")).AsNumber(), Bo = Bn ? 0 : B.Get(TEXT("order")).AsNumber();
		if (Ao_ != Bo) return Ao_ < Bo;
		return CmpStr(A.GetString(TEXT("actor")), B.GetString(TEXT("actor"))) < 0;
	});
	// --- event graph
	struct FNode { FConvJson* T; TSet<FString> Ins, Outs; };
	TArray<FNode> Nodes;
	for (const TCHAR* Kind : { TEXT("triggers"), TEXT("movers"), TEXT("vehicle_factories"), TEXT("spawn_managers") })
	{
		for (FConvJson& T : M.Find(Kind)->A)
		{
			FNode N;
			N.T = &T;
			const FConvJson& Tag = T.Get(TEXT("tag"));
			if (Tag.Truthy() && Tag.Type == FConvJson::EType::String && Tag.S.ToLower() != T.GetString(TEXT("class")).ToLower()) N.Ins.Add(Tag.S.ToLower());
			const FConvJson& Ev = T.Get(TEXT("event"));
			if (Ev.Truthy() && Ev.Type == FConvJson::EType::String) N.Outs.Add(Ev.S.ToLower());
			for (const FConvJson& X : T.Get(TEXT("objective_tags")).A) if (X.Type == FConvJson::EType::String) N.Outs.Add(X.S.ToLower());
			for (const TPair<FString, FConvJson>& KV : T.Get(TEXT("trigger_events")).O) if (KV.Value.Truthy()) N.Outs.Add(KV.Value.S.ToLower());
			if (FCString::Strcmp(Kind, TEXT("triggers")) == 0)
			{
				if (const TArray<FString>* Ao2 = ActionOuts.Find(T.GetString(TEXT("actor"))))
				{
					for (const FString& X : *Ao2) N.Outs.Add(X.ToLower());
				}
			}
			Nodes.Add(MoveTemp(N));
		}
	}
	TSet<FString> Rel;
	TArray<const FConvJson*> RelSrc;
	for (const FConvJson& O : M.Find(TEXT("game_objectives"))->A) RelSrc.Add(&O);
	for (FConvJson* C : Cores) RelSrc.Add(C);
	for (const FConvJson* O : RelSrc)
	{
		for (const TCHAR* K : { TEXT("tag"), TEXT("event"), TEXT("destroyed_event"), TEXT("constructed_event"), TEXT("physical_actors_tag"),
			TEXT("damage_event"), TEXT("critical_volume_tag") })
		{
			const FConvJson& V = O->Get(K);
			if (V.Truthy() && V.Type == FConvJson::EType::String) Rel.Add(V.S.ToLower());
		}
	}
	for (int32 It = 0; It < 8; ++It)
	{
		bool bGrown = false;
		for (FNode& N : Nodes)
		{
			if (N.T->Get(TEXT("objective_relevant")).Truthy()) continue;
			if (N.Ins.Intersect(Rel).Num() || N.Outs.Intersect(Rel).Num())
			{
				N.T->Set(TEXT("objective_relevant"), true);
				Rel.Append(N.Ins);
				Rel.Append(N.Outs);
				bGrown = true;
			}
		}
		if (!bGrown) break;
	}
	TMap<FString, TArray<FString>> ByEvent;
	for (FNode& N : Nodes)
	{
		if (!N.T->Has(TEXT("objective_relevant"))) N.T->Set(TEXT("objective_relevant"), false);
		for (const FString& E : N.Outs) ByEvent.FindOrAdd(E).Add(N.T->GetString(TEXT("actor")));
	}
	for (FNode& N : Nodes)
	{
		TSet<FString> By;
		const FString Me = N.T->GetString(TEXT("actor"));
		for (const FString& I : N.Ins)
		{
			if (const TArray<FString>* A = ByEvent.Find(I))
			{
				for (const FString& X : *A) if (X != Me) By.Add(X);
			}
		}
		TArray<FString> Sorted2 = By.Array();
		Sorted2.Sort([](const FString& A, const FString& B) { return CmpStr(A, B) < 0; });
		FConvJson Tb = FConvJson::Array();
		for (const FString& X : Sorted2) Tb.Add(X);
		N.T->Set(TEXT("triggered_by"), Tb);
	}
}

FConvJson FConvMapExporter::Volume(FConvObject* O, const FConvJson& Rec)
{
	const FConvProps& P = O->Props();
	FConvJson D = Base(Rec);
	D.Set(TEXT("physics"), O->IsA(TEXT("PhysicsVolume")));
	D.Set(TEXT("water"), GB(O, TEXT("bWaterVolume"), false));
	D.Set(TEXT("pain_causing"), GB(O, TEXT("bPainCausing"), false));
	D.Set(TEXT("damage_per_sec"), GJ(O, TEXT("DamagePerSec"), FConvJson(0.0)));
	D.Set(TEXT("kill_volume"), O->IsA(TEXT("xFallingVolume")) || O->ClassName().ToLower().Contains(TEXT("kill")));
	const FConvProp* B = ConvFindProp(P, TEXT("Brush"));
	if (B && B->Truthy())
	{
		FConvObject* Mo = B->IsRef() ? B->Ref.Load() : nullptr;
		if (!Mo)
		{
			D.Set(TEXT("bounds_error"), TEXT("'NoneType' object has no attribute 'native_reader'"));
			return D;
		}
		FConvReader R = Mo->NativeReader();
		R.Skip(25 + 16);
		const int32 Nvec = R.Index();
		R.Skip(int64(Nvec) * 12);
		const int32 Npt = R.Index();
		const float* Pts = (const float*)R.Bytes(int64(FMath::Max(Npt, 0)) * 12);
		if (R.bError || Npt < 0)
		{
			D.Set(TEXT("bounds_error"), R.Error);
			return D;
		}
		if (Npt)
		{
			auto Vec = [&P](const TCHAR* K, double Def, double Out[3])
			{
				Out[0] = Out[1] = Out[2] = Def;
				const FConvProp* V = ConvFindProp(P, K);
				if (V && V->Kind == FConvProp::EKind::Vector) { Out[0] = V->V[0]; Out[1] = V->V[1]; Out[2] = V->V[2]; }
			};
			auto ScaleOf = [&P](const TCHAR* K, double Out[3])
			{
				Out[0] = Out[1] = Out[2] = 1.0;
				const FConvProp* V = ConvFindProp(P, K);
				if (V && V->IsStruct())
				{
					const FConvProp* Sc = V->Find(TEXT("Scale"));
					if (Sc && Sc->Kind == FConvProp::EKind::Vector) { Out[0] = Sc->V[0]; Out[1] = Sc->V[1]; Out[2] = Sc->V[2]; }
				}
			};
			double Pre[3], Ms[3], Ps[3], Loc[3], Rm[3][3];
			Vec(TEXT("PrePivot"), 0.0, Pre);
			Vec(TEXT("Location"), 0.0, Loc);
			ScaleOf(TEXT("MainScale"), Ms);
			ScaleOf(TEXT("PostScale"), Ps);
			int32 Rot[3] = { 0, 0, 0 };
			if (const FConvProp* Rp = ConvFindProp(P, TEXT("Rotation")))
			{
				if (Rp->Kind == FConvProp::EKind::Rotator) { Rot[0] = Rp->R[0]; Rot[1] = Rp->R[1]; Rot[2] = Rp->R[2]; }
			}
			RotRows(Rot, Rm);
			// brush space -> world centimetres
			auto ToWorld = [&](const double In[3], double W[3])
			{
				double V[3];
				for (int32 c = 0; c < 3; ++c) V[c] = (In[c] - Pre[c]) * Ms[c];
				for (int32 c = 0; c < 3; ++c) W[c] = ((V[0] * Rm[0][c] + V[1] * Rm[1][c] + V[2] * Rm[2][c]) * Ps[c] + Loc[c]) * S;
			};
			double Mn[3], Mx[3];
			for (int32 i = 0; i < Npt; ++i)
			{
				const double In[3] = { Pts[i * 3], Pts[i * 3 + 1], Pts[i * 3 + 2] };
				double W[3];
				ToWorld(In, W);
				for (int32 c = 0; c < 3; ++c)
				{
					if (i == 0) Mn[c] = Mx[c] = W[c];
					Mn[c] = FMath::Min(Mn[c], W[c]);
					Mx[c] = FMath::Max(Mx[c], W[c]);
				}
			}
			FConvJson Bw = FConvJson::Object(), A = FConvJson::Array(), C = FConvJson::Array();
			for (int32 c = 0; c < 3; ++c) { A.Add(FConvJson::RoundTo(Mn[c], 3)); C.Add(FConvJson::RoundTo(Mx[c], 3)); }
			Bw.Set(TEXT("min"), A);
			Bw.Set(TEXT("max"), C);
			D.Set(TEXT("bounds_world"), Bw);
			D.Set(TEXT("brush_point_count"), Npt);

			// the brush's own BSP: node polygons (faces) and the tree (UModel::PointCheck: inside = ends behind a leaf plane)
			const int32 Nn = R.Index();
			struct FBNode { double Plane[4]; int32 VPool, Back, Front, NVerts; };
			TArray<FBNode> BNodes;
			if (!R.bError && Nn > 0 && Nn <= 20000)
			{
				BNodes.SetNumUninitialized(Nn);
				for (int32 i = 0; i < Nn; ++i)
				{
					float Pl[4];
					R.Raw(Pl, 16);
					for (int32 c = 0; c < 4; ++c) BNodes[i].Plane[c] = Pl[c];
					R.U64();
					R.U8();
					BNodes[i].VPool = R.Index();
					R.Index();
					BNodes[i].Back = R.Index();
					BNodes[i].Front = R.Index();
					R.Index(); R.Index(); R.Index();
					R.Skip(16);
					R.U8(); R.U8();
					BNodes[i].NVerts = R.U8();
					R.Skip(20);
				}
				const int32 Ns = R.Index();
				for (int32 i = 0; i < Ns && !R.bError && Ns <= 20000; ++i)
				{
					R.Index(); R.U32(); R.Index(); R.Index(); R.Index(); R.Index(); R.Index(); R.Index(); R.Skip(16); R.F32();
				}
				const int32 Nv = R.Index();
				TArray<int32> Verts;
				if (!R.bError && Ns >= 0 && Ns <= 20000 && Nv >= 0 && Nv <= 200000)
				{
					Verts.SetNumUninitialized(Nv);
					for (int32 i = 0; i < Nv; ++i)
					{
						Verts[i] = R.Index();
						R.Index();
					}
				}
				if (!R.bError && Verts.Num() > 0)
				{
					FConvJson Faces = FConvJson::Array(), Planes = FConvJson::Array(), Backs = FConvJson::Array(), Fronts = FConvJson::Array();
					bool bOk = true;
					for (const FBNode& N : BNodes)
					{
						if (N.NVerts < 3 || N.VPool < 0 || N.VPool + N.NVerts > Verts.Num()) { bOk = false; break; }
						TArray<double, TInlineAllocator<48>> Wp;
						double Cen[3] = { 0, 0, 0 };
						for (int32 k = 0; k < N.NVerts; ++k)
						{
							const int32 Pi = Verts[N.VPool + k];
							if (Pi < 0 || Pi >= Npt) { bOk = false; break; }
							const double In[3] = { Pts[Pi * 3], Pts[Pi * 3 + 1], Pts[Pi * 3 + 2] };
							double W[3];
							ToWorld(In, W);
							for (int32 c = 0; c < 3; ++c) { Wp.Add(W[c]); Cen[c] += In[c] / N.NVerts; }
						}
						if (!bOk) break;
						// world plane: Newell normal of the transformed polygon, oriented like the brush-space plane
						double Nw[3] = { 0, 0, 0 };
						for (int32 k = 0; k < N.NVerts; ++k)
						{
							const double* Pa = &Wp[k * 3];
							const double* Pb = &Wp[((k + 1) % N.NVerts) * 3];
							Nw[0] += (Pa[1] - Pb[1]) * (Pa[2] + Pb[2]);
							Nw[1] += (Pa[2] - Pb[2]) * (Pa[0] + Pb[0]);
							Nw[2] += (Pa[0] - Pb[0]) * (Pa[1] + Pb[1]);
						}
						const double Len = FMath::Sqrt(Nw[0] * Nw[0] + Nw[1] * Nw[1] + Nw[2] * Nw[2]);
						if (Len < 1e-9) { bOk = false; break; }
						for (int32 c = 0; c < 3; ++c) Nw[c] /= Len;
						const double Out[3] = { Cen[0] + N.Plane[0] * 8.0, Cen[1] + N.Plane[1] * 8.0, Cen[2] + N.Plane[2] * 8.0 };
						double OutW[3];
						ToWorld(Out, OutW);
						double Wd = Nw[0] * Wp[0] + Nw[1] * Wp[1] + Nw[2] * Wp[2];
						if (Nw[0] * OutW[0] + Nw[1] * OutW[1] + Nw[2] * OutW[2] - Wd < 0.0)
						{
							for (int32 c = 0; c < 3; ++c) Nw[c] = -Nw[c];
							Wd = -Wd;
						}
						FConvJson F = FConvJson::Array();
						for (const double X : Wp) F.Add(FConvJson::RoundTo(X, 3));
						Faces.Add(F);
						FConvJson Pl = FConvJson::Array();
						for (int32 c = 0; c < 3; ++c) Pl.Add(FConvJson::RoundTo(Nw[c], 6));
						Pl.Add(FConvJson::RoundTo(Wd, 3));
						Planes.Add(Pl);
						Backs.Add(N.Back);
						Fronts.Add(N.Front);
					}
					if (bOk && BNodes.Num() > 0)
					{
						bool bConvex = true;
						for (int32 i = 0; i < BNodes.Num(); ++i)
						{
							bConvex &= BNodes[i].Front < 0 && BNodes[i].Back == (i + 1 < BNodes.Num() ? i + 1 : -1);
						}
						FConvJson Br = FConvJson::Object();
						Br.Set(TEXT("faces"), Faces);
						Br.Set(TEXT("planes"), Planes);
						Br.Set(TEXT("back"), Backs);
						Br.Set(TEXT("front"), Fronts);
						Br.Set(TEXT("convex"), bConvex);
						Br.Set(TEXT("note"), TEXT("world cm; faces are flat [x,y,z,...] polygons; a point is inside when the walk from node 0 (front if dot(n,p)-w >= 0) leaves through a back (-1)"));
						D.Set(TEXT("brush"), Br);
					}
				}
			}
		}
	}
	return D;
}

FConvJson FConvMapExporter::Terrain(FConvObject* O, const FConvJson& Rec)
{
	const FConvProps& P = O->Props();
	FConvJson Layers = FConvJson::Array();
	for (const auto* KV : SortedIndexed(P, TEXT("Layers")))
	{
		const FConvProp& V = KV->Value;
		const FConvProp* T = V.IsStruct() ? V.Find(TEXT("Texture")) : nullptr;
		if (!T || !T->Truthy()) continue;
		FConvJson L = FConvJson::Object();
		L.Set(TEXT("texture"), RefJ(T));
		const FConvProp* Am = V.Find(TEXT("AlphaMap"));
		L.Set(TEXT("alpha_map"), Am && Am->Truthy() ? RefJ(Am) : FConvJson());
		const FConvProp* Us = V.Find(TEXT("UScale"));
		const FConvProp* Vs = V.Find(TEXT("VScale"));
		L.Set(TEXT("u_scale"), Us ? Ex.Jsonify(*Us) : FConvJson(1.0));
		L.Set(TEXT("v_scale"), Vs ? Ex.Jsonify(*Vs) : FConvJson(1.0));
		Layers.Add(L);
	}
	const FConvProp* Hm = ConvFindProp(P, TEXT("TerrainMap"));
	const bool bHm = Hm && Hm->IsRef() && Hm->Truthy();
	if (bHm) Dep(Hm->Ref);
	const FConvProp* Sc = G(O, TEXT("TerrainScale"));
	float SV[3] = { 64.f, 64.f, 64.f };
	if (Sc && Sc->Kind == FConvProp::EKind::Vector) { SV[0] = Sc->V[0]; SV[1] = Sc->V[1]; SV[2] = Sc->V[2]; }
	FConvJson D = Base(Rec);
	const FString HmPath = bHm ? Ex.Canon(Hm->Ref) : FString();
	D.Set(TEXT("heightmap"), FConvJson::StrOrNull(HmPath));
	D.Set(TEXT("heightmap_file"), bHm ? FConvJson::StrOrNull(Ex.FileFor(HmPath, TEXT("textures"))) : FConvJson());
	D.Set(TEXT("terrain_scale"), Sv(SV));
	FConvJson Raw = FConvJson::Array();
	for (int32 i = 0; i < 3; ++i) Raw.Add(FConvJson::RoundTo(SV[i], 6));
	D.Set(TEXT("terrain_scale_raw"), Raw);
	D.Set(TEXT("layers"), Layers);
	D.Set(TEXT("note"), TEXT("UE2 terrain: vertex(x,y) = location + ((x - W/2) * sx, (y - H/2) * sy, (h - 32767) * sz / 256); quads split along (x,y)-(x+1,y+1), the other diagonal where EdgeTurnBitmap is set; heightmap is a 16-bit PNG"));
	return D;
}

FConvJson FConvMapExporter::LevelInfo(FConvPackage* Pkg, const TArray<FConvObject*>& Objs, FConvJson& OutLevel, FConvJson& OutKillZ)
{
	FConvJson Summary = FConvJson::Object();
	const TArray<int32> Si = Pkg->ExportsOfClass({ TEXT("LevelSummary") });
	if (Si.Num())
	{
		FConvObject* So = Pkg->GetObject(Si[0]);
		auto PJ = [&](const TCHAR* K, FConvJson Def) { const FConvProp* V = So->Prop(K); return V ? Ex.Jsonify(*V) : Def; };
		Summary.Set(TEXT("title"), PJ(TEXT("Title"), FConvJson(TEXT(""))));
		Summary.Set(TEXT("author"), PJ(TEXT("Author"), FConvJson(TEXT(""))));
		Summary.Set(TEXT("description"), PJ(TEXT("Description"), FConvJson(TEXT(""))));
		FConvJson Ideal = FConvJson::Array();
		Ideal.Add(PJ(TEXT("IdealPlayerCountMin"), FConvJson()));
		Ideal.Add(PJ(TEXT("IdealPlayerCountMax"), FConvJson()));
		Summary.Set(TEXT("ideal_player_count"), Ideal);
		const FConvProp* Ss = So->Prop(TEXT("Screenshot"));
		Summary.Set(TEXT("screenshot"), Ss && Ss->Truthy() ? RefJ(Ss) : FConvJson());
	}
	FConvObject* Li = nullptr;
	for (FConvObject* O : Objs)
	{
		if (O->IsA(TEXT("LevelInfo"))) { Li = O; break; }
	}
	OutLevel = FConvJson();
	OutKillZ = FConvJson();
	if (Li)
	{
		auto PJ = [&](const TCHAR* K, FConvJson Def) { const FConvProp* V = Li->Prop(K); return V ? Ex.Jsonify(*V) : Def; };
		FConvJson L = FConvJson::Object();
		L.Set(TEXT("title"), PJ(TEXT("Title"), FConvJson(TEXT(""))));
		L.Set(TEXT("author"), PJ(TEXT("Author"), FConvJson(TEXT(""))));
		L.Set(TEXT("song"), PJ(TEXT("Song"), FConvJson()));
		L.Set(TEXT("default_game_type"), PJ(TEXT("DefaultGameType"), FConvJson()));
		L.Set(TEXT("kill_z_raw"), PJ(TEXT("KillZ"), FConvJson()));
		L.Set(TEXT("soft_kill_z"), GB(Li, TEXT("bSoftKillZ"), false));
		L.Set(TEXT("stall_z_raw"), PJ(TEXT("StallZ"), FConvJson()));
		L.Set(TEXT("default_gravity"), GJ(Li, TEXT("DefaultGravity")));
		L.Set(TEXT("distance_fog"), GB(Li, TEXT("bDistanceFog"), false));
		L.Set(TEXT("fog_color"), GJ(Li, TEXT("DistanceFogColor")));
		L.Set(TEXT("fog_start"), GJ(Li, TEXT("DistanceFogStart")));
		L.Set(TEXT("fog_end"), GJ(Li, TEXT("DistanceFogEnd")));
		if (!Summary.Truthy())
		{
			Summary = FConvJson::Object();
			Summary.Set(TEXT("title"), L.Get(TEXT("title")));
			Summary.Set(TEXT("author"), L.Get(TEXT("author")));
		}
		if (const FConvProp* Kz = Li->Prop(TEXT("KillZ")))
		{
			FConvJson K = FConvJson::Object();
			K.Set(TEXT("z"), FConvJson::RoundTo(Kz->Num() * S, 4));
			K.Set(TEXT("z_raw"), Ex.Jsonify(*Kz));
			K.Set(TEXT("source"), Li->Name());
			OutKillZ = K;
		}
		OutLevel = L;
	}
	if (OutKillZ.IsNull())
	{
		bool bAny = false;
		double BestZ = 0;
		FString BestN;
		const FConvProp* BestP = nullptr;
		for (FConvObject* O : Objs)
		{
			if (!O->IsA(TEXT("ZoneInfo"))) continue;
			const FConvProp* Kz = O->Prop(TEXT("KillZ"));
			if (!Kz) continue;
			const double Z = Kz->Num();
			if (!bAny || Z < BestZ || (Z == BestZ && CmpStr(O->Name(), BestN) < 0))
			{
				bAny = true;
				BestZ = Z;
				BestN = O->Name();
				BestP = Kz;
			}
		}
		if (bAny)
		{
			FConvJson K = FConvJson::Object();
			K.Set(TEXT("z"), FConvJson::RoundTo(BestZ * S, 4));
			K.Set(TEXT("z_raw"), Ex.Jsonify(*BestP));
			K.Set(TEXT("source"), BestN);
			OutKillZ = K;
		}
	}
	return Summary;
}

FConvJson FConvMapExporter::ExportBsp(FConvPackage* Pkg, int32 ModelIdx, const FString& OutDir)
{
	FConvObject* Mo = Pkg->GetObject(ModelIdx - 1);
	FConvReader R = Mo->NativeReader();
	auto Fail = [&](const FString& Msg) { Ex.Error(FString::Printf(TEXT("%s BSP"), *Pkg->Name), Msg); return FConvJson(); };
	R.Skip(25 + 16);
	const int32 Nvec = R.Index();
	if (R.bError || Nvec < 0) return Fail(R.Error);
	TArray<double> Vecs;
	Vecs.SetNumUninitialized(int64(Nvec) * 3);
	{
		const float* F = (const float*)R.Bytes(int64(Nvec) * 12);
		if (!F) return Fail(R.Error);
		for (int64 i = 0; i < int64(Nvec) * 3; ++i) Vecs[i] = F[i];
	}
	const int32 Npt = R.Index();
	if (R.bError || Npt < 0) return Fail(R.Error);
	TArray<double> Pts;
	Pts.SetNumUninitialized(int64(Npt) * 3);
	{
		const float* F = (const float*)R.Bytes(int64(Npt) * 12);
		if (!F && Npt) return Fail(R.Error);
		for (int64 i = 0; i < int64(Npt) * 3; ++i) Pts[i] = F[i];
	}
	const int32 Nn = R.Index();
	if (R.bError || Nn < 0 || Nn > 4000000) return Fail(TEXT("bad node count"));
	struct FNodeR { double Plane[4]; int32 VPool, Surf, NVerts, Back, Front; uint8 Zone[2]; };
	TArray<FNodeR> Nodes;
	Nodes.SetNumUninitialized(Nn);
	for (int32 i = 0; i < Nn; ++i)
	{
		float Pl[4];
		R.Raw(Pl, 16);
		for (int32 c = 0; c < 4; ++c) Nodes[i].Plane[c] = Pl[c];
		R.U64();
		R.U8();
		Nodes[i].VPool = R.Index();
		Nodes[i].Surf = R.Index();
		Nodes[i].Back = R.Index();
		Nodes[i].Front = R.Index();
		R.Index(); R.Index(); R.Index();
		R.Skip(16);
		Nodes[i].Zone[0] = R.U8();
		Nodes[i].Zone[1] = R.U8();
		Nodes[i].NVerts = R.U8();
		R.Skip(20);
		if (R.bError) return Fail(R.Error);
	}
	const int32 Ns = R.Index();
	if (R.bError || Ns < 0 || Ns > 4000000) return Fail(TEXT("bad surf count"));
	struct FSurfR { int32 Mat; uint32 Flags; int32 PBase, VNormal, VTu, VTv; };
	TArray<FSurfR> Surfs;
	Surfs.SetNumUninitialized(Ns);
	for (int32 i = 0; i < Ns; ++i)
	{
		FSurfR& Sf = Surfs[i];
		Sf.Mat = R.Index();
		Sf.Flags = R.U32();
		Sf.PBase = R.Index();
		Sf.VNormal = R.Index();
		Sf.VTu = R.Index();
		Sf.VTv = R.Index();
		R.Index();
		R.Index();
		R.Skip(16);
		R.F32();
		if (R.bError) return Fail(R.Error);
	}
	const int32 Nv = R.Index();
	if (R.bError || Nv < 0 || Nv > 20000000) return Fail(TEXT("bad vert count"));
	TArray<int32> Verts;
	Verts.SetNumUninitialized(Nv);
	for (int32 i = 0; i < Nv; ++i)
	{
		Verts[i] = R.Index();
		R.Index();
	}
	if (R.bError) return Fail(R.Error);
	// zones (FZoneProperties: ZoneActor, Connectivity, Visibility, LastRenderTime); optional, the tree is skipped on errors
	TArray<FConvObject*> ZoneActors;
	TArray<bool> SkyZone;
	{
		R.I32(); // NumSharedSides
		const int32 Nz = R.I32();
		if (!R.bError && Nz > 0 && Nz <= 64)
		{
			for (int32 z = 0; z < Nz && !R.bError; ++z)
			{
				const int32 Za = R.Index();
				R.U64(); R.U64(); R.F32();
				FConvObject* ZoneObj = Za != 0 ? Pkg->MakeRef(Za).Load() : nullptr;
				ZoneActors.Add(ZoneObj);
				SkyZone.Add(ZoneObj && ZoneObj->IsA(TEXT("SkyZoneInfo")));
			}
		}
		if (R.bError)
		{
			ZoneActors.Reset();
			SkyZone.Reset();
		}
	}
	// validate
	if (Nn)
	{
		for (const FNodeR& N : Nodes)
		{
			if (N.VPool < 0 || N.VPool + N.NVerts > Nv) return Fail(TEXT("node vertex pool out of range"));
			if (N.Surf < 0 || N.Surf >= Ns) return Fail(TEXT("node surface index out of range"));
		}
		const int32 Step = FMath::Max(1, Nn / 500);
		int32 Bad = 0, Total = 0;
		for (int32 i = 0; i < Nn; i += Step)
		{
			const FNodeR& N = Nodes[i];
			for (int32 k = 0; k < N.NVerts; ++k)
			{
				const int32 Pi = Verts[N.VPool + k];
				if (!(Pi >= 0 && Pi < Npt)) return Fail(TEXT("vertex index out of range"));
				const double D = FMath::Abs(Pts[Pi * 3] * N.Plane[0] + Pts[Pi * 3 + 1] * N.Plane[1] + Pts[Pi * 3 + 2] * N.Plane[2] - N.Plane[3]);
				++Total;
				if (D > 2.0) ++Bad;
			}
		}
		if (Total && Bad > Total * 0.02)
		{
			return Fail(FString::Printf(TEXT("node vertices are not on their planes (%d/%d): unsupported layout"), Bad, Total));
		}
	}
	// triangulate
	struct FGroup { TArray<double> Pos, Nrm, Uv; TArray<uint32> Idx; int32 Count = 0; TSet<int32> Surfs; uint32 Flags = 0; FString Mat; bool bSky = false; bool bCollisionOnly = false; };
	TMap<FString, int32> GroupIndex;
	TArray<FGroup> Groups;
	TMap<int32, FString> MatCache;
	int32 SkippedInvisible = 0, SkippedDegenerate = 0, CollisionOnly = 0;
	for (int32 i = 0; i < Nn; ++i)
	{
		const FNodeR& N = Nodes[i];
		const int32 Cnt = N.NVerts;
		if (Cnt < 3) continue;
		const FSurfR& Sf = Surfs[N.Surf];
		// invisible but solid (an invisible wall or floor): collision only, outside the sky zone
		const bool bCollisionOnly = (Sf.Flags & SkipFlags) && !(Sf.Flags & NoCollisionFlags)
			&& !(SkyZone.IsValidIndex(N.Zone[1]) && SkyZone[N.Zone[1]]);
		if ((Sf.Flags & SkipFlags) && !bCollisionOnly) { ++SkippedInvisible; continue; }
		CollisionOnly += bCollisionOnly ? 1 : 0;
		// collision only primitives carry no material (and add no dependency)
		const int32 MatKey = bCollisionOnly ? 0 : Sf.Mat;
		FString* Mp = MatCache.Find(MatKey);
		if (!Mp)
		{
			FString P = TEXT("None");
			if (MatKey != 0)
			{
				const FConvRef Ref = Pkg->MakeRef(MatKey);
				Dep(Ref);
				P = Ex.Canon(Ref);
			}
			Mp = &MatCache.Add(MatKey, P);
		}
		const FString MatPath = *Mp;
		const uint32 Fl = bCollisionOnly ? Sf.Flags & uint32(ArenaConvLevelLocal::PF_Invisible | ArenaConvLevelLocal::PF_FakeBackdrop) : Sf.Flags & RenderFlags;
		// polygon
		TArray<double, TInlineAllocator<64>> Poly;
		for (int32 k = 0; k < Cnt; ++k)
		{
			const int32 Pi = Verts[N.VPool + k];
			Poly.Add(Pts[Pi * 3]); Poly.Add(Pts[Pi * 3 + 1]); Poly.Add(Pts[Pi * 3 + 2]);
		}
		double Normal[3];
		if (Sf.VNormal >= 0 && Sf.VNormal < Nvec) { for (int32 c = 0; c < 3; ++c) Normal[c] = Vecs[Sf.VNormal * 3 + c]; }
		else { for (int32 c = 0; c < 3; ++c) Normal[c] = N.Plane[c]; }
		double Nw[3] = { 0, 0, 0 };
		for (int32 k = 0; k < Cnt; ++k)
		{
			const double* A = &Poly[k * 3];
			const double* B = &Poly[((k + 1) % Cnt) * 3];
			const double Dx = A[0] - B[0], Dy = A[1] - B[1], Dz = A[2] - B[2];
			const double Sx = A[0] + B[0], Sy = A[1] + B[1], Sz = A[2] + B[2];
			Nw[0] += Dy * Sz;
			Nw[1] += Dz * Sx;
			Nw[2] += Dx * Sy;
		}
		if (FMath::Sqrt(Nw[0] * Nw[0] + Nw[1] * Nw[1] + Nw[2] * Nw[2]) < 1e-6) { ++SkippedDegenerate; continue; }
		// surfaces seen from a SkyZoneInfo's zone belong to the 3D sky box
		const bool bSky = SkyZone.IsValidIndex(N.Zone[1]) && SkyZone[N.Zone[1]];
		const FString Key = MatPath + FString::Printf(TEXT("|%u"), Fl) + (bSky ? TEXT("|sky") : TEXT("")) + (bCollisionOnly ? TEXT("|col") : TEXT(""));
		int32* Gi = GroupIndex.Find(Key);
		if (!Gi)
		{
			FGroup Gr;
			Gr.Flags = Fl;
			Gr.Mat = MatPath;
			Gr.bSky = bSky;
			Gr.bCollisionOnly = bCollisionOnly;
			Groups.Add(MoveTemp(Gr));
			Gi = &GroupIndex.Add(Key, Groups.Num() - 1);
		}
		FGroup& Gr = Groups[*Gi];
		double BaseP[3];
		if (Sf.PBase >= 0 && Sf.PBase < Npt) { for (int32 c = 0; c < 3; ++c) BaseP[c] = Pts[Sf.PBase * 3 + c]; }
		else { for (int32 c = 0; c < 3; ++c) BaseP[c] = Poly[c]; }
		double Tu[3] = { 0, 0, 0 }, Tv[3] = { 0, 0, 0 };
		if (Sf.VTu >= 0 && Sf.VTu < Nvec) for (int32 c = 0; c < 3; ++c) Tu[c] = Vecs[Sf.VTu * 3 + c];
		if (Sf.VTv >= 0 && Sf.VTv < Nvec) for (int32 c = 0; c < 3; ++c) Tv[c] = Vecs[Sf.VTv * 3 + c];
		const FIntPoint Size = bCollisionOnly ? FIntPoint(256, 256) : TexSize(MatPath);
		const uint32 B0 = Gr.Count;
		for (int32 k = 0; k < Cnt; ++k)
		{
			const double* P = &Poly[k * 3];
			const double Rx = P[0] - BaseP[0], Ry = P[1] - BaseP[1], Rz = P[2] - BaseP[2];
			Gr.Pos.Append({ P[0], P[1], P[2] });
			Gr.Nrm.Append({ Normal[0], Normal[1], Normal[2] });
			Gr.Uv.Add((Rx * Tu[0] + Ry * Tu[1] + Rz * Tu[2]) / double(Size.X));
			Gr.Uv.Add((Rx * Tv[0] + Ry * Tv[1] + Rz * Tv[2]) / double(Size.Y));
		}
		const bool bFront = Nw[0] * Normal[0] + Nw[1] * Normal[1] + Nw[2] * Normal[2] > 0;
		for (int32 k = 1; k < Cnt - 1; ++k)
		{
			if (bFront) Gr.Idx.Append({ B0, B0 + k + 1, B0 + k });
			else Gr.Idx.Append({ B0, B0 + k, B0 + k + 1 });
		}
		Gr.Count += Cnt;
		Gr.Surfs.Add(N.Surf);
	}
	Groups.Sort([](const FGroup& A, const FGroup& B)
	{
		const int32 C = CmpStr(A.Mat, B.Mat);
		return C != 0 ? C < 0 : A.Flags != B.Flags ? A.Flags < B.Flags : A.bSky != B.bSky ? int32(A.bSky) < int32(B.bSky)
			: int32(A.bCollisionOnly) < int32(B.bCollisionOnly);
	});
	TArray<FConvPrim> Prims;
	int64 Tris = 0, NVerts = 0;
	TArray<FString> MatList;
	for (const FGroup& Gr : Groups)
	{
		FConvPrim P;
		P.Positions.SetNumUninitialized(Gr.Pos.Num());
		for (int32 i = 0; i < Gr.Pos.Num(); ++i) P.Positions[i] = (float)(Gr.Pos[i] * S);
		P.Normals.SetNumUninitialized(Gr.Nrm.Num());
		for (int32 i = 0; i < Gr.Nrm.Num(); ++i) P.Normals[i] = (float)Gr.Nrm[i];
		P.UVs.SetNumUninitialized(Gr.Uv.Num());
		for (int32 i = 0; i < Gr.Uv.Num(); ++i) P.UVs[i] = (float)Gr.Uv[i];
		P.Indices = Gr.Idx;
		P.Material = Gr.Mat;
		P.Extras = FConvJson::Object();
		P.Extras.Set(TEXT("poly_flags"), (int64)Gr.Flags);
		P.Extras.Set(TEXT("poly_flag_names"), FlagNames(Gr.Flags));
		P.Extras.Set(TEXT("surface_count"), Gr.Surfs.Num());
		if (Gr.bSky) P.Extras.Set(TEXT("sky_zone"), true);
		if (Gr.bCollisionOnly) P.Extras.Set(TEXT("collision_only"), true);
		if (Gr.bCollisionOnly && (Gr.Flags & ArenaConvLevelLocal::PF_FakeBackdrop)) P.Extras.Set(TEXT("fake_backdrop"), true);
		Tris += Gr.Idx.Num() / 3;
		NVerts += Gr.Pos.Num() / 3;
		if (!Gr.bCollisionOnly) MatList.AddUnique(Gr.Mat);
		Prims.Add(MoveTemp(P));
	}
	FConvGltf Gb(S);
	Gb.AddPrimitiveList(TEXT("BSP_") + Pkg->Name, Prims);
	FConvJson Extra = FConvJson::Object();
	Extra.Set(TEXT("source"), Mo->Path());
	Extra.Set(TEXT("map"), Pkg->Name);
	Gb.Write(OutDir / TEXT("bsp.gltf"), Extra);
	MatList.Sort([](const FString& A, const FString& B) { return CmpStr(A, B) < 0; });
	FConvJson Info = FConvJson::Object();
	Info.Set(TEXT("file"), TEXT("bsp.gltf"));
	Info.Set(TEXT("model"), Mo->Path());
	Info.Set(TEXT("triangles"), Tris);
	Info.Set(TEXT("vertices"), NVerts);
	Info.Set(TEXT("primitives"), Prims.Num());
	Info.Set(TEXT("nodes"), Nn);
	Info.Set(TEXT("surfaces"), Ns);
	FConvJson Sk = FConvJson::Object();
	Sk.Set(TEXT("invisible_or_portal"), SkippedInvisible);
	Sk.Set(TEXT("degenerate"), SkippedDegenerate);
	Info.Set(TEXT("skipped_nodes"), Sk);
	Info.Set(TEXT("collision_only_nodes"), CollisionOnly);
	FConvJson Ml = FConvJson::Array();
	for (const FString& M : MatList) Ml.Add(M);
	Info.Set(TEXT("materials"), Ml);
	// BSP tree for point -> zone queries (UModel::PointRegion): bsptree.bin = "AZT1", int32 nodes, int32 zones, then per node
	// float plane[4] (normal, w in cm: dot(n, p) - w >= 0 is the front), int32 back, int32 front (-1 = leaf), uint8 zone[2]
	// (back, front), 2 pad bytes
	if (ZoneActors.Num() > 0 && Nn > 0)
	{
		TArray<uint8> Tree;
		Tree.Reserve(12 + int64(Nn) * 28);
		auto Put = [&Tree](const void* Data, int32 Size) { Tree.Append((const uint8*)Data, Size); };
		Put("AZT1", 4);
		const int32 Counts[2] = { Nn, ZoneActors.Num() };
		Put(Counts, 8);
		for (const FNodeR& N : Nodes)
		{
			const float Pl[4] = { float(N.Plane[0]), float(N.Plane[1]), float(N.Plane[2]), float(N.Plane[3] * S) };
			Put(Pl, 16);
			const int32 Ch[2] = { N.Back, N.Front };
			Put(Ch, 8);
			const uint8 Z[4] = { N.Zone[0], N.Zone[1], 0, 0 };
			Put(Z, 4);
		}
		if (FFileHelper::SaveArrayToFile(Tree, *(OutDir / TEXT("bsptree.bin"))))
		{
			FConvJson T = FConvJson::Object();
			T.Set(TEXT("file"), TEXT("bsptree.bin"));
			T.Set(TEXT("nodes"), Nn);
			FConvJson Zs = FConvJson::Array();
			for (int32 z = 0; z < ZoneActors.Num(); ++z)
			{
				FConvJson Z = FConvJson::Object();
				Z.Set(TEXT("zone"), z);
				Z.Set(TEXT("actor"), ZoneActors[z] ? FConvJson(ZoneActors[z]->Name()) : FConvJson());
				Z.Set(TEXT("sky"), SkyZone[z]);
				Zs.Add(Z);
			}
			T.Set(TEXT("zones"), Zs);
			Info.Set(TEXT("tree"), T);
		}
	}
	Info.Set(TEXT("uv_note"), TEXT("u = dot(P - pBase, TextureU) / USize, v = dot(P - pBase, TextureV) / VSize (UE2 surfaces carry no PanU/PanV; panning is baked into pBase)"));
	return Info;
}

bool FConvMapExporter::Run()
{
	const double T0 = FPlatformTime::Seconds();
	FConvPackage* Pkg = Mgr.LoadPackage(Name);
	if (!Pkg)
	{
		if (IFileManager::Get().FileExists(*Name)) Pkg = Mgr.OpenFile(Name);
		if (!Pkg)
		{
			Ex.Error(FString::Printf(TEXT("map %s"), *Name), TEXT("not found"));
			return false;
		}
	}
	const FString MapName = Pkg->Name;
	const FString OutDir = Ex.Opt.OutDir / TEXT("maps") / FConvExporter::Safe(MapName);
	const FString MapJson = OutDir / TEXT("map.json");
	if (Ex.Opt.bSkipExisting)
	{
		IFileManager& FM = IFileManager::Get();
		const FDateTime Mt = FM.GetTimeStamp(*MapJson);
		if (Mt != FDateTime::MinValue() && Mt >= FM.GetTimeStamp(*Pkg->PathOnDisk))
		{
			FConvJson Doc;
			if (FConvJson::LoadFile(MapJson, Doc) && Doc.GetNumber(TEXT("schema_version"), 0) >= SchemaVersion)
			{
				Ex.Counts.FindOrAdd(TEXT("maps_skipped"))++;
				return false;
			}
		}
	}
	Ex.CurrentItem = MapName;
	const int32 NErr0 = Ex.Errors.Num();
	const int32 Depth = Ex.Collect.Num();
	Ex.BeginCollect();
	FConvJson Timings = FConvJson::Object();
	double T = FPlatformTime::Seconds();
	TArray<int32> ActorIdx;
	int32 ModelIdx = 0;
	ReadLevel(Pkg, ActorIdx, ModelIdx);
	FConvJson Actors = FConvJson::Array();
	TArray<FConvObject*> Objs;
	for (int32 Ai : ActorIdx)
	{
		FConvObject* O = Pkg->GetObject(Ai - 1);
		if (!O)
		{
			Ex.Error(FString::Printf(TEXT("%s actor %s"), *MapName, *Pkg->RefName(Ai)), TEXT("bad export index"));
			continue;
		}
		Actors.Add(ActorRecord(O, Actors.A.Num()));
		Objs.Add(O);
	}
	Timings.Set(TEXT("actors"), FConvJson::RoundTo(FPlatformTime::Seconds() - T, 3));
	T = FPlatformTime::Seconds();
	for (int32 i = 0; i < Objs.Num(); ++i)
	{
		if (Ex.IsCancelled())
		{
			Ex.Collect.SetNum(Depth);
			return false;
		}
		TSet<FString> Seen;
		WalkRefs(Objs[i], 0, Seen);
		if (Actors.A[i].Get(TEXT("static_mesh")).Truthy())
		{
			const FConvProp* Sm = Objs[i]->GetProp(TEXT("StaticMesh"));
			if (Sm && Sm->IsRef() && Sm->Truthy()) Dep(Sm->Ref);
		}
	}
	// every exportable object of the map package itself (myLevel resources): script actors and class defaults of
	// classes defined in the map can reference them without any placed actor doing so
	for (int32 i = 0; i < Pkg->Exports.Num(); ++i)
	{
		FConvObject* O = Pkg->GetObject(i);
		if (O && !Ex.KindOf(O).IsEmpty())
		{
			Dep(Pkg->MakeRef(i + 1));
		}
	}
	Timings.Set(TEXT("assets"), FConvJson::RoundTo(FPlatformTime::Seconds() - T, 3));
	T = FPlatformTime::Seconds();
	FConvJson BspInfo;
	if (ModelIdx)
	{
		BspInfo = ExportBsp(Pkg, ModelIdx, OutDir);
	}
	Timings.Set(TEXT("bsp"), FConvJson::RoundTo(FPlatformTime::Seconds() - T, 3));
	T = FPlatformTime::Seconds();
	FConvJson M = FConvJson::Object();
	for (const TCHAR* K : { TEXT("player_starts"), TEXT("path_nodes"), TEXT("inventory_spots"), TEXT("jump_spots"), TEXT("other_nav_points"),
		TEXT("weapon_bases"), TEXT("weapon_lockers"), TEXT("pickup_bases"), TEXT("pickups"), TEXT("jump_pads"), TEXT("teleporters"),
		TEXT("flag_bases"), TEXT("bomb_spawns"), TEXT("bomb_deliveries"), TEXT("domination_points"), TEXT("game_objectives"), TEXT("lifts"),
		TEXT("lights"), TEXT("zones"), TEXT("volumes"), TEXT("terrains"), TEXT("ambient_sounds"), TEXT("movers"),
		TEXT("vehicle_factories"), TEXT("vehicles"), TEXT("power_cores"), TEXT("power_nodes"), TEXT("power_link_setups"), TEXT("power_links"),
		TEXT("assault_objectives"), TEXT("triggers"), TEXT("spawn_managers"), TEXT("invasion_spawns"), TEXT("monsters") })
	{
		M.Set(K, FConvJson::Array());
	}
	for (int32 i = 0; i < Objs.Num(); ++i)
	{
		Marker(Objs[i], Actors.A[i], M);
		ExtraMarker(Objs[i], Actors.A[i], M);
	}
	PostMarkers(M);
	Timings.Set(TEXT("markers"), FConvJson::RoundTo(FPlatformTime::Seconds() - T, 3));
	FConvJson Level, KillZ;
	FConvJson Summary = LevelInfo(Pkg, Objs, Level, KillZ);
	FConvJson Deps = Ex.EndCollect();
	Ex.Collect.SetNum(Depth);
	FConvJson Doc = FConvJson::Object();
	Doc.Set(TEXT("schema"), TEXT("ut2k4-map"));
	Doc.Set(TEXT("schema_version"), SchemaVersion);
	Doc.Set(TEXT("scale"), S);
	Doc.Set(TEXT("units"), TEXT("project centimetres (UE2 units * scale)"));
	Doc.Set(TEXT("coordinate_system"), TEXT("Unreal (X forward, Y right, Z up, left-handed) - same axes as UE5"));
	Doc.Set(TEXT("rotation_units"), TEXT("degrees (pitch, yaw, roll); rotation_raw is UE2 65536-per-turn"));
	Doc.Set(TEXT("transform_note"), TEXT("world = location + R(rotation) * (draw_scale * draw_scale3d * (vertex - pre_pivot)); mesh vertices are already scaled"));
	Doc.Set(TEXT("properties_note"), TEXT("actor \"properties\" are raw UE2 values (not scaled); object references are full object paths"));
	Doc.Set(TEXT("map"), MapName);
	Doc.Set(TEXT("source_file"), FPaths::GetCleanFilename(Pkg->PathOnDisk));
	Doc.Set(TEXT("package_version"), (int32)Pkg->Version);
	// the package GUID (UE2 FGuid::String): servers name their map by it, so a joining client can tell this conversion
	// from another map of the same name. Header offset 36; UE1 style packages (version < 68) keep it as the last entry
	// of the heritage table (count at 36, offset at 40), as UE2's linker reads it.
	int64 GuidAt = -1;
	if (Pkg->Version >= 68 && Pkg->BufSize() >= 52)
	{
		GuidAt = 36;
	}
	else if (Pkg->Version < 68 && Pkg->BufSize() >= 44)
	{
		int32 HeritageCount = 0, HeritageOffset = 0;
		FMemory::Memcpy(&HeritageCount, Pkg->Buf() + 36, sizeof(int32));
		FMemory::Memcpy(&HeritageOffset, Pkg->Buf() + 40, sizeof(int32));
		const int64 At = int64(HeritageOffset) + (int64(HeritageCount) - 1) * 16;
		if (HeritageCount > 0 && HeritageOffset > 0 && At + 16 <= int64(Pkg->BufSize()))
		{
			GuidAt = At;
		}
	}
	if (GuidAt >= 0)
	{
		uint32 G[4];
		FMemory::Memcpy(G, Pkg->Buf() + GuidAt, sizeof(G));
		Doc.Set(TEXT("package_guid"), FString::Printf(TEXT("%08X%08X%08X%08X"), G[0], G[1], G[2], G[3]));
	}
	Doc.Set(TEXT("licensee_version"), (int32)Pkg->Licensee);
	int32 Dash;
	Doc.Set(TEXT("game_type_prefix"), MapName.FindChar(TEXT('-'), Dash) ? FConvJson(MapName.Left(Dash).ToUpper()) : FConvJson());
	Doc.Set(TEXT("summary"), Summary);
	Doc.Set(TEXT("level_info"), Level);
	Doc.Set(TEXT("kill_z"), KillZ);
	Doc.Set(TEXT("bsp"), BspInfo);
	Doc.Set(TEXT("markers"), M);
	Doc.Set(TEXT("dependencies"), Deps);
	Doc.Set(TEXT("actor_count"), Actors.A.Num());
	Doc.Set(TEXT("actors"), Actors);
	Doc.Set(TEXT("class_defaults"), ClassDefaultsJson);
	Timings.Set(TEXT("total"), FConvJson::RoundTo(FPlatformTime::Seconds() - T0, 3));
	FConvJson Stats = FConvJson::Object();
	Stats.Set(TEXT("timings_s"), Timings);
	Stats.Set(TEXT("errors"), Ex.Errors.Num() - NErr0);
	Doc.Set(TEXT("export_stats"), Stats);
	Ex.WriteJson(MapJson, Doc);
	Ex.Counts[TEXT("maps")]++;
	return true;
}

bool FConvExporter::ExportMap(const FString& MapName)
{
	FConvMapExporter M(*this, MapName);
	return M.Run();
}

#endif // ARENA_CONV_BUILD
