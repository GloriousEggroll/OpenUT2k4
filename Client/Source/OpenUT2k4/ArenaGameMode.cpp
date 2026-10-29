#include "ArenaGameMode.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "ArenaTranslocator.h"
#include "ArenaArtItems.h"
#include "ArenaFallbackScan.h"
#include "ArenaArtFX.h"
#include "OpenUT2k4.h"
#include "ArenaAS_Turret.h"
#include "ArenaAssets.h"
#include "ArenaCharacter.h"
#include "ArenaPlayerState.h"
#include "ArenaGameState.h"
#include "ArenaPlayerController.h"
#include "ArenaBotController.h"
#include "ArenaHUD.h"
#include "ArenaUserSettings.h"
#include "ArenaLevelBuilder.h"
#include "ArenaContent.h"
#include "ArenaRuntimeMap.h"
#include "ArenaUE2World.h"
#include "ArenaNavGraph.h"
#include "ArenaFlag.h"
#include "ArenaGameRules.h"
#include "ArenaGameSession.h"
#include "ArenaRules_CTF.h"
#include "ArenaWeapons.h"
#include "UScript/ArenaUScript.h"
#include "Net/UE2ServerWorld.h"
#include "GameFramework/PlayerStart.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/GameSession.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet/GameplayStatics.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "UnrealClient.h"

namespace
{
	// Original bot callsigns
	const TCHAR* BotNames[] = {
		TEXT("Vexa"), TEXT("Korrin"), TEXT("Dace"), TEXT("Mirel"), TEXT("Tavik"), TEXT("Sorn"), TEXT("Quill"), TEXT("Brannoc"),
		TEXT("Ilsa"), TEXT("Rook"), TEXT("Kestrel"), TEXT("Juno"), TEXT("Malak"), TEXT("Oryn"), TEXT("Petra"), TEXT("Zeph"),
	};

	FString SpreeMessage(int32 Spree)
	{
		switch (Spree)
		{
		case 5:  return TEXT("is on fire!");
		case 10: return TEXT("is relentless!");
		case 15: return TEXT("is unchained!");
		case 20: return TEXT("is an apex predator!");
		case 25: return TEXT("is legendary!");
		default: return FString();
		}
	}

	FString MultiKillMessage(int32 Count)
	{
		switch (Count)
		{
		case 2:  return TEXT("Double Frag!");
		case 3:  return TEXT("Triple Frag!");
		case 4:  return TEXT("Quad Frag!");
		default: return Count >= 5 ? TEXT("Frag Frenzy!") : FString();
		}
	}

	template <typename Func>
	void ForEachArenaPC(UWorld* World, Func&& F)
	{
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			if (AArenaPlayerController* PC = Cast<AArenaPlayerController>(It->Get()))
			{
				F(PC);
			}
		}
	}
}

AArenaGameMode::AArenaGameMode()
{
	PrimaryActorTick.bCanEverTick = true;
	DefaultPawnClass = AArenaCharacter::StaticClass();
	PlayerControllerClass = AArenaPlayerController::StaticClass();
	// ?SpectatorOnly=1 logins (watching a UT2004 server) get this class from GameModeBase; ours keeps the HUD
	// messages and the spectator controls
	ReplaySpectatorPlayerControllerClass = AArenaPlayerController::StaticClass();
	PlayerStateClass = AArenaPlayerState::StaticClass();
	GameStateClass = AArenaGameState::StaticClass();
	HUDClass = AArenaHUD::StaticClass();
	// Host Game: server name / password / UT2004 query answers (ArenaGameSession.h)
	GameSessionClass = AArenaGameSession::StaticClass();
	bUseSeamlessTravel = false;
}

AArenaGameState* AArenaGameMode::GetArenaGameState() const
{
	return GetGameState<AArenaGameState>();
}

bool AArenaGameMode::IsPlaying() const
{
	const AArenaGameState* GS = GetArenaGameState();
	return GS && GS->IsPlaying();
}

void AArenaGameMode::CreateRules(const FString& ModeId, const FString& Options)
{
	Rules = ArenaGameRulesRegistry::CreateRules(this, ModeId);
	if (Rules)
	{
		Rules->InitRules(this, Options);
	}
	else
	{
		UE_LOG(LogOpenUT2k4, Error, TEXT("Arena: no game rules registered (mode '%s')"), *ModeId);
	}
}

FString AArenaGameMode::GetModeId() const
{
	return Rules ? Rules->GetModeId() : FString(TEXT("DM"));
}

EArenaGameType AArenaGameMode::GetGameType() const
{
	return Rules ? Rules->GetLegacyGameType() : EArenaGameType::Deathmatch;
}

bool AArenaGameMode::IsTeamGame() const
{
	return Rules && Rules->IsTeamGame();
}

int32 AArenaGameMode::GetRound() const
{
	const AArenaGameState* GS = GetArenaGameState();
	return GS ? GS->Round : 1;
}

void AArenaGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);

	MatchOptions = Options;
	while (MatchOptions.StartsWith(TEXT("?")))
	{
		MatchOptions.RightChopInline(1);
	}

	// The rules come first: they provide the defaults and filter the layout.
	CreateRules(UGameplayStatics::ParseOption(Options, TEXT("Mode")), Options);
	const UArenaGameRules* InitialRules = Rules;

	LayoutId = UGameplayStatics::ParseOption(Options, TEXT("Layout"));
	const int32 DefaultBots = InitialRules ? InitialRules->GetDefaultNumBots() : 5;
	const int32 DefaultGoal = InitialRules ? InitialRules->GetDefaultGoalScore() : 25;
	const int32 DefaultTime = InitialRules ? InitialRules->GetDefaultTimeLimit() : 15;
	NumBots = FMath::Clamp(UGameplayStatics::GetIntOption(Options, TEXT("Bots"), DefaultBots), 0, 31);
	BotSkill = FMath::Clamp(UGameplayStatics::GetIntOption(Options, TEXT("Skill"), 3), 0, 7);
	GoalScore = FMath::Max(0, UGameplayStatics::GetIntOption(Options, TEXT("Goal"), DefaultGoal));
	TimeLimitMinutes = FMath::Max(0, UGameplayStatics::GetIntOption(Options, TEXT("Time"), DefaultTime));
	bInstagib = UGameplayStatics::GetIntOption(Options, TEXT("Instagib"), 0) != 0;
	PreferredHumanTeam = FMath::Clamp(UGameplayStatics::GetIntOption(Options, TEXT("Team"), 0), 0, 1);
	// DeathMatch.InitGame: ParseOption(Options, "Translocator") overrides the game type's default
	{
		const FString TransOpt = UGameplayStatics::ParseOption(Options, TEXT("Translocator"));
		bAllowTrans = TransOpt.IsEmpty() ? (InitialRules && InitialRules->GetDefaultTranslocator())
			: (TransOpt == TEXT("1") || TransOpt.Equals(TEXT("true"), ESearchCase::IgnoreCase));
		UE_LOG(LogOpenUT2k4, Log, TEXT("Arena: translocator %s%s"), bAllowTrans ? TEXT("on") : TEXT("off"), TransOpt.IsEmpty() ? TEXT(" (game type default)") : TEXT(""));
	}

	UE_LOG(LogOpenUT2k4, Log, TEXT("Arena: mode=%s layout=%s bots=%d skill=%d goal=%d time=%d instagib=%d"),
		*GetModeId(), *LayoutId, NumBots, BotSkill, GoalScore, TimeLimitMinutes, bInstagib ? 1 : 0);

	// UnrealScript mutators (?Mutator=Package.Class,...): the script world exists before pickups are placed.
	ArenaUScriptHooks::InitGame(this, Options);

	// The level is either a cooked map (/Game/Imported/Maps/<Map>, geometry and lighting already in it) or the
	// base level /Game/Maps/Arena, into which the map selected by ?Layout= is built from the content cache.
	const FString LevelPackage = UWorld::RemovePIEPrefix(GetWorld()->GetOutermost()->GetName());
	const FString LevelShortName = FPackageName::GetShortName(LevelPackage);
	const bool bBaseLevel = LevelPackage == AArenaLevelBuilder::GetBaseLevel();

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	LevelBuilder = GetWorld()->SpawnActor<AArenaLevelBuilder>(AArenaLevelBuilder::StaticClass(), FTransform::Identity, Params);
	if (LevelBuilder)
	{
		FString PlayMap;
		bool bGeometry = false;
		if (!bBaseLevel)
		{
			PlayMap = AArenaLevelBuilder::HasImportedData(LevelShortName) ? LevelShortName : FString();
			bGeometry = !PlayMap.IsEmpty();
		}
		else
		{
			// The requested map if this mode can be played on it, else the mode's default map, else any map that fits.
			const FString Requested = LayoutId;
			// a UT2004 server's map is always the server's map (its game state drives the world), whatever our mode
			// rules would pick for it
			const bool bServerMap = UGameplayStatics::HasOption(Options, TEXT("UE2Net"));
			LayoutId = AArenaLevelBuilder::ChooseLayout(Requested, Rules ? Rules->GetDefaultLayout() : FString(),
				[this, bServerMap](const FArenaLayoutInfo& Info) { return bServerMap || !Rules || Rules->SupportsLayout(Info); });
			if (!Requested.IsEmpty() && LayoutId != Requested)
			{
				// a bare map name ("DM-Rankin") naming the imported layout is the normal way to ask for a map
				if (AArenaLevelBuilder::GetMapNameFromLayoutId(LayoutId).Equals(Requested, ESearchCase::IgnoreCase))
				{
					UE_LOG(LogOpenUT2k4, Log, TEXT("Arena: using imported map %s"), *Requested);
				}
				else
				{
					UE_LOG(LogOpenUT2k4, Warning, TEXT("Arena: layout '%s' not available for %s, using '%s'"), *Requested, *GetModeId(), *LayoutId);
				}
			}
			PlayMap = AArenaLevelBuilder::GetMapNameFromLayoutId(LayoutId);
			if (PlayMap.IsEmpty())
			{
				UE_LOG(LogOpenUT2k4, Error, TEXT("Arena: no UT2004 maps available (convert a UT2004 install first), returning to the menu"));
				PendingTravelLevel = TEXT("/Game/Maps/MainMenu");
				PendingTravelOptions = TEXT("game=Menu");
			}
			else if (ArenaContent::UseRuntimeContent())
			{
				bGeometry = ArenaRuntimeMap::BuildWorld(GetWorld(), PlayMap);
				if (!bGeometry)
				{
					UE_LOG(LogOpenUT2k4, Error, TEXT("Arena: map %s could not be built from the content cache, returning to the menu"), *PlayMap);
					PendingTravelLevel = TEXT("/Game/Maps/MainMenu");
					PendingTravelOptions = TEXT("game=Menu");
				}
			}
			else
			{
				// Cooked imports: the map is its own level.
				PendingTravelLevel = AArenaLevelBuilder::GetLevelForLayout(LayoutId);
				PendingTravelOptions = MatchOptions;
				if (PendingTravelLevel == AArenaLevelBuilder::GetBaseLevel())
				{
					PendingTravelLevel = TEXT("/Game/Maps/MainMenu");
					PendingTravelOptions = TEXT("game=Menu");
				}
				UE_LOG(LogOpenUT2k4, Log, TEXT("Arena: %s is a cooked map level, travelling to %s"), *LayoutId, *PendingTravelLevel);
			}
		}
		if (bGeometry && !LevelBuilder->BuildImported(PlayMap, IsTeamGame()))
		{
			UE_LOG(LogOpenUT2k4, Error, TEXT("Arena: map %s could not be set up (marker json missing or no player starts)"), *PlayMap);
		}
		if (!LevelBuilder->GetBuiltLayoutId().IsEmpty())
		{
			LayoutId = LevelBuilder->GetBuiltLayoutId();
		}
		if (AWorldSettings* WS = GetWorld()->GetWorldSettings())
		{
			WS->KillZ = LevelBuilder->GetKillZ();
			WS->bEnableWorldBoundsChecks = true;
		}
	}

	// Let the rules place their actors; fall back (e.g. CTF without flag bases -> TDM) when the level can't host the mode.
	if (!PendingTravelLevel.IsEmpty())
	{
		return;
	}
	if (Rules && !Rules->OnLevelBuilt(LevelBuilder))
	{
		const FString Fallback = Rules->GetFallbackModeId();
		UE_LOG(LogOpenUT2k4, Warning, TEXT("Arena: mode %s cannot be played on %s, using %s"), *GetModeId(), *LayoutId, *Fallback);
		CreateRules(Fallback, Options);
		if (Rules)
		{
			Rules->OnLevelBuilt(LevelBuilder);
		}
	}
	// the map's script actors (map UScript, UScript/UScriptBridge.h) run as the authority: not in a UT2004 server's
	// world, where the server runs them and our script mirror (Net/UE2NetScriptHUD.h) shows its simulated copies
	if (!ArenaUE2World::IsUE2NetWorld(GetWorld()))
	{
		ArenaUScriptHooks::LevelBuilt(this);
	}
}

void AArenaGameMode::InitGameState()
{
	Super::InitGameState();
	if (AArenaGameState* GS = GetArenaGameState())
	{
		GS->GameType = GetGameType();
		GS->ModeId = GetModeId();
		GS->ModeName = Rules ? Rules->GetModeName() : FString(TEXT("Deathmatch"));
		GS->bTeamGame = IsTeamGame();
		if (GS->bTeamGame)
		{
			ArenaArtItems::PickTeamSymbols(TEXT("?") + MatchOptions, GS->TeamSymbols);
			UE_LOG(LogOpenUT2k4, Log, TEXT("Arena: team symbols red %s, blue %s"), *GS->TeamSymbols[0], *GS->TeamSymbols[1]);
		}
		GS->Round = 1;
		GS->bSidesSwapped = false;
		GS->bInstagib = bInstagib;
		GS->bAllowTrans = bAllowTrans && !bInstagib;
		GS->GoalScore = GoalScore;
		GS->TimeLimit = TimeLimitMinutes * 60;
		GS->RemainingTime = GS->TimeLimit;
		GS->Phase = EArenaMatchPhase::Countdown;
		GS->CountdownRemaining = 3;
		GS->LayoutName = LayoutId;
		for (const FArenaLayoutInfo& Info : AArenaLevelBuilder::GetLayouts())
		{
			if (Info.Id == LayoutId)
			{
				GS->LayoutName = Info.DisplayName;
			}
		}
		if (Rules)
		{
			Rules->InitGameState(GS);
		}
	}
}

