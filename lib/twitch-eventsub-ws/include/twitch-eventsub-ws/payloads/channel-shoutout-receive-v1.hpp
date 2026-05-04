#pragma once

#include "twitch-eventsub-ws/chrono.hpp"  // IWYU pragma: keep
#include "twitch-eventsub-ws/payloads/subscription.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <string>

namespace chatterino::eventsub::lib::payload::channel_shoutout_receive::v1 {

/// json_transform=snake_case
struct Event {
    // The receiving broadcaster (us)
    std::string broadcasterUserID;
    std::string broadcasterUserLogin;
    std::string broadcasterUserName;

    // The broadcaster who sent the shoutout
    std::string fromBroadcasterUserID;
    std::string fromBroadcasterUserLogin;
    std::string fromBroadcasterUserName;

    // Number of viewers who saw the shoutout
    int viewerCount;

    // When the shoutout was sent
    std::chrono::system_clock::time_point startedAt;
};

struct Payload {
    subscription::Subscription subscription;

    Event event;
};

#include "twitch-eventsub-ws/payloads/channel-shoutout-receive-v1.inc"

}  // namespace chatterino::eventsub::lib::payload::channel_shoutout_receive::v1
