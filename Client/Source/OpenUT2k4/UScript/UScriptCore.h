#pragma once

#include "CoreMinimal.h"

/** Work in progress switch for the UScript VM (keeps the shared build green while it is developed). */
#ifndef OPENUT2K4_USCRIPT_ENABLED
#define OPENUT2K4_USCRIPT_ENABLED 1
#endif

/**
 * UScript VM core types: a UE2.5 (UT2004) style object graph and bytecode interpreter.
 * See Docs/UnrealScript.md for the design. Everything here runs on the game thread only.
 *
 * Naming: FUS* = VM types. Property memory uses our own 64-bit layout (see FUSProperty); zeroed memory is a valid
 * value of every type.
 */

DECLARE_LOG_CATEGORY_EXTERN(LogUScript, Log, All);

class FUSRuntime;
class FUSLinker;
class FUSWorld;
struct FUSObject;
struct FUSField;
struct FUSProperty;
struct FUSStruct;
struct FUSFunction;
struct FUSState;
struct FUSClass;
struct FUSEnum;
struct FUSConst;
struct FUSFrame;

// ---------------------------------------------------------------------------------------------
// UE2 flags
// ---------------------------------------------------------------------------------------------

namespace USFlags
{
	// Property flags
	constexpr uint32 CPF_Edit = 0x00000001;
	constexpr uint32 CPF_Const = 0x00000002;
	constexpr uint32 CPF_OptionalParm = 0x00000010;
	constexpr uint32 CPF_Net = 0x00000020;
	constexpr uint32 CPF_Parm = 0x00000080;
	constexpr uint32 CPF_OutParm = 0x00000100;
	constexpr uint32 CPF_SkipParm = 0x00000200;
	constexpr uint32 CPF_ReturnParm = 0x00000400;
	constexpr uint32 CPF_CoerceParm = 0x00000800;
	constexpr uint32 CPF_Native = 0x00001000;
	constexpr uint32 CPF_Transient = 0x00002000;
	constexpr uint32 CPF_Config = 0x00004000;
	constexpr uint32 CPF_Localized = 0x00008000;
	constexpr uint32 CPF_GlobalConfig = 0x00040000;

	// Function flags
	constexpr uint32 FUNC_Final = 0x00000001;
	constexpr uint32 FUNC_Defined = 0x00000002;
	constexpr uint32 FUNC_Iterator = 0x00000004;
	constexpr uint32 FUNC_Latent = 0x00000008;
	constexpr uint32 FUNC_PreOperator = 0x00000010;
	constexpr uint32 FUNC_Singular = 0x00000020;
	constexpr uint32 FUNC_Net = 0x00000040;
	constexpr uint32 FUNC_Simulated = 0x00000100;
	constexpr uint32 FUNC_Exec = 0x00000200;
	constexpr uint32 FUNC_Native = 0x00000400;
	constexpr uint32 FUNC_Event = 0x00000800;
	constexpr uint32 FUNC_Operator = 0x00001000;
	constexpr uint32 FUNC_Static = 0x00002000;
	constexpr uint32 FUNC_Delegate = 0x00100000;

	// State flags
	constexpr uint32 STATE_Editable = 0x00000001;
	constexpr uint32 STATE_Auto = 0x00000002;

	// Class flags
	constexpr uint32 CLASS_Abstract = 0x00000001;
	constexpr uint32 CLASS_Config = 0x00000004;
	constexpr uint32 CLASS_Localized = 0x00000020;
	constexpr uint32 CLASS_Placeable = 0x00000200;
	constexpr uint32 CLASS_PerObjectConfig = 0x00000400;

	// Object (export) flags
	constexpr uint32 RF_HasStack = 0x02000000;
	constexpr uint32 RF_Native = 0x04000000;
}

