// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "providers/tiktok/TikTokLiveChat.hpp"

#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/tiktok/TikTokFrameDecoder.hpp"
#include "providers/tiktok/TikTokInjectScript.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <memory>

// Windows / WebView2 - kept out of the public header.
// clang-format off
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
// clang-format on

namespace chatterino {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"MergerinoTikTokWebView2Host";
std::atomic_flag g_windowClassRegistered = ATOMIC_FLAG_INIT;

LRESULT CALLBACK hostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ensureWindowClass()
{
    if (g_windowClassRegistered.test_and_set())
    {
        return;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = hostWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    RegisterClassW(&wc);
}

std::wstring toWide(const QString &s)
{
    return std::wstring(reinterpret_cast<const wchar_t *>(s.utf16()),
                        static_cast<std::size_t>(s.size()));
}

QString fromWide(const wchar_t *data)
{
    if (data == nullptr)
    {
        return {};
    }
    return QString::fromWCharArray(data);
}

}  // namespace

struct TikTokLiveChat::Impl {
    HWND host{};
    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    EventRegistrationToken navToken{};
    EventRegistrationToken msgToken{};

    // Activity-event aggregation. TikTok likes / joins can burst at tens per
    // second; we accumulate and flush on a debounce timer.
    QTimer likeTimer;
    QTimer joinTimer;
    qint32 pendingLikeCount{0};
    qint64 pendingLikeTotal{0};
    QString pendingLikeUser;
    qint32 pendingJoinCount{0};
    QString pendingJoinUser;

    ~Impl()
    {
        if (webview)
        {
            if (navToken.value != 0)
            {
                webview->remove_NavigationCompleted(navToken);
            }
            if (msgToken.value != 0)
            {
                webview->remove_WebMessageReceived(msgToken);
            }
        }
        if (controller)
        {
            controller->Close();
        }
        if (host != nullptr)
        {
            DestroyWindow(host);
        }
    }
};

TikTokLiveChat::TikTokLiveChat(QString source)
    : source_(std::move(source))
    , lifetimeGuard_(std::make_shared<bool>(true))
{
    this->username_ = TikTokLiveChat::normalizeSource(this->source_);
}

TikTokLiveChat::~TikTokLiveChat()
{
    this->stop();
}

void TikTokLiveChat::start()
{
    if (this->running_)
    {
        return;
    }
    if (this->username_.isEmpty())
    {
        this->setStatusText(QStringLiteral("Invalid TikTok source"), true);
        return;
    }
    this->running_ = true;
    this->sourceResolved.invoke(this->username_);
    this->setStatusText(QStringLiteral("Connecting to TikTok..."), false);
    this->emitSystemMessage(
        QStringLiteral("[TikTok diag] start() entered for @%1")
            .arg(this->username_));

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE)
    {
        this->setStatusText(
            QStringLiteral("TikTok: CoInitializeEx failed (0x%1)")
                .arg(static_cast<quint32>(hrCo), 8, 16, QChar('0')),
            true);
        this->running_ = false;
        return;
    }

