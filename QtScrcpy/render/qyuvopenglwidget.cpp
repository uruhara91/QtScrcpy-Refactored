#include "qyuvopenglwidget.h"

#include <QDebug>
#include <QMetaObject>
#include <QSurfaceFormat>
#include <algorithm>
#include <concepts>
#include <cstring>
#include <limits>
#include <mutex>

// State definitions for each persistently mapped PBO set.
constexpr int STATE_FREE = 0;
constexpr int STATE_WRITING = 1;
constexpr int STATE_READY = 2;
constexpr int STATE_PROCESSING = 3;

static const char *vertShader = R"(#version 450 core
layout(location = 0) in vec3 vertexIn;
layout(location = 1) in vec2 textureIn;
out vec2 textureOut;

void main(void) {
    gl_Position = vec4(vertexIn, 1.0);
    textureOut = textureIn;
}
)";

static const char *fragShader = R"(#version 450 core
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

QYuvOpenGLWidget::QYuvOpenGLWidget(QWidget *parent) : QOpenGLWidget(parent)
{
    QSurfaceFormat format;
    format.setVersion(4, 5);
    format.setProfile(QSurfaceFormat::CoreProfile);
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
            [this](int w, int h, int strideY, int strideU, int strideV) {
        if (!m_acceptFrames.load(std::memory_order_acquire)) return;

        if (w < 16 || w > 8192 || h < 16 || h > 8192 ||
            strideY <= 0 || strideU <= 0 || strideV <= 0) {
            m_textureSizeMismatch.store(false, std::memory_order_release);
            return;
        }

        if (!isValid()) {
            m_textureSizeMismatch.store(false, std::memory_order_release);
            return;
        }

        makeCurrent();
        setFrameSize(QSize(w, h));
        const bool pboReady = initPBOs(h, strideY, strideU, strideV);
        if (pboReady) {
            initTextures(w, h);
        }
        doneCurrent();

        m_textureSizeMismatch.store(false, std::memory_order_release);
        if (pboReady) update();
    }, Qt::QueuedConnection);
}

QYuvOpenGLWidget::~QYuvOpenGLWidget()
{
    m_acceptFrames.store(false, std::memory_order_release);

    if (isValid()) {
        makeCurrent();
        deInitTextures();
        deInitPBOs();
        if (m_vao) glDeleteVertexArrays(1, &m_vao);
        if (m_vbo) glDeleteBuffers(1, &m_vbo);
        m_vao = 0;
        m_vbo = 0;
        doneCurrent();
    }
}

QSize QYuvOpenGLWidget::minimumSizeHint() const
{
    return QSize(50, 50);
}

QSize QYuvOpenGLWidget::sizeHint() const
{
    return QSize(m_frameWidth.load(std::memory_order_relaxed),
                 m_frameHeight.load(std::memory_order_relaxed));
}

QSize QYuvOpenGLWidget::frameSize() const
{
    return QSize(m_frameWidth.load(std::memory_order_relaxed),
                 m_frameHeight.load(std::memory_order_relaxed));
}

void QYuvOpenGLWidget::setFrameSize(const QSize &frameSize)
{
    const int w = frameSize.width();
    const int h = frameSize.height();
    if (m_frameWidth.load(std::memory_order_relaxed) == w &&
        m_frameHeight.load(std::memory_order_relaxed) == h) {
        return;
    }

    m_frameWidth.store(w, std::memory_order_relaxed);
    m_frameHeight.store(h, std::memory_order_relaxed);
    m_pboSizeValid.store(false, std::memory_order_release);
    updateGeometry();
}

