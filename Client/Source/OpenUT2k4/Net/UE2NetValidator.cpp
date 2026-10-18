#include "UE2NetValidator.h"

#include "UE2NetConnection.h"
#include "UE2NetLog.h"
#include "UE2NetValidatorCrypto.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DelayedAutoRegister.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#if PLATFORM_UNIX || PLATFORM_MAC
#include <sys/stat.h>
#endif

namespace UE2NetValidatorLocal
{
	using namespace UE2ValidatorCrypto;

	/** Characters of the answer per part (one reliable call or control line each). */
	constexpr int32 PartLen = 240;

	struct FKeyState
	{
		FCriticalSection Lock;
		FRsaPrivateKey Key;
		FString PublicHex;
		FString Fingerprint;
		/** Why there is no key (the answer is then NOKEY). */
		FString Error;
		bool bReady = false;
		bool bLoading = false;
		/** -UE2ValidatorTest=wrongkey: a second key that signs instead (the server must reject it). */
		FRsaPrivateKey OtherKey;
	};

	FKeyState& State()
	{
		static FKeyState S;
		return S;
	}

	struct FPending
	{
		FString Challenge;
		/** The address we dialed, signed with the challenge ("ip:port[/host]"). */
		FString Dialed;
		/** Why we will not sign this challenge (it names another server's port); answered as NOKEY. */
		FString Refusal;
		/** Actor channel of the OUTValidatorLink, or 0 for a control channel challenge. */
		int32 Channel = 0;
	};
	/** Challenges waiting for the key, and the ones already answered, per connection. */
	TMap<FUE2NetConnection*, TArray<FPending>> GPending;
	TMap<FUE2NetConnection*, TSet<FString>> GAnswered;

	FString TestMode()
	{
#if UE_BUILD_SHIPPING
		return FString();
#else
		FString Mode;
		FParse::Value(FCommandLine::Get(), TEXT("UE2ValidatorTest="), Mode, false);
		return Mode;
#endif
	}

	bool MakeKey(FRsaPrivateKey& Out, FString& Error)
	{
		if (!GenerateKey(Out))
		{
			Error = TEXT("could not make a key (no random source?)");
			return false;
		}
		return true;
	}

	bool WriteKeyFile(const FString& Path, const FRsaPrivateKey& Key)
	{
		const FString Text = FormatKeyFile(Key, TEXT("OpenUT2k4 player key, made on this computer ") + FDateTime::Now().ToString(TEXT("%Y-%m-%d")));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		const FString Temp = Path + TEXT(".tmp");
		if (!FFileHelper::SaveStringToFile(Text, *Temp, FFileHelper::EEncodingOptions::ForceAnsi))
		{
			return false;
		}
#if PLATFORM_UNIX || PLATFORM_MAC
		chmod(TCHAR_TO_UTF8(*Temp), S_IRUSR | S_IWUSR);
#endif
		return IFileManager::Get().Move(*Path, *Temp, true, true);
	}

