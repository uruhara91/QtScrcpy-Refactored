#include "qyuvopenglwidget.h"
#include "qtscrcpytelemetry.h"

#include <QDebug>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <QtGlobal>
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>

#if __has_include(<QtGui/qopenglcontext_platform.h>)
#include <QtGui/qopenglcontext_platform.h>
#define QSC_HAS_GL_NATIVE_INTERFACE 1
#endif

#if defined(Q_OS_LINUX) && defined(QSC_HAS_GL_NATIVE_INTERFACE)
#define QSC_HAS_ZEROCOPY_SUPPORT 1
// ---------------------------------------------------------------------
// EGL/DRM constants for the zero-copy dma-buf import path
// ---------------------------------------------------------------------
// Values copied from the system's own <EGL/egl.h>/<EGL/eglext.h>/
// <drm/drm_fourcc.h> (verified against them directly, not guessed)
// rather than including those headers here. EGL/eglext.h transitively
// pulls in X11 headers on Linux, which are notorious for #define-ing
// short, common identifiers (Bool, True, False, Status, ...) that
// collide with unrelated code - this file is included, via
// qyuvopenglwidget.h, from videoform.h and therefore most of the rest of
// the UI layer, so that risk isn't worth taking for a handful of
// integer constants and function-pointer types this file already
// declares its own EGL-agnostic equivalents of (see the header).
namespace egl_zerocopy {
constexpr std::int32_t kWidth = 0x3057;                        // EGL_WIDTH
constexpr std::int32_t kHeight = 0x3056;                       // EGL_HEIGHT
constexpr std::int32_t kNone = 0x3038;                         // EGL_NONE
constexpr std::int32_t kExtensions = 0x3055;                   // EGL_EXTENSIONS
constexpr std::int32_t kLinuxDmaBufExt = 0x3270;               // EGL_LINUX_DMA_BUF_EXT (eglCreateImageKHR target)
constexpr std::int32_t kLinuxDrmFourccExt = 0x3271;            // EGL_LINUX_DRM_FOURCC_EXT
constexpr std::int32_t kDmaBufPlane0FdExt = 0x3272;            // EGL_DMA_BUF_PLANE0_FD_EXT
constexpr std::int32_t kDmaBufPlane0OffsetExt = 0x3273;        // EGL_DMA_BUF_PLANE0_OFFSET_EXT
constexpr std::int32_t kDmaBufPlane0PitchExt = 0x3274;         // EGL_DMA_BUF_PLANE0_PITCH_EXT
constexpr std::int32_t kDmaBufPlane0ModifierLoExt = 0x3443;    // EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
constexpr std::int32_t kDmaBufPlane0ModifierHiExt = 0x3444;    // EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT
constexpr quint64 kDrmFormatModInvalid = 0x00FFFFFFFFFFFFFFULL; // DRM_FORMAT_MOD_INVALID
} // namespace egl_zerocopy

// Core EGL 1.0/1.1 entry points, guaranteed to be real exported symbols
// in any libEGL.so (unlike KHR/EXT extension functions, which must be
// resolved dynamically through eglGetProcAddress instead - see
// tryInitZeroCopy()). Hand-declared with matching C linkage/signatures
// rather than including <EGL/egl.h> for the same X11-pollution reason as
// the constants above; QtScrcpyCore/CMakeLists.txt links -lEGL on Linux
// to satisfy these.
extern "C" {
void *eglGetProcAddress(const char *procname);
const char *eglQueryString(void *dpy, std::int32_t name);
}
#endif

// ---------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------
// Two versions of each shader: the "450" pair uses explicit
// layout(binding=N) sampler bindings, which needs GLSL 4.20
// (ARB_shading_language_420pack / GL 4.2) and is only ever used together
// with the GL 4.5 DSA renderer path. The "410" pair drops that qualifier
// (GLSL 4.10, the version GL 4.1 core actually mandates, doesn't have it)
// and relies purely on the glUniform1i calls in initShader() - which are
// already made unconditionally for both paths - to assign texture units.
// Everything else about the two pairs is identical.
static const char *vertShader450 = R"(#version 450 core
layout(location = 0) in vec3 vertexIn;
layout(location = 1) in vec2 textureIn;
out vec2 textureOut;

void main(void) {
    gl_Position = vec4(vertexIn, 1.0);
    textureOut = textureIn;
}
)";

static const char *vertShader410 = R"(#version 410 core
layout(location = 0) in vec3 vertexIn;
layout(location = 1) in vec2 textureIn;
out vec2 textureOut;

void main(void) {
    gl_Position = vec4(vertexIn, 1.0);
    textureOut = textureIn;
}
)";

static const char *fragShader450 = R"(#version 450 core
in vec2 textureOut;
out vec4 FragColor;
layout(binding = 0) uniform sampler2D tex_y;
layout(binding = 1) uniform sampler2D tex_u;
layout(binding = 2) uniform sampler2D tex_v;

const mat3 yuv2rgb = mat3(
    1.164,  1.164,  1.164,
    0.0,   -0.213,  2.112,
    1.793, -0.533,  0.0
);

const vec3 rgbOffset = vec3(0.9729, -0.30148, 1.1334);
void main(void) {
    vec3 yuv;
    yuv.x = texture(tex_y, textureOut).r;
    yuv.y = texture(tex_u, textureOut).r;
    yuv.z = texture(tex_v, textureOut).r;
    FragColor = vec4(yuv2rgb * yuv - rgbOffset, 1.0);
}
)";

static const char *fragShader410 = R"(#version 410 core
in vec2 textureOut;
out vec4 FragColor;
uniform sampler2D tex_y;
uniform sampler2D tex_u;
uniform sampler2D tex_v;

const mat3 yuv2rgb = mat3(
    1.164,  1.164,  1.164,
    0.0,   -0.213,  2.112,
    1.793, -0.533,  0.0
);

const vec3 rgbOffset = vec3(0.9729, -0.30148, 1.1334);
void main(void) {
    vec3 yuv;
    yuv.x = texture(tex_y, textureOut).r;
    yuv.y = texture(tex_u, textureOut).r;
    yuv.z = texture(tex_v, textureOut).r;
    FragColor = vec4(yuv2rgb * yuv - rgbOffset, 1.0);
}
)";

#ifdef QSC_HAS_ZEROCOPY_SUPPORT
// NV12 variants for the experimental zero-copy path (see
// tryInitZeroCopy()): two textures instead of three - luma (R8) and a
// single interleaved-chroma texture (RG8) imported directly from the
// decoded hardware surface's own memory layout via
// glEGLImageTargetTexture2DOES(), rather than three CPU-written planes.
// Reuses the exact same proven YUV -> RGB colour matrix as the shaders
// above; the only actual difference is sourcing U/V from one 2-channel
// texture's .r/.g instead of two separate 1-channel textures. NV12
// stores chroma as U-then-V per sample (as opposed to NV21's V-then-U),
// and DRM's GR88 fourcc (what FFmpeg's VAAPI->DRM_PRIME export reports
// for NV12's chroma plane) preserves that same memory byte order when
// imported as a plain (non-swizzled) GL_RG8 texture - so .r should be U
// and .g should be V here, matching tex_u/tex_v above exactly. Flagged
// here specifically because it's one of the only things in this whole
// path that's genuinely a guess rather than something verified against
// a spec or a real header: if colours come out visibly wrong (a
// green/magenta tint shift is the classic symptom of U/V being swapped)
// on real hardware, swapping .r/.g on the line below is the fix.
static const char *fragShaderNV12_450 = R"(#version 450 core
in vec2 textureOut;
out vec4 FragColor;
layout(binding = 0) uniform sampler2D tex_y;
layout(binding = 1) uniform sampler2D tex_uv;

