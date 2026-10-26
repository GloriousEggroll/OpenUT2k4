// UScript package loading: script classes, functions, states, properties, bytecode and defaultproperties
// from UE2.5 packages (see Docs/UnrealScript.md for the verified format).

#include "UScriptRuntime.h"
#if OPENUT2K4_USCRIPT_ENABLED
#include "../ArenaContent.h"
#include "../Convert/ArenaConvPackage.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY(LogUScript);

namespace
{
	enum class EFieldKind : uint8 { Other, Class, Function, State, Struct, Enum, Const, Property, TextBuffer };

	EFieldKind KindOfClassName(const FString& Cn)
	{
		if (Cn == TEXT("Class")) return EFieldKind::Class;
		if (Cn == TEXT("Function")) return EFieldKind::Function;
		if (Cn == TEXT("State")) return EFieldKind::State;
		if (Cn == TEXT("Struct")) return EFieldKind::Struct;
		if (Cn == TEXT("Enum")) return EFieldKind::Enum;
		if (Cn == TEXT("Const")) return EFieldKind::Const;
		if (Cn == TEXT("TextBuffer")) return EFieldKind::TextBuffer;
		if (Cn.EndsWith(TEXT("Property"))) return EFieldKind::Property;
		return EFieldKind::Other;
	}

	EUSType TypeOfPropertyClass(const FString& Cn)
	{
		if (Cn == TEXT("ByteProperty")) return EUSType::Byte;
		if (Cn == TEXT("IntProperty")) return EUSType::Int;
		if (Cn == TEXT("BoolProperty")) return EUSType::Bool;
		if (Cn == TEXT("FloatProperty")) return EUSType::Float;
		if (Cn == TEXT("ObjectProperty")) return EUSType::Object;
		if (Cn == TEXT("ClassProperty")) return EUSType::Class;
		if (Cn == TEXT("NameProperty")) return EUSType::Name;
		if (Cn == TEXT("StrProperty")) return EUSType::Str;
		if (Cn == TEXT("StructProperty")) return EUSType::Struct;
		if (Cn == TEXT("ArrayProperty")) return EUSType::Array;
		if (Cn == TEXT("DelegateProperty")) return EUSType::Delegate;
		if (Cn == TEXT("PointerProperty")) return EUSType::Pointer;
		return EUSType::None;
	}

	/** Structs stored as raw binary in tagged properties (UE2 "immutable" natives). */
	bool IsBinaryStructName(FName N)
	{
		static const FName Names[] = { TEXT("Vector"), TEXT("Rotator"), TEXT("Color"), TEXT("Plane"), TEXT("Sphere"), TEXT("Quat"),
			TEXT("Scale"), TEXT("Box"), TEXT("Coords"), TEXT("Range"), TEXT("RangeVector"), TEXT("Guid"), TEXT("Vector2D"),
			TEXT("IntPoint"), TEXT("ADrop") };
		for (const FName& X : Names)
		{
			if (X == N)
			{
				return true;
			}
		}
		return false;
	}

	/** Size of a struct in UE2 binary serialisation (only valid for flat numeric structs). */
	int32 BinaryStructSize(const FUSStruct* S)
	{
		int32 Size = 0;
		for (const FUSProperty* P : S->AllProps)
		{
			int32 E = 0;
			switch (P->Type)
			{
			case EUSType::Byte: E = 1; break;
			case EUSType::Int: case EUSType::Float: case EUSType::Bool: E = 4; break;
			case EUSType::Struct: E = P->Struct ? BinaryStructSize(P->Struct) : -1; break;
			default: E = -1; break;
			}
			if (E < 0)
			{
				return -1;
			}
			Size += E * P->ArrayDim;
		}
		return Size;
	}

	void AppendU32(TArray<uint8>& Out, uint32 V) { Out.Append(reinterpret_cast<const uint8*>(&V), 4); }
	void AppendU16(TArray<uint8>& Out, uint16 V) { Out.Append(reinterpret_cast<const uint8*>(&V), 2); }

	/** Skips a tagged property list (object headers of non-class exports). */
	bool SkipTaggedProps(FConvReader& R, const FUSLinker* L)
	{
		for (int32 Guard = 0; Guard < 100000 && !R.bError; ++Guard)
		{
			const FName N = L->NameAt(R.Index());
			if (R.bError)
			{
				return false;
			}
			if (N == NAME_None)
			{
				return true;
			}
			const uint8 Info = R.U8();
			const int32 Type = Info & 0x0F;
			const int32 SizeCode = (Info >> 4) & 7;
			if (Type == 10)
			{
				R.Index();
			}
			int32 Size = 0;
			switch (SizeCode)
			{
			case 0: Size = 1; break;
			case 1: Size = 2; break;
			case 2: Size = 4; break;
			case 3: Size = 12; break;
			case 4: Size = 16; break;
			case 5: Size = R.U8(); break;
			case 6: Size = R.U16(); break;
			default: Size = R.I32(); break;
			}
			if ((Info & 0x80) && Type != 3)
			{
				const uint8 B = R.U8();
				if (B & 0x80)
				{
					R.Skip((B & 0x40) ? 3 : 1);
				}
			}
			if (Type != 3)
			{
				R.Skip(Size);
			}
		}
		return !R.bError;
	}
}

// =============================================================================================
// FUSLinker
// =============================================================================================

FUSLinker::FUSLinker(FUSRuntime* InRuntime, FConvPackage* InPkg)
	: Runtime(InRuntime), Pkg(InPkg)
{
	PackageName = FName(*InPkg->Name);
	Names.Reserve(InPkg->Names.Num());
	for (const FString& N : InPkg->Names)
	{
		Names.Add(FName(*N));
	}
	Exports.SetNumZeroed(InPkg->Exports.Num());
	Imports.SetNumZeroed(InPkg->Imports.Num());
	ImportResolved.SetNumZeroed(InPkg->Imports.Num());
	PackageObject = Runtime->GetPackageObject(PackageName);
}

FUSObject* FUSLinker::IndexToObject(int32 Index, bool bLoad)
{
	if (Index > 0)
	{
		return GetExport(Index - 1, bLoad);
	}
	if (Index < 0)
	{
		FUSObject* Obj = GetImport(-Index - 1);
		if (bLoad && Obj && (Obj->Flags & USF_Field))
		{
			FUSField* F = static_cast<FUSField*>(Obj);
			if (F->Linker)
			{
				F->Linker->LoadField(F);
			}
		}
		return Obj;
	}
	return nullptr;
}

FUSObject* FUSLinker::GetExport(int32 Ei, bool bLoad)
{
	if (!Exports.IsValidIndex(Ei))
	{
		return nullptr;
	}
	if (!Exports[Ei])
	{
		Exports[Ei] = CreateExport(Ei);
	}
	FUSObject* Obj = Exports[Ei];
	if (bLoad && Obj && (Obj->Flags & USF_Field))
	{
		LoadField(static_cast<FUSField*>(Obj));
	}
	return Obj;
}

FUSObject* FUSLinker::CreateExport(int32 Ei)
{
	const FConvExport& E = Pkg->Exports[Ei];
	const FString ClassName = E.ClassIndex == 0 ? FString(TEXT("Class")) : Pkg->RefName(E.ClassIndex);
	const EFieldKind Kind = KindOfClassName(ClassName);
	FUSObject* Obj = nullptr;
	switch (Kind)
	{
	case EFieldKind::Class: Obj = new FUSClass(); break;
	case EFieldKind::Function: Obj = new FUSFunction(); break;
	case EFieldKind::State: Obj = new FUSState(); break;
	case EFieldKind::Struct: Obj = new FUSStruct(); break;
	case EFieldKind::Enum: Obj = new FUSEnum(); break;
	case EFieldKind::Const: Obj = new FUSConst(); break;
	case EFieldKind::Property:
	{
		FUSProperty* P = new FUSProperty();
		P->Type = TypeOfPropertyClass(ClassName);
		Obj = P;
		break;
	}
	default: break;
	}
	if (!Obj)
	{
		// Non-script object (texture, sound, level actor ...): a placeholder shared by path.
		FUSClass* ObjClass = nullptr;
		if (Kind != EFieldKind::TextBuffer && E.ClassIndex != 0)
		{
			ObjClass = static_cast<FUSClass*>(IndexToObject(E.ClassIndex));
			if (ObjClass && !(ObjClass->Flags & USF_Field))
			{
				ObjClass = nullptr;
			}
		}
		FUSObject* P = Runtime->GetPlaceholder(Pkg->ExportPath(Ei), ObjClass ? ObjClass : Runtime->GetIntrinsicClass(*ClassName));
		return P;
	}
	Obj->Name = FName(*E.Name);
	Obj->Flags |= USF_Field;
	Obj->Class = Runtime->GetIntrinsicClass(*ClassName);
	FUSField* F = static_cast<FUSField*>(Obj);
	F->Linker = this;
	F->ExportIndex = Ei;
	Exports[Ei] = Obj; // before resolving the outer (cycles)
	Obj->Outer = E.PackageIndex != 0 ? IndexToObject(E.PackageIndex) : PackageObject;
	Runtime->AddMetadata(Obj);
	return Obj;
}

