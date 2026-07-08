// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/AccountsPage.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/accounts/AccountModel.hpp"
#include "providers/twitch/api/TwitchModerationAuth.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/dialogs/LoginDialog.hpp"
#include "widgets/helper/EditableModelView.hpp"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace chatterino {

AccountsPage::AccountsPage()
{
    auto *app = getApp();

    LayoutCreator<AccountsPage> layoutCreator(this);
    auto layout = layoutCreator.emplace<QVBoxLayout>().withoutMargin();

    EditableModelView *view =
        layout
            .emplace<EditableModelView>(
                app->getAccounts()->createModel(nullptr), false)
            .getElement();

    view->getTableView()->horizontalHeader()->setVisible(false);
    view->getTableView()->horizontalHeader()->setStretchLastSection(true);

    // We can safely ignore this signal connection since we own the view
    std::ignore = view->addButtonPressed.connect([this] {
        LoginDialog d(this);
        d.exec();
    });

    view->getTableView()->setStyleSheet("background: #333");

    auto *moderationAuthFrame = new QFrame(this);
    moderationAuthFrame->setFrameShape(QFrame::StyledPanel);
    auto *moderationAuthLayout = new QVBoxLayout(moderationAuthFrame);
    moderationAuthLayout->setContentsMargins(10, 8, 10, 8);
    moderationAuthLayout->setSpacing(6);

    auto *moderationAuthTitle =
        new QLabel(QStringLiteral("Twitch mod actions"), moderationAuthFrame);
    moderationAuthTitle->setStyleSheet(
        QStringLiteral("QLabel { font-weight: 700; }"));
    moderationAuthLayout->addWidget(moderationAuthTitle);

    auto *moderationAuthDescription = new QLabel(
        QStringLiteral(
            "Use this login when you need to start Twitch polls or predictions "
            "as a channel moderator."),
        moderationAuthFrame);
    moderationAuthDescription->setWordWrap(true);
    moderationAuthLayout->addWidget(moderationAuthDescription);

    auto *moderationAuthButtons = new QHBoxLayout;
    moderationAuthButtons->setContentsMargins(0, 0, 0, 0);
    moderationAuthButtons->setSpacing(8);

    this->moderationAuthLoginButton_ =
        new QPushButton(QStringLiteral("Copy Helper"), moderationAuthFrame);
    this->moderationAuthCopyButton_ =
        new QPushButton(QStringLiteral("Paste Token"), moderationAuthFrame);
    this->moderationAuthClearButton_ =
        new QPushButton(QStringLiteral("Logout"), moderationAuthFrame);

    moderationAuthButtons->addWidget(this->moderationAuthLoginButton_);
    moderationAuthButtons->addWidget(this->moderationAuthCopyButton_);
    moderationAuthButtons->addWidget(this->moderationAuthClearButton_);
    moderationAuthButtons->addStretch(1);
    moderationAuthLayout->addLayout(moderationAuthButtons);

    this->moderationAuthCodeLabel_ =
        new QLabel(QStringLiteral("Helper: not copied"), moderationAuthFrame);
    this->moderationAuthCodeLabel_->setStyleSheet(QStringLiteral(
        "QLabel { font-family: monospace; font-weight: 700; }"));
    moderationAuthLayout->addWidget(this->moderationAuthCodeLabel_);

    this->moderationAuthStatusLabel_ = new QLabel(moderationAuthFrame);
    this->moderationAuthStatusLabel_->setWordWrap(true);
    moderationAuthLayout->addWidget(this->moderationAuthStatusLabel_);

    layout->addWidget(moderationAuthFrame);

    QObject::connect(this->moderationAuthLoginButton_, &QPushButton::clicked,
                     this, [this] {
                         this->copyModerationAuthHelper();
                     });
    QObject::connect(this->moderationAuthCopyButton_, &QPushButton::clicked,
                     this, [this] {
                         this->pasteModerationAuthToken();
                     });
    QObject::connect(this->moderationAuthClearButton_, &QPushButton::clicked,
                     this, [this] {
                         TwitchModerationAuth::clearSavedAccount();
                         ++this->moderationAuthGeneration_;
                         this->moderationAuthHelperCopied_ = false;
                         this->moderationAuthInFlight_ = false;
                         this->updateModerationAuthStatus();
                     });

    this->managedConnections_.managedConnect(
        TwitchModerationAuth::accountChanged(), [this] {
            this->updateModerationAuthStatus();
        });

    this->updateModerationAuthStatus();

    //    auto buttons = layout.emplace<QDialogButtonBox>();
    //    {
    //        this->addButton = buttons->addButton("Add",
    //        QDialogButtonBox::YesRole); this->removeButton =
    //        buttons->addButton("Remove", QDialogButtonBox::NoRole);
    //    }

    //    layout.emplace<AccountSwitchWidget>(this).assign(&this->accSwitchWidget);

    // ----
    //    QObject::connect(this->addButton, &QPushButton::clicked, []() {
    //        static auto loginWidget = new LoginWidget();
    //        loginWidget->show();
    //    });

    //    QObject::connect(this->removeButton, &QPushButton::clicked, [this] {
    //        auto selectedUser = this->accSwitchWidget->currentItem()->text();
    //        if (selectedUser == ANONYMOUS_USERNAME_LABEL) {
    //            // Do nothing
    //            return;
    //        }

    //        getApp()->getAccounts()->Twitch.removeUser(selectedUser);
    //    });
}

