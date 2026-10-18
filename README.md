# OpenUT2k4

> **Unofficial fan project.** OpenUT2k4 is not affiliated with, endorsed by or sponsored by Epic Games, Inc.
> Unreal and Unreal Tournament are trademarks of Epic Games, Inc. **No UT2004 game data is included.** You need your
> own copy of UT2004, or the free download that OldUnreal hosts with Epic's permission (the game can fetch it for you).
> See [`NOTICE.md`](NOTICE.md).

OpenUT2k4 is a recreation of Unreal Tournament 2004 on Unreal Engine 5.8, written in C++. It plays the maps,
characters, sounds and music of your own UT2004 install. It converts them once into a local content cache and builds
each map at runtime.

The game types are DM, TDM, CTF, Bombing Run, Double Domination, Onslaught, Assault, Invasion, Last Man Standing and
Mutant, with bots, a Single Player tournament, Instant Action and Host Game. It speaks UT2004's own network protocol,
so the client can join the UT2004 servers that are online today, and real UT2004 players can join games it hosts.

| Folder | Contents |
|---|---|
| [`Client/`](Client) | The Unreal Engine project: game code (`Source/`), config, generated content, tools, build scripts and docs. The dedicated server's C++ also lives here (`Client/Source/OpenUT2k4/Server/`, target `OpenUT2k4Server`), because it is the same module. |
| [`Server/`](Server) | The dedicated server: build and package scripts, the default server ini, launchers (`openut2k4-server`, `ucc`), service files for systemd, Windows and launchd, and [`Server/Docs/DedicatedServer.md`](Server/Docs/DedicatedServer.md). |

Detailed documentation:

- [`Client/README.md`](Client/README.md): features, converter, maps, menus, graphics, every command-line switch.
- [`Client/Docs/`](Client/Docs): game modes, the UE2 network protocol (`UE2Net.md`), HDR, upscaling, UnrealScript notes.
- [`Server/Docs/DedicatedServer.md`](Server/Docs/DedicatedServer.md): the full dedicated server reference.

---

## Requirements

- **Unreal Engine 5.8**: the Epic launcher / binary build or a source build, with target support for your platform.
- **A C++ toolchain**: the engine's bundled clang toolchain on Linux, Visual Studio 2022 on Windows, Xcode on macOS.
- **About 20 GB free disk space**: the engine build output, the cooked package and the converted content cache (about 6 GB).
- **To play**: a Vulkan-capable GPU and driver (Linux and Windows) or a Metal-capable Mac, plus a UT2004 install
  (retail, Steam, or the OldUnreal download).
- **To host a dedicated server**: no GPU is needed. The server needs the UT2004 install and the content cache.

Platform status: Linux is built and tested. The Windows and macOS scripts and service files exist but are
**unverified**: they were written without those machines.

### Getting Unreal Engine 5.8

