# Handoff

## Goal
toonyx went live again and the YouTube tab still didn't join (report: "still not
loading if mergerino is open when the live starts") -- despite yesterday's
cooldown fix. Find and fix the real remaining cause.

## Completed
- [x] **Root-caused via the always-on diag log** (added yesterday; paid off
  immediately): YouTube's servers send an HTTP/2 GOAWAY exactly 1h after each
  connection opens (`qt.network.http2.connection ... GOAWAY invalid
  stream/error code (1)` CRIT at app-start+1h in every session). A request
  assigned to the dying connection can emit NEITHER requestSent NOR finished
  NOR any error -- a zombie reply. `NetworkTask` armed its timeout QTimer only
  inside a `requestSent` handler, so zombie tasks lived forever, their
  onSuccess/onError/finally never ran, and the callback-driven YouTube poll
  loop died silently. Log evidence: each hourly GOAWAY permanently killed the
  poll loop of whichever channels had a probe in flight that second; toonyx's
  died at 09:40, hours before the 17:46 live. Yesterday's cooldown fix was
  correct but sat above this.
- [x] **Fix** (`src/common/network/NetworkTask.cpp` + `.hpp`): arm the
  single-shot watchdog immediately in `run()`, restart it on `requestSent`
  (queue wait and on-wire time each get the full budget), track
  `requestSentSeen_`; `timeout()` logs qCWarning `[timed out, never sent]`
  for never-sent zombies (lands in the diag log), qCDebug otherwise. Covers
  every timeout-bearing request app-wide; all YouTubeLiveChat requests set
  15-20s timeouts, so both the offline resolve loop and live polling self-heal.
- [x] **Adversarial review (single opus, per the <30-line-diff rule): HOLDS.**
  Verified: worker-thread event loop exists (timer fires); abort() on a
  never-sent reply is safe; no double-emit (timeout/finished serialize on one
  thread, each disables the other first); old code leaked one QTimer per
  redirect hop, new code is strictly better; image/emote floods immune (they
  set no timeout, `Image.cpp:707` -- timer never armed).
- [x] Built (`.compile-only.bat mergerino-lib`), deployed via `.dev-cycle.bat`,
  relaunched (PID 21888, 18:18): toonyx YouTube joined 2s after launch
  (videoId ddwlYVq1f7Q "Rust Savant", setLive true, polling).

## Key decisions
- Fixed at the NetworkTask layer, not with a YouTubeLiveChat-level watchdog:
  every request in the chain already has a timeout, so arming it correctly
  revives all loops; a provider watchdog would be redundant.
- Queue-wait now counts against the timeout (reset on send). Reviewed as the
  one real semantic change: low risk (h2 hosts multiplex; the flood path sets
  no timeout). Canary if it ever bites: `tests/src/NetworkRequest.cpp:305`
  `BatchedTimeouts` -- now timing-sensitive (~500ms margin), could go flaky
  on loaded CI.

## Dead ends
- (Prior sessions, still valid) Watch-page subMenuItem continuations are
  32-char stubs that 400 the poll endpoint -- never poll them directly.

## Files changed
- `src/common/network/NetworkTask.cpp` -- watchdog armed in run(), restarted
  on requestSent; never-sent timeouts warn
- `src/common/network/NetworkTask.hpp` -- `requestSentSeen_` member
- `CHANGELOG.md` -- one bullet (hourly poll-loop death)

## Current state
- Build: passing; deployed to `C:\Program Files\Mergerino`; app running it
  (launched 18:18 via dev-cycle, so logging goes to
  `%TEMP%\mergerino-dev.log`), toonyx joined and polling.
- Tests: NOT run this session. `BatchedTimeouts` is the one to watch.
- Branch: `main`. Held `review/keychain-ipc-auth` unchanged (SEC-G4 + SEC-02,
  unpushed).

## Next session
1. **Verify the GOAWAY fix survived the 1h mark**: the running session started
   18:18, so ~19:18 the hourly GOAWAY hit. Grep `%TEMP%\mergerino-dev.log` for
   `GOAWAY` CRITs and confirm youtube poll lines CONTINUE after them (and/or a
   `[timed out, never sent]` WARN followed by recovery). That is the acid test
   the fix was built for.
2. Run `ctest` (not run for two sessions now) -- watch `BatchedTimeouts`.
3. Still pending: unit test for `extractUnselectedViewContinuation` in
   `tests/src/YouTubeParsing.cpp` (two-view selector with full unselected
   token -> returns it; 32-char stub / single item -> empty); optional sibling
   for `jsonContainsLiveMarker` waiting-room vs live fixtures.
