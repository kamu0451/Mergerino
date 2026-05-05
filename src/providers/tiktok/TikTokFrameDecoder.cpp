// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "providers/tiktok/TikTokFrameDecoder.hpp"

#include "providers/tiktok/ProtobufReader.hpp"

#include <zlib.h>

#include <span>
#include <string_view>
#include <vector>

namespace chatterino::tiktok {

using proto::Reader;
using proto::WireType;

namespace {

QString toQString(std::string_view sv)
{
    return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size()));
}

bool decodeUser(std::span<const std::uint8_t> bytes, DecodedUser &out)
{
    Reader r = Reader::fromSpan(bytes);
    std::uint32_t field{};
    WireType wire{};
    while (r.hasMore())
    {
        if (!r.readTag(field, wire))
        {
            return false;
        }
        switch (field)
        {
            case 1: {  // id (int64)
                std::uint64_t v{};
                if (!r.readVarint(v))
                {
                    return false;
                }
                out.userId = static_cast<qint64>(v);
                break;
            }
            case 3: {  // nick_name (string)
                std::string_view s;
                if (!r.readString(s))
                {
                    return false;
                }
                out.nickname = toQString(s);
                break;
            }
            case 38: {  // username (string)
                std::string_view s;
                if (!r.readString(s))
                {
                    return false;
                }
                out.uniqueId = toQString(s);
                break;
            }
            case 46: {  // sec_uid (string)
                std::string_view s;
                if (!r.readString(s))
                {
                    return false;
                }
                out.secUid = toQString(s);
                break;
            }
            default:
                if (!r.skip(wire))
                {
                    return false;
                }
                break;
        }
    }
    return true;
}

bool decodeLikeMessage(std::span<const std::uint8_t> bytes,
                       DecodedLikeEvent &out)
{
    Reader r = Reader::fromSpan(bytes);
    std::uint32_t field{};
    WireType wire{};
    while (r.hasMore())
    {
        if (!r.readTag(field, wire))
        {
            return false;
        }
        switch (field)
        {
            case 2: {  // count (int32)
                std::uint64_t v{};
                if (!r.readVarint(v))
                {
                    return false;
                }
                out.count = static_cast<qint32>(v);
                break;
            }
            case 3: {  // total (int64)
                std::uint64_t v{};
                if (!r.readVarint(v))
                {
                    return false;
                }
                out.total = static_cast<qint64>(v);
                break;
            }
            case 5: {  // user (User)
                std::span<const std::uint8_t> sub;
                if (!r.readLengthDelimited(sub))
                {
                    return false;
                }
                if (!decodeUser(sub, out.user))
                {
                    return false;
                }
                break;
            }
            default:
                if (!r.skip(wire))
                {
                    return false;
                }
                break;
        }
    }
    return true;
}

bool decodeMemberMessage(std::span<const std::uint8_t> bytes,
                         DecodedMemberEvent &out)
{
    Reader r = Reader::fromSpan(bytes);
    std::uint32_t field{};
    WireType wire{};
    while (r.hasMore())
    {
        if (!r.readTag(field, wire))
        {
            return false;
        }
        switch (field)
        {
            case 2: {  // user (User)
                std::span<const std::uint8_t> sub;
                if (!r.readLengthDelimited(sub))
                {
                    return false;
                }
                if (!decodeUser(sub, out.user))
                {
                    return false;
                }
                break;
            }
            case 9: {  // enter_type (int32)
                std::uint64_t v{};
                if (!r.readVarint(v))
                {
                    return false;
                }
                out.enterType = static_cast<qint32>(v);
                break;
            }
            default:
                if (!r.skip(wire))
                {
                    return false;
                }
                break;
        }
    }
    return true;
}

bool decodeSocialMessage(std::span<const std::uint8_t> bytes,
                         DecodedSocialEvent &out)
{
    Reader r = Reader::fromSpan(bytes);
    std::uint32_t field{};
    WireType wire{};
    while (r.hasMore())
    {
        if (!r.readTag(field, wire))
        {
            return false;
        }
        switch (field)
        {
            case 2: {  // user (User)
                std::span<const std::uint8_t> sub;
                if (!r.readLengthDelimited(sub))
                {
                    return false;
                }
                if (!decodeUser(sub, out.user))
                {
                    return false;
                }
                break;
            }
            case 4: {  // action (int64)
                std::uint64_t v{};
                if (!r.readVarint(v))
                {
                    return false;
                }
                out.action = static_cast<qint64>(v);
                break;
            }
            case 6: {  // follow_count (int64)
                std::uint64_t v{};
                if (!r.readVarint(v))
                {
                    return false;
                }
                out.followCount = static_cast<qint64>(v);
                break;
            }
            default:
                if (!r.skip(wire))
                {
                    return false;
                }
                break;
        }
    }
    return true;
}

