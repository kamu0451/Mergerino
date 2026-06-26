// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/seventv/SeventvBadges.hpp"

#include "Application.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"  // IWYU pragma: keep
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvAPI.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/PostToThread.hpp"

#include <unordered_set>

using namespace Qt::StringLiterals;

namespace {

}  // namespace

namespace chatterino {

QString SeventvBadges::idForBadge(const QJsonObject &badgeJson) const
{
    return badgeJson["id"].toString();
}

EmotePtr SeventvBadges::createBadge(const QString &id,
                                    const QJsonObject &badgeJson) const
{
    auto emote = Emote{
        // We utilize the "emote" "name" for filtering badges, and expect
        // the format to be "7tv:badge name" (e.g. "7tv:NNYS 2024")
        .name = EmoteName{u"7tv:" % badgeJson["name"].toString()},
        .images = SeventvEmotes::createImageSet(
            badgeJson, !getSettings()->animateSevenTVBadges),
        .tooltip = Tooltip{badgeJson["tooltip"].toString()},
        .homePage = Url{},
        .id = EmoteId{id},
    };

    if (emote.images.getImage1()->isEmpty())
    {
        return nullptr;  // Bad images
    }

    return std::make_shared<const Emote>(std::move(emote));
}

void SeventvBadges::onMissingBadgeReferenced(const QString &badgeID)
{
    if (badgeID.isEmpty())
    {
        return;
    }

    {
        std::unique_lock lock(this->requestedBadgeMutex_);
        if (!this->requestedBadgeIDs_.emplace(badgeID).second)
        {
            return;
        }
    }

    auto *api = getApp()->getSeventvAPI();
    if (!api)
    {
        std::unique_lock lock(this->requestedBadgeMutex_);
        this->requestedBadgeIDs_.erase(badgeID);
        return;
    }

    api->getCosmeticsByIDs(
        {badgeID},
        [this, badgeID](const QJsonObject &cosmetics) {
            const auto badges = cosmetics["badges"].toArray();
            for (const auto &badgeValue : badges)
            {
                auto badge = badgeValue.toObject();
                if (!badge.isEmpty())
                {
                    getApp()->getSeventvBadges()->registerBadge(badge);
                }
            }

            std::unique_lock lock(this->requestedBadgeMutex_);
            this->requestedBadgeIDs_.erase(badgeID);
        },
        [this, badgeID](const auto &) {
            std::unique_lock lock(this->requestedBadgeMutex_);
            this->requestedBadgeIDs_.erase(badgeID);
        });
}

void SeventvBadges::onBadgeUsersChanged(
    std::span<const QString> twitchUserIDs, std::span<const uint64_t> kickUserIDs)
{
    if (twitchUserIDs.empty() && kickUserIDs.empty())
    {
        return;
    }

    postToThread([] {
        auto *app = tryGetApp();
        if (app == nullptr || app->isTest())
        {
            return;
        }
        if (auto *windows = app->getWindows())
        {
            windows->invalidateChannelViewBuffers();
        }
    });
}

}  // namespace chatterino