/** VM object flags (FUSObject::Flags). */
enum EUSObjectFlags : uint32
{
	USF_PendingKill = 1 << 0,   // destroyed (actors) / dead
	USF_ClassDefault = 1 << 1,  // a class default object (Data points into the class defaults)
	USF_Field = 1 << 2,         // FUSField subclass
	USF_Actor = 1 << 3,         // instance of Engine.Actor
	USF_Placeholder = 1 << 4,   // non-script object referenced by script (texture, sound, mesh ...): name only
	USF_InSingular = 1 << 5,    // inside a singular function
	USF_Loaded = 1 << 6,        // fields: body loaded
	USF_Loading = 1 << 7,
	USF_LoadFailed = 1 << 8,
	USF_Intrinsic = 1 << 9,     // C++-only metaclass (Class, Function, ...)
	USF_Transient = 1 << 10,    // created at runtime (instances)
};

// ---------------------------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------------------------

/** Dynamic array memory (UE2 FArray). Element type from the ArrayProperty's Inner. */
struct FUSScriptArray
{
	uint8* Data = nullptr;
	int32 Num = 0;
	int32 Max = 0;
};

/** Delegate value. */
struct FUSDelegate
{
	FUSObject* Object = nullptr;
	FName FunctionName;
};

/** UE2 Vector / Rotator / Color / Plane memory (matches the script struct layout). */
struct FUSVector
{
	float X = 0.f, Y = 0.f, Z = 0.f;
	FUSVector() = default;
	FUSVector(float InX, float InY, float InZ) : X(InX), Y(InY), Z(InZ) {}
	explicit FUSVector(const FVector& V) : X(float(V.X)), Y(float(V.Y)), Z(float(V.Z)) {}
	FVector ToFVector() const { return FVector(X, Y, Z); }
	FUSVector operator+(const FUSVector& B) const { return FUSVector(X + B.X, Y + B.Y, Z + B.Z); }
	FUSVector operator-(const FUSVector& B) const { return FUSVector(X - B.X, Y - B.Y, Z - B.Z); }
	FUSVector operator*(float S) const { return FUSVector(X * S, Y * S, Z * S); }
	float Size() const { return FMath::Sqrt(X * X + Y * Y + Z * Z); }
	float SizeSquared() const { return X * X + Y * Y + Z * Z; }
};
static_assert(sizeof(FUSVector) == 12, "UE2 vector layout");

struct FUSRotator
{
	int32 Pitch = 0, Yaw = 0, Roll = 0;
	FUSRotator() = default;
	FUSRotator(int32 P, int32 Y, int32 R) : Pitch(P), Yaw(Y), Roll(R) {}
};
static_assert(sizeof(FUSRotator) == 12, "UE2 rotator layout");

struct FUSPlane
{
	float X = 0.f, Y = 0.f, Z = 0.f, W = 0.f;
};

struct FUSColor
{
	uint8 B = 0, G = 0, R = 0, A = 0;
};

/** Property value kinds. */
enum class EUSType : uint8
{
	None,
	Byte,
	Int,
	Bool,
	Float,
	Object,
	Class,
	Name,
	Str,
	Struct,
	Array,
	Delegate,
	Pointer,
};

// ---------------------------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------------------------

/** Script state of an object (current state, state code position, latent action). */
struct FUSStateFrame
{
	FUSState* StateNode = nullptr;
	/** State code being executed (null: none). Points into StateNode->Script (or a super state's). */
	const uint8* Code = nullptr;
	/** Node owning Code (the state whose script Code points into). */
	FUSStruct* CodeNode = nullptr;
	uint64 ProbeMask = ~0ull;
	/** Latent action in progress (0: none). See USLatent. */
	int32 LatentAction = 0;
	float LatentFloat = 0.f;
	FUSObject* LatentObject = nullptr;
	/** The frame currently executing this state code (GotoState/GotoLabel update it too). */
	FUSFrame* ActiveFrame = nullptr;
	/** Events disabled with Disable('Name'). */
	TArray<FName> DisabledProbes;
};

