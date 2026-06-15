#include <QCoreApplication>
#include <QDebug>
#include <QThread>

#include "videosocket.h"

VideoSocket::VideoSocket(QObject *parent) : QTcpSocket(parent)
{
}

VideoSocket::~VideoSocket()
{
}

qint32 VideoSocket::subThreadRecvData(quint8 *buf, qint32 bufSize)
{
    if (!buf || bufSize <= 0) return 0;

    Q_ASSERT(QCoreApplication::instance()->thread() != QThread::currentThread());

    while (bytesAvailable() < bufSize) {
        if (m_quit.load(std::memory_order_acquire)) return -1;
        if (state() != QAbstractSocket::ConnectedState) return -1;

        // A finite wait keeps shutdown deterministic. quitNotify() is observed
        // within at most this interval even when the peer stays connected but idle.
        if (!waitForReadyRead(100)) {
            if (m_quit.load(std::memory_order_acquire)) return -1;
            if (state() != QAbstractSocket::ConnectedState) return -1;
            continue;
        }
    }

    return static_cast<qint32>(read(reinterpret_cast<char *>(buf), bufSize));
}
