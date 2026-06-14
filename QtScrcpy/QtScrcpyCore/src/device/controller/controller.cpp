#include <QApplication>
#include <QClipboard>

#include "controller.h"
#include "controlmsg.h"
#include "inputconvertgame.h"
#include "receiver.h"
#include "videosocket.h"

Controller::Controller(std::function<qint64(const QByteArray&)> sendData, QString gameScript, QObject *parent)
    : QObject(parent)
    , m_sendData(sendData)
{
    m_receiver = new Receiver(this);
    Q_ASSERT(m_receiver);

    updateScript(gameScript);
    
    // 1. Hubungkan sinyal Queued untuk bypass QEvent overhead
    connect(this, &Controller::requestSendControl, this, &Controller::sendControl, Qt::QueuedConnection);

    // 2. Konfigurasi Mouse Throttler (125Hz = 8ms)
    m_mouseTimer.setInterval(8);
    m_mouseTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_mouseTimer, &QTimer::timeout, this, &Controller::processPendingMouseMove);
    m_mouseTimer.start();
}

Controller::~Controller() {
    m_mouseTimer.stop();
}

void Controller::postControlMsg(ControlMsg *controlMsg)
{
    if (controlMsg) {
        // Serialisasi datanya SEKARANG. QByteArray menggunakan memory yang aman
        // dan dikelola otomatis oleh Qt (Implicit Sharing).
        QByteArray data = controlMsg->serializeData();
        
        // Kirim via signal-slot QueuedConnection
        emit requestSendControl(data);
        
        // Langsung HAPUS objeknya di sini. Kita tidak lagi butuh QEvent.
        // Jika kamu merombak kelas lain nanti, objek ControlMsg bahkan bisa
        // dialokasikan di stack (tanpa "new") untuk performa maksimal.
        delete controlMsg; 
    }
}

void Controller::recvDeviceMsg(DeviceMsg *deviceMsg)
{
    if (!m_receiver) {
        return;
    }

    m_receiver->recvDeviceMsg(deviceMsg);
}

void Controller::test(QRect rc)
{
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_INJECT_TOUCH);
    controlMsg->setInjectTouchMsgData(
        static_cast<quint64>(POINTER_ID_MOUSE), AMOTION_EVENT_ACTION_DOWN, AMOTION_EVENT_BUTTON_PRIMARY, AMOTION_EVENT_BUTTON_PRIMARY, rc, 1.0f);
    postControlMsg(controlMsg);
}

void Controller::updateScript(QString gameScript)
{
    if (m_inputConvert) {
        delete m_inputConvert;
    }
    if (!gameScript.isEmpty()) {
        InputConvertGame *convertgame = new InputConvertGame(this);
        convertgame->loadKeyMap(gameScript);
        m_inputConvert = convertgame;
    } else {
        m_inputConvert = new InputConvertNormal(this);
    }
    Q_ASSERT(m_inputConvert);
    connect(m_inputConvert, &InputConvertBase::grabCursor, this, &Controller::grabCursor);
}

bool Controller::isCurrentCustomKeymap()
{
    if (!m_inputConvert) {
        return false;
    }

    return m_inputConvert->isCurrentCustomKeymap();
}

void Controller::postBackOrScreenOn(bool down)
{
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_BACK_OR_SCREEN_ON);
    controlMsg->setBackOrScreenOnData(down);
    if (!controlMsg) {
        return;
    }
    postControlMsg(controlMsg);
}

void Controller::postGoHome()
{
    postKeyCodeClick(AKEYCODE_HOME);
}

void Controller::postGoMenu()
{
    postKeyCodeClick(AKEYCODE_MENU);
}

void Controller::postGoBack()
{
    postKeyCodeClick(AKEYCODE_BACK);
}

void Controller::postAppSwitch()
{
    postKeyCodeClick(AKEYCODE_APP_SWITCH);
}

void Controller::postPower()
{
    postKeyCodeClick(AKEYCODE_POWER);
}

void Controller::postVolumeUp()
{
    postKeyCodeClick(AKEYCODE_VOLUME_UP);
}

void Controller::postVolumeDown()
{
    postKeyCodeClick(AKEYCODE_VOLUME_DOWN);
}

void Controller::copy()
{
    postKeyCodeClick(AKEYCODE_COPY);
}

void Controller::cut()
{
    postKeyCodeClick(AKEYCODE_CUT);
}

void Controller::expandNotificationPanel()
{
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_EXPAND_NOTIFICATION_PANEL);
    if (!controlMsg) {
        return;
    }
    postControlMsg(controlMsg);
}

void Controller::collapsePanel()
{
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_COLLAPSE_PANELS);
    if (!controlMsg) {
        return;
    }
    postControlMsg(controlMsg);
}

void Controller::requestDeviceClipboard()
{
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_GET_CLIPBOARD);
    if (!controlMsg) {
        return;
    }
    postControlMsg(controlMsg);
}

