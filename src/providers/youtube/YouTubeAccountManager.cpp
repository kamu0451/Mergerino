#include "providers/youtube/YouTubeAccountManager.hpp"

#include "common/QLogging.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "util/RapidJsonSerializeQString.hpp"  // IWYU pragma: keep
#include "util/SharedPtrElementLess.hpp"

#include <pajlada/settings/setting.hpp>

namespace chatterino {

YouTubeAccountManager::YouTubeAccountManager()
    : accounts(SharedPtrElementLess<YouTubeAccount>{})
    , anonymousUser_(std::make_shared<YouTubeAccount>(YouTubeAccountData{}))
{
    std::ignore = this->accounts.itemRemoved.connect([this](const auto &acc) {
        this->removeAccount(acc.item.get());
    });

    this->refreshTimer_.setSingleShot(false);
    this->refreshTimer_.setInterval(YouTubeAccount::CHECK_REFRESH_INTERVAL);
    QObject::connect(&this->refreshTimer_, &QTimer::timeout, [this] {
        this->refreshAccounts();
    });
    this->refreshTimer_.start();
}

std::shared_ptr<YouTubeAccount> YouTubeAccountManager::current()
{
    if (!this->currentUser_)
    {
        return this->anonymousUser_;
    }
    return this->currentUser_;
}

std::vector<QString> YouTubeAccountManager::channelNames() const
{
    std::vector<QString> names;
    for (const auto &acc : this->accounts.raw())
    {
        names.emplace_back(acc->displayName());
    }
    return names;
}

std::shared_ptr<YouTubeAccount> YouTubeAccountManager::findUserByChannelID(
    const QString &channelID) const
{
    for (const auto &acc : this->accounts.raw())
    {
        if (acc->channelID() == channelID)
        {
            return acc;
        }
    }
    return nullptr;
}

bool YouTubeAccountManager::userExists(const QString &channelID) const
{
    return this->findUserByChannelID(channelID) != nullptr;
}

bool YouTubeAccountManager::isLoggedIn() const
{
    return this->currentUser_ && !this->currentUser_->isAnonymous();
}

void YouTubeAccountManager::reloadUsers()
{
    auto keys =
        pajlada::Settings::SettingManager::getObjectKeys("/youtubeAccounts");

    bool listUpdated = false;

    for (const auto &key : keys)
    {
        if (key == "current")
        {
            continue;
        }

        auto data = YouTubeAccountData::loadRaw(key);
        if (!data)
        {
            continue;
        }

        switch (this->addAccount(*data))
        {
            case AddUserResponse::UserAlreadyExists: {
                qCDebug(chatterinoYouTube)
                    << "User" << data->displayName << "already exists";
            }
            break;
            case AddUserResponse::UserUpdated: {
                qCDebug(chatterinoYouTube)
                    << "User" << data->displayName << "updated";
                if (data->channelID == this->current()->channelID())
                {
                    this->currentUserChanged.invoke();
                }
            }
            break;
            case AddUserResponse::UserAdded: {
                qCDebug(chatterinoYouTube)
                    << "Added account" << data->displayName;
                listUpdated = true;
            }
            break;
        }
    }

    if (listUpdated)
    {
        this->userListUpdated.invoke();
        this->refreshAccounts();
    }
}

void YouTubeAccountManager::load()
{
    this->reloadUsers();

    this->currentChannelID.connect([this](const QString &newChannelID) {
        auto user = this->findUserByChannelID(newChannelID);
        if (user)
        {
            qCDebug(chatterinoYouTube)
                << "YouTube user updated to" << user->displayName();
            this->currentUser_ = user;
        }
        else
        {
            qCDebug(chatterinoYouTube)
                << "YouTube user updated to anonymous";
            this->currentUser_ = this->anonymousUser_;
        }

        this->currentUserChanged.invoke();
    });
}

YouTubeAccountManager::AddUserResponse YouTubeAccountManager::addAccount(
    const YouTubeAccountData &data)
{
    auto previousUser = this->findUserByChannelID(data.channelID);
    if (previousUser)
    {
        bool userUpdated = previousUser->update(data);
        if (userUpdated)
        {
            return AddUserResponse::UserUpdated;
        }

        return AddUserResponse::UserAlreadyExists;
    }

    auto account = std::make_shared<YouTubeAccount>(data);
    this->accounts.insert(account);
    this->holder_.managedConnect(account->authUpdated, [account] {
        qCDebug(chatterinoYouTube)
            << "YouTube auth updated for" << account->displayName();
    });

    return AddUserResponse::UserAdded;
}

bool YouTubeAccountManager::removeAccount(YouTubeAccount *account)
{
    if (account->isAnonymous())
    {
        return false;
    }

    const auto accountPath =
        "/youtubeAccounts/channel" + account->channelID().toStdString();
    pajlada::Settings::SettingManager::gRemoveSetting(accountPath);

    if (account->channelID() == this->currentChannelID)
    {
        this->currentChannelID = "";
    }

    this->userListUpdated.invoke();
    return true;
}

void YouTubeAccountManager::refreshAccounts() const
{
    for (const auto &acc : this->accounts.raw())
    {
        acc->refreshIfNeeded();
    }
}

}  // namespace chatterino
