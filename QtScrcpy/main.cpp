#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#ifdef Q_OS_LINUX
#include <QIcon>
#endif
#include <QSurfaceFormat>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTranslator>
#include <QDateTime>
#include <QStandardPaths>
#include <QSysInfo>
#include <QDir>

#include "config.h"
#include "dialog.h"
#include "mousetap/mousetap.h"
#include "adbprocess.h"
#include "qtscrcpytelemetry.h"
#ifdef Q_OS_WIN
#include <mimalloc-new-delete.h>
#elif defined(Q_OS_LINUX) && defined(QSC_HAVE_MIMALLOC)
// Windows memasang override ini secara eksplisit; di Linux mimalloc dulunya
// hanya di-link tanpa header ini, jadi kemungkinan besar tidak benar-benar
// dipakai sebagai allocator untuk operator new/delete (audit §6).
// QSC_HAVE_MIMALLOC hanya didefinisikan oleh CMake kalau find_package(mimalloc)
// berhasil, jadi include ini aman (tidak memaksa dependency di mesin yang
// belum punya paket mimalloc).
#include <mimalloc-new-delete.h>
#endif

#ifndef QTSCRCPY_VERSION
#define QTSCRCPY_VERSION "0.0.0"
#endif
#ifndef QTSCRCPY_BUILD_TYPE
#define QTSCRCPY_BUILD_TYPE "unknown"
#endif

static Dialog *g_mainDlg = Q_NULLPTR;
static QtMessageHandler g_oldMessageHandler = Q_NULLPTR;
void myMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg);
void installTranslator();

static QtMsgType g_msgType = QtInfoMsg;
QtMsgType covertLogLevel(const QString &logLevel);

int main(int argc, char *argv[])
{
    QCoreApplication::setApplicationName(QStringLiteral("QtScrcpy"));
    QCoreApplication::setApplicationVersion(QStringLiteral(QTSCRCPY_VERSION));

    // QCoreApplication::applicationDirPath() is invalid before QApplication
    // exists. Resolve argv[0] directly because deployment paths are needed
    // before Config initializes and before the GUI application is constructed.
    const QString executable = argc > 0
        ? QString::fromLocal8Bit(argv[0])
        : QString();
    const QString appPath = QFileInfo(executable).absoluteDir().absolutePath();

#ifdef Q_OS_WIN32
    QString adbPath = appPath + "/adb.exe";
    if (!QFile::exists(adbPath)) {
        adbPath = "../../../QtScrcpy/QtScrcpyCore/src/third_party/adb/win/adb.exe";
    }
    qputenv("QTSCRCPY_ADB_PATH", adbPath.toLocal8Bit());

    QString serverPath = appPath + "/scrcpy-server";
    if (!QFile::exists(serverPath)) {
        serverPath = "../../../QtScrcpy/QtScrcpyCore/src/third_party/scrcpy-server";
    }
    qputenv("QTSCRCPY_SERVER_PATH", serverPath.toLocal8Bit());
    qputenv("QTSCRCPY_KEYMAP_PATH", (appPath + "/keymap").toLocal8Bit());
    qputenv("QTSCRCPY_CONFIG_PATH", (appPath + "/config").toLocal8Bit());
#endif

#ifdef Q_OS_OSX
    const QString contentsPath = appPath + "/../";
    qputenv("QTSCRCPY_ADB_PATH", (contentsPath + "MacOS/adb").toLocal8Bit());
    qputenv("QTSCRCPY_SERVER_PATH", (contentsPath + "MacOS/scrcpy-server").toLocal8Bit());
    qputenv("QTSCRCPY_KEYMAP_PATH", (contentsPath + "Resources/keymap").toLocal8Bit());
    qputenv("QTSCRCPY_CONFIG_PATH", (contentsPath + "Resources/config").toLocal8Bit());
#endif

#ifdef Q_OS_LINUX
    qputenv("QTSCRCPY_ADB_PATH", QByteArrayLiteral("/usr/bin/adb"));
    qputenv("QTSCRCPY_SERVER_PATH", (appPath + "/scrcpy-server").toLocal8Bit());
    qputenv("QTSCRCPY_KEYMAP_PATH", (appPath + "/keymap").toLocal8Bit());
    qputenv("QTSCRCPY_CONFIG_PATH", (appPath + "/config").toLocal8Bit());
#endif

    g_msgType = covertLogLevel(Config::getInstance().getLogLevel());

    QSurfaceFormat varFormat = QSurfaceFormat::defaultFormat();
    varFormat.setDepthBufferSize(0);
    varFormat.setStencilBufferSize(0);
    varFormat.setVersion(4, 5);
    varFormat.setProfile(QSurfaceFormat::CoreProfile);
    varFormat.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(varFormat);

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    #if (QT_VERSION >= QT_VERSION_CHECK(5,14,0))
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    #endif
#endif

    g_oldMessageHandler = qInstallMessageHandler(myMessageOutput);

    QApplication a(argc, argv);

#ifdef Q_OS_LINUX
    QIcon appIcon(":/image/tray/logo.png");
    if (!appIcon.isNull()) {
        a.setWindowIcon(appIcon);
    }
#endif

    qDebug() << "App Name:" << a.applicationName();
    qDebug() << "App Version:" << a.applicationVersion();
    if (qsc::telemetry::enabled()) {
        qInfo() << "[Telemetry][Runtime]"
                << "version=" << a.applicationVersion()
                << "buildType=" << QTSCRCPY_BUILD_TYPE
                << "qt=" << qVersion()
                << "platform=" << QGuiApplication::platformName()
                << "arch=" << QSysInfo::currentCpuArchitecture();
    }

    installTranslator();

#if defined(Q_OS_WIN32) || defined(Q_OS_OSX)
    MouseTap::getInstance()->initMouseEventTap();
#endif

    QFile file(":/qss/psblack.css");
    if (file.open(QFile::ReadOnly)) {
        QString qss = QLatin1String(file.readAll());
        if (qss.length() > 30) {
            QString paletteColor = qss.mid(20, 7);
            qApp->setPalette(QPalette(QColor(paletteColor)));
            qApp->setStyleSheet(qss);
        }
        file.close();
    }

    qsc::AdbProcess::setAdbPath(Config::getInstance().getAdbPath());

    g_mainDlg = new Dialog {};
    g_mainDlg->show();

    int ret = a.exec();

    delete g_mainDlg;
    g_mainDlg = Q_NULLPTR;

#if defined(Q_OS_WIN32) || defined(Q_OS_OSX)
    MouseTap::getInstance()->quitMouseEventTap();
#endif
    return ret;
}

