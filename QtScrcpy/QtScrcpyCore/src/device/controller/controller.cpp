#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QMetaObject>
#include <QThread>
#include <array>
#include <span>
#include <utility>

#include "controller.h"
#include "controlmsg.h"
#include "inputconvertgame.h"
#include "qtscrcpytelemetry.h"
#include "receiver.h"
#include "videosocket.h"

namespace {

[[nodiscard]] bool isClipboardControlType(ControlMsg::ControlMsgType type) noexcept
{
    return type == ControlMsg::CMT_INJECT_TEXT ||
           type == ControlMsg::CMT_GET_CLIPBOARD ||
           type == ControlMsg::CMT_SET_CLIPBOARD;
}

[[nodiscard]] bool isGuiThread() noexcept
{
    const QCoreApplication *app = QCoreApplication::instance();
    return app && QThread::currentThread() == app->thread();
}

} // namespace

Controller::Controller(std::function<qint64(const QByteArray&)> sendData,
                       QString gameScript,
                       QObject *parent)
    : QObject(parent)
    , m_sendData(std::move(sendData))
{
    m_sendBuffer.reserve(4096);
    m_flushBuffer.reserve(4096);

    m_receiver = new Receiver(this);
    Q_ASSERT(m_receiver);
    updateScript(std::move(gameScript));

    m_networkTimer.setSingleShot(true);
    m_networkTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_networkTimer, &QTimer::timeout,
            this, &Controller::flushNetworkBuffer);
}

Controller::~Controller()
{
    m_stopping.store(true, std::memory_order_release);
    m_networkTimer.stop();
    flushNetworkBuffer();
}

void Controller::postControlMsg(ControlMsg *controlMsg)
{
    if (!controlMsg) return;
    (void)sendControl(*controlMsg);
    delete controlMsg;
}

void Controller::scheduleNetworkFlush(int delayMs)
{
    if (m_stopping.load(std::memory_order_acquire)) return;
    const int safeDelay = qMax(0, delayMs);

    if (QThread::currentThread() == thread()) {
        if (!m_networkTimer.isActive() ||
            m_networkTimer.remainingTime() > safeDelay) {
            m_networkTimer.start(safeDelay);
        }
        return;
    }

    QMetaObject::invokeMethod(this, [this, safeDelay]() {
        if (m_stopping.load(std::memory_order_acquire)) return;
        if (!m_networkTimer.isActive() ||
            m_networkTimer.remainingTime() > safeDelay) {
            m_networkTimer.start(safeDelay);
        }
    }, Qt::QueuedConnection);
}

void Controller::flushNetworkBuffer()
{
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        if (m_flushInProgress || m_sendBuffer.isEmpty()) return;
        m_flushInProgress = true;
        m_flushBuffer.clear();
        m_flushBuffer.swap(m_sendBuffer);
    }

    qint64 written = m_sendData ? m_sendData(m_flushBuffer) : -1;
    if (written < 0) written = 0;
    if (written > m_flushBuffer.size()) written = m_flushBuffer.size();

    if (written > 0) {
        m_flushBuffer.remove(0, static_cast<int>(written));
    }

    bool hasPending = false;
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        if (!m_flushBuffer.isEmpty()) {
            m_sendBuffer.prepend(m_flushBuffer);
        }
        m_flushBuffer.clear();
        m_flushInProgress = false;
        hasPending = !m_sendBuffer.isEmpty();
    }

    if (!hasPending || m_stopping.load(std::memory_order_acquire)) return;
    scheduleNetworkFlush(written > 0 ? 0 : 10);
}

void Controller::recvDeviceMsg(DeviceMsg *deviceMsg)
{
    if (m_receiver) m_receiver->recvDeviceMsg(deviceMsg);
}

void Controller::test(QRect rc)
{
    ControlMsg controlMsg(ControlMsg::CMT_INJECT_TOUCH);
    controlMsg.setInjectTouchMsgData(
        static_cast<quint64>(POINTER_ID_MOUSE),
        AMOTION_EVENT_ACTION_DOWN,
        AMOTION_EVENT_BUTTON_PRIMARY,
        AMOTION_EVENT_BUTTON_PRIMARY,
        rc,
        1.0f);
    (void)sendControl(controlMsg);
}

