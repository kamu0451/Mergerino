// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/ChatterListWidget.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"  // IWYU pragma: keep
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "util/Helpers.hpp"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPixmap>
#include <QSize>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>
#include <memory>

namespace chatterino {

namespace {

QString formatVIPListError(HelixListVIPsError error, const QString &message)
{
    using Error = HelixListVIPsError;

    QString errorMessage = QString("Failed to list VIPs - ");

    switch (error)
    {
        case Error::Forwarded: {
            errorMessage += message;
        }
        break;

        case Error::Ratelimited: {
            errorMessage += "You are being ratelimited by Twitch. Try "
                            "again in a few seconds.";
        }
        break;

        case Error::UserMissingScope: {
            // TODO(pajlada): Phrase MISSING_REQUIRED_SCOPE
            errorMessage += "Missing required scope. "
                            "Re-login with your "
                            "account and try again.";
        }
        break;

        case Error::UserNotAuthorized: {
            // TODO(pajlada): Phrase MISSING_PERMISSION
            errorMessage += "You don't have permission to "
                            "perform that action.";
        }
        break;

        case Error::UserNotBroadcaster: {
            errorMessage +=
                "Due to Twitch restrictions, "
                "this command can only be used by the broadcaster. "
                "To see the list of VIPs you must use the Twitch website.";
        }
        break;

        case Error::Unknown: {
            errorMessage += "An unknown error has occurred.";
        }
        break;
    }
    return errorMessage;
}

QString formatModsError(HelixGetModeratorsError error, const QString &message)
{
    using Error = HelixGetModeratorsError;

    QString errorMessage = QString("Failed to get moderators: ");

    switch (error)
    {
        case Error::Forwarded: {
            errorMessage += message;
        }
        break;

        case Error::UserMissingScope: {
            errorMessage += "Missing required scope. "
                            "Re-login with your "
                            "account and try again.";
        }
        break;

        case Error::UserNotAuthorized: {
            errorMessage +=
                "Due to Twitch restrictions, "
                "this command can only be used by the broadcaster. "
                "To see the list of mods you must use the Twitch website.";
        }
        break;

        case Error::Unknown: {
            errorMessage += "An unknown error has occurred.";
        }
        break;
    }
    return errorMessage;
}

QString formatChattersError(HelixGetChattersError error, const QString &message)
{
    using Error = HelixGetChattersError;

    QString errorMessage = QString("Failed to get viewers: ");

    switch (error)
    {
        case Error::Forwarded: {
            errorMessage += message;
        }
        break;

        case Error::UserMissingScope: {
            errorMessage += "Missing required scope. "
                            "Re-login with your "
                            "account and try again.";
        }
        break;

        case Error::UserNotAuthorized: {
            errorMessage +=
                "Due to Twitch restrictions, "
                "this command can only be used by moderators. "
                "To see the viewer list you must use the Twitch website.";
        }
        break;

        case Error::Unknown: {
            errorMessage += "An unknown error has occurred.";
        }
        break;
    }
    return errorMessage;
}

}  // namespace

ChatterListWidget::ChatterListWidget(const TwitchChannel *twitchChannel,
                                     QWidget *parent)
    : BaseWindow({}, parent)
{
    assert(twitchChannel != nullptr);
    this->setWindowTitle("Viewer List - " + twitchChannel->getName());

    const auto roomId = twitchChannel->roomId();
    const auto broadcasterName = twitchChannel->getName().toLower();
    const auto isBroadcaster = twitchChannel->isBroadcaster();
    const auto hasModRights = twitchChannel->hasModRights();
    const auto currentUserId =
        getApp()->getAccounts()->twitch.getCurrent()->getUserId();

    this->setAttribute(Qt::WA_DeleteOnClose);

    auto *dockVbox = new QVBoxLayout();
    dockVbox->setContentsMargins(0, 0, 0, 0);
    auto *searchBar = new QLineEdit(this);
    auto *searchTerms = new QWidget(this);
    auto *searchTermsLayout = new QVBoxLayout(searchTerms);
    auto searchQueries = std::make_shared<QStringList>();

    auto *chattersList = new QListWidget();
    auto *resultList = new QListWidget();
    searchBar->installEventFilter(this);
    chattersList->installEventFilter(this);
    resultList->installEventFilter(this);

    auto *loadingLabel = new QLabel("Loading...");
    const QPointer<ChatterListWidget> lifetimeGuard(this);
    const QPointer<QListWidget> chattersListGuard(chattersList);
    const QPointer<QListWidget> resultListGuard(resultList);
    const QPointer<QLineEdit> searchBarGuard(searchBar);
    const QPointer<QLabel> loadingLabelGuard(loadingLabel);
    searchBar->setPlaceholderText("Search viewer...");
    searchTerms->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    searchTermsLayout->setContentsMargins(0, 0, 0, 0);
    searchTermsLayout->setSpacing(0);
    searchTerms->hide();

    auto formatListItemText = [](const QString &text) {
        auto *item = new QListWidgetItem();
        item->setText(text);
        item->setFont(
            getApp()->getFonts()->getFont(FontStyle::ChatMedium, 1.0));
        return item;
    };

    auto addLabel = [this, formatListItemText,
                     chattersList](const QString &label) {
        auto *formattedLabel = formatListItemText(label);
        formattedLabel->setFlags(Qt::NoItemFlags);
        formattedLabel->setForeground(this->theme->accent);
        chattersList->addItem(formattedLabel);
    };

    auto addUserList = [=](const QStringList &users, QString label) {
        if (users.isEmpty())
        {
            return;
        }

        addLabel(QString("%1 (%2)").arg(label, localizeNumbers(users.size())));

        for (const auto &user : users)
        {
            chattersList->addItem(formatListItemText(user));
        }
        chattersList->addItem(new QListWidgetItem());
    };

    auto activeSearchQueries = [=]() {
        QStringList queries = *searchQueries;
        const auto current = searchBar->text().trimmed();
        if (!current.isEmpty())
        {
            queries.append(current);
        }
        return queries;
    };

    auto performListSearch = [=]() {
        auto queries = activeSearchQueries();
        if (queries.isEmpty())
        {
            resultList->hide();
            chattersList->show();
            return;
        }

        chattersList->hide();
        resultList->clear();
        QSet<QString> addedUsers;
        for (const auto &query : queries)
        {
            auto results = chattersList->findItems(query, Qt::MatchContains);
            for (auto &item : results)
            {
                const auto text = item->text();
                if (!text.contains("(") && !addedUsers.contains(text))
                {
                    addedUsers.insert(text);
                    resultList->addItem(formatListItemText(text));
                }
            }
        }
        resultList->show();
    };

    auto rebuildSearchTerms = std::make_shared<std::function<void()>>();
    *rebuildSearchTerms = [=]() {
        while (auto *item = searchTermsLayout->takeAt(0))
        {
            if (auto *widget = item->widget())
            {
                widget->deleteLater();
            }
            delete item;
        }

        for (int i = 0; i < searchQueries->size(); ++i)
        {
            auto *row = new QWidget(searchTerms);
            row->setStyleSheet(QStringLiteral(
                "QWidget { background: rgba(255, 255, 255, 10); }"));
            row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            row->setFixedHeight(22);

            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(8, 0, 4, 0);
            rowLayout->setSpacing(6);

            auto *label = new QLabel(searchQueries->at(i), row);
            label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            rowLayout->addWidget(label);

            auto *removeButton = new QToolButton(row);
            removeButton->setAutoRaise(true);
            removeButton->setCursor(Qt::PointingHandCursor);
            removeButton->setIcon(
                QPixmap(QStringLiteral(":/buttons/trash-lightMode.svg")));
            removeButton->setIconSize(QSize(14, 14));
            removeButton->setFixedSize(20, 20);
            removeButton->setToolTip(QStringLiteral("Remove search term"));
            rowLayout->addWidget(removeButton);

            QObject::connect(removeButton, &QToolButton::clicked, this,
                             [=] {
                                 const int index = searchTermsLayout->indexOf(row);
                                 if (index >= 0 && index < searchQueries->size())
                                 {
                                     searchQueries->removeAt(index);
                                     (*rebuildSearchTerms)();
                                     performListSearch();
                                 }
                             });

            searchTermsLayout->addWidget(row);
        }

        searchTerms->setVisible(!searchQueries->isEmpty());
        searchTerms->adjustSize();
    };

    auto addSearchTerm = [=]() {
        const auto query = searchBar->text().trimmed();
        if (query.isEmpty())
        {
            return;
        }

        for (const auto &existing : *searchQueries)
        {
            if (existing.compare(query, Qt::CaseInsensitive) == 0)
            {
                searchBar->clear();
                performListSearch();
                return;
            }
        }

        searchQueries->append(query);
        searchBar->clear();
        (*rebuildSearchTerms)();
        performListSearch();
    };

    auto loadChatters = [=](auto modList, auto vipList, bool isBroadcaster) {
        getHelix()->getChatters(
            roomId, currentUserId, 50000,
            [=](const auto &chatters) {
                if (lifetimeGuard.isNull() || chattersListGuard.isNull() ||
                    resultListGuard.isNull() || searchBarGuard.isNull() ||
                    loadingLabelGuard.isNull())
                {
                    return;
                }

                auto broadcaster = broadcasterName;
                QStringList chatterList;
                QStringList modChatters;
                QStringList vipChatters;

                bool addedBroadcaster = false;
                for (auto chatter : chatters.chatters)
                {
                    chatter = chatter.toLower();

                    if (!addedBroadcaster && chatter == broadcaster)
                    {
                        addedBroadcaster = true;
                        addLabel("Broadcaster");
                        chattersList->addItem(broadcaster);
                        chattersList->addItem(new QListWidgetItem());
                        continue;
                    }

                    if (modList.contains(chatter))
                    {
                        modChatters.append(chatter);
                        continue;
                    }

                    if (vipList.contains(chatter))
                    {
                        vipChatters.append(chatter);
                        continue;
                    }

                    chatterList.append(chatter);
                }

                modChatters.sort();
                vipChatters.sort();
                chatterList.sort();

                if (isBroadcaster)
                {
                    addUserList(modChatters, QString("Moderators"));
                    addUserList(vipChatters, QString("VIPs"));
                }

                addUserList(chatterList, QString("Viewers"));

                loadingLabel->hide();
                performListSearch();
            },
            [=](auto error, const auto &message) {
                if (lifetimeGuard.isNull() || chattersListGuard.isNull())
                {
                    return;
                }

                auto errorMessage = formatChattersError(error, message);
                chattersList->addItem(formatListItemText(errorMessage));
            });
    };

    QObject::connect(searchBar, &QLineEdit::textEdited, this,
                     performListSearch);
    QObject::connect(searchBar, &QLineEdit::returnPressed, this, addSearchTerm);

    // Only broadcaster can get role lists. Moderators can get viewers.
    if (isBroadcaster)
    {
        // Add moderators
        getHelix()->getModerators(
            roomId, 1000,
            [=](const auto &mods) {
                if (lifetimeGuard.isNull() || chattersListGuard.isNull())
                {
                    return;
                }

                QSet<QString> modList;
                for (const auto &mod : mods)
                {
                    modList.insert(mod.userName.toLower());
                }

                // Add vips
                getHelix()->getChannelVIPs(
                    roomId,
                    [=](const auto &vips) {
                        if (lifetimeGuard.isNull() ||
                            chattersListGuard.isNull())
                        {
                            return;
                        }

                        QSet<QString> vipList;
                        for (const auto &vip : vips)
                        {
                            vipList.insert(vip.userName.toLower());
                        }

                        // Add viewers
                        loadChatters(modList, vipList, true);
                    },
                    [=](auto error, const auto &message) {
                        if (lifetimeGuard.isNull() ||
                            chattersListGuard.isNull())
                        {
                            return;
                        }

                        auto errorMessage = formatVIPListError(error, message);
                        chattersList->addItem(formatListItemText(errorMessage));
                    });
            },
            [=](auto error, const auto &message) {
                if (lifetimeGuard.isNull() || chattersListGuard.isNull())
                {
                    return;
                }

                auto errorMessage = formatModsError(error, message);
                chattersList->addItem(formatListItemText(errorMessage));
            });
    }
    else if (hasModRights)
    {
        QSet<QString> modList;
        QSet<QString> vipList;
        loadChatters(modList, vipList, false);
    }
    else
    {
        chattersList->addItem(
            formatListItemText("Due to Twitch restrictions, this feature is "
                               "only \navailable for moderators."));
        chattersList->addItem(
            formatListItemText("If you would like to see the viewer list, you "
                               "must \nuse the Twitch website."));
        loadingLabel->hide();
    }

    this->setMinimumWidth(300);

    auto listDoubleClick = [this](const QModelIndex &index) {
        const auto itemText = index.data().toString();

        if (!itemText.isEmpty())
        {
            this->userClicked(itemText);
        }
    };

    QObject::connect(chattersList, &QListWidget::doubleClicked, this,
                     listDoubleClick);

    QObject::connect(resultList, &QListWidget::doubleClicked, this,
                     listDoubleClick);

    HotkeyController::HotkeyMap actions{
        {"delete",
         [this](const std::vector<QString> &) -> QString {
             this->close();
             return "";
         }},
        {"accept", nullptr},
        {"reject", nullptr},
        {"scrollPage", nullptr},
        {"openTab", nullptr},
        {"search",
         [searchBar](const std::vector<QString> &) -> QString {
             searchBar->setFocus();
             searchBar->selectAll();
             return "";
         }},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::PopupWindow, actions, this);

    dockVbox->addWidget(searchBar);
    dockVbox->addWidget(searchTerms);
    dockVbox->addWidget(loadingLabel);
    dockVbox->addWidget(chattersList);
    dockVbox->addWidget(resultList);
    resultList->hide();

    this->setStyleSheet(this->theme->splits.input.styleSheet);
    this->setLayout(dockVbox);
}

bool ChatterListWidget::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::KeyPress)
    {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Escape)
        {
            keyEvent->accept();
            this->close();
            return true;
        }
    }

    return BaseWindow::eventFilter(object, event);
}

}  // namespace chatterino
