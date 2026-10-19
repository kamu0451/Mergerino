# Handoff

## Next session
1. Look at a busy Kick chat in the installed build and confirm the new
   badges render: 2022 founder (`kick_founder22`), GOAT, custom channel
   badges, lead moderator. Nothing logs badges, so this needs eyes.
2. On kamu: the owner must log into Kick again in Mergerino. Its saved
   token expired 2026-05-16 and every refresh fails 401 (diag log), so
   sending Kick messages from kamu fails. Reading chat is unaffected.

## Current state
- Build: passing. Installed at `C:\Program Files\Mergerino` (fa3d636),
  running on Repe; kamu uses the same install.
- Tests: full `ctest --repeat until-pass:4` = 701/701 passed, 52 skipped
  (need httpbox/pubsub servers, incl. `NetworkRequest.BatchedTimeouts`).
- Branch: `main` = origin/main after the wrap push. CI for c2fc85d was
  green (release 1.6.8); CI for 12d3890/fa3d636 not checked yet.
- Uncommitted: only long-standing untracked working files (logs,
  REVIEW-PLAN.md, REMEDIATION-PLAN.md, `review/`), not from this work.

## In progress
- [ ] Live-unverified: 30 s emote retries (no fetch failed), merged
  rerun marker (no Twitch rerun seen), Pusher key lookup's
  "key changed" branch (Kick's key equals the built-in fallback).
- [ ] Carried over: TikTok grace-expiry / "ended" announce still not seen
  in prod; unit test for `extractUnselectedViewContinuation`
  (tests/src/YouTubeParsing.cpp).

## Goal
Owner streamed on kamu (~16-17 Oct) and Kick chat showed no new
messages while Mergerino showed Kick live. Find and fix, then port the
other worthwhile Kick fixes from upstream Fixlation/Mergerino 1.3.4.

## Completed
- [x] Root cause from kamu's diag log
  (`C:\Users\kamu\AppData\Roaming\Mergerino\Logs\mergerino-diag.log`):
  Kick rotated its Pusher app key; Pusher 4001 + clean close every
  ~1.25 s all stream. Last good run was 1.5.250 on 1 Oct. Not our code.
- [x] c2fc85d: new key + runtime lookup from
  `web.kick.com/api/v1/realtime/connection` (port of upstream 1.3.4).
- [x] Review of upstream 1.3.4 Kick changes: `review/upstream-1.3.4-kick.md`
  (untracked) - 18 changes, port 9 / skip 7 / have 2.
- [x] 12d3890: six small ports (anonymous-as-broadcaster guard, lead-mod
  badge lookup, global + channel emote 30 s retry, merged rerun marker,
  weak emote/badge caches).
- [x] fa3d636: unlisted Kick badges via Kick's image, merged
  badges_v2/badges list, bounded image decode, strict level-badge check,
  4 new tests in `tests/src/KickApi.cpp`.

## Key decisions
- Known Kick badge types keep our bundled icon; only unknown types use
  Kick's image (upstream uses Kick's image for all). Simpler, no fetch.
- Bounded decode is opt-in (`Image::fromBoundedAutoscaledUrl`); other
  images load exactly as before. `readFrames` scales when given bounds
  so it is testable; the RAM check in `actuallyLoad` uses the decoded size.
- Kept our `closing_` heartbeat guard in KickLiveUpdates (upstream lacks it).
- Skipped: upstream's Kick website-session login and everything on it
  (collectibles, cookie hardening), bare-word Kick emote matching, the
  merged WeakMessageIndex rewrite. Reasons in the review file.

## Dead ends
- The 1.6.7 update did not cause the Kick outage; it was just the first
  launch after Kick's key change. Don't bisect our Kick commits for it.
- Tests that build Emote/Badge message elements crash at ctest teardown
  (memory note); the new badge tests deliberately test parsing only.

## Files changed
- `src/providers/kick/KickLiveUpdates.cpp`, `src/providers/liveupdates/BasicPubSub{Client,Manager}.hpp` - key rotation
- `src/providers/kick/{KickApi,KickBadges,KickChannel,KickChatServer,KickEmotes,KickMessageBuilder}.*` - ports
- `src/providers/merged/MergedChannel.cpp` - rerun marker, weak platform badges
- `src/messages/Image.{hpp,cpp}` - bounded decode
- `tests/src/KickApi.cpp`, `CHANGELOG.md`
