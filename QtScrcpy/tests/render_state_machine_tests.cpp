#include <QtTest>
#include <atomic>
#include <thread>

#include "pbomailboxstatemachine.h"

class RenderStateMachineTests final : public QObject
{
    Q_OBJECT

private slots:
    void freshMachineHasNoReadySlots();
    void publishedSlotBecomesReady();
    void consumerGetsNewestOfSeveralReady();
    void stalerReadySlotsAreReleasedAfterSelection();
    void producerReclaimsOldestReadyWhenNoFreeSlotExists();
    void producerReportsWhenItReclaimedAReadySlot();
    void producerBlocksWhenEverySlotIsProcessingOrWriting();
    void abandonedWriteReturnsSlotToFree();
    void processingSlotReturnsToFreeOnRelease();
    void resetReturnsEveryStateToFree();
    void concurrentProducerConsumerNeverLosesOrDuplicatesASlot();
};

void RenderStateMachineTests::freshMachineHasNoReadySlots()
{
    PboMailboxStateMachine<3> mailbox;
    QVERIFY(!mailbox.anyReady());
    QVERIFY(!mailbox.acquireNewestReady().has_value());
    for (std::size_t i = 0; i < 3; ++i) {
        QCOMPARE(mailbox.stateOf(i), PboMailboxStateMachine<3>::SlotState::Free);
    }
}

void RenderStateMachineTests::publishedSlotBecomesReady()
{
    PboMailboxStateMachine<3> mailbox;

    const auto index = mailbox.acquireWritable();
    QVERIFY(index.has_value());
    QCOMPARE(mailbox.stateOf(*index), PboMailboxStateMachine<3>::SlotState::Writing);

    mailbox.publishReady(*index);
    QCOMPARE(mailbox.stateOf(*index), PboMailboxStateMachine<3>::SlotState::Ready);
    QVERIFY(mailbox.anyReady());
}

void RenderStateMachineTests::consumerGetsNewestOfSeveralReady()
{
    PboMailboxStateMachine<3> mailbox;

    const auto first = mailbox.acquireWritable();
    mailbox.publishReady(*first);
    const auto second = mailbox.acquireWritable();
    mailbox.publishReady(*second);
    const auto third = mailbox.acquireWritable();
    mailbox.publishReady(*third);

    // All three slots are Ready; the consumer must always get the one that
    // was published most recently (highest sequence number), not simply
    // the first Free-turned-Ready slot found by index order.
    const auto selected = mailbox.acquireNewestReady();
    QVERIFY(selected.has_value());
    QCOMPARE(*selected, *third);
    QCOMPARE(mailbox.stateOf(*third), PboMailboxStateMachine<3>::SlotState::Processing);
}

void RenderStateMachineTests::stalerReadySlotsAreReleasedAfterSelection()
{
    PboMailboxStateMachine<3> mailbox;

    const auto first = mailbox.acquireWritable();
    mailbox.publishReady(*first);
    const auto second = mailbox.acquireWritable();
    mailbox.publishReady(*second);

    const auto selected = mailbox.acquireNewestReady();
    QVERIFY(selected.has_value());
    QCOMPARE(*selected, *second);

    const std::size_t dropped = mailbox.releaseStaleReady(*selected);
    QCOMPARE(dropped, std::size_t{1});
    QCOMPARE(mailbox.stateOf(*first), PboMailboxStateMachine<3>::SlotState::Free);
    // The selected slot itself must be left untouched (still Processing).
    QCOMPARE(mailbox.stateOf(*second), PboMailboxStateMachine<3>::SlotState::Processing);
}

void RenderStateMachineTests::producerReclaimsOldestReadyWhenNoFreeSlotExists()
{
    PboMailboxStateMachine<2> mailbox;

    const auto first = mailbox.acquireWritable();
    mailbox.publishReady(*first);
    const auto second = mailbox.acquireWritable();
    mailbox.publishReady(*second);
    // Both slots are now Ready; none Free.

    const auto third = mailbox.acquireWritable();
    QVERIFY(third.has_value());
    // Must have reclaimed the OLDEST ready slot (first), not the newest
    // one a consumer would otherwise still want to display.
    QCOMPARE(*third, *first);
    QCOMPARE(mailbox.stateOf(*first), PboMailboxStateMachine<2>::SlotState::Writing);
    QCOMPARE(mailbox.stateOf(*second), PboMailboxStateMachine<2>::SlotState::Ready);
}

void RenderStateMachineTests::producerReportsWhenItReclaimedAReadySlot()
{
    PboMailboxStateMachine<1> mailbox;

    bool reclaimed = true;
    const auto first = mailbox.acquireWritable(&reclaimed);
    QVERIFY(first.has_value());
    QVERIFY(!reclaimed); // slot was Free, nothing was reclaimed

    mailbox.publishReady(*first);

    const auto second = mailbox.acquireWritable(&reclaimed);
    QVERIFY(second.has_value());
    QVERIFY(reclaimed); // had to overwrite the still-unread Ready slot
}

