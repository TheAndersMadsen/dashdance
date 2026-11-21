# Portable Slippi service checkpoint

The macOS headless runtime is an offline diagnostic target. It does not yet
provide native online play. Its `MELEE_PORT_OFFLINE=1` build compiles the actual
online command owner with an offline implementation and omits identity, ENet,
HTTP and reporting implementations. `online::available()` returns false even
if a caller requests online mode at runtime. The full Windows implementation
is retained; installed Launcher profile discovery has been removed everywhere.

## Verified locally

- `slippi_offline_test`: the real command owner initializes and shuts down
  repeatedly without host, profile, filesystem or network dependencies. Login
  status is logged out, netplay input is disconnected, matchmaking reports an
  error, and rank fetch reports an error. Local delay and deterministic seed
  requests work. Playback/music commands retain their existing owners.
- `slippi_text_test`: actual Win32/iconv CP932 interface, tested on macOS with
  Japanese, Windows-31J extension bytes, punctuation, fixed-width connect codes
  and one replacement for an unrepresentable Unicode code point.
- `slippi_loopback_test`: the actual `NetplayClient` and ENet 1.3.13 exchange
  exact input/checksum bytes and acknowledgements, and deliver an explicit
  disconnect reason. Both sockets bind `127.0.0.1` on ephemeral ports; numeric
  addresses bypass DNS. No `User`, `Matchmaking` or reporter is constructed.
- `slippi_game_file_test`: legitimate resource names, 63-byte/NUL boundaries,
  truncated requests, regular files, direct/patch containment and symlink
  escapes. The Sys root is captured at EXI initialization.
- `slippi_record_events_test`: command-table resets, signed frame -123,
  game/frame/end counts, truncated records and the once-per-game offline input
  marker at post-frame 1. Actual `.slp` parsing remains the independent gameplay
  acceptance source.
- `slippi_matchmaking_protocol_test`: Slippi 3.7.0 matchmaking's pure parts: STUN
  binding requests and responses (RFC 5769's sample IPv4 response, XOR over plain
  MAPPED-ADDRESS, padding, foreign or error replies), NAT classification, the
  join/resume/cancel/ping messages and the server message kinds, the match-result
  parse (rank and mmr, bots, chat messages, LAN/public address choice, stages,
  items) and the rank thresholds. `slippi_net.cpp` uses these helpers.
- `port_mm_probe` (built with `MELEE_BUILD_PORT_TRANSPORT_TESTS` on macOS): the real
  `Matchmaking` against the live service with a real user.json, cancelling as soon
  as the ticket is queued (`unranked`) or holding a queue nobody can join (`direct`).
  Not a ctest because it needs the network and an account.
- Existing `native-slippi-services` Rust library: all 26 offline tests passed
  again (7 core/FFI, 8 lifecycle/mapping, 11 bounded-worker tests). Its actual
  release static library also passed the linked C lifecycle smoke test on
  macOS with `-fshort-enums`; no activation, profile read or report was requested.

The standalone executables are in `build/service-tests/`. The normal CMake
service tests require C++17. The transport test is an explicit option,
`MELEE_BUILD_PORT_TRANSPORT_TESTS=ON`; the offline runtime does not link it.
Native text uses `Iconv::Iconv`; transport additionally uses `Threads::Threads`
and the vendored ENet C target. Full Windows builds retain their Win32 host
dependencies. Windows and Linux have not been built in this checkpoint.

ENet's missing `unix.c` is the exact source from tag `v1.3.13`, commit
`f7c46f03fd8d883ac2811948aa71c7623069d070`, matching the existing vendored
headers/core files. See `port/third_party/enet/PORTABLE_SOURCE.md` for the
source hash and platform feature probes. Do not mix the separate Slippi
ENet 1.3.17 submodule into this version.

## Native online target and source mapping

The existing Rust ownership implementation is at
`/Users/andersmadsen/Documents/GitHub/melee-native-slippi/native/slippi/services`.
Its `Cargo.lock` and `Cargo.toml` pin `slippi-gg-api`, `slippi-user`, and
`slippi-game-reporter` to Slippi Rust Extensions commit
`2d29e794de8497582675fb70877851f2cdd2f256`. Reuse that implementation and its
tests, retaining its provenance. (That module labels its own adaptations GPL-2.0-only; Slippi Rust Extensions itself
ships the GPLv2 text without stating a version, see `THIRD_PARTY_NOTICES.md`.) It currently remains outside this
checkout; a reproducible native online target must import/pin that ownership
source or require an explicit verified source-directory/library input.

The current C ABI has exactly six lifecycle operations: create, enable network,
attempt login, enable reporting, shutdown and destroy. It does not expose
credential retrieval, user summaries, reporting submissions, replay chunks,
rank refresh or the asynchronous worker. Compiling this ABI does not complete
those integrations.

