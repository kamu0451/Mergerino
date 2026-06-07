// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/UpdateDialog.hpp"

#include "Application.hpp"
#include "common/Version.hpp"
#include "singletons/Updates.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/Label.hpp"

#include <QCoreApplication>
#include <QDate>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringList>
#include <QTextEdit>
#include <QVBoxLayout>

#include <optional>

namespace chatterino {
namespace {

QString trimPatchNotesSection(QStringList lines)
{
    while (!lines.isEmpty() && lines.first().trimmed().isEmpty())
    {
        lines.removeFirst();
    }

    while (!lines.isEmpty() && lines.last().trimmed().isEmpty())
    {
        lines.removeLast();
    }

    return lines.join('\n').trimmed();
}

std::optional<QDate> parsePatchNotesDateHeader(const QString &line)
{
    static const QRegularExpression dateHeader{
        QStringLiteral(R"(^(\d{1,2})/(\d{1,2})/(\d{4})$)")};

    const auto match = dateHeader.match(line.trimmed());
    if (!match.hasMatch())
    {
        return std::nullopt;
    }

    const auto date =
        QDate(match.captured(3).toInt(), match.captured(2).toInt(),
              match.captured(1).toInt());
    if (!date.isValid())
    {
        return std::nullopt;
    }

    return date;
}

QString firstPatchNotesSectionFrom(const QStringList &lines)
{
    QStringList sectionLines;
    bool foundSection = false;
    bool foundContent = false;

    for (const auto &line : lines)
    {
        const auto trimmed = line.trimmed();

        if (!foundSection)
        {
            if (trimmed.isEmpty() ||
                trimmed.compare(QStringLiteral("Patch Notes"),
                                Qt::CaseInsensitive) == 0)
            {
                continue;
            }

            foundSection = true;
        }
        else if (trimmed.isEmpty() && foundContent)
        {
            break;
        }

        if (!trimmed.isEmpty())
        {
            foundContent = true;
        }

        sectionLines.append(trimmed);
    }

    const auto latestSection = sectionLines.join('\n').trimmed();
    return latestSection;
}

QString latestPatchNotesFrom(QString text)
{
    const auto lines =
        text.replace("\r\n", "\n").replace('\r', '\n').split('\n');

    QDate latestDate;
    QStringList latestSectionLines;
    QDate currentDate;
    QStringList currentSectionLines;

    const auto commitCurrentSection = [&] {
        if (!currentDate.isValid())
        {
            return;
        }

        if (!latestDate.isValid() || currentDate > latestDate)
        {
            latestDate = currentDate;
            latestSectionLines = currentSectionLines;
        }
    };

    for (const auto &line : lines)
    {
        const auto trimmed = line.trimmed();

        if (const auto date = parsePatchNotesDateHeader(trimmed))
        {
            commitCurrentSection();
            currentDate = *date;
            currentSectionLines.clear();
            continue;
        }

        if (currentDate.isValid())
        {
            currentSectionLines.append(trimmed);
        }
    }
    commitCurrentSection();

    const auto latestSection = trimPatchNotesSection(latestSectionLines);
    if (!latestSection.isEmpty())
    {
        return latestSection;
    }

    const auto firstSection = firstPatchNotesSectionFrom(lines);
    return firstSection.isEmpty() ? text.trimmed() : firstSection;
}

QString loadLatestPatchNotes()
{
    const QStringList paths = {
        QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("patchnotes.txt")),
        QStringLiteral(":/patchnotes.txt"),
    };

    for (const auto &path : paths)
    {
        QFile file(path);
        if (!file.open(QFile::ReadOnly | QFile::Text))
        {
            continue;
        }

        auto text = QString::fromUtf8(file.readAll()).trimmed();
        if (!text.isEmpty())
        {
            return latestPatchNotesFrom(text);
        }
    }

    return QStringLiteral("No patch notes were found.");
}

}  // namespace

PostUpdateDialog::PostUpdateDialog(const QString &version, QWidget *parent)
    : BaseWindow({}, parent)
{
    this->setWindowTitle("Mergerino updated");
    this->setAttribute(Qt::WA_DeleteOnClose);

    auto *layout = new QVBoxLayout(this->getLayoutContainer());
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    auto *title = new QLabel(
        QStringLiteral("Mergerino has been successfully updated to %1")
            .arg(version),
        this);
    title->setWordWrap(true);
    layout->addWidget(title);

    auto *patchNotes = new QTextEdit(this);
    patchNotes->setReadOnly(true);
    patchNotes->setText(loadLatestPatchNotes());
    patchNotes->setMinimumHeight(170);
    layout->addWidget(patchNotes);

    auto *link = new QLabel(
        QStringLiteral("<a href=\"https://github.com/Fixlation/Mergerino/blob/"
                       "main/patchnotes.txt\">View all patch notes here</a>"),
        this);
    link->setTextFormat(Qt::RichText);
    link->setTextInteractionFlags(Qt::TextBrowserInteraction |
                                  Qt::LinksAccessibleByKeyboard);
    link->setOpenExternalLinks(true);
    layout->addWidget(link);

    this->setScaleIndependentWidth(420);
    this->setScaleIndependentHeight(300);
}

