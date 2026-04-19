// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#pragma once

#include <QtGlobal>

#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

namespace chatterino {

/// Sliding-window percentage-delta tracker for a single time-series counter
/// (e.g. viewers of a stream). Platform-agnostic: the caller feeds raw
/// viewer counts plus a "now" timestamp, and the tracker returns the
/// % change vs. the oldest sample within the requested window.
///
/// Callers own an instance per logical view / split, so each display can
/// have its own independent window size and sample history.
class ViewerCountDeltaTracker
{
public:
    struct Delta {
        double percent = 0.0;
        int spanMinutes = 0;
    };

    /// Records the latest viewer count (if meaningfully changed or enough
    /// time has elapsed) and returns the delta vs. the oldest sample
    /// within the requested window. Returns std::nullopt if there isn't
    /// enough data yet (fewer than two samples, under 60 s span, or the
    /// oldest sample was zero so no percentage is defined).
    ///
    /// @param currentCount   the latest total viewer count
    /// @param nowMs          milliseconds-since-epoch of the sample
    /// @param windowMinutes  desired window length, clamped to [1, 60]
    std::optional<Delta> sampleAndCompute(unsigned currentCount, qint64 nowMs,
                                          int windowMinutes);

    /// Drops all recorded samples. Useful when a source channel changes
    /// or the user navigates away.
    void clear();

    /// Introspection for tests.
    std::size_t sampleCount() const
    {
        return this->samples_.size();
    }

private:
    // (msSinceEpoch, viewer count) pairs, oldest at front.
    std::deque<std::pair<qint64, unsigned>> samples_;

    static constexpr qint64 MIN_SPAN_MS = 60LL * 1000LL;
    static constexpr qint64 SAMPLE_INTERVAL_MS = 30LL * 1000LL;
    static constexpr std::size_t MAX_ENTRIES = 200;
    static constexpr int WINDOW_MIN_MINUTES = 1;
    static constexpr int WINDOW_MAX_MINUTES = 60;
};

}  // namespace chatterino
