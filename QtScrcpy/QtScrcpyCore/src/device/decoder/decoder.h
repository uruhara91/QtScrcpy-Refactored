#ifndef DECODER_H
#define DECODER_H

#include <QThread>
#include <functional>
#include <memory>
#include <span>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/pixfmt.h"
}

struct AVCodecContext;
struct AVPacket;

struct AVCodecContextDeleter {
    void operator()(AVCodecContext* ctx) const;
};

class VideoBuffer;

class Decoder : public QThread
{
    Q_OBJECT
public:
    using FrameCallback = std::function<void(int width, int height, 
                                           std::span<const uint8_t> dataY, 
                                           std::span<const uint8_t> dataU, 
                                           std::span<const uint8_t> dataV, 
                                           int linesizeY, int linesizeU, int linesizeV)>;

    explicit Decoder(FrameCallback onFrame, QObject *parent = nullptr);
    virtual ~Decoder() override;

    [[nodiscard]] bool open();
    void close();

    void peekFrame(std::function<void(int width, int height, uint8_t* dataRGB32)> onFrame);

    VideoBuffer* videoBuffer() const { return m_vb.get(); }

public slots:
    void onDecodeFrame(AVPacket *packet);

signals:
    void updateFPS(quint32 fps);
    void newFrame();

protected:
    void run() override;

private:
    void pushFrameToBuffer();

private:
    std::unique_ptr<VideoBuffer> m_vb;
    std::unique_ptr<AVCodecContext, AVCodecContextDeleter> m_codecCtx;
    
    bool m_isCodecCtxOpen = false;
    FrameCallback m_onFrame;
};

#endif // DECODER_H