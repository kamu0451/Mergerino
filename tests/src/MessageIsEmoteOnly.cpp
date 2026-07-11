// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "messages/Message.hpp"

#include "messages/MessageElement.hpp"
#include "messages/MessageFlag.hpp"
#include "Test.hpp"

#include <memory>

using namespace chatterino;

// NOTE: these cases stick to TextElement with hand-picked flags rather than
// EmoteElement/BadgeElement. Building an Emote/Image-backed element registers
// with the process-global image pool, which is torn down out of order under
// the minimal mock application and crashes the standalone test process ctest
// spawns per test (see the comment in KickMessageBuilder.cpp). isEmoteOnly()
// only inspects MessageElement::getFlags(), so a plain TextElement flagged as
// Emote/EmojiAll exercises the same logic without touching that pool.

TEST(MessageIsEmoteOnly, TrueForEmoteFlaggedElementsOnly)
{
    Message message;
    message.elements.push_back(std::make_unique<TextElement>(
        "Kappa", MessageElementFlag::Emote));
    message.elements.push_back(std::make_unique<TextElement>(
        "PogChamp", MessageElementFlag::Emote));

    EXPECT_TRUE(message.isEmoteOnly());
}

TEST(MessageIsEmoteOnly, TrueForEmojiFlaggedElements)
{
    // The element's flags are all isEmoteOnly() looks at; the actual emoji
    // text/codepoint is irrelevant here.
    Message message;
    message.elements.push_back(
        std::make_unique<TextElement>("emoji", MessageElementFlag::EmojiAll));

    EXPECT_TRUE(message.isEmoteOnly());
}

TEST(MessageIsEmoteOnly, FalseWithNoElements)
{
    Message message;

    EXPECT_FALSE(message.isEmoteOnly());
}

TEST(MessageIsEmoteOnly, FalseForPlainText)
{
    Message message;
    message.elements.push_back(
        std::make_unique<TextElement>("hello world", MessageElementFlag::Text));

    EXPECT_FALSE(message.isEmoteOnly());
}

TEST(MessageIsEmoteOnly, FalseWhenTextIsMixedWithEmotes)
{
    Message message;
    message.elements.push_back(
        std::make_unique<TextElement>("gg", MessageElementFlag::Text));
    message.elements.push_back(std::make_unique<TextElement>(
        "Kappa", MessageElementFlag::Emote));

    EXPECT_FALSE(message.isEmoteOnly());
}

// A cheer's visible body is its cheermote(s) plus any emotes, which would
// otherwise satisfy the emote-only check, but it's a paid bits message and
// must never be hidden by "hide emote-only messages".
TEST(MessageIsEmoteOnly, FalseForCheerMessageEvenWithOnlyEmoteElements)
{
    Message message;
    message.flags.set(MessageFlag::CheerMessage);
    message.bits = 100;
    message.elements.push_back(std::make_unique<TextElement>(
        "Cheer100", MessageElementFlag::Emote));

    EXPECT_FALSE(message.isEmoteOnly());
}

// Same rejection must apply purely off a positive bits count, independent of
// whether CheerMessage happens to be set.
TEST(MessageIsEmoteOnly, FalseWhenBitsIsPositiveWithoutCheerMessageFlag)
{
    Message message;
    message.bits = 1;
    message.elements.push_back(std::make_unique<TextElement>(
        "Kappa", MessageElementFlag::Emote));

    EXPECT_FALSE(message.isEmoteOnly());
}
