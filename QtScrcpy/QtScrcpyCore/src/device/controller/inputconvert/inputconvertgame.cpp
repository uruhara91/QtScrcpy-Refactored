#include <QDebug>
#include <QCursor>
#include <QGuiApplication>
#include <QTimer>
#include <QTime>
#include <QWidget>
#include <QRandomGenerator>
#include <QElapsedTimer>
#include <random>

#include "inputconvertgame.h"
#include "../controller.h"

// Tidak lagi digunakan untuk boundary check, tapi kita tetap define
#define CURSOR_POS_CHECK 50

InputConvertGame::InputConvertGame(Controller *controller) : InputConvertNormal(controller) {
    // Steer Wheel Timer
    m_ctrlSteerWheel.delayData.timer = new QTimer(this);
    m_ctrlSteerWheel.delayData.timer->setSingleShot(true);
    connect(m_ctrlSteerWheel.delayData.timer, &QTimer::timeout, this, &InputConvertGame::onSteerWheelTimer);

    // Drag Timer (Pre-allocated, hapus 'new QTimer' dari processKeyDrag)
    m_dragDelayData.timer = new QTimer(this);
    m_dragDelayData.timer->setSingleShot(true);
    connect(m_dragDelayData.timer, &QTimer::timeout, this, &InputConvertGame::onDragTimer);
}

InputConvertGame::~InputConvertGame() {}

void InputConvertGame::mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize)
{
    // Handle switch key
    if (m_keyMap.isSwitchOnKeyboard() == false && m_keyMap.getSwitchKey() == static_cast<int>(from->button())) {
        if (from->type() != QEvent::MouseButtonPress) {
            return;
        }
        if (!switchGameMap()) {
            m_needBackMouseMove = false;
        }
        return;
    }

    if (!m_needBackMouseMove && m_gameMap) {
        updateSize(frameSize, showSize);
        // mouse move
        if (m_keyMap.isValidMouseMoveMap()) {
            if (processMouseMove(from)) {
                return;
            }
        }
        // mouse click
        if (processMouseClick(from)) {
            return;
        }
    }
    InputConvertNormal::mouseEvent(from, frameSize, showSize);
}

void InputConvertGame::wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (m_gameMap) {
        updateSize(frameSize, showSize);
    } else {
        InputConvertNormal::wheelEvent(from, frameSize, showSize);
    }
}

