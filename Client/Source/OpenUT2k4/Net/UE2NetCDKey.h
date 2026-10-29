#pragma once

#include "CoreMinimal.h"

/**
 * The UT2004 login identity, as the OldUnreal 3374 client sends it. See Docs/UE2Net.md "CD key and AUTH".
 *
 * The secret is always a CD key: the UT2004 install's (read only when joining, hashed and dropped), or, when the install
 * has none, one generated into it like the OldUnreal 3374 client does (EnsureInstallKey). Only hashes reach the server;
 * the key and the hashes are never logged.
 */
namespace UE2NetCDKey
{
	/** AUTH HASH: the lower-case hex MD5 of the secret (OldUnreal GetCDKeyHash). Never empty. */
	FString ComputeAuthHash();

	/** AUTH GM: 132 hex digits, random apart from the client's (all-zero) code MD5 hidden in them. */
	FString MakeGM();

	/** AUTH PASSWORD for a stats account: the password XORed with the MD5 hex of the secret + "STATS", as hex. */
	FString EncryptStatsPassword(const FString& Password);

	/** Answer to a ClientValidate challenge (ServerValidationResponse): MD5 of the secret followed by the challenge. */
	FString ComputeResponse(const FString& Challenge);

	/** True when a CD key can be read from the player's install (for the UI; never exposes the key). */
	bool HasKey();

	/**
	 * Gives an install without a CD key a new random one, in System/cdkey and in Registry_Fix.reg (for Wine). Does
	 * nothing when the install already has a key. False when it has none and cannot be written.
	 */
	bool EnsureInstallKey(const FString& Install);
}
