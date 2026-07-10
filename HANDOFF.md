# Handoff

## Goal
Execute the phased remediation of the Mergerino full-project review
(`REMEDIATION-PLAN.md`, driven by `review/FINDINGS.md` — both UNTRACKED, local
only). Scope this drive: P0-P5. P6 (features/UX) is NOT started — needs per-item
sign-off.

## Completed this drive (branch `feat/hide-chat-bot-messages`, pushed to origin)
All groups: clean RelWithDebInfo build green + full GoogleTest suite green
(594 -> 627 tests; 33 added) + idle runtime smoke clean. One coherent commit per
group, finding IDs in messages.

- **P0** `bb92179` — SEC-01 (Kick token-log redacted), LOG-03/SEC-03 (YouTube
  refresh log redacted), FEAT-06 (Kick Block checkbox hidden), SEC-G1
  (BoostJsonWrap `(*a)[i]` + test), SEC-G2 (Kick timeout clamp + test), STAB-G3
  (assert -> guard). TESTFIX-01 was already `6f5ab09`.
- **P1** `aae499d` — STAB-01/03 (atomic cache + test), STAB-02 (image transient
  retry + test), STAB-04 (EventSub/IRC deletion built-then-hidden + test),
  STAB-05 (BTTV/FFZ fetch retry + test), STAB-06 (Twitch emote pagination
  resilient), STAB-G1 (7TV throttle armed at request), STAB-G2 (Pusher no
  ping-after-close).
- **P2 (pushed)** `f4f0f65 SEC-06`, `a76ed3e TOS-01`, `6806aad TOS-02`,
  `8c10eba TOS-03`, `5ffd4aa SEC-04`, `a97444f SEC-05`. Each its own commit.
- **P3** `b5d08e8` (DEAD-01 crashpad removal, DEAD-02/03/04),
  `02ded07` (SIMP-01 announce dedup, SIMP-03 YouTubeParsing extract + tests),
  `bc8fca2` (SIMP-02/DEAD-05 shared BurstRateLimiter + tests).
- **P4** `3ad90e7` — BLD-01 (sol2 out of default PCH), BLD-05 (sccache doc).
- **P5** `8d26e4a` — LOG-01 (MergedChannel diagnostics, verified live via
  `QT_LOGGING_RULES=chatterino.merged.debug=true`), LOG-02 (TikTok ws warnings),
  LOG-04 (32 MiB `--log-file` cap + `.1` rollover).

## HELD FOR YOUR SIGN-OFF — local branch `review/keychain-ipc-auth` (NOT pushed)
Two auth-changing P2 items, committed locally only, awaiting diff review before push:
- `5f2b0cb` **SEC-G4** — native-messaging IPC per-install queue name + per-session
  secret handshake (+ tests).
- `efac95c` **SEC-02** — Kick/YouTube tokens + Kick clientSecret moved to
  QtKeychain (Windows Credential Manager), async account load + per-secret
  plaintext migration. VERIFIED LIVE: an existing Kick account migrated (its
  secrets left settings.json for the Credential Store) and restored from the
  keychain across a restart. Still needs a fresh-login + YouTube round-trip
  before push. `git diff a97444f review/keychain-ipc-auth` to review.

## Reverted / deferred (with reasons)
- **TOS-05** (Helix 429 Retry-After) — REVERTED. The drafted retry wrapper never
  read Retry-After (NetworkResult exposes no headers) and introduced a UAF across
  every Helix call + a `finally` double-fire. Needs response-header surfacing;
  deferred.
- **BLD-02** (split WebSocketConnectionImpl.cpp) — REVERTED. A transport split
  isolates only the cheap TCP TU (~24s); the TLS boost::beast::ssl instantiation
  stays a ~45s single TU, so clean-build wall time was unchanged and total work
  rose. Real fix = extern-template firewall on the TLS stream types (larger).
- **BLD-03** (IApplication interface split) — deferred, too invasive (~167-TU
  blast radius) for this pass.
- **SEC-G3** (plugin FS canonicalization) — optional, skipped.

## P6 — DONE (user approved all items on this drive; pushed)
Additive features/tests, all built + 663 tests green + smoke clean.
- **P6 tests** `85c2b40` — TEST-01 first Kick tests (7 KickMessageBuilder cases
  via mock app; no TEST-04 decouple needed. The chat case uses plain text: an
  Emote/Badge element registers an Image with the process-global pool which
  tears down out of order in ctest's per-test process and crashes it, so
  element/badge indexing stays covered by the snapshot suites). TEST-02 27
  direct ProtobufReader cases incl. bounds-safety.
- **P6 docs** `5e8198f` — TEST-03 (document the EventSub snapshot suite),
  UX-01 (README Usage/Features section).
