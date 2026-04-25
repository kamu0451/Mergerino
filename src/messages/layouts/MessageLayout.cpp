// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/layouts/MessageLayout.hpp"

#include "Application.hpp"
#include "messages/layouts/MessageLayoutContainer.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "messages/Selection.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/WindowManager.hpp"
#include "util/DebugCount.hpp"

#include <QApplication>
#include <QDebug>
#include <QLinearGradient>
#include <QPainter>
#include <QtGlobal>
#include <QThread>

#include <algorithm>

namespace chatterino {

namespace {

QColor blendColors(const QColor &base, const QColor &apply)
{
    const qreal &alpha = apply.alphaF();
    QColor result;
    result.setRgbF(base.redF() * (1 - alpha) + apply.redF() * alpha,
                   base.greenF() * (1 - alpha) + apply.greenF() * alpha,
                   base.blueF() * (1 - alpha) + apply.blueF() * alpha);
    return result;
}

QColor defaultPlatformAccent(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QColor(83, 252, 24);
        case MessagePlatform::YouTube:
            return QColor(255, 48, 64);
        case MessagePlatform::TikTok:
            return QColor(254, 44, 85);
        case MessagePlatform::AnyOrTwitch:
        default:
            return QColor(145, 70, 255);
    }
}

QColor fallbackPlatformHighlightColor(const Message &message,
                                      const QColor &highlightColor)
{
    auto color =
        message.platformAccentColor.value_or(defaultPlatformAccent(message.platform));
    int alpha = 72;
    if (highlightColor.isValid())
    {
        alpha = highlightColor.alpha();
    }
    else if (message.platformAccentColor &&
             message.platformAccentColor->alpha() > 0)
    {
        alpha = std::max(message.platformAccentColor->alpha(), alpha);
    }

    color.setAlpha(alpha);
    return color;
}

QColor activityPlatformHighlightColor(const Message &message)
{
    const auto accent = defaultPlatformAccent(message.platform);
    const auto hsv = accent.toHsv();
    const int hue = hsv.hsvHue() >= 0 ? hsv.hsvHue() : 0;
    const int saturation =
        std::clamp(std::max(hsv.hsvSaturation(), 150) - 10, 0, 180);
    const int value = std::clamp(std::min(220, std::max(hsv.value(), 198)), 0,
                                 220);
    constexpr int alpha = 102;

    QColor popped;
    popped.setHsv(hue, saturation, value, alpha);
    return popped;
}

QColor automaticEventHighlightColor(const Message &message,
                                    const QColor &highlightColor,
                                    const MessagePaintContext &ctx)
{
    if (ctx.forceFlatEventHighlights)
    {
        if (!mergedPlatformIndicatorShowsLineColor(ctx.platformIndicatorMode))
        {
            return {};
        }

        return activityPlatformHighlightColor(message);
    }

    const auto style = getSettings()->platformEventHighlightStyle.getEnum();
    if (style == PlatformEventHighlightStyle::None)
    {
        return {};
    }

    if (style == PlatformEventHighlightStyle::CustomColor)
    {
        QColor custom(getSettings()->platformEventHighlightCustomColor);
        if (custom.isValid())
        {
            return custom;
        }
    }

    if ((message.flags.has(MessageFlag::FirstMessage) ||
         message.flags.has(MessageFlag::FirstMessageSession)) &&
        highlightColor.isValid())
    {
        return highlightColor;
    }

    return fallbackPlatformHighlightColor(message, highlightColor);
}

bool automaticEventHighlightUsesGradient(const MessagePaintContext &ctx)
{
    if (ctx.forceFlatEventHighlights)
    {
        return false;
    }

    return getSettings()->platformEventHighlightStyle.getEnum() ==
           PlatformEventHighlightStyle::Gradient;
}

bool isPlatformAlertMessage(const Message &message)
{
    return message.flags.has(MessageFlag::ModerationAction) &&
           !message.flags.has(MessageFlag::AutoMod) &&
           !message.flags.has(MessageFlag::LowTrustUsers);
}

bool automaticEventIncludesUserMessage(const Message &message)
{
    for (const auto &element : message.elements)
    {
        if (dynamic_cast<const MentionElement *>(element.get()) != nullptr)
        {
            continue;
        }

        if (const auto *text = dynamic_cast<const TextElement *>(element.get()))
        {
            if (text->color().type() != MessageColor::System)
            {
                return true;
            }
        }
        else if (const auto *singleLine =
                     dynamic_cast<const SingleLineTextElement *>(
                         element.get()))
        {
            if (singleLine->color().type() != MessageColor::System)
            {
                return true;
            }
        }
    }

    return false;
}

bool usesAutomaticEventOverlay(const Message &message,
                               const MessagePreferences &preferences,
                               bool includeActivityCheerOverlay)
{
    if (message.flags.has(MessageFlag::ElevatedMessage) &&
        preferences.enableElevatedMessageHighlight)
    {
        return true;
    }

    if ((message.flags.has(MessageFlag::FirstMessage) &&
         preferences.enableFirstMessageHighlight) ||
        (message.flags.has(MessageFlag::FirstMessageSession) &&
         preferences.enableFirstMessageSessionHighlight))
    {
        return true;
    }

    if (message.flags.has(MessageFlag::WatchStreak) &&
        preferences.enableWatchStreakHighlight)
    {
        return true;
    }

    if (message.flags.has(MessageFlag::Subscription) &&
        preferences.enableSubHighlight)
    {
        return true;
    }

    if ((message.flags.has(MessageFlag::RedeemedHighlight) ||
         message.flags.has(MessageFlag::RedeemedChannelPointReward)) &&
        preferences.enableRedeemedHighlight)
    {
        return true;
    }

    if (includeActivityCheerOverlay &&
        message.flags.has(MessageFlag::CheerMessage))
    {
        return true;
    }

    return isPlatformAlertMessage(message);
}

bool hasEnabledFirstMessageHighlight(const Message &message,
                                     const MessagePreferences &preferences)
{
    return (message.flags.has(MessageFlag::FirstMessage) &&
            preferences.enableFirstMessageHighlight) ||
           (message.flags.has(MessageFlag::FirstMessageSession) &&
            preferences.enableFirstMessageSessionHighlight);
}

QColor brightenGradientColor(const QColor &color)
{
    auto brighter = color.lighter(130);
    brighter.setAlpha(std::min(255, qRound(color.alpha() * 1.25)));
    return brighter;
}

bool applyAutomaticEventOverlay(const Message &message, const QColor &baseColor,
                                bool brightenGradient,
                                const MessagePaintContext &ctx,
                                QColor &gradientOverlayColor,
                                QColor &solidOverlayColor)
{
    auto resolvedColor =
        automaticEventHighlightColor(message, baseColor, ctx);
    if (!resolvedColor.isValid())
    {
        return false;
    }

    if (automaticEventHighlightUsesGradient(ctx))
    {
        gradientOverlayColor =
            brightenGradient ? brightenGradientColor(resolvedColor)
                             : resolvedColor;
    }
    else
    {
        solidOverlayColor = resolvedColor;
    }

    return true;
}

QColor firstMessageGradientLeadInColor(const QColor &baseColor,
                                       const QColor &gradientColor)
{
    if (!baseColor.isValid())
    {
        return {};
    }

    auto leadIn = baseColor;
    leadIn.setAlpha(gradientColor.alpha());
    return leadIn;
}

void fillLeadingFade(QPainter &painter, const QRect &rect,
                     const QColor &leadingColor,
                     const QColor &leadInColor = {})
{
    if (!leadingColor.isValid() || leadingColor.alpha() == 0)
    {
        return;
    }

    QLinearGradient gradient(rect.left(), 0, rect.right(), 0);
    QColor transparent(leadingColor);
    transparent.setAlpha(0);

    if (leadInColor.isValid() && leadInColor.alpha() > 0)
    {
        gradient.setColorAt(0.0, leadInColor);
        gradient.setColorAt(0.1, leadingColor);
    }
    else
    {
        gradient.setColorAt(0.0, leadingColor);
    }

    gradient.setColorAt(0.5, transparent);
    gradient.setColorAt(1.0, transparent);
    painter.fillRect(rect, gradient);
}
}  // namespace

