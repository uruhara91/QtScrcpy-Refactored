#ifndef STREAM_H
#define STREAM_H

#include <QPointer>
#include <QSize>
#include <QThread>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavcodec/packet.h"
#include "libavformat/avformat.h"
#include "libavutil/buffer.h"
#include "libavutil/mem.h"
}

class VideoSocket;

// Reuses packet *payload* memory across network packets, keyed by a
// power-of-two size class. AVPacket structs themselves are already reused by
// PacketPool below, but PacketPool only recycles the empty struct: the data
// buffer is still allocated/freed per packet via av_new_packet(), which
// means one av_malloc()+av_free() per network packet (potentially 60+/s for
// video). This pool instead recycles the underlying data allocations too,
// wrapping recycled/new memory in an AVBufferRef via av_buffer_create() so
// it plugs directly into AVPacket::buf and is released automatically by
// FFmpeg's normal refcounting (av_packet_unref) - no change needed at any
// call site that already deals in AVPacket.
//
// Design notes:
//  - Size classes are power-of-two buckets. A request for N bytes is
//    rounded up to the next power of two (with a minimum floor) so that,
//    e.g., a buffer freed after a large keyframe can later satisfy a
//    smaller delta-frame request from the same bucket, maximizing reuse
//    without needing an exact-size match.
//  - Every allocation includes AV_INPUT_BUFFER_PADDING_SIZE extra bytes at
//    the end, zero-initialized, exactly like av_new_packet() - required
//    because FFmpeg decoders may read a few bytes past the declared size
//    for SIMD-friendly parsing. Recycled buffers have their *entire*
//    capacity (payload + padding) re-zeroed before reuse so this guarantee
//    holds every time, not just on first allocation.
//  - Thread-safe: acquire()/release() may be called from any thread ( the
//    demuxer thread acquires; the free callback may run on whichever thread
//    drops the last AVBufferRef reference, e.g. the decoder thread).
//  - Bounded: both per-bucket and total pooled memory are capped so a
//    transient spike (e.g. a burst of large keyframes after a resize) can't
//    make the pool grow unbounded. Buffers beyond the cap are simply freed
//    instead of pooled, falling back to ordinary alloc/free for that
//    packet - correctness is never affected, only reuse rate.
class PacketBufferPool {
public:
    static PacketBufferPool& get() {
        static PacketBufferPool instance;
        return instance;
    }

    // Returns an AVBufferRef whose data has room for at least `size` bytes
    // of payload plus AV_INPUT_BUFFER_PADDING_SIZE bytes of zeroed padding
    // immediately after. AVBufferRef::size is set to exactly `size` (not
    // the rounded bucket capacity), matching av_new_packet() semantics.
    // Returns nullptr on allocation failure or invalid size.
    [[nodiscard]] AVBufferRef* acquire(std::size_t size) {
        if (size == 0 ||
            size > static_cast<std::size_t>(std::numeric_limits<int>::max()) -
                       kPadding) {
            return nullptr;
        }

        const std::size_t bucketCapacity = bucketFor(size);
        const std::size_t bucketIndex = bucketIndexFor(bucketCapacity);

        uint8_t* raw = nullptr;
        if (bucketIndex < kBucketCount) {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto& freeList = m_buckets[bucketIndex];
            if (!freeList.empty()) {
                raw = freeList.back();
                freeList.pop_back();
                m_pooledBytes -= bucketCapacity;
            }
        }

        if (!raw) {
            raw = static_cast<uint8_t*>(av_malloc(bucketCapacity + kPadding));
            if (!raw) return nullptr;
        }

        // Zero the requested payload tail + full padding region, matching
        // av_new_packet()'s guarantee. Recycled buffers may carry stale
        // data from a previous, larger use, so this must cover from `size`
        // (not just the bucket boundary) through the end of the allocation.
        std::memset(raw + size, 0, (bucketCapacity - size) + kPadding);

        auto* freeCtx = new BufferFreeContext{bucketCapacity};
        AVBufferRef* ref = av_buffer_create(
            raw, size,
            &PacketBufferPool::releaseTrampoline,
            freeCtx,
            0);
        if (!ref) {
            delete freeCtx;
            releaseRaw(raw, bucketCapacity);
            return nullptr;
        }
        return ref;
    }

private:
    // AV_INPUT_BUFFER_PADDING_SIZE as a compile-time constant (statically
    // checked below to stay in sync with the actual FFmpeg build linked
    // in - this matters because this project injects a custom-built
    // FFmpeg, so the value isn't necessarily the upstream default).
    static constexpr std::size_t kPadding = 64;
    static_assert(kPadding == AV_INPUT_BUFFER_PADDING_SIZE,
                  "PacketBufferPool::kPadding must track "
                  "AV_INPUT_BUFFER_PADDING_SIZE from the linked FFmpeg build "
                  "(differs across builds/versions) - update the constant "
                  "above if this fails.");
    static constexpr std::size_t kMinBucket = 4096;       // smallest class
    static constexpr std::size_t kMaxBucket = 8u << 20;   // 8 MiB ceiling
    static constexpr std::size_t kBucketCount = 12;       // 4KiB .. 8MiB
    static constexpr std::size_t kMaxPooledPerBucket = 8;
    static constexpr std::size_t kMaxTotalPooledBytes = 64u << 20; // 64 MiB

