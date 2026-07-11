// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <queue>

namespace chatterino {

/// Outcome of a single BurstRateLimiter::check() call.
enum class RateLimitResult {
    /// The message may be sent; its timestamp has been recorded.
    Allowed,
    /// The previous message is more recent than the minimum offset.
    TooFast,
    /// The burst window already holds the maximum number of messages.
    TooMany,
};

/// A sliding-window burst limiter over a queue of send timestamps.
///
/// This is the mechanical windowing shared by the Kick and Twitch outbound
/// send paths. It owns only the timestamp queue; the caller passes in the
/// configuration (minimum per-message offset, maximum burst count, and the
/// cooldown after which a timestamp is evicted) on every call so each platform
/// keeps its own values. It is deliberately clock-injected and free of any
/// global state so it can be unit-tested: the caller passes `now` rather than
/// the limiter reading a clock itself.
///
/// The limiter never emits UI. On TooFast / TooMany the caller decides whether
/// (and with which text) to surface a throttled system message; shouldNotify()
/// implements the shared 30s throttle for that.
class BurstRateLimiter
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    /// Evaluate whether a message sent at `now` is allowed.
    ///
    /// The checks run in the same order as the original inline code:
    ///   1. reject as TooFast if the last recorded message is newer than
    ///      `minOffset`;
    ///   2. evict every recorded timestamp older than `cooldown`;
    ///   3. reject as TooMany if the window still holds >= `maxBurst` entries;
    ///   4. otherwise record `now` and return Allowed.
    RateLimitResult check(TimePoint now, std::chrono::milliseconds minOffset,
                          size_t maxBurst, std::chrono::milliseconds cooldown)
    {
        // check if you are sending messages too fast
        if (!this->timestamps_.empty() &&
            this->timestamps_.back() + minOffset > now)
        {
            return RateLimitResult::TooFast;
        }

        // remove messages older than `cooldown`
        while (!this->timestamps_.empty() &&
               this->timestamps_.front() + cooldown < now)
        {
            this->timestamps_.pop();
        }

        // check if you are sending too many messages
        if (this->timestamps_.size() >= maxBurst)
        {
            return RateLimitResult::TooMany;
        }

        this->timestamps_.push(now);
        return RateLimitResult::Allowed;
    }

private:
    std::queue<TimePoint> timestamps_;
};

/// Shared 30s-style throttle for the "too fast" / "too many" system messages.
///
/// Returns true (and advances `lastNotified` to `now`) when at least `interval`
/// has elapsed since the last notification, so the caller surfaces the message
/// at most once per `interval`. `lastNotified` is owned by the caller so a
/// single throttle can be shared across several limiters (as Twitch does across
/// its mod and pleb queues) or kept per-limiter (as Kick does).
inline bool shouldNotify(BurstRateLimiter::TimePoint &lastNotified,
                         BurstRateLimiter::TimePoint now,
                         std::chrono::milliseconds interval)
{
    if (lastNotified + interval < now)
    {
        lastNotified = now;
        return true;
    }
    return false;
}

}  // namespace chatterino
