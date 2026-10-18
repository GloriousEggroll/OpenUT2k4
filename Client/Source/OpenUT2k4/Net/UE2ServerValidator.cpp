#include "UE2ServerValidator.h"

#include "UE2NetLog.h"
#include "UE2NetValidatorCrypto.h"
#include "UE2Server.h"
#include "UE2ServerConnection.h"
#include "../Server/ArenaServerConfig.h"
#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "IPAddress.h"
#include "SocketSubsystem.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UE2ServerValidatorLocal
{
	using namespace UE2ValidatorCrypto;

	const TCHAR* Section = TEXT("OpenUT2k4Validator.OUTValidator");
	const TCHAR* StoreSection = TEXT("OpenUT2k4Validator.OUTKeyStore");
	const TCHAR* SharedSection = TEXT("OpenUT2k4Validator.OUTSharedBans");
	constexpr int32 MaxListBytes = 8 << 20;

	struct FPlayer
	{
		UE2ServerValidator::FPlayerStatus S;
		FString Claim;
		FString Challenge;
		TArray<FString> Parts;
		int32 Received = 0;
		double Deadline = 0.0;
		bool bAnswered = false;
	};

	FUE2ValidatorSettings GSettings;
	FString GStateDir;
	TMap<int32, FPlayer> GPlayers;
	TArray<FString> GKeys;
	TArray<FString> GBans;
	TArray<FString> GWhitelist;
	FString GSecret;
	/** Shared bans by source (file or URL), and each source's Sources= line. */
	TMap<FString, TArray<FString>> GSharedBySource;
	TMap<FString, FString> GSourceLines;
	bool bKeysDirty = false;
	double GLastSave = 0.0;
	double GNextFetch = 0.0;
	int32 GPendingFetches = 0;
	int32 GChallengeCounter = 0;

	FString KeysFile() { return GStateDir / TEXT("ValidatorKeys.ini"); }
	FString SharedFile() { return GStateDir / TEXT("ValidatorShared.ini"); }

	FString DateNow()
	{
		return FDateTime::Now().ToString(TEXT("%Y-%m-%d %H"));
	}

	int32 HoursOf(const FString& Date)
	{
		// "YYYY-MM-DD HH"
		if (Date.Len() < 13)
		{
			return 0;
		}
		const FDateTime T(FCString::Atoi(*Date.Left(4)), FMath::Clamp(FCString::Atoi(*Date.Mid(5, 2)), 1, 12), FMath::Clamp(FCString::Atoi(*Date.Mid(8, 2)), 1, 28),
			FMath::Clamp(FCString::Atoi(*Date.Mid(11, 2)), 0, 23));
		return int32((T - FDateTime(1970, 1, 1)).GetTotalHours());
	}

	FString Clean(const FString& In, int32 Max = 80)
	{
		FString Out;
		for (const TCHAR C : In)
		{
			if (C >= 32 && C < 127 && C != TEXT('|') && C != TEXT('"'))
			{
				Out.AppendChar(C);
			}
		}
		return Out.Left(Max);
	}

	bool IsHex(const FString& S, int32 Len)
	{
		if (S.Len() != Len)
		{
			return false;
		}
		for (const TCHAR C : S)
		{
			if (!FChar::IsHexDigit(C))
			{
				return false;
			}
		}
		return true;
	}

	bool ParseIPv4(const FString& S, uint32& Out)
	{
		TArray<FString> P;
		S.ParseIntoArray(P, TEXT("."), false);
		if (P.Num() != 4)
		{
			return false;
		}
		Out = 0;
		for (const FString& X : P)
		{
			if (X.IsEmpty() || !X.IsNumeric() || FCString::Atoi(*X) > 255 || FCString::Atoi(*X) < 0)
			{
				return false;
			}
			Out = (Out << 8) | uint32(FCString::Atoi(*X));
		}
		return true;
	}

	bool InCIDR(const FString& IP, const FString& CIDR)
	{
		FString Net, Bits;
		uint32 A = 0, B = 0;
		if (!CIDR.Split(TEXT("/"), &Net, &Bits) || !ParseIPv4(IP, A) || !ParseIPv4(Net, B))
		{
			return false;
		}
		const int32 N = FCString::Atoi(*Bits);
		if (N <= 0 || N > 32)
		{
			return false;
		}
		const uint32 Mask = N == 32 ? 0xffffffffu : ~((1u << (32 - N)) - 1u);
		return (A & Mask) == (B & Mask);
	}

	/** A ban entry checked and cleaned ("fp|ip|cidr|legacy|date|reason|issuer"); empty when malformed or empty. */
	FString CleanEntry(const FString& Line, const FString* Issuer)
	{
		TArray<FString> F;
		Line.ParseIntoArray(F, TEXT("|"), false);
		F.SetNum(7);
		const FString Fp = F[0].ToLower(), Ip = F[1], Cidr = F[2], Legacy = F[3].ToLower();
		uint32 Unused = 0;
		if ((!Fp.IsEmpty() && !IsHex(Fp, 32)) || (!Ip.IsEmpty() && !ParseIPv4(Ip, Unused)) || (!Legacy.IsEmpty() && !IsHex(Legacy, 32)))
		{
			return FString();
		}
		if (!Cidr.IsEmpty())
		{
			FString Net, Bits;
			if (!Cidr.Split(TEXT("/"), &Net, &Bits) || !ParseIPv4(Net, Unused) || !Bits.IsNumeric() || FCString::Atoi(*Bits) < 8 || FCString::Atoi(*Bits) > 32)
			{
				return FString();
			}
		}
		if (Fp.IsEmpty() && Ip.IsEmpty() && Cidr.IsEmpty() && Legacy.IsEmpty())
		{
			return FString();
		}
		return FString::Join(TArray<FString>{ Fp, Ip, Cidr, Legacy, Clean(F[4], 20), Clean(F[5]), Clean(Issuer ? *Issuer : F[6], 40) }, TEXT("|"));
	}

	bool FindBanIn(const TArray<FString>& List, const TCHAR* ListName, const FString& Fp, const FString& IP, const FString& Legacy, FString* OutReason)
	{
		for (int32 i = 0; i < List.Num(); ++i)
		{
			TArray<FString> F;
			List[i].ParseIntoArray(F, TEXT("|"), false);
			F.SetNum(7);
			FString How;
			if (!Fp.IsEmpty() && !F[0].IsEmpty() && F[0].Equals(Fp, ESearchCase::IgnoreCase))
			{
				How = TEXT("key");
			}
			else if (GSettings.bMatchBanIP && !IP.IsEmpty() && !F[1].IsEmpty() && F[1] == IP)
			{
				How = TEXT("IP");
			}
			else if (GSettings.bMatchBanCIDR && !IP.IsEmpty() && !F[2].IsEmpty() && InCIDR(IP, F[2]))
			{
				How = TEXT("subnet ") + F[2];
			}
			else if (GSettings.bMatchBanLegacyId && !Legacy.IsEmpty() && !F[3].IsEmpty() && F[3].Equals(Legacy, ESearchCase::IgnoreCase))
			{
				How = TEXT("UT2004 id");
			}
			if (How.IsEmpty())
			{
				continue;
			}
			if (OutReason)
			{
				*OutReason = FString::Printf(TEXT("%s ban %d (%s)%s%s%s"), ListName, i, *How, F[5].IsEmpty() ? TEXT("") : *(TEXT(": ") + F[5]),
					F[6].IsEmpty() ? TEXT("") : *(TEXT(" by ") + F[6]), F[4].IsEmpty() ? TEXT("") : *(TEXT(", ") + F[4]));
			}
			return true;
		}
		return false;
	}

	TArray<FString> SharedBans()
	{
		TArray<FString> All;
		for (const TPair<FString, TArray<FString>>& P : GSharedBySource)
		{
			All.Append(P.Value);
		}
		return All;
	}

	// ---- state files (the mutator's format) ----

	void SaveKeys()
	{
		if (GStateDir.IsEmpty())
		{
			return;
		}
		FString Text = TEXT("; OpenUT2k4 Validator state of this server: keys (first seen), bans, whitelist. Same format as the mutator's\n");
		Text += TEXT("; System/OpenUT2k4ValidatorKeys.ini (Server/Docs/Validator.md). Edit only while the server is stopped.\n");
		Text += FString::Printf(TEXT("[%s]\nServerSecret=%s\n"), StoreSection, *GSecret);
		for (const FString& K : GKeys) Text += TEXT("Keys=") + K + TEXT("\n");
		for (const FString& B : GBans) Text += TEXT("Bans=") + B + TEXT("\n");
		for (const FString& W : GWhitelist) Text += TEXT("Whitelist=") + W + TEXT("\n");
		IFileManager::Get().MakeDirectory(*GStateDir, true);
		FFileHelper::SaveStringToFile(Text, *KeysFile());
		bKeysDirty = false;
	}

	void SaveShared()
	{
		FString Text = TEXT("; Shared ban lists imported by the OpenUT2k4 server (signatures checked). Rewritten on every import.\n");
		Text += FString::Printf(TEXT("[%s]\n"), SharedSection);
		for (const TPair<FString, FString>& S : GSourceLines) Text += TEXT("Sources=") + S.Value + TEXT("\n");
		for (const FString& B : SharedBans()) Text += TEXT("Bans=") + B + TEXT("\n");
		FFileHelper::SaveStringToFile(Text, *SharedFile());
	}

	void LoadState()
	{
		FArenaIniFile Ini;
		GKeys.Reset();
		GBans.Reset();
		GWhitelist.Reset();
		GSecret.Reset();
		if (Ini.Load(KeysFile()))
		{
			GKeys = Ini.GetAll(StoreSection, TEXT("Keys"));
			GBans = Ini.GetAll(StoreSection, TEXT("Bans"));
			GWhitelist = Ini.GetAll(StoreSection, TEXT("Whitelist"));
			Ini.Get(StoreSection, TEXT("ServerSecret"), GSecret);
		}
		if (GSecret.IsEmpty())
		{
			uint8 R[32];
			SecureRandomBytes(R, 32);
			GSecret = UE2ValidatorCrypto::BytesToHex(R, 32);
			SaveKeys();
		}
		GSharedBySource.Reset();
		GSourceLines.Reset();
		FArenaIniFile Shared;
		if (Shared.Load(SharedFile()))
		{
			// entries carry their list's issuer: give them back to the source of that issuer
			const TArray<FString> Bans = Shared.GetAll(SharedSection, TEXT("Bans"));
			for (const FString& Line : Shared.GetAll(SharedSection, TEXT("Sources")))
			{
				TArray<FString> F;
				Line.ParseIntoArray(F, TEXT("|"), false);
				if (F.Num() < 2)
				{
					continue;
				}
				GSourceLines.Add(F[0], Line);
				TArray<FString>& List = GSharedBySource.FindOrAdd(F[0]);
				for (const FString& B : Bans)
				{
					if (B.EndsWith(TEXT("|") + F[1]))
					{
						List.AddUnique(B);
					}
				}
			}
		}
	}

	FUE2ServerConnection* FindConn(int32 ConnId)
	{
		FUE2Server* S = FUE2Server::Get();
		return S ? S->FindConnection(ConnId) : nullptr;
	}

	void Kick(FPlayer& P, const FString& Text)
	{
		UE_LOG(LogUE2Net, Log, TEXT("Validator: kicking \"%s\" %s: %s"), *P.S.Name, *P.S.IP, *Text);
		if (FUE2ServerConnection* C = FindConn(P.S.ConnId))
		{
			C->Close(TEXT("validator: ") + Text, FString(), Text);
		}
	}

	void Finish(FPlayer& P, const FString& Result, const FString& Reason, const FString& Cost = FString())
	{
		P.S.Result = Result;
		P.S.Reason = Reason;
		const FString Policy = Result == TEXT("Identified") ? GSettings.IdentifiedPolicy : Result == TEXT("Failed") ? GSettings.FailedPolicy : GSettings.NoResponsePolicy;
		UE_LOG(LogUE2Net, Log, TEXT("Validator: \"%s\" %s %s%s%s%s; policy %s"), *P.S.Name, *P.S.IP, *Result,
			Result == TEXT("Identified") ? *FString::Printf(TEXT(" key %s first seen %s (%d h)"), *P.S.Fingerprint, *P.S.FirstSeen, P.S.KeyAgeHours) : TEXT(""),
			Reason.IsEmpty() ? TEXT("") : *(TEXT(": ") + Reason), *Cost, *Policy);
		if (Result == TEXT("Identified"))
		{
			FString BanReason;
			if (UE2ServerValidator::MatchesBanList(P.S.Fingerprint, P.S.IP, P.S.LegacyId, &BanReason))
			{
				P.S.bBanMatch = true;
				UE_LOG(LogUE2Net, Log, TEXT("Validator: \"%s\" matches a ban: %s; policy %s"), *P.S.Name, *BanReason, *GSettings.BanPolicy);
				if (GSettings.BanPolicy.Equals(TEXT("Kick"), ESearchCase::IgnoreCase))
				{
					Kick(P, TEXT("You are banned from this server."));
					return;
				}
			}
			if (GSettings.bUseKeyAsPlayerId)
			{
				if (FUE2ServerConnection* C = FindConn(P.S.ConnId))
				{
					C->PlayerHashId = P.S.Fingerprint;
				}
				if (ArenaHost::IsUE2IdBanned(P.S.Fingerprint))
				{
					UE_LOG(LogUE2Net, Log, TEXT("Validator: \"%s\" key %s is on the server's ban list"), *P.S.Name, *P.S.Fingerprint);
					Kick(P, TEXT("You are banned from this server."));
					return;
				}
			}
			if (GSettings.bWhitelistOnly && !P.S.bWhitelisted)
			{
				Kick(P, TEXT("This server only admits whitelisted players."));
				return;
			}
			if (GSettings.ProbationHours > 0 && !P.S.bWhitelisted && P.S.KeyAgeHours < GSettings.ProbationHours)
			{
				int32 OnProbation = 0;
				for (const TPair<int32, FPlayer>& O : GPlayers)
				{
					OnProbation += (&O.Value != &P && O.Value.S.bProbation) ? 1 : 0;
				}
				P.S.bProbation = true;
				UE_LOG(LogUE2Net, Log, TEXT("Validator: \"%s\" is on probation (key first seen %d h ago, probation %d h): %s"), *P.S.Name, P.S.KeyAgeHours,
					GSettings.ProbationHours, *GSettings.ProbationPolicy);
				if (GSettings.ProbationPolicy.Equals(TEXT("Kick"), ESearchCase::IgnoreCase))
				{
					Kick(P, FString::Printf(TEXT("This server does not admit new player keys yet (probation %d hours)."), GSettings.ProbationHours));
					return;
				}
				if (GSettings.MaxProbationPlayers > 0 && OnProbation >= GSettings.MaxProbationPlayers)
				{
					Kick(P, TEXT("Too many new players on this server right now; try again later."));
					return;
				}
			}
		}
		if (Policy.Equals(TEXT("Kick"), ESearchCase::IgnoreCase))
		{
			Kick(P, FString::Printf(TEXT("This server does not admit this player (%s)."), *Result));
		}
	}

	void Identified(FPlayer& P, const FString& Fingerprint, const FString& Cost)
	{
		P.S.Fingerprint = Fingerprint;
		bool bFound = false;
		for (FString& K : GKeys)
		{
			if (K.StartsWith(Fingerprint + TEXT("|")))
			{
				TArray<FString> F;
				K.ParseIntoArray(F, TEXT("|"), false);
				F.SetNum(4);
				P.S.FirstSeen = F[1];
				K = FString::Join(TArray<FString>{ Fingerprint, F[1], DateNow(), Clean(P.S.Name, 40) }, TEXT("|"));
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			P.S.FirstSeen = DateNow();
			GKeys.Add(FString::Join(TArray<FString>{ Fingerprint, P.S.FirstSeen, P.S.FirstSeen, Clean(P.S.Name, 40) }, TEXT("|")));
		}
		bKeysDirty = true;
		P.S.KeyAgeHours = FMath::Max(0, HoursOf(DateNow()) - HoursOf(P.S.FirstSeen));
		P.S.bWhitelisted = GWhitelist.ContainsByPredicate([&Fingerprint](const FString& W) { return W.StartsWith(Fingerprint, ESearchCase::IgnoreCase); });
		Finish(P, TEXT("Identified"), FString(), Cost);
	}

	/** This server's own addresses: bind / multihome IP, local interfaces, loopback (PublicAddress= comes on top). */
	TArray<FString> GLocalAddresses;

	void CollectLocalAddresses()
	{
		GLocalAddresses = { TEXT("127.0.0.1") };
		FString Bind;
		if (FParse::Value(FCommandLine::Get(), TEXT("UE2ServerBind="), Bind) || FParse::Value(FCommandLine::Get(), TEXT("multihome="), Bind))
		{
			GLocalAddresses.AddUnique(Bind);
		}
		if (ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			TArray<TSharedPtr<FInternetAddr>> Addrs;
			if (SS->GetLocalAdapterAddresses(Addrs))
			{
				for (const TSharedPtr<FInternetAddr>& A : Addrs)
				{
					if (A.IsValid())
					{
						GLocalAddresses.AddUnique(A->ToString(false));
					}
				}
			}
		}
	}

	/**
	 * Relay defence: the address the client signed as dialed must be this server. Empty when it is (or cannot be
	 * told apart behind NAT: bOutUnverified), otherwise why not.
	 */
	FString CheckDialed(const FString& Dialed, bool& bOutUnverified)
	{
		bOutUnverified = false;
		FString Ip, Host;
		int32 Port = 0;
		if (!ParseDialed(Dialed, Ip, Port, Host))
		{
			return TEXT("malformed dialed address");
		}
		FUE2Server* Server = FUE2Server::Get();
		if (Server && Port != Server->GetSettings().Port)
		{
			return FString::Printf(TEXT("relayed: the client dialed %s, not this server's port %d"), *Clean(Dialed, 80), Server->GetSettings().Port);
		}
		for (const FString& Pub : GSettings.PublicAddresses)
		{
			FString A = Pub.TrimStartAndEnd(), PortPart;
			A.Split(TEXT(":"), &A, &PortPart);
			if (A.Equals(Ip, ESearchCase::IgnoreCase) || (!Host.IsEmpty() && A.Equals(Host, ESearchCase::IgnoreCase)))
			{
				return FString();
			}
		}
		if (GLocalAddresses.Contains(Ip))
		{
			return FString();
		}
		if (GSettings.PublicAddresses.Num() > 0)
		{
			return FString::Printf(TEXT("relayed: the client dialed %s, which is not one of this server's addresses (PublicAddress=)"), *Clean(Dialed, 80));
		}
		bOutUnverified = true;
		return FString();
	}

	void Answer(FPlayer& P, const FString& Text)
	{
		if (!Text.StartsWith(FString(ResponseMagic()) + TEXT(";")))
		{
			Finish(P, TEXT("Failed"), Text.StartsWith(TEXT("OUTR")) ? TEXT("old answer format (without the dialed address)") : TEXT("malformed answer"));
			return;
		}
		FString Rest = Text.Mid(6), Dialed, KeyHex, SigHex;
		if (Rest.StartsWith(TEXT("NOKEY;")))
		{
			Finish(P, TEXT("None"), TEXT("the client did not sign (") + Clean(Rest.Mid(6), 100) + TEXT(")"));
			return;
		}
		if (!Rest.Split(TEXT(";"), &Dialed, &Rest) || !Rest.Split(TEXT(";"), &KeyHex, &SigHex))
		{
			Finish(P, TEXT("Failed"), TEXT("malformed answer"));
			return;
		}
		P.S.DialedAddress = Clean(Dialed, 120);
		KeyHex = NormalizeHex(KeyHex);
		TArray<uint8> Modulus, Sig;
		if (KeyHex.Len() != 512 || !HexToBytes(KeyHex, Modulus) || !HexToBytes(SigHex, Sig))
		{
			Finish(P, TEXT("Failed"), KeyHex.Len() != 512 ? TEXT("the public key is not RSA-2048") : TEXT("malformed answer"));
			return;
		}
		const double T0 = FPlatformTime::Seconds();
		const bool bValid = RsaVerify(Modulus, StringBytes(SignedResponseMessage(P.Challenge, Dialed)), Sig);
		FString Cost = FString::Printf(TEXT(", verified in %.2f ms"), (FPlatformTime::Seconds() - T0) * 1000.0);
		bool bUnverified = false;
		const FString Relay = bValid ? CheckDialed(Dialed, bUnverified) : FString();
		if (bValid && !Relay.IsEmpty())
		{
			P.S.Fingerprint = Fingerprint(KeyHex);
			Finish(P, TEXT("Failed"), Relay, Cost);
		}
		else if (bValid)
		{
			P.S.bAddressUnverified = bUnverified;
			Cost += bUnverified ? FString::Printf(TEXT(", address unverified (dialed %s; set PublicAddress= behind NAT)"), *P.S.DialedAddress)
				: FString::Printf(TEXT(", dialed %s"), *P.S.DialedAddress);
			Identified(P, Fingerprint(KeyHex), Cost);
		}
		else
		{
			P.S.Fingerprint = Fingerprint(KeyHex);
			Finish(P, TEXT("Failed"), TEXT("bad signature (wrong key, tampered or replayed answer)"), Cost);
		}
	}

	// ---- shared ban lists ----

	void ImportList(const FString& Source, const FString& Text, const FString& FetchError)
	{
		FString Issuer, Error = FetchError;
		TArray<FString> Entries;
		if (Error.IsEmpty() && UE2ServerValidator::ParseSignedBanList(Text, GSettings.TrustedBanIssuers, Issuer, Entries, Error))
		{
			GSharedBySource.Add(Source, Entries);
			GSourceLines.Add(Source, FString::Join(TArray<FString>{ Clean(Source, 200), Issuer, FString(), FString::FromInt(Entries.Num()), DateNow() }, TEXT("|")));
			UE_LOG(LogUE2Net, Log, TEXT("Validator: imported %d shared bans from %s (issuer %s)"), Entries.Num(), *Source, *Issuer);
			SaveShared();
		}
		else
		{
			UE_LOG(LogUE2Net, Warning, TEXT("Validator: ban list %s REJECTED: %s (its previous entries stay: %d)"), *Source, *Error,
				GSharedBySource.Contains(Source) ? GSharedBySource[Source].Num() : 0);
		}
	}

	void FetchLists()
	{
		for (const FString& Source : GSettings.BanListSources)
		{
			if (Source.StartsWith(TEXT("http://")) || Source.StartsWith(TEXT("https://")))
			{
				TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
				Req->SetURL(Source);
				Req->SetVerb(TEXT("GET"));
				Req->SetTimeout(30.f);
				Req->OnProcessRequestComplete().BindLambda([Source](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
				{
					--GPendingFetches;
					if (!bOk || !Resp.IsValid() || Resp->GetResponseCode() != 200)
					{
						ImportList(Source, FString(), FString::Printf(TEXT("download failed (HTTP %d)"), Resp.IsValid() ? Resp->GetResponseCode() : 0));
						return;
					}
					const TArray<uint8>& Bytes = Resp->GetContent();
					if (Bytes.Num() > MaxListBytes)
					{
						ImportList(Source, FString(), TEXT("list too large"));
						return;
					}
					const FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
					ImportList(Source, FString(Conv.Length(), Conv.Get()), FString());
				});
				++GPendingFetches;
				Req->ProcessRequest();
			}
			else
			{
				FString Text;
				const FString Path = FPaths::IsRelative(Source) ? GStateDir / Source : Source;
				if (IFileManager::Get().FileSize(*Path) > MaxListBytes || !FFileHelper::LoadFileToString(Text, *Path))
				{
					ImportList(Source, FString(), TEXT("cannot read the file"));
				}
				else
				{
					ImportList(Source, Text, FString());
				}
			}
		}
	}

	FPlayer* FindByName(const FString& Name)
	{
		for (TPair<int32, FPlayer>& P : GPlayers)
		{
			if (P.Value.S.Name.Equals(Name, ESearchCase::IgnoreCase) || FString::FromInt(P.Key) == Name || (!P.Value.S.Fingerprint.IsEmpty() && P.Value.S.Fingerprint.Equals(Name, ESearchCase::IgnoreCase)))
			{
				return &P.Value;
			}
		}
		return nullptr;
	}
}

void FUE2ValidatorSettings::ApplyIni(const FArenaIniFile& Ini)
{
	using namespace UE2ServerValidatorLocal;
	FString V;
	auto Str = [&](const TCHAR* Key, FString& Out) { if (Ini.Get(Section, Key, V)) Out = V.TrimStartAndEnd(); };
	auto Bool = [&](const TCHAR* Key, bool& Out) { if (Ini.Get(Section, Key, V)) Out = FArenaIniFile::ParseBool(V, Out); };
	auto Int = [&](const TCHAR* Key, int32& Out) { if (Ini.Get(Section, Key, V)) Out = FCString::Atoi(*V); };
	Str(TEXT("IdentifiedPolicy"), IdentifiedPolicy);
	Str(TEXT("FailedPolicy"), FailedPolicy);
	Str(TEXT("NoResponsePolicy"), NoResponsePolicy);
	if (Ini.Get(Section, TEXT("ResponseTimeout"), V)) ResponseTimeout = FMath::Clamp(FCString::Atof(*V), 5.f, 300.f);
	Bool(TEXT("bWhitelistOnly"), bWhitelistOnly);
	Int(TEXT("ProbationHours"), ProbationHours);
	Str(TEXT("ProbationPolicy"), ProbationPolicy);
	Int(TEXT("MaxProbationPlayers"), MaxProbationPlayers);
	Str(TEXT("BanPolicy"), BanPolicy);
	Bool(TEXT("bMatchBanIP"), bMatchBanIP);
	Bool(TEXT("bMatchBanCIDR"), bMatchBanCIDR);
	Bool(TEXT("bMatchBanLegacyId"), bMatchBanLegacyId);
	Bool(TEXT("bApplyBansToAllClients"), bApplyBansToAllClients);
	Bool(TEXT("bUseKeyAsPlayerId"), bUseKeyAsPlayerId);
	Int(TEXT("BanListRefreshMinutes"), BanListRefreshMinutes);
	const TArray<FString> Public = Ini.GetAll(Section, TEXT("PublicAddress"));
	if (Public.Num() > 0)
	{
		PublicAddresses = Public;
	}
	const TArray<FString> Issuers = Ini.GetAll(Section, TEXT("TrustedBanIssuer"));
	if (Issuers.Num() > 0)
	{
		TrustedBanIssuers = Issuers;
	}
	const TArray<FString> Sources = Ini.GetAll(Section, TEXT("BanListSource"));
	if (Sources.Num() > 0)
	{
		BanListSources = Sources;
	}
}

namespace UE2ServerValidator
{
	using namespace UE2ServerValidatorLocal;

	bool IsValidatorClass(const FString& ClassPath)
	{
		return ClassPath.TrimStartAndEnd().Equals(TEXT("OpenUT2k4Validator.OUTValidator"), ESearchCase::IgnoreCase);
	}

	void Configure(const FUE2ValidatorSettings& Settings, const FString& StateDir)
	{
		GSettings = Settings;
		GStateDir = StateDir;
		GPlayers.Reset();
		if (!GSettings.bEnabled)
		{
			return;
		}
		LoadState();
		CollectLocalAddresses();
		GNextFetch = 0.0;
		UE_LOG(LogUE2Net, Log, TEXT("Validator: on (native). Policies identified %s, failed %s, no response %s; probation %d h (%s, cap %d); bans %d local + %d shared (%s, ip %d, subnet %d, UT2004 id %d), key as player id %d; keys %d, whitelist %d; %d trusted ban list issuers, %d sources; state %s"),
			*GSettings.IdentifiedPolicy, *GSettings.FailedPolicy, *GSettings.NoResponsePolicy, GSettings.ProbationHours, *GSettings.ProbationPolicy, GSettings.MaxProbationPlayers,
			GBans.Num(), SharedBans().Num(), *GSettings.BanPolicy, GSettings.bMatchBanIP, GSettings.bMatchBanCIDR, GSettings.bMatchBanLegacyId, GSettings.bUseKeyAsPlayerId,
			GKeys.Num(), GWhitelist.Num(), GSettings.TrustedBanIssuers.Num(), GSettings.BanListSources.Num(), *GStateDir);
	}

	bool IsEnabled()
	{
		return GSettings.bEnabled;
	}

	void CheckLogin(FUE2ServerConnection& Conn, const FString& Url, FString& OutFailCode, FString& OutFailText)
	{
		if (!GSettings.bEnabled)
		{
			return;
		}
		FPlayer P;
		P.S.ConnId = Conn.GetId();
		P.S.Name = Conn.PlayerName;
		P.S.bOpenUT2k4 = Url.Contains(TEXT("?OpenUT2k4"), ESearchCase::IgnoreCase);
		P.Claim = Clean(FArenaServerConfig::UrlOption(Url, TEXT("OpenUT2k4")), 40);
		FString Ip = Conn.GetAddrString();
		Ip.Split(TEXT(":"), &Ip, nullptr);
		P.S.IP = Ip;
		P.S.LegacyId = Conn.PlayerHashId;
		P.S.Result = TEXT("None");
		UE_LOG(LogUE2Net, Log, TEXT("Validator: login \"%s\" %s %s"), *P.S.Name, *P.S.IP, P.S.bOpenUT2k4 ? *(TEXT("OpenUT2k4 ") + P.Claim) : TEXT("UT2004 client"));
		if (GSettings.bApplyBansToAllClients || P.S.bOpenUT2k4)
		{
			FString Reason;
			if (MatchesBanList(FString(), P.S.IP, P.S.LegacyId, &Reason))
			{
				P.S.bBanMatch = true;
				UE_LOG(LogUE2Net, Log, TEXT("Validator: \"%s\" %s matches a ban: %s; policy %s"), *P.S.Name, *P.S.IP, *Reason, *GSettings.BanPolicy);
				if (GSettings.BanPolicy.Equals(TEXT("Kick"), ESearchCase::IgnoreCase))
				{
					OutFailCode = TEXT("LOCALBAN");
					OutFailText = TEXT("You are banned from this server.");
					return;
				}
			}
		}
		GPlayers.Add(P.S.ConnId, P);
	}

	void PlayerJoined(FUE2ServerConnection& Conn)
	{
		FPlayer* P = GSettings.bEnabled ? GPlayers.Find(Conn.GetId()) : nullptr;
		if (!P || !P->S.bOpenUT2k4)
		{
			return;
		}
		FUE2Server* Server = FUE2Server::Get();
		uint8 R[16];
		SecureRandomBytes(R, 16);
		++GChallengeCounter;
		const FString Nonce = Sha256Hex(GSecret + FString::FromInt(GChallengeCounter) + UE2ValidatorCrypto::BytesToHex(R, 16) + FDateTime::UtcNow().ToString()).Left(32);
		// as the mutator: nonce, this server (its game port), the map, the player
		P->Challenge = FString::Printf(TEXT("%s%s;*:%d;%s;%d"), ChallengePrefix(), *Nonce, Server ? Server->GetSettings().Port : 0,
			Server ? *Clean(Server->GetMapName(), 60).Replace(TEXT(";"), TEXT("")) : TEXT(""), Conn.GetId());
		P->S.Result = TEXT("Pending");
		P->Deadline = FPlatformTime::Seconds() + GSettings.ResponseTimeout;
		Conn.SendControl(TEXT("OUTVALIDATE CHALLENGE=") + P->Challenge);
	}

	void ReceivedControl(FUE2ServerConnection& Conn, const TCHAR* Args)
	{
		FPlayer* P = GSettings.bEnabled ? GPlayers.Find(Conn.GetId()) : nullptr;
		if (!P || P->S.Result != TEXT("Pending") || P->bAnswered)
		{
			return;
		}
		int32 Part = -1, Total = 0;
		FParse::Value(Args, TEXT("PART="), Part);
		FParse::Value(Args, TEXT("TOTAL="), Total);
		const TCHAR* DataAt = FCString::Strstr(Args, TEXT("DATA="));
		const FString Data = DataAt ? FString(DataAt + 5).TrimStartAndEnd() : FString();
		if (Total < 1 || Total > 16 || Part < 0 || Part >= Total || Data.Len() > 400 || (P->Parts.Num() > 0 && P->Parts.Num() != Total))
		{
			P->bAnswered = true;
			Finish(*P, TEXT("Failed"), TEXT("malformed answer"));
			return;
		}
		if (P->Parts.Num() == 0)
		{
			P->Parts.SetNum(Total);
		}
		if (P->Parts[Part].IsEmpty())
		{
			++P->Received;
		}
		P->Parts[Part] = Data;
		if (P->Received == Total)
		{
			P->bAnswered = true;
			Answer(*P, FString::Join(P->Parts, TEXT("")));
		}
	}

	void PlayerLeft(FUE2ServerConnection& Conn)
	{
		GPlayers.Remove(Conn.GetId());
	}

	void PlayerBanned(FUE2ServerConnection& Conn, const FString& Reason)
	{
		FPlayer* P = GSettings.bEnabled ? GPlayers.Find(Conn.GetId()) : nullptr;
		if (!P)
		{
			return;
		}
		GBans.Add(FString::Join(TArray<FString>{ P->S.Fingerprint, P->S.IP, FString(), P->S.LegacyId, DateNow(), Clean(Reason), TEXT("admin") }, TEXT("|")));
		SaveKeys();
		UE_LOG(LogUE2Net, Log, TEXT("Validator: banned \"%s\": key %s, IP %s, UT2004 id %s"), *P->S.Name, P->S.Fingerprint.IsEmpty() ? TEXT("(none)") : *P->S.Fingerprint,
			*P->S.IP, P->S.LegacyId.IsEmpty() ? TEXT("(none)") : TEXT("(set)"));
	}

	void Tick(double)
	{
		if (!GSettings.bEnabled)
		{
			return;
		}
		const double Now = FPlatformTime::Seconds();
		for (TPair<int32, FPlayer>& P : GPlayers)
		{
			if (P.Value.S.Result == TEXT("Pending") && !P.Value.bAnswered && Now > P.Value.Deadline)
			{
				P.Value.bAnswered = true;
				Finish(P.Value, TEXT("None"), FString::Printf(TEXT("no answer in %.0f s"), GSettings.ResponseTimeout));
			}
		}
		if (bKeysDirty && Now - GLastSave > 20.0)
		{
			GLastSave = Now;
			SaveKeys();
		}
		if (GSettings.BanListSources.Num() > 0 && Now >= GNextFetch && GPendingFetches == 0)
		{
			GNextFetch = Now + 60.0 * FMath::Max(1, GSettings.BanListRefreshMinutes);
			FetchLists();
		}
	}

	FString AdminCommand(const FString& InArgs)
	{
		if (!GSettings.bEnabled)
		{
			return TEXT("the validator is off (install it: ServerActors=OpenUT2k4Validator.OUTValidator, see Server/Docs/Validator.md)");
		}
		FString Cmd = InArgs.TrimStartAndEnd(), Arg;
		InArgs.TrimStartAndEnd().Split(TEXT(" "), &Cmd, &Arg);
		Cmd.ToLowerInline();
		Arg.TrimStartAndEndInline();
		if (Cmd.IsEmpty() || Cmd == TEXT("status"))
		{
			FString Out = FString::Printf(TEXT("validator: %d players, keys %d, bans %d local + %d shared, whitelist %d"), GPlayers.Num(), GKeys.Num(), GBans.Num(), SharedBans().Num(), GWhitelist.Num());
			for (const TPair<int32, FPlayer>& P : GPlayers)
			{
				const FPlayerStatus& S = P.Value.S;
				Out += FString::Printf(TEXT("\n  %d \"%s\" %s %s%s%s%s%s%s"), S.ConnId, *S.Name, *S.IP, *S.Result,
					S.Fingerprint.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" key %s first seen %s (%d h)"), *S.Fingerprint, *S.FirstSeen, S.KeyAgeHours),
					S.bProbation ? TEXT(" PROBATION") : (S.bAddressUnverified ? TEXT(" (address unverified)") : TEXT("")), S.bBanMatch ? TEXT(" BAN MATCH") : TEXT(""), S.bWhitelisted ? TEXT(" whitelisted") : TEXT(""),
					S.Reason.IsEmpty() ? TEXT("") : *(TEXT(" (") + S.Reason + TEXT(")")));
			}
			return Out + TEXT("\ncommands: validator status | ban <id|name|fingerprint|ip|cidr> [reason] | unban <fingerprint|ip|cidr> | whitelist <id|name|fingerprint> | unwhitelist <fingerprint> | export | import");
		}
		FString Target = Arg, Reason;
		Arg.Split(TEXT(" "), &Target, &Reason);
		if (Cmd == TEXT("ban"))
		{
			if (FPlayer* P = FindByName(Target))
			{
				GBans.Add(FString::Join(TArray<FString>{ P->S.Fingerprint, P->S.IP, FString(), P->S.LegacyId, DateNow(), Clean(Reason), TEXT("admin") }, TEXT("|")));
				SaveKeys();
				Kick(*P, TEXT("You are banned from this server."));
				return FString::Printf(TEXT("banned \"%s\" (key %s, IP %s)"), *P->S.Name, P->S.Fingerprint.IsEmpty() ? TEXT("none") : *P->S.Fingerprint, *P->S.IP);
			}
			uint32 Unused = 0;
			FString Entry;
			if (IsHex(Target, 32))
			{
				Entry = Target.ToLower() + TEXT("||||");
			}
			else if (Target.Contains(TEXT("/")))
			{
				Entry = TEXT("||") + Target + TEXT("||");
			}
			else if (ParseIPv4(Target, Unused))
			{
				Entry = TEXT("|") + Target + TEXT("|||");
			}
			Entry = Entry.IsEmpty() ? FString() : CleanEntry(Entry + DateNow() + TEXT("|") + Clean(Reason) + TEXT("|admin"), nullptr);
			if (Entry.IsEmpty())
			{
				return TEXT("no such player, key, IP or subnet: ") + Target;
			}
			GBans.Add(Entry);
			SaveKeys();
			return TEXT("banned ") + Target;
		}
		if (Cmd == TEXT("unban"))
		{
			const int32 N = GBans.RemoveAll([&Arg](const FString& B) { return !Arg.IsEmpty() && (B.StartsWith(Arg + TEXT("|"), ESearchCase::IgnoreCase) || B.Contains(TEXT("|") + Arg + TEXT("|"))); });
			SaveKeys();
			return FString::Printf(TEXT("removed %d bans"), N);
		}
		if (Cmd == TEXT("whitelist"))
		{
			FPlayer* P = FindByName(Arg);
			const FString Fp = P ? P->S.Fingerprint : IsHex(Arg, 32) ? Arg.ToLower() : FString();
			if (Fp.IsEmpty())
			{
				return TEXT("no identified player or key: ") + Arg;
			}
			GWhitelist.AddUnique(Fp + TEXT("|") + (P ? Clean(P->S.Name, 40) : FString()));
			if (P)
			{
				P->S.bWhitelisted = true;
			}
			SaveKeys();
			return TEXT("whitelisted ") + Fp;
		}
		if (Cmd == TEXT("unwhitelist"))
		{
			const int32 N = GWhitelist.RemoveAll([&Arg](const FString& W) { return !Arg.IsEmpty() && W.StartsWith(Arg, ESearchCase::IgnoreCase); });
			SaveKeys();
			return FString::Printf(TEXT("removed %d whitelist entries"), N);
		}
		if (Cmd == TEXT("export"))
		{
			FString Text = FString::Printf(TEXT("OUT2K4BANLIST1\nissuer=%s\ndate=%s\n"), *Clean(Arg.IsEmpty() ? FString(TEXT("OpenUT2k4 server")) : Arg, 40),
				*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M")));
			int32 N = 0;
			for (const FString& B : GBans)
			{
				const FString E = CleanEntry(B, nullptr);
				if (!E.IsEmpty())
				{
					Text += TEXT("ban=") + E + TEXT("\n");
					++N;
				}
			}
			const FString Path = GStateDir / TEXT("ValidatorBanList.txt");
			FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			return FString::Printf(TEXT("%d bans exported to %s (unsigned: sign it with Client/Tools/openut2k4_signing.py banlist-sign before you publish it)"), N, *Path);
		}
		if (Cmd == TEXT("import"))
		{
			if (GSettings.BanListSources.Num() == 0)
			{
				return TEXT("no BanListSource= lines in [OpenUT2k4Validator.OUTValidator]");
			}
			GNextFetch = 0.0;
			return FString::Printf(TEXT("fetching %d ban lists (see the log)"), GSettings.BanListSources.Num());
		}
		return TEXT("unknown validator command (validator status)");
	}

	bool GetStatus(int32 ConnId, FPlayerStatus& Out)
	{
		if (const FPlayer* P = GPlayers.Find(ConnId))
		{
			Out = P->S;
			return true;
		}
		return false;
	}

	TArray<FPlayerStatus> GetAllStatus()
	{
		TArray<FPlayerStatus> Out;
		for (const TPair<int32, FPlayer>& P : GPlayers)
		{
			Out.Add(P.Value.S);
		}
		return Out;
	}

	bool MatchesBanList(const FString& Fp, const FString& IP, const FString& LegacyId, FString* OutReason)
	{
		return FindBanIn(GBans, TEXT("local"), Fp, IP, LegacyId, OutReason) || FindBanIn(SharedBans(), TEXT("shared"), Fp, IP, LegacyId, OutReason);
	}

	bool ParseSignedBanList(const FString& InText, const TArray<FString>& TrustedIssuers, FString& OutIssuer, TArray<FString>& OutEntries, FString& OutError)
	{
		OutEntries.Reset();
		const FString Text = InText.Replace(TEXT("\r\n"), TEXT("\n"));
		if (!Text.StartsWith(TEXT("OUT2K4BANLIST1\n")))
		{
			OutError = TEXT("not an OpenUT2k4 ban list");
			return false;
		}
		const int32 At = Text.Find(TEXT("\nsig="), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (At < 0)
		{
			OutError = TEXT("unsigned list");
			return false;
		}
		const FString Body = Text.Left(At + 1);
		const FString SigHex = Text.Mid(At + 5).TrimStartAndEnd();
		TArray<FString> Lines;
		Body.ParseIntoArray(Lines, TEXT("\n"), true);
		FString KeyHex, Issuer;
		TArray<FString> Raw;
		for (const FString& L : Lines)
		{
			if (L.StartsWith(TEXT("key="))) KeyHex = L.Mid(4).ToLower();
			else if (L.StartsWith(TEXT("issuer="))) Issuer = Clean(L.Mid(7), 40);
			else if (L.StartsWith(TEXT("ban="))) Raw.Add(L.Mid(4));
		}
		const FString* Trusted = TrustedIssuers.FindByPredicate([&KeyHex](const FString& T) { return !KeyHex.IsEmpty() && T.TrimStartAndEnd().StartsWith(KeyHex, ESearchCase::IgnoreCase); });
		if (!IsHex(KeyHex, 64))
		{
			OutError = TEXT("no issuer key in the list");
			return false;
		}
		if (!Trusted)
		{
			OutError = FString::Printf(TEXT("issuer key %s is not trusted (TrustedBanIssuer=)"), *KeyHex.Left(16));
			return false;
		}
		TArray<uint8> Key, Sig;
		const FTCHARToUTF8 Utf8(*Body);
		if (!IsHex(SigHex, 128) || !HexToBytes(KeyHex, Key) || !HexToBytes(SigHex, Sig)
			|| !Ed25519Verify(Key.GetData(), reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), Sig.GetData()))
		{
			OutError = TEXT("BAD SIGNATURE");
			return false;
		}
		if (Issuer.IsEmpty())
		{
			FString K, Name;
			Issuer = Trusted->TrimStartAndEnd().Split(TEXT(" "), &K, &Name) ? Clean(Name, 40) : KeyHex.Left(16);
		}
		for (const FString& R : Raw)
		{
			const FString E = CleanEntry(R, &Issuer);
			if (!E.IsEmpty())
			{
				OutEntries.Add(E);
			}
		}
		OutIssuer = Issuer;
		return true;
	}
}
