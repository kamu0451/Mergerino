// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/LastRunCrashDialog.hpp"

#include "common/Args.hpp"
#include "common/Version.hpp"  // IWYU pragma: keep
#include "singletons/Paths.hpp"
#include "util/LayoutCreator.hpp"

#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRandomGenerator>
#include <QStringBuilder>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace {

const std::initializer_list<QString> MESSAGES = {
    u"Oops..."_s,        u"NotLikeThis"_s,
    u"NOOOOOO"_s,        u"I'm sorry"_s,
    u"We're sorry"_s,    u"My bad"_s,
    u"FailFish"_s,       u"O_o"_s,
    u"Sorry :("_s,       u"I blame cosmic rays"_s,
    u"I blame TMI"_s,    u"I blame Helix"_s,
    u"Oopsie woopsie"_s,
};

QString randomMessage()
{
    return *(MESSAGES.begin() +
             (QRandomGenerator::global()->generate64() % MESSAGES.size()));
}

}  // namespace

namespace chatterino {

LastRunCrashDialog::LastRunCrashDialog(const Args &, const Paths &)
{
    this->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    this->setWindowTitle(u"Mergerino - " % randomMessage());

    auto layout =
        LayoutCreator<LastRunCrashDialog>(this).setLayoutType<QVBoxLayout>();

    QString text =
        u"Mergerino unexpectedly crashed and restarted. "_s
        "<i>You can disable automatic restarts in the settings.</i><br><br>";

    auto label = layout.emplace<QLabel>(text);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    label->setOpenExternalLinks(true);
    label->setWordWrap(true);

    layout->addSpacing(16);

    auto buttons = layout.emplace<QDialogButtonBox>();

    auto *okButton = buttons->addButton(u"Ok"_s, QDialogButtonBox::AcceptRole);
    QObject::connect(okButton, &QPushButton::clicked, [this] {
        this->accept();
    });
}

}  // namespace chatterino
