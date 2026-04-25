// SPDX-FileCopyrightText: 2026 Contributors to Mergerino <https://mergerino.app>
//
// SPDX-License-Identifier: MIT

#include "util/ChatterinoImport.hpp"

#include "util/CombinePath.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

namespace chatterino::chatterinoImport {

namespace {

constexpr const char *const kSubdirsToCopy[] = {
    "Settings",      // settings.json, window-layout.json
    "Themes",        // user-installed custom themes
    "Plugins",       // user-installed Lua plugins
    "Misc",          // ChatLogs, badge cache, nicknames, ignores
    "Dictionaries",  // hunspell dictionaries the user has added
};

bool copyDirRecursive(const QString &src, const QString &dst, int &copied,
                      QString &error)
{
    QDir srcDir(src);
    if (!srcDir.exists())
    {
        return true;  // nothing to copy is fine
    }

    if (!QDir().mkpath(dst))
    {
        error = QStringLiteral("Could not create directory: %1").arg(dst);
        return false;
    }

    QDirIterator it(src, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        it.next();
        const QFileInfo &info = it.fileInfo();
        const QString relative = srcDir.relativeFilePath(info.absoluteFilePath());
        const QString target = combinePath(dst, relative);

        if (info.isDir())
        {
            if (!QDir().mkpath(target))
            {
                error =
                    QStringLiteral("Could not create directory: %1").arg(target);
                return false;
            }
            continue;
        }

        // Overwrite if present.
        if (QFile::exists(target))
        {
            QFile::remove(target);
        }

        if (!QFile::copy(info.absoluteFilePath(), target))
        {
            error = QStringLiteral("Failed to copy %1 -> %2")
                        .arg(info.absoluteFilePath(), target);
            return false;
        }
        ++copied;
    }
    return true;
}

}  // namespace

QString chatterino2RootDir()
{
#ifdef Q_OS_WIN
    const auto roaming = qEnvironmentVariable("APPDATA");
    if (roaming.isEmpty())
    {
        return {};
    }
    return QDir::cleanPath(roaming + QStringLiteral("/Chatterino2"));
#else
    return {};
#endif
}

bool chatterino2HasSettings()
{
    const auto root = chatterino2RootDir();
    if (root.isEmpty())
    {
        return false;
    }
    return QFileInfo::exists(
        combinePath(root, QStringLiteral("Settings/settings.json")));
}

bool isFreshInstall(const QString &mergerinoRoot)
{
    return !QFileInfo::exists(
        combinePath(mergerinoRoot, QStringLiteral("Settings/settings.json")));
}

ImportResult importFromChatterino2(const QString &mergerinoRoot)
{
    ImportResult result;

    const auto src = chatterino2RootDir();
    if (src.isEmpty() || !QDir(src).exists())
    {
        result.error = QStringLiteral("Chatterino2 directory not found.");
        return result;
    }

    for (const char *sub : kSubdirsToCopy)
    {
        const auto from = combinePath(src, QString::fromLatin1(sub));
        const auto to = combinePath(mergerinoRoot, QString::fromLatin1(sub));
        if (!copyDirRecursive(from, to, result.filesCopied, result.error))
        {
            return result;
        }
    }

    result.ok = true;
    return result;
}

}  // namespace chatterino::chatterinoImport