void AccountsPage::updateModerationAuthStatus()
{
    if (this->moderationAuthCodeLabel_ != nullptr)
    {
        this->moderationAuthCodeLabel_->setText(
            this->moderationAuthHelperCopied_
                ? QStringLiteral("Helper: copied to clipboard")
                : QStringLiteral("Helper: not copied"));
    }

    const auto account = TwitchModerationAuth::savedAccount();
    const bool hasAccount = account.isValid();
    const bool hasUsableAccount = account.supportsWebGql();
    if (!this->moderationAuthInFlight_ &&
        this->moderationAuthStatusLabel_ != nullptr)
    {
        if (hasUsableAccount)
        {
            this->moderationAuthStatusLabel_->setText(
                QStringLiteral("Logged in as %1.").arg(account.displayLabel()));
        }
        else if (hasAccount)
        {
            this->moderationAuthStatusLabel_->setText(QStringLiteral(
                "Saved activation login is not usable for Twitch moderator "
                "poll/prediction actions. Copy the helper and paste the token."));
        }
        else
        {
            this->moderationAuthStatusLabel_->setText(QStringLiteral(
                "Not logged in for moderator poll/prediction actions."));
        }
        this->moderationAuthStatusLabel_->setStyleSheet(
            QStringLiteral("QLabel { color: %1; }")
                .arg(hasUsableAccount
                         ? QStringLiteral("#47d16c")
                         : hasAccount ? QStringLiteral("#ff7b72")
                                      : QStringLiteral("#9aa0a6")));
    }

    if (this->moderationAuthLoginButton_ != nullptr)
    {
        this->moderationAuthLoginButton_->setVisible(!hasUsableAccount);
        this->moderationAuthLoginButton_->setEnabled(
            !hasUsableAccount && !this->moderationAuthInFlight_);
        this->moderationAuthLoginButton_->setText(
            hasUsableAccount ? QStringLiteral("Copy Helper Again")
                             : QStringLiteral("Copy Helper"));
    }
    if (this->moderationAuthCopyButton_ != nullptr)
    {
        this->moderationAuthCopyButton_->setVisible(!hasUsableAccount);
        this->moderationAuthCopyButton_->setEnabled(
            !hasUsableAccount && !this->moderationAuthInFlight_);
    }
    if (this->moderationAuthCodeLabel_ != nullptr)
    {
        this->moderationAuthCodeLabel_->setVisible(!hasUsableAccount);
    }
    if (this->moderationAuthClearButton_ != nullptr)
    {
        this->moderationAuthClearButton_->setEnabled(
            hasAccount || this->moderationAuthHelperCopied_ ||
            this->moderationAuthInFlight_);
    }
}

void AccountsPage::copyModerationAuthHelper()
{
    if (this->moderationAuthInFlight_)
    {
        return;
    }

    ++this->moderationAuthGeneration_;
    this->moderationAuthHelperCopied_ = true;
    TwitchModerationAuth::copyHelperToClipboard();
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.twitch.tv/")));
    this->finishModerationAuthLogin(
        QStringLiteral("Helper copied. On twitch.tv, press F12 to open "
                       "DevTools, switch to Console, paste it, then return "
                       "here and click Paste Token."),
        false);
}

