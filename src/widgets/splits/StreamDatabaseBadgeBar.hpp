// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "widgets/BaseWidget.hpp"

#include <QHash>
#include <QNetworkAccessManager>
#include <QPoint>
#include <QPointer>
#include <QPixmap>
#include <QRect>
#include <QSize>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVariantAnimation>

#include <pajlada/signals/signalholder.hpp>

#include <memory>
#include <vector>

class QByteArray;
class QEvent;
class QFont;
class QMenu;
class QMouseEvent;
class QNetworkReply;
class QPaintEvent;
class QPainter;
class QResizeEvent;

namespace chatterino {

struct StreamDatabaseBadgeChannelLink {
    QString name;
    QString login;
};

struct StreamDatabaseBadgeTextLink {
    QString text;
    QString url;
};

class StreamDatabaseBadgeBar final : public BaseWidget
{
public:
    explicit StreamDatabaseBadgeBar(QWidget *parent = nullptr);
    ~StreamDatabaseBadgeBar() override;

    struct BadgeItem {
        QString badgeName;
        QString eventTitle;
        QString statusText;
        QString endsText;
        QString requirementText;
        QString accent;
        QString badgeImageUrl;
        QString siteUrl;
        QString setID;
        QString versionID;
        std::vector<StreamDatabaseBadgeChannelLink> channelLinks;
        std::vector<StreamDatabaseBadgeTextLink> requirementLinks;
        bool active = false;
    };

    static std::vector<BadgeItem> parseEventBadges(const QByteArray &data,
                                                   bool activeOnly = true,
                                                   bool upcomingOnly = false);
    static QString badgeKey(const BadgeItem &badge);
    void applyEventsData(const QByteArray &data, bool allowPings);

    void setChannel(const ChannelPtr &channel);
    void setBadgeFeedVisible(bool visible);
    bool badgeFeedVisible() const;

