#pragma once

#include "CoreMinimal.h"

/**
 * Small ordered JSON value for the content converter.
 *
 * The converter writes the same json files as the Python exporter (the Tools/ut2k4 modules): objects keep their
 * insertion order and numbers keep the int / float distinction, so the files read the same as the
 * reference output. UE's FJsonObject is not used for writing because it neither keeps the int / float
 * distinction nor is it fast enough for the hundreds of thousands of files of a full conversion.
 */
struct FConvJson
{
	enum class EType : uint8 { Null, Bool, Int, Float, String, Array, Object };

	EType Type = EType::Null;
	bool B = false;
	int64 I = 0;
	double F = 0.0;
	FString S;
	TArray<FConvJson> A;
	TArray<TPair<FString, FConvJson>> O;

	FConvJson() = default;
	FConvJson(bool In) : Type(EType::Bool), B(In) {}
	FConvJson(int32 In) : Type(EType::Int), I(In) {}
	FConvJson(uint32 In) : Type(EType::Int), I(In) {}
	FConvJson(int64 In) : Type(EType::Int), I(In) {}
	FConvJson(double In) : Type(EType::Float), F(In) {}
	FConvJson(float In) : Type(EType::Float), F(In) {}
	FConvJson(const FString& In) : Type(EType::String), S(In) {}
	FConvJson(FString&& In) : Type(EType::String), S(MoveTemp(In)) {}
	FConvJson(const TCHAR* In) : Type(EType::String), S(In) {}

	static FConvJson Null() { return FConvJson(); }
	static FConvJson Array() { FConvJson J; J.Type = EType::Array; return J; }
	static FConvJson Object() { FConvJson J; J.Type = EType::Object; return J; }
	/** String or null when empty. */
	static FConvJson StrOrNull(const FString& In) { return In.IsEmpty() ? FConvJson() : FConvJson(In); }
	/** Python round(x, Digits) as a float value. */
	static FConvJson Round(double In, int32 Digits) { return FConvJson(RoundTo(In, Digits)); }

	bool IsNull() const { return Type == EType::Null; }
	bool IsNumber() const { return Type == EType::Int || Type == EType::Float; }
	double AsNumber(double Default = 0.0) const { return Type == EType::Int ? double(I) : Type == EType::Float ? F : Type == EType::Bool ? (B ? 1.0 : 0.0) : Default; }
	/** Python truthiness. */
	bool Truthy() const;

	// ---- objects ----
	/** Sets (or replaces in place, keeping the position) a key. */
	FConvJson& Set(const FString& Key, FConvJson Value);
	const FConvJson* Find(const FString& Key) const;
	FConvJson* Find(const FString& Key);
	/** Member or a shared null value. */
	const FConvJson& Get(const FString& Key) const;
	bool Has(const FString& Key) const { return Find(Key) != nullptr; }
	bool Remove(const FString& Key);
	FString GetString(const FString& Key, const FString& Default = FString()) const;
	double GetNumber(const FString& Key, double Default = 0.0) const;

	// ---- arrays ----
	FConvJson& Add(FConvJson Value) { A.Add(MoveTemp(Value)); return A.Last(); }
	int32 Num() const { return Type == EType::Array ? A.Num() : Type == EType::Object ? O.Num() : 0; }

	/** Python json.dump(indent=Indent) text (Indent < 0: compact with ',' / ':' separators). UTF-8. */
	void Write(TArray<ANSICHAR>& Out, int32 Indent = 1) const;
	bool SaveToFile(const FString& Path, int32 Indent = 1) const;

	static bool Parse(const ANSICHAR* Text, int32 Len, FConvJson& Out, FString* OutError = nullptr);
	static bool LoadFile(const FString& Path, FConvJson& Out, FString* OutError = nullptr);

	/** Python round(Value, Digits) (correctly rounded decimal). */
	static double RoundTo(double Value, int32 Digits);
	/** Python repr(float): shortest text that reads back to the same double. */
	static void AppendDouble(TArray<ANSICHAR>& Out, double Value);

private:
	void WriteImpl(TArray<ANSICHAR>& Out, int32 Indent, int32 Depth) const;
};

/** Writes a buffer to a file, creating the folder. Thread-safe. */
bool ConvWriteFile(const FString& Path, const void* Data, int64 Size);
