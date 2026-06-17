#include "decoder.h"
#include "videobuffer.h"
#include "compat.h"
#include "qtscrcpytelemetry.h"

#include <QDebug>
#include <QMutexLocker>
#include <QtGlobal>
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
    m_packetQueue.reserve(MAX_PACKET_QUEUE_SIZE);
    m_telemetryEnabled = qsc::telemetry::enabled();

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

int Decoder::selectDecoderThreadCount() const
{
    const int logicalCpus = QThread::idealThreadCount();
    const int fallback = logicalCpus > 1 ? logicalCpus - 1 : 1;
    return qsc::telemetry::boundedEnvironmentInt(
        "QTSCRCPY_DECODER_THREADS", fallback, 0, 32);
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
    m_codecCtx->thread_count = selectDecoderThreadCount();
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
    if (m_telemetryEnabled) {
        m_queueWaitStats.reset();
        m_workerServiceStats.reset();
        m_frameIntervalStats.reset();
    }
    m_lastFrameTime = {};
    m_droppedPackets.store(0, std::memory_order_relaxed);
    m_recoveryEvents.store(0, std::memory_order_relaxed);
    m_maximumQueueDepth.store(0, std::memory_order_relaxed);
    m_flushBeforeNextDecode.store(false, std::memory_order_relaxed);
    m_stopping.store(false, std::memory_order_release);
    m_codecOpen.store(true, std::memory_order_release);

    qInfo("Decoder initialized. Threads: %d, queue capacity: %d",
          m_codecCtx->thread_count, MAX_PACKET_QUEUE_SIZE);
    start();
    return true;
}

void Decoder::close()
{
    const bool wasOpen = m_codecOpen.load(std::memory_order_acquire) || isRunning();

    m_stopping.store(true, std::memory_order_release);
    {
        QMutexLocker locker(&m_queueMutex);
        m_queueCondition.wakeAll();
    }

    if (isRunning()) wait();
    clearPacketQueue();

    if (wasOpen) {
        if (m_telemetryEnabled) logTimingStats();
        else logQueueHealth();
    }

    m_codecOpen.store(false, std::memory_order_release);
    m_codecCtx.reset();

    if (m_recvFrame) {
        av_frame_free(&m_recvFrame);
        m_recvFrame = nullptr;
    }
}

