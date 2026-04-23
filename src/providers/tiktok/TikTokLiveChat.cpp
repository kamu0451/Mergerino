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

    // Watchdog: after ws-open we expect check_alive to report an alive_state
    // within ~30s. If it never arrives the room is almost certainly offline
    // or the user is not live; surface it instead of sitting on
    // "Connecting..." forever.
    QTimer stuckConnectionTimer;

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
                         this->setStatusText(
                             QStringLiteral("TikTok live chat unavailable "
                                            "(no response from room)"),
                             true);
                         this->setLive(false);
                     });
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
                                            if (guard.expired() ||
                                                FAILED(addHr))
                                            {
                                                return S_OK;
                                            }
                                            this->impl_->webview->Navigate(
                                                toWide(url).c_str());
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

    // Per-room entries: TikTok has used two shapes historically -
    //   older: { "alive_state": N }  where N == 2 means live, else ended
    //   newer: { "alive": true|false, "room_id_str": "..." }
    // Accept either. The WebSocket close event is unreliable on stream-
    // end, so this is the authoritative live/offline signal.
    if (!roomEntries.isEmpty())
    {
        bool anyRoomLive = false;
        bool sawAliveField = false;
        for (const auto &entry : roomEntries)
        {
            const auto obj = entry.toObject();
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
        if (sawAliveField)
        {
            // Room responded with a definitive live/offline state - cancel
            // the stuck-connection watchdog regardless of value.
            if (this->impl_)
            {
                this->impl_->stuckConnectionTimer.stop();
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
    // WebcastRoomUserSeqMessage frames carry the current online viewer
    // count; the HTTP room-info endpoints on the current TikTok page
    // (check_alive) no longer include it, so this is the authoritative
    // source for the tooltip's viewer line.
    if (frame.roomViewerCount > 0)
    {
        this->setViewerCount(static_cast<unsigned>(frame.roomViewerCount));
    }

    // Known follow-up: TikTok's current WebcastPushFrame schema sometimes
    // gzips the inner payload (envelope field 6 = "gzip"). We don't yet
    // decompress, so those frames produce no events. Uncompressed frames
    // (heartbeats, keepalive, occasional raw frames) still decode, which
    // is why chat / viewer-count works intermittently.

    const bool hasLiveContent =
        !frame.chatMessages.empty() || !frame.likeEvents.empty() ||
        !frame.memberEvents.empty() || !frame.socialEvents.empty() ||
        !frame.giftEvents.empty() || frame.roomViewerCount > 0;
    if (hasLiveContent)
    {
        // Fallback live signal when check_alive hasn't fired yet. Any real
        // chat/like/join frame is proof the room is live - cancel the
        // stuck-connection watchdog and clear a prior "unavailable" status
        // so the user sees the recovery.
        if (this->impl_)
        {
            this->impl_->stuckConnectionTimer.stop();
        }
        if (!this->live_)
        {
            this->setStatusText(QStringLiteral("Connected to TikTok"));
        }
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

MessagePtr TikTokLiveChat::buildActivityMessage(const QString &text,
                                                const QString &loginName,
                                                uint32_t diamondCount) const
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
    const qint64 totalDiamonds =
        static_cast<qint64>(std::max(0, ev.diamondCount)) * count;
    QString text;
    if (count > 1)
    {
        text = QStringLiteral("%1 sent a gift x%2").arg(name).arg(count);
    }
    else
    {
        text = QStringLiteral("%1 sent a gift").arg(name);
    }
    if (totalDiamonds > 0)
    {
        text += QStringLiteral(" (%1 diamonds)").arg(totalDiamonds);
    }
    const auto clampedDiamonds =
        static_cast<uint32_t>(std::min<qint64>(totalDiamonds, UINT32_MAX));
    this->messageReceived.invoke(
        this->buildActivityMessage(text, name, clampedDiamonds));
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
