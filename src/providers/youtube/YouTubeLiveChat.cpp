// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeLiveChat.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <optional>
#include <vector>

namespace {

using namespace chatterino;

QString trimmedSource(QString value)
{
    return value.trimmed();
}

QString trimTrailingSlash(QString value)
{
    while (value.size() > 1 && value.endsWith('/'))
    {
        value.chop(1);
    }

    return value;
}

QJsonObject makeYoutubeClientContext(const QString &clientVersion)
{
    return {
        {"clientName", "WEB"},
        {"clientVersion", clientVersion},
        {"hl", "en"},
        {"gl", "US"},
    };
}

const QString YOUTUBE_BOOTSTRAP_URL = "https://www.youtube.com/embed/jNQXAC9IVRw";
const QColor YOUTUBE_PLATFORM_ACCENT(255, 48, 64, 60);
constexpr int YOUTUBE_RECONNECT_DELAY_MS = 3000;

std::vector<std::pair<QByteArray, QByteArray>> youtubeHeaders()
{
    return {
        {"User-Agent",
         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
         "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"},
        {"Accept-Language", "en-US,en;q=0.9"},
        {"Cookie", "CONSENT=YES+cb.20210328-17-p0.en+FX+111; SOCS=CAI"},
        {"Origin", "https://www.youtube.com"},
        {"Referer", "https://www.youtube.com/"},
    };
}

void appendSimpleWords(MessageBuilder &builder, const QString &text)
{
    for (auto word : QStringView(text).tokenize(u' ', Qt::SkipEmptyParts))
    {
        builder.addWordFromUserMessage(word);
    }
}

MessagePtr makeSystemStatusMessage(
    const QString &text,
    const std::optional<QColor> &platformAccentColor = std::nullopt)
{
    MessageBuilder builder;
    builder->flags.set(MessageFlag::System);
    builder->flags.set(MessageFlag::DoNotTriggerNotification);
    builder->messageText = text;
    builder->searchText = text;
    builder->serverReceivedTime = QDateTime::currentDateTime();
    builder.emplace<TimestampElement>(builder->serverReceivedTime.time());
    builder.emplace<TextElement>(text, MessageElementFlag::Text,
                                 MessageColor::System);
    builder->platformAccentColor = platformAccentColor;
    return builder.release();
}

QDateTime parseTimestampUsec(const QString &timestampUsec)
{
    bool ok = false;
    const auto usec = timestampUsec.toLongLong(&ok);
    if (!ok)
    {
        return QDateTime::currentDateTime();
    }

    return QDateTime::fromMSecsSinceEpoch(usec / 1000, Qt::UTC)
        .toLocalTime();
}

int cappedYouTubePollDelay(int timeoutMs)
{
    constexpr int minimumDelayMs = 1000;
    constexpr int maximumDelayMs = 1500;

    if (timeoutMs <= 0)
    {
        return minimumDelayMs;
    }

    return std::clamp(timeoutMs, minimumDelayMs, maximumDelayMs);
}

}  // namespace

