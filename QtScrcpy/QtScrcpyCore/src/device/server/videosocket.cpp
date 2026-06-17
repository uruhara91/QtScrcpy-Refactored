#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>

#include "qtscrcpytelemetry.h"
#include "videosocket.h"

VideoSocket::VideoSocket(QObject *parent)
    : QTcpSocket(parent)
    , m_telemetryEnabled(qsc::telemetry::enabled())
{
}

VideoSocket::~VideoSocket()
{
    logTelemetry();
}

void VideoSocket::logTelemetry() const
{
    if (!m_telemetryEnabled) return;

    const double totalWaitMs =
        static_cast<double>(m_totalWaitNanoseconds) / 1'000'000.0;
    const double maxWaitMs =
        static_cast<double>(m_maxWaitNanoseconds) / 1'000'000.0;

    qInfo() << "[Telemetry][VideoSocket] reads"
            << "bytes=" << static_cast<qulonglong>(m_totalBytesRead)
            << "readCalls=" << static_cast<qulonglong>(m_readCalls)
            << "waitCalls=" << static_cast<qulonglong>(m_waitCalls)
            << "waitTimeouts=" << static_cast<qulonglong>(m_waitTimeouts)
            << "interruptedReads="
            << static_cast<qulonglong>(m_interruptedReads)
            << "failedReads=" << static_cast<qulonglong>(m_failedReads)
            << "totalWaitMs=" << totalWaitMs
            << "maxWaitMs=" << maxWaitMs
            << "maxReadChunk=" << m_maxReadChunk
            << "peakBufferedBytes=" << m_peakBufferedBytes;
}

qint32 VideoSocket::subThreadRecvData(quint8 *buf, qint32 bufSize)
{
    if (!buf || bufSize <= 0) return -1;

    Q_ASSERT(QCoreApplication::instance());
    Q_ASSERT(QCoreApplication::instance()->thread() != QThread::currentThread());
    Q_ASSERT(thread() == QThread::currentThread());

    qint32 totalRead = 0;
    while (totalRead < bufSize) {
        if (m_quit.load(std::memory_order_acquire)) {
            ++m_interruptedReads;
            return -1;
        }
        if (state() != QAbstractSocket::ConnectedState) {
            ++m_failedReads;
            return -1;
        }

        const qint64 buffered = bytesAvailable();
        m_peakBufferedBytes = std::max(m_peakBufferedBytes, buffered);

        if (buffered <= 0) {
            ++m_waitCalls;
            QElapsedTimer waitTimer;
            if (m_telemetryEnabled) waitTimer.start();

            const bool ready = waitForReadyRead(100);
            if (m_telemetryEnabled && waitTimer.isValid()) {
                const qint64 elapsed = waitTimer.nsecsElapsed();
                m_totalWaitNanoseconds += elapsed;
                m_maxWaitNanoseconds =
                    std::max(m_maxWaitNanoseconds, elapsed);
            }

            if (!ready) {
                ++m_waitTimeouts;
                // A finite timeout is expected while the peer is idle. Qt may
                // report SocketTimeoutError here even though the connection is
                // still healthy, so only quit/state determine termination.
                if (m_quit.load(std::memory_order_acquire)) {
                    ++m_interruptedReads;
                    return -1;
                }
                if (state() != QAbstractSocket::ConnectedState) {
                    ++m_failedReads;
                    return -1;
                }
            }
            continue;
        }

        const qint64 remaining = static_cast<qint64>(bufSize - totalRead);
        const qint64 requestSize = std::min(buffered, remaining);
        const qint64 chunk = read(
            reinterpret_cast<char *>(buf + totalRead), requestSize);

        if (chunk <= 0) {
            if (m_quit.load(std::memory_order_acquire)) {
                ++m_interruptedReads;
                return -1;
            }
            if (chunk < 0 ||
                state() != QAbstractSocket::ConnectedState) {
                ++m_failedReads;
                return -1;
            }
            continue;
        }

        totalRead += static_cast<qint32>(chunk);
        m_totalBytesRead += static_cast<std::uint64_t>(chunk);
        ++m_readCalls;
        m_maxReadChunk = std::max(m_maxReadChunk, chunk);
    }

    return totalRead;
}
