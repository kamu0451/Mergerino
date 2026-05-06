// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "providers/tiktok/TikTokLiveChat.hpp"

#include "common/QLogging.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/tiktok/TikTokFrameDecoder.hpp"
#include "providers/tiktok/TikTokInjectScript.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

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

// One ICoreWebView2Environment is reusable across many controllers; sharing
// is also REQUIRED, because two concurrent CreateCoreWebView2EnvironmentWith-
// Options calls against the same userDataDir race and the loser's controller
// callback fires with E_ABORT (0x80004004). Observed when 3 TikTok tabs
// started together: 2 of 3 hit "controller failed".
//
// All access on the UI thread (TikTokLiveChat is a UI-thread object).
enum class EnvState
{
    NotStarted,
    Creating,
    Ready,
    Failed,
};
EnvState g_envState{EnvState::NotStarted};
ComPtr<ICoreWebView2Environment> g_env;
HRESULT g_envHr{S_OK};
std::vector<std::function<void(HRESULT, ICoreWebView2Environment *)>>
    g_envWaiters;

void requestSharedEnvironment(
    const std::wstring &userDataDirW,
    std::function<void(HRESULT, ICoreWebView2Environment *)> cb)
{
    switch (g_envState)
    {
        case EnvState::Ready:
            cb(S_OK, g_env.Get());
            return;
        case EnvState::Failed:
            cb(g_envHr, nullptr);
            return;
        case EnvState::Creating:
            g_envWaiters.push_back(std::move(cb));
            return;
        case EnvState::NotStarted:
            g_envState = EnvState::Creating;
            g_envWaiters.push_back(std::move(cb));
            const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
                nullptr, userDataDirW.c_str(), nullptr,
                Callback<
                    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                    [](HRESULT envHr,
                       ICoreWebView2Environment *env) -> HRESULT {
                        if (SUCCEEDED(envHr) && env != nullptr)
                        {
                            g_env = env;
                            g_envState = EnvState::Ready;
                        }
                        else
                        {
                            g_envHr = envHr;
                            g_envState = EnvState::Failed;
                        }
                        auto waiters = std::move(g_envWaiters);
                        g_envWaiters.clear();
                        for (auto &w : waiters)
                        {
                            w(envHr, env);
                        }
                        return S_OK;
                    })
                    .Get());
            if (FAILED(hr))
            {
                g_envHr = hr;
                g_envState = EnvState::Failed;
                auto waiters = std::move(g_envWaiters);
                g_envWaiters.clear();
                for (auto &w : waiters)
                {
                    w(hr, nullptr);
                }
            }
            return;
    }
}

// Concurrent CreateCoreWebView2Controller calls against the same environment
// race in WebView2's internals: with 3 controllers requested simultaneously
// (3 TikTok tabs at startup), 2 typically come back with E_ABORT. Serialize
// them: only one in-flight at a time, others queue and start when the prior
// finishes.
//
// All access on the UI thread.
struct PendingControllerRequest
{
    HWND host;
    std::function<void(HRESULT, ICoreWebView2Controller *)> callback;
};
bool g_controllerInFlight{false};
std::vector<PendingControllerRequest> g_controllerQueue;

void processNextController();

void requestController(
    HWND host,
    std::function<void(HRESULT, ICoreWebView2Controller *)> cb)
{
    g_controllerQueue.push_back({host, std::move(cb)});
    if (!g_controllerInFlight)
    {
        processNextController();
    }
}

void processNextController()
{
    if (g_controllerQueue.empty() || g_envState != EnvState::Ready ||
        !g_env)
    {
        return;
    }
    g_controllerInFlight = true;
    auto next = std::move(g_controllerQueue.front());
    g_controllerQueue.erase(g_controllerQueue.begin());

    auto cb = std::make_shared<
        std::function<void(HRESULT, ICoreWebView2Controller *)>>(
        std::move(next.callback));
    // WebView2's CreateCoreWebView2Controller occasionally fires its
    // completion handler twice (known SDK quirk - usually one good
    // invocation followed by a stale E_ABORT). Guard with a one-shot flag
    // so we don't double-process the same request.
    auto fired = std::make_shared<bool>(false);
    g_env->CreateCoreWebView2Controller(
        next.host,
        Callback<
            ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [cb, fired](HRESULT hr,
                        ICoreWebView2Controller *controller) -> HRESULT {
                if (*fired)
                {
                    return S_OK;
                }
                *fired = true;
                (*cb)(hr, controller);
                g_controllerInFlight = false;
                processNextController();
                return S_OK;
            })
            .Get());
}

}  // namespace

struct TikTokLiveChat::Impl {
    HWND host{};
    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    EventRegistrationToken navToken{};
    EventRegistrationToken msgToken{};
    EventRegistrationToken resReqToken{};
    // Number of times CreateCoreWebView2Controller has come back with
    // E_ABORT. WebView2 occasionally hands E_ABORT back even with our
    // serialized + shared-env path (transient internal race); a short retry
    // ladder works around it.
    int controllerRetryCount{0};
    static constexpr int controllerMaxRetries = 5;

    // Activity-event aggregation. TikTok likes / joins can burst at tens per
    // second; we accumulate and flush on a debounce timer.
    QTimer likeTimer;
    QTimer joinTimer;
    qint32 pendingLikeCount{0};
    qint64 pendingLikeTotal{0};
    QString pendingLikeUser;
    qint32 pendingJoinCount{0};
    QString pendingJoinUser;

    // Watchdog: after ws-open we expect check_alive to report an alive_state
    // within ~30s. If it never arrives the room is almost certainly offline
    // or the user is not live; surface it instead of sitting on
    // "Connecting..." forever.
    QTimer stuckConnectionTimer;

    // Liveness staleness watchdog. Once live_ flips true we expect a steady
    // stream of evidence: chat/gift/like/social/member frames, viewer-count
    // updates, or check_alive batches confirming our room. If everything
    // goes quiet for `livenessTimeoutMs`, the broadcast has almost
    // certainly ended even though TikTok never sent us a definitive
    // offline signal (no ws-close, no "room has finished" object-form
    // response, and check_alive batches stop including ended rooms
    // entirely). Without this backstop the tab can sit on "live" for
    // hours after the streamer has gone offline.
    QTimer livenessTimer;
    qint64 lastLiveEvidenceMs{0};
    static constexpr int livenessCheckIntervalMs = 30000;
    static constexpr qint64 livenessTimeoutMs = 180000;

