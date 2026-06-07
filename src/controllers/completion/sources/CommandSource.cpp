// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/completion/sources/CommandSource.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/completion/sources/Helpers.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "widgets/splits/InputCompletionItem.hpp"

#include <algorithm>

namespace chatterino::completion {

namespace {

void addCommand(const QString &command, std::vector<CommandItem> &out)
{
    if (command.startsWith('/') || command.startsWith('.'))
    {
        out.push_back({
            .name = command.mid(1),
            .prefix = command.at(0),
        });
    }
    else
    {
        out.push_back({
            .name = command,
            .prefix = "",
        });
    }
}

QString displayText(const CommandItem &command)
{
    return command.prefix + command.name;
}

QString commandKey(const CommandItem &command)
{
    return displayText(command).toLower();
}

bool commandLessThan(const CommandItem &a, const CommandItem &b)
{
    const auto nameCompare = QString::compare(a.name, b.name,
                                              Qt::CaseInsensitive);
    if (nameCompare != 0)
    {
        return nameCompare < 0;
    }

    return QString::compare(a.prefix, b.prefix, Qt::CaseInsensitive) < 0;
}

bool commandEquals(const CommandItem &a, const CommandItem &b)
{
    return QString::compare(commandKey(a), commandKey(b),
                            Qt::CaseInsensitive) == 0;
}

const QStringList &moderatorCommands()
{
    static const QStringList commands{
        "/announce",
        "/announceblue",
        "/announcegreen",
        "/announceorange",
        "/announcepurple",
        "/ban",
        "/banid",
        "/chatters",
        "/clear",
        "/delete",
        "/emoteonly",
        "/emoteonlyoff",
        "/followers",
        "/followersoff",
        "/lowtrust",
        "/monitor",
        "/poll",
        "/prediction",
        "/requests",
        "/restrict",
        "/r9kbeta",
        "/r9kbetaoff",
        "/shield",
        "/shieldoff",
        "/shoutout",
        "/slow",
        "/slowoff",
        "/subscribers",
        "/subscribersoff",
        "/timeout",
        "/unban",
        "/uniquechat",
        "/uniquechatoff",
        "/unmonitor",
        "/unrestrict",
        "/untimeout",
        "/warn",
    };

    return commands;
}

const QStringList &broadcasterCommands()
{
    static const QStringList commands{
        "/cancelprediction",
        "/cancelpoll",
        "/commercial",
        "/completeprediction",
        "/endpoll",
        "/host",
        "/lockprediction",
        "/marker",
        "/mod",
        "/mods",
        "/raid",
        "/setgame",
        "/settitle",
        "/unmod",
        "/unhost",
        "/unraid",
        "/unvip",
        "/vip",
        "/vips",
    };

    return commands;
}

bool canUseCommand(const CommandItem &command, const Channel *channel)
{
    const auto key = commandKey(command);

    if (broadcasterCommands().contains(key, Qt::CaseInsensitive))
    {
        return channel != nullptr && channel->isBroadcaster();
    }

    if (moderatorCommands().contains(key, Qt::CaseInsensitive))
    {
        return channel != nullptr && channel->hasModRights();
    }

    return true;
}

}  // namespace

CommandSource::CommandSource(std::unique_ptr<CommandStrategy> strategy,
                             ActionCallback callback, const Channel *channel,
                             bool slashCommandsOnly)
    : strategy_(std::move(strategy))
    , callback_(std::move(callback))
    , channel_(channel)
    , slashCommandsOnly_(slashCommandsOnly)
{
    this->initializeItems();
}

void CommandSource::update(const QString &query)
{
    this->output_.clear();
    if (this->strategy_)
    {
        this->strategy_->apply(this->items_, this->output_, query);
    }
}

void CommandSource::addToListModel(GenericListModel &model,
                                   size_t maxCount) const
{
    addVecToListModel(this->output_, model, maxCount,
                      [this](const CommandItem &command) {
                          return std::make_unique<InputCompletionItem>(
                              nullptr, displayText(command), this->callback_);
                      });
}

void CommandSource::addToStringList(QStringList &list, size_t maxCount,
                                    bool /* isFirstWord */) const
{
    addVecToStringList(this->output_, list, maxCount,
                       [](const CommandItem &command) {
                           return command.prefix + command.name + " ";
                       });
}

void CommandSource::initializeItems()
{
    std::vector<CommandItem> commands;

#ifdef CHATTERINO_HAVE_PLUGINS
    for (const auto &command : getApp()->getCommands()->pluginCommands())
    {
        addCommand(command, commands);
    }
#endif

    // Custom Chatterino commands
    for (const auto &command : getApp()->getCommands()->items)
    {
        addCommand(command.name, commands);
    }

    // Default Chatterino commands
    auto x = getApp()->getCommands()->getDefaultChatterinoCommandList();
    for (const auto &command : x)
    {
        addCommand(command, commands);
    }

    // Default Twitch commands
    for (const auto &command : TWITCH_DEFAULT_COMMANDS)
    {
        addCommand(command, commands);
    }

    commands.erase(std::remove_if(commands.begin(), commands.end(),
                                  [this](const CommandItem &command) {
                                      if (this->slashCommandsOnly_ &&
                                          command.prefix != "/")
                                      {
                                          return true;
                                      }

                                      return !canUseCommand(command,
                                                            this->channel_);
                                  }),
                   commands.end());

    std::sort(commands.begin(), commands.end(), commandLessThan);
    commands.erase(std::unique(commands.begin(), commands.end(), commandEquals),
                   commands.end());

    this->items_ = std::move(commands);
}

const std::vector<CommandItem> &CommandSource::output() const
{
    return this->output_;
}

}  // namespace chatterino::completion
