#include "ArenaConvGltf.h"
#include "Misc/Paths.h"

namespace ArenaConvGltfLocal
{
	constexpr int32 GL_FLOAT = 5126;
	constexpr int32 GL_UINT16 = 5123;
	constexpr int32 GL_UINT32 = 5125;
	constexpr int32 ARRAY_BUFFER = 34962;
	constexpr int32 ELEMENT_ARRAY_BUFFER = 34963;
}

namespace ArenaConvGltf
{
	FConvJson CoordNote(double Scale, bool bSkeletal)
	{
		FConvJson N = FConvJson::Object();
		N.Set(TEXT("coordinateSystem"), TEXT("Unreal (X forward, Y right, Z up, left-handed); positions are NOT converted to glTF Y-up space"));
		N.Set(TEXT("units"), TEXT("project centimetres (UE2 units * scale)"));
		N.Set(TEXT("winding"), TEXT("Unreal native (same as UE2/UE5 static meshes): cross(v1 - v0, v2 - v0) points OPPOSITE the face normal in the stored coordinates"));
		N.Set(TEXT("uvOrigin"), TEXT("top-left (U right, V down)"));
		if (bSkeletal)
		{
			N.Set(TEXT("skeletal"), TEXT("joints/IBMs/animations in the same Unreal space; mesh-to-actor transform (MeshOrigin, MeshScale, RotOrigin) baked in"));
			N.Set(TEXT("rotations"), TEXT("standard quaternions (x, y, z, w), local-to-parent"));
		}
		N.Set(TEXT("scale"), Scale);
		return N;
	}
}

int32 FConvGltf::AddView(const void* Data, int64 Bytes, int32 Target, bool bAlwaysTarget)
{
	while (Bin.Num() % 4)
	{
		Bin.Add(0);
	}
	const int64 Off = Bin.Num();
	Bin.Append((const uint8*)Data, Bytes);
	FConvJson V = FConvJson::Object();
	V.Set(TEXT("buffer"), 0);
	V.Set(TEXT("byteOffset"), Off);
	V.Set(TEXT("byteLength"), Bytes);
	if (bAlwaysTarget || Target)
	{
		V.Set(TEXT("target"), Target);
	}
	BufferViews.Add(MoveTemp(V));
	return BufferViews.A.Num() - 1;
}

int32 FConvGltf::Accessor(const void* Data, int32 Count, int32 NumComp, int32 CompType, const TCHAR* Type, int32 Target, bool bMinMax, bool bStaticStyle)
{
	const int32 CompSize = CompType == ArenaConvGltfLocal::GL_UINT16 ? 2 : 4;
	const int32 View = AddView(Data, int64(Count) * NumComp * CompSize, Target, bStaticStyle);
	FConvJson A = FConvJson::Object();
	A.Set(TEXT("bufferView"), View);
	A.Set(TEXT("componentType"), CompType);
	A.Set(TEXT("count"), Count);
	A.Set(TEXT("type"), Type);
	if (bMinMax && Count > 0 && CompType == ArenaConvGltfLocal::GL_FLOAT)
	{
		const float* F = (const float*)Data;
		FConvJson Mn = FConvJson::Array(), Mx = FConvJson::Array();
		for (int32 c = 0; c < NumComp; ++c)
		{
			float Lo = F[c], Hi = F[c];
			for (int32 i = 1; i < Count; ++i)
			{
				const float V = F[int64(i) * NumComp + c];
				Lo = FMath::Min(Lo, V);
				Hi = FMath::Max(Hi, V);
			}
			Mn.Add(double(Lo));
			Mx.Add(double(Hi));
		}
		A.Set(TEXT("min"), MoveTemp(Mn));
		A.Set(TEXT("max"), MoveTemp(Mx));
	}
	Accessors.Add(MoveTemp(A));
	return Accessors.A.Num() - 1;
}

