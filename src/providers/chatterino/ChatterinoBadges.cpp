// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/chatterino/ChatterinoBadges.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QUrl>

namespace chatterino {

ChatterinoBadges::ChatterinoBadges()
{
    this->loadChatterinoBadges();
}

std::optional<EmotePtr> ChatterinoBadges::getBadge(const UserId &id)
{
    std::shared_lock lock(this->mutex_);

    auto it = this->badgeMap.find(id.string);
    if (it != this->badgeMap.end())
    {
        return this->emotes[it->second];
    }
    return std::nullopt;
}

void ChatterinoBadges::loadChatterinoBadges()
{
    std::unique_lock lock(this->mutex_);
    this->badgeMap.clear();
    this->emotes.clear();
}

}  // namespace chatterino
