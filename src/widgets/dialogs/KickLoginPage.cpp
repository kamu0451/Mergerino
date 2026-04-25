#include "widgets/dialogs/KickLoginPage.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/kick/KickAccount.hpp"
#include "singletons/Theme.hpp"
#include "util/HttpServer.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <functional>
#include <utility>

using namespace Qt::Literals;

namespace {

using namespace chatterino;

const QString REDIRECT_URL = u"http://localhost:38275"_s;
const QUrl LOCAL_LOGIN_URL{REDIRECT_URL};
const QUrl KICK_DEVELOPER_URL{u"https://kick.com/settings/developer"_s};
constexpr uint16_t SERVER_PORT = 38275;
constexpr auto HTML_CONTENT_TYPE = "text/html; charset=utf-8";
constexpr auto JSON_CONTENT_TYPE = "application/json; charset=utf-8";

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

QString formatAPIError(const NetworkResult &result)
{
    const auto json = result.parseJson();
    auto error =
        json["error_description"_L1].toString(json["message"_L1].toString());
    if (!error.isEmpty())
    {
        return u"Error: " % error % u" (" % result.formatError() % ')';
    }
    return u"Error: " % result.formatError() % u" (no further information)";
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
    auto codeVerifier = generateRandomBytes(1024).toBase64(base64Opts);

    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(codeVerifier);
    auto codeChallenge = h.result().toBase64(base64Opts);

    return {
        .codeVerifier = codeVerifier,
        .codeChallenge = codeChallenge,
        .state = generateRandomBytes(512).toBase64(base64Opts),
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

QUrlQuery formQuery(const QByteArray &body)
{
    return QUrlQuery(QString::fromUtf8(body));
}

QByteArray renderPage(const QString &title, const QString &body,
                      const QString &scripts = {})
{
    auto page = QStringLiteral(R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>)") %
                title.toHtmlEscaped() %
                QStringLiteral(R"(</title>
<style>
  :root {
    color-scheme: dark;
    --bg: #0b1110;
    --bg-soft: #0f1716;
    --card: rgba(18, 26, 25, 0.94);
    --card-strong: #111918;
    --ink: #edf6f2;
    --muted: #9eb3ab;
    --line: rgba(148, 174, 164, 0.2);
    --accent: #00a650;
    --accent-dark: #008844;
    --accent-soft: rgba(0, 166, 80, 0.14);
    --danger-bg: rgba(161, 56, 24, 0.16);
    --danger-border: rgba(239, 135, 100, 0.38);
    --danger-ink: #ffb49d;
    --success-bg: rgba(0, 166, 80, 0.14);
    --success-border: rgba(45, 211, 111, 0.36);
    --success-ink: #92f2bc;
  }

  * { box-sizing: border-box; }

  body {
    margin: 0;
    min-height: 100vh;
    padding: 24px;
    background:
      radial-gradient(circle at top left, rgba(0, 166, 80, 0.18), transparent 32%),
      radial-gradient(circle at bottom right, rgba(10, 120, 68, 0.2), transparent 28%),
      linear-gradient(180deg, #08100f 0%, var(--bg) 100%);
    color: var(--ink);
    font: 16px/1.5 "Segoe UI", "Helvetica Neue", sans-serif;
  }

  .card {
    max-width: 860px;
    margin: 0 auto;
    min-height: calc(100vh - 48px);
    background: var(--card);
    border: 1px solid var(--line);
    border-radius: 18px;
    padding: 28px 32px;
    box-shadow: 0 24px 70px rgba(0, 0, 0, 0.34);
    backdrop-filter: blur(10px);
    display: flex;
    flex-direction: column;
  }

  .eyebrow {
    margin: 0 0 8px;
    color: var(--accent-dark);
    font-size: 12px;
    font-weight: 700;
    letter-spacing: 0.12em;
    text-align: center;
    text-transform: uppercase;
  }

  h1 {
    margin: 0 0 12px;
    font-size: clamp(28px, 4vw, 40px);
    line-height: 1.05;
  }

  h2 {
    margin: 0;
    font-size: clamp(22px, 3vw, 30px);
    line-height: 1.1;
  }

  .intro-copy {
    margin: 0 0 12px;
    color: #d4e4dd;
    font-size: 17px;
    text-align: center;
  }

  .wizard-form {
    flex: 1;
    display: flex;
  }

  .wizard-viewport {
    overflow: hidden;
    flex: 1;
  }

  .wizard-track {
    display: flex;
    height: 100%;
    transition: transform 420ms cubic-bezier(0.22, 1, 0.36, 1);
    will-change: transform;
  }

  .wizard-step {
    flex: 0 0 100%;
    min-width: 100%;
  }

  .step-screen {
    min-height: 100%;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 18px;
    padding: 28px 16px;
    text-align: center;
  }

  .step-index {
    margin: 0;
    color: #7fe3ad;
    font-size: 12px;
    font-weight: 800;
    letter-spacing: 0.14em;
    text-transform: uppercase;
  }

  .step-copy {
    margin: 0;
    color: #d8e7e0;
    font-size: 18px;
    max-width: 560px;
  }

  .setting-intent {
    margin: 0;
    color: #7fe3ad;
    font-size: 12px;
    font-weight: 800;
    letter-spacing: 0.14em;
    text-transform: uppercase;
  }

  .step-actions {
    display: flex;
    flex-wrap: wrap;
    gap: 10px;
    justify-content: center;
  }

  code {
    font-family: "Cascadia Mono", "Consolas", monospace;
    font-size: 14px;
  }

  .hero-code {
    display: block;
    max-width: 680px;
    overflow-wrap: anywhere;
    color: #d8f7e7;
    font-size: clamp(24px, 3.2vw, 36px);
    font-weight: 800;
    letter-spacing: -0.02em;
    line-height: 1.2;
  }

  .setting-check {
    display: none;
  }

  .scope-intro {
    margin: 0;
    color: #d8e7e0;
    font-size: 18px;
    max-width: 620px;
  }

  .scope-warning {
    margin: -4px 0 0;
    color: #92f2bc;
    font-size: 14px;
    font-weight: 700;
    letter-spacing: 0.04em;
    text-transform: uppercase;
  }

  .scope-list {
    display: grid;
    gap: 12px;
    width: min(100%, 760px);
    margin: 6px 0 0;
    text-align: left;
  }

  .scope-item {
    display: grid;
    grid-template-columns: 28px minmax(0, 1fr);
    align-items: start;
    gap: 14px;
  }

  .scope-icon {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 28px;
    height: 28px;
    border-radius: 999px;
    background: rgba(32, 196, 102, 0.16);
    color: #7fe3ad;
    font-size: 16px;
    font-weight: 900;
    line-height: 1;
  }

  .scope-label {
    color: #edf6f2;
    font-size: 20px;
    font-weight: 700;
    line-height: 1.25;
  }

  .single-field {
    display: grid;
    gap: 6px;
    width: min(100%, 460px);
    text-align: left;
  }

  .single-field span {
    font-weight: 700;
    color: #dbe7e1;
  }

  input {
    width: 100%;
    padding: 12px 14px;
    border: 1px solid rgba(148, 174, 164, 0.28);
    border-radius: 12px;
    background: rgba(7, 11, 11, 0.72);
    color: var(--ink);
    font: inherit;
  }

  input:focus {
    outline: 2px solid rgba(0, 166, 80, 0.24);
    border-color: var(--accent);
  }

  button, .link-button {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    min-height: 46px;
    padding: 0 16px;
    border: 0;
    border-radius: 999px;
    cursor: pointer;
    text-decoration: none;
    font: inherit;
    font-weight: 700;
    transition: background 140ms ease, border-color 140ms ease, color 140ms ease;
  }

  button.primary, .link-button.primary {
    background: var(--accent);
    color: #fff;
  }

  button.primary:hover, .link-button.primary:hover {
    background: var(--accent-dark);
  }

  button.secondary, .link-button.secondary {
    background: #1b2422;
    color: var(--ink);
    border: 1px solid rgba(148, 174, 164, 0.22);
  }

  button.secondary:hover, .link-button.secondary:hover {
    background: #22302d;
  }

  button.secondary.copied {
    background: var(--success-bg);
    border-color: var(--success-border);
    color: var(--success-ink);
  }

  .actions {
    display: flex;
    flex-wrap: wrap;
    gap: 10px;
  }

  .notice {
    margin: 0 0 18px;
    padding: 14px 16px;
    border-radius: 14px;
    text-align: center;
  }

  .notice.error {
    border: 1px solid var(--danger-border);
    background: var(--danger-bg);
    color: var(--danger-ink);
  }

  .status {
    margin: 12px 0 18px;
    color: var(--muted);
  }

  .footnote {
    margin-top: 16px;
    color: var(--muted);
    font-size: 14px;
  }

  @media (max-width: 640px) {
    body {
      padding: 14px;
    }

    .card {
      min-height: calc(100vh - 28px);
      padding: 22px 18px;
    }

    .step-actions {
      flex-direction: column;
    }

    .step-actions > * {
      width: 100%;
    }

    .step-screen {
      padding: 18px 8px;
    }
  }
</style>
</head>
<body>)") %
        body % scripts % QStringLiteral(R"(
</body>
</html>)");
    return page.toUtf8();
}

QByteArray renderWizardPage(const QString &clientID, const QString &clientSecret,
                            const QString &errorMessage = {})
{
    QString errorMarkup;
    if (!errorMessage.isEmpty())
    {
        errorMarkup = QStringLiteral(R"(
<div class="notice error">%1</div>)")
                          .arg(errorMessage.toHtmlEscaped());
    }

    auto body = QStringLiteral(R"KICK(
<main class="card">
  <p class="eyebrow">Kick Login</p>
  <p class="intro-copy">One thing at a time.</p>
  %1
  <form id="kick-login-form" class="wizard-form" method="post" action="/start" autocomplete="off">
    <div class="wizard-viewport">
      <div id="wizard-track" class="wizard-track">
        <section class="wizard-step">
          <div class="step-screen">
            <p class="step-index">1 / 5</p>
            <h1>Create a Kick app</h1>
            <p class="step-copy">Open Kick's developer page and make a new app.</p>
            <div class="step-actions">
              <a class="link-button secondary" href="%3" target="_blank" rel="noreferrer">Open Kick developer page</a>
              <button type="button" class="primary" data-next-step>Next</button>
            </div>
          </div>
        </section>
        <section class="wizard-step">
          <div class="step-screen">
            <p class="step-index">2 / 5</p>
            <h1>Add this redirect URL</h1>
            <code id="redirect-uri" class="hero-code">%2</code>
            <div class="step-actions">
              <button type="button" class="secondary" data-prev-step>Back</button>
              <button type="button" id="copy-button" class="secondary" onclick="copyRedirect()">Copy redirect URL</button>
              <button type="button" class="primary" data-next-step>Next</button>
            </div>
          </div>
        </section>
        <section class="wizard-step">
          <div class="step-screen">
            <p class="step-index">3 / 5</p>
            <p class="setting-intent">Turn these on in Kick</p>
            <div class="setting-check">✓</div>
            <h1>Enable these 6 boxes only</h1>
            <p class="scope-intro">Turn on every box in this list. Leave every other Kick box off.</p>
            <p class="scope-warning">Only these 6</p>
            <div class="scope-list" role="list" aria-label="Required Kick scopes">
              <div class="scope-item" role="listitem">
                <span class="scope-icon">✓</span>
                <span class="scope-label">Read user information (including email address)</span>
              </div>
              <div class="scope-item" role="listitem">
                <span class="scope-icon">✓</span>
                <span class="scope-label">Read channel information</span>
              </div>
              <div class="scope-item" role="listitem">
                <span class="scope-icon">✓</span>
                <span class="scope-label">Update channel information</span>
              </div>
              <div class="scope-item" role="listitem">
                <span class="scope-icon">✓</span>
                <span class="scope-label">Write to Chat feed</span>
              </div>
              <div class="scope-item" role="listitem">
                <span class="scope-icon">✓</span>
                <span class="scope-label">Execute moderation actions for moderators</span>
              </div>
              <div class="scope-item" role="listitem">
                <span class="scope-icon">✓</span>
                <span class="scope-label">Execute moderation actions on chat messages</span>
              </div>
            </div>
            <div class="step-actions">
              <button type="button" class="secondary" data-step-target="2">Back</button>
              <button type="button" class="primary" data-step-target="8">Next</button>
            </div>
          </div>
        </section>
        <section class="wizard-step">
          <div class="step-screen">
            <p class="step-index">4 / 10</p>
            <p class="setting-intent">Turn this on in Kick</p>
            <div class="setting-check">✓</div>
            <h1>Read channel information</h1>
            <div class="step-actions">
              <button type="button" class="secondary" data-step-target="2">Back</button>
              <button type="button" class="primary" data-next-step>Next</button>
            </div>
          </div>
        </section>
        <section class="wizard-step">
          <div class="step-screen">
            <p class="step-index">5 / 10</p>
            <p class="setting-intent">Turn this on in Kick</p>
            <div class="setting-check">✓</div>
            <h1>Update channel information</h1>
            <div class="step-actions">
              <button type="button" class="secondary" data-prev-step>Back</button>
              <button type="button" class="primary" data-next-step>Next</button>
            </div>
          </div>
        </section>
        <section class="wizard-step">
          <div class="step-screen">
            <p class="step-index">6 / 10</p>
            <p class="setting-intent">Turn this on in Kick</p>
            <div class="setting-check">✓</div>
            <h1>Write to Chat feed</h1>
            <div class="step-actions">
              <button type="button" class="secondary" data-prev-step>Back</button>
              <button type="button" class="primary" data-next-step>Next</button>
            </div>
          </div>
        </section>
        <section class="wizard-step">
          <div class="step-screen">
            <p class="step-index">7 / 10</p>
            <p class="setting-intent">Turn this on in Kick</p>
            <div class="setting-check">✓</div>
            <h1>Execute moderation actions for moderators</h1>
            <div class="step-actions">
              <button type="button" class="secondary" data-prev-step>Back</button>
              <button type="button" class="primary" data-next-step>Next</button>
            </div>
          </div>
        </section>
        <section class="wizard-step">
          <div class="step-screen">
            <p class="step-index">8 / 10</p>
            <p class="setting-intent">Turn this on in Kick</p>
            <div class="setting-check">✓</div>
            <h1>Execute moderation actions on chat messages</h1>
            <div class="step-actions">
              <button type="button" class="secondary" data-prev-step>Back</button>
              <button type="button" class="primary" data-next-step>Next</button>
            </div>
          </div>
        </section>
        <section class="wizard-step">
          <div class="step-screen">
            <p class="step-index">4 / 5</p>
            <p class="setting-intent">Paste this from Kick</p>
            <h1>Client ID</h1>
            <label class="single-field">
              <span>Client ID</span>
              <input id="client-id-input" name="client_id" value="%4" placeholder="Paste Client ID here" spellcheck="false" required>
            </label>
            <div class="step-actions">
              <button type="button" class="secondary" data-step-target="2">Back</button>
              <button type="button" class="primary" data-next-step data-require="client-id-input">Next</button>
            </div>
          </div>
        </section>
        <section class="wizard-step">
          <div class="step-screen">
            <p class="step-index">5 / 5</p>
            <p class="setting-intent">Paste this from Kick</p>
            <h1>Client Secret</h1>
            <label class="single-field">
              <span>Client Secret</span>
              <input id="client-secret-input" type="password" name="client_secret" value="%5" placeholder="Paste Client Secret here" spellcheck="false" required>
            </label>
            <div class="step-actions">
              <button type="button" class="secondary" data-prev-step>Back</button>
              <button type="submit" class="primary">Continue to Kick</button>
            </div>
          </div>
        </section>
      </div>
    </div>
  </form>
</main>)KICK")
                    .arg(errorMarkup, REDIRECT_URL.toHtmlEscaped(),
                         KICK_DEVELOPER_URL.toString().toHtmlEscaped(),
                         clientID.toHtmlEscaped(),
                         clientSecret.toHtmlEscaped());

    auto scripts = QStringLiteral(R"(
<script>
const totalSteps = document.querySelectorAll('.wizard-step').length;
let currentStep = 0;
let copyResetTimer = null;
const wizardTrack = document.getElementById('wizard-track');

function setStep(nextStep) {
  currentStep = Math.max(0, Math.min(totalSteps - 1, nextStep));
  wizardTrack.style.transform = 'translateX(-' + (currentStep * 100) + '%)';
}

document.querySelectorAll('[data-step-target]').forEach((button) => {
  button.addEventListener('click', () => {
    const target = Number.parseInt(button.dataset.stepTarget ?? '', 10);
    if (!Number.isNaN(target)) {
      setStep(target);
    }
  });
});

document.querySelectorAll('[data-next-step]').forEach((button) => {
  button.addEventListener('click', () => {
    const requiredId = button.dataset.require;
    if (requiredId) {
      const requiredInput = document.getElementById(requiredId);
      if (requiredInput && !requiredInput.value.trim()) {
        requiredInput.reportValidity();
        requiredInput.focus();
        return;
      }
    }

    setStep(currentStep + 1);
  });
});

document.querySelectorAll('[data-prev-step]').forEach((button) => {
  button.addEventListener('click', () => setStep(currentStep - 1));
});

const clientIdInput = document.getElementById('client-id-input');
if (clientIdInput) {
  clientIdInput.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') {
      event.preventDefault();
      const nextButton = document.querySelector('[data-require="client-id-input"]');
      if (nextButton) {
        nextButton.click();
      }
    }
  });
}

function showCopied() {
  const button = document.getElementById('copy-button');
  button.classList.add('copied');
  button.textContent = 'Copied';

  if (copyResetTimer) {
    clearTimeout(copyResetTimer);
  }

  copyResetTimer = setTimeout(() => {
    button.classList.remove('copied');
    button.textContent = 'Copy redirect URL';
  }, 1600);
}

async function copyRedirect() {
  const text = document.getElementById('redirect-uri').textContent;
  if (navigator.clipboard && navigator.clipboard.writeText) {
    try {
      await navigator.clipboard.writeText(text);
      showCopied();
      return;
    } catch (error) {
    }
  }

  const input = document.createElement('input');
  input.value = text;
  document.body.appendChild(input);
  input.select();
  document.execCommand('copy');
  document.body.removeChild(input);
  showCopied();
}

setStep(0);
</script>)");

    return renderPage(u"Kick Login"_s, body, scripts);
}

QByteArray renderRedirectPage(const QUrl &authURL)
{
    auto encodedAuthUrl = authURL.toString(QUrl::FullyEncoded).toHtmlEscaped();
    auto body = QStringLiteral(R"(
<main class="card">
  <p class="eyebrow">Kick Login</p>
  <h1>Redirecting to Kick</h1>
  <p class="status">Mergerino has your local app credentials and is sending you to Kick now.</p>
  <div class="actions">
    <a id="continue-link" class="link-button primary" href="%1">Continue to Kick</a>
    <a class="link-button secondary" href="/">Back</a>
  </div>
</main>)")
                    .arg(encodedAuthUrl);

    auto scripts = QStringLiteral(R"(
<script>
const continueLink = document.getElementById('continue-link');
window.location.replace(continueLink.href);
</script>)");

    return renderPage(u"Redirecting to Kick"_s, body, scripts);
}

QByteArray renderFinishingPage()
{
    auto body = QStringLiteral(R"(
<main class="card">
  <p class="eyebrow">Kick Login</p>
  <h1>Finishing sign-in</h1>
  <p id="status" class="status">Mergerino is exchanging your Kick authorization code.</p>
  <div class="actions">
    <a id="retry-link" class="link-button secondary" href="/" hidden>Start over</a>
  </div>
  <p id="detail" class="footnote">You can leave this tab open while Mergerino finishes the login.</p>
</main>)");

    auto scripts = QStringLiteral(R"(
<script>
const statusNode = document.getElementById('status');
const detailNode = document.getElementById('detail');
const retryLink = document.getElementById('retry-link');

async function pollStatus() {
  try {
    const response = await fetch('/status?t=' + Date.now(), { cache: 'no-store' });
    const data = await response.json();

    statusNode.textContent = data.message;

    if (data.state === 'success') {
      detailNode.textContent = 'Kick login completed. You can close this tab.';
      retryLink.hidden = true;
      setTimeout(() => window.close(), 800);
      return;
    }

    if (data.state === 'error') {
      detailNode.textContent = 'Kick login did not finish successfully.';
      retryLink.hidden = false;
      return;
    }
  } catch (error) {
    detailNode.textContent = 'Still waiting for Mergerino to finish the login.';
  }

  setTimeout(pollStatus, 1000);
}

pollStatus();
</script>)");

    return renderPage(u"Finishing Kick Login"_s, body, scripts);
}

QByteArray renderNotFoundPage()
{
    auto body = QStringLiteral(R"(
<main class="card">
  <p class="eyebrow">Kick Login</p>
  <h1>Page not found</h1>
  <p class="status">That local login page does not exist.</p>
  <div class="actions">
    <a class="link-button secondary" href="/">Back to the Kick login flow</a>
  </div>
</main>)");
    return renderPage(u"Kick Login"_s, body);
}

class AuthSession : public QObject
{
public:
    AuthSession(QString clientID, QString clientSecret,
                std::function<void()> onAuthenticated,
                QObject *parent = nullptr)
        : QObject(parent)
        , clientID_(std::move(clientID))
        , clientSecret_(std::move(clientSecret))
        , authParams_(startAuthSession())
        , onAuthenticated_(std::move(onAuthenticated))
    {
        this->server_ = new HttpServer(SERVER_PORT, this);
        this->server_->setHandler([this](const HttpServer::Request &request) {
            return this->handleRequest(request);
        });
    }

