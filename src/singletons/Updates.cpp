// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Updates.hpp"

#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "singletons/Paths.hpp"
#include "util/CombinePath.hpp"
#include "util/PostToThread.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>
#include <QUrl>
#include <QtConcurrent>
#include <semver/semver.hpp>

namespace {

using namespace chatterino;
using namespace literals;

const QUrl MERGERINO_LATEST_RELEASE_API(
    u"https://api.github.com/repos/Fixlation/Mergerino/releases/latest"_s);

QString normalizedReleaseVersion(const QString &value)
{
    auto candidate = value.trimmed();
    if (candidate.isEmpty())
    {
        return {};
    }

    semver::version parsed;
    if (parsed.from_string_noexcept(candidate.toStdString()))
    {
        return candidate;
    }

    if (candidate.startsWith(u'v', Qt::CaseInsensitive))
    {
        const auto withoutPrefix = candidate.sliced(1);
        if (parsed.from_string_noexcept(withoutPrefix.toStdString()))
        {
            return withoutPrefix;
        }
    }

    static const QRegularExpression semverPattern{
        QStringLiteral(R"((\d+\.\d+\.\d+(?:-(?:alpha|beta|rc)(?:\.\d+)?)?))"),
        QRegularExpression::CaseInsensitiveOption};

    const auto match = semverPattern.match(candidate);
    if (!match.hasMatch())
    {
        return {};
    }

    candidate = match.captured(1).toLower();
    if (!parsed.from_string_noexcept(candidate.toStdString()))
    {
        return {};
    }

    return candidate;
}

QString releaseVersionFrom(const QString &releaseName, const QString &tagName)
{
    for (const auto &candidate : {releaseName, tagName})
    {
        auto version = normalizedReleaseVersion(candidate);
        if (!version.isEmpty())
        {
            return version;
        }
    }

    return {};
}

bool isNewerVersion(const QString &online, const QString &current)
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

    return currentVersion < onlineVersion;
}

}  // namespace

namespace chatterino {

Updates::Updates(const Paths &paths_, Settings &settings)
    : paths(paths_)
    , currentVersion_(CHATTERINO_VERSION)
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
        {
            auto path = combinePath(dir, "install-update.ps1");
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
        return;
    }

    this->downloadAndRunInstaller_();
}

