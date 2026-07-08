// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitInput.hpp"

#include "Application.hpp"
#include "common/ChannelChatters.hpp"
#include "common/enums/MessageOverflow.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/completion/TabCompletionModel.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/spellcheck/SpellChecker.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/Link.hpp"
#include "messages/Message.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchModerationAuth.hpp"
#include "providers/twitch/api/TwitchWebApi.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "providers/twitch/CurrentUserBadges.hpp"
#include "providers/twitch/TwitchBadgeIdentity.hpp"
#include "providers/twitch/TwitchBadges.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "providers/youtube/YouTubeLiveChat.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Helpers.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/buttons/Button.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/CreatePollDialog.hpp"
#include "widgets/dialogs/CreatePredictionDialog.hpp"
#include "widgets/dialogs/EmotePopup.hpp"
#include "widgets/dialogs/ManagePredictionDialog.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/CmdDeleteKeyFilter.hpp"
#include "widgets/helper/MessageView.hpp"
#include "widgets/helper/ResizingTextEdit.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/InputCompletionPopup.hpp"
#include "widgets/splits/InputHighlighter.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/splits/StreamDatabaseBadgeBar.hpp"
#include "widgets/splits/TwitchPollsAndPredictionsBar.hpp"

#include <QCompleter>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialog>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QFrame>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGuiApplication>
#include <QGraphicsOpacityEffect>
#include <QHash>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QSignalBlocker>
#include <QSvgRenderer>
#include <QSet>
#include <QSizePolicy>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QVector>
#include <QWidgetAction>

#include <QtCore/qscopeguard.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <ranges>
#include <utility>

using namespace Qt::Literals;

namespace chatterino {

namespace {

// Current function: https://www.desmos.com/calculator/vdyamchjwh
qreal highlightEasingFunction(qreal progress)
{
    if (progress <= 0.1)
    {
        return 1.0 - pow(10.0 * progress, 3.0);
    }
    return 1.0 + pow((20.0 / 9.0) * (0.5 * progress - 0.5), 3.0);
}

QString platformDisplayName(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return u"Kick"_s;
        case MessagePlatform::YouTube:
            return u"YouTube"_s;
        case MessagePlatform::AnyOrTwitch:
        default:
            return u"Twitch"_s;
    }
}

QString platformIconPath(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return u":/platforms/kick.svg"_s;
        case MessagePlatform::YouTube:
            return u":/platforms/youtube.svg"_s;
        case MessagePlatform::AnyOrTwitch:
        default:
            return u":/platforms/twitch.svg"_s;
    }
}

MessagePlatform messagePlatformForProvider(ProviderId provider)
{
    switch (provider)
    {
        case ProviderId::Kick:
            return MessagePlatform::Kick;
        case ProviderId::YouTube:
            return MessagePlatform::YouTube;
        case ProviderId::Twitch:
        default:
            return MessagePlatform::AnyOrTwitch;
    }
}

QString platformDisplayName(const std::vector<MessagePlatform> &platforms)
{
    QStringList names;
    for (const auto platform : platforms)
    {
        names.append(platformDisplayName(platform));
    }
    return names.join(u" + "_s);
}

bool containsPlatform(const std::vector<MessagePlatform> &platforms,
                      MessagePlatform platform)
{
    return std::ranges::find(platforms, platform) != platforms.end();
}

bool isRealTwitchChatIdentityChannel(const std::shared_ptr<TwitchChannel> &channel)
{
    return channel != nullptr && channel->getType() == Channel::Type::Twitch &&
           !channel->getName().trimmed().startsWith(QLatin1Char('/'));
}

QString &badgeIdentityObservedAccountID()
{
    static QString accountID;
    return accountID;
}

struct ChatIdentityBadgeSource {
    QString key;
    QString pixmapKey;
    QString imageUrl;
    QString fallbackBadgeName;

    bool operator==(const ChatIdentityBadgeSource &other) const
    {
        return this->key == other.key && this->pixmapKey == other.pixmapKey &&
               this->imageUrl == other.imageUrl &&
               this->fallbackBadgeName == other.fallbackBadgeName;
    }
};

QString chatIdentityBadgePixmapKey(const ChatIdentityBadgeSource &badge)
{
    if (!badge.pixmapKey.trimmed().isEmpty())
    {
        return badge.pixmapKey.trimmed();
    }
    if (!badge.imageUrl.trimmed().isEmpty())
    {
        return badge.imageUrl.trimmed();
    }
    if (!badge.fallbackBadgeName.trimmed().isEmpty())
    {
        return QStringLiteral("twitch:%1").arg(badge.fallbackBadgeName.trimmed());
    }
    return {};
}

class ChatIdentityButton final : public QToolButton
{
public:
    explicit ChatIdentityButton(QWidget *parent = nullptr)
        : QToolButton(parent)
        , network_(this)
        , carouselTimer_(this)
        , slideAnimation_(this)
    {
        this->setMouseTracking(true);

        this->carouselTimer_.setInterval(2000);
        QObject::connect(&this->carouselTimer_, &QTimer::timeout, this,
                         [this] {
                             this->advanceCarousel();
                         });

        this->slideAnimation_.setDuration(170);
        this->slideAnimation_.setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(&this->slideAnimation_, &QVariantAnimation::valueChanged,
                         this, [this](const QVariant &value) {
                             this->slideProgress_ =
                                 std::clamp(value.toReal(), 0.0, 1.0);
                             this->update();
                         });
        QObject::connect(&this->slideAnimation_, &QVariantAnimation::finished,
                         this, [this] {
                             this->displayIndex_ = this->targetIndex_;
                             this->slideProgress_ = 1.0;
                             this->update();
                         });
    }

    void setBadgeSources(QVector<ChatIdentityBadgeSource> badges)
    {
        if (this->badges_ == badges)
        {
            return;
        }

        this->badges_ = std::move(badges);
        this->displayIndex_ = 0;
        this->targetIndex_ = 0;
        this->previousIndex_ = 0;
        this->slideProgress_ = 1.0;
        this->slideAnimation_.stop();
        this->carouselTimer_.stop();
        this->badgePixmaps_.clear();
        ++this->badgeGeneration_;

        for (const auto &badge : this->badges_)
        {
            this->requestBadgePixmap(badge);
        }

        if (this->underMouse() && this->badges_.size() > 1)
        {
            this->carouselTimer_.start();
        }
        this->update();
    }

protected:
    void enterEvent(QEnterEvent *event) override
    {
        QToolButton::enterEvent(event);
        if (this->badges_.size() > 1)
        {
            this->carouselTimer_.start();
        }
    }

    void leaveEvent(QEvent *event) override
    {
        QToolButton::leaveEvent(event);
        this->carouselTimer_.stop();
        if (this->displayIndex_ != 0 || this->targetIndex_ != 0)
        {
            this->animateToIndex(0, -1);
        }
        this->update();
    }

    void paintEvent(QPaintEvent *event) override
    {
        (void)event;

        const qreal paintNudgeX =
            std::max<qreal>(0.0, this->width() - this->height());
        const qreal paintedSize = std::max<qreal>(0.0, this->height() - 4.0);
        const QRectF paintedBounds{paintNudgeX + 1.0, 2.0, paintedSize,
                                   paintedSize};
        const QColor background =
            this->isDown() ? QColor("#3a3a42")
                           : (this->underMouse() ? QColor("#2f2f35")
                                                 : QColor("#24242a"));
        const QColor border =
            this->underMouse() ? QColor("#777782") : QColor("#3f3f46");

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(border, 1));
        painter.setBrush(background);
        painter.drawRoundedRect(paintedBounds, 3, 3);

        QPainterPath clipPath;
        clipPath.addRoundedRect(paintedBounds.adjusted(1, 1, -1, -1), 2, 2);
        painter.setClipPath(clipPath);

        const QRectF iconBounds = paintedBounds.adjusted(2, 2, -2, -2);
        if (this->slideAnimation_.state() == QAbstractAnimation::Running &&
            this->previousIndex_ != this->targetIndex_)
        {
            const qreal distance = iconBounds.width() + 5.0;
            const qreal progress = std::clamp(this->slideProgress_, 0.0, 1.0);
            this->paintBadge(painter, iconBounds.translated(
                                          -this->slideDirection_ * distance *
                                              progress,
                                          0),
                             this->previousIndex_, 1.0 - (progress * 0.35));
            this->paintBadge(painter, iconBounds.translated(
                                          this->slideDirection_ * distance *
                                              (1.0 - progress),
                                          0),
                             this->targetIndex_, 0.65 + (progress * 0.35));
            return;
        }

        this->paintBadge(painter, iconBounds, this->displayIndex_, 1.0);
    }

private:
    static QHash<QString, QPixmap> &badgeImageCache()
    {
        static QHash<QString, QPixmap> cache;
        return cache;
    }

    static QString cacheKeyForBadge(const ChatIdentityBadgeSource &badge)
    {
        return chatIdentityBadgePixmapKey(badge);
    }

    void requestBadgePixmap(const ChatIdentityBadgeSource &badge)
    {
        const auto cacheKey = cacheKeyForBadge(badge);
        const auto pixmapKey = badge.pixmapKey.trimmed().isEmpty()
                                   ? cacheKey
                                   : badge.pixmapKey.trimmed();
        if (badge.key.isEmpty() || pixmapKey.isEmpty() || cacheKey.isEmpty())
        {
            return;
        }

        auto &cache = this->badgeImageCache();
        const auto cached = cache.constFind(cacheKey);
        if (cached != cache.cend())
        {
            this->badgePixmaps_.insert(pixmapKey, cached.value());
            return;
        }

        const int generation = this->badgeGeneration_;
        if (!badge.imageUrl.trimmed().isEmpty())
        {
            auto *reply = this->network_.get(QNetworkRequest(QUrl(badge.imageUrl)));
            this->pendingReplies_.insert(reply, pixmapKey);
            QObject::connect(reply, &QNetworkReply::finished, this,
                             [this, reply, pixmapKey, cacheKey,
                              generation] {
                                 this->pendingReplies_.remove(reply);
                                 const auto cleanup = qScopeGuard([reply] {
                                     reply->deleteLater();
                                 });
                                 if (generation != this->badgeGeneration_ ||
                                     reply->error() != QNetworkReply::NoError)
                                 {
                                     return;
                                 }

                                 QPixmap pixmap;
                                 pixmap.loadFromData(reply->readAll());
                                 if (pixmap.isNull())
                                 {
                                     return;
                                 }

                                 this->badgeImageCache().insert(cacheKey, pixmap);
                                 this->badgePixmaps_.insert(pixmapKey, pixmap);
                                 this->update();
                             });
            return;
        }

        if (badge.fallbackBadgeName.trimmed().isEmpty())
        {
            return;
        }

        getApp()->getTwitchBadges()->getBadgeIcon(
            badge.fallbackBadgeName,
            [guard = QPointer<ChatIdentityButton>(this),
             pixmapKey, cacheKey, generation](const QString &, const auto icon) {
                if (guard == nullptr || generation != guard->badgeGeneration_ ||
                    icon == nullptr)
                {
                    return;
                }

                const auto pixmap = icon->pixmap(QSize{64, 64});
                if (pixmap.isNull())
                {
                    return;
                }

                guard->badgeImageCache().insert(cacheKey, pixmap);
                guard->badgePixmaps_.insert(pixmapKey, pixmap);
                guard->update();
            });
    }

    void advanceCarousel()
    {
        if (this->badges_.size() <= 1)
        {
            return;
        }

        this->animateToIndex((this->displayIndex_ + 1) % this->badges_.size(), 1);
    }

    void animateToIndex(int index, int direction)
    {
        if (this->badges_.isEmpty())
        {
            this->displayIndex_ = 0;
            this->targetIndex_ = 0;
            this->previousIndex_ = 0;
            this->slideProgress_ = 1.0;
            this->slideAnimation_.stop();
            this->update();
            return;
        }

        index = std::clamp(index, 0,
                           static_cast<int>(this->badges_.size()) - 1);
        const int current =
            this->slideAnimation_.state() == QAbstractAnimation::Running
                ? this->targetIndex_
                : this->displayIndex_;
        if (current == index)
        {
            return;
        }

        this->slideAnimation_.stop();
        this->previousIndex_ = current;
        this->targetIndex_ = index;
        this->slideDirection_ = direction < 0 ? -1 : 1;
        this->slideProgress_ = 0.0;
        this->slideAnimation_.setStartValue(0.0);
        this->slideAnimation_.setEndValue(1.0);
        this->slideAnimation_.start();
    }

    void paintBadge(QPainter &painter, const QRectF &iconBounds, int index,
                    qreal opacity) const
    {
        painter.save();
        painter.setOpacity(std::clamp(opacity, 0.0, 1.0));

        const QPixmap pixmap = this->pixmapForIndex(index);
        if (!pixmap.isNull())
        {
            const auto scaled = pixmap.scaled(
                iconBounds.size().toSize(), Qt::KeepAspectRatio,
                Qt::SmoothTransformation);
            const QPointF topLeft{
                iconBounds.center().x() - (scaled.width() / 2.0),
                iconBounds.center().y() - (scaled.height() / 2.0)};
            painter.drawPixmap(topLeft, scaled);
            painter.restore();
            return;
        }

        const QPointF center = iconBounds.center();
        const qreal outerRadius =
            std::min(iconBounds.width(), iconBounds.height()) * 0.34;
        const qreal innerRadius = outerRadius * 0.45;

        QPainterPath star;
        constexpr qreal halfPi = 1.5707963267948966;
        constexpr qreal quarterPi = 0.7853981633974483;
        for (int i = 0; i < 8; ++i)
        {
            const qreal angle = -halfPi + (quarterPi * i);
            const qreal radius = i % 2 == 0 ? outerRadius : innerRadius;
            const QPointF point{center.x() + std::cos(angle) * radius,
                                center.y() + std::sin(angle) * radius};
            if (i == 0)
            {
                star.moveTo(point);
            }
            else
            {
                star.lineTo(point);
            }
        }
        star.closeSubpath();

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#adadb8"));
        painter.drawPath(star);
        painter.restore();
    }

    QPixmap pixmapForIndex(int index) const
    {
        if (index < 0 || index >= this->badges_.size())
        {
            return {};
        }

        const auto &badge = this->badges_[index];
        return this->badgePixmaps_.value(chatIdentityBadgePixmapKey(badge));
    }

    QNetworkAccessManager network_;
    QHash<QNetworkReply *, QString> pendingReplies_;
    QHash<QString, QPixmap> badgePixmaps_;
    QVector<ChatIdentityBadgeSource> badges_;
    QTimer carouselTimer_;
    QVariantAnimation slideAnimation_;
    int badgeGeneration_ = 0;
    int displayIndex_ = 0;
    int targetIndex_ = 0;
    int previousIndex_ = 0;
    int slideDirection_ = 1;
    qreal slideProgress_ = 1.0;
};

class BadgeIdentityToggle final : public QCheckBox
{
public:
    explicit BadgeIdentityToggle(const QString &text, QWidget *parent = nullptr)
        : QCheckBox(text, parent)
        , animation_(this)
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setMinimumHeight(28);
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        this->animation_.setDuration(190);
        this->animation_.setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(&this->animation_, &QVariantAnimation::valueChanged,
                         this, [this](const QVariant &value) {
                             this->progress_ =
                                 std::clamp(value.toReal(), 0.0, 1.0);
                             this->update();
                         });
        QObject::connect(this, &QCheckBox::toggled, this, [this](bool checked) {
            this->animation_.stop();
            this->animation_.setStartValue(this->progress_);
            this->animation_.setEndValue(checked ? 1.0 : 0.0);
            this->animation_.start();
        });
    }

    void setCheckedInstantly(bool checked)
    {
        const QSignalBlocker blocker(this);
        this->animation_.stop();
        QCheckBox::setChecked(checked);
        this->progress_ = checked ? 1.0 : 0.0;
        this->update();
    }

    QSize sizeHint() const override
    {
        const QFontMetrics metrics(this->font());
        return {46 + metrics.horizontalAdvance(this->text()), 28};
    }

protected:
    bool hitButton(const QPoint &pos) const override
    {
        return this->rect().contains(pos);
    }

    void paintEvent(QPaintEvent *event) override
    {
        (void)event;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const qreal trackWidth = 38.0;
        const qreal trackHeight = 20.0;
        const QRectF trackRect{
            0.0, (this->height() - trackHeight) / 2.0, trackWidth,
            trackHeight};
        const auto progress = std::clamp(this->progress_, 0.0, 1.0);

        const QColor offTrack("#30313a");
        const QColor onTrack("#9146ff");
        const QColor offBorder("#6f707a");
        const QColor onBorder("#bf94ff");
        const auto mixColor = [](const QColor &a, const QColor &b, qreal t) {
            return QColor::fromRgbF(a.redF() + ((b.redF() - a.redF()) * t),
                                    a.greenF() +
                                        ((b.greenF() - a.greenF()) * t),
                                    a.blueF() + ((b.blueF() - a.blueF()) * t),
                                    a.alphaF() +
                                        ((b.alphaF() - a.alphaF()) * t));
        };

        painter.setPen(QPen(mixColor(offBorder, onBorder, progress), 1.4));
        painter.setBrush(mixColor(offTrack, onTrack, progress));
        painter.drawRoundedRect(trackRect, trackHeight / 2.0,
                                trackHeight / 2.0);

        const qreal knobSize = 14.0;
        const qreal knobX = trackRect.left() + 3.0 +
                            ((trackRect.width() - knobSize - 6.0) * progress);
        const QRectF shadowRect{knobX, trackRect.center().y() - knobSize / 2.0 +
                                           1.0,
                                knobSize, knobSize};
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 65));
        painter.drawEllipse(shadowRect);

        const QRectF knobRect{knobX, trackRect.center().y() - knobSize / 2.0,
                              knobSize, knobSize};
        painter.setBrush(QColor("#f4f1ff"));
        painter.drawEllipse(knobRect);

        painter.setPen(QColor("#efeff1"));
        const QRect textRect{int(trackRect.right() + 8), 0,
                             std::max(0, this->width() -
                                             int(trackRect.right() + 8)),
                             this->height()};
        painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                         this->text());
    }

private:
    QVariantAnimation animation_;
    qreal progress_ = 0.0;
};

std::shared_ptr<TwitchChannel> twitchChannelForPollPrediction(
    const ChannelPtr &channel)
{
    if (channel == nullptr)
    {
        return nullptr;
    }

    if (auto twitch = std::dynamic_pointer_cast<TwitchChannel>(channel))
    {
        return twitch;
    }

    if (auto merged = std::dynamic_pointer_cast<MergedChannel>(channel))
    {
        return std::dynamic_pointer_cast<TwitchChannel>(
            merged->twitchChannel());
    }

    return nullptr;
}

bool canManagePollPredictions(const std::shared_ptr<TwitchChannel> &channel)
{
    return channel != nullptr &&
           (channel->isMod() || channel->isBroadcaster());
}

bool hasBroadcasterPollPredictionToken(
    const std::shared_ptr<TwitchChannel> &channel)
{
    if (channel == nullptr)
    {
        return false;
    }

    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (account == nullptr || account->isAnon())
    {
        return false;
    }

    const auto roomId = channel->roomId();
    if (!roomId.isEmpty())
    {
        return roomId == account->getUserId();
    }

    return channel->isBroadcaster();
}

bool needsModerationAuthLogin(const std::shared_ptr<TwitchChannel> &channel)
{
    if (!canManagePollPredictions(channel) ||
        hasBroadcasterPollPredictionToken(channel))
    {
        return false;
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser == nullptr || currentUser->isAnon())
    {
        return false;
    }

    return !TwitchModerationAuth::resolveForCurrentUser(
                currentUser->getUserId())
                .isValid();
}

void showModerationAuthLoginPrompt(QWidget *parent, const QString &currentUserID,
                                   const QString &actionText,
                                   std::function<void()> successCallback)
{
    auto *dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->setWindowTitle(QStringLiteral("Twitch Browser Helper Login"));
    dialog->setMinimumWidth(460);

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("Helper token required"), dialog);
    title->setStyleSheet(QStringLiteral("QLabel { font-weight: 700; }"));
    layout->addWidget(title);

    auto *body = new QLabel(
        QStringLiteral(
            "To %1, Mergerino needs a Twitch browser helper token. Copy the "
            "helper, run it in the twitch.tv console, then click Paste Token.")
            .arg(actionText),
        dialog);
    body->setWordWrap(true);
    layout->addWidget(body);

    auto *status = new QLabel(
        QStringLiteral(
            "On twitch.tv, press F12 to open DevTools, switch to Console, "
            "paste the helper, then return here."),
        dialog);
    status->setWordWrap(true);
    status->setStyleSheet(QStringLiteral("QLabel { color: #9aa0a6; }"));
    layout->addWidget(status);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 4, 0, 0);
    buttonRow->setSpacing(8);

    auto *copyButton = new QPushButton(QStringLiteral("Copy Helper"), dialog);
    auto *pasteButton = new QPushButton(QStringLiteral("Paste Token"), dialog);
    auto *cancelButton = new QPushButton(QStringLiteral("Cancel"), dialog);

    buttonRow->addStretch(1);
    buttonRow->addWidget(copyButton);
    buttonRow->addWidget(pasteButton);
    buttonRow->addWidget(cancelButton);
    layout->addLayout(buttonRow);

    auto inFlight = std::make_shared<bool>(false);
    const auto setStatus = [status](const QString &message, bool isError) {
        status->setText(message);
        status->setStyleSheet(QStringLiteral("QLabel { color: %1; }")
                                  .arg(isError ? QStringLiteral("#ff7b72")
                                               : QStringLiteral("#9aa0a6")));
    };
    const auto setBusy = [inFlight, copyButton, pasteButton, cancelButton](
                             bool busy) {
        *inFlight = busy;
        copyButton->setEnabled(!busy);
        pasteButton->setEnabled(!busy);
        cancelButton->setEnabled(!busy);
    };

    QObject::connect(copyButton, &QPushButton::clicked, dialog,
                     [setStatus] {
                         TwitchModerationAuth::copyHelperToClipboard();
                         QDesktopServices::openUrl(
                             QUrl(QStringLiteral("https://www.twitch.tv/")));
                         setStatus(QStringLiteral(
                                       "Helper copied. On twitch.tv, press F12 "
                                       "to open DevTools, switch to Console, "
                                       "paste it, then return here and click "
                                       "Paste Token."),
                                   false);
                     });

    QObject::connect(cancelButton, &QPushButton::clicked, dialog,
                     [dialog] {
                         dialog->reject();
                     });

    QObject::connect(
        pasteButton, &QPushButton::clicked, dialog,
        [dialog = QPointer<QDialog>(dialog), currentUserID, setStatus, setBusy,
         inFlight, successCallback = std::move(successCallback)]() mutable {
            if (*inFlight)
            {
                return;
            }

            const auto clipboardText =
                TwitchModerationAuth::clipboardText().trimmed();
            if (clipboardText.isEmpty() ||
                clipboardText.contains(QStringLiteral("localStorage")) ||
                clipboardText.contains(
                    QStringLiteral("Mergerino token copied")))
            {
                setStatus(QStringLiteral(
                              "Run the copied helper in the Twitch console "
                              "first. On twitch.tv, press F12 to open "
                              "DevTools, switch to Console, paste it, then "
                              "click Paste Token."),
                          true);
                return;
            }

            const auto payload =
                TwitchModerationAuth::parseClipboardPayload(clipboardText);
            if (payload.oauthToken.isEmpty())
            {
                setStatus(QStringLiteral(
                              "Clipboard text is not a Twitch token. Run the "
                              "copied helper on twitch.tv, then click Paste "
                              "Token."),
                          true);
                return;
            }

            if (payload.oauthToken.size() > TwitchModerationAuth::maxTokenLength())
            {
                setStatus(QStringLiteral(
                              "Clipboard token is too long to be a Twitch "
                              "token. Run the copied helper on twitch.tv and "
                              "paste only the copied token."),
                          true);
                return;
            }

            setBusy(true);
            setStatus(QStringLiteral("Validating Twitch token..."), false);
            TwitchModerationAuth::validateToken(
                payload.oauthToken,
                [dialog, currentUserID, payload, setStatus, setBusy,
                 successCallback](TwitchModerationAuth::Account account) mutable {
                    if (dialog == nullptr)
                    {
                        return;
                    }

                    QTimer::singleShot(
                        0, dialog.data(),
                        [dialog, currentUserID, payload, account, setStatus,
                         setBusy, successCallback]() mutable {
                            if (dialog == nullptr)
                            {
                                return;
                            }

                            setBusy(false);
                            if (!account.supportsWebGql())
                            {
                                setStatus(QStringLiteral(
                                              "That token is not a Twitch "
                                              "browser token. Run the copied "
                                              "helper on twitch.tv."),
                                          true);
                                return;
                            }

                            const auto currentID = currentUserID.trimmed();
                            if (!currentID.isEmpty() &&
                                !account.userId.isEmpty() &&
                                account.userId != currentID)
                            {
                                setStatus(QStringLiteral(
                                              "That token belongs to a "
                                              "different Twitch account. Log "
                                              "into twitch.tv with the same "
                                              "account Mergerino is using, "
                                              "then run the helper again."),
                                          true);
                                return;
                            }

                            account.clientIntegrity = payload.clientIntegrity;
                            account.deviceId = payload.deviceId;
                            TwitchModerationAuth::saveAccount(account);
                            dialog->accept();
                            successCallback();
                        });
                },
                [dialog, setStatus, setBusy](const QString &error) {
                    if (dialog == nullptr)
                    {
                        return;
                    }

                    QTimer::singleShot(0, dialog.data(),
                                       [dialog, error, setStatus, setBusy] {
                                           if (dialog == nullptr)
                                           {
                                               return;
                                           }

                                           setBusy(false);
                                           setStatus(error, true);
                                       });
                });
        });

    dialog->show();
    dialog->activateWindow();
    dialog->raise();
}