FUSObject* FUSLinker::GetImport(int32 Ii)
{
	if (!Imports.IsValidIndex(Ii))
	{
		return nullptr;
	}
	if (ImportResolved[Ii])
	{
		return Imports[Ii];
	}
	ImportResolved[Ii] = true;
	const FConvImport& Im = Pkg->Imports[Ii];
	if (Im.PackageIndex == 0)
	{
		Imports[Ii] = Runtime->GetPackageObject(FName(*Im.Name));
		return Imports[Ii];
	}
	// outermost package and the path below it
	TArray<FString, TInlineAllocator<8>> Parts;
	int32 Index = -Ii - 1;
	FString Top;
	for (int32 Guard = 0; Guard < 64 && Index < 0; ++Guard)
	{
		const FConvImport& X = Pkg->Imports[-Index - 1];
		if (X.PackageIndex == 0)
		{
			Top = X.Name;
			break;
		}
		Parts.Insert(X.Name, 0);
		Index = X.PackageIndex;
	}
	const FString RelPath = FString::Join(Parts, TEXT("."));
	FUSObject* Result = nullptr;
	const EFieldKind Kind = KindOfClassName(Im.ClassName);
	const bool bScriptField = Kind != EFieldKind::Other && Kind != EFieldKind::TextBuffer;
	if (bScriptField)
	{
		FString Err;
		bool bPackageLoaded = false;
		if (FUSLinker* L = Runtime->LoadPackage(FName(*Top), &Err))
		{
			bPackageLoaded = true;
			const int32 Ei = L->FindExport(RelPath, Im.ClassName);
			if (Ei != INDEX_NONE)
			{
				Result = L->GetExport(Ei);
			}
		}
		// classes without script (Core's, and C++-only classes such as Engine.Font or Engine.Viewport, which have no
		// export in their package) are intrinsic
		if (!Result && Kind == EFieldKind::Class && (bPackageLoaded || Top.Equals(TEXT("Core"), ESearchCase::IgnoreCase)))
		{
			Result = Runtime->GetIntrinsicClass(FName(*Im.Name));
		}
		if (!Result)
		{
			Errors.AddUnique(FString::Printf(TEXT("missing import %s %s.%s"), *Im.ClassName, *Top, *RelPath));
		}
	}
	else
	{
		// Do not open content packages for textures/sounds: a placeholder with the right class is enough.
		FUSClass* ObjClass = nullptr;
		if (Kind != EFieldKind::TextBuffer)
		{
			ObjClass = Runtime->FindClass(Im.ClassPackage + TEXT(".") + Im.ClassName);
			if (!ObjClass)
			{
				ObjClass = Runtime->GetIntrinsicClass(FName(*Im.ClassName));
			}
		}
		if (Im.ClassName == TEXT("Package"))
		{
			ObjClass = Runtime->GetIntrinsicClass(TEXT("Package"));
		}
		Result = Runtime->GetPlaceholder(Top + TEXT(".") + RelPath, ObjClass);
	}
	Imports[Ii] = Result;
	return Result;
}

int32 FUSLinker::FindExport(const FString& RelPath, const FString& ClassName) const
{
	const int32 Ei = Pkg->FindExport(RelPath, ClassName);
	if (Ei < 0)
	{
		return INDEX_NONE;
	}
	const FString Cn = Pkg->Exports[Ei].ClassIndex == 0 ? FString(TEXT("Class")) : Pkg->RefName(Pkg->Exports[Ei].ClassIndex);
	if (!ClassName.IsEmpty() && !Cn.Equals(ClassName, ESearchCase::IgnoreCase))
	{
		return INDEX_NONE;
	}
	return Ei;
}

bool FUSLinker::IsTopLevelClass(int32 Ei) const
{
	return Pkg->Exports.IsValidIndex(Ei) && Pkg->Exports[Ei].ClassIndex == 0 && Pkg->Exports[Ei].PackageIndex == 0 && Pkg->Exports[Ei].Size > 0;
}

int32 FUSLinker::FindClassExport(FName ClassName) const
{
	return FindExport(ClassName.ToString(), TEXT("Class"));
}

bool FUSLinker::LoadField(FUSField* Field)
{
	if (!Field)
	{
		return false;
	}
	if (Field->Flags & (USF_Loaded | USF_Loading | USF_Intrinsic))
	{
		return (Field->Flags & USF_LoadFailed) == 0;
	}
	if (Field->Linker != this)
	{
		return Field->Linker ? Field->Linker->LoadField(Field) : false;
	}
	Field->Flags |= USF_Loading;
	bool bOk = true;
	if (FUSClass* C = (Field->Class && Field->Class->Name == TEXT("Class")) ? static_cast<FUSClass*>(Field) : nullptr)
	{
		bOk = LoadClassBody(C);
	}
	else
	{
		const FName Cn = Field->Class ? Field->Class->Name : NAME_None;
		FConvReader R = Pkg->ExportReader(Field->ExportIndex);
		const FConvExport& E = Pkg->Exports[Field->ExportIndex];
		if (E.Flags & USFlags::RF_HasStack)
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
		bOk = SkipTaggedProps(R, this);
		if (bOk)
		{
			if (Cn == TEXT("Function") || Cn == TEXT("State") || Cn == TEXT("Struct"))
			{
				bOk = LoadStruct(static_cast<FUSStruct*>(Field), R);
			}
			else if (Cn == TEXT("Enum"))
			{
				FUSEnum* En = static_cast<FUSEnum*>(Field);
				En->SuperField = static_cast<FUSField*>(IndexToObject(R.Index()));
				En->Next = static_cast<FUSField*>(IndexToObject(R.Index()));
				const int32 N = R.Index();
				for (int32 i = 0; i < N && !R.bError; ++i)
				{
					En->Names.Add(NameAt(R.Index()));
				}
				bOk = !R.bError;
			}
			else if (Cn == TEXT("Const"))
			{
				FUSConst* Co = static_cast<FUSConst*>(Field);
				Co->SuperField = static_cast<FUSField*>(IndexToObject(R.Index()));
				Co->Next = static_cast<FUSField*>(IndexToObject(R.Index()));
				Co->Value = R.FString_();
				bOk = !R.bError;
			}
			else if (Field->Class && Field->Class->Name.ToString().EndsWith(TEXT("Property")))
			{
				// LoadProperty re-reads from the start
				bOk = LoadProperty(static_cast<FUSProperty*>(Field));
			}
		}
		if (R.bError)
		{
			bOk = false;
			Errors.Add(FString::Printf(TEXT("%s: %s"), *Field->GetPathName(), *R.Error));
		}
	}
	Field->Flags &= ~USF_Loading;
	Field->Flags |= USF_Loaded;
	if (!bOk)
	{
		Field->Flags |= USF_LoadFailed;
	}
	return bOk;
}

bool FUSLinker::LoadProperty(FUSProperty* P)
{
	FConvReader R = Pkg->ExportReader(P->ExportIndex);
	if (!SkipTaggedProps(R, this))
	{
		return false;
	}
	P->SuperField = static_cast<FUSField*>(IndexToObject(R.Index()));
	P->Next = static_cast<FUSField*>(IndexToObject(R.Index()));
	const uint32 Dim = R.U32();
	P->ArrayDim = FMath::Max<int32>(1, int32(Dim & 0xFFFF));
	P->PropertyFlags = R.U32();
	P->Category = NameAt(R.Index());
	if (P->PropertyFlags & USFlags::CPF_Net)
	{
		R.U16();
	}
	switch (P->Type)
	{
	case EUSType::Byte:
		P->Enum = static_cast<FUSEnum*>(IndexToObject(R.Index(), true));
		P->ElementSize = 1;
		break;
	case EUSType::Int: case EUSType::Float: case EUSType::Bool:
		P->ElementSize = 4;
		break;
	case EUSType::Object:
		P->PropertyClass = static_cast<FUSClass*>(IndexToObject(R.Index()));
		P->ElementSize = sizeof(FUSObject*);
		break;
	case EUSType::Class:
		P->PropertyClass = static_cast<FUSClass*>(IndexToObject(R.Index()));
		P->MetaClass = static_cast<FUSClass*>(IndexToObject(R.Index()));
		P->ElementSize = sizeof(FUSObject*);
		break;
	case EUSType::Name:
		P->ElementSize = sizeof(FName);
		break;
	case EUSType::Str:
		P->ElementSize = sizeof(FString);
		break;
	case EUSType::Pointer:
		P->ElementSize = sizeof(void*);
		break;
	case EUSType::Delegate:
		P->DelegateFunction = static_cast<FUSFunction*>(IndexToObject(R.Index()));
		P->ElementSize = sizeof(FUSDelegate);
		break;
	case EUSType::Array:
	{
		P->Inner = static_cast<FUSProperty*>(IndexToObject(R.Index(), true));
		P->ElementSize = sizeof(FUSScriptArray);
		if (!P->Inner || P->Inner->Type == EUSType::None || (P->Inner->Flags & USF_LoadFailed))
		{
			Errors.Add(FString::Printf(TEXT("%s: bad array inner"), *P->GetPathName()));
			return false;
		}
		break;
	}
	case EUSType::Struct:
	{
		FUSObject* S = IndexToObject(R.Index(), true);
		P->Struct = (S && (S->Flags & USF_Field)) ? static_cast<FUSStruct*>(S) : nullptr;
		if (!P->Struct || (P->Struct->Flags & USF_LoadFailed))
		{
			Errors.Add(FString::Printf(TEXT("%s: missing struct"), *P->GetPathName()));
			return false;
		}
		P->ElementSize = P->Struct->PropertiesSize;
		break;
	}
	default:
		Errors.Add(FString::Printf(TEXT("%s: unsupported property type %s"), *P->GetPathName(), *P->Class->GetName()));
		return false;
	}
	return !R.bError;
}

