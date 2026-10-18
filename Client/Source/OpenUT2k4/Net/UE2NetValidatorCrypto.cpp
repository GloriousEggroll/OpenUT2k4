#include "UE2NetValidatorCrypto.h"

#include "Misc/ScopeLock.h"

namespace UE2ValidatorCryptoLocal
{
	const uint32 K256[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
	};

	/** DER prefix of a SHA-256 DigestInfo (RFC 8017 9.2, note 1). */
	const uint8 Sha256Prefix[19] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };

	inline uint32 Rotr(uint32 X, int32 N) { return (X >> N) | (X << (32 - N)); }

	/** Non-negative big integer: 32-bit limbs, least significant first. */
	using FLimbs = TArray<uint32>;

	FLimbs FromBytes(const TArray<uint8>& Bytes, int32 NumLimbs)
	{
		FLimbs Out;
		Out.SetNumZeroed(NumLimbs);
		const int32 Len = Bytes.Num();
		for (int32 i = 0; i < Len; ++i)
		{
			const int32 Bit = (Len - 1 - i) * 8;
			if (Bit / 32 < NumLimbs)
			{
				Out[Bit / 32] |= uint32(Bytes[i]) << (Bit % 32);
			}
		}
		return Out;
	}

	TArray<uint8> ToBytes(const FLimbs& X, int32 NumBytes)
	{
		TArray<uint8> Out;
		Out.SetNumZeroed(NumBytes);
		for (int32 i = 0; i < NumBytes; ++i)
		{
			const int32 Bit = (NumBytes - 1 - i) * 8;
			if (Bit / 32 < X.Num())
			{
				Out[i] = uint8(X[Bit / 32] >> (Bit % 32));
			}
		}
		return Out;
	}

	/** -1, 0, 1 (same limb counts). */
	int32 Compare(const FLimbs& A, const FLimbs& B)
	{
		for (int32 i = A.Num() - 1; i >= 0; --i)
		{
			if (A[i] != B[i])
			{
				return A[i] < B[i] ? -1 : 1;
			}
		}
		return 0;
	}

	void SubInPlace(FLimbs& A, const FLimbs& B)
	{
		int64 Borrow = 0;
		for (int32 i = 0; i < A.Num(); ++i)
		{
			const int64 D = int64(A[i]) - int64(B[i]) - Borrow;
			A[i] = uint32(D);
			Borrow = D < 0 ? 1 : 0;
		}
	}

	/** Montgomery arithmetic modulo an odd N (R = 2^(32 L)). */
	struct FMont
	{
		FLimbs N;
		int32 L = 0;
		uint32 N0Inv = 0;
		/** R^2 mod N. */
		FLimbs RR;

		bool Init(const FLimbs& InN)
		{
			N = InN;
			L = N.Num();
			if (L == 0 || (N[0] & 1) == 0)
			{
				return false;
			}
			// -N^-1 mod 2^32 by Newton's iteration (each step doubles the correct bits)
			uint32 X = N[0];
			for (int32 i = 0; i < 5; ++i)
			{
				X *= 2u - N[0] * X;
			}
			N0Inv = 0u - X;
			// R^2 mod N by doubling 1 (2 * 32 * L times), reducing as we go
			RR.SetNumZeroed(L);
			RR[0] = 1;
			for (int32 Bit = 0; Bit < 64 * L; ++Bit)
			{
				uint32 Carry = 0;
				for (int32 i = 0; i < L; ++i)
				{
					const uint32 Next = RR[i] >> 31;
					RR[i] = (RR[i] << 1) | Carry;
					Carry = Next;
				}
				if (Carry || Compare(RR, N) >= 0)
				{
					SubInPlace(RR, N);
				}
			}
			return true;
		}

		/** A * B * R^-1 mod N (CIOS), inputs below N. */
		FLimbs Mul(const FLimbs& A, const FLimbs& B) const
		{
			TArray<uint64> T;
			T.SetNumZeroed(L + 2);
			for (int32 i = 0; i < L; ++i)
			{
				uint64 C = 0;
				for (int32 j = 0; j < L; ++j)
				{
					const uint64 S = T[j] + uint64(A[j]) * B[i] + C;
					T[j] = S & 0xffffffffu;
					C = S >> 32;
				}
				uint64 S = T[L] + C;
				T[L] = S & 0xffffffffu;
				T[L + 1] = S >> 32;
				const uint32 M = uint32(T[0]) * N0Inv;
				S = T[0] + uint64(M) * N[0];
				C = S >> 32;
				for (int32 j = 1; j < L; ++j)
				{
					S = T[j] + uint64(M) * N[j] + C;
					T[j - 1] = S & 0xffffffffu;
					C = S >> 32;
				}
				S = T[L] + C;
				T[L - 1] = S & 0xffffffffu;
				T[L] = T[L + 1] + (S >> 32);
			}
			FLimbs Out;
			Out.SetNumUninitialized(L);
			for (int32 i = 0; i < L; ++i)
			{
				Out[i] = uint32(T[i]);
			}
			if (T[L] != 0 || Compare(Out, N) >= 0)
			{
				SubInPlace(Out, N);
			}
			return Out;
		}

		/** Base^Exp mod N (square and multiply over Exp's bits, most significant first). */
		FLimbs Pow(const FLimbs& Base, const TArray<uint8>& Exp) const
		{
			FLimbs One;
			One.SetNumZeroed(L);
			One[0] = 1;
			const FLimbs BaseM = Mul(Base, RR);
			FLimbs X = Mul(One, RR);
			for (const uint8 Byte : Exp)
			{
				for (int32 Bit = 7; Bit >= 0; --Bit)
				{
					X = Mul(X, X);
					if ((Byte >> Bit) & 1)
					{
						X = Mul(X, BaseM);
					}
				}
			}
			return Mul(X, One);
		}
	};

	/** EMSA-PKCS1-v1_5 encoding of SHA-256(Message) for a K byte modulus. */
	bool EncodeMessage(const TArray<uint8>& Message, int32 K, TArray<uint8>& Out)
	{
		const int32 TLen = 19 + 32;
		if (K < TLen + 11)
		{
			return false;
		}
		uint8 Hash[32];
		UE2ValidatorCrypto::Sha256(Message.GetData(), Message.Num(), Hash);
		Out.Reset(K);
		Out.Add(0x00);
		Out.Add(0x01);
		for (int32 i = 0; i < K - TLen - 3; ++i)
		{
			Out.Add(0xff);
		}
		Out.Add(0x00);
		Out.Append(Sha256Prefix, 19);
		Out.Append(Hash, 32);
		return true;
	}

	/** Big-endian bytes without leading zeros. */
	TArray<uint8> Trim(const TArray<uint8>& In)
	{
		int32 i = 0;
		while (i < In.Num() - 1 && In[i] == 0)
		{
			++i;
		}
		return TArray<uint8>(In.GetData() + i, In.Num() - i);
	}
}