    ensureWindowClass();
    this->impl_ = std::make_unique<Impl>();
    this->impl_->likeTimer.setSingleShot(true);
    this->impl_->likeTimer.setInterval(1000);
    this->impl_->joinTimer.setSingleShot(true);
    this->impl_->joinTimer.setInterval(1500);
    QObject::connect(&this->impl_->likeTimer, &QTimer::timeout,
                     [this]() { this->flushPendingLikes(); });
    QObject::connect(&this->impl_->joinTimer, &QTimer::timeout,
                     [this]() { this->flushPendingJoins(); });
    this->impl_->host = CreateWindowExW(
        0, kWindowClass, L"mergerino-tiktok-host", WS_POPUP, 0, 0, 1, 1,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (this->impl_->host == nullptr)
    {
        this->setStatusText(QStringLiteral("TikTok: CreateWindow failed"), true);
        this->impl_.reset();
        this->running_ = false;
        return;
    }

    const QString userDataDir =
        QDir::toNativeSeparators(
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
            QStringLiteral("/WebView2"));
    QDir().mkpath(userDataDir);
    const std::wstring userDataDirW = toWide(userDataDir);

    std::weak_ptr<bool> guard = this->lifetimeGuard_;

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataDirW.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, guard](HRESULT envHr,
                          ICoreWebView2Environment *env) -> HRESULT {
                if (guard.expired())
                {
                    return S_OK;
                }
                if (FAILED(envHr) || env == nullptr)
                {
                    this->setStatusText(
                        QStringLiteral(
                            "TikTok: WebView2 Runtime unavailable (0x%1)")
                            .arg(static_cast<quint32>(envHr), 8, 16,
                                 QChar('0')),
                        true);
                    this->running_ = false;
                    return S_OK;
                }
                this->impl_->env = env;
                this->emitSystemMessage(QStringLiteral(
                    "[TikTok diag] WebView2 environment ready, creating "
                    "controller"));

                return env->CreateCoreWebView2Controller(
                    this->impl_->host,
                    Callback<
                        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this, guard](
                            HRESULT ctrlHr,
                            ICoreWebView2Controller *controller) -> HRESULT {
                            if (guard.expired())
                            {
                                return S_OK;
                            }
                            if (FAILED(ctrlHr) || controller == nullptr)
                            {
                                this->setStatusText(
                                    QStringLiteral(
                                        "TikTok: controller failed (0x%1)")
                                        .arg(static_cast<quint32>(ctrlHr), 8,
                                             16, QChar('0')),
                                    true);
                                this->running_ = false;
                                return S_OK;
                            }
                            this->impl_->controller = controller;
                            controller->get_CoreWebView2(&this->impl_->webview);
                            if (!this->impl_->webview)
                            {
                                this->setStatusText(
                                    QStringLiteral("TikTok: no webview"), true);
                                this->running_ = false;
                                return S_OK;
                            }
                            this->emitSystemMessage(QStringLiteral(
                                "[TikTok diag] Controller + webview ready, "
                                "wiring handlers"));

                            // NavigationCompleted: detect page load state
                            this->impl_->webview->add_NavigationCompleted(
                                Callback<
                                    ICoreWebView2NavigationCompletedEventHandler>(
                                    [this, guard](
                                        ICoreWebView2 *,
                                        ICoreWebView2NavigationCompletedEventArgs
                                            *args) -> HRESULT {
                                        if (guard.expired())
                                        {
                                            return S_OK;
                                        }
                                        BOOL ok = FALSE;
                                        args->get_IsSuccess(&ok);
                                        COREWEBVIEW2_WEB_ERROR_STATUS status{};
                                        args->get_WebErrorStatus(&status);
                                        this->emitSystemMessage(
                                            QStringLiteral(
                                                "[TikTok diag] "
                                                "NavigationCompleted "
                                                "(ok=%1, status=%2)")
                                                .arg(ok ? "true" : "false")
                                                .arg(static_cast<int>(status)));
                                        if (!ok)
                                        {
                                            this->setStatusText(
                                                QStringLiteral(
                                                    "TikTok: navigation failed"),
                                                true);
                                            return S_OK;
                                        }
                                        // Post-nav sanity probe. Uses
                                        // ExecuteScript's result callback
                                        // rather than postMessage so we get
                                        // the answer even if chrome.webview
                                        // IPC is wedged. Returns a JSON
                                        // string that C++ parses below.
                                        const wchar_t *postNavProbe = L"(function(){"
                                            L"try{"
                                            L"return JSON.stringify({"
                                            L"url:String(location.href),"
                                            L"title:String(document.title||''),"
                                            L"readyState:String(document.readyState),"
                                            L"chromeWebview:"
                                            L"!!(window.chrome&&window.chrome.webview),"
                                            L"wsPatched:!!(window.WebSocket&&window.WebSocket.__mergerinoPatched),"
                                            L"fetchPatched:!!(window.fetch&&window.fetch.__mergerinoPatched)"
                                            L"});"
                                            L"}catch(e){return JSON.stringify({error:String(e)});}"
                                            L"})()";
                                        HRESULT execHr =
                                            this->impl_->webview->ExecuteScript(
                                                postNavProbe,
                                                Callback<
                                                    ICoreWebView2ExecuteScriptCompletedHandler>(
                                                    [this, guard](
                                                        HRESULT errorCode,
                                                        LPCWSTR resultJson)
                                                        -> HRESULT {
                                                        if (guard.expired())
                                                        {
                                                            return S_OK;
                                                        }
                                                        if (FAILED(errorCode))
                                                        {
                                                            this->emitSystemMessage(
                                                                QStringLiteral(
                                                                    "[TikTok "
                                                                    "diag] "
                                                                    "ExecuteScript "
                                                                    "failed "
                                                                    "(0x%1)")
                                                                    .arg(
                                                                        static_cast<
                                                                            quint32>(
                                                                            errorCode),
                                                                        8, 16,
                                                                        QChar(
                                                                            '0')));
                                                            return S_OK;
                                                        }
                                                        this->emitSystemMessage(
                                                            QStringLiteral(
                                                                "[TikTok diag] "
                                                                "ExecuteScript "
                                                                "result: %1")
                                                                .arg(fromWide(
                                                                    resultJson)));
                                                        return S_OK;
                                                    })
                                                    .Get());
                                        if (FAILED(execHr))
                                        {
                                            this->emitSystemMessage(
                                                QStringLiteral(
                                                    "[TikTok diag] "
                                                    "ExecuteScript dispatch "
                                                    "failed synchronously "
                                                    "(0x%1)")
                                                    .arg(
                                                        static_cast<quint32>(
                                                            execHr),
                                                        8, 16, QChar('0')));
                                        }
                                        return S_OK;
                                    })
                                    .Get(),
                                &this->impl_->navToken);

                            // WebMessageReceived: payloads from the JS hook
                            this->impl_->webview->add_WebMessageReceived(
                                Callback<
                                    ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this, guard](
                                        ICoreWebView2 *,
                                        ICoreWebView2WebMessageReceivedEventArgs
                                            *args) -> HRESULT {
                                        if (guard.expired())
                                        {
                                            return S_OK;
                                        }
                                        LPWSTR json = nullptr;
                                        if (SUCCEEDED(
                                                args->get_WebMessageAsJson(
                                                    &json)) &&
                                            json != nullptr)
                                        {
                                            this->handleWebMessage(
                                                fromWide(json));
                                            CoTaskMemFree(json);
                                        }
                                        return S_OK;
                                    })
                                    .Get(),
                                &this->impl_->msgToken);

