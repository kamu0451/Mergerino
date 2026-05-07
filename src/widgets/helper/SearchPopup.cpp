// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/SearchPopup.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/filters/FilterSet.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "messages/MessageElement.hpp"
#include "messages/search/AuthorPredicate.hpp"
#include "messages/search/BadgePredicate.hpp"
#include "messages/search/ChannelPredicate.hpp"
#include "messages/search/LinkPredicate.hpp"
#include "messages/search/MessageFlagsPredicate.hpp"
#include "messages/search/RegexPredicate.hpp"
#include "messages/search/SubstringPredicate.hpp"
#include "messages/search/SubtierPredicate.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/splits/Split.hpp"

#include <QAbstractButton>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSize>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

namespace chatterino {

ChannelPtr SearchPopup::filter(const QString &text, const QString &channelName,
                               const std::vector<MessagePtr> &snapshot)
{
    ChannelPtr channel(new Channel(channelName, Channel::Type::None));

    // Parse predicates from tags in "text"
    auto predicates = parsePredicates(text);

    // Check for every message whether it fulfills all predicates that have
    // been registered
    for (size_t i = 0; i < snapshot.size(); ++i)
    {
        MessagePtr message = snapshot[i];
        if (!message)
        {
            continue;
        }

        bool accept = true;
        for (const auto &pred : predicates)
        {
            // Discard the message as soon as one predicate fails
            if (!pred->appliesTo(*message))
            {
                accept = false;
                break;
            }
        }

        // If all predicates match, add the message to the channel
        if (accept)
        {
            auto overrideFlags = std::optional<MessageFlags>(message->flags);
            overrideFlags->set(MessageFlag::DoNotLog);

            channel->addMessage(message, MessageContext::Repost, overrideFlags);
        }
    }

    return channel;
}

ChannelPtr SearchPopup::filter(const QStringList &queries,
                               const QString &channelName,
                               const std::vector<MessagePtr> &snapshot)
{
    if (queries.isEmpty())
    {
        return filter(QString(), channelName, snapshot);
    }

    std::vector<std::vector<std::unique_ptr<MessagePredicate>>> queryPredicates;
    queryPredicates.reserve(static_cast<size_t>(queries.size()));
    for (const auto &query : queries)
    {
        const auto trimmed = query.trimmed();
        if (!trimmed.isEmpty())
        {
            queryPredicates.push_back(parsePredicates(trimmed));
        }
    }

    if (queryPredicates.empty())
    {
        return filter(QString(), channelName, snapshot);
    }

    ChannelPtr channel(new Channel(channelName, Channel::Type::None));

    for (const auto &message : snapshot)
    {
        if (!message)
        {
            continue;
        }

        bool accept = false;
        for (const auto &predicates : queryPredicates)
        {
            bool matchesQuery = true;
            for (const auto &pred : predicates)
            {
                if (!pred->appliesTo(*message))
                {
                    matchesQuery = false;
                    break;
                }
            }

            if (matchesQuery)
            {
                accept = true;
                break;
            }
        }

        if (accept)
        {
            auto overrideFlags = std::optional<MessageFlags>(message->flags);
            overrideFlags->set(MessageFlag::DoNotLog);

            channel->addMessage(message, MessageContext::Repost, overrideFlags);
        }
    }

    return channel;
}

SearchPopup::SearchPopup(QWidget *parent, Split *split)
    : BasePopup(
          {
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
    , split_(split)
{
    this->initLayout();
    if (this->split_ && this->split_->getChannelView().hasSelection())
    {
        this->searchInput_->setText(
            this->split_->getChannelView().getSelectedText().trimmed());
        this->searchInput_->selectAll();
    }
    this->resize(400, 600);
    this->addShortcuts();

    this->themeChangedEvent();
}

void SearchPopup::addShortcuts()
{
    HotkeyController::HotkeyMap actions{
        {"search",
         [this](const std::vector<QString> &) -> QString {
             this->searchInput_->setFocus();
             this->searchInput_->selectAll();
             return "";
         }},
        {"delete",
         [this](const std::vector<QString> &) -> QString {
             this->close();
             return "";
         }},

        {"reject", nullptr},
        {"accept", nullptr},
        {"openTab", nullptr},
        {"scrollPage", nullptr},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::PopupWindow, actions, this);
}

void SearchPopup::addChannel(ChannelView &channel)
{
    auto underlyingChannel = channel.underlyingChannel();
    if (!underlyingChannel)
    {
        return;
    }

    if (this->searchChannels_.empty())
    {
        this->channelView_->setSourceChannel(underlyingChannel);
        this->channelName_ = underlyingChannel->getName();
    }
    else if (this->searchChannels_.size() == 1)
    {
        this->channelView_->setSourceChannel(
            std::make_shared<Channel>("multichannel", Channel::Type::None));

        auto flags = this->channelView_->getFlags();
        flags.set(MessageElementFlag::ChannelName);
        flags.unset(MessageElementFlag::ModeratorTools);
        this->channelView_->setOverrideFlags(flags);
    }

    this->searchChannels_.append(std::ref(channel));

    this->updateWindowTitle();
}

void SearchPopup::goToMessage(const MessagePtr &message)
{
    for (const auto &view : this->searchChannels_)
    {
        const auto underlyingChannel = view.get().underlyingChannel();
        if (!underlyingChannel)
        {
            continue;
        }

        const auto type = underlyingChannel->getType();
        if (type == Channel::Type::TwitchMentions ||
            type == Channel::Type::TwitchAutomod)
        {
            getApp()->getWindows()->scrollToMessage(message);
            return;
        }

        if (view.get().scrollToMessage(message))
        {
            return;
        }
    }
}

void SearchPopup::goToMessageId(const QString &messageId)
{
    for (const auto &view : this->searchChannels_)
    {
        if (view.get().scrollToMessageId(messageId))
        {
            return;
        }
    }
}

void SearchPopup::updateWindowTitle()
{
    QString historyName;

    if (this->searchChannels_.size() > 1)
    {
        historyName = "multiple channels'";
    }
    else if (this->channelName_ == "/automod")
    {
        historyName = "automod";
    }
    else if (this->channelName_ == "/mentions")
    {
        historyName = "mentions";
    }
    else if (this->channelName_ == "/whispers")
    {
        historyName = "whispers";
    }
    else if (this->channelName_.isEmpty())
    {
        historyName = "<empty>'s";
    }
    else
    {
        historyName = QString("%1's").arg(this->channelName_);
    }
    this->setWindowTitle("Searching in " + historyName + " history");
}

void SearchPopup::showEvent(QShowEvent *e)
{
    this->search();
    BaseWindow::showEvent(e);
}

bool SearchPopup::eventFilter(QObject *object, QEvent *event)
{
    if (object == this->searchInput_ && event->type() == QEvent::KeyPress)
    {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Return ||
            keyEvent->key() == Qt::Key_Enter)
        {
            this->addSearchTerm();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Backspace &&
            this->searchInput_->text().isEmpty() &&
            !this->searchQueries_.isEmpty())
        {
            this->removeSearchTerm(this->searchQueries_.size() - 1);
            return true;
        }
        if (keyEvent->matches(QKeySequence::DeleteStartOfWord) &&
            this->searchInput_->selectionLength() > 0)
        {
            this->searchInput_->backspace();
            return true;
        }
    }
    return false;
}

void SearchPopup::themeChangedEvent()
{
    BasePopup::themeChangedEvent();

    this->setPalette(getTheme()->palette);
}

void SearchPopup::search()
{
    if (this->snapshot_.size() == 0)
    {
        this->snapshot_ = this->buildSnapshot();
    }

    this->channelView_->setChannel(filter(this->activeSearchQueries(),
                                          this->channelName_, this->snapshot_));
}

void SearchPopup::addSearchTerm()
{
    const auto query = this->searchInput_->text().trimmed();
    if (query.isEmpty())
    {
        return;
    }

    for (const auto &existing : this->searchQueries_)
    {
        if (existing.compare(query, Qt::CaseInsensitive) == 0)
        {
            this->searchInput_->clear();
            this->search();
            return;
        }
    }

    this->searchQueries_.append(query);
    this->searchInput_->clear();
    this->rebuildSearchTerms();
    this->search();
}

void SearchPopup::removeSearchTerm(int index)
{
    if (index < 0 || index >= this->searchQueries_.size())
    {
        return;
    }

    this->searchQueries_.removeAt(index);
    this->rebuildSearchTerms();
    this->search();
}

void SearchPopup::rebuildSearchTerms()
{
    while (auto *item = this->searchTermsLayout_->takeAt(0))
    {
        if (auto *widget = item->widget())
        {
            widget->deleteLater();
        }
        delete item;
    }

    for (int i = 0; i < this->searchQueries_.size(); ++i)
    {
        auto *row = new QWidget(this->searchTerms_);
        row->setObjectName(QStringLiteral("searchTermRow"));
        row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        row->setFixedHeight(22);

        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(8, 0, 4, 0);
        layout->setSpacing(6);

        auto *label = new QLabel(this->searchQueries_.at(i), row);
        label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(label);

        auto *removeButton = new QToolButton(row);
        removeButton->setAutoRaise(true);
        removeButton->setCursor(Qt::PointingHandCursor);
        removeButton->setIcon(QPixmap(QStringLiteral(":/buttons/trashCan.png")));
        removeButton->setIconSize(QSize(14, 14));
        removeButton->setFixedSize(20, 20);
        removeButton->setToolTip(QStringLiteral("Remove search term"));
        layout->addWidget(removeButton);

        QObject::connect(removeButton, &QToolButton::clicked, this,
                         [this, row] {
                             const int index =
                                 this->searchTermsLayout_->indexOf(row);
                             this->removeSearchTerm(index);
                         });

        this->searchTermsLayout_->addWidget(row);
    }

    this->searchTerms_->setVisible(!this->searchQueries_.isEmpty());
    this->searchTerms_->adjustSize();
}

QStringList SearchPopup::activeSearchQueries() const
{
    QStringList queries = this->searchQueries_;

    const auto current = this->searchInput_->text().trimmed();
    if (!current.isEmpty())
    {
        queries.append(current);
    }

    return queries;
}

std::vector<MessagePtr> SearchPopup::buildSnapshot()
{
    // no point in filtering/sorting if it's a single channel search
    if (this->searchChannels_.length() == 1)
    {
        const auto channelPtr = this->searchChannels_.at(0).get().channel();
        if (!channelPtr)
        {
            return {};
        }

        return channelPtr->getMessageSnapshot();
    }

    auto combinedSnapshot = std::vector<std::shared_ptr<const Message>>{};
    for (auto &channel : this->searchChannels_)
    {
        ChannelView &sharedView = channel.get();
        const auto channelPtr = sharedView.channel();
        const auto underlyingChannel = sharedView.underlyingChannel();
        if (!channelPtr || !underlyingChannel)
        {
            continue;
        }

        const FilterSetPtr filterSet = sharedView.getFilterSet();
        std::vector<MessagePtr> snapshot = channelPtr->getMessageSnapshot();

        for (const auto &message : snapshot)
        {
            if (!message)
            {
                continue;
            }

            if (filterSet && !filterSet->filter(message, underlyingChannel))
            {
                continue;
            }

            combinedSnapshot.push_back(message);
        }
    }

    // remove any duplicate messages from splits containing the same channel
    std::sort(combinedSnapshot.begin(), combinedSnapshot.end(),
              [](MessagePtr &a, MessagePtr &b) {
                  if (!a || !b)
                  {
                      return static_cast<bool>(b);
                  }

                  return a->id > b->id;
              });

    auto uniqueIterator =
        std::unique(combinedSnapshot.begin(), combinedSnapshot.end(),
                    [](MessagePtr &a, MessagePtr &b) {
                        if (!a || !b)
                        {
                            return false;
                        }

                        // nullptr check prevents system messages from being dropped
                        return (a->id != nullptr) && a->id == b->id;
                    });

    combinedSnapshot.erase(uniqueIterator, combinedSnapshot.end());

    // resort by time for presentation
    std::sort(combinedSnapshot.begin(), combinedSnapshot.end(),
              [](MessagePtr &a, MessagePtr &b) {
                  if (!a || !b)
                  {
                      return static_cast<bool>(b);
                  }

                  return a->serverReceivedTime < b->serverReceivedTime;
              });

    return combinedSnapshot;
}

void SearchPopup::initLayout()
{
    // VBOX
    {
        auto *layout1 = new QVBoxLayout(this);
        layout1->setContentsMargins(0, 0, 0, 0);
        layout1->setSpacing(0);

        // HBOX
        {
            auto *layout2 = new QHBoxLayout();
            layout2->setContentsMargins(8, 8, 8, 8);
            layout2->setSpacing(8);

            // SEARCH INPUT
            {
                this->searchInput_ = new QLineEdit(this);
                layout2->addWidget(this->searchInput_);

                this->searchInput_->setPlaceholderText("Type to search");
                this->searchInput_->setClearButtonEnabled(true);
                if (auto *clearButton =
                        this->searchInput_->findChild<QAbstractButton *>())
                {
                    clearButton->setIcon(
                        QPixmap(":/buttons/clearSearch.png"));
                }
                QObject::connect(this->searchInput_, &QLineEdit::textChanged,
                                 this, &SearchPopup::search);
                this->searchInput_->installEventFilter(this);
            }

            layout1->addLayout(layout2);
        }

        // SEARCH TERMS
        {
            this->searchTerms_ = new QWidget(this);
            this->searchTerms_->setSizePolicy(QSizePolicy::Expanding,
                                              QSizePolicy::Maximum);
            this->searchTermsLayout_ = new QVBoxLayout(this->searchTerms_);
            this->searchTermsLayout_->setContentsMargins(0, 0, 0, 0);
            this->searchTermsLayout_->setSpacing(0);
            this->searchTerms_->setVisible(false);
            layout1->addWidget(this->searchTerms_);
        }

        // CHANNELVIEW
        {
            this->channelView_ = new ChannelView(
                this, this->split_, ChannelView::Context::Search,
                getSettings()->scrollbackSplitLimit);

            layout1->addWidget(this->channelView_);
        }

        this->setLayout(layout1);
    }

    this->searchInput_->setFocus();
}

std::vector<std::unique_ptr<MessagePredicate>> SearchPopup::parsePredicates(
    const QString &input)
{
    // This regex captures all name:value predicate pairs into named capturing
    // groups and matches all other inputs seperated by spaces as normal
    // strings.
    // It also ignores whitespaces in values when being surrounded by quotation
    // marks, to enable inputs like this => regex:"kappa 123"
    static QRegularExpression predicateRegex(
        R"lit((?<negation>[!\-])?(?:(?<name>\w+):(?<value>".+?"|[^\s]+))|[^\s]+?(?=$|\s))lit");
    static QRegularExpression trimQuotationMarksRegex(R"(^"|"$)");

    QRegularExpressionMatchIterator it = predicateRegex.globalMatch(input);

    std::vector<std::unique_ptr<MessagePredicate>> predicates;

    while (it.hasNext())
    {
        QRegularExpressionMatch match = it.next();

        QString name = match.captured("name");
        bool isNegated = !match.captured("negation").isEmpty();
        QString value = match.captured("value");
        value.remove(trimQuotationMarksRegex);

        // match predicates

        if (name == "from")
        {
            predicates.push_back(
                std::make_unique<AuthorPredicate>(value, isNegated));
        }
        else if (name == "badge")
        {
            predicates.push_back(
                std::make_unique<BadgePredicate>(value, isNegated));
        }
        else if (name == "subtier")
        {
            predicates.push_back(
                std::make_unique<SubtierPredicate>(value, isNegated));
        }
        else if (name == "has" && value == "link")
        {
            predicates.push_back(std::make_unique<LinkPredicate>(isNegated));
        }
        else if (name == "in")
        {
            predicates.push_back(
                std::make_unique<ChannelPredicate>(value, isNegated));
        }
        else if (name == "is")
        {
            predicates.push_back(
                std::make_unique<MessageFlagsPredicate>(value, isNegated));
        }
        else if (name == "regex")
        {
            predicates.push_back(
                std::make_unique<RegexPredicate>(value, isNegated));
        }
        else
        {
            predicates.push_back(
                std::make_unique<SubstringPredicate>(match.captured()));
        }
    }

    return predicates;
}

}  // namespace chatterino