    void begin()
    {
        this->openBrowser();
    }

private:
    void openBrowser()
    {
        if (QDesktopServices::openUrl(LOCAL_LOGIN_URL))
        {
            this->setBrowserState(
                u"pending"_s,
                u"Complete the Kick sign-in flow in your browser."_s);
            return;
        }

        qCWarning(chatterinoKick)
            << "Mergerino could not open the local Kick login URL";
        this->setBrowserState(
            u"error"_s,
            u"Mergerino could not open your browser automatically."_s);
        this->deleteLater();
    }

    HttpServer::Response handleRequest(const HttpServer::Request &request)
    {
        const auto path = requestPath(request.target);

        if (path == u"/"_s)
        {
            if (request.method.compare(u"GET"_s, Qt::CaseInsensitive) != 0)
            {
                return {
                    .status = 405,
                    .body = renderWizardPage(
                        this->clientID_, this->clientSecret_,
                        u"Only GET requests are supported on this page."_s),
                    .contentType = HTML_CONTENT_TYPE,
                };
            }

            const auto query = targetQuery(request.target);
            if (query.hasQueryItem(u"code"_s) || query.hasQueryItem(u"error"_s))
            {
                return this->handleKickCallback(query);
            }

            return {
                .status = 200,
                .body = renderWizardPage(this->clientID_, this->clientSecret_),
                .contentType = HTML_CONTENT_TYPE,
            };
        }

        if (path == u"/start"_s)
        {
            if (request.method.compare(u"POST"_s, Qt::CaseInsensitive) != 0)
            {
                return {
                    .status = 405,
                    .body = renderWizardPage(
                        this->clientID_, this->clientSecret_,
                        u"Start requests must use POST."_s),
                    .contentType = HTML_CONTENT_TYPE,
                };
            }

            return this->handleStart(formQuery(request.body));
        }

        if (path == u"/status"_s)
        {
            QJsonObject payload{
                {"state"_L1, this->browserState_},
                {"message"_L1, this->browserMessage_},
            };
            return {
                .status = 200,
                .body = QJsonDocument(payload).toJson(QJsonDocument::Compact),
                .contentType = JSON_CONTENT_TYPE,
            };
        }

        return {
            .status = 404,
            .body = renderNotFoundPage(),
            .contentType = HTML_CONTENT_TYPE,
        };
    }

