// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT
//
// Direct unit tests for the TikTok protobuf wire-format reader
// (src/providers/tiktok/ProtobufReader.hpp). Previously this reader was
// only exercised indirectly through full-frame decode tests in
// TikTokProvider.cpp; these tests hand-build byte arrays to cover the
// varint/tag/length-delimited/fixed decode paths and the bounds-safety
// behavior on truncated or malformed input.

#include "providers/tiktok/ProtobufReader.hpp"
#include "Test.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

using namespace chatterino::tiktok::proto;

namespace {

Reader makeReader(const std::vector<std::uint8_t> &bytes)
{
    return Reader(bytes.data(), bytes.size());
}

}  // namespace

// --- construction / empty buffer -------------------------------------------

TEST(ProtobufReader, EmptyBufferHasNoMore)
{
    Reader reader;
    EXPECT_FALSE(reader.hasMore());
}

TEST(ProtobufReader, EmptyBufferReadVarintFails)
{
    std::vector<std::uint8_t> bytes;
    Reader reader = makeReader(bytes);

    std::uint64_t value{};
    EXPECT_FALSE(reader.readVarint(value));
}

TEST(ProtobufReader, EmptyBufferReadTagFails)
{
    std::vector<std::uint8_t> bytes;
    Reader reader = makeReader(bytes);

    std::uint32_t fieldNumber{};
    WireType wireType{};
    EXPECT_FALSE(reader.readTag(fieldNumber, wireType));
}

TEST(ProtobufReader, FromSpanConstructsEquivalentReader)
{
    std::vector<std::uint8_t> bytes{0x01};
    Reader reader = Reader::fromSpan(bytes);

    EXPECT_TRUE(reader.hasMore());
    std::uint64_t value{};
    ASSERT_TRUE(reader.readVarint(value));
    EXPECT_EQ(value, 1U);
    EXPECT_FALSE(reader.hasMore());
}

// --- varint decode -----------------------------------------------------

TEST(ProtobufReader, ReadVarintSingleByteZero)
{
    std::vector<std::uint8_t> bytes{0x00};
    Reader reader = makeReader(bytes);

    std::uint64_t value{};
    ASSERT_TRUE(reader.readVarint(value));
    EXPECT_EQ(value, 0U);
}

TEST(ProtobufReader, ReadVarintSingleByteMax)
{
    // 0x7F is the largest value that fits in a single varint byte (the
    // continuation bit, 0x80, is clear).
    std::vector<std::uint8_t> bytes{0x7F};
    Reader reader = makeReader(bytes);

    std::uint64_t value{};
    ASSERT_TRUE(reader.readVarint(value));
    EXPECT_EQ(value, 127U);
    EXPECT_FALSE(reader.hasMore());
}

TEST(ProtobufReader, ReadVarintCrossesSingleByteBoundary)
{
    // 128 is the smallest value that needs a second byte: low 7 bits are
    // 0 (continuation set -> 0x80), remaining bit is 1 (-> 0x01).
    std::vector<std::uint8_t> bytes{0x80, 0x01};
    Reader reader = makeReader(bytes);

    std::uint64_t value{};
    ASSERT_TRUE(reader.readVarint(value));
    EXPECT_EQ(value, 128U);
}

TEST(ProtobufReader, ReadVarintMultiByte300)
{
    // 300 = 0b1_0010_1100 -> low 7 bits 0101100 (0x2C, continuation set,
    // 0xAC), remaining bits 10 (0x02).
    std::vector<std::uint8_t> bytes{0xAC, 0x02};
    Reader reader = makeReader(bytes);

    std::uint64_t value{};
    ASSERT_TRUE(reader.readVarint(value));
    EXPECT_EQ(value, 300U);
    EXPECT_FALSE(reader.hasMore());
}

TEST(ProtobufReader, ReadVarintNearUint64Max)
{
    // Standard 10-byte varint encoding of UINT64_MAX: nine 0xFF continuation
    // bytes (7 payload bits each = 63 bits, all set) followed by a
    // terminating 0x01 (bit 63). Exercises the reader right up to its
    // 64-bit shift limit without tripping the overflow guard.
    std::vector<std::uint8_t> bytes{0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF, 0xFF, 0x01};
    Reader reader = makeReader(bytes);

    std::uint64_t value{};
    ASSERT_TRUE(reader.readVarint(value));
    EXPECT_EQ(value, UINT64_MAX);
    EXPECT_FALSE(reader.hasMore());
}

TEST(ProtobufReader, ReadVarintTruncatedContinuationByteFailsSafely)
{
    // Continuation bit set on the final byte of the buffer: the encoder
    // promised more bytes that never arrive. Must fail, not read past end_.
    std::vector<std::uint8_t> bytes{0x80};
    Reader reader = makeReader(bytes);

    std::uint64_t value{};
    EXPECT_FALSE(reader.readVarint(value));
}