const mat3 yuv2rgb = mat3(
    1.164,  1.164,  1.164,
    0.0,   -0.213,  2.112,
    1.793, -0.533,  0.0
);

const vec3 rgbOffset = vec3(0.9729, -0.30148, 1.1334);
void main(void) {
    vec3 yuv;
    yuv.x = texture(tex_y, textureOut).r;
    yuv.yz = texture(tex_uv, textureOut).rg; // .r = U/Cb, .g = V/Cr - see note above
    FragColor = vec4(yuv2rgb * yuv - rgbOffset, 1.0);
}
)";

static const char *fragShaderNV12_410 = R"(#version 410 core
in vec2 textureOut;
out vec4 FragColor;
uniform sampler2D tex_y;
uniform sampler2D tex_uv;

const mat3 yuv2rgb = mat3(
    1.164,  1.164,  1.164,
    0.0,   -0.213,  2.112,
    1.793, -0.533,  0.0
);

const vec3 rgbOffset = vec3(0.9729, -0.30148, 1.1334);
void main(void) {
    vec3 yuv;
    yuv.x = texture(tex_y, textureOut).r;
    yuv.yz = texture(tex_uv, textureOut).rg; // .r = U/Cb, .g = V/Cr - see note above
    FragColor = vec4(yuv2rgb * yuv - rgbOffset, 1.0);
}
)";
#endif

QYuvOpenGLWidget::QYuvOpenGLWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    m_telemetryEnabled = qsc::telemetry::enabled();

    // Start from the application-wide default format - main.cpp already
    // requests the right GL version per platform there (4.5 Core on
    // Windows/Linux, 4.1 Core on macOS - see main.cpp for why) and zeroes
    // depth/stencil, since this 2D video widget uses neither. Previously
    // this constructed a brand new QSurfaceFormat from scratch instead of
    // inheriting that, and never zeroed depth/stencil itself either; since
    // setFormat() on a specific widget always wins over the application
    // default, that meant main.cpp's zeroing had no effect on the one GL
    // context the app actually creates. Re-zeroing explicitly here too, on
    // top of inheriting it, so this stays correct even if main.cpp's
    // default format is ever changed independently.
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setDepthBufferSize(0);
    format.setStencilBufferSize(0);
    format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    format.setSwapInterval(0);
    format.setRedBufferSize(8);
    format.setGreenBufferSize(8);
    format.setBlueBufferSize(8);
    format.setAlphaBufferSize(8);
    setFormat(format);

    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);

    connect(this, &QYuvOpenGLWidget::requestUpdateTextures, this,
            [this](int width, int height,
                   int strideY, int strideU, int strideV) {
        if (!m_acceptFrames.load(std::memory_order_acquire)) return;

        if (width < 16 || width > 8192 ||
            height < 16 || height > 8192 ||
            strideY <= 0 || strideU <= 0 || strideV <= 0) {
            m_textureSizeMismatch.store(false, std::memory_order_release);
            return;
        }

        if (!isValid()) {
            m_textureSizeMismatch.store(false, std::memory_order_release);
            return;
        }

        makeCurrent();
        setFrameSize(QSize(width, height));
        const bool pboReady = initPBOs(height, strideY, strideU, strideV);
        if (pboReady) initTextures(width, height);
        doneCurrent();

        m_textureSizeMismatch.store(false, std::memory_order_release);
        if (pboReady) scheduleUpdate();
    }, Qt::QueuedConnection);
}

QYuvOpenGLWidget::~QYuvOpenGLWidget()
{
    m_acceptFrames.store(false, std::memory_order_release);

#ifdef QSC_HAS_ZEROCOPY_SUPPORT
    // Release order doesn't matter between these two (independent
    // frames/references) - just make sure both get released exactly
    // once rather than leaked, regardless of whether zero-copy ever
    // actually produced a frame this session.
    {
        std::function<void()> pendingRelease;
        {
            std::lock_guard<std::mutex> lock(m_hwFrameMutex);
            if (m_pendingHwFrame.has_value()) {
                pendingRelease = std::move(m_pendingHwFrame->release);
                m_pendingHwFrame.reset();
            }
        }
        if (pendingRelease) pendingRelease();
    }
    if (m_activeHwFrameRelease) {
        m_activeHwFrameRelease();
        m_activeHwFrameRelease = nullptr;
    }
#endif

    if (isValid()) {
        makeCurrent();
        deInitTextures();
        deInitPBOs();

#ifdef QSC_HAS_ZEROCOPY_SUPPORT
        if (m_zeroCopyTextures[0] != 0 || m_zeroCopyTextures[1] != 0) {
            if (m_useDsaPath.load(std::memory_order_relaxed)) {
                glDeleteTextures(2, m_zeroCopyTextures.data());
            } else {
                m_gl41.glDeleteTextures(2, m_zeroCopyTextures.data());
            }
            m_zeroCopyTextures = {0, 0};
        }
#endif

        // m_vao/m_vbo were created through whichever function resolver
        // actually initialized successfully (see initializeGL()) - they
        // must be torn down through that same resolver. The *other*
        // resolver's function pointers are unresolved/null whenever the
        // renderer ended up on this path, so calling through it here would
        // crash instead of harmlessly no-op'ing.
        if (m_useDsaPath.load(std::memory_order_relaxed)) {
            if (m_vao) glDeleteVertexArrays(1, &m_vao);
            if (m_vbo) glDeleteBuffers(1, &m_vbo);
        } else {
            if (m_vao) m_gl41.glDeleteVertexArrays(1, &m_vao);
            if (m_vbo) m_gl41.glDeleteBuffers(1, &m_vbo);
        }
        m_vao = 0;
        m_vbo = 0;
        doneCurrent();
    }

    if (m_telemetryEnabled) {
        qInfo() << "[Telemetry][Renderer] mailbox submitted="
                << m_submittedFrames.load(std::memory_order_relaxed)
                << "rendered:"
                << m_renderedFrames.load(std::memory_order_relaxed)
                << "overwritten ready:"
                << m_overwrittenReadyFrames.load(std::memory_order_relaxed)
                << "dropped:"
                << m_droppedFrames.load(std::memory_order_relaxed);
    }
}

QSize QYuvOpenGLWidget::minimumSizeHint() const
{
    return QSize(50, 50);
}

QSize QYuvOpenGLWidget::sizeHint() const
{
    const QSize current = frameSize();
    return current.isValid() ? current : QSize(640, 360);
}

QSize QYuvOpenGLWidget::frameSize() const
{
    return QSize(m_frameWidth.load(std::memory_order_relaxed),
                 m_frameHeight.load(std::memory_order_relaxed));
}

