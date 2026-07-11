// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeAccount.hpp"

#include "Application.hpp"
#include "common/ChatterinoSetting.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "singletons/Settings.hpp"
#include "providers/youtube/YouTubeCommon.hpp"

#include <pajlada/settings/setting.hpp>
#include <QJsonObject>
#include <QStringBuilder>
#include <QUrlQuery>

#include <algorithm>
#include <limits>
#include <utility>

namespace chatterino {

using namespace Qt::Literals;

namespace {

std::string accountPath(const QString &channelID)
{
    return "/youtubeAccounts/channel" + channelID.toStdString();
}

QString formatGoogleError(const NetworkResult &result)
{
    const auto json = result.parseJson();
    QString error;

    const auto errorValue = json["error"_L1];
    if (errorValue.isObject())
    {
        error = errorValue.toObject()["message"_L1].toString();
    }
    else
    {
        error = json["error_description"_L1].toString(errorValue.toString());
    }

    if (!error.isEmpty())
    {
        return u"Error: "_s % error % u" ("_s % result.formatError() % u")"_s;
    }
    return u"Error: "_s % result.formatError() %
           u" (no further information)"_s;
}

void addOAuthClientSecret(QUrlQuery &payload, const QString &clientID)
{
    const auto clientSecret = youTubeOAuthClientSecret();
    if (clientID == youTubeOAuthClientID() && !clientSecret.isEmpty())
    {
        payload.addQueryItem(u"client_secret"_s, clientSecret);
    }
}

}  // namespace

std::optional<YouTubeAccountData> YouTubeAccountData::loadRaw(
    const std::string &key)
{
    auto displayName =
        QStringSetting::get("/youtubeAccounts/" + key + "/displayName");
    auto channelID =
        QStringSetting::get("/youtubeAccounts/" + key + "/channelID");
    auto clientID =
        QStringSetting::get("/youtubeAccounts/" + key + "/clientID");
    auto authToken =
        QStringSetting::get("/youtubeAccounts/" + key + "/authToken");
    auto refreshToken =
        QStringSetting::get("/youtubeAccounts/" + key + "/refreshToken");
    auto expiresAtStr =
        QStringSetting::get("/youtubeAccounts/" + key + "/expiresAt");

    if (displayName.isEmpty() || channelID.isEmpty() || clientID.isEmpty() ||
        authToken.isEmpty() || refreshToken.isEmpty() || expiresAtStr.isEmpty())
    {
        return std::nullopt;
    }

    QDateTime expiresAt = QDateTime::fromString(expiresAtStr, Qt::ISODate);
    if (!expiresAt.isValid())
    {
        return std::nullopt;
    }

    return YouTubeAccountData{
        .displayName = displayName.trimmed(),
        .channelID = channelID.trimmed(),
        .clientID = clientID.trimmed(),
        .authToken = authToken.trimmed(),
        .refreshToken = refreshToken.trimmed(),
        .expiresAt = expiresAt,
    };
}

void YouTubeAccountData::save() const
{
    const auto basePath = accountPath(this->channelID);
    QStringSetting::set(basePath + "/displayName", this->displayName);
    QStringSetting::set(basePath + "/channelID", this->channelID);
    QStringSetting::set(basePath + "/clientID", this->clientID);
    QStringSetting::set(basePath + "/authToken", this->authToken);
    QStringSetting::set(basePath + "/refreshToken", this->refreshToken);
    QStringSetting::set(basePath + "/expiresAt",
                        this->expiresAt.toString(Qt::ISODate));
    std::ignore = getSettings()->requestSave();
}

YouTubeAccount::YouTubeAccount(const YouTubeAccountData &args)
    : Account(ProviderId::YouTube)
    , displayName_(args.displayName)
    , channelID_(args.channelID)
    , clientID_(args.clientID)
    , authToken_(args.authToken)
    , refreshToken_(args.refreshToken)
    , expiresAt_(args.expiresAt)
{
}

YouTubeAccount::~YouTubeAccount() = default;

void YouTubeAccount::save() const
{
    YouTubeAccountData{
        .displayName = this->displayName_,
        .channelID = this->channelID_,
        .clientID = this->clientID_,
        .authToken = this->authToken_,
        .refreshToken = this->refreshToken_,
        .expiresAt = this->expiresAt_,
    }
        .save();
}

bool YouTubeAccount::update(const YouTubeAccountData &data)
{
    bool changed = false;

    if (this->displayName_ != data.displayName)
    {
        changed = true;
        this->displayName_ = data.displayName;
    }
    if (this->channelID_ != data.channelID)
    {
        changed = true;
        this->channelID_ = data.channelID;
    }
    if (this->clientID_ != data.clientID)
    {
        changed = true;
        this->clientID_ = data.clientID;
    }
    if (this->authToken_ != data.authToken)
    {
        changed = true;
        this->authToken_ = data.authToken;
    }
    if (this->refreshToken_ != data.refreshToken)
    {
        changed = true;
        this->refreshToken_ = data.refreshToken;
    }
    if (this->expiresAt_ != data.expiresAt)
    {
        changed = true;
        this->expiresAt_ = data.expiresAt;
    }

    if (changed)
    {
        this->save();
    }
    return changed;
}

QString YouTubeAccount::toString() const
{
    return this->displayName_.isEmpty() ? this->channelID_
                                        : this->displayName_;
}

void YouTubeAccount::refreshIfNeeded()
{
    if (this->isAnonymous())
    {
        return;
    }

    auto now = QDateTime::currentDateTimeUtc() + CHECK_REFRESH_INTERVAL +
               std::chrono::seconds{60};
    if (now < this->expiresAt_)
    {
        return;
    }

    this->ensureFreshToken({});
}

void YouTubeAccount::ensureFreshToken(std::function<void(bool)> cb)
{
    if (this->isAnonymous() || this->refreshToken_.isEmpty())
    {
        if (cb)
        {
            cb(false);
        }
        return;
    }

    const auto refreshAt = QDateTime::currentDateTimeUtc().addSecs(60);
    if (!this->authToken_.isEmpty() && refreshAt < this->expiresAt_)
    {
        if (cb)
        {
            cb(true);
        }
        return;
    }

    if (cb)
    {
        this->refreshCallbacks_.push_back(std::move(cb));
    }

    if (this->refreshing_)
    {
        return;
    }

    this->doRefresh();
}

void YouTubeAccount::doRefresh()
{
    this->refreshing_ = true;

    QUrlQuery payload{
        {"client_id"_L1, this->clientID_},
        {"refresh_token"_L1, this->refreshToken_},
        {"grant_type"_L1, "refresh_token"_L1},
    };
    addOAuthClientSecret(payload, this->clientID_);

    auto weak = this->weak_from_this();
    NetworkRequest("https://oauth2.googleapis.com/token",
                   NetworkRequestType::Post)
        .header("Content-Type", "application/x-www-form-urlencoded")
        .hideRequestBody()
        .payload(payload.toString(QUrl::FullyEncoded).toUtf8())
        .timeout(20'000)
        .onSuccess([weak](const NetworkResult &res) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }

            const auto json = res.parseJson();
            const auto authToken = json["access_token"_L1].toString();
            if (authToken.isEmpty())
            {
                qCWarning(chatterinoYouTube)
                    << "YouTube refresh returned no access token"
                    << formatGoogleError(res);
                self->finishRefresh(false);
                return;
            }

            self->authToken_ = authToken;
            const auto nextRefreshToken =
                json["refresh_token"_L1].toString().trimmed();
            if (!nextRefreshToken.isEmpty())
            {
                self->refreshToken_ = nextRefreshToken;
            }

            auto expiresInSec =
                std::clamp<qint64>(json["expires_in"_L1].toInteger(), 0,
                                   std::numeric_limits<qint32>::max());
            if (expiresInSec <= 0)
            {
                expiresInSec = 3600;
            }
            self->expiresAt_ =
                QDateTime::currentDateTimeUtc().addSecs(expiresInSec);
            self->save();
            self->authUpdated.invoke();
            qCDebug(chatterinoYouTube)
                << "[Refresh] Successful, next expiry:" << self->expiresAt_;
            self->finishRefresh(true);
        })
        .onError([weak](const NetworkResult &res) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }

            qCWarning(chatterinoYouTube)
                << "Failed to refresh YouTube account" << self->displayName()
                << "error:" << formatGoogleError(res);
            self->finishRefresh(false);
        })
        .execute();
}

void YouTubeAccount::finishRefresh(bool success)
{
    this->refreshing_ = false;
    auto callbacks = std::exchange(this->refreshCallbacks_, {});
    for (const auto &cb : callbacks)
    {
        if (cb)
        {
            cb(success);
        }
    }
}

}  // namespace chatterino
