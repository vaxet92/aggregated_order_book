#include "seq_dedup.h"

namespace market_data {

bool SeqDedup::Accept(uint64_t id, uint32_t conn_index, bool is_reset) {
    // Health is allowed to be approximate; undefined behaviour is not. An
    // out-of-range index contributes no bit rather than shifting past the
    // width of the mask. Config validation caps "connections" at
    // kMaxConnections (8), so this should be unreachable.
    const uint8_t bit = conn_index < 8 ? static_cast<uint8_t>(1u << conn_index) : 0;

    // A reset makes every earlier id meaningless: the venue restarted its
    // sequence, so `id` may legitimately be far BELOW last_.
    //
    // the other N-1 connections deliver this same reset moments later,
    // and are caught by `id == last_`. Clearing the state instead of moving
    // it down would let each of them through, and every one would re-apply a
    // full snapshot.
    if (is_reset) {
        if (seen_ && id == last_) {
            seen_mask_ |= bit;
            ++consecutive_drops_;
            return false;
        }
        Rotate(id, bit);
        return true;
    }

    // Anything at or below the high-water mark was already taken. `<=` rather
    // than `!=` because a connection can lag by more than one message: one
    // io_context thread reads all N sockets and processes whatever is ready,
    // so conn0 delivering 4057 AND 4058 before conn1 delivers 4057 is normal
    // event-loop batching, not a rare race.
    if (seen_ && id <= last_) {
        seen_mask_ |= bit;
        ++consecutive_drops_;
        return false;
    }

    Rotate(id, bit);
    return true;
}

void SeqDedup::Rotate(uint64_t id, uint8_t bit) {
    // seen_mask_ describes the id being left behind - publish it before
    // starting a new one. Health is therefore always one message late, which
    // is unavoidable: a message's copies are only all counted once the next
    // id proves no more are coming.
    last_mask_ = seen_mask_;
    seen_mask_ = bit;
    last_ = id;
    seen_ = true;
    consecutive_drops_ = 0;
}

}  // namespace market_data
