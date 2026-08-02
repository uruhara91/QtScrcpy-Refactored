#ifndef PBOMAILBOXSTATEMACHINE_H
#define PBOMAILBOXSTATEMACHINE_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

// ---------------------------------------------------------------------
// PboMailboxStateMachine
// ---------------------------------------------------------------------
// Pure bookkeeping for an N-slot "mailbox" (Vulkan-swapchain-style) frame
// hand-off between a producer thread (the decoder, writing new frames) and
// a consumer thread (the GL render thread, reading the newest one). It
// owns no GPU resources whatsoever - no PBOs, no textures, no GL calls of
// any kind - it only tracks which of the N slots is Free / being Written /
// Ready-to-read / being Processed (read by the GPU), plus a monotonic
// sequence number per slot so the consumer can always find the *newest*
// ready slot and the producer can always reclaim the *oldest* one.
//
// This was deliberately extracted out of QYuvOpenGLWidget (which pairs
// each slot with real PBO ids / mapped pointers / a GL fence) so the
// concurrency logic itself - the part that is actually easy to get subtly
// wrong (ABA races, double-claiming a slot, losing the newest frame to a
// stale read) - can be exercised with plain QtTest unit tests that need no
// OpenGL context, GPU, or display at all. See
// tests/render_state_machine_tests.cpp.
//
// Thread-safety: every method is safe to call concurrently from one
// producer thread and one consumer thread (that is the only usage pattern
// this class supports; it is not a general multi-producer/multi-consumer
// structure).
template <std::size_t SlotCount>
class PboMailboxStateMachine
{
public:
    enum class SlotState : int {
        Free = 0,       // available for the producer to claim
        Writing = 1,    // producer is currently filling this slot
        Ready = 2,      // filled, waiting to be picked up by the consumer
        Processing = 3, // consumer claimed it; GPU may still be reading it
    };

    static_assert(SlotCount > 0, "PboMailboxStateMachine needs at least one slot");

    PboMailboxStateMachine() { reset(); }

    // Resets every slot to Free and the sequence counter to zero. Only
    // safe to call when the producer/consumer are both idle (e.g. while
    // tearing down or reallocating backing PBOs on a resolution change).
    void reset() noexcept
    {
        for (auto &slot : m_slots) {
            slot.state.store(SlotState::Free, std::memory_order_relaxed);
            slot.sequence.store(0, std::memory_order_relaxed);
        }
        m_globalSequence.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] SlotState stateOf(std::size_t index) const noexcept
    {
        return m_slots[index].state.load(std::memory_order_acquire);
    }

    // Producer side -----------------------------------------------------

