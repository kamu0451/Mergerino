# Handoff

## Goal
User reported repeated "TikTok live chat disconnected" messages while the
TikTok stream (wash_fps) was live and viewer counts kept updating.
Diagnosed from the diag log: TikTok's server drops the webcast socket
(1006, no close handshake, frames thin out ~20s before) every ~1.5-3.5 min
into a healthy session; the recheck rejoined ~92s later but every drop was
announced while the same-roomId rejoin stayed silent (MergedChannel Joined
announce is latched per roomId). Fixed with a disconnect-announce debounce.

## Completed
- [x] **Disconnect-announce grace** (`TikTokLiveChat.cpp`): ws-close on a
  live session sets silent status "reconnecting...", starts
  `Impl::disconnectAnnounceTimer` (180s), and pins the recheck to the fast
  cadence regardless of sibling votes (a just-dropped live room is proof
  the streamer is broadcasting). setLive(true) cancels the grace (WARN
  "reconnected within the grace period"); expiry announces "disconnected",
  sets `disconnectAnnounced`, and re-arms normal cadence. A later same-room
  rejoin then announces "TikTok live chat reconnected".
- [x] **Definitive verdicts win over the grace**: the 30003/"room has
  finished" branch was HOISTED above the prompts branch in handleRoomInfo
  (the real end payload `{message:"room has finished",prompts:"This LIVE
  has ended"}` matches BOTH; prompts used to win and answer "is not live").
  Finished/prompts/array-form-offline/stuck-watchdog all stop the grace,
  clear `disconnectAnnounced`, and announce their own verdict ("TikTok
  live chat ended" on wasLive || gracePending).
- [x] **Spam fixes**: ws-close while not live (sidebar sockets on offline
  pages) no longer announces; ws-open only sets "Connected to TikTok" when
  status is "Connecting to TikTok..." (was resetting the dedupe key ->
  "is not live" re-announced every recheck cycle); ws-error is silent
  while a grace is pending; refreshOfflineRecheckCadence won't slow to
  idle mid-grace.
- [x] **MemoryUsageTargetLevel Low REVERTED (A/B result: innocent)** -
  drops continued without it (1006 at 13:20:35 on the new build). The
  drops are TikTok-side for hidden clients; debounce is the fix.
- [x] Opus review (1 agent) of the first cut: 2 defects (prompts branch
  shadowing the finished branch; fixed grace vs 10-min idle cadence) +
  4 nits - all fixed.
- [x] **Live-verified end to end** (PID 27432, 13:15): drop 13:20:35 ->
  forced-fast reload 13:22:05 (+90s exactly) -> rejoin 13:22:06, grace
  cancelled, nothing announced in chat.

## Key decisions
- Grace = 180s (`Impl::disconnectAnnounceGraceMs`): covers one fast
  recheck (90s) + load + 60s stuck-connection window.
- `Impl::disconnectAnnounced` flag pairs an announced disconnect with a
  "reconnected" announce; cleared by every verdict branch so a NEW session
  after a real end says "Joined", not "reconnected".
- Yesterday's sibling-gated cadence + rising-edge poke verified working in
  prod (cadence -> fast at 12:10:26, joins within ~92s of each drop).

## Dead ends
- Do NOT re-suspect MemoryUsageTargetLevel Low for the socket drops - A/B
  proved them present without it. Comment at the former call site.
- (Prior sessions) Watch-page subMenuItem continuations are 32-char stubs
  that 400 the poll endpoint - never poll them directly.

## Files changed
- `src/providers/tiktok/TikTokLiveChat.cpp` - all of the above
- `CHANGELOG.md` - 2 bullets (debounce bugfix, mem-target revert)

## Current state
- Built, deployed, running (PID 27432 since 13:15, logging to
  `%TEMP%\mergerino-dev.log`); wash_fps live, drop/rejoin cycle silent.
- Not yet observed on this build: grace EXPIRY (real 3-min outage), the
  "ended" announce on a genuine stream end, "reconnected" pairing. Watch
  the diag log when the stream actually ends: expect ONE "TikTok live chat
  ended" in chat, no trailing "disconnected".
- Tests: NOT run for five sessions. `BatchedTimeouts`
  (tests/src/NetworkRequest.cpp:305) is the one to watch.
- Branch `main`. Held `review/keychain-ipc-auth` unchanged (unpushed).

## Next session
1. Check the diag log for how the live session ENDED - the ended/grace
   paths are code-reviewed but not yet exercised in prod.
2. Run `ctest` - watch `BatchedTimeouts`.
3. Still pending: unit test for `extractUnselectedViewContinuation`
   (tests/src/YouTubeParsing.cpp); optional `jsonContainsLiveMarker`
   waiting-room fixtures.
