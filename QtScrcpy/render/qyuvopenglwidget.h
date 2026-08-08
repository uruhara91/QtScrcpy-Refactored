#ifndef QYUVOPENGLWIDGET_H
#define QYUVOPENGLWIDGET_H

#include <QOpenGLFunctions_4_1_Core>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "pbomailboxstatemachine.h"

// Describes one importable plane of a hardware-decoded frame as a Linux
// DMA-BUF - see QYuvOpenGLWidget::submitHwFrame(). Field-for-field mirror
// of qsc::HwFramePlane / Decoder's internal DrmFramePlane; kept as its
// own type here for the same reason those two are separate from each
// other - this widget has zero dependency on QtScrcpyCore today (its
// existing setFrameData() below takes plain ints/std::span, not qsc::
// types either), and VideoForm is already the adapter that translates
// between the two worlds.
struct DmaBufPlane {
    int fd = -1;
    uint32_t fourcc = 0;
    uint64_t modifier = 0;
    int64_t offset = 0;
    int64_t pitch = 0;
    int width = 0;
    int height = 0;
};

// ---------------------------------------------------------------------
// Renderer capability paths
// ---------------------------------------------------------------------
// The fast path needs OpenGL 4.5 Core + ARB_direct_state_access +
// ARB_buffer_storage (glCreateBuffers/glCreateTextures/glTextureStorage2D,
// persistent+coherent-mapped triple-buffered PBOs the decoder thread
// writes into directly with no GL call at all). That combination is
// available on every desktop Windows/Linux driver worth supporting, but
// NOT on macOS: Apple deprecated OpenGL at 10.14 and never shipped a
// context above 4.1 Core, so DSA/persistent-mapping are simply not there.
//
// initializeGL() detects which one the actual context supports at
// runtime (QOpenGLFunctions_4_5_Core::initializeOpenGLFunctions() itself
// refuses to resolve on anything below 4.5 core - it doesn't partially
// succeed) and picks the corresponding path below. On a context that
// can't even do 4.1 Core, initializeGL() logs full GL_VERSION/RENDERER/
// VENDOR diagnostics and leaves the widget uninitialized; paintGL() then
// just clears to black instead of crashing or reading uninitialized
// function pointers.
class QYuvOpenGLWidget : public QOpenGLWidget, protected QOpenGLFunctions_4_5_Core
{
    Q_OBJECT
public:
    explicit QYuvOpenGLWidget(QWidget *parent = nullptr);
    ~QYuvOpenGLWidget() override;

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

    void setFrameData(int width, int height,
                      std::span<const uint8_t> dataY,
                      std::span<const uint8_t> dataU,
                      std::span<const uint8_t> dataV,
                      int linesizeY, int linesizeU, int linesizeV);

    QSize frameSize() const;

    // Experimental zero-copy hardware-frame import (VAAPI/Linux, EGL-
    // backed contexts only - see logGlPlatformBackend()). Safe to call
    // from any thread (in practice, always the decoder thread). Feasibility
    // (EGL context confirmed, required extensions present) is checked
    // once, lazily, on the first call; if it isn't viable at all, this
    // returns false immediately (having already invoked `release`) and
    // the caller should fall back to setFrameData() for this and every
    // later frame this session - matching Decoder::trySubmitZeroCopyFrame()'s
    // contract on the other side of this same call. Otherwise returns
    // true right away and does the actual GL/EGL import later, on the GL
    // thread, inside paintGL() - `release` is invoked from there once
    // this frame's dma-buf fds are no longer needed (imported into a
    // texture, and - since the texture must stay valid for repeated
    // redraws until a newer frame replaces it - not necessarily before
    // this call returns, possibly not for several more repaints).
    bool submitHwFrame(int width, int height,
                       const DmaBufPlane *planes, int planeCount,
                       std::function<void()> release);

    // True once isRendererInitialized() and, sometime this session, an
    // EGL-backed context, the necessary EGL/GL extensions, and at least
    // one successfully-imported zero-copy frame have all been confirmed.
    // Purely informational (matches hwAccelActive()/zeroCopyActive() on
    // the Decoder side).
    bool zeroCopyActive() const { return m_zeroCopyFrameReady; }

