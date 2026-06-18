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


replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/server/server.h",
    '#include <QObject>\n#include <QPointer>\n#include <QSize>\n',
    '#include <QElapsedTimer>\n#include <QObject>\n#include <QPointer>\n#include <QSize>\n',
)
replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/server/server.h",
    '''    quint32 m_connectCount = 0;\n    quint32 m_restartCount = 0;\n''',
    '''    quint32 m_connectCount = 0;\n    quint32 m_restartCount = 0;\n    QElapsedTimer m_forwardConnectElapsed;\n''',
)

replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/server/server.cpp",
    '#include <array>\n#include <algorithm>\n',
    '#include <array>\n#include <algorithm>\n#include <memory>\n',
)
replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/server/server.cpp",
    '''#define MAX_CONNECT_COUNT 30\n#define MAX_RESTART_COUNT 1\n''',
    '''#define MAX_CONNECT_COUNT 100\n#define MAX_RESTART_COUNT 1\n#define CONNECT_RETRY_INTERVAL_MS 100\n#define CONNECT_PROBE_TIMEOUT_MS 300\n#define CONNECT_TOTAL_TIMEOUT_MS 10000\n''',
)

replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/server/server.cpp",
    '''void Server::startConnectTimeoutTimer()\n{\n    stopConnectTimeoutTimer();\n    m_connectTimeoutTimer = startTimer(300);\n}\n''',
    '''void Server::startConnectTimeoutTimer()\n{\n    stopConnectTimeoutTimer();\n    m_forwardConnectElapsed.start();\n    m_connectTimeoutTimer = startTimer(\n        CONNECT_RETRY_INTERVAL_MS, Qt::PreciseTimer);\n    // Do not add a full retry interval before the first probe.\n    QTimer::singleShot(0, this, [this]() {\n        if (m_connectTimeoutTimer && m_tunnelForward &&\n            m_serverStartStep == SSS_RUNNING) {\n            onConnectTimer();\n        }\n    });\n}\n''',
)
replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/server/server.cpp",
    '''    m_connectCount = 0;\n}\n\nvoid Server::onConnectTimer()\n''',
    '''    m_connectCount = 0;\n    m_forwardConnectElapsed.invalidate();\n}\n\nvoid Server::onConnectTimer()\n''',
)

start = '''void Server::onConnectTimer()\n{\n    // device server need time to start\n    // 这里连接太早时间不够导致安卓监听socket还没有建立，readInfo会失败，所以采取定时重试策略\n    // 每隔100ms尝试一次，最多尝试MAX_CONNECT_COUNT次\n    QString deviceName;\n    QSize deviceSize;\n    bool success = false;\n\n    VideoSocket *videoSocket = new VideoSocket();\n    QTcpSocket *controlSocket = new QTcpSocket();\n\n    videoSocket->connectToHost(QHostAddress::LocalHost, m_params.localPort);\n    videoSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);\n    videoSocket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);\n    if (!videoSocket->waitForConnected(1000)) {\n        // 连接到adb很快的，这里失败不重试\n        m_connectCount = MAX_CONNECT_COUNT;\n        qWarning("video socket connect to server failed");\n        goto result;\n    }\n\n    controlSocket->connectToHost(QHostAddress::LocalHost, m_params.localPort);\n    controlSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);\n    controlSocket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);\n    if (!controlSocket->waitForConnected(1000)) {\n        // 连接到adb很快的，这里失败不重试\n        m_connectCount = MAX_CONNECT_COUNT;\n        qWarning("control socket connect to server failed");\n        goto result;\n    }\n\n    if (QTcpSocket::ConnectedState == videoSocket->state()) {\n        // connect will success even if devices offline, recv data is real connect success\n        // because connect is to pc adb server\n        videoSocket->waitForReadyRead(1000);\n        // devices will send 1 byte first on tunnel forward mode\n        QByteArray data = videoSocket->read(1);\n        if (!data.isEmpty() && readInfo(videoSocket, deviceName, deviceSize)) {\n            success = true;\n            goto result;\n        } else {\n            qWarning("video socket connect to server read device info failed, try again");\n            goto result;\n        }\n    } else {\n        qWarning("connect to server failed");\n        m_connectCount = MAX_CONNECT_COUNT;\n        goto result;\n    }\n\nresult:\n    if (success) {\n        stopConnectTimeoutTimer();\n        m_videoSocket = videoSocket;\n        // devices will send 1 byte first on tunnel forward mode\n        controlSocket->read(1);\n        m_controlSocket = controlSocket;\n        // we don't need the adb tunnel anymore\n        disableTunnelForward();\n        m_tunnelEnabled = false;\n        m_restartCount = 0;\n        emit serverStarted(success, deviceName, deviceSize);\n        return;\n    }\n\n    if (videoSocket) {\n        videoSocket->deleteLater();\n    }\n    if (controlSocket) {\n        controlSocket->deleteLater();\n    }\n\n    if (MAX_CONNECT_COUNT <= m_connectCount++) {\n        stopConnectTimeoutTimer();\n        stop();\n        if (MAX_RESTART_COUNT > m_restartCount++) {\n            qWarning("restart server auto");\n            start(m_params);\n        } else {\n            m_restartCount = 0;\n            emit serverStarted(false);\n        }\n    }\n}\n'''

