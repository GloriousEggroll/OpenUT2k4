#include "ArenaConvJson.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

namespace ArenaConvJsonLocal
{
	static const FConvJson NullValue;

	void AppendStr(TArray<ANSICHAR>& Out, const char* S)
	{
		Out.Append(S, FCStringAnsi::Strlen(S));
	}

	void AppendIndent(TArray<ANSICHAR>& Out, int32 Indent, int32 Depth)
	{
		Out.Add('\n');
		const int32 N = Indent * Depth;
		for (int32 i = 0; i < N; ++i)
		{
			Out.Add(' ');
		}
	}

	void AppendQuoted(TArray<ANSICHAR>& Out, const FString& S)
	{
		Out.Add('"');
		const TCHAR* P = *S;
		const int32 Len = S.Len();
		for (int32 i = 0; i < Len; ++i)
		{
			uint32 C = (uint32)P[i];
			// UTF-16 surrogate pair (TCHAR may be 16 bit)
			if (sizeof(TCHAR) == 2 && C >= 0xD800 && C <= 0xDBFF && i + 1 < Len)
			{
				const uint32 Lo = (uint32)P[i + 1];
				if (Lo >= 0xDC00 && Lo <= 0xDFFF)
				{
					C = 0x10000 + ((C - 0xD800) << 10) + (Lo - 0xDC00);
					++i;
				}
			}
			switch (C)
			{
			case '"': AppendStr(Out, "\\\""); continue;
			case '\\': AppendStr(Out, "\\\\"); continue;
			case '\n': AppendStr(Out, "\\n"); continue;
			case '\r': AppendStr(Out, "\\r"); continue;
			case '\t': AppendStr(Out, "\\t"); continue;
			case '\b': AppendStr(Out, "\\b"); continue;
			case '\f': AppendStr(Out, "\\f"); continue;
			default: break;
			}
			if (C < 0x20)
			{
				char Buf[8];
				snprintf(Buf, sizeof(Buf), "\\u%04x", C);
				AppendStr(Out, Buf);
			}
			else if (C < 0x80)
			{
				Out.Add((ANSICHAR)C);
			}
			else if (C < 0x800)
			{
				Out.Add((ANSICHAR)(0xC0 | (C >> 6)));
				Out.Add((ANSICHAR)(0x80 | (C & 0x3F)));
			}
			else if (C < 0x10000)
			{
				Out.Add((ANSICHAR)(0xE0 | (C >> 12)));
				Out.Add((ANSICHAR)(0x80 | ((C >> 6) & 0x3F)));
				Out.Add((ANSICHAR)(0x80 | (C & 0x3F)));
			}
			else
			{
				Out.Add((ANSICHAR)(0xF0 | (C >> 18)));
				Out.Add((ANSICHAR)(0x80 | ((C >> 12) & 0x3F)));
				Out.Add((ANSICHAR)(0x80 | ((C >> 6) & 0x3F)));
				Out.Add((ANSICHAR)(0x80 | (C & 0x3F)));
			}
		}
		Out.Add('"');
	}

	// ---- parser ----
	struct FParser
	{
		const ANSICHAR* P;
		const ANSICHAR* End;
		FString Error;

		void Ws()
		{
			while (P < End && (*P == ' ' || *P == '\n' || *P == '\r' || *P == '\t'))
			{
				++P;
			}
		}

		bool Fail(const TCHAR* Msg)
		{
			if (Error.IsEmpty())
			{
				Error = Msg;
			}
			return false;
		}

		bool Lit(const char* S)
		{
			const int32 N = FCStringAnsi::Strlen(S);
			if (End - P >= N && FCStringAnsi::Strncmp(P, S, N) == 0)
			{
				P += N;
				return true;
			}
			return false;
		}

		static int32 Hex(ANSICHAR C)
		{
			return C >= '0' && C <= '9' ? C - '0' : C >= 'a' && C <= 'f' ? C - 'a' + 10 : C >= 'A' && C <= 'F' ? C - 'A' + 10 : -1;
		}

