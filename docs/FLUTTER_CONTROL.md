# Flutter Windows control host

The integration baseline is `edf4e48e881ed3f7b270892a64dbe89febf25191`.
Build with `VENTURE_BUILD_GUI=OFF`, `VENTURE_BUILD_CONTROL=ON`, then build
`venture-control`, `venture-runtime`, and `package-control` using the pinned
Windows toolchain. The packaged layout replaces the GUI entry point and retains
the complete Java/ANGLE/media runtime, dependency checks, and licenses.

## Protocol v1

The console executable is a persistent UTF-8 NDJSON server. Send one request per
line on stdin: `{"id":1,"method":"hello","params":{}}`.
Responses have the same `id` and either `result` or
`error: {"code":"operation_failed","message":"..."}`. stdout is exclusively
protocol data; diagnostics go to stderr and the profile logs. Closing stdin
cancels pending work, rolls back staged installs, stops the owned runtime, and
exits. The Flutter parent starts the process without a console window.

| Method | Parameters / result |
| --- | --- |
| `hello` | Protocol, Venture version, data directory, writable status |
| `library` | Refresh; full installation details and runtime state |
| `schema` | Controller profiles, host/Quest inputs, compatibility switches, ranges and defaults |
| `scan` | `path`; task result is APK candidates with package/name/version/path |
| `install` | APK `path`, `sourceRoot`, `package`, optional target `installId` |
| `patch` | Data `path`, `package`, explicit `installId` when ambiguous |
| `play`, `stop` | `installId` |
| `uninstall` | `installId`; retains saves |
| `directories` | `installId`; application/mods/saves directories |
| `settings.get` | Optional `installId`; game overrides include global values |
| `settings.set` | Optional `installId`, `changes` JSON merge patch |
| `settings.reset` | Optional `installId`; reset runtime defaults / inherit global |
| `saves.import` | `installId`, local `path` or optional headset `serial` and per-request `adbExecutable` |
| `saves.export` | `installId`, optional headset `serial` and per-request `adbExecutable`; task result includes files/kept/removed/backup/hasPrivate |
| `task.get`, `task.cancel` | `taskId` |

For headset save imports, the assistant supplies the selected ADB transport and
its bundled executable. The override must be an existing absolute executable
path and applies to this request only; it is not saved in settings. The
installation's package selects the headset app, and existing save locking,
pinned-version/account storage, staging and backup handling remain in effect.
Imports read ADB-accessible `Android/data/<package>` files, not Android private
storage. The control RPC regression uses a binary-clean fake headset to check
device/package selection, imported bytes, backups and missing-app preservation.

`saves.export` sends the selected installation/account's shared saves to the same
package on the selected headset, including pinned-version storage in control
mode. It uses upstream's backup, remote staging and size/count verification
before stopping the headset game and publishing the files. Unreadable files
that could not be backed up are kept on the headset; private Android storage is
not exported. Both directions require the emulator game to be stopped. The
Flutter UI uses the existing confirmation dialog and makes no task request
until the user accepts the target-specific overwrite warning. Cancelling or
closing the confirmation leaves both copies untouched.

Long operations return `{"taskId":"..."}` immediately. State events contain
`{"event":"state","library":[],"tasks":[],"scanning":false,"notice":"","warning":""}`.
Tasks contain taskId/state/done/total/error/result. State is running, complete,
failed or cancelled. Progress byte counters can restart between staging/copy
phases; a zero total means indeterminate progress. Recent 256 task results are
retained. `library` also responds immediately with the current snapshot, so wait
for scanning=false to consume the refreshed state.

Library entries and `play`/`stop` results include `running`, `starting`,
`loading`, and `failure`. `loading` spans queued startup and the live runtime's
initial load. It clears after the first successfully submitted projection layer,
not on process creation or empty XR frames; this is a rendering boundary, not a
guarantee that the game's main menu is ready. Readiness travels over the private
launcher pipe (`runtime.ready`) from a monitor thread, never the render thread.
Worker replacement resets readiness. Exit or failure clears loading; the Flutter
service also clears active states if the control process disconnects. The client
locks repeated launch requests immediately, even before receiving the RPC reply,
and retains that lock across library tab reconstruction.

`installId` is the absolute physical installation directory, not just a package
name; copies in different libraries remain distinct. Settings use Venture's
existing app/pinned identity and saves use its account/local/pinned rules.
`--launch package[@pin]` remains available for generated Steam shortcuts.

## Ownership and recovery

Core is only called on its owning thread. The stdin reader queues requests,
disk operations use Worker, and tick/drain publishes completions. One disk task
runs at a time, the shared profile queue lock excludes GUI/control writers, and
package locks prevent changing running games. Settings writes reload and merge
under the settings lock. Another instance is rejected before profile migration.

Install/patch transactions stage each destination on its own volume. Durable
profile journals record original targets and same-parent staging/backup paths;
uncommitted publication rolls back on the next controlled startup, committed
publication only cleans backups. Recovery requires profile and package locks;
failure retains the journal/backups and reports an error. Generated runtime OBB
junctions are skipped during staging and reconstructed at launch; other source
or installation links are rejected. No power-loss guarantees beyond the host
filesystem's rename/write durability are made.

