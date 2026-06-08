#ifndef STREAM_H
#define STREAM_H

#include <QPointer>
#include <QSize>
#include <QThread>
#include <atomic>
#include <vector>
#include <mutex>

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

    AVPacket* acquire() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pool.empty()) {
            return av_packet_alloc();
        }
        AVPacket* pkt = m_pool.back();
        m_pool.pop_back();
        return pkt;
    }

    void release(AVPacket* pkt) {
        if (!pkt) return;
        av_packet_unref(pkt);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pool.push_back(pkt);
    }

private:
    PacketPool() { 
    m_pool.reserve(256); 
}
    ~PacketPool() {
        for (auto p : m_pool) av_packet_free(&p);
    }
    std::vector<AVPacket*> m_pool;
    std::mutex m_mutex;
};

class Demuxer : public QThread
{
    Q_OBJECT
public:
    explicit Demuxer(QObject *parent = nullptr);
    virtual ~Demuxer() override;

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
    // Helper internal
    bool recvPacket(AVPacket *packet);
    bool pushPacket(AVPacket *packet);
    bool processConfigPacket(AVPacket *packet);
    bool parse(AVPacket *packet);
    bool processFrame(AVPacket *packet);
    qint32 recvData(quint8 *buf, qint32 bufSize);

private:
    QPointer<VideoSocket> m_videoSocket;
    QSize m_frameSize;

    AVCodecContext *m_codecCtx = nullptr;
    AVCodecParserContext *m_parser = nullptr;
    
    std::vector<uint8_t> m_configBuffer;

    std::atomic<bool> m_isInterrupted { false };
};

#endif // STREAM_H