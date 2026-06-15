#include "videobuffer.h"
#include "avframeconvert.h"

#include <QDebug>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThread>
#include <utility>

extern "C" {
#include "libavutil/imgutils.h"
}

VideoBuffer::VideoBuffer(QObject *parent)
    : QObject(parent)
{
    connect(&m_fpsCounter, &FpsCounter::updateFPS,
            this, &VideoBuffer::updateFPS);
}

VideoBuffer::~VideoBuffer()
{
    m_frameWork.store(WorkNone, std::memory_order_release);
    m_fpsCounter.stop();

    QMutexLocker locker(&m_requestMutex);
    m_pendingCaptures.clear();
}

void VideoBuffer::setFpsCounterEnabled(bool enabled)
{
    constexpr std::uint8_t fpsMask = WorkFps;
    bool changed = false;

    if (enabled) {
        const std::uint8_t previous = m_frameWork.fetch_or(
            fpsMask, std::memory_order_acq_rel);
        changed = (previous & fpsMask) == 0;
    } else {
        const std::uint8_t previous = m_frameWork.fetch_and(
            static_cast<std::uint8_t>(~fpsMask),
            std::memory_order_acq_rel);
        changed = (previous & fpsMask) != 0;
    }

    if (!changed) return;

    if (QThread::currentThread() == thread()) {
        applyFpsCounterState(enabled);
        return;
    }

    QMetaObject::invokeMethod(
        this,
        [this, enabled]() { applyFpsCounterState(enabled); },
        Qt::QueuedConnection);
}

void VideoBuffer::applyFpsCounterState(bool enabled)
{
    if (enabled) m_fpsCounter.start();
    else m_fpsCounter.stop();
}

void VideoBuffer::updateLatestFrame(const AVFrame *frame)
{
    if (!frame) return;

    const std::uint8_t work = m_frameWork.load(std::memory_order_acquire);
    if (work == WorkNone) return;

    if ((work & WorkFps) != 0) {
        m_fpsCounter.addRenderedFrame();
    }

    if ((work & WorkCapture) == 0) return;

    std::vector<FrameCallback> callbacks;
    {
        QMutexLocker locker(&m_requestMutex);
        if (m_pendingCaptures.empty()) {
            m_frameWork.fetch_and(
                static_cast<std::uint8_t>(~WorkCapture),
                std::memory_order_release);
            return;
        }

        callbacks.swap(m_pendingCaptures);
        m_frameWork.fetch_and(
            static_cast<std::uint8_t>(~WorkCapture),
            std::memory_order_release);
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

    QMutexLocker locker(&m_requestMutex);
    m_pendingCaptures.push_back(std::move(onFrame));
    m_frameWork.fetch_or(WorkCapture, std::memory_order_release);
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

    const bool converted = convert.convert(snapshot.get(), rgbFrame.get());
    convert.deInit();
    if (!converted) return;

    for (FrameCallback &callback : callbacks) {
        if (callback) callback(width, height, rgbBuffer.data());
    }
}
