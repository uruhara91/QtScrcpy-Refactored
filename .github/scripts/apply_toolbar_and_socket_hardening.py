from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(path: str, old: str, new: str) -> None:
    file_path = ROOT / path
    text = file_path.read_text(encoding="utf-8-sig")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected exactly one match, found {count}: {old[:120]!r}")
    file_path.write_text(text.replace(old, new, 1), encoding="utf-8")


# Toolbar dragging: use compositor-approved native system move on Qt 5.15+
# (required by Wayland and robust on Windows), with a global-position manual
# fallback for older/unsupported platforms.
replace_once(
    "QtScrcpy/ui/toolform.h",
    "    QPoint m_dragPosition;\n",
    "    QPoint m_dragPosition;\n    bool m_manualDragActive = false;\n",
)

replace_once(
    "QtScrcpy/ui/toolform.cpp",
    '#include <QShowEvent>\n',
    '#include <QShowEvent>\n#include <QWindow>\n',
)

replace_once(
    "QtScrcpy/ui/toolform.cpp",
    '#include "../groupcontroller/groupcontroller.h"\n\n',
    '''#include "../groupcontroller/groupcontroller.h"\n\nnamespace {\nQPoint mouseGlobalPosition(const QMouseEvent *event)\n{\n    if (!event) return {};\n#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)\n    return event->globalPosition().toPoint();\n#else\n    return event->globalPos();\n#endif\n}\n}\n\n''',
)

replace_once(
    "QtScrcpy/ui/toolform.cpp",
    '''void ToolForm::mousePressEvent(QMouseEvent *event)\n{\n    if (event->button() == Qt::LeftButton) {\n        m_dragPosition = event->pos(); \n        event->accept();\n    }\n}\n\nvoid ToolForm::mouseReleaseEvent(QMouseEvent *event)\n{\n    Q_UNUSED(event)\n}\n\nvoid ToolForm::mouseMoveEvent(QMouseEvent *event)\n{\n    if (event->buttons() & Qt::LeftButton) {\n        move(pos() + (event->pos() - m_dragPosition));\n        event->accept();\n    }\n}\n''',
    '''void ToolForm::mousePressEvent(QMouseEvent *event)\n{\n    if (!event || event->button() != Qt::LeftButton) {\n        MagneticWidget::mousePressEvent(event);\n        return;\n    }\n\n    m_manualDragActive = false;\n#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)\n    // Wayland forbids clients from positioning top-level windows directly.\n    // startSystemMove() hands the drag to the compositor and is also the most\n    // reliable path for frameless tool windows on Windows 11.\n    if (QWindow *window = windowHandle(); window && window->startSystemMove()) {\n        event->accept();\n        return;\n    }\n#endif\n\n    m_dragPosition = mouseGlobalPosition(event) - frameGeometry().topLeft();\n    m_manualDragActive = true;\n    event->accept();\n}\n\nvoid ToolForm::mouseReleaseEvent(QMouseEvent *event)\n{\n    m_manualDragActive = false;\n    if (event) event->accept();\n}\n\nvoid ToolForm::mouseMoveEvent(QMouseEvent *event)\n{\n    if (event && m_manualDragActive &&\n        event->buttons().testFlag(Qt::LeftButton)) {\n        move(mouseGlobalPosition(event) - m_dragPosition);\n        event->accept();\n        return;\n    }\n    MagneticWidget::mouseMoveEvent(event);\n}\n''',
)

# Preserve a user-detached toolbar position. The magnetic widget still follows
# the video window when adsorbed, but hiding/showing it no longer resets it.
replace_once(
    "QtScrcpy/ui/videoform.cpp",
    '''void VideoForm::showToolForm(bool show)\n{\n    if (!m_toolForm) {\n        m_toolForm = new ToolForm(this, ToolForm::AP_OUTSIDE_RIGHT);\n        m_toolForm->setSerial(m_serial);\n    }\n    m_toolForm->move(pos().x() + geometry().width(), pos().y() + 30);\n    m_toolForm->setVisible(show);\n}\n''',
    '''void VideoForm::showToolForm(bool show)\n{\n    if (!m_toolForm) {\n        m_toolForm = new ToolForm(this, ToolForm::AP_OUTSIDE_RIGHT);\n        m_toolForm->setSerial(m_serial);\n        m_toolForm->move(pos().x() + geometry().width(), pos().y() + 30);\n    }\n    m_toolForm->setVisible(show);\n    if (show) m_toolForm->raise();\n}\n''',
)

