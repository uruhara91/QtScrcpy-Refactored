#include "videobuffer.h"
#include "avframeconvert.h"

#include <QDebug>
#include <QMetaObject>
#include <QMutexLocker>
#include <utility>

extern "C" {
#include "libavutil/imgutils.h"
}

VideoBuffer::VideoBuffer(QObject *parent)
    : QObject(parent)
{
    connect(&m_fpsCounter, &FpsCounter::updateFPS,
            this, &VideoBuffer::updateFPS);
    m_fpsCounter.start();
}

VideoBuffer::~VideoBuffer()
{
    m_fpsCounter.stop();

    QMutexLocker locker(&m_requestMutex);
    m_pendingCaptures.clear();
    m_captureRequested.store(false, std::memory_order_release);
}

void VideoBuffer::updateLatestFrame(const AVFrame *frame)
{
    if (!frame) return;

    m_fpsCounter.addRenderedFrame();

    if (!m_captureRequested.load(std::memory_order_acquire)) {
        return;
    }

    std::vector<FrameCallback> callbacks;
    {
        QMutexLocker locker(&m_requestMutex);
        if (m_pendingCaptures.empty()) {
            m_captureRequested.store(false, std::memory_order_release);
            return;
        }

        callbacks.swap(m_pendingCaptures);
        m_captureRequested.store(false, std::memory_order_release);
    }

    AVFrame *rawSnapshot = av_frame_alloc();
    if (!rawSnapshot || av_frame_ref(rawSnapshot, frame) < 0) {
        if (rawSnapshot) av_frame_free(&rawSnapshot);
        return;
    }

    SharedFrame snapshot(rawSnapshot, FrameDeleter{});
    const bool queued = QMetaObject::invokeMethod(
        this,
        [this, snapshot = std::move(snapshot),
         callbacks = std::move(callbacks)]() mutable {
            deliverSnapshot(std::move(snapshot), std::move(callbacks));
        },
        Qt::QueuedConnection);

    if (!queued) {
        qWarning("VideoBuffer: failed to queue screenshot conversion");
    }
}

void VideoBuffer::peekRenderedFrame(FrameCallback onFrame)
{
    if (!onFrame) return;

    {
        QMutexLocker locker(&m_requestMutex);
        m_pendingCaptures.push_back(std::move(onFrame));
        m_captureRequested.store(true, std::memory_order_release);
    }
}

void VideoBuffer::deliverSnapshot(SharedFrame snapshot,
                                  std::vector<FrameCallback> callbacks)
{
    if (!snapshot || callbacks.empty() ||
        snapshot->width <= 0 || snapshot->height <= 0) {
        return;
    }

    const int width = snapshot->width;
    const int height = snapshot->height;
    const int format = snapshot->format;
    const int size = av_image_get_buffer_size(
        AV_PIX_FMT_RGB32, width, height, 1);
    if (size <= 0) return;

    std::vector<uint8_t> rgbBuffer(static_cast<std::size_t>(size));
    AVFrame *rawRgbFrame = av_frame_alloc();
    if (!rawRgbFrame) return;

    SharedFrame rgbFrame(rawRgbFrame, FrameDeleter{});
    const int fillResult = av_image_fill_arrays(
        rgbFrame->data,
        rgbFrame->linesize,
        rgbBuffer.data(),
        AV_PIX_FMT_RGB32,
        width,
        height,
        1);
    if (fillResult < 0) return;

    AVFrameConvert convert;
    convert.setSrcFrameInfo(
        width, height, static_cast<AVPixelFormat>(format));
    convert.setDstFrameInfo(width, height, AV_PIX_FMT_RGB32);
    if (!convert.init()) return;

    convert.convert(snapshot.get(), rgbFrame.get());
    convert.deInit();

    for (FrameCallback &callback : callbacks) {
        if (callback) callback(width, height, rgbBuffer.data());
    }
}
