#include <QDebug>
#include <QFile>
#include <QFileDialog>
#include <QKeyEvent>
#include <QRandomGenerator>
#include <QTimer>

#include "config.h"
#include "dialog.h"
#include "ui_dialog.h"
#include "videoform.h"
#include "qtscrcpytelemetry.h"
#include "../groupcontroller/groupcontroller.h"

#ifdef Q_OS_WIN32
#include "../util/winutils.h"
#endif

QString s_keyMapPath = "";

const QString &getKeyMapPath()
{
    if (s_keyMapPath.isEmpty()) {
        s_keyMapPath = QString::fromLocal8Bit(qgetenv("QTSCRCPY_KEYMAP_PATH"));
        QFileInfo fileInfo(s_keyMapPath);
        if (s_keyMapPath.isEmpty() || !fileInfo.isDir()) {
            s_keyMapPath = QCoreApplication::applicationDirPath() + "/keymap";
        }
    }
    return s_keyMapPath;
}

Dialog::Dialog(QWidget *parent) : QWidget(parent), ui(new Ui::Widget)
{
    ui->setupUi(this);
    this->setStyleSheet("background-color: black;");
    initUI();

    updateBootConfig(true);

    on_useSingleModeCheck_clicked();
    on_updateDevice_clicked();

    connect(&m_autoUpdatetimer, &QTimer::timeout, this, &Dialog::on_updateDevice_clicked);
    if (ui->autoUpdatecheckBox->isChecked()) {
        m_autoUpdatetimer.start(5000);
    }

    connect(&m_adb, &qsc::AdbProcess::adbProcessResult,
            this, &Dialog::handleAdbResult);

    m_workflowRetryTimer.setSingleShot(true);
    m_workflowRetryTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_workflowRetryTimer, &QTimer::timeout, this, [this]() {
        if (m_scheduledGeneration != m_workflowGeneration ||
            m_adbWorkflow == AdbWorkflow::Idle) {
            if (qsc::telemetry::enabled()) {
                qInfo() << "[Telemetry][ADBWorkflow] stale-retry-dropped"
                        << "scheduledGeneration=" << m_scheduledGeneration
                        << "currentGeneration=" << m_workflowGeneration;
            }
            return;
        }
        executeWorkflowCommand(m_scheduledWorkflow,
                               m_scheduledSerial,
                               m_scheduledArguments);
    });

    m_hideIcon = new QSystemTrayIcon(this);
    m_hideIcon->setIcon(QIcon(":/image/tray/logo.png"));
    m_menu = new QMenu(this);
    m_quit = new QAction(this);
    m_showWindow = new QAction(this);
    m_showWindow->setText(tr("show"));
    m_quit->setText(tr("quit"));
    m_menu->addAction(m_showWindow);
    m_menu->addAction(m_quit);
    m_hideIcon->setContextMenu(m_menu);
    m_hideIcon->show();
    connect(m_showWindow, &QAction::triggered, this, &Dialog::show);
    connect(m_quit, &QAction::triggered, this, [this]() {
        m_hideIcon->hide();
        qApp->quit();
    });
    connect(m_hideIcon, &QSystemTrayIcon::activated, this, &Dialog::slotActivated);

    connect(&qsc::IDeviceManage::getInstance(), &qsc::IDeviceManage::deviceConnected, this, &Dialog::onDeviceConnected);
    connect(&qsc::IDeviceManage::getInstance(), &qsc::IDeviceManage::deviceDisconnected, this, &Dialog::onDeviceDisconnected);
}

Dialog::~Dialog()
{
    m_workflowRetryTimer.stop();
    qDebug() << "~Dialog()";
    updateBootConfig(false);
    qsc::IDeviceManage::getInstance().disconnectAllDevice();
    delete ui;
}

void Dialog::initUI()
{
    setAttribute(Qt::WA_DeleteOnClose);
    //setWindowFlags(windowFlags() | Qt::WindowMinimizeButtonHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint);

    setWindowTitle(Config::getInstance().getTitle());
#ifdef Q_OS_LINUX
    // Set window icon (inherits from application icon set in main.cpp)
    // If application icon was set, this will use it automatically
    if (!qApp->windowIcon().isNull()) {
        setWindowIcon(qApp->windowIcon());
    }
#endif

#ifdef Q_OS_WIN32
    WinUtils::setDarkBorderToWindow((HWND)this->winId(), true);
#endif

    ui->bitRateEdit->setValidator(new QIntValidator(1, 99999, this));

    ui->maxSizeBox->addItem("640");
    ui->maxSizeBox->addItem("720");
    ui->maxSizeBox->addItem("1080");
    ui->maxSizeBox->addItem("1280");
    ui->maxSizeBox->addItem("1920");
    ui->maxSizeBox->addItem(tr("original"));

    ui->formatBox->addItem("mp4");
    ui->formatBox->addItem("mkv");

    ui->lockOrientationBox->addItem(tr("no lock"));
    ui->lockOrientationBox->addItem("0");
    ui->lockOrientationBox->addItem("90");
    ui->lockOrientationBox->addItem("180");
    ui->lockOrientationBox->addItem("270");
    ui->lockOrientationBox->setCurrentIndex(0);

    // 加载IP历史记录
    loadIpHistory();

    // 加载端口历史记录
    loadPortHistory();

    // 为deviceIpEdt添加右键菜单
    if (ui->deviceIpEdt->lineEdit()) {
        ui->deviceIpEdt->lineEdit()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(ui->deviceIpEdt->lineEdit(), &QWidget::customContextMenuRequested,
                this, &Dialog::showIpEditMenu);
    }
    
    // 为devicePortEdt添加右键菜单
    if (ui->devicePortEdt->lineEdit()) {
        ui->devicePortEdt->lineEdit()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(ui->devicePortEdt->lineEdit(), &QWidget::customContextMenuRequested,
                this, &Dialog::showPortEditMenu);
    }
}

