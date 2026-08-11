#include "decoder.h"
#include "videobuffer.h"
#include "qtscrcpytelemetry.h"

#include <QDebug>
#include <QMutexLocker>
#include <QStringList>
#include <QtGlobal>
#include <array>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#ifdef Q_OS_LINUX
#include <libavutil/hwcontext_drm.h>
#endif
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

Decoder::Decoder(FrameCallback onFrame, bool useHwDecode,
                 HwFrameCallback onHwFrame, QObject *parent)
    : QThread(parent)
    , m_vb(std::make_unique<VideoBuffer>())
    , m_onFrame(std::move(onFrame))
    , m_onHwFrame(std::move(onHwFrame))
    , m_hwDecodePreferred(useHwDecode)
{
    m_packetQueue.reserve(MAX_PACKET_QUEUE_SIZE);
    m_telemetryEnabled = qsc::telemetry::enabled();
    m_zeroCopyRequested = static_cast<bool>(m_onHwFrame) &&
        qsc::telemetry::environmentFlag("QTSCRCPY_EXPERIMENTAL_ZEROCOPY", false);

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
    m_hwTransferFormatProbed = false;
    m_zeroCopySetupDone = false;
    m_zeroCopyAvailable = false;
    m_zeroCopyConsecutiveDeclines = 0;
    m_zeroCopyActive.store(false, std::memory_order_release);
    m_drmFramesCtx.reset();
    m_drmDeviceCtx.reset();
    if (m_codecCtx) {
        av_buffer_unref(&m_codecCtx->hw_device_ctx);
    }
    m_hwDeviceCtx.reset();
}

// ---------------------------------------------------------------------
// Experimental zero-copy hardware-frame handoff (VAAPI / Linux only)
// ---------------------------------------------------------------------
// The copy-back path above (transferHwFrame()) reads a decoded VAAPI
// surface back into system memory with av_hwframe_transfer_data(), then
// (usually) reshuffles it with sws_scale(). That readback is itself a
// GPU->CPU synchronization point and a real memory copy - on hardware
// where that turns out to cost more than it saves (see the telemetry
// discussion in AUDIT_FIXES.md), the alternative is to never leave the
// GPU at all: export the decoded VAAPI surface as a set of DMA-BUF file
// descriptors (av_hwframe_map() to AV_PIX_FMT_DRM_PRIME, which FFmpeg
// implements via VAAPI's vaExportSurfaceHandle() internally) and hand
// those off to the renderer to import directly as GL textures via EGL's
// EGL_EXT_image_dma_buf_import extension - no CPU ever touches the pixel
// data.
//
// This is meaningfully riskier than the copy-back path: it depends on
// the GL context actually being EGL-backed (not GLX - see
// QYuvOpenGLWidget::logGlPlatformBackend()), on the specific driver
// supporting DMA-BUF export/import for the exact surface layout in use,
// and on getting several non-obvious pieces of cross-API plumbing
// exactly right (device/frames-context derivation, DRM format modifiers,
// frame lifetime across the decoder/render thread boundary). Every
// fallible step below is treated as exactly that - fallible - and falls
// back to the existing, proven transferHwFrame() copy-back path rather
// than ever hard-failing. Opt-in only, via QTSCRCPY_EXPERIMENTAL_ZEROCOPY
// (see the constructor) - not the default, and not intended to be until
// it's had real hardware validation across more than one machine.
#ifdef Q_OS_LINUX

