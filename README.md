# Mergerino

Mergerino is a desktop chat client that combines a fast multi-channel workflow with support for Twitch, Kick, YouTube, and TikTok LIVE in one app. The goal is to keep chat fast and practical while making multi-platform viewing easier from a single desktop client.

This repository keeps the source here and ships installable Windows builds through GitHub Releases.

Current version: Mergerino 1.3.0

[![Download Windows Build](https://img.shields.io/badge/Download-Windows%20Build-2ea44f?style=for-the-badge)](https://github.com/Fixlation/Mergerino/releases/latest/download/Mergerino.zip)
[![All Releases](https://img.shields.io/badge/GitHub-Releases-1f6feb?style=for-the-badge)](https://github.com/Fixlation/Mergerino/releases)

## What Is Mergerino?

- Merged desktop chat for Twitch, Kick, YouTube, and TikTok LIVE
- Fast multi-channel chat workflow in a standalone app
- 7TV, BTTV, and FFZ integration
- Windows-only builds packaged around `mergerino.exe`

## Download

If you just want to install the app, do not download the source code zip from the repository page.

Use the latest GitHub release instead:

- Open [the releases page](https://github.com/Fixlation/Mergerino/releases)
- Download [Mergerino.zip](https://github.com/Fixlation/Mergerino/releases/latest/download/Mergerino.zip)
- Extract it somewhere you want to keep it
- Open `mergerino.exe` from inside the extracted folder

## Install

### Windows

1. Download `Mergerino.zip`
2. Extract the zip
3. Open the extracted `Mergerino` folder
4. Run `mergerino.exe`
5. Keep the `.exe` inside that folder with the bundled files

## Build Your Own

Mergerino currently supports Windows only.

Clone with submodules:

```shell
git clone --recurse-submodules <your-mergerino-repository-url>
```

If you already cloned without submodules:

```shell
git submodule update --init --recursive
```

## Privacy Policy

<sub>Effective date: May 14, 2026</sub>

<sub>Mergerino is a Windows desktop chat client. It does not operate a hosted backend for Google or YouTube login. When you sign in with Google, Mergerino opens Google's OAuth flow and uses the YouTube Data API directly from your desktop device so YouTube chat features can work inside the app.</sub>

<sub>Mergerino requests the `https://www.googleapis.com/auth/youtube.force-ssl` OAuth scope because YouTube requires an authenticated YouTube Data API scope for the live chat write and moderation actions Mergerino provides. This scope may allow broader YouTube account permissions, but Mergerino uses it only for the YouTube chat features described here.</sub>

<sub>Depending on which YouTube features you use, Mergerino may access or process OAuth authorization data, access tokens, refresh tokens, token expiration time, OAuth client ID, your authenticated YouTube channel ID and display name, live stream and live chat identifiers, stream title, stream start time, viewer count, thumbnail or preview URL, owner channel ID, displayed live chat messages, message IDs, author names, author channel IDs, message text, timestamps, badges, emoji, membership events, Super Chat or sticker details, messages you choose to send, and moderation data for actions you choose to perform.</sub>

<sub>Mergerino uses Google user data only to connect your YouTube account, keep you signed in locally, find and display the active live chat for YouTube streams you open, send messages you submit, perform moderation actions you explicitly choose, and show related YouTube chat status or error messages.</sub>

<sub>Mergerino stores the connected YouTube account's display name, channel ID, OAuth client ID, access token, refresh token, and token expiration time locally in the app's settings on your device. Live chat messages and stream metadata are displayed in the app while you use YouTube chat features and are not uploaded to a Mergerino server. Ban IDs returned by YouTube may be cached in memory during the current app session so Mergerino can unban users it banned during that same session.</sub>

<sub>Google user data is sent directly between your desktop device and Google or YouTube services to provide the YouTube features you request. Mergerino does not operate a server that receives your Google OAuth tokens or Google user data. The Mergerino developer does not receive, store, sell, rent, or share your Google OAuth tokens or Google user data.</sub>

<sub>Mergerino does not transfer Google user data to advertising platforms, data brokers, information resellers, analytics providers, or other third parties. It does not use Google user data for advertising, tracking, credit decisions, lending, data brokerage, unrelated analytics, or training generalized AI or machine learning models. It does not intentionally access, upload, edit, or delete YouTube videos, ratings, comments outside live chat, captions, playlists, subscriptions, or analytics.</sub>

<sub>Mergerino sends Google OAuth and YouTube Data API requests over HTTPS. OAuth token request bodies are hidden from Mergerino's network request logging. Google OAuth tokens are stored locally on your device and are not transmitted to the Mergerino developer. You are responsible for protecting your Windows account, local device, backups, and any copied Mergerino settings files.</sub>

<sub>You can revoke Mergerino's Google access at any time from your Google Account permissions page: https://myaccount.google.com/permissions</sub>

<sub>For privacy or support questions, open an issue on this GitHub repository or contact the maintainer via Discord or Twitter/X.</sub>

## Terms of Service

<sub>Effective date: May 11, 2026</sub>

<sub>Mergerino is provided as an unofficial desktop chat client for accessing supported live chat platforms from one app. By using Mergerino, you are responsible for following the terms, community guidelines, and developer policies of the services you connect to, including YouTube, Twitch, Kick, and TikTok.</sub>

<sub>When you sign in to a third-party service through Mergerino, any messages, moderation actions, or account actions made through the app are performed by your signed-in account. You are responsible for how your account is used inside the app.</sub>

<sub>Mergerino is provided "as is", without warranties of any kind. The maintainer does not guarantee that every platform feature will remain available, uninterrupted, or compatible with future changes made by third-party services.</sub>

<sub>Mergerino is not affiliated with, endorsed by, or sponsored by YouTube, Google, Twitch, Kick, TikTok, or Chatterino.</sub>

<sub>For support questions, bug reports, or terms-related questions, open an issue on this GitHub repository or contact the maintainer via Discord or Twitter/X.</sub>

## Notes

- This is an unofficial fork of [Chatterino](https://github.com/Chatterino/chatterino2).
- Mergerino is intended to provide a merged multi-service chat client and Windows distribution path, not to represent the upstream project.
- GitHub workflows and release packaging in this repo are Windows-only.
