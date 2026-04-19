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
#include <QFormLayout>
#include <QGroupBox>
#include <QVBoxLayout>

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

}  // namespace

SplitSettingsDialog::SplitSettingsDialog(bool isActivityPane, QWidget *parent)
    : BaseWindow(
          {
              BaseWindow::Flags::EnableCustomFrame,
              BaseWindow::Flags::Dialog,
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
    , isActivityPane_(isActivityPane)
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
    }

    rootLayout->addWidget(appearanceGroup);
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
