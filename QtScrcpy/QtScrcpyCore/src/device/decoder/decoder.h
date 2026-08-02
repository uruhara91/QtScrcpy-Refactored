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
struct AVBufferRef;
struct SwsContext;

struct AVCodecContextDeleter {
    void operator()(AVCodecContext *ctx) const;
};

// Owns one extra reference on an AVHWDeviceContext buffer (the handle
// returned by av_hwdevice_ctx_create()). RAII wrapper so hardware-decode
// failure/teardown paths can't leak the device context.
struct AVBufferRefDeleter {
    void operator()(AVBufferRef *ref) const;
};

// libswscale context used only to reshuffle a hardware-transferred frame
// (typically NV12) into the planar YUV420P layout the rest of the pipeline
// (VideoBuffer, QYuvOpenGLWidget) already expects. Not used at all on the
// software-decode path.
struct SwsContextDeleter {
    void operator()(SwsContext *ctx) const;
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

    // useHwDecode: the user's saved preference (Dialog's "decoder:"
    // dropdown, DeviceParams::useHwDecode) for whether to attempt hardware
    // decode at all. true still means "try hardware, fall back to
    // software automatically if it's unavailable or fails to open" - not
    // a guarantee hardware decode ends up used; false always skips
    // straight to software. QTSCRCPY_DISABLE_HWACCEL, if explicitly set,
    // overrides this (see tryInitHwAccel()) the same way
    // QTSCRCPY_SERVER_ROOT overrides DeviceParams::useRoot in server.cpp.
    explicit Decoder(FrameCallback onFrame, bool useHwDecode = true, QObject *parent = nullptr);
    ~Decoder() override;

    [[nodiscard]] bool open();
    void close();

    [[nodiscard]] bool enqueuePacket(PacketHandle packet);

    void peekFrame(std::function<void(int width, int height, uint8_t *dataRGB32)> onFrame);
    VideoBuffer *videoBuffer() const { return m_vb.get(); }

    std::uint64_t droppedPacketCount() const {
        return m_droppedPackets.load(std::memory_order_relaxed);
    }
    std::size_t maximumQueueDepth() const {
        return m_maximumQueueDepth.load(std::memory_order_relaxed);
    }
    // True once a hardware decoder (VideoToolbox/D3D11VA/DXVA2/VAAPI/NVDEC,
    // depending on platform) has been successfully opened for the current
    // session. Always false on the software-decode fallback path.
    bool hwAccelActive() const {
        return m_hwAccelActive.load(std::memory_order_relaxed);
    }

signals:
    void updateFPS(quint32 fps);

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
    void logQueueHealth() const;
    int selectDecoderThreadCount() const;

    // --- Hardware-accelerated decode (see decoder.cpp for the full design
    // note). Tries, in order, the hwaccel APIs that make sense for the
    // current platform (e.g. D3D11VA -> DXVA2 -> NVDEC on Windows) and
    // leaves the decoder in plain software mode if none of them succeed -
    // this is a pure performance/CPU-usage optimization, never a hard
    // requirement to actually decode anything.
    bool tryInitHwAccel(const AVCodec *codec);
    void resetHwAccelState();
    // Reads back the just-decoded hardware surface into system memory and
    // reshuffles it into YUV420P in m_hwSwFrame. Returns false if the
    // surface could not be read back (the caller drops that frame).
    bool transferHwFrame();
    static AVPixelFormat getHwFormat(AVCodecContext *ctx, const AVPixelFormat *pixFmts);

private:
    std::unique_ptr<VideoBuffer> m_vb;
    std::unique_ptr<AVCodecContext, AVCodecContextDeleter> m_codecCtx;
    AVFrame *m_recvFrame = nullptr;
    FrameCallback m_onFrame;

    // Hardware-decode state. All unused/empty when hw accel is unavailable
    // or disabled (QTSCRCPY_DISABLE_HWACCEL=1) - the decoder then behaves
    // exactly as it did before this feature existed.
    std::unique_ptr<AVBufferRef, AVBufferRefDeleter> m_hwDeviceCtx;
    AVPixelFormat m_hwPixFmt = AV_PIX_FMT_NONE;
    AVHWDeviceType m_hwDeviceType = AV_HWDEVICE_TYPE_NONE;
    std::atomic_bool m_hwAccelActive{false};
    bool m_hwDecodePreferred = true; // constructor's useHwDecode, see there
    AVFrame *m_hwTransferFrame = nullptr; // system-memory copy of the hw surface (native format, usually NV12)
    AVFrame *m_hwSwFrame = nullptr;       // m_hwTransferFrame reshuffled to YUV420P
    std::unique_ptr<SwsContext, SwsContextDeleter> m_hwSwsCtx;

    QMutex m_queueMutex;
    QWaitCondition m_queueCondition;
    QQueue<QueuedPacket> m_packetQueue;
    bool m_waitingForKeyFrame = false;

    bool m_telemetryEnabled = false;
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
