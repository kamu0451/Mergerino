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
    QString getCurrentStreamID() const override;

    QString statusSuffix() const;
    QString tooltipText() const;
    unsigned totalViewerCount() const;

    struct ViewerDelta {
        double percent;
        int spanMinutes;
    };
    std::optional<ViewerDelta> viewerCountDeltaPercent() const;

    ChannelPtr twitchChannel() const;
    ChannelPtr kickChannel() const;

    pajlada::Signals::NoArgSignal streamStatusChanged;

private:
    void initializeSources();
    void connectSourceSignals(const ChannelPtr &source, MessagePlatform platform,
                              pajlada::Signals::SignalHolder &connections);
    void appendInitialMessages(const ChannelPtr &source,
                               MessagePlatform platform);
    void appendMergedMessage(const MessagePtr &source, MessagePlatform platform);
    void fillMergedMessages(const std::vector<MessagePtr> &messages,
                            MessagePlatform platform);
    void replaceMergedMessage(const MessagePtr &previous,
                              const MessagePtr &replacement,
                              MessagePlatform platform);
    std::shared_ptr<Message> createMergedMessage(const MessagePtr &source,
                                                 MessagePlatform platform) const;
    void addYouTubeMessage(const MessagePtr &message);
    void addTikTokMessage(const MessagePtr &message);
    void addSystemStatusMessage(const MessagePtr &message);
    void addSystemStatusMessage(const QString &message);
    void announceJoinedLiveChat(MessagePlatform platform,
                                const QString &title = {});
    void refreshStatusText();

    static bool shouldMirrorSourceMessage(const MessagePtr &message);
    static QColor platformAccent(MessagePlatform platform);
    static EmotePtr platformBadge(MessagePlatform platform);
    static QString messageKey(const MessagePtr &message,
                              MessagePlatform platform);

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

    bool twitchLive_{false};
    bool kickLive_{false};
    bool youtubeLive_{false};
    bool tiktokLive_{false};
    bool twitchLiveJoinAnnounced_{false};
    bool kickLiveJoinAnnounced_{false};
    bool tiktokLiveJoinAnnounced_{false};
};

}  // namespace chatterino
