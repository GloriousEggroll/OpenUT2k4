# UnrealScript runtime (UScript VM)

OpenUT2k4 can run compiled UT2004 UnrealScript (UE2.5, package version 128 / licensee 29) from `.u` packages and
from script classes embedded in `.ut2` maps. The bytecode is executed by a small interpreter written in portable C++
(`Source/OpenUT2k4/UScript/`), bridged to the native C++ gameplay. Nothing from UT2004 is shipped: the stock
packages (Core, Engine, XGame, UnrealGame, ...) and the user's custom packages are read at runtime from the player's
UT2004 install and UserContent folder.

This document is the design and the status. Status and roadmap are at the end.

## Layers

```
 .u / .ut2 file ──► FConvPackage (Convert/ArenaConvPackage, shared with the content converter)
                        │ names / imports / exports / raw bytes
                        ▼
                  FUSLinker (one per package)      UScriptLoader.cpp
                        │ UClass/UStruct/UFunction/UState/UProperty/UEnum/UConst exports
                        │ bytecode deserialised token by token (disk → in-memory layout)
                        │ defaultproperties (tagged properties) → class default object
                        ▼
                  FUSClass / FUSFunction / FUSState / FUSProperty (metadata)   UScriptObject.h
                  FUSObject instances (property memory blocks)
                        ▼
                  Interpreter (token handlers, frames, states, latent code)    UScriptVM.cpp
                  Natives: Core (Object) and Engine (Actor ...)               UScriptNatives*.cpp
                        ▼
                  Bridge: VM actors ⇄ UE5 actors, game rules hooks          UScriptBridge.cpp
```

## Package format facts (verified)

All verified with a prototype disassembler on every stock `.u` package and the bundled maps: every function/state/
class deserialises to exactly `ScriptSize` in-memory bytes, every jump target lands on a token boundary, and every
class export's defaultproperties parse to the end of the export (one exception, `XInterface.HudBCaptureTheFlag`).

* `UObject`: optional state frame when `RF_HasStack` (0x02000000), then tagged properties (for non-class exports).
* `UField`: `SuperField` (index), `Next` (index).
* `UStruct`: `ScriptText` (index), `Children` (index), `FriendlyName` (name), `int32` (always 0 in stock packages,
  UE2.5 struct flags), `Line` (int32), `TextPos` (int32), `ScriptSize` (int32, *in-memory* size), then the bytecode.
* `UFunction`: `iNative` (uint16), `OperPrecedence` (uint8), `FunctionFlags` (uint32), `RepOffset` (uint16, if `FUNC_Net`).
* `UState`: `ProbeMask` (uint64), `IgnoreMask` (uint64), `LabelTableOffset` (uint16), `StateFlags` (uint32).
* `UClass`: `ClassFlags` (uint32), GUID (16), dependencies (`[class idx, int32 deep, uint32 crc]`), package imports
  (names), `ClassWithin` (index), `ClassConfigName` (name), `HideCategories` (names), defaultproperties (tagged).
* `UProperty`: `ArrayDim | ElementSize << 16` (int32), `PropertyFlags` (uint32), `Category` (name),
  `RepOffset` (uint16 if `CPF_Net`), then per type: Byte → enum, Object → class, Class → class + metaclass,
  Struct → struct, Array → inner property, Delegate → function. (Map/FixedArray properties never occur.)
* Bytecode on disk stores object references and names as compact indices; in memory they are 4 bytes each
  (32-bit UE2). Jump offsets (`EX_Jump`, `EX_JumpIfNot`, `EX_Case`, `EX_Iterator`, label table) are in-memory
  offsets, so the VM keeps the UE2 in-memory layout: object references are 4-byte indices into a global
  reference table (resolved lazily), names are 4-byte indices into a global name table.

### Tokens (UT2004)

