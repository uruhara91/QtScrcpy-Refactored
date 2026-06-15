#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QTcpSocket>
#include <QTimer>
#include <algorithm>
#include <mutex>
#include <utility>

#include "controller.h"
#include "devicemsg.h"
#include "decoder.h"
#include "device.h"
#include "filehandler.h"
#include "recorder.h"
#include "server.h"
#include "demuxer.h"

namespace qsc {

Device::Device(DeviceParams params, QObject *parent)
    : IDevice(parent), m_params(std::move(params))
{
    if (!m_params.display && !m_params.recordFile) {
        qCritical("not display must be recorded");
        return;
    }

    if (m_params.display) {
        m_decoder = std::make_unique<Decoder>(
            [this](int w, int h,
                   std::span<const uint8_t> y,
                   std::span<const uint8_t> u,
                   std::span<const uint8_t> v,
                   int sy, int su, int sv) {
                std::shared_lock<std::shared_mutex> lock(m_frameSinkMutex);
                if (m_frameSink) {
                    m_frameSink->submitFrame(w, h, y, u, v, sy, su, sv);
                }
            });

        m_fileHandler = std::make_unique<FileHandler>();
        m_controller = std::make_unique<Controller>(
            [this](const QByteArray &data) -> qint64 {
                QTcpSocket *socket = m_server ? m_server->getControlSocket() : nullptr;
                return socket ? socket->write(data.constData(), data.size()) : 0;
            },
            m_params.gameScript);
    }

    m_stream = std::make_unique<Demuxer>();
    m_server = std::make_unique<Server>();

    if (m_params.recordFile && !m_params.recordPath.trimmed().isEmpty()) {
        QDir dir(m_params.recordPath);
        if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
            qCritical() << "Failed to create save folder:" << m_params.recordPath;
        }
        QString name = m_params.serial + QLatin1Char('_') +
                       QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss_zzz") +
                       QLatin1Char('.') + m_params.recordFileFormat;
        name.replace(QLatin1Char(':'), QLatin1Char('_'));
        m_recorder = std::make_unique<Recorder>(dir.absoluteFilePath(name));
    }

    initSignals();
}

Device::~Device()
{
    disconnectDevice();
}

void Device::setUserData(void *data)
{
    m_userData = data;
}

void *Device::getUserData()
{
    return m_userData;
}

void Device::registerFrameSink(FrameSink *sink)
{
    if (!sink) return;

    sink->activateFrameSink();
    std::unique_lock<std::shared_mutex> lock(m_frameSinkMutex);
    m_frameSink = sink;
}

void Device::deRegisterFrameSink(FrameSink *sink)
{
    if (!sink) return;

    {
        std::unique_lock<std::shared_mutex> lock(m_frameSinkMutex);
        if (m_frameSink != sink) return;
        m_frameSink = nullptr;
    }
    sink->deactivateFrameSink();
}

void Device::registerDeviceObserver(DeviceObserver *observer)
{
    if (!observer) return;

    if (auto *sink = dynamic_cast<FrameSink *>(observer)) {
        registerFrameSink(sink);
    }

    std::unique_lock<std::shared_mutex> lock(m_observerMutex);
    if (std::find(m_deviceObservers.begin(), m_deviceObservers.end(), observer) ==
        m_deviceObservers.end()) {
        m_deviceObservers.push_back(observer);
    }
}

void Device::deRegisterDeviceObserver(DeviceObserver *observer)
{
    if (!observer) return;

    if (auto *sink = dynamic_cast<FrameSink *>(observer)) {
        deRegisterFrameSink(sink);
    }

    std::unique_lock<std::shared_mutex> lock(m_observerMutex);
    std::erase(m_deviceObservers, observer);
}

const QString &Device::getSerial()
{
    return m_params.serial;
}

void Device::updateScript(QString script)
{
    if (m_controller) m_controller->updateScript(std::move(script));
}

