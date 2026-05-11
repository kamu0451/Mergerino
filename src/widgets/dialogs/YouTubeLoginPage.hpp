#pragma once

#include <QWidget>

#include <functional>

namespace chatterino {

class YouTubeLoginPage : public QWidget
{
public:
    YouTubeLoginPage();
    static QWidget *createReviewNotice(QWidget *parent = nullptr);
    static bool startLoginFlow(
        QWidget *parent = nullptr,
        std::function<void()> onAuthenticated = {});
};

}  // namespace chatterino
