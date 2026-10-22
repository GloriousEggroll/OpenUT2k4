#include "ArenaConvPackage.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Async/MappedFileHandle.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace ArenaConvPackageLocal
{
	constexpr uint32 PackageTag = 0x9E2A83C1;
	constexpr uint32 RF_HasStack = 0x02000000;

	enum EPropType : int32
	{
		PT_BYTE = 1, PT_INT, PT_BOOL, PT_FLOAT, PT_OBJECT, PT_NAME, PT_STRING, PT_CLASS,
		PT_ARRAY, PT_STRUCT, PT_VECTOR, PT_ROTATOR, PT_STR, PT_MAP, PT_FIXEDARRAY
	};

	const TCHAR* TypeName(int32 T)
	{
		static const TCHAR* Names[] = { TEXT("?"), TEXT("byte"), TEXT("int"), TEXT("bool"), TEXT("float"), TEXT("object"), TEXT("name"),
			TEXT("string"), TEXT("class"), TEXT("array"), TEXT("struct"), TEXT("vector"), TEXT("rotator"), TEXT("str"), TEXT("map"), TEXT("fixedarray") };
		return T >= 1 && T <= 15 ? Names[T] : TEXT("?");
	}

	FString Lower(const FString& S) { return S.ToLower(); }

	/** Raw placeholder {'_raw': size, '_type': type} */
	FConvProp RawValue(int32 Size, int32 Type, int32 Count = -1, const FString& StructName = FString(), bool bStruct = false)
	{
		FConvProp P = FConvProp::MakeStruct();
		if (bStruct)
		{
			P.Set(TEXT("_struct"), StructName.IsEmpty() ? FConvProp() : FConvProp::MakeStr(StructName));
			P.Set(TEXT("_raw"), FConvProp::MakeInt(Size));
			return P;
		}
		P.Set(TEXT("_raw"), FConvProp::MakeInt(Size));
		P.Set(TEXT("_type"), FConvProp::MakeStr(TypeName(Type)));
		if (Count >= 0)
		{
			P.Set(TEXT("_count"), FConvProp::MakeInt(Count));
		}
		return P;
	}

	bool LooksLikeFloat(uint32 U)
	{
		if (U == 0)
		{
			return true;
		}
		const uint32 E = (U >> 23) & 0xFF;
		return 90 < E && E < 170;
	}

	/** Element-type hints for well-known dynamic array properties (properties.py ARRAY_HINTS). */
	const FString* WellKnownHint(const FString& LowerName)
	{
		static const TMap<FString, FString> Hints = {
			{ TEXT("materials"), TEXT("") }, { TEXT("skins"), TEXT("object") }, { TEXT("sounds"), TEXT("object") },
			{ TEXT("decolayers"), TEXT("struct") }, { TEXT("alternatepaths"), TEXT("object") }, { TEXT("excludetag"), TEXT("name") },
			{ TEXT("pathlist"), TEXT("object") },
		};
		return Hints.Find(LowerName);
	}

	bool ReadValue(FConvReader& R, FConvPackage* Pkg, int32 Type, const FString& StructName, int32 Size, const FString& PropName,
		const TMap<FString, FString>* Hints, bool bRelaxed, FConvProp& Out);

	// ---- native structs ----
	bool ReadNativeStruct(FConvReader& R, const FString& LowerName, FConvProp& Out, int32& OutSize)
	{
		auto Vec = [&R]() { float V[3]; R.Vector(V); return FConvProp::MakeVector(V); };
		auto Range = [&R]() { FConvProp P = FConvProp::MakeStruct(); P.Set(TEXT("Min"), FConvProp::MakeFloat(R.F32())); P.Set(TEXT("Max"), FConvProp::MakeFloat(R.F32())); return P; };
		if (LowerName == TEXT("vector")) { OutSize = 12; Out = Vec(); }
		else if (LowerName == TEXT("rotator")) { OutSize = 12; Out = FConvProp(); Out.Kind = FConvProp::EKind::Rotator; Out.R[0] = R.I32(); Out.R[1] = R.I32(); Out.R[2] = R.I32(); }
		else if (LowerName == TEXT("color"))
		{
			OutSize = 4;
			Out = FConvProp::MakeStruct();
			// UE2 struct Color is declared (and stored) B, G, R, A; keys in R G B A order
			// (checked against texture MipZero averages: the first byte is blue)
			const int32 C0 = R.U8(), C1 = R.U8(), C2 = R.U8(), C3 = R.U8();
			Out.Set(TEXT("R"), FConvProp::MakeInt(C2)); Out.Set(TEXT("G"), FConvProp::MakeInt(C1));
			Out.Set(TEXT("B"), FConvProp::MakeInt(C0)); Out.Set(TEXT("A"), FConvProp::MakeInt(C3));
		}
		else if (LowerName == TEXT("plane") || LowerName == TEXT("sphere") || LowerName == TEXT("quat"))
		{
			OutSize = 16;
			Out = FConvProp::MakeStruct();
			const float X = R.F32(), Y = R.F32(), Z = R.F32(), W = R.F32();
			Out.Set(TEXT("X"), FConvProp::MakeFloat(X)); Out.Set(TEXT("Y"), FConvProp::MakeFloat(Y));
			Out.Set(TEXT("Z"), FConvProp::MakeFloat(Z)); Out.Set(TEXT("W"), FConvProp::MakeFloat(W));
		}
		else if (LowerName == TEXT("scale"))
		{
			OutSize = 17;
			Out = FConvProp::MakeStruct();
			Out.Set(TEXT("Scale"), Vec());
			Out.Set(TEXT("SheerRate"), FConvProp::MakeFloat(R.F32()));
			Out.Set(TEXT("SheerAxis"), FConvProp::MakeInt(R.U8()));
		}
		else if (LowerName == TEXT("box"))
		{
			OutSize = 25;
			Out = FConvProp::MakeStruct();
			Out.Set(TEXT("Min"), Vec());
			Out.Set(TEXT("Max"), Vec());
			Out.Set(TEXT("IsValid"), FConvProp::MakeInt(R.U8()));
		}
		else if (LowerName == TEXT("coords"))
		{
			OutSize = 48;
			Out = FConvProp::MakeStruct();
			Out.Set(TEXT("Origin"), Vec()); Out.Set(TEXT("XAxis"), Vec()); Out.Set(TEXT("YAxis"), Vec()); Out.Set(TEXT("ZAxis"), Vec());
		}
		else if (LowerName == TEXT("range")) { OutSize = 8; Out = Range(); }
		else if (LowerName == TEXT("rangevector"))
		{
			OutSize = 24;
			Out = FConvProp::MakeStruct();
			Out.Set(TEXT("X"), Range()); Out.Set(TEXT("Y"), Range()); Out.Set(TEXT("Z"), Range());
		}
		else if (LowerName == TEXT("guid"))
		{
			OutSize = 16;
			const uint32 A = R.U32(), B = R.U32(), C = R.U32(), D = R.U32();
			Out = FConvProp::MakeStr(FString::Printf(TEXT("%08X-%08X-%08X-%08X"), A, B, C, D));
		}
		else if (LowerName == TEXT("vector2d"))
		{
			OutSize = 8;
			Out = FConvProp::MakeStruct();
			const float X = R.F32(), Y = R.F32();
			Out.Set(TEXT("X"), FConvProp::MakeFloat(X)); Out.Set(TEXT("Y"), FConvProp::MakeFloat(Y));
		}
		else if (LowerName == TEXT("adrop"))
		{
			// FractalTexture.ADrop (WaterTexture / WetTexture drops): 8 bytes, stored binary like the other natives
			OutSize = 8;
			Out = FConvProp::MakeStruct();
			const TCHAR* Fields[8] = { TEXT("Type"), TEXT("Depth"), TEXT("X"), TEXT("Y"), TEXT("ByteA"), TEXT("ByteB"), TEXT("ByteC"), TEXT("ByteD") };
			for (const TCHAR* F : Fields)
			{
				Out.Set(F, FConvProp::MakeInt(R.U8()));
			}
		}
		else if (LowerName == TEXT("intpoint"))
		{
			OutSize = 8;
			Out = FConvProp::MakeStruct();
			const int32 X = R.I32(), Y = R.I32();
			Out.Set(TEXT("X"), FConvProp::MakeInt(X)); Out.Set(TEXT("Y"), FConvProp::MakeInt(Y));
		}
		else
		{
			return false;
		}
		return true;
	}

	bool IsNativeStruct(const FString& LowerName, int32& OutSize)
	{
		static const TMap<FString, int32> Sizes = {
			{ TEXT("vector"), 12 }, { TEXT("rotator"), 12 }, { TEXT("color"), 4 }, { TEXT("plane"), 16 }, { TEXT("sphere"), 16 },
			{ TEXT("quat"), 16 }, { TEXT("scale"), 17 }, { TEXT("box"), 25 }, { TEXT("coords"), 48 }, { TEXT("range"), 8 },
			{ TEXT("rangevector"), 24 }, { TEXT("guid"), 16 }, { TEXT("vector2d"), 8 }, { TEXT("intpoint"), 8 }, { TEXT("adrop"), 8 } };
		const int32* S = Sizes.Find(LowerName);
		if (S)
		{
			OutSize = *S;
		}
		return S != nullptr;
	}

	bool ReadStruct(FConvReader& R, FConvPackage* Pkg, const FString& StructName, int32 Size, const TMap<FString, FString>* Hints, bool bRelaxed, FConvProp& Out)
	{
		const FString LowerName = StructName.ToLower();
		int32 NatSize = 0;
		const bool bNative = IsNativeStruct(LowerName, NatSize);
		if (bNative && NatSize == Size && !bRelaxed)
		{
			int32 Dummy;
			ReadNativeStruct(R, LowerName, Out, Dummy);
			return !R.bError;
		}
		const int64 Start = R.Pos;
		{
			FConvReader Sub = R;
			FConvProps Props;
			const bool bOk = ArenaConvProps::ReadProperties(Sub, Pkg, Props, Hints);
			if (bOk && (bRelaxed || Sub.Pos == Sub.End || Sub.Pos + 1 == Sub.End))
			{
				Out = FConvProp::MakeStruct();
				Out.M = MoveTemp(Props);
				Out.Set(TEXT("_struct"), StructName.IsEmpty() ? FConvProp() : FConvProp::MakeStr(StructName));
				R.Pos = Sub.Pos;
				return true;
			}
			if (!bOk && bRelaxed)
			{
				R.Fail(Sub.Error);
				return false;
			}
		}
		R.Pos = Start;
		if (bNative)
		{
			FConvReader Sub = R;
			int32 Dummy;
			FConvProp V;
			ReadNativeStruct(Sub, LowerName, V, Dummy);
			if (!Sub.bError)
			{
				R.Pos = Sub.Pos;
				Out = MoveTemp(V);
				return true;
			}
		}
		Out = RawValue(Size, PT_STRUCT, -1, StructName, true);
		return true;
	}

	bool ReadArray(FConvReader& R, FConvPackage* Pkg, const FString& PropName, int32 Size, const TMap<FString, FString>* Hints, bool bRelaxed, FConvProp& Out)
	{
		const int32 Count = R.Index();
		if (R.bError)
		{
			return false;
		}
		if (Count == 0)
		{
			Out = FConvProp::MakeArray();
			return true;
		}
		const int64 DataStart = R.Pos;
		const int64 DataLen = R.End - DataStart;
		if (Count < 0 || Count > DataLen)
		{
			return R.Fail(TEXT("bad array count"));
		}
		FString Hint;
		{
			const FString* H = Hints ? Hints->Find(PropName.ToLower()) : nullptr;
			if (H && !H->IsEmpty())
			{
				Hint = *H;
			}
			else
			{
				const FString* W = WellKnownHint(PropName.ToLower());
				if (W)
				{
					Hint = *W;
				}
			}
		}

		auto TryStructs = [&](FConvProp& Res) -> bool
		{
			FConvReader Sub = R;
			Sub.Pos = DataStart;
			FConvProp Arr = FConvProp::MakeArray();
			for (int32 i = 0; i < Count; ++i)
			{
				FConvProps P;
				if (!ArenaConvProps::ReadProperties(Sub, Pkg, P, Hints))
				{
					return false;
				}
				FConvProp E = FConvProp::MakeStruct();
				E.M = MoveTemp(P);
				Arr.A.Add(MoveTemp(E));
			}
			if (bRelaxed || Sub.Pos == Sub.End || Sub.Pos == Sub.End - 1)
			{
				R.Pos = Sub.Pos;
				Res = MoveTemp(Arr);
				return true;
			}
			return false;
		};

		if (bRelaxed)
		{
			FConvProp Res;
			if (TryStructs(Res))
			{
				Out = MoveTemp(Res);
				return true;
			}
			return R.Fail(TEXT("relaxed struct array failed"));
		}

		auto TryIndices = [&](bool bNames, FConvProp& Res) -> bool
		{
			FConvReader Sub = R;
			Sub.Pos = DataStart;
			TArray<int32> Idx;
			Idx.Reserve(Count);
			for (int32 i = 0; i < Count; ++i)
			{
				Idx.Add(Sub.Index());
			}
			if (Sub.bError || Sub.Pos != Sub.End)
			{
				return false;
			}
			FConvProp Arr = FConvProp::MakeArray();
			if (bNames)
			{
				for (int32 I : Idx)
				{
					FString N;
					if (!Pkg->NameAt(I, N))
					{
						return false;
					}
					Arr.A.Add(FConvProp::MakeStr(N));
				}
			}
			else
			{
				const int32 NImp = Pkg->Imports.Num(), NExp = Pkg->Exports.Num();
				for (int32 I : Idx)
				{
					if (!(-NImp <= I && I <= NExp))
					{
						return false;
					}
				}
				for (int32 I : Idx)
				{
					Arr.A.Add(FConvProp::MakeRef(Pkg, I));
				}
			}
			R.Pos = Sub.Pos;
			Res = MoveTemp(Arr);
			return true;
		};

		auto TryStrings = [&](FConvProp& Res) -> bool
		{
			FConvReader Sub = R;
			Sub.Pos = DataStart;
			FConvProp Arr = FConvProp::MakeArray();
			for (int32 i = 0; i < Count; ++i)
			{
				FString S = Sub.FString_();
				if (Sub.bError)
				{
					return false;
				}
				Arr.A.Add(FConvProp::MakeStr(S));
			}
			if (Sub.Pos != Sub.End)
			{
				return false;
			}
			R.Pos = Sub.Pos;
			Res = MoveTemp(Arr);
			return true;
		};

		auto TryScalars = [&](int32 Kind, FConvProp& Res) -> bool // 0 auto, 1 float, 2 int
		{
			if (DataLen != 4 * int64(Count))
			{
				return false;
			}
			FConvReader Sub = R;
			Sub.Pos = DataStart;
			TArray<uint32> Raw;
			for (int32 i = 0; i < Count; ++i)
			{
				Raw.Add(Sub.U32());
			}
			bool bFloat = Kind == 1;
			if (Kind == 0)
			{
				bFloat = true;
				for (uint32 U : Raw)
				{
					bFloat &= LooksLikeFloat(U);
				}
			}
			FConvProp Arr = FConvProp::MakeArray();
			for (uint32 U : Raw)
			{
				if (bFloat)
				{
					float F;
					FMemory::Memcpy(&F, &U, 4);
					Arr.A.Add(FConvProp::MakeFloat(F));
				}
				else
				{
					Arr.A.Add(FConvProp::MakeInt((int32)U));
				}
			}
			R.Pos = Sub.Pos;
			Res = MoveTemp(Arr);
			return true;
		};

		auto TryBytes = [&](FConvProp& Res) -> bool
		{
			if (DataLen != Count)
			{
				return false;
			}
			FConvProp Arr = FConvProp::MakeArray();
			for (int32 i = 0; i < Count; ++i)
			{
				Arr.A.Add(FConvProp::MakeInt(R.Buf[DataStart + i]));
			}
			R.Pos = DataStart + Count;
			Res = MoveTemp(Arr);
			return true;
		};

		auto TryVectors = [&](FConvProp& Res) -> bool
		{
			if (DataLen != 12 * int64(Count))
			{
				return false;
			}
			FConvReader Sub = R;
			Sub.Pos = DataStart;
			FConvProp Arr = FConvProp::MakeArray();
			for (int32 i = 0; i < Count; ++i)
			{
				float V[3];
				Sub.Vector(V);
				Arr.A.Add(FConvProp::MakeVector(V));
			}
			R.Pos = Sub.Pos;
			Res = MoveTemp(Arr);
			return true;
		};

		enum EFn { FStructs, FObjects, FNames, FStrings, FFloats, FInts, FBytes, FVectors, FScalars };
		TArray<EFn, TInlineAllocator<12>> Order;
		if (Hint == TEXT("struct")) Order.Add(FStructs);
		else if (Hint == TEXT("object")) Order.Add(FObjects);
		else if (Hint == TEXT("name")) Order.Add(FNames);
		else if (Hint == TEXT("string")) Order.Add(FStrings);
		else if (Hint == TEXT("float")) Order.Add(FFloats);
		else if (Hint == TEXT("int")) Order.Add(FInts);
		else if (Hint == TEXT("byte") || Hint == TEXT("bool")) Order.Add(FBytes);
		else if (Hint == TEXT("vector")) { Order.Add(FVectors); Order.Add(FStructs); }
		if (DataLen <= 3 * int64(Count))
		{
			Order.Add(FObjects); Order.Add(FBytes); Order.Add(FStructs);
		}
		else
		{
			Order.Add(FStructs); Order.Add(FObjects); Order.Add(FStrings); Order.Add(FVectors); Order.Add(FScalars);
		}
		for (EFn Fn : Order)
		{
			FConvProp Res;
			bool bOk = false;
			switch (Fn)
			{
			case FStructs: bOk = TryStructs(Res); break;
			case FObjects: bOk = TryIndices(false, Res); break;
			case FNames: bOk = TryIndices(true, Res); break;
			case FStrings: bOk = TryStrings(Res); break;
			case FFloats: bOk = TryScalars(1, Res); break;
			case FInts: bOk = TryScalars(2, Res); break;
			case FBytes: bOk = TryBytes(Res); break;
			case FVectors: bOk = TryVectors(Res); break;
			case FScalars: bOk = TryScalars(0, Res); break;
			}
			if (bOk)
			{
				Out = MoveTemp(Res);
				return true;
			}
		}
		Out = RawValue(Size, PT_ARRAY, Count);
		return true;
	}

	bool ReadValue(FConvReader& R, FConvPackage* Pkg, int32 Type, const FString& StructName, int32 Size, const FString& PropName,
		const TMap<FString, FString>* Hints, bool bRelaxed, FConvProp& Out)
	{
		switch (Type)
		{
		case PT_BYTE: Out = FConvProp::MakeInt(R.U8()); break;
		case PT_INT: Out = FConvProp::MakeInt(R.I32()); break;
		case PT_FLOAT: Out = FConvProp::MakeFloat(R.F32()); break;
		case PT_OBJECT:
		case PT_CLASS: Out = FConvProp::MakeRef(Pkg, R.Index()); break;
		case PT_NAME:
		{
			const int32 I = R.Index();
			FString N;
			if (R.bError || !Pkg->NameAt(I, N))
			{
				return false;
			}
			Out = FConvProp::MakeStr(N);
			break;
		}
		case PT_STR:
		case PT_STRING: Out = FConvProp::MakeStr(R.FString_()); break;
		case PT_VECTOR: { float V[3]; R.Vector(V); Out = FConvProp::MakeVector(V); break; }
		case PT_ROTATOR: Out = FConvProp(); Out.Kind = FConvProp::EKind::Rotator; Out.R[0] = R.I32(); Out.R[1] = R.I32(); Out.R[2] = R.I32(); break;
		case PT_STRUCT: return ReadStruct(R, Pkg, StructName, Size, Hints, bRelaxed, Out) && !R.bError;
		case PT_ARRAY: return ReadArray(R, Pkg, PropName, Size, Hints, bRelaxed, Out) && !R.bError;
		default: Out = RawValue(Size, Type); break;
		}
		return !R.bError;
	}
}

