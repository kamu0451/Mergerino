# Handoff

## Goal
Diagnose why 7TV / custom emotes "keep not working" and why Twitch channel
emotes sometimes don't show up at all.

## Completed
- [x] Root-caused the 7TV failure: a `getSeventvJson()` regression in commit
      b909cd9 dropped the trailing `.execute()`, so EVERY 7TV fetch (Twitch
      channel via getUserByTwitchID, Kick channel via getUserByKickID, global +
      personal via getEmoteSet) was built and silently destroyed -- no HTTP
      call, no onSuccess, AND no onError, so no "Failed to fetch 7TV" ever
      surfaced. Channels showed only the on-disk cache, or nothing when uncached.
- [x] Fixed it: appended `.execute()` to the `getSeventvJson` chain.
- [x] Restored the guard that would have caught it: `~NetworkRequest` was changed
      to `= default` even though its header comment still promised an assert;
      restored `assert(!this->data || this->executed_)` (debug-only, no release cost).
- [x] Built (RelWithDebInfo, exit 0), deployed to C:\Program Files\Mergerino,
      relaunched. VERIFIED end-to-end: log now shows live
      `https://7tv.io/v3/users/twitch/...` and `.../kick/...` success handlers
      firing -- those requests were never sent before the fix.
- [x] Ran a 7-subsystem diagnostic workflow (each finding adversarially
      re-verified) to map ALL emote-loading failure modes (see below).

## Key decisions
- Committed ONLY this session's actual work (SeventvAPI.cpp, NetworkRequest.cpp,
  CHANGELOG.md, this file). The other 6 dirty files (KickApi.cpp, EmotePopup.cpp,
  ChannelView.cpp/.hpp, SplitInput.cpp, CLAUDE.md) are pre-existing WIP from prior
  sessions and were left untouched/uncommitted, per the established convention.

## Files changed (this session)
- `src/providers/seventv/SeventvAPI.cpp` -- `.execute()` added to getSeventvJson.
- `src/common/network/NetworkRequest.cpp` -- restored forgotten-execute() assert.
- `CHANGELOG.md` -- Bugfix entry under Unversioned.

## Current state
- Build: passing (exit 0). Deployed + running with the fix; 7TV fetches confirmed live.
- Branch: `feat/hide-chat-bot-messages`.
- Working tree still has the 6 pre-existing WIP files dirty + untracked artifacts
  (.analyze_dump.py, .claudeignore, .local-build.log, .tiktok-trace.log) -- all
  deliberately left out, not this session's work.

## Next session (remaining emote bugs -- NOT fixed by the 7TV fix)
These are confirmed, separate from H1, and explain "even channel emotes sometimes
don't show up at all". Recommend fixing the first two next:
1. Image-fetch poisoning (best explanation for native emotes vanishing): a transient
   emote-IMAGE download failure sets `Image::empty_` permanently with no retry, the
   GC (`freeOld`) skips empty-frame images so it never re-arms, and the renderer then
   draws the emote as grey TEXT. URL-keyed + shared process-wide -> one blip kills
   that emote (incl. Twitch native) until restart. `Image.cpp:619-630`,
   `Image.cpp:755-760`, `MessageElement.cpp:250-265`.
2. Twitch native emote-set load is all-or-nothing: `TwitchAccount::reloadEmotes`
   commits only after the LAST page; any page failure discards everything, no retry
   -> empty picker/autocomplete until F5/re-login. `TwitchAccount.cpp:528-553`.
3. BTTV/FFZ have no transient-timeout retry (7TV does), and the per-tab "Reload
   emotes" header action no-ops on merged tabs (dynamic_cast to TwitchChannel*/
   KickChannel* misses MergedChannel). `BttvEmotes.cpp:280`, `FfzEmotes.cpp:273`,
   `SplitHeader.cpp:1736`.
4. Build-before-maps race: messages built before the async emote map loads stay
   plain text forever (set*Emotes just .set() with no rebuild/signal); MergedChannel
   clones that frozen state. `MessageBuilder.cpp:460-528`, `TwitchChannel.cpp:481-494`.
5. `OperationCanceledError` is silently swallowed in `NetworkTask::finished`
   (returns before emitError/emitFinally), which also makes the 7TV cancel-retry
   dead code. `NetworkTask.cpp:264-271`. Also worth a quick check: the global
   `/emotes/enableEmoteImages` setting (default on) renders ALL emotes as text if off.
6. Cache write is non-atomic (truncate-in-place, no QSaveFile) and a corrupt/partial
   cache is applied as an empty map with cacheHit=true -- latent, lower priority.
   `util/Helpers.cpp:392-442`.
