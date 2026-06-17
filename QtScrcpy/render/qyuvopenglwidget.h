#ifndef QYUVOPENGLWIDGET_H
#define QYUVOPENGLWIDGET_H

#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>

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

signals:
    void requestUpdateTextures(int width, int height,
                               int strideY, int strideU, int strideV);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;

private:
    static constexpr int PBO_COUNT = 3;
    static constexpr int STATE_FREE = 0;
    static constexpr int STATE_WRITING = 1;
    static constexpr int STATE_READY = 2;
    static constexpr int STATE_PROCESSING = 3;

    struct alignas(64) FrameBuffer {
        std::array<GLuint, 3> pboIds{0, 0, 0};
        std::array<void *, 3> mappedPtrs{nullptr, nullptr, nullptr};
        GLsync fence = nullptr;
        std::atomic<int> state{STATE_FREE};
        // Producer may reclaim a READY slot while the render thread is
        // selecting the newest frame. Keep the sequence atomic so that this
        // comparison is defined even when that reclamation races with a read.
        std::atomic<std::uint64_t> sequence{0};
    };

    void initShader();
    void initTextures(int width, int height);
    void deInitTextures();
    bool initPBOs(int height, int strideY, int strideU, int strideV);
    void deInitPBOs();
    void deInitPBOsUnlocked();
    void setFrameSize(const QSize &frameSize);
    void checkFences();

    FrameBuffer *acquireWritableFrame();
    int acquireNewestReadyFrame();
    void releaseStaleReadyFrames(int selectedIndex);
    void scheduleUpdate();

private:
    std::atomic<int> m_frameWidth{-1};
    std::atomic<int> m_frameHeight{-1};

    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    QOpenGLShaderProgram m_program;
    std::array<GLuint, 3> m_textures{0, 0, 0};

    std::array<FrameBuffer, PBO_COUNT> m_frames;
    std::array<int, 3> m_pboStrides{0, 0, 0};

    std::atomic_bool m_acceptFrames{true};
    std::atomic_bool m_pboSizeValid{false};
    std::atomic_bool m_textureSizeMismatch{false};
    std::atomic_flag m_updatePending = ATOMIC_FLAG_INIT;

    bool m_isInitialized = false;
    bool m_telemetryEnabled = false;
    mutable std::mutex m_pboMutex;
    std::atomic<std::uint64_t> m_globalSequence{0};

    std::atomic<std::uint64_t> m_submittedFrames{0};
    std::atomic<std::uint64_t> m_renderedFrames{0};
    std::atomic<std::uint64_t> m_overwrittenReadyFrames{0};
    std::atomic<std::uint64_t> m_droppedFrames{0};
};

#endif // QYUVOPENGLWIDGET_H
