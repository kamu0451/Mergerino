// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT
//
// Minimal protobuf wire-format reader for the TikTok provider. Not a full
// protobuf implementation: varint, length-delimited, fixed32/64, and
// tag-aware skip for unknown fields. Enough to walk the tree of messages
// TikTok streams over webcast sockets without pulling libprotobuf in.

#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace chatterino::tiktok::proto {

enum class WireType : std::uint8_t {
    Varint = 0,
    Fixed64 = 1,
    LengthDelimited = 2,
    StartGroup = 3,
    EndGroup = 4,
    Fixed32 = 5,
};

class Reader
{
public:
    Reader() = default;

    Reader(const std::uint8_t *data, std::size_t size)
        : cur_(data)
        , end_(data + size)
    {
    }

    static Reader fromSpan(std::span<const std::uint8_t> bytes)
    {
        return Reader(bytes.data(), bytes.size());
    }

    bool hasMore() const
    {
        return cur_ < end_;
    }

    bool readTag(std::uint32_t &fieldNumber, WireType &wireType)
    {
        std::uint64_t tag{};
        if (!this->readVarint(tag))
        {
            return false;
        }
        fieldNumber = static_cast<std::uint32_t>(tag >> 3);
        wireType = static_cast<WireType>(tag & 0b111U);
        return true;
    }

    bool readVarint(std::uint64_t &value)
    {
        value = 0;
        int shift = 0;
        while (cur_ < end_)
        {
            const std::uint8_t b = *cur_++;
            value |= static_cast<std::uint64_t>(b & 0x7FU) << shift;
            if ((b & 0x80U) == 0)
            {
                return true;
            }
            shift += 7;
            if (shift >= 64)
            {
                return false;
            }
        }
        return false;
    }

    bool readFixed32(std::uint32_t &value)
    {
        if (end_ - cur_ < 4)
        {
            return false;
        }
        std::memcpy(&value, cur_, 4);
        cur_ += 4;
        return true;
    }

    bool readFixed64(std::uint64_t &value)
    {
        if (end_ - cur_ < 8)
        {
            return false;
        }
        std::memcpy(&value, cur_, 8);
        cur_ += 8;
        return true;
    }

    bool readLengthDelimited(std::span<const std::uint8_t> &out)
    {
        std::uint64_t length{};
        if (!this->readVarint(length))
        {
            return false;
        }
        if (static_cast<std::uint64_t>(end_ - cur_) < length)
        {
            return false;
        }
        out = std::span<const std::uint8_t>(
            cur_, static_cast<std::size_t>(length));
        cur_ += length;
        return true;
    }

    bool readString(std::string_view &out)
    {
        std::span<const std::uint8_t> bytes;
        if (!this->readLengthDelimited(bytes))
        {
            return false;
        }
        out = std::string_view(reinterpret_cast<const char *>(bytes.data()),
                               bytes.size());
        return true;
    }

    bool skip(WireType wireType)
    {
        switch (wireType)
        {
            case WireType::Varint: {
                std::uint64_t v{};
                return this->readVarint(v);
            }
            case WireType::Fixed32: {
                std::uint32_t v{};
                return this->readFixed32(v);
            }
            case WireType::Fixed64: {
                std::uint64_t v{};
                return this->readFixed64(v);
            }
            case WireType::LengthDelimited: {
                std::span<const std::uint8_t> s;
                return this->readLengthDelimited(s);
            }
            case WireType::StartGroup:
            case WireType::EndGroup:
                return false;
        }
        return false;
    }

private:
    const std::uint8_t *cur_{nullptr};
    const std::uint8_t *end_{nullptr};
};

}  // namespace chatterino::tiktok::proto