namespace ArenaConvProps
{
	using namespace ArenaConvPackageLocal;

	/** True if a property tag (or the 'None' terminator) can start at Pos. */
	static bool PlausibleTag(const uint8* Buf, int64 Pos, int64 End, FConvPackage* Pkg)
	{
		FConvReader T(Buf, Pos, End);
		const int32 Ni = T.Index();
		FString N;
		if (T.bError || !Pkg->NameAt(Ni, N))
		{
			return false;
		}
		if (N == TEXT("None"))
		{
			return true;
		}
		const uint8 Info = T.U8();
		return !T.bError && (Info & 0x0F) != 0;
	}

	bool ReadProperties(FConvReader& R, FConvPackage* Pkg, FConvProps& Out, const TMap<FString, FString>* Hints,
		TMap<FString, TPair<int32, FString>>* OutTypes, int64 Limit)
	{
		const int64 EndLimit = Limit < 0 ? R.End : Limit;
		for (int32 Guard = 0; Guard < 100000; ++Guard)
		{
			// tag
			const int32 Ni = R.Index();
			if (R.bError)
			{
				return false;
			}
			FString Name;
			if (!Pkg->NameAt(Ni, Name))
			{
				return R.Fail(FString::Printf(TEXT("name index %d out of range"), Ni));
			}
			if (Name == TEXT("None"))
			{
				return true;
			}
			const uint8 Info = R.U8();
			const int32 Type = Info & 0x0F;
			if (Type == 0)
			{
				return R.Fail(FString::Printf(TEXT("invalid property type 0 for %s"), *Name));
			}
			const bool bIsArray = (Info & 0x80) != 0;
			FString StructName;
			if (Type == PT_STRUCT)
			{
				const int32 Si = R.Index();
				if (R.bError || !Pkg->NameAt(Si, StructName))
				{
					return R.Fail(TEXT("bad struct name"));
				}
			}
			const int32 Sc = (Info >> 4) & 7;
			int32 Size;
			if (Sc < 5)
			{
				static const int32 Sizes[5] = { 1, 2, 4, 12, 16 };
				Size = Sizes[Sc];
			}
			else if (Sc == 5)
			{
				Size = R.U8();
			}
			else if (Sc == 6)
			{
				Size = R.U16();
			}
			else
			{
				Size = R.I32();
			}
			int32 ArrayIndex = 0;
			bool bBoolValue = false;
			if (Type == PT_BOOL)
			{
				bBoolValue = bIsArray;
			}
			else if (bIsArray)
			{
				const uint32 B = R.U8();
				if (B < 128)
				{
					ArrayIndex = B;
				}
				else
				{
					const uint32 B2 = R.U8();
					if (B & 0x40)
					{
						const uint32 B3 = R.U8();
						const uint32 B4 = R.U8();
						ArrayIndex = ((B << 24) | (B2 << 16) | (B3 << 8) | B4) & 0x3FFFFF;
					}
					else
					{
						ArrayIndex = ((B << 8) | B2) & 0x3FFF;
					}
				}
			}
			if (R.bError)
			{
				return false;
			}
			// value
			const int64 Start = R.Pos;
			if (Size < 0 || Start + Size > EndLimit)
			{
				return R.Fail(FString::Printf(TEXT("property %s size %d overruns object"), *Name, Size));
			}
			FConvProp Value;
			if (Type == PT_BOOL)
			{
				Value = FConvProp::MakeBool(bBoolValue);
			}
			else
			{
				int64 End = Start + Size;
				FConvReader Sub(R.Buf, Start, End);
				bool bOk = ReadValue(Sub, Pkg, Type, StructName, Size, Name, Hints, false, Value);
				if (!bOk || Value.IsRaw())
				{
					bOk = false;
					if (Type == PT_ARRAY || Type == PT_STRUCT)
					{
						// UE2 quirk: the stored size of struct/array data is sometimes smaller than the data (UE2 reads
						// the value itself and only skips by the size). An array is read element by element, up to the
						// object's end: DM-Big_City-Final-(Eisbaer)'s BigCitySMPCTastatur1 stores 916 bytes for
						// 1006 of Materials (103 StaticMeshMaterial structs)
						FConvReader Sub2(R.Buf, Start, Type == PT_ARRAY ? EndLimit : FMath::Min(EndLimit, End + 64));
						FConvProp V2;
						// accept data read past the stored size only when a property tag follows it: otherwise the relaxed
						// parse ran into the next property (binary structs such as WetTexture drops)
						if (ReadValue(Sub2, Pkg, Type, StructName, Size, Name, Hints, true, V2) && !V2.IsRaw()
							&& (Sub2.Pos <= End || PlausibleTag(R.Buf, Sub2.Pos, EndLimit, Pkg)))
						{
							Value = MoveTemp(V2);
							End = FMath::Max(End, Sub2.Pos);
							bOk = true;
						}
					}
					if (!bOk)
					{
						Value = RawValue(Size, Type);
					}
				}
				R.Pos = End;
			}
			const FString Key = ArrayIndex == 0 ? Name : FString::Printf(TEXT("%s[%d]"), *Name, ArrayIndex);
			bool bReplaced = false;
			for (TPair<FString, FConvProp>& KV : Out)
			{
				if (KV.Key == Key)
				{
					KV.Value = MoveTemp(Value);
					bReplaced = true;
					break;
				}
			}
			if (!bReplaced)
			{
				Out.Emplace(Key, MoveTemp(Value));
			}
			if (OutTypes)
			{
				OutTypes->Add(Name, TPair<int32, FString>(Type, StructName));
			}
		}
		return R.Fail(TEXT("too many properties"));
	}
}

