#include "UE2NetCDKey.h"

#include "../ArenaContent.h"
#include "../ArenaUserSettings.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace UE2NetCDKeyLocal
{
	/** Overwrites a string's characters before it is freed (best effort: the key should not linger in memory). */
	void Wipe(FString& S)
	{
		for (TCHAR& C : S.GetCharArray())
		{
			C = 0;
		}
		S.Empty();
	}

	/** Lower-case hex MD5 of the 8-bit text of Prefix + Text. */
	FString Md5Hex(const ANSICHAR* Prefix, const FString& Text)
	{
		TArray<uint8> Bytes;
		for (const ANSICHAR* P = Prefix; P && *P; ++P)
		{
			Bytes.Add(uint8(*P));
		}
		for (TCHAR C : Text)
		{
			Bytes.Add(uint8(C));
		}
		uint8 Digest[16];
		FMD5 Md5;
		Md5.Update(Bytes.GetData(), Bytes.Num());
		Md5.Final(Digest);
		FMemory::Memzero(Bytes.GetData(), Bytes.Num());
		return BytesToHexLower(Digest, 16);
	}

	/** Canonical key text: letters and digits upper case, in dash-separated groups of five (the UT2004 layout). */
	FString Normalize(const FString& Raw)
	{
		FString Chars;
		for (TCHAR C : Raw)
		{
			if (FChar::IsAlnum(C))
			{
				Chars.AppendChar(FChar::ToUpper(C));
			}
		}
		FString Out;
		for (int32 i = 0; i < Chars.Len(); ++i)
		{
			if (i > 0 && i % 5 == 0)
			{
				Out.AppendChar(TEXT('-'));
			}
			Out.AppendChar(Chars[i]);
		}
		Wipe(Chars);
		return Out;
	}

	/** Reads CDKey="..." from a Windows .reg export (a UT2004 Registry_Fix.reg next to the install). */
	bool ReadKeyFromReg(const FString& RegPath, FString& OutKey)
	{
		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *RegPath))
		{
			return false;
		}
		bool bOk = false;
		for (FString Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (Line.StartsWith(TEXT("\"CDKey\""), ESearchCase::IgnoreCase))
			{
				int32 Eq = INDEX_NONE;
				if (Line.FindChar(TEXT('='), Eq))
				{
					FString Value = Line.Mid(Eq + 1).TrimStartAndEnd().TrimQuotes();
					if (!Value.IsEmpty() && !Value.Equals(TEXT("PWCKV-REMOVED"), ESearchCase::IgnoreCase))
					{
						OutKey = Normalize(Value);
						bOk = !OutKey.IsEmpty();
					}
					Wipe(Value);
				}
			}
			Wipe(Line);
		}
		return bOk;
	}

	/** The player's CD key: System/cdkey, a Registry_Fix.reg next to the install, or the Windows registry. */
	bool ReadKey(FString& OutKey)
	{
		OutKey.Empty();
#if !UE_BUILD_SHIPPING
		// -UE2NoCDKey: test the no-key login (as with an OldUnreal install, which has no key) on a keyed install
		if (FParse::Param(FCommandLine::Get(), TEXT("UE2NoCDKey")))
		{
			return false;
		}
#endif
		const FString Install = ArenaContent::FindUT2004Install();
		if (!Install.IsEmpty())
		{
			// case-insensitive "System/cdkey"
			FString Found;
			IFileManager::Get().IterateDirectory(*FPaths::Combine(Install, TEXT("System")), [&Found](const TCHAR* Path, bool bIsDir)
			{
				if (!bIsDir && FPaths::GetCleanFilename(Path).Equals(TEXT("cdkey"), ESearchCase::IgnoreCase))
				{
					Found = Path;
					return false;
				}
				return true;
			});
			FString Raw;
			if (!Found.IsEmpty() && FFileHelper::LoadFileToString(Raw, *Found, FFileHelper::EHashOptions::None, FILEREAD_Silent))
			{
				OutKey = Normalize(Raw);
				Wipe(Raw);
			}
			// Registry export kept next to the install (the user's Linux layout): <install>/Registry_Fix.reg, or one directory up
			if (OutKey.IsEmpty())
			{
				for (const FString& Dir : { Install, FPaths::GetPath(Install) })
				{
					const FString Reg = FPaths::Combine(Dir, TEXT("Registry_Fix.reg"));
					if (IFileManager::Get().FileExists(*Reg) && ReadKeyFromReg(Reg, OutKey))
					{
						break;
					}
				}
			}
		}
#if PLATFORM_WINDOWS
		if (OutKey.IsEmpty())
		{
			FString Raw;
			if (FWindowsPlatformMisc::QueryRegKey(HKEY_LOCAL_MACHINE, TEXT("SOFTWARE\\Unreal Technology\\Installed Apps\\UT2004"), TEXT("CDKey"), Raw)
				|| FWindowsPlatformMisc::QueryRegKey(HKEY_LOCAL_MACHINE, TEXT("SOFTWARE\\WOW6432Node\\Unreal Technology\\Installed Apps\\UT2004"), TEXT("CDKey"), Raw))
			{
				OutKey = Normalize(Raw);
				Wipe(Raw);
			}
		}
#endif
		return !OutKey.IsEmpty();
	}
}