void Dialog::updateBootConfig(bool toView)
{
    if (toView) {
        UserBootConfig config = Config::getInstance().getUserBootConfig();

        if (config.bitRate == 0) {
            ui->bitRateBox->setCurrentText("Mbps");
        } else if (config.bitRate % 1000000 == 0) {
            ui->bitRateEdit->setText(QString::number(config.bitRate / 1000000));
            ui->bitRateBox->setCurrentText("Mbps");
        } else {
            ui->bitRateEdit->setText(QString::number(config.bitRate / 1000));
            ui->bitRateBox->setCurrentText("Kbps");
        }

        ui->maxSizeBox->setCurrentIndex(config.maxSizeIndex);
        ui->formatBox->setCurrentIndex(config.recordFormatIndex);
        ui->recordPathEdt->setText(config.recordPath);
        ui->lockOrientationBox->setCurrentIndex(config.lockOrientationIndex);
        ui->framelessCheck->setChecked(config.framelessWindow);
        ui->recordScreenCheck->setChecked(config.recordScreen);
        ui->notDisplayCheck->setChecked(config.recordBackground);
        ui->useReverseCheck->setChecked(config.reverseConnect);
        ui->fpsCheck->setChecked(config.showFPS);
        ui->alwaysTopCheck->setChecked(config.windowOnTop);
        ui->closeScreenCheck->setChecked(config.autoOffScreen);
        ui->stayAwakeCheck->setChecked(config.keepAlive);
        ui->useRootCheck->setChecked(config.useRoot);
        ui->reniceIndicatorCheck->setChecked(config.useRoot);
        ui->useSingleModeCheck->setChecked(config.simpleMode);
        ui->autoUpdatecheckBox->setChecked(config.autoUpdateDevice);
        ui->showToolbar->setChecked(config.showToolbar);
    } else {
        UserBootConfig config;

        config.bitRate = getBitRate();
        config.maxSizeIndex = ui->maxSizeBox->currentIndex();
        config.recordFormatIndex = ui->formatBox->currentIndex();
        config.recordPath = ui->recordPathEdt->text();
        config.lockOrientationIndex = ui->lockOrientationBox->currentIndex();
        config.recordScreen = ui->recordScreenCheck->isChecked();
        config.recordBackground = ui->notDisplayCheck->isChecked();
        config.reverseConnect = ui->useReverseCheck->isChecked();
        config.showFPS = ui->fpsCheck->isChecked();
        config.windowOnTop = ui->alwaysTopCheck->isChecked();
        config.autoOffScreen = ui->closeScreenCheck->isChecked();
        config.framelessWindow = ui->framelessCheck->isChecked();
        config.keepAlive = ui->stayAwakeCheck->isChecked();
        config.useRoot = ui->useRootCheck->isChecked();
        config.simpleMode = ui->useSingleModeCheck->isChecked();
        config.autoUpdateDevice = ui->autoUpdatecheckBox->isChecked();
        config.showToolbar = ui->showToolbar->isChecked();

        // 保存当前IP到历史记录
        QString currentIp = ui->deviceIpEdt->currentText().trimmed();
        if (!currentIp.isEmpty()) {
            saveIpHistory(currentIp);
        }

        Config::getInstance().setUserBootConfig(config);
    }
}

void Dialog::execAdbCmd()
{
    if (checkAdbRun()) {
        return;
    }
    QString cmd = ui->adbCommandEdt->text().trimmed();
    outLog("adb " + cmd, false);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    m_adb.execute(ui->serialBox->currentText().trimmed(), cmd.split(" ", Qt::SkipEmptyParts));
#else
    m_adb.execute(ui->serialBox->currentText().trimmed(), cmd.split(" ", QString::SkipEmptyParts));
#endif
}

QString Dialog::getGameScript(const QString &fileName)
{
    if (fileName.isEmpty()) {
        return "";
    }

    QFile loadFile(getKeyMapPath() + "/" + fileName);
    if (!loadFile.open(QIODevice::ReadOnly)) {
        outLog("open file failed:" + fileName, true);
        return "";
    }

    QString ret = loadFile.readAll();
    loadFile.close();
    return ret;
}

void Dialog::slotActivated(QSystemTrayIcon::ActivationReason reason)
{
    switch (reason) {
    case QSystemTrayIcon::Trigger:
#ifdef Q_OS_WIN32
        this->show();
#endif
        break;
    default:
        break;
    }
}

void Dialog::closeEvent(QCloseEvent *event)
{
    this->hide();
    if (!Config::getInstance().getTrayMessageShown()) {
        Config::getInstance().setTrayMessageShown(true);
        m_hideIcon->showMessage(tr("Notice"),
                                tr("Hidden here!"),
                                QSystemTrayIcon::Information,
                                3000);
    }
    event->ignore();
}

void Dialog::on_updateDevice_clicked()
{
    if (checkAdbRun()) {
        return;
    }
    outLog("update devices...", false);
    m_adb.execute("", QStringList() << "devices");
}

