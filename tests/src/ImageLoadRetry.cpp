// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/Image.hpp"

#include "Test.hpp"

#include <QNetworkReply>

#include <optional>

using namespace chatterino;

namespace {

using Err = QNetworkReply::NetworkError;

}  // namespace

// Transport-layer failures that produced no HTTP response should be retried.
TEST(ImageLoadRetry, TransientNetworkErrors)
{
    EXPECT_TRUE(isTransientImageLoadError(Err::TimeoutError, std::nullopt));
    EXPECT_TRUE(
        isTransientImageLoadError(Err::ConnectionRefusedError, std::nullopt));
    EXPECT_TRUE(
        isTransientImageLoadError(Err::RemoteHostClosedError, std::nullopt));
    EXPECT_TRUE(
        isTransientImageLoadError(Err::HostNotFoundError, std::nullopt));
    EXPECT_TRUE(isTransientImageLoadError(
        Err::TemporaryNetworkFailureError, std::nullopt));
    EXPECT_TRUE(
        isTransientImageLoadError(Err::NetworkSessionFailedError, std::nullopt));
    EXPECT_TRUE(
        isTransientImageLoadError(Err::ProxyTimeoutError, std::nullopt));
    EXPECT_TRUE(
        isTransientImageLoadError(Err::UnknownNetworkError, std::nullopt));
}

// Cancellation / policy / redirect failures are not worth retrying.
TEST(ImageLoadRetry, DefinitiveNetworkErrors)
{
    EXPECT_FALSE(
        isTransientImageLoadError(Err::OperationCanceledError, std::nullopt));
    EXPECT_FALSE(
        isTransientImageLoadError(Err::TooManyRedirectsError, std::nullopt));
    EXPECT_FALSE(
        isTransientImageLoadError(Err::InsecureRedirectError, std::nullopt));
    EXPECT_FALSE(isTransientImageLoadError(
        Err::BackgroundRequestNotAllowedError, std::nullopt));
}

// A resolved HTTP status dominates the classification.
TEST(ImageLoadRetry, HttpStatusClassification)
{
    // 5xx server errors are transient.
    EXPECT_TRUE(isTransientImageLoadError(Err::InternalServerError, 500));
    EXPECT_TRUE(isTransientImageLoadError(Err::ServiceUnavailableError, 503));
    EXPECT_TRUE(isTransientImageLoadError(Err::UnknownServerError, 599));

    // 408 Request Timeout and 429 Too Many Requests are transient.
    EXPECT_TRUE(isTransientImageLoadError(Err::UnknownContentError, 408));
    EXPECT_TRUE(isTransientImageLoadError(Err::UnknownContentError, 429));

    // 404 / 410 and other 4xx are definitive misses.
    EXPECT_FALSE(isTransientImageLoadError(Err::ContentNotFoundError, 404));
    EXPECT_FALSE(isTransientImageLoadError(Err::ContentGoneError, 410));
    EXPECT_FALSE(isTransientImageLoadError(Err::ContentAccessDenied, 403));
    EXPECT_FALSE(
        isTransientImageLoadError(Err::ProtocolInvalidOperationError, 400));

    // A present 2xx status (degenerate in onError) is not transient.
    EXPECT_FALSE(isTransientImageLoadError(Err::NoError, 200));
}

// The HTTP status takes precedence even when the network-error enum would
// classify differently on its own.
TEST(ImageLoadRetry, StatusOverridesErrorEnum)
{
    // 404 with a content-not-found enum stays definitive.
    EXPECT_FALSE(isTransientImageLoadError(Err::ContentNotFoundError, 404));
    // A 503 is transient regardless of the accompanying enum.
    EXPECT_TRUE(isTransientImageLoadError(Err::ContentNotFoundError, 503));
}
