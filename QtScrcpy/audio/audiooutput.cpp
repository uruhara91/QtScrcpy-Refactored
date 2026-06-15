#include "audiooutput.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHostAddress>
#include <QIODevice>
#include <QMetaObject>
#include <QProcess>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTimer>
#include <QtGlobal>

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QAudioDevice>
#include <QAudioSink>
#include <QMediaDevices>
#else
#include <QAudioDeviceInfo>
#include <QAudioOutput>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {

constexpr quint32 CODEC_ID_OPUS = 0x6f707573U;
constexpr quint64 PACKET_FLAG_SESSION = quint64{1} << 63;
constexpr quint64 PACKET_FLAG_CONFIG = quint64{1} << 62;
constexpr quint64 PACKET_FLAG_KEY_FRAME = quint64{1} << 61;
constexpr quint64 PACKET_PTS_MASK = PACKET_FLAG_KEY_FRAME - 1;
constexpr int PACKET_HEADER_SIZE = 12;
constexpr quint32 MAX_AUDIO_PACKET_SIZE = 1U << 20;
constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNELS = 2;
constexpr int BYTES_PER_SAMPLE = 2;
constexpr int BYTES_PER_FRAME = CHANNELS * BYTES_PER_SAMPLE;
constexpr int BYTES_PER_MILLISECOND = SAMPLE_RATE * BYTES_PER_FRAME / 1000;
constexpr int CONNECT_TIMEOUT_MS = 8000;
constexpr int CONNECT_RETRY_MS = 100;
constexpr int DISCONNECT_DIAGNOSTIC_DELAY_MS = 100;

[[nodiscard]] int boundedEnvironmentValue(const char *name,
                                          int fallback,
                                          int minimum,
                                          int maximum)
{
    bool ok = false;
    const int value = qEnvironmentVariableIntValue(name, &ok);
    return ok ? qBound(minimum, value, maximum) : fallback;
}

[[nodiscard]] bool environmentEnabled(const char *name, bool fallback)
{
    if (!qEnvironmentVariableIsSet(name)) return fallback;
    return qEnvironmentVariableIntValue(name) != 0;
}

[[nodiscard]] quint32 read32be(const char *data) noexcept
{
    return (static_cast<quint32>(static_cast<unsigned char>(data[0])) << 24) |
           (static_cast<quint32>(static_cast<unsigned char>(data[1])) << 16) |
           (static_cast<quint32>(static_cast<unsigned char>(data[2])) << 8) |
           static_cast<quint32>(static_cast<unsigned char>(data[3]));
}

[[nodiscard]] quint64 read64be(const char *data) noexcept
{
    return (static_cast<quint64>(read32be(data)) << 32) |
           static_cast<quint64>(read32be(data + 4));
}

class PcmRingBuffer
{
public:
    explicit PcmRingBuffer(std::size_t capacity)
        : m_storage(std::max<std::size_t>(capacity, 1))
    {
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] bool empty() const noexcept { return m_size == 0; }

    void clear() noexcept
    {
        m_head = 0;
        m_size = 0;
    }

    void discard(std::size_t bytes) noexcept
    {
        bytes = std::min(bytes, m_size);
        m_head = (m_head + bytes) % m_storage.size();
        m_size -= bytes;
    }

    void push(const char *data, std::size_t bytes) noexcept
    {
        if (!data || bytes == 0) return;

        if (bytes >= m_storage.size()) {
            data += bytes - m_storage.size();
            bytes = m_storage.size();
            clear();
        } else if (m_size + bytes > m_storage.size()) {
            discard(m_size + bytes - m_storage.size());
        }

        const std::size_t tail = (m_head + m_size) % m_storage.size();
        const std::size_t first = std::min(bytes, m_storage.size() - tail);
        std::memcpy(m_storage.data() + tail, data, first);
        if (bytes > first) {
            std::memcpy(m_storage.data(), data + first, bytes - first);
        }
        m_size += bytes;
    }

    [[nodiscard]] std::pair<const char *, std::size_t> frontSpan() const noexcept
    {
        if (m_size == 0) return {nullptr, 0};
        return {m_storage.data() + m_head,
                std::min(m_size, m_storage.size() - m_head)};
    }

private:
    std::vector<char> m_storage;
    std::size_t m_head = 0;
    std::size_t m_size = 0;
};

} // namespace

struct ScrcpyAudioWorker::Impl
{
    enum class InputState {
        DummyByte,
        CodecId,
        PacketStream,
    };

