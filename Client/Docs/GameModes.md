# Writing a game mode for OpenUT2k4

This guide is for anyone adding a game mode such as Bombing Run, Double Domination, Last Man Standing, Mutant,
Invasion, Assault or Onslaught. A mode is a `UArenaGameRules` subclass plus any actors it needs, in your own
files. You register it with one macro. The menu, the `?Mode=` option, the HUD hooks and the bot hooks then pick
it up without changes to shared code.

All gameplay is C++ in `Source/OpenUT2k4` (module `OpenUT2k4`, API macro `OPENUT2K4_API`). Engine: Unreal 5.8.

---

## 1. Files: what to create, what not to touch

**Create** (replace `XX` with your mode id, e.g. `BR`, `DOM`, `LMS`, `MUT`, `INV`, `AS`, `ONS`):

| File | Contents |
|------|----------|
| `Source/OpenUT2k4/ArenaRules_XX.h/.cpp` | `UArenaRules_XX : UArenaGameRules` and the `ARENA_REGISTER_RULES` line |
| `Source/OpenUT2k4/ArenaXX_<Thing>.h/.cpp` | your actors, e.g. `ArenaBR_Ball`, `ArenaDOM_ControlPoint`, `ArenaONS_PowerNode`, `ArenaINV_Monster` |

Name every class with your mode in it (`AArenaBR_Ball`, `UArenaRules_BR`). Give file-scope helpers your mode's
namespace (`namespace ArenaBR { ... }`), never a plain anonymous namespace with generic names. UBT merges files
into unity translation units, so two modes that both define `namespace { float Radius; }` break each other's build.

**Do not edit** these shared files. Other modes are developed against them at the same time:

`ArenaGameRules.*`, `ArenaGameMode.*`, `ArenaGameState.*`, `ArenaPlayerState.*`, `ArenaBotController.*`,
`ArenaCharacter.*`, `ArenaHUD.*`, `ArenaMenuGameMode.*`, `ArenaLevelBuilder.*`, `ArenaNavGraph.*`,
`ArenaCarryable.*`, `ArenaFlag.*`, `ArenaPickup.*`, `ArenaPlayerController.*`, `ArenaTypes.h`, `ArenaWeapon*.*`,
`ArenaRules_DM/TDM/CTF.*`, `OpenUT2k4.Build.cs`, `OpenUT2k4.uproject`, `Config/*`, `README.md`, `Tools/*`, `Scripts/*`.

If your mode needs something the framework does not offer (a new hook, a module dependency, a config change),
don't edit the shared file. Write the requirement in your final report so it can be added centrally. Until then,
work around it inside your own files (subclass, own actor, own component).

You may subclass shared classes in your own files, for example `AArenaINV_Monster : AArenaCharacter`,
`AArenaBR_Ball : AArenaCarryable` or `AArenaXX_Weapon : AArenaWeapon`.

Module dependencies already available: Core, CoreUObject, Engine, InputCore, EnhancedInput, AIModule, Slate,
SlateCore, Json.

---

## 2. Registration

```cpp
// ArenaRules_BR.cpp, at file scope, exactly once
#include "ArenaRules_BR.h"
ARENA_REGISTER_RULES(UArenaRules_BR, "BR", "Bombing Run");
```

- The first string is the **mode id**: what `?Mode=` takes (case-insensitive) and what the menu saves. The second
  string is the display name shown in the menu, HUD and scoreboard.
- Registration runs during static initialisation. The registry is a function-local static, so file order and unity
  builds do not matter. The macro stores `&UArenaRules_BR::StaticClass` and resolves it later, so never call
  `StaticClass()` or other UObject functions in static initialisers yourself.
- Menu order is fixed for the known ids: DM, TDM, CTF, BR, DOM, LMS, MUT, INV, AS, ONS. Other ids come after
  them, sorted alphabetically.
- The layout filter is your class's `SupportsLayout()`. `ARENA_REGISTER_RULES_WITH_LAYOUTS(Class, "ID", "Name", Fn)`
  takes a free function `bool Fn(const FArenaLayoutInfo&)` instead.
- Lookup API, in `ArenaGameRules.h`, namespace `ArenaGameRulesRegistry`: `GetModes()`, `Find(Id)`,
  `GetDisplayName(Id)`, `SupportsLayout(Id, Layout)`, `CreateRules(Outer, Id)`.
- An unknown `?Mode=` falls back to DM with a warning.

### The menu

You don't edit anything for the menu. Instant Action lists every registered mode, and it reads these virtuals from
your class default object:

| Virtual | Used for |
|---------|----------|
| `GetModeName()` | the "Game Type" value (defaults to the registered display name) |
| `SupportsLayout(const FArenaLayoutInfo&)` | which layouts the Arena row offers |
| `GetDefaultLayout()` | the map picked when the current one is not supported (else the first supported map) |
| `GetGoalLabel()`, `GetGoalRange(Min, Max, Step)`, `GetDefaultGoalScore()` | the goal row |
| `IsTeamGame()` | whether the "Your Team" row is shown |

START opens the level with `Mode=<ModeId>?Layout=...?Bots=..?Skill=..?Goal=..?Time=..?Instagib=0|1?Team=..`.
The instagib toggle applies to every mode. With instagib on, the default inventory is the instagib rifle, damage is
1000 and only adrenaline pickups spawn. Override `ModifyDamage`, `ModifyDefaultInventory` and `AllowPickup` if
your mode must behave differently. `GameMode->IsInstagib()` tells you whether it is on.

---

## 3. Match flow and where each hook is called

All hooks are virtual. The defaults implement plain Deathmatch, or the generic team game when `IsTeamGame()` is
true, so you only override what changes. **Server hooks** run on the authority; `GetGameMode()` is valid there.
**HUD hooks** also run on network clients, where `GetGameMode()` is **null** (see section 7).

