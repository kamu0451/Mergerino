// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/BadgeRegistry.hpp"

#include "messages/Emote.hpp"
#include "providers/seventv/eventapi/Dispatch.hpp"
#include "util/Variant.hpp"

#include <array>
#include <QJsonArray>
#include <QUrl>
#include <QUrlQuery>
#include <utility>
#include <vector>

namespace {

void clearIfEquals(auto &map, auto &&key, const auto &expectedID)
{
    const auto it = map.find(std::forward<decltype(key)>(key));
    if (it != map.end() && it->second->id.string == expectedID)
    {
        map.erase(it);
    }
}

void clearPendingIfEquals(auto &map, auto &&key, const auto &expectedID)
{
    const auto it = map.find(std::forward<decltype(key)>(key));
    if (it != map.end() && it->second == expectedID)
    {
        map.erase(it);
    }
}

bool setIfDifferent(auto &map, auto &&key, const auto &value)
{
    auto it = map.find(key);
    if (it == map.end())
    {
        map.emplace(std::forward<decltype(key)>(key), value);
        return true;
    }

    if (it->second != value)
    {
        it->second = value;
        return true;
    }

    return false;
}

}  // namespace

namespace chatterino {

std::optional<EmotePtr> BadgeRegistry::getBadge(const UserId &id) const
{
    std::shared_lock lock(this->mutex_);

    auto it = this->badgeMap_.find(id.string);
    if (it != this->badgeMap_.end())
    {
        return it->second;
    }
    return std::nullopt;
}

std::optional<EmotePtr> BadgeRegistry::getKickBadge(uint64_t id) const
{
    std::shared_lock lock(this->mutex_);

    auto it = this->kickBadgeMap_.find(id);
    if (it != this->kickBadgeMap_.end())
    {
        return it->second;
    }
    return std::nullopt;
}

void BadgeRegistry::assignBadgeToUser(const QString &badgeID,
                                      const UserId &userID)
{
    bool changed = false;
    bool missingBadge = false;
    std::array<QString, 1> changedTwitchUserIDs{};

    {
        const std::unique_lock lock(this->mutex_);

        const auto badgeIt = this->knownBadges_.find(badgeID);
        if (badgeIt != this->knownBadges_.end())
        {
            changed =
                setIfDifferent(this->badgeMap_, userID.string, badgeIt->second);
            this->pendingBadgeAssignments_.erase(userID.string);
        }
        else
        {
            this->pendingBadgeAssignments_[userID.string] = badgeID;
            missingBadge = true;
        }
    }

    if (missingBadge)
    {
        this->onMissingBadgeReferenced(badgeID);
    }

    if (changed)
    {
        changedTwitchUserIDs[0] = userID.string;
        this->onBadgeUsersChanged(changedTwitchUserIDs,
                                  std::span<const uint64_t>{});
    }
}

void BadgeRegistry::assignBadgeToUsers(
    const QString &badgeID, std::span<const seventv::eventapi::User> users)
{
    std::vector<QString> changedTwitchUserIDs;
    std::vector<uint64_t> changedKickUserIDs;
    std::vector<QString> missingBadgeIDs;

    {
        const std::unique_lock lock(this->mutex_);

        const auto badgeIt = this->knownBadges_.find(badgeID);
        const bool hasKnownBadge = badgeIt != this->knownBadges_.end();
        for (const auto &user : users)
        {
            std::visit(variant::Overloaded{
                           [&](const seventv::eventapi::TwitchUser &u) {
                               if (hasKnownBadge)
                               {
                                   if (setIfDifferent(this->badgeMap_, u.id,
                                                      badgeIt->second))
                                   {
                                       changedTwitchUserIDs.push_back(u.id);
                                   }
                                   this->pendingBadgeAssignments_.erase(u.id);
                               }
                               else
                               {
                                   this->pendingBadgeAssignments_[u.id] =
                                       badgeID;
                                   missingBadgeIDs.push_back(badgeID);
                               }
                           },
                           [&](const seventv::eventapi::KickUser &u) {
                               if (hasKnownBadge)
                               {
                                   if (setIfDifferent(this->kickBadgeMap_,
                                                      u.id, badgeIt->second))
                                   {
                                       changedKickUserIDs.push_back(u.id);
                                   }
                                   this->pendingKickBadgeAssignments_.erase(
                                       u.id);
                               }
                               else
                               {
                                   this->pendingKickBadgeAssignments_[u.id] =
                                       badgeID;
                                   missingBadgeIDs.push_back(badgeID);
                               }
                           },
                       },
                       user);
        }
    }

    for (const auto &missingBadgeID : missingBadgeIDs)
    {
        this->onMissingBadgeReferenced(missingBadgeID);
    }

    if (!changedTwitchUserIDs.empty() || !changedKickUserIDs.empty())
    {
        this->onBadgeUsersChanged(changedTwitchUserIDs, changedKickUserIDs);
    }
}

void BadgeRegistry::clearBadgeFromUser(const QString &badgeID,
                                       const UserId &userID)
{
    bool changed = false;
    std::array<QString, 1> changedTwitchUserIDs{};

    {
        const std::unique_lock lock(this->mutex_);

        const auto it = this->badgeMap_.find(userID.string);
        if (it != this->badgeMap_.end() && it->second->id.string == badgeID)
        {
            this->badgeMap_.erase(it);
            changed = true;
        }

        clearPendingIfEquals(this->pendingBadgeAssignments_, userID.string,
                             badgeID);
    }

    if (changed)
    {
        changedTwitchUserIDs[0] = userID.string;
        this->onBadgeUsersChanged(changedTwitchUserIDs,
                                  std::span<const uint64_t>{});
    }
}

void BadgeRegistry::clearBadgeFromUsers(
    const QString &badgeID, std::span<const seventv::eventapi::User> users)
{
    std::vector<QString> changedTwitchUserIDs;
    std::vector<uint64_t> changedKickUserIDs;

    {
        const std::unique_lock lock(this->mutex_);

        for (const auto &user : users)
        {
            std::visit(variant::Overloaded{
                           [&](const seventv::eventapi::TwitchUser &u) {
                               const auto it = this->badgeMap_.find(u.id);
                               if (it != this->badgeMap_.end() &&
                                   it->second->id.string == badgeID)
                               {
                                   this->badgeMap_.erase(it);
                                   changedTwitchUserIDs.push_back(u.id);
                               }
                               clearPendingIfEquals(
                                   this->pendingBadgeAssignments_, u.id,
                                   badgeID);
                           },
                           [&](const seventv::eventapi::KickUser &u) {
                               const auto it =
                                   this->kickBadgeMap_.find(u.id);
                               if (it != this->kickBadgeMap_.end() &&
                                   it->second->id.string == badgeID)
                               {
                                   this->kickBadgeMap_.erase(it);
                                   changedKickUserIDs.push_back(u.id);
                               }
                               clearPendingIfEquals(
                                   this->pendingKickBadgeAssignments_, u.id,
                                   badgeID);
                           },
                       },
                       user);
        }
    }

    if (!changedTwitchUserIDs.empty() || !changedKickUserIDs.empty())
    {
        this->onBadgeUsersChanged(changedTwitchUserIDs, changedKickUserIDs);
    }
}

QString BadgeRegistry::registerBadge(const QJsonObject &badgeJson)
{
    const auto badgeID = this->idForBadge(badgeJson);

    std::vector<QString> changedTwitchUserIDs;
    std::vector<uint64_t> changedKickUserIDs;

    {
        const std::unique_lock lock(this->mutex_);

        auto emote = this->createBadge(badgeID, badgeJson);
        if (!emote)
        {
            return badgeID;
        }

        this->knownBadges_[badgeID] = std::move(emote);
        const auto &badge = this->knownBadges_.at(badgeID);

        for (auto it = this->pendingBadgeAssignments_.begin();
             it != this->pendingBadgeAssignments_.end();)
        {
            if (it->second != badgeID)
            {
                ++it;
                continue;
            }

            if (setIfDifferent(this->badgeMap_, it->first, badge))
            {
                changedTwitchUserIDs.push_back(it->first);
            }
            it = this->pendingBadgeAssignments_.erase(it);
        }

        for (auto it = this->pendingKickBadgeAssignments_.begin();
             it != this->pendingKickBadgeAssignments_.end();)
        {
            if (it->second != badgeID)
            {
                ++it;
                continue;
            }

            if (setIfDifferent(this->kickBadgeMap_, it->first, badge))
            {
                changedKickUserIDs.push_back(it->first);
            }
            it = this->pendingKickBadgeAssignments_.erase(it);
        }

        for (auto &[userID, assignedBadge] : this->badgeMap_)
        {
            if (assignedBadge->id.string == badgeID && assignedBadge != badge)
            {
                assignedBadge = badge;
                changedTwitchUserIDs.push_back(userID);
            }
        }

        for (auto &[userID, assignedBadge] : this->kickBadgeMap_)
        {
            if (assignedBadge->id.string == badgeID && assignedBadge != badge)
            {
                assignedBadge = badge;
                changedKickUserIDs.push_back(userID);
            }
        }
    }

    if (!changedTwitchUserIDs.empty() || !changedKickUserIDs.empty())
    {
        this->onBadgeUsersChanged(changedTwitchUserIDs, changedKickUserIDs);
    }

    return badgeID;
}

void BadgeRegistry::removeBadge(const QString &badgeID)
{
    std::vector<QString> changedTwitchUserIDs;
    std::vector<uint64_t> changedKickUserIDs;

    {
        const std::unique_lock lock(this->mutex_);

        this->knownBadges_.erase(badgeID);

        for (auto it = this->badgeMap_.begin(); it != this->badgeMap_.end();)
        {
            if (it->second->id.string == badgeID)
            {
                changedTwitchUserIDs.push_back(it->first);
                it = this->badgeMap_.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = this->kickBadgeMap_.begin();
             it != this->kickBadgeMap_.end();)
        {
            if (it->second->id.string == badgeID)
            {
                changedKickUserIDs.push_back(it->first);
                it = this->kickBadgeMap_.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = this->pendingBadgeAssignments_.begin();
             it != this->pendingBadgeAssignments_.end();)
        {
            if (it->second == badgeID)
            {
                it = this->pendingBadgeAssignments_.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = this->pendingKickBadgeAssignments_.begin();
             it != this->pendingKickBadgeAssignments_.end();)
        {
            if (it->second == badgeID)
            {
                it = this->pendingKickBadgeAssignments_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    if (!changedTwitchUserIDs.empty() || !changedKickUserIDs.empty())
    {
        this->onBadgeUsersChanged(changedTwitchUserIDs, changedKickUserIDs);
    }
}

void BadgeRegistry::onMissingBadgeReferenced(const QString &)
{
}

void BadgeRegistry::onBadgeUsersChanged(
    std::span<const QString> /*twitchUserIDs*/,
    std::span<const uint64_t> /*kickUserIDs*/)
{
}

}  // namespace chatterino
