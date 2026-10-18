#pragma once

#include "CoreMinimal.h"

/**
 * The crypto of the OpenUT2k4 Validator (Server/Docs/Validator.md), shared by the client (Net/UE2NetValidator) and our
 * own server (Net/UE2ServerValidator): SHA-256 and RSA-2048 PKCS#1 v1.5 signatures with SHA-256 and public exponent 3,
 * and making a player's key. Own code (no third-party crypto). The verification also runs in UnrealScript in the
 * validator mutator (Server/Validator/Classes/OUTCrypto.uc, OUTRSAJob.uc).
 *
 * Exponent 3 keeps verification cheap (two modular multiplications) where it is expensive: UnrealScript on a 2004
 * server. It is safe for signatures because the whole encoded block is compared, never parsed (no Bleichenbacher-style
 * forgery of a lax padding check).
 */
namespace UE2ValidatorCrypto
{
	/** Incremental SHA-256 (FIPS 180-4). */
	class OPENUT2K4_API FSha256
	{
	public:
		FSha256() { Reset(); }
		void Reset();
		void Update(const uint8* Data, int64 Len);
		void Final(uint8 Out[32]);

	private:
		void Block(const uint8* P);
		uint32 H[8];
		uint8 Buf[64];
		int32 BufLen = 0;
		uint64 TotalLen = 0;
	};

	OPENUT2K4_API void Sha256(const uint8* Data, int64 Len, uint8 Out[32]);
	/** Lower-case hex of the SHA-256 of a string's ASCII / UTF-8 bytes. */
	OPENUT2K4_API FString Sha256Hex(const FString& Text);
	OPENUT2K4_API FString BytesToHex(const uint8* Data, int32 Len);
	OPENUT2K4_API bool HexToBytes(const FString& Hex, TArray<uint8>& Out);
	/** ASCII / UTF-8 bytes of a string (no terminator). */
	OPENUT2K4_API TArray<uint8> StringBytes(const FString& Text);

	/** An RSA private key (exponent 3): modulus and private exponent, big-endian bytes. */
	struct FRsaPrivateKey
	{
		TArray<uint8> N;
		TArray<uint8> D;
		bool IsValid() const { return N.Num() >= 128 && D.Num() > 0; }
	};

	/** Reads a player key file (FormatKeyFile: "OUT2K4KEY1", n=, e=3, d=). */
	OPENUT2K4_API bool ParseKeyFile(const FString& Text, FRsaPrivateKey& Out, FString& OutError);
	/** PKCS#1 v1.5 SHA-256 signature (the modulus' length), false on a bad key. */
	OPENUT2K4_API bool RsaSign(const FRsaPrivateKey& Key, const TArray<uint8>& Message, TArray<uint8>& OutSig);
	/** Checks a PKCS#1 v1.5 SHA-256 signature made with exponent 3 against a modulus (big-endian bytes). */
	OPENUT2K4_API bool RsaVerify(const TArray<uint8>& Modulus, const TArray<uint8>& Message, const TArray<uint8>& Sig);
	/** Lower-case hex without leading zeros (the form public keys are sent and fingerprinted in). */
	OPENUT2K4_API FString NormalizeHex(const FString& Hex);

	/**
	 * Makes a new RSA-2048 key with exponent 3 (the player identity key, Net/UE2NetValidator): primes from the OS random
	 * source, Miller-Rabin. Takes about a second; call it off the game thread.
	 */
	OPENUT2K4_API bool GenerateKey(FRsaPrivateKey& Out, int32 Bits = 2048);
	/** The key file text ParseKeyFile reads. */
	OPENUT2K4_API FString FormatKeyFile(const FRsaPrivateKey& Key, const FString& Comment);
	/** Cryptographically random bytes (/dev/urandom; Windows: CoCreateGuid's random GUIDs through SHA-256). */
	OPENUT2K4_API bool SecureRandomBytes(uint8* Out, int32 Len);
	/** A player's id: the first 32 hex digits of SHA-256 over the public modulus' lower-case hex without leading zeros. */
	OPENUT2K4_API FString Fingerprint(const FString& ModulusHex);

	/** SHA-512 (FIPS 180-4). */
	OPENUT2K4_API void Sha512(const uint8* Data, int64 Len, uint8 Out[64]);
	/**
	 * Ed25519 signature check (RFC 8032, cofactored verification): the signed shared ban lists
	 * (Net/UE2ServerValidator, Client/Tools/openut2k4_signing.py banlist-sign).
	 */
	OPENUT2K4_API bool Ed25519Verify(const uint8 PublicKey[32], const uint8* Message, int64 Len, const uint8 Signature[64]);

	/** Validator protocol constants (Docs/UE2Net.md "OpenUT2k4 Validator"). */
	inline const TCHAR* ChallengePrefix() { return TEXT("OUTV1;"); }
	inline const TCHAR* ResponseSigPrefix() { return TEXT("OUT2K4RESP2;"); }
	inline const TCHAR* ResponseMagic() { return TEXT("OUTR2"); }
	/**
	 * What the player key signs: "OUT2K4RESP2;" + challenge + ";" + the address the client dialed ("ip:port", or
	 * "ip:port/host" when it joined by name). The verifier checks the dialed address against its own (relay defence).
	 */
	OPENUT2K4_API FString SignedResponseMessage(const FString& Challenge, const FString& Dialed);
	/** The server port a challenge names (its third field, "ip:port" or "*:port"), -1 when there is none. */
	OPENUT2K4_API int32 ChallengePort(const FString& Challenge);
	/** Splits a dialed address "ip:port[/host]"; false when malformed. */
	OPENUT2K4_API bool ParseDialed(const FString& Dialed, FString& OutIP, int32& OutPort, FString& OutHost);
	/** Longest challenge a client signs (the validator's are about 100 characters). */
	constexpr int32 MaxChallengeLen = 240;

	/** Test vectors (SHA-256 of FIPS examples, RSA sign / verify / tamper, key generation). Returns the failures. */
	OPENUT2K4_API int32 SelfTest(TArray<FString>& OutLines);
}
