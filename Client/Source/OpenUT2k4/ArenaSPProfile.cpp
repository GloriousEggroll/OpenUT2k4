#include "ArenaSPProfile.h"
#include "ArenaContent.h"
#include "OpenUT2k4.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

namespace ArenaSPProfileLocal
{
	using namespace ArenaSP;

	constexpr int32 MaxInt = 2147483647;
	/** GameProfile.LINEUP_SIZE */
	constexpr int32 LineupSize = 4;
	constexpr int32 SaveVersion = 1;

	float FRand() { return FMath::FRand(); }
	int32 Rand(int32 N) { return N > 0 ? FMath::RandRange(0, N - 1) : 0; }

	FString SafeFileName(const FString& Name)
	{
		FString Out;
		for (TCHAR C : Name)
		{
			Out.AppendChar(FChar::IsAlnum(C) || C == TEXT('_') || C == TEXT('-') ? C : TEXT('_'));
		}
		return Out;
	}

	TSharedPtr<FJsonValue> Num(double V) { return MakeShared<FJsonValueNumber>(V); }
	TSharedPtr<FJsonValue> Str(const FString& V) { return MakeShared<FJsonValueString>(V); }

	template <typename T, int32 N>
	TArray<TSharedPtr<FJsonValue>> IntArray(const T (&A)[N])
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (int32 i = 0; i < N; ++i)
		{
			Out.Add(Num(double(A[i])));
		}
		return Out;
	}

	TArray<TSharedPtr<FJsonValue>> StrArray(const TArray<FString>& A)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& S : A)
		{
			Out.Add(Str(S));
		}
		return Out;
	}

	template <typename T, int32 N>
	void ReadIntArray(const TSharedPtr<FJsonObject>& J, const TCHAR* Key, T (&A)[N])
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (J->TryGetArrayField(Key, Arr))
		{
			for (int32 i = 0; i < N && i < Arr->Num(); ++i)
			{
				A[i] = T((*Arr)[i]->AsNumber());
			}
		}
	}

	TArray<FString> ReadStrArray(const TSharedPtr<FJsonObject>& J, const TCHAR* Key)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (J->TryGetArrayField(Key, Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				Out.Add(V->AsString());
			}
		}
		return Out;
	}

	TSharedPtr<FJsonObject> MatchToJson(const FMatchInfo& M)
	{
		TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("Id"), M.Id);
		J->SetStringField(TEXT("LevelName"), M.LevelName);
		J->SetStringField(TEXT("MenuName"), M.MenuName);
		J->SetStringField(TEXT("EnemyTeamName"), M.EnemyTeamName);
		J->SetStringField(TEXT("SpecialEvent"), M.SpecialEvent);
		J->SetStringField(TEXT("URLString"), M.URLString);
		J->SetStringField(TEXT("GameType"), M.GameType);
		J->SetStringField(TEXT("Requirements"), M.Requirements);
		J->SetArrayField(TEXT("AltLevels"), StrArray(M.AltLevels));
		J->SetNumberField(TEXT("DifficultyModifier"), M.DifficultyModifier);
		J->SetNumberField(TEXT("GoalScore"), M.GoalScore);
		J->SetNumberField(TEXT("TimeLimit"), M.TimeLimit);
		J->SetNumberField(TEXT("NumBots"), M.NumBots);
		J->SetNumberField(TEXT("PrizeMoney"), M.PrizeMoney);
		J->SetNumberField(TEXT("EntryFee"), M.EntryFee);
		return J;
	}

	FMatchInfo MatchFromJson(const TSharedPtr<FJsonObject>& J)
	{
		FMatchInfo M;
		if (!J.IsValid())
		{
			return M;
		}
		M.Id = J->GetStringField(TEXT("Id"));
		M.LevelName = J->GetStringField(TEXT("LevelName"));
		M.MenuName = J->GetStringField(TEXT("MenuName"));
		M.EnemyTeamName = J->GetStringField(TEXT("EnemyTeamName"));
		M.SpecialEvent = J->GetStringField(TEXT("SpecialEvent"));
		M.URLString = J->GetStringField(TEXT("URLString"));
		M.GameType = J->GetStringField(TEXT("GameType"));
		M.Requirements = J->GetStringField(TEXT("Requirements"));
		M.AltLevels = ReadStrArray(J, TEXT("AltLevels"));
		M.DifficultyModifier = float(J->GetNumberField(TEXT("DifficultyModifier")));
		M.GoalScore = float(J->GetNumberField(TEXT("GoalScore")));
		M.TimeLimit = float(J->GetNumberField(TEXT("TimeLimit")));
		M.NumBots = int32(J->GetNumberField(TEXT("NumBots")));
		M.PrizeMoney = int32(J->GetNumberField(TEXT("PrizeMoney")));
		M.EntryFee = int32(J->GetNumberField(TEXT("EntryFee")));
		return M;
	}
}

using namespace ArenaSPProfileLocal;

// =================================================================================================
// Persistence
// =================================================================================================

FString ArenaSP::FProfile::GetDir()
{
	FString Override;
	if (FParse::Value(FCommandLine::Get(), TEXT("SPProfileDir="), Override) && !Override.IsEmpty())
	{
		return FPaths::ConvertRelativePathToFull(Override);
	}
	return ArenaContent::GetUserDataDir() / TEXT("SinglePlayer");
}

TArray<FString> ArenaSP::FProfile::List()
{
	TArray<FString> Files, Out;
	IFileManager::Get().FindFiles(Files, *(GetDir() / TEXT("*.json")), true, false);
	for (const FString& F : Files)
	{
		Out.Add(FPaths::GetBaseFilename(F));
	}
	Out.Sort();
	return Out;
}

bool ArenaSP::FProfile::Exists(const FString& Name)
{
	return IFileManager::Get().FileExists(*(GetDir() / SafeFileName(Name) + TEXT(".json")));
}

bool ArenaSP::FProfile::Delete(const FString& Name)
{
	return IFileManager::Get().Delete(*(GetDir() / SafeFileName(Name) + TEXT(".json")), false, true, true);
}

bool ArenaSP::FProfile::Save() const
{
	TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
	ToJson(J);
	FString Text;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Text);
	if (!FJsonSerializer::Serialize(J.ToSharedRef(), W))
	{
		return false;
	}
	IFileManager::Get().MakeDirectory(*GetDir(), true);
	const FString Path = GetDir() / SafeFileName(ProfileName) + TEXT(".json");
	const bool bOk = FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogOpenUT2k4, Log, TEXT("SinglePlayer: profile %s %s (%s)"), *ProfileName, bOk ? TEXT("saved") : TEXT("NOT saved"), *Path);
	return bOk;
}

TSharedPtr<ArenaSP::FProfile> ArenaSP::FProfile::Load(const FString& Name)
{
	FString Text;
	const FString Path = GetDir() / SafeFileName(Name) + TEXT(".json");
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return nullptr;
	}
	TSharedPtr<FJsonObject> J;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(R, J) || !J.IsValid())
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("SinglePlayer: profile %s is not valid json"), *Path);
		return nullptr;
	}
	TSharedPtr<FProfile> P = MakeShared<FProfile>();
	P->FromJson(J);
	P->ProfileName = Name;
	P->PlayerTeam.SetNum(P->MaxTeamSize);
	UE_LOG(LogOpenUT2k4, Log, TEXT("SinglePlayer: profile %s loaded (balance %d, matches %d, ladder %s)"), *Name, P->Balance, P->Matches,
		*FString::JoinBy(P->LadderProgress, TEXT(","), [](int32 V) { return FString::FromInt(V); }));
	return P;
}

