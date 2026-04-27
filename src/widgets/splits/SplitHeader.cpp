// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitHeader.hpp"

#include "Application.hpp"
#include "common/network/NetworkCommon.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/hotkeys/Hotkey.hpp"
#include "controllers/hotkeys/HotkeyCategory.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/notifications/NotificationController.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/youtube/YouTubeLiveChat.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Resources.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/FormatTime.hpp"
#include "util/Helpers.hpp"
#include "util/LayoutHelper.hpp"
#include "widgets/buttons/DrawnButton.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/buttons/PixmapButton.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/helper/CommonTexts.hpp"
#include "widgets/Label.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/TooltipWidget.hpp"
#include "singletons/Fonts.hpp"

#include <QDrag>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMimeData>
#include <QPainter>

#include <cmath>
#include <limits>

namespace {

using namespace chatterino;

/// The width of the standard button.
constexpr const int BUTTON_WIDTH = 28;
constexpr const int QUEUED_SLOW_CHAT_COUNT_DIGITS = 4;

/// The width of the "Add split" button.
///
/// This matches the scrollbar's full width.
constexpr const int ADD_SPLIT_BUTTON_WIDTH = 16;

// 5 minutes
constexpr const qint64 THUMBNAIL_MAX_AGE_MS = 5LL * 60 * 1000;

auto formatRoomModeUnclean(const TwitchChannel::RoomModes &modes) -> QString
{
    QString text;

    if (modes.r9k)
    {
        text += "r9k, ";
    }
    if (modes.slowMode > 0)
    {
        text += QString("slow(%1), ").arg(localizeNumbers(modes.slowMode));
    }
    if (modes.emoteOnly)
    {
        text += "emote, ";
    }
    if (modes.submode)
    {
        text += "sub, ";
    }
    if (modes.followerOnly != -1)
    {
        if (modes.followerOnly != 0)
        {
            text += QString("follow(%1), ")
                        .arg(formatDurationExact(
                            std::chrono::minutes{modes.followerOnly}));
        }
        else
        {
            text += QString("follow, ");
        }
    }

    return text;
}

QString formatRoomModeUnclean(const KickChannel::RoomModes &modes)
{
    TwitchChannel::RoomModes twitch{
        .submode = modes.subscribersMode,
        .r9k = false,
        .emoteOnly = modes.emotesMode,
        .followerOnly = -1,
        .slowMode = 0,
    };
    if (modes.followersModeDuration)
    {
        twitch.followerOnly =
            static_cast<int>(modes.followersModeDuration->count());
    }
    if (modes.slowModeDuration)
    {
        twitch.slowMode = static_cast<int>(modes.slowModeDuration->count());
    }
    return formatRoomModeUnclean(twitch);
}

void cleanRoomModeText(QString &text, bool hasModRights)
{
    if (text.length() > 2)
    {
        text = text.mid(0, text.size() - 2);
    }

    if (!text.isEmpty())
    {
        static QRegularExpression commaReplacement("^(.+?, .+?,) (.+)$");

        auto match = commaReplacement.match(text);
        if (match.hasMatch())
        {
            text = match.captured(1) + '\n' + match.captured(2);
        }
    }

    if (text.isEmpty() && hasModRights)
    {
        text = "none";
    }
}

auto formatTooltip(const TwitchChannel::StreamStatus &s, QString thumbnail,
                   bool limitSize = false)
{
    auto title = [&s]() -> QString {
        if (s.title.isEmpty())
        {
            return QStringLiteral("");
        }

        return s.title.toHtmlEscaped() + "<br><br>";
    }();

    auto tooltip = [&]() -> QString {
        if (getSettings()->thumbnailSizeStream.getValue() == 0)
        {
            return QStringLiteral("");
        }

        if (thumbnail.isEmpty())
        {
            return QStringLiteral("Couldn't fetch thumbnail<br>");
        }

        QString sizeStr;
        if (limitSize)
        {
            auto height =
                std::min(getSettings()->thumbnailSizeStream.getValue(), 4) * 80;
            sizeStr =
                QStringLiteral(" height=\"") % QString::number(height) % '"';
        }

        return u"<img " % sizeStr % u" src=\"data:image/jpg;base64, " %
               thumbnail % u"\"><br>";
    }();

    auto game = [&s]() -> QString {
        if (s.game.isEmpty())
        {
            return QStringLiteral("");
        }

        return s.game.toHtmlEscaped() + "<br>";
    }();

    auto extraStreamData = [&s]() -> QString {
        if (getApp()->getStreamerMode()->isEnabled() &&
            getSettings()->streamerModeHideViewerCountAndDuration)
        {
            return QStringLiteral(
                "<span style=\"color: #808892;\">&lt;Streamer "
                "Mode&gt;</span>");
        }

        return QString("%1 for %2 with %3 viewers")
            .arg(s.rerun ? "Vod-casting" : "Live")
            .arg(s.uptime)
            .arg(localizeNumbers(s.viewerCount));
    }();

    return QString("<p style=\"text-align: center;\">" +  //
                   title +                                //
                   tooltip +                              //
                   game +                                 //
                   extraStreamData +                      //
                   "</p>"                                 //
    );
}

auto formatOfflineTooltip(const TwitchChannel::StreamStatus &s)
{
    return QString("<p style=\"text-align: center;\">Offline<br>%1</p>")
        .arg(s.title.toHtmlEscaped());
}

QString formatCompactStreamTooltip(const TwitchChannel::StreamStatus &s,
                                   const QString &thumbnail)
{
    constexpr qsizetype MAX_COMPACT_TITLE_LENGTH = 48;
    auto tooltip = QStringLiteral("<p style=\"text-align: center;\">");

    auto title = s.title.simplified();
    if (title.size() > MAX_COMPACT_TITLE_LENGTH)
    {
        title = title.left(MAX_COMPACT_TITLE_LENGTH - 3).trimmed() + "...";
    }
    if (!title.isEmpty())
    {
        tooltip += title.toHtmlEscaped() % u"<br>";
    }

    if (getSettings()->thumbnailSizeStream.getValue() != 0 &&
        !thumbnail.isEmpty())
    {
        tooltip += u"<img width=\"160\" height=\"90\" "
                   u"src=\"data:image/jpg;base64, " %
                   thumbnail % u"\"><br>";
    }

    if (!s.game.isEmpty())
    {
        tooltip += s.game.toHtmlEscaped() % u"<br>";
    }

    if (getApp()->getStreamerMode()->isEnabled() &&
        getSettings()->streamerModeHideViewerCountAndDuration)
    {
        tooltip += QStringLiteral(
            "<span style=\"color: #808892;\">&lt;Streamer Mode&gt;</span>");
    }
    else
    {
        tooltip += QString("%1 with %2 viewers")
                       .arg(s.rerun ? "Vod-casting" : "Live")
                       .arg(localizeNumbers(s.viewerCount));
    }

    tooltip += QStringLiteral("</p>");
    return tooltip;
}

TwitchChannel::StreamStatus toTwitchStreamStatus(
    const QString &title, uint64_t viewerCount)
{
    return {
        .live = true,
        .viewerCount = static_cast<unsigned>(
            std::min<uint64_t>(viewerCount,
                               std::numeric_limits<unsigned>::max())),
        .title = title,
        .streamType = QStringLiteral("live"),
    };
}

auto formatTitle(const TwitchChannel::StreamStatus &s, Settings &settings)
{
    auto title = QString();

    // live
    if (s.rerun)
    {
        title += " (rerun)";
    }
    else if (s.streamType.isEmpty())
    {
        title += " (" + s.streamType + ")";
    }
    else
    {
        title += " (live)";
    }

    // description
    if (settings.headerUptime)
    {
        title += " - " + s.uptime;
    }
    if (settings.headerViewerCount)
    {
        title += " - " + localizeNumbers(s.viewerCount);
    }
    if (settings.headerGame && !s.game.isEmpty())
    {
        title += " - " + s.game;
    }
    if (settings.headerStreamTitle && !s.title.isEmpty())
    {
        title += " - " + s.title.simplified();
    }

    return title;
}

TwitchChannel::StreamStatus toTwitchStreamStatus(
    const KickChannel::StreamData &data)
{
    return {
        .live = data.isLive,
        .viewerCount = static_cast<unsigned>(data.viewerCount),
        .title = data.title,
        .game = data.category,
        .uptime = data.uptime,
        .streamType = QStringLiteral("live"),
    };
}

auto distance(QPoint a, QPoint b)
{
    auto x = std::abs(a.x() - b.x());
    auto y = std::abs(a.y() - b.y());

    return std::sqrt(x * x + y * y);
}

QPixmap tintPixmap(const QPixmap &source, const QColor &color)
{
    QPixmap tinted(source.size());
    tinted.setDevicePixelRatio(source.devicePixelRatio());
    tinted.fill(Qt::transparent);

    QPainter painter(&tinted);
    painter.drawPixmap(0, 0, source);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(tinted.rect(), color);

    return tinted;
}

}  // namespace