		bool ParseString(FString& Out)
		{
			++P; // opening quote
			TArray<TCHAR> Buf;
			while (P < End && *P != '"')
			{
				uint32 C = (uint8)*P++;
				if (C == '\\')
				{
					if (P >= End)
					{
						return Fail(TEXT("bad escape"));
					}
					const ANSICHAR E = *P++;
					switch (E)
					{
					case 'n': C = '\n'; break;
					case 'r': C = '\r'; break;
					case 't': C = '\t'; break;
					case 'b': C = '\b'; break;
					case 'f': C = '\f'; break;
					case '/': C = '/'; break;
					case '\\': C = '\\'; break;
					case '"': C = '"'; break;
					case 'u':
					{
						if (End - P < 4)
						{
							return Fail(TEXT("bad \\u escape"));
						}
						C = 0;
						for (int32 k = 0; k < 4; ++k)
						{
							const int32 H = Hex(P[k]);
							if (H < 0)
							{
								return Fail(TEXT("bad \\u escape"));
							}
							C = C * 16 + H;
						}
						P += 4;
						// surrogate pair
						if (C >= 0xD800 && C <= 0xDBFF && End - P >= 6 && P[0] == '\\' && P[1] == 'u')
						{
							uint32 Lo = 0;
							bool bOk = true;
							for (int32 k = 0; k < 4; ++k)
							{
								const int32 H = Hex(P[2 + k]);
								bOk &= H >= 0;
								Lo = Lo * 16 + FMath::Max(H, 0);
							}
							if (bOk && Lo >= 0xDC00 && Lo <= 0xDFFF)
							{
								P += 6;
								C = 0x10000 + ((C - 0xD800) << 10) + (Lo - 0xDC00);
							}
						}
						break;
					}
					default: return Fail(TEXT("bad escape"));
					}
				}
				else if (C >= 0x80)
				{
					// UTF-8 sequence
					int32 Extra = C >= 0xF0 ? 3 : C >= 0xE0 ? 2 : C >= 0xC0 ? 1 : 0;
					C &= Extra == 3 ? 0x07 : Extra == 2 ? 0x0F : Extra == 1 ? 0x1F : 0x7F;
					while (Extra-- > 0 && P < End)
					{
						C = (C << 6) | ((uint8)*P++ & 0x3F);
					}
				}
				if (sizeof(TCHAR) == 2 && C >= 0x10000)
				{
					C -= 0x10000;
					Buf.Add((TCHAR)(0xD800 + (C >> 10)));
					Buf.Add((TCHAR)(0xDC00 + (C & 0x3FF)));
				}
				else
				{
					Buf.Add((TCHAR)C);
				}
			}
			if (P >= End)
			{
				return Fail(TEXT("unterminated string"));
			}
			++P;
			Buf.Add(0);
			Out = FString(Buf.GetData());
			return true;
		}

		bool ParseValue(FConvJson& Out, int32 Depth)
		{
			if (Depth > 512)
			{
				return Fail(TEXT("too deep"));
			}
			Ws();
			if (P >= End)
			{
				return Fail(TEXT("unexpected end"));
			}
			const ANSICHAR C = *P;
			if (C == '{')
			{
				Out = FConvJson::Object();
				++P;
				Ws();
				if (P < End && *P == '}')
				{
					++P;
					return true;
				}
				while (true)
				{
					Ws();
					if (P >= End || *P != '"')
					{
						return Fail(TEXT("expected key"));
					}
					FString Key;
					if (!ParseString(Key))
					{
						return false;
					}
					Ws();
					if (P >= End || *P != ':')
					{
						return Fail(TEXT("expected ':'"));
					}
					++P;
					FConvJson V;
					if (!ParseValue(V, Depth + 1))
					{
						return false;
					}
					Out.O.Emplace(MoveTemp(Key), MoveTemp(V));
					Ws();
					if (P < End && *P == ',')
					{
						++P;
						continue;
					}
					if (P < End && *P == '}')
					{
						++P;
						return true;
					}
					return Fail(TEXT("expected ',' or '}'"));
				}
			}
			if (C == '[')
			{
				Out = FConvJson::Array();
				++P;
				Ws();
				if (P < End && *P == ']')
				{
					++P;
					return true;
				}
				while (true)
				{
					FConvJson V;
					if (!ParseValue(V, Depth + 1))
					{
						return false;
					}
					Out.A.Add(MoveTemp(V));
					Ws();
					if (P < End && *P == ',')
					{
						++P;
						continue;
					}
					if (P < End && *P == ']')
					{
						++P;
						return true;
					}
					return Fail(TEXT("expected ',' or ']'"));
				}
			}
			if (C == '"')
			{
				Out.Type = FConvJson::EType::String;
				return ParseString(Out.S);
			}
			if (Lit("true")) { Out = FConvJson(true); return true; }
			if (Lit("false")) { Out = FConvJson(false); return true; }
			if (Lit("null")) { Out = FConvJson(); return true; }
			if (Lit("NaN")) { Out = FConvJson(double(NAN)); return true; }
			if (Lit("Infinity")) { Out = FConvJson(double(INFINITY)); return true; }
			if (Lit("-Infinity")) { Out = FConvJson(double(-INFINITY)); return true; }
			// number
			const ANSICHAR* Start = P;
			bool bFloat = false;
			if (P < End && (*P == '-' || *P == '+'))
			{
				++P;
			}
			while (P < End && ((*P >= '0' && *P <= '9') || *P == '.' || *P == 'e' || *P == 'E' || *P == '-' || *P == '+'))
			{
				bFloat |= *P == '.' || *P == 'e' || *P == 'E';
				++P;
			}
			if (P == Start)
			{
				return Fail(TEXT("unexpected character"));
			}
			char Buf[64];
			const int32 N = FMath::Min<int32>(int32(P - Start), 63);
			FMemory::Memcpy(Buf, Start, N);
			Buf[N] = 0;
			if (bFloat)
			{
				Out = FConvJson(strtod(Buf, nullptr));
			}
			else
			{
				Out = FConvJson((int64)strtoll(Buf, nullptr, 10));
			}
			return true;
		}
	};
}

