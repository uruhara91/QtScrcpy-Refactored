#include "demuxer.h"
#include "videosocket.h"
#include "qtscrcpytelemetry.h"

#include <QDebug>
#include <algorithm>
#include <bit>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#define HEADER_SIZE 12

// scrcpy-server >= 4.0 wire format for the 12-byte packet header:
//
// If the MSB of byte 0 is set, this is a "session packet" (video stream
// only), NOT a media packet. It carries no payload beyond the 12-byte
// header itself:
//   byte 0..3 : flags (bit 31 = session marker, bit 0 = client-resized flag)
//   byte 4..7 : video width  (uint32 BE)
//   byte 8..11: video height (uint32 BE)
//
// Otherwise (MSB of byte 0 is 0), this is a regular media packet header:
//   byte 0..7 : PTS, with bit 62 = CONFIG flag, bit 61 = KEY_FRAME flag
//   byte 8..11: payload size (uint32 BE), followed by that many bytes
//
// NOTE: versus scrcpy-server 3.3.4, SC_PACKET_FLAG_CONFIG and
// SC_PACKET_FLAG_KEY_FRAME each shifted down by one bit (63->62, 62->61) to
// make room for the new session-packet marker at bit 63.
#define SC_PACKET_FLAG_SESSION   (uint64_t(1) << 63)
#define SC_PACKET_FLAG_CONFIG    (uint64_t(1) << 62)
#define SC_PACKET_FLAG_KEY_FRAME (uint64_t(1) << 61)
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

Demuxer::Demuxer(QObject *parent)
    : QThread(parent)
    , m_telemetryEnabled(qsc::telemetry::enabled())
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
    if (!buf || !m_videoSocket) return -1;
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

void Demuxer::logTelemetry() const
{
    if (!m_telemetryEnabled) return;

    qInfo() << "[Telemetry][Demuxer] packets"
            << "count=" << static_cast<qulonglong>(m_packetCount)
            << "payloadBytes=" << static_cast<qulonglong>(m_payloadBytes)
            << "maxPayloadBytes=" << m_maxPayloadBytes
            << "configPackets="
            << static_cast<qulonglong>(m_configPacketCount)
            << "keyFrames=" << static_cast<qulonglong>(m_keyFrameCount)
            << "configPrepends="
            << static_cast<qulonglong>(m_configPrependCount)
            << "sessionPackets="
            << static_cast<qulonglong>(m_sessionPacketCount)
            << "interruptedReads="
            << static_cast<qulonglong>(m_interruptedReads)
            << "readFailures=" << static_cast<qulonglong>(m_readFailures)
            << "invalidPackets=" << static_cast<qulonglong>(m_invalidPackets)
            << "allocationFailures="
            << static_cast<qulonglong>(m_allocationFailures);
}

void Demuxer::run()
{
    while (!m_isInterrupted.load(std::memory_order_acquire)) {
        PacketHandle packet = acquirePacketHandle();
        if (!packet) {
            ++m_allocationFailures;
            break;
        }
        bool isSession = false;
        if (!processNetworkPacket(packet, isSession)) break;
        // On a session packet, `packet` was left untouched (no frame to
        // dispatch) and is simply returned to the pool by PacketHandle's
        // destructor here; just loop back and read the next header.
    }

    m_configBuffer.clear();
    logTelemetry();

    if (m_videoSocket) {
        m_videoSocket->close();
        delete m_videoSocket;
        m_videoSocket = nullptr;
    }

    emit onStreamStop();
}

void Demuxer::handleSessionHeader(const quint8 *header)
{
    // See the format documented above SC_PACKET_FLAG_SESSION.
    const std::uint32_t flags = readBigEndian<std::uint32_t>(header);
    const std::uint32_t width = readBigEndian<std::uint32_t>(&header[4]);
    const std::uint32_t height = readBigEndian<std::uint32_t>(&header[8]);
    const bool clientResized = (flags & 1u) != 0;

    ++m_sessionPacketCount;

    const QSize size(static_cast<int>(width), static_cast<int>(height));
    if (!size.isValid()) return;

    m_lastVideoSize = size;
    emit sessionInfo(size, clientResized);
}

