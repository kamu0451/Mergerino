// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Message.hpp"

#include <pajlada/signals/signal.hpp>
#include <QString>

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

    void setStatusText(QString text, bool notifyAsSystemMessage = false);
    void setLive(bool live);
    void setViewerCount(unsigned count);
    void handleWebMessage(const QString &json);
    void handleRoomInfo(const QJsonObject &root);
    void emitSystemMessage(const QString &text);
    void processDecodedFrame(const tiktok::DecodedFrame &frame);
    MessagePtr buildChatMessage(const tiktok::DecodedChatMessage &chat) const;
    MessagePtr buildActivityMessage(const QString &text,
                                    const QString &loginName = {},
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
