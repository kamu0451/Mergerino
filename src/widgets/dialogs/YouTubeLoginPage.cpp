// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/YouTubeLoginPage.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "providers/youtube/YouTubeCommon.hpp"
#include "util/HttpServer.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QStringBuilder>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <cassert>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>

using namespace Qt::Literals;

namespace {

using namespace chatterino;

const QString REDIRECT_URL = u"http://127.0.0.1:38277"_s;
constexpr uint16_t SERVER_PORT = 38277;
constexpr auto HTML_CONTENT_TYPE = "text/html; charset=utf-8";
constexpr auto JSON_CONTENT_TYPE = "application/json; charset=utf-8";
constexpr qsizetype PKCE_VERIFIER_BYTES = 32;
constexpr qsizetype STATE_BYTES = 32;
constexpr int AUTHORIZATION_TIMEOUT_MS = 5 * 60 * 1000;
constexpr int NETWORK_STEP_TIMEOUT_MS = 30 * 1000;
// Gives the browser tab time to poll /status and show the error before the
// AuthSession (and its loopback HTTP server) is torn down.
constexpr int ERROR_CLEANUP_DELAY_MS = 30 * 1000;

QByteArray generateRandomBytes(qsizetype size)
{
    assert((size % 4) == 0);
    QByteArray bytes;
    bytes.resize(size);
    auto *gen = QRandomGenerator::system();
    for (qsizetype i = 0; i < bytes.size() / 4; i++)
    {
        quint32 v = gen->generate();
        std::memcpy(bytes.data() + (i * 4), &v, 4);
    }
    return bytes;
}

struct AuthParams {
    QByteArray codeVerifier;
    QByteArray codeChallenge;
    QByteArray state;
};

AuthParams startAuthSession()
{
    auto base64Opts =
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;
    auto codeVerifier =
        generateRandomBytes(PKCE_VERIFIER_BYTES).toBase64(base64Opts);

    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(codeVerifier);
    auto codeChallenge = h.result().toBase64(base64Opts);

    return {
        .codeVerifier = codeVerifier,
        .codeChallenge = codeChallenge,
        .state = generateRandomBytes(STATE_BYTES).toBase64(base64Opts),
    };
}

QString requestPath(const QString &target)
{
    const auto queryIdx = target.indexOf('?');
    return queryIdx >= 0 ? target.left(queryIdx) : target;
}

QUrlQuery targetQuery(const QString &target)
{
    const auto queryIdx = target.indexOf('?');
    return queryIdx >= 0 ? QUrlQuery(target.mid(queryIdx + 1)) : QUrlQuery{};
}

QString youtubeLogoMarkup()
{
    return uR"(<svg class="mark" viewBox="0 0 24 24" role="img" aria-label="YouTube">
<path fill="#ff0000" d="M23.498 6.186a3.016 3.016 0 0 0-2.122-2.136C19.505 3.545 12 3.545 12 3.545s-7.505 0-9.377.505A3.017 3.017 0 0 0 .502 6.186C0 8.07 0 12 0 12s0 3.93.502 5.814a3.016 3.016 0 0 0 2.122 2.136c1.871.505 9.376.505 9.376.505s7.505 0 9.377-.505a3.015 3.015 0 0 0 2.122-2.136C24 15.93 24 12 24 12s0-3.93-.502-5.814z"/>
<path fill="#ffffff" d="M9.545 15.568V8.432L15.818 12l-6.273 3.568z"/>
</svg>)"_s;
}

QByteArray renderPage(const QString &title, const QString &body,
                      const QString &scripts = {})
{
    auto page = QStringLiteral(R"(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>%1</title>
<style>
    :root {
        color-scheme: dark;
        font-family: "Inter", "Segoe UI", system-ui, sans-serif;
        background: #121316;
        color: #f5f5f5;
    }
    body {
        min-height: 100vh;
        margin: 0;
        display: grid;
        place-items: center;
        background: #121316;
    }
    main {
        width: min(480px, calc(100vw - 32px));
        border: 1px solid #2b2d34;
        border-radius: 8px;
        background: #1a1c22;
        padding: 28px;
        box-sizing: border-box;
    }
    .mark {
        width: 40px;
        height: 40px;
        margin: 0 0 18px;
        display: block;
    }
    h1 {
        margin: 0 0 12px;
        font-size: 22px;
        font-weight: 700;
        letter-spacing: 0;
    }
    p {
        margin: 0;
        color: #c7cad1;
        line-height: 1.45;
    }
    .error {
        color: #ffb3b3;
    }
    .success {
        color: #9df0ba;
    }
</style>
</head>
<body>
<main>%2</main>
%3
</body>
</html>)")
                    .arg(title.toHtmlEscaped(), body, scripts);
    return page.toUtf8();
}