void InputConvertGame::keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize)
{
    // Handle switch key
    if (m_keyMap.isSwitchOnKeyboard() && m_keyMap.getSwitchKey() == from->key()) {
        if (QEvent::KeyPress != from->type()) {
            return;
        }
        if (!switchGameMap()) {
            m_needBackMouseMove = false;
        }
        return;
    }

    const KeyMap::KeyMapNode &node = m_keyMap.getKeyMapNodeKey(from->key());
    // Handle special keys: keys that can release the mouse
    if (m_needBackMouseMove && KeyMap::KMT_CLICK == node.type && node.data.click.switchMap) {
        updateSize(frameSize, showSize);
        // Qt::Key_Tab Qt::Key_M for PUBG mobile
        processKeyClick(node.data.click.keyNode.pos, false, node.data.click.switchMap, from);
        return;
    }

    if (m_gameMap) {
        updateSize(frameSize, showSize);
        if (!from || from->isAutoRepeat()) {
            return;
        }

        // small eyes
        if (m_keyMap.isValidMouseMoveMap() && from->key() == m_keyMap.getMouseMoveMap().data.mouseMove.smallEyes.key) {
            m_ctrlMouseMove.smallEyes = (QEvent::KeyPress == from->type());

            if (QEvent::KeyPress == from->type()) {
                m_processMouseMove = false;
                int delay = 30;
                QTimer::singleShot(delay, this, [this]() { mouseMoveStopTouch(); });
                QTimer::singleShot(delay * 2, this, [this]() {
                    mouseMoveStartTouch(nullptr);
                    m_processMouseMove = true;
                });

                stopMouseMoveTimer();
            } else {
                mouseMoveStopTouch();
                mouseMoveStartTouch(nullptr);
            }
            return;
        }

        switch (node.type) {
        // Handle steer wheel
        case KeyMap::KMT_STEER_WHEEL:
            processSteerWheel(node, from);
            return;
        // Handle normal click
        case KeyMap::KMT_CLICK:
            processKeyClick(node.data.click.keyNode.pos, false, node.data.click.switchMap, from);
            processAndroidKey(node.data.click.keyNode.androidKey, from);
            return;
        case KeyMap::KMT_CLICK_TWICE:
            processKeyClick(node.data.clickTwice.keyNode.pos, true, false, from);
            processAndroidKey(node.data.clickTwice.keyNode.androidKey, from);
            return;
        case KeyMap::KMT_CLICK_MULTI:
            processKeyClickMulti(node.data.clickMulti.keyNode.delayClickNodes, node.data.clickMulti.keyNode.delayClickNodesCount, from);
            return;
        case KeyMap::KMT_DRAG:
            processKeyDrag(node.data.drag.keyNode.pos, node.data.drag.keyNode.extendPos,
                         node.data.drag.startDelay, node.data.drag.dragSpeed, from);
            return;
        case KeyMap::KMT_ANDROID_KEY:
            processAndroidKey(node.data.androidKey.keyNode.androidKey, from);
        default:
            break;
        }
    } else {
        InputConvertNormal::keyEvent(from, frameSize, showSize);
    }
}

bool InputConvertGame::isCurrentCustomKeymap()
{
    return m_gameMap;
}

void InputConvertGame::loadKeyMap(const QString &json)
{
    m_keyMap.loadKeyMap(json);
}

void InputConvertGame::updateSize(const QSize &frameSize, const QSize &showSize)
{
    if (showSize != m_showSize) {
        if (m_gameMap && m_keyMap.isValidMouseMoveMap()) {
            // Force grab cursor to prevent escaping
            emit grabCursor(true);
        }
    }
    m_frameSize = frameSize;
    m_showSize = showSize;
}

void InputConvertGame::sendTouchDownEvent(int id, QPointF pos)
{
    sendTouchEvent(id, pos, AMOTION_EVENT_ACTION_DOWN);
}

void InputConvertGame::sendTouchMoveEvent(int id, QPointF pos)
{
    sendTouchEvent(id, pos, AMOTION_EVENT_ACTION_MOVE);
}

void InputConvertGame::sendTouchUpEvent(int id, QPointF pos)
{
    sendTouchEvent(id, pos, AMOTION_EVENT_ACTION_UP);
}

void InputConvertGame::sendTouchEvent(int id, QPointF pos, AndroidMotioneventAction action)
{
    if (0 > id || MULTI_TOUCH_MAX_NUM - 1 < id) {
        Q_ASSERT(0);
        return;
    }
    
    QPoint absolutePos = calcFrameAbsolutePos(pos).toPoint();
    static QPoint lastAbsolutePos = absolutePos;
    if (AMOTION_EVENT_ACTION_MOVE == action && lastAbsolutePos == absolutePos) {
        return;
    }
    lastAbsolutePos = absolutePos;

    // OPTIMASI: Alokasi objek di Stack, jauh lebih cepat dari Heap (new)
    ControlMsg controlMsg(ControlMsg::CMT_INJECT_TOUCH);
    
    controlMsg.setInjectTouchMsgData(
        static_cast<quint64>(id),
        action,
        static_cast<AndroidMotioneventButtons>(0),
        static_cast<AndroidMotioneventButtons>(0),
        QRect(absolutePos, m_frameSize),
        AMOTION_EVENT_ACTION_DOWN == action ? 1.0f : 0.0f);
        
    // OPTIMASI: Langsung serialize dan kirim ke socket via controller, 
    // Bypass QCoreApplication::postEvent() !
    if (m_controller) {
        m_controller->sendControl(controlMsg.serializeData());
    }
}

