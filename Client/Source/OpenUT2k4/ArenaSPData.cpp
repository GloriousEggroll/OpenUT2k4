#include "ArenaSPData.h"
#include "ArenaContent.h"
#include "ArenaLocalMessages.h"
#include "ArenaGUI2K4Pages.h"
#include "ArenaArtItems.h"
#include "OpenUT2k4.h"
#include "UScript/ArenaUScript.h"
#include "UScript/UScriptCore.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#if OPENUT2K4_USCRIPT_ENABLED
#include "UScript/UScriptRuntime.h"
#include "UScript/UScriptWorld.h"
#endif

namespace ArenaSPDataLocal
{
	using namespace ArenaSP;

	/** Stock classes the data comes from (names of the player's own packages, nothing copied). */
	const TCHAR* const LadderClass = TEXT("XGame.UT2K4LadderInfo");
	const TCHAR* const ProfileClass = TEXT("XGame.UT2K4GameProfile");
	const TCHAR* const ProfileNewClass = TEXT("GUI2K4.UT2K4SPTab_ProfileNew");
	const TCHAR* const PhantomGroupClass = TEXT("XGame.UT2K4TeamRosterPhantom");
	/** UT2K4LadderInfo.AdditionalLadders is a config array that the stock install leaves empty; the Additional tab of
	 *  the menu lists the stock custom ladder class. */
	const TCHAR* const StockCustomLadders[] = { TEXT("XGame.CLOnslaught") };

	FString Unquote(const FString& S)
	{
		FString R = S.TrimStartAndEnd();
		if (R.Len() >= 2 && R.StartsWith(TEXT("\"")) && R.EndsWith(TEXT("\"")))
		{
			R = R.Mid(1, R.Len() - 2);
		}
		return R;
	}

#if OPENUT2K4_USCRIPT_ENABLED
	FUSProperty* Prop(FUSObject* O, const TCHAR* Name)
	{
		return (O && O->Data && O->Class) ? O->FindProperty(FName(Name)) : nullptr;
	}

	const uint8* Addr(FUSObject* O, FUSProperty* P, int32 Index = 0)
	{
		return O->Data + P->Offset + Index * P->ElementSize;
	}

	FString GetStr(FUSObject* O, const TCHAR* Name, int32 Index = 0)
	{
		FUSProperty* P = Prop(O, Name);
		if (!P || Index >= P->ArrayDim)
		{
			return FString();
		}
		FString Out;
		P->ExportText(Out, Addr(O, P, Index), true);
		if (P->Type == EUSType::Object || P->Type == EUSType::Class)
		{
			const FUSObject* Ref = *reinterpret_cast<FUSObject* const*>(Addr(O, P, Index));
			return Ref ? Ref->GetPathName() : FString();
		}
		return Out;
	}

	float GetNum(FUSObject* O, const TCHAR* Name, float Default, int32 Index = 0)
	{
		FUSProperty* P = Prop(O, Name);
		if (!P || Index >= P->ArrayDim)
		{
			return Default;
		}
		const uint8* A = Addr(O, P, Index);
		switch (P->Type)
		{
		case EUSType::Int: return float(*reinterpret_cast<const int32*>(A));
		case EUSType::Float: return *reinterpret_cast<const float*>(A);
		case EUSType::Byte: return float(*A);
		case EUSType::Bool: return *reinterpret_cast<const uint32*>(A) ? 1.f : 0.f;
		default: return Default;
		}
	}

	/** Elements of a dynamic array as strings (object elements as path names). */
	TArray<FString> GetArray(FUSObject* O, const TCHAR* Name)
	{
		TArray<FString> Out;
		FUSProperty* P = Prop(O, Name);
		if (!P || P->Type != EUSType::Array || !P->Inner)
		{
			return Out;
		}
		const FUSScriptArray* A = reinterpret_cast<const FUSScriptArray*>(Addr(O, P));
		for (int32 i = 0; i < A->Num; ++i)
		{
			const uint8* E = A->Data + int64(i) * P->Inner->ElementSize;
			if (P->Inner->Type == EUSType::Object || P->Inner->Type == EUSType::Class)
			{
				const FUSObject* Ref = *reinterpret_cast<FUSObject* const*>(E);
				Out.Add(Ref ? Ref->GetPathName() : FString());
			}
			else
			{
				FString S;
				P->Inner->ExportText(S, E, true);
				Out.Add(S);
			}
		}
		return Out;
	}

	FUSObject* ClassDefaults(const FString& ClassPath)
	{
		FUSClass* C = FUSRuntime::Get().FindClass(ClassPath);
		return C ? C->GetDefaultObject() : nullptr;
	}

