// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "providers/tiktok/TikTokFrameDecoder.hpp"

#include "providers/tiktok/ProtobufReader.hpp"

#include <span>
#include <string_view>

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
        // Schema (best-effort, from public RE): field 3 total (historical
        // total viewers), field 4/6 online (current viewers). Field names
        // have drifted over TikTok versions. Scan every int64-typed field
        // in the 3-10 range and take the largest - that's almost always
        // the current live count.
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
                if (signedV > best)
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
    // Other methods (control, room notify, ...) are left for follow-up
    // work and ignored here.
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
        if (field == 1 && wire == WireType::LengthDelimited)
        {
            std::span<const std::uint8_t> sub;
            if (!r.readLengthDelimited(sub))
            {
                return false;
            }
            decodeBaseMessage(sub, out);
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
            case 7: {  // payload_type (string)
                std::string_view s;
                if (!r.readString(s))
                {
                    return out;
                }
                out.payloadType = toQString(s);
                break;
            }
            case 8: {  // payload (bytes)
                if (!r.readLengthDelimited(innerPayload))
                {
                    return out;
                }
                break;
            }
            default:
                if (!r.skip(wire))
                {
                    return out;
                }
                break;
        }
    }

    if (out.payloadType == QStringLiteral("msg") && !innerPayload.empty())
    {
        decodeFetchResult(innerPayload, out);
    }
    return out;
}

}  // namespace chatterino::tiktok