void ArenaSP::FProfile::ToJson(TSharedPtr<FJsonObject> J) const
{
	J->SetNumberField(TEXT("SaveVersion"), SaveVersion);
	J->SetStringField(TEXT("PlayerName"), PlayerName);
	J->SetStringField(TEXT("PlayerCharacter"), PlayerCharacter);
	J->SetStringField(TEXT("TeamName"), TeamName);
	J->SetStringField(TEXT("TeamSymbolName"), TeamSymbolName);
	J->SetNumberField(TEXT("BaseDifficulty"), BaseDifficulty);
	J->SetNumberField(TEXT("Revision"), Revision);
	{
		TArray<TSharedPtr<FJsonValue>> A;
		for (int32 V : LadderProgress)
		{
			A.Add(Num(V));
		}
		J->SetArrayField(TEXT("LadderProgress"), A);
	}
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& KV : CustomLadderProgress)
		{
			O->SetNumberField(KV.Key, KV.Value);
		}
		J->SetObjectField(TEXT("CustomLadders"), O);
	}
	J->SetNumberField(TEXT("CurrentLadder"), CurrentLadder);
	J->SetNumberField(TEXT("CurrentMenuRung"), CurrentMenuRung);
	J->SetStringField(TEXT("EnemyTeam"), EnemyTeam);
	J->SetNumberField(TEXT("Difficulty"), Difficulty);
	J->SetNumberField(TEXT("AltPath"), AltPath);
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		for (const TPair<int32, int32>& KV : LevelRoute)
		{
			O->SetNumberField(FString::FromInt(KV.Key), KV.Value);
		}
		J->SetObjectField(TEXT("LevelRoute"), O);
	}
	J->SetStringField(TEXT("FinalEnemyTeam"), FinalEnemyTeam);
	J->SetBoolField(TEXT("bCompleted"), bCompleted);
	J->SetBoolField(TEXT("bCheater"), bCheater);
	J->SetArrayField(TEXT("PlayerTeam"), StrArray(PlayerTeam));
	{
		TArray<TSharedPtr<FJsonValue>> A;
		for (int32 V : PlayerLineup)
		{
			A.Add(Num(V));
		}
		J->SetArrayField(TEXT("PlayerLineup"), A);
	}
	J->SetArrayField(TEXT("PlayerPositions"), StrArray(PlayerPositions));
	{
		TArray<TSharedPtr<FJsonValue>> A;
		for (const FBotStats& B : BotStats)
		{
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("Name"), B.Name);
			O->SetNumberField(TEXT("Price"), B.Price);
			O->SetNumberField(TEXT("Health"), B.Health);
			O->SetBoolField(TEXT("FreeAgent"), B.bFreeAgent);
			O->SetNumberField(TEXT("TeamId"), B.TeamId);
			A.Add(MakeShared<FJsonValueObject>(O));
		}
		J->SetArrayField(TEXT("BotStats"), A);
	}
	{
		TArray<TSharedPtr<FJsonValue>> A;
		for (const FTeamStats& T : TeamStats)
		{
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("Name"), T.Name);
			O->SetNumberField(TEXT("Level"), T.Level);
			O->SetNumberField(TEXT("Matches"), T.Matches);
			O->SetNumberField(TEXT("Won"), T.Won);
			O->SetNumberField(TEXT("Rating"), T.Rating);
			A.Add(MakeShared<FJsonValueObject>(O));
		}
		J->SetArrayField(TEXT("TeamStats"), A);
	}
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		for (const TPair<FString, TArray<FString>>& KV : AltTeamRoster)
		{
			O->SetArrayField(KV.Key, StrArray(KV.Value));
		}
		J->SetObjectField(TEXT("AltTeamRoster"), O);
	}
	J->SetNumberField(TEXT("Matches"), Matches);
	J->SetNumberField(TEXT("Wins"), Wins);
	J->SetNumberField(TEXT("Kills"), Kills);
	J->SetNumberField(TEXT("Deaths"), Deaths);
	J->SetNumberField(TEXT("Goals"), Goals);
	J->SetNumberField(TEXT("TotalTime"), TotalTime);
	J->SetArrayField(TEXT("Spree"), IntArray(Spree));
	J->SetArrayField(TEXT("MultiKills"), IntArray(MultiKills));
	J->SetArrayField(TEXT("SpecialAwards"), IntArray(SpecialAwards));
	{
		TArray<TSharedPtr<FJsonValue>> A;
		for (const FFightHistory& H : FightHistory)
		{
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("Date"), H.Date);
			O->SetStringField(TEXT("MatchData"), H.MatchData);
			O->SetStringField(TEXT("MatchExtra"), H.MatchExtra);
			O->SetStringField(TEXT("Level"), H.Level);
			O->SetStringField(TEXT("GameType"), H.GameType);
			O->SetStringField(TEXT("EnemyTeam"), H.EnemyTeam);
			O->SetNumberField(TEXT("PrizeMoney"), H.PrizeMoney);
			O->SetNumberField(TEXT("BalanceChange"), H.BalanceChange);
			O->SetNumberField(TEXT("BonusMoney"), H.BonusMoney);
			O->SetNumberField(TEXT("GameTime"), H.GameTime);
			O->SetBoolField(TEXT("Won"), H.bWon);
			O->SetBoolField(TEXT("TeamGame"), H.bTeamGame);
			O->SetArrayField(TEXT("TeamScore"), IntArray(H.TeamScore));
			O->SetArrayField(TEXT("TeamLayout"), StrArray({ H.TeamLayout[0], H.TeamLayout[1] }));
			O->SetNumberField(TEXT("MyScore"), H.MyScore);
			O->SetNumberField(TEXT("MyKills"), H.MyKills);
			O->SetNumberField(TEXT("MyDeaths"), H.MyDeaths);
			O->SetStringField(TEXT("MyAwards"), H.MyAwards);
			A.Add(MakeShared<FJsonValueObject>(O));
		}
		J->SetArrayField(TEXT("FightHistory"), A);
	}
	J->SetNumberField(TEXT("Balance"), Balance);
	J->SetNumberField(TEXT("MinBalance"), MinBalance);
	J->SetNumberField(TEXT("MaxTeamSize"), MaxTeamSize);
	J->SetNumberField(TEXT("TeamPercentage"), TeamPercentage);
	J->SetNumberField(TEXT("MatchBonus"), MatchBonus);
	J->SetNumberField(TEXT("LoserFee"), LoserFee);
	J->SetNumberField(TEXT("FeeIncrease"), FeeIncrease);
	J->SetNumberField(TEXT("InjuryChance"), InjuryChance);
	J->SetNumberField(TEXT("InjuryTreatment"), InjuryTreatment);
	J->SetNumberField(TEXT("ChallengeChance"), ChallengeChance);
	J->SetNumberField(TEXT("MapChallengeCost"), MapChallengeCost);
	J->SetNumberField(TEXT("ForfeitFee"), ForfeitFee);
	J->SetNumberField(TEXT("LastInjured"), LastInjured);
	J->SetStringField(TEXT("SpecialEvent"), SpecialEvent);
	J->SetBoolField(TEXT("bInLadderGame"), bInLadderGame);
	J->SetBoolField(TEXT("bIsChallenge"), bIsChallenge);
	J->SetBoolField(TEXT("bGotChallenged"), bGotChallenged);
	J->SetStringField(TEXT("ChallengeClass"), ChallengeClass);
	J->SetStringField(TEXT("ChallengeVariable"), ChallengeVariable);
	J->SetObjectField(TEXT("ChallengeInfo"), MatchToJson(ChallengeInfo));
	J->SetBoolField(TEXT("bShowDetails"), bShowDetails);
	// last match details
	TSharedPtr<FJsonObject> L = MakeShared<FJsonObject>();
	L->SetBoolField(TEXT("Fresh"), bLmdFresh);
	L->SetNumberField(TEXT("BalanceChange"), LmdBalanceChange);
	L->SetBoolField(TEXT("Won"), bLmdWon);
	L->SetStringField(TEXT("EnemyTeam"), LmdEnemyTeam);
	L->SetStringField(TEXT("GameType"), LmdGameType);
	L->SetBoolField(TEXT("Challenge"), bLmdChallenge);
	L->SetStringField(TEXT("Map"), LmdMap);
	L->SetNumberField(TEXT("PrizeMoney"), LmdPrizeMoney);
	L->SetNumberField(TEXT("TotalBonusMoney"), LmdTotalBonusMoney);
	L->SetArrayField(TEXT("Spree"), IntArray(LmdSpree));
	L->SetArrayField(TEXT("MultiKills"), IntArray(LmdMultiKills));
	L->SetNumberField(TEXT("Injury"), LmdInjury);
	L->SetNumberField(TEXT("InjuryHealth"), LmdInjuryHealth);
	L->SetNumberField(TEXT("InjuryTreatment"), LmdInjuryTreatment);
	L->SetNumberField(TEXT("GameTime"), LmdGameTime);
	L->SetBoolField(TEXT("TeamGame"), bLmdTeamGame);
	L->SetNumberField(TEXT("MyTeam"), LmdMyTeam);
	{
		TArray<TSharedPtr<FJsonValue>> A;
		for (const FPlayerMatchDetails& D : PlayerMatchDetails)
		{
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetNumberField(TEXT("Id"), D.Id);
			O->SetStringField(TEXT("Name"), D.Name);
			O->SetNumberField(TEXT("Kills"), D.Kills);
			O->SetNumberField(TEXT("Score"), D.Score);
			O->SetNumberField(TEXT("Deaths"), D.Deaths);
			O->SetNumberField(TEXT("Team"), D.Team);
			O->SetArrayField(TEXT("Awards"), StrArray(D.SpecialAwards));
			A.Add(MakeShared<FJsonValueObject>(O));
		}
		L->SetArrayField(TEXT("Players"), A);
	}
	{
		TArray<TSharedPtr<FJsonValue>> A;
		for (const FPayCheck& P : PayCheck)
		{
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetNumberField(TEXT("BotId"), P.BotId);
			O->SetNumberField(TEXT("Payment"), P.Payment);
			A.Add(MakeShared<FJsonValueObject>(O));
		}
		L->SetArrayField(TEXT("PayCheck"), A);
	}
	{
		TArray<TSharedPtr<FJsonValue>> A;
		for (const FPhantomMatch& P : PhantomMatches)
		{
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetNumberField(TEXT("Team1"), P.Team1);
			O->SetNumberField(TEXT("Team2"), P.Team2);
			O->SetNumberField(TEXT("Score1"), P.ScoreTeam1);
			O->SetNumberField(TEXT("Score2"), P.ScoreTeam2);
			O->SetNumberField(TEXT("Ladder"), P.LadderId);
			O->SetNumberField(TEXT("Match"), P.MatchId);
			O->SetNumberField(TEXT("GameTime"), P.GameTime);
			A.Add(MakeShared<FJsonValueObject>(O));
		}
		L->SetArrayField(TEXT("PhantomMatches"), A);
	}
	J->SetObjectField(TEXT("LastMatch"), L);
}