void QYuvOpenGLWidget::setFrameData(int width, int height,
                                    std::span<const uint8_t> dataY,
                                    std::span<const uint8_t> dataU,
                                    std::span<const uint8_t> dataV,
                                    int linesizeY, int linesizeU, int linesizeV)
{
    if (!m_acceptFrames.load(std::memory_order_acquire)) return;
    if (width <= 0 || height <= 0 || linesizeY <= 0 || linesizeU <= 0 || linesizeV <= 0) return;

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

    std::shared_lock<std::shared_mutex> readLock(m_rwLock, std::try_to_lock);
    if (!readLock.owns_lock()) return;

    const bool sizeChanged = width != m_frameWidth.load(std::memory_order_relaxed) ||
                             height != m_frameHeight.load(std::memory_order_relaxed);
    const bool strideChanged = linesizeY != m_pboStrides[0] ||
                               linesizeU != m_pboStrides[1] ||
                               linesizeV != m_pboStrides[2];
    const bool pboInvalid = !m_pboSizeValid.load(std::memory_order_acquire);

    if (sizeChanged || strideChanged || pboInvalid) {
        readLock.unlock();
        bool expected = false;
        if (m_textureSizeMismatch.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
            emit requestUpdateTextures(width, height, linesizeY, linesizeU, linesizeV);
        }
        return;
    }

    FrameBuffer *targetFrame = nullptr;
    for (FrameBuffer &frame : m_frames) {
        int expected = STATE_FREE;
        if (frame.state.compare_exchange_strong(
                expected, STATE_WRITING, std::memory_order_acq_rel, std::memory_order_acquire)) {
            targetFrame = &frame;
            break;
        }
    }

    if (!targetFrame) return;

    const std::array<const uint8_t *, 3> srcData{dataY.data(), dataU.data(), dataV.data()};
    for (int plane = 0; plane < 3; ++plane) {
        auto *destination = static_cast<uint8_t *>(targetFrame->mappedPtrs[plane]);
        if (!destination) {
            targetFrame->state.store(STATE_FREE, std::memory_order_release);
            return;
        }
        std::memcpy(destination, srcData[plane], requiredBytes[plane]);
    }

    targetFrame->sequence = m_globalSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    targetFrame->state.store(STATE_READY, std::memory_order_release);

    if (!m_updatePending.test_and_set(std::memory_order_acq_rel)) {
        QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
    }
}

void QYuvOpenGLWidget::initializeGL()
{
    if (!initializeOpenGLFunctions()) return;

    m_isInitialized = true;
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);

    initShader();

    static const float coordinate[] = {
        -1.0f, -1.0f, 0.0f,   0.0f, 1.0f,
         1.0f, -1.0f, 0.0f,   1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f,   0.0f, 0.0f,
         1.0f,  1.0f, 0.0f,   1.0f, 0.0f
    };

    glCreateVertexArrays(1, &m_vao);
    glCreateBuffers(1, &m_vbo);
    glNamedBufferStorage(m_vbo, sizeof(coordinate), coordinate, 0);

    glVertexArrayVertexBuffer(m_vao, 0, m_vbo, 0, 5 * sizeof(float));
    glEnableVertexArrayAttrib(m_vao, 0);
    glVertexArrayAttribFormat(m_vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(m_vao, 0, 0);
    glEnableVertexArrayAttrib(m_vao, 1);
    glVertexArrayAttribFormat(m_vao, 1, 2, GL_FLOAT, GL_FALSE, 3 * sizeof(float));
    glVertexArrayAttribBinding(m_vao, 1, 0);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void QYuvOpenGLWidget::initShader()
{
    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShader) ||
        !m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShader) ||
        !m_program.link()) {
        qCritical() << "Failed to initialize YUV shader:" << m_program.log();
        return;
    }

    m_program.bind();
    m_program.setUniformValue("tex_y", 0);
    m_program.setUniformValue("tex_u", 1);
    m_program.setUniformValue("tex_v", 2);
    m_program.release();
}

void QYuvOpenGLWidget::initTextures(int width, int height)
{
    if (!m_isInitialized || width <= 0 || height <= 0) return;

    deInitTextures();
    glCreateTextures(GL_TEXTURE_2D, 3, m_textures.data());

    const std::array<int, 3> widths{width, (width + 1) / 2, (width + 1) / 2};
    const std::array<int, 3> heights{height, (height + 1) / 2, (height + 1) / 2};

    for (int plane = 0; plane < 3; ++plane) {
        glTextureParameteri(m_textures[plane], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_textures[plane], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_textures[plane], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_textures[plane], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureStorage2D(m_textures[plane], 1, GL_R8, widths[plane], heights[plane]);
    }
}

bool QYuvOpenGLWidget::initPBOs(int height, int strideY, int strideU, int strideV)
{
    std::unique_lock<std::shared_mutex> writeLock(m_rwLock);
    deInitPBOsUnlocked();

    if (height <= 0 || strideY <= 0 || strideU <= 0 || strideV <= 0) return false;

    const int chromaHeight = (height + 1) / 2;
    const std::array<GLsizeiptr, 3> sizes{
        static_cast<GLsizeiptr>(strideY) * height,
        static_cast<GLsizeiptr>(strideU) * chromaHeight,
        static_cast<GLsizeiptr>(strideV) * chromaHeight
    };

    if (std::ranges::any_of(sizes, [](GLsizeiptr size) { return size <= 0; })) return false;

    constexpr GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
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
        frame.fence = nullptr;
        frame.sequence = 0;
        frame.state.store(STATE_FREE, std::memory_order_release);
    }

    m_pboStrides = {strideY, strideU, strideV};
    m_pboSizeValid.store(true, std::memory_order_release);
    return true;
}