    // True once initializeGL() has run and picked a working renderer path
    // (either DSA or the classic GL 4.1 fallback). False if neither GL 4.5
    // nor GL 4.1 Core turned out to be available at all - see the
    // qCritical() diagnostic logged by initializeGL() in that case.
    bool isRendererInitialized() const { return m_isInitialized; }
    // True on the fast DSA + persistent-mapped-PBO path (GL >= 4.5), false
    // on the classic bind-to-edit + orphaned-PBO fallback path (GL 4.1,
    // e.g. macOS) or if initialization failed entirely.
    bool usesDsaPath() const { return m_useDsaPath.load(std::memory_order_acquire); }

signals:
    void requestUpdateTextures(int width, int height,
                               int strideY, int strideU, int strideV);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;

private:
    static constexpr int PBO_COUNT = 3;
    using Slots = PboMailboxStateMachine<PBO_COUNT>;
    using SlotState = Slots::SlotState;

    struct alignas(64) FrameBuffer {
        // --- DSA path (GL >= 4.5) ---
        std::array<GLuint, 3> pboIds{0, 0, 0};
        std::array<void *, 3> mappedPtrs{nullptr, nullptr, nullptr};
        GLsync fence = nullptr;

        // --- Classic fallback path (GL 4.1) ---
        // The decoder thread cannot safely map/unmap a GL buffer itself
        // (that needs a current GL context, and this thread has none), so
        // it stages into plain heap memory here; the render thread does
        // the orphan-map-memcpy-unmap dance into the PBO on its own turn
        // in paintGL(). See setFrameData()/paintGL() for the two paths.
        std::array<std::vector<uint8_t>, 3> cpuStaging;
    };

    void initShader();
    void initTextures(int width, int height);
    void deInitTextures();
    bool initPBOs(int height, int strideY, int strideU, int strideV);
    void deInitPBOs();
    void deInitPBOsUnlocked();
    void setFrameSize(const QSize &frameSize);
    void checkFences();
    void logInitFailureDiagnostics();
    void logGlPlatformBackend();
    void scheduleUpdate();

    // --- Experimental zero-copy (VAAPI/Linux, EGL-backed contexts only)
    // All GL/EGL work happens here, exclusively on the GL thread (called
    // only from initializeGL()/paintGL()) - submitHwFrame() above never
    // touches GL/EGL itself, only the thread-safe m_pendingHwFrame
    // handoff, specifically to avoid needing a second, shared GL context
    // for the decoder thread to import into.
    //
    // Resolves eglCreateImageKHR/eglDestroyImageKHR/
    // glEGLImageTargetTexture2DOES and confirms the context is EGL-backed
    // with the needed extensions present. Called once, lazily, from the
    // first submitHwFrame() call (via a flag checked in paintGL() too, in
    // case a frame arrives before initializeGL() has even run once).
    bool tryInitZeroCopy();
    // Imports m_pendingHwFrame (if any) into m_zeroCopyTextures, releasing
    // whatever previously backed them first. Called from paintGL().
    void importPendingHwFrameLocked();
    // Draws using m_zeroCopyTextures (NV12 shader). Called from paintGL()
    // instead of the normal YUV420P draw when a zero-copy frame is active.
    void drawZeroCopyFrame(bool dsa);
    void initZeroCopyShader();

private:
    std::atomic<int> m_frameWidth{-1};
    std::atomic<int> m_frameHeight{-1};

    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    QOpenGLShaderProgram m_program;
    std::array<GLuint, 3> m_textures{0, 0, 0};

    Slots m_slots;
    std::array<FrameBuffer, PBO_COUNT> m_frames;
    std::array<int, 3> m_pboStrides{0, 0, 0};

    std::atomic_bool m_acceptFrames{true};
    std::atomic_bool m_pboSizeValid{false};
    std::atomic_bool m_textureSizeMismatch{false};
    std::atomic_flag m_updatePending = ATOMIC_FLAG_INIT;