    QSize sizeHint() const override;

protected:
    bool event(QEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scaleChangedEvent(float scale) override;
    void themeChangedEvent() override;

private:
    struct ChannelLinkRect {
        QRect rect;
        QString login;
        QString url;
    };

    void setVisibleForChannel(bool visible);
    void updateEventsRefreshTimer();
    void restartTickerIfNeeded();
    void pauseTickerForPointer();
    void cancelTickerSlideToSource();
    bool beginTickerSlide(int direction, int durationMs);
    void advanceTicker();
    void finishTickerSlide();
    [[nodiscard]] int tickerIndexAfterStep(
        const std::vector<int> &visibleBadgeIndexes, int sourceIndex,
        int direction) const;
    void setNavigationHoverVisible(bool visible);
    void stepBadgeFeed(int direction);
    void setExpandedIndex(int index);
    void animateDetail(qreal targetHeight);
    void updateFixedHeight();
    void updateNavigationHover(const QPoint &position);
    void updateHoverState(const QPoint &position);
    void showBadgeMenu(int badgeIndex, const QPoint &globalPosition);
    void showSettingsMenu(const QPoint &globalPosition,
                          bool toggleExisting = false,
                          bool alignRight = false);
    void requestEvents();
    void notifyBadgePings(const std::vector<BadgeItem> &newBadges,
                          const std::vector<BadgeItem> &availableNowBadges);
    void requestBadgeImages();
    void handleBadgeImageFinished(QNetworkReply *reply);
    void requestStreamDatabaseLogo();
    void handleStreamDatabaseLogoFinished(QNetworkReply *reply);
    void reconcileOwnedBadgeVisibility();

    [[nodiscard]] int collapsedHeight() const;
    [[nodiscard]] int expandedDetailHeight() const;
    [[nodiscard]] int barHeight() const;
    [[nodiscard]] int badgeIndexAt(const QPoint &position) const;
    [[nodiscard]] int channelLinkIndexAt(const QPoint &position) const;
    [[nodiscard]] bool linkContains(const QPoint &position) const;
    [[nodiscard]] bool settingsButtonContains(const QPoint &position) const;
    [[nodiscard]] bool sourceLinkContains(const QPoint &position) const;
    [[nodiscard]] bool navigationHoverContains(const QPoint &position) const;
    [[nodiscard]] bool previousArrowContains(const QPoint &position) const;
    [[nodiscard]] bool nextArrowContains(const QPoint &position) const;
    [[nodiscard]] bool collapsedBadgeInfoContains(
        const QPoint &position) const;
    [[nodiscard]] int linkedBadgeIndex() const;
    [[nodiscard]] bool canRunTicker() const;
    [[nodiscard]] bool badgeHiddenByOwnedMode(int index) const;
    [[nodiscard]] std::vector<int> visibleBadgeIndexes() const;
    [[nodiscard]] int normalizedTickerIndex(
        const std::vector<int> &visibleBadgeIndexes) const;

    void drawBadgeIcon(QPainter &painter, const QRect &rect,
                       const BadgeItem &badge, bool active) const;
    void drawRequirementText(QPainter &painter, const QRect &rect,
                             const BadgeItem &badge, const QFont &font,
                             const QColor &textColor,
                             const QColor &linkColor);
    void drawOwnedBadgeMark(QPainter &painter, const QRect &rect) const;
    void drawNavigationArrow(QPainter &painter, const QRect &rect,
                             bool previous, qreal progress, bool hovered,
                             const QColor &color) const;
    void drawStreamDatabaseLink(QPainter &painter, const QRect &rect,
                                const QFont &font, bool hovered) const;
    void drawSettingsButton(QPainter &painter, const QRect &rect,
                            bool hovered) const;
    void drawStreamDatabaseLogoMark(QPainter &painter,
                                    const QRect &rect) const;

    std::vector<BadgeItem> badges_;
    std::weak_ptr<Channel> streamDatabaseChannel_;
    QNetworkAccessManager network_;
    QTimer tickerTimer_;
    QVariantAnimation tickerSlideAnimation_;
    QVariantAnimation detailAnimation_;
    QVariantAnimation navigationHoverAnimation_;
    pajlada::Signals::SignalHolder ownedBadgeConnections_;
    QHash<QString, QPixmap> badgePixmaps_;
    QHash<QNetworkReply *, QString> pendingBadgeRequests_;
    QSet<QString> seenEventBadgeKeys_;
    QSet<QString> seenActiveEventBadgeKeys_;
    QNetworkReply *pendingLogoRequest_ = nullptr;
    QPixmap streamDatabaseLogo_;
    bool hasLoadedEvents_ = false;
    bool channelAllowsFeed_ = false;
    bool badgeFeedVisible_ = true;
    bool visibleForChannel_ = false;
    bool hoverPaused_ = false;
    int tickerIndex_ = 0;
    int slideSourceTickerIndex_ = -1;
    int pendingTickerIndex_ = -1;
    qreal tickerSlideProgress_ = 0.0;
    int tickerSlideDirection_ = 1;
    qreal navigationHoverProgress_ = 0.0;
    int expandedIndex_ = -1;
    qreal detailVisibleHeight_ = 0.0;
    int hoverBadgeIndex_ = -1;
    bool hoverSiteLink_ = false;
    bool hoverSettingsButton_ = false;
    bool hoverSourceLink_ = false;
    bool hoverChannelLink_ = false;
    bool hoverPreviousArrow_ = false;
    bool hoverNextArrow_ = false;
    QRect siteLinkRect_;
    QRect settingsButtonRect_;
    QRect sourceLinkRect_;
    QRect previousArrowRect_;
    QRect nextArrowRect_;
    std::vector<QRect> badgeRects_;
    std::vector<ChannelLinkRect> channelLinkRects_;
    QPointer<QMenu> settingsMenu_;
};

}  // namespace chatterino