namespace USLatent
{
	constexpr int32 None = 0;
	constexpr int32 Sleep = 1;
	constexpr int32 FinishAnim = 2;
	constexpr int32 FinishInterpolation = 3;
	constexpr int32 MoveTo = 4;
	constexpr int32 WaitForLanding = 5;
	constexpr int32 Generic = 6; // completes after LatentFloat seconds
}

/** Base of every VM object. */
struct FUSObject
{
	FUSObject() = default;
	virtual ~FUSObject();
	FUSObject(const FUSObject&) = delete;
	FUSObject& operator=(const FUSObject&) = delete;

	FUSClass* Class = nullptr;
	FName Name;
	FUSObject* Outer = nullptr;
	uint32 Flags = 0;
	/** Instance property memory (Class->PropertiesSize bytes). Null for metadata objects. */
	uint8* Data = nullptr;
	FUSStateFrame* StateFrame = nullptr;
	/** World the instance lives in (null for metadata / class defaults). */
	FUSWorld* World = nullptr;
	/** Bridge link (FUSActorLink, owned by the world). */
	void* Link = nullptr;
	/** Index in FUSRuntime::Objects (metadata) or the world's object list. */
	int32 Index = INDEX_NONE;

	bool IsA(const FUSClass* SomeBase) const;
	bool IsA(FName ClassName) const;
	bool IsPendingKill() const { return (Flags & USF_PendingKill) != 0; }
	FString GetName() const { return Name.ToString(); }
	/** Package.Group.Name */
	FString GetPathName() const;
	/** "Class Package.Group.Name" */
	FString GetFullName() const;
	/** Outermost object's name (the package). */
	FName GetPackageName() const;

	/** Property memory accessors by name (slow: use FUSPropRef for hot paths). */
	FUSProperty* FindProperty(FName PropName) const;
};

/** Field: linked in a struct's Children list. */
struct FUSField : FUSObject
{
	FUSField* Next = nullptr;
	FUSField* SuperField = nullptr;
	/** Package location (for lazy loading). */
	FUSLinker* Linker = nullptr;
	int32 ExportIndex = INDEX_NONE;
};

struct FUSProperty : FUSField
{
	EUSType Type = EUSType::None;
	int32 ArrayDim = 1;
	/** Size of one element in our layout. */
	int32 ElementSize = 0;
	uint32 PropertyFlags = 0;
	/** Offset in the owner's memory. */
	int32 Offset = 0;
	FName Category;

	FUSEnum* Enum = nullptr;             // Byte
	FUSClass* PropertyClass = nullptr;   // Object / Class
	FUSClass* MetaClass = nullptr;       // Class
	FUSStruct* Struct = nullptr;         // Struct
	FUSProperty* Inner = nullptr;        // Array
	FUSFunction* DelegateFunction = nullptr; // Delegate

	int32 GetSize() const { return ElementSize * ArrayDim; }
	int32 GetAlignment() const;
	/** True when values own memory (strings, arrays, structs containing those). */
	bool NeedsDestruct() const;
	/** True when values contain object references (objects, classes, delegates, arrays/structs of those). */
	bool HasReferences() const;

	// value operations on ONE element
	void CopySingle(void* Dst, const void* Src) const;
	void DestroySingle(void* Value) const;
	bool IdenticalSingle(const void* A, const void* B) const;
	// all ArrayDim elements
	void CopyComplete(void* Dst, const void* Src) const;
	void DestroyComplete(void* Value) const;
	/** UE2 ExportText of one element (no quotes for strings when bPlain). */
	void ExportText(FString& Out, const void* Value, bool bPlain = true) const;
	/** UE2 ImportText of one element. Returns false on parse error. */
	bool ImportText(const TCHAR*& In, void* Value) const;
	/** Clears object references to pending-kill objects in one element. */
	void CleanupRefs(void* Value) const;
};

struct FUSEnum : FUSField
{
	TArray<FName> Names;
};

struct FUSConst : FUSField
{
	FString Value;
};

