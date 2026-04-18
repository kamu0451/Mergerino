// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/Label.hpp"

#include "Application.hpp"

#include <QPainter>

namespace chatterino {

Label::Label(QString text, FontStyle style)
    : Label(nullptr, std::move(text), style)
{
}

Label::Label(BaseWidget *parent, QString text, FontStyle style)
    : BaseWidget(parent)
    , text_(std::move(text))
    , fontStyle_(style)
    , basePadding_(8, 0, 8, 0)
{
    this->connections_.managedConnect(getApp()->getFonts()->fontChanged,
                                      [this] {
                                          this->updateSize();
                                      });
    this->updateSize();
}

const QString &Label::getText() const
{
    return this->text_;
}

void Label::setText(const QString &text)
{
    if (this->text_ != text)
    {
        this->text_ = text;
        if (this->shouldElide_)
        {
            this->updateElidedText(this->getFontMetrics(),
                                   this->textRect().width());
        }
        this->updateSize();
        this->update();
    }
}

FontStyle Label::getFontStyle() const
{
    return this->fontStyle_;
}

bool Label::getCentered() const
{
    return this->centered_;
}

void Label::setCentered(bool centered)
{
    this->centered_ = centered;
    this->updateSize();
}

void Label::setPadding(QMargins padding)
{
    this->basePadding_ = padding;
    this->updateSize();
}

bool Label::getWordWrap() const
{
    return this->wordWrap_;
}

void Label::setWordWrap(bool wrap)
{
    this->wordWrap_ = wrap;
    this->update();
}

void Label::setShouldElide(bool shouldElide)
{
    this->shouldElide_ = shouldElide;
    this->updateSize();
    this->update();
}

void Label::setTrailingText(const QString &text, QColor color)
{
    if (this->trailingText_ == text && this->trailingColor_ == color)
    {
        return;
    }
    this->trailingText_ = text;
    this->trailingColor_ = color;
    this->updateSize();
    this->update();
}

void Label::setFontStyle(FontStyle style)
{
    this->fontStyle_ = style;
    this->updateSize();
}

void Label::scaleChangedEvent(float /*scale*/)
{
    this->updateSize();
}

QSize Label::sizeHint() const
{
    return this->sizeHint_;
}

QSize Label::minimumSizeHint() const
{
    return this->minimumSizeHint_;
}

void Label::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);

    auto metrics = this->getFontMetrics();

    painter.setFont(
        getApp()->getFonts()->getFont(this->getFontStyle(), this->scale()));

    // draw text
    QRectF textRect = this->textRect();

    auto text = [this] {
        if (this->shouldElide_)
        {
            return this->elidedText_;
        }

        return this->text_;
    }();

    const qreal mainWidth = metrics.horizontalAdvance(text);
    const qreal trailingWidth =
        this->trailingText_.isEmpty()
            ? 0.0
            : metrics.horizontalAdvance(this->trailingText_);
    const qreal combinedWidth = mainWidth + trailingWidth;

    Qt::Alignment alignment =
        !this->centered_ || combinedWidth > textRect.width()
            ? Qt::AlignLeft | Qt::AlignVCenter
            : Qt::AlignCenter;

    if (this->trailingText_.isEmpty())
    {
        painter.setBrush(this->palette().windowText());

        QTextOption option(alignment);
        if (this->wordWrap_)
        {
            option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        }
        else
        {
            option.setWrapMode(QTextOption::NoWrap);
        }
        painter.drawText(textRect, text, option);
    }
    else
    {
        // Custom two-color draw: main text in palette color, trailing text
        // in trailingColor_. Drawn as a single centered or left-aligned unit.
        qreal startX = textRect.left();
        if (alignment.testFlag(Qt::AlignHCenter) ||
            alignment == Qt::AlignCenter)
        {
            startX =
                textRect.left() + (textRect.width() - combinedWidth) / 2.0;
        }
        const qreal baselineY = textRect.top() +
                                (textRect.height() + metrics.ascent() -
                                 metrics.descent()) /
                                    2.0;

        painter.setPen(this->palette().color(QPalette::WindowText));
        painter.drawText(QPointF(startX, baselineY), text);

        painter.setPen(this->trailingColor_);
        painter.drawText(QPointF(startX + mainWidth, baselineY),
                         this->trailingText_);
    }

#if 0
    painter.setPen(QColor(255, 0, 0));
    painter.drawRect(0, 0, this->width() - 1, this->height() - 1);
#endif
}

void Label::resizeEvent(QResizeEvent *event)
{
    if (this->shouldElide_)
    {
        auto metrics = this->getFontMetrics();
        if (this->updateElidedText(metrics, this->textRect().width()))
        {
            this->update();
        }
    }

    BaseWidget::resizeEvent(event);
}

QFontMetricsF Label::getFontMetrics() const
{
    return getApp()->getFonts()->getFontMetrics(this->fontStyle_,
                                                this->scale());
}

void Label::updateSize()
{
    this->currentPadding_ = this->basePadding_.toMarginsF() * this->scale();

    auto metrics = this->getFontMetrics();

    auto yPadding =
        this->currentPadding_.top() + this->currentPadding_.bottom();

    auto height = metrics.height() + yPadding;
    if (this->shouldElide_)
    {
        this->updateElidedText(metrics, this->textRect().width());
        this->sizeHint_ = QSizeF(-1, height).toSize();
        this->minimumSizeHint_ = this->sizeHint_;
    }
    else
    {
        auto width = metrics.horizontalAdvance(this->text_) +
                     metrics.horizontalAdvance(this->trailingText_) +
                     this->currentPadding_.left() +
                     this->currentPadding_.right();
        this->sizeHint_ = QSizeF(width, height).toSize();
        this->minimumSizeHint_ = this->sizeHint_;
    }

    this->updateGeometry();
}

bool Label::updateElidedText(const QFontMetricsF &fontMetrics, qreal width)
{
    assert(this->shouldElide_ == true);
    auto elidedText = fontMetrics.elidedText(
        this->text_, Qt::TextElideMode::ElideRight, width);

    if (elidedText != this->elidedText_)
    {
        this->elidedText_ = elidedText;
        return true;
    }

    return false;
}

QRectF Label::textRect() const
{
    return QRectF(this->rect()).marginsRemoved(this->currentPadding_);
}

}  // namespace chatterino
