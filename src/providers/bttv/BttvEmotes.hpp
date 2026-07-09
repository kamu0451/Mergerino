// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"
#include "common/Atomic.hpp"
#include "util/ExponentialBackoff.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QJsonObject>
#include <QString>

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;
class EmoteMap;
class Channel;
class NetworkResult;
struct BttvLiveUpdateEmoteUpdateAddMessage;
struct BttvLiveUpdateEmoteRemoveMessage;

namespace bttv::detail {

EmoteMap parseChannelEmotes(const QJsonObject &jsonRoot,
                            const QString &channelDisplayName);

/// Decides whether a failed global/channel BTTV emote fetch is worth
/// retrying: true for transport-level failures (no HTTP response at all,
/// e.g. a timeout) and 5xx server errors, false for a definitive 4xx
/// client response (which won't change on a retry).
bool isRetryableFetchError(const NetworkResult &result);

}  // namespace bttv::detail

class BttvEmotes final
{
    static constexpr const char *globalEmoteApiUrl =
        "https://api.betterttv.net/3/cached/emotes/global";
    static constexpr const char *bttvChannelEmoteApiUrl =
        "https://api.betterttv.net/3/cached/users/twitch/";

public:
    BttvEmotes();

    std::shared_ptr<const EmoteMap> emotes() const;
    std::optional<EmotePtr> emote(const EmoteName &name) const;
    void loadEmotes();
    void setEmotes(std::shared_ptr<const EmoteMap> emotes);
    static void loadChannel(std::weak_ptr<Channel> channel,
                            const QString &channelId,
                            const QString &channelDisplayName,
                            std::function<void(EmoteMap &&)> callback,
                            bool manualRefresh, bool cacheHit);

    /**
     * Adds an emote to the `channelEmoteMap`.
     * This will _copy_ the emote map and
     * update the `Atomic`.
     *
     * @return The added emote.
     */
    static EmotePtr addEmote(
        const QString &channelDisplayName,
        Atomic<std::shared_ptr<const EmoteMap>> &channelEmoteMap,
        const BttvLiveUpdateEmoteUpdateAddMessage &message);

    /**
     * Updates an emote in this `channelEmoteMap`.
     * This will _copy_ the emote map and
     * update the `Atomic`.
     *
     * @return pair<old emote, new emote> if any emote was updated.
     */
    static std::optional<std::pair<EmotePtr, EmotePtr>> updateEmote(
        const QString &channelDisplayName,
        Atomic<std::shared_ptr<const EmoteMap>> &channelEmoteMap,
        const BttvLiveUpdateEmoteUpdateAddMessage &message);

    /**
     * Removes an emote from this `channelEmoteMap`.
     * This will _copy_ the emote map and
     * update the `Atomic`.
     *
     * @return The removed emote if any emote was removed.
     */
    static std::optional<EmotePtr> removeEmote(
        Atomic<std::shared_ptr<const EmoteMap>> &channelEmoteMap,
        const BttvLiveUpdateEmoteRemoveMessage &message);

private:
    /// Fetches the global BTTV emote list, retrying a bounded number of
    /// times (with a growing delay) on transient network failures.
    void fetchGlobalEmotes(ExponentialBackoff<3> backoff, int attempt);

    /// Fetches a channel's BTTV emotes, retrying a bounded number of times
    /// (with a growing delay) on transient network failures.
    static void fetchChannelEmotes(std::weak_ptr<Channel> channel,
                                   const QString &channelId,
                                   const QString &channelDisplayName,
                                   std::function<void(EmoteMap &&)> callback,
                                   bool manualRefresh, bool cacheHit,
                                   ExponentialBackoff<3> backoff, int attempt);

    Atomic<std::shared_ptr<const EmoteMap>> global_;

    std::vector<std::unique_ptr<pajlada::Signals::ScopedConnection>>
        managedConnections;
};

}  // namespace chatterino