void Dialog::on_startServerBtn_clicked()
{
    outLog("start server...", false);

    // this is ok that "original" toUshort is 0
    quint16 videoSize = ui->maxSizeBox->currentText().trimmed().toUShort();
    qsc::DeviceParams params;
    params.serial = ui->serialBox->currentText().trimmed();
    params.maxSize = videoSize;
    params.bitRate = getBitRate();
    // on devices with Android >= 10, the capture frame rate can be limited
    params.maxFps = static_cast<quint32>(Config::getInstance().getMaxFps());
    params.closeScreen = ui->closeScreenCheck->isChecked();
    params.useReverse = ui->useReverseCheck->isChecked();
    params.display = !ui->notDisplayCheck->isChecked();
    params.renderExpiredFrames = Config::getInstance().getRenderExpiredFrames();
    if (ui->lockOrientationBox->currentIndex() > 0) {
        params.captureOrientationLock = 1;
        params.captureOrientation = (ui->lockOrientationBox->currentIndex() - 1) * 90;
    }
    params.stayAwake = ui->stayAwakeCheck->isChecked();
    params.useRoot = ui->useRootCheck->isChecked();
    params.recordFile = ui->recordScreenCheck->isChecked();
    params.recordPath = ui->recordPathEdt->text().trimmed();
    params.recordFileFormat = ui->formatBox->currentText().trimmed();
    params.serverLocalPath = getServerPath();
    params.serverRemotePath = Config::getInstance().getServerPath();
    params.pushFilePath = Config::getInstance().getPushFilePath();
    params.gameScript = getGameScript(ui->gameBox->currentText());
    params.logLevel = Config::getInstance().getLogLevel();
    params.codecOptions = Config::getInstance().getCodecOptions();
    params.codecName = Config::getInstance().getCodecName();
    params.scid = QRandomGenerator::global()->bounded(1, 10000) & 0x7FFFFFFF;

    qsc::IDeviceManage::getInstance().connectDevice(params);
}

void Dialog::on_stopServerBtn_clicked()
{
    if (qsc::IDeviceManage::getInstance().disconnectDevice(ui->serialBox->currentText().trimmed())) {
        outLog("stop server");
    }
}

void Dialog::on_wirelessConnectBtn_clicked()
{
    if (checkAdbRun()) {
        return;
    }
    QString addr = ui->deviceIpEdt->currentText().trimmed();
    if (addr.isEmpty()) {
        outLog("error: device ip is null", false);
        return;
    }

    if (!ui->devicePortEdt->currentText().isEmpty()) {
        addr += ":";
        addr += ui->devicePortEdt->currentText().trimmed();
    } else if (!ui->devicePortEdt->lineEdit()->placeholderText().isEmpty()) {
        addr += ":";
        addr += ui->devicePortEdt->lineEdit()->placeholderText().trimmed();
    } else {
        outLog("error: device port is null", false);
        return;
    }

    // 保存IP历史记录 - 只保存IP部分,不包含端口
    QString ip = addr.split(":").first();
    if (!ip.isEmpty()) {
        saveIpHistory(ip);
    }
    
    // 保存端口历史记录
    QString port = addr.split(":").last();
    if (!port.isEmpty() && port != ip) {
        savePortHistory(port);
    }

    outLog("wireless connect...", false);
    QStringList adbArgs;
    adbArgs << "connect";
    adbArgs << addr;
    m_adb.execute("", adbArgs);
}

void Dialog::on_startAdbdBtn_clicked()
{
    if (checkAdbRun()) {
        return;
    }
    outLog("start devices adbd...", false);
    // adb tcpip 5555
    QStringList adbArgs;
    adbArgs << "tcpip";
    adbArgs << "5555";
    m_adb.execute(ui->serialBox->currentText().trimmed(), adbArgs);
}

void Dialog::outLog(const QString &log, bool newLine)
{
    // avoid sub thread update ui
    QString backLog = log;
    QTimer::singleShot(0, this, [this, backLog, newLine]() {
        ui->outEdit->append(backLog);
        if (newLine) {
            ui->outEdit->append("<br/>");
        }
    });
}

bool Dialog::filterLog(const QString &log)
{
    if (log.contains("app_proces")) {
        return true;
    }
    if (log.contains("Unable to set geometry")) {
        return true;
    }
    return false;
}

bool Dialog::checkAdbRun()
{
    const bool workflowActive = m_adbWorkflow != AdbWorkflow::Idle;
    const bool adbRunning = m_adb.isRuning();
    if (workflowActive || adbRunning) {
        outLog(workflowActive
                   ? QStringLiteral("wait for the current device workflow to finish")
                   : QStringLiteral("wait for the current adb command to finish"));
    }
    return workflowActive || adbRunning;
}

const char *Dialog::workflowName(AdbWorkflow state) noexcept
{
    switch (state) {
    case AdbWorkflow::Idle: return "idle";
    case AdbWorkflow::UsbScan: return "usb-scan";
    case AdbWorkflow::WifiScanUsb: return "wifi-scan-usb";
    case AdbWorkflow::WifiGetIpIfconfig: return "wifi-get-ip-ifconfig";
    case AdbWorkflow::WifiGetIpFallback: return "wifi-get-ip-fallback";
    case AdbWorkflow::WifiTcpip: return "wifi-tcpip";
    case AdbWorkflow::WifiConnect: return "wifi-connect";
    case AdbWorkflow::WifiRescan: return "wifi-rescan";
    }
    return "unknown";
}

void Dialog::updateDeviceLists(const QStringList &devices)
{
    const QString previous = ui->serialBox->currentText().trimmed();
    ui->serialBox->clear();
    ui->connectedPhoneList->clear();

    for (const QString &serial : devices) {
        ui->serialBox->addItem(serial);
        ui->connectedPhoneList->addItem(
            Config::getInstance().getNickName(serial) + "-" + serial);
    }

    const int previousIndex = findSerialIndex(previous);
    if (previousIndex >= 0) ui->serialBox->setCurrentIndex(previousIndex);
}