void ArenaSP::FProfile::FromJson(const TSharedPtr<FJsonObject>& J)
{
	auto GetNum = [&J](const TCHAR* K, double Def) { double V = Def; J->TryGetNumberField(K, V); return V; };
	auto GetS = [&J](const TCHAR* K) { FString V; J->TryGetStringField(K, V); return V; };
	auto GetB = [&J](const TCHAR* K, bool Def) { bool V = Def; J->TryGetBoolField(K, V); return V; };
	PlayerName = GetS(TEXT("PlayerName"));
	PlayerCharacter = GetS(TEXT("PlayerCharacter"));
	TeamName = GetS(TEXT("TeamName"));
	TeamSymbolName = GetS(TEXT("TeamSymbolName"));
	BaseDifficulty = float(GetNum(TEXT("BaseDifficulty"), 2.0));
	Revision = int32(GetNum(TEXT("Revision"), 5));
	LadderProgress.Reset();
	{
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (J->TryGetArrayField(TEXT("LadderProgress"), A))
		{
			for (const TSharedPtr<FJsonValue>& V : *A)
			{
				LadderProgress.Add(int32(V->AsNumber()));
			}
		}
		while (LadderProgress.Num() < NumLadders)
		{
			LadderProgress.Add(LadderProgress.Num() == 0 ? 1 : -1);
		}
	}
	CustomLadderProgress.Reset();
	const TSharedPtr<FJsonObject>* O = nullptr;
	if (J->TryGetObjectField(TEXT("CustomLadders"), O))
	{
		for (const auto& KV : (*O)->Values)
		{
			CustomLadderProgress.Add(FString(*KV.Key), int32(KV.Value->AsNumber()));
		}
	}
	CurrentLadder = int32(GetNum(TEXT("CurrentLadder"), 0));
	CurrentMenuRung = int32(GetNum(TEXT("CurrentMenuRung"), 0));
	EnemyTeam = GetS(TEXT("EnemyTeam"));
	Difficulty = float(GetNum(TEXT("Difficulty"), BaseDifficulty));
	AltPath = int32(GetNum(TEXT("AltPath"), 0));
	LevelRoute.Reset();
	if (J->TryGetObjectField(TEXT("LevelRoute"), O))
	{
		for (const auto& KV : (*O)->Values)
		{
			LevelRoute.Add(FCString::Atoi(*KV.Key), int32(KV.Value->AsNumber()));
		}
	}
	FinalEnemyTeam = GetS(TEXT("FinalEnemyTeam"));
	bCompleted = GetB(TEXT("bCompleted"), false);
	bCheater = GetB(TEXT("bCheater"), false);
	PlayerTeam = ReadStrArray(J, TEXT("PlayerTeam"));
	PlayerLineup.Reset();
	{
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (J->TryGetArrayField(TEXT("PlayerLineup"), A))
		{
			for (const TSharedPtr<FJsonValue>& V : *A)
			{
				PlayerLineup.Add(int32(V->AsNumber()));
			}
		}
	}
	PlayerPositions = ReadStrArray(J, TEXT("PlayerPositions"));
	BotStats.Reset();
	{
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (J->TryGetArrayField(TEXT("BotStats"), A))
		{
			for (const TSharedPtr<FJsonValue>& V : *A)
			{
				const TSharedPtr<FJsonObject> B = V->AsObject();
				FBotStats S;
				S.Name = B->GetStringField(TEXT("Name"));
				S.Price = int32(B->GetNumberField(TEXT("Price")));
				S.Health = int32(B->GetNumberField(TEXT("Health")));
				S.bFreeAgent = B->GetBoolField(TEXT("FreeAgent"));
				S.TeamId = int32(B->GetNumberField(TEXT("TeamId")));
				BotStats.Add(S);
			}
		}
	}
	TeamStats.Reset();
	{
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (J->TryGetArrayField(TEXT("TeamStats"), A))
		{
			for (const TSharedPtr<FJsonValue>& V : *A)
			{
				const TSharedPtr<FJsonObject> B = V->AsObject();
				FTeamStats S;
				S.Name = B->GetStringField(TEXT("Name"));
				S.Level = int32(B->GetNumberField(TEXT("Level")));
				S.Matches = int32(B->GetNumberField(TEXT("Matches")));
				S.Won = int32(B->GetNumberField(TEXT("Won")));
				S.Rating = float(B->GetNumberField(TEXT("Rating")));
				TeamStats.Add(S);
			}
		}
	}
	AltTeamRoster.Reset();
	if (J->TryGetObjectField(TEXT("AltTeamRoster"), O))
	{
		for (const auto& KV : (*O)->Values)
		{
			TArray<FString> Names;
			for (const TSharedPtr<FJsonValue>& V : KV.Value->AsArray())
			{
				Names.Add(V->AsString());
			}
			AltTeamRoster.Add(FString(*KV.Key), Names);
		}
	}
	Matches = int32(GetNum(TEXT("Matches"), 0));
	Wins = int32(GetNum(TEXT("Wins"), 0));
	Kills = int32(GetNum(TEXT("Kills"), 0));
	Deaths = int32(GetNum(TEXT("Deaths"), 0));
	Goals = int32(GetNum(TEXT("Goals"), 0));
	TotalTime = float(GetNum(TEXT("TotalTime"), 0));
	ReadIntArray(J, TEXT("Spree"), Spree);
	ReadIntArray(J, TEXT("MultiKills"), MultiKills);
	ReadIntArray(J, TEXT("SpecialAwards"), SpecialAwards);
	FightHistory.Reset();
	{
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (J->TryGetArrayField(TEXT("FightHistory"), A))
		{
			for (const TSharedPtr<FJsonValue>& V : *A)
			{
				const TSharedPtr<FJsonObject> B = V->AsObject();
				FFightHistory H;
				H.Date = B->GetStringField(TEXT("Date"));
				H.MatchData = B->GetStringField(TEXT("MatchData"));
				H.MatchExtra = B->GetStringField(TEXT("MatchExtra"));
				H.Level = B->GetStringField(TEXT("Level"));
				H.GameType = B->GetStringField(TEXT("GameType"));
				H.EnemyTeam = B->GetStringField(TEXT("EnemyTeam"));
				H.PrizeMoney = int32(B->GetNumberField(TEXT("PrizeMoney")));
				H.BalanceChange = int32(B->GetNumberField(TEXT("BalanceChange")));
				H.BonusMoney = int32(B->GetNumberField(TEXT("BonusMoney")));
				H.GameTime = float(B->GetNumberField(TEXT("GameTime")));
				H.bWon = B->GetBoolField(TEXT("Won"));
				H.bTeamGame = B->GetBoolField(TEXT("TeamGame"));
				ReadIntArray(B, TEXT("TeamScore"), H.TeamScore);
				const TArray<FString> Layout = ReadStrArray(B, TEXT("TeamLayout"));
				H.TeamLayout[0] = Layout.IsValidIndex(0) ? Layout[0] : FString();
				H.TeamLayout[1] = Layout.IsValidIndex(1) ? Layout[1] : FString();
				H.MyScore = float(B->GetNumberField(TEXT("MyScore")));
				H.MyKills = int32(B->GetNumberField(TEXT("MyKills")));
				H.MyDeaths = int32(B->GetNumberField(TEXT("MyDeaths")));
				H.MyAwards = B->GetStringField(TEXT("MyAwards"));
				FightHistory.Add(H);
			}
		}
	}
	Balance = int32(GetNum(TEXT("Balance"), 250));
	MinBalance = int32(GetNum(TEXT("MinBalance"), 75));
	MaxTeamSize = FMath::Clamp(int32(GetNum(TEXT("MaxTeamSize"), 5)), 1, 7);
	TeamPercentage = float(GetNum(TEXT("TeamPercentage"), TeamPercentage));
	MatchBonus = float(GetNum(TEXT("MatchBonus"), MatchBonus));
	LoserFee = float(GetNum(TEXT("LoserFee"), LoserFee));
	FeeIncrease = float(GetNum(TEXT("FeeIncrease"), FeeIncrease));
	InjuryChance = float(GetNum(TEXT("InjuryChance"), InjuryChance));
	InjuryTreatment = float(GetNum(TEXT("InjuryTreatment"), InjuryTreatment));
	ChallengeChance = float(GetNum(TEXT("ChallengeChance"), ChallengeChance));
	MapChallengeCost = float(GetNum(TEXT("MapChallengeCost"), MapChallengeCost));
	ForfeitFee = int32(GetNum(TEXT("ForfeitFee"), 0));
	LastInjured = int32(GetNum(TEXT("LastInjured"), -1));
	SpecialEvent = GetS(TEXT("SpecialEvent"));
	bInLadderGame = GetB(TEXT("bInLadderGame"), false);
	bIsChallenge = GetB(TEXT("bIsChallenge"), false);
	bGotChallenged = GetB(TEXT("bGotChallenged"), false);
	ChallengeClass = GetS(TEXT("ChallengeClass"));
	ChallengeVariable = GetS(TEXT("ChallengeVariable"));
	if (J->TryGetObjectField(TEXT("ChallengeInfo"), O))
	{
		ChallengeInfo = MatchFromJson(*O);
	}
	bShowDetails = GetB(TEXT("bShowDetails"), true);
	if (J->TryGetObjectField(TEXT("LastMatch"), O))
	{
		const TSharedPtr<FJsonObject>& L = *O;
		auto LN = [&L](const TCHAR* K, double Def) { double V = Def; L->TryGetNumberField(K, V); return V; };
		auto LS = [&L](const TCHAR* K) { FString V; L->TryGetStringField(K, V); return V; };
		auto LB = [&L](const TCHAR* K) { bool V = false; L->TryGetBoolField(K, V); return V; };
		bLmdFresh = LB(TEXT("Fresh"));
		LmdBalanceChange = int32(LN(TEXT("BalanceChange"), 0));
		bLmdWon = LB(TEXT("Won"));
		LmdEnemyTeam = LS(TEXT("EnemyTeam"));
		LmdGameType = LS(TEXT("GameType"));
		bLmdChallenge = LB(TEXT("Challenge"));
		LmdMap = LS(TEXT("Map"));
		LmdPrizeMoney = int32(LN(TEXT("PrizeMoney"), 0));
		LmdTotalBonusMoney = int32(LN(TEXT("TotalBonusMoney"), 0));
		ReadIntArray(L, TEXT("Spree"), LmdSpree);
		ReadIntArray(L, TEXT("MultiKills"), LmdMultiKills);
		LmdInjury = int32(LN(TEXT("Injury"), -1));
		LmdInjuryHealth = int32(LN(TEXT("InjuryHealth"), 100));
		LmdInjuryTreatment = int32(LN(TEXT("InjuryTreatment"), 0));
		LmdGameTime = float(LN(TEXT("GameTime"), 0));
		bLmdTeamGame = LB(TEXT("TeamGame"));
		LmdMyTeam = int32(LN(TEXT("MyTeam"), 1));
		PlayerMatchDetails.Reset();
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (L->TryGetArrayField(TEXT("Players"), A))
		{
			for (const TSharedPtr<FJsonValue>& V : *A)
			{
				const TSharedPtr<FJsonObject> B = V->AsObject();
				FPlayerMatchDetails D;
				D.Id = int32(B->GetNumberField(TEXT("Id")));
				D.Name = B->GetStringField(TEXT("Name"));
				D.Kills = float(B->GetNumberField(TEXT("Kills")));
				D.Score = float(B->GetNumberField(TEXT("Score")));
				D.Deaths = float(B->GetNumberField(TEXT("Deaths")));
				D.Team = int32(B->GetNumberField(TEXT("Team")));
				D.SpecialAwards = ReadStrArray(B, TEXT("Awards"));
				PlayerMatchDetails.Add(D);
			}
		}
		PayCheck.Reset();
		if (L->TryGetArrayField(TEXT("PayCheck"), A))
		{
			for (const TSharedPtr<FJsonValue>& V : *A)
			{
				const TSharedPtr<FJsonObject> B = V->AsObject();
				PayCheck.Add({ int32(B->GetNumberField(TEXT("BotId"))), int32(B->GetNumberField(TEXT("Payment"))) });
			}
		}
		PhantomMatches.Reset();
		if (L->TryGetArrayField(TEXT("PhantomMatches"), A))
		{
			for (const TSharedPtr<FJsonValue>& V : *A)
			{
				const TSharedPtr<FJsonObject> B = V->AsObject();
				FPhantomMatch M;
				M.Team1 = int32(B->GetNumberField(TEXT("Team1")));
				M.Team2 = int32(B->GetNumberField(TEXT("Team2")));
				M.ScoreTeam1 = float(B->GetNumberField(TEXT("Score1")));
				M.ScoreTeam2 = float(B->GetNumberField(TEXT("Score2")));
				M.LadderId = int32(B->GetNumberField(TEXT("Ladder")));
				M.MatchId = int32(B->GetNumberField(TEXT("Match")));
				M.GameTime = float(B->GetNumberField(TEXT("GameTime")));
				PhantomMatches.Add(M);
			}
		}
	}
}

