// UScript interpreter: token handlers, function calls, states and latent code (UE2.5 semantics).

#include "UScriptCore.h"
#if OPENUT2K4_USCRIPT_ENABLED
#include "UScriptRuntime.h"
#include "UScriptWorld.h"

namespace USVM
{
	FUSProperty* GProperty = nullptr;
	uint8* GPropAddr = nullptr;
	bool GAbort = false;
	FUSNativeFunc GNatives[4096] = {};
	uint64 GStatCalls = 0;
	uint64 GStatTokens = 0;
}

namespace
{
	FUSRuntime* GRT = nullptr;
	/** Set by DynArrayLength evaluated as an lvalue (array.Length = N). */
	bool GArrayLengthSet = false;
	/** Incremented by iterator natives when they run a loop. */
	int32 GIteratorRuns = 0;
	int32 GCallDepth = 0;
	int64 GRunaway = 0;
	constexpr int32 MaxCallDepth = 250;
	constexpr int64 MaxRunaway = 10000000;
	constexpr int32 TempBufferSize = 2048;

	/** Bump allocator for script locals (LIFO). */
	struct FUSVMStack
	{
		TArray<uint8*> Blocks;
		int32 Block = 0;
		int64 Used = 0;
		static constexpr int64 BlockSize = 1 << 20;

		struct FMark { int32 Block; int64 Used; };
		FMark Mark() const { return { Block, Used }; }
		void Pop(const FMark& M) { Block = M.Block; Used = M.Used; }
		uint8* Alloc(int64 Size)
		{
			Size = (FMath::Max<int64>(Size, 16) + 15) & ~int64(15);
			if (Size > BlockSize)
			{
				return nullptr;
			}
			if (Blocks.Num() == 0)
			{
				Blocks.Add(static_cast<uint8*>(FMemory::Malloc(BlockSize, 16)));
			}
			if (Used + Size > BlockSize)
			{
				++Block;
				Used = 0;
				if (Block >= Blocks.Num())
				{
					Blocks.Add(static_cast<uint8*>(FMemory::Malloc(BlockSize, 16)));
				}
			}
			uint8* P = Blocks[Block] + Used;
			Used += Size;
			FMemory::Memzero(P, Size);
			return P;
		}
	};
	FUSVMStack GStack;

	/** Typed temporary (zeroed, destroyed with its property when known). */
	struct FTemp
	{
		alignas(16) uint8 Buffer[TempBufferSize];
		FUSExprType Type;
		FTemp() { FMemory::Memzero(Buffer, sizeof(Buffer)); }
		~FTemp() { Destroy(); }
		void Destroy()
		{
			if (Type.Prop)
			{
				Type.Prop->DestroySingle(Buffer);
			}
			else if (Type.Type == EUSType::Str)
			{
				reinterpret_cast<FString*>(Buffer)->~FString();
				new (Buffer) FString();
			}
		}
	};

	TSet<FString> GWarned;

	void ZeroResult(void* Result, const FUSExprType& T)
	{
		if (!Result || !T.IsKnown())
		{
			return;
		}
		if (T.Prop)
		{
			T.Prop->DestroySingle(Result);
			FMemory::Memzero(Result, T.Prop->ElementSize);
		}
		else if (T.Type == EUSType::Str)
		{
			reinterpret_cast<FString*>(Result)->Empty();
		}
		else
		{
			FMemory::Memzero(Result, T.Size());
		}
	}

	FORCEINLINE FUSObject* LiveObject(FUSObject* O)
	{
		return (O && !O->IsPendingKill()) ? O : nullptr;
	}

	bool CheckRunaway(FUSFrame& Stack)
	{
		if (++GRunaway > MaxRunaway)
		{
			USVM::ScriptError(&Stack, TEXT("Runaway loop detected (over 10000000 iterations)"));
			return false;
		}
		return true;
	}

	/** Type cache for Context-on-None and Switch (by code address). */
	TMap<const uint8*, FUSExprType> GTypeCache;
}

// =============================================================================================
// Frame helpers
// =============================================================================================

FName FUSFrame::ReadName()
{
	return GRT->GetName(ReadInt());
}

FUSObject* FUSFrame::ReadObject()
{
	return GRT->ResolveRef(ReadInt());
}

int32 FUSFrame::CodeOffset() const
{
	if (!Node || !Code || Node->Script.Num() == 0)
	{
		return -1;
	}
	return int32(Code - Node->Script.GetData());
}

FString FUSFrame::Describe() const
{
	FString Where = Node ? Node->GetPathName() : FString(TEXT("?"));
	return FString::Printf(TEXT("%s:%04X (%s)"), *Where, FMath::Max(0, CodeOffset()), Object ? *Object->GetName() : TEXT("None"));
}

void FUSFrame::Step(FUSObject* Context, void* Result)
{
	const uint8 B = *Code++;
	++USVM::GStatTokens;
	if (FUSNativeFunc F = USVM::GNatives[B])
	{
		F(Context, *this, Result);
		return;
	}
	extern void USCallMissingNativeByIndex(int32 Index, FUSObject* Self, FUSFrame& Stack, void* Result);
	if (B >= USToken::FirstNative)
	{
		USCallMissingNativeByIndex(B, Context, *this, Result);
		return;
	}
	USVM::ScriptError(this, FString::Printf(TEXT("Unknown token 0x%02X"), B));
}

void USVM::ScriptWarn(const FUSFrame* Stack, const FString& Message)
{
	const FString Where = Stack ? Stack->Describe() : FString();
	const FString Key = Where + Message;
	if (GWarned.Num() < 4096 && !GWarned.Contains(Key))
	{
		GWarned.Add(Key);
		UE_LOG(LogUScript, Warning, TEXT("%s: %s"), *Where, *Message);
	}
}

void USVM::ScriptError(const FUSFrame* Stack, const FString& Message)
{
	UE_LOG(LogUScript, Error, TEXT("%s: %s (script call aborted)"), Stack ? *Stack->Describe() : TEXT("?"), *Message);
	GAbort = true;
}

// =============================================================================================
// Missing natives
// =============================================================================================

static void USSkipParmsGeneric(FUSFrame& Stack, FUSFunction* Decl)
{
	int32 i = 0;
	while (*Stack.Code != USToken::EndFunctionParms && !USVM::GAbort)
	{
		FTemp T;
		if (Decl && Decl->Parms.IsValidIndex(i))
		{
			T.Type.Prop = Decl->Parms[i];
			T.Type.Type = Decl->Parms[i]->Type;
		}
		else
		{
			const uint8* Peek = Stack.Code;
			T.Type = USVM::InferExpr(Stack.Node, Peek, Stack.Object ? Stack.Object->Class : nullptr);
		}
		Stack.Step(Stack.Object, T.Buffer);
		++i;
	}
	Stack.Code++;
}

static void USCallMissingNative(FUSFunction* Decl, FUSObject* Self, FUSFrame& Stack, void* Result)
{
	if (Decl && Stack.Node == Decl)
	{
		Stack.Code = nullptr; // called from C++ on its own NativeParm script: nothing to skip
	}
	else
	{
		USSkipParmsGeneric(Stack, Decl);
	}
	if (Decl)
	{
		USVM::ScriptWarn(&Stack, FString::Printf(TEXT("native function %s.%s is not implemented"),
			Decl->GetOwnerClass() ? *Decl->GetOwnerClass()->GetName() : TEXT("?"), *Decl->GetName()));
		if (Result && Decl->ReturnProperty)
		{
			Decl->ReturnProperty->DestroySingle(Result);
			FMemory::Memzero(Result, Decl->ReturnProperty->ElementSize);
		}
	}
}

void USCallMissingNativeByIndex(int32 Index, FUSObject* Self, FUSFrame& Stack, void* Result)
{
	FUSFunction* Decl = (Index >= 0 && Index < 4096) ? GRT->NativeDecls[Index] : nullptr;
	if (!Decl)
	{
		USVM::ScriptWarn(&Stack, FString::Printf(TEXT("native %d is not declared by any loaded class"), Index));
	}
	USCallMissingNative(Decl, Self, Stack, Result);
}

// =============================================================================================
// Token handlers
// =============================================================================================

namespace USExec
{
	using namespace USVM;

	US_NATIVE(execLocalVariable)
	{
		FUSProperty* P = static_cast<FUSProperty*>(Stack.ReadObject());
		GProperty = P;
		GPropAddr = (P && Stack.Locals) ? Stack.Locals + P->Offset : nullptr;
		if (Result && GPropAddr)
		{
			P->CopyComplete(Result, GPropAddr);
		}
	}

	US_NATIVE(execInstanceVariable)
	{
		FUSProperty* P = static_cast<FUSProperty*>(Stack.ReadObject());
		GProperty = P;
		if (!P || !Self || !Self->Data || !Self->Class || P->Offset + P->GetSize() > Self->Class->PropertiesSize)
		{
			GPropAddr = nullptr;
			if (P && Self && Self->Class)
			{
				ScriptWarn(&Stack, FString::Printf(TEXT("variable %s does not exist in %s"), *P->GetName(), *Self->GetFullName()));
			}
			return;
		}
		GPropAddr = Self->Data + P->Offset;
		if (Result)
		{
			P->CopyComplete(Result, GPropAddr);
		}
	}