// ---------------------------------------------------------------------------------------------
// reader

int32 FConvReader::Index()
{
	if (bError)
	{
		return 0;
	}
	if (Pos >= End)
	{
		Fail(TEXT("compact index past end"));
		return 0;
	}
	int64 P = Pos;
	const uint8 B0 = Buf[P++];
	int32 V = B0 & 0x3F;
	if (B0 & 0x40)
	{
		int32 Shift = 6;
		while (true)
		{
			if (P >= End)
			{
				Fail(TEXT("compact index past end"));
				return 0;
			}
			const uint8 B = Buf[P++];
			V |= int32(B & 0x7F) << Shift;
			Shift += 7;
			if (!(B & 0x80) || Shift > 27)
			{
				break;
			}
		}
	}
	Pos = P;
	return (B0 & 0x80) ? -V : V;
}

FString FConvReader::FString_()
{
	int32 N = Index();
	if (bError || N == 0)
	{
		return FString();
	}
	if (N > 0)
	{
		if (N > (1 << 20))
		{
			Fail(TEXT("string too long"));
			return FString();
		}
		const uint8* P = Bytes(N);
		if (!P)
		{
			return FString();
		}
		int32 Len = N;
		while (Len > 0 && P[Len - 1] == 0)
		{
			--Len;
		}
		FString S;
		S.GetCharArray().SetNumUninitialized(Len + 1);
		TCHAR* D = S.GetCharArray().GetData();
		for (int32 i = 0; i < Len; ++i)
		{
			D[i] = (TCHAR)P[i]; // latin-1
		}
		D[Len] = 0;
		if (Len == 0)
		{
			return FString();
		}
		return S;
	}
	N = -N;
	if (N > (1 << 20))
	{
		Fail(TEXT("string too long"));
		return FString();
	}
	const uint8* P = Bytes(int64(N) * 2);
	if (!P)
	{
		return FString();
	}
	TArray<TCHAR> Out;
	for (int32 i = 0; i < N; ++i)
	{
		const uint16 C = uint16(P[2 * i]) | (uint16(P[2 * i + 1]) << 8);
		Out.Add((TCHAR)C);
	}
	while (Out.Num() && Out.Last() == 0)
	{
		Out.Pop();
	}
	Out.Add(0);
	return FString(Out.GetData());
}