QUrl twitchPollPopoutUrl(const TwitchChannel &channel)
{
    return QUrl(QStringLiteral("https://www.twitch.tv/popout/%1/poll")
                    .arg(channel.getName()));
}

QUrl twitchPredictionSummaryUrl(const TwitchChannel &channel)
{
    return QUrl(QStringLiteral(
                    "https://www.twitch.tv/popout/%1/predictions/summary")
                    .arg(channel.getName()));
}

std::vector<MessagePlatform> platformSelectionIntersection(
    const std::vector<MessagePlatform> &availablePlatforms,
    const std::vector<MessagePlatform> &selectedPlatforms)
{
    std::vector<MessagePlatform> result;
    for (const auto platform : selectedPlatforms)
    {
        if (containsPlatform(availablePlatforms, platform) &&
            !containsPlatform(result, platform))
        {
            result.push_back(platform);
        }
    }
    return result;
}

int platformButtonWidthForCount(int platformCount, float scale)
{
    platformCount = std::max(platformCount, 1);
    if (platformCount == 1)
    {
        return int(22 * scale);
    }

    return int((16 * platformCount + 12 * (platformCount - 1) + 2) * scale);
}

QIcon badgeIdentityChevronIcon(bool collapsed)
{
    QPixmap pixmap(18, 18);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor("#dedee3"), 2.2, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));

    QPainterPath path;
    if (collapsed)
    {
        path.moveTo(6.5, 4.5);
        path.lineTo(11.0, 9.0);
        path.lineTo(6.5, 13.5);
    }
    else
    {
        path.moveTo(4.5, 6.5);
        path.lineTo(9.0, 11.0);
        path.lineTo(13.5, 6.5);
    }
    painter.drawPath(path);

    return QIcon(pixmap);
}

}  // namespace

class StreamDatabaseBadgePickerPopup final : public QFrame
{
public:
    explicit StreamDatabaseBadgePickerPopup(QWidget *parent = nullptr);

    void setContext(const QString &channelName, const QString &channelID,
                    const QString &accountName,
                    const std::shared_ptr<TwitchChannel> &twitchChannel);
    void setAppliedCallback(std::function<void()> callback);
    void showForAnchor(QWidget *anchor);
    bool wasRecentlyHidden() const;

protected:
    void hideEvent(QHideEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    using BadgeItem = twitch::CurrentUserBadgeIdentity;
    enum class BadgeGridKind {
        Global,
        ChannelCustom,
    };

    struct BadgeButton {
        QToolButton *button = nullptr;
        BadgeItem badge;
        BadgeGridKind kind = BadgeGridKind::Global;
    };

    struct BadgeIconLabel {
        QLabel *label = nullptr;
        BadgeItem badge;
    };

    void initLayout();
    void requestEvents();
    void setBadges(QVector<BadgeItem> badges);
    void rebuildGrid();
    void addCollapsibleSection(
        const QString &key, const QString &title,
        const std::function<void(QVBoxLayout *)> &builder);
    void addBadgeGrid(QVBoxLayout *layout, const QVector<BadgeItem> &badges,
                      BadgeGridKind kind);
    void requestBadgeImages();
    void handleBadgeImageFinished(QNetworkReply *reply);
    void applyBadgeSelection(const BadgeItem &badge, BadgeGridKind kind);
    void applyNameColor(const QString &color);
    void requestCurrentUserDisplayName();
    void syncBadgeButtonStates();
    void updateButtonIcon(QToolButton *button, const BadgeItem &badge) const;
    void updateBadgeIconLabel(QLabel *label, const BadgeItem &badge) const;
    void updatePreview();
    void setEmptyText(const QString &text);
    QVector<BadgeItem> globalBadges() const;
    QVector<BadgeItem> roleBadges() const;
    QVector<BadgeItem> subscriberBadges() const;
    const BadgeItem *badgeForKey(const QString &key) const;
    void chooseDefaultSelections();

    QLabel *contextLabel_ = nullptr;
    QWidget *previewBadgesWidget_ = nullptr;
    QHBoxLayout *previewBadgesLayout_ = nullptr;
    QLabel *previewName_ = nullptr;
    QScrollArea *scrollArea_ = nullptr;
    QWidget *contentWidget_ = nullptr;
    QVBoxLayout *contentLayout_ = nullptr;
    QLabel *emptyLabel_ = nullptr;

    QNetworkAccessManager network_;
    QNetworkReply *pendingEventsRequest_ = nullptr;
    QNetworkReply *pendingSelectionRequest_ = nullptr;
    QHash<QNetworkReply *, QString> pendingBadgeRequests_;
    QHash<QString, QPixmap> badgePixmaps_;
    QVector<BadgeItem> badges_;
    std::vector<BadgeButton> badgeButtons_;
    std::vector<BadgeIconLabel> badgeIconLabels_;
    QHash<QString, bool> collapsedSections_;
    QString channelName_;
    QString channelID_;
    QString accountName_;
    std::weak_ptr<TwitchChannel> twitchChannel_;
    QString selectedGlobalBadgeKey_;
    QString selectedCustomBadgeKey_;
    QString selectedColor_ = QStringLiteral("#00ff7f");
    bool hideBadgeFlair_ = false;
    bool useCustomChannelBadge_ = true;
    int badgeRequestGeneration_ = 0;
    int nameColorRequestGeneration_ = 0;
    int displayNameRequestGeneration_ = 0;
    QElapsedTimer lastHideTimer_;
    std::function<void()> appliedCallback_;
};

namespace {

QString initialsForBadge(const twitch::CurrentUserBadgeIdentity &badge)
{
    QString initials;
    const QString source = badge.title.isEmpty() ? badge.setID : badge.title;
    for (const auto ch : source)
    {
        if (ch.isLetterOrNumber())
        {
            initials.append(ch.toUpper());
            if (initials.size() >= 2)
            {
                break;
            }
        }
    }

    return initials.isEmpty() ? QStringLiteral("?") : initials;
}

bool badgeMatchesSearch(const twitch::CurrentUserBadgeIdentity &badge,
                        const QString &query)
{
    if (query.isEmpty())
    {
        return true;
    }

    return badge.title.toCaseFolded().contains(query) ||
           badge.description.toCaseFolded().contains(query) ||
           badge.setID.toCaseFolded().contains(query) ||
           badge.channelDisplayName.toCaseFolded().contains(query) ||
           badge.channelLogin.toCaseFolded().contains(query);
}

QString badgeSet(const twitch::CurrentUserBadgeIdentity &badge)
{
    return badge.setID.trimmed().toCaseFolded();
}

bool isRoleBadge(const twitch::CurrentUserBadgeIdentity &badge)
{
    const auto set = badgeSet(badge);
    return set == QStringLiteral("broadcaster") ||
           set == QStringLiteral("lead_moderator") ||
           set == QStringLiteral("lead-moderator") ||
           set == QStringLiteral("moderator") ||
           set == QStringLiteral("vip") ||
           set == QStringLiteral("artist-badge") ||
           set == QStringLiteral("staff") ||
           set == QStringLiteral("admin") ||
           set == QStringLiteral("global_mod");
}

bool isSubscriberBadge(const twitch::CurrentUserBadgeIdentity &badge)
{
    const auto set = badgeSet(badge);
    return set == QStringLiteral("subscriber") ||
           set == QStringLiteral("founder");
}

bool isLeadModeratorBadge(const twitch::CurrentUserBadgeIdentity &badge)
{
    const auto set = badgeSet(badge);
    return set == QStringLiteral("lead_moderator") ||
           set == QStringLiteral("lead-moderator") ||
           twitch::isLeadModeratorBadgeIdentity(badge);
}

bool isChannelBadge(const twitch::CurrentUserBadgeIdentity &badge)
{
    return !badge.channelLogin.trimmed().isEmpty() || isRoleBadge(badge) ||
           isSubscriberBadge(badge);
}

bool isGlobalBadge(const twitch::CurrentUserBadgeIdentity &badge)
{
    return !isChannelBadge(badge);
}

QString channelDisplayNameForIdentity(const QString &channelName)
{
    const auto trimmed = channelName.trimmed();
    if (trimmed.isEmpty())
    {
        return QStringLiteral("this channel");
    }
    return trimmed;
}

QString channelPossessiveForIdentity(const QString &channelName)
{
    const auto displayName = channelDisplayNameForIdentity(channelName);
    if (displayName == QStringLiteral("this channel"))
    {
        return displayName;
    }
    if (displayName.endsWith(QLatin1Char('s'), Qt::CaseInsensitive))
    {
        return displayName + QStringLiteral("' channel");
    }
    return displayName + QStringLiteral("'s channel");
}

QString roleBadgeText(const twitch::CurrentUserBadgeIdentity &badge,
                      const QString &channelName)
{
    const auto set = badgeSet(badge);
    const auto channel = channelDisplayNameForIdentity(channelName);
    if (set == QStringLiteral("broadcaster"))
    {
        return QStringLiteral("Broadcaster for this channel");
    }
    if (isLeadModeratorBadge(badge))
    {
        return QStringLiteral("Lead Moderator in %1's channel").arg(channel);
    }
    if (set == QStringLiteral("moderator"))
    {
        return QStringLiteral("Moderator in %1's channel").arg(channel);
    }
    if (set == QStringLiteral("vip"))
    {
        return QStringLiteral("VIP in %1's channel").arg(channel);
    }
    if (set == QStringLiteral("artist-badge"))
    {
        return QStringLiteral("Artist for this channel");
    }

    return badge.description.isEmpty() ? badge.title : badge.description;
}

QString subscriberBadgeText(const twitch::CurrentUserBadgeIdentity &badge)
{
    if (!badge.description.trimmed().isEmpty())
    {
        return badge.description.trimmed();
    }
    if (!badge.title.trimmed().isEmpty())
    {
        return QStringLiteral("Subscribed as %1").arg(badge.title.trimmed());
    }
    return QStringLiteral("Subscriber badge for this channel");
}

QString imageUrlForEmoteBadge(const EmotePtr &emote)
{
    if (emote == nullptr)
    {
        return {};
    }

    auto imageUrl = [](const ImagePtr &image) {
        if (image == nullptr || image->isEmpty())
        {
            return QString{};
        }
        return image->url().string;
    };

    auto url = imageUrl(emote->images.getImage3());
    if (!url.isEmpty())
    {
        return url;
    }
    url = imageUrl(emote->images.getImage2());
    if (!url.isEmpty())
    {
        return url;
    }
    return imageUrl(emote->images.getImage1());
}

QString twitchBadgeImageUrl(const TwitchChannel *channel,
                            const twitch::CurrentUserBadgeIdentity &badge)
{
    const auto version = badge.versionID.trimmed();
    if (version.isEmpty())
    {
        return {};
    }

    auto tryBadge = [&](const QString &setID) {
        const auto normalizedSet = setID.trimmed();
        if (normalizedSet.isEmpty())
        {
            return QString{};
        }

        if (channel != nullptr)
        {
            if (const auto emote = channel->twitchBadge(normalizedSet, version))
            {
                return imageUrlForEmoteBadge(*emote);
            }
        }

        const auto globalEmote =
            getApp()->getTwitchBadges()->badge(normalizedSet, version);
        return globalEmote ? imageUrlForEmoteBadge(*globalEmote) : QString{};
    };

    if (isLeadModeratorBadge(badge))
    {
        auto url = tryBadge(QStringLiteral("lead-moderator"));
        if (!url.isEmpty())
        {
            return url;
        }
        url = tryBadge(QStringLiteral("lead_moderator"));
        if (!url.isEmpty())
        {
            return url;
        }
    }

    auto url = tryBadge(badge.setID);
    if (!url.isEmpty())
    {
        return url;
    }

    return {};
}

QString customFfzRoleBadgeImageUrl(
    const TwitchChannel &channel, const twitch::CurrentUserBadgeIdentity &badge)
{
    const auto set = badgeSet(badge);
    if (set == QStringLiteral("moderator") &&
        getSettings()->useCustomFfzModeratorBadges)
    {
        const auto customModBadge = channel.ffzCustomModBadge();
        return customModBadge ? imageUrlForEmoteBadge(*customModBadge)
                              : QString{};
    }
    if (set == QStringLiteral("vip") && getSettings()->useCustomFfzVipBadges)
    {
        const auto customVipBadge = channel.ffzCustomVipBadge();
        return customVipBadge ? imageUrlForEmoteBadge(*customVipBadge)
                              : QString{};
    }

    return {};
}

QString leadModeratorBadgeImageUrl()
{
    return QStringLiteral(
        "https://static-cdn.jtvnw.net/badges/v1/"
        "0822047b-65e0-46f2-94a9-d1091d685d33/3");
}

ChatIdentityBadgeSource chatIdentitySourceForBadge(
    const twitch::CurrentUserBadgeIdentity &badge,
    const TwitchChannel *channel = nullptr)
{
    ChatIdentityBadgeSource source;
    source.key = twitch::badgeIdentityKey(badge);
    if (source.key.isEmpty())
    {
        source.key = badge.setID.trimmed().toCaseFolded();
        if (!badge.versionID.trimmed().isEmpty())
        {
            source.key += QLatin1Char('/');
            source.key += badge.versionID.trimmed().toCaseFolded();
        }
    }

    const auto normalizedChannel =
        channel == nullptr ? QString{} : channel->getName().trimmed().toCaseFolded();
    const auto normalizedBadgeChannel =
        twitch::normalizedBadgeChannelLogin(badge.channelLogin);
    const bool badgeBelongsToChannel =
        !normalizedChannel.isEmpty() && normalizedBadgeChannel == normalizedChannel;
    const bool scopedToChannel =
        (isRoleBadge(badge) || isSubscriberBadge(badge)) &&
        (!normalizedBadgeChannel.isEmpty() || !normalizedChannel.isEmpty());
    const auto scopedChannel =
        !normalizedBadgeChannel.isEmpty() ? normalizedBadgeChannel
                                          : normalizedChannel;
    if (scopedToChannel && !scopedChannel.isEmpty() && !source.key.isEmpty())
    {
        source.key =
            QStringLiteral("channel:%1:%2").arg(scopedChannel, source.key);
    }

    source.imageUrl = badge.imageUrl.trimmed();
    if (channel != nullptr && (isRoleBadge(badge) || isSubscriberBadge(badge)))
    {
        const auto ffzRoleImageUrl = customFfzRoleBadgeImageUrl(*channel, badge);
        if (!ffzRoleImageUrl.isEmpty())
        {
            source.imageUrl = ffzRoleImageUrl;
        }
        else
        {
            const auto twitchImageUrl = twitchBadgeImageUrl(channel, badge);
            if (!twitchImageUrl.isEmpty())
            {
                source.imageUrl = twitchImageUrl;
            }
            else if (isSubscriberBadge(badge) && !badgeBelongsToChannel)
            {
                source.imageUrl.clear();
            }
        }
    }
    if (source.imageUrl.isEmpty() && isLeadModeratorBadge(badge))
    {
        source.imageUrl = leadModeratorBadgeImageUrl();
    }
    source.fallbackBadgeName = badge.setID.trimmed().toCaseFolded();
    if (!badge.versionID.trimmed().isEmpty())
    {
        source.fallbackBadgeName += QLatin1Char('/');
        source.fallbackBadgeName += badge.versionID.trimmed();
    }
    if (isSubscriberBadge(badge) && source.imageUrl.isEmpty())
    {
        source.fallbackBadgeName.clear();
    }
    source.pixmapKey = chatIdentityBadgePixmapKey(source);
    if (scopedToChannel && !scopedChannel.isEmpty() &&
        !source.pixmapKey.isEmpty() &&
        !source.pixmapKey.startsWith(QStringLiteral("channel:")))
    {
        source.pixmapKey =
            QStringLiteral("channel:%1:%2").arg(scopedChannel, source.pixmapKey);
    }
    return source;
}

QString nameColorErrorText(HelixUpdateUserChatColorError error,
                           const QString &message)
{
    switch (error)
    {
        case HelixUpdateUserChatColorError::UserMissingScope:
            return QStringLiteral(
                "Missing user:manage:chat_color scope. Re-login with Twitch "
                "and try again.");

        case HelixUpdateUserChatColorError::InvalidColor:
            return QStringLiteral("Twitch rejected that chat color.");

        case HelixUpdateUserChatColorError::Forwarded:
            return message.trimmed().isEmpty()
                       ? QStringLiteral("Twitch rejected that chat color.")
                       : message.trimmed();

        case HelixUpdateUserChatColorError::Unknown:
        default:
            return QStringLiteral("Failed to change chat color.");
    }
}

void setBadgePickerSectionFont(QLabel *label)
{
    if (label == nullptr)
    {
        return;
    }

    auto font = label->font();
    font.setBold(true);
    label->setFont(font);
}

QLabel *makeBadgePickerText(QWidget *parent, const QString &text, bool muted)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setProperty("muted", muted);
    return label;
}

void addBadgePickerDivider(QVBoxLayout *layout, QWidget *parent)
{
    auto *divider = new QFrame(parent);
    divider->setObjectName(QStringLiteral("BadgeIdentityDivider"));
    divider->setFrameShape(QFrame::HLine);
    layout->addWidget(divider);
}

void clearBadgePickerLayout(QLayout *layout)
{
    while (auto *item = layout->takeAt(0))
    {
        if (auto *widget = item->widget())
        {
            widget->deleteLater();
        }
        if (auto *childLayout = item->layout())
        {
            clearBadgePickerLayout(childLayout);
        }
        delete item;
    }
}

QString badgePickerStyleSheet()
{
    return QStringLiteral(R"(
QFrame#StreamDatabaseBadgePickerPopup {
    background: #1f1f23;
    border: 1px solid #3a3a40;
    border-radius: 6px;
}
QFrame#BadgeIdentityPreview {
    background: #1f1f23;
    border: 0;
}
QFrame#BadgeIdentityDivider {
    color: #303038;
    background: #303038;
    min-height: 1px;
    max-height: 1px;
    border: 0;
}
QLabel {
    color: #efeff1;
}
QLabel[muted="true"] {
    color: #adadb8;
}
QScrollArea {
    background: transparent;
    border: 0;
}
QWidget#BadgeIdentityContent {
    background: #0e0e10;
}
QScrollBar:vertical {
    background: transparent;
    width: 10px;
}
QScrollBar::handle:vertical {
    background: #777782;
    border-radius: 5px;
    min-height: 30px;
}
QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical {
    height: 0;
}
QToolButton#BadgeIdentityCloseButton {
    background: transparent;
    border: 0;
    border-radius: 4px;
    color: #dedee3;
    font-weight: 700;
}
QToolButton#BadgeIdentityCloseButton:hover {
    background: #2f2f35;
}
QToolButton#BadgeIdentitySectionHeader {
    background: transparent;
    border: 0;
    color: #efeff1;
    font-weight: 700;
    text-align: left;
    padding: 4px 0;
}
QToolButton#BadgeIdentitySectionHeader:hover {
    color: #bf94ff;
}
QToolButton#StreamDatabaseBadgeOption {
    background: #111114;
    border: 1px solid #2d2d35;
    border-radius: 3px;
    color: #dedee3;
    font-weight: 700;
    padding: 1px;
}
QToolButton#StreamDatabaseBadgeOption:hover {
    border-color: #777782;
    background: #19191f;
}
QToolButton#StreamDatabaseBadgeOption:checked {
    border: 2px solid #9146ff;
    background: #251548;
}
QToolButton#BadgeIdentityColorSwatch {
    border: 2px solid #2f2f35;
    border-radius: 14px;
}
QToolButton#BadgeIdentityColorSwatch:hover,
QToolButton#BadgeIdentityColorSwatch:checked {
    border-color: #efeff1;
}
QPushButton#BadgeIdentityLinkButton {
    background: transparent;
    border: 0;
    color: #bf94ff;
    text-align: left;
    padding: 0;
}
QPushButton#BadgeIdentityLinkButton:hover {
    color: #d7c0ff;
}
)");
}

QString badgeSectionTitle(const QString &title, int count)
{
    return QStringLiteral("%1 (%2)").arg(title).arg(count);
}

}  // namespace

StreamDatabaseBadgePickerPopup::StreamDatabaseBadgePickerPopup(QWidget *parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint)
    , network_(this)
{
    this->setObjectName(QStringLiteral("StreamDatabaseBadgePickerPopup"));
    this->setAttribute(Qt::WA_StyledBackground);
    this->setFixedSize(358, 650);
    this->setStyleSheet(badgePickerStyleSheet());

    this->initLayout();
    this->setEmptyText(QStringLiteral("Loading Twitch badges..."));
}

