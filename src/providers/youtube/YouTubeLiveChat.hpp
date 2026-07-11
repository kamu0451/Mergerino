// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Emote.hpp"
#include "messages/Message.hpp"
#include "util/QStringHash.hpp"

#include <pajlada/signals/signal.hpp>
#include <QDateTime>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>

class QJsonObject;
class QJsonValue;

namespace chatterino {

class NetworkResult;
class YouTubeAccount;

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
    void sendMessage(const QString &message);
    void deleteMessage(const QString &messageId);
    void banUser(const QString &channelId, const QString &displayName = {});
    void timeoutUser(const QString &channelId, int durationSeconds,
                     const QString &displayName = {});
    void unbanUser(const QString &channelId, const QString &displayName = {});

    bool isLive() const;
    bool hasModeratorPrivileges() const;
    const QString &videoId() const;
    const QString &statusText() const;
    const QString &liveTitle() const;
    uint64_t liveViewerCount() const;
    unsigned viewerCount() const;
    QString liveUptime() const;
    QString previewThumbnailUrl() const;
    QString resolvedSource() const;

    // Custom (channel/member) chat emoji seen in this source's live chat,
    // keyed by their :shortcut: name. Populated as messages are parsed (see
    // parseRendererMessage) so emote autocomplete can offer them. Standard
    // unicode emoji are intentionally excluded - they already come from the
    // Emoji provider. GUI-thread only, like every other accessor here.
    const EmoteMap &customEmotes() const;

    pajlada::Signals::Signal<QString> sourceResolved;
    pajlada::Signals::Signal<MessagePtr> messageReceived;
    pajlada::Signals::Signal<MessagePtr> systemMessageReceived;
    pajlada::Signals::NoArgSignal liveStatusChanged;
    pajlada::Signals::NoArgSignal viewerCountChanged;
    pajlada::Signals::NoArgSignal moderationStatusChanged;

    static QString maybeExtractVideoId(const QString &url);
    static QString normalizeSource(const QString &source);
    static bool isLikelyChannelId(const QString &value);

    // Anonymously fetch the channel's avatar image URL for `source` (a handle,
    // channelId, channel URL, or custom name). Resolves with an empty string
    // on any failure. Read-only scrape path: uses youtubeHeaders() only and
    // never attaches an account/OAuth token (see file-level INVARIANT).
    static void fetchChannelAvatarUrl(const QString &source,
                                      std::function<void(QString)> onResolved);
    static QString extractLiveChatContinuation(
        const QJsonObject &liveChatRenderer);
    static QString extractLiveStreamTitle(const QJsonObject &nextResponse);

private:
    using ActiveLiveChatIdCallback =
        std::function<void(std::shared_ptr<YouTubeAccount>, QString)>;

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
    void ensureActiveLiveChatId(std::shared_ptr<YouTubeAccount> account,
                                ActiveLiveChatIdCallback callback,
                                QString operationName);
    void postLiveChatMessage(std::shared_ptr<YouTubeAccount> account,
                             QString liveChatId, QString message);
    void postLiveChatBan(std::shared_ptr<YouTubeAccount> account,
                         QString liveChatId, QString channelId,
                         QString displayName, int durationSeconds);
    void deleteLiveChatBan(std::shared_ptr<YouTubeAccount> account,
                           QString banId, QString channelId,
                           QString displayName);
    void verifySourceLiveAfterMissingContinuation(
        const QString &requestedVideoId, bool skipInitialBacklog);
    void poll();
    void refreshLiveChatContinuation(QString text, int retryDelayMs);
    void schedulePoll(int delayMs);
    void scheduleHealthCheck(int delayMs);
    void scheduleResolve(int delayMs);
    void fetchUpdatedMetadata();
    void scheduleUpdatedMetadata(int delayMs);
    bool videoIdRecentlyFailed(const QString &videoId) const;
    void recoverLiveChat(QString text, int retryDelayMs,
                         bool notifyAsSystemMessage = true);
    void waitForNextLive(QString text, int retryDelayMs);
    void resetInnertubeContext();
    void resetModeratorPrivileges();
    bool moderationToolsDisabledForCurrentAccount() const;
    void reportModerationToolsUnavailable();
    void reportModerationActionFailure(const QString &action,
                                       const QString &error,
                                       const NetworkResult &result);
    void updateModeratorPrivilegesFromRenderer(const QJsonObject &renderer);
    void updateLiveViewerCount(uint64_t viewerCount);
    void setLive(bool live);
    void setStatusText(QString text, bool notifyAsSystemMessage = false);
    bool shouldResolveLiveStreamFromSource() const;