void Controller::updateScript(QString gameScript)
{
    delete m_inputConvert;
    m_inputConvert = nullptr;

    if (!gameScript.isEmpty()) {
        auto *convertGame = new InputConvertGame(this);
        convertGame->loadKeyMap(gameScript);
        m_inputConvert = convertGame;
    } else {
        m_inputConvert = new InputConvertNormal(this);
    }

    Q_ASSERT(m_inputConvert);
    connect(m_inputConvert, &InputConvertBase::grabCursor,
            this, &Controller::grabCursor);
}

bool Controller::isCurrentCustomKeymap()
{
    return m_inputConvert && m_inputConvert->isCurrentCustomKeymap();
}

void Controller::postBackOrScreenOn(bool down)
{
    ControlMsg controlMsg(ControlMsg::CMT_BACK_OR_SCREEN_ON);
    controlMsg.setBackOrScreenOnData(down);
    (void)sendControl(controlMsg);
}

void Controller::postGoHome() { postKeyCodeClick(AKEYCODE_HOME); }
void Controller::postGoMenu() { postKeyCodeClick(AKEYCODE_MENU); }
void Controller::postGoBack() { postKeyCodeClick(AKEYCODE_BACK); }
void Controller::postAppSwitch() { postKeyCodeClick(AKEYCODE_APP_SWITCH); }
void Controller::postPower() { postKeyCodeClick(AKEYCODE_POWER); }
void Controller::postVolumeUp() { postKeyCodeClick(AKEYCODE_VOLUME_UP); }
void Controller::postVolumeDown() { postKeyCodeClick(AKEYCODE_VOLUME_DOWN); }

void Controller::copy()
{
    if (qsc::telemetryEnabled()) {
        qInfo() << "[Telemetry][Clipboard] action=copy"
                << "thread=" << qsc::telemetryThreadId()
                << "guiThread=" << isGuiThread();
    }
    getDeviceClipboard(false);
}

void Controller::cut()
{
    if (qsc::telemetryEnabled()) {
        qInfo() << "[Telemetry][Clipboard] action=cut"
                << "thread=" << qsc::telemetryThreadId()
                << "guiThread=" << isGuiThread();
    }
    getDeviceClipboard(true);
}

void Controller::expandNotificationPanel()
{
    ControlMsg controlMsg(ControlMsg::CMT_EXPAND_NOTIFICATION_PANEL);
    (void)sendControl(controlMsg);
}

void Controller::collapsePanel()
{
    ControlMsg controlMsg(ControlMsg::CMT_COLLAPSE_PANELS);
    (void)sendControl(controlMsg);
}

void Controller::requestDeviceClipboard()
{
    ControlMsg controlMsg(ControlMsg::CMT_GET_CLIPBOARD);
    (void)sendControl(controlMsg);
}

void Controller::getDeviceClipboard(bool cut)
{
    ControlMsg controlMsg(ControlMsg::CMT_GET_CLIPBOARD);
    controlMsg.setGetClipboardMsgData(
        cut ? ControlMsg::GCCK_CUT : ControlMsg::GCCK_COPY);
    (void)sendControl(controlMsg);
}

void Controller::setDeviceClipboard(bool pause)
{
    const QString text = QApplication::clipboard()->text();
    if (qsc::telemetryEnabled()) {
        qInfo() << "[Telemetry][Clipboard] action=set"
                << "paste=" << pause
                << "utf8Bytes=" << text.toUtf8().size()
                << "thread=" << qsc::telemetryThreadId()
                << "guiThread=" << isGuiThread();
    }

    ControlMsg controlMsg(ControlMsg::CMT_SET_CLIPBOARD);
    controlMsg.setSetClipboardMsgData(text, pause);
    (void)sendControl(controlMsg);
}

