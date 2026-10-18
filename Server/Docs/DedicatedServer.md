# Dedicated server

`OpenUT2k4Server` is the headless dedicated server of OpenUT2k4: the game's own code hosting one match after another
with OpenUT2k4's UE2 network server (`Source/OpenUT2k4/Net/UE2Server*`, see `Docs/UE2Net.md` "Hosting: the UE2 server"),
so real UT2004 clients (retail 3369, OldUnreal 3374) and OpenUT2k4 players join it. It has no window, no GPU use, no
audio and no local player. It is configured from UT2004-style ini files and a UCC-style command line, runs as a
service on Linux (systemd), Windows (a native service) and macOS (launchd), and is meant as the base for an improved,
open source UT2004 dedicated server.

Like the game, the package contains **no UT2004 data**. The server needs the owner's UT2004 install (for the clients'
package list) and the content cache converted from it once (maps, collision, game data).

## Design

| Part | Files | What it does |
|---|---|---|
| Server mode | `Source/OpenUT2k4/Server/ArenaDedicatedServer.*` | Chosen when the executable is named `OpenUT2k4Server*` (the server target), with `-OpenUT2k4Server` (game or editor binary), or always in a Type=Server build (`UE_SERVER`). At module start: reads the configuration, puts the content paths on the command line, turns `GIsClient` off so `UGameEngine::Init` creates no viewport, window or local player, and installs `UArenaServerGameInstance`. Then a core ticker runs the admin commands, `MinPlayers` bots, the frame rate cap and a watchdog. |
| Game instance | `Server/ArenaServerGameInstance.*` | Instead of the main menu it opens the first match (`/Game/Maps/Arena?Mode=..?Layout=Imported:<Map>?...?UE2Host=1?UE2Dedicated=1...`). `GIsClient` goes back on before the map loads, so `LoadMap` starts no UE5 net driver; the UE2 server is the only network endpoint. |
| Configuration | `Server/ArenaServerConfig.*` | A UT2004 ini reader (sections, repeated keys, `Key[n]=`), the layers below, the UCC URL, the stock map lists, the match URL. |
| Windows service | `Server/ArenaServerService.cpp` | `-Service=<name>`: the service control dispatcher on a helper thread, `ServiceMain`, STOP / SHUTDOWN / PRESHUTDOWN → a normal engine exit, `SERVICE_STOPPED` with the exit code at module shutdown. |
| Targets | `Source/OpenUT2k4Server.Target.cs` | One target, two shapes: a **Game** target on an installed (binary) engine, a **Server** target on a source engine (see "Building"). |
| Scripts | `Server/Package/` | Default ini, launchers (`openut2k4-server`, `.bat`, `ucc`), systemd / launchd / Windows service files and installers. Copied into the packages by `Server/build-*.sh` / `.bat`. |

What the server reuses from the game: the match itself (`AArenaGameMode`, bots, rules), the runtime map builder, the
UE2 server and its master server uplink, query port and LAN beacon, map rotation (`?MapList=`, `UE2ServerHooks::TravelToMap`
/ `TravelToNextMap`: clients follow with `ClientTravel`), kick / ban (`ArenaHost::KickUE2Player`, the ban store hook
`ArenaHost::SetUE2BanStore`), server chat (`ArenaHost::UE2ServerSay`), MOTD / admin name in the game replication info.

Headless and cooked: a packaged build with `-nullrhi` must not initialise static mesh render resources
(`FStaticMeshRenderData::InitResources` asserts "RenderData should not initialize resources in headless cooked runs").
`ArenaRuntimeMeshes::BuildStaticMeshHeadlessSafe` builds the CPU side of runtime meshes without it there (all collision
needs); with a renderer, or in the editor binary, it is the engine's `BuildFromMeshDescriptions` as before. The game's
own Host Game > DEDICATED (a `-nullrhi` instance of the packaged game) runs the same code path and should now work too
(not tested from a package).

## Quick start (Linux package)

```sh
Server/build-linux.sh                    # -> Client/Dist/Server-Linux/
cd Client/Dist/Server-Linux
./openut2k4-server -ConvertOnly -UT2004Dir=/path/to/UT2004          # once: the content cache (as the game does)
./openut2k4-server                                                   # DM, the stock DM map list, port 7777
./openut2k4-server "CTF-Face3?game=XGame.xCTFGame?MinPlayers=8" -port=7787
./ucc server "DM-Rankin?game=XGame.xDeathMatch?MaxPlayers=8" ini=MyServer.ini log=server.log    # UT2004 style
./openut2k4-server -ServerConfig=MyServer.ini -ServerProfile=/path/to/profile
```

