#include "inputconvertgame.h"
#include "../controller.h"

#include <QCursor>
#include <QDebug>
#include <QGuiApplication>
#include <QTimerEvent>
#include <algorithm>
#include <cmath>
#include <random>

namespace {
constexpr int MOUSE_MOVE_INTERVAL_MS = 8;
constexpr int MOUSE_TOUCH_TIMEOUT_MS = 500;
constexpr double HALF_PI = 1.57079632679489661923;

std::mt19937 &randomGenerator()
{
    thread_local std::mt19937 generator(std::random_device{}());
    return generator;
}
}

InputConvertGame::InputConvertGame(Controller *controller)
    : InputConvertNormal(controller)
{
    m_ctrlSteerWheel.delayData.timer = new QTimer(this);
    m_ctrlSteerWheel.delayData.timer->setSingleShot(true);
    connect(m_ctrlSteerWheel.delayData.timer, &QTimer::timeout,
            this, &InputConvertGame::onSteerWheelTimer);

    m_dragDelayData.timer = new QTimer(this);
    m_dragDelayData.timer->setSingleShot(true);
    connect(m_dragDelayData.timer, &QTimer::timeout,
            this, &InputConvertGame::onDragTimer);
}

InputConvertGame::~InputConvertGame()
{
    releaseAllKeys();
    emit grabCursor(false);
    hideMouseCursor(false);
}

void InputConvertGame::mouseEvent(const QMouseEvent *from,
                                  const QSize &frameSize,
                                  const QSize &showSize)
{
    if (!from) return;

    if (!m_keyMap.isSwitchOnKeyboard() &&
        m_keyMap.getSwitchKey() == static_cast<int>(from->button())) {
        if (from->type() == QEvent::MouseButtonPress) {
            switchGameMap();
        }
        return;
    }

    if (m_gameMap && !m_needBackMouseMove) {
        updateSize(frameSize, showSize);
        if (m_keyMap.isValidMouseMoveMap() && processMouseMove(from)) return;
        if (processMouseClick(from)) return;
    }

    InputConvertNormal::mouseEvent(from, frameSize, showSize);
}

void InputConvertGame::wheelEvent(const QWheelEvent *from,
                                  const QSize &frameSize,
                                  const QSize &showSize)
{
    if (!from) return;
    if (m_gameMap) {
        updateSize(frameSize, showSize);
    } else {
        InputConvertNormal::wheelEvent(from, frameSize, showSize);
    }
}

void InputConvertGame::keyEvent(const QKeyEvent *from,
                                const QSize &frameSize,
                                const QSize &showSize)
{
    if (!from) return;

    if (m_keyMap.isSwitchOnKeyboard() && m_keyMap.getSwitchKey() == from->key()) {
        if (from->type() == QEvent::KeyPress && !from->isAutoRepeat()) {
            switchGameMap();
        }
        return;
    }

    const KeyMap::KeyMapNode &node = m_keyMap.getKeyMapNodeKey(from->key());
    if (m_needBackMouseMove && node.type == KeyMap::KMT_CLICK && node.data.click.switchMap) {
        updateSize(frameSize, showSize);
        processKeyClick(node.data.click.keyNode.pos, false, true, from);
        return;
    }

    if (!m_gameMap) {
        InputConvertNormal::keyEvent(from, frameSize, showSize);
        return;
    }

    updateSize(frameSize, showSize);
    if (from->isAutoRepeat()) return;

    if (m_keyMap.isValidMouseMoveMap() &&
        from->key() == m_keyMap.getMouseMoveMap().data.mouseMove.smallEyes.key) {
        m_ctrlMouseMove.smallEyes = from->type() == QEvent::KeyPress;
        invalidatePendingActions();
        const std::uint64_t epoch = m_actionEpoch;

        if (m_ctrlMouseMove.smallEyes) {
            m_processMouseMove = false;
            stopMouseMoveTimer();
            QTimer::singleShot(30, this, [this, epoch]() {
                if (epoch != m_actionEpoch || !m_gameMap) return;
                mouseMoveStopTouch();
            });
            QTimer::singleShot(60, this, [this, epoch]() {
                if (epoch != m_actionEpoch || !m_gameMap) return;
                mouseMoveStartTouch();
                m_processMouseMove = true;
            });
        } else {
            mouseMoveStopTouch();
            mouseMoveStartTouch();
            m_processMouseMove = true;
        }
        return;
    }

    switch (node.type) {
    case KeyMap::KMT_STEER_WHEEL:
        processSteerWheel(node, from);
        return;
    case KeyMap::KMT_CLICK:
        processKeyClick(node.data.click.keyNode.pos, false,
                        node.data.click.switchMap, from);
        processAndroidKey(node.data.click.keyNode.androidKey, from);
        return;
    case KeyMap::KMT_CLICK_TWICE:
        processKeyClick(node.data.clickTwice.keyNode.pos, true, false, from);
        processAndroidKey(node.data.clickTwice.keyNode.androidKey, from);
        return;
    case KeyMap::KMT_CLICK_MULTI:
        processKeyClickMulti(node.data.clickMulti.keyNode.delayClickNodes,
                             node.data.clickMulti.keyNode.delayClickNodesCount,
                             from);
        return;
    case KeyMap::KMT_DRAG:
        processKeyDrag(node.data.drag.keyNode.pos,
                       node.data.drag.keyNode.extendPos,
                       node.data.drag.startDelay,
                       node.data.drag.dragSpeed,
                       from);
        return;
    case KeyMap::KMT_ANDROID_KEY:
        processAndroidKey(node.data.androidKey.keyNode.androidKey, from);
        return;
    default:
        return;
    }
}