`0x00` LocalVariable, `0x01` InstanceVariable, `0x02` DefaultVariable, `0x04` Return, `0x05` Switch, `0x06` Jump,
`0x07` JumpIfNot, `0x08` Stop, `0x09` Assert, `0x0A` Case, `0x0B` Nothing, `0x0C` LabelTable, `0x0D` GotoLabel,
`0x0E` EatString, `0x0F` Let, `0x10` DynArrayElement, `0x11` New, `0x12` ClassContext, `0x13` MetaCast, `0x14` LetBool,
`0x16` EndFunctionParms, `0x17` Self, `0x18` Skip, `0x19` Context, `0x1A` ArrayElement, `0x1B` VirtualFunction,
`0x1C` FinalFunction, `0x1D` IntConst, `0x1E` FloatConst, `0x1F` StringConst, `0x20` ObjectConst, `0x21` NameConst,
`0x22` RotationConst, `0x23` VectorConst, `0x24` ByteConst, `0x25` IntZero, `0x26` IntOne, `0x27` True, `0x28` False,
`0x29` NativeParm, `0x2A` NoObject, `0x2C` IntConstByte, `0x2D` BoolVariable, `0x2E` DynamicCast, `0x2F` Iterator,
`0x30` IteratorPop, `0x31` IteratorNext, `0x32` StructCmpEq, `0x33` StructCmpNe, `0x34` UnicodeStringConst,
`0x36` StructMember, `0x37` DynArrayLength, `0x38` GlobalFunction, `0x39` PrimitiveCast (+ cast byte 0x39..0x59),
`0x40` DynArrayInsert, `0x41` DynArrayRemove, `0x43` DelegateFunction (delegate property, name, parms),
`0x44` DelegateProperty (name), `0x45` LetDelegate, `0x47` EndOfScript, `0x60..0x6F` extended native
(index = (token-0x60)<<8 | next byte), `0x70..0xFF` native by index.

## Object model

UE2 objects are not UObjects: the VM has its own light object graph (no UE garbage collector, no reflection).

* `FUSObject`: class, name, outer, flags, property memory (`Data`), optional state frame, optional bridge link.
* Metadata objects (`FUSClass`, `FUSStruct`, `FUSFunction`, `FUSState`, `FUSProperty`, `FUSEnum`, `FUSConst`) are
  `FUSObject`s too, so `class'X'`, enum and function references behave like UE2 objects.
* Property memory layout is our own (64-bit): byte 1, int/float/bool 4 (one bool per uint32, mask 1), object/class
  pointers 8, name `FName` (UE2 names are case-insensitive like FName), string `FString`, dynamic array
  `FUSScriptArray {Data, Num, Max}`, delegate `{object, name}`, structs inline with natural alignment. Zeroed memory
  is a valid value for every type (empty string/array, None).
* Every class has a default object (`Class->Defaults`), built from the superclass defaults plus the class's
  defaultproperties; `default.X` and static calls run on it. Config values are read from the player's UT2004.ini
  (read only); localized strings from the `.int` files.
* Native C++ classes of UE2 are replaced by native *functions*: every `native` function (by index or by name) is
  bound at load time to a C++ implementation by `Class.FunctionName`; missing natives are reported per package
  (`-UScriptTest` lists them) and, when called, log once and return zero / None / empty (the script keeps running).
* Lifetime: instances live in a per-map `FUSWorld` and die with it. Destroyed actors are flagged and every reference
  to them is cleared at the end of the tick (UE2 `CleanupDestroyed` semantics); accessing them is "Accessed None".

## Execution model

The interpreter mirrors UE2's: `Step()` reads one token and dispatches through a 4096-entry handler table (tokens
and natives share it). Variable tokens produce an address (`GPropAddr`/`GProperty`) and copy the value to the
result buffer if one is given; `Let` evaluates the left side for its address and the right side straight into it.

* Function calls: script functions get a frame whose locals come from a bump-allocated VM stack; parameters are
  evaluated in the caller's context, out parameters are copied back. Natives read their own parameters from the
  bytecode stream (UE2 `P_GET_*` style), which also works for natives called from C++ (their script is a
  `NativeParm` list).
* Virtual calls look up the function by name in the current state (state inheritance chain), then the class chain
  (per-class/state hash maps). `Global.` calls skip states; `Super.` calls are final calls. Singular functions and
  `Enable`/`Disable` follow UE2; `ignores` of non-probe functions works (the compiler made them empty overrides),
  the probe-name bit masks of `ignores` are not decoded yet.
* `Context` on None logs "Accessed None" (rate limited) and zeroes the result. UE2 stores the result *size* in the
  token; our layout differs, so the VM infers the expression type from the bytecode (the same walker sizes `Switch`).
* States: `GotoState`, labels, `BeginState`/`EndState`, state code runs every tick until a latent function blocks it.
  Latent functions are coroutines: `Sleep` sets the frame's latent action and is polled every tick; `FinishAnim`,
  `FinishInterpolation`, `MoveTo`/`MoveToward` complete on the next tick for now.
* Iterators (`ForEach AllActors / DynamicActors / RadiusActors / VisibleActors / ...`) are natives looping over the
  body like UE2's `PRE_ITERATOR/POST_ITERATOR`.
