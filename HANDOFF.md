# Handoff

## Goal
Post-merge hygiene + a live bug: audit the repo for private/secret content,
scrub a leaked personal email from history, clean up branches, and fix the
"YouTube channel stays live after the stream ends" bug.

## Completed
- [x] **Privacy audit of tracked content**: plan files (`REMEDIATION-PLAN.md`,
  `REVIEW-PLAN.md`, `review/`), scratch logs, and `.analyze_dump.py` confirmed
  untracked; no secrets/keys/.env anywhere tracked. Removed the one personal
  path (`C:\Users\Repe\...`) from a TikTokFrameDecoder comment.
  `resources/kick_onboarding/client-credentials.png` shows a Kick Client ID --
  user confirmed it is not theirs, left as-is.
- [x] **Email scrub via history rewrite**: the PR #2 merge commit carried
  `nappihaukka+aigit@gmail.com`. Rewrote main from the merge forward with
  identical trees (merge is now `0fecf40`, GitHub GPG signature dropped),
  force-pushed with lease. Old objects remain sha-addressable on GitHub until
  GC; user should enable "Block command line pushes that expose my email".
- [x] **Branch cleanup**: deleted `feat/hide-chat-bot-messages` (merged) and
  `fix/crash-on-close` local+origin. Before deleting the latter, ported its
  still-valid half to main as `3ad0e54` (shutdown guard in
  `SplitHeader::updateIcons` -- use-after-free on quit); its RunGui half is
  obsolete (that recovery block no longer exists on main).
- [x] **YouTube stuck-live fix** (`352e273`): an ended stream can revert to a
  scheduled *waiting room* whose `videoViewCountRenderer` still says
  `isLive:true` AND whose chat serves get_live_chat continuations, so the
  /next gate passed, the first poll succeeded, and the channel latched live
  with a phantom "1 viewer" (really "1 waiting"). Confirmed by fetching the
  real watch page of stuck videoId `0tBLmmIWrTQ` (`isUpcoming:true`,
  `isLiveNow:false`, counter "1 waiting", dateText "Scheduled for ...").
  `extractIsLiveFromNextResponse` (src/providers/youtube/YouTubeParsing.cpp)
  now rejects "waiting" counter text, "Scheduled for" dateText, and
  `videoDetails.isUpcoming`. Regression tests use the captured shape.
- [x] **Deployed + verified live**: .dev-cycle.bat relaunched the app
  (PID 928604); fresh log shows `fetchLiveChatPage rejecting non-live
  videoId="0tBLmmIWrTQ"` while a genuinely-live channel still connects.

## Key decisions
- History rewrite done with `git commit-tree` on identical trees (same
  messages/dates), so content is byte-identical -- only identities changed.
- Waiting-room detection uses en-locale strings ("waiting", "Scheduled for");
  safe because the InnerTube context pins hl=en and headers pin en-US.
- Left the 10-min `YOUTUBE_SESSION_REFRESH_MS` re-validation cadence: with the
  gate fixed, an in-session zombie end clears within one refresh; the
  stuck-forever behavior was the bug, not the lag.

## Deliberately NOT fixed (upstream-inherited; forward to Fixlation)
- CurrentUserBadges dead registry; StreamDatabase intro gated on hardcoded
  "1.3.2"; CreatePollDialog dead icon fetch; 7TV badge-refresh no-op; anon
  Helix credential fallback (TwitchAccountManager.cpp:469).

## HELD FOR SIGN-OFF -- local branch `review/keychain-ipc-auth` (NOT pushed)
- `5f2b0cb` SEC-G4 (IPC queue name + session secret), `efac95c` SEC-02
  (tokens -> QtKeychain). Needs a rebase onto main and a fresh-login +
  YouTube round-trip test before push.

## Current state
- Build: green (RelWithDebInfo, ninja). Tests: 688 green via ctest (17
  YouTubeParsing incl. the new waiting-room case; standard 6 network skips).
- `main` = `352e273`, pushed. App running from it (PID 928604).
- Branches: only `main` + held `review/keychain-ipc-auth` remain.
- CI on main re-triggered by the pushes; rolling `latest` release updates on
  Build success (/release-status to confirm).
- Plan files + `review/` + scratch logs remain untracked, per instruction.

## Next session
1. Confirm main CI green + rolling release updated (/release-status).
2. Decide on `review/keychain-ipc-auth`: rebase, test, sign-off, push.
3. Watch whether any YouTube channel still latches live on a waiting room
   (log line to grep: `rejecting non-live videoId`).
