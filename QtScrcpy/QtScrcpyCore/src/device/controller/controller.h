#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QTimer>
#include <QMouseEvent>
#include <atomic>
#include <functional>
#include <mutex>

#include "adbprocess.h"
#include "inputconvertbase.h"
#include "devicemsg.h"

class QTcpSocket;
class Receiver;
class InputConvertBase;
class DeviceMsg;
class ControlMsg;
class VideoSocket;

class Controller : public QObject
{
    Q_OBJECT
public:
    explicit Controller(std::function<qint64(const QByteArray&)> sendData,
               QString gameScript = "",
               QObject *parent = Q_NULLPTR);
    ~Controller() override;

    void postControlMsg(ControlMsg *controlMsg);
    void recvDeviceMsg(DeviceMsg *deviceMsg);
    void test(QRect rc);
    bool sendControl(const QByteArray &buffer);
    bool sendControl(const ControlMsg &message);

    void updateScript(QString gameScript = "");
    bool isCurrentCustomKeymap();
    void cancelActiveInputs();

    void postGoBack();
    void postGoHome();
    void postGoMenu();
    void postAppSwitch();
    void postPower();
    void postVolumeUp();
    void postVolumeDown();
    void copy();
    void cut();
    void expandNotificationPanel();
    void collapsePanel();
    void setDisplayPower(bool on);

    void mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize);
    void relativeMouseMoveEvent(const QPointF &delta, const QSize &frameSize, const QSize &showSize);
    void wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize);
    void keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize);

    void postBackOrScreenOn(bool down);
    void requestDeviceClipboard();
    void getDeviceClipboard(bool cut = false);
    void setDeviceClipboard(bool pause = true);
    void clipboardPaste();
    void postTextInput(QString &text);

signals:
    void grabCursor(bool grab);

private slots:
    void flushNetworkBuffer();

protected:
    bool event(QEvent *event) override;

private:
    void postKeyCodeClick(AndroidKeycode keycode);
    void scheduleNetworkFlush(int delayMs = 0);
    bool sendControlBytes(const char *data, int size);

private:
    QPointer<Receiver> m_receiver;
    QPointer<InputConvertBase> m_inputConvert;
    std::function<qint64(const QByteArray&)> m_sendData = Q_NULLPTR;

    QTimer m_networkTimer;
    QByteArray m_sendBuffer;
    QByteArray m_flushBuffer;
    std::mutex m_bufferMutex;
    bool m_flushInProgress = false;
    std::atomic_bool m_stopping{false};
};

#endif // CONTROLLER_H