void Device::screenshot()
{
    if (m_decoder) {
        m_decoder->peekFrame([this](int w, int h, uint8_t *rgb) {
            saveFrame(w, h, rgb);
        });
    }
}

void Device::showTouch(bool show)
{
    auto *adb = new AdbProcess();
    connect(adb, &AdbProcess::adbProcessResult, adb,
            [adb](AdbProcess::ADB_EXEC_RESULT result) {
        if (result != AdbProcess::AER_SUCCESS_START) adb->deleteLater();
    });
    adb->setShowTouchesEnabled(getSerial(), show);
}

bool Device::isReversePort(quint16 port)
{
    return m_server && m_server->isReverse() &&
           port == m_server->getParams().localPort;
}

void Device::initSignals()
{
    if (m_controller) {
        connect(m_controller.get(), &Controller::grabCursor, this, [this](bool grab) {
            forEachObserver([grab](DeviceObserver &o) { o.grabCursor(grab); });
        });
    }

    if (m_fileHandler) {
        connect(m_fileHandler.get(), &FileHandler::fileHandlerResult, this,
                [this](FileHandler::FILE_HANDLER_RESULT result, bool apk) {
            const QString action = apk ? QStringLiteral("install apk")
                                       : QStringLiteral("file transfer");
            if (result == FileHandler::FAR_IS_RUNNING)
                qInfo() << QStringLiteral("wait current %1 to complete").arg(action);
            else if (result == FileHandler::FAR_SUCCESS_EXEC)
                qInfo() << QStringLiteral("%1 complete, save in %2")
                               .arg(action, m_params.pushFilePath);
            else if (result == FileHandler::FAR_ERROR_EXEC)
                qInfo() << QStringLiteral("%1 failed").arg(action);
        });
    }

    if (m_server) {
        connect(m_server.get(), &Server::serverStarted, this,
                [this](bool ok, const QString &name, const QSize &size) {
            m_serverStartSuccess = ok;
            emit deviceConnected(ok, m_params.serial, name, size);
            if (!ok) {
                m_server->stop();
                return;
            }

            qInfo() << "server start finish in"
                    << m_startTimeCount.elapsed() / 1000.0 << "s";

            if (m_recorder) {
                m_recorder->setFrameSize(size);
                if (!m_recorder->open() || !m_recorder->startRecorder())
                    qCritical("Could not start recorder");
            }
            if (m_decoder && !m_decoder->open()) {
                qCritical("Could not open decoder");
                m_server->stop();
                return;
            }

            m_stream->installVideoSocket(m_server->removeVideoSocket());
            m_stream->setFrameSize(size);
            if (!m_stream->startDecode()) {
                qCritical("Could not start demuxer");
                m_server->stop();
                return;
            }

            if (QTcpSocket *socket = m_server->getControlSocket()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                    if (!m_controller) return;
                    int quota = 60;
                    while (socket->bytesAvailable() > 0 && quota-- > 0) {
                        const QByteArray bytes = socket->peek(socket->bytesAvailable());
                        DeviceMsg message;
                        const qint32 consumed = message.deserialize(bytes);
                        if (consumed <= 0) break;
                        socket->read(consumed);
                        m_controller->recvDeviceMsg(&message);
                    }
                });
            }

            if (m_params.closeScreen && m_params.display && m_controller)
                m_controller->setDisplayPower(false);
        });

        connect(m_server.get(), &Server::serverStoped, this, [this]() {
            disconnectDevice();
            qDebug() << "server process stop";
        });
    }

    if (m_stream) {
        connect(m_stream.get(), &Demuxer::onStreamStop, this, [this]() {
            disconnectDevice();
            qDebug() << "stream thread stop";
        });

        connect(m_stream.get(), &Demuxer::getFrame, this,
                [this](AVPacket *raw) {
            PacketHandle packet(raw);
            if (!packet) return;

            if (m_recorder) {
                PacketHandle copy = clonePacketReference(packet.get());
                if (copy && m_recorder->push(copy.get())) copy.release();
            }
            if (m_decoder) (void)m_decoder->enqueuePacket(std::move(packet));
        }, Qt::DirectConnection);

        connect(m_stream.get(), &Demuxer::getConfigFrame, this,
                [this](AVPacket *raw) {
            PacketHandle packet(raw);
            if (packet && m_recorder && m_recorder->push(packet.get()))
                packet.release();
        }, Qt::DirectConnection);
    }

    if (m_decoder) {
        connect(m_decoder.get(), &Decoder::updateFPS, this, [this](quint32 fps) {
            forEachObserver([fps](DeviceObserver &o) { o.updateFPS(fps); });
        });
    }
}

