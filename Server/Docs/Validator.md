# OpenUT2k4 Validator

An **opt-in** tool for server admins. It gives OpenUT2k4 players a stable, unforgeable id (a fingerprint of a key their
game made on its own), so admins can ban, whitelist and put new keys on probation. It also runs conservative
server-side sanity checks. It is honest identification, not an anti-cheat bypass:

- OpenUT2k4 always tells every server what it is: its login URL carries `?OpenUT2k4=<version>` (stock servers ignore
  unknown options). It never claims to be the stock client, and it never runs, answers, emulates or imitates any
  third-party anti-cheat (AntiTCC, itsfake, ...). Servers that require such a tool will not admit OpenUT2k4.
- Players do nothing: no registration, no prompt, no account, no service. The key is made silently on first start.
- Nothing secret ships in any build. The validator proves **player identity** (the same player as before, and a key
  nobody else can use), **not build integrity**: a modified OpenUT2k4 identifies just as well. Cheat detection is the
  server-side checks and the admins' bans.
- Real UT2004 clients are never challenged. With the mutator installed they download its package (about 110 KB, like
  any server-side mod), and the ban list checks by IP / UT2004 id apply to them as to everyone.

| Part | Where |
|---|---|
| Mutator for stock UT2004 servers (3369, OldUnreal 3374), UnrealScript | `Server/Validator/Classes/`, built by `Server/Validator/build-validator.sh` |
| Settings template | `Server/Validator/OpenUT2k4Validator.ini` |
| The same validator built into the OpenUT2k4 dedicated server (C++) | `Client/Source/OpenUT2k4/Net/UE2ServerValidator.*` |
| Client side: the player key, the answer | `Client/Source/OpenUT2k4/Net/UE2NetValidator.*` |
| Crypto (SHA-256, SHA-512, RSA, Ed25519 verify), own code | `Client/Source/OpenUT2k4/Net/UE2NetValidatorCrypto.*` |
| Tools: release signing, signed shared ban lists | `Client/Tools/openut2k4_signing.py`, `Client/Scripts/sign-release.sh` |

## How it works

```
OpenUT2k4 client                                server with the validator
  LOGIN URL=...?OpenUT2k4=0.1.0           ->     notes "OpenUT2k4 0.1.0"; IP / subnet / UT2004 id bans checked
  JOIN                                    ->     stock server: spawns OUTValidatorLink (owned by the player's
                                                 PlayerController, replicated to that player only) with
                                                 ChallengeText = "OUTV1;<nonce>;<server ip:port>;<map>;<player id>"
                                                 OpenUT2k4 server: control message "OUTVALIDATE CHALLENGE=<same form>"
  refuses a challenge that names another port than the one it dialed (relay), else signs
  "OUT2K4RESP2;" + challenge + ";" + dialed      (dialed = the IP:port it really sends to, "/host" when joined by name)
  ServerResponse(Part, Total, Data) x 5   ->     joins "OUTR2;<dialed>;<public key hex>;<signature hex>", checks the
  (OpenUT2k4 server: OUTVALIDATE PART= TOTAL= DATA=)   signature, then that <dialed> is this server (below);
                                                 fingerprint = first 32 hex digits of SHA-256(public key hex)
                                                 Identified / Failed / None -> key store, bans, probation, policy
```

- **Player key**: RSA-2048, public exponent 3, made on the player's computer on first start (in the background, about
  0.3 - 0.9 s) and kept in the player's profile: `<Saved>/Identity/PlayerKey.txt` (on Linux
  `~/.config/Epic/OpenUT2k4/Saved/Identity/`, with `-userdir` under that folder), mode 0600 where the OS has it. Not in
  the package, not in the content cache. A missing or damaged file is replaced by a new key, silently (the player then
  has a new id on every server). The client logs the fingerprint; the private key never reaches a log.
- **Signature**: PKCS#1 v1.5 with SHA-256. Exponent 3 makes the check cheap where it matters (UnrealScript on a 2004
  server); it is safe for signatures because the whole encoded block is compared, never parsed.
- **Replay**: every challenge has a fresh nonce (SHA-256 over a per-server secret, a counter, the time and random
  values) plus this server's address, the map and the player id, and only the challenge sent to that connection is
  accepted, once. An answer recorded elsewhere, a signature made with another key and a tampered signature all fail.