	US_NATIVE(execDefaultVariable)
	{
		FUSProperty* P = static_cast<FUSProperty*>(Stack.ReadObject());
		GProperty = P;
		FUSClass* C = Self ? Self->Class : nullptr;
		if (!P || !C || C->Defaults.Num() < P->Offset + P->GetSize())
		{
			GPropAddr = nullptr;
			return;
		}
		GPropAddr = C->Defaults.GetData() + P->Offset;
		if (Result)
		{
			P->CopyComplete(Result, GPropAddr);
		}
	}

	US_NATIVE(execNativeParm)
	{
		FUSProperty* P = static_cast<FUSProperty*>(Stack.ReadObject());
		if (P && Stack.Locals)
		{
			GProperty = P;
			GPropAddr = Stack.Locals + P->Offset;
			if (Result)
			{
				P->CopyComplete(Result, GPropAddr);
			}
		}
	}

	US_NATIVE(execReturn)
	{
		// Only reached outside ProcessInternal (malformed code): evaluate and stop.
		Stack.Step(Stack.Object, Result);
	}

	US_NATIVE(execNothing) {}

	US_NATIVE(execEndOfScript)
	{
		Stack.Code = nullptr;
		if (Self && Self->StateFrame && Self->StateFrame->ActiveFrame == &Stack)
		{
			Self->StateFrame->Code = nullptr;
		}
	}

	US_NATIVE(execJump)
	{
		const uint16 Off = Stack.ReadWord();
		if (Stack.Node->Script.GetData() + Off < Stack.Code && !CheckRunaway(Stack))
		{
			return;
		}
		Stack.Code = Stack.Node->Script.GetData() + Off;
	}

	US_NATIVE(execJumpIfNot)
	{
		const uint16 Off = Stack.ReadWord();
		uint32 Value = 0;
		Stack.Step(Stack.Object, &Value);
		if (!Value)
		{
			if (Stack.Node->Script.GetData() + Off < Stack.Code && !CheckRunaway(Stack))
			{
				return;
			}
			Stack.Code = Stack.Node->Script.GetData() + Off;
		}
	}

	US_NATIVE(execStop)
	{
		Stack.Code = nullptr;
		if (Self && Self->StateFrame)
		{
			Self->StateFrame->Code = nullptr;
		}
	}

	US_NATIVE(execAssert)
	{
		const uint16 Line = Stack.ReadWord();
		uint32 Value = 0;
		Stack.Step(Stack.Object, &Value);
		if (!Value)
		{
			ScriptWarn(&Stack, FString::Printf(TEXT("Assertion failed, line %d"), Line));
		}
	}

	US_NATIVE(execCase)
	{
		// fall through into the next case: skip its value
		const uint16 Next = Stack.ReadWord();
		if (Next != 0xFFFF)
		{
			SkipExpr(Stack.Code);
		}
	}

	US_NATIVE(execLabelTable)
	{
		for (int32 G = 0; G < 4096; ++G)
		{
			const FName N = Stack.ReadName();
			Stack.ReadInt();
			if (N == NAME_None)
			{
				break;
			}
		}
	}

	US_NATIVE(execGotoLabel)
	{
		USP_GET_NAME(Label);
		if (!USGotoLabel(Self, Label))
		{
			ScriptWarn(&Stack, FString::Printf(TEXT("GotoLabel (%s): label not found"), *Label.ToString()));
		}
	}

	US_NATIVE(execEatString)
	{
		FString Tmp;
		Stack.Step(Stack.Object, &Tmp);
	}

	US_NATIVE(execLet)
	{
		GPropAddr = nullptr;
		GProperty = nullptr;
		GArrayLengthSet = false;
		Stack.Step(Stack.Object, nullptr);
		uint8* Addr = GPropAddr;
		FUSProperty* Prop = GProperty;
		if (GArrayLengthSet)
		{
			GArrayLengthSet = false;
			int32 NewSize = 0;
			Stack.Step(Stack.Object, &NewSize);
			FUSScriptArray* A = reinterpret_cast<FUSScriptArray*>(Addr);
			if (A && Prop && Prop->Type == EUSType::Array)
			{
				FUSProperty* Inner = Prop->Inner;
				const int32 ES = Inner->ElementSize;
				NewSize = FMath::Clamp(NewSize, 0, 1 << 24);
				if (NewSize > A->Num)
				{
					if (NewSize > A->Max)
					{
						A->Data = static_cast<uint8*>(FMemory::Realloc(A->Data, int64(NewSize) * ES, 16));
						A->Max = NewSize;
					}
					FMemory::Memzero(A->Data + int64(A->Num) * ES, int64(NewSize - A->Num) * ES);
					A->Num = NewSize;
				}
				else if (NewSize < A->Num)
				{
					if (Inner->NeedsDestruct())
					{
						for (int32 i = NewSize; i < A->Num; ++i)
						{
							Inner->DestroySingle(A->Data + int64(i) * ES);
						}
					}
					A->Num = NewSize;
				}
			}
			return;
		}
		if (!Addr)
		{
			ScriptWarn(&Stack, TEXT("Attempt to assign variable through None"));
			FTemp T;
			const uint8* Peek = Stack.Code;
			T.Type = InferExpr(Stack.Node, Peek, Self ? Self->Class : nullptr);
			Stack.Step(Stack.Object, T.Buffer);
			return;
		}
		Stack.Step(Stack.Object, Addr);
	}

	US_NATIVE(execLetBool)
	{
		GPropAddr = nullptr;
		Stack.Step(Stack.Object, nullptr);
		uint32* BoolAddr = reinterpret_cast<uint32*>(GPropAddr);
		uint32 NewValue = 0;
		Stack.Step(Stack.Object, &NewValue);
		if (BoolAddr)
		{
			*BoolAddr = NewValue ? 1u : 0u;
		}
	}

	US_NATIVE(execLetDelegate)
	{
		GPropAddr = nullptr;
		Stack.Step(Stack.Object, nullptr);
		FUSDelegate* Addr = reinterpret_cast<FUSDelegate*>(GPropAddr);
		FUSDelegate D;
		Stack.Step(Stack.Object, &D);
		if (Addr)
		{
			*Addr = D;
		}
	}

	US_NATIVE(execDelegateProperty)
	{
		const FName F = Stack.ReadName();
		if (Result)
		{
			FUSDelegate* D = static_cast<FUSDelegate*>(Result);
			D->FunctionName = F;
			D->Object = F == NAME_None ? nullptr : Self;
		}
	}

	US_NATIVE(execDelegateFunction)
	{
		FUSProperty* P = static_cast<FUSProperty*>(Stack.ReadObject());
		const FName DefaultName = Stack.ReadName();
		FUSDelegate* D = (P && Self && Self->Data && P->Offset + int32(sizeof(FUSDelegate)) <= Self->Class->PropertiesSize)
			? reinterpret_cast<FUSDelegate*>(Self->Data + P->Offset) : nullptr;
		if (D && LiveObject(D->Object) && D->FunctionName != NAME_None)
		{
			if (FUSFunction* F = FindFunction(D->Object, D->FunctionName))
			{
				CallFunction(D->Object, Stack, Result, F);
				return;
			}
		}
		FUSFunction* F = Self ? FindFunction(Self, DefaultName) : nullptr;
		if (F)
		{
			CallFunction(Self, Stack, Result, F);
		}
		else
		{
			USSkipParmsGeneric(Stack, nullptr);
		}
	}

	US_NATIVE(execArrayElement)
	{
		USP_GET_INT(Index);
		GProperty = nullptr;
		GPropAddr = nullptr;
		Stack.Step(Self, nullptr);
		if (GPropAddr && GProperty)
		{
			if (Index >= GProperty->ArrayDim || Index < 0)
			{
				ScriptWarn(&Stack, FString::Printf(TEXT("Accessed array '%s' out of bounds (%d/%d)"), *GProperty->GetName(), Index, GProperty->ArrayDim));
				Index = FMath::Clamp(Index, 0, GProperty->ArrayDim - 1);
			}
			GPropAddr += Index * GProperty->ElementSize;
			if (Result)
			{
				GProperty->CopySingle(Result, GPropAddr);
			}
		}
	}

	US_NATIVE(execDynArrayElement)
	{
		USP_GET_INT(Index);
		GProperty = nullptr;
		GPropAddr = nullptr;
		Stack.Step(Self, nullptr);
		if (!GProperty || !GPropAddr || GProperty->Type != EUSType::Array)
		{
			GPropAddr = nullptr;
			return;
		}
		FUSProperty* ArrayProp = GProperty;
		FUSProperty* Inner = ArrayProp->Inner;
		FUSScriptArray* A = reinterpret_cast<FUSScriptArray*>(GPropAddr);
		const int32 ES = Inner->ElementSize;
		if (Index >= A->Num || Index < 0)
		{
			if (Result || Index < 0 || Index > (1 << 24))
			{
				ScriptWarn(&Stack, FString::Printf(TEXT("Accessed array '%s' out of bounds (%d/%d)"), *ArrayProp->GetName(), Index, A->Num));
				GPropAddr = nullptr;
				return;
			}
			// assignment past the end grows the array
			const int32 NewNum = Index + 1;
			if (NewNum > A->Max)
			{
				A->Data = static_cast<uint8*>(FMemory::Realloc(A->Data, int64(NewNum) * ES, 16));
				A->Max = NewNum;
			}
			FMemory::Memzero(A->Data + int64(A->Num) * ES, int64(NewNum - A->Num) * ES);
			A->Num = NewNum;
		}
		GPropAddr = A->Data + int64(Index) * ES;
		GProperty = Inner;
		if (Result)
		{
			Inner->CopySingle(Result, GPropAddr);
		}
	}

