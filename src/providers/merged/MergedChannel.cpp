// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/merged/MergedChannel.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/youtube/YouTubeLiveChat.hpp"
#include "util/QStringHash.hpp"

#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>
#include <QStringList>

#include <algorithm>

namespace {

using namespace chatterino;

QString normalizeChannelName(QString value)
{
    value = value.trimmed();
    if (value.startsWith('#'))
    {
        value.remove(0, 1);
    }
    if (value.startsWith(u":kick:"))
    {
        value.remove(0, 6);
    }

    return value.toLower();
}

QString platformName(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return "Kick";
        case MessagePlatform::YouTube:
            return "YouTube";
        case MessagePlatform::AnyOrTwitch:
        default:
            return "Twitch";
    }
}

QString platformIconPath(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return ":/platforms/kick.svg";
        case MessagePlatform::YouTube:
            return ":/platforms/youtube.svg";
        case MessagePlatform::AnyOrTwitch:
        default:
            return ":/platforms/twitch.svg";
    }
}

QPixmap renderPlatformBadge(MessagePlatform platform)
{
    QPixmap pixmap(18, 18);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    QSvgRenderer renderer(platformIconPath(platform));
    renderer.render(&painter);
    return pixmap;
}

}  // namespace

namespace chatterino {

QString MergedChannelConfig::displayName() const
{
    const auto trimmed = this->tabName.trimmed();
    if (!trimmed.isEmpty())
    {
        return trimmed;
    }

    if (this->twitchEnabled)
    {
        const auto twitch = this->effectiveTwitchChannelName();
        if (!twitch.isEmpty())
        {
            return twitch;
        }
    }

    if (this->kickEnabled)
    {
        const auto kick = this->effectiveKickChannelName();
        if (!kick.isEmpty())
        {
            return kick;
        }
    }

    return "Merged chat";
}

QString MergedChannelConfig::effectiveTwitchChannelName() const
{
    const auto overrideName = normalizeChannelName(this->twitchChannelName);
    if (!overrideName.isEmpty())
    {
        return overrideName;
    }
    return normalizeChannelName(this->tabName);
}

QString MergedChannelConfig::effectiveKickChannelName() const
{
    const auto overrideName = normalizeChannelName(this->kickChannelName);
    if (!overrideName.isEmpty())
    {
        return overrideName;
    }
    return normalizeChannelName(this->tabName);
}

MergedChannel::MergedChannel(MergedChannelConfig config)
    : Channel(config.displayName(), Channel::Type::Merged)
    , ChannelChatters(static_cast<Channel &>(*this))
    , config_(std::move(config))
    , displayName_(this->config_.displayName())
{
    this->platform_ = "merged";
    this->initializeSources();
    this->refreshStatusText();
}

MergedChannel::~MergedChannel() = default;

const MergedChannelConfig &MergedChannel::config() const
{
    return this->config_;
}

const QString &MergedChannel::getDisplayName() const
{
    return this->displayName_;
}

const QString &MergedChannel::getLocalizedName() const
{
    return this->displayName_;
}

bool MergedChannel::isMergedChannel() const
{
    return true;
}

bool MergedChannel::canSendMessage() const
{
    return this->isWritable();
}

bool MergedChannel::isWritable() const
{
    return (this->config_.twitchEnabled && this->twitchChannel_ != nullptr) ||
           (this->config_.kickEnabled && this->kickChannel_ != nullptr);
}

void MergedChannel::sendMessage(const QString &message)
{
    if (message.trimmed().isEmpty())
    {
        return;
    }

    bool sent = false;
    QStringList unavailablePlatforms;

    if (this->config_.twitchEnabled && this->twitchChannel_)
    {
        if (getApp()->getAccounts()->twitch.isLoggedIn())
        {
            this->twitchChannel_->sendMessage(message);
            sent = true;
        }
        else
        {
            unavailablePlatforms.append("Twitch");
        }
    }

    if (this->config_.kickEnabled && this->kickChannel_)
    {
        if (getApp()->getAccounts()->kick.isLoggedIn())
        {
            this->kickChannel_->sendMessage(message);
            sent = true;
        }
        else
        {
            unavailablePlatforms.append("Kick");
        }
    }

    if (!sent && !unavailablePlatforms.isEmpty())
    {
        this->addSystemStatusMessage(
            QString("Log in to %1 to send merged chat.")
                .arg(unavailablePlatforms.join(" or ")));
    }
}

bool MergedChannel::isMod() const
{
    return (this->twitchChannel_ && this->twitchChannel_->isMod()) ||
           (this->kickChannel_ && this->kickChannel_->isMod());
}

bool MergedChannel::isBroadcaster() const
{
    return (this->twitchChannel_ && this->twitchChannel_->isBroadcaster()) ||
           (this->kickChannel_ && this->kickChannel_->isBroadcaster());
}

bool MergedChannel::hasModRights() const
{
    return (this->twitchChannel_ && this->twitchChannel_->hasModRights()) ||
           (this->kickChannel_ && this->kickChannel_->hasModRights());
}

bool MergedChannel::isLive() const
{
    return this->twitchLive_ || this->kickLive_ || this->youtubeLive_;
}

bool MergedChannel::isRerun() const
{
    if (auto *twitch = dynamic_cast<TwitchChannel *>(this->twitchChannel_.get()))
    {
        return twitch->isRerun();
    }
    return false;
}

QString MergedChannel::getCurrentStreamID() const
{
    if (this->youtubeLiveChat_ && !this->youtubeLiveChat_->videoId().isEmpty())
    {
        return this->youtubeLiveChat_->videoId();
    }
    if (this->twitchChannel_)
    {
        auto id = this->twitchChannel_->getCurrentStreamID();
        if (!id.isEmpty())
        {
            return id;
        }
    }
    if (this->kickChannel_)
    {
        return this->kickChannel_->getCurrentStreamID();
    }
    return {};
}

QString MergedChannel::statusSuffix() const
{
    if (!this->isLive())
    {
        return {};
    }

    QStringList livePlatforms;
    if (this->twitchLive_)
    {
        livePlatforms.append("Twitch");
    }
    if (this->kickLive_)
    {
        livePlatforms.append("Kick");
    }
    if (this->youtubeLive_)
    {
        livePlatforms.append("YouTube");
    }

    if (livePlatforms.isEmpty())
    {
        return " (live)";
    }

    return QString(" (%1 live)").arg(livePlatforms.join(" + "));
}

QString MergedChannel::tooltipText() const
{
    return this->tooltipText_;
}

ChannelPtr MergedChannel::twitchChannel() const
{
    return this->twitchChannel_;
}

ChannelPtr MergedChannel::kickChannel() const
{
    return this->kickChannel_;
}

void MergedChannel::initializeSources()
{
    if (this->config_.twitchEnabled)
    {
        const auto twitchName = this->config_.effectiveTwitchChannelName();
        if (!twitchName.isEmpty())
        {
            this->twitchChannel_ =
                getApp()->getTwitch()->getOrAddChannel(twitchName);
            this->connectSourceSignals(this->twitchChannel_,
                                       MessagePlatform::AnyOrTwitch,
                                       this->twitchConnections_);
            this->appendInitialMessages(this->twitchChannel_,
                                        MessagePlatform::AnyOrTwitch);
            this->twitchLive_ = this->twitchChannel_->isLive();
            if (this->twitchLive_)
            {
                if (auto *twitch =
                        dynamic_cast<TwitchChannel *>(this->twitchChannel_.get()))
                {
                    this->announceJoinedLiveChat(
                        MessagePlatform::AnyOrTwitch,
                        twitch->accessStreamStatus()->title);
                }
            }
        }
    }

    if (this->config_.kickEnabled)
    {
        const auto kickName = this->config_.effectiveKickChannelName();
        if (!kickName.isEmpty())
        {
            this->kickChannel_ =
                getApp()->getKickChatServer()->getOrCreate(kickName);
            this->connectSourceSignals(this->kickChannel_, MessagePlatform::Kick,
                                       this->kickConnections_);
            this->appendInitialMessages(this->kickChannel_, MessagePlatform::Kick);
            this->kickLive_ = this->kickChannel_->isLive();
            if (this->kickLive_)
            {
                if (auto *kick =
                        dynamic_cast<KickChannel *>(this->kickChannel_.get()))
                {
                    this->announceJoinedLiveChat(MessagePlatform::Kick,
                                                 kick->streamData().title);
                }
            }
        }
    }

    if (this->config_.youtubeEnabled &&
        !this->config_.youtubeStreamUrl.trimmed().isEmpty())
    {
        this->youtubeLiveChat_ =
            std::make_unique<YouTubeLiveChat>(this->config_.youtubeStreamUrl);
        this->youtubeConnections_.managedConnect(
            this->youtubeLiveChat_->messageReceived,
            [this](const MessagePtr &message) { this->addYouTubeMessage(message); });
        this->youtubeConnections_.managedConnect(
            this->youtubeLiveChat_->sourceResolved, [this](const QString &source) {
                if (!source.isEmpty())
                {
                    this->config_.youtubeStreamUrl = source;
                }
            });
        this->youtubeConnections_.managedConnect(
            this->youtubeLiveChat_->systemMessageReceived, [this](const MessagePtr &message) {
                this->addSystemStatusMessage(message);
                this->refreshStatusText();
            });
        this->youtubeConnections_.managedConnect(
            this->youtubeLiveChat_->liveStatusChanged, [this] {
                this->youtubeLive_ = this->youtubeLiveChat_->isLive();
                this->refreshStatusText();
                this->streamStatusChanged.invoke();
            });
        this->youtubeLiveChat_->start();
    }
}

void MergedChannel::connectSourceSignals(
    const ChannelPtr &source, MessagePlatform platform,
    pajlada::Signals::SignalHolder &connections)
{
    if (!source)
    {
        return;
    }

    connections.managedConnect(
        source->messageAppended,
        [this, platform](MessagePtr &message, std::optional<MessageFlags>) {
            this->appendMergedMessage(message, platform);
        });
    connections.managedConnect(
        source->messageReplaced,
        [this, platform](size_t, const MessagePtr &previous,
                         const MessagePtr &replacement) {
            this->replaceMergedMessage(previous, replacement, platform);
        });
    connections.managedConnect(source->messagesCleared, [this, platform] {
        this->clearMirrorsForPlatform(platform);
    });

    if (auto *twitch = dynamic_cast<TwitchChannel *>(source.get()))
    {
        connections.managedConnect(twitch->streamStatusChanged, [this, twitch] {
            this->twitchLive_ = twitch->isLive();
            if (this->twitchLive_)
            {
                if (!this->twitchLiveJoinAnnounced_)
                {
                    this->announceJoinedLiveChat(
                        MessagePlatform::AnyOrTwitch,
                        twitch->accessStreamStatus()->title);
                }
            }
            else
            {
                this->twitchLiveJoinAnnounced_ = false;
            }
            this->refreshStatusText();
            this->streamStatusChanged.invoke();
        });
    }
    else if (auto *kick = dynamic_cast<KickChannel *>(source.get()))
    {
        connections.managedConnect(kick->liveStatusChanged, [this, kick] {
            this->kickLive_ = kick->isLive();
            if (this->kickLive_)
            {
                if (!this->kickLiveJoinAnnounced_)
                {
                    this->announceJoinedLiveChat(MessagePlatform::Kick,
                                                 kick->streamData().title);
                }
            }
            else
            {
                this->kickLiveJoinAnnounced_ = false;
            }
            this->refreshStatusText();
            this->streamStatusChanged.invoke();
        });
    }
}

void MergedChannel::appendInitialMessages(const ChannelPtr &source,
                                          MessagePlatform platform)
{
    if (!source)
    {
        return;
    }

    const auto snapshot = source->getMessageSnapshot(150);
    for (const auto &message : snapshot)
    {
        this->appendMergedMessage(message, platform);
    }
}

void MergedChannel::appendMergedMessage(const MessagePtr &source,
                                        MessagePlatform platform)
{
    if (!shouldMirrorSourceMessage(source))
    {
        return;
    }

    const auto key = messageKey(source, platform);
    if (!key.isEmpty() && this->mirroredMessages_.contains(key))
    {
        return;
    }

    auto merged = this->createMergedMessage(source, platform);
    if (!merged)
    {
        return;
    }

    if (!key.isEmpty())
    {
        this->insertMirror(key, merged);
    }

    const auto chatterName =
        !merged->loginName.isEmpty() ? merged->loginName : merged->displayName;
    if (!chatterName.isEmpty())
    {
        this->addRecentChatter(chatterName);
    }

    this->addMessage(merged, MessageContext::Repost);
}

void MergedChannel::replaceMergedMessage(const MessagePtr &previous,
                                         const MessagePtr &replacement,
                                         MessagePlatform platform)
{
    const auto key = messageKey(previous, platform);
    if (key.isEmpty())
    {
        return;
    }

    auto it = this->mirroredMessages_.find(key);
    if (it == this->mirroredMessages_.end())
    {
        return;
    }

    auto updated = this->createMergedMessage(replacement, platform);
    if (!updated)
    {
        return;
    }

    this->replaceMessage(it->second, updated);
    const auto updatedKey = messageKey(replacement, platform);
    if (!updatedKey.isEmpty() && updatedKey != key)
    {
        this->eraseMirror(key);
        this->insertMirror(updatedKey, updated);
        return;
    }

    it->second = updated;
}

std::shared_ptr<Message> MergedChannel::createMergedMessage(
    const MessagePtr &source, MessagePlatform platform) const
{
    if (!source)
    {
        return nullptr;
    }

    auto merged = source->clone();
    merged->platform = platform;
    merged->platformAccentColor = platformAccent(platform);

    if (platform == MessagePlatform::AnyOrTwitch)
    {
        merged->channelName = this->config_.effectiveTwitchChannelName();
    }
    else if (platform == MessagePlatform::Kick)
    {
        merged->channelName = this->config_.effectiveKickChannelName();
    }

    merged->searchText =
        platformName(platform) + " " + merged->searchText.trimmed();

    const auto badge = platformBadge(platform);
    if (badge)
    {
        merged->elements.insert(
            merged->elements.begin(),
            std::make_unique<BadgeElement>(badge,
                                           MessageElementFlag::PlatformBadge));
    }

    return merged;
}

void MergedChannel::addYouTubeMessage(const MessagePtr &message)
{
    const auto key = messageKey(message, MessagePlatform::YouTube);
    if (!key.isEmpty() && this->mirroredMessages_.contains(key))
    {
        return;
    }

    auto merged = this->createMergedMessage(message, MessagePlatform::YouTube);
    if (!merged)
    {
        return;
    }

    if (!key.isEmpty())
    {
        this->insertMirror(key, merged);
    }

    const auto chatterName =
        !merged->loginName.isEmpty() ? merged->loginName : merged->displayName;
    if (!chatterName.isEmpty())
    {
        this->addRecentChatter(chatterName);
    }

    this->addMessage(merged, MessageContext::Repost);
}

bool MergedChannel::shouldMirrorSourceMessage(const MessagePtr &message)
{
    if (!message)
    {
        return false;
    }

    if (message->flags.has(MessageFlag::ConnectedMessage))
    {
        return false;
    }

    if (!message->flags.has(MessageFlag::System))
    {
        return true;
    }

    const auto text = message->messageText.trimmed().toLower();
    if (text.contains(" is live!") || text.contains(" is live:"))
    {
        return false;
    }

    return text != "joined" && text != "joined channel" &&
           text != "connected" && text != "reconnected";
}

void MergedChannel::announceJoinedLiveChat(MessagePlatform platform,
                                           const QString &title)
{
    auto message = makeSystemMessage(
                       title.trimmed().isEmpty()
                           ? QString("Joined %1 live chat")
                                 .arg(platformName(platform))
                           : QString("Joined %1 live chat: %2")
                                 .arg(platformName(platform), title.trimmed()))
                       ->clone();
    message->platform = platform;
    message->platformAccentColor = platformAccent(platform);

    if (platform == MessagePlatform::Kick)
    {
        this->kickLiveJoinAnnounced_ = true;
    }
    else if (platform == MessagePlatform::AnyOrTwitch)
    {
        this->twitchLiveJoinAnnounced_ = true;
    }

    this->addSystemStatusMessage(message);
}

void MergedChannel::addSystemStatusMessage(const QString &message)
{
    if (message.isEmpty())
    {
        return;
    }

    this->addMessage(makeSystemMessage(message), MessageContext::Repost);
}

void MergedChannel::addSystemStatusMessage(const MessagePtr &message)
{
    if (!message)
    {
        return;
    }

    this->addMessage(message, MessageContext::Repost);
}

void MergedChannel::refreshStatusText()
{
    QStringList lines;

    if (this->config_.twitchEnabled)
    {
        lines.append(QString("Twitch: %1 (%2)")
                         .arg(this->config_.effectiveTwitchChannelName(),
                              this->twitchLive_ ? "live" : "offline"));
    }
    if (this->config_.kickEnabled)
    {
        lines.append(QString("Kick: %1 (%2)")
                         .arg(this->config_.effectiveKickChannelName(),
                              this->kickLive_ ? "live" : "offline"));
    }
    if (this->config_.youtubeEnabled)
    {
        QString youtubeStatus =
            this->youtubeLive_ ? QStringLiteral("live")
                               : QStringLiteral("waiting for live chat");
        if (this->youtubeLiveChat_ &&
            !this->youtubeLiveChat_->statusText().trimmed().isEmpty())
        {
            youtubeStatus = this->youtubeLiveChat_->statusText();
        }
        lines.append(QString("YouTube: %1").arg(youtubeStatus));
    }

    this->tooltipText_ = lines.join("<br>");
}

QColor MergedChannel::platformAccent(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QColor(83, 252, 24, 36);
        case MessagePlatform::YouTube:
            return QColor(255, 48, 64, 60);
        case MessagePlatform::AnyOrTwitch:
        default:
            return QColor(145, 70, 255, 36);
    }
}