// Pulls diamond_count from a nested WebcastGiftMessage.GiftStruct payload.
// In observed TikTok protos the diamond value lives at field 4 of GiftStruct,
// but the layout has shifted across versions, so we tolerate the field being
// absent and leave the count at zero rather than failing the whole frame.
bool decodeGiftStruct(std::span<const std::uint8_t> bytes, qint32 &diamondCount)
{
    Reader r = Reader::fromSpan(bytes);
    std::uint32_t field{};
    WireType wire{};
    while (r.hasMore())
    {
        if (!r.readTag(field, wire))
        {
            return false;
        }
        if (field == 4 && wire == WireType::Varint)
        {
            std::uint64_t v{};
            if (!r.readVarint(v))
            {
                return false;
            }
            diamondCount = static_cast<qint32>(v);
        }
        else if (!r.skip(wire))
        {
            return false;
        }
    }
    return true;
}

bool decodeGiftMessage(std::span<const std::uint8_t> bytes,
                       DecodedGiftEvent &out)
{
    Reader r = Reader::fromSpan(bytes);
    std::uint32_t field{};
    WireType wire{};
    while (r.hasMore())
    {
        if (!r.readTag(field, wire))
        {
            return false;
        }
        switch (field)
        {
            case 5: {  // repeat_count (int32)
                std::uint64_t v{};
                if (!r.readVarint(v))
                {
                    return false;
                }
                out.repeatCount = static_cast<qint32>(v);
                break;
            }
            case 7: {  // from_user (User)
                std::span<const std::uint8_t> sub;
                if (!r.readLengthDelimited(sub))
                {
                    return false;
                }
                if (!decodeUser(sub, out.fromUser))
                {
                    return false;
                }
                break;
            }
            case 9: {  // repeat_end (int32)
                std::uint64_t v{};
                if (!r.readVarint(v))
                {
                    return false;
                }
                out.repeatEnd = static_cast<qint32>(v);
                break;
            }
            case 15: {  // gift (GiftStruct)
                std::span<const std::uint8_t> sub;
                if (!r.readLengthDelimited(sub))
                {
                    return false;
                }
                qint32 diamond = 0;
                if (decodeGiftStruct(sub, diamond))
                {
                    out.diamondCount = diamond;
                }
                break;
            }
            default:
                if (!r.skip(wire))
                {
                    return false;
                }
                break;
        }
    }
    return true;
}

bool decodeChatMessage(std::span<const std::uint8_t> bytes,
                       DecodedChatMessage &out)
{
    Reader r = Reader::fromSpan(bytes);
    std::uint32_t field{};
    WireType wire{};
    while (r.hasMore())
    {
        if (!r.readTag(field, wire))
        {
            return false;
        }
        switch (field)
        {
            case 2: {  // user_info (User)
                std::span<const std::uint8_t> sub;
                if (!r.readLengthDelimited(sub))
                {
                    return false;
                }
                if (!decodeUser(sub, out.user))
                {
                    return false;
                }
                break;
            }
            case 3: {  // content (string)
                std::string_view s;
                if (!r.readString(s))
                {
                    return false;
                }
                out.content = toQString(s);
                break;
            }
            default:
                if (!r.skip(wire))
                {
                    return false;
                }
                break;
        }
    }
    return true;
}

