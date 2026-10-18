# OpenUT2k4

> **Unofficial fan project.** OpenUT2k4 is not affiliated with, endorsed by or sponsored by Epic Games, Inc.
> Unreal and Unreal Tournament are trademarks of Epic Games, Inc. No UT2004 game data is included: you need your
> own copy of the game (or the free download from OldUnreal, which the game can fetch for you). See `NOTICE.md`.
>
> Licence: MIT (`LICENSE`) for this project's code and generated content. Unreal Engine is licensed separately by Epic Games.

OpenUT2k4 is a fast arena shooter for Unreal Engine 5.8. All gameplay is written in C++ (module `OpenUT2k4`).
The game ships no maps: it plays the maps of the player's own UT2004 install, converted once into a
local content cache and built into an empty base level at runtime (see "Maps" below), so the project ships with almost
no authored content. The only assets are a few that `Tools/GenerateContent.py` generates: four
materials, a set of synthesised sound effects and two empty maps. Everything in the project is original.

## For players

### First start: your UT2004 content

OpenUT2k4 plays with the maps, characters, textures, sounds and music of **your own copy of UT2004**. On first start
the game opens the **UT2004 Content** screen and offers:

- **Use an install it found**: it looks in the usual places (and in a `UT2004` folder next to the game).
- **Browse for UT2004...**: pick the folder that contains `System` and `Maps` (often called `Unreal Tournament 2004`
  or `UT2004`). If you pick a folder one level off, the game corrects it.
- **Type a path...**: if no folder dialog is available.
- **Download UT2004 from OldUnreal**: no copy? OldUnreal hosts the full game for free (3 GB download, about 9 GB of
  free disk space needed while installing). You must accept the Epic Games Terms of Service first.

The game then converts the content once (about 1.5 minutes, about 6 GB). Your UT2004 folder is only read, never
changed. You can cancel at any time: converting again continues where it stopped. To convert again later (for
example from another copy), choose **UT2004 Content** in the main menu.

### Adding custom maps and content

Community maps and their packages (textures, meshes, sounds, music) go into the **custom content folder**. You
never have to touch your UT2004 install.

| OS | Custom content folder |
| --- | --- |
| Linux | `~/.local/share/OpenUT2k4/UserContent` (or `$XDG_DATA_HOME/OpenUT2k4/UserContent`) |
| Windows | `%LOCALAPPDATA%\OpenUT2k4\UserContent` |
| macOS | `~/Library/Application Support/OpenUT2k4/UserContent` |

The game creates the folder on first start. The quickest way there: **UT2004 Content > Custom content... > Open the
folder**.

1. **Copy the files in.** Any layout works: the usual UT2004 folders (`Maps`, `Textures`, `StaticMeshes`, `Sounds`,
   `Music`, `System`), everything loose in the top folder, or a whole download in its own subfolder.
   - Maps: `.ut2`. Packages: `.utx` (textures), `.usx` (static meshes), `.uax` (sounds), `.ukx` (animations),
     `.u` (code), `.ogg` (music). Map names and authors come from the map's `.int` / `.ucl` files if included.
   - **Archives work without unpacking**: `.umod`, `.ut2mod` and `.zip`. Unpack `.7z` and `.rar` files yourself first.
2. **Start the game** (or choose **Rescan custom content** on the UT2004 Content screen while it is running). Only
   new or changed files are converted; a few maps take about a second.
3. **Play**: custom maps appear in **Instant Action** in the map list of every mode they support, marked
   **[custom]**, with their title, author, recommended player count and preview picture.

To remove a map, delete its files from the folder; the game cleans up its converted copy on the next start.

**What works and what does not (yet):**

| Content | Status |
| --- | --- |
| Maps built from standard UT2004 parts (most DM, CTF, BR, DOM, ONS, AS maps and map remakes) | Play like stock maps |
| Custom texture, static mesh, sound and music packages a map needs | Supported |
| Maps whose objects need custom UnrealScript code (from a `.u` package or embedded in the map) | The game runs the map's own script for triggers, counters, custom health and shield pickups and other scripted map events. Custom weapons, monsters, vehicles, movers, pickup bases and effects still play as their standard UT2004 version (or are left out); such maps are marked **[custom\*]** |
| Mutators made of UnrealScript (UT2004's own and custom ones) | Rule mutators work: low gravity, InstaGib, quad jump, regeneration, vampire, no adrenaline, big head (heads grow with the score: the head bone is scaled, and the head hit sphere with it), game speed, weapon replacements and more. Pick them on the Instant Action Mutators tab, or start a match with `?Mutator=Package.Class` (several separated by commas) |
| Mods that add new weapons, game types, HUDs or menus in UnrealScript | Not supported yet |
| Character packs (`.upl` + skins) and `.umx` music | Recognised but not used yet |
| A custom file with the same name as a UT2004 file | UT2004's own file is used; rename the custom one |

**When a map does not show up**, open **UT2004 Content > Custom content...**: every map is listed as ready, plays
with limits, or cannot be played, with the reason (for example `Missing package: SomeTextures. Put it in the custom
content folder.`). Conversion details are in `<content cache>/usercontent/errors.log`. The content cache is
`~/.local/share/OpenUT2k4/ContentCache` on Linux, `%LOCALAPPDATA%\OpenUT2k4\ContentCache` on Windows and
`~/Library/Application Support/OpenUT2k4/ContentCache` on macOS.

Developer details (file formats, incremental conversion, test switches) are in
[Custom content (community maps)](#custom-content-community-maps) below.

### Showing the frame rate

On Linux, start the game with [MangoHud](https://github.com/flightlessmango/MangoHud) to see the frame rate:
`mangohud ./OpenUT2k4.sh`.

MangoHud's frame time logging (`autostart_log=1` or the logging hotkey) runs a thread that MangoHud does not
stop when the Vulkan loader unloads the layer at shutdown; the thread then wakes up in unloaded code and the
process ends with a segfault (exit code 139) after `Log file closed`, or with a heap corruption abort when the
library's static destructors free the logger under the running thread. When MangoHud is loaded the game keeps
its library loaded until the process ends (`Exit: keeping overlay layer ...` in the log) and, once the engine has
shut down completely, ends the process before MangoHud's destructors run (`OpenUT2k4.cpp`), so logging runs
exit cleanly too. `-NoOverlayPin` turns this off (to reproduce the crash).

Every Vulkan run ends with `FVulkanSubresourceAllocator ... has unfreed Image resources` / `LEAK ... 4976644 ImageRT`
(allocated by `FRenderTargetPool::CreateRenderTarget`). It is the engine's: our code keeps no pooled render
target of its own (FSR's history lives in the view state), it also happens in the main menu, and an empty engine template project shows the same leak with only
`r.DynamicGlobalIlluminationMethod=1` / `r.ReflectionMethod=1` (Lumen) set, and none without them. It is harmless.

### Server browser (Join Game)

**Join Game** in the main menu opens a server browser for stock UT2004 servers (build 3369 / 3374), styled like
UT2004's own. JOIN (and OPEN IP) hand the address to the UE2 network client (`ArenaNet::JoinServer`, see "Joining
UT2004 servers" below and `Docs/UE2Net.md`). The browser's status line shows the connection status, and a failure
opens a message box with the client's reason. Builds without that client say "Joining UT2004 servers is not
available yet".

- **Tabs**: Favorites, LAN, and one tab per game type (DM, TDM, CTF, BR, DDOM, ONS, AS, INV, LMS), plus All for
  every listed server, mods included. A game type tab asks the master servers for that game type. Each tab keeps its
  list until you press REFRESH, and a tab is fetched the first time you open it.
- **Server list**: Server Name (password and instagib icons), Map, Players, Ping. Click a column heading to sort by
  it; click it again to reverse the order. Servers that don't answer show "N/A" and are listed last. "No Empty
  Servers" and "No Full Servers" filter the list.
- **Rules and Players** of the selected server are shown below the list and refresh every 6 seconds. The rules use
  UT2004's names, and players show their team.
- **Footer**: BACK, OPEN IP (type `host:port`; OK joins, ADD FAVORITE stores it), ADD / REMOVE FAVORITE, REFRESH,
  and JOIN. Double click or Enter on a server also joins it.
- **Saved** in `Game.ini` (`[/Script/OpenUT2k4.ArenaUserSettings]`): `ServerFavourites=host:port` (one line per
  favourite), `bBrowserHideEmpty`, `bBrowserHideFull` and `BrowserTab`. `MasterServers=host:port,group` (one line per
  master) replaces the default master list.

How it works (`Source/OpenUT2k4/Net/ArenaServerBrowser.*` is the network code; `ArenaServerBrowserPage.*` is the
page):

- **Master servers**: the browser uses the list from the OldUnreal 3374 patch's `Default.ini`:
  ut2004master.333networks.com, ut2004master.gonespy.com, ut2004master.errorist.eu, master.frag-net.com and
  master.noccer.de (group 1, which share their lists), and utmaster.openspy.net (group 2). It asks one master of each
  group, trying the next in the group if one fails, and merges the replies. It uses UE2's master protocol over TCP
  port 28902: length-prefixed messages, the challenge, the `UT2K4CLIENT` login, the verification, then a query with
  a `gametype` filter. The reply has the address, ports, name, map, game type, players and flags of each server.
- **No CD key**: these masters accept listing queries with an empty CD key hash and response, so the browser never
  reads, hashes or sends a CD key.
- **Server queries**: UDP to the query port (the game port + 1), using the `0x80 0 0 0` packets: 0 is server info
  (also used as the ping), 1 is rules, and 2 is players. Queries run on the game thread with a non-blocking socket:
  at most 24 in flight and 60 sent per second, with a 2 second timeout and one retry. Master servers and host
  names are handled on worker threads.
- **LAN**: the info query is broadcast to UDP 10777 (and sent to 127.0.0.1), and each LAN server answers from its
  query port.

Test hooks (non-shipping):

```
# open the browser on a tab: -AutoMenuPage=ServerBrowser -AutoBrowserTab=<Favourites|LAN|DM|TDM|CTF|BR|DOM|ONS|AS|INV|LMS|All or index>
#   [-AutoBrowserSort=Name|Map|Players|Ping] [-AutoBrowserOpenIP=<address>]
# -AutoMenuScript commands: BrowserTab=, BrowserSort=, BrowserSelect=<row or ip:port>, BrowserRefresh=1,
#   BrowserAddFavourite=<address>, BrowserRemoveFavourite=<address>, BrowserOpenIP=<address>, BrowserJoin=<address>
OpenUT2k4 -AutoMenuPage=ServerBrowser -AutoBrowserTab=LAN -AutoMenuScript="4:BrowserSelect=127.0.0.1:7777;11:Shot=lan;12:Quit"
```

To test against a local stock server, `Scripts/ut2004-test-server.sh` runs a LAN-only UT2004 dedicated server (see
"Test server" in `Docs/UE2Net.md`).

### Hosting games (Host Game)

**Host Game** in the main menu opens UT2004's Host Game page (UT2K4GamePageMP): the Instant Action tabs (Gametype,
Select Map, Game Rules, Mutators, Bot Config) with **Server Rules** as the 4th tab: Server Name, Max Players, Max
Spectators, Game Password, Port (UDP game port; queries use port + 1), LAN Server (no internet rate limit), Show in
Server Browsers. Saved in `Game.ini` (`HostServerName`, `HostMaxPlayers`, `HostMaxSpectators`, `HostPassword`,
`HostPort`, `bHostLANServer`, `bHostAdvertise`).

The match is hosted with OpenUT2k4's own server for the UT2004 network protocol (`Source/OpenUT2k4/Net/UE2Server*`):
**real UT2004 clients (retail 3369 and OldUnreal 3374) and OpenUT2k4 players join the same server**. OpenUT2k4
players join through the UE2 client (see "Joining UT2004 servers"), so there is one protocol for everyone; UE5's net
driver is not used. The protocol side is described in `Docs/UE2Net.md`, "Hosting: the UE2 server".

- **LISTEN** starts the match with `?UE2Host=1?UE2Port=<port>` and the server options (`?MaxPlayers ?MaxSpectators
  ?ServerName ?GamePassword ?LANServer ?Advertise`); you play in the hosted match.
- **DEDICATED** starts the dedicated server mode of the same executable (`-OpenUT2k4Server`, see
  `Server/Docs/DedicatedServer.md`: headless, admin console, `MinPlayers` bots) with the match settings as a UT2004 command
  line; its log is `OpenUT2k4Server.log`. The menu stays open.
- **The map** must be one the clients have: the converted stock maps, or a custom map from your custom content folder
  (UT2004 players need the same file; the server offers no downloads). Procedural layouts cannot be hosted for UT2004
  clients.
- **Joining**: UT2004 players open `<your address>:<port>` (Open IP / the command line / their LAN tab); OpenUT2k4 players
  use Join Game (the LAN tab lists the server) or Open IP. A password is asked as in UT2004 (`NEEDPW` / `WRONGPW`), a
  full server says so (`SERVERFULL`). The package list (`USES`) comes from your UT2004 install, with its real GUIDs, so
  UT2004 clients find every package.
- **What players see**: the game info, every player (names, scores, deaths, teams, characters), pawns running, shooting
  (muzzle flashes, fire sounds, shock, lightning and link beams, shield gun shields, bullet impacts), dying (ragdolls)
  and respawning, projectiles, pickups taken (with their sound and message) and respawning, lifts and doors, CTF flags
  and bases, DOM points, the bombing run ball, Onslaught nodes, cores and vehicles (with drivers and guns), Assault
  objectives, Invasion monsters, LMS lives; kill lines, first blood, sprees, multi kills, head shots, CTF messages, the
  countdown, the end of match announcement and view of the winner, chat (team chat with the speaker's location); link
  gun links and colours; each character's own pain, death, landing and jump sounds (female, robot, Skaarj ... voices
  from the character's species). Remote players move with UT2004's movement rules
  (`ServerMove`, corrections only when their position is off by more than 48 UU), fire, switch weapons, use, respawn,
  change team, chat, use combos and spectate. They are ordinary players of the match: scores, kills, team games and
  bots alongside.
- **Server browsers**: the server answers UT2004 server queries on port + 1 and the LAN broadcast on 10777 (the LAN tab
  of UT2004 and OpenUT2k4). **Show in Server Browsers** registers the server with the OldUnreal master servers; it is
  off by default.
- **Remote players** can also drive, aim and fire vehicles (`ServerDrive`, `VehicleFire`) and change seats with the
  number keys (`ServerChangeDriverPosition`); other players see the turrets turn.
- **Chat**: the host's player types with T / R like everyone else (see Controls); its lines go to the UT2004 clients.
- **Internet play**: UE2's relevancy (players and projectiles out of sight are not sent) and each client's `NETSPEED`
  are kept, so a client on a slow line gets the most important updates first.
- **Map rotation**: at the end of the match the next map of the game type loads and every client follows
  (`ClientTravel`), as on a stock server (`?MapList=A,B,C` sets the rotation).
- **Admin**: the pause menu's **Admin** tab lists the connected players with Kick, Ban and the ban list (Unban). Bans
  go by the player id UT2004 servers use (from the CD key hash), so they hold across names.
- See `Docs/UE2Net.md` for what is not hosted yet.

Test hooks: `-UE2ServerTrace` (control lines, channels, function calls), `-UE2NoUplink` (never contact a master
server), `-UE2ServerBind=<ip>`, `-UE2ServerTestDrive=<s>` (`-UE2ServerTestDriveSeats`), `-UE2ServerTestMessages=<s>`.

```
# host DM-Rankin with 3 bots for UT2004 clients on UDP 7787 (no master server uplink)
UnrealEditor OpenUT2k4.uproject "/Game/Maps/Arena?Mode=DM?Layout=DM-Rankin?Bots=3?UE2Host=1?UE2Port=7787" -game -RuntimeContent -ContentCache=<cache> -UE2NoUplink
# the same from the Host Game page: LISTEN after 7 s on port 7790
UnrealEditor OpenUT2k4.uproject -game -RuntimeContent -ContentCache=<cache> -AutoMenuPage=HostGame -AutoMenuScript="2:Mode=DM;3:Layout=Imported:DM-Rankin;4:HostPort=7790;7:HostStart=1" -UE2NoUplink
# an OpenUT2k4 player: the LAN tab finds it, BrowserJoin joins it (UE2 protocol)
UnrealEditor OpenUT2k4.uproject -game -RuntimeContent -ContentCache=<cache> -AutoMenuPage=ServerBrowser -AutoBrowserTab=LAN -AutoMenuScript="14:BrowserJoin=127.0.0.1:7790"
# a UT2004 player (OldUnreal 3374 Linux client)
ut2004-bin-amd64 127.0.0.1:7790
```

### Dedicated server

`OpenUT2k4Server` is a headless dedicated server for UT2004 and OpenUT2k4 players (no window, GPU use, audio or local
player), configured from UT2004-style ini files and a UCC-style command line, runnable as a service on Linux
(systemd), Windows (a native service) and macOS (launchd). Like the game it ships no UT2004 data: it uses your UT2004
install and the content cache converted from it. Full documentation: `Server/Docs/DedicatedServer.md`.

```sh
Server/build-linux.sh                     # package: Dist/Server-Linux/ (also build-mac.sh, build-windows.bat)
cd Dist/Server-Linux
./openut2k4-server -ConvertOnly -UT2004Dir=<UT2004 install>          # once, unless the game has converted it already
./openut2k4-server "CTF-Face3?game=XGame.xCTFGame?MinPlayers=8" -port=7777
./ucc server "DM-Rankin?game=XGame.xDeathMatch" ini=MyServer.ini      # UT2004 style
./openut2k4-server -ServerConfig=MyServer.ini [-ServerProfile=<dir>]  # copy Server/OpenUT2k4Server.ini to start from
Server/linux/install-service.sh --user [--name dm1] [--config MyServer.ini]   # systemd; --system as root
```

- **Configuration**: `Server/OpenUT2k4Server.ini` holds the stock UT2004 server defaults (port 7777, 16 players, 2
  spectators, time limit 20, stock goal scores; master server uplink off) under UT2004's section names
  (`[Engine.GameReplicationInfo]` server name / MOTD, `[Engine.AccessControl]` passwords and bans, `[Engine.GameInfo]`,
  per game type sections, `[URL] Port`, `[IpDrv.MasterServerUplink] DoUplink`, `[Engine.GameEngine]`). The map rotation
  defaults to the install's stock map list of the game type; `MinPlayers` bots fill the match while players are on.
- **Admin**: `help` on stdin, or a password protected TCP admin console: `status`, `say`, `kick`, `ban`, `banid`,
  `unban`, `map`, `nextmap`, `restartmap`, `minplayers`, `addbot`, `quit`. WebAdmin is future work.
- **Binary**: on this installed engine `OpenUT2k4Server` is a Game target started headless (the server mode follows
  from the executable name; `-OpenUT2k4Server` does the same for the game or editor binary). With a source-built engine
  the same target becomes a real Type=Server target (`Server/build-linux.sh --server-target`).
- Tested on Linux as a `systemctl --user` service with the OldUnreal 3374 client and our client; the Windows and macOS
  service parts are written but unverified (`Server/Docs/DedicatedServer.md` lists what was tested and measured).

### Joining UT2004 servers

JOIN in the server browser, Open IP, or `ut2004://host:port[?Name=...][?Password=...]` links connect to a UT2004
server with OpenUT2k4's own UE2 network client (`Source/OpenUT2k4/Net/UE2Net*`, UE5's net driver is not used). The
protocol is described in `Docs/UE2Net.md`.

- **Playing**: you join as a player (add `?SpectatorOnly=1` to the address to watch). Your pawn is predicted on your
  machine with the UT2004 movement rules (running, jumping, double jumps, dodges, wall dodges, crouching) and checked
  by the server like a UT2004 client's: the server corrects it when it disagrees, and the client replays its newer
  moves.
  - Fire and Alt-Fire shoot; the shots, hits and kills come from the server.
  - The weapon keys, the mouse wheel and the weapon bar switch weapons. Pickups are the server's.
  - The HUD shows your health, shield, adrenaline, weapons, ammo and the direction of hits.
  - After a death the camera stays behind your body; Fire respawns.
  - Use, suicide, combos (adrenaline key sequences) work as in UT2004.
  - Console commands: `Say <text>`, `TeamSay <text>`, `SwitchTeam [0|1]`, `Ready`.
  - Your character, sex, preferred team and voice (Settings > Player) are sent in the login URL like UT2004 does.
  Details and measurements: `Docs/UE2Net.md` "Step 4".
- The client logs in and loads the content cache's converted copy of the server's map. You see the match: the players with their own UT2004 characters and team skins, running,
  shooting, dying (ragdolls, gibs) and driving ONS vehicles; projectiles, beams and bullet impacts; pickups taken and
  respawning; lifts and doors moving; CTF flags carried and dropped, DOM points changing owner. The HUD and
  scoreboard show the server's names, scores, deaths, teams, score limit and remaining time; kill lines, chat,
  UT2004's local messages (first blood, sprees, countdown, flag messages), the server's sounds and the announcer.
- Onslaught: power nodes and cores follow the server (neutral, under construction, active, destroyed, team, health,
  under attack) on the map, the HUD minimap and the core health; vehicles are shown with their own models and team
  skins, wheels turning and steering, their turrets aiming where the server says they aim, muzzle flashes and tracers
  when they fire, drivers and gunners in their seats, the wreck when one is destroyed. Assault objectives follow
  the server (active, completed, health) with the round time; the bombing run ball is carried, thrown, dropped and
  returned as on the server. The messages of these modes (ONS node and core messages, bombing run, Assault) and of
  mods are shown with the message class's own text and look (the class's UnrealScript `GetString` is run).
- Following a player shows their health in the bar next to their score (what the server sends a spectator; ammo,
  weapons and adrenaline go to that player only).
- **Spectator controls**: Fire views the next player (the server's `ServerViewNextPlayer`, the camera behind them as in
  UT2004), Alt-Fire switches to the free camera and back. The bar at the bottom shows "Now viewing <name>" and their
  score, or in the free camera "Spectating <server> - Fire: view a player, Alt-Fire: free camera". The free camera's
  position is sent to the server (it moves the server's spectator), so players near the camera are replicated.
- Packages: the server's packages are matched with your UT2004 install by GUID. Missing ones (mods such as 3SPN /
  UTComp, custom maps and their textures) are **downloaded while joining**. The server's HTTP redirect (`.uz2` files)
  is tried first, then the server itself (the UE2 file channel). The server browser shows the progress over the server
  list. Each file is checked (package GUID, size, safe file name; only package files, never native code) and kept
  in a per-user net cache, `NetCache/` in the game's user data folder, by GUID. Downloaded maps and texture packages
  are converted into the content cache like custom content, so the server's custom map is loaded. Mod script
  packages are read to decode the server's mod actors; what their client code does for a player is done natively:
  UTComp's **NewNet** weapons (lag compensated: our client sends their timestamped fire, so hits register where you
  saw the target), **hit sounds** (UTComp, UTPlus; the sounds are converted from the mod's package the first time
  they play), UTComp's **warmup** (`Ready` / `NotReady` in the console, a warmup line on the HUD). Mod weapons show
  as their stock weapon. Mod HUDs and scoreboards are not run. A map whose name matches ours but whose GUID differs
  joins without a map rather than showing the wrong level. Packages that cannot be downloaded
  are skipped: actors that need them are not shown. UT2004's own packages (the files in the install's
  `System/Manifest.ini`) are never downloaded: when they are
  missing (install not found) they are skipped, with a notice in the connecting status and the spectator bar, and a server with another version of UT2004's code
  ends the join with "Version mismatch". A download that would take longer than 30 minutes at the speed the server
  allows ends the join with a message saying so. Details in `Docs/UE2Net.md` "Downloads".
- Map changes on the server are followed automatically (the client reconnects). Leaving the map or quitting
  disconnects cleanly.
