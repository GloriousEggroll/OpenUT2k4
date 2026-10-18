#include "UE2NetBits.h"
#include "ArenaServerBrowser.h"

int32 UE2CeilLogTwo(uint32 Value)
{
	int32 Bits = 0;
	while (Bits < 32 && (uint64(1) << Bits) < Value)
	{
		++Bits;
	}
	return Bits;
}

// =================================================================================================
// Reader
// =================================================================================================

FUE2BitReader::FUE2BitReader(const uint8* Data, int64 NumBits)
	: Num(FMath::Max<int64>(0, NumBits))
{
	const int64 Bytes = (Num + 7) >> 3;
	Buffer.SetNumZeroed(int32(Bytes));
	if (Data && Bytes > 0)
	{
		FMemory::Memcpy(Buffer.GetData(), Data, Bytes);
	}
}

bool FUE2BitReader::ReadBit()
{
	if (Pos >= Num)
	{
		bError = true;
		return false;
	}
	const bool bBit = (Buffer[int32(Pos >> 3)] >> (Pos & 7)) & 1;
	++Pos;
	return bBit;
}

uint32 FUE2BitReader::ReadInt(uint32 Max)
{
	uint32 Value = 0;
	for (uint32 Mask = 1; Value + Mask < Max && Mask; Mask <<= 1)
	{
		if (ReadBit())
		{
			Value |= Mask;
		}
		if (bError)
		{
			return 0;
		}
	}
	return Value;
}

uint8 FUE2BitReader::ReadByte()
{
	if (Pos + 8 > Num)
	{
		bError = true;
		Pos = Num;
		return 0;
	}
	uint8 Value = 0;
	const int32 Shift = int32(Pos & 7);
	const int32 Index = int32(Pos >> 3);
	if (Shift == 0)
	{
		Value = Buffer[Index];
	}
	else
	{
		Value = uint8((Buffer[Index] >> Shift) | (Buffer[Index + 1] << (8 - Shift)));
	}
	Pos += 8;
	return Value;
}

void FUE2BitReader::ReadBytes(void* Out, int32 Count)
{
	uint8* Dst = static_cast<uint8*>(Out);
	for (int32 i = 0; i < Count; ++i)
	{
		Dst[i] = ReadByte();
	}
}

int32 FUE2BitReader::ReadInt32()
{
	return int32(ReadUInt32());
}

uint32 FUE2BitReader::ReadUInt32()
{
	uint8 B[4];
	ReadBytes(B, 4);
	return uint32(B[0]) | (uint32(B[1]) << 8) | (uint32(B[2]) << 16) | (uint32(B[3]) << 24);
}

int16 FUE2BitReader::ReadInt16()
{
	uint8 B[2];
	ReadBytes(B, 2);
	return int16(uint16(B[0]) | (uint16(B[1]) << 8));
}

float FUE2BitReader::ReadFloat()
{
	const uint32 Bits = ReadUInt32();
	float Value;
	FMemory::Memcpy(&Value, &Bits, 4);
	return Value;
}

int32 FUE2BitReader::ReadCompactIndex()
{
	// first byte: sign (0x80), "more" (0x40), 6 value bits; then up to 4 bytes of "more" (0x80) + 7 value bits
	const uint8 B0 = ReadByte();
	int32 Value = B0 & 0x3F;
	if (B0 & 0x40)
	{
		int32 Shift = 6;
		for (int32 i = 0; i < 4 && !bError; ++i)
		{
			const uint8 B = ReadByte();
			Value |= int32(B & 0x7F) << Shift;
			Shift += 7;
			if (!(B & 0x80))
			{
				break;
			}
		}
	}
	return (B0 & 0x80) ? -Value : Value;
}

FString FUE2BitReader::ReadString()
{
	const int32 Len = ReadCompactIndex();
	// strings in the protocol are short; anything this long is a decoding error
	constexpr int32 MaxLen = 65536;
	if (bError || Len > MaxLen || Len < -MaxLen)
	{
		bError = true;
		return FString();
	}
	FString Out;
	if (Len > 0)
	{
		TArray<uint8> Chars;
		Chars.SetNumUninitialized(Len);
		ReadBytes(Chars.GetData(), Len);
		// like the server browser: colour codes removed, UTF-8 when valid, else Windows-1252; replacement characters
		// that a server's own text holds (colour codes mangled by a UTF-8 editor) are dropped
		Out = ArenaServerBrowser::DecodeBytes(Chars.GetData(), Len, true);
	}
	else if (Len < 0)
	{
		const int32 Count = -Len;
		Out.Reserve(Count);
		for (int32 i = 0; i < Count && !bError; ++i)
		{
			uint8 B[2];
			ReadBytes(B, 2);
			const TCHAR C = TCHAR(uint16(B[0]) | (uint16(B[1]) << 8));
			if (C == 0)
			{
				// skip the rest (the terminator is the last character)
				for (int32 j = i + 1; j < Count && !bError; ++j)
				{
					ReadBytes(B, 2);
				}
				break;
			}
			Out.AppendChar(C);
		}
	}
	const TCHAR Replacement[] = { TCHAR(0xFFFD), 0 };
	Out.ReplaceInline(Replacement, TEXT(""));
	return Out;
}

