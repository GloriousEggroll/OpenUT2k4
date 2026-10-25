# UE2 networking

OpenUT2k4 joins stock and modded UT2004 servers (engine build 3369, also OldUnreal 3374 servers) with its own client
for the UE2 network protocol. It does not use UE5's net driver: `Source/OpenUT2k4/Net/UE2Net*` opens a plain UDP
socket and speaks the UE2 packet format itself. This page describes the protocol as implemented, how each part was
worked out, and what the later steps build on.

Status (step 4 of 4):

| Step | What | State |
|---|---|---|
| 1 | Server browser (`Net/ArenaServerBrowser*`) | separate work |
| 2 | Protocol, handshake, package map, actor channel decoding, joining | **done**: joins, decodes every replicated actor of a DM-Rankin match with 0 errors, follows map changes, disconnects cleanly |
| 3 | Replicated actors drive our native actors (xPawn, projectiles, pickups, GRI/PRI, flags...) | **done** for spectators: `Net/UE2NetWorld*`, see "Step 3: the server's game state in our world" |
| 4 | Playing: our own pawn, `ServerMove` / `ClientAdjustPosition` with saved-move replay, firing, weapons, respawn, HUD, the spectator's view | **done**: `Net/UE2NetPlay*`, see "Step 4: playing on the server" |
| Host | Serving the protocol: Host Game hosts matches that real UT2004 clients (3369, OldUnreal 3374) and OpenUT2k4 clients join | **done** for DM / TDM / CTF (also DOM points, the bombing run ball): `Net/UE2Server*`, see "Hosting: the UE2 server" |

Package downloads (custom maps and mod packages: HTTP redirect, file channel, net cache, conversion) work: see
"Downloads".

## Files