	/** A default-property sub object ("XGame.UT2K4LadderInfo.DM1") as an instance of its class with its saved values. */
	FUSObject* LoadSubObject(const FString& Path, const FString& ClassPath)
	{
		FUSRuntime& RT = FUSRuntime::Get();
		FString Pkg, Rel;
		if (!Path.Split(TEXT("."), &Pkg, &Rel))
		{
			return nullptr;
		}
		FUSLinker* L = RT.LoadPackage(FName(*Pkg));
		FUSClass* C = RT.FindClass(ClassPath);
		if (!L || !C || !C->EnsureLoaded())
		{
			return nullptr;
		}
		FString ClassName = ClassPath;
		ClassPath.Split(TEXT("."), nullptr, &ClassName);
		const int32 Ei = L->FindExport(Rel, ClassName);
		if (Ei == INDEX_NONE)
		{
			return nullptr;
		}
		FString Leaf = Rel;
		Rel.Split(TEXT("."), nullptr, &Leaf, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		FUSObject* O = FUSWorld::Transient().ConstructObject(C, nullptr, FName(*Leaf));
		if (O && !L->LoadInstanceProperties(Ei, O))
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("SinglePlayer: cannot read %s"), *Path);
		}
		return O;
	}

	FMatchInfo ReadMatch(const FString& Path, const TCHAR* LocSection)
	{
		FMatchInfo M;
		FUSObject* O = LoadSubObject(Path, TEXT("XGame.UT2K4MatchInfo"));
		Path.Split(TEXT("."), nullptr, &M.Id, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (!O)
		{
			return M;
		}
		M.LevelName = GetStr(O, TEXT("LevelName"));
		M.MenuName = GetStr(O, TEXT("MenuName"));
		M.EnemyTeamName = GetStr(O, TEXT("EnemyTeamName"));
		M.SpecialEvent = GetStr(O, TEXT("SpecialEvent"));
		M.URLString = GetStr(O, TEXT("URLString"));
		M.GameType = GetStr(O, TEXT("GameType"));
		M.Requirements = GetStr(O, TEXT("Requirements"));
		M.AltLevels = GetArray(O, TEXT("AltLevels"));
		M.DifficultyModifier = GetNum(O, TEXT("DifficultyModifier"), 0.f);
		M.GoalScore = GetNum(O, TEXT("GoalScore"), 0.f);
		M.TimeLimit = GetNum(O, TEXT("TimeLimit"), 0.f);
		M.NumBots = int32(GetNum(O, TEXT("NumBots"), 0.f));
		M.Priority = int32(GetNum(O, TEXT("Priority"), 0.f));
		M.PrizeMoney = int32(GetNum(O, TEXT("PrizeMoney"), 0.f));
		M.EntryFee = int32(GetNum(O, TEXT("EntryFee"), 0.f));
		// localized sub object values ([UT2K4LadderInfo] DM0.MenuName=...)
		FString V;
		if (ArenaLocalMessages::Localize(TEXT("XGame"), LocSection, M.Id + TEXT(".MenuName"), V))
		{
			M.MenuName = V;
		}
		return M;
	}
#endif

	TArray<FPlayerRecord> ReadPlayerLists()
	{
		TArray<FPlayerRecord> Out;
		for (const TMap<FString, FString>& KV : ReadPlayerListFields())
		{
			FPlayerRecord P;
			auto S = [&KV](const TCHAR* K) { const FString* V = KV.Find(K); return V ? *V : FString(); };
			auto N = [&KV](const TCHAR* K) { const FString* V = KV.Find(K); return V ? FCString::Atof(**V) : 0.f; };
			P.DefaultName = S(TEXT("defaultname"));
			if (P.DefaultName.IsEmpty())
			{
				continue;
			}
			P.Race = S(TEXT("race"));
			P.Mesh = S(TEXT("mesh"));
			P.Species = S(TEXT("species"));
			P.BodySkin = S(TEXT("bodyskin"));
			P.FaceSkin = S(TEXT("faceskin"));
			P.Portrait = S(TEXT("portrait"));
			P.TextName = S(TEXT("text"));
			P.Sex = S(TEXT("sex"));
			P.Menu = S(TEXT("menu"));
			P.FavoriteWeapon = S(TEXT("favoriteweapon"));
			P.Accuracy = N(TEXT("accuracy"));
			P.Aggressiveness = N(TEXT("aggressiveness"));
			P.StrafingAbility = N(TEXT("strafingability"));
			P.CombatStyle = N(TEXT("combatstyle"));
			P.Tactics = N(TEXT("tactics"));
			P.ReactionTime = N(TEXT("reactiontime"));
			P.Jumpiness = N(TEXT("jumpiness"));
			P.BotUse = KV.Contains(TEXT("botuse")) ? int32(N(TEXT("botuse"))) : 1;
			// characters of packs that are not in the converted content (no portrait / skins) are left out, like
			// characters of packages a UT2004 install does not have
			FString PortraitPkg;
			if (P.Portrait.Split(TEXT("."), &PortraitPkg, nullptr) && !IFileManager::Get().DirectoryExists(*(ArenaContent::GetCacheDir() / TEXT("textures") / PortraitPkg)))
			{
				continue;
			}
			if (!Out.ContainsByPredicate([&P](const FPlayerRecord& R) { return R.DefaultName.Equals(P.DefaultName, ESearchCase::IgnoreCase); }))
			{
				Out.Add(P);
			}
		}
		return Out;
	}

	FData Load()
	{
		FData D;
		D.Players = ReadPlayerLists();
		// the symbol list of the stock symbol packages as UE2 names ("TeamSymbols_UT2004.Design1")
		for (const FString& Rel : ArenaArtItems::GetTeamSymbolList())
		{
			TArray<FString> Parts;
			Rel.ParseIntoArray(Parts, TEXT("/"));
			if (Parts.Num() >= 3)
			{
				FString Group, Name;
				D.TeamSymbols.Add(Parts[2].Split(TEXT("_"), &Group, &Name) ? Parts[1] + TEXT(".") + Name : Parts[1] + TEXT(".") + Parts[2]);
			}
		}
#if OPENUT2K4_USCRIPT_ENABLED
		FString Err;
		if (!ArenaUScript::EnsureRuntime(&Err))
		{
			D.Error = Err.IsEmpty() ? FString(TEXT("the UT2004 script packages are not available")) : Err;
			return D;
		}
		FUSObject* LD = ClassDefaults(LadderClass);
		if (!LD)
		{
			D.Error = TEXT("XGame.UT2K4LadderInfo not found in the script packages");
			return D;
		}
		static const TCHAR* const Arrays[NumLadders] = { TEXT("DMMatches"), TEXT("TDMMatches"), TEXT("CTFMatches"), TEXT("BRMatches"),
			TEXT("DOMMatches"), TEXT("ASMatches"), TEXT("ChampionshipMatches") };
		static const TCHAR* const LidNames[NumLadders] = { TEXT("LID_DM"), TEXT("LID_TDM"), TEXT("LID_CTF"), TEXT("LID_BR"), TEXT("LID_DOM"),
			TEXT("LID_AS"), TEXT("LID_CHAMP") };
		D.Ladders.SetNum(NumLadders);
		for (int32 i = 0; i < NumLadders; ++i)
		{
			// the stock LID_* values are the indices we use; warn if a modified package differs
			const int32 Lid = int32(GetNum(LD, LidNames[i], float(i)));
			if (Lid != i)
			{
				UE_LOG(LogOpenUT2k4, Warning, TEXT("SinglePlayer: %s is %d, expected %d"), LidNames[i], Lid, i);
			}
			FLadder& L = D.Ladders[i];
			L.Id = i;
			L.Class = LadderClass;
			for (const FString& Path : GetArray(LD, Arrays[i]))
			{
				L.Matches.Add(ReadMatch(Path, TEXT("UT2K4LadderInfo")));
			}
		}
		for (int32 i = 0; i < 8; ++i)
		{
			D.OpenNextLadderAtRung.Add(int32(GetNum(LD, TEXT("OpenNextLadderAtRung"), 0.f, i)));
		}
		D.Ladders[LID_DM].EntryLabels.Reset();
		for (int32 i = 0; i < D.Ladders[LID_DM].Matches.Num(); ++i)
		{
			D.Ladders[LID_DM].EntryLabels.Add(ArenaGUI2K4Pages::LocArray(TEXT("GUI2K4"), TEXT("UT2K4SPTab_Qualification"), TEXT("EntryLabels"), i, FString()));
		}
		for (int32 i = 0; i < D.Ladders[LID_TDM].Matches.Num(); ++i)
		{
			D.Ladders[LID_TDM].EntryLabels.Add(ArenaGUI2K4Pages::LocArray(TEXT("GUI2K4"), TEXT("UT2K4SPTab_TeamQualification"), TEXT("EntryLabels"), i, FString()));
		}
		// additional ladders
		for (const TCHAR* CL : StockCustomLadders)
		{
			FUSObject* CD = ClassDefaults(CL);
			if (!CD)
			{
				continue;
			}
			FLadder L;
			L.Id = FirstCustomLadder + D.CustomLadders.Num();
			L.Class = CL;
			FString Short;
			FString(CL).Split(TEXT("."), nullptr, &Short);
			L.Name = GetStr(CD, TEXT("LadderName"));
			for (const FString& Path : GetArray(CD, TEXT("Matches")))
			{
				L.Matches.Add(ReadMatch(Path, *Short));
			}
			L.EntryLabels = GetArray(CD, TEXT("EntryLabels"));
			if (L.Matches.Num() > 0)
			{
				D.CustomLadders.Add(L);
			}
		}
		// challenges (UT2K4LadderInfo.ChallengeGames)
		for (const FString& C : GetArray(LD, TEXT("ChallengeGames")))
		{
			FUSObject* CD = ClassDefaults(C);
			if (!CD)
			{
				continue;
			}
			FChallengeInfo CI;
			CI.Class = C;
			CI.Name = GetStr(CD, TEXT("ChallengeName"));
			CI.Description = GetStr(CD, TEXT("ChallengeDescription"));
			CI.FeeMultiply = GetNum(CD, TEXT("ChalFeeMultiply"), 0.f);
			D.Challenges.Add(CI);
		}
		// profile defaults
		FProfileDefaults& P = D.Defaults;
		if (FUSObject* GP = ClassDefaults(ProfileClass))
		{
			P.Balance = int32(GetNum(GP, TEXT("Balance"), P.Balance));
			P.MinBalance = int32(GetNum(GP, TEXT("MinBalance"), P.MinBalance));
			P.MaxTeamSize = int32(GetNum(GP, TEXT("MaxTeamSize"), P.MaxTeamSize));
			P.SalaryCap = int32(GetNum(GP, TEXT("SalaryCap"), P.SalaryCap));
			P.TeamPercentage = GetNum(GP, TEXT("TeamPercentage"), P.TeamPercentage);
			P.MatchBonus = GetNum(GP, TEXT("MatchBonus"), P.MatchBonus);
			P.LoserFee = GetNum(GP, TEXT("LoserFee"), P.LoserFee);
			P.FeeIncrease = GetNum(GP, TEXT("FeeIncrease"), P.FeeIncrease);
			P.InjuryChance = GetNum(GP, TEXT("InjuryChance"), P.InjuryChance);
			P.InjuryTreatment = GetNum(GP, TEXT("InjuryTreatment"), P.InjuryTreatment);
			P.ChallengeChance = GetNum(GP, TEXT("ChallengeChance"), P.ChallengeChance);
			P.MapChallengeCost = GetNum(GP, TEXT("MapChallengeCost"), P.MapChallengeCost);
			for (int32 i = 0; i < 6; ++i)
			{
				P.SpreeBonus[i] = int32(GetNum(GP, TEXT("SpreeBonus"), 0.f, i));
				P.SpecialAwardNames.Add(GetStr(GP, TEXT("msgSpecialAward"), i));
			}
			for (int32 i = 0; i < 7; ++i)
			{
				P.MultiKillBonus[i] = int32(GetNum(GP, TEXT("MultiKillBonus"), 0.f, i));
			}
			P.AwardFlakMonkey = int32(GetNum(GP, TEXT("sae_flackmonkey"), P.AwardFlakMonkey));
			P.AwardComboWhore = int32(GetNum(GP, TEXT("sae_combowhore"), P.AwardComboWhore));
			P.AwardHeadHunter = int32(GetNum(GP, TEXT("sae_headhunter"), P.AwardHeadHunter));
			P.AwardRoadRampage = int32(GetNum(GP, TEXT("sae_roadrampage"), P.AwardRoadRampage));
			P.AwardHatTrick = int32(GetNum(GP, TEXT("sae_hattrick"), P.AwardHatTrick));
			P.AwardUntouchable = int32(GetNum(GP, TEXT("sae_untouchable"), P.AwardUntouchable));
			for (int32 i = 0; i < 5; ++i)
			{
				const FString Pos = GetStr(GP, TEXT("PositionName"), i);
				if (!Pos.IsEmpty())
				{
					P.PositionNames.Add(Pos);
				}
			}
		}
		if (FUSObject* PN = ClassDefaults(ProfileNewClass))
		{
			P.DefaultCharacter = GetStr(PN, TEXT("DefaultCharacter"));
			P.DefaultDifficulty = int32(GetNum(PN, TEXT("DefaultDifficulty"), P.DefaultDifficulty));
			P.InitialFreeAgents = GetArray(PN, TEXT("InitialFreeAgents"));
			P.DefaultTeamName = GetStr(PN, TEXT("DefaultTeamName"));
			for (int32 i = 0; i < 8; ++i)
			{
				P.DifficultyLevels.Add(GetStr(PN, TEXT("DifficultyLevels"), i));
			}
		}
		if (FUSObject* PG = ClassDefaults(PhantomGroupClass))
		{
			D.PhantomGroup.Class = PhantomGroupClass;
			D.PhantomGroup.Rosters = GetArray(PG, TEXT("Rosters"));
			D.PhantomGroup.Difficulty = int32(GetNum(PG, TEXT("Difficulty"), -1.f));
		}
		int32 NumMatches = 0;
		for (const FLadder& L : D.Ladders)
		{
			NumMatches += L.Matches.Num();
		}
		D.bValid = D.Ladders[LID_DM].Matches.Num() > 0;
		if (!D.bValid)
		{
			D.Error = TEXT("the qualification ladder could not be read from XGame.u");
		}
		UE_LOG(LogOpenUT2k4, Log, TEXT("SinglePlayer: read %d ladder matches (%d additional ladders), %d challenges, %d characters from the stock packages"),
			NumMatches, D.CustomLadders.Num(), D.Challenges.Num(), D.Players.Num());
#else
		D.Error = TEXT("this build has no UnrealScript runtime");
#endif
		return D;
	}
}

