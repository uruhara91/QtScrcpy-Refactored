#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>

#include "compat.h"
#include "demuxer.h"
#include "qtscrcpytelemetry.h"
#include "recorder.h"

static const AVRational SCRCPY_TIME_BASE = { 1, 1000000 }; // timestamps in us

Recorder::Recorder(const QString &fileName, QObject *parent)
    : QThread(parent)
    , m_fileName(fileName)
    , m_format(guessRecordFormat(fileName))
{
    const int queueMiB = qsc::telemetry::boundedEnvironmentInt(
        "QTSCRCPY_RECORDER_QUEUE_MB", 8, 1, 256);
    m_maxQueueBytes = static_cast<std::size_t>(queueMiB) * 1024U * 1024U;
    m_maxQueuePackets = qsc::telemetry::boundedEnvironmentInt(
        "QTSCRCPY_RECORDER_QUEUE_PACKETS", 240, 16, 4096);
    m_telemetryEnabled = qsc::telemetry::enabled();
}

Recorder::~Recorder()
{
    stopRecorder();
    wait();
    queueClear();
    close();
}

void Recorder::packetDelete(AVPacket *packet)
{
    if (packet) PacketPool::get().release(packet);
}

void Recorder::queueClearLocked()
{
    while (!m_queue.isEmpty()) {
        packetDelete(m_queue.dequeue());
    }
    m_queuedBytes = 0;
}

void Recorder::queueClear()
{
    QMutexLocker locker(&m_mutex);
    queueClearLocked();
}

void Recorder::updateQueuePeaksLocked()
{
    const std::size_t currentBytes = m_peakQueueBytes.load(std::memory_order_relaxed);
    if (m_queuedBytes > currentBytes) {
        m_peakQueueBytes.store(m_queuedBytes, std::memory_order_relaxed);
    }

    const int queuePackets = m_queue.size();
    const int currentPackets = m_peakQueuePackets.load(std::memory_order_relaxed);
    if (queuePackets > currentPackets) {
        m_peakQueuePackets.store(queuePackets, std::memory_order_relaxed);
    }
}

void Recorder::logTelemetry() const
{
    if (!m_telemetryEnabled) return;

    qInfo() << "[Telemetry][Recorder] queue"
            << "limitBytes=" << m_maxQueueBytes
            << "limitPackets=" << m_maxQueuePackets
            << "peakBytes=" << m_peakQueueBytes.load(std::memory_order_relaxed)
            << "peakPackets=" << m_peakQueuePackets.load(std::memory_order_relaxed)
            << "overflows=" << m_overflowEvents.load(std::memory_order_relaxed)
            << "failed=" << m_failed.load(std::memory_order_relaxed);
}

void Recorder::setFrameSize(const QSize &declaredFrameSize)
{
    m_declaredFrameSize = declaredFrameSize;
}

void Recorder::setFormat(Recorder::RecorderFormat format)
{
    m_format = format;
}

bool Recorder::open()
{
    const AVCodec *inputCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!inputCodec) {
        qCritical("H.264 decoder not found");
        return false;
    }

    const QString formatName = recorderGetFormatName(m_format);
    if (formatName.isEmpty()) {
        qCritical("Invalid recorder format");
        return false;
    }

    const AVOutputFormat *format = findMuxer(formatName.toUtf8());
    if (!format) {
        qCritical("Could not find muxer");
        return false;
    }

    m_formatCtx = avformat_alloc_context();
    if (!m_formatCtx) {
        qCritical("Could not allocate output context");
        return false;
    }

    m_formatCtx->oformat = const_cast<AVOutputFormat *>(format);

    const QString comment = "Recorded by QtScrcpy " +
                            QCoreApplication::applicationVersion();
    av_dict_set(&m_formatCtx->metadata, "comment", comment.toUtf8(), 0);

    AVStream *outStream = avformat_new_stream(m_formatCtx, inputCodec);
    if (!outStream) {
        avformat_free_context(m_formatCtx);
        m_formatCtx = nullptr;
        return false;
    }

#ifdef QTSCRCPY_LAVF_HAS_NEW_CODEC_PARAMS_API
    outStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    outStream->codecpar->codec_id = inputCodec->id;
    outStream->codecpar->format = AV_PIX_FMT_YUV420P;
    outStream->codecpar->width = m_declaredFrameSize.width();
    outStream->codecpar->height = m_declaredFrameSize.height();
#else
    outStream->codec->codec_type = AVMEDIA_TYPE_VIDEO;
    outStream->codec->codec_id = inputCodec->id;
    outStream->codec->pix_fmt = AV_PIX_FMT_YUV420P;
    outStream->codec->width = m_declaredFrameSize.width();
    outStream->codec->height = m_declaredFrameSize.height();
