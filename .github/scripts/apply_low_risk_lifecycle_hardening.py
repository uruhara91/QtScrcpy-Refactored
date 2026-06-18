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
# Server lifecycle: deterministic cleanup of sockets/timers/processes owned by
# Server. Sockets already transferred to Demuxer are not touched because
# removeVideoSocket() clears m_videoSocket first.
# ---------------------------------------------------------------------------
replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/server/server.h",
    '''    void onConnectTimer();\n\nprivate:\n''',
    '''    void onConnectTimer();\n    void cleanupOwnedSockets();\n\nprivate:\n''',
)

replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/server/server.cpp",
    '''static quint32 bufferRead32be(quint8 *buf)\n{\n    return static_cast<quint32>((buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3]);\n}\n''',
    '''static quint32 bufferRead32be(const quint8 *buf)\n{\n    if (!buf) return 0;\n    return (static_cast<quint32>(buf[0]) << 24) |\n           (static_cast<quint32>(buf[1]) << 16) |\n           (static_cast<quint32>(buf[2]) << 8) |\n           static_cast<quint32>(buf[3]);\n}\n''',
)

replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/server/server.cpp",
    '''Server::~Server() {}\n\nbool Server::pushServer()\n''',
    '''Server::~Server()\n{\n    // Device normally calls stop() before destruction, but keep destruction\n    // deterministic for failed/partial startup paths as well. Do not issue new\n    // adb cleanup commands here: child processes are already being destroyed.\n    stopAcceptTimeoutTimer();\n    stopConnectTimeoutTimer();\n    m_serverSocket.close();\n    cleanupOwnedSockets();\n}\n\nvoid Server::cleanupOwnedSockets()\n{\n    if (m_controlSocket) {\n        m_controlSocket->abort();\n        delete m_controlSocket.data();\n        m_controlSocket = nullptr;\n    }\n    if (m_videoSocket) {\n        m_videoSocket->quit();\n        m_videoSocket->abort();\n        delete m_videoSocket.data();\n        m_videoSocket = nullptr;\n    }\n}\n\nbool Server::pushServer()\n''',
)

replace_once(
    "QtScrcpy/QtScrcpyCore/src/device/server/server.cpp",
    '''void Server::stop()\n{\n    if (m_tunnelForward) {\n        stopConnectTimeoutTimer();\n    } else {\n        stopAcceptTimeoutTimer();\n    }\n\n    if (m_controlSocket) {\n        m_controlSocket->close();\n        m_controlSocket->deleteLater();\n    }\n    // ignore failure\n    m_serverProcess.kill();\n    if (m_tunnelEnabled) {\n        if (m_tunnelForward) {\n            disableTunnelForward();\n        } else {\n            disableTunnelReverse();\n        }\n        m_tunnelForward = false;\n        m_tunnelEnabled = false;\n    }\n    m_serverSocket.close();\n}\n''',
    '''void Server::stop()\n{\n    // Set the state first so process termination callbacks cannot advance a\n    // partially stopped startup state machine.\n    m_serverStartStep = SSS_NULL;\n    stopAcceptTimeoutTimer();\n    stopConnectTimeoutTimer();\n\n    m_serverSocket.close();\n    cleanupOwnedSockets();\n\n    // Both processes may be active during startup fallback/retry.\n    if (m_workProcess.isRuning()) m_workProcess.kill();\n    if (m_serverProcess.isRuning()) m_serverProcess.kill();\n\n    if (m_tunnelEnabled) {\n        if (m_tunnelForward) {\n            disableTunnelForward();\n        } else {\n            disableTunnelReverse();\n        }\n    }\n    m_tunnelForward = false;\n    m_tunnelEnabled = false;\n    m_connectCount = 0;\n}\n''',
)

# ---------------------------------------------------------------------------
# Device manager: reserve a unique localhost port for every session, including
# forward mode, and remove devices/ports deterministically on explicit stop.
# ---------------------------------------------------------------------------
replace_once(
    "QtScrcpy/QtScrcpyCore/src/devicemanage/devicemanage.h",
    '#include <QMap>\n',
    '#include <QMap>\n#include <QSet>\n',
)
replace_once(
    "QtScrcpy/QtScrcpyCore/src/devicemanage/devicemanage.h",
    '''    QMap<QString, QPointer<IDevice>> m_devices;\n    quint16 m_localPortStart = 27183;\n''',
    '''    QMap<QString, QPointer<IDevice>> m_devices;\n    QMap<QString, quint16> m_devicePorts;\n    QSet<quint16> m_allocatedPorts;\n    quint16 m_localPortStart = 27183;\n''',
)

replace_once(
    "QtScrcpy/QtScrcpyCore/src/devicemanage/devicemanage.cpp",
    '''    if (DM_MAX_DEVICES_NUM < m_devices.size()) {\n        qInfo("over the maximum number of connections");\n        return false;\n    }\n    \n    IDevice *device = new Device(params);\n''',
    '''    if (m_devices.size() >= DM_MAX_DEVICES_NUM) {\n        qInfo("over the maximum number of connections");\n        return false;\n    }\n\n    quint16 localPort = params.localPort;\n    if (localPort == 0 || m_allocatedPorts.contains(localPort)) {\n        localPort = getFreePort();\n    }\n    if (localPort == 0) {\n        qCritical("no free local tunnel port");\n        return false;\n    }\n    params.localPort = localPort;\n\n    IDevice *device = new Device(params);\n''',
)