bool InputConvertGame::isCurrentCustomKeymap()
{
    return m_gameMap;
}

void InputConvertGame::loadKeyMap(const QString &json)
{
    const bool wasActive = m_gameMap;
    if (wasActive) releaseAllKeys();
    m_keyMap.loadKeyMap(json);

    if (wasActive) {
        const bool captureMouse = m_keyMap.isValidMouseMoveMap() && !m_needBackMouseMove;
        emit grabCursor(captureMouse);
        hideMouseCursor(captureMouse);
    }
}

void InputConvertGame::updateSize(const QSize &frameSize, const QSize &showSize)
{
    if (frameSize.isValid() && !frameSize.isEmpty()) m_frameSize = frameSize;
    if (showSize.isValid() && !showSize.isEmpty()) m_showSize = showSize;
}

bool InputConvertGame::sendTouchDownEvent(int id, const QPointF &pos)
{
    return sendTouchEvent(id, pos, AMOTION_EVENT_ACTION_DOWN);
}

bool InputConvertGame::sendTouchMoveEvent(int id, const QPointF &pos)
{
    return sendTouchEvent(id, pos, AMOTION_EVENT_ACTION_MOVE);
}

bool InputConvertGame::sendTouchUpEvent(int id, const QPointF &pos)
{
    return sendTouchEvent(id, pos, AMOTION_EVENT_ACTION_UP);
}

bool InputConvertGame::sendTouchEvent(int id,
                                      const QPointF &pos,
                                      AndroidMotioneventAction action)
{
    if (id < 0 || id >= MULTI_TOUCH_MAX_NUM ||
        m_frameSize.width() <= 0 || m_frameSize.height() <= 0 || !m_controller) {
        return false;
    }

    const QPointF safePos = clampRelativePos(pos);
    const QPoint absolutePos = calcFrameAbsolutePos(safePos).toPoint();

    if (action == AMOTION_EVENT_ACTION_MOVE &&
        m_hasLastAbsolutePosition[id] &&
        m_lastAbsolutePositions[id] == absolutePos) {
        return true;
    }

    ControlMsg controlMsg(ControlMsg::CMT_INJECT_TOUCH);
    controlMsg.setInjectTouchMsgData(
        static_cast<quint64>(id),
        action,
        static_cast<AndroidMotioneventButtons>(0),
        static_cast<AndroidMotioneventButtons>(0),
        QRect(absolutePos, m_frameSize),
        action == AMOTION_EVENT_ACTION_DOWN ? 1.0f : 0.0f);

    if (!m_controller->sendControl(controlMsg)) return false;

    m_touchPositions[id] = safePos;
    m_lastAbsolutePositions[id] = absolutePos;
    m_hasLastAbsolutePosition[id] = action != AMOTION_EVENT_ACTION_UP;
    return true;
}

