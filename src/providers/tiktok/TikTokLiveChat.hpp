// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Message.hpp"

#include <pajlada/signals/signal.hpp>
#include <QString>

#include <map>
#include <memory>

class QJsonObject;

namespace chatterino {

namespace tiktok {
struct DecodedChatMessage;
struct DecodedLikeEvent;
struct DecodedMemberEvent;
struct DecodedSocialEvent;
struct DecodedGiftEvent;
struct DecodedFrame;
}  // namespace tiktok

// Reads a TikTok LIVE room by hosting a hidden WebView2 that runs TikTok's
// own webapp. TikTok's signing stack (X-Bogus, X-Gnarly, msToken) rotates
// often and no open-source library signs natively; running the real page
// in-process sidesteps that entirely. Our injected script forwards every
// /webcast/ WebSocket frame to C++ via chrome.webview.postMessage.
//
// Public surface intentionally mirrors YouTubeLiveChat so MergedChannel can
// treat both providers symmetrically.
class TikTokLiveChat
{
public:
    explicit TikTokLiveChat(QString source);
    ~TikTokLiveChat();

    TikTokLiveChat(const TikTokLiveChat &) = delete;
    TikTokLiveChat &operator=(const TikTokLiveChat &) = delete;

    // Returns a shared instance for `source`. Sharing is especially important
    // here because each TikTokLiveChat hosts its own WebView2 (memory- and
    // CPU-expensive); two MergedChannels for the same TikTok username should
    // not spin up two browser hosts. Calls start() lazily on first creation.
    static std::shared_ptr<TikTokLiveChat> getOrCreateShared(
        const QString &source);

    /// Drops the registry strong refs and releases the process-wide shared
    /// `ICoreWebView2Environment`. Call from Application::aboutToQuit() so
    /// the env is released while Qt's event loop and the WebView2 message
    /// pump are still alive; releasing it at static-destructor time (after
    /// main returns) is a known crash pattern on Windows.
    static void releaseSharedEnvironment();

    void start();
    void stop();

    /// Sibling-platform live hint from a consuming MergedChannel, keyed by
    /// consumer so multiple channels sharing this source aggregate by OR.
    /// While any sibling (Twitch/Kick/YouTube) is live, the offline recheck
    /// reloads on the fast cadence - the streamer is actively broadcasting,
    /// so a TikTok live could start any moment. With nothing live anywhere
    /// it drops to the idle cadence (channels virtually never go live on
    /// TikTok alone). A false->true aggregate transition also pokes an
    /// immediate recheck so TikTok is joined within seconds of the sibling
    /// going live instead of a full interval later.
    void setSiblingLiveHint(const void *consumer, bool siblingLive);
    /// Forgets a consumer's hint; call when the MergedChannel goes away.
    void removeSiblingLiveHint(const void *consumer);

    bool isLive() const;
    const QString &roomId() const;
    const QString &username() const;
    const QString &statusText() const;
    const QString &liveTitle() const;
    unsigned viewerCount() const;

    // Reduces @user / full /@user/live URL / bare username to bare username.
    // Strips TikTok tracking params (enter_from_merge, enter_method, etc.).
    static QString normalizeSource(const QString &ref);

    pajlada::Signals::Signal<QString> sourceResolved;
    pajlada::Signals::Signal<MessagePtr> messageReceived;
    pajlada::Signals::Signal<MessagePtr> systemMessageReceived;
    pajlada::Signals::NoArgSignal liveStatusChanged;
    pajlada::Signals::NoArgSignal viewerCountChanged;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    QString username_;
    QString roomId_;
    QString statusText_;
    QString liveTitle_;
    unsigned viewerCount_{0};
    bool live_{false};
    bool running_{false};

    std::shared_ptr<bool> lifetimeGuard_;

    // Per-consumer sibling-live hints; aggregate is OR. Lives on the outer
    // object (not Impl) so it survives stop()/start() cycles.
    std::map<const void *, bool> siblingLiveHints_;

    void setStatusText(QString text, bool notifyAsSystemMessage = false);
    void setLive(bool live);
    void setViewerCount(unsigned count);
    void armOfflineRecheck();
    bool anySiblingLive() const;
    void refreshOfflineRecheckCadence(bool siblingNowLive);
    void performOfflineRecheckReload();
    /// (Re-)registers the image/media/font + CDN-segment WebResourceRequested
    /// filters and the blocking handler. Idempotent: safe to call after
    /// promoteToLoginHost() detached the handler or after a login-mode start
    /// that never installed it.
    void installResourceBlocking();
    void handleWebMessage(const QString &json);
    void handleRoomInfo(const QJsonObject &root);
    void emitSystemMessage(const QString &text);
    void processDecodedFrame(const tiktok::DecodedFrame &frame);
    MessagePtr buildChatMessage(const tiktok::DecodedChatMessage &chat) const;
    MessagePtr buildActivityMessage(
        const QString &text, const QString &loginName = {},
        Message::TikTokActivityKind kind = Message::TikTokActivityKind::None,
        uint32_t diamondCount = 0) const;

    void launchControllerCreate();
    void handleLike(const tiktok::DecodedLikeEvent &ev);
    void handleMember(const tiktok::DecodedMemberEvent &ev);
    void handleSocial(const tiktok::DecodedSocialEvent &ev);
    void handleGift(const tiktok::DecodedGiftEvent &ev);
    void flushPendingLikes();
    void flushPendingJoins();
};

}  // namespace chatterino
