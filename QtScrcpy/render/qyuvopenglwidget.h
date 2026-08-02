#ifndef QYUVOPENGLWIDGET_H
#define QYUVOPENGLWIDGET_H

#include <QOpenGLFunctions_4_1_Core>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include "pbomailboxstatemachine.h"

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
    void scheduleUpdate();

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
};

#endif // QYUVOPENGLWIDGET_H
