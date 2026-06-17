#ifndef VIDEOSOCKET_H
#define VIDEOSOCKET_H

#include <QTcpSocket>
#include <atomic>
#include <cstdint>

class VideoSocket : public QTcpSocket
{
    Q_OBJECT
public:
    explicit VideoSocket(QObject *parent = nullptr);
    ~VideoSocket() override;

    // Read exactly bufSize bytes directly into the caller-owned destination.
    // Returns bufSize on success and -1 on shutdown, disconnect, or socket error.
    qint32 subThreadRecvData(quint8 *buf, qint32 bufSize);

    void quitNotify() noexcept
    {
        m_quit.store(true, std::memory_order_release);
    }

private:
    void logTelemetry() const;

private:
    std::atomic<bool> m_quit{false};
    bool m_telemetryEnabled = false;

    std::uint64_t m_totalBytesRead = 0;
    std::uint64_t m_readCalls = 0;
    std::uint64_t m_waitCalls = 0;
    std::uint64_t m_waitTimeouts = 0;
    std::uint64_t m_failedReads = 0;
    qint64 m_totalWaitNanoseconds = 0;
    qint64 m_maxWaitNanoseconds = 0;
    qint64 m_maxReadChunk = 0;
    qint64 m_peakBufferedBytes = 0;
};

#endif // VIDEOSOCKET_H
