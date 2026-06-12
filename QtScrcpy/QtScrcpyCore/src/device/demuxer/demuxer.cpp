#include "demuxer.h"
#include "videosocket.h"
#include "compat.h"

#include <QDebug>
#include <bit>
#include <cstring>

#define HEADER_SIZE 12
#define SC_PACKET_FLAG_CONFIG    (uint64_t(1) << 63)
#define SC_PACKET_FLAG_KEY_FRAME (uint64_t(1) << 62)
#define SC_PACKET_PTS_MASK       (SC_PACKET_FLAG_KEY_FRAME - 1)

template <typename T>
static T readBigEndian(const quint8 *buf) {
    T val;
    std::memcpy(&val, buf, sizeof(T));
    return std::byteswap(val);
}

static void avLogCallback(void *avcl, int level, const char *fmt, va_list vl)
{
    Q_UNUSED(avcl);
    Q_UNUSED(vl);
    if (level > AV_LOG_WARNING) return;

    QString localFmt = QString::fromUtf8(fmt).trimmed();
    localFmt.prepend("[FFmpeg] ");
    
    if (level <= AV_LOG_ERROR) qCritical() << localFmt;
    else qWarning() << localFmt;
}

Demuxer::Demuxer(QObject *parent) : QThread(parent)
{
    m_configBuffer.reserve(1024 * 64);
}

Demuxer::~Demuxer() { stopDecode(); }

bool Demuxer::init()
{
    if (avformat_network_init()) return false;
    av_log_set_callback(avLogCallback);
    return true;
}

void Demuxer::deInit() { avformat_network_deinit(); }

void Demuxer::installVideoSocket(VideoSocket *videoSocket)
{
    if (videoSocket) videoSocket->moveToThread(this);
    m_videoSocket = videoSocket;
}

void Demuxer::setFrameSize(const QSize &frameSize) { m_frameSize = frameSize; }

qint32 Demuxer::recvData(quint8 *buf, qint32 bufSize)
{
    if (!buf || !m_videoSocket) return 0;
    return m_videoSocket->subThreadRecvData(buf, bufSize);
}

bool Demuxer::startDecode()
{
    if (!m_videoSocket) return false;
    m_isInterrupted = false;
    start();
    return true;
}

void Demuxer::stopDecode()
{
    m_isInterrupted = true;
    if (m_videoSocket) m_videoSocket->quitNotify();
    wait();
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

    packet = av_packet_alloc();
    if (!packet) goto runQuit;

    while (!m_isInterrupted) {
        bool ok = processNetworkPacket(packet);
        av_packet_unref(packet);
        if (!ok) break;
    }

runQuit:
    m_configBuffer.clear();
    m_configBuffer.shrink_to_fit();

    if (packet) av_packet_free(&packet);
    if (m_parser) av_parser_close(m_parser);
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
    quint8 header[HEADER_SIZE];
    if (recvData(header, HEADER_SIZE) < HEADER_SIZE) return false;

    uint64_t ptsFlags = readBigEndian<uint64_t>(header);
    uint32_t len = readBigEndian<uint32_t>(&header[8]);
    if (!len) return false;

    bool isConfig = (ptsFlags & SC_PACKET_FLAG_CONFIG);
    bool prependConfig = (!isConfig && !m_configBuffer.empty());
    
    uint32_t totalLen = len + (prependConfig ? m_configBuffer.size() : 0);

    if (av_new_packet(packet, static_cast<int>(totalLen)) != 0) return false;

    uint8_t *writePtr = packet->data;
    
    if (prependConfig) {
        std::memcpy(writePtr, m_configBuffer.data(), m_configBuffer.size());
        writePtr += m_configBuffer.size();
    }

    if (recvData(writePtr, len) < static_cast<qint32>(len)) return false;

    if (isConfig) {
        packet->pts = AV_NOPTS_VALUE;
    } else {
        packet->pts = ptsFlags & SC_PACKET_PTS_MASK;
    }

    if (ptsFlags & SC_PACKET_FLAG_KEY_FRAME) {
        packet->flags |= AV_PKT_FLAG_KEY;
    }
    packet->dts = packet->pts;

    if (isConfig) {
        m_configBuffer.assign(packet->data, packet->data + packet->size);
        
        AVPacket *clone = PacketPool::get().acquire();
        if (av_packet_ref(clone, packet) >= 0) {
            emit getConfigFrame(clone);
        } else {
            PacketPool::get().release(clone);
        }
        return true;
    }

    if (prependConfig) m_configBuffer.clear();

    return parse(packet);
}

bool Demuxer::parse(AVPacket *packet)
{
    quint8 *outData = nullptr;
    int outLen = 0;

    // Parser H264 FFmpeg
    av_parser_parse2(m_parser, m_codecCtx, &outData, &outLen, 
                     packet->data, packet->size, packet->pts, packet->dts, -1);

    if (m_parser->key_frame == 1) {
        packet->flags |= AV_PKT_FLAG_KEY;
    }

    // Cloning
    AVPacket *clone = PacketPool::get().acquire();
    if (av_packet_ref(clone, packet) < 0) {
        PacketPool::get().release(clone);
        return false;
    }

    emit getFrame(clone);
    return true;
}