| Unlocked operation | Existing implementation to reuse | Concrete remaining work |
| --- | --- | --- |
| Offline EXI initialization | `online::init`, `slippi_offline.cpp` | Keep this target independent of native service activation. |
| Native profile/session creation | C `native_slippi_services_create`; Rust `Config`, `SessionServices::new` | Supply the explicit canonical owned profile and explicit application/reference versions. Keep construction inert. |
| `User` / `CMD_OPEN_LOGIN` / online status | C `enable_network`, `attempt_login`; Rust `user_summary`, `credentials` | Add typed summary/credential access to a service-thread adapter. ProfileLoaded means local parsing, not server authentication. Never discover another app's profile. |
| Matchmaking ticket | `Matchmaking::startMatchmaking` (STUN, then the websocket `join` from `protocol::join`) | Replace the raw `User*` dependency with an owned session snapshot and wire the pure serializer. Keep credential-bearing ticket bytes off the simulation thread and logs. |
| Ticket acknowledgement / assignment | The websocket session in `slippi_net.cpp` (`joined`, `status`, `match`, `error`); `protocol::parse_server_message`, `match_result` | Wire parsing before publishing a complete assignment. Preserve public/LAN choice, global ports, chat/rank fields, stage/item rules and mode/version. Publish immutable results back to the game thread. |
| Direct P2P inputs / rollback | Existing `NetplayClient`, `slippi_online.cpp` savestates | Retain ENet wire messages; resolve cross-thread ownership before broader tests. Validate selections, chat, game-prep, teams, time sync, packet loss/reordering and rollback against the pinned stock client. |
| `CMD_REPORT_GAME` | Rust `GameResult`, `PlayerResult`, `reference::map_report`, `ServiceWorker::FinalizedReport` | Extend the C interface for all existing report fields and all four global player slots. Move blocking work to the existing bounded worker. Stable host event IDs must survive rollback and distinguish ranked recovery events. |
| Replay report association | Rust `ServiceWorker::ReplayData` and actual upstream reporter | Forward the original EXI recording stream, including 0x35 resets and repeated rollback records. The Rust reporter uses raw replay association; the legacy C++ `.slp` filename is not a substitute. |
| Ranked game/set/status updates | Rust `MatchStatus::from_game_status`, `ReportMatchStatus` | Map existing C0-C4/status commands exactly. An accepted queue item is not delivered reporting success. Surface failures and explicit draining. |
| Rank, login watcher, logout | Current native Rust module intentionally omits asynchronous rank/watcher ownership | Add explicit worker lifetime and typed cache/status results; do not expose a fetched rank or authenticated session before those outcomes exist. Browser login remains a separate platform implementation. |
| Shutdown | Rust `ServiceWorker::Shutdown` and upstream draining | Join owned workers; queued reports may block while upstream HTTP/retries/hash work drains. Do not detach or invent abandoned/completed statuses merely from UI exit. |

The bounded Rust worker is already implemented and mock-tested. It accepts
EnableNetwork, AttemptLogin, EnableReporting, ReplayData, FinalizedReport,
ReportMatchStatus and Shutdown. Its replies distinguish ProfileLoaded,
ReportQueued and ShutdownComplete. Extend that typed command/reply surface
through a new explicit C worker adapter, retaining exclusive ownership, bounded
admission, request IDs, fixed errors and panic containment. The deterministic
game thread should only submit/poll owned commands and immutable snapshots.
Do not call the synchronous C lifecycle API from simulation, rendering or audio.

The Rust report-mode enum currently covers ranked, unranked, direct and teams;
Unlocked also names party mode. Party reporting must be compared with the pinned
upstream source and implemented explicitly; silently remapping its numeric mode
would not establish parity. Keep native application identification truthful;
the existing Rust composition replaces the upstream HTTP agent identity with
`MeleeNative/<version>` while retaining its reference-version routing.

Before a native online executable can be enabled, complete these gates in order:

1. Link the actual Rust owner through the native adapter and repeat inert C
   lifecycle plus mock worker/ticket/report/status tests with fixed ABI layout.
2. Resolve existing matchmaker/peer ownership: mutable selection/chat/prep state
   crosses threads, and the legacy connection cleanup detaches owners that hold
   a raw `User*`. Move publication/cleanup to the owned host service lifecycle.
3. Pass gameplay/AOT determinism and rollback checks, then compare transport and
   EXI behavior with the pinned stock client using local peers and test accounts.
4. Validate login and every ranked reporting outcome through an authorized
   service environment. No production acceptance or compatibility is inferred
   from local profile parsing, source similarity, compilation or loopback success.

## Sys input immutability checkpoint

The current offline milestone treats the configured Sys tree as immutable local
input. Canonical basename/file checks reject static directory/file symlink
escapes and non-regular files for both direct and `.diff` reads. However, the
helper returns a checked pathname which is opened afterward. An adversarial
concurrent rename/symlink replacement between those operations is not excluded.

Before a production online/frontend target, replace the pathname-return seam
with root-owned open/read operations: on POSIX use owned directory handles,
`openat`/no-follow handling and `fstat`; on Windows use reparse-safe handle
validation. Alternatively, complete and verify an immutable preload, then serve
only owned bytes for the session. Add concurrent replacement tests at that seam.
The current EXI preload worker joins on reinitialization and shutdown; cached
resource names are cleared when capturing a new Sys root.
