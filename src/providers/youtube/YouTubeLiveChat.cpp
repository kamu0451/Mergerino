// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeLiveChat.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "util/GuardedCallback.hpp"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <atomic>
#include <limits>
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
constexpr int YOUTUBE_BLOCKED_RETRY_DELAY_MS = 60000;
constexpr int YOUTUBE_SESSION_REFRESH_MS = 10 * 60 * 1000;
constexpr int YOUTUBE_HEALTH_CHECK_INTERVAL_MS = 10000;
constexpr int YOUTUBE_STALL_TIMEOUT_MS = 30000;

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

bool appendSimpleWords(MessageBuilder &builder, const QString &text)
{
    bool appended = false;
    for (auto word : QStringView(text).tokenize(u' ', Qt::SkipEmptyParts))
    {
        builder.addWordFromUserMessage(word);
        appended = true;
    }

    return appended;
}

QString youtubeEmojiShortcut(const QJsonObject &emoji)
{
    const auto shortcuts = emoji["shortcuts"].toArray();
    if (shortcuts.isEmpty())
    {
        return {};
    }

    return shortcuts.first().toString().trimmed();
}

QString youtubeEmojiLabel(const QJsonObject &emoji)
{
    return emoji["image"]
        .toObject()["accessibility"]
        .toObject()["accessibilityData"]
        .toObject()["label"]
        .toString()
        .trimmed();
}

QString youtubeEmojiFallbackText(const QJsonObject &emoji)
{
    auto fallback = youtubeEmojiShortcut(emoji);
    if (!fallback.isEmpty())
    {
        return fallback;
    }

    fallback = emoji["emojiId"].toString().trimmed();
    if (!fallback.isEmpty())
    {
        return fallback;
    }

    return youtubeEmojiLabel(emoji);
}

QString normalizeYouTubeImageUrl(QString url)
{
    url = url.trimmed();
    if (url.startsWith("//"))
    {
        url.prepend("https:");
    }

    return url;
}

QString youtubeEmojiImageUrl(const QJsonObject &emoji)
{
    const auto thumbnails = emoji["image"].toObject()["thumbnails"].toArray();

    QString bestUrl;
    int bestWidth = -1;
    for (const auto &thumbnailValue : thumbnails)
    {
        const auto thumbnail = thumbnailValue.toObject();
        const auto url = normalizeYouTubeImageUrl(thumbnail["url"].toString());
        if (url.isEmpty())
        {
            continue;
        }

        const auto width = thumbnail["width"].toInt();
        if (bestUrl.isEmpty() || width >= bestWidth)
        {
            bestUrl = url;
            bestWidth = width;
        }
    }

    return bestUrl;
}

EmotePtr makeYouTubeEmojiEmote(const QJsonObject &emoji)
{
    const auto imageUrl = youtubeEmojiImageUrl(emoji);
    if (imageUrl.isEmpty())
    {
        return nullptr;
    }

    auto name = youtubeEmojiFallbackText(emoji);
    if (name.isEmpty())
    {
        name = QStringLiteral("YouTube Emoji");
    }

    const auto label = youtubeEmojiLabel(emoji);
    auto tooltip = name.toHtmlEscaped();
    if (!label.isEmpty() && label != name)
    {
        tooltip += QStringLiteral("<br>") + label.toHtmlEscaped();
    }
    tooltip += QStringLiteral("<br>YouTube Emoji");

    auto id = emoji["emojiId"].toString().trimmed();
    if (id.isEmpty())
    {
        id = imageUrl;
    }

    return std::make_shared<const Emote>(Emote{
        .name = {std::move(name)},
        .images = ImageSet(Image::fromAutoscaledUrl({imageUrl}, 28)),
        .tooltip = {std::move(tooltip)},
        .id = {std::move(id)},
    });
}

bool appendYouTubeText(MessageBuilder &builder, const QJsonValue &value)
{
    const auto object = value.toObject();
    if (object.contains("simpleText"))
    {
        return appendSimpleWords(builder, object["simpleText"].toString());
    }

    bool appended = false;
    for (const auto &runValue : object["runs"].toArray())
    {
        const auto run = runValue.toObject();
        if (run.contains("text"))
        {
            appended =
                appendSimpleWords(builder, run["text"].toString()) || appended;
            continue;
        }

        if (!run.contains("emoji"))
        {
            continue;
        }

        const auto emoji = run["emoji"].toObject();
        if (const auto emote = makeYouTubeEmojiEmote(emoji))
        {
            builder.emplace<EmoteElement>(emote, MessageElementFlag::Emote,
                                          builder.textColor());
            appended = true;
            continue;
        }

        appended =
            appendSimpleWords(builder, youtubeEmojiFallbackText(emoji)) ||
            appended;
    }

    return appended;
}

