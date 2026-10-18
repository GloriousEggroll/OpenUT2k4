#pragma once

#include "CoreMinimal.h"
#include "ArenaSPData.h"

/**
 * A single player tournament profile: the state and rules of UT2004's UT2K4GameProfile (ladder progress, alternative
 * arenas, your team and the free agents with their prices and health, the enemy team statistics, money, the last
 * match details, phantom matches, challenges), reimplemented in C++ on the stock data of ArenaSPData.h.
 * Saved as JSON in GetUserDataDir()/SinglePlayer/<profile>.json (or -SPProfileDir=<dir>).
 */
namespace ArenaSP
{
	struct FBotStats
	{
		FString Name;
		/** Price to hire; also the base of the match fee and the injury treatment. */
		int32 Price = 0;
		/** 0..100 */
		int32 Health = 100;
		bool bFreeAgent = true;
		/** Index into FProfile::TeamStats of the team the bot plays for (-1: none / ours). */
		int32 TeamId = -1;
	};

	struct FTeamStats
	{
		/** Roster class ("xGame.TeamNewSunblade"). */
		FString Name;
		int32 Level = 0;
		int32 Matches = 0;
		/** Matches this team won against you. */
		int32 Won = 0;
		/** Rating compared to you (positive: they did well against you). */
		float Rating = 0.f;
	};

	struct FPlayerMatchDetails
	{
		int32 Id = 0;
		FString Name;
		float Kills = 0.f;
		float Score = 0.f;
		float Deaths = 0.f;
		int32 Team = -1;
		TArray<FString> SpecialAwards;
	};

	struct FPayCheck
	{
		int32 BotId = -1;
		int32 Payment = 0;
	};

	struct FPhantomMatch
	{
		int32 Team1 = -1;
		int32 Team2 = -1;
		float ScoreTeam1 = 0.f;
		float ScoreTeam2 = 0.f;
		int32 LadderId = 0;
		int32 MatchId = 0;
		float GameTime = 0.f;
	};

	struct FFightHistory
	{
		FString Date;
		FString MatchData;
		FString MatchExtra;
		FString Level;
		FString GameType;
		FString EnemyTeam;
		int32 PrizeMoney = 0;
		int32 BalanceChange = 0;
		int32 BonusMoney = 0;
		float GameTime = 0.f;
		bool bWon = false;
		bool bTeamGame = false;
		float TeamScore[2] = { 0.f, 0.f };
		FString TeamLayout[2];
		float MyScore = 0.f;
		int32 MyKills = 0;
		int32 MyDeaths = 0;
		FString MyAwards;
	};

	/** Result of a played match, collected from the game when it ends (ArenaSinglePlayer.cpp). */
	struct FMatchResult
	{
		struct FPlayer
		{
			int32 Id = 0;
			FString Name;
			float Kills = 0.f;
			float Deaths = 0.f;
			float Score = 0.f;
			int32 Goals = 0;
			int32 Team = -1;
			bool bMe = false;
			/** Special award counters (ArenaMatchStats.h): flak kills, adrenaline combos, sniper head shots, road kills. */
			int32 FlakCount = 0;
			int32 ComboCount = 0;
			int32 HeadCount = 0;
			int32 RanOverCount = 0;
		};
		bool bValid = false;
		bool bWon = false;
		bool bTeamGame = false;
		/** Our team index in the match (0 red, 1 blue). */
		int32 MyTeam = 1;
		/** Team scores by team index. */
		float TeamScore[2] = { 0.f, 0.f };
		float GameTime = 0.f;
		FString Map;
		FString GameName;
		/** Killing sprees (5, 10, 15, 20, 25, 30 kills) and multi kills (2 .. 8+) of the player in this match. */
		int32 Spree[6] = {};
		int32 MultiKills[7] = {};
		TArray<FPlayer> Players;
	};

	/** A message / page the menu shows after special events (UT2K4SP_Main.HandleSpecialEvent). */
	struct FMenuRequest
	{
		enum EKind : uint8 { Message, Draft, Injury, Challenge, Donation, HighScore, Trade, UnTrade, LadderMessage } Kind = Message;
		int32 Index = 0;
		FString Param1;
		FString Param2;
	};

