#pragma once

#include "messages/Emote.hpp"
#include "messages/MessageElement.hpp"

#include <string_view>

namespace chatterino {

class KickBadges
{
public:
    static std::pair<EmotePtr, MessageElementFlag> lookup(
        std::string_view name);
    /// Falls back to `imageUrl` for badges we have no bundled icon for
    static std::pair<EmotePtr, MessageElementFlag> lookup(
        std::string_view name, const QString &imageUrl,
        const QString &displayName);
};

}  // namespace chatterino