int Dialog::findSerialIndex(const QString &serial) const
{
    if (serial.isEmpty()) return -1;
    for (int i = 0; i < ui->serialBox->count(); ++i) {
        if (ui->serialBox->itemText(i).trimmed() == serial) return i;
    }
    return -1;
}

QString Dialog::wifiAddressFromUi() const
{
    const QString ip = ui->deviceIpEdt->currentText().trimmed();
    QString port = ui->devicePortEdt->currentText().trimmed();
    if (port.isEmpty() && ui->devicePortEdt->lineEdit()) {
        port = ui->devicePortEdt->lineEdit()->placeholderText().trimmed();
    }
    if (port.isEmpty()) port = QStringLiteral("5555");
    return ip.isEmpty() ? QString() : ip + QLatin1Char(':') + port;
}

void Dialog::executeWorkflowCommand(AdbWorkflow state,
                                    const QString &serial,
                                    const QStringList &arguments)
{
    if (m_adbWorkflow == AdbWorkflow::Idle || m_adb.isRuning()) return;

    m_adbWorkflow = state;
    if (state == AdbWorkflow::WifiConnect) ++m_wifiConnectAttempts;
    m_workflowStepElapsed.restart();

    if (qsc::telemetry::enabled()) {
        qInfo() << "[Telemetry][ADBWorkflow] command"
                << "generation=" << m_workflowGeneration
                << "state=" << workflowName(state)
                << "serial=" << serial
                << "arguments=" << arguments
                << "wifiAttempt=" << m_wifiConnectAttempts;
    }
    m_adb.execute(serial, arguments);
}

void Dialog::scheduleWorkflowCommand(AdbWorkflow state,
                                     const QString &serial,
                                     const QStringList &arguments,
                                     int delayMs)
{
    if (m_adbWorkflow == AdbWorkflow::Idle) return;

    m_adbWorkflow = state;
    m_scheduledWorkflow = state;
    m_scheduledSerial = serial;
    m_scheduledArguments = arguments;
    m_scheduledGeneration = m_workflowGeneration;

    if (qsc::telemetry::enabled()) {
        qInfo() << "[Telemetry][ADBWorkflow] retry-scheduled"
                << "generation=" << m_workflowGeneration
                << "state=" << workflowName(state)
                << "delayMs=" << delayMs
                << "wifiAttempt=" << m_wifiConnectAttempts;
    }
    m_workflowRetryTimer.start(qMax(0, delayMs));
}

void Dialog::finishAdbWorkflow(bool success, const QString &message)
{
    const AdbWorkflow finishedState = m_adbWorkflow;
    const qint64 totalMs = m_workflowElapsed.isValid()
        ? m_workflowElapsed.elapsed()
        : -1;

    m_workflowRetryTimer.stop();
    m_adbWorkflow = AdbWorkflow::Idle;
    m_scheduledWorkflow = AdbWorkflow::Idle;
    ++m_workflowGeneration;

    if (qsc::telemetry::enabled()) {
        qInfo() << "[Telemetry][ADBWorkflow] complete"
                << "state=" << workflowName(finishedState)
                << "success=" << success
                << "totalMs=" << totalMs
                << "wifiAttempts=" << m_wifiConnectAttempts
                << "message=" << message;
    }

    if (!message.isEmpty()) {
        if (success) outLog(message, true);
        else qWarning().noquote() << message;
    }

    m_workflowUsbSerial.clear();
    m_workflowWifiAddress.clear();
    m_scheduledSerial.clear();
    m_scheduledArguments.clear();
    m_wifiConnectAttempts = 0;

    if (m_resumeAutoUpdateAfterWorkflow &&
        ui->autoUpdatecheckBox->isChecked()) {
        m_autoUpdatetimer.start(5000);
    }
    m_resumeAutoUpdateAfterWorkflow = false;
}

void Dialog::cancelAdbWorkflow(const QString &reason)
{
    if (m_adbWorkflow == AdbWorkflow::Idle) return;
    if (qsc::telemetry::enabled()) {
        qInfo() << "[Telemetry][ADBWorkflow] cancelled"
                << "generation=" << m_workflowGeneration
                << "state=" << workflowName(m_adbWorkflow)
                << "reason=" << reason;
    }
    finishAdbWorkflow(false, reason);
}

void Dialog::beginUsbWorkflow()
{
    if (checkAdbRun()) return;

    on_stopAllServerBtn_clicked();
    ++m_workflowGeneration;
    m_adbWorkflow = AdbWorkflow::UsbScan;
    m_workflowElapsed.restart();
    m_wifiConnectAttempts = 0;
    m_resumeAutoUpdateAfterWorkflow = m_autoUpdatetimer.isActive();
    m_autoUpdatetimer.stop();

    if (qsc::telemetry::enabled()) {
        qInfo() << "[Telemetry][ADBWorkflow] begin"
                << "generation=" << m_workflowGeneration
                << "kind=usb";
    }
    executeWorkflowCommand(AdbWorkflow::UsbScan, QString(),
                           {QStringLiteral("devices")});
}