| File | Contents |
|---|---|
| `Net/UE2NetJoin.h/.cpp` | Public API `ArenaNet::JoinServer / LeaveServer / IsConnectedToServer / GetConnectionStatus / GetSpectatorBarText`; join flow, map loading, map changes, test hooks |
| `Net/UE2NetMapVote.h/.cpp` | Map voting (xVoting's `VotingReplicationInfo`): map list, vote counts, count down, `SendMapVote`; drawn by the mid-game menu's Map Voting tab |
| `Net/UE2NetPlayerAgreement.h/.cpp` | A server's player agreement (DruidsPlayerAgreement's `PlayerAgreementManager`): its text, `showPage`, `yes` / `no`; drawn by the mid-game menu as the mod's page |
| `Net/UE2NetConnection.h/.cpp` | `FUE2NetConnection`: socket, packets, acks, bunches, channels, control handshake, actor channel decoding, client to server function calls |
| `Net/UE2NetDownload.h/.cpp` | `FUE2NetDownloader`: package downloads between WELCOME and JOIN (HTTP redirect, file channel), `.uz2` unpacking, the per-user net cache, conversion of downloaded maps |
| `Net/UE2NetPackageMap.h/.cpp` | `FUE2PackageMap`: USES list, local package matching (GUID, generations), object / name numbering, per-class replicated field tables (`FUE2ClassNetCache`) |
| `Net/UE2NetWorld.h/.cpp` | Step 3, `FUE2NetWorld`: the client world layer. Turns replicated actors and our PlayerController's calls into native actors, game state, HUD messages and sounds; the spectator controls (`ArenaNet::HandleSpectatorFire`) |
| `Net/UE2NetActors.h/.cpp` | Step 3 stand-ins: `AUE2NetPawn` (xPawn: our character body, interpolation, animation, weapon, ragdoll), `AUE2NetProjectile`, `AUE2NetPlaceholder` (vehicles, bombing run ball) |
| `Net/UE2NetNewNet.h/.cpp` | NewNet (UTComp's lag compensated weapons, all forks): the server's time stamp (`NewNet_TimeStamp` / `TimeStamp_Pawn`), `NewNet_ServerStartFire` / `NewNet_OldServerStartFire` with the parameters each weapon class declares, `NewNet_PRI` ping |
| `Net/UE2NetModClient.h/.cpp` | Client-side parts of common mods: UTPlus and UTComp hit sounds, UTComp ready-up and warmup |
| `Net/UE2NetModContent.h/.cpp` | On-demand conversion of single resources (sounds, textures, materials, fonts) from mod packages into the content cache |
| `Net/UE2NetScriptHUD.h/.cpp` | A modded server's own HUD and scoreboard classes run by the script VM: the client-side script mirror of the replicated actors, `ClientSetHUD`, the canvas backend (see "Mod HUDs and scoreboards") |
| `Net/UE2NetPlay.h/.cpp` | Step 4, `FUE2NetPlay`: playing. Our pawn (`AUE2NetLocalPawn`, an `AArenaCharacter` with the UE2 movement), saved moves, `ServerMove` / `DualServerMove` / `ShortServerMove`, corrections and replay, firing, weapons, respawn, owner-only HUD values, chat / team / ready / combo commands, the spectator's `ServerMove`; the `-UE2NetPlayScript` test hook |
| `Net/UE2NetBits.h/.cpp` | Bit reader / writer in the UE2 layout |
| `Net/UE2NetCDKey.h/.cpp` | login identity: AUTH hash, GM, stats password, CD key challenge response |
| `Net/UE2NetValidator.h/.cpp` | honest identification (`?OpenUT2k4=<version>` in every login URL), the player key, answering the OpenUT2k4 Validator's challenge (see "OpenUT2k4 identification and the validator") |
| `Net/UE2NetValidatorCrypto.h/.cpp` | SHA-256, SHA-512, RSA-2048 (e = 3) sign / verify / key generation, Ed25519 verify: own code, shared by client and server |
| `Net/UE2NetLog.h` | `LogUE2Net` |
| `Net/UE2Server.h/.cpp` | Hosting, `FUE2Server`: the UDP game port, the connections, the package list (USES) from the player's install, replication of the server actors to each connection |
| `Net/UE2ServerConnection.h/.cpp` | `FUE2ServerConnection`: one client of our server (packets, acks, resends, channels, the server side of the handshake, actor channels with per-property shadows, function calls both ways) |
| `Net/UE2ServerWorld.h/.cpp` | `FUE2ServerWorld`: our native game as stock UT2004 actors, the remote players (`AUE2RemoteController`), their moves, fire and commands; `UE2ServerHooks` (kills, CTF events, chat from the game) |
| `Net/UE2ServerPlayer.h/.cpp` | `AUE2RemoteController`: a remote player's controller in our game |
| `Net/UE2ServerCodec.h/.cpp` | Value encoding shared by server and client: properties, structs (`CompressedPosition`, `Quat`, generic), function parameters (`UE2Codec`) |
| `Net/UE2ServerUplink.h/.cpp` | "Show in Server Browsers": the master server uplink (off by default) |
| `Net/UE2ServerValidator.h/.cpp` | the OpenUT2k4 Validator built into our server (off by default; `Server/Docs/Validator.md`) |

Small hooks outside `Net/`: `ArenaHUDUT.cpp` shows `ArenaNet::GetSpectatorBarText()` in the spectator bar, and
`AArenaGameMode::RestartPlayer` never gives a spectator-only player (`?SpectatorOnly=1`) a pawn. For downloads,
`Convert/ArenaConvUserContent` has the options `StateDirName`, `Files`, `ReferencePackages` and `bMaintenance` (see
"Downloads"). Step 3 hooks (see its section): `AArenaGameMode` (spectator controller class, no local match logic in
`?UE2Net=1` worlds), `AArenaPlayerController` (fire / alt fire, `AddConsoleLine`, `SetScoreboardHeld`),
`ArenaHUDMessages.cpp` (console line colours), `AArenaPickup::SetNetAvailable`, `AArenaFlag::SetNetState`,
`AArenaMover::SetNetMove / SetNetRest`. Step 4 hooks:
- `AArenaPlayerController`: fire release, next / previous weapon, slots, use and suicide go to
  `ArenaNet::HandleNetPlayInput`.
- `AArenaCharacter`: `FUE2StepInput` / `LastStepInput` / `StepInputOverride`, `bNetProxy`, `DoJumpUE2(bUpdating)`,
  a virtual `ActivateCombo`.
- `UArenaCharacterMovement`: `SimulateNetStep`, `bUE2ServerBraking`.
- `AArenaWeapon::PlayNetFireEffects`.
- `ArenaServerBrowser::DecodeBytes` (shared string decoding).
- `FUE2NetConnection`'s `-UE2NetLag` queues.

## Using it

`ArenaNet::JoinServer(Address)` accepts `host`, `host:port` (default port 7777) and the `ut2004://` / `unreal://`
link form, with an optional map part and URL options, e.g. `ut2004://185.107.97.16:7777?spectatorOnly=1`:

- `?Name=` sets the player name (default: the settings' player name, `UArenaUserSettings::PlayerName`).
- Without options the client joins **as a player**. `?SpectatorOnly=1` (or `?SpectatorOnly`) joins as a spectator.
- The settings' player options go into the login URL the way UT2004's `PlayerController.UpdateURL` puts them:
  `?Character=` (`PlayerCharacter`), `?Sex=`, `?Team=` (0, 1 or 255) and `?Voice=` when set. An option given in the
  address wins.
- Every other option (`?Password=`, `?Team=`, `?Character=`...) is passed to the server in the login URL unchanged.
  Passwords are masked in our logs.

Command line (development and test hooks):

| Option | Effect |
|---|---|
| `-UE2Connect=<address>` | joins once the game is up (same forms as `JoinServer`) |
| `-UE2NetTrace` | logs every control message, every opened / closed actor, every replicated property value and every function call (`LogUE2Net: UE2NetTrace: ...`) |
| `-UE2NetTraceBits` | also logs the raw bits of each actor bunch and every class's replicated field table (`UE2NetFields:`), for protocol work |
| `-UE2NetCache=<dir>` | net cache folder for downloaded packages (default `<user data>/NetCache`) |
| `-UE2NetDownloadTimeLimit=<s>` | time limit of a join's whole download pass (default 1800 s) |
| `-UE2NetNoRedirect` | ignores the server's HTTP redirect, so every download uses the file channel (test hook) |
| `-UE2NetDownloadSelfTest` | at startup, checks the file name rules and `.uz2` unpacking and logs `UE2NetDownloadSelfTest: n passed, m failed` (non-shipping) |
| `-UE2NetTestMapVote=<map>` | once the server counts a map vote down (and the map list is complete), votes for that map in the current game type (test hook for map voting, non-shipping) |
| `-UE2NetViewPlayer` | once joined, asks the server every 20 s to let our spectator follow the next player (`ServerViewNextPlayer`); keeps a pawn relevant and tests client to server calls. Local test servers only |
| `-UE2NetPlayScript="<s>:<cmd>;..."` | step 4 test hook (see "Step 4"): scripted movement, jumps, dodges, fire, weapon switching, respawn, use, suicide, team, ready, chat (only to servers on this machine), combos, routes over the nav graph (`route=pickups`, `route=flag`), `goto=water|lift|vehicle:<class>|x,y,z` (walk there on the nav graph; `vehicle:` also gets in), `face` (turn toward the last goto target), `aim` (look straight at it; `aim=vehicle` / `aim=player`: at the nearest vehicle / player shown), `yaw=` / `pitch=`, `say` / `teamsay` (local servers only) and `hunt=1` (aim at the nearest visible player and fire; `hunt=tap`: one press per shot, released once the start fire went out) |
| `-UE2NetPlayProbe` | step 4 test hook, local servers only: `ClientLoc` is sent 500 UU off, so the server answers every move with its own pawn state. Logged (`UE2NetPlayProbe: ...`), never applied. This measured the 3369 server's physics |
| `-UE2NetLag=<ms>` | adds that much round trip time (half in each direction) to every packet (test hook, non-shipping) |
| `-UE2NetNoNewNet` | NewNet weapons get the stock `ServerStartFire` (no lag compensation; A/B test hook, non-shipping) |
| `-UE2NetNoSpectatorMoves` | spectators send no `ServerMove` (the step 3 behaviour, for comparison) |
| `-UE2NetNoSlopeOffsets` | no learned height offsets on floors (per surface and steepness) (A/B test hook, see "Step 4") |
| `-UE2NetCapsuleCollision` | our pawn collides as the engine's capsule instead of UE2's box (A/B test hook) |
| `-UE2NetNoBackoff` | no UE2 `MoveActor` 2 UU back-off from what our pawn hits (A/B test hook) |
| `-UE2NetSpectateScript="<s>:<cmd>;..."` | step 3 test hook: at <s> seconds after the world layer started, `next` (fire: ServerViewNextPlayer), `alt` / `self` (alt fire: free camera and back), `scores` / `noscores` (shows / hides the scoreboard and logs every PRI's score, deaths and team as `UE2NetWorld: score ...`) |

Example (local test server from `Scripts/ut2004-test-server.sh -p 7787 -b 3 <dir>`):

```
UnrealEditor OpenUT2k4.uproject "/Game/Maps/MainMenu?game=Menu" -game -RuntimeContent -ContentCache=<cache> \
    -UE2Connect=127.0.0.1:7787 -UE2NetTrace -AutoScreenshot=20,60 -AutoQuit=78
```

## Bit streams

Everything below the packet is a bit stream (`FUE2BitReader` / `FUE2BitWriter`):

- Bits are numbered from the least significant bit of byte 0 upwards.
- A byte is 8 bits in that order; an `INT` / `FLOAT` is its 4 little-endian bytes. Nothing is ever aligned.
- **Ranged integers** (`ReadInt(Max)`) carry a value in `[0, Max)` in as few bits as the range allows, low bit first:
  bit k is present only while `value_so_far + 2^k < Max`. With `Max = 1023` a channel index takes 10 bits, but
  with a range that is not a power of two the top bit is skipped for larger low parts. Example: with `Max = 91`,
  the value 57 takes 6 bits and 32 takes 7.
- **Compact indices** (strings' lengths) are UE2 `FCompactIndex`: the first byte has the sign (0x80), a "more"
  flag (0x40) and 6 value bits, and each further byte has a "more" flag (0x80) and 7 value bits.
- **Strings**: a compact length that counts the terminating zero, then the characters. A positive length means
  8-bit characters, a negative one means UTF-16 characters.

## Packets

A UDP datagram is one packet, of at most 512 bytes:

```
packet  = PacketId:ReadInt(16384)  { item }  1  (zero padding to the byte)
item    = 1 AckId:ReadInt(16384)                    an acknowledgement
        | 0 bunch
```

- The sender appends a single 1 bit after the last data bit. The receiver finds the end by searching the last byte
  for its highest set bit. A last byte of 0 is invalid.
- Packet ids count up from 0 on each side and wrap at 16384. The receiver rebuilds the full number as the value
  nearest to the last one seen (`MakeRelative`).
- **Every received packet is acknowledged** with an ack item in the next outgoing packet. Acks arrive in packet order,
  so an ack for id N means that ids between the previous ack and N were lost (a "nak"). The reliable bunches of a
  lost packet are sent again, unchanged except for the packet id. We also resend any reliable bunch not acked within
  1 s (the server's `AckTimeout`).
- The client sends a packet at least every 0.2 s (acks or an empty keep-alive packet) and gives up after 20 s
  without any packet from the server (the server's `ConnectionTimeout` is also 20 s).
- If the queued bunches do not fit in 512 bytes they go out in several packets. A single control bunch with many
  `HAVE` lines would otherwise be too big; the server drops oversized packets without a word.

## Bunches and channels

```
bunch   = bControl [bOpen bClose]  bReliable  ChIndex:ReadInt(1023)
          [ChSequence:ReadInt(1024)]          if bReliable
          [ChType:ReadInt(8)]                 if bReliable or bOpen
          DataBits:ReadInt(4096)  data
```

- Channel types: 1 control, 2 actor, 3 file. The control channel is index 0. The client opens it with its first
  bunch (HELLO, `bOpen`, reliable); the server opens actor channels, one per replicated actor. The client opens file
  channels for downloads (see "Downloads").
- **Reliable bunches** carry a per-channel-index sequence number (wrapping at 1024, rebuilt with `MakeRelative`). The
  receiver processes them strictly in order, keeps early ones in a queue and drops duplicates. Sequence counters
  belong to the channel index and keep counting when the index is reused for a new actor.
- **Unreliable bunches** (most property updates) have no sequence number. They are processed as they arrive, and
  ignored if their channel is not open.
- `bClose` ends a channel: the server closes actor channels when an actor stops being relevant or is destroyed, and
  closes the control channel to kick the client. Our clean disconnect is a reliable close bunch on the control
  channel; the server then logs `Close TcpipConnection`.
- **No partial bunches.** Unlike UE3 and later, UE2 has no bit for bunches split across packets. Every bunch fits
  in one packet, which also caps an actor's initial bunch. The header above was checked against real traffic: a
  wrong field width would break the parsing of the next bunch in the packet, and no such error occurred over
  thousands of packets.

## Control channel: the handshake

Control bunches carry strings, one or more per bunch. The formats below are those of the 3369 server binary
(its format strings), confirmed against the local server:

```
client: HELLO REVISION=0 MINVER=3180 VER=3369
server: CHALLENGE VER=3369 CHALLENGE=<int> STATS=<0|1> SEC=<int> GZ=<int>
        (or UPGRADE MINVER=<n> VER=<n> when our version is not accepted)
client: AUTH HASH=<identity hash> GM=<gm> USERNAME=[<stats user>] PASSWORD=[<encrypted stats password>]   (see "CD key and AUTH")
client: NETSPEED 20000
client: LOGIN RESPONSE=<int> URL=Entry?Name=<name>?SpectatorOnly=1[?other options]
server: USES GUID=<32 hex> PKG=<name> FLAGS=<int> SIZE=<bytes> GEN=<generations> FNAME=<file name>   (one per package)
server: DLMGR CLASS=Engine.ChannelDownload PARAMS=Enabled COMPRESSION=0                               (download methods)
server: WELCOME LEVEL=<map> GAME=<game class> LONE=0
        (or FAILURE <text> / FAILCODE <code> <error>, e.g. a wrong password, a full server, a ban; see "Login rejection")
        (file channels: package downloads, see "Downloads")
client: [SKIP GUID=<guid>] [HAVE GUID=<guid> GEN=<n>]   (see "Package map")
client: JOIN          (after our map is loaded)
```

- `RESPONSE` is the engine's fixed answer to the challenge number, a scramble of the number:
  `(C * 237) ^ 0x93FE92CE ^ (C >> 16) ^ (C << 16)` in 32-bit signed arithmetic (arithmetic right shift). The server
  checks it (`Client %s failed CHALLENGE`). Our login passes.
- The map part of the login URL is ignored by the server; the options are the player's (`Name`, `Class`,
  `Character`, `Team`, `SpectatorOnly`, `Password`...), the same ones the server log shows on `Login request`.
- `NETSPEED` sets the bytes per second the server may send us (the server caps it with `MaxClientRate`). The server
  also replicates `ClientCapBandwidth`.
- `SEC=` / `GZ=` in CHALLENGE are UT2004 additions for its security and compression features. The client does not
  need to answer them.

**Compared with the OldUnreal 3374 client** (its Linux client, no CD key, run on its own Xvfb display and captured
through a UDP logging proxy against the same local 3369 servers as ours: stock DM, a TAM server with
WS3SPN / WSUTComp, the Vehicle CTF profile with NewNet weapons; see "Real client reference"):

| Step | 3374 client | Ours |
|---|---|---|
| Hello | `HELLO REVISION=0 MINVER=3180 VER=3374` | `MINVER=3180 VER=3369` (the server answers `VER=3369` either way; a 3339 server answers `VER=3339`, and a MINVER above its version gets `UPGRADE`) |
| Login | `AUTH`, `NETSPEED 20000`, `LOGIN RESPONSE=.. URL=:<port>/Index.ut2?Name=..?Class=Engine.Pawn?Character=..?team=255` in one packet | the same three; the URL carries our options without `Class=` |
| After `WELCOME`, map loaded | every `HAVE` in USES order (one per package whose generation count differs from the server's, older or newer: 145 of 204 on DM-Rankin with the 3374 packages), then `PETE PKT=.. PKG=..`, two `REPEAT <scrambled text>`, `JOIN`, `OPENVOICE 1` | `SKIP` / `HAVE` in USES order, then `JOIN` |

So the order of HAVE / SKIP relative to JOIN is the same: all of them before `JOIN`, in USES order. `PETE` and
`REPEAT` are the stock engine's package check report (the server binary reads them, `REPEAT` carries scrambled
package data); we do not send them, and the 3369 servers do not need them. `OPENVOICE` opens voice chat, which we do
not have.

## CD key and AUTH

### What the OldUnreal 3374 client sends

Read from the Linux client of OldUnreal's latest UT2004 patch (3374 preview 23: `Engine.so`
`UNetPendingLevel::NotifyReceivedText`, `GetCDKeyHash`, `EncryptWithCDKeyHash`, `GetCDKeyResponse`, `GetRealCDKey`)
and confirmed by running that client, with no CD key installed, against the patch's own dedicated server (both
headless and local, through a logging proxy). On `CHALLENGE` the client sends, in this order:

```
AUTH HASH=<32 hex> GM=<132 hex> USERNAME= PASSWORD=
NETSPEED 20000
LOGIN RESPONSE=<int> URL=<options>
```

- **No CD key**: OldUnreal installs have none. On the first run the 3374 client makes a unique key of its own and keeps
  it in `System/cdkey` (the release notes' "replace [the shared GoG key] with a unique key" does the same). From then
  on it logs in exactly like a keyed client. Every server sees a unique, stable id per install.
- **HASH**: the plain MD5 of the key text, 32 lower-case hex digits (checked: the HASH equals the MD5 of the generated
  `System/cdkey`). The server salts it itself (`UNetConnection::SecureCDKeyHash`, salt `1230asadkgjk...4958`) to make
  the player id that `AccessControl.bBanByID` bans by. The client never sends the salted value.
- **GM**: 66 bytes as 132 hex digits. Bytes 0-63 are random. The client's 16-byte `GMD5` is hidden in them: each byte
  is XORed with `X + i`, and the bytes are interleaved at a random offset `O` (0-31): byte `O+1+2i` =
  `(previous & 0x0F) | (this & 0xF0)`, byte `O+2+2i` = this. Byte 64 is `X` and byte 65 is `O`. In 3374 nothing ever
  sets `GMD5`, so the hidden MD5 is all zeros (decoded from every captured GM). The server decodes it.
- **USERNAME / PASSWORD**: filled only when all of these are true: the server's `CHALLENGE` has `STATS=1`, Track Stats
  is on (`bEnableStatsTracking`), and the stats username and password are both set. Otherwise both are blank, even
  with Track Stats on. The password is not sent in clear text: `EncryptWithCDKeyHash(Password, "STATS")` takes
  `H` = the MD5 hex of key + `"STATS"` and writes each character XORed with `H[i & 15]` as two hex digits.
- **Track Stats off on a `STATS=1` server**: the 3374 client gets `FAILCODE NEEDSTATS`, the same as ours. On a
  `STATS=1` server with an account set, it is welcomed. The local server does not check the account; a live server
  can.
- **ClientValidate**: `GetCDKeyResponse(C)` is the MD5 of the key followed by the challenge.
- `NETSPEED` is the 3374 default `ConfiguredInternetSpeed=20000`. Its `LOGIN` URL is
  `:<port>/Index.ut2?Name=..?Class=Engine.Pawn?Character=Jakob?team=255?SpectatorOnly=1`; ours sends the same options
  without the defaults it does not use (the server reads only the options).

### Our client

`FUE2NetConnection::SendAuth` and `Net/UE2NetCDKey` send the same thing in the same order:

- **With a CD key** (`System/cdkey`, a `Registry_Fix.reg` next to the install or one folder up, or the Windows
  registry): HASH = MD5 of the key (dashed, upper case). The key is read only when joining, hashed and wiped. Before
  this change our client sent the server-side salted hash as HASH and the key's MD5 as GM.
- **Without a CD key**: OpenUT2k4 does not make CD keys. Instead `UArenaUserSettings::NetIdentity` is 32 random hex
  digits, made on the first join and saved with the settings. It plays the part of the 3374 client's generated key:
  HASH = its MD5, so a server sees a unique, stable 32-hex id exactly like the one from a no-key OldUnreal install.
  Before this change no AUTH was sent without a key, and `bBanByID` servers answer an empty id with `LOCALBAN`.
- GM: built as above with an all-zero hidden MD5 (`UE2NetCDKey::MakeGM`).
- Stats: `bTrackStats`, `StatsUsername`, `StatsPassword` on `UArenaUserSettings` (off and blank by default, as in
  UT2004). Sent only to `STATS=1` servers, with the password encrypted as above, using the key or NetIdentity, and
  only when the account follows the UT2004 rules (username at least 4 characters, password at least 6). They are set
  in Settings > Game > Stats (UT2K4Tab_GameSettings: Track Stats, UserName, masked Password). On `NEEDSTATS` the
  server browser shows UT2K4StatsPrompt; YES opens that tab, and closing it joins again when the account is valid.
- ClientValidate is answered from the key, or from NetIdentity when there is none.

The key, NetIdentity, the hashes and the stats password are never logged: the trace masks the whole AUTH line.
Test switches (not in Shipping): `-UE2NoCDKey` ignores an installed key to test the no-key login, and `-UE2TestStats`
makes up a throw-away stats account for one run (never saved, password never logged).

### Tests

Local server: the OldUnreal 3374 Linux dedicated server, `bBanByID=True`, LAN only, with `STATS=0` and with `STATS=1`
(`[Engine.GameInfo] bEnableStatLogging=True`). The OldUnreal client and ours were run against it through a UDP
logging proxy that decoded the AUTH line in memory (hash values masked in all saved logs):

| Client | Server | Sent | Result |
|---|---|---|---|
| OldUnreal 3374, no key | STATS=0 | AUTH (HASH = MD5 of its generated key), blank account | welcomed |
| OldUnreal 3374, no key, Track Stats off | STATS=1 | blank account | `NEEDSTATS` |
| OldUnreal 3374, no key, Track Stats on | STATS=1 | account, encrypted password | welcomed |
| OldUnreal 3374, no key, Track Stats on | STATS=0 | blank account | welcomed |
| ours, key | STATS=0 | HASH = MD5 of the key | welcomed |
| ours, key, Track Stats off | STATS=1 | blank account | `NEEDSTATS` |
| ours, key, Track Stats on | STATS=1 | account, password encrypted with the key | welcomed |
| ours, `-UE2NoCDKey`, Track Stats on | STATS=1 | HASH = MD5 of NetIdentity, password encrypted with it | welcomed |
| ours, `-UE2NoCDKey`, Track Stats on | STATS=0 | blank account | welcomed |
| ours, `-UE2NoCDKey` | STATS=0 | HASH = MD5 of NetIdentity | welcomed |

In every case our AUTH had the same layout and order as the 3374 client's, and GM decoded to the same all-zero MD5.

Live, one spectator join each (`-UE2NoCDKey`):

- **ProTheory TAM** (`185.107.96.9:7777`, `STATS=1`), with `-UE2TestStats`: **accepted**. Before this change it answered
  `NEEDSTATS`. The server sent 210 `USES` (map `DM-Rankin-NE2`, game `WS3SPN_V6.TeamArenaMaster`), the redirect
  `DLMGR CLASS=IpDrv.HTTPDownload PARAMS=http://protheory1.site.nfoservers.com/redirect/ COMPRESSION=1`, and
  `WELCOME`. 11 packages were missing (61.8 MB). The redirect copy of the map failed our size check ("unpacked file
  is larger than 24.7 MB"), the file channel ran at about 129 KB/s, and the join was cancelled by the 10-minute
  download estimate.
- **TUS TAM** (`185.107.97.16:7777`, `STATS=1`), Track Stats off: still `FAILCODE LOCALBAN`, with a new, never-banned id
  in the 3374 format. So the ban does not come from the AUTH format. It is the server's own access control (an IP or
  name rule, or a list of known ids); an OldUnreal client from the same address may get the same answer.

## Login rejection

A server that refuses the login sends `FAILCODE <code> [error text]`, usually with a `FAILURE <text>`. The client
maps the code to a plain-language message, records the code (`GetCloseFailCode()` / `ArenaNet::GetLastFailCode()`) and
closes:

| FAILCODE | Meaning | Client action |
|---|---|---|
| `SERVERFULL` | the server is full | message |
| `NEEDPW` | a password is required | message (the browser prompts and retries with `?Password=`) |
| `WRONGPW` | wrong password | message |
| `NEEDSTATS` | stats account required (`CHALLENGE STATS=1`) | the UT2004 stats prompt (`ArenaNet::GetStatsPrompt`, texts from `GUI2K4.int UT2K4StatsPrompt`); YES opens the stats settings and retries |
| `LOCALBAN` | banned by the server's AccessControl | message |
| `SESSIONBAN` | banned for this session | message |
| `CHALLENGE` | the CD key challenge failed | message |
| `UPGRADE` | our engine version is too old | message |

Two things made a rejected join safe:

- **Stop sending after a close.** Before, a rejected connection kept flushing keep-alive packets (~70/s) at the
  server until the 20 s receive timeout. `Flush` now drops everything once the state is `Closed`, so a rejection stops
  the traffic at once (the live ProTheory and TUS joins each sent three packets total).
- **A login timeout.** If `WELCOME` or a rejection does not arrive within 12 s of `LOGIN`, the client gives up
  instead of waiting out the full receive timeout.

The browser shows the message in a box, and the test hooks (`-AutoScreenshot` / `-AutoQuit`) fire even when no map
ever loads, so a rejected join still screenshots and quits (`AutomationFallback` in `UE2NetJoin.cpp`).

## OpenUT2k4 identification and the validator

Our client never pretends to be the stock client. Every `LOGIN` URL ends with `?OpenUT2k4=<version>` (the project
version, `0.1.0`); stock servers ignore options they do not know, and the server log shows it in `Login request`. The
client does not run, answer or imitate any third-party anti-cheat; servers that require one will not admit it.

**AntiTCC** (`MutAntiTCCFinal`) lets the client log in, then holds the new player ("Please wait, Anti TCC is
initializing...") until its client-side check answers, so `ServerReStartPlayer` never spawns us (reproduced on a local
3369 server with `AntiTCC_2026r6`; without the mutator the same server spawns us at once). The client says so instead
of "Press Fire to join the match": the spectator bar reads "The AntiTCC anti-cheat on this server does not let
OpenUT2k4 play: you can only spectate" while the package map holds an `AntiTCC*` package and we have not spawned
(`ArenaNet::FindSpawnBlockingAntiCheat`). The server browser asks before joining a server whose details name it
(rules `Mutator MutAntiTCCFinal`, `Anti TCC version`): SPECTATE joins with `?SpectatorOnly=1` (spectating works),
CANCEL does nothing. Test hook: `-AutoMenuScript` `BrowserJoinSelected` (the JOIN button's path).

Servers that run the **OpenUT2k4 Validator** (`Server/Docs/Validator.md`: a mutator for stock servers, built into our
own server) challenge clients that sent the option; the answer proves the player's identity (a key the client made on
first start, `<Saved>/Identity/PlayerKey.txt`), not anything about the build.

- **Stock server with the mutator**: the package `OpenUT2k4Validator.u` is in USES (it calls `AddToPackageMap`) and is
  downloaded like any mod package. After JOIN the server spawns `OpenUT2k4Validator.OUTValidatorLink`, owned by our
  PlayerController and relevant to us only (`bOnlyRelevantToOwner`), with the replicated string `ChallengeText` =
  `OUTV1;<32 hex nonce>;<server ip:port>;<map>;<player id>`. `FUE2NetConnection` passes every opened / updated actor to
  `UE2NetValidator::ActorChanged`, which signs `OUT2K4RESP2;<ChallengeText>;<dialed>` with the player key (`dialed`
  = the server IP and port we really send to, `FUE2NetConnection::GetRemoteIP`, plus `/<host>` when joined by name)
  and calls the reliable server function `ServerResponse(int Part, int Total, string Data)` on that actor's channel,
  240 characters per part (5 calls): the parts joined are `OUTR2;<dialed>;<public key hex>;<signature hex>`
  (RSA-2048, e = 3, PKCS#1 v1.5 SHA-256), or `OUTR2;NOKEY;<reason>`. A challenge whose port is not the port we
  dialed is refused that way (a relayed challenge); the server checks that the signed dialed address is its own. The world layer ignores the actor (an unknown class). `OUTValidationInfo`
  actors (one per player, replicated only when the admin marks results) are ignored too.
- **Our own server**: no package; after JOIN the control channel carries `OUTVALIDATE CHALLENGE=<same form, the port
  as *:port>` and the client answers with `OUTVALIDATE PART=<i> TOTAL=<n> DATA=<part>` lines (`SendControlText`).
  Stock servers never send `OUTVALIDATE`, and our client sends it only in answer.
- The client signs only challenges that start with `OUTV1;`, are printable and at most 240 characters, once each per
  connection. A challenge that arrives before the key is ready (first start) is answered from `FUE2NetConnection::Tick`.
- Test switches (non-shipping): `-UE2PlayerKey=<file>`, `-UE2ValidatorSelfTest`, `-UE2ValidatorTest=badsig | wrongkey
  | nokey | silent | record=<file> | replay=<file> | dialport=<n> | relaydialed=<ip:port>`. Results:
  `Server/Docs/Validator.md` "Tests".

## Package map

`USES` lists the server's packages in order: the map first, then everything its actors can reference (script
packages, textures, sounds, meshes...). `FUE2PackageMap` matches each one with a local file and numbers every
object.

- **USES line forms**: the 3369 server sends `FNAME=<name>` unquoted (`FNAME=Engine.u`). OldUnreal 3374 quotes it
  (`FNAME="ONS-Torlan.ut2"`) and sends an empty `FNAME=""` for code packages that are not downloadable (those also
  have `FLAGS=32`, i.e. without the AllowDownload bit). `AddUses` strips the quotes and keeps an explicitly empty
  file name empty (an absent `FNAME` falls back to the package name).
- **Matching**: the local file is found by package name in the player's install (`UT2004.ini` paths), then the
  content cache's `scripts/` copies, then custom content. It must have the same **GUID** (the GUID is in the package
  header; it changes whenever a package is rebuilt). Without a match, the net cache is searched by GUID
  (packages downloaded from servers, `UE2NetDownload::FindCachedPackage`).
- **Generations**: each package header has a generation table, one `(ExportCount, NameCount)` per save. Both sides
  use the newer generation both have: `min(server GEN, our generation count)`. The client tells the server its count
  with `HAVE GUID=... GEN=...` before `JOIN`, so the server numbers that package the same way. Without `HAVE` the name
  numbering differs, because later generations of texture packages add names.
- **Missing or different packages** get `SKIP GUID=...`: the server removes them from this connection's package map,
  so the numbering of everything after them still matches. Objects of a skipped package then arrive as references to
  an actor channel that is not open (seen as channel 511). Actors whose class is in a skipped package are not
  replicated at all. A map actor of a skipped map package arrives with such a reference instead of its class and is
  ignored.
- **Object numbering**: the packages' export tables are concatenated in USES order, each cut to its export count at
  the chosen generation (`ObjectBase` / `ObjectCount`). Name numbering works the same way with the name tables
  (`NameBase` / `NameCount`).
- **Object references** on the wire: one bit. A 1 is followed by `ReadInt(1023)`: an actor channel, where 0 means
  None. A 0 is followed by `ReadInt(MaxObjectIndex)`: a static object, such as a class, a map actor, a texture or a
  sound.
- **Names** on the wire: `ReadInt(MaxNameIndex + 1)`. The value `MaxNameIndex` and anything that does not resolve
  mean None.

### Version compatibility (3369.3 server vs the player's install)

Against the local 3369.3 dedicated server (with the Bonus Pack), all 204 packages of DM-Rankin, and all packages of
DM-Corrugation and DM-DE-Grendelkeep, match the GUIDs of the user's install, which is build 3369 (2005-11-23). The
generation counts differ for 123 packages: the server's texture packages have one more generation than the
retail ones, and `XPickups_rc`, `XGame_rc` and `XWeapons_rc` have 42 server generations against 40 locally. `HAVE`
takes care of that.

Real UT2004 clients handle a GUID mismatch as a missing package: they try to download it, and a server never
offers its stock code packages for download, so the client fails with a version mismatch. Net play therefore needs
the same build of every stock package as the server. OldUnreal's 3374 is meant to keep the stock packages
compatible with 3369 servers. That is not verified here: no 3374 server was available, and the user's install is
3369. **Recommendation for the converter**: keep, for every package of the install (not only `.u`), its GUID,
generation table, name table and export paths/classes in the content cache. The client would then not need the
install to number objects. Today it opens the install's files for that, and the cache's `scripts/` for classes.

## Actor channels

### Initial bunch

The first bunch of an actor channel (`bOpen`) starts with an object reference (static):

- **a class**: a spawned actor. It is followed by the spawn location as a compressed vector (below), then, only if the
  class's default `bNetInitialRotation` is true, the rotation as a compressed rotator;
- **a map actor**: an actor placed in the map (`LevelInfo0`, movers, ...). Its class is the class of that export of
  the map package.

The replicated properties follow in the same bunch.

### Replicated fields and their numbering

Each class has a table of replicated fields: properties with `CPF_Net` (they appear in the class's `replication`
block) and functions with `FUNC_Net`. Overrides of a replicated function in a subclass are left out; only the first
declaration counts. A class's index range starts where its super class's ends (`FieldsBase`), and Actor's starts
at 0.

**The order inside a class is the order of the fields' objects in the package's export table.** It is not the
order of the class's field list (the order of declaration), and not alphabetical. This was the hard part. Both
other orders gave garbage, and the export order was found from captured GameReplicationInfo bunches. In those, the
strings `GameClass`, `ServerName` and `ShortName` arrive with indices 57, 61 and 74. Their positions in Engine.u's
export table among the class's replicated fields are 0, 4 and 17, and Actor has 57 replicated fields. With this
order, every class seen decodes without a single error (thousands of properties and function calls per minute).
`-UE2NetTraceBits` prints the tables (`UE2NetFields: <Class> <index> <name> ...`).

`FUE2PackageMap::GetClassNetCache` builds the tables from the classes our UnrealScript loader reads
(`UScript/UScriptLoader`: field flags, types, enums, structs, function parameters). The class package must be the
same build as the server's: the GUID is checked again for the script runtime's copy.

### Bunch contents

```
field   = RepIndex:ReadInt(class MaxIndex)
          property: [Element:8 bits if ArrayDim != 1] value
          function: for each parameter in declaration order (until the first non-parameter or the return value):
                    bool -> 1 bit;  other -> 1 "present" bit, then the value if present
```

### Property values (`ReadProperty`)

| Type | Wire format |
|---|---|
| byte | enum: `ceil(log2(number of enum values))` raw bits; plain byte: 8 bits |
| int / float | 32 bits |
| bool | 1 bit |
| object / class | object reference (above) |
| name | `ReadInt(MaxNameIndex + 1)` |
| string | string (above) |
| `Vector` | compressed vector: `Bits:ReadInt(20)`, then X, Y, Z each `ReadInt(2^(Bits+2)) - 2^(Bits+1)`: whole units only |
| `Rotator` | compressed rotator: for pitch, yaw, roll: a bit, then (if 1) one byte = the high byte of the 16-bit angle |
| `Plane` | four 16-bit integers |
| `Quat` (the ONS vehicles' `HoverCraftState`, `SCarState`, `CopterState`, `TreadCraftState`: `ChassisQuaternion`) | X, Y, Z as three 32-bit floats; **W is not sent** (implied: a unit quaternion with W >= 0). Worked out from 200 captured `ONSHoverBike.HoverState` bunches (local ONS-Torlan, `-UE2NetTraceBits`): with W left out, 198 of 200 end exactly at the bunch end (the other 2 carry further properties); the generic rule overran every one |
| `CompressedPosition` (`Pawn.PawnPosition`) | location as a compressed vector, then pitch and yaw as one byte each (no roll, no "present" bits), then velocity as a compressed vector. Worked out from captured xPawn bunches: the generic struct rule misreads it |
| other structs | members in field order: the struct's own fields, then its super struct's; static arrays element by element |
| dynamic arrays, delegates | never replicated in UE2 |

Notes for step 3 (implemented in `Net/UE2NetWorld*`):

- `Role` / `RemoteRole` arrive as the **server's** values. The server's `Role` (Authority) is our remote role, and
  its `RemoteRole` is our local role (SimulatedProxy, or AutonomousProxy for our own PlayerController and pawn).
- Only properties that changed are sent; the first bunch has everything that differs from the class defaults. Step 3
  starts each actor from its class's defaults (which our loader already reads) and applies the updates.
- Values are kept per actor in `FUE2NetActor::Properties` (`"Name"` or `"Name[i]"`), and reported through
  `OnActorOpened`, `OnActorUpdated` (with the changed keys), `OnActorClosed` and `OnFunctionCall`.

Observed in a DM-Rankin match with 3 bots (spectating, 60+ s): the PlayerController (xPlayer) with `ClientSetHUD`,
`ClientSetViewTarget`, `ReceiveLocalizedMessage`, `ClientHearSound` (sound, location, parameters); the
GameReplicationInfo (`GameName "DeathMatch"`, `GameClass "XGame.xDeathMatch"`, `ServerName`, `GoalScore 25`,
`TimeLimit 20`, `RemainingTime`, `TeamSymbols[0..1]`); xPlayerReplicationInfo for every bot and us (`PlayerName`,
`CharacterName`, `Score`, `Deaths`, `PlayerID`, `bBot`, `PlayerZone`, `PlayerVolume`); the xPawns of the bots
(`PawnPosition` moving at the run speed, `Health`, `AnimAction`, `Physics`); weapon attachments; projectiles
(rockets, flak, shock); every pickup; the map's movers (`RealPosition`, `StaticMesh`) and `LevelInfo0`.

### Client to server function calls

`FUE2NetConnection::SendFunction(Channel, Name, Params)` sends a replicated function on our own PlayerController's
channel (`FindPlayerControllerChannel`). The bunch is a field index followed by the parameters, in the same format
as received calls. It is sent reliably when the function has `FUNC_NetReliable` (0x80). Tested with
`ServerViewNextPlayer`: the server answers with `ClientSetViewTarget(<the bot's xPawn>)`. Step 4 sends its moves,
fire and commands this way (see "Step 4"). Parameters equal to zero may be sent as "present" with a zero value: the
server reads them either way.

## Joining, map changes and leaving

1. `JoinServer` starts the handshake.
2. On `WELCOME`, missing packages are downloaded and downloaded maps converted first (see "Downloads"). Then our
   converted copy of the map is loaded through the normal travel path:
   `/Game/Maps/Arena?Mode=<mode of the game class>?Layout=<map>?Bots=0?Time=0?Goal=0?SpectatorOnly=1?UE2Net=1`.
   The local login spectates (UE5's `SpectatorOnly` option). As a player, the local player possesses our pawn when the
   server gives us one (see "Step 4"). If the map cannot be shown (not in the content cache: it could not be
   downloaded, or the converter could not make a playable copy), the join fails with the reason (the converter's
   problems, e.g. "Missing package: ...", or the packages that could not be downloaded) instead of a connection that
   never shows anything. During a server map change the loading screen shows that reason instead. `-UE2NetDecodeOnly`
   (not in Shipping) joins anyway without a map (decode only), for protocol tests.
   Packages a map only takes sounds, textures or meshes from may be missing (servers that do not allow downloading
   them, or a UT99 sound package a map imports): the map is converted without those parts and listed as playing with
   limits. Only a missing package the map takes classes from makes it unplayable.
3. When the map has loaded (`PostLoadMapWithWorld`), the client sends the SKIP and HAVE lines, then `JOIN`. The server
   logs `New Player <name>`, and the actor channels open.
4. **Map changes**: the server calls `ClientTravel(<map>)` on every client and then closes the connections. The client
   reconnects to the same address (with the same options) after 2 s; the new `WELCOME` loads the next map. Tested
   through two changes (DM-Rankin, DM-Corrugation, DM-DE-Grendelkeep). From `ClientTravel` until the next map's world
   is up, the HUD covers the old map like UT2004's loading screen (`ArenaNet::GetMapChangeStatus`): "Loading <map>"
   with what happens meanwhile (the server changing maps, reconnecting, downloading and converting a custom map with a
   progress bar), or why the next map cannot be shown (the rejoin failed, the map is not in the content cache) with
   how to leave. Before, the old map stayed on screen with its scoreboard emptied (its actors close with the
   connection) and nothing said what was happening, which looked like a hang while a custom map downloaded.
5. **Map voting** (xVoting, `Net/UE2NetMapVote`): every player has a `VotingReplicationInfo`, and all of them reach
   every client (`ReplicationInfo.bAlwaysRelevant`). Ours is the one whose `PlayerOwner` is our controller (other
   players' controllers are not replicated to us, so theirs arrive as None), or any one the server calls (it calls
   only the owner's). The server sends each player's `VotingReplicationInfo` the game
   types (`ReceiveGameConfig`), the map list (`ReceiveMapInfo`) and the vote counts (`ReceiveMapVoteCount`), one call
   per tick, each waiting for the client's `ReplicationReply`. The client answers them (decoded from the class
   metadata; none of xVoting's code runs), and the mid-game menu gets a Map Voting tab (and the Map Voting button in
   place of Restart Match): the maps of the chosen game type by name, with their votes and play counts. `OpenWindow`
   at the end of the match opens it, `PlayCountDown` shows the seconds left, a double click or Vote sends
   `SendMapVote(MapIndex, GameConfigIndex)`, `CloseWindow` closes it. Without our replies the list stopped after the
   first item and the end of a match was a silent `VoteTimeLimit` wait (70 s by default). Tested on a local 3369
   server with `[xVoting.xVotingHandler] bMapVote=True`: 46 maps received, a vote made the server switch to that map.
   Restart Match is not offered on a server (it restarted our local copy of the match).
6. **Player agreement** (DruidsPlayerAgreement, `Net/UE2NetPlayerAgreement`): the server actor
   `PlayerAgreementServerActor` gives every new player a `PlayerAgreementManager` (owned by the player's controller)
   unless the player accepted this agreement's `Timestamp` before. `setup` makes the player a spectator
   (`bOnlySpectator`, state `Spectating`), so `ServerReStartPlayer` does nothing (on such a server "Fire to
   join" did nothing) until an answer. The manager calls the client's `PlayerAgreement` (the stock client opens the
   mod's `PlayerAgreementPage`, ours the same page in the mid-game menu: "Player Agreement" text, "I agree!" usable
   after the `doneAdding` count down, "I do not agree."; it cannot be closed without an answer) and the client calls
   `showPage` back; `addToAgreement` sends each line. "I agree!" sends `yes()`: the server lets the player in
   (`PlayerWaiting`, Fire joins); `no()` gets the player kicked with `ClientNetworkMessage("AC_Kicked", "You must
   accept the agreement to play on this server")`, which the leave screen shows; no answer within the manager's
   `Timeout` (90 s by default) kicks too. Until then the spectator bar says the agreement holds the player.
   Script steps `agree` / `disagree` answer it. Tested on a local 3369 server running `Classic_FreezeTag_i4i` with
   `DruidsPlayerAgreement110.PlayerAgreementServerActor`: the page with the server's text, `yes` and Fire spawned us,
   `no` closed the connection with the kick message. FreezeTag itself spawns players who join mid-match at once.
7. **Leaving**: opening any other map (the menu), `LeaveServer()` or quitting the game sends the control channel close.

## Mod servers

Online servers are mostly modded (3SPN / TAM, UTComp, NewNet weapons...). Their USES lists contain mod packages
the player does not have.

- Missing mod packages and custom maps are **downloaded** before `JOIN` (below). Packages that cannot be downloaded
  are `SKIP`ped, so the connection still works and everything from stock packages decodes. But the server does not
  replicate actors whose class is in a skipped package, and mods often replace central classes (PRI, pawn, weapons).
  Classes that cannot be loaded are marked broken: their bunches are skipped without breaking the connection.
- **Decode-only mod classes**: decoding needs only the class metadata (fields, flags, types), which our loader reads
  from any `.u`. A downloaded mod package therefore decodes without running any of its code. What the mods' client
  code does for a player is done by `Net/UE2NetNewNet` (the NewNet fire protocol) and `Net/UE2NetModClient` (hit
  sounds, ready-up, warmup), from the mods' own class metadata and defaults (below). Their HUDs, scoreboards and HUD
  interactions run in the script VM on a client-side copy of the replicated actors (`Net/UE2NetScriptHUD`, "Mod HUDs
  and scoreboards" below); nothing else of a mod's code runs.
- **Mod classes of stock kinds** are shown as their stock ancestor: a weapon, its attachment and its projectiles by
  the first stock class up the mod class's super chain (`NewNet_ShockRifle` -> `ShockRifle`, view model, effects and
  HUD icon of our pulse rifle; `UE2NetActors::WeaponClassFor`). Before, every mod weapon was "ours none": no view model
  and no local fire effects. A mod weapon that extends `Weapon` itself takes the stock weapon its `PickupClass`,
  `AmmoClass` or `AttachmentClass` comes from (`ProSniperRifle_20_BETA3.ProSniperRifle`: `ProSniperPickup` extends
  `ClassicSniperRiflePickup`, so our marksman rifle with its zoom), and a mod attachment that extends
  `xWeaponAttachment` itself the weapon of its package that hands it out. Mods' copies of a stock hit effect under
  their own name (`ProSniperWallHitEffect`, IK_Stealth's `Stealth_WallHitEffect`, both extending `Effects`) show as the
  stock one (class name ending in `WallHitEffect`).
- **Mod weapons' own assets** (`Net/UE2NetModWeapons`): the stand-in shows what the mod's class defaults name where
  they differ from the stock relative's (the class the stand-in reproduces: `UTClassic.ClassicSniperRifle` for
  ProSniperRifle): the first-person `Mesh` (its animation set from the converted mesh, `SelectAnim` / `PutDownAnim` /
  `IdleAnim` and the fire modes' `FireAnim`), `Skins`, the view placement (`PlayerViewOffset`, `SmallViewOffset`,
  `PlayerViewPivot`, `DrawScale` x `DrawScale3D`, `DisplayFOV`, `Centered*`), each fire mode's `FireSound` and
  `FireRate`, the weapon bar's `IconCoords` (on the stock HUD atlas, or on the mod's own `IconMaterial` texture), the
  third-person attachment's `Mesh`, `Skins`, `RelativeLocation` / `RelativeRotation` / scale (other players' weapons),
  and a mod pickup's `StaticMesh`. Every item falls back to the stand-in's own when it is missing, did not convert or
  does not load; the log line `ModWeapons:` lists what is the mod's and what fell back. The assets come from the
  downloaded package: the net cache conversion exports a script package's own textures, materials, meshes,
  animations and sounds, and those it imports from other custom packages (a mod vehicle's skins in its .utx, its mesh
  in a .ukx), once per package version (`ArenaConvUserContent::ScriptAssetsVersion`, state.json packages' `assets`; a
  server whose script packages were not exported yet runs the converter on join), into the content cache like a map's
  content. A map that defines classes of its own (vehicles embedded in the map) exports its classes' assets the same
  way with the map (its signature carries the version). What is still missing (a mod installed in the UT2004 folder
  instead of downloaded) is converted on demand (`UE2NetModContent::EnsureConverted`). Our weapon carries the server class it stands in for (`FArenaNetStandIn`), one weapon per
  server class, so a mod weapon and its stock relative are two. Not taken from the mod: timing its code sets at run
  time (ProSniperFire.ModeDoFire's 0.14 s / 1.0 s: the default `FireRate`, 0.5 s, is used) and projectile classes (the
  stock relative's; a mod's replicated projectiles run their own client code: "Mods' client code"). Taken: the fire
  modes' `FlashEmitterClass` (the mod's muzzle flash on our barrel) and `SelectSound`. Reflections
  (`TexEnvMap`, a `Cubemap`), which our masters cannot compute, are drawn as a smooth shiny surface of the reflected
  image's average colour, and a `ConstantColor` / `FadeColor` as its colour (`ArenaRuntimeMaterials.cpp`
  `StandInColour`), not as a cube face stretched over the mesh. Tested on a local 3369 server: ProSniperRifle_20_BETA3
  (its fire sound and view placement; mesh and skins are the stock ones it names) and AK47Bin2L (an AK47 that extends
  `ShockRifle`: its own first- and third-person meshes, skins, view placement, fire sounds and rates).

- **Mod vehicles** (`UE2NetModWeapons::FindLook`, `AUE2NetVehicle`): a vehicle, stationary turret or vehicle weapon is
  drawn with its class's `Skins` and `RedSkin` / `BlueSkin` (the team's on the first slot, as ONSVehicle / ONSWeapon
  do) and `DrawScale` x `DrawScale3D`; a weapon on a chassis bone keeps its own scale (UE2 attachments do not inherit
  the chassis's). Our art rig stands in for a stock mesh only when the class looks like its nearest stock ancestor; a
  mod's Manta or Raptor with its own skins or scale is drawn with the UE2 mesh. `bHidden` hides the body (a stationary
  turret's base is hidden behind its gun until it is destroyed, then its dead base shows) and a weapon. The wreck has
  the chassis's scale. `PrePivot` offsets the body, a gun and a decoration along the world axes (UE2 draws an actor at
  `Location + PrePivot`). What a weapon's client-side code spawns and attaches to its bones is found without running
  the code (`UE2NetModWeapons::FindClientAttachments`): an instance variable whose type is a client-only (`RemoteRole`
  `ROLE_None`) actor class with a mesh, passed to `AttachToBone` with a bone name (a `name` variable or constant) in the
  class's script; it is drawn on that bone of the gun with its class's scale, placement and skins (`RedSkins` /
  `BlueSkins` per team), so it turns with the gun (OdinV2Omni's ion cannon `IonTurretAttachment` on the invisible
  `IonTurretDummy` gun's `PitchBone`). This is the fallback for when the mod's client code does not run ("Mods' client
  code": there the code attaches the cannon itself, plays its animations and spawns its charge beams).
- **Decorations and loose pickups**: an actor of a class we do not know that draws a static mesh (`DT_StaticMesh`: a
  mutator's mask on a player's head, AngelMapperHalloweenMasks) and a pickup that is not one of the map's (a dropped
  weapon, a mutator's or a map script's pickup) are drawn as their `StaticMesh` with their `Skins` and scale, on their
  `Base` (a player's bone or attach alias: `AttachmentBone`, `RelativeLocation` / `RelativeRotation`) or where they
  are. Actors placed in the map are left to the converted map.
- **Overlays and Freeze Tag**: a pawn's replicated `OverlayMaterial` / `OverlayTimer` (the server's
  `SetOverlayMaterial`: spawn protection's `PlayerShieldSh`, a mod's glow) is drawn over the body for that time (0:
  until replaced), over our own team rim. A pawn class with a replicated `bFrozen` (WS3SPN `Freon_Pawn`, L7's freeze
  tag and its relatives) does what its client Tick does once it is set: the animation stops in the pose it froze in
  and the class's `FrostMaterial` is added to the body (UE2 `Combiner` `CO_Add` of the skins and the frost texture;
  here an additive overlay tinted towards ice blue). The frozen body is not a death (no ragdoll, no gibs); thawing,
  the server replaces it with a new pawn. With a mod HUD, pawns with `bScriptPostRender` get `PostRender2D` each frame
  before `HUD.PostRender`, as UE2's renderer does (Pawn's calls the HUD's `DrawCustomBeacon`: Freon's snowflake
  beacon, "Name (thaw %)" and the thaw bar); a frozen pawn's `bScriptPostRender` is set from its `bFrozen` (the line of
  its client Tick the mirror does not run).