Type `help` on the console for the admin commands. A server that was started by a player who has run the game
already finds the cache and the install the way the game does (`ArenaContent::GetCacheDir`, `FindUT2004Install`).

## Command line

```
OpenUT2k4Server [server] [<Map>?game=<GameClass>?<option>=<value>...] [ini=<file>] [log=<file>]
    [-ServerConfig=<ini>] [-ServerProfile=<dir>] [-ServerStateDir=<dir>] [-port=<n>] [-multihome=<ip>] [-lanplay]
    [-NoUplink] [-ServerName=<name>] [-AdminConsolePort=<n>] [-ContentCache=<dir>] [-UT2004Dir=<dir>] [-UserContent=<dir>]
    [-Service[=<name>]] [-NoServerConsole] [-ConvertOnly -UT2004Dir=<install>]
    engine switches: -nullrhi -nosound (the launchers add them), -userdir=<dir> (Saved/ goes there), -abslog=<file>
```

- `server` (UCC's command word) is accepted and ignored. The URL's map and options follow UT2004:
  `game=` (short or full class name), `MaxPlayers`, `MaxSpectators`, `MinPlayers` (also `NumBots`), `GoalScore`,
  `TimeLimit`, `GameDifficulty`, `Mutator=A,B`, `GamePassword`, `AdminPassword`, `AdminName`, `ServerName`, `Port`,
  `LANServer`, `MapList=A,B,C`. Any other option goes to the game as it is (`?Translocator=1`, `?WeaponStay=1`).
- `ini=` / `-ServerConfig=` is the server ini; `log=` names the log file (the engine's `LOG=`).
- `-multihome=<ip>` binds the game port to one address (`-UE2ServerBind`), `-lanplay` makes it a LAN server.
- `-OpenUT2k4Server` makes the game or editor binary a dedicated server (development: `UnrealEditor OpenUT2k4.uproject
  -game -OpenUT2k4Server -nullrhi -nosound -RuntimeContent -ContentCache=<cache> ...`).

## Configuration

Layers, later ones win:

1. built-in defaults: the stock UT2004 dedicated server's defaults (port 7777, 16 players, 2 spectators, time limit
   20, the stock goal scores), except that the **master server uplink is off**;
2. the shipped default ini, `<package>/Server/OpenUT2k4Server.ini` (`Server/Package/OpenUT2k4Server.ini`): our own
   file with the stock values, no UT2004 file is copied;
3. the server ini (`-ServerConfig=` / `ini=`): a copy of the default ini, or a UT2004.ini (same section names);
4. a profile folder (`-ServerProfile=<dir>`): its `Server.ini`, or a `UT2004.ini` when it has no `Server.ini`. Any path
   works (a private profile under `Saved/PrivateServer/<name>/`, which is git-ignored and never packaged);
5. the command line.

| Section | Keys |
|---|---|
| `[OpenUT2k4.Server]` | `GameType` (stock class: `XGame.xDeathMatch` `xTeamGame` `xCTFGame` `xBombingRun` `xDoubleDom` `xVehicleCTFGame`, `BonusPack.xLastManStandingGame` `xMutantGame`, `Onslaught.ONSOnslaughtGame`, `UT2k4Assault.ASGameInfo`, `SkaarjPack.Invasion`; or our ids `DM` `CTF` ...), `Map` (first map), `Maps` (rotation, one line each), `Mutators` (comma list), `MinPlayers`, `BotsWhenEmpty`, `BotSkill` (0..7), `ExtraOptions`, `TickRate`, `LANServer`, `UseServerPackages` |
| `[URL]` | `Port` |
| `[IpDrv.TcpNetDriver]` | `NetServerMaxTickRate` (server frames per second, default 30) |
| `[IpDrv.MasterServerUplink]` | `DoUplink` (register with the OldUnreal master servers; default False) |
| `[Engine.GameReplicationInfo]` | `ServerName`, `ShortName`, `AdminName`, `AdminEmail`, `MessageOfTheDay` or `MOTDLine1..4` (sent as the GRI's `MessageOfTheDay`) |
| `[Engine.AccessControl]` | `AdminPassword`, `GamePassword`, `BannedIDs=<id> <name>` (repeated) |
| `[Engine.GameInfo]`, `[UnrealGame.DeathMatch]`, `[UnrealGame.TeamGame]`, `[<game class>]` | `MaxPlayers`, `MaxSpectators`, `TimeLimit`, `GoalScore`, `MinPlayers`, `GameDifficulty` (later sections win: the game class's own section last) |
| `[Engine.GameEngine]` | `ServerPackages` (informational unless `UseServerPackages=True`, then they replace the install's list in the clients' package list), `ServerActors` (the stock uplink, beacon, query and web server actors have native equivalents; others are reported as unsupported) |
| `[OpenUT2k4.AdminConsole]` | `bEnabled`, `ListenAddress` (default 127.0.0.1), `Port` (default 7780) |
| `[OpenUT2k4.Paths]` | `ContentCache`, `UT2004Dir`, `UserContent` (relative to the ini's folder; empty: the game's defaults) |

- **Map rotation**: `Maps=` lines, else the UT2004 install's own stock map list for the game type (`[XInterface.MapListDeathMatch]`
  ... in its `System/UT2004.ini`, else `Default.ini`, read at run time), else every converted map of the game type's
  prefix. Maps missing from the content cache are left out (logged). The match ends, the next map loads, the clients
  follow.
- **Bots** follow UT2004's `MinPlayers`: while players are connected, bots fill the match up to `MinPlayers` (one per
  second, added or removed); an empty server has none unless `BotsWhenEmpty=True`.
- **Game types**: the stock classes map to our modes (DM, TDM, CTF, BR, DOM, LMS, MUT, ONS, AS, INV). Vehicle CTF
  (`xVehicleCTFGame`) is played with the CTF rules; the UE2 server does not replicate vehicles, Assault objectives or
  Invasion monsters yet (`Docs/UE2Net.md` "Gaps").
- **Tick rate**: `t.MaxFPS` is kept at `TickRate` (the game user settings would otherwise reset it to unlimited).
- **State** (`-ServerStateDir`, default `<Saved>/Server`): `Bans.ini`, the bans made with the admin commands
  (`[Engine.AccessControl] BannedIDs=`), merged at start with the ini's `BannedIDs`. Player ids are the stock server's
  CD key hash ids (`Docs/UE2Net.md` "AUTH"); a banned id gets `FAILCODE LOCALBAN`.
- **Exit codes**: 0 normal (143 / 130 after SIGTERM / SIGINT), 2 bad configuration, 3 no content cache or UT2004 install,
  4 the match could not be hosted (3 failed matches in a row, or the UE2 server did not start: port in use). The
  service files do not restart on 2 and 3.

## Admin commands

On stdin (an interactive server) and on the TCP admin console (`[OpenUT2k4.AdminConsole] bEnabled=True`, needs
`AdminPassword`; line based, `nc 127.0.0.1 7780` or telnet; three wrong passwords close the connection). UT2004's
`admin <command>` spelling works too.

| Command | |
|---|---|
| `status` / `players` | server, map, game, phase, time left, frames per second, uptime; every player with connection id, score, deaths, ping, address and player id |
| `say <text>` | a server message to every player |
| `kick <id\|name>` / `ban <id\|name>` | remove a player (ban: and ban the player id) |
| `banid <id> [name]` / `bans` / `unban <n\|id>` | the ban list without a connection |
| `map <Map>` / `nextmap` / `restartmap` / `maplist` | map changes, the clients follow |
| `minplayers <n>` / `addbot [n]` / `removebot [n]` | bots |
| `quit` / `exit` / `shutdown` | stop the server (on the TCP console: `shutdown`; `logout` leaves) |

The TCP console sends the password in clear text: keep it on 127.0.0.1 or behind an SSH tunnel. Not done yet (future
work): a WebAdmin like UT2004's `xWebAdmin`, admin login for players in the game (`adminlogin`, `admin kick ...`
from UT2004 clients; `?AdminPassword=` would carry it), vote handling.