MessageLayout::MessageLayout(MessagePtr message)
    : message_(std::move(message))
{
    DebugCount::increase(DebugObject::MessageLayout);
}

MessageLayout::~MessageLayout()
{
    DebugCount::decrease(DebugObject::MessageLayout);
}

const Message *MessageLayout::getMessage()
{
    return this->message_.get();
}

const MessagePtr &MessageLayout::getMessagePtr() const
{
    return this->message_;
}

// Height
int MessageLayout::getHeight() const
{
    return static_cast<int>(this->container_.getHeight());
}

int MessageLayout::getWidth() const
{
    return static_cast<int>(this->container_.getWidth());
}

// Layout
// return true if redraw is required
bool MessageLayout::layout(const MessageLayoutContext &ctx,
                           bool shouldInvalidateBuffer)
{
    //    BenchmarkGuard benchmark("MessageLayout::layout()");

    bool layoutRequired = false;

    // check if width changed
    bool widthChanged = ctx.width != this->currentLayoutWidth_;
    layoutRequired |= widthChanged;
    this->currentLayoutWidth_ = ctx.width;

    // check if layout state changed
    const auto layoutGeneration = getApp()->getWindows()->getGeneration();
    if (this->layoutState_ != layoutGeneration)
    {
        layoutRequired = true;
        this->flags.set(MessageLayoutFlag::RequiresBufferUpdate);
        this->layoutState_ = layoutGeneration;
    }

    // check if work mask changed
    layoutRequired |= this->currentWordFlags_ != ctx.flags;
    this->currentWordFlags_ = ctx.flags;  // getSettings()->getWordTypeMask();

    // check if layout was requested manually
    layoutRequired |= this->flags.has(MessageLayoutFlag::RequiresLayout);
    this->flags.unset(MessageLayoutFlag::RequiresLayout);

    // check if dpi changed
    layoutRequired |= this->scale_ != ctx.scale;
    this->scale_ = ctx.scale;
    layoutRequired |= this->imageScale_ != ctx.imageScale;
    this->imageScale_ = ctx.imageScale;

    if (!layoutRequired)
    {
        if (shouldInvalidateBuffer)
        {
            this->invalidateBuffer();
            return true;
        }
        return false;
    }

    qreal oldHeight = this->container_.getHeight();
    this->actuallyLayout(ctx);
    if (widthChanged || this->container_.getHeight() != oldHeight)
    {
        this->deleteBuffer();
    }
    this->invalidateBuffer();

    return true;
}