void InputConvertGame::sendKeyEvent(AndroidKeyeventAction action, AndroidKeycode keyCode) {
    // OPTIMASI: Alokasi Stack
    ControlMsg controlMsg(ControlMsg::CMT_INJECT_KEYCODE);

    controlMsg.setInjectKeycodeMsgData(action, keyCode, 0, AMETA_NONE);
    
    // Langsung tembak ke socket
    if (m_controller) {
        m_controller->sendControl(controlMsg.serializeData());
    }
}

QPointF InputConvertGame::calcFrameAbsolutePos(QPointF relativePos)
{
    QPointF absolutePos;
    absolutePos.setX(m_frameSize.width() * relativePos.x());
    absolutePos.setY(m_frameSize.height() * relativePos.y());
    return absolutePos;
}

QPointF InputConvertGame::calcScreenAbsolutePos(QPointF relativePos)
{
    QPointF absolutePos;
    absolutePos.setX(m_showSize.width() * relativePos.x());
    absolutePos.setY(m_showSize.height() * relativePos.y());
    return absolutePos;
}

int InputConvertGame::attachTouchID(int key)
{
    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; i++) {
        if (0 == m_multiTouchID[i]) {
            m_multiTouchID[i] = key;
            return i;
        }
    }
    return -1;
}

void InputConvertGame::detachTouchID(int key)
{
    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; i++) {
        if (key == m_multiTouchID[i]) {
            m_multiTouchID[i] = 0;
            return;
        }
    }
}

int InputConvertGame::getTouchID(int key)
{
    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; i++) {
        if (key == m_multiTouchID[i]) {
            return i;
        }
    }
    return -1;
}

// -------- steer wheel event --------

void InputConvertGame::getDelayQueue(const QPointF& start, const QPointF& end,
                                     const double& distanceStep, const double& posStepconst,
                                     quint32 lowestTimer, quint32 highestTimer,
                                     QVector<QPointF>& pathPos, QVector<quint32>& pathTimer) {
    double x1 = start.x();
    double y1 = start.y();
    double x2 = end.x();
    double y2 = end.y();

    double dx = x2 - x1;
    double dy = y2 - y1;
    double e = (fabs(dx) > fabs(dy)) ? fabs(dx) : fabs(dy);
    e /= distanceStep;
    dx /= e;
    dy /= e;

    int steps = static_cast<int>(e);
    
    pathPos.clear();
    pathTimer.clear();

    if (steps > 0) {
        pathPos.reserve(steps);
        pathTimer.reserve(steps);

        // OPTIMASI: Thread-local PRNG (sangat ringan dan lock-free)
        thread_local std::mt19937 gen(std::random_device{}());
        std::uniform_real_distribution<double> posDist(-posStepconst, posStepconst);
        std::uniform_int_distribution<quint32> timeDist(lowestTimer, highestTimer);

        for(int i = 1; i <= steps; i++) {
            // Kita terapkan jitter (noise) ke pergerakan kursor
            QPointF pos(x1 + posDist(gen), y1 + posDist(gen));
            
            pathPos.append(pos);
            pathTimer.append(timeDist(gen));
            
            x1 += dx;
            y1 += dy;
        }
    }
}