void appendYouTubeTextParts(MessageBuilder &builder,
                            const std::vector<QJsonValue> &parts,
                            const QString &fallbackText)
{
    bool appended = false;
    for (const auto &part : parts)
    {
        appended = appendYouTubeText(builder, part) || appended;
    }

    if (!appended)
    {
        appendSimpleWords(builder, fallbackText);
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
    constexpr int maximumDelayMs = 30000;

    if (timeoutMs <= 0)
    {
        return minimumDelayMs;
    }

    return std::clamp(timeoutMs, minimumDelayMs, maximumDelayMs);
}

int adjustedYouTubePollDelay(int timeoutMs, qint64 /* requestElapsedMs */,
                             int deliveredMessageCount,
                             int activePollStreak)
{
    const auto serverDelay = cappedYouTubePollDelay(timeoutMs);
    if (deliveredMessageCount <= 0)
    {
        constexpr int idleDelayCapMs = 3000;
        return std::min(serverDelay, idleDelayCapMs);
    }

    int busyDelayCapMs = 1000;
    if (deliveredMessageCount >= 25)
    {
        busyDelayCapMs = 1000;
    }
    else if (deliveredMessageCount >= 10 || activePollStreak >= 2)
    {
        busyDelayCapMs = 1000;
    }

    return std::min(serverDelay, busyDelayCapMs);
}

QDateTime parseYouTubeIsoDateTime(const QString &value)
{
    if (value.trimmed().isEmpty())
    {
        return {};
    }

    auto timestamp = QDateTime::fromString(value.trimmed(), Qt::ISODateWithMs);
    if (!timestamp.isValid())
    {
        timestamp = QDateTime::fromString(value.trimmed(), Qt::ISODate);
    }

    return timestamp.isValid() ? timestamp.toUTC() : QDateTime{};
}

QString formatYouTubeUptime(const QDateTime &startedAt)
{
    if (!startedAt.isValid())
    {
        return {};
    }

    const auto seconds = startedAt.secsTo(QDateTime::currentDateTimeUtc());
    if (seconds < 0)
    {
        return {};
    }

    return QString::number(seconds / 3600) % u"h " %
           QString::number(seconds % 3600 / 60) % u"m";
}

QString extractLiveLockupVideoId(const QString &html)
{
    static const QString lockupMarker =
        QStringLiteral(R"("lockupViewModel":{)");
    static const QString nextLockupMarker = QStringLiteral(
        R"("richItemRenderer":{"content":{"lockupViewModel":{)");
    static const QRegularExpression contentIdRegex(
        R"yt("contentId":"([A-Za-z0-9_-]{11})")yt");

    qsizetype searchFrom = 0;
    while (searchFrom < html.size())
    {
        const auto start = html.indexOf(lockupMarker, searchFrom);
        if (start < 0)
        {
            break;
        }

        auto end = html.indexOf(nextLockupMarker, start + lockupMarker.size());
        if (end < 0)
        {
            end = html.size();
        }

        const auto block = html.sliced(start, end - start);
        searchFrom = start + lockupMarker.size();

        if (!block.contains(QStringLiteral("LOCKUP_CONTENT_TYPE_VIDEO")))
        {
            continue;
        }

        const bool hasLiveBadge = block.contains(
                                      QStringLiteral(
                                          "THUMBNAIL_OVERLAY_BADGE_STYLE_LIVE")) ||
                                  block.contains(QStringLiteral(R"("text":"LIVE")"));
        const bool hasWatchingMetadata =
            block.contains(QStringLiteral(" watching"), Qt::CaseInsensitive);
        if (!hasLiveBadge && !hasWatchingMetadata)
        {
            continue;
        }

        const auto match = contentIdRegex.match(block);
        if (match.hasMatch())
        {
            return match.captured(1);
        }
    }

    return {};
}

QString extractLiveVideoRendererVideoId(const QString &html)
{
    static const QString rendererMarker =
        QStringLiteral(R"("videoRenderer":{)");
    static const QRegularExpression videoIdRegex(
        R"yt("videoId":"([A-Za-z0-9_-]{11})")yt");

    qsizetype searchFrom = 0;
    while (searchFrom < html.size())
    {
        const auto start = html.indexOf(rendererMarker, searchFrom);
        if (start < 0)
        {
            break;
        }

        auto end = html.indexOf(rendererMarker, start + rendererMarker.size());
        if (end < 0)
        {
            end = html.size();
        }

        const auto block = html.sliced(start, end - start);
        searchFrom = start + rendererMarker.size();

        const bool hasLiveBadge =
            block.contains(QStringLiteral(R"("style":"LIVE")")) ||
            block.contains(QStringLiteral(R"("text":"LIVE")")) ||
            block.contains(QStringLiteral("THUMBNAIL_OVERLAY_BADGE_STYLE_LIVE"));
        const bool hasWatchingMetadata =
            block.contains(QStringLiteral(" watching"), Qt::CaseInsensitive);
        if (!hasLiveBadge && !hasWatchingMetadata)
        {
            continue;
        }

        const auto match = videoIdRegex.match(block);
        if (match.hasMatch())
        {
            return match.captured(1);
        }
    }

    return {};
}

QString extractLiveWatchEndpointVideoId(const QString &html)
{
    static const QRegularExpression watchEndpointRegex(
        R"yt("watchEndpoint":\{"videoId":"([A-Za-z0-9_-]{11})")yt");

    auto match = watchEndpointRegex.match(html);
    while (match.hasMatch())
    {
        const auto start =
            std::max<qsizetype>(0, match.capturedStart() - 8000);
        const auto end =
            std::min<qsizetype>(html.size(), match.capturedEnd() + 8000);
        const auto block = html.sliced(start, end - start);

        const bool hasLiveBadge =
            block.contains(QStringLiteral(R"("text":"LIVE")")) ||
            block.contains(QStringLiteral("THUMBNAIL_OVERLAY_BADGE_STYLE_LIVE"));
        const bool hasWatchingMetadata =
            block.contains(QStringLiteral(" watching"), Qt::CaseInsensitive);
        if (hasLiveBadge || hasWatchingMetadata)
        {
            return match.captured(1);
        }

        match = watchEndpointRegex.match(html, match.capturedEnd());
    }

    return {};
}

bool isLikelyJsonVideoId(const QString &value)
{
    static const QRegularExpression videoIdRegex("^[A-Za-z0-9_-]{11}$");
    return videoIdRegex.match(value.trimmed()).hasMatch();
}

bool jsonContainsLiveMarker(const QJsonValue &value)
{
    if (value.isString())
    {
        const auto text = value.toString();
        return text.contains(QStringLiteral("LIVE")) ||
            text.contains(QStringLiteral("watching"), Qt::CaseInsensitive) ||
            text.contains(QStringLiteral("THUMBNAIL_OVERLAY_BADGE_STYLE_LIVE")) ||
            text.contains(
                QStringLiteral("THUMBNAIL_OVERLAY_TIME_STATUS_STYLE_LIVE"));
    }

    if (value.isArray())
    {
        for (const auto &item : value.toArray())
        {
            if (jsonContainsLiveMarker(item))
            {
                return true;
            }
        }
        return false;
    }

    if (!value.isObject())
    {
        return false;
    }

    const auto object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it)
    {
        if (jsonContainsLiveMarker(it.value()))
        {
            return true;
        }
    }

    return false;
}

QString extractLiveVideoIdFromJson(const QJsonValue &value)
{
    if (value.isArray())
    {
        for (const auto &item : value.toArray())
        {
            const auto videoId = extractLiveVideoIdFromJson(item);
            if (!videoId.isEmpty())
            {
                return videoId;
            }
        }
        return {};
    }

    if (!value.isObject())
    {
        return {};
    }

    const auto object = value.toObject();
    const auto videoId = object["videoId"].toString().trimmed();
    if (isLikelyJsonVideoId(videoId) && jsonContainsLiveMarker(value))
    {
        return videoId;
    }

    const auto contentId = object["contentId"].toString().trimmed();
    if (isLikelyJsonVideoId(contentId) && jsonContainsLiveMarker(value))
    {
        return contentId;
    }

    for (const auto &priorityKey :
         {QStringLiteral("channelVideoPlayerRenderer"),
          QStringLiteral("videoRenderer"), QStringLiteral("gridVideoRenderer"),
          QStringLiteral("lockupViewModel")})
    {
        const auto videoId = extractLiveVideoIdFromJson(object[priorityKey]);
        if (!videoId.isEmpty())
        {
            return videoId;
        }
    }

    for (auto it = object.begin(); it != object.end(); ++it)
    {
        const auto videoId = extractLiveVideoIdFromJson(it.value());
        if (!videoId.isEmpty())
        {
            return videoId;
        }
    }

    return {};
}

QString extractContinuationFromObject(const QJsonObject &object)
{
    for (const auto &key :
         {QStringLiteral("reloadContinuationData"),
          QStringLiteral("timedContinuationData"),
          QStringLiteral("invalidationContinuationData")})
    {
        const auto continuation =
            object[key].toObject()["continuation"].toString().trimmed();
        if (!continuation.isEmpty())
        {
            return continuation;
        }
    }

    return {};
}

