#ifndef VIDEOFORM_H
#define VIDEOFORM_H

#include <QPointer>
#include <QWidget>
#include <atomic>
#include <span>

#include "../QtScrcpyCore/include/QtScrcpyCore.h"

namespace Ui
{
    class videoForm;
}

class ToolForm;
class FileHandler;
class QYuvOpenGLWidget;
class QLabel;

class VideoForm : public QWidget,
                  public qsc::DeviceObserver,
                  public qsc::FrameSink
{
    Q_OBJECT
public:
    explicit VideoForm(bool framelessWindow = false, bool skin = true, bool showToolBar = true, QWidget *parent = Q_NULLPTR);
    ~VideoForm();

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
    // FrameSink implementation. submitFrame() runs on the decoder worker;
    // activation/deactivation run on the GUI thread under Device synchronization.
    void activateFrameSink() noexcept override;
    void deactivateFrameSink() noexcept override;
    void submitFrame(int width, int height,
                     std::span<const uint8_t> dataY,
                     std::span<const uint8_t> dataU,
                     std::span<const uint8_t> dataV,
                     int linesizeY, int linesizeU, int linesizeV) noexcept override;

    // DeviceObserver implementation. Kept as a compatibility fallback; Device
    // routes decoded frames through FrameSink in the optimized path.
    void onFrame(int width, int height,
                 std::span<const uint8_t> dataY,
                 std::span<const uint8_t> dataU,
                 std::span<const uint8_t> dataV,
                 int linesizeY, int linesizeU, int linesizeV) override;
    void updateFPS(quint32 fps) override;
    void grabCursor(bool grab) override;

    void scheduleFrameUiUpdate() noexcept;
    void processFrameUiUpdate();

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

    // Legacy updateRender() coalescing remains for compatibility.
    std::atomic<bool> m_resizePending = false;

    // Dedicated frame path state. The decoder thread only touches these atomics
    // and QYuvOpenGLWidget::setFrameData(), which owns its own synchronization.
    std::atomic<QYuvOpenGLWidget*> m_frameSinkWidget{nullptr};
    std::atomic<int> m_latestFrameWidth{0};
    std::atomic<int> m_latestFrameHeight{0};
    std::atomic_bool m_frameUiUpdatePending{false};
};

#endif // VIDEOFORM_H