Local modified files match the manifest by filename regardless of catalog size.
Application SHA comparisons, recorded-size refusal, and Unity declared-length
refusal are removed. Manifest records remain useful for location/progress, and
APK parsing, required-file, incomplete-install, transfer/IO, archive and path
checks remain. Release dependency verification is unchanged.

## Local user without a Meta account

Launching with account `local` creates a random positive user ID, alias and
display name in `<data-dir>/local-user/profile.json`. Creation uses a profile
lock and atomic write; subsequent launches and games reuse the same identity.
Existing local save paths remain unchanged. Invalid profiles report failure
without replacing the identity. Keep this file when moving the data profile.

The launcher sends the local identity to the runtime over its private broker
pipe. Local Platform initialization, logged-in user queries, own-user lookup
and entitlement checks succeed. UserProof returns a fresh `venture-local-...`
offline placeholder for SDK initialization; it cannot authenticate to Meta.
No real Meta access token, user proof, purchase or online credential is generated.
Signed-in accounts retain their original
identity, entitlement and network flows.

`local_user_broker` tests persistence, concurrent initialization, corrupt-profile
preservation, local entitlement, broker identity binding and the existing real
account credential/refresh/sign-out flows against a loopback server.

The Android framework also includes DNS-SD types used in SDK method signatures.
Without an NSD daemon it reports asynchronous failures instead of advertising
successful network discovery. `framework_nsd` covers reflection, service lookup,
parcel round trips and failure callbacks; `framework_java` remains the framework
regression suite.

H3VR2 (`com.vrzwk.rustltd.h3vr2`, 200502) reached its Chinese save-slot screen
with the persistent local identity and loaded base HDOG content. Captured GPU
eye images confirm the menu, rather than only a surviving process. Two bounded
150-second runs exited normally. The final run lost host input focus later and
then submitted empty layers, so it does not prove continuous gameplay rendering.
BrainCloud's online Meta authentication rejects the local identity with 403;
local simulation does not supply a real remote account. Controller selection,
new-game progression and full gameplay still need manual hardware verification.

Run `ctest --test-dir build/windows-release --output-on-failure -R
'^(local_user_broker|control_install|control_rpc|launcher_settings_cli)$'`. The RPC test generates
an isolated legal APK fixture and uses a launch probe, not a real PC VR game.
Real headset import, Vulkan/OpenXR rendering, audio and controller operation
require hardware acceptance testing.

## Upstream v0.2.5.1 merge

`questtool_v0.2.5.1` merges upstream tag `v0.2.5.1`
(`f88cc2b7fb2556887926c637d21d037c1ea6856a`,
`https://github.com/Villagers654/Venture.git`) into QuestTool's `questtool`
integration (`b42370cdf8d6ad2f5ae2bac8e8ef9dd8e3124798`).

The control executable, settings JSON API, explicit installation selection,
local identity and installation-file handling remain available. Unlike the
upstream GUI, QuestTool continues to accept a persisted `runtimeExecutable`
override; missing executables fall back to the bundled runtime. Upstream's
`VENTURE_RUNTIME` development override is also supported.

Save import keeps QuestTool's pinned-version storage and uses upstream's shared
headset selection and backup timestamp handling, alongside the new save export
implementation. NSD uses upstream's fuller API implementation, without duplicate
Context registrations; the QuestTool NSD regression fixture is retained. The
upstream JRE EC crypto module fix is included in core-only release packaging.

Merge verification (Windows, runtime and GUI disabled in the verification build):

- Built `venture-control`, `venture-cli`, `venture_control_tests`,
  `venture_local_user_tests` and `venture_passthrough_launch_probe` in
  `build/questtool-merge-check`.
- CTest `control_install`, `control_rpc`, `local_user_broker` and
  `launcher_settings_cli` passed. The RPC suite checks modified installation
  files, per-install selection, startup readiness, settings and save import.
- Separately linked `test_launcher.cpp` against the launcher core: all 43 tests
  (456 assertions) passed, including the retained runtime override, missing-file
  scanning and save backup behavior.
- Compiled all 586 framework/bridge Java sources and the 21 CMake-listed Java
  fixtures with the pinned JDK; `FrameworkTest`, `NsdTest` and
  `AndroidKeyStoreTest` passed. `tests/jre_modules_test.py` passed both tests.

The monolithic `venture_tests` target does not link with the runtime disabled:
its HLE/loader tests require runtime symbols. This verification does not cover a
complete runtime/Release build or actual Vulkan/OpenXR gameplay.

On 2026-11-21, the full Windows runtime and control-only distribution were also
built successfully, including the headset-save-import ADB override. All six
integration CTests (`control_install`, `control_rpc`, `local_user_broker`,
`launcher_settings_cli`, `framework_java`, `framework_nsd`) passed. The packaged
control executable passed the RPC regression with a system-only PATH; its
`hello` response reports `0.2.5.1`. The Flutter 3.9.5 Windows installer includes
all 273 manifest-listed components plus the manifest itself, with matching
hashes and the JRE EC crypto module. Real headset save import and gameplay remain
hardware acceptance items.