void QYuvOpenGLWidget::setFrameSize(const QSize &frameSize)
{
    const int width = frameSize.width();
    const int height = frameSize.height();
    if (m_frameWidth.load(std::memory_order_relaxed) == width &&
        m_frameHeight.load(std::memory_order_relaxed) == height) {
        return;
    }

    m_frameWidth.store(width, std::memory_order_relaxed);
    m_frameHeight.store(height, std::memory_order_relaxed);
    m_pboSizeValid.store(false, std::memory_order_release);
    updateGeometry();
}

void QYuvOpenGLWidget::scheduleUpdate()
{
    if (!m_acceptFrames.load(std::memory_order_acquire)) return;
    if (m_updatePending.test_and_set(std::memory_order_acq_rel)) return;

    QMetaObject::invokeMethod(this, [this]() {
        if (m_acceptFrames.load(std::memory_order_acquire)) {
            update();
        } else {
            m_updatePending.clear(std::memory_order_release);
        }
    }, Qt::QueuedConnection);
}

void QYuvOpenGLWidget::setFrameData(int width, int height,
                                    std::span<const uint8_t> dataY,
                                    std::span<const uint8_t> dataU,
                                    std::span<const uint8_t> dataV,
                                    int linesizeY, int linesizeU, int linesizeV)
{
    if (!m_acceptFrames.load(std::memory_order_acquire)) return;
    if (width <= 0 || height <= 0 ||
        linesizeY <= 0 || linesizeU <= 0 || linesizeV <= 0) {
        return;
    }

    const int chromaHeight = (height + 1) / 2;
    const std::array<std::size_t, 3> requiredBytes{
        static_cast<std::size_t>(linesizeY) * static_cast<std::size_t>(height),
        static_cast<std::size_t>(linesizeU) * static_cast<std::size_t>(chromaHeight),
        static_cast<std::size_t>(linesizeV) * static_cast<std::size_t>(chromaHeight)
    };

    if (dataY.size() < requiredBytes[0] ||
        dataU.size() < requiredBytes[1] ||
        dataV.size() < requiredBytes[2]) {
        return;
    }

    std::unique_lock<std::mutex> pboLock(m_pboMutex, std::try_to_lock);
    if (!pboLock.owns_lock()) {
        if (m_telemetryEnabled) {
            m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    const bool sizeChanged =
        width != m_frameWidth.load(std::memory_order_relaxed) ||
        height != m_frameHeight.load(std::memory_order_relaxed);
    const bool strideChanged =
        linesizeY != m_pboStrides[0] ||
        linesizeU != m_pboStrides[1] ||
        linesizeV != m_pboStrides[2];
    const bool pboInvalid = !m_pboSizeValid.load(std::memory_order_acquire);

    if (sizeChanged || strideChanged || pboInvalid) {
        pboLock.unlock();
        bool expected = false;
        if (m_textureSizeMismatch.compare_exchange_strong(
                expected, true,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            emit requestUpdateTextures(width, height,
                                       linesizeY, linesizeU, linesizeV);
        }
        return;
    }

    bool reclaimedReady = false;
    const std::optional<std::size_t> targetIndex = m_slots.acquireWritable(&reclaimedReady);
    if (!targetIndex.has_value()) {
        if (m_telemetryEnabled) {
            m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
        }
        scheduleUpdate();
        return;
    }
    if (reclaimedReady && m_telemetryEnabled) {
        m_overwrittenReadyFrames.fetch_add(1, std::memory_order_relaxed);
    }

    FrameBuffer &target = m_frames[*targetIndex];
    const std::array<const uint8_t *, 3> sources{
        dataY.data(), dataU.data(), dataV.data()
    };

    if (m_useDsaPath.load(std::memory_order_acquire)) {
        // Fast path: the destination is a persistently+coherently mapped
        // GPU-visible pointer. No GL call needed to write into it at all -
        // this is the "zero-copy, zero-marshalling" hot path called
        // directly from the decoder thread.
        for (int plane = 0; plane < 3; ++plane) {
            auto *destination = static_cast<uint8_t *>(target.mappedPtrs[plane]);
            if (!destination) {
                m_slots.abandonWrite(*targetIndex);
                if (m_telemetryEnabled) {
                    m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
                }
                return;
            }
            std::memcpy(destination, sources[plane], requiredBytes[plane]);
        }
    } else {
        // Classic fallback path: this thread has no current GL context, so
        // it cannot map/unmap a GL buffer itself. Stage into plain heap
        // memory instead; the render thread copies from here into an
        // orphaned PBO on its own turn in paintGL().
        for (int plane = 0; plane < 3; ++plane) {
            if (target.cpuStaging[plane].size() != requiredBytes[plane]) {
                target.cpuStaging[plane].resize(requiredBytes[plane]);
            }
            std::memcpy(target.cpuStaging[plane].data(), sources[plane], requiredBytes[plane]);
        }
    }

    m_slots.publishReady(*targetIndex);
    if (m_telemetryEnabled) {
        m_submittedFrames.fetch_add(1, std::memory_order_relaxed);
    }
    scheduleUpdate();
}

void QYuvOpenGLWidget::logInitFailureDiagnostics()
{
    // Neither QOpenGLFunctions_4_5_Core nor _4_1_Core resolved, so their
    // function pointers are unresolved/null - deliberately not calling
    // through either of them here. QOpenGLContext::functions() is the one
    // resolver Qt guarantees works on essentially any context (it targets
    // the GL 2.0/GLES 2.0 common subset), which is what makes it safe to
    // use purely for this diagnostic.
    QString version = QStringLiteral("unknown");
    QString renderer = QStringLiteral("unknown");
    QString vendor = QStringLiteral("unknown");

    if (QOpenGLContext *ctx = context()) {
        if (QOpenGLFunctions *baseFuncs = ctx->functions()) {
            baseFuncs->initializeOpenGLFunctions();
            const auto toQString = [](const GLubyte *text) {
                return text ? QString::fromLatin1(reinterpret_cast<const char *>(text))
                            : QStringLiteral("unknown");
            };
            version = toQString(baseFuncs->glGetString(GL_VERSION));
            renderer = toQString(baseFuncs->glGetString(GL_RENDERER));
            vendor = toQString(baseFuncs->glGetString(GL_VENDOR));
        }
    }

    qCritical().noquote()
        << "QYuvOpenGLWidget: no usable OpenGL context (need at least "
           "OpenGL 4.1 Core; this build prefers 4.5 Core for the faster "
           "DSA renderer path). GL_VERSION:" << version
        << "GL_RENDERER:" << renderer << "GL_VENDOR:" << vendor
        << "- video will not render. This usually means the GPU driver is "
           "missing, too old, or a software/remote-desktop/VM fallback "
           "renderer without real GPU acceleration.";
}

void QYuvOpenGLWidget::logGlPlatformBackend()
{
    // Purely informational, no behavior depends on this. It answers one
    // specific question: is this widget's GL context EGL-backed? That is
    // the single fact that determines whether a *true* zero-copy render
    // path (importing a VAAPI-decoded surface as a GL texture directly
    // via VA-API's DMA-BUF export + EGL_EXT_image_dma_buf_import, with no
    // CPU copy at all - as opposed to the current copy-back-then-upload
    // path in Decoder::transferHwFrame()) is even reachable at all. Qt's
    // default "xcb" platform plugin on Linux traditionally hands out a
    // GLX context for a plain windowed QOpenGLWidget, in which case that
    // path isn't available without forcing EGL (QT_XCB_GL_INTEGRATION=
    // xcb_egl) or a different platform integration - both bigger changes
    // than they might sound, since they can affect window-manager/
    // compositor interop in ways this note doesn't decide for you.
#ifdef QSC_HAS_GL_NATIVE_INTERFACE
    QOpenGLContext *ctx = context();
    if (!ctx) return;

    QString backend = QStringLiteral("unknown");
#if QT_CONFIG(egl) || defined(Q_CLANG_QDOC)
    if (ctx->nativeInterface<QNativeInterface::QEGLContext>()) {
        backend = QStringLiteral("EGL");
    }
#endif
#if QT_CONFIG(xcb_glx_plugin) || defined(Q_CLANG_QDOC)
    if (backend == QStringLiteral("unknown") &&
        ctx->nativeInterface<QNativeInterface::QGLXContext>()) {
        backend = QStringLiteral("GLX");
    }
#endif
    qInfo().noquote() << "QYuvOpenGLWidget: GL platform backend:" << backend
                       << (backend == QStringLiteral("GLX")
                               ? "(a DMA-BUF/EGLImage zero-copy path would need "
                                 "an EGL context instead - not reachable as-is)"
                               : "");
#endif
}

#ifdef QSC_HAS_ZEROCOPY_SUPPORT

bool QYuvOpenGLWidget::tryInitZeroCopy()
{
    // Called once, from initializeGL() (always the GL thread, with the
    // context current) rather than lazily from the first submitHwFrame()
    // call - that can arrive on the decoder thread before this widget has
    // painted even once, and everything this function checks (extension
    // strings, function resolution) needs GL/EGL calls that are only
    // valid with a current context.
    if (!qsc::telemetry::environmentFlag("QTSCRCPY_EXPERIMENTAL_ZEROCOPY", false)) {
        m_zeroCopyState.store(ZeroCopyState::Unavailable, std::memory_order_release);
        return false;
    }

    QOpenGLContext *ctx = context();
    auto *eglInterface = ctx ? ctx->nativeInterface<QNativeInterface::QEGLContext>() : nullptr;
    if (!eglInterface) {
        qInfo() << "QYuvOpenGLWidget: zero-copy requested but the GL context "
                   "isn't EGL-backed (see the GL platform backend log above); "
                   "staying on the regular hardware copy-back path";
        m_zeroCopyState.store(ZeroCopyState::Unavailable, std::memory_order_release);
        return false;
    }
    m_eglDisplay = eglInterface->display();
    if (!m_eglDisplay) {
        qWarning() << "QYuvOpenGLWidget: zero-copy unavailable - EGLDisplay is null";
        m_zeroCopyState.store(ZeroCopyState::Unavailable, std::memory_order_release);
        return false;
    }

    const char *eglExtensions = eglQueryString(m_eglDisplay, egl_zerocopy::kExtensions);
    const bool haveDmaBufImport = eglExtensions &&
        QByteArray(eglExtensions).contains("EGL_EXT_image_dma_buf_import");
    if (!haveDmaBufImport) {
        qInfo() << "QYuvOpenGLWidget: zero-copy unavailable - EGL_EXT_image_dma_buf_import "
                   "not in this EGL display's extension string; staying on the "
                   "regular hardware copy-back path";
        m_zeroCopyState.store(ZeroCopyState::Unavailable, std::memory_order_release);
        return false;
    }
    const bool haveModifiers = eglExtensions &&
        QByteArray(eglExtensions).contains("EGL_EXT_image_dma_buf_import_modifiers");
    if (!haveModifiers) {
        // Not a hard blocker - importPendingHwFrameLocked() only sends
        // modifier attributes when this is true, and simply omits them
        // otherwise (implying linear layout). Worth knowing about though:
        // a tiled (non-linear) surface without modifier support importing
        // "successfully" but rendering garbage is exactly the kind of
        // thing this log line exists to help diagnose after the fact.
        qInfo() << "QYuvOpenGLWidget: EGL_EXT_image_dma_buf_import_modifiers "
                   "not available - proceeding without explicit tiling/modifier "
                   "info; if the image imports but renders as garbage/noise, "
                   "this is the most likely reason";
    }

    const bool dsa = m_useDsaPath.load(std::memory_order_relaxed);

    // glGetString(GL_EXTENSIONS) is deprecated on core profiles (which is
    // what this widget always requests - see main.cpp) and, at least on
    // this Intel/Mesa (iris) driver on a core context, doesn't reliably
    // report every supported extension through it - GL_OES_EGL_image
    // specifically was observed missing from it despite actually being
    // supported. glGetStringi() per-index is the spec-correct way to
    // query extensions on a core profile; use that instead.
    bool haveEglImageExt = false;
    {
        GLint numExtensions = 0;
        if (dsa) glGetIntegerv(GL_NUM_EXTENSIONS, &numExtensions);
        else m_gl41.glGetIntegerv(GL_NUM_EXTENSIONS, &numExtensions);
        for (GLint i = 0; i < numExtensions && !haveEglImageExt; ++i) {
            const GLubyte *ext = dsa ? glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i))
                                     : m_gl41.glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
            if (ext && std::strcmp(reinterpret_cast<const char *>(ext), "GL_OES_EGL_image") == 0) {
                haveEglImageExt = true;
            }
        }
        if (!haveEglImageExt) {
            // Fall back to the legacy query too before giving up - belt
            // and braces, in case some other driver combination has the
            // opposite problem (populates the legacy string but not the
            // indexed one).
            const char *glExtensions = reinterpret_cast<const char *>(
                dsa ? glGetString(GL_EXTENSIONS) : m_gl41.glGetString(GL_EXTENSIONS));
            haveEglImageExt = glExtensions &&
                QByteArray(glExtensions).contains("GL_OES_EGL_image");
        }
    }
    if (!haveEglImageExt) {
        qInfo() << "QYuvOpenGLWidget: zero-copy unavailable - GL_OES_EGL_image "
                   "not in this GL context's extension string; staying on the "
                   "regular hardware copy-back path";
        m_zeroCopyState.store(ZeroCopyState::Unavailable, std::memory_order_release);
        return false;
    }

    m_eglCreateImageKHR = reinterpret_cast<PFN_eglCreateImageKHR>(
        eglGetProcAddress("eglCreateImageKHR"));
    m_eglDestroyImageKHR = reinterpret_cast<PFN_eglDestroyImageKHR>(
        eglGetProcAddress("eglDestroyImageKHR"));
    m_glEGLImageTargetTexture2DOES = reinterpret_cast<PFN_glEGLImageTargetTexture2DOES>(
        reinterpret_cast<void *>(ctx->getProcAddress("glEGLImageTargetTexture2DOES")));
    if (!m_eglCreateImageKHR || !m_eglDestroyImageKHR || !m_glEGLImageTargetTexture2DOES) {
        qWarning() << "QYuvOpenGLWidget: zero-copy unavailable - failed to resolve "
                      "one or more required EGL/GL extension functions "
                      "(eglCreateImageKHR:" << (m_eglCreateImageKHR != nullptr)
                   << "eglDestroyImageKHR:" << (m_eglDestroyImageKHR != nullptr)
                   << "glEGLImageTargetTexture2DOES:" << (m_glEGLImageTargetTexture2DOES != nullptr)
                   << "); staying on the regular hardware copy-back path";
        m_zeroCopyState.store(ZeroCopyState::Unavailable, std::memory_order_release);
        return false;
    }

    initZeroCopyShader();
    if (!m_zeroCopyProgram.isLinked()) {
        m_zeroCopyState.store(ZeroCopyState::Unavailable, std::memory_order_release);
        return false;
    }

    if (dsa) {
        glCreateTextures(GL_TEXTURE_2D, 2, m_zeroCopyTextures.data());
    } else {
        m_gl41.glGenTextures(2, m_zeroCopyTextures.data());
    }
    for (GLuint tex : m_zeroCopyTextures) {
        if (tex == 0) {
            qWarning() << "QYuvOpenGLWidget: zero-copy unavailable - texture "
                          "allocation failed";
            m_zeroCopyState.store(ZeroCopyState::Unavailable, std::memory_order_release);
            return false;
        }
        if (dsa) {
            glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            m_gl41.glBindTexture(GL_TEXTURE_2D, tex);
            m_gl41.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            m_gl41.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            m_gl41.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            m_gl41.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }
    if (!dsa) m_gl41.glBindTexture(GL_TEXTURE_2D, 0);

    m_zeroCopyState.store(ZeroCopyState::Available, std::memory_order_release);
    qInfo() << "QYuvOpenGLWidget: zero-copy hardware frame import is available "
               "(EGL_EXT_image_dma_buf_import + GL_OES_EGL_image confirmed) and "
               "will be used for hardware-decoded frames";
    return true;
}

void QYuvOpenGLWidget::initZeroCopyShader()
{
    const bool dsa = m_useDsaPath.load(std::memory_order_relaxed);
    const char *vertSource = dsa ? vertShader450 : vertShader410;
    const char *fragSource = dsa ? fragShaderNV12_450 : fragShaderNV12_410;

    if (!m_zeroCopyProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, vertSource) ||
        !m_zeroCopyProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, fragSource) ||
        !m_zeroCopyProgram.link()) {
        qCritical() << "Failed to initialize zero-copy NV12 shader:"
                    << m_zeroCopyProgram.log();
        return;
    }

    m_zeroCopyProgram.bind();
    m_zeroCopyProgram.setUniformValue("tex_y", 0);
    m_zeroCopyProgram.setUniformValue("tex_uv", 1);
    m_zeroCopyProgram.release();
}