    HttpServer::Response handleStart(const QUrlQuery &query)
    {
        const auto clientID = query.queryItemValue(u"client_id"_s).trimmed();
        const auto clientSecret =
            query.queryItemValue(u"client_secret"_s).trimmed();

        if (clientID.isEmpty() || clientSecret.isEmpty())
        {
            this->setBrowserState(
                u"error"_s,
                u"Kick app credentials are missing."_s);
            return {
                .status = 400,
                .body = renderWizardPage(
                    clientID, clientSecret,
                    u"Paste both the Kick Client ID and Client Secret before "
                    u"continuing."_s),
                .contentType = HTML_CONTENT_TYPE,
            };
        }

        this->clientID_ = clientID;
        this->clientSecret_ = clientSecret;
        this->authParams_ = startAuthSession();
        this->setBrowserState(
            u"pending"_s,
            u"Waiting for Kick authorization..."_s);

        return {
            .status = 200,
            .body = renderRedirectPage(this->buildAuthUrl()),
            .contentType = HTML_CONTENT_TYPE,
        };
    }

    HttpServer::Response handleKickCallback(const QUrlQuery &query)
    {
        if (query.hasQueryItem(u"error"_s))
        {
            const auto error = query.queryItemValue(u"error"_s).trimmed();
            const auto description =
                query.queryItemValue(u"error_description"_s).trimmed();
            auto message =
                description.isEmpty()
                    ? QString(u"Kick returned an authorization error: " % error)
                    : QString(u"Kick returned an authorization error: " %
                              description);
            this->setBrowserState(u"error"_s, message);
            return {
                .status = 400,
                .body = renderWizardPage(this->clientID_, this->clientSecret_,
                                         message),
                .contentType = HTML_CONTENT_TYPE,
            };
        }

        if (this->clientID_.isEmpty() || this->clientSecret_.isEmpty())
        {
            const auto message = u"Kick app credentials were missing. Start "
                                 u"the login flow again."_s;
            this->setBrowserState(u"error"_s, message);
            return {
                .status = 400,
                .body = renderWizardPage(QString{}, QString{}, message),
                .contentType = HTML_CONTENT_TYPE,
            };
        }

        if (!query.hasQueryItem(u"code"_s))
        {
            const auto message =
                u"Kick did not return an authorization code."_s;
            this->setBrowserState(u"error"_s, message);
            return {
                .status = 400,
                .body = renderWizardPage(this->clientID_, this->clientSecret_,
                                         message),
                .contentType = HTML_CONTENT_TYPE,
            };
        }

        if (query.queryItemValue(u"state"_s) != this->authParams_.state)
        {
            const auto message =
                u"Kick returned an invalid login state. Start again."_s;
            this->setBrowserState(u"error"_s, message);
            return {
                .status = 400,
                .body = renderWizardPage(this->clientID_, this->clientSecret_,
                                         message),
                .contentType = HTML_CONTENT_TYPE,
            };
        }

        this->setBrowserState(
            u"pending"_s,
            u"Exchanging the Kick authorization code..."_s);
        this->requestToken(query.queryItemValue(u"code"_s));

        return {
            .status = 200,
            .body = renderFinishingPage(),
            .contentType = HTML_CONTENT_TYPE,
        };
    }