namespace chatterino {

SplitHeader::SplitHeader(Split *split)
    : BaseWidget(split)
    , split_(split)
    , tooltipWidget_(new TooltipWidget(this))
{
    this->initializeLayout();

    this->setMouseTracking(true);
    this->updateChannelText();
    this->handleChannelChanged();
    this->updateIcons();

    // The lifetime of these signals are tied to the lifetime of the Split.
    // Since the SplitHeader is owned by the Split, they will always be destroyed
    // at the same time.
    std::ignore = this->split_->focused.connect([this]() {
        this->themeChangedEvent();
    });
    std::ignore = this->split_->focusLost.connect([this]() {
        this->themeChangedEvent();
    });
    std::ignore = this->split_->channelChanged.connect([this]() {
        this->handleChannelChanged();
    });

    this->bSignals_.emplace_back(
        getApp()->getAccounts()->twitch.currentUserChanged.connect([this] {
            this->updateIcons();
        }));

    auto _ = [this](const auto &, const auto &) {
        this->updateChannelText();
    };
    getSettings()->headerViewerCount.connect(_, this->managedConnections_);
    getSettings()->headerStreamTitle.connect(_, this->managedConnections_);
    getSettings()->headerGame.connect(_, this->managedConnections_);
    getSettings()->headerUptime.connect(_, this->managedConnections_);

    auto *window = dynamic_cast<BaseWindow *>(this->window());
    if (window)
    {
        // Hack: In some cases Qt doesn't send the leaveEvent the "actual" last mouse receiver.
        // This can happen when quickly moving the mouse out of the window and right clicking.
        // To prevent the tooltip from getting stuck, we use the window's leaveEvent.
        this->managedConnections_.managedConnect(window->leaving, [this] {
            if (this->tooltipWidget_->isVisible())
            {
                this->tooltipWidget_->hide();
            }
        });
    }

    this->scaleChangedEvent(this->scale());
}

void SplitHeader::setSlowChatQueueIndicatorReady(bool value)
{
    this->slowChatQueueIndicatorReady_ = value;
    this->updateIcons();
}

bool SplitHeader::eventFilter(QObject *watched, QEvent *event)
{
    const auto eventType = event->type();
    auto *target = qobject_cast<QWidget *>(watched);

    if (target != nullptr)
    {
        if (eventType == QEvent::Enter)
        {
            if (watched == this->titleLabel_ && !this->tooltipText_.isEmpty())
            {
                this->showHoverTooltip(target, this->tooltipText_, true);
            }
            else if (watched == this->queuedSlowChatCountLabel_ &&
                     !this->queuedSlowChatCountLabel_->getText().isEmpty())
            {
                this->showHoverTooltip(
                    target, "Queued messages waiting for slower chat.", false);
            }
            else if (watched == this->alertsButton_ &&
                     this->alertsButton_->isVisible())
            {
                this->showHoverTooltip(target,
                                       "Toggle the linked activity tab.", false);
            }
            else if (watched == this->moderationButton_ &&
                     this->moderationButton_->isVisible())
            {
                this->showHoverTooltip(target, "Toggle moderation mode.", false);
            }
        }
        else if (eventType == QEvent::Leave || eventType == QEvent::Hide ||
                 eventType == QEvent::MouseButtonPress)
        {
            this->hideHoverTooltip();
        }
    }

    return BaseWidget::eventFilter(watched, event);
}

void SplitHeader::showHoverTooltip(QWidget *target, const QString &text,
                                   bool wordWrap)
{
    if (target == nullptr || text.isEmpty())
    {
        this->hideHoverTooltip();
        return;
    }

    this->tooltipWidget_->setOne({nullptr, text});
    this->tooltipWidget_->setWordWrap(wordWrap);
    this->tooltipWidget_->adjustSize();

#ifdef Q_OS_WIN
    this->tooltipWidget_->show();
#endif

    auto pos = target->mapToGlobal(target->rect().bottomLeft()) +
               QPoint((target->width() - this->tooltipWidget_->width()) / 2, 1);

    this->tooltipWidget_->moveTo(pos, widgets::BoundsChecking::CursorPosition);

#ifndef Q_OS_WIN
    this->tooltipWidget_->show();
#endif
}

void SplitHeader::hideHoverTooltip()
{
    this->tooltipWidget_->hide();
}

void SplitHeader::initializeLayout()
{
    assert(this->layout() == nullptr);

    this->moderationButton_ = new SvgButton(
        {
            .dark = ":/buttons/moderationDisabled-darkMode.svg",
            .light = ":/buttons/moderationDisabled-lightMode.svg",
        },
        this, {5, 5});

    this->chattersButton_ = new SvgButton(
        {
            .dark = ":/buttons/chatters-darkMode.svg",
            .light = ":/buttons/chatters-lightMode.svg",
        },
        this, {4, 4});

    this->alertsButton_ = new SvgButton(
        {
            .dark = ":/buttons/alertsPane-darkMode.svg",
            .light = ":/buttons/alertsPane-lightMode.svg",
        },
        this, {4, 4});

    this->addButton_ = new DrawnButton(DrawnButton::Symbol::Plus,
                                       {
                                           .padding = 3,
                                           .thickness = 1,
                                       },
                                       this);

    this->dropdownButton_ =
        new DrawnButton(DrawnButton::Symbol::Kebab, {}, this);

    this->clearActivityButton_ = new PixmapButton(this);
    this->clearActivityButton_->setPixmap(
        tintPixmap(getResources().buttons.trashCan, Qt::white));
    this->clearActivityButton_->setScaleIndependentSize(BUTTON_WIDTH, 24);
    this->clearActivityButton_->hide();

    /// XXX: this never gets disconnected
    QObject::connect(this->dropdownButton_, &Button::leftMousePress, this,
                     [this] {
                         this->dropdownButton_->setMenu(this->createMainMenu());
                     });

    auto *layout = makeLayout<QHBoxLayout>({
        // space
        makeWidget<BaseWidget>([](auto w) {
            w->setScaleIndependentSize(8, 4);
        }),
        // title
        this->titleLabel_ = makeWidget<Label>([](auto w) {
            w->setSizePolicy(QSizePolicy::MinimumExpanding,
                             QSizePolicy::Preferred);
            w->setCentered(true);
            w->setPadding(QMargins{});
        }),
        // space
        makeWidget<BaseWidget>([](auto w) {
            w->setScaleIndependentSize(8, 4);
        }),
        // mode
        this->modeButton_ = makeWidget<LabelButton>([&](auto w) {
            w->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
            w->hide();
            w->setMenu(this->createChatModeMenu());
        }),
        // slower-chat queued count
        this->queuedSlowChatCountLabel_ = makeWidget<Label>([](auto w) {
            w->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
            w->setCentered(true);
            w->setFontStyle(FontStyle::UiMediumBold);
            w->setPadding(QMargins(5, 0, 5, 0));
            w->setText(QString());
        }),
        // alerts
        this->alertsButton_,
        // moderator
        this->moderationButton_,
        // chatter list
        this->chattersButton_,
        // activity clear
        this->clearActivityButton_,
        // dropdown
        this->dropdownButton_,
        // add split
        this->addButton_,
    });

    QObject::connect(
        this->moderationButton_, &Button::clicked, this,
        [this](Qt::MouseButton button) mutable {
            switch (button)
            {
                case Qt::LeftButton:
                    if (getSettings()->moderationActions.empty())
                    {
                        getApp()->getWindows()->showSettingsDialog(
                            this, SettingsDialogPreference::ModerationActions);
                        this->split_->setModerationMode(true);
                    }
                    else
                    {
                        auto moderationMode = this->split_->getModerationMode();

                        this->split_->setModerationMode(!moderationMode);
                        // w->setDim(moderationMode ? DimButton::Dim::Some
                        //                          : DimButton::Dim::None);
                    }
                    break;

                case Qt::RightButton:
                case Qt::MiddleButton:
                    getApp()->getWindows()->showSettingsDialog(
                        this, SettingsDialogPreference::ModerationActions);
                    break;

                default:
                    break;
            }
        });

    QObject::connect(this->chattersButton_, &Button::leftClicked, this,
                     [this]() {
                         this->split_->openChatterList();
                     });

    QObject::connect(this->alertsButton_, &Button::leftClicked, this, [this]() {
        this->split_->openAlertsPane();
    });

    QObject::connect(this->clearActivityButton_, &Button::leftClicked, this,
                     [this]() {
                         this->split_->clear();
                     });

    QObject::connect(this->addButton_, &Button::leftClicked, this, [this]() {
        this->split_->addSibling();
    });

    getSettings()->customURIScheme.connect(
        [this] {
            if (auto *const drop = this->dropdownButton_)
            {
                drop->setMenu(this->createMainMenu());
            }
        },
        this->managedConnections_);

    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    this->setLayout(layout);

    this->titleLabel_->installEventFilter(this);
    this->queuedSlowChatCountLabel_->installEventFilter(this);
    this->alertsButton_->installEventFilter(this);
    this->moderationButton_->installEventFilter(this);

    this->setAddButtonVisible(false);
}

void SplitHeader::updateThumbnail(const QString &url, bool followRedirects)
{
    if (url.isEmpty())
    {
        this->thumbnailUrl_.clear();
        this->thumbnail_.clear();
        this->lastThumbnail_.invalidate();
        return;
    }

    if (this->thumbnailUrl_ != url)
    {
        this->thumbnailUrl_ = url;
        this->thumbnail_.clear();
        this->lastThumbnail_.invalidate();
    }

    if (this->lastThumbnail_.isValid() &&
        this->lastThumbnail_.elapsed() <= THUMBNAIL_MAX_AGE_MS)
    {
        return;
    }

    auto request = NetworkRequest(url, NetworkRequestType::Get)
                       .caller(this)
                       .onSuccess([this, url](const auto &result) {
                           assert(!isAppAboutToQuit());

                           if (this->thumbnailUrl_ != url)
                           {
                               return;
                           }
                           if (result.status() == 200)
                           {
                               this->thumbnail_ = QString::fromLatin1(
                                   result.getData().toBase64());
                           }
                           else
                           {
                               this->thumbnail_.clear();
                           }
                           this->updateChannelText();
                       });
    if (followRedirects)
    {
        request = std::move(request).followRedirects(true);
    }
    std::move(request).execute();
    this->lastThumbnail_.restart();
}

QString SplitHeader::mergedStreamPreviewTooltip(MergedChannel *mergedChannel)
{
    if (!mergedChannel || !mergedChannel->isLive())
    {
        return {};
    }

    if (auto *twitch = dynamic_cast<TwitchChannel *>(
            mergedChannel->twitchChannel().get());
        twitch && twitch->isLive())
    {
        const auto streamStatus = twitch->accessStreamStatus();
        QString thumbnailUrl =
            "https://static-cdn.jtvnw.net/previews-ttv/live_user_" +
            twitch->getName().toLower();
        switch (getSettings()->thumbnailSizeStream.getValue())
        {
            case 1:
                thumbnailUrl.append("-80x45.jpg");
                break;
            case 2:
                thumbnailUrl.append("-160x90.jpg");
                break;
            case 3:
                thumbnailUrl.append("-360x203.jpg");
                break;
            default:
                thumbnailUrl.clear();
                break;
        }
        this->updateThumbnail(thumbnailUrl, false);
        return formatCompactStreamTooltip(*streamStatus, this->thumbnail_);
    }

    if (auto *kick = dynamic_cast<KickChannel *>(
            mergedChannel->kickChannel().get());
        kick && kick->isLive())
    {
        const auto &stream = kick->streamData();
        this->updateThumbnail(stream.thumbnailUrl, true);
        return formatCompactStreamTooltip(toTwitchStreamStatus(stream),
                                          this->thumbnail_);
    }

    if (auto *youtube = mergedChannel->youtubeLiveChat();
        youtube && youtube->isLive())
    {
        this->updateThumbnail(youtube->previewThumbnailUrl(), true);
        return formatCompactStreamTooltip(
            toTwitchStreamStatus(youtube->liveTitle(),
                                 youtube->liveViewerCount()),
            this->thumbnail_);
    }

    return {};
}

std::unique_ptr<QMenu> SplitHeader::createMainMenu()
{
    // top level menu
    const auto &h = getApp()->getHotkeys();
    auto menu = std::make_unique<QMenu>();
    menu->addAction(
        "Change channel",
        h->getDisplaySequence(HotkeyCategory::Split, "changeChannel"),
        this->split_, &Split::changeChannel);
    menu->addAction("Close",
                    h->getDisplaySequence(HotkeyCategory::Split, "delete"),
                    this->split_, &Split::deleteFromContainer);
    menu->addSeparator();
    menu->addAction(
        "Popup",
        h->getDisplaySequence(HotkeyCategory::Window, "popup", {{"split"}}),
        this->split_, &Split::popup);
    menu->addAction(
        "Popup overlay",
        h->getDisplaySequence(HotkeyCategory::Split, "popupOverlay"),
        this->split_, &Split::showOverlayWindow);
    menu->addAction("Search",
                    h->getDisplaySequence(HotkeyCategory::Split, "showSearch"),
                    this->split_, [this] {
                        this->split_->showSearch(true);
                    });
    if (this->split_->isActivityPane())
    {
        menu->addAction("Settings", this->split_, &Split::showSettingsDialog);
    }
    else
    {
        menu->addAction("Settings", this->split_, &Split::changeChannel);
    }
    menu->addSeparator();

    auto *twitchChannel =
        dynamic_cast<TwitchChannel *>(this->split_->getChannel().get());
    auto *kickChannel =
        dynamic_cast<KickChannel *>(this->split_->getChannel().get());

    if (twitchChannel || kickChannel)
    {
        menu->addAction(
            OPEN_IN_BROWSER,
            h->getDisplaySequence(HotkeyCategory::Split, "openInBrowser"),
            this->split_, &Split::openInBrowser);
        if (twitchChannel)
        {
            menu->addAction(OPEN_PLAYER_IN_BROWSER,
                            h->getDisplaySequence(HotkeyCategory::Split,
                                                  "openPlayerInBrowser"),
                            this->split_, &Split::openBrowserPlayer);
        }
        menu->addAction(
            OPEN_IN_STREAMLINK,
            h->getDisplaySequence(HotkeyCategory::Split, "openInStreamlink"),
            this->split_, &Split::openInStreamlink);

        if (!getSettings()->customURIScheme.getValue().isEmpty())
        {
            menu->addAction("Open in custom player",
                            h->getDisplaySequence(HotkeyCategory::Split,
                                                  "openInCustomPlayer"),
                            this->split_, &Split::openWithCustomScheme);
        }

        if (this->split_->getChannel()->hasModRights())
        {
            menu->addAction(
                OPEN_MOD_VIEW_IN_BROWSER,
                h->getDisplaySequence(HotkeyCategory::Split, "openModView"),
                this->split_, &Split::openModViewInBrowser);
        }

        if (twitchChannel)
        {
            menu->addAction(
                    "Create a clip",
                    h->getDisplaySequence(HotkeyCategory::Split, "createClip"),
                    this->split_,
                    [twitchChannel] {
                        twitchChannel->createClip({}, {});
                    })
                ->setVisible(twitchChannel->isLive());
        }

        if (this->split_->getIndirectChannel().getType() ==
            Channel::Type::TwitchWatching)
        {
            menu->addAction("Reset /watching", this->split_, [] {
                if (!getApp()
                         ->getTwitch()
                         ->getWatchingChannel()
                         .get()
                         ->isEmpty())
                {
                    getApp()->getTwitch()->setWatchingChannel(
                        Channel::getEmpty());
                }
            });
        }

        menu->addSeparator();
    }

    if (this->split_->getChannel()->getType() == Channel::Type::TwitchWhispers)
    {
        menu->addAction(
            OPEN_WHISPERS_IN_BROWSER,
            h->getDisplaySequence(HotkeyCategory::Split, "openInBrowser"),
            this->split_, &Split::openWhispersInBrowser);
        menu->addSeparator();
    }

    // reload / reconnect
    if (this->split_->getChannel()->canReconnect())
    {
        menu->addAction(
            "Reconnect",
            h->getDisplaySequence(HotkeyCategory::Split, "reconnect"), this,
            &SplitHeader::reconnect);
    }

    if (twitchChannel || kickChannel)
    {
        auto bothSeq = h->getDisplaySequence(
            HotkeyCategory::Split, "reloadEmotes", {std::vector<QString>()});
        auto channelSeq = h->getDisplaySequence(HotkeyCategory::Split,
                                                "reloadEmotes", {{"channel"}});
        auto subSeq = h->getDisplaySequence(HotkeyCategory::Split,
                                            "reloadEmotes", {{"subscriber"}});
        menu->addAction("Reload channel emotes",
                        channelSeq.isEmpty() ? bothSeq : channelSeq, this,
                        &SplitHeader::reloadChannelEmotes);
        if (twitchChannel)
        {
            menu->addAction("Reload subscriber emotes",
                            subSeq.isEmpty() ? bothSeq : subSeq, this,
                            &SplitHeader::reloadSubscriberEmotes);
        }
    }

    menu->addSeparator();

    {
        // "How to..." sub menu
        auto *subMenu = new QMenu("How to...", this);
        subMenu->addAction("move split", this->split_, &Split::explainMoving);
        subMenu->addAction("add/split", this->split_, &Split::explainSplitting);
        menu->addMenu(subMenu);
    }

    menu->addSeparator();

    // sub menu
    auto *moreMenu = new QMenu("More", this);

    auto modModeSeq = h->getDisplaySequence(HotkeyCategory::Split,
                                            "setModerationMode", {{"toggle"}});
    if (modModeSeq.isEmpty())
    {
        modModeSeq =
            h->getDisplaySequence(HotkeyCategory::Split, "setModerationMode",
                                  {std::vector<QString>()});
        // this makes a full std::optional<> with an empty vector inside
    }
    moreMenu->addAction(
        "Toggle moderation mode", modModeSeq, this->split_, [this]() {
            this->split_->setModerationMode(!this->split_->getModerationMode());
        });

    if (this->split_->getChannel()->getType() == Channel::Type::TwitchMentions)
    {
        auto *action = new QAction(this);
        action->setText("Enable /mention tab highlights");
        action->setCheckable(true);

        QObject::connect(moreMenu, &QMenu::aboutToShow, this, [action]() {
            action->setChecked(getSettings()->highlightMentions);
        });
        QObject::connect(action, &QAction::triggered, this, []() {
            getSettings()->highlightMentions =
                !getSettings()->highlightMentions;
        });

        moreMenu->addAction(action);
    }

    if (twitchChannel)
    {
        if (twitchChannel->hasModRights())
        {
            moreMenu->addAction(
                "Show chatter list",
                h->getDisplaySequence(HotkeyCategory::Split, "openViewerList"),
                this->split_, &Split::openChatterList);
        }

        moreMenu->addAction("Subscribe",
                            h->getDisplaySequence(HotkeyCategory::Split,
                                                  "openSubscriptionPage"),
                            this->split_, &Split::openSubPage);

        {
            auto *action = new QAction(this);
            action->setText("Notify when live");
            action->setCheckable(true);

            auto notifySeq = h->getDisplaySequence(
                HotkeyCategory::Split, "setChannelNotification", {{"toggle"}});
            if (notifySeq.isEmpty())
            {
                notifySeq = h->getDisplaySequence(HotkeyCategory::Split,
                                                  "setChannelNotification",
                                                  {std::vector<QString>()});
                // this makes a full std::optional<> with an empty vector inside
            }
            action->setShortcut(notifySeq);

            QObject::connect(
                moreMenu, &QMenu::aboutToShow, this, [action, this]() {
                    action->setChecked(
                        getApp()->getNotifications()->isChannelNotified(
                            this->split_->getChannel()->getName(),
                            Platform::Twitch));
                });
            QObject::connect(action, &QAction::triggered, this, [this]() {
                getApp()->getNotifications()->updateChannelNotification(
                    this->split_->getChannel()->getName(), Platform::Twitch);
            });

            moreMenu->addAction(action);
        }

        {
            auto *action = new QAction(this);
            action->setText("Mute highlight sounds");
            action->setCheckable(true);

            auto notifySeq = h->getDisplaySequence(
                HotkeyCategory::Split, "setHighlightSounds", {{"toggle"}});
            if (notifySeq.isEmpty())
            {
                notifySeq = h->getDisplaySequence(HotkeyCategory::Split,
                                                  "setHighlightSounds",
                                                  {std::vector<QString>()});
            }
            action->setShortcut(notifySeq);

            QObject::connect(
                moreMenu, &QMenu::aboutToShow, this, [action, this]() {
                    action->setChecked(getSettings()->isMutedChannel(
                        this->split_->getChannel()->getName()));
                });
            QObject::connect(action, &QAction::triggered, this, [this]() {
                getSettings()->toggleMutedChannel(
                    this->split_->getChannel()->getName());
            });

            moreMenu->addAction(action);
        }
    }

    moreMenu->addSeparator();
    moreMenu->addAction(
        "Clear messages",
        h->getDisplaySequence(HotkeyCategory::Split, "clearMessages"),
        this->split_, &Split::clear);
    //    moreMenu->addSeparator();
    //    moreMenu->addAction("Show changelog", this,
    //    SLOT(moreMenuShowChangelog()));
    menu->addMenu(moreMenu);

    return menu;
}

std::unique_ptr<QMenu> SplitHeader::createChatModeMenu()
{
    auto menu = std::make_unique<QMenu>();

    this->modeActionSetSub = new QAction("Subscriber only", this);
    this->modeActionSetEmote = new QAction("Emote only", this);
    this->modeActionSetSlow = new QAction("Slow", this);
    this->modeActionSetR9k = new QAction("R9K", this);
    this->modeActionSetFollowers = new QAction("Followers only", this);

    this->modeActionSetFollowers->setCheckable(true);
    this->modeActionSetSub->setCheckable(true);
    this->modeActionSetEmote->setCheckable(true);
    this->modeActionSetSlow->setCheckable(true);
    this->modeActionSetR9k->setCheckable(true);

    menu->addAction(this->modeActionSetEmote);
    menu->addAction(this->modeActionSetSub);
    menu->addAction(this->modeActionSetSlow);
    menu->addAction(this->modeActionSetR9k);
    menu->addAction(this->modeActionSetFollowers);

    auto execCommand = [this](const QString &command) {
        auto text = getApp()->getCommands()->execCommand(
            command, this->split_->getChannel(), false);
        this->split_->getChannel()->sendMessage(text);
    };
    auto toggle = [execCommand](const QString &command,
                                QAction *action) mutable {
        execCommand(command + (action->isChecked() ? "" : "off"));
        action->setChecked(!action->isChecked());
    };

    QObject::connect(this->modeActionSetSub, &QAction::triggered, this,
                     [this, toggle]() mutable {
                         toggle("/subscribers", this->modeActionSetSub);
                     });

    QObject::connect(this->modeActionSetEmote, &QAction::triggered, this,
                     [this, toggle]() mutable {
                         toggle("/emoteonly", this->modeActionSetEmote);
                     });

    QObject::connect(this->modeActionSetSlow, &QAction::triggered, this,
                     [this, execCommand]() {
                         if (!this->modeActionSetSlow->isChecked())
                         {
                             execCommand("/slowoff");
                             this->modeActionSetSlow->setChecked(false);
                             return;
                         };
                         auto ok = bool();
                         auto seconds = QInputDialog::getInt(
                             this, "", "Seconds:", 10, 0, 500, 1, &ok,
                             Qt::FramelessWindowHint);
                         if (ok)
                         {
                             execCommand(QString("/slow %1").arg(seconds));
                         }
                         else
                         {
                             this->modeActionSetSlow->setChecked(false);
                         }
                     });

    QObject::connect(this->modeActionSetFollowers, &QAction::triggered, this,
                     [this, execCommand]() {
                         if (!this->modeActionSetFollowers->isChecked())
                         {
                             execCommand("/followersoff");
                             this->modeActionSetFollowers->setChecked(false);
                             return;
                         };
                         auto ok = bool();
                         auto time = QInputDialog::getText(
                             this, "", "Time:", QLineEdit::Normal, "15m", &ok,
                             Qt::FramelessWindowHint,
                             Qt::ImhLowercaseOnly | Qt::ImhPreferNumbers);
                         if (ok)
                         {
                             execCommand(QString("/followers %1").arg(time));
                         }
                         else
                         {
                             this->modeActionSetFollowers->setChecked(false);
                         }
                     });

    QObject::connect(this->modeActionSetR9k, &QAction::triggered, this,
                     [this, toggle]() mutable {
                         toggle("/r9kbeta", this->modeActionSetR9k);
                     });

    return menu;
}

void SplitHeader::updateRoomModes()
{
    assert(this->modeButton_ != nullptr);

    // Update the mode button
    if (auto *twitchChannel =
            dynamic_cast<TwitchChannel *>(this->split_->getChannel().get()))
    {
        this->modeButton_->setEnabled(twitchChannel->hasModRights());

        QString text;
        {
            auto roomModes = twitchChannel->accessRoomModes();
            text = formatRoomModeUnclean(*roomModes);

            // Set menu action
            this->modeActionSetR9k->setChecked(roomModes->r9k);
            this->modeActionSetSlow->setChecked(roomModes->slowMode > 0);
            this->modeActionSetEmote->setChecked(roomModes->emoteOnly);
            this->modeActionSetSub->setChecked(roomModes->submode);
            this->modeActionSetFollowers->setChecked(roomModes->followerOnly !=
                                                     -1);
        }
        cleanRoomModeText(text, twitchChannel->hasModRights());

        // set the label text

        if (!text.isEmpty())
        {
            this->modeButton_->setText(text);
            this->modeButton_->show();
        }
        else
        {
            this->modeButton_->hide();
        }

        // Update the mode button menu actions
    }
    else if (auto *kc =
                 dynamic_cast<KickChannel *>(this->split_->getChannel().get()))
    {
        this->modeButton_->setEnabled(false);

        QString text = formatRoomModeUnclean(kc->roomModes());
        cleanRoomModeText(text, false);

        if (!text.isEmpty())
        {
            this->modeButton_->setText(text);
            this->modeButton_->show();
        }
        else
        {
            this->modeButton_->hide();
        }
    }
    else
    {
        this->modeButton_->hide();
    }
}

void SplitHeader::resetThumbnail()
{
    this->lastThumbnail_.invalidate();
    this->thumbnail_.clear();
    this->thumbnailUrl_.clear();
}

void SplitHeader::handleChannelChanged()
{
    this->resetThumbnail();

    this->updateChannelText();

    this->channelConnections_.clear();

    auto channel = this->split_->getChannel();
    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        this->channelConnections_.managedConnect(
            twitchChannel->streamStatusChanged, [this]() {
                this->updateChannelText();
            });
    }
    else if (auto *kickChannel = dynamic_cast<KickChannel *>(channel.get()))
    {
        this->channelConnections_.managedConnect(kickChannel->streamDataChanged,
                                                 [this]() {
                                                     this->updateChannelText();
                                                 });
    }
    else if (auto *mergedChannel = dynamic_cast<MergedChannel *>(channel.get()))
    {
        this->channelConnections_.managedConnect(
            mergedChannel->streamStatusChanged, [this]() {
                this->updateChannelText();
            });
    }
}

