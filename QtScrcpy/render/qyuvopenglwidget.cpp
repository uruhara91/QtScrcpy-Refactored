#include "qyuvopenglwidget.h"
#include <QDebug>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <algorithm>
#include <bit>
#include <concepts>

extern "C" {
#include <libavutil/imgutils.h>
}

template <std::integral T>
constexpr T alignUp(T value, T alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr int align32(int width) {
    return alignUp(width, 32);
}

// State Definitions
constexpr int STATE_FREE = 0;
constexpr int STATE_READY = 1;
constexpr int STATE_PROCESSING = 2;

// Shader Vertex
static const char *vertShader = R"(#version 450 core
layout(location = 0) in vec3 vertexIn;
layout(location = 1) in vec2 textureIn;
out vec2 textureOut;

void main(void) {
    gl_Position = vec4(vertexIn, 1.0);
    textureOut = textureIn;
}
)";

// Shader Fragment
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

const vec3 yuvOffset = vec3(0.0625, 0.5, 0.5);
const vec3 rgbOffset = vec3(0.9729, -0.30148, 1.1334);
void main(void) {
    vec3 yuv;
    yuv.x = texture(tex_y, textureOut).r;
    yuv.y = texture(tex_u, textureOut).r;
    yuv.z = texture(tex_v, textureOut).r;

    FragColor = vec4(yuv2rgb * yuv - rgbOffset, 1.0);
}
)";

QYuvOpenGLWidget::QYuvOpenGLWidget(QWidget *parent) : QOpenGLWidget(parent) {
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
        [this](int w, int h, int strideY, int strideU, int strideV){
        if (isValid()) {
            makeCurrent();
            setFrameSize(QSize(w, h));
            initPBOs(h, strideY, strideU, strideV); 
            initTextures(w, h);
            m_textureSizeMismatch = false;
            doneCurrent();
            update();
        } else {
            m_textureSizeMismatch = false; 
        }
    });
}

QYuvOpenGLWidget::~QYuvOpenGLWidget() {
    if (isValid()) {
        makeCurrent();
        deInitTextures();
        deInitPBOs();
        if (m_vao) glDeleteVertexArrays(1, &m_vao);
        if (m_vbo) glDeleteBuffers(1, &m_vbo);
        doneCurrent();
    }
}

QSize QYuvOpenGLWidget::minimumSizeHint() const { return QSize(50, 50); }

QSize QYuvOpenGLWidget::sizeHint() const { 
    return QSize(m_frameWidth.load(std::memory_order_relaxed), 
                 m_frameHeight.load(std::memory_order_relaxed)); 
}

QSize QYuvOpenGLWidget::frameSize() const { 
    return QSize(m_frameWidth.load(std::memory_order_relaxed), 
                 m_frameHeight.load(std::memory_order_relaxed)); 
}

void QYuvOpenGLWidget::setFrameSize(const QSize &frameSize) {
    int w = frameSize.width();
    int h = frameSize.height();
    
    if (m_frameWidth.load(std::memory_order_relaxed) != w || 
        m_frameHeight.load(std::memory_order_relaxed) != h) {
        
        m_frameWidth.store(w, std::memory_order_relaxed);
        m_frameHeight.store(h, std::memory_order_relaxed);
        
        updateGeometry();
        m_pboSizeValid = false;
    }
}

void QYuvOpenGLWidget::setFrameData(int width, int height, 
                                   std::span<const uint8_t> dataY, 
                                   std::span<const uint8_t> dataU, 
                                   std::span<const uint8_t> dataV, 
                                   int linesizeY, int linesizeU, int linesizeV)
{
    int currentW = m_frameWidth.load(std::memory_order_relaxed);
    int currentH = m_frameHeight.load(std::memory_order_relaxed);

    bool sizeChanged = (width != currentW || height != currentH);
    bool strideChanged = (linesizeY != m_pboStrides[0] || linesizeU != m_pboStrides[1] || linesizeV != m_pboStrides[2]);

    if (sizeChanged || strideChanged || !m_pboSizeValid) [[unlikely]] {
        if (!m_textureSizeMismatch) {
            m_textureSizeMismatch = true;
            emit requestUpdateTextures(width, height, linesizeY, linesizeU, linesizeV);
        }
        return;
    }

    if (m_textureSizeMismatch) return;

    FrameBuffer* targetFrame = nullptr;
    
    std::scoped_lock lock(m_initLock); 
    if (!m_pboSizeValid) return;

    for (int i = 0; i < PBO_COUNT; ++i) {
        if (m_frames[i].state.load(std::memory_order_acquire) == STATE_FREE) {
            targetFrame = &m_frames[i];
            break;
        }
    }

    if (!targetFrame) {
        for (int i = 0; i < PBO_COUNT; ++i) {
            int expected = STATE_READY;
            if (m_frames[i].state.compare_exchange_strong(expected, STATE_FREE, std::memory_order_acq_rel)) {
                targetFrame = &m_frames[i];
                break;
            }
        }
    }

    if (!targetFrame) {
        if (!m_updatePending.test_and_set(std::memory_order_acq_rel)) {
            QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
        }
        return; 
    }

    const uint8_t* srcData[3] = { dataY.data(), dataU.data(), dataV.data() };
    int heights[3] = { height, (height + 1) / 2, (height + 1) / 2 };
    
    for (int i = 0; i < 3; i++) {
        auto dstPtr = static_cast<uint8_t*>(targetFrame->mappedPtrs[i]);
        if (dstPtr) {
            size_t totalBytes = static_cast<size_t>(m_pboStrides[i]) * heights[i];
            memcpy(dstPtr, srcData[i], totalBytes);
        }
    }

    targetFrame->state.store(STATE_READY, std::memory_order_release);

    if (!m_updatePending.test_and_set(std::memory_order_acq_rel)) {
        QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
    }
}

