// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/AccountSwitchPopup.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "common/ProviderId.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/AccountSwitchWidget.hpp"
#include "widgets/dialogs/KickLoginPage.hpp"
#include "widgets/dialogs/LoginDialog.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/helper/KickAccountSwitchWidget.hpp"

#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QPainter>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>

namespace chatterino {

using namespace literals;

namespace {

QString providerName(ProviderId provider)
{
    switch (provider)
    {
        case ProviderId::Kick:
            return "Kick";
        case ProviderId::Twitch:
        default:
            return "Twitch";
    }
}

QString providerIconPath(ProviderId provider)
{
    switch (provider)
    {
        case ProviderId::Kick:
            return u":/platforms/kick.svg"_s;
        case ProviderId::Twitch:
        default:
            return u":/platforms/twitch.svg"_s;
    }
}

}  // namespace

AccountSwitchPopup::AccountSwitchPopup(QWidget *parent)
    : BaseWindow(
          {
              BaseWindow::TopMost,
              BaseWindow::Frameless,
              BaseWindow::DisableLayoutSave,
              BaseWindow::LinuxPopup,
          },
          parent)
{
    this->focusOutAction = FocusOutAction::Hide;

    this->setContentsMargins(0, 0, 0, 0);

    auto *vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(10, 8, 10, 10);
    vbox->setSpacing(6);

    auto *providerHbox = new QHBoxLayout();
    providerHbox->setContentsMargins(0, 0, 0, 0);
    providerHbox->setSpacing(4);

    this->ui_.twitchProviderButton = new QPushButton(this);
    this->ui_.twitchProviderButton->setObjectName("providerButton");
    this->ui_.twitchProviderButton->setIcon(
        QIcon(providerIconPath(ProviderId::Twitch)));
    this->ui_.twitchProviderButton->setIconSize(QSize{16, 16});
    this->ui_.twitchProviderButton->setCheckable(true);
    this->ui_.twitchProviderButton->setFocusPolicy(Qt::NoFocus);
    this->ui_.twitchProviderButton->setFixedSize(32, 24);
    providerHbox->addWidget(this->ui_.twitchProviderButton);

    this->ui_.kickProviderButton = new QPushButton(this);
    this->ui_.kickProviderButton->setObjectName("providerButton");
    this->ui_.kickProviderButton->setIcon(
        QIcon(providerIconPath(ProviderId::Kick)));
    this->ui_.kickProviderButton->setIconSize(QSize{16, 16});
    this->ui_.kickProviderButton->setCheckable(true);
    this->ui_.kickProviderButton->setFocusPolicy(Qt::NoFocus);
    this->ui_.kickProviderButton->setFixedSize(32, 24);
    providerHbox->addWidget(this->ui_.kickProviderButton);
    providerHbox->addStretch(1);
    vbox->addLayout(providerHbox);

    this->ui_.accountStack = new QStackedWidget(this);
    vbox->addWidget(this->ui_.accountStack, 1);

    this->ui_.accountSwitchWidget = new AccountSwitchWidget(this);
    this->ui_.accountSwitchWidget->setFocusPolicy(Qt::NoFocus);
    this->ui_.kickAccountSwitcher = new KickAccountSwitchWidget(this);
    this->ui_.kickAccountSwitcher->setFocusPolicy(Qt::NoFocus);
    this->ui_.accountStack->addWidget(this->ui_.accountSwitchWidget);
    this->ui_.accountStack->addWidget(this->ui_.kickAccountSwitcher);

    this->ui_.statusLabel = new QLabel(this);
    this->ui_.statusLabel->setWordWrap(true);
    vbox->addWidget(this->ui_.statusLabel);

    auto *hbox = new QHBoxLayout();
    this->ui_.loginButton = new QPushButton(this);
    this->ui_.loginButton->setFocusPolicy(Qt::NoFocus);
    hbox->addWidget(this->ui_.loginButton, 1);

    this->ui_.manageAccountsButton = new QPushButton(this);
    this->ui_.manageAccountsButton->setText("Manage Accounts");
    this->ui_.manageAccountsButton->setFocusPolicy(Qt::NoFocus);
    hbox->addWidget(this->ui_.manageAccountsButton, 1);
    vbox->addLayout(hbox);

    QObject::connect(this->ui_.loginButton, &QPushButton::clicked, this,
                     [this]() {
                         const auto provider =
                             getApp()->getWindows()->activeAccountProvider();
                         if (provider == ProviderId::Kick)
                         {
                             auto current = getApp()->getAccounts()->kick.current();
                             if (!current->isAnonymous())
                             {
                                 getApp()->getAccounts()->kick.currentUsername =
                                     "";
                                 this->refresh();
                                 return;
                             }

                             KickLoginPage::startLoginFlow(
                                 this->parentWidget(),
                                 [this]() { this->refresh(); });
                             this->hide();
                             return;
                         }

                         auto current = getApp()->getAccounts()->twitch.getCurrent();
                         if (!current->isAnon())
                         {
                             getApp()->getAccounts()->twitch.currentUsername =
                                 "";
                             this->refresh();
                             return;
                         }

                         LoginDialog dialog(
                             this,
                             provider);
                         dialog.exec();
                         this->refresh();
                     });
    QObject::connect(this->ui_.twitchProviderButton, &QPushButton::clicked,
                     this, [this]() {
                         getApp()->getWindows()->setActiveAccountProvider(
                             ProviderId::Twitch);
                         this->refresh();
                     });
    QObject::connect(this->ui_.kickProviderButton, &QPushButton::clicked,
                     this, [this]() {
                         getApp()->getWindows()->setActiveAccountProvider(
                             ProviderId::Kick);
                         this->refresh();
                     });
    QObject::connect(this->ui_.manageAccountsButton, &QPushButton::clicked,
                     [this]() {
        SettingsDialog::showDialog(this->parentWidget(),
                                   SettingsDialogPreference::Accounts);
    });

    this->getLayoutContainer()->setLayout(vbox);

    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->kick.userListUpdated,
        [this]() { this->refresh(); });
    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->kick.currentUserChanged,
        [this]() { this->refresh(); });
    this->signalHolder_.managedConnect(
        getApp()->getWindows()->activeAccountProviderChanged,
        [this](ProviderId) { this->refresh(); });
    this->bSignals_.emplace_back(
        getApp()->getAccounts()->twitch.currentUserChanged.connect([this] {
            this->refresh();
        }));
    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->twitch.userListUpdated,
        [this]() { this->refresh(); });

    this->setScaleIndependentSize(260, 260);
    this->refresh();
    this->themeChangedEvent();
}

