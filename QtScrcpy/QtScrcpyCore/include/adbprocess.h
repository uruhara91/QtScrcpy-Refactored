#ifndef ADBPROCESS_H
#define ADBPROCESS_H

#include <QObject>
#include <memory>

class AdbProcessImpl;
namespace qsc {

class AdbProcess : public QObject
{
    Q_OBJECT

public:
    enum ADB_EXEC_RESULT
    {
        AER_SUCCESS_START,        // 启动成功
        AER_ERROR_START,          // 启动失败
        AER_SUCCESS_EXEC,         // 执行成功
        AER_ERROR_EXEC,           // 执行失败
        AER_ERROR_MISSING_BINARY, // 找不到文件
    };

    explicit AdbProcess(QObject *parent = nullptr);
    ~AdbProcess() override;

    static void setAdbPath(const QString& adbPath);
    // Resolusi path adb yang sama persis dengan yang dipakai jalur video/control
    // (Config "AdbPath" -> env QTSCRCPY_ADB_PATH -> <appdir>/adb). Modul lain
    // (mis. audio) HARUS memakai ini, bukan reimplementasi resolusinya sendiri,
    // supaya "AdbPath" custom dari Config konsisten dipakai di semua jalur.
    static const QString& getAdbPath();

    void execute(const QString &serial, const QStringList &args);
    void forward(const QString &serial, quint16 localPort, const QString &deviceSocketName);
    void forwardRemove(const QString &serial, quint16 localPort);
    void reverse(const QString &serial, const QString &deviceSocketName, quint16 localPort);
    void reverseRemove(const QString &serial, const QString &deviceSocketName);
    void push(const QString &serial, const QString &local, const QString &remote);
    void install(const QString &serial, const QString &local);
    void removePath(const QString &serial, const QString &path);
    bool isRuning();
    void setShowTouchesEnabled(const QString &serial, bool enabled);
    void kill();
    QStringList arguments();
    QStringList getDevicesSerialFromStdOut();
    QString getDeviceIPFromStdOut();
    QString getDeviceIPByIpFromStdOut();
    QString getStdOut();
    QString getErrorOut();

signals:
    void adbProcessResult(ADB_EXEC_RESULT processResult);

private:
    std::unique_ptr<AdbProcessImpl> m_adbImpl;
};

}
#endif // ADBPROCESS_H