void QYuvOpenGLWidget::initializeGL() {
    if (!initializeOpenGLFunctions()) return;

    m_isInitialized = true;

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);

    initShader();

    // Setup Fullscreen Quad
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

void QYuvOpenGLWidget::initShader() {
    m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertShader);
    m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragShader);
    m_program.link();
    
    m_program.bind();
    m_program.setUniformValue("tex_y", 0);
    m_program.setUniformValue("tex_u", 1);
    m_program.setUniformValue("tex_v", 2);
    m_program.release();
}

void QYuvOpenGLWidget::initTextures(int width, int height) {
    if (!m_isInitialized) return;

    if (m_textures[0] != 0) {
        glDeleteTextures(3, m_textures.data());
    }

    glCreateTextures(GL_TEXTURE_2D, 3, m_textures.data());

    int widths[3] = {width, width / 2, width / 2};
    int heights[3] = {height, height / 2, height / 2};

    for (int i = 0; i < 3; i++) {
        glTextureParameteri(m_textures[i], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_textures[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_textures[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_textures[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureStorage2D(m_textures[i], 1, GL_R8, widths[i], heights[i]);
    }
}

void QYuvOpenGLWidget::initPBOs(int height, int strideY, int strideU, int strideV) {
    std::scoped_lock lock(m_initLock);
    deInitPBOs();

    m_pboStrides[0] = strideY;
    m_pboStrides[1] = strideU;
    m_pboStrides[2] = strideV;

    int sizes[3] = {
        strideY * height,           
        strideU * ((height + 1) / 2),
        strideV * ((height + 1) / 2)
    };

    GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

    for (int i = 0; i < PBO_COUNT; ++i) {
        glCreateBuffers(3, m_frames[i].pboIds.data());
        
        for (int plane = 0; plane < 3; plane++) {
            glNamedBufferStorage(m_frames[i].pboIds[plane], sizes[plane], nullptr, flags);
            m_frames[i].mappedPtrs[plane] = glMapNamedBufferRange(m_frames[i].pboIds[plane], 0, sizes[plane], flags);
        }
        
        m_frames[i].fence = 0;
        m_frames[i].state.store(STATE_FREE);
    }
    m_pboSizeValid = true;
}

void QYuvOpenGLWidget::deInitTextures() {
    if (!m_isInitialized) return;
    if (m_textures[0] != 0) {
        glDeleteTextures(3, m_textures.data());
        std::ranges::fill(m_textures, 0);
    }
}

void QYuvOpenGLWidget::deInitPBOs() {
    m_pboSizeValid = false;

    for (int i = 0; i < PBO_COUNT; ++i) {
        if (m_frames[i].fence) {
            glDeleteSync(m_frames[i].fence);
            m_frames[i].fence = 0;
        }

        for (int plane = 0; plane < 3; plane++) {
            if (m_frames[i].pboIds[plane] != 0) {
                glUnmapNamedBuffer(m_frames[i].pboIds[plane]);
                glDeleteBuffers(1, &m_frames[i].pboIds[plane]);
                m_frames[i].pboIds[plane] = 0;
                m_frames[i].mappedPtrs[plane] = nullptr;
            }
        }
        m_frames[i].state.store(STATE_FREE);
    }
}

void QYuvOpenGLWidget::checkFences() {
    for (int i = 0; i < PBO_COUNT; ++i) {
        if (m_frames[i].state.load(std::memory_order_acquire) == STATE_PROCESSING) {
            if (m_frames[i].fence) {
                GLenum result = glClientWaitSync(m_frames[i].fence, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
                
                if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) {
                    glDeleteSync(m_frames[i].fence);
                    m_frames[i].fence = 0;
                    m_frames[i].state.store(STATE_FREE, std::memory_order_release);
                }
            }
        }
    }
}

void QYuvOpenGLWidget::paintGL() {
    m_updatePending.clear(std::memory_order_release);

    if (!m_pboSizeValid) return;

    checkFences();

    int drawIndex = -1;
    
    for (int i = 0; i < PBO_COUNT; ++i) {
        if (m_frames[i].state.load(std::memory_order_acquire) == STATE_READY) {
            drawIndex = i;
            break; 
        }
    }

    if (drawIndex != -1) {
        FrameBuffer& fb = m_frames[drawIndex];
        
        int w = m_frameWidth.load(std::memory_order_relaxed);
        int h = m_frameHeight.load(std::memory_order_relaxed);

        for (int i = 0; i < 3; i++) {
            glBindTextureUnit(i, m_textures[i]);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, fb.pboIds[i]);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, m_pboStrides[i]);
            glTextureSubImage2D(m_textures[i], 0, 0, 0, 
                                w / (i>0?2:1), 
                                h / (i>0?2:1), 
                                GL_RED, GL_UNSIGNED_BYTE, nullptr);
        }
        
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        if (fb.fence) glDeleteSync(fb.fence);
        fb.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        fb.state.store(STATE_PROCESSING, std::memory_order_release);
    } 

    m_program.bind();
    glBindVertexArray(m_vao);
    
    for (int i = 0; i < 3; i++) {
        glBindTextureUnit(i, m_textures[i]);
    }
    
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    
    glBindVertexArray(0);
    m_program.release();
}

void QYuvOpenGLWidget::resizeGL(int width, int height) {
    glViewport(0, 0, width, height);
}