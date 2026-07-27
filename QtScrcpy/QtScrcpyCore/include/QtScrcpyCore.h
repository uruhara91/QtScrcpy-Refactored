#pragma once
#include <QMouseEvent>
#include <QPointF>
#include <QPointer>
#include <span>

#include "QtScrcpyCoreDef.h"

namespace qsc {

class FrameSink {
protected:
    FrameSink() = default;
    virtual ~FrameSink() = default;

public:
    virtual void activateFrameSink() noexcept {}
    virtual void deactivateFrameSink() noexcept {}

    virtual void submitFrame(int width, int height,
                             std::span<const uint8_t> dataY,
                             std::span<const uint8_t> dataU,
                             std::span<const uint8_t> dataV,
                             int linesizeY, int linesizeU, int linesizeV) noexcept = 0;
};

class DeviceObserver {
protected:
    DeviceObserver() = default;
    virtual ~DeviceObserver() = default;

public:
    virtual void onFrame(int width, int height,
                         std::span<const uint8_t> dataY,
                         std::span<const uint8_t> dataU,
                         std::span<const uint8_t> dataV,
                         int linesizeY, int linesizeU, int linesizeV)
    {
        Q_UNUSED(width);
        Q_UNUSED(height);
        Q_UNUSED(dataY);
        Q_UNUSED(dataU);
        Q_UNUSED(dataV);
        Q_UNUSED(linesizeY);
        Q_UNUSED(linesizeU);
        Q_UNUSED(linesizeV);
    }
    virtual void updateFPS(quint32 fps) { Q_UNUSED(fps); }
    virtual void grabCursor(bool grab) { Q_UNUSED(grab); }

    virtual void mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize) {
        Q_UNUSED(from);
        Q_UNUSED(frameSize);
        Q_UNUSED(showSize);
    }
    virtual void relativeMouseMoveEvent(const QPointF &delta, const QSize &frameSize, const QSize &showSize) {
        Q_UNUSED(delta);
        Q_UNUSED(frameSize);
        Q_UNUSED(showSize);
    }
    virtual void wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize) {
        Q_UNUSED(from);
        Q_UNUSED(frameSize);
        Q_UNUSED(showSize);
    }
    virtual void keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize) {
        Q_UNUSED(from);
        Q_UNUSED(frameSize);
        Q_UNUSED(showSize);
    }

    virtual void postGoBack() {}
    virtual void postGoHome() {}
    virtual void postGoMenu() {}
    virtual void postAppSwitch() {}
    virtual void postPower() {}
    virtual void postVolumeUp() {}
    virtual void postVolumeDown() {}
    virtual void postCopy() {}
    virtual void postCut() {}
    virtual void setDisplayPower(bool on) { Q_UNUSED(on); }
    virtual void expandNotificationPanel() {}
    virtual void collapsePanel() {}
    virtual void postBackOrScreenOn(bool down) { Q_UNUSED(down); }
    virtual void postTextInput(QString &text) { Q_UNUSED(text); }
    virtual void requestDeviceClipboard() {}
    virtual void setDeviceClipboard(bool pause = true) { Q_UNUSED(pause); }
    virtual void clipboardPaste() {}
    virtual void pushFileRequest(const QString &file, const QString &devicePath) {
        Q_UNUSED(file);
        Q_UNUSED(devicePath);
    }
    virtual void installApkRequest(const QString &apkFile) { Q_UNUSED(apkFile); }
    virtual void screenshot() {}
    virtual void showTouch(bool show) { Q_UNUSED(show); }
};