void Dialog::beginWifiWorkflow()
{
    if (checkAdbRun()) return;

    on_stopAllServerBtn_clicked();
    ++m_workflowGeneration;
    m_adbWorkflow = AdbWorkflow::WifiScanUsb;
    m_workflowElapsed.restart();
    m_wifiConnectAttempts = 0;
    m_resumeAutoUpdateAfterWorkflow = m_autoUpdatetimer.isActive();
    m_autoUpdatetimer.stop();

    if (qsc::telemetry::enabled()) {
        qInfo() << "[Telemetry][ADBWorkflow] begin"
                << "generation=" << m_workflowGeneration
                << "kind=wifi";
    }
    executeWorkflowCommand(AdbWorkflow::WifiScanUsb, QString(),
                           {QStringLiteral("devices")});
}

void Dialog::handleAdbResult(qsc::AdbProcess::ADB_EXEC_RESULT result)
{
    const QStringList arguments = m_adb.arguments();

    if (result == qsc::AdbProcess::AER_SUCCESS_START) {
        if (m_adbWorkflow == AdbWorkflow::Idle) outLog("adb run", false);
        return;
    }

    if (m_adbWorkflow != AdbWorkflow::Idle) {
        if (qsc::telemetry::enabled()) {
            qInfo() << "[Telemetry][ADBWorkflow] result"
                    << "generation=" << m_workflowGeneration
                    << "state=" << workflowName(m_adbWorkflow)
                    << "result=" << static_cast<int>(result)
                    << "stepMs="
                    << (m_workflowStepElapsed.isValid()
                            ? m_workflowStepElapsed.elapsed()
                            : -1)
                    << "stdout=" << m_adb.getStdOut().right(512)
                    << "stderr=" << m_adb.getErrorOut().right(512);
        }
        advanceAdbWorkflow(result);
        return;
    }

    handleGenericAdbResult(result, arguments);
}

void Dialog::handleGenericAdbResult(
    qsc::AdbProcess::ADB_EXEC_RESULT result,
    const QStringList &arguments)
{
    QString log;
    switch (result) {
    case qsc::AdbProcess::AER_ERROR_START:
        log = QStringLiteral("adb failed to start");
        break;
    case qsc::AdbProcess::AER_ERROR_EXEC:
        if (arguments.contains(QStringLiteral("ifconfig")) &&
            arguments.contains(QStringLiteral("wlan0"))) {
            getIPbyIp();
        }
        break;
    case qsc::AdbProcess::AER_ERROR_MISSING_BINARY:
        log = QStringLiteral("adb not found");
        break;
    case qsc::AdbProcess::AER_SUCCESS_EXEC:
        if (arguments.contains(QStringLiteral("devices"))) {
            updateDeviceLists(m_adb.getDevicesSerialFromStdOut());
        } else if ((arguments.contains(QStringLiteral("show")) ||
                    arguments.contains(QStringLiteral("ifconfig"))) &&
                   arguments.contains(QStringLiteral("wlan0"))) {
            const QString ip = m_adb.getDeviceIPFromStdOut();
            log = ip.isEmpty()
                ? QStringLiteral("ip not find, connect to wifi?")
                : QString();
            if (!ip.isEmpty()) ui->deviceIpEdt->setEditText(ip);
        } else if (arguments.contains(QStringLiteral("ip -o a"))) {
            const QString ip = m_adb.getDeviceIPByIpFromStdOut();
            log = ip.isEmpty()
                ? QStringLiteral("ip not find, connect to wifi?")
                : QString();
            if (!ip.isEmpty()) ui->deviceIpEdt->setEditText(ip);
        }
        break;
    case qsc::AdbProcess::AER_SUCCESS_START:
        break;
    }

    if (!log.isEmpty()) outLog(log, true);
}