## Services

All service setups run the server unprivileged, log to `<state>/Logs/OpenUT2k4Server.log` (the engine keeps older logs
as `-backup-<date>.log`), stop it gracefully (the players are told "the server is shutting down", the log is flushed)
and restart it after a crash or a failure exit.

### Linux (systemd)

```sh
Server/linux/install-service.sh --user [--name dm1] [--config my.ini] [--convert --ut2004 /path/to/UT2004]
systemctl --user status openut2k4-server-dm1;  journalctl --user -u openut2k4-server-dm1 -f
loginctl enable-linger $USER          # a user service that runs without a login session
sudo Server/linux/install-service.sh --system [--run-as openut2k4] ...     # a system service
Server/linux/uninstall-service.sh --user [--name dm1] [--purge]
```

The unit (`Server/linux/openut2k4-server.service.in`): `Type=simple`, `KillSignal=SIGTERM`, `TimeoutStopSec=30`,
`Restart=on-failure`, `RestartSec=10`, `RestartPreventExitStatus=2 3`, `SuccessExitStatus=130 143`,
`NoNewPrivileges`, `PrivateTmp`, `ProtectSystem=full` (system units also `ProtectHome=read-only` and
`ReadWritePaths=<state>`), stdin `/dev/null`, stdout to the journal. Each instance has its own name, config, state
folder and port. The installer copies the default ini once per instance (system: `/etc/openut2k4-server/<name>.ini`,
mode 0640, it can hold the admin password; user: `~/.config/openut2k4-server/<name>.ini`), creates the state folder
(system: `/var/lib/openut2k4-server/<name>`, user: `~/.local/share/openut2k4-server/<name>`) and can convert the
content once (`--convert`). The engine's own SIGTERM handler ends the main loop; the server then stops the UE2 server
before the world is torn down.