void RenderStateMachineTests::producerBlocksWhenEverySlotIsProcessingOrWriting()
{
    PboMailboxStateMachine<1> mailbox;

    const auto writing = mailbox.acquireWritable();
    QVERIFY(writing.has_value());
    // Slot is Writing, not Ready - there is nothing safe to reclaim: the
    // producer must never be handed a slot that is already claimed by
    // someone else (that would be the exact "double-claimed slot" bug
    // class this state machine exists to make impossible).
    QVERIFY(!mailbox.acquireWritable().has_value());

    mailbox.publishReady(*writing);
    const auto processing = mailbox.acquireNewestReady();
    QVERIFY(processing.has_value());
    // Now Processing - still not reclaimable by the producer.
    QVERIFY(!mailbox.acquireWritable().has_value());
}

void RenderStateMachineTests::abandonedWriteReturnsSlotToFree()
{
    PboMailboxStateMachine<1> mailbox;

    const auto index = mailbox.acquireWritable();
    QVERIFY(index.has_value());
    mailbox.abandonWrite(*index);
    QCOMPARE(mailbox.stateOf(*index), PboMailboxStateMachine<1>::SlotState::Free);

    // The slot must be immediately reusable.
    const auto again = mailbox.acquireWritable();
    QVERIFY(again.has_value());
    QCOMPARE(*again, *index);
}

void RenderStateMachineTests::processingSlotReturnsToFreeOnRelease()
{
    PboMailboxStateMachine<1> mailbox;

    const auto index = mailbox.acquireWritable();
    mailbox.publishReady(*index);
    const auto selected = mailbox.acquireNewestReady();
    QVERIFY(selected.has_value());

    mailbox.releaseProcessing(*selected);
    QCOMPARE(mailbox.stateOf(*selected), PboMailboxStateMachine<1>::SlotState::Free);

    // Calling it again (e.g. a second, redundant fence-signal callback)
    // must be a harmless no-op, not corrupt an unrelated later write.
    mailbox.releaseProcessing(*selected);
    QCOMPARE(mailbox.stateOf(*selected), PboMailboxStateMachine<1>::SlotState::Free);
}

void RenderStateMachineTests::resetReturnsEveryStateToFree()
{
    PboMailboxStateMachine<3> mailbox;

    const auto a = mailbox.acquireWritable();
    mailbox.publishReady(*a);
    const auto b = mailbox.acquireWritable();
    mailbox.publishReady(*b);
    const auto processingSlot = mailbox.acquireNewestReady(); // moves one slot into Processing
    QVERIFY(processingSlot.has_value());

    mailbox.reset();

    for (std::size_t i = 0; i < 3; ++i) {
        QCOMPARE(mailbox.stateOf(i), PboMailboxStateMachine<3>::SlotState::Free);
    }
    QVERIFY(!mailbox.anyReady());
}

void RenderStateMachineTests::concurrentProducerConsumerNeverLosesOrDuplicatesASlot()
{
    // This is the scenario the class exists for: one producer thread
    // (the decoder) and one consumer thread (the GL render thread)
    // hammering the same 3-slot mailbox concurrently, the way they
    // actually do in QYuvOpenGLWidget. The invariant under test is
    // structural, not timing-dependent: no two "owners" (producer/
    // consumer) can ever believe they hold the same slot index at the
    // same time. We assert that by having each side record, for every
    // slot it acquires, a per-slot "currently owned by me" flag and
    // failing loudly if it was already set.
    constexpr std::size_t kSlots = 3;
    PboMailboxStateMachine<kSlots> mailbox;

    std::array<std::atomic_bool, kSlots> ownedByProducer{};
    std::array<std::atomic_bool, kSlots> ownedByConsumer{};
    for (auto &flag : ownedByProducer) flag.store(false);
    for (auto &flag : ownedByConsumer) flag.store(false);

    std::atomic_bool stop{false};
    std::atomic_bool sawDoubleOwnership{false};
    std::atomic<std::uint64_t> producedCount{0};
    std::atomic<std::uint64_t> consumedCount{0};

    std::thread producer([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            const auto index = mailbox.acquireWritable();
            if (!index.has_value()) continue;

            if (ownedByProducer[*index].exchange(true, std::memory_order_acq_rel) ||
                ownedByConsumer[*index].load(std::memory_order_acquire)) {
                sawDoubleOwnership.store(true, std::memory_order_relaxed);
            }

            // Simulate doing a little work while "holding" the slot.
            std::this_thread::yield();

            ownedByProducer[*index].store(false, std::memory_order_release);
            mailbox.publishReady(*index);
            producedCount.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread consumer([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            const auto index = mailbox.acquireNewestReady();
            if (!index.has_value()) continue;

            mailbox.releaseStaleReady(*index);

            if (ownedByConsumer[*index].exchange(true, std::memory_order_acq_rel) ||
                ownedByProducer[*index].load(std::memory_order_acquire)) {
                sawDoubleOwnership.store(true, std::memory_order_relaxed);
            }

            std::this_thread::yield();

            ownedByConsumer[*index].store(false, std::memory_order_release);
            mailbox.releaseProcessing(*index);
            consumedCount.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true, std::memory_order_relaxed);
    producer.join();
    consumer.join();

    QVERIFY(!sawDoubleOwnership.load());
    // Sanity: the threads actually got to run a meaningful number of
    // iterations, so the invariant check above was actually exercised.
    QVERIFY(producedCount.load() > 100);
    QVERIFY(consumedCount.load() > 0);
}

QTEST_APPLESS_MAIN(RenderStateMachineTests)
#include "render_state_machine_tests.moc"
