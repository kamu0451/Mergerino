// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"
#include "common/Atomic.hpp"
#include "util/ExponentialBackoff.hpp"
#include "util/QStringHash.hpp"

#include <boost/unordered/unordered_flat_map.hpp>
#include <pajlada/signals/scoped-connection.hpp>
#include <QJsonObject>
#include <QString>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;
class EmoteMap;
class Channel;
class NetworkResult;

/// Maps a Twitch User ID to a list of badge IDs
using FfzChannelBadgeMap =
    boost::unordered::unordered_flat_map<QString, std::vector<int>>;

namespace ffz::detail {

EmoteMap parseChannelEmotes(const QJsonObject &jsonRoot);

/// Parse the `user_badge_ids` into a map of User IDs -> Badge IDs
FfzChannelBadgeMap parseChannelBadges(const QJsonObject &badgeRoot);

/// Decides whether a failed global/channel FFZ emote fetch is worth
/// retrying: true for transport-level failures (no HTTP response at all,
/// e.g. a timeout) and 5xx server errors, false for a definitive 4xx
/// client response (which won't change on a retry).
bool isRetryableFetchError(const NetworkResult &result);

}  // namespace ffz::detail

class FfzEmotes final
{
public:
    FfzEmotes();

    std::shared_ptr<const EmoteMap> emotes() const;
    std::optional<EmotePtr> emote(const EmoteName &name) const;
    void loadEmotes();
    void setEmotes(std::shared_ptr<const EmoteMap> emotes);
    static void loadChannel(
        std::weak_ptr<Channel> channel, const QString &channelId,
        std::function<void(EmoteMap &&)> emoteCallback,
        std::function<void(std::optional<EmotePtr>)> modBadgeCallback,
        std::function<void(std::optional<EmotePtr>)> vipBadgeCallback,
        std::function<void(FfzChannelBadgeMap &&)> channelBadgesCallback,
        bool manualRefresh, bool cacheHit);

private:
    /// Fetches the global FFZ emote list, retrying a bounded number of
    /// times (with a growing delay) on transient network failures.
    void fetchGlobalEmotes(ExponentialBackoff<3> backoff, int attempt);

    /// Fetches a channel's FFZ emotes/badges, retrying a bounded number of
    /// times (with a growing delay) on transient network failures.
    static void fetchChannelEmotes(
        std::weak_ptr<Channel> channel, const QString &channelId,
        std::function<void(EmoteMap &&)> emoteCallback,
        std::function<void(std::optional<EmotePtr>)> modBadgeCallback,
        std::function<void(std::optional<EmotePtr>)> vipBadgeCallback,
        std::function<void(FfzChannelBadgeMap &&)> channelBadgesCallback,
        bool manualRefresh, bool cacheHit, ExponentialBackoff<3> backoff,
        int attempt);

    Atomic<std::shared_ptr<const EmoteMap>> global_;

    std::vector<std::unique_ptr<pajlada::Signals::ScopedConnection>>
        managedConnections;
};

}  // namespace chatterino
