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
#include <QFontMetrics>
#include <QFormLayout>
#include <QGraphicsOpacityEffect>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QToolTip>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWidget>

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

constexpr qreal MIN_SLOWER_CHAT_MESSAGES_PER_SECOND = 0.25;
constexpr qreal MAX_SLOWER_CHAT_MESSAGES_PER_SECOND = 20.0;

class InstantTooltipButton final : public QToolButton
{
public:
    using QToolButton::QToolButton;

protected:
    bool event(QEvent *event) override
    {
        switch (event->type())
        {
            case QEvent::Enter:
            case QEvent::MouseMove:
                this->showInstantTooltip();
                break;
            case QEvent::ToolTip:
                return true;
            case QEvent::Leave:
            case QEvent::Hide:
            case QEvent::WindowDeactivate:
            case QEvent::MouseButtonPress:
                this->hideInstantTooltip();
                break;
            default:
                break;
        }

        return QToolButton::event(event);
    }

private:
    class HoverInfoPopup final : public QLabel
    {
    public:
        explicit HoverInfoPopup(QWidget *parent)
            : QLabel(parent, Qt::ToolTip | Qt::FramelessWindowHint |
                                 Qt::NoDropShadowWindowHint)
        {
            this->setAttribute(Qt::WA_ShowWithoutActivating);
            this->setAttribute(Qt::WA_TransparentForMouseEvents);
            this->setFocusPolicy(Qt::NoFocus);
        }

        void applyTheme(const QFont &font)
        {
            const auto *theme = getTheme();

            QColor tooltipBackground = theme->window.background;
            tooltipBackground = theme->isLightTheme()
                                    ? tooltipBackground.darker(105)
                                    : tooltipBackground.lighter(120);

            QColor tooltipBorder = theme->splits.header.border;
            if (!tooltipBorder.isValid() || tooltipBorder.alpha() == 0)
            {
                tooltipBorder = theme->window.text;
                tooltipBorder.setAlpha(90);
            }

            this->setFont(font);
            this->setStyleSheet(
                QStringLiteral("QLabel {"
                               " padding: 1px 3px;"
                               " background-color: %1;"
                               " border: 1px solid %2;"
                               " color: %3;"
                               "}")
                    .arg(tooltipBackground.name(QColor::HexArgb),
                         tooltipBorder.name(QColor::HexArgb),
                         theme->window.text.name(QColor::HexArgb)));
        }
    };

    void showInstantTooltip()
    {
        const QString text = this->toolTip().trimmed();
        if (text.isEmpty() || !this->underMouse())
        {
            this->hideInstantTooltip();
            return;
        }

        if (this->tooltipPopup_ == nullptr)
        {
            this->tooltipPopup_ = new HoverInfoPopup(this);
        }

        this->tooltipPopup_->applyTheme(QToolTip::font());
        this->tooltipPopup_->setText(text);
        this->tooltipPopup_->adjustSize();

        constexpr int tooltipVerticalGap = 3;
        const auto popupSize = this->tooltipPopup_->sizeHint();
        const QPoint tooltipTopLeft =
            this->mapToGlobal(QPoint(
                (this->width() - popupSize.width()) / 2,
                -popupSize.height() - tooltipVerticalGap));

        this->tooltipPopup_->move(tooltipTopLeft);
        this->tooltipPopup_->show();
        this->tooltipPopup_->raise();
    }

    void hideInstantTooltip()
    {
        if (this->tooltipPopup_ != nullptr)
        {
            this->tooltipPopup_->hide();
        }
    }

    HoverInfoPopup *tooltipPopup_{};
};

QString buildDialogTooltipStyleSheet()
{
    const auto *theme = getTheme();

    QColor tooltipBackground = theme->window.background;
    tooltipBackground = theme->isLightTheme()
                            ? tooltipBackground.darker(105)
                            : tooltipBackground.lighter(120);

    QColor tooltipBorder = theme->splits.header.border;
    if (!tooltipBorder.isValid() || tooltipBorder.alpha() == 0)
    {
        tooltipBorder = theme->window.text;
        tooltipBorder.setAlpha(90);
    }

    return QStringLiteral(
               "QToolTip {"
               " padding: 1px 3px;"
               " background-color: %1;"
               " border: 1px solid %2;"
               " color: %3;"
               " font-size: 11px;"
               "}")
        .arg(tooltipBackground.name(QColor::HexArgb),
             tooltipBorder.name(QColor::HexArgb),
             theme->window.text.name(QColor::HexArgb));
}

QToolButton *createInfoButton(QWidget *parent, const QString &tooltip)
{
    auto *button = new InstantTooltipButton(parent);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setText("i");
    button->setFixedSize(12, 12);
    button->setToolTip(tooltip);
    button->setStyleSheet(
        "QToolButton {"
        " color: palette(mid);"
        " border: 1px solid palette(mid);"
        " border-radius: 6px;"
        " background: transparent;"
        " padding: 0px;"
        " font-size: 8px;"
        " font-weight: 600;"
        "}");
    return button;
}

QWidget *createLabelWithInfo(const QString &labelText, const QString &tooltip,
                             QWidget *parent)
{
    auto *container = new QWidget(parent);
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *label = new QLabel(labelText, container);
    layout->addWidget(label);
    layout->addWidget(createInfoButton(container, tooltip), 0,
                      Qt::AlignVCenter);
    layout->addStretch(1);
    return container;
}

