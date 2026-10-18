#include "ArenaRuntimeGltf.h"
#include "OpenUT2k4.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

bool FArenaGltf::Load(const FString& Path)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Doc) || !Doc.IsValid())
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("glTF: cannot parse %s"), *Path);
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Buffers = Array(TEXT("buffers"));
	FString Uri;
	if (Buffers && Buffers->Num() > 0 && (*Buffers)[0]->AsObject()->TryGetStringField(TEXT("uri"), Uri))
	{
		if (!FFileHelper::LoadFileToArray(Bin, *(FPaths::GetPath(Path) / Uri)))
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("glTF: missing buffer %s for %s"), *Uri, *Path);
			return false;
		}
	}
	return true;
}

const TArray<TSharedPtr<FJsonValue>>* FArenaGltf::Array(const TCHAR* Field) const
{
	const TArray<TSharedPtr<FJsonValue>>* Out = nullptr;
	return Doc.IsValid() && Doc->TryGetArrayField(Field, Out) ? Out : nullptr;
}

TSharedPtr<FJsonObject> FArenaGltf::Item(const TCHAR* Field, int32 Index) const
{
	const TArray<TSharedPtr<FJsonValue>>* A = Array(Field);
	return A && A->IsValidIndex(Index) && (*A)[Index].IsValid() ? (*A)[Index]->AsObject() : nullptr;
}

namespace ArenaGltfLocal
{
	int32 NumComponents(const FString& Type)
	{
		if (Type == TEXT("SCALAR")) return 1;
		if (Type == TEXT("VEC2")) return 2;
		if (Type == TEXT("VEC3")) return 3;
		if (Type == TEXT("VEC4")) return 4;
		if (Type == TEXT("MAT4")) return 16;
		if (Type == TEXT("MAT3")) return 9;
		if (Type == TEXT("MAT2")) return 4;
		return 0;
	}

	int32 ComponentSize(int32 ComponentType)
	{
		switch (ComponentType)
		{
		case 5120: case 5121: return 1;
		case 5122: case 5123: return 2;
		case 5125: case 5126: return 4;
		default: return 0;
		}
	}

	/** Walks every component of an accessor: Visit(element, component, const uint8* data). */
	template <typename FVisit>
	bool ForEach(const FArenaGltf& G, int32 Accessor, int32& OutCount, int32& OutComps, int32& OutType, FVisit&& Visit)
	{
		const TSharedPtr<FJsonObject> Acc = G.Item(TEXT("accessors"), Accessor);
		if (!Acc.IsValid())
		{
			return false;
		}
		OutCount = Acc->GetIntegerField(TEXT("count"));
		OutComps = NumComponents(Acc->GetStringField(TEXT("type")));
		OutType = Acc->GetIntegerField(TEXT("componentType"));
		const int32 CSize = ComponentSize(OutType);
		int32 ViewIndex = -1;
		if (!OutComps || !CSize || !Acc->TryGetNumberField(TEXT("bufferView"), ViewIndex))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> View = G.Item(TEXT("bufferViews"), ViewIndex);
		if (!View.IsValid())
		{
			return false;
		}
		int64 ViewOffset = 0, AccOffset = 0;
		int32 Stride = 0;
		View->TryGetNumberField(TEXT("byteOffset"), ViewOffset);
		View->TryGetNumberField(TEXT("byteStride"), Stride);
		Acc->TryGetNumberField(TEXT("byteOffset"), AccOffset);
		const int32 ElemSize = CSize * OutComps;
		if (Stride <= 0)
		{
			Stride = ElemSize;
		}
		const int64 Start = ViewOffset + AccOffset;
		if (OutCount < 0 || Start < 0 || (OutCount > 0 && Start + int64(OutCount - 1) * Stride + ElemSize > G.Bin.Num()))
		{
			return false;
		}
		const uint8* Base = G.Bin.GetData() + Start;
		for (int32 E = 0; E < OutCount; ++E)
		{
			const uint8* P = Base + int64(E) * Stride;
			for (int32 C = 0; C < OutComps; ++C)
			{
				Visit(E, C, P + C * CSize);
			}
		}
		return true;
	}
}

