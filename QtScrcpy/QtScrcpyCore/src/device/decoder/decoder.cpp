#include "decoder.h"
#include "videobuffer.h"
#include "qtscrcpytelemetry.h"

#include <QDebug>
#include <QMutexLocker>
#include <QtGlobal>
#include <array>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

void AVCodecContextDeleter::operator()(AVCodecContext *ctx) const
{
    if (ctx) avcodec_free_context(&ctx);
}

void AVBufferRefDeleter::operator()(AVBufferRef *ref) const
{
    if (ref) av_buffer_unref(&ref);
}

void SwsContextDeleter::operator()(SwsContext *ctx) const
{
    if (ctx) sws_freeContext(ctx);
}

namespace {

// ---------------------------------------------------------------------
// Hardware-decode backend selection
// ---------------------------------------------------------------------
// scrcpy's Android-side encoder always produces H.264 (server.cpp locks
// the negotiated codec to H.264 - see the comment there). Every mainstream
// desktop GPU has a fixed-function H.264 decode block, so offloading to it
// removes the single biggest CPU cost in the whole pipeline instead of
// burning a full CPU core on software entropy/motion-compensation work.
//
// Candidates are tried in order for the current platform; the first one
// that both (a) the linked libavcodec actually advertises for the H.264
// decoder and (b) successfully creates a real device context, wins. If
// none do (missing driver, headless/software-only GPU, VM without GPU
// passthrough, etc.) the decoder silently stays on the software path that
// already existed - hardware decode is strictly a performance opt-in,
// never a correctness requirement.
#if defined(Q_OS_WIN32)
constexpr std::array<AVHWDeviceType, 3> kHwCandidates{
    AV_HWDEVICE_TYPE_D3D11VA, // modern, broadly supported (Intel/AMD/NVIDIA)
    AV_HWDEVICE_TYPE_DXVA2,   // legacy fallback for older drivers/GPUs
    AV_HWDEVICE_TYPE_CUDA,    // NVDEC, in case D3D11VA/DXVA2 are unavailable
};
#elif defined(Q_OS_OSX)
constexpr std::array<AVHWDeviceType, 1> kHwCandidates{
    AV_HWDEVICE_TYPE_VIDEOTOOLBOX, // the only game in town on macOS
};
#elif defined(Q_OS_LINUX)
constexpr std::array<AVHWDeviceType, 2> kHwCandidates{
    AV_HWDEVICE_TYPE_VAAPI, // Intel/AMD, and NVIDIA via nvidia-vaapi-driver
    AV_HWDEVICE_TYPE_CUDA,  // NVDEC, for NVIDIA setups without VA-API
};
#else
constexpr std::array<AVHWDeviceType, 0> kHwCandidates{};
#endif

} // namespace

Decoder::Decoder(FrameCallback onFrame, bool useHwDecode, QObject *parent)
    : QThread(parent)
    , m_vb(std::make_unique<VideoBuffer>())
    , m_onFrame(std::move(onFrame))
    , m_hwDecodePreferred(useHwDecode)
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
    // Slice-threading only helps when the encoder actually emits multiple
    // slices per frame. Android's MediaCodec H.264 encoder, which is the
    // only source scrcpy-server ever streams from, virtually always emits
    // one slice per frame in practice - so FF_THREAD_SLICE has little to
    // parallelize here and this is mostly a defensive default rather than
    // a measured win. It is intentionally overridable without a rebuild so
    // it can actually be benchmarked against FF_THREAD_FRAME (which trades
    // a frame of extra latency for real parallelism) on a given machine:
    //   QTSCRCPY_DECODER_THREAD_TYPE_FRAME=1 ./QtScrcpy
    // Also moot entirely once hardware decode is active below, since the
    // GPU's fixed-function decode block does this work instead of FFmpeg's
    // software threading.
    const bool useFrameThreading = qsc::telemetry::environmentFlag(
        "QTSCRCPY_DECODER_THREAD_TYPE_FRAME", false);
    m_codecCtx->thread_type = useFrameThreading ? FF_THREAD_FRAME : FF_THREAD_SLICE;
    m_codecCtx->thread_count = selectDecoderThreadCount();
    m_codecCtx->skip_loop_filter = AVDISCARD_NONREF;

    resetHwAccelState();
    m_codecCtx->opaque = this;
    if (tryInitHwAccel(codec)) {
        m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx.get());
        m_codecCtx->get_format = &Decoder::getHwFormat;
        m_hwAccelActive.store(true, std::memory_order_release);
    }

    if (avcodec_open2(m_codecCtx.get(), codec, nullptr) < 0) {
        if (m_hwAccelActive.load(std::memory_order_acquire)) {
            // The hw config looked valid but the driver refused to open it
            // for this exact stream (profile/level, VRAM pressure, etc.).
            // Don't fail the whole session over what is purely a
            // performance optimization - retry once in plain software mode.
            qWarning("Decoder: hardware-accelerated open failed, "
                     "retrying with software decode");
            resetHwAccelState();
            m_codecCtx->get_format = nullptr;
            if (avcodec_open2(m_codecCtx.get(), codec, nullptr) < 0) {
                m_codecCtx.reset();
                return false;
            }
        } else {
            m_codecCtx.reset();
            return false;
        }
    }

    m_recvFrame = av_frame_alloc();
    m_hwTransferFrame = av_frame_alloc();
    m_hwSwFrame = av_frame_alloc();
    if (!m_recvFrame || !m_hwTransferFrame || !m_hwSwFrame) {
        if (m_recvFrame) av_frame_free(&m_recvFrame);
        if (m_hwTransferFrame) av_frame_free(&m_hwTransferFrame);
        if (m_hwSwFrame) av_frame_free(&m_hwSwFrame);
        m_recvFrame = m_hwTransferFrame = m_hwSwFrame = nullptr;
        resetHwAccelState();
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

    if (m_hwAccelActive.load(std::memory_order_acquire)) {
        qInfo("Decoder initialized. Hardware decode: %s, queue capacity: %d",
              av_hwdevice_get_type_name(m_hwDeviceType), MAX_PACKET_QUEUE_SIZE);
    } else {
        qInfo("Decoder initialized. Software decode, threads: %d, queue capacity: %d",
              m_codecCtx->thread_count, MAX_PACKET_QUEUE_SIZE);
    }
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
    // avcodec_free_context() already unrefs AVCodecContext::hw_device_ctx
    // internally; resetHwAccelState() below releases our own extra
    // reference on top of that (see tryInitHwAccel()).
    m_codecCtx.reset();
    resetHwAccelState();

    if (m_recvFrame) {
        av_frame_free(&m_recvFrame);
        m_recvFrame = nullptr;
    }
    if (m_hwTransferFrame) {
        av_frame_free(&m_hwTransferFrame);
        m_hwTransferFrame = nullptr;
    }
    if (m_hwSwFrame) {
        av_frame_free(&m_hwSwFrame);
        m_hwSwFrame = nullptr;
    }
    m_hwSwsCtx.reset();
}