/** Types inferred from bytecode (Context results, Switch values). */
struct FUSExprType
{
	EUSType Type = EUSType::None;
	/** Property describing the value (struct/array/object details), when known. */
	FUSProperty* Prop = nullptr;
	int32 Size() const;
	bool IsKnown() const { return Type != EUSType::None; }
};

struct FUSStruct : FUSField
{
	FUSField* Children = nullptr;
	/** Bytecode in UE2 in-memory layout (object refs and names are 4-byte table indices). */
	TArray<uint8> Script;
	int32 PropertiesSize = 0;
	int32 MinAlignment = 1;
	FName FriendlyName;
	int32 Line = 0;
	/** All properties including the super struct's, in layout order. */
	TArray<FUSProperty*> AllProps;
	/** Properties whose values need destruction (subset of AllProps). */
	TArray<FUSProperty*> CtorProps;
	/** Properties holding object references (subset of AllProps). */
	TArray<FUSProperty*> RefProps;
	/** Name -> property (own and inherited). */
	TMap<FName, FUSProperty*> PropertyMap;

	FUSStruct* GetSuperStruct() const { return static_cast<FUSStruct*>(SuperField); }
	bool IsChildOf(const FUSStruct* Other) const;
	FUSProperty* FindPropertyByName(FName InName) const { FUSProperty* const* P = PropertyMap.Find(InName); return P ? *P : nullptr; }
	/** Default-construct (zero) / destroy a memory block laid out like this struct. */
	void InitializeValue(uint8* Mem) const { FMemory::Memzero(Mem, PropertiesSize); }
	void DestroyValue(uint8* Mem) const;
	void CopyValue(uint8* Dst, const uint8* Src) const;
	bool IdenticalValue(const uint8* A, const uint8* B) const;
};

using FUSNativeFunc = void (*)(FUSObject* Self, FUSFrame& Stack, void* Result);

struct FUSFunction : FUSStruct
{
	uint32 FunctionFlags = 0;
	uint16 iNative = 0;
	uint8 OperPrecedence = 0;
	uint8 NumParms = 0;
	int32 ParmsSize = 0;
	int32 ReturnValueOffset = INDEX_NONE;
	FUSProperty* ReturnProperty = nullptr;
	/** Parameters in call order (without the return value). */
	TArray<FUSProperty*> Parms;
	/** Native implementation (FUNC_Native); null when missing. */
	FUSNativeFunc Func = nullptr;
	/** Inferred Context/Switch types; the token's size byte indexes this (255 = unknown). */
	TArray<FUSExprType> ExprTypes;
	bool bExprTypesDone = false;

	bool HasAnyFunctionFlags(uint32 F) const { return (FunctionFlags & F) != 0; }
	FUSClass* GetOwnerClass() const;
};

struct FUSState : FUSStruct
{
	uint64 ProbeMask = 0;
	uint64 IgnoreMask = 0;
	uint16 LabelTableOffset = 0xFFFF;
	uint32 StateFlags = 0;
	/** Functions visible in this scope (own + super states; for classes: own + super classes). */
	TMap<FName, FUSFunction*> FuncMap;
};

struct FUSClass : FUSState
{
	uint32 ClassFlags = 0;
	FUSClass* ClassWithin = nullptr;
	FName ConfigName;
	/** Default property values (PropertiesSize bytes). */
	TArray<uint8> Defaults;
	FUSObject* DefaultObject = nullptr;
	/** States visible in this class (own + inherited), by name. */
	TMap<FName, FUSState*> StateMap;
	/** Any script function named Tick/Timer in the class chain (fast checks for the world tick). */
	bool bHasTick = false;
	/** Load error (when USF_LoadFailed). */
	FString LoadError;

