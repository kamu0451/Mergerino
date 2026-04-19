// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "providers/merged/MergedChannel.hpp"
#include "Test.hpp"

#include "messages/Message.hpp"
#include "messages/MessageFlag.hpp"

#include <QDateTime>

using namespace chatterino;

namespace {

MessagePtrMut makeMessage()
{
    return std::make_shared<Message>();
}

MessagePtrMut makeSystemMessage(QString text)
{
    auto msg = std::make_shared<Message>();
    msg->flags.set(MessageFlag::System);
    msg->messageText = std::move(text);
    return msg;
}

}  // namespace

// ---------- shouldMirrorSourceMessage ----------

TEST(MergedChannelHelpers, shouldMirrorSourceMessageRejectsNull)
{
    EXPECT_FALSE(MergedChannel::shouldMirrorSourceMessage(nullptr));
}

TEST(MergedChannelHelpers, shouldMirrorSourceMessageRejectsConnectedMessages)
{
    auto msg = makeMessage();
    msg->flags.set(MessageFlag::ConnectedMessage);
    EXPECT_FALSE(MergedChannel::shouldMirrorSourceMessage(msg));
}

TEST(MergedChannelHelpers, shouldMirrorSourceMessageAcceptsRegularChat)
{
    auto msg = makeMessage();
    msg->messageText = "hello world";
    EXPECT_TRUE(MergedChannel::shouldMirrorSourceMessage(msg));
}

TEST(MergedChannelHelpers,
     shouldMirrorSourceMessageRejectsIsLiveAnnouncements)
{
    EXPECT_FALSE(MergedChannel::shouldMirrorSourceMessage(
        makeSystemMessage("shroud is live!")));
    EXPECT_FALSE(MergedChannel::shouldMirrorSourceMessage(
        makeSystemMessage("tarik is live: Valorant")));
}

TEST(MergedChannelHelpers,
     shouldMirrorSourceMessageRejectsBareConnectionStatus)
{
    EXPECT_FALSE(MergedChannel::shouldMirrorSourceMessage(
        makeSystemMessage("joined")));
    EXPECT_FALSE(MergedChannel::shouldMirrorSourceMessage(
        makeSystemMessage("joined channel")));
    EXPECT_FALSE(MergedChannel::shouldMirrorSourceMessage(
        makeSystemMessage("connected")));
    EXPECT_FALSE(MergedChannel::shouldMirrorSourceMessage(
        makeSystemMessage("reconnected")));
}

TEST(MergedChannelHelpers,
     shouldMirrorSourceMessageIsCaseInsensitiveForStatusStrings)
{
    // Filter lowercases the text before comparing, so differently-cased
    // provider output should still be filtered.
    EXPECT_FALSE(MergedChannel::shouldMirrorSourceMessage(
        makeSystemMessage("Joined")));
    EXPECT_FALSE(MergedChannel::shouldMirrorSourceMessage(
        makeSystemMessage("CONNECTED")));
}

TEST(MergedChannelHelpers,
     shouldMirrorSourceMessageKeepsTikTokActivityMessages)
{
    // TikTok activity events (likes, joins, gifts, follows) carry System flag
    // but always include a username/count prefix, so they must not be exact
    // matches for the filter's drop list. Regression guard for Workstream 1.4
    // which consolidated the YouTube/TikTok mirror paths through this filter.
    EXPECT_TRUE(MergedChannel::shouldMirrorSourceMessage(
        makeSystemMessage("alice joined")));
    EXPECT_TRUE(MergedChannel::shouldMirrorSourceMessage(
        makeSystemMessage("5 viewers joined")));
    EXPECT_TRUE(MergedChannel::shouldMirrorSourceMessage(
        makeSystemMessage("bob liked the stream")));
    EXPECT_TRUE(MergedChannel::shouldMirrorSourceMessage(
        makeSystemMessage("charlie sent a gift")));
    EXPECT_TRUE(MergedChannel::shouldMirrorSourceMessage(
        makeSystemMessage("dan followed the host")));
    EXPECT_TRUE(MergedChannel::shouldMirrorSourceMessage(
        makeSystemMessage("Joined YouTube live chat: My Stream")));
}

// ---------- messageKey ----------

TEST(MergedChannelHelpers, messageKeyReturnsEmptyForNull)
{
    EXPECT_TRUE(MergedChannel::messageKey(nullptr,
                                          MessagePlatform::AnyOrTwitch)
                    .isEmpty());
}

TEST(MergedChannelHelpers, messageKeyUsesIdWhenPresent)
{
    auto msg = makeMessage();
    msg->id = "abc-123";
    const auto key =
        MergedChannel::messageKey(msg, MessagePlatform::AnyOrTwitch);
    EXPECT_FALSE(key.isEmpty());
    EXPECT_TRUE(key.endsWith("abc-123"));
}