void AArenaGameMode::StartPlay()
{
	Super::StartPlay();

	if (!PendingTravelLevel.IsEmpty())
	{
		// InitGame found no map to play here: leave on the next tick, without a match.
		GetWorldTimerManager().SetTimerForNextTick([this]()
		{
			UGameplayStatics::OpenLevel(this, FName(*PendingTravelLevel), true, PendingTravelOptions);
		});
		return;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	// the nav graph is for bots and the rules' bot helpers. A UT2004 server's world has neither (seconds on large
	// maps): only test runs whose play script walks routes (Net/UE2NetPlay.h route= / goto=) or -UE2NetNavGraph build it
	bool bWantNavGraph = true;
	if (ArenaUE2World::IsUE2NetWorld(GetWorld()))
	{
		FString PlayScript;
		FParse::Value(FCommandLine::Get(), TEXT("UE2NetPlayScript="), PlayScript, false);
		bWantNavGraph = FParse::Param(FCommandLine::Get(), TEXT("UE2NetNavGraph")) || PlayScript.Contains(TEXT("route=")) || PlayScript.Contains(TEXT("goto="));
		UE_LOG(LogOpenUT2k4, Log, TEXT("Arena: UT2004 server world, nav graph %s"), bWantNavGraph ? TEXT("built for the play script") : TEXT("not built"));
	}
	NavGraph = bWantNavGraph ? GetWorld()->SpawnActor<AArenaNavGraph>(AArenaNavGraph::StaticClass(), FTransform::Identity, Params) : nullptr;
	if (NavGraph && LevelBuilder)
	{
		TArray<FVector> NavHints;
		if (Rules)
		{
			Rules->GetNavHints(NavHints);
		}
		const double StartTime = FPlatformTime::Seconds();
		NavGraph->Build(LevelBuilder->GetArenaBounds(), LevelBuilder, NavHints);
		UE_LOG(LogOpenUT2k4, Log, TEXT("Arena: nav graph built in %.2f s (%d nodes)"), FPlatformTime::Seconds() - StartTime, NavGraph->NumNodes());
#if !UE_BUILD_SHIPPING
		if (FParse::Param(FCommandLine::Get(), TEXT("NavPadTest")))
		{
			NavGraph->RunPadPathTest();
		}
#endif
	}
#if !UE_BUILD_SHIPPING
	if (LevelBuilder && FParse::Param(FCommandLine::Get(), TEXT("ValidateAnchors")))
	{
		LevelBuilder->ValidateAnchors();
	}
#endif

	for (int32 i = 0; i < NumBots; ++i)
	{
		AddBot(i);
	}

	PhaseTimer = 0.f;
	SecondAccum = 0.f;
	// the 3-2-1 start messages and their beeps are shown by every player controller from the replicated countdown
	// (AArenaPlayerController::UpdateStartupMessages, UT2004 StartupMessage)

	if (Rules)
	{
		Rules->OnStartPlay();
	}
}

void AArenaGameMode::PostLogin(APlayerController* NewPlayer)
{
	if (AArenaPlayerState* PS = NewPlayer ? NewPlayer->GetPlayerState<AArenaPlayerState>() : nullptr)
	{
		const FString Name = UArenaUserSettings::Get()->PlayerName;
		ChangeName(NewPlayer, Name.IsEmpty() ? TEXT("Player") : Name, false);
		BalanceTeam(PS, PreferredHumanTeam);
	}
	Super::PostLogin(NewPlayer);
	if (Rules && NewPlayer)
	{
		Rules->OnPlayerJoined(NewPlayer);
	}
}

FString AArenaGameMode::InitNewPlayer(APlayerController* NewPlayerController, const FUniqueNetIdRepl& UniqueId, const FString& Options, const FString& Portal)
{
	const FString Result = Super::InitNewPlayer(NewPlayerController, UniqueId, Options, Portal);
	if (AArenaPlayerState* PS = NewPlayerController ? NewPlayerController->GetPlayerState<AArenaPlayerState>() : nullptr)
	{
		// ?Character= (UT2004 login option; remote players send theirs), else the local player's Settings > Player choice
		FString Character = UGameplayStatics::ParseOption(Options, TEXT("Character"));
		if (Character.IsEmpty() && NewPlayerController->IsLocalController())
		{
			Character = UArenaUserSettings::Get()->PlayerCharacter;
		}
		PS->CharacterName = Character.Left(64);
	}
	return Result;
}

void AArenaGameMode::Logout(AController* Exiting)
{
	if (Rules)
	{
		Rules->OnPlayerLeft(Exiting);
	}
	RespawnTimes.Remove(Exiting);
	ForcedRespawnDelays.Remove(Exiting);
	Bots.Remove(Exiting);
	Super::Logout(Exiting);
}

void AArenaGameMode::BalanceTeam(AArenaPlayerState* PS, int32 PreferredTeam)
{
	if (!PS)
	{
		return;
	}
	PS->Team = Rules ? Rules->ChooseTeam(PS, PreferredTeam) : ArenaTeam::None;
}

void AArenaGameMode::SetPlayerTeam(AController* Controller, int32 Team)
{
	AArenaPlayerState* PS = Controller ? Controller->GetPlayerState<AArenaPlayerState>() : nullptr;
	if (!PS)
	{
		return;
	}
	PS->Team = (Team == 0 || Team == 1) ? Team : ArenaTeam::None;
	if (AArenaCharacter* C = Cast<AArenaCharacter>(Controller->GetPawn()))
	{
		C->UpdateTeamColors();
	}
}

int32 AArenaGameMode::GetTeamSide(int32 Team) const
{
	const AArenaGameState* GS = GetArenaGameState();
	if ((Team == 0 || Team == 1) && GS && GS->bSidesSwapped)
	{
		return 1 - Team;
	}
	return Team;
}

AArenaBotController* AArenaGameMode::AddBot(int32 Index, int32 PreferredTeam)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AArenaBotController* Bot = GetWorld()->SpawnActor<AArenaBotController>(AArenaBotController::StaticClass(), FTransform::Identity, Params);
	if (!Bot)
	{
		return nullptr;
	}
	if (!Bot->PlayerState)
	{
		Bot->InitPlayerState();
	}

	const float Skill = FMath::Clamp(BotSkill + FMath::FRandRange(-0.5f, 0.5f), 0.f, 7.f);
	Bot->SetSkill(Skill);

	if (AArenaPlayerState* PS = Bot->GetPlayerState<AArenaPlayerState>())
	{
		// Bots are not registered with the session, so give them a unique player id like humans get
		// (the bot uses it for its random seed and its defender role).
		if (GameSession)
		{
			PS->SetPlayerId(GameSession->GetNextPlayerID());
		}
		PS->SetIsABot(true);
		PS->BotSkill = Skill;
		const int32 NumNames = UE_ARRAY_COUNT(BotNames);
		FString Name = BotNames[Index % NumNames];
		if (Index >= NumNames)
		{
			Name += FString::Printf(TEXT("%d"), Index / NumNames + 1);
		}
		// named rosters (single player ladders, ArenaSinglePlayer.cpp): ?BotNames=A,B,C?BotTeams=1,0,0 (-1 = no team)
		{
			TArray<FString> Names, Teams;
			UGameplayStatics::ParseOption(TEXT("?") + MatchOptions, TEXT("BotNames")).ParseIntoArray(Names, TEXT(","));
			UGameplayStatics::ParseOption(TEXT("?") + MatchOptions, TEXT("BotTeams")).ParseIntoArray(Teams, TEXT(","));
			if (Names.IsValidIndex(Index))
			{
				Name = Names[Index];
			}
			if (Teams.IsValidIndex(Index) && PreferredTeam < 0 && FCString::Atoi(*Teams[Index]) >= 0)
			{
				PreferredTeam = FCString::Atoi(*Teams[Index]);
			}
		}
		PS->SetPlayerName(Name);
		// Human team is filled first so bots balance around the player
		BalanceTeam(PS, PreferredTeam);
	}

	Bots.Add(Bot);
	RestartPlayer(Bot);
	if (Rules)
	{
		Rules->OnPlayerJoined(Bot);
	}
	return Bot;
}

