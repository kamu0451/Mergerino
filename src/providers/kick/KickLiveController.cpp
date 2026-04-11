#include "providers/kick/KickLiveController.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/kick/KickApi.hpp"
#include "providers/kick/KickChatServer.hpp"

#include <QPointer>

#include <chrono>

namespace {

using namespace std::chrono_literals;

constexpr auto IMMEDIATE_DELAY = 1s;
constexpr auto REFRESH_INTERVAL = 1min;

}  // namespace

namespace chatterino {

KickLiveController::KickLiveController(KickChatServer &chatServer)
    : chatServer(chatServer)
{
    this->immediateTimer.setInterval(IMMEDIATE_DELAY);
    this->immediateTimer.setSingleShot(true);
    QObject::connect(&this->immediateTimer, &QTimer::timeout, this,
                     &KickLiveController::refreshImmediate);

    this->refreshTimer.setInterval(REFRESH_INTERVAL);
    this->refreshTimer.setSingleShot(false);
    this->refreshTimer.start();
    QObject::connect(&this->refreshTimer, &QTimer::timeout, this,
                     &KickLiveController::refreshAll);
}

KickLiveController::~KickLiveController() = default;

void KickLiveController::queueNewChannel(uint64_t userID)
{
    this->immediateChannels.emplace_back(userID);
    if (!this->immediateTimer.isActive())
    {
        this->immediateTimer.start();
    }
}

void KickLiveController::refreshImmediate()
{
    if (this->immediateChannels.empty())
    {
        return;
    }
    this->refreshList(this->immediateChannels);
    this->immediateChannels.clear();
}

void KickLiveController::refreshAll()
{
    std::vector<uint64_t> userIDs;
    for (const auto &[channelID, weak] : this->chatServer.channelMap())
    {
        auto chan = weak.lock();
        if (chan && chan->userID() != 0)
        {
            userIDs.emplace_back(chan->userID());
        }
    }
    this->refreshList(userIDs);
}

void KickLiveController::refreshList(const std::span<uint64_t> userIDs)
{
    for (const auto userID : userIDs)
    {
        auto chan = this->chatServer.findByUserID(userID);
        if (!chan)
        {
            continue;
        }

        KickApi::privateChannelInfo(
            chan->slug(),
            [self = QPointer(this), weak = chan->weakFromThis()](
                const ExpectedStr<KickPrivateChannelInfo> &res) {
                if (!self)
                {
                    return;
                }

                auto chan = weak.lock();
                if (!chan)
                {
                    return;
                }

                if (!res)
                {
                    qCDebug(chatterinoKick)
                        << "Failed to refresh channel" << chan->slug()
                        << res.error();
                    return;
                }

                chan->updateStreamData(*res);
            });
    }
}

}  // namespace chatterino