void Dialog::advanceAdbWorkflow(qsc::AdbProcess::ADB_EXEC_RESULT result)
{
    const bool success = result == qsc::AdbProcess::AER_SUCCESS_EXEC;
    if (result == qsc::AdbProcess::AER_ERROR_START ||
        result == qsc::AdbProcess::AER_ERROR_MISSING_BINARY) {
        finishAdbWorkflow(false, QStringLiteral("ADB workflow could not start"));
        return;
    }

    switch (m_adbWorkflow) {
    case AdbWorkflow::UsbScan: {
        if (!success) {
            finishAdbWorkflow(false, QStringLiteral("USB device scan failed"));
            return;
        }
        updateDeviceLists(m_adb.getDevicesSerialFromStdOut());
        const int index = findDeviceFromeSerialBox(false);
        if (index < 0) {
            finishAdbWorkflow(false, QStringLiteral("No USB device is available"));
            return;
        }
        ui->serialBox->setCurrentIndex(index);
        finishAdbWorkflow(true, QStringLiteral("USB device selected"));
        on_startServerBtn_clicked();
        return;
    }
    case AdbWorkflow::WifiScanUsb: {
        if (!success) {
            finishAdbWorkflow(false, QStringLiteral("USB device scan failed"));
            return;
        }
        updateDeviceLists(m_adb.getDevicesSerialFromStdOut());
        const int index = findDeviceFromeSerialBox(false);
        if (index < 0) {
            finishAdbWorkflow(false, QStringLiteral("No USB device is available"));
            return;
        }
        ui->serialBox->setCurrentIndex(index);
        m_workflowUsbSerial = ui->serialBox->currentText().trimmed();
        executeWorkflowCommand(
            AdbWorkflow::WifiGetIpIfconfig,
            m_workflowUsbSerial,
            {QStringLiteral("shell"), QStringLiteral("ifconfig"),
             QStringLiteral("wlan0")});
        return;
    }
    case AdbWorkflow::WifiGetIpIfconfig: {
        const QString ip = success ? m_adb.getDeviceIPFromStdOut() : QString();
        if (ip.isEmpty()) {
            executeWorkflowCommand(
                AdbWorkflow::WifiGetIpFallback,
                m_workflowUsbSerial,
                {QStringLiteral("shell"), QStringLiteral("ip -o a")});
            return;
        }
        ui->deviceIpEdt->setEditText(ip);
        m_workflowWifiAddress = wifiAddressFromUi();
        if (m_workflowWifiAddress.isEmpty()) {
            finishAdbWorkflow(false, QStringLiteral("Could not determine Wi-Fi address"));
            return;
        }
        executeWorkflowCommand(
            AdbWorkflow::WifiTcpip,
            m_workflowUsbSerial,
            {QStringLiteral("tcpip"),
             m_workflowWifiAddress.section(QLatin1Char(':'), -1)});
        return;
    }
    case AdbWorkflow::WifiGetIpFallback: {
        const QString ip = success ? m_adb.getDeviceIPByIpFromStdOut() : QString();
        if (ip.isEmpty()) {
            finishAdbWorkflow(false, QStringLiteral("Could not determine device Wi-Fi IP"));
            return;
        }
        ui->deviceIpEdt->setEditText(ip);
        m_workflowWifiAddress = wifiAddressFromUi();
        if (m_workflowWifiAddress.isEmpty()) {
            finishAdbWorkflow(false, QStringLiteral("Could not determine Wi-Fi address"));
            return;
        }
        executeWorkflowCommand(
            AdbWorkflow::WifiTcpip,
            m_workflowUsbSerial,
            {QStringLiteral("tcpip"),
             m_workflowWifiAddress.section(QLatin1Char(':'), -1)});
        return;
    }
    case AdbWorkflow::WifiTcpip:
        if (!success) {
            finishAdbWorkflow(false, QStringLiteral("Could not restart adbd in TCP mode"));
            return;
        }
        scheduleWorkflowCommand(
            AdbWorkflow::WifiConnect,
            QString(),
            {QStringLiteral("connect"), m_workflowWifiAddress},
            250);
        return;
    case AdbWorkflow::WifiConnect: {
        const QString response =
            (m_adb.getStdOut() + QLatin1Char(' ') + m_adb.getErrorOut()).toLower();
        const bool connected = success &&
            (response.contains(QStringLiteral("connected to")) ||
             response.contains(QStringLiteral("already connected")));
        if (!connected) {
            if (m_wifiConnectAttempts < 8 &&
                (!m_workflowElapsed.isValid() ||
                 m_workflowElapsed.elapsed() < 12000)) {
                const int delay = qMin(1000, 250 * m_wifiConnectAttempts);
                scheduleWorkflowCommand(
                    AdbWorkflow::WifiConnect,
                    QString(),
                    {QStringLiteral("connect"), m_workflowWifiAddress},
                    delay);
                return;
            }
            finishAdbWorkflow(false, QStringLiteral("Could not connect to device over Wi-Fi"));
            return;
        }
        scheduleWorkflowCommand(
            AdbWorkflow::WifiRescan,
            QString(),
            {QStringLiteral("devices")},
            150);
        return;
    }
    case AdbWorkflow::WifiRescan: {
        if (!success) {
            finishAdbWorkflow(false, QStringLiteral("Wi-Fi device scan failed"));
            return;
        }
        updateDeviceLists(m_adb.getDevicesSerialFromStdOut());
        const int index = findSerialIndex(m_workflowWifiAddress);
        if (index < 0) {
            if (m_wifiConnectAttempts < 8) {
                scheduleWorkflowCommand(
                    AdbWorkflow::WifiConnect,
                    QString(),
                    {QStringLiteral("connect"), m_workflowWifiAddress},
                    500);
                return;
            }
            finishAdbWorkflow(false, QStringLiteral("Connected Wi-Fi endpoint was not listed"));
            return;
        }
        ui->serialBox->setCurrentIndex(index);
        saveIpHistory(ui->deviceIpEdt->currentText().trimmed());
        savePortHistory(m_workflowWifiAddress.section(QLatin1Char(':'), -1));
        finishAdbWorkflow(true, QStringLiteral("Wi-Fi device selected"));
        on_startServerBtn_clicked();
        return;
    }
    case AdbWorkflow::Idle:
        return;
    }
}

void Dialog::on_getIPBtn_clicked()
{
    if (checkAdbRun()) {
        return;
    }

    outLog("get ip...", false);
    // adb -s P7C0218510000537 shell ifconfig wlan0
    // or
    // adb -s P7C0218510000537 shell ip -f inet addr show wlan0
    QStringList adbArgs;
#if 0
    adbArgs << "shell";
    adbArgs << "ip";
    adbArgs << "-f";
    adbArgs << "inet";
    adbArgs << "addr";
    adbArgs << "show";
    adbArgs << "wlan0";
#else
    adbArgs << "shell";
    adbArgs << "ifconfig";
    adbArgs << "wlan0";
#endif
    m_adb.execute(ui->serialBox->currentText().trimmed(), adbArgs);
}

void Dialog::getIPbyIp()
{
    if (checkAdbRun()) {
        return;
    }

    QStringList adbArgs;
    adbArgs << "shell";
    adbArgs << "ip -o a";

    m_adb.execute(ui->serialBox->currentText().trimmed(), adbArgs);
}

