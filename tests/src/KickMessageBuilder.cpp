// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT

#include "providers/kick/KickMessageBuilder.hpp"

#include "controllers/accounts/AccountController.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "messages/Emote.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "messages/MessageFlag.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/seventv/SeventvBadges.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvPersonalEmotes.hpp"
#include "Test.hpp"
#include "util/BoostJsonWrap.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>

#include <memory>

using namespace chatterino;

namespace {

// A mock application that wires up exactly the getApp() subsystems the Kick
// message builder reaches for: emote providers (FFZ/BTTV/7TV/personal + the
// emoji parser), the account controller (for the "is this me?" self-state
// check), 7TV badges, the highlight controller, and a Kick chat server so the
// KickChannel destructor's leaveRoom() call has a live target.
class MockApplication : public mock::BaseApplication
{
public:
    MockApplication()
        : highlights(this->settings, &this->accounts)
    {
    }

    EmoteController *getEmotes() override
    {
        return &this->emotes;
    }

    AccountController *getAccounts() override
    {
        return &this->accounts;
    }

    HighlightController *getHighlights() override
    {
        return &this->highlights;
    }

    SeventvBadges *getSeventvBadges() override
    {
        return &this->seventvBadges;
    }

    SeventvPersonalEmotes *getSeventvPersonalEmotes() override
    {
        return &this->personalEmotes;
    }

    BttvEmotes *getBttvEmotes() override
    {
        return &this->bttvEmotes;
    }

    FfzEmotes *getFfzEmotes() override
    {
        return &this->ffzEmotes;
    }

    SeventvEmotes *getSeventvEmotes() override
    {
        return &this->seventvEmotes;
    }

    KickChatServer *getKickChatServer() override
    {
        return &this->kickChatServer;
    }

    mock::EmoteController emotes;
    AccountController accounts;
    HighlightController highlights;
    SeventvBadges seventvBadges;
    SeventvPersonalEmotes personalEmotes;
    BttvEmotes bttvEmotes;
    FfzEmotes ffzEmotes;
    SeventvEmotes seventvEmotes;
    KickChatServer kickChatServer;
};

// Collects the rendered text of every TextElement in a message, in element
// order. Some builder output (e.g. the Kicks-gift name) is only ever visible
// through its own TextElement and never makes it into Message::messageText,
// so asserting on messageText alone can't verify it landed correctly.
QStringList textElementTexts(const Message &msg)
{
    QStringList out;
    for (const auto &el : msg.elements)
    {
        if (const auto *text = dynamic_cast<TextElement *>(el.get()))
        {
            out.append(text->words().join(' '));
        }
    }
    return out;
}


}  // namespace

class KickMessageBuilderTest : public ::testing::Test
{
public:
    void SetUp() override
    {
        this->app = std::make_unique<MockApplication>();
        this->channel = std::make_shared<KickChannel>("TestStreamer");
    }

    void TearDown() override
    {
        // The KickChannel destructor calls getApp()->getKickChatServer(), so
        // it must be torn down while the mock application is still alive.
        this->channel.reset();
        this->app.reset();
    }

    // Parses `json` and hands the builder a BoostJsonObject view over it. The
    // returned value must outlive the view, so callers keep it on the stack.
    static boost::json::value parse(std::string_view json)
    {
        return boost::json::parse(json);
    }

    std::unique_ptr<MockApplication> app;
    std::shared_ptr<KickChannel> channel;
};

// ---------- makeChatMessage: core fields ----------
//
// NOTE: this case uses plain text and does NOT exercise emote/badge elements.
// Building an EmoteElement or BadgeElement registers an Image with the
// process-global image/expiration pool, and under the minimal mock application
// (no real image pipeline) that global is torn down out of order at process
// exit, crashing the standalone test process that ctest spawns per test.
// Emote-token expansion and badge indexing are exercised end-to-end by the
// full-app snapshot suites; here we lock in the getApp-free JSON->fields path.

TEST_F(KickMessageBuilderTest, ChatMessageBasicFields)
{
    auto json = parse(R"({
        "id": "8f2c0f7e-1c2d-4e5a-9b3c-0a1b2c3d4e5f",
        "content": "hello world",
        "created_at": "2024-05-14T12:31:47Z",
        "sender": {
            "id": 998877,
            "username": "CoolViewer",
            "identity": {
                "color": "#00FF00"
            }
        }
    })");
    BoostJsonObject data(json.as_object());

    auto result =
        KickMessageBuilder::makeChatMessage(this->channel.get(), data);
    auto message = result.first;

    ASSERT_NE(message, nullptr);

    // Core identity + platform tagging.
    EXPECT_EQ(message->platform, MessagePlatform::Kick);
    EXPECT_EQ(message->id, "8f2c0f7e-1c2d-4e5a-9b3c-0a1b2c3d4e5f");
    EXPECT_EQ(message->displayName, "CoolViewer");
    EXPECT_EQ(message->loginName, "coolviewer");
    EXPECT_EQ(message->userID, "998877");
    EXPECT_EQ(message->messageText, "hello world");

    // No highlight phrases are configured on the mock application, so the
    // returned alert must be the default "nothing matched" value.
    EXPECT_FALSE(result.second.playSound);
    EXPECT_FALSE(result.second.windowAlert);
    EXPECT_TRUE(result.second.customSound.isEmpty());
}

