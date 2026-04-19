// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeLiveChat.hpp"
#include "Test.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace chatterino;

namespace {

// Any 11-char base64url-alphabet string is a valid YouTube video id shape.
constexpr auto VIDEO_ID = "dQw4w9WgXcQ";

// A valid channel id is "UC" + 22 base64url chars; using a plausible one here.
constexpr auto CHANNEL_ID = "UC-lHJZR3Gqxm24_Vd_AJ5Yw";

}  // namespace

TEST(YouTubeLiveChat, maybeExtractVideoIdAcceptsBareVideoId)
{
    EXPECT_EQ(YouTubeLiveChat::maybeExtractVideoId(VIDEO_ID), VIDEO_ID);
}

TEST(YouTubeLiveChat, maybeExtractVideoIdExtractsFromWatchUrl)
{
    EXPECT_EQ(YouTubeLiveChat::maybeExtractVideoId(
                  "https://www.youtube.com/watch?v=dQw4w9WgXcQ"),
              VIDEO_ID);
}

TEST(YouTubeLiveChat, maybeExtractVideoIdExtractsFromYoutuBeShortUrl)
{
    EXPECT_EQ(YouTubeLiveChat::maybeExtractVideoId(
                  "https://youtu.be/dQw4w9WgXcQ"),
              VIDEO_ID);
}

TEST(YouTubeLiveChat, maybeExtractVideoIdExtractsFromLivePath)
{
    EXPECT_EQ(YouTubeLiveChat::maybeExtractVideoId(
                  "https://www.youtube.com/live/dQw4w9WgXcQ"),
              VIDEO_ID);
}

TEST(YouTubeLiveChat, maybeExtractVideoIdExtractsFromShortsPath)
{
    EXPECT_EQ(YouTubeLiveChat::maybeExtractVideoId(
                  "https://www.youtube.com/shorts/dQw4w9WgXcQ"),
              VIDEO_ID);
}

TEST(YouTubeLiveChat, maybeExtractVideoIdExtractsFromEmbedPath)
{
    EXPECT_EQ(YouTubeLiveChat::maybeExtractVideoId(
                  "https://www.youtube.com/embed/dQw4w9WgXcQ"),
              VIDEO_ID);
}

TEST(YouTubeLiveChat, maybeExtractVideoIdAcceptsSchemelessHost)
{
    EXPECT_EQ(YouTubeLiveChat::maybeExtractVideoId(
                  "www.youtube.com/watch?v=dQw4w9WgXcQ"),
              VIDEO_ID);
    EXPECT_EQ(YouTubeLiveChat::maybeExtractVideoId(
                  "youtu.be/dQw4w9WgXcQ"),
              VIDEO_ID);
}

TEST(YouTubeLiveChat, maybeExtractVideoIdIgnoresExtraQueryParams)
{
    EXPECT_EQ(YouTubeLiveChat::maybeExtractVideoId(
                  "https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=42s&si=abc"),
              VIDEO_ID);
}

TEST(YouTubeLiveChat, maybeExtractVideoIdReturnsEmptyOnHandleUrl)
{
    EXPECT_TRUE(YouTubeLiveChat::maybeExtractVideoId(
                    "https://www.youtube.com/@gevad1ch")
                    .isEmpty());
}

TEST(YouTubeLiveChat, maybeExtractVideoIdReturnsEmptyOnBlank)
{
    EXPECT_TRUE(YouTubeLiveChat::maybeExtractVideoId("").isEmpty());
    EXPECT_TRUE(YouTubeLiveChat::maybeExtractVideoId("   ").isEmpty());
}

TEST(YouTubeLiveChat, normalizeSourcePreservesAtHandleAndLowercases)
{
    EXPECT_EQ(YouTubeLiveChat::normalizeSource("@FooBar"), "@foobar");
    EXPECT_EQ(YouTubeLiveChat::normalizeSource("@gevad1ch"), "@gevad1ch");
}

TEST(YouTubeLiveChat, normalizeSourceStripsTrailingSegmentsAfterHandle)
{
    EXPECT_EQ(YouTubeLiveChat::normalizeSource("@foobar/live"), "@foobar");
}

TEST(YouTubeLiveChat, normalizeSourceReturnsChannelIdUnchanged)
{
    EXPECT_EQ(YouTubeLiveChat::normalizeSource(CHANNEL_ID), CHANNEL_ID);
}

TEST(YouTubeLiveChat, normalizeSourceExtractsHandleFromFullUrl)
{
    EXPECT_EQ(YouTubeLiveChat::normalizeSource(
                  "https://www.youtube.com/@foobar"),
              "@foobar");
    EXPECT_EQ(YouTubeLiveChat::normalizeSource(
                  "https://www.youtube.com/@FooBar/live"),
              "@foobar");
}

TEST(YouTubeLiveChat, normalizeSourceExtractsChannelIdFromFullUrl)
{
    const QString url = QString("https://www.youtube.com/channel/%1")
                            .arg(CHANNEL_ID);
    EXPECT_EQ(YouTubeLiveChat::normalizeSource(url), CHANNEL_ID);
}

TEST(YouTubeLiveChat, normalizeSourceReturnsEmptyOnBlank)
{
    EXPECT_TRUE(YouTubeLiveChat::normalizeSource("").isEmpty());
    EXPECT_TRUE(YouTubeLiveChat::normalizeSource("  \t").isEmpty());
}

