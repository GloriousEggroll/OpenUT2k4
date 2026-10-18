//=============================================================================
// OUTCrypto: SHA-256 (FIPS 180-4) and hex helpers in UnrealScript, for the
// OpenUT2k4 Validator (Server/Docs/Validator.md).
//
// UnrealScript ints are 32-bit and wrap on overflow, so the 32-bit SHA-256
// arithmetic maps directly; >>> is the unsigned shift.
//
// Part of OpenUT2k4 (MIT licence). Not affiliated with Epic Games.
//=============================================================================
class OUTCrypto extends Object;

var int K[64];
var int H0[8];

// ---- hex ----

static final function int HexDigit(string C)
{
	local int A;

	A = Asc(C);
	if (A >= 48 && A <= 57)
		return A - 48;
	if (A >= 97 && A <= 102)
		return A - 87;
	if (A >= 65 && A <= 70)
		return A - 55;
	return -1;
}

// Hex text to bytes (an odd length gets a leading 0). False on a non-hex character.
static final function bool HexToBytes(string Hex, out array<byte> Out)
{
	local int i, n, Hi, Lo;

	if ((Len(Hex) & 1) == 1)
		Hex = "0" $ Hex;
	n = Len(Hex) / 2;
	Out.Length = n;
	for (i = 0; i < n; i++)
	{
		Hi = HexDigit(Mid(Hex, i * 2, 1));
		Lo = HexDigit(Mid(Hex, i * 2 + 1, 1));
		if (Hi < 0 || Lo < 0)
			return false;
		Out[i] = Hi * 16 + Lo;
	}
	return true;
}

static final function string IntToHex(int V)
{
	local string S;
	local int i;

	for (i = 7; i >= 0; i--)
		S = S $ Mid("0123456789abcdef", (V >>> (i * 4)) & 15, 1);
	return S;
}

// Lower case, no leading zeros, no spaces: the form public keys are fingerprinted in.
static final function string NormalizeHex(string Hex)
{
	local int i;

	Hex = Locs(Hex);
	while (Len(Hex) > 1 && Left(Hex, 1) == "0")
		Hex = Mid(Hex, 1);
	for (i = 0; i < Len(Hex); i++)
		if (HexDigit(Mid(Hex, i, 1)) < 0)
			return "";
	return Hex;
}

// ---- SHA-256 ----

// SHA-256 of a string's characters (each taken as one byte: ASCII), as 64 lower-case hex digits.
function string Sha256String(string S)
{
	local array<byte> B;
	local int i, n;

	n = Len(S);
	B.Length = n;
	for (i = 0; i < n; i++)
		B[i] = Asc(Mid(S, i, 1)) & 255;
	return Sha256Hex(B);
}

// The SHA-256 of a string's characters as 32 bytes.
function Sha256StringBytes(string S, out array<byte> Out)
{
	local array<byte> B;
	local int i, n;

	n = Len(S);
	B.Length = n;
	for (i = 0; i < n; i++)
		B[i] = Asc(Mid(S, i, 1)) & 255;
	Sha256Bytes(B, Out);
}

function string Sha256Hex(array<byte> Msg)
{
	local array<int> D;
	local int i;
	local string S;

	Sha256(Msg, D);
	for (i = 0; i < 8; i++)
		S = S $ IntToHex(D[i]);
	return S;
}

// The digest as 32 bytes.
function Sha256Bytes(array<byte> Msg, out array<byte> Out)
{
	local array<int> D;
	local int i;

	Sha256(Msg, D);
	Out.Length = 32;
	for (i = 0; i < 32; i++)
		Out[i] = (D[i / 4] >>> (24 - (i & 3) * 8)) & 255;
}