bool Demuxer::processNetworkPacket(PacketHandle &packet, bool &isSession)
{
    isSession = false;

    if (!packet) {
        ++m_allocationFailures;
        return false;
    }

    quint8 header[HEADER_SIZE];
    if (recvData(header, HEADER_SIZE) != HEADER_SIZE) {
        if (m_isInterrupted.load(std::memory_order_acquire)) {
            ++m_interruptedReads;
        } else {
            ++m_readFailures;
        }
        return false;
    }

    // A session packet is identified by the MSB of the first byte. It is
    // not a media packet: it carries no additional payload beyond the
    // 12-byte header already read above.
    if (header[0] & 0x80) {
        isSession = true;
        handleSessionHeader(header);
        return true;
    }

    const std::uint64_t ptsFlags = readBigEndian<std::uint64_t>(header);
    const std::uint32_t payloadLength = readBigEndian<std::uint32_t>(&header[8]);
    if (payloadLength == 0 || payloadLength > MAX_NETWORK_PACKET_SIZE) {
        ++m_invalidPackets;
        return false;
    }

    const bool isConfig = (ptsFlags & SC_PACKET_FLAG_CONFIG) != 0;
    const bool isKeyFrame = (ptsFlags & SC_PACKET_FLAG_KEY_FRAME) != 0;
    const bool prependConfig = !isConfig && !m_configBuffer.empty();
    const std::size_t configLength = prependConfig ? m_configBuffer.size() : 0;

    if (configLength >
        static_cast<std::size_t>(std::numeric_limits<int>::max()) -
            payloadLength) {
        ++m_invalidPackets;
        return false;
    }

    const int totalLength = static_cast<int>(configLength + payloadLength);
    AVBufferRef *buf = PacketBufferPool::get().acquire(
        static_cast<std::size_t>(totalLength));
    if (!buf) {
        ++m_allocationFailures;
        return false;
    }
    // packet is freshly allocated/unref'd (from PacketPool), so buf/data/size
    // are already null/zero here. av_packet_unref() is called defensively
    // first anyway (cheap no-op when already clean) so this stays correct
    // even if that invariant ever changes; assigning buf/data/size directly
    // afterwards is the same contract av_packet_from_data() uses internally,
    // just with our pooled AVBufferRef (custom free callback) instead of a
    // default-allocated one.
    av_packet_unref(packet.get());
    packet->buf = buf;
    packet->data = buf->data;
    packet->size = totalLength;

    uint8_t *writePtr = packet->data;
    if (prependConfig) {
        std::memcpy(writePtr, m_configBuffer.data(), configLength);
        writePtr += configLength;
        ++m_configPrependCount;
    }

    if (recvData(writePtr, static_cast<qint32>(payloadLength)) !=
        static_cast<qint32>(payloadLength)) {
        if (m_isInterrupted.load(std::memory_order_acquire)) {
            ++m_interruptedReads;
        } else {
            ++m_readFailures;
        }
        return false;
    }

    ++m_packetCount;
    m_payloadBytes += payloadLength;
    m_maxPayloadBytes = std::max(m_maxPayloadBytes, payloadLength);
    if (isConfig) ++m_configPacketCount;
    if (isKeyFrame) ++m_keyFrameCount;

    packet->pts = isConfig ? AV_NOPTS_VALUE
                           : static_cast<int64_t>(ptsFlags & SC_PACKET_PTS_MASK);
    packet->dts = packet->pts;
    if (isKeyFrame) packet->flags |= AV_PKT_FLAG_KEY;

    if (isConfig) {
        m_configBuffer.assign(packet->data, packet->data + packet->size);
        emit getConfigFrame(packet.release());
        return true;
    }

    if (prependConfig) m_configBuffer.clear();
    emit getFrame(packet.release());
    return true;
}