bool QYuvOpenGLWidget::submitHwFrame(int width, int height,
                                     const DmaBufPlane *planes, int planeCount,
                                     std::function<void()> release)
{
    if (!m_acceptFrames.load(std::memory_order_acquire) ||
        m_zeroCopyState.load(std::memory_order_acquire) != ZeroCopyState::Available ||
        width <= 0 || height <= 0 || !planes || planeCount <= 0 || planeCount > 2) {
        release();
        return false;
    }

    PendingHwFrame pending;
    pending.width = width;
    pending.height = height;
    pending.planeCount = planeCount;
    for (int i = 0; i < planeCount; ++i) pending.planes[static_cast<std::size_t>(i)] = planes[i];
    pending.release = std::move(release);

    std::function<void()> supersededRelease;
    {
        std::lock_guard<std::mutex> lock(m_hwFrameMutex);
        if (m_pendingHwFrame.has_value()) {
            // A previous frame arrived and paintGL() hasn't run since to
            // import it - release it now rather than leaking it / holding
            // the decoder's surface pool hostage for a frame that's about
            // to be superseded and was never going to be shown anyway.
            supersededRelease = std::move(m_pendingHwFrame->release);
        }
        m_pendingHwFrame.emplace(std::move(pending));
    }
    if (supersededRelease) supersededRelease();

    scheduleUpdate();
    return true;
}

