#ifndef GROUPCONTROLLER_H
#define GROUPCONTROLLER_H

#include <QObject>
#include <QVector>
#include <span> // <--- Tambahkan ini

#include "QtScrcpyCore.h"

class GroupController : public QObject, public qsc::DeviceObserver
{
    Q_OBJECT
public:
    static GroupController& instance();

    void updateDeviceState(const QString& serial);
    void addDevice(const QString& serial);
    void removeDevice(const QString& serial);

private:
    void onFrame(int width, int height, 
                 std::span<const uint8_t> dataY, 
                 std::span<const uint8_t> dataU, 
                 std::span<const uint8_t> dataV, 
                 int linesizeY, int linesizeU, int linesizeV) override;

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

private:
    explicit GroupController(QObject *parent = nullptr);
    bool isHost(const QString& serial);
    QSize getFrameSize(const QString& serial);

    // Runs func(serial, device) for every tracked device that isn't the
    // host and is still alive (getDevice() didn't return null). This is
    // the single shared implementation of the "skip host, look up device,
    // skip if it's gone, act on it" loop that used to be copy-pasted
    // verbatim into 20 separate override methods below -- including the
    // isHost()/getDevice() null-check sequencing, so there's exactly one
    // place that can get that sequencing wrong instead of 20.
    template <typename Func>
    void forEachControlledDevice(Func &&func);

private:
    QVector<QString> m_devices;
};

#endif // GROUPCONTROLLER_H