// ---------- makeTimeoutMessage: timeout + permanent ban ----------

TEST_F(KickMessageBuilderTest, TimeoutMessageWithModerator)
{
    auto json = parse(R"({
        "user": { "username": "BadUser" },
        "banned_by": { "username": "ModGuy" },
        "permanent": false,
        "duration": 10
    })");
    BoostJsonObject data(json.as_object());

    auto message = KickMessageBuilder::makeTimeoutMessage(
        this->channel.get(), QDateTime::currentDateTime(), data);

    ASSERT_NE(message, nullptr);
    EXPECT_EQ(message->platform, MessagePlatform::Kick);
    EXPECT_TRUE(message->flags.has(MessageFlag::Timeout));
    EXPECT_TRUE(message->flags.has(MessageFlag::ModerationAction));
    EXPECT_EQ(message->timeoutUser, "baduser");

    // "ModGuy timed out BadUser for 10m." -- duration is minutes, so 10 -> 10m.
    EXPECT_TRUE(message->messageText.contains("ModGuy"));
    EXPECT_TRUE(message->messageText.contains("timed out"));
    EXPECT_TRUE(message->messageText.contains("BadUser"));
    EXPECT_TRUE(message->messageText.contains("10m"));
}

TEST_F(KickMessageBuilderTest, PermanentBanMessage)
{
    auto json = parse(R"({
        "user": { "username": "BadUser" },
        "banned_by": { "username": "ModGuy" },
        "permanent": true
    })");
    BoostJsonObject data(json.as_object());

    auto message = KickMessageBuilder::makeTimeoutMessage(
        this->channel.get(), QDateTime::currentDateTime(), data);

    ASSERT_NE(message, nullptr);
    EXPECT_TRUE(message->flags.has(MessageFlag::Timeout));
    EXPECT_TRUE(message->flags.has(MessageFlag::ModerationAction));
    EXPECT_EQ(message->timeoutUser, "baduser");
    EXPECT_TRUE(message->messageText.contains("permanently banned"));
    // No duration should be printed for a permanent ban.
    EXPECT_FALSE(message->messageText.contains("for"));
}

TEST_F(KickMessageBuilderTest, TimeoutMessageWithoutModerator)
{
    auto json = parse(R"({
        "user": { "username": "BadUser" },
        "permanent": false,
        "duration": 5
    })");
    BoostJsonObject data(json.as_object());

    auto message = KickMessageBuilder::makeTimeoutMessage(
        this->channel.get(), QDateTime::currentDateTime(), data);

    ASSERT_NE(message, nullptr);
    EXPECT_TRUE(message->flags.has(MessageFlag::Timeout));
    EXPECT_EQ(message->timeoutUser, "baduser");

    // No banned_by -> loginName (normally set to the moderator) stays unset,
    // and the phrasing is "BadUser has been timed out for 5m." with no
    // moderator mentioned at all.
    EXPECT_TRUE(message->loginName.isEmpty());
    EXPECT_TRUE(message->messageText.contains("BadUser has been timed out"));
    EXPECT_TRUE(message->messageText.contains("5m"));
}

TEST_F(KickMessageBuilderTest, TimeoutMessageDurationFromExpiresAt)
{
    // duration is 0 but expires_at is 5 minutes out; the builder must derive
    // the printed duration from the expiry timestamp instead of rendering an
    // empty/zero duration.
    const auto now = QDateTime::fromMSecsSinceEpoch(1700000000000LL, Qt::UTC);
    const auto expiresAt = now.addSecs(300);

    const auto jsonText = QStringLiteral(R"({
        "user": { "username": "BadUser" },
        "banned_by": { "username": "ModGuy" },
        "permanent": false,
        "duration": 0,
        "expires_at": "%1"
    })")
                              .arg(expiresAt.toString(Qt::ISODate))
                              .toStdString();
    auto json = parse(jsonText);
    BoostJsonObject data(json.as_object());

    auto message =
        KickMessageBuilder::makeTimeoutMessage(this->channel.get(), now, data);

    ASSERT_NE(message, nullptr);
    EXPECT_TRUE(message->messageText.contains("5m"));
}