TEST(ProtobufReader, ReadVarintOverlongEncodingFailsSafely)
{
    // Ten straight 0xFF bytes all carry the continuation bit, so the shift
    // counter reaches 70 (>= 64) before a terminator byte is ever seen.
    // The reader must reject this instead of shifting UB into a uint64_t.
    std::vector<std::uint8_t> bytes(10, 0xFF);
    Reader reader = makeReader(bytes);

    std::uint64_t value{};
    EXPECT_FALSE(reader.readVarint(value));
}

// --- tag decode ----------------------------------------------------------

TEST(ProtobufReader, ReadTagDecodesFieldNumberAndWireType)
{
    // field 1, wire type LengthDelimited(2): tag = (1 << 3) | 2 = 0x0A.
    std::vector<std::uint8_t> bytes{0x0A};
    Reader reader = makeReader(bytes);

    std::uint32_t fieldNumber{};
    WireType wireType{};
    ASSERT_TRUE(reader.readTag(fieldNumber, wireType));
    EXPECT_EQ(fieldNumber, 1U);
    EXPECT_EQ(wireType, WireType::LengthDelimited);
}

TEST(ProtobufReader, ReadTagDecodesMultiByteFieldNumber)
{
    // field 100, wire type Varint(0): tag = (100 << 3) | 0 = 800, which
    // needs a two-byte varint (0xA0, 0x06).
    std::vector<std::uint8_t> bytes{0xA0, 0x06};
    Reader reader = makeReader(bytes);

    std::uint32_t fieldNumber{};
    WireType wireType{};
    ASSERT_TRUE(reader.readTag(fieldNumber, wireType));
    EXPECT_EQ(fieldNumber, 100U);
    EXPECT_EQ(wireType, WireType::Varint);
}

// --- length-delimited / string decode -------------------------------------

TEST(ProtobufReader, ReadLengthDelimitedReturnsExactSlice)
{
    // length = 5, payload "hello".
    std::vector<std::uint8_t> bytes{0x05, 'h', 'e', 'l', 'l', 'o'};
    Reader reader = makeReader(bytes);

    std::span<const std::uint8_t> out;
    ASSERT_TRUE(reader.readLengthDelimited(out));
    ASSERT_EQ(out.size(), 5U);
    EXPECT_EQ(out[0], 'h');
    EXPECT_EQ(out[4], 'o');
    EXPECT_FALSE(reader.hasMore());
}

TEST(ProtobufReader, ReadStringDecodesUtf8Payload)
{
    std::vector<std::uint8_t> bytes{0x03, 'f', 'o', 'o'};
    Reader reader = makeReader(bytes);

    std::string_view out;
    ASSERT_TRUE(reader.readString(out));
    EXPECT_EQ(out, "foo");
}

TEST(ProtobufReader, ReadLengthDelimitedEmptyPayload)
{
    std::vector<std::uint8_t> bytes{0x00};
    Reader reader = makeReader(bytes);

    std::span<const std::uint8_t> out;
    ASSERT_TRUE(reader.readLengthDelimited(out));
    EXPECT_EQ(out.size(), 0U);
}

TEST(ProtobufReader, ReadLengthDelimitedClaimingMoreThanRemainingFailsSafely)
{
    // length says 10 bytes follow, but only 3 are actually present. The
    // reader must refuse rather than returning a span that runs past end_.
    std::vector<std::uint8_t> bytes{0x0A, 'a', 'b', 'c'};
    Reader reader = makeReader(bytes);

    std::span<const std::uint8_t> out;
    EXPECT_FALSE(reader.readLengthDelimited(out));

    // The cursor must not have been advanced into the (non-existent)
    // payload: the 3 bytes after the length prefix are still readable as
    // their own values, proving no over-read / corruption occurred.
    EXPECT_TRUE(reader.hasMore());
    std::uint64_t value{};
    ASSERT_TRUE(reader.readVarint(value));
    EXPECT_EQ(value, static_cast<std::uint64_t>('a'));
}

TEST(ProtobufReader, ReadLengthDelimitedOnEmptyBufferFailsSafely)
{
    std::vector<std::uint8_t> bytes;
    Reader reader = makeReader(bytes);

    std::span<const std::uint8_t> out;
    EXPECT_FALSE(reader.readLengthDelimited(out));
}

// --- fixed32 / fixed64 decode ----------------------------------------------

