// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "providers/kick/KickApi.hpp"

#include "Test.hpp"

using namespace chatterino;

TEST(KickApi, PreservesChannelSlugSeparators)
{
    EXPECT_EQ(KickApi::slugify(QStringLiteral("channel_with_underscores")),
              QStringLiteral("channel_with_underscores"));
    EXPECT_EQ(KickApi::slugify(QStringLiteral("hyphen-channel")),
              QStringLiteral("hyphen-channel"));
}
