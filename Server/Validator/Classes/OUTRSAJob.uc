//=============================================================================
// OUTRSAJob: checks one RSA signature (PKCS#1 v1.5, SHA-256, public exponent
// 3) in small steps, so the work spreads over server ticks.
//
// s^3 = EM (mod n) is checked with Montgomery multiplication on 15-bit limbs
// (products stay below 2^31 in UnrealScript's signed ints):
//   X = Mont(Mont(s, s), s) = s^3 R^-2,  Y = Mont(Mont(EM, 1), 1) = EM R^-2
// and X == Y exactly when s^3 mod n == EM (R is invertible mod n). No division
// and no precomputed constants are needed. The whole encoded block is
// compared, never parsed: exponent 3 is safe that way.
//
// Part of OpenUT2k4 (MIT licence). Not affiliated with Epic Games.
//=============================================================================
class OUTRSAJob extends Object;

const LIMB_BITS = 15;
const LIMB_MASK = 32767;

var array<int> N, S, EM, One, A, B, T, X, Y;
var int L, N0Inv;
// 1..4: the four multiplications, 5: done
var int Phase, Row;
// 0 running, 1 the signature is valid, 2 invalid
var int Result;
var int RowsDone;
var string FailReason;

static final function BytesToLimbs(array<byte> Bytes, int NumLimbs, out array<int> Out)
{
	local int i, Acc, Bits, Li;

	Out.Length = NumLimbs;
	for (i = 0; i < NumLimbs; i++)
		Out[i] = 0;
	for (i = Bytes.Length - 1; i >= 0 && Li < NumLimbs; i--)
	{
		Acc = Acc | (Bytes[i] << Bits);
		Bits += 8;
		while (Bits >= LIMB_BITS && Li < NumLimbs)
		{
			Out[Li++] = Acc & LIMB_MASK;
			Acc = Acc >>> LIMB_BITS;
			Bits -= LIMB_BITS;
		}
	}
	if (Bits > 0 && Li < NumLimbs)
		Out[Li] = Acc;
}

// Modulus (big-endian, 128..512 bytes, odd), signature (same length), SHA-256 digest of the message (32 bytes).
function bool Init(array<byte> Modulus, array<byte> Sig, array<byte> Digest)
{
	local array<byte> E;
	local int K, i, Xi, Tt;
	local array<byte> Prefix;

	Result = 0;
	FailReason = "";
	K = Modulus.Length;
	if (K < 128 || K > 512 || Modulus[0] == 0 || (Modulus[K - 1] & 1) == 0)
		return Fail("bad public key");
	if (Sig.Length != K)
		return Fail("bad signature length");
	if (Digest.Length != 32)
		return Fail("bad digest");
	// EM = 00 01 FF..FF 00 || DigestInfo(SHA-256) || digest
	class'OUTCrypto'.static.HexToBytes("3031300d060960864801650304020105000420", Prefix);
	E.Length = K;
	E[0] = 0;
	E[1] = 1;
	for (i = 2; i < K - 52; i++)
		E[i] = 255;
	E[K - 52] = 0;
	for (i = 0; i < 19; i++)
		E[K - 51 + i] = Prefix[i];
	for (i = 0; i < 32; i++)
		E[K - 32 + i] = Digest[i];

	L = (K * 8 + LIMB_BITS - 1) / LIMB_BITS;
	BytesToLimbs(Modulus, L, N);
	BytesToLimbs(Sig, L, S);
	BytesToLimbs(E, L, EM);
	One.Length = L;
	for (i = 0; i < L; i++)
		One[i] = 0;
	One[0] = 1;
	// the signature must be below the modulus
	if (CompareLimbs(S, N, L) >= 0)
		return Fail("signature out of range");
	// -n^-1 mod 2^15 (Newton: each step doubles the correct low bits; n * n = 1 mod 8 for odd n)
	Xi = N[0];
	for (i = 0; i < 4; i++)
	{
		Tt = (N[0] * Xi) & LIMB_MASK;
		Tt = (2 - Tt) & LIMB_MASK;
		Xi = (Xi * Tt) & LIMB_MASK;
	}
	N0Inv = (32768 - Xi) & LIMB_MASK;
	Phase = 1;
	StartMul(S, S);
	return true;
}