```
AArenaGameMode::InitGame
  -> rules created from ?Mode=, InitRules(GameMode, Options)
  -> options read with your defaults (GetDefaultNumBots / GetDefaultGoalScore / GetDefaultTimeLimit)
  -> layout validated with SupportsLayout (else GetDefaultLayout, else the first supported map); the map is
     built into /Game/Maps/Arena from the content cache (runtime content) or is the cooked level; markers read
     pickups are filtered with AllowPickups / AllowPickup during the build
  -> OnLevelBuilt(Builder)       spawn your actors here; return false -> GetFallbackModeId() rules are used instead
InitGameState -> InitGameState(GS)
StartPlay
  -> GetNavHints(Points)         extra nav graph points (objective spots), then the nav graph is built
  -> bots added (ChooseTeam, spawn, OnPawnSpawned, OnPlayerJoined), OnStartPlay()
Countdown (3 s) -> Playing: OnMatchStart() once, then OnRoundStart(Round) for every round (1 too)
Every tick: Tick(DeltaSeconds), all phases (check GS->Phase)
Time limit reached: OnTimeLimitReached()
Kills: ScoreKill -> OnKilled -> CanRespawn / GetRespawnDelay -> CheckEndConditions()
EndMatch(...) -> OnMatchEnd(...), back to the menu after 20 s
```

### Hook reference (`UArenaGameRules`)

Identity and defaults. These are also called on the class default object, so they must not depend on instance state.

| Hook | Default |
|------|---------|
| `FString GetModeName() const` | registered display name |
| `bool IsTeamGame() const` | false |
| `EArenaGameType GetLegacyGameType() const` | TDM if team game, else DM (compatibility only) |
| `int32 GetDefaultGoalScore() const` | 25 |
| `int32 GetDefaultNumBots() const` | 5 |
| `int32 GetDefaultTimeLimit() const` | 15 (minutes, 0 = none) |
| `FString GetDefaultLayout() const` | `"Imported:DM-Rankin"` |
| `bool SupportsLayout(const FArenaLayoutInfo&) const` | `Layout.bSupportsDM` (every layout) |
| `FString GetGoalLabel() const` | `"Frag Limit"` |
| `void GetGoalRange(int32& Min, int32& Max, int32& Step) const` | 0, 100, 5 |
| `FString GetGoalText(int32 Goal) const` | `"First to %d"` (HUD line under the score) |
| `FString GetFallbackModeId() const` | `"TDM"` for team games, else `"DM"` |

Lifecycle (server):

| Hook | Notes |
|------|-------|
| `void InitRules(AArenaGameMode* GM, const FString& Options)` | **Call `Super::InitRules` first**: it sets `GameMode`. Read your own URL options with `UGameplayStatics::ParseOption` / `GetIntOption`. |
| `bool OnLevelBuilt(AArenaLevelBuilder* Builder)` | Spawn mode actors from anchors / imported json. Return false if the map can't host the mode. |
| `void InitGameState(AArenaGameState* GS)` | Fill game state fields. |
| `void GetNavHints(TArray<FVector>& OutPoints) const` | Points the bot graph must include, about 10 cm above the floor. Add every objective spot. |
| `void OnStartPlay()` | Nav graph built, bots exist. |
| `void OnMatchStart()` | First switch to Playing. |
| `void OnRoundStart(int32 Round)` | Each time a round becomes Playing. |
| `void OnRoundEnd(int32 Round, int32 WinningTeam)` | `EndRound` was called. |
| `void OnRoundReset(int32 Round)` | After the intermission: pawns are already removed. Reset objectives here; everyone respawns right after. |
| `bool ShouldSwapSides(int32 Round) const` | Return true to toggle `GS->bSidesSwapped` before round `Round` (>= 2). |
| `void OnMatchEnd(const FString& WinnerText, int32 WinningTeam, APlayerState* WinningPlayer)` | |
| `void Tick(float DeltaSeconds)` | Every game mode tick, all phases. |
| `void OnPlayerJoined(AController*)` / `void OnPlayerLeft(AController*)` | Humans and bots. |
| `int32 ChooseTeam(AArenaPlayerState* PS, int32 PreferredTeam) const` | Default: smaller team, the preferred one on ties. Non-team modes return `ArenaTeam::None`. |

Spawning and inventory (server):

| Hook | Notes |
|------|-------|
| `UClass* GetPawnClass(AController*) const` | nullptr = `AArenaCharacter`. Return your subclass for special pawns. |
| `AActor* ChoosePlayerStart(AController*, const TArray<APlayerStart*>& Candidates)` | Candidates are the level's starts, tagged `"Red"`, `"Blue"` or `"Any"`. You may return any actor; its transform is used. The default calls `PickBestStart(Player, Candidates, Side)` (own side's starts in team games, free and far from enemies). |
| `bool CanRespawn(AController*) const` | false keeps the player dead (spectating its corpse). Checked at death and at every respawn attempt. |
| `float GetRespawnDelay(AController*) const` | Default: bots 1.5 to 2.5 s, humans 1 s (then fire to respawn). |
| `float GetForcedRespawnDelay(AController*) const` | Extra time after which a waiting human is respawned automatically. Default 4 s, bots 0. |
| `void OnPawnSpawned(AArenaCharacter*)` | After possession and default inventory. Set health, give items, change appearance. |
| `void ModifyDefaultInventory(AController*, TArray<TSubclassOf<AArenaWeapon>>& Weapons) const` | `Weapons` already holds gauntlet + carbine, or the instagib rifle. |
| `bool AllowPickups() const` | false: the level builder places no pickups. |
| `bool AllowPickup(EArenaPickupType, TSubclassOf<AArenaWeapon>) const` | Spawn filter. Default: instagib keeps adrenaline only. **Call Super** to keep that behaviour. |
| `bool CanPickUp(AArenaCharacter*, AArenaPickup*) const` | Runtime filter, also respected by bots. |