void MessageLayout::actuallyLayout(const MessageLayoutContext &ctx)
{
#ifdef FOURTF
    this->layoutCount_++;
#endif

    auto messageFlags = this->message_->flags;

    if (this->flags.has(MessageLayoutFlag::Expanded) ||
        (ctx.flags.has(MessageElementFlag::ModeratorTools) &&
         !this->message_->flags.has(MessageFlag::Disabled)))
    {
        messageFlags.unset(MessageFlag::Collapsed);
    }

    bool hideModerated = getSettings()->hideModerated;
    bool hideModerationActions = getSettings()->hideModerationActions;
    bool hideBlockedTermAutomodMessages =
        getSettings()->showBlockedTermAutomodMessages.getEnum() ==
        ShowModerationState::Never;
    bool hideSimilar = getSettings()->hideSimilar;
    bool hideReplies = !ctx.flags.has(MessageElementFlag::RepliedMessage);

    this->container_.beginLayout(ctx.width, this->scale_, this->imageScale_,
                                 messageFlags);

    for (const auto &element : this->message_->elements)
    {
        if (hideModerated && this->message_->flags.has(MessageFlag::Disabled))
        {
            continue;
        }

        if (hideBlockedTermAutomodMessages &&
            this->message_->flags.has(MessageFlag::AutoModBlockedTerm))
        {
            // NOTE: This hides the message but it will make the message re-appear if moderation message hiding is no longer active, and the layout is re-laid-out.
            // This is only the case for the moderation messages that don't get filtered during creation.
            // We should decide which is the correct method & apply that everywhere
            continue;
        }

        if (this->message_->flags.has(MessageFlag::RestrictedMessage))
        {
            if (getApp()->getStreamerMode()->shouldHideRestrictedUsers())
            {
                // Message is being hidden because the source is a
                // restricted user
                continue;
            }
        }

        if (this->message_->flags.has(MessageFlag::ModerationAction))
        {
            if (hideModerationActions ||
                getApp()->getStreamerMode()->shouldHideModActions())
            {
                // Message is being hidden because we consider the message
                // a moderation action (something a streamer is unlikely to
                // want to share if they briefly show their chat on stream)
                continue;
            }
        }

        if (hideSimilar && this->message_->flags.has(MessageFlag::Similar))
        {
            continue;
        }

        if (hideReplies &&
            element->getFlags().has(MessageElementFlag::RepliedMessage))
        {
            continue;
        }

        element->addToContainer(this->container_, ctx);
    }

    if (this->height_ != this->container_.getHeight())
    {
        this->deleteBuffer();
    }

    this->container_.endLayout();
    this->height_ = this->container_.getHeight();

    // collapsed state
    this->flags.unset(MessageLayoutFlag::Collapsed);
    if (this->container_.isCollapsed())
    {
        this->flags.set(MessageLayoutFlag::Collapsed);
    }
}

