#ifndef DIALOG_H
#define DIALOG_H

#include <QComboBox>
#include <QElapsedTimer>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWidget>

#include "adbprocess.h"
#include "../QtScrcpyCore/include/QtScrcpyCore.h"
#include "audio/audiooutput.h"

namespace Ui
{
    class Widget;
}

class QYUVOpenGLWidget;
class Dialog : public QWidget
{
    Q_OBJECT

public:
    explicit Dialog(QWidget *parent = nullptr);
    ~Dialog() override;

    void outLog(const QString &log, bool newLine = true);
    bool filterLog(const QString &log);
    void getIPbyIp();

private slots:
    void onDeviceConnected(bool success, const QString &serial,
                           const QString &deviceName, const QSize &size);
    void onDeviceDisconnected(const QString &serial);

    void on_updateDevice_clicked();
    void on_startServerBtn_clicked();
    void on_stopServerBtn_clicked();
    void on_wirelessConnectBtn_clicked();
    void on_startAdbdBtn_clicked();
    void on_getIPBtn_clicked();
    void on_wirelessDisConnectBtn_clicked();
    void on_selectRecordPathBtn_clicked();
    void on_recordPathEdt_textChanged(const QString &arg1);
    void on_adbCommandBtn_clicked();
    void on_stopAdbBtn_clicked();
    void on_clearOut_clicked();
    void on_stopAllServerBtn_clicked();
    void on_refreshGameScriptBtn_clicked();
    void on_applyScriptBtn_clicked();
    void on_recordScreenCheck_clicked(bool checked);
    void on_useRootCheck_clicked(bool checked);
    void on_usbConnectBtn_clicked();
    void on_wifiConnectBtn_clicked();
    void on_connectedPhoneList_itemDoubleClicked(QListWidgetItem *item);
    void on_updateNameBtn_clicked();
    void on_useSingleModeCheck_clicked();

    // Qt 6 auto-connects QComboBox::currentIndexChanged(int). Forward it to
    // the existing text-based implementation without changing behavior.
    void on_serialBox_currentIndexChanged(int index)
    {
        Q_UNUSED(index);
        auto *serialBox = findChild<QComboBox *>(QStringLiteral("serialBox"));
        on_serialBox_currentIndexChanged(
            serialBox ? serialBox->currentText() : QString());
    }

    void on_startAudioBtn_clicked();
    void on_stopAudioBtn_clicked();

    void on_autoUpdatecheckBox_toggled(bool checked);
    void showIpEditMenu(const QPoint &pos);

private:
    enum class AdbWorkflow
    {
        Idle,
        UsbScan,
        WifiScanUsb,
        WifiGetIpIfconfig,
        WifiGetIpFallback,
        WifiTcpip,
        WifiConnect,
        WifiRescan,
    };

    // Keep the implementation already defined in dialog.cpp, but do not expose
    // it as a Qt slot because Qt 6 has no matching QString signal overload.
    void on_serialBox_currentIndexChanged(const QString &arg1);

    bool checkAdbRun();
    void initUI();
    void updateBootConfig(bool toView = true);
    void execAdbCmd();
    QString getGameScript(const QString &fileName);
    void slotActivated(QSystemTrayIcon::ActivationReason reason);
    int findDeviceFromeSerialBox(bool wifi);
    quint32 getBitRate();
    const QString &getServerPath();
    void loadIpHistory();
    void saveIpHistory(const QString &ip);
    void loadPortHistory();
    void savePortHistory(const QString &port);
    void showPortEditMenu(const QPoint &pos);

    void handleAdbResult(qsc::AdbProcess::ADB_EXEC_RESULT result);
    void handleGenericAdbResult(qsc::AdbProcess::ADB_EXEC_RESULT result,
                                const QStringList &arguments);
    void advanceAdbWorkflow(qsc::AdbProcess::ADB_EXEC_RESULT result);
    void beginUsbWorkflow();
    void beginWifiWorkflow();
    void executeWorkflowCommand(AdbWorkflow state,
                                const QString &serial,
                                const QStringList &arguments);
    void scheduleWorkflowCommand(AdbWorkflow state,
                                 const QString &serial,
                                 const QStringList &arguments,
                                 int delayMs);
    void finishAdbWorkflow(bool success, const QString &message = QString());
    void cancelAdbWorkflow(const QString &reason);
    void updateDeviceLists(const QStringList &devices);
    QString wifiAddressFromUi() const;
    int findSerialIndex(const QString &serial) const;
    static const char *workflowName(AdbWorkflow state) noexcept;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    Ui::Widget *ui;
    qsc::AdbProcess m_adb;
    QSystemTrayIcon *m_hideIcon;
    QMenu *m_menu;
    QAction *m_showWindow;
    QAction *m_quit;
    AudioOutput m_audioOutput;
    QMetaObject::Connection m_audioStartedConnection = QObject::connect(
        &m_audioOutput,
        &AudioOutput::started,
        this,
        [this]() {
            outLog(QStringLiteral("Audio codec and output sink initialized."),
                   true);
        });
    QMetaObject::Connection m_audioErrorConnection = QObject::connect(
        &m_audioOutput,
        &AudioOutput::errorOccurred,
        this,
        [this](const QString &message) {
            outLog(message, true);
        });
    QTimer m_autoUpdatetimer;
    QTimer m_workflowRetryTimer;
    AdbWorkflow m_adbWorkflow = AdbWorkflow::Idle;
    AdbWorkflow m_scheduledWorkflow = AdbWorkflow::Idle;
    quint64 m_workflowGeneration = 0;
    quint64 m_scheduledGeneration = 0;
    bool m_resumeAutoUpdateAfterWorkflow = false;
    int m_wifiConnectAttempts = 0;
    QString m_workflowUsbSerial;
    QString m_workflowWifiAddress;
    QString m_scheduledSerial;
    QStringList m_scheduledArguments;
    QElapsedTimer m_workflowElapsed;
    QElapsedTimer m_workflowStepElapsed;
};

#endif // DIALOG_H