namespace UE2ValidatorCrypto
{
	using namespace UE2ValidatorCryptoLocal;

	void FSha256::Reset()
	{
		static const uint32 Init[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
		FMemory::Memcpy(H, Init, sizeof(H));
		BufLen = 0;
		TotalLen = 0;
	}

	void FSha256::Block(const uint8* P)
	{
		uint32 W[64];
		for (int32 i = 0; i < 16; ++i)
		{
			W[i] = (uint32(P[i * 4]) << 24) | (uint32(P[i * 4 + 1]) << 16) | (uint32(P[i * 4 + 2]) << 8) | uint32(P[i * 4 + 3]);
		}
		for (int32 i = 16; i < 64; ++i)
		{
			const uint32 S0 = Rotr(W[i - 15], 7) ^ Rotr(W[i - 15], 18) ^ (W[i - 15] >> 3);
			const uint32 S1 = Rotr(W[i - 2], 17) ^ Rotr(W[i - 2], 19) ^ (W[i - 2] >> 10);
			W[i] = W[i - 16] + S0 + W[i - 7] + S1;
		}
		uint32 A = H[0], B = H[1], C = H[2], D = H[3], E = H[4], F = H[5], G = H[6], Hh = H[7];
		for (int32 i = 0; i < 64; ++i)
		{
			const uint32 S1 = Rotr(E, 6) ^ Rotr(E, 11) ^ Rotr(E, 25);
			const uint32 Ch = (E & F) ^ (~E & G);
			const uint32 T1 = Hh + S1 + Ch + K256[i] + W[i];
			const uint32 S0 = Rotr(A, 2) ^ Rotr(A, 13) ^ Rotr(A, 22);
			const uint32 Maj = (A & B) ^ (A & C) ^ (B & C);
			const uint32 T2 = S0 + Maj;
			Hh = G; G = F; F = E; E = D + T1; D = C; C = B; B = A; A = T1 + T2;
		}
		H[0] += A; H[1] += B; H[2] += C; H[3] += D; H[4] += E; H[5] += F; H[6] += G; H[7] += Hh;
	}

	void FSha256::Update(const uint8* Data, int64 Len)
	{
		TotalLen += uint64(Len);
		while (Len > 0)
		{
			const int32 Take = int32(FMath::Min<int64>(64 - BufLen, Len));
			FMemory::Memcpy(Buf + BufLen, Data, Take);
			BufLen += Take;
			Data += Take;
			Len -= Take;
			if (BufLen == 64)
			{
				Block(Buf);
				BufLen = 0;
			}
		}
	}

	void FSha256::Final(uint8 Out[32])
	{
		const uint64 Bits = TotalLen * 8;
		const uint8 Pad = 0x80;
		const uint8 Zero = 0;
		const uint64 Saved = TotalLen;
		Update(&Pad, 1);
		while (BufLen != 56)
		{
			Update(&Zero, 1);
		}
		uint8 LenBytes[8];
		for (int32 i = 0; i < 8; ++i)
		{
			LenBytes[i] = uint8(Bits >> (56 - 8 * i));
		}
		Update(LenBytes, 8);
		TotalLen = Saved;
		for (int32 i = 0; i < 8; ++i)
		{
			Out[i * 4] = uint8(H[i] >> 24);
			Out[i * 4 + 1] = uint8(H[i] >> 16);
			Out[i * 4 + 2] = uint8(H[i] >> 8);
			Out[i * 4 + 3] = uint8(H[i]);
		}
	}

	void Sha256(const uint8* Data, int64 Len, uint8 Out[32])
	{
		FSha256 S;
		S.Update(Data, Len);
		S.Final(Out);
	}

	TArray<uint8> StringBytes(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}

	FString Sha256Hex(const FString& Text)
	{
		const TArray<uint8> Bytes = StringBytes(Text);
		uint8 Hash[32];
		Sha256(Bytes.GetData(), Bytes.Num(), Hash);
		return BytesToHex(Hash, 32);
	}

	FString BytesToHex(const uint8* Data, int32 Len)
	{
		static const TCHAR* Digits = TEXT("0123456789abcdef");
		FString Out;
		Out.Reserve(Len * 2);
		for (int32 i = 0; i < Len; ++i)
		{
			Out.AppendChar(Digits[Data[i] >> 4]);
			Out.AppendChar(Digits[Data[i] & 15]);
		}
		return Out;
	}

	bool HexToBytes(const FString& InHex, TArray<uint8>& Out)
	{
		FString Hex = InHex.TrimStartAndEnd();
		if (Hex.Len() % 2 == 1)
		{
			Hex = TEXT("0") + Hex;
		}
		Out.Reset(Hex.Len() / 2);
		for (int32 i = 0; i < Hex.Len(); i += 2)
		{
			if (!FChar::IsHexDigit(Hex[i]) || !FChar::IsHexDigit(Hex[i + 1]))
			{
				return false;
			}
			Out.Add(uint8(FParse::HexDigit(Hex[i]) * 16 + FParse::HexDigit(Hex[i + 1])));
		}
		return true;
	}

	FString NormalizeHex(const FString& Hex)
	{
		FString Out = Hex.TrimStartAndEnd().ToLower();
		int32 i = 0;
		while (i < Out.Len() - 1 && Out[i] == TEXT('0'))
		{
			++i;
		}
		return Out.Mid(i);
	}

	FString Fingerprint(const FString& ModulusHex)
	{
		return Sha256Hex(NormalizeHex(ModulusHex)).Left(32);
	}

	bool ParseKeyFile(const FString& Text, FRsaPrivateKey& Out, FString& OutError)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines);
		if (Lines.Num() == 0 || Lines[0].TrimStartAndEnd() != TEXT("OUT2K4KEY1"))
		{
			OutError = TEXT("not an OpenUT2k4 key file");
			return false;
		}
		Out = FRsaPrivateKey();
		int32 E = 3;
		for (const FString& Raw : Lines)
		{
			const FString Line = Raw.TrimStartAndEnd();
			FString Key, Value;
			if (Line.StartsWith(TEXT("#")) || !Line.Split(TEXT("="), &Key, &Value))
			{
				continue;
			}
			if (Key == TEXT("n"))
			{
				HexToBytes(Value, Out.N);
			}
			else if (Key == TEXT("d"))
			{
				HexToBytes(Value, Out.D);
			}
			else if (Key == TEXT("e"))
			{
				E = FCString::Atoi(*Value);
			}
		}
		Out.N = Trim(Out.N);
		Out.D = Trim(Out.D);
		if (E != 3 || !Out.IsValid())
		{
			OutError = TEXT("incomplete key or exponent other than 3");
			return false;
		}
		return true;
	}

	bool RsaSign(const FRsaPrivateKey& Key, const TArray<uint8>& Message, TArray<uint8>& OutSig)
	{
		const TArray<uint8> N = Trim(Key.N);
		const int32 K = N.Num();
		TArray<uint8> EM;
		if (!Key.IsValid() || !EncodeMessage(Message, K, EM))
		{
			return false;
		}
		const int32 L = (K + 3) / 4;
		FMont Mont;
		if (!Mont.Init(FromBytes(N, L)))
		{
			return false;
		}
		const FLimbs S = Mont.Pow(FromBytes(EM, L), Key.D);
		OutSig = ToBytes(S, K);
		// never hand out a signature that does not verify (a bad key file would leak through faults otherwise)
		return RsaVerify(N, Message, OutSig);
	}

	bool RsaVerify(const TArray<uint8>& InModulus, const TArray<uint8>& Message, const TArray<uint8>& Sig)
	{
		const TArray<uint8> N = Trim(InModulus);
		const int32 K = N.Num();
		if (K < 128 || K > 1024 || Sig.Num() != K || (N.Last() & 1) == 0)
		{
			return false;
		}
		TArray<uint8> EM;
		if (!EncodeMessage(Message, K, EM))
		{
			return false;
		}
		const int32 L = (K + 3) / 4;
		FMont Mont;
		const FLimbs NL = FromBytes(N, L);
		if (!Mont.Init(NL))
		{
			return false;
		}
		const FLimbs S = FromBytes(Sig, L);
		if (Compare(S, NL) >= 0)
		{
			return false;
		}
		const TArray<uint8> Three = { 3 };
		return ToBytes(Mont.Pow(S, Three), K) == EM;
	}

	// ---------------------------------------------------------------------------------------------
	// SHA-512 and Ed25519 verification (signed ban lists)
	// ---------------------------------------------------------------------------------------------

	void Sha512(const uint8* Data, int64 Len, uint8 Out[64])
	{
		static const uint64 K512[80] = {
			0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull, 0x59f111f1b605d019ull,
			0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
			0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull,
			0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull, 0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
			0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
			0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
			0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull, 0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull,
			0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
			0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull,
			0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
			0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull, 0xca273eceea26619cull, 0xd186b8c721c0c207ull,
			0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
			0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull,
			0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
		};
		uint64 H[8] = { 0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
			0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull };
		auto R = [](uint64 X, int32 N) { return (X >> N) | (X << (64 - N)); };
		TArray<uint8> M(Data, int32(Len));
		M.Add(0x80);
		while (M.Num() % 128 != 112)
		{
			M.Add(0);
		}
		for (int32 i = 0; i < 8; ++i)
		{
			M.Add(0);
		}
		const uint64 Bits = uint64(Len) * 8;
		for (int32 i = 0; i < 8; ++i)
		{
			M.Add(uint8(Bits >> (56 - 8 * i)));
		}
		for (int32 Off = 0; Off < M.Num(); Off += 128)
		{
			uint64 W[80];
			for (int32 i = 0; i < 16; ++i)
			{
				W[i] = 0;
				for (int32 j = 0; j < 8; ++j)
				{
					W[i] = (W[i] << 8) | M[Off + i * 8 + j];
				}
			}
			for (int32 i = 16; i < 80; ++i)
			{
				const uint64 S0 = R(W[i - 15], 1) ^ R(W[i - 15], 8) ^ (W[i - 15] >> 7);
				const uint64 S1 = R(W[i - 2], 19) ^ R(W[i - 2], 61) ^ (W[i - 2] >> 6);
				W[i] = W[i - 16] + S0 + W[i - 7] + S1;
			}
			uint64 A = H[0], B = H[1], C = H[2], D = H[3], E = H[4], F = H[5], G = H[6], Hh = H[7];
			for (int32 i = 0; i < 80; ++i)
			{
				const uint64 T1 = Hh + (R(E, 14) ^ R(E, 18) ^ R(E, 41)) + ((E & F) ^ (~E & G)) + K512[i] + W[i];
				const uint64 T2 = (R(A, 28) ^ R(A, 34) ^ R(A, 39)) + ((A & B) ^ (A & C) ^ (B & C));
				Hh = G; G = F; F = E; E = D + T1; D = C; C = B; B = A; A = T1 + T2;
			}
			H[0] += A; H[1] += B; H[2] += C; H[3] += D; H[4] += E; H[5] += F; H[6] += G; H[7] += Hh;
		}
		for (int32 i = 0; i < 64; ++i)
		{
			Out[i] = uint8(H[i / 8] >> (56 - 8 * (i % 8)));
		}
	}
}

