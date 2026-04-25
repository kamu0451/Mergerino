# Mergerino

Mergerino is a desktop chat client that combines a Chatterino-style multi-channel workflow with support for Twitch, Kick, YouTube, and TikTok LIVE in one app. The goal is to keep chat fast and practical while making multi-platform viewing easier from a single desktop client.

This repository keeps the source here and ships installable Windows builds through GitHub Releases.

[![Download Windows Build](https://img.shields.io/badge/Download-Windows%20Build-2ea44f?style=for-the-badge)](../../releases/latest/download/Mergerino.zip)
[![All Releases](https://img.shields.io/badge/GitHub-Releases-1f6feb?style=for-the-badge)](../../releases)

## What Is Mergerino?

- Merged desktop chat for Twitch, Kick, YouTube, and TikTok LIVE
- Chatterino-style multi-channel chat workflow in a standalone app
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

## Notes

- This is an unofficial fork of [Chatterino](https://github.com/Chatterino/chatterino2).
- Mergerino is intended to provide a merged multi-service chat client and Windows distribution path, not to represent the upstream project.
- GitHub workflows and release packaging in this repo are Windows-only.
