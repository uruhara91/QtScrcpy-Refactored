#include "demuxer.h"
#include "videosocket.h"
#include "compat.h"

#include <QDebug>
#include <bit>
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

void Demuxer::setFrameSize(const QSize &frameSize)
{
    m_frameSize = frameSize;
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
    m_codecCtx = nullptr;
    m_parser = nullptr;
    AVPacket *packet = nullptr;

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) goto runQuit;

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) goto runQuit;

    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    m_codecCtx->width = m_frameSize.width();
    m_codecCtx->height = m_frameSize.height();
    m_codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;

    m_parser = av_parser_init(AV_CODEC_ID_H264);
    if (!m_parser) goto runQuit;
    m_parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;

    packet = PacketPool::get().acquire();
    if (!packet) goto runQuit;

    while (!m_isInterrupted.load(std::memory_order_acquire)) {
        const bool ok = processNetworkPacket(packet);
        av_packet_unref(packet);
        if (!ok) break;
    }

runQuit:
    m_configBuffer.clear();

    if (packet) PacketPool::get().release(packet);
    if (m_parser) {
        av_parser_close(m_parser);
        m_parser = nullptr;
    }
    if (m_codecCtx) avcodec_free_context(&m_codecCtx);

    if (m_videoSocket) {
        m_videoSocket->close();
        delete m_videoSocket;
        m_videoSocket = nullptr;
    }

    emit onStreamStop();
}

bool Demuxer::processNetworkPacket(AVPacket *packet)
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
    if (av_new_packet(packet, totalLength) != 0) return false;

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

        AVPacket *clone = PacketPool::get().acquire();
        if (!clone) return false;
        if (av_packet_ref(clone, packet) < 0) {
            PacketPool::get().release(clone);
            return false;
        }
        emit getConfigFrame(clone);
        return true;
    }

    if (prependConfig) m_configBuffer.clear();
    return parse(packet);
}

bool Demuxer::parse(AVPacket *packet)
{
    if (!packet || !m_parser || !m_codecCtx) return false;

    quint8 *outData = nullptr;
    int outLength = 0;
    const int parseResult = av_parser_parse2(
        m_parser,
        m_codecCtx,
        &outData,
        &outLength,
        packet->data,
        packet->size,
        packet->pts,
        packet->dts,
        -1);

    if (parseResult < 0) return false;
    if (m_parser->key_frame == 1) packet->flags |= AV_PKT_FLAG_KEY;

    AVPacket *clone = PacketPool::get().acquire();
    if (!clone) return false;
    if (av_packet_ref(clone, packet) < 0) {
        PacketPool::get().release(clone);
        return false;
    }

    emit getFrame(clone);
    return true;
}