namespace UE2ValidatorCryptoLocal
{
	/** Arithmetic on edwards25519 (RFC 8032), field elements in Montgomery form modulo p = 2^255 - 19. */
	struct FEd25519
	{
		FMont M;
		FLimbs P, One, D2, DConst, SqrtM1, BX, BY;
		bool bInit = false;

		struct FPoint
		{
			FLimbs X, Y, Z, T;
		};

		static FLimbs FromLE(const uint8* Bytes, int32 Len, int32 NumLimbs)
		{
			FLimbs Out;
			Out.SetNumZeroed(NumLimbs);
			for (int32 i = 0; i < Len && i / 4 < NumLimbs; ++i)
			{
				Out[i / 4] |= uint32(Bytes[i]) << (8 * (i % 4));
			}
			return Out;
		}
		static FLimbs Small(uint32 V)
		{
			FLimbs Out;
			Out.SetNumZeroed(8);
			Out[0] = V;
			return Out;
		}
		FLimbs ToM(const FLimbs& A) const { return M.Mul(A, M.RR); }
		FLimbs FromM(const FLimbs& A) const { return M.Mul(A, One); }
		FLimbs Mul(const FLimbs& A, const FLimbs& B) const { return M.Mul(A, B); }
		FLimbs Add(const FLimbs& A, const FLimbs& B) const
		{
			FLimbs R;
			R.SetNumUninitialized(8);
			uint64 C = 0;
			for (int32 i = 0; i < 8; ++i)
			{
				const uint64 S = uint64(A[i]) + B[i] + C;
				R[i] = uint32(S);
				C = S >> 32;
			}
			if (C || Compare(R, P) >= 0)
			{
				SubInPlace(R, P);
			}
			return R;
		}
		FLimbs Sub(const FLimbs& A, const FLimbs& B) const
		{
			FLimbs R = A;
			if (Compare(A, B) < 0)
			{
				// A + P - B (A + P may carry past 2^256: subtract first)
				FLimbs T = P;
				SubInPlace(T, B);
				return Add(A, T);
			}
			SubInPlace(R, B);
			return R;
		}
		/** A^E for a Montgomery-form A and a big-endian exponent. */
		FLimbs Pow(const FLimbs& A, const TArray<uint8>& E) const { return ToM(M.Pow(FromM(A), E)); }
		static TArray<uint8> ExpBytes(const FLimbs& V)
		{
			return ToBytes(V, 32);
		}
		FLimbs Inv(const FLimbs& A) const
		{
			FLimbs E = P;
			E[0] -= 2;
			return Pow(A, ExpBytes(E));
		}