class IDevice : public QObject {
    Q_OBJECT
public:
    IDevice(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~IDevice(){}

signals:
    // NOTE: on success (success == true), this is now emitted once the
    // first video frame's size is known, not immediately after the ADB/TCP
    // handshake completes. This is a consequence of scrcpy-server >= 4.0,
    // which no longer sends the video size during the handshake itself, but
    // slightly later as part of the video stream. `size` is guaranteed
    // valid whenever success is true. On failure (success == false), size
    // is always an invalid QSize().
    void deviceConnected(bool success, const QString& serial, const QString& deviceName, const QSize& size);
    void deviceDisconnected(QString serial);

public:
    virtual void setUserData(void* data) = 0;
    virtual void* getUserData() = 0;
    // KONTRAK PENTING: observer TIDAK BOLEH memanggil registerDeviceObserver()/
    // deRegisterDeviceObserver() secara sinkron dari dalam callback
    // DeviceObserver miliknya sendiri (mis. dari dalam mouseEvent()/keyEvent()
    // dkk yang dipanggil Device). Implementasinya pakai std::shared_mutex yang
    // TIDAK reentrant: shared_lock (dipegang Device selama iterasi observer)
    // lalu upgrade ke exclusive lock di thread yang sama = UB/berpotensi
    // deadlock (audit §4.4). Kalau perlu (de)register akibat sebuah event,
    // defer lewat QMetaObject::invokeMethod(..., Qt::QueuedConnection) atau
    // sejenisnya supaya baru dieksekusi setelah callback ini selesai.
    virtual void registerDeviceObserver(DeviceObserver* observer) = 0;
    virtual void deRegisterDeviceObserver(DeviceObserver* observer) = 0;
    virtual void registerFrameSink(FrameSink* sink) = 0;
    virtual void deRegisterFrameSink(FrameSink* sink) = 0;
    virtual void setFpsCounterEnabled(bool enabled) = 0;

    virtual bool connectDevice() = 0;
    virtual void disconnectDevice() = 0;

    virtual void mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize) = 0;
    // Feeds a raw relative pointer motion delta (physical pixels of the
    // `showSize` surface, unaccelerated) directly into the input pipeline,
    // bypassing the QMouseEvent/absolute-position path entirely. Intended
    // for platform-native relative-pointer sources - currently the Wayland
    // zwp_relative_pointer_v1 protocol (see util/mousetap/waylandmousetap.h
    // in the QtScrcpy app) - where the OS/compositor locks the cursor in
    // place and reports motion deltas directly, instead of the
    // warp-cursor-to-center + reconstruct-delta-from-QMouseEvent technique
    // used on X11/Windows/macOS. Only meaningful while game-mode/mouse-move
    // mapping is active; implementations are free to ignore this call
    // otherwise (mirrors mouseEvent()'s own behavior in that regard).
    virtual void relativeMouseMoveEvent(const QPointF &delta, const QSize &frameSize, const QSize &showSize) = 0;
    virtual void wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize) = 0;
    virtual void keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize) = 0;

    virtual void postGoBack() = 0;
    virtual void postGoHome() = 0;
    virtual void postGoMenu() = 0;
    virtual void postAppSwitch() = 0;
    virtual void postPower() = 0;
    virtual void postVolumeUp() = 0;
    virtual void postVolumeDown() = 0;
    virtual void postCopy() = 0;
    virtual void postCut() = 0;
    virtual void setDisplayPower(bool on) = 0;
    virtual void expandNotificationPanel() = 0;
    virtual void collapsePanel() = 0;
    virtual void postBackOrScreenOn(bool down) = 0;
    virtual void postTextInput(QString &text) = 0;
    virtual void requestDeviceClipboard() = 0;
    virtual void setDeviceClipboard(bool pause = true) = 0;
    virtual void clipboardPaste() = 0;
    virtual void pushFileRequest(const QString &file, const QString &devicePath = "") = 0;
    virtual void installApkRequest(const QString &apkFile) = 0;

    virtual void screenshot() = 0;
    virtual void showTouch(bool show) = 0;

    virtual bool isReversePort(quint16 port) = 0;
    virtual const QString &getSerial() = 0;

    virtual void updateScript(QString script) = 0;
    virtual bool isCurrentCustomKeymap() = 0;
    virtual void cancelActiveInputs() = 0;
};

class IDeviceManage : public QObject {
    Q_OBJECT
public:
    static IDeviceManage& getInstance();
    virtual bool connectDevice(DeviceParams params) = 0;
    virtual bool disconnectDevice(const QString &serial) = 0;
    virtual void disconnectAllDevice() = 0;
    virtual QPointer<IDevice> getDevice(const QString& serial) = 0;

signals:
    void deviceConnected(bool success, const QString& serial, const QString& deviceName, const QSize& size);
    void deviceDisconnected(QString serial);
};

}
