// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "Test.hpp"

#include "messages/Message.hpp"
#include "messages/MessageFlag.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "widgets/helper/ActivityMessageUtils.hpp"

using namespace chatterino;

TEST(ActivityMessageUtils, AllowsNativeAlertMessages)
{
    Message message;
    message.flags.set(MessageFlag::Subscription);

    EXPECT_TRUE(isActivityAlertMessage(message));
    EXPECT_TRUE(shouldShowMessageInActivityPane(message));
}

TEST(ActivityMessageUtils, RejectsBotBadgedMessages)
{
    Message twitchBadgedMessage;
    twitchBadgedMessage.flags.set(MessageFlag::CheerMessage);
    twitchBadgedMessage.twitchBadges.emplace_back("bot", "1");

    EXPECT_TRUE(isActivityBotMessage(twitchBadgedMessage));
    EXPECT_FALSE(shouldShowMessageInActivityPane(twitchBadgedMessage));

    Message externalBadgedMessage;
    externalBadgedMessage.flags.set(MessageFlag::ElevatedMessage);
    externalBadgedMessage.externalBadges = {"frankerfacez:bot"};

    EXPECT_TRUE(isActivityBotMessage(externalBadgedMessage));
    EXPECT_FALSE(shouldShowMessageInActivityPane(externalBadgedMessage));
}

TEST(ActivityMessageUtils, RejectsDateSeparators)
{
    Message separator;
    separator.flags.set(MessageFlag::System);
    separator.flags.set(MessageFlag::DoNotLog);
    separator.parseTime = QTime(0, 0);

    EXPECT_TRUE(isActivityDateSeparatorMessage(separator));
    EXPECT_FALSE(shouldShowMessageInActivityPane(separator));

    Message otherSystemMessage;
    otherSystemMessage.flags.set(MessageFlag::System);
    otherSystemMessage.flags.set(MessageFlag::DoNotLog);
    otherSystemMessage.parseTime = QTime(12, 0);

    EXPECT_FALSE(isActivityDateSeparatorMessage(otherSystemMessage));
    EXPECT_FALSE(shouldShowMessageInActivityPane(otherSystemMessage));
}

TEST(ActivityMessageUtils, RejectsPlainChatMessages)
{
    Message message;
    message.messageText = "Thanks for the follow!";

    EXPECT_FALSE(isActivityAlertMessage(message));
    EXPECT_FALSE(isActivityBotMessage(message));
    EXPECT_FALSE(shouldShowMessageInActivityPane(message));
}

TEST(ActivityMessageUtils, CompactsGiftBombMembershipsToSubs)
{
    Message message;
    message.flags.set(MessageFlag::Subscription);
    message.messageText = "GiftLord gifted 5 memberships to viewers!";

    ASSERT_EQ(getActivityGiftBombRecipientCount(message), 5);
    EXPECT_EQ(compactActivityGiftBombText(message), "GiftLord gifted 5 subs");
}

TEST(ActivityMessageUtils, CompactsYouTubeGiftPurchaseAnnouncements)
{
    Message message;
    message.flags.set(MessageFlag::Subscription);
    message.displayName = "Halfman";
    message.messageText = "Sent 20 Coconut B gift memberships";

    ASSERT_EQ(getActivityGiftBombRecipientCount(message), 20);
    EXPECT_EQ(compactActivityGiftBombText(message), "Halfman gifted 20 subs");
}

TEST(ActivityMessageUtils, DetectsTwitchAndYouTubeGiftRecipients)
{
    Message twitchRecipient;
    twitchRecipient.flags.set(MessageFlag::Subscription);
    twitchRecipient.flags.set(MessageFlag::System);
    twitchRecipient.messageText = "GiftLord gifted a Tier 1 sub to Viewer!";

    EXPECT_TRUE(isActivityGiftRecipientMessage(twitchRecipient));

    Message youtubeRecipient;
    youtubeRecipient.flags.set(MessageFlag::Subscription);
    youtubeRecipient.messageText =
        "Viewer received a gift membership from GiftLord!";

    EXPECT_TRUE(isActivityGiftRecipientMessage(youtubeRecipient));

    Message youtubeRecipientBy;
    youtubeRecipientBy.flags.set(MessageFlag::Subscription);
    youtubeRecipientBy.messageText =
        "Viewer received a gift membership by GiftLord";

    EXPECT_TRUE(isActivityGiftRecipientMessage(youtubeRecipientBy));
}

TEST(ActivityMessageUtils, RejectsKickRewardRedemptions)
{
    Message message;
    message.platform = MessagePlatform::Kick;
    message.flags.set(MessageFlag::CheerMessage);
    message.flags.set(MessageFlag::RedeemedChannelPointReward);
    message.messageText = "Viewer redeemed Highlight My Message";

    EXPECT_TRUE(isActivityKickRewardRedemptionMessage(message));
    EXPECT_FALSE(shouldShowMessageInActivityPane(message));
}

