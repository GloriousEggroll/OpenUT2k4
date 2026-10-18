# Release builds: self-hosted runners

`.github/workflows/release.yml` builds the client and the dedicated server (with the OpenUT2k4 Validator) and attaches
them to a GitHub release. GitHub's hosted runners have no Unreal Engine, and the engine may not be redistributed, so
the builds run on **self-hosted runners**: machines with Unreal Engine 5.8 installed that GitHub sends the jobs to.

| Job | Runner labels | Needs | Enabled |
|---|---|---|---|
| `linux` | `self-hosted, Linux, X64, openut2k4` | UE 5.8, a UT2004 install (only to compile the validator mutator), Wine, Python 3 | always |
| `windows` | `self-hosted, Windows, X64, openut2k4` | UE 5.8, Visual Studio 2022, Git for Windows (bash), Python 3, 7-Zip on PATH | repository variable `OPENUT2K4_BUILD_WINDOWS=true` |
| `mac` | `self-hosted, macOS, openut2k4` | UE 5.8, Xcode, Python 3 | repository variable `OPENUT2K4_BUILD_MAC=true` |
| `release` | GitHub-hosted `ubuntu-latest` | nothing | always (attaches the archives) |

The validator mutator is compiled once on Linux and passed to the Windows and macOS server packages.

## Set up a runner (Linux)

1. **Register it.** On GitHub: *Settings → Actions → Runners → New self-hosted runner → Linux x64*. Run the download
   commands shown there in a folder of your choice, for example `~/actions-runner`, then register:

   ```sh
   ./config.sh --url https://github.com/GloriousEggroll/OpenUT2k4 --token <token from that page> \
       --labels openut2k4 --name "$(hostname)-ue5" --unattended
   ```

2. **Tell it where the engine and UT2004 are.** Create `.env` in the runner folder:

   ```sh
   UE_ROOT=/home/<you>/UnrealEngine/Linux_Unreal_Engine_5.x
   UT2004_DIR=/home/<you>/UnrealEngine/UT2004/Unreal Tournament 2004
   ```

   The UT2004 install is only read, to compile the validator. Nothing from it goes into a package (`check-release.sh`
   checks every package).

3. **Run it as a service** so it survives reboots:

   ```sh
   sudo ./svc.sh install "$USER" && sudo ./svc.sh start
   ```

   or run `./run.sh` in a terminal while you make a release.

On Windows and macOS the steps are the same: use the matching runner download, and set `UE_ROOT` in `.env`. Then set
the repository variable (*Settings → Secrets and variables → Actions → Variables*) `OPENUT2K4_BUILD_WINDOWS` or
`OPENUT2K4_BUILD_MAC` to `true`.

## Release signing secret

Add the release key as a repository secret: *Settings → Secrets and variables → Actions → New repository secret*.
Name it `OPENUT2K4_SIGNING_KEY`, and paste the whole content of `~/.config/openut2k4/signing/release.key`.

Each job writes the key to a 0600 file in the runner's temp folder only for the signing step, and deletes it
afterwards. It never reaches the log. Without the secret, the archives are published unsigned, with a `.sha256` file
and a warning in the run.

## Making a release

1. Set `ProjectVersion=X.Y.Z` in `Client/Config/DefaultGame.ini` and commit it.
2. Tag and push:

   ```sh
   git tag vX.Y.Z && git push origin vX.Y.Z
   ```

   The workflow builds everything and publishes the release `vX.Y.Z` with these files, each with `.minisig` and
   `.sha256`:

   - `OpenUT2k4-X.Y.Z-<Platform>.tar.gz` (`.zip` on Windows)
   - `OpenUT2k4Server-X.Y.Z-<Platform>.tar.gz` (`.zip` on Windows)

3. To rebuild an existing tag, use *Actions → Release → Run workflow* with the tag. Manual runs create a **draft**
   release by default.

## Security

- **Pull requests:** the workflow never runs for them. Self-hosted runners execute whatever a workflow tells them,
  so never add `pull_request` triggers to workflows that use these runners. Keep *Settings → Actions → General →
  Fork pull request workflows* on "Require approval".
- **Runner account:** the runner runs as the user that installed it. A dedicated user account with read access to
  the engine and the UT2004 install is safer than your own.
- **The signing key:** it is stored in GitHub's secret store and briefly on the runner during signing. If it ever
  leaks:
  1. Make a new key with `genkey --force`.
  2. Publish the new public key in `README.md`.
  3. Replace the secret.
