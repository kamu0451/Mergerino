// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <utility>

namespace chatterino {

/// Wraps a callable so that it becomes a no-op once the weak guard has
/// expired.
///
/// Intended for async callbacks (QTimer, network, QFutureWatcher, pajlada
/// signals) that outlive their owning object. The owner holds a
/// `std::shared_ptr<bool>` as a lifetime token and passes a weak copy into
/// the callback; when the owner is destroyed the shared_ptr drops, the
/// weak_ptr expires, and the next invocation falls through silently
/// instead of dereferencing a dangling `this`.
///
/// Usage:
///   std::shared_ptr<bool> lifetimeGuard_;
///   ...
///   QTimer::singleShot(delayMs, guardedCallback(lifetimeGuard_, [this] {
///       // only runs if *this is still alive
///       this->doWork();
///   }));
///
/// The wrapped callable receives any forwarded arguments. Call-site state
/// checks (e.g. `if (!this->running_) return;`) stay inside the callable;
/// this helper only handles the lifetime half of the check.
template <typename Fn>
auto guardedCallback(std::weak_ptr<bool> guard, Fn fn)
{
    return [guard = std::move(guard),
            fn = std::move(fn)](auto &&...args) mutable {
        if (auto keepalive = guard.lock())
        {
            fn(std::forward<decltype(args)>(args)...);
        }
    };
}

/// Shared-pointer overload for the common case of passing the owner's
/// lifetime token directly - caller doesn't have to construct a weak_ptr.
template <typename Fn>
auto guardedCallback(const std::shared_ptr<bool> &guard, Fn fn)
{
    return guardedCallback(std::weak_ptr<bool>(guard), std::move(fn));
}

}  // namespace chatterino