int32 FConvGltf::Material(const FString& Name, const FConvJson& Extras)
{
	if (const int32* Hit = MatIndex.Find(Name))
	{
		return *Hit;
	}
	FConvJson M = FConvJson::Object();
	M.Set(TEXT("name"), Name);
	FConvJson Pbr = FConvJson::Object();
	Pbr.Set(TEXT("metallicFactor"), 0.0);
	Pbr.Set(TEXT("roughnessFactor"), 1.0);
	M.Set(TEXT("pbrMetallicRoughness"), MoveTemp(Pbr));
	if (Extras.Truthy())
	{
		M.Set(TEXT("extras"), Extras);
	}
	Materials.Add(MoveTemp(M));
	const int32 Idx = Materials.A.Num() - 1;
	MatIndex.Add(Name, Idx);
	return Idx;
}

int32 FConvGltf::AddPrimitiveList(const FString& MeshName, const TArray<FConvPrim>& Prims)
{
	using namespace ArenaConvGltfLocal;
	FConvJson Out = FConvJson::Array();
	for (const FConvPrim& P : Prims)
	{
		const int32 N = P.NumVerts();
		if (N == 0 || P.Indices.Num() == 0)
		{
			continue;
		}
		FConvJson Attrs = FConvJson::Object();
		Attrs.Set(TEXT("POSITION"), Accessor(P.Positions.GetData(), N, 3, GL_FLOAT, TEXT("VEC3"), ARRAY_BUFFER, true, true));
		if (P.Normals.Num())
		{
			TArray<float> Nrm = P.Normals;
			for (int32 i = 0; i < N; ++i)
			{
				float* V = &Nrm[i * 3];
				float L = FMath::Sqrt(V[0] * V[0] + V[1] * V[1] + V[2] * V[2]);
				if (L == 0.f)
				{
					L = 1.f;
				}
				V[0] /= L;
				V[1] /= L;
				V[2] /= L;
			}
			Attrs.Set(TEXT("NORMAL"), Accessor(Nrm.GetData(), N, 3, GL_FLOAT, TEXT("VEC3"), ARRAY_BUFFER, false, true));
		}
		if (P.UVs.Num())
		{
			Attrs.Set(TEXT("TEXCOORD_0"), Accessor(P.UVs.GetData(), N, 2, GL_FLOAT, TEXT("VEC2"), ARRAY_BUFFER, false, true));
		}
		int32 IAcc;
		if (N < 65536)
		{
			TArray<uint16> I16;
			I16.SetNumUninitialized(P.Indices.Num());
			for (int32 i = 0; i < P.Indices.Num(); ++i)
			{
				I16[i] = (uint16)P.Indices[i];
			}
			IAcc = Accessor(I16.GetData(), I16.Num(), 1, GL_UINT16, TEXT("SCALAR"), ELEMENT_ARRAY_BUFFER, false, true);
		}
		else
		{
			IAcc = Accessor(P.Indices.GetData(), P.Indices.Num(), 1, GL_UINT32, TEXT("SCALAR"), ELEMENT_ARRAY_BUFFER, false, true);
		}
		FConvJson Prim = FConvJson::Object();
		Prim.Set(TEXT("attributes"), MoveTemp(Attrs));
		Prim.Set(TEXT("indices"), IAcc);
		Prim.Set(TEXT("mode"), 4);
		Prim.Set(TEXT("material"), Material(P.Material, P.MaterialExtras));
		if (P.Extras.Truthy())
		{
			Prim.Set(TEXT("extras"), P.Extras);
		}
		Out.Add(MoveTemp(Prim));
	}
	const int32 Count = Out.A.Num();
	FConvJson Mesh = FConvJson::Object();
	Mesh.Set(TEXT("name"), MeshName);
	Mesh.Set(TEXT("primitives"), MoveTemp(Out));
	Meshes.Add(MoveTemp(Mesh));
	FConvJson Node = FConvJson::Object();
	Node.Set(TEXT("name"), MeshName);
	Node.Set(TEXT("mesh"), Meshes.A.Num() - 1);
	Nodes.Add(MoveTemp(Node));
	return Count;
}

FConvJson FConvGltf::BaseDoc(const FString& BinName, const FConvJson& AssetExtras) const
{
	FConvJson Doc = FConvJson::Object();
	FConvJson Asset = FConvJson::Object();
	Asset.Set(TEXT("version"), TEXT("2.0"));
	Asset.Set(TEXT("generator"), TEXT("ut2k4_export"));
	Asset.Set(TEXT("extras"), AssetExtras);
	Doc.Set(TEXT("asset"), MoveTemp(Asset));
	Doc.Set(TEXT("scene"), 0);
	return Doc;
}

