// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "util/ViewerCountDeltaTracker.hpp"

#include <algorithm>

namespace chatterino {

std::optional<ViewerCountDeltaTracker::Delta>
ViewerCountDeltaTracker::sampleAndCompute(unsigned currentCount, qint64 nowMs,
                                          int windowMinutes)
{
    const int clampedWindow =
        std::clamp(windowMinutes, WINDOW_MIN_MINUTES, WINDOW_MAX_MINUTES);
    const qint64 windowMs = static_cast<qint64>(clampedWindow) * 60LL * 1000LL;

    // Prune samples outside the window.
    while (!this->samples_.empty() &&
           nowMs - this->samples_.front().first > windowMs)
    {
        this->samples_.pop_front();
    }

    // Discontinuity guard: if the current count is wildly different from
    // the oldest sample in the window (>3x up or down), the window now
    // straddles two regimes (e.g. another merged source just came
    // online, bumping the total from 126 to 647). That produces a bogus
    // "+400%" delta that isn't real viewer flux. Drop the prior window
    // and start fresh from this sample.
    if (!this->samples_.empty())
    {
        const auto oldest = this->samples_.front().second;
        if (oldest > 0 && currentCount > 0)
        {
            const auto hi = std::max(currentCount, oldest);
            const auto lo = std::min(currentCount, oldest);
            if (static_cast<double>(hi) > static_cast<double>(lo) * 3.0)
            {
                this->samples_.clear();
            }
        }
    }

    // Push a fresh sample if empty, the count changed, or the sampling
    // interval has elapsed since the last sample. The interval keeps a
    // stable count from spamming entries; the change check keeps a
    // bouncing count responsive.
    if (this->samples_.empty() ||
        this->samples_.back().second != currentCount ||
        nowMs - this->samples_.back().first >= SAMPLE_INTERVAL_MS)
    {
        this->samples_.emplace_back(nowMs, currentCount);
        while (this->samples_.size() > MAX_ENTRIES)
        {
            this->samples_.pop_front();
        }
    }

    if (this->samples_.size() < 2)
    {
        return std::nullopt;
    }
    const auto &oldest = this->samples_.front();
    const qint64 spanMs = nowMs - oldest.first;
    if (spanMs < MIN_SPAN_MS || oldest.second == 0)
    {
        return std::nullopt;
    }

    const double delta = static_cast<double>(currentCount) -
                         static_cast<double>(oldest.second);
    const double percent =
        (delta / static_cast<double>(oldest.second)) * 100.0;
    const int spanMinutes =
        std::clamp(static_cast<int>((spanMs + 30LL * 1000LL) / (60LL * 1000LL)),
                   1, clampedWindow);
    return Delta{percent, spanMinutes};
}

void ViewerCountDeltaTracker::clear()
{
    this->samples_.clear();
}

}  // namespace chatterino
