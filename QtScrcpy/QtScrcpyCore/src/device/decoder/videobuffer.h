#ifndef VIDEOBUFFER_H
#define VIDEOBUFFER_H

#include <QObject>
#include <QMutex>
#include <cstdint>
#include <functional>
#include <vector>

extern "C" {
#include "libavutil/frame.h"
}

#include "fpscounter.h"

class VideoBuffer : public QObject {
    Q_OBJECT
public:
    explicit VideoBuffer(QObject *parent = nullptr);
    ~VideoBuffer() override;

    void updateLatestFrame(const AVFrame* frame);
    void peekFrameInfo(int &width, int &height, int &format);
    void peekRenderedFrame(std::function<void(int, int, uint8_t*)> onFrame);

signals:
    void updateFPS(quint32 fps);

private:
    FpsCounter m_fpsCounter;

    AVFrame* m_latestFrame = nullptr;
    QMutex m_frameMutex;
    std::uint64_t m_frameGeneration = 0;

    QMutex m_cacheMutex;
    std::uint64_t m_cachedGeneration = 0;
    int m_cachedWidth = 0;
    int m_cachedHeight = 0;
    int m_cachedFormat = -1;
    std::vector<uint8_t> m_cachedFrame;
};

#endif // VIDEOBUFFER_H