TEST_F(KickMessageBuilderTest, TimeoutMessageInvalidExpiresAtFallsBackToZero)
{
    // duration is 0 and expires_at fails to parse. KickMessageBuilder.cpp
    // falls back to a bare 0-second duration rather than propagating garbage
    // from QDateTime::fromString() -- formatTime(0) renders as an empty
    // string, so "for" ends up with nothing behind it but the trailing ".".
    auto json = parse(R"({
        "user": { "username": "BadUser" },
        "banned_by": { "username": "ModGuy" },
        "permanent": false,
        "duration": 0,
        "expires_at": "not-a-real-timestamp"
    })");
    BoostJsonObject data(json.as_object());

    auto message = KickMessageBuilder::makeTimeoutMessage(
        this->channel.get(), QDateTime::currentDateTime(), data);

    ASSERT_NE(message, nullptr);
    EXPECT_TRUE(message->messageText.contains("for ."));
}

// ---------- makeGiftedSubscriptionMessage: multi-recipient gift ----------

TEST_F(KickMessageBuilderTest, GiftedSubscriptionMessage)
{
    auto json = parse(R"({
        "gifter_username": "Santa",
        "gifted_usernames": ["Alice", "Bob"],
        "gifter_total": 5
    })");
    BoostJsonObject data(json.as_object());

    auto message = KickMessageBuilder::makeGiftedSubscriptionMessage(
        this->channel.get(), data);

    ASSERT_NE(message, nullptr);
    EXPECT_EQ(message->platform, MessagePlatform::Kick);
    EXPECT_TRUE(message->flags.has(MessageFlag::Subscription));
    EXPECT_EQ(message->giftedSubscriptionRecipientCount, 2U);
    EXPECT_EQ(message->loginName, "Santa");

    EXPECT_TRUE(message->messageText.contains("Santa"));
    EXPECT_TRUE(message->messageText.contains("gifted 2 subscriptions"));
    EXPECT_TRUE(message->messageText.contains("Alice"));
    EXPECT_TRUE(message->messageText.contains("Bob"));
    EXPECT_TRUE(message->messageText.contains("5 subs in total"));
}

TEST_F(KickMessageBuilderTest, GiftedSubscriptionEmptyReturnsNull)
{
    auto json = parse(R"({
        "gifter_username": "Santa",
        "gifted_usernames": [],
        "gifter_total": 0
    })");
    BoostJsonObject data(json.as_object());

    auto message = KickMessageBuilder::makeGiftedSubscriptionMessage(
        this->channel.get(), data);
    EXPECT_EQ(message, nullptr);
}

// ---------- makeRewardRedeemedMessage: channel-point reward ----------

TEST_F(KickMessageBuilderTest, RewardRedeemedMessage)
{
    auto json = parse(R"({
        "reward_title": "Hydrate",
        "username": "Gifter",
        "user_input": ""
    })");
    BoostJsonObject data(json.as_object());

    auto message = KickMessageBuilder::makeRewardRedeemedMessage(
        this->channel.get(), data);

    ASSERT_NE(message, nullptr);
    EXPECT_EQ(message->platform, MessagePlatform::Kick);
    EXPECT_TRUE(message->flags.has(MessageFlag::RedeemedChannelPointReward));
    EXPECT_TRUE(message->flags.has(MessageFlag::CheerMessage));
    EXPECT_EQ(message->loginName, "Gifter");
    EXPECT_TRUE(message->messageText.contains("redeemed"));
    EXPECT_TRUE(message->messageText.contains("Hydrate"));
}

// ---------- makeKicksGiftedMessage: "Kicks" tip gift ----------

TEST_F(KickMessageBuilderTest, KicksGiftedMessage)
{
    auto json = parse(R"({
        "sender": { "username": "Tipper" },
        "gift": { "name": "Rose", "amount": 100 },
        "message": ""
    })");
    BoostJsonObject data(json.as_object());

    auto message =
        KickMessageBuilder::makeKicksGiftedMessage(this->channel.get(), data);

    ASSERT_NE(message, nullptr);
    EXPECT_EQ(message->platform, MessagePlatform::Kick);
    EXPECT_TRUE(message->flags.has(MessageFlag::RedeemedChannelPointReward));
    EXPECT_TRUE(message->flags.has(MessageFlag::ElevatedMessage));
    EXPECT_TRUE(message->flags.has(MessageFlag::CheerMessage));
    EXPECT_EQ(message->kickGiftKicks, 100U);
    EXPECT_EQ(message->loginName, "Tipper");
    EXPECT_TRUE(message->messageText.contains("gifted"));
    EXPECT_TRUE(message->messageText.contains("100 Kicks"));

    // The gift name is rendered as its own bold TextElement rather than
    // folded into messageText, and appendOrEmplaceText() then appends the
    // kicks summary directly onto that same element -- so a bare element
    // count is trivially satisfied by unrelated TextElements ("gifted"). Find
    // the element that actually holds the gift name and check its content.
    bool foundGiftName = false;
    for (const auto &text : textElementTexts(*message))
    {
        if (text.contains("Rose"))
        {
            foundGiftName = true;
            EXPECT_TRUE(text.contains("100 Kicks"));
        }
    }
    EXPECT_TRUE(foundGiftName);
}
