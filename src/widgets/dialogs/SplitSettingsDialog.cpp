// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/SplitSettingsDialog.hpp"

#include "Application.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGraphicsOpacityEffect>
#include <QGroupBox>
#include <QSpinBox>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>

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

struct ActivityScaleOption {
    const char *label;
    qreal scale;
};

constexpr qreal MIN_SLOWER_CHAT_MESSAGES_PER_SECOND = 0.25;
constexpr qreal MAX_SLOWER_CHAT_MESSAGES_PER_SECOND = 20.0;

constexpr std::array<ActivityScaleOption, 8> ACTIVITY_SCALE_OPTIONS{{
    {"75%", 0.75},
    {"80%", 0.80},
    {"85%", 0.85},
    {"90%", 0.90},
    {"95%", 0.95},
    {"100%", 1.00},
    {"105%", 1.05},
    {"110%", 1.10},
}};

void applyAnimatedRowProgress(QWidget *widget, qreal progress)
{
    if (widget == nullptr)
    {
        return;
    }

    progress = std::clamp(progress, 0.0, 1.0);
    auto *effect =
        qobject_cast<QGraphicsOpacityEffect *>(widget->graphicsEffect());

    if (progress >= 1.0)
    {
        if (effect != nullptr)
        {
            widget->setGraphicsEffect(nullptr);
        }
        widget->setMaximumHeight(QWIDGETSIZE_MAX);
        widget->show();
        return;
    }

    if (effect == nullptr)
    {
        effect = new QGraphicsOpacityEffect(widget);
        widget->setGraphicsEffect(effect);
    }

    effect->setOpacity(progress);
    widget->setMaximumHeight(QWIDGETSIZE_MAX);

    if (progress <= 0.0)
    {
        widget->hide();
        return;
    }

    widget->show();
}

}  // namespace

