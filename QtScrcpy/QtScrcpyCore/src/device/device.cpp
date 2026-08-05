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
            },
            m_params.useHwDecode);

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
    auto *adb = new AdbProcess(this);
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
            Q_UNUSED(size); // always invalid now, see note below
            m_serverStartSuccess = ok;
            if (!ok) {
                emit deviceConnected(ok, m_params.serial, name, QSize());
                m_server->stop();
                return;
            }

            qInfo() << "server start finish in"
                    << m_startTimeCount.elapsed() / 1000.0 << "s";

            // NOTE: since scrcpy-server 4.0, the video size is no longer
            // sent during the handshake, so it is not available here
            // anymore. We stash the device name and defer emitting
            // deviceConnected() (and opening the recorder) until the
            // demuxer reports the real size via sessionInfo(), once the
            // stream's first session packet has been parsed.
            m_pendingDeviceName = name;

            if (m_decoder && !m_decoder->open()) {
                qCritical("Could not open decoder");
                emit deviceConnected(false, m_params.serial, name, QSize());
                m_server->stop();
                return;
            }

            m_stream->installVideoSocket(m_server->removeVideoSocket());
            if (!m_stream->startDecode()) {
                qCritical("Could not start demuxer");
                emit deviceConnected(false, m_params.serial, name, QSize());
                m_server->stop();
                return;
            }

            if (QTcpSocket *socket = m_server->getControlSocket()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                    if (!m_controller) return;
                    // Peek once per invocation instead of once per message:
                    // re-peeking the full buffer on every loop iteration
                    // copies everything already seen this call over and
                    // over. sliced() below shares the underlying buffer
                    // (no extra copy) since we only ever read from it.
                    const QByteArray bytes = socket->peek(socket->bytesAvailable());
                    qint32 offset = 0;
                    int quota = 60;
                    bool protocolError = false;
                    while (offset < bytes.size() && quota-- > 0) {
                        DeviceMsg message;
                        const qint32 consumed = message.deserialize(bytes.sliced(offset));
                        if (consumed < 0) {
                            // Malformed/unexpected data: the same bad prefix
                            // will still be sitting unconsumed at the head
                            // of the socket the next time readyRead fires,
                            // so previously this silently wedged the whole
                            // control channel forever (clipboard sync dead,
                            // socket buffer growing unbounded) rather than
                            // surfacing an error. Treat it as a desynced
                            // connection and drop it instead of retrying.
                            qWarning("Device control channel: malformed message, "
                                     "closing connection to resync");
                            protocolError = true;
                            break;
                        }
                        if (consumed == 0) break; // need more bytes, wait for next readyRead
                        offset += consumed;
                        m_controller->recvDeviceMsg(&message);
                    }
                    if (offset > 0) socket->read(offset);
                    if (protocolError) socket->close();
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

        // scrcpy-server >= 4.0: the video size is not known at handshake
        // time anymore. It arrives here, from the demuxer thread, as soon
        // as the stream's first session packet is parsed - which happens
        // before any config/key frame is emitted.
        //
        // Two separate connections are used deliberately:
        //
        //  1) DirectConnection to open/start the recorder synchronously on
        //     the demuxer thread itself. The recorder needs the size up
        //     front to write the output stream's codec parameters, and this
        //     must complete before the subsequent getConfigFrame/getFrame
        //     signals (also DirectConnection) try to push packets into it.
        //     This handler does not touch Qt GUI objects, so running on the
        //     demuxer thread is safe.
        //
        //  2) A plain (auto) connection to emit deviceConnected(), so Qt
        //     delivers it queued back to whatever thread `this` (the
        //     Device, normally on the GUI/main thread) lives on - exactly
        //     like every other signal this class emits. Emitting a public,
        //     UI-facing signal directly from the demuxer thread would be
        //     unsafe for callers that touch widgets in their slot.
        connect(m_stream.get(), &Demuxer::sessionInfo, this,
                [this](QSize size, bool /*clientResized*/) {
            if (!size.isValid() || m_firstSessionInfoHandled) return;
            m_firstSessionInfoHandled = true;

            if (m_recorder) {
                m_recorder->setFrameSize(size);
                if (!m_recorder->open() || !m_recorder->startRecorder()) {
                    qCritical("Could not start recorder");
                }
            }
        }, Qt::DirectConnection);

        connect(m_stream.get(), &Demuxer::sessionInfo, this,
                [this](QSize size, bool clientResized) {
            if (!size.isValid()) return;

            if (!m_deviceConnectedEmitted) {
                m_deviceConnectedEmitted = true;
                emit deviceConnected(true, m_params.serial,
                                      m_pendingDeviceName, size);
                return;
            }

            // A later session packet (e.g. after a device-side rotate,
            // resize, or a client-initiated RESIZE_DISPLAY) changed the
            // size again. Re-opening the recorder mid-recording with a
            // different size isn't supported - MP4/MKV don't cleanly
            // support a stream's declared dimensions changing partway
            // through, and Recorder::open() writes codecpar->width/height
            // once, from whatever size was current at the time. Silently
            // continuing to feed it packets at the new size would produce
            // a file whose container-level metadata is wrong for the
            // rest of the recording - how that actually looks when
            // played back depends entirely on how forgiving a given
            // player is about trusting the in-stream SPS over the
            // container header. Stop and finalize the recording cleanly
            // instead: everything captured before this point is written
            // out correctly (valid, playable file, just shorter than
            // expected), and it's a clear, loud signal rather than a
            // silently-wrong tail. Same shutdown sequence
            // disconnectDevice() already uses for the recorder.
            if (m_recorder && m_recorder->isRunning()) {
                qWarning() << "Video size changed mid-session to" << size
                           << "(clientResized=" << clientResized
                           << "); stopping the recording - a resolution "
                              "change mid-recording is not supported. "
                              "Start a new recording to capture the new size.";
                m_recorder->stopRecorder();
                m_recorder->wait();
                m_recorder.reset();
            }
        });

        connect(m_stream.get(), &Demuxer::getFrame, this,
                [this](AVPacket *raw) {
            PacketHandle packet(raw);
            if (!packet) return;

            if (m_recorder && !m_recorder->isFailed()) {
                PacketHandle copy = clonePacketReference(packet.get());
                if (copy && m_recorder->push(copy.get())) (void)copy.release();
            }
            if (m_decoder) (void)m_decoder->enqueuePacket(std::move(packet));
        }, Qt::DirectConnection);

        connect(m_stream.get(), &Demuxer::getConfigFrame, this,
                [this](AVPacket *raw) {
            PacketHandle packet(raw);
            if (packet && m_recorder && !m_recorder->isFailed() &&
                m_recorder->push(packet.get()))
                (void)packet.release();
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
        p.useRoot = m_params.useRoot;
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
    m_pendingDeviceName.clear();
    m_firstSessionInfoHandled = false;
    m_deviceConnectedEmitted = false;
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

void Device::relativeMouseMoveEvent(const QPointF &delta, const QSize &frame, const QSize &show)
{
    if (!m_controller) return;
    m_controller->relativeMouseMoveEvent(delta, frame, show);
    forEachObserver([&](DeviceObserver &o){ o.relativeMouseMoveEvent(delta, frame, show); });
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

void Device::cancelActiveInputs()
{
    if (m_controller) m_controller->cancelActiveInputs();
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