- Login (AUTH) matches the OldUnreal 3374 client. With a CD key in your install, only its MD5 is sent. Without one
  (OldUnreal installs have none), a random per-install id (`NetIdentity` in the settings) is made on the first join
  and only its MD5 is sent, like the unique key the OldUnreal client makes for itself. The stats account goes only to
  servers that track stats, with Track Stats on, and the password is encrypted the same way. Keys, ids and hashes are
  never logged. Details in `Docs/UE2Net.md` "CD key and AUTH".

Test hooks: `-UE2Connect=<address>` joins at startup, `-UE2NoCDKey` ignores an installed CD key (tests the no-key login), `-UE2TestStats` uses a throw-away stats account, `-UE2NetTrace` logs every decoded actor, property and function
call, and `-UE2NetTraceBits` adds raw bits and the replicated field tables. `-UE2NetSpectateScript="8:next;30:alt;40:scores"`
presses fire (`next`) / alt fire (`alt`), shows the scoreboard and logs the scores (`scores`, `noscores`), or views the
bombing run ball (`ball`: its carrier, else the free camera at the ball) at those seconds; the log has a `UE2NetWorld summary` every 30 s (actors per kind, projectiles, pawn smoothing, unknown
classes). Downloads: `-UE2NetCache=<dir>` uses another
net cache, `-UE2NetNoRedirect` downloads everything through the file channel, `-UE2NetDownloadTimeLimit=<s>` changes the
download time limit, and `-UE2NetDownloadSelfTest` checks the
file name rules and `.uz2` unpacking. Playing (step 4):
- `-UE2NetPlayScript="5:route=pickups;5:hunt=1;20:dodge=l;30:dj;40:slot=2;60:suicide;63:fire"` scripts the local
  pawn. It can move, turn, jump, double jump, dodge, fire, switch weapons, restart, use, change team, ready, combo and
  chat (chat only to a server on this machine). `route=pickups` / `route=flag` walk the nav graph; `hunt=1` aims at the
  nearest visible player and fires (`hunt=tap`: one press per shot). `goto=water|lift|vehicle:ONSRV|x,y,z` walks there (and gets into the vehicle),
  `use` gets in and out, and driving sends `ServerDrive`.
- `-UE2NetCapsuleCollision` / `-UE2NetNoBackoff` turn the UE2 box collision / the 2 UU `MoveActor` back-off off
  (A/B). The test server's `TestBots` mutator takes `?TestAdrenaline=1`, which keeps human players at 100 adrenaline
  for combo tests.
- `-UE2NetPlayProbe` (local servers) logs the server's pawn state next to ours for every move.
- `-UE2NetLag=<ms>` adds round trip time. `-UE2NetNoNewNet` sends the stock `ServerStartFire` for NewNet weapons
  (no lag compensation, A/B).
- `-UE2NetNoSpectatorMoves` turns the spectator's moves off.
- The log has a `UE2NetPlay summary` every 30 s: moves, corrections, average and maximum error.

Test only against a local server (`Scripts/ut2004-test-server.sh`, see `Docs/UE2Net.md`).

## Requirements

- Unreal Engine 5.8 (binary or source build) with Linux, Windows or Mac target support installed
- A C++ toolchain for your platform (the bundled clang toolchain on Linux, Visual Studio 2022 on
  Windows, Xcode on macOS)
- The `PythonScriptPlugin` and `EditorScriptingUtilities` plugins, which the project enables. They are
  only used in the editor for content generation.
- A GPU and driver with Vulkan support (Linux and Windows), or a Metal-capable Mac

## Graphics / RHI

| Platform | RHI | Notes |
|----------|-----|-------|
| Linux    | Vulkan (SM6, SM5) | Vulkan is the only RHI UE supports on Linux. |
| Windows  | Vulkan (default)  | `DefaultGraphicsRHI_Vulkan`; the D3D shader formats are removed from the targeted list. |
| macOS    | Metal (SM5, SM6)  | UE has no Vulkan RHI for Mac, so the Mac build uses Metal. |

The settings are in `Config/DefaultEngine.ini`.

### Upscaling and low latency

Settings rows (main menu and pause menu, saved in `UArenaUserSettings`, applied live by
`Source/OpenUT2k4/ArenaUpscalerSettings.*` through the `Plugins/ArenaUpscaling` plugin):

