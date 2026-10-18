#pragma once

#include "CoreMinimal.h"

/**
 * Stock single player tournament data (UT2004's XGame UT2K4LadderInfo / CLOnslaught match lists, team rosters, roster
 * groups, the UT2K4GameProfile economy defaults, the new profile defaults of GUI2K4 UT2K4SPTab_ProfileNew and the
 * character list of the System .upl files). Nothing of UT2004 is shipped or copied into the source: everything is read
 * at runtime from the player's converted script packages (the UnrealScript runtime's loader, class defaults and the
 * default-property sub objects of the ladder classes), the .int localization and the .upl character lists.
 */
namespace ArenaSP
{
	/** Ladder ids (UT2K4LadderInfo LID_*); additional ladders (CLOnslaught ...) start at FirstCustomLadder. */
	enum ELadder : int32 { LID_DM = 0, LID_TDM = 1, LID_CTF = 2, LID_BR = 3, LID_DOM = 4, LID_AS = 5, LID_CHAMP = 6, NumLadders = 7, FirstCustomLadder = 10 };

	/** A UT2K4MatchInfo record. */
	struct FMatchInfo
	{
		/** Sub object name in the stock package ("DM1", "ONS3"). */
		FString Id;
		FString LevelName;
		FString MenuName;
		/** Team roster class, or a ";Group;least|most|best|worst|final|random" selection string. */
		FString EnemyTeamName;
		FString SpecialEvent;
		FString URLString;
		/** UE2 game class ("xGame.xDeathMatch"). */
		FString GameType;
		FString Requirements;
		TArray<FString> AltLevels;
		float DifficultyModifier = 0.f;
		float GoalScore = 0.f;
		float TimeLimit = 0.f;
		int32 NumBots = 0;
		int32 Priority = 0;
		int32 PrizeMoney = 0;
		int32 EntryFee = 0;
	};

	struct FLadder
	{
		int32 Id = 0;
		/** Stock class ("XGame.CLOnslaught" for additional ladders). */
		FString Class;
		/** Additional ladders: LadderName ("Onslaught"); empty for the main ladders. */
		FString Name;
		TArray<FMatchInfo> Matches;
		/** Labels next to the rungs (CustomLadderInfo.EntryLabels / UT2K4SPTab_Qualification.EntryLabels). */
		TArray<FString> EntryLabels;
	};

	/** A UT2K4TeamRoster (or UT2K4DMRoster) class. */
	struct FTeamRoster
	{
		FString Class;
		FString TeamName;
		FString TeamDescription;
		FString TeamSymbolName;
		FString TeamLeader;
		int32 TeamLevel = 0;
		TArray<FString> RosterNames;
		/** UT2K4DMRoster.BotList (random deathmatch opponents). */
		TArray<FString> BotList;
		bool bPhantom = false;
		bool bDMRoster = false;
		bool bValid = false;
	};

	/** UT2K4RosterGroup. */
	struct FRosterGroup
	{
		FString Class;
		TArray<FString> Rosters;
		int32 Difficulty = 0;
	};

	/** xUtil.PlayerRecord from a System .upl "Player=" line. */
	struct FPlayerRecord
	{
		FString DefaultName;
		FString Race;
		FString Mesh;
		FString Species;
		FString BodySkin;
		FString FaceSkin;
		FString Portrait;
		FString TextName;
		FString Sex;
		FString Menu;
		FString FavoriteWeapon;
		float Accuracy = 0.f;
		float Aggressiveness = 0.f;
		float StrafingAbility = 0.f;
		float CombatStyle = 0.f;
		float Tactics = 0.f;
		float ReactionTime = 0.f;
		float Jumpiness = 0.f;
		int32 BotUse = 1;
	};

	/** UT2K4GameProfile and UT2K4SPTab_ProfileNew defaults. */
	struct FProfileDefaults
	{
		int32 Balance = 250;
		int32 MinBalance = 75;
		int32 MaxTeamSize = 5;
		int32 SalaryCap = 3500;
		float TeamPercentage = 0.25f;
		float MatchBonus = 0.05f;
		float LoserFee = 0.1f;
		float FeeIncrease = 0.02f;
		float InjuryChance = 0.3f;
		float InjuryTreatment = 0.75f;
		float ChallengeChance = 0.3f;
		float MapChallengeCost = 0.1f;
		int32 SpreeBonus[6] = {};
		int32 MultiKillBonus[7] = {};
		int32 AwardFlakMonkey = 15, AwardComboWhore = 15, AwardHeadHunter = 15, AwardRoadRampage = 10, AwardHatTrick = 3, AwardUntouchable = 0;
		TArray<FString> PositionNames;
		TArray<FString> SpecialAwardNames;
		FString DefaultCharacter;
		int32 DefaultDifficulty = 2;
		TArray<FString> InitialFreeAgents;
		TArray<FString> DifficultyLevels;
		FString DefaultTeamName;
	};