# Server handshake: read the exact metadata size instead of assuming a single
# QTcpSocket::read() returns all bytes after bytesAvailable() crosses a threshold.
replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/server/server.cpp",
    '#include <QTimerEvent>\n',
    '#include <QTimerEvent>\n#include <array>\n#include <algorithm>\n',
)

replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/server/server.cpp",
    '''bool Server::readInfo(VideoSocket *videoSocket, QString &deviceName, QSize &size)\n{\n    QElapsedTimer timer;\n    timer.start();\n    unsigned char buf[DEVICE_NAME_FIELD_LENGTH + 12];\n    while (videoSocket->bytesAvailable() < (DEVICE_NAME_FIELD_LENGTH + 12)) {\n        videoSocket->waitForReadyRead(300);\n        if (timer.elapsed() > 3000) {\n            qInfo("readInfo timeout");\n            return false;\n        }\n    }\n    qDebug() << "readInfo wait time:" << timer.elapsed();\n\n    qint64 len = videoSocket->read((char *)buf, sizeof(buf));\n    if (len < DEVICE_NAME_FIELD_LENGTH + 12) {\n        qInfo("Could not retrieve device information");\n        return false;\n    }\n    buf[DEVICE_NAME_FIELD_LENGTH - 1] = '\\0'; // in case the client sends garbage\n    deviceName = QString::fromUtf8((const char *)buf);\n\n    // 前4个字节是AVCodecID,当前只支持H264,所以先不解析\n    size.setWidth(bufferRead32be(&buf[DEVICE_NAME_FIELD_LENGTH + 4]));\n    size.setHeight(bufferRead32be(&buf[DEVICE_NAME_FIELD_LENGTH + 8]));\n\n    return true;\n}\n''',
    '''bool Server::readInfo(VideoSocket *videoSocket, QString &deviceName, QSize &size)\n{\n    if (!videoSocket) return false;\n\n    constexpr qint64 infoSize = DEVICE_NAME_FIELD_LENGTH + 12;\n    constexpr qint64 timeoutMs = 3000;\n    std::array<quint8, static_cast<std::size_t>(infoSize)> buf{};\n\n    QElapsedTimer timer;\n    timer.start();\n    qint64 totalRead = 0;\n    while (totalRead < infoSize) {\n        if (videoSocket->bytesAvailable() <= 0) {\n            const qint64 remaining = timeoutMs - timer.elapsed();\n            if (remaining <= 0 ||\n                !videoSocket->waitForReadyRead(\n                    static_cast<int>(std::min<qint64>(300, remaining)))) {\n                if (timer.elapsed() >= timeoutMs ||\n                    videoSocket->state() != QAbstractSocket::ConnectedState) {\n                    qInfo("readInfo timeout or disconnect");\n                    return false;\n                }\n                continue;\n            }\n        }\n\n        const qint64 chunk = videoSocket->read(\n            reinterpret_cast<char *>(buf.data() + totalRead),\n            infoSize - totalRead);\n        if (chunk < 0) {\n            qInfo("Could not retrieve device information");\n            return false;\n        }\n        if (chunk == 0) continue;\n        totalRead += chunk;\n    }\n    qDebug() << "readInfo wait time:" << timer.elapsed();\n\n    buf[DEVICE_NAME_FIELD_LENGTH - 1] = '\\0';\n    deviceName = QString::fromUtf8(\n        reinterpret_cast<const char *>(buf.data()));\n\n    // The first 4 bytes after the device name are the codec id (H.264 here).\n    size.setWidth(bufferRead32be(&buf[DEVICE_NAME_FIELD_LENGTH + 4]));\n    size.setHeight(bufferRead32be(&buf[DEVICE_NAME_FIELD_LENGTH + 8]));\n    return size.isValid();\n}\n''',
)

checks = {
    "QtScrcpy/ui/toolform.cpp": ["startSystemMove", "m_manualDragActive"],
    "QtScrcpy/ui/videoform.cpp": ["if (show) m_toolForm->raise();"],
    "QtScrcpy/QtScrcpyCore/src/device/server/server.cpp": ["constexpr qint64 infoSize", "totalRead += chunk"],
    "QtScrcpy/QtScrcpyCore/src/device/server/tcpserver.cpp": ["LowDelayOption", "KeepAliveOption"],
}
for path, needles in checks.items():
    text = (ROOT / path).read_text(encoding="utf-8")
    for needle in needles:
        if needle not in text:
            raise RuntimeError(f"{path}: missing invariant {needle!r}")

print("Toolbar and socket hardening applied")