void InputConvertGame::onSteerWheelTimer() {
    if(m_ctrlSteerWheel.delayData.stepIndex >= m_ctrlSteerWheel.delayData.pathPos.size()) {
        return;
    }
    
    int id = getTouchID(m_ctrlSteerWheel.touchKey);
    if (id == -1) return;

    m_ctrlSteerWheel.delayData.currentPos = m_ctrlSteerWheel.delayData.pathPos[m_ctrlSteerWheel.delayData.stepIndex];
    quint32 nextTimer = m_ctrlSteerWheel.delayData.pathTimer[m_ctrlSteerWheel.delayData.stepIndex];
    
    sendTouchMoveEvent(id, m_ctrlSteerWheel.delayData.currentPos);
    
    m_ctrlSteerWheel.delayData.stepIndex++;

    if(m_ctrlSteerWheel.delayData.stepIndex >= m_ctrlSteerWheel.delayData.pathPos.size() && m_ctrlSteerWheel.delayData.pressedNum == 0) {
        sendTouchUpEvent(id, m_ctrlSteerWheel.delayData.currentPos);
        detachTouchID(m_ctrlSteerWheel.touchKey);
        return;
    }

    if(m_ctrlSteerWheel.delayData.stepIndex < m_ctrlSteerWheel.delayData.pathPos.size()) {
        m_ctrlSteerWheel.delayData.timer->start(nextTimer);
    }
}

void InputConvertGame::processSteerWheel(const KeyMap::KeyMapNode &node, const QKeyEvent *from)
{
    int key = from->key();
    bool flag = from->type() == QEvent::KeyPress;
    // identify keys
    if (key == node.data.steerWheel.up.key) {
        m_ctrlSteerWheel.pressedUp = flag;
    } else if (key == node.data.steerWheel.right.key) {
        m_ctrlSteerWheel.pressedRight = flag;
    } else if (key == node.data.steerWheel.down.key) {
        m_ctrlSteerWheel.pressedDown = flag;
    } else { // left
        m_ctrlSteerWheel.pressedLeft = flag;
    }

    // calc offset and pressed number
    QPointF offset(0.0, 0.0);
    int pressedNum = 0;
    if (m_ctrlSteerWheel.pressedUp) {
        ++pressedNum;
        offset.ry() -= node.data.steerWheel.up.extendOffset;
    }
    if (m_ctrlSteerWheel.pressedRight) {
        ++pressedNum;
        offset.rx() += node.data.steerWheel.right.extendOffset;
    }
    if (m_ctrlSteerWheel.pressedDown) {
        ++pressedNum;
        offset.ry() += node.data.steerWheel.down.extendOffset;
    }
    if (m_ctrlSteerWheel.pressedLeft) {
        ++pressedNum;
        offset.rx() -= node.data.steerWheel.left.extendOffset;
    }
    m_ctrlSteerWheel.delayData.pressedNum = pressedNum;

    // last key release and timer no active, active timer to detouch
    if (pressedNum == 0) {
        if (m_ctrlSteerWheel.delayData.timer->isActive()) {
            m_ctrlSteerWheel.delayData.timer->stop();
        }
        m_ctrlSteerWheel.delayData.pathTimer.clear();
        m_ctrlSteerWheel.delayData.pathPos.clear();
        m_ctrlSteerWheel.delayData.stepIndex = 0;

        sendTouchUpEvent(getTouchID(m_ctrlSteerWheel.touchKey), m_ctrlSteerWheel.delayData.currentPos);
        detachTouchID(m_ctrlSteerWheel.touchKey);
        return;
    }

    // process steer wheel key event
    m_ctrlSteerWheel.delayData.timer->stop();
    m_ctrlSteerWheel.delayData.pathTimer.clear();
    m_ctrlSteerWheel.delayData.pathPos.clear();
    m_ctrlSteerWheel.delayData.stepIndex = 0;

    // first press, get key and touch down
    if (pressedNum == 1 && flag) {
        m_ctrlSteerWheel.touchKey = from->key();
        int id = attachTouchID(m_ctrlSteerWheel.touchKey);
        sendTouchDownEvent(id, node.data.steerWheel.centerPos);

        getDelayQueue(node.data.steerWheel.centerPos, node.data.steerWheel.centerPos+offset,
                      0.01f, 0.002f, 2, 8,
                      m_ctrlSteerWheel.delayData.pathPos,
                      m_ctrlSteerWheel.delayData.pathTimer);
    } else {
        getDelayQueue(m_ctrlSteerWheel.delayData.currentPos, node.data.steerWheel.centerPos+offset,
                      0.01f, 0.002f, 2, 8,
                      m_ctrlSteerWheel.delayData.pathPos,
                      m_ctrlSteerWheel.delayData.pathTimer);
    }
    
    if (!m_ctrlSteerWheel.delayData.pathPos.isEmpty()) {
        m_ctrlSteerWheel.delayData.timer->start(1);
    }
    return;
}