QString extractLiveChatContinuationFromJson(const QJsonValue &value)
{
    if (value.isArray())
    {
        for (const auto &item : value.toArray())
        {
            const auto continuation = extractLiveChatContinuationFromJson(item);
            if (!continuation.isEmpty())
            {
                return continuation;
            }
        }
        return {};
    }

    if (!value.isObject())
    {
        return {};
    }

    const auto object = value.toObject();
    const auto continuation = extractContinuationFromObject(object);
    if (!continuation.isEmpty())
    {
        return continuation;
    }

    if (object.contains("continuation") &&
        (object.contains("timeoutMs") || object.contains("invalidationId") ||
         object.contains("clickTrackingParams")))
    {
        const auto directContinuation =
            object["continuation"].toString().trimmed();
        if (!directContinuation.isEmpty())
        {
            return directContinuation;
        }
    }

    for (const auto &priorityKey :
         {QStringLiteral("liveChatRenderer"),
          QStringLiteral("liveChatContinuation")})
    {
        const auto continuation =
            extractLiveChatContinuationFromJson(object[priorityKey]);
        if (!continuation.isEmpty())
        {
            return continuation;
        }
    }

    for (auto it = object.begin(); it != object.end(); ++it)
    {
        const auto continuation = extractLiveChatContinuationFromJson(it.value());
        if (!continuation.isEmpty())
        {
            return continuation;
        }
    }

    return {};
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
    this->liveStartedAt_ = {};
    this->liveViewerCount_ = 0;
    this->seenMessageIds_.clear();
    this->failureReported_ = false;
    this->skipInitialBacklog_ = false;
    this->activePollStreak_ = 0;
    this->liveChatSessionRefreshTimer_.invalidate();
    this->liveChatProgressTimer_.invalidate();
    this->setLive(false);
    this->scheduleHealthCheck(YOUTUBE_HEALTH_CHECK_INTERVAL_MS);
    this->resolveVideoId();
}