FString FConvReader::CString()
{
	if (bError)
	{
		return FString();
	}
	int64 E = Pos;
	while (E < End && Buf[E] != 0)
	{
		++E;
	}
	if (E >= End)
	{
		Fail(TEXT("unterminated string"));
		return FString();
	}
	TArray<TCHAR> Out;
	for (int64 i = Pos; i < E; ++i)
	{
		Out.Add((TCHAR)Buf[i]);
	}
	Out.Add(0);
	Pos = E + 1;
	return FString(Out.GetData());
}

bool FConvReader::LazyBytesSpan(bool bHasSkip, int64& OutOffset, int64& OutLen)
{
	if (bHasSkip)
	{
		I32();
	}
	const int32 N = Index();
	if (bError)
	{
		return false;
	}
	if (!Check(N))
	{
		return false;
	}
	OutOffset = Pos;
	OutLen = N;
	Pos += N;
	return true;
}

// ---------------------------------------------------------------------------------------------
// values

bool FConvProp::Truthy() const
{
	switch (Kind)
	{
	case EKind::None: return false;
	case EKind::Bool: return B;
	case EKind::Int: return I != 0;
	case EKind::Float: return F != 0.0;
	case EKind::Str: return !S.IsEmpty();
	case EKind::Ref: return Ref.Index != 0;
	case EKind::Vector: return true; // a 3-tuple
	case EKind::Rotator: return true;
	case EKind::Struct: return M.Num() > 0;
	case EKind::Array: return A.Num() > 0;
	}
	return false;
}

const FConvProp& FConvProp::NoneValue()
{
	static const FConvProp None;
	return None;
}

const FConvProp* FConvProp::Find(const FString& Key) const
{
	if (Kind != EKind::Struct)
	{
		return nullptr;
	}
	return ConvFindProp(M, Key);
}

const FConvProp& FConvProp::Get(const FString& Key) const
{
	const FConvProp* P = Find(Key);
	return P ? *P : NoneValue();
}

void FConvProp::Set(const FString& Key, FConvProp Value)
{
	for (TPair<FString, FConvProp>& KV : M)
	{
		if (KV.Key == Key)
		{
			KV.Value = MoveTemp(Value);
			return;
		}
	}
	M.Emplace(Key, MoveTemp(Value));
}

const FConvProp* ConvFindProp(const FConvProps& Props, const FString& Key)
{
	for (const TPair<FString, FConvProp>& KV : Props)
	{
		if (KV.Key.Equals(Key, ESearchCase::CaseSensitive))
		{
			return &KV.Value;
		}
	}
	return nullptr;
}

FString FConvRef::Path() const { return Pkg ? Pkg->RefPath(Index) : FString(TEXT("None")); }
FString FConvRef::Name() const { return Pkg ? Pkg->RefName(Index) : FString(TEXT("None")); }
FString FConvRef::ClassName() const { return Pkg ? Pkg->RefClassName(Index) : FString(TEXT("None")); }

FConvObject* FConvRef::Load() const
{
	if (!Pkg || Index == 0)
	{
		return nullptr;
	}
	FConvPackage* P = nullptr;
	int32 Ei = 0;
	if (!Pkg->ResolveRef(Index, P, Ei))
	{
		return nullptr;
	}
	return P->GetObject(Ei);
}

// ---------------------------------------------------------------------------------------------
// objects

const FConvExport& FConvObject::Export() const { return Pkg->Exports[Ei]; }
const FString& FConvObject::Name() const { return Pkg->Exports[Ei].Name; }
FString FConvObject::ClassName() const { return Pkg->ExportClassName(Ei); }
FString FConvObject::Path() const { return Pkg->ExportPath(Ei); }

const TArray<FString>& FConvObject::ClassChain()
{
	if (!bChain)
	{
		bChain = true;
		const int32 Ci = Export().ClassIndex;
		if (!Pkg->Manager || Ci == 0)
		{
			Chain = { ClassName() };
		}
		else
		{
			Chain = Pkg->Manager->ClassChain(Pkg, Ci);
		}
	}
	return Chain;
}

bool FConvObject::IsA(const TCHAR* A, const TCHAR* B, const TCHAR* C, const TCHAR* D)
{
	for (const FString& Cls : ClassChain())
	{
		if ((A && Cls.Equals(A, ESearchCase::IgnoreCase)) || (B && Cls.Equals(B, ESearchCase::IgnoreCase))
			|| (C && Cls.Equals(C, ESearchCase::IgnoreCase)) || (D && Cls.Equals(D, ESearchCase::IgnoreCase)))
		{
			return true;
		}
	}
	return false;
}

FConvReader FConvObject::Reader() const
{
	return Pkg->ExportReader(Ei);
}

void FConvObject::Load()
{
	bLoaded = true;
	FConvReader R = Reader();
	const FConvExport& E = Export();
	if (E.Flags & ArenaConvPackageLocal::RF_HasStack)
	{
		const int32 Node = R.Index();
		R.Index();
		R.U64();
		R.I32();
		if (Node != 0)
		{
			R.Index();
		}
	}
	const TMap<FString, FString>* Hints = nullptr;
	if (E.ClassIndex != 0 && Pkg->Manager)
	{
		FConvPackage* CP = nullptr;
		int32 CEi = 0;
		if (Pkg->ResolveRef(E.ClassIndex, CP, CEi))
		{
			Hints = &Pkg->Manager->ArrayHints(CP, CEi);
		}
	}
	FConvProps Props;
	if (!R.bError && ArenaConvProps::ReadProperties(R, Pkg, Props, Hints))
	{
		PropsData = MoveTemp(Props);
		NativePos = R.Pos;
	}
	else
	{
		PropsData.Reset();
		NativePos = -1;
		Error = R.Error;
	}
}

