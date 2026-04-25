#pragma once

#include <QObject>

#include <QByteArray>
#include <QString>

#include <functional>

namespace chatterino {

/// A very minimal local-only HTTP server.
///
/// It's intended to be used for short-lived authentication procedures, such as
/// local OAuth callbacks or temporary browser-based setup wizards.
class HttpServer : public QObject
{
public:
    struct Request {
        QString method;
        QString target;
        QByteArray body;
    };

    struct Response {
        unsigned status = 200;
        QByteArray body;
        QByteArray contentType = "text/plain; charset=utf-8";
    };

    HttpServer(uint16_t port, QObject *parent = nullptr);

    using HandlerCb = std::function<Response(const Request &)>;

    void setHandler(HandlerCb handler);
    const HandlerCb &handler() const;

private:
    HandlerCb handler_;
};

}  // namespace chatterino
