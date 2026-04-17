// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "providers/tiktok/TikTokFrameDecoder.hpp"
#include "providers/tiktok/TikTokLiveChat.hpp"
#include "Test.hpp"

#include <QByteArray>

#include <cstdint>

using namespace chatterino;

namespace {

// Minimal protobuf writer for constructing synthetic webcast frames. Mirrors
// the wire format the decoder consumes - only the varint + length-delimited
// paths we exercise. Kept in the test TU so production code carries no
// encoder surface it doesn't need.
class PbWriter
{
public:
    PbWriter &varint(std::uint64_t v)
    {
        while (v >= 0x80U)
        {
            buf_.append(static_cast<char>((v & 0x7FU) | 0x80U));
            v >>= 7;
        }
        buf_.append(static_cast<char>(v));
        return *this;
    }

    PbWriter &tag(std::uint32_t field, std::uint32_t wire)
    {
        return this->varint((static_cast<std::uint64_t>(field) << 3) | wire);
    }

    PbWriter &int64Field(std::uint32_t field, std::int64_t value)
    {
        this->tag(field, 0);
        return this->varint(static_cast<std::uint64_t>(value));
    }

    PbWriter &int32Field(std::uint32_t field, std::int32_t value)
    {
        this->tag(field, 0);
        return this->varint(static_cast<std::uint32_t>(value));
    }

    PbWriter &stringField(std::uint32_t field, const QByteArray &value)
    {
        this->tag(field, 2);
        this->varint(static_cast<std::uint64_t>(value.size()));
        buf_.append(value);
        return *this;
    }

    PbWriter &bytesField(std::uint32_t field, const QByteArray &value)
    {
        return this->stringField(field, value);
    }

    QByteArray take() const
    {
        return buf_;
    }

private:
    QByteArray buf_;
};

QByteArray buildUser(std::int64_t id, const QByteArray &nick,
                     const QByteArray &uniqueId)
{
    PbWriter w;
    if (id != 0)
    {
        w.int64Field(1, id);
    }
    if (!nick.isEmpty())
    {
        w.stringField(3, nick);
    }
    if (!uniqueId.isEmpty())
    {
        w.stringField(38, uniqueId);
    }
    return w.take();
}

// Wraps an inner message payload into the full push-frame envelope so the
// decoder sees the layout that actually comes off the wire:
//   WebcastPushFrame{ payload_type="msg", payload=
//     ProtoMessageFetchResult{ messages=[ BaseMessage{ method, payload=inner } ] } }
QByteArray wrapFrame(const QByteArray &method, const QByteArray &innerPayload)
{
    const auto baseMsg =
        PbWriter().stringField(1, method).bytesField(2, innerPayload).take();
    const auto fetchResult = PbWriter().bytesField(1, baseMsg).take();
    return PbWriter()
        .stringField(7, QByteArrayLiteral("msg"))
        .bytesField(8, fetchResult)
        .take();
}

}  // namespace

TEST(TikTokLiveChat, normalizeSourceAcceptsBareUsername)
{
    EXPECT_EQ(TikTokLiveChat::normalizeSource("gevad1ch"), "gevad1ch");
}

TEST(TikTokLiveChat, normalizeSourceStripsAtPrefix)
{
    EXPECT_EQ(TikTokLiveChat::normalizeSource("@gevad1ch"), "gevad1ch");
}

TEST(TikTokLiveChat, normalizeSourceExtractsFromFullUrl)
{
    EXPECT_EQ(TikTokLiveChat::normalizeSource(
                  "https://www.tiktok.com/@gevad1ch/live"),
              "gevad1ch");
}

TEST(TikTokLiveChat, normalizeSourceStripsTrackingParams)
{
    EXPECT_EQ(TikTokLiveChat::normalizeSource(
                  "https://www.tiktok.com/@gevad1ch/live"
                  "?enter_from_merge=others_homepage&enter_method=video"),
              "gevad1ch");
}

TEST(TikTokLiveChat, normalizeSourceAcceptsMobileHost)
{
    EXPECT_EQ(TikTokLiveChat::normalizeSource("https://m.tiktok.com/@foo"),
              "foo");
}

TEST(TikTokLiveChat, normalizeSourceReturnsEmptyOnBlankInput)
{
    EXPECT_TRUE(TikTokLiveChat::normalizeSource("").isEmpty());
    EXPECT_TRUE(TikTokLiveChat::normalizeSource("   ").isEmpty());
}

TEST(TikTokFrameDecoder, decodesChatMessage)
{
    const auto userBytes =
        buildUser(99, QByteArrayLiteral("geva"),
                  QByteArrayLiteral("gevad1ch"));
    const auto chatBody = PbWriter()
                              .bytesField(2, userBytes)
                              .stringField(3, QByteArrayLiteral("hello"))
                              .take();
    const auto wire = wrapFrame(QByteArrayLiteral("WebcastChatMessage"),
                                chatBody);

    const auto frame = tiktok::decodeWebcastPushFrame(wire);

    ASSERT_EQ(frame.chatMessages.size(), 1U);
    EXPECT_EQ(frame.chatMessages[0].content, "hello");
    EXPECT_EQ(frame.chatMessages[0].user.userId, 99);
    EXPECT_EQ(frame.chatMessages[0].user.nickname, "geva");
    EXPECT_EQ(frame.chatMessages[0].user.uniqueId, "gevad1ch");
}

