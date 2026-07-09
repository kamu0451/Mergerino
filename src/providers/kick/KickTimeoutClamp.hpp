// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT
//
// Shared clamping for Kick-provided timeout/ban durations. Kick delivers the
// duration (in minutes) as a JSON integer we do not control, so a hostile or
// malformed value must be bounded before it is multiplied out to seconds -- a
// huge value would otherwise overflow the signed 64-bit chrono representation
// (undefined behaviour). Both KickMessageBuilder and KickChatServer route the
// raw minutes through here so the bound is identical and unit-testable.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace chatterino {

// The largest timeout/ban we will honour, matching Twitch's ceiling of two
// weeks (1209600 seconds). Larger requests are clamped down to this; negative
// requests are clamped up to zero.
inline constexpr std::chrono::seconds KICK_MAX_TIMEOUT_DURATION{1209600};

// Clamps a timeout/ban duration expressed in minutes to
// [0, KICK_MAX_TIMEOUT_DURATION] and returns it in seconds. The minutes value
// is clamped first, so the minutes-to-seconds multiplication can never
// overflow.
inline std::chrono::seconds clampKickTimeoutMinutes(std::int64_t minutes)
{
    constexpr std::int64_t maxMinutes = KICK_MAX_TIMEOUT_DURATION.count() / 60;
    return std::chrono::seconds{
        std::clamp<std::int64_t>(minutes, 0, maxMinutes) * 60};
}

}  // namespace chatterino