TEST(YouTubeLiveChat, normalizeSourceReturnsEmptyOnUnrecognizedUrl)
{
    EXPECT_TRUE(
        YouTubeLiveChat::normalizeSource("https://example.com/something")
            .isEmpty());
    EXPECT_TRUE(
        YouTubeLiveChat::normalizeSource("ftp://example.com").isEmpty());
}

TEST(YouTubeLiveChat, isLikelyChannelIdAcceptsCanonicalShape)
{
    EXPECT_TRUE(YouTubeLiveChat::isLikelyChannelId(CHANNEL_ID));
}

TEST(YouTubeLiveChat, isLikelyChannelIdRejectsShortInput)
{
    EXPECT_FALSE(YouTubeLiveChat::isLikelyChannelId("UC123"));
}

TEST(YouTubeLiveChat, isLikelyChannelIdRejectsMissingUcPrefix)
{
    EXPECT_FALSE(YouTubeLiveChat::isLikelyChannelId(
        "XC-lHJZR3Gqxm24_Vd_AJ5Yw"));
}

TEST(YouTubeLiveChat, isLikelyChannelIdRejectsInvalidChars)
{
    // Valid length, but '!' is not in the base64url alphabet.
    EXPECT_FALSE(YouTubeLiveChat::isLikelyChannelId(
        "UC-lHJZR3Gqxm24_Vd_AJ5Y!"));
}

TEST(YouTubeLiveChat, extractLiveChatContinuationReturnsPrimaryContinuation)
{
    const auto json = R"({
        "continuations": [
            {"reloadContinuationData": {"continuation": "abc123"}}
        ]
    })";
    const auto doc = QJsonDocument::fromJson(json);
    EXPECT_EQ(
        YouTubeLiveChat::extractLiveChatContinuation(doc.object()),
        "abc123");
}

TEST(YouTubeLiveChat,
     extractLiveChatContinuationFallsBackToSubMenuItems)
{
    const auto json = R"({
        "continuations": [],
        "header": {
            "liveChatHeaderRenderer": {
                "viewSelector": {
                    "sortFilterSubMenuRenderer": {
                        "subMenuItems": [
                            {"continuation": {
                                "reloadContinuationData": {
                                    "continuation": "fallback-token"
                                }}}
                        ]
                    }
                }
            }
        }
    })";
    const auto doc = QJsonDocument::fromJson(json);
    EXPECT_EQ(
        YouTubeLiveChat::extractLiveChatContinuation(doc.object()),
        "fallback-token");
}

TEST(YouTubeLiveChat, extractLiveChatContinuationReturnsEmptyOnMissingShape)
{
    EXPECT_TRUE(
        YouTubeLiveChat::extractLiveChatContinuation(QJsonObject{}).isEmpty());
}

TEST(YouTubeLiveChat, extractLiveStreamTitleReturnsSimpleText)
{
    const auto json = R"({
        "contents": {
            "twoColumnWatchNextResults": {
                "results": {
                    "results": {
                        "contents": [
                            {"videoPrimaryInfoRenderer": {
                                "title": {"simpleText": "Live now!"}
                            }}
                        ]
                    }
                }
            }
        }
    })";
    const auto doc = QJsonDocument::fromJson(json);
    EXPECT_EQ(
        YouTubeLiveChat::extractLiveStreamTitle(doc.object()), "Live now!");
}

TEST(YouTubeLiveChat, extractLiveStreamTitleConcatsRuns)
{
    const auto json = R"({
        "contents": {
            "twoColumnWatchNextResults": {
                "results": {
                    "results": {
                        "contents": [
                            {"videoPrimaryInfoRenderer": {
                                "title": {"runs": [
                                    {"text": "Part "},
                                    {"text": "1"}
                                ]}
                            }}
                        ]
                    }
                }
            }
        }
    })";
    const auto doc = QJsonDocument::fromJson(json);
    EXPECT_EQ(
        YouTubeLiveChat::extractLiveStreamTitle(doc.object()), "Part 1");
}

TEST(YouTubeLiveChat, extractLiveStreamTitleReturnsEmptyOnMissingShape)
{
    EXPECT_TRUE(
        YouTubeLiveChat::extractLiveStreamTitle(QJsonObject{}).isEmpty());
}

TEST(YouTubeLiveChat, computeBackoffDelayBaseAtZeroOrOne)
{
    EXPECT_EQ(YouTubeLiveChat::computeBackoffDelay(0), 3000);
    EXPECT_EQ(YouTubeLiveChat::computeBackoffDelay(-1), 3000);
    EXPECT_EQ(YouTubeLiveChat::computeBackoffDelay(1), 3000);
}

TEST(YouTubeLiveChat, computeBackoffDelayDoublesPerFailure)
{
    EXPECT_EQ(YouTubeLiveChat::computeBackoffDelay(2), 6000);
    EXPECT_EQ(YouTubeLiveChat::computeBackoffDelay(3), 12000);
    EXPECT_EQ(YouTubeLiveChat::computeBackoffDelay(4), 24000);
    EXPECT_EQ(YouTubeLiveChat::computeBackoffDelay(5), 48000);
}

TEST(YouTubeLiveChat, computeBackoffDelayCapsAt60Seconds)
{
    EXPECT_EQ(YouTubeLiveChat::computeBackoffDelay(6), 60000);
    EXPECT_EQ(YouTubeLiveChat::computeBackoffDelay(10), 60000);
    EXPECT_EQ(YouTubeLiveChat::computeBackoffDelay(100), 60000);
}