	FUSClass* GetSuperClass() const { return static_cast<FUSClass*>(SuperField); }
	bool IsChildOf(const FUSClass* Other) const;
	FUSFunction* FindFunction(FName FuncName) const { FUSFunction* const* F = FuncMap.Find(FuncName); return F ? *F : nullptr; }
	FUSState* FindState(FName StateName) const { FUSState* const* S = StateMap.Find(StateName); return S ? *S : nullptr; }
	/** Loads the class body if this is still a shell. False if it cannot be loaded. */
	bool EnsureLoaded();
	bool IsLoaded() const { return (Flags & USF_Loaded) != 0; }
	FUSObject* GetDefaultObject() { EnsureLoaded(); return DefaultObject; }
};

/** Cached property lookup by class and name ("Actor", "Location"). Resolve() once the class is loaded. */
struct FUSPropRef
{
	FUSProperty* Prop = nullptr;
	bool Resolve(FUSClass* InClass, const TCHAR* PropName);
	template <typename T> T* Ptr(FUSObject* Obj) const { return (Prop && Obj && Obj->Data) ? reinterpret_cast<T*>(Obj->Data + Prop->Offset) : nullptr; }
	template <typename T> T Get(FUSObject* Obj, T Default = T()) const { T* P = Ptr<T>(Obj); return P ? *P : Default; }
	template <typename T> void Set(FUSObject* Obj, const T& V) const { if (T* P = Ptr<T>(Obj)) { *P = V; } }
};

// ---------------------------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------------------------

/** Script tokens. */
namespace USToken
{
	enum : uint8
	{
		LocalVariable = 0x00, InstanceVariable = 0x01, DefaultVariable = 0x02, Return = 0x04, Switch = 0x05, Jump = 0x06,
		JumpIfNot = 0x07, Stop = 0x08, Assert = 0x09, Case = 0x0A, Nothing = 0x0B, LabelTable = 0x0C, GotoLabel = 0x0D,
		EatString = 0x0E, Let = 0x0F, DynArrayElement = 0x10, New = 0x11, ClassContext = 0x12, MetaCast = 0x13,
		LetBool = 0x14, EndFunctionParms = 0x16, Self = 0x17, Skip = 0x18, Context = 0x19, ArrayElement = 0x1A,
		VirtualFunction = 0x1B, FinalFunction = 0x1C, IntConst = 0x1D, FloatConst = 0x1E, StringConst = 0x1F,
		ObjectConst = 0x20, NameConst = 0x21, RotationConst = 0x22, VectorConst = 0x23, ByteConst = 0x24, IntZero = 0x25,
		IntOne = 0x26, True = 0x27, False = 0x28, NativeParm = 0x29, NoObject = 0x2A, IntConstByte = 0x2C,
		BoolVariable = 0x2D, DynamicCast = 0x2E, Iterator = 0x2F, IteratorPop = 0x30, IteratorNext = 0x31,
		StructCmpEq = 0x32, StructCmpNe = 0x33, UnicodeStringConst = 0x34, StructMember = 0x36, DynArrayLength = 0x37,
		GlobalFunction = 0x38, PrimitiveCast = 0x39, DynArrayInsert = 0x40, DynArrayRemove = 0x41,
		DelegateFunction = 0x43, DelegateProperty = 0x44, LetDelegate = 0x45, EndOfScript = 0x47,
		ExtendedNative = 0x60, FirstNative = 0x70,
	};
}

/** Out parameter being copied back after a script call. */
struct FUSOutParm
{
	FUSProperty* Prop = nullptr;
	uint8* Addr = nullptr;
	FUSOutParm* Next = nullptr;
};

/** A script call frame. */
struct FUSFrame
{
	FUSStruct* Node = nullptr;
	FUSObject* Object = nullptr;
	const uint8* Code = nullptr;
	uint8* Locals = nullptr;
	FUSFrame* PrevFrame = nullptr;
	FUSOutParm* OutParms = nullptr;

	FUSFrame(FUSObject* InObject, FUSStruct* InNode, const uint8* InCode, uint8* InLocals, FUSFrame* InPrev = nullptr)
		: Node(InNode), Object(InObject), Code(InCode), Locals(InLocals), PrevFrame(InPrev) {}

	/** Executes one expression/statement in Context, writing its value to Result (may be null). */
	void Step(FUSObject* Context, void* Result);