void InputConvertGame::sendKeyEvent(AndroidKeyeventAction action,
                                    AndroidKeycode keyCode)
{
    if (!m_controller || keyCode == AKEYCODE_UNKNOWN) return;

    ControlMsg controlMsg(ControlMsg::CMT_INJECT_KEYCODE);
    controlMsg.setInjectKeycodeMsgData(action, keyCode, 0, AMETA_NONE);
    (void)m_controller->sendControl(controlMsg);
}

QPointF InputConvertGame::clampRelativePos(const QPointF &pos) const
{
    return QPointF(qBound(0.0, pos.x(), 1.0),
                   qBound(0.0, pos.y(), 1.0));
}

QPointF InputConvertGame::calcFrameAbsolutePos(const QPointF &relativePos) const
{
    return QPointF(m_frameSize.width() * relativePos.x(),
                   m_frameSize.height() * relativePos.y());
}

QPointF InputConvertGame::calcScreenAbsolutePos(const QPointF &relativePos) const
{
    return QPointF(m_showSize.width() * relativePos.x(),
                   m_showSize.height() * relativePos.y());
}

int InputConvertGame::attachTouchID(int key)
{
    if (key == 0 || key == Qt::Key_unknown) return -1;

    const int existing = getTouchID(key);
    if (existing >= 0) return existing;

    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; ++i) {
        if (m_multiTouchID[i] == 0) {
            m_multiTouchID[i] = key;
            m_touchPositions[i] = QPointF(0.5, 0.5);
            m_hasLastAbsolutePosition[i] = false;
            return i;
        }
    }
    return -1;
}

void InputConvertGame::detachTouchID(int key)
{
    const int id = getTouchID(key);
    if (id >= 0) detachTouchIDByIndex(id);
}

void InputConvertGame::detachTouchIDByIndex(int id)
{
    if (id < 0 || id >= MULTI_TOUCH_MAX_NUM) return;
    m_multiTouchID[id] = 0;
    m_touchPositions[id] = QPointF();
    m_lastAbsolutePositions[id] = QPoint();
    m_hasLastAbsolutePosition[id] = false;
}

int InputConvertGame::getTouchID(int key) const
{
    if (key == 0 || key == Qt::Key_unknown) return -1;
    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; ++i) {
        if (m_multiTouchID[i] == key) return i;
    }
    return -1;
}

void InputConvertGame::getDelayQueue(const QPointF &start,
                                     const QPointF &end,
                                     double distanceStep,
                                     double positionJitter,
                                     quint32 lowestTimer,
                                     quint32 highestTimer,
                                     QVector<QPointF> &pathPos,
                                     QVector<quint32> &pathTimer)
{
    pathPos.clear();
    pathTimer.clear();

    const QPointF safeStart = clampRelativePos(start);
    const QPointF safeEnd = clampRelativePos(end);
    const double dx = safeEnd.x() - safeStart.x();
    const double dy = safeEnd.y() - safeStart.y();
    const double totalDistance = std::hypot(dx, dy);
    if (totalDistance < 0.0001 || distanceStep <= 0.0) return;

    int steps = qMax(2, static_cast<int>(qMax(std::abs(dx), std::abs(dy)) / distanceStep));
    steps = qMin(steps, 512);
    pathPos.reserve(steps);
    pathTimer.reserve(steps);

    const double midX = safeStart.x() + dx / 2.0;
    const double midY = safeStart.y() + dy / 2.0;
    const double perpX = -dy / totalDistance;
    const double perpY = dx / totalDistance;

    std::uniform_real_distribution<double> curveDistribution(-0.15, 0.15);
    double curveFactor = curveDistribution(randomGenerator());
    if (std::abs(curveFactor) < 0.05) curveFactor = curveFactor < 0.0 ? -0.05 : 0.05;

    const double offsetLength = totalDistance * curveFactor;
    const double controlX = midX + perpX * offsetLength;
    const double controlY = midY + perpY * offsetLength;

    std::uniform_real_distribution<double> jitterDistribution(-positionJitter, positionJitter);
    if (highestTimer < lowestTimer) std::swap(highestTimer, lowestTimer);
    std::uniform_int_distribution<quint32> timeDistribution(lowestTimer, highestTimer);

    for (int i = 1; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        const double eased = std::sin(t * HALF_PI);
        const double u = 1.0 - eased;
        const double x = u * u * safeStart.x() +
                         2.0 * u * eased * controlX +
                         eased * eased * safeEnd.x();
        const double y = u * u * safeStart.y() +
                         2.0 * u * eased * controlY +
                         eased * eased * safeEnd.y();

        pathPos.append(clampRelativePos(QPointF(
            x + jitterDistribution(randomGenerator()),
            y + jitterDistribution(randomGenerator()))));

        quint32 delay = timeDistribution(randomGenerator());
        if (i > static_cast<int>(steps * 0.8)) {
            delay += std::uniform_int_distribution<quint32>(1, 4)(randomGenerator());
        }
        pathTimer.append(qMax<quint32>(1, delay));
    }
}

