#ifndef VIDEOBUFFER_H
#define VIDEOBUFFER_H

#include <QObject>
#include <atomic>
#include <memory>
#include <vector>

extern "C" {
#include "libavutil/frame.h"
}

#include "fpscounter.h"

class VideoBuffer : public QObject {
    Q_OBJECT
public:
    explicit VideoBuffer(QObject *parent = nullptr);
    ~VideoBuffer();

    void updateLatestFrame(const AVFrame* frame);
    void peekFrameInfo(int &width, int &height, int &format);
    void peekRenderedFrame(std::function<void(int, int, uint8_t*)> onFrame);

signals:
    void updateFPS(quint32 fps);

private:
    FpsCounter m_fpsCounter;
    
    AVFrame* m_frames[3] = {nullptr, nullptr, nullptr};
    std::atomic<int> m_writeIdx{0};
    std::atomic<int> m_readIdx{1};
    std::atomic<int> m_idleIdx{2};
    std::atomic<bool> m_hasNewFrame{false};
    
    std::atomic<uint64_t> m_frameGen{0};
    uint64_t m_cacheGen = 0;
    int m_cachedWidth = 0;
    int m_cachedHeight = 0;
    int m_cachedFormat = -1;
    std::shared_ptr<std::vector<uint8_t>> m_cachedFrame;
};

#endif // VIDEOBUFFER_H