void SplitHeader::scaleChangedEvent(float scale)
{
    int w = int(BUTTON_WIDTH * scale);
    int addSplitWidth = int(ADD_SPLIT_BUTTON_WIDTH * scale);

    this->setFixedHeight(w);
    this->dropdownButton_->setFixedWidth(w);
    this->alertsButton_->setFixedWidth(w);
    this->moderationButton_->setFixedWidth(w);
    this->chattersButton_->setFixedWidth(w);
    const auto queuedCountFont =
        getApp()->getFonts()->getFont(FontStyle::UiMediumBold, scale);
    const auto queuedCountMetrics = QFontMetrics(queuedCountFont);
    const auto queuedCountWidth =
        queuedCountMetrics.horizontalAdvance(
            QString(QUEUED_SLOW_CHAT_COUNT_DIGITS, QChar(u'9'))) +
        static_cast<int>(std::round(12 * scale));
    this->queuedSlowChatCountLabel_->setFixedWidth(queuedCountWidth);

    this->addButton_->setFixedWidth(addSplitWidth);
}

void SplitHeader::setAddButtonVisible(bool value)
{
    this->addButton_->setVisible(value && !this->split_->isActivityPane());
}

void SplitHeader::updateChannelText()
{
    auto indirectChannel = this->split_->getIndirectChannel();
    auto channel = this->split_->getChannel();
    this->isLive_ = false;
    this->tooltipText_ = QString();

    auto title = channel->getLocalizedName();
    QString deltaText;
    QColor deltaColor;

    if (indirectChannel.getType() == Channel::Type::TwitchWatching)
    {
        title = "watching: " + (title.isEmpty() ? "none" : title);
    }

    if (this->split_->isActivityPane())
    {
        this->resetThumbnail();
        this->titleLabel_->setText(this->split_->activityPaneTitle());
        return;
    }

    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        const auto streamStatus = twitchChannel->accessStreamStatus();

        if (streamStatus->live)
        {
            this->isLive_ = true;
            // XXX: This URL format can be figured out from the Helix Get Streams API which we parse in TwitchChannel::parseLiveStatus
            QString url = "https://static-cdn.jtvnw.net/"
                          "previews-ttv/live_user_" +
                          channel->getName().toLower();
            switch (getSettings()->thumbnailSizeStream.getValue())
            {
                case 1:
                    url.append("-80x45.jpg");
                    break;
                case 2:
                    url.append("-160x90.jpg");
                    break;
                case 3:
                    url.append("-360x203.jpg");
                    break;
                default:
                    url = "";
            }
            this->updateThumbnail(url, false);
            this->tooltipText_ = formatTooltip(*streamStatus, this->thumbnail_);
            title += formatTitle(*streamStatus, *getSettings());
            if (getSettings()->headerViewerCount &&
                streamStatus->viewerCount > 0)
            {
                const auto delta =
                    this->viewerDeltaTracker_.sampleAndCompute(
                        streamStatus->viewerCount,
                        QDateTime::currentMSecsSinceEpoch(),
                        getSettings()
                            ->mergedViewerDeltaWindowMinutes.getValue());
                if (delta && std::abs(delta->percent) >= 0.1)
                {
                    deltaText = QString(" (%1%2% / %3 min)")
                                    .arg(delta->percent >= 0 ? "+" : "")
                                    .arg(delta->percent, 0, 'f', 1)
                                    .arg(delta->spanMinutes);
                    deltaColor = delta->percent >= 0
                                     ? QColor(0x4c, 0xaf, 0x50)
                                     : QColor(0xef, 0x53, 0x50);
                }
            }
        }
        else
        {
            this->tooltipText_ = formatOfflineTooltip(*streamStatus);
            this->viewerDeltaTracker_.clear();
        }
    }
    else if (auto *kickChannel = dynamic_cast<KickChannel *>(channel.get()))
    {
        const auto &stream = kickChannel->streamData();
        auto twitch = toTwitchStreamStatus(stream);
        if (stream.isLive)
        {
            this->isLive_ = true;
            this->updateThumbnail(stream.thumbnailUrl, true);
            this->tooltipText_ = formatTooltip(twitch, this->thumbnail_, true);
            title += formatTitle(twitch, *getSettings());
            if (getSettings()->headerViewerCount && stream.viewerCount > 0)
            {
                const auto delta =
                    this->viewerDeltaTracker_.sampleAndCompute(
                        static_cast<unsigned>(stream.viewerCount),
                        QDateTime::currentMSecsSinceEpoch(),
                        getSettings()
                            ->mergedViewerDeltaWindowMinutes.getValue());
                if (delta && std::abs(delta->percent) >= 0.1)
                {
                    deltaText = QString(" (%1%2% / %3 min)")
                                    .arg(delta->percent >= 0 ? "+" : "")
                                    .arg(delta->percent, 0, 'f', 1)
                                    .arg(delta->spanMinutes);
                    deltaColor = delta->percent >= 0
                                     ? QColor(0x4c, 0xaf, 0x50)
                                     : QColor(0xef, 0x53, 0x50);
                }
            }
        }
        else
        {
            this->tooltipText_ = formatOfflineTooltip(twitch);
            this->viewerDeltaTracker_.clear();
        }
    }
    else if (auto *mergedChannel = dynamic_cast<MergedChannel *>(channel.get()))
    {
        this->isLive_ = mergedChannel->isLive();
        this->tooltipText_ = this->mergedStreamPreviewTooltip(mergedChannel);
        if (this->tooltipText_.isEmpty())
        {
            this->tooltipText_ = mergedChannel->tooltipText();
        }
        if (this->isLive_)
        {
            title += mergedChannel->statusSuffix();
            if (getSettings()->headerUptime)
            {
                QString uptime;
                if (auto twitchSource = mergedChannel->twitchChannel())
                {
                    if (auto *twitch = dynamic_cast<TwitchChannel *>(
                            twitchSource.get()))
                    {
                        const auto status = twitch->accessStreamStatus();
                        if (status->live)
                        {
                            uptime = status->uptime;
                        }
                    }
                }
                if (uptime.isEmpty())
                {
                    if (auto kickSource = mergedChannel->kickChannel())
                    {
                        if (auto *kick = dynamic_cast<KickChannel *>(
                                kickSource.get()))
                        {
                            const auto &stream = kick->streamData();
                            if (stream.isLive)
                            {
                                uptime = stream.uptime;
                            }
                        }
                    }
                }
                if (!uptime.isEmpty())
                {
                    title += " - " + uptime;
                }
            }
            if (getSettings()->headerViewerCount)
            {
                const auto totalViewers = mergedChannel->totalViewerCount();
                if (totalViewers > 0)
                {
                    title += " - " + localizeNumbers(totalViewers);
                    const auto delta = mergedChannel->viewerCountDeltaPercent();
                    if (delta.has_value() && std::abs(delta->percent) >= 0.1)
                    {
                        deltaText = QString(" (%1%2% / %3 min)")
                                        .arg(delta->percent >= 0 ? "+" : "")
                                        .arg(delta->percent, 0, 'f', 1)
                                        .arg(delta->spanMinutes);
                        deltaColor = delta->percent >= 0
                                         ? QColor(0x4c, 0xaf, 0x50)
                                         : QColor(0xef, 0x53, 0x50);
                    }
                }
            }
        }
    }

    if (this->split_->isActivityPane())
    {
        title = this->split_->activityPaneTitle();
        deltaText.clear();
    }
    else if (!title.isEmpty() && !this->split_->getFilters().empty())
    {
        title += " - filtered";
    }

    this->titleLabel_->setText(title.isEmpty() ? "<empty>" : title);
    this->titleLabel_->setTrailingText(deltaText, deltaColor);
}

