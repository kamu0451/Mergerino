# Handoff

## Goal
Make a tab's YouTube source stay the CHANNEL (user request: "UI always resolves
the channel, under the hood fetch the latest/ongoing live stream") instead of
decaying into an opaque resolved id that pins to an old video.

## Completed
- [x] **Mapped the whole flow first** (workflow, 4 readers): SelectChannelDialog
  normalizes input to @handle/UC-id; MergedChannel's `sourceResolved` write-back
  overwrote the config with the resolved UC-id (shown verbatim on dialog reopen,
  in "Waiting for UC... to go live" chat/status messages, and on the OBS overlay);
  three state-machine defects could pin a tab to a dead video until the URL was
  re-entered.
- [x] **Channel-handle learning** (`21fca67`): new
  `extractYouTubeChannelHandle(html, ownerScopedOnly)` in YouTubeParsing;
  `YouTubeLiveChat` learns the vanity @handle (ctor seed from @-input; owner-
  scoped patterns on watch pages; UC cross-check once resolved) and exposes
  `displaySource()`. Waiting messages + browser URLs use it. MergedChannel
  write-back persists upgrades only - never downgrades an @handle to a UC-id.
- [x] **Stale-stream fixes** (same commit):
  (1) missing-continuation + positively-not-live -> mark id, route to
  verifySource (seamless back-to-back restream switch); verifySource can no
  longer setLive(true) on a recently-failed id -> the infinite false-live loop
  on a dead video is gone.
  (2) `classifyLivenessFromNextResponse` (Live/NotLive/Unknown): only a positive
  not-live signal poisons; transient garbled /next responses no longer lock a
  live stream out (review finding).
  (3) recently-failed streak resets when the videoId changes (a new id inherited
  up to 60 min cooldown).
  (4) probe chain: liveness-unvalidated /live-canonical + embed stages skip a
  recently-failed id and fall through to marker-gated browse/streams probes.
- [x] **Review workflow** (3 lenses + adversarial verify): 4 confirmed findings;
  3 fixed, 1 accepted deliberately (see Key decisions).
- [x] **Deployed + verified live**: PID 24000; all five persisted UC-id sources
  in window-layout.json upgraded to @handles (@gevad1ch, @toonyx4316, @LEDOO_,
  @TheBurntPeanut, @TheCoconutB) within one resolution cycle; the stale toonyx
  waiting room still rejected. 694/694 ctest green (6 new YouTubeParsing tests).

## Key decisions
- Persist the mutable @handle over the immutable UC-id (review flagged the
  handle-recycle wrong-channel risk): ACCEPTED deliberately - Twitch/Kick/TikTok
  tabs are all name-keyed with the identical risk, and the channel-visible field
  is exactly what the user asked for.
- NotLive requires a POSITIVE signal (explicit isLive:false, waiting/scheduled
  markers, or a recognized watch page with no live flag); empty/unrecognized
  JSON is Unknown and routes to verifySource (pre-existing behavior) so
  transients never poison the recently-failed cache.
- Internal `streamUrl_` stays the UC-id for API calls; only display/persistence
  surfaces use the handle.

## Files changed
- `src/providers/youtube/YouTubeParsing.{hpp,cpp}` - handle extractor + liveness tri-state
- `src/providers/youtube/YouTubeLiveChat.{hpp,cpp}` - channelHandle_/displaySource, gate rework, streak fix, probe fall-through
- `src/providers/merged/MergedChannel.cpp` - write-back guard, friendly browser URLs
- `tests/src/YouTubeParsing.cpp` - 6 new tests (handle extraction, owner-scoped, liveness classify)

## Current state
- `main` = `21fca67`, pushed; CI re-triggered by the push (rolling release updates on Build success).
- App running from the new build (PID 24000). Build green, 694/694 ctest.
- Branches: `main` + held `review/keychain-ipc-auth` (SEC-G4 + SEC-02, NOT pushed, needs rebase + fresh-login/YouTube round-trip test + sign-off).
- Plan files (`REMEDIATION-PLAN.md`, `REVIEW-PLAN.md`, `review/`) + scratch logs stay untracked, per instruction.

## Next session
1. Confirm main CI green + rolling release updated (/release-status).
2. Watch a real stream-end/next-stream cycle on a YouTube tab: expect no
   re-entering of the URL; log lines to grep: `probing for the channel's
   current stream`, `probe skipping recently-failed`.
3. Decide on `review/keychain-ipc-auth` (rebase, test, sign-off, push).
