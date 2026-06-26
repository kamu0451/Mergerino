// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "singletons/Updates.hpp"
#include "widgets/BaseWindow.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QNetworkAccessManager>
#include <QString>

#include <functional>

class QLabel;
class QNetworkReply;
class QPushButton;

namespace chatterino {

class Label;

class StreamDatabaseUpdateDialog : public BaseWindow
{
public:
    explicit StreamDatabaseUpdateDialog(
        QWidget *parent = nullptr,
        std::function<void()> onContinueToPatchNotes = {});

private:
    void addStreamDatabaseTab();
    void requestStreamDatabaseLogo();
    void handleStreamDatabaseLogoFinished(QNetworkReply *reply);

    QLabel *streamDatabaseLogo_ = nullptr;
    QPushButton *addTabButton_ = nullptr;
    QNetworkAccessManager network_;
    QNetworkReply *pendingLogoReply_ = nullptr;
    std::function<void()> onContinueToPatchNotes_;
    bool addedStreamDatabaseTab_ = false;
};

class PostUpdateDialog : public BaseWindow
{
public:
    explicit PostUpdateDialog(const QString &version, QWidget *parent = nullptr);
};

/// The UpdateDialog is what's shown to the user after they clicked the update indicator in the tab bar/title bar.
///
/// For Nightly builds, we change the "Install" text to "Yes" to better match the question posed by the available update.
class UpdateDialog : public BaseWindow
{
public:
    UpdateDialog();

    /// The user chose to dismiss this update.
    pajlada::Signals::NoArgSignal dismissed;

private:
    void updateStatusChanged(Updates::Status status);

    struct {
        Label *label = nullptr;
        QPushButton *installButton = nullptr;
    } ui_;

    pajlada::Signals::SignalHolder connections_;
};

}  // namespace chatterino