void Dialog::onDeviceConnected(bool success, const QString &serial, const QString &deviceName, const QSize &size)
{
    Q_UNUSED(deviceName);
    if (!success) {
        return;
    }
    auto videoForm = new VideoForm(ui->framelessCheck->isChecked(), Config::getInstance().getSkin(), ui->showToolbar->isChecked());
    videoForm->setSerial(serial);

    qsc::IDeviceManage::getInstance().getDevice(serial)->setUserData(static_cast<void*>(videoForm));
    qsc::IDeviceManage::getInstance().getDevice(serial)->registerDeviceObserver(videoForm);


    videoForm->showFPS(ui->fpsCheck->isChecked());

    if (ui->alwaysTopCheck->isChecked()) {
        videoForm->staysOnTop();
    }

#ifndef Q_OS_WIN32
    // must be show before updateShowSize
    videoForm->show();
#endif
    QString name = Config::getInstance().getNickName(serial);
    if (name.isEmpty()) {
        name = Config::getInstance().getTitle();
    }
    videoForm->setWindowTitle(name + "-" + serial);
    videoForm->updateShowSize(size);

    bool deviceVer = size.height() > size.width();
    QRect rc = Config::getInstance().getRect(serial);
    bool rcVer = rc.height() > rc.width();
    // same width/height rate
    if (rc.isValid() && (deviceVer == rcVer)) {
        // mark: resize is for fix setGeometry magneticwidget bug
        videoForm->resize(rc.size());
        videoForm->setGeometry(rc);
    }

#ifdef Q_OS_WIN32
    // windows是show太早可以看到resize的过程
    QTimer::singleShot(200, videoForm, [videoForm](){videoForm->show();});
#endif

    GroupController::instance().addDevice(serial);
}

void Dialog::onDeviceDisconnected(const QString &serial)
{
    GroupController::instance().removeDevice(serial);
    auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
    if (!device) {
        return;
    }
    auto data = device->getUserData();
    if (data) {
        VideoForm* vf = static_cast<VideoForm*>(data);
        qsc::IDeviceManage::getInstance().getDevice(serial)->deRegisterDeviceObserver(vf);
        vf->close();
        vf->deleteLater();
    }
}

void Dialog::on_wirelessDisConnectBtn_clicked()
{
    if (checkAdbRun()) {
        return;
    }
    QString addr = ui->deviceIpEdt->currentText().trimmed();
    outLog("wireless disconnect...", false);
    QStringList adbArgs;
    adbArgs << "disconnect";
    adbArgs << addr;
    m_adb.execute("", adbArgs);
}

void Dialog::on_selectRecordPathBtn_clicked()
{
    QFileDialog::Options options = QFileDialog::DontResolveSymlinks | QFileDialog::ShowDirsOnly;
    QString directory = QFileDialog::getExistingDirectory(this, tr("select path"), "", options);
    ui->recordPathEdt->setText(directory);
}

void Dialog::on_recordPathEdt_textChanged(const QString &arg1)
{
    ui->recordPathEdt->setToolTip(arg1.trimmed());
    ui->notDisplayCheck->setCheckable(!arg1.trimmed().isEmpty());
}

void Dialog::on_adbCommandBtn_clicked()
{
    execAdbCmd();
}

void Dialog::on_stopAdbBtn_clicked()
{
    cancelAdbWorkflow(QStringLiteral("ADB workflow cancelled by user"));
    m_adb.kill();
}

void Dialog::on_clearOut_clicked()
{
    ui->outEdit->clear();
}

void Dialog::on_stopAllServerBtn_clicked()
{
    qsc::IDeviceManage::getInstance().disconnectAllDevice();
}

void Dialog::on_refreshGameScriptBtn_clicked()
{
    ui->gameBox->clear();
    QDir dir(getKeyMapPath());
    if (!dir.exists()) {
        outLog("keymap directory not find", true);
        return;
    }
    dir.setFilter(QDir::Files | QDir::NoSymLinks);
    QFileInfoList list = dir.entryInfoList();
    QFileInfo fileInfo;
    int size = list.size();
    for (int i = 0; i < size; ++i) {
        fileInfo = list.at(i);
        ui->gameBox->addItem(fileInfo.fileName());
    }
}

void Dialog::on_applyScriptBtn_clicked()
{
    auto curSerial = ui->serialBox->currentText().trimmed();
    auto device = qsc::IDeviceManage::getInstance().getDevice(curSerial);
    if (!device) {
        return;
    }

    device->updateScript(getGameScript(ui->gameBox->currentText()));
}

void Dialog::on_recordScreenCheck_clicked(bool checked)
{
    if (!checked) {
        return;
    }

    QString fileDir(ui->recordPathEdt->text().trimmed());
    if (fileDir.isEmpty()) {
        qWarning() << "please select record save path!!!";
        ui->recordScreenCheck->setChecked(false);
    }
}

void Dialog::on_useRootCheck_clicked(bool checked)
{
    // Renice/ionice boosting is only meaningful (and only permitted) when
    // the server runs as root, so the indicator just mirrors this checkbox.
    ui->reniceIndicatorCheck->setChecked(checked);
}

void Dialog::on_usbConnectBtn_clicked()
{
    beginUsbWorkflow();
}

int Dialog::findDeviceFromeSerialBox(bool wifi)
{
    QString regStr = "\\b(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\:([0-9]|[1-9]\\d|[1-9]\\d{2}|[1-9]\\d{3}|[1-5]\\d{4}|6[0-4]\\d{3}|65[0-4]\\d{2}|655[0-2]\\d|6553[0-5])\\b";
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QRegExp regIP(regStr);
#else
    QRegularExpression regIP(regStr);
#endif
    for (int i = 0; i < ui->serialBox->count(); ++i) {
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        bool isWifi = regIP.exactMatch(ui->serialBox->itemText(i));
#else
        bool isWifi = regIP.match(ui->serialBox->itemText(i)).hasMatch();
#endif
        bool found = wifi ? isWifi : !isWifi;
        if (found) {
            return i;
        }
    }

    return -1;
}