bool decodeBaseMessage(std::span<const std::uint8_t> bytes, DecodedFrame &out)
{
    Reader r = Reader::fromSpan(bytes);
    QString method;
    std::span<const std::uint8_t> payload;
    qint64 msgId{0};
    std::uint32_t field{};
    WireType wire{};
    while (r.hasMore())
    {
        if (!r.readTag(field, wire))
        {
            return false;
        }
        switch (field)
        {
            case 1: {  // method (string)
                std::string_view s;
                if (!r.readString(s))
                {
                    return false;
                }
                method = toQString(s);
                break;
            }
            case 2: {  // payload (bytes)
                if (!r.readLengthDelimited(payload))
                {
                    return false;
                }
                break;
            }
            case 3: {  // msg_id (int64)
                std::uint64_t v{};
                if (!r.readVarint(v))
                {
                    return false;
                }
                msgId = static_cast<qint64>(v);
                break;
            }
            default:
                if (!r.skip(wire))
                {
                    return false;
                }
                break;
        }
    }
    // Push even when method is empty so diagnostics can tell us if the
    // BaseMessage schema changed (different field number for method).
    out.allMethods.push_back(method.isEmpty() ? QStringLiteral("<empty>")
                                              : method);

    if (payload.empty())
    {
        return true;
    }

    if (method == QStringLiteral("WebcastChatMessage"))
    {
        DecodedChatMessage chat;
        chat.msgId = msgId;
        if (decodeChatMessage(payload, chat) && !chat.content.isEmpty())
        {
            out.chatMessages.push_back(std::move(chat));
        }
    }
    else if (method == QStringLiteral("WebcastLikeMessage"))
    {
        DecodedLikeEvent ev;
        if (decodeLikeMessage(payload, ev))
        {
            out.likeEvents.push_back(std::move(ev));
        }
    }
    else if (method == QStringLiteral("WebcastMemberMessage"))
    {
        DecodedMemberEvent ev;
        if (decodeMemberMessage(payload, ev))
        {
            out.memberEvents.push_back(std::move(ev));
        }
    }
    else if (method == QStringLiteral("WebcastSocialMessage"))
    {
        DecodedSocialEvent ev;
        if (decodeSocialMessage(payload, ev))
        {
            out.socialEvents.push_back(std::move(ev));
        }
    }
    else if (method == QStringLiteral("WebcastGiftMessage"))
    {
        DecodedGiftEvent ev;
        if (decodeGiftMessage(payload, ev))
        {
            out.giftEvents.push_back(std::move(ev));
        }
    }
    else if (method == QStringLiteral("WebcastRoomUserSeqMessage"))
    {
        // Schema (best-effort, from public RE): fields 3, 5, 7 commonly
        // carry total/online viewer counts across TikTok versions. But
        // room_id / anchor_id are ALSO int64 in the same range and hold
        // huge values (10^18), so taking the raw max picks the id. Cap
        // at 10 million - more than any live concurrent count - to
        // filter IDs out. Take the max of remaining values.
        constexpr qint64 VIEWER_COUNT_MAX_PLAUSIBLE = 10'000'000;
        qint64 best = 0;
        Reader r = Reader::fromSpan(payload);
        std::uint32_t fld{};
        WireType wt{};
        while (r.hasMore())
        {
            if (!r.readTag(fld, wt))
            {
                break;
            }
            if (wt == WireType::Varint && fld >= 3 && fld <= 10)
            {
                std::uint64_t v{};
                if (!r.readVarint(v))
                {
                    break;
                }
                const auto signedV = static_cast<qint64>(v);
                if (signedV > best && signedV <= VIEWER_COUNT_MAX_PLAUSIBLE)
                {
                    best = signedV;
                }
            }
            else if (!r.skip(wt))
            {
                break;
            }
        }
        if (best > out.roomViewerCount)
        {
            out.roomViewerCount = best;
        }
    }
    else
    {
        // Capture so diagnostics can tell us what we're missing.
        out.unhandledMethods.push_back(method);
    }
    return true;
}

bool decodeFetchResult(std::span<const std::uint8_t> bytes, DecodedFrame &out)
{
    Reader r = Reader::fromSpan(bytes);
    std::uint32_t field{};
    WireType wire{};
    while (r.hasMore())
    {
        if (!r.readTag(field, wire))
        {
            return false;
        }
        if (wire == WireType::LengthDelimited)
        {
            std::span<const std::uint8_t> sub;
            if (!r.readLengthDelimited(sub))
            {
                return false;
            }
            // TikTok's webcast envelope historically put the repeated
            // BaseProtoMessage messages at field 1. As of mid-2026 the
            // wire format moved them to field 2 (with field 1 now carrying
            // a small response-metadata sub-message: response_code, cursor,
            // server time). Try both: decodeBaseMessage returns harmlessly
            // when the sub-bytes don't match the BaseMessage shape, so
            // probing both costs nothing.
            if (field == 1 || field == 2)
            {
                decodeBaseMessage(sub, out);
            }
        }
        else if (!r.skip(wire))
        {
            return false;
        }
    }
    return true;
}

}  // namespace