- Test hook: `-UE2NetSpectateScript="30:vehicle;34:vehicle"`, the free camera on the next vehicle class not viewed yet
  (parked ones first); `vehicle=<Class>` that class; `frozen` the free camera beside the next frozen pawn.

### NewNet (lag compensated weapons)

UTComp's "NewNet" weapons (also in its forks, such as `WSUTComp_V6`) compensate the shooter's ping on the
server: the client tells the server when it saw the world, and the server traces against the players' positions of
that time (`NewNet_PawnCollisionCopy`, a history of every pawn). A client that sends the stock `ServerStartFire`
still fires, but without compensation: the server traces against where the targets are now. Worked out from the
packages' script source (the `.u` files carry it) and their class metadata; implemented in `Net/UE2NetNewNet.h/.cpp`.

- **The clock**, two variants:
  - `NewNet_TimeStamp` (UTComp 1.7 and forks of it): a ReplicationInfo whose `ClientTimeStamp` (float) the server sets
    every tick (`NewNetMut.Tick`). Its `Tick` is not simulated, so the client's value is simply the last one received.
  - `TimeStamp_Pawn` (UTComp 1.8+, WSUTComp): a pawn whose replicated `Rotation` is a tick counter (yaw =
    (counter % 256) * 256, pitch = counter / 256 * 256; it has no mesh, so `Rotation` replicates as a plain rotator,
    not in `PawnPosition`). The client's `TimeStamp_Pawn.Tick` (simulated) turns it into `NewTimeStamp = (Yaw + Pitch *
    256) / 256` and counts `DT`, the time since it last changed. The server keeps the time of each counter value
    (`MutUTComp.StampArray`). The client follows the first such pawn (the script takes the first one `DynamicActors`
    finds); the world layer never shows it.
- **Fire**: a NewNet weapon's `ClientStartFire` sends `NewNet_ServerStartFire` on the weapon's channel instead of
  `ServerStartFire`. Its parameters differ per weapon and per fork; ours are filled by name from the weapon class's
  own declaration (`FUE2NetNewNet::SendStartFire`):

  | Parameter | Value |
  |---|---|
  | `Mode` | fire mode |
  | `ClientTimeStamp` | float: the last `NewNet_TimeStamp.ClientTimeStamp`; byte: the `TimeStamp_Pawn` counter |
  | `DT` | time since the counter last changed (game seconds) |
  | `avgPing` | `NewNet_PRI.PredictedPing` (below) |
  | `R` (`ReplicatedRotator {Yaw, Pitch}`) | the view (controller rotation) |
  | `V` (`ReplicatedVector {X, Y, Z}`) | `Pawn.Location + EyePosition()`: our camera in the frame of `ClientLoc` (with the learned floor offsets); the server uses R / V when they are within 27 UU of its own eye (`IsReasonable`) |
  | `bBelievesHit`, `A` | what the client's own trace along the view hits first: a pawn (its UE2 cylinder where we show it) or vehicle (its bounds), before the world. UTComp's server then searches nearby times for that actor when its own trace misses, and for a miss when the client saw one |

  The server turns the stamp into `PingDT` (float stamps: `ClientTimeStamp(now) - stamp + 1.75 * AverDT`, or with
  `bNewNetExp` `+ AverDT * NewNetExp_HSMult` clamped to `NewNetExp_ThresholdHS`; UTComp: `ClientTimeStamp(now) -
  StampArray[stamp] - DT + 0.5 * AverDT`) and rewinds the pawns by it. Projectile modes use it to move the projectile
  ahead (capped; the float-stamp fork tested here has `NewNetExp_ThresholdProj = -1.5` by default, which turns that off).
- **When**: UE2 sends a start fire only once the weapon is ready (`Weapon.ReadyToFire`; the engine retries
  `ClientStartFire` every frame while the button is held). For NewNet weapons we do the same: a press while the fire
  mode is not ready waits (`FUE2NetPlay::TickNewNetFire`), so R, V and the stamp are those of the moment the shot
  leaves. Started while the other, exclusive mode is still busy: `NewNet_OldServerStartFire(Mode, stamp[, DT])`
  where the weapon has it. `ServerStopFire` stays stock. Stock weapons are unchanged (`ServerStartFire` on press).
- **Ping**: `NewNet_PRI` (linked from our PRI's `CustomReplicationInfo` chain) gets `Ping()` and answers `Pong()`
  every `PingTweenTime` (UTComp: the class default or `UTComp_ServerReplicationInfo.PingTweenTime`; float-stamp forks: 0.5 s). The
  round trip feeds `PredictedPing` with the script's own average (float-stamp forks: 10 samples, UTComp: `(2P + rtt) /
  PingTweenTime`).
- **Compared with the 3374 client** (it runs the mods' own client script; its calls decoded from captures against the
  same local servers): WSUTComp's rocket launcher sends `NewNet_ServerStartFire(Mode, Stamp, DT, R, V)` with
  `R` = the controller's yaw / pitch and `V` = `Pawn.Location + EyePosition()`, the shock rifle and lightning gun add
  `bBelievesHit, A`; the float-stamp fork's assault rifle sends `(Mode, ClientTimeStamp)` with the last received
  `NewNet_TimeStamp` value (it advances 1.1 per real second with the game speed). Ours sends the same parameters with
  the same meaning. Differences that do not matter to the server: the script passes `A` = whatever its trace hit
  (also walls), ours only a pawn or vehicle (the server reads `A` only when `bBelievesHit`); zero parameters were sent
  in full by ours (now a 0 bit, see "Moves"). The 3374 client also sends `NewNet_PRI.Ping` on each `NewNet_PRI`
  channel once after joining and then on its own every `PingTweenTime` (ours: its own only), and mod UI traffic that
  we do not run: UTComp's vote map list replies (`UTComp_PRI.ReplyToMapSend`, 20 per second while the list loads)
  and a scoreboard's periodic requests.
- **Measuring it**: a test-only mutator in the scratch folder (never in the repository) logged every shot and every
  damage event of the human player on the server, with the fire mode's `PingDT`. The test client hunted bots with
  `-UE2NetPlayScript="3:route=pickups;4:hunt=tap"` (one press per shot, aimed at the target as shown). Shock rifle
  primary, DM-Rankin, 3 bots moving and dodging, 240 s runs:

  | Server | Client | Lag (RTT) | Shots | Hits | Server `PingDT` |
  |---|---|---|---|---|---|
  | NewNet fork with float stamps, DM | NewNet fire | 0 | 34 | 32 (94 %) | 0.021 |
  | same | NewNet fire | 200 ms (`-UE2NetLag=200`) | 21 | 18 (86 %) | 0.255 |
  | same | stock `ServerStartFire` (`-UE2NetNoNewNet`) | 200 ms | 28 | 10 (36 %) | (none) |
  | WS3SPN TAM + WSUTComp_V6 (counter stamp) | NewNet fire | 0 | 46 | 42 (91 %) | 0.020 |
  | WS3SPN TAM + WSUTComp_V6 | NewNet fire | 200 ms | 58 | 51 (88 %) | 0.252 |

  With the NewNet protocol a 200 ms round trip costs a few percent of hits instead of more than half. `PingDT` follows
  the lag (round trip plus the age of the last stamp). Before the `bBelievesHit` trace (always "no hit") the UTComp
  server turned some of our hits into misses ("reverse fix"): 77 % at lag 0.

### Mod client-side code

`Net/UE2NetModClient.h/.cpp` does what common mods' client code does for the local player, reading the mods'
settings from their own classes (class defaults and the player's config, through the script runtime) and converting
the resources it needs on demand (`Net/UE2NetModContent`, below):

| Mod | What the client does | Ours |
|---|---|---|
| UTPlus hit sounds (`UTPlusHitSoundTag`, an inventory of our PRI) | `PostNetReceive`: a changed `HitSoundRepInfo {HitType, HitData}` plays `UTPlusSettings`' sound for the hit type (enemy, team, vehicle, empty vehicle, objective), volume `HitSoundVolume`, pitch `(HitData & 31) * 0.0613 + 0.1` (damage) | the same; unqualified sound names are in the tag package's `HitSounds` group |
| UTComp hit sounds (`BS_xPlayer.ReceiveHitSound(Damage, iTeam)` / `ReceiveHit(DamageType, Damage, Injured, instigatedBy)`) | `PlayEnemyHitSound` / `PlayTeammateHitSound`: `UTComp_Settings.EnemySound` / `FriendlySound` at `HitSoundVolume`, CPMA style pitch `CPMAPitchModifier * 30 / Damage`, at most every 0.05 s | the same; a sound path without its package ("Sounds.HitSound", the WSUTComp default) is in the settings class's package |
| UTComp warmup (`UTComp_Warmup.bInWarmup`, `BS_xPlayer.NotifyEndWarmup`) | the warmup overlay, `Ready` / `NotReady` console commands: `UTComp_PRI.Ready()` / `NotReady()` and `BS_xPlayer.BroadcastReady` | `Ready` / `NotReady` send those; the HUD shows "Warmup: n of m ready (type Ready / NotReady)" from every `UTComp_PRI.bIsReady` |

Sounds the server plays from a mod package (`ClientPlaySound` / `ClientHearSound` with a mod `Sound`) are converted
the same way and play.

**On-demand conversion** (`UE2NetModContent::EnsureConverted / LoadSound`): the content converter converts maps and
what they use, so resources only mod classes use (hit sounds in a `.u`, HUD textures) are missing from the content
cache. The first time one is needed, its package file (the script runtime's: a downloaded or installed `.u`; a name
the install has always keeps the install's file) is opened by the converter's package manager and only that object is
exported into the content cache (`sounds/<Package>/<Group.Name>.wav + .json`), then the runtime content index of that
kind folder is refreshed (`ArenaRuntimeContent::OnCacheFilesAdded`). A hit sound took 2 - 5 ms. Failures are
remembered and not retried.

Results (local servers, 240 s): UTComp (WSUTComp_V6 on WS3SPN TAM) 62 of 62 and 75 of 75 hit sound events played
(`WSUTComp_V6.Sounds.HitSound` converted on the first hit); warmup seen, `Ready` sent, the warmup ended by the server
("UTComp warmup ended"). A replica of a popular Vehicle CTF server's mod set (stock 3369 server, NewNet with float stamps, UTPlus hit sounds,
RezQ scoreboard, bright skins, 15 bots, `-UE2NetLag=100`, 190 s): 802 NewNet start fires (793 with a believed hit),
262 of 262 UTPlus hit sounds played (`HitSounds.HitSound`, converted on the first hit, 17 ms), 0 bunch errors,
185 corrections in 25219 moves (0.7 %), the NewNet weapons with our view models.

### Mod HUDs and scoreboards

A modded server's HUD and scoreboard are the mod's own UnrealScript (`PlayerController.ClientSetHUD(HUD class,
scoreboard class)`, e.g. `WS3SPN_V6.TAM_HUD` / `TAM_Scoreboard`, `WSUTComp_V6.UTComp_HudCTeamDeathmatch` /
`UTComp_ScoreBoard`, the RezQ scoreboard on stock VCTF). `Net/UE2NetScriptHUD` runs them in the script VM and draws
them on our canvas, like the real client draws them:

- **Client mirror**: every replicated actor whose class the script runtime has (stock or downloaded) gets an object of
  that class in a client-side script world (`FUE2NetMirrorWorld`): class defaults plus the received properties, Role
  and RemoteRole swapped, references resolved to the mirrored actors, to classes, to placeholders of textures / fonts /
  sounds, or, for map actors without a channel (`PRI.PlayerZone`, `PlayerLocation`), to a data copy with the map
  package's saved properties (location names). What UE2's client sets up itself is set up the same way: the Level
  (`NM_Client`, `TimeSeconds`, the clock, `Title` from the converted map, `GRI`, `GetLocalPlayerController`),
  `GRI.PRIArray`, the controller's `GameReplicationInfo`, `Player` (viewport with an `InteractionMaster`) and
  `ViewTarget` (the pawn we watch), `Pawn.Weapon` (our current weapon), the weapons' fire mode objects and ammo classes
  (`Weapon.InitWeaponFires`), and the GameReplicationInfo's client `Timer` (elapsed / remaining time). Mirrored actors
  are data, except the local PlayerController's `PostBeginPlay` (mods create their client settings objects there,
  e.g. UTComp's `HUDSettings`), the GRI's `Timer` and the mods' client code (below, "Mods' client code").
- **The HUD**: `ClientSetHUD` from the server runs on the mirrored controller, which spawns the HUD and scoreboard in
  the script world (their spawn events, timers, `Tick` and state code run there). Each frame `HUD.PostRender` draws on
  a script `Canvas` (the scoreboard when the scoreboard key is held: `bShowScoreBoard`), then the viewport's
  interactions (`InteractionMaster.Process_PostRender`). Interactions a mod's client code adds for the local player and
  keeps in a controller property (UTComp's team overlay, `BS_xPlayer.Overlay`) are added through the stock
  `InteractionMaster.AddInteraction`.
- **Canvas bridge** (`UScript/UScriptCanvas.h`, `UScriptNativesCanvas.cpp`): UE2's Canvas natives (`DrawText` with
  UE2's word wrapping and cursor, `DrawTextClipped`, `StrLen`, `TextSize`, `DrawTextJustified`, `WrapStringToArray`,
  `WrapText`, `DrawTile`, `DrawTileClipped`, `DrawTileStretched`, `DrawTileScaled`, `DrawTileJustified`,
  `DrawTilePartialStretched`, `WorldToScreen`, `GetCameraLocation`), `HudBase.DrawSpriteWidget` / `DrawNumericWidget`,
  `HUD.DrawCanvasLine` / `StaticDrawCanvasLine` / `Draw3DLine`, `Material.MaterialUSize` / `VSize`,
  `CacheManager.GetCrosshairList` (the install's `.ucl` records), `GUIFont.GetFont`, `Interaction.Initialize`; the
  script-side helpers (`SetPos`, `SetDrawColor`, `DrawIcon`, `DrawRect`, `Reset` ...) are the stock script. The
  canvas state (Font, Style, DrawColor, ColorModulate, Org, Clip, Cur, SpaceX/Y, bCenter, FontScale) lives in the
  script Canvas object. Styles: Normal / Masked / Alpha alpha blended, Translucent / Additive additive, Modulated
  multiplied; a FinalBlend's own blending wins. `DrawActor`, `DrawPortal`, `DrawScreenActor`, `Clear` and the screen
  lights do nothing (our renderer draws the world and the first person weapon).
- **Widgets**: the widget natives keep their aspect ratio like OldUnreal's 3374 HUD (`ResScaleX = ResScaleY =
  Min(SizeX / 640, SizeY / 480)`); the retail 3369 scripts in the player's install would stretch them over a wide
  screen. Sizes and positions otherwise follow UE2 (`HudCanvasScale`, `HudScale`, pivot, scale modes, `Tints[TeamIndex]`).
- **Resources**: a texture or material placeholder is resolved through the content cache's json (a material to its base
  texture with its FinalBlend blending, TexRotator turning, TexScaler scale and a Combiner's FadeColor pulse; UE2 sizes
  from the json, also as `Texture.USize` / `VSize` the first time script reads them). Fonts are the cache's UE2 bitmap
  fonts drawn 1:1 (`ArenaRuntimeContent::LoadFontByObjectPath`), line height from the font's cell height, colour
  codes (ESC R G B) as colour runs. What only a mod package has (WS3SPN's scoreboard and flag textures, UTComp's logo,
  mod fonts and materials) is converted on demand from the package (`UE2NetModContent::EnsureConverted`, 15 - 25 ms per
  texture, once per cache).
- **Messages**: the server's `ReceiveLocalizedMessage`, `ClientMessage` and `TeamMessage` to our controller go to the
  script HUD the way UE2's PlayerController passes them on (`LocalMessage.static.ClientReceive(PC, Switch, PRI1, PRI2,
  OptionalObject)`, which a mod's message class may override, and `HUD.Message(PRI, S, Type)`), so its own lists and
  layout show them (`FUE2NetScriptHUD::FeedMessage`); ours then only plays their sounds (`AArenaPlayerController::
  MessageDisplayHold`). A message whose script errs, and every message while the script HUD has fallen back, is ours.
- **What stays ours**: stock HUD classes (the game's own packages) are drawn by our native HUD (`ArenaHUDUT`) unless
  `-UE2ScriptHUD=all`. With a script HUD our HUD still draws the join / spectator notice and the mod status line; a stock HUD with a mod scoreboard (VCTF with the RezQ scoreboard) draws ours and runs only the scoreboard's
  `DrawScoreboard`. Per element (HUD, scoreboard) the script falls back to ours after 5 script errors, 120 (60) frames
  without a draw call or a runaway frame (over 5 M tokens); unsupported natives log once and return zero.
- **Never run**: anti-cheat and client-check packages (`AntiTCC*`, `itsfake`, `UTSecure`, `ACE*`, ... and
  `-UE2ScriptDeny=Pkg,Pkg`) are neither mirrored nor run, and nothing of them is emulated or answered (`ClientValidate`
  and the like). Mutators and other `Info` actors, controllers and HUD classes do not run as client code (below);
  mods' menus are not opened.

### Mods' client code

What a UE2 client runs of a mod's replicated actors runs in the client mirror (`Net/UE2NetScriptHUD.cpp`, `-UE2ScriptClient=0`
turns it off): for every mirrored actor of a mod's package (not UT2004's own, whose actors our native code shows; not
an `Info`, controller or HUD) the spawn events (`PreBeginPlay` .. `SetInitialState`, `PostNetBeginPlay`), then each
frame its timer, `Tick` and state code; `ClientTrigger` when `bClientTrigger` flips, `FlashMuzzleFlash` /
`ThirdPersonEffects` when `FlashCount` changes, `PostNetReceive` with `bNetNotify`, `Destroyed` when its channel closes.
The VM follows UE2's role rule (`USVM::IsSkippedOnProxy`): on a simulated proxy only simulated functions and the code
of simulated states run (natives and static functions always). The mirror's actors are placed where ours show them
each frame (their channel's component: a pawn, a vehicle's body or gun, a weapon attachment's third-person weapon), so
script reads the right `Location` / `Rotation` and `GetBoneCoords`.

- **What it spawns** (client-side actors, `Role` Authority on our side) is drawn by `Net/UE2NetScriptVisuals`: meshes
  (`DT_Mesh`, playing what `PlayAnim` / `LoopAnim` ask: `UScript/UScriptPresentation.h`), static meshes, `Emitter`
  classes (their own emitters: `ArenaUE2Effects`, also from a mod's package), sprite xEmitters, and mesh xEmitters (a
  burst each time script triggers them: the weapons' muzzle flashes); with `Skins`, scale, `bHidden`, on its `Base`'s
  bone (`AttachToBone`) or where it is. `LifeSpan`, `Emitter.Kill` / `AutoDestroy` and xEmitters without `mRegen` end
  them. The presentation natives (`SetDrawType`, `SetStaticMesh`, `LinkMesh`, `SetDrawScale3D`, `SetRelative*`,
  `AttachToBone`, `DetachFromBone`, `GetBoneCoords`, `GetBoneRotation`) keep the actor's own UE2 properties;
  `PlaySound`, traces and `FastTrace` (world geometry) work in the mirror (the map's world).
- **Budget**: per actor, three script errors or a call over a million tokens stop its client code (its data mirror and
  our native parts stay); the summary line counts actors started, client-side actors spawned, stopped ones and time.
- **Stand-ins retired**: what the VM shows replaces a targeted stand-in when it is at least as good: an attachment a
  vehicle weapon's client code attaches (the Odin's ion cannon) removes the class-defaults stand-in
  (`FUE2NetWorld::RetireClientAttachmentStandIn`). Kept: Freeze Tag's frozen look (the mod's frozen skins are
  `Combiner`s made at run time, which our materials cannot draw) and the beacon's `bScriptPostRender` line.
- **Mod pawns' skins**: a mod pawn's client code setting `Skins` to a mod's own materials (bright skins, a mod's team
  skins) is shown on our pawn (UT2004's own skins and run-time materials are left out).
- **Weapons**: for the local player's mod weapons (they play as our stand-in), the fire modes' `FlashEmitterClass`
  (the mod's muzzle flash on the barrel) and `SelectSound`; other players' mod weapon attachments run their own
  `ThirdPersonEffects` (muzzle flashes, tracers, shell and hit effects: Ballistic Weapons' `TraceEmitter`s).
- Results (local 3369.3 servers): ONS-Tek-)o(-Randomizer-V13 with 10 bots, 240 s: 413 replicated mod actors started,
  701 client-side actors spawned, 0 stopped, about 16 ms of script per second; the Odin's ion cannon spawned by
  `OdinIonTurret.PostNetBeginPlay` on its gun's `PlasmaGunBarrel` (the stand-in retired). DM-Rankin with AK47Bin2L and
  3 bots: the AK47's own muzzle flash (`AK47BinPMuzFlash`, mesh particles) on our first-person barrel, the bots'
  projectiles' smoke rings. Freon (WS3SPN_V30): 73 messages fed to `Freon_HUD` in 110 s, the frozen look and thaw
  unchanged. Ballistic Weapons V25 with 3 bots: the bots' tracers and impacts (`TraceEmitter_*`, `BG_ShellHit`).
- Statistics: `-UE2ScriptHUDLog` logs frames, average time and tokens, mirrored actors and resources every 5 s; the
  summary line `UE2NetScriptHUD summary` is logged when the connection ends.

Results (local 3369.3 servers, client at 1280x720):

| Server | Drawn by the script VM | Cost |
|---|---|---|
| WS3SPN_V6 TAM + WSUTComp_V6 (DM-Rankin, DM-TrainingDay, 4 bots) | `TAM_HUD` (timer, team scores with team symbols, adrenaline, health / shield, weapon bar with ammo bars, ammo with icon, the weapon's custom crosshair, spectator bar) and `TAM_Scoreboard` (header with score limit / stats / server skill / date, both teams with NOT RDY, ranks and the TAM logo, elapsed and round time) | 0.1 - 0.25 ms, 2000 - 4000 tokens per frame |
| WSUTComp_V6 on stock TDM | `UTComp_HudCTeamDeathmatch`, `UTComp_ScoreBoard` (UTComp logo, per player ping / PL / time, location names), the team overlay interaction runs | 0.2 - 0.3 ms, about 5000 tokens |
| Replica of a popular Vehicle CTF server (stock VCTF HUD, RezQ scoreboard, 16 bots) | `RZQ_ScoreBoardTeamDeathMatch` (per player stats, denied / flag kills, location names, times) | 1.3 ms per scoreboard frame |
| Stock TDM with `-UE2ScriptHUD=all` | `HudCTeamDeathMatch` / `ScoreBoardTeamDeathMatch` | 0.2 ms |

Compared with the OldUnreal 3374 client on the same TAM server (screenshots of the other test round): HUD layout,
widget sizes, scoreboard layout, fonts, date line and team scores match; texts the mods show as local messages ("You
are in warmup, press F5 ...") are ours (the warmup line).

### Downloads

`Net/UE2NetDownload.h/.cpp` (`FUE2NetDownloader`). The pass runs between `WELCOME` and `JOIN`, while the connection
keeps ticking: keep-alives and acks go out, and the server waits for `JOIN`.

1. **Stock packages are never downloaded.** `UE2NetDownload::GetStockPackageNames` builds the list of UT2004's own
   packages for each join. The log says which rule was used.
   - **The installer's manifest**: the install's `System/Manifest.ini`, plus any other `System` ini with a `[Setup]`
     section, `MasterProduct=UT2004` and `File=` lines. Its `File=Folder\Name.ext` lines are the files UT2004's
     installer put there. The user's retail install lists 1465 files, 831 of them packages, including the Bonus Pack
     and ECE (`DM-BP2-Calandras`, `ONSTorlanREDONE`). Mods the player copied into the UT2004 folder (UCMP, CBP maps,
     UTComp...) are not in it.
   - **Fallback, only without a manifest** (no install found, or none written): the install's package folders, and
     what the content cache was converted from (its asset folders, `scripts/System`, `scripts/Maps`). The custom
     content and net cache conversions are left out (their `state.json`).
   - A mod in the UT2004 folder whose GUID differs from the server's is downloaded like any other package. The
     downloaded `.u` is opened in the script runtime right away, so the runtime uses it rather than the same-named
     file in the install.
   Servers flag stock resource packages as downloadable, and a 3374 server offers them over the file channel, but
   refuses each request.
   - A stock package without a local file of the same GUID is `SKIP`ped and reported once in the log. A non-blocking
     notice, `UE2NetDownload::GetJoinNotice()`, also appears in the connecting status and the spectator bar: "N UT2004
     packages missing or another version (install not found or mismatch)". The HUD draws it above the spectator
     bar, on a dark band (`ArenaHUDUT.cpp`).
   - A stock **code** package found locally with another GUID cancels the join: "Version mismatch: the server's X is
     another version than your UT2004 install's". Its classes would not decode, and UT2004 refuses such servers too.
   - Live, before this rule: a 3374 ONS server joined without the install path got 137 stock packages (283 MB) queued.
     The join never reached `JOIN`.
2. **What is downloaded**: other USES entries without a local file of the same GUID. The package's `FLAGS` must have
   `PKG_AllowDownload` (0x1), its `FNAME` must be a safe file name (below), and `SIZE` must be within
   `UE2NetDownload::MaxPackageBytes` (512 MB). A local file of the same name but another GUID counts as missing, as in
   UT2004: the server's version goes to the net cache, and the install is never touched.
3. **Methods**, in the order of the server's `DLMGR` lines:
   - `DLMGR CLASS=IpDrv.HTTPDownload PARAMS=<RedirectToURL> COMPRESSION=<UseCompression>`. The server sends it only
     when its `[IpDrv.HTTPDownload] RedirectToURL` is set. The client GETs `<URL>/<FNAME>.uz2`, or `<URL>/<FNAME>`
     without compression, with the engine's HTTP module. Only `http://` and `https://` URLs are used. The response is
     capped only by the 1 GB sanity limit (`MaxPackageBytes`); UT2004 has no limit, and maps of 30 to 150 MB are
     common. Before unpacking, the uz2 chunk headers' declared sizes are added up and checked against the cap. A
     redirect copy is judged by its **GUID**: with the server's GUID but another size it is used with a warning
     (UT2004 compares GUIDs only); with another GUID it is a stale version on the redirect, rejected, and the file
     channel is tried. ProTheory's redirect holds such a stale `DM-Rankin-NE2` (31.7 MB, another GUID than the
     server's 25.9 MB map).
   - `DLMGR CLASS=Engine.ChannelDownload PARAMS=Enabled COMPRESSION=0`: the file channel. It is used when there is no
     redirect, or the redirect fails (HTTP error, wrong size or GUID, no answer, a stall).
   - **A dead redirect.** A redirect that sends neither a status line nor data within 10 s, that cannot be reached at
     all, or that stops sending for 20 s is given up for the rest of the pass, as UT2004 does: every following file
     goes straight to the file channel instead of waiting on the redirect again. An HTTP error (404) only moves that
     one file to the channel. The status says so ("redirect not answering: using the server (slower)").
   - **Failing fast.** A file request must get an answer (data or a refusal) within 10 s. After 3 requests in a row
     without any answer, the file channel is given up for this join. A stall during a transfer ends it after 30 s.
     The whole pass has a time limit: 30 min, or `-UE2NetDownloadTimeLimit=<s>`. Live servers cap the file channel
     at 10 to 20 KB/s, so a 25 MB map takes about 20 minutes over it, as it does in UT2004. After 15 s of file
     channel transfer, the channel's own measured rate projects the time still needed over the channel: the current
     file, plus the following files when there is no redirect (with a redirect they try it first). A pass that cannot
     make it cancels the join at once with "Join cancelled: downloading the server's packages would take too long
     (X MB left over the file channel at Y KB/s, limit Z min; ...)". The join does not go on after a cancel.
     Redirect bytes never enter the channel rate; before this fix a failed redirect download inflated it (ProTheory
     showed "129 KB/s" when the channel ran at 21 KB/s). During channel downloads the status shows the rate and the
     time left ("21 KB/s, about 17:30 left").
   - A package that cannot be downloaded either way is `SKIP`ped at `JOIN`, as before.
   - **Progress and Cancel.** The download band shows the file (N of M, MB, speed and time left), the total MB of the
     pass and any note, a progress bar and a CANCEL button that leaves the server. The menu draws it on every page,
     not only in the server browser, and the page under it does not take the band's clicks.
   - **UE1 style packages** (version < 68, UT99-era sounds and textures such as `AmbOutside.uax`) have no GUID in
     the summary: UE2 takes the last entry of the heritage table (`HeritageCount` at 36, `HeritageOffset` at 40), and
     so do the package reader (`UE2ReadPackageHeader`, also for the net cache and local matching) and the converter.
     Before, these read as GUID 0, so a correct redirect copy was rejected as "another version".
