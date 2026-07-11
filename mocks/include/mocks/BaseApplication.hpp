#pragma once

#include "common/Args.hpp"
#include "mocks/DisabledStreamerMode.hpp"
#include "mocks/EmptyApplication.hpp"
#include "mocks/TwitchUsers.hpp"
#include "providers/bttv/BttvLiveUpdates.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"

#include <QString>
#include <QTemporaryDir>

namespace chatterino::mock {

/**
 * BaseApplication intends to be a mock application with a few more sane defaults, but with less configurability
 */
class BaseApplication : public EmptyApplication
{
public:
    BaseApplication()
        : settings(this->args, this->settingsDir.path(), /*isTest=*/true)
        , updates(this->paths_, this->settings)
        , theme(this->paths_)
        , fonts(this->settings)
    {
        // Paths::cacheDirectory() falls back to this setting before the
        // real per-machine cache directory, so redirect it into a temp dir
        // to keep cache-related tests out of the user's real profile.
        this->settings.cachePath = this->cacheDir.path();
    }

    explicit BaseApplication(const QString &settingsData)
        : EmptyApplication(settingsData)
        , settings(this->args, this->settingsDir.path(), /*isTest=*/true)
        , updates(this->paths_, this->settings)
        , theme(this->paths_)
        , fonts(this->settings)
    {
        this->settings.cachePath = this->cacheDir.path();
    }

    Updates &getUpdates() override
    {
        return this->updates;
    }

    IStreamerMode *getStreamerMode() override
    {
        return &this->streamerMode;
    }

    Theme *getThemes() override
    {
        return &this->theme;
    }

    Fonts *getFonts() override
    {
        return &this->fonts;
    }

    ITwitchUsers *getTwitchUsers() override
    {
        return &this->twitchUsers;
    }

    BttvLiveUpdates *getBttvLiveUpdates() override
    {
        return nullptr;
    }

    SeventvEventAPI *getSeventvEventAPI() override
    {
        return nullptr;
    }

    Args args;
    Settings settings;
    Updates updates;
    DisabledStreamerMode streamerMode;
    Theme theme;
    Fonts fonts;
    TwitchUsers twitchUsers;
    QTemporaryDir cacheDir;
};

}  // namespace chatterino::mock
