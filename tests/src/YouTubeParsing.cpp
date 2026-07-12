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

TEST(YouTubeParsing, extractIsLiveFromNextResponseRejectsWaitingRooms)
{
    // Real shape captured from an ended stream that reverted to a stale
    // waiting room (videoId 0tBLmmIWrTQ): the view-count renderer still says
    // "isLive": true, but the counter reads "1 waiting" and the dateText is
    // "Scheduled for ...". Trusting isLive alone latched the channel live.
    const auto waitingRoom = parseObject(R"({
        "contents": {"twoColumnWatchNextResults": {"results": {"results": {
            "contents": [{"videoPrimaryInfoRenderer": {
                "dateText": {"simpleText": "Scheduled for Jun 6, 2026"},
                "viewCount": {"videoViewCountRenderer": {
                    "viewCount": {"runs": [{"text": "1 waiting"}]},
                    "isLive": true,
                    "originalViewCount": "1"}}}}]
        }}}}
    })");
    EXPECT_FALSE(extractIsLiveFromNextResponse(waitingRoom));

    // The same renderer with an actively-broadcasting counter stays live.
    const auto watchingNow = parseObject(R"({
        "contents": {"twoColumnWatchNextResults": {"results": {"results": {
            "contents": [{"videoPrimaryInfoRenderer": {
                "viewCount": {"videoViewCountRenderer": {
                    "viewCount": {"runs": [{"text": "1,234 watching now"}]},
                    "isLive": true}}}}]
        }}}}
    })");
    EXPECT_TRUE(extractIsLiveFromNextResponse(watchingNow));

    // videoDetails fallback: upcoming broadcasts carry isLive true too.
    const auto upcomingDetails = parseObject(R"({
        "videoDetails": {"isLive": true, "isUpcoming": true}
    })");
    EXPECT_FALSE(extractIsLiveFromNextResponse(upcomingDetails));
}

TEST(YouTubeParsing, classifyLivenessDistinguishesNotLiveFromUnknown)
{
    // An unrecognized response (consent/throttle page parsed to empty JSON,
    // or a shape without any liveness signal) must be Unknown, not NotLive:
    // callers penalize a videoId on NotLive and a transient garbage response
    // must not lock a live stream out for the cooldown window.
    EXPECT_EQ(classifyLivenessFromNextResponse(QJsonObject{}),
              YouTubeLiveness::Unknown);
    EXPECT_EQ(classifyLivenessFromNextResponse(
                  parseObject(R"({"someUnrelatedShape": true})")),
              YouTubeLiveness::Unknown);

    // A recognized watch page whose view-count block has no live flag is a
    // plain video / finished stream - positively NotLive.
    const auto plainVideo = parseObject(R"({
        "contents": {"twoColumnWatchNextResults": {"results": {"results": {
            "contents": [{"videoPrimaryInfoRenderer": {
                "viewCount": {"videoViewCountRenderer": {
                    "viewCount": {"simpleText": "12,345 views"}}}}}]
        }}}}
    })");
    EXPECT_EQ(classifyLivenessFromNextResponse(plainVideo),
              YouTubeLiveness::NotLive);

    // Explicit isLive false is positively NotLive.
    const auto endedStream = parseObject(R"({
        "contents": {"twoColumnWatchNextResults": {"results": {"results": {
            "contents": [{"videoPrimaryInfoRenderer": {
                "viewCount": {"videoViewCountRenderer": {"isLive": false}}}}]
        }}}}
    })");
    EXPECT_EQ(classifyLivenessFromNextResponse(endedStream),
              YouTubeLiveness::NotLive);

    // An actively-broadcasting stream is Live.
    const auto liveNow = parseObject(R"({
        "contents": {"twoColumnWatchNextResults": {"results": {"results": {
            "contents": [{"videoPrimaryInfoRenderer": {
                "viewCount": {"videoViewCountRenderer": {"isLive": true}}}}]
        }}}}
    })");
    EXPECT_EQ(classifyLivenessFromNextResponse(liveNow),
              YouTubeLiveness::Live);
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