    // Set once check_alive (or the inline `alive` field) has returned a
    // definitive live/offline answer for this session. While false, the
    // WebSocket fallback can flip live_ on chat/gift frames; once true,
    // the fallback only stops the watchdog -- the authoritative signal
    // wins. Without this gate, stale viewer-count or member-join frames
    // from a recently-ended room flip us back to live and re-fire the
    // "Joined TikTok live chat" announce.
    bool checkAliveSeen{false};

    // Login-mode auto-hide latch. When TIKTOK_LOGIN_MODE=1, the host
    // window is created visible so the user can sign in. Once chat
    // frames start flowing, we shrink it to 1x1, hide it, and flip the
    // controller invisible -- the UX becomes "show browser, log in,
    // browser disappears" with no manual close. Latch ensures the
    // transition runs once per Impl.
    bool loginVisible{false};
    bool loginAutoHidden{false};

    // Helper: shrink + hide the host once we've confirmed the page is past
    // the TikTok login wall. Idempotent.
    void autoHideLoginHost(const QString &username);

    // Dedicated single-thread pool for offloading protobuf decode off the UI
    // thread. Single-threaded on purpose: preserves frame ordering so chat
    // messages don't show up reordered under bursty traffic.
    QThreadPool decodePool;

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
            if (resReqToken.value != 0)
            {
                webview->remove_WebResourceRequested(resReqToken);
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
    : username_(TikTokLiveChat::normalizeSource(source))
    , lifetimeGuard_(std::make_shared<bool>(true))
{
}

TikTokLiveChat::~TikTokLiveChat()
{
    this->stop();
}

namespace {

// Process-wide registry. WebView2 hosts are expensive (each is a chromium
// renderer); MergedChannels with the same TikTok username share one. Keyed by
// the normalized username so different forms (`@user`, `user`, the full live
// URL) all collapse onto the same instance.
//
// Holds STRONG references on purpose: the layout-restore path creates and
// destroys throwaway MergedChannels in rapid succession (probing what each
// "merged" descriptor resolves to), and a weak_ptr-only registry would let
// the instance die between the throwaway MergedChannel dropping it and the
// real one picking it up. Three controller-create attempts per source result
// (each from a fresh Impl with a different host HWND), and the racy ones get
// E_ABORT from WebView2. Strong-ref keeps the instance alive across the
// churn; a unique TikTok source produces exactly one Impl for the app's
// lifetime.
std::mutex g_tiktokRegistryMutex;
std::unordered_map<QString, std::shared_ptr<TikTokLiveChat>> g_tiktokRegistry;

}  // namespace

std::shared_ptr<TikTokLiveChat> TikTokLiveChat::getOrCreateShared(
    const QString &source)
{
    const QString key = TikTokLiveChat::normalizeSource(source);
    std::lock_guard<std::mutex> lock(g_tiktokRegistryMutex);

    auto it = g_tiktokRegistry.find(key);
    if (it != g_tiktokRegistry.end())
    {
        return it->second;
    }

    auto shared = std::make_shared<TikTokLiveChat>(source);
    g_tiktokRegistry.emplace(key, shared);
    shared->start();
    return shared;
}

void TikTokLiveChat::Impl::autoHideLoginHost(const QString &username)
{
    if (!this->loginVisible || this->loginAutoHidden)
    {
        return;
    }
    this->loginAutoHidden = true;
    if (this->host != nullptr)
    {
        ShowWindow(this->host, SW_HIDE);
        SetWindowPos(this->host, nullptr, 0, 0, 1, 1,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (this->controller != nullptr)
    {
        this->controller->put_IsVisible(FALSE);
        RECT bounds{0, 0, 1, 1};
        this->controller->put_Bounds(bounds);
    }
    qCDebug(chatterinoTikTok).nospace()
        << "[" << username << "] login-mode auto-hide engaged";

    // First-tab-confirms-login broadcast: hide every other login-mode
    // host process-wide. Offline streamers / login-walled tabs never see
    // real chat or pin a roomId on their own, so without this they'd
    // sit visible forever even though the login is already done.
    std::vector<std::shared_ptr<TikTokLiveChat>> others;
    {
        std::lock_guard<std::mutex> lock(g_tiktokRegistryMutex);
        others.reserve(g_tiktokRegistry.size());
        for (const auto &[_, ref] : g_tiktokRegistry)
        {
            others.push_back(ref);
        }
    }
    for (const auto &other : others)
    {
        if (!other || other->impl_.get() == this)
        {
            continue;
        }
        if (!other->impl_->loginVisible || other->impl_->loginAutoHidden)
        {
            continue;
        }
        other->impl_->loginAutoHidden = true;
        if (other->impl_->host != nullptr)
        {
            ShowWindow(other->impl_->host, SW_HIDE);
            SetWindowPos(other->impl_->host, nullptr, 0, 0, 1, 1,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (other->impl_->controller != nullptr)
        {
            other->impl_->controller->put_IsVisible(FALSE);
            RECT bounds{0, 0, 1, 1};
            other->impl_->controller->put_Bounds(bounds);
        }
        qCDebug(chatterinoTikTok).nospace()
            << "[" << other->username_
            << "] login-mode auto-hide propagated";
    }
}

void TikTokLiveChat::releaseSharedEnvironment()
{
    // Application is shutting down. Drop the registry strong refs so each
    // TikTokLiveChat destructs (which calls Impl::~Impl, releasing the
    // controller/webview/env refs held by the Impl). Then drop our own
    // env ref and reject any pending env waiters. Doing this from
    // Application::aboutToQuit() means the Qt event loop is still alive
    // for the WebView2 message pump; releasing g_env at static-destructor
    // time (after main returns) is a known crash pattern on Windows.
    {
        std::lock_guard<std::mutex> lock(g_tiktokRegistryMutex);
        g_tiktokRegistry.clear();
    }

    // Drop pending controller requests; their per-Impl callbacks would
    // short-circuit on guard.expired() anyway since the Impls are gone.
    g_controllerQueue.clear();

    // Reject any callers still waiting on env creation so they don't sit
    // forever or fire after we've torn down.
    auto waiters = std::move(g_envWaiters);
    g_envWaiters.clear();
    for (auto &w : waiters)
    {
        w(E_ABORT, nullptr);
    }

    g_env.Reset();
    g_envState = EnvState::NotStarted;
    g_envHr = S_OK;
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
    qCDebug(chatterinoTikTok)
        << "start() username=" << this->username_;
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
    this->impl_->likeTimer.setSingleShot(true);
    this->impl_->likeTimer.setInterval(1000);
    this->impl_->joinTimer.setSingleShot(true);
    this->impl_->joinTimer.setInterval(1500);
    QObject::connect(&this->impl_->likeTimer, &QTimer::timeout,
                     [this]() { this->flushPendingLikes(); });
    QObject::connect(&this->impl_->joinTimer, &QTimer::timeout,
                     [this]() { this->flushPendingJoins(); });
    this->impl_->stuckConnectionTimer.setSingleShot(true);
    // 60s: TikTok's check_alive can legitimately take 30+ seconds on a
    // slow room or rate-limited endpoint, so the earlier 30s threshold
    // false-positived on real-but-slow live rooms.
    this->impl_->stuckConnectionTimer.setInterval(60000);
    this->impl_->decodePool.setMaxThreadCount(1);
    QObject::connect(&this->impl_->stuckConnectionTimer, &QTimer::timeout,
                     [this]() {
                         if (!this->running_)
                         {
                             return;
                         }
                         // If we never received a room/enter or room/info
                         // response, TikTok never opened a live room for
                         // this user - i.e. they aren't live. Otherwise we
                         // have a roomId_ but no chat/check_alive activity,
                         // which is the actual "stuck" case.
                         const QString msg =
                             this->roomId_.isEmpty()
                                 ? QStringLiteral("TikTok: %1 is not live")
                                       .arg(this->username_)
                                 : QStringLiteral(
                                       "TikTok live chat unavailable "
                                       "(no response from room)");
                         this->setStatusText(msg, true);
                         this->setLive(false);
                     });
    this->impl_->livenessTimer.setSingleShot(false);
    this->impl_->livenessTimer.setInterval(Impl::livenessCheckIntervalMs);
    QObject::connect(&this->impl_->livenessTimer, &QTimer::timeout, [this]() {
        if (!this->running_ || !this->live_ || !this->impl_)
        {
            return;
        }
        const auto now = QDateTime::currentMSecsSinceEpoch();
        const auto last = this->impl_->lastLiveEvidenceMs;
        if (last == 0 || now - last < Impl::livenessTimeoutMs)
        {
            return;
        }
        // No chat/gift/viewer-count frames, and no check_alive
        // confirmation, in livenessTimeoutMs. TikTok stops including
        // ended rooms in check_alive batches and often leaves the WS
        // open silently, so this is the only reliable backstop for
        // streams that end without a definitive offline signal.
        qCDebug(chatterinoTikTok)
            << "liveness watchdog: no evidence for"
            << (now - last) / 1000 << "s, marking offline";
        this->setStatusText(
            QStringLiteral("TikTok live chat ended (no activity)"), true);
        this->setLive(false);
    });
    // TIKTOK_LOGIN_MODE=1 in env: open a visible WebView so the user can
    // sign in to TikTok. Cookies persist in the user-data-dir, so a
    // single one-time login carries over to subsequent hidden runs.
    const bool loginMode =
        qEnvironmentVariableIsSet("TIKTOK_LOGIN_MODE") &&
        qEnvironmentVariable("TIKTOK_LOGIN_MODE") != QStringLiteral("0");
    if (loginMode)
    {
        this->impl_->host = CreateWindowExW(
            0, kWindowClass, L"mergerino-tiktok-host",
            WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (this->impl_->host != nullptr)
        {
            ShowWindow(this->impl_->host, SW_SHOW);
            this->impl_->loginVisible = true;
        }
    }
    else
    {
        this->impl_->host = CreateWindowExW(
            0, kWindowClass, L"mergerino-tiktok-host", WS_POPUP, 0, 0, 1, 1,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
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

    requestSharedEnvironment(
        userDataDirW,
        [this, guard](HRESULT envHr, ICoreWebView2Environment *env) {
            if (guard.expired())
            {
                return;
            }
            if (FAILED(envHr) || env == nullptr)
            {
                this->setStatusText(
                    QStringLiteral(
                        "TikTok: WebView2 Runtime unavailable (0x%1)")
                        .arg(static_cast<quint32>(envHr), 8, 16, QChar('0')),
                    true);
                this->running_ = false;
                return;
            }
            this->impl_->env = env;
            this->launchControllerCreate();
        });
}

void TikTokLiveChat::launchControllerCreate()
{
    if (!this->running_ || !this->impl_ || this->impl_->host == nullptr)
    {
        return;
    }
    std::weak_ptr<bool> guard = this->lifetimeGuard_;
    requestController(
        this->impl_->host,
        [this, guard](HRESULT ctrlHr,
                      ICoreWebView2Controller *controller) {
                            if (guard.expired())
                            {
                                return;
                            }
                            if (FAILED(ctrlHr) || controller == nullptr)
                            {
                                // E_ABORT (0x80004004) is a known transient
                                // race even with shared env + serialized
                                // creation. Retry a few times before giving
                                // up - usually succeeds on the second try.
                                // Also retry on ERROR_INVALID_WINDOW_HANDLE
                                // (0x80070578) which we've seen on first
                                // controller-create after env init.
                                const bool retryable =
                                    ctrlHr == E_ABORT ||
                                    static_cast<quint32>(ctrlHr) == 0x80070578;
                                if (retryable && this->impl_ &&
                                    this->impl_->controllerRetryCount <
                                        Impl::controllerMaxRetries)
                                {
                                    this->impl_->controllerRetryCount++;
                                    qCWarning(chatterinoTikTok).nospace()
                                        << "[" << this->username_
                                        << "] controller create E_ABORT, "
                                           "retry "
                                        << this->impl_->controllerRetryCount
                                        << "/" << Impl::controllerMaxRetries;
                                    QTimer::singleShot(
                                        250, [this, guard] {
                                            if (guard.expired())
                                            {
                                                return;
                                            }
                                            this->launchControllerCreate();
                                        });
                                    return;
                                }
                                // Don't surface as a chat system message -
                                // WebView2 sometimes fires this callback
                                // twice per request (the second invocation
                                // arrives with a stale E_ABORT, a known
                                // SDK quirk), so the user would see noise
                                // even when retry succeeds. Log to file
                                // for diagnostics; the channel will simply
                                // stay disconnected if all retries fail.
                                qCWarning(chatterinoTikTok).nospace()
                                    << "[" << this->username_
                                    << "] controller create failed hr=0x"
                                    << QString::number(
                                           static_cast<quint32>(ctrlHr), 16)
                                    << " after "
                                    << (this->impl_
                                            ? this->impl_->controllerRetryCount
                                            : 0)
                                    << " retries (suppressed from UI)";
                                this->running_ = false;
                                return;
                            }
                            this->impl_->controllerRetryCount = 0;
                            this->impl_->controller = controller;
                            controller->get_CoreWebView2(&this->impl_->webview);
                            if (!this->impl_->webview)
                            {
                                this->setStatusText(
                                    QStringLiteral("TikTok: no webview"), true);
                                this->running_ = false;
                                return;
                            }

                            // In login mode we want a normal visible WebView
                            // for the user to sign in. Otherwise, hidden 1x1
                            // host with image/media/font blocking to drop
                            // idle CPU from ~14% per tab to <1%.
                            const bool loginModeRuntime =
                                qEnvironmentVariableIsSet("TIKTOK_LOGIN_MODE") &&
                                qEnvironmentVariable("TIKTOK_LOGIN_MODE") !=
                                    QStringLiteral("0");
                            if (loginModeRuntime)
                            {
                                controller->put_IsVisible(TRUE);
                                RECT rb{0, 0, 1280, 800};
                                controller->put_Bounds(rb);
                            }
                            else
                            {
                                controller->put_IsVisible(FALSE);
                            }
                            ComPtr<ICoreWebView2_8> webview8;
                            if (SUCCEEDED(this->impl_->webview.As(&webview8)) &&
                                webview8)
                            {
                                webview8->put_IsMuted(TRUE);
                            }

                            // In login mode, don't block image/media/font so
                            // the login UI renders properly.
                            if (!loginModeRuntime)
                            {
                                this->impl_->webview->AddWebResourceRequestedFilter(
                                    L"*",
                                    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE);
                                this->impl_->webview->AddWebResourceRequestedFilter(
                                    L"*",
                                    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_MEDIA);
                                this->impl_->webview->AddWebResourceRequestedFilter(
                                    L"*",
                                    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_FONT);
                                this->impl_->webview->add_WebResourceRequested(
                                    Callback<
                                        ICoreWebView2WebResourceRequestedEventHandler>(
                                        [this, guard](
                                            ICoreWebView2 *,
                                            ICoreWebView2WebResourceRequestedEventArgs
                                                *args) -> HRESULT {
                                            if (guard.expired() || !this->impl_ ||
                                                !this->impl_->env)
                                            {
                                                return S_OK;
                                            }
                                            ComPtr<ICoreWebView2WebResourceResponse>
                                                resp;
                                            this->impl_->env
                                                ->CreateWebResourceResponse(
                                                    nullptr, 403, L"Blocked", L"",
                                                    &resp);
                                            if (resp)
                                            {
                                                args->put_Response(resp.Get());
                                            }
                                            return S_OK;
                                        })
                                        .Get(),
                                    &this->impl_->resReqToken);
                            }

                            // NavigationCompleted: detect page load state
                            this->impl_->webview->add_NavigationCompleted(
                                Callback<
                                    ICoreWebView2NavigationCompletedEventHandler>(
                                    [this, guard](
                                        ICoreWebView2 *wv,
                                        ICoreWebView2NavigationCompletedEventArgs
                                            *args) -> HRESULT {
                                        if (guard.expired())
                                        {
                                            return S_OK;
                                        }
                                        BOOL ok = FALSE;
                                        args->get_IsSuccess(&ok);
                                        COREWEBVIEW2_WEB_ERROR_STATUS errorStatus =
                                            COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                        args->get_WebErrorStatus(&errorStatus);
                                        LPWSTR src = nullptr;
                                        QString srcStr;
                                        if (wv != nullptr &&
                                            SUCCEEDED(wv->get_Source(&src)) &&
                                            src != nullptr)
                                        {
                                            srcStr = fromWide(src);
                                            CoTaskMemFree(src);
                                        }
                                        qCDebug(chatterinoTikTok).nospace()
                                            << "[" << this->username_
                                            << "] NavigationCompleted ok="
                                            << static_cast<bool>(ok)
                                            << " webErrorStatus="
                                            << static_cast<int>(errorStatus)
                                            << " source=" << srcStr;
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
                            // script runs. AddScriptToExecuteOnDocumentCreated
                            // is async: the script is only registered once
                            // the completion callback fires. If we call
                            // Navigate before then, the page's first document
                            // starts loading without our hook and TikTok's
                            // /@user/live SPA never navigates again, so we'd
                            // miss every WebSocket. Navigate from inside the
                            // completion handler instead.
                            // AddScriptToExecuteOnDocumentCreated is async;
                            // the script is only registered when its
                            // completion callback fires. Navigate from
                            // inside the callback so the WebSocket hook is
                            // guaranteed to be in place before TikTok's
                            // page JS opens its chat socket.
                            const QString url =
                                QStringLiteral("https://www.tiktok.com/@%1/live")
                                    .arg(this->username_);
                            this->impl_->webview
                                ->AddScriptToExecuteOnDocumentCreated(
                                    std::wstring(tiktok::kInjectScript).c_str(),
                                    Callback<
                                        ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                                        [this, guard, url](HRESULT addHr,
                                                           LPCWSTR) -> HRESULT {
                                            if (guard.expired())
                                            {
                                                return S_OK;
                                            }
                                            if (FAILED(addHr))
                                            {
                                                qCWarning(chatterinoTikTok).nospace()
                                                    << "[" << this->username_
                                                    << "] AddScriptToExecuteOnDocumentCreated"
                                                    << " failed hr=0x"
                                                    << QString::number(
                                                           static_cast<quint32>(
                                                               addHr),
                                                           16);
                                                return S_OK;
                                            }
                                            qCDebug(chatterinoTikTok).nospace()
                                                << "[" << this->username_
                                                << "] Navigate -> " << url;
                                            this->impl_->webview->Navigate(
                                                toWide(url).c_str());
                                            return S_OK;
                                        })
                                        .Get());
        });
}

void TikTokLiveChat::stop()
{
    this->running_ = false;
    this->setLive(false);
    this->impl_.reset();
    this->roomId_.clear();
    this->liveTitle_.clear();
    this->statusText_.clear();
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

unsigned TikTokLiveChat::viewerCount() const
{
    return this->viewerCount_;
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
    if (!live)
    {
        this->setViewerCount(0);
        if (this->impl_)
        {
            this->impl_->livenessTimer.stop();
            this->impl_->lastLiveEvidenceMs = 0;
        }
    }
    else if (this->impl_)
    {
        // Seed the evidence clock so the watchdog grace period starts
        // from the transition to live, not from the previous session.
        this->impl_->lastLiveEvidenceMs =
            QDateTime::currentMSecsSinceEpoch();
        this->impl_->livenessTimer.start();
    }
    this->liveStatusChanged.invoke();
}

void TikTokLiveChat::setViewerCount(unsigned count)
{
    if (this->viewerCount_ == count)
    {
        return;
    }
    this->viewerCount_ = count;
    this->viewerCountChanged.invoke();
}

void TikTokLiveChat::handleRoomInfo(const QJsonObject &root)
{
    // TikTok's /webcast/room/(enter|info|check_alive) responses arrive in
    // two top-level shapes:
    //   A) root.data is an OBJECT (enter / info endpoints, or login errors)
    //        { "data": { "user_count": N, "room": {..}, "message": "..." } }
    //      For this shape, room-state info may also appear as a nested
    //      data[] array: { "data": { "data": [ {alive_state / alive} ] } }
    //   B) root.data is an ARRAY directly (current check_alive endpoint)
    //        { "data": [ {"alive": true, "room_id_str": "..."} ] }
    // Normalise by extracting roomEntries (the array of per-room objects)
    // and dataObj (the object form, if any) up front.
    const auto rootDataValue = root.value(QStringLiteral("data"));
    QJsonObject dataObj;
    QJsonArray roomEntries;
    if (rootDataValue.isArray())
    {
        roomEntries = rootDataValue.toArray();
    }
    else
    {
        dataObj = rootDataValue.toObject();
        if (dataObj.isEmpty())
        {
            return;
        }
        roomEntries = dataObj.value(QStringLiteral("data")).toArray();
    }

    // Login wall: for some streamers (typically high-viewer ones) TikTok
    // refuses to serve room info to anonymous WebView2 sessions and
    // returns { data: { prompts: [..login UI..] } } with no room and no
    // id_str. The WebSocket still opens, but it only carries heartbeat
    // frames - no chat. Surface a clear "sign-in required" status instead
    // of falling back to "is not live" 60s later.
    if (this->roomId_.isEmpty() && roomEntries.isEmpty() &&
        dataObj.contains(QStringLiteral("prompts")) &&
        !dataObj.contains(QStringLiteral("room")) &&
        !dataObj.contains(QStringLiteral("id_str")))
    {
        if (this->impl_)
        {
            this->impl_->stuckConnectionTimer.stop();
        }
        this->setStatusText(
            QStringLiteral("TikTok: sign-in required to view %1")
                .arg(this->username_),
            true);
        this->setLive(false);
        return;
    }

    unsigned count = 0;
    const auto extract = [&count](const QJsonValue &v) {
        if (count > 0)
        {
            return;
        }
        const auto n = v.toDouble(-1);
        if (n >= 0)
        {
            count = static_cast<unsigned>(n);
        }
    };

    // Object form: webcast/room/enter / webcast/room/info variants.
    extract(dataObj.value(QStringLiteral("user_count")));
    extract(dataObj.value(QStringLiteral("total_user")));
    const auto room = dataObj.value(QStringLiteral("room")).toObject();
    if (!room.isEmpty())
    {
        extract(room.value(QStringLiteral("user_count")));
        extract(room.value(QStringLiteral("total_user")));
    }

    // TikTok's web page issues check_alive batches for OTHER rooms it's
    // recommending in the sidebar / "for you" feed - not just ours. If we
    // OR-fold alive across all entries, an unrelated live recommendation
    // can flip us to live and falsely fire "Joined TikTok live chat".
    // Filter by our own room_id_str. We pin it from the first batch that
    // includes a room_id_str if we don't already know it (TikTok queries
    // our room individually before the recommendation batches).
    //
    // Also detect TikTok's "this room has ended" notice (status_code 30003
    // or data.message == "room has finished") and lock live=false. Those
    // arrive on the OBJECT-form enter/info response and are the most
    // direct signal that gevad1ch's room is over.
    const auto statusCode = root.value(QStringLiteral("status_code")).toInt(0);
    const auto roomFinishedMsg =
        dataObj.value(QStringLiteral("message")).toString();
    if (statusCode == 30003 ||
        roomFinishedMsg.contains(QStringLiteral("room has finished"),
                                 Qt::CaseInsensitive) ||
        roomFinishedMsg.contains(QStringLiteral("LIVE has ended"),
                                 Qt::CaseInsensitive))
    {
        if (this->impl_)
        {
            this->impl_->stuckConnectionTimer.stop();
            this->impl_->checkAliveSeen = true;
        }
        this->setLive(false);
        // The object-form "finished" response carries no room data, no
        // viewer count, and no title - nothing else worth processing.
        return;
    }

    // Per-room entries: TikTok has used two shapes historically -
    //   older: { "alive_state": N }  where N == 2 means live, else ended
    //   newer: { "alive": true|false, "room_id_str": "..." }
    // Accept either. The WebSocket close event is unreliable on stream-
    // end, so this is the authoritative live/offline signal.
    if (!roomEntries.isEmpty())
    {
        bool anyRoomLive = false;
        bool sawAliveField = false;
        bool sawOurRoom = false;
        const bool haveOurRoomId = !this->roomId_.isEmpty();
        for (const auto &entry : roomEntries)
        {
            const auto obj = entry.toObject();
            const auto entryRoomId =
                obj.value(QStringLiteral("room_id_str")).toString();

            // Skip entries for OTHER rooms (TikTok recommendation batches).
            if (haveOurRoomId && !entryRoomId.isEmpty() &&
                entryRoomId != this->roomId_)
            {
                continue;
            }
            if (haveOurRoomId && entryRoomId == this->roomId_)
            {
                sawOurRoom = true;
            }

            extract(obj.value(QStringLiteral("user_count")));
            const auto aliveState = obj.value(QStringLiteral("alive_state"));
            if (aliveState.isDouble())
            {
                sawAliveField = true;
                if (aliveState.toInt() == 2)
                {
                    anyRoomLive = true;
                }
            }
            const auto aliveBool = obj.value(QStringLiteral("alive"));
            if (aliveBool.isBool())
            {
                sawAliveField = true;
                if (aliveBool.toBool())
                {
                    anyRoomLive = true;
                }
            }
        }

        // Only treat this batch as authoritative if our room was actually
        // in it. We deliberately do NOT pin roomId_ from a check_alive
        // batch: for offline users, TikTok never queries our (non-existent)
        // room individually, so the only batches that fire are sidebar
        // recommendations. Pinning the first entry as "ours" would let an
        // unrelated live recommendation flip us to live. roomId_ is
        // instead learned below from the object-form room/enter or
        // room/info responses, which are issued for our specific user
        // when the room is actually open.
        if (sawAliveField && haveOurRoomId && sawOurRoom)
        {
            if (this->impl_)
            {
                this->impl_->stuckConnectionTimer.stop();
                this->impl_->checkAliveSeen = true;
                if (anyRoomLive)
                {
                    // Authoritative live confirmation - resets the
                    // liveness staleness watchdog.
                    this->impl_->lastLiveEvidenceMs =
                        QDateTime::currentMSecsSinceEpoch();
                }
            }
            this->setLive(anyRoomLive);
        }
    }

    if (count > 0)
    {
        this->setViewerCount(count);
    }

    // liveTitle is displayed in tooltips; room title may also be here.
    if (this->liveTitle_.isEmpty())
    {
        QString title;
        if (!room.isEmpty())
        {
            title = room.value(QStringLiteral("title")).toString();
        }
        if (title.isEmpty())
        {
            title = dataObj.value(QStringLiteral("title")).toString();
        }
        if (!title.isEmpty())
        {
            this->liveTitle_ = title;
        }
    }

    // Some check_alive responses include room_id; cache if we haven't got it
    // via another path yet.
    if (this->roomId_.isEmpty())
    {
        const auto rid = dataObj.value(QStringLiteral("id_str")).toString();
        if (!rid.isEmpty())
        {
            this->roomId_ = rid;
        }
        else if (!room.isEmpty())
        {
            this->roomId_ = room.value(QStringLiteral("id_str")).toString();
        }
        if (!this->roomId_.isEmpty())
        {
            qCDebug(chatterinoTikTok).nospace()
                << "[" << this->username_ << "] pinned roomId from object-form="
                << this->roomId_;
            if (this->impl_)
            {
                this->impl_->autoHideLoginHost(this->username_);
            }
        }
        else
        {
            qCDebug(chatterinoTikTok).nospace()
                << "[" << this->username_
                << "] object-form had no usable id_str (data keys="
                << QStringList(dataObj.keys()).join(QStringLiteral(","))
                << " room keys="
                << QStringList(room.keys()).join(QStringLiteral(","))
                << ")";
        }
    }
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

    if (kind == QStringLiteral("ws-detect"))
    {
        qCDebug(chatterinoTikTok).nospace()
            << "[" << this->username_ << "] ws-detect url="
            << obj.value(QStringLiteral("url")).toString();
        return;
    }
    if (kind == QStringLiteral("http-detect"))
    {
        qCDebug(chatterinoTikTok).nospace()
            << "[" << this->username_ << "] http-detect url="
            << obj.value(QStringLiteral("url")).toString();
        return;
    }
    if (kind == QStringLiteral("xhr-detect"))
    {
        qCDebug(chatterinoTikTok).nospace()
            << "[" << this->username_ << "] xhr-detect "
            << obj.value(QStringLiteral("method")).toString() << " "
            << obj.value(QStringLiteral("url")).toString();
        return;
    }
    if (kind == QStringLiteral("sse-detect"))
    {
        qCDebug(chatterinoTikTok).nospace()
            << "[" << this->username_ << "] sse-detect url="
            << obj.value(QStringLiteral("url")).toString();
        return;
    }
    if (kind == QStringLiteral("event-feed-response") ||
        kind == QStringLiteral("generic-response") ||
        kind == QStringLiteral("xhr-response"))
    {
        qCDebug(chatterinoTikTok).nospace()
            << "[" << this->username_ << "] " << kind << " url="
            << obj.value(QStringLiteral("url")).toString().left(120)
            << " len="
            << obj.value(QStringLiteral("length")).toInt()
            << " head=" << obj.value(QStringLiteral("head")).toString();
        return;
    }
    if (kind == QStringLiteral("mcs-response"))
    {
        qCDebug(chatterinoTikTok).nospace()
            << "[" << this->username_ << "] mcs-response len="
            << obj.value(QStringLiteral("length")).toInt()
            << " head=" << obj.value(QStringLiteral("head")).toString();
        return;
    }
    if (kind == QStringLiteral("feed-response"))
    {
        qCDebug(chatterinoTikTok).nospace()
            << "[" << this->username_ << "] feed-response len="
            << obj.value(QStringLiteral("length")).toInt()
            << " head=" << obj.value(QStringLiteral("head")).toString();
        return;
    }
    if (kind == QStringLiteral("worker-detect"))
    {
        qCDebug(chatterinoTikTok).nospace()
            << "[" << this->username_ << "] worker-detect url="
            << obj.value(QStringLiteral("url")).toString();
        return;
    }
    if (kind == QStringLiteral("shared-worker-detect"))
    {
        qCDebug(chatterinoTikTok).nospace()
            << "[" << this->username_ << "] shared-worker-detect url="
            << obj.value(QStringLiteral("url")).toString();
        return;
    }
    if (kind == QStringLiteral("service-worker-status"))
    {
        qCDebug(chatterinoTikTok).nospace()
            << "[" << this->username_
            << "] service-worker-status hasController="
            << obj.value(QStringLiteral("hasController")).toBool()
            << " url="
            << obj.value(QStringLiteral("controllerUrl")).toString();
        return;
    }
    if (kind == QStringLiteral("ws-open"))
    {
        // A WebSocket opening tells us nothing about whether this user is
        // actually live - TikTok opens im-ws sockets on offline profile
        // pages too. Wait for alive_state == 2 from check_alive or an
        // actual decoded chat event before treating the room as live.
        qCDebug(chatterinoTikTok) << "ws-open for" << this->username_;
        this->setStatusText(QStringLiteral("Connected to TikTok"));
        if (this->impl_)
        {
            this->impl_->stuckConnectionTimer.start();
        }
        return;
    }
    if (kind == QStringLiteral("room-info"))
    {
        const auto data = obj.value(QStringLiteral("data")).toObject();
        qCDebug(chatterinoTikTok).nospace()
            << "room-info: "
            << QJsonDocument(data).toJson(QJsonDocument::Compact);
        this->handleRoomInfo(data);
        return;
    }
    if (kind == QStringLiteral("ws-close"))
    {
        qCDebug(chatterinoTikTok)
            << "ws-close code=" << obj.value(QStringLiteral("code")).toInt();
        if (this->impl_)
        {
            this->impl_->stuckConnectionTimer.stop();
        }
        this->setLive(false);
        this->setStatusText(QStringLiteral("TikTok live chat disconnected"),
                            true);
        return;
    }
    if (kind == QStringLiteral("ws-error"))
    {
        qCDebug(chatterinoTikTok) << "ws-error";
        if (this->impl_)
        {
            this->impl_->stuckConnectionTimer.stop();
        }
        this->setStatusText(QStringLiteral("TikTok live chat error"), true);
        return;
    }
    if (kind == QStringLiteral("dom-chat"))
    {
        // DOM-scraped chat path. The webcast WebSocket only sends
        // heartbeat/state-poll frames to our hidden host (verified via
        // 1680-byte uniform-size dumps with no chat methods), so the
        // actual user-visible chat is read from the rendered DOM by
        // a MutationObserver in TikTokInjectScript.hpp.
        const QString user = obj.value(QStringLiteral("user")).toString();
        const QString text = obj.value(QStringLiteral("text")).toString();
        if (user.isEmpty() || text.isEmpty())
        {
            return;
        }
        tiktok::DecodedChatMessage chat;
        chat.user.nickname = user;
        chat.user.uniqueId = user;
        chat.content = text;
        if (this->impl_)
        {
            this->impl_->stuckConnectionTimer.stop();
            if (this->live_)
            {
                this->impl_->lastLiveEvidenceMs =
                    QDateTime::currentMSecsSinceEpoch();
            }
        }
        if (this->impl_ && !this->impl_->checkAliveSeen && !this->live_ &&
            !this->roomId_.isEmpty())
        {
            this->setStatusText(QStringLiteral("Connected to TikTok"));
            this->setLive(true);
        }
        this->messageReceived.invoke(this->buildChatMessage(chat));
        return;
    }
    if (kind == QStringLiteral("dom-chat-attached"))
    {
        qCDebug(chatterinoTikTok).nospace()
            << "[" << this->username_
            << "] dom-chat observer attached, existing rows="
            << obj.value(QStringLiteral("count")).toInt();
        return;
    }
    if (kind == QStringLiteral("dom-snapshot"))
    {
        const auto e2e = obj.value(QStringLiteral("e2eValues")).toArray();
        QStringList e2eNames;
        e2eNames.reserve(e2e.size());
        for (const auto &v : e2e)
        {
            e2eNames.append(v.toString());
        }
        qCDebug(chatterinoTikTok).nospace()
            << "[" << this->username_ << "] dom-snapshot t="
            << obj.value(QStringLiteral("tick")).toInt()
            << " elements="
            << obj.value(QStringLiteral("elementCount")).toInt()
            << " focus=" << obj.value(QStringLiteral("hasFocus")).toBool()
            << " containerKids="
            << obj.value(QStringLiteral("containerChildren")).toInt()
            << " containerDesc="
            << obj.value(QStringLiteral("containerDescendants")).toInt()
            << " e2e={" << e2eNames.join(QStringLiteral(",")) << "}"
            << " containerHTML="
            << obj.value(QStringLiteral("containerHtml")).toString();
        return;
    }
    if (kind == QStringLiteral("dom-snapshot-error"))
    {
        qCWarning(chatterinoTikTok).nospace()
            << "[" << this->username_ << "] dom-snapshot-error: "
            << obj.value(QStringLiteral("message")).toString();
        return;
    }
    if (kind == QStringLiteral("dom-survey"))
    {
        const auto arr = obj.value(QStringLiteral("candidates")).toArray();
        QStringList names;
        names.reserve(arr.size());
        for (const auto &v : arr)
        {
            names.append(v.toString());
        }
        qCDebug(chatterinoTikTok).nospace()
            << "[" << this->username_
            << "] dom-survey: " << names.join(QStringLiteral(" | "));
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
        // Decode inline on the UI thread. Earlier versions offloaded this
        // to a QtConcurrent worker; that turned out to break message
        // delivery on some rooms (watchdog would time out even though
        // frames were flowing). Revert for correctness; if busy-stream
        // perf becomes an issue again we can revisit with a different
        // marshal mechanism.
        this->processDecodedFrame(tiktok::decodeWebcastPushFrame(raw));
    }
}

void TikTokLiveChat::processDecodedFrame(const tiktok::DecodedFrame &frame)
{
    if (!frame.fetchResultFields.empty() || !frame.allMethods.empty())
    {
        QStringList frFields;
        frFields.reserve(static_cast<int>(frame.fetchResultFields.size()));
        for (const auto &s : frame.fetchResultFields)
        {
            frFields.append(s);
        }
        QStringList methods;
        methods.reserve(static_cast<int>(frame.allMethods.size()));
        for (const auto &s : frame.allMethods)
        {
            methods.append(s);
        }
        qCDebug(chatterinoTikTok).nospace()
            << "[" << this->username_
            << "] frame inflated=" << frame.inflatedSize
            << " fetchResultFields=[" << frFields.join(QStringLiteral(","))
            << "] allMethods=[" << methods.join(QStringLiteral(",")) << "]";
    }
    // WebcastRoomUserSeqMessage frames carry the current online viewer
    // count; the HTTP room-info endpoints on the current TikTok page
    // (check_alive) no longer include it, so this is the authoritative
    // source for the tooltip's viewer line.
    if (frame.roomViewerCount > 0)
    {
        this->setViewerCount(static_cast<unsigned>(frame.roomViewerCount));
    }

    // Only chat messages and gift events are unambiguous proof the room is
    // actively broadcasting. Likes can be replayed from cache; member-join /
    // social events and roomViewerCount > 0 still arrive on rooms that just
    // ended, which previously false-positived the room as live.
    const bool hasLiveContent =
        !frame.chatMessages.empty() || !frame.giftEvents.empty();
    if (hasLiveContent && this->impl_)
    {
        this->impl_->stuckConnectionTimer.stop();
    }

    // Login-mode auto-hide: any frame with a real method (chat, gift,
    // member, social, like, room-user-seq, ...) means we're past the
    // login wall and the visible browser has done its job.
    bool sawRealMethod = false;
    for (const auto &m : frame.allMethods)
    {
        if (!m.isEmpty() && m != QStringLiteral("<empty>"))
        {
            sawRealMethod = true;
            break;
        }
    }
    if (sawRealMethod && this->impl_)
    {
        this->impl_->autoHideLoginHost(this->username_);
    }
    // Liveness watchdog evidence. Only count signals that are unambiguous
    // proof of an active broadcast: chat and gift frames. Viewer-count,
    // member-join, social, and like frames continue to flow (or get
    // replayed) for a while after a stream ends, so feeding them in here
    // would defeat the watchdog.
    if (hasLiveContent && this->impl_ && this->live_)
    {
        this->impl_->lastLiveEvidenceMs =
            QDateTime::currentMSecsSinceEpoch();
    }
    if (hasLiveContent && this->impl_ && !this->impl_->checkAliveSeen &&
        !this->live_ && !this->roomId_.isEmpty())
    {
        // Fallback live signal: only used before check_alive has spoken.
        // Once check_alive has reported a definitive value the fallback
        // is suppressed -- otherwise stale frames after a stream end
        // re-flip live_ and re-fire the join announce.
        //
        // Require a confirmed roomId_: our injected JS hook captures every
        // WebSocket the TikTok page opens, including ones pointed at sidebar
        // recommendations on an offline user's /@user/live page. Without a
        // roomId_ pinned by the object-form room/enter or room/info
        // response (which TikTok only issues for our user when actually
        // live), chat frames here may belong to a recommended room and
        // would falsely fire "Joined TikTok live chat".
        this->setStatusText(QStringLiteral("Connected to TikTok"));
        this->setLive(true);
    }
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

MessagePtr TikTokLiveChat::buildActivityMessage(
    const QString &text, const QString &loginName,
    Message::TikTokActivityKind kind, uint32_t diamondCount) const
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
    b->tiktokGiftDiamondCount = diamondCount;
    b->tiktokActivityKind = kind;
    b.emplace<TimestampElement>(b->serverReceivedTime.toLocalTime().time());
    b.emplace<TextElement>(text, MessageElementFlag::Text, MessageColor::System);
    return b.release();
}

namespace {

// TikTok lets people set nicknames to invisible-only characters (zero-width
// joiners, variation selectors, lone combining marks). isEmpty() returns
// false for those, which leaves "" in the activity log. Fall back to the
// uniqueId handle when the nickname has no character that would render.
QString pickDisplayName(const tiktok::DecodedUser &user)
{
    const auto hasRenderable = [](const QString &s) {
        for (const QChar c : s)
        {
            if (c.isLetterOrNumber() || c.isPunct() || c.isSymbol())
            {
                return true;
            }
        }
        return false;
    };
    if (!user.nickname.isEmpty() && hasRenderable(user.nickname))
    {
        return user.nickname;
    }
    if (!user.uniqueId.isEmpty())
    {
        return user.uniqueId;
    }
    return user.nickname;  // last-resort, may still be invisible
}

}  // namespace

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
    const QString name = pickDisplayName(ev.user);
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
    const QString name = pickDisplayName(ev.user);
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
    const QString name = pickDisplayName(ev.user);
    if (name.isEmpty())
    {
        return;
    }
    QString text;
    auto kind = Message::TikTokActivityKind::None;
    switch (ev.action)
    {
        case 1:
            text = QStringLiteral("%1 followed the streamer").arg(name);
            kind = Message::TikTokActivityKind::Follow;
            break;
        case 3:
            text = QStringLiteral("%1 shared the stream").arg(name);
            kind = Message::TikTokActivityKind::Share;
            break;
        default:
            return;  // unknown social actions are dropped
    }
    this->messageReceived.invoke(this->buildActivityMessage(text, name, kind));
}

void TikTokLiveChat::handleGift(const tiktok::DecodedGiftEvent &ev)
{
    // Suppress intermediate combo frames; only emit on the final frame.
    if (ev.repeatEnd != 1)
    {
        return;
    }
    const QString name = pickDisplayName(ev.fromUser);
    if (name.isEmpty())
    {
        return;
    }
    const int count = std::max(1, static_cast<int>(ev.repeatCount));
    const qint64 totalCoins =
        static_cast<qint64>(std::max(0, ev.coinValue)) * count;
    const QString gift =
        ev.giftName.isEmpty() ? QStringLiteral("a gift") : ev.giftName;
    QString text;
    if (count > 1)
    {
        text = QStringLiteral("%1 sent %2 x%3").arg(name, gift).arg(count);
    }
    else
    {
        text = QStringLiteral("%1 sent %2").arg(name, gift);
    }
    if (totalCoins > 0)
    {
        const QString unit = totalCoins == 1 ? QStringLiteral("coin")
                                             : QStringLiteral("coins");
        text += QStringLiteral(" (%1 %2)").arg(totalCoins).arg(unit);
    }
    const auto clampedCoins =
        static_cast<uint32_t>(std::min<qint64>(totalCoins, UINT32_MAX));
    this->messageReceived.invoke(this->buildActivityMessage(
        text, name, Message::TikTokActivityKind::Gift, clampedCoins));
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
    this->messageReceived.invoke(this->buildActivityMessage(
        text, user, Message::TikTokActivityKind::Like));
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
    this->messageReceived.invoke(this->buildActivityMessage(
        text, user, Message::TikTokActivityKind::Join));
}

}  // namespace chatterino