		bool Init()
		{
			if (bInit)
			{
				return true;
			}
			P.SetNumZeroed(8);
			for (int32 i = 0; i < 8; ++i)
			{
				P[i] = 0xffffffffu;
			}
			P[0] = 0xffffffedu;
			P[7] = 0x7fffffffu;
			One = Small(1);
			if (!M.Init(P))
			{
				return false;
			}
			// d = -121665 / 121666
			const FLimbs DPos = Mul(ToM(Small(121665)), Inv(ToM(Small(121666))));
			const FLimbs Dd = Sub(ToM(Small(0)), DPos);
			DConst = Dd;
			D2 = Add(Dd, Dd);
			// sqrt(-1) = 2^((p - 1) / 4)
			FLimbs E = P;
			E[0] -= 1;
			for (int32 i = 0; i < 8; ++i)
			{
				E[i] = (E[i] >> 2) | (i < 7 ? E[i + 1] << 30 : 0u);
			}
			SqrtM1 = Pow(ToM(Small(2)), ExpBytes(E));
			// base point: y = 4 / 5, x even
			BY = Mul(ToM(Small(4)), Inv(ToM(Small(5))));
			if (!RecoverX(BY, 0, BX, Dd))
			{
				return false;
			}
			bInit = true;
			return true;
		}

