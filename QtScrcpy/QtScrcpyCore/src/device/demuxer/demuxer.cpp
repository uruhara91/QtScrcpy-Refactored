#include "demuxer.h"
#include "videosocket.h"
#include "compat.h"

#include <QDebug>
#include <bit>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#define HEADER_SIZE 12
#define SC_PACKET_FLAG_CONFIG    (uint64_t(1) << 63)
#define SC_PACKET_FLAG_KEY_FRAME (uint64_t(1) << 62)
#define SC_PACKET_PTS_MASK       (SC_PACKET_FLAG_KEY_FRAME - 1)

namespace {
constexpr std::uint32_t MAX_NETWORK_PACKET_SIZE = 64U * 1024U * 1024U;

template <typename T>
T readBigEndian(const quint8 *buf) {
    T value;
    std::memcpy(&value, buf, sizeof(T));
    return std::byteswap(value);
}
}

static void avLogCallback(void *avcl, int level, const char *fmt, va_list vl)
{
    Q_UNUSED(avcl);
    if (level > AV_LOG_WARNING || !fmt) return;

    char message[1024] = {};
    va_list copy;
    va_copy(copy, vl);
    std::vsnprintf(message, sizeof(message), fmt, copy);
    va_end(copy);

    const QString localMessage = QStringLiteral("[FFmpeg] ") +
                                 QString::fromUtf8(message).trimmed();
    if (level <= AV_LOG_ERROR) qCritical().noquote() << localMessage;
    else qWarning().noquote() << localMessage;
}

Demuxer::Demuxer(QObject *parent) : QThread(parent)
{
    m_configBuffer.reserve(64U * 1024U);
}

Demuxer::~Demuxer()
{
    stopDecode();
}

bool Demuxer::init()
{
    if (avformat_network_init()) return false;
    av_log_set_callback(avLogCallback);
    return true;
}

void Demuxer::deInit()
{
    avformat_network_deinit();
}

void Demuxer::installVideoSocket(VideoSocket *videoSocket)
{
    if (videoSocket) videoSocket->moveToThread(this);
    m_videoSocket = videoSocket;
}

qint32 Demuxer::recvData(quint8 *buf, qint32 bufSize)
{
    if (!buf || !m_videoSocket) return 0;
    return m_videoSocket->subThreadRecvData(buf, bufSize);
}

bool Demuxer::startDecode()
{
    if (!m_videoSocket || isRunning()) return false;
    m_isInterrupted.store(false, std::memory_order_release);
    start();
    return true;
}

void Demuxer::stopDecode()
{
    m_isInterrupted.store(true, std::memory_order_release);
    if (m_videoSocket) m_videoSocket->quitNotify();
    if (isRunning()) wait();
}

void Demuxer::run()
{
    while (!m_isInterrupted.load(std::memory_order_acquire)) {
        PacketHandle packet = acquirePacketHandle();
        if (!packet || !processNetworkPacket(packet)) break;
    }

    m_configBuffer.clear();

    if (m_videoSocket) {
        m_videoSocket->close();
        delete m_videoSocket;
        m_videoSocket = nullptr;
    }

    emit onStreamStop();
}

bool Demuxer::processNetworkPacket(PacketHandle &packet)
{
    if (!packet) return false;

    quint8 header[HEADER_SIZE];
    if (recvData(header, HEADER_SIZE) < HEADER_SIZE) return false;

    const std::uint64_t ptsFlags = readBigEndian<std::uint64_t>(header);
    const std::uint32_t payloadLength = readBigEndian<std::uint32_t>(&header[8]);
    if (payloadLength == 0 || payloadLength > MAX_NETWORK_PACKET_SIZE) return false;

    const bool isConfig = (ptsFlags & SC_PACKET_FLAG_CONFIG) != 0;
    const bool prependConfig = !isConfig && !m_configBuffer.empty();
    const std::size_t configLength = prependConfig ? m_configBuffer.size() : 0;

    if (configLength > static_cast<std::size_t>(std::numeric_limits<int>::max()) - payloadLength) {
        return false;
    }

    const int totalLength = static_cast<int>(configLength + payloadLength);
    if (av_new_packet(packet.get(), totalLength) != 0) return false;

    uint8_t *writePtr = packet->data;
    if (prependConfig) {
        std::memcpy(writePtr, m_configBuffer.data(), configLength);
        writePtr += configLength;
    }

    if (recvData(writePtr, static_cast<qint32>(payloadLength)) <
        static_cast<qint32>(payloadLength)) {
        return false;
    }

    packet->pts = isConfig ? AV_NOPTS_VALUE
                           : static_cast<int64_t>(ptsFlags & SC_PACKET_PTS_MASK);
    packet->dts = packet->pts;
    if (ptsFlags & SC_PACKET_FLAG_KEY_FRAME) packet->flags |= AV_PKT_FLAG_KEY;

    if (isConfig) {
        m_configBuffer.assign(packet->data, packet->data + packet->size);
        emit getConfigFrame(packet.release());
        return true;
    }

    if (prependConfig) m_configBuffer.clear();
    emit getFrame(packet.release());
    return true;
}