void YouTubeLiveChat::stop()
{
    this->running_ = false;
    this->activePollStreak_ = 0;
    this->liveChatSessionRefreshTimer_.invalidate();
    this->liveChatProgressTimer_.invalidate();
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

uint64_t YouTubeLiveChat::liveViewerCount() const
{
    return this->liveViewerCount_;
}

unsigned YouTubeLiveChat::viewerCount() const
{
    return static_cast<unsigned>(
        std::min<uint64_t>(this->liveViewerCount_,
                           std::numeric_limits<unsigned>::max()));
}

QString YouTubeLiveChat::liveUptime() const
{
    return formatYouTubeUptime(this->liveStartedAt_);
}

QString YouTubeLiveChat::previewThumbnailUrl() const
{
    if (this->videoId_.isEmpty())
    {
        return {};
    }

    return QString("https://i.ytimg.com/vi/%1/hqdefault_live.jpg")
        .arg(this->videoId_);
}

void YouTubeLiveChat::resolveVideoId()
{
    if (!this->running_)
    {
        return;
    }

    const auto sourceVideoId = maybeExtractVideoId(this->streamUrl_);
    auto source = this->resolvedSource();
    if (source.isEmpty() && !sourceVideoId.isEmpty())
    {
        this->resolveChannelIdFromVideoId(
            sourceVideoId, [this, sourceVideoId](QString channelId) {
                if (!this->running_)
                {
                    return;
                }

                auto joinSourceVideo = [this, sourceVideoId] {
                    if (!this->running_)
                    {
                        return;
                    }

                    this->videoId_ = sourceVideoId;
                    this->seenMessageIds_.clear();
                    this->fetchLiveChatPage();
                };

                if (channelId.isEmpty())
                {
                    joinSourceVideo();
                    return;
                }

                if (this->streamUrl_ != channelId)
                {
                    this->streamUrl_ = channelId;
                    this->sourceResolved.invoke(channelId);
                }

                this->probeLiveVideoIdFromSource(
                    channelId, [this, joinSourceVideo](QString videoId) {
                        if (!this->running_)
                        {
                            return;
                        }

                        if (videoId.isEmpty())
                        {
                            joinSourceVideo();
                            return;
                        }

                        this->videoId_ = std::move(videoId);
                        this->seenMessageIds_.clear();
                        this->fetchLiveChatPage();
                    });
            });
        return;
    }

    if (source.isEmpty())
    {
        this->setStatusText(
            "Enter a YouTube @handle or any video link from the desired channel.",
                            !this->failureReported_);
        this->failureReported_ = true;
        return;
    }

    this->resolveChannelIdFromSource(source, [this](QString channelId) {
        if (!this->running_)
        {
            return;
        }

        if (channelId.isEmpty())
        {
            this->setStatusText(
                "Couldn't resolve that YouTube source to a channel ID.",
                !this->failureReported_);
            this->failureReported_ = true;
            this->scheduleResolve(YOUTUBE_RECONNECT_DELAY_MS);
            return;
        }

        if (this->streamUrl_ != channelId)
        {
            this->streamUrl_ = channelId;
            this->sourceResolved.invoke(channelId);
        }

        this->probeLiveVideoIdFromSource(
            channelId, [this, channelId](QString videoId) {
                if (!this->running_)
                {
                    return;
                }

                if (videoId.isEmpty())
                {
                    this->waitForNextLive(
                        QString("Waiting for %1 to go live on YouTube.")
                            .arg(channelId),
                        YOUTUBE_RECONNECT_DELAY_MS);
                    return;
                }

                this->videoId_ = std::move(videoId);
                this->seenMessageIds_.clear();
                this->fetchLiveChatPage();
            });
    });
}

void YouTubeLiveChat::resolveChannelIdFromSource(
    const QString &source, std::function<void(QString)> onResolved)
{
    const auto trimmed = source.trimmed();
    if (!this->running_ || trimmed.isEmpty())
    {
        onResolved({});
        return;
    }

    if (isLikelyChannelId(trimmed))
    {
        onResolved(trimmed);
        return;
    }

    const auto videoId = maybeExtractVideoId(trimmed);
    if (!videoId.isEmpty())
    {
        this->resolveChannelIdFromVideoId(videoId, std::move(onResolved));
        return;
    }

    if (trimmed.startsWith('@'))
    {
        this->resolveChannelIdFromHandle(trimmed, std::move(onResolved));
        return;
    }

    this->resolveChannelIdFromSearch(trimmed, std::move(onResolved));
}

void YouTubeLiveChat::resolveChannelIdFromHandle(
    const QString &handle, std::function<void(QString)> onResolved)
{
    const auto trimmed = handle.trimmed();
    if (!this->running_ || !trimmed.startsWith('@'))
    {
        onResolved({});
        return;
    }

    auto callback =
        std::make_shared<std::function<void(QString)>>(std::move(onResolved));
    const auto url = QString("https://www.youtube.com/%1").arg(trimmed);
    NetworkRequest(url.toStdString())
        .headerList(youtubeHeaders())
        .followRedirects(true)
        .timeout(15000)
        .onSuccess(guardedCallback(this->lifetimeGuard_,
                                   [this, callback, trimmed](
                                       const NetworkResult &result) {
            if (!this->running_)
            {
                return;
            }

            const auto html = QString::fromUtf8(result.getData());
            const auto channelId = extractVideoChannelId(html);
            if (!channelId.isEmpty())
            {
                (*callback)(channelId);
                return;
            }

            this->resolveChannelIdFromSearch(trimmed, [callback](QString id) {
                (*callback)(std::move(id));
            });
                                   }))
        .onError(guardedCallback(this->lifetimeGuard_,
                                 [this, callback, trimmed](NetworkResult) {
            if (!this->running_)
            {
                return;
            }

            this->resolveChannelIdFromSearch(trimmed, [callback](QString id) {
                (*callback)(std::move(id));
            });
                                 }))
        .execute();
}

void YouTubeLiveChat::resolveChannelIdFromVideoId(
    const QString &videoId, std::function<void(QString)> onResolved)
{
    if (!this->running_ || videoId.isEmpty())
    {
        onResolved({});
        return;
    }

    auto callback =
        std::make_shared<std::function<void(QString)>>(std::move(onResolved));
    NetworkRequest(
        QString("https://www.youtube.com/watch?v=%1").arg(videoId).toStdString())
        .headerList(youtubeHeaders())
        .followRedirects(true)
        .timeout(15000)
        .onSuccess(guardedCallback(this->lifetimeGuard_,
                                   [this, callback](
                                       const NetworkResult &result) {
            if (!this->running_)
            {
                return;
            }

            const auto html = QString::fromUtf8(result.getData());
            (*callback)(extractVideoChannelId(html));
                                   }))
        .onError(guardedCallback(this->lifetimeGuard_,
                                 [this, callback](NetworkResult) {
            if (!this->running_)
            {
                return;
            }

            (*callback)({});
                                 }))
        .execute();
}

void YouTubeLiveChat::resolveChannelIdFromSearch(
    const QString &source, std::function<void(QString)> onResolved)
{
    const auto searchSource = source.trimmed();
    if (!this->running_ || searchSource.isEmpty())
    {
        onResolved({});
        return;
    }

    QUrl url("https://www.youtube.com/results");
    QUrlQuery query;
    query.addQueryItem("search_query", searchSource);
    url.setQuery(query);

    auto callback =
        std::make_shared<std::function<void(QString)>>(std::move(onResolved));
    NetworkRequest(url.toString().toStdString())
        .headerList(youtubeHeaders())
        .followRedirects(true)
        .timeout(15000)
        .onSuccess(guardedCallback(this->lifetimeGuard_,
                                   [this, callback](
                                       const NetworkResult &result) {
            if (!this->running_)
            {
                return;
            }

            const auto html = QString::fromUtf8(result.getData());
            (*callback)(extractVideoChannelId(html));
                                   }))
        .onError(guardedCallback(this->lifetimeGuard_,
                                 [this, callback](NetworkResult) {
            if (!this->running_)
            {
                return;
            }

            (*callback)({});
                                 }))
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
    const auto streamsPath = sourceStreamsPath(source);
    if (livePath.isEmpty() && streamsPath.isEmpty())
    {
        onResolved({});
        return;
    }

    auto callback =
        std::make_shared<std::function<void(QString)>>(std::move(onResolved));

    auto finishEmpty = [callback] {
        (*callback)({});
    };

    auto tryStreams = [this, streamsPath, finishEmpty, callback] {
        if (streamsPath.isEmpty())
        {
            finishEmpty();
            return;
        }

        this->probeLiveVideoIdFromPath(streamsPath,
                                       [this, finishEmpty, callback](
                                           QString videoId) {
            if (!this->running_)
            {
                return;
            }

            if (videoId.isEmpty())
            {
                finishEmpty();
                return;
            }

            (*callback)(std::move(videoId));
        });
    };

    auto tryLivePath = [this, livePath, tryStreams, callback] {
        if (livePath.isEmpty())
        {
            tryStreams();
            return;
        }

        this->probeLiveVideoIdFromPath(livePath, [this, tryStreams, callback](
                                                     QString videoId) {
            if (!this->running_)
            {
                return;
            }

            if (!videoId.isEmpty())
            {
                (*callback)(std::move(videoId));
                return;
            }

            tryStreams();
        });
    };

    auto tryEmbed = [this, source, tryLivePath, callback] {
        if (!isLikelyChannelId(source))
        {
            tryLivePath();
            return;
        }

        this->probeLiveVideoIdFromEmbed(
            source, [this, tryLivePath, callback](QString videoId) {
                if (!this->running_)
                {
                    return;
                }

                if (!videoId.isEmpty())
                {
                    (*callback)(std::move(videoId));
                    return;
                }

                tryLivePath();
            });
    };

    if (!isLikelyChannelId(source))
    {
        tryLivePath();
        return;
    }

    this->probeLiveVideoIdFromBrowse(
        source, [this, tryEmbed, callback](QString videoId) {
                if (!this->running_)
                {
                    return;
                }

                if (!videoId.isEmpty())
                {
                    (*callback)(std::move(videoId));
                    return;
                }

                tryEmbed();
            });
}

void YouTubeLiveChat::probeLiveVideoIdFromEmbed(
    const QString &channelId, std::function<void(QString)> onResolved)
{
    if (!this->running_ || !isLikelyChannelId(channelId))
    {
        onResolved({});
        return;
    }

    QUrl url("https://www.youtube.com/embed/live_stream");
    QUrlQuery query;
    query.addQueryItem("channel", channelId);
    url.setQuery(query);

    auto callback =
        std::make_shared<std::function<void(QString)>>(std::move(onResolved));
    NetworkRequest(url.toString().toStdString())
        .headerList(youtubeHeaders())
        .followRedirects(true)
        .timeout(15000)
        .header("Referer",
                QString("https://www.youtube.com/channel/%1").arg(channelId))
        .onSuccess(guardedCallback(this->lifetimeGuard_,
                                   [this, callback](
                                       const NetworkResult &result) {
            if (!this->running_)
            {
                return;
            }

            const auto html = QString::fromUtf8(result.getData());
            (*callback)(extractEmbedLiveVideoId(html));
                                   }))
        .onError(guardedCallback(this->lifetimeGuard_,
                                 [this, callback](NetworkResult) {
            if (!this->running_)
            {
                return;
            }

            (*callback)({});
                                 }))
        .execute();
}

void YouTubeLiveChat::probeLiveVideoIdFromPath(
    const QString &path, std::function<void(QString)> onResolved)
{
    if (!this->running_ || path.isEmpty())
    {
        onResolved({});
        return;
    }

    const auto url = QString("https://www.youtube.com%1").arg(path);
    auto callback =
        std::make_shared<std::function<void(QString)>>(std::move(onResolved));
    NetworkRequest(url.toStdString())
        .headerList(youtubeHeaders())
        .followRedirects(true)
        .timeout(15000)
        .onSuccess(guardedCallback(
            this->lifetimeGuard_,
            [this, callback, path](const NetworkResult &result) {
                if (!this->running_)
                {
                    return;
                }

                const auto html = QString::fromUtf8(result.getData());
                (*callback)(extractLiveVideoId(html, path.endsWith("/live")));
            }))
        .onError(guardedCallback(this->lifetimeGuard_,
                                 [this, callback](NetworkResult) {
            if (!this->running_)
            {
                return;
            }

            (*callback)({});
                                 }))
        .execute();
}

void YouTubeLiveChat::probeLiveVideoIdFromBrowse(
    const QString &channelId, std::function<void(QString)> onResolved)
{
    if (!this->running_ || !isLikelyChannelId(channelId))
    {
        onResolved({});
        return;
    }

    if (this->apiKey_.isEmpty() || this->clientVersion_.isEmpty())
    {
        this->bootstrapInnertubeContext(
            [this, channelId, onResolved = std::move(onResolved)]() mutable {
                this->probeLiveVideoIdFromBrowse(channelId,
                                                 std::move(onResolved));
            },
            "Couldn't initialize YouTube live chat.");
        return;
    }

    auto callback =
        std::make_shared<std::function<void(QString)>>(std::move(onResolved));
    const auto liveRefererPath = QString("/channel/%1/live").arg(channelId);
    const auto streamsRefererPath =
        QString("/channel/%1/streams").arg(channelId);

    auto tryStreamsTab = [this, channelId, streamsRefererPath, callback] {
        this->probeLiveVideoIdFromBrowseTab(
            channelId, QStringLiteral("EgdzdHJlYW1zuAEAkgMA8gYECgJ6AA=="),
            streamsRefererPath, [this, callback](QString videoId) {
                if (!this->running_)
                {
                    return;
                }

                (*callback)(std::move(videoId));
            });
    };

    this->probeLiveVideoIdFromBrowseTab(
        channelId, QStringLiteral("EgRsaXZluAEAkgMA8gYECgJ6AA=="),
        liveRefererPath,
        [this, tryStreamsTab, callback](QString videoId) {
            if (!this->running_)
            {
                return;
            }

            if (!videoId.isEmpty())
            {
                (*callback)(std::move(videoId));
                return;
            }

            tryStreamsTab();
        });
}

void YouTubeLiveChat::probeLiveVideoIdFromBrowseTab(
    const QString &channelId, const QString &params, const QString &refererPath,
    std::function<void(QString)> onResolved)
{
    if (!this->running_ || !isLikelyChannelId(channelId) ||
        params.trimmed().isEmpty() || refererPath.trimmed().isEmpty())
    {
        onResolved({});
        return;
    }

    const auto url =
        QString("https://www.youtube.com/youtubei/v1/browse?key=%1")
            .arg(this->apiKey_);
    QJsonObject root{
        {"context",
         QJsonObject{{"client", makeYoutubeClientContext(this->clientVersion_)}}},
        {"browseId", channelId},
        {"params", params},
    };

    auto request = NetworkRequest(url.toStdString())
                       .type(NetworkRequestType::Post)
                       .headerList(youtubeHeaders())
                       .json(root)
                       .timeout(15000)
                       .header("Referer",
                               QString("https://www.youtube.com%1")
                                   .arg(refererPath));
    if (!this->visitorData_.isEmpty())
    {
        request = std::move(request).header("X-Goog-Visitor-Id",
                                            this->visitorData_);
    }

    auto callback =
        std::make_shared<std::function<void(QString)>>(std::move(onResolved));
    std::move(request)
        .onSuccess(guardedCallback(
            this->lifetimeGuard_,
            [this, callback](const NetworkResult &result) {
                if (!this->running_)
                {
                    return;
                }

                (*callback)(extractLiveVideoIdFromJson(result.parseJson()));
            }))
        .onError(guardedCallback(this->lifetimeGuard_,
                                 [this, callback](NetworkResult) {
            if (!this->running_)
            {
                return;
            }

            (*callback)({});
                                 }))
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

    NetworkRequest(YOUTUBE_BOOTSTRAP_URL.toStdString())
        .headerList(youtubeHeaders())
        .followRedirects(true)
        .timeout(15000)
        .onSuccess(guardedCallback(
            this->lifetimeGuard_,
            [this, onReady = std::move(onReady), failureText](
                const NetworkResult &result) mutable {
                if (!this->running_)
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
                this->scheduleResolve(YOUTUBE_BLOCKED_RETRY_DELAY_MS);
                return;
            }
            if (html.contains("unusual traffic", Qt::CaseInsensitive))
            {
                this->setStatusText(
                    "YouTube blocked the live chat request from this network.",
                    !this->failureReported_);
                this->failureReported_ = true;
                this->scheduleResolve(YOUTUBE_BLOCKED_RETRY_DELAY_MS);
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
            }))
        .onError(guardedCallback(
            this->lifetimeGuard_,
            [this, failureText = std::move(failureText)](NetworkResult) mutable {
                if (!this->running_)
                {
                    return;
                }

            this->setStatusText(failureText, !this->failureReported_);
            this->failureReported_ = true;
            this->scheduleResolve(YOUTUBE_RECONNECT_DELAY_MS);
            }))
        .execute();
}

void YouTubeLiveChat::fetchLiveChatPage(bool skipInitialBacklog)
{
    if (!this->running_ || this->videoId_.isEmpty())
    {
        return;
    }

    const bool activeLiveRefresh = this->live_;
    const auto requestedVideoId = this->videoId_;

    if (this->apiKey_.isEmpty() || this->clientVersion_.isEmpty())
    {
        this->bootstrapInnertubeContext(
            [this, skipInitialBacklog] {
                this->fetchLiveChatPage(skipInitialBacklog);
            },
            "Couldn't initialize YouTube live chat.");
        return;
    }

    const auto url =
        QString("https://www.youtube.com/youtubei/v1/next?prettyPrint=false&key=%1")
            .arg(this->apiKey_);
    QJsonObject root{
        {"context",
         QJsonObject{{"client", makeYoutubeClientContext(this->clientVersion_)}}},
        {"videoId", requestedVideoId},
    };

    auto request = NetworkRequest(url.toStdString())
                       .type(NetworkRequestType::Post)
                       .headerList(youtubeHeaders())
                       .json(root)
                       .timeout(15000)
                       .header("Referer",
                               QString("https://www.youtube.com/watch?v=%1")
                                   .arg(requestedVideoId));
    if (!this->visitorData_.isEmpty())
    {
        request = std::move(request).header("X-Goog-Visitor-Id",
                                            this->visitorData_);
    }

    auto requestTimer = std::make_shared<QElapsedTimer>();
    requestTimer->start();
    std::move(request)
        .onSuccess(guardedCallback(
            this->lifetimeGuard_,
            [this, requestTimer, skipInitialBacklog, activeLiveRefresh,
             requestedVideoId](const NetworkResult &result) {
                if (!this->running_ || this->videoId_ != requestedVideoId)
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
                this->continuation_ = extractLiveChatContinuationFromJson(json);
            }
            if (this->continuation_.isEmpty())
            {
                if (activeLiveRefresh && !this->failureReported_)
                {
                    this->recoverLiveChat(
                        "YouTube live chat refresh lost its continuation. "
                        "Reconnecting.",
                        YOUTUBE_RECONNECT_DELAY_MS);
                    return;
                }

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
                    this->resetInnertubeContext();
                    this->scheduleResolve(YOUTUBE_RECONNECT_DELAY_MS);
                }
                return;
            }

            this->liveTitle_ = extractLiveStreamTitle(json);
            const auto liveStartedAt = extractLiveStartTime(json);
            if (liveStartedAt.isValid())
            {
                this->liveStartedAt_ = liveStartedAt;
            }
            else if (!activeLiveRefresh)
            {
                this->liveStartedAt_ = {};
            }
            if (!activeLiveRefresh)
            {
                this->liveViewerCount_ = 0;
            }
            this->failureReported_ = false;
            this->activePollStreak_ = 0;
            this->liveChatSessionRefreshTimer_.restart();
            this->liveChatProgressTimer_.restart();
            // "Joined YouTube live chat: <title>" is announced by
            // MergedChannel's liveStatusChanged handler via the same flag
            // pattern Twitch/Kick/TikTok use. setLive(true) below fires the
            // signal; liveTitle_ is already set above so MergedChannel can
            // read it.
            this->setStatusText(
                this->liveTitle_.isEmpty()
                    ? QString("Watching YouTube live chat for %1")
                          .arg(this->videoId_)
                    : QString("Watching YouTube live chat for %1")
                          .arg(this->liveTitle_));
            this->setLive(true);
            this->skipInitialBacklog_ = skipInitialBacklog;
            this->poll();
            }))
        .onError(guardedCallback(
            this->lifetimeGuard_,
            [this, activeLiveRefresh, requestedVideoId](NetworkResult) {
                if (!this->running_ || this->videoId_ != requestedVideoId)
                {
                    return;
                }

            if (activeLiveRefresh && !this->failureReported_)
            {
                this->recoverLiveChat(
                    "YouTube live chat refresh failed. Reconnecting.",
                    YOUTUBE_RECONNECT_DELAY_MS);
                return;
            }

            this->setStatusText("Couldn't load the YouTube live chat page.",
                                !this->failureReported_);
            this->failureReported_ = true;
            this->resetInnertubeContext();
            this->scheduleResolve(YOUTUBE_RECONNECT_DELAY_MS);
            }))
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
                       .timeout(15000)
                       .header("Referer",
                               QString("https://www.youtube.com/watch?v=%1")
                                   .arg(this->videoId_));
    if (!this->visitorData_.isEmpty())
    {
        request = std::move(request).header("X-Goog-Visitor-Id",
                                            this->visitorData_);
    }

    auto requestTimer = std::make_shared<QElapsedTimer>();
    requestTimer->start();
    std::move(request)
        .onSuccess(guardedCallback(
            this->lifetimeGuard_,
            [this, requestTimer](const NetworkResult &result) {
                if (!this->running_)
                {
                    return;
                }

            auto json = result.parseJson();
            auto continuation = json["continuationContents"]
                                    .toObject()["liveChatContinuation"]
                                    .toObject();
            this->liveChatProgressTimer_.restart();
            if (continuation.isEmpty())
            {
                this->recoverLiveChat(
                    "YouTube live chat continuation expired. Reconnecting.",
                    YOUTUBE_RECONNECT_DELAY_MS);
                return;
            }

            auto actions = continuation["actions"].toArray();
            const auto viewerCount = parseViewerCount(
                continuation["viewerCount"]);
            if (viewerCount > 0 && viewerCount != this->liveViewerCount_)
            {
                this->liveViewerCount_ = viewerCount;
                this->viewerCountChanged.invoke();
            }

            int deliveredMessageCount = 0;
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
                deliveredMessageCount++;
            }

            this->skipInitialBacklog_ = false;
            if (deliveredMessageCount > 0)
            {
                this->activePollStreak_ =
                    std::min(this->activePollStreak_ + 1, 3);
            }
            else
            {
                this->activePollStreak_ = 0;
            }

            const auto continuations = continuation["continuations"].toArray();
            int nextDelay = 1000;
            bool updatedContinuation = false;
            for (const auto &continuationValue : continuations)
            {
                auto continuationObject = continuationValue.toObject();
                const auto timed =
                    continuationObject["timedContinuationData"].toObject();
                if (!timed.isEmpty())
                {
                    this->continuation_ = timed["continuation"].toString();
                    nextDelay = adjustedYouTubePollDelay(
                        timed["timeoutMs"].toInt(nextDelay),
                        requestTimer->elapsed(), deliveredMessageCount,
                        this->activePollStreak_);
                    updatedContinuation = !this->continuation_.isEmpty();
                    break;
                }

                const auto invalidation =
                    continuationObject["invalidationContinuationData"]
                        .toObject();
                if (!invalidation.isEmpty())
                {
                    this->continuation_ =
                        invalidation["continuation"].toString();
                    nextDelay = adjustedYouTubePollDelay(
                        invalidation["timeoutMs"].toInt(nextDelay),
                        requestTimer->elapsed(), deliveredMessageCount,
                        this->activePollStreak_);
                    updatedContinuation = !this->continuation_.isEmpty();
                    break;
                }
            }

            if (!updatedContinuation)
            {
                this->recoverLiveChat(
                    "YouTube live chat continuation expired. Reconnecting.",
                    YOUTUBE_RECONNECT_DELAY_MS);
                return;
            }

            this->failureReported_ = false;
            this->setLive(true);
            if (this->liveChatSessionRefreshTimer_.isValid() &&
                this->liveChatSessionRefreshTimer_.elapsed() >=
                    YOUTUBE_SESSION_REFRESH_MS)
            {
                this->fetchLiveChatPage(false);
                return;
            }
            this->schedulePoll(nextDelay);
            }))
        .onError(guardedCallback(this->lifetimeGuard_,
                                 [this](NetworkResult) {
            if (!this->running_)
            {
                return;
            }

            this->recoverLiveChat("YouTube live chat polling failed. "
                                  "Reconnecting.",
                                  YOUTUBE_RECONNECT_DELAY_MS);
                                 }))
        .execute();
}