namespace UE2NetCDKeyLocal
{
	/**
	 * The secret behind the AUTH HASH: the player's CD key when the install has one, otherwise the per-install
	 * NetIdentity (made on first use and saved). The OldUnreal 3374 client in the same situation creates a unique key
	 * of its own and keeps it in System/cdkey; NetIdentity plays that role without being a key.
	 */
	void ReadSecret(FString& OutSecret)
	{
		if (ReadKey(OutSecret))
		{
			return;
		}
		UArenaUserSettings* Settings = UArenaUserSettings::Get();
		bool bValid = Settings->NetIdentity.Len() == 32;
		for (TCHAR C : Settings->NetIdentity)
		{
			bValid = bValid && FChar::IsHexDigit(C);
		}
		if (!bValid)
		{
			Settings->NetIdentity = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
			Settings->Save();
		}
		OutSecret = Settings->NetIdentity;
	}
}

FString UE2NetCDKey::ComputeAuthHash()
{
	using namespace UE2NetCDKeyLocal;
	// OldUnreal 3374 GetCDKeyHash: the plain MD5 of the key text (the server salts it itself, SecureCDKeyHash)
	FString Secret;
	ReadSecret(Secret);
	FString Hash = Md5Hex("", Secret);
	Wipe(Secret);
	return Hash;
}

FString UE2NetCDKey::MakeGM()
{
	// OldUnreal 3374 UNetPendingLevel::NotifyReceivedText: 66 bytes as lower-case hex. Bytes 0-63 are random; the
	// 16 bytes of the client's GMD5 (all zero in the 3374 client), each XORed with X + i, are interleaved at a random
	// offset O (0-31): byte O + 1 + 2i = (previous & 0x0F) | (this & 0xF0), byte O + 2 + 2i = this. Byte 64 is X, 65 is O.
	uint8 Buf[66];
	for (int32 i = 0; i < 64; ++i)
	{
		Buf[i] = uint8(FMath::RandRange(0, 255));
	}
	const uint8 X = uint8(FMath::RandRange(0, 255));
	const uint8 O = uint8(FMath::RandRange(0, 31));
	Buf[64] = X;
	Buf[65] = O;
	static const uint8 ClientMD5[16] = {};
	uint8 Prev = Buf[O];
	for (int32 i = 0; i < 16; ++i)
	{
		const uint8 K = uint8(ClientMD5[i] ^ uint8(X + i));
		Buf[O + 1 + 2 * i] = uint8((Prev & 0x0F) | (K & 0xF0));
		Buf[O + 2 + 2 * i] = K;
		Prev = K;
	}
	return BytesToHexLower(Buf, 66);
}

FString UE2NetCDKey::EncryptStatsPassword(const FString& Password)
{
	using namespace UE2NetCDKeyLocal;
	// OldUnreal 3374 EncryptWithCDKeyHash(Password, "STATS"): H = MD5 hex of secret + "STATS"; each character XORed
	// with H[i & 15], written as two hex digits
	FString Secret;
	ReadSecret(Secret);
	FString H = Md5Hex("", Secret + TEXT("STATS"));
	Wipe(Secret);
	FString Out;
	for (int32 i = 0; i < Password.Len(); ++i)
	{
		Out += FString::Printf(TEXT("%02x"), uint32(Password[i] ^ H[i & 15]) & 0xFFFF);
	}
	Wipe(H);
	return Out;
}

FString UE2NetCDKey::ComputeResponse(const FString& Challenge)
{
	using namespace UE2NetCDKeyLocal;
	// OldUnreal 3374 GetCDKeyResponse: the MD5 of the key followed by the challenge
	FString Secret;
	ReadSecret(Secret);
	FString Response = Md5Hex("", Secret + Challenge);
	Wipe(Secret);
	return Response;
}

bool UE2NetCDKey::HasKey()
{
	using namespace UE2NetCDKeyLocal;
	FString Key;
	const bool bHas = ReadKey(Key);
	Wipe(Key);
	return bHas;
}