TEST(ProtobufReader, ReadFixed32DecodesLittleEndian)
{
    std::vector<std::uint8_t> bytes{0x01, 0x00, 0x00, 0x00};
    Reader reader = makeReader(bytes);

    std::uint32_t value{};
    ASSERT_TRUE(reader.readFixed32(value));
    EXPECT_EQ(value, 1U);
    EXPECT_FALSE(reader.hasMore());
}

TEST(ProtobufReader, ReadFixed64DecodesLittleEndian)
{
    std::vector<std::uint8_t> bytes{0x02, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00};
    Reader reader = makeReader(bytes);

    std::uint64_t value{};
    ASSERT_TRUE(reader.readFixed64(value));
    EXPECT_EQ(value, 2U);
    EXPECT_FALSE(reader.hasMore());
}

TEST(ProtobufReader, ReadFixed32TruncatedFailsSafely)
{
    // Only 2 of the required 4 bytes are present.
    std::vector<std::uint8_t> bytes{0x01, 0x02};
    Reader reader = makeReader(bytes);

    std::uint32_t value{};
    EXPECT_FALSE(reader.readFixed32(value));

    // Cursor must not have moved: the same 2 bytes are still there,
    // readable as a single-byte varint.
    std::uint64_t varintValue{};
    ASSERT_TRUE(reader.readVarint(varintValue));
    EXPECT_EQ(varintValue, 1U);
}

TEST(ProtobufReader, ReadFixed64TruncatedFailsSafely)
{
    std::vector<std::uint8_t> bytes{0x01, 0x02, 0x03};
    Reader reader = makeReader(bytes);

    std::uint64_t value{};
    EXPECT_FALSE(reader.readFixed64(value));
}

// --- skip ------------------------------------------------------------------

TEST(ProtobufReader, SkipVarintConsumesValue)
{
    std::vector<std::uint8_t> bytes{0xAC, 0x02};
    Reader reader = makeReader(bytes);

    EXPECT_TRUE(reader.skip(WireType::Varint));
    EXPECT_FALSE(reader.hasMore());
}

TEST(ProtobufReader, SkipFixed32ConsumesFourBytes)
{
    std::vector<std::uint8_t> bytes{0x01, 0x02, 0x03, 0x04};
    Reader reader = makeReader(bytes);

    EXPECT_TRUE(reader.skip(WireType::Fixed32));
    EXPECT_FALSE(reader.hasMore());
}

TEST(ProtobufReader, SkipFixed64ConsumesEightBytes)
{
    std::vector<std::uint8_t> bytes{0x01, 0x02, 0x03, 0x04,
                                    0x05, 0x06, 0x07, 0x08};
    Reader reader = makeReader(bytes);

    EXPECT_TRUE(reader.skip(WireType::Fixed64));
    EXPECT_FALSE(reader.hasMore());
}

TEST(ProtobufReader, SkipLengthDelimitedConsumesPrefixAndPayload)
{
    std::vector<std::uint8_t> bytes{0x03, 'a', 'b', 'c'};
    Reader reader = makeReader(bytes);

    EXPECT_TRUE(reader.skip(WireType::LengthDelimited));
    EXPECT_FALSE(reader.hasMore());
}

TEST(ProtobufReader, SkipGroupWireTypesAreUnsupportedAndFailSafely)
{
    std::vector<std::uint8_t> bytes{0x01, 0x02, 0x03};
    Reader startGroupReader = makeReader(bytes);
    Reader endGroupReader = makeReader(bytes);

    EXPECT_FALSE(startGroupReader.skip(WireType::StartGroup));
    EXPECT_FALSE(endGroupReader.skip(WireType::EndGroup));
}

TEST(ProtobufReader, SkipTruncatedFieldFailsSafely)
{
    // Claims Fixed64 but only 3 bytes are present.
    std::vector<std::uint8_t> bytes{0x01, 0x02, 0x03};
    Reader reader = makeReader(bytes);

    EXPECT_FALSE(reader.skip(WireType::Fixed64));
}

// --- sequencing --------------------------------------------------------

TEST(ProtobufReader, ReadTagThenLengthDelimitedFieldRoundTrips)
{
    // field 1, wire type LengthDelimited, payload "hi".
    std::vector<std::uint8_t> bytes{0x0A, 0x02, 'h', 'i'};
    Reader reader = makeReader(bytes);

    std::uint32_t fieldNumber{};
    WireType wireType{};
    ASSERT_TRUE(reader.readTag(fieldNumber, wireType));
    EXPECT_EQ(fieldNumber, 1U);
    EXPECT_EQ(wireType, WireType::LengthDelimited);

    std::string_view value;
    ASSERT_TRUE(reader.readString(value));
    EXPECT_EQ(value, "hi");
    EXPECT_FALSE(reader.hasMore());
}