TArray<TMap<FString, FString>> ArenaSP::ReadPlayerListFields()
{
	TArray<TMap<FString, FString>> Out;
	// the .upl character lists: the content cache's copy (localization folder), else the install's System folder
	TArray<FString> Dirs;
	Dirs.Add(ArenaContent::GetCacheDir() / TEXT("localization"));
	const FString Install = ArenaContent::FindUT2004Install();
	if (!Install.IsEmpty())
	{
		Dirs.Add(Install / TEXT("System"));
	}
	TSet<FString> Seen;
	for (const FString& Dir : Dirs)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.upl")), true, false);
		Files.Sort();
		for (const FString& F : Files)
		{
			FString Text;
			if (Seen.Contains(F.ToLower()) || !FFileHelper::LoadFileToString(Text, *(Dir / F)))
			{
				continue;
			}
			Seen.Add(F.ToLower());
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				TMap<FString, FString> KV;
				if (ParsePlayerLine(Line, KV))
				{
					Out.Add(MoveTemp(KV));
				}
			}
		}
	}
	return Out;
}

bool ArenaSP::ParsePlayerLine(const FString& Line, TMap<FString, FString>& OutFields)
{
	OutFields.Reset();
	if (!Line.StartsWith(TEXT("Player=("), ESearchCase::IgnoreCase))
	{
		return false;
	}
	// key=value pairs separated by commas outside quotes
	const FString Body = Line.Mid(8);
	FString Cur;
	bool bQuote = false;
	auto Flush = [&]()
	{
		FString K, V;
		if (Cur.Split(TEXT("="), &K, &V))
		{
			OutFields.Add(K.TrimStartAndEnd().ToLower(), ArenaSPDataLocal::Unquote(V));
		}
		Cur.Reset();
	};
	for (TCHAR Ch : Body)
	{
		if (Ch == TEXT('"'))
		{
			bQuote = !bQuote;
		}
		if (!bQuote && (Ch == TEXT(',') || Ch == TEXT(')')))
		{
			Flush();
			continue;
		}
		Cur.AppendChar(Ch);
	}
	Flush();
	return true;
}