### Windows (native service) — unverified

```bat
rem elevated prompt, in the server package
Server\windows\install-service.bat -Name dm1 -ContentCache D:\OpenUT2k4\ContentCache -UT2004Dir D:\UT2004
Server\windows\uninstall-service.bat -Name dm1 [-Purge]
```

`install-service.ps1` registers `OpenUT2k4Server-<name>` with the real binary (`OpenUT2k4\Binaries\Win64\OpenUT2k4Server.exe
OpenUT2k4 -Service=<name> -nullrhi -nosound ...`), start type delayed automatic, account `NT AUTHORITY\LocalService`
(or `-Account <user> -Password <pw>`), recovery actions restart after 10 s / 10 s / 60 s with `failureflag 1` (a
non-zero exit counts as a failure). It grants the account access to its state folder (`%ProgramData%\OpenUT2k4Server\<name>`),
the config and the content. The binary handles the service protocol itself (`Server/ArenaServerService.cpp`).
`openut2k4-server.bat` starts it interactively with a log window (`-log`).

### macOS (launchd) — unverified

```sh
Server/mac/install-service.sh [--agent | --daemon] [--name dm1] [--config my.ini] [--run-as _openut2k4]
Server/mac/install-service.sh --uninstall [--name dm1] [--purge]
```

A LaunchAgent (`~/Library/LaunchAgents/org.openut2k4.server.<name>.plist`) or a LaunchDaemon (`/Library/LaunchDaemons`,
`UserName` an existing unprivileged account): `RunAtLoad`, `KeepAlive` `SuccessfulExit=false` (restart after a failure),
`ThrottleInterval` 15 s, `ExitTimeOut` 30 s (SIGTERM, then SIGKILL), stdout / stderr into the state folder's `Logs`.
launchd cannot leave out exit codes 2 and 3: a misconfigured server is retried every 15 s.

## Building