* Timers (`SetTimer`), `Tick` (only for classes that define it), dynamic arrays, strings, structs, delegates,
  `new`, `Spawn`, casts, defaults, enums and consts are supported. Replication blocks are ignored (single player
  and listen server only for now: script runs on the server).
* Errors never crash the game: runaway loops (10M tokens per call), recursion (250 frames), bad bytecode or a
  missing native abort the current script call with a log line; the game keeps running.

## Bridge to the game

A VM actor ("script actor") can be paired with a UE5 actor:

* `FUSWorld` creates the UE2 world skeleton: `LevelInfo` (Level), a `GameInfo` object of the matching stock class
  (`xDeathMatch`, `xTeamGame`, ...; its properties mirror `AArenaGameMode`), `GameReplicationInfo`, the default
  `PhysicsVolume`, and proxies for controllers (`xPlayer` / `xBot`), pawns (`xPawn`) and player replication infos
  (pickups and weapons get short-lived proxies while mutators decide on them), linked like UE2 (`Level.ControllerList`, `Controller.Pawn`, `PlayerReplicationInfo`, ...).
* Sync: before script runs for a proxy, its UE2 properties are refreshed from the UE5 actor (location, rotation,
  velocity, health, score ...); after script ran, changed values are pushed back (health, speeds, jump, gravity,
  head scale ...). Natives with side effects (`Spawn`, `Destroy`, `SetLocation`, `PlaySound`, `GiveTo` ...) call
  the game directly.
* Mutators: `?Mutator=Pkg.Class,Pkg.Class` (UT2004 syntax) spawns script mutators linked to `Level.Game.BaseMutator`.
  The game mode calls the chain at the UT2004 points: `CheckRelevance/CheckReplacement/AlwaysKeep/IsRelevant`
  for spawned pickups/weapons/pawns, `ModifyPlayer` after spawn, `GetInventoryClassOverride` and
  `GetDefaultWeapon` for starting inventory, `ScoreKill` (mutators and game rules), `NetDamage` (game rules); the
  physics volume passes `CheckRelevance` after the level is built. Not called yet: `PreventDeath`, `Mutate`,
  `ModifyLogin`, `PlayerChangedClass`. Script class names are mapped to our C++ classes (weapons, pickups) by tables.
* Map scripts: actors of script classes defined by the map or a custom package are instantiated in the VM with their
  saved properties and get an `AArenaScriptActor` body; events (`Trigger`, `UnTrigger`, `Touch`, `UnTouch`,
  `TakeDamage`, `TriggerEvent`, `Timer`, state code) flow both ways through the world module's UE2 event system
  (`UArenaEventSubsystem`), so script triggers drive native movers and native triggers reach script actors.
  Kinds that are not supported yet keep the native stand-in for their stock base class.

## Canvas bridge

`UScriptCanvas.h` / `UScriptNativesCanvas.cpp` implement UE2's Canvas natives with UE2's layout rules (the cursor,
`DrawText` word wrapping at `ClipX`, `DrawTextClipped` / `DrawTileClipped` clipping with UV adjustment, the
`DrawTileStretched` corners at 1:1 texels, `ColorModulate` and `bForceAlpha`), the HUD widget natives
(`DrawSpriteWidget` / `DrawNumericWidget` with `HudCanvasScale`, `HudScale`, pivots, scale modes, `Tints[TeamIndex]`;
`ResScaleX = ResScaleY = Min(SizeX / 640, SizeY / 480)` as in OldUnreal's 3374), HUD lines and material sizes on top of
a backend (`IUSCanvasBackend`: tiles, text, text size, material size, projection, lines) set while script draws. The
canvas state lives in the script `Engine.Canvas` object. Without a backend the natives draw nothing and return zero
sizes. The only backend is the UE2 client's (`Net/UE2NetScriptHUD`: our UCanvas, the content cache's textures and
UE2 bitmap fonts).

