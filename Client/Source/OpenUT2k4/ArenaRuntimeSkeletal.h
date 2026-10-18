#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimSequence.h"
#include "ArenaRuntimeSkeletal.generated.h"

class USkeletalMesh;
class USkeleton;

/**
 * Runtime skeletal content: USkeletalMesh / USkeleton / animation sequences built from the content cache
 * (skeletalmeshes/, animations/ and vertmeshes/ glTF + json written by Tools/ut2k4_export.py), so that release
 * builds need no cooked /Game/Imported assets. Same result as Tools/ImportUT2004.py's SkeletalImporter:
 *
 *  - "SkeletalMeshes/<Pkg>/<Name>"            skeletal mesh (or vertex mesh: one joint per vertex), one LOD,
 *                                              material slot per distinct section material (first-use order),
 *                                              mesh-only sockets from the json, no physics asset.
 *  - "SkeletalMeshes/<Pkg>/<Name>_Skeleton"   its USkeleton.
 *  - "Animations/<Pkg>/<Set>_<Seq>"           one sequence of an animation set (or of a vertex mesh).
 *  Bone names get spaces replaced by '-' (as Interchange does, the game code uses those names).
 *  Skeletons are shared per animation set: every mesh whose json "animation" names a set uses the set's
 *  skeleton, so UAnimSequence::GetSkeleton() == USkeletalMesh::GetSkeleton() checks behave as with the cooked
 *  assets. Paths use the editor importer's names (non [A-Za-z0-9_-] characters -> '_'), case-insensitive.
 *
 * Animations are UArenaRuntimeAnimSequence objects: a UAnimSequence subclass that keeps the cache keyframes
 * (uncompressed, linear interpolation) and samples them itself in GetAnimationPose. Non-editor builds cannot
 * compress animation data at runtime, and the engine's sequence evaluation only reads compressed data there,
 * so the subclass is what makes runtime animations work in cooked builds - and it behaves identically in the
 * editor, which keeps -game tests representative. Being a UAnimSequence it works everywhere the game plays
 * sequences: the native character anim proxy, single-node PlayAnimation (first-person weapons, flags,
 * projectiles, vehicle drivers). Tracks are matched to the evaluating mesh's bones by name.
 *
 * The loaders register themselves with ArenaRuntimeContent (RegisterLoader "SkeletalMeshes" / "Animations")
 * at engine init; ArenaArt::LoadArt reaches them through ArenaRuntimeContent::LoadByRelativePath. Everything
 * is transient, game-thread only and kept alive permanently (characters / weapons are reused every match).
 * Works with WITH_EDITOR=0: render data is filled directly (FSkeletalMeshLODRenderData), no mesh builder.
 */
UCLASS(Transient, NotBlueprintable)
class OPENUT2K4_API UArenaRuntimeAnimSequence : public UAnimSequence
{
	GENERATED_BODY()

public:
	/** Keyframes of one bone: Times are shared per sampler input; empty arrays keep the reference pose. */
	struct FTrack
	{
		/** Bone name as in the runtime meshes (spaces -> '-'). */
		FName Bone;
		int32 PosTimes = INDEX_NONE;
		int32 RotTimes = INDEX_NONE;
		int32 ScaleTimes = INDEX_NONE;
		TArray<FVector3f> Pos;
		TArray<FQuat4f> Rot;
		TArray<FVector3f> Scale;
	};

	/** Initialises the sequence (game thread, before first use). */
	void InitRuntime(USkeleton* InSkeleton, float InLength, float InFrameRate, TArray<TArray<float>>&& InTimes, TArray<FTrack>&& InTracks);

	int32 GetNumTracks() const { return Tracks.Num(); }

	// UAnimSequence / UAnimSequenceBase
	virtual void GetAnimationPose(FAnimationPoseData& OutAnimationPoseData, const FAnimExtractContext& ExtractionContext) const override;
	virtual float GetPlayLength() const override { return Length; }
#if WITH_EDITOR
	virtual bool IsValidToPlay() const override { return Length > 0.f && Tracks.Num() > 0; }
#endif
	virtual int32 GetNumberOfSampledKeys() const override { return FMath::Max(1, FMath::RoundToInt(Length * FrameRate) + 1); }
	virtual FFrameRate GetSamplingFrameRate() const override { return FFrameRate(FMath::Max(1, FMath::RoundToInt(FrameRate)), 1); }

private:
	float Length = 0.f;
	float FrameRate = 30.f;
	TArray<TArray<float>> Times;
	TArray<FTrack> Tracks;
};

namespace ArenaRuntimeSkeletal
{
	/** "SkeletalMeshes/<Pkg>/<Name>" -> skeletal mesh built from the cache (nullptr if the cache has none). */
	OPENUT2K4_API USkeletalMesh* LoadSkeletalMesh(const FString& RelativePath);
	/** "Pkg.Name" (UT2004 SkeletalMesh / VertMesh object path). */
	OPENUT2K4_API USkeletalMesh* LoadSkeletalMeshByObjectPath(const FString& ObjectPath);
	/** "SkeletalMeshes/<Pkg>/<Name>_Skeleton" or a mesh path -> the skeleton that mesh uses. */
	OPENUT2K4_API USkeleton* LoadSkeleton(const FString& RelativePath);
	/** "Animations/<Pkg>/<Set>_<Seq>" (builds the whole set on first use). */
	OPENUT2K4_API UAnimSequence* LoadAnimation(const FString& RelativePath);
	/** "Pkg.Set" + sequence name. */
	OPENUT2K4_API UAnimSequence* LoadAnimationByObjectPath(const FString& SetObjectPath, const FString& Sequence);

	/** Loader entry point registered with ArenaRuntimeContent for the "SkeletalMeshes" and "Animations" kinds. */
	OPENUT2K4_API UObject* LoadByRelativePath(UClass* Class, const FString& RelativePath);

	/**
	 * Development check (editor -game builds with cooked imports): builds the given meshes / animation sets from
	 * the cache and compares them with the /Game/Imported assets (bones, reference pose, sockets, bounds, section
	 * counts, sequence lengths, sampled poses). Logs a summary line per asset. Console: arena.CompareRuntimeSkeletal.
	 */
	OPENUT2K4_API void CompareWithImported(const TArray<FString>& RelativePaths);

	/** Stats line for logs. */
	OPENUT2K4_API FString GetStats();
}