// -------- key event --------

void InputConvertGame::processKeyClick(const QPointF &clickPos, bool clickTwice, bool switchMap, const QKeyEvent *from)
{
    if (switchMap && QEvent::KeyRelease == from->type()) {
        m_needBackMouseMove = !m_needBackMouseMove;
        hideMouseCursor(!m_needBackMouseMove);
    }

    if (QEvent::KeyPress == from->type()) {
        int id = attachTouchID(from->key());
        sendTouchDownEvent(id, clickPos);
        if (clickTwice) {
            sendTouchUpEvent(getTouchID(from->key()), clickPos);
            detachTouchID(from->key());
        }
    } else if (QEvent::KeyRelease == from->type()) {
        if (clickTwice) {
            int id = attachTouchID(from->key());
            sendTouchDownEvent(id, clickPos);
        }
        sendTouchUpEvent(getTouchID(from->key()), clickPos);
        detachTouchID(from->key());
    }
}

void InputConvertGame::processKeyClickMulti(const KeyMap::DelayClickNode *nodes, const int count, const QKeyEvent *from)
{
    if (QEvent::KeyPress != from->type()) {
        return;
    }

    int key = from->key();
    int delay = 0;
    QPointF clickPos;

    for (int i = 0; i < count; i++) {
        delay += nodes[i].delay;
        clickPos = nodes[i].pos;
        QTimer::singleShot(delay, this, [this, key, clickPos]() {
            int id = attachTouchID(key);
            sendTouchDownEvent(id, clickPos);
        });

        // Don't up it too fast
        delay += 20;
        QTimer::singleShot(delay, this, [this, key, clickPos]() {
            int id = getTouchID(key);
            sendTouchUpEvent(id, clickPos);
            detachTouchID(key);
        });
    }
}

void InputConvertGame::onDragTimer() {
    if(m_dragDelayData.stepIndex >= m_dragDelayData.pathPos.size()) {
        return;
    }
    
    int id = getTouchID(m_dragDelayData.pressKey);
    if (id == -1) return;

    m_dragDelayData.currentPos = m_dragDelayData.pathPos[m_dragDelayData.stepIndex];
    quint32 nextTimer = m_dragDelayData.pathTimer[m_dragDelayData.stepIndex];
    
    sendTouchMoveEvent(id, m_dragDelayData.currentPos);

    m_dragDelayData.stepIndex++;

    if(m_dragDelayData.stepIndex >= m_dragDelayData.pathPos.size()) {
        sendTouchUpEvent(id, m_dragDelayData.currentPos);
        detachTouchID(m_dragDelayData.pressKey);

        m_dragDelayData.currentPos = QPointF();
        m_dragDelayData.pressKey = 0;
        return;
    }

    if(m_dragDelayData.stepIndex < m_dragDelayData.pathPos.size()) {
        m_dragDelayData.timer->start(nextTimer);
    }
}

