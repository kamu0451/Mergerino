// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "providers/tiktok/TikTokLiveChat.hpp"

#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/tiktok/TikTokInjectScript.hpp"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QStandardPaths>

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
                                        if (!ok)
                                        {
                                            this->setStatusText(
                                                QStringLiteral(
                                                    "TikTok: navigation failed"),
                                                true);
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
                            // script runs. The completion handler is required
                            // by the interface even though we don't act on it.
                            this->impl_->webview
                                ->AddScriptToExecuteOnDocumentCreated(
                                    std::wstring(tiktok::kInjectScript).c_str(),
                                    Callback<
                                        ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                                        [](HRESULT, LPCWSTR) -> HRESULT {
                                            return S_OK;
                                        })
                                        .Get());

                            const QString url =
                                QStringLiteral("https://www.tiktok.com/@%1/live")
                                    .arg(this->username_);
                            this->impl_->webview->Navigate(
                                toWide(url).c_str());
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
    // ws-binary / ws-text: protobuf decode lives in a follow-up task.
    // The frame is deliberately dropped here - emitting placeholder chat
    // rows would be noise until real decoding is in place.
}

}  // namespace chatterino
