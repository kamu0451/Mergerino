// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/ActivityMessageUtils.hpp"

#include "messages/MessageFlag.hpp"
#include "providers/twitch/TwitchBadge.hpp"

#include <QRegularExpression>
#include <QTime>

#include <ranges>

namespace {

using namespace chatterino;

const QRegularExpression ACTIVITY_GIFT_BOMB_SUMMARY_REGEX(
    QStringLiteral(
        R"(^(.+?)\s+(?:is gifting|gifted)\s+(\d+)\s+.*?\b(subs|subscriptions|memberships)\b(?:\s+to\b.*)?(?:!|\.)?(?: .*)?$)"),
    QRegularExpression::CaseInsensitiveOption);

const QRegularExpression ACTIVITY_GIFT_RECIPIENT_REGEX(
    QStringLiteral(
        R"((?:^.+\s+gifted\s+(?:(?:an?\s+|\d+\s+months?\s+of\s+an?\s+).*)?\b(?:sub|subscription|membership)\b\s+to\s+.+(?:!|\.)?(?: .*)?$)|(?:^.+\s+received\s+(?:a\s+gift\s+)?membership\s+from\s+.+(?:!|\.)?(?: .*)?$))"),
    QRegularExpression::CaseInsensitiveOption);

const QRegularExpression ACTIVITY_TWITCH_BITS_BADGE_REGEX(
    QStringLiteral(R"(^.+\s+just earned a new\s+.+\s+Bits badge!$)"),
    QRegularExpression::CaseInsensitiveOption);

QString normalizedActivityGiftUnit(int count)
{
    return count == 1 ? QStringLiteral("sub") : QStringLiteral("subs");
}

QRegularExpressionMatch activityGiftBombSummaryMatch(const Message &message)
{
    return ACTIVITY_GIFT_BOMB_SUMMARY_REGEX.match(message.messageText);
}

}  // namespace

namespace chatterino {

bool isActivityAlertMessage(const Message &message)
{
    return message.flags.hasAny({MessageFlag::Subscription,
                                 MessageFlag::ElevatedMessage,
                                 MessageFlag::CheerMessage});
}

bool isActivityBotMessage(const Message &message)
{
    const auto hasTwitchBotBadge =
        std::ranges::any_of(message.twitchBadges, [](const auto &badge) {
            return badge.key_.compare("bot", Qt::CaseInsensitive) == 0 ||
                   badge.key_.compare("chatbot", Qt::CaseInsensitive) == 0;
        });
    if (hasTwitchBotBadge)
    {
        return true;
    }

    return std::ranges::any_of(message.externalBadges, [](const auto &badge) {
        return badge.endsWith(":bot", Qt::CaseInsensitive);
    });
}

bool isActivityDateSeparatorMessage(const Message &message)
{
    return message.flags.has(MessageFlag::System) &&
           message.flags.has(MessageFlag::DoNotLog) &&
           message.parseTime == QTime(0, 0);
}

bool isActivityKickRewardRedemptionMessage(const Message &message)
{
    return message.platform == MessagePlatform::Kick &&
           message.flags.has(MessageFlag::RedeemedChannelPointReward);
}

bool isActivityTwitchAnnouncementHeaderMessage(const Message &message)
{
    return message.platform == MessagePlatform::AnyOrTwitch &&
           message.flags.has(MessageFlag::Subscription) &&
           message.flags.has(MessageFlag::System) &&
           message.messageText == QStringLiteral("Announcement");
}

bool isActivityTwitchAnnouncementFollowupMessage(const Message &message)
{
    return message.platform == MessagePlatform::AnyOrTwitch &&
           message.flags.has(MessageFlag::Subscription) &&
           !message.flags.has(MessageFlag::System);
}

bool isActivityTwitchBitsBadgeMessage(const Message &message)
{
    return message.platform == MessagePlatform::AnyOrTwitch &&
           message.flags.has(MessageFlag::Subscription) &&
           message.flags.has(MessageFlag::System) &&
           ACTIVITY_TWITCH_BITS_BADGE_REGEX.match(message.messageText)
               .hasMatch();
}

std::optional<int> getActivityGiftBombRecipientCount(const Message &message)
{
    if (!message.flags.has(MessageFlag::Subscription))
    {
        return std::nullopt;
    }

    const auto match = activityGiftBombSummaryMatch(message);
    if (!match.hasMatch())
    {
        return std::nullopt;
    }

    return match.captured(2).toInt();
}

bool isActivityGiftRecipientMessage(const Message &message)
{
    return message.flags.has(MessageFlag::Subscription) &&
           ACTIVITY_GIFT_RECIPIENT_REGEX.match(message.messageText).hasMatch();
}

QString compactActivityGiftBombText(const Message &message)
{
    const auto match = activityGiftBombSummaryMatch(message);
    if (!match.hasMatch())
    {
        return message.messageText;
    }

    auto name = match.captured(1).trimmed();
    if (name.isEmpty())
    {
        name = message.displayName.trimmed();
    }
    if (name.isEmpty())
    {
        name = message.loginName.trimmed();
    }
    if (name.isEmpty())
    {
        name = QStringLiteral("Someone");
    }

    const int count = match.captured(2).toInt();
    return QStringLiteral("%1 gifted %2 %3")
        .arg(name, QString::number(count), normalizedActivityGiftUnit(count));
}

bool shouldShowMessageInActivityPane(const Message &message)
{
    if (isActivityBotMessage(message))
    {
        return false;
    }

    if (isActivityDateSeparatorMessage(message) ||
        isActivityKickRewardRedemptionMessage(message) ||
        isActivityTwitchAnnouncementHeaderMessage(message) ||
        isActivityTwitchBitsBadgeMessage(message))
    {
        return false;
    }

    return isActivityAlertMessage(message);
}

}  // namespace chatterino
