#ifndef VIDEOFORM_H
#define VIDEOFORM_H

#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QWidget>
#include <atomic>
#include <span>

#include "../QtScrcpyCore/include/QtScrcpyCore.h"
#include "../render/qyuvopenglwidget.h"

namespace Ui
{
    class videoForm;
}

class ToolForm;
class FileHandler;
class QLabel;

class VideoForm : public QWidget,
                  public qsc::DeviceObserver,
                  public qsc::FrameSink
{
    Q_OBJECT
public:
    explicit VideoForm(bool framelessWindow = false, bool skin = true, bool showToolBar = true, QWidget *parent = Q_NULLPTR);
    ~VideoForm();

    // Legacy compatibility entry point. Decoded frames use FrameSink directly.
    void updateRender(int width, int height,
                      std::span<const uint8_t> dataY,
                      std::span<const uint8_t> dataU,
                      std::span<const uint8_t> dataV,
                      int linesizeY, int linesizeU, int linesizeV);
    void staysOnTop(bool top = true);
    void updateShowSize(const QSize &newSize);
    void setSerial(const QString& serial);
    QRect getGrabCursorRect();
    const QSize &frameSize();
    void resizeSquare();
    void removeBlackRect();
    void showFPS(bool show);
    void switchFullScreen();
    bool isHost();

private:
    void activateFrameSink() noexcept override;
    void deactivateFrameSink() noexcept override;
    void submitFrame(int width, int height,
                     std::span<const uint8_t> dataY,
                     std::span<const uint8_t> dataU,
                     std::span<const uint8_t> dataV,
                     int linesizeY, int linesizeU, int linesizeV) noexcept override;

    void onFrame(int width, int height,
                 std::span<const uint8_t> dataY,
                 std::span<const uint8_t> dataU,
                 std::span<const uint8_t> dataV,
                 int linesizeY, int linesizeU, int linesizeV) override;
    void updateFPS(quint32 fps) override;
    void grabCursor(bool grab) override;

    void scheduleFrameUiUpdate() noexcept;
    void processFrameUiUpdate();
    void syncFpsCounterState();

    void updateStyleSheet(bool vertical);
    QMargins getMargins(bool vertical);

    void initUI();
    void showToolForm(bool show = true);
    void moveCenter();
    void installShortcut();
    QRect getScreenRect();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    Ui::videoForm *ui;
    QPointer<ToolForm> m_toolForm;
    QPointer<QWidget> m_loadingWidget;
    QPointer<QYuvOpenGLWidget> m_videoWidget;
    QPointer<QLabel> m_fpsLabel;

    QSize m_frameSize;
    QSize m_normalSize;
    QPoint m_dragPosition;
    float m_widthHeightRatio = 0.5f;
    bool m_skin = true;
    QPoint m_fullScreenBeforePos;
    QString m_serial;

    bool show_toolbar = true;
    bool m_isFullScreen = false;
    bool m_framelessWindow = false;

    std::atomic<bool> m_resizePending = false;
    std::atomic<QYuvOpenGLWidget*> m_frameSinkWidget{nullptr};
    std::atomic<int> m_latestFrameWidth{0};
    std::atomic<int> m_latestFrameHeight{0};
    std::atomic_bool m_frameUiUpdatePending{false};
};

inline void VideoForm::activateFrameSink() noexcept
{
    Q_ASSERT(QThread::currentThread() == thread());

    m_latestFrameWidth.store(0, std::memory_order_relaxed);
    m_latestFrameHeight.store(0, std::memory_order_relaxed);
    m_frameUiUpdatePending.store(false, std::memory_order_relaxed);
    m_frameSinkWidget.store(m_videoWidget.data(), std::memory_order_release);

    QMetaObject::invokeMethod(
        this,
        [this]() { syncFpsCounterState(); },
        Qt::QueuedConnection);
}

inline void VideoForm::deactivateFrameSink() noexcept
{
    Q_ASSERT(QThread::currentThread() == thread());

    if (auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial)) {
        device->setFpsCounterEnabled(false);
    }

    m_frameSinkWidget.store(nullptr, std::memory_order_release);
    m_latestFrameWidth.store(0, std::memory_order_relaxed);
    m_latestFrameHeight.store(0, std::memory_order_relaxed);
}

inline void VideoForm::submitFrame(int width, int height,
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

inline void VideoForm::scheduleFrameUiUpdate() noexcept
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

inline void VideoForm::processFrameUiUpdate()
{
    Q_ASSERT(QThread::currentThread() == thread());

    QYuvOpenGLWidget *widget = m_frameSinkWidget.load(std::memory_order_acquire);
    const int width = m_latestFrameWidth.load(std::memory_order_acquire);
    const int height = m_latestFrameHeight.load(std::memory_order_acquire);

    if (widget && width > 0 && height > 0) {
        if (m_loadingWidget) m_loadingWidget->close();
        if (widget->isHidden()) widget->show();
        updateShowSize(QSize(width, height));
        syncFpsCounterState();
    }

    m_frameUiUpdatePending.store(false, std::memory_order_release);

    if (m_frameSinkWidget.load(std::memory_order_acquire) &&
        (m_latestFrameWidth.load(std::memory_order_acquire) != width ||
         m_latestFrameHeight.load(std::memory_order_acquire) != height)) {
        scheduleFrameUiUpdate();
    }
}

inline void VideoForm::syncFpsCounterState()
{
    Q_ASSERT(QThread::currentThread() == thread());

    if (auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial)) {
        const bool sinkActive =
            m_frameSinkWidget.load(std::memory_order_acquire) != nullptr;
        const bool enabled = sinkActive && m_fpsLabel && m_fpsLabel->isVisible();
        device->setFpsCounterEnabled(enabled);
    }
}

#endif // VIDEOFORM_H
