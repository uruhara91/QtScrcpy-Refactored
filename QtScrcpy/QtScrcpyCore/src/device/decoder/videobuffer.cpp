#include "videobuffer.h"
#include "avframeconvert.h"

#include <QDebug>
#include <QMutexLocker>

extern "C" {
#include "libavutil/imgutils.h"
}

VideoBuffer::VideoBuffer(QObject *parent) : QObject(parent) {
    m_latestFrame = av_frame_alloc();
    if (!m_latestFrame) {
        qCritical("VideoBuffer: failed to allocate frame storage");
    }

    connect(&m_fpsCounter, &FpsCounter::updateFPS, this, &VideoBuffer::updateFPS);
    m_fpsCounter.start();
}

VideoBuffer::~VideoBuffer() {
    m_fpsCounter.stop();

    QMutexLocker locker(&m_frameMutex);
    if (m_latestFrame) {
        av_frame_free(&m_latestFrame);
    }
}

void VideoBuffer::updateLatestFrame(const AVFrame* frame) {
    if (!frame || !m_latestFrame) return;

    bool stored = false;
    {
        QMutexLocker locker(&m_frameMutex);
        av_frame_unref(m_latestFrame);
        if (av_frame_ref(m_latestFrame, frame) >= 0) {
            ++m_frameGeneration;
            stored = true;
        }
    }

    if (stored) {
        m_fpsCounter.addRenderedFrame();
    }
}

void VideoBuffer::peekFrameInfo(int &width, int &height, int &format) {
    QMutexLocker locker(&m_frameMutex);
    if (m_latestFrame && m_latestFrame->width > 0 && m_latestFrame->height > 0) {
        width = m_latestFrame->width;
        height = m_latestFrame->height;
        format = m_latestFrame->format;
    } else {
        width = 0;
        height = 0;
        format = -1;
    }
}

void VideoBuffer::peekRenderedFrame(std::function<void(int, int, uint8_t*)> onFrame) {
    if (!onFrame) return;

    AVFrame* snapshot = av_frame_alloc();
    if (!snapshot) return;

    std::uint64_t generation = 0;
    {
        QMutexLocker locker(&m_frameMutex);
        if (!m_latestFrame || m_latestFrame->width <= 0 || m_latestFrame->height <= 0 ||
            av_frame_ref(snapshot, m_latestFrame) < 0) {
            av_frame_free(&snapshot);
            return;
        }
        generation = m_frameGeneration;
    }

    const int width = snapshot->width;
    const int height = snapshot->height;
    const int format = snapshot->format;

    QMutexLocker cacheLocker(&m_cacheMutex);
    bool frameReady = generation == m_cachedGeneration &&
                      width == m_cachedWidth &&
                      height == m_cachedHeight &&
                      format == m_cachedFormat &&
                      !m_cachedFrame.empty();

    if (!frameReady) {
        const int size = av_image_get_buffer_size(AV_PIX_FMT_RGB32, width, height, 1);
        if (size <= 0) {
            av_frame_free(&snapshot);
            return;
        }

        m_cachedFrame.resize(static_cast<std::size_t>(size));

        AVFrame* rgbFrame = av_frame_alloc();
        if (rgbFrame) {
            const int fillResult = av_image_fill_arrays(
                rgbFrame->data,
                rgbFrame->linesize,
                m_cachedFrame.data(),
                AV_PIX_FMT_RGB32,
                width,
                height,
                1);

            if (fillResult >= 0) {
                AVFrameConvert convert;
                convert.setSrcFrameInfo(width, height, static_cast<AVPixelFormat>(format));
                convert.setDstFrameInfo(width, height, AV_PIX_FMT_RGB32);

                if (convert.init()) {
                    convert.convert(snapshot, rgbFrame);
                    m_cachedGeneration = generation;
                    m_cachedWidth = width;
                    m_cachedHeight = height;
                    m_cachedFormat = format;
                    frameReady = true;
                }
                convert.deInit();
            }

            av_frame_free(&rgbFrame);
        }
    }

    av_frame_free(&snapshot);

    if (frameReady && !m_cachedFrame.empty()) {
        onFrame(m_cachedWidth, m_cachedHeight, m_cachedFrame.data());
    }
}