namespace chatterino {

YouTubeLiveChat::YouTubeLiveChat(QString streamUrl)
    : streamUrl_(std::move(streamUrl))
    , lifetimeGuard_(std::make_shared<bool>(true))
{
}

YouTubeLiveChat::~YouTubeLiveChat()
{
    this->stop();
}

void YouTubeLiveChat::start()
{
    if (this->running_)
    {
        return;
    }

    if (!this->lifetimeGuard_)
    {
        this->lifetimeGuard_ = std::make_shared<bool>(true);
    }

    this->running_ = true;
    this->videoId_.clear();
    this->apiKey_.clear();
    this->clientVersion_.clear();
    this->visitorData_.clear();
    this->continuation_.clear();
    this->statusText_.clear();
    this->liveTitle_.clear();
    this->seenMessageIds_.clear();
    this->joinedLiveVideoId_.clear();
    this->failureReported_ = false;
    this->skipInitialBacklog_ = false;
    this->immediateSourceProbePending_ = this->shouldResolveLiveStreamFromSource();
    this->setLive(false);
    this->resolveVideoId();
}

void YouTubeLiveChat::stop()
{
    this->running_ = false;
    this->lifetimeGuard_.reset();
}

bool YouTubeLiveChat::isLive() const
{
    return this->live_;
}

const QString &YouTubeLiveChat::videoId() const
{
    return this->videoId_;
}

const QString &YouTubeLiveChat::statusText() const
{
    return this->statusText_;
}

const QString &YouTubeLiveChat::liveTitle() const
{
    return this->liveTitle_;
}

void YouTubeLiveChat::resolveVideoId()
{
    if (!this->running_)
    {
        return;
    }

    this->videoId_ = maybeExtractVideoId(this->streamUrl_);
    if (!this->videoId_.isEmpty())
    {
        this->resolveChannelIdFromVideoId(
            this->videoId_, [this] {
                if (!this->running_)
                {
                    return;
                }

                this->seenMessageIds_.clear();
                this->fetchLiveChatPage();
            });
        return;
    }

    const auto source = this->resolvedSource();
    if (source.isEmpty())
    {
        this->setStatusText(
            "Enter a YouTube @handle or any video link from the desired channel.",
                            !this->failureReported_);
        this->failureReported_ = true;
        return;
    }

    if (this->immediateSourceProbePending_)
    {
        this->immediateSourceProbePending_ = false;
        this->probeLiveVideoIdFromSource(source, [this, source](QString videoId) {
            if (!this->running_)
            {
                return;
            }

            if (!videoId.isEmpty())
            {
                this->videoId_ = std::move(videoId);
                this->seenMessageIds_.clear();
                this->fetchLiveChatPage();
                return;
            }

            this->bootstrapInnertubeContext(
                [this, source] { this->resolveSourceToVideoId(source); },
                "Couldn't initialize YouTube live chat.");
        });
        return;
    }

    this->bootstrapInnertubeContext(
        [this, source] { this->resolveSourceToVideoId(source); },
        "Couldn't initialize YouTube live chat.");
}

void YouTubeLiveChat::resolveChannelIdFromVideoId(
    const QString &videoId, std::function<void()> onResolved)
{
    if (!this->running_ || videoId.isEmpty())
    {
        onResolved();
        return;
    }

    auto callback = std::make_shared<std::function<void()>>(std::move(onResolved));
    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    NetworkRequest(
        QString("https://www.youtube.com/watch?v=%1").arg(videoId).toStdString())
        .headerList(youtubeHeaders())
        .followRedirects(true)
        .onSuccess([this, weak, callback](const NetworkResult &result) {
                if (!weak.lock() || !this->running_)
                {
                    return;
                }

                const auto html = QString::fromUtf8(result.getData());
                const auto channelId = extractVideoChannelId(html);
                if (!channelId.isEmpty() && this->streamUrl_ != channelId)
                {
                    this->streamUrl_ = channelId;
                    this->sourceResolved.invoke(channelId);
                }

                (*callback)();
            })
        .onError([this, weak, callback](NetworkResult) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            (*callback)();
        })
        .execute();
}

void YouTubeLiveChat::probeLiveVideoIdFromSource(
    const QString &source, std::function<void(QString)> onResolved)
{
    if (!this->running_ || source.isEmpty())
    {
        onResolved({});
        return;
    }

    const auto livePath = sourceLivePath(source);
    if (livePath.isEmpty())
    {
        onResolved({});
        return;
    }

    const auto liveUrl = QString("https://www.youtube.com%1").arg(livePath);
    auto callback =
        std::make_shared<std::function<void(QString)>>(std::move(onResolved));
    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    NetworkRequest(liveUrl.toStdString())
        .headerList(youtubeHeaders())
        .followRedirects(true)
        .onSuccess([this, weak, callback](const NetworkResult &result) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            const auto html = QString::fromUtf8(result.getData());
            (*callback)(extractLiveVideoId(html));
        })
        .onError([this, weak, callback](NetworkResult) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            (*callback)({});
        })
        .execute();
}