    explicit Impl(ScrcpyAudioWorker *owner)
        : q(owner)
        , audioBufferMs(boundedEnvironmentValue(
              "QTSCRCPY_AUDIO_BUFFER_MS", 40, 20, 200))
        , audioStartMs(boundedEnvironmentValue(
              "QTSCRCPY_AUDIO_START_MS", 20, 5, 100))
        , audioMaximumMs(boundedEnvironmentValue(
              "QTSCRCPY_AUDIO_MAX_MS", 120, 40, 300))
        , pcmRing(static_cast<std::size_t>(
              std::max(audioMaximumMs + 40, 160) * BYTES_PER_MILLISECOND))
    {
        rxBuffer.reserve(64 * 1024);
    }

    ~Impl()
    {
        stop(false);
    }

    ScrcpyAudioWorker *q = nullptr;
    QString serial;
    QString adbPath;
    QString serverPath;
    QString serverVersion;
    QString remoteServerPath;
    QString socketName;
    quint16 localPort = 0;
    quint32 scid = 0;

    QProcess *serverProcess = nullptr;
    QTcpSocket *socket = nullptr;
    QTimer *connectTimer = nullptr;
    QTimer *pumpTimer = nullptr;
    QElapsedTimer connectElapsed;

    QByteArray rxBuffer;
    qsizetype rxOffset = 0;
    InputState inputState = InputState::DummyByte;

    AVCodecContext *codecContext = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    SwrContext *resampler = nullptr;
    AVSampleFormat resamplerInputFormat = AV_SAMPLE_FMT_NONE;
    int resamplerInputRate = 0;

    QIODevice *audioIo = nullptr;
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    QAudioSink *audioSink = nullptr;
#else
    QAudioOutput *audioSink = nullptr;
#endif

    const int audioBufferMs;
    const int audioStartMs;
    const int audioMaximumMs;
    PcmRingBuffer pcmRing;
    bool playbackPrimed = false;
    bool active = false;
    bool announcedStarted = false;
    bool stopping = false;
    bool disconnectDiagnosticPending = false;
    bool protocolStarted = false;
    int connectionAttempts = 0;
    quint64 latencyDrops = 0;
    quint64 packetsReceived = 0;
    quint64 framesDecoded = 0;
    quint64 pcmBytesQueued = 0;
    quint64 pcmBytesWritten = 0;