void SplitHeader::updateIcons()
{
    auto channel = this->split_->getChannel();
    auto activityInactive = this->theme->splits.header.text;
    activityInactive.setAlpha(this->theme->isLightTheme() ? 120 : 150);
    const auto activityActive = this->theme->isLightTheme()
                                    ? QColor(24, 24, 24)
                                    : QColor(255, 255, 255);

    if (this->split_->isActivityPane())
    {
        this->queuedSlowChatCountLabel_->hide();
        this->queuedSlowChatCountLabel_->setText(QString());
        this->alertsButton_->hide();
        this->moderationButton_->hide();
        this->clearActivityButton_->show();

        if (channel->hasModRights() && channel->isTwitchChannel())
        {
            this->chattersButton_->show();
        }
        else
        {
            this->chattersButton_->hide();
        }

        return;
    }
    this->clearActivityButton_->hide();

    if (this->slowChatQueueIndicatorReady_ && this->split_->slowerChatEnabled())
    {
        const int queuedSlowChatMessages =
            this->split_->getChannelView().pendingSlowChatMessageCount();
        if (queuedSlowChatMessages > 0)
        {
            this->queuedSlowChatCountLabel_->setText(
                localizeNumbers(queuedSlowChatMessages));
        }
        else
        {
            this->queuedSlowChatCountLabel_->setText(QString());
        }
    }
    else
    {
        this->queuedSlowChatCountLabel_->setText(QString());
    }
    this->queuedSlowChatCountLabel_->show();

    this->alertsButton_->show();
    this->alertsButton_->setColor(this->split_->hasLinkedActivityPane()
                                      ? std::optional<QColor>(activityActive)
                                      : std::optional<QColor>(activityInactive));

    if (channel->isTwitchOrKickChannel() || channel->isMergedChannel())
    {
        auto moderationMode = this->split_->getModerationMode() &&
                              !getSettings()->moderationActions.empty();

        if (moderationMode)
        {
            this->moderationButton_->setSource({
                .dark = ":/buttons/moderationEnabled-darkMode.svg",
                .light = ":/buttons/moderationEnabled-lightMode.svg",
            });
        }
        else
        {
            this->moderationButton_->setSource({
                .dark = ":/buttons/moderationDisabled-darkMode.svg",
                .light = ":/buttons/moderationDisabled-lightMode.svg",
            });
        }

        if (channel->hasModRights() || moderationMode)
        {
            this->moderationButton_->show();
        }
        else
        {
            this->moderationButton_->hide();
        }

        if (channel->hasModRights() && channel->isTwitchChannel())
        {
            this->chattersButton_->show();
        }
        else
        {
            this->chattersButton_->hide();
        }
    }
    else
    {
        this->moderationButton_->hide();
        this->chattersButton_->hide();
    }
}

