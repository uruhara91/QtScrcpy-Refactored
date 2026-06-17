from pathlib import Path

path = Path("QtScrcpy/audio/audiooutput.cpp")
text = path.read_text(encoding="utf-8-sig")


def replace_once(old: str, new: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(
            f"expected one match, found {count}: {old[:100]!r}"
        )
    text = text.replace(old, new, 1)


replace_once(
    '#include "audiooutput.h"\n',
    '#include "audiooutput.h"\n#include "qtscrcpytelemetry.h"\n',
)
replace_once(
    "constexpr int CONNECT_TIMEOUT_MS = 8000;\n"
    "constexpr int CONNECT_RETRY_MS = 100;\n",
    "constexpr int CONNECT_TIMEOUT_MS = 8000;\n"
    "constexpr int CONNECT_RETRY_INITIAL_MS = 100;\n"
    "constexpr int CONNECT_RETRY_MAX_MS = 500;\n",
)
replace_once(
    "[[nodiscard]] int boundedEnvironmentValue(const char *name,\n"
    "                                          int fallback,\n"
    "                                          int minimum,\n"
    "                                          int maximum)\n"
    "{\n"
    "    bool ok = false;\n"
    "    const int value = qEnvironmentVariableIntValue(name, &ok);\n"
    "    return ok ? qBound(minimum, value, maximum) : fallback;\n"
    "}\n\n"
    "[[nodiscard]] bool environmentEnabled(const char *name, bool fallback)\n"
    "{\n"
    "    if (!qEnvironmentVariableIsSet(name)) return fallback;\n"
    "    return qEnvironmentVariableIntValue(name) != 0;\n"
    "}\n\n",
    "",
)
text = text.replace(
    "boundedEnvironmentValue(",
    "qsc::telemetry::boundedEnvironmentInt(",
)
text = text.replace(
    "environmentEnabled(",
    "qsc::telemetry::environmentFlag(",
)
replace_once(
    '        , telemetryEnabled(qsc::telemetry::environmentFlag("QTSCRCPY_TELEMETRY", false))\n',
    "        , telemetryEnabled(qsc::telemetry::enabled())\n",
)
replace_once(
    "    QElapsedTimer connectElapsed;\n"
    "    QElapsedTimer playbackElapsed;\n",
    "    QElapsedTimer connectElapsed;\n"
    "    QElapsedTimer startupElapsed;\n"
    "    QElapsedTimer playbackElapsed;\n",
)
replace_once(
    "    int connectionAttempts = 0;\n\n"
    "    quint64 packetsReceived = 0;\n",
    "    int connectionAttempts = 0;\n"
    "    int scheduledRetryMs = 0;\n"
    "    qint64 handshakeMs = -1;\n\n"
    "    quint64 packetsReceived = 0;\n",
)
replace_once(
    "        playbackElapsed.invalidate();\n"
    "        connectionAttempts = 0;\n",
    "        playbackElapsed.invalidate();\n"
    "        connectElapsed.invalidate();\n"
    "        startupElapsed.invalidate();\n"
    "        connectionAttempts = 0;\n"
    "        scheduledRetryMs = 0;\n"
    "        handshakeMs = -1;\n",
)
replace_once(
    '            << "Audio stats - packets: " << packetsReceived\n',
    '            << "Audio stats - startup: " << handshakeMs\n'
    '            << "ms attempts: " << connectionAttempts\n'
    '            << " packets: " << packetsReceived\n',
)
replace_once(
    "        resetRuntimeState();\n\n"
    "        prepareAndroid11Audio();\n",
    "        resetRuntimeState();\n"
    "        startupElapsed.start();\n\n"
    "        prepareAndroid11Audio();\n",
)
replace_once(
    "        retryTimer = new QTimer(q);\n"
    "        retryTimer->setInterval(CONNECT_RETRY_MS);\n"
    "        QObject::connect(retryTimer, &QTimer::timeout, q, [this]() {\n"
    "            attemptConnect();\n"
    "        });\n"
    "        retryTimer->start();\n"
    "        attemptConnect();\n",
    "        retryTimer = new QTimer(q);\n"
    "        retryTimer->setSingleShot(true);\n"
    "        retryTimer->setTimerType(Qt::PreciseTimer);\n"
    "        QObject::connect(retryTimer, &QTimer::timeout, q, [this]() {\n"
    "            attemptConnect();\n"
    "        });\n"
    "        attemptConnect();\n",
)
replace_once(
    '            qInfo() << "[Audio] ADB tunnel connected, attempt:"\n'
    "                    << connectionAttempts;\n",
    "            if (telemetryEnabled) {\n"
    '                qInfo() << "[Telemetry][Audio] tunnel-connected"\n'
    '                        << "attempt=" << connectionAttempts\n'
    '                        << "elapsedMs="\n'
    "                        << (connectElapsed.isValid()\n"
    "                                ? connectElapsed.elapsed()\n"
    "                                : -1);\n"
    "            }\n",
)
replace_once(
    '                qInfo() << "[Audio] Android socket not ready yet; retrying";\n'
    "                QTimer::singleShot(CONNECT_RETRY_MS, q, [this]() {\n"
    "                    attemptConnect();\n"
    "                });\n"
    "                return;\n",
    '                scheduleConnectRetry("android-socket-not-ready");\n'
    "                return;\n",
)
replace_once(
    "    void attemptConnect()\n"
    "    {\n",
    "    [[nodiscard]] int nextRetryDelayMs() const noexcept\n"
    "    {\n"
    "        int delay = CONNECT_RETRY_INITIAL_MS;\n"
    "        const int growthSteps = std::min(connectionAttempts, 5);\n"
    "        for (int i = 0; i < growthSteps; ++i) {\n"
    "            delay = std::min(CONNECT_RETRY_MAX_MS,\n"
    "                             delay + std::max(delay / 2, 1));\n"
    "        }\n"
    "        return delay;\n"
    "    }\n\n"
    "    void scheduleConnectRetry(const char *reason)\n"
    "    {\n"
    "        if (!active || stopping || protocolStarted || !retryTimer) return;\n"
    "        if (retryTimer->isActive()) return;\n\n"
    "        if (connectElapsed.isValid() &&\n"
    "            connectElapsed.elapsed() >= CONNECT_TIMEOUT_MS) {\n"
    "            fail(QStringLiteral(\n"
    '                "Audio: timed out waiting for the Android audio socket"));\n'
    "            return;\n"
    "        }\n\n"
    "        scheduledRetryMs = nextRetryDelayMs();\n"
    "        if (telemetryEnabled) {\n"
    '            qInfo() << "[Telemetry][Audio] retry-scheduled"\n'
    '                    << "reason=" << reason\n'
    '                    << "delayMs=" << scheduledRetryMs\n'
    '                    << "attempts=" << connectionAttempts;\n'
    "        }\n"
    "        retryTimer->start(scheduledRetryMs);\n"
    "    }\n\n"
    "    void attemptConnect()\n"
    "    {\n",
)
replace_once(
    "                if (retryTimer) retryTimer->stop();\n"
    '                qInfo() << "[Audio] Android audio socket handshake completed";\n',
    "                if (retryTimer) retryTimer->stop();\n"
    "                handshakeMs = connectElapsed.isValid()\n"
    "                    ? connectElapsed.elapsed()\n"
    "                    : -1;\n"
    '                qInfo() << "[Audio] Android audio socket handshake completed";\n'
    "                if (telemetryEnabled) {\n"
    '                    qInfo() << "[Telemetry][Audio] handshake"\n'
    '                            << "elapsedMs=" << handshakeMs\n'
    '                            << "attempts=" << connectionAttempts;\n'
    "                }\n",
)
replace_once(
    "        if (pumpTimer) {\n"
    "            pumpTimer->stop();\n"
    "            pumpTimer->deleteLater();\n"
    "            pumpTimer = nullptr;\n"
    "        }\n"
    "        if (audioSink) {\n"
    "            audioSink->stop();\n"
    "            audioSink->deleteLater();\n"
    "            audioSink = nullptr;\n"
    "        }\n",
    "        if (pumpTimer) {\n"
    "            pumpTimer->stop();\n"
    "            delete pumpTimer;\n"
    "            pumpTimer = nullptr;\n"
    "        }\n"
    "        if (audioSink) {\n"
    "            audioSink->stop();\n"
    "            delete audioSink;\n"
    "            audioSink = nullptr;\n"
    "        }\n",
)
replace_once(
    "        if (stopping) return;\n"
    "        const bool wasActive = active || announcedStarted;\n",
    "        if (stopping) return;\n"
    "        if (!active && !announcedStarted && !serverProcess &&\n"
    "            !socket && !retryTimer && !pumpTimer &&\n"
    "            !audioSink && !codecContext) {\n"
    "            return;\n"
    "        }\n"
    "        const bool wasActive = active || announcedStarted;\n",
)
replace_once(
    "        if (retryTimer) {\n"
    "            retryTimer->stop();\n"
    "            retryTimer->deleteLater();\n"
    "            retryTimer = nullptr;\n"
    "        }\n"
    "        if (socket) {\n"
    "            QObject::disconnect(socket, nullptr, q, nullptr);\n"
    "            socket->abort();\n"
    "            socket->deleteLater();\n"
    "            socket = nullptr;\n"
    "        }\n",
    "        if (retryTimer) {\n"
    "            retryTimer->stop();\n"
    "            delete retryTimer;\n"
    "            retryTimer = nullptr;\n"
    "        }\n"
    "        if (socket) {\n"
    "            QObject::disconnect(socket, nullptr, q, nullptr);\n"
    "            socket->abort();\n"
    "            delete socket;\n"
    "            socket = nullptr;\n"
    "        }\n",
)
replace_once(
    "        if (serverProcess) {\n"
    "            QObject::disconnect(serverProcess, nullptr, q, nullptr);\n"
    "            if (serverProcess->state() != QProcess::NotRunning) {\n"
    "                serverProcess->terminate();\n"
    "                if (!serverProcess->waitForFinished(500)) {\n"
    "                    serverProcess->kill();\n"
    "                    serverProcess->waitForFinished(500);\n"
    "                }\n"
    "            }\n"
    "            serverProcess->deleteLater();\n"
    "            serverProcess = nullptr;\n"
    "        }\n",
    "        if (serverProcess) {\n"
    "            QProcess *process = serverProcess;\n"
    "            serverProcess = nullptr;\n"
    "            QObject::disconnect(process, nullptr, q, nullptr);\n"
    "            if (process->state() != QProcess::NotRunning) {\n"
    "                process->terminate();\n"
    "                if (!process->waitForFinished(750)) {\n"
    "                    process->kill();\n"
    "                    if (!process->waitForFinished(3000)) {\n"
    '                        qWarning() << "[Audio] Waiting for adb process termination";\n'
    "                        process->waitForFinished(-1);\n"
    "                    }\n"
    "                }\n"
    "            }\n"
    "            delete process;\n"
    "        }\n",
)
replace_once(
    "        resetRuntimeState();\n"
    "        announcedStarted = false;\n"
    "        stopping = false;\n",
    "        resetRuntimeState();\n"
    "        serial.clear();\n"
    "        localServerPath.clear();\n"
    "        remoteServerPath.clear();\n"
    "        serverVersion.clear();\n"
    "        socketName.clear();\n"
    "        localPort = 0;\n"
    "        announcedStarted = false;\n"
    "        stopping = false;\n",
)

path.write_text(text, encoding="utf-8")
