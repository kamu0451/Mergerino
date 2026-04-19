// SPDX-FileCopyrightText: 2026 Mergerino
// SPDX-License-Identifier: MIT
//
// Decodes a TikTok webcast WebSocket frame into the subset of events we
// currently surface in Mergerino's chat pane. Frame shape (protobuf):
//   WebcastPushFrame  (outer envelope, field 7 = payload_type, 8 = payload)
//   -> ProtoMessageFetchResult  (when payload_type == "msg", field 1 = messages)
//      -> ProtoMessageFetchResultBaseProtoMessage
//           field 1: method (e.g. "WebcastChatMessage")
//           field 2: payload (binary of the specific message type)
//   -> WebcastChatMessage  (field 2 = user_info, field 3 = content)
//      -> User  (field 1 = id, field 3 = nick_name, field 38 = username)

#pragma once

#include <QByteArrayView>
#include <QString>

#include <cstdint>
#include <vector>

namespace chatterino::tiktok {

struct DecodedUser {
    QString uniqueId;
    QString nickname;
    QString secUid;
    qint64 userId{0};
};

struct DecodedChatMessage {
    DecodedUser user;
    QString content;
    qint64 msgId{0};
};

struct DecodedLikeEvent {
    DecodedUser user;
    qint32 count{0};   // likes delivered in this frame (often 1)
    qint64 total{0};   // running total for the room
};

// TikTok bundles join / admin-set / block events into WebcastMemberMessage.
// enter_type=1 is the common "user entered the room" case.
struct DecodedMemberEvent {
    DecodedUser user;
    qint32 enterType{0};
};

// action: 1 = follow, 3 = share (per community-decoded values).
struct DecodedSocialEvent {
    DecodedUser user;
    qint64 action{0};
    qint64 followCount{0};
};

struct DecodedGiftEvent {
    DecodedUser fromUser;
    qint32 repeatCount{0};
    qint32 repeatEnd{0};  // 1 = final frame of a gift combo; earlier frames = still comboing
};

struct DecodedFrame {
    QString payloadType;
    std::vector<DecodedChatMessage> chatMessages;
    std::vector<DecodedLikeEvent> likeEvents;
    std::vector<DecodedMemberEvent> memberEvents;
    std::vector<DecodedSocialEvent> socialEvents;
    std::vector<DecodedGiftEvent> giftEvents;
    // Latest online-viewer count observed from WebcastRoomUserSeqMessage.
    // 0 means "no count in this frame" (consumer should keep previous value).
    qint64 roomViewerCount{0};
};

DecodedFrame decodeWebcastPushFrame(QByteArrayView bytes);

}  // namespace chatterino::tiktok