	/** Background: reads the key file, or makes and saves a new key. */
	void LoadOrMake()
	{
		const FString Path = UE2NetValidator::GetKeyPath();
		FRsaPrivateKey Key;
		FString Error;
		FString Text;
		bool bOk = false;
		bool bMade = false;
		if (FFileHelper::LoadFileToString(Text, *Path))
		{
			FString ParseError;
			bOk = ParseKeyFile(Text, Key, ParseError) && Key.N.Num() == 256;
			if (bOk)
			{
				// a key that cannot sign (damaged d) is replaced
				TArray<uint8> Sig;
				bOk = RsaSign(Key, StringBytes(TEXT("OpenUT2k4 key check")), Sig);
			}
			if (!bOk)
			{
				UE_LOG(LogUE2Net, Warning, TEXT("Validator: the player key file %s is damaged; making a new key"), *Path);
			}
		}
		if (!bOk)
		{
			const double T0 = FPlatformTime::Seconds();
			bOk = MakeKey(Key, Error);
			if (bOk)
			{
				bMade = true;
				if (!WriteKeyFile(Path, Key))
				{
					UE_LOG(LogUE2Net, Warning, TEXT("Validator: could not save the player key to %s (used for this session only)"), *Path);
				}
				UE_LOG(LogUE2Net, Log, TEXT("Validator: made a new player key in %.0f ms (%s)"), (FPlatformTime::Seconds() - T0) * 1000.0, *Path);
			}
		}
		FRsaPrivateKey Other;
		if (bOk && TestMode() == TEXT("wrongkey"))
		{
			FString Unused;
			MakeKey(Other, Unused);
		}
		FKeyState& S = State();
		FScopeLock L(&S.Lock);
		S.bLoading = false;
		S.bReady = true;
		S.Error = Error;
		S.OtherKey = Other;
		if (bOk)
		{
			S.Key = Key;
			S.PublicHex = NormalizeHex(UE2ValidatorCrypto::BytesToHex(Key.N.GetData(), Key.N.Num()));
			S.Fingerprint = Fingerprint(S.PublicHex);
			UE_LOG(LogUE2Net, Log, TEXT("Validator: player key %s (fingerprint %s)"), bMade ? TEXT("made") : TEXT("loaded"), *S.Fingerprint);
		}
	}

	/** The answer to a challenge; empty while the key is not ready (or with -UE2ValidatorTest=silent). */
	FString BuildAnswer(const FString& Challenge, const FString& Dialed, const FString& Refusal)
	{
		const FString Mode = TestMode();
		if (Mode == TEXT("silent"))
		{
			return FString();
		}
		if (Mode.StartsWith(TEXT("replay=")))
		{
			FString Old;
			FFileHelper::LoadFileToString(Old, *Mode.Mid(7));
			return Old.TrimStartAndEnd();
		}
		FKeyState& S = State();
		FScopeLock L(&S.Lock);
		if (!S.bReady)
		{
			return FString();
		}
		if (!Refusal.IsEmpty())
		{
			return FString::Printf(TEXT("%s;NOKEY;%s"), ResponseMagic(), *Refusal);
		}
		if (S.PublicHex.IsEmpty() || Mode == TEXT("nokey"))
		{
			return FString::Printf(TEXT("%s;NOKEY;%s"), ResponseMagic(), S.Error.IsEmpty() ? TEXT("no key") : *S.Error.Replace(TEXT(";"), TEXT(",")));
		}
		TArray<uint8> Sig;
		const FRsaPrivateKey& Signer = Mode == TEXT("wrongkey") && S.OtherKey.IsValid() ? S.OtherKey : S.Key;
		if (!RsaSign(Signer, StringBytes(SignedResponseMessage(Challenge, Dialed)), Sig))
		{
			return FString::Printf(TEXT("%s;NOKEY;signing failed"), ResponseMagic());
		}
		if (Mode == TEXT("badsig"))
		{
			Sig[Sig.Num() / 2] ^= 0x40;
		}
		const FString Answer = FString::Printf(TEXT("%s;%s;%s;%s"), ResponseMagic(), *Dialed, *S.PublicHex, *UE2ValidatorCrypto::BytesToHex(Sig.GetData(), Sig.Num()));
		if (Mode.StartsWith(TEXT("record=")))
		{
			FFileHelper::SaveStringToFile(Answer, *Mode.Mid(7));
		}
		return Answer;
	}

	bool ValidChallenge(const FString& Challenge)
	{
		if (!Challenge.StartsWith(ChallengePrefix()) || Challenge.Len() > MaxChallengeLen)
		{
			return false;
		}
		for (const TCHAR C : Challenge)
		{
			if (C < 32 || C > 126)
			{
				return false;
			}
		}
		return true;
	}