- **Relay**: a server a victim joins could pass on another server's challenge and forward the answer. The client signs
  the address it really dialed (resolved IP and port, plus the host name when joined by name; never reported by the
  server) and refuses (`NOKEY`, result None) a challenge whose port differs from the port it dialed. The verifier
  accepts only when the signed port is its own and the signed IP or host name is one of its addresses: an entry of
  `PublicAddress=`, its bound / multihome IP (the mutator: `Level.GetAddressURL()`), a local interface (our server) or
  loopback. Otherwise: **Failed** ("relayed") when `PublicAddress=` is set; without it (a server behind NAT sees
  only its private address) the answer is accepted when the port matches and marked **address unverified** in the
  log, the admin list and the API. **Admins behind NAT should set `PublicAddress=`** (their public IP and / or host
  name) so relays fail.
- The client signs nothing but `OUT2K4RESP2;` followed by a challenge that starts with `OUTV1;` (printable, at most 240
  characters) and its own dialed address.
- **Cost** (measured, see "Tests"): stock 3369 server, one check = 548 rows of Montgomery multiplication, about 12 - 17
  ms of UnrealScript time, spread over 9 ticks at the default `VerifyRowsPerTick=64` (at most about 2 ms per tick).
  Our C++ server: 0.4 - 1.7 ms. Client: signing about 30 ms once per join.

### Results

