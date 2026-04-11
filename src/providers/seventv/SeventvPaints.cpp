#include "providers/seventv/SeventvPaints.hpp"

#include "Application.hpp"
#include "messages/Image.hpp"
#include "providers/seventv/SeventvAPI.hpp"
#include "providers/seventv/eventapi/Dispatch.hpp"
#include "providers/seventv/paints/LinearGradientPaint.hpp"
#include "providers/seventv/paints/PaintDropShadow.hpp"
#include "providers/seventv/paints/RadialGradientPaint.hpp"
#include "providers/seventv/paints/SolidColorPaint.hpp"
#include "providers/seventv/paints/UrlPaint.hpp"
#include "singletons/WindowManager.hpp"
#include "util/DebugCount.hpp"
#include "util/PostToThread.hpp"
#include "util/Variant.hpp"

#include <QUrlQuery>
#include <utility>

namespace {
using namespace chatterino;
using namespace Qt::Literals;

QColor rgbaToQColor(const uint32_t color)
{
    auto red = (int)((color >> 24) & 0xFF);
    auto green = (int)((color >> 16) & 0xFF);
    auto blue = (int)((color >> 8) & 0xFF);
    auto alpha = (int)(color & 0xFF);

    return {red, green, blue, alpha};
}

std::optional<QColor> parsePaintColor(const QJsonValue &color)
{
    if (color.isNull())
    {
        return std::nullopt;
    }

    return rgbaToQColor(color.toInt());
}

QGradientStops parsePaintStops(const QJsonArray &stops)
{
    QGradientStops parsedStops;
    double lastStop = -1;

    for (const auto &stop : stops)
    {
        const auto stopObject = stop.toObject();

        const auto rgbaColor = stopObject["color"].toInt();
        auto position = stopObject["at"].toDouble();

        // HACK: qt does not support hard edges in gradients like css does
        // Setting a different color at the same position twice just overwrites
        // the previous color. So we have to shift the second point slightly
        // ahead, simulating an actual hard edge
        if (position <= lastStop)
        {
            position = lastStop + 0.0000001;
        }

        lastStop = position;
        parsedStops.append(QGradientStop(position, rgbaToQColor(rgbaColor)));
    }

    return parsedStops;
}

std::vector<PaintDropShadow> parseDropShadows(const QJsonArray &dropShadows)
{
    std::vector<PaintDropShadow> parsedDropShadows;

    for (const auto &shadow : dropShadows)
    {
        const auto shadowObject = shadow.toObject();

        const auto xOffset = shadowObject["x_offset"].toDouble();
        const auto yOffset = shadowObject["y_offset"].toDouble();
        const auto radius = shadowObject["radius"].toDouble();
        const auto rgbaColor = shadowObject["color"].toInt();

        parsedDropShadows.emplace_back(xOffset, yOffset, radius,
                                       rgbaToQColor(rgbaColor));
    }

    return parsedDropShadows;
}

std::optional<std::shared_ptr<Paint>> parsePaint(const QJsonObject &paintJson)
{
    const QString name = paintJson["name"].toString();
    const QString id = paintJson["id"].toString();

    const auto color = parsePaintColor(paintJson["color"]);
    const bool repeat = paintJson["repeat"].toBool();
    const float angle = (float)paintJson["angle"].toDouble();

    const QGradientStops stops = parsePaintStops(paintJson["stops"].toArray());

    const auto shadows = parseDropShadows(paintJson["shadows"].toArray());

    const QString function = paintJson["function"].toString();
    if (function == "LINEAR_GRADIENT" || function == "linear-gradient")
    {
        return std::make_shared<LinearGradientPaint>(name, id, color, stops,
                                                     repeat, angle, shadows);
    }

    if (function == "RADIAL_GRADIENT" || function == "radial-gradient")
    {
        return std::make_shared<RadialGradientPaint>(name, id, stops, repeat,
                                                     shadows);
    }

    if (function == "URL" || function == "url")
    {
        const QString url = paintJson["image_url"].toString();
        const ImagePtr image = Image::fromUrl({url}, 1);
        if (image == nullptr)
        {
            return std::nullopt;
        }

        return std::make_shared<UrlPaint>(name, id, image, shadows);
    }

    if (color || !shadows.empty())
    {
        return std::make_shared<SolidColorPaint>(name, id, color, shadows);
    }

    return std::nullopt;
}

QString normalizeUsername(const QString &userName)
{
    return userName.toLower();
}

bool setIfDifferent(auto &map, auto &&key, const auto &value)
{
    auto it = map.find(key);
    if (it == map.end())
    {
        map.emplace(std::forward<decltype(key)>(key), value);
        return true;
    }

    if (it->second != value)
    {
        it->second = value;
        return true;
    }

    return false;
}

QJsonObject graphQLPaintToLegacyPaint(const QJsonObject &paint)
{
    QJsonObject legacy{
        {"id", paint["id"].toString()},
        {"name", paint["name"].toString()},
        {"color", paint["color"]},
    };

    QJsonArray mergedShadows = paint["shadows"].toArray();
    const auto text = paint["text"].toObject();
    for (const auto &shadow : text["shadows"].toArray())
    {
        mergedShadows.append(shadow);
    }
    legacy["shadows"] = mergedShadows;

    const auto gradients = paint["gradients"].toArray();
    if (!gradients.isEmpty())
    {
        const auto gradient = gradients.first().toObject();
        legacy["function"] = gradient["function"].toString();
        legacy["repeat"] = gradient["repeat"].toBool();
        legacy["angle"] = gradient["angle"].toDouble();
        legacy["stops"] = gradient["stops"].toArray();
        legacy["image_url"] = gradient["image_url"].toString();
    }

    return legacy;
}

}  // namespace

