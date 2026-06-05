// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "widgets/BaseWidget.hpp"

#include <pajlada/signals/signalholder.hpp>

#include <boost/signals2/connection.hpp>
#include <QRect>
#include <QSize>
#include <QTimer>

#include <memory>
#include <optional>
#include <vector>

class QPaintEvent;
class QPainter;

namespace chatterino {

struct HelixPoll;
struct HelixPrediction;
class TwitchChannel;

class TwitchPollsAndPredictionsBar final : public BaseWidget
{
public:
    explicit TwitchPollsAndPredictionsBar(QWidget *parent = nullptr);

    void setChannel(const ChannelPtr &channel);
    void refreshNow();

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void scaleChangedEvent(float scale) override;
    void themeChangedEvent() override;

private:
    enum class ItemKind {
        Poll,
        Prediction,
    };

    struct Choice {
        QString title;
        int weight = 0;
        QString detail;
        int points = 0;
        int users = 0;
        bool showPredictionMetrics = false;
    };

    struct Item {
        ItemKind kind = ItemKind::Poll;
        QString title;
        QString status;
        std::vector<Choice> choices;
    };

    void clearItems();
    void scheduleRefresh(int delayMs);
    void refresh();
    void finishRequest(int generation);
    void updateItems();
    void updateFixedHeight();

    [[nodiscard]] static std::optional<Item> makePollItem(
        const HelixPoll &poll);
    [[nodiscard]] static std::optional<Item> makePredictionItem(
        const HelixPrediction &prediction);
    [[nodiscard]] int barHeight() const;
    [[nodiscard]] int itemHeight(const Item &item) const;
    void drawItem(QPainter &painter, const Item &item, QRect rect) const;

    std::weak_ptr<TwitchChannel> twitchChannel_;
    pajlada::Signals::SignalHolder channelSignalHolder_;
    QTimer refreshTimer_;
    std::vector<Item> items_;
    std::optional<Item> pendingPoll_;
    std::optional<Item> pendingPrediction_;
    int pendingRequests_ = 0;
    int requestGeneration_ = 0;
    std::vector<boost::signals2::scoped_connection> bSignals_;
};

}  // namespace chatterino