void StreamDatabaseBadgePickerPopup::setContext(const QString &channelName,
                                                const QString &channelID,
                                                const QString &accountName,
                                                const std::shared_ptr<TwitchChannel>
                                                    &twitchChannel)
{
    this->twitchChannel_ = twitchChannel;
    const QString safeChannel =
        channelName.trimmed().isEmpty() ? QStringLiteral("this channel")
                                        : channelName.trimmed();
    this->contextLabel_->setText(
        QStringLiteral("How your name will appear in chat on %1:")
            .arg(channelPossessiveForIdentity(safeChannel)));

    this->accountName_ = accountName.trimmed().isEmpty()
                             ? QStringLiteral("Twitch user")
                             : accountName.trimmed();
    this->previewName_->setText(this->accountName_);
    this->requestCurrentUserDisplayName();

    const auto normalizedChannel =
        twitch::normalizedBadgeChannelLogin(channelName);
    this->channelID_ = channelID.trimmed();
    this->useCustomChannelBadge_ =
        twitch::currentUserUsesCustomChannelBadge(normalizedChannel);
    this->hideBadgeFlair_ =
        twitch::currentUserHidesBadgeFlair(normalizedChannel);
    if (this->channelName_ != normalizedChannel)
    {
        this->channelName_ = normalizedChannel;
        ++this->badgeRequestGeneration_;
        if (this->pendingEventsRequest_ != nullptr)
        {
            this->pendingEventsRequest_->abort();
            this->pendingEventsRequest_ = nullptr;
        }
        this->badges_.clear();
        this->badgePixmaps_.clear();
        this->selectedGlobalBadgeKey_.clear();
        this->selectedCustomBadgeKey_.clear();
        this->rebuildGrid();
    }

    this->requestEvents();
}

void StreamDatabaseBadgePickerPopup::setAppliedCallback(
    std::function<void()> callback)
{
    this->appliedCallback_ = std::move(callback);
}

void StreamDatabaseBadgePickerPopup::showForAnchor(QWidget *anchor)
{
    if (anchor == nullptr)
    {
        this->show();
        return;
    }

    QPoint position = anchor->mapToGlobal(QPoint{0, 0});
    int x = position.x();
    int y = position.y() - this->height() - 8;

    auto *screen = anchor->screen();
    if (screen == nullptr)
    {
        screen = QGuiApplication::screenAt(position);
    }
    if (screen != nullptr)
    {
        const QRect bounds = screen->availableGeometry();
        x = std::clamp(x, bounds.left(),
                       std::max(bounds.left(), bounds.right() - this->width()));
        if (y < bounds.top())
        {
            y = position.y() + anchor->height() + 8;
        }
    }

    this->move(x, y);
    this->show();
    this->raise();
    this->activateWindow();
    this->setFocus(Qt::PopupFocusReason);
}

bool StreamDatabaseBadgePickerPopup::wasRecentlyHidden() const
{
    return this->lastHideTimer_.isValid() &&
           this->lastHideTimer_.elapsed() < 250;
}

void StreamDatabaseBadgePickerPopup::hideEvent(QHideEvent *event)
{
    this->lastHideTimer_.restart();
    QFrame::hideEvent(event);
}

void StreamDatabaseBadgePickerPopup::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape)
    {
        this->hide();
        event->accept();
        return;
    }

    QFrame::keyPressEvent(event);
}

void StreamDatabaseBadgePickerPopup::initLayout()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(12, 8, 12, 8);
    titleRow->setSpacing(6);
    auto *spacer = new QWidget(this);
    spacer->setFixedWidth(24);
    titleRow->addWidget(spacer);

    auto *title = new QLabel(QStringLiteral("Chat Identity"), this);
    title->setAlignment(Qt::AlignCenter);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 1);
    title->setFont(titleFont);
    titleRow->addWidget(title, 1);

    auto *close = new QToolButton(this);
    close->setObjectName(QStringLiteral("BadgeIdentityCloseButton"));
    close->setText(QStringLiteral("x"));
    close->setCursor(Qt::PointingHandCursor);
    close->setFixedSize(24, 24);
    titleRow->addWidget(close);
    QObject::connect(close, &QToolButton::clicked, this, [this] {
        this->hide();
    });
    layout->addLayout(titleRow);

    auto *previewFrame = new QFrame(this);
    previewFrame->setObjectName(QStringLiteral("BadgeIdentityPreview"));
    auto *previewLayout = new QVBoxLayout(previewFrame);
    previewLayout->setContentsMargins(12, 8, 12, 12);
    previewLayout->setSpacing(7);

    auto *identityLabel =
        new QLabel(QStringLiteral("Identity Preview"), previewFrame);
    QFont sectionFont = identityLabel->font();
    sectionFont.setBold(true);
    identityLabel->setFont(sectionFont);
    previewLayout->addWidget(identityLabel);

    this->contextLabel_ = new QLabel(previewFrame);
    this->contextLabel_->setWordWrap(true);
    previewLayout->addWidget(this->contextLabel_);

    auto *previewRow = new QHBoxLayout;
    previewRow->setContentsMargins(0, 2, 0, 0);
    previewRow->setSpacing(4);
    this->previewBadgesWidget_ = new QWidget(previewFrame);
    this->previewBadgesLayout_ = new QHBoxLayout(this->previewBadgesWidget_);
    this->previewBadgesLayout_->setContentsMargins(0, 0, 0, 0);
    this->previewBadgesLayout_->setSpacing(3);
    previewRow->addWidget(this->previewBadgesWidget_, 0, Qt::AlignVCenter);

    this->previewName_ = new QLabel(previewFrame);
    QFont previewFont = this->previewName_->font();
    previewFont.setBold(true);
    previewFont.setPointSize(previewFont.pointSize() + 2);
    this->previewName_->setFont(previewFont);
    previewRow->addWidget(this->previewName_, 1);
    previewLayout->addLayout(previewRow);
    layout->addWidget(previewFrame);

    auto *topDivider = new QFrame(this);
    topDivider->setObjectName(QStringLiteral("BadgeIdentityDivider"));
    topDivider->setFrameShape(QFrame::HLine);
    layout->addWidget(topDivider);

    this->scrollArea_ = new QScrollArea(this);
    this->scrollArea_->setWidgetResizable(true);
    this->scrollArea_->setFrameShape(QFrame::NoFrame);
    this->contentWidget_ = new QWidget(this->scrollArea_);
    this->contentWidget_->setObjectName(QStringLiteral("BadgeIdentityContent"));
    this->contentLayout_ = new QVBoxLayout(this->contentWidget_);
    this->contentLayout_->setContentsMargins(12, 10, 12, 12);
    this->contentLayout_->setSpacing(8);
    this->scrollArea_->setWidget(this->contentWidget_);
    layout->addWidget(this->scrollArea_, 1);

    this->emptyLabel_ =
        new QLabel(QStringLiteral("Loading Twitch badges..."), this);
    this->emptyLabel_->setProperty("muted", true);
    this->emptyLabel_->setAlignment(Qt::AlignCenter);
    this->emptyLabel_->setWordWrap(true);
    this->emptyLabel_->hide();
    layout->addWidget(this->emptyLabel_);
}

void StreamDatabaseBadgePickerPopup::requestEvents()
{
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (account == nullptr || account->isAnon())
    {
        twitch::clearCurrentUserOwnedBadges();
        this->setBadges({});
        this->setEmptyText(QStringLiteral("Log in to Twitch to load badges."));
        return;
    }

    QString channelLogin = this->channelName_;
    if (channelLogin.isEmpty())
    {
        channelLogin = account->getUserName().trimmed().toLower();
    }
    if (channelLogin.isEmpty())
    {
        this->setBadges({});
        this->setEmptyText(QStringLiteral("Missing Twitch channel."));
        return;
    }

    if (this->pendingEventsRequest_ != nullptr)
    {
        return;
    }

    const auto badgeAuth =
        TwitchModerationAuth::resolveForCurrentUser(account->getUserId());
    if (!badgeAuth.supportsWebGql())
    {
        if (twitch::hasLoadedCurrentUserOwnedBadgesForChannel(channelLogin))
        {
            this->setBadges(
                twitch::currentUserOwnedBadgesForChannel(channelLogin));
        }
        else
        {
            this->setBadges({});
        }
        this->setEmptyText(
            QStringLiteral("Twitch browser helper token required."));
        showModerationAuthLoginPrompt(
            this, account->getUserId(),
            QStringLiteral("load Twitch chat badges"),
            [guard = QPointer<StreamDatabaseBadgePickerPopup>(this)] {
                if (guard != nullptr)
                {
                    guard->requestEvents();
                }
            });
        return;
    }

    this->setEmptyText(QStringLiteral("Loading Twitch badges..."));
    const int requestGeneration = ++this->badgeRequestGeneration_;
    this->pendingEventsRequest_ = twitch::requestCurrentUserBadgeIdentity(
        this->network_, channelLogin, badgeAuth, this,
        [this, requestGeneration](QVector<twitch::CurrentUserBadgeIdentity> badges) {
            if (requestGeneration != this->badgeRequestGeneration_)
            {
                return;
            }

            this->pendingEventsRequest_ = nullptr;
            twitch::updateCurrentUserOwnedBadgesForChannel(this->channelName_,
                                                           badges);
            this->setBadges(std::move(badges));
        },
        [this, requestGeneration](const QString &error) {
            if (requestGeneration != this->badgeRequestGeneration_)
            {
                return;
            }

            this->pendingEventsRequest_ = nullptr;
            if (twitch::hasLoadedCurrentUserOwnedBadgesForChannel(
                    this->channelName_))
            {
                this->setBadges(twitch::currentUserOwnedBadgesForChannel(
                    this->channelName_));
            }
            else
            {
                this->setBadges({});
            }
            this->setEmptyText(error.isEmpty()
                                   ? QStringLiteral("Failed to load Twitch badges.")
                                   : error);
        });
}

void StreamDatabaseBadgePickerPopup::setBadges(QVector<BadgeItem> badges)
{
    if (auto twitchChannel = this->twitchChannel_.lock())
    {
        for (auto &badge : badges)
        {
            const auto source =
                chatIdentitySourceForBadge(badge, twitchChannel.get());
            if (!source.imageUrl.isEmpty() || isSubscriberBadge(badge) ||
                isLeadModeratorBadge(badge))
            {
                badge.imageUrl = source.imageUrl;
            }
        }
    }

    this->badges_ = std::move(badges);
    this->useCustomChannelBadge_ =
        twitch::currentUserUsesCustomChannelBadge(this->channelName_);
    this->hideBadgeFlair_ =
        twitch::currentUserHidesBadgeFlair(this->channelName_);
    this->chooseDefaultSelections();
    if (this->badges_.isEmpty())
    {
        this->selectedGlobalBadgeKey_.clear();
        this->selectedCustomBadgeKey_.clear();
        if (this->emptyLabel_ != nullptr &&
            this->emptyLabel_->text() ==
                QStringLiteral("Loading Twitch badges..."))
        {
            this->setEmptyText(QStringLiteral("No Twitch badges found."));
        }
    }

    this->rebuildGrid();
    this->requestBadgeImages();
    this->updatePreview();
}

void StreamDatabaseBadgePickerPopup::rebuildGrid()
{
    clearBadgePickerLayout(this->contentLayout_);
    this->badgeButtons_.clear();
    this->badgeIconLabels_.clear();

    const auto globalBadges = this->globalBadges();
    const auto roleBadges = this->roleBadges();
    const auto subscriberBadges = this->subscriberBadges();
    const int channelBadgeCount =
        roleBadges.size() + subscriberBadges.size() +
        (this->useCustomChannelBadge_ ? globalBadges.size() : 0);
    const bool hasBadges = !this->badges_.isEmpty();

    this->addCollapsibleSection(
        QStringLiteral("global"),
        badgeSectionTitle(QStringLiteral("Global Badges"), globalBadges.size()),
        [this, globalBadges](QVBoxLayout *sectionLayout) {
            sectionLayout->addWidget(makeBadgePickerText(
                this->contentWidget_,
                QStringLiteral(
                    "This badge appears across channels and whispers."),
                false));
            this->addBadgeGrid(sectionLayout, globalBadges,
                               BadgeGridKind::Global);
        });

    addBadgePickerDivider(this->contentLayout_, this->contentWidget_);

    this->addCollapsibleSection(
        QStringLiteral("channel"),
        badgeSectionTitle(QStringLiteral("Channel Badges"), channelBadgeCount),
        [this, roleBadges, subscriberBadges,
         globalBadges](QVBoxLayout *sectionLayout) {
            sectionLayout->addWidget(makeBadgePickerText(
                this->contentWidget_,
                QStringLiteral(
                    "These badges appear on %1. Your role and subscriber badge "
                    "will always be shown.")
                    .arg(channelPossessiveForIdentity(this->channelName_)),
                false));

            auto *roleTitle =
                new QLabel(QStringLiteral("Role Badge"), this->contentWidget_);
            setBadgePickerSectionFont(roleTitle);
            sectionLayout->addWidget(roleTitle);

            if (roleBadges.isEmpty())
            {
                sectionLayout->addWidget(makeBadgePickerText(
                    this->contentWidget_,
                    QStringLiteral("No role badge available for this channel."),
                    true));
            }
            for (const auto &badge : roleBadges)
            {
                auto *rowWidget = new QWidget(this->contentWidget_);
                auto *rowLayout = new QHBoxLayout(rowWidget);
                rowLayout->setContentsMargins(0, 0, 0, 0);
                rowLayout->setSpacing(8);

                auto *icon = new QLabel(rowWidget);
                icon->setFixedSize(34, 34);
                icon->setAlignment(Qt::AlignCenter);
                icon->setStyleSheet(QStringLiteral(
                    "background:#111114;border:1px solid #2d2d35;"
                    "border-radius:3px;color:#dedee3;font-weight:700;"));
                this->updateBadgeIconLabel(icon, badge);
                this->badgeIconLabels_.push_back({icon, badge});
                rowLayout->addWidget(icon);
                rowLayout->addWidget(makeBadgePickerText(
                    rowWidget, roleBadgeText(badge, this->channelName_), false),
                                     1);
                sectionLayout->addWidget(rowWidget);
            }

            auto *subscriberTitle = new QLabel(
                QStringLiteral("Subscriber Badge"), this->contentWidget_);
            setBadgePickerSectionFont(subscriberTitle);
            sectionLayout->addWidget(subscriberTitle);

            if (subscriberBadges.isEmpty())
            {
                sectionLayout->addWidget(makeBadgePickerText(
                    this->contentWidget_,
                    QStringLiteral(
                        "No subscriber badge available for this channel."),
                    true));
            }
            for (const auto &badge : subscriberBadges)
            {
                auto *rowWidget = new QWidget(this->contentWidget_);
                auto *rowLayout = new QHBoxLayout(rowWidget);
                rowLayout->setContentsMargins(0, 0, 0, 0);
                rowLayout->setSpacing(8);

                auto *icon = new QLabel(rowWidget);
                icon->setFixedSize(34, 34);
                icon->setAlignment(Qt::AlignCenter);
                icon->setStyleSheet(QStringLiteral(
                    "background:#111114;border:1px solid #2d2d35;"
                    "border-radius:3px;color:#dedee3;font-weight:700;"));
                this->updateBadgeIconLabel(icon, badge);
                this->badgeIconLabels_.push_back({icon, badge});
                rowLayout->addWidget(icon);
                rowLayout->addWidget(makeBadgePickerText(
                    rowWidget, subscriberBadgeText(badge), false),
                                     1);
                sectionLayout->addWidget(rowWidget);
            }

            auto *flairToggle = new BadgeIdentityToggle(
                QStringLiteral("Hide Badge Flair"), this->contentWidget_);
            flairToggle->setCheckedInstantly(this->hideBadgeFlair_);
            QObject::connect(flairToggle, &QCheckBox::toggled, this,
                             [this](bool checked) {
                                 this->hideBadgeFlair_ = checked;
                                 twitch::setCurrentUserHidesBadgeFlair(
                                     this->channelName_, checked);
                             });
            sectionLayout->addWidget(flairToggle);
            sectionLayout->addWidget(makeBadgePickerText(
                this->contentWidget_,
                QStringLiteral("Badge Flair for Tier 2 and 3 Subscriptions"),
                false));

            auto *customToggle = new BadgeIdentityToggle(
                QStringLiteral("Use Custom Badge for This Channel"),
                this->contentWidget_);
            customToggle->setCheckedInstantly(this->useCustomChannelBadge_);
            QObject::connect(customToggle, &QCheckBox::toggled, this,
                             [this](bool checked) {
                                 const int scrollValue =
                                     this->scrollArea_ != nullptr &&
                                             this->scrollArea_
                                                     ->verticalScrollBar() !=
                                                 nullptr
                                         ? this->scrollArea_->verticalScrollBar()
                                               ->value()
                                         : 0;
                                 this->useCustomChannelBadge_ = checked;
                                 twitch::setCurrentUserUsesCustomChannelBadge(
                                     this->channelName_, checked);
                                 this->updatePreview();
                                 this->rebuildGrid();
                                 auto restoreScroll =
                                     [guard =
                                          QPointer<StreamDatabaseBadgePickerPopup>(
                                              this),
                                      scrollValue] {
                                         if (guard == nullptr ||
                                             guard->scrollArea_ == nullptr ||
                                             guard->scrollArea_
                                                     ->verticalScrollBar() ==
                                                 nullptr)
                                         {
                                             return;
                                         }

                                         auto *bar =
                                             guard->scrollArea_
                                                 ->verticalScrollBar();
                                         bar->setValue(std::min(
                                             scrollValue, bar->maximum()));
                                     };
                                 restoreScroll();
                                 QTimer::singleShot(0, this, restoreScroll);
                                 QTimer::singleShot(80, this, restoreScroll);
                                 if (this->appliedCallback_)
                                 {
                                     this->appliedCallback_();
                                 }
                             });
            sectionLayout->addWidget(customToggle);
            sectionLayout->addWidget(makeBadgePickerText(
                this->contentWidget_,
                QStringLiteral(
                    "Use a badge that is different from your default global "
                    "badge for %1.")
                    .arg(channelPossessiveForIdentity(this->channelName_)),
                false));

            if (this->useCustomChannelBadge_)
            {
                auto *customTitle = new QLabel(
                    QStringLiteral("Customizable Badge"), this->contentWidget_);
                setBadgePickerSectionFont(customTitle);
                sectionLayout->addWidget(customTitle);
                this->addBadgeGrid(sectionLayout, globalBadges,
                                   BadgeGridKind::ChannelCustom);
            }
        });

    addBadgePickerDivider(this->contentLayout_, this->contentWidget_);

    this->addCollapsibleSection(
        QStringLiteral("color"), QStringLiteral("Global Name Color"),
        [this](QVBoxLayout *sectionLayout) {
            sectionLayout->addWidget(makeBadgePickerText(
                this->contentWidget_,
                QStringLiteral(
                    "Pick a color, any color! It may take several minutes for "
                    "your color to update in the chat room."),
                false));

            const std::array<QString, 14> colors{
                QStringLiteral("#ff1f23"), QStringLiteral("#1f2fff"),
                QStringLiteral("#008000"), QStringLiteral("#b22222"),
                QStringLiteral("#ff7f50"), QStringLiteral("#9acd32"),
                QStringLiteral("#ff4500"), QStringLiteral("#2e8b57"),
                QStringLiteral("#daa520"), QStringLiteral("#d2691e"),
                QStringLiteral("#5f9ea0"), QStringLiteral("#1e90ff"),
                QStringLiteral("#ff69b4"), QStringLiteral("#8a2be2"),
            };

            auto *colorGrid = new QWidget(this->contentWidget_);
            auto *colorLayout = new QGridLayout(colorGrid);
            colorLayout->setContentsMargins(0, 2, 0, 0);
            colorLayout->setHorizontalSpacing(11);
            colorLayout->setVerticalSpacing(9);
            int row = 0;
            int column = 0;
            for (const auto &color : colors)
            {
                auto *swatch = new QToolButton(colorGrid);
                swatch->setObjectName(QStringLiteral("BadgeIdentityColorSwatch"));
                swatch->setCheckable(true);
                swatch->setChecked(color == this->selectedColor_);
                swatch->setCursor(Qt::PointingHandCursor);
                swatch->setFixedSize(30, 30);
                swatch->setStyleSheet(QStringLiteral(
                                           "QToolButton#BadgeIdentityColorSwatch "
                                           "{ background: %1; border-radius: "
                                           "15px; border: 2px solid %2; }"
                                           "QToolButton#BadgeIdentityColorSwatch:"
                                           "hover, "
                                           "QToolButton#BadgeIdentityColorSwatch:"
                                           "checked "
                                           "{ border-color: #efeff1; }")
                                           .arg(color,
                                                color == this->selectedColor_
                                                    ? QStringLiteral("#efeff1")
                                                    : QStringLiteral("#2f2f35")));
                QObject::connect(swatch, &QToolButton::clicked, this,
                                 [this, color] {
                                     this->applyNameColor(color);
                                 });
                colorLayout->addWidget(swatch, row, column);
                if (++column >= 7)
                {
                    column = 0;
                    ++row;
                }
            }
            sectionLayout->addWidget(colorGrid);

            auto *moreRow = new QWidget(this->contentWidget_);
            auto *moreLayout = new QHBoxLayout(moreRow);
            moreLayout->setContentsMargins(4, 2, 0, 0);
            moreLayout->setSpacing(9);
            auto *moreDot = new QWidget(moreRow);
            moreDot->setAttribute(Qt::WA_StyledBackground);
            moreDot->setFixedSize(30, 30);
            moreDot->setStyleSheet(QStringLiteral(
                "background: #00ff7f; border-radius: 15px; "
                "border: 2px solid #2f2f35;"));
            moreLayout->addWidget(moreDot, 0, Qt::AlignVCenter);
            auto *moreButton =
                new QPushButton(QStringLiteral("More colors"), moreRow);
            moreButton->setObjectName(QStringLiteral("BadgeIdentityLinkButton"));
            moreButton->setCursor(Qt::PointingHandCursor);
            moreButton->setFixedHeight(30);
            QObject::connect(moreButton, &QPushButton::clicked, this, [] {
                QDesktopServices::openUrl(QUrl(QStringLiteral(
                    "https://www.twitch.tv/settings/turbo")));
            });
            moreLayout->addWidget(moreButton, 1, Qt::AlignVCenter);
            sectionLayout->addWidget(moreRow);
        });

    this->contentLayout_->addStretch(1);
    this->emptyLabel_->setVisible(!hasBadges);
    this->syncBadgeButtonStates();
}

void StreamDatabaseBadgePickerPopup::addCollapsibleSection(
    const QString &key, const QString &title,
    const std::function<void(QVBoxLayout *)> &builder)
{
    const bool collapsed = this->collapsedSections_.value(key, false);
    auto *header = new QToolButton(this->contentWidget_);
    header->setObjectName(QStringLiteral("BadgeIdentitySectionHeader"));
    header->setCursor(Qt::PointingHandCursor);
    header->setText(title);
    header->setIcon(badgeIdentityChevronIcon(collapsed));
    header->setIconSize(QSize{18, 18});
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    QObject::connect(header, &QToolButton::clicked, this, [this, key] {
        this->collapsedSections_.insert(
            key, !this->collapsedSections_.value(key, false));
        this->rebuildGrid();
    });
    this->contentLayout_->addWidget(header);

    if (collapsed)
    {
        return;
    }

    auto *sectionWidget = new QWidget(this->contentWidget_);
    auto *sectionLayout = new QVBoxLayout(sectionWidget);
    sectionLayout->setContentsMargins(0, 0, 0, 0);
    sectionLayout->setSpacing(8);
    builder(sectionLayout);
    this->contentLayout_->addWidget(sectionWidget);
}