                            // Inject the WebSocket hook before the first page
                            // script runs. AddScriptToExecuteOnDocumentCreated
                            // is async: the script is only registered once
                            // the completion callback fires. If we call
                            // Navigate before then, the page's first document
                            // starts loading without our hook and TikTok's
                            // /@user/live SPA never navigates again, so we'd
                            // miss every WebSocket. Navigate from inside the
                            // completion handler instead.
                            const QString url =
                                QStringLiteral("https://www.tiktok.com/@%1/live")
                                    .arg(this->username_);
                            this->impl_->webview
                                ->AddScriptToExecuteOnDocumentCreated(
                                    std::wstring(tiktok::kInjectScript).c_str(),
                                    Callback<
                                        ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                                        [this, guard, url](
                                            HRESULT addHr,
                                            LPCWSTR) -> HRESULT {
                                            if (guard.expired())
                                            {
                                                return S_OK;
                                            }
                                            if (FAILED(addHr))
                                            {
                                                this->emitSystemMessage(
                                                    QStringLiteral(
                                                        "[TikTok diag] "
                                                        "AddScriptToExecuteOnDocumentCreated "
                                                        "failed (0x%1)")
                                                        .arg(
                                                            static_cast<
                                                                quint32>(addHr),
                                                            8, 16, QChar('0')));
                                                return S_OK;
                                            }
                                            this->emitSystemMessage(
                                                QStringLiteral(
                                                    "[TikTok diag] "
                                                    "Script registered, "
                                                    "navigating to %1")
                                                    .arg(url));
                                            HRESULT navHr =
                                                this->impl_->webview->Navigate(
                                                    toWide(url).c_str());
                                            if (FAILED(navHr))
                                            {
                                                this->emitSystemMessage(
                                                    QStringLiteral(
                                                        "[TikTok diag] "
                                                        "Navigate() failed "
                                                        "synchronously "
                                                        "(0x%1)")
                                                        .arg(
                                                            static_cast<
                                                                quint32>(navHr),
                                                            8, 16, QChar('0')));
                                            }
                                            return S_OK;
                                        })
                                        .Get());
                            return S_OK;
                        })
                        .Get());
            })
            .Get());

    if (FAILED(hr))
    {
        this->setStatusText(
            QStringLiteral("TikTok: env create failed (0x%1)")
                .arg(static_cast<quint32>(hr), 8, 16, QChar('0')),
            true);
        this->impl_.reset();
        this->running_ = false;
    }
}