	int32 ReadInt() { int32 V; FMemory::Memcpy(&V, Code, 4); Code += 4; return V; }
	float ReadFloat() { float V; FMemory::Memcpy(&V, Code, 4); Code += 4; return V; }
	uint16 ReadWord() { uint16 V; FMemory::Memcpy(&V, Code, 2); Code += 2; return V; }
	FName ReadName();
	FUSObject* ReadObject();
	/** Script offset of Code within Node. */
	int32 CodeOffset() const;
	/** "Class.Function:offset" for logs. */
	FString Describe() const;
};

/** Interpreter globals and entry points (game thread only). */
namespace USVM
{
	/** Last variable evaluated (UE2 GProperty/GPropAddr). */
	extern FUSProperty* GProperty;
	extern uint8* GPropAddr;
	/** Set when the current top-level call must unwind (runaway loop, fatal script error). */
	extern bool GAbort;

	/** Token/native dispatch table. */
	extern FUSNativeFunc GNatives[4096];

	void Init();
	/** Calls Function on Self with parameters read from the caller's Stack (bytecode call). */
	void CallFunction(FUSObject* Self, FUSFrame& Stack, void* Result, FUSFunction* Function);
	/** Calls a function with a parameter block laid out like Function's locals (C++ -> script). */
	void ProcessEvent(FUSObject* Self, FUSFunction* Function, void* Parms);
	/** Runs Self's state code until it blocks on a latent action or ends (called by the world tick). */
	void ProcessState(FUSObject* Self, float DeltaSeconds);
	/** GotoState (events BeginState/EndState). Returns false if the state does not exist. */
	bool GotoState(FUSObject* Self, FName NewState, FName Label = NAME_None);
	/** Finds a function in Self's current state then class ("virtual"). */
	FUSFunction* FindFunction(const FUSObject* Self, FName Name, bool bGlobal = false);
	/** Probe check (state ignores / disabled events). */
	bool IsProbing(const FUSObject* Self, FName FuncName);
	/** Reports a script warning with the script position (rate limited per message). */
	void ScriptWarn(const FUSFrame* Stack, const FString& Message);
	/** Aborts the current top-level script call. */
	void ScriptError(const FUSFrame* Stack, const FString& Message);
	/** Disassembles a function/state script (debugging). */
	FString Disassemble(FUSStruct* Node);
	/** Number of script calls / tokens executed (perf counters). */
	extern uint64 GStatCalls;
	extern uint64 GStatTokens;
	/** Infers the value type of the expression at Code (does not execute). Code advances past it. */
	FUSExprType InferExpr(FUSStruct* Node, const uint8*& Code, FUSClass* ContextClass);
	/** Skips one expression without executing it. */
	void SkipExpr(const uint8*& Code);

	/** A function call found in bytecode (static analysis, see ForEachCall). */
	struct FCallSite
	{
		/** The function the call names (final / native calls), or the one the name finds in ContextClass (virtual calls; may be null). */
		FUSFunction* Function = nullptr;
		/** Virtual / global / delegate calls: the function name (resolved at run time on the object). */
		FName Name;
		/** Static class of the call's object (the self class for calls without a context, when the context type is unknown too). */
		FUSClass* ContextClass = nullptr;
		/** Native calls by index: the index (the declaration may be missing). INDEX_NONE otherwise. */
		int32 NativeIndex = INDEX_NONE;
	};
	/**
	 * Walks Node's script without executing it and reports every function call (any nesting) and every object constant
	 * (Visit / VisitObject). SelfClass is the class of the object running the script (names resolve on it).
	 */
	void ForEachCall(FUSStruct* Node, FUSClass* SelfClass, TFunctionRef<void(const FCallSite&)> Visit, TFunctionRef<void(FUSObject*)> VisitObject);
}

/** Helper for calling script events from C++: builds the parameter block of a function. */
class FUSParms
{
public:
	FUSParms(FUSFunction* InFunction);
	~FUSParms();
	FUSParms(const FUSParms&) = delete;
	FUSParms& operator=(const FUSParms&) = delete;

