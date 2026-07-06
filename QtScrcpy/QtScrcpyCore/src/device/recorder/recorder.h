#ifndef RECORDER_H
#define RECORDER_H

#include <QMutex>
#include <QQueue>
#include <QSize>
#include <QString>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <cstddef>
#include <cstdint>

extern "C"
{
#include "libavformat/avformat.h"
}

class Recorder : public QThread
{
    Q_OBJECT
public:
    enum RecorderFormat
    {
        RECORDER_FORMAT_NULL = 0,
        RECORDER_FORMAT_MP4,
        RECORDER_FORMAT_MKV,
    };

    explicit Recorder(const QString &fileName, QObject *parent = nullptr);
    ~Recorder() override;

    void setFrameSize(const QSize &declaredFrameSize);
    void setFormat(Recorder::RecorderFormat format);

    [[nodiscard]] bool open();
    void close();
    [[nodiscard]] bool startRecorder();
    void stopRecorder();

    [[nodiscard]] bool push(AVPacket *packet);

    // True once open() has failed (for any reason: muxer/protocol
    // unavailable, output file unwritable, allocation failure, etc.) or
    // the internal queue has overflowed (see push()). Callers should treat
    // this as "do not call push() again" - once set, it never clears
    // itself for the lifetime of this Recorder instance (a fresh
    // Recorder/open() call is required to retry). Exists because m_recorder
    // being non-null does not by itself mean the recorder is usable -
    // Device previously only checked the former, which let every
    // subsequent video packet keep queuing up against a recorder that had
    // already failed to open, until the queue limit itself kicked in.
    [[nodiscard]] bool isFailed() const { return m_failed.load(std::memory_order_acquire); }

private:
    const AVOutputFormat *findMuxer(const char *name);
    bool recorderWriteHeader(const AVPacket *packet);
    void recorderRescalePacket(AVPacket *packet);
    QString recorderGetFormatName(Recorder::RecorderFormat format);
    RecorderFormat guessRecordFormat(const QString &fileName);
    bool write(AVPacket *packet);

    void packetDelete(AVPacket *packet);
    void queueClear();
    void queueClearLocked();
    void updateQueuePeaksLocked();
    void logTelemetry() const;

protected:
    void run() override;

private:
    QString m_fileName;
    AVFormatContext *m_formatCtx = nullptr;
    QSize m_declaredFrameSize;

    bool m_headerWritten = false;
    RecorderFormat m_format = RECORDER_FORMAT_NULL;

    QMutex m_mutex;
    QWaitCondition m_recvDataCond;

    std::atomic_bool m_stopped{false};
    std::atomic_bool m_failed{false};

    QQueue<AVPacket *> m_queue;
    std::size_t m_queuedBytes = 0;
    std::size_t m_maxQueueBytes = 8U * 1024U * 1024U;
    int m_maxQueuePackets = 240;
    bool m_telemetryEnabled = false;

    std::atomic<std::size_t> m_peakQueueBytes{0};
    std::atomic<int> m_peakQueuePackets{0};
    std::atomic<std::uint64_t> m_overflowEvents{0};
};

#endif // RECORDER_H