	class OPENUT2K4_API FProfile
	{
	public:
		// ---- identity ----
		FString ProfileName;
		FString PlayerName;
		FString PlayerCharacter;
		FString TeamName;
		FString TeamSymbolName;
		float BaseDifficulty = 2.f;
		int32 Revision = 5;

		// ---- progress ----
		/** Per ladder id 0..6: rungs completed, -1 = locked. */
		TArray<int32> LadderProgress;
		/** Additional ladders: class -> progress. */
		TMap<FString, int32> CustomLadderProgress;
		int32 CurrentLadder = 0;
		int32 CurrentMenuRung = 0;
		FString EnemyTeam;
		float Difficulty = 2.f;
		/** Seed for the alternative arenas. */
		int32 AltPath = 0;
		/** Chosen alternative arenas: ladder * 100 + rung -> AltLevels index. */
		TMap<int32, int32> LevelRoute;
		FString FinalEnemyTeam;
		bool bCompleted = false;
		bool bCheater = false;

		// ---- team ----
		TArray<FString> PlayerTeam;
		/** Team slots playing the next match (-1: none), in lineup order. */
		TArray<int32> PlayerLineup;
		/** Position per team slot ("AUTO-ASSIGN", "DEFENSE" ...). */
		TArray<FString> PlayerPositions;
		TArray<FBotStats> BotStats;
		TArray<FTeamStats> TeamStats;
		/** Roster overrides of enemy teams after Bloodrites trades: roster class -> names. */
		TMap<FString, TArray<FString>> AltTeamRoster;

		// ---- stats ----
		int32 Matches = 0, Wins = 0, Kills = 0, Deaths = 0, Goals = 0;
		float TotalTime = 0.f;
		int32 Spree[6] = {};
		int32 MultiKills[7] = {};
		int32 SpecialAwards[6] = {};
		TArray<FFightHistory> FightHistory;

		// ---- money ----
		int32 Balance = 250;
		int32 MinBalance = 75;
		int32 MaxTeamSize = 5;
		float TeamPercentage = 0.25f;
		float MatchBonus = 0.05f;
		float LoserFee = 0.1f;
		float FeeIncrease = 0.02f;
		float InjuryChance = 0.3f;
		float InjuryTreatment = 0.75f;
		float ChallengeChance = 0.3f;
		float MapChallengeCost = 0.1f;
		int32 ForfeitFee = 0;
		int32 LastInjured = -1;

		// ---- match in progress / special events ----
		FString SpecialEvent;
		bool bInLadderGame = false;
		bool bIsChallenge = false;
		bool bGotChallenged = false;
		FString ChallengeClass;
		FString ChallengeVariable;
		FMatchInfo ChallengeInfo;
		bool bShowDetails = true;

		// ---- last match details ----
		bool bLmdFresh = false;
		int32 LmdBalanceChange = 0;
		bool bLmdWon = false;
		FString LmdEnemyTeam;
		FString LmdGameType;
		bool bLmdChallenge = false;
		FString LmdMap;
		int32 LmdPrizeMoney = 0;
		int32 LmdTotalBonusMoney = 0;
		int32 LmdSpree[6] = {};
		int32 LmdMultiKills[7] = {};
		int32 LmdInjury = -1;
		int32 LmdInjuryHealth = 100;
		int32 LmdInjuryTreatment = 0;
		float LmdGameTime = 0.f;
		bool bLmdTeamGame = false;
		int32 LmdMyTeam = 1;
		TArray<FPlayerMatchDetails> PlayerMatchDetails;
		TArray<FPayCheck> PayCheck;
		TArray<FPhantomMatch> PhantomMatches;

		// ---- persistence ----
		/** Folder of the profiles. */
		static FString GetDir();
		static TArray<FString> List();
		static TSharedPtr<FProfile> Load(const FString& Name);
		static bool Delete(const FString& Name);
		static bool Exists(const FString& Name);
		bool Save() const;

		/** A new profile (UT2K4SPTab_ProfileNew.onPlayClick): defaults, free agents, phantom teams, difficulty modifiers. */
		static TSharedPtr<FProfile> Create(const FString& Name, const FString& Character, const FString& InTeamName, const FString& Symbol, int32 InDifficulty);