bool Device::connectDevice()
{
    if (!m_server || m_serverStartSuccess) return false;
    QTimer::singleShot(0, this, [this]() {
        if (!m_server) return;
        m_startTimeCount.start();
        Server::ServerParams p;
        p.serverLocalPath = m_params.serverLocalPath;
        p.serverRemotePath = m_params.serverRemotePath;
        p.serial = m_params.serial;
        p.localPort = m_params.localPort;
        p.maxSize = m_params.maxSize;
        p.bitRate = m_params.bitRate;
        p.maxFps = m_params.maxFps;
        p.useReverse = m_params.useReverse;
        p.captureOrientationLock = m_params.captureOrientationLock;
        p.captureOrientation = m_params.captureOrientation;
        p.stayAwake = m_params.stayAwake;
        p.serverVersion = m_params.serverVersion;
        p.logLevel = m_params.logLevel;
        p.codecOptions = m_params.codecOptions;
        p.codecName = m_params.codecName;
        p.scid = m_params.scid;
        p.crop.clear();
        p.control = true;
        m_server->start(p);
    });
    return true;
}

void Device::disconnectDevice()
{
    bool expected = false;
    if (!m_disconnecting.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
        return;
    }

    if (m_server) { m_server->stop(); m_server.reset(); }
    if (m_stream) { m_stream->stopDecode(); m_stream.reset(); }
    if (m_decoder) { m_decoder->close(); m_decoder.reset(); }
    if (m_recorder) {
        if (m_recorder->isRunning()) { m_recorder->stopRecorder(); m_recorder->wait(); }
        m_recorder.reset();
    }
    m_controller.reset();
    m_fileHandler.reset();
    if (m_serverStartSuccess) emit deviceDisconnected(m_params.serial);
    m_serverStartSuccess = false;
    m_disconnecting.store(false, std::memory_order_release);
}

