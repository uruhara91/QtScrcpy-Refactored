#pragma once
#include <QString>

namespace qsc {

// ---------------------------------------------------------------------
// Zero-copy hardware-decoded frame handoff (experimental, VAAPI/Linux
// only for now - see FrameSink::submitHwFrame() in QtScrcpyCore.h and
// the QTSCRCPY_EXPERIMENTAL_ZEROCOPY environment variable).
// ---------------------------------------------------------------------
// Describes one importable plane of a hardware-decoded frame as a Linux
// DMA-BUF: everything EGL's EGL_EXT_image_dma_buf_import (+ _modifiers)
// extension needs to import it directly as a GL texture with zero CPU
// copies. Deliberately mirrors FFmpeg's own AVDRMPlaneDescriptor /
// AVDRMObjectDescriptor fields (see libavutil/hwcontext_drm.h) rather
// than reusing those types directly, so this public header never needs
// to include FFmpeg headers or expose FFmpeg types across the library
// boundary - the same reason submitFrame() below uses std::span instead
// of a raw AVFrame.
struct HwFramePlane {
    int fd = -1;          // DRM PRIME (dma-buf) file descriptor - NOT owned by the receiver; valid only for the duration of the submitHwFrame() call, and (if imported into something, e.g. a GL texture, that outlives the call) only until the matching release() callback is invoked
    quint32 fourcc = 0;   // DRM_FORMAT_* code for this plane (e.g. DRM_FORMAT_R8, DRM_FORMAT_GR88)
    quint64 modifier = 0; // DRM format modifier (tiling layout); ~0ULL (DRM_FORMAT_MOD_INVALID) if unknown - treat as "assume linear, best effort" in that case
    qint64 offset = 0;    // Byte offset of this plane's data within fd
    qint64 pitch = 0;     // Row stride in bytes
    int width = 0;
    int height = 0;
};

struct HwFrameDrmDescriptor {
    static constexpr int kMaxPlanes = 4;
    int planeCount = 0;
    HwFramePlane planes[kMaxPlanes];
};

struct DeviceParams {
    // necessary
    QString serial = "";              // 设备序列号
    QString serverLocalPath = "";     // 本地安卓server路径

    // optional
    QString serverRemotePath = "/data/local/tmp/scrcpy-server.jar";    // 要推送到远端设备的server路径
    quint16 localPort = 27183;        // reverse时本地监听端口
    quint16 maxSize = 720;            // 视频分辨率
    quint32 bitRate = 2000000;        // 视频比特率
    quint32 maxFps = 0;               // 视频最大帧率
    bool useReverse = true;           // true:先使用adb reverse，失败后自动使用adb forward；false:直接使用adb forward
    int captureOrientationLock = 0;   // 是否锁定采集方向 0不锁定 1锁定指定方向 2锁定原始方向
    int captureOrientation = 0;       // 采集方向 0 90 180 270
    bool stayAwake = false;           // 是否保持唤醒
    bool useRoot = false;             // 是否以root权限启动server (su -c)
    QString serverVersion = "4.0";    // server版本
    QString logLevel = "debug";     // log级别 verbose/debug/info/warn/error
    // 编码选项 ""表示默认
    // 例如 CodecOptions="profile=1,level=2"
    // 更多编码选项参考 https://d.android.com/reference/android/media/MediaFormat
    QString codecOptions = "";
    // 指定编码器名称(必须是H.264编码器)，""表示默认
    // 例如 CodecName="OMX.qcom.video.encoder.avc"
    QString codecName = "";
    quint32 scid = -1; // 随机数，作为localsocket名字后缀，方便同时连接同一个设备多次

    QString recordPath = "";          // 视频保存路径
    QString recordFileFormat = "mp4"; // 视频保存格式 mp4/mkv
    bool recordFile = false;          // 录制到文件

    QString pushFilePath = "/sdcard/"; // 推送到安卓设备的文件保存路径（必须以/结尾）

    bool closeScreen = false;         // 启动时自动息屏
    bool display = true;              // 是否显示画面（或者仅仅后台录制）
    bool renderExpiredFrames = false; // 是否渲染延迟视频帧
    QString gameScript = "";          // 游戏映射脚本
    // 是否优先尝试硬件解码 (VideoToolbox/D3D11VA/DXVA2/VAAPI/NVDEC，视平台而定)。
    // true 仍然只是"优先尝试"——硬件解码不可用或打开失败时会自动退回软件解码，
    // 并不保证一定用上硬件；false 则直接跳过硬件解码，只用软件解码。
    // 对应 Dialog 里的 "decoder:" 下拉框。QTSCRCPY_DISABLE_HWACCEL 环境变量
    // 如果被显式设置，会覆盖这个值（见 Decoder::tryInitHwAccel()）。
    bool useHwDecode = true;
};
    
}