    bool m_isInitialized = false;
    std::atomic_bool m_useDsaPath{true};
    // Second, independent function resolver for the classic GL 4.1
    // fallback path (see class comment). Deliberately a *separate* object
    // from the QOpenGLFunctions_4_5_Core base: Qt's versioned wrappers
    // refuse to resolve any functions at all - not even the ones common to
    // both versions - if the context version is below what they target.
    QOpenGLFunctions_4_1_Core m_gl41;

    bool m_telemetryEnabled = false;
    mutable std::mutex m_pboMutex;

    std::atomic<std::uint64_t> m_submittedFrames{0};
    std::atomic<std::uint64_t> m_renderedFrames{0};
    std::atomic<std::uint64_t> m_overwrittenReadyFrames{0};
    std::atomic<std::uint64_t> m_droppedFrames{0};

    // --- Experimental zero-copy (VAAPI/Linux, EGL-backed contexts only)
    // A deliberately simple single-slot handoff rather than anything like
    // the PboMailboxStateMachine above: unlike setFrameData(), which
    // writes pixel data from the decoder thread and genuinely needs
    // multiple in-flight buffers to avoid stalling either side,
    // submitHwFrame() below does no GL/EGL work itself at all - it just
    // atomically replaces "the frame to import next", and everything
    // that actually touches GL/EGL happens later, exclusively on the GL
    // thread inside paintGL(). One slot is enough because only the most
    // recent frame ever matters (matching the same "mailbox" policy in
    // spirit, just without needing PBOs/fences/multiple buffers to get
    // there).
    struct PendingHwFrame {
        int width = 0;
        int height = 0;
        std::array<DmaBufPlane, 2> planes{};
        int planeCount = 0;
        std::function<void()> release;
    };
    mutable std::mutex m_hwFrameMutex;
    std::optional<PendingHwFrame> m_pendingHwFrame; // guarded by m_hwFrameMutex; consumed+cleared by importPendingHwFrameLocked() (GL thread)

    enum class ZeroCopyState { NotChecked, Unavailable, Available };
    std::atomic<ZeroCopyState> m_zeroCopyState{ZeroCopyState::NotChecked};

    // EGL/GL extension entry points, resolved lazily in tryInitZeroCopy().
    // Deliberately kept as generic function-pointer types (not the real
    // EGLDisplay/EGLImageKHR/PFNEGLCREATEIMAGEKHRPROC etc. from
    // <EGL/egl.h>/<EGL/eglext.h>) so this header - included from
    // videoform.h and, transitively, most of the rest of the UI layer -
    // never has to include EGL or (transitively, via EGL/X11
    // interop headers) X11 headers, which are well known for polluting
    // the global namespace with macros like Bool/True/False/Status that
    // collide with completely unrelated identifiers all over Qt/C++ code.
    // The real types are used only inside qyuvopenglwidget.cpp, which
    // includes <EGL/egl.h>/<EGL/eglext.h> itself, guarded to Linux only.
    using PFN_eglCreateImageKHR = void *(*)(void *dpy, void *ctx, unsigned int target,
                                            void *buffer, const std::int32_t *attribList);
    using PFN_eglDestroyImageKHR = unsigned int (*)(void *dpy, void *image);
    using PFN_glEGLImageTargetTexture2DOES = void (*)(GLenum target, void *image);
    void *m_eglDisplay = nullptr;
    PFN_eglCreateImageKHR m_eglCreateImageKHR = nullptr;
    PFN_eglDestroyImageKHR m_eglDestroyImageKHR = nullptr;
    PFN_glEGLImageTargetTexture2DOES m_glEGLImageTargetTexture2DOES = nullptr;

    QOpenGLShaderProgram m_zeroCopyProgram;
    std::array<GLuint, 2> m_zeroCopyTextures{0, 0}; // [0]=luma (R8), [1]=chroma (RG8, interleaved U/V - NV12)
    std::function<void()> m_activeHwFrameRelease;   // GL-thread only: release for whichever frame currently backs m_zeroCopyTextures
    int m_zeroCopyFrameWidth = 0;
    int m_zeroCopyFrameHeight = 0;
    bool m_zeroCopyFrameReady = false; // GL-thread only: true once m_zeroCopyTextures hold a validly-imported frame
};

#endif // QYUVOPENGLWIDGET_H
