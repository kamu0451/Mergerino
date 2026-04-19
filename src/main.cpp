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
#include "singletons/CrashHandler.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Updates.hpp"
#include "util/AttachToConsole.hpp"
#include "util/IpcQueue.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QFile>
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

QFile *g_logFile = nullptr;
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

    QMutexLocker lock(&g_logFileMutex);
    g_logFile->write(line.toUtf8());
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

    if (args.logFile.has_value())
    {
        auto *file = new QFile(*args.logFile);
        if (file->open(QIODevice::WriteOnly | QIODevice::Append |
                       QIODevice::Text))
        {
            g_logFile = file;
            g_prevMessageHandler = qInstallMessageHandler(fileLogMessageHandler);
            qCInfo(chatterinoApp).noquote()
                << "Logging to file:" << *args.logFile;
        }
        else
        {
            std::cerr << "Failed to open --log-file at "
                      << args.logFile->toLocal8Bit().constData() << ": "
                      << file->errorString().toLocal8Bit().constData() << '\n';
            delete file;
        }
    }

#ifdef CHATTERINO_WITH_CRASHPAD
    const auto crashpadHandler = installCrashHandler(args, *paths);
#endif

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
