# Handoff

## Goal
Fix "YouTube doesn't work" -- toonyx (@toonyx4316) was live but showed no chat
in Mergerino.

## Completed
- [x] **Root-caused with debug logging** (`QT_LOGGING_RULES=chatterino.youtube.debug=true`):
  the channel-source rework from last session is fine -- it correctly resolved
  @toonyx4316 -> UCkc9c90rjpvJ3OBJvTAOKZg -> the live video ER15qGv_iuE and
  polled `ok`. The real bug is one layer down: polling used YouTube's default
  **"Top chat"** view, which drops most messages. On a ~40-viewer stream Top
  chat stayed empty (`actions=0 delivered=0` on every poll for minutes) so the
  tab looked dead though the stream was live and chatting.
- [x] **Fix: switch to the unfiltered "Live chat" view.** The watch page only
  exposes the two views as 32-char placeholder stubs (unpollable -- the poll
  endpoint 400s on them), but the `get_live_chat` *response's* view selector
  carries both as full 312-char reload continuations. New static helper
  `YouTubeLiveChat::extractUnselectedViewContinuation()` reads the unselected
  (Live chat) view's full token; `poll()` switches `continuation_` onto it once
  per video (new `switchedToLiveChatView_` flag, reset on every watch-page
  rebootstrap and in start()/waitForNextLive).
- [x] **Verified live**: after deploy, log shows
  `switching live-chat polling to the unfiltered Live chat view` and
  `delivered` went from perpetually-0 to delivering real messages on toonyx.

## Key decisions
- Switch at the poll RESPONSE, not the watch page: the watch page's subMenuItem
  tokens are 32-char stubs that 400 (confirmed by a failed first attempt); the
  get_live_chat response gives full pollable tokens for both views.
- Pick the view by the `selected` flag (Top chat is selected by default), not by
  the localized "Live chat"/"Top chat" title; require >64-char token and >=2
  views so we never mistake an unrelated single-item menu.
- `switchedToLiveChatView_` resets on every `fetchLiveChatPage` bootstrap
  (always lands on Top chat) + session refresh; a re-switch each time is cheap.

## Dead ends
- Using the watch page's "Live chat" subMenuItem continuation directly: it's a
  32-char stub, poll returns HTTP 400 x3 -> recoverLiveChat -> marks the video
  recently-failed and locks the channel out. Reverted.

## Files changed
- `src/providers/youtube/YouTubeLiveChat.cpp` (+82) -- `extractUnselectedViewContinuation`, the poll() view-switch, flag resets
- `src/providers/youtube/YouTubeLiveChat.hpp` (+11) -- helper decl + `switchedToLiveChatView_`
- `CHANGELOG.md` -- Bugfix bullet

## Current state
- Build: passing (`.local-build.bat`, exit 0), deployed to `C:\Program Files\Mergerino`.
- App running as the deployed fixed binary (debug logging on from testing; a
  normal relaunch drops back to Warning-level -- the fix is in the binary).
- Tests: NOT run this session (needs the test build + httpbox/pubsub services).
- Branch: `main`. Held `review/keychain-ipc-auth` unchanged (SEC-G4 + SEC-02, unpushed).

## Next session
1. Add a unit test for `extractUnselectedViewContinuation` in
   `tests/src/YouTubeParsing.cpp` (pure JSON in/out, like
   `classifyLivenessDistinguishesNotLiveFromUnknown`): two-view selector with a
   full unselected token -> returns it; 32-char stub / single item -> empty.
   Then `ctest` to confirm the suite is still green.
2. Watch a Top-chat-heavy busy stream to confirm the switch also holds there
   (grep `switching live-chat polling`).