bool FConvJson::Truthy() const
{
	switch (Type)
	{
	case EType::Null: return false;
	case EType::Bool: return B;
	case EType::Int: return I != 0;
	case EType::Float: return F != 0.0;
	case EType::String: return !S.IsEmpty();
	case EType::Array: return A.Num() > 0;
	case EType::Object: return O.Num() > 0;
	}
	return false;
}

FConvJson& FConvJson::Set(const FString& Key, FConvJson Value)
{
	if (Type != EType::Object)
	{
		*this = Object();
	}
	for (TPair<FString, FConvJson>& KV : O)
	{
		if (KV.Key == Key)
		{
			KV.Value = MoveTemp(Value);
			return KV.Value;
		}
	}
	O.Emplace(Key, MoveTemp(Value));
	return O.Last().Value;
}

const FConvJson* FConvJson::Find(const FString& Key) const
{
	if (Type == EType::Object)
	{
		for (const TPair<FString, FConvJson>& KV : O)
		{
			if (KV.Key.Equals(Key, ESearchCase::CaseSensitive))
			{
				return &KV.Value;
			}
		}
	}
	return nullptr;
}

FConvJson* FConvJson::Find(const FString& Key)
{
	return const_cast<FConvJson*>(static_cast<const FConvJson*>(this)->Find(Key));
}

const FConvJson& FConvJson::Get(const FString& Key) const
{
	const FConvJson* V = Find(Key);
	return V ? *V : ArenaConvJsonLocal::NullValue;
}

bool FConvJson::Remove(const FString& Key)
{
	for (int32 i = 0; i < O.Num(); ++i)
	{
		if (O[i].Key.Equals(Key, ESearchCase::CaseSensitive))
		{
			O.RemoveAt(i);
			return true;
		}
	}
	return false;
}

FString FConvJson::GetString(const FString& Key, const FString& Default) const
{
	const FConvJson* V = Find(Key);
	return V && V->Type == EType::String ? V->S : Default;
}

double FConvJson::GetNumber(const FString& Key, double Default) const
{
	const FConvJson* V = Find(Key);
	return V && V->IsNumber() ? V->AsNumber() : Default;
}

double FConvJson::RoundTo(double Value, int32 Digits)
{
	if (!FMath::IsFinite(Value))
	{
		return Value;
	}
	if (FMath::Abs(Value) >= 1e15)
	{
		return Value;
	}
	char Buf[512];
	snprintf(Buf, sizeof(Buf), "%.*f", Digits, Value);
	return strtod(Buf, nullptr);
}

void FConvJson::AppendDouble(TArray<ANSICHAR>& Out, double Value)
{
	if (!FMath::IsFinite(Value))
	{
		ArenaConvJsonLocal::AppendStr(Out, "null");
		return;
	}
	char Buf[64];
	for (int32 Prec = 15; Prec <= 17; ++Prec)
	{
		snprintf(Buf, sizeof(Buf), "%.*g", Prec, Value);
		if (strtod(Buf, nullptr) == Value)
		{
			break;
		}
	}
	// Python repr always marks floats: "1.0", "1e-05"
	bool bMarked = false;
	for (const char* C = Buf; *C; ++C)
	{
		if (*C == '.' || *C == 'e' || *C == 'n' || *C == 'i')
		{
			bMarked = true;
			break;
		}
	}
	ArenaConvJsonLocal::AppendStr(Out, Buf);
	if (!bMarked)
	{
		ArenaConvJsonLocal::AppendStr(Out, ".0");
	}
}