// Painting
MessagePaintResult MessageLayout::paint(const MessagePaintContext &ctx)
{
    MessagePaintResult result;

    QPixmap *pixmap = this->ensureBuffer(ctx.painter, ctx.canvasWidth,
                                         ctx.messageColors.hasTransparency);

    if (!this->bufferValid_)
    {
        if (ctx.messageColors.hasTransparency)
        {
            pixmap->fill(Qt::transparent);
        }
        this->updateBuffer(pixmap, ctx);
    }

    // draw on buffer
    ctx.painter.drawPixmap(QPoint{0, ctx.y}, *pixmap);

    // draw gif emotes
    result.hasAnimatedElements =
        this->container_.paintAnimatedElements(ctx.painter, ctx.y);

    // draw disabled
    if (this->message_->flags.has(MessageFlag::Disabled))
    {
        ctx.painter.fillRect(
            QRect{
                0,
                ctx.y,
                pixmap->width(),
                pixmap->height(),
            },
            ctx.messageColors.disabled);
    }

    if (this->message_->flags.has(MessageFlag::RecentMessage) &&
        ctx.preferences.fadeMessageHistory)
    {
        ctx.painter.fillRect(
            QRect{
                0,
                ctx.y,
                pixmap->width(),
                pixmap->height(),
            },
            ctx.messageColors.disabled);
    }

    if (!ctx.isMentions &&
        (this->message_->flags.has(MessageFlag::RedeemedChannelPointReward) ||
         this->message_->flags.has(MessageFlag::RedeemedHighlight)) &&
        ctx.preferences.enableRedeemedHighlight)
    {
        auto redeemedStripeColor = automaticEventHighlightColor(
            *this->message_,
            *ColorProvider::instance().color(ColorType::RedeemedHighlight),
            ctx);
        if (redeemedStripeColor.isValid())
        {
            ctx.painter.fillRect(
                QRect{
                    0,
                    ctx.y,
                    static_cast<int>(this->scale_ * 4),
                    pixmap->height(),
                },
                redeemedStripeColor);
        }
    }

    // draw selection
    if (!ctx.selection.isEmpty())
    {
        this->container_.paintSelection(ctx.painter, ctx.messageIndex,
                                        ctx.selection, ctx.y);
    }

    // draw message seperation line
    if (ctx.preferences.separateMessages)
    {
        ctx.painter.fillRect(
            QRectF{
                0.0,
                static_cast<qreal>(ctx.y),
                this->container_.getWidth() + 64,
                1.0,
            },
            ctx.messageColors.messageSeperator);
    }

    // draw last read message line
    if (ctx.isLastReadMessage)
    {
        QColor color;
        if (ctx.preferences.lastMessageColor.isValid())
        {
            color = ctx.preferences.lastMessageColor;
        }
        else
        {
            color = ctx.isWindowFocused
                        ? ctx.messageColors.focusedLastMessageLine
                        : ctx.messageColors.unfocusedLastMessageLine;
        }

        QBrush brush(color, ctx.preferences.lastMessagePattern);

        ctx.painter.fillRect(
            QRectF{
                0,
                ctx.y + this->container_.getHeight() - 1,
                static_cast<qreal>(pixmap->width()),
                1,
            },
            brush);
    }

    this->bufferValid_ = true;

    return result;
}

QPixmap *MessageLayout::ensureBuffer(QPainter &painter, qreal width, bool clear)
{
    if (this->buffer_ != nullptr)
    {
        return this->buffer_.get();
    }

    // Create new buffer
    this->buffer_ = std::make_unique<QPixmap>(
        static_cast<int>(width * painter.device()->devicePixelRatioF()),
        static_cast<int>(this->container_.getHeight() *
                         painter.device()->devicePixelRatioF()));
    this->buffer_->setDevicePixelRatio(painter.device()->devicePixelRatioF());

    if (clear)
    {
        this->buffer_->fill(Qt::transparent);
    }

    this->bufferValid_ = false;
    DebugCount::increase(DebugObject::MessageDrawingBuffer);
    return this->buffer_.get();
}

