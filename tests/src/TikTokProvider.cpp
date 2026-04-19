// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "providers/tiktok/TikTokFrameDecoder.hpp"
#include "providers/tiktok/TikTokLiveChat.hpp"
#include "Test.hpp"

#include <QByteArray>

#include <cstdint>
#include <random>

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

TEST(TikTokLiveChat, normalizeSourceAcceptsUppercaseHostAndScheme)
{
    EXPECT_EQ(TikTokLiveChat::normalizeSource(
                  "HTTPS://WWW.TIKTOK.COM/@gevad1ch/live"),
              "gevad1ch");
}

TEST(TikTokLiveChat, normalizeSourcePreservesUsernameCase)
{
    // TikTok usernames are case-preserving in our normalization - the regex
    // captures the literal segment. Downstream code that lowercases for
    // display should do so explicitly.
    EXPECT_EQ(TikTokLiveChat::normalizeSource(
                  "https://www.tiktok.com/@GevadiCh/live"),
              "GevadiCh");
}

TEST(TikTokLiveChat, normalizeSourceStripsLiveSuffixFromBareName)
{
    EXPECT_EQ(TikTokLiveChat::normalizeSource("gevad1ch/live"), "gevad1ch");
    EXPECT_EQ(TikTokLiveChat::normalizeSource("@gevad1ch/live"), "gevad1ch");
}

TEST(TikTokLiveChat, normalizeSourceStripsTrailingSlashAfterName)
{
    EXPECT_EQ(TikTokLiveChat::normalizeSource(
                  "https://www.tiktok.com/@gevad1ch/"),
              "gevad1ch");
}

TEST(TikTokLiveChat, normalizeSourceAcceptsUsernameWithDotsAndUnderscores)
{
    EXPECT_EQ(TikTokLiveChat::normalizeSource("@foo.bar_123"), "foo.bar_123");
}

TEST(TikTokLiveChat, normalizeSourceStripsBareNameQueryParams)
{
    EXPECT_EQ(TikTokLiveChat::normalizeSource("gevad1ch?x=1"), "gevad1ch");
}