    struct BufferFreeContext {
        std::size_t capacity;
    };

    static std::size_t bucketFor(std::size_t size) {
        if (size <= kMinBucket) return kMinBucket;
        // sizes > kMaxBucket are served with an exact-size, unpooled
        // allocation (still correct, just not reused - see releaseRaw()).
        // A size of exactly kMaxBucket still rounds to a valid pooled
        // bucket via bit_ceil below, which is fine since it is already a
        // power of two.
        if (size > kMaxBucket) return size;
        return std::bit_ceil(size);
    }

    static std::size_t bucketIndexFor(std::size_t capacity) {
        if (capacity < kMinBucket || capacity > kMaxBucket) return kBucketCount;
        // capacity is a power of two in [kMinBucket, kMaxBucket] here.
        const unsigned shift = static_cast<unsigned>(
            std::countr_zero(capacity) - std::countr_zero(kMinBucket));
        return shift;
    }

    static void releaseTrampoline(void* opaque, uint8_t* data) {
        auto* ctx = static_cast<BufferFreeContext*>(opaque);
        PacketBufferPool::get().releaseRaw(data, ctx->capacity);
        delete ctx;
    }

    void releaseRaw(uint8_t* raw, std::size_t capacity) noexcept {
        const std::size_t bucketIndex = bucketIndexFor(capacity);
        if (bucketIndex < kBucketCount) {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto& freeList = m_buckets[bucketIndex];
            if (freeList.size() < kMaxPooledPerBucket &&
                m_pooledBytes + capacity <= kMaxTotalPooledBytes) {
                freeList.push_back(raw);
                m_pooledBytes += capacity;
                return;
            }
        }
        av_free(raw);
    }

    PacketBufferPool() = default;
    ~PacketBufferPool() {
        for (const auto& freeList : m_buckets) {
            for (uint8_t* raw : freeList) av_free(raw);
        }
    }
    PacketBufferPool(const PacketBufferPool&) = delete;
    PacketBufferPool& operator=(const PacketBufferPool&) = delete;

    std::array<std::vector<uint8_t*>, kBucketCount> m_buckets;
    std::size_t m_pooledBytes = 0;
    std::mutex m_mutex;
};

class PacketPool {
public:
    static PacketPool& get() {
        static PacketPool instance;
        return instance;
    }

    [[nodiscard]] AVPacket* acquire() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_pool.empty()) {
                AVPacket* packet = m_pool.back();
                m_pool.pop_back();
                return packet;
            }
        }
        return av_packet_alloc();
    }

    void release(AVPacket* packet) noexcept {
        if (!packet) return;
        av_packet_unref(packet);

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pool.size() < MAX_POOL_SIZE) {
            m_pool.push_back(packet);
        } else {
            av_packet_free(&packet);
        }
    }