void QYuvOpenGLWidget::importPendingHwFrameLocked()
{
    std::optional<PendingHwFrame> pending;
    {
        std::lock_guard<std::mutex> lock(m_hwFrameMutex);
        if (!m_pendingHwFrame.has_value()) return;
        pending = std::move(m_pendingHwFrame);
        m_pendingHwFrame.reset();
    }

    const bool dsa = m_useDsaPath.load(std::memory_order_relaxed);
    bool importOk = true;

    for (int i = 0; i < pending->planeCount && i < 2; ++i) {
        const DmaBufPlane &plane = pending->planes[static_cast<std::size_t>(i)];

        std::int32_t attribs[19];
        int n = 0;
        attribs[n++] = egl_zerocopy::kWidth;
        attribs[n++] = plane.width;
        attribs[n++] = egl_zerocopy::kHeight;
        attribs[n++] = plane.height;
        attribs[n++] = egl_zerocopy::kLinuxDrmFourccExt;
        attribs[n++] = static_cast<std::int32_t>(plane.fourcc);
        attribs[n++] = egl_zerocopy::kDmaBufPlane0FdExt;
        attribs[n++] = plane.fd;
        attribs[n++] = egl_zerocopy::kDmaBufPlane0OffsetExt;
        attribs[n++] = static_cast<std::int32_t>(plane.offset);
        attribs[n++] = egl_zerocopy::kDmaBufPlane0PitchExt;
        attribs[n++] = static_cast<std::int32_t>(plane.pitch);
        if (plane.modifier != 0 && plane.modifier != egl_zerocopy::kDrmFormatModInvalid) {
            attribs[n++] = egl_zerocopy::kDmaBufPlane0ModifierLoExt;
            attribs[n++] = static_cast<std::int32_t>(plane.modifier & 0xFFFFFFFFu);
            attribs[n++] = egl_zerocopy::kDmaBufPlane0ModifierHiExt;
            attribs[n++] = static_cast<std::int32_t>(plane.modifier >> 32);
        }
        attribs[n++] = egl_zerocopy::kNone;
        Q_ASSERT(n <= static_cast<int>(std::size(attribs)));

        void *image = m_eglCreateImageKHR(m_eglDisplay, nullptr /*EGL_NO_CONTEXT*/,
                                          static_cast<unsigned int>(egl_zerocopy::kLinuxDmaBufExt),
                                          nullptr, attribs);
        if (!image) {
            qWarning() << "QYuvOpenGLWidget: eglCreateImageKHR failed for plane" << i
                      << "(fourcc=" << Qt::hex << plane.fourcc << Qt::dec
                      << "modifier=" << plane.modifier << ") - dropping this frame";
            importOk = false;
            break;
        }

        if (dsa) {
            glBindTextureUnit(static_cast<GLuint>(i), m_zeroCopyTextures[static_cast<std::size_t>(i)]);
            glBindTexture(GL_TEXTURE_2D, m_zeroCopyTextures[static_cast<std::size_t>(i)]);
        } else {
            m_gl41.glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + i));
            m_gl41.glBindTexture(GL_TEXTURE_2D, m_zeroCopyTextures[static_cast<std::size_t>(i)]);
        }
        m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

        // Per the EGL_KHR_image_base spec, once glEGLImageTargetTexture2DOES()
        // has consumed it, the texture retains the image data and the
        // EGLImage handle itself is no longer needed - safe to destroy
        // immediately. What must still stay valid for as long as the
        // texture is actually used is the underlying dma-buf/hardware
        // surface itself, which is exactly what pending->release (kept in
        // m_activeHwFrameRelease below, called only once this frame is
        // itself superseded) is responsible for.
        m_eglDestroyImageKHR(m_eglDisplay, image);
    }
    if (!dsa) m_gl41.glBindTexture(GL_TEXTURE_2D, 0);

    if (!importOk) {
        pending->release();
        return;
    }

    // This frame is now the one backing m_zeroCopyTextures - release
    // whatever the *previous* one was (safe to reclaim its hardware
    // surface now that nothing references it for drawing anymore), and
    // hang on to this one's release callback until it's superseded in
    // turn (or the widget is torn down - see the destructor).
    if (m_activeHwFrameRelease) m_activeHwFrameRelease();
    m_activeHwFrameRelease = std::move(pending->release);
    m_zeroCopyFrameWidth = pending->width;
    m_zeroCopyFrameHeight = pending->height;
    m_zeroCopyFrameReady = true;
    setFrameSize(QSize(pending->width, pending->height));
}

