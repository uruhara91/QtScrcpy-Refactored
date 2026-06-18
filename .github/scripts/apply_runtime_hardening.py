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


# ---------------------------------------------------------------------------
# Wayland: global QGuiApplication::mouseButtons() polling may report no buttons
# while a button is physically held. Keep event/focus/duplicate-down recovery,
# but only run the global watchdog on platforms where it is reliable.
# ---------------------------------------------------------------------------
replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.h",
    "    void reconcileMouseButtons(Qt::MouseButtons buttons);\n",
    "    void reconcileMouseButtons(Qt::MouseButtons buttons, const char *reason);\n",
)
replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.h",
    "    QSet<int> m_activeMouseButtons;\n    QTimer m_mouseButtonWatchdog;\n",
    "    QSet<int> m_activeMouseButtons;\n    QTimer m_mouseButtonWatchdog;\n    bool m_globalMouseButtonsReliable = true;\n",
)
replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp",
    '''    m_mouseButtonWatchdog.setInterval(100);\n    m_mouseButtonWatchdog.setTimerType(Qt::CoarseTimer);\n    connect(&m_mouseButtonWatchdog, &QTimer::timeout, this, [this]() {\n        reconcileMouseButtons(QGuiApplication::mouseButtons());\n    });\n''',
    '''    m_globalMouseButtonsReliable =\n        !QGuiApplication::platformName().startsWith(QLatin1String("wayland"));\n    m_mouseButtonWatchdog.setInterval(100);\n    m_mouseButtonWatchdog.setTimerType(Qt::CoarseTimer);\n    connect(&m_mouseButtonWatchdog, &QTimer::timeout, this, [this]() {\n        if (!m_globalMouseButtonsReliable) return;\n        reconcileMouseButtons(QGuiApplication::mouseButtons(),\n                              "global-button-state");\n    });\n''',
)
replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp",
    "void InputConvertGame::reconcileMouseButtons(Qt::MouseButtons buttons)\n",
    "void InputConvertGame::reconcileMouseButtons(Qt::MouseButtons buttons,\n                                             const char *reason)\n",
)
replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp",
    '''            qInfo() << "[Telemetry][Input] forced-release"\n                    << "reason=host-button-state"\n                    << "button=" << key\n''',
    '''            qInfo() << "[Telemetry][Input] forced-release"\n                    << "reason=" << (reason ? reason : "button-state")\n                    << "button=" << key\n''',
)
replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp",
    '''void InputConvertGame::updateMouseButtonWatchdog()\n{\n    if (m_activeMouseButtons.isEmpty()) {\n''',
    '''void InputConvertGame::updateMouseButtonWatchdog()\n{\n    if (!m_globalMouseButtonsReliable || m_activeMouseButtons.isEmpty()) {\n''',
)
replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp",
    "    reconcileMouseButtons(from->buttons());\n",
    "    reconcileMouseButtons(from->buttons(), \"event-button-state\");\n",
)

# ---------------------------------------------------------------------------
# Configured ADB path must take precedence over the deployment/platform fallback.
# ---------------------------------------------------------------------------
replace_once(
    "QtScrcpy/QtScrcpyCore/src/adb/adbprocessimpl.cpp",
    '''        QStringList potentialPaths;\n        potentialPaths << QString::fromLocal8Bit(qgetenv("QTSCRCPY_ADB_PATH"))\n                       << g_adbPath;\n''',
    '''        QStringList potentialPaths;\n        // Explicit config wins; the environment value is the deployment\n        // fallback installed by main.cpp.\n        potentialPaths << g_adbPath\n                       << QString::fromLocal8Bit(qgetenv("QTSCRCPY_ADB_PATH"));\n''',
)