void YouTubeLiveChat::bootstrapInnertubeContext(std::function<void()> onReady,
                                                QString failureText)
{
    if (!this->running_)
    {
        return;
    }

    if (!this->apiKey_.isEmpty() && !this->clientVersion_.isEmpty() &&
        !this->visitorData_.isEmpty())
    {
        onReady();
        return;
    }

    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    NetworkRequest(YOUTUBE_BOOTSTRAP_URL.toStdString())
        .headerList(youtubeHeaders())
        .followRedirects(true)
        .onSuccess([this, weak, onReady = std::move(onReady), failureText](
                       const NetworkResult &result) mutable {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            const auto html = QString::fromUtf8(result.getData());
            if (html.contains("Before you continue"))
            {
                this->setStatusText(
                    "YouTube returned a consent gate while loading chat.",
                    !this->failureReported_);
                this->failureReported_ = true;
                return;
            }
            if (html.contains("unusual traffic", Qt::CaseInsensitive))
            {
                this->setStatusText(
                    "YouTube blocked the live chat request from this network.",
                    !this->failureReported_);
                this->failureReported_ = true;
                return;
            }

            this->apiKey_ = extractFirstMatch(
                html, {R"yt("INNERTUBE_API_KEY":"([^"]+)")yt",
                       R"yt("innertubeApiKey":"([^"]+)")yt"});
            this->clientVersion_ = extractFirstMatch(
                html, {R"yt("INNERTUBE_CLIENT_VERSION":"([^"]+)")yt",
                       R"yt("clientVersion":"([^"]+)")yt"});
            this->visitorData_ = extractFirstMatch(
                html, {R"yt("VISITOR_DATA":"([^"]+)")yt",
                       R"yt("visitorData":"([^"]+)")yt"});

            if (this->apiKey_.isEmpty() || this->clientVersion_.isEmpty() ||
                this->visitorData_.isEmpty())
            {
                this->setStatusText(failureText, !this->failureReported_);
                this->failureReported_ = true;
                this->scheduleResolve(YOUTUBE_RECONNECT_DELAY_MS);
                return;
            }

            this->failureReported_ = false;
            onReady();
        })
        .onError([this, weak, failureText = std::move(failureText)](
                     NetworkResult) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            this->setStatusText(failureText, !this->failureReported_);
            this->failureReported_ = true;
            this->scheduleResolve(YOUTUBE_RECONNECT_DELAY_MS);
        })
        .execute();
}

void YouTubeLiveChat::resolveSourceToVideoId(const QString &source)
{
    if (!this->running_ || source.isEmpty())
    {
        return;
    }

    const auto url = QString(
                         "https://www.youtube.com/youtubei/v1/navigation/"
                         "resolve_url?prettyPrint=false&key=%1")
                         .arg(this->apiKey_);
    const auto livePath = sourceLivePath(source);
    if (livePath.isEmpty())
    {
        this->setStatusText(
            "Couldn't resolve that YouTube channel. Try one of the "
            "channel's video links so Mergerino can resolve the channel ID "
            "automatically.",
            !this->failureReported_);
        this->failureReported_ = true;
        return;
    }
    const auto liveUrl = QString("https://www.youtube.com%1").arg(livePath);

    QJsonObject root{
        {"context",
         QJsonObject{{"client", makeYoutubeClientContext(this->clientVersion_)}}},
        {"url", liveUrl},
    };

    auto request = NetworkRequest(url.toStdString())
                       .type(NetworkRequestType::Post)
                       .headerList(youtubeHeaders())
                       .json(root)
                       .header("Referer", liveUrl);
    if (!this->visitorData_.isEmpty())
    {
        request = std::move(request).header("X-Goog-Visitor-Id",
                                            this->visitorData_);
    }

    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    std::move(request)
        .onSuccess([this, weak, source](const NetworkResult &result) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            const auto json = result.parseJson();
            const auto endpoint = json["endpoint"].toObject();
            const auto watchEndpoint = endpoint["watchEndpoint"].toObject();
            const auto videoId = watchEndpoint["videoId"].toString().trimmed();
            if (!videoId.isEmpty())
            {
                this->videoId_ = videoId;
                this->seenMessageIds_.clear();
                this->fetchLiveChatPage();
                return;
            }

            if (!endpoint["browseEndpoint"].toObject().isEmpty())
            {
                this->waitForNextLive(
                    QString("Waiting for %1 to go live on YouTube.")
                        .arg(source),
                    YOUTUBE_RECONNECT_DELAY_MS);
                return;
            }

            this->setStatusText(
                "Couldn't resolve that YouTube channel. Try one of the "
                "channel's video links so Mergerino can resolve the channel "
                "ID automatically.",
                !this->failureReported_);
            this->failureReported_ = true;
        })
        .onError([this, weak](NetworkResult) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            this->setStatusText(
                "Couldn't resolve that YouTube channel. Try one of the "
                "channel's video links so Mergerino can resolve the channel "
                "ID automatically.",
                !this->failureReported_);
            this->failureReported_ = true;
            this->scheduleResolve(YOUTUBE_RECONNECT_DELAY_MS);
        })
        .execute();
}

void YouTubeLiveChat::fetchLiveChatPage()
{
    if (!this->running_ || this->videoId_.isEmpty())
    {
        return;
    }

    if (this->apiKey_.isEmpty() || this->clientVersion_.isEmpty())
    {
        this->bootstrapInnertubeContext(
            [this] { this->fetchLiveChatPage(); },
            "Couldn't initialize YouTube live chat.");
        return;
    }

    const auto url =
        QString("https://www.youtube.com/youtubei/v1/next?prettyPrint=false&key=%1")
            .arg(this->apiKey_);
    QJsonObject root{
        {"context",
         QJsonObject{{"client", makeYoutubeClientContext(this->clientVersion_)}}},
        {"videoId", this->videoId_},
    };

    auto request = NetworkRequest(url.toStdString())
                       .type(NetworkRequestType::Post)
                       .headerList(youtubeHeaders())
                       .json(root)
                       .header("Referer",
                               QString("https://www.youtube.com/watch?v=%1")
                                   .arg(this->videoId_));
    if (!this->visitorData_.isEmpty())
    {
        request = std::move(request).header("X-Goog-Visitor-Id",
                                            this->visitorData_);
    }

    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    std::move(request)
        .onSuccess([this, weak](const NetworkResult &result) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            const auto json = result.parseJson();
            const auto liveChatRenderer =
                json["contents"]
                    .toObject()["twoColumnWatchNextResults"]
                    .toObject()["conversationBar"]
                    .toObject()["liveChatRenderer"]
                    .toObject();
            this->continuation_ = extractLiveChatContinuation(liveChatRenderer);
            if (this->continuation_.isEmpty())
            {
                if (this->shouldResolveLiveStreamFromSource())
                {
                    this->waitForNextLive("Waiting for a live YouTube stream.",
                                          YOUTUBE_RECONNECT_DELAY_MS);
                }
                else
                {
                    this->setStatusText(
                        "Couldn't find the YouTube live chat continuation "
                        "data.",
                        !this->failureReported_);
                    this->failureReported_ = true;
                    this->scheduleResolve(YOUTUBE_RECONNECT_DELAY_MS);
                }
                return;
            }

            this->liveTitle_ = extractLiveStreamTitle(json);
            this->failureReported_ = false;
            if (this->joinedLiveVideoId_ != this->videoId_)
            {
                this->joinedLiveVideoId_ = this->videoId_;
                auto joinedText = this->liveTitle_.isEmpty()
                                      ? QString("Joined YouTube live chat")
                                      : QString("Joined YouTube live chat: %1")
                                            .arg(this->liveTitle_);
                this->systemMessageReceived.invoke(
                    makeSystemStatusMessage(joinedText,
                                            YOUTUBE_PLATFORM_ACCENT));
            }
            this->setStatusText(
                this->liveTitle_.isEmpty()
                    ? QString("Watching YouTube live chat for %1")
                          .arg(this->videoId_)
                    : QString("Watching YouTube live chat for %1")
                          .arg(this->liveTitle_));
            this->setLive(true);
            this->skipInitialBacklog_ = true;
            this->poll();
        })
        .onError([this, weak](NetworkResult) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            this->setStatusText("Couldn't load the YouTube live chat page.",
                                !this->failureReported_);
            this->failureReported_ = true;
            this->scheduleResolve(YOUTUBE_RECONNECT_DELAY_MS);
        })
        .execute();
}

void YouTubeLiveChat::poll()
{
    if (!this->running_ || this->continuation_.isEmpty() ||
        this->apiKey_.isEmpty())
    {
        return;
    }

    const auto url = QString(
                         "https://www.youtube.com/youtubei/v1/live_chat/"
                         "get_live_chat?prettyPrint=false&key=%1")
                         .arg(this->apiKey_);

    QJsonObject root{
        {"context",
         QJsonObject{{"client", makeYoutubeClientContext(this->clientVersion_)}}},
        {"continuation", this->continuation_},
    };

    auto request = NetworkRequest(url.toStdString())
                       .type(NetworkRequestType::Post)
                       .headerList(youtubeHeaders())
                       .json(root)
                       .header("Referer",
                               QString("https://www.youtube.com/watch?v=%1")
                                   .arg(this->videoId_));
    if (!this->visitorData_.isEmpty())
    {
        request = std::move(request).header("X-Goog-Visitor-Id",
                                            this->visitorData_);
    }

    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    std::move(request)
        .onSuccess([this, weak](const NetworkResult &result) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            auto json = result.parseJson();
            auto continuation = json["continuationContents"]
                                    .toObject()["liveChatContinuation"]
                                    .toObject();
            if (continuation.isEmpty())
            {
                this->setLive(false);
                if (this->shouldResolveLiveStreamFromSource())
                {
                    this->waitForNextLive(
                        "YouTube stream ended. Waiting for the next live.",
                        YOUTUBE_RECONNECT_DELAY_MS);
                }
                else
                {
                    this->setStatusText(
                        "YouTube live chat is no longer available for this "
                        "link.",
                        !this->failureReported_);
                    this->failureReported_ = true;
                    this->scheduleResolve(YOUTUBE_RECONNECT_DELAY_MS);
                }
                return;
            }

            auto actions = continuation["actions"].toArray();
            for (const auto &actionValue : actions)
            {
                auto action = actionValue.toObject();
                auto item = action["addChatItemAction"].toObject()["item"]
                                .toObject();
                if (item.isEmpty())
                {
                    continue;
                }

                const auto keys = item.keys();
                if (keys.isEmpty())
                {
                    continue;
                }

                const auto &rendererName = keys.first();
                auto message = parseRendererMessage(
                    item[rendererName].toObject(), rendererName, this->videoId_);
                if (!message || message->id.isEmpty())
                {
                    continue;
                }

                auto inserted = this->seenMessageIds_.emplace(message->id);
                if (!inserted.second)
                {
                    continue;
                }

                if (this->skipInitialBacklog_)
                {
                    continue;
                }

                this->messageReceived.invoke(message);
            }

            this->skipInitialBacklog_ = false;

            const auto continuations = continuation["continuations"].toArray();
            int nextDelay = 1000;
            for (const auto &continuationValue : continuations)
            {
                auto continuationObject = continuationValue.toObject();
                const auto timed =
                    continuationObject["timedContinuationData"].toObject();
                if (!timed.isEmpty())
                {
                    this->continuation_ = timed["continuation"].toString();
                    nextDelay = cappedYouTubePollDelay(
                        timed["timeoutMs"].toInt(nextDelay));
                    break;
                }

                const auto invalidation =
                    continuationObject["invalidationContinuationData"]
                        .toObject();
                if (!invalidation.isEmpty())
                {
                    this->continuation_ =
                        invalidation["continuation"].toString();
                    nextDelay = cappedYouTubePollDelay(
                        invalidation["timeoutMs"].toInt(nextDelay));
                    break;
                }
            }

            this->failureReported_ = false;
            this->setLive(true);
            this->schedulePoll(nextDelay);
        })
        .onError([this, weak](NetworkResult) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            this->setLive(false);
            this->setStatusText(
                "YouTube live chat polling failed. Retrying shortly.",
                !this->failureReported_);
            this->failureReported_ = true;
            this->schedulePoll(YOUTUBE_RECONNECT_DELAY_MS);
        })
        .execute();
}

void YouTubeLiveChat::schedulePoll(int delayMs)
{
    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    QTimer::singleShot(delayMs, [this, weak] {
        if (!weak.lock() || !this->running_)
        {
            return;
        }
        this->poll();
    });
}

void YouTubeLiveChat::scheduleResolve(int delayMs)
{
    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    QTimer::singleShot(delayMs, [this, weak] {
        if (!weak.lock() || !this->running_)
        {
            return;
        }

        this->resolveVideoId();
    });
}

void YouTubeLiveChat::waitForNextLive(QString text, int retryDelayMs)
{
    this->setLive(false);
    this->videoId_.clear();
    this->continuation_.clear();
    this->liveTitle_.clear();
    this->seenMessageIds_.clear();
    this->failureReported_ = false;
    this->skipInitialBacklog_ = false;
    this->setStatusText(std::move(text));
    this->scheduleResolve(retryDelayMs);
}

void YouTubeLiveChat::setLive(bool live)
{
    if (this->live_ == live)
    {
        return;
    }

    this->live_ = live;
    this->liveStatusChanged.invoke();
}

void YouTubeLiveChat::setStatusText(QString text, bool notifyAsSystemMessage)
{
    if (this->statusText_ == text)
    {
        return;
    }

    this->statusText_ = std::move(text);
    this->liveStatusChanged.invoke();

    if (notifyAsSystemMessage)
    {
        this->systemMessageReceived.invoke(
            makeSystemStatusMessage(this->statusText_));
    }
}

bool YouTubeLiveChat::shouldResolveLiveStreamFromSource() const
{
    return maybeExtractVideoId(this->streamUrl_).isEmpty();
}

QString YouTubeLiveChat::resolvedSource() const
{
    return normalizeSource(this->streamUrl_);
}

QString YouTubeLiveChat::maybeExtractVideoId(const QString &urlString)
{
    const auto trimmed = trimmedSource(urlString);
    if (isLikelyVideoId(trimmed))
    {
        return trimmed;
    }

    auto normalized = trimmed;
    if (!normalized.contains("://") &&
        (normalized.startsWith("www.", Qt::CaseInsensitive) ||
         normalized.startsWith("youtube.com/", Qt::CaseInsensitive) ||
         normalized.startsWith("m.youtube.com/", Qt::CaseInsensitive) ||
         normalized.startsWith("youtu.be/", Qt::CaseInsensitive)))
    {
        normalized.prepend("https://");
    }

    const QUrl url(normalized);
    if (!url.isValid())
    {
        return {};
    }

    const auto host = url.host().toLower();
    const auto path = url.path();
    const QUrlQuery query(url);

    if (query.hasQueryItem("v"))
    {
        return query.queryItemValue("v").trimmed();
    }

    if (host.endsWith("youtu.be"))
    {
        return path.sliced(1).section('/', 0, 0).trimmed();
    }

    if (path.startsWith("/live/"))
    {
        return path.section('/', 2, 2).trimmed();
    }

    if (path.startsWith("/shorts/") || path.startsWith("/embed/"))
    {
        return path.section('/', 2, 2).trimmed();
    }

    return {};
}

QString YouTubeLiveChat::normalizeSource(const QString &source)
{
    auto value = trimmedSource(source);
    if (value.isEmpty())
    {
        return {};
    }

    if (value.startsWith('@'))
    {
        return value.section('/', 0, 0).toLower();
    }

    if (isLikelyChannelId(value))
    {
        return value;
    }

    if (value.startsWith("//"))
    {
        value.prepend("https:");
    }
    else if (value.startsWith('/'))
    {
        value.prepend("https://www.youtube.com");
    }
    else if (!value.contains("://") &&
             (value.startsWith("www.", Qt::CaseInsensitive) ||
              value.startsWith("youtube.com/", Qt::CaseInsensitive) ||
              value.startsWith("m.youtube.com/", Qt::CaseInsensitive)))
    {
        value.prepend("https://");
    }

    QUrl url(value);
    if (!url.isValid() || url.host().isEmpty())
    {
        return {};
    }

    const auto host = url.host().toLower();
    const auto path = trimTrailingSlash(url.path());
    if (host.endsWith("youtube.com") && path.startsWith("/@"))
    {
        return path.section('/', 1, 1).toLower();
    }
    if (host.endsWith("youtube.com") && path.startsWith("/channel/"))
    {
        const auto channelId = path.section('/', 2, 2).trimmed();
        if (isLikelyChannelId(channelId))
        {
            return channelId;
        }
    }

    return {};
}

bool YouTubeLiveChat::isLikelyChannelId(const QString &value)
{
    static const QRegularExpression channelIdRegex("^UC[A-Za-z0-9_-]{22}$");
    return channelIdRegex.match(value.trimmed()).hasMatch();
}

QString YouTubeLiveChat::sourceLivePath(const QString &source)
{
    if (source.startsWith('@'))
    {
        return QString("/%1/live").arg(source);
    }

    if (isLikelyChannelId(source))
    {
        return QString("/channel/%1/live").arg(source);
    }

    return {};
}

bool YouTubeLiveChat::isLikelyVideoId(const QString &value)
{
    static const QRegularExpression videoIdRegex("^[A-Za-z0-9_-]{11}$");
    return videoIdRegex.match(value.trimmed()).hasMatch();
}

QString YouTubeLiveChat::extractFirstMatch(const QString &text,
                                           const QStringList &patterns)
{
    for (const auto &pattern : patterns)
    {
        const QRegularExpression regex(pattern);
        const auto match = regex.match(text);
        if (match.hasMatch())
        {
            return match.captured(1);
        }
    }

    return {};
}

QString YouTubeLiveChat::extractLiveChatContinuation(
    const QJsonObject &liveChatRenderer)
{
    const auto continuations = liveChatRenderer["continuations"].toArray();
    for (const auto &continuationValue : continuations)
    {
        const auto continuation =
            continuationValue.toObject()["reloadContinuationData"]
                .toObject()["continuation"]
                .toString()
                .trimmed();
        if (!continuation.isEmpty())
        {
            return continuation;
        }
    }

    const auto items =
        liveChatRenderer["header"]
            .toObject()["liveChatHeaderRenderer"]
            .toObject()["viewSelector"]
            .toObject()["sortFilterSubMenuRenderer"]
            .toObject()["subMenuItems"]
            .toArray();
    for (const auto &itemValue : items)
    {
        const auto continuation =
            itemValue.toObject()["continuation"]
                .toObject()["reloadContinuationData"]
                .toObject()["continuation"]
                .toString()
                .trimmed();
        if (!continuation.isEmpty())
        {
            return continuation;
        }
    }

    return {};
}

QString YouTubeLiveChat::extractVideoChannelId(const QString &html)
{
    return extractFirstMatch(
        html, {R"yt("channelId":"(UC[A-Za-z0-9_-]{22})")yt",
               R"yt("browseId":"(UC[A-Za-z0-9_-]{22})")yt",
               R"yt(itemprop="channelId" content="(UC[A-Za-z0-9_-]{22})")yt"});
}

QString YouTubeLiveChat::extractLiveVideoId(const QString &html)
{
    return extractFirstMatch(
        html, {R"yt(<link rel="canonical" href="https://www\.youtube\.com/watch\?v=([A-Za-z0-9_-]{11})")yt",
               R"yt("canonicalBaseUrl":"\\/watch\\?v=([A-Za-z0-9_-]{11})")yt",
               R"yt(itemprop="url" content="https://www\.youtube\.com/watch\?v=([A-Za-z0-9_-]{11})")yt"});
}

QString YouTubeLiveChat::extractLiveStreamTitle(const QJsonObject &nextResponse)
{
    const auto contents =
        nextResponse["contents"]
            .toObject()["twoColumnWatchNextResults"]
            .toObject()["results"]
            .toObject()["results"]
            .toObject()["contents"]
            .toArray();

    for (const auto &itemValue : contents)
    {
        const auto item = itemValue.toObject();
        const auto primary = item["videoPrimaryInfoRenderer"].toObject();
        if (primary.isEmpty())
        {
            continue;
        }

        const auto title = parseText(primary["title"]);
        if (!title.isEmpty())
        {
            return title;
        }
    }

    return {};
}

QString YouTubeLiveChat::parseText(const QJsonValue &value)
{
    auto object = value.toObject();
    if (object.contains("simpleText"))
    {
        return object["simpleText"].toString();
    }

    QString text;
    for (const auto &runValue : object["runs"].toArray())
    {
        auto run = runValue.toObject();
        if (run.contains("text"))
        {
            text.append(run["text"].toString());
        }
        else if (run.contains("emoji"))
        {
            auto emoji = run["emoji"].toObject();
            auto shortcuts = emoji["shortcuts"].toArray();
            if (!shortcuts.isEmpty())
            {
                text.append(shortcuts.first().toString());
            }
            else
            {
                text.append(emoji["emojiId"].toString());
            }
        }
    }

    return text.trimmed();
}

MessagePtr YouTubeLiveChat::parseRendererMessage(const QJsonObject &renderer,
                                                 const QString &rendererName,
                                                 const QString &channelName)
{
    if (renderer.isEmpty())
    {
        return nullptr;
    }

    QString text;
    MessageFlags flags;

    if (rendererName == "liveChatTextMessageRenderer")
    {
        text = parseText(renderer["message"]);
    }
    else if (rendererName == "liveChatPaidMessageRenderer")
    {
        flags.set(MessageFlag::ElevatedMessage);
        text = parseText(renderer["purchaseAmountText"]);
        const auto body = parseText(renderer["message"]);
        if (!body.isEmpty())
        {
            text += " " + body;
        }
    }
    else if (rendererName == "liveChatPaidStickerRenderer")
    {
        flags.set(MessageFlag::ElevatedMessage);
        text = parseText(renderer["purchaseAmountText"]);
    }
    else if (rendererName == "liveChatMembershipItemRenderer")
    {
        flags.set(MessageFlag::Subscription);
        text = parseText(renderer["headerSubtext"]);
        const auto body = parseText(renderer["message"]);
        if (!body.isEmpty())
        {
            text += " " + body;
        }
    }
    else
    {
        return nullptr;
    }

    const auto author = parseText(renderer["authorName"]);
    if (author.isEmpty())
    {
        return nullptr;
    }

    MessageBuilder builder;
    builder->flags = flags;
    builder->platform = MessagePlatform::YouTube;
    builder->id = renderer["id"].toString();
    builder->loginName = author;
    builder->displayName = author;
    builder->localizedName = author;
    builder->userID = renderer["authorExternalChannelId"].toString();
    builder->channelName = channelName;
    builder->messageText = text;
    builder->searchText = author + ": " + text;
    builder->serverReceivedTime =
        parseTimestampUsec(renderer["timestampUsec"].toString());
    builder.emplace<TimestampElement>(
        builder->serverReceivedTime.toLocalTime().time());
    builder
        .emplace<TextElement>(author + ":", MessageElementFlag::Username,
                              MessageColor::Text, FontStyle::ChatMediumBold)
        ->setLink({Link::UserInfo, author});
    appendSimpleWords(builder, text);
    return builder.release();
}

}  // namespace chatterino
