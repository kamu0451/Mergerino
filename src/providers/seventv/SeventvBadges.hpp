// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/BadgeRegistry.hpp"

#include <QJsonObject>

#include <memory>
#include <shared_mutex>
#include <unordered_set>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

class SeventvBadges : public BadgeRegistry
{
public:
    SeventvBadges() = default;

protected:
    QString idForBadge(const QJsonObject &badgeJson) const override;
    EmotePtr createBadge(const QString &id,
                         const QJsonObject &badgeJson) const override;
    void onMissingBadgeReferenced(const QString &badgeID) override;
    void onBadgeUsersChanged(std::span<const QString> twitchUserIDs,
                             std::span<const uint64_t> kickUserIDs) override;

private:
    mutable std::shared_mutex requestedBadgeMutex_;
    std::unordered_set<QString> requestedBadgeIDs_;
};

}  // namespace chatterino
