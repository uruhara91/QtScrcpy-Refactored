#include "decoder.h"
#include "videobuffer.h"
#include "demuxer.h"
#include "compat.h"

#include <QDebug>
#include <QThread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

void AVCodecContextDeleter::operator()(AVCodecContext* ctx) const {
    if (ctx) avcodec_free_context(&ctx);
}

Decoder::Decoder(FrameCallback onFrame, QObject *parent)
    : QThread(parent)
    , m_vb(std::make_unique<VideoBuffer>())
    , m_onFrame(std::move(onFrame))
{
    moveToThread(this);
    if (m_vb) connect(m_vb.get(), &VideoBuffer::updateFPS, this, &Decoder::updateFPS);
}

Decoder::~Decoder() {
    close();
    quit();
    wait();
}

void Decoder::run() { exec(); }

bool Decoder::open()
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return false;

    m_codecCtx = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>(avcodec_alloc_context3(codec));
    if (!m_codecCtx) return false;

    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    m_codecCtx->thread_type = FF_THREAD_SLICE;
    m_codecCtx->thread_count = qMax(1, QThread::idealThreadCount() - 1);
    m_codecCtx->skip_loop_filter = AVDISCARD_NONREF;

    if (avcodec_open2(m_codecCtx.get(), codec, nullptr) < 0) return false;

    m_recvFrame = av_frame_alloc();
    if (!m_recvFrame) {
        m_codecCtx.reset();
        return false;
    }

    m_isCodecCtxOpen = true;
    qInfo("Decoder initialized. Threads: %d", m_codecCtx->thread_count);
    start();
    return true;
}

void Decoder::close()
{
    quit();
    wait();

    m_codecCtx.reset();
    m_isCodecCtxOpen = false;

    if (m_recvFrame) {
        av_frame_free(&m_recvFrame);
        m_recvFrame = nullptr;
    }
}

void Decoder::onDecodeFrame(AVPacket *packet)
{
    auto packetDeleter = [](AVPacket* p) { PacketPool::get().release(p); };
    std::unique_ptr<AVPacket, decltype(packetDeleter)> packetGuard(packet, packetDeleter);

    if (!packet || !m_codecCtx || !m_isCodecCtxOpen || !m_recvFrame) return;
    if (avcodec_send_packet(m_codecCtx.get(), packet) < 0) return;

    while (true) {
        int ret = avcodec_receive_frame(m_codecCtx.get(), m_recvFrame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;

        if (m_vb) m_vb->updateLatestFrame(m_recvFrame);

        const int width = m_recvFrame->width;
        const int height = m_recvFrame->height;
        const int chromaHeight = (height + 1) / 2;
        const int strideY = m_recvFrame->linesize[0];
        const int strideU = m_recvFrame->linesize[1];
        const int strideV = m_recvFrame->linesize[2];

        if (m_onFrame && width > 0 && height > 0 &&
            strideY > 0 && strideU > 0 && strideV > 0 &&
            m_recvFrame->data[0] && m_recvFrame->data[1] && m_recvFrame->data[2]) {
            std::span<const uint8_t> spanY(m_recvFrame->data[0], static_cast<std::size_t>(strideY) * height);
            std::span<const uint8_t> spanU(m_recvFrame->data[1], static_cast<std::size_t>(strideU) * chromaHeight);
            std::span<const uint8_t> spanV(m_recvFrame->data[2], static_cast<std::size_t>(strideV) * chromaHeight);

            m_onFrame(width, height, spanY, spanU, spanV, strideY, strideU, strideV);
        }

        emit newFrame();
        av_frame_unref(m_recvFrame);
    }
}

void Decoder::peekFrame(std::function<void (int, int, uint8_t *)> onFrame)
{
    if (m_vb) m_vb->peekRenderedFrame(onFrame);
}