    QUrl buildAuthUrl() const
    {
        QUrlQuery query{
            {"response_type", "code"},
            {"client_id", this->clientID_},
            {"redirect_uri", REDIRECT_URL},
            {"scope", "user:read channel:read channel:write chat:write "
                      "moderation:ban moderation:chat_message:manage"},
            {"code_challenge", this->authParams_.codeChallenge},
            {"code_challenge_method", "S256"},
            {"state", this->authParams_.state},
        };
        return QUrl(u"https://id.kick.com/oauth/authorize?"_s %
                    query.toString(QUrl::FullyEncoded));
    }

    void requestToken(const QString &code)
    {
        QUrlQuery payload{
            {"grant_type", "authorization_code"},
            {"client_id", this->clientID_},
            {"client_secret", this->clientSecret_},
            {"redirect_uri", REDIRECT_URL},
            {"code_verifier", this->authParams_.codeVerifier},
            {"code", code},
        };
        NetworkRequest("https://id.kick.com/oauth/token",
                       NetworkRequestType::Post)
            .header("Content-Type", "application/x-www-form-urlencoded")
            .payload(payload.toString(QUrl::FullyEncoded).toUtf8())
            .caller(this)
            .onError([this](const NetworkResult &result) {
                auto error = formatAPIError(result);
                qCWarning(chatterinoKick) << "Getting token failed" << error;
                this->setBrowserState(u"error"_s, error);
            })
            .onSuccess([this](const NetworkResult &result) {
                this->setBrowserState(
                    u"pending"_s,
                    u"Loading the authenticated Kick account..."_s);
                this->getAuthenticatedUser(result.parseJson());
            })
            .execute();
    }

