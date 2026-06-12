#include "videobuffer.h"
#include "avframeconvert.h"
#include <QDebug>

extern "C"
{
#include "libavutil/imgutils.h"
}

VideoBuffer::VideoBuffer(QObject *parent) : QObject(parent) {
    m_decodingFrame = av_frame_alloc();
    m_renderingframe = av_frame_alloc();
    
    if (!m_decodingFrame || !m_renderingframe) {
        qCritical("VideoBuffer: OOM - Failed to allocate AVFrame");
    }

    m_renderingFrameConsumed = true;
    
    connect(&m_fpsCounter, &FpsCounter::updateFPS, this, &VideoBuffer::updateFPS);
    m_fpsCounter.start();
}

VideoBuffer::~VideoBuffer() {
    m_fpsCounter.stop();
    
    if (m_decodingFrame) {
        av_frame_free(&m_decodingFrame);
    }
    if (m_renderingframe) {
        av_frame_free(&m_renderingframe);
    }
}

void VideoBuffer::setRenderExpiredFrames(bool renderExpiredFrames)
{
    m_renderExpiredFrames = renderExpiredFrames;
}

AVFrame *VideoBuffer::decodingFrame()
{
    return m_decodingFrame;
}

void VideoBuffer::offerDecodedFrame(bool &previousFrameSkipped)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_renderingFrameConsumed) {
        previousFrameSkipped = true;
        if (m_fpsCounter.isStarted()) {
            m_fpsCounter.addSkippedFrame();
        }
    } else {
        previousFrameSkipped = false;
    }

    swap();

    m_frameGen++; 
    m_renderingFrameConsumed = false;
}

const AVFrame *VideoBuffer::consumeRenderedFrame()
{
    m_renderingFrameConsumed = true;
    
    if (m_fpsCounter.isStarted()) {
        m_fpsCounter.addRenderedFrame();
    }
    
    return m_renderingframe;
}

void VideoBuffer::peekFrameInfo(int &width, int &height, int &format)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_renderingframe && m_renderingframe->width > 0) {
        width = m_renderingframe->width;
        height = m_renderingframe->height;
        format = m_renderingframe->format;
    } else {
        width = 0; height = 0; format = -1;
    }
}

void VideoBuffer::peekRenderedFrame(std::function<void(int width, int height, uint8_t* dataRGB32)> onFrame)
{
    if (!onFrame) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    
    AVFrame* frame = m_renderingframe;
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        return; 
    }

    int width = frame->width;
    int height = frame->height;
    int format = frame->format;

    bool cacheValid = (m_frameGen == m_cacheGen) &&
                      (m_cachedFrame != nullptr) &&
                      (width == m_cachedWidth) &&
                      (height == m_cachedHeight) &&
                      (format == m_cachedFormat);

    std::shared_ptr<std::vector<uint8_t>> targetBuffer;

    if (!cacheValid) {
        if (!m_cachedFrame) {
            m_cachedFrame = std::make_shared<std::vector<uint8_t>>();
        }
        targetBuffer = m_cachedFrame;

        int size = av_image_get_buffer_size(AV_PIX_FMT_RGB32, width, height, 1);
        if (targetBuffer->size() != (size_t)size) {
            targetBuffer->resize(size);
        }

        AVFrame *rgbFrame = av_frame_alloc();
        if (rgbFrame) {
            av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, targetBuffer->data(), 
                                 AV_PIX_FMT_RGB32, width, height, 1);

            AVFrameConvert convert;
            convert.setSrcFrameInfo(width, height, (AVPixelFormat)format);
            convert.setDstFrameInfo(width, height, AV_PIX_FMT_RGB32);
            
            if (convert.init()) {
                convert.convert(frame, rgbFrame);
                
                m_cacheGen = m_frameGen;
                m_cachedWidth = width;
                m_cachedHeight = height;
                m_cachedFormat = format;
            } 
            
            convert.deInit();
            av_frame_free(&rgbFrame);
        }
    } else {
        targetBuffer = m_cachedFrame;
    }

    if (targetBuffer && !targetBuffer->empty()) {
        onFrame(m_cachedWidth, m_cachedHeight, targetBuffer->data());
    }
}

void VideoBuffer::swap()
{
    AVFrame *tmp = m_decodingFrame;
    m_decodingFrame = m_renderingframe;
    m_renderingframe = tmp;
}