UpdateDialog::UpdateDialog()
    : BaseWindow({BaseWindow::Frameless, BaseWindow::TopMost,
                  BaseWindow::EnableCustomFrame, BaseWindow::DisableLayoutSave})
{
    this->windowDeactivateAction = WindowDeactivateAction::Delete;

    auto layout =
        LayoutCreator<UpdateDialog>(this).setLayoutType<QVBoxLayout>();

    layout.emplace<Label>("You shouldn't be seeing this dialog.")
        .assign(&this->ui_.label)
        ->setWordWrap(true);

    auto buttons = layout.emplace<QDialogButtonBox>();

    const auto *installText = [] {
        if (Version::instance().isNightly())
        {
            return "Yes";
        }

        return "Install";
    }();
    auto *install =
        buttons->addButton(installText, QDialogButtonBox::AcceptRole);
    this->ui_.installButton = install;
    auto *dismiss = buttons->addButton("Dismiss", QDialogButtonBox::RejectRole);

    QObject::connect(install, &QPushButton::clicked, this, [this] {
        getApp()->getUpdates().installUpdates();
    });
    QObject::connect(dismiss, &QPushButton::clicked, this, [this] {
        this->dismissed.invoke();
        this->close();
    });

    this->updateStatusChanged(getApp()->getUpdates().getStatus());
    this->connections_.managedConnect(getApp()->getUpdates().statusUpdated,
                                      [this](auto status) {
                                          this->updateStatusChanged(status);
                                      });

    this->setScaleIndependentHeight(160);
    this->setScaleIndependentWidth(380);
}

void UpdateDialog::updateStatusChanged(Updates::Status status)
{
    this->ui_.installButton->setVisible(status == Updates::UpdateAvailable);
    this->ui_.installButton->setEnabled(status == Updates::UpdateAvailable);

    switch (status)
    {
        case Updates::None: {
            this->ui_.label->setText("Check for updates to load the latest "
                                     "Mergerino release information.");
            this->updateGeometry();
        }
        break;

        case Updates::Searching: {
            this->ui_.label->setText("Checking for Mergerino updates...");
            this->updateGeometry();
        }
        break;

        case Updates::UpdateAvailable: {
            this->ui_.label->setText(
                getApp()->getUpdates().buildUpdateAvailableText());
            this->updateGeometry();
        }
        break;

        case Updates::NoUpdateAvailable: {
            const auto latestVersion = getApp()->getUpdates().getOnlineVersion();
            if (latestVersion.isEmpty())
            {
                this->ui_.label->setText("Mergerino is up to date.");
            }
            else
            {
                this->ui_.label->setText(
                    QString("Mergerino is up to date.\n\nCurrent: %1\nLatest: "
                            "%2")
                        .arg(getApp()->getUpdates().getCurrentVersion(),
                             latestVersion));
            }
            this->updateGeometry();
        }
        break;

        case Updates::SearchFailed: {
            this->ui_.label->setText("Failed to load version information.");
            this->updateGeometry();
        }
        break;

        case Updates::Downloading: {
            this->ui_.label->setText(
                "Downloading updates.\n\nMergerino will restart "
                "automatically when the download is done.");
            this->updateGeometry();
        }
        break;

        case Updates::DownloadFailed: {
            this->ui_.label->setText("Failed to download the update.");
            this->updateGeometry();
        }
        break;

        case Updates::WriteFileFailed: {
            this->ui_.label->setText("Failed to save the update to disk.");
            this->updateGeometry();
        }
        break;

        case Updates::MissingPortableUpdater: {
            this->ui_.label->setText("The portable updater (expected in " %
                                     Updates::portableUpdaterPath() %
                                     ") was not found.");
            this->updateGeometry();
        }
        break;

        case Updates::RunUpdaterFailed: {
            this->ui_.label->setText("Failed to run the updater.");
            this->updateGeometry();
        }
        break;
    }
}

}  // namespace chatterino