bool FArenaGltf::AccessorInfo(int32 Accessor, int32& OutCount, int32& OutComponents) const
{
	const TSharedPtr<FJsonObject> Acc = Item(TEXT("accessors"), Accessor);
	if (!Acc.IsValid())
	{
		return false;
	}
	OutCount = Acc->GetIntegerField(TEXT("count"));
	OutComponents = ArenaGltfLocal::NumComponents(Acc->GetStringField(TEXT("type")));
	return OutComponents > 0;
}

bool FArenaGltf::ReadFloats(int32 Accessor, TArray<float>& Out, int32* OutComponents) const
{
	int32 Count = 0, Comps = 0, Type = 0;
	Out.Reset();
	const TSharedPtr<FJsonObject> Acc = Item(TEXT("accessors"), Accessor);
	const bool bNormalized = Acc.IsValid() && Acc->HasTypedField<EJson::Boolean>(TEXT("normalized")) && Acc->GetBoolField(TEXT("normalized"));
	int32 Cursor = 0;
	bool bSized = false;
	const bool bOk = ArenaGltfLocal::ForEach(*this, Accessor, Count, Comps, Type, [&](int32, int32, const uint8* P)
	{
		if (!bSized)
		{
			Out.SetNumUninitialized(Count * Comps);
			bSized = true;
		}
		float V = 0.f;
		switch (Type)
		{
		case 5126: FMemory::Memcpy(&V, P, 4); break;
		case 5121: V = bNormalized ? *P / 255.f : float(*P); break;
		case 5120: V = bNormalized ? FMath::Max(int8(*P) / 127.f, -1.f) : float(int8(*P)); break;
		case 5123: { uint16 X; FMemory::Memcpy(&X, P, 2); V = bNormalized ? X / 65535.f : float(X); break; }
		case 5122: { int16 X; FMemory::Memcpy(&X, P, 2); V = bNormalized ? FMath::Max(X / 32767.f, -1.f) : float(X); break; }
		case 5125: { uint32 X; FMemory::Memcpy(&X, P, 4); V = float(X); break; }
		}
		Out[Cursor++] = V;
	});
	if (OutComponents)
	{
		*OutComponents = Comps;
	}
	return bOk;
}

bool FArenaGltf::ReadUInts(int32 Accessor, TArray<uint32>& Out, int32* OutComponents) const
{
	int32 Count = 0, Comps = 0, Type = 0;
	Out.Reset();
	int32 Cursor = 0;
	bool bSized = false;
	const bool bOk = ArenaGltfLocal::ForEach(*this, Accessor, Count, Comps, Type, [&](int32, int32, const uint8* P)
	{
		if (!bSized)
		{
			Out.SetNumUninitialized(Count * Comps);
			bSized = true;
		}
		uint32 V = 0;
		switch (Type)
		{
		case 5121: case 5120: V = *P; break;
		case 5123: case 5122: { uint16 X; FMemory::Memcpy(&X, P, 2); V = X; break; }
		case 5125: FMemory::Memcpy(&V, P, 4); break;
		case 5126: { float F; FMemory::Memcpy(&F, P, 4); V = uint32(FMath::Max(F, 0.f)); break; }
		}
		Out[Cursor++] = V;
	});
	if (OutComponents)
	{
		*OutComponents = Comps;
	}
	return bOk;
}

FString FArenaGltf::MaterialName(int32 Index) const
{
	const TSharedPtr<FJsonObject> M = Item(TEXT("materials"), Index);
	FString Name;
	return M.IsValid() && M->TryGetStringField(TEXT("name"), Name) ? Name : FString(TEXT("None"));
}