QWidget *createCheckboxRow(QCheckBox *checkbox, const QString &tooltip,
                           QWidget *parent)
{
    auto *container = new QWidget(parent);
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    layout->addWidget(checkbox);
    layout->addWidget(createInfoButton(container, tooltip), 0,
                      Qt::AlignVCenter);
    layout->addStretch(1);
    return container;
}

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
    const auto platformStyleTooltip =
        QStringLiteral("Show platform color, logo, or both.");
    appearanceLayout->addRow(
        createLabelWithInfo("Platform style", platformStyleTooltip, this),
        this->ui_.indicatorMode);

    if (!this->isActivityPane_)
    {
        this->ui_.filterActivity = new QCheckBox("Filter activity");
        const auto filterActivityTooltip = QStringLiteral(
            "Hide activity alerts from this chat.");
        appearanceLayout->addRow(createCheckboxRow(this->ui_.filterActivity,
                                                   filterActivityTooltip,
                                                   this));

        this->ui_.slowerChat = new QCheckBox("Slower chat");
        const auto slowerChatTooltip = QStringLiteral(
            "Queue messages and release them at a fixed rate.");
        appearanceLayout->addRow(createCheckboxRow(this->ui_.slowerChat,
                                                   slowerChatTooltip, this));

        this->ui_.slowerChatRate = new QDoubleSpinBox();
        this->ui_.slowerChatRate->setDecimals(0);
        this->ui_.slowerChatRate->setRange(1.0,
                                           MAX_SLOWER_CHAT_MESSAGES_PER_SECOND);
        this->ui_.slowerChatRate->setSingleStep(1.0);
        this->ui_.slowerChatRate->setButtonSymbols(
            QAbstractSpinBox::NoButtons);
        this->ui_.slowerChatRate->setFixedWidth(32);
        const auto slowerChatRateTooltip = QStringLiteral(
            "How many queued messages to show each second.");
        this->ui_.slowerChatRateLabel = createLabelWithInfo(
            "Messages per second", slowerChatRateTooltip, this);
        this->ui_.slowerChatRateField = this->ui_.slowerChatRate;
        appearanceLayout->addRow(this->ui_.slowerChatRateLabel,
                                 this->ui_.slowerChatRateField);

        this->ui_.messageAnimations = new QCheckBox("Message animations");
        const auto messageAnimationsTooltip =
            QStringLiteral("Smoothly animate messages.");
        this->ui_.messageAnimationsRow = createCheckboxRow(
            this->ui_.messageAnimations, messageAnimationsTooltip, this);
        appearanceLayout->addRow(this->ui_.messageAnimationsRow);

        QObject::connect(this->ui_.slowerChat, &QCheckBox::toggled, this,
                         [this] {
                             this->updateSlowerChatVisibility();
                         });
    }

    if (this->isActivityPane_)
    {
        this->ui_.activityScale = new QComboBox();
        const auto activityScaleTooltip = QStringLiteral(
            "Set message size for this Activity tab.");
        for (const auto &option : ACTIVITY_SCALE_OPTIONS)
        {
            this->ui_.activityScale->addItem(option.label, option.scale);
        }
        appearanceLayout->addRow(
            createLabelWithInfo("Chat line size", activityScaleTooltip, this),
            this->ui_.activityScale);

        if (this->showTikTokGiftMinimum_)
        {
            this->ui_.tiktokGiftMinimum = new QSpinBox();
            this->ui_.tiktokGiftMinimum->setRange(0, 1000000);
            this->ui_.tiktokGiftMinimum->setSingleStep(1);
            const auto tiktokGiftMinimumTooltip = QStringLiteral(
                "Only show TikTok gifts at or above this diamond count.");
            appearanceLayout->addRow(createLabelWithInfo(
                                         "TikTok min diamonds",
                                         tiktokGiftMinimumTooltip, this),
                                     this->ui_.tiktokGiftMinimum);
        }
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
    this->updateSlowerChatVisibility(false);
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
    if (this->ui_.slowerChat)
    {
        this->ui_.slowerChat->setChecked(enabled);
        this->updateSlowerChatVisibility(false);
    }
}

bool SplitSettingsDialog::slowerChatEnabled() const
{
    return this->ui_.slowerChat && this->ui_.slowerChat->isChecked();
}

void SplitSettingsDialog::setSlowerChatMessagesPerSecond(qreal value)
{
    if (this->ui_.slowerChatRate)
    {
        this->ui_.slowerChatRate->setValue(value);
    }
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
    if (this->ui_.messageAnimations)
    {
        this->ui_.messageAnimations->setChecked(enabled);
    }
}

bool SplitSettingsDialog::slowerChatMessageAnimations() const
{
    return this->ui_.messageAnimations &&
           this->ui_.messageAnimations->isChecked();
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
    this->setStyleSheet(buildDialogTooltipStyleSheet());
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
    if (this->ui_.tiktokGiftMinimum)
    {
        this->ui_.tiktokGiftMinimum->setFont(uiFont);
    }

    this->applySlowerChatRateVisibilityProgress(
        this->ui_.slowerChatRateVisibilityProgress);
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
