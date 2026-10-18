#include "UE2ServerCodec.h"

#include "UE2NetBits.h"
#include "UE2NetPackageMap.h"
#include "../UScript/UScriptCore.h"

namespace UE2ServerCodecLocal
{
	constexpr uint32 UE2Parm = 0x00000080;
	constexpr uint32 UE2ReturnParm = 0x00000400;

	bool IsProperty(const FUSField* F)
	{
		return F && F->Class && F->Class->Name.ToString().EndsWith(TEXT("Property"));
	}

	uint32 ReadRawBits(FUE2BitReader& R, int32 Count)
	{
		uint32 Value = 0;
		for (int32 i = 0; i < Count; ++i)
		{
			if (R.ReadBit())
			{
				Value |= 1u << i;
			}
		}
		return Value;
	}

	void WriteRawBits(FUE2BitWriter& W, uint32 Value, int32 Count)
	{
		for (int32 i = 0; i < Count; ++i)
		{
			W.WriteBit((Value >> i) & 1);
		}
	}

	int32 EnumBits(const FUSProperty* Prop)
	{
		return Prop->Enum ? UE2CeilLogTwo(uint32(Prop->Enum->Names.Num())) : 8;
	}

	const FUE2NetValue* FindMember(const FUE2NetValue* Value, const FString& Key)
	{
		if (!Value)
		{
			return nullptr;
		}
		const FName K(*Key);
		for (const TPair<FName, FUE2NetValue>& M : Value->Members)
		{
			if (M.Key == K)
			{
				return &M.Value;
			}
		}
		return nullptr;
	}
}

using namespace UE2ServerCodecLocal;

FVector3f UE2Codec::ReadCompressedVector(FUE2BitReader& R)
{
	const uint32 Bits = R.ReadInt(20);
	const int32 Bias = 1 << (Bits + 1);
	const uint32 Max = 1u << (Bits + 2);
	const int32 X = int32(R.ReadInt(Max)) - Bias;
	const int32 Y = int32(R.ReadInt(Max)) - Bias;
	const int32 Z = int32(R.ReadInt(Max)) - Bias;
	return FVector3f(float(X), float(Y), float(Z));
}

void UE2Codec::WriteCompressedVector(FUE2BitWriter& W, const FVector3f& V)
{
	// UE2 FVector::SerializeCompressed: whole units with a shared bit count
	const int32 X = FMath::Clamp(FMath::RoundToInt(V.X), -(1 << 20), 1 << 20);
	const int32 Y = FMath::Clamp(FMath::RoundToInt(V.Y), -(1 << 20), 1 << 20);
	const int32 Z = FMath::Clamp(FMath::RoundToInt(V.Z), -(1 << 20), 1 << 20);
	const int32 MaxAbs = FMath::Max3(FMath::Abs(X), FMath::Abs(Y), FMath::Abs(Z));
	const uint32 Bits = uint32(FMath::Clamp(UE2CeilLogTwo(uint32(1 + MaxAbs)), 1, 20) - 1);
	W.WriteInt(Bits, 20);
	const int32 Bias = 1 << (Bits + 1);
	const uint32 Max = 1u << (Bits + 2);
	W.WriteInt(uint32(X + Bias), Max);
	W.WriteInt(uint32(Y + Bias), Max);
	W.WriteInt(uint32(Z + Bias), Max);
}

FIntVector UE2Codec::ReadCompressedRotator(FUE2BitReader& R)
{
	int32 C[3];
	for (int32& V : C)
	{
		V = R.ReadBit() ? int32(R.ReadByte()) << 8 : 0;
	}
	return FIntVector(C[0], C[1], C[2]);
}

void UE2Codec::WriteCompressedRotator(FUE2BitWriter& W, const FIntVector& Rot)
{
	for (int32 V : { Rot.X, Rot.Y, Rot.Z })
	{
		const uint8 B = uint8((V >> 8) & 0xFF);
		W.WriteBit(B != 0);
		if (B != 0)
		{
			W.WriteByte(B);
		}
	}
}