Combat and scoring (server):

| Hook | Notes |
|------|-------|
| `float ModifyDamage(float Damage, AController* Instigator, AArenaCharacter* Victim) const` | Default: 0 when not playing, spawn protected or friendly fire (self damage allowed); instagib 1000. Usually `Super::ModifyDamage(...)`, then scale. |
| `bool AreOnSameTeam(const AController* A, const AController* B) const` | Used for damage, bot targeting and team-kill scoring. Default: same `PS->Team` in team games. Override for Invasion (all players vs monsters) and Mutant (everyone vs the mutant). |
| `bool ScoreKill(AController* Killer, AController* Victim, bool bHeadshot)` | Only called while playing. Return true when you handled it, which skips the default. To add to the default instead, call `GameMode->ScoreKillDefault(Killer, Victim, bHeadshot)` first and return true. |
| `bool KillsAddTeamScore() const` | Default scoring also changes team score per kill (TDM). |
| `void OnKilled(AController* Killer, AController* Victim, AArenaCharacter* VictimPawn, const FString& Weapon, bool bHeadshot)` | After scoring and kill messages, before the respawn decision. Decrement lives here. |
| `void CheckEndConditions()` | Called after kills and captures, and whenever you call `GameMode->CheckEndConditions()`. Default: goal score reached, or overtime with a leader. Call `GameMode->EndMatch(...)` or `EndRound(...)`. |
| `void OnTimeLimitReached()` | Called every second while `RemainingTime == 0` and not in overtime, until you change the phase, set `GS->bOvertime` or reset `GS->RemainingTime`. Default: tie gives overtime, otherwise the leader wins. |
| `bool OnPlayerUse(AController*)` | Use key (E, or gamepad face-left) or `AArenaBotController::RequestUse()`. Enter vehicles, use objectives. |

Bots (server): see section 6. `GetBotObjective`, `GetBotObjectives`, `UpdateBotObjective`, `GetBotEnemyWeight`.

HUD (clients too): see section 7. `DrawScoreInfo`, `DrawHUD`, `GetScoreboardExtra`, `GetScoreboardHeaderExtra`,
`GetDeadScreenMessage`, `ShouldDrawDeadScreen`.

Accessors: `GetGameMode()` (server only), `GetGameState()`, `GetLevelBuilder()`, `GetNavGraph()`,
`GetGoalScore()`, `GetWorld()`, `GetModeId()`, `PickBestStart(...)`, `static GetTeamOf(AController*)`.

---

## 4. Game mode API (`AArenaGameMode`, via `GetGameMode()`)

| Function | Purpose |
|----------|---------|
| `EndMatch(WinnerText, WinTeam, WinPlayer)` | End the match (WinTeam -1 / WinPlayer null for none). |
| `CheckEndConditions()` | Asks your rules. |
| `AddTeamScore(Team, Delta)`, `SetTeamScore(Team, Score)` | `GS->TeamScores[0/1]`, clamped >= 0. |
| `ScoreKillDefault(Killer, Victim, bHeadshot)` | Frags, suicides, team kills, sprees, multi kills, first blood. |
| `BroadcastCenterMessage(Text, Color, Duration, Sound)` | Everyone. Sounds are `/Game/Audio` names, e.g. `TEXT("S_Flag_Taken")`. |
| `BroadcastTeamMessage(Team, Text, Color, Duration, Sound)` | One team's humans. |
| `SendCenterMessage(Controller, Text, Color, Duration)` | One human. |
| `RestartPlayer(Controller)` | (Re)spawn now. Respects `CanRespawn`; ignored after the match ended. |
| `ScheduleRespawn(Controller, Delay)` / `CancelRespawn(Controller)` | Respawn timers. |
| `RemovePawn(Controller)` | Destroy a pawn without a kill. Carried objects drop. |
| `SetPlayerTeam(Controller, Team)` | Reassign team and recolour the pawn (Mutant, team swaps). |
| `GetTeamSide(Team)` | 0 = red base side, 1 = blue base side; flips while `GS->bSidesSwapped`. |
| `AddBot(Index, PreferredTeam = -1)` | Extra bot (Index picks the name). Returns `AArenaBotController*`. |
| `EndRound(WinTeam, Message, IntermissionSeconds = 6)` | Phase `RoundOver`: no damage, no respawns, message shown. Then: `Round++`, `ShouldSwapSides`, all pawns removed, `OnRoundReset`, everyone respawns, `RemainingTime = TimeLimit`, 3 s countdown, `OnRoundStart`. |
| `StartNextRound()` | Skip the intermission. |
| `SetModeStatusLines(TArray<FString>)` | Replicated HUD text lines (drawn by the default `DrawHUD`). |
| `IsPlaying()`, `IsTeamGame()`, `IsInstagib()`, `AreOnSameTeam(A, B)` | Queries. |
| `GetLevelBuilder()`, `GetNavGraph()`, `GetArenaGameState()`, `GetRules()`, `GetModeId()` | Access. |
| `GetBots()`, `GetNumBots()`, `GetBotSkill()`, `GetGoalScore()`, `GetTimeLimitMinutes()`, `GetLayoutId()`, `GetMatchOptions()`, `GetRound()` | Match settings. |

### Game state (`AArenaGameState`, replicated)

`ModeId`, `ModeName`, `bTeamGame`, `Phase` (`Countdown`, `Playing`, `RoundOver`, `Ended`), `Round`,
`bSidesSwapped`, `TeamScores[2]`, `GoalScore`, `TimeLimit` / `RemainingTime` (seconds), `bOvertime`, `bInstagib`,
`LayoutName`, `WinnerText` / `WinningTeam` / `WinningPlayer`, `ModeStatusLines`, `Flags[2]` (CTF only).
`GetSortedPlayers()`, `GetRank(PS)`, `GetGameTypeName()`, `GetRules()`.