void MessageLayout::updateBuffer(QPixmap *buffer,
                                 const MessagePaintContext &ctx)
{
    if (buffer->isNull())
    {
        return;
    }

    QPainter painter(buffer);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // draw background
    QColor backgroundColor = [&] {
        if (this->message_->flags.has(MessageFlag::WatchStreak) &&
            ctx.preferences.enableWatchStreakHighlight)
        {
            return ctx.messageColors.regularBg;
        }

        if (ctx.preferences.alternateMessages &&
            this->flags.has(MessageLayoutFlag::AlternateBackground))
        {
            return ctx.messageColors.alternateBg;
        }

        return ctx.messageColors.regularBg;
    }();
    QColor solidOverlayColor;
    QColor gradientOverlayColor;
    QColor gradientLeadInColor;

    const bool isWatchStreakEvent =
        this->message_->flags.has(MessageFlag::WatchStreak) &&
        ctx.preferences.enableWatchStreakHighlight;

    bool suppressMergedPlatformTint =
        usesAutomaticEventOverlay(*this->message_, ctx.preferences,
                                  ctx.forceFlatEventHighlights) &&
        ((ctx.forceFlatEventHighlights &&
          mergedPlatformIndicatorShowsLineColor(ctx.platformIndicatorMode)) ||
         (automaticEventHighlightUsesGradient(ctx) &&
          (isWatchStreakEvent ||
           !automaticEventIncludesUserMessage(*this->message_))));

    if (this->message_->platformAccentColor &&
        mergedPlatformIndicatorShowsLineColor(
            ctx.platformIndicatorMode) &&
        !suppressMergedPlatformTint)
    {
        backgroundColor = blendColors(backgroundColor,
                                      *this->message_->platformAccentColor);
    }

    if (this->message_->flags.has(MessageFlag::ElevatedMessage) &&
        ctx.preferences.enableElevatedMessageHighlight)
    {
        applyAutomaticEventOverlay(*this->message_,
                                   *ctx.colorProvider.color(
                                       ColorType::ElevatedMessageHighlight),
                                   false, ctx,
                                   gradientOverlayColor, solidOverlayColor);
    }

    else if (hasEnabledFirstMessageHighlight(*this->message_, ctx.preferences))
    {
        auto firstMessageBaseColor =
            *ctx.colorProvider.color(ColorType::FirstMessageHighlight);
        applyAutomaticEventOverlay(*this->message_,
                                   firstMessageBaseColor,
                                   false, ctx,
                                   gradientOverlayColor, solidOverlayColor);
        if (gradientOverlayColor.isValid())
        {
            gradientLeadInColor = firstMessageGradientLeadInColor(
                firstMessageBaseColor, gradientOverlayColor);
        }
    }
    else if (this->message_->flags.has(MessageFlag::WatchStreak) &&
             ctx.preferences.enableWatchStreakHighlight)
    {
        applyAutomaticEventOverlay(
            *this->message_, *ctx.colorProvider.color(ColorType::WatchStreak),
            false, ctx,
            gradientOverlayColor, solidOverlayColor);
    }
    else if (ctx.forceFlatEventHighlights &&
             this->message_->flags.has(MessageFlag::CheerMessage))
    {
        applyAutomaticEventOverlay(*this->message_, {}, false, ctx,
                                   gradientOverlayColor, solidOverlayColor);
    }
    else if ((this->message_->flags.has(MessageFlag::Highlighted) ||
              this->message_->flags.has(MessageFlag::HighlightedWhisper)) &&
             !this->flags.has(MessageLayoutFlag::IgnoreHighlights))
    {
        assert(this->message_->highlightColor);
        solidOverlayColor = this->message_->highlightColor
                                ? *this->message_->highlightColor
                                : QColor{};
    }
    else if (this->message_->flags.has(MessageFlag::Subscription) &&
             ctx.preferences.enableSubHighlight)
    {
        applyAutomaticEventOverlay(
            *this->message_, *ctx.colorProvider.color(ColorType::Subscription),
            false, ctx,
            gradientOverlayColor, solidOverlayColor);
    }
    else if ((this->message_->flags.has(MessageFlag::RedeemedHighlight) ||
              this->message_->flags.has(
                   MessageFlag::RedeemedChannelPointReward)) &&
             ctx.preferences.enableRedeemedHighlight)
    {
        applyAutomaticEventOverlay(
            *this->message_,
            *ctx.colorProvider.color(ColorType::RedeemedHighlight),
            false, ctx,
            gradientOverlayColor, solidOverlayColor);
    }
    else if (isPlatformAlertMessage(*this->message_))
    {
        applyAutomaticEventOverlay(*this->message_, {}, true, ctx,
                                   gradientOverlayColor, solidOverlayColor);
    }
    else if (this->message_->flags.has(MessageFlag::AutoMod) ||
             this->message_->flags.has(MessageFlag::LowTrustUsers))
    {
        if (ctx.preferences.enableAutomodHighlight &&
            (this->message_->flags.has(MessageFlag::AutoModOffendingMessage) ||
             this->message_->flags.has(
                  MessageFlag::AutoModOffendingMessageHeader)))
        {
            solidOverlayColor =
                *ctx.colorProvider.color(ColorType::AutomodHighlight);
        }
        else
        {
            backgroundColor = QColor("#404040");
        }
    }
    else if (this->message_->flags.has(MessageFlag::Debug))
    {
        backgroundColor = QColor("#4A273D");
    }

    painter.fillRect(buffer->rect(), backgroundColor);
    if (gradientOverlayColor.isValid())
    {
        fillLeadingFade(painter, buffer->rect(), gradientOverlayColor,
                        gradientLeadInColor);
    }
    else if (solidOverlayColor.isValid())
    {
        painter.fillRect(buffer->rect(), solidOverlayColor);
    }

    // draw message
    this->container_.paintElements(painter, ctx);

#ifdef FOURTF
    // debug
    painter.setPen(QColor(255, 0, 0));
    painter.drawRect(buffer->rect().x(), buffer->rect().y(),
                     buffer->rect().width() - 1, buffer->rect().height() - 1);

    QTextOption option;
    option.setAlignment(Qt::AlignRight | Qt::AlignTop);

    painter.drawText(QRectF(1, 1, this->container_.getWidth() - 3, 1000),
                     QString::number(this->layoutCount_) + ", " +
                         QString::number(++this->bufferUpdatedCount_),
                     option);
#endif
}

