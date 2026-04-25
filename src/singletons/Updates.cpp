// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Updates.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "singletons/Paths.hpp"
#include "util/CombinePath.hpp"
#include "util/PostToThread.hpp"

#include <cassert>

#include <QApplication>
#include <QDesktopServices>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>
#include <QtConcurrent>
#include <semver/semver.hpp>

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
    if (this->status_ != UpdateAvailable)
    {
        assert(false);
        return;
    }

    // Fall back to a browser-open if we never resolved the portable-zip URL
    // (e.g. the release JSON had no Mergerino.zip asset).
    if (this->updatePortable_.isEmpty())
    {
        QString url =
            this->updateGuideLink_.isEmpty()
                ? QStringLiteral(
                      "https://github.com/kamu0451/Mergerino/releases/latest")
                : this->updateGuideLink_;
        QDesktopServices::openUrl(QUrl(url));
        return;
    }

    // No extra message box here — the UpdateDialog itself flips to
    // "Downloading..." once setStatus_(Downloading) fires, and mergerino
    // exits seconds later when the updater takes over.
    NetworkRequest(this->updatePortable_)
        .timeout(600000)
        .onError([this](const NetworkResult &result) {
            qCWarning(chatterinoUpdate)
                << "update download failed:" << result.formatError();
            this->setStatus_(DownloadFailed);
            postToThread([] {
                auto *errBox = new QMessageBox(
                    QMessageBox::Information, "Mergerino Update",
                    "Failed while trying to download the update.");
                errBox->setAttribute(Qt::WA_DeleteOnClose);
                errBox->show();
                errBox->raise();
            });
            return true;
        })
        .onSuccess([this](const NetworkResult &result) {
            QByteArray data = result.getData();
            if (data.isEmpty())
            {
                this->setStatus_(DownloadFailed);
                return;
            }

            auto filename =
                combinePath(this->paths.miscDirectory, "update.zip");

            QFile file(filename);
            if (!file.open(QIODevice::Truncate | QIODevice::WriteOnly))
            {
                qCWarning(chatterinoUpdate)
                    << "Failed to save update.zip:" << file.errorString();
                this->setStatus_(WriteFileFailed);
                return;
            }
            if (file.write(data) == -1)
            {
                qCWarning(chatterinoUpdate)
                    << "Failed to write update.zip:" << file.errorString();
                this->setStatus_(WriteFileFailed);
                return;
            }
            file.flush();
            file.close();

            auto updaterPath = Updates::portableUpdaterPath();
            if (!QFile::exists(updaterPath))
            {
                qCWarning(chatterinoUpdate)
                    << "Portable updater missing at" << updaterPath;
                this->setStatus_(MissingPortableUpdater);
                return;
            }

            bool ok = QProcess::startDetached(updaterPath,
                                              {filename, "restart"});
            if (!ok)
            {
                qCWarning(chatterinoUpdate)
                    << "startDetached failed for" << updaterPath;
                this->setStatus_(RunUpdaterFailed);
                return;
            }

            QApplication::exit(0);
        })
        .execute();
    this->setStatus_(Downloading);
}

