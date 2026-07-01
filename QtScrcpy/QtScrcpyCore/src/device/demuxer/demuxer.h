#ifndef STREAM_H
#define STREAM_H

#include <QPointer>
#include <QSize>
#include <QThread>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

extern "C" {
#include "libavcodec/packet.h"
#include "libavformat/avformat.h"
}

class VideoSocket;

class PacketPool {
public:
    static PacketPool& get() {
        static PacketPool instance;
        return instance;
    }

    [[nodiscard]] AVPacket* acquire() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_pool.empty()) {
                AVPacket* packet = m_pool.back();
                m_pool.pop_back();
                return packet;
            }
        }
        return av_packet_alloc();
    }

    void release(AVPacket* packet) noexcept {
        if (!packet) return;
        av_packet_unref(packet);

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pool.size() < MAX_POOL_SIZE) {
            m_pool.push_back(packet);
        } else {
            av_packet_free(&packet);
        }
    }

private:
    static constexpr std::size_t PREALLOCATED_PACKETS = 64;
    static constexpr std::size_t MAX_POOL_SIZE = 256;

    PacketPool() {
        m_pool.reserve(MAX_POOL_SIZE);
        for (std::size_t i = 0; i < PREALLOCATED_PACKETS; ++i) {
            if (AVPacket* packet = av_packet_alloc()) {
                m_pool.push_back(packet);
            }
        }
    }

    ~PacketPool() {
        for (AVPacket* packet : m_pool) {
            av_packet_free(&packet);
        }
    }

    PacketPool(const PacketPool&) = delete;
    PacketPool& operator=(const PacketPool&) = delete;

    std::vector<AVPacket*> m_pool;
    std::mutex m_mutex;
};

struct PacketPoolDeleter {
    void operator()(AVPacket* packet) const noexcept {
        PacketPool::get().release(packet);
    }
};

using PacketHandle = std::unique_ptr<AVPacket, PacketPoolDeleter>;

[[nodiscard]] inline PacketHandle acquirePacketHandle() {
    return PacketHandle(PacketPool::get().acquire());
}

[[nodiscard]] inline PacketHandle clonePacketReference(const AVPacket* source) {
    if (!source) return {};

    PacketHandle clone = acquirePacketHandle();
    if (!clone || av_packet_ref(clone.get(), source) < 0) {
        return {};
    }
    return clone;
}

class Demuxer : public QThread
{
    Q_OBJECT
public:
    explicit Demuxer(QObject *parent = nullptr);
    ~Demuxer() override;

    [[nodiscard]] static bool init();
    static void deInit();

    void installVideoSocket(VideoSocket* videoSocket);
    // Kept for API compatibility; intentionally a no-op. The demuxer no
    // longer needs to be told the frame size up front - it learns the real
    // size itself from the stream's session packet (see sessionInfo()).
    void setFrameSize(const QSize &frameSize) { Q_UNUSED(frameSize); }

    // Video size from the most recently received session packet, or an
    // invalid QSize() if none has been received yet (i.e. before the first
    // frame is decodable).
    [[nodiscard]] QSize lastVideoSize() const { return m_lastVideoSize; }

    [[nodiscard]] bool startDecode();
    void stopDecode();

signals:
    void onStreamStop();
    void getFrame(AVPacket* packet);
    void getConfigFrame(AVPacket* packet);
    // Emitted whenever a "session packet" is received from the server
    // (scrcpy-server >= 4.0). This carries the actual video size and can be
    // emitted more than once during a connection (e.g. after a resize/reset
    // on the device side). `clientResized` indicates the resize was
    // initiated by the client itself (e.g. via RESIZE_DISPLAY).
    void sessionInfo(QSize size, bool clientResized);

protected:
    void run() override;

private:
    // Returns false on a fatal read error (caller should stop the loop).
    // On success, `isSession` tells the caller whether `packet` was filled
    // (media packet) or not (session packet, already handled/emitted here).
    bool processNetworkPacket(PacketHandle &packet, bool &isSession);
    void handleSessionHeader(const quint8 *header);
    qint32 recvData(quint8 *buf, qint32 bufSize);
    void logTelemetry() const;

private:
    QPointer<VideoSocket> m_videoSocket;
    std::vector<uint8_t> m_configBuffer;
    std::atomic<bool> m_isInterrupted{false};
    bool m_telemetryEnabled = false;
    QSize m_lastVideoSize;

    std::uint64_t m_packetCount = 0;
    std::uint64_t m_payloadBytes = 0;
    std::uint64_t m_configPacketCount = 0;
    std::uint64_t m_keyFrameCount = 0;
    std::uint64_t m_configPrependCount = 0;
    std::uint64_t m_interruptedReads = 0;
    std::uint64_t m_readFailures = 0;
    std::uint64_t m_invalidPackets = 0;
    std::uint64_t m_allocationFailures = 0;
    std::uint64_t m_sessionPacketCount = 0;
    std::uint32_t m_maxPayloadBytes = 0;
};

#endif // STREAM_H
