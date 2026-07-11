// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "ForwardDecl.hpp"
#include "widgets/BasePopup.hpp"

#include <QStringList>

#include <memory>

class QLineEdit;
class QVBoxLayout;
class QWidget;

namespace chatterino {

class Split;
class MessagePredicate;

class SearchPopup : public BasePopup
{
public:
    SearchPopup(QWidget *parent, Split *split = nullptr);

    virtual void addChannel(ChannelView &channel);
    void goToMessage(const MessagePtr &message);
    /**
     * This method should only be used for searches that
     * don't include a mentions channel,
     * since it will only search in the opened channels (not globally).
     * @param messageId
     */
    void goToMessageId(const QString &messageId);

protected:
    virtual void updateWindowTitle();
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;
    void themeChangedEvent() override;

private:
    void initLayout();
    void search();
    void addSearchTerm();
    void removeSearchTerm(int index);
    void rebuildSearchTerms();
    QStringList activeSearchQueries() const;
    void addShortcuts() override;
    std::vector<MessagePtr> buildSnapshot();

    /**
     * @brief Only retains those message from a list of messages that satisfy a
     *        search query.
     *
     * @param text          the search query -- will be parsed for MessagePredicates
     * @param channelName   name of the channel to be returned
     * @param snapshot      list of messages to filter
     * @param filterSet     channel filter to apply
     *
     * @return a ChannelPtr with "channelName" and the filtered messages from
     *         "snapshot"
     */
    static ChannelPtr filter(const QString &text, const QString &channelName,
                             const std::vector<MessagePtr> &snapshot);
    static ChannelPtr filter(const QStringList &queries,
                             const QString &channelName,
                             const std::vector<MessagePtr> &snapshot);

    /**
     * @brief Checks the input for tags and registers their corresponding
     *        predicates.
     *
     * @param input the string to check for tags
     * @return a vector of MessagePredicates requested in the input
     */
    static std::vector<std::unique_ptr<MessagePredicate>> parsePredicates(
        const QString &input);

    std::vector<MessagePtr> snapshot_;
    QLineEdit *searchInput_{};
    QWidget *searchTerms_{};
    QVBoxLayout *searchTermsLayout_{};
    QStringList searchQueries_;
    ChannelView *channelView_{};
    QString channelName_{};
    Split *split_ = nullptr;
    QList<std::reference_wrapper<ChannelView>> searchChannels_;
};

}  // namespace chatterino