// =================================================================================================
// Creation
// =================================================================================================

TSharedPtr<ArenaSP::FProfile> ArenaSP::FProfile::Create(const FString& Name, const FString& Character, const FString& InTeamName, const FString& Symbol,
	int32 InDifficulty)
{
	const FData& D = Get();
	const FProfileDefaults& Def = D.Defaults;
	TSharedPtr<FProfile> P = MakeShared<FProfile>();
	P->ProfileName = Name;
	P->PlayerName = Name;
	P->PlayerCharacter = Character;
	P->TeamName = InTeamName;
	P->TeamSymbolName = Symbol;
	P->Balance = Def.Balance;
	P->MinBalance = Def.MinBalance;
	P->MaxTeamSize = FMath::Clamp(Def.MaxTeamSize, 1, 7);
	P->TeamPercentage = Def.TeamPercentage;
	P->MatchBonus = Def.MatchBonus;
	P->LoserFee = Def.LoserFee;
	P->FeeIncrease = Def.FeeIncrease;
	P->InjuryChance = Def.InjuryChance;
	P->InjuryTreatment = Def.InjuryTreatment;
	P->ChallengeChance = Def.ChallengeChance;
	P->MapChallengeCost = Def.MapChallengeCost;
	// UT2K4GameProfile defaults: the qualification ladder starts after its tutorial rung, every other ladder is locked
	P->LadderProgress = { 1, -1, -1, -1, -1, -1, -1 };
	P->PlayerTeam.SetNum(P->MaxTeamSize);
	P->PlayerLineup = { 0, 1, 2, 3 };
	P->PlayerPositions.SetNum(P->MaxTeamSize);
	// UT2K4SPTab_ProfileNew.AddDefaultBots: the SP characters of the Egyptian and Mercenary species are free agents
	for (const FPlayerRecord& R : D.Players)
	{
		if (R.Menu.Equals(TEXT("SP"), ESearchCase::IgnoreCase)
			&& (R.Species.EndsWith(TEXT("SPECIES_Egypt"), ESearchCase::IgnoreCase) || R.Species.EndsWith(TEXT("SPECIES_Merc"), ESearchCase::IgnoreCase))
			&& !R.DefaultName.Equals(Character, ESearchCase::IgnoreCase))
		{
			P->GetBotPosition(R.DefaultName, true);
		}
	}
	for (const FString& A : Def.InitialFreeAgents)
	{
		P->GetBotPosition(A, true);
	}
	// AddDefaultTeams: the phantom teams
	for (const FString& T : D.PhantomGroup.Rosters)
	{
		P->GetTeamPosition(T, true);
	}
	P->BaseDifficulty = float(InDifficulty);
	P->Difficulty = P->BaseDifficulty;
	P->InjuryChance += P->BaseDifficulty / 30.f;
	P->ChallengeChance += P->BaseDifficulty / 30.f;
	P->FeeIncrease += P->BaseDifficulty / 300.f;
	P->TeamPercentage += P->BaseDifficulty / 30.f;
	P->MatchBonus += P->TeamPercentage / 10.f;
	P->InjuryTreatment += P->BaseDifficulty / 50.f;
	P->MapChallengeCost += P->BaseDifficulty / 100.f;
	P->AltPath = FMath::RandRange(0, MaxInt - 1);
	P->SpecialEvent = TEXT("WELCOME");
	P->bInLadderGame = true;
	return P;
}

// =================================================================================================
// Ladders
// =================================================================================================

namespace ArenaSPProfileLocal
{
	/** UT2K4LadderInfo.GetUT2K4MatchInfo: the rung (last one past the end) with the arena picked. */
	ArenaSP::FMatchInfo PickMatch(int32 Ladder, int32 Rung, int32 Path, bool bSelect)
	{
		const ArenaSP::FLadder* L = ArenaSP::FindLadder(Ladder);
		if (!L || L->Matches.Num() == 0 || Rung < 0)
		{
			return ArenaSP::FMatchInfo();
		}
		ArenaSP::FMatchInfo M = L->Matches[FMath::Min(Rung, L->Matches.Num() - 1)];
		const int32 N = M.AltLevels.Num();
		if (N > 0)
		{
			if (bSelect)
			{
				const FString& S = M.AltLevels[FMath::Abs(Path) % N];
				if (!S.IsEmpty())
				{
					M.LevelName = S;
				}
			}
			else
			{
				const int64 P = FMath::Abs(int64(Path) + int64(Rung + 1) * int64(Ladder + 1));
				const int32 I = M.Priority > 0 ? M.Priority : N;
				const FString& S = M.AltLevels[int32(P % I) % N];
				if (!S.IsEmpty())
				{
					M.LevelName = S;
				}
			}
		}
		return M;
	}
}

ArenaSP::FMatchInfo ArenaSP::FProfile::GetMatchInfo(int32 Ladder, int32 Rung) const
{
	const int32 Sel = GetSelectedLevel(Ladder, Rung);
	return Sel > -1 ? PickMatch(Ladder, Rung, Sel, true) : PickMatch(Ladder, Rung, AltPath, false);
}

ArenaSP::FMatchInfo ArenaSP::FProfile::GetCurrentMatchInfo() const
{
	return bIsChallenge ? ChallengeInfo : GetMatchInfo(CurrentLadder, CurrentMenuRung);
}

bool ArenaSP::FProfile::HasAltLevel(int32 Ladder, int32 Rung) const
{
	const FLadder* L = FindLadder(Ladder);
	if (!L || L->Matches.Num() == 0 || Rung < 0)
	{
		return false;
	}
	return L->Matches[FMath::Min(Rung, L->Matches.Num() - 1)].AltLevels.Num() > 1;
}

int32 ArenaSP::FProfile::GetAltLevel(int32 Ladder, int32 Rung) const
{
	const FLadder* L = FindLadder(Ladder);
	if (!L || L->Matches.Num() == 0 || Rung < 0)
	{
		return -1;
	}
	const FMatchInfo& M = L->Matches[FMath::Min(Rung, L->Matches.Num() - 1)];
	const int32 N = M.AltLevels.Num();
	if (N == 0)
	{
		return -1;
	}
	const int64 Base = FMath::Abs(int64(AltPath) + int64(Rung + 1) * int64(Ladder + 1));
	const int32 Orig = int32(Base % (M.Priority > 0 ? M.Priority : N));
	const int32 Selected = GetSelectedLevel(Ladder, Rung);
	if (Selected == Orig || Selected == -1)
	{
		int32 Path = int32((Base + 1) % N);
		if (Path == Orig)
		{
			Path = (Path + 1) % N;
		}
		return Path;
	}
	return Orig;
}

int32 ArenaSP::FProfile::GetSelectedLevel(int32 Ladder, int32 Rung) const
{
	const int32* S = LevelRoute.Find(Ladder * 100 + Rung);
	return S ? *S : -1;
}

void ArenaSP::FProfile::SetSelectedLevel(int32 Ladder, int32 Rung, int32 Selection)
{
	LevelRoute.Add(Ladder * 100 + Rung, Selection);
}

void ArenaSP::FProfile::ResetSelectedLevel(int32 Ladder, int32 Rung)
{
	LevelRoute.Remove(Ladder * 100 + Rung);
}

ArenaSP::FMatchInfo ArenaSP::FProfile::GetAltMatchInfo(int32 Ladder, int32 Rung) const
{
	// GetSelectedMatchInfo(GetAltLevel, bOrig = a level was chosen): back to the original arena, or the alternative
	return GetSelectedLevel(Ladder, Rung) != -1 ? PickMatch(Ladder, Rung, AltPath, false) : PickMatch(Ladder, Rung, GetAltLevel(Ladder, Rung), true);
}

int32 ArenaSP::FProfile::GetLadderProgress(int32 Ladder) const
{
	if (Ladder >= FirstCustomLadder)
	{
		const FLadder* L = FindLadder(Ladder);
		const int32* P = L ? CustomLadderProgress.Find(L->Class) : nullptr;
		return P ? *P : -1;
	}
	return LadderProgress.IsValidIndex(Ladder) ? LadderProgress[Ladder] : -1;
}

bool ArenaSP::FProfile::CompletedLadder(int32 Ladder) const
{
	const int32 Len = LengthOfLadder(Ladder);
	return Len >= 0 && GetLadderProgress(Ladder) >= Len;
}

bool ArenaSP::FProfile::OpenChampionshipLadder() const
{
	return CompletedLadder(LID_DOM) && CompletedLadder(LID_CTF) && CompletedLadder(LID_AS) && CompletedLadder(LID_BR);
}