bool Decoder::enqueuePacket(PacketHandle packet)
{
    if (!packet || !m_codecOpen.load(std::memory_order_acquire) ||
        m_stopping.load(std::memory_order_acquire) || !isRunning()) {
        return false;
    }

    QQueue<QueuedPacket> discarded;
    bool accepted = false;
    bool recoveryTriggered = false;
    std::size_t depth = 0;

    {
        QMutexLocker locker(&m_queueMutex);
        if (m_stopping.load(std::memory_order_relaxed) ||
            !m_codecOpen.load(std::memory_order_relaxed)) {
            return false;
        }

        const bool isKeyFrame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        const auto enqueueCurrentPacket = [&]() {
            const auto enqueuedAt = m_telemetryEnabled
                ? Clock::now()
                : Clock::time_point{};
            m_packetQueue.enqueue(QueuedPacket{packet.release(), enqueuedAt});
            accepted = true;
        };

        if (m_waitingForKeyFrame) {
            if (isKeyFrame) {
                discarded.swap(m_packetQueue);
                enqueueCurrentPacket();
                m_waitingForKeyFrame = false;
                m_flushBeforeNextDecode.store(true, std::memory_order_release);
            }
        } else if (m_packetQueue.size() >= MAX_PACKET_QUEUE_SIZE) {
            discarded.swap(m_packetQueue);
            recoveryTriggered = true;

            if (isKeyFrame) {
                enqueueCurrentPacket();
                m_flushBeforeNextDecode.store(true, std::memory_order_release);
            } else {
                m_waitingForKeyFrame = true;
            }
        } else {
            enqueueCurrentPacket();
        }

        if (accepted) {
            depth = static_cast<std::size_t>(m_packetQueue.size());
            m_queueCondition.wakeOne();
        }
    }

    while (!discarded.isEmpty()) {
        PacketPool::get().release(discarded.dequeue().packet);
        m_droppedPackets.fetch_add(1, std::memory_order_relaxed);
    }

    if (recoveryTriggered) {
        m_recoveryEvents.fetch_add(1, std::memory_order_relaxed);
    }

    if (!accepted) {
        m_droppedPackets.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    updateMaximumQueueDepth(depth);
    return true;
}

void Decoder::run()
{
    while (!m_stopping.load(std::memory_order_acquire)) {
        QueuedPacket queued;
        bool hasPacket = false;

        {
            QMutexLocker locker(&m_queueMutex);
            while (m_packetQueue.isEmpty() &&
                   !m_stopping.load(std::memory_order_acquire)) {
                m_queueCondition.wait(&m_queueMutex);
            }

            if (m_stopping.load(std::memory_order_acquire)) break;
            if (!m_packetQueue.isEmpty()) {
                queued = m_packetQueue.dequeue();
                hasPacket = true;
            }
        }

        if (!hasPacket || !queued.packet) continue;

        if (m_telemetryEnabled) {
            m_queueWaitStats.add(Clock::now() - queued.enqueuedAt);
        }

        PacketHandle packet(queued.packet);
        if (m_flushBeforeNextDecode.exchange(false, std::memory_order_acq_rel)) {
            if (m_codecCtx) avcodec_flush_buffers(m_codecCtx.get());
            if (m_recvFrame) av_frame_unref(m_recvFrame);
        }

        if (m_telemetryEnabled) {
            const auto serviceStartedAt = Clock::now();
            decodePacket(std::move(packet));
            m_workerServiceStats.add(Clock::now() - serviceStartedAt);
        } else {
            decodePacket(std::move(packet));
        }
    }
}

void Decoder::decodePacket(PacketHandle packet)
{
    if (!packet || !m_codecCtx || !m_recvFrame ||
        !m_codecOpen.load(std::memory_order_acquire)) {
        return;
    }

    while (!m_stopping.load(std::memory_order_acquire)) {
        const int sendResult = avcodec_send_packet(m_codecCtx.get(), packet.get());
        if (sendResult == AVERROR(EAGAIN)) {
            drainDecodedFrames();
            continue;
        }
        if (sendResult < 0) return;
        break;
    }

    drainDecodedFrames();
}

void Decoder::drainDecodedFrames()
{
    if (!m_codecCtx || !m_recvFrame) return;

    while (!m_stopping.load(std::memory_order_acquire)) {
        const int receiveResult = avcodec_receive_frame(m_codecCtx.get(), m_recvFrame);
        if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) break;
        if (receiveResult < 0) break;

        if (m_telemetryEnabled) {
            const auto frameTime = Clock::now();
            if (m_lastFrameTime != Clock::time_point{}) {
                m_frameIntervalStats.add(frameTime - m_lastFrameTime);
            }
            m_lastFrameTime = frameTime;
        }

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

        av_frame_unref(m_recvFrame);
    }
}

void Decoder::clearPacketQueue()
{
    QQueue<QueuedPacket> pending;
    {
        QMutexLocker locker(&m_queueMutex);
        pending.swap(m_packetQueue);
        m_waitingForKeyFrame = false;
    }

    while (!pending.isEmpty()) {
        PacketPool::get().release(pending.dequeue().packet);
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

void Decoder::logQueueHealth() const
{
    const auto dropped = m_droppedPackets.load(std::memory_order_relaxed);
    const auto recoveries = m_recoveryEvents.load(std::memory_order_relaxed);
    if (dropped == 0 && recoveries == 0) return;

    qInfo() << "[Telemetry][Decoder] queue maxDepth="
            << m_maximumQueueDepth.load(std::memory_order_relaxed)
            << "dropped:" << dropped
            << "recovery events:" << recoveries;
}

void Decoder::logTimingStats() const
{
    const auto queue = m_queueWaitStats.summary();
    const auto service = m_workerServiceStats.summary();
    const auto interval = m_frameIntervalStats.summary();

    const auto logWindow = [](const char *label, const LatencySummary &summary) {
        if (summary.samples == 0) return;
        qInfo().nospace()
            << label
            << " samples=" << summary.samples
            << " p50=" << summary.p50Us / 1000.0 << "ms"
            << " p95=" << summary.p95Us / 1000.0 << "ms"
            << " p99=" << summary.p99Us / 1000.0 << "ms"
            << " max=" << summary.maxUs / 1000.0 << "ms";
    };

    qInfo() << "[Telemetry][Decoder] queue maxDepth="
            << m_maximumQueueDepth.load(std::memory_order_relaxed)
            << "dropped:" << m_droppedPackets.load(std::memory_order_relaxed)
            << "recovery events:" << m_recoveryEvents.load(std::memory_order_relaxed);
    logWindow("[Telemetry][Decoder] queueWait", queue);
    logWindow("[Telemetry][Decoder] workerService", service);
    logWindow("[Telemetry][Decoder] frameInterval", interval);
}

void Decoder::peekFrame(std::function<void(int, int, uint8_t *)> onFrame)
{
    if (m_vb) m_vb->peekRenderedFrame(std::move(onFrame));
}