    [[nodiscard]] bool runAdb(const QStringList &arguments,
                              int timeoutMs,
                              QString *standardOutput = nullptr,
                              bool reportFailure = true)
    {
        QProcess process;
        QStringList params;
        params << "-s" << serial;
        params << arguments;
        process.start(adbPath, params);

        if (!process.waitForStarted(3000)) {
            if (reportFailure) {
                qWarning() << "[Audio] Could not start adb:" << process.errorString();
            }
            return false;
        }

        if (!process.waitForFinished(timeoutMs)) {
            process.kill();
            process.waitForFinished(500);
            if (reportFailure) qWarning() << "[Audio] adb command timed out:" << arguments;
            return false;
        }

        if (standardOutput) {
            *standardOutput = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
        }

        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            if (reportFailure) {
                qWarning().noquote()
                    << "[Audio] adb command failed:"
                    << QString::fromUtf8(process.readAllStandardError()).trimmed();
            }
            return false;
        }
        return true;
    }

    [[nodiscard]] QString takeServerOutput()
    {
        if (!serverProcess) return {};
        return QString::fromUtf8(serverProcess->readAll()).trimmed();
    }

    void fail(const QString &message)
    {
        if (stopping) return;
        qWarning().noquote() << message;
        emit q->errorOccurred(message);
        stop(true);
    }

    void resetStatistics() noexcept
    {
        latencyDrops = 0;
        packetsReceived = 0;
        framesDecoded = 0;
        pcmBytesQueued = 0;
        pcmBytesWritten = 0;
        connectionAttempts = 0;
    }

    void prepareAndroid11Audio()
    {
        QString sdkText;
        if (!runAdb({QStringLiteral("shell"), QStringLiteral("getprop"),
                     QStringLiteral("ro.build.version.sdk")},
                    3000, &sdkText, false)) {
            return;
        }

        bool ok = false;
        const int sdk = sdkText.toInt(&ok);
        if (!ok || sdk != 30) return;

        qInfo() << "[Audio] Android 11 detected; the screen must be unlocked during audio startup";
        if (!environmentEnabled("QTSCRCPY_AUDIO_WAKE_ANDROID11", true)) return;

        // The main mirroring session may have switched the display off. Wake it
        // before scrcpy starts its temporary foreground-activity workaround.
        (void)runAdb({QStringLiteral("shell"), QStringLiteral("input"),
                      QStringLiteral("keyevent"), QStringLiteral("224")},
                     3000, nullptr, false);
    }

    void start(const QString &newSerial,
               quint16 newLocalPort,
               const QString &newServerPath,
               const QString &newServerVersion)
    {
        stop(false);

        if (newSerial.trimmed().isEmpty()) {
            emit q->errorOccurred(QStringLiteral("Audio: device serial is empty"));
            return;
        }
        if (!QFileInfo::exists(newServerPath)) {
            emit q->errorOccurred(
                QStringLiteral("Audio: scrcpy-server not found: %1")
                    .arg(newServerPath));
            return;
        }

        serial = newSerial.trimmed();
        localPort = newLocalPort;
        serverPath = newServerPath;
        serverVersion = newServerVersion;
        adbPath = QString::fromLocal8Bit(qgetenv("QTSCRCPY_ADB_PATH"));
        if (adbPath.isEmpty()) adbPath = QStringLiteral("adb");

        scid = QRandomGenerator::global()->generate() & 0x7fffffffU;
        const QString scidHex = QStringLiteral("%1").arg(
            scid, 8, 16, QLatin1Char('0'));
        socketName = QStringLiteral("scrcpy_%1").arg(scidHex);
        remoteServerPath = QStringLiteral(
            "/data/local/tmp/qtscrcpy-audio-%1.jar").arg(scidHex);

        active = true;
        stopping = false;
        announcedStarted = false;
        disconnectDiagnosticPending = false;
        protocolStarted = false;
        resetStatistics();

        prepareAndroid11Audio();

        if (!runAdb({QStringLiteral("push"), serverPath, remoteServerPath}, 15000)) {
            fail(QStringLiteral("Audio: failed to push scrcpy-server"));
            return;
        }

        (void)runAdb({QStringLiteral("forward"), QStringLiteral("--remove"),
                      QStringLiteral("tcp:%1").arg(localPort)},
                     3000, nullptr, false);

        if (!runAdb({QStringLiteral("forward"),
                     QStringLiteral("tcp:%1").arg(localPort),
                     QStringLiteral("localabstract:%1").arg(socketName)},
                    5000)) {
            fail(QStringLiteral("Audio: failed to create adb forward tunnel"));
            return;
        }

        startServerProcess(scidHex);
    }

    void startServerProcess(const QString &scidHex)
    {
        serverProcess = new QProcess(q);
        serverProcess->setProcessChannelMode(QProcess::MergedChannels);

        QObject::connect(serverProcess, &QProcess::readyRead, q, [this]() {
            const QString output = takeServerOutput();
            if (!output.isEmpty()) qInfo().noquote() << "[Audio server]" << output;
        });

        QObject::connect(
            serverProcess,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            q,
            [this](int exitCode, QProcess::ExitStatus status) {
                const QString output = takeServerOutput();
                if (stopping || !active) return;

                QString message = QStringLiteral(
                    "Audio: scrcpy-server stopped (exit=%1, status=%2)")
                                      .arg(exitCode)
                                      .arg(static_cast<int>(status));
                if (!output.isEmpty()) message += QStringLiteral(": ") + output;
                fail(message);
            });

        QStringList params;
        params << "-s" << serial << "shell";
        params << QStringLiteral("CLASSPATH=%1").arg(remoteServerPath);
        params << "app_process" << "/" << "com.genymobile.scrcpy.Server";
        params << serverVersion;
        params << "log_level=debug";
        params << QStringLiteral("scid=%1").arg(scidHex);
        params << "tunnel_forward=true";
        params << "video=false";
        params << "audio=true";
        params << "audio_codec=opus";
        params << "audio_bit_rate=128000";
        params << "audio_source=output";
        params << "control=false";
        params << "send_device_meta=false";
        params << "send_frame_meta=true";
        params << "send_dummy_byte=true";
        params << "send_stream_meta=true";
        params << "power_on=true";
        params << "cleanup=true";

        serverProcess->start(adbPath, params);
        if (!serverProcess->waitForStarted(3000)) {
            fail(QStringLiteral("Audio: could not start scrcpy-server: %1")
                     .arg(serverProcess->errorString()));
            return;
        }

        createSocket();
        connectElapsed.start();
        connectTimer = new QTimer(q);
        connectTimer->setInterval(CONNECT_RETRY_MS);
        QObject::connect(connectTimer, &QTimer::timeout, q, [this]() {
            attemptConnect();
        });
        connectTimer->start();
        attemptConnect();
    }

    void createSocket()
    {
        socket = new QTcpSocket(q);
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
        socket->setReadBufferSize(128 * 1024);

        QObject::connect(socket, &QTcpSocket::connected, q, [this]() {
            ++connectionAttempts;
            rxBuffer.clear();
            rxOffset = 0;
            inputState = InputState::DummyByte;
            protocolStarted = false;
            qInfo() << "[Audio] ADB tunnel connected, attempt:" << connectionAttempts;
        });
        QObject::connect(socket, &QTcpSocket::readyRead, q, [this]() {
            receiveAudioData();
        });
        QObject::connect(socket, &QTcpSocket::disconnected, q, [this]() {
            receiveAudioData();
            if (stopping || !active) return;

            // adb forward listens locally before the Android abstract socket is
            // ready. The first TCP connection may therefore succeed locally and
            // then close without a single protocol byte. Retry that startup race.
            if (!protocolStarted && inputState == InputState::DummyByte &&
                connectElapsed.isValid() &&
                connectElapsed.elapsed() < CONNECT_TIMEOUT_MS &&
                serverProcess &&
                serverProcess->state() != QProcess::NotRunning) {
                qInfo() << "[Audio] Android socket not ready yet; retrying";
                QTimer::singleShot(CONNECT_RETRY_MS, q, [this]() {
                    attemptConnect();
                });
                return;
            }

            if (disconnectDiagnosticPending) return;
            disconnectDiagnosticPending = true;
            QTimer::singleShot(DISCONNECT_DIAGNOSTIC_DELAY_MS, q, [this]() {
                disconnectDiagnosticPending = false;
                if (stopping || !active) return;

                receiveAudioData();
                if (stopping || !active) return;

                const QString output = takeServerOutput();
                QString message = QStringLiteral("Audio: device audio stream disconnected");
                if (!output.isEmpty()) message += QStringLiteral(": ") + output;
                fail(message);
            });
        });
    }

    void attemptConnect()
    {
        if (!active || stopping || !socket || protocolStarted) return;
        if (socket->state() == QAbstractSocket::ConnectedState ||
            socket->state() == QAbstractSocket::ConnectingState) {
            return;
        }

        if (connectElapsed.isValid() &&
            connectElapsed.elapsed() >= CONNECT_TIMEOUT_MS) {
            fail(QStringLiteral("Audio: timed out waiting for the Android audio socket"));
            return;
        }

        socket->abort();
        socket->connectToHost(QHostAddress::LocalHost, localPort);
    }

    void receiveAudioData()
    {
        if (!socket || stopping) return;
        const QByteArray available = socket->readAll();
        if (!available.isEmpty()) rxBuffer.append(available);
        processInput();
    }

    void compactInputBuffer()
    {
        if (rxOffset == rxBuffer.size()) {
            rxBuffer.clear();
            rxOffset = 0;
        } else if (rxOffset >= 64 * 1024) {
            rxBuffer.remove(0, rxOffset);
            rxOffset = 0;
        }
    }

    void processInput()
    {
        while (!stopping) {
            const qsizetype available = rxBuffer.size() - rxOffset;
            const char *data = rxBuffer.constData() + rxOffset;

            if (inputState == InputState::DummyByte) {
                if (available < 1) break;
                ++rxOffset;
                inputState = InputState::CodecId;
                protocolStarted = true;
                if (connectTimer) connectTimer->stop();
                qInfo() << "[Audio] Android audio socket handshake completed";
                continue;
            }

            if (inputState == InputState::CodecId) {
                if (available < 4) break;
                const quint32 codecId = read32be(data);
                rxOffset += 4;

                if (codecId == 0) {
                    fail(QStringLiteral(
                        "Audio: capture was disabled by the device. This is Android 11; keep the phone unlocked while pressing Start Audio."));
                    return;
                }
                if (codecId == 1) {
                    fail(QStringLiteral(
                        "Audio: the Android audio encoder failed to initialize Opus."));
                    return;
                }
                if (codecId != CODEC_ID_OPUS) {
                    fail(QStringLiteral("Audio: unsupported codec id 0x%1")
                             .arg(codecId, 8, 16, QLatin1Char('0')));
                    return;
                }

                qInfo() << "[Audio] Opus codec metadata received";
                if (!openDecoder() || !setupAudioOutput()) return;

                inputState = InputState::PacketStream;
                announcedStarted = true;
                emit q->started();
                continue;
            }

            if (available < PACKET_HEADER_SIZE) break;
            const quint64 ptsFlags = read64be(data);
            if ((ptsFlags & PACKET_FLAG_SESSION) != 0) {
                fail(QStringLiteral("Audio: unexpected session packet in audio stream"));
                return;
            }

            const quint32 payloadSize = read32be(data + 8);
            if (payloadSize == 0 || payloadSize > MAX_AUDIO_PACKET_SIZE) {
                fail(QStringLiteral("Audio: malformed packet size %1")
                         .arg(payloadSize));
                return;
            }

            const quint64 packetSize = PACKET_HEADER_SIZE +
                                       static_cast<quint64>(payloadSize);
            if (packetSize > static_cast<quint64>(available)) break;

            if (av_new_packet(packet, static_cast<int>(payloadSize)) < 0) {
                fail(QStringLiteral("Audio: packet allocation failed"));
                return;
            }
            std::memcpy(packet->data, data + PACKET_HEADER_SIZE, payloadSize);
            packet->pts = (ptsFlags & PACKET_FLAG_CONFIG)
                ? AV_NOPTS_VALUE
                : static_cast<qint64>(ptsFlags & PACKET_PTS_MASK);
            packet->dts = packet->pts;
            if ((ptsFlags & PACKET_FLAG_KEY_FRAME) != 0) {
                packet->flags |= AV_PKT_FLAG_KEY;
            }

            ++packetsReceived;
            if (packetsReceived == 1) {
                qInfo() << "[Audio] First Opus packet received, bytes:" << payloadSize;
            }

            decodePacket(packet);
            av_packet_unref(packet);
            rxOffset += static_cast<qsizetype>(packetSize);
        }

        compactInputBuffer();
    }

    [[nodiscard]] bool openDecoder()
    {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
        if (!codec) {
            fail(QStringLiteral("Audio: FFmpeg Opus decoder is unavailable"));
            return false;
        }

        codecContext = avcodec_alloc_context3(codec);
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (!codecContext || !packet || !frame) {
            fail(QStringLiteral("Audio: FFmpeg allocation failed"));
            return false;
        }

        codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;
        codecContext->thread_count = 1;
        codecContext->sample_rate = SAMPLE_RATE;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        av_channel_layout_default(&codecContext->ch_layout, CHANNELS);
#else
        codecContext->channel_layout = AV_CH_LAYOUT_STEREO;
        codecContext->channels = CHANNELS;
#endif

        if (avcodec_open2(codecContext, codec, nullptr) < 0) {
            fail(QStringLiteral("Audio: could not open FFmpeg Opus decoder"));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool setupAudioOutput()
    {
        QAudioFormat format;
        format.setSampleRate(SAMPLE_RATE);
        format.setChannelCount(CHANNELS);
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
        format.setSampleFormat(QAudioFormat::Int16);
        const QAudioDevice device = QMediaDevices::defaultAudioOutput();
        if (device.isNull()) {
            fail(QStringLiteral("Audio: no default desktop audio output device"));
            return false;
        }
        if (!device.isFormatSupported(format)) {
            fail(QStringLiteral(
                "Audio: output device does not support 48 kHz stereo S16"));
            return false;
        }
        audioSink = new QAudioSink(device, format, q);
#else
        format.setSampleSize(16);
        format.setCodec(QStringLiteral("audio/pcm"));
        format.setByteOrder(QAudioFormat::LittleEndian);
        format.setSampleType(QAudioFormat::SignedInt);
        const QAudioDeviceInfo device = QAudioDeviceInfo::defaultOutputDevice();
        if (device.isNull() || !device.isFormatSupported(format)) {
            fail(QStringLiteral(
                "Audio: output device does not support 48 kHz stereo S16"));
            return false;
        }
        audioSink = new QAudioOutput(device, format, q);
#endif

        audioSink->setBufferSize(audioBufferMs * BYTES_PER_MILLISECOND);
        audioSink->setVolume(1.0);
        audioIo = audioSink->start();
        if (!audioIo) {
            fail(QStringLiteral("Audio: failed to start the desktop output device"));
            return false;
        }

        qInfo() << "[Audio] Output sink initialized; buffer ms:"
                << audioBufferMs << "start ms:" << audioStartMs;

        pumpTimer = new QTimer(q);
        pumpTimer->setTimerType(Qt::PreciseTimer);
        pumpTimer->setInterval(5);
        QObject::connect(pumpTimer, &QTimer::timeout, q, [this]() {
            pumpAudio();
        });
        pumpTimer->start();
        return true;
    }

    void decodePacket(AVPacket *input)
    {
        if (!codecContext || !frame || !input) return;

        while (!stopping) {
            const int result = avcodec_send_packet(codecContext, input);
            if (result == AVERROR(EAGAIN)) {
                drainDecoder();
                continue;
            }
            if (result < 0) {
                qWarning() << "[Audio] Dropped invalid Opus packet:" << result;
                return;
            }
            break;
        }
        drainDecoder();
    }

    void drainDecoder()
    {
        while (!stopping) {
            const int result = avcodec_receive_frame(codecContext, frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
            if (result < 0) {
                qWarning() << "[Audio] FFmpeg receive error:" << result;
                return;
            }

            ++framesDecoded;
            if (framesDecoded == 1) {
                qInfo() << "[Audio] First PCM frame decoded; samples:"
                        << frame->nb_samples << "format:" << frame->format;
            }
            queueDecodedFrame(frame);
            av_frame_unref(frame);
        }
    }

    [[nodiscard]] bool ensureResampler(const AVFrame *input)
    {
        const auto inputFormat = static_cast<AVSampleFormat>(input->format);
        const int inputRate = input->sample_rate > 0
            ? input->sample_rate
            : SAMPLE_RATE;
        if (resampler && inputFormat == resamplerInputFormat &&
            inputRate == resamplerInputRate) {
            return true;
        }

        if (resampler) swr_free(&resampler);

#if LIBAVUTIL_VERSION_MAJOR >= 57
        AVChannelLayout inputLayout;
        AVChannelLayout outputLayout = AV_CHANNEL_LAYOUT_STEREO;
        if (input->ch_layout.nb_channels > 0) {
            if (av_channel_layout_copy(&inputLayout, &input->ch_layout) < 0) {
                return false;
            }
        } else {
            av_channel_layout_default(&inputLayout, CHANNELS);
        }

        const int result = swr_alloc_set_opts2(
            &resampler,
            &outputLayout,
            AV_SAMPLE_FMT_S16,
            SAMPLE_RATE,
            &inputLayout,
            inputFormat,
            inputRate,
            0,
            nullptr);
        av_channel_layout_uninit(&inputLayout);
        av_channel_layout_uninit(&outputLayout);
        if (result < 0) return false;
#else
        const int inputChannels = input->channels > 0 ? input->channels : CHANNELS;
        resampler = swr_alloc_set_opts(
            nullptr,
            AV_CH_LAYOUT_STEREO,
            AV_SAMPLE_FMT_S16,
            SAMPLE_RATE,
            av_get_default_channel_layout(inputChannels),
            inputFormat,
            inputRate,
            0,
            nullptr);
        if (!resampler) return false;
#endif

        if (!resampler || swr_init(resampler) < 0) {
            if (resampler) swr_free(&resampler);
            return false;
        }

        resamplerInputFormat = inputFormat;
        resamplerInputRate = inputRate;
        return true;
    }

    void queueDecodedFrame(const AVFrame *input)
    {
        if (!input || input->nb_samples <= 0) return;

        const auto format = static_cast<AVSampleFormat>(input->format);
        if (format == AV_SAMPLE_FMT_S16 &&
            input->sample_rate == SAMPLE_RATE &&
            input->data[0]) {
            queuePcm(reinterpret_cast<const char *>(input->data[0]),
                     static_cast<std::size_t>(input->nb_samples * BYTES_PER_FRAME));
            return;
        }

        if (!ensureResampler(input)) {
            qWarning("[Audio] Could not initialize audio resampler");
            return;
        }

        const int inputRate = input->sample_rate > 0
            ? input->sample_rate
            : SAMPLE_RATE;
        const qint64 delayed = swr_get_delay(resampler, inputRate);
        const int outputSamples = static_cast<int>(av_rescale_rnd(
            delayed + input->nb_samples,
            SAMPLE_RATE,
            inputRate,
            AV_ROUND_UP));
        if (outputSamples <= 0) return;

        QByteArray pcm;
        pcm.resize(outputSamples * BYTES_PER_FRAME);
        std::array<uint8_t *, 1> output = {
            reinterpret_cast<uint8_t *>(pcm.data())
        };
        std::array<const uint8_t *, AV_NUM_DATA_POINTERS> inputData{};
        for (std::size_t i = 0; i < inputData.size(); ++i) {
            inputData[i] = input->extended_data[i];
        }
        const int converted = swr_convert(
            resampler,
            output.data(),
            outputSamples,
            inputData.data(),
            input->nb_samples);
        if (converted <= 0) return;

        pcm.resize(converted * BYTES_PER_FRAME);
        queuePcm(pcm.constData(), static_cast<std::size_t>(pcm.size()));
    }

    void queuePcm(const char *data, std::size_t bytes)
    {
        const std::size_t maximumBytes = static_cast<std::size_t>(
            audioMaximumMs * BYTES_PER_MILLISECOND);
        const std::size_t targetBytes = static_cast<std::size_t>(
            std::max(audioStartMs * 2, 40) * BYTES_PER_MILLISECOND);

        if (pcmRing.size() + bytes > maximumBytes) {
            const std::size_t desiredExisting =
                targetBytes > bytes ? targetBytes - bytes : 0;
            if (pcmRing.size() > desiredExisting) {
                pcmRing.discard(pcmRing.size() - desiredExisting);
            }
            ++latencyDrops;
            if (latencyDrops == 1 || latencyDrops % 100 == 0) {
                qWarning() << "[Audio] Latency recovery events:" << latencyDrops;
            }
        }

        pcmRing.push(data, bytes);
        pcmBytesQueued += bytes;
        if (pcmBytesQueued == bytes) {
            qInfo() << "[Audio] First PCM bytes queued:" << bytes;
        }
        pumpAudio();
    }

    [[nodiscard]] qint64 audioBytesFree() const
    {
        return audioSink ? audioSink->bytesFree() : 0;
    }

    void pumpAudio()
    {
        if (!audioIo || !audioSink || pcmRing.empty()) return;

        const std::size_t startBytes = static_cast<std::size_t>(
            audioStartMs * BYTES_PER_MILLISECOND);
        if (!playbackPrimed) {
            if (pcmRing.size() < startBytes) return;
            playbackPrimed = true;
        }

        qint64 freeBytes = audioBytesFree();
        while (freeBytes > 0 && !pcmRing.empty()) {
            const auto [data, contiguous] = pcmRing.frontSpan();
            const qint64 requested = std::min<qint64>(
                freeBytes, static_cast<qint64>(contiguous));
            const qint64 written = audioIo->write(data, requested);
            if (written <= 0) break;
            pcmRing.discard(static_cast<std::size_t>(written));
            freeBytes -= written;
            pcmBytesWritten += static_cast<quint64>(written);
            if (pcmBytesWritten == static_cast<quint64>(written)) {
                qInfo() << "[Audio] First PCM bytes written to sink:" << written;
            }
        }

        if (pcmRing.empty()) playbackPrimed = false;
    }

    void cleanupDecoder()
    {
        if (resampler) swr_free(&resampler);
        if (frame) av_frame_free(&frame);
        if (packet) av_packet_free(&packet);
        if (codecContext) avcodec_free_context(&codecContext);
        resamplerInputFormat = AV_SAMPLE_FMT_NONE;
        resamplerInputRate = 0;
    }

    void cleanupAudioOutput()
    {
        if (pumpTimer) {
            pumpTimer->stop();
            pumpTimer->deleteLater();
            pumpTimer = nullptr;
        }
        if (audioSink) {
            audioSink->stop();
            audioSink->deleteLater();
            audioSink = nullptr;
        }
        audioIo = nullptr;
        pcmRing.clear();
        playbackPrimed = false;
    }

    void stop(bool notify)
    {
        if (stopping) return;
        const bool wasActive = active || announcedStarted;
        stopping = true;
        active = false;
        disconnectDiagnosticPending = false;
        protocolStarted = false;

        if (connectTimer) {
            connectTimer->stop();
            connectTimer->deleteLater();
            connectTimer = nullptr;
        }
        if (socket) {
            QObject::disconnect(socket, nullptr, q, nullptr);
            socket->abort();
            socket->deleteLater();
            socket = nullptr;
        }

        cleanupAudioOutput();
        cleanupDecoder();

        if (serverProcess) {
            QObject::disconnect(serverProcess, nullptr, q, nullptr);
            if (serverProcess->state() != QProcess::NotRunning) {
                serverProcess->terminate();
                if (!serverProcess->waitForFinished(500)) {
                    serverProcess->kill();
                    serverProcess->waitForFinished(500);
                }
            }
            serverProcess->deleteLater();
            serverProcess = nullptr;
        }

        if (!serial.isEmpty() && localPort != 0) {
            (void)runAdb({QStringLiteral("forward"), QStringLiteral("--remove"),
                          QStringLiteral("tcp:%1").arg(localPort)},
                         3000, nullptr, false);
        }
        if (!serial.isEmpty() && !remoteServerPath.isEmpty()) {
            (void)runAdb({QStringLiteral("shell"), QStringLiteral("rm"),
                          QStringLiteral("-f"), remoteServerPath},
                         3000, nullptr, false);
        }

        rxBuffer.clear();
        rxOffset = 0;
        announcedStarted = false;
        stopping = false;

        if (notify && wasActive) emit q->stopped();
    }
};

ScrcpyAudioWorker::ScrcpyAudioWorker(QObject *parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>(this))
{
}

ScrcpyAudioWorker::~ScrcpyAudioWorker() = default;

void ScrcpyAudioWorker::startSession(const QString &serial,
                                     quint16 localPort,
                                     const QString &serverPath,
                                     const QString &serverVersion)
{
    m_impl->start(serial, localPort, serverPath, serverVersion);
}

void ScrcpyAudioWorker::stopSession()
{
    m_impl->stop(true);
}

AudioOutput::AudioOutput(QObject *parent)
    : QObject(parent)
{
    auto *worker = new ScrcpyAudioWorker();
    worker->moveToThread(&m_workerThread);
    m_worker = worker;

    connect(worker, &ScrcpyAudioWorker::started,
            this, &AudioOutput::started);
    connect(worker, &ScrcpyAudioWorker::stopped,
            this, &AudioOutput::stopped);
    connect(worker, &ScrcpyAudioWorker::errorOccurred,
            this, &AudioOutput::errorOccurred);

    m_workerThread.start();
}

AudioOutput::~AudioOutput()
{
    if (m_worker) {
        QMetaObject::invokeMethod(
            m_worker,
            "stopSession",
            Qt::BlockingQueuedConnection);
        ScrcpyAudioWorker *worker = m_worker;
        QMetaObject::invokeMethod(
            worker,
            [worker]() { delete worker; },
            Qt::BlockingQueuedConnection);
    }
    m_workerThread.quit();
    m_workerThread.wait();
}

bool AudioOutput::start(const QString &serial, int port)
{
    if (!m_worker || serial.trimmed().isEmpty() ||
        port <= 0 || port > std::numeric_limits<quint16>::max()) {
        return false;
    }

    const QString serverPath = resolveServerPath();
    if (!QFileInfo::exists(serverPath)) {
        emit errorOccurred(
            QStringLiteral("Audio: scrcpy-server not found: %1")
                .arg(serverPath));
        return false;
    }

    return QMetaObject::invokeMethod(
        m_worker,
        "startSession",
        Qt::QueuedConnection,
        Q_ARG(QString, serial.trimmed()),
        Q_ARG(quint16, static_cast<quint16>(port)),
        Q_ARG(QString, serverPath),
        Q_ARG(QString, resolveServerVersion()));
}

void AudioOutput::stop()
{
    if (m_worker) {
        QMetaObject::invokeMethod(
            m_worker,
            "stopSession",
            Qt::QueuedConnection);
    }
}

bool AudioOutput::install(const QString &serial)
{
    Q_UNUSED(serial)
    return true;
}

QString AudioOutput::resolveServerPath() const
{
    QString path = QString::fromLocal8Bit(qgetenv("QTSCRCPY_SERVER_PATH"));
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        path = QCoreApplication::applicationDirPath() +
               QStringLiteral("/scrcpy-server");
    }
    return path;
}

QString AudioOutput::resolveServerVersion() const
{
    const QString configured = QString::fromLocal8Bit(
        qgetenv("QTSCRCPY_SERVER_VERSION"));
    return configured.isEmpty() ? QStringLiteral("3.3.4") : configured;
}