bool FUSLinker::LoadStruct(FUSStruct* S, FConvReader& R)
{
	const int32 SuperIdx = R.Index();
	const int32 NextIdx = R.Index();
	R.Index(); // ScriptText
	const int32 ChildIdx = R.Index();
	S->FriendlyName = NameAt(R.Index());
	R.I32(); // UE2.5 struct flags (0)
	S->Line = R.I32();
	R.I32(); // TextPos
	const int32 ScriptSize = R.I32();
	if (R.bError)
	{
		return false;
	}
	S->SuperField = static_cast<FUSField*>(IndexToObject(SuperIdx));
	S->Next = static_cast<FUSField*>(IndexToObject(NextIdx));
	S->Children = static_cast<FUSField*>(IndexToObject(ChildIdx));
	if (!SerializeScript(R, S, ScriptSize))
	{
		Errors.Add(FString::Printf(TEXT("%s: bad bytecode"), *S->GetPathName()));
		return false;
	}
	const FName Cn = S->Class->Name;
	if (Cn == TEXT("Function"))
	{
		FUSFunction* F = static_cast<FUSFunction*>(S);
		F->iNative = R.U16();
		F->OperPrecedence = R.U8();
		F->FunctionFlags = R.U32();
		if (F->FunctionFlags & USFlags::FUNC_Net)
		{
			F->RepOffset = R.U16();
		}
	}
	else if (Cn == TEXT("State") || Cn == TEXT("Class"))
	{
		FUSState* St = static_cast<FUSState*>(S);
		St->ProbeMask = R.U64();
		St->IgnoreMask = R.U64();
		St->LabelTableOffset = R.U16();
		St->StateFlags = R.U32();
	}
	if (R.bError)
	{
		return false;
	}
	// super first (layout), then the children
	if (S->SuperField && (S->SuperField->Flags & USF_Field))
	{
		if (Cn == TEXT("Class"))
		{
			static_cast<FUSClass*>(S->SuperField)->EnsureLoaded();
		}
		else if (S->SuperField->Linker)
		{
			S->SuperField->Linker->LoadField(S->SuperField);
		}
	}
	bool bOk = true;
	int32 Guard = 0;
	for (FUSField* C = S->Children; C && Guard < 100000; C = C->Next, ++Guard)
	{
		if (!(C->Flags & USF_Field))
		{
			break;
		}
		if (C->Linker && !C->Linker->LoadField(C))
		{
			if (C->Class && C->Class->Name.ToString().EndsWith(TEXT("Property")))
			{
				bOk = false;
			}
		}
	}
	return LinkStruct(S) && bOk;
}

bool FUSLinker::LinkStruct(FUSStruct* S)
{
	const FName Cn = S->Class->Name;
	const bool bInherit = (Cn == TEXT("Struct") || Cn == TEXT("Class"));
	int32 Offset = 0;
	S->MinAlignment = 1;
	S->AllProps.Reset();
	S->CtorProps.Reset();
	S->RefProps.Reset();
	S->PropertyMap.Reset();
	if (bInherit)
	{
		if (FUSStruct* Super = S->GetSuperStruct())
		{
			Offset = Super->PropertiesSize;
			S->MinAlignment = Super->MinAlignment;
			S->AllProps = Super->AllProps;
			S->CtorProps = Super->CtorProps;
			S->RefProps = Super->RefProps;
			S->PropertyMap = Super->PropertyMap;
		}
	}
	FUSFunction* Func = Cn == TEXT("Function") ? static_cast<FUSFunction*>(S) : nullptr;
	int32 Guard = 0;
	for (FUSField* C = S->Children; C && Guard < 100000; C = C->Next, ++Guard)
	{
		if (!(C->Flags & USF_Field) || !C->Class || !C->Class->Name.ToString().EndsWith(TEXT("Property")))
		{
			continue;
		}
		FUSProperty* P = static_cast<FUSProperty*>(C);
		if (P->ElementSize <= 0)
		{
			continue;
		}
		const int32 Align = P->GetAlignment();
		Offset = Align > 1 ? (Offset + Align - 1) & ~(Align - 1) : Offset;
		P->Offset = Offset;
		Offset += P->GetSize();
		S->MinAlignment = FMath::Max(S->MinAlignment, Align);
		S->AllProps.Add(P);
		if (P->NeedsDestruct())
		{
			S->CtorProps.Add(P);
		}
		if (P->HasReferences())
		{
			S->RefProps.Add(P);
		}
		S->PropertyMap.Add(P->Name, P);
		if (Func && (P->PropertyFlags & USFlags::CPF_Parm))
		{
			if (P->PropertyFlags & USFlags::CPF_ReturnParm)
			{
				Func->ReturnProperty = P;
				Func->ReturnValueOffset = P->Offset;
			}
			else
			{
				Func->Parms.Add(P);
				Func->NumParms = uint8(Func->Parms.Num());
			}
			Func->ParmsSize = Offset;
		}
	}
	S->PropertiesSize = (Offset + S->MinAlignment - 1) & ~(S->MinAlignment - 1);
	return true;
}

// ---------------------------------------------------------------------------------------------
// Bytecode
// ---------------------------------------------------------------------------------------------

bool FUSLinker::SerializeScript(FConvReader& R, FUSStruct* Owner, int32 ScriptSize)
{
	Owner->Script.Reset();
	if (ScriptSize <= 0)
	{
		return true;
	}
	Owner->Script.Reserve(ScriptSize + 1);
	int32 Guard = 0;
	while (Owner->Script.Num() < ScriptSize && !R.bError && Guard++ < 1000000)
	{
		if (!SerializeExpr(R, Owner->Script, 0))
		{
			return false;
		}
	}
	return !R.bError && Owner->Script.Num() == ScriptSize;
}

bool FUSLinker::SerializeExpr(FConvReader& R, TArray<uint8>& Out, int32 Depth)
{
	if (Depth > 512 || R.bError)
	{
		return false;
	}
	auto Obj = [&]() { AppendU32(Out, uint32(Runtime->AddRef(this, R.Index()))); };
	auto Nm = [&]() { AppendU32(Out, uint32(Runtime->AddName(NameAt(R.Index())))); };
	auto B = [&]() { Out.Add(R.U8()); };
	auto W = [&]() { AppendU16(Out, R.U16()); };
	auto I = [&]() { AppendU32(Out, R.U32()); };
	auto E = [&]() { return SerializeExpr(R, Out, Depth + 1); };
	auto Parms = [&]() -> bool
	{
		for (int32 G = 0; G < 256 && !R.bError; ++G)
		{
			if (R.Pos < R.End && R.Buf[R.Pos] == USToken::EndFunctionParms)
			{
				Out.Add(R.U8());
				return true;
			}
			if (!E())
			{
				return false;
			}
		}
		return false;
	};

	const uint8 T = R.U8();
	Out.Add(T);
	using namespace USToken;
	switch (T)
	{
	case LocalVariable: case InstanceVariable: case DefaultVariable: case NativeParm: case ObjectConst:
		Obj();
		return true;
	case Return: case GotoLabel: case EatString: case BoolVariable: case DynArrayLength:
		return E();
	case Switch:
		B();
		return E();
	case Jump:
		W();
		return true;
	case JumpIfNot:
		W();
		return E();
	case Stop: case Nothing: case EndFunctionParms: case Self: case IntZero: case IntOne: case True: case False:
	case NoObject: case IteratorPop: case IteratorNext: case EndOfScript:
		return true;
	case Assert:
		W();
		return E();
	case Case:
	{
		const uint16 Off = R.U16();
		AppendU16(Out, Off);
		return Off == 0xFFFF ? true : E();
	}
	case LabelTable:
		for (int32 G = 0; G < 4096 && !R.bError; ++G)
		{
			const FName L = NameAt(R.Index());
			AppendU32(Out, uint32(Runtime->AddName(L)));
			I();
			if (L == NAME_None)
			{
				return true;
			}
		}
		return false;
	case Let: case LetBool: case DynArrayElement: case ArrayElement: case LetDelegate:
		return E() && E();
	case USToken::New:
		return E() && E() && E() && E();
	case ClassContext: case Context:
		if (!E())
		{
			return false;
		}
		W();
		B();
		return E();
	case MetaCast: case DynamicCast: case StructMember:
		Obj();
		return E();
	case Skip:
		W();
		return E();
	case VirtualFunction: case GlobalFunction:
		Nm();
		return Parms();
	case FinalFunction:
		Obj();
		return Parms();
	case IntConst:
		I();
		return true;
	case FloatConst:
		I();
		return true;
	case StringConst:
		for (int32 G = 0; G < 65536 && !R.bError; ++G)
		{
			const uint8 C = R.U8();
			Out.Add(C);
			if (C == 0)
			{
				return true;
			}
		}
		return false;
	case UnicodeStringConst:
		for (int32 G = 0; G < 65536 && !R.bError; ++G)
		{
			const uint16 C = R.U16();
			AppendU16(Out, C);
			if (C == 0)
			{
				return true;
			}
		}
		return false;
	case NameConst: case DelegateProperty:
		Nm();
		return true;
	case RotationConst: case VectorConst:
		I(); I(); I();
		return true;
	case ByteConst: case IntConstByte:
		B();
		return true;
	case Iterator:
		if (!E())
		{
			return false;
		}
		W();
		return true;
	case StructCmpEq: case StructCmpNe:
		Obj();
		return E() && E();
	case PrimitiveCast:
		B();
		return E();
	case DynArrayInsert: case DynArrayRemove:
		return E() && E() && E();
	case DelegateFunction:
		Obj();
		Nm();
		return Parms();
	default:
		break;
	}
	if (T >= ExtendedNative && T < FirstNative)
	{
		B();
		return Parms();
	}
	if (T >= FirstNative)
	{
		return Parms();
	}
	R.Fail(FString::Printf(TEXT("unknown token 0x%02X"), T));
	return false;
}

