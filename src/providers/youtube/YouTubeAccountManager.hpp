// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/SignalVector.hpp"

#include <pajlada/settings/setting.hpp>
#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QTimer>

#include <memory>
#include <vector>

namespace chatterino {

class YouTubeAccount;
struct YouTubeAccountData;

class YouTubeAccountManager
{
public:
    YouTubeAccountManager();

    std::shared_ptr<YouTubeAccount> current();

    std::vector<QString> channelNames() const;

    std::shared_ptr<YouTubeAccount> findUserByChannelID(
        const QString &channelID) const;
    bool userExists(const QString &channelID) const;

    void reloadUsers();
    void load();

    bool isLoggedIn() const;

    pajlada::Settings::Setting<QString> currentChannelID{
        "/youtubeAccounts/current", ""};

    pajlada::Signals::NoArgSignal currentUserChanged;
    pajlada::Signals::NoArgSignal userListUpdated;

    SignalVector<std::shared_ptr<YouTubeAccount>> accounts;

private:
    enum class AddUserResponse : uint8_t {
        UserAlreadyExists,
        UserUpdated,
        UserAdded,
    };
    AddUserResponse addAccount(const YouTubeAccountData &data);
    bool removeAccount(YouTubeAccount *account);

    void refreshAccounts() const;

    std::shared_ptr<YouTubeAccount> currentUser_;
    std::shared_ptr<YouTubeAccount> anonymousUser_;
    QTimer refreshTimer_;
    pajlada::Signals::SignalHolder holder_;
};

}  // namespace chatterino