bool Decoder::trySetupZeroCopy(AVFrame *vaapiFrame)
{
    m_zeroCopySetupDone = true; // one attempt per session either way

    if (!vaapiFrame->hw_frames_ctx) {
        qWarning("Decoder: zero-copy setup skipped - decoded frame has no "
                 "hw_frames_ctx");
        return false;
    }

    // Definitive diagnostic for "is DRM hwdevice support even compiled
    // into this FFmpeg build at all" - a custom/minimal FFmpeg build
    // (e.g. one using --disable-everything, --disable-autodetect) can
    // easily end up without AV_HWDEVICE_TYPE_DRM if the build didn't
    // explicitly pass --enable-libdrm, since --disable-autodetect means
    // FFmpeg's configure script won't go looking for libdrm on its own.
    // If DRM is missing from this list, av_hwdevice_ctx_create() below
    // fails - but generically, as if it were a plain allocation failure
    // ("Cannot allocate memory"), which is indistinguishable from an
    // actual out-of-memory condition without a list like this to compare
    // against.
    {
        QStringList availableTypes;
        for (AVHWDeviceType t = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE);
             t != AV_HWDEVICE_TYPE_NONE;
             t = av_hwdevice_iterate_types(t)) {
            availableTypes << QString::fromUtf8(av_hwdevice_get_type_name(t));
        }
        qInfo().noquote() << "Decoder: hwdevice types compiled into this FFmpeg build:"
                          << (availableTypes.isEmpty() ? QStringLiteral("(none)")
                                                        : availableTypes.join(", "))
                          << "- zero-copy needs \"drm\" in this list";
    }

    AVBufferRef *rawDrmDeviceCtx = nullptr;
    // Prefer deriving the DRM device directly from the VAAPI device this
    // decoder already has open (m_hwDeviceCtx) rather than independently
    // auto-selecting one - guaranteed to be the exact same physical GPU
    // VAAPI is decoding on, and sidesteps whatever auto-detection issue
    // causes av_hwdevice_ctx_create(..., AV_HWDEVICE_TYPE_DRM, nullptr,
    // ...) to fail outright ("Cannot allocate memory") on at least some
    // systems (observed on an Intel Tiger Lake iGPU-only laptop).
    int result = av_hwdevice_ctx_create_derived(&rawDrmDeviceCtx, AV_HWDEVICE_TYPE_DRM,
                                                  m_hwDeviceCtx.get(), 0);
    if (result < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(result, errBuf, sizeof(errBuf));
        qInfo("Decoder: could not derive a DRM device from the active VAAPI "
              "device (%s); trying /dev/dri/renderD128 directly as a "
              "fallback", errBuf);
        result = av_hwdevice_ctx_create(&rawDrmDeviceCtx, AV_HWDEVICE_TYPE_DRM,
                                          "/dev/dri/renderD128", nullptr, 0);
    }
    if (result < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(result, errBuf, sizeof(errBuf));
        qWarning("Decoder: zero-copy unavailable - could not open a DRM "
                 "device (%s); staying on the hardware copy-back path",
                 errBuf);
        return false;
    }
    m_drmDeviceCtx.reset(rawDrmDeviceCtx);

    AVBufferRef *rawDrmFramesCtx = nullptr;
    result = av_hwframe_ctx_create_derived(&rawDrmFramesCtx, AV_PIX_FMT_DRM_PRIME,
                                             m_drmDeviceCtx.get(),
                                             vaapiFrame->hw_frames_ctx,
                                             AV_HWFRAME_MAP_READ);
    if (result < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(result, errBuf, sizeof(errBuf));
        qWarning("Decoder: zero-copy unavailable - could not derive a DRM "
                 "frames context from the VAAPI one (%s); staying on the "
                 "hardware copy-back path", errBuf);
        m_drmDeviceCtx.reset();
        return false;
    }
    m_drmFramesCtx.reset(rawDrmFramesCtx);

    m_zeroCopyAvailable = true;
    qInfo("Decoder: zero-copy hardware frame export is available "
          "(VAAPI -> DRM_PRIME) and will be offered to the renderer");
    return true;
}

