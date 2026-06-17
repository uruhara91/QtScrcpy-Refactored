from pathlib import Path

path = Path("QtScrcpy/ui/dialog.cpp")
text = path.read_text(encoding="utf-8-sig")


def replace_once(old: str, new: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"expected one match, found {count}: {old[:120]!r}")
    text = text.replace(old, new, 1)


replace_once('#include <QTime>\n', '')
replace_once(
    '#include "videoform.h"\n',
    '#include "videoform.h"\n#include "qtscrcpytelemetry.h"\n',
)

old_callback_start = text.index(
    '    connect(&m_adb, &qsc::AdbProcess::adbProcessResult, this, [this]('
)
old_callback_end = text.index('\n\n    m_hideIcon = new QSystemTrayIcon(this);', old_callback_start)
text = (
    text[:old_callback_start]
    + '    connect(&m_adb, &qsc::AdbProcess::adbProcessResult,\n'
      '            this, &Dialog::handleAdbResult);\n\n'
      '    m_workflowRetryTimer.setSingleShot(true);\n'
      '    m_workflowRetryTimer.setTimerType(Qt::PreciseTimer);\n'
      '    connect(&m_workflowRetryTimer, &QTimer::timeout, this, [this]() {\n'
      '        if (m_scheduledGeneration != m_workflowGeneration ||\n'
      '            m_adbWorkflow == AdbWorkflow::Idle) {\n'
      '            if (qsc::telemetry::enabled()) {\n'
      '                qInfo() << "[Telemetry][ADBWorkflow] stale-retry-dropped"\n'
      '                        << "scheduledGeneration=" << m_scheduledGeneration\n'
      '                        << "currentGeneration=" << m_workflowGeneration;\n'
      '            }\n'
      '            return;\n'
      '        }\n'
      '        executeWorkflowCommand(m_scheduledWorkflow,\n'
      '                               m_scheduledSerial,\n'
      '                               m_scheduledArguments);\n'
      '    });'
    + text[old_callback_end:]
)

replace_once(
    'Dialog::~Dialog()\n'
    '{\n'
    '    qDebug() << "~Dialog()";\n',
    'Dialog::~Dialog()\n'
    '{\n'
    '    m_workflowRetryTimer.stop();\n'
    '    qDebug() << "~Dialog()";\n',
)

old_delay = '''void Dialog::delayMs(int ms)
{
    QTime dieTime = QTime::currentTime().addMSecs(ms);

    while (QTime::currentTime() < dieTime) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    }
}

'''
replace_once(old_delay, '')

replace_once(
    '''bool Dialog::checkAdbRun()
{
    if (m_adb.isRuning()) {
        outLog("wait for the end of the current command to run");
    }
    return m_adb.isRuning();
}
''',
    '''bool Dialog::checkAdbRun()
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
''',
)

replace_once(
    '''void Dialog::on_stopAdbBtn_clicked()
{
    m_adb.kill();
}
''',
    '''void Dialog::on_stopAdbBtn_clicked()
{
    cancelAdbWorkflow(QStringLiteral("ADB workflow cancelled by user"));
    m_adb.kill();
}
''',
)

replace_once(
    '''void Dialog::on_usbConnectBtn_clicked()
{
    on_stopAllServerBtn_clicked();
    delayMs(200);
    on_updateDevice_clicked();
    delayMs(200);

    int firstUsbDevice = findDeviceFromeSerialBox(false);
    if (-1 == firstUsbDevice) {
        qWarning() << "No use device is found!";
        return;
    }
    ui->serialBox->setCurrentIndex(firstUsbDevice);

    on_startServerBtn_clicked();
}
''',
    '''void Dialog::on_usbConnectBtn_clicked()
{
    beginUsbWorkflow();
}
''',
)

replace_once(
    '''void Dialog::on_wifiConnectBtn_clicked()
{
    on_stopAllServerBtn_clicked();
    delayMs(200);

    on_updateDevice_clicked();
    delayMs(200);

    int firstUsbDevice = findDeviceFromeSerialBox(false);
    if (-1 == firstUsbDevice) {
        qWarning() << "No use device is found!";
        return;
    }
    ui->serialBox->setCurrentIndex(firstUsbDevice);

    on_getIPBtn_clicked();
    delayMs(200);

    on_startAdbdBtn_clicked();
    delayMs(1000);

    on_wirelessConnectBtn_clicked();
    delayMs(2000);

    on_updateDevice_clicked();
    delayMs(200);

    int firstWifiDevice = findDeviceFromeSerialBox(true);
    if (-1 == firstWifiDevice) {
        qWarning() << "No wifi device is found!";
        return;
    }
    ui->serialBox->setCurrentIndex(firstWifiDevice);

    on_startServerBtn_clicked();
}
''',
    '''void Dialog::on_wifiConnectBtn_clicked()
{
    beginWifiWorkflow();
}
''',
)

replace_once(
    '''void Dialog::on_autoUpdatecheckBox_toggled(bool checked)
{
    if (checked) {
        m_autoUpdatetimer.start(5000);
    } else {
        m_autoUpdatetimer.stop();
    }
}
''',
    '''void Dialog::on_autoUpdatecheckBox_toggled(bool checked)
{
    if (checked && m_adbWorkflow == AdbWorkflow::Idle) {
        m_autoUpdatetimer.start(5000);
    } else {
        m_autoUpdatetimer.stop();
    }
}
''',
)

if "delayMs(" in text:
    raise SystemExit("delayMs call remains")

path.write_text(text, encoding="utf-8")