| Result | Meaning |
|---|---|
| Identified | a valid signature: the fingerprint is the player's id on this server |
| Failed | bad signature, wrong key, replayed or malformed answer, or relayed (the signed dialed address is another server's) |
| None | a real UT2004 client (never challenged), or an OpenUT2k4 client without an answer in `ResponseTimeout` seconds (an old version), without a key, or refusing a challenge that names another port (`NOKEY`) |
| Pending | (while the check runs) |

## Install

### Stock UT2004 server (3369 or OldUnreal 3374)

1. Build the package (needs Wine and any UT2004 install with `System/UCC.exe`; the compiled `.u` is a build artifact,
   never committed): `Server/Validator/build-validator.sh [--ut2004 <install>]` -> `Server/Validator/Build/`.
2. Copy `OpenUT2k4Validator.u` and `OpenUT2k4Validator.int` into the server's `System/`.
3. In the server's `UT2004.ini`:
   ```
   [Engine.GameEngine]
   ServerActors=OpenUT2k4Validator.OUTValidator
   ```
   (or start the server with `?Mutator=OpenUT2k4Validator.OUTValidator`). It adds itself to the package map
   (`AddToPackageMap`), so no `ServerPackages=` line is needed. Installing it twice is harmless.
4. Copy the `[OpenUT2k4Validator.OUTValidator]` section of `Server/Validator/OpenUT2k4Validator.ini` into `UT2004.ini`
   if you want to change a default.

At map start the server log shows the settings and a self-test with the measured cost:
`OpenUT2k4Validator: self-test passed: SHA-256 and RSA-2048 verify (548 rows) took 11.78 ms of script time over 9 ticks`.

State files in `System/`: `OpenUT2k4ValidatorKeys.ini` (keys, bans, whitelist; written by the validator) and
`OpenUT2k4ValidatorShared.ini` (imported shared ban lists; written by the import script).

### OpenUT2k4 dedicated server

The same `ServerActors=OpenUT2k4Validator.OUTValidator` line (or `Mutators=` / `?Mutator=`) in its server ini turns on
the built-in validator; no `.u` is needed. Off by default. It reads the same `[OpenUT2k4Validator.OUTValidator]` keys
and keeps its state in the state folder (`-ServerStateDir`): `ValidatorKeys.ini` and `ValidatorShared.ini`, in the
mutator's format (you can copy them between the two). Differences:

- The challenge goes over the control channel, only to clients that sent `?OpenUT2k4=`.
- It fetches signed shared ban lists itself (files, http and https) and checks them (`BanListSource=`).
- `bUseKeyAsPlayerId=True`: an Identified player's fingerprint becomes the player id the `kick` / `ban` commands and the
  server's ban list (`Bans.ini`) use. The `ban` command also writes a combined validator ban (key, IP, UT2004 id).
- "Mark" shows in `validator status` only; probation "Limited" acts as "Flag" (our server has no voting yet); no
  sanity checks yet.
- Admin console: `validator status | ban <id|name|fingerprint|ip|cidr> [reason] | unban <fingerprint|ip|cidr> |
  whitelist <id|name|fingerprint> | unwhitelist <fingerprint> | export [issuer] | import`.

## Settings (`[OpenUT2k4Validator.OUTValidator]`)

The defaults change nothing for players: everything is logged and marked, nobody is kicked.

| Key | Default | Meaning |
|---|---|---|
| `IdentifiedPolicy`, `FailedPolicy`, `NoResponsePolicy` | `Mark` | per result: `Allow` (log), `Mark` (log, and publish the result to every client: `OUTValidationInfo` replicates), `Kick` |
| `ResponseTimeout` | 30 | seconds an OpenUT2k4 client has to answer |
| `PublicAddress` | (none) | this server's address as players dial it: IP or host name, one line each. Set it behind NAT: then answers signed for any other address fail (relay); unset, unmatched addresses are accepted as "address unverified" |
| `bPublicFingerprints` | False | replicate fingerprints with a marked result (off: they stay on the server) |
| `bWhitelistOnly` | False | only whitelisted keys may play (OpenUT2k4 clients) |
| `ProbationHours` | 0 (off) | keys this server first saw less than this many hours ago are on probation |
| `ProbationPolicy` | `Flag` | `Allow`, `Flag` (log, admin list, marked), `Limited` (Flag, and the player's map / kick votes go nowhere: their xVoting `VotingReplicationInfo` loses its handler), `Kick` |
| `MaxProbationPlayers` | 0 (no cap) | more players on probation at once are kicked |
| `BanPolicy` | `Kick` | `Kick` or `Flag` when a player matches a ban |
| `bMatchBanIP` | True | match the IP a ban recorded |
| `bMatchBanCIDR` | False | match a ban's subnet (off: shared ISPs, carrier-grade NAT) |
| `bMatchBanLegacyId` | True | match the UT2004 CD key id (`GetPlayerIDHash`) a ban recorded |
| `bApplyBansToAllClients` | True | check every player's IP / UT2004 id against the bans, not only OpenUT2k4 clients |
| `TrustedBanIssuer` | (none) | `<64 hex Ed25519 public key> <name>`, one line per trusted shared list issuer |
| `BanListSource` | (none) | OpenUT2k4 server: files or http(s) URLs of signed lists, fetched every `BanListRefreshMinutes` (60) |
| `bUseKeyAsPlayerId` | False | OpenUT2k4 server: the fingerprint becomes the player id |
| `bSanityChecks` / `bSanityChecksAllClients` | True / False | sanity checks for OpenUT2k4 clients / for everyone |
| `SanityPolicy` | `Log` | `Log` or `Kick` |
| `SpeedHackRatio`, `MaxWalkSpeedFactor`, `SnapAngle`, `MaxNewNetPingDT` | 1.25, 1.6, 60, 1.0 | sanity thresholds (below) |
| `VerifyRowsPerTick` | 64 | the work per server tick (a check is 548 rows) |
| `bSelfTest` | True | check the crypto against a built-in test vector at map start and log the cost |

Admin commands on a stock server (logged-in admins): `mutate validator status`, `mutate validator ban <player |
fingerprint | ip | cidr> [reason]`, `unban <fingerprint | ip | cidr>`, `whitelist <player | fingerprint>`,
`unwhitelist <fingerprint>`, `bench` (one check, synchronously, with its cost).

## Bans, probation and ban evasion

A ban records the key fingerprint, the IP **as the server saw the connection** (`PlayerController.GetPlayerNetworkAddress()`
/ the UE2 connection address; the client is never asked for its IP and never sends it), optionally a subnet, and the
UT2004 CD key id. A match on **any** recorded field applies `BanPolicy`: IP / subnet / UT2004 id at login (every client),
the key once it is Identified.

```
Bans=<fingerprint>|<ip>|<cidr>|<ut2004 id>|<date>|<reason>|<issuer>      (empty fields are not compared)
Keys=<fingerprint>|<first seen YYYY-MM-DD HH>|<last seen>|<last name>
Whitelist=<fingerprint>|<comment>
```

A banned player who deletes their key comes back with a new fingerprint, but from the same IP (caught by the IP), with
the same UT2004 id (caught by that), and with a key this server has never seen (probation: flag, limit, kick or cap).
Limits, stated plainly: a VPN or a new dynamic IP plus a new key and no CD key id gets past a ban; subnet bans catch
more but also hit innocent players on the same ISP or carrier-grade NAT. Probation buys time for admins, nothing more.

## Shared ban lists

Admins can publish their bans for others, signed with their own Ed25519 key (kept outside any repository):

```sh
T=Client/Tools/openut2k4_signing.py
python3 $T genkey --name banlist                      # ~/.config/openut2k4/signing/banlist.key (0600) and .pub
python3 $T pubkey --key ~/.config/openut2k4/signing/banlist.key --hex   # what others put in TrustedBanIssuer=
python3 $T banlist-export --ini System/OpenUT2k4ValidatorKeys.ini --issuer MyLeague --out bans.txt
                                                       # (OpenUT2k4 server: "validator export MyLeague" writes one)
python3 $T banlist-sign --key ~/.config/openut2k4/signing/banlist.key --in bans.txt --out bans.signed.txt
```

Format (UTF-8 text; the signature covers every byte before the `sig=` line):

```
OUT2K4BANLIST1
key=<64 hex Ed25519 public key>
issuer=MyLeague
date=2026-10-18 15:10
ban=<fingerprint>|<ip>|<cidr>|<ut2004 id>|<date>|<reason>|<issuer>
sig=<128 hex Ed25519 signature>
```

Importing: only lists whose `key=` is in `TrustedBanIssuer=` and whose signature checks are used; malformed entries
are dropped; a list that fails keeps that source's previous entries.

- **Stock server**: UnrealScript cannot fetch HTTPS or check Ed25519 at an affordable cost, so a helper does it (cron):
  `python3 openut2k4_signing.py banlist-import --trust-ini System/UT2004.ini --out-ini
  System/OpenUT2k4ValidatorShared.ini https://example.org/bans.signed.txt friends.signed.txt` (it reads the
  `TrustedBanIssuer=` lines of the ini, or take `--trust <hex> [name]`). The mutator reads the file when the next map
  starts.
- **OpenUT2k4 server**: `BanListSource=` lines; fetched at start, every `BanListRefreshMinutes` and on `validator
  import`.

## API for other mods

Every PlayerController gets an `OUTValidationInfo` (real UT2004 clients too, with `Result` None), so mods and
server-side anti-cheat can ask about any player. Static functions of `OpenUT2k4Validator.OUTValidator` (add
`OpenUT2k4Validator` to your `EditPackages` before your package):

| Function | Returns |
|---|---|
| `GetResult(Controller C)` | `RESULT_NONE` 0, `RESULT_PENDING` 1, `RESULT_IDENTIFIED` 2, `RESULT_FAILED` 3 (`ResultName(byte)` gives the text) |
| `GetFingerprint(Controller C)` | 32 hex digits (Identified only), else "" |
| `GetFirstSeen(Controller C)` | "YYYY-MM-DD HH" this server first saw the key (server time), else "" |
| `GetKeyAgeHours(Controller C)` | hours since then, -1 when not Identified |
| `GetIP(Controller C)` | the connection's IP as the server sees it |
| `IsOnProbation(Controller C)`, `IsOpenUT2k4(Controller C)` | bool |
| `GetDialedAddress(Controller C)`, `IsAddressVerified(Controller C)` | the address the client signed as dialed ("ip:port[/host]"); false when an Identified player's address could not be matched to this server (NAT without `PublicAddress=`) |
| `MatchesBanList(Actor Context, string Fingerprint, string IP, optional string LegacyId, optional out string Reason)` | does any of them match the local or shared bans (`bMatchBanIP` / `bMatchBanCIDR` / `bMatchBanLegacyId` apply) |
| `GetInfo(Controller C)`, `GetValidator(Actor Context)` | the actors themselves |

Without a compile-time dependency:

```
local ReplicationInfo RI;
foreach DynamicActors(class'ReplicationInfo', RI)
    if (RI.IsA('OUTValidationInfo') && RI.Owner == SomePlayerController)
        log(RI.GetPropertyText("Result") @ RI.GetPropertyText("Fingerprint") @ RI.GetPropertyText("IP"));
```

(The info's `Owner` is the player's PlayerController.)

`OUTValidationInfo` fields (server side): `Controller`, `PRI`, `Result`, `bOpenUT2k4`, `ClientVersion`, `Fingerprint`,
`FirstSeen`, `KeyAgeHours`, `IP`, `LegacyId`, `DialedAddress`, `bAddressUnverified`, `bProbation`, `bWhitelisted`, `bBanMatch`, `BanReason`, `Reason`.
Replicated to every client when marked: `PRI`, `Result`, `ClientVersion`, `bProbation`, `PublicFingerprint`.

Existing anti-cheat authors may choose to treat Identified OpenUT2k4 players differently; the validator never changes
their mods. OpenUT2k4 server (C++): `UE2ServerValidator::GetStatus(ConnId, Status)`, `GetAllStatus()`,
`MatchesBanList(Fingerprint, IP, LegacyId, &Reason)` (`Net/UE2ServerValidator.h`).

## Sanity checks

Log-only by default (`SanityPolicy=Log`), for OpenUT2k4 clients (or everyone with `bSanityChecksAllClients`). Each
check needs several bad windows in a row and reports at most once a minute per player; a summary is logged when the
player leaves (`time stamp rate avg 1.00 max 1.00, walk speed max 1.01 x ground speed, instant hits ...`).

- **Time stamps** (speed hack): the client's move time stamps (`PlayerController.CurrentTimeStamp`) over 5 s windows
  against server time; three windows above `SpeedHackRatio` (1.25).
- **Walking speed**: horizontal speed while walking on non-moving ground over 1 s windows against `GroundSpeed *
  MaxWalkSpeedFactor + 50` (speed combos included; teleports, translocation, lifts, vehicles and air time excluded).
- **NewNet**: the `PingDT` a lag compensated fire mode computed from the client's time stamp (read by name, no
  dependency on UTComp): negative, above `MaxNewNetPingDT`, or far above the player's ping, three times.
- **Aim**: instant hit hits that came within 0.12 s after a view change of more than `SnapAngle` degrees; reported at
  20+ hits when more than half are such snaps.

These catch crude cheats only. They are deliberately conservative: no false kicks.

## Threat model and limits

- **What it proves**: the player holds the private key of that fingerprint. Ids are stable, cannot be forged or
  copied from another player's answer (replay fails), and bans by key hold until the player throws the key away.
- **What it does not prove**: anything about the build. The key is made and kept by the client; a modified client (or
  a cheat tool) can use it, and a player can make any number of keys (a new key = a new, unknown player). That is why
  probation, IP / UT2004 id bans and shared lists exist, and why real cheat detection is the server-side checks.
- A key copied off a player's computer lets someone else appear as that player (like any stolen credential).
- Relay: closed by the signed dialed address (see "How it works"). It stays possible only against a server behind NAT
  without `PublicAddress=` when the relaying server uses the same port: such answers are marked "address unverified"
  (log, admin list, `IsAddressVerified`), never silently trusted. A relay on the same machine as the server (loopback)
  is not detected: whoever controls the server's machine controls the server anyway.
- IP bans: VPNs, dynamic IPs, shared and carrier-grade NAT (see above).
- Real UT2004 clients: no identity beyond what UT2004 has (IP, CD key id).
- Privacy: the fingerprint is a per-player id across all servers that run the validator, like the UT2004 CD key id.
  It is not shown to other players unless `bPublicFingerprints=True`. Deleting `PlayerKey.txt` gives a new id.

## Release signing (project owner)

Separate from all of the above and invisible to players: release archives get a detached Ed25519 signature in
minisign's format. The owner makes a key once, outside the repository:

```sh
python3 Client/Tools/openut2k4_signing.py genkey            # ~/.config/openut2k4/signing/release.key (0600), release.pub
```

and publishes the public key line (README / release page). The build scripts (`Client/Scripts/build-*.sh`,
`Server/build-*.sh`, `.bat`) pack the `Dist` folder into an archive and sign it **only** when `OPENUT2K4_SIGNING_KEY`
is set to the key's path (`Client/Scripts/sign-release.sh`; Windows: a `.zip` via PowerShell and Python). Without it,
nothing happens. Players check a download with `minisign -Vm OpenUT2k4-0.1.0-Linux.tar.gz -P <public key>`, or
`python3 openut2k4_signing.py verify --pub <public key> <archive>`, or compare the `.sha256` file. Keep the key off CI
logs and never commit it (`.gitignore` covers `*.key`, `signing/`, `*.minisig`).

## Building and testing

- Mutator: `Server/Validator/build-validator.sh` (UCC under Wine, a private Wine prefix in the output folder).
- Client crypto test vectors: `-UE2ValidatorSelfTest` (SHA-256 / SHA-512 FIPS vectors, RSA and Ed25519 vectors made
  with independent implementations, tampering, key generation).
- Test switches (non-shipping): `-UE2PlayerKey=<file>`, `-UE2ValidatorTest=badsig | wrongkey | nokey | silent |
  record=<file> | replay=<file> | dialport=<n> | relaydialed=<ip:port>` (the last one signs another dialed address
  without the client's port check: a victim's answer forwarded by a relay).

### Tests (Linux, 2026-10-18, local servers on 127.0.0.1 only)

Stock 3369.3 dedicated server (native Linux binary) with the mutator and 2 bots; OpenUT2k4 clients (editor `-game`
build, `-UE2NoCDKey`, isolated user folders); the OldUnreal 3374 Linux client.

| Case | Result |
|---|---|
| self-test at map start | passed, 11.8 - 13.1 ms script time over 9 ticks, at most 2.1 ms per tick |
| OpenUT2k4 client, new key | Identified, 12.0 - 16.7 ms script time over 9 ticks; the package (110 KB) downloaded over the file channel first |
| same client again, and after a client restart | same fingerprint, first-seen kept |
| key file deleted | new key made silently (0.3 - 0.6 s), new fingerprint, Identified |
| `badsig`, `wrongkey` | Failed (bad signature) |
| `record` then `replay` (an earlier answer) | Identified, then Failed |
| `nokey` / `silent` | None (no key) / None (no answer in 15 s) |
| `FailedPolicy=Kick` with `badsig` | kicked |
| probation 24 h, Flag | flagged, API shows `probation True` |
| probation, `MaxProbationPlayers=1`, two new keys | the second kicked |
| probation, Kick | new key kicked; a whitelisted key admitted |
| key ban | kicked after identification |
| ban with key + IP, player deletes the key and rejoins | kicked at login by the IP |
| subnet ban `127.0.0.0/8`, `bMatchBanCIDR=True` | kicked |
| signed shared list imported by the helper | key in the list kicked; tampered copy rejected (BAD SIGNATURE), untrusted issuer rejected |
| API test mod (`GetResult`, `GetFingerprint`, `GetFirstSeen`, `GetKeyAgeHours`, `GetIP`, `IsOnProbation`, `MatchesBanList` by IP, the dependency-free route) | right values for OpenUT2k4 and real UT2004 players |
| OldUnreal 3374 client | joined and played; logged as "UT2004 client", Result None, no challenge actor |
| sanity summaries of the test players | time stamp rate 1.00, walking speed at most 1.01 x ground speed: no reports |

OpenUT2k4 dedicated server (development build, `-OpenUT2k4Server`) with `ServerActors=OpenUT2k4Validator.OUTValidator`:
Identified over the control channel (0.4 - 1.7 ms), `badsig` and `replay` Failed, a signed list from a file and over
HTTP imported, the tampered list rejected, the shared-list key kicked, `ban` of a connected player wrote a combined ban
and (with `bUseKeyAsPlayerId`) the fingerprint as the player id, the same player with a new key was refused at login
(`LOCALBAN`, IP), `validator export` gave a list that `banlist-sign` / `banlist-verify` accept.

Relay (after adding the dialed address; a transparent UDP relay in front of the server, and forwarded signatures):

| Case | Stock server (mutator) | OpenUT2k4 server |
|---|---|---|
| direct join | Identified, dialed 127.0.0.1:port (verified) | Identified, verified |
| relay on another port (127.0.0.1:7797 -> the server) | client refused: None ("the challenge names port 7791, we dialed 7797") | same (7795 / 7797) |
| answer signed for another port (`relaydialed=127.0.0.1:7792`) | Failed ("relayed: ... not this server's port") | Failed |
| relay on the same port, another address (127.0.0.2 -> 127.0.0.1), no `PublicAddress` | Identified, "address unverified (dialed 127.0.0.2:7791)" | (not run) |
| same, `PublicAddress=127.0.0.1` | Failed ("relayed: ... not one of this server's addresses") | Failed |
| answer signed for 10.1.2.3 on the right port, no `PublicAddress` | Identified, address unverified | (not run) |

Not tested: probation `Limited` with xVoting map voting on, sanity reports against an actual cheat, Windows / macOS
(`SecureRandomBytes` uses `CoCreateGuid` there), an OldUnreal 3374 server (the package uses only 3369 features).
