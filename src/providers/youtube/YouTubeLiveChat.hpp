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
    uint64_t liveViewerCount_{0};
    QElapsedTimer liveChatSessionRefreshTimer_;
    QElapsedTimer liveChatProgressTimer_;

    std::shared_ptr<bool> lifetimeGuard_;
    std::unordered_set<QString> seenMessageIds_;
};

}  // namespace chatterino
