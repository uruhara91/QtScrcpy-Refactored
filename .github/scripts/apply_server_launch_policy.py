from pathlib import Path

path = Path("QtScrcpy/QtScrcpyCore/src/device/server/server.cpp")
text = path.read_text(encoding="utf-8-sig")


def replace_once(old: str, new: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"expected one match, found {count}: {old[:120]!r}")
    text = text.replace(old, new, 1)


replace_once(
    '#include "server.h"\n',
    '#include "server.h"\n#include "qtscrcpytelemetry.h"\n',
)
replace_once(
    "static quint32 bufferRead32be(quint8 *buf)\n"
    "{\n"
    "    return static_cast<quint32>((buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3]);\n"
    "}\n",
    "static quint32 bufferRead32be(quint8 *buf)\n"
    "{\n"
    "    return static_cast<quint32>((buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3]);\n"
    "}\n\n"
    "static QString shellQuote(QString value)\n"
    "{\n"
    "    value.replace(QLatin1Char('\\''), QStringLiteral(\"'\\\"'\\\"'\"));\n"
    "    return QLatin1Char('\\'') + value + QLatin1Char('\\'');\n"
    "}\n",
)
replace_once(
    "    if (!m_params.logLevel.isEmpty()) {\n"
    "        args << QString(\"log_level=%1\").arg(m_params.logLevel);\n"
    "    }\n",
    "    const QString serverLogLevel = qsc::telemetry::enabled()\n"
    "        ? QStringLiteral(\"debug\")\n"
    "        : m_params.logLevel;\n"
    "    if (!serverLogLevel.isEmpty()) {\n"
    "        args << QString(\"log_level=%1\").arg(serverLogLevel);\n"
    "    }\n",
)
replace_once(
    "    QString hybridCmd = QString(\"su -c '%1' || %1\").arg(cmdObj);\n\n"
    "    QStringList finalArgs;\n"
    "    finalArgs << \"shell\";\n"
    "    finalArgs << hybridCmd;\n\n"
    "    m_serverProcess.execute(m_params.serial, finalArgs);\n",
    "    const bool useRoot = qsc::telemetry::environmentFlag(\n"
    "        \"QTSCRCPY_SERVER_ROOT\", false);\n"
    "    const QString serverCommand = useRoot\n"
    "        ? QStringLiteral(\"su -c %1\").arg(shellQuote(cmdObj))\n"
    "        : cmdObj;\n\n"
    "    if (qsc::telemetry::enabled()) {\n"
    "        qInfo() << \"[Telemetry][Server] launch\"\n"
    "                << \"uidMode=\" << (useRoot ? \"root\" : \"shell\")\n"
    "                << \"logLevel=\" << serverLogLevel\n"
    "                << \"thread=\" << qsc::telemetry::threadId();\n"
    "    }\n\n"
    "    QStringList finalArgs;\n"
    "    finalArgs << QStringLiteral(\"shell\") << serverCommand;\n\n"
    "    m_serverProcess.execute(m_params.serial, finalArgs);\n",
)

path.write_text(text, encoding="utf-8")