	US_NATIVE(execDynArrayLength)
	{
		GProperty = nullptr;
		GPropAddr = nullptr;
		Stack.Step(Self, nullptr);
		FUSScriptArray* A = reinterpret_cast<FUSScriptArray*>(GPropAddr);
		if (Result)
		{
			*static_cast<int32*>(Result) = A ? A->Num : 0;
		}
		else
		{
			GArrayLengthSet = true;
		}
	}

	US_NATIVE(execDynArrayInsert)
	{
		GProperty = nullptr;
		GPropAddr = nullptr;
		Stack.Step(Self, nullptr);
		FUSScriptArray* A = reinterpret_cast<FUSScriptArray*>(GPropAddr);
		FUSProperty* Prop = GProperty;
		USP_GET_INT(Index);
		USP_GET_INT(Count);
		if (!A || !Prop || Prop->Type != EUSType::Array || Count == 0)
		{
			return;
		}
		if (Count < 0 || Index < 0 || Index > A->Num || Count > (1 << 24))
		{
			ScriptWarn(&Stack, FString::Printf(TEXT("Attempt to insert %d items at %d in an array of %d"), Count, Index, A->Num));
			return;
		}
		const int32 ES = Prop->Inner->ElementSize;
		const int32 NewNum = A->Num + Count;
		if (NewNum > A->Max)
		{
			A->Data = static_cast<uint8*>(FMemory::Realloc(A->Data, int64(NewNum) * ES, 16));
			A->Max = NewNum;
		}
		FMemory::Memmove(A->Data + int64(Index + Count) * ES, A->Data + int64(Index) * ES, int64(A->Num - Index) * ES);
		FMemory::Memzero(A->Data + int64(Index) * ES, int64(Count) * ES);
		A->Num = NewNum;
	}

	US_NATIVE(execDynArrayRemove)
	{
		GProperty = nullptr;
		GPropAddr = nullptr;
		Stack.Step(Self, nullptr);
		FUSScriptArray* A = reinterpret_cast<FUSScriptArray*>(GPropAddr);
		FUSProperty* Prop = GProperty;
		USP_GET_INT(Index);
		USP_GET_INT(Count);
		if (!A || !Prop || Prop->Type != EUSType::Array || Count == 0)
		{
			return;
		}
		if (Count < 0 || Index < 0 || Index + Count > A->Num)
		{
			ScriptWarn(&Stack, FString::Printf(TEXT("Attempt to remove %d items at %d from an array of %d"), Count, Index, A->Num));
			return;
		}
		FUSProperty* Inner = Prop->Inner;
		const int32 ES = Inner->ElementSize;
		if (Inner->NeedsDestruct())
		{
			for (int32 i = Index; i < Index + Count; ++i)
			{
				Inner->DestroySingle(A->Data + int64(i) * ES);
			}
		}
		FMemory::Memmove(A->Data + int64(Index) * ES, A->Data + int64(Index + Count) * ES, int64(A->Num - Index - Count) * ES);
		A->Num -= Count;
	}

	US_NATIVE(execNew)
	{
		USP_GET_OBJECT(Outer);
		USP_GET_STR(NameStr);
		USP_GET_INT(ObjFlags);
		USP_GET_OBJECT(ClassObj);
		FUSClass* Cls = static_cast<FUSClass*>(ClassObj);
		if (!Cls || !(Cls->Flags & USF_Field))
		{
			ScriptWarn(&Stack, TEXT("new: class is None"));
			return;
		}
		if (!Cls->EnsureLoaded() || (Cls->ClassFlags & USFlags::CLASS_Abstract))
		{
			ScriptWarn(&Stack, FString::Printf(TEXT("new: cannot create %s"), *Cls->GetName()));
			return;
		}
		if (GRT->ActorClass && Cls->IsChildOf(GRT->ActorClass))
		{
			ScriptWarn(&Stack, FString::Printf(TEXT("new: %s is an actor (use Spawn)"), *Cls->GetName()));
			return;
		}
		FUSWorld* W = (Self && Self->World) ? Self->World : &FUSWorld::Transient();
		FUSObject* Obj = W->ConstructObject(Cls, Outer ? Outer : Self, NameStr.IsEmpty() ? NAME_None : FName(*NameStr));
		if (Result)
		{
			*static_cast<FUSObject**>(Result) = Obj;
		}
		if (FUSFunction* Created = FindFunction(Obj, TEXT("Created")))
		{
			ProcessEvent(Obj, Created, nullptr);
		}
	}

	static void ContextSkip(FUSObject* Self, FUSFrame& Stack, void* Result, FUSClass* StaticClass)
	{
		ScriptWarn(&Stack, TEXT("Accessed None"));
		const uint16 Skip = Stack.ReadWord();
		Stack.Code++; // UE2 result size (our layout differs: the type is inferred instead)
		const uint8* Inner = Stack.Code;
		Stack.Code += Skip;
		GPropAddr = nullptr;
		GProperty = nullptr;
		if (!Result)
		{
			return;
		}
		FUSExprType* Cached = GTypeCache.Find(Inner);
		FUSExprType T;
		if (Cached)
		{
			T = *Cached;
		}
		else
		{
			const uint8* P = Inner;
			T = InferExpr(Stack.Node, P, StaticClass);
			GTypeCache.Add(Inner, T);
		}
		ZeroResult(Result, T);
	}

	US_NATIVE(execContext)
	{
		FUSObject* NewContext = nullptr;
		GProperty = nullptr;
		Stack.Step(Self, &NewContext);
		FUSProperty* CtxProp = GProperty;
		if (LiveObject(NewContext))
		{
			Stack.Code += 3;
			Stack.Step(NewContext, Result);
		}
		else
		{
			FUSClass* StaticClass = (CtxProp && CtxProp->Type == EUSType::Object) ? CtxProp->PropertyClass : nullptr;
			ContextSkip(Self, Stack, Result, StaticClass);
		}
	}

	US_NATIVE(execClassContext)
	{
		FUSObject* ClassObj = nullptr;
		Stack.Step(Self, &ClassObj);
		FUSClass* C = (ClassObj && (ClassObj->Flags & USF_Field)) ? static_cast<FUSClass*>(ClassObj) : nullptr;
		FUSObject* DO = C ? C->GetDefaultObject() : nullptr;
		if (DO)
		{
			Stack.Code += 3;
			Stack.Step(DO, Result);
		}
		else
		{
			ContextSkip(Self, Stack, Result, nullptr);
		}
	}

	US_NATIVE(execMetaCast)
	{
		FUSClass* MetaClass = static_cast<FUSClass*>(Stack.ReadObject());
		FUSObject* Castee = nullptr;
		Stack.Step(Stack.Object, &Castee);
		FUSObject* Out = nullptr;
		if (Castee && (Castee->Flags & USF_Field) && Castee->Class && Castee->Class->Name == TEXT("Class"))
		{
			FUSClass* C = static_cast<FUSClass*>(Castee);
			if (C->IsChildOf(MetaClass))
			{
				Out = C;
			}
		}
		if (Result)
		{
			*static_cast<FUSObject**>(Result) = Out;
		}
	}

	US_NATIVE(execDynamicCast)
	{
		FUSClass* C = static_cast<FUSClass*>(Stack.ReadObject());
		FUSObject* Castee = nullptr;
		Stack.Step(Stack.Object, &Castee);
		if (Result)
		{
			*static_cast<FUSObject**>(Result) = (LiveObject(Castee) && Castee->IsA(C)) ? Castee : nullptr;
		}
	}

	US_NATIVE(execSelf)
	{
		if (Result)
		{
			*static_cast<FUSObject**>(Result) = Self;
		}
	}

	US_NATIVE(execSkip)
	{
		Stack.ReadWord();
		Stack.Step(Stack.Object, Result);
	}

	US_NATIVE(execVirtualFunction)
	{
		const FName N = Stack.ReadName();
		FUSFunction* F = FindFunction(Self, N);
		if (!F)
		{
			ScriptWarn(&Stack, FString::Printf(TEXT("function %s not found in %s"), *N.ToString(), Self ? *Self->GetFullName() : TEXT("None")));
			USSkipParmsGeneric(Stack, nullptr);
			return;
		}
		CallFunction(Self, Stack, Result, F);
	}

	US_NATIVE(execFinalFunction)
	{
		FUSFunction* F = static_cast<FUSFunction*>(Stack.ReadObject());
		if (!F)
		{
			ScriptWarn(&Stack, TEXT("final function could not be resolved"));
			USSkipParmsGeneric(Stack, nullptr);
			return;
		}
		CallFunction(Self, Stack, Result, F);
	}

	US_NATIVE(execGlobalFunction)
	{
		const FName N = Stack.ReadName();
		FUSFunction* F = FindFunction(Self, N, true);
		if (!F)
		{
			ScriptWarn(&Stack, FString::Printf(TEXT("global function %s not found"), *N.ToString()));
			USSkipParmsGeneric(Stack, nullptr);
			return;
		}
		CallFunction(Self, Stack, Result, F);
	}

