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
| `saves.import` | `installId`, local `path` or optional headset `serial` |
| `task.get`, `task.cancel` | `taskId` |

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