    static QString sourceLivePath(const QString &source);
    static QString sourceStreamsPath(const QString &source);
    static bool isLikelyVideoId(const QString &value);
    static QString extractFirstMatch(const QString &text,
                                     const QStringList &patterns);
    static QString extractVideoChannelId(const QString &html);
    static QString extractChannelAvatarUrl(const QString &html);
    static QString extractEmbedLiveVideoId(const QString &html);
    static QString extractLiveVideoId(const QString &html,
                                      bool allowPageCanonical);
    static QString extractLiveOwnerChannelId(const QJsonObject &nextResponse);
    static uint64_t extractLiveViewerCountFromNextResponse(
        const QJsonObject &nextResponse);
    static uint64_t extractLiveViewerCount(const QJsonValue &value);
    static QDateTime extractLiveStartTime(const QJsonObject &nextResponse);
    static bool rendererHasModeratorBadge(const QJsonObject &renderer);
    static QString parseText(const QJsonValue &value);
    static uint64_t parseViewerCount(const QJsonValue &value);
    static MessagePtr parseRendererMessage(const QJsonObject &renderer,
                                          const QString &rendererName,
                                          const QString &channelName,
                                          EmoteMap *customEmoteSink = nullptr);

    QString streamUrl_;
    QString videoId_;
    QString apiKey_;
    QString clientVersion_;
    QString visitorData_;
    QString continuation_;
    QString statusText_;
    QString liveTitle_;
    QString liveOwnerChannelId_;
    QString activeLiveChatVideoId_;
    QString activeLiveChatId_;
    QString moderatorPrivilegesVideoId_;
    QString moderatorPrivilegesAccountChannelId_;
    QString moderationDisabledVideoId_;
    QString moderationDisabledAccountChannelId_;
    QDateTime liveStartedAt_;

    bool running_{false};
    bool live_{false};
    bool hasModeratorPrivileges_{false};
    bool failureReported_{false};
    bool skipInitialBacklog_{false};
    int activePollStreak_{0};
    int pollRefreshFallbackCount_{0};
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
    // Backoff for verifySourceLiveAfterMissingContinuation's same-videoId
    // branch: a stream that is live but has no anonymous chat continuation
    // (chat disabled / members-only) would otherwise re-run the full
    // resolver chain every 3s forever. Doubles the resolve delay per
    // consecutive attempt, capped at 60s. Cleared when a continuation is
    // obtained or keyed off automatically when the videoId changes.
    int missingContinuationStreak_{0};
    QString missingContinuationVideoId_;
    uint64_t liveViewerCount_{0};
    QElapsedTimer liveChatSessionRefreshTimer_;
    QElapsedTimer liveChatProgressTimer_;

    std::shared_ptr<bool> lifetimeGuard_;
    std::unordered_set<QString> seenMessageIds_;
    std::unordered_map<QString, QString> liveChatBanIds_;
    QString joinedLiveVideoId_;
    // Accumulated custom chat emoji for autocomplete. Bounded and deduped by
    // shortcut inside accumulateCustomYouTubeEmoji(). Not cleared between
    // streams: a channel's member/custom emoji persist across broadcasts, so
    // keeping them means they stay completable while the source is loaded.
    EmoteMap customEmotes_;
};

}  // namespace chatterino