const FConvProps& FConvObject::Props()
{
	if (!bLoaded)
	{
		Load();
	}
	return PropsData;
}

FConvReader FConvObject::NativeReader()
{
	Props();
	FConvReader R = Reader();
	if (NativePos < 0)
	{
		R.Fail(FString::Printf(TEXT("properties of %s could not be parsed: %s"), *Path(), *Error));
		return R;
	}
	R.Pos = NativePos;
	return R;
}

const FConvProp* FConvObject::GetProp(const FString& PropName, bool bWithDefaults)
{
	if (const FConvProp* P = ConvFindProp(Props(), PropName))
	{
		return P;
	}
	if (bWithDefaults && Pkg->Manager)
	{
		return ConvFindProp(Pkg->Manager->ClassDefaults(Pkg, Export().ClassIndex), PropName);
	}
	return nullptr;
}

// ---------------------------------------------------------------------------------------------
// package

FConvPackageData::~FConvPackageData()
{
	delete MappedRegion;
	delete MappedFile;
}

FConvPackage::FConvPackage(const FString& InPath, FConvPackageManager* InManager)
	: PathOnDisk(InPath), Name(FPaths::GetBaseFilename(InPath)), Manager(InManager)
{
}

FConvPackage::~FConvPackage() = default;

bool FConvPackage::Open(FString& OutError)
{
	Data = MakeShared<FConvPackageData, ESPMode::ThreadSafe>();
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	const int64 FileSize = PF.FileSize(*PathOnDisk);
	if (FileSize > 0)
	{
		FOpenMappedResult Mapped = PF.OpenMappedEx2(*PathOnDisk);
		if (Mapped.HasValue())
		{
			Data->MappedFile = Mapped.StealValue().Release();
		}
		if (Data->MappedFile)
		{
			Data->MappedRegion = Data->MappedFile->MapRegion(0, FileSize);
		}
		if (Data->MappedRegion)
		{
			Data->Data = Data->MappedRegion->GetMappedPtr();
			Data->Size = Data->MappedRegion->GetMappedSize();
		}
		else
		{
			delete Data->MappedFile;
			Data->MappedFile = nullptr;
			if (!FFileHelper::LoadFileToArray(Data->Owned, *PathOnDisk, FILEREAD_Silent))
			{
				OutError = FString::Printf(TEXT("%s: cannot read"), *PathOnDisk);
				return false;
			}
			Data->Data = Data->Owned.GetData();
			Data->Size = Data->Owned.Num();
		}
	}
	FConvReader R(Data->Data, 0, Data->Size);
	const uint32 Tag = R.U32();
	if (Tag != ArenaConvPackageLocal::PackageTag)
	{
		OutError = FString::Printf(TEXT("%s: not an Unreal package (tag %08X)"), *PathOnDisk, Tag);
		return false;
	}
	Version = R.U16();
	Licensee = R.U16();
	PackageFlags = R.U32();
	const uint32 NameCount = R.U32(), NameOff = R.U32();
	const uint32 ExportCount = R.U32(), ExportOff = R.U32();
	const uint32 ImportCount = R.U32(), ImportOff = R.U32();
	if (!(Version >= 60 && Version <= 200))
	{
		OutError = FString::Printf(TEXT("%s: unsupported package version %d"), *PathOnDisk, Version);
		return false;
	}
	R.Pos = NameOff;
	const bool bOldNames = Version < 64;
	Names.Reserve(NameCount);
	for (uint32 i = 0; i < NameCount && !R.bError; ++i)
	{
		Names.Add(bOldNames ? R.CString() : R.FString_());
		R.U32();
		if (NoneNameIndex < 0 && Names.Last() == TEXT("None"))
		{
			NoneNameIndex = i;
		}
	}
	auto NameOr = [this](int32 I) { return Names.IsValidIndex(I) ? Names[I] : FString(); };
	R.Pos = ImportOff;
	Imports.Reserve(ImportCount);
	for (uint32 i = 0; i < ImportCount && !R.bError; ++i)
	{
		FConvImport Im;
		const int32 Cp = R.Index();
		const int32 Cn = R.Index();
		Im.PackageIndex = R.I32();
		const int32 On = R.Index();
		if (!Names.IsValidIndex(Cp) || !Names.IsValidIndex(Cn) || !Names.IsValidIndex(On))
		{
			R.Fail(TEXT("import name out of range"));
			break;
		}
		Im.ClassPackage = Names[Cp];
		Im.ClassName = Names[Cn];
		Im.Name = Names[On];
		Imports.Add(MoveTemp(Im));
	}
	R.Pos = ExportOff;
	Exports.Reserve(ExportCount);
	for (uint32 i = 0; i < ExportCount && !R.bError; ++i)
	{
		FConvExport Ex;
		Ex.ClassIndex = R.Index();
		Ex.SuperIndex = R.Index();
		Ex.PackageIndex = R.I32();
		const int32 On = R.Index();
		Ex.Flags = R.U32();
		Ex.Size = R.Index();
		Ex.Offset = Ex.Size > 0 ? R.Index() : 0;
		if (!Names.IsValidIndex(On))
		{
			R.Fail(TEXT("export name out of range"));
			break;
		}
		Ex.Name = Names[On];
		Exports.Add(MoveTemp(Ex));
	}
	if (R.bError)
	{
		OutError = FString::Printf(TEXT("%s: %s"), *PathOnDisk, *R.Error);
		return false;
	}
	(void)NameOr;
	return true;
}

bool FConvPackage::NameAt(int32 I, FString& Out) const
{
	if (Names.IsValidIndex(I))
	{
		Out = Names[I];
		return true;
	}
	return false;
}

const FString& FConvPackage::NameAtOr(int32 I) const
{
	static const FString Empty;
	return Names.IsValidIndex(I) ? Names[I] : Empty;
}

FString FConvPackage::RefName(int32 Index) const
{
	if (Index < 0)
	{
		return Imports.IsValidIndex(-Index - 1) ? Imports[-Index - 1].Name : FString(TEXT("None"));
	}
	if (Index > 0)
	{
		return Exports.IsValidIndex(Index - 1) ? Exports[Index - 1].Name : FString(TEXT("None"));
	}
	return TEXT("None");
}

FString FConvPackage::RefClassName(int32 Index) const
{
	if (Index < 0)
	{
		return Imports.IsValidIndex(-Index - 1) ? Imports[-Index - 1].ClassName : FString(TEXT("None"));
	}
	if (Index > 0)
	{
		if (!Exports.IsValidIndex(Index - 1))
		{
			return TEXT("None");
		}
		const int32 Ci = Exports[Index - 1].ClassIndex;
		return Ci == 0 ? FString(TEXT("Class")) : RefName(Ci);
	}
	return TEXT("None");
}

FString FConvPackage::ExportClassName(int32 Ei) const
{
	const int32 Ci = Exports[Ei].ClassIndex;
	return Ci == 0 ? FString(TEXT("Class")) : RefName(Ci);
}

FString FConvPackage::RefPath(int32 Index) const
{
	if (Index == 0)
	{
		return TEXT("None");
	}
	TArray<FString, TInlineAllocator<8>> Parts;
	int32 Guard = 0;
	if (Index < 0)
	{
		while (Index < 0 && Guard < 64)
		{
			if (!Imports.IsValidIndex(-Index - 1))
			{
				break;
			}
			const FConvImport& Im = Imports[-Index - 1];
			Parts.Add(Im.Name);
			Index = Im.PackageIndex;
			++Guard;
		}
	}
	else
	{
		while (Index > 0 && Guard < 64)
		{
			if (!Exports.IsValidIndex(Index - 1))
			{
				break;
			}
			const FConvExport& Ex = Exports[Index - 1];
			Parts.Add(Ex.Name);
			Index = Ex.PackageIndex;
			++Guard;
		}
		if (Index < 0)
		{
			Parts.Add(RefPath(Index));
		}
		else
		{
			Parts.Add(Name);
		}
	}
	FString Out;
	for (int32 i = Parts.Num() - 1; i >= 0; --i)
	{
		Out += Parts[i];
		if (i)
		{
			Out += TEXT(".");
		}
	}
	return Out;
}

