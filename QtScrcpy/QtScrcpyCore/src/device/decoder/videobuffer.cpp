#include "videobuffer.h"
#include "avframeconvert.h"

extern "C" {
#include "libavutil/imgutils.h"
}

VideoBuffer::VideoBuffer(QObject *parent) : QObject(parent) {
    for (int i = 0; i < 3; ++i) {
        m_frames[i] = av_frame_alloc();
    }
    connect(&m_fpsCounter, &FpsCounter::updateFPS, this, &VideoBuffer::updateFPS);
    m_fpsCounter.start();
}

VideoBuffer::~VideoBuffer() {
    m_fpsCounter.stop();
    for (int i = 0; i < 3; ++i) {
        if (m_frames[i]) av_frame_free(&m_frames[i]);
    }
}

void VideoBuffer::updateLatestFrame(const AVFrame* frame) {
    int writeIdx = m_writeIdx.load(std::memory_order_relaxed);
    
    av_frame_unref(m_frames[writeIdx]);
    av_frame_ref(m_frames[writeIdx], frame);
    m_frameGen.fetch_add(1, std::memory_order_relaxed);
    
    int idleIdx = m_idleIdx.load(std::memory_order_relaxed);
    m_idleIdx.store(writeIdx, std::memory_order_relaxed);
    m_writeIdx.store(idleIdx, std::memory_order_relaxed);
    
    m_hasNewFrame.store(true, std::memory_order_release);
    m_fpsCounter.addRenderedFrame();
}

void VideoBuffer::peekFrameInfo(int &width, int &height, int &format) {
    AVFrame* readFrame = m_frames[m_readIdx.load(std::memory_order_relaxed)];
    if (readFrame && readFrame->width > 0) {
        width = readFrame->width;
        height = readFrame->height;
        format = readFrame->format;
    } else {
        width = 0; height = 0; format = -1;
    }
}

void VideoBuffer::peekRenderedFrame(std::function<void(int, int, uint8_t*)> onFrame) {
    if (!onFrame) return;

    if (m_hasNewFrame.exchange(false, std::memory_order_acquire)) {
        int idleIdx = m_idleIdx.load(std::memory_order_relaxed);
        int readIdx = m_readIdx.load(std::memory_order_relaxed);
        m_idleIdx.store(readIdx, std::memory_order_relaxed);
        m_readIdx.store(idleIdx, std::memory_order_relaxed);
    }

    AVFrame* renderFrame = m_frames[m_readIdx.load(std::memory_order_relaxed)];
    
    if (!renderFrame || renderFrame->width <= 0) return;

    int width = renderFrame->width;
    int height = renderFrame->height;
    int format = renderFrame->format;
    uint64_t currentGen = m_frameGen.load(std::memory_order_relaxed);

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
                convert.convert(renderFrame, rgbFrame); 
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
}