void ArenaSP::FProfile::EnableLadder(int32 Ladder)
{
	if (Ladder == -1)
	{
		for (int32 L = LID_DM; L <= LID_AS; ++L)
		{
			EnableLadder(L);
		}
		return;
	}
	if (Ladder >= FirstCustomLadder)
	{
		if (const FLadder* L = FindLadder(Ladder))
		{
			int32& P = CustomLadderProgress.FindOrAdd(L->Class, -1);
			P = FMath::Max(P, 0);
		}
		return;
	}
	if (LadderProgress.IsValidIndex(Ladder) && LadderProgress[Ladder] == -1)
	{
		LadderProgress[Ladder] = 0;
	}
}

FString ArenaSP::FProfile::UpdateLadders(int32 Ladder)
{
	if (bIsChallenge)
	{
		return ChallengeInfo.SpecialEvent;
	}
	const FLadder* L = FindLadder(Ladder);
	if (!L)
	{
		return FString();
	}
	const int32 Progress = GetLadderProgress(Ladder);
	if (Progress > CurrentMenuRung || !L->Matches.IsValidIndex(Progress))
	{
		// a match completed before was played again
		return FString();
	}
	const FString Event = L->Matches[Progress].SpecialEvent;
	if (Ladder >= FirstCustomLadder)
	{
		CustomLadderProgress.FindOrAdd(L->Class, 0) = Progress + 1;
	}
	else
	{
		LadderProgress[Ladder] = Progress + 1;
	}
	return Event;
}

// =================================================================================================
// Team
// =================================================================================================

int32 ArenaSP::FProfile::GetNumTeammatesForMatch(const FMatchInfo& M) const
{
	return IsTeamGameType(M.GameType) ? M.NumBots / 2 : 0;
}

bool ArenaSP::FProfile::AddTeammate(const FString& Bot)
{
	if (Bot.IsEmpty())
	{
		return false;
	}
	PlayerTeam.SetNum(MaxTeamSize);
	for (int32 i = 0; i < MaxTeamSize; ++i)
	{
		if (PlayerTeam[i].IsEmpty())
		{
			PlayerTeam[i] = Bot;
			const int32 B = GetBotPosition(Bot, true);
			BotStats[B].bFreeAgent = false;
			BotStats[B].TeamId = -1;
			return true;
		}
		if (PlayerTeam[i].Equals(Bot, ESearchCase::IgnoreCase))
		{
			return false;
		}
	}
	return false;
}

bool ArenaSP::FProfile::ReleaseTeammate(const FString& Bot)
{
	PlayerTeam.SetNum(MaxTeamSize);
	for (int32 i = 0; i < MaxTeamSize; ++i)
	{
		if (!Bot.IsEmpty() && PlayerTeam[i].Equals(Bot, ESearchCase::IgnoreCase))
		{
			for (int32 j = i; j < PlayerTeam.Num() - 1; ++j)
			{
				PlayerTeam[j] = PlayerTeam[j + 1];
			}
			PlayerTeam.Last() = FString();
			const int32 B = GetBotPosition(Bot, true);
			BotStats[B].bFreeAgent = true;
			BotStats[B].TeamId = -1;
			return true;
		}
	}
	return false;
}

bool ArenaSP::FProfile::IsTeammate(const FString& Bot) const
{
	return PlayerTeam.ContainsByPredicate([&Bot](const FString& N) { return !N.IsEmpty() && N.Equals(Bot, ESearchCase::IgnoreCase); });
}

bool ArenaSP::FProfile::HasFullTeam() const
{
	for (int32 i = 0; i < MaxTeamSize; ++i)
	{
		if (!PlayerTeam.IsValidIndex(i) || PlayerTeam[i].IsEmpty())
		{
			return false;
		}
	}
	return true;
}

int32 ArenaSP::FProfile::FindBot(const FString& Bot) const
{
	return BotStats.IndexOfByPredicate([&Bot](const FBotStats& B) { return B.Name.Equals(Bot, ESearchCase::IgnoreCase); });
}

int32 ArenaSP::FProfile::GetBotPosition(const FString& Bot, bool bAdd)
{
	const int32 Found = FindBot(Bot);
	if (Found != INDEX_NONE || !bAdd || Bot.IsEmpty())
	{
		return Found;
	}
	FBotStats B;
	B.Name = Bot;
	const FPlayerRecord* R = FindPlayer(Bot);
	B.Price = R ? GetSalaryFor(*R) : GetSalaryFor(FPlayerRecord());
	B.Health = 100;
	B.bFreeAgent = true;
	B.TeamId = -1;
	return BotStats.Add(B);
}

int32 ArenaSP::FProfile::GetBotPrice(int32 BotId, float Increase)
{
	if (!BotStats.IsValidIndex(BotId))
	{
		return -1;
	}
	if (Increase > 1.f)
	{
		BotStats[BotId].Price += int32(Increase);
	}
	else if (Increase > 0.f)
	{
		BotStats[BotId].Price = int32(BotStats[BotId].Price * (1.f + Increase));
	}
	return BotStats[BotId].Price;
}

int32 ArenaSP::FProfile::FindTeam(const FString& Team) const
{
	return TeamStats.IndexOfByPredicate([&Team](const FTeamStats& T) { return T.Name.Equals(Team, ESearchCase::IgnoreCase); });
}

int32 ArenaSP::FProfile::GetTeamPosition(const FString& Team, bool bAdd)
{
	const int32 Found = FindTeam(Team);
	if (Found != INDEX_NONE || !bAdd || Team.IsEmpty())
	{
		return Found;
	}
	FTeamStats T;
	T.Name = Team;
	const FTeamRoster* R = ArenaSP::FindTeam(Team);
	T.Level = R ? R->TeamLevel : 0;
	const int32 J = TeamStats.Add(T);
	if (R)
	{
		for (const FString& N : R->RosterNames)
		{
			const int32 K = GetBotPosition(N, true);
			BotStats[K].TeamId = J;
			BotStats[K].bFreeAgent = false;
		}
	}
	return J;
}

FString ArenaSP::FProfile::GetEnemyTeamName(const FString& Magic)
{
	if (!Magic.StartsWith(TEXT(";")))
	{
		return Magic;
	}
	// ";Group;how" -> ["", "Group", "how"] with empty parts kept
	TArray<FString> P;
	Magic.ParseIntoArray(P, TEXT(";"), false);
	if (P.Num() < 3)
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("SinglePlayer: invalid team selection %s"), *Magic);
		return FString();
	}
	const FRosterGroup* G = FindGroup(P[1]);
	if (!G)
	{
		UE_LOG(LogOpenUT2k4, Warning, TEXT("SinglePlayer: invalid roster group %s"), *P[1]);
		return FString();
	}
	const FString& How = P[2];
	if (How.Equals(TEXT("final"), ESearchCase::IgnoreCase) && !FinalEnemyTeam.IsEmpty())
	{
		return FinalEnemyTeam;
	}
	TArray<FString> Teams;
	if (How.Equals(TEXT("least"), ESearchCase::IgnoreCase) || How.Equals(TEXT("most"), ESearchCase::IgnoreCase))
	{
		// least / most played against
		const float Mod = How.Equals(TEXT("most"), ESearchCase::IgnoreCase) ? -1.f : 1.f;
		float X = Mod * float(MaxInt);
		for (const FString& R : G->Rosters)
		{
			const int32 J = GetTeamPosition(R, true);
			const float V = TeamStats[J].Matches * Mod;
			if (V == X)
			{
				Teams.Add(R);
			}
			else if (V < X)
			{
				X = V;
				Teams = { R };
			}
		}
	}
	else if (How.Equals(TEXT("best"), ESearchCase::IgnoreCase) || How.Equals(TEXT("worst"), ESearchCase::IgnoreCase) || How.Equals(TEXT("final"), ESearchCase::IgnoreCase))
	{
		// best / worst rated against you
		const float Mod = How.Equals(TEXT("worst"), ESearchCase::IgnoreCase) ? -1.f : 1.f;
		float X = -Mod * float(MaxInt);
		for (const FString& R : G->Rosters)
		{
			const int32 J = GetTeamPosition(R, true);
			const float V = TeamStats[J].Rating * Mod;
			if (V == X)
			{
				Teams.Add(R);
			}
			else if (V > X)
			{
				X = V;
				Teams = { R };
			}
		}
	}
	else if (How.Equals(TEXT("random"), ESearchCase::IgnoreCase))
	{
		Teams = G->Rosters;
	}
	if (Teams.Num() == 0)
	{
		return G->Rosters[Rand(G->Rosters.Num())];
	}
	const FString Pick = Teams[Rand(Teams.Num())];
	if (How.Equals(TEXT("final"), ESearchCase::IgnoreCase))
	{
		FinalEnemyTeam = Pick;
	}
	return Pick;
}

TArray<FString> ArenaSP::FProfile::GetTeamRoster(const FString& Team) const
{
	if (const TArray<FString>* Alt = AltTeamRoster.Find(Team))
	{
		return *Alt;
	}
	for (const TPair<FString, TArray<FString>>& KV : AltTeamRoster)
	{
		if (KV.Key.Equals(Team, ESearchCase::IgnoreCase))
		{
			return KV.Value;
		}
	}
	const FTeamRoster* R = ArenaSP::FindTeam(Team);
	return R ? R->RosterNames : TArray<FString>();
}

int32 ArenaSP::FProfile::TreatmentCost(int32 BotId) const
{
	if (!BotStats.IsValidIndex(BotId))
	{
		return 0;
	}
	// round((100 - Health) * Price / 100 * InjuryTreatment), the first part in integers like the stock code
	const FBotStats& B = BotStats[BotId];
	return FMath::RoundToInt(float((100 - B.Health) * B.Price / 100) * InjuryTreatment);
}