void StreamDatabaseBadgePickerPopup::addBadgeGrid(QVBoxLayout *layout,
                                                  const QVector<BadgeItem> &badges,
                                                  BadgeGridKind kind)
{
    if (badges.isEmpty())
    {
        layout->addWidget(makeBadgePickerText(
            this->contentWidget_, QStringLiteral("No badges available."), true));
        return;
    }

    auto *gridWidget = new QWidget(this->contentWidget_);
    auto *grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 3, 0, 0);
    grid->setHorizontalSpacing(7);
    grid->setVerticalSpacing(7);

    int row = 0;
    int column = 0;
    constexpr int columns = 7;
    for (const auto &badge : badges)
    {
        auto *button = new QToolButton(gridWidget);
        button->setObjectName(QStringLiteral("StreamDatabaseBadgeOption"));
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setFixedSize(34, 34);
        button->setIconSize(QSize{32, 32});
        button->setToolTip(
            badge.description.isEmpty()
                ? badge.title
                : QStringLiteral("%1\n%2").arg(badge.title, badge.description));
        this->updateButtonIcon(button, badge);

        QObject::connect(button, &QToolButton::clicked, this,
                         [this, badge, kind] {
                             this->applyBadgeSelection(badge, kind);
                         });

        grid->addWidget(button, row, column);
        this->badgeButtons_.push_back({button, badge, kind});

        if (++column >= columns)
        {
            column = 0;
            ++row;
        }
    }

    layout->addWidget(gridWidget);
}

void StreamDatabaseBadgePickerPopup::requestBadgeImages()
{
    for (const auto &badge : this->badges_)
    {
        if (badge.imageUrl.isEmpty() ||
            this->badgePixmaps_.contains(badge.imageUrl))
        {
            continue;
        }

        const auto alreadyPending =
            std::any_of(this->pendingBadgeRequests_.begin(),
                        this->pendingBadgeRequests_.end(),
                        [&badge](const QString &url) {
                            return url == badge.imageUrl;
                        });
        if (alreadyPending)
        {
            continue;
        }

        QNetworkRequest request(QUrl(badge.imageUrl));
        auto *reply = this->network_.get(request);
        this->pendingBadgeRequests_.insert(reply, badge.imageUrl);
        QObject::connect(reply, &QNetworkReply::finished, this,
                         [this, reply] {
                             this->handleBadgeImageFinished(reply);
                         });
    }
}

void StreamDatabaseBadgePickerPopup::handleBadgeImageFinished(
    QNetworkReply *reply)
{
    const QString url = this->pendingBadgeRequests_.take(reply);
    if (reply->error() == QNetworkReply::NoError)
    {
        QPixmap pixmap;
        pixmap.loadFromData(reply->readAll());
        if (!pixmap.isNull())
        {
            this->badgePixmaps_.insert(url, pixmap);
            for (const auto &row : this->badgeButtons_)
            {
                if (row.badge.imageUrl == url)
                {
                    this->updateButtonIcon(row.button, row.badge);
                }
            }
            for (const auto &row : this->badgeIconLabels_)
            {
                if (row.badge.imageUrl == url)
                {
                    this->updateBadgeIconLabel(row.label, row.badge);
                }
            }
            this->updatePreview();
        }
    }

    reply->deleteLater();
}

void StreamDatabaseBadgePickerPopup::applyNameColor(const QString &color)
{
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (account == nullptr || account->isAnon())
    {
        this->setEmptyText(QStringLiteral("Log in to Twitch to change color."));
        if (this->emptyLabel_ != nullptr)
        {
            this->emptyLabel_->show();
        }
        return;
    }

    const auto userID = account->getUserId().trimmed();
    if (userID.isEmpty())
    {
        this->setEmptyText(QStringLiteral("Missing Twitch user ID."));
        if (this->emptyLabel_ != nullptr)
        {
            this->emptyLabel_->show();
        }
        return;
    }

    const auto previousColor = this->selectedColor_;
    const int requestGeneration = ++this->nameColorRequestGeneration_;
    this->selectedColor_ = color;
    this->rebuildGrid();

    getHelix()->updateUserChatColor(
        userID, color,
        [guard = QPointer<StreamDatabaseBadgePickerPopup>(this), color,
         requestGeneration] {
            if (guard == nullptr ||
                requestGeneration != guard->nameColorRequestGeneration_)
            {
                return;
            }

            auto currentAccount = getApp()->getAccounts()->twitch.getCurrent();
            if (currentAccount != nullptr && !currentAccount->isAnon())
            {
                currentAccount->setColor(QColor(color));
            }
            if (guard->emptyLabel_ != nullptr && !guard->badges_.isEmpty())
            {
                guard->emptyLabel_->hide();
            }
        },
        [guard = QPointer<StreamDatabaseBadgePickerPopup>(this), previousColor,
         requestGeneration](auto error, const QString &message) {
            if (guard == nullptr ||
                requestGeneration != guard->nameColorRequestGeneration_)
            {
                return;
            }

            guard->selectedColor_ = previousColor;
            guard->rebuildGrid();
            guard->setEmptyText(nameColorErrorText(error, message));
            if (guard->emptyLabel_ != nullptr)
            {
                guard->emptyLabel_->show();
            }
        });
}

void StreamDatabaseBadgePickerPopup::requestCurrentUserDisplayName()
{
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (account == nullptr || account->isAnon())
    {
        return;
    }

    const auto userID = account->getUserId().trimmed();
    if (userID.isEmpty())
    {
        return;
    }

    const int requestGeneration = ++this->displayNameRequestGeneration_;
    getHelix()->getUserById(
        userID,
        [guard = QPointer<StreamDatabaseBadgePickerPopup>(this),
         requestGeneration](const HelixUser &user) {
            if (guard == nullptr ||
                requestGeneration != guard->displayNameRequestGeneration_)
            {
                return;
            }

            const auto displayName = user.displayName.trimmed();
            if (displayName.isEmpty())
            {
                return;
            }

            guard->accountName_ = displayName;
            guard->previewName_->setText(displayName);
        },
        [] {});
}

void StreamDatabaseBadgePickerPopup::applyBadgeSelection(
    const BadgeItem &badge, BadgeGridKind kind)
{
    const auto key = twitch::badgeIdentityKey(badge);
    if (key.isEmpty() || this->pendingSelectionRequest_ != nullptr)
    {
        return;
    }

    const bool channelSpecific = kind == BadgeGridKind::ChannelCustom;
    if (channelSpecific && this->channelID_.isEmpty())
    {
        this->setEmptyText(QStringLiteral("Missing Twitch channel ID."));
        return;
    }

    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (account == nullptr || account->isAnon())
    {
        this->setEmptyText(QStringLiteral("Log in to Twitch to change badges."));
        return;
    }

    const auto badgeAuth =
        TwitchModerationAuth::resolveForCurrentUser(account->getUserId());
    if (!badgeAuth.supportsWebGql())
    {
        showModerationAuthLoginPrompt(
            this, account->getUserId(),
            QStringLiteral("change Twitch chat badges"),
            [guard = QPointer<StreamDatabaseBadgePickerPopup>(this)] {
                if (guard != nullptr)
                {
                    guard->requestEvents();
                }
            });
        return;
    }

    if (channelSpecific)
    {
        this->useCustomChannelBadge_ = true;
        this->selectedCustomBadgeKey_ = key;
    }
    else
    {
        this->selectedGlobalBadgeKey_ = key;
    }
    this->syncBadgeButtonStates();

    this->pendingSelectionRequest_ = twitch::requestSelectCurrentUserBadgeIdentity(
        this->network_, this->channelID_, badge, badgeAuth, this,
        channelSpecific,
        [this, badge, channelSpecific] {
            this->pendingSelectionRequest_ = nullptr;
            twitch::setCurrentUserAppliedBadge(this->channelName_, badge,
                                               channelSpecific);
            if (this->appliedCallback_)
            {
                this->appliedCallback_();
            }
        },
        [this](const QString &error) {
            this->pendingSelectionRequest_ = nullptr;
            this->setEmptyText(error.isEmpty()
                                   ? QStringLiteral("Failed to change badge.")
                                   : error);
        });
}

void StreamDatabaseBadgePickerPopup::syncBadgeButtonStates()
{
    for (const auto &row : this->badgeButtons_)
    {
        if (row.button == nullptr)
        {
            continue;
        }

        const QSignalBlocker blocker(row.button);
        const auto key = twitch::badgeIdentityKey(row.badge);
        row.button->setChecked(
            row.kind == BadgeGridKind::ChannelCustom
                ? key == this->selectedCustomBadgeKey_
                : key == this->selectedGlobalBadgeKey_);
    }

    this->updatePreview();
}

void StreamDatabaseBadgePickerPopup::updateButtonIcon(
    QToolButton *button, const BadgeItem &badge) const
{
    if (button == nullptr)
    {
        return;
    }

    const auto pixmap = this->badgePixmaps_.value(badge.imageUrl);
    if (!pixmap.isNull())
    {
        button->setText(QString());
        button->setIcon(QIcon(pixmap));
        return;
    }

    button->setIcon(QIcon());
    button->setText(initialsForBadge(badge));
}

void StreamDatabaseBadgePickerPopup::updateBadgeIconLabel(
    QLabel *label, const BadgeItem &badge) const
{
    if (label == nullptr)
    {
        return;
    }

    const auto pixmap = this->badgePixmaps_.value(badge.imageUrl);
    if (!pixmap.isNull())
    {
        label->setText(QString());
        label->setPixmap(pixmap.scaled(QSize{32, 32}, Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation));
        return;
    }

    label->setPixmap(QPixmap());
    label->setText(initialsForBadge(badge));
}

