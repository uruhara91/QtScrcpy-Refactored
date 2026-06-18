from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2] if '.github' in Path(__file__).parts else Path.cwd()


def replace_once(path: str, old: str, new: str) -> None:
    file_path = ROOT / path
    text = file_path.read_text(encoding='utf-8-sig')
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'{path}: expected exactly one match, found {count}: {old[:120]!r}')
    text = text.replace(old, new, 1)
    file_path.write_text(text, encoding='utf-8')


# scrcpy-server 3.3.4 uses the video-specific option names.
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/server/server.cpp',
    'args << QString("codec_options=%1").arg(m_params.codecOptions);',
    'args << QString("video_codec_options=%1").arg(m_params.codecOptions);',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/server/server.cpp',
    'args << QString("encoder_name=%1").arg(m_params.codecName);',
    'args << QString("video_encoder=%1").arg(m_params.codecName);',
)

# A common recovery hook lets the UI atomically release every emulated touch/key state.
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertbase.h',
    '''    virtual bool isCurrentCustomKeymap()\n    {\n        return false;\n    }\n''',
    '''    virtual bool isCurrentCustomKeymap()\n    {\n        return false;\n    }\n    virtual void cancelActiveInputs() {}\n''',
)

replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.h',
    '#include <QHash>\n',
    '#include <QHash>\n#include <QSet>\n',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.h',
    '''    bool isCurrentCustomKeymap() override;\n\n    void loadKeyMap(const QString &json);\n''',
    '''    bool isCurrentCustomKeymap() override;\n    void cancelActiveInputs() override;\n\n    void loadKeyMap(const QString &json);\n''',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.h',
    '''    int attachTouchID(int key);\n    void detachTouchID(int key);\n    void detachTouchIDByIndex(int id);\n    int getTouchID(int key) const;\n''',
    '''    int attachTouchID(int key);\n    void detachTouchID(int key);\n    void detachTouchIDByIndex(int id);\n    int getTouchID(int key) const;\n    int activeTouchCount() const;\n    void recoverDuplicateTouch(int key, const char *reason);\n    void reconcileMouseButtons(Qt::MouseButtons buttons);\n    void updateMouseButtonWatchdog();\n''',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.h',
    '    void moveCursorTo(const QMouseEvent *from, const QPoint &localPosPixel);\n',
    '    bool moveCursorTo(const QMouseEvent *from, const QPoint &localPosPixel);\n',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.h',
    '''    QHash<int, QPointF> m_keyJitterMap;\n};\n''',
    '''    QHash<int, QPointF> m_keyJitterMap;\n    QSet<int> m_activeMouseButtons;\n    QTimer m_mouseButtonWatchdog;\n};\n''',
)

replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp',
    '#include "../controller.h"\n',
    '#include "../controller.h"\n#include "qtscrcpytelemetry.h"\n',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp',
    '''    connect(m_dragDelayData.timer, &QTimer::timeout,\n            this, &InputConvertGame::onDragTimer);\n}\n''',
    '''    connect(m_dragDelayData.timer, &QTimer::timeout,\n            this, &InputConvertGame::onDragTimer);\n\n    // This timer is only active while a physical mouse button is held. It\n    // recovers a release event lost by the compositor, USB stack, or a focus\n    // transition without adding latency to the normal event path.\n    m_mouseButtonWatchdog.setInterval(100);\n    m_mouseButtonWatchdog.setTimerType(Qt::CoarseTimer);\n    connect(&m_mouseButtonWatchdog, &QTimer::timeout, this, [this]() {\n        reconcileMouseButtons(QGuiApplication::mouseButtons());\n    });\n}\n''',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp',
    '''bool InputConvertGame::isCurrentCustomKeymap()\n{\n    return m_gameMap;\n}\n\nvoid InputConvertGame::loadKeyMap''',
    '''bool InputConvertGame::isCurrentCustomKeymap()\n{\n    return m_gameMap;\n}\n\nvoid InputConvertGame::cancelActiveInputs()\n{\n    const int activeTouches = activeTouchCount();\n    if (qsc::telemetry::enabled() && activeTouches > 0) {\n        qInfo() << "[Telemetry][Input] cancel-active-inputs"\n                << "activeTouches=" << activeTouches\n                << "mouseButtons=" << m_activeMouseButtons.size();\n    }\n    releaseAllKeys();\n}\n\nvoid InputConvertGame::loadKeyMap''',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp',
    '''    if (key == node.data.steerWheel.up.key) m_ctrlSteerWheel.pressedUp = pressed;\n    else if (key == node.data.steerWheel.right.key) m_ctrlSteerWheel.pressedRight = pressed;\n    else if (key == node.data.steerWheel.down.key) m_ctrlSteerWheel.pressedDown = pressed;\n    else if (key == node.data.steerWheel.left.key) m_ctrlSteerWheel.pressedLeft = pressed;\n    else return;\n''',
    '''    bool *pressedState = nullptr;\n    if (key == node.data.steerWheel.up.key) pressedState = &m_ctrlSteerWheel.pressedUp;\n    else if (key == node.data.steerWheel.right.key) pressedState = &m_ctrlSteerWheel.pressedRight;\n    else if (key == node.data.steerWheel.down.key) pressedState = &m_ctrlSteerWheel.pressedDown;\n    else if (key == node.data.steerWheel.left.key) pressedState = &m_ctrlSteerWheel.pressedLeft;\n    else return;\n\n    // A second physical press while our state is still down means that the\n    // previous release was lost. Reset the wheel before accepting the press.\n    if (pressed && *pressedState) {\n        if (qsc::telemetry::enabled()) {\n            qInfo() << "[Telemetry][Input] recover-steer-wheel"\n                    << "key=" << key;\n        }\n        stopSteerWheel(true);\n    }\n    *pressedState = pressed;\n''',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp',
    '''    if (from->type() == QEvent::KeyPress) {\n        const QPointF position = addJitter(clickPos);\n''',
    '''    if (from->type() == QEvent::KeyPress) {\n        recoverDuplicateTouch(key, "duplicate-key-down");\n        const QPointF position = addJitter(clickPos);\n''',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp',
    '''int InputConvertGame::getTouchID(int key) const\n{\n    if (key == 0 || key == Qt::Key_unknown) return -1;\n    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; ++i) {\n        if (m_multiTouchID[i] == key) return i;\n    }\n    return -1;\n}\n\nvoid InputConvertGame::getDelayQueue''',
    '''int InputConvertGame::getTouchID(int key) const\n{\n    if (key == 0 || key == Qt::Key_unknown) return -1;\n    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; ++i) {\n        if (m_multiTouchID[i] == key) return i;\n    }\n    return -1;\n}\n\nint InputConvertGame::activeTouchCount() const\n{\n    return static_cast<int>(std::count_if(\n        m_multiTouchID.cbegin(), m_multiTouchID.cend(),\n        [](int key) { return key != 0; }));\n}\n\nvoid InputConvertGame::recoverDuplicateTouch(int key, const char *reason)\n{\n    const int id = getTouchID(key);\n    if (id < 0) return;\n\n    const QPointF lastPosition = m_touchPositions[id];\n    (void)sendTouchUpEvent(id, lastPosition);\n    detachTouchIDByIndex(id);\n    m_activeMouseButtons.remove(key);\n\n    if (qsc::telemetry::enabled()) {\n        qInfo() << "[Telemetry][Input] forced-release"\n                << "reason=" << reason\n                << "key=" << key\n                << "activeTouches=" << activeTouchCount();\n    }\n    updateMouseButtonWatchdog();\n}\n\nvoid InputConvertGame::reconcileMouseButtons(Qt::MouseButtons buttons)\n{\n    const QList<int> activeButtons = m_activeMouseButtons.values();\n    for (int key : activeButtons) {\n        const auto button = static_cast<Qt::MouseButton>(key);\n        if (buttons.testFlag(button)) continue;\n\n        const int id = getTouchID(key);\n        if (id >= 0) {\n            (void)sendTouchUpEvent(id, m_touchPositions[id]);\n            detachTouchIDByIndex(id);\n        }\n        m_activeMouseButtons.remove(key);\n\n        if (qsc::telemetry::enabled()) {\n            qInfo() << "[Telemetry][Input] forced-release"\n                    << "reason=host-button-state"\n                    << "button=" << key\n                    << "activeTouches=" << activeTouchCount();\n        }\n    }\n    updateMouseButtonWatchdog();\n}\n\nvoid InputConvertGame::updateMouseButtonWatchdog()\n{\n    if (m_activeMouseButtons.isEmpty()) {\n        m_mouseButtonWatchdog.stop();\n    } else if (!m_mouseButtonWatchdog.isActive()) {\n        m_mouseButtonWatchdog.start();\n    }\n}\n\nvoid InputConvertGame::getDelayQueue''',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp',
    '''    if (from->type() == QEvent::MouseButtonPress ||\n        from->type() == QEvent::MouseButtonDblClick) {\n        const int id = attachTouchID(key);\n        if (id >= 0 && sendTouchDownEvent(id, node.data.click.keyNode.pos)) return true;\n        if (id >= 0) detachTouchIDByIndex(id);\n        return true;\n    }\n\n    if (from->type() == QEvent::MouseButtonRelease) {\n        const int id = getTouchID(key);\n        if (id >= 0) {\n            sendTouchUpEvent(id, node.data.click.keyNode.pos);\n            detachTouchIDByIndex(id);\n        }\n        return true;\n    }\n''',
    '''    if (from->type() == QEvent::MouseButtonPress ||\n        from->type() == QEvent::MouseButtonDblClick) {\n        recoverDuplicateTouch(key, "duplicate-mouse-down");\n        const int id = attachTouchID(key);\n        if (id >= 0 && sendTouchDownEvent(id, node.data.click.keyNode.pos)) {\n            m_activeMouseButtons.insert(key);\n            updateMouseButtonWatchdog();\n            return true;\n        }\n        if (id >= 0) detachTouchIDByIndex(id);\n        m_activeMouseButtons.remove(key);\n        updateMouseButtonWatchdog();\n        return true;\n    }\n\n    if (from->type() == QEvent::MouseButtonRelease) {\n        const int id = getTouchID(key);\n        if (id >= 0) {\n            sendTouchUpEvent(id, node.data.click.keyNode.pos);\n            detachTouchIDByIndex(id);\n        }\n        m_activeMouseButtons.remove(key);\n        updateMouseButtonWatchdog();\n        return true;\n    }\n''',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp',
    '''    if (!from || from->type() != QEvent::MouseMove ||\n        m_showSize.width() <= 0 || m_showSize.height() <= 0) {\n        return false;\n    }\n\n    const QPoint center''',
    '''    if (!from || from->type() != QEvent::MouseMove ||\n        m_showSize.width() <= 0 || m_showSize.height() <= 0) {\n        return false;\n    }\n\n    reconcileMouseButtons(from->buttons());\n\n    const QPoint center''',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp',
    '''                mouseMoveStopTouch();\n                moveCursorTo(from, center);\n                m_ctrlMouseMove.lastPos = center;\n                return true;\n''',
    '''                mouseMoveStopTouch();\n                if (moveCursorTo(from, center)) {\n                    m_ctrlMouseMove.lastPos = center;\n                } else {\n                    m_ctrlMouseMove.lastPos = current;\n                }\n                return true;\n''',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp',
    '''    if (dx * dx + dy * dy > 2500) {\n        moveCursorTo(from, center);\n        m_ctrlMouseMove.lastPos = center;\n    }\n''',
    '''    if (dx * dx + dy * dy > 2500 && moveCursorTo(from, center)) {\n        m_ctrlMouseMove.lastPos = center;\n    }\n''',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp',
    '''void InputConvertGame::moveCursorTo(const QMouseEvent *from,\n                                    const QPoint &localPosPixel)\n{\n    if (!from) return;\n''',
    '''bool InputConvertGame::moveCursorTo(const QMouseEvent *from,\n                                    const QPoint &localPosPixel)\n{\n    if (!from) return false;\n\n    // Native Wayland intentionally forbids global pointer warping. Rely on\n    // QWindow mouse grab/pointer constraints instead of fighting the compositor.\n    if (QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {\n        return false;\n    }\n''',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp',
    '''    QCursor::setPos(global - offset);\n}\n\nvoid InputConvertGame::mouseMoveStartTouch()''',
    '''    QCursor::setPos(global - offset);\n    return true;\n}\n\nvoid InputConvertGame::mouseMoveStartTouch()''',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp',
    '''    invalidatePendingActions();\n    stopMouseMoveTimer();\n    if (m_ctrlSteerWheel.delayData.timer) m_ctrlSteerWheel.delayData.timer->stop();\n''',
    '''    invalidatePendingActions();\n    stopMouseMoveTimer();\n    m_mouseButtonWatchdog.stop();\n    if (m_ctrlSteerWheel.delayData.timer) m_ctrlSteerWheel.delayData.timer->stop();\n''',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp',
    '''    resetTouchState();\n    stopSteerWheel(false);\n''',
    '''    resetTouchState();\n    m_activeMouseButtons.clear();\n    stopSteerWheel(false);\n''',
)

