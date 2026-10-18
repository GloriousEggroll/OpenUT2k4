#pragma once

#include "CoreMinimal.h"
#include "ArenaConvJson.h"

/**
 * glTF 2.0 writer (.gltf json + external .bin) matching Tools/ut2k4/gltf.py GltfBuilder and
 * skelexport.py SkinnedGltf: Unreal space, project centimetres, Unreal winding (see the asset extras).
 */
struct FConvPrim
{
	/** xyz per vertex */
	TArray<float> Positions;
	/** xyz per vertex, or empty */
	TArray<float> Normals;
	/** uv per vertex, or empty */
	TArray<float> UVs;
	/** 4 per vertex (skinned only) */
	TArray<uint16> Joints;
	/** 4 per vertex (skinned only) */
	TArray<float> Weights;
	TArray<uint32> Indices;
	FString Material = TEXT("None");
	FConvJson Extras;
	FConvJson MaterialExtras;

	int32 NumVerts() const { return Positions.Num() / 3; }
};

class FConvGltf
{
public:
	explicit FConvGltf(double InScale) : Scale(InScale) {}

	/** Static mesh primitives (GltfBuilder.add_primitive_list): normals normalised, one mesh + node. */
	int32 AddPrimitiveList(const FString& MeshName, const TArray<FConvPrim>& Prims);
	/** Writes path.gltf + path.bin (GltfBuilder.write). */
	bool Write(const FString& GltfPath, const FConvJson& Extras);

	// ---- skinned (SkinnedGltf) ----
	/** Joint nodes; returns the root node indices and the node index of every joint. */
	void AddSkeleton(const TArray<FString>& Names, const TArray<int32>& Parents, const TArray<FQuat4d>& LocalRot,
		const TArray<FVector3d>& LocalPos, TArray<int32>& OutRoots, TArray<int32>& OutJoints);
	/** WorldMatrices are column-vector 4x4 bind-pose joint matrices (row-major storage M[r][c]). */
	int32 AddSkin(const TArray<FMatrix44d>& WorldColumn, const TArray<int32>& JointNodes, int32 RootNode);
	int32 AddSkinnedMesh(const FString& Name, const TArray<FConvPrim>& Prims, int32 Skin);
	struct FChannel
	{
		int32 Node = 0;
		bool bRotation = false;
		/** 4 (rotation) or 3 (translation) floats per key */
		TArray<float> Values;
	};
	void AddAnimation(const FString& Name, const TArray<float>& Times, const TArray<FChannel>& Channels, FConvJson Extras);
	bool WriteSkinned(const FString& GltfPath, const TArray<int32>& SceneNodes, const FConvJson& Extras);

	int32 Material(const FString& Name, const FConvJson& Extras = FConvJson());

	double Scale;

private:
	int32 AddView(const void* Data, int64 Bytes, int32 Target, bool bAlwaysTarget);
	int32 Accessor(const void* Data, int32 Count, int32 NumComp, int32 CompType, const TCHAR* Type, int32 Target, bool bMinMax, bool bStaticStyle);
	FConvJson BaseDoc(const FString& BinName, const FConvJson& AssetExtras) const;
	bool Finish(const FString& GltfPath, FConvJson& Doc);

	TArray<uint8> Bin;
	FConvJson BufferViews = FConvJson::Array();
	FConvJson Accessors = FConvJson::Array();
	FConvJson Materials = FConvJson::Array();
	TMap<FString, int32> MatIndex;
	FConvJson Meshes = FConvJson::Array();
	FConvJson Nodes = FConvJson::Array();
	FConvJson Skins = FConvJson::Array();
	FConvJson Animations = FConvJson::Array();
};

namespace ArenaConvGltf
{
	/** gltf.py COORD_NOTE + scale + extras (asset.extras of static files). */
	FConvJson CoordNote(double Scale, bool bSkeletal);
}
