#pragma once

#include "CoreMinimal.h"
#include "UE2NetConnection.h"

class FUE2BitReader;
class FUE2BitWriter;
class FUE2PackageMap;
struct FUSFunction;
struct FUSProperty;
struct FUSStruct;

/**
 * Replicated value encoding in the UE2 wire format, shared by the server (Net/UE2Server*) and usable by the client:
 * properties, structs (Vector / Rotator compressed, Plane, CompressedPosition, Quat, generic members), function
 * parameters. See Docs/UE2Net.md "Property values".
 *
 * Object references in FUE2NetValue go through the context: a static object (Obj.IsStatic(): package map entry +
 * export) is written as its static index in Map, an actor reference (Obj.Channel) is turned into a channel index of
 * this connection by ActorToChannel (0 when the actor has no open channel: it then travels as None, like in UE2).
 */
struct FUE2WireContext
{
	FUE2PackageMap* Map = nullptr;
	/** Writing: the value's Obj.Channel (an id of the caller's choosing) -> actor channel of this connection (0: None). */
	TFunction<int32(int32 Id)> ActorToChannel;
	/** Writing: a static reference's PackageIndex -> package index in Map (INDEX_NONE: not in it). Identity when unset. */
	TFunction<int32(int32 PackageIndex)> PackageToMap;
	/** Reading: an actor channel -> the caller's id (stored in Obj.Channel). Identity when unset. */
	TFunction<int32(int32 Channel)> ChannelToActor;
};

namespace UE2Codec
{
	constexpr uint32 MaxChannels = 1023;

	OPENUT2K4_API FVector3f ReadCompressedVector(FUE2BitReader& R);
	OPENUT2K4_API void WriteCompressedVector(FUE2BitWriter& W, const FVector3f& V);
	OPENUT2K4_API FIntVector ReadCompressedRotator(FUE2BitReader& R);
	OPENUT2K4_API void WriteCompressedRotator(FUE2BitWriter& W, const FIntVector& Rot);

	/** One element of a property. Value may be null (writes the type's zero value). False for types that never replicate. */
	OPENUT2K4_API bool WriteProperty(FUE2BitWriter& W, FUSProperty* Prop, const FUE2NetValue* Value, const FUE2WireContext& Ctx, int32 Depth = 0);
	OPENUT2K4_API bool ReadProperty(FUE2BitReader& R, FUSProperty* Prop, FUE2NetValue& Out, const FUE2WireContext& Ctx, int32 Depth = 0);
	OPENUT2K4_API bool WriteObject(FUE2BitWriter& W, const FUE2ObjectRef& Obj, const FUE2WireContext& Ctx);

	/** The parameters of a replicated function call (bools 1 bit, others a "present" bit and the value). Params by declaration order. */
	OPENUT2K4_API bool WriteFunctionParams(FUE2BitWriter& W, FUSFunction* Function, const TArray<FUE2NetValue>& Params, const FUE2WireContext& Ctx);
	OPENUT2K4_API bool ReadFunctionParams(FUE2BitReader& R, FUSFunction* Function, TArray<TPair<FName, FUE2NetValue>>& Out, const FUE2WireContext& Ctx);

	// value helpers
	inline FUE2NetValue Int(int32 V) { FUE2NetValue X; X.Kind = FUE2NetValue::EKind::Int; X.I = V; return X; }
	inline FUE2NetValue Byte(int32 V) { FUE2NetValue X; X.Kind = FUE2NetValue::EKind::Byte; X.I = V & 0xFF; return X; }
	inline FUE2NetValue Bool(bool V) { FUE2NetValue X; X.Kind = FUE2NetValue::EKind::Bool; X.I = V ? 1 : 0; return X; }
	inline FUE2NetValue Float(float V) { FUE2NetValue X; X.Kind = FUE2NetValue::EKind::Float; X.F = V; return X; }
	inline FUE2NetValue Str(const FString& V) { FUE2NetValue X; X.Kind = FUE2NetValue::EKind::Str; X.S = V; return X; }
	inline FUE2NetValue Name(FName V) { FUE2NetValue X; X.Kind = FUE2NetValue::EKind::Name; X.N = V; return X; }
	inline FUE2NetValue Vec(const FVector3f& V) { FUE2NetValue X; X.Kind = FUE2NetValue::EKind::Vector; X.V = V; return X; }
	inline FUE2NetValue Rot(const FIntVector& V) { FUE2NetValue X; X.Kind = FUE2NetValue::EKind::Rotator; X.R = V; return X; }
	/** An actor reference by id (0: None). */
	inline FUE2NetValue Actor(int32 Id) { FUE2NetValue X; X.Kind = FUE2NetValue::EKind::Object; X.Obj.Channel = Id; return X; }
	inline FUE2NetValue Static(const FUE2ObjectRef& Ref) { FUE2NetValue X; X.Kind = FUE2NetValue::EKind::Object; X.Obj = Ref; return X; }
	inline FUE2NetValue None() { FUE2NetValue X; X.Kind = FUE2NetValue::EKind::Object; return X; }
}
