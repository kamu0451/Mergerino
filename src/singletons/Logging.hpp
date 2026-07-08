// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/QStringHash.hpp"

#include <QString>

#include <pajlada/signals/signalholder.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <unordered_set>

namespace chatterino {

class Settings;
struct Message;
using MessagePtr = std::shared_ptr<const Message>;
class LoggingChannel;

class ILogging
{
public:
    virtual ~ILogging() = default;

    virtual void addMessage(const QString &channelName, MessagePtr message,
                            const QString &platformName,
                            const QString &streamID) = 0;

    virtual void closeChannel(const QString &channelName,
                              const QString &platformName) = 0;
};

class Logging : public ILogging
{
public:
    Logging(Settings &settings);

    void addMessage(const QString &channelName, MessagePtr message,
                    const QString &platformName,
                    const QString &streamID) override;

    void closeChannel(const QString &channelName,
                      const QString &platformName) override;

private:
    using PlatformName = QString;
    using ChannelName = QString;
    std::map<PlatformName,
             std::map<ChannelName, std::unique_ptr<LoggingChannel>>>
        loggingChannels_;
    std::mutex loggingChannelsMutex_;

    // Keeps cached values of logging settings for the message ingestion path.
    std::unordered_set<ChannelName> onlyLogListedChannels_;
    pajlada::Signals::SignalHolder settingConnections_;

    void addMessageToChannel(const QString &channelName, MessagePtr message,
                             const QString &platformName,
                             const QString &streamID);
};

}  // namespace chatterino