void installTranslator()
{
    static QTranslator translator;

    // Hanya res/i18n/en_US.qm yang di-bundle (lihat res/res.qrc) — dulu ada
    // pengecekan Config "Language" == "zh_CN"/"ja_JP" di sini yang meresolve
    // ke QLocale::Chinese/Japanese, tapi switch di bawah tidak pernah punya
    // case untuk itu (cuma English+default), jadi setting itu tidak pernah
    // benar-benar berefek — dead code yang menyesatkan (audit §3.6). Untuk
    // menambah bahasa lain nanti: taruh file .qm baru di res/i18n/, daftarkan
    // di res/res.qrc, lalu load berdasar Config::getLanguage() di sini.
    const QString languagePath = QStringLiteral(":/i18n/en_US.qm");

    if (!translator.load(languagePath)) {
        qWarning() << "Failed to load translation file:" << languagePath;
    } else {
        qApp->installTranslator(&translator);
    }
}

QtMsgType covertLogLevel(const QString &logLevel)
{
    if ("verbose" == logLevel || "debug" == logLevel) return QtDebugMsg;
    if ("info" == logLevel) return QtInfoMsg;
    if ("warn" == logLevel) return QtWarningMsg;
    if ("error" == logLevel) return QtCriticalMsg;

#ifdef QT_NO_DEBUG
    return QtInfoMsg;
#else
    return QtDebugMsg;
#endif
}

void myMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    QString outputMsg;

#ifdef ENABLE_DETAILED_LOGS
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");

    if (context.file && context.line > 0) {
        QString fileName = QString::fromUtf8(context.file);
        int lastSlash = fileName.lastIndexOf('/');
        if (lastSlash >= 0) fileName = fileName.mid(lastSlash + 1);
        lastSlash = fileName.lastIndexOf('\\');
        if (lastSlash >= 0) fileName = fileName.mid(lastSlash + 1);

        outputMsg = QString("[ %1 %2: %3 ] %4").arg(timestamp).arg(fileName).arg(context.line).arg(msg);
    } else {
        outputMsg = QString("[%1] %2").arg(timestamp).arg(msg);
    }

    QString prefix;
    switch (type) {
        case QtDebugMsg: prefix = "[debug] "; break;
        case QtInfoMsg: prefix = "[info] "; break;
        case QtWarningMsg: prefix = "[warning] "; break;
        case QtCriticalMsg: prefix = "[critical] "; break;
        case QtFatalMsg: prefix = "[fatal] "; break;
    }
    outputMsg.prepend(prefix);
    fprintf(stderr, "%s\n", outputMsg.toUtf8().constData());
#else
    outputMsg = msg;
    if (g_oldMessageHandler) {
        g_oldMessageHandler(type, context, outputMsg);
    }
#endif

    auto getLogRank = [](QtMsgType t) -> int {
        switch (t) {
            case QtDebugMsg: return 0;
            case QtInfoMsg: return 1;
            case QtWarningMsg: return 2;
            case QtCriticalMsg: return 3;
            case QtFatalMsg: return 4;
            default: return 0;
        }
    };

    if (getLogRank(g_msgType) <= getLogRank(type)) {
        if (g_mainDlg && g_mainDlg->isVisible() && !g_mainDlg->filterLog(outputMsg)) {
            g_mainDlg->outLog(outputMsg);
        }
    }

    if (QtFatalMsg == type) {
        // abort();
    }
}
