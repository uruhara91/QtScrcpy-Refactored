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
    if (!buf) return 0;

    Q_ASSERT(QCoreApplication::instance()->thread() != QThread::currentThread());

    while (bytesAvailable() < bufSize) {
        if (m_quit) return -1;
        if (state() != QAbstractSocket::ConnectedState) return -1;

        if (!waitForReadyRead(-1)) {
            return -1; 
        }
    }

    return read((char *)buf, bufSize);
}