void Controller::clipboardPaste()
{
    QString text = QApplication::clipboard()->text();
    if (qsc::telemetryEnabled()) {
        qInfo() << "[Telemetry][Clipboard] action=legacy-inject-text"
                << "utf8Bytes=" << text.toUtf8().size()
                << "thread=" << qsc::telemetryThreadId()
                << "guiThread=" << isGuiThread();
    }
    postTextInput(text);
}

void Controller::postTextInput(QString &text)
{
    ControlMsg controlMsg(ControlMsg::CMT_INJECT_TEXT);
    controlMsg.setInjectTextMsgData(text);
    (void)sendControl(controlMsg);
}

void Controller::setDisplayPower(bool on)
{
    ControlMsg controlMsg(ControlMsg::CMT_SET_DISPLAY_POWER);
    controlMsg.setDisplayPowerData(on);
    (void)sendControl(controlMsg);
}

void Controller::mouseEvent(const QMouseEvent *from,
                            const QSize &frameSize,
                            const QSize &showSize)
{
    if (m_inputConvert) m_inputConvert->mouseEvent(from, frameSize, showSize);
}

void Controller::wheelEvent(const QWheelEvent *from,
                            const QSize &frameSize,
                            const QSize &showSize)
{
    if (m_inputConvert) m_inputConvert->wheelEvent(from, frameSize, showSize);
}

void Controller::keyEvent(const QKeyEvent *from,
                          const QSize &frameSize,
                          const QSize &showSize)
{
    if (m_inputConvert) m_inputConvert->keyEvent(from, frameSize, showSize);
}

bool Controller::event(QEvent *event)
{
    if (event && static_cast<ControlMsg::Type>(event->type()) == ControlMsg::Control) {
        if (auto *controlMsg = dynamic_cast<ControlMsg *>(event)) {
            (void)sendControl(*controlMsg);
        }
        return true;
    }
    return QObject::event(event);
}

bool Controller::sendControl(const ControlMsg &message)
{
    std::array<char, ControlMsg::INLINE_SERIALIZED_CAPACITY> inlineBuffer{};
    const int size = message.serializeTo(std::span<char>(inlineBuffer));
    if (size > 0) {
        if (qsc::telemetryEnabled() && isClipboardControlType(message.type())) {
            qInfo() << "[Telemetry][Control] serialize"
                    << "type=" << static_cast<int>(message.type())
                    << "bytes=" << size
                    << "path=inline"
                    << "thread=" << qsc::telemetryThreadId();
        }
        return sendControlBytes(inlineBuffer.data(), size);
    }

    const QByteArray serialized = message.serializeData();
    if (qsc::telemetryEnabled() && isClipboardControlType(message.type())) {
        qInfo() << "[Telemetry][Control] serialize"
                << "type=" << static_cast<int>(message.type())
                << "bytes=" << serialized.size()
                << "path=dynamic"
                << "thread=" << qsc::telemetryThreadId();
    }
    return sendControl(serialized);
}

bool Controller::sendControl(const QByteArray &buffer)
{
    return sendControlBytes(buffer.constData(), buffer.size());
}

bool Controller::sendControlBytes(const char *data, int size)
{
    if (!data || size <= 0 || m_stopping.load(std::memory_order_acquire)) {
        return false;
    }

    bool flushImmediately = false;
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        if (m_stopping.load(std::memory_order_relaxed)) return false;

        flushImmediately = m_sendBuffer.isEmpty() &&
                           !m_flushInProgress &&
                           QThread::currentThread() == thread();
        m_sendBuffer.append(data, size);
    }

    if (flushImmediately) {
        flushNetworkBuffer();
    } else {
        scheduleNetworkFlush(0);
    }
    return true;
}

void Controller::postKeyCodeClick(AndroidKeycode keycode)
{
    ControlMsg down(ControlMsg::CMT_INJECT_KEYCODE);
    down.setInjectKeycodeMsgData(
        AKEY_EVENT_ACTION_DOWN, keycode, 0, AMETA_NONE);
    (void)sendControl(down);

    ControlMsg up(ControlMsg::CMT_INJECT_KEYCODE);
    up.setInjectKeycodeMsgData(
        AKEY_EVENT_ACTION_UP, keycode, 0, AMETA_NONE);
    (void)sendControl(up);
}