// ---------------------------------------------------------------------------------------------
// Classes and defaults
// ---------------------------------------------------------------------------------------------

namespace
{
	struct FDefaultsReader
	{
		FUSLinker* L;
		FConvReader& R;

		bool ReadItemBinary(FUSProperty* P, uint8* Addr)
		{
			switch (P->Type)
			{
			case EUSType::Byte: *Addr = R.U8(); return true;
			case EUSType::Int: case EUSType::Float: case EUSType::Bool: R.Raw(Addr, 4); return true;
			case EUSType::Name: *reinterpret_cast<FName*>(Addr) = L->NameAt(R.Index()); return true;
			case EUSType::Object: case EUSType::Class: *reinterpret_cast<FUSObject**>(Addr) = L->IndexToObject(R.Index()); return true;
			case EUSType::Str: *reinterpret_cast<FString*>(Addr) = R.FString_(); return true;
			case EUSType::Struct: return ReadStruct(P->Struct, Addr, -1);
			case EUSType::Delegate:
			{
				// UE2 UDelegateProperty: the object (in class defaults the class itself, in a subobject the subobject or
				// its owner's class; FUSWorld::ConstructObject and USGUI::InstanceSubobjects bind those to the instance)
				// and the function name
				FUSDelegate* D = reinterpret_cast<FUSDelegate*>(Addr);
				D->Object = L->IndexToObject(R.Index());
				D->FunctionName = L->NameAt(R.Index());
				return true;
			}
			default: return false;
			}
		}

		bool ReadStructBinary(FUSStruct* S, uint8* Addr)
		{
			for (FUSProperty* P : S->AllProps)
			{
				for (int32 i = 0; i < P->ArrayDim; ++i)
				{
					if (!ReadItemBinary(P, Addr + P->Offset + i * P->ElementSize))
					{
						return false;
					}
				}
			}
			return !R.bError;
		}

		/** A struct value: binary for the native structs (when the size matches), else nested tagged properties. */
		bool ReadStruct(FUSStruct* S, uint8* Addr, int32 Size)
		{
			if (!S)
			{
				return false;
			}
			if (IsBinaryStructName(S->Name))
			{
				const int32 BinSize = BinaryStructSize(S);
				if (BinSize > 0 && (Size < 0 || Size == BinSize))
				{
					return ReadStructBinary(S, Addr);
				}
			}
			return ReadTagged(S, Addr);
		}

		bool ReadArray(FUSProperty* P, uint8* Addr, int64 End)
		{
			FUSScriptArray* A = reinterpret_cast<FUSScriptArray*>(Addr);
			P->DestroySingle(Addr);
			const int32 Count = R.Index();
			if (R.bError || Count < 0 || Count > 1000000)
			{
				return false;
			}
			FUSProperty* Inner = P->Inner;
			const int32 ES = Inner->ElementSize;
			if (Count > 0)
			{
				A->Data = static_cast<uint8*>(FMemory::Malloc(int64(Count) * ES, 16));
				FMemory::Memzero(A->Data, int64(Count) * ES);
				A->Num = A->Max = Count;
			}
			for (int32 i = 0; i < Count && !R.bError && R.Pos <= End; ++i)
			{
				if (!ReadItemBinary(Inner, A->Data + int64(i) * ES))
				{
					return false;
				}
			}
			return !R.bError;
		}

		bool ReadTagged(FUSStruct* Owner, uint8* Data)
		{
			for (int32 Guard = 0; Guard < 100000; ++Guard)
			{
				const FName PName = L->NameAt(R.Index());
				if (R.bError)
				{
					return false;
				}
				if (PName == NAME_None)
				{
					return true;
				}
				const uint8 Info = R.U8();
				const int32 Type = Info & 0x0F;
				const int32 SizeCode = (Info >> 4) & 7;
				const bool bArrayFlag = (Info & 0x80) != 0;
				if (Type == 10)
				{
					R.Index();
				}
				int32 Size = 0;
				switch (SizeCode)
				{
				case 0: Size = 1; break;
				case 1: Size = 2; break;
				case 2: Size = 4; break;
				case 3: Size = 12; break;
				case 4: Size = 16; break;
				case 5: Size = R.U8(); break;
				case 6: Size = R.U16(); break;
				default: Size = R.I32(); break;
				}
				int32 ArrayIndex = 0;
				if (bArrayFlag && Type != 3)
				{
					const uint8 B0 = R.U8();
					if ((B0 & 0x80) == 0)
					{
						ArrayIndex = B0;
					}
					else if ((B0 & 0xC0) == 0x80)
					{
						ArrayIndex = ((B0 & 0x7F) << 8) | R.U8();
					}
					else
					{
						const uint8 B1 = R.U8(), B2 = R.U8(), B3 = R.U8();
						ArrayIndex = ((B0 & 0x3F) << 24) | (B1 << 16) | (B2 << 8) | B3;
					}
				}
				if (R.bError)
				{
					return false;
				}
				const int64 Start = R.Pos;
				const int64 ValueEnd = Type == 3 ? Start : Start + Size;
				FUSProperty* P = Owner->FindPropertyByName(PName);
				if (P && ArrayIndex >= 0 && ArrayIndex < P->ArrayDim)
				{
					uint8* Addr = Data + P->Offset + ArrayIndex * P->ElementSize;
					if (Type == 3)
					{
						if (P->Type == EUSType::Bool)
						{
							*reinterpret_cast<uint32*>(Addr) = bArrayFlag ? 1u : 0u;
						}
					}
					else
					{
						FConvReader Sub(R.Buf, Start, FMath::Min<int64>(ValueEnd, R.End));
						FDefaultsReader SubReader{ L, Sub };
						bool bOk = true;
						switch (P->Type)
						{
						case EUSType::Struct: bOk = SubReader.ReadStruct(P->Struct, Addr, Size); break;
						case EUSType::Array: bOk = SubReader.ReadArray(P, Addr, ValueEnd); break;
						case EUSType::Bool: if (Size == 4) { Sub.Raw(Addr, 4); } break;
						default: bOk = SubReader.ReadItemBinary(P, Addr); break;
						}
						if (!bOk || Sub.bError)
						{
							UE_LOG(LogUScript, Verbose, TEXT("defaults: cannot read %s of %s"), *PName.ToString(), *Owner->GetPathName());
						}
					}
				}
				R.Pos = ValueEnd;
				if (R.Pos > R.End)
				{
					return R.Fail(TEXT("property past end"));
				}
			}
			return false;
		}
	};
}

FUSClass* FUSLinker::GetExportClass(int32 Ei)
{
	if (!Pkg->Exports.IsValidIndex(Ei) || Pkg->Exports[Ei].ClassIndex == 0)
	{
		return nullptr;
	}
	FUSObject* C = IndexToObject(Pkg->Exports[Ei].ClassIndex);
	return (C && (C->Flags & USF_Field) && C->Class && C->Class->Name == TEXT("Class")) ? static_cast<FUSClass*>(C) : nullptr;
}

FString FUSLinker::GetExportName(int32 Ei) const
{
	return Pkg->Exports.IsValidIndex(Ei) ? Pkg->Exports[Ei].Name : FString();
}

