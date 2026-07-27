#ifndef INPUTCONVERTBASE_H
#define INPUTCONVERTBASE_H

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QWheelEvent>

#include "controlmsg.h"

class Controller;
class InputConvertBase : public QObject
{
    Q_OBJECT
public:
    explicit InputConvertBase(Controller *controller);
    ~InputConvertBase() override;

    // the frame size may be different from the real device size, so we need the size
    // to which the absolute position apply, to scale it accordingly
    virtual void mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize) = 0;
    virtual void wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize) = 0;
    virtual void keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize) = 0;
    virtual bool isCurrentCustomKeymap()
    {
        return false;
    }
    virtual void cancelActiveInputs() {}

    // Delivers a raw, compositor-provided relative pointer motion delta (in
    // physical pixels of the show/widget surface, unaccelerated), bypassing
    // the platform's absolute-position/QMouseEvent path entirely. This is
    // fed by a native Wayland relative-pointer lock (see
    // util/mousetap/waylandmousetap.h in the QtScrcpy app), as opposed to
    // the historical warp-to-center + QMouseEvent delta reconstruction used
    // for X11/Windows/macOS. Default implementation is a no-op: only
    // InputConvertGame's relative-mouse-look feature consumes this: other
    // converters simply ignore it.
    virtual void relativeMouseMoveEvent(const QPointF &delta, const QSize &frameSize, const QSize &showSize) {
        Q_UNUSED(delta);
        Q_UNUSED(frameSize);
        Q_UNUSED(showSize);
    }

signals:
    void grabCursor(bool grab);

protected:
    void sendControlMsg(ControlMsg *msg);

    QPointer<Controller> m_controller;
    // Qt reports repeated events as a boolean, but Android expects the actual
    // number of repetitions. This variable keeps track of the count.
    unsigned m_repeat = 0;
};

#endif // INPUTCONVERTBASE_H
