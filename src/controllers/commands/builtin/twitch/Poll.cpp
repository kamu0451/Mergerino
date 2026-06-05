// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/twitch/Poll.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "controllers/commands/common/ChannelAction.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "widgets/dialogs/CreatePollDialog.hpp"

#include <chrono>
#include <memory>

namespace {

using namespace chatterino;

constexpr auto MIN_POLL_DURATION = std::chrono::seconds(10);
constexpr auto MAX_POLL_DURATION = std::chrono::seconds(1800);

std::shared_ptr<TwitchChannel> resolveTwitchChannel(const ChannelPtr &channel)
{
    if (channel == nullptr)
    {
        return nullptr;
    }

    if (auto twitch = std::dynamic_pointer_cast<TwitchChannel>(channel))
    {
        return twitch;
    }

    if (auto merged = std::dynamic_pointer_cast<MergedChannel>(channel))
    {
        return std::dynamic_pointer_cast<TwitchChannel>(
            merged->twitchChannel());
    }

    return nullptr;
}

void notifyPollsAndPredictionsChanged(const ChannelPtr &channel)
{
    if (auto twitch = resolveTwitchChannel(channel))
    {
        twitch->streamStatusChanged.invoke();
    }
}

}  // namespace

namespace chatterino::commands {

QString createPoll(const CommandContext &ctx)
{
    if (ctx.words.size() <= 1)
    {
        if (ctx.twitchChannel == nullptr)
        {
            const auto err = QStringLiteral(
                "The /poll command only works in Twitch channels");
            if (ctx.channel != nullptr)
            {
                ctx.channel->addSystemMessage(err);
            }
            else
            {
                qCWarning(chatterinoCommands)
                    << "Invalid command context:" << err;
            }
            return "";
        }

        auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
        if (currentUser->isAnon())
        {
            ctx.channel->addSystemMessage(
                "You must be logged in to create a poll!");
            return "";
        }

        CreatePollDialog::showDialog(ctx.channel, *ctx.twitchChannel);
        return "";
    }

    const auto command = QStringLiteral("/poll");
    const auto usage = QStringLiteral(
        R"(Usage: "/poll --title "<title>" --duration <duration>[time unit] --choice "<choice1>" --choice "<choice2>" [options...]" - Creates a poll for users to vote among the defined options. Title may not exceed 60 characters. There must be between two and five poll choices. Duration must be a positive integer; time unit (optional, default=s) must be one of s, m; maximum duration is 30 minutes. Options: --points <points> to allow spending the specified channel points for each additional vote.)");
    const auto action = parseUserParticipationAction(
        ctx, command, usage, MIN_POLL_DURATION, MAX_POLL_DURATION);

    if (!action.has_value())
    {
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(action.error());
        }
        else
        {
            qCWarning(chatterinoCommands)
                << "Error parsing command:" << action.error();
        }
        return "";
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        ctx.channel->addSystemMessage(
            "You must be logged in to create a poll!");
        return "";
    }

    const auto &poll = action.value();
    getHelix()->createPoll(
        poll.broadcasterID, poll.title, poll.choices, poll.duration,
        poll.pointsPerVote,
        [channel = ctx.channel, poll] {
            channel->addSystemMessage(
                QString("Created poll: '%1'").arg(poll.title));
            notifyPollsAndPredictionsChanged(channel);
        },
        [channel = ctx.channel](const auto &error) {
            channel->addSystemMessage("Failed to create poll - " + error);
        });

    return "";
}

QString endPoll(const CommandContext &ctx)
{
    if (ctx.twitchChannel == nullptr)
    {
        const auto err = QStringLiteral(
            "The /endpoll command only works in Twitch channels");
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(err);
        }
        else
        {
            qCWarning(chatterinoCommands) << "Invalid command context:" << err;
        }
        return "";
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        ctx.channel->addSystemMessage("You must be logged in to end a poll!");
        return "";
    }

    const auto roomId = ctx.twitchChannel->roomId();
    getHelix()->getPolls(
        roomId, {}, 1, {},
        [channel = ctx.channel, roomId](const auto &result) {
            if (result.polls.empty())
            {
                channel->addSystemMessage("Failed to find any polls");
                return;
            }

            auto poll = result.polls.front();
            if (poll.status != "ACTIVE")
            {
                channel->addSystemMessage("Could not find an active poll");
                return;
            }

            getHelix()->endPoll(
                roomId, poll.id, false,
                [channel](const HelixPoll &data) {
                    notifyPollsAndPredictionsChanged(channel);

                    // find most popular choice
                    HelixPollChoice winner = data.choices.front();
                    int totalVotes = 0;
                    int winnerCount = 0;
                    for (const auto &choice : data.choices)
                    {
                        totalVotes += choice.votes;
                        if (choice.votes > winner.votes)
                        {
                            winner = choice;
                            winnerCount = 1;
                        }
                        else if (choice.votes == winner.votes)
                        {
                            winnerCount++;
                        }
                    }

                    if (totalVotes == 0)
                    {
                        channel->addSystemMessage(
                            QString("Poll ended with zero votes: '%1'")
                                .arg(data.title));
                        return;
                    }

                    if (winnerCount > 1)
                    {
                        channel->addSystemMessage(
                            QString("Poll ended in a draw: '%1'")
                                .arg(data.title));
                        return;
                    }

                    const double percent =
                        100.0 * winner.votes / std::max(totalVotes, 1);

                    channel->addSystemMessage(
                        QString(
                            "Ended poll: '%1' - '%2' won with %3 votes (%4%)")
                            .arg(data.title, winner.title,
                                 QString::number(winner.votes),
                                 QString::number(percent, 'f', 1)));
                },
                [channel](const auto &error) {
                    channel->addSystemMessage("Failed to end the poll - " +
                                              error);
                });
        },
        [channel = ctx.channel](const auto &error) {
            channel->addSystemMessage("Failed to query polls - " + error);
        });

    return "";
}

QString cancelPoll(const CommandContext &ctx)
{
    if (ctx.twitchChannel == nullptr)
    {
        const auto err = QStringLiteral(
            "The /cancelpoll command only works in Twitch channels");
        if (ctx.channel != nullptr)
        {
            ctx.channel->addSystemMessage(err);
        }
        else
        {
            qCWarning(chatterinoCommands) << "Invalid command context:" << err;
        }
        return "";
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        ctx.channel->addSystemMessage(
            "You must be logged in to cancel a poll!");
        return "";
    }

    const auto roomId = ctx.twitchChannel->roomId();
    getHelix()->getPolls(
        roomId, {}, 1, {},
        [channel = ctx.channel, roomId](const auto &result) {
            if (result.polls.empty())
            {
                channel->addSystemMessage("Failed to find any polls");
                return;
            }

            auto poll = result.polls.front();
            if (poll.status != "ACTIVE")
            {
                channel->addSystemMessage("Could not find an active poll");
                return;
            }

            getHelix()->endPoll(
                roomId, poll.id, true,
                [channel](const HelixPoll &data) {
                    channel->addSystemMessage(
                        QString("Canceled poll: '%1'").arg(data.title));
                    notifyPollsAndPredictionsChanged(channel);
                },
                [channel](const auto &error) {
                    channel->addSystemMessage("Failed to cancel the poll - " +
                                              error);
                });
        },
        [channel = ctx.channel](const auto &error) {
            channel->addSystemMessage("Failed to query polls - " + error);
        });

    return "";
}

}  // namespace chatterino::commands