	bool IsValid() const { return Function != nullptr; }
	FUSParms& Int(int32 V) { return Put(&V, EUSType::Int); }
	FUSParms& Float(float V) { return Put(&V, EUSType::Float); }
	FUSParms& Bool(bool V) { uint32 B = V ? 1u : 0u; return Put(&B, EUSType::Bool); }
	FUSParms& Byte(uint8 V) { return Put(&V, EUSType::Byte); }
	FUSParms& Object(FUSObject* V) { return Put(&V, EUSType::Object); }
	FUSParms& Name(FName V) { return Put(&V, EUSType::Name); }
	FUSParms& Str(const FString& V);
	FUSParms& Vector(const FUSVector& V) { return Put(&V, EUSType::Struct); }
	/** Skips one parameter (left at its zero / default value). */
	FUSParms& Skip();
	/** Calls the function on Self. */
	void Call(FUSObject* Self);
	/** Address of a parameter by index (after Call: out values). */
	uint8* ParmAddr(int32 Index) const;
	FUSProperty* ParmProp(int32 Index) const;
	int32 GetInt(int32 Index) const;
	float GetFloat(int32 Index) const;
	bool GetBool(int32 Index) const;
	uint8 GetByte(int32 Index) const;
	FUSObject* GetObject(int32 Index) const;
	FString GetStr(int32 Index) const;
	uint8* ReturnAddr() const;
	bool ReturnBool() const;
	int32 ReturnInt() const;
	float ReturnFloat() const;
	FUSObject* ReturnObject() const;
	FString ReturnStr() const;

private:
	FUSParms& Put(const void* Value, EUSType Type);
	FUSFunction* Function = nullptr;
	uint8* Buffer = nullptr;
	FUSProperty* Cursor = nullptr;
};

// ---------------------------------------------------------------------------------------------
// Native helpers (UE2 P_GET style). Natives read their parameters from the bytecode stream.
// ---------------------------------------------------------------------------------------------

