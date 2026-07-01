#ifndef DEVICE_H
#define DEVICE_H

#include <QElapsedTimer>
#include <QPointer>
#include <QTime>
#include <atomic>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <span>
#include <utility>
#include <vector>

#include "QtScrcpyCore.h"
#include "decoder/decoder.h"
#include "decoder/videobuffer.h"

class QMouseEvent;
class QWheelEvent;
class QKeyEvent;
class Recorder;
class Server;
class FileHandler;
class Demuxer;
class VideoForm;
class Controller;
struct AVFrame;
struct AVPacket;

namespace qsc {

class Device : public IDevice
{
    Q_OBJECT
public:
    explicit Device(DeviceParams params, QObject *parent = nullptr);
    ~Device() override;

    Decoder* decoder() const { return m_decoder.get(); }

    void setUserData(void* data) override;
    void* getUserData() override;

    void registerDeviceObserver(DeviceObserver* observer) override;
    void deRegisterDeviceObserver(DeviceObserver* observer) override;
    void registerFrameSink(FrameSink* sink) override;
    void deRegisterFrameSink(FrameSink* sink) override;
    void setFpsCounterEnabled(bool enabled) override
    {
        if (m_decoder) {
            if (VideoBuffer *buffer = m_decoder->videoBuffer()) {
                buffer->setFpsCounterEnabled(enabled);
            }
        }
    }

    [[nodiscard]] bool connectDevice() override;
    void disconnectDevice() override;

    void mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize) override;
    void wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize) override;
    void keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize) override;

    void postGoBack() override;
    void postGoHome() override;
    void postGoMenu() override;
    void postAppSwitch() override;
    void postPower() override;
    void postVolumeUp() override;
    void postVolumeDown() override;
    void postCopy() override;
    void postCut() override;
    void setDisplayPower(bool on) override;
    void expandNotificationPanel() override;
    void collapsePanel() override;
    void postBackOrScreenOn(bool down) override;
    void postTextInput(QString &text) override;
    void requestDeviceClipboard() override;
    void setDeviceClipboard(bool pause = true) override;
    void clipboardPaste() override;
    void pushFileRequest(const QString &file, const QString &devicePath = "") override;
    void installApkRequest(const QString &apkFile) override;

    void screenshot() override;
    void showTouch(bool show) override;

    bool isReversePort(quint16 port) override;
    const QString &getSerial() override;

    void updateScript(QString script) override;
    bool isCurrentCustomKeymap() override;
    void cancelActiveInputs() override;

private:
    void initSignals();
    bool saveFrame(int width, int height, uint8_t* dataRGB32);

    template <typename Callback>
    void forEachObserver(Callback&& callback) const
    {
        std::shared_lock<std::shared_mutex> lock(m_observerMutex);
        for (DeviceObserver* observer : m_deviceObservers) {
            if (observer) {
                std::invoke(std::forward<Callback>(callback), *observer);
            }
        }
    }

private:
    bool m_serverStartSuccess = false;
    std::atomic_bool m_disconnecting{false};

    // scrcpy-server >= 4.0: the video size arrives later than the device
    // name (via the demuxer's first session packet), so we stash the name
    // here until both are available and deviceConnected() can be emitted.
    QString m_pendingDeviceName;
    // Guards the one-time recorder setup, handled on the demuxer thread
    // (Qt::DirectConnection).
    bool m_firstSessionInfoHandled = false;
    // Guards the one-time deviceConnected() emission, handled on this
    // object's own thread (auto connection).
    bool m_deviceConnectedEmitted = false;

    std::unique_ptr<Server> m_server;
    std::unique_ptr<Decoder> m_decoder;
    std::unique_ptr<Controller> m_controller;
    std::unique_ptr<FileHandler> m_fileHandler;
    std::unique_ptr<Demuxer> m_stream;
    std::unique_ptr<Recorder> m_recorder;

    QElapsedTimer m_startTimeCount;
    DeviceParams m_params;

    mutable std::shared_mutex m_observerMutex;
    std::vector<DeviceObserver*> m_deviceObservers;

    mutable std::shared_mutex m_frameSinkMutex;
    FrameSink* m_frameSink = nullptr;

    void *m_userData = nullptr;
};

}

#endif // DEVICE_H