void InputConvertGame::processKeyDrag(const QPointF &startPos, QPointF endPos, quint32 startDelay, float dragSpeed, const QKeyEvent *from)
{
    if (QEvent::KeyPress == from->type()) {
        // stop last safely without re-allocation
        if (m_dragDelayData.timer->isActive()) {
            m_dragDelayData.timer->stop();
        }

        if (m_dragDelayData.pressKey != 0) {
            sendTouchUpEvent(getTouchID(m_dragDelayData.pressKey), m_dragDelayData.currentPos);
            detachTouchID(m_dragDelayData.pressKey);
        }

        // start this
        int id = attachTouchID(from->key());
        sendTouchDownEvent(id, startPos);

        m_dragDelayData.pressKey = from->key();
        m_dragDelayData.currentPos = startPos;
        m_dragDelayData.pathPos.clear();
        m_dragDelayData.pathTimer.clear();
        m_dragDelayData.stepIndex = 0;

        // Clamp dragSpeed to 0-1 range
        const float speed = qBound(0.0f, static_cast<float>(dragSpeed), 1.0f);
        
        // Calculate delays based on dragSpeed
        const quint32 minDelay = static_cast<quint32>(1 + (1.0f - speed) * 29);  // 1 to 30
        const quint32 maxDelay = minDelay + static_cast<quint32>((1.0f - speed) * 9) + 1;  // min + (0 to 9) + 1

        getDelayQueue(startPos, endPos,
                      0.01f, 0.0005f,
                      minDelay,
                      maxDelay,
                      m_dragDelayData.pathPos,
                      m_dragDelayData.pathTimer);

        if (!m_dragDelayData.pathPos.isEmpty()) {
            m_dragDelayData.timer->start(startDelay);
        }
    }
}

void InputConvertGame::processAndroidKey(AndroidKeycode androidKey, const QKeyEvent *from)
{
    if (AKEYCODE_UNKNOWN == androidKey) {
        return;
    }

    AndroidKeyeventAction action;
    switch (from->type()) {
    case QEvent::KeyPress:
        action = AKEY_EVENT_ACTION_DOWN;
        break;
    case QEvent::KeyRelease:
        action = AKEY_EVENT_ACTION_UP;
        break;
    default:
        return;
    }

    sendKeyEvent(action, androidKey);
}

// -------- mouse event --------

bool InputConvertGame::processMouseClick(const QMouseEvent *from)
{
    const KeyMap::KeyMapNode &node = m_keyMap.getKeyMapNodeMouse(from->button());
    if (KeyMap::KMT_INVALID == node.type) {
        return false;
    }

    if (QEvent::MouseButtonPress == from->type() || QEvent::MouseButtonDblClick == from->type()) {
        int id = attachTouchID(from->button());
        sendTouchDownEvent(id, node.data.click.keyNode.pos);
        return true;
    }
    if (QEvent::MouseButtonRelease == from->type()) {
        int id = getTouchID(from->button());
        sendTouchUpEvent(id, node.data.click.keyNode.pos);
        detachTouchID(from->button());
        return true;
    }
    return false;
}

