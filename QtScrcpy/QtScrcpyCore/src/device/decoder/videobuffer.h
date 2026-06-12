#ifndef VIDEOBUFFER_H
#define VIDEOBUFFER_H

#include <QObject>
#include <mutex>
#include <functional>
#include <memory>
#include <vector>

#include "fpscounter.h"

extern "C"
{
#include "libavutil/frame.h"
}

class VideoBuffer : public QObject
{
    Q_OBJECT
public:
    explicit VideoBuffer(QObject *parent = nullptr);
    virtual ~VideoBuffer();

    AVFrame *decodingFrame();
    void offerDecodedFrame(bool &previousFrameSkipped);
    const AVFrame *consumeRenderedFrame();

    void peekFrameInfo(int &width, int &height, int &format);
    void peekRenderedFrame(std::function<void(int width, int height, uint8_t* dataRGB32)> onFrame);
    
    void setRenderExpiredFrames(bool renderExpiredFrames);

signals:
    void updateFPS(quint32 fps);

private:
    void swap();

private:
    AVFrame *m_decodingFrame = nullptr;
    AVFrame *m_renderingframe = nullptr;
    
    bool m_renderExpiredFrames = false;
    bool m_renderingFrameConsumed = true;
    
    FpsCounter m_fpsCounter;
    std::mutex m_mutex;

    std::shared_ptr<std::vector<uint8_t>> m_cachedFrame;
    int m_cachedWidth = 0;
    int m_cachedHeight = 0;
    int m_cachedFormat = -1;
    quint64 m_frameGen = 0;
    quint64 m_cacheGen = 0;
};

#endif // VIDEOBUFFER_H