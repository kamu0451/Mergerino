// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeParsing.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cstdint>

namespace chatterino {

bool isLikelyChannelIdValue(const QString &value)
{
    static const QRegularExpression channelIdRegex("^UC[A-Za-z0-9_-]{22}$");
    return channelIdRegex.match(value.trimmed()).hasMatch();
}

bool isLikelyVideoIdValue(const QString &value)
{
    static const QRegularExpression videoIdRegex("^[A-Za-z0-9_-]{11}$");

    const auto trimmed = value.trimmed();
    return trimmed != QStringLiteral("live_stream") &&
        videoIdRegex.match(trimmed).hasMatch();
}

QString extractFirstBrowseChannelId(const QJsonValue &value)
{
    if (value.isObject())
    {
        const auto object = value.toObject();
        const auto endpoint =
            object["navigationEndpoint"].toObject()["browseEndpoint"].toObject();
        const auto browseId = endpoint["browseId"].toString();
        if (isLikelyChannelIdValue(browseId))
        {
            return browseId;
        }

        const auto directBrowseId =
            object["browseEndpoint"].toObject()["browseId"].toString();
        if (isLikelyChannelIdValue(directBrowseId))
        {
            return directBrowseId;
        }

        for (const auto &key : object.keys())
        {
            const auto result = extractFirstBrowseChannelId(object[key]);
            if (!result.isEmpty())
            {
                return result;
            }
        }
    }
    else if (value.isArray())
    {
        for (const auto &item : value.toArray())
        {
            const auto result = extractFirstBrowseChannelId(item);
            if (!result.isEmpty())
            {
                return result;
            }
        }
    }

    return {};
}

QString normalizeYouTubeTextRunUrl(QString url, bool unwrapRedirect)
{
    url = url.trimmed();
    if (url.isEmpty())
    {
        return {};
    }

    if (url.startsWith("//"))
    {
        url.prepend("https:");
    }
    else if (url.startsWith('/'))
    {
        url.prepend("https://www.youtube.com");
    }
    else if (!url.contains("://") &&
             (url.startsWith("www.", Qt::CaseInsensitive) ||
              url.startsWith("youtube.com/", Qt::CaseInsensitive) ||
              url.startsWith("m.youtube.com/", Qt::CaseInsensitive) ||
              url.startsWith("studio.youtube.com/", Qt::CaseInsensitive)))
    {
        url.prepend("https://");
    }

    const QUrl parsed(url);
    const auto scheme = parsed.scheme().toLower();
    if (!parsed.isValid() || parsed.host().isEmpty() ||
        (scheme != "http" && scheme != "https"))
    {
        return {};
    }

    if (unwrapRedirect && parsed.host().endsWith("youtube.com") &&
        parsed.path() == "/redirect")
    {
        const QUrlQuery query(parsed);
        // FullyDecoded: YouTube percent-encodes the redirect target, and the
        // default PrettyDecoded leaves ':' and '/' encoded, which makes the
        // target unparseable as a URL and silently skips the unwrap.
        const auto target =
            query.queryItemValue("q", QUrl::FullyDecoded).trimmed();
        if (!target.isEmpty())
        {
            const auto normalizedTarget =
                normalizeYouTubeTextRunUrl(target, false);
            if (!normalizedTarget.isEmpty())
            {
                return normalizedTarget;
            }
        }
    }

    return parsed.toString();
}

QString youtubeNavigationEndpointUrl(const QJsonObject &endpoint)
{
    const auto webUrl = endpoint["commandMetadata"]
                            .toObject()["webCommandMetadata"]
                            .toObject()["url"]
                            .toString();
    if (const auto url = normalizeYouTubeTextRunUrl(webUrl); !url.isEmpty())
    {
        return url;
    }

    const auto endpointUrl =
        endpoint["urlEndpoint"].toObject()["url"].toString();
    if (const auto url = normalizeYouTubeTextRunUrl(endpointUrl);
        !url.isEmpty())
    {
        return url;
    }

    const auto watchEndpoint = endpoint["watchEndpoint"].toObject();
    const auto videoId = watchEndpoint["videoId"].toString().trimmed();
    if (isLikelyVideoIdValue(videoId))
    {
        return QStringLiteral("https://www.youtube.com/watch?v=%1")
            .arg(videoId);
    }

    const auto browseEndpoint = endpoint["browseEndpoint"].toObject();
    const auto canonicalBaseUrl =
        browseEndpoint["canonicalBaseUrl"].toString().trimmed();
    if (const auto url = normalizeYouTubeTextRunUrl(canonicalBaseUrl);
        !url.isEmpty())
    {
        return url;
    }

    const auto browseId = browseEndpoint["browseId"].toString().trimmed();
    if (isLikelyChannelIdValue(browseId))
    {
        return QStringLiteral("https://www.youtube.com/channel/%1")
            .arg(browseId);
    }

    return {};
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

uint64_t parseYouTubeViewerCountText(QString text)
{
    text = text.trimmed();
    if (text.isEmpty())
    {
        return 0;
    }

    static const QRegularExpression countRegex(
        R"((\d[\d\s,.]*)(?:\s*([kmb]))?)",
        QRegularExpression::CaseInsensitiveOption);
    const auto match = countRegex.match(text);
    if (!match.hasMatch())
    {
        return 0;
    }

    auto number = match.captured(1);
    number.remove(' ');
    number.remove('\t');
    number.remove(QChar(0x00A0));
    number.remove(QChar(0x202F));

    const auto suffix = match.captured(2).toLower();
    if (!suffix.isEmpty())
    {
        number.remove(',');

        bool ok = false;
        const auto compactCount = number.toDouble(&ok);
        if (!ok || compactCount <= 0)
        {
            return 0;
        }

        double multiplier = 1;
        if (suffix == QStringLiteral("k"))
        {
            multiplier = 1000;
        }
        else if (suffix == QStringLiteral("m"))
        {
            multiplier = 1000000;
        }
        else if (suffix == QStringLiteral("b"))
        {
            multiplier = 1000000000;
        }

        return static_cast<uint64_t>(compactCount * multiplier + 0.5);
    }

    number.remove(',');
    number.remove('.');

    bool ok = false;
    const auto count = number.toULongLong(&ok);
    return ok ? count : 0;
}

bool isLiveViewerCountText(const QString &text)
{
    const auto lower = text.toLower();
    return lower.contains(QStringLiteral("watching")) ||
           lower.contains(QStringLiteral("viewer"));
}

QJsonObject extractLiveBroadcastDetails(const QJsonObject &nextResponse)
{
    return nextResponse["microformat"]
        .toObject()["playerMicroformatRenderer"]
        .toObject()["liveBroadcastDetails"]
        .toObject();
}

bool isEndedOrOfflineLiveBroadcast(const QJsonObject &nextResponse)
{
    const auto liveBroadcastDetails = extractLiveBroadcastDetails(nextResponse);
    if (liveBroadcastDetails.isEmpty())
    {
        return false;
    }

    if (!liveBroadcastDetails["endTimestamp"].toString().trimmed().isEmpty())
    {
        return true;
    }

    const auto isLiveNow = liveBroadcastDetails["isLiveNow"];
    return isLiveNow.isBool() && !isLiveNow.toBool();
}

bool containsActiveLiveMarker(const QString &text)
{
    return text.contains(QStringLiteral("THUMBNAIL_OVERLAY_BADGE_STYLE_LIVE")) ||
           text.contains(QStringLiteral(
               "THUMBNAIL_OVERLAY_TIME_STATUS_STYLE_LIVE")) ||
           text.contains(QStringLiteral("BADGE_STYLE_TYPE_LIVE_NOW")) ||
           text.contains(QStringLiteral(R"("style":"LIVE")")) ||
           text.contains(QStringLiteral(R"("simpleText":"LIVE")")) ||
           text.contains(QStringLiteral(R"("label":"LIVE")")) ||
           text.contains(QStringLiteral(R"("isLive":true)")) ||
           text.contains(QStringLiteral(R"("isLiveNow":true)")) ||
           text.contains(QStringLiteral(" watching now"), Qt::CaseInsensitive) ||
           text.contains(QStringLiteral("started streaming"),
                         Qt::CaseInsensitive);
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

        if (!containsActiveLiveMarker(block))
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

        if (!containsActiveLiveMarker(block))
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

        if (containsActiveLiveMarker(block))
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

    const auto trimmed = value.trimmed();
    return trimmed != QStringLiteral("live_stream") &&
        videoIdRegex.match(trimmed).hasMatch();
}

bool jsonContainsLiveMarker(const QJsonValue &value)
{
    if (value.isString())
    {
        const auto text = value.toString();
        return containsActiveLiveMarker(text);
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
    for (const auto &key :
         {QStringLiteral("isLive"), QStringLiteral("isLiveNow")})
    {
        const auto marker = object[key];
        if (marker.isBool() && marker.toBool())
        {
            return true;
        }
    }

    for (auto it = object.begin(); it != object.end(); ++it)
    {
        if (it.value().isString())
        {
            const auto key = it.key();
            const auto text = it.value().toString().trimmed();
            if ((key == QStringLiteral("style") &&
                text == QStringLiteral("LIVE")) ||
                ((key == QStringLiteral("simpleText") ||
                  key == QStringLiteral("label")) &&
                 (text.compare(QStringLiteral("LIVE"), Qt::CaseInsensitive) ==
                      0 ||
                  text.compare(QStringLiteral("LIVE NOW"),
                               Qt::CaseInsensitive) == 0 ||
                  text.contains(QStringLiteral("watching now"),
                                Qt::CaseInsensitive) ||
                  text.contains(QStringLiteral("started streaming"),
                                Qt::CaseInsensitive))))
            {
                return true;
            }
        }

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

// Walks the InnerTube /next response for a definitive "live now" signal.
// The watch-page microformat path used by /player isn't present here, so
// we look for the videoPrimaryInfoRenderer's view-count block, which carries
// `isLive: true` exactly when the broadcast is live. As a backup we also
// honour `videoDetails.isLiveContent` + `videoDetails.isLive` if YouTube
// changes the renderer shape. Conservative: returns false on any structure
// we don't recognise so we don't false-positive recently-ended streams.
bool extractIsLiveFromNextResponse(const QJsonObject &json)
{
    const auto contents = json["contents"]
                              .toObject()["twoColumnWatchNextResults"]
                              .toObject()["results"]
                              .toObject()["results"]
                              .toObject()["contents"]
                              .toArray();
    for (const auto &itemValue : contents)
    {
        const auto primary =
            itemValue.toObject()["videoPrimaryInfoRenderer"].toObject();
        if (primary.isEmpty())
        {
            continue;
        }
        const auto viewCount = primary["viewCount"].toObject();
        const auto renderer = viewCount["videoViewCountRenderer"].toObject();
        if (renderer.contains("isLive"))
        {
            return renderer["isLive"].toBool(false);
        }
    }

    const auto videoDetails = json["videoDetails"].toObject();
    if (videoDetails.contains("isLive"))
    {
        return videoDetails["isLive"].toBool(false);
    }

    return false;
}

}  // namespace chatterino
