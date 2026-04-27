// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/ObsBrowserDockServer.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "messages/Message.hpp"
#include "messages/layouts/MessageLayout.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "singletons/WindowManager.hpp"
#include "singletons/Theme.hpp"
#include "util/HttpServer.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QSet>
#include <QStringBuilder>
#include <QUrl>
#include <QUrlQuery>

namespace {

using namespace chatterino;

constexpr int OBS_DOCK_MESSAGE_LIMIT = 120;

QString normalizedDockView(const QString &view)
{
    if (view.compare(QStringLiteral("activity"), Qt::CaseInsensitive) == 0)
    {
        return QStringLiteral("activity");
    }

    return QStringLiteral("chat");
}

QString platformName(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QStringLiteral("kick");
        case MessagePlatform::YouTube:
            return QStringLiteral("youtube");
        case MessagePlatform::TikTok:
            return QStringLiteral("tiktok");
        case MessagePlatform::AnyOrTwitch:
        default:
            return QStringLiteral("twitch");
    }
}

QString platformIconPath(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QStringLiteral(":/platforms/kick.svg");
        case MessagePlatform::YouTube:
            return QStringLiteral(":/platforms/youtube.svg");
        case MessagePlatform::TikTok:
            return QStringLiteral(":/platforms/tiktok.svg");
        case MessagePlatform::AnyOrTwitch:
        default:
            return QStringLiteral(":/platforms/twitch.svg");
    }
}

QString resourceDataUrl(const QString &resourcePath)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }

    return QStringLiteral("data:image/svg+xml;base64,%1")
        .arg(QString::fromLatin1(file.readAll().toBase64()));
}

QString platformIconDataUrl(MessagePlatform platform)
{
    return resourceDataUrl(platformIconPath(platform));
}

QColor defaultPlatformAccent(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QColor(83, 252, 24);
        case MessagePlatform::YouTube:
            return QColor(255, 48, 64);
        case MessagePlatform::TikTok:
            return QColor(37, 244, 238);
        case MessagePlatform::AnyOrTwitch:
        default:
            return QColor(145, 70, 255);
    }
}

QColor softenedDockOverlay(QColor color)
{
    if (!color.isValid())
    {
        return {};
    }

    if (color.alpha() >= 250)
    {
        color.setAlpha(92);
    }
    else if (color.alpha() < 72)
    {
        color.setAlpha(72);
    }

    return color;
}

QColor dockEventHighlightColor(const Message &message)
{
    const auto &colors = ColorProvider::instance();

    if (message.highlightColor && message.highlightColor->isValid())
    {
        return softenedDockOverlay(*message.highlightColor);
    }

    if (message.flags.has(MessageFlag::Highlighted))
    {
        return softenedDockOverlay(*colors.color(ColorType::SelfHighlight));
    }

    if (message.flags.has(MessageFlag::ElevatedMessage))
    {
        return softenedDockOverlay(
            *colors.color(ColorType::ElevatedMessageHighlight));
    }

    if (message.flags.has(MessageFlag::FirstMessage) ||
        message.flags.has(MessageFlag::FirstMessageSession))
    {
        return softenedDockOverlay(
            *colors.color(ColorType::FirstMessageHighlight));
    }

    if (message.flags.has(MessageFlag::WatchStreak))
    {
        return softenedDockOverlay(*colors.color(ColorType::WatchStreak));
    }

    if (message.flags.has(MessageFlag::Subscription))
    {
        return softenedDockOverlay(*colors.color(ColorType::Subscription));
    }

    if (message.flags.has(MessageFlag::RedeemedHighlight) ||
        message.flags.has(MessageFlag::RedeemedChannelPointReward))
    {
        return softenedDockOverlay(*colors.color(ColorType::RedeemedHighlight));
    }

    if (message.flags.has(MessageFlag::Whisper) ||
        message.flags.has(MessageFlag::HighlightedWhisper))
    {
        return softenedDockOverlay(*colors.color(ColorType::Whisper));
    }

    if (message.flags.has(MessageFlag::AutoMod) ||
        message.flags.has(MessageFlag::AutoModOffendingMessageHeader) ||
        message.flags.has(MessageFlag::AutoModOffendingMessage))
    {
        return softenedDockOverlay(*colors.color(ColorType::AutomodHighlight));
    }

    if (message.flags.has(MessageFlag::CheerMessage) ||
        message.flags.has(MessageFlag::ModerationAction))
    {
        auto color =
            message.platformAccentColor.value_or(defaultPlatformAccent(message.platform));
        color.setAlpha(102);
        return color;
    }

    return {};
}

QString cssColor(const QColor &color)
{
    if (!color.isValid())
    {
        return QStringLiteral("rgba(0, 0, 0, 0)");
    }

    if (color.alpha() == 255)
    {
        return color.name(QColor::HexRgb);
    }

    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(QString::number(color.alphaF(), 'f', 3));
}