#endif

    const int ret = avio_open(&m_formatCtx->pb,
                              m_fileName.toUtf8().constData(),
                              AVIO_FLAG_WRITE);
    if (ret < 0) {
        char errorbuf[255] = { 0 };
        av_strerror(ret, errorbuf, 254);
        qCritical() << QString("Failed to open output file: %1 %2")
                           .arg(errorbuf)
                           .arg(m_fileName);
        avformat_free_context(m_formatCtx);
        m_formatCtx = nullptr;
        return false;
    }

    m_headerWritten = false;
    m_stopped.store(false, std::memory_order_release);
    m_failed.store(false, std::memory_order_release);
    return true;
}

void Recorder::close()
{
    if (!m_formatCtx) return;

    if (m_headerWritten) {
        const int ret = av_write_trailer(m_formatCtx);
        if (ret < 0) {
            qCritical() << QString("Failed to write trailer to %1").arg(m_fileName);
            m_failed.store(true, std::memory_order_release);
        } else if (m_failed.load(std::memory_order_acquire)) {
            qWarning() << QString("Recording stopped early: %1").arg(m_fileName);
        } else {
            qInfo() << QString("success record %1").arg(m_fileName);
        }
    } else {
        m_failed.store(true, std::memory_order_release);
    }

    if (m_formatCtx->pb) avio_closep(&m_formatCtx->pb);
    avformat_free_context(m_formatCtx);
    m_formatCtx = nullptr;
    m_headerWritten = false;
}

bool Recorder::write(AVPacket *packet)
{
    if (!packet || !m_formatCtx) return false;

    if (!m_headerWritten) {
        if (packet->pts != AV_NOPTS_VALUE) {
            qCritical("The first packet is not a config packet");
            return false;
        }
        if (!recorderWriteHeader(packet)) return false;
        m_headerWritten = true;
        return true;
    }

    if (packet->pts == AV_NOPTS_VALUE) return true;

    recorderRescalePacket(packet);
    return av_write_frame(m_formatCtx, packet) >= 0;
}

const AVOutputFormat *Recorder::findMuxer(const char *name)
{
#ifdef QTSCRCPY_LAVF_HAS_NEW_MUXER_ITERATOR_API
    void *opaque = nullptr;
#endif
    const AVOutputFormat *outFormat = nullptr;
    do {
#ifdef QTSCRCPY_LAVF_HAS_NEW_MUXER_ITERATOR_API
        outFormat = av_muxer_iterate(&opaque);
#else
        outFormat = av_oformat_next(outFormat);
#endif
    } while (outFormat && strcmp(outFormat->name, name));
    return outFormat;
}

bool Recorder::recorderWriteHeader(const AVPacket *packet)
{
    AVStream *ostream = m_formatCtx->streams[0];
    auto *extradata = static_cast<quint8 *>(
        av_malloc(static_cast<std::size_t>(packet->size)));
    if (!extradata) {
        qCritical("Cannot allocate extradata");
        return false;
    }
    memcpy(extradata, packet->data, static_cast<std::size_t>(packet->size));

#ifdef QTSCRCPY_LAVF_HAS_NEW_CODEC_PARAMS_API
    ostream->codecpar->extradata = extradata;
    ostream->codecpar->extradata_size = packet->size;
#else
    ostream->codec->extradata = extradata;
    ostream->codec->extradata_size = packet->size;
#endif

    if (avformat_write_header(m_formatCtx, nullptr) < 0) {
        qCritical("Failed to write header recorder file");
        return false;
    }
    return true;
}

void Recorder::recorderRescalePacket(AVPacket *packet)
{
    AVStream *ostream = m_formatCtx->streams[0];
    av_packet_rescale_ts(packet, SCRCPY_TIME_BASE, ostream->time_base);
}

QString Recorder::recorderGetFormatName(Recorder::RecorderFormat format)
{
    switch (format) {
    case RECORDER_FORMAT_MP4:
        return "mp4";
    case RECORDER_FORMAT_MKV:
        return "matroska";
    default:
        return "";
    }
}

Recorder::RecorderFormat Recorder::guessRecordFormat(const QString &fileName)
{
    if (fileName.length() < 4) return Recorder::RECORDER_FORMAT_NULL;

    const QString ext = QFileInfo(fileName).suffix();
    if (ext.compare("mp4", Qt::CaseInsensitive) == 0) {
        return Recorder::RECORDER_FORMAT_MP4;
    }
    if (ext.compare("mkv", Qt::CaseInsensitive) == 0) {
        return Recorder::RECORDER_FORMAT_MKV;
    }
    return Recorder::RECORDER_FORMAT_NULL;
}

