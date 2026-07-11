// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

// INVARIANT: the InnerTube (youtubei) read path implemented in this file
// (browse, next, live_chat/get_live_chat, updated_metadata) must remain
// anonymous. Do not attach an account's OAuth token / Authorization header
// to any youtubei request here - that would move the risk of scraping
// InnerTube from IP/rate-limit level to account level. OAuth is only
// permitted on the official googleapis.com/youtube/v3 send/moderate calls
// in this file (postLiveChatMessage, postLiveChatBan, deleteMessage, etc.),
// which are a separate, user-initiated write path. CI enforces this via the
// "TOS-03 guard" step in .github/workflows/build.yml.

#include "providers/youtube/YouTubeLiveChat.hpp"

#include "Application.hpp"
#include "common/LinkParser.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/links/LinkResolver.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "providers/youtube/YouTubeParsing.hpp"
#include "util/GuardedCallback.hpp"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStringBuilder>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace {

using namespace chatterino;

QString trimmedSource(QString value)
{
    return value.trimmed();
}

QString formatGoogleError(const NetworkResult &result)
{
    const auto json = result.parseJson();
    QString error;

    const auto errorValue = json["error"];
    if (errorValue.isObject())
    {
        error = errorValue.toObject()["message"].toString();
    }
    else
    {
        error = json["error_description"].toString(errorValue.toString());
    }

    if (!error.isEmpty())
    {
        return QStringLiteral("Error: %1 (%2)")
            .arg(error, result.formatError());
    }
    return QStringLiteral("Error: %1 (no further information)")
        .arg(result.formatError());
}

// Whether a failed moderation call definitively means the current account
// lacks moderator rights on this live chat. Google reports these as 401/403;
// insufficient-permission cases occasionally surface as 400 whose error
// reasons carry a permission/forbidden marker. Timeouts (no HTTP status),
// 429s and 5xx responses are transient and must not hide the mod tools for
// the rest of the stream.
bool isModerationAuthorizationFailure(const NetworkResult &result)
{
    const auto status = result.status();
    if (!status)
    {
        return false;
    }

    if (*status == 401 || *status == 403)
    {
        return true;
    }

    if (*status == 400)
    {
        const auto errors =
            result.parseJson()["error"].toObject()["errors"].toArray();
        const auto reasons = QString::fromUtf8(
                                 QJsonDocument(errors).toJson(
                                     QJsonDocument::Compact))
                                 .toLower();
        return reasons.contains(QStringLiteral("permission")) ||
               reasons.contains(QStringLiteral("forbidden"));
    }

    return false;
}

QString liveChatBanCacheKey(const QString &liveChatId, const QString &channelId)
{
    return liveChatId + QChar(u'\n') + channelId;
}

QString moderationDisplayName(const QString &displayName,
                              const QString &channelId)
{
    return displayName.trimmed().isEmpty() ? channelId : displayName.trimmed();
}

QString trimTrailingSlash(QString value)
{
    while (value.size() > 1 && value.endsWith('/'))
    {
        value.chop(1);
    }

    return value;
}

QJsonObject makeYoutubeClientContext(const QString &clientVersion)
{
    return {
        {"clientName", "WEB"},
        {"clientVersion", clientVersion},
        {"hl", "en"},
        {"gl", "US"},
    };
}

const QString YOUTUBE_BOOTSTRAP_URL = "https://www.youtube.com/embed/jNQXAC9IVRw";
const QColor YOUTUBE_PLATFORM_ACCENT(255, 48, 64, 60);
const QColor YOUTUBE_MEMBER_NAME_COLOR(43, 166, 64);
constexpr int YOUTUBE_RECONNECT_DELAY_MS = 3000;
// Cadence for polling resolveVideoId on a channel that's currently offline.
// Was using YOUTUBE_RECONNECT_DELAY_MS (3s) which spams the YouTube /next
// endpoint and the log file every few seconds for every offline channel
// in the user's layout. 30s catches a stream coming online fast enough
// while cutting steady-state HTTP and logging by 10x.
constexpr int YOUTUBE_OFFLINE_POLL_DELAY_MS = 30000;
constexpr int YOUTUBE_BLOCKED_RETRY_DELAY_MS = 60000;
constexpr int YOUTUBE_SESSION_REFRESH_MS = 10 * 60 * 1000;
constexpr int YOUTUBE_HEALTH_CHECK_INTERVAL_MS = 10000;
constexpr int YOUTUBE_STALL_TIMEOUT_MS = 30000;
// /updated_metadata polling cadence. Long enough to be cheap (one extra POST
// per video per cycle), short enough that the merged-tab viewer count tracks
// the YouTube watch page within a reasonable window.
constexpr int YOUTUBE_VIEWER_COUNT_REFRESH_MS = 15000;
constexpr int YOUTUBE_POLL_REFRESH_FALLBACK_DELAY_MS = 1000;
constexpr int YOUTUBE_POLL_REFRESH_FALLBACK_LIMIT = 2;

std::vector<std::pair<QByteArray, QByteArray>> youtubeHeaders()
{
    return {
        {"User-Agent",
         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
         "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"},
        {"Accept-Language", "en-US,en;q=0.9"},
        {"Cookie", "CONSENT=YES+cb.20210328-17-p0.en+FX+111; SOCS=CAI"},
        {"Origin", "https://www.youtube.com"},
        {"Referer", "https://www.youtube.com/"},
    };
}

bool appendSimpleWords(MessageBuilder &builder, const QString &text)
{
    bool appended = false;
    for (auto word : QStringView(text).tokenize(u' ', Qt::SkipEmptyParts))
    {
        builder.addWordFromUserMessage(word);
        appended = true;
    }

    return appended;
}

QString lowercaseDisplayedLinkText(const QString &text)
{
    const auto source = QStringView{text};
    const auto parsed = linkparser::parse(source);
    if (!parsed)
    {
        return text;
    }

    QString result;
    result.reserve(text.size());
    if (parsed->hasPrefix(source))
    {
        result += parsed->prefix(source).toString();
    }
    result += parsed->protocol.toString();
    result += parsed->host.toString().toLower();
    result += parsed->rest.toString();
    if (parsed->hasSuffix(source))
    {
        result += parsed->suffix(source).toString();
    }
    return result;
}

bool appendYouTubeEndpointLink(MessageBuilder &builder, const QString &text,
                               const QJsonObject &run)
{
    const auto displayText = text.trimmed();
    if (displayText.isEmpty() || displayText.simplified().contains(' ') ||
        !linkparser::parse(QStringView{displayText}))
    {
        return false;
    }

    const auto targetUrl =
        youtubeNavigationEndpointUrl(run["navigationEndpoint"].toObject());
    if (targetUrl.isEmpty())
    {
        return false;
    }

    auto *element = builder.emplace<LinkElement>(
        LinkElement::Parsed{
            .lowercase = lowercaseDisplayedLinkText(displayText),
            .original = displayText,
        },
        targetUrl, MessageElementFlag::Text, MessageColor::Link);
    element->setTooltip(targetUrl);
    getApp()->getLinkResolver()->resolve(element->linkInfo());
    return true;
}

QString youtubeEmojiShortcut(const QJsonObject &emoji)
{
    const auto shortcuts = emoji["shortcuts"].toArray();
    if (shortcuts.isEmpty())
    {
        return {};
    }

    return shortcuts.first().toString().trimmed();
}

QString youtubeEmojiLabel(const QJsonObject &emoji)
{
    return emoji["image"]
        .toObject()["accessibility"]
        .toObject()["accessibilityData"]
        .toObject()["label"]
        .toString()
        .trimmed();
}

QString youtubeEmojiFallbackText(const QJsonObject &emoji)
{
    auto fallback = youtubeEmojiShortcut(emoji);
    if (!fallback.isEmpty())
    {
        return fallback;
    }

    fallback = emoji["emojiId"].toString().trimmed();
    if (!fallback.isEmpty())
    {
        return fallback;
    }

    return youtubeEmojiLabel(emoji);
}

QString normalizeYouTubeImageUrl(QString url)
{
    url = url.trimmed();
    if (url.startsWith("//"))
    {
        url.prepend("https:");
    }

    return url;
}

QString youtubeBestThumbnailUrl(const QJsonObject &image)
{
    const auto thumbnails = image["thumbnails"].toArray();

    QString bestUrl;
    int bestWidth = -1;
    for (const auto &thumbnailValue : thumbnails)
    {
        const auto thumbnail = thumbnailValue.toObject();
        const auto url = normalizeYouTubeImageUrl(thumbnail["url"].toString());
        if (url.isEmpty())
        {
            continue;
        }

        const auto width = thumbnail["width"].toInt();
        if (bestUrl.isEmpty() || width >= bestWidth)
        {
            bestUrl = url;
            bestWidth = width;
        }
    }

    return bestUrl;
}

QString youtubeEmojiImageUrl(const QJsonObject &emoji)
{
    return youtubeBestThumbnailUrl(emoji["image"].toObject());
}

EmotePtr makeYouTubeEmojiEmote(const QJsonObject &emoji)
{
    const auto imageUrl = youtubeEmojiImageUrl(emoji);
    if (imageUrl.isEmpty())
    {
        return nullptr;
    }

    auto name = youtubeEmojiFallbackText(emoji);
    if (name.isEmpty())
    {
        name = QStringLiteral("YouTube Emoji");
    }

    const auto label = youtubeEmojiLabel(emoji);
    auto tooltip = name.toHtmlEscaped();
    if (!label.isEmpty() && label != name)
    {
        tooltip += QStringLiteral("<br>") + label.toHtmlEscaped();
    }
    tooltip += QStringLiteral("<br>YouTube Emoji");

    auto id = emoji["emojiId"].toString().trimmed();
    if (id.isEmpty())
    {
        id = imageUrl;
    }

    return std::make_shared<const Emote>(Emote{
        .name = {std::move(name)},
        .images = ImageSet(Image::fromAutoscaledUrl({imageUrl}, 28)),
        .tooltip = {std::move(tooltip)},
        .id = {std::move(id)},
    });
}

bool isCustomYouTubeEmoji(const QJsonObject &emoji)
{
    // Channel/member emoji are flagged isCustomEmoji; standard unicode emoji
    // are not. Only the custom ones have channel-specific :shortcut: names
    // worth completing - unicode shortcuts already come from the Emoji
    // provider, so completing them here would just duplicate those entries.
    return emoji["isCustomEmoji"].toBool();
}

// Record a custom (channel/member) chat emoji into the per-source completion
// map so emote autocomplete can offer its :shortcut:. Dedupes by shortcut (the
// EmoteMap key); first sighting wins. Bounded so a long session on a channel
// with a large emoji set can't grow the map without limit. No-op when there's
// no sink, the emote failed to build, or the emoji is a standard unicode one.
void accumulateCustomYouTubeEmoji(EmoteMap *customEmoteSink,
                                  const QJsonObject &emoji,
                                  const EmotePtr &emote)
{
    constexpr std::size_t maxAccumulatedEmotes = 512;

    if (customEmoteSink == nullptr || !emote || !isCustomYouTubeEmoji(emoji))
    {
        return;
    }

    const auto shortcut = youtubeEmojiShortcut(emoji);
    if (shortcut.isEmpty() || !shortcut.startsWith(u':'))
    {
        return;
    }

    const EmoteName name{shortcut};
    if (customEmoteSink->contains(name) ||
        customEmoteSink->size() >= maxAccumulatedEmotes)
    {
        return;
    }

    customEmoteSink->emplace(name, emote);
}

QString youtubeAccessibilityLabel(const QJsonObject &object)
{
    return object["accessibility"]
        .toObject()["accessibilityData"]
        .toObject()["label"]
        .toString()
        .trimmed();
}

QString youtubeAuthorBadgeTooltip(const QJsonObject &badgeRenderer)
{
    auto tooltip = badgeRenderer["tooltip"].toString().trimmed();
    if (!tooltip.isEmpty())
    {
        return tooltip;
    }

    tooltip = youtubeAccessibilityLabel(badgeRenderer);
    if (!tooltip.isEmpty())
    {
        return tooltip;
    }

    return youtubeAccessibilityLabel(
        badgeRenderer["customThumbnail"].toObject());
}

bool isYouTubeMembershipBadgeRenderer(const QJsonObject &badgeRenderer)
{
    if (badgeRenderer.isEmpty())
    {
        return false;
    }

    if (!badgeRenderer["customThumbnail"]
             .toObject()["thumbnails"]
             .toArray()
             .isEmpty())
    {
        return true;
    }

    const auto iconType =
        badgeRenderer["icon"].toObject()["iconType"].toString();
    if (iconType.contains(QStringLiteral("sponsor"), Qt::CaseInsensitive) ||
        iconType.contains(QStringLiteral("member"), Qt::CaseInsensitive))
    {
        return true;
    }

    const auto tooltip = youtubeAuthorBadgeTooltip(badgeRenderer);
    return tooltip.contains(QStringLiteral("sponsor"), Qt::CaseInsensitive) ||
           tooltip.contains(QStringLiteral("member"), Qt::CaseInsensitive) ||
           tooltip.contains(QStringLiteral("membership"),
                            Qt::CaseInsensitive);
}

EmotePtr makeYouTubeMembershipBadgeEmote(const QJsonObject &badgeRenderer)
{
    // YouTube sends channel membership badges as custom thumbnails.
    // Built-in badges like moderator/owner are icon-based instead.
    const auto imageUrl =
        youtubeBestThumbnailUrl(badgeRenderer["customThumbnail"].toObject());
    if (imageUrl.isEmpty())
    {
        return nullptr;
    }

    auto tooltip = youtubeAuthorBadgeTooltip(badgeRenderer);
    if (tooltip.isEmpty())
    {
        tooltip = QStringLiteral("YouTube Member");
    }

    return std::make_shared<const Emote>(Emote{
        .name = {tooltip},
        .images = ImageSet(Image::fromAutoscaledUrl({imageUrl}, 18)),
        .tooltip = {tooltip},
        .id = {imageUrl},
    });
}

void appendYouTubeMembershipBadges(MessageBuilder &builder,
                                   const QJsonObject &authorSource)
{
    for (const auto &badgeValue : authorSource["authorBadges"].toArray())
    {
        const auto badgeRenderer =
            badgeValue.toObject()["liveChatAuthorBadgeRenderer"].toObject();
        if (badgeRenderer.isEmpty())
        {
            continue;
        }

        if (const auto badge = makeYouTubeMembershipBadgeEmote(badgeRenderer))
        {
            builder.emplace<BadgeElement>(
                badge, MessageElementFlag::BadgeSubscription);
        }
    }
}

bool youtubeAuthorHasMembershipBadge(const QJsonObject &authorSource)
{
    for (const auto &badgeValue : authorSource["authorBadges"].toArray())
    {
        const auto badgeRenderer =
            badgeValue.toObject()["liveChatAuthorBadgeRenderer"].toObject();
        if (isYouTubeMembershipBadgeRenderer(badgeRenderer))
        {
            return true;
        }
    }

    return false;
}

