// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWindow.hpp"

#include <pajlada/signals/signal.hpp>

#include <cstdint>

class QCloseEvent;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QVariantAnimation;
class QWidget;

namespace chatterino {

enum class PlatformIndicatorMode : std::uint8_t;

class SplitSettingsDialog final : public BaseWindow
{
public:
    explicit SplitSettingsDialog(bool isActivityPane,
                                 bool showTwitchBitsMinimum,
                                 bool showKickKicksMinimum,
                                 bool showTikTokGiftMinimum,
                                 QWidget *parent = nullptr);

    void setPlatformIndicatorMode(PlatformIndicatorMode mode);
    PlatformIndicatorMode platformIndicatorMode() const;

    void setFilterActivity(bool enabled);
    bool filterActivity() const;

    void setActivityMessageScale(qreal scale);
    qreal activityMessageScale() const;
    void setSlowerChatEnabled(bool enabled);
    bool slowerChatEnabled() const;
    void setSlowerChatMessagesPerSecond(qreal value);
    qreal slowerChatMessagesPerSecond() const;
    void setSlowerChatMessageAnimations(bool enabled);
    bool slowerChatMessageAnimations() const;
    void setTwitchActivityMinimumBits(uint32_t value);
    uint32_t twitchActivityMinimumBits() const;
    void setKickActivityMinimumKicks(uint32_t value);
    uint32_t kickActivityMinimumKicks() const;
    void setTikTokActivityMinimumDiamonds(uint32_t value);
    uint32_t tiktokActivityMinimumDiamonds() const;

    bool hasAcceptedChanges() const;

    pajlada::Signals::NoArgSignal closed;

protected:
    void closeEvent(QCloseEvent *event) override;
    void themeChangedEvent() override;
    void scaleChangedEvent(float newScale) override;

private:
    struct {
        QComboBox *indicatorMode{};
        QCheckBox *filterActivity{};
        QComboBox *activityScale{};
        QCheckBox *slowerChat{};
        QDoubleSpinBox *slowerChatRate{};
        QCheckBox *messageAnimations{};
        QSpinBox *twitchBitsMinimum{};
        QSpinBox *kickKicksMinimum{};
        QSpinBox *tiktokGiftMinimum{};
        QWidget *slowerChatRateLabel{};
        QWidget *slowerChatRateField{};
        QWidget *messageAnimationsRow{};
        QVariantAnimation *slowerChatRateAnimation{};
        qreal slowerChatRateVisibilityProgress = 1.0;
    } ui_{};

    const bool isActivityPane_;
    const bool showTwitchBitsMinimum_;
    const bool showKickKicksMinimum_;
    const bool showTikTokGiftMinimum_;
    bool hasAcceptedChanges_{false};

    void updateSlowerChatVisibility(bool animate = true);
    void applySlowerChatRateVisibilityProgress(qreal progress);
    void ok();
    void addShortcuts() override;
};

}  // namespace chatterino
