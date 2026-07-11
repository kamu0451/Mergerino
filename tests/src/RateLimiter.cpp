// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/RateLimiter.hpp"

#include "Test.hpp"

#include <chrono>

using namespace chatterino;
using namespace std::chrono_literals;

namespace {

using TimePoint = BurstRateLimiter::TimePoint;

// A comfortably-positive base so every offset stays well past the clock epoch.
// The core takes `now` as a parameter, so the "clock" here is just increasing
// time_points we construct by hand -- no real clock is ever read.
constexpr TimePoint base = TimePoint{} + 1h;

}  // namespace

TEST(BurstRateLimiter, AllowsNormalPacedMessages)
{
    BurstRateLimiter limiter;

    // maxBurst 3, minOffset 100ms, cooldown 5s. Three messages 200ms apart are
    // both slower than minOffset and fewer than maxBurst, so all are allowed.
    EXPECT_EQ(limiter.check(base, 100ms, 3, 5s), RateLimitResult::Allowed);
    EXPECT_EQ(limiter.check(base + 200ms, 100ms, 3, 5s),
              RateLimitResult::Allowed);
    EXPECT_EQ(limiter.check(base + 400ms, 100ms, 3, 5s),
              RateLimitResult::Allowed);
}

TEST(BurstRateLimiter, RejectsWhenUnderMinOffset)
{
    BurstRateLimiter limiter;

    EXPECT_EQ(limiter.check(base, 100ms, 5, 5s), RateLimitResult::Allowed);

    // 50ms after the last message is inside the 100ms minimum offset.
    EXPECT_EQ(limiter.check(base + 50ms, 100ms, 5, 5s),
              RateLimitResult::TooFast);

    // A TooFast rejection records nothing, so the window stays empty apart from
    // the first message. Exactly at the offset boundary it is allowed again
    // (the check is strictly greater-than).
    EXPECT_EQ(limiter.check(base + 100ms, 100ms, 5, 5s),
              RateLimitResult::Allowed);
}

TEST(BurstRateLimiter, RejectsWhenBurstExceeded)
{
    BurstRateLimiter limiter;

    // maxBurst 3, tiny minOffset so pacing never trips, large cooldown so
    // nothing is evicted. Fill the window with three messages...
    EXPECT_EQ(limiter.check(base, 1ms, 3, 10s), RateLimitResult::Allowed);
    EXPECT_EQ(limiter.check(base + 2ms, 1ms, 3, 10s), RateLimitResult::Allowed);
    EXPECT_EQ(limiter.check(base + 4ms, 1ms, 3, 10s), RateLimitResult::Allowed);

    // ...the fourth is slow enough and old enough to pass those checks but the
    // window already holds maxBurst entries.
    EXPECT_EQ(limiter.check(base + 6ms, 1ms, 3, 10s), RateLimitResult::TooMany);
}

TEST(BurstRateLimiter, EvictsTimestampsOlderThanCooldown)
{
    BurstRateLimiter limiter;

    // maxBurst 2, cooldown 5s, tiny minOffset.
    EXPECT_EQ(limiter.check(base, 1ms, 2, 5s), RateLimitResult::Allowed);
    EXPECT_EQ(limiter.check(base + 1s, 1ms, 2, 5s), RateLimitResult::Allowed);

    // The window is full (two entries within the cooldown) -> TooMany.
    EXPECT_EQ(limiter.check(base + 2s, 1ms, 2, 5s), RateLimitResult::TooMany);

    // 6s after the first message, only that first timestamp is older than the
    // 5s cooldown and is evicted; the second (at base + 1s) survives. Capacity
    // for exactly one more frees up, so this message is allowed. The window now
    // holds {base + 1s, base + 6s}.
    EXPECT_EQ(limiter.check(base + 6s, 1ms, 2, 5s), RateLimitResult::Allowed);

    // 1ms later, base + 1s ages past the cooldown and is evicted, freeing one
    // more slot -> allowed. The window now holds {base + 6s, base + 6s + 1ms}.
    EXPECT_EQ(limiter.check(base + 6s + 1ms, 1ms, 2, 5s),
              RateLimitResult::Allowed);

    // With both remaining timestamps still inside the cooldown, the window is
    // full again.
    EXPECT_EQ(limiter.check(base + 6s + 2ms, 1ms, 2, 5s),
              RateLimitResult::TooMany);
}

TEST(BurstRateLimiter, ThrottleSurfacesOncePerInterval)
{
    // shouldNotify() models the 30s error throttle the send paths use for the
    // "too fast" / "too many" system messages.
    TimePoint lastNotified{};

    // First occurrence surfaces (default-constructed timestamp is far in the
    // past) and records the notification time.
    EXPECT_TRUE(shouldNotify(lastNotified, base, 30s));

    // Anything within the next 30s is suppressed, including the exact boundary.
    EXPECT_FALSE(shouldNotify(lastNotified, base + 10s, 30s));
    EXPECT_FALSE(shouldNotify(lastNotified, base + 29s, 30s));
    EXPECT_FALSE(shouldNotify(lastNotified, base + 30s, 30s));

    // Once the interval has fully elapsed it surfaces again and re-arms.
    EXPECT_TRUE(shouldNotify(lastNotified, base + 31s, 30s));
    EXPECT_FALSE(shouldNotify(lastNotified, base + 31s + 15s, 30s));
    EXPECT_TRUE(shouldNotify(lastNotified, base + 31s + 31s, 30s));
}
