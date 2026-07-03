#ifndef INPUTCONVERTGAME_H
#define INPUTCONVERTGAME_H

#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QPoint>
#include <QPointF>
#include <QTimer>
#include <QVector>
#include <array>
#include <cstdint>

#include "inputconvertnormal.h"
#include "keymap.h"

#define MULTI_TOUCH_MAX_NUM 10

class InputConvertGame : public InputConvertNormal
{
    Q_OBJECT
public:
    explicit InputConvertGame(Controller *controller);
    ~InputConvertGame() override;

    void mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize) override;
    void wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize) override;
    void keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize) override;
    void relativeMouseMoveEvent(const QPointF &delta, const QSize &frameSize, const QSize &showSize) override;
    bool isCurrentCustomKeymap() override;
    void cancelActiveInputs() override;

    void loadKeyMap(const QString &json);

protected:
    void timerEvent(QTimerEvent *event) override;

private slots:
    void onSteerWheelTimer();
    void onDragTimer();

private:
    void updateSize(const QSize &frameSize, const QSize &showSize);

    bool sendTouchDownEvent(int id, const QPointF &pos);
    bool sendTouchMoveEvent(int id, const QPointF &pos);
    bool sendTouchUpEvent(int id, const QPointF &pos);
    bool sendTouchEvent(int id, const QPointF &pos, AndroidMotioneventAction action);
    void sendKeyEvent(AndroidKeyeventAction action, AndroidKeycode keyCode);

    QPointF clampRelativePos(const QPointF &pos) const;
    QPointF calcFrameAbsolutePos(const QPointF &relativePos) const;
    QPointF calcScreenAbsolutePos(const QPointF &relativePos) const;

    int attachTouchID(int key);
    void detachTouchID(int key);
    void detachTouchIDByIndex(int id);
    int getTouchID(int key) const;
    int activeTouchCount() const;
    void recoverDuplicateTouch(int key, const char *reason);
    void reconcileMouseButtons(Qt::MouseButtons buttons, const char *reason);
    void updateMouseButtonWatchdog();

    void processSteerWheel(const KeyMap::KeyMapNode &node, const QKeyEvent *from);
    void processKeyClick(const QPointF &clickPos, bool clickTwice, bool switchMap, const QKeyEvent *from);
    void processKeyClickMulti(const KeyMap::DelayClickNode *nodes, int count, const QKeyEvent *from);
    void processKeyDrag(const QPointF &startPos, const QPointF &endPos,
                        quint32 startDelay, float dragSpeed, const QKeyEvent *from);
    void processAndroidKey(AndroidKeycode androidKey, const QKeyEvent *from);

    bool processMouseClick(const QMouseEvent *from);
    bool processMouseMove(const QMouseEvent *from);
    void applyMouseMoveDelta(const QPointF &delta);
    bool moveCursorTo(const QMouseEvent *from, const QPoint &localPosPixel);
    void mouseMoveStartTouch();
    void mouseMoveStopTouch();
    void startMouseMoveTimer();
    void stopMouseMoveTimer();

    bool switchGameMap();
    void hideMouseCursor(bool hide);

    void getDelayQueue(const QPointF &start, const QPointF &end,
                       double distanceStep, double positionJitter,
                       quint32 lowestTimer, quint32 highestTimer,
                       QVector<QPointF> &pathPos, QVector<quint32> &pathTimer);

    QPointF addJitter(const QPointF &pos);
    void stopSteerWheel(bool releaseTouch = true);
    void stopDrag(bool releaseTouch = true);
    void releaseAllKeys();
    void resetTouchState();
    void invalidatePendingActions();

private:
    QSize m_frameSize;
    QSize m_showSize;
    bool m_gameMap = false;
    bool m_needBackMouseMove = false;
    bool m_processMouseMove = true;
    bool m_cursorHidden = false;
    std::uint64_t m_actionEpoch = 0;

    std::array<int, MULTI_TOUCH_MAX_NUM> m_multiTouchID{};
    std::array<QPointF, MULTI_TOUCH_MAX_NUM> m_touchPositions{};
    std::array<QPoint, MULTI_TOUCH_MAX_NUM> m_lastAbsolutePositions{};
    std::array<bool, MULTI_TOUCH_MAX_NUM> m_hasLastAbsolutePosition{};

    KeyMap m_keyMap;

    struct SteerWheelState {
        int touchKey = Qt::Key_unknown;
        bool pressedUp = false;
        bool pressedDown = false;
        bool pressedLeft = false;
        bool pressedRight = false;

        struct DelayState {
            QPointF currentPos;
            QTimer *timer = nullptr;
            QVector<QPointF> pathPos;
            QVector<quint32> pathTimer;
            int stepIndex = 0;
            int pressedNum = 0;
        } delayData;
    } m_ctrlSteerWheel;

    struct MouseMoveState {
        QPointF lastConverPos;
        QPointF lastPos;
        bool touching = false;
        int timer = 0;
        bool smallEyes = false;
        QElapsedTimer paceTimer;
    } m_ctrlMouseMove;

    struct DragState {
        QPointF currentPos;
        QTimer *timer = nullptr;
        QVector<QPointF> pathPos;
        QVector<quint32> pathTimer;
        int stepIndex = 0;
        int pressKey = 0;
    } m_dragDelayData;

    QHash<int, QPointF> m_keyJitterMap;
    QSet<int> m_activeMouseButtons;
    QTimer m_mouseButtonWatchdog;
    bool m_globalMouseButtonsReliable = true;
};

#endif // INPUTCONVERTGAME_H