# ---------------------------------------------------------------------------
# Build/runtime identity. App Version was previously logged before it was ever
# assigned, so every diagnostic showed an empty version.
# ---------------------------------------------------------------------------
replace_once(
    "QtScrcpy/CMakeLists.txt",
    '''add_executable(${PROJECT_NAME} ${QC_RUNTIME_TYPE} ${QC_PROJECT_SOURCES})\n\n# Log compile definitions\n''',
    '''add_executable(${PROJECT_NAME} ${QC_RUNTIME_TYPE} ${QC_PROJECT_SOURCES})\n\ntarget_compile_definitions(${PROJECT_NAME} PRIVATE\n    QTSCRCPY_VERSION="${PROJECT_VERSION}"\n    QTSCRCPY_BUILD_TYPE="${CMAKE_BUILD_TYPE}"\n)\n\n# Log compile definitions\n''',
)
replace_once(
    "QtScrcpy/main.cpp",
    "#include <QStandardPaths>\n#include <QDir>\n",
    "#include <QStandardPaths>\n#include <QSysInfo>\n#include <QDir>\n",
)
replace_once(
    "QtScrcpy/main.cpp",
    '''#include "adbprocess.h"\n#include <mimalloc-new-delete.h>\n''',
    '''#include "adbprocess.h"\n#include "qtscrcpytelemetry.h"\n#include <mimalloc-new-delete.h>\n\n#ifndef QTSCRCPY_VERSION\n#define QTSCRCPY_VERSION "0.0.0"\n#endif\n#ifndef QTSCRCPY_BUILD_TYPE\n#define QTSCRCPY_BUILD_TYPE "unknown"\n#endif\n''',
)
replace_once(
    "QtScrcpy/main.cpp",
    '''int main(int argc, char *argv[])\n{\n    // QCoreApplication::applicationDirPath() is invalid before QApplication\n''',
    '''int main(int argc, char *argv[])\n{\n    QCoreApplication::setApplicationName(QStringLiteral("QtScrcpy"));\n    QCoreApplication::setApplicationVersion(QStringLiteral(QTSCRCPY_VERSION));\n\n    // QCoreApplication::applicationDirPath() is invalid before QApplication\n''',
)
replace_once(
    "QtScrcpy/main.cpp",
    '''    qDebug() << "App Name:" << a.applicationName();\n    qDebug() << "App Version:" << a.applicationVersion();\n\n    QStringList versionList = QCoreApplication::applicationVersion().split(".");\n    if (versionList.size() >= 3) {\n        QString version = versionList[0] + "." + versionList[1] + "." + versionList[2];\n        a.setApplicationVersion(version);\n    }\n''',
    '''    qDebug() << "App Name:" << a.applicationName();\n    qDebug() << "App Version:" << a.applicationVersion();\n    if (qsc::telemetry::enabled()) {\n        qInfo() << "[Telemetry][Runtime]"\n                << "version=" << a.applicationVersion()\n                << "buildType=" << QTSCRCPY_BUILD_TYPE\n                << "qt=" << qVersion()\n                << "platform=" << QGuiApplication::platformName()\n                << "arch=" << QSysInfo::currentCpuArchitecture();\n    }\n''',
)
replace_once(
    "QtScrcpy/main.cpp",
    '''QtMsgType covertLogLevel(const QString &logLevel)\n{\n    if ("debug" == logLevel) return QtDebugMsg;\n''',
    '''QtMsgType covertLogLevel(const QString &logLevel)\n{\n    if ("verbose" == logLevel || "debug" == logLevel) return QtDebugMsg;\n''',
)

