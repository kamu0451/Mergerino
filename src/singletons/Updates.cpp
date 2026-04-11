// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Updates.hpp"

#include "common/Literals.hpp"
#include "common/Modes.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "util/CombinePath.hpp"
#include "util/PostToThread.hpp"

#include <QApplication>
#include <QDesktopServices>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>
#include <QStringBuilder>
#include <QtConcurrent>
#include <semver/semver.hpp>

namespace {

using namespace chatterino;
using namespace literals;

QString currentBranch()
{
    return getSettings()->betaUpdates ? "beta" : "stable";
}

#if defined(Q_OS_WIN)
const QString CHATTERINO_OS = u"win"_s;
#elif defined(Q_OS_MACOS)
const QString CHATTERINO_OS = u"macos"_s;
#elif defined(Q_OS_LINUX)
const QString CHATTERINO_OS = u"linux"_s;
#elif defined(Q_OS_FREEBSD)
const QString CHATTERINO_OS = u"freebsd"_s;
#else
const QString CHATTERINO_OS = u"unknown"_s;
#endif

QJsonValue getForArchitecture(const QJsonObject &obj, const QString &key)
{
    auto val = obj[key];

#ifdef Q_PROCESSOR_ARM
    QString armKey = key % u"_arm";
    if (obj[armKey].isString())
    {
        val = obj[armKey];
    }
#elifdef Q_PROCESSOR_X86
    QString x86Key = key % u"_x86";
    if (obj[x86Key].isString())
    {
        val = obj[x86Key];
    }
#endif

    return val;
}

}  // namespace

namespace chatterino {

Updates::Updates(const Paths &paths_, Settings &settings)
    : paths(paths_)
    , currentVersion_(CHATTERINO_VERSION)
    , updateGuideLink_()
{
    qCDebug(chatterinoUpdate) << "init UpdateManager";
    (void)settings;
}

/// Checks if the online version is newer or older than the current version.
bool Updates::isDowngradeOf(const QString &online, const QString &current)
{
    semver::version onlineVersion;
    if (!onlineVersion.from_string_noexcept(online.toStdString()))
    {
        qCWarning(chatterinoUpdate) << "Unable to parse online version"
                                    << online << "into a proper semver string";
        return false;
    }

    semver::version currentVersion;
    if (!currentVersion.from_string_noexcept(current.toStdString()))
    {
        qCWarning(chatterinoUpdate) << "Unable to parse current version"
                                    << current << "into a proper semver string";
        return false;
    }

    // TODO: remove once chatterino7's major version switches from `7` to `2`
    if (currentVersion.major == 7 && onlineVersion.major == 2)
    {
        currentVersion = {2, currentVersion.minor, currentVersion.patch,
                          currentVersion.prerelease_type,
                          currentVersion.prerelease_number};
    }

    return onlineVersion < currentVersion;
}

void Updates::deleteOldFiles()
{
    std::ignore = QtConcurrent::run([dir{this->paths.miscDirectory}] {
        {
            auto path = combinePath(dir, "Update.exe");
            if (QFile::exists(path))
            {
                QFile::remove(path);
            }
        }
        {
            auto path = combinePath(dir, "update.zip");
            if (QFile::exists(path))
            {
                QFile::remove(path);
            }
        }
    });
}

const QString &Updates::getCurrentVersion() const
{
    return this->currentVersion_;
}

const QString &Updates::getOnlineVersion() const
{
    return this->onlineVersion_;
}

void Updates::installUpdates()
{
    QMessageBox *box = new QMessageBox(
        QMessageBox::Information, "Mergerino Update",
        "Automatic updates are disabled in this build of Mergerino.\n\n"
        "Install new versions manually when you publish them.");
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
}

void Updates::checkForUpdates()
{
#ifndef CHATTERINO_DISABLE_UPDATER
    this->onlineVersion_.clear();
    this->setStatus_(NoUpdateAvailable);
#endif
}

Updates::Status Updates::getStatus() const
{
    return this->status_;
}

QString Updates::portableUpdaterPath()
{
    return combinePath(QCoreApplication::applicationDirPath(),
                       "updater.1/MergerinoUpdater.exe");
}

bool Updates::shouldShowUpdateButton() const
{
    return false;
}

bool Updates::isError() const
{
    switch (this->getStatus())
    {
        case SearchFailed:
        case DownloadFailed:
        case WriteFileFailed:
        case MissingPortableUpdater:
        case RunUpdaterFailed:
            return true;

        default:
            return false;
    }
}

bool Updates::isDowngrade() const
{
    return this->isDowngrade_;
}

QString Updates::buildUpdateAvailableText() const
{
    const auto &version = Version::instance();

    if (version.isNightly())
    {
        // Since Nightly builds can be installed in many different ways, we ask the user to download the update manually.
        if (this->isDowngrade())
        {
            return QString("The version online (%1) seems to be lower than the "
                           "current (%2).\nEither a version was reverted or "
                           "you are running a newer build.\n\nDo you want to "
                           "head to the releases page to download it?")
                .arg(this->getOnlineVersion(), this->getCurrentVersion());
        }

        return QString("An update (%1) is available.\n\nDo you want to head to "
                       "the releases page to download the new update?")
            .arg(this->getOnlineVersion());
    }

    if (this->isDowngrade())
    {
        return QString("The version online (%1) seems to be lower than the "
                       "current (%2).\nEither a version was reverted or "
                       "you are running a newer build.\n\nDo you want to "
                       "download and install it?")
            .arg(this->getOnlineVersion(), this->getCurrentVersion());
    }

    return QString("An update (%1) is available.\n\nDo you want to "
                   "download and install it?")
        .arg(this->getOnlineVersion());
}

void Updates::setStatus_(Status status)
{
    if (this->status_ != status)
    {
        this->status_ = status;
        postToThread([this, status] {
            this->statusUpdated.invoke(status);
        });
    }
}

}  // namespace chatterino