| Engine | Target | How | Result |
|---|---|---|---|
| installed (binary, this repository's setup) | `OpenUT2k4Server` as a **Game** target | `Server/build-linux.sh` (also `-mac.sh`, `-windows.bat`) | the game engine, started headless: `-nullrhi -nosound`, `GIsClient` off during engine init. The module is compiled exactly like the game's (installed engines only build with the shared build environment, which also forbids leaving plugins out), and the executable name selects the server mode. Development by default: an installed engine has no logging in Shipping game builds (`--shipping` builds one anyway). |
| source-built | `OpenUT2k4Server` as a **Server** target (automatic when the engine is not installed; `OPENUT2K4_SERVER_TARGET_TYPE=Game\|Server` overrides) | `Server/build-linux.sh --server-target` (`-server -noclient -serverplatform=Linux`) | `UE_SERVER=1` (no renderer, audio device or Slate rendering compiled in), cooked for LinuxServer (no shaders), unique build environment with `bUseLoggingInShipping`, the upscaler plugin left out, `OPENUT2K4_SERVER_BINARY=1`, `ARENA_WITH_UPSCALING=0`, `ARENA_WITH_VULKAN=0` (`OpenUT2k4.Build.cs`). **Not built here** (no source engine on this machine). |

The package (`Client/Dist/Server-<Platform>/`): the engine's staged build of `OpenUT2k4Server` plus `openut2k4-server`,
`ucc`, `Server/OpenUT2k4Server.ini`, `Server/<platform>/` service files, `Server/README.txt`, `LICENSE`, `NOTICE.md`.
Debug symbols are left out (`-nodebuginfo`), and on Linux and Windows the Vulkan loader and validation layers
(`Engine/Binaries/ThirdParty/Vulkan`, 263 MB) are removed: a `-nullrhi` server never loads them. Staging goes to
`Saved/StagedBuilds/Server` (not the game's staging folder). `Client/Scripts/check-release.sh` checks the package for UT2004
data.

Linux package (Development, installed engine): **641 MB** on disk, 389 MB as `.tar.gz`: the binary 291 MB, the cooked
content 298 MB (the game's cooked content including shaders; a Server target cook leaves the shaders out), the engine's
ONNX runtime 43 MB (an engine plugin that logs errors without it) and MsQuic 9 MB. The game package for comparison:
739 MB (Shipping) / 1.2 GB (Development).

## Tests (Linux, 2026-10-17)

All local (127.0.0.1); the 3374 client isolated as in `Docs/UE2Net.md` (its own Xvfb display, `WAYLAND_DISPLAY` unset,
SDL forced to X11, `HOME` redirected); our client with `-nullrhi` on the same Xvfb display, joined with `-UE2Connect`.

| Check | Result |
|---|---|
| packaged server (`Dist/Server-Linux`), installed with `install-service.sh --user` (config and state under a scratch folder) | starts in about 5 s, hosts DM-Rrajigar (first map of the install's stock DM list, 13 maps), 29-30 frames/s (`TickRate` 30) |
| OldUnreal 3374 client + our client | both join (UE2 handshake, 201 packages), play with 7 bots (`MinPlayers=9`), bots added / removed as players come and go |
| admin console (TCP 127.0.0.1) | wrong password refused; `status`, `say`, `ban 3` (the 3374 player removed, `Bans.ini` written), `unban`, `banid`, `map DM-Rankin` (both clients followed) |
| ban across a restart | `banid` + `systemctl --user restart`: the ban was reloaded, the 3374 client's reconnect was refused (`LOCALBAN`) |
| `systemctl --user stop` with two players | 0.44 s; both connections closed with "the server is shutting down", our client logged "the server closed the connection", log flushed ("Log file closed"), `Result=success` (exit 143 counted as success) |
| `systemctl --user restart` | 0.48 s, new process hosting again |
| crash (`kill -SEGV`) | `Failed with result 'core-dump'`, restarted after 10 s (`NRestarts=1`) |
| no content cache | exit code 3, not restarted (`RestartPreventExitStatus`) |
| user service removed | `uninstall-service.sh --user`: no unit left |
| editor binary with `-OpenUT2k4Server` | the same (stdin commands `status`, `addbot`, `say`, `maplist`, `banid`/`bans`/`unban`, `nextmap` with our client following, `quit`) |
| server profile (`-ServerProfile=` a Vehicle CTF profile with custom maps) | loaded (Server.ini only), xVehicleCTFGame as CTF, the custom maps not in the test cache left out with a warning, fell back to the converted VCTF map |

Resource use (packaged Development build, DM-Rrajigar, 30 frames/s, measured from `/proc` over 30 s while other
builds kept the machine's load average near 27, so the CPU figures are on the high side):

| | CPU (one core) | RSS | PSS | threads |
|---|---|---|---|---|
| idle (no players, no bots) | 7.5 % | 571 MiB | 551 MiB | 33 |
| 1 player (3374) + 8 bots | 16.0 % | 727 MiB | 692 MiB | 34 |
| 2 players + 7 bots | 16.2-17.7 % | 738-768 MiB | 678-713 MiB | 34 |

## Unverified and future work

- **Windows**: the service code (`ArenaServerService.cpp`), `install-service.ps1` / `.bat`, `openut2k4-server.bat`,
  `ucc.bat` and `Server/build-windows.bat` were written without a Windows machine. Not compiled for Windows either.
  Stdin commands on Windows read the console that `-log` opens (untested); the TCP admin console is the dependable way.
- **macOS**: `Server/build-mac.sh`, the launchd plist and its installer are untested; the launcher looks for
  `OpenUT2k4Server.app/Contents/MacOS/OpenUT2k4Server`.
- **Server target**: `OpenUT2k4Server` as Type=Server needs a source-built engine; it was not built here. The code is
  guarded for it (`UE_SERVER`, `OPENUT2K4_SERVER_BINARY`, `ARENA_WITH_UPSCALING`, `ARENA_WITH_VULKAN`), but expect
  small fixes on the first source-engine build (e.g. modules the game uses that a server build does not provide).
- WebAdmin, in-game admin login, map voting, `ServerActors` beyond the stock ones, downloads (the server offers none),
  vehicles / Assault objectives / Invasion monsters for UT2004 clients.
- Smaller packages: a Server target cook drops the shaders; the ONNX runtime belongs to an engine plugin the server
  does not use but cannot leave out with an installed engine.