# ---------------------------------------------------------------------------
# Config hardening and backward-compatible userdata key migration.
# ---------------------------------------------------------------------------
replace_once(
    "QtScrcpy/util/config.cpp",
    '#include <QSettings>\n#include <QDebug>\n',
    '#include <QSettings>\n#include <QDebug>\n#include <QSet>\n',
)
replace_once(
    "QtScrcpy/util/config.cpp",
    '''#define COMMON_LOCK_ORIENTATION_INDEX_KEY "LockDirectionIndex"\n#define COMMON_LOCK_ORIENTATION_INDEX_DEF 0\n''',
    '''#define COMMON_LOCK_ORIENTATION_INDEX_KEY "LockOrientationIndex"\n#define COMMON_LOCK_ORIENTATION_INDEX_LEGACY_KEY "LockDirectionIndex"\n#define COMMON_LOCK_ORIENTATION_INDEX_DEF 0\n''',
)
replace_once(
    "QtScrcpy/util/config.cpp",
    '''#define COMMON_RECORD_BACKGROUD_KEY "RecordBackGround"\n#define COMMON_RECORD_BACKGROUD_DEF false\n''',
    '''#define COMMON_RECORD_BACKGROUND_KEY "RecordBackground"\n#define COMMON_RECORD_BACKGROUND_LEGACY_KEY "RecordBackGround"\n#define COMMON_RECORD_BACKGROUND_DEF false\n''',
)
replace_once(
    "QtScrcpy/util/config.cpp",
    '''#define COMMON_SHOW_TOOLBAR_KEY "showToolbar"\n#define COMMON_SHOW_TOOLBAR_DEF true\n''',
    '''#define COMMON_SHOW_TOOLBAR_KEY "ShowToolbar"\n#define COMMON_SHOW_TOOLBAR_LEGACY_KEY "showToolbar"\n#define COMMON_SHOW_TOOLBAR_DEF true\n''',
)
replace_once(
    "QtScrcpy/util/config.cpp",
    '''QString Config::s_configPath = "";\n\nConfig::Config(QObject *parent) : QObject(parent)\n''',
    '''QString Config::s_configPath = "";\n\nnamespace {\nQVariant readMigratedValue(QSettings *settings,\n                           const char *key,\n                           const char *legacyKey,\n                           const QVariant &fallback)\n{\n    if (!settings || !key) return fallback;\n    if (settings->contains(QLatin1String(key))) {\n        return settings->value(QLatin1String(key), fallback);\n    }\n    if (legacyKey && settings->contains(QLatin1String(legacyKey))) {\n        const QVariant value = settings->value(QLatin1String(legacyKey), fallback);\n        settings->setValue(QLatin1String(key), value);\n        settings->remove(QLatin1String(legacyKey));\n        return value;\n    }\n    return fallback;\n}\n}\n\nConfig::Config(QObject *parent) : QObject(parent)\n''',
)
replace_once(
    "QtScrcpy/util/config.cpp",
    "    m_userData->setValue(COMMON_RECORD_BACKGROUD_KEY, config.recordBackground);\n",
    "    m_userData->setValue(COMMON_RECORD_BACKGROUND_KEY, config.recordBackground);\n",
)
replace_once(
    "QtScrcpy/util/config.cpp",
    '''    config.lockOrientationIndex = m_userData->value(COMMON_LOCK_ORIENTATION_INDEX_KEY, COMMON_LOCK_ORIENTATION_INDEX_DEF).toInt();\n''',
    '''    config.lockOrientationIndex = readMigratedValue(\n        m_userData, COMMON_LOCK_ORIENTATION_INDEX_KEY,\n        COMMON_LOCK_ORIENTATION_INDEX_LEGACY_KEY,\n        COMMON_LOCK_ORIENTATION_INDEX_DEF).toInt();\n''',
)
replace_once(
    "QtScrcpy/util/config.cpp",
    '''    config.recordBackground = m_userData->value(COMMON_RECORD_BACKGROUD_KEY, COMMON_RECORD_BACKGROUD_DEF).toBool();\n''',
    '''    config.recordBackground = readMigratedValue(\n        m_userData, COMMON_RECORD_BACKGROUND_KEY,\n        COMMON_RECORD_BACKGROUND_LEGACY_KEY,\n        COMMON_RECORD_BACKGROUND_DEF).toBool();\n''',
)
replace_once(
    "QtScrcpy/util/config.cpp",
    '''    config.showToolbar =m_userData->value(COMMON_SHOW_TOOLBAR_KEY,COMMON_SHOW_TOOLBAR_DEF).toBool();\n    m_userData->endGroup();\n''',
    '''    config.showToolbar = readMigratedValue(\n        m_userData, COMMON_SHOW_TOOLBAR_KEY,\n        COMMON_SHOW_TOOLBAR_LEGACY_KEY,\n        COMMON_SHOW_TOOLBAR_DEF).toBool();\n    m_userData->endGroup();\n    m_userData->sync();\n''',
)
replace_once(
    "QtScrcpy/util/config.cpp",
    '''int Config::getMaxFps()\n{\n    int fps = 0;\n    m_settings->beginGroup(GROUP_COMMON);\n    fps = m_settings->value(COMMON_MAX_FPS_KEY, COMMON_MAX_FPS_DEF).toInt();\n    m_settings->endGroup();\n    return fps;\n}\n''',
    '''int Config::getMaxFps()\n{\n    m_settings->beginGroup(GROUP_COMMON);\n    const int fps = m_settings->value(\n        COMMON_MAX_FPS_KEY, COMMON_MAX_FPS_DEF).toInt();\n    m_settings->endGroup();\n    return qBound(0, fps, 240);\n}\n''',
)
replace_once(
    "QtScrcpy/util/config.cpp",
    '''int Config::getSkin()\n{\n    // force disable skin\n    return 0;\n    int skin = 1;\n    m_settings->beginGroup(GROUP_COMMON);\n    skin = m_settings->value(COMMON_SKIN_KEY, COMMON_SKIN_DEF).toInt();\n    m_settings->endGroup();\n    return skin;\n}\n''',
    '''int Config::getSkin()\n{\n    // Skin is intentionally disabled by the current UI implementation.\n    return 0;\n}\n''',
)
replace_once(
    "QtScrcpy/util/config.cpp",
    '''    pushFile = m_settings->value(COMMON_PUSHFILE_KEY, COMMON_PUSHFILE_DEF).toString();\n    m_settings->endGroup();\n    return pushFile;\n''',
    '''    pushFile = m_settings->value(\n        COMMON_PUSHFILE_KEY, COMMON_PUSHFILE_DEF).toString().trimmed();\n    m_settings->endGroup();\n    if (pushFile.isEmpty()) pushFile = QStringLiteral(COMMON_PUSHFILE_DEF);\n    if (!pushFile.endsWith(QLatin1Char('/'))) pushFile += QLatin1Char('/');\n    return pushFile;\n''',
)
replace_once(
    "QtScrcpy/util/config.cpp",
    '''    serverPath = m_settings->value(COMMON_SERVER_PATH_KEY, COMMON_SERVER_PATH_DEF).toString();\n    m_settings->endGroup();\n    return serverPath;\n''',
    '''    serverPath = m_settings->value(\n        COMMON_SERVER_PATH_KEY, COMMON_SERVER_PATH_DEF).toString().trimmed();\n    m_settings->endGroup();\n    return serverPath.isEmpty() ? QStringLiteral(COMMON_SERVER_PATH_DEF)\n                                : serverPath;\n''',
)
replace_once(
    "QtScrcpy/util/config.cpp",
    '''    adbPath = m_settings->value(COMMON_ADB_PATH_KEY, COMMON_ADB_PATH_DEF).toString();\n''',
    '''    adbPath = m_settings->value(\n        COMMON_ADB_PATH_KEY, COMMON_ADB_PATH_DEF).toString().trimmed();\n''',
)
replace_once(
    "QtScrcpy/util/config.cpp",
    '''    logLevel = m_settings->value(COMMON_LOG_LEVEL_KEY, COMMON_LOG_LEVEL_DEF).toString();\n    m_settings->endGroup();\n    return logLevel;\n''',
    '''    logLevel = m_settings->value(\n        COMMON_LOG_LEVEL_KEY, COMMON_LOG_LEVEL_DEF).toString().trimmed().toLower();\n    m_settings->endGroup();\n    static const QSet<QString> levels{\n        QStringLiteral("verbose"), QStringLiteral("debug"),\n        QStringLiteral("info"), QStringLiteral("warn"),\n        QStringLiteral("error")};\n    return levels.contains(logLevel) ? logLevel\n                                     : QStringLiteral(COMMON_LOG_LEVEL_DEF);\n''',
)
replace_once(
    "QtScrcpy/util/config.cpp",
    '''    codecOptions = m_settings->value(COMMON_CODEC_OPTIONS_KEY, COMMON_CODEC_OPTIONS_DEF).toString();\n''',
    '''    codecOptions = m_settings->value(\n        COMMON_CODEC_OPTIONS_KEY, COMMON_CODEC_OPTIONS_DEF).toString().trimmed();\n''',
)
replace_once(
    "QtScrcpy/util/config.cpp",
    '''    codecName = m_settings->value(COMMON_CODEC_NAME_KEY, COMMON_CODEC_NAME_DEF).toString();\n''',
    '''    codecName = m_settings->value(\n        COMMON_CODEC_NAME_KEY, COMMON_CODEC_NAME_DEF).toString().trimmed();\n''',
)