void MessageLayout::invalidateBuffer()
{
    this->bufferValid_ = false;
}

void MessageLayout::deleteBuffer()
{
    if (this->buffer_ != nullptr)
    {
        DebugCount::decrease(DebugObject::MessageDrawingBuffer);

        this->buffer_ = nullptr;
    }
}

void MessageLayout::deleteCache()
{
    this->deleteBuffer();

#ifdef XD
    this->container_.clear();
#endif
}

// Elements
//    assert(QThread::currentThread() == QApplication::instance()->thread());

// returns nullptr if none was found

// fourtf: this should return a MessageLayoutItem
const MessageLayoutElement *MessageLayout::getElementAt(QPointF point) const
{
    // go through all words and return the first one that contains the point.
    return this->container_.getElementAt(point);
}

std::pair<int, int> MessageLayout::getWordBounds(
    const MessageLayoutElement *hoveredElement, QPointF relativePos) const
{
    // An element with wordId != -1 can be multiline, so we need to check all
    // elements in the container
    if (hoveredElement->getWordId() != -1)
    {
        return this->container_.getWordBounds(hoveredElement);
    }

    const auto wordStart = this->getSelectionIndex(relativePos) -
                           hoveredElement->getMouseOverIndex(relativePos);
    const auto selectionLength = hoveredElement->getSelectionIndexCount();
    const auto length = hoveredElement->hasTrailingSpace() ? selectionLength - 1
                                                           : selectionLength;

    return {wordStart, wordStart + length};
}

size_t MessageLayout::getLastCharacterIndex() const
{
    return this->container_.getLastCharacterIndex();
}

size_t MessageLayout::getFirstMessageCharacterIndex() const
{
    return this->container_.getFirstMessageCharacterIndex();
}

size_t MessageLayout::getSelectionIndex(QPointF position) const
{
    return this->container_.getSelectionIndex(position);
}

void MessageLayout::addSelectionText(QString &str, uint32_t from, uint32_t to,
                                     CopyMode copymode)
{
    this->container_.addSelectionText(str, from, to, copymode);
}

}  // namespace chatterino
