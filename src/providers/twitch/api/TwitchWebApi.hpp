// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace chatterino {

struct HelixPolls;
struct HelixPrediction;
struct HelixPredictions;
struct HelixChatterGroups;

class TwitchWebApi
{
public:
    static void startPoll(const QString &channelId, const QString &title,
                          const QStringList &choices, int durationSeconds,
                          std::optional<int> pointsPerVote,
                          const QString &oauthClient,
                          const QString &oauthToken,
                          std::function<void()> successCallback,
                          std::function<void(const QString &)> failureCallback);

    static void getPolls(
        const QString &channelId, QStringList ids, int first,
        const QString &after, const QString &oauthClient,
        const QString &oauthToken,
        std::function<void(const HelixPolls &)> successCallback,
        std::function<void(const QString &)> failureCallback);

    static void startPrediction(
        const QString &channelId, const QString &title,
        const QStringList &outcomes, int predictionWindowSeconds,
        const QString &oauthClient, const QString &oauthToken,
        std::function<void(const QString &, const QJsonObject &)>
            successCallback,
        std::function<void(const QString &)> failureCallback);

    static void getPredictions(
        const QString &channelId, QStringList ids, int first,
        const QString &after, const QString &oauthClient,
        const QString &oauthToken,
        std::function<void(const HelixPredictions &)> successCallback,
        std::function<void(const QString &)> failureCallback);

    static void getActivePollAndPredictions(
        const QString &channelId, const QString &oauthClient,
        const QString &oauthToken,
        std::function<void(const HelixPolls &, const HelixPredictions &)>
            successCallback,
        std::function<void(const QString &)> failureCallback);

    static void getChatterGroups(
        const QString &broadcasterName, const QString &oauthClient,
        const QString &oauthToken, const QString &clientIntegrity,
        const QString &deviceId,
        std::function<void(const HelixChatterGroups &)> successCallback,
        std::function<void(const QString &)> failureCallback);

    static void endPrediction(
        const QString &channelId, const QString &predictionId,
        bool refundPoints, const QString &winningOutcomeId,
        const QString &oauthClient, const QString &oauthToken,
        std::function<void(const HelixPrediction &)> successCallback,
        std::function<void(const QString &)> failureCallback);

    static void endPredictionEvent(
        const QString &predictionId, bool refundPoints,
        const QString &winningOutcomeId, const QString &oauthClient,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
};

}  // namespace chatterino
