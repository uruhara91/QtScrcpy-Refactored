#include "decoder.h"
#include "videobuffer.h"
#include "compat.h"

#include <QDebug>
#include <QThread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

void AVCodecContextDeleter::operator()(AVCodecContext* ctx) const {
    if (ctx) {
        avcodec_free_context(&ctx);
    }
}

Decoder::Decoder(FrameCallback onFrame, QObject *parent)
    : QThread(parent)
    , m_vb(std::make_unique<VideoBuffer>())
    , m_onFrame(std::move(onFrame))
{
    moveToThread(this);
    if (m_vb) {
        connect(m_vb.get(), &VideoBuffer::updateFPS, this, &Decoder::updateFPS);
    }
}

Decoder::~Decoder() {
    close();
    quit();
    wait();
}

void Decoder::run() {
    exec();
}

bool Decoder::open()
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        qCritical("Decoder: H.264 decoder not found");
        return false;
    }

    m_codecCtx = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>(avcodec_alloc_context3(codec));
    if (!m_codecCtx) {
        qCritical("Decoder: Could not allocate codec context");
        return false;
    }

    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    m_codecCtx->thread_type = FF_THREAD_SLICE;
    m_codecCtx->thread_count = qMax(1, QThread::idealThreadCount() - 1);
    m_codecCtx->skip_loop_filter = AVDISCARD_NONREF;

    if (avcodec_open2(m_codecCtx.get(), codec, nullptr) < 0) {
        qCritical("Decoder: Could not open H.264 codec");
        return false;
    }
    
    m_isCodecCtxOpen = true;
    qInfo("SW Decoder initialized. Threads: %d", m_codecCtx->thread_count);
    
    start();
    
    return true;
}

void Decoder::close()
{
    quit();
    wait();

    m_codecCtx.reset();
    m_isCodecCtxOpen = false;
}

void Decoder::onDecodeFrame(AVPacket *packet)
{
    auto packetDeleter = [](AVPacket* p) { 
        if (p) av_packet_free(&p); 
    };

    std::unique_ptr<AVPacket, decltype(packetDeleter)> packetGuard(packet, packetDeleter);

    if (!m_codecCtx || !m_isCodecCtxOpen) {
        return;
    }
    
    int ret = avcodec_send_packet(m_codecCtx.get(), packet);
    if (ret < 0) {
        if (ret != AVERROR(EAGAIN)) {
            qWarning("Decoder: Send packet error: %d", ret);
        }
        return;
    }

    while (true) {
        AVFrame *decodingFrame = m_vb->decodingFrame();
        
        ret = avcodec_receive_frame(m_codecCtx.get(), decodingFrame);
        if (ret == 0) {
            pushFrameToBuffer();
        } else if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            // Butuh input lagi atau EOF
            break;
        } else {
            qWarning("Decoder: Receive frame error: %d", ret);
            return;
        }
    }
}

void Decoder::pushFrameToBuffer()
{
    if (!m_vb) return;

    bool previousFrameSkipped = true;
    
    m_vb->offerDecodedFrame(previousFrameSkipped);
    
    const AVFrame *frame = m_vb->consumeRenderedFrame();
    
    if (m_onFrame && frame) {
         std::span<const uint8_t> spanY(frame->data[0], frame->linesize[0] * frame->height);
         std::span<const uint8_t> spanU(frame->data[1], (frame->linesize[1] * frame->height) / 2);
         std::span<const uint8_t> spanV(frame->data[2], (frame->linesize[2] * frame->height) / 2);

         m_onFrame(frame->width, frame->height,
                   spanY, spanU, spanV,
                   frame->linesize[0], frame->linesize[1], frame->linesize[2]);
    }
    
    emit newFrame();
}

void Decoder::peekFrame(std::function<void (int, int, uint8_t *)> onFrame)
{
    if (!m_vb) {
        return;
    }
    
    m_vb->peekRenderedFrame(onFrame);
}