		bool RecoverX(const FLimbs& Y, int32 Sign, FLimbs& OutX, const FLimbs& Dd) const
		{
			const FLimbs Y2 = Mul(Y, Y);
			const FLimbs OneM = ToM(One);
			const FLimbs U = Sub(Y2, OneM);
			const FLimbs V = Add(Mul(Dd, Y2), OneM);
			const FLimbs X2 = Mul(U, Inv(V));
			// (p + 3) / 8
			FLimbs E = P;
			E[0] += 3;
			for (int32 i = 0; i < 8; ++i)
			{
				E[i] = (E[i] >> 3) | (i < 7 ? E[i + 1] << 29 : 0u);
			}
			FLimbs X = Pow(X2, ExpBytes(E));
			if (Compare(Mul(X, X), X2) != 0)
			{
				X = Mul(X, SqrtM1);
			}
			if (Compare(Mul(X, X), X2) != 0)
			{
				return false;
			}
			const FLimbs XN = FromM(X);
			const bool bZero = Compare(XN, Small(0)) == 0;
			if (bZero && Sign)
			{
				return false;
			}
			if (int32(XN[0] & 1) != Sign)
			{
				X = Sub(ToM(Small(0)), X);
			}
			OutX = X;
			return true;
		}

		FLimbs DValue() const
		{
			return DConst;
		}

		bool Decode(const uint8 In[32], FPoint& Out) const
		{
			uint8 B[32];
			FMemory::Memcpy(B, In, 32);
			const int32 Sign = B[31] >> 7;
			B[31] &= 0x7f;
			const FLimbs YN = FromLE(B, 32, 8);
			if (Compare(YN, P) >= 0)
			{
				return false;
			}
			Out.Y = ToM(YN);
			if (!RecoverX(Out.Y, Sign, Out.X, DValue()))
			{
				return false;
			}
			Out.Z = ToM(One);
			Out.T = Mul(Out.X, Out.Y);
			return true;
		}

		FPoint AddPoints(const FPoint& A, const FPoint& B) const
		{
			const FLimbs Aa = Mul(Sub(A.Y, A.X), Sub(B.Y, B.X));
			const FLimbs Bb = Mul(Add(A.Y, A.X), Add(B.Y, B.X));
			const FLimbs Cc = Mul(Mul(A.T, D2), B.T);
			const FLimbs Z2 = Mul(A.Z, B.Z);
			const FLimbs Dd = Add(Z2, Z2);
			const FLimbs E = Sub(Bb, Aa), F = Sub(Dd, Cc), G = Add(Dd, Cc), H = Add(Bb, Aa);
			FPoint R;
			R.X = Mul(E, F);
			R.Y = Mul(G, H);
			R.T = Mul(E, H);
			R.Z = Mul(F, G);
			return R;
		}

		FPoint Identity() const
		{
			FPoint R;
			R.X = ToM(Small(0));
			R.Y = ToM(One);
			R.Z = ToM(One);
			R.T = ToM(Small(0));
			return R;
		}

		/** [Scalar] Pt, Scalar little-endian. */
		FPoint ScalarMul(const uint8* Scalar, int32 Len, const FPoint& Pt) const
		{
			FPoint Q = Identity();
			for (int32 i = Len * 8 - 1; i >= 0; --i)
			{
				Q = AddPoints(Q, Q);
				if ((Scalar[i / 8] >> (i % 8)) & 1)
				{
					Q = AddPoints(Q, Pt);
				}
			}
			return Q;
		}

		bool Equal(const FPoint& A, const FPoint& B) const
		{
			return Compare(Mul(A.X, B.Z), Mul(B.X, A.Z)) == 0 && Compare(Mul(A.Y, B.Z), Mul(B.Y, A.Z)) == 0;
		}
	};
}

namespace UE2ValidatorCrypto
{
	bool Ed25519Verify(const uint8 PublicKey[32], const uint8* Message, int64 Len, const uint8 Signature[64])
	{
		using namespace UE2ValidatorCryptoLocal;
		static FEd25519 Ed;
		static FCriticalSection Lock;
		FScopeLock L(&Lock);
		if (!Ed.Init())
		{
			return false;
		}
		// s < L (little-endian compare against the group order)
		static const uint8 Order[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };
		const uint8* S = Signature + 32;
		bool bBelow = false;
		for (int32 i = 31; i >= 0; --i)
		{
			if (S[i] != Order[i])
			{
				bBelow = S[i] < Order[i];
				break;
			}
		}
		if (!bBelow)
		{
			return false;
		}
		FEd25519::FPoint A, R;
		if (!Ed.Decode(PublicKey, A) || !Ed.Decode(Signature, R))
		{
			return false;
		}
		TArray<uint8> HIn(Signature, 32);
		HIn.Append(PublicKey, 32);
		HIn.Append(Message, int32(Len));
		uint8 H[64];
		Sha512(HIn.GetData(), HIn.Num(), H);
		FEd25519::FPoint B;
		B.X = Ed.BX;
		B.Y = Ed.BY;
		B.Z = Ed.ToM(Ed.One);
		B.T = Ed.Mul(B.X, B.Y);
		// cofactored: [8][s]B == [8]R + [8][h]A (h unreduced: [8]A has order L)
		FEd25519::FPoint Left = Ed.ScalarMul(S, 32, B);
		FEd25519::FPoint Right = Ed.AddPoints(R, Ed.ScalarMul(H, 64, A));
		for (int32 i = 0; i < 3; ++i)
		{
			Left = Ed.AddPoints(Left, Left);
			Right = Ed.AddPoints(Right, Right);
		}
		return Ed.Equal(Left, Right);
	}