std::optional<QColor> parseYouTubeColor(const QJsonValue &value)
{
    bool ok = false;
    qulonglong rawColor = 0;

    if (value.isString())
    {
        const auto text = value.toString().trimmed();
        if (text.isEmpty())
        {
            return std::nullopt;
        }

        const auto namedColor = QColor::fromString(text);
        if (namedColor.isValid())
        {
            return namedColor;
        }

        rawColor = text.toULongLong(&ok, 0);
    }
    else if (value.isDouble())
    {
        const auto integer = value.toInteger(-1);
        if (integer < 0)
        {
            return std::nullopt;
        }
        rawColor = static_cast<qulonglong>(integer);
        ok = true;
    }

    if (!ok || rawColor > 0xFFFFFFFFULL)
    {
        return std::nullopt;
    }

    const auto rgb = static_cast<QRgb>(rawColor);
    if (rawColor <= 0x00FFFFFFULL)
    {
        return QColor::fromRgb(rgb);
    }

    return QColor::fromRgba(rgb);
}

std::optional<QColor> youtubeAuthorNameColor(const QJsonObject &authorSource)
{
    if (auto color = parseYouTubeColor(authorSource["authorNameTextColor"]))
    {
        return color;
    }

    if (auto color = parseYouTubeColor(authorSource["authorTextColor"]))
    {
        return color;
    }

    const auto authorName = authorSource["authorName"].toObject();
    if (auto color = parseYouTubeColor(authorName["textColor"]))
    {
        return color;
    }

    for (const auto &runValue : authorName["runs"].toArray())
    {
        if (auto color = parseYouTubeColor(runValue.toObject()["textColor"]))
        {
            return color;
        }
    }

    if (youtubeAuthorHasMembershipBadge(authorSource))
    {
        return YOUTUBE_MEMBER_NAME_COLOR;
    }

    return std::nullopt;
}

bool appendYouTubeText(MessageBuilder &builder, const QJsonValue &value,
                       EmoteMap *customEmoteSink = nullptr)
{
    const auto object = value.toObject();
    if (object.contains("simpleText"))
    {
        return appendSimpleWords(builder, object["simpleText"].toString());
    }

    bool appended = false;
    for (const auto &runValue : object["runs"].toArray())
    {
        const auto run = runValue.toObject();
        if (run.contains("text"))
        {
            if (appendYouTubeEndpointLink(builder, run["text"].toString(), run))
            {
                appended = true;
                continue;
            }
            appended =
                appendSimpleWords(builder, run["text"].toString()) || appended;
            continue;
        }

        if (!run.contains("emoji"))
        {
            continue;
        }

        const auto emoji = run["emoji"].toObject();
        if (const auto emote = makeYouTubeEmojiEmote(emoji))
        {
            builder.emplace<EmoteElement>(emote, MessageElementFlag::Emote,
                                          builder.textColor());
            accumulateCustomYouTubeEmoji(customEmoteSink, emoji, emote);
            appended = true;
            continue;
        }

        appended =
            appendSimpleWords(builder, youtubeEmojiFallbackText(emoji)) ||
            appended;
    }

    return appended;
}

void appendYouTubeTextParts(MessageBuilder &builder,
                            const std::vector<QJsonValue> &parts,
                            const QString &fallbackText,
                            EmoteMap *customEmoteSink = nullptr)
{
    bool appended = false;
    for (const auto &part : parts)
    {
        appended =
            appendYouTubeText(builder, part, customEmoteSink) || appended;
    }

    if (!appended)
    {
        appendSimpleWords(builder, fallbackText);
    }
}

MessagePtr makeSystemStatusMessage(
    const QString &text,
    const std::optional<QColor> &platformAccentColor = std::nullopt)
{
    MessageBuilder builder;
    builder->flags.set(MessageFlag::System);
    builder->flags.set(MessageFlag::DoNotTriggerNotification);
    builder->messageText = text;
    builder->searchText = text;
    builder->serverReceivedTime = QDateTime::currentDateTime();
    builder.emplace<TimestampElement>(builder->serverReceivedTime.time());
    builder.emplace<TextElement>(text, MessageElementFlag::Text,
                                 MessageColor::System);
    builder->platformAccentColor = platformAccentColor;
    return builder.release();
}

int cappedYouTubePollDelay(int timeoutMs)
{
    constexpr int minimumDelayMs = 1000;
    constexpr int maximumDelayMs = 30000;

    if (timeoutMs <= 0)
    {
        return minimumDelayMs;
    }

    return std::clamp(timeoutMs, minimumDelayMs, maximumDelayMs);
}

int adjustedYouTubePollDelay(int timeoutMs, qint64 /* requestElapsedMs */,
                             int deliveredMessageCount,
                             int activePollStreak)
{
    const auto serverDelay = cappedYouTubePollDelay(timeoutMs);
    if (deliveredMessageCount <= 0)
    {
        constexpr int idleDelayCapMs = 3000;
        return std::min(serverDelay, idleDelayCapMs);
    }

    int busyDelayCapMs = 1000;
    if (deliveredMessageCount >= 25)
    {
        busyDelayCapMs = 1000;
    }
    else if (deliveredMessageCount >= 10 || activePollStreak >= 2)
    {
        busyDelayCapMs = 1000;
    }

    return std::min(serverDelay, busyDelayCapMs);
}

QString formatYouTubeUptime(const QDateTime &startedAt)
{
    if (!startedAt.isValid())
    {
        return {};
    }

    const auto seconds = startedAt.secsTo(QDateTime::currentDateTimeUtc());
    if (seconds < 0)
    {
        return {};
    }

    return QString::number(seconds / 3600) % u"h " %
           QString::number(seconds % 3600 / 60) % u"m";
}

}  // namespace

namespace chatterino {

YouTubeLiveChat::YouTubeLiveChat(QString streamUrl)
    : streamUrl_(std::move(streamUrl))
    , lifetimeGuard_(std::make_shared<bool>(true))
{
}

YouTubeLiveChat::~YouTubeLiveChat()
{
    this->stop();
}

namespace {

// Process-wide registry so MergedChannels with the same youtube source share
// one YouTubeLiveChat instance (one resolver, one poll loop, one announce
// ladder). Strong refs on purpose: the layout-restore path creates and
// destroys throwaway MergedChannels in rapid succession; with weak_ptrs the
// instance would die between drop and re-pickup, churning multiple resolver
// chains. Strong refs keep one instance per unique source for the app's
// lifetime, which is what we want anyway.
std::mutex g_youtubeRegistryMutex;
std::unordered_map<QString, std::shared_ptr<YouTubeLiveChat>>
    g_youtubeRegistry;

}  // namespace

std::shared_ptr<YouTubeLiveChat> YouTubeLiveChat::getOrCreateShared(
    const QString &source)
{
    // Normalize so @foo, https://youtube.com/@foo, and the channel-id form all
    // collapse to the same registry key; otherwise dedupe is broken for the
    // most common URL-vs-handle case and we silently create duplicate
    // instances for the same channel.
    QString key = normalizeSource(source);
    if (key.isEmpty())
    {
        key = source.trimmed();
    }
    std::lock_guard<std::mutex> lock(g_youtubeRegistryMutex);

    auto it = g_youtubeRegistry.find(key);
    if (it != g_youtubeRegistry.end())
    {
        return it->second;
    }

    auto shared = std::make_shared<YouTubeLiveChat>(key);
    g_youtubeRegistry.emplace(key, shared);
    shared->start();
    return shared;
}

void YouTubeLiveChat::start()
{
    if (this->running_)
    {
        return;
    }

    if (!this->lifetimeGuard_)
    {
        this->lifetimeGuard_ = std::make_shared<bool>(true);
    }

    this->running_ = true;
    this->videoId_.clear();
    this->apiKey_.clear();
    this->clientVersion_.clear();
    this->visitorData_.clear();
    this->continuation_.clear();
    this->statusText_.clear();
    this->liveTitle_.clear();
    this->liveOwnerChannelId_.clear();
    this->activeLiveChatVideoId_.clear();
    this->activeLiveChatId_.clear();
    this->resetModeratorPrivileges();
    this->liveStartedAt_ = {};
    this->liveViewerCount_ = 0;
    this->seenMessageIds_.clear();
    this->failureReported_ = false;
    this->skipInitialBacklog_ = false;
    this->activePollStreak_ = 0;
    this->consecutiveRecoveries_ = 0;
    this->pollRefreshFallbackCount_ = 0;
    this->liveChatSessionRefreshTimer_.invalidate();
    this->liveChatProgressTimer_.invalidate();
    this->setLive(false);
    this->scheduleHealthCheck(YOUTUBE_HEALTH_CHECK_INTERVAL_MS);
    this->resolveVideoId();
}

void YouTubeLiveChat::stop()
{
    this->running_ = false;
    this->activePollStreak_ = 0;
    this->pollRefreshFallbackCount_ = 0;
    this->liveChatSessionRefreshTimer_.invalidate();
    this->liveChatProgressTimer_.invalidate();
    this->lifetimeGuard_.reset();
}

void YouTubeLiveChat::sendMessage(const QString &message)
{
    const auto text = message.trimmed();
    if (text.isEmpty())
    {
        return;
    }

    if (!this->running_ || !this->live_ || this->videoId_.isEmpty())
    {
        this->systemMessageReceived.invoke(
            makeSystemStatusMessage("YouTube live chat is not ready yet."));
        return;
    }

    auto *accounts = getApp()->getAccounts();
    if (!accounts->youtube.isLoggedIn())
    {
        this->systemMessageReceived.invoke(
            makeSystemStatusMessage("Log in to YouTube to send merged chat."));
        return;
    }

    auto account = accounts->youtube.current();
    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    account->ensureFreshToken(
        [this, weak, account, text](bool ok) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            if (!ok)
            {
                this->systemMessageReceived.invoke(makeSystemStatusMessage(
                    "YouTube login expired. Log in again to send chat."));
                return;
            }

            this->ensureActiveLiveChatId(
                account,
                [this, text](std::shared_ptr<YouTubeAccount> account,
                             QString liveChatId) mutable {
                    this->postLiveChatMessage(std::move(account),
                                              std::move(liveChatId), text);
                },
                QStringLiteral("YouTube chat send"));
        });
}

void YouTubeLiveChat::deleteMessage(const QString &messageId)
{
    const auto id = messageId.trimmed();
    if (id.isEmpty())
    {
        return;
    }

    auto *accounts = getApp()->getAccounts();
    if (!accounts->youtube.isLoggedIn())
    {
        this->systemMessageReceived.invoke(makeSystemStatusMessage(
            "Log in to YouTube to moderate merged chat."));
        return;
    }

    if (!this->hasModeratorPrivileges())
    {
        this->reportModerationToolsUnavailable();
        return;
    }

    auto account = accounts->youtube.current();
    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    account->ensureFreshToken(
        [this, weak, account, id](bool ok) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            if (!ok)
            {
                this->systemMessageReceived.invoke(makeSystemStatusMessage(
                    "YouTube login expired. Log in again to moderate chat."));
                return;
            }

            QUrl url("https://www.googleapis.com/youtube/v3/liveChat/messages");
            QUrlQuery query{{"id", id}};
            url.setQuery(query);

            NetworkRequest(url, NetworkRequestType::Delete)
                .header("Authorization",
                        QStringLiteral("Bearer ") + account->authToken())
                .timeout(20'000)
                .onSuccess([this, weak](const NetworkResult &) {
                    if (!weak.lock() || !this->running_)
                    {
                        return;
                    }

                    this->systemMessageReceived.invoke(makeSystemStatusMessage(
                        "Deleted YouTube message."));
                })
                .onError([this, weak](const NetworkResult &result) {
                    if (!weak.lock() || !this->running_)
                    {
                        return;
                    }

                    const auto error = formatGoogleError(result);
                    qCWarning(chatterinoYouTube)
                        << "Deleting YouTube live chat message failed"
                        << error;
                    this->reportModerationActionFailure(
                        QStringLiteral("delete YouTube message"), error,
                        result);
                })
                .execute();
        });
}

void YouTubeLiveChat::banUser(const QString &channelId,
                              const QString &displayName)
{
    const auto userChannelId = channelId.trimmed();
    if (userChannelId.isEmpty())
    {
        this->systemMessageReceived.invoke(makeSystemStatusMessage(
            "YouTube user channel ID is missing."));
        return;
    }

    auto *accounts = getApp()->getAccounts();
    if (!accounts->youtube.isLoggedIn())
    {
        this->systemMessageReceived.invoke(makeSystemStatusMessage(
            "Log in to YouTube to moderate merged chat."));
        return;
    }

    if (!this->hasModeratorPrivileges())
    {
        this->reportModerationToolsUnavailable();
        return;
    }

    auto account = accounts->youtube.current();
    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    account->ensureFreshToken(
        [this, weak, account, userChannelId, displayName](bool ok) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            if (!ok)
            {
                this->systemMessageReceived.invoke(makeSystemStatusMessage(
                    "YouTube login expired. Log in again to moderate chat."));
                return;
            }

            this->ensureActiveLiveChatId(
                account,
                [this, userChannelId, displayName](
                    std::shared_ptr<YouTubeAccount> account,
                    QString liveChatId) mutable {
                    this->postLiveChatBan(std::move(account),
                                          std::move(liveChatId), userChannelId,
                                          displayName, 0);
                },
                QStringLiteral("YouTube moderation"));
        });
}

void YouTubeLiveChat::timeoutUser(const QString &channelId,
                                  int durationSeconds,
                                  const QString &displayName)
{
    const auto userChannelId = channelId.trimmed();
    if (userChannelId.isEmpty())
    {
        this->systemMessageReceived.invoke(makeSystemStatusMessage(
            "YouTube user channel ID is missing."));
        return;
    }

    auto *accounts = getApp()->getAccounts();
    if (!accounts->youtube.isLoggedIn())
    {
        this->systemMessageReceived.invoke(makeSystemStatusMessage(
            "Log in to YouTube to moderate merged chat."));
        return;
    }

    if (!this->hasModeratorPrivileges())
    {
        this->reportModerationToolsUnavailable();
        return;
    }

    auto account = accounts->youtube.current();
    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    account->ensureFreshToken(
        [this, weak, account, userChannelId, displayName,
         durationSeconds](bool ok) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            if (!ok)
            {
                this->systemMessageReceived.invoke(makeSystemStatusMessage(
                    "YouTube login expired. Log in again to moderate chat."));
                return;
            }

            const auto seconds = std::max(durationSeconds, 1);
            this->ensureActiveLiveChatId(
                account,
                [this, userChannelId, displayName, seconds](
                    std::shared_ptr<YouTubeAccount> account,
                    QString liveChatId) mutable {
                    this->postLiveChatBan(std::move(account),
                                          std::move(liveChatId), userChannelId,
                                          displayName, seconds);
                },
                QStringLiteral("YouTube moderation"));
        });
}