bool InputConvertGame::processMouseMove(const QMouseEvent *from)
{
    if (QEvent::MouseMove != from->type()) {
        return false;
    }
    
    QPoint centerPos(m_showSize.width() / 2, m_showSize.height() / 2);
    
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QPoint currentPos = from->pos();
#else
    QPoint currentPos = from->position().toPoint();
#endif

    if (currentPos == centerPos) {
        m_ctrlMouseMove.lastPos = QPointF(centerPos);
        return true;
    }

    if (m_ctrlMouseMove.lastPos.isNull()) {
         m_ctrlMouseMove.lastPos = QPointF(centerPos);
         moveCursorTo(from, centerPos);
         return true;
    }

    if (m_ctrlMouseMove.ignoreCount > 0) {
        --m_ctrlMouseMove.ignoreCount;
        moveCursorTo(from, centerPos);
        return true;
    }

    if (m_processMouseMove) {
        QPointF distance_raw = QPointF(currentPos) - QPointF(centerPos);
        QPointF speedRatio = m_keyMap.getMouseMoveMap().data.mouseMove.speedRatio;
        QPointF distance {distance_raw.x() / speedRatio.x(), distance_raw.y() / speedRatio.y()};
        m_ctrlMouseMove.lastConverPos.setX(m_ctrlMouseMove.lastConverPos.x() + distance.x() / m_showSize.width());
        m_ctrlMouseMove.lastConverPos.setY(m_ctrlMouseMove.lastConverPos.y() + distance.y() / m_showSize.height());

        mouseMoveStartTouch(from);
        startMouseMoveTimer();

        m_ctrlMouseMove.lastConverPos.setX(m_ctrlMouseMove.lastConverPos.x() + distance.x() / m_showSize.width());
        m_ctrlMouseMove.lastConverPos.setY(m_ctrlMouseMove.lastConverPos.y() + distance.y() / m_showSize.height());

        if (m_ctrlMouseMove.lastConverPos.x() < 0.05 || m_ctrlMouseMove.lastConverPos.x() > 0.95 || 
            m_ctrlMouseMove.lastConverPos.y() < 0.05 || m_ctrlMouseMove.lastConverPos.y() > 0.95) {
            
            if (m_ctrlMouseMove.smallEyes) {
                m_processMouseMove = false;
                int delay = 30;
                QTimer::singleShot(delay, this, [this]() { mouseMoveStopTouch(); });
                QTimer::singleShot(delay * 2, this, [this]() {
                    mouseMoveStartTouch(nullptr);
                    m_processMouseMove = true;
                });
            } else {
                mouseMoveStopTouch();
                m_ctrlMouseMove.ignoreCount = 5;
                moveCursorTo(from, centerPos);
                return true;
            }
        }

        if (!m_ctrlMouseMove.paceTimer.isValid()) {
            m_ctrlMouseMove.paceTimer.start();
        }

        if (m_ctrlMouseMove.paceTimer.elapsed() >= 8) { 
            sendTouchMoveEvent(getTouchID(Qt::ExtraButton24), m_ctrlMouseMove.lastConverPos);
            m_ctrlMouseMove.paceTimer.restart();
            moveCursorTo(from, centerPos);
            m_ctrlMouseMove.lastPos = QPointF(centerPos);
        }
    } else {
        moveCursorTo(from, centerPos);
    }

    return true;
}

bool InputConvertGame::checkCursorPos(const QMouseEvent *from)
{
    Q_UNUSED(from)
    return false;
}

void InputConvertGame::moveCursorTo(const QMouseEvent *from, const QPoint &localPosPixel)
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QPoint posOffset = from->pos() - localPosPixel;
    QPoint globalPos = from->globalPos();
#else
    QPoint posOffset = from->position().toPoint() - localPosPixel;
    QPoint globalPos = from->globalPosition().toPoint();
#endif
    globalPos -= posOffset;
    //qDebug()<<"move cursor to "<<globalPos<<" offset "<<posOffset;
    QCursor::setPos(globalPos);
}

void InputConvertGame::mouseMoveStartTouch(const QMouseEvent *from)
{
    Q_UNUSED(from)
    if (!m_ctrlMouseMove.touching) {
        QPointF mouseMoveStartPos
            = m_ctrlMouseMove.smallEyes ? m_keyMap.getMouseMoveMap().data.mouseMove.smallEyes.pos : m_keyMap.getMouseMoveMap().data.mouseMove.startPos;
        int id = attachTouchID(Qt::ExtraButton24);
        sendTouchDownEvent(id, mouseMoveStartPos);
        m_ctrlMouseMove.lastConverPos = mouseMoveStartPos;
        m_ctrlMouseMove.touching = true;
    }
}

void InputConvertGame::mouseMoveStopTouch()
{
    if (m_ctrlMouseMove.touching) {
        sendTouchUpEvent(getTouchID(Qt::ExtraButton24), m_ctrlMouseMove.lastConverPos);
        detachTouchID(Qt::ExtraButton24);
        m_ctrlMouseMove.touching = false;
    }
}

void InputConvertGame::startMouseMoveTimer()
{
    stopMouseMoveTimer();
    m_ctrlMouseMove.timer = startTimer(500);
}

void InputConvertGame::stopMouseMoveTimer()
{
    if (0 != m_ctrlMouseMove.timer) {
        killTimer(m_ctrlMouseMove.timer);
        m_ctrlMouseMove.timer = 0;
    }
}

