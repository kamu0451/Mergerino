# Handoff

## Goal
Ship the remediation drive: open PR #2 (feat/hide-chat-bot-messages -> main),
deep-review it, fix everything worth fixing, merge to main, and run the
deployed app from main.

## Completed
- [x] **PR #2 created and merged** as `0fecf40` (history rewritten from the
  original `446887a` to scrub a personal author email) — the whole drive (upstream
  1.2.6->1.3.2 merge, Helix ToS rework, P0-P6 remediation; 299 files).
- [x] **Multi-agent review of the full PR diff** (10 subsystem reviewers +
  adversarial verification of all 65 findings): 55 confirmed, 9 refuted,
  1 out-of-scope. Verdict data: scratchpad `done-verdicts.json` /
  `sonnet-verdicts.json` (session temp).
- [x] **Fix round** (4 commits, all built + ctest green before push):
  - `a9631be` providers — Kick history stable sort / newest-own-identity /
    badge tier / emote-map preserved on failure / recent-messages latch;
    YouTube mod latch 401/403-only, missing-continuation backoff 3s->60s,
    scoped viewer count, account leak, redirect `q=` FullyDecoded; merged
    highlight dedupe across shared sources; BTTV+FFZ retry lifetime; 7TV
    paint alignment; HelixChatterGroups deleted; Twitch logout only after
    oauth2/validate 401.
  - `57cd967` UI/core — autoCloseUserPopup rewired (default flipped false),
    TikTok excluded from Twitch delete, theme viewer-count color, SplitInput
    timer gated, mock badges removed, spinbox 0.25 min, YouTube login leak,
    log rollover no longer truncates, cheer != emote-only, hideDeletionActions
    relayout, logging cached set + dot-name rejection, surrogate-safe toasts,
    SPDX headers.
  - `9376112` CI — zip ships `kick_onboarding`; TOS-03 guard fail-open closed
    (sentinels, recursive scan, inline-auth check; verified pass+catch locally
    AND guard step passed on the real runner).
  - `072c2af` tests — 663 -> 687 green; the new redirect test EXPOSED the
    PrettyDecoded production bug (fixed in `a9631be`).
- [x] **Deployed from main**: .dev-cycle.bat rebuilt + relaunched
  `C:\Program Files\Mergerino\mergerino.exe` (v1.5.234, main @ merge commit).

## Deliberately NOT fixed (upstream-inherited; forward to Fixlation)
- CurrentUserBadges.hpp ~700-line dead registry (currentUserHasBadge() never
  true); StreamDatabase intro gated on hardcoded "1.3.2" (unreachable);
  CreatePollDialog dead channel-points icon fetch; 7TV badge refresh became a
  no-op invalidate (upstream 1.3.2 simplification); anon Helix credential
  fallback (TwitchAccountManager.cpp:469 — design decision, documented risk).

## Test-coverage follow-ups (known, not urgent)
- No tests: ChatterinoImport pipeline (~770 lines, destructive), StreamDatabase
  parseEventBadges (~500 lines), UpdateDialog patch-notes parsing.

## HELD FOR SIGN-OFF — local branch `review/keychain-ipc-auth` (NOT pushed)
- `5f2b0cb` SEC-G4 (IPC queue name + session secret), `efac95c` SEC-02
  (tokens -> QtKeychain; verified live on a real Kick account).
- NOTE: branched pre-P3; main has moved ~30 commits since — needs a rebase
  onto `main` and a fresh-login + YouTube round-trip test before push.

## Key decisions
- PR review scope was the three-dot diff (GitHub refused to serve it — 20k-line
  cap; generated locally with `git diff origin/main...HEAD`).
- autoCloseUserPopup: upstream 1.3.0 hardcoded false; we rewired the setting
  and flipped its default to false so shipped behavior is unchanged.
- Workflow fan-outs now route mechanical stages (verify/dedup) to sonnet —
  user preference, saved as memory `workflow-model-routing`.

## Current state
- Build: green (RelWithDebInfo, ninja). Tests: 687/687 via ctest.
- `main` head rewritten + force-pushed (email scrub); local main in sync.
  App running from main.
- CI on main (Build + Test) was IN PROGRESS at wrap; on Build success the
  rolling `latest` release updates automatically. Check with /release-status.
- Plan files + `review/` remain UNTRACKED (never committed), per instruction.
- No orphaned processes; scratch logs cleaned.

## Next session
1. Confirm main CI went green and the rolling release updated
   (/release-status).
2. Decide on `review/keychain-ipc-auth` (SEC-G4 + SEC-02): rebase onto main,
   fresh-login + YouTube round-trip test, then sign-off + push.
3. Optionally send Fixlation the second findings list (the upstream-inherited
   defects catalogued above + in the PR #2 review).