const ArenaSP::FData& ArenaSP::Get()
{
	static FData Data;
	static bool bLoaded = false;
	if (!bLoaded)
	{
		bLoaded = true;
		Data = ArenaSPDataLocal::Load();
		if (!Data.bValid)
		{
			UE_LOG(LogOpenUT2k4, Warning, TEXT("SinglePlayer: no tournament data: %s"), *Data.Error);
		}
	}
	return Data;
}

const ArenaSP::FLadder* ArenaSP::FindLadder(int32 LadderId)
{
	const FData& D = Get();
	if (LadderId >= 0 && LadderId < D.Ladders.Num())
	{
		return &D.Ladders[LadderId];
	}
	const int32 C = LadderId - FirstCustomLadder;
	return D.CustomLadders.IsValidIndex(C) ? &D.CustomLadders[C] : nullptr;
}

int32 ArenaSP::LengthOfLadder(int32 LadderId)
{
	const FLadder* L = FindLadder(LadderId);
	return L ? L->Matches.Num() : -1;
}

const ArenaSP::FTeamRoster* ArenaSP::FindTeam(const FString& Class)
{
	static TMap<FString, FTeamRoster> Cache;
	if (Class.IsEmpty())
	{
		return nullptr;
	}
	if (const FTeamRoster* T = Cache.Find(Class.ToLower()))
	{
		return T->bValid ? T : nullptr;
	}
	FTeamRoster& T = Cache.Add(Class.ToLower());
	T.Class = Class;
#if OPENUT2K4_USCRIPT_ENABLED
	using namespace ArenaSPDataLocal;
	if (!ArenaUScript::EnsureRuntime())
	{
		return nullptr;
	}
	FUSClass* C = FUSRuntime::Get().FindClass(Class);
	FUSObject* O = C ? C->GetDefaultObject() : nullptr;
	if (!O)
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("SinglePlayer: team roster %s not found"), *Class);
		return nullptr;
	}
	T.bValid = true;
	T.TeamName = GetStr(O, TEXT("TeamName"));
	T.TeamDescription = GetStr(O, TEXT("TeamDescription"));
	T.TeamSymbolName = GetStr(O, TEXT("TeamSymbolName"));
	T.TeamLeader = GetStr(O, TEXT("TeamLeader"));
	T.TeamLevel = int32(GetNum(O, TEXT("TeamLevel"), 0.f));
	T.RosterNames = GetArray(O, TEXT("RosterNames"));
	T.BotList = GetArray(O, TEXT("BotList"));
	for (FUSClass* K = C; K; K = K->GetSuperClass())
	{
		if (K->Name == TEXT("UT2K4PhantomRoster"))
		{
			T.bPhantom = true;
		}
		if (K->Name == TEXT("xDMRoster"))
		{
			T.bDMRoster = true;
		}
	}
	return &T;
