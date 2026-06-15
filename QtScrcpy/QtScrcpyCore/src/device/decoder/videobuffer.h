#ifndef VIDEOBUFFER_H
#define VIDEOBUFFER_H

#include <QObject>
#include <QMutex>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

extern "C" {
#include "libavutil/frame.h"
}

#include "fpscounter.h"

class VideoBuffer : public QObject
{
    Q_OBJECT
public:
    using FrameCallback = std::function<void(int, int, uint8_t*)>;

    explicit VideoBuffer(QObject *parent = nullptr);
    ~VideoBuffer() override;

    void setFpsCounterEnabled(bool enabled);

    // Called for every decoded frame. With FPS hidden and no screenshot queued,
    // the idle path is one atomic load followed by an immediate return.
    void updateLatestFrame(const AVFrame* frame);
    void peekRenderedFrame(FrameCallback onFrame);

signals:
    void updateFPS(quint32 fps);

private:
    enum FrameWorkFlag : std::uint8_t {
        WorkNone = 0,
        WorkFps = 1U << 0,
        WorkCapture = 1U << 1,
    };

    struct FrameDeleter {
        void operator()(AVFrame *frame) const noexcept {
            if (frame) av_frame_free(&frame);
        }
    };

    using SharedFrame = std::shared_ptr<AVFrame>;

    void applyFpsCounterState(bool enabled);
    void deliverSnapshot(SharedFrame snapshot,
                         std::vector<FrameCallback> callbacks);

private:
    FpsCounter m_fpsCounter;
    std::atomic<std::uint8_t> m_frameWork{WorkNone};

    QMutex m_requestMutex;
    std::vector<FrameCallback> m_pendingCaptures;
};

#endif // VIDEOBUFFER_H