	US_NATIVE(execIntConst) { const int32 V = Stack.ReadInt(); if (Result) { *static_cast<int32*>(Result) = V; } }
	US_NATIVE(execFloatConst) { const float V = Stack.ReadFloat(); if (Result) { *static_cast<float*>(Result) = V; } }
	US_NATIVE(execStringConst)
	{
		FString S;
		while (*Stack.Code)
		{
			S.AppendChar(TCHAR(*Stack.Code));
			Stack.Code++;
		}
		Stack.Code++;
		if (Result)
		{
			*static_cast<FString*>(Result) = MoveTemp(S);
		}
	}
	US_NATIVE(execUnicodeStringConst)
	{
		FString S;
		for (;;)
		{
			const uint16 C = Stack.ReadWord();
			if (C == 0)
			{
				break;
			}
			S.AppendChar(TCHAR(C));
		}
		if (Result)
		{
			*static_cast<FString*>(Result) = MoveTemp(S);
		}
	}
	US_NATIVE(execObjectConst) { FUSObject* O = Stack.ReadObject(); if (Result) { *static_cast<FUSObject**>(Result) = O; } }
	US_NATIVE(execNameConst) { const FName N = Stack.ReadName(); if (Result) { *static_cast<FName*>(Result) = N; } }
	US_NATIVE(execRotationConst)
	{
		FUSRotator R;
		R.Pitch = Stack.ReadInt();
		R.Yaw = Stack.ReadInt();
		R.Roll = Stack.ReadInt();
		if (Result)
		{
			*static_cast<FUSRotator*>(Result) = R;
		}
	}
	US_NATIVE(execVectorConst)
	{
		FUSVector V;
		V.X = Stack.ReadFloat();
		V.Y = Stack.ReadFloat();
		V.Z = Stack.ReadFloat();
		if (Result)
		{
			*static_cast<FUSVector*>(Result) = V;
		}
	}
	US_NATIVE(execByteConst) { const uint8 B = *Stack.Code++; if (Result) { *static_cast<uint8*>(Result) = B; } }
	US_NATIVE(execIntConstByte) { const uint8 B = *Stack.Code++; if (Result) { *static_cast<int32*>(Result) = B; } }
	US_NATIVE(execIntZero) { if (Result) { *static_cast<int32*>(Result) = 0; } }
	US_NATIVE(execIntOne) { if (Result) { *static_cast<int32*>(Result) = 1; } }
	US_NATIVE(execTrue) { if (Result) { *static_cast<uint32*>(Result) = 1; } }
	US_NATIVE(execFalse) { if (Result) { *static_cast<uint32*>(Result) = 0; } }
	US_NATIVE(execNoObject) { if (Result) { *static_cast<FUSObject**>(Result) = nullptr; } }

	US_NATIVE(execBoolVariable)
	{
		GPropAddr = nullptr;
		Stack.Step(Self, nullptr);
		if (Result)
		{
			*static_cast<uint32*>(Result) = (GPropAddr && *reinterpret_cast<uint32*>(GPropAddr)) ? 1u : 0u;
		}
	}

	US_NATIVE(execIterator)
	{
		const int32 Before = GIteratorRuns;
		Stack.Step(Stack.Object, Result);
		if (GIteratorRuns == Before && !GAbort)
		{
			// The iterator did not run (missing native / None context): skip the loop body.
			const uint16 End = Stack.ReadWord();
			Stack.Code = Stack.Node->Script.GetData() + End + 1;
		}
	}

	US_NATIVE(execIteratorPop) {}
	US_NATIVE(execIteratorNext) {}

	uint32 GDummyBool = 0;
	static bool StructCompare(FUSFrame& Stack)
	{
		FUSStruct* S = static_cast<FUSStruct*>(Stack.ReadObject());
		const FUSVMStack::FMark M = GStack.Mark();
		uint8* B1 = GStack.Alloc(S ? S->PropertiesSize : 16);
		uint8* B2 = GStack.Alloc(S ? S->PropertiesSize : 16);
		Stack.Step(Stack.Object, B1);
		Stack.Step(Stack.Object, B2);
		const bool bEq = S ? S->IdenticalValue(B1, B2) : false;
		if (S)
		{
			S->DestroyValue(B1);
			S->DestroyValue(B2);
		}
		GStack.Pop(M);
		return bEq;
	}

	US_NATIVE(execStructCmpEq)
	{
		*static_cast<uint32*>(Result ? Result : &GDummyBool) = StructCompare(Stack) ? 1u : 0u;
	}

	US_NATIVE(execStructCmpNe)
	{
		*static_cast<uint32*>(Result ? Result : &GDummyBool) = StructCompare(Stack) ? 0u : 1u;
	}

	US_NATIVE(execStructMember)
	{
		FUSProperty* P = static_cast<FUSProperty*>(Stack.ReadObject());
		FUSStruct* S = (P && P->Outer && (P->Outer->Flags & USF_Field)) ? static_cast<FUSStruct*>(P->Outer) : nullptr;
		const int32 Size = S ? FMath::Max(S->PropertiesSize, 16) : TempBufferSize;
		const FUSVMStack::FMark Mark = GStack.Mark();
		uint8* Buffer = GStack.Alloc(Size);
		GPropAddr = nullptr;
		Stack.Step(Self, Buffer);
		if (GPropAddr && P)
		{
			GPropAddr += P->Offset;
		}
		GProperty = P;
		if (Result && P)
		{
			P->CopyComplete(Result, Buffer + P->Offset);
		}
		if (S)
		{
			S->DestroyValue(Buffer);
		}
		GStack.Pop(Mark);
	}

	US_NATIVE(execSwitch)
	{
		Stack.Code++; // UE2 size byte
		const uint8* ExprCode = Stack.Code;
		FUSExprType T;
		if (FUSExprType* Cached = GTypeCache.Find(ExprCode))
		{
			T = *Cached;
		}
		else
		{
			const uint8* P = ExprCode;
			T = InferExpr(Stack.Node, P, Self ? Self->Class : nullptr);
			GTypeCache.Add(ExprCode, T);
		}
		FTemp SwitchValue;
		SwitchValue.Type = T;
		Stack.Step(Stack.Object, SwitchValue.Buffer);
		for (int32 Guard = 0; Guard < 100000 && !GAbort; ++Guard)
		{
			Stack.Code++; // EX_Case
			const uint16 Next = Stack.ReadWord();
			if (Next == 0xFFFF)
			{
				break;
			}
			FTemp CaseValue;
			CaseValue.Type = T;
			Stack.Step(Stack.Object, CaseValue.Buffer);
			bool bEqual;
			if (T.Prop)
			{
				bEqual = T.Prop->IdenticalSingle(SwitchValue.Buffer, CaseValue.Buffer);
			}
			else
			{
				switch (T.Type)
				{
				case EUSType::Str: bEqual = reinterpret_cast<FString*>(SwitchValue.Buffer)->Equals(*reinterpret_cast<FString*>(CaseValue.Buffer), ESearchCase::CaseSensitive); break;
				case EUSType::Name: bEqual = *reinterpret_cast<FName*>(SwitchValue.Buffer) == *reinterpret_cast<FName*>(CaseValue.Buffer); break;
				case EUSType::Byte: bEqual = SwitchValue.Buffer[0] == CaseValue.Buffer[0]; break;
				case EUSType::Object: case EUSType::Class: case EUSType::Pointer:
					bEqual = *reinterpret_cast<void**>(SwitchValue.Buffer) == *reinterpret_cast<void**>(CaseValue.Buffer); break;
				default: bEqual = FMemory::Memcmp(SwitchValue.Buffer, CaseValue.Buffer, FMath::Max(1, T.Size())) == 0; break;
				}
			}
			if (bEqual)
			{
				break;
			}
			Stack.Code = Stack.Node->Script.GetData() + Next;
		}
	}

	US_NATIVE(execExtendedNative)
	{
		const int32 Index = ((Stack.Code[-1] - USToken::ExtendedNative) << 8) | *Stack.Code;
		Stack.Code++;
		if (FUSNativeFunc F = GNatives[Index])
		{
			F(Self, Stack, Result);
		}
		else
		{
			USCallMissingNativeByIndex(Index, Self, Stack, Result);
		}
	}