void Device::postGoBack() { if (m_controller) { m_controller->postGoBack(); forEachObserver([](DeviceObserver &o){ o.postGoBack(); }); } }
void Device::postGoHome() { if (m_controller) { m_controller->postGoHome(); forEachObserver([](DeviceObserver &o){ o.postGoHome(); }); } }
void Device::postGoMenu() { if (m_controller) { m_controller->postGoMenu(); forEachObserver([](DeviceObserver &o){ o.postGoMenu(); }); } }
void Device::postAppSwitch() { if (m_controller) { m_controller->postAppSwitch(); forEachObserver([](DeviceObserver &o){ o.postAppSwitch(); }); } }
void Device::postPower() { if (m_controller) { m_controller->postPower(); forEachObserver([](DeviceObserver &o){ o.postPower(); }); } }
void Device::postVolumeUp() { if (m_controller) { m_controller->postVolumeUp(); forEachObserver([](DeviceObserver &o){ o.postVolumeUp(); }); } }
void Device::postVolumeDown() { if (m_controller) { m_controller->postVolumeDown(); forEachObserver([](DeviceObserver &o){ o.postVolumeDown(); }); } }
void Device::postCopy() { if (m_controller) { m_controller->copy(); forEachObserver([](DeviceObserver &o){ o.postCopy(); }); } }
void Device::postCut() { if (m_controller) { m_controller->cut(); forEachObserver([](DeviceObserver &o){ o.postCut(); }); } }
void Device::setDisplayPower(bool on) { if (m_controller) { m_controller->setDisplayPower(on); forEachObserver([on](DeviceObserver &o){ o.setDisplayPower(on); }); } }
void Device::expandNotificationPanel() { if (m_controller) { m_controller->expandNotificationPanel(); forEachObserver([](DeviceObserver &o){ o.expandNotificationPanel(); }); } }
void Device::collapsePanel() { if (m_controller) { m_controller->collapsePanel(); forEachObserver([](DeviceObserver &o){ o.collapsePanel(); }); } }
void Device::postBackOrScreenOn(bool down) { if (m_controller) { m_controller->postBackOrScreenOn(down); forEachObserver([down](DeviceObserver &o){ o.postBackOrScreenOn(down); }); } }
void Device::postTextInput(QString &text) { if (m_controller) { m_controller->postTextInput(text); forEachObserver([&](DeviceObserver &o){ o.postTextInput(text); }); } }
void Device::requestDeviceClipboard() { if (m_controller) { m_controller->requestDeviceClipboard(); forEachObserver([](DeviceObserver &o){ o.requestDeviceClipboard(); }); } }
void Device::setDeviceClipboard(bool pause) { if (m_controller) { m_controller->setDeviceClipboard(pause); forEachObserver([pause](DeviceObserver &o){ o.setDeviceClipboard(pause); }); } }
void Device::clipboardPaste() { if (m_controller) { m_controller->clipboardPaste(); forEachObserver([](DeviceObserver &o){ o.clipboardPaste(); }); } }

void Device::pushFileRequest(const QString &file, const QString &path)
{
    if (!m_fileHandler) return;
    m_fileHandler->onPushFileRequest(getSerial(), file, path);
    forEachObserver([&](DeviceObserver &o){ o.pushFileRequest(file, path); });
}

void Device::installApkRequest(const QString &apk)
{
    if (!m_fileHandler) return;
    m_fileHandler->onInstallApkRequest(getSerial(), apk);
    forEachObserver([&](DeviceObserver &o){ o.installApkRequest(apk); });
}

void Device::mouseEvent(const QMouseEvent *e, const QSize &frame, const QSize &show)
{
    if (!m_controller) return;
    m_controller->mouseEvent(e, frame, show);
    forEachObserver([&](DeviceObserver &o){ o.mouseEvent(e, frame, show); });
}

void Device::wheelEvent(const QWheelEvent *e, const QSize &frame, const QSize &show)
{
    if (!m_controller) return;
    m_controller->wheelEvent(e, frame, show);
    forEachObserver([&](DeviceObserver &o){ o.wheelEvent(e, frame, show); });
}

void Device::keyEvent(const QKeyEvent *e, const QSize &frame, const QSize &show)
{
    if (!m_controller) return;
    m_controller->keyEvent(e, frame, show);
    forEachObserver([&](DeviceObserver &o){ o.keyEvent(e, frame, show); });
}

bool Device::isCurrentCustomKeymap()
{
    return m_controller && m_controller->isCurrentCustomKeymap();
}

bool Device::saveFrame(int w, int h, uint8_t *rgb)
{
    if (!rgb || w <= 0 || h <= 0 || m_params.recordPath.isEmpty()) return false;
    QImage image(rgb, w, h, QImage::Format_RGB32);
    QString name = m_params.serial + QLatin1Char('_') +
                   QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss_zzz") +
                   QStringLiteral(".png");
    name.replace(QLatin1Char(':'), QLatin1Char('_'));
    return image.save(QDir(m_params.recordPath).absoluteFilePath(name), "PNG", 100);
}

} // namespace qsc