AVPixelFormat Decoder::getHwFormat(AVCodecContext *ctx, const AVPixelFormat *pixFmts)
{
    auto *self = static_cast<Decoder *>(ctx->opaque);
    if (self) {
        for (const AVPixelFormat *fmt = pixFmts; *fmt != AV_PIX_FMT_NONE; ++fmt) {
            if (*fmt == self->m_hwPixFmt) return *fmt;
        }
    }
    // The hw format we asked for isn't in the list libavcodec is actually
    // offering for this stream. Rather than fail the decode outright,
    // return whatever it offered first - avcodec_open2()/the first
    // avcodec_send_packet() will then simply run in software, and
    // drainDecodedFrames() will never see m_hwPixFmt so it takes the plain
    // software-frame path automatically.
    qWarning("Decoder: requested hardware pixel format not offered by codec; "
             "falling back to software decode");
    return pixFmts[0];
}

bool Decoder::tryInitHwAccel(const AVCodec *codec)
{
    if (kHwCandidates.empty()) return false; // unknown/unsupported platform

    // QTSCRCPY_DISABLE_HWACCEL remains available as an emergency
    // override (e.g. for debugging) even with the UI "decoder:" dropdown
    // now driving this normally via m_hwDecodePreferred - if explicitly
    // set, it takes precedence over whatever the UI has configured. Same
    // pattern as QTSCRCPY_SERVER_ROOT vs DeviceParams::useRoot in
    // server.cpp.
    const bool disabled = qEnvironmentVariableIsSet("QTSCRCPY_DISABLE_HWACCEL")
        ? qsc::telemetry::environmentFlag("QTSCRCPY_DISABLE_HWACCEL", false)
        : !m_hwDecodePreferred;

    if (disabled) {
        qInfo("Decoder: hardware decode disabled (%s)",
              qEnvironmentVariableIsSet("QTSCRCPY_DISABLE_HWACCEL")
                  ? "via QTSCRCPY_DISABLE_HWACCEL"
                  : "user preference: decoder dropdown set to Software");
        return false;
    }

    for (const AVHWDeviceType candidateType : kHwCandidates) {
        AVPixelFormat candidateFormat = AV_PIX_FMT_NONE;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
            if (!config) break; // no more configs advertised by this decoder
            if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                config->device_type == candidateType) {
                candidateFormat = config->pix_fmt;
                break;
            }
        }

        if (candidateFormat == AV_PIX_FMT_NONE) {
            // This build of libavcodec doesn't advertise this backend for
            // H.264 at all (e.g. FFmpeg compiled without --enable-vaapi) -
            // nothing to try, move on to the next candidate.
            continue;
        }

        AVBufferRef *rawDeviceCtx = nullptr;
        const int result = av_hwdevice_ctx_create(&rawDeviceCtx, candidateType,
                                                   nullptr, nullptr, 0);
        if (result < 0) {
            char errBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(result, errBuf, sizeof(errBuf));
            qInfo("Decoder: %s hardware device init failed (%s); trying next option",
                  av_hwdevice_get_type_name(candidateType), errBuf);
            continue;
        }

        m_hwDeviceCtx.reset(rawDeviceCtx);
        m_hwPixFmt = candidateFormat;
        m_hwDeviceType = candidateType;
        return true;
    }

    return false;
}

