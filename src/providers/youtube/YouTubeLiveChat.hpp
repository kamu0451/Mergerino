// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Message.hpp"
#include "util/QStringHash.hpp"

#include <pajlada/signals/signal.hpp>
#include <QDateTime>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <unordered_set>

class QJsonObject;
class QJsonValue;

namespace chatterino {

class YouTubeLiveChat
{
public:
    explicit YouTubeLiveChat(QString streamUrl);
    ~YouTubeLiveChat();

    // Returns a shared instance for `source` (the user-entered handle / URL /
    // channelId). Multiple MergedChannels with the same source share one
    // instance: one set of network polls, one resolver state, one announce
    // ladder. Calls `start()` lazily on first creation. The instance dies
    // when the last shared_ptr drops.
    static std::shared_ptr<YouTubeLiveChat> getOrCreateShared(
        const QString &source);

    void start();
    void stop();

    bool isLive() const;
    const QString &videoId() const;
    const QString &statusText() const;
    const QString &liveTitle() const;
    uint64_t liveViewerCount() const;
    unsigned viewerCount() const;
    QString liveUptime() const;
    QString previewThumbnailUrl() const;

    pajlada::Signals::Signal<QString> sourceResolved;
    pajlada::Signals::Signal<MessagePtr> messageReceived;
    pajlada::Signals::Signal<MessagePtr> systemMessageReceived;
    pajlada::Signals::NoArgSignal liveStatusChanged;
    pajlada::Signals::NoArgSignal viewerCountChanged;

    static QString maybeExtractVideoId(const QString &url);
    static QString normalizeSource(const QString &source);
    static bool isLikelyChannelId(const QString &value);
    static QString extractLiveChatContinuation(
        const QJsonObject &liveChatRenderer);
    static QString extractLiveStreamTitle(const QJsonObject &nextResponse);

private:
    void resolveVideoId();
    void resolveChannelIdFromSource(const QString &source,
                                    std::function<void(QString)> onResolved);
    void resolveChannelIdFromHandle(const QString &handle,
                                    std::function<void(QString)> onResolved);
    void resolveChannelIdFromVideoId(const QString &videoId,
                                     std::function<void(QString)> onResolved);
    void resolveChannelIdFromSearch(const QString &source,
                                    std::function<void(QString)> onResolved);
    void probeLiveVideoIdFromSource(const QString &source,
                                    std::function<void(QString)> onResolved);
    void probeLiveVideoIdFromEmbed(const QString &channelId,
                                   std::function<void(QString)> onResolved);
    void probeLiveVideoIdFromPath(const QString &path,
                                  std::function<void(QString)> onResolved);
    void probeLiveVideoIdFromBrowse(const QString &channelId,
                                    std::function<void(QString)> onResolved);
    void probeLiveVideoIdFromBrowseTab(const QString &channelId,
                                       const QString &params,
                                       const QString &refererPath,
                                       std::function<void(QString)> onResolved);
    void bootstrapInnertubeContext(std::function<void()> onReady,
                                   QString failureText);
    void fetchLiveChatPage(bool skipInitialBacklog = true);
    void poll();
    void schedulePoll(int delayMs);
    void scheduleHealthCheck(int delayMs);
    void scheduleResolve(int delayMs);
    bool videoIdRecentlyFailed(const QString &videoId) const;
    void recoverLiveChat(QString text, int retryDelayMs);
    void waitForNextLive(QString text, int retryDelayMs);
    void resetInnertubeContext();
    void setLive(bool live);
    void setStatusText(QString text, bool notifyAsSystemMessage = false);
    bool shouldResolveLiveStreamFromSource() const;
    QString resolvedSource() const;

    static QString sourceLivePath(const QString &source);
    static QString sourceStreamsPath(const QString &source);
    static bool isLikelyVideoId(const QString &value);
    static QString extractFirstMatch(const QString &text,
                                     const QStringList &patterns);
    static QString extractVideoChannelId(const QString &html);
    static QString extractEmbedLiveVideoId(const QString &html);
    static QString extractLiveVideoId(const QString &html,
                                      bool allowPageCanonical);
    static QDateTime extractLiveStartTime(const QJsonObject &nextResponse);
    static QString parseText(const QJsonValue &value);
    static uint64_t parseViewerCount(const QJsonValue &value);
    static MessagePtr parseRendererMessage(const QJsonObject &renderer,
                                          const QString &rendererName,
                                          const QString &channelName);

    QString streamUrl_;
    QString videoId_;
    QString apiKey_;
    QString clientVersion_;
    QString visitorData_;
    QString continuation_;
    QString statusText_;
    QString liveTitle_;
    QDateTime liveStartedAt_;

    bool running_{false};
    bool live_{false};
    bool failureReported_{false};
    bool skipInitialBacklog_{false};
    int activePollStreak_{0};
    // Recovery escalation: counts consecutive recoverLiveChat() calls since
    // the last successful poll. Reset by poll success and waitForNextLive.
    // After hitting the escalation threshold we promote to waitForNextLive
    // so the tab's live marker turns off and we stop pretending the stream
    // is live - covers genuine end-of-stream and the case where the resolver
    // settled on a non-live page (e.g. a Shorts thumbnail).
    int consecutiveRecoveries_{0};
    // Tracks the videoId that the rebootstrap loop just escalated on. If
    // resolveVideoId hands back the SAME id within `recentlyFailedWindowMs`,
    // we know the channel's /live endpoint is still serving a stale or non-
    // live page (typical: Shorts thumbnails) - treat as offline instead of
    // setting live=true and retrying the same dead session every ~12s.
    QString recentlyFailedVideoId_;
    QElapsedTimer recentlyFailedTimer_;
    // How many times in a row this same videoId has been escalated. Each new
    // failure on the same id doubles the cooldown (5min, 10min, 20min, ...
    // up to recentlyFailedMaxWindowMs) so a chronically-bogus videoId stops
    // re-flashing the live marker every 5 minutes.
    int recentlyFailedStreak_{0};
    static constexpr qint64 recentlyFailedBaseWindowMs = 5LL * 60 * 1000;
    static constexpr qint64 recentlyFailedMaxWindowMs = 60LL * 60 * 1000;
    uint64_t liveViewerCount_{0};
    QElapsedTimer liveChatSessionRefreshTimer_;
    QElapsedTimer liveChatProgressTimer_;

    std::shared_ptr<bool> lifetimeGuard_;
    std::unordered_set<QString> seenMessageIds_;
};

}  // namespace chatterino