	/** A challenge game (BloodRites, ManoEMano). */
	struct FChallengeInfo
	{
		FString Class;
		FString Name;
		FString Description;
		float FeeMultiply = 0.f;
	};

	struct FData
	{
		bool bValid = false;
		FString Error;
		/** Indexed by ELadder (DM .. CHAMP). */
		TArray<FLadder> Ladders;
		/** Additional ladders (index = ladder id - FirstCustomLadder). */
		TArray<FLadder> CustomLadders;
		FProfileDefaults Defaults;
		TArray<FChallengeInfo> Challenges;
		/** UT2K4LadderInfo.OpenNextLadderAtRung. */
		TArray<int32> OpenNextLadderAtRung;
		TArray<FPlayerRecord> Players;
		/** Team symbols offered for new profiles (UT2K4SPTab_ProfileNew.LoadSponsorSymbols). */
		TArray<FString> TeamSymbols;
		/** Phantom roster group (UT2K4TeamRosterPhantom), added to every new profile. */
		FRosterGroup PhantomGroup;
	};

	/** The data (read once, on first use). */
	OPENUT2K4_API const FData& Get();
	/** A ladder by id (main or custom), null if unknown. */
	OPENUT2K4_API const FLadder* FindLadder(int32 LadderId);
	OPENUT2K4_API int32 LengthOfLadder(int32 LadderId);
	/** A team roster class ("xGame.TeamNewSunblade"), loaded on demand. Null when the class is missing. */
	OPENUT2K4_API const FTeamRoster* FindTeam(const FString& Class);
	OPENUT2K4_API const FRosterGroup* FindGroup(const FString& Class);
	OPENUT2K4_API const FPlayerRecord* FindPlayer(const FString& Name);

	/** Our game mode id for a UE2 game class ("xGame.xCTFGame" -> "CTF"), empty when we have none. */
	OPENUT2K4_API FString GameTypeToMode(const FString& GameType);
	/** True for team game classes. */
	OPENUT2K4_API bool IsTeamGameType(const FString& GameType);
	/** Localized game name of a UE2 game class (its .int GameName). */
	OPENUT2K4_API FString GameTypeName(const FString& GameType);
	/** Localized ladder description (UT2K4LadderInfo.GetLadderDescription: the game name of the first match). */
	OPENUT2K4_API FString LadderDescription(int32 LadderId, int32 MatchId = 0);

	// xUtil character ratings and salary (same rules as the stock functions)
	OPENUT2K4_API int32 GetSalaryFor(const FPlayerRecord& P);
	OPENUT2K4_API int32 AccuracyRating(const FPlayerRecord& P);
	OPENUT2K4_API int32 AgilityRating(const FPlayerRecord& P);
	OPENUT2K4_API int32 TacticsRating(const FPlayerRecord& P);
	OPENUT2K4_API int32 AggressivenessRating(const FPlayerRecord& P);
	/** "Favorite weapon: Rocket Launcher" / "No preference" (xUtil.GetFavoriteWeaponFor). */
	OPENUT2K4_API FString FavoriteWeaponText(const FPlayerRecord& P);
	/** The character's description (XPlayers.int DecoText), with its name line. */
	OPENUT2K4_API FString CharacterText(const FPlayerRecord& P);

	/** A texture / material path of the stock data as the converted object ("Pkg.Name" -> "Pkg.Group.Name"; a wrong group is ignored). */
	OPENUT2K4_API FString ResolveObjectPath(const FString& Path);
	/** Texture object path of a team symbol given by its UE2 name ("TeamSymbols_UT2004.Thundercrash"). */
	OPENUT2K4_API FString SymbolTexture(const FString& Symbol);
	/** A class default of a stock class as text (objects as path names), e.g. ("GUI2K4.UT2K4SP_Message", "sounds", 3). */
	OPENUT2K4_API FString GetStockValue(const FString& Class, const TCHAR* Prop, int32 Index = 0);
	/** A dynamic array class default of a stock class, one text per element (structs as "(Key=Value,...)"). */
	OPENUT2K4_API TArray<FString> GetStockArray(const FString& Class, const TCHAR* Prop);
	/** Value of Key in a "(Key=Value,Key2=\"Text\")" struct text (quotes removed). */
	OPENUT2K4_API FString StructField(const FString& StructText, const TCHAR* Key);

	/** Localized string of the stock .int files, Fallback otherwise. */
	OPENUT2K4_API FString Loc(const TCHAR* Package, const TCHAR* Section, const TCHAR* Key, const FString& Fallback);
	/** Element of a localized array value ("(\"A\",\"B\")"). */
	OPENUT2K4_API FString LocArray(const TCHAR* Package, const TCHAR* Section, const TCHAR* Key, int32 Index, const FString& Fallback);
}
