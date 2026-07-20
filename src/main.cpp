// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "BrowserExtension.hpp"
#include "common/Args.hpp"
#include "common/Env.hpp"
#include "common/Modes.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "providers/IvrApi.hpp"
#include "providers/NetworkConfigurationProvider.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "RunGui.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Updates.hpp"
#include "util/AttachToConsole.hpp"
#include "util/ChatterinoImport.hpp"
#include "util/IpcQueue.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QSslSocket>
#include <QStringList>
#include <QtCore/QtPlugin>
#ifdef Q_OS_WIN
#    include <shobjidl_core.h>
#endif

#include <memory>

#ifdef CHATTERINO_WITH_AVIF_PLUGIN
Q_IMPORT_PLUGIN(QAVIFPlugin)
#endif

using namespace chatterino;

namespace {

// Hard cap on the --log-file size. Once a write would push the file past
// this, we roll over to a single "<path>.1" backup and start a fresh file,
// bounding on-disk usage at roughly 2x this value. 32 MiB is large enough to
// keep a full chatty session for post-mortem review but small enough that an
// unbounded run cannot fill the disk.
constexpr qint64 MAX_LOG_FILE_BYTES = 32 * 1024 * 1024;

QFile *g_logFile = nullptr;
QString g_logFilePath;
QtMessageHandler g_prevMessageHandler = nullptr;
QMutex g_logFileMutex;

void fileLogMessageHandler(QtMsgType type, const QMessageLogContext &ctx,
                           const QString &msg)
{
    if (g_prevMessageHandler != nullptr)
    {
        g_prevMessageHandler(type, ctx, msg);
    }

    if (g_logFile == nullptr)
    {
        return;
    }

    const char *typeStr = "???";
    switch (type)
    {
        case QtDebugMsg:
            typeStr = "DEBUG";
            break;
        case QtInfoMsg:
            typeStr = "INFO";
            break;
        case QtWarningMsg:
            typeStr = "WARN";
            break;
        case QtCriticalMsg:
            typeStr = "CRIT";
            break;
        case QtFatalMsg:
            typeStr = "FATAL";
            break;
    }

    const QString line =
        QStringLiteral("%1 [%2] %3: %4\n")
            .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
                 QString::fromLatin1(typeStr),
                 QString::fromLatin1(ctx.category != nullptr ? ctx.category
                                                             : "default"),
                 msg);

    const QByteArray lineBytes = line.toUtf8();

    QMutexLocker lock(&g_logFileMutex);

    // Re-check under the lock: rotation below may have torn the file down and
    // failed to reopen it, in which case file logging is permanently off.
    if (g_logFile == nullptr)
    {
        return;
    }

    // Roll over before this write would exceed the cap. We keep a single ".1"
    // backup: flush+close the current file, replace any existing "<path>.1"
    // with it via rename, then open a fresh truncated "<path>" as the new
    // sink. If the rename fails (e.g. the backup is held open elsewhere), the
    // original file is left in place, so reopen it in Append mode instead of
    // Truncate - we'd rather temporarily exceed the size cap than destroy the
    // log. On reopen failure we stop file logging (g_logFile = nullptr)
    // rather than crash; we must NOT emit a qC* log here or we would recurse
    // straight back into this handler.
    if (!g_logFilePath.isEmpty() &&
        g_logFile->size() + static_cast<qint64>(lineBytes.size()) >
            MAX_LOG_FILE_BYTES)
    {
        g_logFile->flush();
        g_logFile->close();
        delete g_logFile;
        g_logFile = nullptr;

        const QString backupPath = g_logFilePath + QStringLiteral(".1");
        QFile::remove(backupPath);
        const bool renamed = QFile::rename(g_logFilePath, backupPath);

        auto *rolled = new QFile(g_logFilePath);
        const QIODevice::OpenMode openMode =
            renamed ? (QIODevice::WriteOnly | QIODevice::Truncate |
                      QIODevice::Text)
                    : (QIODevice::WriteOnly | QIODevice::Append |
                      QIODevice::Text);
        if (rolled->open(openMode))
        {
            g_logFile = rolled;
        }
        else
        {
            std::cerr << "Failed to reopen --log-file after rotation at "
                      << g_logFilePath.toLocal8Bit().constData() << ": "
                      << rolled->errorString().toLocal8Bit().constData()
                      << '\n';
            delete rolled;
            g_logFile = nullptr;
            return;
        }
    }

    g_logFile->write(lineBytes);
    g_logFile->flush();
}

}  // namespace