bool AccountSwitchPopup::wasHiddenRecently(int thresholdMs) const
{
    return this->lastHideTimer_.isValid() &&
           this->lastHideTimer_.elapsed() < thresholdMs;
}

void AccountSwitchPopup::hideEvent(QHideEvent *event)
{
    this->lastHideTimer_.restart();
    BaseWindow::hideEvent(event);
}

void AccountSwitchPopup::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();

    auto *t = getTheme();
    auto color = [](const QColor &c) {
        return c.name(QColor::HexArgb);
    };
    this->setStyleSheet(uR"(
        QListView {
            color: %1;
            background: %2;
        }
        QLabel {
            color: %1;
        }
        QListView::item:hover {
            background: %3;
        }
        QListView::item:selected {
            background: %4;
        }

        QPushButton {
            background: %5;
            color: %1;
        }
        QPushButton:checked {
            background: %4;
        }
        QPushButton:hover {
            background: %3;
        }
        QPushButton:pressed {
            background: %6;
        }
        QPushButton#providerButton {
            background: transparent;
            border: 1px solid #12555555;
            padding: 0;
        }
        QPushButton#providerButton[selectedProvider="true"] {
            background: transparent;
            border: 1px solid #6a6a6a;
        }
        QPushButton#providerButton:hover {
            background: transparent;
            border: 1px solid #24555555;
        }
        QPushButton#providerButton[selectedProvider="true"]:hover {
            background: transparent;
            border: 1px solid #777777;
        }
        QPushButton#providerButton[selectedProvider="true"]:pressed {
            background: transparent;
            border: 1px solid #858585;
        }

        chatterino--AccountSwitchPopup {
            background: %7;
        }
    )"_s.arg(color(t->window.text), color(t->splits.header.background),
             color(t->splits.header.focusedBackground), color(t->accent),
             color(t->tabs.regular.backgrounds.regular),
             color(t->tabs.selected.backgrounds.regular),
             color(t->window.background)));
}