bool UE2Codec::WriteObject(FUE2BitWriter& W, const FUE2ObjectRef& Obj, const FUE2WireContext& Ctx)
{
	if (Obj.IsStatic() && Ctx.Map)
	{
		const int32 Pkg = Ctx.PackageToMap ? Ctx.PackageToMap(Obj.PackageIndex) : Obj.PackageIndex;
		const int32 Index = Pkg == INDEX_NONE ? INDEX_NONE : Ctx.Map->ObjectToIndex(Pkg, Obj.ExportIndex);
		if (Index != INDEX_NONE)
		{
			W.WriteBit(false);
			W.WriteInt(uint32(Index), uint32(FMath::Max(1, Ctx.Map->GetMaxObjectIndex())));
			return true;
		}
		// not in this connection's map (a skipped package): None
		W.WriteBit(true);
		W.WriteInt(0, MaxChannels);
		return true;
	}
	int32 Channel = 0;
	if (Obj.Channel != 0)
	{
		Channel = Ctx.ActorToChannel ? Ctx.ActorToChannel(Obj.Channel) : Obj.Channel;
	}
	W.WriteBit(true);
	W.WriteInt(uint32(FMath::Clamp(Channel, 0, int32(MaxChannels) - 1)), MaxChannels);
	return true;
}

bool UE2Codec::WriteProperty(FUE2BitWriter& W, FUSProperty* Prop, const FUE2NetValue* Value, const FUE2WireContext& Ctx, int32 Depth)
{
	if (!Prop || Depth > 8)
	{
		return false;
	}
	switch (Prop->Type)
	{
	case EUSType::Byte:
		WriteRawBits(W, Value ? uint32(Value->I) : 0u, EnumBits(Prop));
		return true;
	case EUSType::Int:
		W.WriteInt32(Value ? (Value->Kind == FUE2NetValue::EKind::Float ? int32(Value->F) : Value->I) : 0);
		return true;
	case EUSType::Bool:
		W.WriteBit(Value && Value->I != 0);
		return true;
	case EUSType::Float:
		W.WriteFloat(Value ? (Value->Kind == FUE2NetValue::EKind::Float ? Value->F : float(Value->I)) : 0.f);
		return true;
	case EUSType::Str:
		W.WriteString(Value ? Value->S : FString());
		return true;
	case EUSType::Name:
	{
		const int32 Max = Ctx.Map ? Ctx.Map->GetMaxNameIndex() : 0;
		const int32 Index = (Value && Ctx.Map) ? Ctx.Map->NameToIndex(Value->N) : Max;
		W.WriteInt(uint32(Index), uint32(Max + 1));
		return true;
	}
	case EUSType::Object:
	case EUSType::Class:
		return WriteObject(W, Value ? Value->Obj : FUE2ObjectRef(), Ctx);
	case EUSType::Struct:
	{
		FUSStruct* Struct = Prop->Struct;
		if (!Struct)
		{
			return false;
		}
		const FName StructName = Struct->Name;
		if (StructName == TEXT("Vector"))
		{
			WriteCompressedVector(W, Value ? Value->V : FVector3f::ZeroVector);
			return true;
		}
		if (StructName == TEXT("Rotator"))
		{
			WriteCompressedRotator(W, Value ? Value->R : FIntVector::ZeroValue);
			return true;
		}
		if (StructName == TEXT("Plane"))
		{
			const FVector4f P = Value ? Value->P : FVector4f(0, 0, 0, 0);
			for (float C : { P.X, P.Y, P.Z, P.W })
			{
				const int16 S = int16(FMath::Clamp(FMath::RoundToInt(C), -32768, 32767));
				W.WriteBytes(&S, 2);
			}
			return true;
		}
		if (StructName == TEXT("CompressedPosition"))
		{
			// Pawn.PawnPosition: location, pitch and yaw bytes, velocity (Docs/UE2Net.md)
			const FUE2NetValue* L = FindMember(Value, TEXT("Location"));
			const FUE2NetValue* Rt = FindMember(Value, TEXT("Rotation"));
			const FUE2NetValue* V = FindMember(Value, TEXT("Velocity"));
			WriteCompressedVector(W, L ? L->V : FVector3f::ZeroVector);
			W.WriteByte(uint8(((Rt ? Rt->R.X : 0) >> 8) & 0xFF));
			W.WriteByte(uint8(((Rt ? Rt->R.Y : 0) >> 8) & 0xFF));
			WriteCompressedVector(W, V ? V->V : FVector3f::ZeroVector);
			return true;
		}
		if (StructName == TEXT("Quat"))
		{
			// X, Y, Z only (W implied, >= 0)
			const FUE2NetValue* X = FindMember(Value, TEXT("X"));
			const FUE2NetValue* Y = FindMember(Value, TEXT("Y"));
			const FUE2NetValue* Z = FindMember(Value, TEXT("Z"));
			const FUE2NetValue* Wv = FindMember(Value, TEXT("W"));
			const float Sign = (Wv && Wv->F < 0.f) ? -1.f : 1.f;
			W.WriteFloat(X ? X->F * Sign : 0.f);
			W.WriteFloat(Y ? Y->F * Sign : 0.f);
			W.WriteFloat(Z ? Z->F * Sign : 0.f);
			return true;
		}
		// generic: the struct's own members, then its super struct's, in field order
		for (FUSStruct* S = Struct; S; S = S->GetSuperStruct())
		{
			int32 Guard = 0;
			for (FUSField* F = S->Children; F && Guard < 10000; F = F->Next, ++Guard)
			{
				if (!IsProperty(F) || !Ctx.Map || Ctx.Map->ObjectToIndex(F) == INDEX_NONE)
				{
					continue;
				}
				FUSProperty* P = static_cast<FUSProperty*>(F);
				for (int32 i = 0; i < P->ArrayDim; ++i)
				{
					const FString Key = P->ArrayDim > 1 ? FString::Printf(TEXT("%s[%d]"), *P->Name.ToString(), i) : P->Name.ToString();
					if (!WriteProperty(W, P, FindMember(Value, Key), Ctx, Depth + 1))
					{
						return false;
					}
				}
			}
		}
		return true;
	}
	default:
		return false;
	}
}