	// ---- primitive casts ----
	US_NATIVE(execPrimitiveCast)
	{
		const uint8 Cast = *Stack.Code++;
		switch (Cast)
		{
		case 0x39: // RotatorToVector
		{
			USP_GET_ROTATOR(R);
			const float P = R.Pitch * (PI / 32768.f), Y = R.Yaw * (PI / 32768.f);
			if (Result) { *static_cast<FUSVector*>(Result) = FUSVector(FMath::Cos(P) * FMath::Cos(Y), FMath::Cos(P) * FMath::Sin(Y), FMath::Sin(P)); }
			return;
		}
		case 0x3A: { USP_GET_BYTE(B); if (Result) { *static_cast<int32*>(Result) = B; } return; }
		case 0x3B: { USP_GET_BYTE(B); if (Result) { *static_cast<uint32*>(Result) = B ? 1u : 0u; } return; }
		case 0x3C: { USP_GET_BYTE(B); if (Result) { *static_cast<float*>(Result) = B; } return; }
		case 0x3D: { USP_GET_INT(I); if (Result) { *static_cast<uint8*>(Result) = uint8(I); } return; }
		case 0x3E: { USP_GET_INT(I); if (Result) { *static_cast<uint32*>(Result) = I ? 1u : 0u; } return; }
		case 0x3F: { USP_GET_INT(I); if (Result) { *static_cast<float*>(Result) = float(I); } return; }
		case 0x40: { USP_GET_UBOOL(B); if (Result) { *static_cast<uint8*>(Result) = B ? 1 : 0; } return; }
		case 0x41: { USP_GET_UBOOL(B); if (Result) { *static_cast<int32*>(Result) = B ? 1 : 0; } return; }
		case 0x42: { USP_GET_UBOOL(B); if (Result) { *static_cast<float*>(Result) = B ? 1.f : 0.f; } return; }
		case 0x43: { USP_GET_FLOAT(F); if (Result) { *static_cast<uint8*>(Result) = uint8(int32(F)); } return; }
		case 0x44: { USP_GET_FLOAT(F); if (Result) { *static_cast<int32*>(Result) = FMath::IsFinite(F) ? int32(FMath::Clamp<double>(F, -2147483648.0, 2147483647.0)) : 0; } return; }
		case 0x45: { USP_GET_FLOAT(F); if (Result) { *static_cast<uint32*>(Result) = F != 0.f ? 1u : 0u; } return; }
		case 0x46: case 0x47: { USP_GET_OBJECT(O); if (Result) { *static_cast<uint32*>(Result) = LiveObject(O) ? 1u : 0u; } return; }
		case 0x48: { USP_GET_NAME(N); if (Result) { *static_cast<uint32*>(Result) = N != NAME_None ? 1u : 0u; } return; }
		case 0x49: { USP_GET_STR(S); if (Result) { *static_cast<uint8*>(Result) = uint8(FCString::Atoi(*S)); } return; }
		case 0x4A: { USP_GET_STR(S); if (Result) { *static_cast<int32*>(Result) = FCString::Atoi(*S); } return; }
		case 0x4B:
		{
			USP_GET_STR(S);
			uint32 V;
			if (S.Equals(TEXT("True"), ESearchCase::IgnoreCase)) { V = 1; }
			else if (S.Equals(TEXT("False"), ESearchCase::IgnoreCase)) { V = 0; }
			else { V = FCString::Atoi(*S) != 0 ? 1u : 0u; }
			if (Result) { *static_cast<uint32*>(Result) = V; }
			return;
		}
		case 0x4C: { USP_GET_STR(S); if (Result) { *static_cast<float*>(Result) = FCString::Atof(*S); } return; }
		case 0x4D:
		{
			USP_GET_STR(S);
			TArray<FString> Parts;
			S.ParseIntoArray(Parts, TEXT(","), false);
			FUSVector V;
			if (Parts.Num() > 0) { V.X = FCString::Atof(*Parts[0]); }
			if (Parts.Num() > 1) { V.Y = FCString::Atof(*Parts[1]); }
			if (Parts.Num() > 2) { V.Z = FCString::Atof(*Parts[2]); }
			if (Result) { *static_cast<FUSVector*>(Result) = V; }
			return;
		}
		case 0x4E:
		{
			USP_GET_STR(S);
			TArray<FString> Parts;
			S.ParseIntoArray(Parts, TEXT(","), false);
			FUSRotator R;
			if (Parts.Num() > 0) { R.Pitch = FCString::Atoi(*Parts[0]); }
			if (Parts.Num() > 1) { R.Yaw = FCString::Atoi(*Parts[1]); }
			if (Parts.Num() > 2) { R.Roll = FCString::Atoi(*Parts[2]); }
			if (Result) { *static_cast<FUSRotator*>(Result) = R; }
			return;
		}
		case 0x4F: { USP_GET_VECTOR(V); if (Result) { *static_cast<uint32*>(Result) = (V.X != 0.f || V.Y != 0.f || V.Z != 0.f) ? 1u : 0u; } return; }
		case 0x50:
		{
			USP_GET_VECTOR(V);
			FUSRotator R;
			R.Yaw = int32(FMath::Atan2(V.Y, V.X) * 32768.f / PI) & 0xFFFF;
			R.Pitch = int32(FMath::Atan2(V.Z, FMath::Sqrt(V.X * V.X + V.Y * V.Y)) * 32768.f / PI) & 0xFFFF;
			R.Roll = 0;
			if (Result) { *static_cast<FUSRotator*>(Result) = R; }
			return;
		}
		case 0x51: { USP_GET_ROTATOR(R); if (Result) { *static_cast<uint32*>(Result) = (R.Pitch || R.Yaw || R.Roll) ? 1u : 0u; } return; }
		case 0x52: { USP_GET_BYTE(B); if (Result) { *static_cast<FString*>(Result) = FString::FromInt(B); } return; }
		case 0x53: { USP_GET_INT(I); if (Result) { *static_cast<FString*>(Result) = FString::FromInt(I); } return; }
		case 0x54: { USP_GET_UBOOL(B); if (Result) { *static_cast<FString*>(Result) = B ? TEXT("True") : TEXT("False"); } return; }
		case 0x55: { USP_GET_FLOAT(F); if (Result) { *static_cast<FString*>(Result) = FString::Printf(TEXT("%.2f"), F); } return; }
		case 0x56: { USP_GET_OBJECT(O); if (Result) { *static_cast<FString*>(Result) = O ? O->GetPathName() : FString(TEXT("None")); } return; }
		case 0x57: { USP_GET_NAME(N); if (Result) { *static_cast<FString*>(Result) = N.ToString(); } return; }
		case 0x58: { USP_GET_VECTOR(V); if (Result) { *static_cast<FString*>(Result) = FString::Printf(TEXT("%.2f,%.2f,%.2f"), V.X, V.Y, V.Z); } return; }
		case 0x59: { USP_GET_ROTATOR(R); if (Result) { *static_cast<FString*>(Result) = FString::Printf(TEXT("%d,%d,%d"), R.Pitch, R.Yaw, R.Roll); } return; }
		default:
			ScriptError(&Stack, FString::Printf(TEXT("unknown cast 0x%02X"), Cast));
			return;
		}
	}
}

// =============================================================================================
// Calls
// =============================================================================================

FUSFunction* USVM::FindFunction(const FUSObject* Self, FName Name, bool bGlobal)
{
	if (!Self || !Self->Class)
	{
		return nullptr;
	}
	if (!bGlobal && Self->StateFrame && Self->StateFrame->StateNode && Self->StateFrame->StateNode != Self->Class)
	{
		if (FUSFunction* const* F = Self->StateFrame->StateNode->FuncMap.Find(Name))
		{
			return *F;
		}
	}
	return Self->Class->FindFunction(Name);
}

bool USVM::IsProbing(const FUSObject* Self, FName FuncName)
{
	if (!Self->StateFrame || Self->StateFrame->DisabledProbes.Num() == 0)
	{
		return true;
	}
	return !Self->StateFrame->DisabledProbes.Contains(FuncName);
}

static void USProcessInternal(FUSObject* Self, FUSFrame& Stack, void* Result)
{
	FUSFunction* F = static_cast<FUSFunction*>(Stack.Node);
	const bool bSingular = (F->FunctionFlags & USFlags::FUNC_Singular) != 0;
	if (bSingular && (Self->Flags & USF_InSingular))
	{
		return;
	}
	if (!USVM::IsProbing(Self, F->Name))
	{
		return;
	}
	if (bSingular)
	{
		Self->Flags |= USF_InSingular;
	}
	alignas(16) uint8 Buffer[TempBufferSize];
	const uint8* End = F->Script.GetData() + F->Script.Num();
	while (!USVM::GAbort && Stack.Code && Stack.Code < End && *Stack.Code != USToken::Return)
	{
		FMemory::Memzero(Buffer, 64);
		Stack.Step(Self, Buffer);
	}
	if (!USVM::GAbort && Stack.Code && Stack.Code < End)
	{
		Stack.Code++;
		Stack.Step(Self, Result);
	}
	if (bSingular)
	{
		Self->Flags &= ~USF_InSingular;
	}
}

void USVM::CallFunction(FUSObject* Self, FUSFrame& Stack, void* Result, FUSFunction* Function)
{
	++GStatCalls;
	if (Function->FunctionFlags & USFlags::FUNC_Native)
	{
		if (Function->Func)
		{
			Function->Func(Self, Stack, Result);
		}
		else
		{
			USCallMissingNative(Function, Self, Stack, Result);
		}
		return;
	}
	if (!Self || !Self->Data)
	{
		// non-script objects (materials, textures ... referenced by script) have no script state: calls do nothing
		USSkipParmsGeneric(Stack, Function);
		return;
	}
	if (GCallDepth >= MaxCallDepth)
	{
		ScriptError(&Stack, FString::Printf(TEXT("Infinite script recursion (%d calls) calling %s"), GCallDepth, *Function->GetName()));
		USSkipParmsGeneric(Stack, Function);
		return;
	}
	if (!CheckRunaway(Stack))
	{
		return;
	}
	const FUSVMStack::FMark Mark = GStack.Mark();
	uint8* Locals = GStack.Alloc(Function->PropertiesSize);
	if (!Locals)
	{
		ScriptError(&Stack, TEXT("function locals too large"));
		return;
	}
	TArray<FUSOutParm, TInlineAllocator<8>> Outs;
	int32 ParmIndex = 0;
	while (*Stack.Code != USToken::EndFunctionParms && !GAbort)
	{
		FUSProperty* P = Function->Parms.IsValidIndex(ParmIndex) ? Function->Parms[ParmIndex] : nullptr;
		if (!P)
		{
			// extra argument: evaluate and drop
			FTemp T;
			const uint8* Peek = Stack.Code;
			T.Type = InferExpr(Stack.Node, Peek, Stack.Object ? Stack.Object->Class : nullptr);
			Stack.Step(Stack.Object, T.Buffer);
			++ParmIndex;
			continue;
		}
		GPropAddr = nullptr;
		Stack.Step(Stack.Object, Locals + P->Offset);
		if ((P->PropertyFlags & USFlags::CPF_OutParm) && GPropAddr)
		{
			FUSOutParm O;
			O.Prop = P;
			O.Addr = GPropAddr;
			Outs.Add(O);
		}
		++ParmIndex;
	}
	Stack.Code++;
	if (!GAbort)
	{
		FUSFrame NewStack(Self, Function, Function->Script.GetData(), Locals, &Stack);
		++GCallDepth;
		USProcessInternal(Self, NewStack, Result);
		--GCallDepth;
		for (const FUSOutParm& O : Outs)
		{
			O.Prop->CopyComplete(O.Addr, Locals + O.Prop->Offset);
		}
	}
	Function->DestroyValue(Locals);
	GStack.Pop(Mark);
	GProperty = nullptr;
	GPropAddr = nullptr;
}

