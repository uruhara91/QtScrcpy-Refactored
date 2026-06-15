#ifndef DECODER_H
#define DECODER_H

#include <QMutex>
#include <QQueue>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/pixfmt.h"
}

struct AVCodecContext;
struct AVPacket;
struct AVFrame;

struct AVCodecContextDeleter {
    void operator()(AVCodecContext *ctx) const;
};

class VideoBuffer;

class Decoder : public QThread
{
    Q_OBJECT
public:
    using FrameCallback = std::function<void(int width, int height,
                                            std::span<const uint8_t> dataY,
                                            std::span<const uint8_t> dataU,
                                            std::span<const uint8_t> dataV,
                                            int linesizeY, int linesizeU, int linesizeV)>;

    explicit Decoder(FrameCallback onFrame, QObject *parent = nullptr);
    ~Decoder() override;

    [[nodiscard]] bool open();
    void close();

    // Takes ownership only when true is returned.
    [[nodiscard]] bool enqueuePacket(AVPacket *packet);

    void peekFrame(std::function<void(int width, int height, uint8_t *dataRGB32)> onFrame);
    VideoBuffer *videoBuffer() const { return m_vb.get(); }

    std::uint64_t droppedPacketCount() const {
        return m_droppedPackets.load(std::memory_order_relaxed);
    }
    std::size_t maximumQueueDepth() const {
        return m_maximumQueueDepth.load(std::memory_order_relaxed);
    }

public slots:
    // Compatibility bridge for the existing Device wiring. The slot always
    // consumes ownership, even when the bounded queue rejects the packet.
    void onDecodeFrame(AVPacket *packet);

signals:
    void updateFPS(quint32 fps);
    void newFrame();

protected:
    void run() override;

private:
    static constexpr int MAX_PACKET_QUEUE_SIZE = 8;

    void decodePacket(AVPacket *packet);
    void clearPacketQueue();
    void updateMaximumQueueDepth(std::size_t depth);

private:
    std::unique_ptr<VideoBuffer> m_vb;
    std::unique_ptr<AVCodecContext, AVCodecContextDeleter> m_codecCtx;
    AVFrame *m_recvFrame = nullptr;
    FrameCallback m_onFrame;

    QMutex m_queueMutex;
    QWaitCondition m_queueCondition;
    QQueue<AVPacket *> m_packetQueue;
    bool m_waitingForKeyFrame = false;

    std::atomic_bool m_codecOpen{false};
    std::atomic_bool m_stopping{false};
    std::atomic_bool m_flushBeforeNextDecode{false};
    std::atomic<std::uint64_t> m_droppedPackets{0};
    std::atomic<std::uint64_t> m_recoveryEvents{0};
    std::atomic<std::size_t> m_maximumQueueDepth{0};
};

#endif // DECODER_H
