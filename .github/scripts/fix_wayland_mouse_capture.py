from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(path: str, old: str, new: str) -> None:
    file_path = ROOT / path
    text = file_path.read_text(encoding="utf-8-sig")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one match, found {count}")
    file_path.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "QtScrcpy/ui/videoform.cpp",
    "#include <QApplication>\n#include <QFileInfo>\n",
    "#include <QApplication>\n#include <QCursor>\n#include <QFileInfo>\n",
)

replace_once(
    "QtScrcpy/ui/videoform.cpp",
    '''void VideoForm::setPlatformMouseGrab(bool grab)\n{\n    const QString platform = QGuiApplication::platformName();\n    const bool isXcb = platform == QLatin1String("xcb");\n    bool qtGrabbed = !grab;\n\n    // X11 already has an explicit XCB grab in MouseTap. On Wayland and\n    // Windows, use Qt's platform abstraction so compositor/security policy is\n    // respected; Windows additionally keeps ClipCursor for confinement.\n    if (!isXcb) {\n        if (QWindow *nativeWindow = windowHandle()) {\n            qtGrabbed = nativeWindow->setMouseGrabEnabled(grab);\n        } else if (!grab) {\n            qtGrabbed = true;\n        }\n    } else {\n        qtGrabbed = true;\n    }\n\n    MouseTap::getInstance()->enableMouseEventTap(getGrabCursorRect(), grab);\n    m_platformMouseGrabActive = grab && qtGrabbed;\n\n    if (grab && !qtGrabbed) {\n        qWarning() << "Mouse grab was rejected by platform:" << platform;\n    }\n    if (qsc::telemetry::enabled()) {\n        qInfo() << "[Telemetry][Input] mouse-grab"\n                << "requested=" << grab\n                << "active=" << m_platformMouseGrabActive\n                << "platform=" << platform;\n    }\n}\n''',
    '''void VideoForm::setPlatformMouseGrab(bool grab)\n{\n    if (!grab && !m_platformMouseGrabActive) return;\n\n    const QString platform = QGuiApplication::platformName();\n    const bool isWayland = platform.startsWith(QLatin1String("wayland"));\n\n    // Keep the proven platform-native paths: ClipCursor on Windows and XCB\n    // pointer grab on X11. Qt's QWindow mouse grab is intentionally not used:\n    // the Wayland plugin rejects it for normal top-level windows and it also\n    // duplicates the native Windows confinement path.\n    MouseTap::getInstance()->enableMouseEventTap(getGrabCursorRect(), grab);\n    m_platformMouseGrabActive = grab;\n\n    // Native Wayland has no grab implementation in MouseTap. The existing\n    // game-input path confines the pointer by periodically warping it back to\n    // the video center, so seed that state immediately when custom mode starts.\n    if (grab && m_videoWidget) {\n        const QPoint center = m_videoWidget->mapToGlobal(\n            m_videoWidget->rect().center());\n        QCursor::setPos(center);\n    }\n\n    if (qsc::telemetry::enabled()) {\n        qInfo() << "[Telemetry][Input] mouse-grab"\n                << "requested=" << grab\n                << "active=" << m_platformMouseGrabActive\n                << "strategy=" << (isWayland ? "recenter" : "native")\n                << "platform=" << platform;\n    }\n}\n''',
)

replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp",
    '''    // Native Wayland intentionally forbids global pointer warping. Rely on\n    // QWindow mouse grab/pointer constraints instead of fighting the compositor.\n    if (QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {\n        return false;\n    }\n''',
    '''    // QWindow mouse grabbing is unavailable for normal Wayland windows in\n    // Qt 5.15. Keep the historical cursor-warp fallback; compositors that\n    // support Qt cursor positioning will confine the FPS camera as before.\n''',
)

# Source-level invariants.
video = (ROOT / "QtScrcpy/ui/videoform.cpp").read_text(encoding="utf-8")
input_game = (ROOT / "QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp").read_text(encoding="utf-8")
assert "setMouseGrabEnabled" not in video
assert '"strategy=" << (isWayland ? "recenter" : "native")' in video
assert "QCursor::setPos(global - offset);" in input_game
assert 'platformName().startsWith(QLatin1String("wayland"))' not in input_game

print("Wayland mouse capture regression fixed")
