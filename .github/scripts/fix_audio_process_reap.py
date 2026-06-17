from pathlib import Path

path = Path("QtScrcpy/audio/audiooutput.cpp")
text = path.read_text(encoding="utf-8-sig")


def replace_once(old: str, new: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"expected one match, found {count}: {old[:100]!r}")
    text = text.replace(old, new, 1)


replace_once(
    "    qint64 handshakeMs = -1;\n\n"
    "    quint64 packetsReceived = 0;\n",
    "    qint64 handshakeMs = -1;\n"
    "    qint64 totalStartupMs = -1;\n\n"
    "    quint64 packetsReceived = 0;\n",
)

replace_once(
    "        if (!process.waitForFinished(timeoutMs)) {\n"
    "            process.kill();\n"
    "            process.waitForFinished(500);\n"
    "            if (reportFailure) {\n"
    '                qWarning() << "[Audio] adb command timed out:" << arguments;\n'
    "            }\n"
    "            return false;\n"
    "        }\n",
    "        if (!process.waitForFinished(timeoutMs)) {\n"
    "            process.kill();\n"
    "            if (!process.waitForFinished(3000)) {\n"
    "                if (telemetryEnabled) {\n"
    '                    qWarning() << "[Telemetry][Audio] waiting-for-adb-reap"\n'
    '                               << "arguments=" << arguments;\n'
    "                }\n"
    "                process.waitForFinished(-1);\n"
    "            }\n"
    "            if (reportFailure) {\n"
    '                qWarning() << "[Audio] adb command timed out:" << arguments;\n'
    "            }\n"
    "            return false;\n"
    "        }\n",
)

replace_once(
    "        scheduledRetryMs = 0;\n"
    "        handshakeMs = -1;\n"
    "        packetsReceived = 0;\n",
    "        scheduledRetryMs = 0;\n"
    "        handshakeMs = -1;\n"
    "        totalStartupMs = -1;\n"
    "        packetsReceived = 0;\n",
)

replace_once(
    '            << "Audio stats - startup: " << handshakeMs\n'
    '            << "ms attempts: " << connectionAttempts\n',
    '            << "Audio stats - startup total: " << totalStartupMs\n'
    '            << "ms handshake: " << handshakeMs\n'
    '            << "ms attempts: " << connectionAttempts\n',
)

replace_once(
    "                inputState = InputState::Packets;\n"
    "                announcedStarted = true;\n"
    "                emit q->started();\n",
    "                inputState = InputState::Packets;\n"
    "                totalStartupMs = startupElapsed.isValid()\n"
    "                    ? startupElapsed.elapsed()\n"
    "                    : -1;\n"
    "                announcedStarted = true;\n"
    "                if (telemetryEnabled) {\n"
    '                    qInfo() << "[Telemetry][Audio] started"\n'
    '                            << "totalMs=" << totalStartupMs\n'
    '                            << "handshakeMs=" << handshakeMs\n'
    '                            << "attempts=" << connectionAttempts;\n'
    "                }\n"
    "                emit q->started();\n",
)

path.write_text(text, encoding="utf-8")
