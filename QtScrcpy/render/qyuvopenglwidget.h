#ifndef QYUVOPENGLWIDGET_H
#define QYUVOPENGLWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLShaderProgram>
#include <span>
#include <array>
#include <atomic>
#include <mutex>

class QYuvOpenGLWidget : public QOpenGLWidget, protected QOpenGLFunctions_4_5_Core
{
    Q_OBJECT
public:
    explicit QYuvOpenGLWidget(QWidget *parent = nullptr);
    virtual ~QYuvOpenGLWidget() override;

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

    void setFrameData(int width, int height, 
                      std::span<const uint8_t> dataY, 
                      std::span<const uint8_t> dataU, 
                      std::span<const uint8_t> dataV, 
                      int linesizeY, int linesizeU, int linesizeV);
    
    QSize frameSize() const;

signals:
    void requestUpdateTextures(int width, int height, int strideY, int strideU, int strideV);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;

private:
    void initShader();
    void initTextures(int width, int height);
    void deInitTextures();
    
    void initPBOs(int height, int strideY, int strideU, int strideV);
    void deInitPBOs();
    
    void setFrameSize(const QSize &frameSize);

    void checkFences();

private:
    std::atomic<int> m_frameWidth { -1 };
    std::atomic<int> m_frameHeight { -1 };

    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    QOpenGLShaderProgram m_program;

    std::array<GLuint, 3> m_textures = {0, 0, 0}; 
    
    static constexpr int PBO_COUNT = 3;
    
    struct FrameBuffer {
        std::array<GLuint, 3> pboIds = {0, 0, 0};
        std::array<void*, 3> mappedPtrs = {nullptr};
        GLsync fence = 0;
        
        std::atomic<int> state {0};
        uint64_t sequence = 0;
    };

    std::array<FrameBuffer, PBO_COUNT> m_frames;
    std::array<int, 3> m_pboStrides = {0, 0, 0};
    
    bool m_pboSizeValid = false;
    bool m_isInitialized = false;

    std::atomic<bool> m_textureSizeMismatch = false;
    std::atomic_flag m_updatePending = ATOMIC_FLAG_INIT;

    std::mutex m_initLock;
    uint64_t m_globalSequence = 0;
};

#endif // QYUVOPENGLWIDGET_H