void USVM::ProcessEvent(FUSObject* Self, FUSFunction* Function, void* Parms)
{
	if (!Self || !Function || Self->IsPendingKill() || !IsProbing(Self, Function->Name))
	{
		return;
	}
	const bool bTop = GCallDepth == 0;
	if (bTop)
	{
		GAbort = false;
		GRunaway = 0;
	}
	if (GCallDepth >= MaxCallDepth)
	{
		UE_LOG(LogUScript, Error, TEXT("Infinite script recursion calling %s"), *Function->GetPathName());
		return;
	}
	++GStatCalls;
	uint8* ParmBytes = static_cast<uint8*>(Parms);
	void* ResultAddr = (ParmBytes && Function->ReturnProperty) ? ParmBytes + Function->ReturnValueOffset : nullptr;
	++GCallDepth;
	if (Function->FunctionFlags & USFlags::FUNC_Native)
	{
		const FUSVMStack::FMark Mark = GStack.Mark();
		uint8* Locals = ParmBytes ? ParmBytes : GStack.Alloc(Function->PropertiesSize);
		FUSFrame Stack(Self, Function, Function->Script.GetData(), Locals);
		alignas(16) uint8 Dummy[TempBufferSize] = {};
		if (Function->Func)
		{
			Function->Func(Self, Stack, ResultAddr ? ResultAddr : Dummy);
		}
		else
		{
			USCallMissingNative(Function, Self, Stack, ResultAddr ? ResultAddr : Dummy);
		}
		GStack.Pop(Mark);
	}
	else
	{
		const FUSVMStack::FMark Mark = GStack.Mark();
		uint8* Locals = GStack.Alloc(Function->PropertiesSize);
		if (ParmBytes)
		{
			for (FUSProperty* P : Function->Parms)
			{
				P->CopyComplete(Locals + P->Offset, ParmBytes + P->Offset);
			}
		}
		FUSFrame Stack(Self, Function, Function->Script.GetData(), Locals);
		alignas(16) uint8 Dummy[TempBufferSize] = {};
		USProcessInternal(Self, Stack, ResultAddr ? ResultAddr : Dummy);
		if (ParmBytes)
		{
			for (FUSProperty* P : Function->Parms)
			{
				if (P->PropertyFlags & USFlags::CPF_OutParm)
				{
					P->CopyComplete(ParmBytes + P->Offset, Locals + P->Offset);
				}
			}
		}
		Function->DestroyValue(Locals);
		GStack.Pop(Mark);
	}
	--GCallDepth;
	if (bTop)
	{
		GAbort = false;
	}
}

// =============================================================================================
// States
// =============================================================================================

bool USGotoLabel(FUSObject* Obj, FName Label)
{
	FUSStateFrame* SF = Obj ? Obj->StateFrame : nullptr;
	if (!SF)
	{
		return false;
	}
	SF->LatentAction = 0;
	if (Label != NAME_None)
	{
		int32 Guard = 0;
		for (FUSState* S = SF->StateNode; S && Guard < 64; S = static_cast<FUSState*>(S->SuperField), ++Guard)
		{
			if (!S->Class || (S->Class->Name != TEXT("State") && S->Class->Name != TEXT("Class")))
			{
				break;
			}
			if (S->LabelTableOffset == 0xFFFF || S->LabelTableOffset >= S->Script.Num())
			{
				continue;
			}
			const uint8* P = S->Script.GetData() + S->LabelTableOffset;
			if (*P == USToken::LabelTable)
			{
				++P;
			}
			const uint8* End = S->Script.GetData() + S->Script.Num();
			while (P + 8 <= End)
			{
				int32 NameIdx, CodeOff;
				FMemory::Memcpy(&NameIdx, P, 4);
				FMemory::Memcpy(&CodeOff, P + 4, 4);
				P += 8;
				const FName N = GRT->GetName(NameIdx);
				if (N == NAME_None)
				{
					break;
				}
				if (N == Label && CodeOff >= 0 && CodeOff < S->Script.Num())
				{
					SF->CodeNode = S;
					SF->Code = S->Script.GetData() + CodeOff;
					if (SF->ActiveFrame)
					{
						SF->ActiveFrame->Node = S;
						SF->ActiveFrame->Code = SF->Code;
					}
					return true;
				}
			}
		}
	}
	SF->Code = nullptr;
	if (SF->ActiveFrame)
	{
		SF->ActiveFrame->Code = nullptr;
	}
	return false;
}

bool USVM::GotoState(FUSObject* Self, FName NewState, FName Label)
{
	FUSStateFrame* SF = Self ? Self->StateFrame : nullptr;
	if (!SF)
	{
		return false;
	}
	SF->LatentAction = 0;
	const FName OldStateName = (SF->StateNode && SF->StateNode != Self->Class) ? SF->StateNode->Name : NAME_None;
	FUSState* StateNode = nullptr;
	static const FName NAME_Auto(TEXT("Auto"));
	if (NewState == NAME_Auto)
	{
		for (const TPair<FName, FUSState*>& KV : Self->Class->StateMap)
		{
			if (KV.Value->StateFlags & USFlags::STATE_Auto)
			{
				// the most derived auto state wins (StateMap holds the class's own version of each name)
				StateNode = KV.Value;
				break;
			}
		}
	}
	else if (NewState != NAME_None)
	{
		StateNode = Self->Class->FindState(NewState);
	}
	bool bFound = StateNode != nullptr;
	if (!StateNode)
	{
		NewState = NAME_None;
		StateNode = Self->Class;
	}
	else
	{
		NewState = StateNode->Name;
	}
	if (OldStateName != NAME_None && NewState != OldStateName)
	{
		if (FUSFunction* F = FindFunction(Self, TEXT("EndState")))
		{
			FUSState* Before = SF->StateNode;
			ProcessEvent(Self, F, nullptr);
			if (SF->StateNode != Before)
			{
				return true; // preempted by a GotoState in EndState
			}
		}
	}
	SF->StateNode = StateNode;
	SF->CodeNode = StateNode;
	SF->Code = nullptr;
	if (SF->ActiveFrame)
	{
		SF->ActiveFrame->Node = StateNode;
		SF->ActiveFrame->Code = nullptr;
	}
	if (NewState != NAME_None && NewState != OldStateName)
	{
		if (FUSFunction* F = FindFunction(Self, TEXT("BeginState")))
		{
			FUSState* Before = SF->StateNode;
			ProcessEvent(Self, F, nullptr);
			if (SF->StateNode != Before)
			{
				return true;
			}
		}
	}
	if (bFound)
	{
		static const FName NAME_Begin(TEXT("Begin"));
		USGotoLabel(Self, Label == NAME_None ? NAME_Begin : Label);
	}
	return bFound;
}

void USVM::ProcessState(FUSObject* Self, float DeltaSeconds)
{
	FUSStateFrame* SF = Self ? Self->StateFrame : nullptr;
	if (!SF || !SF->Code || Self->IsPendingKill())
	{
		return;
	}
	if (SF->LatentAction)
	{
		switch (SF->LatentAction)
		{
		case USLatent::Sleep:
		case USLatent::Generic:
		case USLatent::FinishAnim:
		case USLatent::FinishInterpolation:
		case USLatent::MoveTo:
			SF->LatentFloat -= DeltaSeconds;
			if (SF->LatentFloat <= 0.f)
			{
				SF->LatentAction = 0;
			}
			break;
		default:
			SF->LatentAction = 0;
			break;
		}
		if (SF->LatentAction)
		{
			return;
		}
	}
	GAbort = false;
	GRunaway = 0;
	FUSFrame Stack(Self, SF->CodeNode ? SF->CodeNode : SF->StateNode, SF->Code, nullptr);
	SF->ActiveFrame = &Stack;
	int32 NumStates = 0;
	FUSStruct* OldNode = Stack.Node;
	alignas(16) uint8 Buffer[TempBufferSize];
	while (!Self->IsPendingKill() && Stack.Code && !SF->LatentAction && !GAbort)
	{
		FMemory::Memzero(Buffer, 64);
		Stack.Step(Self, Buffer);
		SF->Code = Stack.Code;
		SF->CodeNode = Stack.Node;
		if (Stack.Node != OldNode)
		{
			OldNode = Stack.Node;
			if (++NumStates > 4)
			{
				break;
			}
		}
	}
	if (GAbort)
	{
		SF->Code = nullptr;
	}
	SF->ActiveFrame = nullptr;
	GAbort = false;
}

// =============================================================================================
// Expression type inference / skipping
// =============================================================================================