void YouTubeLiveChat::unbanUser(const QString &channelId,
                                const QString &displayName)
{
    const auto userChannelId = channelId.trimmed();
    if (userChannelId.isEmpty())
    {
        this->systemMessageReceived.invoke(makeSystemStatusMessage(
            "YouTube user channel ID is missing."));
        return;
    }

    auto *accounts = getApp()->getAccounts();
    if (!accounts->youtube.isLoggedIn())
    {
        this->systemMessageReceived.invoke(makeSystemStatusMessage(
            "Log in to YouTube to moderate merged chat."));
        return;
    }

    if (!this->hasModeratorPrivileges())
    {
        this->reportModerationToolsUnavailable();
        return;
    }

    auto account = accounts->youtube.current();
    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    account->ensureFreshToken(
        [this, weak, account, userChannelId, displayName](bool ok) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            if (!ok)
            {
                this->systemMessageReceived.invoke(makeSystemStatusMessage(
                    "YouTube login expired. Log in again to moderate chat."));
                return;
            }

            this->ensureActiveLiveChatId(
                account,
                [this, userChannelId, displayName](
                    std::shared_ptr<YouTubeAccount> account,
                    QString liveChatId) mutable {
                    const auto key =
                        liveChatBanCacheKey(liveChatId, userChannelId);
                    const auto it = this->liveChatBanIds_.find(key);
                    if (it == this->liveChatBanIds_.end() || it->second.isEmpty())
                    {
                        this->systemMessageReceived.invoke(
                            makeSystemStatusMessage(
                                "YouTube unban needs a ban ID. Mergerino can "
                                "unban users it banned during this session."));
                        return;
                    }

                    this->deleteLiveChatBan(std::move(account), it->second,
                                            userChannelId, displayName);
                },
                QStringLiteral("YouTube moderation"));
        });
}

bool YouTubeLiveChat::isLive() const
{
    return this->live_;
}

bool YouTubeLiveChat::hasModeratorPrivileges() const
{
    if (!this->live_ || this->videoId_.isEmpty())
    {
        return false;
    }

    auto *accounts = getApp()->getAccounts();
    if (!accounts->youtube.isLoggedIn())
    {
        return false;
    }

    const auto account = accounts->youtube.current();
    const auto currentChannelId = account->channelID();
    if (currentChannelId.isEmpty())
    {
        return false;
    }

    if (this->moderationToolsDisabledForCurrentAccount())
    {
        return false;
    }

    return true;
}

const QString &YouTubeLiveChat::videoId() const
{
    return this->videoId_;
}

const QString &YouTubeLiveChat::statusText() const
{
    return this->statusText_;
}

const QString &YouTubeLiveChat::liveTitle() const
{
    return this->liveTitle_;
}

uint64_t YouTubeLiveChat::liveViewerCount() const
{
    return this->liveViewerCount_;
}

unsigned YouTubeLiveChat::viewerCount() const
{
    return static_cast<unsigned>(
        std::min<uint64_t>(this->liveViewerCount_,
                           std::numeric_limits<unsigned>::max()));
}

QString YouTubeLiveChat::liveUptime() const
{
    return formatYouTubeUptime(this->liveStartedAt_);
}

QString YouTubeLiveChat::previewThumbnailUrl() const
{
    if (this->videoId_.isEmpty())
    {
        return {};
    }

    return QString("https://i.ytimg.com/vi/%1/hqdefault_live.jpg")
        .arg(this->videoId_);
}

void YouTubeLiveChat::resolveVideoId()
{
    if (!this->running_)
    {
        return;
    }

    const auto sourceVideoId = maybeExtractVideoId(this->streamUrl_);
    auto source = this->resolvedSource();
    if (source.isEmpty() && !sourceVideoId.isEmpty())
    {
        this->resolveChannelIdFromVideoId(
            sourceVideoId, [this, sourceVideoId](QString channelId) {
                if (!this->running_)
                {
                    return;
                }

                auto joinSourceVideo = [this, sourceVideoId] {
                    if (!this->running_)
                    {
                        return;
                    }

                    this->videoId_ = sourceVideoId;
                    this->seenMessageIds_.clear();
                    this->fetchLiveChatPage();
                };

                if (channelId.isEmpty())
                {
                    joinSourceVideo();
                    return;
                }

                if (this->streamUrl_ != channelId)
                {
                    this->streamUrl_ = channelId;
                    this->sourceResolved.invoke(channelId);
                }

                this->probeLiveVideoIdFromSource(
                    channelId, [this, joinSourceVideo](QString videoId) {
                        if (!this->running_)
                        {
                            return;
                        }

                        if (videoId.isEmpty())
                        {
                            joinSourceVideo();
                            return;
                        }

                        this->videoId_ = std::move(videoId);
                        this->seenMessageIds_.clear();
                        this->fetchLiveChatPage();
                    });
            });
        return;
    }

    if (source.isEmpty())
    {
        this->setStatusText(
            "Enter a YouTube @handle or any video link from the desired channel.",
                            !this->failureReported_);
        this->failureReported_ = true;
        return;
    }

    this->resolveChannelIdFromSource(source, [this](QString channelId) {
        if (!this->running_)
        {
            return;
        }

        if (channelId.isEmpty())
        {
            this->setStatusText(
                "Couldn't resolve that YouTube source to a channel ID.",
                !this->failureReported_);
            this->failureReported_ = true;
            this->scheduleResolve(YOUTUBE_RECONNECT_DELAY_MS);
            return;
        }

        if (this->streamUrl_ != channelId)
        {
            this->streamUrl_ = channelId;
            this->sourceResolved.invoke(channelId);
        }

        this->probeLiveVideoIdFromSource(
            channelId, [this, channelId](QString videoId) {
                if (!this->running_)
                {
                    return;
                }

                if (videoId.isEmpty())
                {
                    this->waitForNextLive(
                        QString("Waiting for %1 to go live on YouTube.")
                            .arg(channelId),
                        YOUTUBE_RECONNECT_DELAY_MS);
                    return;
                }

                if (this->videoIdRecentlyFailed(videoId))
                {
                    qCDebug(chatterinoYouTube).nospace()
                        << "[" << this->streamUrl_
                        << "] resolveVideoId skipping recently-failed videoId="
                        << videoId << " - treating as offline";
                    this->waitForNextLive(
                        QString("Waiting for %1 to go live on YouTube.")
                            .arg(channelId),
                        YOUTUBE_RECONNECT_DELAY_MS);
                    return;
                }

                this->videoId_ = std::move(videoId);
                this->seenMessageIds_.clear();
                this->fetchLiveChatPage();
            });
    });
}

bool YouTubeLiveChat::videoIdRecentlyFailed(const QString &videoId) const
{
    if (videoId.isEmpty() || this->recentlyFailedVideoId_ != videoId ||
        !this->recentlyFailedTimer_.isValid())
    {
        return false;
    }
    const qint64 cooldown = std::min(
        recentlyFailedMaxWindowMs,
        recentlyFailedBaseWindowMs *
            (1LL << std::min(this->recentlyFailedStreak_ - 1, 8)));
    return this->recentlyFailedTimer_.elapsed() < cooldown;
}

void YouTubeLiveChat::resolveChannelIdFromSource(
    const QString &source, std::function<void(QString)> onResolved)
{
    const auto trimmed = source.trimmed();
    if (!this->running_ || trimmed.isEmpty())
    {
        onResolved({});
        return;
    }

    if (isLikelyChannelId(trimmed))
    {
        onResolved(trimmed);
        return;
    }

    const auto videoId = maybeExtractVideoId(trimmed);
    if (!videoId.isEmpty())
    {
        this->resolveChannelIdFromVideoId(videoId, std::move(onResolved));
        return;
    }

    if (trimmed.startsWith('@'))
    {
        this->resolveChannelIdFromHandle(trimmed, std::move(onResolved));
        return;
    }

    static const QRegularExpression handleLikeSourceRegex("^[A-Za-z0-9._-]+$");
    if (handleLikeSourceRegex.match(trimmed).hasMatch())
    {
        this->resolveChannelIdFromSearch(
            trimmed,
            [this, trimmed, onResolved = std::move(onResolved)](
                QString channelId) mutable {
                if (!this->running_)
                {
                    return;
                }

                if (!channelId.isEmpty())
                {
                    onResolved(std::move(channelId));
                    return;
                }

                this->resolveChannelIdFromHandle(QString("@%1").arg(trimmed),
                                                 std::move(onResolved));
            });
        return;
    }

    this->resolveChannelIdFromSearch(trimmed, std::move(onResolved));
}

void YouTubeLiveChat::resolveChannelIdFromHandle(
    const QString &handle, std::function<void(QString)> onResolved)
{
    const auto trimmed = handle.trimmed();
    if (!this->running_ || !trimmed.startsWith('@'))
    {
        onResolved({});
        return;
    }

    auto callback =
        std::make_shared<std::function<void(QString)>>(std::move(onResolved));
    const auto url = QString("https://www.youtube.com/%1").arg(trimmed);
    NetworkRequest(url.toStdString())
        .headerList(youtubeHeaders())
        .followRedirects(true)
        .timeout(15000)
        .onSuccess(guardedCallback(this->lifetimeGuard_,
                                   [this, callback, trimmed](
                                       const NetworkResult &result) {
            if (!this->running_)
            {
                return;
            }

            const auto html = QString::fromUtf8(result.getData());
            const auto channelId = extractVideoChannelId(html);
            if (!channelId.isEmpty())
            {
                (*callback)(channelId);
                return;
            }

            this->resolveChannelIdFromSearch(trimmed, [callback](QString id) {
                (*callback)(std::move(id));
            });
                                   }))
        .onError(guardedCallback(this->lifetimeGuard_,
                                 [this, callback, trimmed](NetworkResult) {
            if (!this->running_)
            {
                return;
            }

            this->resolveChannelIdFromSearch(trimmed, [callback](QString id) {
                (*callback)(std::move(id));
            });
                                 }))
        .execute();
}

void YouTubeLiveChat::resolveChannelIdFromVideoId(
    const QString &videoId, std::function<void(QString)> onResolved)
{
    if (!this->running_ || videoId.isEmpty())
    {
        onResolved({});
        return;
    }

    auto callback =
        std::make_shared<std::function<void(QString)>>(std::move(onResolved));
    NetworkRequest(
        QString("https://www.youtube.com/watch?v=%1").arg(videoId).toStdString())
        .headerList(youtubeHeaders())
        .followRedirects(true)
        .timeout(15000)
        .onSuccess(guardedCallback(this->lifetimeGuard_,
                                   [this, callback](
                                       const NetworkResult &result) {
            if (!this->running_)
            {
                return;
            }

            const auto html = QString::fromUtf8(result.getData());
            (*callback)(extractVideoChannelId(html));
                                   }))
        .onError(guardedCallback(this->lifetimeGuard_,
                                 [this, callback](NetworkResult) {
            if (!this->running_)
            {
                return;
            }

            (*callback)({});
                                 }))
        .execute();
}

void YouTubeLiveChat::resolveChannelIdFromSearch(
    const QString &source, std::function<void(QString)> onResolved)
{
    const auto searchSource = source.trimmed();
    if (!this->running_ || searchSource.isEmpty())
    {
        onResolved({});
        return;
    }

    QUrl url("https://www.youtube.com/results");
    QUrlQuery query;
    query.addQueryItem("search_query", searchSource);
    url.setQuery(query);

    auto callback =
        std::make_shared<std::function<void(QString)>>(std::move(onResolved));
    NetworkRequest(url.toString().toStdString())
        .headerList(youtubeHeaders())
        .followRedirects(true)
        .timeout(15000)
        .onSuccess(guardedCallback(this->lifetimeGuard_,
                                   [this, callback](
                                       const NetworkResult &result) {
            if (!this->running_)
            {
                return;
            }

            const auto html = QString::fromUtf8(result.getData());
            (*callback)(extractVideoChannelId(html));
                                   }))
        .onError(guardedCallback(this->lifetimeGuard_,
                                 [this, callback](NetworkResult) {
            if (!this->running_)
            {
                return;
            }

            (*callback)({});
                                 }))
        .execute();
}

void YouTubeLiveChat::fetchChannelAvatarUrl(
    const QString &source, std::function<void(QString)> onResolved)
{
    // Static, self-contained anonymous fetch. Normalize the user-entered
    // source down to a stable channel identifier, then load that channel's
    // page and pull the avatar image URL out of its <meta og:image> /
    // ytInitialData. Read-only path - youtubeHeaders() only, no account token.
    const auto normalized = normalizeSource(source);
    if (normalized.isEmpty())
    {
        // A bare watch/shorts URL or unrecognized source: no channel handle to
        // resolve here. Let the caller fall through to its next avatar stage.
        onResolved({});
        return;
    }

    QString path;
    if (normalized.startsWith('@'))
    {
        path = QStringLiteral("/") + normalized;
    }
    else if (isLikelyChannelId(normalized))
    {
        path = QStringLiteral("/channel/") + normalized;
    }
    else
    {
        // Legacy custom name / handle without the leading '@'. YouTube resolves
        // this via redirect; followRedirects(true) below lands on the channel.
        path = QStringLiteral("/") + normalized;
    }

    const auto url = QStringLiteral("https://www.youtube.com") + path;
    auto callback =
        std::make_shared<std::function<void(QString)>>(std::move(onResolved));
    NetworkRequest(url.toStdString())
        .headerList(youtubeHeaders())
        .followRedirects(true)
        .timeout(15000)
        .onSuccess([callback](const NetworkResult &result) {
            const auto html = QString::fromUtf8(result.getData());
            (*callback)(extractChannelAvatarUrl(html));
        })
        .onError([callback](const NetworkResult &) {
            (*callback)({});
        })
        .execute();
}

void YouTubeLiveChat::probeLiveVideoIdFromSource(
    const QString &source, std::function<void(QString)> onResolved)
{
    if (!this->running_ || source.isEmpty())
    {
        onResolved({});
        return;
    }

    const auto livePath = sourceLivePath(source);
    const auto streamsPath = sourceStreamsPath(source);
    if (livePath.isEmpty() && streamsPath.isEmpty())
    {
        onResolved({});
        return;
    }

    auto callback =
        std::make_shared<std::function<void(QString)>>(std::move(onResolved));

    auto finishEmpty = [callback] {
        (*callback)({});
    };

    auto tryStreams = [this, streamsPath, finishEmpty, callback] {
        if (streamsPath.isEmpty())
        {
            finishEmpty();
            return;
        }

        this->probeLiveVideoIdFromPath(streamsPath,
                                       [this, finishEmpty, callback](
                                           QString videoId) {
            if (!this->running_)
            {
                return;
            }

            if (videoId.isEmpty())
            {
                finishEmpty();
                return;
            }

            (*callback)(std::move(videoId));
        });
    };

    auto tryEmbed = [this, source, tryStreams, callback] {
        if (!isLikelyChannelId(source))
        {
            tryStreams();
            return;
        }

        this->probeLiveVideoIdFromEmbed(
            source, [this, tryStreams, callback](QString videoId) {
                if (!this->running_)
                {
                    return;
                }

                if (!videoId.isEmpty())
                {
                    (*callback)(std::move(videoId));
                    return;
                }

                tryStreams();
            });
    };

    auto tryBrowse = [this, source, tryEmbed, callback] {
        if (!isLikelyChannelId(source))
        {
            tryEmbed();
            return;
        }

        this->probeLiveVideoIdFromBrowse(
            source, [this, tryEmbed, callback](QString videoId) {
                    if (!this->running_)
                    {
                        return;
                    }

                    if (!videoId.isEmpty())
                    {
                        (*callback)(std::move(videoId));
                        return;
                    }

                    tryEmbed();
                });
    };

    auto tryLivePath = [this, source, livePath, tryStreams, tryBrowse, callback] {
        if (livePath.isEmpty())
        {
            if (isLikelyChannelId(source))
            {
                tryBrowse();
                return;
            }

            tryStreams();
            return;
        }

        this->probeLiveVideoIdFromPath(
            livePath, [this, source, tryStreams, tryBrowse, callback](
                          QString videoId) {
                if (!this->running_)
                {
                    return;
                }

                if (!videoId.isEmpty())
                {
                    (*callback)(std::move(videoId));
                    return;
                }

                if (isLikelyChannelId(source))
                {
                    tryBrowse();
                    return;
                }

                tryStreams();
            });
    };

    tryLivePath();
}

