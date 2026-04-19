// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "util/ViewerCountDeltaTracker.hpp"
#include "Test.hpp"

using namespace chatterino;

namespace {

constexpr qint64 MINUTE_MS = 60LL * 1000LL;

}  // namespace

TEST(ViewerCountDeltaTracker, ReturnsEmptyUntilMinimumSpanElapses)
{
    ViewerCountDeltaTracker t;
    qint64 now = 1'000'000'000;
    // First sample: only one entry, no delta yet.
    EXPECT_FALSE(t.sampleAndCompute(100, now, 5).has_value());

    // 30 s later, only 30 s span - still below the 60 s minimum.
    now += 30 * 1000;
    EXPECT_FALSE(t.sampleAndCompute(110, now, 5).has_value());

    // 60 s total span crosses the threshold.
    now += 30 * 1000;
    auto delta = t.sampleAndCompute(110, now, 5);
    ASSERT_TRUE(delta.has_value());
    EXPECT_NEAR(delta->percent, 10.0, 0.01);  // 100 -> 110
    EXPECT_EQ(delta->spanMinutes, 1);
}

TEST(ViewerCountDeltaTracker, ComputesNegativeDelta)
{
    ViewerCountDeltaTracker t;
    qint64 now = 2'000'000'000;
    t.sampleAndCompute(200, now, 5);
    now += 2 * MINUTE_MS;
    auto delta = t.sampleAndCompute(150, now, 5);
    ASSERT_TRUE(delta.has_value());
    EXPECT_NEAR(delta->percent, -25.0, 0.01);
    EXPECT_GE(delta->spanMinutes, 1);
}

TEST(ViewerCountDeltaTracker, ReturnsEmptyWhenOldestSampleIsZero)
{
    ViewerCountDeltaTracker t;
    qint64 now = 3'000'000'000;
    t.sampleAndCompute(0, now, 5);
    now += 2 * MINUTE_MS;
    // Oldest sample was 0 - percentage is undefined, must refuse.
    EXPECT_FALSE(t.sampleAndCompute(50, now, 5).has_value());
}

TEST(ViewerCountDeltaTracker, PrunesSamplesOlderThanWindow)
{
    ViewerCountDeltaTracker t;
    qint64 now = 4'000'000'000;
    t.sampleAndCompute(100, now, 2);  // 2-minute window

    // 5 minutes later - old sample should be pruned; new sample stands alone.
    now += 5 * MINUTE_MS;
    auto delta = t.sampleAndCompute(200, now, 2);
    EXPECT_FALSE(delta.has_value());  // only one sample again
    EXPECT_EQ(t.sampleCount(), 1U);
}

TEST(ViewerCountDeltaTracker, ClearDropsAllSamples)
{
    ViewerCountDeltaTracker t;
    qint64 now = 5'000'000'000;
    t.sampleAndCompute(100, now, 5);
    now += 2 * MINUTE_MS;
    t.sampleAndCompute(150, now, 5);
    EXPECT_GT(t.sampleCount(), 1U);
    t.clear();
    EXPECT_EQ(t.sampleCount(), 0U);
}

TEST(ViewerCountDeltaTracker, ClampsWindowToValidRange)
{
    ViewerCountDeltaTracker t;
    qint64 now = 6'000'000'000;
    // Window of 0 clamps to minimum 1 minute. At exactly 60 s span, the
    // first sample is still within the (clamped) window and the delta
    // minimum-span guard (60 s) is satisfied.
    t.sampleAndCompute(100, now, 0);
    now += 60 * 1000;
    auto delta = t.sampleAndCompute(120, now, 0);
    ASSERT_TRUE(delta.has_value());
    EXPECT_EQ(delta->spanMinutes, 1);  // clamped to 1-minute window
}

TEST(ViewerCountDeltaTracker, CapsStoredSamplesAt200)
{
    ViewerCountDeltaTracker t;
    qint64 now = 7'000'000'000;
    // Feed 300 samples with alternating counts (forces each to be pushed
    // because the count changed on every call) - should cap at 200.
    for (int i = 0; i < 300; ++i)
    {
        t.sampleAndCompute(100 + (i % 2), now, 60);
        now += 10;  // 10 ms between samples
    }
    EXPECT_LE(t.sampleCount(), 200U);
}
