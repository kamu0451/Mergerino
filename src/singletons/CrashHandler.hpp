// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QtGlobal>

namespace chatterino {

class Paths;

class CrashHandler
{
    const Paths &paths;

public:
    explicit CrashHandler(const Paths &paths_);

    bool shouldRecover() const
    {
        return this->shouldRecover_;
    }

    /// Sets and saves whether Chatterino should restart on a crash
    void saveShouldRecover(bool value);

private:
    bool shouldRecover_ = false;
};

}  // namespace chatterino
