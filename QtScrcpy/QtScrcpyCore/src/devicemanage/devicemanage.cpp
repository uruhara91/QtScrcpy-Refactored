#include <QDebug>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include "devicemanage.h"
#include "device.h"
#include "demuxer.h"

namespace qsc {

#define DM_MAX_DEVICES_NUM 1000

IDeviceManage& IDeviceManage::getInstance() {
    static DeviceManage dm;
    return dm;
}

DeviceManage::DeviceManage() {
    if (!Demuxer::init()) {
        qCritical("Demuxer init failed!");
    }
}

DeviceManage::~DeviceManage() {
    Demuxer::deInit();
}

QPointer<IDevice> DeviceManage::getDevice(const QString &serial)
{
    if (!m_devices.contains(serial)) {
        return QPointer<IDevice>();
    }
    return m_devices[serial];
}

bool DeviceManage::connectDevice(qsc::DeviceParams params)
{
    if (params.serial.trimmed().isEmpty()) {
        return false;
    }
    if (m_devices.contains(params.serial)) {
        return false;
    }
    if (m_devices.size() >= DM_MAX_DEVICES_NUM) {
        qInfo("over the maximum number of connections");
        return false;
    }

    quint16 localPort = params.localPort;
    if (localPort == 0 || m_allocatedPorts.contains(localPort)) {
        localPort = getFreePort();
    }
    if (localPort == 0) {
        qCritical("no free local tunnel port");
        return false;
    }
    params.localPort = localPort;

    IDevice *device = new Device(params);
    connect(device, &Device::deviceConnected, this, &DeviceManage::onDeviceConnected);
    connect(device, &Device::deviceDisconnected, this, &DeviceManage::onDeviceDisconnected);
    
    if (!device->connectDevice()) {
        delete device;
        return false;
    }
    m_devices[params.serial] = device;
    m_devicePorts[params.serial] = localPort;
    m_allocatedPorts.insert(localPort);
    return true;
}

bool DeviceManage::disconnectDevice(const QString &serial)
{
    if (serial.isEmpty() || !m_devices.contains(serial)) return false;

    QPointer<IDevice> device = m_devices.take(serial);
    const quint16 port = m_devicePorts.take(serial);
    if (port != 0) m_allocatedPorts.remove(port);

    if (!device.isNull()) delete device.data();
    return true;
}

void DeviceManage::disconnectAllDevice()
{
    const auto devices = m_devices;
    m_devices.clear();
    m_devicePorts.clear();
    m_allocatedPorts.clear();

    for (const QPointer<IDevice> &device : devices) {
        if (!device.isNull()) delete device.data();
    }
}

void DeviceManage::onDeviceConnected(bool success, const QString &serial, const QString &deviceName, const QSize &size)
{
    emit deviceConnected(success, serial, deviceName, size);
    if (!success) {
        removeDevice(serial);
    }
}

void DeviceManage::onDeviceDisconnected(QString serial)
{
    emit deviceDisconnected(serial);
    removeDevice(serial);
}

quint16 DeviceManage::getFreePort()
{
    const quint32 end = static_cast<quint32>(m_localPortStart) +
                        static_cast<quint32>(DM_MAX_DEVICES_NUM);
    for (quint32 candidate = m_localPortStart;
         candidate < end && candidate <= 65535; ++candidate) {
        const quint16 port = static_cast<quint16>(candidate);
        if (!m_allocatedPorts.contains(port)) return port;
    }
    return 0;
}

void DeviceManage::removeDevice(const QString &serial)
{
    if (serial.isEmpty()) return;

    QPointer<IDevice> device = m_devices.take(serial);
    const quint16 port = m_devicePorts.take(serial);
    if (port != 0) m_allocatedPorts.remove(port);
    if (!device.isNull()) device->deleteLater();
}

}