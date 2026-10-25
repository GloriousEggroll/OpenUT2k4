// UScript natives of Core.Object (math, strings, vectors/rotators, objects, states, config).
// Bound by Class.FunctionName at load time (the native index comes from the package).

#include "UScriptCore.h"
#if OPENUT2K4_USCRIPT_ENABLED
#include "UScriptRuntime.h"
#include "UScriptWorld.h"

// ---------------------------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------------------------

namespace
{
	TMap<TPair<FName, FName>, FUSNativeFunc>& NativeMap()
	{
		static TMap<TPair<FName, FName>, FUSNativeFunc> Map;
		return Map;
	}

	FORCEINLINE FUSObject* Live(FUSObject* O) { return (O && !O->IsPendingKill()) ? O : nullptr; }

	constexpr float RotToRad = PI / 32768.f;

	FRotator ToFRotator(const FUSRotator& R)
	{
		return FRotator(R.Pitch * (360.0 / 65536.0), R.Yaw * (360.0 / 65536.0), R.Roll * (360.0 / 65536.0));
	}

	FUSRotator FromFRotator(const FRotator& R)
	{
		auto C = [](double Deg) { return int32(FMath::RoundToInt(Deg * 65536.0 / 360.0)) & 0xFFFF; };
		return FUSRotator(C(R.Pitch), C(R.Yaw), C(R.Roll));
	}

	FUSVector Rotate(const FUSVector& V, const FUSRotator& R) { return FUSVector(FRotationMatrix(ToFRotator(R)).TransformVector(V.ToFVector())); }
	FUSVector Unrotate(const FUSVector& V, const FUSRotator& R) { return FUSVector(FRotationMatrix(ToFRotator(R)).InverseTransformVector(V.ToFVector())); }

	FUSVector SafeNormal(const FUSVector& V)
	{
		const float S = V.SizeSquared();
		if (S < 1e-8f)
		{
			return FUSVector();
		}
		const float Inv = 1.f / FMath::Sqrt(S);
		return V * Inv;
	}

	/** UE2 FString::Mid / Left / Right clamping. */
	FString UMid(const FString& S, int32 Start, int32 Count)
	{
		int64 End = int64(Start) + Count;
		Start = FMath::Clamp(Start, 0, S.Len());
		End = FMath::Clamp<int64>(End, Start, S.Len());
		return S.Mid(Start, int32(End - Start));
	}
	FString ULeft(const FString& S, int32 N) { return S.Left(FMath::Clamp(N, 0, S.Len())); }
	FString URight(const FString& S, int32 N) { return S.Right(FMath::Clamp(N, 0, S.Len())); }

	struct FUSQuat { float X = 0, Y = 0, Z = 0, W = 1; };
	FQuat ToFQuat(const FUSQuat& Q) { return FQuat(Q.X, Q.Y, Q.Z, Q.W); }
	FUSQuat FromFQuat(const FQuat& Q) { FUSQuat R; R.X = float(Q.X); R.Y = float(Q.Y); R.Z = float(Q.Z); R.W = float(Q.W); return R; }

	void SetStringArray(FUSScriptArray* A, FUSProperty* ArrayProp, const TArray<FString>& Items)
	{
		if (!A)
		{
			return;
		}
		if (ArrayProp && ArrayProp->Type == EUSType::Array)
		{
			ArrayProp->DestroySingle(A);
		}
		else if (A->Data)
		{
			for (int32 i = 0; i < A->Num; ++i)
			{
				reinterpret_cast<FString*>(A->Data)[i].~FString();
			}
			FMemory::Free(A->Data);
			*A = FUSScriptArray();
		}
		if (Items.Num())
		{
			A->Data = static_cast<uint8*>(FMemory::Malloc(sizeof(FString) * Items.Num(), 16));
			FMemory::Memzero(A->Data, sizeof(FString) * Items.Num());
			A->Num = A->Max = Items.Num();
			for (int32 i = 0; i < Items.Num(); ++i)
			{
				reinterpret_cast<FString*>(A->Data)[i] = Items[i];
			}
		}
	}
}

void USNatives::Register(const FUSNativeEntry* Entries, int32 Count)
{
	for (int32 i = 0; i < Count; ++i)
	{
		NativeMap().Add(TPair<FName, FName>(FName(Entries[i].ClassName), FName(Entries[i].FuncName)), Entries[i].Func);
	}
}