TEST(TikTokFrameDecoder, decodesLikeEvent)
{
    const auto userBytes =
        buildUser(0, QByteArrayLiteral("fan"), QByteArrayLiteral(""));
    const auto body = PbWriter()
                          .int32Field(2, 3)
                          .int64Field(3, 42)
                          .bytesField(5, userBytes)
                          .take();
    const auto wire =
        wrapFrame(QByteArrayLiteral("WebcastLikeMessage"), body);

    const auto frame = tiktok::decodeWebcastPushFrame(wire);

    ASSERT_EQ(frame.likeEvents.size(), 1U);
    EXPECT_EQ(frame.likeEvents[0].count, 3);
    EXPECT_EQ(frame.likeEvents[0].total, 42);
    EXPECT_EQ(frame.likeEvents[0].user.nickname, "fan");
    EXPECT_TRUE(frame.chatMessages.empty());
}

TEST(TikTokFrameDecoder, decodesMemberEvent)
{
    const auto userBytes =
        buildUser(7, QByteArrayLiteral("joiner"), QByteArrayLiteral(""));
    const auto body = PbWriter().bytesField(2, userBytes).int32Field(9, 1).take();
    const auto wire =
        wrapFrame(QByteArrayLiteral("WebcastMemberMessage"), body);

    const auto frame = tiktok::decodeWebcastPushFrame(wire);

    ASSERT_EQ(frame.memberEvents.size(), 1U);
    EXPECT_EQ(frame.memberEvents[0].enterType, 1);
    EXPECT_EQ(frame.memberEvents[0].user.nickname, "joiner");
}

TEST(TikTokFrameDecoder, decodesSocialEventWithFollowAction)
{
    const auto userBytes =
        buildUser(0, QByteArrayLiteral("newfollower"), QByteArrayLiteral(""));
    const auto body = PbWriter().bytesField(2, userBytes).int64Field(4, 1).take();
    const auto wire =
        wrapFrame(QByteArrayLiteral("WebcastSocialMessage"), body);

    const auto frame = tiktok::decodeWebcastPushFrame(wire);

    ASSERT_EQ(frame.socialEvents.size(), 1U);
    EXPECT_EQ(frame.socialEvents[0].action, 1);
    EXPECT_EQ(frame.socialEvents[0].user.nickname, "newfollower");
}

TEST(TikTokFrameDecoder, decodesGiftEventWithFinalFrameMarker)
{
    const auto userBytes =
        buildUser(0, QByteArrayLiteral("gifter"), QByteArrayLiteral(""));
    const auto body = PbWriter()
                          .int32Field(5, 3)
                          .bytesField(7, userBytes)
                          .int32Field(9, 1)
                          .take();
    const auto wire =
        wrapFrame(QByteArrayLiteral("WebcastGiftMessage"), body);

    const auto frame = tiktok::decodeWebcastPushFrame(wire);

    ASSERT_EQ(frame.giftEvents.size(), 1U);
    EXPECT_EQ(frame.giftEvents[0].repeatCount, 3);
    EXPECT_EQ(frame.giftEvents[0].repeatEnd, 1);
    EXPECT_EQ(frame.giftEvents[0].fromUser.nickname, "gifter");
}

TEST(TikTokFrameDecoder, dropsUnknownMethod)
{
    const auto body = PbWriter().stringField(1, QByteArrayLiteral("x")).take();
    const auto wire =
        wrapFrame(QByteArrayLiteral("WebcastNotYetHandled"), body);

    const auto frame = tiktok::decodeWebcastPushFrame(wire);

    EXPECT_TRUE(frame.chatMessages.empty());
    EXPECT_TRUE(frame.likeEvents.empty());
    EXPECT_TRUE(frame.memberEvents.empty());
    EXPECT_TRUE(frame.socialEvents.empty());
    EXPECT_TRUE(frame.giftEvents.empty());
}

TEST(TikTokFrameDecoder, survivesTruncatedFrame)
{
    // Deliberately cut a varint off mid-byte - decoder must bail out, not
    // crash or emit garbage.
    const QByteArray junk = QByteArrayLiteral("\x38\xFF\xFF");
    const auto frame = tiktok::decodeWebcastPushFrame(junk);
    EXPECT_TRUE(frame.chatMessages.empty());
}

TEST(TikTokFrameDecoder, skipsNonMsgPayloadTypes)
{
    const auto body = PbWriter().stringField(1, QByteArrayLiteral("ping")).take();
    const auto wire = PbWriter()
                          .stringField(7, QByteArrayLiteral("hb"))
                          .bytesField(8, body)
                          .take();

    const auto frame = tiktok::decodeWebcastPushFrame(wire);

    EXPECT_EQ(frame.payloadType, "hb");
    EXPECT_TRUE(frame.chatMessages.empty());
}
