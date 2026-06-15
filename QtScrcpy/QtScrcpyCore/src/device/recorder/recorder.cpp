#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>

#include "compat.h"
#include "recorder.h"
#include "demuxer.h"

static const AVRational SCRCPY_TIME_BASE = { 1, 1000000 }; // timestamps in us

Recorder::Recorder(const QString &fileName, QObject *parent)
    : QThread(parent), m_fileName(fileName), m_format(guessRecordFormat(fileName))
{}

Recorder::~Recorder()
{
    stopRecorder();
    wait();
    queueClear();
    close();
}

void Recorder::packetDelete(AVPacket *packet)
{
    if (packet) {
        PacketPool::get().release(packet);
    }
}

void Recorder::queueClear()
{
    QMutexLocker locker(&m_mutex);
    while (!m_queue.isEmpty()) {
        packetDelete(m_queue.dequeue());
    }
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
    const AVCodec* inputCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!inputCodec) {
        qCritical("H.264 decoder not found");
        return false;
    }

    QString formatName = recorderGetFormatName(m_format);
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

    QString comment = "Recorded by QtScrcpy " + QCoreApplication::applicationVersion();
    av_dict_set(&m_formatCtx->metadata, "comment", comment.toUtf8(), 0);

    AVStream *outStream = avformat_new_stream(m_formatCtx, inputCodec);
    if (!outStream) {
        avformat_free_context(m_formatCtx);
        m_formatCtx = Q_NULLPTR;
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

    int ret = avio_open(&m_formatCtx->pb, m_fileName.toUtf8().constData(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        char errorbuf[255] = { 0 };
        av_strerror(ret, errorbuf, 254);
        qCritical() << QString("Failed to open output file: %1 %2").arg(errorbuf).arg(m_fileName);
        avformat_free_context(m_formatCtx);
        m_formatCtx = Q_NULLPTR;
        return false;
    }

    m_headerWritten = false;
    m_stopped.store(false, std::memory_order_release);
    m_failed.store(false, std::memory_order_release);
    return true;
}

void Recorder::close()
{
    if (Q_NULLPTR == m_formatCtx) return;

    if (m_headerWritten) {
        int ret = av_write_trailer(m_formatCtx);
        if (ret < 0) {
            qCritical() << QString("Failed to write trailer to %1").arg(m_fileName);
            m_failed.store(true, std::memory_order_release);
        } else {
            qInfo() << QString("success record %1").arg(m_fileName);
        }
    } else {
        m_failed.store(true, std::memory_order_release);
    }

    if (m_formatCtx->pb) {
        avio_closep(&m_formatCtx->pb);
    }
    avformat_free_context(m_formatCtx);
    m_formatCtx = Q_NULLPTR;
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
        if (!recorderWriteHeader(packet)) {
            return false;
        }
        m_headerWritten = true;
        return true;
    }

    if (packet->pts == AV_NOPTS_VALUE) {
        return true;
    }

    recorderRescalePacket(packet);
    return av_write_frame(m_formatCtx, packet) >= 0;
}

const AVOutputFormat *Recorder::findMuxer(const char *name)
{
#ifdef QTSCRCPY_LAVF_HAS_NEW_MUXER_ITERATOR_API
    void *opaque = Q_NULLPTR;
#endif
    const AVOutputFormat *outFormat = Q_NULLPTR;
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
    quint8 *extradata = static_cast<quint8 *>(av_malloc(packet->size * sizeof(quint8)));
    if (!extradata) {
        qCritical("Cannot allocate extradata");
        return false;
    }
    memcpy(extradata, packet->data, packet->size);

#ifdef QTSCRCPY_LAVF_HAS_NEW_CODEC_PARAMS_API
    ostream->codecpar->extradata = extradata;
    ostream->codecpar->extradata_size = packet->size;
#else
    ostream->codec->extradata = extradata;
    ostream->codec->extradata_size = packet->size;
#endif

    int ret = avformat_write_header(m_formatCtx, nullptr);
    if (ret < 0) {
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
    if (fileName.length() < 4) {
        return Recorder::RECORDER_FORMAT_NULL;
    }

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

    QMutexLocker locker(&m_mutex);
    if (m_stopped.load(std::memory_order_acquire) ||
        m_failed.load(std::memory_order_acquire)) {
        return false;
    }

    m_queue.enqueue(packet);
    m_recvDataCond.wakeOne();
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
            while (m_queue.isEmpty() && !m_stopped.load(std::memory_order_acquire)) {
                m_recvDataCond.wait(&m_mutex);
            }

            if (m_queue.isEmpty() && m_stopped.load(std::memory_order_acquire)) {
                break;
            }

            if (!m_queue.isEmpty()) {
                rec = m_queue.dequeue();
            }
        }

        if (!rec) continue;

        if (previous) {
            if (previous->pts != AV_NOPTS_VALUE && rec->pts != AV_NOPTS_VALUE) {
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
            if (ptsOrigin == AV_NOPTS_VALUE) {
                ptsOrigin = rec->pts;
            }
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

    qDebug("Recorder thread ended");
}

bool Recorder::startRecorder()
{
    if (!m_formatCtx || isRunning()) return false;

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