void StreamDatabaseBadgePickerPopup::updatePreview()
{
    clearBadgePickerLayout(this->previewBadgesLayout_);

    QSet<QString> previewKeys;
    auto addPreviewBadge = [this, &previewKeys](const BadgeItem *badge) {
        if (badge == nullptr)
        {
            return;
        }

        const auto key = twitch::badgeIdentityKey(*badge);
        if (key.isEmpty() || previewKeys.contains(key))
        {
            return;
        }
        previewKeys.insert(key);

        auto *label = new QLabel(this->previewBadgesWidget_);
        label->setFixedSize(22, 22);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet(QStringLiteral(
            "background:#111114;border:1px solid #2d2d35;border-radius:3px;"
            "color:#dedee3;font-weight:700;font-size:9px;"));
        const auto pixmap = this->badgePixmaps_.value(badge->imageUrl);
        if (!pixmap.isNull())
        {
            label->setText(QString());
            label->setPixmap(pixmap.scaled(
                QSize{20, 20}, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
        else
        {
            label->setText(initialsForBadge(*badge));
        }
        this->previewBadgesLayout_->addWidget(label);
    };

    for (const auto &badge : this->badges_)
    {
        if (badge.displayed && (isRoleBadge(badge) || isSubscriberBadge(badge)))
        {
            addPreviewBadge(&badge);
        }
    }

    const auto *customBadge = this->badgeForKey(this->selectedCustomBadgeKey_);
    const auto *globalBadge = this->badgeForKey(this->selectedGlobalBadgeKey_);
    addPreviewBadge(this->useCustomChannelBadge_ && customBadge != nullptr
                        ? customBadge
                        : globalBadge);

    if (previewKeys.isEmpty())
    {
        addPreviewBadge(globalBadge);
    }

    this->previewBadgesLayout_->addStretch(1);
}

QVector<StreamDatabaseBadgePickerPopup::BadgeItem>
    StreamDatabaseBadgePickerPopup::globalBadges() const
{
    QVector<BadgeItem> result;
    QSet<QString> seen;
    for (const auto &badge : this->badges_)
    {
        if (!isGlobalBadge(badge))
        {
            continue;
        }

        const auto key = twitch::badgeIdentityKey(badge);
        if (key.isEmpty() || seen.contains(key))
        {
            continue;
        }
        seen.insert(key);
        result.push_back(badge);
    }
    return result;
}

QVector<StreamDatabaseBadgePickerPopup::BadgeItem>
    StreamDatabaseBadgePickerPopup::roleBadges() const
{
    QVector<BadgeItem> result;
    QSet<QString> seen;
    for (const auto &badge : this->badges_)
    {
        if (!isRoleBadge(badge))
        {
            continue;
        }

        const auto key = twitch::badgeIdentityKey(badge);
        if (key.isEmpty() || seen.contains(key))
        {
            continue;
        }
        seen.insert(key);
        result.push_back(badge);
    }
    return result;
}

QVector<StreamDatabaseBadgePickerPopup::BadgeItem>
    StreamDatabaseBadgePickerPopup::subscriberBadges() const
{
    QVector<BadgeItem> result;
    QSet<QString> seen;
    for (const auto &badge : this->badges_)
    {
        if (!isSubscriberBadge(badge))
        {
            continue;
        }

        const auto key = twitch::badgeIdentityKey(badge);
        if (key.isEmpty() || seen.contains(key))
        {
            continue;
        }
        seen.insert(key);
        result.push_back(badge);
    }
    return result;
}

const StreamDatabaseBadgePickerPopup::BadgeItem *
    StreamDatabaseBadgePickerPopup::badgeForKey(const QString &key) const
{
    if (key.isEmpty())
    {
        return nullptr;
    }

    for (const auto &badge : this->badges_)
    {
        if (twitch::badgeIdentityKey(badge) == key)
        {
            return &badge;
        }
    }
    return nullptr;
}

void StreamDatabaseBadgePickerPopup::chooseDefaultSelections()
{
    const auto globalBadges = this->globalBadges();
    auto chooseGlobal = [&globalBadges](const QString &currentKey) {
        if (!currentKey.isEmpty())
        {
            for (const auto &badge : globalBadges)
            {
                if (twitch::badgeIdentityKey(badge) == currentKey)
                {
                    return currentKey;
                }
            }
        }

        const auto selected = std::find_if(
            globalBadges.begin(), globalBadges.end(),
            [](const auto &badge) {
                return badge.selected || badge.displayed;
            });
        if (selected != globalBadges.end())
        {
            return twitch::badgeIdentityKey(*selected);
        }
        if (!globalBadges.isEmpty())
        {
            return twitch::badgeIdentityKey(globalBadges.front());
        }
        return QString{};
    };

    this->selectedGlobalBadgeKey_ =
        chooseGlobal(this->selectedGlobalBadgeKey_);
    this->selectedCustomBadgeKey_ =
        chooseGlobal(this->selectedCustomBadgeKey_.isEmpty()
                         ? this->selectedGlobalBadgeKey_
                         : this->selectedCustomBadgeKey_);
}

void StreamDatabaseBadgePickerPopup::setEmptyText(const QString &text)
{
    if (this->emptyLabel_ != nullptr)
    {
        this->emptyLabel_->setText(text);
    }
}

class PlatformSwitchButton : public Button
{
public:
    explicit PlatformSwitchButton(BaseWidget *parent = nullptr)
        : Button(parent)
        , twitchRenderer_(u":/platforms/twitch.svg"_s, this)
        , kickRenderer_(u":/platforms/kick.svg"_s, this)
        , youtubeRenderer_(u":/platforms/youtube.svg"_s, this)
        , animation_(this)
    {
        this->setContentCacheEnabled(false);

        this->animation_.setDuration(170);
        this->animation_.setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(&this->animation_, &QVariantAnimation::valueChanged,
                         this, [this](const QVariant &value) {
                             this->animationProgress_ = value.toReal();
                             this->update();
                         });
        QObject::connect(&this->animation_, &QVariantAnimation::finished,
                         this, [this] {
                             this->animationProgress_ = 1.0;
                             this->update();
                         });
    }

    void setPlatforms(std::vector<MessagePlatform> platforms, bool animate)
    {
        if (platforms == this->currentPlatforms_)
        {
            return;
        }

        this->previousPlatforms_ = this->currentPlatforms_;
        this->currentPlatforms_ = std::move(platforms);

        this->animation_.stop();
        if (animate && this->isVisible())
        {
            this->animationProgress_ = 0.0;
            this->animation_.setStartValue(0.0);
            this->animation_.setEndValue(1.0);
            this->animation_.start();
            return;
        }

        this->animationProgress_ = 1.0;
        this->update();
    }

    void setContentOffset(QPoint offset)
    {
        if (this->contentOffset_ == offset)
        {
            return;
        }

        this->contentOffset_ = offset;
        this->update();
    }

protected:
    void paintContent(QPainter &painter) override
    {
        const auto contentSize = this->scale() * QSize{16, 16};
        const QPointF topLeft{
            (this->width() - contentSize.width()) / 2.0,
            (this->height() - contentSize.height()) / 2.0};
        auto bounds = QRectF{topLeft, contentSize};
        bounds.translate(this->scale() * this->contentOffset_);

        painter.save();
        painter.setClipRect(this->rect());

        if (this->animationProgress_ >= 1.0)
        {
            this->renderPlatforms(painter, this->currentPlatforms_, bounds, 1.0,
                                  1.0);
        }
        else if (this->previousPlatforms_.size() == 1 &&
                 this->currentPlatforms_.size() == 1)
        {
            const auto slide = bounds.width() * this->animationProgress_;
            this->renderPlatform(
                painter, this->previousPlatforms_.front(),
                bounds.translated(slide, 0), 1.0 - this->animationProgress_);
            this->renderPlatform(
                painter, this->currentPlatforms_.front(),
                bounds.translated(slide - bounds.width(), 0),
                this->animationProgress_);
        }
        else
        {
            this->renderPlatforms(painter, this->previousPlatforms_, bounds,
                                  1.0 - this->animationProgress_,
                                  1.0 - this->animationProgress_);
            this->renderPlatforms(painter, this->currentPlatforms_, bounds,
                                  this->animationProgress_,
                                  this->animationProgress_);
        }

        painter.restore();
    }

private:
    QSvgRenderer &rendererFor(MessagePlatform platform)
    {
        switch (platform)
        {
            case MessagePlatform::Kick:
                return this->kickRenderer_;
            case MessagePlatform::YouTube:
                return this->youtubeRenderer_;
            case MessagePlatform::AnyOrTwitch:
            default:
                return this->twitchRenderer_;
        }
    }

    void renderPlatform(QPainter &painter, MessagePlatform platform,
                        const QRectF &bounds, qreal opacity)
    {
        const qreal dpr = this->devicePixelRatioF();
        QSize imageSize{
            std::max(1, static_cast<int>(std::ceil(bounds.width() * dpr))),
            std::max(1, static_cast<int>(std::ceil(bounds.height() * dpr)))};

        QImage image(imageSize, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);

        QPainter imagePainter(&image);
        const QRectF imageBounds{QPointF{0, 0}, bounds.size()};
        this->rendererFor(platform).render(&imagePainter, imageBounds);
        imagePainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        imagePainter.fillRect(imageBounds, QColor(232, 232, 232));
        imagePainter.end();

        painter.save();
        painter.setOpacity(opacity);
        painter.drawImage(bounds.topLeft(), image);
        painter.restore();
    }

    void renderPlus(QPainter &painter, const QPointF &center, qreal opacity)
    {
        if (opacity <= 0.0)
        {
            return;
        }

        painter.save();
        painter.setOpacity(opacity);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const auto color = QColor(232, 232, 232);
        const qreal half = 2.2 * this->scale();
        const qreal width = std::max<qreal>(1.0, 1.2 * this->scale());
        painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF{center.x() - half, center.y()},
                         QPointF{center.x() + half, center.y()});
        painter.drawLine(QPointF{center.x(), center.y() - half},
                         QPointF{center.x(), center.y() + half});
        painter.restore();
    }

    void renderPlatforms(QPainter &painter,
                         const std::vector<MessagePlatform> &platforms,
                         const QRectF &bounds, qreal opacity, qreal spread)
    {
        if (platforms.empty() || opacity <= 0.0)
        {
            return;
        }

        if (platforms.size() == 1)
        {
            this->renderPlatform(painter, platforms.front(), bounds, opacity);
            return;
        }

        const auto center = bounds.center();
        const auto iconSize = bounds.size();
        const qreal plusWidth = 6.0 * this->scale();
        const qreal gap = 3.0 * this->scale();
        const qreal step = iconSize.width() + plusWidth + (gap * 2.0);
        const qreal groupWidth =
            (iconSize.width() * static_cast<qreal>(platforms.size())) +
            ((plusWidth + (gap * 2.0)) *
             static_cast<qreal>(platforms.size() - 1));
        const qreal firstCenterX =
            center.x() - (groupWidth / 2.0) + (iconSize.width() / 2.0);

        for (size_t i = 0; i < platforms.size(); ++i)
        {
            const qreal targetCenterX =
                firstCenterX + (step * static_cast<qreal>(i));
            const qreal currentCenterX =
                center.x() + ((targetCenterX - center.x()) * spread);
            QRectF iconBounds{
                QPointF{currentCenterX - (iconSize.width() / 2.0),
                        center.y() - (iconSize.height() / 2.0)},
                iconSize,
            };
            this->renderPlatform(painter, platforms[i], iconBounds, opacity);

            if (i + 1 < platforms.size())
            {
                const qreal targetPlusX = targetCenterX +
                                          (iconSize.width() / 2.0) + gap +
                                          (plusWidth / 2.0);
                const qreal currentPlusX =
                    center.x() + ((targetPlusX - center.x()) * spread);
                this->renderPlus(painter, QPointF{currentPlusX, center.y()},
                                 opacity * spread);
            }
        }
    }

    std::vector<MessagePlatform> currentPlatforms_{
        MessagePlatform::AnyOrTwitch};
    std::vector<MessagePlatform> previousPlatforms_{
        MessagePlatform::AnyOrTwitch};
    QSvgRenderer twitchRenderer_;
    QSvgRenderer kickRenderer_;
    QSvgRenderer youtubeRenderer_;
    QVariantAnimation animation_;
    QPoint contentOffset_;
    qreal animationProgress_ = 1.0;
};

SplitInput::SplitInput(Split *_chatWidget, bool enableInlineReplying)
    : SplitInput(_chatWidget, _chatWidget, _chatWidget->view_,
                 enableInlineReplying)
{
}

SplitInput::SplitInput(QWidget *parent, Split *_chatWidget,
                       ChannelView *_channelView, bool enableInlineReplying)
    : BaseWidget(parent)
    , split_(_chatWidget)
    , channelView_(_channelView)
    , enableInlineReplying_(enableInlineReplying)
    , backgroundColorAnimation(this, "backgroundColor"_ba)
    , badgeButtonVisibilityAnimation_(this)
{
    this->installEventFilter(this);
    this->initLayout();

    this->badgeButtonVisibilityAnimation_.setDuration(160);
    this->badgeButtonVisibilityAnimation_.setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(&this->badgeButtonVisibilityAnimation_,
                     &QVariantAnimation::valueChanged, this,
                     [this](const QVariant &value) {
                         const auto progress =
                             std::clamp(value.toReal(), 0.0, 1.0);
                         const int width = int(std::round(
                             this->badgeButtonTargetWidth() * progress));
                         if (this->ui_.badgeButtonWrapper != nullptr)
                         {
                             this->ui_.badgeButtonWrapper->setFixedWidth(width);
                         }
                         if (this->ui_.badgeButton != nullptr)
                         {
                             if (auto *effect =
                                     qobject_cast<QGraphicsOpacityEffect *>(
                                         this->ui_.badgeButton
                                             ->graphicsEffect()))
                             {
                                 effect->setOpacity(progress);
                             }
                         }
                     });
    QObject::connect(&this->badgeButtonVisibilityAnimation_,
                     &QVariantAnimation::finished, this, [this] {
                         if (this->ui_.badgeButtonWrapper == nullptr ||
                             this->ui_.badgeButton == nullptr)
                         {
                             return;
                         }

                         const int targetWidth = this->badgeButtonTargetWidth();
                         if (this->badgeButtonShown_)
                         {
                             this->ui_.badgeButtonWrapper->show();
                             this->ui_.badgeButtonWrapper->setFixedWidth(
                                 targetWidth);
                             this->ui_.badgeButton->setEnabled(true);
                         }
                         else
                         {
                             this->ui_.badgeButtonWrapper->setFixedWidth(0);
                             this->ui_.badgeButtonWrapper->hide();
                             this->ui_.badgeButton->setEnabled(false);
                         }

                         if (auto *effect =
                                 qobject_cast<QGraphicsOpacityEffect *>(
                                     this->ui_.badgeButton->graphicsEffect()))
                         {
                             effect->setOpacity(this->badgeButtonShown_ ? 1.0
                                                                        : 0.0);
                         }
                     });

    this->ui_.textEdit->setCompleter(
        this->createCompleter(this->split_->getChannel()));

    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
    auto *spellChecker = getApp()->getSpellChecker();
    this->inputHighlighter = new InputHighlighter(*spellChecker, this);
    this->inputHighlighter->setChannel(this->split_->getChannel());

    this->signalHolder_.managedConnect(this->split_->channelChanged, [this] {
        auto channel = this->split_->getChannel();
        this->ui_.textEdit->setCompleter(this->createCompleter(channel));
        this->inputHighlighter->setChannel(this->split_->getChannel());
        this->updatePlatformSelector();
        this->updateBadgeButton();
        this->updateBadgePickerContext();
        this->updatePollPredictionButtons();
        this->updateEmotePopupChannel();
    });
    this->signalHolder_.managedConnect(this->sendPlatformChanged, [this] {
        this->ui_.textEdit->resetCompletion();
        this->updateEmotePopupChannel();
        this->updateBadgeButton();
        this->updateBadgePickerContext();
        this->updatePollPredictionButtons();
        this->updateCompletionPopup();
    });
    this->managedConnections_.managedConnect(
        TwitchModerationAuth::accountChanged(), [this] {
            this->resetBadgeIdentityButtonFetch(false);
            this->updatePollPredictionButtons();
            this->updateBadgeButton();
        });
    this->managedConnections_.managedConnect(
        twitch::streamDatabaseBadgeOwnershipChanged(), [this] {
            this->updateBadgeButton();
        });

    auto *pollPredictionButtonTimer = new QTimer(this);
    pollPredictionButtonTimer->setInterval(1000);
    QObject::connect(pollPredictionButtonTimer, &QTimer::timeout, this, [this] {
        this->updatePollPredictionButtons();
    });
    pollPredictionButtonTimer->start();

    getSettings()->enableSpellChecking.connect(
        [this] {
            this->checkSpellingChanged();
        },
        this->signalHolder_);

    // misc
    this->installTextEditEvents();
    this->addShortcuts();
    // The textEdit's signal will be destroyed before this SplitInput is
    // destroyed, so we can safely ignore this signal's connection.
    std::ignore = this->ui_.textEdit->focusLost.connect([this] {
        this->hideCompletionPopup();
    });
    this->scaleChangedEvent(this->scale());
    this->updatePlatformSelector();
    this->updatePollPredictionButtons();
    this->signalHolder_.managedConnect(getApp()->getHotkeys()->onItemsUpdated,
                                       [this]() {
                                           this->clearShortcuts();
                                           this->addShortcuts();
                                       });

    QEasingCurve curve;
    curve.setCustomType(highlightEasingFunction);
    this->backgroundColorAnimation.setDuration(500);
    this->backgroundColorAnimation.setEasingCurve(curve);
}

void SplitInput::initLayout()
{
    auto *app = getApp();
    LayoutCreator<SplitInput> layoutCreator(this);

    auto layout =
        layoutCreator.setLayoutType<QVBoxLayout>().withoutMargin().assign(
            &this->ui_.vbox);
    layout->setSpacing(0);
    this->applyOuterMargin();

    // reply label stuff
    auto replyWrapper =
        layout.emplace<QWidget>().assign(&this->ui_.replyWrapper);
    replyWrapper->setContentsMargins(0, 0, 1, 1);

    auto replyVbox =
        replyWrapper.setLayoutType<QVBoxLayout>().withoutMargin().assign(
            &this->ui_.replyVbox);
    replyVbox->setSpacing(1);

    auto replyHbox =
        replyVbox.emplace<QHBoxLayout>().assign(&this->ui_.replyHbox);

    auto *messageVbox = new QVBoxLayout;
    this->ui_.replyMessage = new MessageView();
    messageVbox->addWidget(this->ui_.replyMessage, 0, Qt::AlignLeft);
    messageVbox->setContentsMargins(10, 0, 0, 0);
    replyVbox->addLayout(messageVbox, 0);

    auto replyLabel = replyHbox.emplace<QLabel>().assign(&this->ui_.replyLabel);
    replyLabel->setAlignment(Qt::AlignLeft);
    replyLabel->setFont(
        app->getFonts()->getFont(FontStyle::ChatMedium, this->scale()));

    replyHbox->addStretch(1);

    auto replyCancelButton = replyHbox
                                 .emplace<SvgButton>(
                                     SvgButton::Src{
                                         .dark = ":/buttons/cancel.svg",
                                         .light = ":/buttons/cancelDark.svg",
                                     },
                                     nullptr, QSize{4, 0})
                                 .assign(&this->ui_.cancelReplyButton);

    replyCancelButton->hide();
    replyLabel->hide();

    auto inputWrapper =
        layout.emplace<QWidget>().assign(&this->ui_.inputWrapper);
    inputWrapper->setContentsMargins(1, 1, 1, 1);

    // hbox for input, right box
    auto hboxLayout =
        inputWrapper.setLayoutType<QHBoxLayout>().withoutMargin().assign(
            &this->ui_.inputHbox);
    this->ui_.inputHbox->setSpacing(0);

    this->ui_.badgeButtonWrapper = new QWidget(this->ui_.inputWrapper);
    this->ui_.badgeButtonWrapper->setSizePolicy(QSizePolicy::Fixed,
                                                QSizePolicy::Preferred);
    auto *badgeLayout = new QHBoxLayout(this->ui_.badgeButtonWrapper);
    badgeLayout->setContentsMargins(0, 0, 0, 0);
    badgeLayout->setSpacing(0);
    this->ui_.inputHbox->addWidget(this->ui_.badgeButtonWrapper, 0,
                                   Qt::AlignVCenter);

    this->ui_.badgeButton = new ChatIdentityButton(this->ui_.badgeButtonWrapper);
    this->ui_.badgeButton->setCursor(Qt::PointingHandCursor);
    this->ui_.badgeButton->setFocusPolicy(Qt::NoFocus);
    this->ui_.badgeButton->setToolTip(QStringLiteral("Chat identity"));
    auto *badgeOpacity = new QGraphicsOpacityEffect(this->ui_.badgeButton);
    badgeOpacity->setOpacity(1.0);
    this->ui_.badgeButton->setGraphicsEffect(badgeOpacity);
    badgeLayout->addWidget(this->ui_.badgeButton, 0,
                           Qt::AlignLeft | Qt::AlignVCenter);

    // input
    auto textEdit =
        hboxLayout.emplace<ResizingTextEdit>().assign(&this->ui_.textEdit);
    connect(textEdit.getElement(), &ResizingTextEdit::textChanged, this,
            &SplitInput::editTextChanged);
    textEdit->setFrameStyle(QFrame::NoFrame);

    auto *shortcutFilter = new CmdDeleteKeyFilter(this);
    textEdit->installEventFilter(shortcutFilter);

    hboxLayout.emplace<LabelButton>("SEND").assign(&this->ui_.sendButton);
    this->ui_.sendButton->hide();

    QObject::connect(this->ui_.sendButton, &Button::leftClicked, [this] {
        std::vector<QString> arguments;
        this->handleSendMessage(arguments);
    });

    getSettings()->showSendButton.connect(
        [this](const bool value, auto) {
            if (value)
            {
                this->ui_.sendButton->show();
            }
            else
            {
                this->ui_.sendButton->hide();
            }
        },
        this->managedConnections_);

    // right box
    auto box =
        hboxLayout.emplace<QVBoxLayout>().withoutMargin().assign(
            &this->ui_.rightVbox);
    box->setSpacing(0);
    box->setAlignment(Qt::AlignVCenter);
    {
        auto hbox = box.emplace<QHBoxLayout>().withoutMargin();
        this->ui_.textEditLength = new QLabel();
        // Right-align the labels contents
        this->ui_.textEditLength->setAlignment(Qt::AlignRight);
        this->ui_.textEditLength->setHidden(true);
        hbox->addWidget(this->ui_.textEditLength);

        this->ui_.sendWaitStatus = new QLabel();
        this->ui_.sendWaitStatus->setAlignment(Qt::AlignRight);
        this->ui_.sendWaitStatus->setHidden(true);
        hbox->addWidget(this->ui_.sendWaitStatus);

        auto buttonHbox =
            box.emplace<QHBoxLayout>().withoutMargin().assign(
                &this->ui_.buttonHbox);
        buttonHbox->setSpacing(0);

        this->ui_.platformButton = new PlatformSwitchButton();
        this->ui_.platformButton->setPaintOffset(QPoint{0, 0});
        buttonHbox->addWidget(this->ui_.platformButton);

        this->ui_.predictionButton = new SvgButton(
            {
                .dark = ":/buttons/startPrediction.svg",
                .light = ":/buttons/startPrediction.svg",
            },
            nullptr, QSize{4, 1});
        this->ui_.predictionButton->setContentSize(QSize{18, 18});
        this->ui_.predictionButton->setPaintOffset(QPoint{0, 0});
        this->ui_.predictionButton->setToolTip(
            QStringLiteral("Start prediction"));
        this->ui_.predictionButton->installEventFilter(this);
        buttonHbox->addWidget(this->ui_.predictionButton);

        this->ui_.pollButton = new SvgButton(
            {
                .dark = ":/buttons/startPoll.svg",
                .light = ":/buttons/startPoll.svg",
            },
            nullptr, QSize{4, 1});
        this->ui_.pollButton->setContentSize(QSize{22, 22});
        this->ui_.pollButton->setPaintOffset(QPoint{0, 0});
        this->ui_.pollButton->setToolTip(QStringLiteral("Start poll"));
        this->ui_.pollButton->installEventFilter(this);
        buttonHbox->addWidget(this->ui_.pollButton);

        this->ui_.emoteButton = new SvgButton(
            {
                .dark = ":/buttons/emote.svg",
                .light = ":/buttons/emoteDark.svg",
            },
            nullptr, QSize{4, 1});
        this->ui_.emoteButton->setContentSize(QSize{16, 16});
        this->ui_.emoteButton->setPaintOffset(QPoint{0, 0});
        buttonHbox->addWidget(this->ui_.emoteButton);
        buttonHbox->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }

    // ---- misc

    // set edit font
    this->ui_.textEdit->setFont(
        app->getFonts()->getFont(FontStyle::ChatMedium, this->scale()));
    QObject::connect(this->ui_.textEdit, &QTextEdit::cursorPositionChanged,
                     this, &SplitInput::onCursorPositionChanged);
    QObject::connect(this->ui_.textEdit, &QTextEdit::textChanged, this,
                     &SplitInput::onTextChanged);

    this->managedConnections_.managedConnect(app->getFonts()->fontChanged,
                                             [this] {
                                                 this->updateFonts();
                                             });

    // open emote popup
    if (this->ui_.badgeButton != nullptr)
    {
        QObject::connect(this->ui_.badgeButton, &QToolButton::clicked, this,
                         [this] {
                             this->openBadgePickerPopup();
                         });
    }
    QObject::connect(this->ui_.emoteButton, &Button::leftClicked, [this] {
        this->openEmotePopup();
    });
    QObject::connect(this->ui_.predictionButton, &Button::leftClicked, [this] {
        this->updatePollPredictionButtons();
        this->openPredictionDialog();
    });
    QObject::connect(this->ui_.pollButton, &Button::leftClicked, [this] {
        this->updatePollPredictionButtons();
        this->openPollDialog();
    });

    QObject::connect(this->ui_.platformButton, &Button::leftClicked, [this] {
        const auto platforms = this->availableSendPlatforms();
        if (platforms.size() >= 3)
        {
            this->showPlatformSelectionMenu();
            return;
        }

        this->tryCycleSendPlatform();
    });

    // clear input and remove reply thread
    QObject::connect(this->ui_.cancelReplyButton, &Button::leftClicked, [this] {
        this->setReply(nullptr);
    });

    // Forward selection change signal
    QObject::connect(this->ui_.textEdit, &QTextEdit::copyAvailable,
                     [this](bool available) {
                         if (available)
                         {
                             this->selectionChanged.invoke();
                         }
                     });

    // textEditLength visibility
    getSettings()->showMessageLength.connect(
        [this](const bool &value, auto) {
            // this->ui_.textEditLength->setHidden(!value);
            this->editTextChanged();
        },
        this->managedConnections_);

    // sendWaitStatus visibility
    getSettings()->showSendWaitTimer.connect(
        [this](bool value, const auto &) {
            if (!this->ui_.sendWaitStatus->text().isEmpty())
            {
                this->ui_.sendWaitStatus->setHidden(!value);
            }
        },
        this->managedConnections_);
}

QCompleter *SplitInput::createCompleter(ChannelPtr channel)
{
    if (channel == nullptr)
    {
        return nullptr;
    }

    auto *completer = new QCompleter(this);
    auto *model = new TabCompletionModel(
        *channel, completer, [guard = QPointer<SplitInput>(this)] {
            if (guard == nullptr)
            {
                return std::vector<MessagePlatform>{};
            }

            return guard->selectedSendPlatforms();
        });
    completer->setModel(model);
    return completer;
}

void SplitInput::triggerSelfMessageReceived()
{
    if (this->backgroundColorAnimation.state() != QPropertyAnimation::Stopped)
    {
        this->backgroundColorAnimation.stop();
    }
    this->backgroundColorAnimation.setDirection(QPropertyAnimation::Forward);
    this->backgroundColorAnimation.start();
}

void SplitInput::scaleChangedEvent(float scale)
{
    // update the icon size of the buttons
    this->updateBadgeButton();
    this->updateEmoteButton();
    this->updatePlatformButtonLayout(
        static_cast<int>(this->selectedSendPlatforms().size()));
    this->updateCancelReplyButton();

    // set maximum height
    if (!this->hidden)
    {
        this->setMaximumHeight(this->scaledMaxHeight());
        if (this->replyTarget_ != nullptr)
        {
            this->ui_.vbox->setSpacing(this->marginForTheme());
        }
    }
    this->updateFonts();
}

void SplitInput::themeChangedEvent()
{
    QPalette palette;

    palette.setColor(QPalette::WindowText, this->theme->splits.input.text);

    this->ui_.textEditLength->setPalette(palette);
    this->ui_.sendWaitStatus->setPalette(palette);

    // Theme changed, reset current background color
    this->setBackgroundColor(this->theme->splits.input.background);
    this->backgroundColorAnimation.setStartValue(
        this->theme->splits.input.backgroundPulse);
    this->backgroundColorAnimation.setEndValue(
        this->theme->splits.input.background);
    this->backgroundColorAnimation.stop();
    this->updateTextEditPalette();
    this->updateBadgeButton();
    this->updatePlatformSelector();

    if (this->theme->isLightTheme())
    {
        this->ui_.replyLabel->setStyleSheet("color: #333");
    }
    else
    {
        this->ui_.replyLabel->setStyleSheet("color: #ccc");
    }

    // update vbox
    this->applyOuterMargin();
    if (this->replyTarget_ != nullptr)
    {
        this->ui_.vbox->setSpacing(this->marginForTheme());
    }
}

void SplitInput::updateBadgeButton()
{
    if (this->ui_.badgeButton == nullptr)
    {
        return;
    }

    auto *identityButton =
        static_cast<ChatIdentityButton *>(this->ui_.badgeButton);
    const int size = std::max(24, int(26 * this->scale()));
    const int targetWidth = this->badgeButtonTargetWidth();
    this->ui_.badgeButton->setFixedSize(targetWidth, size);
    if (this->ui_.badgeButtonWrapper != nullptr)
    {
        this->ui_.badgeButtonWrapper->setFixedHeight(size);
    }

    const auto platforms = this->selectedSendPlatforms();
    const bool canUseTwitch =
        containsPlatform(platforms, MessagePlatform::AnyOrTwitch);
    auto currentAccount = getApp()->getAccounts()->twitch.getCurrent();
    const bool hasTwitchAccount =
        currentAccount != nullptr && !currentAccount->isAnon();
    const QString accountID =
        hasTwitchAccount ? currentAccount->getUserId().trimmed() : QString{};
    auto &observedAccountID = badgeIdentityObservedAccountID();
    if (observedAccountID != accountID)
    {
        observedAccountID = accountID;
        for (auto *reply : this->pendingBadgeIdentityRequests_)
        {
            if (reply != nullptr)
            {
                reply->abort();
            }
        }
        this->pendingBadgeIdentityRequests_.clear();
        this->failedBadgeIdentityChannels_.clear();
        ++this->badgeIdentityRequestGeneration_;
        twitch::clearCurrentUserOwnedBadges();
    }

    QString channelName;
    auto channel = this->channelForSendPlatform(MessagePlatform::AnyOrTwitch);
    auto twitch = std::dynamic_pointer_cast<TwitchChannel>(channel);
    if (twitch)
    {
        channelName = twitch->getName();
    }

    const bool isRealTwitchChannel = isRealTwitchChatIdentityChannel(twitch);
    const bool showBadgeButton =
        canUseTwitch && hasTwitchAccount && isRealTwitchChannel;
    this->setBadgeButtonShown(showBadgeButton, true);
    if (!showBadgeButton)
    {
        identityButton->setBadgeSources({});
        if (this->badgePickerPopup_ != nullptr &&
            this->badgePickerPopup_->isVisible())
        {
            this->badgePickerPopup_->hide();
        }
        return;
    }

    this->requestBadgeIdentityForCurrentTwitchChannel(twitch);

    QVector<ChatIdentityBadgeSource> badgeSources;
    for (const auto &badge : twitch::currentUserIdentityBadges(channelName))
    {
        const auto source = chatIdentitySourceForBadge(badge, twitch.get());
        if (!source.key.isEmpty())
        {
            badgeSources.push_back(source);
        }
    }
    identityButton->setBadgeSources(std::move(badgeSources));
}

int SplitInput::badgeButtonTargetWidth() const
{
    const int size = std::max(24, int(26 * this->scale()));
    const int paintNudge = std::max(4, int(std::round(4 * this->scale())));
    return size + paintNudge - 1;
}

void SplitInput::setBadgeButtonShown(bool shown, bool animate)
{
    if (this->ui_.badgeButtonWrapper == nullptr ||
        this->ui_.badgeButton == nullptr)
    {
        return;
    }

    auto *opacityEffect = qobject_cast<QGraphicsOpacityEffect *>(
        this->ui_.badgeButton->graphicsEffect());
    const int targetWidth = this->badgeButtonTargetWidth();
    const auto applyProgress = [this, opacityEffect, targetWidth](qreal value) {
        const auto progress = std::clamp(value, 0.0, 1.0);
        this->ui_.badgeButtonWrapper->setFixedWidth(
            int(std::round(targetWidth * progress)));
        if (opacityEffect != nullptr)
        {
            opacityEffect->setOpacity(progress);
        }
    };

    if (!this->badgeButtonVisibilityInitialized_)
    {
        this->badgeButtonVisibilityInitialized_ = true;
        this->badgeButtonShown_ = shown;
        this->ui_.badgeButtonWrapper->setVisible(shown);
        this->ui_.badgeButton->setEnabled(shown);
        applyProgress(shown ? 1.0 : 0.0);
        return;
    }

    if (this->badgeButtonShown_ == shown &&
        this->badgeButtonVisibilityAnimation_.state() !=
            QAbstractAnimation::Running)
    {
        this->ui_.badgeButtonWrapper->setVisible(shown);
        this->ui_.badgeButton->setEnabled(shown);
        applyProgress(shown ? 1.0 : 0.0);
        return;
    }

    this->badgeButtonShown_ = shown;
    this->badgeButtonVisibilityAnimation_.stop();
    this->ui_.badgeButtonWrapper->show();
    this->ui_.badgeButton->setEnabled(shown);

    const auto start =
        targetWidth > 0
            ? std::clamp(static_cast<qreal>(
                             this->ui_.badgeButtonWrapper->width()) /
                             static_cast<qreal>(targetWidth),
                         0.0, 1.0)
            : (shown ? 1.0 : 0.0);
    const auto end = shown ? 1.0 : 0.0;
    if (!animate || qFuzzyCompare(start + 1.0, end + 1.0))
    {
        this->ui_.badgeButtonWrapper->setVisible(shown);
        this->ui_.badgeButton->setEnabled(shown);
        applyProgress(end);
        return;
    }

    this->badgeButtonVisibilityAnimation_.setStartValue(start);
    this->badgeButtonVisibilityAnimation_.setEndValue(end);
    this->badgeButtonVisibilityAnimation_.start();
}

void SplitInput::requestBadgeIdentityForCurrentTwitchChannel(
    const std::shared_ptr<TwitchChannel> &twitch)
{
    if (twitch == nullptr)
    {
        return;
    }

    const auto channelLogin =
        twitch::normalizedBadgeChannelLogin(twitch->getName());
    if (!isRealTwitchChatIdentityChannel(twitch) || channelLogin.isEmpty() ||
        channelLogin.startsWith(QLatin1Char('/')) ||
        twitch::hasLoadedCurrentUserOwnedBadgesForChannel(channelLogin) ||
        this->pendingBadgeIdentityRequests_.contains(channelLogin) ||
        this->failedBadgeIdentityChannels_.contains(channelLogin))
    {
        return;
    }

    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (account == nullptr || account->isAnon())
    {
        return;
    }

    const auto badgeAuth =
        TwitchModerationAuth::resolveForCurrentUser(account->getUserId());
    if (!badgeAuth.supportsWebGql())
    {
        this->failedBadgeIdentityChannels_.insert(channelLogin);
        return;
    }

    const int requestGeneration = this->badgeIdentityRequestGeneration_;
    auto *reply = twitch::requestCurrentUserBadgeIdentity(
        this->badgeIdentityNetwork_, channelLogin, badgeAuth, this,
        [guard = QPointer<SplitInput>(this), requestGeneration,
         channelLogin](QVector<twitch::CurrentUserBadgeIdentity> badges) {
            if (guard == nullptr ||
                requestGeneration != guard->badgeIdentityRequestGeneration_)
            {
                return;
            }

            guard->pendingBadgeIdentityRequests_.remove(channelLogin);
            twitch::updateCurrentUserOwnedBadgesForChannel(channelLogin, badges);
            guard->updateBadgeButton();
        },
        [guard = QPointer<SplitInput>(this), requestGeneration,
         channelLogin](const QString &) {
            if (guard == nullptr ||
                requestGeneration != guard->badgeIdentityRequestGeneration_)
            {
                return;
            }

            guard->pendingBadgeIdentityRequests_.remove(channelLogin);
            guard->failedBadgeIdentityChannels_.insert(channelLogin);
        });
    if (reply != nullptr)
    {
        this->pendingBadgeIdentityRequests_.insert(channelLogin, reply);
    }
    else
    {
        this->failedBadgeIdentityChannels_.insert(channelLogin);
    }
}

void SplitInput::resetBadgeIdentityButtonFetch(bool clearBadges)
{
    for (auto *reply : this->pendingBadgeIdentityRequests_)
    {
        if (reply != nullptr)
        {
            reply->abort();
        }
    }
    this->pendingBadgeIdentityRequests_.clear();
    this->failedBadgeIdentityChannels_.clear();
    ++this->badgeIdentityRequestGeneration_;

    if (clearBadges)
    {
        twitch::clearCurrentUserOwnedBadges();
    }

    if (this->ui_.badgeButton != nullptr)
    {
        auto *identityButton =
            static_cast<ChatIdentityButton *>(this->ui_.badgeButton);
        identityButton->setBadgeSources({});
    }
}

void SplitInput::updateEmoteButton()
{
    auto scale = this->scale();

    for (auto *button :
         {this->ui_.predictionButton, this->ui_.pollButton,
          this->ui_.emoteButton})
    {
        if (button == nullptr)
        {
            continue;
        }

        button->setFixedHeight(int(26 * scale));
        // Make button slightly wider so it's easier to click
        button->setFixedWidth(int(24 * scale));
    }
}

void SplitInput::updatePlatformButtonLayout(int platformCount)
{
    auto scale = this->scale();

    constexpr int TOP_TRIM_PX = 2;
    constexpr int BOTTOM_TRIM_PX = 3;
    constexpr int TEXT_RAISE_PX = 1;

    const auto height = int(18 * scale);
    const auto width = platformButtonWidthForCount(platformCount, scale);
    const auto minimumHeight =
        height * 2 + int(4 * scale) -
        int((TOP_TRIM_PX + BOTTOM_TRIM_PX) * scale);

    if (this->ui_.inputWrapper)
    {
        this->ui_.inputWrapper->setMinimumHeight(minimumHeight);
    }

    if (this->ui_.textEdit)
    {
        this->ui_.textEdit->setVerticalPadding(
            std::max(0, int((5 - TOP_TRIM_PX - TEXT_RAISE_PX) * scale)), 0);
    }

    if (this->ui_.platformButton)
    {
        this->ui_.platformButton->setFixedHeight(int(26 * scale));
        this->ui_.platformButton->setFixedWidth(width);
    }

    if (this->ui_.rightVbox)
    {
        this->ui_.rightVbox->setContentsMargins(0, 0, int(3 * scale), 0);
    }
}

std::vector<MessagePlatform> SplitInput::availableSendPlatforms() const
{
    std::vector<MessagePlatform> platforms;

    auto channel = this->split_->getChannel();
    auto *merged = dynamic_cast<MergedChannel *>(channel.get());
    if (merged)
    {
        if (this->canSendToPlatform(MessagePlatform::AnyOrTwitch))
        {
            platforms.push_back(MessagePlatform::AnyOrTwitch);
        }
        if (this->canSendToPlatform(MessagePlatform::Kick))
        {
            platforms.push_back(MessagePlatform::Kick);
        }
        if (this->canSendToPlatform(MessagePlatform::YouTube))
        {
            platforms.push_back(MessagePlatform::YouTube);
        }
        return platforms;
    }

    if (channel && channel->isTwitchChannel() &&
        getApp()->getAccounts()->twitch.isLoggedIn())
    {
        platforms.push_back(MessagePlatform::AnyOrTwitch);
    }
    else if (channel && channel->isKickChannel() &&
             getApp()->getAccounts()->kick.isLoggedIn())
    {
        platforms.push_back(MessagePlatform::Kick);
    }

    return platforms;
}

std::vector<MessagePlatform> SplitInput::cycleSendPlatforms(
    const std::vector<MessagePlatform> &availablePlatforms) const
{
    if (this->enabledSendPlatforms_.empty())
    {
        return availablePlatforms;
    }

    const auto enabledPlatforms = platformSelectionIntersection(
        availablePlatforms, this->enabledSendPlatforms_);
    return enabledPlatforms.empty() ? availablePlatforms : enabledPlatforms;
}

std::optional<MessagePlatform> SplitInput::replySendPlatform() const
{
    if (!this->replyTarget_)
    {
        return std::nullopt;
    }

    switch (this->replyTarget_->platform)
    {
        case MessagePlatform::Kick:
            return MessagePlatform::Kick;
        case MessagePlatform::YouTube:
            return MessagePlatform::YouTube;
        case MessagePlatform::AnyOrTwitch:
            return MessagePlatform::AnyOrTwitch;
        default:
            return std::nullopt;
    }
}

std::vector<MessagePlatform> SplitInput::storedSelectedSendPlatforms(
    const std::vector<MessagePlatform> &availablePlatforms) const
{
    if (availablePlatforms.empty())
    {
        return {};
    }

    const auto cyclePlatforms = this->cycleSendPlatforms(availablePlatforms);

    if (!this->customSelectedSendPlatforms_.empty())
    {
        auto selectedPlatforms = platformSelectionIntersection(
            cyclePlatforms, this->customSelectedSendPlatforms_);
        if (!selectedPlatforms.empty())
        {
            return selectedPlatforms;
        }
    }

    if (this->selectedSendAllPlatforms_ && cyclePlatforms.size() > 1)
    {
        return cyclePlatforms;
    }

    if (std::ranges::find(cyclePlatforms, this->selectedSendPlatform_) ==
        cyclePlatforms.end())
    {
        return {cyclePlatforms.front()};
    }

    return {this->selectedSendPlatform_};
}

std::vector<MessagePlatform> SplitInput::selectedSendPlatforms() const
{
    const auto platforms = this->availableSendPlatforms();
    if (platforms.empty())
    {
        return {};
    }

    if (auto replyPlatform = this->replySendPlatform())
    {
        if (std::ranges::find(platforms, *replyPlatform) != platforms.end())
        {
            return {*replyPlatform};
        }
        return {};
    }

    return this->storedSelectedSendPlatforms(platforms);
}

ChannelPtr SplitInput::channelForSendPlatform(MessagePlatform platform) const
{
    auto channel = this->split_->getChannel();
    auto *merged = dynamic_cast<MergedChannel *>(channel.get());
    if (!merged)
    {
        return channel;
    }

    switch (platform)
    {
        case MessagePlatform::Kick:
            return merged->kickChannel();
        case MessagePlatform::YouTube:
            return nullptr;
        case MessagePlatform::AnyOrTwitch:
        default:
            return merged->twitchChannel();
    }
}

bool SplitInput::canSendToPlatform(MessagePlatform platform) const
{
    auto splitChannel = this->split_->getChannel();
    auto *merged = dynamic_cast<MergedChannel *>(splitChannel.get());
    if (platform == MessagePlatform::YouTube)
    {
        auto *youtube = merged ? merged->youtubeLiveChat() : nullptr;
        return merged && merged->config().youtubeEnabled && youtube != nullptr &&
               youtube->isLive() &&
               getApp()->getAccounts()->youtube.isLoggedIn();
    }

    auto channel = this->channelForSendPlatform(platform);
    if (!channel)
    {
        return false;
    }

    switch (platform)
    {
        case MessagePlatform::Kick:
            return getApp()->getAccounts()->kick.isLoggedIn() &&
                   channel->isKickChannel();
        case MessagePlatform::AnyOrTwitch:
        default:
            return getApp()->getAccounts()->twitch.isLoggedIn() &&
                   channel->isTwitchChannel();
    }
}

void SplitInput::updatePlatformSelector(bool animate)
{
    const auto platforms = this->availableSendPlatforms();

    if (platforms.empty())
    {
        this->updateBadgeButton();
        this->ui_.platformButton->hide();
        return;
    }

    const auto previousSelectedPlatforms =
        this->emotePopup_ ? this->selectedSendPlatforms()
                          : std::vector<MessagePlatform>{};

    this->normalizeSelectedSendPlatforms(platforms);

    const bool replyLocked = this->replySendPlatform().has_value();
    const auto selectedPlatforms = this->selectedSendPlatforms();
    if (selectedPlatforms.empty())
    {
        this->updateBadgeButton();
        this->ui_.platformButton->hide();
        return;
    }

    const auto cyclePlatforms = this->cycleSendPlatforms(platforms);
    const auto targetCount =
        replyLocked
            ? selectedPlatforms.size()
            : cyclePlatforms.size() +
                          (cyclePlatforms.size() > 1 ? 1 : 0);
    const bool canChoosePlatforms = !replyLocked && platforms.size() >= 3;

    this->updatePlatformButtonLayout(static_cast<int>(selectedPlatforms.size()));
    this->updateBadgeButton();
    this->ui_.platformButton->show();
    this->ui_.platformButton->setEnabled(
        !replyLocked && (canChoosePlatforms || targetCount > 1));
    this->ui_.platformButton->setPlatforms(selectedPlatforms,
                                           animate && targetCount > 1);

    auto tooltip =
        QString(replyLocked ? u"Replying on %1"_s : u"Sending to %1"_s)
            .arg(platformDisplayName(selectedPlatforms));
    if (canChoosePlatforms)
    {
        tooltip += u". Click to choose platforms"_s;
        if (targetCount > 1)
        {
            tooltip += u". Press Ctrl+D to cycle targets"_s;
        }
    }
    else if (!replyLocked && targetCount > 1)
    {
        QString nextTarget;
        if (this->selectedSendAllPlatforms_)
        {
            nextTarget = platformDisplayName(cyclePlatforms.front());
        }
        else
        {
            auto it =
                std::ranges::find(cyclePlatforms, this->selectedSendPlatform_);
            if (it != cyclePlatforms.end() && ++it != cyclePlatforms.end())
            {
                nextTarget = platformDisplayName(*it);
            }
            else
            {
                nextTarget = platformDisplayName(cyclePlatforms);
            }
        }
        tooltip += QString(u". Click or press Ctrl+D to switch to %1"_s)
                       .arg(nextTarget);
    }
    this->ui_.platformButton->setToolTip(tooltip);

    if (this->emotePopup_ && previousSelectedPlatforms != selectedPlatforms)
    {
        this->updateEmotePopupChannel();
    }

    this->updatePollPredictionButtons();
}

void SplitInput::updatePollPredictionButtons()
{
    if (this->split_ == nullptr || this->ui_.predictionButton == nullptr ||
        this->ui_.pollButton == nullptr)
    {
        return;
    }

    auto *pollPredictionBar = this->split_->twitchPollsAndPredictionsBar_;
    if (pollPredictionBar == nullptr)
    {
        this->ui_.predictionButton->setVisible(false);
        this->ui_.predictionButton->setEnabled(false);
        this->ui_.pollButton->setVisible(false);
        this->ui_.pollButton->setEnabled(false);
        return;
    }

    const auto channel = this->split_->getChannel();
    const auto twitchChannel = twitchChannelForPollPrediction(channel);
    const bool sendingToTwitch =
        containsPlatform(this->selectedSendPlatforms(),
                         MessagePlatform::AnyOrTwitch);
    const bool twitchVisible = twitchChannel != nullptr && sendingToTwitch;
    const bool canManage = canManagePollPredictions(twitchChannel);
    const bool predictionVisible =
        twitchVisible && (canManage || pollPredictionBar->hasOpenPrediction());
    const bool pollVisible =
        twitchVisible && (canManage || pollPredictionBar->hasActivePoll());

    this->ui_.predictionButton->setVisible(predictionVisible);
    this->ui_.predictionButton->setEnabled(predictionVisible);
    QString predictionTooltip = QStringLiteral("Start prediction");
    if (predictionVisible)
    {
        if (!canManage && pollPredictionBar->hasOpenPrediction())
        {
            predictionTooltip = QStringLiteral("Open prediction on Twitch");
        }
        else if (needsModerationAuthLogin(twitchChannel))
        {
            predictionTooltip = QStringLiteral(
                "Login to Twitch mod actions to start or manage predictions");
        }
        else
        {
            predictionTooltip =
                pollPredictionBar->predictionButtonTooltip(canManage);
        }
    }
    this->ui_.predictionButton->setToolTip(predictionTooltip);

    this->ui_.pollButton->setVisible(pollVisible);
    this->ui_.pollButton->setEnabled(pollVisible);
    QString pollTooltip = QStringLiteral("Start poll");
    if (pollVisible)
    {
        if (!canManage && pollPredictionBar->hasActivePoll())
        {
            pollTooltip = QStringLiteral("Open poll on Twitch");
        }
        else if (needsModerationAuthLogin(twitchChannel))
        {
            pollTooltip =
                QStringLiteral("Login to Twitch mod actions to start polls");
        }
        else
        {
            pollTooltip = pollPredictionBar->pollButtonTooltip(canManage);
        }
    }
    this->ui_.pollButton->setToolTip(pollTooltip);
}

void SplitInput::applyActiveAccountProviderDefault()
{
    const auto platforms = this->availableSendPlatforms();
    if (platforms.empty())
    {
        this->updatePlatformSelector();
        return;
    }

    const auto preferredPlatform = messagePlatformForProvider(
        getApp()->getWindows()->activeAccountProvider());
    auto selectedPlatform = platforms.front();
    if (std::ranges::find(platforms, preferredPlatform) != platforms.end())
    {
        selectedPlatform = preferredPlatform;
    }

    const bool changed = this->selectedSendAllPlatforms_ ||
                         !this->customSelectedSendPlatforms_.empty() ||
                         !this->enabledSendPlatforms_.empty() ||
                         this->selectedSendPlatform_ != selectedPlatform;
    this->selectedSendAllPlatforms_ = false;
    this->customSelectedSendPlatforms_.clear();
    this->enabledSendPlatforms_.clear();
    this->selectedSendPlatform_ = selectedPlatform;
    this->updatePlatformSelector(changed);

    if (changed)
    {
        this->sendPlatformChanged.invoke();
    }
}

SplitInput::SendPlatformSelection SplitInput::sendPlatformSelection() const
{
    return {
        this->selectedSendPlatform_,
        this->selectedSendAllPlatforms_,
        this->customSelectedSendPlatforms_,
        this->enabledSendPlatforms_,
    };
}

void SplitInput::restoreSendPlatformSelection(
    const SendPlatformSelection &selection)
{
    this->selectedSendPlatform_ = selection.selectedPlatform;
    this->selectedSendAllPlatforms_ = selection.allPlatforms;
    this->customSelectedSendPlatforms_ = selection.customPlatforms;
    this->enabledSendPlatforms_ = selection.enabledPlatforms;
    this->updatePlatformSelector();
}

std::optional<MessagePlatform> SplitInput::selectedSendPlatform() const
{
    const auto platforms = this->selectedSendPlatforms();
    if (platforms.size() != 1)
    {
        return std::nullopt;
    }

    return platforms.front();
}

QString SplitInput::selectedSendPlatformDisplayName() const
{
    return platformDisplayName(this->selectedSendPlatforms());
}

QString SplitInput::selectedSendAccountName() const
{
    QStringList accountNames;
    for (const auto platform : this->selectedSendPlatforms())
    {
        switch (platform)
        {
            case MessagePlatform::Kick: {
                auto user = getApp()->getAccounts()->kick.current();
                if (user && !user->isAnonymous())
                {
                    accountNames.append(user->username());
                }
            }
            break;
            case MessagePlatform::YouTube: {
                auto user = getApp()->getAccounts()->youtube.current();
                if (user && !user->isAnonymous())
                {
                    accountNames.append(user->displayName().isEmpty()
                                            ? user->channelID()
                                            : user->displayName());
                }
            }
            break;
            case MessagePlatform::AnyOrTwitch:
            default: {
                auto user = getApp()->getAccounts()->twitch.getCurrent();
                if (user && !user->isAnon())
                {
                    const auto helperAccount = TwitchModerationAuth::savedAccount();
                    const bool helperMatches =
                        (!helperAccount.userId.trimmed().isEmpty() &&
                         helperAccount.userId == user->getUserId()) ||
                        (!helperAccount.login.trimmed().isEmpty() &&
                         helperAccount.login.compare(user->getUserName(),
                                                     Qt::CaseInsensitive) == 0);
                    const auto displayName =
                        helperMatches ? helperAccount.displayName.trimmed()
                                      : QString{};
                    accountNames.append(displayName.isEmpty()
                                            ? user->getUserName()
                                            : displayName);
                }
            }
            break;
        }
    }

    accountNames.removeDuplicates();
    return accountNames.join(u" + "_s);
}

void SplitInput::selectSendPlatform(MessagePlatform platform)
{
    const auto platforms =
        this->cycleSendPlatforms(this->availableSendPlatforms());
    if (!containsPlatform(platforms, platform) ||
        !this->canSendToPlatform(platform))
    {
        return;
    }

    const bool wasCustom = !this->customSelectedSendPlatforms_.empty();
    if (!this->selectedSendAllPlatforms_ &&
        !wasCustom &&
        this->selectedSendPlatform_ == platform)
    {
        this->cycleSendPlatform();
        return;
    }

    this->selectedSendAllPlatforms_ = false;
    this->customSelectedSendPlatforms_.clear();
    this->selectedSendPlatform_ = platform;
    this->updatePlatformSelector(true);
    this->sendPlatformChanged.invoke();
}

void SplitInput::selectAllSendPlatforms()
{
    const auto platforms =
        this->cycleSendPlatforms(this->availableSendPlatforms());
    if (platforms.size() < 2)
    {
        return;
    }

    if (this->selectedSendAllPlatforms_)
    {
        this->cycleSendPlatform();
        return;
    }

    this->selectedSendAllPlatforms_ = true;
    this->customSelectedSendPlatforms_.clear();
    this->selectedSendPlatform_ = platforms.front();
    this->updatePlatformSelector(true);
    this->sendPlatformChanged.invoke();
}

void SplitInput::normalizeSelectedSendPlatforms(
    const std::vector<MessagePlatform> &availablePlatforms)
{
    if (availablePlatforms.empty())
    {
        this->selectedSendAllPlatforms_ = false;
        this->customSelectedSendPlatforms_.clear();
        this->enabledSendPlatforms_.clear();
        return;
    }

    if (!this->enabledSendPlatforms_.empty())
    {
        auto enabledPlatforms = platformSelectionIntersection(
            availablePlatforms, this->enabledSendPlatforms_);
        if (enabledPlatforms.empty() ||
            enabledPlatforms.size() == availablePlatforms.size())
        {
            this->enabledSendPlatforms_.clear();
        }
        else
        {
            this->enabledSendPlatforms_ = std::move(enabledPlatforms);
        }
    }

    const auto cyclePlatforms = this->cycleSendPlatforms(availablePlatforms);

    if (!this->customSelectedSendPlatforms_.empty())
    {
        auto selectedPlatforms = platformSelectionIntersection(
            cyclePlatforms, this->customSelectedSendPlatforms_);
        if (selectedPlatforms.empty())
        {
            this->customSelectedSendPlatforms_.clear();
            this->selectedSendAllPlatforms_ = false;
            this->selectedSendPlatform_ = cyclePlatforms.front();
            return;
        }

        if (selectedPlatforms.size() == cyclePlatforms.size())
        {
            this->customSelectedSendPlatforms_.clear();
            this->selectedSendAllPlatforms_ = cyclePlatforms.size() > 1;
            this->selectedSendPlatform_ = cyclePlatforms.front();
            return;
        }

        if (selectedPlatforms.size() == 1)
        {
            this->customSelectedSendPlatforms_.clear();
            this->selectedSendAllPlatforms_ = false;
            this->selectedSendPlatform_ = selectedPlatforms.front();
            return;
        }

        this->selectedSendAllPlatforms_ = false;
        this->customSelectedSendPlatforms_ = std::move(selectedPlatforms);
        this->selectedSendPlatform_ = this->customSelectedSendPlatforms_.front();
        return;
    }

    if (this->selectedSendAllPlatforms_ && cyclePlatforms.size() < 2)
    {
        this->selectedSendAllPlatforms_ = false;
    }

    if (!containsPlatform(cyclePlatforms, this->selectedSendPlatform_))
    {
        this->selectedSendPlatform_ = cyclePlatforms.front();
    }
}

void SplitInput::setSelectedSendPlatforms(
    std::vector<MessagePlatform> platforms)
{
    const auto availablePlatforms = this->availableSendPlatforms();
    if (availablePlatforms.empty())
    {
        return;
    }

    const auto previousPlatforms = this->selectedSendPlatforms();
    auto selectedPlatforms =
        platformSelectionIntersection(availablePlatforms, platforms);
    if (selectedPlatforms.empty())
    {
        return;
    }

    if (previousPlatforms.size() == 1 && selectedPlatforms.size() > 1)
    {
        const auto previousPlatform = previousPlatforms.front();
        auto it = std::ranges::find(selectedPlatforms, previousPlatform);
        if (it != selectedPlatforms.end())
        {
            selectedPlatforms.erase(it);
            selectedPlatforms.insert(selectedPlatforms.begin(),
                                     previousPlatform);
        }
    }

    if (selectedPlatforms.size() == availablePlatforms.size())
    {
        this->enabledSendPlatforms_.clear();
    }
    else
    {
        this->enabledSendPlatforms_ = selectedPlatforms;
    }

    const auto cyclePlatforms = this->cycleSendPlatforms(availablePlatforms);
    if (cyclePlatforms.size() == 1)
    {
        this->selectedSendAllPlatforms_ = false;
        this->customSelectedSendPlatforms_.clear();
        this->selectedSendPlatform_ = cyclePlatforms.front();
    }
    else
    {
        this->selectedSendAllPlatforms_ = true;
        this->customSelectedSendPlatforms_.clear();
        this->selectedSendPlatform_ = cyclePlatforms.front();
    }

    const auto currentPlatforms = this->selectedSendPlatforms();
    const bool changed = previousPlatforms != currentPlatforms;
    this->updatePlatformSelector(changed);
    if (changed)
    {
        this->sendPlatformChanged.invoke();
    }
}

void SplitInput::showPlatformSelectionMenu()
{
    const auto availablePlatforms = this->availableSendPlatforms();
    if (availablePlatforms.size() < 3 || this->replySendPlatform().has_value())
    {
        this->tryCycleSendPlatform();
        return;
    }

    QMenu menu(this);
    const auto enabledPlatforms = this->cycleSendPlatforms(availablePlatforms);
    for (const auto platform : availablePlatforms)
    {
        auto *action = new QWidgetAction(&menu);
        auto *checkBox = new QCheckBox(platformDisplayName(platform), &menu);
        checkBox->setIcon(QIcon(platformIconPath(platform)));
        checkBox->setChecked(
            containsPlatform(enabledPlatforms, platform));
        checkBox->setFocusPolicy(Qt::NoFocus);
        action->setDefaultWidget(checkBox);
        menu.addAction(action);

        QObject::connect(checkBox, &QCheckBox::toggled, this,
                         [this, checkBox, platform](bool checked) {
                             auto selectedPlatforms =
                                 this->cycleSendPlatforms(
                                     this->availableSendPlatforms());
                             if (checked)
                             {
                                 if (!containsPlatform(selectedPlatforms,
                                                       platform))
                                 {
                                     selectedPlatforms.push_back(platform);
                                 }
                             }
                             else
                             {
                                 if (selectedPlatforms.size() <= 1)
                                 {
                                     QSignalBlocker blocker(checkBox);
                                     checkBox->setChecked(true);
                                     return;
                                 }
                                 std::erase(selectedPlatforms, platform);
                             }

                             this->setSelectedSendPlatforms(
                                 std::move(selectedPlatforms));
                         });
    }

    menu.exec(this->ui_.platformButton->mapToGlobal(
        QPoint{0, this->ui_.platformButton->height()}));
}

bool SplitInput::tryCycleSendPlatform()
{
    this->updatePlatformSelector();

    const auto platforms =
        this->cycleSendPlatforms(this->availableSendPlatforms());
    const auto targetCount = platforms.size() + (platforms.size() > 1 ? 1 : 0);
    if (this->replySendPlatform().has_value() || targetCount < 2)
    {
        return false;
    }

    this->cycleSendPlatform();
    return true;
}

void SplitInput::cycleSendPlatform()
{
    const auto platforms =
        this->cycleSendPlatforms(this->availableSendPlatforms());
    const auto targetCount = platforms.size() + (platforms.size() > 1 ? 1 : 0);
    if (targetCount < 2)
    {
        return;
    }

    if (this->selectedSendAllPlatforms_)
    {
        this->selectSendPlatform(platforms.front());
        return;
    }
    if (!this->customSelectedSendPlatforms_.empty())
    {
        this->selectSendPlatform(platforms.front());
        return;
    }

    auto it = std::ranges::find(platforms, this->selectedSendPlatform_);
    if (it == platforms.end() || ++it == platforms.end())
    {
        this->selectAllSendPlatforms();
        return;
    }
    this->selectSendPlatform(*it);
}

void SplitInput::updateCancelReplyButton()
{
    float scale = this->scale();

    this->ui_.cancelReplyButton->setFixedHeight(int(12 * scale));
    this->ui_.cancelReplyButton->setFixedWidth(int(20 * scale));
}

void SplitInput::openPollDialog()
{
    if (!containsPlatform(this->selectedSendPlatforms(),
                          MessagePlatform::AnyOrTwitch))
    {
        return;
    }

    auto channel = this->split_->getChannel();
    auto twitchChannel = twitchChannelForPollPrediction(channel);
    if (twitchChannel == nullptr)
    {
        return;
    }
    const bool canManage = canManagePollPredictions(twitchChannel);
    const bool hasActivePoll =
        this->split_->twitchPollsAndPredictionsBar_->hasActivePoll();
    if (!canManage)
    {
        if (hasActivePoll)
        {
            QDesktopServices::openUrl(twitchPollPopoutUrl(*twitchChannel));
        }
        return;
    }

    if (!getApp()->getAccounts()->twitch.isLoggedIn())
    {
        if (channel != nullptr)
        {
            channel->addSystemMessage(
                QStringLiteral("You must be logged in to create a poll!"));
        }
        return;
    }

    if (!hasBroadcasterPollPredictionToken(twitchChannel))
    {
        auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
        QString authError;
        const auto moderationAccount =
            TwitchModerationAuth::resolveForCurrentUser(
                currentUser != nullptr ? currentUser->getUserId() : QString{},
                &authError);
        if (!moderationAccount.isValid())
        {
            showModerationAuthLoginPrompt(
                this,
                currentUser != nullptr ? currentUser->getUserId() : QString{},
                QStringLiteral("start Twitch polls"),
                [guard = QPointer<SplitInput>(this)] {
                    if (guard != nullptr)
                    {
                        guard->openPollDialog();
                    }
                });
            return;
        }
    }

    CreatePollDialog::showDialog(std::move(channel), *twitchChannel);
}

void SplitInput::openPredictionDialog()
{
    if (!containsPlatform(this->selectedSendPlatforms(),
                          MessagePlatform::AnyOrTwitch))
    {
        return;
    }

    auto channel = this->split_->getChannel();
    auto twitchChannel = twitchChannelForPollPrediction(channel);
    if (twitchChannel == nullptr)
    {
        return;
    }
    const bool canManage = canManagePollPredictions(twitchChannel);
    const bool canUseBroadcasterHelix =
        hasBroadcasterPollPredictionToken(twitchChannel);
    const bool hasOpenPrediction =
        this->split_->twitchPollsAndPredictionsBar_->hasOpenPrediction();
    if (!canManage)
    {
        if (hasOpenPrediction)
        {
            QDesktopServices::openUrl(
                twitchPredictionSummaryUrl(*twitchChannel));
        }
        return;
    }

    if (!canUseBroadcasterHelix)
    {
        auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
        if (currentUser == nullptr || currentUser->isAnon())
        {
            if (channel != nullptr)
            {
                channel->addSystemMessage(QStringLiteral(
                    "You must be logged in to create or manage a prediction!"));
            }
            return;
        }

        QString authError;
        const auto moderationAccount =
            TwitchModerationAuth::resolveForCurrentUser(
                currentUser->getUserId(), &authError);
        if (!moderationAccount.isValid())
        {
            showModerationAuthLoginPrompt(
                this,
                currentUser != nullptr ? currentUser->getUserId() : QString{},
                QStringLiteral("start or manage Twitch predictions"),
                [guard = QPointer<SplitInput>(this)] {
                    if (guard != nullptr)
                    {
                        guard->openPredictionDialog();
                    }
                });
            return;
        }

        if (hasOpenPrediction)
        {
            const auto roomId = twitchChannel->roomId();
            const auto channelLogin = twitchChannel->getName();
            const auto openLocalPrediction =
                [channel, roomId, channelLogin] {
                    const auto predictionJson =
                        TwitchPollsAndPredictionsBar::localPredictionJson(
                            roomId);
                    if (!predictionJson ||
                        predictionJson->value(QStringLiteral("id"))
                            .toString()
                            .trimmed()
                            .isEmpty())
                    {
                        return false;
                    }

                    ManagePredictionDialog::showDialog(
                        channel, roomId, channelLogin,
                        HelixPrediction(*predictionJson), true);
                    return true;
                };

            TwitchWebApi::getActivePollAndPredictions(
                roomId, moderationAccount.clientId,
                moderationAccount.oauthToken,
                [channel, roomId, channelLogin,
                 openLocalPrediction](const HelixPolls &,
                                      const HelixPredictions &result) {
                    const auto openPrediction = std::find_if(
                        result.predictions.begin(), result.predictions.end(),
                        [](const HelixPrediction &prediction) {
                            return prediction.status == "ACTIVE" ||
                                   prediction.status == "LOCKED";
                        });
                    if (openPrediction != result.predictions.end())
                    {
                        TwitchPollsAndPredictionsBar::rememberLocalPrediction(
                            roomId, *openPrediction);
                        ManagePredictionDialog::showDialog(
                            channel, roomId, channelLogin, *openPrediction,
                            true);
                        return;
                    }

                    if (openLocalPrediction())
                    {
                        return;
                    }

                    if (channel != nullptr)
                    {
                        channel->addSystemMessage(
                            "Could not find an open prediction");
                    }
                },
                [channel, openLocalPrediction](const auto &error) {
                    if (openLocalPrediction())
                    {
                        return;
                    }

                    if (channel != nullptr)
                    {
                        channel->addSystemMessage(
                            "Failed to query predictions - " + error);
                    }
                });
            return;
        }

        CreatePredictionDialog::showDialog(std::move(channel), *twitchChannel);
        return;
    }

    if (!getApp()->getAccounts()->twitch.isLoggedIn())
    {
        if (channel != nullptr)
        {
            channel->addSystemMessage(QStringLiteral(
                "You must be logged in to create or manage a prediction!"));
        }
        return;
    }

    const auto roomId = twitchChannel->roomId();
    const auto channelLogin = twitchChannel->getName();
    getHelix()->getPredictions(
        roomId, {}, 20, {},
        [channel, roomId, channelLogin](const auto &result) {
            const auto openPrediction = std::find_if(
                result.predictions.begin(), result.predictions.end(),
                [](const auto &prediction) {
                    return prediction.status == "ACTIVE" ||
                           prediction.status == "LOCKED";
                });
            if (openPrediction != result.predictions.end())
            {
                ManagePredictionDialog::showDialog(channel, roomId, channelLogin,
                                                   *openPrediction);
                return;
            }

            if (auto twitch = twitchChannelForPollPrediction(channel))
            {
                CreatePredictionDialog::showDialog(channel, *twitch);
                return;
            }

            if (channel != nullptr)
            {
                channel->addSystemMessage(QStringLiteral(
                    "The prediction button only works in Twitch channels"));
            }
        },
        [channel](const auto &error) {
            if (channel != nullptr)
            {
                channel->addSystemMessage("Failed to query predictions - " +
                                          error);
            }
        });
}

void SplitInput::openEmotePopup()
{
    if (!this->emotePopup_)
    {
        this->emotePopup_ = new EmotePopup(this->window());
        this->emotePopup_->setAttribute(Qt::WA_DeleteOnClose);

        QObject::connect(this, &QObject::destroyed, this->emotePopup_,
                         &QWidget::close);

        std::ignore =
            this->emotePopup_->linkClicked.connect([this](const Link &link) {
                if (link.type == Link::InsertText)
                {
                    QTextCursor cursor = this->ui_.textEdit->textCursor();
                    QString textToInsert(link.value + " ");

                    // If symbol before cursor isn't space or empty
                    // Then insert space before emote.
                    if (cursor.position() > 0 &&
                        !this->getInputText()[cursor.position() - 1].isSpace())
                    {
                        textToInsert = " " + textToInsert;
                    }
                    this->insertText(textToInsert);
                    this->ui_.textEdit->activateWindow();
                }
            });
    }

    this->updateEmotePopupChannel();
    this->emotePopup_->show();
    this->emotePopup_->raise();
    this->emotePopup_->activateWindow();
}

void SplitInput::updateEmotePopupChannel()
{
    if (!this->emotePopup_)
    {
        return;
    }

    auto channel = this->split_->getChannel();
    if (channel == nullptr)
    {
        return;
    }

    // Show every emote the channel's platforms provide, independent of the
    // currently selected send platform and login state (an empty platform list
    // means "all platforms" in loadChannel). Gating the browse popup on the
    // send platform left it completely empty whenever no send platform was
    // available (e.g. logged out, or a merged tab whose send target was unset).
    this->emotePopup_->loadChannel(channel, {});
}

void SplitInput::openBadgePickerPopup()
{
    const auto platforms = this->selectedSendPlatforms();
    const bool canUseTwitch =
        containsPlatform(platforms, MessagePlatform::AnyOrTwitch);
    auto currentAccount = getApp()->getAccounts()->twitch.getCurrent();
    auto channel = this->channelForSendPlatform(MessagePlatform::AnyOrTwitch);
    const auto twitch = std::dynamic_pointer_cast<TwitchChannel>(channel);
    if (!canUseTwitch || currentAccount == nullptr || currentAccount->isAnon() ||
        !isRealTwitchChatIdentityChannel(twitch))
    {
        return;
    }

    if (!this->badgePickerPopup_)
    {
        this->badgePickerPopup_ =
            new StreamDatabaseBadgePickerPopup(this->window());
        this->badgePickerPopup_->setAppliedCallback(
            [guard = QPointer<SplitInput>(this)] {
                if (guard != nullptr)
                {
                    guard->updateBadgeButton();
                }
            });
        QObject::connect(this, &QObject::destroyed, this->badgePickerPopup_,
                         &QWidget::close);
    }

    if (this->badgePickerPopup_->isVisible() ||
        this->badgePickerPopup_->wasRecentlyHidden())
    {
        this->badgePickerPopup_->hide();
        return;
    }

    this->updateBadgePickerContext();
    this->badgePickerPopup_->showForAnchor(this->ui_.badgeButton);
}

void SplitInput::updateBadgePickerContext()
{
    if (!this->badgePickerPopup_)
    {
        return;
    }

    QString channelName;
    QString channelID;
    std::shared_ptr<TwitchChannel> twitch;
    auto channel = this->channelForSendPlatform(MessagePlatform::AnyOrTwitch);
    if ((twitch = std::dynamic_pointer_cast<TwitchChannel>(channel)) &&
        isRealTwitchChatIdentityChannel(twitch))
    {
        channelName = twitch->getName();
        channelID = twitch->roomId();
    }
    else if (auto splitChannel = this->split_->getChannel())
    {
        channelName = splitChannel->getName();
    }

    this->badgePickerPopup_->setContext(channelName, channelID,
                                        this->selectedSendAccountName(), twitch);
}

QString SplitInput::handleSendMessage(const std::vector<QString> &arguments)
{
    auto c = this->split_->getChannel();
    if (c == nullptr)
    {
        return "";
    }

    struct SendTarget {
        ChannelPtr channel;
        YouTubeLiveChat *youtubeLiveChat{};
        std::optional<MessagePlatform> platform;
    };

    std::vector<SendTarget> sendTargets;
    auto *merged = dynamic_cast<MergedChannel *>(c.get());
    if (merged)
    {
        this->updatePlatformSelector();
        const auto sendPlatforms = this->selectedSendPlatforms();
        if (sendPlatforms.empty())
        {
            if (auto replyPlatform = this->replySendPlatform())
            {
                c->addSystemMessage(
                    QString(u"Log in to %1 to reply in merged chat."_s)
                        .arg(platformDisplayName(*replyPlatform)));
            }
            else
            {
                c->addSystemMessage(
                    u"Log in to Twitch, Kick, or YouTube to send merged chat."_s);
            }
            return "";
        }

        for (const auto platform : sendPlatforms)
        {
            auto sendChannel = this->channelForSendPlatform(platform);
            auto *youtubeLiveChat =
                platform == MessagePlatform::YouTube ? merged->youtubeLiveChat()
                                                     : nullptr;
            if ((!sendChannel && !youtubeLiveChat) ||
                !this->canSendToPlatform(platform))
            {
                c->addSystemMessage(
                    QString(u"Log in to %1 to send merged chat."_s)
                        .arg(platformDisplayName(platform)));
                return "";
            }
            sendTargets.push_back({sendChannel, youtubeLiveChat, platform});
        }
    }
    else
    {
        sendTargets.push_back({c, nullptr, std::nullopt});
    }

    QString message = this->ui_.textEdit->toPlainText();
    message = message.replace('\n', ' ');
    QString historyMessage = message;

    for (const auto &target : sendTargets)
    {
        const bool replyMatchesSendPlatform =
            !target.platform || this->replyTarget_ == nullptr ||
            this->replyTarget_->platform == *target.platform;

        if (this->replyTarget_ != nullptr && !replyMatchesSendPlatform)
        {
            continue;
        }

        if (target.youtubeLiveChat != nullptr)
        {
            QString sendMessage =
                getApp()->getCommands()->execCommand(message, c, false);
            target.youtubeLiveChat->sendMessage(sendMessage);
            continue;
        }

        if (!target.channel->isTwitchOrKickChannel() ||
            this->replyTarget_ == nullptr)
        {
            // standard message send behavior
            QString sendMessage = getApp()->getCommands()->execCommand(
                message, target.channel, false);

            target.channel->sendMessage(sendMessage);
            continue;
        }

        // Reply to message
        auto *tc = dynamic_cast<TwitchChannel *>(target.channel.get());
        auto *kc = dynamic_cast<KickChannel *>(target.channel.get());
        if (!tc && !kc)
        {
            // this should not fail
            continue;
        }

        QString replyMessage = message;
        if (this->enableInlineReplying_)
        {
            // Remove @username prefix that is inserted when doing inline replies
            replyMessage.remove(0, this->replyTarget_->displayName.length() +
                                       1);  // remove "@username"

            if (!replyMessage.isEmpty() && replyMessage.at(0) == ' ')
            {
                replyMessage.remove(0, 1);  // remove possible space
            }
        }

        QString sendMessage = getApp()->getCommands()->execCommand(
            replyMessage, target.channel, false);

        // Reply within TwitchChannel
        if (tc)
        {
            tc->sendReply(sendMessage, this->replyTarget_->id);
        }
        else if (kc)
        {
            kc->sendReply(sendMessage, this->replyTarget_->id);
        }

        if (sendTargets.size() == 1)
        {
            historyMessage = replyMessage;
        }
    }

    this->postMessageSend(historyMessage, arguments);
    return "";
}

void SplitInput::postMessageSend(const QString &message,
                                 const std::vector<QString> &arguments)
{
    // don't add duplicate messages and empty message to message history
    if ((this->prevMsg_.isEmpty() || !this->prevMsg_.endsWith(message)) &&
        !message.trimmed().isEmpty())
    {
        this->prevMsg_.append(message);
    }

    if (arguments.empty() || arguments.at(0) != "keepInput")
    {
        this->clearInput();
    }
    this->prevIndex_ = this->prevMsg_.size();
}

int SplitInput::scaledMaxHeight() const
{
    if (this->replyTarget_ != nullptr)
    {
        // give more space for showing the message being replied to
        return int(250 * this->scale());
    }
    else
    {
        return int(150 * this->scale());
    }
}

void SplitInput::addShortcuts()
{
    HotkeyController::HotkeyMap actions{
        {"cursorToStart",
         [this](const std::vector<QString> &arguments) -> QString {
             if (arguments.size() != 1)
             {
                 qCWarning(chatterinoHotkeys)
                     << "Invalid cursorToStart arguments. Argument 0: select "
                        "(\"withSelection\" or \"withoutSelection\")";
                 return "Invalid cursorToStart arguments. Argument 0: select "
                        "(\"withSelection\" or \"withoutSelection\")";
             }
             QTextCursor cursor = this->ui_.textEdit->textCursor();
             auto place = QTextCursor::Start;
             const auto &stringTakeSelection = arguments.at(0);
             bool select{};
             if (stringTakeSelection == "withSelection")
             {
                 select = true;
             }
             else if (stringTakeSelection == "withoutSelection")
             {
                 select = false;
             }
             else
             {
                 qCWarning(chatterinoHotkeys)
                     << "Invalid cursorToStart select argument (0)!";
                 return "Invalid cursorToStart select argument (0)!";
             }

             cursor.movePosition(place,
                                 select ? QTextCursor::MoveMode::KeepAnchor
                                        : QTextCursor::MoveMode::MoveAnchor);
             this->ui_.textEdit->setTextCursor(cursor);
             return "";
         }},
        {"cursorToEnd",
         [this](const std::vector<QString> &arguments) -> QString {
             if (arguments.size() != 1)
             {
                 qCWarning(chatterinoHotkeys)
                     << "Invalid cursorToEnd arguments. Argument 0: select "
                        "(\"withSelection\" or \"withoutSelection\")";
                 return "Invalid cursorToEnd arguments. Argument 0: select "
                        "(\"withSelection\" or \"withoutSelection\")";
             }
             QTextCursor cursor = this->ui_.textEdit->textCursor();
             auto place = QTextCursor::End;
             const auto &stringTakeSelection = arguments.at(0);
             bool select{};
             if (stringTakeSelection == "withSelection")
             {
                 select = true;
             }
             else if (stringTakeSelection == "withoutSelection")
             {
                 select = false;
             }
             else
             {
                 qCWarning(chatterinoHotkeys)
                     << "Invalid cursorToEnd select argument (0)!";
                 return "Invalid cursorToEnd select argument (0)!";
             }

             cursor.movePosition(place,
                                 select ? QTextCursor::MoveMode::KeepAnchor
                                        : QTextCursor::MoveMode::MoveAnchor);
             this->ui_.textEdit->setTextCursor(cursor);
             return "";
         }},
        {"openEmotesPopup",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->openEmotePopup();
             return "";
         }},
        {"cycleSendPlatform",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->tryCycleSendPlatform();
             return "";
         }},
        {"sendMessage",
         [this](const std::vector<QString> &arguments) -> QString {
             return this->handleSendMessage(arguments);
         }},
        {"previousMessage",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             if (this->prevMsg_.isEmpty() || this->prevIndex_ == 0)
             {
                 return "";
             }

             if (this->prevIndex_ == (this->prevMsg_.size()))
             {
                 this->currMsg_ = this->ui_.textEdit->toPlainText();
             }

             this->prevIndex_--;
             this->ui_.textEdit->setPlainText(
                 this->prevMsg_.at(this->prevIndex_));
             this->ui_.textEdit->resetCompletion();

             QTextCursor cursor = this->ui_.textEdit->textCursor();
             cursor.movePosition(QTextCursor::End);
             this->ui_.textEdit->setTextCursor(cursor);

             return "";
         }},
        {"nextMessage",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             // If user did not write anything before then just do nothing.
             if (this->prevMsg_.isEmpty())
             {
                 return "";
             }
             bool cursorToEnd = true;
             QString message = this->ui_.textEdit->toPlainText();

             if (this->prevIndex_ != (this->prevMsg_.size() - 1) &&
                 this->prevIndex_ != this->prevMsg_.size())
             {
                 this->prevIndex_++;
                 this->ui_.textEdit->setPlainText(
                     this->prevMsg_.at(this->prevIndex_));
                 this->ui_.textEdit->resetCompletion();
             }
             else
             {
                 this->prevIndex_ = this->prevMsg_.size();
                 if (message == this->prevMsg_.at(this->prevIndex_ - 1))
                 {
                     // If user has just come from a message history
                     // Then simply get currMsg_.
                     this->ui_.textEdit->setPlainText(this->currMsg_);
                     this->ui_.textEdit->resetCompletion();
                 }
                 else if (message != this->currMsg_)
                 {
                     // If user are already in current message
                     // And type something new
                     // Then replace currMsg_ with new one.
                     this->currMsg_ = message;
                 }
                 // If user is already in current message
                 // Then don't touch cursos.
                 cursorToEnd =
                     (message == this->prevMsg_.at(this->prevIndex_ - 1));
             }

             if (cursorToEnd)
             {
                 QTextCursor cursor = this->ui_.textEdit->textCursor();
                 cursor.movePosition(QTextCursor::End);
                 this->ui_.textEdit->setTextCursor(cursor);
             }
             return "";
         }},
        {"undo",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->ui_.textEdit->undo();
             return "";
         }},
        {"redo",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->ui_.textEdit->redo();
             return "";
         }},
        {"copy",
         [this](const std::vector<QString> &arguments) -> QString {
             // XXX: this action is unused at the moment, a qt standard shortcut is used instead
             if (arguments.empty())
             {
                 return "copy action takes only one argument: the source "
                        "of the copy \"split\", \"input\" or "
                        "\"auto\". If the source is \"split\", only text "
                        "from the chat will be copied. If it is "
                        "\"splitInput\", text from the input box will be "
                        "copied. Automatic will pick whichever has a "
                        "selection";
             }

             bool copyFromSplit = false;
             const auto &mode = arguments.at(0);
             if (mode == "split")
             {
                 copyFromSplit = true;
             }
             else if (mode == "splitInput")
             {
                 copyFromSplit = false;
             }
             else if (mode == "auto")
             {
                 const auto &cursor = this->ui_.textEdit->textCursor();
                 copyFromSplit = !cursor.hasSelection();
             }

             if (copyFromSplit)
             {
                 this->channelView_->copySelectedText();
             }
             else
             {
                 this->ui_.textEdit->copy();
             }
             return "";
         }},
        {"paste",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->ui_.textEdit->paste();
             return "";
         }},
        {"clear",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->clearInput();
             return "";
         }},
        {"selectAll",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->ui_.textEdit->selectAll();
             return "";
         }},
        {"selectWord",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             auto cursor = this->ui_.textEdit->textCursor();
             cursor.select(QTextCursor::WordUnderCursor);
             this->ui_.textEdit->setTextCursor(cursor);
             return "";
         }},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::SplitInput, actions, this->parentWidget());
}