TEST(ActivityMessageUtils, RejectsTwitchAnnouncementHeader)
{
    Message message;
    message.platform = MessagePlatform::AnyOrTwitch;
    message.flags.set(MessageFlag::Subscription);
    message.flags.set(MessageFlag::System);
    message.messageText = "Announcement";

    EXPECT_TRUE(isActivityTwitchAnnouncementHeaderMessage(message));
    EXPECT_FALSE(shouldShowMessageInActivityPane(message));
}

TEST(ActivityMessageUtils, DetectsTwitchAnnouncementFollowup)
{
    Message message;
    message.platform = MessagePlatform::AnyOrTwitch;
    message.flags.set(MessageFlag::Subscription);
    message.flags.set(MessageFlag::Highlighted);
    message.messageText = "StreamElements says hello";

    EXPECT_TRUE(isActivityTwitchAnnouncementFollowupMessage(message));
}

TEST(ActivityMessageUtils, RejectsTwitchBitsBadgeMessages)
{
    Message message;
    message.platform = MessagePlatform::AnyOrTwitch;
    message.flags.set(MessageFlag::Subscription);
    message.flags.set(MessageFlag::System);
    message.messageText = "whoopiix just earned a new 1K Bits badge!";

    EXPECT_TRUE(isActivityTwitchBitsBadgeMessage(message));
    EXPECT_FALSE(shouldShowMessageInActivityPane(message));
}

TEST(ActivityMessageUtils, AcceptsYouTubeSubscriptionMessage)
{
    Message message;
    message.platform = MessagePlatform::YouTube;
    message.flags.set(MessageFlag::Subscription);
    message.messageText = "Viewer became a member";

    EXPECT_TRUE(isActivityAlertMessage(message));
    EXPECT_TRUE(shouldShowMessageInActivityPane(message));
}

TEST(ActivityMessageUtils, AcceptsYouTubeElevatedMessage)
{
    Message message;
    message.platform = MessagePlatform::YouTube;
    message.flags.set(MessageFlag::ElevatedMessage);
    message.messageText = "Viewer sent a super chat";

    EXPECT_TRUE(isActivityAlertMessage(message));
    EXPECT_TRUE(shouldShowMessageInActivityPane(message));
}

TEST(ActivityMessageUtils, AcceptsTikTokSubscriptionMessage)
{
    Message message;
    message.platform = MessagePlatform::TikTok;
    message.flags.set(MessageFlag::Subscription);
    message.flags.set(MessageFlag::System);
    message.messageText = "alice joined";

    EXPECT_TRUE(isActivityAlertMessage(message));
    EXPECT_TRUE(shouldShowMessageInActivityPane(message));
}

TEST(ActivityMessageUtils, TwitchAnnouncementFilterIsPlatformScoped)
{
    // The Twitch-specific "Announcement" header must only filter Twitch
    // messages; the same text on a non-Twitch platform must not be dropped.
    Message youtubeAnnouncement;
    youtubeAnnouncement.platform = MessagePlatform::YouTube;
    youtubeAnnouncement.flags.set(MessageFlag::Subscription);
    youtubeAnnouncement.flags.set(MessageFlag::System);
    youtubeAnnouncement.messageText = "Announcement";

    EXPECT_FALSE(
        isActivityTwitchAnnouncementHeaderMessage(youtubeAnnouncement));
    EXPECT_TRUE(shouldShowMessageInActivityPane(youtubeAnnouncement));
}

TEST(ActivityMessageUtils, TwitchBitsBadgeFilterIsPlatformScoped)
{
    // If a YouTube chat happened to contain text matching the Twitch
    // "Bits badge" string, it must not be filtered - the check is
    // Twitch-only.
    Message youtubeBitsLike;
    youtubeBitsLike.platform = MessagePlatform::YouTube;
    youtubeBitsLike.flags.set(MessageFlag::Subscription);
    youtubeBitsLike.flags.set(MessageFlag::System);
    youtubeBitsLike.messageText = "someone just earned a new 1K Bits badge!";

    EXPECT_FALSE(isActivityTwitchBitsBadgeMessage(youtubeBitsLike));
    EXPECT_TRUE(shouldShowMessageInActivityPane(youtubeBitsLike));
}

TEST(ActivityMessageUtils, KickRewardFilterIsPlatformScoped)
{
    // RedeemedChannelPointReward flag on a non-Kick platform should not be
    // dropped by the Kick-specific filter.
    Message youtubeWithRewardFlag;
    youtubeWithRewardFlag.platform = MessagePlatform::YouTube;
    youtubeWithRewardFlag.flags.set(MessageFlag::CheerMessage);
    youtubeWithRewardFlag.flags.set(MessageFlag::RedeemedChannelPointReward);
    youtubeWithRewardFlag.messageText = "Viewer redeemed something";

    EXPECT_FALSE(
        isActivityKickRewardRedemptionMessage(youtubeWithRewardFlag));
    EXPECT_TRUE(shouldShowMessageInActivityPane(youtubeWithRewardFlag));
}