void InputConvertGame::onSteerWheelTimer()
{
    auto &delay = m_ctrlSteerWheel.delayData;
    if (!m_gameMap || delay.stepIndex < 0 ||
        delay.stepIndex >= delay.pathPos.size() ||
        delay.stepIndex >= delay.pathTimer.size()) {
        return;
    }

    const int id = getTouchID(m_ctrlSteerWheel.touchKey);
    if (id < 0) {
        stopSteerWheel(false);
        return;
    }

    delay.currentPos = delay.pathPos[delay.stepIndex];
    const quint32 nextDelay = delay.pathTimer[delay.stepIndex];
    sendTouchMoveEvent(id, delay.currentPos);
    ++delay.stepIndex;

    if (delay.stepIndex < delay.pathPos.size()) {
        delay.timer->start(static_cast<int>(nextDelay));
    } else if (delay.pressedNum == 0) {
        stopSteerWheel(true);
    }
}

void InputConvertGame::processSteerWheel(const KeyMap::KeyMapNode &node,
                                         const QKeyEvent *from)
{
    if (!from) return;

    const bool pressed = from->type() == QEvent::KeyPress;
    const int key = from->key();
    if (key == node.data.steerWheel.up.key) m_ctrlSteerWheel.pressedUp = pressed;
    else if (key == node.data.steerWheel.right.key) m_ctrlSteerWheel.pressedRight = pressed;
    else if (key == node.data.steerWheel.down.key) m_ctrlSteerWheel.pressedDown = pressed;
    else if (key == node.data.steerWheel.left.key) m_ctrlSteerWheel.pressedLeft = pressed;
    else return;

    QPointF offset;
    int pressedCount = 0;
    if (m_ctrlSteerWheel.pressedUp) {
        ++pressedCount;
        offset.ry() -= node.data.steerWheel.up.extendOffset;
    }
    if (m_ctrlSteerWheel.pressedRight) {
        ++pressedCount;
        offset.rx() += node.data.steerWheel.right.extendOffset;
    }
    if (m_ctrlSteerWheel.pressedDown) {
        ++pressedCount;
        offset.ry() += node.data.steerWheel.down.extendOffset;
    }
    if (m_ctrlSteerWheel.pressedLeft) {
        ++pressedCount;
        offset.rx() -= node.data.steerWheel.left.extendOffset;
    }

    auto &delay = m_ctrlSteerWheel.delayData;
    delay.pressedNum = pressedCount;
    delay.timer->stop();
    delay.pathPos.clear();
    delay.pathTimer.clear();
    delay.stepIndex = 0;

    if (pressedCount == 0) {
        stopSteerWheel(true);
        return;
    }

    if (getTouchID(m_ctrlSteerWheel.touchKey) < 0) {
        m_ctrlSteerWheel.touchKey = key;
        const int id = attachTouchID(key);
        if (id < 0) {
            stopSteerWheel(false);
            return;
        }
        delay.currentPos = clampRelativePos(node.data.steerWheel.centerPos);
        if (!sendTouchDownEvent(id, delay.currentPos)) {
            detachTouchIDByIndex(id);
            stopSteerWheel(false);
            return;
        }
    }

    const QPointF target = clampRelativePos(node.data.steerWheel.centerPos + offset);
    getDelayQueue(delay.currentPos, target, 0.01, 0.002, 2, 8,
                  delay.pathPos, delay.pathTimer);
    if (!delay.pathPos.isEmpty()) delay.timer->start(1);
}

