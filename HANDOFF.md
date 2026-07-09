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

## Not started — P6 (needs per-item approval)
TEST-01 (KickMessageBuilder suite, after TEST-04 decoupling), TEST-02
(ProtobufReader), TEST-03 (document 2nd snapshot suite in CLAUDE.md), FEAT-03
(YouTube tab avatar), FEAT-08 (YouTube member-emoji autocomplete), FEAT-04/05/10
(Kick popup-by-ID / isMyself / merged mod-rights), UX-01 (README), UX-02
(Add-Tab live validation).

## Current state
- Build: clean RelWithDebInfo green (final gate ~3m48s). Tests: 627/627.
- Branch `feat/hide-chat-bot-messages` pushed at `8d26e4a`; CI dispatched.
- `review/keychain-ipc-auth` local only (SEC-G4 + SEC-02).
- Plan files + `review/` remain UNTRACKED (never committed), per instruction.
- No orphaned processes/builds.