- **P6 features** `bd609a6` — UX-02 (Add-Tab live validation reusing the OK
  parser), FEAT-05 (Kick isMyself wired to the real current account), FEAT-04
  (open Kick card by id; dormant path, degrades logged-out), FEAT-10 (YouTube in
  merged mod-rights).
- **P6 YouTube** `57ba59c` — FEAT-03 (YouTube tab avatar, anonymous channel-page
  og:image fetch via youtubeHeaders(), same downloadAvatarInto/fallback path;
  bare watch-URL/videoId sources fall through), FEAT-08 (accumulate custom
  YouTube chat emoji into a bounded per-source EmoteMap, exposed to autocomplete
  via EmoteSource). Read path stays anonymous (TOS-03 guard passes).

## Current state
- Build: clean RelWithDebInfo green. Tests: 594 baseline -> 663 (69 added).
- Branch `feat/hide-chat-bot-messages` pushed at `57ba59c`; CI dispatched
  Full P0-P6 HEAD CI-green (Build + Test Windows).
- `review/keychain-ipc-auth` local only (SEC-G4 + SEC-02) — still awaiting
  sign-off before push.
- Plan files + `review/` remain UNTRACKED (never committed), per instruction.
- No orphaned processes/builds.

## Post-P6 follow-ups (this session)
- **Changelog-dialog fix** `5ff45a3` (pushed) — the post-update patch-notes
  dialog re-showed on every launch because the version PATCH is
  `git rev-list --count HEAD` (bumps per commit). Now gated on a SHA-1
  fingerprint of the latest patchnotes.txt section (new setting
  `lastShownPatchNotesFingerprint`): shows only when the notes actually change.
  Verified via window-count (stale fp -> dialog; matching fp -> suppressed).
- **Live-test pass** (~4.5h, YouTube-heavy + brief Kick): app stable, zero
  crashes. All log flags were benign network/lifecycle churn (HTTP/2 GOAWAY,
  websocket close-fails, 7TV reconnects ~1/15min, stream end/recover). Positive
  evidence STAB-G2 (no Pusher ping-after-close) and YouTube recoverLiveChat work.
- **Two minor Kick gaps found (candidates, not defects):** Kick Pusher events
  `StreamerIsLive` / `StopStreamBroadcast` are unhandled ("Unknown event") — Kick
  live/offline is detected via polling, not the instant push, so there's a small
  delay. Optional enhancement.
- **Optional cosmetic:** the generic-websocket "Failed to close" line logs at
  WARN on every teardown (fires ~1/15min from 7TV reconnects); could be debug.
- Session lessons captured as memories (ctest-authoritative, emote/badge test
  teardown crash, CI feature-branch dispatch) + gotchas (git stale lock, guard
  false-positives, C++ LSP phantom diagnostics).
- **Security-trace cleanup** — deleted `.tiktok-trace.log` (stale May-13 TikTok
  trace holding 51 `refresh_token` fields) + all this session's scratch logs.
  Confirmed no plaintext tokens remain: settings.json has none (the SEC-02
  verification run migrated the Kick secrets into the Windows Credential Manager
  and erased the plaintext; no YouTube account is stored). Consequence: the
  deployed feat build can't read those keychain entries, so the Kick account may
  show logged-out until SEC-02 is deployed. Remaining untracked clean scratch
  logs (`.merge-smoke*.log`, `.rework-smoke.log`, `.local-build.log`) left in place.
- Produced a **security/ToS fix list for upstream (Fixlation)** summarizing the
  SEC-*/TOS-* items so his agent can mirror them.
- **Verified the Fixlation list against `upstream/main`** (Fixlation/Mergerino,
  latest 1.3.2 `dd18649`): every item is real in his tree verbatim — SEC-01
  (`KickLoginPage.cpp:1255/1295/1318`), LOG-03/SEC-03 (`YouTubeAccount.cpp:271`),
  SEC-02 (`KickAccount.cpp` uses `QStringSetting`, bypasses the existing keychain),
  SEC-G1 (`BoostJsonWrap.cpp` `return {a[i]}`), SEC-G2 (`KickMessageBuilder.cpp:951`
  `dur*60`), STAB-G3 (`KickChannel.cpp:849/857` assert), SEC-G4 (hardcoded
  `mergerino_gui` queue, no secret), SEC-04/05 (no TikTok nav-gating / no updater
  hash), SEC-06 (`.gitignore` pins only the exact secrets path). TOS-01/02 missing;
  TOS-03 is preventive (read path already anonymous — the ask is a CI guard); Helix
  429 correctly flagged as not-done. The leaked plaintext token *data* was
  local-only (never in any repo); the vulnerable *code* is shared upstream.