    void getAuthenticatedUser(const QJsonObject &tokenData)
    {
        qint64 expiresIn = 0;
        auto expiresInVal = tokenData["expires_in"_L1];
        if (expiresInVal.isString())
        {
            expiresIn = expiresInVal.toString().toLongLong();
        }
        else
        {
            expiresIn = expiresInVal.toInteger();
        }

        auto expiresAt = QDateTime::currentDateTimeUtc().addSecs(expiresIn);
        NetworkRequest("https://api.kick.com/public/v1/users")
            .header("Authorization",
                    u"Bearer " % tokenData["access_token"_L1].toString())
            .caller(this)
            .onError([this](const NetworkResult &result) {
                auto error = formatAPIError(result);
                qCWarning(chatterinoKick) << "Getting user failed" << error;
                this->setBrowserState(u"error"_s, error);
            })
            .onSuccess([this, tokenData,
                        expiresAt](const NetworkResult &result) {
                const auto obj = result.parseJson()
                                     .value("data"_L1)
                                     .toArray()
                                     .at(0)
                                     .toObject();
                KickAccountData data{
                    .username = obj["name"].toString(),
                    .userID =
                        static_cast<uint64_t>(obj["user_id"_L1].toInteger()),
                    .clientID = this->clientID_,
                    .clientSecret = this->clientSecret_,
                    .authToken = tokenData["access_token"_L1].toString(),
                    .refreshToken = tokenData["refresh_token"_L1].toString(),
                    .expiresAt = expiresAt,
                };
                data.save();
                getApp()->getAccounts()->kick.reloadUsers();
                getApp()->getAccounts()->kick.currentUsername = data.username;
                this->setBrowserState(
                    u"success"_s,
                    u"Kick account connected. You can close this tab."_s);
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

    HttpServer *server_{};
    QString clientID_;
    QString clientSecret_;
    AuthParams authParams_;
    std::function<void()> onAuthenticated_;
    QString browserState_ = u"pending"_s;
    QString browserMessage_ =
        u"Open the local Kick login page in your browser."_s;
};

void startKickLoginFlow(QWidget *parent, std::function<void()> onAuthenticated)
{
    QString clientID;
    QString clientSecret;
    auto currentAccount = getApp()->getAccounts()->kick.current();
    if (!currentAccount->isAnonymous())
    {
        clientID = currentAccount->clientID();
        clientSecret = currentAccount->clientSecret();
    }

    auto *session =
        new AuthSession(clientID, clientSecret, std::move(onAuthenticated),
                        parent ? static_cast<QObject *>(parent) : qApp);
    session->begin();
}

}  // namespace

namespace chatterino {

void KickLoginPage::startLoginFlow(QWidget *parent,
                                   std::function<void()> onAuthenticated)
{
    startKickLoginFlow(parent, std::move(onAuthenticated));
}

KickLoginPage::KickLoginPage()
{
    auto *root = new QVBoxLayout(this);

    auto *topLabel = new QLabel(
        "Kick sign-in now finishes in your browser. If this is your first "
        "Kick login, the browser page will walk you through the one-time Kick "
        "app setup before sending you to Kick.");
    topLabel->setWordWrap(true);
    root->addWidget(topLabel);

    auto *startButton = new QPushButton("Log in (Opens in browser)");
    startButton->setDefault(true);
    root->addWidget(startButton, 0, Qt::AlignLeft);
    root->addStretch(1);

    QObject::connect(startButton, &QPushButton::clicked, this, [this] {
        startKickLoginFlow(
            this,
            [window = QPointer<QWidget>(this->window())] {
                if (window)
                {
                    window->close();
                }
            });
    });
}

void KickLoginPage::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    // The default QFrame background in the fusion theme has very poor contrast
    // on links, because it's bright gray.
    painter.setBrush(getTheme()->window.background);
    painter.setPen({});
    painter.drawRect(this->rect());
}

}  // namespace chatterino
