// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Message.hpp"

#include <pajlada/signals/signal.hpp>
#include <QString>

#include <memory>

namespace chatterino {

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

    void start();
    void stop();

    bool isLive() const;
    const QString &roomId() const;
    const QString &username() const;
    const QString &statusText() const;
    const QString &liveTitle() const;

    // Reduces @user / full /@user/live URL / bare username to bare username.
    // Strips TikTok tracking params (enter_from_merge, enter_method, etc.).
    static QString normalizeSource(const QString &ref);

    pajlada::Signals::Signal<QString> sourceResolved;
    pajlada::Signals::Signal<MessagePtr> messageReceived;
    pajlada::Signals::Signal<MessagePtr> systemMessageReceived;
    pajlada::Signals::NoArgSignal liveStatusChanged;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    QString source_;
    QString username_;
    QString roomId_;
    QString statusText_;
    QString liveTitle_;
    bool live_{false};
    bool running_{false};

    std::shared_ptr<bool> lifetimeGuard_;

    void setStatusText(QString text, bool notifyAsSystemMessage = false);
    void setLive(bool live);
    void handleWebMessage(const QString &json);
    void emitSystemMessage(const QString &text);
};

}  // namespace chatterino
