#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <QThread>
#include <utility>

#include "devicemsg.h"
#include "qtscrcpytelemetry.h"
#include "receiver.h"

Receiver::Receiver(QObject *parent) : QObject(parent) {}

Receiver::~Receiver() {}

void Receiver::recvDeviceMsg(DeviceMsg *deviceMsg)
{
    if (!deviceMsg) return;

    switch (deviceMsg->type()) {
    case DeviceMsg::DMT_GET_CLIPBOARD: {
        QString text;
        deviceMsg->getClipboardMsgData(text);

        QCoreApplication *app = QCoreApplication::instance();
        const bool guiThread = app && QThread::currentThread() == app->thread();
        if (qsc::telemetryEnabled()) {
            qInfo() << "[Telemetry][Clipboard] device-response"
                    << "utf8Bytes=" << text.toUtf8().size()
                    << "thread=" << qsc::telemetryThreadId()
                    << "guiThread=" << guiThread;
        }

        auto applyClipboard = [text = std::move(text)]() {
            QClipboard *board = QApplication::clipboard();
            if (!board) {
                if (qsc::telemetryEnabled()) {
                    qWarning() << "[Telemetry][Clipboard] host-clipboard-unavailable";
                }
                return;
            }

            if (board->text() == text) {
                if (qsc::telemetryEnabled()) {
                    qInfo() << "[Telemetry][Clipboard] host-clipboard-unchanged";
                }
                return;
            }

            board->setText(text);
            if (qsc::telemetryEnabled()) {
                qInfo() << "[Telemetry][Clipboard] host-clipboard-updated"
                        << "thread=" << qsc::telemetryThreadId();
            }
        };

        if (guiThread) {
            applyClipboard();
        } else if (app) {
            QMetaObject::invokeMethod(app, std::move(applyClipboard),
                                      Qt::QueuedConnection);
        }
        break;
    }
    default:
        if (qsc::telemetryEnabled()) {
            qWarning() << "[Telemetry][Control] unsupported-device-message"
                       << "type=" << static_cast<int>(deviceMsg->type());
        }
        break;
    }
}