FUE2BitReader FUE2BitReader::ReadBits(int64 Count)
{
	if (Count < 0 || Pos + Count > Num)
	{
		bError = true;
		Pos = Num;
		return FUE2BitReader();
	}
	FUE2BitReader Sub;
	Sub.Num = Count;
	Sub.Buffer.SetNumZeroed(int32((Count + 7) >> 3));
	if ((Pos & 7) == 0)
	{
		FMemory::Memcpy(Sub.Buffer.GetData(), Buffer.GetData() + (Pos >> 3), (Count + 7) >> 3);
		// clear bits past the end of the sub stream
		if (Count & 7)
		{
			Sub.Buffer.Last() &= uint8((1u << (Count & 7)) - 1);
		}
		Pos += Count;
	}
	else
	{
		for (int64 i = 0; i < Count; ++i)
		{
			if (ReadBit())
			{
				Sub.Buffer[int32(i >> 3)] |= uint8(1u << (i & 7));
			}
		}
	}
	return Sub;
}

FString FUE2BitReader::PeekBitString(int32 MaxBits) const
{
	FString Out;
	for (int64 P = Pos; P < Num && P < Pos + MaxBits; ++P)
	{
		Out.AppendChar(((Buffer[int32(P >> 3)] >> (P & 7)) & 1) ? TEXT('1') : TEXT('0'));
	}
	return Out;
}

// =================================================================================================
// Writer
// =================================================================================================

void FUE2BitWriter::WriteBit(bool bBit)
{
	if ((Num & 7) == 0)
	{
		Buffer.Add(0);
	}
	if (bBit)
	{
		Buffer[int32(Num >> 3)] |= uint8(1u << (Num & 7));
	}
	++Num;
}

void FUE2BitWriter::WriteInt(uint32 Value, uint32 Max)
{
	uint32 NewValue = 0;
	for (uint32 Mask = 1; NewValue + Mask < Max && Mask; Mask <<= 1)
	{
		const bool bBit = (Value & Mask) != 0;
		WriteBit(bBit);
		if (bBit)
		{
			NewValue += Mask;
		}
	}
}

void FUE2BitWriter::WriteByte(uint8 Value)
{
	for (int32 i = 0; i < 8; ++i)
	{
		WriteBit((Value >> i) & 1);
	}
}

void FUE2BitWriter::WriteBytes(const void* Data, int32 Count)
{
	const uint8* Src = static_cast<const uint8*>(Data);
	for (int32 i = 0; i < Count; ++i)
	{
		WriteByte(Src[i]);
	}
}

void FUE2BitWriter::WriteInt32(int32 Value)
{
	const uint32 V = uint32(Value);
	const uint8 B[4] = { uint8(V), uint8(V >> 8), uint8(V >> 16), uint8(V >> 24) };
	WriteBytes(B, 4);
}

void FUE2BitWriter::WriteFloat(float Value)
{
	int32 Bits;
	FMemory::Memcpy(&Bits, &Value, 4);
	WriteInt32(Bits);
}

void FUE2BitWriter::WriteCompactIndex(int32 Value)
{
	uint32 Abs = uint32(Value < 0 ? -int64(Value) : int64(Value));
	uint8 B0 = uint8((Value < 0 ? 0x80 : 0) | (Abs & 0x3F));
	Abs >>= 6;
	if (Abs)
	{
		B0 |= 0x40;
	}
	WriteByte(B0);
	while (Abs)
	{
		uint8 B = uint8(Abs & 0x7F);
		Abs >>= 7;
		if (Abs)
		{
			B |= 0x80;
		}
		WriteByte(B);
	}
}

void FUE2BitWriter::WriteString(const FString& Value)
{
	bool bAnsi = true;
	for (TCHAR C : Value)
	{
		if (uint32(C) > 0xFF)
		{
			bAnsi = false;
			break;
		}
	}
	const int32 Count = Value.Len() + 1;
	if (bAnsi)
	{
		WriteCompactIndex(Count);
		for (TCHAR C : Value)
		{
			WriteByte(uint8(C));
		}
		WriteByte(0);
	}
	else
	{
		WriteCompactIndex(-Count);
		for (TCHAR C : Value)
		{
			WriteByte(uint8(uint16(C) & 0xFF));
			WriteByte(uint8(uint16(C) >> 8));
		}
		WriteByte(0);
		WriteByte(0);
	}
}

void FUE2BitWriter::WriteBits(const FUE2BitWriter& Other)
{
	for (int64 i = 0; i < Other.Num; ++i)
	{
		WriteBit((Other.Buffer[int32(i >> 3)] >> (i & 7)) & 1);
	}
}

TArray<uint8> FUE2BitWriter::GetPacketBytes() const
{
	FUE2BitWriter Copy = *this;
	Copy.WriteBit(true);
	return Copy.Buffer;
}