void FConvPackage::BuildPathIndex()
{
	bPathIndex = true;
	for (int32 i = 0; i < Exports.Num(); ++i)
	{
		const FString P = ExportPath(i);
		int32 Dot;
		const FString Rel = P.FindChar(TEXT('.'), Dot) ? P.Mid(Dot + 1) : P;
		PathIndex.FindOrAdd(Rel.ToLower()).Add(i);
	}
}

int32 FConvPackage::FindExport(const FString& RelPath, const FString& ClassName)
{
	if (!bPathIndex)
	{
		BuildPathIndex();
	}
	const TArray<int32>* Cands = PathIndex.Find(RelPath.ToLower());
	if (!Cands || Cands->Num() == 0)
	{
		return -1;
	}
	if (ClassName.IsEmpty())
	{
		return (*Cands)[0];
	}
	for (int32 I : *Cands)
	{
		if (ExportClassName(I).Equals(ClassName, ESearchCase::IgnoreCase))
		{
			return I;
		}
	}
	return (*Cands)[0];
}

int32 FConvPackage::FindUniqueExport(const FString& ObjectName, const FString& ClassName) const
{
	int32 Found = -1;
	for (int32 i = 0; i < Exports.Num(); ++i)
	{
		if (Exports[i].Name.Equals(ObjectName, ESearchCase::IgnoreCase) && ExportClassName(i).Equals(ClassName, ESearchCase::IgnoreCase))
		{
			if (Found >= 0)
			{
				return -1; // ambiguous
			}
			Found = i;
		}
	}
	return Found;
}

TArray<int32> FConvPackage::ExportsOfClass(std::initializer_list<const TCHAR*> ClassNames) const
{
	TArray<int32> Out;
	for (int32 i = 0; i < Exports.Num(); ++i)
	{
		const FString Cn = ExportClassName(i);
		for (const TCHAR* C : ClassNames)
		{
			if (Cn.Equals(C, ESearchCase::IgnoreCase))
			{
				Out.Add(i);
				break;
			}
		}
	}
	return Out;
}

bool FConvPackage::ResolveRef(int32 Index, FConvPackage*& OutPkg, int32& OutEi)
{
	if (Index > 0 && IsReleased() && Manager)
	{
		// a closed package (kept for its tables): resolve through the reopened copy
		FConvPackage* Live = Manager->LoadPackage(Name);
		if (Live && Live != this && Index - 1 < Live->Exports.Num())
		{
			OutPkg = Live;
			OutEi = Index - 1;
			return true;
		}
		return false;
	}
	if (Index > 0)
	{
		if (Index - 1 < Exports.Num())
		{
			OutPkg = this;
			OutEi = Index - 1;
			return true;
		}
		return false;
	}
	if (Index == 0 || !Imports.IsValidIndex(-Index - 1))
	{
		return false;
	}
	const FConvImport& Imp = Imports[-Index - 1];
	TArray<FString, TInlineAllocator<8>> Chain;
	int32 I = Index;
	int32 Guard = 0;
	while (I < 0 && Imports.IsValidIndex(-I - 1) && Guard++ < 64)
	{
		const FConvImport& Im = Imports[-I - 1];
		Chain.Add(Im.Name);
		I = Im.PackageIndex;
	}
	if (Chain.Num() < 2 || !Manager)
	{
		return false;
	}
	Algo::Reverse(Chain);
	FConvPackage* P = Manager->LoadPackage(Chain[0]);
	FString Rel;
	for (int32 k = 1; k < Chain.Num(); ++k)
	{
		if (k > 1)
		{
			Rel += TEXT(".");
		}
		Rel += Chain[k];
	}
	int32 Ei = P ? P->FindExport(Rel, Imp.ClassName) : -1;
	if (Ei < 0 && P)
	{
		// UE2 shipped packages whose objects moved to another group after other packages referenced them
		// (SG_Special_Decos: COG.bluebolt is Mech.bluebolt): the same name and class elsewhere in the package
		Ei = P->FindUniqueExport(Imp.Name, Imp.ClassName);
	}
	if (Ei < 0)
	{
		// UT2004 also ships "X_<Package>" copies holding objects that the plain package lost
		// (AW-Cubes.Cubes.MesaEnv1 lives in X_AW-Cubes.Cubes.MesaEnv1)
		if (FConvPackage* X = Manager->LoadPackage(TEXT("X_") + Chain[0]))
		{
			Ei = X->FindExport(Rel, Imp.ClassName);
			if (Ei >= 0 && !X->ExportClassName(Ei).Equals(Imp.ClassName, ESearchCase::IgnoreCase))
			{
				Ei = -1;
			}
			if (Ei < 0)
			{
				Ei = X->FindUniqueExport(Imp.Name, Imp.ClassName);
			}
			if (Ei >= 0)
			{
				P = X;
			}
		}
	}
	if (Ei < 0)
	{
		return false;
	}
	OutPkg = P;
	OutEi = Ei;
	return true;
}

FConvObject* FConvPackage::GetObject(int32 Ei)
{
	if (!Exports.IsValidIndex(Ei))
	{
		return nullptr;
	}
	if (IsReleased() && Manager)
	{
		FConvPackage* Live = Manager->LoadPackage(Name);
		return Live && Live != this ? Live->GetObject(Ei) : nullptr;
	}
	TUniquePtr<FConvObject>& Obj = Objects.FindOrAdd(Ei);
	if (!Obj)
	{
		Obj = MakeUnique<FConvObject>();
		Obj->Pkg = this;
		Obj->Ei = Ei;
	}
	return Obj.Get();
}

FConvObject* FConvPackage::FindObject(const FString& RelPath, const FString& ClassName)
{
	const int32 Ei = FindExport(RelPath, ClassName);
	return Ei < 0 ? nullptr : GetObject(Ei);
}

FConvReader FConvPackage::ExportReader(int32 Ei) const
{
	const FConvExport& E = Exports[Ei];
	const int64 End = FMath::Min<int64>(int64(E.Offset) + E.Size, BufSize());
	FConvReader R(Buf(), E.Offset, End);
	if (int64(E.Offset) + E.Size > BufSize())
	{
		R.Fail(TEXT("export past end of file"));
	}
	return R;
}

const TArray<int32>* FConvPackage::Children(int32 Ei)
{
	if (!bChildIndex)
	{
		bChildIndex = true;
		for (int32 i = 0; i < Exports.Num(); ++i)
		{
			if (Exports[i].PackageIndex > 0)
			{
				ChildIndex.FindOrAdd(Exports[i].PackageIndex - 1).Add(i);
			}
		}
	}
	return ChildIndex.Find(Ei);
}

// ---------------------------------------------------------------------------------------------
// manager

