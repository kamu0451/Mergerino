// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/seventv/SeventvBadges.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"  // IWYU pragma: keep
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "providers/seventv/SeventvAPI.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Settings.hpp"
#include "util/PostToThread.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Qt::StringLiterals;

namespace {

using namespace chatterino;

const auto SEVENTV_BADGE_PREFIX = u"7tv:"_s;

bool hasUsernameElement(const Message &message)
{
    return std::ranges::any_of(message.elements, [](const auto &element) {
        return element->getFlags().has(MessageElementFlag::Username);
    });
}

void refreshBadgeOnMessage(Channel &channel, const MessagePtr &message,
                           const std::optional<EmotePtr> &badge)
{
    if (!message || !hasUsernameElement(*message))
    {
        return;
    }

    auto cloned = message->clone();
    bool changed = false;

    auto &elements = cloned->elements;
    const auto oldBadgeEnd = std::remove_if(
        elements.begin(), elements.end(), [](const auto &element) {
            return element->getFlags().has(MessageElementFlag::BadgeSevenTV);
        });
    if (oldBadgeEnd != elements.end())
    {
        elements.erase(oldBadgeEnd, elements.end());
        changed = true;
    }

    auto &externalBadges = cloned->externalBadges;
    const auto oldExternalEnd =
        std::remove_if(externalBadges.begin(), externalBadges.end(),
                       [](const QString &badgeName) {
                           return badgeName.startsWith(SEVENTV_BADGE_PREFIX);
                       });
    if (oldExternalEnd != externalBadges.end())
    {
        externalBadges.erase(oldExternalEnd, externalBadges.end());
        changed = true;
    }

    if (badge)
    {
        const auto usernameIt = std::find_if(
            elements.begin(), elements.end(), [](const auto &element) {
                return element->getFlags().has(MessageElementFlag::Username);
            });
        elements.insert(
            usernameIt,
            std::make_unique<BadgeElement>(*badge,
                                           MessageElementFlag::BadgeSevenTV));
        externalBadges.push_back((*badge)->name.string);
        changed = true;
    }

    if (!changed)
    {
        return;
    }

    channel.replaceMessage(message, cloned);
}

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

    const std::vector<QString> twitchIDs(twitchUserIDs.begin(),
                                         twitchUserIDs.end());
    const std::vector<uint64_t> kickIDs(kickUserIDs.begin(), kickUserIDs.end());

    postToThread([twitchIDs, kickIDs] {
        auto *app = getApp();
        std::unordered_map<QString, std::optional<EmotePtr>> twitchBadges;
        twitchBadges.reserve(twitchIDs.size());
        for (const auto &userID : twitchIDs)
        {
            twitchBadges.emplace(userID,
                                 app->getSeventvBadges()->getBadge({userID}));
        }

        std::unordered_map<uint64_t, std::optional<EmotePtr>> kickBadges;
        kickBadges.reserve(kickIDs.size());
        for (const auto userID : kickIDs)
        {
            kickBadges.emplace(userID,
                               app->getSeventvBadges()->getKickBadge(userID));
        }

        if (!twitchBadges.empty())
        {
            app->getTwitch()->forEachChannelAndSpecialChannels(
                [&](const ChannelPtr &channel) {
                    for (const auto &message : channel->getMessageSnapshot())
                    {
                        if (message->userID.isEmpty())
                        {
                            continue;
                        }

                        const auto it = twitchBadges.find(message->userID);
                        if (it == twitchBadges.end())
                        {
                            continue;
                        }

                        refreshBadgeOnMessage(*channel, message, it->second);
                    }
                });
        }

        if (!kickBadges.empty())
        {
            app->getKickChatServer()->forEachChannel([&](KickChannel &channel) {
                for (const auto &message : channel.getMessageSnapshot())
                {
                    bool ok = false;
                    const auto userID = message->userID.toULongLong(&ok);
                    if (!ok)
                    {
                        continue;
                    }

                    const auto it = kickBadges.find(userID);
                    if (it == kickBadges.end())
                    {
                        continue;
                    }

                    refreshBadgeOnMessage(channel, message, it->second);
                }
            });
        }
    });
}

}  // namespace chatterino
