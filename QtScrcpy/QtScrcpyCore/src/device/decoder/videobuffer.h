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

    // Called for every decoded frame. The idle path only updates FPS and checks
    // one atomic flag; an AVFrame reference is retained only for an actual
    // screenshot request.
    void updateLatestFrame(const AVFrame* frame);
    void peekRenderedFrame(FrameCallback onFrame);

signals:
    void updateFPS(quint32 fps);

private:
    struct FrameDeleter {
        void operator()(AVFrame *frame) const noexcept {
            if (frame) av_frame_free(&frame);
        }
    };

    using SharedFrame = std::shared_ptr<AVFrame>;

    void deliverSnapshot(SharedFrame snapshot,
                         std::vector<FrameCallback> callbacks);

private:
    FpsCounter m_fpsCounter;

    QMutex m_requestMutex;
    std::vector<FrameCallback> m_pendingCaptures;
    std::atomic_bool m_captureRequested{false};
};

#endif // VIDEOBUFFER_H