bool FConvGltf::Finish(const FString& GltfPath, FConvJson& Doc)
{
	const FString BinName = FPaths::GetBaseFilename(GltfPath) + TEXT(".bin");
	FConvJson Buf = FConvJson::Object();
	Buf.Set(TEXT("uri"), BinName);
	Buf.Set(TEXT("byteLength"), (int64)Bin.Num());
	FConvJson Bufs = FConvJson::Array();
	Bufs.Add(MoveTemp(Buf));
	Doc.Set(TEXT("buffers"), MoveTemp(Bufs));
	if (!ConvWriteFile(FPaths::GetPath(GltfPath) / BinName, Bin.GetData(), Bin.Num()))
	{
		return false;
	}
	return Doc.SaveToFile(GltfPath, 1);
}

bool FConvGltf::Write(const FString& GltfPath, const FConvJson& Extras)
{
	FConvJson AssetExtras = ArenaConvGltf::CoordNote(Scale, false);
	for (const TPair<FString, FConvJson>& KV : Extras.O)
	{
		AssetExtras.Set(KV.Key, KV.Value);
	}
	FConvJson Doc = BaseDoc(FString(), AssetExtras);
	FConvJson SceneNodes = FConvJson::Array();
	for (int32 i = 0; i < Nodes.A.Num(); ++i)
	{
		SceneNodes.Add(i);
	}
	FConvJson Scene = FConvJson::Object();
	Scene.Set(TEXT("nodes"), MoveTemp(SceneNodes));
	FConvJson Scenes = FConvJson::Array();
	Scenes.Add(MoveTemp(Scene));
	Doc.Set(TEXT("scenes"), MoveTemp(Scenes));
	Doc.Set(TEXT("nodes"), Nodes);
	Doc.Set(TEXT("meshes"), Meshes);
	Doc.Set(TEXT("materials"), Materials);
	Doc.Set(TEXT("accessors"), Accessors);
	Doc.Set(TEXT("bufferViews"), BufferViews);
	return Finish(GltfPath, Doc);
}

void FConvGltf::AddSkeleton(const TArray<FString>& Names, const TArray<int32>& Parents, const TArray<FQuat4d>& LocalRot,
	const TArray<FVector3d>& LocalPos, TArray<int32>& OutRoots, TArray<int32>& OutJoints)
{
	const int32 Base = Nodes.A.Num();
	for (int32 i = 0; i < Names.Num(); ++i)
	{
		FConvJson N = FConvJson::Object();
		N.Set(TEXT("name"), Names[i]);
		FConvJson T = FConvJson::Array();
		T.Add(LocalPos[i].X); T.Add(LocalPos[i].Y); T.Add(LocalPos[i].Z);
		N.Set(TEXT("translation"), MoveTemp(T));
		FConvJson R = FConvJson::Array();
		R.Add(LocalRot[i].X); R.Add(LocalRot[i].Y); R.Add(LocalRot[i].Z); R.Add(LocalRot[i].W);
		N.Set(TEXT("rotation"), MoveTemp(R));
		Nodes.Add(MoveTemp(N));
	}
	OutRoots.Reset();
	for (int32 i = 0; i < Parents.Num(); ++i)
	{
		const int32 P = Parents[i];
		if (P < 0 || P >= i)
		{
			OutRoots.Add(Base + i);
		}
		else
		{
			FConvJson& Parent = Nodes.A[Base + P];
			FConvJson* Kids = Parent.Find(TEXT("children"));
			if (!Kids)
			{
				Kids = &Parent.Set(TEXT("children"), FConvJson::Array());
			}
			Kids->Add(Base + i);
		}
	}
	OutJoints.Reset();
	for (int32 i = 0; i < Names.Num(); ++i)
	{
		OutJoints.Add(Base + i);
	}
}

