// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pajlada/signals/signal.hpp>
#include <QString>

#include <functional>

namespace chatterino {

class TwitchModerationAuth
{
public:
    struct Account {
        QString clientId;
        QString oauthToken;
        QString clientIntegrity;
        QString deviceId;
        QString userId;
        QString login;
        QString displayName;

        bool isValid() const;
        bool supportsWebGql() const;
        bool supportsWebIntegrity() const;
        QString displayLabel() const;
    };

    struct ClipboardPayload {
        QString oauthToken;
        QString clientIntegrity;
        QString deviceId;
    };

    struct DeviceCode {
        QString deviceCode;
        QString userCode;
        QString verificationUri;
        int intervalMs = 5000;

        bool isValid() const;
    };

    enum class DeviceTokenStatus {
        Authorized,
        Pending,
        SlowDown,
        Failed,
    };

    struct DeviceTokenResult {
        DeviceTokenStatus status = DeviceTokenStatus::Failed;
        QString oauthToken;
        QString error;
    };

    static QString normalizeToken(QString token);
    static ClipboardPayload parseClipboardPayload(const QString &text);
    static QString helperClipboardScript();
    static QString clipboardText();
    static void copyHelperToClipboard();
    static int maxTokenLength();
    static Account savedAccount();
    static void saveAccount(const Account &account);
    static void clearSavedAccount();
    static pajlada::Signals::NoArgSignal &accountChanged();
    static Account resolveForCurrentUser(const QString &currentUserId,
                                         QString *errorMessage = nullptr);

    static void requestDeviceCode(
        std::function<void(DeviceCode)> successCallback,
        std::function<void(const QString &)> failureCallback);

    static void pollDeviceToken(
        const QString &deviceCode,
        std::function<void(DeviceTokenResult)> callback);

    static void validateToken(
        const QString &oauthToken,
        std::function<void(Account)> successCallback,
        std::function<void(const QString &)> failureCallback);
};

}  // namespace chatterino
