#ifndef DECODER_H
#define DECODER_H

#include <QMutex>
#include <QQueue>
#include <QThread>
#include <QWaitCondition>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>

#include "../demuxer/demuxer.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/pixfmt.h"
}

struct AVCodecContext;
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

    // Ownership is transferred unconditionally. Rejected packets are released
    // automatically by PacketHandle, so callers cannot leak on failure paths.
    [[nodiscard]] bool enqueuePacket(PacketHandle packet);

    void peekFrame(std::function<void(int width, int height, uint8_t *dataRGB32)> onFrame);
    VideoBuffer *videoBuffer() const { return m_vb.get(); }

    std::uint64_t droppedPacketCount() const {
        return m_droppedPackets.load(std::memory_order_relaxed);
    }
    std::size_t maximumQueueDepth() const {
        return m_maximumQueueDepth.load(std::memory_order_relaxed);
    }

signals:
    void updateFPS(quint32 fps);
    void newFrame();

protected:
    void run() override;

private:
    using Clock = std::chrono::steady_clock;
    static constexpr int MAX_PACKET_QUEUE_SIZE = 8;
    static constexpr std::size_t LATENCY_SAMPLE_CAPACITY = 4096;

    struct QueuedPacket {
        AVPacket *packet = nullptr;
        Clock::time_point enqueuedAt{};
    };

    struct LatencySummary {
        std::size_t samples = 0;
        std::uint32_t p50Us = 0;
        std::uint32_t p95Us = 0;
        std::uint32_t p99Us = 0;
        std::uint32_t maxUs = 0;
    };

    class LatencyWindow {
    public:
        void reset() noexcept {
            m_samples.fill(0);
            m_count = 0;
            m_next = 0;
        }

        template <typename Rep, typename Period>
        void add(std::chrono::duration<Rep, Period> duration) noexcept {
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
            const auto clamped = std::clamp<std::int64_t>(
                micros,
                0,
                static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()));

            m_samples[m_next] = static_cast<std::uint32_t>(clamped);
            m_next = (m_next + 1) % m_samples.size();
            if (m_count < m_samples.size()) ++m_count;
        }

        [[nodiscard]] LatencySummary summary() const {
            LatencySummary result;
            result.samples = m_count;
            if (m_count == 0) return result;

            auto sorted = m_samples;
            std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(m_count));

            const auto percentile = [&](double ratio) -> std::uint32_t {
                const auto index = static_cast<std::size_t>(
                    ratio * static_cast<double>(m_count - 1));
                return sorted[index];
            };

            result.p50Us = percentile(0.50);
            result.p95Us = percentile(0.95);
            result.p99Us = percentile(0.99);
            result.maxUs = sorted[m_count - 1];
            return result;
        }

    private:
        std::array<std::uint32_t, LATENCY_SAMPLE_CAPACITY> m_samples{};
        std::size_t m_count = 0;
        std::size_t m_next = 0;
    };

    void decodePacket(PacketHandle packet);
    void drainDecodedFrames();
    void clearPacketQueue();
    void updateMaximumQueueDepth(std::size_t depth);
    void logTimingStats() const;
    int selectDecoderThreadCount() const;

private:
    std::unique_ptr<VideoBuffer> m_vb;
    std::unique_ptr<AVCodecContext, AVCodecContextDeleter> m_codecCtx;
    AVFrame *m_recvFrame = nullptr;
    FrameCallback m_onFrame;

    QMutex m_queueMutex;
    QWaitCondition m_queueCondition;
    QQueue<QueuedPacket> m_packetQueue;
    bool m_waitingForKeyFrame = false;

    LatencyWindow m_queueWaitStats;
    LatencyWindow m_workerServiceStats;
    LatencyWindow m_frameIntervalStats;
    Clock::time_point m_lastFrameTime{};

    std::atomic_bool m_codecOpen{false};
    std::atomic_bool m_stopping{false};
    std::atomic_bool m_flushBeforeNextDecode{false};
    std::atomic<std::uint64_t> m_droppedPackets{0};
    std::atomic<std::uint64_t> m_recoveryEvents{0};
    std::atomic<std::size_t> m_maximumQueueDepth{0};
};

#endif // DECODER_H