namespace
{
	FUSExprType TypeOfProp(FUSProperty* P)
	{
		FUSExprType T;
		if (P)
		{
			T.Type = P->Type;
			T.Prop = P;
		}
		return T;
	}

	FUSExprType Prim(EUSType Ty)
	{
		FUSExprType T;
		T.Type = Ty;
		return T;
	}

	FUSExprType FunctionReturn(FUSFunction* F)
	{
		return (F && F->ReturnProperty) ? TypeOfProp(F->ReturnProperty) : FUSExprType();
	}

	FUSObject* PeekObject(const uint8*& Code)
	{
		int32 I;
		FMemory::Memcpy(&I, Code, 4);
		Code += 4;
		return GRT->ResolveRef(I);
	}

	FName PeekName(const uint8*& Code)
	{
		int32 I;
		FMemory::Memcpy(&I, Code, 4);
		Code += 4;
		return GRT->GetName(I);
	}

	void SkipParms(FUSStruct* Node, const uint8*& Code, FUSClass* Ctx)
	{
		for (int32 G = 0; G < 256 && *Code != USToken::EndFunctionParms; ++G)
		{
			USVM::InferExpr(Node, Code, Ctx);
		}
		Code++;
	}

	/** ForEachCall's visitors while it walks GCallWalkNode (null otherwise: InferExpr is also used by the interpreter). */
	TFunctionRef<void(const USVM::FCallSite&)>* GCallVisit = nullptr;
	TFunctionRef<void(FUSObject*)>* GObjectVisit = nullptr;
	FUSStruct* GCallWalkNode = nullptr;

	void NoteCall(FUSStruct* Node, FUSFunction* F, FName Name, FUSClass* Ctx, int32 NativeIndex)
	{
		if (GCallVisit && Node == GCallWalkNode)
		{
			USVM::FCallSite Site;
			Site.Function = F;
			Site.Name = Name;
			Site.ContextClass = Ctx;
			Site.NativeIndex = NativeIndex;
			(*GCallVisit)(Site);
		}
	}

	FUSFunction* FindFunctionAnywhere(FName Name, FUSClass* Ctx)
	{
		if (Ctx)
		{
			if (FUSFunction* F = Ctx->FindFunction(Name))
			{
				return F;
			}
			for (const TPair<FName, FUSState*>& KV : Ctx->StateMap)
			{
				if (FUSFunction* const* F = KV.Value->FuncMap.Find(Name))
				{
					return *F;
				}
			}
		}
		return nullptr;
	}
}

FUSExprType USVM::InferExpr(FUSStruct* Node, const uint8*& Code, FUSClass* Ctx)
{
	using namespace USToken;
	const uint8 T = *Code++;
	switch (T)
	{
	case LocalVariable: case InstanceVariable: case DefaultVariable: case NativeParm:
		return TypeOfProp(static_cast<FUSProperty*>(PeekObject(Code)));
	case Return: case GotoLabel: case EatString:
		InferExpr(Node, Code, Ctx);
		return FUSExprType();
	case Switch:
		Code++;
		InferExpr(Node, Code, Ctx);
		return FUSExprType();
	case Jump: Code += 2; return FUSExprType();
	case JumpIfNot: Code += 2; InferExpr(Node, Code, Ctx); return FUSExprType();
	case Assert: Code += 2; InferExpr(Node, Code, Ctx); return FUSExprType();
	case Case:
	{
		uint16 Off;
		FMemory::Memcpy(&Off, Code, 2);
		Code += 2;
		if (Off != 0xFFFF)
		{
			InferExpr(Node, Code, Ctx);
		}
		return FUSExprType();
	}
	case LabelTable:
		for (int32 G = 0; G < 4096; ++G)
		{
			const FName N = PeekName(Code);
			Code += 4;
			if (N == NAME_None)
			{
				break;
			}
		}
		return FUSExprType();
	case Let: case LetBool: case LetDelegate:
		InferExpr(Node, Code, Ctx);
		InferExpr(Node, Code, Ctx);
		return FUSExprType();
	case DynArrayElement:
	{
		InferExpr(Node, Code, Ctx);
		FUSExprType A = InferExpr(Node, Code, Ctx);
		return (A.Prop && A.Prop->Type == EUSType::Array) ? TypeOfProp(A.Prop->Inner) : FUSExprType();
	}
	case ArrayElement:
		InferExpr(Node, Code, Ctx);
		return InferExpr(Node, Code, Ctx);
	case USToken::New:
		InferExpr(Node, Code, Ctx); InferExpr(Node, Code, Ctx); InferExpr(Node, Code, Ctx); InferExpr(Node, Code, Ctx);
		return Prim(EUSType::Object);
	case ClassContext: case Context:
	{
		FUSExprType O = InferExpr(Node, Code, Ctx);
		Code += 3;
		FUSClass* Inner = (O.Prop && (O.Prop->Type == EUSType::Object)) ? O.Prop->PropertyClass
			: (O.Prop && O.Prop->Type == EUSType::Class) ? O.Prop->MetaClass : nullptr;
		return InferExpr(Node, Code, Inner ? Inner : Ctx);
	}
	case MetaCast: PeekObject(Code); InferExpr(Node, Code, Ctx); return Prim(EUSType::Class);
	case DynamicCast: PeekObject(Code); InferExpr(Node, Code, Ctx); return Prim(EUSType::Object);
	case EndFunctionParms: case Nothing: case Stop: case IteratorPop: case IteratorNext: case EndOfScript:
		return FUSExprType();
	case Self: case NoObject: return Prim(EUSType::Object);
	case Skip: Code += 2; return InferExpr(Node, Code, Ctx);
	case VirtualFunction: case GlobalFunction:
	{
		const FName N = PeekName(Code);
		FUSFunction* F = FindFunctionAnywhere(N, Ctx);
		NoteCall(Node, F, N, Ctx, INDEX_NONE);
		SkipParms(Node, Code, Ctx);
		return FunctionReturn(F);
	}
	case FinalFunction:
	{
		FUSFunction* F = static_cast<FUSFunction*>(PeekObject(Code));
		NoteCall(Node, F, NAME_None, Ctx, INDEX_NONE);
		SkipParms(Node, Code, Ctx);
		return FunctionReturn(F);
	}
	case IntConst: Code += 4; return Prim(EUSType::Int);
	case FloatConst: Code += 4; return Prim(EUSType::Float);
	case StringConst:
		while (*Code)
		{
			Code++;
		}
		Code++;
		return Prim(EUSType::Str);
	case UnicodeStringConst:
		while (Code[0] || Code[1])
		{
			Code += 2;
		}
		Code += 2;
		return Prim(EUSType::Str);
	case ObjectConst:
	{
		FUSObject* O = PeekObject(Code);
		if (GObjectVisit && O && Node == GCallWalkNode)
		{
			(*GObjectVisit)(O);
		}
		return Prim(EUSType::Object);
	}
	case NameConst: Code += 4; return Prim(EUSType::Name);
	case RotationConst: Code += 12; { FUSExprType R = Prim(EUSType::Struct); return R; }
	case VectorConst: Code += 12; return Prim(EUSType::Struct);
	case ByteConst: Code += 1; return Prim(EUSType::Byte);
	case IntConstByte: Code += 1; return Prim(EUSType::Int);
	case IntZero: case IntOne: return Prim(EUSType::Int);
	case True: case False: return Prim(EUSType::Bool);
	case BoolVariable: InferExpr(Node, Code, Ctx); return Prim(EUSType::Bool);
	case Iterator: InferExpr(Node, Code, Ctx); Code += 2; return FUSExprType();
	case StructCmpEq: case StructCmpNe: PeekObject(Code); InferExpr(Node, Code, Ctx); InferExpr(Node, Code, Ctx); return Prim(EUSType::Bool);
	case StructMember:
	{
		FUSProperty* P = static_cast<FUSProperty*>(PeekObject(Code));
		InferExpr(Node, Code, Ctx);
		return TypeOfProp(P);
	}
	case DynArrayLength: InferExpr(Node, Code, Ctx); return Prim(EUSType::Int);
	case PrimitiveCast:
	{
		const uint8 C = *Code++;
		InferExpr(Node, Code, Ctx);
		switch (C)
		{
		case 0x39: return Prim(EUSType::Struct);
		case 0x3A: case 0x41: case 0x44: case 0x4A: return Prim(EUSType::Int);
		case 0x3B: case 0x3E: case 0x45: case 0x46: case 0x47: case 0x48: case 0x4B: case 0x4F: case 0x51: return Prim(EUSType::Bool);
		case 0x3C: case 0x3F: case 0x42: case 0x4C: return Prim(EUSType::Float);
		case 0x3D: case 0x40: case 0x43: case 0x49: return Prim(EUSType::Byte);
		case 0x4D: case 0x4E: case 0x50: return Prim(EUSType::Struct);
		default: return Prim(EUSType::Str);
		}
	}
	case DynArrayInsert: case DynArrayRemove:
		InferExpr(Node, Code, Ctx); InferExpr(Node, Code, Ctx); InferExpr(Node, Code, Ctx);
		return FUSExprType();
	case DelegateFunction:
	{
		FUSProperty* P = static_cast<FUSProperty*>(PeekObject(Code));
		const FName N = PeekName(Code);
		NoteCall(Node, FindFunctionAnywhere(N, Ctx), N, Ctx, INDEX_NONE);
		SkipParms(Node, Code, Ctx);
		return P ? FunctionReturn(P->DelegateFunction) : FUSExprType();
	}
	case DelegateProperty: Code += 4; return Prim(EUSType::Delegate);
	default:
		break;
	}
	int32 Index = -1;
	if (T >= ExtendedNative && T < FirstNative)
	{
		Index = ((T - ExtendedNative) << 8) | *Code++;
	}
	else if (T >= FirstNative)
	{
		Index = T;
	}
	if (Index >= 0)
	{
		NoteCall(Node, GRT->NativeDecls[Index], NAME_None, Ctx, Index);
		SkipParms(Node, Code, Ctx);
		return FunctionReturn(GRT->NativeDecls[Index]);
	}
	return FUSExprType();
}