#define US_NATIVE(Name) static void Name(FUSObject* Self, FUSFrame& Stack, void* Result)
/** Steps one parameter expression unless the call ended (packages compiled against older declarations). */
#define US_STEP_PARM(Addr) if (*Stack.Code != USToken::EndFunctionParms) { Stack.Step(Stack.Object, Addr); }
#define USP_GET_INT(V) int32 V = 0; US_STEP_PARM(&V)
#define USP_GET_INT_OPTX(V, Def) int32 V = Def; US_STEP_PARM(&V)
#define USP_GET_FLOAT(V) float V = 0.f; US_STEP_PARM(&V)
#define USP_GET_FLOAT_OPTX(V, Def) float V = Def; US_STEP_PARM(&V)
#define USP_GET_BYTE(V) uint8 V = 0; US_STEP_PARM(&V)
#define USP_GET_BYTE_OPTX(V, Def) uint8 V = Def; US_STEP_PARM(&V)
#define USP_GET_UBOOL(V) uint32 V = 0; US_STEP_PARM(&V)
#define USP_GET_UBOOL_OPTX(V, Def) uint32 V = (Def) ? 1u : 0u; US_STEP_PARM(&V)
#define USP_GET_NAME(V) FName V; US_STEP_PARM(&V)
#define USP_GET_NAME_OPTX(V, Def) FName V = Def; US_STEP_PARM(&V)
#define USP_GET_STR(V) FString V; US_STEP_PARM(&V)
#define USP_GET_STR_OPTX(V, Def) FString V = Def; US_STEP_PARM(&V)
#define USP_GET_OBJECT(V) FUSObject* V = nullptr; US_STEP_PARM(&V)
#define USP_GET_OBJECT_OPTX(V, Def) FUSObject* V = Def; US_STEP_PARM(&V)
#define USP_GET_VECTOR(V) FUSVector V; US_STEP_PARM(&V)
#define USP_GET_VECTOR_OPTX(V, Def) FUSVector V = Def; US_STEP_PARM(&V)
#define USP_GET_ROTATOR(V) FUSRotator V; US_STEP_PARM(&V)
#define USP_GET_ROTATOR_OPTX(V, Def) FUSRotator V = Def; US_STEP_PARM(&V)
#define USP_GET_STRUCT(Type, V) Type V; US_STEP_PARM(&V)
/** Reference parameters: the variable's address (or a temporary when the argument is not a variable). */
#define USP_GET_REF(Type, V) Type V##Temp{}; USVM::GPropAddr = nullptr; US_STEP_PARM(&V##Temp); Type* V = USVM::GPropAddr ? reinterpret_cast<Type*>(USVM::GPropAddr) : &V##Temp
#define USP_GET_INT_REF(V) USP_GET_REF(int32, V)
#define USP_GET_FLOAT_REF(V) USP_GET_REF(float, V)
#define USP_GET_BYTE_REF(V) USP_GET_REF(uint8, V)
#define USP_GET_UBOOL_REF(V) USP_GET_REF(uint32, V)
#define USP_GET_STR_REF(V) USP_GET_REF(FString, V)
#define USP_GET_NAME_REF(V) USP_GET_REF(FName, V)
#define USP_GET_OBJECT_REF(V) USP_GET_REF(FUSObject*, V)
#define USP_GET_VECTOR_REF(V) USP_GET_REF(FUSVector, V)
#define USP_GET_ROTATOR_REF(V) USP_GET_REF(FUSRotator, V)
/** Array reference: the array's address and its property (GProperty) for the element type. */
#define USP_GET_ARRAY_REF(V, PropV) FUSScriptArray V##Temp; USVM::GPropAddr = nullptr; USVM::GProperty = nullptr; US_STEP_PARM(&V##Temp); \
	FUSScriptArray* V = USVM::GPropAddr ? reinterpret_cast<FUSScriptArray*>(USVM::GPropAddr) : &V##Temp; FUSProperty* PropV = USVM::GPropAddr ? USVM::GProperty : nullptr
#define USP_GET_SKIP_OFFSET(V) uint16 V = 0; { Stack.Code++; V = Stack.ReadWord(); }
#define USP_FINISH USFinishParms(Stack)

/** How the game can host a script class. */
enum class EUSClassSupport : uint8
{
	RunsInVM,        // instantiated in the script VM (mutators, game rules, map script actors, plain objects)
	NotSupported,    // not runnable (yet): OutReason says why
};
/** Support policy for a script class (shared by the map loader and ArenaUScript::CanRun). */
EUSClassSupport USGetClassSupport(FUSClass* Class, FString& OutReason);

/** Consumes a native's parameters without reading them (stub natives). */
void USSkipNativeParms(FUSFrame& Stack);

/** Jumps the object's state code to a label (UE2 GotoLabel). */
bool USGotoLabel(FUSObject* Obj, FName Label);

/** Ends a native's parameter list: skips EndFunctionParms (and any extra arguments). */
void USFinishParms(FUSFrame& Stack);
/** Runs a native iterator's loop (UE2 PRE_ITERATOR/POST_ITERATOR): Next() stores the next item, false when done. Call after USP_FINISH. */
void USRunIterator(FUSFrame& Stack, TFunctionRef<bool()> Next);

#define US_RESULT(Type) (*reinterpret_cast<Type*>(Result))

/** Native registration: Class.Function (case-insensitive) -> implementation. */
struct FUSNativeEntry
{
	const TCHAR* ClassName;
	const TCHAR* FuncName;
	FUSNativeFunc Func;
};

namespace USNatives
{
	/** Registers a table of natives (call from a static initializer or Init()). */
	void Register(const FUSNativeEntry* Entries, int32 Count);
	FUSNativeFunc Find(FName ClassName, FName FuncName);
	void RegisterCore();
	void RegisterEngine();
}