replace_once(
    "QtScrcpy/QtScrcpyCore/src/devicemanage/devicemanage.cpp",
    '''    m_devices[params.serial] = device;\n    return true;\n}\n''',
    '''    m_devices[params.serial] = device;\n    m_devicePorts[params.serial] = localPort;\n    m_allocatedPorts.insert(localPort);\n    return true;\n}\n''',
)

replace_once(
    "QtScrcpy/QtScrcpyCore/src/devicemanage/devicemanage.cpp",
    '''bool DeviceManage::disconnectDevice(const QString &serial)\n{\n    bool ret = false;\n    if (!serial.isEmpty() && m_devices.contains(serial)) {\n        auto it = m_devices.find(serial);\n        if (!it.value().isNull()) {\n            delete it.value().data();\n            ret = true;\n        }\n    }\n    return ret;\n}\n''',
    '''bool DeviceManage::disconnectDevice(const QString &serial)\n{\n    if (serial.isEmpty() || !m_devices.contains(serial)) return false;\n\n    QPointer<IDevice> device = m_devices.take(serial);\n    const quint16 port = m_devicePorts.take(serial);\n    if (port != 0) m_allocatedPorts.remove(port);\n\n    if (!device.isNull()) delete device.data();\n    return true;\n}\n''',
)

replace_once(
    "QtScrcpy/QtScrcpyCore/src/devicemanage/devicemanage.cpp",
    '''void DeviceManage::disconnectAllDevice()\n{\n    QMapIterator<QString, QPointer<IDevice>> i(m_devices);\n    while (i.hasNext()) {\n        i.next();\n        if (!i.value().isNull()) {\n            delete i.value().data();\n        }\n    }\n}\n''',
    '''void DeviceManage::disconnectAllDevice()\n{\n    const auto devices = m_devices;\n    m_devices.clear();\n    m_devicePorts.clear();\n    m_allocatedPorts.clear();\n\n    for (const QPointer<IDevice> &device : devices) {\n        if (!device.isNull()) delete device.data();\n    }\n}\n''',
)

replace_once(
    "QtScrcpy/QtScrcpyCore/src/devicemanage/devicemanage.cpp",
    '''quint16 DeviceManage::getFreePort()\n{\n    quint16 port = m_localPortStart;\n    while (port < m_localPortStart + DM_MAX_DEVICES_NUM) {\n        bool used = false;\n        QMapIterator<QString, QPointer<IDevice>> i(m_devices);\n        while (i.hasNext()) {\n            i.next();\n            auto device = i.value();\n            if (!device.isNull() && device->isReversePort(port)) {\n                used = true;\n                break;\n            }\n        }\n        if (!used) {\n            return port;\n        }\n        port++;\n    }\n    return 0;\n}\n''',
    '''quint16 DeviceManage::getFreePort()\n{\n    const quint32 end = static_cast<quint32>(m_localPortStart) +\n                        static_cast<quint32>(DM_MAX_DEVICES_NUM);\n    for (quint32 candidate = m_localPortStart;\n         candidate < end && candidate <= 65535; ++candidate) {\n        const quint16 port = static_cast<quint16>(candidate);\n        if (!m_allocatedPorts.contains(port)) return port;\n    }\n    return 0;\n}\n''',
)

replace_once(
    "QtScrcpy/QtScrcpyCore/src/devicemanage/devicemanage.cpp",
    '''void DeviceManage::removeDevice(const QString &serial)\n{\n    if (!serial.isEmpty() && m_devices.contains(serial)) {\n        if (!m_devices[serial].isNull()) {\n            m_devices[serial]->deleteLater();\n        }\n        m_devices.remove(serial);\n    }\n}\n''',
    '''void DeviceManage::removeDevice(const QString &serial)\n{\n    if (serial.isEmpty()) return;\n\n    QPointer<IDevice> device = m_devices.take(serial);\n    const quint16 port = m_devicePorts.take(serial);\n    if (port != 0) m_allocatedPorts.remove(port);\n    if (!device.isNull()) device->deleteLater();\n}\n''',
)

checks = {
    "QtScrcpy/QtScrcpyCore/src/device/server/server.cpp": [
        "cleanupOwnedSockets", "m_workProcess.isRuning()", "static_cast<quint32>(buf[0])"],
    "QtScrcpy/QtScrcpyCore/src/devicemanage/devicemanage.cpp": [
        "m_allocatedPorts.insert", "m_devices.take", "no free local tunnel port"],
    "QtScrcpy/QtScrcpyCore/src/devicemanage/devicemanage.h": [
        "QSet<quint16> m_allocatedPorts"],
}
for path, needles in checks.items():
    text = (ROOT / path).read_text(encoding="utf-8")
    for needle in needles:
        if needle not in text:
            raise RuntimeError(f"{path}: missing invariant {needle!r}")

print("Low-risk lifecycle hardening applied")