void QYuvOpenGLWidget::deInitTextures()
{
    if (!m_isInitialized) return;
    if (m_textures[0] != 0) {
        glDeleteTextures(3, m_textures.data());
        std::ranges::fill(m_textures, 0);
    }
}

void QYuvOpenGLWidget::deInitPBOs()
{
    std::unique_lock<std::shared_mutex> writeLock(m_rwLock);
    deInitPBOsUnlocked();
}

void QYuvOpenGLWidget::deInitPBOsUnlocked()
{
    m_pboSizeValid.store(false, std::memory_order_release);

    for (FrameBuffer &frame : m_frames) {
        if (frame.fence) {
            glDeleteSync(frame.fence);
            frame.fence = nullptr;
        }

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

        frame.sequence = 0;
        frame.state.store(STATE_FREE, std::memory_order_release);
    }

    m_pboStrides = {0, 0, 0};
}

void QYuvOpenGLWidget::checkFences()
{
    for (FrameBuffer &frame : m_frames) {
        if (frame.state.load(std::memory_order_acquire) != STATE_PROCESSING) continue;

        if (!frame.fence) {
            frame.state.store(STATE_FREE, std::memory_order_release);
            continue;
        }

        const GLenum result = glClientWaitSync(frame.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
        if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) {
            glDeleteSync(frame.fence);
            frame.fence = nullptr;
            frame.state.store(STATE_FREE, std::memory_order_release);
        } else if (result == GL_WAIT_FAILED) {
            glFinish();
            glDeleteSync(frame.fence);
            frame.fence = nullptr;
            frame.state.store(STATE_FREE, std::memory_order_release);
        }
    }
}

void QYuvOpenGLWidget::paintGL()
{
    m_updatePending.clear(std::memory_order_release);
    if (!m_pboSizeValid.load(std::memory_order_acquire)) return;

    checkFences();

    int drawIndex = -1;
    std::uint64_t highestSequence = 0;
    for (int i = 0; i < PBO_COUNT; ++i) {
        if (m_frames[i].state.load(std::memory_order_acquire) != STATE_READY) continue;

        if (m_frames[i].sequence > highestSequence) {
            if (drawIndex != -1) {
                m_frames[drawIndex].state.store(STATE_FREE, std::memory_order_release);
            }
            highestSequence = m_frames[i].sequence;
            drawIndex = i;
        } else {
            m_frames[i].state.store(STATE_FREE, std::memory_order_release);
        }
    }

    if (drawIndex != -1) {
        FrameBuffer &frame = m_frames[drawIndex];
        const int width = m_frameWidth.load(std::memory_order_relaxed);
        const int height = m_frameHeight.load(std::memory_order_relaxed);
        const std::array<int, 3> widths{width, (width + 1) / 2, (width + 1) / 2};
        const std::array<int, 3> heights{height, (height + 1) / 2, (height + 1) / 2};

        for (int plane = 0; plane < 3; ++plane) {
            glBindTextureUnit(plane, m_textures[plane]);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, frame.pboIds[plane]);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, m_pboStrides[plane]);
            glTextureSubImage2D(m_textures[plane], 0, 0, 0,
                                widths[plane], heights[plane],
                                GL_RED, GL_UNSIGNED_BYTE, nullptr);
        }

        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        frame.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (frame.fence) {
            frame.state.store(STATE_PROCESSING, std::memory_order_release);
        } else {
            glFinish();
            frame.state.store(STATE_FREE, std::memory_order_release);
        }
    }

    if (!m_program.isLinked()) return;

    m_program.bind();
    glBindVertexArray(m_vao);
    for (int plane = 0; plane < 3; ++plane) {
        glBindTextureUnit(plane, m_textures[plane]);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    m_program.release();
}

void QYuvOpenGLWidget::resizeGL(int width, int height)
{
    glViewport(0, 0, width, height);
}
