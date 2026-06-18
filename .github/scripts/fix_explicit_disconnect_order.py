from pathlib import Path

root = Path(__file__).resolve().parents[2]


def replace_once(path: str, old: str, new: str) -> None:
    p = root / path
    text = p.read_text(encoding="utf-8-sig")
    if text.count(old) != 1:
        raise RuntimeError(f"{path}: expected one match, found {text.count(old)}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "QtScrcpy/QtScrcpyCore/src/devicemanage/devicemanage.h",
    '''    QMap<QString, quint16> m_devicePorts;\n    QSet<quint16> m_allocatedPorts;\n''',
    '''    QMap<QString, quint16> m_devicePorts;\n    QSet<quint16> m_allocatedPorts;\n    QSet<QString> m_explicitDisconnects;\n''',
)

replace_once(
    "QtScrcpy/QtScrcpyCore/src/devicemanage/devicemanage.cpp",
    '''bool DeviceManage::disconnectDevice(const QString &serial)\n{\n    if (serial.isEmpty() || !m_devices.contains(serial)) return false;\n\n    QPointer<IDevice> device = m_devices.take(serial);\n    const quint16 port = m_devicePorts.take(serial);\n    if (port != 0) m_allocatedPorts.remove(port);\n\n    if (!device.isNull()) delete device.data();\n    return true;\n}\n''',
    '''bool DeviceManage::disconnectDevice(const QString &serial)\n{\n    if (serial.isEmpty() || !m_devices.contains(serial)) return false;\n\n    QPointer<IDevice> device = m_devices.value(serial);\n    if (device.isNull()) {\n        removeDevice(serial);\n        return false;\n    }\n\n    // Keep the map entry alive while Device emits deviceDisconnected so UI\n    // observers can still retrieve userData and deregister the VideoForm.\n    m_explicitDisconnects.insert(serial);\n    delete device.data();\n    m_explicitDisconnects.remove(serial);\n\n    // A device stopped before successful startup does not emit disconnected.\n    // Release its reservation here if the signal path did not already do so.\n    if (m_devices.contains(serial)) {\n        m_devices.remove(serial);\n        const quint16 port = m_devicePorts.take(serial);\n        if (port != 0) m_allocatedPorts.remove(port);\n    }\n    return true;\n}\n''',
)

replace_once(
    "QtScrcpy/QtScrcpyCore/src/devicemanage/devicemanage.cpp",
    '''void DeviceManage::disconnectAllDevice()\n{\n    const auto devices = m_devices;\n    m_devices.clear();\n    m_devicePorts.clear();\n    m_allocatedPorts.clear();\n\n    for (const QPointer<IDevice> &device : devices) {\n        if (!device.isNull()) delete device.data();\n    }\n}\n''',
    '''void DeviceManage::disconnectAllDevice()\n{\n    const QStringList serials = m_devices.keys();\n    for (const QString &serial : serials) {\n        (void)disconnectDevice(serial);\n    }\n}\n''',
)

replace_once(
    "QtScrcpy/QtScrcpyCore/src/devicemanage/devicemanage.cpp",
    '''void DeviceManage::onDeviceDisconnected(QString serial)\n{\n    emit deviceDisconnected(serial);\n    removeDevice(serial);\n}\n''',
    '''void DeviceManage::onDeviceDisconnected(QString serial)\n{\n    // Emit first while the Device and its userData are still addressable.\n    emit deviceDisconnected(serial);\n\n    QPointer<IDevice> device = m_devices.take(serial);\n    const quint16 port = m_devicePorts.take(serial);\n    if (port != 0) m_allocatedPorts.remove(port);\n\n    // Explicit disconnect is already inside delete Device. Spontaneous server\n    // or stream shutdown still needs deferred object destruction.\n    if (!m_explicitDisconnects.contains(serial) && !device.isNull()) {\n        device->deleteLater();\n    }\n}\n''',
)

for path, needle in {
    "QtScrcpy/QtScrcpyCore/src/devicemanage/devicemanage.h": "m_explicitDisconnects",
    "QtScrcpy/QtScrcpyCore/src/devicemanage/devicemanage.cpp": "Keep the map entry alive",
}.items():
    if needle not in (root / path).read_text(encoding="utf-8"):
        raise RuntimeError(f"missing invariant {needle}")

print("Explicit disconnect ordering fixed")
