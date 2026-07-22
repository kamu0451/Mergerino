# Handoff

## Goal
"Copy Stream Overlay URL" embedded the tab's notebook index, so reordering /
adding / removing tabs silently pointed the OBS overlay at a different tab's
chat. User proposed a name-derived uuid; implemented a stored per-tab uuid
instead (names collide and break on rename).

## Completed
- [x] **Persistent tab uuid**: `NotebookTab` generates a QUuid (WithoutBraces)
  in its constructor; `uuid()`/`setUuid()` accessors (`setUuid` ignores empty,
  so pre-uuid layouts keep the fresh one). Saved via
  `WindowManager::encodeTab` ("uuid" key), read in
  `TabDescriptor::loadFromJSON`, applied in the layout-restore path.
  Duplicate Tab / popup / drag paths copy splits only -- uuids stay unique.
- [x] **Overlay URL + resolution**: `ObsBrowserDockServer::overlayUrl` now
  takes the uuid; new `resolveTabIndex(tabParam)` resolves the `?tab=` query
  centrally -- empty -> -1 (active tab), integer -> legacy index (old copied
  URLs keep working), otherwise uuid lookup over the main-window notebook,
  unresolvable -> -1 fallback. Overlay page JS forwards the raw string
  instead of parseInt.
- [x] **Adversarial review (single opus): HOLDS.** Handler runs on the GUI
  thread (same as pre-existing dockStateJson); no duplicate-uuid path; QUuid
  hyphens can never parse as the integer branch; no autosave churn; popup
  round-trip never applies uuid.
- [x] Built clean, deployed via `.dev-cycle.bat`, relaunched (PID 19624,
  08:34). Live-verified end-to-end: real uuids resolved to indices 0 and 2,
  bogus uuid + no-param fell back to active tab, legacy `?tab=0/1` pinned.
- [x] **GOAWAY fix acid test PASSED** (yesterday's d5fb6b0, next-session item
  1): all four hourly GOAWAYs in the 18:18-22:2x session logged
  `[timed out, never sent]` WARNs and youtube poll lines continued at a
  steady ~600/half-hour through every one. Poll loops self-heal in prod.

## Key decisions
- Stored random uuid, not name-derived: names collide across tabs and break
  on rename; a constructor-generated uuid survives reorder/rename/restart.
- Kept the numeric `?tab=N` index fallback (one toInt branch) so existing OBS
  browser sources keep working until re-copied.
- Did NOT expose uuids in the dock state JSON; the interactive dock keeps
  using live indices (self-refreshing), only the pinned overlay URL needed
  stable identity.

## Dead ends
- (Prior sessions, still valid) Watch-page subMenuItem continuations are
  32-char stubs that 400 the poll endpoint -- never poll them directly.

## Files changed
- `src/widgets/helper/NotebookTab.{hpp,cpp}` -- uuid member + accessors, menu
  copies overlayUrl(uuid)
- `src/common/WindowDescriptors.{hpp,cpp}` -- TabDescriptor::uuid_ + load
- `src/singletons/WindowManager.cpp` -- encodeTab writes uuid, restore applies
- `src/util/ObsBrowserDockServer.{hpp,cpp}` -- overlayUrl(QString),
  resolveTabIndex, overlay JS passes string through
- `CHANGELOG.md` -- one bullet (overlay URL pins tab, not position)

## Current state
- Build: passing; deployed to `C:\Program Files\Mergerino`; app running
  (PID 19624, launched 08:34 via dev-cycle, logging to
  `%TEMP%\mergerino-dev.log`); window-layout.json already saved with uuids.
- USER ACTION pending: re-copy "Copy Stream Overlay URL" on the stream chat
  tab and update the OBS browser source once (current OBS URL is still the
  positional `?tab=N` form).
- Tests: NOT run for three sessions. `BatchedTimeouts`
  (tests/src/NetworkRequest.cpp:305) is the one to watch (timing-sensitive
  since d5fb6b0).
- Branch: `main`. Held `review/keychain-ipc-auth` unchanged (SEC-G4 + SEC-02,
  unpushed).

## Next session
1. Run `ctest` -- watch `BatchedTimeouts`.
2. Still pending: unit test for `extractUnselectedViewContinuation` in
   `tests/src/YouTubeParsing.cpp` (two-view selector with full unselected
   token -> returns it; 32-char stub / single item -> empty); optional sibling
   for `jsonContainsLiveMarker` waiting-room vs live fixtures.
