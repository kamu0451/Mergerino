#pragma once

#include "providers/seventv/paints/Paint.hpp"

#include <optional>

namespace chatterino {

class SolidColorPaint : public Paint
{
public:
    SolidColorPaint(QString name, QString id, std::optional<QColor> color,
                    std::vector<PaintDropShadow> dropShadows);

    bool animated() const override;
    QBrush asBrush(QColor userColor, QRectF drawingRect) const override;
    const std::vector<PaintDropShadow> &getDropShadows() const override;

private:
    QString name_;
    std::optional<QColor> color_;
    std::vector<PaintDropShadow> dropShadows_;
};

}  // namespace chatterino
