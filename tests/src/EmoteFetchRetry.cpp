// SPDX-FileCopyrightText: 2026 Mergerino
//
// SPDX-License-Identifier: MIT

#include "common/network/NetworkResult.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"

#include "Test.hpp"

using namespace chatterino;

using Error = NetworkResult::NetworkError;

// STAB-05: BTTV/FFZ global+channel emote fetches retry on transient network
// failures (transport errors / 5xx / timeout) but must not retry on a
// definitive 4xx HTTP response - it won't change on a retry.

TEST(BttvEmoteFetchRetry, RetriesOnTransportFailure)
{
    // No HTTP status was ever received (timeout, connection reset, DNS
    // failure, etc.) - that's transient and worth retrying.
    NetworkResult timeout(Error::TimeoutError, {}, {});
    ASSERT_TRUE(bttv::detail::isRetryableFetchError(timeout));

    NetworkResult hostClosed(Error::RemoteHostClosedError, {}, {});
    ASSERT_TRUE(bttv::detail::isRetryableFetchError(hostClosed));

    NetworkResult connRefused(Error::ConnectionRefusedError, {}, {});
    ASSERT_TRUE(bttv::detail::isRetryableFetchError(connRefused));
}

TEST(BttvEmoteFetchRetry, RetriesOn5xx)
{
    NetworkResult internalError(Error::InternalServerError, 500, {});
    ASSERT_TRUE(bttv::detail::isRetryableFetchError(internalError));

    NetworkResult unavailable(Error::ServiceUnavailableError, 503, {});
    ASSERT_TRUE(bttv::detail::isRetryableFetchError(unavailable));
}

TEST(BttvEmoteFetchRetry, DoesNotRetryOn4xx)
{
    NetworkResult notFound(Error::ContentNotFoundError, 404, {});
    ASSERT_FALSE(bttv::detail::isRetryableFetchError(notFound));

    NetworkResult accessDenied(Error::ContentAccessDenied, 403, {});
    ASSERT_FALSE(bttv::detail::isRetryableFetchError(accessDenied));

    NetworkResult badRequest(Error::ProtocolInvalidOperationError, 400, {});
    ASSERT_FALSE(bttv::detail::isRetryableFetchError(badRequest));
}

TEST(FfzEmoteFetchRetry, RetriesOnTransportFailure)
{
    NetworkResult timeout(Error::TimeoutError, {}, {});
    ASSERT_TRUE(ffz::detail::isRetryableFetchError(timeout));

    NetworkResult hostClosed(Error::RemoteHostClosedError, {}, {});
    ASSERT_TRUE(ffz::detail::isRetryableFetchError(hostClosed));

    NetworkResult connRefused(Error::ConnectionRefusedError, {}, {});
    ASSERT_TRUE(ffz::detail::isRetryableFetchError(connRefused));
}

TEST(FfzEmoteFetchRetry, RetriesOn5xx)
{
    NetworkResult internalError(Error::InternalServerError, 500, {});
    ASSERT_TRUE(ffz::detail::isRetryableFetchError(internalError));

    NetworkResult unavailable(Error::ServiceUnavailableError, 503, {});
    ASSERT_TRUE(ffz::detail::isRetryableFetchError(unavailable));
}

TEST(FfzEmoteFetchRetry, DoesNotRetryOn4xx)
{
    NetworkResult notFound(Error::ContentNotFoundError, 404, {});
    ASSERT_FALSE(ffz::detail::isRetryableFetchError(notFound));

    NetworkResult accessDenied(Error::ContentAccessDenied, 403, {});
    ASSERT_FALSE(ffz::detail::isRetryableFetchError(accessDenied));

    NetworkResult badRequest(Error::ProtocolInvalidOperationError, 400, {});
    ASSERT_FALSE(ffz::detail::isRetryableFetchError(badRequest));
}