EmotePtr MergedChannel::platformBadge(MessagePlatform platform)
{
    static EmoteMap cache;

    const auto name = EmoteName{platformName(platform).toLower()};
    auto it = cache.find(name);
    if (it != cache.end())
    {
        return it->second;
    }

    Emote emote{
        .name = name,
        .images = ImageSet{Image::fromResourcePixmap(renderPlatformBadge(platform))},
        .tooltip = Tooltip{platformName(platform)},
        .homePage = Url{},
    };
    auto badge = cachedOrMakeEmotePtr(std::move(emote), cache);
    cache[badge->name] = badge;
    return badge;
}

QString MergedChannel::messageKey(const MessagePtr &message,
                                  MessagePlatform platform)
{
    if (!message)
    {
        return {};
    }

    auto key = message->id;
    if (key.isEmpty())
    {
        key = QString("%1|%2|%3|%4")
                  .arg(message->channelName, message->loginName,
                       message->messageText,
                       message->serverReceivedTime.toString(Qt::ISODateWithMs));
    }

    return QString("%1|%2").arg(static_cast<int>(platform)).arg(key);
}

void MergedChannel::insertMirror(const QString &key, const MessagePtr &merged)
{
    const auto inserted =
        this->mirroredMessages_.insert_or_assign(key, merged).second;
    if (!inserted)
    {
        return;
    }
    this->mirroredOrder_.push_back(key);
    while (this->mirroredOrder_.size() > MAX_MIRRORED_MESSAGES)
    {
        this->mirroredMessages_.erase(this->mirroredOrder_.front());
        this->mirroredOrder_.pop_front();
    }
}

void MergedChannel::eraseMirror(const QString &key)
{
    if (this->mirroredMessages_.erase(key) == 0)
    {
        return;
    }
    const auto it =
        std::find(this->mirroredOrder_.begin(), this->mirroredOrder_.end(), key);
    if (it != this->mirroredOrder_.end())
    {
        this->mirroredOrder_.erase(it);
    }
}

void MergedChannel::clearMirrorsForPlatform(MessagePlatform platform)
{
    const auto prefix = QString::number(static_cast<int>(platform)) + u'|';
    std::erase_if(this->mirroredMessages_, [&prefix](const auto &entry) {
        return entry.first.startsWith(prefix);
    });
    std::erase_if(this->mirroredOrder_, [&prefix](const QString &key) {
        return key.startsWith(prefix);
    });
}

}  // namespace chatterino