void YouTubeLiveChat::probeLiveVideoIdFromEmbed(
    const QString &channelId, std::function<void(QString)> onResolved)
{
    if (!this->running_ || !isLikelyChannelId(channelId))
    {
        onResolved({});
        return;
    }

    QUrl url("https://www.youtube.com/embed/live_stream");
    QUrlQuery query;
    query.addQueryItem("channel", channelId);
    url.setQuery(query);

    auto callback =
        std::make_shared<std::function<void(QString)>>(std::move(onResolved));
    NetworkRequest(url.toString().toStdString())
        .headerList(youtubeHeaders())
        .followRedirects(true)
        .timeout(15000)
        .header("Referer",
                QString("https://www.youtube.com/channel/%1").arg(channelId))
        .onSuccess(guardedCallback(this->lifetimeGuard_,
                                   [this, callback](
                                       const NetworkResult &result) {
            if (!this->running_)
            {
                return;
            }

            const auto html = QString::fromUtf8(result.getData());
            (*callback)(extractEmbedLiveVideoId(html));
                                   }))
        .onError(guardedCallback(this->lifetimeGuard_,
                                 [this, callback](NetworkResult) {
            if (!this->running_)
            {
                return;
            }

            (*callback)({});
                                 }))
        .execute();
}

void YouTubeLiveChat::probeLiveVideoIdFromPath(
    const QString &path, std::function<void(QString)> onResolved)
{
    if (!this->running_ || path.isEmpty())
    {
        onResolved({});
        return;
    }

    const auto url = QString("https://www.youtube.com%1").arg(path);
    auto callback =
        std::make_shared<std::function<void(QString)>>(std::move(onResolved));
    NetworkRequest(url.toStdString())
        .headerList(youtubeHeaders())
        .followRedirects(true)
        .timeout(15000)
        .onSuccess(guardedCallback(
            this->lifetimeGuard_,
            [this, callback, path](const NetworkResult &result) {
                if (!this->running_)
                {
                    return;
                }

                const auto html = QString::fromUtf8(result.getData());
                (*callback)(extractLiveVideoId(html, path.endsWith("/live")));
            }))
        .onError(guardedCallback(this->lifetimeGuard_,
                                 [this, callback](NetworkResult) {
            if (!this->running_)
            {
                return;
            }

            (*callback)({});
                                 }))
        .execute();
}

void YouTubeLiveChat::probeLiveVideoIdFromBrowse(
    const QString &channelId, std::function<void(QString)> onResolved)
{
    if (!this->running_ || !isLikelyChannelId(channelId))
    {
        onResolved({});
        return;
    }

    if (this->apiKey_.isEmpty() || this->clientVersion_.isEmpty())
    {
        this->bootstrapInnertubeContext(
            [this, channelId, onResolved = std::move(onResolved)]() mutable {
                this->probeLiveVideoIdFromBrowse(channelId,
                                                 std::move(onResolved));
            },
            "Couldn't initialize YouTube live chat.");
        return;
    }

    auto callback =
        std::make_shared<std::function<void(QString)>>(std::move(onResolved));
    const auto liveRefererPath = QString("/channel/%1/live").arg(channelId);
    const auto streamsRefererPath =
        QString("/channel/%1/streams").arg(channelId);

    auto tryStreamsTab = [this, channelId, streamsRefererPath, callback] {
        this->probeLiveVideoIdFromBrowseTab(
            channelId, QStringLiteral("EgdzdHJlYW1zuAEAkgMA8gYECgJ6AA=="),
            streamsRefererPath, [this, callback](QString videoId) {
                if (!this->running_)
                {
                    return;
                }

                (*callback)(std::move(videoId));
            });
    };

    this->probeLiveVideoIdFromBrowseTab(
        channelId, QStringLiteral("EgRsaXZluAEAkgMA8gYECgJ6AA=="),
        liveRefererPath,
        [this, tryStreamsTab, callback](QString videoId) {
            if (!this->running_)
            {
                return;
            }

            if (!videoId.isEmpty())
            {
                (*callback)(std::move(videoId));
                return;
            }

            tryStreamsTab();
        });
}

void YouTubeLiveChat::probeLiveVideoIdFromBrowseTab(
    const QString &channelId, const QString &params, const QString &refererPath,
    std::function<void(QString)> onResolved)
{
    if (!this->running_ || !isLikelyChannelId(channelId) ||
        params.trimmed().isEmpty() || refererPath.trimmed().isEmpty())
    {
        onResolved({});
        return;
    }

    const auto url =
        QString("https://www.youtube.com/youtubei/v1/browse?key=%1")
            .arg(this->apiKey_);
    QJsonObject root{
        {"context",
         QJsonObject{{"client", makeYoutubeClientContext(this->clientVersion_)}}},
        {"browseId", channelId},
        {"params", params},
    };

    auto request = NetworkRequest(url.toStdString())
                       .type(NetworkRequestType::Post)
                       .headerList(youtubeHeaders())
                       .json(root)
                       .timeout(15000)
                       .header("Referer",
                               QString("https://www.youtube.com%1")
                                   .arg(refererPath));
    if (!this->visitorData_.isEmpty())
    {
        request = std::move(request).header("X-Goog-Visitor-Id",
                                            this->visitorData_);
    }

    auto callback =
        std::make_shared<std::function<void(QString)>>(std::move(onResolved));
    std::move(request)
        .onSuccess(guardedCallback(
            this->lifetimeGuard_,
            [this, callback](const NetworkResult &result) {
                if (!this->running_)
                {
                    return;
                }

                (*callback)(extractLiveVideoIdFromJson(result.parseJson()));
            }))
        .onError(guardedCallback(this->lifetimeGuard_,
                                 [this, callback](NetworkResult) {
            if (!this->running_)
            {
                return;
            }

            (*callback)({});
                                 }))
        .execute();
}

