// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Logging.hpp"

#include "controllers/logging/LoggedUsers.hpp"
#include "messages/Message.hpp"
#include "singletons/helper/LoggingChannel.hpp"
#include "singletons/Settings.hpp"

#include <QDir>
#include <QStandardPaths>

#include <memory>
#include <utility>

namespace chatterino {

Logging::Logging(Settings &settings)
{
    logging::initLoggedUsers();

    auto refreshLoggedChannels = [this, &settings] {
        std::lock_guard lock(this->loggingChannelsMutex_);
        this->onlyLogListedChannels_.clear();

        for (const auto &loggedChannel : *settings.loggedChannels.readOnly())
        {
            this->onlyLogListedChannels_.insert(
                loggedChannel.channelName().toLower());
        }
    };

    refreshLoggedChannels();

    this->settingConnections_.managedConnect(
        settings.loggedChannels.delayedItemsChanged,
        [this, refreshLoggedChannels]() {
            refreshLoggedChannels();
        });

    auto refreshLoggedUsers = [this, &settings] {
        std::lock_guard lock(this->loggingChannelsMutex_);
        this->loggedUsers_.clear();

        for (const auto &loggedUser : *settings.loggedUsers.readOnly())
        {
            const auto normalized =
                logging::normalizeLoggedUserName(loggedUser.channelName());
            if (!normalized.isEmpty())
            {
                this->loggedUsers_.insert(normalized);
            }
        }
    };

    refreshLoggedUsers();

    this->settingConnections_.managedConnect(
        settings.loggedUsers.delayedItemsChanged,
        [this, refreshLoggedUsers]() {
            refreshLoggedUsers();
        });
}

void Logging::addMessage(const QString &channelName, MessagePtr message,
                         const QString &platformName, const QString &streamID)
{
    if (platformName.isEmpty())
    {
        return;
    }

    if (!getSettings()->enableLogging)
    {
        return;
    }

    const auto lowerChannelName = channelName.toLower();

    std::lock_guard lock(this->loggingChannelsMutex_);

    // Normalizing runs a regex per message; skip it while no users are logged.
    QString lowerUserName;
    if (!this->loggedUsers_.empty())
    {
        lowerUserName = logging::normalizeLoggedUserName(message->loginName);
    }
    const bool logUser = !lowerUserName.isEmpty() &&
                         this->loggedUsers_.contains(lowerUserName);

    const bool logChannel = !getSettings()->onlyLogListedChannels ||
                            this->onlyLogListedChannels_.contains(
                                lowerChannelName);

    if (logChannel)
    {
        this->addMessageToChannel(channelName, message, platformName, streamID);
    }

    if (logUser)
    {
        this->addMessageToChannel(QStringLiteral("/users/") + lowerUserName,
                                  message, platformName, streamID);
    }
}

void Logging::addMessageToChannel(const QString &channelName, MessagePtr message,
                                  const QString &platformName,
                                  const QString &streamID)
{
    auto platIt = this->loggingChannels_.find(platformName);
    if (platIt == this->loggingChannels_.end())
    {
        auto *channel = new LoggingChannel(channelName, platformName);
        channel->addMessage(message, streamID);
        auto map = std::map<QString, std::unique_ptr<LoggingChannel>>();
        this->loggingChannels_[platformName] = std::move(map);
        auto &ref = this->loggingChannels_.at(platformName);
        ref.emplace(channelName, channel);
        return;
    }
    auto chanIt = platIt->second.find(channelName);
    if (chanIt == platIt->second.end())
    {
        auto *channel = new LoggingChannel(channelName, platformName);
        channel->addMessage(message, streamID);
        platIt->second.emplace(channelName, channel);
    }
    else
    {
        chanIt->second->addMessage(message, streamID);
    }
}

void Logging::closeChannel(const QString &channelName,
                           const QString &platformName)
{
    if (platformName.isEmpty())
    {
        return;
    }

    std::lock_guard lock(this->loggingChannelsMutex_);

    auto platIt = this->loggingChannels_.find(platformName);
    if (platIt == this->loggingChannels_.end())
    {
        return;
    }
    platIt->second.erase(channelName);
}

}  // namespace chatterino
