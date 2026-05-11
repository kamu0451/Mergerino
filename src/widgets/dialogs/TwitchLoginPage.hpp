#pragma once

#include <QWidget>

#include <functional>

namespace chatterino {

class TwitchLoginPage : public QWidget
{
public:
    TwitchLoginPage();
    static bool startLoginFlow(
        QWidget *parent = nullptr,
        std::function<void()> onAuthenticated = {});
};

}  // namespace chatterino
