// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeParsing.hpp"

#include "Test.hpp"

#include <QJsonDocument>
#include <QJsonObject>

using namespace chatterino;

namespace {

QJsonObject parseObject(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

}  // namespace

TEST(YouTubeParsing, isLikelyChannelIdValueAcceptsWellFormedId)
{
    EXPECT_TRUE(isLikelyChannelIdValue("UC-lHJZR3Gqxm24_Vd_AJ5Yw"));
    EXPECT_FALSE(isLikelyChannelIdValue("UC123"));
    EXPECT_FALSE(isLikelyChannelIdValue("dQw4w9WgXcQ"));
}

TEST(YouTubeParsing, isLikelyVideoIdValueRejectsLiveStreamSentinel)
{
    EXPECT_TRUE(isLikelyVideoIdValue("dQw4w9WgXcQ"));
    EXPECT_FALSE(isLikelyVideoIdValue("live_stream"));
    EXPECT_FALSE(isLikelyVideoIdValue("short"));
}

TEST(YouTubeParsing, parseYouTubeViewerCountTextHandlesSuffixesAndSeparators)
{
    EXPECT_EQ(parseYouTubeViewerCountText("1,234"), 1234u);
    EXPECT_EQ(parseYouTubeViewerCountText("12K"), 12000u);
    EXPECT_EQ(parseYouTubeViewerCountText("1.2K"), 1200u);
    EXPECT_EQ(parseYouTubeViewerCountText("3M"), 3000000u);
    EXPECT_EQ(parseYouTubeViewerCountText("42"), 42u);
    EXPECT_EQ(parseYouTubeViewerCountText(""), 0u);
}

TEST(YouTubeParsing, containsActiveLiveMarkerDetectsLiveBadges)
{
    EXPECT_TRUE(containsActiveLiveMarker(R"({"style":"BADGE_STYLE_TYPE_LIVE_NOW"})"));
    EXPECT_TRUE(containsActiveLiveMarker(R"("isLiveNow":true)"));
    EXPECT_FALSE(containsActiveLiveMarker(R"("style":"BADGE_STYLE_TYPE_DEFAULT")"));
}

TEST(YouTubeParsing, extractFirstBrowseChannelIdWalksNavigationEndpoint)
{
    const auto owner = parseObject(R"({
        "navigationEndpoint": {
            "browseEndpoint": {"browseId": "UC-lHJZR3Gqxm24_Vd_AJ5Yw"}
        }
    })");
    EXPECT_EQ(extractFirstBrowseChannelId(owner),
              "UC-lHJZR3Gqxm24_Vd_AJ5Yw");
    EXPECT_TRUE(extractFirstBrowseChannelId(QJsonObject{}).isEmpty());
}

TEST(YouTubeParsing, extractContinuationFromObjectReadsKnownKeys)
{
    const auto obj =
        parseObject(R"({"timedContinuationData": {"continuation": "CONT123"}})");
    EXPECT_EQ(extractContinuationFromObject(obj), "CONT123");
    EXPECT_TRUE(extractContinuationFromObject(QJsonObject{}).isEmpty());
}

TEST(YouTubeParsing, extractIsLiveFromNextResponseReadsViewCountRenderer)
{
    const auto liveNow = parseObject(R"({
        "contents": {"twoColumnWatchNextResults": {"results": {"results": {
            "contents": [{"videoPrimaryInfoRenderer": {
                "viewCount": {"videoViewCountRenderer": {"isLive": true}}}}]
        }}}}
    })");
    EXPECT_TRUE(extractIsLiveFromNextResponse(liveNow));
    EXPECT_FALSE(extractIsLiveFromNextResponse(QJsonObject{}));
}

TEST(YouTubeParsing, parseYouTubeIsoDateTimeParsesValidTimestamps)
{
    EXPECT_TRUE(parseYouTubeIsoDateTime("2026-01-15T12:00:00Z").isValid());
    EXPECT_FALSE(parseYouTubeIsoDateTime("").isValid());
    EXPECT_FALSE(parseYouTubeIsoDateTime("not-a-date").isValid());
}