void QYuvOpenGLWidget::drawZeroCopyFrame(bool dsa)
{
    if (!m_zeroCopyProgram.isLinked()) return;

    m_zeroCopyProgram.bind();
    if (dsa) {
        glBindVertexArray(m_vao);
        glBindTextureUnit(0, m_zeroCopyTextures[0]);
        glBindTextureUnit(1, m_zeroCopyTextures[1]);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
    } else {
        m_gl41.glBindVertexArray(m_vao);
        m_gl41.glActiveTexture(GL_TEXTURE0);
        m_gl41.glBindTexture(GL_TEXTURE_2D, m_zeroCopyTextures[0]);
        m_gl41.glActiveTexture(GL_TEXTURE1);
        m_gl41.glBindTexture(GL_TEXTURE_2D, m_zeroCopyTextures[1]);
        m_gl41.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_gl41.glBindVertexArray(0);
    }
    m_zeroCopyProgram.release();

    if (m_telemetryEnabled) {
        m_renderedFrames.fetch_add(1, std::memory_order_relaxed);
    }
}

#endif // QSC_HAS_ZEROCOPY_SUPPORT

#ifndef QSC_HAS_ZEROCOPY_SUPPORT
bool QYuvOpenGLWidget::submitHwFrame(int, int, const DmaBufPlane *, int,
                                     std::function<void()> release)
{
    // Zero-copy needs Qt's EGL native interface (QNativeInterface::QEGLContext,
    // see qopenglcontext_platform.h) which isn't available in this build
    // configuration - most commonly because this isn't Linux at all
    // (Windows/macOS don't need or use this path; their hardware decode
    // already works via the copy-back path in Decoder::transferHwFrame()).
    // Always declining is the correct, safe behavior here: the caller
    // (Decoder::trySubmitZeroCopyFrame()) falls back to the regular
    // hardware copy-back path for every frame in that case.
    release();
    return false;
}
#endif

void QYuvOpenGLWidget::initializeGL()
{
    const bool dsaAvailable = initializeOpenGLFunctions();
    m_useDsaPath.store(dsaAvailable, std::memory_order_release);
    logGlPlatformBackend();

    if (!dsaAvailable) {
        // Most notably macOS: Apple has capped OpenGL at 4.1 Core since
        // 10.14 (OpenGL itself is deprecated there in favor of Metal) and
        // never implemented ARB_direct_state_access / ARB_buffer_storage,
        // so QOpenGLFunctions_4_5_Core::initializeOpenGLFunctions() above
        // refuses to resolve *any* function - not just the DSA-specific
        // ones - because it requires the context to actually be >= 4.5
        // core. Fall back to the classic (non-DSA) GL 4.1 renderer instead
        // of leaving the widget calling unresolved (null) function
        // pointers, which is what silently produced the black/blank
        // screen this replaces.
        if (!m_gl41.initializeOpenGLFunctions()) {
            logInitFailureDiagnostics();
            return; // m_isInitialized stays false; paintGL() becomes a no-op.
        }
        qWarning().noquote()
            << "QYuvOpenGLWidget: OpenGL 4.5 Core / ARB_direct_state_access "
               "not available on this system - using the classic (non-DSA) "
               "GL 4.1 renderer path instead. Playback still works, with "
               "one extra CPU-side copy per frame instead of the "
               "zero-copy persistent-mapped-PBO fast path.";
    }

    m_isInitialized = true;

    if (dsaAvailable) {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_DITHER);
    } else {
        m_gl41.glDisable(GL_DEPTH_TEST);
        m_gl41.glDepthMask(GL_FALSE);
        m_gl41.glDisable(GL_STENCIL_TEST);
        m_gl41.glDisable(GL_BLEND);
        m_gl41.glDisable(GL_DITHER);
    }

    initShader();

    static const float coordinates[] = {
        -1.0f, -1.0f, 0.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 0.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 0.0f, 1.0f, 0.0f
    };

    if (dsaAvailable) {
        glCreateVertexArrays(1, &m_vao);
        glCreateBuffers(1, &m_vbo);
        glNamedBufferStorage(m_vbo, sizeof(coordinates), coordinates, 0);

        glVertexArrayVertexBuffer(m_vao, 0, m_vbo, 0, 5 * sizeof(float));
        glEnableVertexArrayAttrib(m_vao, 0);
        glVertexArrayAttribFormat(m_vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
        glVertexArrayAttribBinding(m_vao, 0, 0);
        glEnableVertexArrayAttrib(m_vao, 1);
        glVertexArrayAttribFormat(m_vao, 1, 2, GL_FLOAT, GL_FALSE, 3 * sizeof(float));
        glVertexArrayAttribBinding(m_vao, 1, 0);

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    } else {
        // No glCreateVertexArrays/glVertexArrayAttribFormat DSA sugar
        // below GL 4.5 - this is the classic bind-then-edit sequence
        // instead (core since GL 3.0/GL 2.0 respectively).
        m_gl41.glGenVertexArrays(1, &m_vao);
        m_gl41.glGenBuffers(1, &m_vbo);

        m_gl41.glBindVertexArray(m_vao);
        m_gl41.glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        m_gl41.glBufferData(GL_ARRAY_BUFFER, sizeof(coordinates), coordinates, GL_STATIC_DRAW);

        m_gl41.glEnableVertexAttribArray(0);
        m_gl41.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                                      5 * sizeof(float),
                                      reinterpret_cast<const void *>(0));
        m_gl41.glEnableVertexAttribArray(1);
        m_gl41.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                                      5 * sizeof(float),
                                      reinterpret_cast<const void *>(3 * sizeof(float)));

        m_gl41.glBindVertexArray(0);
        m_gl41.glBindBuffer(GL_ARRAY_BUFFER, 0);

        m_gl41.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    }

#ifdef QSC_HAS_ZEROCOPY_SUPPORT
    tryInitZeroCopy();
#endif
}

