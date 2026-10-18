#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Minimal glTF 2.0 reader for the content cache (Tools/ut2k4 exporter output and the C++ converter's).
 *
 * The cache glTF files are plain .gltf json + one external .bin buffer, in Unreal space (centimetres,
 * X forward, Y right, Z up) and Unreal winding, so no axis conversion happens here. Thread-safe (no
 * engine objects): runtime builders parse on worker threads.
 */
struct OPENUT2K4_API FArenaGltf
{
	TSharedPtr<FJsonObject> Doc;
	/** buffers[0] */
	TArray64<uint8> Bin;

	/** Reads <Path> and its first buffer. */
	bool Load(const FString& Path);

	/** Top-level array (meshes, accessors, materials, nodes, skins, animations...); nullptr if absent. */
	const TArray<TSharedPtr<FJsonValue>>* Array(const TCHAR* Field) const;
	/** Object Index of a top-level array. */
	TSharedPtr<FJsonObject> Item(const TCHAR* Field, int32 Index) const;

	/** Element count and component count (SCALAR 1, VEC2 2, VEC3 3, VEC4 4, MAT4 16) of an accessor. */
	bool AccessorInfo(int32 Accessor, int32& OutCount, int32& OutComponents) const;
	/** Any component type -> floats (normalized integer types are mapped to 0..1). */
	bool ReadFloats(int32 Accessor, TArray<float>& Out, int32* OutComponents = nullptr) const;
	/** Integer component types -> uint32 (indices, joints). */
	bool ReadUInts(int32 Accessor, TArray<uint32>& Out, int32* OutComponents = nullptr) const;

	/** Name of material Index ("None" when the primitive has no material). */
	FString MaterialName(int32 Index) const;
};
