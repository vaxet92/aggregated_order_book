#pragma once

// ConflatedChannel: a depth-1 pending slot with overwrite semantics - the
// mechanism behind every gRPC session's conflation. A new push replaces an
// unread value rather than queuing, so a slow client cannot back-pressure the
// publish path; WaitAndTake's bounded timeout is what lets a dead session be
// noticed without adding any latency to a live one.

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

namespace market_data {

// Depth-1 pending slot with overwrite semantics: if a new
// value arrives while the previous one hasn't been read yet, it replaces
// it - nothing ever queues, nothing upstream ever blocks on a slow reader.
template <typename T>
class ConflatedChannel {
   public:
    // Producer side (e.g. Core, after computing a new ConsolidatedBBO).
    // Never blocks.
    void Push(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = std::move(value);
        }
        cv_.notify_one();
    }

    // Consumer side (one gRPC session's handler thread). Returns as soon as a
    // value is available, and after `timeout` at the latest, clearing the slot.
    //
    // std::nullopt has TWO meanings, and the caller must tell them apart with
    // IsClosed(): the channel was closed (stop), or the timeout expired with
    // nothing pending (keep waiting). Treating a timeout as "closed" would
    // drop a healthy session after `timeout` of quiet market.
    //
    // the timeout adds NO latency to updates - Push() still wakes this
    // wait immediately. It only bounds how long the consumer may stay parked
    // without re-checking whether its client is still there. Without that
    // bound, a client that disconnects while its channel is idle leaves this
    // thread blocked forever: grpc::ClientContext::TryCancel() sets a flag in
    // gRPC, it cannot wake a thread parked on OUR condition variable.
    std::optional<T> WaitAndTake(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, timeout, [this] { return pending_.has_value() || closed_; });
        if (!pending_) {
            return std::nullopt;  // closed_, or the timeout expired
        }
        T value = std::move(*pending_);
        pending_.reset();
        return value;
    }

    // Wakes any thread blocked in WaitAndTake() so it can exit - used when
    // a session is being torn down (server shutdown, forced disconnect).
    void Close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_one();
    }

    // Only ever goes false -> true, so a caller that reads this AFTER a
    // nullopt cannot be fooled: a Close() landing in between just makes the
    // consumer exit one iteration earlier than it strictly had to.
    bool IsClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

   private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<T> pending_;
    bool closed_ = false;
};

}  // namespace market_data
