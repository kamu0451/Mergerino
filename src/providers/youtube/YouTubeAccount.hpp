#pragma once

#include "controllers/accounts/Account.hpp"

#include <pajlada/signals/signal.hpp>
#include <QDateTime>
#include <QString>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace chatterino {

struct YouTubeAccountData {
    QString displayName;
    QString channelID;
    QString clientID;
    QString authToken;
    QString refreshToken;
    QDateTime expiresAt;

    void save() const;
    static std::optional<YouTubeAccountData> loadRaw(const std::string &key);
};

class YouTubeAccount : public Account,
                       public std::enable_shared_from_this<YouTubeAccount>
{
public:
    explicit YouTubeAccount(const YouTubeAccountData &args);
    ~YouTubeAccount() override;

    constexpr static std::chrono::minutes CHECK_REFRESH_INTERVAL{2};

    Q_DISABLE_COPY_MOVE(YouTubeAccount);

    void save() const;
    bool update(const YouTubeAccountData &data);

    QString toString() const override;

    bool isAnonymous() const
    {
        return this->channelID_.isEmpty();
    }

    QString displayName() const
    {
        return this->displayName_;
    }

    QString channelID() const
    {
        return this->channelID_;
    }

    QString clientID() const
    {
        return this->clientID_;
    }

    QString authToken() const
    {
        return this->authToken_;
    }

    QString refreshToken() const
    {
        return this->refreshToken_;
    }

    void refreshIfNeeded();
    void ensureFreshToken(std::function<void(bool)> cb);

    pajlada::Signals::NoArgSignal authUpdated;

private:
    void doRefresh();
    void finishRefresh(bool success);

    QString displayName_;
    QString channelID_;
    QString clientID_;
    QString authToken_;
    QString refreshToken_;
    QDateTime expiresAt_;

    bool refreshing_{false};
    std::vector<std::function<void(bool)>> refreshCallbacks_;
};

}  // namespace chatterino
