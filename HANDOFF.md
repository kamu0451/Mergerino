# Handoff

## Goal
Merge Fixlation's upstream features (1.2.6 -> 1.3.2) into our fork keeping our
work; then security/ToS-review the result and make it Twitch-ToS-safe.

## Completed
- [x] Merged `upstream/main` (Fixlation 1.2.6 -> 1.3.2) into
      `feat/hide-chat-bot-messages`. Fork point was 1.2.5 (`8939dab`). 36 file
      conflicts resolved (parallel agents + manual) keeping OUR TikTok / YouTube-
      stability / MergedChannel / hide-bot / floating-emote / activity-filter
      work and taking upstream's new features. Commit `3f9c880`.
      - Central decisions: kept OUR updater (paired with our release.yml; ported
        upstream's PostUpdateDialog + StreamDatabaseUpdateDialog into our
        UpdateDialog.cpp so they link); took UPSTREAM's chatterino_import; kept
        our CI / version 1.5.0 / CHANGELOG.
- [x] Post-merge fixes from the review (`606d2cc`): merged-highlight regression
      (YouTube/TikTok merged msgs now run highlight/sound/alert/mentions via
      appendMergedMessage), YouTube OAuth token no longer logged to --log-file,
      per-user log path sanitized, dead code removed.
- [x] Security/ToS review (7-finder workflow + adversarial verify): found the
      whole upstream poll/prediction/moderation/badge subsystem uses Twitch's
      PRIVATE gql.twitch.tv API (first-party client-id impersonation, scraped
      session token, KPSDK integrity replay, Android-TV device-code) -> account-
      ban risk.
- [x] Helix rework (`fd34d0c` + `13c3eb6`): routed polls/predictions/viewer-list
      to the official Helix paths that already existed in each file; DELETED the
      entire private subsystem (TwitchWebApi + TwitchModerationAuth ~2130-line
      block in Helix.cpp, both headers, TwitchBadgeIdentity.hpp, CMake entries,
      5 Settings keys, AccountsPage "mod actions" UI, SplitInput badge picker +
      session-token paste flow, CreatePollDialog channel-points GQL fetch).
      Full-tree grep: zero gql.twitch.tv / banned symbols. Runtime log: 0 private
      calls.

## Key decisions
- Helix poll/prediction endpoints are broadcaster-token-only; there is NO Helix
  equivalent for a moderator acting on someone else's channel, and NO Helix API
  for chat-badge select. Those two capabilities were dropped (mod case degrades
  to the Twitch popout / a system message; badge picker removed). Everything
  else stays working via Helix.
- Kept `CurrentUserBadges.hpp` (IRC-USERSTATE-fed, legitimate) so the
  StreamDatabase feed's owned-badge awareness survives without GQL.

## Files changed (this session)
- Merge touched 319 files; the hand-resolved/rewired ones: MergedChannel.*,
  YouTubeLiveChat.*, ChannelView.cpp, Split*.cpp/.hpp, SelectChannelDialog.*,
  SplitSettingsDialog.*, Window.cpp/.hpp, WindowDescriptors.cpp, KickChannel.*,
  UpdateDialog.cpp, GeneralPage.cpp, Application.cpp, main.cpp.
- Helix rework: Poll.cpp, Prediction.cpp, Create/ManagePredictionDialog.cpp,
  CreatePollDialog.cpp/.hpp, ChatterListWidget.cpp, TwitchPollsAndPredictionsBar.*,
  SplitInput.cpp/.hpp, StreamDatabaseBadgeBar.cpp, AccountsPage.cpp/.hpp,
  Helix.cpp, Settings.hpp, src/CMakeLists.txt. Deleted: TwitchWebApi.hpp,
  TwitchModerationAuth.hpp, TwitchBadgeIdentity.hpp.

## Current state
- Build: passing (RelWithDebInfo, exit 0; both mergerino.exe + chatterino-test.exe
  link). Deployed to C:\Program Files\Mergerino, launched clean, 0 crashes,
  7TV/YouTube providers healthy, 0 gql.twitch.tv runtime calls.
- Branch: `feat/hide-chat-bot-messages`, PUSHED to origin (kamu0451) at `13c3eb6`.
- Working tree: only untracked artifacts (.analyze_dump.py, .claudeignore,
  .*-smoke.log, .local-build.log, .tiktok-trace.log) -- none are source.

## Next session
- Verify polls/predictions/viewer-list end-to-end as the BROADCASTER (only
  smoke-tested startup + confirmed no private calls; the Helix on-demand paths
  weren't click-exercised).
- Older emote bugs from the prior session still open (image-fetch poisoning
  Image.cpp:619-630/755-760; Twitch-native all-or-nothing TwitchAccount.cpp:528-553;
  BTTV/FFZ no-retry; build-before-maps race; non-atomic cache write).