bool Decoder::trySubmitZeroCopyFrame(AVFrame *vaapiFrame)
{
    if (!m_zeroCopyRequested || !m_onHwFrame ||
        m_zeroCopyConsecutiveDeclines >= kMaxConsecutiveZeroCopyDeclines) {
        return false;
    }

    if (!m_zeroCopySetupDone) {
        if (!trySetupZeroCopy(vaapiFrame)) return false;
    }
    if (!m_zeroCopyAvailable) return false;

    AVFrame *mapped = av_frame_alloc();
    if (!mapped) return false;

    mapped->hw_frames_ctx = av_buffer_ref(m_drmFramesCtx.get());
    if (!mapped->hw_frames_ctx) {
        av_frame_free(&mapped);
        return false;
    }
    mapped->format = AV_PIX_FMT_DRM_PRIME;

    if (av_hwframe_map(mapped, vaapiFrame, AV_HWFRAME_MAP_READ) < 0) {
        // Can genuinely be transient (e.g. a driver hiccup) rather than a
        // fundamental incompatibility, so this deliberately does NOT
        // advance m_zeroCopyConsecutiveDeclines / count toward giving up
        // for the session - only an explicit decline from the sink
        // itself does that, below.
        qWarning("Decoder: zero-copy frame mapping failed for this frame; "
                 "using the copy-back path for it instead");
        av_frame_free(&mapped);
        return false;
    }

    const auto *drmDesc = reinterpret_cast<const AVDRMFrameDescriptor *>(mapped->data[0]);
    if (!drmDesc || drmDesc->nb_layers <= 0 ||
        drmDesc->nb_layers > DrmFrameDescriptor::kMaxPlanes) {
        qWarning("Decoder: zero-copy frame has an unexpected layer count "
                 "(%d); using the copy-back path for it instead",
                 drmDesc ? drmDesc->nb_layers : -1);
        av_frame_free(&mapped);
        return false;
    }

    const int width = vaapiFrame->width;
    const int height = vaapiFrame->height;

    DrmFrameDescriptor drm;
    drm.planeCount = drmDesc->nb_layers;
    for (int i = 0; i < drmDesc->nb_layers; ++i) {
        const AVDRMLayerDescriptor &layer = drmDesc->layers[i];
        if (layer.nb_planes != 1) {
            // Every layer FFmpeg's VAAPI -> DRM_PRIME mapping produces
            // for an 8-bit 4:2:0 surface (the only kind this decoder ever
            // hands to hardware decode - Android's H.264 encoders are
            // always 8-bit) is expected to be exactly one plane per layer
            // (a separate luma layer and chroma layer, not one combined
            // multi-plane layer). A different shape here means an
            // assumption this code depends on doesn't hold on this
            // driver - safer to bail than guess at the layout.
            qWarning("Decoder: zero-copy layer %d has %d planes (expected "
                     "1); using the copy-back path for this frame",
                     i, layer.nb_planes);
            av_frame_free(&mapped);
            return false;
        }
        const AVDRMPlaneDescriptor &plane = layer.planes[0];
        if (plane.object_index < 0 || plane.object_index >= drmDesc->nb_objects) {
            qWarning("Decoder: zero-copy layer %d has an out-of-range "
                     "object_index; using the copy-back path for this frame", i);
            av_frame_free(&mapped);
            return false;
        }
        const AVDRMObjectDescriptor &object = drmDesc->objects[plane.object_index];

        DrmFramePlane &out = drm.planes[i];
        out.fd = object.fd;
        out.fourcc = layer.format;
        out.modifier = object.format_modifier;
        out.offset = plane.offset;
        out.pitch = plane.pitch;
        // Layer 0 is always the full-resolution luma plane and layer 1
        // (when present) the 4:2:0-subsampled chroma plane, in that
        // order, for every pixel format this decoder ever hands to
        // hardware decode - see AVDRMFrameDescriptor's documentation in
        // libavutil/hwcontext_drm.h ("the order of the planes ... must be
        // the same as ... the equivalent software format"), and this
        // decoder's software format is always YUV420P/NV12-family.
        if (i == 0) {
            out.width = width;
            out.height = height;
        } else {
            out.width = (width + 1) / 2;
            out.height = (height + 1) / 2;
        }
    }

    // Keeps the mapped frame - and, transitively, the underlying VAAPI
    // surface (av_hwframe_map() holds its own reference, independent of
    // vaapiFrame/m_recvFrame) - alive until the sink calls release(),
    // which per the documented contract (QtScrcpyCore.h) it must do
    // exactly once, whenever it's actually done with the fds (e.g. right
    // after eglCreateImageKHR() imports them). A shared_ptr with a custom
    // deleter is the simplest way to make "free this AVFrame" callable
    // exactly once from an arbitrary later point, on an arbitrary thread.
    std::shared_ptr<AVFrame> mappedRef(mapped, [](AVFrame *frame) {
        av_frame_free(&frame);
    });

    const bool accepted = m_onHwFrame(width, height, drm, [mappedRef]() mutable {
        mappedRef.reset();
    });

    if (accepted) {
        m_zeroCopyConsecutiveDeclines = 0;
        m_zeroCopyActive.store(true, std::memory_order_relaxed);
    } else {
        // The sink's contract requires it to have already called
        // release() synchronously before returning false (see
        // QtScrcpyCore.h) - so by this point every copy of mappedRef
        // (the one captured above, and this local one once it goes out
        // of scope momentarily) has already dropped to zero references
        // and the frame is already freed.
        ++m_zeroCopyConsecutiveDeclines;
        if (m_zeroCopyConsecutiveDeclines >= kMaxConsecutiveZeroCopyDeclines) {
            qInfo("Decoder: zero-copy frame declined by the renderer %d times in "
                  "a row; disabling zero-copy for the rest of this session "
                  "(falling back to the regular hardware copy-back path)",
                  m_zeroCopyConsecutiveDeclines);
        } else {
            qInfo("Decoder: zero-copy frame declined by the renderer (%d/%d) - "
                  "retrying with the next frame; falling back to the regular "
                  "hardware copy-back path for this one",
                  m_zeroCopyConsecutiveDeclines, kMaxConsecutiveZeroCopyDeclines);
        }
    }

    return accepted;
}

