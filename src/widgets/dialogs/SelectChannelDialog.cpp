// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/SelectChannelDialog.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/tiktok/TikTokLiveChat.hpp"
#include "singletons/Settings.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "widgets/helper/MicroNotebook.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QRadioButton>
#include <QRegularExpression>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace chatterino::detail {

void AutoCheckedRadioButton::focusInEvent(QFocusEvent *event)
{
    this->setChecked(true);
    QRadioButton::focusInEvent(event);
}

}  // namespace chatterino::detail

namespace chatterino {

namespace {

int indicatorModeIndex(PlatformIndicatorMode mode)
{
    switch (mode)
    {
        case PlatformIndicatorMode::Badge:
            return 1;
        case PlatformIndicatorMode::Both:
            return 2;
        case PlatformIndicatorMode::LineColor:
        default:
            return 0;
    }
}

PlatformIndicatorMode indicatorModeFromIndex(int index)
{
    switch (index)
    {
        case 1:
            return PlatformIndicatorMode::Badge;
        case 2:
            return PlatformIndicatorMode::Both;
        case 0:
        default:
            return PlatformIndicatorMode::LineColor;
    }
}

bool isLikelyYouTubeChannelId(const QString &value)
{
    static const QRegularExpression channelIdRegex("^UC[A-Za-z0-9_-]{22}$");
    return channelIdRegex.match(value.trimmed()).hasMatch();
}

bool isLikelyYouTubeVideoId(const QString &value)
{
    static const QRegularExpression videoIdRegex("^[A-Za-z0-9_-]{11}$");
    return videoIdRegex.match(value.trimmed()).hasMatch();
}

QString trimTrailingSlash(QString value)
{
    while (value.size() > 1 && value.endsWith('/'))
    {
        value.chop(1);
    }

    return value;
}

QString maybeExtractYouTubeVideoId(QString value)
{
    value = value.trimmed();
    if (value.isEmpty())
    {
        return {};
    }

    if (isLikelyYouTubeVideoId(value))
    {
        return value;
    }

    if (!value.contains("://") &&
        (value.startsWith("www.", Qt::CaseInsensitive) ||
         value.startsWith("youtube.com/", Qt::CaseInsensitive) ||
         value.startsWith("m.youtube.com/", Qt::CaseInsensitive) ||
         value.startsWith("music.youtube.com/", Qt::CaseInsensitive) ||
         value.startsWith("youtu.be/", Qt::CaseInsensitive)))
    {
        value.prepend("https://");
    }

    const QUrl url(value);
    if (!url.isValid())
    {
        return {};
    }

    const auto host = url.host().toLower();
    const auto path = url.path();
    const QUrlQuery query(url);

    if (query.hasQueryItem("v"))
    {
        return query.queryItemValue("v").trimmed();
    }

    if (host.endsWith("youtu.be"))
    {
        return path.sliced(1).section('/', 0, 0).trimmed();
    }

    if (path.startsWith("/live/") || path.startsWith("/shorts/") ||
        path.startsWith("/embed/"))
    {
        return path.section('/', 2, 2).trimmed();
    }

    return {};
}

QString normalizeYouTubeSource(QString value)
{
    value = value.trimmed();
    if (value.isEmpty())
    {
        return {};
    }

    if (value.startsWith('@'))
    {
        return value.section('/', 0, 0).trimmed();
    }

    if (isLikelyYouTubeChannelId(value))
    {
        return value;
    }

    if (const auto videoId = maybeExtractYouTubeVideoId(value);
        !videoId.isEmpty())
    {
        return QString("https://www.youtube.com/watch?v=%1").arg(videoId);
    }

    if (!value.contains("://") && !value.contains('/') &&
        !value.contains('\\'))
    {
        return value;
    }

    if (!value.contains("://") &&
        (value.startsWith("www.", Qt::CaseInsensitive) ||
         value.startsWith("youtube.com/", Qt::CaseInsensitive) ||
         value.startsWith("m.youtube.com/", Qt::CaseInsensitive) ||
         value.startsWith("music.youtube.com/", Qt::CaseInsensitive)))
    {
        value.prepend("https://");
    }

    const QUrl url(value);
    if (!url.isValid())
    {
        return {};
    }

    const auto path = trimTrailingSlash(url.path());
    if (url.host().toLower().endsWith("youtube.com") && path.startsWith("/@"))
    {
        return path.section('/', 1, 1).trimmed();
    }
    if (url.host().toLower().endsWith("youtube.com") &&
        path.startsWith("/channel/"))
    {
        const auto channelId = path.section('/', 2, 2).trimmed();
        if (isLikelyYouTubeChannelId(channelId))
        {
            return channelId;
        }
    }

    if (url.host().toLower().endsWith("youtube.com"))
    {
        const auto firstSegment = path.section('/', 1, 1).trimmed();
        if (firstSegment == "c" || firstSegment == "user")
        {
            return path.section('/', 2, 2).trimmed();
        }

        if (!firstSegment.isEmpty() && firstSegment != "watch" &&
            firstSegment != "shorts" && firstSegment != "embed" &&
            firstSegment != "live" && firstSegment != "playlist" &&
            firstSegment != "results" && firstSegment != "feed")
        {
            return QUrl::fromPercentEncoding(firstSegment.toUtf8()).trimmed();
        }
    }

    return {};
}

QToolButton *makeHelpButton(const QString &tip, QWidget *parent)
{
    auto *btn = new QToolButton(parent);
    btn->setText(QStringLiteral("?"));
    btn->setToolTip(tip);
    btn->setAutoRaise(true);
    btn->setCursor(Qt::WhatsThisCursor);
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setFixedSize(16, 16);
    QObject::connect(btn, &QToolButton::clicked, btn, [btn]() {
        QToolTip::showText(
            btn->mapToGlobal(QPoint(btn->width() / 2, btn->height())),
            btn->toolTip(), btn);
    });
    return btn;
}

QWidget *makeRowWithHelp(QWidget *primary, const QString &tip)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->addWidget(primary);
    layout->addWidget(makeHelpButton(tip, row));
    layout->addStretch(1);
    return row;
}

}  // namespace

