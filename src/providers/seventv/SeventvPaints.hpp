#pragma once

#include "providers/seventv/paints/Paint.hpp"

#include <QJsonArray>
#include <QString>

#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace chatterino {

namespace seventv::eventapi {
struct TwitchUser;
struct KickUser;
using User = std::variant<TwitchUser, KickUser>;
}  // namespace seventv::eventapi

class SeventvPaints
{
public:
    SeventvPaints();

    void addPaint(const QJsonObject &paintJson);
    void addPaintFromGraphQL(const QJsonObject &paintJson);
    void removePaint(const QString &paintID);
    void assignPaintToUsers(const QString &paintID,
                            std::span<const seventv::eventapi::User> users);
    void clearPaintFromUsers(const QString &paintID,
                             std::span<const seventv::eventapi::User> users);

    std::shared_ptr<Paint> getPaint(const QString &userName, bool kick) const;

private:
    void ensurePaintLoaded(const QString &paintID);

    // Mutex for both `paintMap_` and `knownPaints_`
    mutable std::shared_mutex mutex_;

    // user-name => paint
    std::unordered_map<QString, std::shared_ptr<Paint>> kickPaintMap_;
    // user-name => paint
    std::unordered_map<QString, std::shared_ptr<Paint>> twitchPaintMap_;
    // paint-id => paint
    std::unordered_map<QString, std::shared_ptr<Paint>> knownPaints_;
    // user-name => pending paint-id
    std::unordered_map<QString, QString> pendingKickPaintAssignments_;
    // user-name => pending paint-id
    std::unordered_map<QString, QString> pendingTwitchPaintAssignments_;
    std::unordered_set<QString> requestedPaintIDs_;
};

}  // namespace chatterino