Runtime support added for it: placeholders of script classes with properties (`Engine.Texture` ...) get property memory
the first time script reads one of their variables (`FUSRuntime::MaterializePlaceholder`, values from the game's
`PlaceholderInit`: `USize` / `VSize`); `DynamicLoadObject` / `FindObject` return the placeholder of a content object
the game has (`ContentObjectExists`), also when a group of the same name exists (a font's page textures); imports of
C++-only classes (`Engine.Font`, `Engine.Viewport`) resolve to intrinsic classes; `FUSWorld::AsGameWorld` /
`GetLocalPlayerController` let natives work in worlds that are not game worlds (the UE2 client mirror);
`USVM::GStatAborts` counts aborted script calls.

## Public API (`UScript/ArenaUScript.h`)

* `ArenaUScript::CanRun(PackageOrClass)` / `GetUnsupportedReason(PackageOrClass)`: whether a script class
  (`Pkg.Class`) or every class of a package can run: the class loads (all superclasses resolvable), its kind is
  supported (`USGetClassSupport`), and no unimplemented native is reachable from its gameplay code. Used by the user
  content browser and the mutator menu (`ArenaGUI2K4::GetMutators`).
  * Reachability (`FUSReach` in `ArenaUScript.cpp`): static analysis of the bytecode (`USVM::ForEachCall`, the
    `InferExpr` walker reporting every call and object constant). Roots are the functions the game actually calls
    (`GEntryPoints`: spawn/destroy events, `Timer`, `Tick`, `BeginState`/`EndState`, `Trigger`/`Touch`/`TakeDamage`,
    the mutator chain `AddMutator`/`CheckRelevance`/`AlwaysKeep`/`IsRelevant`/`CheckReplacement`/`ModifyPlayer`/
    `GetInventoryClassOverride`/`GetDefaultWeapon`/`ScoreKill`, game rules `NetDamage`/`ScoreKill`) plus all state
    code. Calls are followed transitively: final/super calls directly, virtual calls by name on the static class of
    the call's context (and on the analysed class when it is a subclass of it, every state included). Classes named
    by `class'X'` constants or class-typed defaults that are mutators, game rules or of the same package become roots
    too (e.g. `MutVampire` -> `VampireGameRules`).
  * Config and menu code (`FillPlayInfo`, `MutatorFillPlayInfo`, `GetDescriptionText`, `GetServerDetails`, cache and
    `.int` queries such as `GetAllInt`/`GetNextIntDesc` ...) is never called by the game, so its natives do not count.
  * The reason names the missing natives and where they are reached from, e.g. `unimplemented natives reached from
    gameplay code: Controller.FindPathToward (from <Pkg.Class.Function>), ...`.
  * A missing native that is reached anyway at run time (code the analysis cannot see, e.g. through a delegate bound
    at run time) is still safe: it logs once (`native function X.Y is not implemented`) and returns zero.
* `ArenaUScript::FindReachableMissingNatives(Class, EntryPoints)`: the same analysis from other entry points (e.g. a
  HUD's `PostRender`); `-UScriptReachReport=Pkg.Class,... [-UScriptReachEntries=...] [-UScriptPackageFiles=a.u;b.u]`
  logs it ("UScriptReach: ...").
* `-UScriptCanRunReport` logs `CanRun()` for every non-abstract mutator class of the cache's script packages
  (`UScriptCanRun: Pkg.Class: ok | reason`, then `UScriptCanRun: N of M mutator classes can run`).
* `ArenaUScript::Runtime()` — the process-wide runtime (package/class cache).
* Test hooks: `-UScriptTest` runs the VM self tests at startup and logs `UScriptTest: N passed, M failed`
  (`-UScriptTestQuit` exits afterwards); `-UScriptDisasm=Pkg.Class.Function` logs a disassembly.

## Files

| File | Contents |
| --- | --- |
| `UScriptCore.h` | object model (`FUSObject`, `FUSClass`, `FUSFunction`, `FUSState`, `FUSProperty`), frames, tokens, native macros (`USP_GET_*`) |
| `UScriptRuntime.h`, `UScriptLoader.cpp` | `FUSRuntime` (package/class cache, name and reference tables, ini/int reading), `FUSLinker` (exports, imports, bytecode deserialisation, defaultproperties, config, localization, native binding, C++ overrides of script functions) |
| `UScriptProperty.cpp` | value copy/destroy/compare/text import-export, `FUSParms` (calling script from C++) |
| `UScriptVM.cpp` | token handlers, function calls, states, labels, latent actions, iterators, expression type inference, disassembler |
| `UScriptNativesCore.cpp` | the 190 natives of `Core.Object` |
| `UScriptNativesEngine.cpp` | Engine natives (spawn/destroy, timers, latent, traces, iterators, controller list, sounds, colours ...) |
| `UScriptWorld.*` | instances, pending kill and UE2 `CleanupDestroyed` |
| `UScriptBridge.*` | `FUSGameWorld` (LevelInfo, shadow GameInfo, proxies, sync), game hooks, map script actors, script functions replaced by the game (`TriggerEvent`, `ClientMessage`, `Broadcast` ...) |
| `UScriptCanvas.h`, `UScriptNativesCanvas.cpp` | Canvas bridge: UE2's Canvas natives, `HudBase.DrawSpriteWidget` / `DrawNumericWidget`, `HUD.DrawCanvasLine`, `Material.MaterialUSize` / `VSize`, `CacheManager.GetCrosshairList`, `GUIFont.GetFont`; the drawing backend interface (`IUSCanvasBackend`) |
| `ArenaScriptActor.*` | game-side body of a map script actor (event system, touch cylinder, shootable capsule, static mesh) |
| `ArenaUScript.*` | public API (`CanRun`, `GetUnsupportedReason`, `ArenaUScriptHooks`), class support policy, `-UScriptTest` |
| `UScriptTests.cpp` | self tests |

Game hooks (one line each, no-ops without script): `AArenaGameMode::InitGame` (`InitGame`, `LevelBuilt`),
`GetDefaultInventory`, `RestartPlayer` (`PawnSpawned`), `ModifyDamage`, `Killed`; `AArenaLevelBuilder::AddPickup`
(`FilterPickup`). Compile switch: `OPENUT2K4_USCRIPT_ENABLED` in `UScriptCore.h`.

## Status (what runs, what is verified)

**M0/M1 — package reader and VM core: done.**
* All 2061 classes of the stock script packages (Core, Engine, UnrealGame, XGame, XWeapons, XPickups, XEffects,
  Onslaught, UT2k4Assault, XInterface, GUI2K4, Gameplay, Vehicles) load in ~100 ms with no failure; bytecode,
  defaults, config (read only from the player's UT2004.ini/User.ini) and `.int` localization.
* Every token of the UT2004 set is implemented; all Core.Object natives (190) are implemented; 124 Engine natives
  that matter for gameplay script are still missing (Karma physics, skeletal animation queries, AI path finding
  `FindPathTo*`, music streams, `TraceActors`, `GetMapName/GetNextSkin`, savegames) — they log once and return zero.
* `-UScriptTest`: 30 checks, all passing: struct layouts, natives called from C++, stock script functions with known
  results (`Object.GetItemName/ReplaceText/RandRange`, `GameInfo.ParseOption/HasOption/GrabOption/GetKeyValue/
  ParseKillMessage`, `StreamBase.RevInStr/ParseDirectories`, `LocalMessage.GetLifeTime`, switch on strings with
  localized text (`MutInstaGib.GetDescriptionText`), class context + dynamic arrays (`UT2K4LadderInfo.LengthOfLadder`,
  `TeamRedConfigured.AddCharacter/FindCharIndex/RemoveCharacter`), `UT2K4GameProfile.MoneyToString`), localized and
  default properties, an actor chain (two `Gameplay.Counter`s linked by Tag/Event through `TriggerEvent`), bulk load,
  and speed (~0.5 µs per `GetItemName` call, 80+ M tokens/s).

**M2 — engine bridge: mutators and map scripts run.**
* Mutators run unmodified through the stock `GameInfo.AddMutator` on a shadow `xDeathMatch` (or the mode's game
  class) with `UnrealGame.DMMutator` as base mutator, exactly like UT2004. Verified in matches (logs):
  `MutLowGrav` (world gravity -300 UE2 → our gravity scaled), `MutInstaGib` (all pickups removed, default
  inventory becomes the instagib rifle, kills with it), `MutQuadJump` (JumpMaxCount 4), `MutRegen` (Timer heals pawns
  through `Level.ControllerList`), `MutVampire` (installs `VampireGameRules` and destroys itself, NetDamage heals the
  instigator: 84 → 88 for 8 damage), `MutBigHead` (game rules + head scale values; the model is not scaled yet),
  `MutNoAdrenaline` (27 adrenaline pills removed on DM-Rankin), `MutUseSniper`, `MutFastWeapSwitch`,
  `MutSlomoDeath`, `MutUDamageReward`, `MutBerserk`, `MutGameSpeed` and the custom `AngelItemPhysics.MutItemPhysics`
  load and run without script errors.
* Map script actors (classes defined by the map or a custom package): instantiated with their saved properties,
  begin-play events, timers, state code and latent `Sleep`, Trigger/UnTrigger/Touch/UnTouch/TakeDamage from the game
  and `TriggerEvent` into the native UE2 event system. Verified on DM-CBP2-Reconstruct (7 `ShockHitTrigger`s fire the
  native movers' events when hit by the shock beam, the native chain triggers the map's `RepeatCounter` back) and
  DM-UCMP4-Mania (custom `NormalHP`/`NiceMiniHP`/`ShieldShard` pickups: +25 / +5 health, +10 shield, hide, sleep and
  respawn after 30 s, all in the map's script).
* Performance (MangoHud, DM-Rankin, 5 bots, 1280x720): 107.0 fps / 1% low 76.4 without script, 105.4 / 77.5 with six
  mutators — inside run-to-run noise; script time 0.0175 ms per frame.

**CanRun of the stock mutators** (`-UScriptCanRunReport`, full DevCache): all 40 non-abstract mutator classes pass —
`MutInstaGib`, `InstagibMutator`, `MutZoomInstagib`, `MutLowGrav`, `MutQuadJump`, `MutRegen`, `MutVampire`, `MutBigHead`,
`MutNoAdrenaline`, `MutSlomoDeath`, `MutSpeciesStats`, `MutUDamageReward`, `MutFastWeapSwitch`, `MutBerserk`,
`MutGameSpeed`, `MutMovementModifier`, `DMMutator`, `MutNoSuperWeapon`, `MutArena`, `MutUTClassic`, `MutUseSniper`,
`MutUseLightning`, `MutLastManStanding`, `MutCrateCombo`, `MutOnslaughtWeapons`, `MutLightweightVehicles`,
`MutBigWheels`, `MutWheeledVehicleStunts`, `MutVehiclePickups`, `MutBonusVehicles`, `MutVehicleArena`,
`InvasionMutator`, `ASMutator`, `utvMutator`, `ONSDefaultMut` and the bundled custom ones (`ONSDefaultMut` passes since
`Actor.StopFeedbackEffect` / `PlayFeedbackEffect` became no-op natives and `PlayerController.ResetInput` releases the
player controller's pressed keys). Before reachability, the check
counted every native declared anywhere in the class chain and rejected all of them. Passing `CanRun()` means the
code the game calls has every native it needs; what the mutator does still depends on the bridge (e.g. vehicle
mutators have nothing to act on yet). Re-verified in DM-Rankin bot matches: `?Mutator=XGame.MutInstaGib` (default inventory
`[Weap_InstaRifle]`, `pickups=0` instead of 61) and `?Mutator=UnrealGame.MutLowGrav` (`world gravity -309 (UE2 -300)`).

**M3 — custom packages:** custom mutators (from `.u` files in the install or UserContent) and map-embedded classes run
when their kind is supported (see the policy in `USGetClassSupport`); the user content browser asks `CanRun()` for
every custom class, so maps whose script classes all run are no longer marked unsupported.

## Not supported yet (roadmap, roughly in order of value / effort)

1. **Script weapons, inventory, projectiles** (most "weapon mods"): need a script-driven `AArenaWeapon` (firing modes,
   ammo, `Instigator.Weapon` chain, `WeaponFire` classes, hit scan/projectile spawning with our effects). Large.
2. **Script pawns, monsters, vehicles, AI controllers** (Invasion monster packs, vehicle mods): need physics
   (`PHYS_Walking/Falling/Karma`), animation natives and the path-finding natives (`FindPathToward` ...). Large.
3. **HUDs, scoreboards, GUI/menus**: the Canvas bridge exists (`UScriptCanvas.h`, `UScriptNativesCanvas.cpp`, see
   "Canvas bridge" above) and runs UT2004 servers' mod HUDs and scoreboards (`Net/UE2NetScriptHUD`). Script HUDs in our
   own matches and the GUI (`GUIController`, menus) are not done; full GUI is large.
4. **Game types written in script** (a whole `GameInfo` subclass): would replace our C++ game mode; needs 1-3.
5. Smaller items: `GameRules.PreventDeath` (needs a hook in `AArenaCharacter::Die`), `Mutate` console command,
   `bAdrenalineEnabled`, head scaling on the character model, `ignores`/probe masks (UE2 probe name indices),
   script movers/emitters/pickup bases (they currently fall back to the stock native version), static meshes that
   only exist inside a map package (the converter does not export ones referenced only by script class defaults),
   replication (script runs on the server only; fine for single player / listen server), savegame/data objects.
6. Per-mutator config pages (`FillPlayInfo`); the mutator menu itself exists and selects what `CanRun()` accepts.