TEST(YouTubeParsing, normalizeYouTubeTextRunUrlRejectsDangerousSchemes)
{
    EXPECT_TRUE(normalizeYouTubeTextRunUrl("javascript:alert(1)").isEmpty());
    EXPECT_TRUE(normalizeYouTubeTextRunUrl(
                    "data:text/html;base64,PHNjcmlwdD5hbGVydCgxKTwvc2NyaXB0Pg==")
                    .isEmpty());
    EXPECT_TRUE(normalizeYouTubeTextRunUrl("ftp://x").isEmpty());
}

TEST(YouTubeParsing, normalizeYouTubeTextRunUrlPassesThroughHttpAndHttps)
{
    EXPECT_EQ(normalizeYouTubeTextRunUrl("http://example.com/foo"),
              "http://example.com/foo");
    EXPECT_EQ(normalizeYouTubeTextRunUrl("https://example.com/foo?a=1"),
              "https://example.com/foo?a=1");
}

TEST(YouTubeParsing, normalizeYouTubeTextRunUrlExpandsRelativePrefixes)
{
    EXPECT_EQ(normalizeYouTubeTextRunUrl("//example.com/path"),
              "https://example.com/path");
    EXPECT_EQ(normalizeYouTubeTextRunUrl("/watch?v=dQw4w9WgXcQ"),
              "https://www.youtube.com/watch?v=dQw4w9WgXcQ");
}

TEST(YouTubeParsing, normalizeYouTubeTextRunUrlUnwrapsRedirectQueryParam)
{
    EXPECT_EQ(
        normalizeYouTubeTextRunUrl(
            "https://www.youtube.com/redirect?q=https%3A%2F%2Fexample.com%2Fpage"),
        "https://example.com/page");

    // Empty q= has nothing to unwrap to, so the redirect URL itself is returned.
    EXPECT_EQ(normalizeYouTubeTextRunUrl("https://www.youtube.com/redirect?q="),
              "https://www.youtube.com/redirect?q=");

    // Only one level of unwrapping happens: a redirect whose q= target is
    // itself a (double-encoded) /redirect URL comes back still wrapped,
    // not recursively unwrapped.
    EXPECT_EQ(
        normalizeYouTubeTextRunUrl(
            "https://www.youtube.com/redirect?q=https%3A%2F%2Fwww.youtube.com%2Fredirect%3Fq%3Dhttps%253A%252F%252Fexample.com"),
        "https://www.youtube.com/redirect?q=https%3A%2F%2Fexample.com");
}

TEST(YouTubeParsing, youtubeNavigationEndpointUrlPrefersUrlEndpoint)
{
    const auto endpoint = parseObject(R"({
        "urlEndpoint": {"url": "https://example.com/from-url-endpoint"}
    })");
    EXPECT_EQ(youtubeNavigationEndpointUrl(endpoint),
              "https://example.com/from-url-endpoint");
}

TEST(YouTubeParsing, youtubeNavigationEndpointUrlFallsBackToWatchEndpoint)
{
    const auto endpoint = parseObject(R"({
        "watchEndpoint": {"videoId": "dQw4w9WgXcQ"}
    })");
    EXPECT_EQ(youtubeNavigationEndpointUrl(endpoint),
              "https://www.youtube.com/watch?v=dQw4w9WgXcQ");
}

TEST(YouTubeParsing, youtubeNavigationEndpointUrlFallsBackToBrowseEndpoint)
{
    const auto endpoint = parseObject(R"({
        "browseEndpoint": {"browseId": "UC-lHJZR3Gqxm24_Vd_AJ5Yw"}
    })");
    EXPECT_EQ(youtubeNavigationEndpointUrl(endpoint),
              "https://www.youtube.com/channel/UC-lHJZR3Gqxm24_Vd_AJ5Yw");
}

TEST(YouTubeParsing, youtubeNavigationEndpointUrlReturnsEmptyWhenNoEndpointPresent)
{
    EXPECT_TRUE(youtubeNavigationEndpointUrl(QJsonObject{}).isEmpty());
    EXPECT_TRUE(
        youtubeNavigationEndpointUrl(parseObject(R"({"someOtherField": true})"))
            .isEmpty());
}
