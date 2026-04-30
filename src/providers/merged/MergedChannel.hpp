// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "common/ChannelChatters.hpp"
#include "messages/Emote.hpp"
#include "messages/Message.hpp"
#include "util/QStringHash.hpp"

#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QColor>
#include <QString>

#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace chatterino {

class YouTubeLiveChat;
class TikTokLiveChat;

struct MergedChannelConfig {
    QString tabName;
    bool twitchEnabled{true};
    QString twitchChannelName;
    bool kickEnabled{true};
    QString kickChannelName;
    bool youtubeEnabled{false};
    QString youtubeStreamUrl;
    bool tiktokEnabled{false};
    QString tiktokUsername;

    bool operator==(const MergedChannelConfig &) const = default;

    QString displayName() const;
    QString effectiveTwitchChannelName() const;
    QString effectiveKickChannelName() const;
};

class MergedChannel final : public Channel, public ChannelChatters
{
public:
    explicit MergedChannel(MergedChannelConfig config);
    ~MergedChannel() override;

    const MergedChannelConfig &config() const;
    const QString &getDisplayName() const override;
    const QString &getLocalizedName() const override;
    bool isMergedChannel() const override;

    bool canSendMessage() const override;
    bool isWritable() const override;
    void sendMessage(const QString &message) override;
    bool isMod() const override;
    bool isBroadcaster() const override;
    bool hasModRights() const override;
    bool isLive() const override;
    bool isRerun() const override;
    bool canReconnect() const override;
    void reconnect() override;
    QString getCurrentStreamID() const override;

    QString statusSuffix() const;
    QString tooltipText() const;
    unsigned totalViewerCount() const;

    /// Returns the URL to open in a browser when the user middle-clicks the
    /// split header. Picks by priority Twitch > Kick > YouTube > TikTok,
    /// preferring a platform that is currently live; falls back to the
    /// highest-priority configured platform when nothing is live. Returns
    /// an empty string if no source is configured.
    QString browserStreamUrl() const;

    struct ViewerDelta {
        double percent;
        int spanMinutes;
    };
    std::optional<ViewerDelta> viewerCountDeltaPercent() const;

    ChannelPtr twitchChannel() const;
    ChannelPtr kickChannel() const;
    YouTubeLiveChat *youtubeLiveChat() const;

    pajlada::Signals::NoArgSignal streamStatusChanged;

protected:
    QString getCurrentStreamIDForMessage(const Message &message) const override;

public:
    // Exposed for unit tests. These are pure helpers that operate on message
    // contents alone; they hold no MergedChannel state.
    static bool shouldMirrorSourceMessage(const MessagePtr &message);
    static QString messageKey(const MessagePtr &message,
                              MessagePlatform platform);

    /// Returns the number of milliseconds the caller should wait before
    /// attempting another send, given the last send time, the current time,
    /// and the platform's minimum interval. Returns 0 if a send is allowed
    /// immediately (including when lastSendMs == 0 meaning "never sent").
    static qint64 sendWaitMs(qint64 lastSendMs, qint64 nowMs,
                             qint64 intervalMs);

    /// Debug helper: injects a synthetic platform-tagged message into the
    /// merged view as if it came from a real source. The message flows
    /// through the same appendMergedMessage path real source signals use,
    /// so the platform badge / accent / dedup are applied. Used by the
    /// /simmsg debug command to exercise the merged-chat UI without real
    /// streams.
    void injectDebugMessage(MessagePlatform platform, const QString &user,
                            const QString &text);

private:
    void initializeSources();
    void connectSourceSignals(const ChannelPtr &source, MessagePlatform platform,
                              pajlada::Signals::SignalHolder &connections);
    void appendInitialMessages(const ChannelPtr &source,
                               MessagePlatform platform);
    void addMergedMessagesAtStart(const std::vector<MessagePtr> &messages,
                                  MessagePlatform platform);
    void fillInMergedMessages(const std::vector<MessagePtr> &messages,
                              MessagePlatform platform);
    void appendMergedMessage(const MessagePtr &source, MessagePlatform platform);
    void replaceMergedMessage(const MessagePtr &previous,
                              const MessagePtr &replacement,
                              MessagePlatform platform);
    std::shared_ptr<Message> createAndTrackMergedMessage(
        const MessagePtr &source, MessagePlatform platform,
        bool markAsRecent = false);
    std::shared_ptr<Message> createMergedMessage(const MessagePtr &source,
                                                 MessagePlatform platform,
                                                 bool markAsRecent = false) const;
    void addSystemStatusMessage(const MessagePtr &message);
    void addSystemStatusMessage(const QString &message);
    void announceJoinedLiveChat(MessagePlatform platform,
                                const QString &title = {});
    void refreshStatusText();

    static QColor platformAccent(MessagePlatform platform);
    static EmotePtr platformBadge(MessagePlatform platform);

    void insertMirror(const QString &key, const MessagePtr &merged);
    void eraseMirror(const QString &key);
    void clearMirrorsForPlatform(MessagePlatform platform);

    static constexpr std::size_t MAX_MIRRORED_MESSAGES = 1000;

    MergedChannelConfig config_;
    QString displayName_;
    QString tooltipText_;

    ChannelPtr twitchChannel_;
    ChannelPtr kickChannel_;
    std::unique_ptr<YouTubeLiveChat> youtubeLiveChat_;
    std::unique_ptr<TikTokLiveChat> tiktokLiveChat_;

    pajlada::Signals::SignalHolder twitchConnections_;
    pajlada::Signals::SignalHolder kickConnections_;
    pajlada::Signals::SignalHolder youtubeConnections_;
    pajlada::Signals::SignalHolder tiktokConnections_;

    std::unordered_map<QString, MessagePtr> mirroredMessages_;
    std::deque<QString> mirroredOrder_;

    // Sliding window of (msEpoch, totalViewerCount) samples for computing
    // the 5-minute delta shown in the header.
    mutable std::deque<std::pair<qint64, unsigned>> viewerCountHistory_;

    // Client-side outbound rate limiting. Keeps a macro-held Enter from
    // tripping Twitch/Kick server-side rate limits (which can time the user
    // out). Values are msSinceEpoch of the last successful fan-out.
    qint64 lastTwitchSendMs_{0};
    qint64 lastKickSendMs_{0};

    bool twitchLive_{false};
    bool kickLive_{false};
    bool youtubeLive_{false};
    bool tiktokLive_{false};
    bool twitchLiveJoinAnnounced_{false};
    bool kickLiveJoinAnnounced_{false};
    bool tiktokLiveJoinAnnounced_{false};
    bool youtubeLiveJoinAnnounced_{false};
};

}  // namespace chatterino