TEST(MergedChannelHelpers,
     messageKeyDiffersBetweenPlatformsForSameMessage)
{
    auto msg = makeMessage();
    msg->id = "shared-id";
    const auto twitch =
        MergedChannel::messageKey(msg, MessagePlatform::AnyOrTwitch);
    const auto kick = MergedChannel::messageKey(msg, MessagePlatform::Kick);
    const auto youtube =
        MergedChannel::messageKey(msg, MessagePlatform::YouTube);
    const auto tiktok = MergedChannel::messageKey(msg, MessagePlatform::TikTok);
    EXPECT_NE(twitch, kick);
    EXPECT_NE(twitch, youtube);
    EXPECT_NE(twitch, tiktok);
    EXPECT_NE(kick, youtube);
    EXPECT_NE(kick, tiktok);
    EXPECT_NE(youtube, tiktok);
}

TEST(MergedChannelHelpers,
     messageKeyFallsBackToContentWhenNoId)
{
    auto msg = makeMessage();
    msg->channelName = "streamer";
    msg->loginName = "viewer";
    msg->messageText = "hello";
    msg->serverReceivedTime =
        QDateTime::fromMSecsSinceEpoch(1700000000000LL, Qt::UTC);

    const auto key =
        MergedChannel::messageKey(msg, MessagePlatform::AnyOrTwitch);
    EXPECT_FALSE(key.isEmpty());
    EXPECT_TRUE(key.contains("streamer"));
    EXPECT_TRUE(key.contains("viewer"));
    EXPECT_TRUE(key.contains("hello"));
}

TEST(MergedChannelHelpers,
     messageKeyContentBasedVariesWithTimestamp)
{
    auto msgA = makeMessage();
    msgA->channelName = "streamer";
    msgA->loginName = "viewer";
    msgA->messageText = "hello";
    msgA->serverReceivedTime =
        QDateTime::fromMSecsSinceEpoch(1700000000000LL, Qt::UTC);

    auto msgB = makeMessage();
    msgB->channelName = "streamer";
    msgB->loginName = "viewer";
    msgB->messageText = "hello";
    msgB->serverReceivedTime =
        QDateTime::fromMSecsSinceEpoch(1700000000001LL, Qt::UTC);

    EXPECT_NE(
        MergedChannel::messageKey(msgA, MessagePlatform::AnyOrTwitch),
        MergedChannel::messageKey(msgB, MessagePlatform::AnyOrTwitch));
}

TEST(MergedChannelHelpers, messageKeyIdDominatesContent)
{
    // Two messages with the same id but different content should key to the
    // same value — the dedup key is id-first, so edits (same id, different
    // text) replace the original rather than appending a duplicate.
    auto msgA = makeMessage();
    msgA->id = "edit-target";
    msgA->messageText = "original";

    auto msgB = makeMessage();
    msgB->id = "edit-target";
    msgB->messageText = "edited";

    EXPECT_EQ(MergedChannel::messageKey(msgA, MessagePlatform::AnyOrTwitch),
              MergedChannel::messageKey(msgB, MessagePlatform::AnyOrTwitch));
}

// ---------- MergedChannelConfig ----------

TEST(MergedChannelConfig, displayNamePrefersTabNameWhenSet)
{
    MergedChannelConfig cfg;
    cfg.tabName = "  My Merged Tab  ";
    cfg.twitchChannelName = "ignored_twitch";
    EXPECT_EQ(cfg.displayName(), "My Merged Tab");
}

TEST(MergedChannelConfig,
     displayNameFallsBackToTwitchChannelWhenTabNameBlank)
{
    MergedChannelConfig cfg;
    cfg.tabName = "";
    cfg.twitchEnabled = true;
    cfg.twitchChannelName = "ShroudCaster";
    EXPECT_EQ(cfg.displayName(), "shroudcaster");
}

TEST(MergedChannelConfig,
     effectiveTwitchChannelNameNormalizesLeadingHashAndCase)
{
    MergedChannelConfig cfg;
    cfg.twitchChannelName = "#ShroudCaster";
    EXPECT_EQ(cfg.effectiveTwitchChannelName(), "shroudcaster");
}

TEST(MergedChannelConfig,
     effectiveTwitchChannelNameFallsBackToTabName)
{
    MergedChannelConfig cfg;
    cfg.tabName = "MyShow";
    cfg.twitchChannelName = "";
    EXPECT_EQ(cfg.effectiveTwitchChannelName(), "myshow");
}

TEST(MergedChannelConfig,
     effectiveKickChannelNameStripsKickPrefix)
{
    MergedChannelConfig cfg;
    cfg.kickChannelName = ":kick:TheKickStreamer";
    EXPECT_EQ(cfg.effectiveKickChannelName(), "thekickstreamer");
}

TEST(MergedChannelConfig,
     effectiveKickChannelNameFallsBackToTabName)
{
    MergedChannelConfig cfg;
    cfg.tabName = "SomeShow";
    cfg.kickChannelName = "";
    EXPECT_EQ(cfg.effectiveKickChannelName(), "someshow");
}
