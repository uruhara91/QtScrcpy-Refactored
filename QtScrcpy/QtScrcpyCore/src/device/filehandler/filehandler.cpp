#include "filehandler.h"
#include <QTimer>

FileHandler::FileHandler(QObject *parent) : QObject(parent)
{
}

FileHandler::~FileHandler() {}

void FileHandler::onPushFileRequest(const QString &serial,
                                    const QString &file,
                                    const QString &devicePath)
{
    auto *adb = new qsc::AdbProcess(this);
    connect(adb, &qsc::AdbProcess::adbProcessResult, adb,
            [this, adb](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        onAdbProcessResult(adb, false, processResult);
    });
    // Safety net kalau adb push hang tanpa pernah sampai state akhir (mis.
    // adb daemon macet): tanpa ini, `adb` numpuk sebagai child FileHandler
    // selama app hidup kalau push di-panggil berkali-kali dalam sesi panjang
    // (audit §4.6). Timeout digenerouskan (2 menit) karena push file besar
    // memang bisa lama. ~AdbProcess() sudah terminate proses yang masih
    // jalan; adb sebagai context object membuat timer otomatis batal kalau
    // adb sudah lebih dulu dihapus lewat onAdbProcessResult().
    QTimer::singleShot(120000, adb, [adb]() { adb->deleteLater(); });

    adb->push(serial, file, devicePath);
}

void FileHandler::onInstallApkRequest(const QString &serial,
                                      const QString &apkFile)
{
    auto *adb = new qsc::AdbProcess(this);
    connect(adb, &qsc::AdbProcess::adbProcessResult, adb,
            [this, adb](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        onAdbProcessResult(adb, true, processResult);
    });
    // Sama seperti onPushFileRequest di atas (audit §4.6).
    QTimer::singleShot(120000, adb, [adb]() { adb->deleteLater(); });

    adb->install(serial, apkFile);
}

void FileHandler::onAdbProcessResult(
    qsc::AdbProcess *adb,
    bool isApk,
    qsc::AdbProcess::ADB_EXEC_RESULT processResult)
{
    switch (processResult) {
    case qsc::AdbProcess::AER_ERROR_START:
    case qsc::AdbProcess::AER_ERROR_EXEC:
    case qsc::AdbProcess::AER_ERROR_MISSING_BINARY:
        emit fileHandlerResult(FAR_ERROR_EXEC, isApk);
        adb->deleteLater();
        break;
    case qsc::AdbProcess::AER_SUCCESS_EXEC:
        emit fileHandlerResult(FAR_SUCCESS_EXEC, isApk);
        adb->deleteLater();
        break;
    default:
        break;
    }
}
