#pragma once

#include "CoreMinimal.h"

/**
 * Bit streams in the UE2 (UT2004) network layout. See Docs/UE2Net.md "Bit streams".
 *
 * Bits are stored least significant bit first within each byte. Multi-byte values (INT, FLOAT) are written as their
 * little-endian bytes, each byte as 8 bits, without any alignment. "Ranged" integers (ReadInt / WriteInt with a
 * maximum) use only as many low bits as the value range needs: bit k is present only while (value so far + 2^k) is
 * still below the maximum, so a range of 1024 takes 10 bits and a range of 20 takes 5 bits (with the 5th bit
 * skipped for small values).
 */
class FUE2BitReader
{
public:
	FUE2BitReader() = default;
	/** Reads NumBits bits starting at bit 0 of Data (copied). */
	FUE2BitReader(const uint8* Data, int64 NumBits);

	bool ReadBit();
	/** A ranged integer in [0, Max). */
	uint32 ReadInt(uint32 Max);
	uint8 ReadByte();
	void ReadBytes(void* Out, int32 Count);
	int32 ReadInt32();
	uint32 ReadUInt32();
	int16 ReadInt16();
	float ReadFloat();
	/** UE2 FCompactIndex (1 to 5 bytes). */
	int32 ReadCompactIndex();
	/** UE2 FString: compact length (negative: UTF-16), characters, terminating zero. */
	FString ReadString();
	/** The next Count bits as a new reader. */
	FUE2BitReader ReadBits(int64 Count);

	bool AtEnd() const { return Pos >= Num; }
	bool IsError() const { return bError; }
	void SetError() { bError = true; }
	int64 GetPosBits() const { return Pos; }
	int64 GetNumBits() const { return Num; }
	int64 GetBitsLeft() const { return Num - Pos; }
	/** "0101..." of up to MaxBits upcoming bits (diagnostics). */
	FString PeekBitString(int32 MaxBits) const;

private:
	TArray<uint8> Buffer;
	int64 Num = 0;
	int64 Pos = 0;
	bool bError = false;
};

class FUE2BitWriter
{
public:
	void WriteBit(bool bBit);
	/** A ranged integer in [0, Max) (see the class comment of FUE2BitReader). */
	void WriteInt(uint32 Value, uint32 Max);
	void WriteByte(uint8 Value);
	void WriteBytes(const void* Data, int32 Count);
	void WriteInt32(int32 Value);
	void WriteFloat(float Value);
	void WriteCompactIndex(int32 Value);
	/** UE2 FString (ANSI when every character fits in Latin-1, UTF-16 otherwise). */
	void WriteString(const FString& Value);
	/** Appends another writer's bits. */
	void WriteBits(const FUE2BitWriter& Other);

	int64 GetNumBits() const { return Num; }
	const TArray<uint8>& GetData() const { return Buffer; }
	/** The bytes of a finished packet: the bits plus a terminating 1 bit (UE2 marks the packet end this way). */
	TArray<uint8> GetPacketBytes() const;
	void Reset() { Buffer.Reset(); Num = 0; }

private:
	TArray<uint8> Buffer;
	int64 Num = 0;
};

/** Number of bits a ranged integer with this maximum takes at most (UE2 appCeilLogTwo). */
int32 UE2CeilLogTwo(uint32 Value);