void AArenaGameMode::GetDefaultInventory(AController* Controller, TArray<TSubclassOf<AArenaWeapon>>& OutWeapons) const
{
	if (bInstagib)
	{
		OutWeapons.Add(AWeap_InstaRifle::StaticClass());
	}
	else
	{
		OutWeapons.Add(AWeap_Gauntlet::StaticClass());
		OutWeapons.Add(AWeap_Carbine::StaticClass());
	}
	// DeathMatch.AddGameSpecificInventory: CreateInventory("XWeapons.TransLauncher") when AllowTransloc(); MutInstaGib
	// (bAllowTranslocator off by default) removes it in instagib
	if (bAllowTrans && !bInstagib)
	{
		OutWeapons.Add(AWeap_Translocator::StaticClass());
	}
	if (Rules)
	{
		Rules->ModifyDefaultInventory(Controller, OutWeapons);
	}
	ArenaUScriptHooks::ModifyDefaultInventory(Controller, OutWeapons);
}

UClass* AArenaGameMode::GetDefaultPawnClassForController_Implementation(AController* InController)
{
	if (Rules)
	{
		if (UClass* Class = Rules->GetPawnClass(InController))
		{
			return Class;
		}
	}
	return Super::GetDefaultPawnClassForController_Implementation(InController);
}

AActor* AArenaGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	TArray<APlayerStart*> Candidates;
	if (LevelBuilder)
	{
		for (APlayerStart* Start : LevelBuilder->GetPlayerStarts())
		{
			if (Start)
			{
				Candidates.Add(Start);
			}
		}
	}
	if (Candidates.Num() == 0)
	{
		for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
		{
			Candidates.Add(*It);
		}
	}

	AActor* Best = Rules ? Rules->ChoosePlayerStart(Player, Candidates) : nullptr;
	return Best ? Best : Super::ChoosePlayerStart_Implementation(Player);
}

void AArenaGameMode::RestartPlayer(AController* NewPlayer)
{
	if (!NewPlayer)
	{
		return;
	}
	// spectator-only players (?SpectatorOnly=1, e.g. watching a UT2004 server's map) never get a pawn
	if (NewPlayer->PlayerState && NewPlayer->PlayerState->IsOnlyASpectator())
	{
		return;
	}
	const AArenaGameState* GS = GetArenaGameState();
	if (GS && GS->Phase == EArenaMatchPhase::Ended)
	{
		return;
	}
	if (Rules && !Rules->CanRespawn(NewPlayer))
	{
		RespawnTimes.Remove(NewPlayer);
		ForcedRespawnDelays.Remove(NewPlayer);
		return;
	}
	RespawnTimes.Remove(NewPlayer);
	ForcedRespawnDelays.Remove(NewPlayer);
	Super::RestartPlayer(NewPlayer);
	if (AArenaCharacter* C = Cast<AArenaCharacter>(NewPlayer->GetPawn()))
	{
		C->UpdateTeamColors();
		if (Rules)
		{
			Rules->OnPawnSpawned(C);
		}
		ArenaUScriptHooks::PawnSpawned(C);
	}
}

