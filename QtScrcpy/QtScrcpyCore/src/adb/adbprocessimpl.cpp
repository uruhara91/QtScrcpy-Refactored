#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
#include <QRegExp>
#else
#include <QRegularExpression>
#endif

#include "adbprocessimpl.h"
#include "qtscrcpytelemetry.h"

namespace {

[[nodiscard]] bool isScrcpyServerCommand(const QStringList &args) noexcept
{
    return args.size() >= 2 &&
           args.constFirst() == QStringLiteral("shell") &&
           args.at(1).contains(QStringLiteral("com.genymobile.scrcpy.Server"));
}

[[nodiscard]] QString normalizeScrcpyServerCommand(QString command)
{
    const bool useRoot = qsc::telemetry::environmentFlag(
        "QTSCRCPY_SERVER_ROOT", false);

    if (!useRoot) {
        const QString separator = QStringLiteral("' || ");
        const int fallbackPos = command.indexOf(separator);
        if (command.startsWith(QStringLiteral("su -c '")) && fallbackPos >= 0) {
            command = command.mid(fallbackPos + separator.size());
        } else if (command.startsWith(QStringLiteral("su -c '")) &&
                   command.endsWith(QLatin1Char('\'')) &&
                   command.size() > 8) {
            command = command.mid(7, command.size() - 8);
        }
    }

    if (qsc::telemetry::enabled()) {
        static const char *const levels[] = {
            "verbose", "debug", "info", "warn", "error"
        };
        for (const char *level : levels) {
            const QString token = QStringLiteral("log_level=") +
                                  QString::fromLatin1(level);
            if (command.contains(token)) {
                command.replace(token, QStringLiteral("log_level=debug"));
                break;
            }
        }

        qInfo() << "[Telemetry][Server] normalized-launch"
                << "uidMode=" << (useRoot ? "root" : "shell")
                << "debugLog=true";
    }

    return command;
}

} // namespace

QString AdbProcessImpl::s_adbPath;
extern QString g_adbPath;

AdbProcessImpl::AdbProcessImpl(QObject *parent) : QProcess(parent)
{
    initSignals();
}

AdbProcessImpl::~AdbProcessImpl()
{
    if (isRuning()) terminateProcess();
}

const QString &AdbProcessImpl::getAdbPath()
{
    if (s_adbPath.isEmpty()) {
        QStringList potentialPaths;
        potentialPaths << QString::fromLocal8Bit(qgetenv("QTSCRCPY_ADB_PATH"))
                       << g_adbPath;

        if (QCoreApplication::instance()) {
#ifdef Q_OS_WIN32
            potentialPaths << QCoreApplication::applicationDirPath() + "/adb.exe";
#else
            potentialPaths << QCoreApplication::applicationDirPath() + "/adb";
#endif
        } else if (qsc::telemetry::enabled()) {
            qInfo() << "[Telemetry][ADB] app-dir-candidate-skipped"
                    << "reason=no-qcoreapplication";
        }

        for (const QString &path : potentialPaths) {
            QFileInfo fileInfo(path);
            if (!path.isEmpty() && fileInfo.isFile()) {
                s_adbPath = path;
                break;
            }
        }

        if (s_adbPath.isEmpty()) {
            qWarning() << "ADB路径未找到";
        } else {
            qInfo("adb path: %s", QDir(s_adbPath).absolutePath().toUtf8().data());
        }
    }
    return s_adbPath;
}

void AdbProcessImpl::initSignals()
{
    connect(this,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        const bool intentional = m_terminationRequested;
        m_terminationRequested = false;

        if (NormalExit == exitStatus && exitCode == 0) {
            emit adbProcessImplResult(qsc::AdbProcess::AER_SUCCESS_EXEC);
        } else {
            emit adbProcessImplResult(qsc::AdbProcess::AER_ERROR_EXEC);
        }

        if (intentional) {
            if (qsc::telemetry::enabled()) {
                qInfo() << "[Telemetry][ADB] process-terminated"
                        << "exitCode=" << exitCode
                        << "exitStatus=" << static_cast<int>(exitStatus);
            }
        } else {
            qDebug() << "adb return " << exitCode
                     << "exit status " << exitStatus;
        }
    });

    connect(this, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (m_terminationRequested && error == QProcess::Crashed) {
            return;
        }

        if (error == QProcess::FailedToStart) {
            emit adbProcessImplResult(qsc::AdbProcess::AER_ERROR_MISSING_BINARY);
            qCritical() << "Could not start adb:" << errorString();
            return;
        }

        emit adbProcessImplResult(qsc::AdbProcess::AER_ERROR_START);
        qCritical().noquote()
            << QStringLiteral("adb process error (%1): %2 %3")
                   .arg(static_cast<int>(error))
                   .arg(program(), arguments().join(QLatin1Char(' ')));
    });

    connect(this, &QProcess::readyReadStandardError, this, [this]() {
        const QString tmp = QString::fromUtf8(readAllStandardError()).trimmed();
        m_errorOutput += tmp;
        qWarning() << QString("AdbProcessImpl::error:%1").arg(tmp).toStdString().data();
    });

    connect(this, &QProcess::readyReadStandardOutput, this, [this]() {
        const QString tmp = QString::fromUtf8(readAllStandardOutput()).trimmed();
        m_standardOutput += tmp;
        qInfo() << QString("AdbProcessImpl::out:%1").arg(tmp).toStdString().data();
    });

    connect(this, &QProcess::started, this, [this]() {
        emit adbProcessImplResult(qsc::AdbProcess::AER_SUCCESS_START);
    });
}