TEST(TikTokLiveChat, normalizeSourceReturnsEmptyForBareAtSymbol)
{
    EXPECT_TRUE(TikTokLiveChat::normalizeSource("@").isEmpty());
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

// Property-style fuzz cases. The decoder consumes arbitrary bytes from a
// TikTok-operated WebSocket; any input (including intentionally malicious
// or corrupt bytes) must yield a valid-but-possibly-empty DecodedFrame and
// must not crash, loop forever, or read past its buffer.

TEST(TikTokFrameDecoder, acceptsEmptyInput)
{
    const auto frame = tiktok::decodeWebcastPushFrame(QByteArray{});
    EXPECT_TRUE(frame.payloadType.isEmpty());
    EXPECT_TRUE(frame.chatMessages.empty());
    EXPECT_TRUE(frame.likeEvents.empty());
    EXPECT_TRUE(frame.memberEvents.empty());
    EXPECT_TRUE(frame.socialEvents.empty());
    EXPECT_TRUE(frame.giftEvents.empty());
}

TEST(TikTokFrameDecoder, acceptsAllZeroes)
{
    // 16 zero bytes - first byte is tag=0 which has no valid wire type, so
    // the reader should bail on the first readTag without dereferencing
    // anything unsafe.
    const QByteArray zeros(16, '\0');
    const auto frame = tiktok::decodeWebcastPushFrame(zeros);
    EXPECT_TRUE(frame.chatMessages.empty());
}

TEST(TikTokFrameDecoder, survivesLengthClaimingMoreThanBufferHas)
{
    // Tag for field=8 wire=2 (length-delimited) then a varint claiming 1000
    // bytes of payload while the buffer has none. Reader must refuse,
    // not read past end.
    const QByteArray lenAttack =
        QByteArrayLiteral("\x42\xE8\x07");  // field=8, wire=2, len=1000
    const auto frame = tiktok::decodeWebcastPushFrame(lenAttack);
    EXPECT_TRUE(frame.chatMessages.empty());
    EXPECT_TRUE(frame.payloadType.isEmpty());
}

TEST(TikTokFrameDecoder, survivesOverlongVarint)
{
    // A varint longer than 10 bytes can't represent any valid uint64.
    // Feed 11 high-bit-set bytes where a varint is expected.
    QByteArray overlong;
    overlong.append('\x38');  // field=7, wire=0 (varint) tag
    for (int i = 0; i < 11; ++i)
    {
        overlong.append('\xFF');
    }
    const auto frame = tiktok::decodeWebcastPushFrame(overlong);
    EXPECT_TRUE(frame.chatMessages.empty());
}

TEST(TikTokFrameDecoder, acceptsNonUtf8InStringFields)
{
    // A chat message whose content bytes aren't valid UTF-8 must still
    // decode (QString::fromUtf8 substitutes replacement chars) rather
    // than being rejected outright.
    const auto userBytes =
        buildUser(1, QByteArrayLiteral("user"), QByteArrayLiteral("uid"));
    QByteArray badContent;
    badContent.append('\xC3');  // lead byte of a 2-byte UTF-8 sequence
    badContent.append('\x28');  // invalid continuation - not 10xxxxxx
    const auto chatBody =
        PbWriter().bytesField(2, userBytes).stringField(3, badContent).take();
    const auto wire =
        wrapFrame(QByteArrayLiteral("WebcastChatMessage"), chatBody);

    const auto frame = tiktok::decodeWebcastPushFrame(wire);

    ASSERT_EQ(frame.chatMessages.size(), 1U);
    EXPECT_FALSE(frame.chatMessages[0].content.isEmpty());
}

TEST(TikTokFrameDecoder, handlesMsgPayloadTypeWithNoBody)
{
    // Outer envelope declares payload_type="msg" but has no payload=
    // field. Decoder should accept and return empty events.
    const auto wire =
        PbWriter().stringField(7, QByteArrayLiteral("msg")).take();
    const auto frame = tiktok::decodeWebcastPushFrame(wire);
    EXPECT_EQ(frame.payloadType, "msg");
    EXPECT_TRUE(frame.chatMessages.empty());
}

TEST(TikTokFrameDecoder, handlesBaseMessageWithUnknownMethodAndNoPayload)
{
    // BaseMessage carrying only method= but no payload= bytes. Decoder
    // must skip cleanly without crashing on the missing payload.
    const auto baseMsg =
        PbWriter().stringField(1, QByteArrayLiteral("WebcastMystery")).take();
    const auto fetchResult = PbWriter().bytesField(1, baseMsg).take();
    const auto wire = PbWriter()
                          .stringField(7, QByteArrayLiteral("msg"))
                          .bytesField(8, fetchResult)
                          .take();
    const auto frame = tiktok::decodeWebcastPushFrame(wire);
    EXPECT_EQ(frame.payloadType, "msg");
    EXPECT_TRUE(frame.chatMessages.empty());
}

TEST(TikTokFrameDecoder, survivesRandomBytes)
{
    // 200 rounds of pseudo-random bytes fed to the decoder. Any of these
    // that hits a parsing error must bail cleanly without crashing, and
    // any that happens to parse as a valid frame (vanishingly unlikely
    // at these sizes) is still a valid DecodedFrame result.
    std::mt19937 rng(0xC0FFEEu);  // deterministic seed - reproducible
    std::uniform_int_distribution<int> sizeDist(0, 256);
    std::uniform_int_distribution<int> byteDist(0, 255);

    for (int round = 0; round < 200; ++round)
    {
        const int n = sizeDist(rng);
        QByteArray input(n, '\0');
        for (int i = 0; i < n; ++i)
        {
            input[i] = static_cast<char>(byteDist(rng));
        }
        // Just decoding without crashing is the assertion; result contents
        // don't matter for random noise.
        (void)tiktok::decodeWebcastPushFrame(input);
    }
}

TEST(TikTokFrameDecoder, survivesRandomBytesAfterValidPrefix)
{
    // A real frame envelope followed by garbage - simulates an in-flight
    // corruption. Decoder should yield what it could up to the corruption.
    const auto userBytes =
        buildUser(0, QByteArrayLiteral("pre"), QByteArrayLiteral("pre"));
    const auto chatBody = PbWriter()
                              .bytesField(2, userBytes)
                              .stringField(3, QByteArrayLiteral("hi"))
                              .take();
    auto wire = wrapFrame(QByteArrayLiteral("WebcastChatMessage"), chatBody);

    std::mt19937 rng(0xBADu);
    std::uniform_int_distribution<int> byteDist(0, 255);
    for (int i = 0; i < 64; ++i)
    {
        wire.append(static_cast<char>(byteDist(rng)));
    }
    (void)tiktok::decodeWebcastPushFrame(wire);
}
