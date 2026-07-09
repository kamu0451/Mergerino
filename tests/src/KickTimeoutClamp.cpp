// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "providers/kick/KickTimeoutClamp.hpp"

#include "Test.hpp"

#include <chrono>
#include <cstdint>
#include <limits>

using namespace chatterino;
using namespace std::chrono_literals;

TEST(KickTimeoutClamp, NormalValue)
{
    // 5 minutes -> 300 seconds, no clamping.
    EXPECT_EQ(clampKickTimeoutMinutes(5), 300s);
    EXPECT_EQ(clampKickTimeoutMinutes(1), 60s);
    EXPECT_EQ(clampKickTimeoutMinutes(1440), 24h);
}

TEST(KickTimeoutClamp, Zero)
{
    EXPECT_EQ(clampKickTimeoutMinutes(0), 0s);
}

TEST(KickTimeoutClamp, NegativeClampsToZero)
{
    EXPECT_EQ(clampKickTimeoutMinutes(-1), 0s);
    EXPECT_EQ(clampKickTimeoutMinutes(-1209600), 0s);
    EXPECT_EQ(clampKickTimeoutMinutes(std::numeric_limits<std::int64_t>::min()),
              0s);
}

TEST(KickTimeoutClamp, AtCeiling)
{
    // 20160 minutes == exactly two weeks; not clamped down.
    EXPECT_EQ(clampKickTimeoutMinutes(20160), KICK_MAX_TIMEOUT_DURATION);
    EXPECT_EQ(KICK_MAX_TIMEOUT_DURATION, 1209600s);
}

TEST(KickTimeoutClamp, HugeValueBoundedNoOverflow)
{
    // A hostile INT64_MAX minutes must be clamped to the ceiling rather than
    // overflowing the signed 64-bit chrono representation.
    const auto clamped =
        clampKickTimeoutMinutes(std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(clamped, KICK_MAX_TIMEOUT_DURATION);
    // Just over the ceiling is still clamped.
    EXPECT_EQ(clampKickTimeoutMinutes(20161), KICK_MAX_TIMEOUT_DURATION);
    // Result never exceeds the bound and is non-negative.
    EXPECT_LE(clamped, KICK_MAX_TIMEOUT_DURATION);
    EXPECT_GE(clamped, 0s);
}