On Windows and macOS, the simplest choice is to install UE 5.8 from the Epic Games Launcher. On Linux, use the
binary build from <https://www.unrealengine.com/linux> or build the engine from source, as shown below. A source
build is also needed for the server's `--server-target` option. Epic's official guide is
[Downloading Unreal Engine Source Code](https://dev.epicgames.com/documentation/unreal-engine/downloading-source-code-in-unreal-engine?lang=en-US).

**1. Get access to the source.** The engine repository is private. Your GitHub account must be linked to your Epic
Games account:

1. Sign in at [unrealengine.com](https://www.unrealengine.com) with your Epic Games account, hover over your
   username and open **Personal**.
2. Go to **Connections**, then **Accounts**, and click **Connect** under GitHub. Accept the Unreal Engine EULA, click
   **Link Account**, and click **Authorize EpicGames** on GitHub.
3. **Accept the EpicGames organisation invite.** Authorizing the link isn't enough on its own: GitHub then sends an
   invitation to join the **@EpicGames** organisation. You can accept it from the email, from
   <https://github.com/orgs/EpicGames/invitation>, or from your GitHub notifications. It expires after seven days. If
   it expired, unlink and link the account again to get a new one.

Until you accept the invite, <https://github.com/EpicGames/UnrealEngine> shows a 404 page and git reports
`Repository not found`, even though the account link shows as connected. Once you accept, the repository opens and
git can clone it.

**2. Clone the 5.8 release.** Cloning over HTTPS needs a GitHub
[personal access token](https://github.com/settings/tokens) as the password, or you can clone over SSH. A shallow
clone of a single branch is much smaller than the full history. If git still reports `Repository not found` after
you accept the invite, git is not signed in as the account that has access:

```sh
git clone --depth=1 --branch 5.8 git@github.com:EpicGames/UnrealEngine.git ~/UnrealEngine/UE_5.8
# or over HTTPS: https://github.com/EpicGames/UnrealEngine.git
```

The `release` branch always holds the newest official release, and `ue5-main` is unstable development. Use the
`5.8` branch, or a tag such as `5.8.3-release`, so that the engine matches the project.

**3. Build the engine (Linux).** The engine ships its own clang toolchain, so you don't need a system compiler.

```sh
cd ~/UnrealEngine/UE_5.8
./Setup.sh                  # downloads binary dependencies and the toolchain (~20 GB) and registers file types
./GenerateProjectFiles.sh   # generates the Makefile and IDE project files
make                        # builds UnrealEditor, ShaderCompileWorker and the other tools (an hour or more)
```

Allow about **150 GB** of free space for a source build of the engine, on top of the project's own needs listed
above. To check the build, start the editor with `Engine/Binaries/Linux/UnrealEditor`.

On Windows, run `Setup.bat` and `GenerateProjectFiles.bat` instead, then build the **UE5** solution in Visual Studio
2022 with the **Development Editor | Win64** configuration. On macOS, run `Setup.command` and
`GenerateProjectFiles.command`, then build in Xcode.

**4. Point the build scripts at the engine.** The engine root is the cloned folder (the folder that contains
`Engine/`):

```sh
export UE_ROOT=~/UnrealEngine/UE_5.8
cd OpenUT2k4/Client && Scripts/build-linux.sh      # see "Build" below
```

---

## Client

### Setup (players)

1. Build the client package (below) or use a released package. Unpack it anywhere and start `OpenUT2k4`
   (`OpenUT2k4.sh` on Linux, `OpenUT2k4.exe` on Windows).
2. On first start the **UT2004 Content** screen opens. Pick one:
   - **Use an install it found**: it searches the usual places, and a `UT2004` folder next to the game.
   - **Browse for UT2004...**: pick the folder that contains `System` and `Maps`.
   - **Download UT2004 from OldUnreal**: if you have no copy. This is a 3 GB download and needs about 9 GB free while
     installing. You must accept the Epic Games Terms of Service first.
3. The game converts the content once, in about 1.5 minutes, into about 6 GB of cache. Your UT2004 folder is only
   read, never changed. To convert again later, choose **UT2004 Content** in the main menu.
4. **Custom maps**: community maps and their packages go into the custom content folder, which is shown on the
   UT2004 Content screen. They are converted automatically.
5. **Online play**:
   - **Join Game** lists UT2004 servers from the 333networks and OpenSpy master servers, plus LAN servers.
   - You can also join with **Open IP** or a `ut2004://host:port` link.
   - Missing server packages download automatically. They go into a separate net cache.
   - No CD key is needed: the client logs in the way the OldUnreal 3374 patch does.
   - Stats tracking is off by default, as in UT2004.
6. **Hosting**: **Host Game** starts a listen server or a dedicated server on your machine. Both OpenUT2k4 and
   UT2004 players can join. To be reachable from the internet, forward the UDP ports listed under
   [Ports](#ports-and-firewall).

On Linux, [MangoHud](https://github.com/flightlessmango/MangoHud) shows the frame rate: `mangohud ./OpenUT2k4.sh`.

### Build

The project is in `Client/`. Each build script takes the engine root as its first argument, or reads it from the
`UE_ROOT` environment variable. On the first build the script also does these steps:

1. builds the editor target;
2. generates the project's own content (materials, synthesised sounds and two empty maps), using
   `Client/Tools/GenerateContent.py`;
3. cooks, stages and packages a **Shipping** build.

This package contains no UT2004 data; `Client/Scripts/check-release.sh` checks it.

```sh
git clone https://github.com/GloriousEggroll/OpenUT2k4
cd OpenUT2k4/Client

# Linux  -> Client/Dist/Linux/
Scripts/build-linux.sh /path/to/UE_5.8          # or: UE_ROOT=/path/to/UE_5.8 Scripts/build-linux.sh
Scripts/build-linux.sh --development            # a Development package (logging, console)

# macOS  -> Client/Dist/Mac/   (unverified)
Scripts/build-mac.sh "/Users/Shared/Epic Games/UE_5.8"
```

```bat
rem Windows -> Client\Dist\Windows\   (unverified)
cd OpenUT2k4\Client
Scripts\build-windows.bat "C:\Program Files\Epic Games\UE_5.8"
```

Run the release check before you distribute a package:

```sh
Scripts/check-release.sh Dist/Linux
```

#### Development builds (editor)

```sh
UE=/path/to/UE_5.8
$UE/Engine/Build/BatchFiles/Linux/Build.sh OpenUT2k4Editor Linux Development -Project="$PWD/OpenUT2k4.uproject" -WaitMutex
Scripts/generate-content.sh "$UE"                                # once: the project's generated assets
$UE/Engine/Binaries/Linux/UnrealEditor "$PWD/OpenUT2k4.uproject"         # the editor
$UE/Engine/Binaries/Linux/UnrealEditor "$PWD/OpenUT2k4.uproject" -game   # the game, without packaging
```

On Windows, open `OpenUT2k4.uproject` (right click, then *Generate Visual Studio project files*) and build the
`OpenUT2k4Editor` target in Visual Studio 2022. On macOS, generate the Xcode project the same way.

The UT2004 install is never part of the build. If the default search doesn't find it,
`Scripts/import-ut2004.sh` and the Python tools in `Client/Tools/` (editor-side importer, used only by developers)
read it from `$UT2004_DIR`.

#### Testing against a stock UT2004 server

`Client/Scripts/ut2004-test-server.sh` runs a local, LAN-only stock UT2004 dedicated server for testing the
browser and the network code. Point it at an extracted UT2004 3369 dedicated server with `$UT2004_SERVER_ROOT`
or `--root`.

---

## Server

The OpenUT2k4 dedicated server (`OpenUT2k4Server`) is headless: no window, GPU or audio. It hosts one match after
another with OpenUT2k4's UE2 network server, so **real UT2004 clients** (retail 3369, OldUnreal 3374) and OpenUT2k4
players can both join.

It reads UT2004-style ini files and a UCC-style command line, and it runs as a service on Linux (systemd),
Windows (a native service) and macOS (launchd).

The distributed server uses only the **stock UT2004 dedicated server defaults**: no custom mods or mutators.

### Build

The server is built from the same project as the client. The scripts in `Server/` build the `OpenUT2k4Server`
target from `Client/`, then add the launchers, the default ini and the service files from `Server/Package/`.

```sh
cd OpenUT2k4
Server/build-linux.sh /path/to/UE_5.8                 # -> Client/Dist/Server-Linux/
Server/build-linux.sh --shipping /path/to/UE_5.8      # Shipping configuration
Server/build-linux.sh --server-target /path/to/UE_5.8 # true Type=Server target (needs a source-built engine)
Server/build-mac.sh  ...                              # -> Client/Dist/Server-Mac/       (unverified)
```

```bat
Server\build-windows.bat "C:\Program Files\Epic Games\UE_5.8"   & rem -> Client\Dist\Server-Windows\ (unverified)
```

If `Client/Content/Materials` does not exist yet, run the client build script once first, because it generates the
project content.

- **Installed (binary) engine**: the server is a Game target started headless (`-nullrhi -nosound`), and the
  launchers add those switches. The Linux package is about 640 MB.
- **Source-built engine**: `--server-target` builds a real dedicated-server binary with no renderer or audio, and
  it is smaller. This path has not been built yet, so expect small fixes.

The package contains no UT2004 data, and `check-release.sh` runs on it automatically.

### Setup

1. **Copy the package** (`Client/Dist/Server-Linux/`) to the server machine, for example to `/opt/openut2k4-server`.
2. **Provide UT2004 content.** The server needs a UT2004 install, for the clients' package list, and the content
   cache converted from it. You can copy the `System`, `Maps`, `Textures`, `StaticMeshes`, `Sounds`, `Music`,
   `Animations` and `Speech` folders of any UT2004 install, or use the OldUnreal download. Convert the content once:

   ```sh
   ./openut2k4-server -ConvertOnly -UT2004Dir=/path/to/UT2004
   ```

   If the game has already run on the same machine and user, the server finds that install and cache by itself.
   Otherwise use `-UT2004Dir=` / `-ContentCache=`, or `[OpenUT2k4.Paths]` in the ini.

3. **Configure.** Copy `Server/OpenUT2k4Server.ini` from the package and edit it (details under
   [Configuration](#configuration)).
4. **Run** it interactively, or install it as a service (both below).

### Running

```sh
./openut2k4-server                                                    # DM, the stock DM map rotation, port 7777
./openut2k4-server -ServerConfig=MyServer.ini                         # your ini
./openut2k4-server "CTF-Face3?game=XGame.xCTFGame?MinPlayers=8" -port=7787
./ucc server "DM-Rankin?game=XGame.xDeathMatch?MaxPlayers=8" ini=MyServer.ini log=server.log    # UT2004 style
./openut2k4-server -ServerProfile=/path/to/profile                    # a profile folder (Server.ini or UT2004.ini)
```

On Windows, use `openut2k4-server.bat` and `ucc.bat`.

**URL options** follow UT2004:

- `game=` takes the short or the full class name.
- `MaxPlayers`, `MaxSpectators`, `MinPlayers` (bots), `GoalScore`, `TimeLimit`, `GameDifficulty`.
- `Mutator=A,B`, `GamePassword`, `AdminPassword`, `ServerName`, `Port`, `LANServer`, `MapList=A,B,C`.

**Useful switches**:

- `-port=` sets the game port.
- `-multihome=<ip>` binds the server to one address.
- `-lanplay` makes it a LAN server.
- `-NoUplink` keeps it off the master servers.
- `-ServerStateDir=` sets where bans and logs go.
- `-userdir=` sets where `Saved/` goes.

**Exit codes**:

| Code | Meaning |
|---|---|
| 0 | Normal exit (143 / 130 after SIGTERM / SIGINT) |
| 2 | Bad configuration |
| 3 | No content cache or UT2004 install |
| 4 | The match could not be hosted, for example because the port is in use |

### Configuration

Settings come in layers, and later layers win:

1. the stock UT2004 defaults;
2. the package's `Server/OpenUT2k4Server.ini`;
3. your ini (`-ServerConfig=` / `ini=`), which can also be an existing `UT2004.ini`, since the section names match;
4. a profile folder (`-ServerProfile=`);
5. the command line.

Main sections:

| Section | Keys |
|---|---|
| `[OpenUT2k4.Server]` | `GameType` (e.g. `XGame.xDeathMatch`, `XGame.xCTFGame`, `Onslaught.ONSOnslaughtGame`, `UT2k4Assault.ASGameInfo`), `Map`, `Maps` (rotation, one line each), `Mutators`, `MinPlayers`, `BotsWhenEmpty`, `BotSkill`, `ExtraOptions`, `TickRate`, `LANServer` |
| `[URL]` | `Port` (default 7777) |
| `[IpDrv.MasterServerUplink]` | `DoUplink`: list the server publicly. **Default `False`.** |
| `[Engine.GameReplicationInfo]` | `ServerName`, `ShortName`, `AdminName`, `AdminEmail`, `MessageOfTheDay` |
| `[Engine.AccessControl]` | `AdminPassword`, `GamePassword`, `BannedIDs` |
| `[Engine.GameInfo]` and `[<game class>]` | `MaxPlayers`, `MaxSpectators`, `TimeLimit`, `GoalScore`, `GameDifficulty` |
| `[OpenUT2k4.AdminConsole]` | `bEnabled`, `ListenAddress` (default 127.0.0.1), `Port` (default 7780) |
| `[OpenUT2k4.Paths]` | `ContentCache`, `UT2004Dir`, `UserContent` |

Without `Maps=` lines, the server uses the stock map list of the game type from the UT2004 install. Bots follow
UT2004's `MinPlayers` rule. Every key is described in `Server/Docs/DedicatedServer.md`.

### Ports and firewall

| Port | Protocol | Purpose |
|---|---|---|
| 7777 (game port, `Port=` / `-port=`) | UDP | game traffic |
| game port + 1 (7778) | UDP | server query (browsers and master servers) |
| 10777 | UDP | LAN beacon (LAN browsing) |
| 7780 (admin console, if enabled) | TCP | admin console. Keep it on 127.0.0.1 or behind an SSH tunnel: the password is sent in clear text |

To be listed publicly, set `DoUplink=True` and forward the game port and the query port (UDP) to the server. The
uplink registers with the 333networks and OpenSpy master servers.

### Administration

Admin commands work on the server's console (stdin), and on the TCP admin console (`nc 127.0.0.1 7780`) when
`[OpenUT2k4.AdminConsole] bEnabled=True` and an `AdminPassword` is set. Type `help` for the list.

| Command | What it does |
|---|---|
| `status`, `players` | Show server and player info |
| `say <text>` | Send a server message |
| `kick <id\|name>`, `ban <id\|name>` | Kick or ban a player |
| `banid <id>`, `bans`, `unban <n\|id>` | Manage bans |
| `map <Map>`, `nextmap`, `restartmap`, `maplist` | Change maps |
| `minplayers <n>`, `addbot [n]`, `removebot [n]` | Manage bots |
| `quit` / `shutdown` | Stop the server |

Bans are saved in `<state>/Bans.ini`.

### Running as a service

All service setups run the server as an unprivileged user, stop it gracefully and restart it after a crash. They do
not restart it on exit codes 2 and 3, which mean the configuration needs fixing. Run these commands from the
package folder.

**Linux (systemd):**

```sh
Server/linux/install-service.sh --user --name dm1 --config my.ini [--convert --ut2004 /path/to/UT2004]
systemctl --user status openut2k4-server-dm1
journalctl --user -u openut2k4-server-dm1 -f
loginctl enable-linger $USER                     # keep a user service running without a login session

sudo Server/linux/install-service.sh --system --run-as openut2k4 --name dm1 ...   # a system service
Server/linux/uninstall-service.sh --user --name dm1 [--purge]
```

Each instance has its own name, ini, state folder and port:

| | User service | System service |
|---|---|---|
| ini | `~/.config/openut2k4-server/<name>.ini` | `/etc/openut2k4-server/<name>.ini` |
| state | `~/.local/share/openut2k4-server/<name>` | `/var/lib/openut2k4-server/<name>` |

**Windows (native service, unverified).** Run these from an elevated prompt:

```bat
Server\windows\install-service.bat -Name dm1 -ContentCache D:\OpenUT2k4\ContentCache -UT2004Dir D:\UT2004
Server\windows\uninstall-service.bat -Name dm1 [-Purge]
```

**macOS (launchd, unverified):**

```sh
Server/mac/install-service.sh --agent --name dm1 --config my.ini      # or --daemon --run-as _openut2k4
Server/mac/install-service.sh --uninstall --name dm1 [--purge]
```

### Known limitations

- No WebAdmin yet.
- No in-game `adminlogin` for UT2004 clients.
- No map voting.
- The server offers no package downloads.
- Windows and macOS builds and services are untested.
- The Type=Server target has not been built yet.

See the "Unverified and future work" section of `Server/Docs/DedicatedServer.md`.

---

## OpenUT2k4 Validator (server admins)

OpenUT2k4 identifies itself honestly: every login carries `?OpenUT2k4=<version>`. It never claims to be the stock
client and never runs, answers or imitates third-party anti-cheat tools. For admins who want more, the **OpenUT2k4
Validator** is an opt-in mutator for stock UT2004 servers (3369 / OldUnreal 3374, source in `Server/Validator/`) and
is built into the OpenUT2k4 dedicated server (off by default):

- Each OpenUT2k4 player has a key the game makes silently on first start (no prompt, no account). The validator
  challenges OpenUT2k4 clients (real UT2004 clients are never challenged) and the key's fingerprint becomes a stable,
  unforgeable player id: Identified / Failed / None, with a policy per result (default: log and mark, never kick).
- Per-server first-seen dates, key bans combined with the connection's IP, an optional subnet and the UT2004 CD key
  id (a banned player who deletes the key is caught by the IP), a whitelist, probation for new keys, signed shared ban
  lists (Ed25519), and an API for other mods and server-side anti-cheat.
- Conservative, log-only server-side sanity checks (time stamp rate, walking speed, NewNet time stamps, aim snaps).
- It proves player identity, not build integrity: a modified client identifies just as well. Cheat detection is the
  server-side checks and the admins.

Build the mutator with `Server/Validator/build-validator.sh` (UCC under Wine); install with
`ServerActors=OpenUT2k4Validator.OUTValidator`. Everything else: [`Server/Docs/Validator.md`](Server/Docs/Validator.md).

**Verifying downloads**: official release archives come with a minisign signature (`.minisig`) and a `.sha256` file.
The project's release public key is:

```
RWTwrDCWkxaitEWZhWEHlXdSShHiq+gy9bdiuAOTGT2/dUxD5Z7d+ap/
```

Check a download with:

```sh
minisign -Vm OpenUT2k4-<version>-<platform>.tar.gz -P RWTwrDCWkxaitEWZhWEHlXdSShHiq+gy9bdiuAOTGT2/dUxD5Z7d+ap/
# or, without minisign:
python3 Client/Tools/openut2k4_signing.py verify --pub RWTwrDCWkxaitEWZhWEHlXdSShHiq+gy9bdiuAOTGT2/dUxD5Z7d+ap/ <archive>
```

See Validator.md, "Release signing".

## What is not in this repository

- **UT2004 game data.** No maps, textures, models, sounds, music or text from the game, and no files converted from
  them. The converter runs on each player's or server owner's machine, and its output stays there.
- **Build output and caches**: `Binaries/`, `Intermediate/`, `Saved/`, `DerivedDataCache/`, `Dist/`, and
  `Content/Imported*` / `ImportCache/` (a developer's local editor imports). All of these are git-ignored.
- **Private server profiles.** Keep your own profiles (mods, passwords) outside the repository, or in
  `Client/Saved/PrivateServer/`, which is git-ignored.

## Licence

The OpenUT2k4 code, scripts and generated content are under the MIT licence ([`LICENSE`](LICENSE)). Unreal Engine is
licensed separately by Epic Games under the Unreal Engine EULA.

Third-party code is listed in [`NOTICE.md`](NOTICE.md):

- an unshield port (MIT);
- the AMD FidelityFX FSR 3.1 shaders (MIT).