void Controller::getDeviceClipboard(bool cut)
{
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_GET_CLIPBOARD);
    if (!controlMsg) {
        return;
    }
    ControlMsg::GetClipboardCopyKey copyKey = cut ? ControlMsg::GCCK_CUT : ControlMsg::GCCK_COPY;
    controlMsg->setGetClipboardMsgData(copyKey);
    postControlMsg(controlMsg);
}

void Controller::setDeviceClipboard(bool pause)
{
    QClipboard *board = QApplication::clipboard();
    QString text = board->text();
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_SET_CLIPBOARD);
    if (!controlMsg) {
        return;
    }
    controlMsg->setSetClipboardMsgData(text, pause);
    postControlMsg(controlMsg);
}

void Controller::clipboardPaste()
{
    QClipboard *board = QApplication::clipboard();
    QString text = board->text();
    postTextInput(text);
}

void Controller::postTextInput(QString &text)
{
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_INJECT_TEXT);
    if (!controlMsg) {
        return;
    }
    controlMsg->setInjectTextMsgData(text);
    postControlMsg(controlMsg);
}

void Controller::setDisplayPower(bool on)
{
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_SET_DISPLAY_POWER);
    if (!controlMsg) {
        return;
    }
    controlMsg->setDisplayPowerData(on);
    postControlMsg(controlMsg);
}

void Controller::mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (!m_inputConvert) {
        return;
    }

    // Jika ini hanya pergerakan (Move), jangan buat ControlMsg!
    // Cukup simpan koordinat terakhirnya.
    if (from->type() == QEvent::MouseMove) {
        m_lastMouseLocalPos = from->localPos();
        m_lastMouseWindowPos = from->windowPos();
        m_lastMouseScreenPos = from->screenPos();
        m_lastMouseButtons = from->buttons();
        m_lastMouseModifiers = from->modifiers();
        m_lastFrameSize = frameSize;
        m_lastShowSize = showSize;
        
        m_hasPendingMouseMove = true;
        return; // Hentikan eksekusi di sini
    }
    
    // Jika ini adalah Klik (Press/Release) atau Scroll, kirim INSTAN
    // agar terasa sangat responsif dan tidak delay.
    m_inputConvert->mouseEvent(from, frameSize, showSize);
}

void Controller::processPendingMouseMove()
{
    // Jika tidak ada pergerakan sejak tick terakhir, diam saja.
    if (!m_hasPendingMouseMove) {
        return;
    }
    
    // Rakit ulang QMouseEvent dari state terakhir
    QMouseEvent fakeEvent(
        QEvent::MouseMove, 
        m_lastMouseLocalPos, 
        m_lastMouseWindowPos, 
        m_lastMouseScreenPos, 
        Qt::NoButton, 
        m_lastMouseButtons, 
        m_lastMouseModifiers
    );
    
    // Tembakkan ke converter
    if (m_inputConvert) {
        m_inputConvert->mouseEvent(&fakeEvent, m_lastFrameSize, m_lastShowSize);
    }
    
    // Reset status
    m_hasPendingMouseMove = false;
}

void Controller::wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (m_inputConvert) {
        m_inputConvert->wheelEvent(from, frameSize, showSize);
    }
}

void Controller::keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (m_inputConvert) {
        m_inputConvert->keyEvent(from, frameSize, showSize);
    }
}

bool Controller::event(QEvent *event)
{
    // Bagian ini sekarang hampir tidak akan pernah dipanggil lagi untuk ControlMsg, 
    // karena kita sudah mem-bypass QCoreApplication::postEvent.
    // Tetapi dibiarkan saja sebagai fallback.
    if (event && static_cast<ControlMsg::Type>(event->type()) == ControlMsg::Control) {
        ControlMsg *controlMsg = dynamic_cast<ControlMsg *>(event);
        if (controlMsg) {
            sendControl(controlMsg->serializeData());
        }
        return true;
    }
    return QObject::event(event);
}

bool Controller::sendControl(const QByteArray &buffer)
{
    if (buffer.isEmpty()) {
        return false;
    }
    qint32 len = 0;
    if (m_sendData) {
        len = static_cast<qint32>(m_sendData(buffer));
    }
    return len == buffer.length() ? true : false;
}

void Controller::postKeyCodeClick(AndroidKeycode keycode)
{
    ControlMsg *controlEventDown = new ControlMsg(ControlMsg::CMT_INJECT_KEYCODE);
    if (!controlEventDown) {
        return;
    }
    controlEventDown->setInjectKeycodeMsgData(AKEY_EVENT_ACTION_DOWN, keycode, 0, AMETA_NONE);
    postControlMsg(controlEventDown);

    ControlMsg *controlEventUp = new ControlMsg(ControlMsg::CMT_INJECT_KEYCODE);
    if (!controlEventUp) {
        return;
    }
    controlEventUp->setInjectKeycodeMsgData(AKEY_EVENT_ACTION_UP, keycode, 0, AMETA_NONE);
    postControlMsg(controlEventUp);
}