replacement = '''void Server::onConnectTimer()\n{\n    if (!m_tunnelForward || m_serverStartStep != SSS_RUNNING) {\n        stopConnectTimeoutTimer();\n        return;\n    }\n\n    const quint32 attempt = ++m_connectCount;\n    const qint64 elapsedMs = m_forwardConnectElapsed.isValid()\n        ? m_forwardConnectElapsed.elapsed()\n        : 0;\n\n    auto failAttempt = [this, attempt, elapsedMs](const char *stage) {\n        if (qsc::telemetry::enabled() &&\n            (attempt == 1 || attempt % 10 == 0)) {\n            qInfo() << "[Telemetry][Server] forward-probe"\n                    << "attempt=" << attempt\n                    << "elapsedMs=" << elapsedMs\n                    << "stage=" << stage;\n        }\n\n        const bool exhausted = attempt >= MAX_CONNECT_COUNT ||\n                               elapsedMs >= CONNECT_TOTAL_TIMEOUT_MS;\n        if (!exhausted) return;\n\n        qWarning() << "forward tunnel handshake timed out"\n                   << "attempts=" << attempt\n                   << "elapsedMs=" << elapsedMs\n                   << "stage=" << stage;\n        stopConnectTimeoutTimer();\n        stop();\n        if (MAX_RESTART_COUNT > m_restartCount++) {\n            qWarning("restart server auto");\n            start(m_params);\n        } else {\n            m_restartCount = 0;\n            emit serverStarted(false);\n        }\n    };\n\n    // Match upstream scrcpy's forward handshake: connect only the first\n    // (video) socket and wait for the server dummy byte. Opening the control\n    // socket before this probe succeeds creates needless stale connections\n    // while app_process is still starting on slower Android versions.\n    auto videoSocket = std::make_unique<VideoSocket>();\n    videoSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);\n    videoSocket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);\n    videoSocket->connectToHost(QHostAddress::LocalHost, m_params.localPort);\n\n    if (!videoSocket->waitForConnected(CONNECT_PROBE_TIMEOUT_MS)) {\n        failAttempt("video-connect");\n        return;\n    }\n\n    if (videoSocket->bytesAvailable() < 1 &&\n        !videoSocket->waitForReadyRead(CONNECT_PROBE_TIMEOUT_MS)) {\n        failAttempt("dummy-byte");\n        return;\n    }\n\n    const QByteArray dummy = videoSocket->read(1);\n    if (dummy.size() != 1) {\n        failAttempt("dummy-byte");\n        return;\n    }\n\n    // The Android server accepts sockets in video -> control order. Once the\n    // video dummy byte arrives, it is already blocked waiting for control.\n    auto controlSocket = std::make_unique<QTcpSocket>();\n    controlSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);\n    controlSocket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);\n    controlSocket->connectToHost(QHostAddress::LocalHost, m_params.localPort);\n    if (!controlSocket->waitForConnected(1000)) {\n        failAttempt("control-connect");\n        return;\n    }\n\n    QString deviceName;\n    QSize deviceSize;\n    if (!readInfo(videoSocket.get(), deviceName, deviceSize)) {\n        failAttempt("device-info");\n        return;\n    }\n\n    const qint64 connectedMs = m_forwardConnectElapsed.isValid()\n        ? m_forwardConnectElapsed.elapsed()\n        : elapsedMs;\n    stopConnectTimeoutTimer();\n    m_videoSocket = videoSocket.release();\n    m_controlSocket = controlSocket.release();\n\n    // The established sockets remain valid after removing the adb rule.\n    disableTunnelForward();\n    m_tunnelEnabled = false;\n    m_restartCount = 0;\n\n    if (qsc::telemetry::enabled()) {\n        qInfo() << "[Telemetry][Server] forward-connected"\n                << "attempts=" << attempt\n                << "elapsedMs=" << connectedMs;\n    }\n    emit serverStarted(true, deviceName, deviceSize);\n}\n'''

replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/server/server.cpp",
    start,
    replacement,
)

# Failed handshake probes use direct QTcpSocket reads, not the demuxer's
# instrumented read path. Avoid emitting misleading all-zero socket summaries.
replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/server/videosocket.cpp",
    '''void VideoSocket::logTelemetry() const\n{\n    if (!m_telemetryEnabled) return;\n\n    const double totalWaitMs =\n''',
    '''void VideoSocket::logTelemetry() const\n{\n    if (!m_telemetryEnabled) return;\n    if (m_totalBytesRead == 0 && m_readCalls == 0 && m_waitCalls == 0 &&\n        m_interruptedReads == 0 && m_failedReads == 0) {\n        return;\n    }\n\n    const double totalWaitMs =\n''',
)

checks = {
    "QtScrcpy/QtScrcpyCore/src/device/server/server.cpp": [
        "CONNECT_RETRY_INTERVAL_MS 100",
        "forward-connected",
        "video -> control order",
        "std::make_unique<VideoSocket>()",
    ],
    "QtScrcpy/QtScrcpyCore/src/device/server/server.h": [
        "QElapsedTimer m_forwardConnectElapsed",
    ],
    "QtScrcpy/QtScrcpyCore/src/device/server/videosocket.cpp": [
        "misleading all-zero socket summaries" if False else "m_totalBytesRead == 0",
    ],
}
for path, needles in checks.items():
    text = (ROOT / path).read_text(encoding="utf-8")
    for needle in needles:
        if needle not in text:
            raise RuntimeError(f"{path}: missing invariant {needle!r}")

print("Forward handshake optimized")