void FUSLinker::SetExportObject(int32 Ei, FUSObject* Obj)
{
	if (Exports.IsValidIndex(Ei))
	{
		Exports[Ei] = Obj;
	}
}

bool FUSLinker::LoadInstanceProperties(int32 Ei, FUSObject* Obj)
{
	if (!Obj || !Obj->Data || !Obj->Class || !Pkg->Exports.IsValidIndex(Ei))
	{
		return false;
	}
	FConvReader R = Pkg->ExportReader(Ei);
	if (Pkg->Exports[Ei].Flags & USFlags::RF_HasStack)
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
	FDefaultsReader DR{ this, R };
	return DR.ReadTagged(Obj->Class, Obj->Data);
}

bool FUSLinker::LoadClassBody(FUSClass* C)
{
	FConvReader R = Pkg->ExportReader(C->ExportIndex);
	const FConvExport& E = Pkg->Exports[C->ExportIndex];
	if (E.Flags & USFlags::RF_HasStack)
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
	if (!LoadStruct(C, R))
	{
		C->LoadError = FString::Printf(TEXT("cannot load %s (%s)"), *C->GetPathName(), Errors.Num() ? *Errors.Last() : TEXT("bad data"));
		return false;
	}
	FUSClass* Super = C->GetSuperClass();
	if (Super && (Super->Flags & USF_LoadFailed))
	{
		C->LoadError = FString::Printf(TEXT("superclass %s failed: %s"), *Super->GetPathName(), *Super->LoadError);
		return false;
	}
	C->ClassFlags = R.U32();
	R.Skip(16);
	const int32 NumDeps = R.Index();
	for (int32 i = 0; i < NumDeps && !R.bError; ++i)
	{
		R.Index();
		R.I32();
		R.U32();
	}
	const int32 NumImports = R.Index();
	for (int32 i = 0; i < NumImports && !R.bError; ++i)
	{
		R.Index();
	}
	C->ClassWithin = static_cast<FUSClass*>(IndexToObject(R.Index()));
	C->ConfigName = NameAt(R.Index());
	const int32 NumHide = R.Index();
	for (int32 i = 0; i < NumHide && !R.bError; ++i)
	{
		R.Index();
	}
	if (R.bError)
	{
		C->LoadError = FString::Printf(TEXT("%s: %s"), *C->GetPathName(), *R.Error);
		return false;
	}

	// function and state scopes
	if (Super)
	{
		C->FuncMap = Super->FuncMap;
		C->StateMap = Super->StateMap;
		C->bHasTick = Super->bHasTick;
	}
	int32 Guard = 0;
	for (FUSField* F = C->Children; F && Guard < 100000; F = F->Next, ++Guard)
	{
		if (!F->Class)
		{
			continue;
		}
		const FName Cn = F->Class->Name;
		if (Cn == TEXT("Function"))
		{
			FUSFunction* Fn = static_cast<FUSFunction*>(F);
			C->FuncMap.Add(Fn->Name, Fn);
			if (Fn->Name == TEXT("Tick") && !(Fn->FunctionFlags & USFlags::FUNC_Native))
			{
				C->bHasTick = true;
			}
		}
		else if (Cn == TEXT("State"))
		{
			C->StateMap.Add(F->Name, static_cast<FUSState*>(F));
		}
	}
	// state function maps: super state's scope + own functions
	TFunction<void(FUSState*, int32)> BuildState = [&](FUSState* St, int32 Depth)
	{
		if (St->Flags & USF_Transient || Depth > 32)
		{
			return;
		}
		St->Flags |= USF_Transient; // "built" marker for states
		St->FuncMap.Reset();
		if (FUSState* SuperState = static_cast<FUSState*>(St->SuperField))
		{
			if (SuperState->Class && SuperState->Class->Name == TEXT("State"))
			{
				if (SuperState->Linker)
				{
					SuperState->Linker->LoadField(SuperState);
				}
				BuildState(SuperState, Depth + 1);
				St->FuncMap = SuperState->FuncMap;
			}
		}
		int32 G = 0;
		for (FUSField* F = St->Children; F && G < 100000; F = F->Next, ++G)
		{
			if (F->Class && F->Class->Name == TEXT("Function"))
			{
				St->FuncMap.Add(F->Name, static_cast<FUSFunction*>(F));
			}
		}
	};
	for (TPair<FName, FUSState*>& KV : C->StateMap)
	{
		BuildState(KV.Value, 0);
	}

	// native bindings for this class's functions (class scope and states)
	auto BindNatives = [&](FUSStruct* Scope)
	{
		int32 G = 0;
		for (FUSField* F = Scope->Children; F && G < 100000; F = F->Next, ++G)
		{
			if (!F->Class || F->Class->Name != TEXT("Function"))
			{
				continue;
			}
			FUSFunction* Fn = static_cast<FUSFunction*>(F);
			if (!(Fn->FunctionFlags & USFlags::FUNC_Native))
			{
				// script functions the bridge replaces with C++ (e.g. Actor.TriggerEvent -> the game's event system)
				if (Scope == C)
				{
					if (FUSNativeFunc Override = USNatives::Find(C->Name, Fn->Name))
					{
						Fn->Func = Override;
						Fn->FunctionFlags |= USFlags::FUNC_Native;
						// natives called from C++ read their parameters from a NativeParm script
						Fn->Script.Reset();
						for (FUSProperty* Parm : Fn->Parms)
						{
							Fn->Script.Add(USToken::NativeParm);
							const uint32 Ref = uint32(Runtime->AddRefObject(Parm));
							Fn->Script.Append(reinterpret_cast<const uint8*>(&Ref), 4);
						}
						Fn->Script.Add(USToken::Nothing);
						Fn->Script.Add(USToken::EndOfScript);
					}
				}
				continue;
			}
			Fn->Func = USNatives::Find(C->Name, Fn->Name);
			if (Fn->iNative != 0)
			{
				Runtime->NativeDecls[Fn->iNative] = Fn;
				if (Fn->Func)
				{
					USVM::GNatives[Fn->iNative] = Fn->Func;
				}
			}
			if (!Fn->Func)
			{
				Runtime->NoteMissingNative(Fn);
			}
		}
	};
	BindNatives(C);
	for (FUSField* F = C->Children; F; F = F->Next)
	{
		if (F->Class && F->Class->Name == TEXT("State"))
		{
			BindNatives(static_cast<FUSStruct*>(F));
		}
	}

	// defaults: superclass defaults, then the class's tagged defaultproperties
	C->Defaults.SetNumZeroed(FMath::Max(C->PropertiesSize, 1));
	if (Super && Super->Defaults.Num() > 0)
	{
		for (FUSProperty* P : Super->AllProps)
		{
			P->CopyComplete(C->Defaults.GetData() + P->Offset, Super->Defaults.GetData() + P->Offset);
		}
	}
	{
		FDefaultsReader DR{ this, R };
		if (!DR.ReadTagged(C, C->Defaults.GetData()))
		{
			UE_LOG(LogUScript, Verbose, TEXT("%s: defaultproperties incomplete (%s)"), *C->GetPathName(), *R.Error);
		}
	}

	// config (player's ini, read only) and localized text (.int)
	const FString ClassPath = C->GetPathName();
	for (FUSProperty* P : C->AllProps)
	{
		if (P->PropertyFlags & USFlags::CPF_Config)
		{
			FString Section = ClassPath;
			if (P->PropertyFlags & USFlags::CPF_GlobalConfig)
			{
				for (FUSObject* O = P->Outer; O; O = O->Outer)
				{
					if (O->Class && O->Class->Name == TEXT("Class"))
					{
						Section = O->GetPathName();
						break;
					}
				}
			}
			TArray<FString> Values;
			if (P->ArrayDim > 1)
			{
				for (int32 i = 0; i < P->ArrayDim; ++i)
				{
					TArray<FString> V;
					if (Runtime->GetConfigValue(Section, FString::Printf(TEXT("%s[%d]"), *P->GetName(), i), V) && V.Num())
					{
						const TCHAR* In = *V.Last();
						P->ImportText(In, C->Defaults.GetData() + P->Offset + i * P->ElementSize);
					}
				}
			}
			else if (Runtime->GetConfigValue(Section, P->GetName(), Values) && Values.Num())
			{
				uint8* Addr = C->Defaults.GetData() + P->Offset;
				if (P->Type == EUSType::Array)
				{
					P->DestroySingle(Addr);
					FUSScriptArray* A = reinterpret_cast<FUSScriptArray*>(Addr);
					const int32 ES = P->Inner->ElementSize;
					A->Data = static_cast<uint8*>(FMemory::Malloc(int64(Values.Num()) * ES, 16));
					FMemory::Memzero(A->Data, int64(Values.Num()) * ES);
					A->Num = A->Max = Values.Num();
					for (int32 i = 0; i < Values.Num(); ++i)
					{
						const TCHAR* In = *Values[i];
						P->Inner->ImportText(In, A->Data + int64(i) * ES);
					}
				}
				else
				{
					const TCHAR* In = *Values.Last();
					P->ImportText(In, Addr);
				}
			}
		}
		if (P->PropertyFlags & USFlags::CPF_Localized)
		{
			FString Value;
			// static arrays: one key per element ("Messages[3]=...")
			if (P->ArrayDim > 1)
			{
				for (int32 i = 0; i < P->ArrayDim; ++i)
				{
					FString Elem;
					if (Runtime->GetLocalized(PackageName.ToString(), C->GetName(), FString::Printf(TEXT("%s[%d]"), *P->GetName(), i), Elem))
					{
						uint8* ElemAddr = C->Defaults.GetData() + P->Offset + i * P->ElementSize;
						if (P->Type == EUSType::Str)
						{
							*reinterpret_cast<FString*>(ElemAddr) = Elem;
						}
						else
						{
							const TCHAR* In = *Elem;
							P->ImportText(In, ElemAddr);
						}
					}
				}
			}
			else if (Runtime->GetLocalized(PackageName.ToString(), C->GetName(), P->GetName(), Value))
			{
				if (P->Type == EUSType::Array && P->Inner && P->Inner->Type == EUSType::Str && Value.StartsWith(TEXT("(")))
				{
					// ("A & B","C",...): split at top-level commas, quotes removed (the text may hold any character)
					TArray<FString> Items;
					FString Cur;
					bool bQuote = false;
					const FString Body = Value.Mid(1, Value.EndsWith(TEXT(")")) ? Value.Len() - 2 : Value.Len() - 1);
					for (int32 i = 0; i < Body.Len(); ++i)
					{
						const TCHAR Ch = Body[i];
						if (Ch == TEXT('\\') && bQuote && i + 1 < Body.Len())
						{
							Cur.AppendChar(Body[++i]);
							continue;
						}
						if (Ch == TEXT('"'))
						{
							bQuote = !bQuote;
							continue;
						}
						if (Ch == TEXT(',') && !bQuote)
						{
							Items.Add(Cur);
							Cur.Reset();
							continue;
						}
						Cur.AppendChar(Ch);
					}
					if (!Cur.IsEmpty() || Items.Num() > 0)
					{
						Items.Add(Cur);
					}
					FUSScriptArray* A = reinterpret_cast<FUSScriptArray*>(C->Defaults.GetData() + P->Offset);
					P->DestroySingle(A);
					FMemory::Memzero(A, sizeof(FUSScriptArray));
					const int32 ES = P->Inner->ElementSize;
					A->Data = static_cast<uint8*>(FMemory::Malloc(FMath::Max<int64>(1, int64(Items.Num()) * ES), 16));
					FMemory::Memzero(A->Data, int64(Items.Num()) * ES);
					A->Num = A->Max = Items.Num();
					for (int32 i = 0; i < Items.Num(); ++i)
					{
						*reinterpret_cast<FString*>(A->Data + int64(i) * ES) = Items[i];
					}
				}
				else if (P->Type == EUSType::Str)
				{
					// as written (ImportText of the unquoted value would drop leading spaces: " entered the game.")
					*reinterpret_cast<FString*>(C->Defaults.GetData() + P->Offset) = Value;
				}
				else
				{
					const TCHAR* In = *Value;
					P->ImportText(In, C->Defaults.GetData() + P->Offset);
				}
			}
		}
	}

	FUSObject* DO = new FUSObject();
	DO->Class = C;
	DO->Name = FName(*(TEXT("Default__") + C->GetName()));
	DO->Outer = PackageObject;
	DO->Flags = USF_ClassDefault;
	DO->Data = C->Defaults.GetData();
	C->DefaultObject = DO;
	Runtime->AddMetadata(DO);

	if (C->Name == TEXT("Actor") && PackageName == TEXT("Engine"))
	{
		Runtime->ActorClass = C;
	}
	else if (C->Name == TEXT("Pawn") && PackageName == TEXT("Engine"))
	{
		Runtime->PawnClass = C;
	}
	else if (C->Name == TEXT("Controller") && PackageName == TEXT("Engine"))
	{
		Runtime->ControllerClass = C;
	}
	return true;
}

