// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "providers/tiktok/TikTokFrameDecoder.hpp"

#include "providers/tiktok/ProtobufReader.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QString>
#include <zlib.h>

#include <atomic>
#include <cstdlib>
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

// Pulls the per-unit coin value from a WebcastGiftMessage.GiftStruct.
// Field 4 of GiftStruct carries coins encoded as coins * 1000 (Rose = 1000,
// Heart Me = 3000, etc.); divide to get the visible coin price the gifter
// actually paid. Field 5 is gift_id, not the value.
bool decodeGiftStruct(std::span<const std::uint8_t> bytes, qint32 &coinValue,
                      QString &giftName)
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
            coinValue = static_cast<qint32>(v / 1000);
        }
        else if (field == 16 && wire == WireType::LengthDelimited)
        {
            std::string_view s;
            if (!r.readString(s))
            {
                return false;
            }
            giftName = toQString(s);
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
                qint32 coins = 0;
                QString giftName;
                if (decodeGiftStruct(sub, coins, giftName))
                {
                    out.coinValue = coins;
                    out.giftName = std::move(giftName);
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
        // Field 3 is the concurrent online viewer count (matches what
        // TikTok shows on the live page). Field 7 is the cumulative
        // ever-online count and is much larger - using the wrong field
        // shows wildly inflated numbers (~20x). Read field 3 specifically.
        Reader r = Reader::fromSpan(payload);
        std::uint32_t fld{};
        WireType wt{};
        while (r.hasMore())
        {
            if (!r.readTag(fld, wt))
            {
                break;
            }
            if (fld == 3 && wt == WireType::Varint)
            {
                std::uint64_t v{};
                if (!r.readVarint(v))
                {
                    break;
                }
                out.roomViewerCount = static_cast<qint64>(v);
                break;
            }
            if (!r.skip(wt))
            {
                break;
            }
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
    // ProtoMessageFetchResult v3 canonical layout (from
    // isaackogan/TikTok-Webcast-Protobuf src/slim/v3/webcast/shared/message.proto):
    //   1: repeated BaseProtoMessage messages
    //   2: string cursor
    //   3: int64 fetch_interval
    //   4: int64 now
    //   5: string internal_ext
    //   6: int32 fetch_type
    //   7: map<string,string> route_params
    //   8: int64 heartbeat_duration
    //   9: bool need_ack
    //  10: string push_server
    //  11: bool is_first
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
            out.fetchResultFields.push_back(QStringLiteral("f%1/w2/l%2")
                                                .arg(field)
                                                .arg(sub.size()));
            if (field == 1)
            {
                decodeBaseMessage(sub, out);
            }
        }
        else
        {
            const auto wireTag = static_cast<int>(wire);
            if (wire == WireType::Varint)
            {
                std::uint64_t v{};
                if (!r.readVarint(v))
                {
                    return false;
                }
                out.fetchResultFields.push_back(
                    QStringLiteral("f%1/w%2/v%3")
                        .arg(field)
                        .arg(wireTag)
                        .arg(static_cast<qulonglong>(v)));
            }
            else if (!r.skip(wire))
            {
                return false;
            }
            else
            {
                out.fetchResultFields.push_back(
                    QStringLiteral("f%1/w%2").arg(field).arg(wireTag));
            }
        }
    }
    return true;
}

// Optional one-shot frame dump for diagnostics. Set TIKTOK_DUMP_DIR to a
// writable directory (e.g. C:\Users\Repe\tiktok-frames). The first
// kMaxDumpedFrames push frames received process-wide get written as paired
// raw_<N>.bin (pre-inflate WS payload) and inflated_<N>.bin (gunzipped
// FetchResult bytes) for hand-decoding against the v3 schema.
constexpr int kMaxDumpedFrames = 600;

void maybeDumpFrame(std::span<const std::uint8_t> raw,
                    std::span<const std::uint8_t> inflated)
{
    static std::atomic<int> counter{0};
    const char *dir = std::getenv("TIKTOK_DUMP_DIR");
    if (dir == nullptr || *dir == '\0')
    {
        return;
    }
    const int n = counter.fetch_add(1);
    if (n >= kMaxDumpedFrames)
    {
        return;
    }
    QDir dumpDir(QString::fromLocal8Bit(dir));
    if (!dumpDir.exists())
    {
        dumpDir.mkpath(QStringLiteral("."));
    }
    const auto stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"));
    {
        QFile f(dumpDir.filePath(QStringLiteral("raw_%1_%2.bin")
                                     .arg(stamp)
                                     .arg(n, 3, 10, QChar('0'))));
        if (f.open(QIODevice::WriteOnly))
        {
            f.write(reinterpret_cast<const char *>(raw.data()),
                    static_cast<qint64>(raw.size()));
        }
    }
    if (!inflated.empty())
    {
        QFile f(dumpDir.filePath(QStringLiteral("inflated_%1_%2.bin")
                                     .arg(stamp)
                                     .arg(n, 3, 10, QChar('0'))));
        if (f.open(QIODevice::WriteOnly))
        {
            f.write(reinterpret_cast<const char *>(inflated.data()),
                    static_cast<qint64>(inflated.size()));
        }
    }
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
        // TikTok wraps most chat-bearing payloads in gzip. Some envelopes
        // declare it via field 6 == "gzip", but many large frames omit
        // payloadEncoding entirely while still sending gzip-magic bytes.
        // Detect by magic ({0x1f, 0x8b}) so those frames are inflated too.
        const bool gzipMagic = innerPayload.size() >= 2 &&
                               innerPayload[0] == 0x1f &&
                               innerPayload[1] == 0x8b;
        if (out.payloadEncoding == QStringLiteral("gzip") || gzipMagic)
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
                    out.inflatedSize = static_cast<qint64>(inflated.size());
                    std::span<const std::uint8_t> inflatedSpan(
                        inflated.data(), inflated.size());
                    maybeDumpFrame(innerPayload, inflatedSpan);
                    decodeFetchResult(inflatedSpan, out);
                }
            }
        }
        else
        {
            maybeDumpFrame(innerPayload, innerPayload);
            decodeFetchResult(innerPayload, out);
        }
    }
    return out;
}

}  // namespace chatterino::tiktok