void AArenaGameMode::RequestRespawn(AController* Controller)
{
	// a living pawn, or one that is not a character (a vehicle, a guided warhead), is not waiting to respawn
	const AArenaCharacter* CurrentChar = Controller ? Cast<AArenaCharacter>(Controller->GetPawn()) : nullptr;
	if (!Controller || (Controller->GetPawn() && (!CurrentChar || !CurrentChar->IsDead())))
	{
		return;
	}
	const AArenaGameState* GS = GetArenaGameState();
	if (GS && (GS->Phase == EArenaMatchPhase::Ended || GS->Phase == EArenaMatchPhase::RoundOver))
	{
		return;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	const float* When = RespawnTimes.Find(Controller);
	if (!When || Now >= *When)
	{
		RestartPlayer(Controller);
	}
}

void AArenaGameMode::ScheduleRespawn(AController* Controller, float DelaySeconds)
{
	if (!Controller)
	{
		return;
	}
	RespawnTimes.Add(Controller, GetWorld()->GetTimeSeconds() + DelaySeconds);
	ForcedRespawnDelays.Add(Controller, Rules ? Rules->GetForcedRespawnDelay(Controller) : 0.f);
}

void AArenaGameMode::CancelRespawn(AController* Controller)
{
	RespawnTimes.Remove(Controller);
	ForcedRespawnDelays.Remove(Controller);
}

void AArenaGameMode::RemovePawn(AController* Controller)
{
	APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	if (!Pawn)
	{
		return;
	}
	if (AArenaCharacter* C = Cast<AArenaCharacter>(Pawn))
	{
		C->StopFire(0);
		C->StopFire(1);
	}
	Controller->UnPossess();
	Pawn->Destroy();
}

bool AArenaGameMode::AreOnSameTeam(const AController* A, const AController* B) const
{
	return Rules ? Rules->AreOnSameTeam(A, B) : false;
}

float AArenaGameMode::ModifyDamage(float Damage, AController* InstigatorController, AArenaCharacter* Victim) const
{
	const float Final = Rules ? Rules->ModifyDamage(Damage, InstigatorController, Victim) : Damage;
	return Final > 0.f ? ArenaUScriptHooks::ModifyDamage(Final, InstigatorController, Victim) : Final;
}

bool AArenaGameMode::PlayerUse(AController* Controller)
{
	return Rules && Controller && Rules->OnPlayerUse(Controller);
}

// ---------------------------------------------------------------------------
// Kills and scoring
// ---------------------------------------------------------------------------

void AArenaGameMode::Killed(AController* Killer, AController* Victim, AArenaCharacter* VictimPawn, const FString& WeaponName, bool bHeadshot)
{
	if (ArenaUE2World::IsUE2NetWorld(GetWorld()))
	{
		// a UT2004 server's world: kills, scores, kill lines and respawns are the server's (Net/UE2NetWorld.h)
		UE_LOG(LogOpenUT2k4, Warning, TEXT("Kill: local kill of %s ignored (UT2004 server world)"), VictimPawn ? *VictimPawn->GetName() : TEXT("?"));
		return;
	}
	AArenaPlayerState* VictimPS = Victim ? Victim->GetPlayerState<AArenaPlayerState>() : (VictimPawn ? VictimPawn->GetPlayerState<AArenaPlayerState>() : nullptr);
	AArenaPlayerState* KillerPS = Killer ? Killer->GetPlayerState<AArenaPlayerState>() : nullptr;
	const bool bSuicide = !Killer || Killer == Victim;

	if (IsPlaying())
	{
		if (!Rules || !Rules->ScoreKill(Killer, Victim, bHeadshot))
		{
			ScoreKillDefault(Killer, Victim, bHeadshot);
		}
	}
	// Automatic turrets have no player state: UT2004 names the turret as the killer in its damage type's DeathString
	FString TurretName;
	const FString TurretText = (!bSuicide && !KillerPS)
		? AArenaAS_TurretController::KillText(Killer, VictimPS ? VictimPS->GetPlayerName() : FString(TEXT("Someone")), TurretName) : FString();
	UE_LOG(LogOpenUT2k4, Log, TEXT("Kill: %s -> %s [%s]%s"), bSuicide ? TEXT("(self)") : (KillerPS ? *KillerPS->GetPlayerName() : (TurretName.IsEmpty() ? TEXT("?") : *TurretName)),
		VictimPS ? *VictimPS->GetPlayerName() : TEXT("?"), *WeaponName, bHeadshot ? TEXT(" headshot") : TEXT(""));
	if (!TurretText.IsEmpty())
	{
		UE_LOG(LogOpenUT2k4, Log, TEXT("Kill message: %s"), *TurretText);
	}

	ForEachArenaPC(GetWorld(), [&](AArenaPlayerController* PC)
	{
		if (!TurretText.IsEmpty())
		{
			PC->AddNamedKillMessage(TurretName, VictimPS, WeaponName, TurretText);
		}
		else
		{
			PC->AddKillMessage(bSuicide ? nullptr : KillerPS, VictimPS, WeaponName, bHeadshot);
		}
	});

	if (AArenaPlayerController* VictimPC = Cast<AArenaPlayerController>(Victim))
	{
		VictimPC->NotifyDied(bSuicide ? nullptr : KillerPS, WeaponName);
	}
	if (AArenaBotController* VictimBot = Cast<AArenaBotController>(Victim))
	{
		VictimBot->NotifyPawnDied();
	}

	if (Rules)
	{
		Rules->OnKilled(Killer, Victim, VictimPawn, WeaponName, bHeadshot);
	}
	ArenaUScriptHooks::Killed(Killer, Victim, VictimPawn);
	UE2ServerHooks::Killed(Killer, Victim, VictimPawn, WeaponName, bHeadshot);

	if (Victim && (!Rules || Rules->CanRespawn(Victim)))
	{
		ScheduleRespawn(Victim, Rules ? Rules->GetRespawnDelay(Victim) : 1.f);
	}

	CheckEndConditions();
}

void AArenaGameMode::ScoreKillDefault(AController* Killer, AController* Victim, bool bHeadshot)
{
	AArenaPlayerState* VictimPS = Victim ? Victim->GetPlayerState<AArenaPlayerState>() : nullptr;
	AArenaPlayerState* KillerPS = Killer ? Killer->GetPlayerState<AArenaPlayerState>() : nullptr;
	const bool bSuicide = !Killer || Killer == Victim;
	const bool bTeamKill = !bSuicide && AreOnSameTeam(Killer, Victim);
	const bool bTeamScore = Rules && Rules->KillsAddTeamScore();

	// UE2 DeathMatch.Killed adrenaline: ADR_Kill (rules), ADR_MajorKill 10 for first blood / ending a spree /
	// multi kills / each 5 kills, ADR_MinorError -2 for a suicide, ADR_KillTeamMate -5
	auto Award = [](AController* C, float Amount)
	{
		if (AArenaCharacter* P = C ? Cast<AArenaCharacter>(C->GetPawn()) : nullptr)
		{
			P->AwardAdrenaline(Amount);
		}
	};
	if (VictimPS && VictimPS->Spree > 4 && !bSuicide && !bTeamKill)
	{
		Award(Killer, 10.f);
	}
	if (bSuicide && Killer)
	{
		Award(Killer, -2.f);
	}
	else if (bTeamKill)
	{
		Award(Killer, -5.f);
	}

	if (VictimPS)
	{
		VictimPS->Deaths++;
		if (VictimPS->Spree >= 5 && KillerPS && !bSuicide)
		{
			// UT2004 KillingSpreeMessage: RelatedPRI_1 = whose spree ended, RelatedPRI_2 = who ended it
			ForEachArenaPC(GetWorld(), [&](AArenaPlayerController* PC) { PC->AddLocalMessage(EArenaLocalMessage::KillingSpree, 0, VictimPS, KillerPS); });
			UE2ServerHooks::LocalMessage(TEXT("UnrealGame.KillingSpreeMessage"), 0, VictimPS, KillerPS, nullptr);
		}
		VictimPS->Spree = 0;
		VictimPS->MultiKillCount = 0;
	}

	if (bSuicide || bTeamKill)
	{
		AArenaPlayerState* Penalized = bSuicide ? VictimPS : KillerPS;
		if (Penalized)
		{
			Penalized->AddScore(-1.f);
			if (bSuicide)
			{
				Penalized->Suicides++;
			}
		}
		if (bTeamScore && Penalized && Penalized->Team >= 0)
		{
			AddTeamScore(Penalized->Team, -1);
		}
		return;
	}

	if (!KillerPS)
	{
		return;
	}

	KillerPS->Kills++;
	KillerPS->AddScore(1.f);
	KillerPS->Spree++;
	if (bTeamScore && KillerPS->Team >= 0)
	{
		AddTeamScore(KillerPS->Team, 1);
	}

	// First kill of the match
	static TWeakObjectPtr<UWorld> FirstBloodWorld;
	if (FirstBloodWorld.Get() != GetWorld())
	{
		FirstBloodWorld = GetWorld();
		Award(Killer, 10.f);
		ForEachArenaPC(GetWorld(), [&](AArenaPlayerController* PC) { PC->AddLocalMessage(EArenaLocalMessage::FirstBlood, 0, KillerPS); });
		UE2ServerHooks::LocalMessage(TEXT("UnrealGame.FirstBloodMessage"), 0, KillerPS, nullptr, nullptr);
		ArenaAssets::PlaySound2D(this, TEXT("S_Spree"));
	}

	Award(Killer, Rules ? Rules->GetKillAdrenaline() : 5.f);
	if (KillerPS->Spree > 0 && KillerPS->Spree % 5 == 0)
	{
		Award(Killer, 10.f);
	}

	const FString Spree = SpreeMessage(KillerPS->Spree);
	if (!Spree.IsEmpty())
	{
		// UT2004 KillingSpreeMessage switch: 0 at 5 kills, one more every 5
		ForEachArenaPC(GetWorld(), [&](AArenaPlayerController* PC) { PC->AddLocalMessage(EArenaLocalMessage::KillingSpree, KillerPS->Spree / 5 - 1, KillerPS); });
		UE2ServerHooks::LocalMessage(TEXT("UnrealGame.KillingSpreeMessage"), KillerPS->Spree / 5 - 1, KillerPS, nullptr, nullptr);
		ArenaAssets::PlaySound2D(this, TEXT("S_Spree"));
	}

	const float Now = GetWorld()->GetTimeSeconds();
	if (Now - KillerPS->LastKillTime < 4.f)
	{
		Award(Killer, 10.f);   // UnrealPlayer.LogMultiKills
	}
	KillerPS->MultiKillCount = (Now - KillerPS->LastKillTime < 4.f) ? KillerPS->MultiKillCount + 1 : 1;
	KillerPS->LastKillTime = Now;
	if (!MultiKillMessage(KillerPS->MultiKillCount).IsEmpty())
	{
		UE2ServerHooks::LocalMessage(TEXT("XGame.MultiKillMessage"), KillerPS->MultiKillCount - 1, nullptr, nullptr, Killer);
	}
	if (AArenaPlayerController* KillerPC = Cast<AArenaPlayerController>(Killer))
	{
		const FString Multi = MultiKillMessage(KillerPS->MultiKillCount);
		if (!Multi.IsEmpty())
		{
			// UT2004 MultiKillMessage switch = MultiKillLevel (1 = the second kill)
			KillerPC->AddLocalMessage(EArenaLocalMessage::MultiKill, KillerPS->MultiKillCount - 1);
		}
		else if (bHeadshot)
		{
			KillerPC->AddLocalMessage(EArenaLocalMessage::SpecialKill);
		}
	}
}

void AArenaGameMode::AddTeamScore(int32 Team, int32 Delta)
{
	AArenaGameState* GS = GetArenaGameState();
	if (GS && (Team == 0 || Team == 1))
	{
		GS->TeamScores[Team] = FMath::Max(0, GS->TeamScores[Team] + Delta);
	}
}

void AArenaGameMode::SetTeamScore(int32 Team, int32 Score)
{
	AArenaGameState* GS = GetArenaGameState();
	if (GS && (Team == 0 || Team == 1))
	{
		GS->TeamScores[Team] = FMath::Max(0, Score);
	}
}

void AArenaGameMode::CheckEndConditions()
{
	if (Rules)
	{
		Rules->CheckEndConditions();
	}
}

void AArenaGameMode::CheckTimeLimit()
{
	AArenaGameState* GS = GetArenaGameState();
	if (!GS || GS->TimeLimit <= 0 || GS->bOvertime)
	{
		return;
	}
	if (GS->RemainingTime > 0)
	{
		return;
	}
	if (Rules)
	{
		Rules->OnTimeLimitReached();
	}
}

void AArenaGameMode::StartMatch()
{
	AArenaGameState* GS = GetArenaGameState();
	if (!GS)
	{
		return;
	}
	GS->Phase = EArenaMatchPhase::Playing;
	GS->CountdownRemaining = 0;
	GS->RemainingTime = GS->TimeLimit;
	// "the match has begun" and the announcement: AArenaPlayerController::UpdateStartupMessages
	if (Rules)
	{
		if (!bMatchStarted)
		{
			bMatchStarted = true;
			Rules->OnMatchStart();
		}
		Rules->OnRoundStart(GS->Round);
	}
}

void AArenaGameMode::EndMatch(const FString& Winner, int32 WinTeam, APlayerState* WinPlayer)
{
	AArenaGameState* GS = GetArenaGameState();
	if (!GS || GS->Phase == EArenaMatchPhase::Ended)
	{
		return;
	}
	GS->Phase = EArenaMatchPhase::Ended;
	GS->WinnerText = Winner;
	GS->WinningTeam = WinTeam;
	GS->WinningPlayer = WinPlayer;
	EndTime = GetWorld()->GetTimeSeconds();
	BroadcastCenterMessage(Winner, WinTeam >= 0 ? ArenaTeam::Color(WinTeam) : FLinearColor(1.f, 0.85f, 0.2f), 8.f, TEXT("S_Flag_Captured"));

	// Freeze everyone in place (GameEnded: Pawn.TurnOff, Velocity 0 and PHYS_None, so bots and players stand still and
	// the end scoreboard comes up once the view target stops)
	for (TActorIterator<AArenaCharacter> It(GetWorld()); It; ++It)
	{
		It->StopFire(0);
		It->StopFire(1);
		if (UCharacterMovementComponent* Move = It->GetCharacterMovement())
		{
			Move->StopMovementImmediately();
			Move->DisableMovement();
		}
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("Arena: match ended - %s"), *Winner);
	if (Rules)
	{
		Rules->OnMatchEnd(Winner, WinTeam, WinPlayer);
	}
}

// ---------------------------------------------------------------------------
// Rounds
// ---------------------------------------------------------------------------

void AArenaGameMode::EndRound(int32 WinTeam, const FString& Message, float IntermissionSeconds)
{
	AArenaGameState* GS = GetArenaGameState();
	if (!GS || GS->Phase != EArenaMatchPhase::Playing)
	{
		return;
	}
	GS->Phase = EArenaMatchPhase::RoundOver;
	RoundOverEndTime = GetWorld()->GetTimeSeconds() + FMath::Max(0.f, IntermissionSeconds);
	if (!Message.IsEmpty())
	{
		BroadcastCenterMessage(Message, (WinTeam == 0 || WinTeam == 1) ? ArenaTeam::Color(WinTeam) : FLinearColor(1.f, 0.85f, 0.2f),
			FMath::Max(2.f, IntermissionSeconds - 1.f), TEXT("S_Flag_Captured"));
	}
	for (TActorIterator<AArenaCharacter> It(GetWorld()); It; ++It)
	{
		It->StopFire(0);
		It->StopFire(1);
	}
	UE_LOG(LogOpenUT2k4, Log, TEXT("Arena: round %d ended (winner team %d) - %s"), GS->Round, WinTeam, *Message);
	if (Rules)
	{
		Rules->OnRoundEnd(GS->Round, WinTeam);
	}
}

void AArenaGameMode::StartNextRound()
{
	AArenaGameState* GS = GetArenaGameState();
	if (!GS || GS->Phase == EArenaMatchPhase::Ended)
	{
		return;
	}
	GS->Round++;
	if (Rules && Rules->ShouldSwapSides(GS->Round))
	{
		GS->bSidesSwapped = !GS->bSidesSwapped;
	}

	// Everyone starts the round fresh
	TArray<AController*> Controllers;
	for (TActorIterator<AController> It(GetWorld()); It; ++It)
	{
		Controllers.Add(*It);
	}
	for (AController* C : Controllers)
	{
		RemovePawn(C);
	}
	RespawnTimes.Reset();
	ForcedRespawnDelays.Reset();

	if (Rules)
	{
		Rules->OnRoundReset(GS->Round);
	}

	GS->bOvertime = false;
	GS->RemainingTime = GS->TimeLimit;
	GS->Phase = EArenaMatchPhase::Countdown;
	GS->CountdownRemaining = 3;
	SecondAccum = 0.f;

	for (APlayerState* PS : GS->PlayerArray)
	{
		AController* C = PS ? Cast<AController>(PS->GetOwner()) : nullptr;
		if (C && !PS->IsOnlyASpectator())
		{
			RestartPlayer(C);
		}
	}
	BroadcastCenterMessage(FString::Printf(TEXT("Round %d"), GS->Round), FLinearColor::White, 1.5f, TEXT("S_Countdown"));
	UE_LOG(LogOpenUT2k4, Log, TEXT("Arena: round %d starting%s"), GS->Round, GS->bSidesSwapped ? TEXT(" (sides swapped)") : TEXT(""));
}

void AArenaGameMode::SetModeStatusLines(const TArray<FString>& Lines)
{
	if (AArenaGameState* GS = GetArenaGameState())
	{
		GS->ModeStatusLines = Lines;
	}
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void AArenaGameMode::TickAutomation(float Now)
{
	// Automated testing: -AutoScreenshot=<sec>[,<sec>...] takes screenshots, -AutoQuit=<sec> exits,
	// -AutoSpectate views through a bot for each screenshot
	{
		static TArray<float> ShotTimes;
		static bool bParsed = false;
		static float QuitTime = -1.f;
		if (!bParsed)
		{
			bParsed = true;
			FString Shots;
			// bShouldStopOnSeparator = false: the list is comma separated
			if (FParse::Value(FCommandLine::Get(), TEXT("AutoScreenshot="), Shots, false))
			{
				TArray<FString> Parts;
				Shots.ParseIntoArray(Parts, TEXT(","));
				for (const FString& P : Parts)
				{
					ShotTimes.Add(FCString::Atof(*P));
				}
			}
			FParse::Value(FCommandLine::Get(), TEXT("AutoQuit="), QuitTime);
		}
		// -AutoSpectate: view through a different bot shortly before each screenshot (checks bot movement)
		static int32 SpectateIndex = 0;
		static float NextSpectate = -1.f;
		if (NextSpectate < 0.f && ShotTimes.Num() > 0 && FParse::Param(FCommandLine::Get(), TEXT("AutoSpectate")))
		{
			NextSpectate = FMath::Max(0.f, ShotTimes[0] - 4.f);
		}
		if (NextSpectate >= 0.f && Now >= NextSpectate && Bots.Num() > 0)
		{
			APlayerController* PC = GetWorld()->GetFirstPlayerController();
			for (int32 Tries = 0; PC && Tries < Bots.Num(); ++Tries)
			{
				AController* Bot = Bots[SpectateIndex++ % Bots.Num()];
				if (Bot && Bot->GetPawn())
				{
					PC->SetViewTarget(Bot->GetPawn());
					break;
				}
			}
			NextSpectate = ShotTimes.Num() > 1 ? ShotTimes[1] - 4.f : 1.0e9f;
		}
		// -AutoViewStarts: view from fixed camera spots at the player starts (eye height, start yaw) instead,
		// spread over the starts, 2.5 s before each screenshot. Repeatable views for lighting comparisons.
		static int32 ViewShotIndex = 0;
		static float NextView = -1.f;
		static int32 ViewShotCount = 0;
		if (NextView < 0.f && ShotTimes.Num() > 0 && ViewShotCount == 0 && FParse::Param(FCommandLine::Get(), TEXT("AutoViewStarts")))
		{
			ViewShotCount = ShotTimes.Num();
			NextView = FMath::Max(0.f, ShotTimes[0] - 2.5f);
		}
		if (NextView >= 0.f && Now >= NextView)
		{
			TArray<APlayerStart*> Starts;
			for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
			{
				Starts.Add(*It);
			}
			Starts.Sort([](const APlayerStart& A, const APlayerStart& B) { return A.GetActorLocation().X != B.GetActorLocation().X ? A.GetActorLocation().X < B.GetActorLocation().X : A.GetActorLocation().Y < B.GetActorLocation().Y; });
			APlayerController* PC = GetWorld()->GetFirstPlayerController();
			if (PC && Starts.Num() > 0)
			{
				const APlayerStart* Start = Starts[((ViewShotIndex * Starts.Num()) / FMath::Max(1, ViewShotCount)) % Starts.Num()];
				FActorSpawnParameters CamParams;
				CamParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				if (ACameraActor* Cam = GetWorld()->SpawnActor<ACameraActor>(Start->GetActorLocation() + FVector(0.f, 0.f, 64.f), FRotator(0.f, Start->GetActorRotation().Yaw, 0.f), CamParams))
				{
					Cam->GetCameraComponent()->SetFieldOfView(100.f);
					Cam->GetCameraComponent()->bConstrainAspectRatio = false;
					if (APawn* Own = PC->GetPawn())
					{
						Own->SetActorHiddenInGame(true);
					}
					PC->SetViewTarget(Cam);
				}
			}
			++ViewShotIndex;
			NextView = ShotTimes.Num() > 1 ? ShotTimes[1] - 2.5f : 1.0e9f;
		}
#if !UE_BUILD_SHIPPING
		// -AutoViewActor=<class name part> [-AutoViewDist=<cm>] [-AutoViewHeight=<cm>]: close-up of the matching actors
		// (e.g. ArenaFlag), one per screenshot in turn, from 2 s before the shot; the camera follows the target (a flag
		// at its current spot, also when carried or dropped). For art checks of game objects.
		static FString ViewActorClass;
		static float NextActorView = -1.f;
		static int32 ActorViewIndex = 0;
		static TWeakObjectPtr<ACameraActor> ActorViewCam;
		static TWeakObjectPtr<AActor> ActorViewTarget;
		static FVector ActorViewDir = FVector::ForwardVector;
		auto ActorViewFocus = [](const AActor* Target)
		{
			float Height = 90.f;
			FParse::Value(FCommandLine::Get(), TEXT("AutoViewHeight="), Height);
			const AArenaFlag* Flag = Cast<AArenaFlag>(Target);
			return (Flag ? Flag->GetFlagLocation() : Target->GetActorLocation()) + FVector(0.f, 0.f, Height);
		};
		if (NextActorView < 0.f && ShotTimes.Num() > 0 && ViewActorClass.IsEmpty()
			&& FParse::Value(FCommandLine::Get(), TEXT("AutoViewActor="), ViewActorClass))
		{
			NextActorView = FMath::Max(0.f, ShotTimes[0] - 2.f);
		}
		if (NextActorView >= 0.f && Now >= NextActorView)
		{
			TArray<AActor*> Found;
			for (TActorIterator<AActor> It(GetWorld()); It; ++It)
			{
				if (It->GetClass()->GetName().Contains(ViewActorClass))
				{
					Found.Add(*It);
				}
			}
			Found.Sort([](const AActor& A, const AActor& B) { return A.GetName() < B.GetName(); });
			APlayerController* PC = GetWorld()->GetFirstPlayerController();
			if (PC && Found.Num() > 0)
			{
				AActor* Target = Found[ActorViewIndex % Found.Num()];
				ActorViewDir = FRotator(-12.f, Target->GetActorRotation().Yaw + 135.f + 90.f * (ActorViewIndex / Found.Num()), 0.f).Vector();
				if (!ActorViewCam.IsValid())
				{
					FActorSpawnParameters CamParams;
					CamParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
					ActorViewCam = GetWorld()->SpawnActor<ACameraActor>(ActorViewFocus(Target), ActorViewDir.Rotation(), CamParams);
				}
				if (ACameraActor* Cam = ActorViewCam.Get())
				{
					Cam->GetCameraComponent()->SetFieldOfView(80.f);
					Cam->GetCameraComponent()->bConstrainAspectRatio = false;
					if (APawn* Own = PC->GetPawn())
					{
						Own->SetActorHiddenInGame(true);
					}
					PC->SetViewTarget(Cam);
					ActorViewTarget = Target;
					UE_LOG(LogOpenUT2k4, Log, TEXT("AutoViewActor: %s at %s"), *Target->GetName(), *ActorViewFocus(Target).ToString());
				}
			}
			++ActorViewIndex;
			NextActorView = ShotTimes.Num() > 1 ? ShotTimes[1] - 2.f : 1.0e9f;
		}
		if (ActorViewCam.IsValid() && ActorViewTarget.IsValid())
		{
			float Dist = 320.f;
			FParse::Value(FCommandLine::Get(), TEXT("AutoViewDist="), Dist);
			ActorViewCam->SetActorLocationAndRotation(ActorViewFocus(ActorViewTarget.Get()) - ActorViewDir * Dist, ActorViewDir.Rotation());
		}
#endif
		if (ShotTimes.Num() > 0 && Now >= ShotTimes[0])
		{
			ShotTimes.RemoveAt(0);
			const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Screenshots") / FString::Printf(TEXT("auto_%03d.png"), FMath::RoundToInt(Now)));
			UE_LOG(LogOpenUT2k4, Log, TEXT("AutoScreenshot: %s"), *File);
			FScreenshotRequest::RequestScreenshot(File, false, false);
		}
		if (QuitTime > 0.f && Now >= QuitTime)
		{
			QuitTime = -1.f;
			// Score summary for automated runs
			if (const AArenaGameState* SumGS = GetArenaGameState())
			{
				FString Line = FString::Printf(TEXT("AutoQuit: mode=%s team scores %d - %d; players"), *GetModeId(), SumGS->TeamScores[0], SumGS->TeamScores[1]);
				for (const AArenaPlayerState* PS : SumGS->GetSortedPlayers())
				{
					if (PS)
					{
						Line += FString::Printf(TEXT(" %s=%.0f(k%d d%d o%d)"), *PS->GetPlayerName(), PS->GetScore(), PS->Kills, PS->Deaths, PS->ObjectiveScore);
					}
				}
				UE_LOG(LogOpenUT2k4, Log, TEXT("%s"), *Line);
			}
			ArenaFallbackScan::Report();
			GEngine->Exec(GetWorld(), TEXT("quit"));
		}
	}
}

void AArenaGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	AArenaGameState* GS = GetArenaGameState();
	if (!GS)
	{
		return;
	}
	const float Now = GetWorld()->GetTimeSeconds();
	TickAutomation(Now);
	ArenaArtFX::TickDebugGallery(GetWorld());
	ArenaFallbackScan::Tick(GetWorld());
	if (GetWorld()->URL.HasOption(TEXT("UE2Net")))
	{
		return; // watching a UT2004 server: its game state drives the match (Net/UE2NetWorld.h), no local rules or timers
	}

	SecondAccum += DeltaSeconds;
	const bool bSecondTick = SecondAccum >= 1.f;
	if (bSecondTick)
	{
		SecondAccum -= 1.f;
	}

	if (Rules)
	{
		Rules->Tick(DeltaSeconds);
	}

	switch (GS->Phase)
	{
	case EArenaMatchPhase::Countdown:
		if (bSecondTick)
		{
			GS->CountdownRemaining--;
			if (GS->CountdownRemaining <= 0)
			{
				StartMatch();
			}
		}
		break;

	case EArenaMatchPhase::Playing:
		if (bSecondTick && GS->TimeLimit > 0 && !GS->bOvertime)
		{
			GS->RemainingTime = FMath::Max(0, GS->RemainingTime - 1);
			if (GS->RemainingTime == 60)
			{
				BroadcastCenterMessage(TEXT("One minute remaining"), FLinearColor(1.f, 0.85f, 0.3f), 2.f, TEXT("S_Countdown"));
			}
			CheckTimeLimit();
		}
		break;

	case EArenaMatchPhase::RoundOver:
		if (Now >= RoundOverEndTime)
		{
			StartNextRound();
		}
		return; // no respawns between rounds: StartNextRound respawns everyone

	case EArenaMatchPhase::Ended:
		if (Now - EndTime > 20.f)
		{
			// hosting for UT2004 clients: the next map of the rotation, the clients follow (UE2 ServerTravel)
			if (!UE2ServerHooks::TravelToNextMap(this))
			{
				ReturnToMenu();
			}
		}
		return;
	}

	// Respawns: when the timer plus the forced delay runs out (bots: no extra delay, humans: 4 s by default)
	TArray<TWeakObjectPtr<AController>> ToRespawn;
	for (auto It = RespawnTimes.CreateIterator(); It; ++It)
	{
		AController* C = It.Key().Get();
		if (!C)
		{
			It.RemoveCurrent();
			continue;
		}
		const float* Forced = ForcedRespawnDelays.Find(It.Key());
		if (Now >= It.Value() + (Forced ? *Forced : 0.f))
		{
			ToRespawn.Add(It.Key());
		}
	}
	for (const TWeakObjectPtr<AController>& C : ToRespawn)
	{
		if (C.IsValid())
		{
			RestartPlayer(C.Get());
		}
	}
}

// ---------------------------------------------------------------------------
// CTF (forwarded to the CTF rules)
// ---------------------------------------------------------------------------

void AArenaGameMode::FlagTaken(AArenaFlag* Flag, AController* By)
{
	UE2ServerHooks::FlagEvent(Flag, By, 4);
	if (UArenaRules_CTF* CTF = Cast<UArenaRules_CTF>(Rules))
	{
		CTF->OnFlagTaken(Flag, By);
	}
}

void AArenaGameMode::FlagDropped(AArenaFlag* Flag, AController* By)
{
	UE2ServerHooks::FlagEvent(Flag, By, 2);
	if (UArenaRules_CTF* CTF = Cast<UArenaRules_CTF>(Rules))
	{
		CTF->OnFlagDropped(Flag, By);
	}
}

void AArenaGameMode::FlagReturned(AArenaFlag* Flag, AController* By)
{
	// UE2 CTFMessage: 1 returned by a player, 3 returned by itself
	UE2ServerHooks::FlagEvent(Flag, By, By ? 1 : 3);
	if (UArenaRules_CTF* CTF = Cast<UArenaRules_CTF>(Rules))
	{
		CTF->OnFlagReturned(Flag, By);
	}
}

void AArenaGameMode::FlagCaptured(AArenaFlag* Flag, AController* By)
{
	UE2ServerHooks::FlagEvent(Flag, By, 0);
	if (UArenaRules_CTF* CTF = Cast<UArenaRules_CTF>(Rules))
	{
		CTF->OnFlagCaptured(Flag, By);
	}
}

// ---------------------------------------------------------------------------
// Messaging / flow
// ---------------------------------------------------------------------------

void AArenaGameMode::BroadcastCenterMessage(const FString& Text, const FLinearColor& Color, float Duration, FName Sound)
{
	ForEachArenaPC(GetWorld(), [&](AArenaPlayerController* PC)
	{
		PC->AddCenterMessage(Text, Color, Duration);
	});
	if (!Sound.IsNone())
	{
		ArenaAssets::PlaySound2D(this, Sound);
	}
}

void AArenaGameMode::BroadcastTeamMessage(int32 Team, const FString& Text, const FLinearColor& Color, float Duration, FName Sound)
{
	bool bAny = false;
	ForEachArenaPC(GetWorld(), [&](AArenaPlayerController* PC)
	{
		const AArenaPlayerState* PS = PC->GetPlayerState<AArenaPlayerState>();
		if (PS && PS->Team == Team)
		{
			PC->AddCenterMessage(Text, Color, Duration);
			bAny = true;
		}
	});
	if (bAny && !Sound.IsNone())
	{
		ArenaAssets::PlaySound2D(this, Sound);
	}
}

void AArenaGameMode::SendCenterMessage(AController* To, const FString& Text, const FLinearColor& Color, float Duration)
{
	if (AArenaPlayerController* PC = Cast<AArenaPlayerController>(To))
	{
		PC->AddCenterMessage(Text, Color, Duration);
	}
}

void AArenaGameMode::ReturnToMenu()
{
	UGameplayStatics::OpenLevel(this, FName(TEXT("/Game/Maps/MainMenu")), true, TEXT("game=Menu"));
}

void AArenaGameMode::RestartMatch()
{
	// Reopen the current level (a cooked map, or the base level the runtime map is built into) with the same options.
	const FString LevelPackage = UWorld::RemovePIEPrefix(GetWorld()->GetOutermost()->GetName());
	UGameplayStatics::OpenLevel(this, FName(*LevelPackage), true, MatchOptions);
}