void Updates::checkForUpdates()
{
    if (this->status_ == Searching || this->status_ == Downloading)
    {
        return;
    }

    this->onlineVersion_.clear();
    this->updateDownloadUrl_.clear();
    this->isDowngrade_ = false;
    this->setStatus_(Searching);

    NetworkRequest(MERGERINO_LATEST_RELEASE_API)
        .header("Accept", "application/vnd.github+json")
        .timeout(15000)
        .onSuccess([this](NetworkResult result) {
            const auto root = result.parseJson();
            const auto releaseName = root["name"].toString();
            const auto tagName = root["tag_name"].toString();
            const auto releaseVersion =
                releaseVersionFrom(releaseName, tagName);

            QString downloadUrl;
            const auto assets = root["assets"].toArray();
            for (const auto &assetValue : assets)
            {
                const auto asset = assetValue.toObject();
                if (asset["name"].toString() == "Mergerino.zip")
                {
                    downloadUrl =
                        asset["browser_download_url"].toString();
                    break;
                }
            }

            if (releaseVersion.isEmpty() || downloadUrl.isEmpty())
            {
                qCWarning(chatterinoUpdate)
                    << "Latest Mergerino release is missing version or asset";
                this->setStatus_(SearchFailed);
                return;
            }

            this->onlineVersion_ = releaseVersion;
            this->updateDownloadUrl_ = downloadUrl;

            this->isDowngrade_ =
                Updates::isDowngradeOf(releaseVersion, this->currentVersion_);
            if (isNewerVersion(releaseVersion, this->currentVersion_))
            {
                this->setStatus_(UpdateAvailable);
                return;
            }

            this->setStatus_(NoUpdateAvailable);
        })
        .onError([this](NetworkResult result) {
            qCWarning(chatterinoUpdate)
                << "Failed to check Mergerino updates:"
                << result.formatError();
            this->setStatus_(SearchFailed);
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
    return this->status_ == UpdateAvailable;
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
    return QString("A new Mergerino version is available.\n\nCurrent: %1\nLatest: "
                   "%2\n\nInstall it and restart Mergerino?")
        .arg(this->currentVersion_, this->onlineVersion_);
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

void Updates::downloadAndRunInstaller_()
{
    if (this->updateDownloadUrl_.isEmpty())
    {
        this->setStatus_(DownloadFailed);
        return;
    }

    this->setStatus_(Downloading);

    NetworkRequest(QUrl(this->updateDownloadUrl_))
        .header("Accept", "application/octet-stream")
        .followRedirects(true)
        .timeout(120000)
        .onSuccess([this](NetworkResult result) {
            const auto data = result.getData();
            if (data.size() < 1024)
            {
                qCWarning(chatterinoUpdate)
                    << "Downloaded Mergerino update was unexpectedly small";
                this->setStatus_(DownloadFailed);
                return;
            }

            if (!QDir().mkpath(this->paths.miscDirectory))
            {
                this->setStatus_(WriteFileFailed);
                return;
            }

            const auto zipPath =
                combinePath(this->paths.miscDirectory, "update.zip");
            QSaveFile zipFile(zipPath);
            if (!zipFile.open(QIODevice::WriteOnly) ||
                zipFile.write(data) != data.size() || !zipFile.commit())
            {
                qCWarning(chatterinoUpdate)
                    << "Failed to write Mergerino update archive" << zipPath;
                this->setStatus_(WriteFileFailed);
                return;
            }

            const auto scriptPath =
                combinePath(this->paths.miscDirectory, "install-update.ps1");
            const auto script = QStringLiteral(R"powershell(
param(
    [Parameter(Mandatory=$true)][int]$MergerinoProcessId,
    [Parameter(Mandatory=$true)][string]$ZipPath,
    [Parameter(Mandatory=$true)][string]$AppDir,
    [Parameter(Mandatory=$true)][string]$ExeName
)

$ErrorActionPreference = 'Stop'

try {
    Wait-Process -Id $MergerinoProcessId -Timeout 60 -ErrorAction SilentlyContinue
} catch {
}

Start-Sleep -Milliseconds 500

Add-Type -AssemblyName System.IO.Compression.FileSystem

$stage = Join-Path ([System.IO.Path]::GetTempPath()) ('MergerinoUpdate-' + [Guid]::NewGuid().ToString('N'))
[System.IO.Directory]::CreateDirectory($stage) | Out-Null

[System.IO.Compression.ZipFile]::ExtractToDirectory($ZipPath, $stage)

$payload = $stage
$nestedPayload = Join-Path $stage 'Mergerino'
if (Test-Path -LiteralPath (Join-Path $nestedPayload 'mergerino.exe')) {
    $payload = $nestedPayload
} elseif (-not (Test-Path -LiteralPath (Join-Path $payload 'mergerino.exe'))) {
    throw 'Update archive does not contain mergerino.exe.'
}

Get-ChildItem -LiteralPath $payload | Copy-Item -Destination $AppDir -Recurse -Force

Start-Process -FilePath (Join-Path $AppDir $ExeName) -WorkingDirectory $AppDir
)powershell");

            const auto scriptBytes = script.toUtf8();
            QSaveFile scriptFile(scriptPath);
            if (!scriptFile.open(QIODevice::WriteOnly) ||
                scriptFile.write(scriptBytes) != scriptBytes.size() ||
                !scriptFile.commit())
            {
                qCWarning(chatterinoUpdate)
                    << "Failed to write Mergerino update script" << scriptPath;
                this->setStatus_(WriteFileFailed);
                return;
            }

            const auto appPath = QCoreApplication::applicationFilePath();
            const QFileInfo appInfo(appPath);
            const QStringList args{
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                QDir::toNativeSeparators(scriptPath),
                "-MergerinoProcessId",
                QString::number(QCoreApplication::applicationPid()),
                "-ZipPath",
                QDir::toNativeSeparators(zipPath),
                "-AppDir",
                QDir::toNativeSeparators(QCoreApplication::applicationDirPath()),
                "-ExeName",
                appInfo.fileName(),
            };

            if (!QProcess::startDetached("powershell.exe", args))
            {
                this->setStatus_(RunUpdaterFailed);
                return;
            }

            QApplication::quit();
        })
        .onError([this](NetworkResult result) {
            qCWarning(chatterinoUpdate)
                << "Failed to download Mergerino update:"
                << result.formatError();
            this->setStatus_(DownloadFailed);
        })
        .execute();
}

}  // namespace chatterino
