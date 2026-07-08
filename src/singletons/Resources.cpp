// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Resources.hpp"

#include "debug/AssertInGuiThread.hpp"

namespace {

using namespace chatterino;

static Resources2 *resources = nullptr;

void loadPixmaps(Resources2 &resources)
{
    resources.buttons.ban = QPixmap(":/buttons/ban.png");
    resources.buttons.copyDark = QPixmap(":/buttons/copyDark.png");
    resources.buttons.copyLight = QPixmap(":/buttons/copyLight.png");
    resources.buttons.mod = QPixmap(":/buttons/mod.png");
    resources.buttons.moderationDisabledDarkMode18x18 =
        QPixmap(":/buttons/moderationDisabledDarkMode18x18.png");
    resources.buttons.replyDark = QPixmap(":/buttons/replyDark.png");
    resources.buttons.replyThreadDark =
        QPixmap(":/buttons/replyThreadDark.png");
    resources.buttons.streamerModeEnabledDark =
        QPixmap(":/buttons/streamerModeEnabledDark.png");
    resources.buttons.streamerModeEnabledLight =
        QPixmap(":/buttons/streamerModeEnabledLight.png");
    resources.buttons.trashCan = QPixmap(":/buttons/trashCan.png");
    resources.buttons.unban = QPixmap(":/buttons/unban.png");
    resources.buttons.unmod = QPixmap(":/buttons/unmod.png");
    resources.buttons.unvip = QPixmap(":/buttons/unvip.png");
    resources.buttons.vip = QPixmap(":/buttons/vip.png");
    resources.predictions.blue_1 = QPixmap(":/predictions/blue-1.png");
    resources.predictions.blue_10 = QPixmap(":/predictions/blue-10.png");
    resources.predictions.blue_2 = QPixmap(":/predictions/blue-2.png");
    resources.predictions.blue_3 = QPixmap(":/predictions/blue-3.png");
    resources.predictions.blue_4 = QPixmap(":/predictions/blue-4.png");
    resources.predictions.blue_5 = QPixmap(":/predictions/blue-5.png");
    resources.predictions.blue_6 = QPixmap(":/predictions/blue-6.png");
    resources.predictions.blue_7 = QPixmap(":/predictions/blue-7.png");
    resources.predictions.blue_8 = QPixmap(":/predictions/blue-8.png");
    resources.predictions.blue_9 = QPixmap(":/predictions/blue-9.png");
    resources.predictions.pink_2 = QPixmap(":/predictions/pink-2.png");
    resources.scrolling.downScroll = QPixmap(":/scrolling/downScroll.png");
    resources.scrolling.neutralScroll =
        QPixmap(":/scrolling/neutralScroll.png");
    resources.scrolling.upScroll = QPixmap(":/scrolling/upScroll.png");
    resources.split.down = QPixmap(":/split/down.png");
    resources.split.left = QPixmap(":/split/left.png");
    resources.split.move = QPixmap(":/split/move.png");
    resources.split.right = QPixmap(":/split/right.png");
    resources.split.up = QPixmap(":/split/up.png");
    resources.streamerMode = QPixmap(":/streamerMode.png");
    resources.twitch.automod = QPixmap(":/twitch/automod.png");
    resources.twitch.channelPoints = QPixmap(":/twitch/channelPoints.png");
    resources.twitch.sharedChat = QPixmap(":/twitch/sharedChat.png");
}

}  // namespace

namespace chatterino {

Resources2 &getResources()
{
    assert(resources);

    return *resources;
}

void initResources()
{
    assertInGuiThread();

    resources = new Resources2;
    loadPixmaps(*resources);
}

}  // namespace chatterino
