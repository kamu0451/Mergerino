# Handoff

## Goal
User reported WebView2 sometimes eating CPU. Attributed it to Mergerino's
hidden TikTok host: the offline-recheck loop full-page-reloaded
tiktok.com/@user/live every ~156s around the clock (794 reloads in one diag
log, 15-35%-of-a-core burst for ~20s each). Fixed by tying the cadence to
sibling-platform live status (user's idea: channels virtually never go live
on TikTok alone).

## Completed
- [x] **Sibling-gated offline recheck**: `TikTokLiveChat` keeps per-consumer
  live hints (`setSiblingLiveHint(consumer, bool)` keyed by MergedChannel
  pointer, OR-aggregated, survives stop/start). Fast 90s cadence while any
  sibling (Twitch/Kick/YouTube) is live, idle 10 min when none. Rising edge
  pokes an immediate recheck (min-gap = fast interval, stamped by every
  NavigationCompleted, guards flapping + mid-load aborts). `MergedChannel`
  votes from every twitchLive_/kickLive_/youtubeLive_ site, seeds after
  creating the provider, removes its vote in the destructor. TikTok-only
  tabs vote "live" (no signal to wait on) so they keep the fast cadence.
- [x] **Resource-blocker re-arm**: filter+handler registration factored into
  `installResourceBlocking()` (remove-then-add filters, handler gated on
  resReqToken==0), called from controller-create AND `autoHideLoginHost`
  (self via new `Impl::owner` back-pointer + propagated hosts), followed by
  a Reload so the running MSE player can't retry-storm 403s. Previously any
  login episode left the hidden renderer decoding TikTok video forever.
  Controller-create now branches on latched login state, not the env var
  (auto-hide broadcast can latch before controller create completes).
- [x] **Opus review (1 agent): 3 defects + 4 nits, all fixed** (TikTok-only
  idle pinning; login-mode race; null impl_ deref in propagate loop; poke
  gap; navigation stamping; mid-playback 403 hedge).
- [x] **MemoryUsageTargetLevel Low** via ICoreWebView2_19 QI at controller
  create (memory lever only, officially safe for hidden-running views).
- [x] **room-info log truncation**: dumps capped at 500 bytes (were 61MB of
  diag log/week; check_alive batches ~306 chars still log whole).
- [x] Deployed (PID 12332) and live-verified: first offline reload came
  exactly 600s after watchdog arm (03:04:40), vs old 156s rhythm.

## Key decisions
- Interval values: fast 90000 / idle 600000 (`Impl::offlineRecheck*Ms`).
  Effective periods include the +66s stuck-timer dance (156s / 666s).
- YouTube counts as a sibling signal (only ever speeds scanning up).
- Did NOT touch: UI-thread webcast frame decode (previous QtConcurrent
  offload broke delivery on some rooms - only revisit on visible stutter),
  dead handleWebMessage branches + unused decodePool (cleanup only),
  IntersectionObserver/visibility spoof (breaking it breaks the scrape).
- Ruled out by docs research: TrySuspend (kills JS/WS), --disable-gpu,
  EcoQoS/priority hacks (unsupported, heartbeat risk).

## Dead ends
- (Prior sessions) Watch-page subMenuItem continuations are 32-char stubs
  that 400 the poll endpoint - never poll them directly.

## Files changed
- `src/providers/tiktok/TikTokLiveChat.{hpp,cpp}` - sibling hints, cadence,
  poke, installResourceBlocking, Impl::owner, mem target, log truncation
- `src/providers/merged/MergedChannel.{hpp,cpp}` - updateTikTokSiblingLiveHint
  + call sites, destructor vote removal
- `CHANGELOG.md` - 3 bullets (cadence major, blocker bugfix, mem/log minor)

## Current state
- Build passing, deployed to `C:\Program Files\Mergerino`, app running
  (PID 12332, 03:20, logging to `%TEMP%\mergerino-dev.log`). wash_fps
  offline; idle cadence confirmed live.
- NOT yet exercised in prod: fast-path rising edge (sibling goes live ->
  `recheck cadence -> fast` debug line + immediate reload) and the login
  re-arm path. Watch the diag log next time the streamer goes live.
- Tests: NOT run for four sessions. `BatchedTimeouts`
  (tests/src/NetworkRequest.cpp:305) is the one to watch.
- Branch `main`. Held `review/keychain-ipc-auth` unchanged (unpushed).

## Next session
1. Check diag log for the first real sibling-go-live: expect cadence->fast
   line, immediate TikTok recheck, and Joined within ~30-60s of Twitch/Kick.
2. Run `ctest` - watch `BatchedTimeouts`.
3. Still pending: unit test for `extractUnselectedViewContinuation`
   (tests/src/YouTubeParsing.cpp); optional `jsonContainsLiveMarker`
   waiting-room fixtures.