bool SplitInput::eventFilter(QObject *obj, QEvent *event)
{
    const bool isPollPredictionButton =
        (this->ui_.predictionButton != nullptr &&
         obj == this->ui_.predictionButton) ||
        (this->ui_.pollButton != nullptr && obj == this->ui_.pollButton);
    if (isPollPredictionButton &&
        (event->type() == QEvent::Enter ||
         event->type() == QEvent::ToolTip))
    {
        this->updatePollPredictionButtons();
    }

    if (event->type() == QEvent::ShortcutOverride ||
        event->type() == QEvent::Shortcut)
    {
        if (auto *popup = this->inputCompletionPopup_.data())
        {
            if (popup->isVisible())
            {
                // Stop shortcut from triggering by saying we will handle it ourselves
                event->accept();

                // Return false means the underlying event isn't stopped, it will continue to propagate
                return false;
            }
        }
    }

    return BaseWidget::eventFilter(obj, event);
}

void SplitInput::installTextEditEvents()
{
    // We can safely ignore this signal's connection because SplitInput owns
    // the textEdit object, so it will always be deleted before SplitInput
    std::ignore =
        this->ui_.textEdit->keyPressed.connect([this](QKeyEvent *event) {
            if (event->key() == Qt::Key_Escape &&
                (event->modifiers() &
                 (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier |
                  Qt::MetaModifier)) == Qt::NoModifier &&
                this->replyTarget_ != nullptr)
            {
                this->clearReplyTarget();
                event->accept();
                return;
            }

            if (auto *popup = this->inputCompletionPopup_.data())
            {
                if (popup->isVisible())
                {
                    if (popup->eventFilter(nullptr, event))
                    {
                        event->accept();
                        return;
                    }
                }
            }

            if (event->key() == Qt::Key_D &&
                event->modifiers() == Qt::ControlModifier)
            {
                if (this->tryCycleSendPlatform())
                {
                    event->accept();
                    return;
                }
            }

            // One of the last remaining of it's kind, the copy shortcut.
            // For some bizarre reason Qt doesn't want this key be rebound.
            // TODO(Mm2PL): Revisit in Qt6, maybe something changed?
            if ((event->key() == Qt::Key_C || event->key() == Qt::Key_Insert) &&
                event->modifiers() == Qt::ControlModifier)
            {
                if (this->channelView_->hasSelection())
                {
                    this->channelView_->copySelectedText();
                    event->accept();
                }
            }
        });

    std::ignore = this->ui_.textEdit->contextMenuRequested.connect(
        [this](QMenu *menu, QPoint pos) {
#ifdef CHATTERINO_WITH_SPELLCHECK
            menu->addSeparator();
            auto *spellcheckAction = new QAction("Check spelling", menu);
            spellcheckAction->setCheckable(true);
            spellcheckAction->setChecked(this->shouldCheckSpelling());
            QObject::connect(spellcheckAction, &QAction::toggled, this,
                             [this](bool enabled) {
                                 this->checkSpellingOverride_ = enabled;
                                 this->checkSpellingChanged();
                             });
            menu->addAction(spellcheckAction);

            int nSuggestions = getSettings()->nSpellCheckingSuggestions;
            if (nSuggestions < 0)
            {
                nSuggestions = std::numeric_limits<int>::max();
            }

            if (!this->inputHighlighter || nSuggestions == 0)
            {
                return;
            }

            auto cursorAtPos = this->ui_.textEdit->cursorForPosition(pos);
            QString text = this->ui_.textEdit->toPlainText();
            QStringView word =
                this->inputHighlighter->getWordAt(text, cursorAtPos.position());
            if (!word.isEmpty())
            {
                auto cursor = this->ui_.textEdit->textCursor();
                // Select `word`. `word` is a view into `text`, so we can use
                // the offsets of `word` from the start of `text`.
                cursor.setPosition(
                    static_cast<int>(word.begin() - text.begin()));
                cursor.setPosition(static_cast<int>(word.end() - text.begin()),
                                   QTextCursor::KeepAnchor);

                auto suggestions =
                    getApp()->getSpellChecker()->suggestions(word.toString());
                for (const auto &sugg :
                     suggestions | std::views::take(nSuggestions))
                {
                    auto qSugg = QString::fromStdString(sugg);
                    menu->addAction(qSugg, [this, qSugg, cursor]() mutable {
                        cursor.insertText(qSugg);
                        this->ui_.textEdit->setTextCursor(cursor);
                    });
                }
            }
#else
            (void)menu;
            (void)pos;
            (void)this;
#endif
        });
}