void FConvJson::Write(TArray<ANSICHAR>& Out, int32 Indent) const
{
	WriteImpl(Out, Indent, 0);
}

void FConvJson::WriteImpl(TArray<ANSICHAR>& Out, int32 Indent, int32 Depth) const
{
	using namespace ArenaConvJsonLocal;
	switch (Type)
	{
	case EType::Null: AppendStr(Out, "null"); return;
	case EType::Bool: AppendStr(Out, B ? "true" : "false"); return;
	case EType::Int:
	{
		char Buf[32];
		snprintf(Buf, sizeof(Buf), "%lld", (long long)I);
		AppendStr(Out, Buf);
		return;
	}
	case EType::Float: AppendDouble(Out, F); return;
	case EType::String: AppendQuoted(Out, S); return;
	case EType::Array:
	{
		if (A.Num() == 0)
		{
			AppendStr(Out, "[]");
			return;
		}
		Out.Add('[');
		for (int32 i = 0; i < A.Num(); ++i)
		{
			if (i)
			{
				Out.Add(',');
				if (Indent < 0 && Indent != -1)
				{
					Out.Add(' ');
				}
			}
			if (Indent >= 0)
			{
				AppendIndent(Out, Indent, Depth + 1);
			}
			A[i].WriteImpl(Out, Indent, Depth + 1);
		}
		if (Indent >= 0)
		{
			AppendIndent(Out, Indent, Depth);
		}
		Out.Add(']');
		return;
	}
	case EType::Object:
	{
		if (O.Num() == 0)
		{
			AppendStr(Out, "{}");
			return;
		}
		Out.Add('{');
		for (int32 i = 0; i < O.Num(); ++i)
		{
			if (i)
			{
				Out.Add(',');
			}
			if (Indent >= 0)
			{
				AppendIndent(Out, Indent, Depth + 1);
			}
			AppendQuoted(Out, O[i].Key);
			Out.Add(':');
			if (Indent != -1)
			{
				Out.Add(' ');
			}
			O[i].Value.WriteImpl(Out, Indent, Depth + 1);
		}
		if (Indent >= 0)
		{
			AppendIndent(Out, Indent, Depth);
		}
		Out.Add('}');
		return;
	}
	}
}

bool FConvJson::SaveToFile(const FString& Path, int32 Indent) const
{
	TArray<ANSICHAR> Out;
	Out.Reserve(4096);
	Write(Out, Indent);
	return ConvWriteFile(Path, Out.GetData(), Out.Num());
}

bool FConvJson::Parse(const ANSICHAR* Text, int32 Len, FConvJson& Out, FString* OutError)
{
	ArenaConvJsonLocal::FParser Parser{ Text, Text + Len, FString() };
	// skip a UTF-8 BOM
	if (Len >= 3 && (uint8)Text[0] == 0xEF && (uint8)Text[1] == 0xBB && (uint8)Text[2] == 0xBF)
	{
		Parser.P += 3;
	}
	const bool bOk = Parser.ParseValue(Out, 0);
	if (!bOk && OutError)
	{
		*OutError = FString::Printf(TEXT("%s at byte %d"), *Parser.Error, int32(Parser.P - Text));
	}
	return bOk;
}

bool FConvJson::LoadFile(const FString& Path, FConvJson& Out, FString* OutError)
{
	TArray64<uint8> Data;
	if (!FFileHelper::LoadFileToArray(Data, *Path, FILEREAD_Silent))
	{
		if (OutError)
		{
			*OutError = TEXT("cannot read file");
		}
		return false;
	}
	if (Data.Num() > MAX_int32)
	{
		return false;
	}
	return Parse((const ANSICHAR*)Data.GetData(), (int32)Data.Num(), Out, OutError);
}

bool ConvWriteFile(const FString& Path, const void* Data, int64 Size)
{
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> H(PF.OpenWrite(*Path));
	if (!H)
	{
		PF.CreateDirectoryTree(*FPaths::GetPath(Path));
		H.Reset(PF.OpenWrite(*Path));
		if (!H)
		{
			return false;
		}
	}
	return Size == 0 || H->Write((const uint8*)Data, Size);
}
