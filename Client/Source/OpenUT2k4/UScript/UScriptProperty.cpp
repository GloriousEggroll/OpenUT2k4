// UScript property value operations (copy/destroy/compare/text), object helpers and FUSParms.

#include "UScriptCore.h"
#if OPENUT2K4_USCRIPT_ENABLED
#include "UScriptRuntime.h"

namespace
{
	int32 GImportDepth = 0;

	void SkipSpaces(const TCHAR*& In)
	{
		while (*In == TEXT(' ') || *In == TEXT('\t'))
		{
			++In;
		}
	}

	/** Reads a token up to a delimiter (',' or ')' inside structs, end of text otherwise). */
	FString ReadToken(const TCHAR*& In, bool bStopAtSpace = false)
	{
		SkipSpaces(In);
		FString Out;
		if (*In == TEXT('"'))
		{
			++In;
			while (*In && *In != TEXT('"'))
			{
				if (*In == TEXT('\\') && In[1])
				{
					++In;
				}
				Out.AppendChar(*In++);
			}
			if (*In == TEXT('"'))
			{
				++In;
			}
			return Out;
		}
		while (*In)
		{
			if (GImportDepth > 0 && (*In == TEXT(',') || *In == TEXT(')')))
			{
				break;
			}
			if (bStopAtSpace && (*In == TEXT(' ') || *In == TEXT('\t')))
			{
				break;
			}
			Out.AppendChar(*In++);
		}
		if (GImportDepth > 0 || bStopAtSpace)
		{
			Out.TrimEndInline();
		}
		return Out;
	}

