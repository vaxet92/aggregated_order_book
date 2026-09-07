#include <gtest/gtest.h>

#include "md_core/spsc_queue.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace market_data;

namespace {

// Generous but finite. A hang here means a real correctness bug (a lost
// wakeup, a stuck full/empty check) - failing loudly after a bound beats
// stalling the whole test suite forever.
constexpr auto kStressTimeout = std::chrono::seconds(10);

}  // namespace

// --- single-threaded: order, capacity, wraparound ---------------------------

TEST(SpscQueueTest, StartsEmpty) {
    SpscQueue<int, 4> queue;
    EXPECT_TRUE(queue.Empty());
    EXPECT_FALSE(queue.Full());
}

TEST(SpscQueueTest, PushThenPopPreservesOrder) {
    SpscQueue<int, 4> queue;
    ASSERT_TRUE(queue.TryPush(10));
    ASSERT_TRUE(queue.TryPush(20));
    ASSERT_TRUE(queue.TryPush(30));

    int value = 0;
    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value, 10);
    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value, 20);
    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value, 30);
}

TEST(SpscQueueTest, PopOnEmptyReturnsFalse) {
    SpscQueue<int, 4> queue;
    int value = -1;
    EXPECT_FALSE(queue.TryPop(value));
    EXPECT_EQ(value, -1);  // untouched on failure
}

// Capacity 4 holds exactly 4 unread items - the 5th push must be rejected
// before it touches the buffer, not after.
TEST(SpscQueueTest, FullAtExactlyCapacityItems) {
    SpscQueue<int, 4> queue;
    EXPECT_TRUE(queue.TryPush(1));
    EXPECT_TRUE(queue.TryPush(2));
    EXPECT_TRUE(queue.TryPush(3));
    EXPECT_TRUE(queue.TryPush(4));
    EXPECT_TRUE(queue.Full());

    EXPECT_FALSE(queue.TryPush(5));  // rejected, not silently dropped-and-lost

    int value = 0;
    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value, 1);  // the rejected push never overwrote slot 0
}

// Exercises the index actually wrapping (`& kMask`) across the array
// boundary, not just the head_/tail_ counters growing - the trace this
// mirrors is in spsc_queue.h's class comment.
TEST(SpscQueueTest, WrapsAroundPastCapacity) {
    SpscQueue<int, 4> queue;
    int value = 0;

    ASSERT_TRUE(queue.TryPush(1));
    ASSERT_TRUE(queue.TryPush(2));
    ASSERT_TRUE(queue.TryPush(3));
    ASSERT_TRUE(queue.TryPush(4));

    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value, 1);  // frees slot 0

    ASSERT_TRUE(queue.TryPush(5));  // reuses slot 0 (head_=4 -> 4 & 3 == 0)
    EXPECT_TRUE(queue.Full());

    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value, 2);
    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value, 3);
    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value, 4);
    ASSERT_TRUE(queue.TryPop(value));
    EXPECT_EQ(value, 5);
    EXPECT_TRUE(queue.Empty());
}

// --- move vs. copy ------------------------------------------------------

namespace {

// Counts which path ran, so the test can assert on behaviour instead of
// reading the source. Copy is left working (SpscQueue's const& overload
// needs it to compile) but the queue must never take it when an rvalue is
// available - that copy would be the exact per-message allocation the
// design exists to avoid (see spsc_queue.h's comment on the && overload).
struct CopyMoveCounter {
    static inline int copies = 0;
    static inline int moves = 0;

    int value = 0;

    CopyMoveCounter() = default;
    explicit CopyMoveCounter(int v) : value(v) {}

    CopyMoveCounter(const CopyMoveCounter& other) : value(other.value) { ++copies; }
    CopyMoveCounter& operator=(const CopyMoveCounter& other) {
        value = other.value;
        ++copies;
        return *this;
    }

    CopyMoveCounter(CopyMoveCounter&& other) noexcept : value(other.value) { ++moves; }
    CopyMoveCounter& operator=(CopyMoveCounter&& other) noexcept {
        value = other.value;
        ++moves;
        return *this;
    }
};

}  // namespace

TEST(SpscQueueTest, PushingAnRvalueMovesRatherThanCopies) {
    CopyMoveCounter::copies = 0;
    CopyMoveCounter::moves = 0;

    SpscQueue<CopyMoveCounter, 4> queue;
    ASSERT_TRUE(queue.TryPush(CopyMoveCounter(42)));  // rvalue in

    CopyMoveCounter out;
    ASSERT_TRUE(queue.TryPop(out));

    EXPECT_EQ(out.value, 42);
    EXPECT_EQ(CopyMoveCounter::copies, 0);
    EXPECT_GE(CopyMoveCounter::moves, 2);  // into the slot, then out of it
}

