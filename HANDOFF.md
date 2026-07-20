# Handoff

## Goal
Diagnose why toonyx's YouTube chat didn't join when the channel went live
(Twitch/Kick joined instantly), then bake in speed-ups for similar incidents.

## Completed
- [x] **Root-caused the missed live pickup**: a pre-live waiting room earns the
  upcoming videoId a recently-failed mark (5 min base / 60 min max cooldown,
  `YouTubeLiveChat.hpp:248`), and `resolveVideoId()` then discarded even the
  marker-gated browse/streams probes' confirmed-live results for that id --
  stream stays unjoined until the cooldown expires or the instance is recreated
  (app restart / re-adding the source, which is why both "fixed" it before).
  Today: live at 14:08:40 (InnerTube /player startTimestamp), still unjoined at
  14:12. Old process had no logging, so evidence was code + timeline.
- [x] **Fix**: `YouTubeLiveChat.cpp` `resolveVideoId()` -- ids reaching the
  outer callback during cooldown can ONLY come from the live-marker-gated
  browse/streams probes (livePath/embed filter internally), so join instead of
  skipping. Verified safe by a 3-lens opus workflow (invariant / regression /
  state machine): all HOLDS -- no marker flicker, no announce spam (setLive
  only fires after first successful poll), no sub-30s loop (waitForNextLive
  clamps to 30s), poll success clears the recently-failed state.
- [x] **Always-on diagnostic logging** (`src/main.cpp`): without `--log-file`,
  logging defaults to `%APPDATA%\Mergerino\Logs\mergerino-diag.log` (append,
  existing 32 MiB cap + `.1` rollover) and raises chatterino.youtube/merged/
  kick/tiktok to Debug + chatterino.app to Info via setFilterRules
  (QT_LOGGING_RULES still overrides). Gated OFF for the browser-extension-host
  process and --version (single opus review caught the concurrent-writer +
  rotation race; that was the only review finding).
- [x] **`.compile-only.bat`** (repo root): vcvars64 + ninja without deploy,
  safe while mergerino.exe runs. Documented in CLAUDE.md gotchas (bare
  `cmake --build` dies with C1083 'type_traits' -- no MSVC env).
- [x] CLAUDE.md: always-on log documented ("check this log FIRST before
  restarting -- restart destroys the repro state"), anomalies-at-qCWarning
  convention. Global ~/.claude/CLAUDE.md: verification fan-outs right-sized
  (single opus reviewer for <~30-line diffs, even in ultracode).
- [x] Verified live: deployed, plain launch (no flags), diag log shows session
  marker + toonyx polling; earlier deploy verified the cooldown fix rejoin.

## Key decisions
- Trust marker-confirmed ids rather than deleting the recently-failed cache:
  the cache still protects against the stale /live-canonical and embed echoes
  (checked inside those probe callbacks); only the redundant outer re-check in
  `resolveVideoId` was removed. Worst-case flap if a marker is wrong: one
  /next fetch per 30s offline poll, self-limiting.
- Default log lives in AppData (not %TEMP%): survives temp cleaners, sits next
  to Settings for bug reports. Dev cycle keeps its explicit `--log-file`.

## Dead ends
- (Prior session, still valid) Watch-page subMenuItem continuations are
  32-char stubs that 400 the poll endpoint -- never poll them directly.

## Files changed
- `src/providers/youtube/YouTubeLiveChat.cpp` -- resolveVideoId joins
  marker-confirmed ids during cooldown (skip block removed)
- `src/main.cpp` -- always-on default diag log + category filter rules
- `.compile-only.bat` -- new; compile without deploy
- `CLAUDE.md` -- logging section rewrite, WARN convention, C1083 gotcha
- `CHANGELOG.md` -- two bullets (cooldown fix, always-on log)

## Current state
- Build: passing; deployed to `C:\Program Files\Mergerino`; app running it
  (plain launch), toonyx joined, diag log active.
- Tests: NOT run this session (compile-only + full ninja only).
- Branch: `main`. Held `review/keychain-ipc-auth` unchanged (SEC-G4 + SEC-02,
  unpushed).

## Next session
1. Still pending from last session: unit test for
   `extractUnselectedViewContinuation` in `tests/src/YouTubeParsing.cpp`
   (pure JSON in/out, like `classifyLivenessDistinguishesNotLiveFromUnknown`):
   two-view selector with full unselected token -> returns it; 32-char stub /
   single item -> empty. Could add a sibling for the marker-gating helpers
   (`jsonContainsLiveMarker` waiting-room vs live fixtures). Then `ctest`.
2. Next time toonyx (or anyone) streams: confirm pickup within ~30s of going
   live with no restart -- grep the diag log for "joining marker-confirmed"
   (fires only if a waiting room preceded the stream) and "setLive true".