void YouTubeLiveChat::schedulePoll(int delayMs)
{
    QTimer::singleShot(delayMs,
                       guardedCallback(this->lifetimeGuard_, [this] {
                           if (!this->running_)
                           {
                               return;
                           }
                           this->poll();
                       }));
}

void YouTubeLiveChat::scheduleHealthCheck(int delayMs)
{
    QTimer::singleShot(delayMs, guardedCallback(this->lifetimeGuard_, [this] {
                           if (!this->running_)
                           {
                               return;
                           }

                           const bool shouldMonitor =
                               this->live_ &&
                               !this->continuation_.isEmpty() &&
                               !this->apiKey_.isEmpty();
                           if (shouldMonitor &&
                               this->liveChatProgressTimer_.isValid() &&
                               this->liveChatProgressTimer_.elapsed() >=
                                   YOUTUBE_STALL_TIMEOUT_MS)
                           {
                               this->recoverLiveChat(
                                   "YouTube live chat stalled. Reconnecting.",
                                   YOUTUBE_RECONNECT_DELAY_MS);
                           }

                           this->scheduleHealthCheck(
                               YOUTUBE_HEALTH_CHECK_INTERVAL_MS);
                       }));
}

void YouTubeLiveChat::scheduleResolve(int delayMs)
{
    QTimer::singleShot(delayMs,
                       guardedCallback(this->lifetimeGuard_, [this] {
                           if (!this->running_)
                           {
                               return;
                           }

                           this->resolveVideoId();
                       }));
}