FConvPackageManager::FConvPackageManager(const FString& InGameDir)
	: GameDir(FPaths::ConvertRelativePathToFull(InGameDir))
{
	IFileManager& FM = IFileManager::Get();
	// case-insensitive child lookup (Linux installs may mix cases)
	auto FindChild = [&FM](const FString& Dir, const FString& Child) -> FString
	{
		const FString Direct = Dir / Child;
		if (FM.DirectoryExists(*Direct))
		{
			return Direct;
		}
		FString Found;
		FM.IterateDirectory(*Dir, [&](const TCHAR* P, bool bIsDir)
		{
			if (bIsDir && FPaths::GetCleanFilename(P).Equals(Child, ESearchCase::IgnoreCase))
			{
				Found = P;
				return false;
			}
			return true;
		});
		return Found;
	};
	const FString System = FindChild(GameDir, TEXT("System"));
	FString Ini;
	for (const TCHAR* Cand : { TEXT("UT2004.ini"), TEXT("Default.ini") })
	{
		if (!System.IsEmpty() && FM.FileExists(*(System / Cand)))
		{
			Ini = System / Cand;
			break;
		}
	}
	if (!Ini.IsEmpty())
	{
		TArray<FString> Lines;
		FFileHelper::LoadFileToStringArray(Lines, *Ini);
		FString Section;
		for (FString Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (Line.StartsWith(TEXT("[")))
			{
				Section = Line.ToLower();
				continue;
			}
			if (Section == TEXT("[core.system]") && Line.StartsWith(TEXT("paths="), ESearchCase::IgnoreCase))
			{
				FString Spec = Line.Mid(6).TrimStartAndEnd().Replace(TEXT("\\"), TEXT("/"));
				FString D = FPaths::GetPath(Spec);
				const FString Ext = FPaths::GetExtension(Spec, true).ToLower();
				// resolve every ../ segment case-insensitively
				FString Full = System;
				TArray<FString> Segs;
				D.ParseIntoArray(Segs, TEXT("/"));
				for (const FString& S : Segs)
				{
					if (S == TEXT(".."))
					{
						Full = FPaths::GetPath(Full);
					}
					else if (S != TEXT("."))
					{
						const FString C = FindChild(Full, S);
						Full = C.IsEmpty() ? Full / S : C;
					}
				}
				SearchPaths.Emplace(Full, Ext);
			}
		}
	}
	if (SearchPaths.Num() == 0)
	{
		const TPair<const TCHAR*, const TCHAR*> Defaults[] = {
			{ TEXT("System"), TEXT(".u") }, { TEXT("Maps"), TEXT(".ut2") }, { TEXT("Textures"), TEXT(".utx") }, { TEXT("Sounds"), TEXT(".uax") },
			{ TEXT("Music"), TEXT(".umx") }, { TEXT("StaticMeshes"), TEXT(".usx") }, { TEXT("Animations"), TEXT(".ukx") }, { TEXT("Saves"), TEXT(".uvx") } };
		for (const auto& D : Defaults)
		{
			const FString C = FindChild(GameDir, D.Key);
			SearchPaths.Emplace(C.IsEmpty() ? GameDir / D.Key : C, D.Value);
		}
	}
	for (const TPair<FString, FString>& SP : SearchPaths)
	{
		if (!FM.DirectoryExists(*SP.Key))
		{
			continue;
		}
		TArray<FString> Files;
		FM.FindFiles(Files, *(SP.Key / TEXT("*")), true, false);
		Files.Sort();
		for (const FString& F : Files)
		{
			if (FPaths::GetExtension(F, true).ToLower() != SP.Value)
			{
				continue;
			}
			const FString Key = FPaths::GetBaseFilename(F).ToLower();
			if (!FileIndex.Contains(Key))
			{
				FileIndex.Add(Key, SP.Key / F);
			}
		}
	}
}

FConvPackageManager::~FConvPackageManager() = default;

TArray<TPair<FString, FString>> FConvPackageManager::PackageFiles(const TCHAR* Ext) const
{
	TArray<TPair<FString, FString>> Out;
	for (const TPair<FString, FString>& KV : FileIndex)
	{
		if (!Ext || KV.Value.EndsWith(Ext, ESearchCase::IgnoreCase))
		{
			Out.Add(KV);
		}
	}
	// Python sorts the (key, path) items: keys are lower-case names (ordinal compare)
	Out.Sort([](const TPair<FString, FString>& A, const TPair<FString, FString>& B) { return FCString::Strcmp(*A.Key, *B.Key) < 0; });
	return Out;
}

FString FConvPackageManager::FindFile(const FString& InName) const
{
	const FString* P = FileIndex.Find(InName.ToLower());
	return P ? *P : FString();
}

void FConvPackageManager::AddPackageFiles(const TArray<FString>& Files, TArray<FString>* OutShadowed)
{
	for (const FString& F : Files)
	{
		const FString Key = FPaths::GetBaseFilename(F).ToLower();
		if (FileIndex.Contains(Key))
		{
			if (OutShadowed && !ExtraPackages.Contains(Key))
			{
				OutShadowed->Add(F);
			}
			continue;
		}
		FileIndex.Add(Key, F);
		ExtraPackages.Add(Key);
		Missing.Remove(Key);
	}
}

FConvPackage* FConvPackageManager::LoadPackage(const FString& InName)
{
	const FString Key = InName.ToLower();
	if (TUniquePtr<FConvPackage>* P = Packages.Find(Key))
	{
		return P->Get();
	}
	if (Missing.Contains(Key))
	{
		return nullptr;
	}
	const FString* Path = FileIndex.Find(Key);
	if (!Path)
	{
		Missing.Add(Key);
		return nullptr;
	}
	TUniquePtr<FConvPackage> Pkg = MakeUnique<FConvPackage>(*Path, this);
	FString Err;
	if (!Pkg->Open(Err))
	{
		Warnings.Add(FString::Printf(TEXT("cannot open %s: %s"), **Path, *Err));
		Missing.Add(Key);
		return nullptr;
	}
	FConvPackage* Raw = Pkg.Get();
	Packages.Add(Key, MoveTemp(Pkg));
	return Raw;
}

FConvPackage* FConvPackageManager::OpenFile(const FString& Path)
{
	const FString Key = FPaths::GetBaseFilename(Path).ToLower();
	if (TUniquePtr<FConvPackage>* P = Packages.Find(Key))
	{
		return P->Get();
	}
	TUniquePtr<FConvPackage> Pkg = MakeUnique<FConvPackage>(Path, this);
	FString Err;
	if (!Pkg->Open(Err))
	{
		Warnings.Add(Err);
		return nullptr;
	}
	FConvPackage* Raw = Pkg.Get();
	Packages.Add(Key, MoveTemp(Pkg));
	return Raw;
}

FConvObject* FConvPackageManager::ResolvePath(const FString& FullPath, const FString& ClassName)
{
	int32 Dot;
	if (!FullPath.FindChar(TEXT('.'), Dot))
	{
		return nullptr;
	}
	FConvPackage* P = LoadPackage(FullPath.Left(Dot));
	return P ? P->FindObject(FullPath.Mid(Dot + 1), ClassName) : nullptr;
}

const TArray<FString>& FConvPackageManager::ClassChain(FConvPackage* Pkg, int32 ClassIndex)
{
	static const TArray<FString> ClassOnly = { TEXT("Class") };
	if (ClassIndex == 0)
	{
		return ClassOnly;
	}
	const FString Key = Pkg->RefPath(ClassIndex).ToLower();
	if (const TArray<FString>* Hit = ClassChainCache.Find(Key))
	{
		return *Hit;
	}
	TArray<FString> Chain;
	FConvPackage* P = Pkg;
	int32 Ci = ClassIndex;
	for (int32 Seen = 0; P && Seen < 64; ++Seen)
	{
		Chain.Add(P->RefName(Ci));
		FConvPackage* RP = nullptr;
		int32 Ei = 0;
		if (!P->ResolveRef(Ci, RP, Ei))
		{
			break;
		}
		const int32 Si = RP->Exports[Ei].SuperIndex;
		if (Si == 0)
		{
			break;
		}
		P = RP;
		Ci = Si;
	}
	return ClassChainCache.Add(Key, MoveTemp(Chain));
}

const TMap<FString, FString>& FConvPackageManager::DeclaredProperties(FConvPackage* Pkg, int32 Ei, int32 Depth)
{
	const FString Key = FString::Printf(TEXT("%s|%d"), *Pkg->PathOnDisk, Ei);
	if (const TMap<FString, FString>* Hit = DeclaredCache.Find(Key))
	{
		return *Hit;
	}
	TMap<FString, FString> Out;
	const FConvExport& E = Pkg->Exports[Ei];
	if (E.SuperIndex != 0 && Depth < 64)
	{
		FConvPackage* SP = nullptr;
		int32 SEi = 0;
		if (Pkg->ResolveRef(E.SuperIndex, SP, SEi))
		{
			Out.Append(DeclaredProperties(SP, SEi, Depth + 1));
		}
	}
	if (const TArray<int32>* Kids = Pkg->Children(Ei))
	{
		for (int32 Ci : *Kids)
		{
			const FString Cn = Pkg->ExportClassName(Ci);
			if (Cn.EndsWith(TEXT("Property")))
			{
				Out.Add(Pkg->Exports[Ci].Name, Cn);
			}
		}
	}
	return DeclaredCache.Add(Key, MoveTemp(Out));
}

bool FConvPackageManager::StructOf(FConvPackage* Pkg, int32 PropEi, FConvPackage*& OutPkg, int32& OutEi)
{
	FConvReader R = Pkg->ExportReader(PropEi);
	FConvProps Dummy;
	if (!ArenaConvProps::ReadProperties(R, Pkg, Dummy, nullptr))
	{
		return false;
	}
	R.Index();
	R.Index();
	R.I32();
	const uint32 Flags = R.U32();
	R.Index();
	if (Flags & 0x20)
	{
		R.U16();
	}
	const int32 Ref = R.Index();
	if (R.bError || Ref == 0)
	{
		return false;
	}
	if (!Pkg->ResolveRef(Ref, OutPkg, OutEi))
	{
		return false;
	}
	return OutPkg->ExportClassName(OutEi) == TEXT("Struct");
}