#else
	return nullptr;
#endif
}

const ArenaSP::FRosterGroup* ArenaSP::FindGroup(const FString& Class)
{
	static TMap<FString, FRosterGroup> Cache;
	if (const FRosterGroup* G = Cache.Find(Class.ToLower()))
	{
		return G->Rosters.Num() > 0 ? G : nullptr;
	}
	FRosterGroup& G = Cache.Add(Class.ToLower());
	G.Class = Class;
#if OPENUT2K4_USCRIPT_ENABLED
	using namespace ArenaSPDataLocal;
	if (ArenaUScript::EnsureRuntime())
	{
		if (FUSObject* O = ClassDefaults(Class))
		{
			G.Rosters = GetArray(O, TEXT("Rosters"));
			G.Difficulty = int32(GetNum(O, TEXT("Difficulty"), 0.f));
		}
	}
#endif
	return G.Rosters.Num() > 0 ? &G : nullptr;
}

const ArenaSP::FPlayerRecord* ArenaSP::FindPlayer(const FString& Name)
{
	return Get().Players.FindByPredicate([&Name](const FPlayerRecord& P) { return P.DefaultName.Equals(Name, ESearchCase::IgnoreCase); });
}

FString ArenaSP::GameTypeToMode(const FString& GameType)
{
	FString Short = GameType;
	GameType.Split(TEXT("."), nullptr, &Short);
	Short.ToLowerInline();
	struct FMap { const TCHAR* Class; const TCHAR* Mode; };
	static const FMap Map[] = {
		{ TEXT("xdeathmatch"), TEXT("DM") }, { TEXT("bossdm"), TEXT("DM") }, { TEXT("xteamgame"), TEXT("TDM") },
		{ TEXT("xctfgame"), TEXT("CTF") }, { TEXT("xvehiclectfgame"), TEXT("CTF") }, { TEXT("xbombingrun"), TEXT("BR") },
		{ TEXT("xdoubledom"), TEXT("DOM") }, { TEXT("asgameinfo"), TEXT("AS") }, { TEXT("onsonslaughtgame"), TEXT("ONS") },
		{ TEXT("xmutantgame"), TEXT("MUT") }, { TEXT("xlastmanstandinggame"), TEXT("LMS") }, { TEXT("invasion"), TEXT("INV") },
	};
	for (const FMap& M : Map)
	{
		if (Short == M.Class)
		{
			return M.Mode;
		}
	}
	return FString();
}