// The property Core's bounded retry loop depends on. TryPush checks for
// space and returns BEFORE touching the value, so a rejected push leaves the
// caller's object intact and the next attempt re-sends the same message
// rather than an empty husk.
//
// this is asserted here rather than trusted, because the retry's
// correctness otherwise rests on statement order inside another function -
// swap the capacity check below the move and every overflow would enqueue a
// default-constructed message, silently.
TEST(SpscQueueTest, TryPushOnFullDoesNotConsumeTheValue) {
    SpscQueue<CopyMoveCounter, 2> queue;
    ASSERT_TRUE(queue.TryPush(CopyMoveCounter(1)));
    ASSERT_TRUE(queue.TryPush(CopyMoveCounter(2)));
    ASSERT_TRUE(queue.Full());

    CopyMoveCounter rejected(99);
    CopyMoveCounter::copies = 0;
    CopyMoveCounter::moves = 0;

    EXPECT_FALSE(queue.TryPush(std::move(rejected)));

    EXPECT_EQ(CopyMoveCounter::moves, 0) << "a rejected push must not move from the value";
    EXPECT_EQ(CopyMoveCounter::copies, 0) << "a rejected push must not copy the value either";
    EXPECT_EQ(rejected.value, 99) << "value must survive so the caller can retry with it";
}

// The retry itself: the same object, rejected once, must arrive intact once
// the consumer frees a slot. This is Core::TryEnqueueBounded in miniature.
TEST(SpscQueueTest, RetryingARejectedPushDeliversTheOriginalValue) {
    SpscQueue<CopyMoveCounter, 2> queue;
    ASSERT_TRUE(queue.TryPush(CopyMoveCounter(1)));
    ASSERT_TRUE(queue.TryPush(CopyMoveCounter(2)));

    CopyMoveCounter pending(42);
    ASSERT_FALSE(queue.TryPush(std::move(pending)));  // full - rejected

    CopyMoveCounter drained;
    ASSERT_TRUE(queue.TryPop(drained));              // make room
    ASSERT_TRUE(queue.TryPush(std::move(pending)));  // same object, retried

    ASSERT_TRUE(queue.TryPop(drained));
    EXPECT_EQ(drained.value, 2);
    ASSERT_TRUE(queue.TryPop(drained));
    EXPECT_EQ(drained.value, 42) << "the retried message, not a default-constructed one";
}

// --- real concurrency: one producer thread, one consumer thread -------------

TEST(SpscQueueTest, ConcurrentProducerConsumerPreservesOrderAndCount) {
    constexpr int kCount = 20'000;
    SpscQueue<int, 64> queue;

    std::thread producer([&queue] {
        for (int i = 0; i < kCount; ++i) {
            while (!queue.TryPush(i)) {
                std::this_thread::yield();  // full - the design's own backoff, block never drop
            }
        }
    });

    std::vector<int> received;
    received.reserve(kCount);
    const auto deadline = std::chrono::steady_clock::now() + kStressTimeout;
    while (static_cast<int>(received.size()) < kCount) {
        int value = 0;
        if (queue.TryPop(value)) {
            received.push_back(value);
        } else {
            ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "consumer stalled - possible lost wakeup/race";
            std::this_thread::yield();
        }
    }
    producer.join();

    ASSERT_EQ(received.size(), static_cast<size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(received[static_cast<size_t>(i)], i) << "order broken at index " << i;
    }
}

// --- stress: small capacity forces constant full/empty cycling, a
// heap-owning payload makes a use-after-move or a torn read visible as
// wrong data rather than just wrong order ------------------------------------

namespace {

struct StressPayload {
    int id = 0;
    std::vector<int> data;  // heap allocation: a moved-from/racy read shows up as empty or wrong, not just "some int"
};

}  // namespace

TEST(SpscQueueTest, StressSmallCapacityHighVolumeVectorPayload) {
    constexpr int kCount = 500'000;
    // Capacity 8 against 500k items forces tens of thousands of Full()
    // rejections on the producer side - the path the smaller correctness
    // test above barely exercises.
    SpscQueue<StressPayload, 8> queue;

    std::atomic<bool> producer_failed{false};

    std::thread producer([&queue, &producer_failed] {
        for (int i = 0; i < kCount && !producer_failed.load(std::memory_order_relaxed); ++i) {
            StressPayload payload;
            payload.id = i;
            payload.data = std::vector<int>(4, i);
            while (!queue.TryPush(std::move(payload))) {
                std::this_thread::yield();
            }
        }
    });

    int expected_id = 0;
    const auto deadline = std::chrono::steady_clock::now() + kStressTimeout;
    while (expected_id < kCount) {
        StressPayload out;
        if (queue.TryPop(out)) {
            ASSERT_EQ(out.id, expected_id) << "order broken at index " << expected_id;
            ASSERT_EQ(out.data, (std::vector<int>(4, expected_id)))
                << "payload content wrong at index " << expected_id << " - use-after-move or torn read";
            ++expected_id;
        } else {
            if (std::chrono::steady_clock::now() >= deadline) {
                producer_failed.store(true, std::memory_order_relaxed);
                FAIL() << "consumer stalled at index " << expected_id << " of " << kCount;
                break;
            }
            std::this_thread::yield();
        }
    }
    producer.join();
}