TEST(YouTubeParsing, extractYouTubeChannelHandleFindsEscapedJsonUrls)
{
    // ytInitialData / microformat JSON escapes slashes. The payloads live in
    // locals because MSVC's stringization of raw strings inside the gtest
    // macros re-reads \/ as an escape sequence (C4129).
    const QString vanity =
        R"("vanityChannelUrl":"http:\/\/www.youtube.com\/@gevad1ch")";
    EXPECT_EQ(extractYouTubeChannelHandle(vanity), "@gevad1ch");
    const QString owner =
        R"("ownerProfileUrl":"http:\/\/www.youtube.com\/@SomeOwner")";
    EXPECT_EQ(extractYouTubeChannelHandle(owner), "@SomeOwner");
    const QString canonicalBase =
        R"("canonicalBaseUrl":"\/@name.with-dots_ok")";
    EXPECT_EQ(extractYouTubeChannelHandle(canonicalBase),
              "@name.with-dots_ok");
}

TEST(YouTubeParsing, extractYouTubeChannelHandleFindsPageLevelTags)
{
    EXPECT_EQ(
        extractYouTubeChannelHandle(
            R"(<link rel="canonical" href="https://www.youtube.com/@gevad1ch">)"),
        "@gevad1ch");
    EXPECT_EQ(
        extractYouTubeChannelHandle(
            R"(<meta property="og:url" content="https://www.youtube.com/@gevad1ch">)"),
        "@gevad1ch");
    EXPECT_EQ(extractYouTubeChannelHandle(
                  R"("channelHandleText":{"runs":[{"text":"@gevad1ch"}]})"),
              "@gevad1ch");
}

TEST(YouTubeParsing, extractYouTubeChannelHandlePrefersOwnerOverBrowseEndpoints)
{
    // A watch page embeds other channels' browse endpoints (recommendations)
    // after the owner metadata; the owner-specific patterns must win.
    const QString html =
        R"("ownerProfileUrl":"http:\/\/www.youtube.com\/@TheOwner")"
        R"(..."canonicalBaseUrl":"\/@SomeRecommendedChannel")";
    EXPECT_EQ(extractYouTubeChannelHandle(html), "@TheOwner");
}

TEST(YouTubeParsing, extractYouTubeChannelHandleReturnsEmptyWhenAbsent)
{
    EXPECT_TRUE(extractYouTubeChannelHandle({}).isEmpty());
    EXPECT_TRUE(
        extractYouTubeChannelHandle("<html><body>no handles here</body></html>")
            .isEmpty());
    // Watch-page canonical is a video URL, not a handle.
    EXPECT_TRUE(
        extractYouTubeChannelHandle(
            R"(<link rel="canonical" href="https://www.youtube.com/watch?v=dQw4w9WgXcQ">)")
            .isEmpty());
    // Too short to be a valid handle (minimum 3 chars after the @).
    const QString shortHandle = R"("canonicalBaseUrl":"\/@ab")";
    EXPECT_TRUE(extractYouTubeChannelHandle(shortHandle).isEmpty());
}

TEST(YouTubeParsing, extractYouTubeChannelHandleOwnerScopedIgnoresFallbacks)
{
    // ownerScopedOnly is for watch pages where the generic fallbacks could
    // name a recommended channel instead of the video's owner.
    const QString recommendationOnly =
        R"("canonicalBaseUrl":"\/@SomeRecommendedChannel")"
        R"(..."channelHandleText":{"runs":[{"text":"@AnotherChannel"}]})";
    EXPECT_TRUE(extractYouTubeChannelHandle(recommendationOnly,
                                            /*ownerScopedOnly=*/true)
                    .isEmpty());
    EXPECT_EQ(extractYouTubeChannelHandle(recommendationOnly),
              "@AnotherChannel");

    const QString withOwner =
        R"("ownerProfileUrl":"http:\/\/www.youtube.com\/@TheOwner")"
        R"(..."canonicalBaseUrl":"\/@SomeRecommendedChannel")";
    EXPECT_EQ(extractYouTubeChannelHandle(withOwner, /*ownerScopedOnly=*/true),
              "@TheOwner");
}
