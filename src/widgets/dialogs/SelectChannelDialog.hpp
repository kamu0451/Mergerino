// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "widgets/BaseWindow.hpp"

#include <pajlada/signals/signal.hpp>
#include <QRadioButton>

#include <optional>

class QCheckBox;
class QComboBox;
class QFocusEvent;
class QLabel;
class QLineEdit;
class QRadioButton;

namespace chatterino {

class MicroNotebook;

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
    SelectChannelDialog(QWidget *parent = nullptr);

    void setSelectedChannel(std::optional<IndirectChannel> channel_);
    void setActivityPaneEnabled(bool enabled);
    IndirectChannel getSelectedChannel() const;
    bool activityPaneEnabled() const;
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
        QComboBox *indicatorMode{};

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
    bool buildMergedSelection();
    bool buildSpecialSelection();
    void addShortcuts() override;
};

}  // namespace chatterino
