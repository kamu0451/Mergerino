// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/HttpServer.hpp"

#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>

namespace chatterino {

class Split;
class SplitContainer;
class Window;

class ObsBrowserDockServer final : public QObject
{
public:
    static constexpr uint16_t PORT = 38479;

    explicit ObsBrowserDockServer(QObject *parent = nullptr);

    static QString dockUrl(const QString &view = QStringLiteral("chat"),
                           int tabIndex = -1);

    /// Build an OBS Browser Source URL for a transparent stream-overlay
    /// rendering of the chat. An empty tabUuid tracks the active tab.
    static QString overlayUrl(const QString &tabUuid = {});

private:
    std::unique_ptr<HttpServer> server_;

    Window *dockWindow() const;
    /// Resolve a `tab` query value - a tab uuid, or a plain integer index
    /// from pre-uuid overlay URLs - to the tab's current notebook index.
    /// Returns -1 (track the active tab) when empty or unresolvable.
    int resolveTabIndex(const QString &tabParam) const;
    SplitContainer *selectedPage(int tabIndex = -1) const;
    Split *resolveSplit(SplitContainer *page, const QString &view) const;

    QByteArray dockPageHtml() const;
    QByteArray overlayPageHtml() const;
    QByteArray dockStateJson(const QString &view, int requestedTabIndex) const;
};

}  // namespace chatterino