	void DestroyArrayElements(const FUSProperty* Inner, FUSScriptArray* A, int32 First, int32 Count)
	{
		if (Inner->NeedsDestruct())
		{
			for (int32 i = First; i < First + Count; ++i)
			{
				Inner->DestroySingle(A->Data + int64(i) * Inner->ElementSize);
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// FUSProperty
// ---------------------------------------------------------------------------------------------

int32 FUSProperty::GetAlignment() const
{
	switch (Type)
	{
	case EUSType::Byte: return 1;
	case EUSType::Int: case EUSType::Bool: case EUSType::Float: return 4;
	case EUSType::Name: return alignof(FName);
	case EUSType::Struct: return Struct ? FMath::Max(1, Struct->MinAlignment) : 4;
	default: return 8;
	}
}

bool FUSProperty::NeedsDestruct() const
{
	switch (Type)
	{
	case EUSType::Str: case EUSType::Array: return true;
	case EUSType::Struct: return Struct && Struct->CtorProps.Num() > 0;
	default: return false;
	}
}

bool FUSProperty::HasReferences() const
{
	switch (Type)
	{
	case EUSType::Object: case EUSType::Class: case EUSType::Delegate: return true;
	case EUSType::Array: return Inner && Inner->HasReferences();
	case EUSType::Struct: return Struct && Struct->RefProps.Num() > 0;
	default: return false;
	}
}

void FUSProperty::CopySingle(void* Dst, const void* Src) const
{
	if (Dst == Src)
	{
		return;
	}
	switch (Type)
	{
	case EUSType::Str:
		*static_cast<FString*>(Dst) = *static_cast<const FString*>(Src);
		break;
	case EUSType::Name:
		*static_cast<FName*>(Dst) = *static_cast<const FName*>(Src);
		break;
	case EUSType::Struct:
		Struct->CopyValue(static_cast<uint8*>(Dst), static_cast<const uint8*>(Src));
		break;
	case EUSType::Array:
	{
		FUSScriptArray* D = static_cast<FUSScriptArray*>(Dst);
		const FUSScriptArray* S = static_cast<const FUSScriptArray*>(Src);
		// copy into a temporary first: Src may live inside Dst's elements
		FUSScriptArray Tmp;
		const int32 ES = Inner->ElementSize;
		if (S->Num > 0)
		{
			Tmp.Data = static_cast<uint8*>(FMemory::Malloc(int64(S->Num) * ES, 16));
			FMemory::Memzero(Tmp.Data, int64(S->Num) * ES);
			Tmp.Num = Tmp.Max = S->Num;
			for (int32 i = 0; i < S->Num; ++i)
			{
				Inner->CopySingle(Tmp.Data + int64(i) * ES, S->Data + int64(i) * ES);
			}
		}
		DestroySingle(Dst);
		*D = Tmp;
		break;
	}
	case EUSType::Delegate:
		*static_cast<FUSDelegate*>(Dst) = *static_cast<const FUSDelegate*>(Src);
		break;
	default:
		FMemory::Memcpy(Dst, Src, ElementSize);
		break;
	}
}

void FUSProperty::DestroySingle(void* Value) const
{
	switch (Type)
	{
	case EUSType::Str:
		static_cast<FString*>(Value)->Empty();
		break;
	case EUSType::Struct:
		Struct->DestroyValue(static_cast<uint8*>(Value));
		break;
	case EUSType::Array:
	{
		FUSScriptArray* A = static_cast<FUSScriptArray*>(Value);
		if (A->Data)
		{
			DestroyArrayElements(Inner, A, 0, A->Num);
			FMemory::Free(A->Data);
		}
		A->Data = nullptr;
		A->Num = A->Max = 0;
		break;
	}
	default:
		break;
	}
}

bool FUSProperty::IdenticalSingle(const void* A, const void* B) const
{
	switch (Type)
	{
	case EUSType::Byte: return *static_cast<const uint8*>(A) == *static_cast<const uint8*>(B);
	case EUSType::Int: return *static_cast<const int32*>(A) == *static_cast<const int32*>(B);
	case EUSType::Bool: return ((*static_cast<const uint32*>(A)) != 0) == ((*static_cast<const uint32*>(B)) != 0);
	case EUSType::Float: return *static_cast<const float*>(A) == *static_cast<const float*>(B);
	case EUSType::Object: case EUSType::Class: case EUSType::Pointer: return *static_cast<void* const*>(A) == *static_cast<void* const*>(B);
	case EUSType::Name: return *static_cast<const FName*>(A) == *static_cast<const FName*>(B);
	case EUSType::Str: return static_cast<const FString*>(A)->Equals(*static_cast<const FString*>(B), ESearchCase::CaseSensitive);
	case EUSType::Struct: return Struct->IdenticalValue(static_cast<const uint8*>(A), static_cast<const uint8*>(B));
	case EUSType::Delegate:
	{
		const FUSDelegate* DA = static_cast<const FUSDelegate*>(A);
		const FUSDelegate* DB = static_cast<const FUSDelegate*>(B);
		return DA->Object == DB->Object && DA->FunctionName == DB->FunctionName;
	}
	case EUSType::Array:
	{
		const FUSScriptArray* AA = static_cast<const FUSScriptArray*>(A);
		const FUSScriptArray* BB = static_cast<const FUSScriptArray*>(B);
		if (AA->Num != BB->Num)
		{
			return false;
		}
		for (int32 i = 0; i < AA->Num; ++i)
		{
			if (!Inner->IdenticalSingle(AA->Data + int64(i) * Inner->ElementSize, BB->Data + int64(i) * Inner->ElementSize))
			{
				return false;
			}
		}
		return true;
	}
	default: return false;
	}
}

void FUSProperty::CopyComplete(void* Dst, const void* Src) const
{
	for (int32 i = 0; i < ArrayDim; ++i)
	{
		CopySingle(static_cast<uint8*>(Dst) + i * ElementSize, static_cast<const uint8*>(Src) + i * ElementSize);
	}
}

void FUSProperty::DestroyComplete(void* Value) const
{
	if (!NeedsDestruct())
	{
		return;
	}
	for (int32 i = 0; i < ArrayDim; ++i)
	{
		DestroySingle(static_cast<uint8*>(Value) + i * ElementSize);
	}
}

void FUSProperty::CleanupRefs(void* Value) const
{
	switch (Type)
	{
	case EUSType::Object: case EUSType::Class:
	{
		FUSObject** O = static_cast<FUSObject**>(Value);
		if (*O && (*O)->IsPendingKill())
		{
			*O = nullptr;
		}
		break;
	}
	case EUSType::Delegate:
	{
		FUSDelegate* D = static_cast<FUSDelegate*>(Value);
		if (D->Object && D->Object->IsPendingKill())
		{
			D->Object = nullptr;
			D->FunctionName = NAME_None;
		}
		break;
	}
	case EUSType::Struct:
		for (FUSProperty* P : Struct->RefProps)
		{
			for (int32 i = 0; i < P->ArrayDim; ++i)
			{
				P->CleanupRefs(static_cast<uint8*>(Value) + P->Offset + i * P->ElementSize);
			}
		}
		break;
	case EUSType::Array:
	{
		FUSScriptArray* A = static_cast<FUSScriptArray*>(Value);
		for (int32 i = 0; i < A->Num; ++i)
		{
			Inner->CleanupRefs(A->Data + int64(i) * Inner->ElementSize);
		}
		break;
	}
	default:
		break;
	}
}

void FUSProperty::ExportText(FString& Out, const void* Value, bool bPlain) const
{
	switch (Type)
	{
	case EUSType::Byte:
	{
		const uint8 B = *static_cast<const uint8*>(Value);
		if (Enum && Enum->Names.IsValidIndex(B))
		{
			Out += Enum->Names[B].ToString();
		}
		else
		{
			Out += FString::FromInt(B);
		}
		break;
	}
	case EUSType::Int: Out += FString::FromInt(*static_cast<const int32*>(Value)); break;
	case EUSType::Bool: Out += (*static_cast<const uint32*>(Value)) ? TEXT("True") : TEXT("False"); break;
	case EUSType::Float: Out += FString::Printf(TEXT("%f"), *static_cast<const float*>(Value)); break;
	case EUSType::Name: Out += static_cast<const FName*>(Value)->ToString(); break;
	case EUSType::Str:
		if (bPlain)
		{
			Out += *static_cast<const FString*>(Value);
		}
		else
		{
			Out += TEXT("\"") + static_cast<const FString*>(Value)->Replace(TEXT("\""), TEXT("\\\"")) + TEXT("\"");
		}
		break;
	case EUSType::Object: case EUSType::Class:
	{
		const FUSObject* O = *static_cast<FUSObject* const*>(Value);
		if (!O)
		{
			Out += TEXT("None");
		}
		else
		{
			Out += FString::Printf(TEXT("%s'%s'"), O->Class ? *O->Class->GetName() : TEXT("Object"), *O->GetPathName());
		}
		break;
	}
	case EUSType::Delegate:
	{
		const FUSDelegate* D = static_cast<const FUSDelegate*>(Value);
		Out += D->FunctionName == NAME_None ? FString(TEXT("None")) : FString::Printf(TEXT("%s.%s"), D->Object ? *D->Object->GetName() : TEXT("None"), *D->FunctionName.ToString());
		break;
	}
	case EUSType::Struct:
	{
		Out += TEXT("(");
		bool bFirst = true;
		for (FUSProperty* P : Struct->AllProps)
		{
			for (int32 i = 0; i < P->ArrayDim; ++i)
			{
				if (!bFirst)
				{
					Out += TEXT(",");
				}
				bFirst = false;
				Out += P->ArrayDim > 1 ? FString::Printf(TEXT("%s[%d]="), *P->GetName(), i) : P->GetName() + TEXT("=");
				P->ExportText(Out, static_cast<const uint8*>(Value) + P->Offset + i * P->ElementSize, false);
			}
		}
		Out += TEXT(")");
		break;
	}
	case EUSType::Array:
	{
		const FUSScriptArray* A = static_cast<const FUSScriptArray*>(Value);
		Out += TEXT("(");
		for (int32 i = 0; i < A->Num; ++i)
		{
			if (i)
			{
				Out += TEXT(",");
			}
			Inner->ExportText(Out, A->Data + int64(i) * Inner->ElementSize, false);
		}
		Out += TEXT(")");
		break;
	}
	default:
		break;
	}
}

bool FUSProperty::ImportText(const TCHAR*& In, void* Value) const
{
	SkipSpaces(In);
	switch (Type)
	{
	case EUSType::Byte:
	{
		const FString T = ReadToken(In, GImportDepth > 0);
		if (Enum)
		{
			const int32 Idx = Enum->Names.IndexOfByKey(FName(*T));
			if (Idx != INDEX_NONE)
			{
				*static_cast<uint8*>(Value) = uint8(Idx);
				return true;
			}
		}
		*static_cast<uint8*>(Value) = uint8(FCString::Atoi(*T));
		return true;
	}
	case EUSType::Int:
		*static_cast<int32*>(Value) = FCString::Atoi(*ReadToken(In, GImportDepth > 0));
		return true;
	case EUSType::Float:
		*static_cast<float*>(Value) = FCString::Atof(*ReadToken(In, GImportDepth > 0));
		return true;
	case EUSType::Bool:
	{
		const FString T = ReadToken(In, GImportDepth > 0);
		*static_cast<uint32*>(Value) = (T.Equals(TEXT("True"), ESearchCase::IgnoreCase) || T.Equals(TEXT("Yes"), ESearchCase::IgnoreCase)
			|| T.Equals(TEXT("On"), ESearchCase::IgnoreCase) || FCString::Atoi(*T) != 0) ? 1u : 0u;
		return true;
	}
	case EUSType::Name:
		*static_cast<FName*>(Value) = FName(*ReadToken(In, GImportDepth > 0));
		return true;
	case EUSType::Str:
		*static_cast<FString*>(Value) = ReadToken(In);
		return true;
	case EUSType::Object: case EUSType::Class:
	{
		FString T = ReadToken(In, GImportDepth > 0);
		FUSObject*& O = *static_cast<FUSObject**>(Value);
		if (T.IsEmpty() || T.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			O = nullptr;
			return true;
		}
		int32 Q;
		if (T.FindChar(TEXT('\''), Q))
		{
			T = T.Mid(Q + 1).Replace(TEXT("'"), TEXT(""));
		}
		FUSRuntime& RT = FUSRuntime::Get();
		if (Type == EUSType::Class)
		{
			O = RT.FindClass(T);
		}
		else
		{
			O = RT.FindObject(T, PropertyClass);
			if (!O)
			{
				O = RT.GetPlaceholder(T, PropertyClass);
			}
		}
		return true;
	}
	case EUSType::Struct:
	{
		if (*In != TEXT('('))
		{
			return false;
		}
		++In;
		++GImportDepth;
		while (*In && *In != TEXT(')'))
		{
			SkipSpaces(In);
			FString Key;
			while (*In && *In != TEXT('=') && *In != TEXT(',') && *In != TEXT(')'))
			{
				Key.AppendChar(*In++);
			}
			Key.TrimStartAndEndInline();
			int32 Index = 0;
			int32 Br;
			if (Key.FindChar(TEXT('['), Br))
			{
				Index = FCString::Atoi(*Key.Mid(Br + 1));
				Key = Key.Left(Br);
			}
			if (*In == TEXT('='))
			{
				++In;
				FUSProperty* P = Struct->FindPropertyByName(FName(*Key));
				if (P && Index >= 0 && Index < P->ArrayDim)
				{
					P->ImportText(In, static_cast<uint8*>(Value) + P->Offset + Index * P->ElementSize);
				}
				else
				{
					ReadToken(In);
				}
			}
			SkipSpaces(In);
			if (*In == TEXT(','))
			{
				++In;
			}
			else if (*In != TEXT(')'))
			{
				// malformed: skip a char to make progress
				if (*In)
				{
					++In;
				}
			}
		}
		--GImportDepth;
		if (*In == TEXT(')'))
		{
			++In;
		}
		return true;
	}
	case EUSType::Array:
	{
		FUSScriptArray* A = static_cast<FUSScriptArray*>(Value);
		DestroySingle(Value);
		if (*In != TEXT('('))
		{
			return false;
		}
		++In;
		++GImportDepth;
		TArray<uint8> Elem;
		while (*In && *In != TEXT(')'))
		{
			Elem.SetNumZeroed(Inner->ElementSize);
			Inner->ImportText(In, Elem.GetData());
			const int32 N = A->Num + 1;
			A->Data = static_cast<uint8*>(FMemory::Realloc(A->Data, int64(N) * Inner->ElementSize, 16));
			FMemory::Memcpy(A->Data + int64(A->Num) * Inner->ElementSize, Elem.GetData(), Inner->ElementSize);
			A->Num = A->Max = N;
			SkipSpaces(In);
			if (*In == TEXT(','))
			{
				++In;
			}
			else if (*In != TEXT(')') && *In)
			{
				++In;
			}
		}
		--GImportDepth;
		if (*In == TEXT(')'))
		{
			++In;
		}
		return true;
	}
	default:
		return false;
	}
}

// ---------------------------------------------------------------------------------------------
// FUSStruct / FUSClass / FUSFunction
// ---------------------------------------------------------------------------------------------

void FUSStruct::DestroyValue(uint8* Mem) const
{
	for (FUSProperty* P : CtorProps)
	{
		P->DestroyComplete(Mem + P->Offset);
	}
}

void FUSStruct::CopyValue(uint8* Dst, const uint8* Src) const
{
	if (Dst == Src)
	{
		return;
	}
	if (CtorProps.Num() == 0)
	{
		FMemory::Memmove(Dst, Src, PropertiesSize);
		return;
	}
	for (FUSProperty* P : AllProps)
	{
		P->CopyComplete(Dst + P->Offset, Src + P->Offset);
	}
}

bool FUSStruct::IdenticalValue(const uint8* A, const uint8* B) const
{
	for (FUSProperty* P : AllProps)
	{
		for (int32 i = 0; i < P->ArrayDim; ++i)
		{
			if (!P->IdenticalSingle(A + P->Offset + i * P->ElementSize, B + P->Offset + i * P->ElementSize))
			{
				return false;
			}
		}
	}
	return true;
}

bool FUSStruct::IsChildOf(const FUSStruct* Other) const
{
	for (const FUSStruct* S = this; S; S = S->GetSuperStruct())
	{
		if (S == Other)
		{
			return true;
		}
	}
	return false;
}

bool FUSClass::IsChildOf(const FUSClass* Other) const
{
	if (!Other)
	{
		return false;
	}
	int32 Guard = 0;
	for (const FUSClass* C = this; C && Guard < 128; ++Guard)
	{
		if (C == Other)
		{
			return true;
		}
		FUSClass* Mutable = const_cast<FUSClass*>(C);
		Mutable->EnsureLoaded();
		C = C->GetSuperClass();
	}
	return false;
}

FUSClass* FUSFunction::GetOwnerClass() const
{
	for (FUSObject* O = Outer; O; O = O->Outer)
	{
		if (O->Class && O->Class->Name == TEXT("Class") && (O->Flags & USF_Field))
		{
			return static_cast<FUSClass*>(O);
		}
	}
	return nullptr;
}

bool FUSPropRef::Resolve(FUSClass* InClass, const TCHAR* PropName)
{
	Prop = InClass ? InClass->FindPropertyByName(PropName) : nullptr;
	return Prop != nullptr;
}

int32 FUSExprType::Size() const
{
	if (Prop)
	{
		return Prop->ElementSize;
	}
	switch (Type)
	{
	case EUSType::Byte: return 1;
	case EUSType::Int: case EUSType::Bool: case EUSType::Float: return 4;
	case EUSType::Name: return sizeof(FName);
	case EUSType::Str: return sizeof(FString);
	case EUSType::Object: case EUSType::Class: case EUSType::Pointer: return 8;
	case EUSType::Delegate: return sizeof(FUSDelegate);
	case EUSType::Array: return sizeof(FUSScriptArray);
	default: return 0;
	}
}

// ---------------------------------------------------------------------------------------------
// FUSObject
// ---------------------------------------------------------------------------------------------

FUSObject::~FUSObject()
{
	delete StateFrame;
}

bool FUSObject::IsA(const FUSClass* SomeBase) const
{
	return Class && SomeBase && Class->IsChildOf(SomeBase);
}

bool FUSObject::IsA(FName ClassName) const
{
	int32 Guard = 0;
	for (const FUSClass* C = Class; C && Guard < 128; C = C->GetSuperClass(), ++Guard)
	{
		if (C->Name == ClassName)
		{
			return true;
		}
	}
	return false;
}

FString FUSObject::GetPathName() const
{
	FString Out = Name.ToString();
	int32 Guard = 0;
	for (const FUSObject* O = Outer; O && Guard < 64; O = O->Outer, ++Guard)
	{
		Out = O->Name.ToString() + TEXT(".") + Out;
	}
	return Out;
}

FString FUSObject::GetFullName() const
{
	return FString::Printf(TEXT("%s %s"), Class ? *Class->GetName() : TEXT("Object"), *GetPathName());
}

FName FUSObject::GetPackageName() const
{
	const FUSObject* O = this;
	int32 Guard = 0;
	while (O->Outer && Guard++ < 64)
	{
		O = O->Outer;
	}
	return O->Name;
}

FUSProperty* FUSObject::FindProperty(FName PropName) const
{
	return Class ? Class->FindPropertyByName(PropName) : nullptr;
}

// ---------------------------------------------------------------------------------------------
// FUSParms
// ---------------------------------------------------------------------------------------------

FUSParms::FUSParms(FUSFunction* InFunction)
	: Function(InFunction)
{
	if (Function)
	{
		Buffer = static_cast<uint8*>(FMemory::Malloc(FMath::Max(Function->PropertiesSize, 16), 16));
		FMemory::Memzero(Buffer, FMath::Max(Function->PropertiesSize, 16));
	}
}

FUSParms::~FUSParms()
{
	if (Buffer)
	{
		Function->DestroyValue(Buffer);
		FMemory::Free(Buffer);
	}
}

FUSParms& FUSParms::Put(const void* Value, EUSType Type)
{
	if (!Function)
	{
		return *this;
	}
	const int32 Index = Cursor ? Function->Parms.IndexOfByKey(Cursor) + 1 : 0;
	if (!Function->Parms.IsValidIndex(Index))
	{
		return *this;
	}
	FUSProperty* P = Function->Parms[Index];
	Cursor = P;
	uint8* Dst = Buffer + P->Offset;
	switch (P->Type)
	{
	case EUSType::Byte:
		*Dst = Type == EUSType::Int ? uint8(*static_cast<const int32*>(Value)) : *static_cast<const uint8*>(Value);
		break;
	case EUSType::Int:
		*reinterpret_cast<int32*>(Dst) = Type == EUSType::Byte ? int32(*static_cast<const uint8*>(Value)) : *static_cast<const int32*>(Value);
		break;
	case EUSType::Float:
		*reinterpret_cast<float*>(Dst) = Type == EUSType::Int ? float(*static_cast<const int32*>(Value)) : *static_cast<const float*>(Value);
		break;
	case EUSType::Str:
		if (Type == EUSType::Str)
		{
			*reinterpret_cast<FString*>(Dst) = *static_cast<const FString*>(Value);
		}
		break;
	case EUSType::Struct:
		if (Type == EUSType::Struct)
		{
			FMemory::Memcpy(Dst, Value, FMath::Min<int32>(P->ElementSize, sizeof(FUSVector)));
		}
		break;
	default:
		if (Type == P->Type || (Type == EUSType::Object && P->Type == EUSType::Class))
		{
			FMemory::Memcpy(Dst, Value, P->ElementSize);
		}
		break;
	}
	return *this;
}

FUSParms& FUSParms::Str(const FString& V)
{
	return Put(&V, EUSType::Str);
}

FUSParms& FUSParms::Skip()
{
	if (Function)
	{
		const int32 Index = Cursor ? Function->Parms.IndexOfByKey(Cursor) + 1 : 0;
		if (Function->Parms.IsValidIndex(Index))
		{
			Cursor = Function->Parms[Index];
		}
	}
	return *this;
}

void FUSParms::Call(FUSObject* Self)
{
	if (Function && Self)
	{
		USVM::ProcessEvent(Self, Function, Buffer);
	}
}

FUSProperty* FUSParms::ParmProp(int32 Index) const
{
	return (Function && Function->Parms.IsValidIndex(Index)) ? Function->Parms[Index] : nullptr;
}

uint8* FUSParms::ParmAddr(int32 Index) const
{
	FUSProperty* P = ParmProp(Index);
	return P ? Buffer + P->Offset : nullptr;
}

int32 FUSParms::GetInt(int32 Index) const { const uint8* A = ParmAddr(Index); return A ? *reinterpret_cast<const int32*>(A) : 0; }
float FUSParms::GetFloat(int32 Index) const { const uint8* A = ParmAddr(Index); return A ? *reinterpret_cast<const float*>(A) : 0.f; }
bool FUSParms::GetBool(int32 Index) const { const uint8* A = ParmAddr(Index); return A ? *reinterpret_cast<const uint32*>(A) != 0 : false; }
uint8 FUSParms::GetByte(int32 Index) const { const uint8* A = ParmAddr(Index); return A ? *A : 0; }
FUSObject* FUSParms::GetObject(int32 Index) const { const uint8* A = ParmAddr(Index); return A ? *reinterpret_cast<FUSObject* const*>(A) : nullptr; }
FString FUSParms::GetStr(int32 Index) const { const uint8* A = ParmAddr(Index); return A ? *reinterpret_cast<const FString*>(A) : FString(); }

uint8* FUSParms::ReturnAddr() const
{
	return (Function && Function->ReturnProperty) ? Buffer + Function->ReturnValueOffset : nullptr;
}

bool FUSParms::ReturnBool() const { const uint8* A = ReturnAddr(); return A ? *reinterpret_cast<const uint32*>(A) != 0 : false; }
int32 FUSParms::ReturnInt() const { const uint8* A = ReturnAddr(); return A ? *reinterpret_cast<const int32*>(A) : 0; }
float FUSParms::ReturnFloat() const { const uint8* A = ReturnAddr(); return A ? *reinterpret_cast<const float*>(A) : 0.f; }
FUSObject* FUSParms::ReturnObject() const { const uint8* A = ReturnAddr(); return A ? *reinterpret_cast<FUSObject* const*>(A) : nullptr; }
FString FUSParms::ReturnStr() const { const uint8* A = ReturnAddr(); return A ? *reinterpret_cast<const FString*>(A) : FString(); }
#endif // OPENUT2K4_USCRIPT_ENABLED
