#ifndef DIALOG_H
#define DIALOG_H

#include <QComboBox>
#include <QWidget>
#include <QPointer>
#include <QMessageBox>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QListWidget>
#include <QTimer>

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
    explicit Dialog(QWidget *parent = 0);
    ~Dialog();

    void outLog(const QString &log, bool newLine = true);
    bool filterLog(const QString &log);
    void getIPbyIp();

private slots:
    void onDeviceConnected(bool success, const QString& serial, const QString& deviceName, const QSize& size);
    void onDeviceDisconnected(QString serial);

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
    // Keep the implementation already defined in dialog.cpp, but do not expose
    // it as a Qt slot because Qt 6 has no matching QString signal overload.
    void on_serialBox_currentIndexChanged(const QString &arg1);

    bool checkAdbRun();
    void initUI();
    void updateBootConfig(bool toView = true);
    void execAdbCmd();
    void delayMs(int ms);
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

protected:
    void closeEvent(QCloseEvent *event);

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
            outLog(QStringLiteral("Audio codec and output sink initialized."), true);
        });
    QMetaObject::Connection m_audioErrorConnection = QObject::connect(
        &m_audioOutput,
        &AudioOutput::errorOccurred,
        this,
        [this](const QString &message) {
            outLog(message, true);
        });
    QTimer m_autoUpdatetimer;
};

#endif // DIALOG_H