void YouTubeLiveChat::bootstrapInnertubeContext(std::function<void()> onReady,
                                                QString failureText)
{
    if (!this->running_)
    {
        return;
    }

    if (!this->apiKey_.isEmpty() && !this->clientVersion_.isEmpty() &&
        !this->visitorData_.isEmpty())
    {
        onReady();
        return;
    }

    NetworkRequest(YOUTUBE_BOOTSTRAP_URL.toStdString())
        .headerList(youtubeHeaders())
        .followRedirects(true)
        .timeout(15000)
        .onSuccess(guardedCallback(
            this->lifetimeGuard_,
            [this, onReady = std::move(onReady), failureText](
                const NetworkResult &result) mutable {
                if (!this->running_)
                {
                    return;
                }

            const auto html = QString::fromUtf8(result.getData());
            if (html.contains("Before you continue"))
            {
                this->setStatusText(
                    "YouTube returned a consent gate while loading chat.",
                    !this->failureReported_);
                this->failureReported_ = true;
                this->scheduleResolve(YOUTUBE_BLOCKED_RETRY_DELAY_MS);
                return;
            }
            if (html.contains("unusual traffic", Qt::CaseInsensitive))
            {
                this->setStatusText(
                    "YouTube blocked the live chat request from this network.",
                    !this->failureReported_);
                this->failureReported_ = true;
                this->scheduleResolve(YOUTUBE_BLOCKED_RETRY_DELAY_MS);
                return;
            }

            this->apiKey_ = extractFirstMatch(
                html, {R"yt("INNERTUBE_API_KEY":"([^"]+)")yt",
                       R"yt("innertubeApiKey":"([^"]+)")yt"});
            this->clientVersion_ = extractFirstMatch(
                html, {R"yt("INNERTUBE_CLIENT_VERSION":"([^"]+)")yt",
                       R"yt("clientVersion":"([^"]+)")yt"});
            this->visitorData_ = extractFirstMatch(
                html, {R"yt("VISITOR_DATA":"([^"]+)")yt",
                       R"yt("visitorData":"([^"]+)")yt"});

            if (this->apiKey_.isEmpty() || this->clientVersion_.isEmpty() ||
                this->visitorData_.isEmpty())
            {
                this->setStatusText(failureText, !this->failureReported_);
                this->failureReported_ = true;
                this->scheduleResolve(YOUTUBE_RECONNECT_DELAY_MS);
                return;
            }

            // See fetchLiveChatPage success path: failureReported_ is only
            // cleared once poll() actually returns chat data. Bootstrapping
            // the InnerTube context proves nothing about the live chat
            // endpoint itself.
            onReady();
            }))
        .onError(guardedCallback(
            this->lifetimeGuard_,
            [this, failureText = std::move(failureText)](NetworkResult) mutable {
                if (!this->running_)
                {
                    return;
                }

            this->setStatusText(failureText, !this->failureReported_);
            this->failureReported_ = true;
            this->scheduleResolve(YOUTUBE_RECONNECT_DELAY_MS);
            }))
        .execute();
}

void YouTubeLiveChat::fetchLiveChatPage(bool skipInitialBacklog)
{
    if (!this->running_ || this->videoId_.isEmpty())
    {
        return;
    }

    const bool activeLiveRefresh = this->live_;
    const auto requestedVideoId = this->videoId_;
    qCDebug(chatterinoYouTube).nospace()
        << "[" << this->streamUrl_ << "] fetchLiveChatPage videoId="
        << requestedVideoId << " activeLiveRefresh=" << activeLiveRefresh
        << " skipInitialBacklog=" << skipInitialBacklog;

    if (this->apiKey_.isEmpty() || this->clientVersion_.isEmpty())
    {
        this->bootstrapInnertubeContext(
            [this, skipInitialBacklog] {
                this->fetchLiveChatPage(skipInitialBacklog);
            },
            "Couldn't initialize YouTube live chat.");
        return;
    }

    const auto url =
        QString("https://www.youtube.com/youtubei/v1/next?prettyPrint=false&key=%1")
            .arg(this->apiKey_);
    QJsonObject root{
        {"context",
         QJsonObject{{"client", makeYoutubeClientContext(this->clientVersion_)}}},
        {"videoId", requestedVideoId},
    };

    auto request = NetworkRequest(url.toStdString())
                       .type(NetworkRequestType::Post)
                       .headerList(youtubeHeaders())
                       .json(root)
                       .timeout(15000)
                       .header("Referer",
                               QString("https://www.youtube.com/watch?v=%1")
                                   .arg(requestedVideoId));
    if (!this->visitorData_.isEmpty())
    {
        request = std::move(request).header("X-Goog-Visitor-Id",
                                            this->visitorData_);
    }

    auto requestTimer = std::make_shared<QElapsedTimer>();
    requestTimer->start();
    std::move(request)
        .onSuccess(guardedCallback(
            this->lifetimeGuard_,
            [this, requestTimer, skipInitialBacklog, activeLiveRefresh,
             requestedVideoId](const NetworkResult &result) {
                if (!this->running_ || this->videoId_ != requestedVideoId)
                {
                    return;
                }

            const auto json = result.parseJson();
            if (isEndedOrOfflineLiveBroadcast(json))
            {
                this->waitForNextLive("Waiting for a live YouTube stream.",
                                      YOUTUBE_RECONNECT_DELAY_MS);
                return;
            }

            const auto liveChatRenderer =
                json["contents"]
                    .toObject()["twoColumnWatchNextResults"]
                    .toObject()["conversationBar"]
                    .toObject()["liveChatRenderer"]
                    .toObject();
            this->continuation_ = extractLiveChatContinuation(liveChatRenderer);
            if (this->continuation_.isEmpty())
            {
                this->continuation_ = extractLiveChatContinuationFromJson(json);
            }
            if (this->continuation_.isEmpty())
            {
                qCWarning(chatterinoYouTube).nospace()
                    << "[" << this->streamUrl_
                    << "] fetchLiveChatPage no continuation videoId="
                    << this->videoId_ << " activeLiveRefresh="
                    << activeLiveRefresh << " handleSource="
                    << this->shouldResolveLiveStreamFromSource();
                if (this->shouldResolveLiveStreamFromSource())
                {
                    this->verifySourceLiveAfterMissingContinuation(
                        requestedVideoId, skipInitialBacklog);
                    return;
                }

                if (activeLiveRefresh && !this->failureReported_)
                {
                    this->recoverLiveChat(
                        "YouTube live chat refresh lost its continuation. "
                        "Reconnecting.",
                        YOUTUBE_RECONNECT_DELAY_MS);
                    return;
                }

                if (activeLiveRefresh)
                {
                    // Fixed video sources do not have a live channel page
                    // to fall back to. After the retry path has already
                    // failed, a missing continuation means this live chat
                    // is no longer active.
                    this->setLive(false);
                    this->liveTitle_.clear();
                    this->liveStartedAt_ = {};
                    this->liveViewerCount_ = 0;
                }
                this->setStatusText(
                    "Couldn't find the YouTube live chat continuation data.",
                    !this->failureReported_);
                this->failureReported_ = true;
                this->resetInnertubeContext();
                this->scheduleResolve(YOUTUBE_RECONNECT_DELAY_MS);
                return;
            }

            // A continuation was obtained: clear the missing-continuation
            // backoff so the next outage starts from the short delay again.
            this->missingContinuationStreak_ = 0;
            this->missingContinuationVideoId_.clear();

            const auto ownerChannelId = extractLiveOwnerChannelId(json);
            if (this->liveOwnerChannelId_ != ownerChannelId)
            {
                this->liveOwnerChannelId_ = ownerChannelId;
                this->moderationStatusChanged.invoke();
            }

            this->liveTitle_ = extractLiveStreamTitle(json);
            qCDebug(chatterinoYouTube).nospace()
                << "[" << this->streamUrl_
                << "] fetchLiveChatPage continuation found videoId="
                << this->videoId_ << " title=\"" << this->liveTitle_
                << "\" activeLiveRefresh=" << activeLiveRefresh;
            // Authoritative non-live signal: the videoPrimaryInfoRenderer
            // viewCount block exposes `isLive: true` only for an actively
            // broadcasting stream. We used to gate Shorts on
            // `liveTitle_.contains("#shorts")`, but streamers routinely put
            // "#shorts" hashtags in real live-stream titles for SEO -- e.g.
            // gevad1ch's "SUUUUPER DEEP MONDAY GROUP @GEVAD1CH#shorts" --
            // which produced false positives that locked the recently-failed
            // cache against legitimate live streams.
            //
            // Note: the InnerTube /next endpoint does NOT include
            // microformat.playerMicroformatRenderer.liveBroadcastDetails;
            // that field is on the /player endpoint. Don't gate on it here.
            const bool isLiveBroadcast = extractIsLiveFromNextResponse(json);
            if (!isLiveBroadcast)
            {
                qCWarning(chatterinoYouTube).nospace()
                    << "[" << this->streamUrl_
                    << "] fetchLiveChatPage rejecting non-live videoId="
                    << this->videoId_ << " title=\"" << this->liveTitle_
                    << "\"";
                this->recentlyFailedVideoId_ = this->videoId_;
                this->recentlyFailedTimer_.restart();
                if (this->recentlyFailedStreak_ < 1)
                {
                    this->recentlyFailedStreak_ = 1;
                }
                this->waitForNextLive(
                    QStringLiteral("Waiting for a live YouTube stream."),
                    YOUTUBE_RECONNECT_DELAY_MS);
                return;
            }
            const auto liveStartedAt = extractLiveStartTime(json);
            if (liveStartedAt.isValid())
            {
                this->liveStartedAt_ = liveStartedAt;
            }
            else if (!activeLiveRefresh)
            {
                this->liveStartedAt_ = {};
            }
            const auto viewerCount =
                extractLiveViewerCountFromNextResponse(json);
            if (viewerCount > 0)
            {
                this->updateLiveViewerCount(viewerCount);
            }
            else if (!activeLiveRefresh)
            {
                this->liveViewerCount_ = 0;
            }
            // Don't clear failureReported_ here: getting a continuation back
            // from the watch page only proves the SPA can reach YouTube, not
            // that polling will work. The poll() success path resets it once
            // we've actually pulled chat. Otherwise a recoverLiveChat loop
            // (poll fails -> rebootstrap succeeds -> poll fails -> ...) re-
            // arms the "Reconnecting." status message every cycle.
            this->activePollStreak_ = 0;
            this->liveChatSessionRefreshTimer_.restart();
            this->liveChatProgressTimer_.restart();
            // Don't setLive(true) here. Finding a continuation in the watch
            // page only proves the page exists; it doesn't prove the live
            // chat endpoint will accept this continuation. Mis-resolved
            // videos (Shorts, ended streams returning a stale conversation
            // bar) reach this point too and would briefly flash the live
            // marker / fire "Joined" before escalation drops them ~8s later.
            // poll() is the authoritative check: setLive(true) happens only
            // after the first successful chat poll.
            this->setStatusText(
                this->liveTitle_.isEmpty()
                    ? QString("Connecting to YouTube live chat (%1)...")
                          .arg(this->videoId_)
                    : QString("Connecting to YouTube live chat (%1)...")
                          .arg(this->liveTitle_));
            this->skipInitialBacklog_ = skipInitialBacklog;
            this->poll();
            }))
        .onError(guardedCallback(
            this->lifetimeGuard_,
            [this, activeLiveRefresh, requestedVideoId](NetworkResult) {
                if (!this->running_ || this->videoId_ != requestedVideoId)
                {
                    return;
                }

            if (activeLiveRefresh && !this->failureReported_)
            {
                this->recoverLiveChat(
                    "YouTube live chat refresh failed. Reconnecting.",
                    YOUTUBE_RECONNECT_DELAY_MS);
                return;
            }

            this->setStatusText("Couldn't load the YouTube live chat page.",
                                !this->failureReported_);
            this->failureReported_ = true;
            this->resetInnertubeContext();
            this->scheduleResolve(YOUTUBE_RECONNECT_DELAY_MS);
            }))
        .execute();
}

void YouTubeLiveChat::ensureActiveLiveChatId(
    std::shared_ptr<YouTubeAccount> account, ActiveLiveChatIdCallback callback,
    QString operationName)
{
    if (!account || account->isAnonymous() || this->videoId_.isEmpty())
    {
        this->systemMessageReceived.invoke(
            makeSystemStatusMessage("YouTube live chat is not ready yet."));
        return;
    }

    if (this->activeLiveChatVideoId_ == this->videoId_ &&
        !this->activeLiveChatId_.isEmpty())
    {
        callback(std::move(account), this->activeLiveChatId_);
        return;
    }

    QUrl url("https://www.googleapis.com/youtube/v3/videos");
    QUrlQuery query{
        {"part", "liveStreamingDetails"},
        {"id", this->videoId_},
    };
    url.setQuery(query);

    const auto isModerationOperation =
        operationName == QStringLiteral("YouTube moderation");
    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    NetworkRequest(url)
        .header("Authorization",
                QStringLiteral("Bearer ") + account->authToken())
        .timeout(20'000)
        .onSuccess([this, weak, account = std::move(account),
                    callback = std::move(callback), isModerationOperation](
                       const NetworkResult &result) mutable {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            const auto items = result.parseJson()["items"].toArray();
            if (items.isEmpty() || !items.at(0).isObject())
            {
                if (isModerationOperation)
                {
                    this->reportModerationActionFailure(
                        QStringLiteral("prepare YouTube moderation"),
                        QStringLiteral(
                            "YouTube did not return the current live stream."),
                        result);
                    return;
                }

                this->systemMessageReceived.invoke(makeSystemStatusMessage(
                    "YouTube did not return the current live stream."));
                return;
            }

            const auto liveChatId =
                items.at(0)
                    .toObject()["liveStreamingDetails"]
                    .toObject()["activeLiveChatId"]
                    .toString()
                    .trimmed();
            if (liveChatId.isEmpty())
            {
                if (isModerationOperation)
                {
                    this->reportModerationActionFailure(
                        QStringLiteral("prepare YouTube moderation"),
                        QStringLiteral("YouTube did not return an active live "
                                       "chat for this stream."),
                        result);
                    return;
                }

                this->systemMessageReceived.invoke(makeSystemStatusMessage(
                    "YouTube did not return an active live chat for this "
                    "stream."));
                return;
            }

            this->activeLiveChatVideoId_ = this->videoId_;
            this->activeLiveChatId_ = liveChatId;
            callback(std::move(account), liveChatId);
        })
        .onError([this, weak, isModerationOperation,
                  operationName = std::move(operationName)](
                     const NetworkResult &result) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            const auto error = formatGoogleError(result);
            qCWarning(chatterinoYouTube)
                << "Fetching active YouTube live chat ID failed" << error;
            if (isModerationOperation)
            {
                this->reportModerationActionFailure(
                    QStringLiteral("prepare YouTube moderation"), error,
                    result);
                return;
            }

            this->systemMessageReceived.invoke(makeSystemStatusMessage(
                QString("Could not prepare %1. %2").arg(operationName, error)));
        })
        .execute();
}

void YouTubeLiveChat::postLiveChatMessage(
    std::shared_ptr<YouTubeAccount> account, QString liveChatId,
    QString message)
{
    if (!account || account->isAnonymous() || liveChatId.isEmpty() ||
        message.trimmed().isEmpty())
    {
        return;
    }

    QUrl url("https://www.googleapis.com/youtube/v3/liveChat/messages");
    QUrlQuery query{{"part", "snippet"}};
    url.setQuery(query);

    QJsonObject root{
        {"snippet",
         QJsonObject{
             {"liveChatId", liveChatId},
             {"type", "textMessageEvent"},
             {"textMessageDetails",
              QJsonObject{
                  {"messageText", message},
              }},
         }},
    };

    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    NetworkRequest(url, NetworkRequestType::Post)
        .header("Authorization",
                QStringLiteral("Bearer ") + account->authToken())
        .json(root)
        .timeout(20'000)
        .onSuccess([weak](const NetworkResult &) {
            if (!weak.lock())
            {
                return;
            }
        })
        .onError([this, weak](const NetworkResult &result) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            if (result.status() && (*result.status() == 403 ||
                                    *result.status() == 404))
            {
                this->activeLiveChatVideoId_.clear();
                this->activeLiveChatId_.clear();
            }

            const auto error = formatGoogleError(result);
            qCWarning(chatterinoYouTube)
                << "Sending YouTube live chat message failed" << error;
            this->systemMessageReceived.invoke(makeSystemStatusMessage(
                QString("Could not send YouTube chat message. %1").arg(error)));
        })
        .execute();
}

void YouTubeLiveChat::postLiveChatBan(
    std::shared_ptr<YouTubeAccount> account, QString liveChatId,
    QString channelId, QString displayName, int durationSeconds)
{
    if (!account || account->isAnonymous() || liveChatId.isEmpty() ||
        channelId.isEmpty())
    {
        return;
    }

    QUrl url("https://www.googleapis.com/youtube/v3/liveChat/bans");
    QUrlQuery query{{"part", "snippet"}};
    url.setQuery(query);

    QJsonObject snippet{
        {"liveChatId", liveChatId},
        {"type", durationSeconds > 0 ? "temporary" : "permanent"},
        {"bannedUserDetails",
         QJsonObject{
             {"channelId", channelId},
         }},
    };
    if (durationSeconds > 0)
    {
        snippet.insert("banDurationSeconds", durationSeconds);
    }

    QJsonObject root{{"snippet", snippet}};

    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    NetworkRequest(url, NetworkRequestType::Post)
        .header("Authorization",
                QStringLiteral("Bearer ") + account->authToken())
        .json(root)
        .timeout(20'000)
        .onSuccess([this, weak, liveChatId, channelId, displayName,
                    durationSeconds](const NetworkResult &result) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            const auto banId = result.parseJson()["id"].toString();
            if (!banId.isEmpty())
            {
                this->liveChatBanIds_[liveChatBanCacheKey(liveChatId,
                                                          channelId)] = banId;
            }

            const auto name = moderationDisplayName(displayName, channelId);
            if (durationSeconds > 0)
            {
                this->systemMessageReceived.invoke(makeSystemStatusMessage(
                    QString("Timed out %1 on YouTube for %2 seconds.")
                        .arg(name, QString::number(durationSeconds))));
            }
            else
            {
                this->systemMessageReceived.invoke(makeSystemStatusMessage(
                    QString("Banned %1 on YouTube.").arg(name)));
            }
        })
        .onError([this, weak, durationSeconds](const NetworkResult &result) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            if (result.status() && (*result.status() == 403 ||
                                    *result.status() == 404))
            {
                this->activeLiveChatVideoId_.clear();
                this->activeLiveChatId_.clear();
            }

            const auto error = formatGoogleError(result);
            qCWarning(chatterinoYouTube)
                << "Banning YouTube live chat user failed" << error;
            const auto action =
                durationSeconds > 0 ? QStringLiteral("timeout YouTube user")
                                    : QStringLiteral("ban YouTube user");
            this->reportModerationActionFailure(action, error, result);
        })
        .execute();
}

void YouTubeLiveChat::deleteLiveChatBan(
    std::shared_ptr<YouTubeAccount> account, QString banId, QString channelId,
    QString displayName)
{
    if (!account || account->isAnonymous() || banId.isEmpty())
    {
        return;
    }

    QUrl url("https://www.googleapis.com/youtube/v3/liveChat/bans");
    QUrlQuery query{{"id", banId}};
    url.setQuery(query);

    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    NetworkRequest(url, NetworkRequestType::Delete)
        .header("Authorization",
                QStringLiteral("Bearer ") + account->authToken())
        .timeout(20'000)
        .onSuccess([this, weak, channelId, displayName, banId](
                       const NetworkResult &) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            for (auto it = this->liveChatBanIds_.begin();
                 it != this->liveChatBanIds_.end();)
            {
                if (it->second == banId)
                {
                    it = this->liveChatBanIds_.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            this->systemMessageReceived.invoke(makeSystemStatusMessage(
                QString("Unbanned %1 on YouTube.")
                    .arg(moderationDisplayName(displayName, channelId))));
        })
        .onError([this, weak, banId](const NetworkResult &result) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            if (result.status() && *result.status() == 404)
            {
                for (auto it = this->liveChatBanIds_.begin();
                     it != this->liveChatBanIds_.end();)
                {
                    if (it->second == banId)
                    {
                        it = this->liveChatBanIds_.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }

            const auto error = formatGoogleError(result);
            qCWarning(chatterinoYouTube)
                << "Removing YouTube live chat ban failed" << error;
            this->reportModerationActionFailure(
                QStringLiteral("unban YouTube user"), error, result);
        })
        .execute();
}

void YouTubeLiveChat::verifySourceLiveAfterMissingContinuation(
    const QString &requestedVideoId, bool skipInitialBacklog)
{
    const auto source = this->resolvedSource();
    if (source.isEmpty())
    {
        this->waitForNextLive("Waiting for a live YouTube stream.",
                              YOUTUBE_RECONNECT_DELAY_MS);
        return;
    }

    this->probeLiveVideoIdFromSource(
        source, [this, requestedVideoId, skipInitialBacklog](QString videoId) {
            if (!this->running_ || this->videoId_ != requestedVideoId)
            {
                return;
            }

            if (videoId.isEmpty())
            {
                this->waitForNextLive("Waiting for a live YouTube stream.",
                                      YOUTUBE_RECONNECT_DELAY_MS);
                return;
            }

            if (videoId != requestedVideoId)
            {
                this->videoId_ = std::move(videoId);
                this->seenMessageIds_.clear();
                this->fetchLiveChatPage(skipInitialBacklog);
                return;
            }

            if (this->missingContinuationVideoId_ != requestedVideoId)
            {
                this->missingContinuationVideoId_ = requestedVideoId;
                this->missingContinuationStreak_ = 0;
            }

            this->setLive(true);
            this->setStatusText(
                "YouTube stream is live. Waiting for live chat.");
            this->failureReported_ = true;
            // Rebuilding the InnerTube context forces an embed bootstrap
            // re-fetch; refresh it every few attempts instead of every
            // cycle so a stale context still recovers eventually.
            if (this->missingContinuationStreak_ % 4 == 3)
            {
                this->resetInnertubeContext();
            }
            const auto shift = std::min(this->missingContinuationStreak_, 5);
            this->missingContinuationStreak_++;
            this->scheduleResolve(
                std::min(YOUTUBE_RECONNECT_DELAY_MS * (1 << shift), 60'000));
        });
}

void YouTubeLiveChat::poll()
{
    if (!this->running_ || this->continuation_.isEmpty() ||
        this->apiKey_.isEmpty())
    {
        return;
    }

    const auto url = QString(
                         "https://www.youtube.com/youtubei/v1/live_chat/"
                         "get_live_chat?prettyPrint=false&key=%1")
                         .arg(this->apiKey_);

    QJsonObject root{
        {"context",
         QJsonObject{{"client", makeYoutubeClientContext(this->clientVersion_)}}},
        {"continuation", this->continuation_},
    };

    auto request = NetworkRequest(url.toStdString())
                       .type(NetworkRequestType::Post)
                       .headerList(youtubeHeaders())
                       .json(root)
                       .timeout(15000)
                       .header("Referer",
                               QString("https://www.youtube.com/watch?v=%1")
                                   .arg(this->videoId_));
    if (!this->visitorData_.isEmpty())
    {
        request = std::move(request).header("X-Goog-Visitor-Id",
                                            this->visitorData_);
    }

    auto requestTimer = std::make_shared<QElapsedTimer>();
    requestTimer->start();
    std::move(request)
        .onSuccess(guardedCallback(
            this->lifetimeGuard_,
            [this, requestTimer](const NetworkResult &result) {
                if (!this->running_)
                {
                    return;
                }

            auto json = result.parseJson();
            auto continuation = json["continuationContents"]
                                    .toObject()["liveChatContinuation"]
                                    .toObject();
            this->liveChatProgressTimer_.restart();
            if (continuation.isEmpty())
            {
                this->refreshLiveChatContinuation(
                    "YouTube live chat continuation expired. Refreshing.",
                    YOUTUBE_POLL_REFRESH_FALLBACK_DELAY_MS);
                return;
            }

            auto actions = continuation["actions"].toArray();
            // YouTube no longer ships viewer count inside the live_chat
            // continuation (the field comes back null). The count lives on
            // the /updated_metadata endpoint instead - see
            // fetchUpdatedMetadata(), scheduled from setLive(true).

            int deliveredMessageCount = 0;
            for (const auto &actionValue : actions)
            {
                auto action = actionValue.toObject();
                auto item = action["addChatItemAction"].toObject()["item"]
                                .toObject();
                if (item.isEmpty())
                {
                    continue;
                }

                const auto keys = item.keys();
                if (keys.isEmpty())
                {
                    continue;
                }

                const auto &rendererName = keys.first();
                const auto renderer = item[rendererName].toObject();
                this->updateModeratorPrivilegesFromRenderer(renderer);
                auto message = parseRendererMessage(
                    renderer, rendererName, this->videoId_,
                    &this->customEmotes_);
                if (!message || message->id.isEmpty())
                {
                    continue;
                }

                auto inserted = this->seenMessageIds_.emplace(message->id);
                if (!inserted.second)
                {
                    continue;
                }

                if (this->skipInitialBacklog_)
                {
                    continue;
                }

                this->messageReceived.invoke(message);
                deliveredMessageCount++;
            }

            this->skipInitialBacklog_ = false;
            if (deliveredMessageCount > 0)
            {
                this->activePollStreak_ =
                    std::min(this->activePollStreak_ + 1, 3);
            }
            else
            {
                this->activePollStreak_ = 0;
            }

            const auto continuations = continuation["continuations"].toArray();
            int nextDelay = 1000;
            bool updatedContinuation = false;
            for (const auto &continuationValue : continuations)
            {
                auto continuationObject = continuationValue.toObject();
                const auto timed =
                    continuationObject["timedContinuationData"].toObject();
                if (!timed.isEmpty())
                {
                    this->continuation_ = timed["continuation"].toString();
                    nextDelay = adjustedYouTubePollDelay(
                        timed["timeoutMs"].toInt(nextDelay),
                        requestTimer->elapsed(), deliveredMessageCount,
                        this->activePollStreak_);
                    updatedContinuation = !this->continuation_.isEmpty();
                    break;
                }

                const auto invalidation =
                    continuationObject["invalidationContinuationData"]
                        .toObject();
                if (!invalidation.isEmpty())
                {
                    this->continuation_ =
                        invalidation["continuation"].toString();
                    nextDelay = adjustedYouTubePollDelay(
                        invalidation["timeoutMs"].toInt(nextDelay),
                        requestTimer->elapsed(), deliveredMessageCount,
                        this->activePollStreak_);
                    updatedContinuation = !this->continuation_.isEmpty();
                    break;
                }
            }

            if (!updatedContinuation)
            {
                this->refreshLiveChatContinuation(
                    "YouTube live chat continuation expired. Refreshing.",
                    YOUTUBE_POLL_REFRESH_FALLBACK_DELAY_MS);
                return;
            }

            qCDebug(chatterinoYouTube).nospace()
                << "[" << this->streamUrl_ << "] poll ok videoId="
                << this->videoId_ << " actions=" << actions.size()
                << " delivered=" << deliveredMessageCount
                << " viewers=" << this->liveViewerCount_
                << " nextDelay=" << nextDelay << "ms"
                << " (counter " << this->consecutiveRecoveries_ << "->0)";
            this->failureReported_ = false;
            this->consecutiveRecoveries_ = 0;
            this->recentlyFailedStreak_ = 0;
            this->recentlyFailedVideoId_.clear();
            this->pollRefreshFallbackCount_ = 0;
            // First successful poll proves the chat session is real - now
            // it's safe to flip live and fire the "Joined" announce.
            if (!this->live_)
            {
                this->setStatusText(
                    this->liveTitle_.isEmpty()
                        ? QString("Watching YouTube live chat for %1")
                              .arg(this->videoId_)
                        : QString("Watching YouTube live chat for %1")
                              .arg(this->liveTitle_));
            }
            this->setLive(true);
            if (this->liveChatSessionRefreshTimer_.isValid() &&
                this->liveChatSessionRefreshTimer_.elapsed() >=
                    YOUTUBE_SESSION_REFRESH_MS)
            {
                this->fetchLiveChatPage(false);
                return;
            }
            this->schedulePoll(nextDelay);
            }))
        .onError(guardedCallback(this->lifetimeGuard_,
                                 [this](NetworkResult result) {
            if (!this->running_)
            {
                return;
            }

            qCWarning(chatterinoYouTube).nospace()
                << "[" << this->streamUrl_ << "] poll error videoId="
                << this->videoId_ << " status="
                << result.status().value_or(-1);
            this->refreshLiveChatContinuation(
                "YouTube live chat polling failed. Refreshing continuation.",
                YOUTUBE_POLL_REFRESH_FALLBACK_DELAY_MS);
                                 }))
        .execute();
}

void YouTubeLiveChat::refreshLiveChatContinuation(QString text, int retryDelayMs)
{
    this->activePollStreak_ = 0;
    this->liveChatProgressTimer_.restart();

    if (++this->pollRefreshFallbackCount_ >
        YOUTUBE_POLL_REFRESH_FALLBACK_LIMIT)
    {
        this->recoverLiveChat(
            "YouTube live chat polling fallback failed. Reconnecting.",
            YOUTUBE_RECONNECT_DELAY_MS, false);
        return;
    }

    this->setStatusText(std::move(text));

    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    QTimer::singleShot(retryDelayMs, [this, weak] {
        if (!weak.lock() || !this->running_)
        {
            return;
        }

        if (this->live_ && !this->videoId_.isEmpty())
        {
            this->fetchLiveChatPage(false);
            return;
        }

        this->resolveVideoId();
    });
}

void YouTubeLiveChat::schedulePoll(int delayMs)
{
    QTimer::singleShot(delayMs,
                       guardedCallback(this->lifetimeGuard_, [this] {
                           if (!this->running_)
                           {
                               return;
                           }
                           this->poll();
                       }));
}

void YouTubeLiveChat::scheduleHealthCheck(int delayMs)
{
    QTimer::singleShot(delayMs, guardedCallback(this->lifetimeGuard_, [this] {
                           if (!this->running_)
                           {
                               return;
                           }

                           const bool shouldMonitor =
                               this->live_ &&
                               !this->continuation_.isEmpty() &&
                               !this->apiKey_.isEmpty();
                           if (shouldMonitor &&
                               this->liveChatProgressTimer_.isValid() &&
                               this->liveChatProgressTimer_.elapsed() >=
                                   YOUTUBE_STALL_TIMEOUT_MS)
                           {
                               this->recoverLiveChat(
                                   "YouTube live chat stalled. Reconnecting.",
                                   YOUTUBE_RECONNECT_DELAY_MS);
                           }

                           this->scheduleHealthCheck(
                               YOUTUBE_HEALTH_CHECK_INTERVAL_MS);
                       }));
}

void YouTubeLiveChat::scheduleResolve(int delayMs)
{
    QTimer::singleShot(delayMs,
                       guardedCallback(this->lifetimeGuard_, [this] {
                           if (!this->running_)
                           {
                               return;
                           }

                           this->resolveVideoId();
                       }));
}

void YouTubeLiveChat::scheduleUpdatedMetadata(int delayMs)
{
    QTimer::singleShot(delayMs,
                       guardedCallback(this->lifetimeGuard_, [this] {
                           if (!this->running_ || !this->live_)
                           {
                               return;
                           }
                           this->fetchUpdatedMetadata();
                       }));
}

void YouTubeLiveChat::fetchUpdatedMetadata()
{
    if (!this->running_ || !this->live_ || this->videoId_.isEmpty())
    {
        return;
    }
    if (this->apiKey_.isEmpty() || this->clientVersion_.isEmpty())
    {
        // Bootstrap is owned by fetchLiveChatPage. Wait for the next chat
        // poll cycle to finish bootstrapping; we'll be re-armed from setLive
        // / fetchUpdatedMetadata's own re-schedule.
        this->scheduleUpdatedMetadata(YOUTUBE_VIEWER_COUNT_REFRESH_MS);
        return;
    }

    const auto requestedVideoId = this->videoId_;
    const auto url =
        QString("https://www.youtube.com/youtubei/v1/updated_metadata"
                "?prettyPrint=false&key=%1")
            .arg(this->apiKey_);
    QJsonObject root{
        {"context",
         QJsonObject{{"client", makeYoutubeClientContext(this->clientVersion_)}}},
        {"videoId", requestedVideoId},
    };

    auto request = NetworkRequest(url.toStdString())
                       .type(NetworkRequestType::Post)
                       .headerList(youtubeHeaders())
                       .json(root)
                       .timeout(15000)
                       .header("Referer",
                               QString("https://www.youtube.com/watch?v=%1")
                                   .arg(requestedVideoId));
    if (!this->visitorData_.isEmpty())
    {
        request = std::move(request).header("X-Goog-Visitor-Id",
                                            this->visitorData_);
    }

    std::move(request)
        .onSuccess(guardedCallback(
            this->lifetimeGuard_,
            [this, requestedVideoId](const NetworkResult &result) {
                if (!this->running_ || this->videoId_ != requestedVideoId)
                {
                    return;
                }
                const auto json = result.parseJson();
                uint64_t viewerCount = 0;
                for (const auto &actionValue : json["actions"].toArray())
                {
                    const auto action = actionValue.toObject();
                    const auto renderer =
                        action["updateViewershipAction"]
                            .toObject()["viewCount"]
                            .toObject()["videoViewCountRenderer"]
                            .toObject();
                    if (renderer.isEmpty())
                    {
                        continue;
                    }
                    viewerCount = parseViewerCount(renderer["viewCount"]);
                    if (viewerCount == 0)
                    {
                        viewerCount = parseViewerCount(
                            renderer["originalViewCount"]);
                    }
                    if (viewerCount > 0)
                    {
                        break;
                    }
                }
                qCDebug(chatterinoYouTube).nospace()
                    << "[" << this->streamUrl_
                    << "] updated_metadata viewerCount=" << viewerCount;
                if (viewerCount > 0 && viewerCount != this->liveViewerCount_)
                {
                    this->liveViewerCount_ = viewerCount;
                    this->viewerCountChanged.invoke();
                }
                this->scheduleUpdatedMetadata(
                    YOUTUBE_VIEWER_COUNT_REFRESH_MS);
            }))
        .onError(guardedCallback(
            this->lifetimeGuard_, [this](const NetworkResult &result) {
                qCDebug(chatterinoYouTube).nospace()
                    << "[" << this->streamUrl_
                    << "] updated_metadata error status="
                    << result.status().value_or(-1);
                this->scheduleUpdatedMetadata(
                    YOUTUBE_VIEWER_COUNT_REFRESH_MS);
            }))
        .execute();
}

void YouTubeLiveChat::recoverLiveChat(QString text, int retryDelayMs,
                                      bool notifyAsSystemMessage)
{
    // Don't drop live_ on every call. recoverLiveChat fires on every
    // transient poll hiccup (3s cadence); flipping live_ false->true each
    // cycle made the tab's red live marker flash and re-fired the join
    // announce. Instead, count consecutive recoveries since the last
    // successful poll: at the threshold, escalate to waitForNextLive so
    // the live marker actually turns off when the stream genuinely ended
    // or was mis-resolved (e.g. a Shorts page that has no real live chat).
    constexpr int kRecoveryEscalationThreshold = 3;
    this->consecutiveRecoveries_++;
    qCWarning(chatterinoYouTube).nospace()
        << "[" << this->streamUrl_ << "] recoverLiveChat #"
        << this->consecutiveRecoveries_ << "/" << kRecoveryEscalationThreshold
        << " text=\"" << text << "\" videoId=" << this->videoId_
        << " live=" << this->live_;
    if (this->consecutiveRecoveries_ >= kRecoveryEscalationThreshold)
    {
        if (this->recentlyFailedVideoId_ == this->videoId_)
        {
            this->recentlyFailedStreak_++;
        }
        else
        {
            this->recentlyFailedStreak_ = 1;
        }
        const qint64 cooldown = std::min(
            recentlyFailedMaxWindowMs,
            recentlyFailedBaseWindowMs *
                (1LL << std::min(this->recentlyFailedStreak_ - 1, 8)));
        qCWarning(chatterinoYouTube).nospace()
            << "[" << this->streamUrl_
            << "] recoverLiveChat threshold hit -> waitForNextLive"
            << " (videoId=" << this->videoId_ << " streak="
            << this->recentlyFailedStreak_ << " cooldown=" << cooldown / 1000
            << "s)";
        this->consecutiveRecoveries_ = 0;
        this->recentlyFailedVideoId_ = this->videoId_;
        this->recentlyFailedTimer_.restart();
        this->waitForNextLive(
            QStringLiteral(
                "YouTube live chat unavailable. Waiting for next stream."),
            retryDelayMs);
        return;
    }
    this->continuation_.clear();
    this->activePollStreak_ = 0;
    this->pollRefreshFallbackCount_ = 0;
    this->liveChatSessionRefreshTimer_.invalidate();
    this->liveChatProgressTimer_.invalidate();
    this->resetInnertubeContext();
    // Stay silent on a single transient blip: a one-off poll error usually
    // recovers on the next cycle without the user ever needing to know.
    // Only push a chat system message once we've seen 2+ consecutive
    // recoveries since the last successful poll. failureReported_ still
    // dedupes within a sustained-failure streak. The caller can also opt
    // out entirely via notifyAsSystemMessage=false.
    const bool notify = notifyAsSystemMessage &&
                        this->consecutiveRecoveries_ >= 2 &&
                        !this->failureReported_;
    this->setStatusText(std::move(text), notify);
    if (this->consecutiveRecoveries_ >= 2)
    {
        this->failureReported_ = true;
    }

    QTimer::singleShot(retryDelayMs,
                       guardedCallback(this->lifetimeGuard_, [this] {
                           if (!this->running_)
                           {
                               return;
                           }

                           if (this->live_ && !this->videoId_.isEmpty())
                           {
                               this->fetchLiveChatPage(false);
                               return;
                           }

                           this->resolveVideoId();
                       }));
}

void YouTubeLiveChat::waitForNextLive(QString text, int retryDelayMs)
{
    // Demoted from WARN to DEBUG: this fires on every poll cycle for every
    // offline channel (steady-state, not exceptional). Was producing tens
    // of WARN lines/minute for normal "channel is offline" state.
    qCDebug(chatterinoYouTube).nospace()
        << "[" << this->streamUrl_ << "] waitForNextLive videoId="
        << this->videoId_ << " text=\"" << text << "\"";
    this->setLive(false);
    this->videoId_.clear();
    this->continuation_.clear();
    this->liveTitle_.clear();
    this->activeLiveChatVideoId_.clear();
    this->activeLiveChatId_.clear();
    this->liveStartedAt_ = {};
    this->liveViewerCount_ = 0;
    this->seenMessageIds_.clear();
    this->failureReported_ = false;
    this->skipInitialBacklog_ = false;
    this->activePollStreak_ = 0;
    this->consecutiveRecoveries_ = 0;
    this->pollRefreshFallbackCount_ = 0;
    this->liveChatSessionRefreshTimer_.invalidate();
    this->liveChatProgressTimer_.invalidate();
    this->setStatusText(std::move(text));
    // Always slow-poll while waiting for next live, regardless of the
    // caller-supplied retryDelayMs (which is typically 3s, intended for
    // mid-session reconnects).
    this->scheduleResolve(
        std::max(retryDelayMs, YOUTUBE_OFFLINE_POLL_DELAY_MS));
}

void YouTubeLiveChat::resetInnertubeContext()
{
    this->apiKey_.clear();
    this->clientVersion_.clear();
    this->visitorData_.clear();
}

void YouTubeLiveChat::resetModeratorPrivileges()
{
    if (!this->hasModeratorPrivileges_ &&
        this->moderatorPrivilegesVideoId_.isEmpty() &&
        this->moderatorPrivilegesAccountChannelId_.isEmpty())
    {
        return;
    }

    this->hasModeratorPrivileges_ = false;
    this->moderatorPrivilegesVideoId_.clear();
    this->moderatorPrivilegesAccountChannelId_.clear();
    this->moderationStatusChanged.invoke();
}

bool YouTubeLiveChat::moderationToolsDisabledForCurrentAccount() const
{
    auto *accounts = getApp()->getAccounts();
    if (!accounts->youtube.isLoggedIn())
    {
        return false;
    }

    const auto account = accounts->youtube.current();
    const auto currentChannelId = account->channelID();
    if (currentChannelId.isEmpty())
    {
        return false;
    }

    return this->moderationDisabledVideoId_ == this->videoId_ &&
           this->moderationDisabledAccountChannelId_.compare(
               currentChannelId, Qt::CaseInsensitive) == 0;
}

void YouTubeLiveChat::reportModerationToolsUnavailable()
{
    this->systemMessageReceived.invoke(makeSystemStatusMessage(
        "YouTube moderation tools are hidden for this live chat after a "
        "moderation action failed."));
}

void YouTubeLiveChat::reportModerationActionFailure(const QString &action,
                                                   const QString &error,
                                                   const NetworkResult &result)
{
    if (!isModerationAuthorizationFailure(result))
    {
        // Transient failure (timeout, rate limit, 5xx, or an API response
        // that isn't about permissions): tell the user, but keep the mod
        // tools available - only a definitive authorization failure may
        // hide them for the rest of the stream.
        qCWarning(chatterinoYouTube)
            << "Transient YouTube moderation failure, keeping mod tools"
            << action << error;
        this->systemMessageReceived.invoke(makeSystemStatusMessage(
            QString("Could not %1. %2").arg(action, error)));
        return;
    }

    auto *accounts = getApp()->getAccounts();
    QString currentChannelId;
    if (accounts->youtube.isLoggedIn())
    {
        currentChannelId = accounts->youtube.current()->channelID();
    }

    bool changed = false;
    if (!this->videoId_.isEmpty() && !currentChannelId.isEmpty() &&
        (this->moderationDisabledVideoId_ != this->videoId_ ||
         this->moderationDisabledAccountChannelId_.compare(
             currentChannelId, Qt::CaseInsensitive) != 0))
    {
        this->moderationDisabledVideoId_ = this->videoId_;
        this->moderationDisabledAccountChannelId_ = currentChannelId;
        changed = true;
    }

    this->systemMessageReceived.invoke(makeSystemStatusMessage(
        QString("Could not %1. You're probably not a mod or owner here. "
                "Hiding YouTube mod tools.")
            .arg(action)));

    if (changed)
    {
        this->moderationStatusChanged.invoke();
    }
}

void YouTubeLiveChat::updateModeratorPrivilegesFromRenderer(
    const QJsonObject &renderer)
{
    auto *accounts = getApp()->getAccounts();
    if (!accounts->youtube.isLoggedIn() || this->videoId_.isEmpty())
    {
        return;
    }

    const auto account = accounts->youtube.current();
    const auto currentChannelId = account->channelID();
    if (currentChannelId.isEmpty() ||
        currentChannelId.compare(renderer["authorExternalChannelId"].toString(),
                                 Qt::CaseInsensitive) != 0)
    {
        return;
    }

    const auto hasPrivileges = rendererHasModeratorBadge(renderer);
    if (!hasPrivileges)
    {
        return;
    }

    const auto moderationWasDisabled =
        this->moderationToolsDisabledForCurrentAccount();
    const auto changed =
        !this->hasModeratorPrivileges_ ||
        this->moderatorPrivilegesVideoId_ != this->videoId_ ||
        this->moderatorPrivilegesAccountChannelId_.compare(
            currentChannelId, Qt::CaseInsensitive) != 0 ||
        moderationWasDisabled;
    if (!changed)
    {
        return;
    }

    this->hasModeratorPrivileges_ = true;
    this->moderatorPrivilegesVideoId_ = this->videoId_;
    this->moderatorPrivilegesAccountChannelId_ = currentChannelId;
    if (moderationWasDisabled)
    {
        this->moderationDisabledVideoId_.clear();
        this->moderationDisabledAccountChannelId_.clear();
    }
    this->moderationStatusChanged.invoke();
}

void YouTubeLiveChat::updateLiveViewerCount(uint64_t viewerCount)
{
    if (viewerCount == 0 || this->liveViewerCount_ == viewerCount)
    {
        return;
    }

    this->liveViewerCount_ = viewerCount;
    this->liveStatusChanged.invoke();
}

void YouTubeLiveChat::setLive(bool live)
{
    if (this->live_ == live)
    {
        return;
    }

    qCWarning(chatterinoYouTube).nospace()
        << "[" << this->streamUrl_ << "] setLive " << (live ? "true" : "false")
        << " (videoId=" << this->videoId_ << ", title=\"" << this->liveTitle_
        << "\")";
    if (!live)
    {
        this->liveOwnerChannelId_.clear();
        this->resetModeratorPrivileges();
    }

    this->live_ = live;
    this->liveStatusChanged.invoke();
    if (live)
    {
        this->fetchUpdatedMetadata();
    }
}

void YouTubeLiveChat::setStatusText(QString text, bool notifyAsSystemMessage)
{
    if (this->statusText_ == text)
    {
        return;
    }

    this->statusText_ = std::move(text);
    this->liveStatusChanged.invoke();

    if (notifyAsSystemMessage)
    {
        this->systemMessageReceived.invoke(
            makeSystemStatusMessage(this->statusText_));
    }
}

bool YouTubeLiveChat::shouldResolveLiveStreamFromSource() const
{
    return maybeExtractVideoId(this->streamUrl_).isEmpty();
}

QString YouTubeLiveChat::resolvedSource() const
{
    return normalizeSource(this->streamUrl_);
}

const EmoteMap &YouTubeLiveChat::customEmotes() const
{
    return this->customEmotes_;
}

QString YouTubeLiveChat::maybeExtractVideoId(const QString &urlString)
{
    const auto trimmed = trimmedSource(urlString);
    if (isLikelyVideoId(trimmed))
    {
        return trimmed;
    }

    auto normalized = trimmed;
    if (!normalized.contains("://") &&
        (normalized.startsWith("www.", Qt::CaseInsensitive) ||
         normalized.startsWith("youtube.com/", Qt::CaseInsensitive) ||
         normalized.startsWith("m.youtube.com/", Qt::CaseInsensitive) ||
         normalized.startsWith("youtu.be/", Qt::CaseInsensitive)))
    {
        normalized.prepend("https://");
    }

    const QUrl url(normalized);
    if (!url.isValid())
    {
        return {};
    }

    const auto host = url.host().toLower();
    const auto path = url.path();
    const QUrlQuery query(url);

    if (query.hasQueryItem("v"))
    {
        const auto videoId = query.queryItemValue("v").trimmed();
        return isLikelyVideoId(videoId) ? videoId : QString{};
    }

    if (host.endsWith("youtu.be"))
    {
        const auto videoId = path.sliced(1).section('/', 0, 0).trimmed();
        return isLikelyVideoId(videoId) ? videoId : QString{};
    }

    if (path.startsWith("/live/"))
    {
        const auto videoId = path.section('/', 2, 2).trimmed();
        return isLikelyVideoId(videoId) ? videoId : QString{};
    }

    if (path.startsWith("/shorts/") || path.startsWith("/embed/"))
    {
        const auto videoId = path.section('/', 2, 2).trimmed();
        return isLikelyVideoId(videoId) ? videoId : QString{};
    }

    return {};
}

QString YouTubeLiveChat::normalizeSource(const QString &source)
{
    auto value = trimmedSource(source);
    if (value.isEmpty())
    {
        return {};
    }

    if (value.startsWith('@'))
    {
        return value.section('/', 0, 0).trimmed();
    }

    if (isLikelyChannelId(value))
    {
        return value;
    }

    if (!value.contains("://") && !value.contains('/') &&
        !value.contains('\\'))
    {
        return value;
    }

    if (value.startsWith("//"))
    {
        value.prepend("https:");
    }
    else if (value.startsWith('/'))
    {
        value.prepend("https://www.youtube.com");
    }
    else if (!value.contains("://") &&
             (value.startsWith("www.", Qt::CaseInsensitive) ||
              value.startsWith("youtube.com/", Qt::CaseInsensitive) ||
              value.startsWith("m.youtube.com/", Qt::CaseInsensitive)))
    {
        value.prepend("https://");
    }

    QUrl url(value);
    if (!url.isValid() || url.host().isEmpty())
    {
        return {};
    }

    const auto host = url.host().toLower();
    const auto path = trimTrailingSlash(url.path());
    const QUrlQuery query(url);
    if (host.endsWith("youtube.com") && path == "/embed/live_stream")
    {
        const auto channelId = query.queryItemValue("channel").trimmed();
        if (isLikelyChannelId(channelId))
        {
            return channelId;
        }
    }

    if (host.endsWith("youtube.com") && path.startsWith("/@"))
    {
        return path.section('/', 1, 1).trimmed();
    }
    if (host.endsWith("youtube.com") && path.startsWith("/channel/"))
    {
        const auto channelId = path.section('/', 2, 2).trimmed();
        if (isLikelyChannelId(channelId))
        {
            return channelId;
        }
    }

    if (host.endsWith("youtube.com"))
    {
        const auto firstSegment = path.section('/', 1, 1).trimmed();
        if (firstSegment == "c" || firstSegment == "user")
        {
            return path.section('/', 2, 2).trimmed();
        }

        if (!firstSegment.isEmpty() && firstSegment != "watch" &&
            firstSegment != "shorts" && firstSegment != "embed" &&
            firstSegment != "live" && firstSegment != "playlist" &&
            firstSegment != "results" && firstSegment != "feed")
        {
            return QUrl::fromPercentEncoding(firstSegment.toUtf8()).trimmed();
        }
    }

    return {};
}

bool YouTubeLiveChat::isLikelyChannelId(const QString &value)
{
    static const QRegularExpression channelIdRegex("^UC[A-Za-z0-9_-]{22}$");
    return channelIdRegex.match(value.trimmed()).hasMatch();
}

QString YouTubeLiveChat::sourceLivePath(const QString &source)
{
    if (source.startsWith('@'))
    {
        return QString("/%1/live").arg(source);
    }

    if (isLikelyChannelId(source))
    {
        return QString("/channel/%1/live").arg(source);
    }

    return {};
}

QString YouTubeLiveChat::sourceStreamsPath(const QString &source)
{
    if (source.startsWith('@'))
    {
        return QString("/%1/streams").arg(source);
    }

    if (isLikelyChannelId(source))
    {
        return QString("/channel/%1/streams").arg(source);
    }

    return {};
}

bool YouTubeLiveChat::isLikelyVideoId(const QString &value)
{
    static const QRegularExpression videoIdRegex("^[A-Za-z0-9_-]{11}$");

    const auto trimmed = value.trimmed();
    return trimmed != QStringLiteral("live_stream") &&
        videoIdRegex.match(trimmed).hasMatch();
}

QString YouTubeLiveChat::extractFirstMatch(const QString &text,
                                           const QStringList &patterns)
{
    for (const auto &pattern : patterns)
    {
        const QRegularExpression regex(pattern);
        const auto match = regex.match(text);
        if (match.hasMatch())
        {
            return match.captured(1);
        }
    }

    return {};
}

QString YouTubeLiveChat::extractLiveChatContinuation(
    const QJsonObject &liveChatRenderer)
{
    const auto continuations = liveChatRenderer["continuations"].toArray();
    for (const auto &continuationValue : continuations)
    {
        const auto continuation =
            extractContinuationFromObject(continuationValue.toObject());
        if (!continuation.isEmpty())
        {
            return continuation;
        }
    }

    const auto items =
        liveChatRenderer["header"]
            .toObject()["liveChatHeaderRenderer"]
            .toObject()["viewSelector"]
            .toObject()["sortFilterSubMenuRenderer"]
            .toObject()["subMenuItems"]
            .toArray();
    for (const auto &itemValue : items)
    {
        const auto continuation =
            extractContinuationFromObject(
                itemValue.toObject()["continuation"].toObject());
        if (!continuation.isEmpty())
        {
            return continuation;
        }
    }

    return {};
}

QString YouTubeLiveChat::extractVideoChannelId(const QString &html)
{
    return extractFirstMatch(
        html,
        {R"yt(<meta itemprop="identifier" content="(UC[A-Za-z0-9_-]{22})")yt",
         R"yt("channelMetadataRenderer":\{[^}]*"externalId":"(UC[A-Za-z0-9_-]{22})")yt",
         R"yt("externalId":"(UC[A-Za-z0-9_-]{22})")yt",
         R"yt("browseId":"(UC[A-Za-z0-9_-]{22})")yt",
         R"yt("channelId":"(UC[A-Za-z0-9_-]{22})")yt",
         R"yt(itemprop="channelId" content="(UC[A-Za-z0-9_-]{22})")yt"});
}

QString YouTubeLiveChat::extractChannelAvatarUrl(const QString &html)
{
    // On a channel page YouTube sets og:image / image_src to the channel's
    // avatar (a yt3.ggpht.com / yt3.googleusercontent.com URL). Fall back to
    // the avatar thumbnail embedded in ytInitialData, whose URL is JSON-escaped
    // (\/), so unescape the slashes before handing it back.
    auto avatar = extractFirstMatch(
        html,
        {R"yt(<meta property="og:image" content="(https://yt3\.[^"]+)")yt",
         R"yt(<link rel="image_src" href="(https://yt3\.[^"]+)")yt",
         R"yt("avatar":\{"thumbnails":\[\{"url":"(https:[^"]+yt3\.[^"]+)")yt"});
    avatar.replace(QStringLiteral("\\/"), QStringLiteral("/"));
    return avatar;
}

QString YouTubeLiveChat::extractEmbedLiveVideoId(const QString &html)
{
    // The page we LOAD is /embed/live_stream?channel=...; YouTube echoes that
    // path back in fallback markup when the channel isn't live, so the embed-
    // path regexes will happily capture the literal "live_stream" (exactly 11
    // chars of [A-Za-z0-9_-]) and pass it back as a real videoId. Filter it.
    const auto videoId = extractFirstMatch(
        html,
        {R"yt(<link rel="canonical" href="https://www\.youtube\.com/watch\?v=([A-Za-z0-9_-]{11})")yt",
         R"yt(<meta property="og:url" content="https://www\.youtube\.com/watch\?v=([A-Za-z0-9_-]{11})")yt",
         R"yt(itemprop="url" content="https://www\.youtube\.com/watch\?v=([A-Za-z0-9_-]{11})")yt",
         R"yt("canonicalBaseUrl":"\\/watch\\?v=([A-Za-z0-9_-]{11})")yt",
         R"yt(https://www\.youtube\.com/embed/([A-Za-z0-9_-]{11}))yt",
         R"yt(/embed/([A-Za-z0-9_-]{11}))yt",
         R"yt(\\/embed\\/([A-Za-z0-9_-]{11}))yt"});
    if (videoId == QStringLiteral("live_stream"))
    {
        return {};
    }
    return isLikelyVideoId(videoId) ? videoId : QString{};
}

QString YouTubeLiveChat::extractLiveVideoId(const QString &html,
                                            bool allowPageCanonical)
{
    if (allowPageCanonical)
    {
        const auto canonicalVideoId = extractFirstMatch(
            html, {R"yt(<link rel="canonical" href="https://www\.youtube\.com/watch\?v=([A-Za-z0-9_-]{11})")yt",
                   R"yt(<meta property="og:url" content="https://www\.youtube\.com/watch\?v=([A-Za-z0-9_-]{11})")yt",
                   R"yt(itemprop="url" content="https://www\.youtube\.com/watch\?v=([A-Za-z0-9_-]{11})")yt",
                   R"yt("canonicalBaseUrl":"\\/watch\\?v=([A-Za-z0-9_-]{11})")yt"});
        if (!canonicalVideoId.isEmpty())
        {
            return canonicalVideoId;
        }
    }

    const auto lockupVideoId = extractLiveLockupVideoId(html);
    if (!lockupVideoId.isEmpty())
    {
        return lockupVideoId;
    }

    const auto videoRendererVideoId = extractLiveVideoRendererVideoId(html);
    if (!videoRendererVideoId.isEmpty())
    {
        return videoRendererVideoId;
    }

    const auto watchEndpointVideoId = extractLiveWatchEndpointVideoId(html);
    if (!watchEndpointVideoId.isEmpty())
    {
        return watchEndpointVideoId;
    }

    return {};
}

QString YouTubeLiveChat::extractLiveStreamTitle(const QJsonObject &nextResponse)
{
    const auto contents =
        nextResponse["contents"]
            .toObject()["twoColumnWatchNextResults"]
            .toObject()["results"]
            .toObject()["results"]
            .toObject()["contents"]
            .toArray();

    for (const auto &itemValue : contents)
    {
        const auto item = itemValue.toObject();
        const auto primary = item["videoPrimaryInfoRenderer"].toObject();
        if (primary.isEmpty())
        {
            continue;
        }

        const auto title = parseText(primary["title"]);
        if (!title.isEmpty())
        {
            return title;
        }
    }

    return {};
}

QString YouTubeLiveChat::extractLiveOwnerChannelId(
    const QJsonObject &nextResponse)
{
    const auto contents =
        nextResponse["contents"]
            .toObject()["twoColumnWatchNextResults"]
            .toObject()["results"]
            .toObject()["results"]
            .toObject()["contents"]
            .toArray();

    for (const auto &itemValue : contents)
    {
        const auto secondary =
            itemValue.toObject()["videoSecondaryInfoRenderer"].toObject();
        if (secondary.isEmpty())
        {
            continue;
        }

        const auto owner =
            secondary["owner"].toObject()["videoOwnerRenderer"].toObject();
        if (!owner.isEmpty())
        {
            const auto ownerId = extractFirstBrowseChannelId(owner);
            if (!ownerId.isEmpty())
            {
                return ownerId;
            }
        }

        const auto fallbackOwnerId = extractFirstBrowseChannelId(secondary);
        if (!fallbackOwnerId.isEmpty())
        {
            return fallbackOwnerId;
        }
    }

    return {};
}

QDateTime YouTubeLiveChat::extractLiveStartTime(const QJsonObject &nextResponse)
{
    const auto liveBroadcastDetails = extractLiveBroadcastDetails(nextResponse);
    return parseYouTubeIsoDateTime(
        liveBroadcastDetails["startTimestamp"].toString());
}

bool YouTubeLiveChat::rendererHasModeratorBadge(const QJsonObject &renderer)
{
    const auto badges = renderer["authorBadges"].toArray();
    if (badges.isEmpty())
    {
        return false;
    }

    const auto badgeText = QString::fromUtf8(
                               QJsonDocument(badges).toJson(
                                   QJsonDocument::Compact))
                               .toLower();
    return badgeText.contains(QStringLiteral("moderator")) ||
           badgeText.contains(QStringLiteral("owner"));
}

QString YouTubeLiveChat::parseText(const QJsonValue &value)
{
    auto object = value.toObject();
    if (object.contains("simpleText"))
    {
        return object["simpleText"].toString();
    }

    QString text;
    for (const auto &runValue : object["runs"].toArray())
    {
        auto run = runValue.toObject();
        if (run.contains("text"))
        {
            text.append(run["text"].toString());
        }
        else if (run.contains("emoji"))
        {
            auto emoji = run["emoji"].toObject();
            text.append(youtubeEmojiFallbackText(emoji));
        }
    }

    return text.trimmed();
}

uint64_t YouTubeLiveChat::extractLiveViewerCountFromNextResponse(
    const QJsonObject &nextResponse)
{
    // Scoped to the primary info column on purpose (mirrors
    // extractIsLiveFromNextResponse): the full /next response also carries
    // secondaryResults (recommended videos) whose "watching now" figures
    // belong to unrelated live streams. The authoritative count comes from
    // updated_metadata anyway, so correctness beats recall here.
    const auto contents =
        nextResponse["contents"]
            .toObject()["twoColumnWatchNextResults"]
            .toObject()["results"]
            .toObject()["results"]
            .toObject()["contents"]
            .toArray();

    for (const auto &itemValue : contents)
    {
        const auto primary =
            itemValue.toObject()["videoPrimaryInfoRenderer"].toObject();
        if (primary.isEmpty())
        {
            continue;
        }

        const auto count = extractLiveViewerCount(primary);
        if (count > 0)
        {
            return count;
        }
    }

    return 0;
}

uint64_t YouTubeLiveChat::extractLiveViewerCount(const QJsonValue &value)
{
    auto parseLiveTextValue = [](const QJsonValue &textValue) -> uint64_t {
        const auto text = textValue.isString() ? textValue.toString().trimmed()
                                               : parseText(textValue);
        if (!isLiveViewerCountText(text))
        {
            return 0;
        }

        return parseYouTubeViewerCountText(text);
    };

    if (value.isString())
    {
        return parseLiveTextValue(value);
    }

    if (value.isArray())
    {
        for (const auto &item : value.toArray())
        {
            const auto count = extractLiveViewerCount(item);
            if (count > 0)
            {
                return count;
            }
        }
        return 0;
    }

    if (!value.isObject())
    {
        return 0;
    }

    const auto object = value.toObject();

    for (const auto &key :
         {QStringLiteral("viewerCount"), QStringLiteral("viewerCountText")})
    {
        const auto count = parseViewerCount(object[key]);
        if (count > 0)
        {
            return count;
        }
    }

    const auto videoViewCountRenderer =
        object["videoViewCountRenderer"].toObject();
    if (!videoViewCountRenderer.isEmpty())
    {
        for (const auto &key : {QStringLiteral("viewCount"),
                                QStringLiteral("shortViewCount"),
                                QStringLiteral("extraShortViewCount")})
        {
            const auto count = parseLiveTextValue(videoViewCountRenderer[key]);
            if (count > 0)
            {
                return count;
            }
        }
    }

    const auto textCount = parseLiveTextValue(value);
    if (textCount > 0)
    {
        return textCount;
    }

    for (auto it = object.begin(); it != object.end(); ++it)
    {
        const auto key = it.key();
        if (key == QStringLiteral("actions") ||
            key == QStringLiteral("message") ||
            key == QStringLiteral("authorName"))
        {
            continue;
        }

        const auto count = extractLiveViewerCount(it.value());
        if (count > 0)
        {
            return count;
        }
    }

    return 0;
}

QString normalizedYouTubeAuthorName(QString author)
{
    author = author.trimmed();
    if (author.size() > 1 && author.startsWith('@'))
    {
        author.remove(0, 1);
    }
    return author;
}

uint64_t YouTubeLiveChat::parseViewerCount(const QJsonValue &value)
{
    if (value.isDouble())
    {
        const auto count = value.toInteger(0);
        return count > 0 ? static_cast<uint64_t>(count) : 0;
    }

    const auto text = value.isString() ? value.toString().trimmed()
                                       : parseText(value);
    if (text.isEmpty())
    {
        return 0;
    }

    return parseYouTubeViewerCountText(text);
}

QString compactMembershipGiftText(const QString &text, const QString &author)
{
    static const QRegularExpression countRegex(R"((\d+))");
    const auto match = countRegex.match(text);
    if (!match.hasMatch() ||
        !text.contains(QStringLiteral("membership"), Qt::CaseInsensitive))
    {
        return text;
    }

    const auto count = match.captured(1).toInt();
    const auto unit = count == 1 ? QStringLiteral("membership")
                                : QStringLiteral("memberships");
    return QStringLiteral("%1 gifted %2 %3")
        .arg(author, QString::number(count), unit);
}

MessagePtr YouTubeLiveChat::parseRendererMessage(const QJsonObject &renderer,
                                                 const QString &rendererName,
                                                 const QString &channelName,
                                                 EmoteMap *customEmoteSink)
{
    if (renderer.isEmpty())
    {
        return nullptr;
    }

    QString text;
    std::vector<QJsonValue> renderedTextParts;
    QJsonValue authorNameValue = renderer["authorName"];
    QJsonObject authorSource = renderer;
    QString authorId = renderer["authorExternalChannelId"].toString();
    MessageFlags flags;
    bool compactMembershipGift = false;

    if (rendererName == "liveChatTextMessageRenderer")
    {
        const auto message = renderer["message"];
        text = parseText(message);
        renderedTextParts.emplace_back(message);
    }
    else if (rendererName == "liveChatPaidMessageRenderer")
    {
        flags.set(MessageFlag::ElevatedMessage);
        const auto purchaseAmount = renderer["purchaseAmountText"];
        text = parseText(purchaseAmount);
        if (!text.isEmpty())
        {
            renderedTextParts.emplace_back(purchaseAmount);
        }

        const auto message = renderer["message"];
        const auto body = parseText(message);
        if (!body.isEmpty())
        {
            if (!text.isEmpty())
            {
                text += " ";
            }
            text += body;
            renderedTextParts.emplace_back(message);
        }
    }
    else if (rendererName == "liveChatPaidStickerRenderer")
    {
        flags.set(MessageFlag::ElevatedMessage);
        const auto purchaseAmount = renderer["purchaseAmountText"];
        text = parseText(purchaseAmount);
        renderedTextParts.emplace_back(purchaseAmount);
    }
    else if (rendererName == "liveChatMembershipItemRenderer")
    {
        flags.set(MessageFlag::Subscription);
        const auto headerSubtext = renderer["headerSubtext"];
        text = parseText(headerSubtext);
        if (!text.isEmpty())
        {
            renderedTextParts.emplace_back(headerSubtext);
        }

        const auto message = renderer["message"];
        const auto body = parseText(message);
        if (!body.isEmpty())
        {
            if (!text.isEmpty())
            {
                text += " ";
            }
            text += body;
            renderedTextParts.emplace_back(message);
        }
    }
    else if (rendererName ==
             "liveChatSponsorshipsGiftPurchaseAnnouncementRenderer")
    {
        flags.set(MessageFlag::Subscription);
        const auto header =
            renderer["header"].toObject()["liveChatSponsorshipsHeaderRenderer"]
                .toObject();
        authorNameValue = header["authorName"];
        authorSource = header;
        const auto primaryText = header["primaryText"];
        text = parseText(primaryText);
        renderedTextParts.emplace_back(primaryText);
        compactMembershipGift = true;
        if (authorId.isEmpty())
        {
            authorId = header["authorExternalChannelId"].toString();
        }
    }
    else if (rendererName ==
             "liveChatSponsorshipsGiftRedemptionAnnouncementRenderer")
    {
        return nullptr;
    }
    else if (rendererName == "giftMessageViewModel")
    {
        // Jewels gift authorName/text use {content, styleRuns} rather than
        // the standard runs/simpleText shape; wrap the content in a
        // simpleText object so parseText() handles it uniformly below.
        flags.set(MessageFlag::ElevatedMessage);
        auto authorContent =
            renderer["authorName"].toObject()["content"].toString().trimmed();
        if (authorContent.startsWith(u'@'))
        {
            authorContent = authorContent.mid(1);
        }
        authorNameValue = QJsonObject{{"simpleText", authorContent}};
        text = renderer["text"].toObject()["content"].toString().trimmed();
    }
    else if (rendererName == "liveChatPlaceholderItemRenderer" ||
             rendererName == "liveChatViewerEngagementMessageRenderer")
    {
        // Placeholder items are filler slots that precede a real message;
        // engagement renderers are YouTube's welcome/rules system notices.
        return nullptr;
    }
    else
    {
        // Cap diagnostic dumps at five per session so a stream with many
        // unknown renderer types can't flood the log.
        static std::atomic<int> warningCount{0};
        const int n = warningCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 5)
        {
            qCWarning(chatterinoYouTube).nospace()
                << "Unhandled live chat renderer: " << rendererName;
            qCWarning(chatterinoYouTube)
                << QJsonDocument(renderer).toJson(QJsonDocument::Compact);
        }
        else if (n == 5)
        {
            qCWarning(chatterinoYouTube)
                << "Further unhandled-renderer warnings suppressed for "
                   "this session";
        }
        return nullptr;
    }

    const auto author = normalizedYouTubeAuthorName(parseText(authorNameValue));
    if (author.isEmpty())
    {
        return nullptr;
    }
    if (compactMembershipGift)
    {
        text = compactMembershipGiftText(text, author);
    }
    const auto authorColor = youtubeAuthorNameColor(authorSource);

    MessageBuilder builder;
    builder->flags = flags;
    builder->platform = MessagePlatform::YouTube;
    builder->id = renderer["id"].toString();
    builder->loginName = author;
    builder->displayName = author;
    builder->localizedName = author;
    builder->userID = authorId;
    builder->channelName = channelName;
    builder->messageText = text;
    builder->searchText = author + ": " + text;
    builder->serverReceivedTime =
        parseTimestampUsec(renderer["timestampUsec"].toString());
    if (authorColor)
    {
        builder->usernameColor = *authorColor;
    }
    builder.emplace<TimestampElement>(
        builder->serverReceivedTime.toLocalTime().time());
    appendYouTubeMembershipBadges(builder, authorSource);
    builder
        .emplace<TextElement>(author + ":", MessageElementFlag::Username,
                              authorColor ? MessageColor(*authorColor)
                                          : MessageColor::Text,
                              FontStyle::ChatMediumBold)
        ->setLink({Link::UserInfo, author});
    if (compactMembershipGift || renderedTextParts.empty())
    {
        appendSimpleWords(builder, text);
    }
    else
    {
        appendYouTubeTextParts(builder, renderedTextParts, text,
                               customEmoteSink);
    }
    return builder.release();
}

}  // namespace chatterino
