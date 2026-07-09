// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"
#include "util/DebugCount.hpp"

#include <boost/variant.hpp>
#include <pajlada/signals/signal.hpp>
#include <QList>
#include <QNetworkReply>
#include <QPixmap>
#include <QString>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace chatterino {

class Image;

}  // namespace chatterino

namespace chatterino::detail {

struct Frame {
    QPixmap image;
    int duration;
};

class Frames
{
public:
    Frames();
    Frames(QList<Frame> &&frames);
    ~Frames();

    Frames(const Frames &) = delete;
    Frames &operator=(const Frames &) = delete;

    Frames(Frames &&) = delete;
    Frames &operator=(Frames &&) = delete;

    void clear();
    bool empty() const;
    bool animated() const;
    void advance();
    std::optional<QPixmap> current() const;
    std::optional<QPixmap> first() const;

private:
    int64_t memoryUsage() const;
    void processOffset();
    QList<Frame> items_;
    QList<Frame>::size_type index_{0};
    int durationOffset_{0};
    pajlada::Signals::Connection gifTimerConnection_;
};

QList<Frame> readFrames(QImageReader &reader, const Url &url);
void assignFrames(std::weak_ptr<Image> weak, QList<Frame> parsed);

}  // namespace chatterino::detail

namespace chatterino {

class Image;
using ImagePtr = std::shared_ptr<Image>;

/// @brief Classifies an image-load failure as transient (worth retrying) or
/// definitive.
///
/// Transient failures are network/transport-layer errors (timeout, connection
/// reset, DNS hiccup, ...) that produced no HTTP response, plus HTTP 5xx / 408 /
/// 429 responses. Definitive failures are the remaining resolved HTTP statuses
/// (404, 410, other 4xx). @p httpStatus is the HTTP status code if a response
/// was received, otherwise `std::nullopt`.
bool isTransientImageLoadError(QNetworkReply::NetworkError error,
                               std::optional<int> httpStatus);

/// This class is thread safe.
class Image : public std::enable_shared_from_this<Image>
{
public:
    // Maximum amount of RAM used by the image in bytes.
    static constexpr int maxBytesRam = 20 * 1024 * 1024;

    // Number of times a transiently-failing load is retried before the image
    // is given up on and marked empty.
    static constexpr int maxLoadAttempts = 3;

    ~Image();

    Image(const Image &) = delete;
    Image &operator=(const Image &) = delete;

    Image(Image &&) = delete;
    Image &operator=(Image &&) = delete;

    static ImagePtr fromUrl(const Url &url, qreal scale = 1,
                            QSize expectedSize = {});
    static ImagePtr fromResourcePixmap(const QPixmap &pixmap, qreal scale = 1);
    static ImagePtr getEmpty();

    static ImagePtr fromAutoscaledUrl(const Url &url, uint16_t autoScale);

    const Url &url() const;
    bool loaded() const;
    // either returns the current pixmap, or triggers loading it (lazy loading)
    std::optional<QPixmap> pixmapOrLoad() const;
    void load() const;
    qreal scale() const;
    bool isEmpty() const;
    int width() const;
    int height() const;
    QSizeF size() const;
    bool animated() const;

    bool operator==(const Image &image) = delete;

private:
    Image();
    Image(const Url &url, qreal scale, QSize expectedSize);
    Image(qreal scale);

    void setPixmap(const QPixmap &pixmap);
    void actuallyLoad();
    /// @brief Handles a failed load attempt (may be called from a worker
    /// thread). On a transient failure the image is re-armed for another load
    /// attempt, up to `maxLoadAttempts`; otherwise (definitive failure or the
    /// retry budget is exhausted) it is marked permanently empty. The member
    /// mutations are hopped onto the GUI thread.
    void handleLoadFailure(bool transient);
    void expireFrames();

    const Url url_{};
    qreal scale_{1};
    /// @brief The expected size of this image once its loaded.
    ///
    /// This doesn't represent the actual size (it can be different) - it's
    /// just an estimation and provided to avoid (large) layout shifts when
    /// loading images.
    const QSize expectedSize_{16, 16};
    std::atomic_bool empty_{false};

    bool shouldLoad_{false};

    // Number of load attempts that have transiently failed so far. GUI thread
    // only (mutated via handleLoadFailure()'s GUI-thread hop and expireFrames).
    int loadAttempts_{0};

    /// Size this image should take when loaded (in both dimensions).
    ///
    /// This is used for images that have an unknown scale when they're created
    /// (i.e. the scale is only known after the image is loaded).
    ///
    /// Upon creation, only `expectedSize_` is set to `(autoScale, autoScale)`.
    /// When the image is loaded, `scale_` is set to `autoScale / actualSize`.
    std::optional<uint16_t> autoScale_;

    mutable std::chrono::time_point<std::chrono::steady_clock> lastUsed_;

    // gui thread only
    std::unique_ptr<detail::Frames> frames_;

    friend class ImageExpirationPool;
    friend void detail::assignFrames(std::weak_ptr<Image>,
                                     QList<detail::Frame>);
};

// forward-declarable function that calls Image::getEmpty() under the hood.
ImagePtr getEmptyImagePtr();

#ifndef DISABLE_IMAGE_EXPIRATION_POOL

class ImageExpirationPool
{
public:
    ImageExpirationPool();
    static ImageExpirationPool &instance();

    void addImagePtr(ImagePtr imgPtr);
    void removeImagePtr(Image *rawPtr);

    /**
     * @brief Frees frame data for all images that ImagePool deems to have expired.
     * 
     * Expiration is based on last accessed time of the Image, stored in Image::lastUsed_.
     * Must be ran in the GUI thread.
     */
    void freeOld();

    /*
     * Debug function that unloads all images in the pool. This is intended to
     * test for possible memory leaks from tracked images.
     */
    void freeAll();

    // Timer to periodically run freeOld()
    QTimer *freeTimer_;
    std::map<Image *, std::weak_ptr<Image>> allImages_;
    std::mutex mutex_;
};

#endif

}  // namespace chatterino
