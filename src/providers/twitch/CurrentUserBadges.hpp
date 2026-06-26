// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QHash>
#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>

#include <pajlada/signals/signal.hpp>

#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>

namespace chatterino::twitch {

struct CurrentUserBadgeIdentity {
    QString id;
    QString setID;
    QString versionID;
    QString title;
    QString description;
    QString imageUrl;
    QString channelLogin;
    QString channelDisplayName;
    bool selected = false;
    bool displayed = false;
    bool available = false;
};

inline bool operator==(const CurrentUserBadgeIdentity &a,
                       const CurrentUserBadgeIdentity &b)
{
    return a.id == b.id && a.setID == b.setID &&
           a.versionID == b.versionID && a.title == b.title &&
           a.description == b.description && a.imageUrl == b.imageUrl &&
           a.channelLogin == b.channelLogin &&
           a.channelDisplayName == b.channelDisplayName &&
           a.selected == b.selected && a.displayed == b.displayed &&
           a.available == b.available;
}

namespace detail {

inline std::mutex currentUserBadgesMutex;
inline QMap<QString, QString> currentUserBadges;
inline QHash<QString, QMap<QString, QString>> currentUserBadgesByChannel;

inline std::mutex currentUserOwnedBadgesMutex;
inline QVector<CurrentUserBadgeIdentity> currentUserOwnedBadges;
inline QSet<QString> currentUserOwnedBadgeKeys;
inline QHash<QString, CurrentUserBadgeIdentity> currentUserOwnedBadgeByKey;
inline QHash<QString, QVector<CurrentUserBadgeIdentity>>
    currentUserOwnedBadgesByChannel;
inline QHash<QString, QHash<QString, CurrentUserBadgeIdentity>>
    currentUserOwnedBadgeByKeyByChannel;
inline QSet<QString> currentUserOwnedBadgesLoadedChannels;
inline bool currentUserOwnedBadgesLoaded = false;
inline QString currentUserSelectedGlobalBadgeKey;
inline QHash<QString, QString> currentUserSelectedChannelBadgeKeys;
inline QHash<QString, bool> currentUserUseCustomChannelBadge;
inline QHash<QString, bool> currentUserHideBadgeFlair;

inline pajlada::Signals::NoArgSignal streamDatabaseBadgesChanged;

}  // namespace detail

inline pajlada::Signals::NoArgSignal &streamDatabaseBadgeOwnershipChanged()
{
    return detail::streamDatabaseBadgesChanged;
}

inline QString streamDatabaseBadgeKey(const QString &setID,
                                      const QString &versionID)
{
    const auto normalizedSet = setID.trimmed().toCaseFolded();
    const auto normalizedVersion = versionID.trimmed().toCaseFolded();
    if (normalizedSet.isEmpty() || normalizedVersion.isEmpty())
    {
        return {};
    }

    return normalizedSet + QLatin1Char('/') + normalizedVersion;
}

inline QString badgeIdentityKey(const CurrentUserBadgeIdentity &badge)
{
    return streamDatabaseBadgeKey(badge.setID, badge.versionID);
}

inline QString normalizedBadgeChannelLogin(QString channelLogin)
{
    channelLogin = channelLogin.trimmed().toCaseFolded();
    channelLogin.remove(QLatin1Char('#'));
    return channelLogin;
}

inline bool currentUserUsesCustomChannelBadge(const QString &channelLogin)
{
    const auto normalizedChannel = normalizedBadgeChannelLogin(channelLogin);
    if (normalizedChannel.isEmpty())
    {
        return true;
    }

    std::lock_guard lock(detail::currentUserOwnedBadgesMutex);
    return detail::currentUserUseCustomChannelBadge.value(normalizedChannel,
                                                          true);
}

inline void setCurrentUserUsesCustomChannelBadge(const QString &channelLogin,
                                                 bool enabled)
{
    const auto normalizedChannel = normalizedBadgeChannelLogin(channelLogin);
    if (normalizedChannel.isEmpty())
    {
        return;
    }

    bool changed = false;
    {
        std::lock_guard lock(detail::currentUserOwnedBadgesMutex);
        changed =
            detail::currentUserUseCustomChannelBadge.value(normalizedChannel,
                                                           true) != enabled;
        detail::currentUserUseCustomChannelBadge.insert(normalizedChannel,
                                                        enabled);
    }

    if (changed)
    {
        detail::streamDatabaseBadgesChanged.invoke();
    }
}

inline bool currentUserHidesBadgeFlair(const QString &channelLogin)
{
    const auto normalizedChannel = normalizedBadgeChannelLogin(channelLogin);
    if (normalizedChannel.isEmpty())
    {
        return false;
    }

    std::lock_guard lock(detail::currentUserOwnedBadgesMutex);
    return detail::currentUserHideBadgeFlair.value(normalizedChannel, false);
}

inline void setCurrentUserHidesBadgeFlair(const QString &channelLogin,
                                          bool hidden)
{
    const auto normalizedChannel = normalizedBadgeChannelLogin(channelLogin);
    if (normalizedChannel.isEmpty())
    {
        return;
    }

    bool changed = false;
    {
        std::lock_guard lock(detail::currentUserOwnedBadgesMutex);
        changed =
            detail::currentUserHideBadgeFlair.value(normalizedChannel, false) !=
            hidden;
        detail::currentUserHideBadgeFlair.insert(normalizedChannel, hidden);
    }

    if (changed)
    {
        detail::streamDatabaseBadgesChanged.invoke();
    }
}

inline QVector<CurrentUserBadgeIdentity> currentUserOwnedBadges()
{
    std::lock_guard lock(detail::currentUserOwnedBadgesMutex);
    return detail::currentUserOwnedBadges;
}

inline QVector<CurrentUserBadgeIdentity> currentUserOwnedBadgesForChannel(
    const QString &channelLogin)
{
    const auto normalizedChannel = normalizedBadgeChannelLogin(channelLogin);
    std::lock_guard lock(detail::currentUserOwnedBadgesMutex);
    if (normalizedChannel.isEmpty())
    {
        return detail::currentUserOwnedBadges;
    }

    return detail::currentUserOwnedBadgesByChannel.value(normalizedChannel);
}

inline QSet<QString> currentUserOwnedBadgeKeys()
{
    std::lock_guard lock(detail::currentUserOwnedBadgesMutex);
    return detail::currentUserOwnedBadgeKeys;
}

inline bool hasLoadedCurrentUserOwnedBadges()
{
    std::lock_guard lock(detail::currentUserOwnedBadgesMutex);
    return detail::currentUserOwnedBadgesLoaded;
}

inline bool hasLoadedCurrentUserOwnedBadgesForChannel(
    const QString &channelLogin)
{
    const auto normalizedChannel = normalizedBadgeChannelLogin(channelLogin);
    std::lock_guard lock(detail::currentUserOwnedBadgesMutex);
    if (normalizedChannel.isEmpty())
    {
        return detail::currentUserOwnedBadgesLoaded;
    }

    return detail::currentUserOwnedBadgesLoadedChannels.contains(
        normalizedChannel);
}

inline bool isLeadModeratorBadgeIdentity(
    const CurrentUserBadgeIdentity &badge)
{
    const auto set = badge.setID.trimmed().toCaseFolded();
    if (set == QStringLiteral("lead_moderator") ||
        set == QStringLiteral("lead-moderator"))
    {
        return true;
    }

    return badge.title.toCaseFolded().contains(
               QStringLiteral("lead moderator")) ||
           badge.description.toCaseFolded().contains(
               QStringLiteral("lead moderator"));
}

inline void updateCurrentUserOwnedBadgesForChannel(
    const QString &channelLogin, const QVector<CurrentUserBadgeIdentity> &badges);

inline void updateCurrentUserOwnedBadges(
    const QVector<CurrentUserBadgeIdentity> &badges)
{
    updateCurrentUserOwnedBadgesForChannel({}, badges);
}

inline void updateCurrentUserOwnedBadgesForChannel(
    const QString &channelLogin, const QVector<CurrentUserBadgeIdentity> &badges)
{
    QSet<QString> keys;
    QHash<QString, CurrentUserBadgeIdentity> badgesByKey;
    QString selectedKey;
    bool selectedKeyIsChannelBadge = false;
    keys.reserve(badges.size());
    badgesByKey.reserve(badges.size());
    for (const auto &badge : badges)
    {
        const auto key = badgeIdentityKey(badge);
        if (!key.isEmpty())
        {
            keys.insert(key);
            badgesByKey.insert(key, badge);
            if (badge.selected)
            {
                selectedKey = key;
                selectedKeyIsChannelBadge =
                    !normalizedBadgeChannelLogin(badge.channelLogin).isEmpty();
            }
        }
    }

    const auto normalizedChannel = normalizedBadgeChannelLogin(channelLogin);
    bool changed = false;
    {
        std::lock_guard lock(detail::currentUserOwnedBadgesMutex);
        changed = !detail::currentUserOwnedBadgesLoaded ||
                  detail::currentUserOwnedBadges != badges ||
                  detail::currentUserOwnedBadgeKeys != keys ||
                  detail::currentUserOwnedBadgeByKey != badgesByKey;
        detail::currentUserOwnedBadges = badges;
        detail::currentUserOwnedBadgeKeys = keys;
        detail::currentUserOwnedBadgeByKey = badgesByKey;
        detail::currentUserOwnedBadgesLoaded = true;
        if (!normalizedChannel.isEmpty())
        {
            changed =
                changed ||
                detail::currentUserOwnedBadgesByChannel.value(
                    normalizedChannel) != badges ||
                detail::currentUserOwnedBadgeByKeyByChannel.value(
                    normalizedChannel) != badgesByKey ||
                !detail::currentUserOwnedBadgesLoadedChannels.contains(
                    normalizedChannel);
            detail::currentUserOwnedBadgesByChannel.insert(normalizedChannel,
                                                           badges);
            detail::currentUserOwnedBadgeByKeyByChannel.insert(
                normalizedChannel, badgesByKey);
            detail::currentUserOwnedBadgesLoadedChannels.insert(
                normalizedChannel);
        }

        if (!selectedKey.isEmpty())
        {
            if (!normalizedChannel.isEmpty() && selectedKeyIsChannelBadge)
            {
                changed = changed ||
                          detail::currentUserSelectedChannelBadgeKeys.value(
                              normalizedChannel) != selectedKey;
                detail::currentUserSelectedChannelBadgeKeys.insert(
                    normalizedChannel, selectedKey);
                changed =
                    changed ||
                    !detail::currentUserUseCustomChannelBadge.value(
                        normalizedChannel, true);
                detail::currentUserUseCustomChannelBadge.insert(
                    normalizedChannel, true);
            }
            else if (!selectedKeyIsChannelBadge)
            {
                changed = changed ||
                          detail::currentUserSelectedGlobalBadgeKey !=
                              selectedKey;
                detail::currentUserSelectedGlobalBadgeKey = selectedKey;
            }
        }
    }

    if (changed)
    {
        detail::streamDatabaseBadgesChanged.invoke();
    }
}

inline void clearCurrentUserOwnedBadges()
{
    bool changed = false;
    {
        std::lock_guard lock(detail::currentUserBadgesMutex);
        changed = changed || !detail::currentUserBadges.isEmpty() ||
                  !detail::currentUserBadgesByChannel.isEmpty();
        detail::currentUserBadges.clear();
        detail::currentUserBadgesByChannel.clear();
    }

    {
        std::lock_guard lock(detail::currentUserOwnedBadgesMutex);
        changed = changed || detail::currentUserOwnedBadgesLoaded ||
                  !detail::currentUserOwnedBadges.isEmpty() ||
                  !detail::currentUserOwnedBadgeKeys.isEmpty() ||
                  !detail::currentUserOwnedBadgeByKey.isEmpty() ||
                  !detail::currentUserOwnedBadgesByChannel.isEmpty() ||
                  !detail::currentUserOwnedBadgeByKeyByChannel.isEmpty() ||
                  !detail::currentUserOwnedBadgesLoadedChannels.isEmpty() ||
                  !detail::currentUserSelectedGlobalBadgeKey.isEmpty() ||
                  !detail::currentUserSelectedChannelBadgeKeys.isEmpty() ||
                  !detail::currentUserUseCustomChannelBadge.isEmpty() ||
                  !detail::currentUserHideBadgeFlair.isEmpty();
        detail::currentUserOwnedBadges.clear();
        detail::currentUserOwnedBadgeKeys.clear();
        detail::currentUserOwnedBadgeByKey.clear();
        detail::currentUserOwnedBadgesByChannel.clear();
        detail::currentUserOwnedBadgeByKeyByChannel.clear();
        detail::currentUserOwnedBadgesLoadedChannels.clear();
        detail::currentUserOwnedBadgesLoaded = false;
        detail::currentUserSelectedGlobalBadgeKey.clear();
        detail::currentUserSelectedChannelBadgeKeys.clear();
        detail::currentUserUseCustomChannelBadge.clear();
        detail::currentUserHideBadgeFlair.clear();
    }

    if (changed)
    {
        detail::streamDatabaseBadgesChanged.invoke();
    }
}

inline void setCurrentUserAppliedBadge(const QString &channelLogin,
                                       const CurrentUserBadgeIdentity &badge,
                                       bool channelSpecific)
{
    const auto key = badgeIdentityKey(badge);
    if (key.isEmpty())
    {
        return;
    }

    const auto normalizedChannel = normalizedBadgeChannelLogin(channelLogin);
    bool changed = false;
    {
        std::lock_guard lock(detail::currentUserOwnedBadgesMutex);
        detail::currentUserOwnedBadgesLoaded = true;
        detail::currentUserOwnedBadgeKeys.insert(key);
        detail::currentUserOwnedBadgeByKey.insert(key, badge);
        if (!normalizedChannel.isEmpty())
        {
            auto &channelBadges =
                detail::currentUserOwnedBadgesByChannel[normalizedChannel];
            auto &channelBadgeByKey =
                detail::currentUserOwnedBadgeByKeyByChannel[normalizedChannel];
            detail::currentUserOwnedBadgesLoadedChannels.insert(
                normalizedChannel);
            channelBadgeByKey.insert(key, badge);

            bool foundChannelBadge = false;
            for (auto &existing : channelBadges)
            {
                if (badgeIdentityKey(existing) == key)
                {
                    changed = changed || existing != badge;
                    existing = badge;
                    foundChannelBadge = true;
                    break;
                }
            }
            if (!foundChannelBadge)
            {
                channelBadges.push_back(badge);
                changed = true;
            }
        }

        bool found = false;
        for (auto &existing : detail::currentUserOwnedBadges)
        {
            if (badgeIdentityKey(existing) == key)
            {
                changed = changed || existing != badge;
                existing = badge;
                found = true;
                break;
            }
        }
        if (!found)
        {
            detail::currentUserOwnedBadges.push_back(badge);
            changed = true;
        }

        if (channelSpecific && !normalizedChannel.isEmpty())
        {
            changed = changed ||
                      detail::currentUserSelectedChannelBadgeKeys.value(
                          normalizedChannel) != key;
            detail::currentUserSelectedChannelBadgeKeys.insert(normalizedChannel,
                                                               key);
            changed =
                changed ||
                !detail::currentUserUseCustomChannelBadge.value(
                    normalizedChannel, true);
            detail::currentUserUseCustomChannelBadge.insert(normalizedChannel,
                                                            true);
        }
        else
        {
            changed = changed ||
                      detail::currentUserSelectedGlobalBadgeKey != key;
            detail::currentUserSelectedGlobalBadgeKey = key;
        }
    }

    if (changed)
    {
        detail::streamDatabaseBadgesChanged.invoke();
    }
}

inline QString currentUserAppliedBadgeKey(const QString &channelLogin)
{
    const auto normalizedChannel = normalizedBadgeChannelLogin(channelLogin);
    std::lock_guard lock(detail::currentUserOwnedBadgesMutex);
    if (!normalizedChannel.isEmpty() &&
        detail::currentUserUseCustomChannelBadge.value(normalizedChannel, true))
    {
        const auto it =
            detail::currentUserSelectedChannelBadgeKeys.constFind(
                normalizedChannel);
        if (it != detail::currentUserSelectedChannelBadgeKeys.cend() &&
            !it.value().isEmpty())
        {
            return it.value();
        }
    }

    return detail::currentUserSelectedGlobalBadgeKey;
}

inline std::optional<CurrentUserBadgeIdentity> currentUserAppliedBadge(
    const QString &channelLogin)
{
    const auto key = currentUserAppliedBadgeKey(channelLogin);
    if (key.isEmpty())
    {
        return std::nullopt;
    }

    const auto normalizedChannel = normalizedBadgeChannelLogin(channelLogin);
    std::lock_guard lock(detail::currentUserOwnedBadgesMutex);
    if (!normalizedChannel.isEmpty())
    {
        const auto channelBadges =
            detail::currentUserOwnedBadgeByKeyByChannel.constFind(
                normalizedChannel);
        if (channelBadges !=
            detail::currentUserOwnedBadgeByKeyByChannel.cend())
        {
            const auto &badges = channelBadges.value();
            const auto channelBadge = badges.constFind(key);
            if (channelBadge != badges.cend())
            {
                return channelBadge.value();
            }
        }
    }

    const auto it = detail::currentUserOwnedBadgeByKey.constFind(key);
    if (it == detail::currentUserOwnedBadgeByKey.cend())
    {
        return std::nullopt;
    }

    return it.value();
}

inline QVector<CurrentUserBadgeIdentity> currentUserIdentityBadges(
    const QString &channelLogin)
{
    const auto normalizedChannel = normalizedBadgeChannelLogin(channelLogin);
    QMap<QString, QString> ircBadges;
    {
        std::lock_guard lock(detail::currentUserBadgesMutex);
        if (!normalizedChannel.isEmpty() &&
            detail::currentUserBadgesByChannel.contains(normalizedChannel))
        {
            ircBadges =
                detail::currentUserBadgesByChannel.value(normalizedChannel);
        }
        else if (normalizedChannel.isEmpty())
        {
            ircBadges = detail::currentUserBadges;
        }
    }

    QVector<CurrentUserBadgeIdentity> ownedBadges;
    QHash<QString, CurrentUserBadgeIdentity> ownedBadgeByKey;
    QString selectedGlobalKey;
    QString selectedChannelKey;
    bool useCustomChannelBadge = true;
    bool hasChannelOwnedBadges = false;
    {
        std::lock_guard lock(detail::currentUserOwnedBadgesMutex);
        ownedBadges = detail::currentUserOwnedBadges;
        ownedBadgeByKey = detail::currentUserOwnedBadgeByKey;
        selectedGlobalKey = detail::currentUserSelectedGlobalBadgeKey;
        if (!normalizedChannel.isEmpty())
        {
            const auto channelBadges =
                detail::currentUserOwnedBadgesByChannel.constFind(
                    normalizedChannel);
            if (channelBadges !=
                detail::currentUserOwnedBadgesByChannel.cend())
            {
                ownedBadges = channelBadges.value();
                hasChannelOwnedBadges = true;
            }

            const auto channelBadgeByKey =
                detail::currentUserOwnedBadgeByKeyByChannel.constFind(
                    normalizedChannel);
            if (channelBadgeByKey !=
                detail::currentUserOwnedBadgeByKeyByChannel.cend())
            {
                ownedBadgeByKey = channelBadgeByKey.value();
            }

            selectedChannelKey =
                detail::currentUserSelectedChannelBadgeKeys.value(
                    normalizedChannel);
            useCustomChannelBadge =
                detail::currentUserUseCustomChannelBadge.value(
                    normalizedChannel, true);

            if (!hasChannelOwnedBadges)
            {
                ownedBadges.clear();
                ownedBadgeByKey.clear();
                selectedChannelKey.clear();
            }
        }
    }

    QVector<CurrentUserBadgeIdentity> result;
    QSet<QString> seen;
    auto appendBadge = [&](CurrentUserBadgeIdentity badge) {
        const auto key = badgeIdentityKey(badge);
        if (key.isEmpty() || seen.contains(key))
        {
            return;
        }

        seen.insert(key);
        result.push_back(std::move(badge));
    };

    auto badgeForSetAndVersion =
        [&](const QString &setID,
            const QString &versionID) -> CurrentUserBadgeIdentity {
        const auto key = streamDatabaseBadgeKey(setID, versionID);
        if (!key.isEmpty())
        {
            const auto exact = ownedBadgeByKey.constFind(key);
            if (exact != ownedBadgeByKey.cend())
            {
                return exact.value();
            }
        }

        const auto normalizedSet = setID.trimmed().toCaseFolded();
        if (normalizedSet == QStringLiteral("lead_moderator") ||
            normalizedSet == QStringLiteral("lead-moderator"))
        {
            for (const auto &badge : ownedBadges)
            {
                if (isLeadModeratorBadgeIdentity(badge))
                {
                    return badge;
                }
            }
        }

        for (const auto &badge : ownedBadges)
        {
            if (badge.setID.compare(setID, Qt::CaseInsensitive) == 0)
            {
                return badge;
            }
        }

        CurrentUserBadgeIdentity fallback;
        fallback.setID = setID;
        fallback.versionID = versionID;
        fallback.displayed = true;
        return fallback;
    };

    auto appendIrcBadge = [&](const QString &setID) {
        const auto it = ircBadges.constFind(setID);
        if (it == ircBadges.cend())
        {
            return false;
        }

        appendBadge(badgeForSetAndVersion(setID, it.value()));
        return true;
    };

    auto hasLeadModeratorBadge = [&] {
        return std::any_of(ownedBadges.begin(), ownedBadges.end(),
                           [](const CurrentUserBadgeIdentity &badge) {
                               return isLeadModeratorBadgeIdentity(badge);
                           });
    };
    auto isRoleBadgeIdentity = [](const CurrentUserBadgeIdentity &badge) {
        const auto set = badge.setID.trimmed().toCaseFolded();
        return set == QStringLiteral("broadcaster") ||
               set == QStringLiteral("moderator") ||
               set == QStringLiteral("lead_moderator") ||
               set == QStringLiteral("lead-moderator") ||
               set == QStringLiteral("vip") || set == QStringLiteral("staff") ||
               set == QStringLiteral("admin") ||
               set == QStringLiteral("global_mod") ||
               isLeadModeratorBadgeIdentity(badge);
    };
    auto isSubscriberBadgeIdentity = [](const CurrentUserBadgeIdentity &badge) {
        const auto set = badge.setID.trimmed().toCaseFolded();
        return set == QStringLiteral("subscriber") ||
               set == QStringLiteral("founder");
    };

    appendIrcBadge(QStringLiteral("broadcaster"));
    bool appendedLeadModerator = appendIrcBadge(QStringLiteral("lead_moderator"));
    if (!appendedLeadModerator)
    {
        appendedLeadModerator = appendIrcBadge(QStringLiteral("lead-moderator"));
    }
    if (!appendedLeadModerator && ircBadges.contains(QStringLiteral("moderator")) &&
        hasLeadModeratorBadge())
    {
        appendBadge(badgeForSetAndVersion(QStringLiteral("lead_moderator"),
                                           ircBadges.value(
                                               QStringLiteral("moderator"))));
        appendedLeadModerator = true;
    }
    if (!appendedLeadModerator)
    {
        appendIrcBadge(QStringLiteral("moderator"));
    }
    appendIrcBadge(QStringLiteral("vip"));
    appendIrcBadge(QStringLiteral("staff"));
    appendIrcBadge(QStringLiteral("admin"));
    appendIrcBadge(QStringLiteral("global_mod"));
    for (const auto &badge : ownedBadges)
    {
        if (badge.displayed && isRoleBadgeIdentity(badge))
        {
            appendBadge(badge);
        }
    }

    bool appendedSubscriber = appendIrcBadge(QStringLiteral("founder"));
    if (!appendedSubscriber)
    {
        appendedSubscriber = appendIrcBadge(QStringLiteral("subscriber"));
    }
    if (!appendedSubscriber)
    {
        for (const auto &badge : ownedBadges)
        {
            if (badge.displayed && isSubscriberBadgeIdentity(badge))
            {
                appendBadge(badge);
                break;
            }
        }
    }

    QString appliedKey;
    if (!normalizedChannel.isEmpty() && useCustomChannelBadge &&
        !selectedChannelKey.isEmpty())
    {
        appliedKey = selectedChannelKey;
    }
    else
    {
        appliedKey = selectedGlobalKey;
    }
    if (!appliedKey.isEmpty())
    {
        const auto applied = ownedBadgeByKey.constFind(appliedKey);
        if (applied != ownedBadgeByKey.cend())
        {
            appendBadge(applied.value());
        }
        else
        {
            std::lock_guard lock(detail::currentUserOwnedBadgesMutex);
            const auto globalApplied =
                detail::currentUserOwnedBadgeByKey.constFind(appliedKey);
            if (globalApplied != detail::currentUserOwnedBadgeByKey.cend())
            {
                appendBadge(globalApplied.value());
            }
        }
    }

    return result;
}

inline void updateCurrentUserBadgesForChannel(
    const QString &channelLogin, const QMap<QString, QString> &badges);

inline void updateCurrentUserBadges(const QMap<QString, QString> &badges)
{
    updateCurrentUserBadgesForChannel({}, badges);
}

inline void updateCurrentUserBadgesForChannel(
    const QString &channelLogin, const QMap<QString, QString> &badges)
{
    bool changed = false;
    const auto normalizedChannel = normalizedBadgeChannelLogin(channelLogin);
    {
        std::lock_guard lock(detail::currentUserBadgesMutex);
        if (normalizedChannel.isEmpty())
        {
            changed = detail::currentUserBadges != badges;
            detail::currentUserBadges = badges;
        }
        else
        {
            changed = detail::currentUserBadgesByChannel.value(
                          normalizedChannel) != badges;
            detail::currentUserBadgesByChannel.insert(normalizedChannel, badges);
        }
    }

    if (changed)
    {
        detail::streamDatabaseBadgesChanged.invoke();
    }
}

inline bool currentUserHasFetchedBadge(const QString &setID,
                                       const QString &versionID = {})
{
    const auto normalizedSet = setID.trimmed().toCaseFolded();
    const auto normalizedVersion = versionID.trimmed().toCaseFolded();
    if (normalizedSet.isEmpty())
    {
        return false;
    }

    std::lock_guard lock(detail::currentUserOwnedBadgesMutex);
    if (!detail::currentUserOwnedBadgesLoaded)
    {
        return false;
    }

    if (!normalizedVersion.isEmpty())
    {
        return detail::currentUserOwnedBadgeKeys.contains(
            streamDatabaseBadgeKey(normalizedSet, normalizedVersion));
    }

    for (const auto &badge : detail::currentUserOwnedBadges)
    {
        if (badge.setID.compare(normalizedSet, Qt::CaseInsensitive) == 0)
        {
            return true;
        }
    }
    return false;
}

inline bool currentUserHasBadge(const QString &setID,
                                const QString &versionID = {})
{
    if (setID.isEmpty())
    {
        return false;
    }

    {
        std::lock_guard lock(detail::currentUserOwnedBadgesMutex);
        if (detail::currentUserOwnedBadgesLoaded)
        {
            const auto normalizedVersion = versionID.trimmed().toCaseFolded();
            if (!normalizedVersion.isEmpty())
            {
                return detail::currentUserOwnedBadgeKeys.contains(
                    streamDatabaseBadgeKey(setID, versionID));
            }

            for (const auto &badge : detail::currentUserOwnedBadges)
            {
                if (badge.setID.compare(setID, Qt::CaseInsensitive) == 0)
                {
                    return true;
                }
            }
            return false;
        }
    }

    std::lock_guard lock(detail::currentUserBadgesMutex);
    const auto it = detail::currentUserBadges.constFind(setID);
    return it != detail::currentUserBadges.cend() &&
           (versionID.isEmpty() || it.value() == versionID);
}

}  // namespace chatterino::twitch
