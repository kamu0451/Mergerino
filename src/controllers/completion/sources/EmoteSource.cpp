// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/completion/sources/EmoteSource.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/completion/sources/Helpers.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Message.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvPersonalEmotes.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "widgets/splits/InputCompletionItem.hpp"

#include <algorithm>

namespace chatterino::completion {

namespace {

void addEmotes(std::vector<EmoteItem> &out, const EmoteMap &map,
               const QString &providerName)
{
    for (auto &&emote : map)
    {
        out.push_back({.emote = emote.second,
                       .searchName = emote.first.string,
                       .tabCompletionName = emote.first.string,
                       .displayName = emote.second->name.string,
                       .providerName = providerName,
                       .isEmoji = false});
    }
}

void addEmojis(std::vector<EmoteItem> &out, const std::vector<EmojiPtr> &map)
{
    for (const auto &emoji : map)
    {
        for (auto &&shortCode : emoji->shortCodes)
        {
            out.push_back(
                {.emote = emoji->emote,
                 .searchName = shortCode,
                 .tabCompletionName = QStringLiteral(":%1:").arg(shortCode),
                 .displayName = shortCode,
                 .providerName = "Emoji",
                 .isEmoji = true});
        }
    };
}

bool includesPlatform(const std::vector<MessagePlatform> &platforms,
                      MessagePlatform platform)
{
    return platforms.empty() ||
           std::find(platforms.begin(), platforms.end(), platform) !=
               platforms.end();
}

}  // namespace

EmoteSource::EmoteSource(const Channel *channel,
                         std::unique_ptr<EmoteStrategy> strategy,
                         ActionCallback callback,
                         std::vector<MessagePlatform> platformFilter)
    : strategy_(std::move(strategy))
    , callback_(std::move(callback))
    , platformFilter_(std::move(platformFilter))
{
    this->initializeFromChannel(channel);
}

void EmoteSource::update(const QString &query)
{
    this->output_.clear();
    if (this->strategy_)
    {
        this->strategy_->apply(this->items_, this->output_, query);
    }
}

void EmoteSource::addToListModel(GenericListModel &model, size_t maxCount) const
{
    addVecToListModel(this->output_, model, maxCount,
                      [this](const EmoteItem &e) {
                          return std::make_unique<InputCompletionItem>(
                              e.emote, e.displayName + " - " + e.providerName,
                              this->callback_);
                      });
}

void EmoteSource::addToStringList(QStringList &list, size_t maxCount,
                                  bool /* isFirstWord */) const
{
    addVecToStringList(this->output_, list, maxCount, [](const EmoteItem &e) {
        return e.tabCompletionName + " ";
    });
}

void EmoteSource::initializeFromChannel(const Channel *channel)
{
    auto *app = getApp();

    std::vector<EmoteItem> emotes;
    if (channel == nullptr)
    {
        addEmojis(emotes, app->getEmotes()->getEmojis()->getEmojis());
        this->items_ = std::move(emotes);
        return;
    }

    const bool includeTwitch =
        includesPlatform(this->platformFilter_, MessagePlatform::AnyOrTwitch);
    const bool includeKick =
        includesPlatform(this->platformFilter_, MessagePlatform::Kick);
    const auto *mergedChannel = dynamic_cast<const MergedChannel *>(channel);
    const auto *tc = dynamic_cast<const TwitchChannel *>(channel);
    // returns true also for special Twitch channels (/live, /mentions, /whispers, etc.)
    if (includeTwitch && channel->isTwitchChannel())
    {
        if (tc)
        {
            if (auto twitch = tc->localTwitchEmotes())
            {
                addEmotes(emotes, *twitch, "Local Twitch Emotes");
            }

            auto user = getApp()->getAccounts()->twitch.getCurrent();
            addEmotes(emotes, **user->accessEmotes(), "Twitch Emote");

            for (const auto &map :
                 app->getSeventvPersonalEmotes()->getEmoteSetsForTwitchUser(
                     app->getAccounts()->twitch.getCurrent()->getUserId()))
            {
                addEmotes(emotes, *map, "Personal 7TV");
            }

            // TODO extract "Channel {BetterTTV,7TV,FrankerFaceZ}" text into a #define.
            if (auto bttv = tc->bttvEmotes())
            {
                addEmotes(emotes, *bttv, "Channel BetterTTV");
            }
            if (auto ffz = tc->ffzEmotes())
            {
                addEmotes(emotes, *ffz, "Channel FrankerFaceZ");
            }
            if (auto seventv = tc->seventvEmotes())
            {
                addEmotes(emotes, *seventv, "Channel 7TV");
            }
        }
    }

    const auto *kickChannel = dynamic_cast<const KickChannel *>(channel);
    if (includeKick && kickChannel)
    {
        const auto list =
            app->getSeventvPersonalEmotes()->getEmoteSetsForKickUser(
                app->getAccounts()->kick.current()->userID());
        for (const auto &map : list)
        {
            addEmotes(emotes, *map, "Personal 7TV");
        }

        addEmotes(emotes, *kickChannel->kickChannelEmotes(),
                  "Channel Kick Emote");
        addEmotes(emotes, *kickChannel->seventvEmotes(), "Channel 7TV");
        addEmotes(emotes, *getApp()->getKickChatServer()->globalEmotes(),
                  "Kick Emote");
    }
    else if (mergedChannel)
    {
        if (includeTwitch)
        {
            if (const auto &twitchSource = mergedChannel->twitchChannel())
            {
                if (const auto *twitch =
                        dynamic_cast<const TwitchChannel *>(twitchSource.get()))
                {
                    if (auto twitchLocal = twitch->localTwitchEmotes())
                    {
                        addEmotes(emotes, *twitchLocal, "Local Twitch Emotes");
                    }

                    auto user = getApp()->getAccounts()->twitch.getCurrent();
                    addEmotes(emotes, **user->accessEmotes(), "Twitch Emote");

                    for (const auto &map :
                         app->getSeventvPersonalEmotes()
                             ->getEmoteSetsForTwitchUser(user->getUserId()))
                    {
                        addEmotes(emotes, *map, "Personal 7TV");
                    }

                    if (auto bttv = twitch->bttvEmotes())
                    {
                        addEmotes(emotes, *bttv, "Channel BetterTTV");
                    }
                    if (auto ffz = twitch->ffzEmotes())
                    {
                        addEmotes(emotes, *ffz, "Channel FrankerFaceZ");
                    }
                    if (auto seventv = twitch->seventvEmotes())
                    {
                        addEmotes(emotes, *seventv, "Channel 7TV");
                    }
                }
            }
        }

        if (includeKick)
        {
            if (const auto &kickSource = mergedChannel->kickChannel())
            {
                if (const auto *kick =
                        dynamic_cast<const KickChannel *>(kickSource.get()))
                {
                    const auto list =
                        app->getSeventvPersonalEmotes()->getEmoteSetsForKickUser(
                            app->getAccounts()->kick.current()->userID());
                    for (const auto &map : list)
                    {
                        addEmotes(emotes, *map, "Personal 7TV");
                    }

                    addEmotes(emotes, *kick->kickChannelEmotes(),
                              "Channel Kick Emote");
                    addEmotes(emotes, *kick->seventvEmotes(), "Channel 7TV");
                    addEmotes(emotes,
                              *getApp()->getKickChatServer()->globalEmotes(),
                              "Kick Emote");
                }
            }
        }
    }

    const bool hasTwitchEmoteContext =
        includeTwitch &&
        (channel->isTwitchChannel() ||
         (mergedChannel && mergedChannel->twitchChannel()));
    const bool hasKickEmoteContext =
        includeKick &&
        (channel->isKickChannel() ||
         (mergedChannel && mergedChannel->kickChannel()));

    if (hasTwitchEmoteContext)
    {
        if (auto bttvG = app->getBttvEmotes()->emotes())
        {
            addEmotes(emotes, *bttvG, "Global BetterTTV");
        }
        if (auto ffzG = app->getFfzEmotes()->emotes())
        {
            addEmotes(emotes, *ffzG, "Global FrankerFaceZ");
        }
    }

    if (hasTwitchEmoteContext || hasKickEmoteContext)
    {
        if (auto seventvG = app->getSeventvEmotes()->globalEmotes())
        {
            addEmotes(emotes, *seventvG, "Global 7TV");
        }
    }

    addEmojis(emotes, app->getEmotes()->getEmojis()->getEmojis());

    this->items_ = std::move(emotes);
}

const std::vector<EmoteItem> &EmoteSource::output() const
{
    return this->output_;
}

}  // namespace chatterino::completion