int normalizedDockTabIndex(int requestedTab, int pageCount, int fallbackIndex)
{
    if (requestedTab >= 0 && requestedTab < pageCount)
    {
        return requestedTab;
    }

    if (fallbackIndex >= 0 && fallbackIndex < pageCount)
    {
        return fallbackIndex;
    }

    if (pageCount > 0)
    {
        return 0;
    }

    return -1;
}

QString splitLabel(Split *split)
{
    if (split == nullptr)
    {
        return QStringLiteral("No split selected");
    }

    if (split->isActivityPane())
    {
        return split->activityPaneTitle();
    }

    const auto channel = split->getChannel();
    if (channel && !channel->getLocalizedName().isEmpty())
    {
        return channel->getLocalizedName();
    }
    if (channel && !channel->getDisplayName().isEmpty())
    {
        return channel->getDisplayName();
    }
    if (channel && !channel->getName().isEmpty())
    {
        return channel->getName();
    }

    return QStringLiteral("Selected split");
}

void appendPlatform(QJsonArray &platforms, QSet<QString> &seen,
                    MessagePlatform platform)
{
    const auto name = platformName(platform);
    if (seen.contains(name))
    {
        return;
    }

    seen.insert(name);
    platforms.push_back(name);
}

QJsonArray pagePlatforms(SplitContainer *page)
{
    QJsonArray platforms;
    QSet<QString> seen;

    if (page == nullptr)
    {
        return platforms;
    }

    for (auto *split : page->getSplits())
    {
        if (split == nullptr || split->isActivityPane())
        {
            continue;
        }

        const auto channel = split->getChannel();
        if (!channel)
        {
            continue;
        }

        if (auto *merged = dynamic_cast<MergedChannel *>(channel.get()))
        {
            const auto &config = merged->config();
            if (config.twitchEnabled)
            {
                appendPlatform(platforms, seen, MessagePlatform::AnyOrTwitch);
            }
            if (config.kickEnabled)
            {
                appendPlatform(platforms, seen, MessagePlatform::Kick);
            }
            if (config.youtubeEnabled)
            {
                appendPlatform(platforms, seen, MessagePlatform::YouTube);
            }
            if (config.tiktokEnabled)
            {
                appendPlatform(platforms, seen, MessagePlatform::TikTok);
            }
            continue;
        }

        if (channel->isKickChannel())
        {
            appendPlatform(platforms, seen, MessagePlatform::Kick);
        }
        else if (channel->isTwitchChannel())
        {
            appendPlatform(platforms, seen, MessagePlatform::AnyOrTwitch);
        }
    }

    return platforms;
}

}  // namespace

