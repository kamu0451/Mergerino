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

namespace chatterino {

enum class PlatformIndicatorMode : std::uint8_t;

class SplitSettingsDialog final : public BaseWindow
{
public:
    explicit SplitSettingsDialog(bool isActivityPane, QWidget *parent = nullptr);

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
    } ui_{};

    const bool isActivityPane_;
    bool hasAcceptedChanges_{false};

    void ok();
    void addShortcuts() override;
};

}  // namespace chatterino