bool FUSClass::EnsureLoaded()
{
	if (Flags & USF_Loaded)
	{
		return (Flags & USF_LoadFailed) == 0;
	}
	if (Flags & (USF_Loading | USF_Intrinsic))
	{
		return true;
	}
	if (!Linker)
	{
		return false;
	}
	const bool bOk = Linker->LoadField(this);
	if (!bOk)
	{
		if (LoadError.IsEmpty())
		{
			LoadError = FString::Printf(TEXT("cannot load %s"), *GetPathName());
		}
		UE_LOG(LogUScript, Warning, TEXT("%s"), *LoadError);
	}
	return bOk;
}

// =============================================================================================
// FUSRuntime
// =============================================================================================

FUSRuntime& FUSRuntime::Get()
{
	static FUSRuntime* Instance = new FUSRuntime();
	return *Instance;
}

FUSRuntime::~FUSRuntime()
{
	delete Packages;
}

bool FUSRuntime::Initialize(const FString& InGameDir, FString& OutError)
{
	if (Packages)
	{
		return true;
	}
	if (InGameDir.IsEmpty() || !FPaths::DirectoryExists(InGameDir))
	{
		OutError = TEXT("no UT2004 install found (the script runtime loads Core/Engine from it)");
		return false;
	}
	GameDir = InGameDir;
	Packages = new FConvPackageManager(InGameDir);
	USVM::Init();
	CreateIntrinsics();
	AddName(NAME_None);
	Refs.AddDefaulted(); // index 0 = None
	FString Err;
	FUSLinker* Core = LoadPackage(TEXT("Core"), &Err);
	if (!Core)
	{
		OutError = FString::Printf(TEXT("cannot load Core.u: %s"), *Err);
		delete Packages;
		Packages = nullptr;
		return false;
	}
	const int32 Ei = Core->FindClassExport(TEXT("Object"));
	CoreObject = Ei != INDEX_NONE ? static_cast<FUSClass*>(Core->GetExport(Ei)) : nullptr;
	if (!CoreObject || !CoreObject->EnsureLoaded())
	{
		OutError = TEXT("Core.Object could not be loaded");
		return false;
	}
	for (TPair<FName, FUSClass*>& KV : Intrinsics)
	{
		if (KV.Value != CoreObject && !KV.Value->SuperField)
		{
			KV.Value->SuperField = CoreObject;
			KV.Value->FuncMap = CoreObject->FuncMap;
			KV.Value->PropertiesSize = CoreObject->PropertiesSize;
			KV.Value->AllProps = CoreObject->AllProps;
			KV.Value->PropertyMap = CoreObject->PropertyMap;
		}
	}
	UE_LOG(LogUScript, Log, TEXT("UScript runtime: %s (Core.Object %d bytes)"), *GameDir, CoreObject->PropertiesSize);
	return true;
}

void FUSRuntime::AddPackageFolder(const FString& Folder)
{
	if (!Packages || ExtraFolders.Contains(Folder))
	{
		return;
	}
	ExtraFolders.Add(Folder);
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Folder, TEXT("*.*"), true, false);
	TArray<FString> Pkgs;
	for (const FString& F : Files)
	{
		const FString Ext = FPaths::GetExtension(F).ToLower();
		if (Ext == TEXT("u") || Ext == TEXT("ut2") || Ext == TEXT("utx") || Ext == TEXT("usx") || Ext == TEXT("uax") || Ext == TEXT("ukx"))
		{
			Pkgs.Add(F);
		}
	}
	Packages->AddPackageFiles(Pkgs, nullptr);
}

void FUSRuntime::AddPackageFiles(const TArray<FString>& Files)
{
	if (!Packages)
	{
		return;
	}
	TArray<FString> Pkgs;
	for (const FString& F : Files)
	{
		if (FPaths::GetExtension(F).Equals(TEXT("int"), ESearchCase::IgnoreCase))
		{
			ExtraIntFiles.Add(FPaths::GetCleanFilename(F).ToLower(), F);
		}
		else
		{
			Pkgs.Add(F);
		}
	}
	Packages->AddPackageFiles(Pkgs, nullptr);
}

void FUSRuntime::CreateIntrinsics()
{
	static const TCHAR* IntrinsicNames[] = { TEXT("Class"), TEXT("Function"), TEXT("State"), TEXT("Struct"), TEXT("Enum"), TEXT("Const"),
		TEXT("ByteProperty"), TEXT("IntProperty"), TEXT("BoolProperty"), TEXT("FloatProperty"), TEXT("ObjectProperty"),
		TEXT("ClassProperty"), TEXT("NameProperty"), TEXT("StrProperty"), TEXT("StructProperty"), TEXT("ArrayProperty"),
		TEXT("DelegateProperty"), TEXT("PointerProperty"), TEXT("Package"), TEXT("TextBuffer"), TEXT("Field"), TEXT("Property") };
	for (const TCHAR* N : IntrinsicNames)
	{
		GetIntrinsicClass(N);
	}
}