int32 ArenaSP::FProfile::GetMinimalTeamFee(int32 Members, bool bIgnoreHealth) const
{
	TArray<int32> Fees;
	for (int32 i = 0; i < MaxTeamSize && i < PlayerTeam.Num(); ++i)
	{
		const int32 J = FindBot(PlayerTeam[i]);
		if (J != INDEX_NONE && !PlayerTeam[i].IsEmpty() && (BotStats[J].Health >= 100 || bIgnoreHealth))
		{
			Fees.Add(int32(BotStats[J].Price * LoserFee));
		}
	}
	Fees.Sort();
	if (Fees.Num() < Members)
	{
		return MaxInt;
	}
	int32 Sum = 0;
	for (int32 i = 0; i < Members; ++i)
	{
		Sum += Fees[i];
	}
	return Sum;
}

int32 ArenaSP::FProfile::GetMinimalEntryFeeFor(const FMatchInfo& M, bool bIgnoreHealth) const
{
	int64 Res = M.EntryFee;
	if (IsTeamGameType(M.GameType))
	{
		Res += GetMinimalTeamFee(M.NumBots / 2, bIgnoreHealth);
	}
	return int32(FMath::Min<int64>(Res, MaxInt));
}

int32 ArenaSP::FProfile::NumHealthyTeammates() const
{
	int32 N = 0;
	for (int32 i = 0; i < MaxTeamSize && i < PlayerTeam.Num(); ++i)
	{
		const int32 J = PlayerTeam[i].IsEmpty() ? INDEX_NONE : FindBot(PlayerTeam[i]);
		if (J != INDEX_NONE && BotStats[J].Health >= 100)
		{
			++N;
		}
	}
	return N;
}

void ArenaSP::FProfile::UpdateFreeAgentTeams()
{
	TArray<FString> Groups, Teams;
	TArray<int32> FreeGroups, FreeTeams;
	for (int32 i = LID_TDM; i < LID_CHAMP; ++i)
	{
		for (int32 j = 0; j < LengthOfLadder(i); ++j)
		{
			TArray<FString> Tmp;
			GetMatchInfo(i, j).EnemyTeamName.ParseIntoArray(Tmp, TEXT(";"), false);
			const bool bOpen = LadderProgress[i] <= j;
			if (Tmp.Num() > 1)
			{
				int32 K = Groups.IndexOfByPredicate([&Tmp](const FString& G) { return G.Equals(Tmp[1], ESearchCase::IgnoreCase); });
				if (K == INDEX_NONE)
				{
					K = Groups.Add(Tmp[1]);
					FreeGroups.Add(1);
				}
				if (bOpen)
				{
					FreeGroups[K] = 0;
				}
			}
			else if (Tmp.Num() == 1)
			{
				int32 K = Teams.IndexOfByPredicate([&Tmp](const FString& G) { return G.Equals(Tmp[0], ESearchCase::IgnoreCase); });
				if (K == INDEX_NONE)
				{
					K = Teams.Add(Tmp[0]);
					FreeTeams.Add(1);
				}
				if (bOpen)
				{
					FreeTeams[K] = 0;
				}
			}
		}
	}
	for (int32 i = 0; i < Groups.Num(); ++i)
	{
		if (FreeGroups[i] <= 0)
		{
			continue;
		}
		if (const FRosterGroup* G = FindGroup(Groups[i]))
		{
			for (const FString& R : G->Rosters)
			{
				if (!Teams.ContainsByPredicate([&R](const FString& T) { return T.Equals(R, ESearchCase::IgnoreCase); }))
				{
					Teams.Add(R);
					FreeTeams.Add(1);
				}
			}
		}
	}
	for (int32 i = 0; i < Teams.Num(); ++i)
	{
		const int32 TeamId = FindTeam(Teams[i]);
		if (TeamId == INDEX_NONE)
		{
			continue;
		}
		for (FBotStats& B : BotStats)
		{
			if (B.TeamId == TeamId)
			{
				B.bFreeAgent = FreeTeams[i] > 0 && !IsTeammate(B.Name);
			}
		}
	}
}

// =================================================================================================
// Match flow
// =================================================================================================

void ArenaSP::FProfile::StartNewMatch(int32 Ladder)
{
	bLmdFresh = false;
	bInLadderGame = true;
	CurrentLadder = Ladder;
	if (!bIsChallenge)
	{
		ChallengeClass.Reset();
	}
	const FMatchInfo M = GetCurrentMatchInfo();
	EnemyTeam = M.EnemyTeamName;
	Difficulty = BaseDifficulty + M.DifficultyModifier;
	EnemyTeam = GetEnemyTeamName(EnemyTeam);
	if (!bIsChallenge)
	{
		SpecialEvent.Reset();
	}
	// the forfeit fee: the minimal fee of the lineup, paid back when the match is registered (kept when you leave)
	bool bPay = true;
	if (bIsChallenge && ChallengeClass.EndsWith(TEXT("BloodRites")))
	{
		bPay = !bGotChallenged;
	}
	TeamMateRanking.Reset();
	ForfeitFee = 0;
	if (bPay)
	{
		const int32 Mates = GetNumTeammatesForMatch(M);
		for (int32 i = 0; i < Mates && i < PlayerLineup.Num(); ++i)
		{
			if (PlayerTeam.IsValidIndex(PlayerLineup[i]))
			{
				FRank R;
				R.BotId = GetBotPosition(PlayerTeam[PlayerLineup[i]]);
				TeamMateRanking.Add(R);
			}
		}
		ForfeitFee = PayTeamMates(Balance, LoserFee);
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("SinglePlayer: starting ladder %d rung %d (%s in %s) against %s, difficulty %.2f, forfeit fee %d"), CurrentLadder,
		CurrentMenuRung, *M.GameType, *M.LevelName, *EnemyTeam, Difficulty, ForfeitFee);
}

int32 ArenaSP::FProfile::PayTeamMates(int32 Money, float FeeModifier, int32 IncreaseFeeOf, float UpdateFee)
{
	int32 Total = 0;
	const int32 N = TeamMateRanking.Num();
	PayCheck.SetNum(N);
	for (int32 i = 0; i < N; ++i)
	{
		const int32 BotId = TeamMateRanking[i].BotId;
		float Fee = GetBotPrice(BotId) * FeeModifier;
		// bonus for the best ranked team mate: Money * MatchBonus * ((N - i) / N), an integer ratio in the stock code
		Fee += Money * MatchBonus * float((N - i) / N);
		PayCheck[i].BotId = BotId;
		PayCheck[i].Payment = int32(Fee);
		Balance = int32(Balance - Fee);
		Total += int32(Fee);
	}
	const int32 From = N - FMath::Min(IncreaseFeeOf, N);
	for (int32 i = N - 1; i >= From; --i)
	{
		GetBotPrice(TeamMateRanking[i].BotId, UpdateFee);
	}
	return Total;
}

void ArenaSP::FProfile::InjureTeamMate(int32 Number)
{
	Number = FMath::Max(1, Number);
	const int32 From = TeamMateRanking.Num() - FMath::Min(Number, TeamMateRanking.Num());
	for (int32 i = TeamMateRanking.Num() - 1; i >= From; --i)
	{
		if (FRand() > InjuryChance || !BotStats.IsValidIndex(TeamMateRanking[i].BotId))
		{
			continue;
		}
		// at least 25 % health, more the better the player did (kills / deaths)
		BotStats[TeamMateRanking[i].BotId].Health = FMath::Clamp(int32(FMath::Min(TeamMateRanking[i].Rank * 25.f, 60.f) + 25.f + 10.f * FRand()), 0, 99);
		LastInjured = TeamMateRanking[i].BotId;
		UE_LOG(LogOpenUT2k4, Log, TEXT("SinglePlayer: %s got injured, health %d%%"), *BotStats[LastInjured].Name, BotStats[LastInjured].Health);
	}
}

void ArenaSP::FProfile::RankTeamMates(const FMatchResult& R)
{
	TeamMateRanking.Reset();
	for (const FMatchResult::FPlayer& P : R.Players)
	{
		if (!P.bMe && P.Team == R.MyTeam)
		{
			FRank K;
			K.BotId = GetBotPosition(P.Name, true);
			K.Rank = (P.Kills + 1.f) / (P.Deaths + 1.f);
			TeamMateRanking.Add(K);
		}
	}
	TeamMateRanking.StableSort([](const FRank& A, const FRank& B) { return A.Rank > B.Rank; });
}