int32 FConvGltf::AddSkin(const TArray<FMatrix44d>& WorldColumn, const TArray<int32>& JointNodes, int32 RootNode)
{
	TArray<float> Ibm;
	Ibm.SetNumUninitialized(JointNodes.Num() * 16);
	for (int32 i = 0; i < WorldColumn.Num(); ++i)
	{
		const FMatrix44d Inv = WorldColumn[i].Inverse();
		// column-major output: element (r, c) at c * 4 + r. Matrices are stored M[r][c] (column-vector math).
		for (int32 r = 0; r < 4; ++r)
		{
			for (int32 c = 0; c < 4; ++c)
			{
				Ibm[i * 16 + c * 4 + r] = (float)Inv.M[r][c];
			}
		}
	}
	const int32 Acc = Accessor(Ibm.GetData(), JointNodes.Num(), 16, ArenaConvGltfLocal::GL_FLOAT, TEXT("MAT4"), 0, false, false);
	FConvJson S = FConvJson::Object();
	FConvJson J = FConvJson::Array();
	for (int32 N : JointNodes)
	{
		J.Add(N);
	}
	S.Set(TEXT("joints"), MoveTemp(J));
	S.Set(TEXT("inverseBindMatrices"), Acc);
	S.Set(TEXT("skeleton"), RootNode);
	Skins.Add(MoveTemp(S));
	return Skins.A.Num() - 1;
}

int32 FConvGltf::AddSkinnedMesh(const FString& Name, const TArray<FConvPrim>& Prims, int32 Skin)
{
	using namespace ArenaConvGltfLocal;
	FConvJson Out = FConvJson::Array();
	for (const FConvPrim& P : Prims)
	{
		const int32 N = P.NumVerts();
		if (N == 0 || P.Indices.Num() == 0)
		{
			continue;
		}
		FConvJson Attrs = FConvJson::Object();
		Attrs.Set(TEXT("POSITION"), Accessor(P.Positions.GetData(), N, 3, GL_FLOAT, TEXT("VEC3"), ARRAY_BUFFER, true, false));
		if (P.Normals.Num())
		{
			Attrs.Set(TEXT("NORMAL"), Accessor(P.Normals.GetData(), N, 3, GL_FLOAT, TEXT("VEC3"), ARRAY_BUFFER, false, false));
		}
		if (P.UVs.Num())
		{
			Attrs.Set(TEXT("TEXCOORD_0"), Accessor(P.UVs.GetData(), N, 2, GL_FLOAT, TEXT("VEC2"), ARRAY_BUFFER, false, false));
		}
		Attrs.Set(TEXT("JOINTS_0"), Accessor(P.Joints.GetData(), N, 4, GL_UINT16, TEXT("VEC4"), ARRAY_BUFFER, false, false));
		Attrs.Set(TEXT("WEIGHTS_0"), Accessor(P.Weights.GetData(), N, 4, GL_FLOAT, TEXT("VEC4"), ARRAY_BUFFER, false, false));
		int32 IAcc;
		if (N < 65536)
		{
			TArray<uint16> I16;
			I16.SetNumUninitialized(P.Indices.Num());
			for (int32 i = 0; i < P.Indices.Num(); ++i)
			{
				I16[i] = (uint16)P.Indices[i];
			}
			IAcc = Accessor(I16.GetData(), I16.Num(), 1, GL_UINT16, TEXT("SCALAR"), ELEMENT_ARRAY_BUFFER, false, false);
		}
		else
		{
			IAcc = Accessor(P.Indices.GetData(), P.Indices.Num(), 1, GL_UINT32, TEXT("SCALAR"), ELEMENT_ARRAY_BUFFER, false, false);
		}
		FConvJson Prim = FConvJson::Object();
		Prim.Set(TEXT("attributes"), MoveTemp(Attrs));
		Prim.Set(TEXT("indices"), IAcc);
		Prim.Set(TEXT("mode"), 4);
		Prim.Set(TEXT("material"), Material(P.Material));
		if (P.Extras.Truthy())
		{
			Prim.Set(TEXT("extras"), P.Extras);
		}
		Out.Add(MoveTemp(Prim));
	}
	FConvJson Mesh = FConvJson::Object();
	Mesh.Set(TEXT("name"), Name);
	Mesh.Set(TEXT("primitives"), MoveTemp(Out));
	Meshes.Add(MoveTemp(Mesh));
	FConvJson Node = FConvJson::Object();
	Node.Set(TEXT("name"), Name);
	Node.Set(TEXT("mesh"), Meshes.A.Num() - 1);
	Node.Set(TEXT("skin"), Skin);
	Nodes.Add(MoveTemp(Node));
	return Nodes.A.Num() - 1;
}