	bool SecureRandomBytes(uint8* Out, int32 Len)
	{
#if PLATFORM_WINDOWS
		// CoCreateGuid makes version 4 GUIDs from the system's cryptographic random source (122 random bits each);
		// SHA-256 over a fresh batch per block mixes them
		int32 Done = 0;
		while (Done < Len)
		{
			FSha256 H;
			for (int32 i = 0; i < 4; ++i)
			{
				const FGuid G = FGuid::NewGuid();
				H.Update(reinterpret_cast<const uint8*>(&G), sizeof(G));
			}
			uint8 Block[32];
			H.Final(Block);
			const int32 Take = FMath::Min(32, Len - Done);
			FMemory::Memcpy(Out + Done, Block, Take);
			Done += Take;
		}
		return true;
#else
		FILE* F = fopen("/dev/urandom", "rb");
		if (!F)
		{
			return false;
		}
		const bool bOk = fread(Out, 1, Len, F) == size_t(Len);
		fclose(F);
		return bOk;
#endif
	}

	FString FormatKeyFile(const FRsaPrivateKey& Key, const FString& Comment)
	{
		return FString::Printf(TEXT("OUT2K4KEY1\n# %s\n# PRIVATE: this key is your player identity. Do not share it.\nn=%s\ne=3\nd=%s\n"), *Comment,
			*BytesToHex(Key.N.GetData(), Key.N.Num()), *BytesToHex(Key.D.GetData(), Key.D.Num()));
	}

	bool GenerateKey(FRsaPrivateKey& Out, int32 Bits)
	{
		using namespace UE2ValidatorCryptoLocal;
		if (Bits < 1024 || Bits % 64 != 0)
		{
			return false;
		}
		const int32 HalfLimbs = Bits / 64;
		auto ModSmall = [](const FLimbs& A, uint32 M)
		{
			uint64 R = 0;
			for (int32 i = A.Num() - 1; i >= 0; --i)
			{
				R = ((R << 32) | A[i]) % M;
			}
			return uint32(R);
		};
		static TArray<uint32> SmallPrimes;
		if (SmallPrimes.Num() == 0)
		{
			for (uint32 C = 3; C < 4000; C += 2)
			{
				bool bPrime = true;
				for (uint32 Dv = 3; Dv * Dv <= C; Dv += 2)
				{
					if (C % Dv == 0)
					{
						bPrime = false;
						break;
					}
				}
				if (bPrime)
				{
					SmallPrimes.Add(C);
				}
			}
		}
		auto IsProbablePrime = [&](const FLimbs& N)
		{
			for (const uint32 Sp : SmallPrimes)
			{
				if (ModSmall(N, Sp) == 0)
				{
					return false;
				}
			}
			FMont Mont;
			if (!Mont.Init(N))
			{
				return false;
			}
			// N - 1 = Dd * 2^S
			FLimbs NMinus1 = N;
			NMinus1[0] -= 1; // N is odd
			FLimbs Dd = NMinus1;
			int32 S = 0;
			while ((Dd[0] & 1) == 0)
			{
				for (int32 i = 0; i < Dd.Num(); ++i)
				{
					Dd[i] = (Dd[i] >> 1) | (i + 1 < Dd.Num() ? Dd[i + 1] << 31 : 0u);
				}
				++S;
			}
			const TArray<uint8> DdBytes = ToBytes(Dd, Dd.Num() * 4);
			FLimbs One;
			One.SetNumZeroed(N.Num());
			One[0] = 1;
			const TArray<uint8> Two = { 2 };
			for (int32 Round = 0; Round < 8; ++Round)
			{
				uint32 BaseValue = 0;
				SecureRandomBytes(reinterpret_cast<uint8*>(&BaseValue), sizeof(BaseValue));
				FLimbs Base;
				Base.SetNumZeroed(N.Num());
				Base[0] = 2 + BaseValue % 0x7ffffff0u;
				FLimbs X = Mont.Pow(Base, DdBytes);
				if (Compare(X, One) == 0 || Compare(X, NMinus1) == 0)
				{
					continue;
				}
				bool bWitness = true;
				for (int32 i = 1; i < S; ++i)
				{
					X = Mont.Pow(X, Two);
					if (Compare(X, NMinus1) == 0)
					{
						bWitness = false;
						break;
					}
				}
				if (bWitness)
				{
					return false;
				}
			}
			return true;
		};
		auto RandomPrime = [&](FLimbs& OutP)
		{
			TArray<uint8> Rand;
			Rand.SetNumUninitialized(HalfLimbs * 4);
			for (int32 Attempt = 0; Attempt < 100000; ++Attempt)
			{
				if (!SecureRandomBytes(Rand.GetData(), Rand.Num()))
				{
					return false;
				}
				FLimbs C = FromBytes(Rand, HalfLimbs);
				// the top two bits set (the product then has exactly Bits bits), odd, and 2 mod 3 (gcd(3, p - 1) = 1)
				C.Last() |= 0xc0000000u;
				C[0] |= 1;
				const uint32 M3 = ModSmall(C, 3);
				if (M3 != 2)
				{
					continue;
				}
				if (IsProbablePrime(C))
				{
					OutP = C;
					return true;
				}
			}
			return false;
		};
		auto Mul = [](const FLimbs& A, const FLimbs& B)
		{
			FLimbs R;
			R.SetNumZeroed(A.Num() + B.Num());
			for (int32 i = 0; i < A.Num(); ++i)
			{
				uint64 C = 0;
				for (int32 j = 0; j < B.Num(); ++j)
				{
					const uint64 T = uint64(R[i + j]) + uint64(A[i]) * B[j] + C;
					R[i + j] = uint32(T);
					C = T >> 32;
				}
				R[i + B.Num()] = uint32(C);
			}
			return R;
		};
		FLimbs Pp, Qq;
		do
		{
			if (!RandomPrime(Pp) || !RandomPrime(Qq))
			{
				return false;
			}
		}
		while (Compare(Pp, Qq) == 0);
		const FLimbs N = Mul(Pp, Qq);
		FLimbs P1 = Pp, Q1 = Qq;
		P1[0] -= 1;
		Q1[0] -= 1;
		// phi = (p - 1)(q - 1) = 1 mod 3 (both factors are 1 mod 3), so d = (2 phi + 1) / 3 is 3's inverse mod phi
		FLimbs Dd = Mul(P1, Q1);
		Dd.Add(0);
		uint32 Carry = 1;
		for (int32 i = 0; i < Dd.Num(); ++i)
		{
			const uint32 Top = Dd[i] >> 31;
			Dd[i] = (Dd[i] << 1) | Carry;
			Carry = Top;
		}
		uint64 Rem = 0;
		for (int32 i = Dd.Num() - 1; i >= 0; --i)
		{
			const uint64 Cur = (Rem << 32) | Dd[i];
			Dd[i] = uint32(Cur / 3);
			Rem = Cur % 3;
		}
		if (Rem != 0)
		{
			return false;
		}
		Out.N = Trim(ToBytes(N, N.Num() * 4));
		Out.D = Trim(ToBytes(Dd, Dd.Num() * 4));
		// a key that cannot sign is never written
		TArray<uint8> Sig;
		return Out.N.Num() * 8 == Bits && RsaSign(Out, StringBytes(TEXT("OpenUT2k4 key check")), Sig);
	}