bool UE2Codec::ReadProperty(FUE2BitReader& R, FUSProperty* Prop, FUE2NetValue& Out, const FUE2WireContext& Ctx, int32 Depth)
{
	if (!Prop || Depth > 8)
	{
		return false;
	}
	switch (Prop->Type)
	{
	case EUSType::Byte:
		Out.Kind = FUE2NetValue::EKind::Byte;
		Out.I = int32(ReadRawBits(R, EnumBits(Prop)));
		break;
	case EUSType::Int:
		Out.Kind = FUE2NetValue::EKind::Int;
		Out.I = R.ReadInt32();
		break;
	case EUSType::Bool:
		Out.Kind = FUE2NetValue::EKind::Bool;
		Out.I = R.ReadBit() ? 1 : 0;
		break;
	case EUSType::Float:
		Out.Kind = FUE2NetValue::EKind::Float;
		Out.F = R.ReadFloat();
		break;
	case EUSType::Str:
		Out.Kind = FUE2NetValue::EKind::Str;
		Out.S = R.ReadString();
		break;
	case EUSType::Name:
	{
		Out.Kind = FUE2NetValue::EKind::Name;
		const int32 Max = Ctx.Map ? Ctx.Map->GetMaxNameIndex() : 0;
		const int32 Index = int32(R.ReadInt(uint32(Max + 1)));
		if (Ctx.Map)
		{
			Ctx.Map->IndexToName(Index, Out.N);
		}
		break;
	}
	case EUSType::Object:
	case EUSType::Class:
		Out.Kind = FUE2NetValue::EKind::Object;
		Out.Obj = FUE2ObjectRef();
		if (R.ReadBit())
		{
			const int32 Channel = int32(R.ReadInt(MaxChannels));
			Out.Obj.Channel = (Channel != 0 && Ctx.ChannelToActor) ? Ctx.ChannelToActor(Channel) : Channel;
		}
		else if (Ctx.Map)
		{
			const int32 Index = int32(R.ReadInt(uint32(FMath::Max(1, Ctx.Map->GetMaxObjectIndex()))));
			Ctx.Map->IndexToObject(Index, Out.Obj);
		}
		break;
	case EUSType::Struct:
	{
		FUSStruct* Struct = Prop->Struct;
		if (!Struct)
		{
			return false;
		}
		const FName StructName = Struct->Name;
		if (StructName == TEXT("Vector"))
		{
			Out.Kind = FUE2NetValue::EKind::Vector;
			Out.V = ReadCompressedVector(R);
		}
		else if (StructName == TEXT("Rotator"))
		{
			Out.Kind = FUE2NetValue::EKind::Rotator;
			Out.R = ReadCompressedRotator(R);
		}
		else if (StructName == TEXT("Plane"))
		{
			Out.Kind = FUE2NetValue::EKind::Plane;
			const float X = R.ReadInt16(), Y = R.ReadInt16(), Z = R.ReadInt16(), Wc = R.ReadInt16();
			Out.P = FVector4f(X, Y, Z, Wc);
		}
		else if (StructName == TEXT("CompressedPosition"))
		{
			Out.Kind = FUE2NetValue::EKind::Struct;
			FUE2NetValue L = Vec(ReadCompressedVector(R));
			const int32 Pitch = int32(R.ReadByte()) << 8;
			const int32 Yaw = int32(R.ReadByte()) << 8;
			FUE2NetValue V = Vec(ReadCompressedVector(R));
			Out.Members.Emplace(TEXT("Location"), MoveTemp(L));
			Out.Members.Emplace(TEXT("Rotation"), Rot(FIntVector(Pitch, Yaw, 0)));
			Out.Members.Emplace(TEXT("Velocity"), MoveTemp(V));
		}
		else if (StructName == TEXT("Quat"))
		{
			Out.Kind = FUE2NetValue::EKind::Struct;
			const float X = R.ReadFloat(), Y = R.ReadFloat(), Z = R.ReadFloat();
			Out.Members.Emplace(TEXT("X"), Float(X));
			Out.Members.Emplace(TEXT("Y"), Float(Y));
			Out.Members.Emplace(TEXT("Z"), Float(Z));
			Out.Members.Emplace(TEXT("W"), Float(FMath::Sqrt(FMath::Max(0.f, 1.f - X * X - Y * Y - Z * Z))));
		}
		else
		{
			Out.Kind = FUE2NetValue::EKind::Struct;
			for (FUSStruct* S = Struct; S; S = S->GetSuperStruct())
			{
				int32 Guard = 0;
				for (FUSField* F = S->Children; F && Guard < 10000; F = F->Next, ++Guard)
				{
					if (!IsProperty(F) || !Ctx.Map || Ctx.Map->ObjectToIndex(F) == INDEX_NONE)
					{
						continue;
					}
					FUSProperty* P = static_cast<FUSProperty*>(F);
					for (int32 i = 0; i < P->ArrayDim; ++i)
					{
						FUE2NetValue Member;
						if (!ReadProperty(R, P, Member, Ctx, Depth + 1))
						{
							return false;
						}
						const FName Key = P->ArrayDim > 1 ? FName(*FString::Printf(TEXT("%s[%d]"), *P->Name.ToString(), i)) : P->Name;
						Out.Members.Emplace(Key, MoveTemp(Member));
					}
				}
			}
		}
		break;
	}
	default:
		return false;
	}
	return !R.IsError();
}

