// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWindow.hpp"

#include <boost/signals2/connection.hpp>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QWidget>

namespace chatterino {

class AccountSwitchWidget;
class KickAccountSwitchWidget;

class AccountSwitchPopup : public BaseWindow
{
    Q_OBJECT

public:
    AccountSwitchPopup(QWidget *parent = nullptr);

    void refresh();
    bool wasHiddenRecently(int thresholdMs) const;

protected:
    void hideEvent(QHideEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

    void themeChangedEvent() override;

private:
    void updateCurrentPage();
    void updateStatusText();

    struct {
        QStackedWidget *accountStack = nullptr;
        AccountSwitchWidget *accountSwitchWidget = nullptr;
        KickAccountSwitchWidget *kickAccountSwitcher = nullptr;
        QLabel *statusLabel = nullptr;
        QPushButton *loginButton = nullptr;
        QPushButton *manageAccountsButton = nullptr;
    } ui_;

    std::vector<boost::signals2::scoped_connection> bSignals_;
    QElapsedTimer lastHideTimer_;
};

}  // namespace chatterino
