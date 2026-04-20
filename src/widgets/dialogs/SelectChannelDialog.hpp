// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "widgets/BaseWindow.hpp"

#include <pajlada/signals/signal.hpp>
#include <QRadioButton>

#include <cstdint>
#include <optional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFocusEvent;
class QLabel;
class QLineEdit;
class QRadioButton;
class QVariantAnimation;
class QWidget;

namespace chatterino {

class MicroNotebook;
enum class PlatformIndicatorMode : std::uint8_t;

namespace detail {

class AutoCheckedRadioButton : public QRadioButton
{
public:
    using QRadioButton::QRadioButton;

protected:
    void focusInEvent(QFocusEvent *event) override;
};

}  // namespace detail

class SelectChannelDialog final : public BaseWindow
{
public:
    SelectChannelDialog(bool showSpecialPage, QWidget *parent = nullptr);

    void setSelectedChannel(std::optional<IndirectChannel> channel_);
    void setActivityPaneEnabled(bool enabled);
    void setFilterActivity(bool enabled);
    void setPlatformIndicatorMode(PlatformIndicatorMode mode);
    void setSlowerChatEnabled(bool enabled);
    void setSlowerChatMessagesPerSecond(qreal value);
    void setSlowerChatMessageAnimations(bool enabled);
    IndirectChannel getSelectedChannel() const;
    bool activityPaneEnabled() const;
    bool filterActivity() const;
    PlatformIndicatorMode platformIndicatorMode() const;
    bool slowerChatEnabled() const;
    qreal slowerChatMessagesPerSecond() const;
    bool slowerChatMessageAnimations() const;
    bool hasSeletedChannel() const;

    pajlada::Signals::NoArgSignal closed;

protected:
    void closeEvent(QCloseEvent *event) override;
    void themeChangedEvent() override;
    void scaleChangedEvent(float newScale) override;

private:
    struct {
        MicroNotebook *notebook{};

        QWidget *mergedPage{};
        QLineEdit *tabName{};
        QCheckBox *enableActivity{};
        QCheckBox *enableTwitch{};
        QLineEdit *twitchName{};
        QCheckBox *enableKick{};
        QLineEdit *kickName{};
        QCheckBox *enableYouTube{};
        QLineEdit *youtubeUrl{};
        QCheckBox *enableTikTok{};
        QLineEdit *tiktokSource{};
        QComboBox *indicatorMode{};
        QCheckBox *filterActivity{};
        QCheckBox *slowerChat{};
        QDoubleSpinBox *slowerChatRate{};
        QCheckBox *messageAnimations{};
        QWidget *slowerChatRateLabel{};
        QWidget *slowerChatRateField{};
        QWidget *messageAnimationsRow{};
        QVariantAnimation *slowerChatRateAnimation{};
        qreal slowerChatRateVisibilityProgress = 1.0;

        QWidget *specialPage{};
        detail::AutoCheckedRadioButton *whispers{};
        detail::AutoCheckedRadioButton *mentions{};
        detail::AutoCheckedRadioButton *watching{};
        detail::AutoCheckedRadioButton *live{};
        detail::AutoCheckedRadioButton *automod{};
    } ui_{};

    IndirectChannel selectedChannel_;
    bool hasSelectedChannel_ = false;

    void ok();
    void setMergedDefaults();
    void loadMergedDefaultsFromChannel(const IndirectChannel &indirectChannel);
    void syncMergedFieldState();
    void updateSlowerChatVisibility(bool animate = true);
    void applySlowerChatRateVisibilityProgress(qreal progress);
    bool buildMergedSelection();
    bool buildSpecialSelection();
    void addShortcuts() override;
};

}  // namespace chatterino