QPointF InputConvertGame::addJitter(const QPointF &pos)
{
    std::uniform_real_distribution<double> distribution(-0.002, 0.002);
    return clampRelativePos(QPointF(
        pos.x() + distribution(randomGenerator()),
        pos.y() + distribution(randomGenerator())));
}

void InputConvertGame::processKeyClick(const QPointF &clickPos,
                                       bool clickTwice,
                                       bool switchMap,
                                       const QKeyEvent *from)
{
    if (!from) return;

    const int key = from->key();
    if (switchMap && from->type() == QEvent::KeyRelease) {
        m_needBackMouseMove = !m_needBackMouseMove;
        const bool captureMouse = m_gameMap && m_keyMap.isValidMouseMoveMap() && !m_needBackMouseMove;
        emit grabCursor(captureMouse);
        hideMouseCursor(captureMouse);
    }

    if (from->type() == QEvent::KeyPress) {
        const QPointF position = addJitter(clickPos);
        m_keyJitterMap.insert(key, position);

        const int id = attachTouchID(key);
        if (id < 0 || !sendTouchDownEvent(id, position)) {
            if (id >= 0) detachTouchIDByIndex(id);
            m_keyJitterMap.remove(key);
            return;
        }

        if (clickTwice) {
            sendTouchUpEvent(id, position);
            detachTouchIDByIndex(id);
        }
        return;
    }

    if (from->type() != QEvent::KeyRelease) return;

    QPointF position = m_keyJitterMap.value(key, clampRelativePos(clickPos));
    if (clickTwice) {
        position = addJitter(clickPos);
        const int secondId = attachTouchID(key);
        if (secondId >= 0 && sendTouchDownEvent(secondId, position)) {
            sendTouchUpEvent(secondId, position);
            detachTouchIDByIndex(secondId);
        }
    } else {
        const int id = getTouchID(key);
        if (id >= 0) {
            sendTouchUpEvent(id, position);
            detachTouchIDByIndex(id);
        }
    }
    m_keyJitterMap.remove(key);
}

void InputConvertGame::processKeyClickMulti(const KeyMap::DelayClickNode *nodes,
                                            int count,
                                            const QKeyEvent *from)
{
    if (!nodes || !from || from->type() != QEvent::KeyPress || count <= 0) return;

    count = qMin(count, MAX_DELAY_CLICK_NODES);
    const int key = from->key();
    const std::uint64_t epoch = m_actionEpoch;
    int delayMs = 0;
    std::uniform_int_distribution<int> dwellDistribution(35, 75);

    for (int i = 0; i < count; ++i) {
        delayMs += qMax(0, nodes[i].delay);
        const QPointF position = addJitter(nodes[i].pos);

        QTimer::singleShot(delayMs, this, [this, epoch, key, position]() {
            if (epoch != m_actionEpoch || !m_gameMap || getTouchID(key) >= 0) return;
            const int id = attachTouchID(key);
            if (id < 0 || !sendTouchDownEvent(id, position)) {
                if (id >= 0) detachTouchIDByIndex(id);
            }
        });

        delayMs += dwellDistribution(randomGenerator());
        QTimer::singleShot(delayMs, this, [this, epoch, key, position]() {
            if (epoch != m_actionEpoch) return;
            const int id = getTouchID(key);
            if (id >= 0) {
                sendTouchUpEvent(id, position);
                detachTouchIDByIndex(id);
            }
        });
    }
}

void InputConvertGame::onDragTimer()
{
    if (!m_gameMap || m_dragDelayData.stepIndex < 0 ||
        m_dragDelayData.stepIndex >= m_dragDelayData.pathPos.size() ||
        m_dragDelayData.stepIndex >= m_dragDelayData.pathTimer.size()) {
        return;
    }

    const int id = getTouchID(m_dragDelayData.pressKey);
    if (id < 0) {
        stopDrag(false);
        return;
    }

    m_dragDelayData.currentPos = m_dragDelayData.pathPos[m_dragDelayData.stepIndex];
    const quint32 nextDelay = m_dragDelayData.pathTimer[m_dragDelayData.stepIndex];
    sendTouchMoveEvent(id, m_dragDelayData.currentPos);
    ++m_dragDelayData.stepIndex;

    if (m_dragDelayData.stepIndex < m_dragDelayData.pathPos.size()) {
        m_dragDelayData.timer->start(static_cast<int>(nextDelay));
    } else {
        stopDrag(true);
    }
}

