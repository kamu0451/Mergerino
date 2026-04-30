// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BasePopup.hpp"

#include <pajlada/signals/signal.hpp>
#include <QLineEdit>

#include <cstdint>
#include <memory>
#include <vector>

namespace chatterino {

struct Link;
class ChannelView;
class Channel;
using ChannelPtr = std::shared_ptr<Channel>;
class Notebook;
class TwitchChannel;
class KickChannel;
enum class MessagePlatform : uint8_t;

class EmotePopup : public BasePopup
{
public:
    EmotePopup(QWidget *parent = nullptr);

    void loadChannel(ChannelPtr channel,
                     std::vector<MessagePlatform> platforms = {});

    void closeEvent(QCloseEvent *event) override;

    pajlada::Signals::Signal<Link> linkClicked;

protected:
    void resizeEvent(QResizeEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void themeChangedEvent() override;

private:
    ChannelView *allEmotesView_{};
    ChannelView *globalEmotesView_{};
    ChannelView *channelEmotesView_{};
    ChannelView *viewEmojis_{};
    /**
     * @brief Visible only when the user has specified a search query into the `search_` input.
     * Otherwise the `notebook_` and all other views are visible.
     */
    ChannelView *searchView_{};

    ChannelPtr channel_;
    TwitchChannel *twitchChannel_{};
    KickChannel *kickChannel_{};

    QLineEdit *search_;
    Notebook *notebook_;

    void filterTwitchEmotes(std::shared_ptr<Channel> searchChannel,
                            const QString &searchText);
    void filterEmotes(const QString &text);
    void addShortcuts() override;
    bool eventFilter(QObject *object, QEvent *event) override;

    void reloadEmotes();

    void saveBounds() const;
};

}  // namespace chatterino