namespace chatterino {

ObsBrowserDockServer::ObsBrowserDockServer(QObject *parent)
    : QObject(parent)
    , server_(std::make_unique<HttpServer>(ObsBrowserDockServer::PORT, this))
{
    this->server_->setHandler([this](const HttpServer::Request &request) {
        const auto requestUrl =
            QUrl(QStringLiteral("http://127.0.0.1") + request.target);
        const auto path = requestUrl.path();
        const auto query = QUrlQuery(requestUrl);
        const auto view =
            normalizedDockView(query.queryItemValue(QStringLiteral("view")));
        bool hasRequestedTab = false;
        const auto requestedTab = query.queryItemValue(QStringLiteral("tab"))
                                      .toInt(&hasRequestedTab);

        if (request.method.compare(QStringLiteral("GET"),
                                   Qt::CaseInsensitive) != 0)
        {
            return HttpServer::Response{
                .status = 405,
                .body = QByteArrayLiteral("Method Not Allowed"),
            };
        }

        if (path == QStringLiteral("/obs-dock") ||
            path == QStringLiteral("/obs-dock/"))
        {
            return HttpServer::Response{
                .body = this->dockPageHtml(),
                .contentType = QByteArrayLiteral("text/html; charset=utf-8"),
            };
        }

        if (path == QStringLiteral("/obs-dock/state"))
        {
            return HttpServer::Response{
                .body = this->dockStateJson(view,
                                            hasRequestedTab ? requestedTab : -1),
                .contentType =
                    QByteArrayLiteral("application/json; charset=utf-8"),
            };
        }

        return HttpServer::Response{
            .status = 404,
            .body = QByteArrayLiteral("Not Found"),
        };
    });
}

QString ObsBrowserDockServer::dockUrl(const QString &view, int tabIndex)
{
    auto url = QStringLiteral("http://127.0.0.1:%1/obs-dock?view=%2")
                   .arg(ObsBrowserDockServer::PORT)
                   .arg(normalizedDockView(view));

    if (tabIndex >= 0)
    {
        url += QStringLiteral("&tab=%1").arg(tabIndex);
    }

    return url;
}

Window *ObsBrowserDockServer::dockWindow() const
{
    auto *windows = getApp()->getWindows();
    if (windows == nullptr)
    {
        return nullptr;
    }

    return &windows->getMainWindow();
}

SplitContainer *ObsBrowserDockServer::selectedPage(int tabIndex) const
{
    auto *window = this->dockWindow();
    if (window == nullptr)
    {
        return nullptr;
    }

    auto &notebook = window->getNotebook();
    const auto pageCount = notebook.getPageCount();
    const auto fallbackIndex = notebook.getSelectedIndex();
    const auto resolvedTabIndex =
        (tabIndex >= 0 && tabIndex < pageCount) ? tabIndex
        : (fallbackIndex >= 0 && fallbackIndex < pageCount) ? fallbackIndex
        : (pageCount > 0)                                 ? 0
                                                           : -1;

    if (resolvedTabIndex < 0)
    {
        return nullptr;
    }

    return dynamic_cast<SplitContainer *>(notebook.getPageAt(resolvedTabIndex));
}

Split *ObsBrowserDockServer::resolveSplit(SplitContainer *page,
                                          const QString &view) const
{
    if (page == nullptr)
    {
        return nullptr;
    }

    if (view == QStringLiteral("activity"))
    {
        for (auto *split : page->getSplits())
        {
            if (split != nullptr && split->isActivityPane())
            {
                return split;
            }
        }

        return nullptr;
    }

    for (auto *split : page->getSplits())
    {
        if (split != nullptr && !split->isActivityPane())
        {
            return split;
        }
    }

    return nullptr;
}

QByteArray ObsBrowserDockServer::dockPageHtml() const
{
    auto *theme = getTheme();

    const auto muted = [&] {
        auto color = theme->window.text;
        color.setAlpha(150);
        return color;
    }();
    const auto divider = [&] {
        auto color = theme->splits.header.border;
        if (!color.isValid() || color.alpha() == 0)
        {
            color = theme->splits.messageSeperator;
        }
        if (!color.isValid() || color.alpha() == 0)
        {
            color = theme->window.text;
            color.setAlpha(40);
        }
        return color;
    }();
    const auto selectedLine = [&] {
        auto color = theme->tabs.selected.line.regular;
        if (!color.isValid() || color.alpha() == 0)
        {
            color = theme->accent;
        }
        return color;
    }();
    const auto twitchIcon = platformIconDataUrl(MessagePlatform::AnyOrTwitch);
    const auto kickIcon = platformIconDataUrl(MessagePlatform::Kick);
    const auto youtubeIcon = platformIconDataUrl(MessagePlatform::YouTube);
    const auto tiktokIcon = platformIconDataUrl(MessagePlatform::TikTok);
    const QString html = QStringLiteral(R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Mergerino OBS Dock</title>
  <style>
    :root {
      color-scheme: dark;
      --window-bg: %1;
      --split-bg: %2;
      --header-bg: %3;
      --header-focused-bg: %4;
      --header-text: %5;
      --header-focused-text: %6;
      --divider: %7;
      --text: %8;
      --muted: %9;
      --msg-bg: %10;
      --msg-alt-bg: %11;
      --msg-text: %12;
      --msg-system: %13;
      --tab-bg: %14;
      --tab-text: %15;
      --tab-line: %16;
      --tab-selected-bg: %17;
      --tab-selected-text: %18;
      --tab-selected-line: %19;
      --scrollbar-bg: %20;
      --scrollbar-thumb: %21;
      --scrollbar-thumb-selected: %22;
      --twitch: #a970ff;
      --kick: #53fc18;
      --youtube: #ff4b4b;
      --tiktok: #3ed7ff;
      font-family: "Segoe UI", "Helvetica Neue", sans-serif;
      font-size: 12px;
    }

    * {
      box-sizing: border-box;
    }

    html, body {
      margin: 0;
      height: 100%;
      background: var(--window-bg);
      color: var(--text);
    }

    body {
      overflow: hidden;
    }

    .shell {
      height: 100%;
      display: grid;
      grid-template-rows: 31px 32px 32px 30px 1fr;
    }

    .windowbar {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 10px;
      padding: 0 10px;
      background: var(--window-bg);
      border-bottom: 1px solid var(--divider);
      font-size: 12px;
    }

    .windowbar-title {
      font-weight: 600;
      color: var(--text);
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    .windowbar-meta {
      color: var(--muted);
      font-size: 11px;
      white-space: nowrap;
    }

    .windowbar-meta.ready {
      color: var(--muted);
    }

    .windowbar-meta.waiting,
    .windowbar-meta.offline {
      color: var(--msg-system);
    }

    .page-toolbar,
    .tabstrip {
      display: flex;
      align-items: center;
      padding: 0 6px;
      background: var(--window-bg);
      border-bottom: 1px solid var(--divider);
    }

    .page-toolbar {
      gap: 6px;
    }

    .tabstrip {
      gap: 1px;
      overflow-x: auto;
      scrollbar-width: none;
    }

    .tabstrip::-webkit-scrollbar {
      display: none;
    }

    .page-tabstrip {
      flex: 1 1 auto;
      min-width: 0;
      white-space: nowrap;
      overflow: hidden;
    }

    .tab {
      display: inline-flex;
      align-items: center;
      padding: 0 10px;
      height: 100%;
      background: var(--tab-bg);
      color: var(--tab-text);
      border-top: 1px solid var(--divider);
      border-left: 1px solid var(--divider);
      border-right: 1px solid var(--divider);
      position: relative;
      user-select: none;
      font-size: 12px;
      text-decoration: none;
      cursor: pointer;
      transition:
        max-width 75ms cubic-bezier(0.55, 0.055, 0.675, 0.19),
        padding 75ms cubic-bezier(0.55, 0.055, 0.675, 0.19),
        background-color 75ms linear,
        color 75ms linear,
        opacity 75ms cubic-bezier(0.55, 0.055, 0.675, 0.19),
        transform 75ms cubic-bezier(0.55, 0.055, 0.675, 0.19);
    }

    .tab::before {
      content: "";
      position: absolute;
      top: 0;
      left: 0;
      right: 0;
      height: 1px;
      background: var(--tab-line);
    }

    .tab.selected {
      background: var(--tab-selected-bg);
      color: var(--tab-selected-text);
    }

    .tab.selected::before {
      height: 2px;
      background: var(--tab-selected-line);
    }

    .page-tab {
      max-width: 200px;
      min-width: 0;
      flex: 0 0 auto;
      overflow: hidden;
    }

    .page-tab-label {
      display: block;
      min-width: 0;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
      transition:
        opacity 75ms cubic-bezier(0.55, 0.055, 0.675, 0.19),
        transform 75ms cubic-bezier(0.55, 0.055, 0.675, 0.19),
        max-width 75ms cubic-bezier(0.55, 0.055, 0.675, 0.19);
    }

    .page-tab-slat {
      display: none;
      width: 2px;
      height: 15px;
      border-radius: 1px;
      background: var(--tab-line);
      flex: 0 0 auto;
    }

    .page-tabstrip.collapsed .page-tab:not(.selected) {
      max-width: 14px;
      padding-left: 0;
      padding-right: 0;
      justify-content: center;
    }

    .page-tabstrip.collapsed .page-tab:not(.selected) .page-tab-label {
      opacity: 0;
      transform: translateX(-6px);
      max-width: 0;
    }

    .page-tabstrip.collapsed .page-tab:not(.selected) .page-tab-slat {
      display: block;
    }

    .tabs-toggle {
      flex: 0 0 auto;
      border: 1px solid var(--divider);
      border-top: 1px solid var(--divider);
      background: var(--tab-bg);
      color: var(--tab-text);
      height: 22px;
      padding: 0 9px;
      font: inherit;
      cursor: pointer;
      position: relative;
    }

    .tabs-toggle::before {
      content: "";
      position: absolute;
      top: 0;
      left: 0;
      right: 0;
      height: 1px;
      background: var(--tab-line);
    }

    .tabs-toggle:hover {
      background: var(--tab-selected-bg);
      color: var(--tab-selected-text);
    }

    .tab:visited,
    .tab:hover,
    .tab:active {
      color: inherit;
    }

    .header {
      display: grid;
      grid-template-columns: 1fr auto;
      align-items: center;
      gap: 8px;
      padding: 0 10px;
      background: var(--header-focused-bg);
      color: var(--header-focused-text);
      border-bottom: 1px solid var(--divider);
    }

    .header-main {
      min-width: 0;
      display: flex;
      align-items: center;
      gap: 6px;
    }

    .header-platforms {
      display: inline-flex;
      align-items: center;
      gap: 4px;
      flex: 0 0 auto;
    }

    .header-platform {
      width: 14px;
      height: 14px;
      display: block;
      opacity: 0.95;
    }

    .pane-title {
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
      font-size: 13px;
      font-weight: 600;
      line-height: 1.2;
    }

    .header-side {
      display: flex;
      align-items: center;
      gap: 8px;
      color: var(--muted);
      font-size: 11px;
      white-space: nowrap;
    }

    .clear-activity {
      display: none;
      border: 1px solid var(--divider);
      background: transparent;
      color: var(--muted);
      height: 20px;
      padding: 0 7px;
      font: inherit;
      cursor: pointer;
    }

    .clear-activity.visible {
      display: inline-flex;
      align-items: center;
    }

    .clear-activity:hover {
      color: var(--header-focused-text);
      border-color: var(--muted);
    }

    .feed {
      min-height: 0;
      overflow: auto;
      background: var(--split-bg);
      scrollbar-width: thin;
      scrollbar-color: var(--scrollbar-thumb) var(--scrollbar-bg);
    }

    .feed::-webkit-scrollbar {
      width: 10px;
    }

    .feed::-webkit-scrollbar-track {
      background: var(--scrollbar-bg);
    }

    .feed::-webkit-scrollbar-thumb {
      background: var(--scrollbar-thumb);
    }

    .msg {
      display: grid;
      grid-template-columns: auto auto auto 1fr;
      align-items: start;
      column-gap: 6px;
      row-gap: 3px;
      padding: 6px 8px 6px 10px;
      position: relative;
      border-bottom: 1px solid var(--divider);
      background: var(--msg-bg);
      overflow: hidden;
      isolation: isolate;
    }

    .msg:nth-child(even) {
      background: var(--msg-alt-bg);
    }

    .msg-top {
      grid-column: 1 / -1;
      display: flex;
      align-items: flex-start;
      gap: 6px;
      min-width: 0;
    }

    .msg::before {
      content: "";
      position: absolute;
      inset: 0 auto 0 0;
      width: 2px;
      background: var(--platform-line, transparent);
      z-index: 3;
    }

    .msg::after {
      content: "";
      position: absolute;
      inset: 0;
      background:
        linear-gradient(90deg, var(--overlay-color, rgba(0, 0, 0, 0)) 0%,
        rgba(0, 0, 0, 0) 58%);
      opacity: 0;
      pointer-events: none;
      z-index: 1;
    }

    .msg.has-overlay::after {
      opacity: 1;
    }

    .msg-top,
    .body {
      position: relative;
      z-index: 2;
    }

    .time,
    .platform-badge,
    .author {
      white-space: nowrap;
    }

    .time {
      color: var(--muted);
      font-size: 11px;
      font-weight: 500;
      line-height: 1.35;
    }

    .platform-badge {
      width: 14px;
      height: 14px;
      min-width: 14px;
      display: inline-flex;
      align-items: center;
      justify-content: center;
      opacity: 0.95;
      line-height: 1;
    }

    .platform-icon {
      width: 14px;
      height: 14px;
      display: block;
    }

    .author {
      font-size: 12px;
      font-weight: 700;
      line-height: 1.35;
      min-width: 0;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    .body {
      grid-column: 2 / -1;
      font-size: 13px;
      line-height: 1.4;
      word-break: break-word;
      color: var(--msg-text);
    }

    .msg.system .body {
      color: var(--msg-system);
    }

    .msg.action .body {
      font-style: italic;
    }

    .msg.moderation .body {
      color: var(--msg-system);
    }

    .empty {
      min-height: 100%;
      display: grid;
      place-items: center;
      padding: 18px;
      color: var(--muted);
      text-align: center;
      line-height: 1.45;
      font-size: 12px;
    }
  </style>
</head>
<body>
  <div class="shell">
    <div class="windowbar">
      <div class="windowbar-title">Mergerino</div>
      <div class="windowbar-meta" id="refresh-state">localhost</div>
    </div>

    <div class="page-toolbar">
      <div class="tabstrip page-tabstrip" id="page-tabs"></div>
      <button class="tabs-toggle" id="tabs-toggle" type="button">Hide Tabs</button>
    </div>

    <div class="tabstrip">
      <a class="tab" id="chat-tab" href="/obs-dock?view=chat">Chat</a>
      <a class="tab" id="activity-tab" href="/obs-dock?view=activity">Activity</a>
    </div>

    <div class="header">
      <div class="header-main">
        <div class="header-platforms" id="header-platforms"></div>
        <div class="pane-title" id="pane-title">Connecting...</div>
      </div>
      <div class="header-side">
        <button class="clear-activity" id="clear-activity" type="button">Clear</button>
        <span id="message-count">0 messages</span>
      </div>
    </div>

    <section class="feed" id="feed"></section>
  </div>
)HTML")
        + QStringLiteral(R"HTML(
  <script>
    const platformIcons = {
      twitch: '%23',
      kick: '%24',
      youtube: '%25',
      tiktok: '%26',
    };
    const tabsCollapsedStorageKey = 'mergerino.obsDockTabsCollapsed';
    const feed = document.getElementById('feed');
    const pageTabs = document.getElementById('page-tabs');
    const tabsToggle = document.getElementById('tabs-toggle');
    const headerPlatforms = document.getElementById('header-platforms');
    const paneTitle = document.getElementById('pane-title');
    const messageCount = document.getElementById('message-count');
    const clearActivity = document.getElementById('clear-activity');
    const refreshState = document.getElementById('refresh-state');
    const chatTab = document.getElementById('chat-tab');
    const activityTab = document.getElementById('activity-tab');
    let tabsCollapsed = localStorage.getItem(tabsCollapsedStorageKey) === '1';
    const clearedActivityIdsByTab = new Map();
    let latestState = null;

    function escapePlatform(platform) {
      return (platform || 'twitch').toLowerCase();
    }

    function setMode(view) {
      chatTab.classList.toggle('selected', view !== 'activity');
      activityTab.classList.toggle('selected', view === 'activity');
    }

    function dockUrl(view, tabIndex) {
      const params = new URLSearchParams();
      params.set('view', view || 'chat');
      if (Number.isInteger(tabIndex) && tabIndex >= 0) {
        params.set('tab', String(tabIndex));
      }
      return `/obs-dock?${params.toString()}`;
    }

    function platformIcon(platform) {
      return platformIcons[escapePlatform(platform)] || platformIcons.twitch;
    }

    function applyTabsCollapsedState() {
      pageTabs.classList.toggle('collapsed', tabsCollapsed);
      tabsToggle.textContent = tabsCollapsed ? 'Show Tabs' : 'Hide Tabs';
      tabsToggle.setAttribute('aria-pressed', tabsCollapsed ? 'true' : 'false');
    }

    function navigateTo(view, tabIndex) {
      history.replaceState(null, '', dockUrl(view, tabIndex));
      refresh();
    }

    function renderHeaderPlatforms(state) {
      headerPlatforms.innerHTML = '';
      if (!state.platforms || state.platforms.length === 0) {
        return;
      }

      for (const platform of state.platforms) {
        const icon = document.createElement('img');
        icon.className = 'header-platform';
        icon.src = platformIcon(platform);
        icon.alt = platform;
        icon.title = platform;
        headerPlatforms.appendChild(icon);
      }
    }

    function renderPageTabs(state) {
      pageTabs.innerHTML = '';

      if (!state.tabs || state.tabs.length === 0) {
        applyTabsCollapsedState();
        return;
      }

      for (const tab of state.tabs) {
        const link = document.createElement('a');
        link.className = 'tab page-tab';
        if (tab.index === state.selectedTab) {
          link.classList.add('selected');
        }
        link.href = dockUrl(state.view || 'chat', tab.index);
        link.title = tab.title || `Tab ${tab.index + 1}`;
        link.addEventListener('click', (event) => {
          event.preventDefault();
          navigateTo(state.view || 'chat', tab.index);
        });

        const label = document.createElement('span');
        label.className = 'page-tab-label';
        label.textContent = tab.title || `Tab ${tab.index + 1}`;
        link.appendChild(label);

        const slat = document.createElement('span');
        slat.className = 'page-tab-slat';
        link.appendChild(slat);

        pageTabs.appendChild(link);
      }

      applyTabsCollapsedState();
    }

    function renderEmpty(text) {
      feed.innerHTML = `<div class="empty">${text}</div>`;
    }

    function activityClearKey(state) {
      return String(Number.isInteger(state.selectedTab) ? state.selectedTab : -1);
    }

    function visibleMessages(state) {
      const messages = state.messages || [];
      if (state.view !== 'activity') {
        return messages;
      }

      const cleared = clearedActivityIdsByTab.get(activityClearKey(state));
      if (!cleared || cleared.size === 0) {
        return messages;
      }

      return messages.filter((message) => !cleared.has(message.id || ''));
    }

    function renderMessages(state) {
      if (!state.ready) {
        renderEmpty(state.emptyMessage || 'Select a split in Mergerino.');
        return;
      }

      const messages = visibleMessages(state);
      if (messages.length === 0) {
        renderEmpty('This pane is live, but there are no visible messages yet.');
        return;
      }

      const shouldStickToBottom =
        feed.scrollTop + feed.clientHeight >= feed.scrollHeight - 28;

      feed.innerHTML = '';

      for (const message of messages) {
        const item = document.createElement('article');
        const platform = escapePlatform(message.platform);
        item.className =
          `msg` +
          (message.system ? ' system' : '') +
          (message.alert ? ' alert' : '') +
          (message.action ? ' action' : '') +
          (message.moderation ? ' moderation' : '') +
          (message.highlightColor ? ' has-overlay' : '');
        item.style.setProperty('--platform-line', message.platformAccentColor || '');
        if (message.highlightColor) {
          item.style.setProperty('--overlay-color', message.highlightColor);
        }

        const top = document.createElement('div');
        top.className = 'msg-top';

        const time = document.createElement('span');
        time.className = 'time';
        time.textContent = message.timestamp || '';
        top.appendChild(time);

        const badge = document.createElement('span');
        badge.className = 'platform-badge';
        const badgeIcon = document.createElement('img');
        badgeIcon.className = 'platform-icon';
        badgeIcon.src = platformIcon(platform);
        badgeIcon.alt = platform;
        badgeIcon.title = platform;
        badge.appendChild(badgeIcon);
        top.appendChild(badge);

        if (message.author) {
          const author = document.createElement('span');
          author.className = 'author';
          author.textContent = message.author;
          if (message.authorColor) {
            author.style.color = message.authorColor;
          }
          top.appendChild(author);
        }

        const body = document.createElement('div');
        body.className = 'body';
        body.textContent = message.text || '';

        item.appendChild(top);
        item.appendChild(body);
        feed.appendChild(item);
      }

      if (shouldStickToBottom) {
        feed.scrollTop = feed.scrollHeight;
      }
    }

    )HTML")
        + QStringLiteral(R"HTML(
    async function refresh() {
      try {
        const response = await fetch(`/obs-dock/state${window.location.search}`, {
          cache: 'no-store',
        });
        if (!response.ok) {
          throw new Error(`HTTP ${response.status}`);
        }

        const state = await response.json();
        latestState = state;
        setMode(state.view || 'chat');
        clearActivity.classList.toggle('visible', state.view === 'activity');
        renderPageTabs(state);
        renderHeaderPlatforms(state);
        chatTab.href = dockUrl('chat', state.selectedTab);
        activityTab.href = dockUrl('activity', state.selectedTab);
        chatTab.onclick = (event) => {
          event.preventDefault();
          navigateTo('chat', state.selectedTab);
        };
        activityTab.onclick = (event) => {
          event.preventDefault();
          navigateTo('activity', state.selectedTab);
        };
        paneTitle.textContent = state.paneTitle || 'Mergerino OBS Dock';
        messageCount.textContent = `${visibleMessages(state).length} messages`;
        refreshState.textContent = state.ready ? 'localhost' : 'waiting';
        refreshState.className = `windowbar-meta ${state.ready ? 'ready' : 'waiting'}`;

        renderMessages(state);
      } catch (error) {
        setMode(new URLSearchParams(window.location.search).get('view') || 'chat');
        clearActivity.classList.remove('visible');
        pageTabs.innerHTML = '';
        headerPlatforms.innerHTML = '';
        paneTitle.textContent = 'Mergerino OBS Dock';
        messageCount.textContent = '0 messages';
        refreshState.textContent = 'offline';
        refreshState.className = 'windowbar-meta offline';
        renderEmpty('Mergerino is not responding on the local dock URL yet.');
      }
    }

    tabsToggle.addEventListener('click', () => {
      tabsCollapsed = !tabsCollapsed;
      localStorage.setItem(tabsCollapsedStorageKey, tabsCollapsed ? '1' : '0');
      applyTabsCollapsedState();
    });

    clearActivity.addEventListener('click', () => {
      if (!latestState || latestState.view !== 'activity') {
        return;
      }

      const key = activityClearKey(latestState);
      const cleared = clearedActivityIdsByTab.get(key) || new Set();
      for (const message of latestState.messages || []) {
        if (message.id) {
          cleared.add(message.id);
        }
      }
      clearedActivityIdsByTab.set(key, cleared);
      renderMessages(latestState);
      messageCount.textContent = `${visibleMessages(latestState).length} messages`;
    });

    applyTabsCollapsedState();
    refresh();
    setInterval(refresh, 900);
  </script>
</body>
</html>
)HTML");

    return html.arg(cssColor(theme->window.background))
        .arg(cssColor(theme->splits.background))
        .arg(cssColor(theme->splits.header.background))
        .arg(cssColor(theme->splits.header.focusedBackground))
        .arg(cssColor(theme->splits.header.text))
        .arg(cssColor(theme->splits.header.focusedText))
        .arg(cssColor(divider))
        .arg(cssColor(theme->window.text))
        .arg(cssColor(muted))
        .arg(cssColor(theme->messages.backgrounds.regular))
        .arg(cssColor(theme->messages.backgrounds.alternate))
        .arg(cssColor(theme->messages.textColors.regular))
        .arg(cssColor(theme->messages.textColors.system))
        .arg(cssColor(theme->tabs.regular.backgrounds.regular))
        .arg(cssColor(theme->tabs.regular.text))
        .arg(cssColor(theme->tabs.regular.line.regular))
        .arg(cssColor(theme->tabs.selected.backgrounds.regular))
        .arg(cssColor(theme->tabs.selected.text))
        .arg(cssColor(selectedLine))
        .arg(cssColor(theme->scrollbars.background))
        .arg(cssColor(theme->scrollbars.thumb))
        .arg(cssColor(theme->scrollbars.thumbSelected))
        .arg(twitchIcon)
        .arg(kickIcon)
        .arg(youtubeIcon)
        .arg(tiktokIcon)
        .toUtf8();
}

QByteArray ObsBrowserDockServer::dockStateJson(const QString &view,
                                               int requestedTabIndex) const
{
    const auto resolvedView = normalizedDockView(view);
    auto *window = this->dockWindow();
    QJsonArray tabs;
    int selectedTabIndex = -1;

    if (window != nullptr)
    {
        auto &notebook = window->getNotebook();
        selectedTabIndex = normalizedDockTabIndex(
            requestedTabIndex, notebook.getPageCount(),
            notebook.getSelectedIndex());

        for (int i = 0; i < notebook.getPageCount(); ++i)
        {
            auto *page = dynamic_cast<SplitContainer *>(notebook.getPageAt(i));
            if (page == nullptr)
            {
                continue;
            }

            auto *tab = page->getTab();
            QString title = tab != nullptr ? tab->getTitle() : QString();
            if (title.isEmpty())
            {
                title = QStringLiteral("Tab %1").arg(i + 1);
            }

            tabs.push_back(QJsonObject{
                {QStringLiteral("index"), i},
                {QStringLiteral("title"), title},
                {QStringLiteral("platforms"), pagePlatforms(page)},
            });
        }
    }

    auto *page = this->selectedPage(selectedTabIndex);
    auto *split = this->resolveSplit(page, resolvedView);
    const auto currentPlatforms = pagePlatforms(page);

    QJsonObject root{
        {QStringLiteral("view"), resolvedView},
        {QStringLiteral("localUrl"),
         ObsBrowserDockServer::dockUrl(resolvedView, selectedTabIndex)},
        {QStringLiteral("ready"), split != nullptr},
        {QStringLiteral("paneTitle"), splitLabel(split)},
        {QStringLiteral("selectedTab"), selectedTabIndex},
        {QStringLiteral("tabs"), tabs},
        {QStringLiteral("platforms"), currentPlatforms},
        {QStringLiteral("messageCount"), 0},
    };

    if (page == nullptr)
    {
        root.insert(QStringLiteral("emptyMessage"),
                    QStringLiteral("No Mergerino tabs are open in the main window."));
        return QJsonDocument(root).toJson(QJsonDocument::Compact);
    }

    if (split == nullptr)
    {
        if (resolvedView == QStringLiteral("activity"))
        {
            root.insert(QStringLiteral("emptyMessage"),
                        QStringLiteral("This tab does not have an activity pane."));
        }
        else
        {
            root.insert(QStringLiteral("emptyMessage"),
                        QStringLiteral("This tab does not have a chat split to show."));
        }
        return QJsonDocument(root).toJson(QJsonDocument::Compact);
    }

    auto &snapshot = split->getChannelView().getMessagesSnapshot();
    QJsonArray messages;
    const auto begin =
        snapshot.size() > OBS_DOCK_MESSAGE_LIMIT
            ? snapshot.end() -
                  static_cast<std::ptrdiff_t>(OBS_DOCK_MESSAGE_LIMIT)
            : snapshot.begin();

    for (auto it = begin; it != snapshot.end(); ++it)
    {
        const auto &layout = *it;
        if (!layout)
        {
            continue;
        }

        const auto *message = layout->getMessage();
        if (message == nullptr)
        {
            continue;
        }

        QString author = message->displayName.trimmed();
        if (author.isEmpty())
        {
            author = message->loginName.trimmed();
        }

        const bool isAlert = message->flags.hasAny(
            {MessageFlag::Subscription, MessageFlag::ElevatedMessage,
             MessageFlag::CheerMessage});
        const auto highlightColor = dockEventHighlightColor(*message);
        const auto platformAccentColor = cssColor(
            message->platformAccentColor.value_or(
                defaultPlatformAccent(message->platform)));

        messages.push_back(QJsonObject{
            {QStringLiteral("id"), message->id},
            {QStringLiteral("timestamp"),
             message->parseTime.isValid()
                 ? message->parseTime.toString(QStringLiteral("H:mm"))
                 : QString()},
            {QStringLiteral("author"), author},
            {QStringLiteral("authorColor"),
             message->usernameColor.isValid()
                 ? cssColor(message->usernameColor)
                 : QString()},
            {QStringLiteral("text"), message->messageText},
            {QStringLiteral("platform"), platformName(message->platform)},
            {QStringLiteral("platformAccentColor"), platformAccentColor},
            {QStringLiteral("highlightColor"),
             highlightColor.isValid() ? cssColor(highlightColor) : QString()},
            {QStringLiteral("system"),
             message->flags.has(MessageFlag::System)},
            {QStringLiteral("alert"),
             isAlert || highlightColor.isValid()},
            {QStringLiteral("action"),
             message->flags.has(MessageFlag::Action)},
            {QStringLiteral("moderation"),
             message->flags.has(MessageFlag::ModerationAction)},
        });
    }

    root.insert(QStringLiteral("messageCount"), messages.size());
    root.insert(QStringLiteral("messages"), messages);

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

}  // namespace chatterino
