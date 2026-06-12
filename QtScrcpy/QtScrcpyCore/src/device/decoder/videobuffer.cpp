#include "videobuffer.h"
#include "avframeconvert.h"
#include <QThread>

extern "C" {
#include "libavutil/imgutils.h"
}

VideoBuffer::VideoBuffer(QObject *parent) : QObject(parent) {
    m_latestFrame = av_frame_alloc();
    connect(&m_fpsCounter, &FpsCounter::updateFPS, this, &VideoBuffer::updateFPS);
    m_fpsCounter.start();
}

VideoBuffer::~VideoBuffer() {
    m_fpsCounter.stop();
    if (m_latestFrame) av_frame_free(&m_latestFrame);
}

void VideoBuffer::updateLatestFrame(const AVFrame* frame) {
    while (m_spinLock.test_and_set(std::memory_order_acquire)) { QThread::yieldCurrentThread(); }
    
    av_frame_unref(m_latestFrame);
    av_frame_ref(m_latestFrame, frame);
    m_frameGen++;
    
    m_spinLock.clear(std::memory_order_release);
    m_fpsCounter.addRenderedFrame();
}

void VideoBuffer::peekFrameInfo(int &width, int &height, int &format) {
    while (m_spinLock.test_and_set(std::memory_order_acquire)) { QThread::yieldCurrentThread(); }
    if (m_latestFrame && m_latestFrame->width > 0) {
        width = m_latestFrame->width;
        height = m_latestFrame->height;
        format = m_latestFrame->format;
    } else {
        width = 0; height = 0; format = -1;
    }
    m_spinLock.clear(std::memory_order_release);
}

void VideoBuffer::peekRenderedFrame(std::function<void(int, int, uint8_t*)> onFrame) {
    if (!onFrame) return;

    AVFrame* clonedFrame = nullptr;
    
    while (m_spinLock.test_and_set(std::memory_order_acquire)) { QThread::yieldCurrentThread(); }
    if (m_latestFrame && m_latestFrame->width > 0) {
        clonedFrame = av_frame_clone(m_latestFrame);
    }
    uint64_t currentGen = m_frameGen;
    m_spinLock.clear(std::memory_order_release);

    if (!clonedFrame) return;

    int width = clonedFrame->width;
    int height = clonedFrame->height;
    int format = clonedFrame->format;
    bool cacheValid = (currentGen == m_cacheGen) && m_cachedFrame && 
                      (width == m_cachedWidth) && (height == m_cachedHeight) && (format == m_cachedFormat);

    if (!cacheValid) {
        if (!m_cachedFrame) m_cachedFrame = std::make_shared<std::vector<uint8_t>>();
        int size = av_image_get_buffer_size(AV_PIX_FMT_RGB32, width, height, 1);
        if (m_cachedFrame->size() != static_cast<size_t>(size)) m_cachedFrame->resize(size);

        AVFrame *rgbFrame = av_frame_alloc();
        if (rgbFrame) {
            av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, m_cachedFrame->data(), 
                                 AV_PIX_FMT_RGB32, width, height, 1);

            AVFrameConvert convert;
            convert.setSrcFrameInfo(width, height, (AVPixelFormat)format);
            convert.setDstFrameInfo(width, height, AV_PIX_FMT_RGB32);
            
            if (convert.init()) {
                convert.convert(clonedFrame, rgbFrame);
                m_cacheGen = currentGen;
                m_cachedWidth = width;
                m_cachedHeight = height;
                m_cachedFormat = format;
            } 
            convert.deInit();
            av_frame_free(&rgbFrame);
        }
    }
    
    if (m_cachedFrame && !m_cachedFrame->empty()) {
        onFrame(m_cachedWidth, m_cachedHeight, m_cachedFrame->data());
    }
    
    av_frame_free(&clonedFrame);
}