void QYuvOpenGLWidget::initShader()
{
    const bool dsa = m_useDsaPath.load(std::memory_order_relaxed);
    const char *vertSource = dsa ? vertShader450 : vertShader410;
    const char *fragSource = dsa ? fragShader450 : fragShader410;

    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertSource) ||
        !m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragSource) ||
        !m_program.link()) {
        qCritical() << "Failed to initialize YUV shader:" << m_program.log();
        return;
    }

    m_program.bind();
    // Assigns texture units explicitly via glUniform1i. Redundant with the
    // fragment shader's own layout(binding=N) on the DSA (450) path, but
    // it's what actually binds the samplers at all on the fallback (410)
    // path, which has no layout(binding=N) support (GLSL 4.10).
    m_program.setUniformValue("tex_y", 0);
    m_program.setUniformValue("tex_u", 1);
    m_program.setUniformValue("tex_v", 2);
    m_program.release();
}

void QYuvOpenGLWidget::initTextures(int width, int height)
{
    if (!m_isInitialized || width <= 0 || height <= 0) return;

    deInitTextures();

    const std::array<int, 3> widths{
        width, (width + 1) / 2, (width + 1) / 2
    };
    const std::array<int, 3> heights{
        height, (height + 1) / 2, (height + 1) / 2
    };

    if (m_useDsaPath.load(std::memory_order_relaxed)) {
        glCreateTextures(GL_TEXTURE_2D, 3, m_textures.data());
        for (int plane = 0; plane < 3; ++plane) {
            glTextureParameteri(m_textures[plane], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri(m_textures[plane], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(m_textures[plane], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(m_textures[plane], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTextureStorage2D(m_textures[plane], 1, GL_R8,
                               widths[plane], heights[plane]);
        }
    } else {
        m_gl41.glGenTextures(3, m_textures.data());
        for (int plane = 0; plane < 3; ++plane) {
            m_gl41.glBindTexture(GL_TEXTURE_2D, m_textures[plane]);
            m_gl41.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            m_gl41.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            m_gl41.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            m_gl41.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            // glTexStorage2D (immutable storage) needs GL 4.2
            // (ARB_texture_storage) - not core in 4.1, so this uses a
            // classic mutable allocation instead. It's filled once here
            // and then overwritten in place by glTexSubImage2D every
            // frame in paintGL(), so mutability is never actually
            // exercised beyond this initial allocation.
            m_gl41.glTexImage2D(GL_TEXTURE_2D, 0, GL_R8,
                                 widths[plane], heights[plane], 0,
                                 GL_RED, GL_UNSIGNED_BYTE, nullptr);
        }
        m_gl41.glBindTexture(GL_TEXTURE_2D, 0);
    }
}

bool QYuvOpenGLWidget::initPBOs(int height,
                                int strideY, int strideU, int strideV)
{
    std::lock_guard<std::mutex> pboLock(m_pboMutex);
    deInitPBOsUnlocked();

    if (!m_isInitialized || height <= 0 ||
        strideY <= 0 || strideU <= 0 || strideV <= 0) {
        return false;
    }

    const int chromaHeight = (height + 1) / 2;
    const std::array<GLsizeiptr, 3> sizes{
        static_cast<GLsizeiptr>(strideY) * height,
        static_cast<GLsizeiptr>(strideU) * chromaHeight,
        static_cast<GLsizeiptr>(strideV) * chromaHeight
    };

    if (std::ranges::any_of(sizes, [](GLsizeiptr size) {
            return size <= 0;
        })) {
        return false;
    }

    if (m_useDsaPath.load(std::memory_order_relaxed)) {
        constexpr GLbitfield flags =
            GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

        for (FrameBuffer &frame : m_frames) {
            glCreateBuffers(3, frame.pboIds.data());
            for (int plane = 0; plane < 3; ++plane) {
                if (!frame.pboIds[plane]) {
                    deInitPBOsUnlocked();
                    return false;
                }

                glNamedBufferStorage(frame.pboIds[plane], sizes[plane], nullptr, flags);
                frame.mappedPtrs[plane] = glMapNamedBufferRange(
                    frame.pboIds[plane], 0, sizes[plane], flags);
                if (!frame.mappedPtrs[plane]) {
                    deInitPBOsUnlocked();
                    return false;
                }
            }
        }
    } else {
        for (FrameBuffer &frame : m_frames) {
            m_gl41.glGenBuffers(3, frame.pboIds.data());
            for (int plane = 0; plane < 3; ++plane) {
                if (!frame.pboIds[plane]) {
                    deInitPBOsUnlocked();
                    return false;
                }

                m_gl41.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, frame.pboIds[plane]);
                // Initial allocation only; paintGL() re-orphans this with
                // a fresh glBufferData() call every frame (see there for
                // why that needs no fence/wait, unlike the DSA path).
                m_gl41.glBufferData(GL_PIXEL_UNPACK_BUFFER, sizes[plane], nullptr,
                                     GL_STREAM_DRAW);
                frame.cpuStaging[plane].assign(static_cast<std::size_t>(sizes[plane]), uint8_t{0});
            }
        }
        m_gl41.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }

    m_slots.reset();
    m_pboStrides = {strideY, strideU, strideV};
    m_pboSizeValid.store(true, std::memory_order_release);
    return true;
}

void QYuvOpenGLWidget::deInitTextures()
{
    if (!m_isInitialized) return;
    if (m_textures[0] == 0) return;

    if (m_useDsaPath.load(std::memory_order_relaxed)) {
        glDeleteTextures(3, m_textures.data());
    } else {
        m_gl41.glDeleteTextures(3, m_textures.data());
    }
    std::ranges::fill(m_textures, 0);
}

void QYuvOpenGLWidget::deInitPBOs()
{
    std::lock_guard<std::mutex> pboLock(m_pboMutex);
    deInitPBOsUnlocked();
}

void QYuvOpenGLWidget::deInitPBOsUnlocked()
{
    m_pboSizeValid.store(false, std::memory_order_release);

    if (m_useDsaPath.load(std::memory_order_relaxed)) {
        // A resolution change (e.g. device rotation) can arrive while a
        // FrameBuffer is still mid-Processing - the GPU may still be
        // reading its PBOs asynchronously via glTextureSubImage2D, with
        // the corresponding fence not yet signaled. For a persistently
        // mapped buffer (GL_MAP_PERSISTENT_BIT), the usual "the driver
        // keeps the store alive until the GPU is actually done with it"
        // deferred-deletion guarantee is not something the
        // ARB_buffer_storage spec strictly promises the way it does for
        // ordinary buffers - synchronization becomes the application's
        // responsibility once you have a persistent pointer. So: wait out
        // every outstanding fence here before unmapping/deleting anything.
        // In practice this path is only reached on a resolution change,
        // which is rare, so a brief stall here is a non-issue.
        for (FrameBuffer &frame : m_frames) {
            if (!frame.fence) continue;

            constexpr GLuint64 kFenceWaitTimeoutNs = 2'000'000'000ULL; // 2s safety bound
            const GLenum waitResult = glClientWaitSync(
                frame.fence, GL_SYNC_FLUSH_COMMANDS_BIT, kFenceWaitTimeoutNs);
            if (waitResult == GL_TIMEOUT_EXPIRED || waitResult == GL_WAIT_FAILED) {
                // Something is very wrong (driver stall/lost context) -
                // fall back to a hard, unconditional sync point rather
                // than risk deleting a buffer the GPU might still touch.
                qWarning("QYuvOpenGLWidget: PBO teardown fence wait did not "
                         "complete cleanly, forcing glFinish()");
                glFinish();
            }
            glDeleteSync(frame.fence);
            frame.fence = nullptr;
        }

        for (FrameBuffer &frame : m_frames) {
            for (int plane = 0; plane < 3; ++plane) {
                if (frame.pboIds[plane] != 0) {
                    if (frame.mappedPtrs[plane]) {
                        glUnmapNamedBuffer(frame.pboIds[plane]);
                    }
                    glDeleteBuffers(1, &frame.pboIds[plane]);
                }
                frame.pboIds[plane] = 0;
                frame.mappedPtrs[plane] = nullptr;
            }
        }
    } else {
        // No persistent mapping on this path (see paintGL()'s per-frame
        // orphan-map-memcpy-unmap sequence), so there is no equivalent
        // synchronization hazard here: glDeleteBuffers on a buffer the GPU
        // may still be reading is well-defined and universally supported
        // (the driver keeps the underlying store alive until the GPU is
        // done, exactly like deleting any other in-use GL object).
        for (FrameBuffer &frame : m_frames) {
            frame.fence = nullptr; // never used on this path
            for (int plane = 0; plane < 3; ++plane) {
                if (frame.pboIds[plane] != 0) {
                    m_gl41.glDeleteBuffers(1, &frame.pboIds[plane]);
                }
                frame.pboIds[plane] = 0;
                frame.cpuStaging[plane].clear();
                frame.cpuStaging[plane].shrink_to_fit();
            }
        }
    }

    m_pboStrides = {0, 0, 0};
    m_slots.reset();
}

void QYuvOpenGLWidget::checkFences()
{
    // The classic fallback path never creates fences (see paintGL()) - it
    // has nothing to reclaim here, slots are freed synchronously instead.
    if (!m_useDsaPath.load(std::memory_order_relaxed)) return;

    for (std::size_t index = 0; index < m_frames.size(); ++index) {
        if (m_slots.stateOf(index) != SlotState::Processing) continue;

        FrameBuffer &frame = m_frames[index];
        if (!frame.fence) {
            m_slots.releaseProcessing(index);
            continue;
        }

        const GLenum result = glClientWaitSync(
            frame.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 0);

        if (result == GL_ALREADY_SIGNALED ||
            result == GL_CONDITION_SATISFIED) {
            glDeleteSync(frame.fence);
            frame.fence = nullptr;
            m_slots.releaseProcessing(index);
        } else if (result == GL_WAIT_FAILED) {
            glFinish();
            glDeleteSync(frame.fence);
            frame.fence = nullptr;
            m_slots.releaseProcessing(index);
        }
    }
}

void QYuvOpenGLWidget::paintGL()
{
    m_updatePending.clear(std::memory_order_release);

    if (!m_isInitialized) return;

    const bool dsa = m_useDsaPath.load(std::memory_order_relaxed);

#ifdef QSC_HAS_ZEROCOPY_SUPPORT
    if (m_zeroCopyState.load(std::memory_order_acquire) == ZeroCopyState::Available) {
        importPendingHwFrameLocked();
        if (m_zeroCopyFrameReady) {
            drawZeroCopyFrame(dsa);
            return;
        }
    }
#endif

    if (!m_pboSizeValid.load(std::memory_order_acquire)) {
        if (dsa) glClear(GL_COLOR_BUFFER_BIT);
        else m_gl41.glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    checkFences();

    const std::optional<std::size_t> selected = m_slots.acquireNewestReady();
    if (selected.has_value()) {
        const std::size_t selectedIndex = *selected;
        m_slots.releaseStaleReady(selectedIndex);

        FrameBuffer &frame = m_frames[selectedIndex];
        const int width = m_frameWidth.load(std::memory_order_relaxed);
        const int height = m_frameHeight.load(std::memory_order_relaxed);
        const std::array<int, 3> widths{
            width, (width + 1) / 2, (width + 1) / 2
        };
        const std::array<int, 3> heights{
            height, (height + 1) / 2, (height + 1) / 2
        };

        if (dsa) {
            glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

            for (int plane = 0; plane < 3; ++plane) {
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, frame.pboIds[plane]);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, m_pboStrides[plane]);
                glTextureSubImage2D(
                    m_textures[plane], 0, 0, 0,
                    widths[plane], heights[plane],
                    GL_RED, GL_UNSIGNED_BYTE, nullptr);
            }

            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

            frame.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            if (!frame.fence) {
                glFinish();
                m_slots.releaseProcessing(selectedIndex);
            }
            // else: stays Processing: checkFences() reclaims it once the
            // fence signals on a later paintGL() call.
        } else {
            m_gl41.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

            for (int plane = 0; plane < 3; ++plane) {
                const auto size = static_cast<GLsizeiptr>(frame.cpuStaging[plane].size());

                m_gl41.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, frame.pboIds[plane]);
                // Orphan: detach whatever storage this buffer object
                // currently has (the GPU may still be reading it from the
                // previous time this slot was uploaded) and allocate a
                // fresh one under the same name. This is the classic
                // streaming-PBO technique and the reason this path needs
                // no fence/wait at all, unlike the DSA persistent-mapping
                // path above.
                m_gl41.glBufferData(GL_PIXEL_UNPACK_BUFFER, size, nullptr, GL_STREAM_DRAW);
                if (void *mapped = m_gl41.glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY)) {
                    std::memcpy(mapped, frame.cpuStaging[plane].data(),
                                static_cast<std::size_t>(size));
                    m_gl41.glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
                }

                m_gl41.glPixelStorei(GL_UNPACK_ROW_LENGTH, m_pboStrides[plane]);
                m_gl41.glBindTexture(GL_TEXTURE_2D, m_textures[plane]);
                m_gl41.glTexSubImage2D(
                    GL_TEXTURE_2D, 0, 0, 0,
                    widths[plane], heights[plane],
                    GL_RED, GL_UNSIGNED_BYTE, nullptr);
            }

            m_gl41.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            m_gl41.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            m_gl41.glBindTexture(GL_TEXTURE_2D, 0);

            m_slots.releaseProcessing(selectedIndex);
        }

        if (m_telemetryEnabled) {
            m_renderedFrames.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (m_program.isLinked()) {
        m_program.bind();
        if (dsa) {
            glBindVertexArray(m_vao);
            for (int plane = 0; plane < 3; ++plane) {
                glBindTextureUnit(plane, m_textures[plane]);
            }
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindVertexArray(0);
        } else {
            m_gl41.glBindVertexArray(m_vao);
            for (int plane = 0; plane < 3; ++plane) {
                m_gl41.glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + plane));
                m_gl41.glBindTexture(GL_TEXTURE_2D, m_textures[plane]);
            }
            m_gl41.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            m_gl41.glBindVertexArray(0);
        }
        m_program.release();
    }

    if (m_slots.anyReady()) {
        scheduleUpdate();
    }
}

void QYuvOpenGLWidget::resizeGL(int width, int height)
{
    if (!m_isInitialized) return;
    if (m_useDsaPath.load(std::memory_order_relaxed)) {
        glViewport(0, 0, width, height);
    } else {
        m_gl41.glViewport(0, 0, width, height);
    }
}