QByteArray renderWaitingPage()
{
    return renderPage(
        u"Mergerino YouTube Login"_s,
        youtubeLogoMarkup() %
            uR"(
<h1>Waiting for Google</h1>
<p>Complete the YouTube authorization page that opened in your browser.</p>)"_s);
}

QByteArray renderCallbackPage()
{
    return renderPage(
        u"Mergerino YouTube Login"_s,
        youtubeLogoMarkup() %
            uR"(
<h1>Finishing YouTube login</h1>
<p id="status">Sending the Google authorization code to Mergerino...</p>)"_s,
        uR"(<script>
const statusNode = document.getElementById("status");

async function poll() {
    try {
        const res = await fetch("/status?t=" + Date.now(), {cache: "no-store"});
        const data = await res.json();
        statusNode.className = data.state === "error" ? "error" : data.state === "success" ? "success" : "";
        statusNode.textContent = data.message;
        if (data.state === "pending") {
            window.setTimeout(poll, 700);
        }
    } catch (error) {
        statusNode.textContent = "Still waiting for Mergerino to finish the login.";
        window.setTimeout(poll, 1000);
    }
}

poll();
</script>)"_s);
}

HttpServer::Response htmlResponse(const QByteArray &body, unsigned status = 200)
{
    return {
        .status = status,
        .body = body,
        .contentType = HTML_CONTENT_TYPE,
    };
}

HttpServer::Response jsonResponse(const QJsonObject &body, unsigned status = 200)
{
    return {
        .status = status,
        .body = QJsonDocument(body).toJson(QJsonDocument::Compact),
        .contentType = JSON_CONTENT_TYPE,
    };
}

QString formatGoogleError(const NetworkResult &result)
{
    const auto json = result.parseJson();
    QString error;

    const auto errorValue = json["error"_L1];
    if (errorValue.isObject())
    {
        error = errorValue.toObject()["message"_L1].toString();
    }
    else
    {
        error = json["error_description"_L1].toString(errorValue.toString());
    }

    if (!error.isEmpty())
    {
        return u"Error: "_s % error % u" ("_s % result.formatError() % u")"_s;
    }
    return u"Error: "_s % result.formatError() %
           u" (no further information)"_s;
}

qint64 tokenExpirySeconds(const QJsonObject &tokenData)
{
    const auto expiresInVal = tokenData["expires_in"_L1];
    qint64 expiresIn = 0;
    if (expiresInVal.isString())
    {
        expiresIn = expiresInVal.toString().toLongLong();
    }
    else
    {
        expiresIn = expiresInVal.toInteger();
    }

    if (expiresIn <= 0 ||
        expiresIn > std::numeric_limits<qint32>::max())
    {
        return 3600;
    }
    return expiresIn;
}

void addOAuthClientSecret(QUrlQuery &payload)
{
    const auto clientSecret = youTubeOAuthClientSecret();
    if (!clientSecret.isEmpty())
    {
        payload.addQueryItem(u"client_secret"_s, clientSecret);
    }
}

class AuthSession final : public QObject
{
public:
    AuthSession(QWidget *ownerWindow, std::function<void()> onAuthenticated,
                QObject *parent)
        : QObject(parent)
        , ownerWindow_(ownerWindow)
        , authParams_(startAuthSession())
        , onAuthenticated_(std::move(onAuthenticated))
    {
        this->authorizationTimer_.setSingleShot(true);
        QObject::connect(&this->authorizationTimer_, &QTimer::timeout, this,
                         [this] {
                             this->setBrowserState(
                                 u"error"_s,
                                 u"YouTube login timed out. Start the login "
                                 u"flow again."_s);
                             QTimer::singleShot(1000, this,
                                                &QObject::deleteLater);
                         });

        this->pendingTimer_.setSingleShot(true);
        QObject::connect(&this->pendingTimer_, &QTimer::timeout, this, [this] {
            this->setBrowserState(u"error"_s, this->pendingTimeoutMessage_);
            this->scheduleErrorCleanup();
        });
    }