void Updates::checkForUpdates()
{
    const auto &version = Version::instance();
    QString localCommit = version.commitHash();

    // GIT.cmake falls back to "GIT-REPOSITORY-NOT-FOUND" when the build tree
    // has no usable git info. In that case we can't compare anything.
    if (localCommit.isEmpty() ||
        localCommit.startsWith(QStringLiteral("GIT-")))
    {
        qCDebug(chatterinoUpdate)
            << "skipping update check: no usable commit hash baked into build";
        this->setStatus_(NoUpdateAvailable);
        return;
    }

    // Don't pester local developers with update prompts when their working
    // tree had uncommitted changes at build time.
    if (version.isModified())
    {
        qCDebug(chatterinoUpdate)
            << "skipping update check: build had local modifications";
        this->setStatus_(NoUpdateAvailable);
        return;
    }

    this->setStatus_(Searching);

    NetworkRequest("https://api.github.com/repos/kamu0451/Mergerino/"
                   "releases/latest")
        .header("Accept", "application/vnd.github+json")
        .timeout(20000)
        .onSuccess([this, localCommit](const NetworkResult &result) {
            auto json = result.parseJson();
            if (json.isEmpty())
            {
                qCWarning(chatterinoUpdate)
                    << "update check: empty/invalid JSON from releases API";
                this->setStatus_(SearchFailed);
                return;
            }

            QString remoteSha = json.value("target_commitish").toString();
            if (remoteSha.length() < 7)
            {
                qCWarning(chatterinoUpdate)
                    << "update check: missing/short target_commitish";
                this->setStatus_(SearchFailed);
                return;
            }

            this->updateGuideLink_ = json.value("html_url").toString();
            if (this->updateGuideLink_.isEmpty())
            {
                this->updateGuideLink_ = QStringLiteral(
                    "https://github.com/kamu0451/Mergerino/releases/latest");
            }

            // Grab the portable-zip asset URL so installUpdates() can
            // download it directly. The release uploads a single
            // version-less Mergerino.zip; tolerate a missing asset by
            // falling back to a browser-open in installUpdates().
            this->updatePortable_.clear();
            QJsonArray assets = json.value("assets").toArray();
            for (const auto &assetVal : assets)
            {
                QJsonObject asset = assetVal.toObject();
                QString name = asset.value("name").toString();
                if (name.compare(QStringLiteral("Mergerino.zip"),
                                 Qt::CaseInsensitive) == 0)
                {
                    this->updatePortable_ =
                        asset.value("browser_download_url").toString();
                    break;
                }
            }

            // Prefer the parsed semver from the release name (CI publishes
            // it as "Mergerino X.Y.Z"). Falls back to the short SHA so old
            // releases still work — they'll just look uglier in the prompt.
            QString releaseName = json.value("name").toString();
            static const QRegularExpression versionRe(
                QStringLiteral("(\\d+\\.\\d+\\.\\d+)"));
            auto versionMatch = versionRe.match(releaseName);
            QString remoteVersion;
            if (versionMatch.hasMatch())
            {
                remoteVersion = versionMatch.captured(1);
                this->onlineVersion_ = remoteVersion;
            }
            else
            {
                this->onlineVersion_ = remoteSha.left(7);
            }
            this->isDowngrade_ = false;

            // local commit hash is "git rev-parse --short HEAD" (7-8 chars);
            // remote target_commitish is the full 40-char SHA.
            if (remoteSha.startsWith(localCommit, Qt::CaseInsensitive))
            {
                this->setStatus_(NoUpdateAvailable);
                return;
            }

            // When both versions parse cleanly, only prompt if remote is
            // strictly newer. Otherwise (e.g. dev build ahead of latest
            // release, or release name without a version) fall back to
            // SHA-mismatch = UpdateAvailable.
            if (!remoteVersion.isEmpty())
            {
                this->isDowngrade_ = Updates::isDowngradeOf(
                    remoteVersion, this->currentVersion_);
                if (this->isDowngrade_)
                {
                    qCDebug(chatterinoUpdate)
                        << "remote" << remoteVersion << "is older than local"
                        << this->currentVersion_ << "- not prompting";
                    this->setStatus_(NoUpdateAvailable);
                    return;
                }
            }

            this->setStatus_(UpdateAvailable);
        })
        .onError([this](const NetworkResult &result) {
            qCWarning(chatterinoUpdate)
                << "update check failed:" << result.formatError();
            this->setStatus_(SearchFailed);
            return true;
        })
        .execute();
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
    switch (this->getStatus())
    {
        case UpdateAvailable:
        case SearchFailed:
        case Downloading:
        case DownloadFailed:
        case WriteFileFailed:
        case MissingPortableUpdater:
        case RunUpdaterFailed:
            return true;

        default:
            return false;
    }
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
    QString prompt = this->updatePortable_.isEmpty()
                         ? QStringLiteral(
                               "Open the releases page to download it?")
                         : QStringLiteral(
                               "Do you want to download and install it?");

    return QString("Mergerino %1 is available (you have %2).\n\n%3")
        .arg(this->getOnlineVersion(), this->getCurrentVersion(), prompt);
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
