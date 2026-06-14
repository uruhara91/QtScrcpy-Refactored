#include <QDir>
#include <QMessageBox>
#include <QTimer>
#include <span>
#include <QMetaObject>

#include "controller.h"
#include "devicemsg.h"
#include "decoder.h"
#include "device.h"
#include "filehandler.h"
#include "recorder.h"
#include "server.h"
#include "demuxer.h"

extern "C" {
#include "libavcodec/avcodec.h"
}

namespace qsc {

Device::Device(DeviceParams params, QObject *parent) : IDevice(parent), m_params(params)
{
    if (!params.display && !m_params.recordFile) {
        qCritical("not display must be recorded");
        return;
    }

    if (params.display) {
        auto decoderCallback = [this](int width, int height, 
                                      std::span<const uint8_t> dataY, 
                                      std::span<const uint8_t> dataU, 
                                      std::span<const uint8_t> dataV, 
                                      int linesizeY, int linesizeU, int linesizeV) {
            for (const auto& item : m_deviceObservers) {
                item->onFrame(width, height, dataY, dataU, dataV, linesizeY, linesizeU, linesizeV);
            }
        };

        // Pass variabel callback
        m_decoder = std::make_unique<Decoder>(decoderCallback, nullptr);

        m_fileHandler = std::make_unique<FileHandler>(nullptr);
        
        auto controllerCallback = [this](const QByteArray& buffer) -> qint64 {
            if (!m_server || !m_server->getControlSocket()) return 0;
            return m_server->getControlSocket()->write(buffer.data(), buffer.length());
        };

        m_controller = std::make_unique<Controller>(controllerCallback, params.gameScript, nullptr);
    }

    m_stream = std::make_unique<Demuxer>(nullptr);
    m_server = std::make_unique<Server>(nullptr);

    if (m_params.recordFile && !m_params.recordPath.trimmed().isEmpty()) {
        QString absFilePath;
        QString fileDir(m_params.recordPath);
        if (!fileDir.isEmpty()) {
            // Gunakan format string yang efisien
            QDateTime dateTime = QDateTime::currentDateTime();
            QString fileName = m_params.serial + "_" + dateTime.toString("yyyyMMdd_hhmmss_zzz");
            fileName.replace(":", "_");
            fileName.replace(".", "_");
            fileName += ("." + m_params.recordFileFormat);
            
            QDir dir(fileDir);
            if (!dir.exists()) {
                if (!dir.mkpath(fileDir)) {
                    qCritical() << QString("Failed to create the save folder: %1").arg(fileDir);
                }
            }
            absFilePath = dir.absoluteFilePath(fileName);
        }
        m_recorder = std::make_unique<Recorder>(absFilePath, nullptr);
    }
    initSignals();
}

Device::~Device()
{
    Device::disconnectDevice();
}

void Device::setUserData(void *data)
{
    m_userData = data;
}

void *Device::getUserData()
{
    return m_userData;
}

void Device::registerDeviceObserver(DeviceObserver* observer) {
    if (!observer) return;
    if (std::ranges::find(m_deviceObservers, observer) == m_deviceObservers.end()) {
        m_deviceObservers.push_back(observer);
    }
}

void Device::deRegisterDeviceObserver(DeviceObserver* observer) {
    std::erase(m_deviceObservers, observer); 
}

const QString &Device::getSerial()
{
    return m_params.serial;
}

void Device::updateScript(QString script)
{
    if (m_controller) {
        m_controller->updateScript(script);
    }
}

void Device::screenshot()
{
    if (!m_decoder) {
        return;
    }

    m_decoder->peekFrame([this](int width, int height, uint8_t* dataRGB32) {
       saveFrame(width, height, dataRGB32);
    });
}

void Device::showTouch(bool show)
{
    AdbProcess *adb = new qsc::AdbProcess();
    if (!adb) {
        return;
    }
    connect(adb, &qsc::AdbProcess::adbProcessResult, this, [this](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        if (AdbProcess::AER_SUCCESS_START != processResult) {
            sender()->deleteLater();
        }
    });
    adb->setShowTouchesEnabled(getSerial(), show);

    qInfo() << getSerial() << " show touch " << (show ? "enable" : "disable");
}

bool Device::isReversePort(quint16 port)
{
    if (m_server && m_server->isReverse() && port == m_server->getParams().localPort) {
        return true;
    }

    return false;
}

void Device::initSignals()
{
    if (m_controller) {
        connect(m_controller.get(), &Controller::grabCursor, this, [this](bool grab){
            for (const auto& item : m_deviceObservers) {
                item->grabCursor(grab);
            }
        });
    }
    if (m_fileHandler) {
        connect(m_fileHandler.get(), &FileHandler::fileHandlerResult, this, [this](FileHandler::FILE_HANDLER_RESULT processResult, bool isApk) {
            QString tipsType = "";
            if (isApk) {
                tipsType = "install apk";
            } else {
                tipsType = "file transfer";
            }
            QString tips;
            if (FileHandler::FAR_IS_RUNNING == processResult) {
                tips = QString("wait current %1 to complete").arg(tipsType);
            }
            if (FileHandler::FAR_SUCCESS_EXEC == processResult) {
                tips = QString("%1 complete, save in %2").arg(tipsType).arg(m_params.pushFilePath);
            }
            if (FileHandler::FAR_ERROR_EXEC == processResult) {
                tips = QString("%1 failed").arg(tipsType);
            }
            qInfo() << tips;
        });
    }

    if (m_server) {
        connect(m_server.get(), &Server::serverStarted, this, [this](bool success, const QString &deviceName, const QSize &size) {
            m_serverStartSuccess = success;
            emit deviceConnected(success, m_params.serial, deviceName, size);
            if (success) {
                double diff = m_startTimeCount.elapsed() / 1000.0;
                qInfo() << QString("server start finish in %1s").arg(diff).toStdString().c_str();

                // init recorder
                if (m_recorder) {
                    m_recorder->setFrameSize(size);
                    if (!m_recorder->open()) {
                        qCritical("Could not open recorder");
                    }
                    if (!m_recorder->startRecorder()) {
                        qCritical("Could not start recorder");
                    }
                }

                // init decoder
                if (m_decoder) {
                    if (!m_decoder->open()) {
                        qCritical("Could not open decoder");
                        m_server->stop();
                        return;
                    }
                }

                // init stream
                m_stream->installVideoSocket(m_server->removeVideoSocket());
                m_stream->setFrameSize(size);
                
                if (!m_stream->startDecode()) {
                    qCritical("Could not start demuxer");
                    m_server->stop();
                    return;
                }

        
         connect(m_server->getControlSocket(), &QTcpSocket::readyRead, this, [this](){
            if (!m_controller) return;

            auto controlSocket = m_server->getControlSocket();
            int quota = 60;
            while (controlSocket->bytesAvailable() && quota-- > 0) { 
                QByteArray byteArray = controlSocket->peek(controlSocket->bytesAvailable());
                
                // FIX: Wajib pakai 'new' karena Receiver dan Qt Event Loop akan 
                // mengambil alih kepemilikan memori ini (dan men-delete-nya nanti).
                DeviceMsg *deviceMsg = new DeviceMsg(); 
                
                qint32 consume = deviceMsg->deserialize(byteArray);
                
                // Jika data belum lengkap, hapus memori yang barusan dibuat lalu keluar dari loop
                if (0 >= consume) {
                    delete deviceMsg; 
                    break;
                }
                
                controlSocket->read(consume);
                m_controller->recvDeviceMsg(deviceMsg); // Oper pointer aslinya
            }
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

        connect(m_stream.get(), &Demuxer::getFrame, this, [this](AVPacket *packet) {
            
            if (m_recorder) {
                AVPacket *recPacket = av_packet_clone(packet);
                if (recPacket) {
                    if (!m_recorder->push(recPacket)) {
                        av_packet_free(&recPacket);
                    }
                }
            }

            if (m_decoder) {
                bool sent = QMetaObject::invokeMethod(m_decoder.get(), "onDecodeFrame", 
                                          Qt::QueuedConnection, 
                                          Q_ARG(AVPacket*, packet));
                if (!sent) {
                    av_packet_free(&packet);
                }
            } else {
                av_packet_free(&packet);
            }

        }, Qt::DirectConnection);

        connect(m_stream.get(), &Demuxer::getConfigFrame, this, [this](AVPacket *packet) {
            if (m_recorder) {
                if (!m_recorder->push(packet)) {
                    av_packet_free(&packet);
                    qCritical("Could not send config packet to recorder");
                }
            } else {
                av_packet_free(&packet);
            }
        }, Qt::DirectConnection);
    }

    if (m_decoder) {
        connect(m_decoder.get(), &Decoder::updateFPS, this, [this](quint32 fps) {
            for (const auto& item : m_deviceObservers) {
                item->updateFPS(fps);
            }
        });
    }
}

bool Device::connectDevice()
{
    if (!m_server || m_serverStartSuccess) {
        return false;
    }

    QTimer::singleShot(0, this, [this]() {
        m_startTimeCount.start();
        Server::ServerParams params;
        params.serverLocalPath = m_params.serverLocalPath;
        params.serverRemotePath = m_params.serverRemotePath;
        params.serial = m_params.serial;
        params.localPort = m_params.localPort;
        params.maxSize = m_params.maxSize;
        params.bitRate = m_params.bitRate;
        params.maxFps = m_params.maxFps;
        params.useReverse = m_params.useReverse;
        params.captureOrientationLock = m_params.captureOrientationLock;
        params.captureOrientation = m_params.captureOrientation;
        params.stayAwake = m_params.stayAwake;
        params.serverVersion = m_params.serverVersion;
        params.logLevel = m_params.logLevel;
        params.codecOptions = m_params.codecOptions;
        params.codecName = m_params.codecName;
        params.scid = m_params.scid;

        params.crop = "";
        params.control = true;
        m_server->start(params);
    });

    return true;
}

void Device::disconnectDevice()
{
    if (m_server) {
        m_server->stop();
        m_server.reset();
    }

    if (m_stream) {
        m_stream->stopDecode();
        m_stream->quit();
        m_stream->wait();
        m_stream.reset();
    }

    if (m_decoder) {
        m_decoder->close();
        m_decoder.reset();
    }

    if (m_recorder) {
        if (m_recorder->isRunning()) {
            m_recorder->stopRecorder();
            m_recorder->wait();
        }
        m_recorder.reset();
    }

    m_controller.reset();
    m_fileHandler.reset();

    if (m_serverStartSuccess) {
        emit deviceDisconnected(m_params.serial);
    }
    m_serverStartSuccess = false;
}

void Device::postGoBack()
{
    if (!m_controller) {
        return;
    }
    m_controller->postGoBack();

    for (const auto& item : m_deviceObservers) {
        item->postGoBack();
    }
}

void Device::postGoHome()
{
    if (!m_controller) {
        return;
    }
    m_controller->postGoHome();

    for (const auto& item : m_deviceObservers) {
        item->postGoHome();
    }
}

void Device::postGoMenu()
{
    if (!m_controller) {
        return;
    }
    m_controller->postGoMenu();

    for (const auto& item : m_deviceObservers) {
        item->postGoMenu();
    }
}

void Device::postAppSwitch()
{
    if (!m_controller) {
        return;
    }
    m_controller->postAppSwitch();

    for (const auto& item : m_deviceObservers) {
        item->postAppSwitch();
    }
}

void Device::postPower()
{
    if (!m_controller) {
        return;
    }
    m_controller->postPower();

    for (const auto& item : m_deviceObservers) {
        item->postPower();
    }
}

void Device::postVolumeUp()
{
    if (!m_controller) {
        return;
    }
    m_controller->postVolumeUp();

    for (const auto& item : m_deviceObservers) {
        item->postVolumeUp();
    }
}

void Device::postVolumeDown()
{
    if (!m_controller) {
        return;
    }
    m_controller->postVolumeDown();

    for (const auto& item : m_deviceObservers) {
        item->postVolumeDown();
    }
}

void Device::postCopy()
{
    if (!m_controller) {
        return;
    }
    m_controller->copy();

    for (const auto& item : m_deviceObservers) {
        item->postCopy();
    }
}

void Device::postCut()
{
    if (!m_controller) {
        return;
    }
    m_controller->cut();

    for (const auto& item : m_deviceObservers) {
        item->postCut();
    }
}

void Device::setDisplayPower(bool on)
{
    if (!m_controller) {
        return;
    }
    m_controller->setDisplayPower(on);

    for (const auto& item : m_deviceObservers) {
        item->setDisplayPower(on);
    }
}

void Device::expandNotificationPanel()
{
    if (!m_controller) {
        return;
    }
    m_controller->expandNotificationPanel();

    for (const auto& item : m_deviceObservers) {
        item->expandNotificationPanel();
    }
}

void Device::collapsePanel()
{
    if (!m_controller) {
        return;
    }
    m_controller->collapsePanel();

    for (const auto& item : m_deviceObservers) {
        item->collapsePanel();
    }
}

void Device::postBackOrScreenOn(bool down)
{
    if (!m_controller) {
        return;
    }
    m_controller->postBackOrScreenOn(down);

    for (const auto& item : m_deviceObservers) {
        item->postBackOrScreenOn(down);
    }
}

void Device::postTextInput(QString &text)
{
    if (!m_controller) {
        return;
    }
    m_controller->postTextInput(text);

    for (const auto& item : m_deviceObservers) {
        item->postTextInput(text);
    }
}

void Device::requestDeviceClipboard()
{
    if (!m_controller) {
        return;
    }
    m_controller->requestDeviceClipboard();

    for (const auto& item : m_deviceObservers) {
        item->requestDeviceClipboard();
    }
}

void Device::setDeviceClipboard(bool pause)
{
    if (!m_controller) {
        return;
    }
    m_controller->setDeviceClipboard(pause);

    for (const auto& item : m_deviceObservers) {
        item->setDeviceClipboard(pause);
    }
}

void Device::clipboardPaste()
{
    if (!m_controller) {
        return;
    }
    m_controller->clipboardPaste();

    for (const auto& item : m_deviceObservers) {
        item->clipboardPaste();
    }
}

void Device::pushFileRequest(const QString &file, const QString &devicePath)
{
    if (!m_fileHandler) {
        return;
    }
    m_fileHandler->onPushFileRequest(getSerial(), file, devicePath);

    for (const auto& item : m_deviceObservers) {
        item->pushFileRequest(file, devicePath);
    }
}

void Device::installApkRequest(const QString &apkFile)
{
    if (!m_fileHandler) {
        return;
    }
    m_fileHandler->onInstallApkRequest(getSerial(), apkFile);

    for (const auto& item : m_deviceObservers) {
        item->installApkRequest(apkFile);
    }
}

void Device::mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (!m_controller) {
        return;
    }
    m_controller->mouseEvent(from, frameSize, showSize);

    for (const auto& item : m_deviceObservers) {
        item->mouseEvent(from, frameSize, showSize);
    }
}

void Device::wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (!m_controller) {
        return;
    }
    m_controller->wheelEvent(from, frameSize, showSize);

    for (const auto& item : m_deviceObservers) {
        item->wheelEvent(from, frameSize, showSize);
    }
}

void Device::keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (!m_controller) {
        return;
    }
    m_controller->keyEvent(from, frameSize, showSize);

    for (const auto& item : m_deviceObservers) {
        item->keyEvent(from, frameSize, showSize);
    }
}

bool Device::isCurrentCustomKeymap()
{
    if (!m_controller) {
        return false;
    }
    return m_controller->isCurrentCustomKeymap();
}

bool Device::saveFrame(int width, int height, uint8_t* dataRGB32)
{
    if (!dataRGB32) return false;
    QImage rgbImage(dataRGB32, width, height, QImage::Format_RGB32);
    QString fileDir(m_params.recordPath);
    if (fileDir.isEmpty()) {
        qWarning() << "please select record save path!!!";
        return false;
    }
    QDateTime dateTime = QDateTime::currentDateTime();
    QString fileName = m_params.serial + "_" + dateTime.toString("yyyyMMdd_hhmmss_zzz") + ".png";
    fileName.replace(":", "_");
    fileName.replace(".", "_");
    QDir dir(fileDir);
    return rgbImage.save(dir.absoluteFilePath(fileName), "PNG", 100);
}

}