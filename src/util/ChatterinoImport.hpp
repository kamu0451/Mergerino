// SPDX-FileCopyrightText: 2026 Contributors to Mergerino <https://mergerino.app>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

namespace chatterino::chatterinoImport {

struct ImportResult {
    bool ok = false;
    int filesCopied = 0;
    QString error;
};

// Path to %APPDATA%\Chatterino2 on Windows, empty string elsewhere or if
// %APPDATA% is unset.
QString chatterino2RootDir();

// True iff Chatterino2's Settings/settings.json exists.
bool chatterino2HasSettings();

// True iff mergerinoRoot has no Settings/settings.json yet — i.e. the user
// has never configured this Mergerino install.
bool isFreshInstall(const QString &mergerinoRoot);

// Copies the user-data subdirectories (Settings, Themes, Plugins, Misc,
// Dictionaries) from %APPDATA%\Chatterino2 into mergerinoRoot. Existing
// files in mergerinoRoot are overwritten.
ImportResult importFromChatterino2(const QString &mergerinoRoot);

}  // namespace chatterino::chatterinoImport
