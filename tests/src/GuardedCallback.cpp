// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "util/GuardedCallback.hpp"
#include "Test.hpp"

#include <memory>

using namespace chatterino;

TEST(GuardedCallback, RunsCallableWhileGuardAlive)
{
    auto guard = std::make_shared<bool>(true);
    int callCount = 0;
    auto cb = guardedCallback(guard, [&callCount] {
        ++callCount;
    });

    cb();
    cb();
    EXPECT_EQ(callCount, 2);
}

TEST(GuardedCallback, SkipsCallableAfterGuardDrops)
{
    auto guard = std::make_shared<bool>(true);
    int callCount = 0;
    auto cb = guardedCallback(guard, [&callCount] {
        ++callCount;
    });

    cb();
    EXPECT_EQ(callCount, 1);

    guard.reset();  // owner "dies"
    cb();
    cb();
    EXPECT_EQ(callCount, 1);  // no further invocations
}

TEST(GuardedCallback, ForwardsArgumentsWhileGuardAlive)
{
    auto guard = std::make_shared<bool>(true);
    int received = 0;
    auto cb = guardedCallback(guard, [&received](int a, int b) {
        received = a + b;
    });

    cb(3, 4);
    EXPECT_EQ(received, 7);
}

TEST(GuardedCallback, DoesNotFireCallableAfterExpiry)
{
    auto guard = std::make_shared<bool>(true);
    bool fired = false;
    auto cb = guardedCallback(guard, [&fired] {
        fired = true;
    });

    // The weak_ptr inside the lambda still holds a reference to the control
    // block but lock() returns null once the shared_ptr drops. Proves that
    // callsites can safely outlive their owner.
    guard.reset();
    cb();
    EXPECT_FALSE(fired);
}

TEST(GuardedCallback, WeakPtrOverloadBehavesTheSame)
{
    auto guard = std::make_shared<bool>(true);
    std::weak_ptr<bool> weak = guard;
    int callCount = 0;
    auto cb = guardedCallback(weak, [&callCount] {
        ++callCount;
    });

    cb();
    EXPECT_EQ(callCount, 1);

    guard.reset();
    cb();
    EXPECT_EQ(callCount, 1);
}

TEST(GuardedCallback, ExtendsLifetimeDuringCall)
{
    // The wrapper does guard.lock() before dispatching, so the shared_ptr
    // is alive for the duration of the callable - a callable that drops
    // its containing object mid-call still runs to completion.
    auto guard = std::make_shared<bool>(true);
    std::weak_ptr<bool> weakSeenInside;
    auto cb = guardedCallback(guard, [&weakSeenInside, &guard] {
        // Observe that the lock() kept the control block alive even if
        // the outer guard is reset from inside the callable.
        weakSeenInside = std::weak_ptr<bool>(guard);
        guard.reset();
        EXPECT_FALSE(weakSeenInside.expired());
    });

    cb();
    EXPECT_TRUE(weakSeenInside.expired());
}
