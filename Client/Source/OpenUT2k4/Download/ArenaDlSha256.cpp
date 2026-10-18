#include "ArenaDlSha256.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"

namespace ArenaDlSha256Local
{
	constexpr uint32 K[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

	FORCEINLINE uint32 Ror(uint32 X, uint32 N) { return (X >> N) | (X << (32 - N)); }
}

void FArenaSha256::Reset()
{
	static const uint32 Init[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	FMemory::Memcpy(State, Init, sizeof(State));
	TotalBytes = 0;
	Buffered = 0;
}

void FArenaSha256::Block(const uint8* Data)
{
	using namespace ArenaDlSha256Local;
	uint32 W[64];
	for (int32 I = 0; I < 16; ++I)
	{
		W[I] = (uint32(Data[I * 4]) << 24) | (uint32(Data[I * 4 + 1]) << 16) | (uint32(Data[I * 4 + 2]) << 8) | uint32(Data[I * 4 + 3]);
	}
	for (int32 I = 16; I < 64; ++I)
	{
		const uint32 S0 = Ror(W[I - 15], 7) ^ Ror(W[I - 15], 18) ^ (W[I - 15] >> 3);
		const uint32 S1 = Ror(W[I - 2], 17) ^ Ror(W[I - 2], 19) ^ (W[I - 2] >> 10);
		W[I] = W[I - 16] + S0 + W[I - 7] + S1;
	}
	uint32 A = State[0], B = State[1], C = State[2], D = State[3], E = State[4], F = State[5], G = State[6], H = State[7];
	for (int32 I = 0; I < 64; ++I)
	{
		const uint32 T1 = H + (Ror(E, 6) ^ Ror(E, 11) ^ Ror(E, 25)) + ((E & F) ^ (~E & G)) + K[I] + W[I];
		const uint32 T2 = (Ror(A, 2) ^ Ror(A, 13) ^ Ror(A, 22)) + ((A & B) ^ (A & C) ^ (B & C));
		H = G; G = F; F = E; E = D + T1; D = C; C = B; B = A; A = T1 + T2;
	}
	State[0] += A; State[1] += B; State[2] += C; State[3] += D;
	State[4] += E; State[5] += F; State[6] += G; State[7] += H;
}

void FArenaSha256::Update(const uint8* Data, int64 Size)
{
	TotalBytes += uint64(Size);
	if (Buffered > 0)
	{
		const int32 Take = int32(FMath::Min<int64>(64 - Buffered, Size));
		FMemory::Memcpy(Buffer + Buffered, Data, Take);
		Buffered += Take;
		Data += Take;
		Size -= Take;
		if (Buffered < 64)
		{
			return;
		}
		Block(Buffer);
		Buffered = 0;
	}
	while (Size >= 64)
	{
		Block(Data);
		Data += 64;
		Size -= 64;
	}
	if (Size > 0)
	{
		FMemory::Memcpy(Buffer, Data, Size);
		Buffered = int32(Size);
	}
}

void FArenaSha256::Final(uint8 OutDigest[32])
{
	const uint64 Bits = TotalBytes * 8;
	uint8 Pad[72] = { 0x80 };
	const int32 PadLen = (Buffered < 56) ? (56 - Buffered) : (120 - Buffered);
	for (int32 I = 0; I < 8; ++I)
	{
		Pad[PadLen + I] = uint8(Bits >> (56 - I * 8));
	}
	const uint64 Saved = TotalBytes;
	Update(Pad, PadLen + 8);
	TotalBytes = Saved;
	for (int32 I = 0; I < 8; ++I)
	{
		OutDigest[I * 4] = uint8(State[I] >> 24);
		OutDigest[I * 4 + 1] = uint8(State[I] >> 16);
		OutDigest[I * 4 + 2] = uint8(State[I] >> 8);
		OutDigest[I * 4 + 3] = uint8(State[I]);
	}
}

FString FArenaSha256::FinalHex()
{
	uint8 Digest[32];
	Final(Digest);
	return BytesToHexLower(Digest, 32);
}

FString FArenaSha256::HashFile(const FString& Path, TFunctionRef<bool(int64, int64)> OnProgress)
{
	TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
	if (!File)
	{
		return FString();
	}
	const int64 Total = File->Size();
	TArray<uint8> Buf;
	Buf.SetNumUninitialized(4 << 20);
	FArenaSha256 Sha;
	int64 Done = 0;
	while (Done < Total)
	{
		const int64 N = FMath::Min<int64>(Buf.Num(), Total - Done);
		if (!File->Read(Buf.GetData(), N))
		{
			return FString();
		}
		Sha.Update(Buf.GetData(), N);
		Done += N;
		if (!OnProgress(Done, Total))
		{
			return FString();
		}
	}
	return Sha.FinalHex();
}