void USVM::SkipExpr(const uint8*& Code)
{
	InferExpr(nullptr, Code, nullptr);
}

void USVM::ForEachCall(FUSStruct* Node, FUSClass* SelfClass, TFunctionRef<void(const FCallSite&)> Visit, TFunctionRef<void(FUSObject*)> VisitObject)
{
	if (!Node || Node->Script.Num() == 0 || GCallVisit)
	{
		return;
	}
	GCallVisit = &Visit;
	GObjectVisit = &VisitObject;
	GCallWalkNode = Node;
	const uint8* Code = Node->Script.GetData();
	const uint8* End = Code + Node->Script.Num();
	for (int32 Guard = 0; Code < End && Guard < 100000; ++Guard)
	{
		const uint8* Start = Code;
		InferExpr(Node, Code, SelfClass);
		if (Code <= Start || Code > End)
		{
			break;
		}
	}
	GCallVisit = nullptr;
	GObjectVisit = nullptr;
	GCallWalkNode = nullptr;
}

// =============================================================================================
// Disassembler (debugging)
// =============================================================================================

FString USVM::Disassemble(FUSStruct* Node)
{
	FString Out;
	if (!Node)
	{
		return Out;
	}
	const uint8* Code = Node->Script.GetData();
	const uint8* End = Code + Node->Script.Num();
	int32 Guard = 0;
	while (Code < End && Guard++ < 100000)
	{
		const uint8* Start = Code;
		const uint8 T = *Code;
		InferExpr(Node, Code, Node->Class ? nullptr : nullptr);
		FString Hex;
		for (const uint8* P = Start; P < Code && P < Start + 24; ++P)
		{
			Hex += FString::Printf(TEXT("%02X "), *P);
		}
		Out += FString::Printf(TEXT("%04X: [%02X] %s%s\n"), int32(Start - Node->Script.GetData()), T, *Hex, (Code - Start) > 24 ? TEXT("...") : TEXT(""));
		if (Code <= Start)
		{
			break;
		}
	}
	return Out;
}

// =============================================================================================
// Init
// =============================================================================================

void USVM::Init()
{
	GRT = &FUSRuntime::Get();
	using namespace USToken;
	using namespace USExec;
	GNatives[LocalVariable] = &execLocalVariable;
	GNatives[InstanceVariable] = &execInstanceVariable;
	GNatives[DefaultVariable] = &execDefaultVariable;
	GNatives[Return] = &execReturn;
	GNatives[Switch] = &execSwitch;
	GNatives[Jump] = &execJump;
	GNatives[JumpIfNot] = &execJumpIfNot;
	GNatives[Stop] = &execStop;
	GNatives[Assert] = &execAssert;
	GNatives[Case] = &execCase;
	GNatives[Nothing] = &execNothing;
	GNatives[LabelTable] = &execLabelTable;
	GNatives[GotoLabel] = &execGotoLabel;
	GNatives[EatString] = &execEatString;
	GNatives[Let] = &execLet;
	GNatives[DynArrayElement] = &execDynArrayElement;
	GNatives[USToken::New] = &execNew;
	GNatives[ClassContext] = &execClassContext;
	GNatives[MetaCast] = &execMetaCast;
	GNatives[LetBool] = &execLetBool;
	GNatives[EndFunctionParms] = &execNothing;
	GNatives[Self] = &execSelf;
	GNatives[Skip] = &execSkip;
	GNatives[Context] = &execContext;
	GNatives[ArrayElement] = &execArrayElement;
	GNatives[VirtualFunction] = &execVirtualFunction;
	GNatives[FinalFunction] = &execFinalFunction;
	GNatives[IntConst] = &execIntConst;
	GNatives[FloatConst] = &execFloatConst;
	GNatives[StringConst] = &execStringConst;
	GNatives[ObjectConst] = &execObjectConst;
	GNatives[NameConst] = &execNameConst;
	GNatives[RotationConst] = &execRotationConst;
	GNatives[VectorConst] = &execVectorConst;
	GNatives[ByteConst] = &execByteConst;
	GNatives[IntZero] = &execIntZero;
	GNatives[IntOne] = &execIntOne;
	GNatives[True] = &execTrue;
	GNatives[False] = &execFalse;
	GNatives[NativeParm] = &execNativeParm;
	GNatives[NoObject] = &execNoObject;
	GNatives[IntConstByte] = &execIntConstByte;
	GNatives[BoolVariable] = &execBoolVariable;
	GNatives[DynamicCast] = &execDynamicCast;
	GNatives[Iterator] = &execIterator;
	GNatives[IteratorPop] = &execIteratorPop;
	GNatives[IteratorNext] = &execIteratorNext;
	GNatives[StructCmpEq] = &execStructCmpEq;
	GNatives[StructCmpNe] = &execStructCmpNe;
	GNatives[UnicodeStringConst] = &execUnicodeStringConst;
	GNatives[StructMember] = &execStructMember;
	GNatives[DynArrayLength] = &execDynArrayLength;
	GNatives[GlobalFunction] = &execGlobalFunction;
	GNatives[PrimitiveCast] = &execPrimitiveCast;
	GNatives[DynArrayInsert] = &execDynArrayInsert;
	GNatives[DynArrayRemove] = &execDynArrayRemove;
	GNatives[DelegateFunction] = &execDelegateFunction;
	GNatives[DelegateProperty] = &execDelegateProperty;
	GNatives[LetDelegate] = &execLetDelegate;
	GNatives[EndOfScript] = &execEndOfScript;
	for (int32 i = ExtendedNative; i < FirstNative; ++i)
	{
		GNatives[i] = &execExtendedNative;
	}
	USNatives::RegisterCore();
	USNatives::RegisterEngine();
}

void USSkipNativeParms(FUSFrame& Stack)
{
	if (Stack.Node && Stack.Node->Class && Stack.Node->Class->Name == TEXT("Function")
		&& (static_cast<FUSFunction*>(Stack.Node)->FunctionFlags & USFlags::FUNC_Native) && Stack.Code >= Stack.Node->Script.GetData()
		&& Stack.Code < Stack.Node->Script.GetData() + Stack.Node->Script.Num())
	{
		Stack.Code = nullptr;
		return;
	}
	USSkipParmsGeneric(Stack, nullptr);
}

void USFinishParms(FUSFrame& Stack)
{
	if (*Stack.Code == USToken::EndFunctionParms)
	{
		Stack.Code++;
		return;
	}
	// natives called from C++ run on their own NativeParm script, which ends with EX_Nothing
	if (Stack.Node && Stack.Node->Script.Num() && Stack.Code >= Stack.Node->Script.GetData()
		&& Stack.Code < Stack.Node->Script.GetData() + Stack.Node->Script.Num() && *Stack.Code == USToken::Nothing
		&& Stack.Node->Class && Stack.Node->Class->Name == TEXT("Function")
		&& (static_cast<FUSFunction*>(Stack.Node)->FunctionFlags & USFlags::FUNC_Native))
	{
		Stack.Code++;
		return;
	}
	// extra arguments (declaration mismatch): evaluate and drop them
	for (int32 G = 0; G < 64 && *Stack.Code != USToken::EndFunctionParms && !USVM::GAbort; ++G)
	{
		FTemp T;
		const uint8* Peek = Stack.Code;
		T.Type = USVM::InferExpr(Stack.Node, Peek, Stack.Object ? Stack.Object->Class : nullptr);
		Stack.Step(Stack.Object, T.Buffer);
	}
	Stack.Code++;
}

// =============================================================================================
// Iterator support for natives
// =============================================================================================

/** Runs a native iterator's loop (UE2 PRE_ITERATOR / POST_ITERATOR). Next() stores the next item and returns false when done. */
void USRunIterator(FUSFrame& Stack, TFunctionRef<bool()> Next)
{
	using namespace USVM;
	++GIteratorRuns;
	const uint16 EndOffset = Stack.ReadWord();
	const uint8* StartCode = Stack.Code;
	alignas(16) uint8 Buffer[TempBufferSize];
	for (;;)
	{
		if (GAbort || !Next())
		{
			Stack.Code = Stack.Node->Script.GetData() + EndOffset + 1;
			break;
		}
		uint8 B = 0;
		while (!GAbort && (B = *Stack.Code) != USToken::IteratorPop && B != USToken::IteratorNext)
		{
			FMemory::Memzero(Buffer, 64);
			Stack.Step(Stack.Object, Buffer);
		}
		if (GAbort)
		{
			break;
		}
		if (*Stack.Code++ == USToken::IteratorNext)
		{
			if (!CheckRunaway(Stack))
			{
				break;
			}
			Stack.Code = StartCode;
		}
		else
		{
			break; // IteratorPop: break out of the loop
		}
	}
}
#endif // OPENUT2K4_USCRIPT_ENABLED
