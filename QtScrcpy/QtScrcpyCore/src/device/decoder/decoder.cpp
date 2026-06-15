#include "decoder.h"
#include "videobuffer.h"
#include "demuxer.h"
#include "compat.h"

#include <QDebug>
#include <QMutexLocker>
#include <algorithm>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

void AVCodecContextDeleter::operator()(AVCodecContext *ctx) const
{
    if (ctx) avcodec_free_context(&ctx);
}

Decoder::Decoder(FrameCallback onFrame, QObject *parent)
    : QThread(parent)
    , m_vb(std::make_unique<VideoBuffer>())
    , m_onFrame(std::move(onFrame))
{
    if (m_vb) {
        connect(m_vb.get(), &VideoBuffer::updateFPS,
                this, &Decoder::updateFPS,
                Qt::DirectConnection);
    }
}

Decoder::~Decoder()
{
    close();
}

bool Decoder::open()
{
    if (isRunning() || m_codecOpen.load(std::memory_order_acquire)) return false;

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return false;

    m_codecCtx = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>(
        avcodec_alloc_context3(codec));
    if (!m_codecCtx) return false;

    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    m_codecCtx->thread_type = FF_THREAD_SLICE;
    m_codecCtx->thread_count = qMax(1, QThread::idealThreadCount() - 1);
    m_codecCtx->skip_loop_filter = AVDISCARD_NONREF;

    if (avcodec_open2(m_codecCtx.get(), codec, nullptr) < 0) {
        m_codecCtx.reset();
        return false;
    }

    m_recvFrame = av_frame_alloc();
    if (!m_recvFrame) {
        m_codecCtx.reset();
        return false;
    }

    clearPacketQueue();
    m_droppedPackets.store(0, std::memory_order_relaxed);
    m_maximumQueueDepth.store(0, std::memory_order_relaxed);
    m_stopping.store(false, std::memory_order_release);
    m_codecOpen.store(true, std::memory_order_release);

    qInfo("Decoder initialized. Threads: %d, queue capacity: %d",
          m_codecCtx->thread_count, MAX_PACKET_QUEUE_SIZE);
    start();
    return true;
}

void Decoder::close()
{
    m_stopping.store(true, std::memory_order_release);
    {
        QMutexLocker locker(&m_queueMutex);
        m_queueCondition.wakeAll();
    }

    if (isRunning()) wait();
    clearPacketQueue();

    m_codecOpen.store(false, std::memory_order_release);
    m_codecCtx.reset();

    if (m_recvFrame) {
        av_frame_free(&m_recvFrame);
        m_recvFrame = nullptr;
    }

    const auto dropped = m_droppedPackets.load(std::memory_order_relaxed);
    const auto maxDepth = m_maximumQueueDepth.load(std::memory_order_relaxed);
    if (dropped > 0) {
        qInfo() << "Decoder stopped. Dropped packets:" << dropped
                << "maximum queue depth:" << maxDepth;
    }
}

bool Decoder::enqueuePacket(AVPacket *packet)
{
    if (!packet || !m_codecOpen.load(std::memory_order_acquire) ||
        m_stopping.load(std::memory_order_acquire) || !isRunning()) {
        return false;
    }

    QQueue<AVPacket *> discarded;
    bool accepted = false;
    std::size_t depth = 0;

    {
        QMutexLocker locker(&m_queueMutex);
        if (m_stopping.load(std::memory_order_relaxed) ||
            !m_codecOpen.load(std::memory_order_relaxed)) {
            return false;
        }

        if (m_packetQueue.size() >= MAX_PACKET_QUEUE_SIZE) {
            if (packet->flags & AV_PKT_FLAG_KEY) {
                discarded.swap(m_packetQueue);
                m_packetQueue.enqueue(packet);
                accepted = true;
            }
        } else {
            m_packetQueue.enqueue(packet);
            accepted = true;
        }

        if (accepted) {
            depth = static_cast<std::size_t>(m_packetQueue.size());
            m_queueCondition.wakeOne();
        }
    }

    while (!discarded.isEmpty()) {
        PacketPool::get().release(discarded.dequeue());
        m_droppedPackets.fetch_add(1, std::memory_order_relaxed);
    }

    if (!accepted) {
        m_droppedPackets.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    updateMaximumQueueDepth(depth);
    return true;
}

void Decoder::onDecodeFrame(AVPacket *packet)
{
    if (!packet) return;
    if (!enqueuePacket(packet)) {
        PacketPool::get().release(packet);
    }
}

void Decoder::run()
{
    while (!m_stopping.load(std::memory_order_acquire)) {
        AVPacket *packet = nullptr;
        {
            QMutexLocker locker(&m_queueMutex);
            while (m_packetQueue.isEmpty() &&
                   !m_stopping.load(std::memory_order_acquire)) {
                m_queueCondition.wait(&m_queueMutex);
            }

            if (m_stopping.load(std::memory_order_acquire)) break;
            if (!m_packetQueue.isEmpty()) packet = m_packetQueue.dequeue();
        }

        if (packet) decodePacket(packet);
    }
}

void Decoder::decodePacket(AVPacket *packet)
{
    auto packetDeleter = [](AVPacket *value) {
        PacketPool::get().release(value);
    };
    std::unique_ptr<AVPacket, decltype(packetDeleter)> packetGuard(packet, packetDeleter);

    if (!packet || !m_codecCtx || !m_recvFrame ||
        !m_codecOpen.load(std::memory_order_acquire)) {
        return;
    }

    const int sendResult = avcodec_send_packet(m_codecCtx.get(), packet);
    if (sendResult < 0 && sendResult != AVERROR(EAGAIN)) return;

    while (!m_stopping.load(std::memory_order_acquire)) {
        const int receiveResult = avcodec_receive_frame(m_codecCtx.get(), m_recvFrame);
        if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) break;
        if (receiveResult < 0) break;

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
            std::span<const uint8_t> spanY(
                m_recvFrame->data[0],
                static_cast<std::size_t>(strideY) * static_cast<std::size_t>(height));
            std::span<const uint8_t> spanU(
                m_recvFrame->data[1],
                static_cast<std::size_t>(strideU) * static_cast<std::size_t>(chromaHeight));
            std::span<const uint8_t> spanV(
                m_recvFrame->data[2],
                static_cast<std::size_t>(strideV) * static_cast<std::size_t>(chromaHeight));

            m_onFrame(width, height, spanY, spanU, spanV,
                      strideY, strideU, strideV);
        }

        emit newFrame();
        av_frame_unref(m_recvFrame);
    }
}

void Decoder::clearPacketQueue()
{
    QQueue<AVPacket *> pending;
    {
        QMutexLocker locker(&m_queueMutex);
        pending.swap(m_packetQueue);
    }

    while (!pending.isEmpty()) {
        PacketPool::get().release(pending.dequeue());
    }
}

void Decoder::updateMaximumQueueDepth(std::size_t depth)
{
    std::size_t current = m_maximumQueueDepth.load(std::memory_order_relaxed);
    while (depth > current &&
           !m_maximumQueueDepth.compare_exchange_weak(
               current, depth,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

void Decoder::peekFrame(std::function<void(int, int, uint8_t *)> onFrame)
{
    if (m_vb) m_vb->peekRenderedFrame(std::move(onFrame));
}