void SplitInput::mousePressEvent(QMouseEvent *event)
{
    this->giveFocus(Qt::MouseFocusReason);

    if (this->hidden)
    {
        BaseWidget::mousePressEvent(event);
    }
    // else, don't call QWidget::mousePressEvent,
    // which will call event->ignore()
}

void SplitInput::onTextChanged()
{
    this->updateCompletionPopup();
}

void SplitInput::onCursorPositionChanged()
{
    this->updateCompletionPopup();
}

void SplitInput::updateCompletionPopup()
{
    auto *channel = this->split_->getChannel().get();
    bool showCommandCompletion = true;
    bool showEmoteCompletion = getSettings()->emoteCompletionWithColon;
    bool showUsernameCompletion =
        dynamic_cast<ChannelChatters *>(channel) != nullptr &&
        getSettings()->showUsernameCompletionMenu;
    if (!showCommandCompletion && !showEmoteCompletion &&
        !showUsernameCompletion)
    {
        this->hideCompletionPopup();
        return;
    }

    // check if in completion prefix
    auto &edit = *this->ui_.textEdit;

    auto text = edit.toPlainText();
    auto position = edit.textCursor().position() - 1;

    if (text.length() == 0 || position == -1)
    {
        this->hideCompletionPopup();
        return;
    }

    for (int i = std::clamp(position, 0, (int)text.length() - 1); i >= 0; i--)
    {
        if (text[i] == ' ')
        {
            this->hideCompletionPopup();
            return;
        }

        if (text[i] == '/' && showCommandCompletion)
        {
            if (text.left(i).trimmed().isEmpty())
            {
                this->showCompletionPopup(text.mid(i, position - i + 1),
                                          CompletionKind::Command);
            }
            else
            {
                this->hideCompletionPopup();
            }
            return;
        }

        if (text[i] == ':' && showEmoteCompletion)
        {
            if (i == 0 || text[i - 1].isSpace())
            {
                this->showCompletionPopup(text.mid(i, position - i + 1),
                                          CompletionKind::Emote);
            }
            else
            {
                this->hideCompletionPopup();
            }
            return;
        }

        if (text[i] == '@' && showUsernameCompletion)
        {
            if (i == 0 || text[i - 1].isSpace())
            {
                this->showCompletionPopup(text.mid(i, position - i + 1),
                                          CompletionKind::User);
            }
            else
            {
                this->hideCompletionPopup();
            }
            return;
        }
    }

    this->hideCompletionPopup();
}