	/** Sends the answer now if the key is ready; false to try again later. */
	bool TryAnswer(FUE2NetConnection& Connection, const FPending& P)
	{
		const FString Answer = BuildAnswer(P.Challenge, P.Dialed, P.Refusal);
		if (Answer.IsEmpty())
		{
			return false;
		}
		const int32 Total = (Answer.Len() + PartLen - 1) / PartLen;
		for (int32 i = 0; i < Total; ++i)
		{
			const FString Part = Answer.Mid(i * PartLen, PartLen);
			if (P.Channel > 0)
			{
				FUE2NetValue VPart, VTotal, VData;
				VPart.Kind = VTotal.Kind = FUE2NetValue::EKind::Int;
				VPart.I = i;
				VTotal.I = Total;
				VData.Kind = FUE2NetValue::EKind::Str;
				VData.S = Part;
				if (!Connection.SendFunction(P.Channel, FName(TEXT("ServerResponse")), { VPart, VTotal, VData }))
				{
					UE_LOG(LogUE2Net, Warning, TEXT("Validator: could not send the answer (the server's OUTValidatorLink has no ServerResponse?)"));
					return true;
				}
			}
			else
			{
				Connection.SendControlText(FString::Printf(TEXT("OUTVALIDATE PART=%d TOTAL=%d DATA=%s"), i, Total, *Part));
			}
		}
		UE_LOG(LogUE2Net, Log, TEXT("Validator: %s (%d parts, %s, dialed %s)"), P.Refusal.IsEmpty() ? *(TEXT("answered the server's challenge with player key ") + UE2NetValidator::GetFingerprint())
			: TEXT("sent the refusal"), Total, P.Channel > 0 ? TEXT("OpenUT2k4Validator mutator") : TEXT("OpenUT2k4 server"), *P.Dialed);
		return true;
	}

	void Challenge(FUE2NetConnection& Connection, const FString& Text, int32 Channel)
	{
		if (!ValidChallenge(Text))
		{
			UE_LOG(LogUE2Net, Warning, TEXT("Validator: ignored a malformed challenge"));
			return;
		}
		TSet<FString>& Answered = GAnswered.FindOrAdd(&Connection);
		if (Answered.Contains(Text))
		{
			return;
		}
		Answered.Add(Text);
		UE2NetValidator::EnsurePlayerKey();
		FPending P;
		P.Challenge = Text;
		P.Channel = Channel;
		// relay defence: sign the address we really dialed, and never a challenge that names another port
		const FUE2NetConnection::FSettings& Set = Connection.GetSettings();
		int32 DialedPort = Set.Port;
		FString DialedIP = Connection.GetRemoteIP();
		FString Host = Set.Host.TrimStartAndEnd().ToLower();
		bool bHostOk = !Host.IsEmpty() && Host != DialedIP && Host.Len() <= 80;
		for (const TCHAR C : Host)
		{
			bHostOk &= FChar::IsAlnum(C) || C == TEXT('.') || C == TEXT('-');
		}
		const FString Mode = TestMode();
		if (Mode.StartsWith(TEXT("dialport=")))
		{
			DialedPort = FCString::Atoi(*Mode.Mid(9));
		}
		P.Dialed = FString::Printf(TEXT("%s:%d%s"), *DialedIP, DialedPort, bHostOk ? *(TEXT("/") + Host) : TEXT(""));
		if (Mode.StartsWith(TEXT("relaydialed=")))
		{
			// test: a signature made while connected to another server, forwarded here by a relay
			P.Dialed = Mode.Mid(12);
		}
		else if (ChallengePort(Text) != DialedPort)
		{
			P.Refusal = FString::Printf(TEXT("refused: the challenge names port %d, we dialed %d (relayed challenge?)"), ChallengePort(Text), DialedPort);
			UE_LOG(LogUE2Net, Warning, TEXT("Validator: %s"), *P.Refusal);
		}
		if (!TryAnswer(Connection, P))
		{
			GPending.FindOrAdd(&Connection).Add(P);
		}
	}