void InputConvertGame::processKeyDrag(const QPointF &startPos,
                                      const QPointF &endPos,
                                      quint32 startDelay,
                                      float dragSpeed,
                                      const QKeyEvent *from)
{
    if (!from || from->type() != QEvent::KeyPress) return;

    stopDrag(true);
    const int id = attachTouchID(from->key());
    const QPointF safeStart = clampRelativePos(startPos);
    if (id < 0 || !sendTouchDownEvent(id, safeStart)) {
        if (id >= 0) detachTouchIDByIndex(id);
        return;
    }

    m_dragDelayData.pressKey = from->key();
    m_dragDelayData.currentPos = safeStart;
    m_dragDelayData.stepIndex = 0;

    const float speed = qBound(0.0f, dragSpeed, 1.0f);
    const quint32 minimumDelay = static_cast<quint32>(1 + (1.0f - speed) * 29.0f);
    const quint32 maximumDelay = minimumDelay +
        static_cast<quint32>((1.0f - speed) * 9.0f) + 1;

    getDelayQueue(safeStart, endPos, 0.01, 0.0005,
                  minimumDelay, maximumDelay,
                  m_dragDelayData.pathPos,
                  m_dragDelayData.pathTimer);

    if (m_dragDelayData.pathPos.isEmpty()) {
        stopDrag(true);
    } else {
        m_dragDelayData.timer->start(static_cast<int>(startDelay));
    }
}

void InputConvertGame::processAndroidKey(AndroidKeycode androidKey,
                                         const QKeyEvent *from)
{
    if (!from || androidKey == AKEYCODE_UNKNOWN) return;

    if (from->type() == QEvent::KeyPress) {
        sendKeyEvent(AKEY_EVENT_ACTION_DOWN, androidKey);
    } else if (from->type() == QEvent::KeyRelease) {
        sendKeyEvent(AKEY_EVENT_ACTION_UP, androidKey);
    }
}

bool InputConvertGame::processMouseClick(const QMouseEvent *from)
{
    if (!from) return false;
    const KeyMap::KeyMapNode &node = m_keyMap.getKeyMapNodeMouse(from->button());
    if (node.type == KeyMap::KMT_INVALID) return false;

    const int key = static_cast<int>(from->button());
    if (from->type() == QEvent::MouseButtonPress ||
        from->type() == QEvent::MouseButtonDblClick) {
        const int id = attachTouchID(key);
        if (id >= 0 && sendTouchDownEvent(id, node.data.click.keyNode.pos)) return true;
        if (id >= 0) detachTouchIDByIndex(id);
        return true;
    }

    if (from->type() == QEvent::MouseButtonRelease) {
        const int id = getTouchID(key);
        if (id >= 0) {
            sendTouchUpEvent(id, node.data.click.keyNode.pos);
            detachTouchIDByIndex(id);
        }
        return true;
    }
    return false;
}