    // Claims a slot to write a new frame into: prefers a genuinely Free
    // slot, and if none exists, reclaims the oldest Ready slot (the mailbox
    // policy - an unconsumed frame may be silently overwritten by a newer
    // one). Returns std::nullopt only if every slot is Writing/Processing,
    // meaning the consumer has fallen fully behind.
    [[nodiscard]] std::optional<std::size_t> acquireWritable(bool *reclaimedReady = nullptr) noexcept
    {
        if (reclaimedReady) *reclaimedReady = false;

        for (std::size_t index = 0; index < SlotCount; ++index) {
            SlotState expected = SlotState::Free;
            if (m_slots[index].state.compare_exchange_strong(
                    expected, SlotState::Writing,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return index;
            }
        }

        for (int attempt = 0; attempt < static_cast<int>(SlotCount); ++attempt) {
            std::size_t oldestIndex = SlotCount;
            std::uint64_t oldestSequence = std::numeric_limits<std::uint64_t>::max();

            for (std::size_t index = 0; index < SlotCount; ++index) {
                if (m_slots[index].state.load(std::memory_order_acquire) == SlotState::Ready) {
                    const auto sequence = m_slots[index].sequence.load(std::memory_order_acquire);
                    if (sequence < oldestSequence) {
                        oldestSequence = sequence;
                        oldestIndex = index;
                    }
                }
            }

            if (oldestIndex == SlotCount) return std::nullopt;

            SlotState expected = SlotState::Ready;
            if (m_slots[oldestIndex].state.compare_exchange_strong(
                    expected, SlotState::Writing,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                if (reclaimedReady) *reclaimedReady = true;
                return oldestIndex;
            }
            // Lost the race (consumer grabbed it in acquireNewestReady()
            // between our load and CAS) - loop and look again.
        }

        return std::nullopt;
    }

    // Publishes a Writing slot as Ready with a fresh monotonic sequence
    // number, making it visible to the consumer.
    void publishReady(std::size_t index) noexcept
    {
        const auto sequence = m_globalSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        m_slots[index].sequence.store(sequence, std::memory_order_relaxed);
        m_slots[index].state.store(SlotState::Ready, std::memory_order_release);
    }

    // Releases a Writing slot back to Free without publishing it (used
    // when the producer aborts a write, e.g. the destination pointer
    // turned out to be invalid).
    void abandonWrite(std::size_t index) noexcept
    {
        m_slots[index].state.store(SlotState::Free, std::memory_order_release);
    }

    // Consumer side -------------------------------------------------------

    // Claims the newest Ready slot for reading/upload, transitioning it to
    // Processing. Returns std::nullopt if nothing is Ready.
    [[nodiscard]] std::optional<std::size_t> acquireNewestReady() noexcept
    {
        for (int attempt = 0; attempt < static_cast<int>(SlotCount); ++attempt) {
            std::size_t newestIndex = SlotCount;
            std::uint64_t newestSequence = 0;

            for (std::size_t index = 0; index < SlotCount; ++index) {
                if (m_slots[index].state.load(std::memory_order_acquire) == SlotState::Ready) {
                    const auto sequence = m_slots[index].sequence.load(std::memory_order_acquire);
                    if (newestIndex == SlotCount || sequence >= newestSequence) {
                        newestSequence = sequence;
                        newestIndex = index;
                    }
                }
            }

            if (newestIndex == SlotCount) return std::nullopt;

            SlotState expected = SlotState::Ready;
            if (m_slots[newestIndex].state.compare_exchange_strong(
                    expected, SlotState::Processing,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return newestIndex;
            }
        }

        return std::nullopt;
    }

    // Frees every Ready slot other than `selectedIndex` (frames made stale
    // by a newer frame arriving before they were ever displayed). Returns
    // how many slots were dropped, for telemetry.
    std::size_t releaseStaleReady(std::size_t selectedIndex) noexcept
    {
        std::size_t dropped = 0;
        for (std::size_t index = 0; index < SlotCount; ++index) {
            if (index == selectedIndex) continue;
            SlotState expected = SlotState::Ready;
            if (m_slots[index].state.compare_exchange_strong(
                    expected, SlotState::Free,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                ++dropped;
            }
        }
        return dropped;
    }

    // Releases a Processing slot back to Free once the GPU (or, in the
    // classic non-DSA path, the render-thread-side upload) is done reading
    // it. Safe to call unconditionally; a no-op if the slot isn't
    // Processing.
    void releaseProcessing(std::size_t index) noexcept
    {
        SlotState expected = SlotState::Processing;
        m_slots[index].state.compare_exchange_strong(
            expected, SlotState::Free,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    [[nodiscard]] bool anyReady() const noexcept
    {
        for (const auto &slot : m_slots) {
            if (slot.state.load(std::memory_order_acquire) == SlotState::Ready) return true;
        }
        return false;
    }

private:
    struct alignas(64) Slot {
        std::atomic<SlotState> state{SlotState::Free};
        std::atomic<std::uint64_t> sequence{0};
    };

    std::array<Slot, SlotCount> m_slots{};
    std::atomic<std::uint64_t> m_globalSequence{0};
};

#endif // PBOMAILBOXSTATEMACHINE_H
