#include "providers/seventv/paints/Paint.hpp"

#include "Application.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"

#include <private/qpixmapfilter_p.h>
#include <QLabel>
#include <QPainter>

#include <algorithm>

namespace chatterino {

using namespace Qt::Literals;

QMarginsF Paint::pixmapPadding(float scale, float dpr) const
{
    if (this->getDropShadows().empty() ||
        !getSettings()->displaySevenTVPaintShadows)
    {
        return {};
    }

    QMarginsF padding;
    const auto effectiveDpr = std::max(dpr, 1.0F);
    const auto shadowScale = scale / effectiveDpr;
    for (const auto &shadow : this->getDropShadows())
    {
        if (!shadow.isValid())
        {
            continue;
        }

        const auto shadowPadding = shadow.padding(shadowScale);
        padding.setLeft(std::max(padding.left(), shadowPadding.left()));
        padding.setTop(std::max(padding.top(), shadowPadding.top()));
        padding.setRight(std::max(padding.right(), shadowPadding.right()));
        padding.setBottom(std::max(padding.bottom(), shadowPadding.bottom()));
    }
    return padding;
}

QPixmap Paint::getPixmap(const QString &text, const QFont &font,
                         QColor userColor, QSizeF size, float scale,
                         float dpr) const
{
    const auto effectiveDpr = std::max(dpr, 1.0F);
    const auto padding = this->pixmapPadding(scale, effectiveDpr);
    const QSizeF pixmapSize(
        size.width() + padding.left() + padding.right(),
        size.height() + padding.top() + padding.bottom());
    const QPointF textOffset(padding.left(), padding.top());

    QPixmap pixmap((pixmapSize * effectiveDpr).toSize());
    pixmap.setDevicePixelRatio(effectiveDpr);
    pixmap.fill(Qt::transparent);

    QPainter pixmapPainter(&pixmap);
    pixmapPainter.setRenderHint(QPainter::SmoothPixmapTransform);
    pixmapPainter.setFont(font);

    // NOTE: draw colon separately from the nametag
    // otherwise the paint would extend onto the colon
    bool drawColon = false;
    QRectF nametagBoundingRect{textOffset, size};
    QString nametagText = text;
    if (nametagText.endsWith(':'))
    {
        drawColon = true;
        nametagText = nametagText.chopped(1);
        nametagBoundingRect = pixmapPainter.boundingRect(
            QRectF(textOffset, QSizeF(10000, 10000)), nametagText,
            QTextOption(Qt::AlignLeft | Qt::AlignTop));
    }

    QPen pen;
    // Anchor the brush to the rect the text is actually drawn in - an
    // origin-anchored rect would shift gradients/textures by the shadow
    // padding offset.
    const QBrush brush = this->asBrush(userColor, nametagBoundingRect);
    pen.setBrush(brush);
    pixmapPainter.setPen(pen);

    pixmapPainter.drawText(nametagBoundingRect, nametagText,
                           QTextOption(Qt::AlignLeft | Qt::AlignTop));
    pixmapPainter.end();

    if (!this->getDropShadows().empty() &&
        getSettings()->displaySevenTVPaintShadows)
    {
        QPixmap outMap((pixmapSize * effectiveDpr).toSize());
        outMap.setDevicePixelRatio(effectiveDpr);
        for (const auto &shadow : this->getDropShadows())
        {
            if (!shadow.isValid())
            {
                continue;
            }
            outMap.fill(Qt::transparent);

            {
                QPainter outPainter(&outMap);
                auto scaled = shadow.scaled(
                    scale / static_cast<float>(outMap.devicePixelRatio()));

                QPixmapDropShadowFilter filter;
                scaled.apply(filter);
                filter.draw(&outPainter, {0, 0}, pixmap);
            }
            outMap.swap(pixmap);
        }
    }

    if (drawColon)
    {
        auto colonColor = getApp()->getThemes()->messages.textColors.regular;

        pixmapPainter.begin(&pixmap);

        pixmapPainter.setPen(QPen(colonColor));
        pixmapPainter.setFont(font);

        QRectF colonBoundingRect(nametagBoundingRect.right(), textOffset.y(),
                                 10000, 10000);
        pixmapPainter.drawText(colonBoundingRect, u":"_s,
                               QTextOption(Qt::AlignLeft | Qt::AlignTop));
        pixmapPainter.end();
    }

    return pixmap;
}

QColor Paint::overlayColors(QColor background, QColor foreground)
{
    auto alpha = foreground.alphaF();

    auto r = ((1 - alpha) * static_cast<float>(background.red())) +
             (alpha * static_cast<float>(foreground.red()));
    auto g = ((1 - alpha) * static_cast<float>(background.green())) +
             (alpha * static_cast<float>(foreground.green()));
    auto b = ((1 - alpha) * static_cast<float>(background.blue())) +
             (alpha * static_cast<float>(foreground.blue()));

    return {static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)};
}

qreal Paint::offsetRepeatingStopPosition(const qreal position,
                                         const QGradientStops &stops)
{
    const qreal gradientStart = stops.first().first;
    const qreal gradientEnd = stops.last().first;
    const qreal gradientLength = gradientEnd - gradientStart;
    const qreal offsetPosition = (position - gradientStart) / gradientLength;

    return offsetPosition;
}

}  // namespace chatterino