# Forward recovery from the public device interface down to the converter.
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/controller.h',
    '''    void updateScript(QString gameScript = "");\n    bool isCurrentCustomKeymap();\n''',
    '''    void updateScript(QString gameScript = "");\n    bool isCurrentCustomKeymap();\n    void cancelActiveInputs();\n''',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/controller/controller.cpp',
    '''bool Controller::isCurrentCustomKeymap()\n{\n    return m_inputConvert && m_inputConvert->isCurrentCustomKeymap();\n}\n\nvoid Controller::postBackOrScreenOn''',
    '''bool Controller::isCurrentCustomKeymap()\n{\n    return m_inputConvert && m_inputConvert->isCurrentCustomKeymap();\n}\n\nvoid Controller::cancelActiveInputs()\n{\n    if (m_inputConvert) m_inputConvert->cancelActiveInputs();\n}\n\nvoid Controller::postBackOrScreenOn''',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/include/QtScrcpyCore.h',
    '''    virtual void updateScript(QString script) = 0;\n    virtual bool isCurrentCustomKeymap() = 0;\n''',
    '''    virtual void updateScript(QString script) = 0;\n    virtual bool isCurrentCustomKeymap() = 0;\n    virtual void cancelActiveInputs() = 0;\n''',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/device.h',
    '''    void updateScript(QString script) override;\n    bool isCurrentCustomKeymap() override;\n''',
    '''    void updateScript(QString script) override;\n    bool isCurrentCustomKeymap() override;\n    void cancelActiveInputs() override;\n''',
)
replace_once(
    'QtScrcpy/QtScrcpyCore/src/device/device.cpp',
    '''bool Device::isCurrentCustomKeymap()\n{\n    return m_controller && m_controller->isCurrentCustomKeymap();\n}\n\nbool Device::saveFrame''',
    '''bool Device::isCurrentCustomKeymap()\n{\n    return m_controller && m_controller->isCurrentCustomKeymap();\n}\n\nvoid Device::cancelActiveInputs()\n{\n    if (m_controller) m_controller->cancelActiveInputs();\n}\n\nbool Device::saveFrame''',
)

