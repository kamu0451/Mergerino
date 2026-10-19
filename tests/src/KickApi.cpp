// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "providers/kick/KickApi.hpp"

#include "messages/Image.hpp"
#include "Test.hpp"
#include "util/BoostJsonWrap.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <QBuffer>
#include <QColor>
#include <QImage>
#include <QImageReader>

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

TEST(KickAssets, ParsesSelectedGlobalBadgeNamesAndImages)
{
    const auto data = boost::json::parse(R"({
        "id":123,"username":"Viewer","badges_v2":[
          {"name":"GOAT","badge_type":"global",
           "image_url":"https://ext.cdn.kick.com/chat/badges/GOAT.png",
           "selected":true,"sort_order":4},
          {"name":"rs_ags_blade","badge_type":"global",
           "image_url":"https://ext.cdn.kick.com/chat/badges/rs_ags_blade.png",
           "selected":false,"sort_order":5}]})")
                          .as_object();
    const KickPrivateUserInChannelInfo info{BoostJsonObject(data)};
    ASSERT_EQ(info.badges.size(), 2);
    EXPECT_EQ(info.badges[0].type, "GOAT");
    EXPECT_EQ(info.badges[0].text, "GOAT");
    EXPECT_TRUE(info.badges[0].selected);
    EXPECT_FALSE(info.badges[0].channelAssigned);
    EXPECT_EQ(info.badges[0].imageUrl,
              "https://ext.cdn.kick.com/chat/badges/GOAT.png");
    EXPECT_FALSE(info.badges[1].selected);
}

TEST(KickAssets, ParsesNestedBadgeImagesAndStringNumbers)
{
    const auto data = boost::json::parse(R"({"name":"subscriber",
      "count":"12","sort_order":"9","badge_image":{
      "src":"//files.kick.com/subscriber.png"}})")
                          .as_object();
    const KickPrivateUserBadgeInfo badge{BoostJsonObject(data)};
    EXPECT_EQ(badge.count, 12);
    EXPECT_EQ(badge.sortOrder, 9);
    EXPECT_EQ(badge.imageUrl, "https://files.kick.com/subscriber.png");

    const auto subData = boost::json::parse(R"({"months":"12",
      "badge_image":{"src":"https://files.kick.com/subscriber.png"}})")
                             .as_object();
    const KickPrivateSubscriberBadgeInfo sub{BoostJsonObject(subData)};
    EXPECT_EQ(sub.months, 12);
    EXPECT_EQ(sub.imageUrl, badge.imageUrl);
}

TEST(KickAssets, ChannelBadgesSortBeforeGlobalBadges)
{
    const auto data = boost::json::parse(R"({
        "id":123,"username":"Viewer","badges_v2":[
          {"name":"GOAT","badge_type":"global","sort_order":1,
           "image_url":"https://ext.cdn.kick.com/GOAT.png"},
          {"name":"cool_regular","badge_type":"channel","sort_order":7,
           "image_url":"https://files.kick.com/channel/cool.png"},
          {"type":"lead-moderator","sort_order":20}]})")
                          .as_object();
    const KickPrivateUserInChannelInfo info{BoostJsonObject(data)};
    ASSERT_EQ(info.badges.size(), 3);
    EXPECT_EQ(info.badges[0].type, "cool_regular");
    EXPECT_TRUE(info.badges[0].channelAssigned);
    EXPECT_EQ(info.badges[1].type, "lead-moderator");
    EXPECT_TRUE(info.badges[1].channelAssigned);
    EXPECT_EQ(info.badges[2].type, "GOAT");
    EXPECT_FALSE(info.badges[2].channelAssigned);
}

TEST(KickAssets, LargeFounderSizedImageDecodesWithinBadgeBounds)
{
    // Kick's 2022 founder badge is a static 3087 x 3087 PNG, which is over
    // Image::maxBytesRam when decoded at full size
    QImage source(3087, 3087, QImage::Format_ARGB32);
    source.fill(Qt::yellow);
    QByteArray data;
    QBuffer output(&data);
    ASSERT_TRUE(output.open(QIODevice::WriteOnly));
    ASSERT_TRUE(source.save(&output, "PNG"));
    output.close();
    source = {};

    QBuffer badgeBuffer(&data);
    ASSERT_TRUE(badgeBuffer.open(QIODevice::ReadOnly));
    QImageReader badgeReader(&badgeBuffer);
    const auto frames = detail::readFrames(
        badgeReader, {"https://example.test/founder.png"}, {72, 72});
    ASSERT_EQ(frames.size(), 1);
    EXPECT_EQ(frames[0].image.size(), QSize(72, 72));
    EXPECT_EQ(frames[0].image.toImage().pixelColor(36, 36), QColor(Qt::yellow));
}
