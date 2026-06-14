
#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <QObject>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QTimer>
#include <QMouseEvent>

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
    Controller(std::function<qint64(const QByteArray&)> sendData, QString gameScript = "", QObject *parent = Q_NULLPTR);
    virtual ~Controller();

    void postControlMsg(ControlMsg *controlMsg);
    void recvDeviceMsg(DeviceMsg *deviceMsg);
    void test(QRect rc);
    bool sendControl(const QByteArray &buffer);

    void updateScript(QString gameScript = "");
    bool isCurrentCustomKeymap();

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

    // for input convert
    void mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize);
    void wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize);
    void keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize);

    // turn the screen on if it was off, press BACK otherwise
    // If the screen is off, it is turned on only on down
    void postBackOrScreenOn(bool down);
    void requestDeviceClipboard();
    void getDeviceClipboard(bool cut = false);
    void setDeviceClipboard(bool pause = true);
    void clipboardPaste();
    void postTextInput(QString &text);

signals:
    void grabCursor(bool grab);
    void requestSendControl(const QByteArray &buffer);

private slots:
    void processPendingMouseMove();

protected:
    bool event(QEvent *event);

private:
    void postKeyCodeClick(AndroidKeycode keycode);

private:
    QPointer<Receiver> m_receiver;
    QPointer<InputConvertBase> m_inputConvert;
    std::function<qint64(const QByteArray&)> m_sendData = Q_NULLPTR;
    QTimer m_mouseTimer;
    bool m_hasPendingMouseMove = false;
    
    // Menyimpan state terakhir mouse
    QPointF m_lastMouseLocalPos;
    QPointF m_lastMouseWindowPos;
    QPointF m_lastMouseScreenPos;
    Qt::MouseButtons m_lastMouseButtons;
    Qt::KeyboardModifiers m_lastMouseModifiers;
    QSize m_lastFrameSize;
    QSize m_lastShowSize;
};

#endif // CONTROLLER_H