void TikTokLiveChat::stop()
{
    this->running_ = false;
    this->setLive(false);
    this->impl_.reset();
}

bool TikTokLiveChat::isLive() const
{
    return this->live_;
}

const QString &TikTokLiveChat::roomId() const
{
    return this->roomId_;
}

const QString &TikTokLiveChat::username() const
{
    return this->username_;
}

const QString &TikTokLiveChat::statusText() const
{
    return this->statusText_;
}

const QString &TikTokLiveChat::liveTitle() const
{
    return this->liveTitle_;
}

QString TikTokLiveChat::normalizeSource(const QString &ref)
{
    QString s = ref.trimmed();
    if (s.isEmpty())
    {
        return {};
    }
    static const QRegularExpression kUrl(
        QStringLiteral("^https?://(?:www\\.|m\\.)?tiktok\\.com/@([^/?#]+)"),
        QRegularExpression::CaseInsensitiveOption);
    auto m = kUrl.match(s);
    if (m.hasMatch())
    {
        return m.captured(1);
    }
    if (s.startsWith('@'))
    {
        s = s.mid(1);
    }
    // Trim any trailing /live or query string the user may have left in.
    int cut = s.indexOf('/');
    if (cut >= 0)
    {
        s = s.left(cut);
    }
    cut = s.indexOf('?');
    if (cut >= 0)
    {
        s = s.left(cut);
    }
    return s;
}

void TikTokLiveChat::setStatusText(QString text, bool notifyAsSystemMessage)
{
    this->statusText_ = std::move(text);
    if (notifyAsSystemMessage && !this->statusText_.isEmpty())
    {
        this->emitSystemMessage(this->statusText_);
    }
}

void TikTokLiveChat::setLive(bool live)
{
    if (this->live_ == live)
    {
        return;
    }
    this->live_ = live;
    this->liveStatusChanged.invoke();
}

void TikTokLiveChat::emitSystemMessage(const QString &text)
{
    this->systemMessageReceived.invoke(makeSystemMessage(text));
}

MessagePtr TikTokLiveChat::buildChatMessage(const tiktok::DecodedChatMessage &chat) const
{
    const QString displayName = !chat.user.nickname.isEmpty()
                                    ? chat.user.nickname
                                    : chat.user.uniqueId;
    const QString loginName = !chat.user.uniqueId.isEmpty()
                                  ? chat.user.uniqueId
                                  : chat.user.nickname;

    MessageBuilder b;
    b->platform = MessagePlatform::TikTok;
    if (chat.msgId != 0)
    {
        b->id = QString::number(chat.msgId);
    }
    b->loginName = loginName;
    b->displayName = displayName;
    b->localizedName = displayName;
    if (chat.user.userId != 0)
    {
        b->userID = QString::number(chat.user.userId);
    }
    b->channelName = this->username_;
    b->messageText = chat.content;
    b->searchText = displayName + QStringLiteral(": ") + chat.content;
    b->serverReceivedTime = QDateTime::currentDateTime();

    b.emplace<TimestampElement>(b->serverReceivedTime.toLocalTime().time());
    b.emplace<TextElement>(
         displayName + QStringLiteral(":"), MessageElementFlag::Username,
         MessageColor::Text, FontStyle::ChatMediumBold)
        ->setLink({Link::UserInfo, displayName});
    b.emplace<TextElement>(chat.content, MessageElementFlag::Text);
    return b.release();
}