bool InputConvertGame::processMouseMove(const QMouseEvent *from)
{
    if (!from || from->type() != QEvent::MouseMove ||
        m_showSize.width() <= 0 || m_showSize.height() <= 0) {
        return false;
    }

    const QPoint center(m_showSize.width() / 2, m_showSize.height() / 2);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    const QPoint current = from->pos();
#else
    const QPoint current = from->position().toPoint();
#endif

    if (current == center && m_ctrlMouseMove.lastPos == QPointF(center)) return true;
    if (m_ctrlMouseMove.lastPos.isNull()) {
        m_ctrlMouseMove.lastPos = current;
        return true;
    }

    const QPointF delta = QPointF(current) - m_ctrlMouseMove.lastPos;
    m_ctrlMouseMove.lastPos = current;

    if (m_processMouseMove) {
        QPointF speedRatio = m_keyMap.getMouseMoveMap().data.mouseMove.speedRatio;
        if (qFuzzyIsNull(speedRatio.x())) speedRatio.setX(1.0);
        if (qFuzzyIsNull(speedRatio.y())) speedRatio.setY(1.0);

        m_ctrlMouseMove.lastConverPos.rx() += delta.x() / speedRatio.x() / m_showSize.width();
        m_ctrlMouseMove.lastConverPos.ry() += delta.y() / speedRatio.y() / m_showSize.height();

        mouseMoveStartTouch();
        startMouseMoveTimer();

        const bool outsideSafeZone =
            m_ctrlMouseMove.lastConverPos.x() < 0.05 ||
            m_ctrlMouseMove.lastConverPos.x() > 0.95 ||
            m_ctrlMouseMove.lastConverPos.y() < 0.05 ||
            m_ctrlMouseMove.lastConverPos.y() > 0.95;

        if (outsideSafeZone) {
            if (m_ctrlMouseMove.smallEyes) {
                invalidatePendingActions();
                const std::uint64_t epoch = m_actionEpoch;
                m_processMouseMove = false;
                QTimer::singleShot(30, this, [this, epoch]() {
                    if (epoch != m_actionEpoch || !m_gameMap) return;
                    mouseMoveStopTouch();
                });
                QTimer::singleShot(60, this, [this, epoch]() {
                    if (epoch != m_actionEpoch || !m_gameMap) return;
                    mouseMoveStartTouch();
                    m_processMouseMove = true;
                });
            } else {
                mouseMoveStopTouch();
                moveCursorTo(from, center);
                m_ctrlMouseMove.lastPos = center;
                return true;
            }
        }

        if (!m_ctrlMouseMove.paceTimer.isValid()) m_ctrlMouseMove.paceTimer.start();
        if (m_ctrlMouseMove.paceTimer.elapsed() >= MOUSE_MOVE_INTERVAL_MS) {
            const int id = getTouchID(Qt::ExtraButton24);
            if (id >= 0) sendTouchMoveEvent(id, m_ctrlMouseMove.lastConverPos);
            m_ctrlMouseMove.paceTimer.restart();
        }
    }

    const int dx = current.x() - center.x();
    const int dy = current.y() - center.y();
    if (dx * dx + dy * dy > 2500) {
        moveCursorTo(from, center);
        m_ctrlMouseMove.lastPos = center;
    }
    return true;
}

void InputConvertGame::moveCursorTo(const QMouseEvent *from,
                                    const QPoint &localPosPixel)
{
    if (!from) return;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QPoint offset = from->pos() - localPosPixel;
    QPoint global = from->globalPos();
#else
    QPoint offset = from->position().toPoint() - localPosPixel;
    QPoint global = from->globalPosition().toPoint();
#endif
    QCursor::setPos(global - offset);
}

void InputConvertGame::mouseMoveStartTouch()
{
    if (m_ctrlMouseMove.touching || !m_keyMap.isValidMouseMoveMap()) return;

    const QPointF start = m_ctrlMouseMove.smallEyes
        ? m_keyMap.getMouseMoveMap().data.mouseMove.smallEyes.pos
        : m_keyMap.getMouseMoveMap().data.mouseMove.startPos;
    const int id = attachTouchID(Qt::ExtraButton24);
    if (id < 0 || !sendTouchDownEvent(id, start)) {
        if (id >= 0) detachTouchIDByIndex(id);
        return;
    }

    m_ctrlMouseMove.lastConverPos = clampRelativePos(start);
    m_ctrlMouseMove.touching = true;
}

void InputConvertGame::mouseMoveStopTouch()
{
    if (!m_ctrlMouseMove.touching) return;
    const int id = getTouchID(Qt::ExtraButton24);
    if (id >= 0) {
        sendTouchUpEvent(id, m_ctrlMouseMove.lastConverPos);
        detachTouchIDByIndex(id);
    }
    m_ctrlMouseMove.touching = false;
}

void InputConvertGame::startMouseMoveTimer()
{
    stopMouseMoveTimer();
    m_ctrlMouseMove.timer = startTimer(MOUSE_TOUCH_TIMEOUT_MS, Qt::CoarseTimer);
}

void InputConvertGame::stopMouseMoveTimer()
{
    if (m_ctrlMouseMove.timer != 0) {
        killTimer(m_ctrlMouseMove.timer);
        m_ctrlMouseMove.timer = 0;
    }
}

