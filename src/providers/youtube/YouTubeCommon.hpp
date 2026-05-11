#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <cstddef>

#if __has_include("YouTubeOAuthSecrets.local.hpp")
#    include "YouTubeOAuthSecrets.local.hpp"
#endif

namespace chatterino {

inline QString youTubeOAuthClientID()
{
    return QStringLiteral(
        "8534918506-k76hkejg2r9gjbfq7sqkqbjv1uvvg2ft.apps.googleusercontent.com");
}

inline QString youTubeOAuthClientSecret()
{
#ifdef CHATTERINO_RELEASE_AUTH_BLOB_OBFUSCATED
    QByteArray decoded;
    decoded.reserve(
        static_cast<qsizetype>(release_auth_blob::DATA.size()));

    for (std::size_t i = 0; i < release_auth_blob::DATA.size(); ++i)
    {
        const auto mask = static_cast<unsigned char>(
            (static_cast<unsigned int>(release_auth_blob::KEY_A) +
             static_cast<unsigned int>(i) * 31U) ^
            (static_cast<unsigned int>(release_auth_blob::KEY_B) +
             static_cast<unsigned int>(i) * 17U) ^
            (static_cast<unsigned int>(release_auth_blob::KEY_C) +
             static_cast<unsigned int>(i) * 13U));

        decoded.append(static_cast<char>(release_auth_blob::DATA[i] ^ mask));
    }

    return QString::fromUtf8(decoded).trimmed();
#else
    return qEnvironmentVariable("MRAB").trimmed();
#endif
}

inline QString youTubeOAuthScope()
{
    return QStringLiteral("https://www.googleapis.com/auth/youtube.force-ssl");
}

}  // namespace chatterino