		// ---- ladders ----
		/** The match of a rung with its arena chosen (alternative levels by AltPath or the player's choice). */
		FMatchInfo GetMatchInfo(int32 Ladder, int32 Rung) const;
		/** The match being played (challenge info or the current rung). */
		FMatchInfo GetCurrentMatchInfo() const;
		bool HasAltLevel(int32 Ladder, int32 Rung) const;
		/** AltLevels index the arena change would select. */
		int32 GetAltLevel(int32 Ladder, int32 Rung) const;
		int32 GetSelectedLevel(int32 Ladder, int32 Rung) const;
		void SetSelectedLevel(int32 Ladder, int32 Rung, int32 Selection);
		void ResetSelectedLevel(int32 Ladder, int32 Rung);
		/** The match with the arena CHANGE ARENA would switch to (UT2K4SP_MapChallenge). */
		FMatchInfo GetAltMatchInfo(int32 Ladder, int32 Rung) const;
		int32 GetLadderProgress(int32 Ladder) const;
		bool CompletedLadder(int32 Ladder) const;
		bool OpenChampionshipLadder() const;
		/** -1: every ladder except the championship. */
		void EnableLadder(int32 Ladder);
		/** Advances the ladder after a won match; returns the rung's special event. */
		FString UpdateLadders(int32 Ladder);

		// ---- team ----
		int32 GetMaxTeamSize() const { return MaxTeamSize; }
		int32 GetNumTeammatesForMatch(const FMatchInfo& M) const;
		bool AddTeammate(const FString& Bot);
		bool ReleaseTeammate(const FString& Bot);
		bool IsTeammate(const FString& Bot) const;
		bool HasFullTeam() const;
		int32 GetBotPosition(const FString& Bot, bool bAdd = false);
		int32 FindBot(const FString& Bot) const;
		/** Price of a bot; Increase > 1 adds, 0 < Increase <= 1 multiplies by (1 + Increase). */
		int32 GetBotPrice(int32 BotId, float Increase = 0.f);
		int32 GetTeamPosition(const FString& Team, bool bAdd = false);
		int32 FindTeam(const FString& Team) const;
		/** Resolves a ";Group;least" selection to a roster class. */
		FString GetEnemyTeamName(const FString& Magic);
		/** Roster of an enemy team (with Bloodrites changes). */
		TArray<FString> GetTeamRoster(const FString& Team) const;
		int32 TreatmentCost(int32 BotId) const;
		/** Minimal fee for Members healthy team mates if the match is lost. */
		int32 GetMinimalTeamFee(int32 Members, bool bIgnoreHealth = false) const;
		/** Entry fee plus the minimal team fee (INT32_MAX-ish when there are not enough team mates). */
		int32 GetMinimalEntryFeeFor(const FMatchInfo& M, bool bIgnoreHealth = false) const;
		int32 NumHealthyTeammates() const;
		/** UT2K4SP_Main.UpdateFreeAgentTeams: frees the members of teams you no longer meet. */
		void UpdateFreeAgentTeams();

		// ---- match flow ----
		/**
		 * StartNewMatch: resolves the enemy team, prepares the lineup ranking and holds back the forfeit fee (the
		 * minimal team fee, paid back when the match is registered). The entry fee is paid by the caller.
		 */
		void StartNewMatch(int32 Ladder);
		/** RegisterGame: stats, money, injuries, ladder progress, special events, phantom matches. */
		void RegisterGame(const FMatchResult& Result);
		/** The pages the menu should open for the special events (and applies their profile changes). */
		TArray<FMenuRequest> HandleSpecialEvents();
		/** Random challenge game the enemy may challenge you with (empty when none can). */
		FString GetChallengeGame() const;

		static FString MoneyToString(int32 Money);

	private:
		struct FRank { int32 BotId = -1; float Rank = 0.f; };
		TArray<FRank> TeamMateRanking;
		int32 PayTeamMates(int32 Money, float FeeModifier, int32 IncreaseFeeOf = 0, float UpdateFee = 0.f);
		void InjureTeamMate(int32 Number = 1);
		void RankTeamMates(const FMatchResult& R);
		void ProcPhantomMatches(int32 Games);
		void RandomIncreaseBotFee(int32 TeamId, float UpdateFee);
		void AddHistoryRecord(const FMatchResult& R, const FMatchInfo& MI);
		void ToJson(TSharedPtr<class FJsonObject> J) const;
		void FromJson(const TSharedPtr<class FJsonObject>& J);
	};
}