void AdbProcessImpl::execute(const QString &serial, const QStringList &args)
{
    m_terminationRequested = false;
    m_standardOutput.clear();
    m_errorOutput.clear();

    QStringList normalizedArgs = args;
    if (isScrcpyServerCommand(normalizedArgs)) {
        normalizedArgs[1] = normalizeScrcpyServerCommand(normalizedArgs.at(1));
    }

    QStringList adbArgs;
    adbArgs.reserve(normalizedArgs.size() + 2);

    if (!serial.isEmpty()) {
        adbArgs << QStringLiteral("-s") << serial;
    }
    adbArgs.append(normalizedArgs);

    if (s_adbPath.isEmpty()) getAdbPath();
    start(s_adbPath, adbArgs);
}

void AdbProcessImpl::terminateProcess()
{
    if (!isRuning()) return;
    m_terminationRequested = true;
    QProcess::kill();
}

bool AdbProcessImpl::isRuning()
{
    return state() != QProcess::NotRunning;
}

void AdbProcessImpl::setShowTouchesEnabled(const QString &serial, bool enabled)
{
    execute(serial, {
        QStringLiteral("shell"),
        QStringLiteral("settings"),
        QStringLiteral("put"),
        QStringLiteral("system"),
        QStringLiteral("show_touches"),
        enabled ? QStringLiteral("1") : QStringLiteral("0")
    });
}

QStringList AdbProcessImpl::getDevicesSerialFromStdOut()
{
    QStringList serials;
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QRegExp lineExp("\r\n|\n");
    QRegExp tExp("\t");
#else
    QRegularExpression lineExp("\r\n|\n");
    QRegularExpression tExp("\t");
#endif

    QSet<QString> seen;
    const QStringList devicesInfoList = m_standardOutput.split(lineExp);
    for (const QString &deviceInfo : devicesInfoList) {
        const QStringList deviceInfos = deviceInfo.split(tExp);
        if (deviceInfos.count() != 2 ||
            deviceInfos[1] != QStringLiteral("device")) {
            continue;
        }

        const QString serial = deviceInfos[0].trimmed();
        if (serial.isEmpty() || seen.contains(serial)) continue;
        seen.insert(serial);
        serials << serial;
    }
    return serials;
}

QString AdbProcessImpl::getDeviceIPFromStdOut()
{
    QString ip;
    const QString strIPExp = "inet addr:[\\d.]*";
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QRegExp ipRegExp(strIPExp, Qt::CaseInsensitive);
    if (ipRegExp.indexIn(m_standardOutput) != -1) {
        ip = ipRegExp.cap(0);
        ip = ip.right(ip.size() - 10);
    }
#else
    QRegularExpression ipRegExp(strIPExp,
                               QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = ipRegExp.match(m_standardOutput);
    if (match.hasMatch()) {
        ip = match.captured(0);
        ip = ip.right(ip.size() - 10);
    }
#endif
    return ip;
}

QString AdbProcessImpl::getDeviceIPByIpFromStdOut()
{
    QString ip;
    const QString strIPExp = "wlan0    inet [\\d.]*";
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QRegExp ipRegExp(strIPExp, Qt::CaseInsensitive);
    if (ipRegExp.indexIn(m_standardOutput) != -1) {
        ip = ipRegExp.cap(0);
        ip = ip.right(ip.size() - 14);
    }
#else
    QRegularExpression ipRegExp(strIPExp,
                               QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = ipRegExp.match(m_standardOutput);
    if (match.hasMatch()) {
        ip = match.captured(0);
        ip = ip.right(ip.size() - 14);
    }
#endif
    qDebug() << "get ip: " << ip;
    return ip;
}

QString AdbProcessImpl::getStdOut()
{
    return m_standardOutput;
}

QString AdbProcessImpl::getErrorOut()
{
    return m_errorOutput;
}

void AdbProcessImpl::forward(const QString &serial, quint16 localPort,
                             const QString &deviceSocketName)
{
    execute(serial, {
        QStringLiteral("forward"),
        QStringLiteral("tcp:%1").arg(localPort),
        QStringLiteral("localabstract:%1").arg(deviceSocketName)
    });
}

void AdbProcessImpl::forwardRemove(const QString &serial, quint16 localPort)
{
    execute(serial, {
        QStringLiteral("forward"),
        QStringLiteral("--remove"),
        QStringLiteral("tcp:%1").arg(localPort)
    });
}

void AdbProcessImpl::reverse(const QString &serial,
                             const QString &deviceSocketName,
                             quint16 localPort)
{
    execute(serial, {
        QStringLiteral("reverse"),
        QStringLiteral("localabstract:%1").arg(deviceSocketName),
        QStringLiteral("tcp:%1").arg(localPort)
    });
}

void AdbProcessImpl::reverseRemove(const QString &serial,
                                   const QString &deviceSocketName)
{
    execute(serial, {
        QStringLiteral("reverse"),
        QStringLiteral("--remove"),
        QStringLiteral("localabstract:%1").arg(deviceSocketName)
    });
}

void AdbProcessImpl::push(const QString &serial, const QString &local,
                          const QString &remote)
{
    execute(serial, {QStringLiteral("push"), local, remote});
}

void AdbProcessImpl::install(const QString &serial, const QString &local)
{
    execute(serial, {QStringLiteral("install"), QStringLiteral("-r"), local});
}

void AdbProcessImpl::removePath(const QString &serial, const QString &path)
{
    execute(serial, {QStringLiteral("shell"), QStringLiteral("rm"), path});
}