void ArenaSP::FProfile::RegisterGame(const FMatchResult& R)
{
	const FMatchInfo MI = GetCurrentMatchInfo();
	const FProfileDefaults& Def = Get().Defaults;
	const bool bWon = R.bWon;
	const bool bChallenge = bIsChallenge;
	const bool bBloodRites = bChallenge && ChallengeClass.EndsWith(TEXT("BloodRites"));
	Balance += ForfeitFee; // the forfeit fee is paid back
	ForfeitFee = 0;
	const int32 OldBalance = Balance;
	PayCheck.Reset();
	SpecialEvent.Reset();

	const FMatchResult::FPlayer* Me = R.Players.FindByPredicate([](const FMatchResult::FPlayer& P) { return P.bMe; });
	if (Me)
	{
		Kills += int32(Me->Kills);
		Goals += Me->Goals;
		Deaths += int32(Me->Deaths);
	}
	++Matches;
	TotalTime += R.GameTime;
	LmdTotalBonusMoney = 0;
	LastInjured = -1;
	for (int32 i = 0; i < 6; ++i)
	{
		Spree[i] += R.Spree[i];
		LmdSpree[i] = R.Spree[i];
		LmdTotalBonusMoney += R.Spree[i] * Def.SpreeBonus[i];
	}
	for (int32 i = 0; i < 7; ++i)
	{
		MultiKills[i] += R.MultiKills[i];
		LmdMultiKills[i] = R.MultiKills[i];
		LmdTotalBonusMoney += R.MultiKills[i] * Def.MultiKillBonus[i];
	}
	Balance += LmdTotalBonusMoney;

	float TeamRating = 0.f;
	if (CurrentLadder != LID_DM)
	{
		const int32 T = GetTeamPosition(EnemyTeam, true);
		if (TeamStats.IsValidIndex(T))
		{
			TeamStats[T].Matches++;
			if (!bWon)
			{
				TeamStats[T].Won++;
			}
			if (R.bTeamGame)
			{
				// enemy score / our score, rises faster than it falls
				TeamRating = (R.TeamScore[1 - R.MyTeam] + 1.f) / (R.TeamScore[R.MyTeam] + 1.f);
				RankTeamMates(R);
				const bool bInjure = bChallenge ? !bGotChallenged : true;
				if (bInjure)
				{
					InjureTeamMate();
				}
			}
			else
			{
				for (const FMatchResult::FPlayer& P : R.Players)
				{
					if (!P.bMe)
					{
						TeamRating = FMath::Max(TeamRating, P.Score);
					}
				}
				TeamRating /= ((Me ? Me->Score : 0.f) + 1.f);
			}
			if (bWon)
			{
				TeamRating = -TeamRating;
			}
			TeamStats[T].Rating += TeamRating;
		}
	}
	if (R.bTeamGame && !bChallenge)
	{
		// a beaten team may challenge you for a rematch
		float Chance = TeamRating;
		if (Chance > 0.f)
		{
			Chance = 0.5f / Chance;
		}
		else if (Chance > -0.5f)
		{
			Chance = -1.f - Chance;
		}
		Chance = 0.5f + FMath::Abs(Chance);
		if (FRand() < ChallengeChance * Chance)
		{
			const FString C = GetChallengeGame();
			if (!C.IsEmpty())
			{
				SpecialEvent = FString::Printf(TEXT("CHALLENGE %s %s;%s"), *EnemyTeam, *C, *SpecialEvent);
			}
		}
	}
	const bool bPayMates = bBloodRites ? !bGotChallenged : true;
	if (bWon)
	{
		SpecialEvent += TEXT(";") + UpdateLadders(CurrentLadder);
		++Wins;
		Balance += MI.PrizeMoney;
		if (R.bTeamGame && bPayMates)
		{
			PayTeamMates(MI.PrizeMoney, TeamPercentage, TeamMateRanking.Num() / 2, FeeIncrease);
		}
	}
	else if (R.bTeamGame && bPayMates)
	{
		// team mates take a part of the balance
		PayTeamMates(Balance, LoserFee, TeamMateRanking.Num() / 3, FeeIncrease / 2.f);
	}

	// last match details
	LmdBalanceChange = Balance - OldBalance;
	bLmdFresh = true;
	bLmdChallenge = bChallenge;
	bLmdTeamGame = R.bTeamGame;
	bLmdWon = bWon;
	LmdEnemyTeam = EnemyTeam;
	LmdGameType = R.GameName;
	LmdGameTime = R.GameTime;
	LmdMap = R.Map;
	LmdMyTeam = R.MyTeam;
	LmdPrizeMoney = MI.PrizeMoney;
	LmdInjury = LastInjured;
	if (LmdInjury > -1)
	{
		LmdInjuryHealth = BotStats[LmdInjury].Health;
		LmdInjuryTreatment = TreatmentCost(LmdInjury);
	}
	PlayerMatchDetails.Reset();
	for (const FMatchResult::FPlayer& P : R.Players)
	{
		FPlayerMatchDetails D;
		D.Id = P.Id;
		D.Name = P.Name;
		D.Kills = P.Kills;
		D.Score = P.Score;
		D.Deaths = P.Deaths;
		D.Team = P.Team >= 0 ? P.Team : (P.bMe ? 0 : -1);
		// special awards (procLastMatchDetails): Flak Monkey, Combo Whore, Head Hunter, Road Rampage, Hat Trick, Untouchable
		const int32 Counts[4] = { P.FlakCount, P.ComboCount, P.HeadCount, P.RanOverCount };
		const int32 Limits[4] = { Def.AwardFlakMonkey, Def.AwardComboWhore, Def.AwardHeadHunter, Def.AwardRoadRampage };
		for (int32 a = 0; a < 4; ++a)
		{
			if (Counts[a] >= Limits[a] && Def.SpecialAwardNames.IsValidIndex(a))
			{
				D.SpecialAwards.Add(Def.SpecialAwardNames[a]);
				if (P.bMe)
				{
					SpecialAwards[a]++;
				}
			}
		}
		if (P.Goals >= Def.AwardHatTrick && Def.SpecialAwardNames.IsValidIndex(4))
		{
			D.SpecialAwards.Add(Def.SpecialAwardNames[4]);
			if (P.bMe)
			{
				SpecialAwards[4]++;
			}
		}
		if (int32(P.Deaths) == Def.AwardUntouchable && !R.bTeamGame && Def.SpecialAwardNames.IsValidIndex(5))
		{
			D.SpecialAwards.Add(Def.SpecialAwardNames[5]);
			if (P.bMe)
			{
				SpecialAwards[5]++;
			}
		}
		PlayerMatchDetails.Add(D);
	}
	AddHistoryRecord(R, MI);
	bIsChallenge = false;
	if (CompletedLadder(LID_DM))
	{
		ProcPhantomMatches(FMath::CeilToInt(float(TeamStats.Num() - 1) / 2.f * FRand()));
	}
	if (bBloodRites && bGotChallenged)
	{
		// when you were challenged and lost, the trade goes the other way
		if (!bWon)
		{
			SpecialEvent += TEXT(";") + ChallengeInfo.SpecialEvent;
		}
		else
		{
			SpecialEvent = SpecialEvent.Replace(*ChallengeInfo.SpecialEvent, TEXT(""));
		}
	}
	if (Balance < MinBalance)
	{
		Balance = MinBalance;
		SpecialEvent += TEXT(";DONATION");
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("SinglePlayer: match registered: %s, balance %d (%+d), special events '%s'"), bWon ? TEXT("won") : TEXT("lost"),
		Balance, LmdBalanceChange, *SpecialEvent);
}

void ArenaSP::FProfile::AddHistoryRecord(const FMatchResult& R, const FMatchInfo& MI)
{
	FFightHistory H;
	H.Date = FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M"));
	H.Level = R.Map;
	H.GameType = R.GameName;
	H.EnemyTeam = EnemyTeam;
	H.PrizeMoney = MI.PrizeMoney;
	H.BalanceChange = LmdBalanceChange;
	H.BonusMoney = LmdTotalBonusMoney;
	H.GameTime = R.GameTime;
	H.bWon = R.bWon;
	H.bTeamGame = R.bTeamGame;
	if (R.bTeamGame)
	{
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const int32 Team = Side == 0 ? 1 - R.MyTeam : R.MyTeam;
			TArray<FString> Names;
			for (const FMatchResult::FPlayer& P : R.Players)
			{
				if (P.Team == Team)
				{
					Names.Add(P.Name);
				}
			}
			H.TeamScore[Side] = R.TeamScore[Team];
			H.TeamLayout[Side] = FString::Join(Names, TEXT(", "));
		}
	}
	for (const FMatchResult::FPlayer& P : R.Players)
	{
		if (P.bMe)
		{
			H.MyScore = P.Score;
			H.MyKills = int32(P.Kills);
			H.MyDeaths = int32(P.Deaths);
		}
	}
	const TCHAR* GP = TEXT("UT2K4GameProfile");
	if (bIsChallenge)
	{
		const FChallengeInfo* C = Get().Challenges.FindByPredicate([this](const FChallengeInfo& I) { return I.Class.Equals(ChallengeClass, ESearchCase::IgnoreCase); });
		H.MatchData = Loc(TEXT("XGame"), GP, TEXT("msgChallengeGame"), TEXT("Challenge game")) + TEXT(";") + (C ? C->Name : ChallengeClass);
		H.MatchExtra = bGotChallenged ? Loc(TEXT("XGame"), TEXT("ChallengeGame"), TEXT("msgGotChallenged"), TEXT("We got challenged"))
			: Loc(TEXT("XGame"), TEXT("ChallengeGame"), TEXT("msgWeChallenged"), TEXT("We challenged"));
		if (!ChallengeVariable.IsEmpty())
		{
			H.MatchExtra += TEXT(" ") + Loc(TEXT("XGame"), TEXT("ChallengeGame"), TEXT("msgFor"), TEXT("for")) + TEXT(" ") + ChallengeVariable;
		}
	}
	else if (CurrentLadder >= FirstCustomLadder)
	{
		const FLadder* L = FindLadder(CurrentLadder);
		H.MatchData = Loc(TEXT("XGame"), GP, TEXT("msgAdditionalLadder"), TEXT("Additional ladder game")) + TEXT(";") + (L ? L->Name : FString())
			+ TEXT(" ") + Loc(TEXT("XGame"), GP, TEXT("msgMatch"), TEXT("match")) + TEXT(" ") + FString::FromInt(CurrentMenuRung);
	}
	else
	{
		H.MatchData = Loc(TEXT("XGame"), GP, TEXT("msgLadderGame"), TEXT("Ladder game")) + TEXT(";")
			+ (CurrentLadder == LID_CHAMP ? Loc(TEXT("XGame"), GP, TEXT("msgChampionship"), TEXT("Championship")) : LadderDescription(CurrentLadder))
			+ TEXT(" ") + Loc(TEXT("XGame"), GP, TEXT("msgMatch"), TEXT("match")) + TEXT(" ") + FString::FromInt(CurrentMenuRung);
	}
	FightHistory.Add(H);
}

