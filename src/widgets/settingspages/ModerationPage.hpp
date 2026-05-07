// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/settingspages/SettingsPage.hpp"

#include <QTimer>

class QTabWidget;
class QPushButton;

namespace chatterino {

class EditableModelView;

class ModerationPage : public SettingsPage
{
public:
    ModerationPage();

    bool filterElements(const QString &query) override;
    void selectModerationActions();

private:
    void addModerationButtonSettings(QTabWidget *);

    QTimer itemsChangedTimer_;
    QTabWidget *tabWidget_{};
    EditableModelView *loggedChannelsView_{};
    EditableModelView *loggedUsersView_{};
    EditableModelView *moderationActionsView_{};

    std::vector<QLineEdit *> durationInputs_;
    std::vector<QComboBox *> unitInputs_;
};

}  // namespace chatterino
