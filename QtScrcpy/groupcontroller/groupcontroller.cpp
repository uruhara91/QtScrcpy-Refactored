#include <QPointer>
#include <span>

#include "groupcontroller.h"
#include "videoform.h"

GroupController::GroupController(QObject *parent) : QObject(parent)
{

}

bool GroupController::isHost(const QString &serial)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
    if (!device) {
        return true;
    }

    auto data = device->getUserData();
    if (!data) {
        return true;
    }

    return static_cast<VideoForm*>(data)->isHost();
}

QSize GroupController::getFrameSize(const QString &serial)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
    if (!device) {
        return QSize();
    }

    auto data = device->getUserData();
    if (!data) {
        return QSize();
    }

    return static_cast<VideoForm*>(data)->frameSize();
}

GroupController &GroupController::instance()
{
    static GroupController gc;
    return gc;
}

template <typename Func>
void GroupController::forEachControlledDevice(Func &&func)
{
    for (const auto &serial : m_devices) {
        if (isHost(serial)) {
            continue;
        }
        auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
        if (!device) {
            continue;
        }
        func(serial, device);
    }
}

void GroupController::updateDeviceState(const QString &serial)
{
    if (!m_devices.contains(serial)) {
        return;
    }

    auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
    if (!device) {
        return;
    }

    if (isHost(serial)) {
        device->registerDeviceObserver(this);
    } else {
        device->deRegisterDeviceObserver(this);
    }
}

void GroupController::addDevice(const QString &serial)
{
    if (m_devices.contains(serial)) {
        return;
    }

    m_devices.append(serial);
}

void GroupController::removeDevice(const QString &serial)
{
    if (!m_devices.contains(serial)) {
        return;
    }

    m_devices.removeOne(serial);

    auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
    if (!device) {
        return;
    }

    if (isHost(serial)) {
        device->deRegisterDeviceObserver(this);
    }
}

void GroupController::onFrame(int width, int height, 
                              std::span<const uint8_t> dataY, 
                              std::span<const uint8_t> dataU, 
                              std::span<const uint8_t> dataV, 
                              int linesizeY, int linesizeU, int linesizeV)
{
    // GroupController tidak memproses video, hanya input control.
    // Jadi kita abaikan saja datanya.
    Q_UNUSED(width);
    Q_UNUSED(height);
    Q_UNUSED(dataY);
    Q_UNUSED(dataU);
    Q_UNUSED(dataV);
    Q_UNUSED(linesizeY);
    Q_UNUSED(linesizeU);
    Q_UNUSED(linesizeV);
}

void GroupController::mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize)
{
    Q_UNUSED(frameSize);
    forEachControlledDevice([&](const QString &serial, auto &device) {
        device->mouseEvent(from, getFrameSize(serial), showSize);
    });
}

void GroupController::wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize)
{
    Q_UNUSED(frameSize);
    forEachControlledDevice([&](const QString &serial, auto &device) {
        device->wheelEvent(from, getFrameSize(serial), showSize);
    });
}

void GroupController::keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize)
{
    Q_UNUSED(frameSize);
    forEachControlledDevice([&](const QString &serial, auto &device) {
        device->keyEvent(from, getFrameSize(serial), showSize);
    });
}

void GroupController::postGoBack()
{
    forEachControlledDevice([](const QString &, auto &device) { device->postGoBack(); });
}

void GroupController::postGoHome()
{
    forEachControlledDevice([](const QString &, auto &device) { device->postGoHome(); });
}

void GroupController::postGoMenu()
{
    forEachControlledDevice([](const QString &, auto &device) { device->postGoMenu(); });
}

void GroupController::postAppSwitch()
{
    forEachControlledDevice([](const QString &, auto &device) { device->postAppSwitch(); });
}

void GroupController::postPower()
{
    forEachControlledDevice([](const QString &, auto &device) { device->postPower(); });
}

void GroupController::postVolumeUp()
{
    forEachControlledDevice([](const QString &, auto &device) { device->postVolumeUp(); });
}

void GroupController::postVolumeDown()
{
    forEachControlledDevice([](const QString &, auto &device) { device->postVolumeDown(); });
}

void GroupController::postCopy()
{
    forEachControlledDevice([](const QString &, auto &device) { device->postCopy(); });
}

void GroupController::postCut()
{
    forEachControlledDevice([](const QString &, auto &device) { device->postCut(); });
}

void GroupController::setDisplayPower(bool on)
{
    forEachControlledDevice([on](const QString &, auto &device) { device->setDisplayPower(on); });
}

void GroupController::expandNotificationPanel()
{
    forEachControlledDevice([](const QString &, auto &device) { device->expandNotificationPanel(); });
}

void GroupController::collapsePanel()
{
    forEachControlledDevice([](const QString &, auto &device) { device->collapsePanel(); });
}

void GroupController::postBackOrScreenOn(bool down)
{
    forEachControlledDevice([down](const QString &, auto &device) { device->postBackOrScreenOn(down); });
}

void GroupController::postTextInput(QString &text)
{
    forEachControlledDevice([&text](const QString &, auto &device) { device->postTextInput(text); });
}

void GroupController::requestDeviceClipboard()
{
    forEachControlledDevice([](const QString &, auto &device) { device->requestDeviceClipboard(); });
}

void GroupController::setDeviceClipboard(bool pause)
{
    forEachControlledDevice([pause](const QString &, auto &device) { device->setDeviceClipboard(pause); });
}

void GroupController::clipboardPaste()
{
    forEachControlledDevice([](const QString &, auto &device) { device->clipboardPaste(); });
}

void GroupController::pushFileRequest(const QString &file, const QString &devicePath)
{
    forEachControlledDevice([&file, &devicePath](const QString &, auto &device) {
        device->pushFileRequest(file, devicePath);
    });
}

void GroupController::installApkRequest(const QString &apkFile)
{
    forEachControlledDevice([&apkFile](const QString &, auto &device) { device->installApkRequest(apkFile); });
}

void GroupController::screenshot()
{
    forEachControlledDevice([](const QString &, auto &device) { device->screenshot(); });
}

void GroupController::showTouch(bool show)
{
    forEachControlledDevice([show](const QString &, auto &device) { device->showTouch(show); });
}
