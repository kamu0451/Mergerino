# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Mergerino is an unofficial fork of Chatterino2. It is a Qt 6, C++23 desktop chat client that merges Twitch, Kick, and YouTube live chat into a single Chatterino-style multi-tab workflow. **Windows-only** — `CMakeLists.txt` issues a `FATAL_ERROR` on non-Windows hosts, and all CI/packaging is Windows-only. The source root namespace is still `chatterino` and the library target is `mergerino-lib`; the binary is `mergerino.exe`.

## Build

Two supported toolchains; both require Visual Studio 2022 (`Desktop development with C++`), Qt 6 (with `qtimageformats`) on `PATH`, CMake, Git, and recursively cloned submodules.

Conan 2 (matches CI):

```cmd
conan profile detect
conan install . -of build-conan -s build_type=Release --build=missing
cmake -S . -B build-conan -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=build-conan\conan_toolchain.cmake
cmake --build build-conan --parallel
```

vcpkg alternative: `vcpkg install` then configure with `-DCMAKE_TOOLCHAIN_FILE="$Env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"`.

Run: `build-conan\bin\mergerino.exe`. To produce a standalone bundle: `windeployqt build-conan\bin\mergerino.exe --release --no-compiler-runtime --no-translations --no-opengl-sw --dir build-conan\bin`.

Key CMake options (from `CMakeLists.txt`): `BUILD_TESTS`, `BUILD_BENCHMARKS`, `CHATTERINO_PLUGINS` (Lua/Sol2, on by default), `CHATTERINO_SPELLCHECK` (requires Hunspell — CI turns this on), `BUILD_WITH_CRASHPAD` (off by default, and off in the release workflow), `CHATTERINO_LTO`, `USE_PRECOMPILED_HEADERS`.

## Tests

Tests use GoogleTest. They need two external services running first: `httpbox --port 9051` and `twitch-pubsub-server-test 127.0.0.1:9050` (a `docker compose up` in `tests/` brings both up). CI uses `NMake Makefiles` with `-DBUILD_APP=OFF -DBUILD_TESTS=On -DCMAKE_BUILD_TYPE=RelWithDebInfo`, then:

```cmd
ctest --repeat until-pass:4 --output-on-failure
```

To run the binary directly: `./bin/chatterino-test` (test target name is unchanged from upstream). To run a single test, use GoogleTest filters: `./bin/chatterino-test --gtest_filter=TwitchAccount.*`.

**Snapshot tests**: message-building changes frequently break snapshots. To refresh them, flip `UPDATE_SNAPSHOTS` to `true` at the top of `tests/src/IrcMessageHandler.cpp`, rerun tests, flip it back, rerun, then review the resulting JSON diffs in `tests/snapshots/`.

Benchmarks live in `benchmarks/` and are gated behind `-DBUILD_BENCHMARKS=On` (Google Benchmark).

## CI / Release pipeline

- `.github/workflows/build.yml` — builds the Windows x64 package on every push/PR to `main`, uploads `Mergerino-1.3-win64.zip` as an artifact. `.CI/deploy-crt.ps1` pulls in the MSVC runtime DLLs.
- `.github/workflows/release.yml` — triggered by `workflow_run` on a successful Build. Force-updates the `latest` tag to the new head SHA and replaces the single asset on the `Latest` GitHub release. This is a **rolling release** — there is no per-version tagging flow. The `1.3` in the filename is hard-coded in both workflows; `CMakeLists.txt`'s `project(... VERSION 1.3.0 ...)` drives `src/common/Version.hpp` via `configure_file`, so bumping the version means editing the workflow filenames, `README.md`, and the `project()` VERSION.
- `.github/workflows/test-windows.yml` — runs the GoogleTest suite against `windows-latest` + Qt 6.9.3.

## Architecture

This is Chatterino2's architecture with Mergerino-specific additions. The big pieces:

- **`src/Application.{hpp,cpp}`** — the `IApplication` god-object hands out singletons (theme, window manager, emote controllers, Twitch IRC server, Kick chat server, EventSub controller, plugin controller, spellchecker, etc.). Most subsystems are looked up through it rather than passed explicitly.
- **`src/singletons/`** — process-wide managers: `WindowManager`, `Settings` (pajlada settings-backed, persisted JSON), `Theme`, `Paths`, `Resources`, `Toasts`, `Updates`, `NativeMessaging` (browser extension bridge).
- **`src/providers/<service>/`** — one subdirectory per chat/emote/data provider: `twitch/` (IRC + PubSub + Helix + EventSub), `kick/` (REST + Pusher-style chat websocket + live controller), `youtube/` (`YouTubeLiveChat` polls the InnerTube `get_live_chat` endpoint), plus emote providers `seventv/`, `bttv/`, `ffz/`, and the aggregator in `merged/`.
- **`src/providers/merged/MergedChannel`** — the Mergerino-specific concept. A `MergedChannel` owns child `ChannelPtr`s for Twitch and Kick and a `YouTubeLiveChat` instance, subscribes to each source's `messageAppended`/`messageReplaced` signals, and rebroadcasts a platform-tagged copy on the merged channel. Keys messages via `messageKey()` to de-duplicate edits across sources. When extending merged behavior, preserve the per-platform `SignalHolder`s and the `mirroredMessages_` map — lifetime and replacement rely on them.
- **`src/widgets/`** — the UI tree. `Window` → `Notebook` (tabs) → `SplitContainer` (layouts inside a tab) → `Split` (single channel view) → `ChannelView` (the scrolling message list). Dialogs and settings pages are in `widgets/dialogs/` and `widgets/settingspages/`.
- **`src/messages/`** — the message data model (`Message`, `MessageElement`, `MessageLayout`, emotes, links). Layout is computed lazily and cached per view.
- **`src/controllers/`** — non-UI user-facing features: commands, completion, highlights, hotkeys, ignores, logging, moderation actions, nicknames, notifications, plugins, sound, spellcheck.
- **`lib/`** — vendored submodules: `libcommuni` (IRC), `qtkeychain`, `signals`/`serialize`/`settings` (pajlada), `rapidjson`, `lua` + Sol2 for plugins, `twitch-eventsub-ws`, `WinToast`, `googletest`.
- **Plugins (alpha)** — Lua scripting behind `CHATTERINO_PLUGINS`; API shape is documented in `docs/chatterino.d.ts` and `docs/plugin-info.schema.json`, runtime lives in `src/controllers/plugins/`.

## Conventions

- C++23, `CMAKE_CXX_STANDARD 23`, PCH enabled via `src/PrecompiledHeader.hpp`.
- Formatting is enforced by `.clang-format`; lint rules live in `.clang-tidy`. `.prettierrc`/`.prettierignore` cover the non-C++ files.
- `.git-blame-ignore-revs` lists bulk-reformat commits — use `git blame --ignore-revs-file=.git-blame-ignore-revs` when chasing history.
- Licensing: REUSE-compliant. New C++ files should carry `SPDX-FileCopyrightText` + `SPDX-License-Identifier: MIT` headers (see existing files; Mergerino-new files use year 2026).
- Keep new code inside the `chatterino` namespace — the fork has not renamed it.