    bool begin()
    {
        this->server_ = new HttpServer(SERVER_PORT, this);
        if (!this->server_->isListening())
        {
            this->showError(
                u"Unable to start the local YouTube login server on %1. %2"_s
                    .arg(REDIRECT_URL, this->server_->errorString()));
            this->deleteLater();
            return false;
        }

        this->server_->setHandler([this](const auto &request) {
            return this->handleRequest(request);
        });
        this->authorizationTimer_.start(AUTHORIZATION_TIMEOUT_MS);

        if (!QDesktopServices::openUrl(this->buildAuthUrl()))
        {
            this->showError(
                u"Unable to open Google login in your browser. Try again from "
                u"the accounts settings page."_s);
            this->deleteLater();
            return false;
        }

        return true;
    }

private:
    HttpServer::Response handleRequest(const HttpServer::Request &request)
    {
        const auto path = requestPath(request.target);
        if (request.method.compare(u"GET"_s, Qt::CaseInsensitive) == 0 &&
            path == u"/"_s)
        {
            const auto query = targetQuery(request.target);
            if (query.hasQueryItem(u"code"_s) ||
                query.hasQueryItem(u"error"_s))
            {
                return this->handleCallback(query);
            }

            return htmlResponse(renderWaitingPage());
        }

        if (request.method.compare(u"GET"_s, Qt::CaseInsensitive) == 0 &&
            path == u"/status"_s)
        {
            return jsonResponse({
                {"state"_L1, this->browserState_},
                {"message"_L1, this->browserMessage_},
            });
        }

        return htmlResponse(renderWaitingPage());
    }

    HttpServer::Response handleCallback(const QUrlQuery &query)
    {
        this->authorizationTimer_.stop();

        if (query.hasQueryItem(u"error"_s))
        {
            const auto error = query.queryItemValue(u"error"_s).trimmed();
            const auto description =
                query.queryItemValue(u"error_description"_s).trimmed();
            auto message =
                description.isEmpty()
                    ? QString(u"Google returned an authorization error: " %
                              error)
                    : QString(u"Google returned an authorization error: " %
                              description);
            this->setBrowserState(u"error"_s, message);
            this->scheduleErrorCleanup();
            return htmlResponse(renderCallbackPage(), 400);
        }

        if (!query.hasQueryItem(u"code"_s))
        {
            const auto message =
                u"Google did not return an authorization code."_s;
            this->setBrowserState(u"error"_s, message);
            this->scheduleErrorCleanup();
            return htmlResponse(renderCallbackPage(), 400);
        }

        if (query.queryItemValue(u"state"_s) != this->authParams_.state)
        {
            const auto message =
                u"Google returned an invalid login state. Start again."_s;
            this->setBrowserState(u"error"_s, message);
            this->scheduleErrorCleanup();
            return htmlResponse(renderCallbackPage(), 400);
        }

        this->setBrowserState(
            u"pending"_s,
            u"Exchanging the Google authorization code..."_s);
        this->startPendingTimeout(
            u"Google token exchange timed out. Start the login flow again."_s,
            NETWORK_STEP_TIMEOUT_MS);
        this->requestToken(query.queryItemValue(u"code"_s));

        return htmlResponse(renderCallbackPage());
    }

    QUrl buildAuthUrl() const
    {
        QUrlQuery query{
            {"response_type"_L1, "code"_L1},
            {"client_id"_L1, youTubeOAuthClientID()},
            {"redirect_uri"_L1, REDIRECT_URL},
            {"scope"_L1, youTubeOAuthScope()},
            {"access_type"_L1, "offline"_L1},
            {"prompt"_L1, "consent"_L1},
            {"code_challenge"_L1, this->authParams_.codeChallenge},
            {"code_challenge_method"_L1, "S256"_L1},
            {"state"_L1, this->authParams_.state},
        };
        return QUrl(u"https://accounts.google.com/o/oauth2/v2/auth?"_s %
                    query.toString(QUrl::FullyEncoded));
    }