# Platform-safe cursor capture and focus-loss recovery in the UI.
replace_once(
    'QtScrcpy/ui/videoform.h',
    '''    void syncFpsCounterState();\n\n    void updateStyleSheet(bool vertical);\n''',
    '''    void syncFpsCounterState();\n    void cancelActiveInputs(const char *reason);\n    void setPlatformMouseGrab(bool grab);\n    void restorePlatformMouseGrab();\n\n    void updateStyleSheet(bool vertical);\n''',
)
replace_once(
    'QtScrcpy/ui/videoform.h',
    '''protected:\n    void mousePressEvent(QMouseEvent *event) override;\n''',
    '''protected:\n    bool event(QEvent *event) override;\n    void mousePressEvent(QMouseEvent *event) override;\n''',
)
replace_once(
    'QtScrcpy/ui/videoform.h',
    '''    std::atomic<int> m_latestFrameHeight{0};\n    std::atomic_bool m_frameUiUpdatePending{false};\n};\n''',
    '''    std::atomic<int> m_latestFrameHeight{0};\n    std::atomic_bool m_frameUiUpdatePending{false};\n\n    bool m_cursorGrabRequested = false;\n    bool m_platformMouseGrabActive = false;\n};\n''',
)
replace_once(
    'QtScrcpy/ui/videoform.cpp',
    '#include <QFileInfo>\n',
    '#include <QApplication>\n#include <QFileInfo>\n',
)
replace_once(
    'QtScrcpy/ui/videoform.cpp',
    '#include "videoform.h"\n',
    '#include "videoform.h"\n#include "qtscrcpytelemetry.h"\n',
)
replace_once(
    'QtScrcpy/ui/videoform.cpp',
    '''    if (framelessWindow) {\n        setWindowFlags(windowFlags() | Qt::FramelessWindowHint);\n    }\n}\n\nVideoForm::~VideoForm()\n{\n    delete ui;\n}\n''',
    '''    if (framelessWindow) {\n        setWindowFlags(windowFlags() | Qt::FramelessWindowHint);\n    }\n\n    connect(qApp, &QGuiApplication::applicationStateChanged,\n            this, [this](Qt::ApplicationState state) {\n        if (state == Qt::ApplicationActive) {\n            QTimer::singleShot(0, this, [this]() { restorePlatformMouseGrab(); });\n        } else {\n            cancelActiveInputs("application-inactive");\n            setPlatformMouseGrab(false);\n        }\n    });\n}\n\nVideoForm::~VideoForm()\n{\n    cancelActiveInputs("destructor");\n    m_cursorGrabRequested = false;\n    setPlatformMouseGrab(false);\n    delete ui;\n}\n''',
)
replace_once(
    'QtScrcpy/ui/videoform.cpp',
    '''void VideoForm::grabCursor(bool grab)\n{\n    QRect rc = getGrabCursorRect();\n    MouseTap::getInstance()->enableMouseEventTap(rc, grab);\n}\n''',
    '''void VideoForm::grabCursor(bool grab)\n{\n    m_cursorGrabRequested = grab;\n    if (!grab) {\n        setPlatformMouseGrab(false);\n        return;\n    }\n\n    QTimer::singleShot(0, this, [this]() { restorePlatformMouseGrab(); });\n}\n\nvoid VideoForm::cancelActiveInputs(const char *reason)\n{\n    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);\n    if (device) device->cancelActiveInputs();\n\n    if (qsc::telemetry::enabled()) {\n        qInfo() << "[Telemetry][Input] ui-cancel"\n                << "reason=" << reason\n                << "serial=" << m_serial;\n    }\n}\n\nvoid VideoForm::setPlatformMouseGrab(bool grab)\n{\n    const QString platform = QGuiApplication::platformName();\n    const bool isXcb = platform == QLatin1String("xcb");\n    bool qtGrabbed = !grab;\n\n    // X11 already has an explicit XCB grab in MouseTap. On Wayland and\n    // Windows, use Qt's platform abstraction so compositor/security policy is\n    // respected; Windows additionally keeps ClipCursor for confinement.\n    if (!isXcb) {\n        if (QWindow *nativeWindow = windowHandle()) {\n            qtGrabbed = nativeWindow->setMouseGrabEnabled(grab);\n        } else if (!grab) {\n            qtGrabbed = true;\n        }\n    } else {\n        qtGrabbed = true;\n    }\n\n    MouseTap::getInstance()->enableMouseEventTap(getGrabCursorRect(), grab);\n    m_platformMouseGrabActive = grab && qtGrabbed;\n\n    if (grab && !qtGrabbed) {\n        qWarning() << "Mouse grab was rejected by platform:" << platform;\n    }\n    if (qsc::telemetry::enabled()) {\n        qInfo() << "[Telemetry][Input] mouse-grab"\n                << "requested=" << grab\n                << "active=" << m_platformMouseGrabActive\n                << "platform=" << platform;\n    }\n}\n\nvoid VideoForm::restorePlatformMouseGrab()\n{\n    if (!m_cursorGrabRequested || !isVisible() || !isActiveWindow() ||\n        QGuiApplication::applicationState() != Qt::ApplicationActive) {\n        return;\n    }\n    if (!m_platformMouseGrabActive) setPlatformMouseGrab(true);\n}\n''',
)
replace_once(
    'QtScrcpy/ui/videoform.cpp',
    '''void VideoForm::mousePressEvent(QMouseEvent *event)\n{\n''',
    '''bool VideoForm::event(QEvent *event)\n{\n    if (event) {\n        switch (event->type()) {\n        case QEvent::WindowDeactivate:\n        case QEvent::Hide:\n        case QEvent::Close:\n            cancelActiveInputs("window-inactive");\n            setPlatformMouseGrab(false);\n            break;\n        case QEvent::WindowActivate:\n        case QEvent::Show:\n            QTimer::singleShot(0, this, [this]() { restorePlatformMouseGrab(); });\n            break;\n        case QEvent::WindowStateChange:\n            if (windowState().testFlag(Qt::WindowMinimized)) {\n                cancelActiveInputs("window-minimized");\n                setPlatformMouseGrab(false);\n            } else {\n                QTimer::singleShot(0, this, [this]() { restorePlatformMouseGrab(); });\n            }\n            break;\n        default:\n            break;\n        }\n    }\n    return QWidget::event(event);\n}\n\nvoid VideoForm::mousePressEvent(QMouseEvent *event)\n{\n''',
)
replace_once(
    'QtScrcpy/ui/videoform.cpp',
    '''void VideoForm::showEvent(QShowEvent *event)\n{\n    Q_UNUSED(event)\n''',
    '''void VideoForm::showEvent(QShowEvent *event)\n{\n    Q_UNUSED(event)\n    QTimer::singleShot(0, this, [this]() { restorePlatformMouseGrab(); });\n''',
)
replace_once(
    'QtScrcpy/ui/videoform.cpp',
    '''void VideoForm::closeEvent(QCloseEvent *event)\n{\n    Q_UNUSED(event)\n    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);\n''',
    '''void VideoForm::closeEvent(QCloseEvent *event)\n{\n    Q_UNUSED(event)\n    cancelActiveInputs("close");\n    m_cursorGrabRequested = false;\n    setPlatformMouseGrab(false);\n    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);\n''',
)

# Basic source-level invariants for the generated commit.
checks = {
    'QtScrcpy/QtScrcpyCore/src/device/server/server.cpp': [
        'video_codec_options=%1', 'video_encoder=%1'],
    'QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp': [
        'cancelActiveInputs()', 'forced-release', 'host-button-state'],
    'QtScrcpy/ui/videoform.cpp': [
        'setMouseGrabEnabled', 'applicationStateChanged', 'window-inactive'],
}
for path, needles in checks.items():
    text = (ROOT / path).read_text(encoding='utf-8')
    for needle in needles:
        if needle not in text:
            raise RuntimeError(f'{path}: missing generated invariant {needle!r}')

print('Realtime fixes applied successfully')