SelectChannelDialog::SelectChannelDialog(bool showSpecialPage, QWidget *parent)
    : BaseWindow(
          {
              BaseWindow::Flags::EnableCustomFrame,
              BaseWindow::Flags::Dialog,
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
    , selectedChannel_(Channel::getEmpty())
{
    this->setWindowTitle("Select a channel to join");

    auto &ui = this->ui_;
    auto *rootLayout = new QVBoxLayout(this->getLayoutContainer());
    rootLayout->setContentsMargins({});
    rootLayout->setSpacing(6);

    ui.notebook = new MicroNotebook(this->getLayoutContainer());
    rootLayout->addWidget(ui.notebook, 1);

    ui.mergedPage = new QWidget;
    auto *mergedLayout = new QVBoxLayout(ui.mergedPage);

    auto *intro = new QLabel(
        "By default, Twitch and Kick both use the tab name as the channel "
        "name. Override either platform only when the names differ, and add "
        "the streamer's YouTube @handle or any video link from the desired "
        "channel, this will resolve the channel ID automatically.");
    intro->setWordWrap(true);
    mergedLayout->addWidget(intro);

    auto *baseGroup = new QGroupBox;
    auto *baseLayout = new QFormLayout(baseGroup);
    ui.tabName = new QLineEdit();
    ui.tabName->setPlaceholderText("Streamer / tab name");
    baseLayout->addRow("Tab name", ui.tabName);
    mergedLayout->addWidget(baseGroup);

    auto *platformGroup = new QGroupBox;
    auto *platformLayout = new QFormLayout(platformGroup);

    ui.enableTwitch = new QCheckBox("Enable Twitch");
    ui.enableTwitch->setChecked(true);
    platformLayout->addRow(ui.enableTwitch);
    ui.twitchName = new QLineEdit();
    ui.twitchName->setPlaceholderText("Same as tab name");
    platformLayout->addRow("Twitch channel", ui.twitchName);

    ui.enableKick = new QCheckBox("Enable Kick");
    ui.enableKick->setChecked(true);
    platformLayout->addRow(ui.enableKick);
    ui.kickName = new QLineEdit();
    ui.kickName->setPlaceholderText("Same as tab name");
    platformLayout->addRow("Kick channel", ui.kickName);

    ui.enableYouTube = new QCheckBox("Enable YouTube");
    platformLayout->addRow(ui.enableYouTube);
    ui.youtubeUrl = new QLineEdit();
    ui.youtubeUrl->setPlaceholderText("@handle or any YouTube video link");
    platformLayout->addRow("YouTube", ui.youtubeUrl);

    ui.enableTikTok = new QCheckBox("Enable TikTok");
    platformLayout->addRow(ui.enableTikTok);
    ui.tiktokInput = new QLineEdit();
    ui.tiktokInput->setPlaceholderText("@username or /@user/live URL");
    platformLayout->addRow("TikTok", ui.tiktokInput);

    ui.indicatorMode = new QComboBox();
    ui.indicatorMode->addItem("Highlights");
    ui.indicatorMode->addItem("Logos");
    ui.indicatorMode->addItem("Both");
    ui.indicatorMode->setToolTip(
        "Choose whether merged messages use platform-colored rows, "
        "platform logo badges, or both.");
    platformLayout->addRow("Platform style", ui.indicatorMode);
    ui.enableActivity = new QCheckBox("Show activity tab");
    const QString activityTabTip =
        QStringLiteral("Open the linked activity tab for this merged tab and "
                       "keep it in sync with this channel.");
    ui.enableActivity->setToolTip(activityTabTip);
    platformLayout->addRow(makeRowWithHelp(ui.enableActivity, activityTabTip));
    ui.filterActivity = new QCheckBox("Hide activity from main chat");
    const QString filterActivityTip =
        QStringLiteral("Hide sub, hype chat, and cheer activity from the main "
                       "chat. Turning on the activity tab also enables this "
                       "automatically.");
    ui.filterActivity->setToolTip(filterActivityTip);
    platformLayout->addRow(
        makeRowWithHelp(ui.filterActivity, filterActivityTip));

    mergedLayout->addWidget(platformGroup);

    QObject::connect(ui.enableTwitch, &QCheckBox::toggled, this,
                     [this](bool) { this->syncMergedFieldState(); });
    QObject::connect(ui.enableKick, &QCheckBox::toggled, this,
                     [this](bool) { this->syncMergedFieldState(); });
    QObject::connect(ui.enableYouTube, &QCheckBox::toggled, this,
                     [this](bool) { this->syncMergedFieldState(); });
    QObject::connect(ui.enableTikTok, &QCheckBox::toggled, this,
                     [this](bool) { this->syncMergedFieldState(); });
    QObject::connect(ui.enableActivity, &QCheckBox::toggled, this,
                     [this](bool enabled) {
                         if (enabled && this->ui_.filterActivity != nullptr &&
                             !this->ui_.filterActivity->isChecked())
                         {
                             this->ui_.filterActivity->setChecked(true);
                         }
                     });

    ui.notebook->addPage(ui.mergedPage, "Merged");

    if (showSpecialPage)
    {
        ui.specialPage = new QWidget;
        auto *specialLayout = new QVBoxLayout(ui.specialPage);
        auto *specialIntro = new QLabel(
            "Use this page for Twitch-wide utility channels that are not tied "
            "to a specific streamer tab.");
        specialIntro->setWordWrap(true);
        specialLayout->addWidget(specialIntro);

        ui.whispers = new detail::AutoCheckedRadioButton("Whispers");
        ui.mentions = new detail::AutoCheckedRadioButton("Mentions");
        ui.watching = new detail::AutoCheckedRadioButton("Watching");
        ui.live = new detail::AutoCheckedRadioButton("Live");
        ui.automod = new detail::AutoCheckedRadioButton("AutoMod");

        specialLayout->addWidget(ui.whispers);
        specialLayout->addWidget(ui.mentions);
        specialLayout->addWidget(ui.watching);
        specialLayout->addWidget(ui.live);
        specialLayout->addWidget(ui.automod);

        ui.notebook->addPage(ui.specialPage, "Twitch Special");
    }
    else
    {
        ui.notebook->setShowHeader(false);
    }

    auto *buttonBox =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttonBox->setContentsMargins({10, 4, 10, 10});
    rootLayout->addWidget(buttonBox);

    QObject::connect(buttonBox, &QDialogButtonBox::accepted, this, [this] {
        this->ok();
    });
    QObject::connect(buttonBox, &QDialogButtonBox::rejected, this, [this] {
        this->close();
    });

    this->setMergedDefaults();
    this->syncMergedFieldState();
    this->addShortcuts();
    this->themeChangedEvent();
}

void SelectChannelDialog::ok()
{
    const auto built = this->ui_.notebook->isSelected(this->ui_.specialPage)
                           ? this->buildSpecialSelection()
                           : this->buildMergedSelection();
    if (!built)
    {
        return;
    }

    this->hasSelectedChannel_ = true;
    this->close();
}

void SelectChannelDialog::setMergedDefaults()
{
    this->ui_.notebook->select(this->ui_.mergedPage);
    this->ui_.tabName->clear();
    this->ui_.enableActivity->setChecked(true);
    this->ui_.filterActivity->setChecked(true);
    this->ui_.enableTwitch->setChecked(true);
    this->ui_.twitchName->clear();
    this->ui_.enableKick->setChecked(true);
    this->ui_.kickName->clear();
    this->ui_.enableYouTube->setChecked(false);
    this->ui_.youtubeUrl->clear();
    this->ui_.enableTikTok->setChecked(false);
    this->ui_.tiktokInput->clear();
    this->ui_.indicatorMode->setCurrentIndex(indicatorModeIndex(
        getSettings()->mergedPlatformIndicatorMode));
}

void SelectChannelDialog::loadMergedDefaultsFromChannel(
    const IndirectChannel &indirectChannel)
{
    const auto &channel = indirectChannel.get();
    if (!channel)
    {
        this->setMergedDefaults();
        return;
    }

    this->setMergedDefaults();
    this->ui_.notebook->select(this->ui_.mergedPage);

    if (auto *merged = dynamic_cast<MergedChannel *>(channel.get()))
    {
        const auto &config = merged->config();
        this->ui_.tabName->setText(config.tabName);
        this->ui_.enableTwitch->setChecked(config.twitchEnabled);
        this->ui_.twitchName->setText(config.twitchChannelName);
        this->ui_.enableKick->setChecked(config.kickEnabled);
        this->ui_.kickName->setText(config.kickChannelName);
        this->ui_.enableYouTube->setChecked(config.youtubeEnabled);
        this->ui_.youtubeUrl->setText(
            normalizeYouTubeSource(config.youtubeStreamUrl).isEmpty()
                ? config.youtubeStreamUrl
                : normalizeYouTubeSource(config.youtubeStreamUrl));
        this->ui_.enableTikTok->setChecked(config.tiktokEnabled);
        this->ui_.tiktokInput->setText(config.tiktokUsername);
    }
    else if (indirectChannel.getType() == Channel::Type::Twitch)
    {
        this->ui_.tabName->setText(channel->getName());
        this->ui_.enableTwitch->setChecked(true);
        this->ui_.enableKick->setChecked(false);
    }
    else if (indirectChannel.getType() == Channel::Type::Kick)
    {
        this->ui_.tabName->setText(channel->getName());
        this->ui_.enableTwitch->setChecked(false);
        this->ui_.enableKick->setChecked(true);
    }

    this->syncMergedFieldState();
}

void SelectChannelDialog::setSelectedChannel(
    std::optional<IndirectChannel> channel_)
{
    this->hasSelectedChannel_ = false;

    if (!channel_.has_value())
    {
        this->setMergedDefaults();
        return;
    }

    const auto &indirectChannel = channel_.value();
    assert(indirectChannel.get());

    this->selectedChannel_ = indirectChannel;

    switch (indirectChannel.getType())
    {
        case Channel::Type::TwitchWhispers:
            if (this->ui_.specialPage == nullptr)
            {
                this->setMergedDefaults();
                break;
            }
            this->ui_.notebook->select(this->ui_.specialPage);
            this->ui_.whispers->setChecked(true);
            break;
        case Channel::Type::TwitchMentions:
            if (this->ui_.specialPage == nullptr)
            {
                this->setMergedDefaults();
                break;
            }
            this->ui_.notebook->select(this->ui_.specialPage);
            this->ui_.mentions->setChecked(true);
            break;
        case Channel::Type::TwitchWatching:
            if (this->ui_.specialPage == nullptr)
            {
                this->setMergedDefaults();
                break;
            }
            this->ui_.notebook->select(this->ui_.specialPage);
            this->ui_.watching->setChecked(true);
            break;
        case Channel::Type::TwitchLive:
            if (this->ui_.specialPage == nullptr)
            {
                this->setMergedDefaults();
                break;
            }
            this->ui_.notebook->select(this->ui_.specialPage);
            this->ui_.live->setChecked(true);
            break;
        case Channel::Type::TwitchAutomod:
            if (this->ui_.specialPage == nullptr)
            {
                this->setMergedDefaults();
                break;
            }
            this->ui_.notebook->select(this->ui_.specialPage);
            this->ui_.automod->setChecked(true);
            break;
        default:
            this->loadMergedDefaultsFromChannel(indirectChannel);
            break;
    }
}

void SelectChannelDialog::setActivityPaneEnabled(bool enabled)
{
    if (this->ui_.enableActivity)
    {
        this->ui_.enableActivity->setChecked(enabled);
    }
}

void SelectChannelDialog::setPlatformIndicatorMode(PlatformIndicatorMode mode)
{
    if (this->ui_.indicatorMode)
    {
        this->ui_.indicatorMode->setCurrentIndex(indicatorModeIndex(mode));
    }
}

void SelectChannelDialog::setFilterActivity(bool enabled)
{
    if (this->ui_.filterActivity)
    {
        this->ui_.filterActivity->setChecked(enabled);
    }
}

IndirectChannel SelectChannelDialog::getSelectedChannel() const
{
    return this->selectedChannel_;
}

bool SelectChannelDialog::activityPaneEnabled() const
{
    return this->ui_.enableActivity != nullptr &&
           this->ui_.enableActivity->isChecked();
}

bool SelectChannelDialog::filterActivity() const
{
    return this->ui_.filterActivity != nullptr &&
           this->ui_.filterActivity->isChecked();
}

PlatformIndicatorMode SelectChannelDialog::platformIndicatorMode() const
{
    if (this->ui_.indicatorMode == nullptr)
    {
        return PlatformIndicatorMode::LineColor;
    }

    return indicatorModeFromIndex(this->ui_.indicatorMode->currentIndex());
}

bool SelectChannelDialog::hasSeletedChannel() const
{
    return this->hasSelectedChannel_;
}

void SelectChannelDialog::syncMergedFieldState()
{
    this->ui_.twitchName->setEnabled(this->ui_.enableTwitch->isChecked());
    this->ui_.kickName->setEnabled(this->ui_.enableKick->isChecked());
    this->ui_.youtubeUrl->setEnabled(this->ui_.enableYouTube->isChecked());
    this->ui_.tiktokInput->setEnabled(this->ui_.enableTikTok->isChecked());
}

bool SelectChannelDialog::buildMergedSelection()
{
    const auto youtubeChannel = normalizeYouTubeSource(this->ui_.youtubeUrl->text());
    const auto tiktokUsername =
        TikTokLiveChat::normalizeSource(this->ui_.tiktokInput->text());
    getSettings()->mergedPlatformIndicatorMode.setValue(
        qmagicenum::enumNameString(
            indicatorModeFromIndex(this->ui_.indicatorMode->currentIndex()))
            .toLower());
    MergedChannelConfig config{
        .tabName = this->ui_.tabName->text().trimmed(),
        .twitchEnabled = this->ui_.enableTwitch->isChecked(),
        .twitchChannelName = this->ui_.twitchName->text().trimmed(),
        .kickEnabled = this->ui_.enableKick->isChecked(),
        .kickChannelName = this->ui_.kickName->text().trimmed(),
        .youtubeEnabled = this->ui_.enableYouTube->isChecked(),
        .youtubeStreamUrl = youtubeChannel,
        .tiktokEnabled = this->ui_.enableTikTok->isChecked(),
        .tiktokUsername = tiktokUsername,
    };

    if (!config.twitchEnabled && !config.kickEnabled &&
        !config.youtubeEnabled && !config.tiktokEnabled)
    {
        QMessageBox::warning(this, "Select a platform",
                             "Enable at least one platform for this merged tab.");
        return false;
    }

    if (config.twitchEnabled && config.effectiveTwitchChannelName().isEmpty())
    {
        QMessageBox::warning(
            this, "Missing Twitch channel",
            "Enter a tab name or Twitch override before enabling Twitch.");
        return false;
    }

    if (config.kickEnabled && config.effectiveKickChannelName().isEmpty())
    {
        QMessageBox::warning(
            this, "Missing Kick channel",
            "Enter a tab name or Kick override before enabling Kick.");
        return false;
    }

    if (config.youtubeEnabled && config.youtubeStreamUrl.isEmpty())
    {
        QMessageBox::warning(
            this, "Missing YouTube source",
            "Enter the streamer's YouTube @handle or any video link from the "
            "desired channel before enabling YouTube.");
        return false;
    }

    if (config.tiktokEnabled && config.tiktokUsername.isEmpty())
    {
        QMessageBox::warning(
            this, "Missing TikTok username",
            "Enter the streamer's TikTok @username or their /@user/live URL "
            "before enabling TikTok.");
        return false;
    }

    this->selectedChannel_ = IndirectChannel(
        std::make_shared<MergedChannel>(std::move(config)));
    return true;
}

bool SelectChannelDialog::buildSpecialSelection()
{
    if (this->ui_.watching->isChecked())
    {
        this->selectedChannel_ = getApp()->getTwitch()->getWatchingChannel();
    }
    else if (this->ui_.mentions->isChecked())
    {
        this->selectedChannel_ = getApp()->getTwitch()->getMentionsChannel();
    }
    else if (this->ui_.whispers->isChecked())
    {
        this->selectedChannel_ = getApp()->getTwitch()->getWhispersChannel();
    }
    else if (this->ui_.live->isChecked())
    {
        this->selectedChannel_ = getApp()->getTwitch()->getLiveChannel();
    }
    else if (this->ui_.automod->isChecked())
    {
        this->selectedChannel_ = getApp()->getTwitch()->getAutomodChannel();
    }
    else
    {
        return false;
    }

    return true;
}

void SelectChannelDialog::closeEvent(QCloseEvent *event)
{
    BaseWindow::closeEvent(event);
    this->closed.invoke();
}

void SelectChannelDialog::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();
    this->setPalette(getTheme()->palette);
}

void SelectChannelDialog::scaleChangedEvent(float newScale)
{
    BaseWindow::scaleChangedEvent(newScale);

    auto uiFont =
        getApp()->getFonts()->getFont(FontStyle::UiMedium, this->scale());
    this->ui_.tabName->setFont(uiFont);
    this->ui_.twitchName->setFont(uiFont);
    this->ui_.kickName->setFont(uiFont);
    this->ui_.youtubeUrl->setFont(uiFont);
    this->ui_.tiktokInput->setFont(uiFont);
}

void SelectChannelDialog::addShortcuts()
{
    HotkeyController::HotkeyMap actions{
        {"accept",
         [this](const std::vector<QString> &) -> QString {
             this->ok();
             return "";
         }},
        {"reject",
         [this](const std::vector<QString> &) -> QString {
             this->close();
             return "";
         }},
        {"scrollPage", nullptr},
        {"search", nullptr},
        {"delete", nullptr},
        {"openTab", nullptr},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::PopupWindow, actions, this);
}

}  // namespace chatterino