4. **The file channel**, worked out from the 3369 server (stripped binary: confirmed by traffic and results):
   - Request: the client opens a new channel of type 3 with a reliable `bOpen` bunch. Its data is the package GUID
     as four little-endian DWORDs (16 bytes), the same layout as `UChannelDownload::ReceiveFile` in UE1 / UE2.
   - Reply: the file's raw bytes, uncompressed, in reliable bunches on that channel. The last bunch has `bClose`.
   - Refusal: a `bClose` bunch without data. The server answers this way when its `[IpDrv.TcpNetDriver]
     AllowDownloads` is `False`. It still announces `ChannelDownload ... PARAMS=Enabled` then. It also refuses
     GUIDs that are not in the connection's package map.
   - **Each request uses a new channel index.** The server keeps a closed channel until our ack of its close
     arrives, and silently drops an open bunch for that index meanwhile. That was seen as a 30 s stall when a request
     reused the index right after a refusal. On the live 3374 server, a build without this fix got refusals and
     30 s timeouts in strict alternation.
   - Speed: the server sends at the connection's net speed. `NETSPEED` is clamped by the server's
     `MaxInternetClientRate` (10000 by default, even for 127.0.0.1), so a stock server sends about 9 KB/s, whatever
     the client asks for. That is why real servers use redirects. During channel downloads the client asks for
     `NETSPEED 1000000`, then goes back to its setting. With `MaxInternetClientRate=1000000` on the test server, 49.5 MB
     took 56 s. A real client sends its configured net speed (default 20000 in 3374, 10000 in 3369); asking for more is the best a client
     can do, and the server's limit is final.
5. **`.uz2`** (the output of `ucc compress`, which also runs on the native Linux server): no header, just a series of
   chunks. Each chunk is an `int32` compressed size, an `int32` uncompressed size (32768 except for the last one),
   then a zlib stream (with the zlib header) of that chunk. Unpacked with `FCompression::UncompressMemory(NAME_Zlib)`.
   Checked against `ucc compress` output: 523 chunks for a 17 MB map, byte-identical after unpacking.
6. **Checks** before a file is kept:
   - the unpacked size equals the USES `SIZE`;
   - the header is an Unreal package with the USES GUID;
   - it has at least the server's number of generations.
   The file is written to `<name>.part` first, then renamed.
7. **Net cache** (per user: `<user data>/NetCache`, or `-UE2NetCache=<dir>`), indexed by GUID:
   - `scripts/<GUID>/<Package>.u`: code packages;
   - `packages/<GUID>/<Package>.<ext>`: maps (`.ut2`) and resources (`.utx .usx .ukx .uax`);
   - `index.json`: GUID to name, file, size, generations, and the download and last-use times.
   Several versions of a package (from different servers) live side by side. There is no size limit or eviction yet.