void Decoder::resetHwAccelState()
{
    m_hwAccelActive.store(false, std::memory_order_release);
    m_hwPixFmt = AV_PIX_FMT_NONE;
    m_hwDeviceType = AV_HWDEVICE_TYPE_NONE;
    if (m_codecCtx) {
        av_buffer_unref(&m_codecCtx->hw_device_ctx);
    }
    m_hwDeviceCtx.reset();
}

bool Decoder::transferHwFrame()
{
    if (!m_hwTransferFrame || !m_hwSwFrame) return false;

    if (av_hwframe_transfer_data(m_hwTransferFrame, m_recvFrame, 0) < 0) {
        // Reading the surface back can fail transiently (driver hiccup,
        // VRAM pressure). Drop this one frame instead of tearing down the
        // whole decode session - the next frame tries independently.
        return false;
    }

    const int width = m_recvFrame->width;
    const int height = m_recvFrame->height;
    const auto sourceFormat = static_cast<AVPixelFormat>(m_hwTransferFrame->format);

    const bool needsRealloc = !m_hwSwFrame->buf[0] ||
                               m_hwSwFrame->width != width ||
                               m_hwSwFrame->height != height;
    if (needsRealloc) {
        av_frame_unref(m_hwSwFrame);
        m_hwSwFrame->format = AV_PIX_FMT_YUV420P;
        m_hwSwFrame->width = width;
        m_hwSwFrame->height = height;
        if (av_frame_get_buffer(m_hwSwFrame, 32) < 0) return false;
    } else if (av_frame_make_writable(m_hwSwFrame) < 0) {
        return false;
    }

    // The hw backend's natural transfer format (almost always NV12 across
    // VAAPI/D3D11VA/DXVA2/VideoToolbox/NVDEC) is reshuffled into planar
    // YUV420P here so the rest of the pipeline - which was written for,
    // and already thoroughly exercises, plain software-decoded YUV420P -
    // needs zero changes to support hardware decode. Width/height are
    // identical on both sides, so this is a pure plane-layout conversion,
    // not a rescale; cost is a single SIMD-optimized libswscale pass, tiny
    // next to the decode work it replaces.
    SwsContext *rawSws = sws_getCachedContext(
        m_hwSwsCtx.release(),
        width, height, sourceFormat,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    m_hwSwsCtx.reset(rawSws);
    if (!m_hwSwsCtx) return false;

    sws_scale(m_hwSwsCtx.get(),
              m_hwTransferFrame->data, m_hwTransferFrame->linesize, 0, height,
              m_hwSwFrame->data, m_hwSwFrame->linesize);

    return true;
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
            // Forces transferHwFrame() to reallocate m_hwTransferFrame /
            // m_hwSwFrame at their new size instead of assuming the
            // previous frame's dimensions still apply (see below).
            if (m_hwTransferFrame) av_frame_unref(m_hwTransferFrame);
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

        // Hardware-decoded frames arrive as an opaque GPU surface (a
        // VASurfaceID/CVPixelBuffer/ID3D11Texture2D handle wrapped in the
        // AVFrame, format == m_hwPixFmt, data[]/linesize[] not directly
        // readable) rather than plain mapped memory. transferHwFrame()
        // reads it back into system memory and reshuffles it into YUV420P
        // in m_hwSwFrame, so everything downstream of this point - the
        // span/stride logic, VideoBuffer, the renderer - stays completely
        // unaware of whether hw accel is even active.
        const AVFrame *presentFrame = m_recvFrame;
        bool dropFrame = false;
        if (m_hwAccelActive.load(std::memory_order_relaxed) &&
            m_recvFrame->format == static_cast<int>(m_hwPixFmt)) {
            if (transferHwFrame()) {
                presentFrame = m_hwSwFrame;
            } else {
                dropFrame = true;
            }
        }

        if (dropFrame) {
            av_frame_unref(m_recvFrame);
            continue;
        }

        if (m_vb) m_vb->updateLatestFrame(presentFrame);

        const int width = presentFrame->width;
        const int height = presentFrame->height;
        const int chromaHeight = (height + 1) / 2;
        const int strideY = presentFrame->linesize[0];
        const int strideU = presentFrame->linesize[1];
        const int strideV = presentFrame->linesize[2];

        if (m_onFrame && width > 0 && height > 0 &&
            strideY > 0 && strideU > 0 && strideV > 0 &&
            presentFrame->data[0] && presentFrame->data[1] && presentFrame->data[2]) {
            std::span<const uint8_t> spanY(
                presentFrame->data[0],
                static_cast<std::size_t>(strideY) * static_cast<std::size_t>(height));
            std::span<const uint8_t> spanU(
                presentFrame->data[1],
                static_cast<std::size_t>(strideU) * static_cast<std::size_t>(chromaHeight));
            std::span<const uint8_t> spanV(
                presentFrame->data[2],
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
