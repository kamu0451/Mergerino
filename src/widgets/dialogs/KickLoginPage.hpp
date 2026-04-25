#pragma once

#include <QWidget>

#include <functional>

namespace chatterino {

class KickLoginPage : public QWidget
{
public:
    KickLoginPage();
    static void startLoginFlow(
        QWidget *parent = nullptr,
        std::function<void()> onAuthenticated = {});

protected:
    void paintEvent(QPaintEvent *event) override;
};

}  // namespace chatterino
