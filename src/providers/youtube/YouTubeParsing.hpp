// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QDateTime>
#include <QString>

#include <cstdint>

class QJsonObject;
class QJsonValue;

namespace chatterino {

// Stateless parsing / normalization helpers for the YouTube InnerTube read
// path. These were extracted verbatim from YouTubeLiveChat.cpp so they can be
// unit-tested in isolation from that networking-heavy translation unit. Every
// function here is a pure function of its arguments: no member access, no
// timers, no network, no global state.

bool isLikelyChannelIdValue(const QString &value);
bool isLikelyVideoIdValue(const QString &value);
QString extractFirstBrowseChannelId(const QJsonValue &value);

QString normalizeYouTubeTextRunUrl(QString url, bool unwrapRedirect = true);
QString youtubeNavigationEndpointUrl(const QJsonObject &endpoint);

QDateTime parseTimestampUsec(const QString &timestampUsec);
QDateTime parseYouTubeIsoDateTime(const QString &value);

uint64_t parseYouTubeViewerCountText(QString text);
bool isLiveViewerCountText(const QString &text);

QJsonObject extractLiveBroadcastDetails(const QJsonObject &nextResponse);
bool isEndedOrOfflineLiveBroadcast(const QJsonObject &nextResponse);
bool extractIsLiveFromNextResponse(const QJsonObject &json);

// Three-way liveness read of an InnerTube /next response. NotLive requires a
// POSITIVE signal (explicit isLive:false, a waiting-room/scheduled marker, or
// a recognized watch page whose view-count block carries no live flag at
// all); a response we can't recognize (consent/throttle page, empty JSON,
// A/B shape change) is Unknown, NOT NotLive - callers that penalize a
// videoId on NotLive must not do so on a transient garbage response.
enum class YouTubeLiveness : std::uint8_t {
    Live,
    NotLive,
    Unknown,
};
YouTubeLiveness classifyLivenessFromNextResponse(const QJsonObject &json);

QString extractYouTubeChannelHandle(const QString &html,
                                    bool ownerScopedOnly = false);

bool containsActiveLiveMarker(const QString &text);
bool jsonContainsLiveMarker(const QJsonValue &value);

QString extractLiveLockupVideoId(const QString &html);
QString extractLiveVideoRendererVideoId(const QString &html);
QString extractLiveWatchEndpointVideoId(const QString &html);
bool isLikelyJsonVideoId(const QString &value);
QString extractLiveVideoIdFromJson(const QJsonValue &value);

QString extractContinuationFromObject(const QJsonObject &object);
QString extractLiveChatContinuationFromJson(const QJsonValue &value);

}  // namespace chatterino