void ArenaSP::FProfile::ProcPhantomMatches(int32 Games)
{
	PhantomMatches.Reset();
	if (Games <= 0)
	{
		return;
	}
	TArray<FTeamStats> Enemies = TeamStats;
	Enemies.RemoveAll([this](const FTeamStats& T) { return T.Name.Equals(EnemyTeam, ESearchCase::IgnoreCase); });
	TArray<int32> History = { CurrentLadder * 100 + CurrentMenuRung };
	while (Games > 0 && Enemies.Num() > 1)
	{
		FPhantomMatch M;
		int32 Key = 0;
		int32 Tries = 0;
		do
		{
			// UT2K4LadderInfo.GetRandomLadder: TDM .. AS
			M.LadderId = Rand(LID_AS) + 1;
			M.MatchId = Rand(FMath::Max(1, LengthOfLadder(M.LadderId)));
			Key = M.LadderId * 100 + M.MatchId;
		} while (History.Contains(Key) && ++Tries < 100);
		History.Add(Key);
		const FMatchInfo MI = GetMatchInfo(M.LadderId, M.MatchId);
		int32 J = Rand(Enemies.Num());
		M.Team1 = FindTeam(Enemies[J].Name);
		int32 N1 = Enemies[J].Level;
		Enemies.RemoveAt(J);
		J = Rand(Enemies.Num());
		M.Team2 = FindTeam(Enemies[J].Name);
		int32 N2 = Enemies[J].Level;
		Enemies.RemoveAt(J);
		// -1 = as strong as the other team
		if (N1 == -1)
		{
			N1 = FMath::Max(N2, 0);
		}
		if (N2 == -1)
		{
			N2 = FMath::Max(N1, 0);
		}
		++N1;
		++N2;
		int32 Winner;
		if (Rand(N1 + N2) >= N2)
		{
			Winner = M.Team1;
			M.ScoreTeam1 = MI.GoalScore;
			M.ScoreTeam2 = (MI.GoalScore - 1.f) * FRand();
		}
		else
		{
			Winner = M.Team2;
			M.ScoreTeam1 = (MI.GoalScore - 1.f) * FRand();
			M.ScoreTeam2 = MI.GoalScore;
		}
		M.GameTime = (M.ScoreTeam1 + 1.f) * (M.ScoreTeam2 + 1.f) + FRand() * MI.PrizeMoney;
		PhantomMatches.Add(M);
		RandomIncreaseBotFee(Winner, FeeIncrease / 2.f);
		--Games;
	}
}

void ArenaSP::FProfile::RandomIncreaseBotFee(int32 TeamId, float UpdateFee)
{
	for (int32 i = 0; i < BotStats.Num(); ++i)
	{
		if (BotStats[i].TeamId == TeamId && FRand() > 0.5f)
		{
			GetBotPrice(i, UpdateFee);
		}
	}
}

FString ArenaSP::FProfile::GetChallengeGame() const
{
	TArray<FChallengeInfo> C = Get().Challenges;
	while (C.Num() > 0)
	{
		const int32 I = Rand(C.Num());
		// BloodRites needs a qualified team; the others can always be played
		const bool bCan = !C[I].Class.EndsWith(TEXT("BloodRites")) || CompletedLadder(LID_TDM);
		if (bCan)
		{
			return C[I].Class;
		}
		C.RemoveAt(I);
	}
	return FString();
}

TArray<ArenaSP::FMenuRequest> ArenaSP::FProfile::HandleSpecialEvents()
{
	TArray<FMenuRequest> Out;
	bool bDonation = false;
	if (Balance < MinBalance)
	{
		Balance = MinBalance;
		bDonation = !SpecialEvent.Contains(TEXT(";DONATION"));
	}
	auto Add = [&Out](FMenuRequest::EKind K, int32 Index = 0, const FString& P1 = FString(), const FString& P2 = FString())
	{
		FMenuRequest R;
		R.Kind = K;
		R.Index = Index;
		R.Param1 = P1;
		R.Param2 = P2;
		Out.Add(R);
	};
	TFunction<void(const FString&)> Handle = [&](const FString& Events)
	{
		TArray<FString> List;
		Events.ParseIntoArray(List, TEXT(";"), true);
		for (const FString& Ev : List)
		{
			TArray<FString> E;
			Ev.ParseIntoArray(E, TEXT(" "), true);
			if (E.Num() == 0)
			{
				continue;
			}
			const FString& Cmd = E[0];
			const FString Arg = E.Num() > 1 ? E[1] : FString();
			UE_LOG(LogOpenUT2k4, Log, TEXT("SinglePlayer: special event '%s'"), *Ev);
			if (Cmd == TEXT("WELCOME"))
			{
				Add(FMenuRequest::Message, 0);
			}
			else if (Cmd == TEXT("DRAFT"))
			{
				if (!HasFullTeam())
				{
					Add(FMenuRequest::Draft);
					Add(FMenuRequest::Message, 1);
				}
			}
			else if (Cmd == TEXT("QUALIFIED"))
			{
				if (Arg == TEXT("SINGLE"))
				{
					Add(FMenuRequest::Message, 2);
				}
				else if (Arg == TEXT("TEAM"))
				{
					Add(FMenuRequest::Message, 3);
					// won every match so far: the secret phantom team
					if (Matches == Wins)
					{
						GetTeamPosition(TEXT("xGame.PhantomEpic"), true);
					}
				}
			}
			else if (Cmd == TEXT("OPEN"))
			{
				int32 Msg = -1;
				if (Arg == TEXT("TDM")) { EnableLadder(LID_TDM); }
				else if (Arg == TEXT("CTF")) { EnableLadder(LID_CTF); Msg = 4; }
				else if (Arg == TEXT("BR")) { EnableLadder(LID_BR); Msg = 5; }
				else if (Arg == TEXT("DOM")) { EnableLadder(LID_DOM); }
				else if (Arg == TEXT("AS")) { EnableLadder(LID_AS); Msg = 6; }
				else if (Arg == TEXT("ALL")) { EnableLadder(-1); }
				else if (Arg == TEXT("CHAMP"))
				{
					EnableLadder(LID_CHAMP);
					// picks the final team
					const FString Final = GetEnemyTeamName(GetMatchInfo(LID_CHAMP, 0).EnemyTeamName);
					if (Final.Equals(TEXT("xGame.TeamThundercrash"), ESearchCase::IgnoreCase)) { Msg = 8; }
					else if (Final.Equals(TEXT("xGame.TeamIronSkull"), ESearchCase::IgnoreCase)) { Msg = 7; }
					else if (Final.Equals(TEXT("xGame.TeamCorrupt"), ESearchCase::IgnoreCase)) { Msg = 9; }
				}
				if (Msg > -1)
				{
					Add(FMenuRequest::Message, Msg);
				}
			}
			else if (Cmd == TEXT("COMPLETED"))
			{
				if (Arg == TEXT("CHAMP"))
				{
					if (!bCompleted)
					{
						Add(FMenuRequest::HighScore);
					}
					bCompleted = true;
					Add(FMenuRequest::Message, 10);
				}
				else if (OpenChampionshipLadder() && GetLadderProgress(LID_CHAMP) < 0)
				{
					Handle(TEXT("OPEN CHAMP"));
				}
			}
			else if (Cmd == TEXT("BALANCE"))
			{
				Balance += FCString::Atoi(*Arg);
			}
			else if (Cmd == TEXT("UPDATETEAMS"))
			{
				UpdateFreeAgentTeams();
			}
			else if (Cmd == TEXT("CHALLENGE") && E.Num() > 2)
			{
				Add(FMenuRequest::Challenge, 0, E[1], E[2]);
			}
			else if (Cmd == TEXT("DONATION"))
			{
				Add(FMenuRequest::Donation);
			}
			else if ((Cmd == TEXT("TRADE") || Cmd == TEXT("UNTRADE")) && E.Num() > 2)
			{
				// Bloodrites: a team mate changes teams
				TArray<FString> Roster = GetTeamRoster(E[1]);
				const float Mult = [this]()
				{
					const FChallengeInfo* C = Get().Challenges.FindByPredicate([](const FChallengeInfo& I) { return I.Class.EndsWith(TEXT("BloodRites")); });
					return C ? C->FeeMultiply : 3.f;
				}();
				if (Cmd == TEXT("TRADE"))
				{
					Roster.RemoveAll([&E](const FString& N) { return N.Equals(E[2], ESearchCase::IgnoreCase); });
					AltTeamRoster.Add(E[1], Roster);
					const int32 B = GetBotPosition(E[2]);
					if (B > -1)
					{
						BotStats[B].Health = 100;
						BotStats[B].TeamId = -1;
						Balance += int32(BotStats[B].Price * Mult); // the entry fee comes back
					}
					Add(FMenuRequest::Trade, 0, E[1], E[2]);
				}
				else
				{
					ReleaseTeammate(E[2]);
					Roster.AddUnique(E[2]);
					const int32 B = GetBotPosition(E[2]);
					if (B > -1)
					{
						BotStats[B].Health = 100;
						BotStats[B].bFreeAgent = false;
						BotStats[B].TeamId = GetTeamPosition(E[1]);
					}
					AltTeamRoster.Add(E[1], Roster);
					Add(FMenuRequest::UnTrade, 0, E[1], E[2]);
				}
			}
			else
			{
				UE_LOG(LogOpenUT2k4, Log, TEXT("SinglePlayer: unknown special event '%s'"), *Ev);
			}
		}
	};
	if (bInLadderGame && !SpecialEvent.IsEmpty())
	{
		Handle(SpecialEvent);
	}
	if (LastInjured != -1)
	{
		Add(FMenuRequest::Injury, LastInjured);
		LastInjured = -1;
	}
	bInLadderGame = false;
	bIsChallenge = false;
	if (bDonation)
	{
		Add(FMenuRequest::Donation);
	}
	return Out;
}

FString ArenaSP::FProfile::MoneyToString(int32 Money)
{
	const FString Credits = Loc(TEXT("XGame"), TEXT("UT2K4GameProfile"), TEXT("msgCredits"), TEXT("credits"));
	if (Money == 0)
	{
		return TEXT("0 ") + Credits;
	}
	const FString Prefix = Money < 0 ? TEXT("-") : TEXT("");
	int64 M = FMath::Abs(int64(Money));
	if (M == 1)
	{
		return Prefix + TEXT("1 ") + Loc(TEXT("XGame"), TEXT("UT2K4GameProfile"), TEXT("msgCredit"), TEXT("credit"));
	}
	FString Res;
	while (M > 0)
	{
		FString Part = FString::Printf(TEXT("%lld"), M % 1000);
		M /= 1000;
		if (M > 0)
		{
			Part = FString::Printf(TEXT("%03lld"), FCString::Atoi64(*Part));
		}
		Res = Res.IsEmpty() ? Part : Part + TEXT(",") + Res;
	}
	return Prefix + Res + TEXT(" ") + Credits;
}