void SplitInput::showCompletionPopup(const QString &text, CompletionKind kind)
{
    if (this->inputCompletionPopup_.isNull())
    {
        this->inputCompletionPopup_ = new InputCompletionPopup(this);
        this->inputCompletionPopup_->setInputAction(
            [that = QPointer(this)](const QString &text) mutable {
                if (auto *this2 = that.data())
                {
                    this2->insertCompletionText(text);
                    this2->hideCompletionPopup();
                }
            });
    }

    auto *popup = this->inputCompletionPopup_.data();
    assert(popup);

    auto channel = this->split_->getChannel();
    const auto platforms = this->selectedSendPlatforms();
    if ((kind == CompletionKind::Command || kind == CompletionKind::User) &&
        platforms.size() == 1)
    {
        channel = this->channelForSendPlatform(platforms.front());
    }

    if (channel == nullptr)
    {
        this->hideCompletionPopup();
        return;
    }

    popup->updateCompletion(text, kind, std::move(channel),
                            kind == CompletionKind::Emote
                                ? platforms
                                : std::vector<MessagePlatform>{});

    auto pos = this->mapToGlobal(QPoint{0, 0}) - QPoint(0, popup->height()) +
               QPoint((this->width() - popup->width()) / 2, 0);

    popup->move(pos);
    popup->show();
}

void SplitInput::hideCompletionPopup()
{
    if (auto *popup = this->inputCompletionPopup_.data())
    {
        popup->hide();
    }
}

void SplitInput::insertCompletionText(const QString &input_) const
{
    auto &edit = *this->ui_.textEdit;
    auto input = input_ + ' ';

    auto text = edit.toPlainText();
    auto position = edit.textCursor().position() - 1;

    for (int i = std::clamp(position, 0, (int)text.length() - 1); i >= 0; i--)
    {
        bool done = false;
        if (text[i] == ':')
        {
            done = true;
        }
        else if (text[i] == '/')
        {
            done = true;
        }
        else if (text[i] == '@')
        {
            const auto userMention =
                formatUserMention(input_, edit.isFirstWord(),
                                  getSettings()->mentionUsersWithComma);
            input = "@" + userMention + " ";
            done = true;
        }

        if (done)
        {
            auto cursor = edit.textCursor();
            edit.setPlainText(
                text.remove(i, position - i + 1).insert(i, input));

            cursor.setPosition(i + input.size());
            edit.setTextCursor(cursor);
            break;
        }
    }
}

bool SplitInput::hasSelection() const
{
    return this->ui_.textEdit->textCursor().hasSelection();
}

void SplitInput::clearSelection() const
{
    auto cursor = this->ui_.textEdit->textCursor();
    cursor.clearSelection();
    this->ui_.textEdit->setTextCursor(cursor);
}

bool SplitInput::isEditFirstWord() const
{
    return this->ui_.textEdit->isFirstWord();
}

QString SplitInput::getInputText() const
{
    return this->ui_.textEdit->toPlainText();
}

void SplitInput::insertText(const QString &text)
{
    this->ui_.textEdit->insertPlainText(text);
}

void SplitInput::hide()
{
    if (this->isHidden())
    {
        return;
    }

    this->hidden = true;
    this->setMaximumHeight(0);
    this->updateGeometry();
}

void SplitInput::show()
{
    if (!this->isHidden())
    {
        return;
    }

    this->hidden = false;
    this->setMaximumHeight(this->scaledMaxHeight());
    this->updateGeometry();
}

bool SplitInput::isHidden() const
{
    return this->hidden;
}

void SplitInput::setInputText(const QString &newInputText)
{
    this->ui_.textEdit->setPlainText(newInputText);
}

void SplitInput::editTextChanged()
{
    auto *app = getApp();

    // set textLengthLabel value
    QString text = this->ui_.textEdit->toPlainText();

    if (this->shouldPreventInput(text))
    {
        this->ui_.textEdit->setPlainText(text.left(TWITCH_MESSAGE_LIMIT));
        this->ui_.textEdit->moveCursor(QTextCursor::EndOfBlock);
        return;
    }

    if (text.startsWith("/r ", Qt::CaseInsensitive) &&
        this->split_->getChannel()->isTwitchChannel())
    {
        auto lastUser = app->getTwitch()->getLastUserThatWhisperedMe();
        if (!lastUser.isEmpty())
        {
            this->ui_.textEdit->setPlainText("/w " + lastUser + text.mid(2));
            this->ui_.textEdit->moveCursor(QTextCursor::EndOfBlock);
        }
    }
    else
    {
        this->textChanged.invoke(text);

        text = text.trimmed();
        text = app->getCommands()->execCommand(text, this->split_->getChannel(),
                                               true);
    }

    if (text.length() > 0 &&
        getSettings()->messageOverflow.getValue() == MessageOverflow::Highlight)
    {
        QTextCursor cursor = this->ui_.textEdit->textCursor();
        QTextCharFormat format;
        QList<QTextEdit::ExtraSelection> selections;

        cursor.setPosition(qMin(text.length(), TWITCH_MESSAGE_LIMIT),
                           QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::Start, QTextCursor::KeepAnchor);
        selections.append({cursor, format});

        if (text.length() > TWITCH_MESSAGE_LIMIT)
        {
            cursor.setPosition(TWITCH_MESSAGE_LIMIT, QTextCursor::MoveAnchor);
            cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
            format.setForeground(Qt::red);
            selections.append({cursor, format});
        }
        // block reemit of QTextEdit::textChanged()
        {
            const QSignalBlocker b(this->ui_.textEdit);
            this->ui_.textEdit->setExtraSelections(selections);
        }
    }

    QString labelText;

    if (text.length() > 0 && getSettings()->showMessageLength)
    {
        labelText = QString::number(text.length());
        if (text.length() > TWITCH_MESSAGE_LIMIT)
        {
            this->ui_.textEditLength->setStyleSheet("color: red");
        }
        else
        {
            this->ui_.textEditLength->setStyleSheet("");
        }
    }
    else
    {
        labelText = "";
    }

    this->ui_.textEditLength->setText(labelText);
    this->ui_.textEditLength->setVisible(!labelText.isEmpty());

    bool hasReply = false;
    if (this->enableInlineReplying_)
    {
        if (this->replyTarget_ != nullptr)
        {
            // Check if the input still starts with @username. If not, don't reply.
            //
            // We need to verify that
            // 1. the @username prefix exists and
            // 2. if a character exists after the @username, it is a space
            QString replyPrefix = "@" + this->replyTarget_->displayName;
            if (!text.startsWith(replyPrefix) ||
                (text.length() > replyPrefix.length() &&
                 text.at(replyPrefix.length()) != ' '))
            {
                this->clearReplyTarget();
            }
        }

        // Show/hide reply label if inline replies are possible
        hasReply = this->replyTarget_ != nullptr;
    }

    this->ui_.replyWrapper->setVisible(hasReply);
    this->ui_.replyLabel->setVisible(hasReply);
    this->ui_.cancelReplyButton->setVisible(hasReply);
}

void SplitInput::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);

    QColor borderColor =
        this->theme->isLightTheme() ? QColor("#ccc") : QColor("#333");

    auto *inputWrap = this->ui_.inputWrapper;
    auto inputBoxRect = inputWrap->geometry();
    inputBoxRect.setSize(inputBoxRect.size() - QSize{1, 1});

    const auto inputBackground = this->theme->splits.input.background;

    painter.setBrush(inputBackground);
    painter.fillRect(inputBoxRect, inputBackground);

    if (this->enableInlineReplying_ && this->replyTarget_ != nullptr)
    {
        auto replyRect = this->ui_.replyWrapper->geometry();
        replyRect.setSize(replyRect.size() - QSize{1, 1});

        painter.setBrush(this->theme->splits.input.background);
        painter.setPen(borderColor);
        painter.drawRect(replyRect);

        QPoint replyLabelBorderStart(
            replyRect.x(),
            replyRect.y() + this->ui_.replyHbox->geometry().height());
        QPoint replyLabelBorderEnd(replyRect.right(),
                                   replyLabelBorderStart.y());
        painter.drawLine(replyLabelBorderStart, replyLabelBorderEnd);
    }
}

void SplitInput::resizeEvent(QResizeEvent *event)
{
    (void)event;

    if (this->height() == this->maximumHeight())
    {
        this->ui_.textEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    }
    else
    {
        this->ui_.textEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    this->ui_.replyMessage->setWidth(this->replyMessageWidth());
}

void SplitInput::giveFocus(Qt::FocusReason reason)
{
    this->ui_.textEdit->setFocus(reason);
}

void SplitInput::setReply(MessagePtr target)
{
    auto oldParent = this->replyTarget_;
    if (this->enableInlineReplying_ && oldParent)
    {
        // Remove old reply prefix
        auto replyPrefix = "@" + oldParent->displayName;
        auto plainText = this->ui_.textEdit->toPlainText().trimmed();
        if (plainText.startsWith(replyPrefix))
        {
            plainText.remove(0, replyPrefix.length());
        }
        this->ui_.textEdit->setPlainText(plainText.trimmed());
        this->ui_.textEdit->moveCursor(QTextCursor::EndOfBlock);
        this->ui_.textEdit->resetCompletion();
    }

    if (target != nullptr)
    {
        this->replyTarget_ = std::move(target);

        if (this->enableInlineReplying_)
        {
            this->ui_.replyMessage->setWidth(this->replyMessageWidth());
            this->ui_.replyMessage->setMessage(this->replyTarget_);

            // add spacing between reply box and input box
            this->ui_.vbox->setSpacing(this->marginForTheme());
            if (!this->isHidden())
            {
                // update maximum height to give space for message
                this->setMaximumHeight(this->scaledMaxHeight());
            }

            // Only enable reply label if inline replying
            auto replyPrefix = "@" + this->replyTarget_->displayName;
            auto plainText = this->ui_.textEdit->toPlainText().trimmed();

            // This makes it so if plainText contains "@StreamerFan" and
            // we are replying to "@Streamer" we don't just leave "Fan"
            // in the text box
            if (plainText.startsWith(replyPrefix))
            {
                if (plainText.length() > replyPrefix.length())
                {
                    if (plainText.at(replyPrefix.length()) == ',' ||
                        plainText.at(replyPrefix.length()) == ' ')
                    {
                        plainText.remove(0, replyPrefix.length() + 1);
                    }
                }
                else
                {
                    plainText.remove(0, replyPrefix.length());
                }
            }
            if (!plainText.isEmpty() && !plainText.startsWith(' '))
            {
                replyPrefix.append(' ');
            }
            this->ui_.textEdit->setPlainText(replyPrefix + plainText + " ");
            this->ui_.textEdit->moveCursor(QTextCursor::EndOfBlock);
            this->ui_.textEdit->resetCompletion();
            this->ui_.replyLabel->setText("Replying to @" +
                                          this->replyTarget_->displayName);
        }

        this->updatePlatformSelector(true);
        this->sendPlatformChanged.invoke();
    }
    else
    {
        this->clearReplyTarget();
    }
}

void SplitInput::setPlaceholderText(const QString &text)
{
    this->ui_.textEdit->setPlaceholderText(text);
}

void SplitInput::clearInput()
{
    this->currMsg_ = "";
    this->ui_.textEdit->setText("");
    this->ui_.textEdit->moveCursor(QTextCursor::Start);
    this->clearReplyTarget();
}

void SplitInput::clearReplyTarget()
{
    const bool hadReply = this->replyTarget_ != nullptr;
    this->replyTarget_.reset();
    this->ui_.replyMessage->clearMessage();
    this->ui_.replyWrapper->hide();
    this->ui_.replyLabel->hide();
    this->ui_.cancelReplyButton->hide();
    this->ui_.vbox->setSpacing(0);
    if (!this->isHidden())
    {
        this->setMaximumHeight(this->scaledMaxHeight());
    }
    if (hadReply)
    {
        this->updatePlatformSelector(true);
        this->sendPlatformChanged.invoke();
    }
}

bool SplitInput::shouldPreventInput(const QString &text) const
{
    if (getSettings()->messageOverflow.getValue() != MessageOverflow::Prevent)
    {
        return false;
    }

    auto channel = this->split_->getChannel();

    if (channel == nullptr)
    {
        return false;
    }

    if (!channel->isTwitchChannel())
    {
        // Don't respect this setting for IRC channels as the limits might be server-specific
        return false;
    }

    return text.length() > TWITCH_MESSAGE_LIMIT;
}

int SplitInput::marginForTheme() const
{
    if (this->theme->isLightTheme())
    {
        return int(3 * this->scale());
    }
    else
    {
        return int(1 * this->scale());
    }
}

void SplitInput::applyOuterMargin()
{
    auto margin = std::max(this->marginForTheme() - 1, 0);
    this->ui_.vbox->setContentsMargins(margin, margin, margin, margin);
}

int SplitInput::replyMessageWidth() const
{
    return this->ui_.inputWrapper->width() - 1 - 10;
}

void SplitInput::updateTextEditPalette()
{
    QPalette p;

    // Placeholder text color
    p.setColor(QPalette::PlaceholderText,
               this->theme->messages.textColors.chatPlaceholder);

    // Text color
    p.setColor(QPalette::Text, this->theme->messages.textColors.regular);

    // Selection background color
    p.setBrush(QPalette::Highlight,
               this->theme->isLightTheme()
                   ? QColor(u"#68B1FF"_s)
                   : this->theme->tabs.selected.backgrounds.regular);

    // Background color
    p.setBrush(QPalette::Base, this->backgroundColor());

    this->ui_.textEdit->setPalette(p);
}

QColor SplitInput::backgroundColor() const
{
    return this->backgroundColor_;
}

void SplitInput::setBackgroundColor(QColor newColor)
{
    this->backgroundColor_ = newColor;

    this->updateTextEditPalette();
}

std::optional<bool> SplitInput::checkSpellingOverride() const
{
    return this->checkSpellingOverride_;
}

void SplitInput::setCheckSpellingOverride(std::optional<bool> override)
{
    this->checkSpellingOverride_ = override;
    this->checkSpellingChanged();
}

bool SplitInput::shouldCheckSpelling() const
{
    if (this->checkSpellingOverride_)
    {
        return *this->checkSpellingOverride_;
    }
    return getSettings()->enableSpellChecking;
}

void SplitInput::checkSpellingChanged()
{
    QTextDocument *target = nullptr;
    if (this->shouldCheckSpelling())
    {
        target = this->ui_.textEdit->document();
    }

    if (this->inputHighlighter->document() != target)
    {
        this->inputHighlighter->setDocument(target);
    }
}

void SplitInput::updateFonts()
{
    auto *app = getApp();
    this->ui_.textEdit->setFont(
        app->getFonts()->getFont(FontStyle::ChatMedium, this->scale()));

    // NOTE: We're using TimestampMedium here to get a font that uses the tnum font feature,
    // meaning numbers get equal width & don't bounce around while the user is typing.
    auto tsMedium =
        app->getFonts()->getFont(FontStyle::TimestampMedium, this->scale());
    this->ui_.textEditLength->setFont(tsMedium);
    this->ui_.sendWaitStatus->setFont(tsMedium);
    this->ui_.replyLabel->setFont(
        app->getFonts()->getFont(FontStyle::ChatMediumBold, this->scale()));
}

void SplitInput::setSendWaitStatus(const QString &text) const
{
    this->ui_.sendWaitStatus->setText(text);
    if (text.isEmpty())
    {
        this->ui_.sendWaitStatus->setHidden(true);
    }
    else
    {
        this->ui_.sendWaitStatus->setHidden(!getSettings()->showSendWaitTimer);
    }
}

}  // namespace chatterino