| Row | Values |
|-----|--------|
| Upscaler | TSR (default) / AMD FSR 3.1 / Off (native, FXAA) |
| Upscaler Quality | Auto (Unreal's display-based default) / Native AA 100 % / Quality 67 % / Balanced 59 % / Performance 50 % / Ultra Performance 33 % |
| FSR Sharpness | Off, 10 % .. 100 % (RCAS, default 20 %) |
| Low Latency | Off / On. "On (AMD Anti-Lag)" with a native `VK_AMD_anti_lag` (AMD driver, RADV in Mesa 26.3+); "On (frame pacing)" otherwise |

- FSR 3.1.4 is compiled from the MIT FidelityFX SDK v1.1.4 shader source by Unreal's shader compiler, so it runs on
  Vulkan, D3D12 and Metal on any SM5 GPU. It replaces TSR as a third-party temporal upscaler. HUD and menus are drawn
  afterwards at native resolution.
- FSR 4 and frame generation are not available, and the reasons are documented: FSR 4 is DX12-only signed Windows
  DLLs under a binary-only licence, and FSR 3 frame generation needs a swapchain replacement that UE's Vulkan RHI
  cannot take. Mesa 26.2's anti-lag layer loses the device with Unreal, so it is never used. Research, sources and
  details are in `Docs/Upscaling.md`.
- Test switches (non-shipping): `-ArenaUpscaler=TSR|FSR|Off`, `-ArenaUpscalerQuality=0..5`, `-ArenaFSRSharpness=0..1`,
  `-ArenaLowLatency=0|1` (all builds), `-ArenaMesaAntiLagLayer=1`. Console: `r.Arena.FSR`, `r.Arena.FSR.Sharpness`,
  `r.Arena.LowLatency`, `r.Arena.LowLatency.LogInterval` (the log prints the input-to-present time).

Measured on DM-Rankin (3 bots, spectator camera, Lumen with hardware ray tracing) on an RX 9070 XT, Mesa 26.2.4 RADV,
Linux/Wayland, editor `-game` Development build, windowed, with no other GPU load. "UE" columns cover world seconds
12 to 40 (`-ArenaFrameStats`). MangoHud columns cover its first 30 s log (100 ms samples). The camera follows bots,
so the scene differs between runs. Low latency used the frame pacing fallback, because Mesa 26.2 has no usable
anti-lag (see `Docs/Upscaling.md`).

| Resolution / mode | UE fps | UE GPU ms (avg / p99) | MangoHud avg fps / 1 % low | MangoHud frame time avg / p99 | input-to-present |
|---|---|---|---|---|---|
| 1080p Off (native, FXAA) | 149 | 5.93 / 6.87 | 154 / 129 | 6.50 / 7.76 ms | 14.9 ms |
| 1080p TSR Quality | 160 | 5.54 / 6.41 | 159 / 125 | 6.28 / 8.01 ms | 13.8 ms |
| 1080p TSR Performance | 175 | 4.98 / 5.82 | 179 / 142 | 5.57 / 7.02 ms | 12.9 ms |
| 1080p FSR Quality | 177 | 4.97 / 5.86 | 180 / 148 | 5.55 / 6.74 ms | 12.4 ms |
| 1080p FSR Performance | 191 | 4.55 / 5.41 | 136 / 133 | 7.37 / 7.53 ms | 12.4 ms |
| 1080p FSR Quality + Low Latency | 157 | 5.00 / 5.88 | 159 / 103 | 6.30 / 9.73 ms | **5.8 ms** |
| 4K Off (native, FXAA) | 68 | 13.23 / 15.00 | 55 / 58 | 18.35 / 17.33 ms | 30.4 ms |
| 4K TSR Quality | 77 | 11.41 / 12.88 | 79 / 66 | 12.66 / 15.07 ms | 27.3 ms |
| 4K TSR Performance | 96 | 9.03 / 10.11 | 97 / 85 | 10.37 / 11.81 ms | 22.0 ms |
| 4K FSR Quality | 89 | 9.87 / 10.83 | 92 / 79 | 10.87 / 12.70 ms | 23.4 ms |
| 4K FSR Performance | 111 | 7.42 / 8.47 | 116 / 102 | 8.65 / 9.82 ms | 19.3 ms |
| 4K FSR Quality + Low Latency | 89 | 9.49 / 10.63 | 66 / 78 | 15.13 / 12.79 ms | **5.8 ms** |

FSR 3.1 at the same render resolution costs less GPU time than TSR: about 1.5 ms less at 4K Quality and 1.6 ms less
at 4K Performance. Its image is sharper (RCAS 20 %). Low Latency (frame pacing) cut the input-to-present time from
23 ms to 6 ms at 4K at the same GPU time, at the price of somewhat less even CPU frame times at 1080p. HUD and menus
are pixel-identical in every mode.

### Display: wide / ultrawide screens, window modes, HDR

Settings rows (saved in `UArenaUserSettings`, applied live by `Source/OpenUT2k4/ArenaDisplay.*` and `ArenaHDR.*`):

| Row | Values |
|-----|--------|
| Field of View | 80 .. 130, UT2004's definition: horizontal degrees **on a 4:3 screen** (UT2004's DefaultFOV 90 is the default). The value text shows the horizontal angle on this screen, e.g. `90 (106 wide)` at 16:9 |
| Monitor | Automatic / each display (fullscreen on the chosen output, also on Wayland) |
| Window Mode | Fullscreen / Borderless (default) / Windowed |
| Resolution | Fullscreen: the display's modes (SDL on Linux, the RHI's list elsewhere). Windowed: those plus 16:9, 16:10, 21:9 (2560x1080, 3440x1440, 3840x1600, 5120x2160) and 32:9 (3840x1080, 5120x1440, 7680x2160) sizes that fit the display. Borderless: always the desktop size |
| HDR | Off / On; "Off (not supported by display)" when the swapchain or the display cannot show HDR |
| HDR Peak Brightness | Auto (from the compositor's reported headroom, else 1000) / 400 .. 4000 nits; drives the HDR tonemap curve |
| HDR Paper White | 80 .. 400 nits (default 200): brightness of SDR white for the scene, the HUD and the menus |

Ultrawide FOV rules (Hor+):

- The engine keeps the vertical FOV (`[/Script/Engine.LocalPlayer] AspectRatioAxisConstraint=AspectRatio_MaintainYFOV`
  in `Config/DefaultEngine.ini`); no camera constrains the aspect ratio, so nothing is letterboxed.
- The FOV setting keeps its vertical extent on every screen: 90 is about 106 degrees horizontal at 16:9, 100 at 16:10,
  121 at 21:9 and 139 at 32:9. The horizontal angle is capped at 150 degrees (`ArenaDisplay::MaxHorizontalFOV`); past
  that (high FOV on 32:9 or multi-monitor-wide windows) the view loses a little height instead of stretching further.
- Weapon zoom (lightning gun scope) is a 4:3 FOV as well and is converted the same way.
- The first-person weapon uses each weapon's UT2004 DisplayFOV (also 4:3 horizontal) as a vertical extent relative to
  the camera (`AArenaWeapon::ApplyViewFOV`), so the gun has the same size on 16:9, 21:9 and 32:9 and wider screens
  only show more around it. Muzzle flashes and beams start at the drawn muzzle (`TransformWorldToFirstPerson`), which
  does not depend on the aspect ratio.
- Old saves stored the FOV as horizontal degrees at 16:9; they are converted once (`FOVVersion`), e.g. 100 becomes 85.

HDR requirements (details, engine internals and sources in `Docs/HDR.md`):

| OS / compositor | Needs |
|---|---|
| Linux, Wayland | A compositor with the `wp_color_manager_v1` protocol and HDR on for the monitor: KDE Plasma 6.3+, GNOME 48+, labwc 0.20 / Sway 1.12 on wlroots 0.20.1+ (Vulkan renderer), or gamescope `--hdr-enabled`. Mesa 25.1+ (or NVIDIA 595+) so Vulkan offers the HDR10 colour space. X11 / XWayland: no HDR |
| Windows | "Use HDR" on in Windows display settings; the Vulkan driver then offers HDR10 (not tested in this pass) |
| macOS | Metal EDR through the engine (the toggle is always offered; not tested) |

- UE 5.8's Vulkan RHI can present HDR10 (ST 2084 PQ, BT.2020) but never reports HDR support, so the game probes the
  window's Vulkan surface itself and enables the engine's HDR path (`r.HDR.EnableHDROutput`,
  `r.HDR.Display.OutputDevice` 3/4, `r.HDR.Display.ColorGamut` 2, `r.HDR.Display.MaxLuminance`, `r.HDR.PaperWhite`)
  and recreates the swapchain, live. HUD and menus (canvas, bitmap fonts) are SDR content composited at paper white
  (`r.HDR.UI.CompositeMode 1`). The Brightness setting is an exposure bias before the tonemapper and works in both.
- The log shows `HDR: Vulkan surface offers ...`, `HDR: ... display HDR on/off`, `HDR: output on (peak ..., paper
  white ...)` and the RHI's `Creating new VK swapchain with ..., VK_FORMAT_A2B10G10R10_UNORM_PACK32,
  VK_COLOR_SPACE_HDR10_ST2084_EXT`.
- Screenshots taken with HDR on contain the PQ-encoded image saved as if it were sRGB, so they look washed out.
- Switches: `-ArenaHDR=0|1` (all builds, overrides the saved choice for the run). Non-shipping: `-ArenaHDRToggle=<sec>[,<sec>]`
  flips HDR live at those times, `-ArenaFOV=<deg>` sets the FOV for the run, `-ArenaWindowTest=<sec>` logs aspect,
  FOV, window mode and the resolution lists. Window mode and resolution follow `-windowed` / `-fullscreen` /
  `-ResX=` / `-ResY=` when given (the settings then leave the window alone).

## Building

Each build script takes the engine root as its first argument or from the `UE_ROOT` environment variable.
If `Content/Materials` does not exist yet, the script first builds the `OpenUT2k4Editor` target, then runs
the content generator, then runs `RunUAT BuildCookRun` (Shipping, cook, stage, pak, IoStore, archive).
Packaged builds are written to `Dist/<Platform>/`. The dedicated server has its own scripts
(`Server/build-linux.sh`, `Server/build-mac.sh`, `Server/build-windows.bat`, output `Dist/Server-<Platform>/`;
see "Dedicated server" and `Server/Docs/DedicatedServer.md`).

### Linux

```sh
Scripts/build-linux.sh /path/to/UE_5.8          # or: UE_ROOT=/path/to/UE_5.8 Scripts/build-linux.sh
```

The default engine path is `~/UnrealEngine/Linux_Unreal_Engine_5.x`. The output goes to `Dist/Linux/`.

### Windows

```bat
Scripts\build-windows.bat "C:\Program Files\Epic Games\UE_5.8"
```

The default engine path is `%UE_ROOT%`, then `C:\Program Files\Epic Games\UE_5.8`. The output goes to `Dist\Windows\`.

### macOS

```sh
Scripts/build-mac.sh "/Users/Shared/Epic Games/UE_5.8"
```

The output goes to `Dist/Mac/`.

### Regenerating content

To recreate or refresh the generated assets on Linux, run:

```sh
Scripts/generate-content.sh [UE_ROOT]            # OPENUT2K4_BUILD_EDITOR=1 also builds OpenUT2k4Editor first
```

This runs:

```sh
UnrealEditor-Cmd OpenUT2k4.uproject -run=pythonscript -script=Tools/GenerateContent.py \
    -unattended -nop4 -nosplash -nullrhi
```

The script is idempotent. Running it again rebuilds the materials and re-imports the sounds in place. It creates:

- `/Game/Materials/M_ArenaSurface`: an opaque, lit material with a world-aligned grid. Parameters: `Color`, `Emissive`,
  `Roughness`, `Metallic`, `GridScale`.
- `/Game/Materials/M_ArenaGlow`: an unlit, additive, two-sided material with a soft fresnel falloff. Parameters: `Color`,
  `Intensity`, `Opacity`.
- `/Game/Materials/M_ArenaSprite` / `M_ArenaSpriteAlpha`: unlit, two-sided, additive / translucent textured effect
  materials for the imported-art effects. Parameters: `Base`, `Tint`, `Intensity`, `Opacity`, flipbook `Columns`,
  `Rows`, `Frame`, `TileU`, `TileV`, `PanU`, `PanV` (and `UseAlpha` on the additive one).
- `/Game/Materials/UT2004/M_Imported{Opaque,Masked,Translucent,Additive,Modulate}` and `_2S` (two-sided) variants:
  our own master materials for UT2004 content built at runtime (one `Base` texture with `UVTransform` /
  `UVOffsetPan` / `Tint`, `Opacity`, `Emissive`, `EmissiveAlpha`, `Roughness`, `Specular`; `Intensity` on the unlit
  ones), the same graphs as the editor importer's `/Game/Imported/Materials/Masters`. Usage flags (skeletal meshes,
  morph targets, instancing, Niagara) are set here because cooked builds cannot add them.
  `OPENUT2K4_GENERATE_ONLY="ut2004"` rebuilds only these.
- `/Game/Audio/S_*`: 36 sound effects synthesised in pure Python (16-bit mono, 44.1 kHz).
- `/Game/Maps/MainMenu` and `/Game/Maps/Arena`: empty levels whose game mode overrides are
  `ArenaMenuGameMode` and `ArenaGameMode`.

The game falls back to engine assets if the generated content is missing.

## Local UT2004 placeholder maps

If you own Unreal Tournament 2004, you can convert maps from your own install into local placeholder
levels. They are only for local testing until the project has its own art. Nothing from the game is ever
committed: `ImportCache/`, `Content/Imported/` and `Content/ImportedData/` are git-ignored.

```sh
Scripts/import-ut2004.sh DM-Rankin DM-Deck17 CTF-BridgeOfFate   # or: --all-exported, --force
Scripts/import-ut2004.sh --everything                            # the whole install (hours), resumable
```

The script runs `Tools/ut2k4_export.py` for maps that are not in `ImportCache/` yet (the game folder comes from
`UT2004_DIR`), then runs `Tools/ImportUT2004.py` in the editor. That creates textures, material instances of the
`/Game/Imported/Materials/Masters/M_Imported*` masters, static meshes, BSP and terrain chunks, and the level
`/Game/Imported/Maps/<Map>`. It also writes the gameplay markers to `Content/ImportedData/<Map>.json`.
Each UT2004 material becomes one instance of a master with a single `Base` texture.
`Tools/ut2k4/matresolve.py` picks the texture that carries the colour:
- Shader: Diffuse, then SelfIllumination.
- Combiner: chosen by CombineOperation. Lightmaps, colour masks, detail and flat textures are skipped, and
  AlphaBlend_With_Mask follows the mask coverage.
- FinalBlend, TexModifier, MaterialSwitch and MaterialSequence: follow their material.

It also collects the TexScaler / TexPanner / fixed TexRotator UV transform (`UVTransform`, `UVOffsetPan`), the tint
of a Combiner Multiply with a flat colour, self-illumination (`Emissive`, `EmissiveAlpha`) and specular
(`Roughness`, `Specular`). `Tools/ut2k4_materials.py` (plain `python3 -I`, run by the script after an export)
samples the exported textures (`ImportCache/texture_stats.json`) and bakes Combiner Adds of two plain textures,
such as the Onslaught team skins (diffuse plus team colour layer), into `<Combiner>_Baked` textures. It also
writes `Saved/ImportUT2004/material_audit.json` and the list of changed instances (`material_fixes.json`).
`Scripts/import-ut2004.sh --fix-materials` runs it and then rewrites the existing material instances in place.
Meshes and levels pick up the result without being imported again.
Imported maps appear in the main menu's layout list. Our gameplay classes replace UT2004's: weapon bases,
lockers and pickups become our weapons and pickups, lifts become jump pads, and CTF flag bases become our flags.
You can also open an imported map directly: `OpenUT2k4 "/Game/Imported/Maps/DM-Rankin?Mode=DM?Bots=5"`.

### Content converter (players)

Release builds ship no UT2004 data. On first start, when runtime content is in use and the content cache is not ready
(`ArenaContent::IsCacheReady()`), the main menu switches to the **UT2004 Content** screen
(`Source/OpenUT2k4/ArenaContentSetup.*`). The main menu entry "UT2004 Content" opens the same screen later, for
example to convert again or to use another copy. It is drawn like the main menu (the UT2004 menu backdrop, window,
header and buttons once converted art exists; the menu's procedural look before) and shows one state at a time in
plain language: UT2004 not found, UT2004 found, converting (progress bar, time left, Cancel), done, failed, paused,
ready. Technical text (searched folders, cache folder, converter problems, log file) is behind "Show details"; problems
are summarised ("3 items could not be converted - details in convert_errors.log"). The footer carries
`ArenaBranding::Disclaimer()`.

The install is detected (`ArenaContent::FindUT2004Install`: `-UT2004Dir=`, `$OPENUT2K4_UT2004_DIR`, `UT2004Dir` in
Game.ini, a `UT2004` folder next to the game, common install places) or chosen with **Browse...**, a native folder
dialog (`Source/OpenUT2k4/ArenaFolderPicker.*`, no DesktopPlatform, works in Shipping): on Linux the engine's SDL3
`SDL_ShowFileDialogWithProperties` (xdg-desktop-portal FileChooser on Wayland/X11, KDE/GNOME, Flatpak; else zenity),
then kdialog / zenity started directly; `IFileOpenDialog` + `FOS_PICKFOLDERS` on Windows; `NSOpenPanel` on Mac. The
dialog never blocks the game thread (callback on the game thread), and a fullscreen window drops to a window while
it is open. **Type a path...** is the fallback (text field with Tab completion and Ctrl+V; also used automatically when
no dialog exists). `AArenaContentSetupHUD::CheckInstallFolder` validates the choice and corrects near misses (the
System or another subfolder picked: the install around it; the folder above picked: the install one or two levels
down) or explains what is wrong; the result is saved in `UArenaUserSettings::UT2004Dir` and Convert is offered.
A cancelled or interrupted run resumes where it stopped. Afterwards the map list is refreshed and Continue returns
to the menu. Other code can add blocks or buttons to the screen with `AArenaContentSetupHUD::RegisterExtension`
(optionally modal, e.g. the Terms of Service step) and start a conversion with `ContinueWithInstall`.

The converter (`Source/OpenUT2k4/Convert/`, entry point `FArenaConverter` in `ArenaConverter.h`) is a C++ port of
`Tools/ut2k4_export.py` and its modules, the cache-side parts of `Tools/ut2k4_materials.py` (`texture_stats.json`
and the `_Baked` Combiner textures) and the game-data part of `Tools/ImportUT2004.py`. It writes the same files as
`ImportCache/` into `ArenaContent::GetCacheDir()`, plus `data/<Map>.json` and `data/Assets/*.json` (the same shape
as `Content/ImportedData`, with asset paths still written `/Game/Imported/...`), and finally `cache.json`. The object
graph walk runs on one thread; pixel decoding, PNG compression and file copies run as task-graph jobs. On a 16-thread
machine the whole install takes about 95 s and 5.8 GB. Per-asset problems go to `<cache>/convert_errors.log` (empty for the retail
install). `<cache>/convert_notes.log` lists what the original data itself lacks: 5 dangling references (objects
that no UT2004 package contains; the retail game leaves them empty too) and 6 textures whose pixels were stripped
from their package (they get a flat stand-in in their stored average colour, `MipZero`). Beyond the Python
exporter, the converter decodes textures whose top mips were stripped (`mip_used` in the json), resolves imports
whose object moved to another group or to the `X_<Package>` copy, reads `ADrop` structs (WetTexture),
treats ProceduralSound as BaseSound + settings, and rebuilds skeletal meshes that only kept the packed
soft-skinning stream (`geometry_source` `lodN-packed-soft`, e.g. the Outback kangaroo). The resume state is in
`<cache>/convert_state.json`. The Python tools stay in `Tools/` as the developer reference and are
not shipped.

Developer and test switches (also listed in `ArenaConverter.h`):

```sh
# convert headless and quit; compare structurally with a Python-made ImportCache (report: <dir>/compare_report.txt)
UnrealEditor OpenUT2k4.uproject -game -nullrhi -ConvertOnly -ConvertTo=<dir> [-ConvertMaps=DM-Rankin,CTF-Face3] \
    [-UT2004Dir=<install>] [-ConvertForce] [-ConvertCompare=ImportCache [-ConvertCompareData=Content/ImportedData]] \
    [-ConvertCompareOnly]
# first-run flow without input: -RuntimeContent -ContentCache=<dir> -AutoConvert [-ConvertMaps=...]
#   -AutoSetupShots=<sec>,... (Saved/Screenshots/<platform>/setup_N.png) -AutoSetupQuit=<sec>
# setup screen states: -SetupOpen (open even when the cache is ready), -SetupChoose=<folder> (as if browsed),
#   -SetupConvert (convert it), -SetupDetails, -SetupTypePath=<text>, -SetupBrowse [-SetupBrowseStart=<folder>] (opens the real folder dialog)
```

### Downloading UT2004 from OldUnreal (players without an install)

When no install is found, the UT2004 Content screen offers **Download UT2004 from OldUnreal**
(`Source/OpenUT2k4/ArenaContentDownload.*`, registered as a setup screen extension). It does what OldUnreal's
official installer (`FullGameInstallers/Linux/install-ut2004.sh`) does, minus the patch and executables: the first
click shows the Epic Games Terms of Service notice and URL, and only "I agree - download" starts (the agreement is
stored in `UArenaUserSettings::AcceptedEpicTermsUtc`). It downloads `UT2004.ISO` (2.995 GB) from the shuffled
`files*.oldunreal.net` mirrors (archive.org as backup) with the engine HTTP module, streamed to disk in 256 MB Range
requests and resumable after a cancel or restart, checks size and SHA-256, then reads the InstallShield cabinets
straight out of the ISO (`Download/ArenaDlIso`: ISO 9660 + Joliet; `Download/ArenaDlCab`: a port of unshield, MIT,
see `Download/ThirdParty/`) and installs the groups the converter needs (Animations, Maps, Music, StaticMeshes,
Textures, Sounds, Speech, System without .exe/.dll/.bat, Help: 1133 files, 6.05 GB) into
`<user data>/OpenUT2k4/UT2004` (built in `UT2004.partial`, renamed when complete). The ISO (in `<user data>/Download`)
is deleted afterwards. Peak disk use about 9.1 GB. When done, the screen continues with the normal conversion.
On a fast connection: download about 2 minutes, SHA-256 7 s, install 45 s.

```sh
# headless (non-shipping builds); quits when done
UnrealEditor OpenUT2k4.uproject -game -nullrhi -DownloadUT2004 -AcceptEpicTerms [-DownloadTo=<install dir>] \
    [-DownloadDir=<folder for UT2004.ISO>] [-ISO=<local UT2004.ISO, skips the download>] [-KeepISO] \
    [-FullInstall] [-DownloadOnly] [-DownloadNoQuit]
# screenshot of the Terms of Service step: -DownloadShowTerms (with -AutoSetupShots)
```

### Custom content (community maps)

Players can add their own UT2004-format content (community maps, remakes, and the texture, static mesh, sound,
music and script packages they ship with) without touching their UT2004 install. Put the files in the **custom
content folder**:

| OS | Folder |
| --- | --- |
| Linux | `$XDG_DATA_HOME/OpenUT2k4/UserContent` (usually `~/.local/share/OpenUT2k4/UserContent`) |
| Windows | `%LOCALAPPDATA%\OpenUT2k4\UserContent` |
| macOS | `~/Library/Application Support/OpenUT2k4/UserContent` |

`-UserContent=<dir>` uses another folder. The game creates the folder (with empty `Maps`, `Textures`,
`StaticMeshes`, `Sounds`, `Music`, `System`, `Animations`, `KarmaData` subfolders) on first start and never changes
the files in it; the setup screen's **Custom content... > Open the folder** opens it.

- **Any layout works**: the UE2 folders, files dropped loose at the top level, or a whole unpacked download in a
  subfolder. Files are sorted by extension: `.ut2` maps; `.utx` `.usx` `.uax` `.ukx` `.u` packages; `.ogg` music;
  `.int` / `.ucl` map titles, authors and player counts (`[LevelSummary] Title=` of `<Map>.int`, the `Map=(...)`
  records of `.ucl` files; otherwise the map's own LevelInfo / LevelSummary); `.upl` character lists and `.umx`
  (UT99 tracker music) are recognised but not used.
- **Archives**: `.umod` / `.ut2mod` installers (own reader of the UE2 umod format: trailer, file table) and `.zip`
  (own reader using the engine's zlib; stored and deflate, a `.umod` inside a zip is unpacked too) are unpacked into
  `<cache>/usercontent/unpacked/<archive>/`, never into the folder itself. `.7z` and `.rar` are not read: the screen
  asks the player to unpack them.
- **Dependencies**: a custom map's packages are searched in the custom content folder first, then in the UT2004
  install (`FConvPackageManager::AddPackageFiles`). A custom file with the same name as a UT2004 package or map never
  replaces the UT2004 one (the shared content cache must stay valid for the stock maps): identical copies are
  ignored, a different package of the same name is reported ("UT2004's own ... is used"), and a map with a stock
  map's name has to be renamed.
- **Incremental conversion** (`Source/OpenUT2k4/ArenaUserContent.*`, `Convert/ArenaConvUserContent.*`,
  `Convert/ArenaConvArchive.*`): on startup (when the cache is ready and the folder changed: a cheap size / date
  check), after a full conversion and with **Rescan custom content** on the UT2004 Content screen, only new or
  changed files are processed. Every file gets size + mtime + MD5 in `<cache>/usercontent/state.json` (MD5 only when
  size or date changed); a map is converted again only when its own file, a custom package it uses (transitively),
  its `.int` / `.ucl` file or the converter's map schema changed. Cache entries of deleted maps and packages
  (`maps/<Map>`, `data/<Map>.json`, the `<kind>/<Package>` folders, custom music) are removed, `texture_stats.json`
  is merged instead of rebuilt, and only the converted maps get new `data/<Map>.json` files. Measured with 7 test maps (stock
  maps copied under new names, one with a renamed texture package, one in a .umod, one in a .zip, one with a missing
  package): converting 5 new maps took 0.7 s (0.05 - 0.3 s per map), a startup scan converting one new map 0.34 s in all, a rescan
  with nothing changed 0.06 s, a rescan after replacing a package (its map is converted again), adding a map and
  deleting one 0.6 s.
- **Older converted maps**: when the map exporter's schema (`ArenaConverterVersions::MapSchemaVersion`) is newer
  than a cached `map.json` (stock or custom), the same pass exports that map again from the install
  ("Updating converted maps..." on the setup screen), so earlier caches get new map features without a full
  conversion (all 239 stock maps from schema 3 to 4: 41 s, plus 8.5 s for their marker files). Maps that cannot be updated are remembered and not retried until the schema changes again.
- **Menu**: custom maps appear in the Instant Action map list for the modes their markers support, marked
  `[custom]`, with title, author, recommended players and the LevelInfo screenshot next to the panel.
- **What cannot be played is named**: the custom content page lists every map as ready / plays with limits /
  cannot be played with the reason: missing packages by name ("Missing package: MissingPkg123. Put it in the
  custom content folder."), damaged files, no player starts, conversion errors (`<cache>/usercontent/errors.log`).
  Unplayable maps are left out of the map list. Maps with objects whose class comes from a custom `.u` package or
  from the map itself (embedded UnrealScript) play with limits ("Uses custom scripts (not supported yet): ...")
  and are marked `[custom*]` unless the UnrealScript runtime can run those classes: it answers
  `ArenaUserContent::ScriptClassSupport()` with `ArenaUScript::CanRun()` (see [UnrealScript runtime](#unrealscript-runtime)).

```sh
# scan + convert once without the menu, log the result per map, quit (-UserContentForce: convert every custom map again)
UnrealEditor OpenUT2k4.uproject -game -nullrhi -RuntimeContent -ContentCache=<dir> -UserContent=<dir> -UserContentScan
# open the setup screen on the custom content page: -SetupOpen -UserContentShowList (with -AutoSetupShots=...)
# no scans at all: -NoUserContent
```

### Maps

The game has no maps of its own (the original procedural layouts are gone); every layout is a UT2004 map, with id
`Imported:<Map>` (`?Layout=` also takes the bare map name). The list comes from the marker json files
(`AArenaLevelBuilder::GetImportedDataDir()`: `<cache>/data` with runtime content, `Content/ImportedData` with
cooked imports; a developer cache without `data/` falls back to `Content/ImportedData`) for which the map's geometry
exists: `<cache>/maps/<Map>/map.json` (runtime) or the level `/Game/Imported/Maps/<Map>` (cooked).

- **Runtime content** (`ArenaContent::UseRuntimeContent()`, release builds or `-RuntimeContent
  -ContentCache=<dir>`): the menu opens `/Game/Maps/Arena?Layout=Imported:<Map>`, and `AArenaGameMode::InitGame`
  builds the map with `ArenaRuntimeMap::BuildWorld` (`Source/OpenUT2k4/ArenaRuntimeMap.*`) the way
  `Tools/ImportUT2004.py` builds the cooked level: BSP and terrain in 60 m XY chunks with complex-as-simple collision
  (NotSolid polygons don't collide, TwoSided / Unlit polygons use those material variants), static mesh actors with
  pre-pivot / draw scale transforms, skins and collision flags (movers, volumes, triggers, emitters and the sky box are native UE2 world actors, see below;
  meshes with `bBlockNonZeroExtentTraces=False` stop shots and sight but players walk through them, as in UT2004, e.g. the
  charger ring around DM-Morpheus3's jump pad), UE2 collision that is never drawn (see "World collision" below), UT2004 lights
  as unshadowed movable point / spot lights, and a sun, sky light, atmosphere, height fog and post process volume.
  Meshes and materials come from the runtime asset provider (`ArenaRuntimeContent`). Then `BuildImported` (player
  starts, pickups, jump pads, anchors) and `ArenaImportedLighting` run as for cooked maps. The load blocks for a
  moment; the log line `RuntimeMap: built <Map> in N s (...)` breaks the time down.
- **Cooked imports** (developer builds with `Content/ImportedData`): the menu opens `/Game/Imported/Maps/<Map>`;
  opening `/Game/Maps/Arena` with a layout travels there.

Default map per mode when `?Layout=` is missing or not playable in the mode: DM / TDM / LMS / Mutant `DM-Rankin`,
CTF `CTF-Face3`, Bombing Run `BR-Colossus`, Double Domination `DOM-SunTemple`, Onslaught `ONS-Torlan`, Assault
`AS-Convoy`, Invasion `DM-Antalus`; when that map is not converted, the first map the mode supports. With no maps at
all the menu shows an empty state (Instant Action disabled) and a match URL returns to the menu.

### UE2 world behaviour (zones, volumes, movers, triggers, emitters, sky boxes)

Runtime maps behave like UT2004: after the geometry, `ArenaRuntimeMap::BuildWorld` calls
`ArenaUE2World::SpawnWorldActors` (`Source/OpenUT2k4/ArenaUE2World.*`), which spawns native C++ actors for the map's
UE2 world actors. Each one reads the UE2 values: the actor's own `properties` merged with its class defaults, which
the converter writes to map.json as `class_defaults` from the user's `.u` files (`FArenaUE2Props`). Every native actor
implements `IArenaUE2Actor` (`InitFromUE2`, `PostInitUE2`, `Trigger` / `UnTrigger`, `ResetUE2`).
`ArenaUE2World::RegisterClass` maps a UE2 class name to a native class (the most derived registered class in
`class_chain` wins), so script classes can subclass or replace them.

- **Event system** (`UArenaEventSubsystem`): UE2 `TriggerEvent` / `UntriggerEvent` by Tag. Assault objectives fire
  their UE2 `Event` when completed, TriggeredObjectives complete when their Tag fires, and every native actor is reset
  at the start of each Assault round.
- **Zones** (`ArenaWorldZones.*`): the converter exports the BSP tree (`bsptree.bin` plus `bsp.tree` in map.json), and
  `UArenaWorldZoneSubsystem::GetZoneAt` walks it like `UModel::PointRegion`. ZoneInfo properties apply per zone:
  KillZ (and bSoftKillZ), location names, sky zone.
- **Physics volumes** (PhysicsVolume, WaterVolume, LavaVolume, PressureVolume, xFallingVolume, kill volumes): these
  are engine `APhysicsVolume`s with the UE2 `PhysicsVolume` rules. Gravity is relative to -950 (e.g. DM-Morpheus3
  outdoors at -375) and is the full UE2 vector: falling pawns (`UArenaCharacterMovement::NewFallVelocity`), falling
  projectiles and thrown flags / balls (`AArenaCarryable::TickFlight`) get its X / Y part too, so the sideways and
  upward tubes of BR-Colossus, DM-TokaraForest or BR-Disclosure carry players. A pawn cannot stand in a volume whose
  Gravity.Z points up (Colossus tubes use +1): it floats. TerminalVelocity limits the falling speed (the downward Z
  part only; a sideways tube with TerminalVelocity 200 still carries at full speed). The level's
  DefaultPhysicsVolume TerminalVelocity applies outside volumes. Falling projectiles (UE2 PHYS_Falling: bio globs,
  grenades, flak shells, ONS grenades and spider mines; `ArenaWorldZones::NewFallVelocity`) are limited the same way,
  are slowed by FluidFriction in water and have no other speed cap; rockets and other PHYS_Projectile shots have
  neither. GroundFriction applies when walking, FluidFriction
  when swimming (and falling in water). ZoneVelocity is added to a pawn's move, not its velocity (walking: the
  horizontal part), as in UE2 physWalking / physFalling / physSwimming. bMoveProjectiles adds it once to projectiles
  entering (PhysicsVolume.Touch) and moves thrown flags / balls. bBounceVelocity makes a landing pawn bounce off
  with ZoneVelocity. Vehicles (Karma bodies) fall with the full Gravity vector of their volume times their
  KActorGravScale, sideways and upward parts included (a flown craft's lift cancels only the vertical part), and
  ZoneVelocity carries them like pawns. KExtraLinearDamping / KExtraAngularDamping slow them inside a volume (water
  2.5 / 0.4, lava 0.8 / 0.1 per second), and in water a vehicle's KBuoyancy x the volume's KBuoyancy of gravity pushes
  up (0 for the stock vehicles, which sink). UE2's Karma volume code is native, so the damping and buoyancy forces are
  an approximation from these documented properties. Membership follows the UE2 rule: the actor's
  location must be inside the converter-exported brush BSP, and the highest Priority wins. The subsystem assigns it
  to characters, projectiles and vehicles each tick. It also runs PainTimer (DamagePerSec / DamageType, "Lava" /
  "Fell" kills), bDestructive (projectiles), entry / exit sounds, drowning (20 s, then 5 damage every 2 s) and the
  camera's volume look: tint plus the height fog turned into the volume's distance fog (UT2004 blue water, red lava).
- **Jump pads** (`AArenaJumpPad`, placed by `AArenaLevelBuilder`): the trigger is the UE2 pad's own collision
  cylinder (CollisionRadius / CollisionHeight around the UE2 location, from map.json through
  `ArenaUE2World::FindJumpPad`; UE2 defaults 40 / 43 UU otherwise). A pawn touches it when its cylinder overlaps, and
  its path since the last frame is sampled, so a pawn shot out of a tube at 100 m/s still triggers the pad at the
  mouth. The launch is the map's UE2 JumpVelocity (`PostTouch: Velocity = JumpVelocity`); pads without one (converted
  lifts) solve an arc to the target. As in UE2 only pawns are launched (JumpPad.Touch ignores the ball and flags).
  Pads hanging in the air (tube mouths) draw no plate. Launches are logged
  (`JumpPad: <pawn> touched <UE2 pad> at ... (speed ...) -> launched ...`).
- **Blocking volumes**: invisible collision built from the brush faces. It blocks players, vehicles and physics
  objects, but not weapon traces or projectiles. bClassBlocker with vehicle classes blocks vehicles only.
- **Movers** (`ArenaMover.*`): keyframes (BasePos + KeyPos[i], BaseRot + KeyRot[i]), MoveTime per key and
  MoverGlideType. Supported states: BumpOpenTimed, BumpButton, StandOpenTimed (lifts), TriggerOpenTimed,
  TriggerToggle, TriggerControl, TriggerPound, TriggerAdvance (one key further per trigger, AS-Glacier's base doors)
  and ConstantLoop. Also StayOpenTime, DelayTime, MoverEncroachType
  (stop / return / crush / ignore, EncroachDamage), ReturnGroup, Opening / Opened / Closing / ClosedSound, the
  looping MoveAmbientSound, the matching events, bDamageTriggered, and PHYS_Rotating fans. Characters standing on a
  mover ride it. The converted "Lift:" jump pads are not spawned when native movers exist.
- **Bots** (`ArenaNavGraph`, `AArenaBotController::UpdateLiftRide`): vertical movers get Lift links from their bottom
  stop to the top, which exits to the map's LiftExits and nearby nodes. Bots wait until the lift is down, step on,
  stand still until it is up, then walk off. Teleporters get Teleport links. Nodes inside pain or kill volumes are
  never link targets. Jump pads get JumpPad links from the nodes on the pad to its landing (UE2 JumpTarget). Pads at
  the end of a physics volume that blows players along (sideways or upward Gravity, ZoneVelocity; BR-Colossus' goal
  return and side tubes) are entered in flight: they have no walking route in, as in UT2004's own path network, and
  are logged as `NavGraph: jump pad at ... is the exit of the <Volume> stream`; any other pad without links in or out
  is a `Warning` with the nodes around it and the nearest reached node. Drops and jumps use the gravity of the
  volume they start in, so low gravity maps (DM-Morpheus3 at -375) get the long drops and high jumps between
  buildings. Bot launches are logged (`JumpPad: bot ... (path link)` when the pad was on the bot's path). Test hook
  (non-shipping): `-NavPadTest` plans, for every pad link, a path from a few links before the pad to its landing and
  logs whether it goes through the pad (`NavPadTest: N of M pad links are on the planned path`).
- **Triggers** (`ArenaTriggers.*`): Trigger (TriggerType proximity / shoot, ReTriggerDelay, RepeatTriggerTime, the
  OtherTrigger* states, Message), Dispatcher (OutEvents / OutDelays), Counter, TriggeredCondition, Teleporter
  (URL -> Tag, bChangesYaw, bChangesVelocity / TargetVelocity, bReverses*, toggled by Trigger), and the basics of
  ScriptedTrigger. Supported actions: TriggerEvent, UntriggerEvent, WaitForEvent, WaitForTimer, PlaySound,
  GotoAction, DisplayMessage and DestroyActor. Other actions are skipped and logged with `-LogUE2Events`.
- **Emitters** (`ArenaEmitters.*`): `AArenaEmitter` simulates the Emitter's SpriteEmitter, SparkEmitter, BeamEmitter
  and MeshEmitter subobjects (exported inline as actor `subobjects`) and old xEmitters from their UE2 parameters.
  Each emitter is drawn as one dynamic mesh of camera-facing quads (`UArenaParticleComponent`) with
  `/Game/Materials/M_ArenaParticle{Add,Alpha,Mod}` (texture x vertex colour, generated by `Tools/GenerateContent.py`).
  Emitters pause when unseen (UE2 SecondsBeforeInactive) and beyond `arena.EmitterDistance` (25000 cm);
  `arena.Emitters 0` turns them off. **Ambient sounds**: AmbientSound actors, and the AmbientSound of any other
  actor, play with UE2 SoundRadius (audible within 25 * (SoundRadius + 1) units), SoundVolume and SoundPitch.
- **3D sky box** (`ArenaSkybox.*`): BSP surfaces seen from a SkyZoneInfo's zone are exported as `sky_zone` primitives.
  They, plus the static meshes and emitters in that zone, are shown only to six 90 degree scene captures at the
  SkyZoneInfo. A camera-centred cube with `/Game/Materials/M_ArenaSkyFace` far away shows the six faces, so the sky
  has no parallax, like UE2's. Height fog stops before the cube. Settings: `arena.SkyboxResolution` (512) and
  `arena.SkyboxRefresh` (1 s for a full refresh, one face at a time; 0 = once). Maps without a sky zone keep the sky
  atmosphere.

### World collision

UT2004 has collision nobody sees, and maps rely on it for floors and walls (CTF-Maul's upper decks are visual-only
tower meshes, `bBlockActors=False`, on top of hidden collision hulls). The runtime map builds all of it:

- **Invisible BSP surfaces** (`PF_Invisible` without `NotSolid` / `Portal`: invisible walls and floors made with
  brushes) still bound solid space in UE2. The converter writes them as `collision_only` primitives in `bsp.gltf` (no
  material; map.json `bsp.collision_only_nodes`), the runtime builds `BSPCollision_<Map>_NN` chunks that collide but are
  never drawn (tag `ArenaCollisionOnly`: no rendering, shadows or ray tracing). Sky-zone surfaces stay out.
- **Sky surfaces** (`PF_FakeBackdrop`) are solid in UE2 too: players cannot leave through the sky, and projectiles hit
  it. UT2004's `Projectile.HitWall` has no sky case (it hurts non-world walls, calls `Explode` and leaves a decal), so
  rockets explode on the sky, grenades and flak chunks bounce off it, hitscan stops at it. They are `collision_only`
  primitives with `fake_backdrop` (schema 8), built as `SkyBackdrop_<Map>_NN` chunks tagged `ArenaFakeBackdrop`, which
  `ArenaImportedLighting`'s openness probe looks through. Test hook (non-shipping): `-SkyShotTest` fires rockets straight
  up from the player starts that have sky above them and logs where each ends against the sky surface
  (`SkyShotTest: ... OK (exploded on the sky)`; DM-Morpheus3 6/6, CTF-Face3 5/5).
- **Hidden static mesh actors** (`bHidden=True`, e.g. `TowerStatic.Collision.*_col` on Maul) are placed as
  collision-only actors (if `bCollideActors` / `bBlockActors` allow), instead of being skipped.
- **Static mesh collision models** (`StaticMesh.CollisionModel`, a UModel of hulls the mesher built from brushes): UE2's
  `UStaticMesh::LineCheck` uses it for extent traces (players, vehicles, physics: `UseSimpleBoxCollision`, the default)
  and for zero-extent ones (shots, sight: `UseSimpleLineCollision`) instead of the collision-enabled sections; 129 stock
  and bonus meshes have no colliding section at all (e.g. DM-CBP2-Masurao's `far.rampsraight01` ramps) and many trees
  only collide with their trunk this way. The converter adds the model as a `collision_model` primitive (`box`, `line`
  extras; static mesh json `tool_version` 2, `collision.collision_model_triangles`); `ArenaRuntimeContent::
  LoadStaticMeshCollisionModel` builds it as a collision-only mesh; map mesh actors and movers (`AArenaMover::
  CollisionModel`: riders, encroachment and bump checks use whichever of the two pawns collide with) get it as a hidden
  component. Emitter mesh particles never collide, in UE2 neither.
- **Who collides with what** (`ArenaRuntimeMap::ApplyUE2MeshCollision`, map meshes and movers): extent traces (Pawn,
  WorldStatic floor probes, WorldDynamic) use the model when `UseSimpleBoxCollision`, else the colliding sections, and
  pass when the actor has `bBlockNonZeroExtentTraces=False`; zero-extent traces (Visibility, Camera, Weapon,
  Projectile) use the model when `UseSimpleLineCollision` and pass when `bBlockZeroExtentTraces=False` (e.g. grates
  players cannot pass but shots can); physics bodies (Vehicle, PhysicsBody, Destructible) collide with the sections,
  or the model when there are none.
- **Terrain** (`TerrainPrimitive`): UE2 splits each quad along (x, y) - (x + 1, y + 1), the other diagonal where
  `EdgeTurnBitmap` is set, and a vertex is `(h - 32767) x TerrainScale.Z / 256` above the actor (one height step above
  the documented `h - 32768`). Both were checked against UT2004 itself: the vertices it saves in every `.ut2`
  (226 of the 230 terrains on 133 maps match to 0.01 UU, 3 have no heightmap; DM-UCMP2-Dynarak's TerrainInfo1 is 20 UU off as a whole: its
  saved vertices disagree with its heightmap and location, not checked against a server) and a 3369 server's pawn heights on DM-DesertIsle (`-UE2NetPlayProbe`: the pawn rests
  2.5 UU above the surface under its centre on any slope, std 0.2 UU with this split, 2.8 with the other one).
  UE2 pawns stand on terrain with their centre point, while a capsule or box rests higher on a slope, so pawns collide
  with lowered copies of the terrain (tags `ArenaUE2Terrain` + `ArenaTerrainCapsule` / `ArenaTerrainBox`; each vertex
  lowered by what the shape adds on the slope there, not more than on the walkable quads around it, at most as on a
  50 degree slope; `UArenaCharacterMovement` ignores the copy for the other shape), everything else with the real
  surface. Test hook (non-shipping): `-TerrainPawnTest` drops a capsule and a box onto 4000 terrain points and logs how
  far above the surface under the centre they rest, by slope (UE2: 0). DM-CBP2-KillbillyBarn: capsule and box median
  within 1 UU up to 46 degrees; DM-DesertIsle (rough dunes): capsule 0 - 4, box 0 - 10 (from 1 - 23 before).

Floor check (developer tool, not shipped): a script over the content cache (BSP, terrain, blocking volumes, movers,
static meshes with the runtime's collision rules) counts player starts, pickups, path nodes and jump spots with no
colliding floor within 320 cm below, and a terrain check compares the runtime's terrain vertices and sampled heights
with the vertices saved in each `.ut2`. On the 239 stock and bonus maps the fixes above took the floor check from 47
maps / 1306 points to 32 maps / 179 points; what is left is mostly intended (Assault starts that spawn into space fighters on
AS-MotherShip, AS-BP2-Acatana and AS-BP2-Jumpship, nodes placed in mid air or inside geometry) or community maps whose
collision helpers have `bCollideActors=False` (DM-UCMP-BloodRun).

Converter: map.json schema 5 (`ArenaConverterVersions::MapSchemaVersion`) adds the BSP zone tree, sky zone
primitives, volume `brush` faces / planes / BSP, `subobjects`, `class_defaults`, and every asset inside the map
package; schema 6 adds `collision_only` BSP primitives, schema 7 re-exports the maps' static meshes with their UE2
collision model, schema 8 adds the sky surfaces as collision (see "World collision" above). Older caches are re-exported automatically. UE2 `Color` structs are stored B,G,R,A (cache format 2 fixed
swapped red / blue). Cooked levels (`Tools/ImportUT2004.py`) are unchanged and keep the static behaviour.

Test hooks (non-shipping): `-UE2Report` logs each character's volume, gravity, zone and movement base every 3 s,
jump apexes, and changes of the camera's volume. `-LogUE2Events` (or `arena.LogUE2Events 1`) logs events, mover
opens / closes and teleports. `-UE2View=x,y,z,pitch,yaw[;...]` with `-UE2ViewEvery=<s>` gives fixed screenshot
cameras. `-UE2PlaceOnMover=<MoverName>[,<s>]` and `-UE2PlaceBotAt=x|y|z|<s>` put the first bot on a lift, in water or
in a teleporter, with its AI off and at rest. With `-UE2Report` every volume change of a pawn is logged with its
velocity, and the 3 s lines name what the pawn would run into along its volume's gravity; `-UE2LogImpacts` logs
what a fast falling pawn hits. Tube transport on BR-Colossus:
`-UE2Report "-UE2PlaceBotAt=17|-2550|-11450|12"` (PhysicsVolume15 -> 1 -> 19 -> 13 -> UTJumppad6 -> JumpSpot60)
or `"-UE2PlaceBotAt=-28|-8700|-7644|12"` (PhysicsVolume10 -> UTJumppad2).

### Runtime asset provider

With runtime content every `/Game/Imported/<rel>` asset the game asks for is built from the content cache instead by
`ArenaRuntimeContent` (`Source/OpenUT2k4/ArenaRuntimeContent.h`, `ArenaRuntime{Textures,Materials,Meshes,Audio,Gltf}.cpp`).
`ArenaArt::LoadArt` still tries `/Game/Replacements/<rel>` first, then calls `ArenaRuntimeContent::LoadByRelativePath`;
nested references (a mesh's materials, a material's texture) also honour `/Game/Replacements`. Built assets are transient,
kept per session and released by `TrimCache()` on map change (`arena.RuntimeContentStats` logs counts, memory and time).
- Textures: PNG decoded with the engine's ImageWrapper, full mip chain, compressed to BC1/BC3 by a small block encoder on
  worker threads (`arena.RuntimeTextureCompression 0` keeps BGRA8); sRGB, world LOD group, wrap, like the editor import.
- Materials: a C++ port of `Tools/ut2k4/matresolve.py` plus the importer's instance rules picks master, `Base` texture and
  parameters; each material becomes a `UMaterialInstanceDynamic` of the shipped masters (two-sided ones use the `_2S`
  masters, since dynamic instances cannot override two-sidedness). `arena.DumpMaterialResolves` writes every cache
  material's resolution to `Saved/RuntimeMaterialResolves.json` for a diff against the Python resolver (identical on the
  full install). Runtime materials are shared: to change parameters per actor use
  `ArenaRuntimeContent::CreateMaterialInstance(Comp->GetMaterial(i), Comp)`, never `CreateDynamicMaterialInstance`
  (it returns the shared instance already in the slot, and a dynamic instance cannot parent another one).
- Static meshes: `UStaticMesh::BuildFromMeshDescriptions` fast build, one slot per (material, collides) pair,
  complex-as-simple double-sided collision from the colliding sections (the UE2 collision model is a separate
  collision-only mesh, `LoadStaticMeshCollisionModel`, see "World collision"), cooked by Chaos at runtime
  (`CookCollision` cooks a batch in parallel). No Nanite, lightmap UVs or distance fields. `BuildStaticMesh` takes
  generated geometry (BSP / terrain chunks).
- Sounds and music: `UArenaRuntimeSoundWave`, a procedural wave whose plays each get their own generator over shared
  samples (wav PCM 8/16/24/32-bit and float, loop points from the cache json) or stream-decode the ogg with the engine's
  Vorbis decoder (music, looping). `ArenaMusic` takes the track list from `Music.json` or the cache's `music/` folder.
  UT2004 `ProceduralSound`s (json `base_sound` + `properties`) share their base sound's samples; each play renders it at
  (100 + PitchModification +- PitchVariance) % pitch and (100 + VolumeModification +- VolumeVariance) % volume, the
  variances random per play as in UE2.

Lighting: runtime meshes have no mesh distance fields or Lumen cards, so software Lumen sees them only in screen space
and every off-screen ray reaches the sky. The project therefore enables hardware ray tracing (`r.RayTracing=True`,
`r.RayTracing.EnableOnDemand=1`, `r.Lumen.HardwareRayTracing=True`, `r.SkinCache.CompileShaders=True` in
`DefaultEngine.ini`; the fast build creates the meshes' ray tracing geometry). With it runtime levels match the cooked
imports. `Source/OpenUT2k4/ArenaRayTracing.*` applies the Settings row "Ray Tracing" (`UArenaUserSettings::RayTracing`:
Auto = on with a discrete GPU that supports it, On, Off) at startup and immediately when changed. Without support
(no RT hardware, SM5, Metal without ray tracing, `-noraytracing`) the row shows "Off (unsupported)" and Lumen falls back
to software tracing; `ArenaImportedLighting::Refresh` then scales the ambient fill of runtime levels down, since
nothing occludes it: to `SoftwareIndoorFillScale` on enclosed levels, full on open-air ones (openness = share of
upward probe rays from the spawn points and pickups that hit no collision, measured once per level). Test hooks (non-shipping): `-ArenaRayTracing=0|1|2`
overrides the setting, `-ImpSwFill=` the fallback fill scale, `-ArenaFrameStats=<from>-<to>` logs frame / GPU times
between those world seconds (add `-ExecCmds="r.VSync 0,t.MaxFPS 0"`).

Full Ray Tracing (Settings row "Full Ray Tracing", `UArenaUserSettings::FullRayTracing`, default Off) builds on ray
traced Lumen and can only be switched on while that is active (otherwise the row shows "Off (needs ray tracing)"). It
applies live: MegaLights direct lighting with ray traced shadows for every map light (`ArenaImportedLighting::Refresh`
turns their shadows on; UT2004 lights are unshadowed as built), masked materials evaluated in shadow rays, Lumen hit
lighting for reflections, rougher and two-bounce reflections, front layer translucency reflections, and half a stop more
auto exposure headroom for the now shadowed interiors. The sun keeps virtual shadow maps. The render cvars live in
`FullCVars` in `ArenaRayTracing.cpp`; switching off restores their previous values and priorities. Test hooks
(non-shipping): `-ArenaFullRT=0|1` overrides the setting, `-ArenaToggleFullRT=<sec>,...` flips it live (and saves) at
those world seconds, `-ArenaExecAt=<sec>:<command>;...` runs console commands (e.g. `profilegpu`), `-ImpFullExp=` the
extra exposure.

Zone distance fog (`Source/OpenUT2k4/ArenaZoneFog.*`, runtime levels): the level's height fog takes the
`bDistanceFog` / `DistanceFogStart` / `DistanceFogEnd` / `DistanceFogColor` of the zone the camera is in
(`UArenaWorldZoneSubsystem::GetZoneAt`; zones without a ZoneInfo use the LevelInfo's, passed in by `ArenaRuntimeMap`)
and returns to the level fog elsewhere, blending over 0.25 s. UE2's linear fog becomes uniform exponential fog from
Start, 55% opaque half way and 80% at End; the colour is flat (no sun or sky atmosphere inscattering). Water / lava /
fog volumes (`UArenaWorldZoneSubsystem::IsViewFogActive`) take over while the camera is inside one. The 3D sky box is
excluded (FogCutoffDistance) and its captures keep plain deferred lighting under Full Ray Tracing. Test hooks
(non-shipping): `-ZoneFogReport`, `-NoZoneFog`, `-ZoneFogScale=`.

### Runtime skeletal meshes and animations

With runtime content, player models, first-person and third-person weapons, vehicle rigs, flags, projectiles and
monsters are built from the cache by `Source/OpenUT2k4/ArenaRuntimeSkeletal.*`. The loaders are registered with
`ArenaRuntimeContent` as `SkeletalMeshes/...` and `Animations/...`, so `ArenaArt::Load` resolves the same relative
paths as with cooked imports. They follow `Tools/ImportUT2004.py`:
- `skeletalmeshes/` and `vertmeshes/` glTF become `USkeletalMesh` assets with one LOD, smooth normals
  recomputed, one material slot `S<n>` per distinct section material, mesh-only sockets from the json, and no physics
  asset. Bone names have spaces replaced by `-`.
- Skeletons are shared per animation set: every mesh whose json names the set uses the representative mesh's
  skeleton.
- Each sequence becomes `Animations/<Pkg>/<Set>_<Seq>`.

The sequences are `UArenaRuntimeAnimSequence` objects, a `UAnimSequence` subclass that keeps the cache keyframes and
samples them itself, because cooked builds cannot compress animations at runtime. That makes them work with the
native character animation and with single-node `PlayAnimation`. The render data is filled directly, with no editor
mesh builder, so it all works in Shipping builds.
For development, `-CompareRuntimeSkeletal=<rel>+<rel>...` (or the console command `arena.CompareRuntimeSkeletal`) builds
the given meshes / sequences from the cache and logs how they differ from the cooked `/Game/Imported` assets (`SkelCompare`
lines: bones, reference pose, sockets, vertex / triangle counts, bounds, materials, sequence length, sampled poses).
Add `-SkelShowcase` to place each listed mesh twice in front of the player, cooked on the left and runtime on the
right, each playing the animation listed right after it. Use it with `-AutoScreenshot`, and with `-saveddirsuffix=<Name>`
so that parallel test runs do not overwrite each other's `auto_*.png`.

### Importing the whole install

`--everything` exports every map in `Maps/` and every texture, static mesh, sound, skeletal mesh, animation and
music package of the install (including the resources inside `System/*.u`), not only what maps reference. It runs
in steps that can be repeated: the export (`ut2k4_export.py --everything`, which skips files that are already up to
date), one editor run for skeletal content and music, `--chunks N` editor runs for the other assets (default 16),
and map batches of `--map-batch K` (default 6) that skip levels already built from the current `map.json`. A failing
step is logged and the next one starts, so run the command again to resume; `--steps export,skeletal,assets,maps,report`
picks steps. Logs go to `Saved/ImportUT2004/` (`import_errors.log` lists per-asset failures,
`ImportCache/export_errors.log` the export ones), and the `report` step (`Tools/ImportUT2004.py --report`) prints
asset counts, sizes and maps that could not be imported.

| Content | Path |
|---------|------|
| Textures, materials, static meshes | `/Game/Imported/{Textures,Materials,StaticMeshes}/<Pkg>/<Group_Name>` |
| Sounds (every package) | `/Game/Imported/Sounds/<Pkg>/<Group_Name>` (SoundWave, looping when the WAV has a loop) |
| Music (`Music/*.ogg`) | `/Game/Imported/Music/<Name>` (SoundWave, looping) |
| Skeletal meshes (characters, weapons, vehicles) | `/Game/Imported/SkeletalMeshes/<Pkg>/<Name>` + sockets |
| Skeletons | `<mesh>_Skeleton` next to the first mesh of each animation set; the other meshes that use the set share it |
| Animations | `/Game/Imported/Animations/<Pkg>/<AnimSet>_<Sequence>` (AnimSequence) |
| Vertex-animated meshes (Invasion monsters, flags) | skeletal mesh with one joint per vertex; sequences as `Animations/<Pkg>/<Mesh>_<Sequence>` |

`Content/ImportedData/Assets/{SkeletalMeshes,Animations,Sounds,Music}.json` map UT2004 object paths to these
assets (plus skeletons, sockets, frame counts, rates and notifies). Skeletal content keeps the static meshes'
convention: Unreal axes, scale 2.0, and the UE2 mesh transform (MeshScale, MeshOrigin, RotOrigin) baked in, so a
player model stands upright at its in-game size around the actor origin.

`Content/ImportedData/<Map>.json` also carries `raw_markers`: every marker list of the exporter's `map.json`
(schema 2), including vehicle factories, Onslaught power cores, nodes and link setups, Assault objectives in stage
order, objective-relevant triggers and movers, bomb spawns and deliveries, domination points and Invasion spawns.

### Replacing placeholder art

Before the importer uses an imported asset `/Game/Imported/<Kind>/<Pkg>/<Name>` (`Kind` is `Textures`,
`Materials`, `StaticMeshes`, `SkeletalMeshes`, `Animations`, `Sounds` or `Music`), it checks for
`/Game/Replacements/<Kind>/<Pkg>/<Name>` and uses that asset instead:

- a replacement texture becomes the `Base` texture of the generated material instance,
- a replacement material (any material or material instance) is assigned instead of the generated instance,
- a replacement static mesh is placed instead of the imported mesh, using the same transform and collision settings,
- a replacement skeletal mesh, sound or music asset is not imported, and the `Assets/*.json` manifests list
  the replacement as `asset` (the imported path stays in `imported`); animations are imported onto the
  replacement skeletal mesh's skeleton.

BSP and terrain chunks are `StaticMeshes/<Map>/BSP_<Map>_NN` and `Terrain_<Map>_NN`. Put your own assets under
`/Game/Replacements/...` with the matching path and run the import again (without `--force`, which only rebuilds
the imported assets themselves). `/Game/Replacements` is normal project content and can be committed once it
contains only your own work.

#### Player models and weapons

Characters and weapons do not go through the importer: the game loads their art at runtime by soft path
(`Source/OpenUT2k4/ArenaArt.*`), trying `/Game/Replacements/<path>` first and then `/Game/Imported/<path>`. Nothing is
hard-referenced, so when neither exists the procedural body and view models are used (also with the console
variable `arena.ImportedArt 0`). Both folders are in `DirectoriesToAlwaysCook`.

- **Player models.** Each player or bot gets a model from the pool in `ArenaArt.cpp` (`GetCharacterModels`),
  picked by player id: male and female human, heavy, alien and robot models from
  `SkeletalMeshes/{HumanMaleA,HumanFemaleA,Jugg,Aliens,Bot}/<Model>`. Animations come from
  `Animations/<Pkg>/<AnimSet>_<Sequence>`; only sequences on the mesh's own skeleton are used. In team games the
  body material's `Base` texture is swapped for `Textures/PlayerSkins/<BodySkin>_0` (red) or `_1` (blue); a model
  without team textures gets a team-coloured rim instead. The mesh sits on `ACharacter`'s mesh component, offset
  so that its feet (bind pose foot bones at z = -75.5, facing +X) touch the bottom of the capsule. The owner does
  not see it in first person.
- **Animation.** `UArenaCharacterAnimInstance` (`ArenaCharacterAnim.*`) is a native anim instance with its own
  proxy, so no AnimBlueprint is needed. It blends idle / 4-way run (8 directions), crouch, jump, double jump,
  dodge, landing, swim and death sequences, overlays fire, weapon-switch and hit reactions on the upper body
  (`bip01-spine` and up) and bends the spine with the aim pitch.
- **Ragdolls and gibs** (`ArenaRagdoll.*`). On death the model ragdolls (UT2004's Karma ragdolls): the model's own
  physics asset if it has one, else one built at runtime from its skeleton (capsules on pelvis / spine / limbs, a sphere
  for the head, sized from the skeleton's height, cone and twist limits per joint; works for runtime and cooked meshes
  and in Shipping). The bodies start with the body's velocity plus the killing blow's momentum (extra kick on the head
  for headshots), settle and are removed with the pawn after 6 s. Deaths at or below `arena.GibHealth` (-60) gib as
  `xPawn.SpawnGibs` did: 8 `XEffects` gib meshes (`GibOrganic*`, robots `GibBot*`, aliens with the green skin), a
  `pcl_Blooda` blood burst (robots: `BotSpark` sparks) and a `Final_Giblets` sound. The procedural body only breaks apart
  when there is no art at all. `arena.Ragdolls 0` plays the death sequences instead (held on their last frame).
- **Weapons.** `GetWeaponArtTable` in `ArenaArt.cpp` maps each weapon class to a first-person mesh (arms
  included) with its idle / select / fire / alt-fire / hold / put-down sequences, a third-person mesh shown in
  the model's `righthand` socket, fire sounds and the view placement (offset, rotation, scale and the field of
  view the view model is drawn with, using UE's first-person rendering):

  | Weapon | First person | Third person | Fire sounds |
  |--------|--------------|--------------|-------------|
  | Gauntlet | `Weapons/ShieldGun_1st` | `Weapons/ShieldGun_3rd` | `BShieldGunFire`, `BShield1` |
  | Carbine | `NewWeapons2004/AssaultRifle` | `NewWeapons2004/NewAssaultRifle_3rd` | `BAssaultRifleFire`, `BAssaultRifleAltFire` |
  | Goo Launcher | `Weapons/BioRifle_1st` | `Weapons/BioRifle_3rd` | `BBioRifleFire` |
  | Pulse Rifle | `NewWeapons2004/ShockRifle` | `NewWeapons2004/NewShockRifle_3rd` | `BShockRifleFire`, `BShockRifleAltFire` |
  | Arc Linker | `NewWeapons2004/FatLinkGun` | `NewWeapons2004/NewLinkGun_3rd` | `BLinkedFire` (beam: synthesized) |
  | Rotary Gun | `Weapons/Minigun_1st` | `Weapons/Minigun_3rd` | `BMiniGunFire`, `BMiniGunAltFire` |
  | Shard Cannon | `Weapons/Flak_1st` | `Weapons/Flak_3rd` | `BFlakCannonFire`, `BFlakCannonAltFire` |
  | Rocket Launcher | `Weapons/RocketLauncher_1st` | `Weapons/RocketLauncher_3rd` | `BRocketLauncherFire`, `BRocketLauncherAltFire` |
  | Rail Rifle | `Weapons/Sniper_1st` | `Weapons/Sniper_3rd` | `BLightningGunFire` |
  | Insta Rifle | `NewWeapons2004/FinalShockRifle` | `NewWeapons2004/NewShockRifle_3rd` | `instagib_rifleshot` |
  | Ball launcher (Bombing Run) | `Weapons/BallLauncher_1st` | `Weapons/BallLauncher_3rd` | `ballgun_launch` |

  Meshes are under `SkeletalMeshes/`, sequences under `Animations/` and sounds under `Sounds/WeaponSounds/`.
  The muzzle flash and beams start at the first-person mesh's muzzle socket (mapped to where it appears on
  screen) or, seen from outside, at the third-person mesh's `tip` socket. Looping sound waves are never used as
  fire sounds. Weapons without art keep their procedural view model and synthesized sounds.

- **Onslaught vehicles.** `ArenaArtVehicles.*` maps each vehicle to a chassis skeletal mesh, gun meshes on its
  bones, a wreck static mesh and sounds (Wisp `ONSVehicles-A/HoverBike`, Dustrunner `ONSVehicles-A/RV` +
  `ONSWeapons-A/RVnewGun`, Bulwark `ONSVehicles-A/HoverTank` + `ONSWeapons-A/HoverTankCannon` / `TankMachineGun`,
  Kestrel `ONSVehicles-A/AttackCraft`, Caravan `ONSVehicles-A/PRVchassis` + `ONSWeapons-A/PRVrearGUN` / `PRVsideGun`).
  The meshes are posed directly (`UPoseableMeshComponent`, no AnimBlueprint): wheel bones follow the suspension,
  spin and steer, tank rollers spin per side, and turret yaw / pitch bones follow the (replicated) aim. When a
  model is present its vehicle also uses collision, suspension, camera and muzzle values fitted to it (the
  `ArenaArtVehicles.cpp` table); otherwise the procedural model and tuning are used, as with `arena.ImportedArt 0`.
  Team skins are the uncoloured diffuse tinted red / blue with a faint team rim. This is a deliberate choice for
  readable team colours: the imported `<Combiner>_Baked` skins (UT2004's own tan / teal team colours) are correct
  but intentionally overridden at runtime. Hover jets and exhausts are additive flare sprites, open vehicles seat the driver's model in its
  `Vehicle_Driving` pose, destroyed vehicles leave a tumbling `ONSDeadVehicles-SM` wreck, and engine / start / stop /
  fire / explosion sounds come from `Sounds/ONSVehicleSounds-S`. Vehicle bays have no marking of their own (UT2004's
  `ONSVehicleFactory` is invisible; the maps model their bays) and show a hologram of the vehicle while it is rebuilt;
  only procedural vehicles (no art) get a team outline and plate.
  `-ONSArtShowcase` (with `-AutoScreenshot`) frames each vehicle type in turn for checking.
  Brakes hold a vehicle on the ground that is slower than UT2004's StopThreshold (100 UU/s) with no throttle or steering
  (ONSWheeledCraft / ONSHoverCraft; empty flyers too): static friction cancels its creep and gravity's pull down a slope
  (up to 1.2 g), so parked and idle vehicles stay put on flat ground and slopes (`-MoveTest=w_vehrest`: every type on
  flat ground and a 15 degree slope; `-UE2FXTest=vehrest` logs how far each empty vehicle moves every 10 s).

To replace a character or weapon, put your asset at the same relative path under `/Game/Replacements`, e.g.
`/Game/Replacements/SkeletalMeshes/HumanMaleA/MercMaleA` plus its sequences at
`/Game/Replacements/Animations/HumanMaleA/BipedMaleA_RunF` and so on (same skeleton as the mesh), or change the
paths and placement values in `ArenaArt.cpp` to point at differently named assets. A replacement player model
should face +X with its feet 75.5 cm below its origin (or adjust `ModelFeetZ` in `ArenaCharacter.cpp`), and have
the `righthand` socket and the `bip01-spine*` bones, or the upper-body overlays and aim are skipped. A physics asset on
the replacement is used for its ragdoll; without one it is built from the skeleton (biped bone names). Materials used on skeletal meshes need the "Used with Skeletal Mesh" usage
(the importer sets it on its master materials).

#### Pickups, weapon effects and game objects

These also load their art at runtime by soft path through `ArenaArt` (Replacements, then Imported) and keep their
procedural shapes whenever an asset is missing or `arena.ImportedArt` is 0. Tables: `Source/OpenUT2k4/ArenaArtItems.cpp`
(pickups, game objects) and `Source/OpenUT2k4/ArenaArtFX.cpp` (effects, projectiles, per-weapon styles). Meshes are fitted
by their bounds to the sizes in the tables, so a replacement of a different scale keeps the same footprint.

- **Pickups** (`ArenaPickup`): weapon pickups use the source game's pickup meshes matching the weapon art above
  (`NewWeaponPickups/{AssaultPickupSM,ShockPickupSM,LinkPickupSM}`, `WeaponStaticMesh/<Weapon>Pickup`), ammo uses
  `WeaponStaticMesh/<Weapon>AmmoPickup`; health vial `XPickups_rc/MiniHealthPack`, health pack
  `E_Pickups/Health_MidHealth`, super health `E_Pickups/Health_SuperKeg`, shield packs
  `E_Pickups/General_{RegShield,SuperShield}`, double damage `E_Pickups/General_Udamage`, adrenaline
  `XPickups_rc/AdrenalinePack`. Bases: `2K4chargerMESHES/ChargerMeshes_{WeaponChargerMesh,HealthChargerMESH}-DS`,
  `XGame_rc/{ShieldChargerMesh,WildcardChargerMesh}`. A base is shown where the original item stood
  on one (`xWeaponBase` / `*Charger` sources). Items keep
  their spin, bob and respawn hiding; the base stays.
- **Weapon effects** are built from components, not particle systems: camera-facing sprite quads, quads stretched
  along beams and trails and turned to the viewer, flat rings, and the imported projectile meshes, each scaling and
  fading over its short life (`UArenaSpriteComponent`). Textures are drawn with the generated
  `/Game/Materials/M_ArenaSprite` (additive) / `M_ArenaSpriteAlpha` (translucent) materials, which add flipbooks,
  tiling and panning (`Tools/GenerateContent.py`; rebuild only them with `OPENUT2K4_GENERATE_ONLY=sprite`). Without them
  the imported master materials are used and flipbook layers are skipped.

  | Weapon | Projectile / beam | Muzzle | Impact / explosion |
  |--------|-------------------|--------|--------------------|
  | Gauntlet | shield `WeaponStaticMesh/Shield` | `MuzFlashWhite_t` | `Link_grey_ring` |
  | Carbine | tracer `TracerT`; grenade `GrenadeMesh` | `MuzFlashA_t` | sparks + smoke; `exp7_frames` |
  | Goo Launcher | `XWeapons_rc/GoopMesh` (+ `goop_GoopShader`, Flying / Drip) | `MuzFlashBio_t` | bio flash + `GreenSmokeTex` |
  | Pulse Rifle | beam `ShockBeamTex`; core `shock_core_low` + `shock_flare_a` | `MuzShockFlash_t` | `Shock_ring_a`, combo `ShockComboFlash` + rings |
  | Arc Linker | bolt `LinkProjectile`; beam `link_beam_green` (tiled, panning) | `MuzFlashLink_t` | `Link_projectile_splash`, `link_ring_green` |
  | Rotary Gun | tracer `TracerT` | `MuzFlashA_t` | sparks + smoke |
  | Shard Cannon | `FlakChunk` + `FlakTrailTex`; shell `FlakShell` | `MuzFlashFlak_t` | `we1_frames` + `exp2_frames` |
  | Rocket Launcher | `RocketProj` + `RocketFlare` + `TrailBlur` smoke; grenade `GrenadeMesh` | `MuzFlashRocket_t` | `exp2_frames` + `SmokeReOrdered` |
  | Rail Rifle | `LightningBoltT` + `pcl_BlueSpark` | `MuzFlashWhite_t` | `LightningChargeT` |
  | Insta Rifle | `ShockBeamRedTex` | `MuzShockFlash_t` | red shock ring / flare |

  Muzzle flashes appear on the first-person muzzle socket and, for other viewers, on the third-person weapon's `tip`.
- **UT2004's own effect classes** (`ArenaUE2Effects.*`): the super weapons and the Onslaught weapons and vehicles spawn
  the stock Emitter classes, read at run time from the cache's script packages (`scripts/System/XEffects.u`,
  `Onslaught.u`) with the converter's package reader: the class's default properties and its inline ParticleEmitter
  subobjects become the same description a map Emitter has and are simulated by `AArenaEmitter` (see "UE2 world
  behaviour"). Redeemer: `RedeemerExplosion` and `IonCore` where it blows up, `SmallRedeemerExplosion` when it is shot
  down, `NewRedeemerTrail` / `RedeemerTrail` behind the warheads. Ion cannon: `IonEffect` (native `AArenaIonEffect`
  after its script: the `IonCore` falling over 0.5 s, the `IonShaft` and `IonLightBeam` beams, three of them as in
  `IonCannon.SpawnEffect`) and `NewIonEffect` where it lands. AVRiL rocket: `ONSAVRiLRocketExplosion`; ONS grenades and
  spider mines: `ONSGrenadeExplosionEffect`; vehicles: `ONSVehicleExplosionEffect` / `ONSSmallVehicleExplosionEffect`
  (their `DestructionEffectClass`); the Ion Painter and Mine Layer beams use the `PainterBeamEffect` /
  `ONSMineLayerTargetBeamEffect` textures and widths. The classes are read when a match starts (a few hundredths of a
  second). Without a cache (cooked content) or with `arena.UE2Effects 0`, the procedural effects are used.
  Test hook (non-shipping): `-UE2FXTest=nuke|ion|both [-UE2FXTestTime=<s>]` fires the real Redeemer blast and ion strike
  on the ground in front of the local view and takes screenshots (`ue2fx_<kind>_<n>.png`); `-UE2FXTest=warhead` sends
  five unguided warheads in over it instead (logs `Warhead: <bot> shoots at the incoming warhead` / `shot down by`).
  `-UE2FXTest=bighead` follows the character with the biggest head from behind (for `?Mutator=UnrealGame.MutBigHead`).
- **CTF**: the waving flag `XGame_rc/FlagMesh` (sequence `flag`) with `XGameShaders2004/CTFShaders_{Red,Blue}FlagShader`,
  on `XGame_rc/FlagBaseMesh` with `XGameTextures/FlagBaseTex{R,B}`. The team symbol on the cloth is a second,
  translucent layer of the same mesh following the flag's pose: UT2004 fills it in at runtime (the flag shader's
  Combiner adds a TexScaler whose material `CTFFlag.UpdateForTeam` sets), so the converted flag material has none.
  **Team symbols** are picked per match like UT2004's `TeamGame.GetRandomTeamSymbol` from its non single-player symbol
  list (`TeamSymbols_UT2003` / `TeamSymbols_UT2004` entries without `Description="SP"`, `ArenaArtItems::GetTeamSymbolList`;
  red from index 1, blue from index 10), never the same for both teams, or set with `?RedTeamSymbol=` /
  `?BlueTeamSymbol=` (`TeamSymbols_UT2003.sym05`, `TeamSymbols_UT2004.Team.Phoenix` or just `Phoenix`). The game state
  replicates them (`AArenaGameState::TeamSymbols`); every flag of a team and the HUD team score boxes show them.
- **Bombing Run**: ball `XGame_rc/BallMesh` on `XGame_rc/BombSpawnMesh` at its home spot, goals
  `XGame_rc/BombDeliveryMesh` with the team pulse `SuperPickups_BombgatePulse{R,B}C`.
- **Domination**: pad `XGame_rc/DominationPointMesh`, letter `XGame_rc/Dom{A,B}Mesh` and ring `XGame_rc/DomRing`, with
  `SuperPickups_DOMab{R,B,G}s` / `SuperPickups_DOM{Red,Blue,Grey}S` for red, blue and neutral / locked.
- **Onslaught**: node base `VMStructures/CoreGroup_powerNodeBaseSM` with the energy frame `ONSWeapons-A/PowerNode`
  (its energy sections hidden while neutral, rising while under construction, red / blue
  `ONSstructureTextures` materials when owned), core `VMStructures/CoreGroup_CoreDivided` with the energy column
  `CoreGroup_COREenergy`, shield domes `CoreGroup_CoreShieldSM` (team energy shaders), team-tinted soft flares, and
  link beams from `AW-2004Particles/Energy_{PowerBeam,BeamBolt1a}` tinted by the link state.
- **Jump pads**: `EpicParticles/JumpPad_JumpCircle01aw` turning on the pad with `Link_grey_ring` rings closing in.

- **Invasion monsters** (`ArenaINV_Monster.*`): the SkaarjPack monsters of UT2004's Invasion, under our own names:
  Gnashling (`SkaarjPupae`), Razorwing (`Razorfly`), Skyray (`Manta`), Sputterpod / Elite (`Krall` / `EliteKrall`, skin
  `ekrall`), Whirrsting (`Gasbag`), Slagbrute / Slagtitan (`Brute` / `Behemoth`, skin `jBrute2`), Vaultfang / Frost / Ember
  (`Skaarj` / `IceSkaarj` / `FireSkaarj`, skins `Skaarjw1-3`) and the boss Cindermaw (`WarLord`). `ArenaINV::GetMonsterDef`
  holds per type the vertex mesh (`SkeletalMeshes/SkaarjPack_rc/<Mesh>`), skins, the sequences for idle / walk / run /
  fly / leap / melee / fire / hit / death (`Animations/SkaarjPack_rc/<Mesh>_<Seq>`), the sounds (`Sounds/SkaarjPack_rc`),
  and UT2004's class defaults (health, speed, collision size, melee range, score, projectile speed / damage / radius;
  1 unit = 2 cm). The mesh sits at the capsule centre as in UE2; movement picks idle / walk / run (play rate by speed) or
  the flight loop, attacks and hits play their one-shots, death plays a death sequence held on its last frame (vertex
  meshes have no bones to ragdoll) or gibs. Projectiles: Krall bolts, Skaarj energy balls (green / blue / red), Gasbag
  fireballs, Brute and WarLord rockets. Waves follow UT2004's `Invasion.Waves` table (16 waves, monster masks, max
  alive). Without the art a procedural stand-in body is used.

- **Assault objectives** are the map's own things, as in UT2004: a `DestroyableObjective_SM` shows its static mesh
  (from the map json's actor, placed like the map's static mesh actors) and switches to its `DestroyedStaticMesh` (or
  disappears) when destroyed; hold / use / reach / other destroy objectives are invisible spots at the map's consoles,
  doors and trailers, shown by the HUD objective icons and markers (`NewHUDicons`). The procedural objective models
  remain only for objectives that are not from a map (fallback layouts). Every stock objective type is played: hold /
  use (`HoldObjective`), reach (`ProximityObjective`, `TriggeredObjective`, energy core pick-up and delivery), destroy
  (`DestroyableObjective(_SM)`, `DestroyVehicleObjective`). Bot-only helper objectives are not shown, as in UT2004.
- **Assault vehicles and turrets** (`ArenaAS_Turret.h`, `UArenaRules_AS`): the map's `ASVehicleFactory*` actors are
  spawned from the stock class defaults read through the UScript loader (`ArenaASTurret::DescribeFactory`):
  sentinels (floor / ceiling, automatic, `Weapon_Sentinel`: 20 damage lasers every 0.25 s), minigun, link, energy
  (ball) turrets and the ion cannon with their base / swivel / gun models, health, fire rate, projectile speed, damage
  and splash, pitch limits. Sentinels and the link / energy turrets (UE2 `bAutoTurret`) fire at the nearest seen enemy
  on their own, as their own controller (`AArenaAS_TurretController`: their shots follow the team rules and the kills
  are the turret's). Sentinels stay closed until an enemy shows up, then play the meshes' own Open / IdleOpen / Fire
  animations and Close again 5 s after the last one, with their OpenCloseSound; players get into the manned ones with Use (aim with the view, fire, Use again to get out) and
  defender bots man the ones near their current objectives. Factories of ONS vehicles (AS-Glacier's tanks, Outback,
  Junkyard ...) spawn our vehicle of the closest type for their team, respawned 30 s after being destroyed; bot
  drivers go for their team's current objective and shoot destroy objectives and enemy turrets. A sentinel whose
  `VehicleEvent` is a `DestroyVehicleObjective`'s tag is that objective (AS-MotherShip's sentinels): damage to it is
  the objective's. Players whose spawn manager forces a space fighter (`ForcedPawnClass`, AS-MotherShip's space part)
  spawn in our gunship. With vehicles for the attackers, objectives that need one (`bMustBoardVehicleFirst`, vehicle
  paths) are played; objectives the paths do not reach with the movers at rest are kept (the movers run). UE2
  `UseTrigger`s fire their event on Use. When the attacking bots make no progress for 20 s and their path crosses a
  closed mover, they touch / use / shoot what opens it, or wait in front of it (`UpdateMoverStall`). In `?UE2Net=1`
  worlds none of this is spawned: the server's turrets and vehicles are shown (Docs/UE2Net.md).
  Developer hook: `-DumpUScriptDefaults=Pkg.Class,Pkg.Class` (Assault maps) logs every non-empty class default of
  those UnrealScript classes (`UScriptDefaults ...` lines), as used to find the stock turret numbers.
- **Onslaught node health**: as `ONSHUDOnslaught.DrawHealthBar`: a 75 x 8 pixel bar (`InterfaceContent/Menu_BorderBoxD`
  frame, `ONSInterface-TX/HealthBar` in red / gold / green) 485 UU above every active or building node / core in sight
  within 4000 UU (see "UT2004 HUD" below). Without the HUD art the procedural meter over each node is drawn instead.

Debug console variables: `arena.DebugFXGallery <seconds>` with `arena.DebugFXFilter <text>` (every effect, beam and projectile in front of the player; `Pickup` spawns every pickup, `Objects` / `ONS` cycle a camera over flags, ball, goals, points, pads / nodes and cores), `arena.BehindView 1|2` (third-person camera behind / in front of the viewed pawn),
`arena.DebugCycleWeapons <seconds>` (give the local player every weapon and cycle through them),
`arena.GibHealth <health>` (gib threshold; 1000 gibs every death), `arena.DebugRagdoll 1` (log the bodies of each built
physics asset), and
`arena.DebugArt 1|2` (log model placement and bone positions).

#### Interface art (HUD and menus)

The menus load their textures at runtime by soft path (`Source/OpenUT2k4/ArenaArtHUD.*`), again `/Game/Replacements/<path>`
first, then `/Game/Imported/<path>`; the in-game HUD and scoreboards are described under "UT2004 HUD" below. Every
textured widget falls back to its procedural drawing when its texture is missing or `arena.ImportedArt` is 0. Atlas
regions are in pixels of the source texture and turned into UVs, so a replacement at another resolution works if it
keeps the layout.

| Widget | Texture | Region / notes |
|--------|---------|----------------|
| In-game HUD, scoreboards | `Textures/HudContent/Generic_HUD` and others | see "UT2004 HUD" |
| Menus | `2K4Menus.utx` (`MainMenu_*`, `NewControls_*`, `Controls_*`, `BkRenders_*`), `UT2004Thumbnails`, `ULogo` | see "Menus (GUI2K4 look)" below |

The game title, subtitle and version tag come from `Source/OpenUT2k4/ArenaBranding.h`.

#### Menus (GUI2K4 look)

The main menu, Instant Action, Settings, the in-game menu, the end-of-match banner and the UT2004 Content screen are
drawn like UT2004's GUI2K4 menus by a small canvas GUI (`ArenaGUI2K4.*`: styles, fonts, widgets, input;
`ArenaGUI2K4Pages.*`: tabbed page frame, Settings, game type / map / mutator data, automation; `ArenaMenuScreens.*`:
main menu and Instant Action; `ArenaMidGameMenu.*`: in-game menu and end banner). Nothing of UT2004 is shipped: the
layout and style values are a C++ table taken from the class defaults of the player's own `GUI2K4.u` / `XInterface.u`
(each value is commented with its source, e.g. `UT2k4MainMenu.InstantActionButton`, `STY2SquareButton`,
`fntUT2k4Menu`), the textures, fonts and sounds load at runtime from the converted content, and captions / hints come
from UT2004's localization files (`<cache>/localization`, else the install; `GUI2K4.int`, the game classes' `.int` for game type names and descriptions,
mutator `.int`s) with our own fallback wording.

* **Styles**: every `STY2*` style used (per menu state Blurry / Watched / Focused / Pressed / Disabled: images, image
  styles, font names, font / image colours, border offsets). Image styles are UE2's: Stretched (corners keep their
  size, the middle stretches: crisp borders at any size), PartialScaled (end caps keep their width), Scaled,
  Justified, Tiled, Normal.
* **Fonts**: the `fntUT2k4*` GUIFonts (Verdana 8-26, Impact32 for the main menu, Eurostile for the mid-game font),
  picked like `GUIFont.GetFont(ResX)` for the virtual resolution of the page (screen height / 720 = the text size
  factor, at least 1); that font's height times the factor is the size drawn. The closest size of the same face in the
  content (Verdana 8-34, Eurostile / Neuzeit 8-37; the next larger one is shrunk, the largest enlarged) is resampled
  on the CPU to exactly that height (`ArenaRuntimeContent::LoadFontAtHeight`: bicubic in premultiplied alpha, an area
  filter when shrinking, a contrast curve on enlarged glyph edges; uncompressed, bilinear, clamped) and drawn 1 texel
  = 1 pixel, so text is smooth at 1440p / 4K. Up to 720 lines it is the stock font 1:1, as UT2004 drew it. (Earlier
  builds drew the bitmap fonts point sampled at 2x / 3x and the main menu's Impact at 2.4x: blocky, stair-stepped.)
* **Scaling**: pages are laid out in a centred 4:3 area sized by the screen height (UE2's relative coordinates of a
  4:3 screen); backgrounds, the header and the footer bar run across the full width, the main menu backdrop covers the
  screen without stretching (cropped), the title bands tile across it. UE2 pixel sizes scale with the height
  (1024 x 768 reference). Text sizes follow the fonts rule above; the main menu's Impact font scales by page
  width / 800 like the stock `bScaled` font (resampled to that size). Hit areas are the drawn rectangles.
* **Input**: mouse (hover = Watched, press / release click, wheel scrolls lists, slider dragging, combo lists, tool
  tips after a short hover), keyboard and gamepad (Up / Down / Tab move the focus, Left / Right change the focused
  option or tab, Enter / A activate, Esc / B back, PageUp / PageDown / shoulder buttons switch tabs, typing in the
  player name field). An input pre-processor queues the input while a menu is open and keeps it from the game.
* **Pages**: main menu (UT2k4MainMenu: animated title bands, character render, logo, the corner view sequence, text
  buttons sliding in with the highlight bar; the stock buttons in the stock order, positions, slide-in timing,
  captions and hints (`GUI2K4.int [UT2k4MainMenu]`): Single Player (the tournament pages of `ArenaSinglePlayer.h`;
  disabled in a build without them), Join Game (server browser), Host Game, Instant Action, Community (our UT2004
  Content screen: install, downloads, custom content), Settings, Exit UT2004; Esc or Exit opens the quit page);
  Host Game (UT2K4GamePageMP: the Instant Action tabs with Server Rules inserted as the 4th tab, LISTEN / DEDICATED /
  BACK, see "Hosting games (Host Game)"); Instant Action (UT2K4GamePageSP: Gametype with the UT2004Thumbnails preview and
  description, Select Map with the map's LevelSummary screenshot (the material named by LevelSummary.Screenshot,
  the same as the cache record's ScreenShot; most are MaterialSequences that cycle and cross fade through the map's
  shots: it starts at its first shot whenever a map is selected, like UE2 playing the sequence while drawn, and is
  drawn opaque like a GUIImage with MSTY_Normal, so DXT3 shots with zero alpha (DM-Injector) show), author,
  players and description (stock maps
  whose LevelSummary.Description is empty, most of them, show their `TextName` DecoText from the cache records,
  e.g. `[DecoText] Face3=` of XMaps.int, as UT2004 does; `Found N map(s) ..., M without a description`) and an "only
  official maps" filter, Game Rules, Mutators, Bot Config; PLAY / SPECTATE (not available) / BACK); Settings
  (UT2K4SettingsPage tabs Display, Audio, Player, Input, HUD; our rows mapped in: Display = monitor, window mode,
  resolution, brightness + the Gamma Test image, ray tracing, full ray tracing, upscaler rows and an HDR group; Audio =
  volume, music; Player = UT2K4Tab_PlayerSettings, see below; Input = sensitivity, invert mouse; HUD = crosshair (with preview) and
  the HUD rows; rows added later appear automatically; BACK / DEFAULTS); in-game menu (UT2K4PlayerLoginMenu: player
  lists per team, Resume / Restart Match / Leave Match / Settings / Exit Game, Rules tab, close box); UT2004 Content
  screen (page background, header with the state, section frames, round buttons; extension blocks draw with the
  same fonts).
* **Mutators**: the Mutators tab lists the mutators of the install's cache records (`System/*.ucl` `Mutator=`
  entries, like UT2004's CacheManager) and `Object=(...MetaClass=Engine.Mutator...)` entries of `.int` files in
  UserContent, with localized names and descriptions; one per `GroupName`; the choice is saved
  (`ArenaUserSettings.LastMutators`) and passed as `?Mutator=Pkg.Class,...` (UnrealScript runtime). Mutators that
  `ArenaUScript::CanRun()` rejects are greyed out with its reason (only natives reachable from gameplay code count).
* Before any content exists the same layouts are drawn with flat stand-ins and the engine font.

Test hooks (non-shipping; injected input goes into the GUI's own queue, the desktop is never touched):

```
# open a page / tab: -AutoMenuPage=InstantAction|HostGame|SinglePlayer|Settings|ServerBrowser|Quit [-AutoMenuTab=<n>] [-AutoMenuMode=CTF] [-AutoMenuLayout=Imported:CTF-Face3]
# screenshot of the menu after <sec> (Saved/Screenshots/menu.png), then quit: -AutoMenuShot=<sec>
# start the Instant Action match after <sec>: -AutoMenuStart=<sec> [-AutoMenuMode=...] [-AutoMenuLayout=...]
# scripted navigation, main menu and in a match: -AutoMenuScript="<sec>:<cmd>;<sec>:<cmd>..." with
#   Key=<UE key name> (Down, Enter, Escape, Gamepad_RightShoulder ...), Click=<x>,<y> (0..1 of the 4:3 page),
#   Wheel=<n>, Shot=<name> (Saved/Screenshots/<platform>/menu_<name>.png), Quit,
#   menu: Page=Main|InstantAction|HostGame|SinglePlayer|Settings|ServerBrowser|Quit, Tab=<n>, Mode=<id>, Layout=<id>,
#   Mutator=<Pkg.Class>, HostPort=<n>, HostName=<name, _ for spaces>, HostPassword=<pw>, HostStart=1 (LISTEN) | 2
#   (DEDICATED), SP<cmd>=... (forwarded to the Single Player page),
#   Browser*=... (server browser, see "Server browser (Join Game)")
#   match: Pause, Resume, Settings, Tab=<n>  (the script runs once per process: in the menu, or in the match the
#   game was started in)
OpenUT2k4 -AutoMenuScript="3:Key=Down;4:Key=Enter;6:Shot=ia;7:Key=Gamepad_RightShoulder;8:Shot=maps;9:Quit"
```

#### Settings > Player

`Source/OpenUT2k4/ArenaPlayerSettingsTab.*`, laid out like UT2004's `UT2K4Tab_PlayerSettings`:
* **3D View / Portrait** (toggle button): the character's converted mesh with its idle animation and body skin
  (normal, red or blue skin preview; skins outside PlayerSkins, such as DemoPlayerSkins.Jakob_Body, through
  `ArenaArt::LoadBodySkin`), lit by a key, fill and back light like the stock SpinnyWeap and drawn straight over the
  page background (an HDR scene capture whose alpha is the coverage: the page is darkened by that mask, then the
  premultiplied colours are added); drag to turn it. The other
  mode shows the portrait. **Change Character** opens the `UT2k4ModelSelect` window: the `.upl` characters (DUP
  entries left out) in a 4 x 3 portrait grid, filtered by race, with a 3D view of the selection. Characters with a
  Menu tag other than SP (Malcolm, the Clan Lord, Xan) stay locked (cDefault picture, OK disabled) until the tournament
  unlocks them (`UArenaUserSettings.UnlockedCharacters`).
* **Misc.**: Name, Small Weapons, Preferred Team (Red / Blue / None), Weapon Hand (Left, Center, Right, Hidden), Voice
  Type (the `xVoicePack` classes of the `.int` Object lists matching the character's gender; "Default"; picking one
  plays one of its taunts, as `TeamVoicePack.PickCustomTauntFor`, from the class defaults read through the UnrealScript
  loader), Default FOV (our field of view setting). **Biography**: the character's DecoText. DEFAULTS restores
  Jakob, "Player", no team, right hand.
* Saved in `UArenaUserSettings` (PlayerCharacter, PlayerSex, PreferredTeam, VoiceType, WeaponHand, bSmallWeapons).
  The local player uses the character in every match: `AArenaGameMode::InitNewPlayer` reads the `?Character=` login
  option (remote players), else the local choice, into the replicated `AArenaPlayerState::CharacterName`, which picks
  the model (the mesh and skin of its `.upl` record). The New profile page of the tournament starts on it.

* Weapon hand and small weapons in the first person view (`Source/OpenUT2k4/ArenaWeaponHand.*`, applied by
  `AArenaWeapon::ApplyHandToViewModel`), as UE2's `Weapon.RenderOverlays`: Handedness = WeaponHand - 1 (bots use 0, as
  `Bot.ChangedWeapon`). Left mirrors the view model (scale Y, offset Y, pivot yaw and roll negated); Center uses the
  weapon's `CenteredOffsetY`, `CenteredYaw` and `CenteredRoll`; Hidden draws no first person weapon. Small Weapons
  uses `SmallViewOffset` (falling back to `PlayerViewOffset`). The values come from the stock weapon class defaults
  (XWeapons) through the UnrealScript loader. Projectile spawn offsets flip `ProjSpawnOffset.Y` with the hand (no side
  or vertical offset when hidden), the shock beam and combo starts and the muzzle flash follow the hand, and a hidden
  weapon fires from the player's location (`GetEffectStart`). The dual Carbine is never centred (left hand, as stock);
  its off hand copy mirrors the main view model. Remote players' hands are not replicated (right handed).
* Saved settings are sent to UT2004 servers too (login URL Handedness / Voice).

Test hooks (with `-AutoMenuScript`, Settings page, Tab=2): `PlayerChar=<name>`, `PlayerSelect=1|0` (Change Character),
`Player3D=0|1`, `PlayerSkin=0|1|2`, `PlayerHand=0..3` (Left, Center, Right, Hidden), `PlayerSmall=0|1`,
`PlayerVoice=<voice class>` (plays a preview line), `PlayerLight=<scale>|src<n>` (preview light scale / capture source).

#### Single player tournament

Main menu → Single Player opens UT2004's tournament (GUI2K4 `UT2K4SP_Main`): `Source/OpenUT2k4/ArenaSinglePlayer*.cpp`
(pages and dialogs), `ArenaSPProfile.*` (the profile and its rules, after UT2004's `UT2K4GameProfile`) and
`ArenaSPData.*` (the stock data). Nothing of UT2004 is shipped: the ladders are read at runtime from the player's own
`XGame.u` through the UnrealScript loader (the `UT2K4LadderInfo` / `CLOnslaught` match lists are default-property sub
objects (`UT2K4MatchInfo`: map, alternative maps, game type, bots, goal, time limit, prize money, entry fee, enemy team
or roster group, special events, requirements), the team rosters (`UT2K4TeamRoster` classes: names, leaders, levels,
symbols, descriptions), the roster groups, the challenge games and the economy values of `UT2K4GameProfile`), the
texts from the `.int` files (GUI2K4, XGame, XPlayers DecoText) and the characters from the System `.upl` lists (in the
content cache since localization version 2, else the install; characters of packs that are not converted are left
out). The ratings, salaries, fees, injuries, team ratings, phantom matches and enemy team selection follow the stock
rules.

* **Tabs**: Profile (list, portrait, team symbol, details; DELETE / CREATE NEW / HIGH SCORES), New profile (character,
  team symbol, name, team name, difficulty), Tutorials (shown locked: the scripted tutorial maps are not supported
  yet), Qualification (DM ladder), Team Qualification, Ladder (DOM, CTF, BR, AS ladder selectors with their pictures
  and progress, the championship lock, challenges, the selected ladder's rungs), Team Management (your team with
  health bars, hire / release / auto fill, treatment, free agents with stats, fee and description; locked until the
  team qualification is won, except during the draft), Additional (the Onslaught ladder, after the championship).
  The selected match shows the map's screenshot, entry fee, prize money, description and CHANGE ARENA (pay to switch to
  the rung's alternative map).
* **Dialogs**: the voiced story messages (UT2K4SP_Message with the announcer's lines and subtitles), team roles
  (the lineup and positions before team matches, minimal fees), injuries, arena change, match details (last match,
  opponent teams, other tournament matches), team details, challenges (Head to Head with a bet, Bloodrites trading
  team mates; also the rematch challenges enemy teams send), trades, donations, high scores.
* **Match flow**: PLAY (requirements, entry fee, team roles) starts the match through the Instant Action path
  (`?Mode=<ours>?Layout=<map>?Bots=?Skill=<difficulty>?Goal=?Time=?Team=1`) with the rung's bots by name and team
  (`?BotNames=A,B?BotTeams=1,0`, read by `AArenaGameMode::AddBot`; the bots' and the player's character models follow
  their UE2 meshes where we have them, `ArenaArt::SetCharacterOverride`) and the teams' symbols. When the match ends the
  result (winner, scores, kills, deaths, goals, sprees, multi kills) is registered, the profile is saved and the menu
  reopens on the tournament with the details and the special events (ladders opening, the draft, qualification
  messages, team updates, challenges, injuries, the championship). Leaving a match early keeps the forfeit fee.
* Profiles: JSON in `<user data>/SinglePlayer/<name>.json` (`-SPProfileDir=<dir>` for tests); the high scores in
  `HighScores.json` next to them.
* **Special awards**: the match keeps per player counters (`ArenaMatchStats.h`, a world subsystem fed by the death and
  combo code): flak cannon kills, adrenaline combos, sniper / classic rifle head shots, road kills; with goals and
  deaths they give Flak Monkey, Combo Whore, Head Hunter, Road Rampage, Hat Trick and Untouchable at UT2004's
  thresholds (`UT2K4GameProfile` sae_* defaults).
* **Bots**: each named bot gets its character's profile from the `.upl` record (Accuracy, ReactionTime, Jumpiness,
  StrafingAbility, like xRosterEntry) and its character model: the converted mesh with the character's body skin, also
  for meshes outside the default model pool (Thunder Crash, Hellions, Skaarj, Xan's robots ...: built from the cached
  mesh and its animation set).
* **Unlocking**: finishing the championship and making the high score list unlocks the final team's champion
  (Malcolm, the Clan Lord or Xan, `UArenaUserSettings.UnlockedCharacters` like UT2004's TotalUnlockedCharacters); we
  have no separate character select, so unlocked characters are offered in the New profile character list.
* Portraits of custom character packs: shader portraits show their base texture; a wrong group in a `.upl` path is
  ignored (resolved by name in the converted package).
* Not available yet: the scripted tutorials and the intro / ending movies (cinematic maps).

Test hooks (non-shipping):

```
# open the page: -AutoMenuPage=SinglePlayer
# scripts: -SPScript="<sec>:<cmd>;..." when the page first opens, -SPScriptAfterMatch<n>="..." when it reopens after the
#   n-th match of the process; commands: Shot=<name> (Saved/Screenshots/<platform>/sp_<name>.png), Quit, Key=<UE key>,
#   Click=<x>,<y>, SPCreate=<name>[,<character>[,<difficulty>]], SPLoad=<name>, SPDelete=<name>,
#   SPTab=Profile|NewProfile|Tutorials|Qualification|TeamQualification|Ladder|TeamManagement|Additional,
#   SPSelect=<ladder>,<rung> (0 DM, 1 TDM, 2 CTF, 3 BR, 4 DOM, 5 AS, 6 championship, 10 Onslaught), SPPlay, SPOk (team
#   roles OK), SPAutoFill, SPDone (end the draft), SPCloseDialog(s), SPDialog=Message:<n>|Details:<tab>|TeamRoles|
#   Injury:<bot>|MapChallenge|HighScores|TeamDetails:<roster>|Challenge:<class>,
#   SPCheat=Ladder,<id>,<progress>|Balance,<n>|Health,<bot>,<n>|Final,<roster class>|Completed (the same commands work in -AutoMenuScript)
# -SPTestAwards: the player reaches every special award threshold in the match (tests the award path)
# end a single player match after <sec> of play with a win / loss, back to the menu 4 s later: -SPAutoWin=<sec> / -SPAutoLose=<sec>
OpenUT2k4 -AutoMenuPage=SinglePlayer -SPAutoWin=20 -SPScript="3:SPCreate=Tester;6:SPCloseDialogs;7:SPSelect=0,1;8:SPPlay" -SPScriptAfterMatch1="4:Shot=details;6:Quit"
```

#### UT2004 HUD

With the HUD atlas in the content (`Textures/HudContent/Generic_HUD`) the in-game HUD and the scoreboards are UT2004's
(`ArenaHUDWidgets.*` widget engine, `ArenaHUDUT.cpp` player HUD, `ArenaHUDUTModes.cpp` game modes,
`ArenaHUDScoreboard.cpp`); without it the procedural HUD in `ArenaHUD.cpp` is drawn. The values are the stock class
defaults and the drawing follows the classes' script (read from the player's `XInterface.u`, `Onslaught.u`,
`UT2k4Assault.u`, `SkaarjPack.u`, `BonusPack.u` while this was written; nothing of it is shipped):

- **Widgets** (`HudBase.SpriteWidget` / `NumericWidget`, `ArenaHudUT::FSpriteWidget` / `FNumericWidget`): size
  `|X2 - X1 + 1| x TextureScale x 0.95 x ResScale x HudScale`, position `Pos x 0.95 x Clip + 0.025 x Clip + Offset x
  TextureScale x 0.95 x ResScale x HudScale`, then the `DrawPivot`; colour `Tints[TeamIndex]` (0 red team, 1 blue team
  and every non-team game). UT2004 scaled widths by `SizeX / 640` and heights by `SizeY / 480`, stretching on wide
  screens; here both follow the height (`ResScale = SizeY / 480`), and widgets keep their screen edge: `PosX` 0 / 1 and
  the left / right ones (vehicle health box at 0.139, double damage icon at 0.95, ...) are laid out from that edge,
  the rest (weapon bar, team scores, centre widgets) from the centre, in a 4:3 frame of the screen height. At 4:3 this
  is exactly UT2004.
- **Player HUD** (`HudCDeathmatch.DrawHudPassA / C`): health box bottom left (`0,110-166,163`, TS 0.53, team colour
  200,0,0 / 50,64,200), cross icon `74,165-123,216`, digits TS 0.49 at offset 174,-29 (orange 255,160,0 fading back
  after damage), pulsing outline `125,458-291,511` below 26% health; shield box above it (offset -54) only while there
  is shield; ammo box bottom right with the ammo icon (offset -28,-30) and digits (-174,-29), pulsing below 15%; weapon
  bar at PosX 0.139 ... 0.78 (slots `0,39-94,93`, ammo strip `0,94-94,109` cut to the ammo left, the current slot
  raised 10 and highlighted, missing weapons as black silhouettes of the stock icon, pickup animation); adrenaline top
  right (black capsule A 150, disc, pill TS 0.33, count TS 0.49 at -60,35, pulsing glow `Generic_GlowCircle` when
  full); timer top left (capsule TS 0.4 at 60,14, clock icon, `mm:ss` TS 0.32 with the colon `495,91-503,112`;
  remaining time with a time limit, else elapsed); DM score / rank / spread under it (score TS 0.49 at PosX 0.015
  offset 70,94 on a capsule at 0,64; rank and spread TS 0.3 at 30,132 / 30,170); double damage `0,164-73,246` TS 0.75
  at 0.95, 0.75 with its seconds; crosshair `Crosshairs/HUD_Crosshair_*` at its TextureScale x 0.5 (0.75 cross / dot,
  0.7 triad, 0.6 pointer / bracket, 0.4 circle) and HudScale 1, growing for 0.4 s after a pickup (Settings >
  Crosshair "Default" = UT2004's default Cross1). Animated materials: `HUDPulse` / `fbLinks` (atlas x FadeColor
  0.1 s), `fbHUDAlertSlow` (0.4 s), `rotDomRing` / `rotReticle001` (26000 / 65536 turns a second).
- **Team scores** (`HudCTeamDeathMatch.ShowTeamScorePassA`): two black capsules at the top centre (offset -50 / 50,
  10), discs, team symbols (`GRI.TeamSymbols`, TS 0.1), the `VS` sign `435,0-508,31`, scores TS 0.54 at -90 / 90, 32.
  CTF: the flags `336,129-397,170` in team colour (blue mirrored), "taken" `341,211-366,271` / "dropped"
  `77,271-116,311` (pulsing), the bases' direction dots (`Draw2DLocationDot`, `340,432` 78 x 78). Bombing Run: disc and
  ball `79,223-116,264` (TS 0.7; white at home, own team colour, pulsing enemy colour, pulsing white when loose) with the
  held / dropped marks, a dot towards the ball (holding it: the enemy goal), the turning `Reticles_Reticle001` on the
  locked pass target. Double Domination: capsules moved out by 0.058, letters `0,451-63,511` / `64,451-125,511`
  (TS 0.33) in the point state colour over the turning `DomRing`, direction dots, "held" marks. Onslaught: capsules
  at 0.442 / 0.558, cores `365,233-432,313` with their health in percent (flashing while hit), the radar map top right
  (map `LevelInfo.RadarMapImage`, range from the primary terrain or `CustomRadarRange`, `ONSInterface-TX/NewHUDicons`
  node icons, power links, node numbers, `CurrentPlayerIcon`, frame `MapBorderTEX`), world health bars, and in a
  vehicle its health box at PosX 0.139. Assault: no team scores, timer or adrenaline; the round time pod top left and
  the objective type icons (`AS_FX_TX/Icons_OBJ_*`, `Icons_HoldObjective`) in the defender's colour where in sight, with
  their health / progress bar (`HUD_OBJ_Status` ring for defenders). Invasion: the monster radar (`HUD_SkinA` pulse
  rings and dots, `AS_FX_TX/HUD_AssaultRadar`) instead of team scores. LMS: score, players left and lives in the score /
  spread / rank widgets. Mutant: the centre ring (`MutantSkins/HUD_big_circle`) with a dot towards the Mutant (blue
  above, red below) and range dots, its name in a box at the top, the bottom feeder icon, the Mutant's own HUD in
  200,0,128,100. Game messages (flag taken, ...) use `CriticalEventPlus` (0,160,255, font size 1, 83% height), pickups
  `PickupMessagePlus` (white, 90%).
- **Damage** (`HudBase.DisplayHit` / `DrawDamageIndicators`): red gradients (`Generic_HUD 395,209` / `404,182`) along
  the screen edges facing the hit, wider when it came from straight that way, fading 120 alpha a second.
- **Dead / spectating / match over** (`DrawSpectatingHud`): no HUD, a bar along the bottom (`Menu_SquareBoxA` with its
  blue removed: black inside, yellow frame) with "You were killed..." / "Now viewing <name>" / the match result.
- **Scoreboards**: `ScoreBoardDeathMatch` (rows of `Menu_ScoreBoxA` at half white over 93.75% of the width, PLAYER /
  SCORE / DEATHS at 6.25 / 50 / 68.75%, the local player gold, the title "SKILL game in map" and the limits line in
  gold; LMS shows LIVES / OUT, Invasion the team score box and the wave) and `ScoreBoardTeamDeathMatch` (two columns of
  `Menu_ScoreBoxC` red / `Menu_ScoreBoxB` blue, symbol and score above each, `Menu_ButtonBob` separators in team colour).
  Flag / ball carriers get the HUD flag icon where UT2004 drew a logo. Wider than 16:9 the board keeps a 16:9 area in
  the middle. Strings come from the install's `XInterface.int` etc. (our wording without them).
- **Big screens**: everything follows the screen height. The bitmap fonts are picked for `height / k` (as UT2004 would
  for a 4:3 screen of that size) and drawn k times bigger, k = round(height / 1080), with nearest filtering; the 1:1
  parts (DrawTileStretched corners) scale the same way.
- **Settings**: HUD Scale (UT2004's `HudScale`, 50 - 150%) and HUD Width (Full Width, 21:9, 16:9 or Custom with HUD
  Width (Custom) 50 - 100% of the screen): the edge anchored widgets, the kill feed, the radar maps and the death bar
  sit in a centred region of that width (never narrower than 4:3 of the height), the scoreboard never gets wider than
  it, centre widgets stay. On a 16:9 screen the 16:9 setting changes nothing.
- Test hooks (non-shipping): `-HudWidth=<0..3> [-HudWidthPercent=<50..100>]` override the HUD width;
  `-AutoHudShow=Kill:<from>-<to>` kills the local player in that window (death bar screenshots).
- Not reproduced: the voice chat portrait, weapon specific info (`NewDrawWeaponInfo`) and charge bars, the link gun's
  link counter, the Assault info pods / 3D arrow / off-screen trackers / objective board / reinforcement pod (no
  reinforcement waves here), the scoreboard's bot orders, locations and NET column (standalone games), and the LMS logo on the death bar (logos are never used).

#### HUD fonts and UT2004 messages

The converter exports every UE2 `Font` object of the install (`fonts/<Pkg>/<Name>.json`: glyph table, remapping,
kerning; the pages stay ordinary textures; `Convert/ArenaConvFonts.*`). Caches converted before that get the fonts from
the startup custom content pass (stage "Updating fonts", `ArenaConverterVersions::FontsVersion`,
`<cache>/fonts/index.json`), or from any converter run; `-ConvertFontsOnly [-ConvertTo=<cache>]` exports only the
fonts headless and quits. At runtime
`ArenaRuntimeContent::LoadFont("Fonts/<Pkg>/<Name>")` builds an offline `UFont`, and `FArenaDraw::TextUT` /
`TextSizeUT` draw with the HUD's fonts (`ArenaHudFonts.h`: HudBase `FontArrayNames` = `UT2003Fonts.FontEurostile37` ...
`FontSmallText`, picked like `HUD.GetFontSizeIndex` / `GetConsoleFont` for the width of a 4:3 screen of the canvas
height, `FArenaDraw::UTClipX`) at their native pixel size, times round(height / 1080) on big screens: the font is then
resampled to that size and drawn 1:1 (`ArenaHudFonts::GetScaled`, `ArenaRuntimeContent::LoadFontAtHeight`), smooth
instead of point-sampled blocks.
Without fonts in the cache (cooked import mode, an old cache) the engine font is scaled to the same height.

These messages follow the stock UT2004 message classes (`ArenaLocalMessages.h` has the table and the layout rules):

| Message | UT2004 class | Colour | Font (1280 / 1920 wide) | Place, life |
|---------|--------------|--------|--------------------------|-------------|
| 3-2-1 match start, "match has begun" | `StartupMessage` (stages 2-4, 5) | 32,64,255 | size 1: Eurostile24 / 29 | centre at 83% height, 3 s fade, replaces the previous stage; beep `MenuSounds.selectJ` on 3/2/1, status announcer `Play` at the start |
| You killed X | `xKillerMessagePlus` | 0,160,255 | size 0: Eurostile21 / 24 | 10% height, stacks downwards, 3 s fade |
| You were killed by X | `xVictimMessage` | 255,0,0 | size 0 | 10% height stack, 6 s fade |
| Head shot | `SpecialKillMessage` | 255,0,0 | size 0 | 10% height stack, 3 s |
| Multi kills | `MultiKillMessage` | 255,0,0 | 0 / 1 / 2 / 3 by level | 24.2% height stack, 3 s |
| Sprees / spree ended | `KillingSpreeMessage` | 0,160,255 | 0 (2 for your own spree) | 83% height, 3 s |
| First blood | `FirstBloodMessage` | 255,0,0 | 1 | 83% height, 3 s |
| Kill feed | `xDeathMessage` console line | 0,255,0 | console: Eurostile12 / 14 | bottom left (`ConsoleMessagePosY` 0.87), 4 lines, 3 s, no fade |

Positions use `HudCanvasScale` 0.95 and are centred on the text. Texts come from UT2004's localization files at runtime
(the kill feed uses the stock damage type of the UT2004 weapon each of ours stands in for); without them our own
wording is used. The English `.int` files leave out the weapon damage types (their `DeathString` / `MaleSuicide` are
only in the defaultproperties), so the kill feed reads those from the damage type's class defaults in the cache's
script packages through the UnrealScript runtime (cached per damage type), before falling back to
`Engine.DamageType`'s "%o was killed by %k.". Telefrags use `XWeapons.DamTypeTeleFrag`, `Engine.DamTypeTelefragged`
and `Engine.Gibbed` as UT2004's `UnrealPawn.gibbedBy` picks them.

**Localization files in the cache.** The converter copies every `.int` and `.ucl` file (and, since localization
version 2, the `.upl` character lists the single player tournament reads) of the install's `System`
folder to `<cache>/localization/` (with `index.json`, `ArenaConverterVersions::LocalizationVersion`,
`Convert/ArenaConvLocalization.*`), so the HUD, message, scoreboard, menu and script texts keep working when the install
is moved or deleted after converting, or when the game cannot find it. Caches converted before get them from the
startup content pass (stage "Updating language files", copies only these files) while an install is found.
`ArenaContent::FindLocalizationFile` (case-insensitive) looks in the cache first, then in the install; it is used by
`ArenaLocalMessages::Localize` (HUD, messages, scoreboards, menu captions) and the UnrealScript runtime's localized
text; custom content `.int` files are still read from the UserContent folder. The files stay in the player's cache and
are never shipped.

**Script packages in the cache.** The same way the converter copies the install's `System/*.u` script packages and
`Default.ini`, `DefUser.ini`, `UT2004.ini`, `User.ini` to `<cache>/scripts/System/`, and the maps that define their own
script classes (23 stock maps, about 580 MB, e.g. DM-UCMP4-Mania, DM-CBP2-Reconstruct, AS-BP2-Outback) to
`<cache>/scripts/Maps/` (`Convert/ArenaConvScripts.*`, `ArenaConverterVersions::ScriptsVersion`, startup stage
"Updating script packages"; about 610 MB in all). The folder is laid out like an install, so the UnrealScript runtime
uses it as its game folder (`ArenaUScript::EnsureRuntime`) and adds the install's packages when an install is found;
the Instant Action mutator list reads the `.ucl` files from `<cache>/localization` and checks for packages in the cache
first. Mutators and map scripts therefore work with the install moved or deleted; custom `.u` files stay in UserContent.

To replace any of it, put a texture at the same relative path under `/Game/Replacements` (for atlases keep the
regions, or change them in `ArenaArtHUD.h` / `ArenaArtHUD.cpp`). Test hook (non-shipping): `-AutoHudShow=Scoreboard:20-24,Pause:26-28,Settings:30-33`
forces those pages on during the given world-time windows, for `-AutoScreenshot` checks.
`-AutoViewActor=<class name part>` (e.g. `ArenaFlag`, `ArenaBR_`, `ArenaDOM_`; optional `-AutoViewDist=<cm>`,
`-AutoViewHeight=<cm>`) frames the matching actors in turn, one per `-AutoScreenshot`, with a camera that follows
the target (a flag also while carried or dropped).
`-AutoDeathCam[=<count>]` films the first deaths (default 6) with a camera following the body, screenshots at 0.05,
0.5, 1.5 and 4 s (`Saved/Screenshots/death_<n>_<step>.png`). `-LogProceduralShapes` scans the world twice a second for
visible procedural stand-in shapes (cube / sphere / cylinder / cone, i.e. missing art) and logs each owner once
(`FallbackScan:` lines, summary at `-AutoQuit` or with `arena.FallbackScanReport`).

## Controls

| Input | Action |
|-------|--------|
| W A S D | Move |
| Mouse | Look |
| Space | Jump; press again near the top of a jump or dodge to double jump |
| Double-tap a direction | Dodge (in the air: wall dodge off a wall behind you) |
| V | Dodge towards the held movement keys (same rules as a double tap) |
| Left Shift (hold) | Walk (40% speed, won't walk off ledges) |
| C / Ctrl (hold) | Crouch (40% speed, won't walk off ledges, no jumping) |
| Left mouse button | Fire |
| Right mouse button | Alt-fire |
| Mouse wheel, 1 to 0 | Select weapon |
| Tab | Scoreboard |
| Esc | Menu |
| K | Suicide |
| E | Use (game mode objectives / vehicles) |
| T | Chat (`say`): opens UT2004's typing line, Enter sends, Esc cancels |
| R (or Y) | Team chat (`teamsay`) |

Chat works offline (bots do not answer), when hosting (the line goes to the UT2004 clients) and on UT2004 servers.
The console commands `say <text>` and `teamsay <text>` do the same.

### Movement

Movement follows UT2004 exactly: the values come from the player's own `Engine.Pawn`, `UnrealGame.UnrealPawn` and
`XGame.xPawn` defaults and the logic from their script (`xPawn.Dodge` / `PerformDodge` / `DoJump` / `DoDoubleJump`,
`PlayerInput.CheckForDoubleClickMove`, `UnrealPlayer.NotifyLanded`, `Pawn.TakeFallingDamage` / `AddVelocity`), with
UE2's native walking and falling physics on top of the engine's collision (`ArenaCharacterMovement.*`). 1 UE2 unit
is 2 cm. There is no Quake-style strafe jumping or bunny hopping: speed comes from dodges, double jumps, wall dodges
and weapon boosts.

- **Running**: 880 cm/s (GroundSpeed 440), acceleration 4096 cm/s^2, ground friction from the zone (8). Landing
  from anything faster than running speed drops you to running speed at once, as in UT2004.
- **Jump**: 680 cm/s up (JumpZ 340), 1.22 m high. Holding the button does not jump again on landing.
- **Double jump**: press jump again while your vertical speed is within 200 cm/s of zero (near the top of a jump,
  dodge or wall dodge): 730 cm/s up (JumpZ + MultiJumpBoost 25), one per air time. A jump plus a double jump at its
  top reaches 2.62 m.
- **Dodge**: double-tap a direction within 0.25 s (the timer runs at 1.1x, as in UT2004): 1320 cm/s
  (1.5 x GroundSpeed) in that direction plus 420 cm/s up; about 5.8 m long and 46 cm high. A sideways dodge
  keeps your forward speed and the other way round. Landing from a dodge keeps only 10% of your speed and the next
  dodge waits 0.32 s.
- **Dodge jump**: dodge, then double jump near the top of the dodge: 1.87 m high and about 13.8 m long. Jumping on
  the landing of a dodge gives only a plain jump (the landing has already slowed you down).
- **Wall dodge**: in the air, double-tap away from a wall that is within 64 cm of your feet: the same kick as a
  dodge. One dodge per air time (a ground dodge or a wall dodge), but a wall dodge works after a double jump and a
  double jump still works after a wall dodge.
- **Air control**: 35% (steering at 1434 cm/s^2); in the air you can steer but never get faster than
  max(current speed, 880 cm/s).
- **Rocket jump**: look down, jump and fire the Rocket Launcher at your feet. The blast pushes with UT2004's
  MomentumTransfer 50000 (1000 cm/s at the centre, 60% for your own rockets), so rocket plus jump reaches about
  3 m. It costs about 80 health, or about 40 at bot skill 3 or lower (UT2004 halves your own damage offline at
  those skills); a rocket alone from standing only lifts you about 0.8 m. Rockets fly at 2700 cm/s.
  Other explosions push the same way (Shard Cannon shell and Carbine grenade: 75000); Arc Linker plasma does not
  push.
- **Shield Gun jump** (our shield weapon is the **Gauntlet**, slot 1): charge the primary fire (full charge
  2.5 s), look at the floor and release while standing close to it, ideally right after jumping. A full charge
  pushes you up 1200 cm/s (3.7 m from standing, about 9 m with a jump) and costs 53 health (26 at bot skill 3 or
  lower). Above 760 cm/s of upward speed any further boost is halved, as in UT2004.
- **Lifts**: jumping off a rising lift adds the lift's vertical speed; its sideways motion is not added.
- **Partial input** (gamepad stick): UE2 `calcVelocity` has no analog speed cap, so a half-pushed stick accelerates more
  slowly but still reaches GroundSpeed / WaterSpeed / AirSpeed (UE5's `AnalogInputModifier` cap is not applied, offline
  or in net play). Flying uses AirSpeed (440 UU/s). Rig check `w_analog`.
- **Stepping into barriers** (UE2 `APawn::stepUp` / `physWalking`): a walking pawn that runs into anything it cannot walk
  on steps up MaxStepHeight (35 UU) and moves on, sliding along the barrier as if it were vertical, then drops back up to
  a step to the floor. If that lands on a steep face (slanted door frames, 47 degree ramps) or on nothing, the drop is
  undone and it falls. Against DM-Rankin's slanted `houredoorframe` legs it hops (rises ~36 UU and falls back, like a
  UT2004 server's pawn) instead of sticking; 47 degree ramps still cannot be climbed. UE5's own step up is used
  first. Rig check `w_stepup` (a face leaning back 8.6 degrees: rises 70 cm, goes airborne). Test hook (non-shipping):
  `-UE2FXTest=walkinto "-UE2FXWalk=x,y,z" [-UE2FXWalkDir=<yaw>] [-UE2FXTestTime=<s>]` walks the player from a UE2
  location for 4 s and logs its path and floor.
- **Steps and slopes**: steps up to 70 cm, slopes up to 45.6 degrees (UE2 MAXSTEPHEIGHT 35, MINFLOORZ 0.7).
- **Crouch**: 352 cm/s, 1.16 m tall; no jumping or dodging while crouched; you can't start crouching in the air.
- **Falling**: landing faster than 2400 cm/s hurts: 100 x (speed - 2400) / 2400 (about 15 for a 20 m drop).
- **Terrain**: as in UE2, terrain only blocks from above. A pawn rising from under a terrain, e.g. out of a pit
  through a hole in it (BR-Colossus' pit pads), passes through its underside until its feet are above the surface
  (console `arena.UE2TerrainOneSided 0` turns this off). Walking pawns collide with terrain as before.
- **Water**: swim at 440 cm/s along the view; Space swims up, Crouch down; swimming forward and up against a low
  edge jumps you out (940 cm/s). After 20 s with your head under water you drown: 5 damage every 2 s.
- **View**: eyes 76 cm above the body's centre (38 cm when crouched, the view lowers smoothly); landings faster
  than 400 cm/s dip the view; walking bobs it sideways (0.006 x speed, 5.3 cm at full run) and up and down
  (4 cm). Air control drops to 5% while it would push you up a walkable slope (no climbing slopes in the air).
- **Zones**: everything uses the zone's gravity (UE2 -950 = -1900 cm/s^2), so low gravity raises all jumps.
- **Speed combo**: ground, air and water speed and air control x1.4, jump x1.5 (dodges follow the ground speed).

Test hook (non-shipping builds): `-MoveTest=all` (or a list such as `-MoveTest=jump,dodge,walldodge,rocketjump`)
builds a flat test rig (floor, wall, 70 / 76 cm steps, 44 / 47 degree ramps, a lift) far outside the map once the
match runs and puts the player's pawn through scripted moves, fed through the same entry points as the input.
Each test logs samples (`MoveTest: <test> t=... z=... vz=... hspeed=...`) and `MoveTest: RESULT <test> <metric>
expected=<UT2004 value> measured=<value> OK|MISMATCH` lines, then `MoveTest: done, N checks passed, M failed`.
`-MoveTestShots` takes screenshots at key moments (`movetest_<test>_<moment>.png`), `-MoveTestQuit` exits when done.
`-MoveTest=botwatch` (with `?Bots=N -AutoSpectate`) logs how many jumps, double jumps, dodges and wall dodges the bots
made after 60 s, their combat counters (shot warnings, dodges away from shots, strafe jumps, dodges towards path
nodes, shock combos, locked rockets) and each bot's turn rate and combat timer against UnrealGame.Bot's formulas. Use `?Bots=0` for the rig tests, and `-UseFixedTimeStep -FPS=120` for frame-rate independent numbers:

```
UnrealEditor OpenUT2k4.uproject "/Game/Maps/Arena?Mode=DM?Layout=DM-Rankin?Bots=0" -game -RuntimeContent
  -ContentCache=<dir> -UseFixedTimeStep -FPS=120 -MoveTest=all -MoveTestShots -MoveTestQuit
```

### Adrenaline combos

When you have 100 adrenaline, tap a movement sequence (each tap within 0.35 s of the last) to start a combo. The
adrenaline drains over the combo's duration (twice as fast while you carry a flag or the ball):

| Sequence | Combo | Duration | Effect (UT2004 XGame combos) |
|----------|-------|----------|------------------------------|
| Forward Forward Forward Forward | Speed | 16 s | ground / air / water speed and air control x1.4, jump x1.5 |
| Back Back Back Back | Booster | 30 s | +5 health every 0.9 s up to 199, then +5 shield |
| Right Right Left Left | Invisible | 30 s | invisible |
| Forward Forward Back Back | Berserk | 30 s | fire rate x1.33 (FireRate x0.75), assault rifle / minigun / link beam damage x1.33, shield blast x2 |

Adrenaline: +5 per kill (+2 in CTF, Bombing Run, Double Domination, Onslaught and Assault), +10 for first blood,
a multi kill, every 5 kills of a spree and ending someone's spree, -2 for a suicide, -5 for a team kill; an
adrenaline pickup gives 2.

### Weapons and items

The weapons keep their own names; their numbers and behaviour are UT2004's (XWeapons fire mode, projectile, ammo
and pickup classes; 1 UE2 unit = 2 cm, momentum = MomentumTransfer / 100 x 2 cm/s):

| Ours (slot) | UT2004 | Primary | Alt | Ammo (start / max / pickup) |
|-------------|--------|---------|-----|------------------------------|
| Gauntlet (1) | Shield Gun | charge 0.4-2.5 s: 40-150 damage, 224 cm reach; releases by itself on a pawn in reach; Shield Gun jump | shield: takes up to 2 x energy (max 100) of frontal damage for half that energy; needs 15 to raise, 5 energy/s | 100 energy, regains 5/s |
| Carbine (2) | Assault Rifle | 7 damage every 0.16 s, spread 0.02 growing to 0.12 | grenade (own ammo 4 / 8): hold for 1700-3200 cm/s, bounces, explodes 2 s after the first bounce or on a player (70, 440 cm) | 100 / 200; pickup +50 bullets +4 grenades; a second rifle = dual (fire rate x1.8, spread x1.5, double ammo) |
| Goo Launcher (3) | Bio Rifle | glob 4000 cm/s every 0.33 s: 20 + 19 x goop damage, sticks 2.25 s | charge 1 goop per 0.25 s up to 10, bigger globs split on landing | 20 / 50 / 20 |
| Pulse Rifle (4) | Shock Rifle | beam 45, 0.7 s | core 2300 cm/s, 45 (300 cm); beam on a core = combo 200 (550 cm) costing 3 more ammo | 20 / 50 / 10 |
| Arc Linker (5) | Link Gun | plasma 30, 2000 accelerating to 8000 cm/s, 2 ammo every 0.2 s | beam 9 every 0.12 s, 2200 cm; linking team mates raises damage and range | 70 / 220 / 50 |
| Rotary Gun (6) | Minigun | 7-8 at 15/s after 0.27 s wind-up, spread 0.08 | 14-16 at 5/s after 0.15 s, spread 0.03 | 150 / 300 / 50 |
| Shard Cannon (7) | Flak Cannon | 9 chunks 5000 cm/s, 13 each (fading after 1 s), 0-2 bounces | shell 2400 cm/s lobbed, 90 (440 cm) + 6 chunks | 15 / 35 / 10 |
| Rocket Launcher (8) | Rocket Launcher | rocket 2700 cm/s, 90 (440 cm), 0.9 s; homes when locked on (aim at an enemy for 1.25 s) | load up to 3 (0.95 s each, fires itself at 2.3 s), spread; press primary while loading for a spiral | 12 / 30 / 9 |
| Rail Rifle (9) | Lightning Gun | 70, headshots x2, 1.6 s, 3 side arcs (35) from the hit point | zoom (hold: up to 0.9 s, press again to zoom out) | 15 / 40 / 10 |
| Insta Rifle | Super Shock Rifle | 1000, 1.1 s (instagib) | same | - |
| Warhead Launcher (0) | Redeemer | warhead 2000 cm/s, shootable (an enemy's hit makes it fizzle: that damage in 500 cm); impact: 250 damage in six rings growing to 4000 cm over 1.3 s | fly it yourself: your view follows it (mouse steers, scope overlay), fire again to detonate; your body stays behind | 1 / 1 |
| Orbital Designator (0) | Ion Painter | hold the beam on one spot under open sky for 1.2 s: the ion cannon strikes it 0.5 s later, 150 damage in six rings growing to 4000 cm (one strike per pickup) | zoom | 1 / 1 |
| Marksman Rifle (9) | Classic Sniper Rifle | 60, headshots x2, 1.33 s | zoom | 15 / 35 / 10 |
| Seeker Launcher (8) | AVRiL | rocket 1100 cm/s accelerating to 5600, 125 (300 cm), 4 s reload; homes on the enemy vehicle you keep in your sights (locks within 30000 cm) | while locked: zoom and keep the view on it | 5 / 25 / 5 |
| Sticky Launcher (7) | ONS Grenade Launcher | grenade 2400 cm/s, bounces, sticks to players and vehicles; up to 8 out (they go off when you die) | detonate them all: 100 (350 cm) each | 10 / 50 / 5 |
| Crawler Layer (3) | ONS Mine Layer | spider mine (8 out at most): waits, runs at enemies within 1500 cm at 1050 cm/s and leaps at them, 95 (500 cm) | laser: the mines walk to where it points | 4 / 25 / 8 |

The last six come from maps that place them (the converter and the level builder map UT2004's `Redeemer`, `Painter`
/ `ONSPainter`, `ClassicSniperRifle`, `ONSAVRiL`, `ONSGrenadeLauncher`, `ONSMineLayer` weapon bases, lockers, pickups
and ammo pickups to them; map data converted before they existed is mapped again from its raw markers), from
mutators and UnrealScript that ask for those classes. Weapons sharing a number key cycle when you press it again (UT2004
inventory groups). A second assault rifle shows in your left hand in first and third person (the same rifle mirrored,
swinging up over 0.5 s, and the third-person rifle on the left hand bone).

The Gauntlet's shield sends shock beams on along your view (up to four bounces; UT2004 `ShockBeamFire.bReflective`),
shock cores straight back along your view and link plasma mirrored about it (`ShockProjectile` / `LinkProjectile`
`ProcessTouch` with `xPawn.CheckReflect`), each costing a quarter of the shot's damage in shield energy; the shot stays
its shooter's.

**View shake** (`ArenaViewShake.*`, UE2 `PlayerController.ShakeView` / `ViewShake` / `CheckShake`): the player's
camera manager runs UT2004's shake (rotation and offset magnitudes, rates and shake counts per axis) and adds it to the
view of the player's own pawn or vehicle. Every shot shakes with its UT2004 fire mode's `ShakeRotMag` / `ShakeOffsetMag`
values (`WeaponFire.ShakeView` -> `WeaponShakeView`, x (1 - zoom)), e.g. the rocket and flak kick of 20 UU back, the
minigun / assault rifle rattle, the link beam roll; the Bulwark cannon, its repeater and the Caravan's mortar use their
`ONSWeapon` values. Instant Action > Rules has UT2004's "Weapons Shake View" checkbox (`GameInfo.bWeaponShouldViewShake`,
saved as `bWeaponViewShake`); like UT2004 it only turns off the weapon shakes. Redeemer and ion cannon blasts
(`RedeemerProjectile` / `IonCannon.ShakeView`: full within 2000 UU, fading to nothing at 4000 UU; the ion cannon only
for players standing on something) and vehicle explosions (300 UU x 2.5) shake everyone near, a driven Bulwark (`ONSHoverTank` bEnableProximityViewShake:
ViewShakeRadius 600 UU, ViewShakeOffsetMag (0.5, 0, 2) UU at ViewShakeOffsetFreq 7 Hz, fading with distance; an
approximation of UE2's native code) rumbles the view of players near it, damage taken
(`ClientDamageShake`) and hard landings (`Pawn.TakeFallingDamage`) shake the victim. `arena.LogViewShake 1` logs every
shake.

Projectiles start where UT2004 starts them (`ProjectileFire.ProjSpawnOffset` from the eye: rockets 50 cm ahead, 12 cm
right, 12 cm down, flak shells 18 right / 24 down, and so on) and a player's fly parallel to the view; the muzzle flash
stays on the gun model. A player's shock beam starts at the shock core's offset too.

Switching takes the old weapon's put-down (0.33 s) plus the new one's bring-up (0.33 s, Lightning Gun 0.36 s).
Weapons stay: a weapon pickup you already own can't be picked up again (the assault rifle makes you dual-wield);
every pickup respawns after UT2004's time: health, vials, shield packs, adrenaline, ammo and weapons 30 s, super
health and super shield 60 s, double damage 90 s. Health pack +25 up to 100, vial +5 and super health +100 up to
199, shield packs 50 / 100 up to 150 (armour takes 100% above 100 shield, 75% of the big shield part and 50% of the
small shield part), double damage 30 s (a second one restarts the 30 s). In non-team games super health, vials and
shields are taken even when you can't use them. Spawn protection (2 s, Assault 3 s, ends when you fire) only
stops other players' damage.

#### Translocator

The Translocator (key **Q**, UT2004's `SwitchWeapon 10`; also on key 0 after the super weapons) is UT2004's
`XWeapons.TransLauncher` with `TransFire`, `TransRecall` and `TransBeacon`. Its values are the stock defaults, and its
behaviour follows the stock script. Both were read from the player's `XWeapons.u` / `UnrealGame.u` while this was
written:

- **Throw** (fire): the beacon leaves `ProjSpawnOffset` (25, 8, -10) UU from the eye at 1200 UU/s (2400 cm/s) along the
  view and does not inherit the thrower's velocity. It falls with the gravity and `TerminalVelocity` of its physics volume. Each wall, floor or pawn
  hit sets velocity = 0.3 x the mirrored velocity, and it rests on a floor below 20 UU/s. Slow beacons (under 200 UU/s
  across) pass through pawns. Walking over your resting beacon picks it up. A beacon can't come to rest on a flag base:
  it keeps hopping and gets disrupted.
- **Recall** (fire again): the beacon returns and no charge is used. A disrupted beacon can't be recalled.
- **Translocate** (alt fire): you appear at the beacon, raised by your height (half of it while it flies). Your
  horizontal speed is kept up to ground speed. For 1 s the player model shows the panning team-coloured cell overlay
  (`TransRecall.TransMaterials` `PlayerTransRed` / `PlayerTrans`). A carried flag or ball is dropped where you were
  (with half your velocity). Enemies at the destination are telefragged (gibbed: "had their atoms scattered by"). Team mates and players
  still inside their spawn protection make the translocation fail (`TranslocatorError` sound).
- **Charges**: 6 (`AmmoChargeF` / `AmmoChargeMax`), regained at 0.4 per second (2.5 s per charge). A throw needs 1 but
  doesn't use it; each translocation uses 1. The HUD shows the whole charges as the ammo count, plus the recharge bar
  of `HudCDeathmatch.DrawChargeBar`. In Bombing Run, shooting the ball drains it (`bBallDrainsTransloc`: -1 charge,
  back to 1.5 as soon as anyone holds the ball).
- **Disruption**: damage to the beacon adds up; in team games only enemies' damage counts. Above 65
  (`DisruptionThreshold`) it sparks, and translocating to it gibs you, credited to the disruptor ("was telefragged by"
  when the disruptor wasn't holding a translocator).
- **Camera**: press Q again while holding it to look through the beacon (`TransLauncher.Reselect`). You get the
  `TransCamFB` overlay and screen noise that grows with the distance (up to `MaxCamDist` 4000 UU). Press Q again,
  fire, translocate or switch weapons to get your own view back. Press fire while alt fire is still held to switch
  back to the previous weapon (`bPrevWeaponSwitch`).
- **Art and sound**: the first-person `NewTranslauncher_1st`, the third-person `NEWTranslauncher_3rd` (blue team:
  `NEWTranslocatorBLUE` skin), the `NEWTranslocatorPUCK` beacon with a team-coloured trail and resting flare, and the
  stock fire, recall, error, select and impact sounds.
- **Who gets it**: UT2004's `bAllowTrans`. It is on by default in CTF and Bombing Run (`bDefaultTranslocator`) and off
  elsewhere. Instant Action > Rules has a "Translocator" checkbox, saved per game mode, and the in-game Rules tab
  shows the setting. The URL option is
  `?Translocator=0|1`. Instagib leaves it out, like `MutInstaGib`'s default `bAllowTranslocator` off. Script mutators
  see it as `XWeapons.TransLauncher` (`CheckRelevance`).
- **Bots** (skill 2+, never while carrying the flag / ball, `TransLauncher.FireHack` / `MonitoringThrow`):
  - **Translocator routes**: the map's `ForcedPaths` to JumpSpots become `Translocate` nav links (`JumpSpot.TryTranslocator`).
    A link is skipped when a forced jump already covers the pair, unless the spot is `bOnlyTranslocator`. It is also
    skipped when there is no walkable floor under the spot in our build: the log line "no floor under the translocator
    spot" points at missing map geometry, for example CTF-Maul's upper decks. The bot stops
    at the start and throws with `AdjustToss` at the spot raised by its `TranslocZOffset`. It translocates once the beacon
    is over the spot's cylinder. If the throw misses, the route is left alone for 3 s and the bot plans a new path.
  - **Hops** (`Bot.FaceActor`): bots throw ahead along their route to move faster, to catch up with an enemy carrier,
    and at the flag / ball when it is close and in sight. The rules are UT2004's: skill and tactics checks,
    `TranslocUse` 1, the `TranslocFreq` / `minDist` back-off and `AlternateTranslocDest`. Our nav graph is denser than
    UE2's path nodes, so "MoveTarget" is the path point about 12 m ahead. One addition to UT2004: hops need safe
    floor (no void / KillZ) at the target and under the beacon when they translocate.
  - **Enemy beacons** (`SquadAI.ShouldDestroyTranslocator`): a bot without an enemy shoots an enemy beacon that is in
    sight within 1500 UU and not disrupted yet.
- CTF-Face3's four `TransbeaconTeleporter` triggers point at JumpSpot tags (`wicked1`-`wicked4`) that no actor in the
  stock map carries, so they do nothing, as in UT2004.
- Test hooks (non-shipping): `-MoveTest=w_transloc` (part of `weapons`), and `-TranslocShots`, which takes
  screenshots of the weapon, the beacon, the camera and a translocation in a match and then quits. Add `-TranslocLog`
  (or `-BotDebug=`) to log every bot hop, throw and translocation ("BotTransloc: ...").

Weapon test hook (non-shipping): `-MoveTest=weapons` (or `w_rates`, `w_damage`, `w_proj`, `w_combo`, `w_bio`,
`w_rocketload`, `w_switch`, `w_shield`, `w_reflect`, `w_dual`, `w_extra`, `w_armor`, `w_pickups`, `w_combos`,
`w_transloc`, `w_shake`, `w_fall`, `w_vehrest`, `w_analog`, `w_stepup`) checks
fire rates, ammo use, wind-up, damage per hit, projectile speeds / gravity / lifespans, the shock combo, bio charge,
rocket loading, switch times, the shield gun and its reflections, dual rifles, the six weapons above (warhead speed and
rings, guided flight and the return of control, ion strike timing and rings, sticking and detonating grenades, mine
runs, the seeker's acceleration and kick), armour, pickups, combos and the translocator (throw speed and gravity,
bounce damping, rest, recall, teleport spot and charge use, flag drop, the 2.5 s recharge, no throw below one charge,
telefrag, disruption at 65 and the gib), the view shake (the Redeemer blast's first and second roll swing and offset,
the rocket kick, the "Weapons Shake View" setting, the damage shake, the blast's falloff with distance) and falling
physics (the level's terminal velocity for a falling bio glob and an ONS grenade, a vehicle in a volume with sideways
gravity and in a ZoneVelocity volume) against UT2004's values on the test rig, with a target dummy.
Timing checks follow game time (projectile flight time, the blast and pawn where a rocket went off, the frame a
shot is seen in); for a run that does not depend on machine load add `-benchmark -fps=60` (fixed 1/60 s steps; the
full `-MoveTest=all,weapons` passes all its checks that way).
`-MoveTest=view,airslope,swim,drown` covers the view, slope air control, swimming / water jump and drowning, and
`-MoveTest=swimmap` swims in a water volume of the loaded map (e.g. `?Layout=CTF-Face3`).

### Bots

Bots follow UnrealGame.Bot at their skill (0-7, `?Skill=`), with an average UT2004 character's profile (accuracy,
reaction time, jumpiness and strafing 0):

- **Seeing and turning**: peripheral vision cos 0.7 below skill 2, 1 - 0.2 x skill, -0.2 above 6; turn rate
  30000 + 4000 x skill rotator units per second, 7000 + 10000 x skill from skill 4 (`ResetSkill`).
- **Reaction**: they shoot on a repeating combat timer of 1.2 - 0.09 x skill seconds (`SetCombatTimer`), so the first
  shot at a new enemy comes up to one period later.
- **Aim**: every shot gets UT2004's yaw error (`AdjustAim` / `AdjustAimError`): the weapon's AimError (600, assault rifle
  800, minigun 900, shock 700, lightning / classic sniper 850) scaled by how much the target moves across the line of
  fire, x0.5 against a Shield Gun in close, smaller against standing or straight-running targets, larger for the first
  0.5-4 s on a new enemy, while hurt or in the air, never more than 100 UU (instant hit) / 320 UU wide of the target;
  leading moving targets from skill 4, aiming at the feet with splash weapons, at the head when sniping and standing.
  The view snaps to the shot like UE2's `SetRotation(FireRotation)`.
- **Dodging**: shots aimed at them warn them (`InstantWarnTarget` with the fire mode's WarnTargetPct, every projectile
  aimed at them via `ReceiveProjectileWarning`); from skill 3 they may dodge sideways (`TryToDuck`: 335 UU clear
  with floor at the end), from skill 4 for instant hits. Each new strafe move may be a strafe jump with a double jump
  at the apex (chance 0.05 x skill) or, with non-splash weapons, a sideways dodge (`FightEnemy`). From skill 3 they
  dodge towards far path nodes (DodgeToGoalPct skill / 6, then double jump: `MayDodgeToMoveTarget`).
- **Strafing**: on the way to an objective they look at the enemy or along the path as `FaceActor` decides (always
  the enemy from skill 6).
- **Weapons**: rocket lock-on at long range (`RocketLauncher.SpawnProjectile`: skill > 2 + 5 x FRand, 60%, beyond
  4000-8000 cm, enemy acquired over 1.5 s ago); shock combos (`ShockRifle.BestMode`: a core at the enemy, the beam when
  the core is within 275 cm of them or passes them; proficiency 2 + 4 x FRand < skill).
- **Incoming warheads** (`RedeemerProjectile.Timer`, every 0.5 s): enemy bots of skill 2+ that are not firing and have
  no enemy in sight shoot at an unguided warhead they can hit (`Focus` / `FireWeaponAt`,
  `AArenaBotController::SetShootTarget`), leading it. As in UT2004 the warhead's team is its instigator's, so this
  happens in team games only, and guided warheads are not shot at this way.
- **AVRiL** (`ONSAVRiL.GetAIRating` / `WeaponTick`): rated 0.55 against a vehicle (+0.1 when it is 200 UU above, more
  beyond 2000 UU, up to 2) and 0 against anything else, so bots switch to it when their enemy drives; the bot's enemy's
  vehicle is locked at once while the line to it is clear, and the rocket homes on it. Shots are logged
  (`BotAVRiL: <bot> fired at <vehicle> (locked 1, <m>)`).
- **Onslaught vehicles**: bots claim free vehicles near their route (`UArenaRules_ONS::FindVehicleForBot`), drive them
  to the node they attack or defend with the built-in vehicle AI and fire the vehicle's guns at enemies; enemy riders
  stand in for their vehicle as targets.
- **Exception**: bots double jump on navigation jump links at any skill (UT2004 only lets skill 3+ double jump),
  because our generated nav graph has links that need it; tactical double jumps follow the skill rule.
- **Jump pads and map jumps**: after a jump pad launches a bot, the bot steers in the air to land on the pad's target,
  like UE2's `JumpPad.PostTouch`, which sets MoveTarget to JumpTarget. It aims for the horizontal speed that reaches
  the target as the fall gets back to its height. The map's own `ForcedPaths` to JumpSpots are nav links when a jump
  at up to dodge speed with up to a double jump's lift reaches them. The bot then leaves with that velocity, like
  `JumpDest.DoJump`: this is how bots cross BR-Skyline's low-gravity islands. Physics volume streams (BR-Colossus'
  tubes) link every node inside them to the stream's exit pad.
- **Level gravity**: the nav graph's jump and fall reach use the map's gravity. A gravity saved on the map's
  DefaultPhysicsVolume is the world gravity, as in UE2: -600 on BR-Skyline and DM-Plunge, -425 on DOM-OutRigger.
- **Void and kill zones**: bots never walk off ledges where they would fall below the world kill Z, or below a UE2
  zone's own KillZ (e.g. the holes under BR-Skyline's goals), or onto a floor inside a lava / pain / kill volume
  (CTF-Face3's fall volumes under the bridges), or into a zone whose KillZ is above the floor (CTF-Citadel's canyon).
  A ledge counts where the fall would land at the bot's speed, not straight below the edge, and a bot running at a
  ledge brakes first. The floor checks see what blocks pawns (decoration pawns walk through is no floor). Drop and
  jump links of their path are taken at full speed even in a fight: the nav graph only links drops onto floors it
  found.
- **Hearing** (`Pawn.MakeNoise` / `Bot.HearNoise`): every shot makes a noise of loudness 1, picking up an item 0.2,
  taking the Bombing Run ball 1. A bot hears it within the UE2 HearingThreshold (2800 UU, 56 m) x sqrt(loudness)
  with a clear line, a third of that through walls. With no enemy in sight it takes the noise maker as its enemy,
  turns to it and may hunt it.
- **Memory** (`Bot.LostContact`): an enemy out of sight stays known for 7 s, 4 s for a defender in a team game. The
  bot looks where it last saw or heard it for 1.5 s, then along its way again.
- **Squad enemies** (`SquadAI.Enemies`): a team bot without an enemy takes on the nearest enemy that a squadmate
  (same team and role, attacker or defender) within 40 m is fighting.
- **Hunting**: a known enemy out of sight can be hunted (its last seen spot becomes the goal). The mode decides how
  much that weighs against its objectives (`UArenaGameRules::GetBotHuntPriority`). The default, 0.3 + 0.05 x skill,
  is below any objective.
- **Personality** (UT2004 .upl, -1..1, 0 = average; setters on `AArenaBotController`): Aggressiveness (hunt priority
  +0.3 x it, a shorter preferred range; below 40 health a bot with Aggressiveness + CombatStyle / 2 < 0.3 backs
  off), CombatStyle (preferred range x (1 - 0.3 x it)), Tactics (it takes on squadmates' enemies within
  40 m x (1 + 0.5 x it)), FavoriteWeapon (rated +0.1, proficiency +2 skill; `SetFavoriteWeaponUE2` takes the .upl
  class name). Accuracy, ReactionTime, Jumpiness and StrafingAbility as before.
- **Jump pads**: bots walk around pads their path doesn't use (UE2 bots only touch a pad on their way through), and
  Bombing Run defenders don't guard spots next to one.
- **Ledges** (`Pawn.bStopAtLedges`): a walking bot stops at a ledge instead of walking off it, unless its path link
  is a drop, jump, jump pad, lift or teleporter.
- **Weapon choice**: a new enemy (seen or heard) makes the bot pick its best weapon at once (`SwitchToBestWeapon`);
  a splash weapon whose blast would reach the bot itself (the warhead launcher up close) is not chosen against
  that enemy.
- **Nav graph and the map's own paths**: besides the sampled grid, every UE2 navigation point of the map (PathNode,
  InventorySpot, JumpSpot, Door, AIMarker, LiftExit, teleporters) whose spot fits a pawn is a node. Two of them are
  linked up to 20 m apart when the walk or drop between them works, as UE2's long ReachSpecs do on ramps, narrow
  bridges and in doorways the grid misses. Doors (movers opened by bumping them, or by any triggered mover with a
  proximity Trigger within 8 m, e.g. CTF-Face3's base doors, AS-Glacier's TriggerAdvance doors, and movers a UE2
  Door navigation point names as its MyDoor) are passages: bots open them by walking into them. The graph is built
  with each sliding door in its open pose, so links go through the gap the open panels leave, not through the
  panels; doors that rise or sink are left out of the build's collision. JumpSpot ForcedPaths marked for the translocator
  (`bOnlyTranslocator`) stay jump links when a jump reaches them, since ball and flag carriers can't translocate.

**Bombing Run** (`UArenaRules_BR`, after UT2004's `BombingRunSquadAI`):

- **Roles**: about one bot in three defends and guards spots near its own goal. A defender fights any enemy it sees,
  as in DM, and hunts enemies last seen within 40 m of its goal. It goes for the ball when the ball is close, and
  for the enemy carrier when that carrier is its known enemy (seen, heard or reported by the squad) or within 80 m
  of its goal. The other bots attack: they go for the loose ball (or the ball at its home spot) and intercept the
  enemy carrier.
- **Escorts**: while a teammate carries the ball, the attackers run a few nodes ahead on the carrier's route and
  fight there (UT2004 uses the carrier's RouteCache). Enemies within 30 m of the carrier come first: escorts hunt
  them when they are out of sight.
- **Carrier**: heads for the enemy goal. It scores 7 by jumping through the ring from a floor spot level with it, or
  by taking a jump pad that throws it through the ring (BR-Colossus). It throws for 3 from close range with a clear
  line in three cases: the ring can't be reached, the carrier prefers shooting for this possession (`ScoringRand` <
  0.3), or it is about to lose the ball. It passes with a homing pass when it is away from both bases. The receiver
  must be a healthy teammate ahead on its route, in sight, with no enemy near the ball's path; when threatened, the
  carrier passes to any such teammate. The log line "BR: bots score at the <team> goal by ..." shows the chosen way.
- **Targets**: a visible ball carrier is the preferred enemy, even more so within 30 m (UT2004 `ModifyThreat`).
- **Test hook**: `-BotDebug=<seconds>` logs each bot's state (position, goal, path, enemy, weapon, movement) at
  that interval. On Bombing Run it also logs whether ball and goals can be reached from each other.

**Capture the Flag** (`UArenaRules_CTF`, after UT2004's `CTFSquadAI`):

- **Roles**: one bot in four of each team defends (UT2004 `TeamAI.OrderList`: Defend, Attack, Attack, Freelance).
  Defenders guard spots near their flag, fight what they see, hunt enemies last seen within 35 m of it, return
  their dropped flag and go after the carrier of their flag.
- **Attackers** go for the enemy flag and fight an enemy that comes within 20 m (`SquadAI.CheckSquadObjectives` ->
  `FightEnemy`) unless the flag is within 20 m and in sight; further enemies are shot at on the way. They keep
  attacking while their own flag is away: only attackers that see the carrier of their flag or are within 25 m of
  it turn on it.
- **Alternate routes**: each trip goes via one of the map's UE2 AssaultPaths that lead to the enemy flag (picked at
  random, weighted by its Priority, if it isn't much of a detour): on the way out not the `bReturnOnly` ones, on
  the way home with the flag not the `bNoReturn` ones. Attacks and returns don't all take the same way.
- **Carrier**: runs home without stopping to hunt; when hurt (below 60 health) it picks up health or armour within
  30 m on the way. It captures only when its own flag is at home, as in UT2004; until then it waits at its base.
- **Escorts**: while a teammate carries the flag, attackers stay with it, and fight threats within 25 m of it (or
  hunt them when out of sight).

**Assault** (`UArenaRules_AS`, bot side):

- **Doors the map's script opens** (no player trigger; AS-Glacier's hangar, AS-Convoy's side doors): in Assault they are
  nav passages, but each link through one is shut while the door is at rest closed (UE2 BlockedPath). The planner routes
  around closed ones; the AS spawn choice and reachability checks may assume they open later.
- **Spawns** follow the map's own PlayerSpawnManagers (UT2004 `ASGameInfo`): at round start the ones with `bEnabled`
  for each side (AS-Glacier's attackers start in the north, not in the closed hangar), then the ones the map's events
  toggle by Tag. Later stages that no event has toggled yet use a stage table: the nearest attacker group reachable now,
  or a forward one behind script doors when the reachable one is the previous stage's and over three times further
  (AS-Convoy's 'Spawnt' trailer spawn). The spawn log line is `AS: stage N spawns: attackers ... defenders ...`.
  The groups exist from the first spawn on (the local player may spawn before the nav graph is ready); a player whose
  side isn't set yet starts with the attackers.
- **Mover crowding**: when a stage stalls on a mover (`UpdateMoverStall`), only the two attackers nearest it are sent
  to operate it, at a priority below the objective, so the rest keep attacking.
- **Movers that bring floors**: when a big scripted mover comes to rest (AS-Convoy's crane extending the boarding
  platform), the map's navigation points around it that had no floor get nodes and links. Hold / Use objectives trigger
  their `MoverTag` when completed (the crane).
- **Water**: UE2 paths through water are swum: water navigation points are nodes, linked in straight lines, and bots swim
  to their path points in 3D (AS-Glacier's flooded tunnel from the attackers' start).
- **Space fighters** (AS-MotherShip): attackers whose PlayerSpawnManager forces a fighter start in one (our Kestrel).
  A flyer attacking a destroy objective heads for the nearest of the spots in the open with a clear shot at it
  (computed once per objective), flies there in 3D (climbing over what is ahead), then slows, levels with the
  target, turns to face it so its guns bear and strafes side to side; it fires once the target is within its aim cone and 50 m. Fighters
  outside Onslaught never get out: one hung up on the hull climbs clear and picks its spot again. Vehicles engage
  objectives from 2.5 times their normal weapon range.
- `UT2004 TriggerAdvance` movers advance one key per trigger, TriggerOpenTimed movers with bTriggerOnceOnly stay open
  (UE2 `GotoState('')`), and Trigger type 6 (LivePlayerProximity) fires for players and bots. `Trigger_ASTeam` fires
  only for its Assault side, the attackers by default (AS-MotherShip's 'Corereached' used to fire for the defenders
  spawning in the core at round start and moved the attackers' spawns inside the ship).

**Nav graph build time and cache**: the grid expansion runs in parallel waves (`ParallelFor`, the same graph as the
serial build; `-NavSerial` forces one thread). The log line `NavGraph: build phases:` times each phase. The built graph
is saved per map and mode in the content cache (`<ContentCache>/navcache/<map>_<mode>_<key>.bin`); the key covers the
map file's size and time, the mode, the bounds, gravity, door movers and the mode's extra points, so a changed map
builds again. A cached graph loads in a few milliseconds instead of 2-4 s (AS-Glacier, AS-MotherShip, the largest).
Bump `ArenaNavBuild::NavCacheVersion` when the build changes what it produces. `-NoNavCache` builds without the
cache; `-ArenaDumpNav` and `-NavCompare` always build.

**Nav test hooks** (non-shipping builds):

- `-NavCompare=<file>`: the file lists UE2 ReachSpecs (`sx sy sz ex ey ez flags start end` per line, our
  coordinates). Each one is planned on our graph, and the log shows the ones without a path or with a long detour
  (`NavCompare: N specs, X without a path, Y long detours`). The ReachSpecs come from the map package, read
  with Tools/ut2k4.
- `-NavProbe=x|y|z|height`: logs the free distance to world geometry in 16 directions every 50 cm up from a point,
  for measuring shafts and corridors.
- `-PadShaftTest=<UE2 pad name>[,<trials>]` (with `?Bots=1`): the bot runs at the pad from evenly spread directions,
  steering for the pad's target in the air like a player. Each flight is logged (touch point, apex, landing), then
  `PadShaftTest: N of M flights reached the target`.

## Game modes

`AArenaGameMode` runs the match. The rules of the selected mode live in a `UArenaGameRules` subclass:
`UArenaRules_DM`, `UArenaRules_TDM`, `UArenaRules_CTF` and any other `ArenaRules_*` class. Each class registers
itself with `ARENA_REGISTER_RULES`. The main menu lists every registered mode and only offers layouts the mode
supports. `Docs/GameModes.md` covers the hooks, map anchors, bot objectives and HUD drawing.

## Command-line options

Pass these as URL options on the map:

```
OpenUT2k4 Arena?Mode=CTF?Layout=Imported:CTF-Face3?Bots=7?Skill=4?Goal=3?Time=20
```

| Option | Values |
|--------|--------|
| `Mode` | a registered game mode id: `DM`, `TDM`, `CTF`, plus any mode added under `Source/OpenUT2k4/ArenaRules_*` (see `Docs/GameModes.md`) |
| `Layout` | `Imported:<Map>` or just `<Map>` (a converted UT2004 map); missing / unsupported = the mode's default map |
| `Bots` | number of bots |
| `Skill` | bot skill, `0` to `7` |
| `Goal` | frag, team score or capture limit; Onslaught: points to win (a core kill scores 2, 1 in overtime; default 3 like UT2004's GoalScore); Assault: attack / defend pairs to win (default 1: each team attacks once) |
| `Time` | time limit in minutes; Assault: per round (default 10, UT2004's RoundTimeLimit) |
| `Instagib` | `1` enables instagib |
| `Translocator` | `0` / `1`: players start with the translocator (UT2004's option; default on in CTF and Bombing Run) |
| `Team` | `0` or `1`, the preferred team in team modes |
| `RedTeamSymbol`, `BlueTeamSymbol` | team symbol of a team (`TeamSymbols_UT2003.sym05`, `Phoenix`, ...); random otherwise |
| `Mutator` | UnrealScript mutators, UT2004 syntax: `XGame.MutInstaGib,UnrealGame.MutLowGrav` (run by the script VM, see below) |

### UnrealScript runtime

`Source/OpenUT2k4/UScript/` is a UT2004 (UE2.5, package version 128) bytecode interpreter: it loads compiled script
classes from the player's `.u` packages and from `.ut2` maps at runtime (nothing from UT2004 is shipped) and runs
them against the game: mutators and game rules at the UT2004 call points (`CheckRelevance` / `CheckReplacement`,
`ModifyPlayer`, default inventory, `NetDamage`, `ScoreKill`, physics volume gravity, game speed) and script actors
placed in maps (Trigger / Touch / TakeDamage / TriggerEvent through the native UE2 event system, timers, state code,
latent `Sleep`, custom pickups). Design, verified package format, status and roadmap: `Docs/UnrealScript.md`.
Overhead with six mutators: about 0.01-0.02 ms of script per frame, no measurable FPS change.

```sh
# VM self tests (stock Core/Engine/XGame/UnrealGame functions with known results, bulk load of 2061 stock classes)
UnrealEditor OpenUT2k4.uproject -game -nullrhi -RuntimeContent -ContentCache=<dir> -UScriptTest -UScriptTestQuit
# which mutators of the cache's script packages can run (ArenaUScript::CanRun; only natives reachable from the code the
# game calls count, not FillPlayInfo / GetDescriptionText menu code): logs "UScriptCanRun: Pkg.Class: ok | reason"
UnrealEditor OpenUT2k4.uproject -game -nullrhi -RuntimeContent -ContentCache=<dir> -UScriptCanRunReport -UScriptTestQuit -UScriptTest
# a match with stock mutators (the log shows the mutator / game rules chain and what script changed)
UnrealEditor OpenUT2k4.uproject "/Game/Maps/Arena?Mode=DM?Layout=DM-Rankin?Bots=3?Mutator=XGame.MutInstaGib,UnrealGame.MutLowGrav" -game ...
# map script actors: -UScriptShootTest=<seconds> shoots every script actor with the shock beam and makes the player
# touch every script pickup (e.g. DM-CBP2-Reconstruct's ShockHitTriggers, DM-UCMP4-Mania's custom pickups)
```

## Project layout

```
OpenUT2k4.uproject          project file (module OpenUT2k4, plugins)
Config/                engine, game, input and editor settings
Source/OpenUT2k4/           all gameplay C++ (game mode + pluggable game rules, character, weapons, bots, level builder, HUD/menu)
Docs/GameModes.md      how to write a game mode (rules hooks, registration, anchors, bots, HUD)
Docs/Upscaling.md      FSR / FSR 4 / frame generation / anti-lag research and implementation notes
Docs/UE2Net.md         UT2004 (UE2) networking; "Test server": the local stock UT2004 server for tests
Plugins/ArenaUpscaling/  FSR 3.1 upscaler (FidelityFX SDK shaders, MIT) and low latency (VK_AMD_anti_lag)
Tools/GenerateContent.py   editor Python script that generates materials, sounds and maps
Tools/ut2k4_export.py, Tools/ut2k4/   UT2004 package reader / exporter (local placeholders only)
Tools/ImportUT2004.py  editor Python script that imports the exported UT2004 maps
Scripts/               build-linux.sh, build-windows.bat, build-mac.sh, generate-content.sh, import-ut2004.sh,
                       ut2004-test-server.sh (local LAN-only UT2004 dedicated server for network tests)
Content/               generated assets (Materials, Audio, Maps)
Dist/                  packaged builds (not committed)
```
