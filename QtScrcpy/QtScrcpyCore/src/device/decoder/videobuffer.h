#ifndef VIDEOBUFFER_H
#define VIDEOBUFFER_H

#include <QObject>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "fpscounter.h"

extern "C" {
#include "libavutil/frame.h"
}

class VideoBuffer : public QObject
{
    Q_OBJECT
public:
    explicit VideoBuffer(QObject *parent = nullptr);
    virtual ~VideoBuffer();

    void updateLatestFrame(const AVFrame* frame);
    void peekFrameInfo(int &width, int &height, int &format);
    void peekRenderedFrame(std::function<void(int width, int height, uint8_t* dataRGB32)> onFrame);
    void setRenderExpiredFrames(bool renderExpiredFrames) { m_renderExpiredFrames = renderExpiredFrames; }

signals:
    void updateFPS(quint32 fps);

private:
    AVFrame *m_latestFrame = nullptr;
    std::atomic_flag m_spinLock = ATOMIC_FLAG_INIT;
    
    bool m_renderExpiredFrames = false;
    FpsCounter m_fpsCounter;

    std::shared_ptr<std::vector<uint8_t>> m_cachedFrame;
    int m_cachedWidth = 0;
    int m_cachedHeight = 0;
    int m_cachedFormat = -1;
    quint64 m_frameGen = 0;
    quint64 m_cacheGen = 0;
};

#endif // VIDEOBUFFER_H