You may set `GS->TimeLimit` / `GS->RemainingTime` in `InitGameState` or `OnRoundReset` for per-round timers.

### Player state (`AArenaPlayerState`, replicated)

`Team`, `Kills`, `Deaths`, `Suicides`, `Captures`, `Returns`, `Spree`, `BotSkill`, `GetScore()` / `AddScore()`,
plus fields whose meaning your mode defines:

- `Lives` (-1 = unused)
- `bEliminated`
- `ObjectiveScore` (goals, points held, objectives or nodes completed)
- `ModeFlags` (role bits, e.g. mutant / bottom feeder)

Everything is reset by `APlayerState::Reset`.

Anything else your HUD needs on clients must be replicated by **your own actors**, for example a control point
actor with a replicated owner team.

---

## 5. Map data: anchors and imported json

`AArenaLevelBuilder* Builder` is passed to `OnLevelBuilt` and is always available as `GetLevelBuilder()`.

### Anchors

Named floor points with a facing yaw, mapped by the level builder from the map's markers (list below).

| Name | Meaning |
|------|---------|
| `TeamBase0`, `TeamBase1` | flag / goal spot of the red (0) and blue (1) side |
| `Goal0`, `Goal1` | bombing run goal (raw, possibly floating location) |
| `Center` | neutral middle (ball spawn) |
| `ControlA`, `ControlB` | two control points (Double Domination) |
| `Objective0`..`ObjectiveN` | game objectives in file order (assault fallback) |
| `Node0`..`NodeN`, `Core0`, `Core1` | power nodes / cores (Onslaught) |
| `VehicleSpawn0`..`N` | vehicle factories |

```cpp
FTransform T;
if (Builder->GetAnchorPoint(TEXT("Center"), T)) { /* T.GetLocation() is on the floor, T.Rotator().Yaw faces */ }
TArray<FTransform> All = Builder->GetAnchorPoints(TEXT("TeamBase0"));    // empty if missing
TArray<FTransform> Chain = Builder->GetAnchorSeries(TEXT("Objective"));  // Objective0, 1, ... until a gap
TArray<FName> Names = Builder->GetAnchorNames();
Builder->AddAnchor(TEXT("MyThing0"), FloorLocation, Yaw);               // add your own
```

The anchor point is the floor. Lift your actor yourself: a standing character's centre is
`ArenaTuning::CapsuleHalfHeight` (88 cm) above it.

Where they come from:

- `TeamBase0/1` from the CTF flag bases. Otherwise from `raw_markers.bomb_deliveries` (by team), which also
  gives `Goal0/1` at the raw, possibly floating, delivery location.
- `Center` from the first `raw_markers.bomb_spawns`.
- `ControlA/B` from the first two `raw_markers.domination_points`.
- `ObjectiveN` from `raw_markers.game_objectives`, in file order, minus power nodes and cores.
- `NodeN` and `Core0/1` (by team) from `game_objectives` whose class chain contains `ONSPowerNode` /
  `ONSPowerCore`.
- `VehicleSpawnN` from `raw_markers.vehicle_factories`.

Anything else (monster spawns, power links, assault order, ...) you read from the json yourself and map in your own
code, e.g. with `Builder->AddAnchor` in `OnLevelBuilt`. When a needed anchor is missing on a map, fall
back to something sensible (player starts, `Builder->GetPlayerStarts()`, nav graph nodes) or return false from
`OnLevelBuilt`.

### Imported json

```cpp
TSharedPtr<FJsonObject> Json = Builder->GetImportedJson();   // the map's marker json
bool bImported = Builder->IsImportedMap();
TArray<FString> Lists = Builder->GetRawMarkerListNames();    // keys of "raw_markers"
for (const FArenaRawMarker& M : Builder->GetRawMarkers(TEXT("bomb_spawns")))
{
	// M.Class, M.ClassChain (most derived first), M.Location, M.Rotation, M.Properties, M.Json
	// M.IsA(TEXT("xBombSpawn")), M.GetTeam() (-1 if none), M.GetNumberProperty(Key, Out), M.GetStringProperty(Key, Out)
}
FVector V; FRotator R;
AArenaLevelBuilder::ReadJsonVector(Obj, TEXT("location"), V);   // [x, y, z]
AArenaLevelBuilder::ReadJsonRotator(Obj, TEXT("rotation"), R);  // [pitch, yaw, roll], {pitch,yaw,roll} or a yaw number
AArenaLevelBuilder::ParseRawMarker(Obj, Marker);
```

The importer is adding `"raw_markers"`: an object with one array per exporter list, for example `bomb_spawns`,
`bomb_deliveries`, `domination_points`, `game_objectives`, `vehicle_factories` and `power_links`. Each entry has
`class`, `class_chain`, `location`, `rotation` and `properties`. Locations use the same UE coordinates as the
other markers in the file. **Check a real file before relying on exact property names**: open
`<Map>.json` in `AArenaLevelBuilder::GetImportedDataDir()` (`<cache>/data` or `Content/ImportedData`), read only,
and never edit it. `GetTeam()` tries the properties `team`,
`DefenderTeamIndex`, `TeamNum` and `TeamIndex`.

Use `Builder->GetBuiltLayoutId()` (`"Imported:<Map>"`) and the layout info to decide whether a
map suits your mode.

```cpp
struct FArenaLayoutInfo
{
	FString Id;                // "Imported:<Map>"
	FString DisplayName;
	FString Description;
	FString Author;
	bool bSupportsDM;          // every layout
	bool bSupportsCTF;         // CTF maps with both flag bases
	int32 RecommendedPlayers;
	FString MapName;           // e.g. "BR-Anubis"
	FString GameTypePrefix;    // upper case: "DM", "CTF", "BR", "DOM", "AS", "ONS" ...
	TArray<FString> Features;
	bool HasFeature(const FString&) const;
};
```