function bool Fail(string Why)
{
	Result = 2;
	FailReason = Why;
	Phase = 5;
	return false;
}

static final function int CompareLimbs(array<int> P, array<int> Q, int Count)
{
	local int i;

	for (i = Count - 1; i >= 0; i--)
	{
		if (P[i] != Q[i])
		{
			if (P[i] < Q[i])
				return -1;
			return 1;
		}
	}
	return 0;
}

function StartMul(array<int> InA, array<int> InB)
{
	local int i;

	A = InA;
	B = InB;
	T.Length = L + 2;
	for (i = 0; i < L + 2; i++)
		T[i] = 0;
	Row = 0;
}

// One row of the CIOS Montgomery multiplication: T = (T + A * B[Row] + m * N) / 2^15.
function MulRow()
{
	local int j, Bi, C, Sum, M;

	Bi = B[Row];
	if (Bi != 0)
	{
		C = 0;
		for (j = 0; j < L; j++)
		{
			Sum = T[j] + A[j] * Bi + C;
			T[j] = Sum & LIMB_MASK;
			C = Sum >>> LIMB_BITS;
		}
		Sum = T[L] + C;
		T[L] = Sum & LIMB_MASK;
		T[L + 1] = Sum >>> LIMB_BITS;
	}
	M = (T[0] * N0Inv) & LIMB_MASK;
	Sum = T[0] + M * N[0];
	C = Sum >>> LIMB_BITS;
	for (j = 1; j < L; j++)
	{
		Sum = T[j] + M * N[j] + C;
		T[j - 1] = Sum & LIMB_MASK;
		C = Sum >>> LIMB_BITS;
	}
	Sum = T[L] + C;
	T[L - 1] = Sum & LIMB_MASK;
	T[L] = T[L + 1] + (Sum >>> LIMB_BITS);
	T[L + 1] = 0;
}

// T (L + 1 limbs) reduced below N, as L limbs.
function FinishMul(out array<int> Out)
{
	local int i, Borrow, D;
	local bool bSub;

	bSub = T[L] != 0;
	if (!bSub)
	{
		bSub = true;
		for (i = L - 1; i >= 0; i--)
		{
			if (T[i] != N[i])
			{
				bSub = T[i] > N[i];
				break;
			}
		}
	}
	Out.Length = L;
	Borrow = 0;
	for (i = 0; i < L; i++)
	{
		if (bSub)
		{
			D = T[i] - N[i] - Borrow;
			Borrow = 0;
			if (D < 0)
			{
				D += 32768;
				Borrow = 1;
			}
			Out[i] = D;
		}
		else
			Out[i] = T[i];
	}
}

// Does up to Budget rows of work. Returns Result (0 still running).
function int Step(int Budget)
{
	while (Budget > 0 && Result == 0)
	{
		MulRow();
		Row++;
		RowsDone++;
		Budget--;
		if (Row < L)
			continue;
		switch (Phase)
		{
			case 1:	FinishMul(X); Phase = 2; StartMul(X, S); break;
			case 2:	FinishMul(X); Phase = 3; StartMul(EM, One); break;
			case 3:	FinishMul(Y); Phase = 4; StartMul(Y, One); break;
			case 4:
				FinishMul(Y);
				Phase = 5;
				if (CompareLimbs(X, Y, L) == 0)
					Result = 1;
				else
				{
					Result = 2;
					FailReason = "bad signature";
				}
				break;
		}
	}
	return Result;
}

// Rows a whole check takes (four multiplications of L rows).
function int TotalRows()
{
	return 4 * L;
}

defaultproperties
{
}
