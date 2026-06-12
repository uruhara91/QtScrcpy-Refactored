#ifndef STREAM_H
#define STREAM_H

#include <QPointer>
#include <QSize>
#include <QThread>
#include <atomic>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}

class VideoSocket;

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
    AVPacket* m_pending = nullptr;

    std::atomic<bool> m_isInterrupted { false };
};

#endif // STREAM_H