// The digest as 8 ints (big-endian words).
function Sha256(array<byte> Msg, out array<int> Digest)
{
	local int W[64];
	local int a, b, c, d, e, f, g, h, t1, t2, s0, s1;
	local int i, j, n, Total, Off, Byte;
	local array<int> HH;

	n = Msg.Length;
	// padding: 0x80, zeros, then the bit length as 8 big-endian bytes (messages here are far below 2^28 bytes)
	Total = ((n + 8) / 64 + 1) * 64;
	HH.Length = 8;
	for (i = 0; i < 8; i++)
		HH[i] = H0[i];
	for (Off = 0; Off < Total; Off += 64)
	{
		for (i = 0; i < 16; i++)
		{
			W[i] = 0;
			for (j = 0; j < 4; j++)
			{
				Byte = Off + i * 4 + j;
				if (Byte < n)
					W[i] = W[i] | (Msg[Byte] << (24 - j * 8));
				else if (Byte == n)
					W[i] = W[i] | (128 << (24 - j * 8));
				else if (Byte >= Total - 4)
					W[i] = W[i] | ((((n * 8) >>> ((Total - 1 - Byte) * 8)) & 255) << (24 - j * 8));
			}
		}
		for (i = 16; i < 64; i++)
		{
			s0 = ((W[i-15] >>> 7) | (W[i-15] << 25)) ^ ((W[i-15] >>> 18) | (W[i-15] << 14)) ^ (W[i-15] >>> 3);
			s1 = ((W[i-2] >>> 17) | (W[i-2] << 15)) ^ ((W[i-2] >>> 19) | (W[i-2] << 13)) ^ (W[i-2] >>> 10);
			W[i] = W[i-16] + s0 + W[i-7] + s1;
		}
		a = HH[0]; b = HH[1]; c = HH[2]; d = HH[3]; e = HH[4]; f = HH[5]; g = HH[6]; h = HH[7];
		for (i = 0; i < 64; i++)
		{
			s1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
			t1 = h + s1 + ((e & f) ^ ((~e) & g)) + K[i] + W[i];
			s0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
			t2 = s0 + ((a & b) ^ (a & c) ^ (b & c));
			h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
		}
		HH[0] += a; HH[1] += b; HH[2] += c; HH[3] += d; HH[4] += e; HH[5] += f; HH[6] += g; HH[7] += h;
	}
	Digest = HH;
}

defaultproperties
{
     K(0)=1116352408
     K(1)=1899447441
     K(2)=-1245643825
     K(3)=-373957723
     K(4)=961987163
     K(5)=1508970993
     K(6)=-1841331548
     K(7)=-1424204075
     K(8)=-670586216
     K(9)=310598401
     K(10)=607225278
     K(11)=1426881987
     K(12)=1925078388
     K(13)=-2132889090
     K(14)=-1680079193
     K(15)=-1046744716
     K(16)=-459576895
     K(17)=-272742522
     K(18)=264347078
     K(19)=604807628
     K(20)=770255983
     K(21)=1249150122
     K(22)=1555081692
     K(23)=1996064986
     K(24)=-1740746414
     K(25)=-1473132947
     K(26)=-1341970488
     K(27)=-1084653625
     K(28)=-958395405
     K(29)=-710438585
     K(30)=113926993
     K(31)=338241895
     K(32)=666307205
     K(33)=773529912
     K(34)=1294757372
     K(35)=1396182291
     K(36)=1695183700
     K(37)=1986661051
     K(38)=-2117940946
     K(39)=-1838011259
     K(40)=-1564481375
     K(41)=-1474664885
     K(42)=-1035236496
     K(43)=-949202525
     K(44)=-778901479
     K(45)=-694614492
     K(46)=-200395387
     K(47)=275423344
     K(48)=430227734
     K(49)=506948616
     K(50)=659060556
     K(51)=883997877
     K(52)=958139571
     K(53)=1322822218
     K(54)=1537002063
     K(55)=1747873779
     K(56)=1955562222
     K(57)=2024104815
     K(58)=-2067236844
     K(59)=-1933114872
     K(60)=-1866530822
     K(61)=-1538233109
     K(62)=-1090935817
     K(63)=-965641998
     H0(0)=1779033703
     H0(1)=-1150833019
     H0(2)=1013904242
     H0(3)=-1521486534
     H0(4)=1359893119
     H0(5)=-1694144372
     H0(6)=528734635
     H0(7)=1541459225
}