FUSNativeFunc USNatives::Find(FName ClassName, FName FuncName)
{
	const FUSNativeFunc* F = NativeMap().Find(TPair<FName, FName>(ClassName, FuncName));
	return F ? *F : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Natives
// ---------------------------------------------------------------------------------------------

namespace USCoreNatives
{
	// ---- float ----
#define FLOAT_BINOP(Name, Expr) US_NATIVE(Name) { USP_GET_FLOAT(A); USP_GET_FLOAT(B); USP_FINISH; US_RESULT(float) = (Expr); }
#define FLOAT_CMP(Name, Expr) US_NATIVE(Name) { USP_GET_FLOAT(A); USP_GET_FLOAT(B); USP_FINISH; US_RESULT(uint32) = (Expr) ? 1u : 0u; }
#define FLOAT_ASSIGN(Name, Op) US_NATIVE(Name) { USP_GET_FLOAT_REF(A); USP_GET_FLOAT(B); USP_FINISH; *A Op B; US_RESULT(float) = *A; }
#define FLOAT_UNARY(Name, Expr) US_NATIVE(Name) { USP_GET_FLOAT(A); USP_FINISH; US_RESULT(float) = (Expr); }
	FLOAT_ASSIGN(DivideEqual_FloatFloat, /=)
	FLOAT_ASSIGN(MultiplyEqual_FloatFloat, *=)
	FLOAT_ASSIGN(AddEqual_FloatFloat, +=)
	FLOAT_ASSIGN(SubtractEqual_FloatFloat, -=)
	FLOAT_CMP(NotEqual_FloatFloat, A != B)
	FLOAT_CMP(ComplementEqual_FloatFloat, FMath::Abs(A - B) < 0.0001f)
	FLOAT_CMP(EqualEqual_FloatFloat, A == B)
	FLOAT_CMP(GreaterEqual_FloatFloat, A >= B)
	FLOAT_CMP(LessEqual_FloatFloat, A <= B)
	FLOAT_CMP(Greater_FloatFloat, A > B)
	FLOAT_CMP(Less_FloatFloat, A < B)
	FLOAT_BINOP(Subtract_FloatFloat, A - B)
	FLOAT_BINOP(Add_FloatFloat, A + B)
	FLOAT_BINOP(Multiply_FloatFloat, A * B)
	FLOAT_BINOP(Divide_FloatFloat, B != 0.f ? A / B : 0.f)
	FLOAT_BINOP(Percent_FloatFloat, B != 0.f ? FMath::Fmod(A, B) : 0.f)
	FLOAT_BINOP(MultiplyMultiply_FloatFloat, FMath::Pow(A, B))
	FLOAT_BINOP(FMin, FMath::Min(A, B))
	FLOAT_BINOP(FMax, FMath::Max(A, B))
	FLOAT_UNARY(Abs, FMath::Abs(A))
	FLOAT_UNARY(Sin, FMath::Sin(A))
	FLOAT_UNARY(Asin, FMath::Asin(FMath::Clamp(A, -1.f, 1.f)))
	FLOAT_UNARY(Cos, FMath::Cos(A))
	FLOAT_UNARY(Acos, FMath::Acos(FMath::Clamp(A, -1.f, 1.f)))
	FLOAT_UNARY(Tan, FMath::Tan(A))
	FLOAT_UNARY(Exp, FMath::Exp(A))
	FLOAT_UNARY(Loge, A > 0.f ? FMath::Loge(A) : 0.f)
	FLOAT_UNARY(Sqrt, A > 0.f ? FMath::Sqrt(A) : 0.f)
	FLOAT_UNARY(Square, A * A)
	FLOAT_UNARY(Subtract_PreFloat, -A)
	FLOAT_UNARY(Ceil, FMath::CeilToFloat(A))
	FLOAT_UNARY(Round, float(FMath::RoundToInt(A)))
	US_NATIVE(Atan)
	{
		USP_GET_FLOAT(A);
		USP_GET_FLOAT_OPTX(B, 1.f);
		USP_FINISH;
		US_RESULT(float) = FMath::Atan2(A, B);
	}
	US_NATIVE(FRand) { USP_FINISH; US_RESULT(float) = FMath::FRand(); }
	US_NATIVE(FClamp) { USP_GET_FLOAT(V); USP_GET_FLOAT(A); USP_GET_FLOAT(B); USP_FINISH; US_RESULT(float) = FMath::Clamp(V, A, B); }
	US_NATIVE(Lerp)
	{
		USP_GET_FLOAT(Alpha); USP_GET_FLOAT(A); USP_GET_FLOAT(B); USP_GET_UBOOL_OPTX(bClamp, false); USP_FINISH;
		if (bClamp) { Alpha = FMath::Clamp(Alpha, 0.f, 1.f); }
		US_RESULT(float) = A + Alpha * (B - A);
	}
	US_NATIVE(Smerp)
	{
		USP_GET_FLOAT(Alpha); USP_GET_FLOAT(A); USP_GET_FLOAT(B); USP_FINISH;
		US_RESULT(float) = A + (3.f * Alpha * Alpha - 2.f * Alpha * Alpha * Alpha) * (B - A);
	}

	// ---- int ----
#define INT_BINOP(Name, Expr) US_NATIVE(Name) { USP_GET_INT(A); USP_GET_INT(B); USP_FINISH; US_RESULT(int32) = (Expr); }
#define INT_CMP(Name, Expr) US_NATIVE(Name) { USP_GET_INT(A); USP_GET_INT(B); USP_FINISH; US_RESULT(uint32) = (Expr) ? 1u : 0u; }
	INT_BINOP(Or_IntInt, A | B)
	INT_BINOP(Xor_IntInt, A ^ B)
	INT_BINOP(And_IntInt, A & B)
	INT_BINOP(GreaterGreaterGreater_IntInt, int32(uint32(A) >> (B & 31)))
	INT_BINOP(GreaterGreater_IntInt, A >> (B & 31))
	INT_BINOP(LessLess_IntInt, int32(uint32(A) << (B & 31)))
	INT_BINOP(Subtract_IntInt, int32(uint32(A) - uint32(B)))
	INT_BINOP(Add_IntInt, int32(uint32(A) + uint32(B)))
	INT_BINOP(Multiply_IntInt, int32(uint32(A) * uint32(B)))
	INT_BINOP(Min, FMath::Min(A, B))
	INT_BINOP(Max, FMath::Max(A, B))
	INT_CMP(NotEqual_IntInt, A != B)
	INT_CMP(EqualEqual_IntInt, A == B)
	INT_CMP(GreaterEqual_IntInt, A >= B)
	INT_CMP(LessEqual_IntInt, A <= B)
	INT_CMP(Greater_IntInt, A > B)
	INT_CMP(Less_IntInt, A < B)
	US_NATIVE(Divide_IntInt)
	{
		USP_GET_INT(A); USP_GET_INT(B); USP_FINISH;
		if (B == 0)
		{
			USVM::ScriptWarn(&Stack, TEXT("Divide by zero"));
			US_RESULT(int32) = 0;
			return;
		}
		US_RESULT(int32) = (A == INT32_MIN && B == -1) ? A : A / B;
	}
	US_NATIVE(Clamp) { USP_GET_INT(V); USP_GET_INT(A); USP_GET_INT(B); USP_FINISH; US_RESULT(int32) = FMath::Clamp(V, A, B); }
	US_NATIVE(Rand) { USP_GET_INT(Max); USP_FINISH; US_RESULT(int32) = Max > 0 ? FMath::RandHelper(Max) : 0; }
	US_NATIVE(SubtractSubtract_Int) { USP_GET_INT_REF(A); USP_FINISH; US_RESULT(int32) = (*A)--; }
	US_NATIVE(AddAdd_Int) { USP_GET_INT_REF(A); USP_FINISH; US_RESULT(int32) = (*A)++; }
	US_NATIVE(SubtractSubtract_PreInt) { USP_GET_INT_REF(A); USP_FINISH; US_RESULT(int32) = --(*A); }
	US_NATIVE(AddAdd_PreInt) { USP_GET_INT_REF(A); USP_FINISH; US_RESULT(int32) = ++(*A); }
	US_NATIVE(SubtractEqual_IntInt) { USP_GET_INT_REF(A); USP_GET_INT(B); USP_FINISH; *A -= B; US_RESULT(int32) = *A; }
	US_NATIVE(AddEqual_IntInt) { USP_GET_INT_REF(A); USP_GET_INT(B); USP_FINISH; *A += B; US_RESULT(int32) = *A; }
	US_NATIVE(DivideEqual_IntFloat) { USP_GET_INT_REF(A); USP_GET_FLOAT(B); USP_FINISH; *A = B != 0.f ? int32(*A / B) : 0; US_RESULT(int32) = *A; }
	US_NATIVE(MultiplyEqual_IntFloat) { USP_GET_INT_REF(A); USP_GET_FLOAT(B); USP_FINISH; *A = int32(*A * B); US_RESULT(int32) = *A; }
	US_NATIVE(Subtract_PreInt) { USP_GET_INT(A); USP_FINISH; US_RESULT(int32) = -A; }
	US_NATIVE(Complement_PreInt) { USP_GET_INT(A); USP_FINISH; US_RESULT(int32) = ~A; }
	US_NATIVE(ClockwiseFrom_IntInt)
	{
		USP_GET_INT(A); USP_GET_INT(B); USP_FINISH;
		A &= 0xFFFF;
		B &= 0xFFFF;
		US_RESULT(uint32) = (FMath::Abs(A - B) > 32768 ? (A < B) : (A > B)) ? 1u : 0u;
	}

	// ---- byte ----
	US_NATIVE(SubtractSubtract_Byte) { USP_GET_BYTE_REF(A); USP_FINISH; US_RESULT(uint8) = (*A)--; }
	US_NATIVE(AddAdd_Byte) { USP_GET_BYTE_REF(A); USP_FINISH; US_RESULT(uint8) = (*A)++; }
	US_NATIVE(SubtractSubtract_PreByte) { USP_GET_BYTE_REF(A); USP_FINISH; US_RESULT(uint8) = --(*A); }
	US_NATIVE(AddAdd_PreByte) { USP_GET_BYTE_REF(A); USP_FINISH; US_RESULT(uint8) = ++(*A); }
	US_NATIVE(SubtractEqual_ByteByte) { USP_GET_BYTE_REF(A); USP_GET_BYTE(B); USP_FINISH; *A -= B; US_RESULT(uint8) = *A; }
	US_NATIVE(AddEqual_ByteByte) { USP_GET_BYTE_REF(A); USP_GET_BYTE(B); USP_FINISH; *A += B; US_RESULT(uint8) = *A; }
	US_NATIVE(DivideEqual_ByteByte) { USP_GET_BYTE_REF(A); USP_GET_BYTE(B); USP_FINISH; *A = B ? *A / B : 0; US_RESULT(uint8) = *A; }
	US_NATIVE(MultiplyEqual_ByteByte) { USP_GET_BYTE_REF(A); USP_GET_BYTE(B); USP_FINISH; *A = uint8(*A * B); US_RESULT(uint8) = *A; }

	// ---- bool ----
	US_NATIVE(OrOr_BoolBool)
	{
		USP_GET_UBOOL(A);
		USP_GET_SKIP_OFFSET(W);
		if (!A)
		{
			USP_GET_UBOOL(B);
			US_RESULT(uint32) = B ? 1u : 0u;
			Stack.Code++;
		}
		else
		{
			US_RESULT(uint32) = 1;
			Stack.Code += W;
		}
	}
	US_NATIVE(AndAnd_BoolBool)
	{
		USP_GET_UBOOL(A);
		USP_GET_SKIP_OFFSET(W);
		if (A)
		{
			USP_GET_UBOOL(B);
			US_RESULT(uint32) = B ? 1u : 0u;
			Stack.Code++;
		}
		else
		{
			US_RESULT(uint32) = 0;
			Stack.Code += W;
		}
	}
	US_NATIVE(XorXor_BoolBool) { USP_GET_UBOOL(A); USP_GET_UBOOL(B); USP_FINISH; US_RESULT(uint32) = (!A != !B) ? 1u : 0u; }
	US_NATIVE(NotEqual_BoolBool) { USP_GET_UBOOL(A); USP_GET_UBOOL(B); USP_FINISH; US_RESULT(uint32) = (!A != !B) ? 1u : 0u; }
	US_NATIVE(EqualEqual_BoolBool) { USP_GET_UBOOL(A); USP_GET_UBOOL(B); USP_FINISH; US_RESULT(uint32) = (!A == !B) ? 1u : 0u; }
	US_NATIVE(Not_PreBool) { USP_GET_UBOOL(A); USP_FINISH; US_RESULT(uint32) = A ? 0u : 1u; }

	// ---- vector ----
	US_NATIVE(Subtract_PreVector) { USP_GET_VECTOR(A); USP_FINISH; US_RESULT(FUSVector) = FUSVector(-A.X, -A.Y, -A.Z); }
	US_NATIVE(Multiply_VectorFloat) { USP_GET_VECTOR(A); USP_GET_FLOAT(B); USP_FINISH; US_RESULT(FUSVector) = A * B; }
	US_NATIVE(Multiply_FloatVector) { USP_GET_FLOAT(A); USP_GET_VECTOR(B); USP_FINISH; US_RESULT(FUSVector) = B * A; }
	US_NATIVE(Multiply_VectorVector) { USP_GET_VECTOR(A); USP_GET_VECTOR(B); USP_FINISH; US_RESULT(FUSVector) = FUSVector(A.X * B.X, A.Y * B.Y, A.Z * B.Z); }
	US_NATIVE(Divide_VectorFloat) { USP_GET_VECTOR(A); USP_GET_FLOAT(B); USP_FINISH; US_RESULT(FUSVector) = B != 0.f ? A * (1.f / B) : FUSVector(); }
	US_NATIVE(Add_VectorVector) { USP_GET_VECTOR(A); USP_GET_VECTOR(B); USP_FINISH; US_RESULT(FUSVector) = A + B; }
	US_NATIVE(Subtract_VectorVector) { USP_GET_VECTOR(A); USP_GET_VECTOR(B); USP_FINISH; US_RESULT(FUSVector) = A - B; }
	US_NATIVE(LessLess_VectorRotator) { USP_GET_VECTOR(A); USP_GET_ROTATOR(B); USP_FINISH; US_RESULT(FUSVector) = Unrotate(A, B); }
	US_NATIVE(GreaterGreater_VectorRotator) { USP_GET_VECTOR(A); USP_GET_ROTATOR(B); USP_FINISH; US_RESULT(FUSVector) = Rotate(A, B); }
	US_NATIVE(EqualEqual_VectorVector) { USP_GET_VECTOR(A); USP_GET_VECTOR(B); USP_FINISH; US_RESULT(uint32) = (A.X == B.X && A.Y == B.Y && A.Z == B.Z) ? 1u : 0u; }
	US_NATIVE(NotEqual_VectorVector) { USP_GET_VECTOR(A); USP_GET_VECTOR(B); USP_FINISH; US_RESULT(uint32) = (A.X != B.X || A.Y != B.Y || A.Z != B.Z) ? 1u : 0u; }
	US_NATIVE(Dot_VectorVector) { USP_GET_VECTOR(A); USP_GET_VECTOR(B); USP_FINISH; US_RESULT(float) = A.X * B.X + A.Y * B.Y + A.Z * B.Z; }
	US_NATIVE(Cross_VectorVector)
	{
		USP_GET_VECTOR(A); USP_GET_VECTOR(B); USP_FINISH;
		US_RESULT(FUSVector) = FUSVector(A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X);
	}
	US_NATIVE(MultiplyEqual_VectorFloat) { USP_GET_VECTOR_REF(A); USP_GET_FLOAT(B); USP_FINISH; *A = *A * B; US_RESULT(FUSVector) = *A; }
	US_NATIVE(MultiplyEqual_VectorVector) { USP_GET_VECTOR_REF(A); USP_GET_VECTOR(B); USP_FINISH; *A = FUSVector(A->X * B.X, A->Y * B.Y, A->Z * B.Z); US_RESULT(FUSVector) = *A; }
	US_NATIVE(DivideEqual_VectorFloat) { USP_GET_VECTOR_REF(A); USP_GET_FLOAT(B); USP_FINISH; *A = B != 0.f ? *A * (1.f / B) : FUSVector(); US_RESULT(FUSVector) = *A; }
	US_NATIVE(AddEqual_VectorVector) { USP_GET_VECTOR_REF(A); USP_GET_VECTOR(B); USP_FINISH; *A = *A + B; US_RESULT(FUSVector) = *A; }
	US_NATIVE(SubtractEqual_VectorVector) { USP_GET_VECTOR_REF(A); USP_GET_VECTOR(B); USP_FINISH; *A = *A - B; US_RESULT(FUSVector) = *A; }
	US_NATIVE(VSize) { USP_GET_VECTOR(A); USP_FINISH; US_RESULT(float) = A.Size(); }
	US_NATIVE(Normal) { USP_GET_VECTOR(A); USP_FINISH; US_RESULT(FUSVector) = SafeNormal(A); }
	US_NATIVE(Invert)
	{
		USP_GET_VECTOR_REF(X); USP_GET_VECTOR_REF(Y); USP_GET_VECTOR_REF(Z); USP_FINISH;
		FMatrix M(X->ToFVector(), Y->ToFVector(), Z->ToFVector(), FVector::ZeroVector);
		const FMatrix I = M.Inverse();
		*X = FUSVector(I.GetScaledAxis(EAxis::X));
		*Y = FUSVector(I.GetScaledAxis(EAxis::Y));
		*Z = FUSVector(I.GetScaledAxis(EAxis::Z));
	}
	US_NATIVE(VRand) { USP_FINISH; US_RESULT(FUSVector) = FUSVector(FMath::VRand()); }
	US_NATIVE(MirrorVectorByNormal)
	{
		USP_GET_VECTOR(V); USP_GET_VECTOR(N); USP_FINISH;
		N = SafeNormal(N);
		const float D = V.X * N.X + V.Y * N.Y + V.Z * N.Z;
		US_RESULT(FUSVector) = V - N * (2.f * D);
	}

	// ---- rotator ----
	US_NATIVE(EqualEqual_RotatorRotator) { USP_GET_ROTATOR(A); USP_GET_ROTATOR(B); USP_FINISH; US_RESULT(uint32) = (A.Pitch == B.Pitch && A.Yaw == B.Yaw && A.Roll == B.Roll) ? 1u : 0u; }
	US_NATIVE(NotEqual_RotatorRotator) { USP_GET_ROTATOR(A); USP_GET_ROTATOR(B); USP_FINISH; US_RESULT(uint32) = (A.Pitch != B.Pitch || A.Yaw != B.Yaw || A.Roll != B.Roll) ? 1u : 0u; }
	US_NATIVE(Multiply_RotatorFloat) { USP_GET_ROTATOR(A); USP_GET_FLOAT(B); USP_FINISH; US_RESULT(FUSRotator) = FUSRotator(int32(A.Pitch * B), int32(A.Yaw * B), int32(A.Roll * B)); }
	US_NATIVE(Multiply_FloatRotator) { USP_GET_FLOAT(A); USP_GET_ROTATOR(B); USP_FINISH; US_RESULT(FUSRotator) = FUSRotator(int32(B.Pitch * A), int32(B.Yaw * A), int32(B.Roll * A)); }
	US_NATIVE(Divide_RotatorFloat)
	{
		USP_GET_ROTATOR(A); USP_GET_FLOAT(B); USP_FINISH;
		US_RESULT(FUSRotator) = B != 0.f ? FUSRotator(int32(A.Pitch / B), int32(A.Yaw / B), int32(A.Roll / B)) : FUSRotator();
	}
	US_NATIVE(MultiplyEqual_RotatorFloat) { USP_GET_ROTATOR_REF(A); USP_GET_FLOAT(B); USP_FINISH; *A = FUSRotator(int32(A->Pitch * B), int32(A->Yaw * B), int32(A->Roll * B)); US_RESULT(FUSRotator) = *A; }
	US_NATIVE(DivideEqual_RotatorFloat)
	{
		USP_GET_ROTATOR_REF(A); USP_GET_FLOAT(B); USP_FINISH;
		if (B != 0.f) { *A = FUSRotator(int32(A->Pitch / B), int32(A->Yaw / B), int32(A->Roll / B)); }
		US_RESULT(FUSRotator) = *A;
	}
	US_NATIVE(Add_RotatorRotator) { USP_GET_ROTATOR(A); USP_GET_ROTATOR(B); USP_FINISH; US_RESULT(FUSRotator) = FUSRotator(A.Pitch + B.Pitch, A.Yaw + B.Yaw, A.Roll + B.Roll); }
	US_NATIVE(Subtract_RotatorRotator) { USP_GET_ROTATOR(A); USP_GET_ROTATOR(B); USP_FINISH; US_RESULT(FUSRotator) = FUSRotator(A.Pitch - B.Pitch, A.Yaw - B.Yaw, A.Roll - B.Roll); }
	US_NATIVE(AddEqual_RotatorRotator) { USP_GET_ROTATOR_REF(A); USP_GET_ROTATOR(B); USP_FINISH; *A = FUSRotator(A->Pitch + B.Pitch, A->Yaw + B.Yaw, A->Roll + B.Roll); US_RESULT(FUSRotator) = *A; }
	US_NATIVE(SubtractEqual_RotatorRotator) { USP_GET_ROTATOR_REF(A); USP_GET_ROTATOR(B); USP_FINISH; *A = FUSRotator(A->Pitch - B.Pitch, A->Yaw - B.Yaw, A->Roll - B.Roll); US_RESULT(FUSRotator) = *A; }
	US_NATIVE(GetAxes)
	{
		USP_GET_ROTATOR(A); USP_GET_VECTOR_REF(X); USP_GET_VECTOR_REF(Y); USP_GET_VECTOR_REF(Z); USP_FINISH;
		*X = Rotate(FUSVector(1, 0, 0), A);
		*Y = Rotate(FUSVector(0, 1, 0), A);
		*Z = Rotate(FUSVector(0, 0, 1), A);
	}
	US_NATIVE(GetUnAxes)
	{
		USP_GET_ROTATOR(A); USP_GET_VECTOR_REF(X); USP_GET_VECTOR_REF(Y); USP_GET_VECTOR_REF(Z); USP_FINISH;
		*X = Unrotate(FUSVector(1, 0, 0), A);
		*Y = Unrotate(FUSVector(0, 1, 0), A);
		*Z = Unrotate(FUSVector(0, 0, 1), A);
	}
	US_NATIVE(RotRand)
	{
		USP_GET_UBOOL_OPTX(bRoll, false); USP_FINISH;
		FUSRotator R(FMath::RandHelper(65536), FMath::RandHelper(65536), bRoll ? FMath::RandHelper(65536) : 0);
		US_RESULT(FUSRotator) = R;
	}
	US_NATIVE(OrthoRotation)
	{
		USP_GET_VECTOR(X); USP_GET_VECTOR(Y); USP_GET_VECTOR(Z); USP_FINISH;
		const FMatrix M(X.ToFVector(), Y.ToFVector(), Z.ToFVector(), FVector::ZeroVector);
		US_RESULT(FUSRotator) = FromFRotator(M.Rotator());
	}
	US_NATIVE(Normalize)
	{
		USP_GET_ROTATOR(R); USP_FINISH;
		auto N = [](int32 V) { V &= 0xFFFF; return V > 32767 ? V - 65536 : V; };
		US_RESULT(FUSRotator) = FUSRotator(N(R.Pitch), N(R.Yaw), N(R.Roll));
	}

	// ---- quaternions / curves ----
	US_NATIVE(QuatProduct) { USP_GET_STRUCT(FUSQuat, A); USP_GET_STRUCT(FUSQuat, B); USP_FINISH; US_RESULT(FUSQuat) = FromFQuat(ToFQuat(A) * ToFQuat(B)); }
	US_NATIVE(QuatInvert) { USP_GET_STRUCT(FUSQuat, A); USP_FINISH; US_RESULT(FUSQuat) = FromFQuat(ToFQuat(A).Inverse()); }
	US_NATIVE(QuatRotateVector) { USP_GET_STRUCT(FUSQuat, A); USP_GET_VECTOR(B); USP_FINISH; US_RESULT(FUSVector) = FUSVector(ToFQuat(A).RotateVector(B.ToFVector())); }
	US_NATIVE(QuatFindBetween) { USP_GET_VECTOR(A); USP_GET_VECTOR(B); USP_FINISH; US_RESULT(FUSQuat) = FromFQuat(FQuat::FindBetweenVectors(A.ToFVector(), B.ToFVector())); }
	US_NATIVE(QuatFromAxisAndAngle) { USP_GET_VECTOR(Axis); USP_GET_FLOAT(Angle); USP_FINISH; US_RESULT(FUSQuat) = FromFQuat(FQuat(SafeNormal(Axis).ToFVector(), Angle)); }
	US_NATIVE(QuatFromRotator) { USP_GET_ROTATOR(A); USP_FINISH; US_RESULT(FUSQuat) = FromFQuat(ToFRotator(A).Quaternion()); }
	US_NATIVE(QuatToRotator) { USP_GET_STRUCT(FUSQuat, A); USP_FINISH; US_RESULT(FUSRotator) = FromFRotator(ToFQuat(A).Rotator()); }
	US_NATIVE(QuatSlerp) { USP_GET_STRUCT(FUSQuat, A); USP_GET_STRUCT(FUSQuat, B); USP_GET_FLOAT(Alpha); USP_FINISH; US_RESULT(FUSQuat) = FromFQuat(FQuat::Slerp(ToFQuat(A), ToFQuat(B), Alpha)); }

	/** InterpCurve { array<InterpCurvePoint {InVal, OutVal}> Points; } */
	struct FCurveTemp
	{
		FUSScriptArray Points;
		~FCurveTemp() { if (Points.Data) { FMemory::Free(Points.Data); } }
		const float* Pt(int32 i) const { return reinterpret_cast<const float*>(Points.Data) + i * 2; }
	};
	US_NATIVE(InterpCurveEval)
	{
		alignas(16) uint8 Buf[64] = {};
		US_STEP_PARM(Buf);
		USP_GET_FLOAT(Input);
		USP_FINISH;
		FCurveTemp C;
		C.Points = *reinterpret_cast<FUSScriptArray*>(Buf);
		float Out = 0.f;
		if (C.Points.Num > 0)
		{
			if (Input <= C.Pt(0)[0]) { Out = C.Pt(0)[1]; }
			else if (Input >= C.Pt(C.Points.Num - 1)[0]) { Out = C.Pt(C.Points.Num - 1)[1]; }
			else
			{
				for (int32 i = 1; i < C.Points.Num; ++i)
				{
					if (Input < C.Pt(i)[0])
					{
						const float* P0 = C.Pt(i - 1);
						const float* P1 = C.Pt(i);
						const float D = P1[0] - P0[0];
						Out = D > 0.f ? P0[1] + (Input - P0[0]) / D * (P1[1] - P0[1]) : P0[1];
						break;
					}
				}
			}
		}
		US_RESULT(float) = Out;
	}
	static void CurveRange(FUSFrame& Stack, int32 Which)
	{
		alignas(16) uint8 Buf[64] = {};
		US_STEP_PARM(Buf);
		USP_GET_FLOAT_REF(Min);
		USP_GET_FLOAT_REF(Max);
		USP_FINISH;
		FCurveTemp C;
		C.Points = *reinterpret_cast<FUSScriptArray*>(Buf);
		*Min = 0.f;
		*Max = 0.f;
		for (int32 i = 0; i < C.Points.Num; ++i)
		{
			const float V = C.Pt(i)[Which];
			*Min = i == 0 ? V : FMath::Min(*Min, V);
			*Max = i == 0 ? V : FMath::Max(*Max, V);
		}
	}
	US_NATIVE(InterpCurveGetOutputRange) { CurveRange(Stack, 1); }
	US_NATIVE(InterpCurveGetInputDomain) { CurveRange(Stack, 0); }

	// ---- strings ----
	US_NATIVE(Concat_StrStr) { USP_GET_STR(A); USP_GET_STR(B); USP_FINISH; US_RESULT(FString) = A + B; }
	US_NATIVE(At_StrStr) { USP_GET_STR(A); USP_GET_STR(B); USP_FINISH; US_RESULT(FString) = A + TEXT(" ") + B; }
#define STR_CMP(Name, Expr) US_NATIVE(Name) { USP_GET_STR(A); USP_GET_STR(B); USP_FINISH; const int32 C = FCString::Strcmp(*A, *B); US_RESULT(uint32) = (Expr) ? 1u : 0u; }
	STR_CMP(Less_StrStr, C < 0)
	STR_CMP(Greater_StrStr, C > 0)
	STR_CMP(LessEqual_StrStr, C <= 0)
	STR_CMP(GreaterEqual_StrStr, C >= 0)
	STR_CMP(EqualEqual_StrStr, C == 0)
	STR_CMP(NotEqual_StrStr, C != 0)
	US_NATIVE(ComplementEqual_StrStr) { USP_GET_STR(A); USP_GET_STR(B); USP_FINISH; US_RESULT(uint32) = A.Equals(B, ESearchCase::IgnoreCase) ? 1u : 0u; }
	US_NATIVE(Len) { USP_GET_STR(S); USP_FINISH; US_RESULT(int32) = S.Len(); }
	US_NATIVE(InStr) { USP_GET_STR(S); USP_GET_STR(T); USP_FINISH; US_RESULT(int32) = T.IsEmpty() ? 0 : S.Find(T, ESearchCase::CaseSensitive); }
	US_NATIVE(Mid) { USP_GET_STR(S); USP_GET_INT(I); USP_GET_INT_OPTX(J, 65535); USP_FINISH; US_RESULT(FString) = UMid(S, I, J); }
	US_NATIVE(Left) { USP_GET_STR(S); USP_GET_INT(I); USP_FINISH; US_RESULT(FString) = ULeft(S, I); }
	US_NATIVE(Right) { USP_GET_STR(S); USP_GET_INT(I); USP_FINISH; US_RESULT(FString) = URight(S, I); }
	US_NATIVE(Caps) { USP_GET_STR(S); USP_FINISH; US_RESULT(FString) = S.ToUpper(); }
	US_NATIVE(Locs) { USP_GET_STR(S); USP_FINISH; US_RESULT(FString) = S.ToLower(); }
	US_NATIVE(Chr) { USP_GET_INT(I); USP_FINISH; FString S; if (I != 0) { S.AppendChar(TCHAR(I & 0xFFFF)); } US_RESULT(FString) = S; }
	US_NATIVE(Asc) { USP_GET_STR(S); USP_FINISH; US_RESULT(int32) = S.Len() ? int32(S[0]) : 0; }
	US_NATIVE(Divide)
	{
		USP_GET_STR(Src); USP_GET_STR(Div); USP_GET_STR_REF(L); USP_GET_STR_REF(R); USP_FINISH;
		const int32 I = Div.IsEmpty() ? INDEX_NONE : Src.Find(Div, ESearchCase::CaseSensitive);
		if (I == INDEX_NONE)
		{
			US_RESULT(uint32) = 0;
			return;
		}
		*L = Src.Left(I);
		*R = Src.Mid(I + Div.Len());
		US_RESULT(uint32) = 1;
	}
	US_NATIVE(Split)
	{
		USP_GET_STR(Src); USP_GET_STR(Div); USP_GET_ARRAY_REF(Parts, PartsProp); USP_FINISH;
		TArray<FString> Out;
		if (!Div.IsEmpty())
		{
			FString S = Src;
			int32 I;
			while ((I = S.Find(Div, ESearchCase::CaseSensitive)) != INDEX_NONE)
			{
				Out.Add(S.Left(I));
				S = S.Mid(I + Div.Len());
			}
			Out.Add(S);
		}
		else
		{
			Out.Add(Src);
		}
		SetStringArray(Parts, PartsProp, Out);
		US_RESULT(int32) = Out.Num();
	}
	US_NATIVE(Repl)
	{
		USP_GET_STR(Src); USP_GET_STR(Match); USP_GET_STR(With); USP_GET_UBOOL_OPTX(bCase, false); USP_FINISH;
		US_RESULT(FString) = Match.IsEmpty() ? Src : Src.Replace(*Match, *With, bCase ? ESearchCase::CaseSensitive : ESearchCase::IgnoreCase);
	}
	US_NATIVE(StrCmp)
	{
		USP_GET_STR(S); USP_GET_STR(T); USP_GET_INT_OPTX(Count, 0); USP_GET_UBOOL_OPTX(bCase, false); USP_FINISH;
		int32 R;
		if (Count > 0)
		{
			R = bCase ? FCString::Strncmp(*S, *T, Count) : FCString::Strnicmp(*S, *T, Count);
		}
		else
		{
			R = bCase ? FCString::Strcmp(*S, *T) : FCString::Stricmp(*S, *T);
		}
		US_RESULT(int32) = R;
	}
	US_NATIVE(Eval) { USP_GET_UBOOL(C); USP_GET_STR(A); USP_GET_STR(B); USP_FINISH; US_RESULT(FString) = C ? A : B; }
	US_NATIVE(SubtractEqual_StrStr) { USP_GET_STR_REF(A); USP_GET_STR(B); USP_FINISH; if (!B.IsEmpty()) { *A = A->Replace(*B, TEXT(""), ESearchCase::CaseSensitive); } US_RESULT(FString) = *A; }
	US_NATIVE(AtEqual_StrStr) { USP_GET_STR_REF(A); USP_GET_STR(B); USP_FINISH; *A = *A + TEXT(" ") + B; US_RESULT(FString) = *A; }
	US_NATIVE(ConcatEqual_StrStr) { USP_GET_STR_REF(A); USP_GET_STR(B); USP_FINISH; *A += B; US_RESULT(FString) = *A; }

	// ---- objects / names ----
	US_NATIVE(EqualEqual_ObjectObject) { USP_GET_OBJECT(A); USP_GET_OBJECT(B); USP_FINISH; US_RESULT(uint32) = Live(A) == Live(B) ? 1u : 0u; }
	US_NATIVE(NotEqual_ObjectObject) { USP_GET_OBJECT(A); USP_GET_OBJECT(B); USP_FINISH; US_RESULT(uint32) = Live(A) != Live(B) ? 1u : 0u; }
	US_NATIVE(EqualEqual_NameName) { USP_GET_NAME(A); USP_GET_NAME(B); USP_FINISH; US_RESULT(uint32) = A == B ? 1u : 0u; }
	US_NATIVE(NotEqual_NameName) { USP_GET_NAME(A); USP_GET_NAME(B); USP_FINISH; US_RESULT(uint32) = A != B ? 1u : 0u; }

	US_NATIVE(Log)
	{
		USP_GET_STR(S); USP_GET_NAME_OPTX(Tag, NAME_None); USP_FINISH;
		UE_LOG(LogUScript, Log, TEXT("%s: %s"), Tag == NAME_None ? TEXT("ScriptLog") : *Tag.ToString(), *S);
	}
	US_NATIVE(Warn)
	{
		USP_GET_STR(S); USP_FINISH;
		UE_LOG(LogUScript, Warning, TEXT("ScriptWarning: %s %s"), *Stack.Describe(), *S);
	}
	US_NATIVE(Localize)
	{
		USP_GET_STR(Section); USP_GET_STR(Key); USP_GET_STR(Package); USP_FINISH;
		FString V;
		if (!FUSRuntime::Get().GetLocalized(Package, Section, Key, V))
		{
			V = FString::Printf(TEXT("<?int?%s.%s.%s?>"), *Package, *Section, *Key);
		}
		US_RESULT(FString) = V;
	}

	// ---- states ----
	US_NATIVE(GotoState)
	{
		USP_GET_NAME_OPTX(S, NAME_None); USP_GET_NAME_OPTX(L, NAME_None); USP_FINISH;
		if (!Self || !Self->StateFrame)
		{
			return;
		}
		const FName Current = Self->StateFrame->StateNode ? Self->StateFrame->StateNode->Name : NAME_None;
		if (S != Current || S == NAME_None)
		{
			if (!USVM::GotoState(Self, S, L) && S != NAME_None && S != FName(TEXT("Auto")))
			{
				USVM::ScriptWarn(&Stack, FString::Printf(TEXT("GotoState (%s %s): state not found"), *S.ToString(), *L.ToString()));
			}
		}
		else
		{
			USGotoLabel(Self, L == NAME_None ? FName(TEXT("Begin")) : L);
		}
	}
	US_NATIVE(IsInState)
	{
		USP_GET_NAME(N); USP_FINISH;
		bool bIn = false;
		if (Self && Self->StateFrame)
		{
			int32 Guard = 0;
			for (FUSStruct* S = Self->StateFrame->StateNode; S && Guard < 64; S = S->GetSuperStruct(), ++Guard)
			{
				if (S->Name == N)
				{
					bIn = true;
					break;
				}
				if (!S->Class || S->Class->Name != TEXT("State"))
				{
					break;
				}
			}
		}
		US_RESULT(uint32) = bIn ? 1u : 0u;
	}
	US_NATIVE(GetStateName)
	{
		USP_FINISH;
		US_RESULT(FName) = (Self && Self->StateFrame && Self->StateFrame->StateNode) ? Self->StateFrame->StateNode->Name : (Self && Self->Class ? Self->Class->Name : NAME_None);
	}
	US_NATIVE(Enable)
	{
		USP_GET_NAME(N); USP_FINISH;
		if (Self && Self->StateFrame)
		{
			Self->StateFrame->DisabledProbes.Remove(N);
		}
	}
	US_NATIVE(Disable)
	{
		USP_GET_NAME(N); USP_FINISH;
		if (Self && Self->StateFrame)
		{
			Self->StateFrame->DisabledProbes.AddUnique(N);
		}
	}

	// ---- classes / reflection ----
	US_NATIVE(ClassIsChildOf)
	{
		USP_GET_OBJECT(A); USP_GET_OBJECT(B); USP_FINISH;
		const bool bClassA = A && (A->Flags & USF_Field) && A->Class && A->Class->Name == TEXT("Class");
		const bool bClassB = B && (B->Flags & USF_Field) && B->Class && B->Class->Name == TEXT("Class");
		US_RESULT(uint32) = (bClassA && bClassB && static_cast<FUSClass*>(A)->IsChildOf(static_cast<FUSClass*>(B))) ? 1u : 0u;
	}
	US_NATIVE(IsA) { USP_GET_NAME(N); USP_FINISH; US_RESULT(uint32) = (Self && Self->IsA(N)) ? 1u : 0u; }
	US_NATIVE(GetPropertyText)
	{
		USP_GET_STR(Name); USP_FINISH;
		FString Out;
		if (FUSProperty* P = Self ? Self->FindProperty(FName(*Name)) : nullptr)
		{
			if (Self->Data)
			{
				P->ExportText(Out, Self->Data + P->Offset, true);
			}
		}
		US_RESULT(FString) = Out;
	}
	US_NATIVE(SetPropertyText)
	{
		USP_GET_STR(Name); USP_GET_STR(Value); USP_FINISH;
		bool bOk = false;
		if (FUSProperty* P = Self ? Self->FindProperty(FName(*Name)) : nullptr)
		{
			if (Self->Data)
			{
				const TCHAR* In = *Value;
				bOk = P->ImportText(In, Self->Data + P->Offset);
			}
		}
		US_RESULT(uint32) = bOk ? 1u : 0u;
	}
	US_NATIVE(GetEnum)
	{
		USP_GET_OBJECT(E); USP_GET_INT(I); USP_FINISH;
		FName N = NAME_None;
		if (E && (E->Flags & USF_Field) && E->Class && E->Class->Name == TEXT("Enum"))
		{
			const FUSEnum* En = static_cast<FUSEnum*>(E);
			if (En->Names.IsValidIndex(I))
			{
				N = En->Names[I];
			}
		}
		US_RESULT(FName) = N;
	}
	static FUSObject* LoadObjectByName(const FString& Path, FUSObject* ClassObj)
	{
		FUSRuntime& RT = FUSRuntime::Get();
		FUSClass* C = (ClassObj && (ClassObj->Flags & USF_Field)) ? static_cast<FUSClass*>(ClassObj) : nullptr;
		if (C && C->Name == TEXT("Class") && (C->Flags & USF_Intrinsic))
		{
			return RT.FindClass(Path);
		}
		FUSObject* O = RT.FindObject(Path, C);
		if (O && C && (O->Flags & USF_Placeholder) && O->Class && O->Class->Name == TEXT("Package") && C->Name != TEXT("Package"))
		{
			// a group of the same name (a font's page textures are in a group named like the font): the object itself
			O = RT.GetPlaceholder(Path, C);
		}
		if (O && C && !(O->Flags & USF_Placeholder) && !O->IsA(C))
		{
			return nullptr;
		}
		if (!O && C && Path.Contains(TEXT(".")) && RT.ContentObjectExists && RT.ContentObjectExists(Path, C->GetName()))
		{
			// a texture / font / sound the game has (its package is content, not script)
			O = RT.GetPlaceholder(Path, C);
		}
		return O;
	}
	US_NATIVE(DynamicLoadObject)
	{
		USP_GET_STR(Name); USP_GET_OBJECT(ClassObj); USP_GET_UBOOL_OPTX(bMayFail, false); USP_FINISH;
		FUSObject* O = LoadObjectByName(Name, ClassObj);
		if (!O && !bMayFail)
		{
			USVM::ScriptWarn(&Stack, FString::Printf(TEXT("DynamicLoadObject: cannot load %s"), *Name));
		}
		US_RESULT(FUSObject*) = O;
	}
	US_NATIVE(FindObject)
	{
		USP_GET_STR(Name); USP_GET_OBJECT(ClassObj); USP_FINISH;
		US_RESULT(FUSObject*) = LoadObjectByName(Name, ClassObj);
	}

	// ---- config (never written: the player's install is read only) ----
	US_NATIVE(NoOp) { USP_FINISH; }
	/** Object.SaveConfig: this object's config values into our own per-user ini (FUSRuntime::SaveConfig). */
	US_NATIVE(SaveConfig)
	{
		USP_FINISH;
		if (Self && Self->Class && Self->Data)
		{
			FUSRuntime::Get().SaveConfig(Self->Class, Self->Data);
		}
	}
	/** Object.StaticSaveConfig: the class defaults' config values. */
	US_NATIVE(StaticSaveConfig)
	{
		USP_FINISH;
		FUSClass* C = Self && (Self->Flags & USF_Field) ? static_cast<FUSClass*>(Self) : (Self ? Self->Class : nullptr);
		if (C && C->Defaults.Num() >= C->PropertiesSize)
		{
			FUSRuntime::Get().SaveConfig(C, C->Defaults.GetData());
		}
	}
	US_NATIVE(NoOpStr) { USP_GET_STR_OPTX(S, FString()); USP_FINISH; }
	US_NATIVE(StopWatch) { USP_GET_UBOOL_OPTX(B, false); USP_FINISH; }
	US_NATIVE(ReturnFalse) { USP_FINISH; US_RESULT(uint32) = 0; }
	US_NATIVE(PlatformIs64Bit) { USP_FINISH; US_RESULT(uint32) = 1u; } // UE is 64-bit only (PLATFORM_64BITS is deprecated)
	US_NATIVE(PlatformIsWindows) { USP_FINISH; US_RESULT(uint32) = PLATFORM_WINDOWS ? 1u : 0u; }
	US_NATIVE(PlatformIsUnix) { USP_FINISH; US_RESULT(uint32) = (PLATFORM_LINUX || PLATFORM_MAC) ? 1u : 0u; }
	US_NATIVE(PlatformIsMacOS) { USP_FINISH; US_RESULT(uint32) = PLATFORM_MAC ? 1u : 0u; }
	US_NATIVE(GetPerObjectNames)
	{
		USP_GET_STR(Ini); USP_GET_STR_OPTX(Cls, FString()); USP_GET_INT_OPTX(Max, 1024); USP_FINISH;
		// empty array result (zeroed by the caller)
	}
	US_NATIVE(GetReferencers)
	{
		USP_GET_OBJECT(Target); USP_GET_ARRAY_REF(Refs, RefsProp); USP_FINISH;
	}
	US_NATIVE(AllObjects)
	{
		USP_GET_OBJECT(BaseClass); USP_GET_OBJECT_REF(Out); USP_FINISH;
		FUSClass* C = static_cast<FUSClass*>(BaseClass);
		FUSWorld* W = (Self && Self->World) ? Self->World : &FUSWorld::Transient();
		TArray<FUSObject*> Snapshot = W->GetObjects();
		int32 I = 0;
		USRunIterator(Stack, [&]() -> bool
		{
			while (I < Snapshot.Num())
			{
				FUSObject* O = Snapshot[I++];
				if (Live(O) && (!C || O->IsA(C)))
				{
					*Out = O;
					return true;
				}
			}
			*Out = nullptr;
			return false;
		});
	}
}

void USNatives::RegisterCore()
{
	using namespace USCoreNatives;
#define N(Fn) { TEXT("Object"), TEXT(#Fn), &Fn }
	static const FUSNativeEntry Entries[] = {
		N(DivideEqual_FloatFloat), N(MultiplyEqual_FloatFloat), N(AddEqual_FloatFloat), N(SubtractEqual_FloatFloat),
		N(NotEqual_FloatFloat), N(ComplementEqual_FloatFloat), N(EqualEqual_FloatFloat), N(GreaterEqual_FloatFloat),
		N(LessEqual_FloatFloat), N(Greater_FloatFloat), N(Less_FloatFloat), N(Subtract_FloatFloat), N(Add_FloatFloat),
		N(Multiply_FloatFloat), N(Divide_FloatFloat), N(Percent_FloatFloat), N(MultiplyMultiply_FloatFloat), N(FMin), N(FMax),
		N(Abs), N(Sin), N(Asin), N(Cos), N(Acos), N(Tan), N(Atan), N(Exp), N(Loge), N(Sqrt), N(Square), N(Subtract_PreFloat),
		N(Ceil), N(Round), N(FRand), N(FClamp), N(Lerp), N(Smerp),
		N(Or_IntInt), N(Xor_IntInt), N(And_IntInt), N(GreaterGreaterGreater_IntInt), N(GreaterGreater_IntInt), N(LessLess_IntInt),
		N(Subtract_IntInt), N(Add_IntInt), N(Multiply_IntInt), N(Min), N(Max), N(NotEqual_IntInt), N(EqualEqual_IntInt),
		N(GreaterEqual_IntInt), N(LessEqual_IntInt), N(Greater_IntInt), N(Less_IntInt), N(Divide_IntInt), N(Clamp), N(Rand),
		N(SubtractSubtract_Int), N(AddAdd_Int), N(SubtractSubtract_PreInt), N(AddAdd_PreInt), N(SubtractEqual_IntInt),
		N(AddEqual_IntInt), N(DivideEqual_IntFloat), N(MultiplyEqual_IntFloat), N(Subtract_PreInt), N(Complement_PreInt),
		N(ClockwiseFrom_IntInt),
		N(SubtractSubtract_Byte), N(AddAdd_Byte), N(SubtractSubtract_PreByte), N(AddAdd_PreByte), N(SubtractEqual_ByteByte),
		N(AddEqual_ByteByte), N(DivideEqual_ByteByte), N(MultiplyEqual_ByteByte),
		N(OrOr_BoolBool), N(AndAnd_BoolBool), N(XorXor_BoolBool), N(NotEqual_BoolBool), N(EqualEqual_BoolBool), N(Not_PreBool),
		N(Subtract_PreVector), N(Multiply_VectorFloat), N(Multiply_FloatVector), N(Multiply_VectorVector), N(Divide_VectorFloat),
		N(Add_VectorVector), N(Subtract_VectorVector), N(LessLess_VectorRotator), N(GreaterGreater_VectorRotator),
		N(EqualEqual_VectorVector), N(NotEqual_VectorVector), N(Dot_VectorVector), N(Cross_VectorVector),
		N(MultiplyEqual_VectorFloat), N(MultiplyEqual_VectorVector), N(DivideEqual_VectorFloat), N(AddEqual_VectorVector),
		N(SubtractEqual_VectorVector), N(VSize), N(Normal), N(Invert), N(VRand), N(MirrorVectorByNormal),
		N(EqualEqual_RotatorRotator), N(NotEqual_RotatorRotator), N(Multiply_RotatorFloat), N(Multiply_FloatRotator),
		N(Divide_RotatorFloat), N(MultiplyEqual_RotatorFloat), N(DivideEqual_RotatorFloat), N(Add_RotatorRotator),
		N(Subtract_RotatorRotator), N(AddEqual_RotatorRotator), N(SubtractEqual_RotatorRotator), N(GetAxes), N(GetUnAxes),
		N(RotRand), N(OrthoRotation), N(Normalize),
		N(QuatProduct), N(QuatInvert), N(QuatRotateVector), N(QuatFindBetween), N(QuatFromAxisAndAngle), N(QuatFromRotator),
		N(QuatToRotator), N(QuatSlerp), N(InterpCurveEval), N(InterpCurveGetOutputRange), N(InterpCurveGetInputDomain),
		N(Concat_StrStr), N(At_StrStr), N(Less_StrStr), N(Greater_StrStr), N(LessEqual_StrStr), N(GreaterEqual_StrStr),
		N(EqualEqual_StrStr), N(NotEqual_StrStr), N(ComplementEqual_StrStr), N(Len), N(InStr), N(Mid), N(Left), N(Right),
		N(Caps), N(Locs), N(Chr), N(Asc), N(Divide), N(Split), N(Repl), N(StrCmp), N(Eval), N(SubtractEqual_StrStr),
		N(AtEqual_StrStr), N(ConcatEqual_StrStr),
		N(EqualEqual_ObjectObject), N(NotEqual_ObjectObject), N(EqualEqual_NameName), N(NotEqual_NameName),
		N(Log), N(Warn), N(Localize), N(GotoState), N(IsInState), N(GetStateName), N(Enable), N(Disable),
		N(ClassIsChildOf), N(IsA), N(GetPropertyText), N(SetPropertyText), N(GetEnum), N(DynamicLoadObject), N(FindObject),
		N(StopWatch), N(PlatformIs64Bit), N(PlatformIsWindows), N(PlatformIsUnix), N(PlatformIsMacOS), N(GetPerObjectNames),
		N(GetReferencers), N(AllObjects),
		{ TEXT("Object"), TEXT("SaveConfig"), &SaveConfig }, { TEXT("Object"), TEXT("StaticSaveConfig"), &StaticSaveConfig },
		{ TEXT("Object"), TEXT("ResetConfig"), &NoOpStr }, { TEXT("Object"), TEXT("ClearConfig"), &NoOpStr },
		{ TEXT("Object"), TEXT("StaticClearConfig"), &NoOpStr }, { TEXT("Object"), TEXT("IsOnConsole"), &ReturnFalse },
		{ TEXT("Object"), TEXT("IsSoaking"), &ReturnFalse },
	};
#undef N
	Register(Entries, UE_ARRAY_COUNT(Entries));
}
#endif // OPENUT2K4_USCRIPT_ENABLED