void FConvGltf::AddAnimation(const FString& Name, const TArray<float>& Times, const TArray<FChannel>& Channels, FConvJson Extras)
{
	using namespace ArenaConvGltfLocal;
	const int32 TAcc = Accessor(Times.GetData(), Times.Num(), 1, GL_FLOAT, TEXT("SCALAR"), 0, true, false);
	FConvJson Samplers = FConvJson::Array(), Chans = FConvJson::Array();
	for (const FChannel& C : Channels)
	{
		const int32 NC = C.bRotation ? 4 : 3;
		const int32 OAcc = Accessor(C.Values.GetData(), C.Values.Num() / NC, NC, GL_FLOAT, C.bRotation ? TEXT("VEC4") : TEXT("VEC3"), 0, false, false);
		FConvJson S = FConvJson::Object();
		S.Set(TEXT("input"), TAcc);
		S.Set(TEXT("output"), OAcc);
		S.Set(TEXT("interpolation"), TEXT("LINEAR"));
		Samplers.Add(MoveTemp(S));
		FConvJson Ch = FConvJson::Object();
		Ch.Set(TEXT("sampler"), Samplers.A.Num() - 1);
		FConvJson T = FConvJson::Object();
		T.Set(TEXT("node"), C.Node);
		T.Set(TEXT("path"), C.bRotation ? TEXT("rotation") : TEXT("translation"));
		Ch.Set(TEXT("target"), MoveTemp(T));
		Chans.Add(MoveTemp(Ch));
	}
	FConvJson Anim = FConvJson::Object();
	Anim.Set(TEXT("name"), Name);
	Anim.Set(TEXT("samplers"), MoveTemp(Samplers));
	Anim.Set(TEXT("channels"), MoveTemp(Chans));
	if (Extras.Truthy())
	{
		Anim.Set(TEXT("extras"), MoveTemp(Extras));
	}
	Animations.Add(MoveTemp(Anim));
}

bool FConvGltf::WriteSkinned(const FString& GltfPath, const TArray<int32>& SceneNodes, const FConvJson& Extras)
{
	FConvJson AssetExtras = ArenaConvGltf::CoordNote(Scale, true);
	for (const TPair<FString, FConvJson>& KV : Extras.O)
	{
		AssetExtras.Set(KV.Key, KV.Value);
	}
	FConvJson Doc = BaseDoc(FString(), AssetExtras);
	FConvJson SN = FConvJson::Array();
	for (int32 N : SceneNodes)
	{
		SN.Add(N);
	}
	FConvJson Scene = FConvJson::Object();
	Scene.Set(TEXT("nodes"), MoveTemp(SN));
	FConvJson Scenes = FConvJson::Array();
	Scenes.Add(MoveTemp(Scene));
	Doc.Set(TEXT("scenes"), MoveTemp(Scenes));
	Doc.Set(TEXT("nodes"), Nodes);
	Doc.Set(TEXT("meshes"), Meshes);
	Doc.Set(TEXT("materials"), Materials);
	Doc.Set(TEXT("skins"), Skins);
	Doc.Set(TEXT("accessors"), Accessors);
	Doc.Set(TEXT("bufferViews"), BufferViews);
	// buffers are added by Finish; animations come after them as in the Python writer
	FConvJson Anims = Animations;
	const bool bAnims = Animations.A.Num() > 0;
	const FString BinName = FPaths::GetBaseFilename(GltfPath) + TEXT(".bin");
	FConvJson Buf = FConvJson::Object();
	Buf.Set(TEXT("uri"), BinName);
	Buf.Set(TEXT("byteLength"), (int64)Bin.Num());
	FConvJson Bufs = FConvJson::Array();
	Bufs.Add(MoveTemp(Buf));
	Doc.Set(TEXT("buffers"), MoveTemp(Bufs));
	if (bAnims)
	{
		Doc.Set(TEXT("animations"), MoveTemp(Anims));
	}
	if (!ConvWriteFile(FPaths::GetPath(GltfPath) / BinName, Bin.GetData(), Bin.Num()))
	{
		return false;
	}
	return Doc.SaveToFile(GltfPath, 1);
}