int main(int argc, char **argv)
{
    QApplication a(argc, argv);

    QCoreApplication::setApplicationName("mergerino");
    QCoreApplication::setApplicationVersion(CHATTERINO_VERSION);
    QCoreApplication::setOrganizationDomain("mergerino.app");
#ifdef Q_OS_WIN
    SetCurrentProcessExplicitAppUserModelID(
        Version::instance().appUserModelID().c_str());
#endif

    std::unique_ptr<Paths> paths;

    try
    {
        paths = std::make_unique<Paths>();
    }
    catch (std::runtime_error &error)
    {
        QMessageBox box;
        if (Modes::instance().isPortable)
        {
            auto errorMessage =
                error.what() +
                QStringLiteral(
                    "\n\nInfo: Portable mode requires the application to "
                    "be in a writeable location. If you don't want "
                    "portable mode reinstall the application.");
            std::cerr << errorMessage.toLocal8Bit().constData() << '\n';
            std::cerr.flush();
            box.setText(errorMessage);
        }
        else
        {
            box.setText(error.what());
        }
        box.exec();
        return 1;
    }
    ipc::initPaths(paths.get());

    const Args args(a, *paths);

    // Diagnostic file logging is always on: without an explicit --log-file
    // (the dev cycle passes one) it defaults to a rotating log under the
    // app-data directory, so a "provider X didn't pick the stream up" report
    // is diagnosable post-mortem instead of needing a relaunch with special
    // flags that destroys the live reproduction state.
    // Only the GUI run gets the default sink: the browser-extension host is
    // a second concurrent mergerino.exe process, and two writers on one
    // append-mode file interleave and race the size-cap rotation on Windows.
    QString logFilePath;
    if (args.logFile.has_value())
    {
        logFilePath = *args.logFile;
    }
    else if (!args.shouldRunBrowserExtensionHost && !args.printVersion)
    {
        const QString logDir = paths->rootAppDataDirectory + "/Logs";
        if (QDir().mkpath(logDir))
        {
            logFilePath = logDir + "/mergerino-diag.log";
        }
    }

    if (!logFilePath.isEmpty())
    {
        auto *file = new QFile(logFilePath);
        if (file->open(QIODevice::WriteOnly | QIODevice::Append |
                       QIODevice::Text))
        {
            g_logFile = file;
            g_logFilePath = logFilePath;
            g_prevMessageHandler = qInstallMessageHandler(fileLogMessageHandler);
            // With a file sink attached, raise the Mergerino provider
            // categories to Debug (their liveness/poll state machines only
            // narrate at qCDebug) and chatterino.app to Info (session-start
            // markers in the append-mode file). QT_LOGGING_RULES still
            // overrides these: Qt applies env rules after API rules.
            QLoggingCategory::setFilterRules(
                QStringLiteral("chatterino.youtube.debug=true\n"
                               "chatterino.merged.debug=true\n"
                               "chatterino.kick.debug=true\n"
                               "chatterino.tiktok.debug=true\n"
                               "chatterino.app.info=true"));
            qCInfo(chatterinoApp).noquote()
                << "Logging to file:" << logFilePath << "- Mergerino"
                << CHATTERINO_VERSION;
        }
        else
        {
            std::cerr << "Failed to open log file at "
                      << logFilePath.toLocal8Bit().constData() << ": "
                      << file->errorString().toLocal8Bit().constData() << '\n';
            delete file;
        }
    }

    // run in gui mode or browser extension host mode
    if (args.shouldRunBrowserExtensionHost)
    {
#ifdef Q_OS_MACOS
        ::chatterinoSetMacOsActivationPolicyProhibited();
#endif
        runBrowserExtensionHost();
    }
    else if (args.printVersion)
    {
        attachToConsole();

        auto version = Version::instance();
        auto versionMessage =
            QString("%1 (commit %2%3)")
                .arg(version.fullVersion())
                .arg(version.commitHash())
                .arg(version.isNightly() ? ", " + version.dateOfBuild() : "");
        std::cout << versionMessage.toLocal8Bit().constData() << '\n';
        std::cout.flush();
    }
    else
    {
        if (args.verbose)
        {
            attachToConsole();
        }

        qCInfo(chatterinoApp).noquote()
            << "Mergerino Qt SSL library build version:"
            << QSslSocket::sslLibraryBuildVersionString();
        qCInfo(chatterinoApp).noquote()
            << "Mergerino Qt SSL library version:"
            << QSslSocket::sslLibraryVersionString();
        qCInfo(chatterinoApp).noquote()
            << "Mergerino Qt SSL active backend:"
            << QSslSocket::activeBackend() << "of"
            << QSslSocket::availableBackends().join(", ");
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
        qCInfo(chatterinoApp) << "Mergerino Qt SSL active backend features:"
                              << QSslSocket::supportedFeatures();
#endif
        qCInfo(chatterinoApp) << "Mergerino Qt SSL active backend protocols:"
                              << QSslSocket::supportedProtocols();

        if (chatterino_import::hasPendingImport(*paths))
        {
            auto result = chatterino_import::applyPendingImport(*paths);
            if (!result)
            {
                QMessageBox::warning(
                    nullptr, "Chatterino import failed",
                    "Mergerino could not import Chatterino settings:\n\n" +
                        result.error());
            }
        }

        Settings settings(args, paths->settingsDirectory);

        Updates updates(*paths, settings);

        NetworkConfigurationProvider::applyFromEnv(Env::get());

        IvrApi::initialize();
        Helix::initialize();

        runGui(a, *paths, settings, args, updates);
    }

    if (g_prevMessageHandler != nullptr)
    {
        qInstallMessageHandler(g_prevMessageHandler);
        g_prevMessageHandler = nullptr;
    }
    {
        QMutexLocker lock(&g_logFileMutex);
        if (g_logFile != nullptr)
        {
            g_logFile->close();
            delete g_logFile;
            g_logFile = nullptr;
        }
    }

    return 0;
}