bool ArenaSP::IsTeamGameType(const FString& GameType)
{
	const FString Mode = GameTypeToMode(GameType);
	return !Mode.IsEmpty() && Mode != TEXT("DM") && Mode != TEXT("MUT") && Mode != TEXT("LMS");
}

FString ArenaSP::GameTypeName(const FString& GameType)
{
	FString Pkg, Cls;
	if (!GameType.Split(TEXT("."), &Pkg, &Cls))
	{
		return GameType;
	}
	FString V;
	if (ArenaLocalMessages::Localize(Pkg, Cls, TEXT("GameName"), V) && !V.IsEmpty())
	{
		return V;
	}
	return Cls;
}

FString ArenaSP::LadderDescription(int32 LadderId, int32 MatchId)
{
	const FLadder* L = FindLadder(LadderId);
	if (!L || L->Matches.Num() == 0)
	{
		return FString();
	}
	return GameTypeName(L->Matches[FMath::Clamp(MatchId, 0, L->Matches.Num() - 1)].GameType);
}

// ---- xUtil ratings ----

namespace ArenaSPDataLocal
{
	/** xUtil.RatingModifier: a small per-name offset (-2..2) from the first character. */
	int32 RatingModifier(const FString& Name)
	{
		int32 Hash = Name.Len() > 0 ? int32(Name[0]) : 0;
		if (Hash == 2)
		{
			Hash = 1;
		}
		return Hash % 5 - 2;
	}
}