	FString SignedResponseMessage(const FString& Challenge, const FString& Dialed)
	{
		return FString(ResponseSigPrefix()) + Challenge + TEXT(";") + Dialed;
	}

	int32 ChallengePort(const FString& Challenge)
	{
		TArray<FString> F;
		Challenge.ParseIntoArray(F, TEXT(";"), false);
		int32 Colon = INDEX_NONE;
		if (F.Num() < 3 || !F[2].FindLastChar(TEXT(':'), Colon))
		{
			return -1;
		}
		const FString Port = F[2].Mid(Colon + 1);
		return Port.IsNumeric() && !Port.IsEmpty() ? FCString::Atoi(*Port) : -1;
	}

	bool ParseDialed(const FString& Dialed, FString& OutIP, int32& OutPort, FString& OutHost)
	{
		FString Addr = Dialed;
		OutHost.Reset();
		Dialed.Split(TEXT("/"), &Addr, &OutHost);
		int32 Colon = INDEX_NONE;
		if (!Addr.FindLastChar(TEXT(':'), Colon))
		{
			return false;
		}
		OutIP = Addr.Left(Colon);
		const FString Port = Addr.Mid(Colon + 1);
		OutPort = FCString::Atoi(*Port);
		return !OutIP.IsEmpty() && Port.IsNumeric() && OutPort > 0 && OutPort < 65536 && Dialed.Len() <= 120 && !Dialed.Contains(TEXT(";"));
	}

