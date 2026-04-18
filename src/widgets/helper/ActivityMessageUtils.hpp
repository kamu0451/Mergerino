// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Message.hpp"

#include <optional>

namespace chatterino {

bool isActivityAlertMessage(const Message &message);
bool isActivityBotMessage(const Message &message);
bool isActivityDateSeparatorMessage(const Message &message);
bool isActivityKickRewardRedemptionMessage(const Message &message);
bool isActivityTwitchAnnouncementHeaderMessage(const Message &message);
bool isActivityTwitchAnnouncementFollowupMessage(const Message &message);
bool isActivityTwitchBitsBadgeMessage(const Message &message);
std::optional<int> getActivityGiftBombRecipientCount(const Message &message);
bool isActivityGiftRecipientMessage(const Message &message);
QString compactActivityGiftBombText(const Message &message);
bool shouldShowMessageInActivityPane(const Message &message);

}  // namespace chatterino
