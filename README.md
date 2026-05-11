# Mergerino

Mergerino is a desktop chat client that combines a Chatterino-style multi-channel workflow with support for Twitch, Kick, YouTube, and TikTok LIVE in one app. The goal is to keep chat fast and practical while making multi-platform viewing easier from a single desktop client.

This repository keeps the source here and ships installable Windows builds through GitHub Releases.

Current version: Mergerino 1.2.6

[![Download Windows Build](https://img.shields.io/badge/Download-Windows%20Build-2ea44f?style=for-the-badge)](https://github.com/Fixlation/Mergerino/releases/latest/download/Mergerino.zip)
[![All Releases](https://img.shields.io/badge/GitHub-Releases-1f6feb?style=for-the-badge)](https://github.com/Fixlation/Mergerino/releases)

## What Is Mergerino?

- Merged desktop chat for Twitch, Kick, YouTube, and TikTok LIVE
- Chatterino-style multi-channel chat workflow in a standalone app
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

Effective date: May 11, 2026

Mergerino is a desktop chat client. It does not operate a hosted backend for YouTube login. When you sign in with Google, the app uses Google's OAuth flow and the YouTube Data API directly from your desktop device so YouTube chat features can work inside the app.

Mergerino requests YouTube access only for user-facing chat features, including:

- Showing YouTube live chat for channels you open in the app
- Sending YouTube live chat messages as the signed-in Google account
- Performing YouTube live chat moderation actions, such as deleting messages or hiding users, when the signed-in account is permitted to do so by YouTube
- Displaying basic YouTube account/channel information needed to show which account is signed in

Mergerino stores YouTube OAuth tokens locally on your device so you do not have to sign in every time you open the app. The Mergerino developer does not receive, store, sell, rent, or share your Google OAuth tokens or Google user data.

Mergerino uses Google user data only to provide and improve the visible YouTube chat features in the app. It does not use Google user data for advertising, tracking, credit decisions, data brokerage, or unrelated analytics.

You can revoke Mergerino's Google access at any time from your Google Account permissions page: https://myaccount.google.com/permissions

For privacy or support questions, open an issue on this GitHub repository or contact the maintainer via Discord or Twitter/X.

## Terms of Service

Effective date: May 11, 2026

Mergerino is provided as an unofficial desktop chat client for accessing supported live chat platforms from one app. By using Mergerino, you are responsible for following the terms, community guidelines, and developer policies of the services you connect to, including YouTube, Twitch, Kick, and TikTok.

When you sign in to a third-party service through Mergerino, any messages, moderation actions, or account actions made through the app are performed by your signed-in account. You are responsible for how your account is used inside the app.

Mergerino is provided "as is", without warranties of any kind. The maintainer does not guarantee that every platform feature will remain available, uninterrupted, or compatible with future changes made by third-party services.

Mergerino is not affiliated with, endorsed by, or sponsored by YouTube, Google, Twitch, Kick, TikTok, or Chatterino.

For support questions, bug reports, or terms-related questions, open an issue on this GitHub repository or contact the maintainer via Discord or Twitter/X.

## Notes

- This is an unofficial fork of [Chatterino](https://github.com/Chatterino/chatterino2).
- Mergerino is intended to provide a merged multi-service chat client and Windows distribution path, not to represent the upstream project.
- GitHub workflows and release packaging in this repo are Windows-only.
