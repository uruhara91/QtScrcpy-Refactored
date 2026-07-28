#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QTimerEvent>
#include <array>
#include <algorithm>
#include <memory>

#include "server.h"
#include "qtscrcpytelemetry.h"

#define DEVICE_NAME_FIELD_LENGTH 64
#define SOCKET_NAME_PREFIX "scrcpy"
#define MAX_CONNECT_COUNT 100
#define MAX_RESTART_COUNT 1
#define CONNECT_RETRY_INTERVAL_MS 100
#define CONNECT_PROBE_TIMEOUT_MS 300
#define CONNECT_TOTAL_TIMEOUT_MS 10000

static quint32 bufferRead32be(const quint8 *buf)
{
    if (!buf) return 0;
    return (static_cast<quint32>(buf[0]) << 24) |
           (static_cast<quint32>(buf[1]) << 16) |
           (static_cast<quint32>(buf[2]) << 8) |
           static_cast<quint32>(buf[3]);
}

static QString shellQuote(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("'\"'\"'"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

namespace {

// Non-blocking pengganti QAbstractSocket::waitForConnected()/waitForReadyRead().
//
// waitForConnected()/waitForReadyRead() memblokir thread pemanggil di level
// socket TANPA memompa event loop Qt. Server selalu hidup di GUI thread, jadi
// setiap pemanggilan itu bikin seluruh UI (repaint, input, device lain yang
// sudah connect) macet sampai socket ready atau timeout (audit §3.5).
//
// Helper ini menunggu lewat QEventLoop lokal: signal socket (connected/
// readyRead/errorOccurred/disconnected) atau timer guard men-quit loop.
// QEventLoop::exec() tetap memompa event loop Qt selama menunggu, jadi GUI
// tidak freeze. Perilaku dari sudut pandang caller tetap sama seperti
// waitFor*() asli: synchronous, return bool, sampai socket ready/timeout.
//
// QPointer di sini WAJIB, bukan sekadar jaga-jaga: karena event loop lokal
// ini memompa event lain, ada kemungkinan (walau jarang) sesuatu yang lain
// menghapus socket ini di tengah tunggu (mis. Server::stop() ke-trigger dari
// aksi UI yang kebetulan diproses loop ini). waitForReadyRead() versi asli
// tidak punya risiko ini sama sekali karena tidak ada kode lain yang bisa
// jalan selagi ia blocking.
bool waitForConnectedNonBlocking(QAbstractSocket *socket, int timeoutMs)
{
    if (!socket) return false;
    if (socket->state() == QAbstractSocket::ConnectedState) return true;

    QPointer<QAbstractSocket> guarded(socket);
    QEventLoop loop;
    QTimer guardTimer;
    guardTimer.setSingleShot(true);
    QObject::connect(socket, &QAbstractSocket::connected, &loop, &QEventLoop::quit);
    QObject::connect(socket, &QAbstractSocket::errorOccurred, &loop, &QEventLoop::quit);
    QObject::connect(&guardTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    guardTimer.start(timeoutMs);
    loop.exec();

    if (!guarded) return false; // socket dihapus selagi menunggu
    return guarded->state() == QAbstractSocket::ConnectedState;
}

bool waitForReadyReadNonBlocking(QAbstractSocket *socket, int timeoutMs)
{
    if (!socket) return false;
    if (socket->bytesAvailable() > 0) return true;

    QPointer<QAbstractSocket> guarded(socket);
    QEventLoop loop;
    QTimer guardTimer;
    guardTimer.setSingleShot(true);
    QObject::connect(socket, &QAbstractSocket::readyRead, &loop, &QEventLoop::quit);
    QObject::connect(socket, &QAbstractSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(&guardTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    guardTimer.start(timeoutMs);
    loop.exec();

    if (!guarded) return false; // socket dihapus selagi menunggu
    return guarded->bytesAvailable() > 0;
}

} // namespace

Server::Server(QObject *parent) : QObject(parent)
{
    connect(&m_workProcess, &qsc::AdbProcess::adbProcessResult, this, &Server::onWorkProcessResult);
    connect(&m_serverProcess, &qsc::AdbProcess::adbProcessResult, this, &Server::onWorkProcessResult);

    connect(&m_serverSocket, &QTcpServer::newConnection, this, [this]() {
        // readInfo() di bawah memompa nested QEventLoop (lihat
        // waitForReadyReadNonBlocking()) selagi menunggu handshake byte dari
        // device. Event lain bisa ikut diproses selama loop lokal itu jalan,
        // termasuk klik "disconnect" di UI yang berujung ke
        // DeviceManage::disconnectDevice() -> delete device.data() SINKRON,
        // yang lewat unique_ptr<Server> milik Device ikut men-destroy `this`
        // di tengah stack call ini sendiri. readInfo() sudah guard dirinya
        // sendiri lewat selfGuard dan pulang aman (return false) begitu itu
        // terjadi, tapi TANPA re-check di sini, kode di bawah bakal lanjut
        // menyentuh `this` yang sudah freed -> use-after-free (mis. crash di
        // dalam QProcess::state() lewat stop() -> m_workProcess.isRuning()).
        // emit serverStarted(...) juga bisa memicu destruksi `this` lewat
        // receiver-nya, jadi guard dicek lagi setelahnya sebelum
        // stopAcceptTimeoutTimer().
        QPointer<Server> selfGuard(this);

        QTcpSocket *tmp = m_serverSocket.nextPendingConnection();
        if (dynamic_cast<VideoSocket *>(tmp)) {
            m_videoSocket = dynamic_cast<VideoSocket *>(tmp);
            const bool ok = m_videoSocket->isValid() && readInfo(m_videoSocket, m_deviceName);
            if (!selfGuard) {
                qInfo("newConnection(video) aborted: server destroyed while waiting");
                return;
            }
            if (!ok) {
                stop();
                emit serverStarted(false);
            }
        } else {
            m_controlSocket = tmp;
            if (m_controlSocket && m_controlSocket->isValid()) {
                // we don't need the server socket anymore
                // just m_videoSocket is ok
                m_serverSocket.close();
                // we don't need the adb tunnel anymore
                disableTunnelReverse();
                m_tunnelEnabled = false;
                // Video size is not known yet at handshake time (scrcpy-server
                // >= 4.0 sends it later, embedded in the video stream). Emit
                // an invalid QSize(); listen to Demuxer::sessionInfo() instead.
                emit serverStarted(true, m_deviceName, QSize());
            } else {
                stop();
                emit serverStarted(false);
            }
            if (!selfGuard) {
                qInfo("newConnection(control) aborted: server destroyed by serverStarted() receiver");
                return;
            }
            stopAcceptTimeoutTimer();
        }
    });
}

Server::~Server()
{
    // Device normally calls stop() before destruction, but keep destruction
    // deterministic for failed/partial startup paths as well. Do not issue new
    // adb cleanup commands here: child processes are already being destroyed.
    stopAcceptTimeoutTimer();
    stopConnectTimeoutTimer();
    m_serverSocket.close();
    cleanupOwnedSockets();
}

void Server::cleanupOwnedSockets()
{
    if (m_controlSocket) {
        m_controlSocket->abort();
        delete m_controlSocket.data();
        m_controlSocket = nullptr;
    }
    if (m_videoSocket) {
        m_videoSocket->quitNotify();
        m_videoSocket->abort();
        delete m_videoSocket.data();
        m_videoSocket = nullptr;
    }
}

bool Server::pushServer()
{
    if (m_workProcess.isRuning()) {
        m_workProcess.kill();
    }
    m_workProcess.push(m_params.serial, m_params.serverLocalPath, m_params.serverRemotePath);
    return true;
}

bool Server::enableTunnelReverse()
{
    if (m_workProcess.isRuning()) {
        m_workProcess.kill();
    }
    m_workProcess.reverse(m_params.serial, QString(SOCKET_NAME_PREFIX "_%1").arg(m_params.scid, 8, 16, QChar('0')), m_params.localPort);
    return true;
}

bool Server::disableTunnelReverse()
{
    auto *adb = new qsc::AdbProcess(this);
    connect(adb, &qsc::AdbProcess::adbProcessResult, adb,
            [adb](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        if (processResult != qsc::AdbProcess::AER_SUCCESS_START) {
            adb->deleteLater();
        }
    });
    adb->reverseRemove(
        m_params.serial,
        QString(SOCKET_NAME_PREFIX "_%1")
            .arg(m_params.scid, 8, 16, QChar('0')));
    return true;
}

bool Server::enableTunnelForward()
{
    if (m_workProcess.isRuning()) {
        m_workProcess.kill();
    }
    m_workProcess.forward(m_params.serial, m_params.localPort, QString(SOCKET_NAME_PREFIX "_%1").arg(m_params.scid, 8, 16, QChar('0')));
    return true;
}
bool Server::disableTunnelForward()
{
    auto *adb = new qsc::AdbProcess(this);
    connect(adb, &qsc::AdbProcess::adbProcessResult, adb,
            [adb](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        if (processResult != qsc::AdbProcess::AER_SUCCESS_START) {
            adb->deleteLater();
        }
    });
    adb->forwardRemove(m_params.serial, m_params.localPort);
    return true;
}

bool Server::execute()
{
    if (m_serverProcess.isRuning()) {
        m_serverProcess.kill();
    }
    QStringList args;
    // args << "shell";
    // shellQuote() tiap field string bebas (bukan cuma seluruh blok su -c)
    // supaya value yang mengandung spasi/karakter shell tidak memecah
    // argumen atau ditafsirkan sebagai sintaks shell oleh remote shell di
    // device -- berlaku utk root maupun non-root, karena keduanya sama-sama
    // dikirim sebagai satu string ke `adb shell` (audit §4.7).
    args << QString("CLASSPATH=%1").arg(shellQuote(m_params.serverRemotePath));
    args << "app_process";

#ifdef SERVER_DEBUGGER
#define SERVER_DEBUGGER_PORT "5005"

    args <<
#ifdef SERVER_DEBUGGER_METHOD_NEW
        /* Android 9 and above */
        "-XjdwpProvider:internal -XjdwpOptions:transport=dt_socket,suspend=y,server=y,address="
#else
        /* Android 8 and below */
        "-agentlib:jdwp=transport=dt_socket,suspend=y,server=y,address="
#endif
        SERVER_DEBUGGER_PORT,
#endif

        args << "/"; // unused;
    args << "com.genymobile.scrcpy.Server";
    args << shellQuote(m_params.serverVersion);

    args << QString("video_bit_rate=%1").arg(QString::number(m_params.bitRate));
    const QString serverLogLevel = qsc::telemetry::enabled()
        ? QStringLiteral("debug")
        : m_params.logLevel;
    if (!serverLogLevel.isEmpty()) {
        args << QString("log_level=%1").arg(shellQuote(serverLogLevel));
    }
    if (m_params.maxSize > 0) {
        args << QString("max_size=%1").arg(QString::number(m_params.maxSize));
    }
    if (m_params.maxFps > 0) {
        args << QString("max_fps=%1").arg(QString::number(m_params.maxFps));
    }

    // capture_orientation=@90
    // 有@表示锁定，没@不锁定
    // 有值表示指定方向，没值表示原始方向
    if (1 == m_params.captureOrientationLock) {
        args << QString("capture_orientation=@%1").arg(m_params.captureOrientation);
    } else if (2 == m_params.captureOrientationLock) {
        args << QString("capture_orientation=@");
    } else {
        args << QString("capture_orientation=%1").arg(m_params.captureOrientation);
    }
    if (m_tunnelForward) {
        args << QString("tunnel_forward=true");
    }
    if (!m_params.crop.isEmpty()) {
        args << QString("crop=%1").arg(shellQuote(m_params.crop));
    }
    if (!m_params.control) {
        args << QString("control=false");
    }
    // 默认是0，不需要设置
    // args << "display_id=0";
    // 默认是false，不需要设置
    // args << "show_touches=false";
    if (m_params.stayAwake) {
        args << QString("stay_awake=true");
    }
    // code option
    // https://github.com/Genymobile/scrcpy/commit/080a4ee3654a9b7e96c8ffe37474b5c21c02852a
    // <https://d.android.com/reference/android/media/MediaFormat>
    if (!m_params.codecOptions.isEmpty()) {
        args << QString("video_codec_options=%1").arg(shellQuote(m_params.codecOptions));
    }
    if (!m_params.codecName.isEmpty()) {
        args << QString("video_encoder=%1").arg(shellQuote(m_params.codecName));
    }
    args << "audio=false";
    // 服务端默认-1，可不传
    if (-1 != m_params.scid) {
        args << QString("scid=%1").arg(m_params.scid, 8, 16, QChar('0'));
    }

    // 默认是false，不需要设置
    // args << "power_off_on_close=false";

    // 下面的参数都用服务端默认值即可，尽量减少参数传递，传参太长导致三星手机报错：stack corruption detected (-fstack-protector)
    /*
    args << "clipboard_autosync=true";    
    args << "downsize_on_error=true";
    args << "cleanup=true";
    args << "power_on=true";
    
    args << "send_device_meta=true";
    args << "send_frame_meta=true";
    args << "send_dummy_byte=true";
    args << "raw_video_stream=false";
    */

#ifdef SERVER_DEBUGGER
    qInfo("Server debugger waiting for a client on device port " SERVER_DEBUGGER_PORT "...");
    // From the computer, run
    //     adb forward tcp:5005 tcp:5005
    // Then, from Android Studio: Run > Debug > Edit configurations...
    // On the left, click on '+', "Remote", with:
    //     Host: localhost
    //     Port: 5005
    // Then click on "Debug"
#endif

    // adb -s P7C0218510000537 shell CLASSPATH=/data/local/tmp/scrcpy-server app_process / com.genymobile.scrcpy.Server 0 8000000 false
    // mark: crop input format: "width:height:x:y" or "" for no crop, for example: "100:200:0:0"
    // 这条adb命令是阻塞运行的，m_serverProcess进程不会退出了
    QString cmdObj = args.join(" ");

    // QTSCRCPY_SERVER_ROOT env var remains available as an emergency
    // override (e.g. for debugging) even with the UI checkbox now driving
    // this normally via ServerParams::useRoot - if set, it takes
    // precedence over whatever the UI has configured.
    const bool useRoot = qEnvironmentVariableIsSet("QTSCRCPY_SERVER_ROOT")
        ? qsc::telemetry::environmentFlag("QTSCRCPY_SERVER_ROOT", false)
        : m_params.useRoot;

    // When running as root we also boost the server's scheduling priority:
    // CPU nice -9 (higher priority, requires root to go below the default
    // -8 negative floor on most Android kernels) and best-effort I/O class
    // at priority 0. `$$` is the PID of this very shell (the one that will
    // `exec` into app_process below); exec replaces the process image
    // in-place without changing the PID, so renice/ionice applied here
    // stick to the eventual scrcpy-server process itself. Adjustments run
    // silently (`>/dev/null 2>&1`) and never abort the launch on failure -
    // e.g. on kernels without CFQ/BFQ ionice support - since capture must
    // still proceed without the boost.
    static const int kServerNiceLevel = -9;
    static const QString kServerIoClass = QStringLiteral("2"); // best-effort
    static const QString kServerIoPriority = QStringLiteral("0");
    const QString reniceCmd = useRoot
        ? QStringLiteral("renice -n %1 -p $$ >/dev/null 2>&1; ionice -c %2 -n %3 -p $$ >/dev/null 2>&1; ")
              .arg(kServerNiceLevel)
              .arg(kServerIoClass, kServerIoPriority)
        : QString();
    // cmdObj's first token is "CLASSPATH=<path>", relying on the shell's
    // VAR=val prefix form to scope the env var to the command that
    // follows. That prefix form only applies to ordinary commands, not to
    // the `exec` builtin - `exec CLASSPATH=x app_process ...` makes the
    // shell try to exec a literal file named "CLASSPATH=x" instead of
    // setting the variable. So when wrapping with exec, the assignment
    // must be split out into its own `export` statement first.
    QString serverCommand = cmdObj;
    if (useRoot) {
        const QString classpathAssign = args.isEmpty() ? QString() : args.first();
        const QString execArgs = args.size() > 1
            ? QStringList(args.mid(1)).join(" ")
            : QString();
        const QString innerCmd = reniceCmd
            + QStringLiteral("export %1; exec %2").arg(classpathAssign, execArgs);
        serverCommand = QStringLiteral("su -c %1").arg(shellQuote(innerCmd));
    }

    if (qsc::telemetry::enabled()) {
        qInfo() << "[Telemetry][Server] launch"
                << "uidMode=" << (useRoot ? "root" : "shell")
                << "renice=" << (useRoot ? "nice-9/ionice-be0" : "none")
                << "version=" << m_params.serverVersion
                << "maxSize=" << m_params.maxSize
                << "maxFps=" << m_params.maxFps
                << "bitRate=" << m_params.bitRate
                << "codecOptions=" << m_params.codecOptions
                << "encoder=" << (m_params.codecName.isEmpty()
                                       ? QStringLiteral("auto")
                                       : m_params.codecName)
                << "tunnel=" << (m_tunnelForward ? "forward" : "reverse")
                << "logLevel=" << serverLogLevel
                << "thread=" << qsc::telemetry::threadId();
    }

    QStringList finalArgs;
    finalArgs << QStringLiteral("shell") << serverCommand;

    m_serverProcess.execute(m_params.serial, finalArgs);
    
    return true;
}

bool Server::start(Server::ServerParams params)
{
    m_params = params;
    m_serverStartStep = SSS_PUSH;
    return startServerByStep();
}

bool Server::connectTo()
{
    if (SSS_RUNNING != m_serverStartStep) {
        qWarning("server not run");
        return false;
    }

    if (!m_tunnelForward && !m_videoSocket) {
        startAcceptTimeoutTimer();
        return true;
    }

    startConnectTimeoutTimer();
    return true;
}

bool Server::isReverse()
{
    return !m_tunnelForward;
}

Server::ServerParams Server::getParams()
{
    return m_params;
}

void Server::timerEvent(QTimerEvent *event)
{
    if (event && m_acceptTimeoutTimer == event->timerId()) {
        stopAcceptTimeoutTimer();
        emit serverStarted(false);
    } else if (event && m_connectTimeoutTimer == event->timerId()) {
        onConnectTimer();
    }
}

VideoSocket* Server::removeVideoSocket()
{
    VideoSocket* socket = m_videoSocket;
    m_videoSocket = Q_NULLPTR;
    return socket;
}

QTcpSocket *Server::getControlSocket()
{
    return m_controlSocket;
}

void Server::stop()
{
    // Set the state first so process termination callbacks cannot advance a
    // partially stopped startup state machine.
    m_serverStartStep = SSS_NULL;
    stopAcceptTimeoutTimer();
    stopConnectTimeoutTimer();

    m_serverSocket.close();
    cleanupOwnedSockets();

    // Both processes may be active during startup fallback/retry.
    if (m_workProcess.isRuning()) m_workProcess.kill();
    if (m_serverProcess.isRuning()) m_serverProcess.kill();

    if (m_tunnelEnabled) {
        if (m_tunnelForward) {
            disableTunnelForward();
        } else {
            disableTunnelReverse();
        }
    }
    m_tunnelForward = false;
    m_tunnelEnabled = false;
    m_connectCount = 0;
}

bool Server::startServerByStep()
{
    bool stepSuccess = false;
    // push, enable tunnel et start the server
    if (SSS_NULL != m_serverStartStep) {
        switch (m_serverStartStep) {
        case SSS_PUSH:
            stepSuccess = pushServer();
            break;
        case SSS_ENABLE_TUNNEL_REVERSE:
            stepSuccess = enableTunnelReverse();
            break;
        case SSS_ENABLE_TUNNEL_FORWARD:
            stepSuccess = enableTunnelForward();
            break;
        case SSS_EXECUTE_SERVER:
            // server will connect to our server socket
            stepSuccess = execute();
            break;
        default:
            break;
        }
    }

    if (!stepSuccess) {
        emit serverStarted(false);
    }
    return stepSuccess;
}

bool Server::readInfo(VideoSocket *videoSocket, QString &deviceName)
{
    if (!videoSocket) return false;

    // Since scrcpy-server 4.0, only the device name (64 bytes) followed by
    // the 4-byte video codec id are sent right after the connection is
    // established. The video width/height are NOT sent here anymore: they
    // now arrive later, embedded in the video stream itself as a "session
    // packet" (see Demuxer::processNetworkPacket / Demuxer::sessionInfo).
    constexpr qint64 infoSize = DEVICE_NAME_FIELD_LENGTH + 4;
    constexpr qint64 timeoutMs = 3000;
    std::array<quint8, static_cast<std::size_t>(infoSize)> buf{};

    // Lihat komentar di waitForReadyReadNonBlocking(): loop di bawah ini
    // memompa event loop Qt selagi menunggu, jadi kode lain (mis.
    // Server::stop() lewat aksi disconnect di UI) berpotensi menghapus
    // `this` atau `videoSocket` di tengah tunggu. Guard ini dicek sebelum
    // menyentuh keduanya lagi setiap kali sehabis menunggu.
    QPointer<Server> selfGuard(this);
    QPointer<VideoSocket> socketGuard(videoSocket);

    QElapsedTimer timer;
    timer.start();
    qint64 totalRead = 0;
    while (totalRead < infoSize) {
        if (videoSocket->bytesAvailable() <= 0) {
            const qint64 remaining = timeoutMs - timer.elapsed();
            bool gotReady = false;
            if (remaining > 0) {
                gotReady = waitForReadyReadNonBlocking(
                    videoSocket, static_cast<int>(std::min<qint64>(300, remaining)));
            }
            if (!selfGuard || !socketGuard) {
                qInfo("readInfo aborted: server or socket destroyed while waiting");
                return false;
            }
            if (!gotReady) {
                if (timer.elapsed() >= timeoutMs ||
                    videoSocket->state() != QAbstractSocket::ConnectedState) {
                    qInfo("readInfo timeout or disconnect");
                    return false;
                }
                continue;
            }
        }

        const qint64 chunk = videoSocket->read(
            reinterpret_cast<char *>(buf.data() + totalRead),
            infoSize - totalRead);
        if (chunk < 0) {
            qInfo("Could not retrieve device information");
            return false;
        }
        if (chunk == 0) continue;
        totalRead += chunk;
    }
    qDebug() << "readInfo wait time:" << timer.elapsed();

    if (!selfGuard) return false;

    buf[DEVICE_NAME_FIELD_LENGTH - 1] = '\0';
    deviceName = QString::fromUtf8(
        reinterpret_cast<const char *>(buf.data()));

    // The 4 bytes after the device name are the video codec id (e.g. "h264").
    // We don't need to interpret it here: the demuxer already validates/uses
    // it when it opens the decoder.
    const quint32 codecId = bufferRead32be(&buf[DEVICE_NAME_FIELD_LENGTH]);
    Q_UNUSED(codecId);
    return true;
}

void Server::startAcceptTimeoutTimer()
{
    stopAcceptTimeoutTimer();
    m_acceptTimeoutTimer = startTimer(1000);
}

void Server::stopAcceptTimeoutTimer()
{
    if (m_acceptTimeoutTimer) {
        killTimer(m_acceptTimeoutTimer);
        m_acceptTimeoutTimer = 0;
    }
}

void Server::startConnectTimeoutTimer()
{
    stopConnectTimeoutTimer();
    m_forwardConnectElapsed.start();
    m_connectTimeoutTimer = startTimer(
        CONNECT_RETRY_INTERVAL_MS, Qt::PreciseTimer);
    // Do not add a full retry interval before the first probe.
    QTimer::singleShot(0, this, [this]() {
        if (m_connectTimeoutTimer && m_tunnelForward &&
            m_serverStartStep == SSS_RUNNING) {
            onConnectTimer();
        }
    });
}

void Server::stopConnectTimeoutTimer()
{
    if (m_connectTimeoutTimer) {
        killTimer(m_connectTimeoutTimer);
        m_connectTimeoutTimer = 0;
    }
    m_connectCount = 0;
    m_forwardConnectElapsed.invalidate();
}

void Server::onConnectTimer()
{
    if (!m_tunnelForward || m_serverStartStep != SSS_RUNNING) {
        stopConnectTimeoutTimer();
        return;
    }

    const quint32 attempt = ++m_connectCount;
    const qint64 elapsedMs = m_forwardConnectElapsed.isValid()
        ? m_forwardConnectElapsed.elapsed()
        : 0;

    auto failAttempt = [this, attempt, elapsedMs](const char *stage) {
        if (qsc::telemetry::enabled() &&
            (attempt == 1 || attempt % 10 == 0)) {
            qInfo() << "[Telemetry][Server] forward-probe"
                    << "attempt=" << attempt
                    << "elapsedMs=" << elapsedMs
                    << "stage=" << stage;
        }

        const bool exhausted = attempt >= MAX_CONNECT_COUNT ||
                               elapsedMs >= CONNECT_TOTAL_TIMEOUT_MS;
        if (!exhausted) return;

        qWarning() << "forward tunnel handshake timed out"
                   << "attempts=" << attempt
                   << "elapsedMs=" << elapsedMs
                   << "stage=" << stage;
        stopConnectTimeoutTimer();
        stop();
        if (MAX_RESTART_COUNT > m_restartCount++) {
            qWarning("restart server auto");
            start(m_params);
        } else {
            m_restartCount = 0;
            emit serverStarted(false);
        }
    };

    // Server (this) bisa dihapus di tengah tunggu non-blocking di bawah
    // (mis. user klik disconnect selagi event loop lokal ini kebetulan
    // memproses event UI itu) — lihat komentar di waitForConnectedNonBlocking().
    // videoSocket/controlSocket sendiri TIDAK perlu guard terpisah: keduanya
    // unique_ptr lokal yang belum di-assign ke m_videoSocket/m_controlSocket
    // atau di-expose lewat signal apa pun, jadi tidak ada kode lain yang bisa
    // menghapusnya di tengah jalan selain scope function ini sendiri.
    QPointer<Server> selfGuard(this);
    auto safeFail = [&selfGuard, &failAttempt](const char *stage) {
        if (selfGuard) failAttempt(stage);
    };

    // Match upstream scrcpy's forward handshake: connect only the first
    // (video) socket and wait for the server dummy byte. Opening the control
    // socket before this probe succeeds creates needless stale connections
    // while app_process is still starting on slower Android versions.
    auto videoSocket = std::make_unique<VideoSocket>();
    videoSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    videoSocket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    videoSocket->connectToHost(QHostAddress::LocalHost, m_params.localPort);

    const bool videoConnected = waitForConnectedNonBlocking(videoSocket.get(), CONNECT_PROBE_TIMEOUT_MS);
    if (!selfGuard) return;
    if (!videoConnected) {
        safeFail("video-connect");
        return;
    }

    bool gotDummyByte = videoSocket->bytesAvailable() >= 1;
    if (!gotDummyByte) {
        gotDummyByte = waitForReadyReadNonBlocking(videoSocket.get(), CONNECT_PROBE_TIMEOUT_MS);
        if (!selfGuard) return;
    }
    if (!gotDummyByte) {
        safeFail("dummy-byte");
        return;
    }

    const QByteArray dummy = videoSocket->read(1);
    if (dummy.size() != 1) {
        safeFail("dummy-byte");
        return;
    }

    // The Android server accepts sockets in video -> control order. Once the
    // video dummy byte arrives, it is already blocked waiting for control.
    auto controlSocket = std::make_unique<QTcpSocket>();
    controlSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    controlSocket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    controlSocket->connectToHost(QHostAddress::LocalHost, m_params.localPort);
    const bool controlConnected = waitForConnectedNonBlocking(controlSocket.get(), 1000);
    if (!selfGuard) return;
    if (!controlConnected) {
        safeFail("control-connect");
        return;
    }

    QString deviceName;
    if (!readInfo(videoSocket.get(), deviceName)) {
        safeFail("device-info");
        return;
    }
    if (!selfGuard) return;

    const qint64 connectedMs = m_forwardConnectElapsed.isValid()
        ? m_forwardConnectElapsed.elapsed()
        : elapsedMs;
    stopConnectTimeoutTimer();
    m_videoSocket = videoSocket.release();
    m_controlSocket = controlSocket.release();

    // The established sockets remain valid after removing the adb rule.
    disableTunnelForward();
    m_tunnelEnabled = false;
    m_restartCount = 0;

    if (qsc::telemetry::enabled()) {
        qInfo() << "[Telemetry][Server] forward-connected"
                << "attempts=" << attempt
                << "elapsedMs=" << connectedMs;
    }
    // Video size is not known yet at handshake time (scrcpy-server >= 4.0
    // sends it later, embedded in the video stream). Emit an invalid
    // QSize(); listen to Demuxer::sessionInfo() instead.
    emit serverStarted(true, deviceName, QSize());
}

void Server::onWorkProcessResult(qsc::AdbProcess::ADB_EXEC_RESULT processResult)
{
    if (sender() == &m_workProcess) {
        if (SSS_NULL != m_serverStartStep) {
            switch (m_serverStartStep) {
            case SSS_PUSH:
                if (qsc::AdbProcess::AER_SUCCESS_EXEC == processResult) {
                    if (m_params.useReverse) {
                        m_serverStartStep = SSS_ENABLE_TUNNEL_REVERSE;
                    } else {
                        m_tunnelForward = true;
                        m_serverStartStep = SSS_ENABLE_TUNNEL_FORWARD;
                    }
                    startServerByStep();
                } else if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
                    qCritical("adb push failed");
                    m_serverStartStep = SSS_NULL;
                    emit serverStarted(false);
                }
                break;
            case SSS_ENABLE_TUNNEL_REVERSE:
                if (qsc::AdbProcess::AER_SUCCESS_EXEC == processResult) {
                    // At the application level, the device part is "the server" because it
                    // serves video stream and control. However, at the network level, the
                    // client listens and the server connects to the client. That way, the
                    // client can listen before starting the server app, so there is no need to
                    // try to connect until the server socket is listening on the device.
                    m_serverSocket.setMaxPendingConnections(2);
                    m_serverSocket.resetSocketSequence(); // audit §4.5
                    if (!m_serverSocket.listen(QHostAddress::LocalHost, m_params.localPort)) {
                        qCritical() << QString("Could not listen on port %1").arg(m_params.localPort).toStdString().c_str();
                        m_serverStartStep = SSS_NULL;
                        disableTunnelReverse();
                        emit serverStarted(false);
                        break;
                    }

                    m_serverStartStep = SSS_EXECUTE_SERVER;
                    startServerByStep();
                } else if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
                    // 有一些设备reverse会报错more than o'ne device，adb的bug
                    // https://github.com/Genymobile/scrcpy/issues/5
                    qCritical("adb reverse failed");
                    m_tunnelForward = true;
                    m_serverStartStep = SSS_ENABLE_TUNNEL_FORWARD;
                    startServerByStep();
                }
                break;
            case SSS_ENABLE_TUNNEL_FORWARD:
                if (qsc::AdbProcess::AER_SUCCESS_EXEC == processResult) {
                    m_serverStartStep = SSS_EXECUTE_SERVER;
                    startServerByStep();
                } else if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
                    qCritical("adb forward failed");
                    m_serverStartStep = SSS_NULL;
                    emit serverStarted(false);
                }
                break;
            default:
                break;
            }
        }
    }
    if (sender() == &m_serverProcess) {
        if (SSS_EXECUTE_SERVER == m_serverStartStep) {
            if (qsc::AdbProcess::AER_SUCCESS_START == processResult) {
                m_serverStartStep = SSS_RUNNING;
                m_tunnelEnabled = true;
                connectTo();
            } else if (qsc::AdbProcess::AER_ERROR_START == processResult) {
                if (!m_tunnelForward) {
                    m_serverSocket.close();
                    disableTunnelReverse();
                } else {
                    disableTunnelForward();
                }
                qCritical("adb shell start server failed");
                m_serverStartStep = SSS_NULL;
                emit serverStarted(false);
            }
        } else if (SSS_RUNNING == m_serverStartStep) {
            m_serverStartStep = SSS_NULL;
            emit serverStoped();
        }
    }
}
