#ifndef STREAM_H
#define STREAM_H

#include <QPointer>
#include <QSize>
#include <QThread>
#include <atomic>
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

    AVPacket* acquire() {
        while (m_lock.test_and_set(std::memory_order_acquire)) {
            QThread::yieldCurrentThread();
        }
        
        AVPacket* pkt = nullptr;
        if (m_pool.empty()) {
            pkt = av_packet_alloc();
        } else {
            pkt = m_pool.back();
            m_pool.pop_back();
        }
        
        m_lock.clear(std::memory_order_release);
        return pkt;
    }

    void release(AVPacket* pkt) {
        if (!pkt) return;
        av_packet_unref(pkt);
        
        while (m_lock.test_and_set(std::memory_order_acquire)) {
            QThread::yieldCurrentThread();
        }
        m_pool.push_back(pkt);
        m_lock.clear(std::memory_order_release);
    }

private:
    PacketPool() { 
        m_pool.reserve(256);
        for(int i = 0; i < 64; ++i) {
            m_pool.push_back(av_packet_alloc());
        }
    }
    ~PacketPool() {
        for (auto p : m_pool) av_packet_free(&p);
    }
    
    std::vector<AVPacket*> m_pool;
    std::atomic_flag m_lock = ATOMIC_FLAG_INIT;
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
    // Pipeline Refactored
    bool processNetworkPacket(AVPacket *packet);
    bool parse(AVPacket *packet);
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