# ---------------------------------------------------------------------------
# Make effective server parameters visible in telemetry without exposing them
# during normal runs.
# ---------------------------------------------------------------------------
replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/server/server.cpp",
    '''        qInfo() << "[Telemetry][Server] launch"\n                << "uidMode=" << (useRoot ? "root" : "shell")\n                << "logLevel=" << serverLogLevel\n                << "thread=" << qsc::telemetry::threadId();\n''',
    '''        qInfo() << "[Telemetry][Server] launch"\n                << "uidMode=" << (useRoot ? "root" : "shell")\n                << "version=" << m_params.serverVersion\n                << "maxSize=" << m_params.maxSize\n                << "maxFps=" << m_params.maxFps\n                << "bitRate=" << m_params.bitRate\n                << "codecOptions=" << m_params.codecOptions\n                << "encoder=" << (m_params.codecName.isEmpty()\n                                       ? QStringLiteral("auto")\n                                       : m_params.codecName)\n                << "tunnel=" << (m_tunnelForward ? "forward" : "reverse")\n                << "logLevel=" << serverLogLevel\n                << "thread=" << qsc::telemetry::threadId();\n''',
)

# Validate key invariants before committing.
checks = {
    "QtScrcpy/QtScrcpyCore/src/device/controller/inputconvert/inputconvertgame.cpp": [
        "m_globalMouseButtonsReliable", "global-button-state", "event-button-state"],
    "QtScrcpy/QtScrcpyCore/src/adb/adbprocessimpl.cpp": [
        "potentialPaths << g_adbPath"],
    "QtScrcpy/main.cpp": [
        "QTSCRCPY_VERSION", "[Telemetry][Runtime]"],
    "QtScrcpy/util/config.cpp": [
        "readMigratedValue", "RecordBackground", "ShowToolbar"],
    "QtScrcpy/QtScrcpyCore/src/device/server/server.cpp": [
        '"maxFps=" << m_params.maxFps'],
}
for path, needles in checks.items():
    text = (ROOT / path).read_text(encoding="utf-8")
    for needle in needles:
        if needle not in text:
            raise RuntimeError(f"{path}: missing invariant {needle!r}")

print("Runtime hardening applied successfully")