int32 ArenaSP::GetSalaryFor(const FPlayerRecord& P)
{
	float Salary = 500.f;
	if (P.FavoriteWeapon.IsEmpty())
	{
		Salary += 5.f;
	}
	Salary += 30.f * P.Jumpiness;
	Salary += 150.f * P.Accuracy;
	if (P.Accuracy > 0.3f)
	{
		Salary += 250.f * (P.Accuracy - 0.3f);
	}
	Salary += 70.f * P.Tactics;
	if (P.Tactics > 0.5f)
	{
		Salary += 100.f * (P.Tactics - 0.5f);
	}
	Salary += 100.f * P.StrafingAbility;
	if (P.StrafingAbility > 0.5f)
	{
		Salary += 100.f * (P.StrafingAbility - 0.5f);
	}
	Salary -= 5.f * FMath::Abs(P.Aggressiveness);
	Salary -= 5.f * FMath::Abs(P.CombatStyle);
	return int32(Salary);
}

int32 ArenaSP::AccuracyRating(const FPlayerRecord& P)
{
	const int32 Mod = ArenaSPDataLocal::RatingModifier(P.DefaultName);
	const float A2 = 2.f * P.Accuracy;
	if (A2 < -1.f)
	{
		return int32(55.f + 8.f * FMath::Max(-7.f, A2));
	}
	if (A2 == 0.f)
	{
		return 75 - Mod;
	}
	if (A2 < 1.f)
	{
		return int32(75.f + 20.f * A2 - 0.5f * Mod);
	}
	return FMath::Min(100, int32(95.f + A2));
}

int32 ArenaSP::AgilityRating(const FPlayerRecord& P)
{
	const int32 Mod = ArenaSPDataLocal::RatingModifier(P.DefaultName);
	const float Add = 3.f * P.Jumpiness;
	if (P.StrafingAbility < -1.f)
	{
		return int32(Add + 58.f + 8.f * FMath::Max(-7.f, P.StrafingAbility));
	}
	if (Add == 0.f && P.StrafingAbility == 0.f)
	{
		return int32(75.f + 0.5f * Mod);
	}
	if (P.StrafingAbility < 1.f)
	{
		return int32(Add + 75.f + 17.f * P.StrafingAbility - 0.5f * Mod);
	}
	return FMath::Min(100, int32(Add + 92.f + P.StrafingAbility));
}

int32 ArenaSP::TacticsRating(const FPlayerRecord& P)
{
	const int32 Mod = ArenaSPDataLocal::RatingModifier(P.DefaultName);
	if (P.Tactics < -1.f)
	{
		return int32(55.f + 8.f * FMath::Max(-7.f, P.Tactics));
	}
	if (P.Tactics == 0.f)
	{
		return 75 + Mod;
	}
	if (P.Tactics < 1.f)
	{
		return int32(75.f + 20.f * P.Tactics + 0.5f * Mod);
	}
	return FMath::Min(100, int32(95.f + P.Tactics));
}

int32 ArenaSP::AggressivenessRating(const FPlayerRecord& P)
{
	const int32 Mod = ArenaSPDataLocal::RatingModifier(P.DefaultName);
	return FMath::Clamp(int32(73.f + 25.f * (P.Aggressiveness + P.CombatStyle) + 0.5f * Mod), 0, 100);
}

FString ArenaSP::FavoriteWeaponText(const FPlayerRecord& P)
{
	FString Pkg, Cls, Item;
	if (P.FavoriteWeapon.Split(TEXT("."), &Pkg, &Cls) && ArenaLocalMessages::Localize(Pkg, Cls, TEXT("ItemName"), Item) && !Item.IsEmpty())
	{
		return Loc(TEXT("XGame"), TEXT("xUtil"), TEXT("FavoriteWeapon"), TEXT("Favorite Weapon:")) + TEXT(" ") + Item;
	}
	return Loc(TEXT("XGame"), TEXT("xUtil"), TEXT("NoPreference"), TEXT("No Weapon Preference"));
}

FString ArenaSP::CharacterText(const FPlayerRecord& P)
{
	FString Pkg = TEXT("XPlayers"), Item = P.TextName;
	FString V;
	if (P.TextName.Contains(TEXT("|")) || P.TextName.Contains(TEXT(" ")))
	{
		V = P.TextName; // custom characters give their text inline
	}
	else
	{
		P.TextName.Split(TEXT("."), &Pkg, &Item);
		if (!ArenaLocalMessages::Localize(Pkg, TEXT("DecoText"), Item, V))
		{
			return P.DefaultName;
		}
	}
	// UT2K4SPTab_TeamManagement.showPlayerDetails: the first line's value is the character's name
	FString First, Rest;
	if (V.Split(TEXT("|"), &First, &Rest))
	{
		FString Label, Name;
		if (First.Split(TEXT(":"), &Label, &Name) && !Name.TrimStartAndEnd().IsEmpty())
		{
			V = V.Replace(*Name.TrimStartAndEnd(), *P.DefaultName);
		}
	}
	return V;
}