	int32 SelfTest(TArray<FString>& OutLines)
	{
		int32 Failed = 0;
		auto Check = [&](bool bOk, const FString& What)
		{
			OutLines.Add(FString::Printf(TEXT("%s %s"), bOk ? TEXT("ok  ") : TEXT("FAIL"), *What));
			Failed += bOk ? 0 : 1;
		};
		// FIPS 180-4 examples
		Check(Sha256Hex(TEXT("abc")) == TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), TEXT("SHA-256 \"abc\""));
		Check(Sha256Hex(TEXT("")) == TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), TEXT("SHA-256 empty"));
		Check(Sha256Hex(TEXT("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) == TEXT("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), TEXT("SHA-256 two blocks"));
		{
			TArray<uint8> Million;
			Million.Init('a', 1000000);
			uint8 H[32];
			Sha256(Million.GetData(), Million.Num(), H);
			Check(BytesToHex(H, 32) == TEXT("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), TEXT("SHA-256 a million a"));
		}
		// RSA test vector made by Client/Tools/openut2k4_signing.py with a throw-away 1024-bit TEST key (not a signing key
		// of any build): its signature of "OpenUT2k4" must verify, sign the same, and tampering must fail
		FRsaPrivateKey Key;
		HexToBytes(FString(
			TEXT("ca8341525326f7c60ed44479af8e8ed3a33371ce0edac5a2e715c79d65a97134ffd0eeef37cf7c29dde36124dfa09482")
			TEXT("30ebd7d14ec81699a974ca6b4d31ba7c359f25ff1b49d7f0ad01de1500260a4c0efa0a2f43958b45df776c01503097ca")
			TEXT("08309b57853475c07a1424232e6e20b24baf5bd35214633bfcf116caffc8a4bf")), Key.N);
		HexToBytes(FString(
			TEXT("87022b8c376f4fd95f382da67509b48d1777a13409e72e6c9a0e8513991ba0cdffe09f4a2534fd713e9796189515b856")
			TEXT("cb47e5363485646670f886f233767c519d0e94b1bd472f6ac6891ffaa4db9fdc4163bab793dc9cf60336fcab88aa19d8")
			TEXT("d814eedf3154d60ba370ee8b866cbfb21d1932610b303bee0c5c403f54880e7b")), Key.D);
		TArray<uint8> PySig;
		HexToBytes(FString(
			TEXT("9dfb55b235b8bdf30208aa42e498c845070717b73d49ac8bb4cc4c5e186245b5295247825a757956d69e189b4756f789")
			TEXT("bb5ced24be88379973e7fd0df5d7d2f73898518e1ac596871826839db2a655535796bc664d31988887f5ba1609948b57")
			TEXT("4f00e707c9709591899dc6af75a90f244aaf5e4d26c5d4d9b4c17b0bf2a4b01d")), PySig);
		const TArray<uint8> Msg = StringBytes(TEXT("OpenUT2k4"));
		Check(RsaVerify(Key.N, Msg, PySig), TEXT("RSA verify (Python signature)"));
		TArray<uint8> Sig;
		Check(RsaSign(Key, Msg, Sig) && Sig == PySig, TEXT("RSA sign (same as Python)"));
		Check(!RsaVerify(Key.N, StringBytes(TEXT("OpenUT2k5")), PySig), TEXT("RSA other message fails"));
		TArray<uint8> Bad = PySig;
		Bad[Bad.Num() / 2] ^= 0x10;
		Check(!RsaVerify(Key.N, Msg, Bad), TEXT("RSA tampered signature fails"));
		TArray<uint8> OtherN = Key.N;
		OtherN[10] ^= 0x01;
		Check(!RsaVerify(OtherN, Msg, PySig), TEXT("RSA wrong key fails"));
		{
			// Ed25519 test vector (made with an independent implementation; the key is the bytes 0..31, test only)
			TArray<uint8> Pub, EdSig;
			HexToBytes(TEXT("03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8"), Pub);
			HexToBytes(FString(TEXT("e52427b44d0ad63cf1cd99290d63f4b3b06babd10fbed2a39f338ae75c858b2f")) + TEXT("55e45f085a2ae0a46d896283500e77d3d0c4bada972505d04ef1695b48f41d02"), EdSig);
			const TArray<uint8> EdMsg = StringBytes(TEXT("OUT2K4BANLIST1 test"));
			uint8 H512[64];
			Sha512(reinterpret_cast<const uint8*>("abc"), 3, H512);
			Check(BytesToHex(H512, 64) == TEXT("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"), TEXT("SHA-512 \"abc\""));
			const double T0 = FPlatformTime::Seconds();
			const bool bEdOk = Ed25519Verify(Pub.GetData(), EdMsg.GetData(), EdMsg.Num(), EdSig.GetData());
			const double T1 = FPlatformTime::Seconds();
			Check(bEdOk, FString::Printf(TEXT("Ed25519 verify (%.1f ms)"), (T1 - T0) * 1000.0));
			TArray<uint8> BadMsg = EdMsg;
			BadMsg[0] ^= 1;
			Check(!Ed25519Verify(Pub.GetData(), BadMsg.GetData(), BadMsg.Num(), EdSig.GetData()), TEXT("Ed25519 other message fails"));
			EdSig[40] ^= 1;
			Check(!Ed25519Verify(Pub.GetData(), EdMsg.GetData(), EdMsg.Num(), EdSig.GetData()), TEXT("Ed25519 tampered signature fails"));
		}
		{
			FString Ip, Host;
			int32 Port = 0;
			Check(ChallengePort(TEXT("OUTV1;abc;10.0.0.1:7777;DM-Rankin;3")) == 7777 && ChallengePort(TEXT("OUTV1;abc;*:7787;DM-X;3")) == 7787
				&& ChallengePort(TEXT("OUTV1;abc")) == -1 && ParseDialed(TEXT("10.0.0.1:7777/ut.example.org"), Ip, Port, Host) && Ip == TEXT("10.0.0.1")
				&& Port == 7777 && Host == TEXT("ut.example.org") && !ParseDialed(TEXT("nonsense"), Ip, Port, Host), TEXT("challenge port and dialed address"));
		}
		Check(Fingerprint(BytesToHex(Key.N.GetData(), Key.N.Num())) == TEXT("a034932253a683e7716a715684f61a54"), TEXT("fingerprint"));
		{
			const double T0 = FPlatformTime::Seconds();
			FRsaPrivateKey NewKey;
			const bool bMade = GenerateKey(NewKey);
			const double T1 = FPlatformTime::Seconds();
			TArray<uint8> NewSig;
			const bool bSigned = bMade && RsaSign(NewKey, Msg, NewSig);
			const double T2 = FPlatformTime::Seconds();
			const bool bVerified = bSigned && RsaVerify(NewKey.N, Msg, NewSig);
			const double T3 = FPlatformTime::Seconds();
			FRsaPrivateKey Parsed;
			FString Err;
			const bool bParsed = ParseKeyFile(FormatKeyFile(NewKey, TEXT("test")), Parsed, Err) && Parsed.N == NewKey.N && Parsed.D == NewKey.D;
			Check(bMade && bVerified && bParsed && NewKey.N.Num() == 256, FString::Printf(TEXT("RSA-2048 key generation (%.0f ms), sign (%.1f ms), verify (%.2f ms), key file"),
				(T1 - T0) * 1000.0, (T2 - T1) * 1000.0, (T3 - T2) * 1000.0));
		}
		return Failed;
	}
}