bool Recorder::push(AVPacket *packet)
{
    if (!packet) return false;

    const std::size_t packetBytes = packet->size > 0
        ? static_cast<std::size_t>(packet->size)
        : 0;
    bool overflow = false;

    {
        QMutexLocker locker(&m_mutex);
        if (m_stopped.load(std::memory_order_acquire) ||
            m_failed.load(std::memory_order_acquire)) {
            return false;
        }

        const bool packetLimitReached = m_queue.size() >= m_maxQueuePackets;
        const bool byteLimitReached =
            packetBytes > m_maxQueueBytes ||
            m_queuedBytes > m_maxQueueBytes - packetBytes;

        if (packetLimitReached || byteLimitReached) {
            overflow = true;
            m_overflowEvents.fetch_add(1, std::memory_order_relaxed);
            m_failed.store(true, std::memory_order_release);
            m_stopped.store(true, std::memory_order_release);
            queueClearLocked();
            m_recvDataCond.wakeAll();
        } else {
            m_queue.enqueue(packet);
            m_queuedBytes += packetBytes;
            updateQueuePeaksLocked();
            m_recvDataCond.wakeOne();
        }
    }

    if (overflow) {
        qCritical() << "Recorder queue limit exceeded; recording stopped"
                    << "packetBytes=" << packetBytes
                    << "limitBytes=" << m_maxQueueBytes
                    << "limitPackets=" << m_maxQueuePackets;
        return false;
    }
    return true;
}

void Recorder::run()
{
    AVPacket *previous = nullptr;
    int64_t ptsOrigin = AV_NOPTS_VALUE;

    while (true) {
        AVPacket *rec = nullptr;
        {
            QMutexLocker locker(&m_mutex);
            while (m_queue.isEmpty() &&
                   !m_stopped.load(std::memory_order_acquire)) {
                m_recvDataCond.wait(&m_mutex);
            }

            if (m_queue.isEmpty() &&
                m_stopped.load(std::memory_order_acquire)) {
                break;
            }

            if (!m_queue.isEmpty()) {
                rec = m_queue.dequeue();
                const std::size_t bytes = rec && rec->size > 0
                    ? static_cast<std::size_t>(rec->size)
                    : 0;
                m_queuedBytes = bytes > m_queuedBytes
                    ? 0
                    : m_queuedBytes - bytes;
            }
        }

        if (!rec) continue;

        if (previous) {
            if (previous->pts != AV_NOPTS_VALUE &&
                rec->pts != AV_NOPTS_VALUE) {
                previous->duration = rec->pts - previous->pts;
            }

            const bool ok = write(previous);
            packetDelete(previous);
            previous = nullptr;

            if (!ok) {
                qCritical("Recorder: Could not record packet");
                m_failed.store(true, std::memory_order_release);
                packetDelete(rec);
                queueClear();
                break;
            }
        }

        if (rec->pts == AV_NOPTS_VALUE) {
            if (!write(rec)) {
                qCritical("Recorder: Could not record config packet");
                m_failed.store(true, std::memory_order_release);
            }
            packetDelete(rec);
        } else {
            if (ptsOrigin == AV_NOPTS_VALUE) ptsOrigin = rec->pts;
            rec->pts -= ptsOrigin;
            rec->dts = rec->pts;
            previous = rec;
        }
    }

    if (previous && !m_failed.load(std::memory_order_acquire)) {
        previous->duration = 100000;
        if (!write(previous)) {
            m_failed.store(true, std::memory_order_release);
        }
        packetDelete(previous);
    } else if (previous) {
        packetDelete(previous);
    }

    logTelemetry();
    qDebug("Recorder thread ended");
}

bool Recorder::startRecorder()
{
    if (!m_formatCtx || isRunning()) return false;

    {
        QMutexLocker locker(&m_mutex);
        queueClearLocked();
    }
    m_peakQueueBytes.store(0, std::memory_order_relaxed);
    m_peakQueuePackets.store(0, std::memory_order_relaxed);
    m_overflowEvents.store(0, std::memory_order_relaxed);
    m_stopped.store(false, std::memory_order_release);
    m_failed.store(false, std::memory_order_release);
    start();
    return true;
}

void Recorder::stopRecorder()
{
    QMutexLocker locker(&m_mutex);
    m_stopped.store(true, std::memory_order_release);
    m_recvDataCond.wakeAll();
}