void InputConvertGame::stopSteerWheel(bool releaseTouch)
{
    auto &delay = m_ctrlSteerWheel.delayData;
    if (delay.timer) delay.timer->stop();

    const int id = getTouchID(m_ctrlSteerWheel.touchKey);
    if (releaseTouch && id >= 0) {
        sendTouchUpEvent(id, delay.currentPos);
        detachTouchIDByIndex(id);
    }

    delay.pathPos.clear();
    delay.pathTimer.clear();
    delay.stepIndex = 0;
    delay.pressedNum = 0;
    delay.currentPos = QPointF();
    m_ctrlSteerWheel.touchKey = Qt::Key_unknown;
    m_ctrlSteerWheel.pressedUp = false;
    m_ctrlSteerWheel.pressedDown = false;
    m_ctrlSteerWheel.pressedLeft = false;
    m_ctrlSteerWheel.pressedRight = false;
}

void InputConvertGame::stopDrag(bool releaseTouch)
{
    if (m_dragDelayData.timer) m_dragDelayData.timer->stop();

    const int id = getTouchID(m_dragDelayData.pressKey);
    if (releaseTouch && id >= 0) {
        sendTouchUpEvent(id, m_dragDelayData.currentPos);
        detachTouchIDByIndex(id);
    }

    m_dragDelayData.pathPos.clear();
    m_dragDelayData.pathTimer.clear();
    m_dragDelayData.stepIndex = 0;
    m_dragDelayData.pressKey = 0;
    m_dragDelayData.currentPos = QPointF();
}

void InputConvertGame::resetTouchState()
{
    m_multiTouchID.fill(0);
    m_touchPositions.fill(QPointF());
    m_lastAbsolutePositions.fill(QPoint());
    m_hasLastAbsolutePosition.fill(false);
}

void InputConvertGame::invalidatePendingActions()
{
    ++m_actionEpoch;
}

void InputConvertGame::releaseAllKeys()
{
    invalidatePendingActions();
    stopMouseMoveTimer();
    if (m_ctrlSteerWheel.delayData.timer) m_ctrlSteerWheel.delayData.timer->stop();
    if (m_dragDelayData.timer) m_dragDelayData.timer->stop();

    for (int id = 0; id < MULTI_TOUCH_MAX_NUM; ++id) {
        if (m_multiTouchID[id] != 0) {
            sendTouchUpEvent(id, m_touchPositions[id]);
        }
    }

    resetTouchState();
    stopSteerWheel(false);
    stopDrag(false);
    m_ctrlMouseMove.touching = false;
    m_ctrlMouseMove.smallEyes = false;
    m_ctrlMouseMove.lastConverPos = QPointF();
    m_ctrlMouseMove.lastPos = QPointF();
    m_ctrlMouseMove.paceTimer.invalidate();
    m_keyJitterMap.clear();
    m_processMouseMove = true;
}

bool InputConvertGame::switchGameMap()
{
    if (m_gameMap) {
        releaseAllKeys();
        m_gameMap = false;
        m_needBackMouseMove = false;
        emit grabCursor(false);
        hideMouseCursor(false);
    } else {
        m_gameMap = true;
        m_needBackMouseMove = false;
        m_ctrlMouseMove.lastPos = QPointF();
        m_ctrlMouseMove.paceTimer.invalidate();
        const bool captureMouse = m_keyMap.isValidMouseMoveMap();
        emit grabCursor(captureMouse);
        hideMouseCursor(captureMouse);
    }

    qInfo() << "current keymap mode:" << (m_gameMap ? "custom" : "normal");
    return m_gameMap;
}

void InputConvertGame::hideMouseCursor(bool hide)
{
    if (hide == m_cursorHidden || !QGuiApplication::instance()) return;

    if (hide) {
        QGuiApplication::setOverrideCursor(QCursor(Qt::BlankCursor));
        m_cursorHidden = true;
    } else {
        QGuiApplication::restoreOverrideCursor();
        m_cursorHidden = false;
    }
}

void InputConvertGame::timerEvent(QTimerEvent *event)
{
    if (event && m_ctrlMouseMove.timer == event->timerId()) {
        stopMouseMoveTimer();
        mouseMoveStopTouch();
        return;
    }
    QObject::timerEvent(event);
}
