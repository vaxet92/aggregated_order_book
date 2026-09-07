#pragma once

// SpscQueue: a fixed-capacity, lock-free single-producer/single-consumer ring
// buffer over acquire/release atomics. Generic and policy-free - it does not
// block, retry, or decide what to do when full; that is left entirely to the
// caller. Used as the provider -> consolidator handoff (ProviderQueue in
// provider_message.h).

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <type_traits>

namespace market_data {

// Single-producer, single-consumer ring buffer. One producer thread calls TryPush, one consumer thread calls
// TryPop - never the other way round, and never two threads on the same
// side. That restriction is what lets both sides run with no lock and no
// compare-and-swap: each side owns one counter outright and only ever reads
// the other side's.
//
// head_ is the PRODUCER's cursor (next slot to write). tail_ is the
// CONSUMER's cursor (next slot to read). Both count up forever and are only
// ever wrapped into the array with `& kMask` at the point of use - so "full"
// and "empty" are a plain integer comparison on the two cursors, never a
// special case on the wrapped index.
//
// TryPush does NOT block when full. Blocking (spin, yield, or condition
// variable) is a POLICY choice - whether to wait, how long, whether to give
// up - and this class makes none of those choices. The caller decides. On
// this project's provider->core path the rule is "block, never drop"
// (sequenced deltas - dropping one breaks continuity for every later one),
// enforced by the caller looping on TryPush, not by this class.
template <typename ValueType, std::size_t Capacity>
class SpscQueue {
   public:
    static_assert(std::has_single_bit(Capacity), "Capacity must be a power of two");
    // TryPush/TryPop are noexcept, so a throwing move would call
    // std::terminate instead of propagating - catch that at compile time
    // instead of at a live crash.
    static_assert(std::is_nothrow_move_assignable_v<ValueType>,
                  "SpscQueue moves ValueType under noexcept - it must not throw on move");

    // Producer only. False means the queue is full right now.
    bool TryPush(const ValueType& value) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = head + 1;

        // Acquire: must see every slot the consumer has already freed by the
        // time we decide there is room, or we could overwrite one it is
        // still reading.
        if (next - tail_.load(std::memory_order_acquire) > Capacity) {
            return false;
        }

        buffer_[head & kMask] = value;

        // Release: publishes the write above. The consumer's matching
        // acquire load of head_ is what guarantees it never observes a
        // slot before this write has actually landed.
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool TryPush(ValueType&& value) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = head + 1;

        if (next - tail_.load(std::memory_order_acquire) > Capacity) {
            return false;
        }

        // Move, not copy: ValueType is ProviderMessage in practice, and a
        // copy of a BookUpdate alternative would copy two std::vectors -
        // exactly the allocation this queue exists to avoid on the hot path.
        buffer_[head & kMask] = std::move(value);

        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer only. False means nothing is available right now.
    bool TryPop(ValueType& value) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);

        // Acquire: must see the producer's buffer_ write, not just its
        // incremented head_, or this could read a half-written slot.
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }

        value = std::move(buffer_[tail & kMask]);

        // Release: tells the producer this slot is free to reuse. Must come
        // AFTER the read above, or the producer could overwrite the slot
        // while it is still being read out.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Point-in-time hints for logging/instrumentation - by the time either
    // returns, the real answer may already have changed on the other
    // thread. Never used to decide correctness, only to observe it (e.g.
    // confirming this queue stays near size 1 in production).
    bool Empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    bool Full() const noexcept {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire) == Capacity;
    }

   private:
    static constexpr std::size_t kMask = Capacity - 1;

    // Each on its own cache line. head_ and tail_ are written by different
    // threads on every single call; sharing a line would make every push
    // invalidate the line the consumer is polling, and vice versa - real
    // cross-core traffic neither side's own work needs. buffer_ gets the
    // same treatment so its first element does not land on tail_'s line.
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) std::array<ValueType, Capacity> buffer_{};
};

}  // namespace market_data
