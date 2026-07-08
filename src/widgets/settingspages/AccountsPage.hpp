// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/settingspages/SettingsPage.hpp"

#include <QLabel>
#include <QPushButton>

namespace chatterino {

class AccountSwitchWidget;

class AccountsPage : public SettingsPage
{
public:
    AccountsPage();

private:
    void updateModerationAuthStatus();
    void copyModerationAuthHelper();
    void pasteModerationAuthToken();
    void finishModerationAuthLogin(const QString &message, bool isError);

    QLabel *moderationAuthCodeLabel_ = nullptr;
    QLabel *moderationAuthStatusLabel_ = nullptr;
    QPushButton *moderationAuthLoginButton_ = nullptr;
    QPushButton *moderationAuthCopyButton_ = nullptr;
    QPushButton *moderationAuthClearButton_ = nullptr;
    int moderationAuthGeneration_ = 0;
    bool moderationAuthHelperCopied_ = false;
    bool moderationAuthInFlight_ = false;
};

}  // namespace chatterino