FUSClass* FUSRuntime::GetIntrinsicClass(FName ClassName)
{
	if (FUSClass** Hit = Intrinsics.Find(ClassName))
	{
		return *Hit;
	}
	FUSClass* C = new FUSClass();
	C->Name = ClassName;
	C->Flags = USF_Field | USF_Loaded | USF_Intrinsic;
	C->Outer = GetPackageObject(TEXT("Core"));
	C->SuperField = CoreObject;
	if (CoreObject)
	{
		C->FuncMap = CoreObject->FuncMap;
		C->PropertiesSize = CoreObject->PropertiesSize;
		C->AllProps = CoreObject->AllProps;
		C->PropertyMap = CoreObject->PropertyMap;
	}
	Intrinsics.Add(ClassName, C);
	// metaclass of every class is "Class" (itself included)
	C->Class = Intrinsics.FindRef(TEXT("Class"));
	if (!C->Class)
	{
		C->Class = C;
	}
	AddMetadata(C);
	return C;
}

FUSObject* FUSRuntime::GetPackageObject(FName PackageName)
{
	if (FUSObject** Hit = PackageObjects.Find(PackageName))
	{
		return *Hit;
	}
	FUSObject* P = new FUSObject();
	P->Name = PackageName;
	P->Flags = USF_Placeholder;
	PackageObjects.Add(PackageName, P);
	P->Class = Intrinsics.FindRef(TEXT("Package"));
	AddMetadata(P);
	return P;
}

FUSObject* FUSRuntime::GetPlaceholder(const FString& PathName, FUSClass* ObjClass)
{
	const FString Key = PathName.ToLower();
	if (FUSObject** Hit = Placeholders.Find(Key))
	{
		// made as the outer of another placeholder (a font's page textures are inside the font): now its class is known
		if (ObjClass && (*Hit)->Class && (*Hit)->Class->Name == TEXT("Package") && ObjClass->Name != TEXT("Package"))
		{
			(*Hit)->Class = ObjClass;
		}
		return *Hit;
	}
	FString Outer, Leaf;
	FUSObject* OuterObj = nullptr;
	if (PathName.Split(TEXT("."), &Outer, &Leaf, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
	{
		OuterObj = Outer.Contains(TEXT(".")) ? GetPlaceholder(Outer, GetIntrinsicClass(TEXT("Package"))) : GetPackageObject(FName(*Outer));
	}
	else
	{
		Leaf = PathName;
	}
	FUSObject* P = new FUSObject();
	P->Name = FName(*Leaf);
	P->Outer = OuterObj;
	P->Class = ObjClass ? ObjClass : CoreObject;
	P->Flags = USF_Placeholder;
	Placeholders.Add(Key, P);
	AddMetadata(P);
	return P;
}

bool FUSRuntime::MaterializePlaceholder(FUSObject* Obj)
{
	if (!Obj || Obj->Data || !(Obj->Flags & USF_Placeholder) || !Obj->Class || (Obj->Class->Flags & USF_Intrinsic) || !Obj->Class->EnsureLoaded()
		|| Obj->Class->PropertiesSize <= 0)
	{
		return Obj && Obj->Data;
	}
	FUSClass* C = Obj->Class;
	Obj->Data = static_cast<uint8*>(FMemory::Malloc(C->PropertiesSize, 16));
	FMemory::Memzero(Obj->Data, C->PropertiesSize);
	if (C->Defaults.Num() >= C->PropertiesSize)
	{
		for (FUSProperty* P : C->AllProps)
		{
			P->CopyComplete(Obj->Data + P->Offset, C->Defaults.GetData() + P->Offset);
		}
	}
	if (PlaceholderInit)
	{
		PlaceholderInit(Obj);
	}
	return true;
}

void FUSRuntime::AddMetadata(FUSObject* Obj)
{
	Obj->Index = Metadata.Num();
	Metadata.Add(Obj);
}

FUSLinker* FUSRuntime::LoadPackage(FName PackageName, FString* OutError)
{
	if (FUSLinker** Hit = Linkers.Find(PackageName))
	{
		return *Hit;
	}
	if (!Packages)
	{
		if (OutError)
		{
			*OutError = TEXT("runtime not initialised");
		}
		return nullptr;
	}
	FConvPackage* Pkg = Packages->LoadPackage(PackageName.ToString());
	if (!Pkg)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("package %s not found"), *PackageName.ToString());
		}
		Linkers.Add(PackageName, nullptr);
		return nullptr;
	}
	FUSLinker* L = new FUSLinker(this, Pkg);
	OwnedLinkers.Add(L);
	Linkers.Add(PackageName, L);
	return L;
}

FUSLinker* FUSRuntime::LoadPackageFile(const FString& Path, FString* OutError)
{
	const FName PackageName(*FPaths::GetBaseFilename(Path));
	if (FUSLinker** Hit = Linkers.Find(PackageName))
	{
		if (*Hit)
		{
			return *Hit;
		}
	}
	if (!Packages)
	{
		return nullptr;
	}
	FConvPackage* Pkg = Packages->OpenFile(Path);
	if (!Pkg)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("cannot open %s"), *Path);
		}
		return nullptr;
	}
	FUSLinker* L = new FUSLinker(this, Pkg);
	OwnedLinkers.Add(L);
	Linkers.Add(PackageName, L);
	return L;
}

FUSClass* FUSRuntime::FindClass(const FString& PathName, FString* OutError)
{
	FString PkgName, ClassName;
	if (!PathName.Split(TEXT("."), &PkgName, &ClassName))
	{
		if (FUSClass* C = FindLoadedClass(FName(*PathName)))
		{
			return C;
		}
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("bad class name %s"), *PathName);
		}
		return nullptr;
	}
	FUSLinker* L = LoadPackage(FName(*PkgName), OutError);
	if (!L)
	{
		return nullptr;
	}
	const int32 Ei = L->FindClassExport(FName(*ClassName));
	if (Ei == INDEX_NONE)
	{
		if (PkgName.Equals(TEXT("Core"), ESearchCase::IgnoreCase))
		{
			if (FUSClass** I = Intrinsics.Find(FName(*ClassName)))
			{
				return *I;
			}
		}
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("class %s not found"), *PathName);
		}
		return nullptr;
	}
	FUSClass* C = static_cast<FUSClass*>(L->GetExport(Ei));
	if (!C || !C->EnsureLoaded())
	{
		if (OutError)
		{
			*OutError = C ? C->LoadError : FString::Printf(TEXT("class %s not loadable"), *PathName);
		}
		return nullptr;
	}
	return C;
}

FUSClass* FUSRuntime::FindLoadedClass(FName ClassName) const
{
	for (const TPair<FName, FUSLinker*>& KV : Linkers)
	{
		if (!KV.Value)
		{
			continue;
		}
		const int32 Ei = KV.Value->FindClassExport(ClassName);
		if (Ei != INDEX_NONE)
		{
			FUSClass* C = static_cast<FUSClass*>(KV.Value->GetExport(Ei));
			if (C && C->EnsureLoaded())
			{
				return C;
			}
		}
	}
	if (FUSClass* const* I = Intrinsics.Find(ClassName))
	{
		return *I;
	}
	return nullptr;
}

FUSObject* FUSRuntime::FindObject(const FString& PathName, FUSClass* RequiredClass)
{
	FString PkgName, Rest;
	if (!PathName.Split(TEXT("."), &PkgName, &Rest))
	{
		return nullptr;
	}
	FUSLinker* L = LoadPackage(FName(*PkgName));
	if (!L)
	{
		return nullptr;
	}
	const FString ClassName = (RequiredClass && RequiredClass->Name == TEXT("Class")) ? FString(TEXT("Class")) : FString();
	const int32 Ei = L->FindExport(Rest, ClassName);
	if (Ei == INDEX_NONE)
	{
		return nullptr;
	}
	FUSObject* O = L->GetExport(Ei, true);
	if (O && O->Class && O->Class->Name == TEXT("Class"))
	{
		static_cast<FUSClass*>(O)->EnsureLoaded();
	}
	return O;
}

int32 FUSRuntime::AddName(FName Name)
{
	if (const int32* Hit = NameLookup.Find(Name))
	{
		return *Hit;
	}
	const int32 I = Names.Add(Name);
	NameLookup.Add(Name, I);
	return I;
}

int32 FUSRuntime::AddRef(FUSLinker* Linker, int32 PackageIndex)
{
	if (PackageIndex == 0)
	{
		return 0;
	}
	FRef R;
	R.Linker = Linker;
	R.Index = PackageIndex;
	return Refs.Add(R);
}

int32 FUSRuntime::AddRefObject(FUSObject* Obj)
{
	if (!Obj)
	{
		return 0;
	}
	if (const int32* Hit = RefLookup.Find(Obj))
	{
		return *Hit;
	}
	FRef R;
	R.Obj = Obj;
	const int32 I = Refs.Add(R);
	RefLookup.Add(Obj, I);
	return I;
}