void AccountSwitchPopup::refresh()
{
    this->ui_.accountSwitchWidget->refresh();
    this->ui_.kickAccountSwitcher->refresh();
    this->updateCurrentPage();
    this->updateStatusText();
}

void AccountSwitchPopup::paintEvent(QPaintEvent *)
{
    QPainter painter(this);

    painter.setPen(QColor("#999"));
    painter.drawRect(0, 0, this->width() - 1, this->height() - 1);
}

void AccountSwitchPopup::updateCurrentPage()
{
    const auto provider = getApp()->getWindows()->activeAccountProvider();
    this->ui_.accountStack->setCurrentWidget(
        provider == ProviderId::Kick
            ? static_cast<QWidget *>(this->ui_.kickAccountSwitcher)
            : static_cast<QWidget *>(this->ui_.accountSwitchWidget));
    auto setProviderButtonSelected = [](QPushButton *button, bool selected) {
        button->setChecked(selected);
        button->setProperty("selectedProvider", selected);
        button->style()->unpolish(button);
        button->style()->polish(button);
        button->update();
    };
    setProviderButtonSelected(this->ui_.twitchProviderButton,
                              provider == ProviderId::Twitch);
    setProviderButtonSelected(this->ui_.kickProviderButton,
                              provider == ProviderId::Kick);

    const bool loggedIn =
        provider == ProviderId::Kick
            ? !getApp()->getAccounts()->kick.current()->isAnonymous()
            : !getApp()->getAccounts()->twitch.getCurrent()->isAnon();
    this->ui_.loginButton->setText(
        QString("%1 %2")
            .arg(loggedIn ? "Log out of" : "Log in to")
            .arg(providerName(provider)));
}

void AccountSwitchPopup::updateStatusText()
{
    const auto provider = getApp()->getWindows()->activeAccountProvider();
    if (provider == ProviderId::Kick)
    {
        const auto usernames = getApp()->getAccounts()->kick.usernames();
        const auto current = getApp()->getAccounts()->kick.current();
        if (!current->isAnonymous())
        {
            this->ui_.statusLabel->setText(
                QString("Current Kick account: %1").arg(current->username()));
            return;
        }
        if (!usernames.empty())
        {
            this->ui_.statusLabel->setText(
                "Kick is currently set to anonymous. Select an account above "
                "or log in.");
            return;
        }

        this->ui_.statusLabel->setText(
            "No Kick account added yet. Click below to log in.");
        return;
    }

    const auto usernames = getApp()->getAccounts()->twitch.getUsernames();
    const auto current = getApp()->getAccounts()->twitch.getCurrent();
    if (!current->isAnon())
    {
        this->ui_.statusLabel->setText(
            QString("Current Twitch account: %1").arg(current->getUserName()));
        return;
    }
    if (!usernames.empty())
    {
        this->ui_.statusLabel->setText(
            "Twitch is currently set to anonymous. Select an account above "
            "or log in.");
        return;
    }

    this->ui_.statusLabel->setText(
        "No Twitch account added yet. Click below to log in.");
}

}  // namespace chatterino