void SplitHeader::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);

    QColor background = this->theme->splits.header.background;
    QColor border = this->theme->splits.header.border;

    if (this->split_->hasFocus())
    {
        background = this->theme->splits.header.focusedBackground;
        border = this->theme->splits.header.focusedBorder;
    }

    painter.fillRect(this->rect(), background);
    painter.setPen(border);
    painter.drawRect(0, 0, this->width() - 1, this->height() - 2);
    painter.fillRect(0, this->height() - 1, this->width(), 1, background);
}

void SplitHeader::mousePressEvent(QMouseEvent *event)
{
    switch (event->button())
    {
        case Qt::LeftButton: {
            this->split_->setFocus(Qt::MouseFocusReason);

            this->dragging_ = true;

            this->dragStart_ = event->pos();
        }
        break;

        case Qt::RightButton: {
            auto *menu = this->createMainMenu().release();
            menu->setAttribute(Qt::WA_DeleteOnClose);
            menu->popup(this->mapToGlobal(event->pos() + QPoint(0, 4)));
        }
        break;

        case Qt::MiddleButton: {
            this->split_->openInBrowser();
        }
        break;

        default: {
        }
        break;
    }

    this->doubleClicked_ = false;
}

void SplitHeader::mouseReleaseEvent(QMouseEvent * /*event*/)
{
    this->dragging_ = false;
}