bool UE2Codec::WriteFunctionParams(FUE2BitWriter& W, FUSFunction* Function, const TArray<FUE2NetValue>& Params, const FUE2WireContext& Ctx)
{
	int32 ParmIndex = 0;
	for (FUSField* F = Function ? Function->Children : nullptr; F; F = F->Next)
	{
		if (!IsProperty(F))
		{
			break;
		}
		FUSProperty* P = static_cast<FUSProperty*>(F);
		if ((P->PropertyFlags & (UE2ServerCodecLocal::UE2Parm | UE2ServerCodecLocal::UE2ReturnParm)) != UE2ServerCodecLocal::UE2Parm)
		{
			break;
		}
		const FUE2NetValue* V = Params.IsValidIndex(ParmIndex) ? &Params[ParmIndex] : nullptr;
		++ParmIndex;
		if (Ctx.Map && Ctx.Map->ObjectToIndex(P) == INDEX_NONE)
		{
			continue;
		}
		if (P->Type == EUSType::Bool)
		{
			W.WriteBit(V && V->I != 0);
			continue;
		}
		const bool bPresent = V && V->Kind != FUE2NetValue::EKind::None;
		W.WriteBit(bPresent);
		if (bPresent && !WriteProperty(W, P, V, Ctx))
		{
			return false;
		}
	}
	return true;
}

bool UE2Codec::ReadFunctionParams(FUE2BitReader& R, FUSFunction* Function, TArray<TPair<FName, FUE2NetValue>>& Out, const FUE2WireContext& Ctx)
{
	int32 Guard = 0;
	for (FUSField* F = Function ? Function->Children : nullptr; F && Guard < 1000; F = F->Next, ++Guard)
	{
		if (!IsProperty(F))
		{
			break;
		}
		FUSProperty* P = static_cast<FUSProperty*>(F);
		if ((P->PropertyFlags & (UE2ServerCodecLocal::UE2Parm | UE2ServerCodecLocal::UE2ReturnParm)) != UE2ServerCodecLocal::UE2Parm)
		{
			break;
		}
		if (Ctx.Map && Ctx.Map->ObjectToIndex(P) == INDEX_NONE)
		{
			continue;
		}
		FUE2NetValue Value;
		if (P->Type == EUSType::Bool || R.ReadBit())
		{
			if (!ReadProperty(R, P, Value, Ctx))
			{
				return false;
			}
		}
		Out.Emplace(P->Name, MoveTemp(Value));
	}
	return !R.IsError();
}