private:
    static constexpr std::size_t PREALLOCATED_PACKETS = 64;
    static constexpr std::size_t MAX_POOL_SIZE = 256;

    PacketPool() {
        m_pool.reserve(MAX_POOL_SIZE);
        for (std::size_t i = 0; i < PREALLOCATED_PACKETS; ++i) {
            if (AVPacket* packet = av_packet_alloc()) {
                m_pool.push_back(packet);
            }
        }
    }

    ~PacketPool() {
        for (AVPacket* packet : m_pool) {
            av_packet_free(&packet);
        }
    }

    PacketPool(const PacketPool&) = delete;
    PacketPool& operator=(const PacketPool&) = delete;

    std::vector<AVPacket*> m_pool;
    std::mutex m_mutex;
};

struct PacketPoolDeleter {
    void operator()(AVPacket* packet) const noexcept {
        PacketPool::get().release(packet);
    }
};

using PacketHandle = std::unique_ptr<AVPacket, PacketPoolDeleter>;

[[nodiscard]] inline PacketHandle acquirePacketHandle() {
    return PacketHandle(PacketPool::get().acquire());
}

[[nodiscard]] inline PacketHandle clonePacketReference(const AVPacket* source) {
    if (!source) return {};

    PacketHandle clone = acquirePacketHandle();
    if (!clone || av_packet_ref(clone.get(), source) < 0) {
        return {};
    }
    return clone;
}

class Demuxer : public QThread
{
    Q_OBJECT
public:
    explicit Demuxer(QObject *parent = nullptr);
    ~Demuxer() override;

    [[nodiscard]] static bool init();
    static void deInit();

    void installVideoSocket(VideoSocket* videoSocket);
    // Kept for API compatibility; intentionally a no-op. The demuxer no
    // longer needs to be told the frame size up front - it learns the real
    // size itself from the stream's session packet (see sessionInfo()).
    void setFrameSize(const QSize &frameSize) { Q_UNUSED(frameSize); }

    // Video size from the most recently received session packet, or an
    // invalid QSize() if none has been received yet (i.e. before the first
    // frame is decodable).
    [[nodiscard]] QSize lastVideoSize() const { return m_lastVideoSize; }

    [[nodiscard]] bool startDecode();
    void stopDecode();

signals:
    void onStreamStop();
    void getFrame(AVPacket* packet);
    void getConfigFrame(AVPacket* packet);
    // Emitted whenever a "session packet" is received from the server
    // (scrcpy-server >= 4.0). This carries the actual video size and can be
    // emitted more than once during a connection (e.g. after a resize/reset
    // on the device side). `clientResized` indicates the resize was
    // initiated by the client itself (e.g. via RESIZE_DISPLAY).
    void sessionInfo(QSize size, bool clientResized);

protected:
    void run() override;

private:
    // Returns false on a fatal read error (caller should stop the loop).
    // On success, `isSession` tells the caller whether `packet` was filled
    // (media packet) or not (session packet, already handled/emitted here).
    bool processNetworkPacket(PacketHandle &packet, bool &isSession);
    void handleSessionHeader(const quint8 *header);
    qint32 recvData(quint8 *buf, qint32 bufSize);
    void logTelemetry() const;

private:
    QPointer<VideoSocket> m_videoSocket;
    std::vector<uint8_t> m_configBuffer;
    std::atomic<bool> m_isInterrupted{false};
    bool m_telemetryEnabled = false;
    QSize m_lastVideoSize;

    std::uint64_t m_packetCount = 0;
    std::uint64_t m_payloadBytes = 0;
    std::uint64_t m_configPacketCount = 0;
    std::uint64_t m_keyFrameCount = 0;
    std::uint64_t m_configPrependCount = 0;
    std::uint64_t m_interruptedReads = 0;
    std::uint64_t m_readFailures = 0;
    std::uint64_t m_invalidPackets = 0;
    std::uint64_t m_allocationFailures = 0;
    std::uint64_t m_sessionPacketCount = 0;
    std::uint32_t m_maxPayloadBytes = 0;
};

#endif // STREAM_H
