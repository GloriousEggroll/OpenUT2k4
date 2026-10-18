#pragma once

#include "CoreMinimal.h"
#include "ArenaSPProfile.h"

class UWorld;

/** Shared helpers of ArenaSinglePlayer*.cpp (not part of the module API). */
namespace ArenaSPUI
{
	/** The match in progress (process lifetime: survives the travel to the match and back to the menu). */
	struct FSession
	{
		bool bActive = false;
		bool bResultReady = false;
		/** Open the single player page when the menu starts (after a match, even without a result). */
		bool bReturnToPage = false;
		/** Matches started in this process (test scripts). */
		int32 MatchesStarted = 0;
		FString ProfileName;
		/** The profile loaded last (reopened with the page). */
		FString LastProfile;
		FString PlayerName;
		FString Map;
		FString GameType;
		bool bTeamGame = false;
		int32 MyTeam = 1;
		/** Bot name -> team (-1 none). */
		TMap<FString, int32> BotTeams;
		ArenaSP::FMatchResult Result;
		// match observation
		TWeakObjectPtr<UWorld> MatchWorld;
		bool bSetup = false;
		float PlayTime = 0.f;
		int32 LastSpree = 0;
		int32 LastMulti = 0;
	};
	FSession& Session();
	void EnsureTicker();

	/** GUI2K4.int caption with "|" line breaks turned into new lines. */
	FString Caption(const TCHAR* Section, const TCHAR* Key, const TCHAR* Fallback);
	FString Lines(const FString& S);
	FString MapTitle(const FString& Map);
	bool HasMap(const FString& Map);
	/** First shot of the map's LevelSummary screenshot (sequence), as a texture path. */
	FString MapShot(const FString& Map);
	FString PortraitOf(const FString& Character);
	FString TeamTitle(const FString& RosterClass);
	FString MatchTitle(const ArenaSP::FMatchInfo& M);
	FString MeshOf(const FString& Character);
	FString OptionSafe(const FString& S);
	/** UT2K4MainPage.UnlockCharacter / IsUnlocked: unlocked character Menu tags (UArenaUserSettings.UnlockedCharacters). */
	bool UnlockCharacter(const FString& MenuTag);
	bool IsUnlocked(const FString& MenuTag);
}