void YouTubeLiveChat::recoverLiveChat(QString text, int retryDelayMs)
{
    // Empty continuation, stalled progress, or a poll error all mean the
    // chat session we were tracking is gone. Drop live_ here so the rest
    // of the app stops believing the stream is live while we rebootstrap.
    // If the next fetchLiveChatPage finds a fresh chat session, setLive(true)
    // restores it.
    this->setLive(false);
    this->liveViewerCount_ = 0;
    this->continuation_.clear();
    this->activePollStreak_ = 0;
    this->liveChatSessionRefreshTimer_.invalidate();
    this->liveChatProgressTimer_.invalidate();
    this->resetInnertubeContext();
    this->setStatusText(std::move(text), !this->failureReported_);
    this->failureReported_ = true;

    QTimer::singleShot(retryDelayMs,
                       guardedCallback(this->lifetimeGuard_, [this] {
                           if (!this->running_)
                           {
                               return;
                           }

                           if (this->live_ && !this->videoId_.isEmpty())
                           {
                               this->fetchLiveChatPage(false);
                               return;
                           }

                           this->resolveVideoId();
                       }));
}

void YouTubeLiveChat::waitForNextLive(QString text, int retryDelayMs)
{
    this->setLive(false);
    this->videoId_.clear();
    this->continuation_.clear();
    this->liveTitle_.clear();
    this->liveStartedAt_ = {};
    this->liveViewerCount_ = 0;
    this->seenMessageIds_.clear();
    this->failureReported_ = false;
    this->skipInitialBacklog_ = false;
    this->activePollStreak_ = 0;
    this->liveChatSessionRefreshTimer_.invalidate();
    this->liveChatProgressTimer_.invalidate();
    this->setStatusText(std::move(text));
    this->scheduleResolve(retryDelayMs);
}