SplitSettingsDialog::SplitSettingsDialog(bool isActivityPane,
                                         bool showTwitchBitsMinimum,
                                         bool showKickKicksMinimum,
                                         bool showTikTokGiftMinimum,
                                         QWidget *parent)
    : BaseWindow(
          {
              BaseWindow::Flags::EnableCustomFrame,
              BaseWindow::Flags::Dialog,
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
    , isActivityPane_(isActivityPane)
    , showTwitchBitsMinimum_(showTwitchBitsMinimum)
    , showKickKicksMinimum_(showKickKicksMinimum)
    , showTikTokGiftMinimum_(showTikTokGiftMinimum)
{
    this->setWindowTitle(isActivityPane ? "Activity settings"
                                        : "Split settings");

    auto *rootLayout = new QVBoxLayout(this->getLayoutContainer());
    rootLayout->setContentsMargins(10, 10, 10, 10);
    rootLayout->setSpacing(10);

    auto *appearanceGroup = new QGroupBox("Appearance");
    auto *appearanceLayout = new QFormLayout(appearanceGroup);

    this->ui_.indicatorMode = new QComboBox();
    this->ui_.indicatorMode->addItem("Highlights");
    this->ui_.indicatorMode->addItem("Logos");
    this->ui_.indicatorMode->addItem("Both");
    this->ui_.indicatorMode->setToolTip(
        "Choose whether this split uses platform-colored rows, platform "
        "logo badges, or both.");
    appearanceLayout->addRow("Platform style", this->ui_.indicatorMode);

    if (!this->isActivityPane_)
    {
        this->ui_.filterActivity = new QCheckBox("Filter activity");
        this->ui_.filterActivity->setToolTip(
            "Hide sub, hype chat, and cheer activity from this main chat. "
            "When a linked Activity tab is enabled, this starts turned on by "
            "default.");
        appearanceLayout->addRow(this->ui_.filterActivity);
    }

    if (this->isActivityPane_)
    {
        this->ui_.activityScale = new QComboBox();
        this->ui_.activityScale->setToolTip(
            "Adjust the message size used only in this Activity tab.");
        for (const auto &option : ACTIVITY_SCALE_OPTIONS)
        {
            this->ui_.activityScale->addItem(option.label, option.scale);
        }
        appearanceLayout->addRow("Chat line size", this->ui_.activityScale);

        if (this->showTwitchBitsMinimum_)
        {
            this->ui_.twitchBitsMinimum = new QSpinBox();
            this->ui_.twitchBitsMinimum->setRange(0, 1000000);
            this->ui_.twitchBitsMinimum->setSingleStep(1);
            this->ui_.twitchBitsMinimum->setToolTip(
                "Only show Twitch bit alerts at or above this bit count.");
            appearanceLayout->addRow("Twitch min bits",
                                     this->ui_.twitchBitsMinimum);
        }

        if (this->showKickKicksMinimum_)
        {
            this->ui_.kickKicksMinimum = new QSpinBox();
            this->ui_.kickKicksMinimum->setRange(0, 1000000);
            this->ui_.kickKicksMinimum->setSingleStep(1);
            this->ui_.kickKicksMinimum->setToolTip(
                "Only show Kick Kicks gifts at or above this Kicks amount.");
            appearanceLayout->addRow("Kick min Kicks",
                                     this->ui_.kickKicksMinimum);
        }

        if (this->showTikTokGiftMinimum_)
        {
            this->ui_.tiktokGiftMinimum = new QSpinBox();
            this->ui_.tiktokGiftMinimum->setRange(0, 1000000);
            this->ui_.tiktokGiftMinimum->setSingleStep(1);
            this->ui_.tiktokGiftMinimum->setToolTip(
                "Only show TikTok gifts at or above this diamond count.");
            appearanceLayout->addRow("TikTok min diamonds",
                                     this->ui_.tiktokGiftMinimum);
        }
    }

    rootLayout->addWidget(appearanceGroup);

    if (!this->isActivityPane_)
    {
        auto *paceGroup = new QGroupBox("Pace");
        auto *paceLayout = new QFormLayout(paceGroup);

        this->ui_.slowerChat = new QCheckBox("Slow down incoming chat");
        this->ui_.slowerChat->setToolTip(
            "Buffer incoming messages and release them at a steady rate. "
            "Moderator messages bypass the queue; VIP messages jump to the "
            "front.");
        paceLayout->addRow(this->ui_.slowerChat);

        this->ui_.slowerChatRate = new QDoubleSpinBox();
        this->ui_.slowerChatRate->setRange(
            MIN_SLOWER_CHAT_MESSAGES_PER_SECOND,
            MAX_SLOWER_CHAT_MESSAGES_PER_SECOND);
        this->ui_.slowerChatRate->setSingleStep(0.5);
        this->ui_.slowerChatRate->setDecimals(2);
        this->ui_.slowerChatRate->setSuffix(" msg/s");
        this->ui_.slowerChatRate->setToolTip(
            "How many messages per second are released from the queue.");
        paceLayout->addRow("Release rate", this->ui_.slowerChatRate);

        this->ui_.messageAnimations = new QCheckBox(
            "Animate arriving and shifting messages");
        this->ui_.messageAnimations->setToolTip(
            "Fade and slide newly released messages into view, and animate "
            "layout shifts when messages arrive.");
        paceLayout->addRow(this->ui_.messageAnimations);

        QObject::connect(this->ui_.slowerChat, &QCheckBox::toggled,
                         this->ui_.slowerChatRate,
                         &QDoubleSpinBox::setEnabled);
        QObject::connect(this->ui_.slowerChat, &QCheckBox::toggled,
                         this->ui_.messageAnimations,
                         &QCheckBox::setEnabled);

        rootLayout->addWidget(paceGroup);
    }

    rootLayout->addStretch(1);

    auto *buttonBox =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttonBox->setContentsMargins({});
    rootLayout->addWidget(buttonBox);

    QObject::connect(buttonBox, &QDialogButtonBox::accepted, this, [this] {
        this->ok();
    });
    QObject::connect(buttonBox, &QDialogButtonBox::rejected, this, [this] {
        this->close();
    });

    this->addShortcuts();
    this->themeChangedEvent();
}

void SplitSettingsDialog::setPlatformIndicatorMode(PlatformIndicatorMode mode)
{
    if (this->ui_.indicatorMode)
    {
        this->ui_.indicatorMode->setCurrentIndex(indicatorModeIndex(mode));
    }
}

PlatformIndicatorMode SplitSettingsDialog::platformIndicatorMode() const
{
    if (this->ui_.indicatorMode == nullptr)
    {
        return PlatformIndicatorMode::LineColor;
    }

    return indicatorModeFromIndex(this->ui_.indicatorMode->currentIndex());
}

void SplitSettingsDialog::setFilterActivity(bool enabled)
{
    if (this->ui_.filterActivity)
    {
        this->ui_.filterActivity->setChecked(enabled);
    }
}

bool SplitSettingsDialog::filterActivity() const
{
    return this->ui_.filterActivity && this->ui_.filterActivity->isChecked();
}

void SplitSettingsDialog::setActivityMessageScale(qreal scale)
{
    if (this->ui_.activityScale == nullptr)
    {
        return;
    }

    for (int i = 0; i < this->ui_.activityScale->count(); i++)
    {
        if (std::abs(this->ui_.activityScale->itemData(i).toDouble() - scale) <
            0.0001)
        {
            this->ui_.activityScale->setCurrentIndex(i);
            return;
        }
    }
}

qreal SplitSettingsDialog::activityMessageScale() const
{
    if (this->ui_.activityScale == nullptr)
    {
        return 1.0;
    }

    return this->ui_.activityScale->currentData().toDouble();
}

void SplitSettingsDialog::setSlowerChatEnabled(bool enabled)
{
    if (this->ui_.slowerChat == nullptr)
    {
        return;
    }
    this->ui_.slowerChat->setChecked(enabled);
    if (this->ui_.slowerChatRate)
    {
        this->ui_.slowerChatRate->setEnabled(enabled);
    }
    if (this->ui_.messageAnimations)
    {
        this->ui_.messageAnimations->setEnabled(enabled);
    }
}

bool SplitSettingsDialog::slowerChatEnabled() const
{
    return this->ui_.slowerChat && this->ui_.slowerChat->isChecked();
}

void SplitSettingsDialog::setSlowerChatMessagesPerSecond(qreal value)
{
    if (this->ui_.slowerChatRate == nullptr)
    {
        return;
    }
    const auto clamped = std::clamp(value, MIN_SLOWER_CHAT_MESSAGES_PER_SECOND,
                                    MAX_SLOWER_CHAT_MESSAGES_PER_SECOND);
    this->ui_.slowerChatRate->setValue(clamped);
}

qreal SplitSettingsDialog::slowerChatMessagesPerSecond() const
{
    if (this->ui_.slowerChatRate == nullptr)
    {
        return 5.0;
    }
    return this->ui_.slowerChatRate->value();
}

void SplitSettingsDialog::setSlowerChatMessageAnimations(bool enabled)
{
    if (this->ui_.messageAnimations == nullptr)
    {
        return;
    }
    this->ui_.messageAnimations->setChecked(enabled);
}

bool SplitSettingsDialog::slowerChatMessageAnimations() const
{
    return this->ui_.messageAnimations &&
           this->ui_.messageAnimations->isChecked();
}

void SplitSettingsDialog::setTwitchActivityMinimumBits(uint32_t value)
{
    if (this->ui_.twitchBitsMinimum)
    {
        this->ui_.twitchBitsMinimum->setValue(static_cast<int>(value));
    }
}

uint32_t SplitSettingsDialog::twitchActivityMinimumBits() const
{
    if (this->ui_.twitchBitsMinimum == nullptr)
    {
        return 100;
    }

    return static_cast<uint32_t>(this->ui_.twitchBitsMinimum->value());
}

void SplitSettingsDialog::setKickActivityMinimumKicks(uint32_t value)
{
    if (this->ui_.kickKicksMinimum)
    {
        this->ui_.kickKicksMinimum->setValue(static_cast<int>(value));
    }
}

uint32_t SplitSettingsDialog::kickActivityMinimumKicks() const
{
    if (this->ui_.kickKicksMinimum == nullptr)
    {
        return 100;
    }

    return static_cast<uint32_t>(this->ui_.kickKicksMinimum->value());
}

void SplitSettingsDialog::setTikTokActivityMinimumDiamonds(uint32_t value)
{
    if (this->ui_.tiktokGiftMinimum)
    {
        this->ui_.tiktokGiftMinimum->setValue(static_cast<int>(value));
    }
}

uint32_t SplitSettingsDialog::tiktokActivityMinimumDiamonds() const
{
    if (this->ui_.tiktokGiftMinimum == nullptr)
    {
        return 0;
    }

    return static_cast<uint32_t>(this->ui_.tiktokGiftMinimum->value());
}

bool SplitSettingsDialog::hasAcceptedChanges() const
{
    return this->hasAcceptedChanges_;
}

void SplitSettingsDialog::closeEvent(QCloseEvent *event)
{
    BaseWindow::closeEvent(event);
    this->closed.invoke();
}

void SplitSettingsDialog::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();
    this->setPalette(getTheme()->palette);
}

void SplitSettingsDialog::scaleChangedEvent(float newScale)
{
    BaseWindow::scaleChangedEvent(newScale);

    auto uiFont =
        getApp()->getFonts()->getFont(FontStyle::UiMedium, this->scale());
    if (this->ui_.indicatorMode)
    {
        this->ui_.indicatorMode->setFont(uiFont);
    }
    if (this->ui_.filterActivity)
    {
        this->ui_.filterActivity->setFont(uiFont);
    }
    if (this->ui_.activityScale)
    {
        this->ui_.activityScale->setFont(uiFont);
    }
    if (this->ui_.slowerChat)
    {
        this->ui_.slowerChat->setFont(uiFont);
    }
    if (this->ui_.slowerChatRate)
    {
        this->ui_.slowerChatRate->setFont(uiFont);
    }
    if (this->ui_.messageAnimations)
    {
        this->ui_.messageAnimations->setFont(uiFont);
    }
    if (this->ui_.twitchBitsMinimum)
    {
        this->ui_.twitchBitsMinimum->setFont(uiFont);
    }
    if (this->ui_.kickKicksMinimum)
    {
        this->ui_.kickKicksMinimum->setFont(uiFont);
    }
    if (this->ui_.tiktokGiftMinimum)
    {
        this->ui_.tiktokGiftMinimum->setFont(uiFont);
    }
}

void SplitSettingsDialog::applySlowerChatRateVisibilityProgress(qreal progress)
{
    this->ui_.slowerChatRateVisibilityProgress = progress;
    applyAnimatedRowProgress(this->ui_.slowerChatRateLabel, progress);
    applyAnimatedRowProgress(this->ui_.slowerChatRateField, progress);

    if (auto *layout = this->getLayoutContainer()->layout())
    {
        layout->activate();
    }

    if (this->isVisible())
    {
        this->adjustSize();
    }
}

void SplitSettingsDialog::updateSlowerChatVisibility(bool animate)
{
    const bool visible =
        this->ui_.slowerChat && this->ui_.slowerChat->isChecked();

    if (this->ui_.slowerChatRateLabel == nullptr ||
        this->ui_.slowerChatRateField == nullptr)
    {
        return;
    }

    const qreal targetProgress = visible ? 1.0 : 0.0;
    if (this->ui_.slowerChatRateAnimation == nullptr)
    {
        this->ui_.slowerChatRateAnimation = new QVariantAnimation(this);
        this->ui_.slowerChatRateAnimation->setDuration(160);
        this->ui_.slowerChatRateAnimation->setEasingCurve(
            QEasingCurve::InOutCubic);
        QObject::connect(this->ui_.slowerChatRateAnimation,
                         &QVariantAnimation::valueChanged, this,
                         [this](const QVariant &value) {
                             this->applySlowerChatRateVisibilityProgress(
                                 value.toReal());
                         });
    }

    if (!animate || !this->isVisible())
    {
        this->ui_.slowerChatRateAnimation->stop();
        this->applySlowerChatRateVisibilityProgress(targetProgress);
        return;
    }

    if (qFuzzyCompare(this->ui_.slowerChatRateVisibilityProgress, targetProgress))
    {
        this->applySlowerChatRateVisibilityProgress(targetProgress);
        return;
    }

    this->ui_.slowerChatRateAnimation->stop();
    this->ui_.slowerChatRateAnimation->setStartValue(
        this->ui_.slowerChatRateVisibilityProgress);
    this->ui_.slowerChatRateAnimation->setEndValue(targetProgress);
    this->ui_.slowerChatRateAnimation->start();
}

void SplitSettingsDialog::ok()
{
    this->hasAcceptedChanges_ = true;
    this->close();
}

void SplitSettingsDialog::addShortcuts()
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