const TMap<FString, FString>& FConvPackageManager::ArrayHints(FConvPackage* Pkg, int32 Ei, int32 Depth)
{
	const FString Key = FString::Printf(TEXT("%s|%d"), *Pkg->PathOnDisk, Ei);
	if (const TMap<FString, FString>* Hit = HintsCache.Find(Key))
	{
		return *Hit;
	}
	HintsCache.Add(Key); // recursion guard (Python stores the dict being filled)
	TMap<FString, FString> Out;
	const FConvExport& E = Pkg->Exports[Ei];
	if (E.SuperIndex != 0 && Depth < 64)
	{
		FConvPackage* SP = nullptr;
		int32 SEi = 0;
		if (Pkg->ResolveRef(E.SuperIndex, SP, SEi))
		{
			Out.Append(ArrayHints(SP, SEi, Depth + 1));
		}
	}
	static const TMap<FString, FString> PropKind = {
		{ TEXT("NameProperty"), TEXT("name") }, { TEXT("ObjectProperty"), TEXT("object") }, { TEXT("ClassProperty"), TEXT("object") },
		{ TEXT("StructProperty"), TEXT("struct") }, { TEXT("StrProperty"), TEXT("string") }, { TEXT("IntProperty"), TEXT("int") },
		{ TEXT("FloatProperty"), TEXT("float") }, { TEXT("ByteProperty"), TEXT("byte") }, { TEXT("BoolProperty"), TEXT("bool") } };
	if (const TArray<int32>* KidsPtr = Pkg->Children(Ei))
	{
		const TArray<int32> Kids = *KidsPtr;
		for (int32 Ci : Kids)
		{
			const FString Cn = Pkg->ExportClassName(Ci);
			if (Cn == TEXT("ArrayProperty"))
			{
				if (const TArray<int32>* Inner = Pkg->Children(Ci))
				{
					const TArray<int32> InnerCopy = *Inner;
					for (int32 Ii : InnerCopy)
					{
						const FString Icn = Pkg->ExportClassName(Ii);
						if (const FString* Kind = PropKind.Find(Icn))
						{
							// keys are matched case-insensitively later: store lower
							const FString PName = Pkg->Exports[Ci].Name.ToLower();
							if (!Out.Contains(PName))
							{
								Out.Add(PName, *Kind);
							}
							if (Icn == TEXT("StructProperty") && Depth < 64)
							{
								FConvPackage* StP = nullptr;
								int32 StEi = 0;
								if (StructOf(Pkg, Ii, StP, StEi))
								{
									for (const TPair<FString, FString>& KV : ArrayHints(StP, StEi, Depth + 1))
									{
										if (!Out.Contains(KV.Key))
										{
											Out.Add(KV.Key, KV.Value);
										}
									}
								}
							}
							break;
						}
					}
				}
			}
			else if (Cn == TEXT("StructProperty") && Depth < 64)
			{
				FConvPackage* StP = nullptr;
				int32 StEi = 0;
				if (StructOf(Pkg, Ci, StP, StEi))
				{
					for (const TPair<FString, FString>& KV : ArrayHints(StP, StEi, Depth + 1))
					{
						if (!Out.Contains(KV.Key))
						{
							Out.Add(KV.Key, KV.Value);
						}
					}
				}
			}
		}
	}
	TMap<FString, FString>& Slot = HintsCache.FindOrAdd(Key);
	Slot = MoveTemp(Out);
	return Slot;
}

FConvProps FConvPackageManager::ScanDefaults(FConvPackage* Pkg, int32 Ei)
{
	static const TMap<int32, TArray<FString>> TagPropClasses = {
		{ 1, { TEXT("ByteProperty") } }, { 2, { TEXT("IntProperty") } }, { 3, { TEXT("BoolProperty") } }, { 4, { TEXT("FloatProperty") } },
		{ 5, { TEXT("ObjectProperty"), TEXT("ClassProperty") } }, { 6, { TEXT("NameProperty") } }, { 7, { TEXT("StringProperty") } },
		{ 8, { TEXT("ClassProperty"), TEXT("ObjectProperty") } }, { 9, { TEXT("ArrayProperty") } }, { 10, { TEXT("StructProperty") } },
		{ 11, { TEXT("StructProperty") } }, { 12, { TEXT("StructProperty") } }, { 13, { TEXT("StrProperty") } },
		{ 14, { TEXT("MapProperty") } }, { 15, { TEXT("FixedArrayProperty") } } };
	const FConvExport& E = Pkg->Exports[Ei];
	const int64 End = FMath::Min<int64>(int64(E.Offset) + E.Size, Pkg->BufSize());
	const TMap<FString, FString> Declared = DeclaredProperties(Pkg, Ei);
	const TMap<FString, FString> Hints = ArrayHints(Pkg, Ei);
	const uint8* Buf = Pkg->Buf();
	const int32 NNames = Pkg->Names.Num();
	for (int64 Pos = E.Offset; Pos < End; ++Pos)
	{
		FConvReader R(Buf, Pos, End);
		const int32 Ni = R.Index();
		if (R.bError || !(Ni >= 0 && Ni < NNames))
		{
			continue;
		}
		if (Ni == Pkg->NoneNameIndex)
		{
			if (R.Pos == End)
			{
				return FConvProps();
			}
			continue;
		}
		const FString& Nm = Pkg->Names[Ni];
		if (Nm.IsEmpty() || !(FChar::IsAlpha(Nm[0]) || Nm[0] == TEXT('_')))
		{
			continue;
		}
		FConvReader R2(Buf, Pos, End);
		FConvProps Props;
		TMap<FString, TPair<int32, FString>> Types;
		if (!ArenaConvProps::ReadProperties(R2, Pkg, Props, &Hints, &Types) || R2.Pos != End)
		{
			continue;
		}
		bool bMatch = true;
		for (const TPair<FString, TPair<int32, FString>>& T : Types)
		{
			const FString* Pc = Declared.Find(T.Key);
			const TArray<FString>* Allowed = TagPropClasses.Find(T.Value.Key);
			if (!Pc || !Allowed || !Allowed->Contains(*Pc))
			{
				bMatch = false;
				break;
			}
		}
		if (bMatch)
		{
			return Props;
		}
	}
	return FConvProps();
}

const FConvProps& FConvPackageManager::ClassDefaults(FConvPackage* Pkg, int32 ClassIndex)
{
	static const FConvProps Empty;
	if (ClassIndex == 0)
	{
		return Empty;
	}
	const FString Key = Pkg->RefPath(ClassIndex).ToLower();
	if (const FConvProps* Hit = DefaultsCache.Find(Key))
	{
		return *Hit;
	}
	DefaultsCache.Add(Key); // recursion guard
	FConvProps Merged;
	FConvPackage* CP = nullptr;
	int32 CEi = 0;
	if (Pkg->ResolveRef(ClassIndex, CP, CEi))
	{
		const int32 Si = CP->Exports[CEi].SuperIndex;
		if (Si != 0)
		{
			Merged = ClassDefaults(CP, Si);
		}
		FConvProps Own = ScanDefaults(CP, CEi);
		for (TPair<FString, FConvProp>& KV : Own)
		{
			bool bFound = false;
			for (TPair<FString, FConvProp>& M : Merged)
			{
				if (M.Key == KV.Key)
				{
					M.Value = MoveTemp(KV.Value);
					bFound = true;
					break;
				}
			}
			if (!bFound)
			{
				Merged.Emplace(KV.Key, MoveTemp(KV.Value));
			}
		}
		CP->bPinned = true;
	}
	FConvProps& Slot = DefaultsCache.FindOrAdd(Key);
	Slot = MoveTemp(Merged);
	return Slot;
}

void FConvPackageManager::ClosePackage(const FString& InName)
{
	TUniquePtr<FConvPackage> P;
	if (Packages.RemoveAndCopyValue(InName.ToLower(), P) && P)
	{
		if (P->bPinned)
		{
			P->Release();
			Graveyard.Add(MoveTemp(P));
		}
	}
}

void FConvPackageManager::CloseAll()
{
	for (TPair<FString, TUniquePtr<FConvPackage>>& KV : Packages)
	{
		if (KV.Value->bPinned)
		{
			KV.Value->Release();
			Graveyard.Add(MoveTemp(KV.Value));
		}
	}
	Packages.Reset();
	Missing.Reset();
}