void AccountsPage::pasteModerationAuthToken()
{
    if (this->moderationAuthInFlight_)
    {
        return;
    }

    const auto clipboardText = TwitchModerationAuth::clipboardText().trimmed();
    if (clipboardText.isEmpty() ||
        clipboardText.contains(QStringLiteral("localStorage")) ||
        clipboardText.contains(QStringLiteral("Mergerino token copied")))
    {
        this->finishModerationAuthLogin(
            QStringLiteral("Run the copied helper in the Twitch console first. "
                           "On twitch.tv, press F12 to open DevTools, switch "
                           "to Console, paste it, then click Paste Token."),
            true);
        return;
    }

    const auto payload = TwitchModerationAuth::parseClipboardPayload(clipboardText);
    if (payload.oauthToken.isEmpty())
    {
        this->finishModerationAuthLogin(
            QStringLiteral(
                "Clipboard text is not a Twitch token. Run the copied helper "
                "on twitch.tv, then click Paste Token."),
            true);
        return;
    }

    if (payload.oauthToken.size() > TwitchModerationAuth::maxTokenLength())
    {
        this->finishModerationAuthLogin(
            QStringLiteral(
                "Clipboard token is too long to be a Twitch token. Run the "
                "copied helper on twitch.tv and paste only the copied token."),
            true);
        return;
    }

    this->moderationAuthInFlight_ = true;
    const int generation = ++this->moderationAuthGeneration_;
    QPointer<AccountsPage> self(this);
    this->finishModerationAuthLogin(QStringLiteral("Validating Twitch token..."),
                                    false);
    TwitchModerationAuth::validateToken(
        payload.oauthToken,
        [self, generation, payload](TwitchModerationAuth::Account account) {
            if (self == nullptr)
            {
                return;
            }

            QTimer::singleShot(0, self.data(),
                               [self, generation, payload, account]() mutable {
                if (self == nullptr ||
                    generation != self->moderationAuthGeneration_)
                {
                    return;
                }

                self->moderationAuthInFlight_ = false;
                if (!account.supportsWebGql())
                {
                    self->finishModerationAuthLogin(
                        QStringLiteral(
                            "That token is not a Twitch browser token. Run the "
                            "copied helper on twitch.tv."),
                        true);
                    return;
                }

                account.clientIntegrity = payload.clientIntegrity;
                account.deviceId = payload.deviceId;
                TwitchModerationAuth::saveAccount(account);
                self->finishModerationAuthLogin(
                    QStringLiteral("Logged in as %1.")
                        .arg(account.displayLabel()),
                    false);
                self->updateModerationAuthStatus();
            });
        },
        [self, generation](const QString &error) {
            if (self == nullptr)
            {
                return;
            }

            QTimer::singleShot(0, self.data(), [self, generation, error] {
                if (self == nullptr ||
                    generation != self->moderationAuthGeneration_)
                {
                    return;
                }

                self->moderationAuthInFlight_ = false;
                self->finishModerationAuthLogin(error, true);
            });
        });
}

void AccountsPage::finishModerationAuthLogin(const QString &message,
                                             bool isError)
{
    if (this->moderationAuthCodeLabel_ != nullptr)
    {
        this->moderationAuthCodeLabel_->setText(
            this->moderationAuthHelperCopied_
                ? QStringLiteral("Helper: copied to clipboard")
                : QStringLiteral("Helper: not copied"));
    }

    if (this->moderationAuthStatusLabel_ != nullptr)
    {
        this->moderationAuthStatusLabel_->setText(message);
        this->moderationAuthStatusLabel_->setStyleSheet(
            QStringLiteral("QLabel { color: %1; }")
                .arg(isError ? QStringLiteral("#ff7b72")
                             : QStringLiteral("#9aa0a6")));
    }

    const auto account = TwitchModerationAuth::savedAccount();
    const bool hasAccount = account.isValid();
    const bool hasUsableAccount = account.supportsWebGql();
    if (this->moderationAuthLoginButton_ != nullptr)
    {
        this->moderationAuthLoginButton_->setVisible(!hasUsableAccount);
        this->moderationAuthLoginButton_->setEnabled(
            !hasUsableAccount && !this->moderationAuthInFlight_);
        this->moderationAuthLoginButton_->setText(
            hasUsableAccount ? QStringLiteral("Copy Helper Again")
                             : QStringLiteral("Copy Helper"));
    }
    if (this->moderationAuthCopyButton_ != nullptr)
    {
        this->moderationAuthCopyButton_->setVisible(!hasUsableAccount);
        this->moderationAuthCopyButton_->setEnabled(
            !hasUsableAccount && !this->moderationAuthInFlight_);
    }
    if (this->moderationAuthCodeLabel_ != nullptr)
    {
        this->moderationAuthCodeLabel_->setVisible(!hasUsableAccount);
    }
    if (this->moderationAuthClearButton_ != nullptr)
    {
        this->moderationAuthClearButton_->setEnabled(
            hasAccount || this->moderationAuthHelperCopied_ ||
            this->moderationAuthInFlight_);
    }
}

}  // namespace chatterino
