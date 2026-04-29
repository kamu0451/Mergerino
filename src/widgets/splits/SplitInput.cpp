// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitInput.hpp"

#include "Application.hpp"
#include "common/enums/MessageOverflow.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/spellcheck/SpellChecker.hpp"
#include "messages/Link.hpp"
#include "messages/Message.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "util/Helpers.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/buttons/Button.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/EmotePopup.hpp"
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

#include <QCompleter>
#include <QImage>
#include <QPainter>
#include <QPen>
#include <QSignalBlocker>
#include <QSvgRenderer>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>
#include <functional>
#include <ranges>

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
        case MessagePlatform::AnyOrTwitch:
        default:
            return u"Twitch"_s;
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

int platformButtonWidthForCount(int platformCount, float scale)
{
    platformCount = std::max(platformCount, 1);
    if (platformCount == 1)
    {
        return int(22 * scale);
    }

    return int((16 * platformCount + 12 * (platformCount - 1) + 2) * scale);
}

}  // namespace

class PlatformSwitchButton : public Button
{
public:
    explicit PlatformSwitchButton(BaseWidget *parent = nullptr)
        : Button(parent)
        , twitchRenderer_(u":/platforms/twitch.svg"_s, this)
        , kickRenderer_(u":/platforms/kick.svg"_s, this)
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
        return platform == MessagePlatform::Kick ? this->kickRenderer_
                                                 : this->twitchRenderer_;
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
{
    this->installEventFilter(this);
    this->initLayout();

    auto *completer =
        new QCompleter(this->split_->getChannel()->completionModel);
    this->ui_.textEdit->setCompleter(completer);

    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
    auto *spellChecker = getApp()->getSpellChecker();
    this->inputHighlighter = new InputHighlighter(*spellChecker, this);
    this->inputHighlighter->setChannel(this->split_->getChannel());

    this->signalHolder_.managedConnect(this->split_->channelChanged, [this] {
        auto channel = this->split_->getChannel();
        auto *completer = new QCompleter(channel->completionModel);
        this->ui_.textEdit->setCompleter(completer);
        this->inputHighlighter->setChannel(this->split_->getChannel());
        this->updatePlatformSelector();
    });

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
        buttonHbox->addWidget(this->ui_.platformButton);

        this->ui_.emoteButton = new SvgButton(
            {
                .dark = ":/buttons/emote.svg",
                .light = ":/buttons/emoteDark.svg",
            },
            nullptr, QSize{4, 1});
        this->ui_.emoteButton->setContentSize(QSize{16, 16});
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
    QObject::connect(this->ui_.emoteButton, &Button::leftClicked, [this] {
        this->openEmotePopup();
    });

    QObject::connect(this->ui_.platformButton, &Button::leftClicked, [this] {
        this->cycleSendPlatform();
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

void SplitInput::updateEmoteButton()
{
    auto scale = this->scale();

    this->ui_.emoteButton->setFixedHeight(int(26 * scale));
    // Make button slightly wider so it's easier to click
    this->ui_.emoteButton->setFixedWidth(int(24 * scale));
}

void SplitInput::updatePlatformButtonLayout(int platformCount)
{
    auto scale = this->scale();

    const auto height = int(18 * scale);
    const auto width = platformButtonWidthForCount(platformCount, scale);
    const auto minimumHeight = height * 2 + int(4 * scale);

    if (this->ui_.inputWrapper)
    {
        this->ui_.inputWrapper->setMinimumHeight(minimumHeight);
    }

    if (this->ui_.textEdit)
    {
        this->ui_.textEdit->setVerticalPadding(int(5 * scale), 0);
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
        case MessagePlatform::AnyOrTwitch:
            return MessagePlatform::AnyOrTwitch;
        default:
            return std::nullopt;
    }
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

    if (this->selectedSendAllPlatforms_ && platforms.size() > 1)
    {
        return platforms;
    }

    if (std::ranges::find(platforms, this->selectedSendPlatform_) ==
        platforms.end())
    {
        return {platforms.front()};
    }

    return {this->selectedSendPlatform_};
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
        case MessagePlatform::AnyOrTwitch:
        default:
            return merged->twitchChannel();
    }
}

bool SplitInput::canSendToPlatform(MessagePlatform platform) const
{
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
        this->ui_.platformButton->hide();
        return;
    }

    if (this->selectedSendAllPlatforms_ && platforms.size() < 2)
    {
        this->selectedSendAllPlatforms_ = false;
    }

    if (std::ranges::find(platforms, this->selectedSendPlatform_) ==
        platforms.end())
    {
        this->selectedSendPlatform_ = platforms.front();
    }

    const auto selectedPlatforms = this->selectedSendPlatforms();
    if (selectedPlatforms.empty())
    {
        this->ui_.platformButton->hide();
        return;
    }

    const bool replyLocked = this->replySendPlatform().has_value();
    const auto targetCount =
        replyLocked ? selectedPlatforms.size()
                    : platforms.size() + (platforms.size() > 1 ? 1 : 0);

    this->updatePlatformButtonLayout(static_cast<int>(selectedPlatforms.size()));
    this->ui_.platformButton->show();
    this->ui_.platformButton->setEnabled(!replyLocked && targetCount > 1);
    this->ui_.platformButton->setPlatforms(selectedPlatforms,
                                           animate && targetCount > 1);

    auto tooltip =
        QString(replyLocked ? u"Replying on %1"_s : u"Sending to %1"_s)
            .arg(platformDisplayName(selectedPlatforms));
    if (!replyLocked && targetCount > 1)
    {
        QString nextTarget;
        if (this->selectedSendAllPlatforms_)
        {
            nextTarget = platformDisplayName(platforms.front());
        }
        else
        {
            auto it = std::ranges::find(platforms, this->selectedSendPlatform_);
            if (it != platforms.end() && ++it != platforms.end())
            {
                nextTarget = platformDisplayName(*it);
            }
            else
            {
                nextTarget = platformDisplayName(platforms);
            }
        }
        tooltip += QString(u". Click to switch to %1"_s).arg(nextTarget);
    }
    this->ui_.platformButton->setToolTip(tooltip);
    this->updateEmotePopupChannel();
}

std::optional<MessagePlatform> SplitInput::selectedSendPlatform() const
{
    if (auto replyPlatform = this->replySendPlatform())
    {
        return *replyPlatform;
    }

    if (this->selectedSendAllPlatforms_)
    {
        return std::nullopt;
    }

    const auto platforms = this->availableSendPlatforms();
    if (std::ranges::find(platforms, this->selectedSendPlatform_) ==
        platforms.end())
    {
        if (platforms.empty())
        {
            return std::nullopt;
        }
        return platforms.front();
    }
    return this->selectedSendPlatform_;
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
            case MessagePlatform::AnyOrTwitch:
            default: {
                auto user = getApp()->getAccounts()->twitch.getCurrent();
                if (user && !user->isAnon())
                {
                    accountNames.append(user->getUserName());
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
    if (!this->canSendToPlatform(platform))
    {
        return;
    }

    if (!this->selectedSendAllPlatforms_ &&
        this->selectedSendPlatform_ == platform)
    {
        this->cycleSendPlatform();
        return;
    }

    this->selectedSendAllPlatforms_ = false;
    this->selectedSendPlatform_ = platform;
    this->updatePlatformSelector(true);
    this->sendPlatformChanged.invoke();
}

void SplitInput::selectAllSendPlatforms()
{
    if (this->availableSendPlatforms().size() < 2)
    {
        return;
    }

    if (this->selectedSendAllPlatforms_)
    {
        this->cycleSendPlatform();
        return;
    }

    this->selectedSendAllPlatforms_ = true;
    this->updatePlatformSelector(true);
    this->sendPlatformChanged.invoke();
}

void SplitInput::cycleSendPlatform()
{
    const auto platforms = this->availableSendPlatforms();
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

    this->emotePopup_->loadChannel(channel, this->selectedSendPlatforms());
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
                    u"Log in to Twitch or Kick to send merged chat."_s);
            }
            return "";
        }

        for (const auto platform : sendPlatforms)
        {
            auto sendChannel = this->channelForSendPlatform(platform);
            if (!sendChannel || !this->canSendToPlatform(platform))
            {
                c->addSystemMessage(
                    QString(u"Log in to %1 to send merged chat."_s)
                        .arg(platformDisplayName(platform)));
                return "";
            }
            sendTargets.push_back({sendChannel, platform});
        }
    }
    else
    {
        sendTargets.push_back({c, std::nullopt});
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
    auto *tc = dynamic_cast<TwitchChannel *>(channel);
    bool showEmoteCompletion = getSettings()->emoteCompletionWithColon;
    bool showUsernameCompletion =
        tc != nullptr && getSettings()->showUsernameCompletionMenu;
    if (!showEmoteCompletion && !showUsernameCompletion)
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

    popup->updateCompletion(text, kind, this->split_->getChannel());

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

    QRect baseRect = this->rect();
    baseRect.setWidth(baseRect.width() - 1);

    auto *inputWrap = this->ui_.inputWrapper;
    auto inputBoxRect = inputWrap->geometry();
    inputBoxRect.setSize(inputBoxRect.size() - QSize{1, 1});

    painter.setBrush({this->theme->splits.input.background});
    painter.setPen(borderColor);
    painter.drawRect(inputBoxRect);

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