void Dialog::on_wifiConnectBtn_clicked()
{
    beginWifiWorkflow();
}

void Dialog::on_connectedPhoneList_itemDoubleClicked(QListWidgetItem *item)
{
    Q_UNUSED(item);
    ui->serialBox->setCurrentIndex(ui->connectedPhoneList->currentRow());
    on_startServerBtn_clicked();
}

void Dialog::on_updateNameBtn_clicked()
{
    if (ui->serialBox->count() != 0) {
        if (ui->userNameEdt->text().isEmpty()) {
            Config::getInstance().setNickName(ui->serialBox->currentText(), "Phone");
        } else {
            Config::getInstance().setNickName(ui->serialBox->currentText(), ui->userNameEdt->text());
        }

        on_updateDevice_clicked();

        qDebug() << "Update OK!";
    } else {
        qWarning() << "No device is connected!";
    }
}

void Dialog::on_useSingleModeCheck_clicked()
{
    if (ui->useSingleModeCheck->isChecked()) {
        ui->rightWidget->hide();
    } else {
        ui->rightWidget->show();
    }

    adjustSize();
}

void Dialog::on_serialBox_currentIndexChanged(const QString &arg1)
{
    ui->userNameEdt->setText(Config::getInstance().getNickName(arg1));
}

quint32 Dialog::getBitRate()
{
    return ui->bitRateEdit->text().trimmed().toUInt() *
            (ui->bitRateBox->currentText() == QString("Mbps") ? 1000000 : 1000);
}

const QString &Dialog::getServerPath()
{
    static QString serverPath;
    if (serverPath.isEmpty()) {
        serverPath = QString::fromLocal8Bit(qgetenv("QTSCRCPY_SERVER_PATH"));
        QFileInfo fileInfo(serverPath);
        if (serverPath.isEmpty() || !fileInfo.isFile()) {
            serverPath = QCoreApplication::applicationDirPath() + "/scrcpy-server";
        }
    }
    return serverPath;
}

void Dialog::on_startAudioBtn_clicked()
{
    if (ui->serialBox->count() == 0) {
        qWarning() << "No device is connected!";
        return;
    }

    // Panggil Native C++ Start (Instant)
    QString serial = ui->serialBox->currentText().trimmed();
    m_audioOutput.start(serial, 28200);
    
    outLog("Starting audio forwarding...", true);
}

void Dialog::on_stopAudioBtn_clicked()
{
    m_audioOutput.stop();
    outLog("Audio forwarding stopped.", true);
}

void Dialog::on_autoUpdatecheckBox_toggled(bool checked)
{
    if (checked && m_adbWorkflow == AdbWorkflow::Idle) {
        m_autoUpdatetimer.start(5000);
    } else {
        m_autoUpdatetimer.stop();
    }
}

void Dialog::loadIpHistory()
{
    QStringList ipList = Config::getInstance().getIpHistory();
    ui->deviceIpEdt->clear();
    ui->deviceIpEdt->addItems(ipList);
    ui->deviceIpEdt->setContentsMargins(0, 0, 0, 0);

    if (ui->deviceIpEdt->lineEdit()) {
        ui->deviceIpEdt->lineEdit()->setMaxLength(128);
        ui->deviceIpEdt->lineEdit()->setPlaceholderText("192.168.0.1");
    }
}

void Dialog::saveIpHistory(const QString &ip)
{
    if (ip.isEmpty()) {
        return;
    }
    
    Config::getInstance().saveIpHistory(ip);
    
    // 更新ComboBox
    loadIpHistory();
    ui->deviceIpEdt->setCurrentText(ip);
}

void Dialog::showIpEditMenu(const QPoint &pos)
{
    QMenu *menu = ui->deviceIpEdt->lineEdit()->createStandardContextMenu();
    menu->addSeparator();
    
    QAction *clearHistoryAction = new QAction(tr("Clear History"), menu);
    connect(clearHistoryAction, &QAction::triggered, this, [this]() {
        Config::getInstance().clearIpHistory();
        loadIpHistory();
    });
    
    menu->addAction(clearHistoryAction);
    menu->exec(ui->deviceIpEdt->lineEdit()->mapToGlobal(pos));
    delete menu;
}

void Dialog::loadPortHistory()
{
    QStringList portList = Config::getInstance().getPortHistory();
    ui->devicePortEdt->clear();
    ui->devicePortEdt->addItems(portList);
    ui->devicePortEdt->setContentsMargins(0, 0, 0, 0);

    if (ui->devicePortEdt->lineEdit()) {
        ui->devicePortEdt->lineEdit()->setMaxLength(6);
        ui->devicePortEdt->lineEdit()->setPlaceholderText("5555");
    }
}

void Dialog::savePortHistory(const QString &port)
{
    if (port.isEmpty()) {
        return;
    }
    
    Config::getInstance().savePortHistory(port);
    
    // 更新ComboBox
    loadPortHistory();
    ui->devicePortEdt->setCurrentText(port);
}

void Dialog::showPortEditMenu(const QPoint &pos)
{
    QMenu *menu = ui->devicePortEdt->lineEdit()->createStandardContextMenu();
    menu->addSeparator();
    
    QAction *clearHistoryAction = new QAction(tr("Clear History"), menu);
    connect(clearHistoryAction, &QAction::triggered, this, [this]() {
        Config::getInstance().clearPortHistory();
        loadPortHistory();
    });
    
    menu->addAction(clearHistoryAction);
    menu->exec(ui->devicePortEdt->lineEdit()->mapToGlobal(pos));
    delete menu;
}
