#include "providers/seventv/paints/SolidColorPaint.hpp"

#include <utility>

namespace chatterino {

SolidColorPaint::SolidColorPaint(QString name, QString id,
                                 std::optional<QColor> color,
                                 std::vector<PaintDropShadow> dropShadows)
    : Paint(std::move(id))
    , name_(std::move(name))
    , color_(color)
    , dropShadows_(std::move(dropShadows))
{
}

bool SolidColorPaint::animated() const
{
    return false;
}

QBrush SolidColorPaint::asBrush(QColor userColor, QRectF) const
{
    return {this->color_.value_or(userColor)};
}

const std::vector<PaintDropShadow> &SolidColorPaint::getDropShadows() const
{
    return this->dropShadows_;
}

}  // namespace chatterino