void SplitHeader::mouseMoveEvent(QMouseEvent *event)
{
    if (this->dragging_)
    {
        if (distance(this->dragStart_, event->pos()) > 15 * this->scale())
        {
            this->split_->drag();
            this->dragging_ = false;
        }
    }
}

void SplitHeader::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        this->dragging_ = false;
        if (this->split_->isActivityPane())
        {
            this->split_->showSettingsDialog();
        }
        else
        {
            this->split_->changeChannel();
        }
    }
    this->doubleClicked_ = true;
}

void SplitHeader::enterEvent(QEnterEvent *event)
{
    BaseWidget::enterEvent(event);
}

void SplitHeader::leaveEvent(QEvent *event)
{
    this->hideHoverTooltip();

    BaseWidget::leaveEvent(event);
}

void SplitHeader::themeChangedEvent()
{
    auto palette = QPalette();

    if (this->split_->hasFocus())
    {
        palette.setColor(QPalette::WindowText,
                         this->theme->splits.header.focusedText);
    }
    else
    {
        palette.setColor(QPalette::WindowText, this->theme->splits.header.text);
    }
    this->titleLabel_->setPalette(palette);

    auto queuedPalette = QPalette();
    queuedPalette.setColor(QPalette::WindowText, this->theme->splits.header.text);
    this->queuedSlowChatCountLabel_->setPalette(queuedPalette);

    auto bg = this->theme->splits.header.background;
    this->addButton_->setOptions({
        .background = bg,
        .backgroundHover = bg,
    });

    this->updateIcons();
    this->update();
}

void SplitHeader::reloadChannelEmotes()
{
    using namespace std::chrono_literals;

    auto now = std::chrono::steady_clock::now();
    if (this->lastReloadedChannelEmotes_ + 30s > now)
    {
        return;
    }
    this->lastReloadedChannelEmotes_ = now;

    auto channel = this->split_->getChannel();

    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        twitchChannel->refreshFFZChannelEmotes(true);
        twitchChannel->refreshBTTVChannelEmotes(true);
        twitchChannel->refreshSevenTVChannelEmotes(true);
    }
    else if (auto *kc = dynamic_cast<KickChannel *>(channel.get()))
    {
        kc->reloadSeventvEmotes(true);
    }
}

void SplitHeader::reloadSubscriberEmotes()
{
    using namespace std::chrono_literals;

    auto now = std::chrono::steady_clock::now();
    if (this->lastReloadedSubEmotes_ + 30s > now)
    {
        return;
    }
    this->lastReloadedSubEmotes_ = now;

    auto channel = this->split_->getChannel();
    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        twitchChannel->refreshTwitchChannelEmotes(true);
    }
}

void SplitHeader::reconnect()
{
    this->split_->getChannel()->reconnect();
}

}  // namespace chatterino
