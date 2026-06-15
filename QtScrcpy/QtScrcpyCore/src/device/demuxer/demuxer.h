#ifndef STREAM_H
#define STREAM_H

#include <QPointer>
#include <QSize>
#include <QThread>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

extern "C" {
#include "libavcodec/avcodec.h"
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
    void setFrameSize(const QSize &frameSize);

    [[nodiscard]] bool startDecode();
    void stopDecode();

signals:
    void onStreamStop();
    void getFrame(AVPacket* packet);
    void getConfigFrame(AVPacket* packet);

protected:
    void run() override;

private:
    bool processNetworkPacket(AVPacket *packet);
    bool parse(AVPacket *packet);
    qint32 recvData(quint8 *buf, qint32 bufSize);

private:
    QPointer<VideoSocket> m_videoSocket;
    QSize m_frameSize;

    AVCodecContext *m_codecCtx = nullptr;
    AVCodecParserContext *m_parser = nullptr;

    std::vector<uint8_t> m_configBuffer;
    std::atomic<bool> m_isInterrupted{false};
};

#endif // STREAM_H