	void RunSelfTest()
	{
		TArray<FString> Lines;
		const int32 Failed = SelfTest(Lines);
		for (const FString& L : Lines)
		{
			UE_LOG(LogUE2Net, Display, TEXT("UE2ValidatorSelfTest: %s"), *L);
		}
		UE_LOG(LogUE2Net, Display, TEXT("UE2ValidatorSelfTest: %d passed, %d failed"), Lines.Num() - Failed, Failed);
	}

	FDelayedAutoRegisterHelper GStartup(EDelayedRegisterRunPhase::EndOfEngineInit, []()
	{
#if !UE_BUILD_SHIPPING
		if (FParse::Param(FCommandLine::Get(), TEXT("UE2ValidatorSelfTest")))
		{
			RunSelfTest();
		}
#endif
		// a game client makes its player key on first start, in the background (servers and commandlets do not need one)
		if (!IsRunningCommandlet() && !IsRunningDedicatedServer() && !FParse::Param(FCommandLine::Get(), TEXT("nullrhi")))
		{
			UE2NetValidator::EnsurePlayerKey();
		}
	});
}

namespace UE2NetValidator
{
	using namespace UE2NetValidatorLocal;

	FString GetLoginOption()
	{
		FString Version;
		GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("ProjectVersion"), Version, GGameIni);
		Version = Version.Replace(TEXT("?"), TEXT("")).Replace(TEXT(" "), TEXT("")).Left(20);
		EnsurePlayerKey();
		return FString::Printf(TEXT("?OpenUT2k4=%s"), Version.IsEmpty() ? TEXT("unknown") : *Version);
	}

	FString GetKeyPath()
	{
#if !UE_BUILD_SHIPPING
		FString Override;
		if (FParse::Value(FCommandLine::Get(), TEXT("UE2PlayerKey="), Override))
		{
			return Override;
		}
#endif
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Identity"), TEXT("PlayerKey.txt")));
	}

	void EnsurePlayerKey()
	{
		FKeyState& S = State();
		{
			FScopeLock L(&S.Lock);
			if (S.bReady || S.bLoading)
			{
				return;
			}
			S.bLoading = true;
		}
		Async(EAsyncExecution::Thread, []() { LoadOrMake(); });
	}

	FString GetFingerprint()
	{
		FKeyState& S = State();
		FScopeLock L(&S.Lock);
		return S.Fingerprint;
	}

	void ActorChanged(FUE2NetConnection& Connection, const FUE2NetActor& Actor)
	{
		if (!Actor.ClassPath.EndsWith(TEXT(".OUTValidatorLink")))
		{
			return;
		}
		const FUE2NetValue* V = Actor.Properties.Find(TEXT("ChallengeText"));
		if (V && V->Kind == FUE2NetValue::EKind::Str && !V->S.IsEmpty())
		{
			Challenge(Connection, V->S, Actor.Channel);
		}
	}

	void ReceivedControl(FUE2NetConnection& Connection, const TCHAR* Args)
	{
		FString Text;
		if (FParse::Value(Args, TEXT("CHALLENGE="), Text, false))
		{
			Challenge(Connection, Text, 0);
		}
	}

	void Tick(FUE2NetConnection& Connection)
	{
		TArray<FPending>* List = GPending.Find(&Connection);
		if (!List)
		{
			return;
		}
		for (int32 i = List->Num() - 1; i >= 0; --i)
		{
			if (TryAnswer(Connection, (*List)[i]))
			{
				List->RemoveAt(i);
			}
		}
		if (List->Num() == 0)
		{
			GPending.Remove(&Connection);
		}
	}

	void ConnectionClosed(FUE2NetConnection& Connection)
	{
		GPending.Remove(&Connection);
		GAnswered.Remove(&Connection);
	}
}
