// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "providers/kick/KickApi.hpp"

#include "Test.hpp"
#include "util/BoostJsonWrap.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

using namespace chatterino;

TEST(KickApi, PreservesChannelSlugSeparators)
{
    EXPECT_EQ(KickApi::slugify(QStringLiteral("channel_with_underscores")),
              QStringLiteral("channel_with_underscores"));
    EXPECT_EQ(KickApi::slugify(QStringLiteral("hyphen-channel")),
              QStringLiteral("hyphen-channel"));
}

TEST(KickApi, ParsesAuthoritativeChannelRolesWithoutSelectedBadges)
{
    const boost::json::object payload{
        {"id", 123},
        {"username", "ModUser"},
        {"is_moderator", true},
        {"is_channel_owner", false},
        {"badges", boost::json::array{}},
        {"badges_v2", boost::json::array{}},
    };

    const KickPrivateUserInChannelInfo info{BoostJsonObject(payload)};
    EXPECT_EQ(info.userID, 123U);
    EXPECT_EQ(info.username, QStringLiteral("ModUser"));
    EXPECT_TRUE(info.isModerator);
    EXPECT_FALSE(info.isChannelOwner);
}

TEST(KickApi, ParsesChannelOwnerSeparatelyFromModerator)
{
    const boost::json::object payload{
        {"id", 456},
        {"username", "Broadcaster"},
        {"is_moderator", false},
        {"is_channel_owner", true},
        {"badges", boost::json::array{}},
        {"badges_v2", boost::json::array{}},
    };

    const KickPrivateUserInChannelInfo info{BoostJsonObject(payload)};
    EXPECT_FALSE(info.isModerator);
    EXPECT_TRUE(info.isChannelOwner);
}