namespace chatterino {

SeventvPaints::SeventvPaints() = default;

std::shared_ptr<Paint> SeventvPaints::getPaint(const QString &userName,
                                               bool kick) const
{
    std::shared_lock lock(this->mutex_);
    const auto normalized = normalizeUsername(userName);

    if (kick)
    {
        const auto it = this->kickPaintMap_.find(normalized);
        if (it != this->kickPaintMap_.end())
        {
            return it->second;
        }
    }
    else
    {
        const auto it = this->twitchPaintMap_.find(normalized);
        if (it != this->twitchPaintMap_.end())
        {
            return it->second;
        }
    }
    return nullptr;
}

void SeventvPaints::addPaint(const QJsonObject &paintJson)
{
    const auto paintID = paintJson["id"].toString();

    bool changed = false;
    int64_t nAdded = 0;

    {
        std::unique_lock lock(this->mutex_);

        std::optional<std::shared_ptr<Paint>> paint = parsePaint(paintJson);
        if (!paint)
        {
            return;
        }

        const bool isNewPaint = !this->knownPaints_.contains(paintID);
        if (isNewPaint)
        {
            DebugCount::increase(DebugObject::SeventvPaints);
        }
        this->knownPaints_[paintID] = *paint;
        const auto &knownPaint = this->knownPaints_.at(paintID);

        for (auto it = this->pendingTwitchPaintAssignments_.begin();
             it != this->pendingTwitchPaintAssignments_.end();)
        {
            if (it->second != paintID)
            {
                ++it;
                continue;
            }

            const bool inserted = !this->twitchPaintMap_.contains(it->first);
            if (setIfDifferent(this->twitchPaintMap_, it->first, knownPaint))
            {
                changed = true;
                if (inserted)
                {
                    nAdded++;
                }
            }
            it = this->pendingTwitchPaintAssignments_.erase(it);
        }

        for (auto it = this->pendingKickPaintAssignments_.begin();
             it != this->pendingKickPaintAssignments_.end();)
        {
            if (it->second != paintID)
            {
                ++it;
                continue;
            }

            const bool inserted = !this->kickPaintMap_.contains(it->first);
            if (setIfDifferent(this->kickPaintMap_, it->first, knownPaint))
            {
                changed = true;
                if (inserted)
                {
                    nAdded++;
                }
            }
            it = this->pendingKickPaintAssignments_.erase(it);
        }

        for (auto &[userName, assignedPaint] : this->twitchPaintMap_)
        {
            if (assignedPaint->id == paintID && assignedPaint != knownPaint)
            {
                assignedPaint = knownPaint;
                changed = true;
            }
        }

        for (auto &[userName, assignedPaint] : this->kickPaintMap_)
        {
            if (assignedPaint->id == paintID && assignedPaint != knownPaint)
            {
                assignedPaint = knownPaint;
                changed = true;
            }
        }
    }

    if (nAdded > 0)
    {
        DebugCount::increase(DebugObject::SeventvPaintAssignments, nAdded);
    }

    if (changed)
    {
        postToThread([] {
            getApp()->getWindows()->invalidateChannelViewBuffers();
        });
    }
}

void SeventvPaints::addPaintFromGraphQL(const QJsonObject &paintJson)
{
    const auto paint = graphQLPaintToLegacyPaint(paintJson);
    if (!paint.isEmpty())
    {
        this->addPaint(paint);
    }
}

void SeventvPaints::removePaint(const QString &paintID)
{
    int64_t nRemoved = 0;
    bool removedKnownPaint = false;

    {
        std::unique_lock lock(this->mutex_);

        removedKnownPaint = this->knownPaints_.erase(paintID) > 0;
        this->requestedPaintIDs_.erase(paintID);

        for (auto it = this->twitchPaintMap_.begin();
             it != this->twitchPaintMap_.end();)
        {
            if (it->second->id == paintID)
            {
                it = this->twitchPaintMap_.erase(it);
                nRemoved++;
            }
            else
            {
                ++it;
            }
        }

        for (auto it = this->kickPaintMap_.begin();
             it != this->kickPaintMap_.end();)
        {
            if (it->second->id == paintID)
            {
                it = this->kickPaintMap_.erase(it);
                nRemoved++;
            }
            else
            {
                ++it;
            }
        }

        for (auto it = this->pendingTwitchPaintAssignments_.begin();
             it != this->pendingTwitchPaintAssignments_.end();)
        {
            if (it->second == paintID)
            {
                it = this->pendingTwitchPaintAssignments_.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = this->pendingKickPaintAssignments_.begin();
             it != this->pendingKickPaintAssignments_.end();)
        {
            if (it->second == paintID)
            {
                it = this->pendingKickPaintAssignments_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    if (removedKnownPaint)
    {
        DebugCount::decrease(DebugObject::SeventvPaints);
    }

    if (nRemoved > 0)
    {
        DebugCount::decrease(DebugObject::SeventvPaintAssignments, nRemoved);
        postToThread([] {
            getApp()->getWindows()->invalidateChannelViewBuffers();
        });
    }
}

void SeventvPaints::assignPaintToUsers(
    const QString &paintID, std::span<const seventv::eventapi::User> users)
{
    bool changed = false;
    int64_t nAdded = 0;
    bool missingPaint = false;

    {
        std::unique_lock lock(this->mutex_);

        const auto paintIt = this->knownPaints_.find(paintID);
        const bool hasKnownPaint = paintIt != this->knownPaints_.end();
        auto addToMap = [&](auto &map, auto &pendingMap,
                            const QString &username) {
            const auto normalized = normalizeUsername(username);
            if (hasKnownPaint)
            {
                const bool inserted = !map.contains(normalized);
                if (setIfDifferent(map, normalized, paintIt->second))
                {
                    changed = true;
                    if (inserted)
                    {
                        nAdded++;
                    }
                }
                pendingMap.erase(normalized);
            }
            else
            {
                pendingMap[normalized] = paintID;
                missingPaint = true;
            }
        };
        for (const auto &user : users)
        {
            std::visit(variant::Overloaded{
                           [&](const seventv::eventapi::TwitchUser &u) {
                               addToMap(this->twitchPaintMap_,
                                        this->pendingTwitchPaintAssignments_,
                                        u.userName);
                           },
                           [&](const seventv::eventapi::KickUser &u) {
                               addToMap(this->kickPaintMap_,
                                        this->pendingKickPaintAssignments_,
                                        u.userName);
                           },
                       },
                       user);
        }
    }

    if (missingPaint)
    {
        this->ensurePaintLoaded(paintID);
    }

    if (nAdded > 0)
    {
        DebugCount::increase(DebugObject::SeventvPaintAssignments, nAdded);
    }

    if (changed)
    {
        postToThread([] {
            getApp()->getWindows()->invalidateChannelViewBuffers();
        });
    }
}

void SeventvPaints::clearPaintFromUsers(
    const QString &paintID, std::span<const seventv::eventapi::User> users)
{
    int64_t nRemoved = 0;

    {
        std::unique_lock lock(this->mutex_);

        auto removeFromMap = [&](auto &map, auto &pendingMap,
                                 const QString &username) {
            const auto normalized = normalizeUsername(username);
            const auto it = map.find(normalized);
            if (it != map.end() && it->second->id == paintID)
            {
                map.erase(it);
                nRemoved++;
            }

            const auto pendingIt = pendingMap.find(normalized);
            if (pendingIt != pendingMap.end() && pendingIt->second == paintID)
            {
                pendingMap.erase(pendingIt);
            }
        };
        for (const auto &user : users)
        {
            std::visit(variant::Overloaded{
                           [&](const seventv::eventapi::TwitchUser &u) {
                               removeFromMap(this->twitchPaintMap_,
                                             this->pendingTwitchPaintAssignments_,
                                             u.userName);
                           },
                           [&](const seventv::eventapi::KickUser &u) {
                               removeFromMap(this->kickPaintMap_,
                                             this->pendingKickPaintAssignments_,
                                             u.userName);
                           },
                       },
                       user);
        }
    }

    if (nRemoved > 0)
    {
        DebugCount::decrease(DebugObject::SeventvPaintAssignments, nRemoved);
        postToThread([] {
            getApp()->getWindows()->invalidateChannelViewBuffers();
        });
    }
}

void SeventvPaints::ensurePaintLoaded(const QString &paintID)
{
    if (paintID.isEmpty())
    {
        return;
    }

    {
        std::shared_lock lock(this->mutex_);
        if (this->knownPaints_.contains(paintID) ||
            this->requestedPaintIDs_.contains(paintID))
        {
            return;
        }
    }

    {
        std::unique_lock lock(this->mutex_);
        if (this->knownPaints_.contains(paintID) ||
            !this->requestedPaintIDs_.emplace(paintID).second)
        {
            return;
        }
    }

    auto *api = getApp()->getSeventvAPI();
    if (!api)
    {
        std::unique_lock lock(this->mutex_);
        this->requestedPaintIDs_.erase(paintID);
        return;
    }

    api->getCosmeticsByIDs(
        {paintID},
        [this, paintID](const QJsonObject &cosmetics) {
            const auto paints = cosmetics["paints"].toArray();
            for (const auto &paintValue : paints)
            {
                getApp()->getSeventvPaints()->addPaintFromGraphQL(
                    paintValue.toObject());
            }

            std::unique_lock lock(this->mutex_);
            this->requestedPaintIDs_.erase(paintID);
        },
        [this, paintID](const auto &) {
            std::unique_lock lock(this->mutex_);
            this->requestedPaintIDs_.erase(paintID);
        });
}

}  // namespace chatterino