`Features` holds:

- `"FlagBases"` (both CTF flag bases), and `"raw:<list>"` for every non-empty raw marker list, e.g.
  `"raw:bomb_spawns"`.

A typical filter:

```cpp
bool UArenaRules_BR::SupportsLayout(const FArenaLayoutInfo& L) const
{
	return L.HasFeature(TEXT("raw:bomb_spawns")) || L.GameTypePrefix == TEXT("BR");
}
```

Other builder helpers:

| Helper | Purpose |
|--------|---------|
| `TraceFloor(Location, MaxDrop, OutFloor)` | Floor below a point, against all static level geometry. |
| `AddSpawnPoint(FloorLocation, Yaw, Team)` | Extra `APlayerStart`, tagged Red / Blue / Any. |
| `GetPlayerStarts()` | The level's player starts. |
| `GetPickups()` | The level's pickups. |
| `GetJumpPads()` | The level's jump pads. |
| `GetArenaBounds()` | Bounds of the level geometry. |
| `GetKillZ()` | Kill Z of the level. |

**Geometry timing:** the map geometry (cooked level, or built at runtime before the builder reads the markers) is
in the world before `OnLevelBuilt`, and traces against it work there. Anchors are already floor points.

**Navigation:** add each objective spot through `GetNavHints`. The nav graph is built in `StartPlay` with traces
on the `ECC_WorldStatic` channel, so any of your actors that block that channel (for example the `BlockAll`
profile) count as walls. Give actors that must not block bot paths the actor tag `ArenaNavIgnoreTag()`
(`"ArenaNavIgnore"`, declared in `ArenaNavGraph.h`), or use overlap-only triggers like `AArenaPickup`.
`AArenaCarryable` is always ignored. Solid objectives, like a power core you walk around, should keep blocking.

---

## 6. Bots

Every goal evaluation (about once a second, and immediately when the goal becomes invalid or is reached),
`AArenaBotController` collects candidates:

- item pickups, scored `Desire * 2000 / (Dist + 1000)`, roughly 0.05 to 2;
- your objectives;
- hunting a recently seen enemy, scored 0.3 to 0.65.

It takes the highest score. The current goal gets a 1.2x bonus so bots don't flip between goals. Wandering
(0.05) is the fallback.

```cpp
struct FArenaBotObjective
{
	FVector Location;                 // where to go
	TWeakObjectPtr<AActor> Actor;     // optional (flag, ball, point, carrier)
	float Priority;                   // compared with the scores above; >= 0.9 = "important":
	                                  // the bot keeps running to it while fighting instead of strafing
	EArenaBotAction Action;           // Move, Attack, Defend, Collect, Deliver, Escort, Use (informational)
	bool bTrackActor = false;         // default UpdateBotObjective: Location follows Actor
	bool bCompleteOnReach = true;     // re-evaluate when reached (100 cm 2D, 150 cm Z); false = stay on it
	int32 Tag = 0;                    // yours, e.g. the kind of objective
};
```