void YouTubeLiveChat::resetInnertubeContext()
{
    this->apiKey_.clear();
    this->clientVersion_.clear();
    this->visitorData_.clear();
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
        return value.section('/', 0, 0).trimmed();
    }

    if (isLikelyChannelId(value))
    {
        return value;
    }

    if (!value.contains("://") && !value.contains('/') &&
        !value.contains('\\'))
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
        return path.section('/', 1, 1).trimmed();
    }
    if (host.endsWith("youtube.com") && path.startsWith("/channel/"))
    {
        const auto channelId = path.section('/', 2, 2).trimmed();
        if (isLikelyChannelId(channelId))
        {
            return channelId;
        }
    }

    if (host.endsWith("youtube.com"))
    {
        const auto firstSegment = path.section('/', 1, 1).trimmed();
        if (firstSegment == "c" || firstSegment == "user")
        {
            return path.section('/', 2, 2).trimmed();
        }

        if (!firstSegment.isEmpty() && firstSegment != "watch" &&
            firstSegment != "shorts" && firstSegment != "embed" &&
            firstSegment != "live" && firstSegment != "playlist" &&
            firstSegment != "results" && firstSegment != "feed")
        {
            return QUrl::fromPercentEncoding(firstSegment.toUtf8()).trimmed();
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

QString YouTubeLiveChat::sourceStreamsPath(const QString &source)
{
    if (source.startsWith('@'))
    {
        return QString("/%1/streams").arg(source);
    }

    if (isLikelyChannelId(source))
    {
        return QString("/channel/%1/streams").arg(source);
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
            extractContinuationFromObject(continuationValue.toObject());
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
            extractContinuationFromObject(
                itemValue.toObject()["continuation"].toObject());
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

QString YouTubeLiveChat::extractEmbedLiveVideoId(const QString &html)
{
    return extractFirstMatch(
        html,
        {R"yt(<link rel="canonical" href="https://www\.youtube\.com/watch\?v=([A-Za-z0-9_-]{11})")yt",
         R"yt(<meta property="og:url" content="https://www\.youtube\.com/watch\?v=([A-Za-z0-9_-]{11})")yt",
         R"yt(itemprop="url" content="https://www\.youtube\.com/watch\?v=([A-Za-z0-9_-]{11})")yt",
         R"yt("canonicalBaseUrl":"\\/watch\\?v=([A-Za-z0-9_-]{11})")yt",
         R"yt(https://www\.youtube\.com/embed/([A-Za-z0-9_-]{11}))yt",
         R"yt(/embed/([A-Za-z0-9_-]{11}))yt",
         R"yt(\\/embed\\/([A-Za-z0-9_-]{11}))yt"});
}

QString YouTubeLiveChat::extractLiveVideoId(const QString &html,
                                            bool allowPageCanonical)
{
    if (allowPageCanonical)
    {
        const auto canonicalVideoId = extractFirstMatch(
            html, {R"yt(<link rel="canonical" href="https://www\.youtube\.com/watch\?v=([A-Za-z0-9_-]{11})")yt",
                   R"yt(<meta property="og:url" content="https://www\.youtube\.com/watch\?v=([A-Za-z0-9_-]{11})")yt",
                   R"yt(itemprop="url" content="https://www\.youtube\.com/watch\?v=([A-Za-z0-9_-]{11})")yt"});
        if (!canonicalVideoId.isEmpty())
        {
            return canonicalVideoId;
        }
    }

    const auto lockupVideoId = extractLiveLockupVideoId(html);
    if (!lockupVideoId.isEmpty())
    {
        return lockupVideoId;
    }

    const auto videoRendererVideoId = extractLiveVideoRendererVideoId(html);
    if (!videoRendererVideoId.isEmpty())
    {
        return videoRendererVideoId;
    }

    const auto watchEndpointVideoId = extractLiveWatchEndpointVideoId(html);
    if (!watchEndpointVideoId.isEmpty())
    {
        return watchEndpointVideoId;
    }

    return {};
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

QDateTime YouTubeLiveChat::extractLiveStartTime(const QJsonObject &nextResponse)
{
    const auto liveBroadcastDetails =
        nextResponse["microformat"]
            .toObject()["playerMicroformatRenderer"]
            .toObject()["liveBroadcastDetails"]
            .toObject();
    return parseYouTubeIsoDateTime(
        liveBroadcastDetails["startTimestamp"].toString());
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
            text.append(youtubeEmojiFallbackText(emoji));
        }
    }

    return text.trimmed();
}

uint64_t YouTubeLiveChat::parseViewerCount(const QJsonValue &value)
{
    const auto text = parseText(value);
    if (text.isEmpty())
    {
        return 0;
    }

    static const QRegularExpression countRegex(R"(([\d,.]+))");
    const auto match = countRegex.match(text);
    if (!match.hasMatch())
    {
        return 0;
    }

    auto digits = match.captured(1);
    digits.remove(',');
    digits.remove('.');

    bool ok = false;
    const auto count = digits.toULongLong(&ok);
    return ok ? count : 0;
}

QString compactMembershipGiftText(const QString &text, const QString &author)
{
    static const QRegularExpression countRegex(R"((\d+))");
    const auto match = countRegex.match(text);
    if (!match.hasMatch() ||
        !text.contains(QStringLiteral("membership"), Qt::CaseInsensitive))
    {
        return text;
    }

    const auto count = match.captured(1).toInt();
    const auto unit = count == 1 ? QStringLiteral("membership")
                                : QStringLiteral("memberships");
    return QStringLiteral("%1 gifted %2 %3")
        .arg(author, QString::number(count), unit);
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
    std::vector<QJsonValue> renderedTextParts;
    QJsonValue authorNameValue = renderer["authorName"];
    QString authorId = renderer["authorExternalChannelId"].toString();
    MessageFlags flags;
    bool compactMembershipGift = false;

    if (rendererName == "liveChatTextMessageRenderer")
    {
        const auto message = renderer["message"];
        text = parseText(message);
        renderedTextParts.emplace_back(message);
    }
    else if (rendererName == "liveChatPaidMessageRenderer")
    {
        flags.set(MessageFlag::ElevatedMessage);
        const auto purchaseAmount = renderer["purchaseAmountText"];
        text = parseText(purchaseAmount);
        if (!text.isEmpty())
        {
            renderedTextParts.emplace_back(purchaseAmount);
        }

        const auto message = renderer["message"];
        const auto body = parseText(message);
        if (!body.isEmpty())
        {
            if (!text.isEmpty())
            {
                text += " ";
            }
            text += body;
            renderedTextParts.emplace_back(message);
        }
    }
    else if (rendererName == "liveChatPaidStickerRenderer")
    {
        flags.set(MessageFlag::ElevatedMessage);
        const auto purchaseAmount = renderer["purchaseAmountText"];
        text = parseText(purchaseAmount);
        renderedTextParts.emplace_back(purchaseAmount);
    }
    else if (rendererName == "liveChatMembershipItemRenderer")
    {
        flags.set(MessageFlag::Subscription);
        const auto headerSubtext = renderer["headerSubtext"];
        text = parseText(headerSubtext);
        if (!text.isEmpty())
        {
            renderedTextParts.emplace_back(headerSubtext);
        }

        const auto message = renderer["message"];
        const auto body = parseText(message);
        if (!body.isEmpty())
        {
            if (!text.isEmpty())
            {
                text += " ";
            }
            text += body;
            renderedTextParts.emplace_back(message);
        }
    }
    else if (rendererName ==
             "liveChatSponsorshipsGiftPurchaseAnnouncementRenderer")
    {
        flags.set(MessageFlag::Subscription);
        const auto header =
            renderer["header"].toObject()["liveChatSponsorshipsHeaderRenderer"]
                .toObject();
        authorNameValue = header["authorName"];
        const auto primaryText = header["primaryText"];
        text = parseText(primaryText);
        renderedTextParts.emplace_back(primaryText);
        compactMembershipGift = true;
        if (authorId.isEmpty())
        {
            authorId = header["authorExternalChannelId"].toString();
        }
    }
    else if (rendererName ==
             "liveChatSponsorshipsGiftRedemptionAnnouncementRenderer")
    {
        return nullptr;
    }
    else if (rendererName == "giftMessageViewModel")
    {
        // Jewels gift authorName/text use {content, styleRuns} rather than
        // the standard runs/simpleText shape; wrap the content in a
        // simpleText object so parseText() handles it uniformly below.
        flags.set(MessageFlag::ElevatedMessage);
        auto authorContent =
            renderer["authorName"].toObject()["content"].toString().trimmed();
        if (authorContent.startsWith(u'@'))
        {
            authorContent = authorContent.mid(1);
        }
        authorNameValue = QJsonObject{{"simpleText", authorContent}};
        text = renderer["text"].toObject()["content"].toString().trimmed();
    }
    else if (rendererName == "liveChatPlaceholderItemRenderer" ||
             rendererName == "liveChatViewerEngagementMessageRenderer")
    {
        // Placeholder items are filler slots that precede a real message;
        // engagement renderers are YouTube's welcome/rules system notices.
        return nullptr;
    }
    else
    {
        // Cap diagnostic dumps at five per session so a stream with many
        // unknown renderer types can't flood the log.
        static std::atomic<int> warningCount{0};
        const int n = warningCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 5)
        {
            qCWarning(chatterinoYouTube).nospace()
                << "Unhandled live chat renderer: " << rendererName;
            qCWarning(chatterinoYouTube)
                << QJsonDocument(renderer).toJson(QJsonDocument::Compact);
        }
        else if (n == 5)
        {
            qCWarning(chatterinoYouTube)
                << "Further unhandled-renderer warnings suppressed for "
                   "this session";
        }
        return nullptr;
    }

    const auto author = parseText(authorNameValue);
    if (author.isEmpty())
    {
        return nullptr;
    }
    if (compactMembershipGift)
    {
        text = compactMembershipGiftText(text, author);
    }

    MessageBuilder builder;
    builder->flags = flags;
    builder->platform = MessagePlatform::YouTube;
    builder->id = renderer["id"].toString();
    builder->loginName = author;
    builder->displayName = author;
    builder->localizedName = author;
    builder->userID = authorId;
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
    if (compactMembershipGift || renderedTextParts.empty())
    {
        appendSimpleWords(builder, text);
    }
    else
    {
        appendYouTubeTextParts(builder, renderedTextParts, text);
    }
    return builder.release();
}

}  // namespace chatterino