#else // !Q_OS_LINUX

bool Decoder::trySetupZeroCopy(AVFrame *)
{
    return false;
}

bool Decoder::trySubmitZeroCopyFrame(AVFrame *)
{
    return false;
}

#endif

AVFrame *Decoder::transferHwFrame()
{
    if (!m_hwTransferFrame || !m_hwSwFrame) return nullptr;

    // A resolution change (most commonly the device rotating - Android
    // reconfigures its encoder with swapped width/height and emits a new
    // SPS) arrives as an entirely ordinary mid-stream frame. Nothing else
    // in this class's flush/recovery bookkeeping is necessarily
    // triggered by it: m_flushBeforeNextDecode exists purely for
    // *network*-side recovery (queue overflow / keyframe recovery - see
    // enqueuePacket()), a different situation entirely, and is not set
    // just because the stream's dimensions changed. m_hwTransferFrame's
    // buffer, once allocated, is deliberately reused across frames for
    // performance (avoids a realloc every frame) - but reusing it at the
    // OLD dimensions once the incoming surface is a different size is
    // exactly the kind of thing a driver can fail on silently rather
    // than error cleanly. This check is cheap (integer comparisons) and
    // catches it before that happens; getting this wrong previously
    // showed up as: the mirror freezes on device rotation and stays
    // frozen (every subsequent frame silently dropped) until reconnecting.
    if (m_hwTransferFrame->buf[0] &&
        (m_hwTransferFrame->width != m_recvFrame->width ||
         m_hwTransferFrame->height != m_recvFrame->height)) {
        av_frame_unref(m_hwTransferFrame);
        // Re-probe direct-YUV420P-transfer support at the new resolution
        // too, rather than assuming it's unaffected - cheap (happens
        // only right after a resolution change, not every frame) and
        // avoids relying on an assumption that may not hold on every
        // driver.
        m_hwTransferFormatProbed = false;
    }

    if (!m_hwTransferFormatProbed) {
        // First hardware frame of this session (or since the last
        // resolution change, which unrefs m_hwTransferFrame and clears
        // this flag - see the flush handling in run()). Try asking the
        // transfer to hand back YUV420P directly instead of leaving the
        // format unset (which lets FFmpeg auto-pick, almost always
        // NV12). Several VAAPI/NVDEC driver combinations support a
        // direct YUV420P transfer even though NV12 is the more common
        // native surface layout; when it works, it skips the sws_scale
        // pass below for the rest of the session - one less full-frame
        // pass in the hot path, for free. This is a one-time probe, not
        // a per-frame gamble: if it fails here, that's remembered (by
        // simply leaving the format unset from this point on) so later
        // frames go straight to the auto-negotiated path without
        // wasting time repeating an attempt already known to fail.
        m_hwTransferFrame->format = AV_PIX_FMT_YUV420P;
        if (av_hwframe_transfer_data(m_hwTransferFrame, m_recvFrame, 0) < 0) {
            av_frame_unref(m_hwTransferFrame); // back to blank/format-unset
            if (av_hwframe_transfer_data(m_hwTransferFrame, m_recvFrame, 0) < 0) {
                return nullptr;
            }
        }
        m_hwTransferFormatProbed = true;
        qInfo() << "Decoder: hardware frame transfer format:"
                << (m_hwTransferFrame->format == AV_PIX_FMT_YUV420P
                        ? "YUV420P direct (sws_scale reshuffle skipped every frame)"
                        : "native surface format (one sws_scale reshuffle per frame)");
    } else if (av_hwframe_transfer_data(m_hwTransferFrame, m_recvFrame, 0) < 0) {
        // Reading the surface back can fail transiently (driver hiccup,
        // VRAM pressure). Drop this one frame instead of tearing down
        // the whole decode session - the next frame tries independently.
        return nullptr;
    }

    const auto sourceFormat = static_cast<AVPixelFormat>(m_hwTransferFrame->format);
    if (sourceFormat == AV_PIX_FMT_YUV420P) {
        // Already exactly the layout the rest of the pipeline wants -
        // hand it back directly, no conversion pass at all.
        return m_hwTransferFrame;
    }

    const int width = m_recvFrame->width;
    const int height = m_recvFrame->height;

    const bool needsRealloc = !m_hwSwFrame->buf[0] ||
                               m_hwSwFrame->width != width ||
                               m_hwSwFrame->height != height;
    if (needsRealloc) {
        av_frame_unref(m_hwSwFrame);
        m_hwSwFrame->format = AV_PIX_FMT_YUV420P;
        m_hwSwFrame->width = width;
        m_hwSwFrame->height = height;
        if (av_frame_get_buffer(m_hwSwFrame, 32) < 0) return nullptr;
    } else if (av_frame_make_writable(m_hwSwFrame) < 0) {
        return nullptr;
    }

    // The hw backend's natural transfer format (typically NV12 across
    // VAAPI/D3D11VA/DXVA2/VideoToolbox/NVDEC when the direct-YUV420P
    // probe above didn't pan out) is reshuffled into planar YUV420P here
    // so the rest of the pipeline - which was written for, and already
    // thoroughly exercises, plain software-decoded YUV420P - needs zero
    // changes to support hardware decode. Width/height are identical on
    // both sides, so this is a pure plane-layout conversion, not a
    // rescale - SWS_POINT (nearest-neighbor) rather than SWS_BILINEAR
    // because there is no actual resampling to do here (the chroma
    // planes are already at the correct 4:2:0 subsampling on both sides,
    // just interleaved on one and planar on the other), so bilinear's
    // interpolation math would be pure unneeded overhead. Cost is a
    // single SIMD-optimized libswscale pass, tiny next to the decode
    // work it replaces.
    SwsContext *rawSws = sws_getCachedContext(
        m_hwSwsCtx.release(),
        width, height, sourceFormat,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_POINT, nullptr, nullptr, nullptr);
    m_hwSwsCtx.reset(rawSws);
    if (!m_hwSwsCtx) return nullptr;

    sws_scale(m_hwSwsCtx.get(),
              m_hwTransferFrame->data, m_hwTransferFrame->linesize, 0, height,
              m_hwSwFrame->data, m_hwSwFrame->linesize);

    return m_hwSwFrame;
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
            m_hwTransferFormatProbed = false;
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
        // readable) rather than plain mapped memory. Two ways this gets
        // to the renderer from here: trySubmitZeroCopyFrame() (used
        // first when requested - see the design note above it) hands off
        // the surface directly as GPU-importable DMA-BUF planes, with no
        // CPU copy at all; transferHwFrame() (the fallback, and the only
        // path when zero-copy isn't requested/available) reads it back
        // into system memory - and, on drivers that support it, directly
        // as YUV420P, skipping the sws_scale reshuffle entirely (see
        // there). Either way, everything downstream of this point - the
        // span/stride logic, VideoBuffer, the renderer - stays
        // completely unaware of whether hw accel, let alone zero-copy,
        // is even active; a frame handled by zero-copy just doesn't
        // reach any of that code at all for this iteration (see the
        // `continue` below).
        const AVFrame *presentFrame = m_recvFrame;
        bool dropFrame = false;
        bool handledByZeroCopy = false;
        if (m_hwAccelActive.load(std::memory_order_relaxed) &&
            m_recvFrame->format == static_cast<int>(m_hwPixFmt)) {
            if (m_zeroCopyRequested && trySubmitZeroCopyFrame(m_recvFrame)) {
                handledByZeroCopy = true;
            } else {
                presentFrame = transferHwFrame();
                dropFrame = (presentFrame == nullptr);
            }
        }

        if (handledByZeroCopy) {
            // trySubmitZeroCopyFrame() took its own independent reference
            // (via av_hwframe_map()) for whatever the sink now holds;
            // this av_frame_unref() only ever drops m_recvFrame's own
            // reference, which is unrelated and always correct to drop
            // here regardless.
            av_frame_unref(m_recvFrame);
            continue;
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
