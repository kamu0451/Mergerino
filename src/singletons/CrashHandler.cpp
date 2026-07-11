// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/CrashHandler.hpp"

#include "common/Literals.hpp"
#include "common/QLogging.hpp"
#include "singletons/Paths.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace {

using namespace chatterino;
using namespace literals;

const QString RECOVERY_FILE = u"chatterino-recovery.json"_s;

/// The recovery options are saved outside the settings
/// to be able to read them without loading the settings.
///
/// The flags are saved in the `RECOVERY_FILE` as JSON.
std::optional<bool> readRecoverySettings(const Paths &paths)
{
    QFile file(QDir(paths.crashdumpDirectory).filePath(RECOVERY_FILE));
    if (!file.open(QFile::ReadOnly))
    {
        return std::nullopt;
    }

    QJsonParseError error{};
    auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError)
    {
        qCWarning(chatterinoCrashhandler)
            << "Failed to parse recovery settings" << error.errorString();
        return std::nullopt;
    }

    const auto obj = doc.object();
    auto shouldRecover = obj["shouldRecover"_L1];
    if (!shouldRecover.isBool())
    {
        return std::nullopt;
    }

    return shouldRecover.toBool();
}

}  // namespace

namespace chatterino {

CrashHandler::CrashHandler(const Paths &paths_)
    : paths(paths_)
{
    auto optSettings = readRecoverySettings(this->paths);
    if (optSettings)
    {
        this->shouldRecover_ = *optSettings;
    }
    else
    {
        // By default, we don't restart after a crash.
        this->saveShouldRecover(false);
    }
}

void CrashHandler::saveShouldRecover(bool value)
{
    this->shouldRecover_ = value;

    QFile file(QDir(this->paths.crashdumpDirectory).filePath(RECOVERY_FILE));
    if (!file.open(QFile::WriteOnly | QFile::Truncate))
    {
        qCWarning(chatterinoCrashhandler)
            << "Failed to open" << file.fileName();
        return;
    }
    file.write(QJsonDocument(QJsonObject{
                                 {"shouldRecover"_L1, value},
                             })
                   .toJson(QJsonDocument::Compact));
}

}  // namespace chatterino