void InputConvertGame::stopSteerWheel() {
    // 1. Matikan Timer
    if (m_ctrlSteerWheel.delayData.timer->isActive()) {
        m_ctrlSteerWheel.delayData.timer->stop();
    }
    m_ctrlSteerWheel.delayData.pathTimer.clear();
    m_ctrlSteerWheel.delayData.pathPos.clear();
    m_ctrlSteerWheel.delayData.stepIndex = 0;

    // 2. Lepas Touch Analog
    if (m_ctrlSteerWheel.touchKey != 0) {
        int id = getTouchID(m_ctrlSteerWheel.touchKey);
        if (id != -1) {
            // Kirim event angkat jari di posisi terakhir
            sendTouchUpEvent(id, m_ctrlSteerWheel.delayData.currentPos);
            detachTouchID(m_ctrlSteerWheel.touchKey);
        }
    }

    // 3. Reset semua flag
    m_ctrlSteerWheel.touchKey = 0;
    m_ctrlSteerWheel.pressedUp = false;
    m_ctrlSteerWheel.pressedDown = false;
    m_ctrlSteerWheel.pressedLeft = false;
    m_ctrlSteerWheel.pressedRight = false;
    m_ctrlSteerWheel.delayData.pressedNum = 0;
}

void InputConvertGame::stopDrag() {
    // Bersihkan state drag mouse/skill tanpa menghancurkan (delete) QTimer
    if (m_dragDelayData.timer->isActive()) {
        m_dragDelayData.timer->stop();
    }
    
    m_dragDelayData.pathPos.clear();
    m_dragDelayData.pathTimer.clear();
    m_dragDelayData.stepIndex = 0;

    if (m_dragDelayData.pressKey != 0) {
        int id = getTouchID(m_dragDelayData.pressKey);
        if (id != -1) {
            sendTouchUpEvent(id, m_dragDelayData.currentPos);
            detachTouchID(m_dragDelayData.pressKey);
        }
        m_dragDelayData.pressKey = 0;
        m_dragDelayData.currentPos = QPointF();
    }
}

void InputConvertGame::releaseAllKeys() {
    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; i++) {
        if (m_multiTouchID[i] != 0) {
            sendTouchUpEvent(i, QPointF(0.5, 0.5));
            m_multiTouchID[i] = 0;
        }
    }
    stopSteerWheel();
    stopDrag();
    stopMouseMoveTimer();
    mouseMoveStopTouch();
}

bool InputConvertGame::switchGameMap()
{
    m_gameMap = !m_gameMap;
    qInfo() << QString("current keymap mode: %1").arg(m_gameMap ? "custom" : "normal");

    if (!m_keyMap.isValidMouseMoveMap()) {
        return m_gameMap;
    }
    
    emit grabCursor(m_gameMap);
    hideMouseCursor(m_gameMap);

    if (!m_gameMap) {
        releaseAllKeys();
    } else {
        m_ctrlMouseMove.lastPos = QPointF(); 
    }

    return m_gameMap;
}

void InputConvertGame::hideMouseCursor(bool hide)
{
    if (hide) {
        QGuiApplication::setOverrideCursor(QCursor(Qt::BlankCursor));

        if (m_controller && m_controller->parent()) {
             QWidget* view = qobject_cast<QWidget*>(m_controller->parent());
             if (view) view->grabMouse();
        }
    } else {
        QGuiApplication::restoreOverrideCursor();
        
        if (m_controller && m_controller->parent()) {
             QWidget* view = qobject_cast<QWidget*>(m_controller->parent());
             if (view) view->releaseMouse();
        }
    }
}

void InputConvertGame::timerEvent(QTimerEvent *event)
{
    if (m_ctrlMouseMove.timer == event->timerId()) {
        stopMouseMoveTimer();
        mouseMoveStopTouch();
    }
}