void TikTokLiveChat::handleWebMessage(const QString &json)
{
    QJsonParseError err{};
    auto doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
    {
        return;
    }
    const auto obj = doc.object();
    const QString kind = obj.value(QStringLiteral("kind")).toString();

    if (kind == QStringLiteral("ws-open"))
    {
        this->setLive(true);
        this->setStatusText(QStringLiteral("Joined TikTok live chat"), true);
        return;
    }
    if (kind == QStringLiteral("ws-close"))
    {
        this->setLive(false);
        this->setStatusText(QStringLiteral("TikTok live chat disconnected"),
                            true);
        return;
    }
    if (kind == QStringLiteral("ws-error"))
    {
        this->setStatusText(QStringLiteral("TikTok live chat error"), true);
        return;
    }
    if (kind == QStringLiteral("ws-probe"))
    {
        const QString url = obj.value(QStringLiteral("url")).toString();
        this->emitSystemMessage(
            QStringLiteral("[TikTok diag] WebSocket opened: %1").arg(url));
        return;
    }
    if (kind == QStringLiteral("http-probe"))
    {
        const QString url = obj.value(QStringLiteral("url")).toString();
        const QString via = obj.value(QStringLiteral("via")).toString();
        this->emitSystemMessage(
            QStringLiteral("[TikTok diag] %1 request: %2").arg(via, url));
        return;
    }
    if (kind == QStringLiteral("nav-end"))
    {
        const QString url = obj.value(QStringLiteral("url")).toString();
        const QString title = obj.value(QStringLiteral("title")).toString();
        this->emitSystemMessage(
            QStringLiteral("[TikTok diag] Navigated to %1 (title: %2)")
                .arg(url, title));
        return;
    }
    if (kind == QStringLiteral("exec-probe"))
    {
        const QString url = obj.value(QStringLiteral("url")).toString();
        const QString title = obj.value(QStringLiteral("title")).toString();
        const QString readyState =
            obj.value(QStringLiteral("readyState")).toString();
        const bool chromeWebview =
            obj.value(QStringLiteral("chromeWebviewAvailable")).toBool();
        const bool wsPatched = obj.value(QStringLiteral("wsPatched")).toBool();
        this->emitSystemMessage(
            QStringLiteral(
                "[TikTok diag] ExecuteScript probe: url=%1, title=%2, "
                "readyState=%3, chrome.webview=%4, ws-patched=%5")
                .arg(url, title, readyState,
                     chromeWebview ? "yes" : "no", wsPatched ? "yes" : "no"));
        return;
    }
    if (kind == QStringLiteral("ws-binary"))
    {
        const QString base64 = obj.value(QStringLiteral("base64")).toString();
        if (base64.isEmpty())
        {
            return;
        }
        const QByteArray raw = QByteArray::fromBase64(base64.toLatin1());
        if (raw.isEmpty())
        {
            return;
        }
        const auto frame = tiktok::decodeWebcastPushFrame(raw);
        for (const auto &chat : frame.chatMessages)
        {
            this->messageReceived.invoke(this->buildChatMessage(chat));
        }
        for (const auto &ev : frame.likeEvents)
        {
            this->handleLike(ev);
        }
        for (const auto &ev : frame.memberEvents)
        {
            this->handleMember(ev);
        }
        for (const auto &ev : frame.socialEvents)
        {
            this->handleSocial(ev);
        }
        for (const auto &ev : frame.giftEvents)
        {
            this->handleGift(ev);
        }
    }
}

MessagePtr TikTokLiveChat::buildActivityMessage(const QString &text,
                                                const QString &loginName) const
{
    MessageBuilder b;
    b->platform = MessagePlatform::TikTok;
    b->flags.set(MessageFlag::System);
    b->flags.set(MessageFlag::Subscription);
    b->flags.set(MessageFlag::DoNotTriggerNotification);
    b->channelName = this->username_;
    if (!loginName.isEmpty())
    {
        b->loginName = loginName;
        b->displayName = loginName;
        b->localizedName = loginName;
    }
    b->messageText = text;
    b->searchText = text;
    b->serverReceivedTime = QDateTime::currentDateTime();
    b.emplace<TimestampElement>(b->serverReceivedTime.toLocalTime().time());
    b.emplace<TextElement>(text, MessageElementFlag::Text, MessageColor::System);
    return b.release();
}