8. **Use**:
   - After the downloads the package map matches again: entries found by GUID in the net cache count as local, so
     `HAVE` / `SKIP` and the object numbering include them.
   - Downloaded `.u` files are added to the UnrealScript runtime (`FUSRuntime::AddPackageFiles`), and so are the net
     cache's `.u` files of this server when nothing had to be downloaded: the runtime resolves imports between mods by
     package name (WS3SPN's `Misc_Player` extends WSUTComp's `BS_xPlayer`), and a class whose super class does not
     resolve gets a short replicated field table, so its function calls decode as other functions. Mod classes then
     decode through the existing loader, metadata only: no downloaded script is run, and no native code is ever
     loaded.
   - Maps and resources are converted into the content cache by the custom content converter, on a worker thread:
     `ArenaConvUserContent::Run` with `StateDirName="netcache"`, so its state is `<content cache>/netcache/state.json`,
     separate from the custom content folder's. Its inputs:
     - `Files`: every package in the net cache, one version per name. This server's versions come first, then the
       most recently used.
     - `ReferencePackages`: the custom content folder's packages, which are found but never converted or removed here.
     - `bMaintenance=false`.
     The conversion is incremental (MD5 per file): rejoining converts nothing. A map converted with errors by an older
     converter revision is converted again once (`ArenaConvUserContent::ErrorRetryVersion`, the record's
     `error_retry`): DM-Big_City-Final-(Eisbaer) lost a static mesh whose `Materials` array is stored with a size 90
     bytes short (the converter now reads a struct array element by element up to the object's end, as UE2 does). A
     package with the name of a stock one is never converted over it (the converter's shadowing rule).
   - Then `ArenaRuntimeContent::ResetCacheIndex` and `AArenaLevelBuilder::RefreshImportedLayouts` run, and the
     server's map loads like any converted map.
9. **Progress**:
   - `ArenaNet::GetJoinProgress` gives the text and fraction, for example
     "Downloading DM-CBP2-Griffin.ut2 (1 of 3): 4.1 / 28.8 MB" or "Converting downloaded content: ...".
   - The server browser draws it as a band with a bar over the list. Its status line and
     `ArenaNet::GetConnectionStatus()` show the same text.
10. **Safety**:
   - Only package extensions are accepted (`.u .ut2 .utx .usx .ukx .uax`), never `.so` / `.dll` (UT2004 never
     downloads native code either).
   - `FNAME` must be exactly `<PKG>.<ext>`. Only letters, digits and `- _ . [ ] ( ) ! + ~ =` are allowed: no `/`,
     `\`, `:`, leading dot or `..`.
   - The destination folder is the GUID's own folder, checked to be inside the net cache.
   - Sizes are capped, and redirect URLs must be http(s).
   - `-UE2NetDownloadSelfTest` checks these rules and the `.uz2` decoder (16 checks).

## Step 3: the server's game state in our world

`FUE2NetWorld` (`Net/UE2NetWorld.h`) is created when our copy of the map has loaded and JOIN goes out, and gets the
connection's `OnActorOpened / OnActorUpdated / OnActorClosed / OnFunctionCall`. Values come from the received
properties with the class defaults below them (the UScript loader's default objects: `DefaultText`); the server's
Role and RemoteRole are swapped. Actors are classified by their class chain:

| UE2 class (chain) | Ours |
|---|---|
| `PlayerReplicationInfo` | an `AArenaPlayerState` (name, score, deaths, team, bot, spectator, out of lives, ping): the HUD, scoreboard and spectator bar use it. Our own PRI (the PlayerController's `PlayerReplicationInfo`) is the local spectator's player state |
| `GameReplicationInfo` | `AArenaGameState`: goal score, time limit, remaining time (counted down locally between `RemainingMinute` updates like UE2's `GRI.Timer`), match phase (`bMatchHasBegun`, `Winner`, `ClientGameEnded`), winner |
| `TeamInfo` | `AArenaGameState::TeamScores`; the team index of PRIs, flags and DOM points |
| `Pawn` (xPawn) | `AUE2NetPawn`: our character body with the player model and skins of the PRI's `CharacterName` (the install's player lists, `System/*.upl`: mesh, `BodySkin`, `FaceSkin`; team skins `<skin>_0 / _1`; the animation set from the cache's mesh record), `PawnPosition` extrapolated along its velocity and eased in (no lag behind a moving target, snaps above 4 m), yaw smoothed, aim pitch from `ViewPitch`, movement state from `bIsCrouched` / `CollisionHeight` and from each `PawnPosition` (`Physics` reaches a simulated pawn in its initial bunch only, so, like the UE2 client, we work it out: a water volume means swimming, a vertical speed of 100 UU/s or more falling, a small one or a fall's end walking once a floor is below; our native animation reads velocity and movement mode), `AnimAction` weapon switches, hit reactions from `HitFxTicker`, death on `bTearOff` (ragdoll with `TearOffMomentum`, gibs for damage types with `bAlwaysGibs`); the body stays 8 s after the channel closes. Camera: behind the pawn when the server says `ClientSetBehindView(True)` (stock spectating), else first person |
| `WeaponAttachment` | the third-person weapon of its `Instigator` (attachment class -> our weapon art), fire animation and muzzle flash on `FlashCount`, bullet tracers and impacts from `mHitLocation` / `SpawnHitCount` (assault rifle, minigun; for our own attachment, which gets no `FlashCount`, the impacts only: our weapon draws the tracer) |
| `ShockBeamEffect`, `NewLightningBolt`, `ShockCombo` | our shock / instagib / lightning beams and impacts (our own shock beams from our first-person weapon), the shock combo explosion |
| `SniperWallHitEffect` (and mods' `*WallHitEffect` copies), `RedeemerExplosion`, `SmallRedeemerExplosion` | the classic sniper rifle's hit effect, the Redeemer's nuke (`AArenaWarheadShell::SpawnNukeEffects`) and its small blast |
| `NewTransEffect*`, `NewTransDeres*` | the translocation flashes where a pawn arrives and where it left (`AArenaTransEffect`) |
| `LinkBeamEffect`, `PainterBeamEffect`, `ONSMineLayerTargetBeamEffect` (other players') | `AUE2NetBeam` from the player's third-person weapon to the replicated `EndEffect` while it exists (the mine laser until `bCancel`); the beams are camera-facing quads of the effect's texture and width (xEmitter beam textures run lengthwise, so `XEffectMat.Ion.painter_beam`'s stripe runs along the beam; an xEmitter beam is 1.5 x `mSizeRange` across, the painter's 12 UU, a BeamEmitter 2 x `StartSizeRange`; an xEmitter beam's colour is `mColorRange` x 2 x 1.8: UE2 adds it to the frame in gamma space, we add it in linear light, which over the mid-tone ground under a painted spot came out about 0.6 as bright as 3369). The painter's beam is dim where it starts, as 3369 draws it (measured against 3369 in the same view: 0.12 as bright at the start, closing on full as 1 - exp(-UU along / 38), from quads along its first 110 UU; `PainterBeamEffect` is the one PT_Beam xEmitter with a non-zero `mAttenKa`, the cause is native). Our own first-person beam starts where `PainterBeamEffect.Tick` puts it: the eye + `CalcDrawOffset` (`PlayerViewOffset` x 0.9 / `DisplayFOV` x 100, Y x Hand) + its `EffectOffset` (-5, 15 x Hand, -7) in the view axes (the pawn's location for the hidden hand); seen from outside, and other players', at the third-person weapon's tip. The painter's brightness and width (`mSizeRange`) follow `TargetState` as `PainterBeamEffect` does (40 aiming, up to 250 once marked; acquired: 250 and widening to 16 UU for its 0.4 s, then the `Cancelled` fade to 40 and 1 UU, since `TargetState` stays acquired on the wire), with its `MarkSound` / `AquiredSound` and its hum (`AmbientSound` TAGFireA, `SoundVolume` 45, `SoundRadius` 120, off once acquired). On a dedicated server the beam is gone 4 game seconds after it came (its xEmitter `mLifeRange`, as measured against 3369; the cause is native) unless a strike ended it first: painting on without a mark then shows no beam, ours included. Ours: our own weapon draws the beam while we hold fire and until our `PainterBeamEffect` closes (released: `TargetState` cancelled, its 0.4 s fade at the last end); our painter takes `TargetState` from it. A strike puts the painter down at once (`ClientSwitchToBestWeapon`: the best other weapon with ammo), so our beam ends there (stock UT2004 would point it at the new weapon's missing `EndEffect`) |
| `ShockCombo` | also takes the place of the shock core's own explosion (the core's close waits 0.1 s for it) |
| `Projectile` | `AUE2NetProjectile` with the art of our projectile table (rocket, flak chunk and shell, shock core, link plasma, bio, grenades; others a glow), flown along the replicated velocity with gravity when falling. **`bNetTemporary` projectiles** (rockets, flak, link plasma...) are sent once and their channel closes at once: the client flies them itself until they hit the world or a pawn (then their explosion / impact) or their `LifeSpan` ends; flak chunks and grenades bounce. Other projectiles explode when their channel closes |
| `Pickup` | the map's `AArenaPickup` nearest to the replicated location (pickups are spawned actors in UE2): shown / hidden by `bHidden` (taken / respawned) |
| `Mover` (map actors) | the map's `AArenaMover` of the same export name (`ArenaUE2World::FindActorByName`), moved by `SimInterpolate` (X = 100 * alpha, Y = 100 * rate, Z = 256 * previous key + key) and `RealPosition / RealRotation`. In `?UE2Net=1` worlds every mover is the server's from the start: our pawns standing on it, bumping it, local triggers and encroachment do nothing |
| `CTFFlag` | our `AArenaFlag` of its team: home, carried by the pawn of `HolderPRI` (attached like ours), dropped at its location |
| `xDomPoint` | our `AArenaDOM_ControlPoint` with the same letter: `ControllingTeam`, locked while not `bControllable` |
| `Vehicle` (ONS vehicles, Assault vehicles and turrets) | `AUE2NetVehicle`: the class's own skeletal mesh (`Mesh` default), or its static mesh when drawn as one. When the mesh is one of our vehicle types' (`ArenaArtVehicles.h`: Manta, Scorpion, Goliath, Raptor, Hellbender), our art rig draws it (team skins, wheels spinning with the forward speed and steering from the yaw rate, tank rollers, thruster glows); other classes (Leviathan, SPMA, Paladin, mods ...) get their mesh with `RedSkin` / `BlueSkin`, and their wheels from the class's `SVehicleWheel` subobjects (`BoneName`, `WheelRadius`, `SteerType`; read from the package's saved subobject properties) spin, steer and follow the ground. Wheel suspension is not replicated: every wheel (ours and the rig's) traces the ground under it each frame and moves along the chassis up axis within its travel (60 % of the radius). Moved by the KRigidBody state struct (`ChassisPosition`, `ChassisQuaternion`; `ChassisLinVel` is 10 x the velocity in UU/s, see below), extrapolated and eased like pawns. `Driver` rides along (seated, see `ONSWeaponPawn`); `AmbientSound` / `SoundVolume` is the engine loop; `bDestroyAppearance` swaps in the class's `DestroyedVehicleMesh` (still moved by its rigid body) with an explosion, `bDisintegrateVehicle` a bigger one, `ExplosionCount` adds damage explosions |
| `ONSWeaponPawn` (gunner seats; `ONSStationaryWeaponPawn` turrets) | the gunner (`Driver`) is seated on its `Gun` of `VehicleBase` when the class draws it (`bDrawDriverInTP`, at `DrivePos` / `DriveRot`), in the model's `Vehicle_Driving` pose (pool models and the 2004 characters' own animation sets); a model without one keeps its idle stance, as UE2 does when `PlayAnim(DriveAnim)` finds nothing. A stationary turret (no `VehicleBase`) is a vehicle of its own |
| `ONSWeapon` (turrets, gun pods) | its own mesh (class `Mesh`, `RedSkin` / `BlueSkin`) on the vehicle's `AttachmentBone`; `YawBone` / `PitchBone` turn towards the replicated `CurrentHitLocation` (at most a few degrees per frame, like UE2's turret rotation rate), within the class's `PitchUpLimit` / `PitchDownLimit` and `YawStartConstraint` .. `YawEndConstraint` (the Manta's guns: -45 .. 45); a gun whose yaw and pitch bone are the same bone (Manta, Raptor) gets both on it. A gun our rig already draws (same mesh) drives the rig's driver or rear turret instead; a rig gun the rig keeps fixed (the Hellbender's side gun) is replaced by an aimed one. `FlashCount`: muzzle flash at `WeaponFireAttachmentBone` + `WeaponFireOffset` along the aim (tracer and impact for `bInstantFire` weapons); `HitCount`: impact at `LastHitLocation` |
| `GameObjective`: `ONSPowerCore` / `ONSPowerNode` | our `AArenaONS_PowerNode` made from the same map actor (`UE2SourceName`, the export name): `CoreStage` (0 active, 2 constructing, 1 / 4 neutral), `DefenderTeamIndex`, `Health` of `DamageCapacity` (construction progress counts it up), `bUnderAttack` / falling health (the HUD's flashing), `bPoweredByRed` / `bPoweredByBlue` (attackable / buildable). The node's own built / destroyed effects play on the changes; local construction, touch and damage are off (`SetNetState`). The ONS HUD (minimap, links, core health) reads the nodes |
| `GameObjective` (Assault) | our `AArenaAS_Objective` made from the same map actor (all of the server's: in `?UE2Net=1` worlds our rules keep bot-only and unreachable ones). Current, as UT2004 works on them: active (`bActive`, not `bDisabled`) objectives of the highest `DefensePriority` (our stage; several when they share it, e.g. AS-Glacier's two consoles) plus active bonus objectives; completed = `bDisabled`. `DestroyableObjective` `Health` (the map's `DamageCapacity` is not replicated: the full health is the highest reported, or the map's when none was sent), `bIsCritical` / `bIsUnderAttack`. Hold / use progress is not replicated (only `HoldObjective.bIsHeld`; `SavedObjectiveProgress` is the round's saved total): while held, it is counted locally over the objective's time. `ASGameReplicationInfo.ObjectiveProgress` (objectives completed this round) is our stage counter. `ASGameReplicationInfo`: `bTeamZeroIsAttacking` (attacking team), `CurrentRound`, and the round time pod: `RoundTimeLimit` - (`ElapsedTime` - `RoundStartTime`), `ElapsedTime` counted locally between updates. Objectives our rules left out (bot-only helpers, unreachable ones) are not shown |
| `xBombFlag` (bombing run ball) | our `AArenaBR_Ball` (`AArenaCarryable::SetNetState`): carried by the `HolderPRI`'s pawn (our local pawn for our own PRI), thrown (falling with a velocity: flown locally between updates, bouncing), dropped, home. Loose, it shows its replicated `StaticMesh` at its `DrawScale` (`E_Pickups.BombBall.FullBomb`, 1.5); carried, our ball model on the carrier. No rules run (no pickups, scoring or returns here). Other `GameObject`s: a placeholder with their static mesh |
| Everything else | ignored. Known infrastructure (inventory, squads, emitters, `LevelInfo`...) silently, the rest counted as unknown (mod classes) |

Calls on our PlayerController: `ReceiveLocalizedMessage` (xDeathMessage: the console kill line from the damage type's
`DeathString` / suicide string in the install's `.int` files; FirstBlood, KillingSpree, MultiKill, SpecialKill,
xKillerMessagePlus, xVictimMessage, TimerMessage, StartupMessage: our UT2004 local messages and fonts; CTFMessage:
centre message in the team colour; **every other class** (`ONSOnslaughtMessage`, `ONSVehicleKillMessage`,
`xBombMessage`, Assault and mod messages, `GameMessage`): the class's static `GetString` is run by the UScript VM with
the received switch, transient script stand-ins for the related PRIs (their `PlayerName`, `PlayerID`, `Team`) and the
optional object (a team, a PRI, or a static object / class), and shown with the class's own look: `GetColor`,
`GetFontSize`, `GetLifeTime` per switch where the class has them, else its `DrawColor`, `FontSize`, `Lifetime`, plus
`PosX` / `PosY`, `DrawPivot`, `StackMode`, `bIsUnique`, `bFadeMessage` (UT2004 fonts, our local message list);
not `bIsSpecial` (or `bIsConsoleMessage`) ones go to the console lines; `AnnouncerSounds[Switch]` is played when the
class has one), `TeamMessage` / `ClientMessage` (console lines; team chat in the team colour),
`PlayStartupMessage`, `ClientHearSound` (positional, the same sound id only once; `Parameters` = (Volume * 1000,
Radius, Pitch * 1000), see below) and `ClientPlaySound`,
`ClientSetViewTarget`, `ClientSetBehindView`, `ClientGameEnded`. The announcer: first blood, flag taken / dropped /
returned / scores, and the sprees, multi kills and head shots of the followed player (AnnouncerMale2K4).

The local game mode runs no rules or timers in `?UE2Net=1` worlds (the server's state drives the match), and
`?SpectatorOnly=1` logins get our `AArenaPlayerController` (`ReplaySpectatorPlayerControllerClass`; the engine default,
a plain `APlayerController`, had no HUD messages or input).

Spectator controls (`ArenaNet::HandleSpectatorFire`, called first by the player controller's fire handlers): fire sends
`ServerViewNextPlayer`, alt fire `ServerViewSelf` (free camera; from the free camera, alt fire follows the next player
again). The server answers with `ClientSetViewTarget`. Its reference is None when the new target's pawn is not yet
relevant to us (actors without an open channel serialize as None): like a UE2 client we then call
`ServerVerifyViewTarget`, and the answer names the pawn, which is open by then. A pawn named before its channel opens
is followed when it opens. While following a player, their next pawn is followed after a death. When the cycle
reaches our own controller, the view is the free camera. The spectator bar shows "Now viewing <name>" with the
player's score and health (`Pawn.Health` is replicated to everyone; ammo, inventory, adrenaline and the weapon only to
the pawn's owner, so HudCDeathmatch's other widgets have nothing to show for a spectator), else "Spectating <server> -
Fire: view a player, Alt-Fire: free camera".

Values confirmed from captures:

- `ChassisLinVel` (all KRigidBody vehicle states) is 10 x the velocity in UU/s: the velocity from successive
  `ChassisPosition`s of a Manta over 1.5 s is 0.08 .. 0.11 x `ChassisLinVel` (ONSVehicle packs it with 10 x and unpacks
  it with 0.1 x).
- `ClientHearSound` `Parameters` = (Volume * 1000, Radius, Pitch * 1000): xPawn landing sounds (volume
  `FMin(1, ...)`) top out at 1000, `TransientSoundVolume` 0.3 (pickups, weapon switches) arrives as 300, ONS vehicle
  guns' `FireSoundVolume` 160 / 255 as 627, `LightningGunChargeUp` (pitch 1.1) as 1100; radii are the actors'
  `TransientSoundRadius` (300 by default, 400 .. 600 for weapons and vehicle collisions). The falloff per radius: full
  volume within half the radius, logarithmic over 8 radii (approximated; UE2's audio is not reproduced exactly).
  Levels follow UE2's gain staging: the whole mix plays at UT2004's default effects volume (`SoundVolume` 0.3,
  `arena.AudioMixLevel`), so volumes above 1 (up to 1 / 0.3, OpenAL's gain cap) stay louder; ambient loops use
  `SoundVolume / 255` times ALAudio's `AmbientVolume` (0.5) in place of the effects volume, and map `AmbientSound`
  actors also their class's `AmbientVolume` (0.25).

Every 30 s and at the end the log has a `UE2NetWorld summary`: live / opened actors per kind, projectiles flown and
their average distance, pawn updates with the distance between the shown and the received position, pickups matched,
messages, kill lines, sounds, unknown classes and message classes not shown; a second line (`game objects`): power
nodes matched / updates, Assault objectives matched / updates, ball updates, vehicle aim updates, weapon flashes, seats
shown, wrecks and script-run messages. `-UE2NetSpectateScript` `ball` views the bombing run ball (its carrier, else the
free camera at it). The game-object line includes an **aim check**: every 2 s, for each vehicle gun whose aim point
is within its limits and that has turned onto it, the angle between the barrel bone's X axis and the direction to the
replicated `CurrentHitLocation`.

## Step 4: playing on the server

`FUE2NetPlay` (`Net/UE2NetPlay.h`) is created with the world layer and gets every connection event first; it handles
what is about our own player and passes the rest on (`UE2NetJoin.cpp`). All UE2 rules below come from the 3369
scripts in the player's install (`Engine.PlayerController`, `UnrealGame.UnrealPlayer`, `XGame.xPlayer` / `xPawn`,
`Engine.Weapon`), disassembled for this step; the physics constants were measured against the 3369.3 server.

### Our pawn

- The server possesses a pawn for us with `ClientReStart(NewPawn)` / `GivePawn(NewPawn)`. `NewPawn` is often None,
  because the pawn's channel opens in the same packet, so the PlayerController's replicated `Pawn` and any xPawn whose
  role is AutonomousProxy also name it. We spawn an `AUE2NetLocalPawn` there: an `AArenaCharacter`, with our UE2
  movement (`UArenaCharacterMovement`), first-person camera, view models and HUD. The local player possesses it, and
  its player state stops being a spectator.
- **`ServerAcknowledgePossession(Pawn, Handedness, bAutoTaunt)`** must follow: until then 3369's `ServerMove` returns
  early (`AcknowledgedPawn != Pawn`) and only calls `GivePawn` again. Handedness is the settings' weapon hand minus one.
- The pawn is a proxy (`AArenaCharacter::bNetProxy`). It takes no damage, never dies or picks anything up by itself,
  and gets no default inventory. Pickups, flags and damage are the server's. The other players' stand-ins
  (`AUE2NetPawn`) are not obstacles for it: the server's own pawn collisions come back as corrections.
- The world layer leaves our xPawn's channel alone (`FUE2NetPlay::IsOwnPawnChannel`, one check in `ApplyPawn`).
- **Death**: `bTearOff` on our xPawn. Our pawn is replaced by a ragdoll `AUE2NetPawn` (the death camera, behind the
  body), and the HUD shows "You died. Press Fire to respawn!". Fire sends `ServerReStartPlayer`. The first spawn
  happens the same way: once joined, the client sends `ServerSetReadyToStart` and `ServerReStartPlayer`, as UT2004
  does when its login menu closes. The server starts a new player in `PlayerWaiting` (`bWaitingPlayer`). Before the
  match begins (`DeathMatch.PendingMatch`: waiting for `MinNetPlayers` / `NetWait`, or for ready signals when
  `bPlayersMustBeReady`), `ServerReStartPlayer` is the ready signal (`bReadyToPlay`): the server's `StartupMessage`
  stages show as local messages ("Waiting for other players.", the count down) and the spectator bar says "You are
  READY - waiting for the match to begin"; the match start spawns us. If the server leaves us a pawn without weapons
  while its controller is in `PlayerWaiting` (a player agreement answered after a match began), Fire sends
  `ServerReStartPlayer` as `PlayerWaiting.Fire` does, and the restart hands out the inventory.
- **Waiting for the next round**: round games (3SPN / Freon, `ArenaMaster.PostLogin`) put a player who joins
  mid-round out of lives (`PRI.bOutOfLives`) and into `Spectating`, and spawn them only when the next round starts;
  `ServerReStartPlayer` does nothing until then. Without a pawn the client sends the spectator's `ServerMove`
  (UE2 `BaseSpectating.PlayerMove` does in `PlayerWaiting` and `Spectating` too), and the server's corrections carry
  its controller state. While our PRI is out of lives or the state is `Spectating`, Fire / Alt-Fire are the
  spectator's (`ServerViewNextPlayer` / `ServerViewSelf`) and the bar says the player joins at the next round.

### Time

UT2004 games run at `Level.TimeDilation` 1.1 (`GameInfo.SetGameSpeed`; the LevelInfo default in 3369). Our world runs
at the same dilation while joined, so world seconds are the server's game seconds: move time stamps, the server's
speed hack check and all replicated velocities line up. A frame is at most `MaxResponseTime` (0.125 game seconds), as a
UE2 client's move is (`AWorldSettings::MaxUndilatedFrameTime`). Longer frames (hitches) slow the game down instead of
putting us ahead of the server, which clamps each move to that.

### Moves (`ReplicateMove` / `CallServerMove`)

Every physics step of our pawn is a saved move: time stamp, delta, acceleration, input flags (`AArenaCharacter::
LastStepInput`: the dodge direction `DoubleClickMove`, jump press, double jump, walk, duck), view rotation and the
resulting location and velocity. Moves are sent like UE2 does:

| Call | When | Parameters |
|---|---|---|
| `ServerMove` | one move | `TimeStamp, InAccel, ClientLoc, NewbRun, NewbDuck, NewbJumpStatus, NewbDoubleJump, DoubleClickMove, ClientRoll, View, [OldTimeDelta, OldAccel]` |
| `DualServerMove` | two moves in one bunch (more than one frame since the last send) | `TimeStamp0, InAccel0, PendingCompress, DoubleClickMove0, View0, TimeStamp, InAccel, ClientLoc, DoubleClickMove, ClientRoll, View, [OldTimeDelta, OldAccel]` |
| `ShortServerMove` | a move without acceleration, dodge or double jump | `TimeStamp, ClientLoc, NewbRun, NewbDuck, NewbJumpStatus, ClientRoll, View` |

- `InAccel` is the acceleration in UU/s² times 10, because vectors travel as whole numbers. It is the acceleration we
  simulated with (`AccelRate` 2048 at full input). The 3374 client sends the input acceleration capped at 3072
  (`SavedMove.SetMoveFor`, 30720 on the wire at full input); its own positions (and the server's) show that the
  native physics limits it to `AccelRate`, so both arrive at the same motion.
- `NewbJumpStatus` toggles with each jump press (the server compares it with its last value).
  `PendingCompress` = run0 1, duck0 2, jump status0 4, double jump0 8, run 16, duck 32, jump status 64, double jump 128.
- `View` = `(32767 & (Pitch / 2)) * 32768 + (32767 & (Yaw / 2))`.
- `OldTimeDelta` (500 per second) / `OldAccel` repeat an important move of the past half second, in case its packet
  was lost: as UE2's `ReplicateMove` picks `OldMove` among the moves not yet confirmed, the newest jump / dodge move,
  else the newest one whose acceleration turned (dot product of the directions < 0.95). `OldAccel` packs
  `CompressAccel(0.05 * accel + 0.5)` per axis at bits 23 / 15 / 7, plus 64 run, 32 double jump, 16 jump and the
  dodge direction. (Before: jump and dodge moves only.)
- **Cadence**: a move is held back and sent with the next one (`DualServerMove`) while less than `NetMoveDelta` has
  passed since the last send, counting game time * `TimeDilation` * 0.91 as UE2 does. `NetMoveDelta` follows the
  OldUnreal 3374 script: `FMax(0.011, 2 * MoveRepSize (42) / CurrentNetSpeed)`, the net speed capped by the server's
  `ClientCapBandwidth` (stock servers: 10000), so 0.011 s. 3369 used `FMax(0.0222, ...)` unless the net speed was
  above 10000 with at most 10 players, so we sent at most every 1/45 s and below 45 fps two frames per packet. The
  3374 client measured at 50 fps sends one `ServerMove` per frame; ours now does the same. A jump or dodge goes at
  once. `-UE2NetLegacyMoves` (test hook) brings the old cadence and the old `OldMove` choice back for A/B runs.
- UE2 does not combine moves the way we save them (one per physics step): its `ReplicateMove` merges a held-back
  walking move into the next one when the acceleration direction is the same. That only changes how many moves a
  packet carries, not where the pawn goes.
- `ClientLoc` is our location in UU plus the measured offsets below.
- **Zero parameters**: like UE2's `UNetDriver::ProcessRemoteFunction`, a parameter equal to zero (0, 0.0, a zero
  vector or struct, None, an empty string) is sent as a single 0 bit (`FUE2NetConnection::SendFunction`); the
  receiver zeroes it. Before, we sent zeros in full: `ServerStopFire(0)` took 15 bits instead of the 3374 client's 7,
  a ShortServerMove's zero `ClientRoll` 9 instead of 1.

### Corrections (`ClientAdjustPosition` and its short forms)

The server answers a move whose `ClientLoc` is more than √3 UU off (squared error > 3) with `ClientAdjustPosition`
(time stamp, state, physics, location, velocity, base), `ShortClientAdjustPosition` (no velocity),
`VeryShortClientAdjustPosition` (walking) or `LongClientAdjustPosition` (with the floor). **3369 has no
`ClientAckGoodMove`**: saved moves older than an adjustment, or than 2 s, are dropped.

On an adjustment the client does what `LongClientAdjustPosition` / `ClientUpdatePosition` do. If the saved move of that
time stamp is already within 3 UU and 3 UU/s ("already matching"), nothing happens. Otherwise the pawn takes the
server's physics, location and velocity (relative to a mover base) and **replays every newer saved move**:
`UArenaCharacterMovement::SimulateNetStep`, with the move's inputs through `AArenaCharacter::StepInputOverride` and
`DoJumpUE2(bUpdating)` (no double jump from a replayed jump press, as in UE2). The log counts the corrections and their
size, and every 30 s prints a `UE2NetPlay summary`. Test runs log each correction with its details.

### What the server's physics turned out to be

`-UE2NetPlayProbe` makes the server report its pawn's state for every move. The client logs it next to its own. What
that showed, and what our pawn now does to match:

| Finding | Our pawn |
|---|---|
| A walking xPawn (height 44) rests 5.85 UU above our floor; our capsule rests 1.07 UU above it. Falling, it lands when it touches (lower) and rises over the next moves | walks with a 4.78 UU taller capsule, falls with the plain one, and is lifted when it lands (`AUE2NetLocalPawn::ApplyNetCollision`) |
| Walking velocity is computed once per move, for the whole DeltaTime; only the movement is split | no movement substeps (`MaxSimulationTimeStep` 0.15) |
| Braking without input (`APawn::calcVelocity`, read from the server binary): while more than 0.03 s remain, `V -= V * 0.06 * Friction` per 0.03 s step, the result being the average velocity of the steps whose velocity still points the old way; the last step `V -= 2 * V * Friction * rest`; zero when it reverses or falls below 10 UU/s | `UArenaCharacterMovement::bUE2ServerBraking` (this loop, no fit) |
| Falling (jumps, dodges, double jumps, gravity) matches our midpoint integration to 0.01 UU | as before |
| UE2 collides a pawn with the world (BSP, static meshes, terrain) as an axis-aligned box of half size (R, R, H); the cylinder is only for actor against actor | `UUE2NetBoxCollision`: our pawn's collision is that box (it does not turn with the view). `-UE2NetCapsuleCollision` brings the capsule back (A/B) |
| `ULevel::MoveActor` extends each move's trace by 2 UU and pulls a hit back by 2 UU along the move (`TestAdjust`: the constant 2.0 next to the hit time in the binary); a hit within 2 UU does not move the actor. Pawns stop up to 2 UU off walls and land one step early | `UArenaCharacterMovement::bUE2MoveBackoff`: the same for falling moves and mostly horizontal walking moves. `-UE2NetNoBackoff` turns it off (A/B) |
| The floor height differs by surface (BSP, each static mesh, terrain) and steepness | a height offset learned per floor component and steepness class from height-only corrections; the class average for a surface not seen yet |
| On a lift the adjustment's location is relative to the lift, and the two copies of the lift move on their own clocks | matching compares our location relative to our lift at the end of that move |
| Swimming at part input: UE2 `calcVelocity` has no analog speed limit (a part input reaches `WaterSpeed` / `GroundSpeed`, only more slowly) | net play skips the engine's `AnalogInputModifier` limit in `UArenaCharacterMovement::CalcVelocity` (a 0.6 input swam 88 UU/s slower than the server) |
| Falling while pressing into a wall (jumps, dodges and falls along walls): the server's pawn gets no air control at all. Its horizontal velocity stays exactly as it was (0 into the wall, unchanged along it, also above the maximum speed during a dodge), so it never drifts closer than where it took off. physFalling's `TestWalk` check (the next step's horizontal move with air control) cuts air control when it would hit something: to 0.05 on a walkable slope, to 0 on a wall | `UArenaCharacterMovement::GetAirControl`: 0 when the test move (with the pawn's own collision shape, the box in net play) hits a wall. Ours drifted up to 2 UU into the wall's 2 UU gap during every jump and slid along it with air control: one correction after another while jumping along walls |
| Air control at the maximum speed: the server's horizontal speed is bound only while it is already above the maximum (`BoundSpeed`); from at or below it, one step may go past it (440 → 444.7 UU/s at 7 ms steps, 448.4 at 11 ms) | `CalcVelocity` (falling) binds to the current speed only when that is above the maximum |
| Floors under the box: UE2 finds the floor with the pawn's box (physWalking's drop to the floor, physFalling's landing), so a contact anywhere under the bottom face counts, also at a corner (up to √2 R from the centre). The engine's capsule rules (a contact within the radius; its flat-base floor check is an octagon inside the capsule) rejected those: our pawn hung in the falling state at the floor after landing on a corner contact (often next to a wall, 3.9 UU below the server's walking pawn, until the next correction or jump), and walked off ledges the server's pawn still stood on (on DM-Rankin the server's pawn walked 32 UU higher along a ledge for 40 s while ours dropped back after every correction: 964 corrections) | for a box pawn `UArenaCharacterMovement::FloorSweepTest` sweeps the box (a walkable contact is the floor; where the box only catches a step's edge with a contact normal that is not walkable, the engine's own floor sweep decides, or the pawn slid down stairs it pressed against) and `IsWithinEdgeTolerance` accepts the whole bottom face (less the engine's 0.15 cm margin) |
| `PHYS_None` in an adjustment (Assault's `RoundEnded`, the end of a match): the server's pawn stands still | our pawn stops too (`DisableMovement`); before, it kept falling or walking and drew a correction every move until the next round |

Remaining differences are mostly geometry. Static meshes with a UE2 collision model collide with that model since map
schema 7 (the pillar `PF_Ancient_Pillar2` on DM-1on1-Spirit: a correction every move while pushing into it before,
one now). Some still differ: DM-Rankin's `houredoorframe` (its collision model blocks our pawn on the floor where the
server's pawn steps up, 24 - 37 UU) and AS-BP2-Outback's `painting1` (the server's pawn stops 5 - 8 UU before ours).
Terrain heights differ by up to 20 UU (DM-DesertIsle, DM-CBP2-KillbillyBarn). Test runs log each correction's
blocker (`wall probe`) and a `details` line: our and the server's velocity, the move's length, walking or falling,
and our floor component.

### Firing, weapons, HUD

- **Fire / alt fire**: `Weapon.ServerStartFire(Mode)` on press and `ServerStopFire(Mode)` on release, on the current
  weapon's channel. NewNet weapons send their own timestamped start fire instead, once the weapon is ready (see
  "NewNet"). The local weapon plays the shot's sound, muzzle flash and view animation at its fire rate while
  the button is held (`AArenaWeapon::PlayNetFireEffects`; UE2 clients play their own shots too), and the assault
  rifle's and minigun's tracers along our view. Charged modes (shield gun, bio and assault rifle alt, rocket load) play
  their shot on release (the shield gun also when a pawn is in front of it, as `ShieldFire.Timer` does on the owning
  client every 0.15 s of the hold, and not again when the button comes up); the shield (with the 15 energy it needs), the link beam, the mine layer's laser and the painter's beam show
  while held (`AArenaWeapon::SetNetFireHeld`). Zooms are the client's own as in UE2: alt fire runs the weapon's own
  zoom (lightning gun, classic sniper rifle, painter; the AVRiL's while the server's `ONSAVRiL.bLockedOn` /
  `HomingTarget` say it is locked), reset when the weapon goes down. The AVRiL locks (`ONSAVRiL.WeaponTick`, server
  side) through `Controller.PickTarget`, which only finds pawns with a controller: driven vehicles, not empty ones (in
  team games only the other team's). The translocator plays one throw / recall per
  press with its charges from `TransLauncher.RepAmmo` (full until it first comes) and whether `TransBeacon` is out;
  the translocations themselves are the server's (`NewTransEffect*` / `NewTransDeres*`). Instagib's two modes share
  one refire time (`bSharedFireMode`). Projectiles, beams, bullet impacts, hits and kills come from the server.
- **Guided Redeemer**: alt fire makes the server possess a `RedeemerWarhead` (`ClientRestart`, then our autonomous
  pawn): we fly it like `PlayerController.PlayerRocketing` (`AArenaGuidedWarhead` in net mode: its
  `UpdateRocketAcceleration`, view and scope HUD) and send `RocketServerMove(TimeStamp, Accel * 10, Location, bDuck,
  ClientRoll, View)` at most every 1/45 s; the server's corrections (`ClientAdjustPosition`) are replayed with our
  moves since. Fire / alt fire send `RedeemerWarhead.ServerBlowUp`; `bStaticScreen` shows the static until the server
  gives us our pawn back (`ClientRestart`). Meanwhile our old body is an ordinary replicated pawn. Other players'
  warheads are shown as the missile (projectile kind).
- **Weapons**: weapons are replicated to their owner only; each is our inventory. The class is mapped with
  `UE2NetActors::WeaponClassFor`, and `InventoryGroup` / `Priority` come from the class defaults.
  - `Weapon.ClientWeaponSet(bPossiblySwitch)` brings the first weapon up, and a better one when `bPossiblySwitch`.
  - Next / previous weapon and the number keys pick from that list.
  - Switching sends `xPawn.ServerChangedWeapon(Old, New)`, like `Pawn.ChangedWeapon` on a client.
  - Ammo is the weapon's `Ammo[0]` (an `Ammunition` actor's `AmmoAmount`) or `AmmoCharge`, plus `ClientForceAmmoUpdate`.
- **Owner-only values** go into our pawn for the HUD:
  - xPawn: `Health`, `ShieldStrength` / `SmallShieldStrength`, and `GroundSpeed`, `JumpZ`, `AirControl`, `AccelRate`,
    `WaterSpeed` (speed combo, mutators).
  - PlayerController: `Adrenaline`.
- **Hits**: `UnrealPlayer.NewClientPlayTakeHit` gives the HUD's damage direction indicator.
- **Other input**:
  - Use: `ServerUse`. Suicide: `Suicide`, the PlayerController's own reliable function.
  - Team: the `SwitchTeam [n]` console command sends `ServerChangeTeam`.
  - Ready: the `Ready` command sends `ServerSetReadyToStart`, and `ServerReStartPlayer` while waiting. 3369 has no
    "not ready" call.
  - Combos: the adrenaline key sequences of `AArenaCharacter` send `xPlayer.ServerDoCombo(<combo class>)`, the class
    as a static object reference.
- **Chat**: `Say` / `TeamSay` (the player controller's commands and the T / Y typing line, `ArenaChat`) end in
  `FUE2NetPlay::Say`, which sends `ServerSay` / `ServerTeamSay`. The test script's `say` can chat only to a server on
  this machine.
- **Our name**: the server may change the name we log in with (`Net Tester` becomes `Net_Tester`, a taken name gets a
  number). The PlayerName of our own PRI replaces our player state's name, so kill lines, the scoreboard and the HUD
  use it (`UE2NetPlay: the server calls us "..."`).

### Vehicles, ladders, lifts, death camera

- **Vehicles**: Use (`ServerUse`) near a vehicle. The server possesses the vehicle (`ClientRestart` / `GivePawn` with
  a Vehicle), and we acknowledge it with `ServerAcknowledgePossession` like any pawn (without it the server keeps
  sending `GivePawn`). Our pawn hides, stops moving and hands our xPawn to the world layer, which shows us as the
  driver. The camera sits behind the vehicle. Driving sends `ServerDrive(aForward, aStrafe, aUp, bJump, View)` as
  `PlayerDriving` does (throttle = aForward / 5000), at most every 1/45 s, and every 0.5 s when nothing changes. Fire
  is `VehicleFire(bAlt)` / `VehicleCeaseFire(bAlt)`; the server aims turrets from `View`. Vehicles are not predicted
  (UE2 clients do not either): the vehicle is the server's. Leaving (Use again) gives us our xPawn back, with another
  `ServerAcknowledgePossession`.
- **Ladders**: the server's `PHYS_Ladder` (11) in an adjustment puts our pawn in a ladder mode
  (`UArenaCharacterMovement::CustomLadder`). `PlayerClimbing` sends `Acceleration = aForward * ClimbDir`; our pawn
  climbs along it at `LadderSpeed` (200 UU/s) and steps off onto a walkable floor going down. Our level has no ladder
  volumes, so the first move on a ladder comes from the server.
- **Lifts**: movers in a joined world move only with the server's `SimInterpolate` / `RealPosition` (world layer).
  The adjustment's location is relative to its `NewBase` (a mover, also on a dynamic channel).
- **Death camera**: the server does not replicate our own corpse to us in a usable form. The death camera shows a
  ragdoll at our predicted location (the server's, within a few UU) with the server's hit momentum, hit location and
  gib rule (`bAlwaysGibs` of the damage type). Dying in a vehicle shows the world layer's body.

### Spectators

A spectator's `ServerMove` moves the server's spectator by `SpectateSpeed * Normal(InAccel) * DeltaTime`
(`BaseSpectating.ProcessMove`, colliding with the world). In the free camera the client now steers the server's
spectator toward the camera, and the server's corrections tell it where that spectator is. The server computes
relevancy from its spectator's location, so far players are replicated around the free camera too. Following a
player sends no moves: the followed pawn is the view target.

### Results (local 3369.3 servers, editor `-game` build)

The machine ran several other agents' game instances at the same time (load average 8 to 24), so frame rates varied
from 9 to 140 fps between runs.

| Run | Moves | Corrections | Average / max error | Notes |
|---|---|---|---|---|
| Empty DM-Rankin, route over 12 pickups (nav graph), jump, double jump, two dodges, 73 s, 137 fps | 7132 | **31 (0.4 %)** | 4.2 / 9.3 UU | ledges / landings 13, walls 8, stairs 7, other 3 |
| Same route, two more runs (13-20 fps) | 1436, 940 | 151, 167 | 2.6, 3.6 UU | more moves end in a wall or on a ledge at large frame times |
| Same route with `-UE2NetLag=100`, 73 s, 2 runs | 1880, 8569 | 126, 402 | 2.5, 12.4 UU | the second run got stuck at a wall for a while; the first is like the runs without lag |
| DM-Rankin with 3 bots, route + `hunt=1`, 150 s, 32 fps | 3741 | 201 (5.4 %) | 6.7 / 254 UU | being hit (momentum) and walls; the 254 UU one is a hit. Server log `TestBots scores Player=4/0`; 4 client kill lines "... rode Player's lightning" / "ventilated by Player's assault rifle"; picked up the sniper rifle, health to 199 |
| Standing jump and double jump, 4 standing dodges (9 fps) | 183 | 0 for the jumps, 5 for the dodges | 8.2 / 15.2 UU | dodges land up to a frame apart from the server |
| CTF-Face3, no bots, `route=flag` | | | | "Player took the blue flag!", "Player captured the blue flag!"; the server then changed maps, and the client rejoined CTF-MoonDragon and captured again |
| Spectator, free camera flying across DM-Rankin for 60 s | 874 spectator moves | | | the server's spectator stays 26-82 UU behind the camera across the map (`-UE2NetNoSpectatorMoves`: it stays at the start) |

Also tested:
- Respawn after death and after `suicide`.
- Weapon switching by slot, next and previous (`ServerChangedWeapon`).
- Fire and alt fire (assault rifle ammo 100 to 86, shield gun charge 100 to 75 and back).
- `ServerUse`, `ServerChangeTeam`, `Ready`.
- Chat: `Say` and `TeamSay` came back from the server as `TeamMessage` (logged as `UE2NetPlay: chat from the server`).
- `ServerDoCombo` is sent (not triggered: it needs 100 adrenaline).
- The HUD shows health, adrenaline, the weapon bar, ammo, the first-person weapon and our score.

MangoHud (first 30 s of a run, 1280x720 window, editor build, shared machine): empty-server route 137.6 fps average;
DM-Rankin with 3 bots 32 fps; CTF-Face3 21 fps.

Strings: 8-bit strings from the server go through the browser's decoder (`ArenaServerBrowser::DecodeBytes(...,
true)`). The bytes are UTF-8 only when valid UTF-8, else Windows-1252; U+FFFD characters are dropped. Colour codes
(ESC R G B) are kept, in 8-bit and UTF-16 strings, and drawn as coloured runs: `FArenaDraw::TextUT` (HUD, scoreboard,
chat, messages) and `FGUI::Text` (menus, server browser) split a string at each code and draw each run in its colour
with the UT2004 fonts and the caller's alpha. Sizes, sorting and logs use the text without codes
(`StripColourCodes`, `ArenaServerBrowser::StripColours`).

Round 2 (box collision, exact braking, per-surface floor offsets, MoveActor back-off, lifts, vehicles, ladders):

| Run | Moves | Corrections | Average / max error | Notes |
|---|---|---|---|---|
| Empty DM-Rankin route, capsule and no back-off (`-UE2NetCapsuleCollision -UE2NetNoBackoff`), 3 runs | 1475, 2049, 7351 | 213, 81, 168 | 2.8 - 4.9 / 21 UU | 57 of the 168 against walls |
| Same route, box, no back-off, 2 runs | 6053, 2222 | 67, 38 | 8.3, 8.1 UU | 16 and 3 against walls, the server about 2 UU off ours |
| Same route, box and back-off (final build), 4 runs | 9543, 2533, 1673, 6198 | 27, 58, 71, 52 | 3.8 - 8.6 UU | 2 - 11 against walls, 1.7 - 3.2 UU |
| AS-BP2-Outback, walking diagonally into a BSP wall, 50 s | | no back-off 113, back-off 28 | | wall corrections 83 → 10 |
| Movement test (jumps, double jumps, 10 dodges), DM-Rankin | | before 41 (6.0 / 67 UU), after back-off in the air 44 (**2.2 / 11.5 UU**) | | dodge landings now within 1 - 3 UU; most of the rest are jumps against a wall (1.5 UU) |
| DM-1on1-Spirit lift, riding it up and down, 55 s | | **268 → 1** | 8.9 UU → one of 16 UU as the lift starts | comparing relative to the lift; the server drives our movers |
| ONS-Torlan: Manta, Hellbender (driven about 3300 / 1400 UU, in and out), the manual gun turret | | | | `ServerDrive`, the vehicle camera; no corrections while driving (the vehicle is the server's) |
| DM-CBP2-KillbillyBarn ladder (a server given the client's map paths), climbed 1260 UU at 200 UU/s | | climbing: 0 - 0.3 UU after the first | | the terrain around it: 10 - 30 UU height corrections (world side) |
| DM-Rankin with `TestAdrenaline=1` | | | | `ServerDoCombo`: Speed ("Speed!", GroundSpeed 440 → 616 → 440), Booster (health 150 → 199), Berserk |
| Login name `Net Tester` | | | | the server renames it `Net_Tester`; our player state follows |

Swimming (CTF-Face3's pool, `goto` laps inside it for 33 s; `goto=water` now picks the deepest point of a water
volume's UE2 brush, using the pawn's own collision): before the analog fix, 251 corrections in about 525 moves at a
0.6 input (a constant 88 UU/s speed difference); after it, **38 corrections in 2473 moves (1.5 %), 1.5 UU on
average**. On DM-Antalus the lake's deep part is behind terrain the server's pawn cannot pass either.

Map schema 7 (static mesh collision models, hidden collision meshes, invisible BSP collision), same tests:

| Run | Moves | Corrections | Average / max | Notes |
|---|---|---|---|---|
| DM-Rankin route, 3 runs | 2118, 1051, 925 | 235, 39, 57 | 8.4 / 41, 7.0 / 40, 6.1 / 47 UU | the 235 run spent 30 s stuck at the `houredoorframe` collision model (191 falling corrections); the others have 0 and 19 wall corrections |
| DM-1on1-Spirit pillar push | 368 | **1** (was one per move) | 6.4 UU | the pillar's collision model |
| AS-BP2-Outback wall push | 1673 | 253 (was 28) | 4.4 / 8.9 UU | the server stops at `painting1` 5 - 8 UU before ours |
| Movement test (jumps, dodges) | 2736 | **20** (was 44) | 2.7 / 9.0 UU | |

### Real client reference (OldUnreal 3374)

The reference: OldUnreal's 3374 Linux patch (preview 23) applied to a scratch copy of the player's install (`System/`
copied, the other folders linked read-only), run with its own HOME on its own Xvfb display and driven with xdotool on
that display only. It runs without a CD key (it makes its own `System/cdkey` in that HOME). A UDP logging proxy sat
between it and the server; a small decoder (packet and bunch framing, control strings, function calls by field index
with the parameter types of the scripts) read the captures, and our client went through the same proxy. Servers, all
local 3369.3 dedicated servers on 127.0.0.1 without uplink or stats: stock DM-Rankin, a WS3SPN TAM server with
WSUTComp_V6 (its stats link off), and a Vehicle CTF profile with NewNet weapons (its anti-cheat packages are not part
of it). None of this is in the repository.

| Topic | 3374 client | Ours before | Ours now |
|---|---|---|---|
| Handshake | `HELLO MINVER=3180 VER=3374`; HAVE in USES order, then `PETE`, `REPEAT`, `JOIN`, `OPENVOICE` | HAVE / SKIP in USES order, `JOIN` | unchanged (see "Control channel") |
| Move cadence | `NetMoveDelta` 0.011 s: one `ServerMove` per frame at 50 fps | at most one packet per 1/45 s: below 45 fps two frames per `DualServerMove` | the 3374 rule, capped by `ClientCapBandwidth` |
| Redundant old move | the newest jump / dodge move, else one whose acceleration turned | jump / dodge moves only | as the 3374 client |
| Zero parameters | one 0 bit | sent in full | one 0 bit |
| `InAccel` | the input capped at 3072 UU/s² | `AccelRate` (2048) | unchanged: the native physics caps it to `AccelRate`, the motion is the same |
| NewNet fire | `NewNet_ServerStartFire` with the class's own parameters (stamp, `DT`, view, eye location, believed hit) | the same | unchanged |

Movement against the same servers was compared with `-UE2NetPlayProbe` (the server's state for every move): standing
at a wall on DM-1on1-Spirit and jumping while pressing into it, the server's pawn kept its distance from the wall and
landed walking; ours drifted 2 UU towards the wall in every jump (air control) and then hung in the falling state at
the floor 3.9 UU too low (the box's corner contact refused as a landing). Both are fixed (see "What the server's
physics turned out to be"); the same test now matches to 0.05 - 0.15 UU in height and 0.2 UU from the wall.

Correction rates, our client through the route script (`route=pickups`, CTF-Face3 `route=flag`, with jumps, double
jumps and dodges) for 110 s on an empty local 3369.3 server, editor build:

| Map | Before: corrections / moves (falling) | After (falling) | Average / max error before → after | Left (after) |
|---|---|---|---|---|
| DM-Rankin | 80 / 13881, 0.58 % (42) | 32 / 8736, 0.37 % (13) | 5.6 / 45.5 → 2.4 / 10.8 UU | `houredoorframe` and a step down that block our box where the server's pawn passes, once per lap |
| DM-1on1-Spirit (lifts) | 92 / 16077, 0.57 % (61) | 64 / 6848, 0.93 % (19) | 2.1 / 13.9 → 6.5 / 39.2 UU | a 37 UU step the server's pawn steps up and ours does not (7, depending on the route), stairs walls |
| CTF-Face3 (jump pads) | 92 / 12948, 0.71 % (14) | 75 / 9889, 0.76 % (14) | 11.7 / 39.0 → 10.3 / 38.2 UU | pillar and door frame collision models, jump pad launches one move apart, floor heights of BSP sections |
| ONS-Torlan (terrain) | 83 / 12624, 0.66 % (30) | 76 / 9507, 0.80 % (9) | 4.1 / 20.8 → 4.8 / 12.9 UU | height only (43): the road trims' collision model is flat where the server's floor rises and falls 3 - 10 UU |
| AS-Convoy | 207 / 9589, 2.16 % (59) | 49 / 10330, 0.47 % (22) | 3.0 / 38.2 → 7.9 / 38.3 UU | before: a correction every move after `RoundEnded` (`PHYS_None`); walls of the spawn trucks |

"Before" is the build with the old physics, run with `-UE2NetLegacyMoves`; "after" sends at the 3374 cadence, so the server
checks more moves per second (a `DualServerMove`'s first move is never checked), and the frame rate (so the number of
moves) differed with the load other work put on the machine (60 - 145 fps). The falling corrections (jumps, dodges,
falls along walls, landings) dropped on four of the five maps (CTF-Face3: 14 and 14); most of what is left is geometry (collision models, BSP edges and
terrain under static meshes), checked per spot with the correction log's `details` line (our and the server's
velocity, the move's length, our floor component).
The offline movement rig (`DM-1on1-Spirit -MoveTest=all,weapons -benchmark -fps=60`) passes 286 of 286 checks with
these changes.

### Step 4 open problems

- Static meshes with a UE2 collision model, and terrain, differ from our runtime collision (see above). A wall the
  server's pawn stops at a few UU away from ours gives a correction every checked move while we push into it.
- Swimming at the surface (in and out of the water) is a few UU apart from the server.
- Steps near the step height: on DM-1on1-Spirit the server's pawn steps up a 37 UU ledge that ours stops at.
- Jump pads launch our pawn one move later than the server's (the pad's actor tick, not the move), and replayed
  moves do not touch pads.
- UE2 merges a held-back walking move into the next one (`ReplicateMove`); we send each physics step as its own move.
- No "not ready" call exists in 3369; tournament ready-up uses `ServerReStartPlayer` while the match waits.

## Testing, evidence, open problems

Tested against local dedicated servers (`Scripts/ut2004-test-server.sh`, native Linux 3369.3, bound to
127.0.0.1, no uplink), plus single approved spectator joins on a live 3374 ONS server.

| Check | Result |
|---|---|
| Handshake | HELLO, CHALLENGE, LOGIN, USES (204), WELCOME, JOIN; server log `Open myLevel ...`, `New Player Player` |
| Decoding | DM-Rankin, 3 bots, 80 s: 403 actors opened, 5444 properties, 401 function calls, **0 bunch errors**; GRI, PRI names, pawn locations sane |
| Stay connected, disconnect | 80 s connected, then the server logs `Close TcpipConnection` at our quit |
| Map changes | two server travels followed, 0 errors |
| Missing packages | with no install (178 packages missing), SKIP keeps the numbering right: GRI and PlayerController decode |
| Client to server call | `ServerViewNextPlayer` answered by `ClientSetViewTarget` |
| Downloads, HTTP redirect | 3SPN TAM server (`3SPNv3141.TeamArenaMaster`) on the custom map DM-CBP2-Griffin; the client's install has neither the map, its texture package `R3tex` nor `3SPNv3141.u`. All three were fetched as `.uz2` from a local `python3 -m http.server`, GUIDs match, files byte-identical. The map converted in 0.4 s, loaded and rendered. `3SPNv3141.Misc_Player`, `Misc_Pawn`, `Misc_PRI`, `TAM_GRI`, `TAM_TeamInfo`... decode with 0 bunch errors |
| Downloads, file channel | the same 49.5 MB with `-UE2NetNoRedirect`: 56 s at `MaxInternetClientRate=1000000` (about 9 KB/s at the stock 10000), then converted, loaded and joined with 0 bunch errors. A redirect 404 falls back to the file channel |
| Download refused | `AllowDownloads=False`: each request is answered by an empty close; the packages are `SKIP`ped and the join goes on (228 packages) |
| No install, stock packages | the client's "install" is an empty folder. 202 stock packages were skipped at once with the warning, and only the 3 custom packages were downloaded; JOIN after under 1 s. Live, on a 3374 ONS server (ONS-Torlan, file channel only) without the install: 137 stock packages skipped, no download requests, JOIN after 9 s (map load), 78 actors, 0 bunch errors |
| Mod in the UT2004 folder, other version | the install's `3SPNv3141.u` replaced by another package (not in `Manifest.ini`): it is downloaded, and its classes decode (24 actor opens, 0 bunch errors) |
| Stock version mismatch | the install's `XGame.u` replaced by another package: the join is cancelled with "Version mismatch: the server's XGame is another version ..." |
| Time limit | stock server rate (10000), `-UE2NetDownloadTimeLimit=120`: cancelled after 15 s with "49.4 MB left at 7.2 KB/s"; the join does not go on |
| Large map over the redirect | a 114 MB map (DM-CBP2-Griffin padded with 80 MB of random data, a 96 MB `.uz2`) while the server announced the original 30 MB file with the same GUID: used with the size warning, converted, loaded and joined, 0 bunch errors |
| Map names with "][" and other characters | local replica of a VCTF server on `VCTF-Arcade][Blocktastic]-Rev2`: the map converts, `ArenaRuntimeMap::GetMapDir` finds its folder (the converter's file-safe name, `maps/VCTF-Arcade__Blocktastic_-Rev2`), it loads and renders, and the join decodes 214 actors with 0 bunch errors. Download names may use any printable ASCII except `/ \ : * ? " < > |` (`{V][M}`, `(v4Fix)`, `R!P`, spaces) |
| Live, ProTheory (WS3SPN_V6 TAM + WSUTComp_V6) | 11 missing packages, 61.8 MB. The redirect's `DM-Rankin-NE2` has another GUID (stale) and is rejected, so the map came over the file channel at 21 KB/s (the server's cap) in 20 min. The other 10 came from the redirect in seconds; `itsfake.u` and `AntiTCC_2026r4.u` 404 there and came over the channel. All GUIDs match. The map converted in 1.0 s (0 errors) and loaded, and the join reached JOIN. Over 3 min spectating: `WS3SPN_V6` (`Misc_Player`, `Misc_PRI`, `TAM_GRI`, `TAM_TeamInfo`, `TAM_Mutator`, `TAM_Warmup`, pickups...) and `WSUTComp_V6` (`UTComp_PRI`, `NewNet_PRI`, `UTComp_ServerReplicationInfo`, `TimeStamp_Pawn`...) decode, 2880 properties and 4023 calls, 0 bunch errors. The old 10 min limit had cancelled the first try; the limit is now 30 min |
| Net cache reuse | rejoining with the filled net cache: 0 missing, conversion "1 unchanged" in 0.0 s, map loaded |
| Step 3, DM-Rankin, 4 bots | 95 s spectating: 520 actors, 0 bunch errors; bots shown with their own characters and skins, running and shooting; the followed bot changes on fire, alt fire gives the free camera and back; 269 projectiles flown (average 39 m); shown vs received pawn position at each update: average 20 cm, max 2.2 m; 63 of 67 pickups matched (the rest: dropped weapons); 8 kill lines. The scores and deaths the client logged at 70 s and 90 s (`-UE2NetSpectateScript` `scores`) equal the server's (`TestBots scores` log lines); the 8 kill lines equal the 8 deaths on the server in that time |
| Step 3, CTF-Face3, 6 bots | 120 s: 649 actors, 0 bunch errors; flags carried by their holders, dropped, returned; CTF messages and announcer; team skins; scoreboard with names, scores, team scores (0 - 1), score limit and remaining time; client scores at 105 s and 115 s equal the server's |
| Step 3, DOM-Access, 6 bots | 80 s: 0 bunch errors; the HUD's A / B points follow `ControllingTeam` (white, blue, red, blue), team scores |
| Step 3, ONS-Torlan, 8 bots | 90 s before the `Quat` fix: 4300 struct read errors on vehicle state structs. After it: 1171 actors, 23697 properties, **0 bunch errors**; vehicles with their meshes, orientation from the quaternion, drivers inside them (camera behind the vehicle), 798 projectiles. One live join (3374 ONS server, spectator, 150 s): 0 bunch errors, the parked vehicles' states decode to unit quaternions and neutral inputs |
| Step 3, FPS (MangoHud, 1280x720 window, editor binary, other agents' work on the same GPU) | DM-Rankin 69.7 average, CTF-Face3 74.6, ONS-Torlan 79.3 |
| Step 3 game objects, ONS-Torlan, 10 bots | 95 s: 1016 actors, 26755 properties, 0 bunch errors; all 7 cores / nodes matched by map actor name, their stage / team / health follow the server (logged per change: nodes constructing, built, destroyed and rebuilt; the minimap and core health 100 / 89 follow); vehicles with their art (rig types) and other meshes, 1104 turret aim updates, 470 weapon flashes, 13 seated drivers / gunners (Manta riders, turret gunners), 6 wrecks; ONSOnslaughtMessage texts and colours from the class's own `GetString` / `GetColor` ("Red Team PowerNode is under Attack!" ...) |
| Step 3 game objects, BR-Colossus, 8 bots | 80 s: the ball carried by its holders, thrown, dropped (loose: `FullBomb` at 1.5), as the HUD's ball icon; `xBombMessage` from `GetString` ("Motig has the ball!", "... dropped the ball!") |
| Step 3 game objects, AS-Convoy, 8 bots | 100 s: 5 of the server's 8 objectives matched (the other 3 are left out by our rules), active / completed / critical from the server, the round time pod counting `RoundTimeLimit` down |
| Step 3 regression, CTF-Face3, 6 bots | 75 s: 0 bunch errors, flags, CTF messages, 161 of 161 pickups matched |
| Step 3 game objects, live 3374 ONS server (spectator, 150 s, nobody playing) | 0 bunch errors; 7 / 7 nodes matched (neutral nodes, active cores at full health), 13 turret seats and 9 turret guns |
| Step 3 follow-up, ONS-Torlan, 10 bots | turret aim check (settled guns within their limits, 67 checks in 90 s): the barrel is **2.7 deg** off the server's aim point on average (Scorpion 1.9 .. 2.6, Manta 0 .. 0.5, Hellbender rear gun, turret 2.9 .. 3.5; max 28 on a moving vehicle). Before the per-class limits and the shared yaw / pitch bone the Manta's guns were 22 deg off |
| Step 3 follow-up, ONS-Adara, 10 bots | SPMA (ONSArtillery) 6 of 6 and Paladin (ONSShockTank) 8 of 8 wheels found from the class's wheel subobjects; screenshots show their wheels on the ground. Riders with the 2004 characters' own `Vehicle_Driving` |
| Step 3 follow-up, AS-Convoy / AS-Glacier, 8 bots | all **8 of 8** (Convoy) and **13 of 13** (Glacier) server objectives matched; Convoy: the Lock Panel and the bonus Gate Terminal current, then the Bridge Controls after the panel was completed; Glacier: the two "Operate" objectives of one priority current at once, completed in turn; the bot-only HoldObjective0 inactive |
| Step 3 game objects, FPS (MangoHud, same setup, heavier GPU sharing than the row above) | ONS-Torlan 24.0 average, BR-Colossus 19.7, AS-Convoy 17.4, CTF-Face3 22.8 (CTF has no vehicles: the drop from the earlier row is the shared GPU) |

Open problems:

- ClientValidate response not seen live (no server asked); its format is read from the 3374 client. The 3369
  server triggers it natively (`PlayerController.ClientValidate` is a native event); no local server setting that
  sends it was found, so it is still untested.
- Mod HUDs, scoreboards, their message lists and mods' client code run in the script VM ("Mod HUDs and scoreboards",
  "Mods' client code"). Not done: mods' in-game menus (UTComp's settings menu: UE2's GUI is native C++ in the original
  engine, so a script GUI runner needs the GUIController / GUIComponent drawing and input natives; the settings come
  read-only from the player's ini, as for the hit sounds), their client mutators' `Tick`, interactions other than the
  ones a controller property holds, first-person weapons with no stock relative (Ballistic Weapons: their whole
  weapon is script), fire timing a mod changes during play (the fire mode objects' code is not run), the effects of
  script that changes an emitter's properties while it plays (the Odin's charge beam lengths), `Canvas.DrawActor`
  overlays (sniper zoom in script). Player characters that exist only in a server's packages: UE2 finds a character
  by its `.upl` record, which is never downloaded, so a stock client shows the default model as we do.
- The real client reference (see "Real client reference") covers the handshake, moves and NewNet fire. Not compared
  yet: vehicles (`ServerDrive`), spectator moves in the free camera, and the 3374 client against a 3374 server.
- Downloads: no size limit or eviction for the net cache yet.
- The live 3374 server's answers to file requests are only seen through refusals from a build without the index
  rotation fix. With the fix, no live request was made: every missing package there was stock. Locally, 3369 refusals
  of 3 requests in a row on channels 1, 2 and 3 take 0.1 s in total.
- Stock packages come from the installer's `Manifest.ini`. Without an install there is no manifest: the content cache's
  folder rule then also counts mods that were in the UT2004 folder at conversion time as stock. The content cache does
  not copy `Manifest.ini` yet (`ArenaConvScripts` could).
- Packages added by the OldUnreal 3374 patch are not in the retail manifest. A server's version of one, if missing
  locally, is requested like a mod package; the server refuses it in about 0.1 s.
- Resources used only by mod classes are converted on demand (see "Mod client-side code"): sounds, textures,
  materials and fonts. Static meshes and animations of mod packages are not.
- Only one converted copy per package name: a downloaded package with the name of one in the custom content folder
  uses the custom content's conversion. Two servers with different versions of the same package name re-convert it
  when the other one is joined.
- Converting a map also re-exports the stock material json files it uses into the content cache (the exporter
  rewrites them; the output is the same for the same converter version). The custom content scan does the same.
- **Map identity**: the converter writes the map package's GUID into `map.json` (`package_guid`). At `WELCOME` the
  client compares it with the server's map GUID (caches from before that field: the GUID of the install's map of
  that name). Another map with the same name is not shown: the join fails and says so ("The server's DM-Rankin is
  another version than the converted one (GUID ..., ours ...)"; during a map change the loading screen says it). Checked with a local
  server whose `DM-Rankin.ut2` was another map. A server's map is never replaced by another layout either: before,
  a VCTF map (prefix "VCTF", not "CTF") failed our CTF rules' check and the client built CTF-Face3 in its place, so the
  pawn walked in the wrong level (5360 corrections in 180 s on the replica server); VCTF maps now count as CTF maps,
  and `?UE2Net=1` worlds always use the server's map.
- Not tested against real UT2004 redirect servers (only the local `http.server`).
- `CompressedPosition`'s first byte stayed 0 in step 3 as well (the pitch comes with `ViewPitch`); the yaw byte is
  right (bots face where they run).
- Generic struct serialization is checked with `AnimRep`, `HitFXData` and the ONS vehicle states (with the `Quat`
  rule). Assault vehicles and turrets were not checked. A function call named `Server...` arriving from the server
  marks a misread bunch: the rest of the bunch is dropped instead of acting on garbage.
- Step 3 gaps: (the free camera's position now steers the server's spectator through `ServerMove`, see "Step 4".)
  Alt fire back from the free camera follows the next player,
  not the previous one (UE2 has no "view this player" call). Turret aim is checked against the server's aim point (see the aim check row). Wheel
  suspension is local (ground traces), not the server's. The hold / use progress bar is counted locally while the
  server says an objective is held. Mod message classes run through the VM as far as it runs their `GetString`; a class
  whose `GetString` returns nothing stays counted as not shown.
- The order of HAVE / SKIP relative to JOIN is settled by the 3374 client's capture (all before `JOIN`, see
  "Control channel"). The file channel request is confirmed by working downloads from the 3369 server.
- While loading a map the game thread does not tick the connection. The server waits for `JOIN`, so this is fine for
  the 2 s loads seen; a very slow load could hit the 20 s timeout of map-change reconnects.

## Hosting: the UE2 server

Host Game (LISTEN and DEDICATED) hosts the match with OpenUT2k4's own server for the UE2 protocol (`Net/UE2Server*`).
Real UT2004 clients (retail 3369 and OldUnreal 3374) and OpenUT2k4 clients, which use the UE2 client above, join it
the same way. UE5's net driver is not used. The server is the mirror image of the client: the same packets, bunches,
channels and package map, with the roles reversed.

### Starting it

| Option | Effect |
|---|---|
| `?UE2Host=1` | `AArenaGameSession` starts `FUE2Server` for the match (its `BeginPlay`) and stops it with the match |
| `?UE2Port=<n>` | UDP game port (default 7777). Server queries are answered on n + 1, the LAN broadcast on 10777 |
| `?UE2Dedicated=1` | no local player of our own: the listen flag in queries is off |
| `?ServerName= ?GamePassword= ?MaxPlayers= ?MaxSpectators= ?LANServer=` | as before (`ArenaHost::BuildServerOptions`) |
| `?Advertise=1` | the master server uplink, only when the option is given (Host Game: "Show in Server Browsers", off by default) |
| `-UE2ServerTrace` | logs every control line, opened / closed channel and function call (`UE2ServerTrace:`) |
| `-UE2ServerBind=<ip>` | binds the game port to one address (default: all) |
| `-UE2NoUplink` | never starts the master server uplink, whatever the options say (tests) |
| `-UE2UplinkMaster=<host:port>` | the uplink talks to this master only (tests with a local stand-in; not in Shipping) |
| `?MapList=A,B,C` | the map rotation (else every stock map of the game type in the install) |
| `?ServerPackages=A,B,C` | replaces the install's `[Engine.GameEngine] ServerPackages` in the USES list (dedicated server ini) |
| `?ShortName= ?AdminName= ?AdminEmail= ?MOTD=<lines joined by \|>` | the GRI's server info (`MessageOfTheDay`) |
| `-UE2ServerTestDrive=<s>` | test hook (not in Shipping): after that time the remote players' pawns enter the nearest free vehicle (tried every second until one is in); with `-UE2ServerTestDriveSeats` only vehicles with a gunner seat |
| `-UE2ServerTestMessages=<s>` | test hook (not in Shipping): from then on, one message every 2 s to each remote player through the game's own path: head shot, multi kills 1..3, then sprees (its own, another player's, that spree ended by it) |
| `-UE2ServerTestGunner=<s>` | test hook (not in Shipping): a bot on foot takes the gunner seat of the free multi-seat vehicle nearest to the first remote player, again whenever it is left |

- **LISTEN** (Host Game page) opens the match with `?UE2Host=1?UE2Port=<HostPort>` and the server options; the local
  player plays in the hosted match.
- **DEDICATED** starts the dedicated server mode of the same executable (`-OpenUT2k4Server -nullrhi -nosound`,
  `Server/Docs/DedicatedServer.md`: no local player, admin console, `MinPlayers` bots) with a UT2004 command line built from
  the Host Game options (`<Map>?game=<class>?MaxPlayers=..?ServerName=..?GamePassword=..?MinPlayers=<bots>?GoalScore=
  ?TimeLimit=?GameDifficulty=?Mutator=?Translocator=?Instagib=`, `-port=<HostPort>`); its log is `OpenUT2k4Server.log`.
  The menu stays open.
- The map must be a UT2004 package the clients have: the converted stock maps (`Imported:<Map>`), or a custom map
  from the custom content folder. A procedural layout cannot be hosted for UT2004 clients (the server does not start,
  and the match runs offline: "Host: the UE2 server could not start").
- Server browsers: the query answers have no `ArenaHost::OpenServerFlag` any more, so OpenUT2k4's browser joins with the
  UE2 client, like any UT2004 server. The players query lists bots and local players too.

### Handshake (server side)

```
client: HELLO REVISION=0 MINVER=3180 VER=3374            (OldUnreal 3374; 3369 clients send VER=3369)
server: CHALLENGE VER=3369 CHALLENGE=<random> STATS=0 SEC=1 GZ=<10..99>   (UPGRADE MINVER=3369 VER=3369 for older clients)
client: AUTH HASH=... GM=... USERNAME= PASSWORD=          NETSPEED <n>     LOGIN RESPONSE=<n> URL=<options>
server: USES ... (one per package)   DLMGR CLASS=Engine.ChannelDownload PARAMS=Enabled COMPRESSION=0   WELCOME LEVEL=<map> GAME=<class> LONE=0
        or FAILCODE NEEDPW / WRONGPW / SERVERFULL / CHALLENGE, then FAILURE <text>, and the control channel closes
client: HAVE GUID=<guid> GEN=<n> (3374 clients send one for every package of theirs with more generations)
        PETE PKT=1 PKG=1, REPEAT <bytes>   (3374 additions: ignored)
        JOIN
```

- `RESPONSE` must be the engine's scramble of the challenge (see "Control channel"); a wrong one gets `FAILCODE
  CHALLENGE`.
- **AUTH**: the server keeps what the stock server makes of `HASH` for kick and ban lists: `MD5("1230asadkgjk358dmvmbjt6838320yjkdhnchjg4958" + HASH)`
  (the salt and format of `UNetConnection::SecureCDKeyHash`, read from the 3374 `Engine.so`). It is stored on the
  connection (`PlayerHashId`) and never logged.
- `NETSPEED` is kept (clamped to 1800..25000, 1000000 on LAN servers). Password and capacity are checked at `LOGIN`
  (`?Password=`, `MaxPlayers` for players and `MaxSpectators` for `?SpectatorOnly=1` logins; bots and the host's own
  players do not count). Tested: our client gets `NEEDPW` / `WRONGPW`, the 3374 client logs `FailCode SERVERFULL`.
- File channels (package downloads) are refused with an empty close: every package of the list is stock (or the
  player's own custom content).
- A packet from an unknown address that does not open the control channel (a refused client's last ack) is dropped
  instead of becoming a connection.

### Package list (USES)

The list is built once per match from the player's install, in the order of UE2's `UPackageMap::AddLinker`: the map
package, then each package it imports (depth first, a package before the ones it imports), then the install's
`[Engine.GameEngine] ServerPackages` (`UT2004.ini`, else `Default.ini`, else the retail list) with their imports, and
the game class's package. Packages flagged server-side only (0x4) are left out, as UE2 does. Each line has the file's
own GUID, package flags, file size and generation count:

```
USES GUID=D17879374594705756AB7D88C47FDF11 PKG=DM-Rankin FLAGS=33 SIZE=13400320 GEN=1 FNAME=DM-Rankin.ut2
USES GUID=17158B3B4FF9A24F520996A03FACE2BE PKG=Engine FLAGS=1 SIZE=2720630 GEN=62 FNAME=Engine.u
...
```

DM-Rankin gives 205 packages (the stock 3369.3 server: 204), CTF-Face3 210. The GUIDs are the ones the stock server
sends; texture packages have one generation in the retail install and two on the dedicated server build, and other
sizes (the server's are stripped), which clients do not mind (they match by GUID). OldUnreal 3374 clients have the
same GUIDs with more generations (Engine 83 against our 62) and answer with `HAVE`, so both sides number by our
generation. Each connection gets its own `FUE2PackageMap` from these lines, `HAVE` / `SKIP` applied at `JOIN`; class
field tables are per connection too, so a client with fewer generations of a package gets the numbering it expects.

### Replication

Every replicated actor is an `FUE2SvActor`: class (or map actor), spawn location and rotation, relevancy (always,
owner only, or by distance) and a bag of property values by key (`"Health"`, `"Teams[1]"`), each with a condition
(everyone, owner only, everyone but the owner, initial bunch only) and whether it is a movement value. `FUE2ServerWorld`
refreshes the bag from our native game every tick; each connection then sends what changed:

- **Shadows**: a channel keeps the encoded bits of every value the client has. A value goes out when its encoding
  differs. A new channel's shadow starts from the class defaults (the UScript loader's default objects), so the
  initial bunch carries only non-default values, as UE2's does.
- **Roles**: the stock server sends `Role` from the client's side (it was seen as `xPlayer.Role = 3` in captured
  traffic: AutonomousProxy, not the server's Authority). Our server does the same: `Role = 3` for the owner's
  PlayerController and pawn, the default (SimulatedProxy) otherwise; the shadow of `Role` starts from the class's
  default `RemoteRole`. `bNetOwner = True` goes to the owner's controller.
- **Reliability**: open bunches and ordinary values are reliable. Movement values (`PawnPosition`, `ViewPitch`,
  moving projectiles, the dropped ball) are unreliable; when their packet is lost (a nak) their shadow is dropped, so
  they go out again if they still differ. Unreliable updates wait until the open bunch is acknowledged (the client drops
  bunches for channels it has not opened). A channel with 48 reliable bunches in flight gets no new updates.
- **Relevancy** (UE2 `ServerTickClients`): always relevant actors (game info, PRIs, teams, pickups, flags,
  objectives), the owner's own actors, map actors, and the rest when the viewer sees them: a line of sight test
  against the level's static geometry from the viewer's eyes (the pawn, the followed player, the spectator) to the
  actor and a little above it, every 0.2 s, kept relevant 5 s after the last sight (`RELEVANT_TIMEOUT`). Attachments,
  link beams, shields and vehicle guns follow their pawn or vehicle (`RelevancyParent`). Channels of actors that stay
  out of sight close after those 5 s.
- **Bandwidth** (`NETSPEED`): each client has a byte budget refilled at its net speed (clamped to 1800 .. 25000, LAN
  servers up to 1000000), at most half a second ahead; everything queued for it (bunches, calls) is charged. Actors due
  for an update are sent in order of priority x time since their last update (UE2 `NetPriority`: pawns and the
  controller 3, projectiles 2.5, attachments 2, pickups 1.4, PRIs and the GRI 1; the owner's actors x2); once the
  budget is spent the rest wait for the next tick, except the client's own controller and pawn and the game info.
  The summary log counts the ticks that ran out (`saturated ticks`).
- **No partial bunches**: a long initial bunch is split into the open bunch and further reliable bunches (3600 bits
  each).
- **References**: an actor reference is written as its channel on that connection, None while the actor has no channel
  there (like UE2). Before a function call that names an actor (a PRI in a message, the pawn in `ClientRestart`), the
  actor's channel is opened first.
- `bNetTemporary` classes (rockets, flak, link plasma ...) are sent once and closed at once; the client flies them.
  Dead pawns get `bTearOff` (with `TearOffMomentum`, `TakeHitLocation`, `HitDamageType`) and their channel closes 2 s
  later; the client keeps the ragdoll. Either way the actor is never opened again for that client.
- Order: the PlayerController, the game info, level info, teams and PRIs first, then pawns, weapons and attachments,
  then the rest. On join, the GRI, level info, teams and PRIs are sent before the controller, its pawn and weapons
  (the 3374 client's `Weapon.PutDown` reads the GRI: without this it logged `Accessed None 'GRI'`).

| Our actor | Stock UT2004 actor | Values |
|---|---|---|
| the map | `<Map>.LevelInfo0` (map actor) | none (as the stock server) |
| `AArenaGameState` | `Engine.GameReplicationInfo` | `GameName`, `GameClass`, `bTeamGame`, `ServerName`, `ShortName`, `GoalScore`, `TimeLimit` (min), `RemainingTime`, `ElapsedTime`, `RemainingMinute` (once a minute), `bMatchHasBegun`, `bStopCountDown`, `Teams[]`, `Winner`, `FlagState[]` |
| team scores | `XGame.xTeamRoster` | `TeamIndex`, `Score`, `HomeBase` (the flag base) |
| `AArenaPlayerState` | `XGame.xPlayerReplicationInfo` | `PlayerName`, `Score`, `Deaths`, `CharacterName`, `bIsFemale` (the .upl record's `Sex`), `PlayerID`, `bBot`, `bNoTeam`, `Team`, `TeamID`, `bIsSpectator`, `bOnlySpectator`, `bOutOfLives`, `Ping`, `HasFlag`, `PlayerZone` / `PlayerVolume` (the pawn's zone and named volume as static map references: `GetLocationName` for team chat and the scoreboard) |
| a remote player | `XGame.xPlayer` (owner only) | `Role`, `bNetOwner`, `PlayerReplicationInfo`, `GameReplicationInfo`, `Pawn`, `Adrenaline` |
| `AArenaCharacter` | `XGame.xPawn` | `PawnPosition` (not owner; walking pawns 4.78 UU higher, where a UE2 pawn rests), `ViewPitch`, `Physics` (initial), `PlayerReplicationInfo`, `Health`, `ShieldStrength`, `bIsCrouched`, `bInvis`, `AnimAction` (weapon switch), owner: `Owner`, `Controller`, `Inventory`; death: `bTearOff`, `TearOffMomentum`, `TakeHitLocation`, `HitDamageType` |
| current weapon | its attachment (`XWeapons.RocketAttachment` ...) | `Instigator`, `Base`, `FlashCount` and `FiringMode` (each shot, from `AArenaWeapon::NetShotCount`), `mHitLocation` / `SpawnHitCount` (assault rifle, minigun) |
| instant hit shots | `XWeapons.ShockBeamEffect`, `SuperShockBeamEffect`, `NewLightningBolt` (short-lived) | spawned at the shot's start, `mSpawnVecA` (end), `HitNormal`, `Instigator` |
| `AArenaWeapon` (remote players' own) | the stock weapon (`XWeapons.RocketLauncher` ...), owner only | `Owner`, `Instigator`, `Inventory` (the next weapon), `AmmoCharge[0..1]`, `ThirdPersonActor` |
| `AArenaProjectile` | `XWeapons.RocketProj`, `FlakChunk`, `FlakShell`, `ShockProjectile`, `LinkProjectile`, `BioGlob`, `Grenade`, ONS projectiles | `Velocity` (initial for `bNetTemporary` classes, else with `Location` while it flies), `Instigator` |
| `AArenaPickup` | `XPickups.*` / `XWeapons.*Pickup` / `*AmmoPickup` (always relevant) | `bHidden` while taken |
| `AArenaMover` | the map's mover (map actor, its own class from the map package) | `SimOldPos`, `SimOldRot*`, `SimInterpolate` (alpha, rate, keys at the start of each move: `AArenaMover::GetNetMoveState`), `RealPosition` / `RealRotation` at rest |
| `AArenaFlag` | `XGame.xRedFlag` / `xBlueFlag`; the map's `xRedFlagBase0` / `xBlueFlagBase0` | flag: `Team`, `bHome`, `bHeld`, `HolderPRI`, `Base` + `AttachmentBone FlagHand` while carried, `Location` when down; base: `bHidden` while the flag is away |
| `AArenaDOM_ControlPoint` | the map's `xDomPointA` / `xDomPointB` | `ControllingTeam`, `bControllable` |
| `AArenaBR_Ball` | `XGame.xBombFlag` | as the flag; thrown or lying: `Location`, `Velocity`, `Physics` |
| `AArenaONS_PowerNode` | the map's `ONSPowerNode*` / `ONSPowerCore*` (`UE2SourceName`) | `CoreStage`, `DefenderTeamIndex`, `Health` (as a fraction of the class's `DamageCapacity`, what the HUD shows), `bPoweredByRed`, `bPoweredByBlue` |
| `AArenaONS_Vehicle` | `Onslaught.ONSHoverBike` (Wisp), `ONSRV` (Dustrunner), `ONSHoverTank` (Bulwark), `ONSAttackCraft` (Kestrel), `ONSPRV` (Caravan) | the class's KRigidBody state (`CarState` / `HoverState` / `TreadState` / `CopState`: `ChassisPosition`, `ChassisQuaternion`, `ChassisLinVel` x 10, the driver's controls as range bytes `ServerGas` / `ServerBrake` / `ServerSteering` / `ServerGear` or `ServerThrust` / `ServerStrafe` / `ServerTurn` / `ServerRise`, `ServerViewPitch` / `ServerViewYaw`). The position is the stock vehicle's origin, its mesh's origin: our actor location plus the art's `MeshOffset` (ArenaArtVehicles). Sent at our collision box centre, the clients' Karma chassis floated about 20 UU above the ground and the 3374 client showed "Daredevil!" stunt messages while standing still. `Team`, `bDriving`, `Driver`, `Health`, `bTeamLocked`, `bVehicleDestroyed` / `bDestroyAppearance` / `ExplosionCount`; owner: `Role` 3, `Owner`, `Controller`. Its guns: the class's default `DriverWeapons` (`ONSWeapon` actors on their `WeaponBone`), `CurrentHitLocation` (to everyone but the driver: our turret's aim, 5000 cm out) so the turret turns on the other clients. The Caravan's gunner seat: its `PassengerWeapons` pawn (`VehicleBase`, `Driver`, `Gun`) and that pawn's own gun (its `GunClass`, `ONSPRVSideGun`, on the vehicle's `WeaponBone`, as `ONSWeaponPawn.BeginPlay` / `AttachToVehicle`), with `CurrentHitLocation` from our rear turret's aim. Drivers: `xPawn.DrivenVehicle` |
| `AArenaINV_Monster` | `SkaarjPack.SkaarjPupae`, `RazorFly`, `Manta`, `Krall`, `EliteKrall`, `GasBag`, `Brute`, `Behemoth`, `Skaarj`, `IceSkaarj`, `FireSkaarj`, `Warlord` | as a pawn (`PawnPosition`, `Health`, death) |
| `AArenaAS_Objective` | the map's objective (`UE2SourceName`) | `bActive`, `bDisabled` (done), `DefenderTeamIndex`, `bIsCritical`, `Health` (destroyable ones) |
| Assault / Invasion game info | `UT2k4Assault.ASGameReplicationInfo` (`bTeamZeroIsAttacking`, `CurrentRound`, `MaxRounds`, `ObjectiveProgress`), `SkaarjPack.InvasionGameReplicationInfo` (`WaveNumber`, `BaseDifficulty`); LMS: GRI `MaxLives`; ONS players: `Onslaught.ONSPlayerReplicationInfo` | |
| link gun beam / shield | `XWeapons.LinkBeamEffect` while the beam fires (`StartEffect`, `EndEffect`, `bHitSomething`, `Links`, `LinkColor` 0 / team + 1 while linked to a team mate, `LinkedPawn`, `bLockedOn`); `LinkAttachment.Links` and `LinkColor` (`LC_Red` / `LC_Blue` while linking a team mate, `LC_Gold` when powered by links, `LC_Green`); `ShieldEffect3rd(RED/BLUE)` on the shield gun's attachment, shown while the shield is up | |

Weapon, attachment, pickup and damage type classes come from tables in `UE2ServerWorld.cpp` (our weapon class to the
stock names). Fire sounds come from the stock classes themselves: the weapon's `FireModeClass[mode]` default and its
`FireSound` / `TransientSoundVolume` / `TransientSoundRadius`.

Calls to the clients:

- on join: `ClientSetHUD` (the game's stock HUD and scoreboard), `ReceiveLocalizedMessage(Engine.GameMessage, 1)` ("entered
  the game") to everyone; on leave switch 4
- each new pawn: `ClientRestart(pawn)`, `ClientSetRotation`, then `ClientWeaponSet` once per weapon; `GivePawn` when the
  client asks (`AskForPawn`) or moves without having acknowledged its pawn
- `ClientGotoState('Dead' / 'Spectating' / 'GameEnded')` and `ClientGameEnded` on changes without a pawn
- `ReceiveLocalizedMessage`: kills (`XGame.xDeathMessage`: switch 0 killer / victim / damage type, switch 1 suicides with
  `Engine.Suicided`, `Engine.Fell` ...; `xKillerMessagePlus` to the killer, `xVictimMessage` to the victim), first blood
  (`UnrealGame.FirstBloodMessage`), sprees (`UnrealGame.KillingSpreeMessage`), multi kills (`XGame.MultiKillMessage`, to
  the killer), CTF (`UnrealGame.CTFMessage`: 0 captured, 1 returned, 2 dropped, 3 returned by itself, 4 taken)
- `TeamMessage(PRI, text, 'Say' / 'TeamSay')`: chat, relayed to everyone (team chat to the team; in a game without
  teams `ServerTeamSay` is plain chat, as `PlayerController.ServerTeamSay`). The listen host's own player types with
  T / R (see "In-game chat" below), its lines go out through `UE2ServerHooks::Chat`, and remote players' lines show
  in its console (`ArenaChat::ShowChat`)
- `ClientHearSound(None, id, sound, location, (volume * 1000, radius, pitch * 1000), false)`: other players' shots
  (PlayOwnedSound: not to the shooter)
- spectators: `ClientSetViewTarget` + `ClientSetBehindView`
- head shots (`XGame.SpecialKillMessage`), picked up items (the pickup's `MessageClass` with its class), the countdown
  (`PlayStartupMessage` 2..4, 5 when the match begins), the end of the match (`PlayStatusAnnouncement` of the game
  class's `EndGameSoundName[0]` to the winners, `[1]` to the others)
- sounds (`ClientHearSound`, from the stock classes' defaults): pickups taken (`PickupSound`), pain, death, landing and
  jumping (`PainSounds`, `DeathSounds`, `Sounds[EST_Land / EST_Jump]` of the character's own sound group: the species of
  its .upl record (`Species=xGame.SPECIES_Merc` ...), its `MaleSoundGroup` / `FemaleSoundGroup` default by the record's
  `Sex`, as `SpeciesType.Setup`: female voices, `xBotSoundGroup` for robots, Juggernauts, Skaarj ...); weapon loops as the
  pawn's `AmbientSound` (the fire mode's `FiringSound` / `ChargingSound`: minigun, shield gun charge)
- vehicles: `ClientRestart(vehicle)` and `Vehicle.ClientKDriverEnter(PC)` when a remote player gets in,
  `ClientKDriverLeave` when they get out
- match end: `ClientTravel(<NEXTMAP>, TRAVEL_Relative, false)` to everyone, then the next map loads (below)
- movement: `ClientAdjustPosition` / `ShortClientAdjustPosition` (below)

### Remote players and their input

A client that joins becomes `AUE2RemoteController` (an `AController` with a player state) in our game: it gets a name
(unique like `GameInfo.ChangeName`), its `?Character=`, a team (`?Team=` or the smaller team), and spawns through the
game mode's `RestartPlayer`, so scoring, kills, respawns, team games and bots treat it like any player. It leaves
with `Logout` when the connection closes.

Moves (`PlayerController.ServerMove`, `DualServerMove`, `ShortServerMove`, read in the formats of
`Net/UE2NetPlay.cpp` and the stock script):

- Moves older than the last one are dropped. A move before `ServerAcknowledgePossession` names the current pawn is
  not applied (the client is sent `GivePawn`), like UE2.
- `OldTimeDelta` / `OldAccel` (a lost move resent in compressed form) are run first.
- View: pitch and yaw from `View` (15 bits each, halved): the controller's rotation, the pawn's yaw.
- Each move is one physics step of the pawn (UE2 `MoveAutonomous`): `UArenaCharacterMovement::SimulateNetStep` with the
  step's inputs (`AArenaCharacter::StepInputOverride`: run, duck, jump pressed when `bJumpStatus` toggles, double jump,
  dodge direction) and `InAccel / 10` as the acceleration. The remote pawn's movement component does not tick by itself.
- Then the client's `ClientLoc` is compared with our pawn (a walking UE2 pawn rests 4.78 UU higher than ours above a
  floor: added to ours). Within 48 UU the client's position is taken as it is (UE2 itself corrects anything above
  sqrt(3) UU, and our movement, though following UE2's rules, is not bit-identical), except when walking with only a
  small height difference (our pawn stays on its floor). Further away, `ClientAdjustPosition` (or
  `ShortClientAdjustPosition` when our velocity is zero) is sent at the end of the tick, at most one per player and
  tick, unreliable, as UE2's `SendClientAdjustment`.
- UE2 also confirms the position every 0.3 s. Our server does not confirm a move it took from the client: a
  confirmation carries our velocity, and the client corrects itself whenever velocities differ by more than 3 UU/s.
  The client keeps at most 100 saved moves (`PlayerController.GetFreeMove`) without confirmations. Dead and
  spectating players get a `ShortClientAdjustPosition` every 0.3 s (their state and view point).

Other calls:

| Client call | Effect |
|---|---|
| `Weapon.ServerStartFire(mode)` / `ServerStopFire(mode)` | the pawn fires its weapon (switching to it first if needed) |
| `xPawn.ServerChangedWeapon(old, new)` | weapon switch |
| `ServerReStartPlayer` | respawn when dead (`AArenaGameMode::RequestRespawn`) |
| `ServerUse`, `Suicide`, `ServerChangeTeam(n)` (team games: switches and dies, like UT2004) | as in our game |
| `ServerSay` / `ServerTeamSay` | chat |
| `xPlayer.ServerDoCombo(class)` | speed / berserk / booster / invisibility combo |
| `ServerViewNextPlayer`, `ServerViewSelf`, `ServerVerifyViewTarget` | spectators: follow the next player, free camera |
| `ServerUpdatePing`, `ChangeName`, `AskForPawn`, `ServerAcknowledgePossession` | as UE2 |
| `PlayerController.ServerDrive(forward, strafe, up, jump, view)` | the driven vehicle's controls (UE2 `ProcessDrive`: / 5000) and aim (`GArenaONSExternalInput` in `ArenaONS_Vehicle.h`) |
| `Vehicle.ServerChangeDriverPosition(n)` (number keys while driving) | seat change: 1 the driver's seat, 2 the gunner seat (`LeaveSeat` + `EnterSeat`, when the seat is free) |
| `Vehicle.VehicleFire(alt)` / `VehicleCeaseFire(alt)` | the vehicle's (or gunner seat's) guns |
| `ServerUse` in a vehicle | gets out |

### Match end and map rotation

When the match ends every remote player goes to `GameEnded` (`ClientGotoState` + `ClientGameEnded`, with or without a
pawn, as `PlayerController.GameHasEnded`), gets `ClientSetViewTarget(winner's pawn)` + `ClientSetBehindView(true)`
(`DeathMatch.CheckEndGame`; that pawn is then always relevant) and the game class's end announcement. Every pawn
stops where it is (`AArenaGameMode::EndMatch`: velocity 0 and no movement, as `Pawn.TurnOff`), bots included. The
stock client does not open the scoreboard by itself in `GameEnded` (only its `Dead` state does, once the view target
stops): F1 (ShowScores) shows it, with the final scores. Monster deaths
send no death message (`GameInfo.Killed`: only `bIsPlayer` victims); Invasion's death messages use
`SkaarjPack.InvasionDeathMessage` ("a monster").

At the end of the match (20 s after it ended, as before) a hosting game mode asks `UE2ServerHooks::TravelToNextMap`:
the next map of the rotation (`?MapList=`, else every stock map of the game type in the install, in name order;
`TravelToMap` goes to a given one), the same options with that map, `ClientTravel(<MAP>, 2, false)` to every client
(the stock server's form: map name upper case, `TRAVEL_Relative`) and `OpenLevel`. The server stops with the old world
and starts with the new one on the same port; clients reconnect and get the new `WELCOME`. Tested: DM-Rankin, then
DM-Rrajigar, then DM-Rustatorium, with the 3374 client and ours following both changes.

### Kick and ban (host admin)

The pause menu of a hosting game has an **Admin** tab (UT2004's `UT2K4Tab_AdminPlayerList` idea): the connected
UE2 players with their ping and the start of their player id, **Kick** and **Ban** for the selected one, and the ban
list with **Unban**. A ban keeps the player id from AUTH (the stock server's salted CD key hash), so it holds across
names; a banned player gets `FAILCODE LOCALBAN` ("You are banned from this server.") at login. Bans are saved in
`UArenaUserSettings::HostBannedPlayers` ("id|name"); a dedicated server keeps its own (`ArenaHost::SetUE2BanStore`).
API: `ArenaHost::GetUE2Players / KickUE2Player / GetUE2BanList / UnbanUE2 / UE2ServerSay`. Test hook in a match's
`-AutoMenuScript`: `AdminKick=<name>`, `AdminBan=<name>`. Tested: our client kicked ("You have been kicked from this
server."), the 3374 client banned and then refused (its log: `FailCode LOCALBAN`, the stock "Your IP address has been
banned on this server." dialog).

Pings come from the server's own measure (the time from a packet to its ack, smoothed), since UE2 clients report
theirs only after position corrections, which a trusted client rarely gets.

Game events reach the server through `UE2ServerHooks` (called from `AArenaGameMode::Killed` and the flag events).

### Master server uplink

`FUE2MasterUplink` (`Net/UE2ServerUplink.*`) registers the server with the OldUnreal master servers
(`[IpDrv.MasterServerLink] MasterServerList` of the 3374 patch, `ArenaServerBrowser::GetMasterServers`). It is created
only with `?Advertise=1` (Host Game "Show in Server Browsers", `UArenaUserSettings::bHostAdvertise`, now off by default)
and never with `-UE2NoUplink`. The protocol was read from the 3374 `IpDrv.so` (`FMasterServerUplinkLink`; the enum
names from `IpDrv.u`):

- TCP to the master (28902), every message framed as `[int32 length][payload]` in UE2 archive encoding. The master
  speaks first: a challenge string.
- The server answers: CD key hash, `MD5(key + challenge)`, `"UT2K4SERVER"`, int version (3369), byte platform (8 on
  Linux), language `"int"`, int match id (-1), byte bIgnoreUTANBans. The key is the player's CD key or, without one, the
  same per-install `NetIdentity` the client uses (`UE2NetCDKey`).
- `"APPROVED"`: the server sends int behind-NAT (0) and int GameSpy link (0). `DENIED` / `UPGRADE` end the uplink for
  the match; anything else retries after 30 s with the next master of the list.
- The master asks for UDP heartbeats (`byte 0, byte type, int code`); each is answered from the matching socket
  (type 0: the query port, 1: the game port) with `80 00 00 00, type, code` to the master's address at its TCP port.
  `byte 1, int period, 3 dwords` ends that phase.
- Then the server sends `STM_MD5Version` (`byte 4, int -1`) and `STM_GameState` (`byte 1`, the connected clients'
  addresses, the server line: ports, name, map, game type, players, max players, flags, rules, players) every 60 s.
- Master commands: client CD key challenges (`ClientValidate` to that client; its `ServerValidationResponse` goes
  back as `STM_ClientResponse` with its AUTH hash), kicks (auth failed, banned, duplicate key, modified client),
  match id, `CheckOption` (answered with an empty value). `UpdateOption` (a master changing the server's ini) and MD5
  updates are ignored.

Test (local only): `-UE2UplinkMaster=127.0.0.1:<port>` (not in Shipping) replaces the master list with a stand-in
master, a small Python script on 127.0.0.1 that plays the master's side. The server logged in (32-digit hash and
response, `UT2K4SERVER`, 3369, platform 8), answered both heartbeat requests from UDP 7787 (game) and 7788 (query),
sent its MD5 version, a 258-byte game state (name, map, `xDeathMatch`, rules) and answered `CheckOption`.

The uplink keeps one link per master group (the masters of a group share their lists). One real test, approved by the
user: a dedicated, password-protected DM-Rankin server without bots named "OpenUT2k4 uplink test - do not join", listed
for 10 minutes. Both groups accepted it (`ut2004master.333networks.com` and `utmaster.openspy.net`: challenge,
`APPROVED`, heartbeat phase with a 120 s period and no UDP heartbeat requests, game state sent), and a list query found
it on both (333networks: "75.166.170.232 7787 7788 OpenUT2k4 uplink test - d DM-Rankin", openspy: the full name). No
query came back from the internet to the query port in those 10 minutes, and 333networks' web list (which shows the
servers its query daemon reaches) did not show it: the router's NAT most likely drops incoming UDP to the game and
query ports. Nothing was changed on the router or the system. After the stop, openspy dropped the server at once,
333networks after its own timeout.

### In-game chat (ArenaChat.h)

Our game has UT2004's chat input for its local player (`Source/OpenUT2k4/ArenaChat.h/.cpp`), offline, hosting and on
UT2004 servers:

- **Keys**: T = `Talk`, R = `TeamTalk` (the stock `DefUser.ini` bindings), Y also opens team chat. They open the typing
  line with `Say ` / `TeamSay ` typed, as `Console.Talk` / `TeamTalk`; Enter sends it (a line that is not a Say /
  TeamSay runs as a console command, like the stock console), Escape clears the line and then closes it, Up / Down
  step through the last 16 lines, Left / Right / Home / End / Backspace / Delete edit. While it is open an invisible
  widget holds keyboard focus (characters with the keyboard layout and IME), the game gets no keys; keys held when
  it opened are released (`FlushPressedKeys`). The pause menu closes it.
- **Look**: `HUD.DrawTypingPrompt`: `(> Say hello_` in the console font and `HUD.ConsoleColor` (153, 216, 253), one
  line under the console messages (`AArenaHUD::DrawKillFeed`).
- **Console**: `say <text>` / `teamsay <text>` (exec functions of `AArenaPlayerController`), also `Talk` / `TeamTalk`.
- **Routing** (`ArenaChat::Send`, 128 characters, colour codes stripped as `GameInfo.StripColor`): on a UT2004 server
  `FUE2NetPlay::Say` (`ServerSay` / `ServerTeamSay`; the server's `TeamMessage` echo shows it); hosting UT2004
  clients `UE2ServerHooks::Chat` (relayed as `TeamMessage`) and the local console; offline the local console only
  (bots do not read chat). Team chat in a game without teams is plain chat.
- **Display** (`ArenaChat::ShowChat`, `HudBase.Message`): `Name: text` for Say, `Name(location): text` for TeamSay
  (`PlayerReplicationInfo.GetLocationName`: volume / zone name, else Dead / Spectating); colours as the 3374 client
  shows them: Say yellow (255, 255, 0) without teams, else the speaker's team colour (red 255, 64, 64 / blue 64,
  192, 255); TeamSay light green (128, 255, 128).
- **Tests never chat on public servers**: with `-saveddirsuffix=`, `AutoMenuScript=` or `-unattended` on the command
  line, `Send` refuses a UT2004 server that is not on this machine or a private network (and netplay's
  `-UE2NetPlayScript` `say` only works on local servers).
- **Test hooks** (`-AutoMenuScript`, in a match): `Talk`, `TeamTalk`, `ChatType=<text>` (`_` = space), `ChatKey=<key>`.

Checked: the host's typing line on screen, its lines reaching the 3374 client and our client, their lines reaching
the host. Not checked: typing with a real keyboard (all runs feed the same handlers through the test hooks; our game
cannot run on the test display), so whether the viewport keeps the mouse captured while the line is open is unverified.

### Tests

All local (127.0.0.1, no uplink). Real client: the OldUnreal 3374 Linux client (`OldUnreal-UT2004Patch3374-Linux`,
preview 23) on a copy of the install's `System/` with the patch's `System/` over it, the install's other folders
linked read-only, its home folder (`~/.ut2004`) redirected to a scratch folder, on its own `Xvfb` display with SDL
forced to X11 (SDL3 otherwise picks the desktop's Wayland session even under `xvfb-run`), joined by command line
(`ut2004-bin-amd64 127.0.0.1:<port>`). It gets no input: it stands where it spawned.

| Check | Result |
|---|---|
| 3374 client, DM-Rankin, 3 bots | joins (HELLO VER=3374, 205 USES, 28 HAVE, JOIN), loads the map, HUD (health, weapon bar, ammo), assault rifle and shield gun in hand, the bots' kill lines in its console, takes damage, dies (ragdoll, death camera on its corpse) and respawns. Its log: no script warnings once the GRI goes out before its weapons. 2325 moves in 70 s, 0 corrections, 0 bunch errors |
| 3374 client, CTF-Face3, 4 bots | CTF HUD with both flags' states, "The enemy has your flag, recover it!" (CTFMessage), kill lines; 0 script warnings |
| 3374 client, DOM-Access / BR-Anubis | in the match, 0 script warnings (DOM: 2873 moves, 0 corrections) |
| our client, DM-Rankin, playing (netplay's `-UE2NetPlayScript` `route=pickups`, `fire`, `hunt=1`) | 102 s: 3662 moves, 1 correction (52 UU), 4 spawns, 3 deaths, 16 fire commands, weapon pickups and switches, score 2; 0 bunch errors |
| our client, CTF-Face3, `route=flag` | 120 s: 2631 moves, 4 corrections, flags and bases shown, deaths and respawns, 0 bunch errors |
| our client, spectator (`?SpectatorOnly=1`, `-UE2NetSpectateScript`) | follows the next player (behind view), free camera, team scores, a remote player's chat line |
| chat / team | `say`, `teamsay` relayed (`UE2Server chat:`), `team=1` switches team and suicides |
| Host Game LISTEN from the menu (`HostStart=1`) | the server starts; our client's LAN tab lists "Menu Host DM-Rankin" and `BrowserJoin` joins through the UE2 client |
| Host Game DEDICATED (`HostStart=2`) | a headless second process hosts CTF-Face3; the 3374 client joins it |
| refusals | `?GamePassword=`: our client gets "This server needs a password." / "Wrong password."; `MaxPlayers=1`: the 3374 client logs `FailCode SERVERFULL` |
| 3374 client and our client together, DM-Rankin (final build) | 3374 client: 3511 moves, 0 corrections, 0 script warnings; ours: shoots and is shot, 0 corrections, beams arrive (39 beam actors) |
| 3374 client, ONS-Torlan | the Onslaught HUD with both cores and the radar map, link gun; 0 script warnings (vehicles are not replicated) |
| master server uplink | local stand-in, then one real test with both master groups (above) |
| relevancy / NETSPEED | DM-Rankin with both clients: no saturated ticks at NETSPEED 20000, 0 bunch errors; pawns out of sight close after 5 s |
| map rotation | DM-Rankin, DM-Rrajigar, DM-Rustatorium (`Time=1`): both clients followed each `ClientTravel` |
| kick / ban | Admin tab: our client kicked, the 3374 client banned and refused at its next join (`FailCode LOCALBAN`) |
| ONS vehicle | 3374 client put in a Kestrel (`ONSAttackCraft`) with `-UE2ServerTestDrive=60`: `ClientKDriverEnter`, behind view of the Raptor with its HUD, 2587 `ServerDrive` calls, 0 script warnings |
| ONS driving (round 3) | 3374 client, ONS-Torlan, no bots, `-UE2ServerTestDrive=50 -UE2ServerTestDriveSeats`, keys and mouse on its own Xvfb display: put in a Caravan (Hellbender), W / A / D drive and steer it (server speed up to 1600 cm/s; dust from the wheels on the client), mouse aim, `VehicleFire`, key 2 moves it to the gunner seat and key 1 back (`ServerChangeDriverPosition`, both ways); 0 script warnings |
| Assault (round 3) | 3374 client, AS-Convoy, 4 bots: objectives on the HUD ("Defend the boarding platform", "Defend the door"), spawns and plays; only the map's own two script warnings (HoldObjective without MoverTag / Event, stock map data). Our client: in the match, 0 bunch errors |
| Invasion (round 3) | 3374 client and our client, DM-Rankin, 2 bots: waves 1 and 2, monsters on the radar and in view, deaths, "The swarm overran the team" end; 0 script warnings |
| Last Man Standing (round 3) | 3374 client and our client, DM-Rankin, 3 bots: lives on the HUD, out of lives -> spectating, the end view stays on the winner (Dace), then map rotation to DM-Rrajigar with both clients; 0 script warnings |
| messages on screen (round 3) | 3374 client: "Head Shot!!", "Double Kill!", "Multi Kill!", "Mega Kill!" (`-UE2ServerTestMessages`), "Korrin drew first blood!", "The match has begun!" after a map change, "Player2 entered the game", "You picked up 10 Flak Shells." / "You picked up lightning ammo.", "You were killed by Vexa!", kill lines; end of match: behind view of the winner, then the next map |
| gunner seat (round 3) | 3374 client driving a Caravan, a bot in its gunner seat (`-UE2ServerTestGunner`): `ONSPRVSideGun` opens with the seat pawn, the gunner sits at it and its barrels turn between frames on the driver's client; 0 script warnings |
| sprees (round 3) | 3374 client: "Killing Spree!" (its own), "Player is on a rampage!", "Player's killing spree ended by Player2" |
| end of match (round 3) | 3374 client: behind view of the winner (Korrin), who stands still over 8 s of frames; F1 shows the stock scoreboard (DeathMatch in Rankin, final scores). Ours offline (DM, `Time=1`): our UT2004 scoreboard with "Dace wins the match!" / "You've lost the match." comes up by itself |
| chat (round 3) | the 3374 client's own T / R lines go through (`ServerSay` / `ServerTeamSay`), the host's typed lines reach it ("Player: host public"); our client's `say` / `teamsay` reach the host, the host's line reaches our client; the host shows Player2's line in the blue team colour |
| Host Game DEDICATED (server mode) | `-OpenUT2k4Server` started from the menu (`HostStart=2`): "Dedicated Host" DM-Rankin, MinPlayers 7; the 3374 client joined, 0 script warnings |

Gaps:

- ONS: driving was checked with the 3374 client (our client's driving is netplay's); flying (Raptor) was entered but
  not flown in these runs. Our Caravan creeps about 11 cm/s at rest on Torlan's base slope (vehicle
  physics, not the net code).
- Sounds: ambient map sounds are the clients' own (as in UE2); projectile explosions are simulated by the clients
  (as in UE2). The shield's hit flashes are not sent.
- The dedicated server's `say` uses `ArenaHost::UE2ServerSay`.
- The master server query-back was not seen (likely NAT, see above).

## Test server

`Scripts/ut2004-test-server.sh` runs a stock UT2004 dedicated server locally for testing the server browser
(`Source/OpenUT2k4/Net/ArenaServerBrowser*`) and the UE2 client. It is test tooling only: no UT2004 files are
shipped or committed, and the server install is never written to.

```
Scripts/ut2004-test-server.sh [-m Map] [-g GameClass] [-p Port] [-b Bots] [-n Name] [-o "?Extra=Options"] \
    [-r ServerRoot] [-c ClientRoot] [-w] [-f] <scratch dir>
```

- **Server root** (`-r`, `$UT2004_SERVER_ROOT`): a UT2004 dedicated server, read only. The default is the native
  Linux 3369.3 server with the Bonus Pack, extracted from the user's archive into the session scratchpad
  (`System/ucc-bin-linux-amd64`). A Windows install with `System/UCC.exe` works as a fallback under Wine
  (`-w` forces Wine).
- **Scratch dir**: one per server instance. The script copies `System/` there (about 100 MB, so the inis and
  logs are written to the copy), symlinks every other folder of the root (Maps, Textures, ...) and writes
  `server.pid`, `server.stdout` and `System/server.log`. The native 3369 binary buffers its log and writes
  `server.log` when it exits.
- **LAN only**: bound to 127.0.0.1 (`-multihome=127.0.0.1`). The ini copy has `DoUplink`, `UplinkToGamespy` and
  `SendStats` off (`[IpDrv.MasterServerUplink]`), and the web admin is disabled. The server never contacts a
  master server.
- **Ports**: the game port is `-p` (default 7777, UDP). The query port is the game port + 1 (UDP), and the LAN
  beacon is UDP 10777: a `0x80 0 0 0 0` server info query sent there is answered from the query port.
- **Bots** (`-b N`): stock dedicated servers wait for a human before they start a match, add `MinPlayers` bots
  only for connected players, and leave bots out of the players query. `-b` adds `?MinPlayers=N` and our test
  mutator `OUTestBots.TestBots` (`Scripts/ut2004-test-server/TestBots.uc`). The mutator starts the match, keeps N bots
  in it and lists them in the players query, with ping 0. The server info reply still counts only humans
  (`CurrentPlayers` 0). `ucc make` does not run on Linux, so the mutator is compiled once per scratch dir with
  the Windows `UCC.exe` of a full UT2004 install (`-c`, `$UT2004_CLIENT_ROOT`, default the user's install) under
  Wine, in a temporary copy of its `System/`. The result is cached in `<scratch dir>/testbots/`.
- The server caches its query replies, so player changes show in queries after 30 to 60 seconds.
- **Stopping**: `kill "$(cat <scratch dir>/server.pid)"` stops only that server.
- **Scores**: with `-b`, the mutator also logs every player's score / deaths (and the team scores) whenever they
  change (`ScriptLog: TestBots scores Name=score/deaths ... teams=r-b`), to compare a client's view with the server's.
  With `-o "?TestAdrenaline=1"` it also gives human players 100 adrenaline every second while no combo runs
  (`TestBots adrenaline ...`), for combo tests.

Examples:

```
# DM-Rankin deathmatch with 4 bots on 127.0.0.1:7777 (query port 7778)
Scripts/ut2004-test-server.sh -b 4 /path/to/scratch/srv-dm
# Bombing Run on another port
Scripts/ut2004-test-server.sh -m BR-Anubis -g XGame.xBombingRun -p 7787 -b 6 /path/to/scratch/srv-br
# Capture the Flag with a short time limit, in the foreground
Scripts/ut2004-test-server.sh -m CTF-FaceClassic -g XGame.xCTFGame -o "?TimeLimit=5" -f /path/to/scratch/srv-ctf
```

The common game classes are `XGame.xDeathMatch`, `XGame.xTeamGame`, `XGame.xCTFGame`, `XGame.xBombingRun`,
`XGame.xDoubleDom`, `XGame.xLastManStandingGame`, `XGame.xMutantGame`, `SkaarjPack.Invasion`,
`Onslaught.ONSOnslaughtGame` and `UT2k4Assault.ASGameInfo`.

**Testing downloads**: the script copies the server root's `System/` for each run. To test downloads, use a server
root of your own with `-r`: symlinks to the server's folders, a copy of `System/`, and `Maps/` / `Textures/` folders
that also link the custom map and its packages. Then edit that copy's `UT2004.ini`:

- `ServerPackages=<mod>` in `[Engine.GameEngine]`; a mod game type's package is in USES anyway;
- `[IpDrv.HTTPDownload] RedirectToURL=http://127.0.0.1:<port>/`, with `UseCompression=True`;
- `[IpDrv.TcpNetDriver] AllowDownloads`, and `MaxInternetClientRate` for a file channel faster than 10 KB/s.

Make the `.uz2` files with the server's own `ucc-bin-linux-amd64 compress <file>` (it writes `<file>.uz2` next to it).
Serve them with `python3 -m http.server <port> --bind 127.0.0.1 --directory <dir>`. Point the client at an install
copy without those files (`-UT2004Dir=`), and at a test net cache (`-UE2NetCache=`) and content cache (`-ContentCache=`).

In OpenUT2k4, open the server browser on its LAN tab (`-AutoMenuPage=ServerBrowser -AutoBrowserTab=LAN`), or
use Open IP with `127.0.0.1:7777`.
