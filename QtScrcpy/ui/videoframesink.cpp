#include "videoform.h"

#include <QMetaObject>
#include <QThread>

#include "../render/qyuvopenglwidget.h"

void VideoForm::activateFrameSink() noexcept
{
    Q_ASSERT(QThread::currentThread() == thread());

    m_latestFrameWidth.store(0, std::memory_order_relaxed);
    m_latestFrameHeight.store(0, std::memory_order_relaxed);
    m_frameUiUpdatePending.store(false, std::memory_order_relaxed);
    m_frameSinkWidget.store(m_videoWidget.data(), std::memory_order_release);
}

void VideoForm::deactivateFrameSink() noexcept
{
    Q_ASSERT(QThread::currentThread() == thread());

    m_frameSinkWidget.store(nullptr, std::memory_order_release);
    m_latestFrameWidth.store(0, std::memory_order_relaxed);
    m_latestFrameHeight.store(0, std::memory_order_relaxed);
}

void VideoForm::submitFrame(int width, int height,
                            std::span<const uint8_t> dataY,
                            std::span<const uint8_t> dataU,
                            std::span<const uint8_t> dataV,
                            int linesizeY, int linesizeU, int linesizeV) noexcept
{
    if (width <= 0 || height <= 0) return;

    QYuvOpenGLWidget *widget = m_frameSinkWidget.load(std::memory_order_acquire);
    if (!widget) return;

    widget->setFrameData(width, height,
                         dataY, dataU, dataV,
                         linesizeY, linesizeU, linesizeV);

    const int previousWidth = m_latestFrameWidth.exchange(
        width, std::memory_order_acq_rel);
    const int previousHeight = m_latestFrameHeight.exchange(
        height, std::memory_order_acq_rel);

    if (previousWidth != width || previousHeight != height) {
        scheduleFrameUiUpdate();
    }
}

void VideoForm::scheduleFrameUiUpdate() noexcept
{
    bool expected = false;
    if (!m_frameUiUpdatePending.compare_exchange_strong(
            expected, true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }

    const bool queued = QMetaObject::invokeMethod(
        this,
        [this]() { processFrameUiUpdate(); },
        Qt::QueuedConnection);

    if (!queued) {
        m_frameUiUpdatePending.store(false, std::memory_order_release);
    }
}

void VideoForm::processFrameUiUpdate()
{
    Q_ASSERT(QThread::currentThread() == thread());

    QYuvOpenGLWidget *widget = m_frameSinkWidget.load(std::memory_order_acquire);
    const int width = m_latestFrameWidth.load(std::memory_order_acquire);
    const int height = m_latestFrameHeight.load(std::memory_order_acquire);

    if (widget && width > 0 && height > 0) {
        if (m_loadingWidget) m_loadingWidget->close();
        if (widget->isHidden()) widget->show();
        updateShowSize(QSize(width, height));
    }

    m_frameUiUpdatePending.store(false, std::memory_order_release);

    if (m_frameSinkWidget.load(std::memory_order_acquire) &&
        (m_latestFrameWidth.load(std::memory_order_acquire) != width ||
         m_latestFrameHeight.load(std::memory_order_acquire) != height)) {
        scheduleFrameUiUpdate();
    }
}