void TikTokLiveChat::handleLike(const tiktok::DecodedLikeEvent &ev)
{
    if (!this->impl_)
    {
        return;
    }
    const qint32 inc = std::max<qint32>(ev.count, 1);
    this->impl_->pendingLikeCount += inc;
    if (ev.total > this->impl_->pendingLikeTotal)
    {
        this->impl_->pendingLikeTotal = ev.total;
    }
    const QString name = !ev.user.nickname.isEmpty() ? ev.user.nickname
                                                     : ev.user.uniqueId;
    if (!name.isEmpty())
    {
        this->impl_->pendingLikeUser = name;
    }
    if (!this->impl_->likeTimer.isActive())
    {
        this->impl_->likeTimer.start();
    }
}

void TikTokLiveChat::handleMember(const tiktok::DecodedMemberEvent &ev)
{
    if (!this->impl_)
    {
        return;
    }
    // enter_type == 1 is "entered the room". Other action values carry
    // admin / block / kick semantics we don't route to activity for now.
    if (ev.enterType != 1 && ev.enterType != 0)
    {
        return;
    }
    this->impl_->pendingJoinCount++;
    const QString name = !ev.user.nickname.isEmpty() ? ev.user.nickname
                                                     : ev.user.uniqueId;
    if (!name.isEmpty())
    {
        this->impl_->pendingJoinUser = name;
    }
    if (!this->impl_->joinTimer.isActive())
    {
        this->impl_->joinTimer.start();
    }
}

void TikTokLiveChat::handleSocial(const tiktok::DecodedSocialEvent &ev)
{
    const QString name = !ev.user.nickname.isEmpty() ? ev.user.nickname
                                                     : ev.user.uniqueId;
    if (name.isEmpty())
    {
        return;
    }
    QString text;
    switch (ev.action)
    {
        case 1:
            text = QStringLiteral("%1 followed the streamer").arg(name);
            break;
        case 3:
            text = QStringLiteral("%1 shared the stream").arg(name);
            break;
        default:
            return;  // unknown social actions are dropped
    }
    this->messageReceived.invoke(this->buildActivityMessage(text, name));
}

void TikTokLiveChat::handleGift(const tiktok::DecodedGiftEvent &ev)
{
    // Suppress intermediate combo frames; only emit on the final frame.
    if (ev.repeatEnd != 1)
    {
        return;
    }
    const QString name = !ev.fromUser.nickname.isEmpty()
                             ? ev.fromUser.nickname
                             : ev.fromUser.uniqueId;
    if (name.isEmpty())
    {
        return;
    }
    const int count = std::max(1, static_cast<int>(ev.repeatCount));
    const QString text = count > 1
                             ? QStringLiteral("%1 sent a gift x%2")
                                   .arg(name)
                                   .arg(count)
                             : QStringLiteral("%1 sent a gift").arg(name);
    this->messageReceived.invoke(this->buildActivityMessage(text, name));
}

void TikTokLiveChat::flushPendingLikes()
{
    if (!this->impl_ || this->impl_->pendingLikeCount <= 0)
    {
        return;
    }
    const qint32 count = this->impl_->pendingLikeCount;
    const qint64 total = this->impl_->pendingLikeTotal;
    const QString user = this->impl_->pendingLikeUser;
    this->impl_->pendingLikeCount = 0;
    this->impl_->pendingLikeTotal = 0;
    this->impl_->pendingLikeUser.clear();

    QString text;
    if (count == 1 && !user.isEmpty())
    {
        text = QStringLiteral("%1 liked the stream").arg(user);
    }
    else if (total > 0)
    {
        text = QStringLiteral("%1 likes (%2 total)")
                   .arg(QString::number(count), QString::number(total));
    }
    else
    {
        text = QStringLiteral("%1 likes").arg(QString::number(count));
    }
    this->messageReceived.invoke(this->buildActivityMessage(text, user));
}

void TikTokLiveChat::flushPendingJoins()
{
    if (!this->impl_ || this->impl_->pendingJoinCount <= 0)
    {
        return;
    }
    const qint32 count = this->impl_->pendingJoinCount;
    const QString user = this->impl_->pendingJoinUser;
    this->impl_->pendingJoinCount = 0;
    this->impl_->pendingJoinUser.clear();

    QString text;
    if (count == 1 && !user.isEmpty())
    {
        text = QStringLiteral("%1 joined").arg(user);
    }
    else
    {
        text = QStringLiteral("%1 viewers joined").arg(QString::number(count));
    }
    this->messageReceived.invoke(this->buildActivityMessage(text, user));
}

}  // namespace chatterino