FString ArenaSP::Loc(const TCHAR* Package, const TCHAR* Section, const TCHAR* Key, const FString& Fallback)
{
	FString V;
	return ArenaLocalMessages::Localize(Package, Section, Key, V) && !V.IsEmpty() ? V : Fallback;
}

FString ArenaSP::LocArray(const TCHAR* Package, const TCHAR* Section, const TCHAR* Key, int32 Index, const FString& Fallback)
{
	return ArenaGUI2K4Pages::LocArray(Package, Section, Key, Index, Fallback);
}

FString ArenaSP::ResolveObjectPath(const FString& Path)
{
	// "Pkg.Name" (group left out, team symbols) or "Pkg.WrongGroup.Name" (some custom character lists) -> the converted
	// texture / material ("Pkg.Group.Name"), found by its name in the package's folders of the content cache
	static TMap<FString, FString> Cache;
	if (const FString* Hit = Cache.Find(Path.ToLower()))
	{
		return *Hit;
	}
	FString Result = Path;
	FString Pkg, Rest, Name;
	if (Path.Split(TEXT("."), &Pkg, &Rest))
	{
		Name = Rest;
		Rest.Split(TEXT("."), nullptr, &Name, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		bool bExact = false;
		for (const TCHAR* Kind : { TEXT("textures"), TEXT("materials") })
		{
			if (IFileManager::Get().FileExists(*(ArenaContent::GetCacheDir() / Kind / Pkg / Rest + TEXT(".json"))))
			{
				bExact = true;
			}
		}
		for (const TCHAR* Kind : { TEXT("textures"), TEXT("materials") })
		{
			if (bExact)
			{
				break;
			}
			TArray<FString> Files;
			IFileManager::Get().FindFiles(Files, *(ArenaContent::GetCacheDir() / Kind / Pkg / TEXT("*.json")), true, false);
			for (const FString& F : Files)
			{
				const FString Stem = FPaths::GetBaseFilename(F);
				if (Stem.Equals(Name, ESearchCase::IgnoreCase) || Stem.EndsWith(TEXT(".") + Name, ESearchCase::IgnoreCase))
				{
					Result = Pkg + TEXT(".") + Stem;
					bExact = true;
					break;
				}
			}
		}
	}
	Cache.Add(Path.ToLower(), Result);
	return Result;
}

FString ArenaSP::SymbolTexture(const FString& Symbol)
{
	return ResolveObjectPath(Symbol);
}

FString ArenaSP::GetStockValue(const FString& Class, const TCHAR* Prop, int32 Index)
{
#if OPENUT2K4_USCRIPT_ENABLED
	if (ArenaUScript::EnsureRuntime())
	{
		return ArenaSPDataLocal::GetStr(ArenaSPDataLocal::ClassDefaults(Class), Prop, Index);
	}
#endif
	return FString();
}

TArray<FString> ArenaSP::GetStockArray(const FString& Class, const TCHAR* Prop)
{
#if OPENUT2K4_USCRIPT_ENABLED
	if (ArenaUScript::EnsureRuntime())
	{
		return ArenaSPDataLocal::GetArray(ArenaSPDataLocal::ClassDefaults(Class), Prop);
	}
#endif
	return TArray<FString>();
}

FString ArenaSP::StructField(const FString& StructText, const TCHAR* Key)
{
	FString S = StructText.TrimStartAndEnd();
	if (S.StartsWith(TEXT("(")))
	{
		S.RightChopInline(1);
	}
	if (S.EndsWith(TEXT(")")))
	{
		S.LeftChopInline(1);
	}
	bool bQuote = false;
	FString Cur;
	TArray<FString> Parts;
	int32 Depth = 0;
	for (TCHAR C : S)
	{
		if (C == TEXT('"'))
		{
			bQuote = !bQuote;
		}
		else if (!bQuote && C == TEXT('('))
		{
			++Depth;
		}
		else if (!bQuote && C == TEXT(')'))
		{
			--Depth;
		}
		if (!bQuote && Depth == 0 && C == TEXT(','))
		{
			Parts.Add(Cur);
			Cur.Reset();
			continue;
		}
		Cur.AppendChar(C);
	}
	Parts.Add(Cur);
	for (const FString& P : Parts)
	{
		FString K, V;
		if (P.Split(TEXT("="), &K, &V) && K.TrimStartAndEnd().Equals(Key, ESearchCase::IgnoreCase))
		{
			return ArenaSPDataLocal::Unquote(V);
		}
	}
	return FString();
}
