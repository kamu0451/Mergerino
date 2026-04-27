// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Message.hpp"
#include "util/QStringHash.hpp"

#include <pajlada/signals/signal.hpp>
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
    QString previewThumbnailUrl() const;

    pajlada::Signals::Signal<QString> sourceResolved;
    pajlada::Signals::Signal<MessagePtr> messageReceived;
    pajlada::Signals::Signal<MessagePtr> systemMessageReceived;
    pajlada::Signals::NoArgSignal liveStatusChanged;

private:
    void resolveVideoId();
    void resolveChannelIdFromVideoId(const QString &videoId,
                                     std::function<void()> onResolved);
    void probeLiveVideoIdFromSource(const QString &source,
                                    std::function<void(QString)> onResolved);
    void bootstrapInnertubeContext(std::function<void()> onReady,
                                   QString failureText);
    void resolveSourceToVideoId(const QString &source);
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

    static QString maybeExtractVideoId(const QString &url);
    static QString normalizeSource(const QString &source);
    static bool isLikelyChannelId(const QString &value);
    static QString sourceLivePath(const QString &source);
    static bool isLikelyVideoId(const QString &value);
    static QString extractFirstMatch(const QString &text,
                                     const QStringList &patterns);
    static QString extractLiveChatContinuation(
        const QJsonObject &liveChatRenderer);
    static QString extractVideoChannelId(const QString &html);
    static QString extractLiveVideoId(const QString &html);
    static QString extractLiveStreamTitle(const QJsonObject &nextResponse);
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
    QString joinedLiveVideoId_;
};

}  // namespace chatterino