    void requestToken(const QString &code)
    {
        QUrlQuery payload{
            {"code"_L1, code},
            {"client_id"_L1, youTubeOAuthClientID()},
            {"code_verifier"_L1, this->authParams_.codeVerifier},
            {"redirect_uri"_L1, REDIRECT_URL},
            {"grant_type"_L1, "authorization_code"_L1},
        };
        addOAuthClientSecret(payload);

        NetworkRequest("https://oauth2.googleapis.com/token",
                       NetworkRequestType::Post)
            .header("Content-Type", "application/x-www-form-urlencoded")
            .hideRequestBody()
            .payload(payload.toString(QUrl::FullyEncoded).toUtf8())
            .timeout(20'000)
            .caller(this)
            .onError([this](const NetworkResult &result) {
                this->clearPendingTimeout();
                auto error = formatGoogleError(result);
                qCWarning(chatterinoYouTube)
                    << "Getting YouTube token failed" << error;
                this->setBrowserState(u"error"_s, error);
                this->scheduleErrorCleanup();
            })
            .onSuccess([this](const NetworkResult &result) {
                this->setBrowserState(
                    u"pending"_s,
                    u"Loading the authenticated YouTube channel..."_s);
                this->startPendingTimeout(
                    u"YouTube channel lookup timed out. Start the login flow "
                    u"again."_s,
                    NETWORK_STEP_TIMEOUT_MS);
                this->getAuthenticatedChannel(result.parseJson());
            })
            .execute();
    }

    void getAuthenticatedChannel(const QJsonObject &tokenData)
    {
        const auto accessToken = tokenData["access_token"_L1].toString();
        const auto refreshToken = tokenData["refresh_token"_L1].toString();
        if (accessToken.isEmpty() || refreshToken.isEmpty())
        {
            this->clearPendingTimeout();
            const auto error =
                u"Google did not return usable YouTube login tokens."_s;
            // Do NOT log tokenData: on a 2xx that returns an access_token but
            // no refresh_token this branch still fires with a live bearer token
            // in the payload, which --log-file would persist to disk. Log only
            // which fields were present.
            qCWarning(chatterinoYouTube)
                << error << "hasAccessToken:" << !accessToken.isEmpty()
                << "hasRefreshToken:" << !refreshToken.isEmpty();
            this->setBrowserState(u"error"_s, error);
            this->scheduleErrorCleanup();
            return;
        }

        auto expiresAt = QDateTime::currentDateTimeUtc().addSecs(
            tokenExpirySeconds(tokenData));

        QUrl url("https://www.googleapis.com/youtube/v3/channels");
        QUrlQuery query{
            {"part"_L1, "snippet"_L1},
            {"mine"_L1, "true"_L1},
        };
        url.setQuery(query);

        NetworkRequest(url)
            .header("Authorization", u"Bearer "_s % accessToken)
            .timeout(20'000)
            .caller(this)
            .onError([this](const NetworkResult &result) {
                this->clearPendingTimeout();
                auto error = formatGoogleError(result);
                qCWarning(chatterinoYouTube)
                    << "Getting YouTube channel failed" << error;
                this->setBrowserState(u"error"_s, error);
                this->scheduleErrorCleanup();
            })
            .onSuccess([this, accessToken, refreshToken,
                        expiresAt](const NetworkResult &result) {
                this->clearPendingTimeout();

                const auto items = result.parseJson()["items"_L1].toArray();
                if (items.isEmpty() || !items.at(0).isObject())
                {
                    const auto error =
                        u"YouTube did not return the authenticated channel."_s;
                    qCWarning(chatterinoYouTube) << error << result.getData();
                    this->setBrowserState(u"error"_s, error);
                    this->scheduleErrorCleanup();
                    return;
                }

                const auto channel = items.at(0).toObject();
                const auto channelID = channel["id"_L1].toString().trimmed();
                const auto displayName =
                    channel["snippet"_L1]
                        .toObject()["title"_L1]
                        .toString()
                        .trimmed();

                if (channelID.isEmpty() || displayName.isEmpty())
                {
                    const auto error =
                        u"YouTube returned incomplete channel information."_s;
                    qCWarning(chatterinoYouTube) << error << channel;
                    this->setBrowserState(u"error"_s, error);
                    this->scheduleErrorCleanup();
                    return;
                }

                YouTubeAccountData data{
                    .displayName = displayName,
                    .channelID = channelID,
                    .clientID = youTubeOAuthClientID(),
                    .authToken = accessToken,
                    .refreshToken = refreshToken,
                    .expiresAt = expiresAt,
                };
                data.save();
                getApp()->getAccounts()->youtube.reloadUsers();
                getApp()->getAccounts()->youtube.currentChannelID =
                    data.channelID;
                this->setBrowserState(
                    u"success"_s,
                    u"YouTube account connected. You can close this browser "
                    u"tab."_s);
                if (this->onAuthenticated_)
                {
                    this->onAuthenticated_();
                    this->onAuthenticated_ = {};
                }
                QTimer::singleShot(15000, this, &QObject::deleteLater);
            })
            .execute();
    }

    void setBrowserState(const QString &state, const QString &message)
    {
        this->browserState_ = state;
        this->browserMessage_ = message;
    }

    void startPendingTimeout(const QString &message, int timeoutMs)
    {
        this->pendingTimeoutMessage_ = message;
        this->pendingTimer_.start(timeoutMs);
    }

    void clearPendingTimeout()
    {
        this->pendingTimer_.stop();
        this->pendingTimeoutMessage_.clear();
    }

    // Every terminal error branch must call this so the session (and its
    // loopback HTTP server on 127.0.0.1:38277) doesn't leak until the next
    // login attempt or app exit.
    void scheduleErrorCleanup()
    {
        QTimer::singleShot(ERROR_CLEANUP_DELAY_MS, this,
                          &QObject::deleteLater);
    }

    void showError(const QString &message)
    {
        this->setBrowserState(u"error"_s, message);
        if (this->ownerWindow_)
        {
            QMessageBox::critical(this->ownerWindow_, u"YouTube login"_s,
                                  message);
        }
    }

    HttpServer *server_{};
    QPointer<QWidget> ownerWindow_;
    AuthParams authParams_;
    std::function<void()> onAuthenticated_;
    QTimer authorizationTimer_;
    QTimer pendingTimer_;
    QString pendingTimeoutMessage_;
    QString browserState_ = u"pending"_s;
    QString browserMessage_ =
        u"Complete the YouTube authorization page in your browser."_s;
};

bool startYouTubeLoginFlow(QWidget *parent,
                           std::function<void()> onAuthenticated)
{
    static QPointer<AuthSession> activeSession;
    if (activeSession)
    {
        delete activeSession.data();
        activeSession.clear();
    }

    auto *session = new AuthSession(parent ? parent->window() : nullptr,
                                    std::move(onAuthenticated), qApp);
    activeSession = session;
    QObject::connect(session, &QObject::destroyed, qApp, [session] {
        if (activeSession == session)
        {
            activeSession.clear();
        }
    });
    return session->begin();
}

}  // namespace

namespace chatterino {

QWidget *YouTubeLoginPage::createReviewNotice(QWidget *parent)
{
    auto *notice = new QFrame(parent);
    notice->setObjectName(u"youtubeReviewNotice"_s);
    notice->setFrameShape(QFrame::NoFrame);
    notice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    notice->setStyleSheet(uR"(
        QFrame#youtubeReviewNotice {
            background: #4b2f13;
            border: 1px solid #d1852b;
            border-radius: 6px;
        }
        QFrame#youtubeReviewNotice QLabel {
            color: #ffd8a8;
        }
        QLabel#youtubeReviewIcon {
            background: #f59f00;
            border-radius: 9px;
            color: #1f1405;
            font-weight: 800;
        }
    )"_s);

    auto *layout = new QHBoxLayout(notice);
    layout->setContentsMargins(10, 9, 10, 9);
    layout->setSpacing(8);

    auto *icon = new QLabel(u"!"_s, notice);
    icon->setObjectName(u"youtubeReviewIcon"_s);
    icon->setAlignment(Qt::AlignCenter);
    icon->setFixedSize(18, 18);
    layout->addWidget(icon, 0, Qt::AlignTop);

    auto *text = new QLabel(
        u"YouTube sign-in uses official Google OAuth, but it is currently "
        u"under review and may take several weeks.\n\n"
        u"If Google shows a warning during sign-in, click Advanced, then "
        u"Go to Mergerino, then continue."_s,
        notice);
    text->setWordWrap(true);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(text, 1);

    return notice;
}

bool YouTubeLoginPage::startLoginFlow(QWidget *parent,
                                      std::function<void()> onAuthenticated)
{
    return startYouTubeLoginFlow(parent, std::move(onAuthenticated));
}

YouTubeLoginPage::YouTubeLoginPage()
{
    auto *root = new QVBoxLayout(this);

    if (getApp()->getAccounts()->youtube.accounts.raw().empty())
    {
        root->addWidget(YouTubeLoginPage::createReviewNotice(this));
    }

    auto *topLabel = new QLabel(
        "YouTube sign-in opens Google in your browser, then Mergerino catches "
        "the login locally and saves the account on this PC.");
    topLabel->setWordWrap(true);
    root->addWidget(topLabel);

    auto *startButton = new QPushButton("Log in with YouTube");
    startButton->setDefault(true);
    root->addWidget(startButton, 0, Qt::AlignLeft);
    root->addStretch(1);

    QObject::connect(startButton, &QPushButton::clicked, this, [this] {
        startYouTubeLoginFlow(
            this,
            [window = QPointer<QWidget>(this->window())] {
                if (window)
                {
                    window->close();
                }
            });
    });
}

}  // namespace chatterino