DecodedFrame decodeWebcastPushFrame(QByteArrayView bytes)
{
    DecodedFrame out;
    std::span<const std::uint8_t> raw(
        reinterpret_cast<const std::uint8_t *>(bytes.data()),
        static_cast<std::size_t>(bytes.size()));
    Reader r = Reader::fromSpan(raw);

    std::uint32_t field{};
    WireType wire{};
    std::span<const std::uint8_t> innerPayload;
    while (r.hasMore())
    {
        if (!r.readTag(field, wire))
        {
            return out;
        }
        switch (field)
        {
            case 6: {  // payload_encoding (string)
                std::string_view s;
                if (!r.readString(s))
                {
                    return out;
                }
                out.payloadEncoding = toQString(s);
                out.envelopeFields.push_back(
                    QStringLiteral("f6/str/%1").arg(s.size()));
                break;
            }
            case 7: {  // payload_type (string)
                std::string_view s;
                if (!r.readString(s))
                {
                    return out;
                }
                out.payloadType = toQString(s);
                out.envelopeFields.push_back(
                    QStringLiteral("f7/str/%1").arg(s.size()));
                break;
            }
            case 8: {  // payload (bytes)
                if (!r.readLengthDelimited(innerPayload))
                {
                    return out;
                }
                out.envelopeFields.push_back(
                    QStringLiteral("f8/bytes/%1").arg(innerPayload.size()));
                break;
            }
            default: {
                const auto wireTag = static_cast<int>(wire);
                if (wire == WireType::LengthDelimited)
                {
                    std::span<const std::uint8_t> sub;
                    if (!r.readLengthDelimited(sub))
                    {
                        return out;
                    }
                    out.envelopeFields.push_back(
                        QStringLiteral("f%1/w%2/l%3")
                            .arg(field)
                            .arg(wireTag)
                            .arg(sub.size()));
                }
                else if (!r.skip(wire))
                {
                    return out;
                }
                else
                {
                    out.envelopeFields.push_back(
                        QStringLiteral("f%1/w%2").arg(field).arg(wireTag));
                }
                break;
            }
        }
    }

    // Newer TikTok schemas sometimes leave payload_type empty (routing
    // metadata moved into the headers_list at field 5). Treat an empty
    // payloadType as permissive: try to decode if innerPayload looks
    // parseable. The inner FetchResult decoder is self-consistent - if
    // the bytes aren't a valid FetchResult it just returns no events.
    const bool payloadTypeKnown = out.payloadType == QStringLiteral("msg");
    const bool tryDecode = payloadTypeKnown || out.payloadType.isEmpty();
    if (tryDecode && !innerPayload.empty())
    {
        // TikTok wraps most chat-bearing payloads in gzip (envelope field
        // 6 == "gzip"). Without this decompression the FetchResult decoder
        // sees raw deflate bytes and silently returns no events, which is
        // why "TikTok says they're live but no chat shows" was the typical
        // symptom. Run zlib's gzip-aware inflate and decode the result.
        if (out.payloadEncoding == QStringLiteral("gzip"))
        {
            std::vector<std::uint8_t> inflated;
            // 32 KiB initial; grow up to 4 MiB. Real frames top out at
            // ~80 KiB but caps protect against runaway inputs.
            inflated.resize(32 * 1024);
            constexpr std::size_t kMaxInflated = 4 * 1024 * 1024;
            z_stream strm{};
            strm.next_in =
                const_cast<Bytef *>(innerPayload.data());
            strm.avail_in = static_cast<uInt>(innerPayload.size());
            // 15 + 32 lets zlib auto-detect gzip vs zlib headers.
            if (inflateInit2(&strm, 15 + 32) == Z_OK)
            {
                std::size_t produced = 0;
                int rc = Z_OK;
                while (true)
                {
                    strm.next_out = inflated.data() + produced;
                    strm.avail_out =
                        static_cast<uInt>(inflated.size() - produced);
                    rc = inflate(&strm, Z_NO_FLUSH);
                    produced = inflated.size() - strm.avail_out;
                    if (rc == Z_STREAM_END || rc < 0)
                    {
                        break;
                    }
                    // Z_OK / Z_BUF_ERROR with output buffer full: grow.
                    if (strm.avail_out == 0)
                    {
                        if (inflated.size() >= kMaxInflated)
                        {
                            break;
                        }
                        inflated.resize(
                            std::min(kMaxInflated, inflated.size() * 2));
                        continue;
                    }
                    // avail_out > 0 and not Z_STREAM_END: inflate stalled
                    // because input was consumed but more is expected.
                    // Truncated frame; bail.
                    break;
                }
                inflateEnd(&strm);
                if (rc == Z_STREAM_END && produced > 0)
                {
                    inflated.resize(produced);
                    decodeFetchResult(
                        std::span<const std::uint8_t>(inflated.data(),
                                                      inflated.size()),
                        out);
                }
            }
        }
        else
        {
            decodeFetchResult(innerPayload, out);
        }
    }
    return out;
}

}  // namespace chatterino::tiktok
