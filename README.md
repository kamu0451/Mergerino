# Mergerino

Mergerino is a desktop chat client that combines a fast multi-channel workflow with support for Twitch, Kick, YouTube, and TikTok LIVE in one app. The goal is to keep chat fast and practical while making multi-platform viewing easier from a single desktop client.

This repository keeps the source here and ships installable Windows builds through GitHub Releases.

[![Download Windows Build](https://img.shields.io/badge/Download-Windows%20Build-2ea44f?style=for-the-badge)](../../releases/latest/download/Mergerino.zip)
[![All Releases](https://img.shields.io/badge/GitHub-Releases-1f6feb?style=for-the-badge)](../../releases)
[![Discord](https://img.shields.io/badge/Discord-Join%20Server-5865F2?style=for-the-badge&logo=discord&logoColor=white)](https://discord.gg/tAjdrbvxwT)

## What Is Mergerino?

- Merged desktop chat for Twitch, Kick, YouTube, and TikTok LIVE
- Fast multi-channel chat workflow in a standalone app
- 7TV, BTTV, and FFZ integration
- Windows-only builds packaged around `mergerino.exe`

## Download

If you just want to install the app, do not download the source code zip from the repository page.

Use the latest GitHub release instead:

- Open [the releases page](../../releases)
- Download [Mergerino.zip](../../releases/latest/download/Mergerino.zip)
- Extract it somewhere you want to keep it
- Open `mergerino.exe` from inside the extracted folder

## Install

### Windows

1. Download `Mergerino.zip`
2. Extract the zip
3. Open the extracted `Mergerino` folder
4. Run `mergerino.exe`
5. Keep the `.exe` inside that folder with the bundled files

## Usage

### Merged tabs

A merged tab combines Twitch, Kick, YouTube, and TikTok LIVE chat for one streamer into a single scrolling view, tagging each message with its source platform.

To add one:

- Click the `+` (add tab) button on the tab bar. This opens the "Select a channel to join" dialog.
- Switch to the "Merged" page in that dialog.
- Set a tab name, then enable and fill in whichever sources you want: Twitch/Kick channel name, a YouTube `@handle` or video link, and/or a TikTok `@username` or `/@user/live` URL.
- Confirm to open the tab. You can later add, remove, or edit sources for an existing merged tab the same way.

### Fork-specific settings

Open Settings -> General -> Messages for the merged-chat options this fork adds on top of upstream Chatterino:

- Hide chat bot messages, hide command messages (`!command`), and hide emote-only messages -- each applies across all merged platforms at once, not per-platform.
- Float emotes on the chat background -- lets emotes from emote-only messages drift across the background instead of just appearing inline.
- Merged platform indicator -- controls whether merged rows show a platform color, a platform logo badge, both, or neither, so you can tell which service a message came from.

### YouTube and TikTok integrations

YouTube and TikTok support in Mergerino are receive-only: chat is read via polling/scraping, not an official chat API, and (aside from YouTube send/mod actions added via OAuth) you cannot post or moderate through them. TikTok chat works by loading TikTok's own web player in a hidden background window, which the in-app disclosure in the Add-Tab dialog flags as a potential conflict with TikTok's Terms of Service -- anonymous mode is recommended, and a throwaway account if you do log in. TikTok also requires a working Edge WebView2 runtime on your machine (bundled by default on Windows 11; installable separately on Windows 10 -- see Notes below).

## Build Your Own

Mergerino currently supports Windows only. The build needs Visual Studio 2022 (Desktop C++ workload), Qt 6 with `qtimageformats` on PATH, CMake, and Git. Build instructions for Conan and vcpkg are in [CLAUDE.md](CLAUDE.md).

Clone with submodules:

```shell
git clone --recurse-submodules <your-mergerino-repository-url>
```

If you already cloned without submodules:

```shell
git submodule update --init --recursive
```

## Notes

- This is an unofficial fork of [Chatterino](https://github.com/Chatterino/chatterino2).
- Mergerino is intended to provide a merged multi-service chat client and Windows distribution path, not to represent the upstream project.
- GitHub workflows and release packaging in this repo are Windows-only.
- The TikTok provider runs TikTok's own webapp inside a hidden WebView2 host. Windows 11 ships the Edge WebView2 Runtime by default; on Windows 10 you may need to install it from Microsoft if TikTok rooms fail to connect.
- [Privacy Policy](https://fixlation.github.io/Mergerino/privacy/)
- [Terms of Service](https://fixlation.github.io/Mergerino/terms/)