FUSObject* FUSRuntime::ResolveRef(int32 Index)
{
	if (!Refs.IsValidIndex(Index) || Index == 0)
	{
		return nullptr;
	}
	if (!Refs[Index].Obj && Refs[Index].Linker)
	{
		// loading can add references (Refs may reallocate), so the entry is looked up again afterwards
		FUSObject* O = Refs[Index].Linker->IndexToObject(Refs[Index].Index, true);
		// fields used by code need their owning class loaded (offsets, function scopes)
		if (O && (O->Flags & USF_Field))
		{
			for (FUSObject* Outer = O; Outer; Outer = Outer->Outer)
			{
				if (Outer->Class && Outer->Class->Name == TEXT("Class") && (Outer->Flags & USF_Field))
				{
					static_cast<FUSClass*>(Outer)->EnsureLoaded();
					break;
				}
			}
		}
		FRef& R = Refs[Index];
		R.Obj = O;
		R.Linker = nullptr;
	}
	return Refs[Index].Obj;
}

void FUSRuntime::NoteMissingNative(FUSFunction* Function)
{
	const FUSClass* Owner = Function->GetOwnerClass();
	const FString Key = FString::Printf(TEXT("%s.%s"), Owner ? *Owner->GetName() : TEXT("?"), *Function->GetName());
	MissingNatives.Add(Key);
	MissingNativesByPackage.FindOrAdd(Function->GetPackageName().ToString().ToLower()).Add(Key);
}

// ---------------------------------------------------------------------------------------------
// ini / int files (read only)
// ---------------------------------------------------------------------------------------------

FUSRuntime::FIniFile* FUSRuntime::LoadIni(const FString& Path)
{
	if (FIniFile* Hit = IniFiles.Find(Path))
	{
		return Hit->bLoaded ? Hit : nullptr;
	}
	FIniFile& F = IniFiles.Add(Path);
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return nullptr;
	}
	F.bLoaded = true;
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	TMap<FString, TArray<FString>>* Section = nullptr;
	for (FString& Line : Lines)
	{
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty() || Line.StartsWith(TEXT(";")))
		{
			continue;
		}
		if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]")))
		{
			Section = &F.Sections.FindOrAdd(Line.Mid(1, Line.Len() - 2).ToLower());
			continue;
		}
		int32 Eq;
		if (Section && Line.FindChar(TEXT('='), Eq))
		{
			Section->FindOrAdd(Line.Left(Eq).TrimEnd().ToLower()).Add(Line.Mid(Eq + 1).TrimStart());
		}
	}
	return &F;
}

FString FUSRuntime::UserConfigFile()
{
	return FPaths::ProjectSavedDir() / TEXT("UE2Config") / TEXT("User.ini");
}

void FUSRuntime::SaveConfig(FUSClass* Class, const uint8* Data)
{
	if (!Class || !Data)
	{
		return;
	}
	FIniFile* F = LoadIni(UserConfigFile());
	if (!F)
	{
		F = &IniFiles.FindOrAdd(UserConfigFile()); // the first save makes it
		F->bLoaded = true;
	}
	const FString ClassPath = Class->GetPathName();
	for (FUSProperty* P : Class->AllProps)
	{
		if (!(P->PropertyFlags & USFlags::CPF_Config))
		{
			continue;
		}
		FString Section = ClassPath;
		if (P->PropertyFlags & USFlags::CPF_GlobalConfig)
		{
			for (FUSObject* O = P->Outer; O; O = O->Outer)
			{
				if (O->Class && O->Class->Name == TEXT("Class"))
				{
					Section = O->GetPathName();
					break;
				}
			}
		}
		TMap<FString, TArray<FString>>& S = F->Sections.FindOrAdd(Section.ToLower());
		const uint8* Addr = Data + P->Offset;
		if (P->Type == EUSType::Array && P->Inner)
		{
			const FUSScriptArray& A = *reinterpret_cast<const FUSScriptArray*>(Addr);
			TArray<FString>& V = S.FindOrAdd(P->GetName().ToLower());
			V.Reset();
			for (int32 i = 0; i < A.Num && A.Data; ++i)
			{
				FString T;
				P->Inner->ExportText(T, A.Data + i * P->Inner->ElementSize, true);
				V.Add(T);
			}
		}
		else
		{
			for (int32 i = 0; i < P->ArrayDim; ++i)
			{
				FString T;
				P->ExportText(T, Addr + i * P->ElementSize, true);
				const FString Key = P->ArrayDim > 1 ? FString::Printf(TEXT("%s[%d]"), *P->GetName(), i) : P->GetName();
				S.FindOrAdd(Key.ToLower()) = { T };
			}
		}
	}
	WriteUserConfig(F);
}

void FUSRuntime::SetUserConfigValue(const FString& Section, const FString& Key, const FString& Value)
{
	FIniFile* F = LoadIni(UserConfigFile());
	if (!F)
	{
		F = &IniFiles.FindOrAdd(UserConfigFile());
		F->bLoaded = true;
	}
	TMap<FString, TArray<FString>>& S = F->Sections.FindOrAdd(Section.ToLower());
	if (Value.IsEmpty())
	{
		S.Remove(Key.ToLower());
	}
	else
	{
		S.FindOrAdd(Key.ToLower()) = { Value };
	}
	WriteUserConfig(F);
}

TMap<FString, FString> FUSRuntime::GetUserConfigSection(const FString& Section)
{
	TMap<FString, FString> Out;
	if (FIniFile* F = LoadIni(UserConfigFile()))
	{
		if (const TMap<FString, TArray<FString>>* S = F->Sections.Find(Section.ToLower()))
		{
			for (const TPair<FString, TArray<FString>>& KV : *S)
			{
				if (KV.Value.Num())
				{
					Out.Add(KV.Key, KV.Value.Last());
				}
			}
		}
	}
	return Out;
}

void FUSRuntime::WriteUserConfig(FIniFile* F)
{
	// written whole (section and key names as kept: lower case, which UE2 reads case-insensitively too)
	FString Out;
	for (const TPair<FString, TMap<FString, TArray<FString>>>& Sec : F->Sections)
	{
		Out += TEXT("[") + Sec.Key + TEXT("]\n");
		for (const TPair<FString, TArray<FString>>& KV : Sec.Value)
		{
			for (const FString& V : KV.Value)
			{
				Out += KV.Key + TEXT("=") + V + TEXT("\n");
			}
		}
		Out += TEXT("\n");
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(UserConfigFile()), true);
	FFileHelper::SaveStringToFile(Out, *UserConfigFile());
}

bool FUSRuntime::GetConfigValue(const FString& Section, const FString& Key, TArray<FString>& OutValues)
{
	if (FIniFile* U = LoadIni(UserConfigFile()))
	{
		if (const TMap<FString, TArray<FString>>* S = U->Sections.Find(Section.ToLower()))
		{
			if (const TArray<FString>* V = S->Find(Key.ToLower()))
			{
				OutValues = *V;
				return true;
			}
		}
	}
	const FString System = GameDir / TEXT("System");
	for (const TCHAR* File : { TEXT("UT2004.ini"), TEXT("Default.ini"), TEXT("User.ini"), TEXT("DefUser.ini") })
	{
		if (FIniFile* F = LoadIni(System / File))
		{
			if (const TMap<FString, TArray<FString>>* S = F->Sections.Find(Section.ToLower()))
			{
				if (const TArray<FString>* V = S->Find(Key.ToLower()))
				{
					OutValues = *V;
					return true;
				}
			}
		}
	}
	return false;
}

bool FUSRuntime::GetLocalized(const FString& Package, const FString& Section, const FString& Key, FString& OutValue)
{
	// the content cache's copy of the stock file first (<cache>/localization, ArenaConvLocalization.h), then the install
	FString Path = ArenaContent::FindLocalizationFile(Package + TEXT(".int"));
	if (Path.IsEmpty())
	{
		Path = GameDir / TEXT("System") / (Package + TEXT(".int"));
	}
	if (!FPaths::FileExists(Path))
	{
		if (const FString* Extra = ExtraIntFiles.Find((Package + TEXT(".int")).ToLower()))
		{
			Path = *Extra;
		}
	}
	if (!FPaths::FileExists(Path))
	{
		for (const FString& Folder : ExtraFolders)
		{
			TArray<FString> Found;
			IFileManager::Get().FindFilesRecursive(Found, *Folder, *(Package + TEXT(".int")), true, false);
			if (Found.Num())
			{
				Path = Found[0];
				break;
			}
		}
	}
	FIniFile* F = LoadIni(Path);
	if (!F)
	{
		return false;
	}
	const TMap<FString, TArray<FString>>* S = F->Sections.Find(Section.ToLower());
	const TArray<FString>* V = S ? S->Find(Key.ToLower()) : nullptr;
	if (!V || V->Num() == 0)
	{
		return false;
	}
	OutValue = V->Last();
	// .int values are usually quoted
	if (OutValue.Len() >= 2 && OutValue.StartsWith(TEXT("\"")) && OutValue.EndsWith(TEXT("\"")))
	{
		OutValue = OutValue.Mid(1, OutValue.Len() - 2);
	}
	return true;
}
#endif // OPENUT2K4_USCRIPT_ENABLED
