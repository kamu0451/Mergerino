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

struct DecodedFrame {
    QString payloadType;
    std::vector<DecodedChatMessage> chatMessages;
};

DecodedFrame decodeWebcastPushFrame(QByteArrayView bytes);

}  // namespace chatterino::tiktok