| Hook | Notes |
|------|-------|
| `bool GetBotObjective(Bot, FArenaBotObjective& Out)` | The simple form: one objective, or false for none. |
| `void GetBotObjectives(Bot, TArray<FArenaBotObjective>& Out)` | Several candidates competing with each other and with pickups. Override this instead of the single form when you have alternatives (CTF does). |
| `bool UpdateBotObjective(Bot, FArenaBotObjective& O)` | Every bot tick while pursuing O. Update `O.Location` for moving targets; return false when it became pointless (flag returned, point captured). The bot then re-evaluates at once. **Call `Bot->RequestUse()` here** when the bot is close enough to use something. |
| `float GetBotEnemyWeight(Bot, AArenaCharacter* Candidate) const` | Target choice multiplies distance by this; < 1 prefers the target (CTF: 0.4 for our flag's carrier). |

Bot API for rules:

| Function | Purpose |
|----------|---------|
| `GetArenaPawn()` | The bot's pawn. |
| `GetTeam()` | The bot's team. |
| `IsDefender()` / `SetDefender(bool)` | About one bot in three defends. |
| `GetEnemy()` | Current target. |
| `IsEnemyVisible()` | Whether the target is in sight. |
| `GetLastSeenEnemyLocation()` | Where the target was last seen. |
| `GetCurrentObjective()` | Null unless the goal is one of yours. |
| `GetGoalLocation()` | Where the bot is heading. |
| `HasReachedGoal()` | Whether the goal was reached. |
| `SetGoal(const FArenaBotObjective&)` | Force a goal now; normal evaluation resumes after about 1 s. |
| `ForceGoalUpdate()` | Re-evaluate on the next tick. |
| `RequestUse()` | Press use. |
| `HasFlag()` | Carrying a CTF flag. |
| `IsCarryingObjective()` | Carrying a flag or any carryable. |
| `GetRandomStream()` | The bot's deterministic RNG. |
| `GetNavGraph()` | The nav graph. |
| `GetSkill()` | Skill, 0 to 7. |

Movement, aiming, weapon choice and dodging stay automatic. Bots move on the nav graph and shoot whatever
`AreOnSameTeam` says is an enemy. A bot carrying your object is told where to deliver it by an objective with
`Action = Deliver`, a high priority and `bCompleteOnReach = false`.

---

## 7. HUD

The HUD calls these hooks on the instance returned by `AArenaGameState::GetRules()`:

- on the server, the game mode's rules;
- on a network client, a local instance created from the replicated `ModeId`, where `GetGameMode()` is null.

Use only replicated data in HUD hooks: game state, player states and replicated actors.

| Hook | Notes |
|------|-------|
| `bool DrawScoreInfo(AArenaHUD* HUD, UCanvas*)` | Replace the top-right score panel. Return true when drawn. For team panels with extra rows, call `HUD->DrawTeamScorePanels(ExtraHeight, [&](int32 Team, float X, float Top, float W) { ... }, GoalText)`, as CTF does for its flag states. |
| `void DrawHUD(AArenaHUD* HUD, UCanvas*)` | Extra drawing after the normal HUD and under the scoreboard. Default: `GS->ModeStatusLines`, right-aligned under the score panel. Call Super to keep them. |
| `FString GetScoreboardExtra(const AArenaPlayerState*) const` | Short text after a name: `"2 lives"`, `"OUT"`, `"MUTANT"`. |
| `FString GetScoreboardHeaderExtra(const AArenaGameState*) const` | Added to the scoreboard info line: `"Round 2 / 4"`. |
| `FString GetDeadScreenMessage(const AArenaPlayerState*) const` | Default `"Press FIRE to respawn"`; return `"Out of lives"` and similar. An empty string hides the box. |
| `bool ShouldDrawDeadScreen(APlayerController*) const` | Default true. Return false to skip the red tint and message entirely, e.g. for eliminated players who are spectating (LMS). Only asked when the player has no live character and is not in a non-character pawn such as a vehicle. |

Drawing toolkit, all in pixels:

- `const FArenaDraw& D = HUD->GetDraw();`
- `D.S(RefPx)` converts 1080p reference units to pixels; `D.W()` and `D.H()` are the canvas size.
- `D.Text(Str, X, Y, Color, SizePx, EArenaAlign::Left/Center/Right)` and `D.TextMid(...)` (vertically centred).
- `D.Rect`, `D.Outline`, `D.Panel(X, Y, W, H, Accent)`, `D.Bar(X, Y, W, H, Fraction, Color)`, `D.Line`, `D.Arc`,
  `D.VerticalGradient`.
- `HUD->GetScoreInfoBottom()` is the bottom of the score panel; `HUD->Now()` is the world time for animations.
- Colours: `ArenaHudStyle::TextWhite()`, `TextDim()`, `HealthColor()`, `ShieldColor()`, `AccentBlue()`,
  `TeamTint(Team)`, `TeamTextColor(Team)`, `WithAlpha(C, A)`, `FormatTime(Seconds)`.
- Optional interface art: `ArenaArtHUD::DrawSpriteCentered(D, ArenaArtHUD::Sprites::Flag(), ...)`, `DrawNumber`,
  `DrawHudPanel` and friends (`ArenaArtHUD.h`) return false when the texture is missing; draw your procedural
  version in that case. `D.Panel` already uses the HUD box art when present.
- Teams: `ArenaTeam::Red` (0), `ArenaTeam::Blue` (1), `ArenaTeam::None` (-1), `ArenaTeam::Color(Team)`,
  `ArenaTeam::Name(Team)`.

The timer panel shows `WARMUP` / `ROUND n` during the countdown and `ROUND OVER` between rounds. The carrier of
an `AArenaCarryable` automatically sees its `GetCarrierHudText()`.

---

## 8. Carried objectives (`AArenaCarryable`)

Subclass `AArenaCarryable` for a ball or any object a player carries. Spawn it at its **home floor location**; the
actor location is home. It handles:

- touch pickup;
- attaching to the carrier's `BodyRoot`;
- dropping on the carrier's death or removal: `AArenaCharacter::Die` / `EndPlay` call `Drop(Killer)`;
- falling to the floor;
- returning when there is no floor or it falls below kill Z;
- auto return after `GetAutoReturnTime()` seconds on the ground (default 20, 0 = never);
- throwing with bouncing flight, catchable in the air.

| Function | Purpose |
|----------|---------|
| `PickUp(Character)` | Give it to a character. |
| `Drop(By)` | Leave the carrier and fall to the floor. |
| `Throw(Start, Velocity, By)` | Leave the carrier (if any) and fly. |
| `ReturnHome(By)` | Send it home and notify `OnReturned`. |
| `SendHome()` | Send it home silently. |
| `SetHomeLocation(FloorLocation)` | Move the home spot. |
| `SetTeam(Team)` | -1 = neutral. |
| `GetCarryState()` | `Home`, `Carried`, `Dropped` or `InFlight`. |
| `GetCarrier()`, `GetObjectLocation()`, `GetTimeOnGround()`, `IsHome()`, `IsCarried()`, `GetTeam()`, `GetHomeLocation()` | Queries. |

Override hooks:

- `CanBePickedUpBy(Character)`: default alive, not carrying anything, match playing, and not its thrower within
  `RepickDelay`.
- `OnTouched(Character)`: default picks up when allowed. Override for "touch own ball = return" or "touch the
  enemy's = take".
- `OnPickedUp`, `OnDropped(By)`, `OnReturned(By)`, `OnLanded`.
- `BuildModel()`: default is a glowing sphere in the team colour. Use `AddModelPart(Mesh, Loc, SizeCm, Material)`
  with `ArenaAssets::Sphere()` / `Cube()` / `Cylinder()` / `Cone()`, `ArenaAssets::MakeSurface(...)` and
  `MakeGlow(...)`.
- `GetCarrierHudText()`.

Tuning properties: `DisplayName`, `AutoReturnTime`, `CarryOffset`, `CarryRotation`, `CarryScale`, `RestHeight`,
`FlightGravityScale`, `Bounciness`, `FlightRadius`, `RepickDelay`.

`AArenaCharacter` exposes `CarriedObject` / `GetCarriedObject()`, `HasFlag()`, `IsCarryingObjective()` and
`Use()`. Report scores from the hooks to your rules, e.g.
`Cast<UArenaRules_BR>(GetWorld()->GetAuthGameMode<AArenaGameMode>()->GetRules())`.

The CTF flag (`AArenaFlag`) predates this class and keeps its own implementation. CTF scoring lives in
`UArenaRules_CTF`.

---

## 9. Assets, sounds, weapons

- Meshes and materials: see `ArenaAssets.h`. Every visual is built from basic shapes with
  `MakeSurface` / `MakeGlow`, as in `ArenaFlag.cpp` and `ArenaPickup.cpp`. Don't add `.uasset` content.
- Sounds, by name: `S_Countdown`, `S_Spree`, `S_Flag_Taken`, `S_Flag_Returned`, `S_Flag_Captured`,
  `S_Pickup_Power`, `S_Respawn`, `S_Explosion`, `S_ExplosionBig`, `S_JumpPad`, `S_Hit`, `S_Death`,
  `S_MenuSelect` ... (`Content/Audio`). Play them with `ArenaAssets::PlaySoundAt` / `PlaySound2D`.
- Weapons: `ArenaWeapons.h`.
  - `AWeap_Gauntlet`, `AWeap_Carbine`, `AWeap_PulseRifle`, `AWeap_ArcLinker`, `AWeap_RailRifle`,
    `AWeap_RocketLauncher`, `AWeap_ShardCannon`, `AWeap_RotaryGun`, `AWeap_GooLauncher`, `AWeap_InstaRifle`.
  - Give them with `Character->GiveWeapon(Class)`; ammo with `GiveAmmo`; health and shield with `GiveHealth`
    and `GiveShield`.
- Damage: `ArenaDamage.h` and `Character->ReceiveArenaDamage(...)`.
- Everything must be original: names, text and art. Don't copy names, text or content from other games.

---

## 10. Minimal example mode (King of the Hill)

This mode was compiled and played while the framework was written. It shows registration, an anchor,
nav hints, scoring in `Tick`, a bot objective and HUD status lines.

`Source/OpenUT2k4/ArenaRules_KOTH.h`

```cpp
#pragma once

#include "CoreMinimal.h"
#include "ArenaGameRules.h"
#include "ArenaRules_KOTH.generated.h"

/** King of the Hill (example): stand on the hill alone to score a point for your team every second. */
UCLASS()
class OPENUT2K4_API UArenaRules_KOTH : public UArenaGameRules
{
	GENERATED_BODY()

public:
	virtual bool IsTeamGame() const override { return true; }
	virtual int32 GetDefaultGoalScore() const override { return 60; }
	virtual FString GetGoalLabel() const override { return TEXT("Point Limit"); }
	virtual void GetGoalRange(int32& OutMin, int32& OutMax, int32& OutStep) const override { OutMin = 0; OutMax = 300; OutStep = 10; }
	virtual bool SupportsLayout(const FArenaLayoutInfo& Layout) const override;

	virtual bool OnLevelBuilt(AArenaLevelBuilder* Builder) override;
	virtual void GetNavHints(TArray<FVector>& OutPoints) const override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool GetBotObjective(AArenaBotController* Bot, FArenaBotObjective& OutGoal) override;

protected:
	FVector Hill = FVector::ZeroVector;
	float ScoreAccum = 0.f;
};
```

`Source/OpenUT2k4/ArenaRules_KOTH.cpp`

```cpp
#include "ArenaRules_KOTH.h"
#include "ArenaGameMode.h"
#include "ArenaGameState.h"
#include "ArenaCharacter.h"
#include "ArenaBotController.h"
#include "ArenaLevelBuilder.h"
#include "EngineUtils.h"

ARENA_REGISTER_RULES(UArenaRules_KOTH, "KOTH", "King of the Hill");

namespace ArenaKOTH
{
	constexpr float HillRadius = 300.f;
}

bool UArenaRules_KOTH::SupportsLayout(const FArenaLayoutInfo& Layout) const
{
	// The "Center" anchor comes from a bomb spawn.
	return Layout.HasFeature(TEXT("raw:bomb_spawns"));
}

bool UArenaRules_KOTH::OnLevelBuilt(AArenaLevelBuilder* Builder)
{
	FTransform Center;
	if (!Builder || !Builder->GetAnchorPoint(TEXT("Center"), Center))
	{
		return false; // falls back to TDM (GetFallbackModeId)
	}
	Hill = Center.GetLocation();
	return true;
}

void UArenaRules_KOTH::GetNavHints(TArray<FVector>& OutPoints) const
{
	OutPoints.Add(Hill + FVector(0.f, 0.f, 10.f));
}

void UArenaRules_KOTH::Tick(float DeltaSeconds)
{
	AArenaGameState* GS = GetGameState();
	if (!GS || !GS->IsPlaying())
	{
		return;
	}
	// Who stands on the hill?
	int32 Count[2] = { 0, 0 };
	for (TActorIterator<AArenaCharacter> It(GetWorld()); It; ++It)
	{
		const int32 Team = It->GetTeam();
		if (It->IsAlive() && (Team == 0 || Team == 1) && FVector::Dist(It->GetActorLocation(), Hill) < ArenaKOTH::HillRadius + 100.f)
		{
			Count[Team]++;
		}
	}
	const int32 Owner = (Count[0] > 0) != (Count[1] > 0) ? (Count[0] > 0 ? 0 : 1) : -1;

	ScoreAccum += DeltaSeconds;
	if (ScoreAccum >= 1.f)
	{
		ScoreAccum -= 1.f;
		if (Owner >= 0)
		{
			GameMode->AddTeamScore(Owner, 1);
			GameMode->CheckEndConditions(); // default: team score >= goal ends the match
		}
	}
	GameMode->SetModeStatusLines({ Owner >= 0 ? FString::Printf(TEXT("Hill: %s"), *ArenaTeam::Name(Owner)) : FString(TEXT("Hill: contested / free")) });
}

bool UArenaRules_KOTH::GetBotObjective(AArenaBotController* Bot, FArenaBotObjective& OutGoal)
{
	// Attackers go for the hill; defenders only when it is far away from the action anyway.
	OutGoal = FArenaBotObjective(Hill, nullptr, Bot->IsDefender() ? 0.6f : 1.2f, EArenaBotAction::Defend);
	return true;
}
```

That's the whole mode. It appears in the menu after the known modes, because KOTH is not in the fixed order list,
and it runs with `?Mode=KOTH`.

---

## 11. Recipes for the planned modes

These are suggestions; the design is yours.

**Bombing Run (BR)**

- Ball: `AArenaBR_Ball : AArenaCarryable`, spawned at `Center`.
- Goals at `TeamBase0/1` (imported maps: `Goal0/1`) as your own trigger actors.
- Throwing: on use or alt-fire, `Ball->Throw(...)`. Scoring: carry in or throw through.
- After a score: `SendHome()` and `AddTeamScore`.
- Bots: Collect the ball; carriers Deliver to the enemy goal; others Escort or Attack the carrier
  (`GetBotEnemyWeight`).

**Double Domination (DOM)**

- Control point actors at `ControlA/B`, touched to take; replicate the owning team.
- Holding both for 10 s scores: `AddTeamScore`, then reset the points.
- `DrawScoreInfo` / `DrawHUD` show the point owners. Bots: Collect / Defend points.

**Last Man Standing (LMS)**

- Set `PS->Lives` in `OnPlayerJoined` and decrement in `OnKilled`. `CanRespawn` checks lives; set `bEliminated`.
- `CheckEndConditions`: one player left. Optionally `ModifyDefaultInventory` gives all weapons and
  `AllowPickups` returns false.
- `GetDeadScreenMessage` / `GetScoreboardExtra` show lives.

**Mutant (MUT)**

- Not a team game. Track the mutant controller (`PS->ModeFlags`).
- `AreOnSameTeam`: true for two non-mutants. `ScoreKill`: only kills involving the mutant score.
- `OnPawnSpawned` / kills transfer mutant status: buff via `GiveHealth`, `GiveDoubleDamage`, invisibility combo.
- `GetBotEnemyWeight`: hunt the mutant.

**Invasion (INV)**

- Monsters: `AArenaINV_Monster : AArenaCharacter` with your own controller, or `AArenaBotController` without a
  player state, spawned at `MonsterSpawnN` (imported: player starts or nav nodes).
- `AreOnSameTeam`: players with players, monsters with monsters.
- Waves in `Tick`; `CanRespawn` false during a wave (respawn between waves with `RestartPlayer`).
- `GetPawnClass` is only for player pawns.
- Monster deaths also go through `GameMode->Killed`: return true from `ScoreKill` for them, and `CanRespawn`
  false for monster controllers.

**Assault (AS)**

- Objective actors along `GetAnchorSeries("Objective")`. Attackers are the team on side 0: the team `t` with
  `GameMode->GetTeamSide(t) == 0`.
- Completing the last objective, or `OnTimeLimitReached` (defenders win), calls
  `EndRound(Winner, Msg, 6)`. `ShouldSwapSides` returns true; `OnRoundReset` restores the objectives.
- Two rounds per match: decide in `CheckEndConditions` / `OnRoundEnd`, and call `EndMatch` after the last.
- Set `GS->TimeLimit` for the round length.

**Onslaught (ONS)**

- Cores at `TeamBase0/1` (imported: `Core0/1`), nodes at `NodeN`, links from `raw_markers.power_links` or a chain.
- Vehicles: your own pawn classes, entered through `OnPlayerUse`, spawned at `VehicleSpawnN`.
- Players spawn at owned nodes: return a node or your own `APlayerStart` from `ChoosePlayerStart`.
- If you need extra engine modules or plugins (e.g. Chaos vehicles), report it instead of editing `Build.cs`.

---

## 12. Building and testing

```sh
UE=~/UnrealEngine/Linux_Unreal_Engine_5.x
$UE/Engine/Build/BatchFiles/Linux/Build.sh OpenUT2k4Editor Linux Development -Project=<repo>/Client/OpenUT2k4.uproject -WaitMutex
$UE/Engine/Build/BatchFiles/Linux/Build.sh OpenUT2k4 Linux Shipping -Project=<repo>/Client/OpenUT2k4.uproject -WaitMutex
```

The second build checks the Shipping target. Add `-DisableAdaptiveUnity` once to check the full unity build.

Short game run with screenshots in `Saved/Screenshots/auto_<sec>.png` and the log in `Saved/Logs/X.log`. Keep
test windows short; someone may be using the desktop.

```sh
$UE/Engine/Binaries/Linux/UnrealEditor <repo>/Client/OpenUT2k4.uproject \
  "/Game/Maps/Arena?Mode=BR?Layout=Imported:BR-Colossus?Bots=7" -game -vulkan -windowed -ResX=1280 -ResY=720 \
  -RuntimeContent -ContentCache=<repo>/Client/ImportCache \
  -nosound -AutoSpectate -AutoScreenshot=10,20 -AutoQuit=30 -log=BR.log
# cooked map level (developer builds with cooked imports):
#   "/Game/Imported/Maps/BR-Anubis?Mode=BR?Bots=7" -NoRuntimeContent
# through the menu:
#   ... -AutoMenuStart=2 -AutoMenuMode=BR -AutoMenuLayout=Imported:BR-Colossus
```

- `-AutoSpectate` views through a different bot before each screenshot.
- Log with `UE_LOG(LogOpenUT2k4, Log, ...)`; include `OpenUT2k4.h`.
- `-ValidateAnchors` makes the level builder warn about anchors that have no floor or are blocked.
- Another agent may be compiling or running the editor at the same time. `-WaitMutex` serialises builds. If a
  build fails on a locked or missing module, retry.
