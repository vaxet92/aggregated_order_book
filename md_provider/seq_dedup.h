#pragma once

// SeqDedup: duplicate rejection for redundant WebSocket connections. One
// monotonic high-water-mark compare per message (18 bytes of state) rejects
// every copy but the first, sitting between parse and the continuity check so
// a duplicate never gets mistaken for a sequence gap. One instance per
// (venue, stream).

#include <bit>
#include <cstdint>

namespace market_data {

// Duplicate rejection for redundant WebSocket connections ("line arbitration").
//
// With "connections": N in server_config_<market>.json, N sockets carry the
// SAME messages. Exactly one copy must reach the book; the other N-1 must be
// dropped. The benefit is failover: when one socket dies the others are
// already delivering, so there is no gap, no resync and no REST refetch.
//
// Where this sits:
//
//     N sockets -> parse -> SeqDedup -> continuity check -> Emit -> Core
//
// AFTER parse, because is_reset needs the parsed message.
//
// BEFORE the continuity check. Continuity is a state machine keyed on the
// last sequence number - a duplicate reaching CheckBinanceSpotContinuity looks
// like U != last_u + 1, which it reports as a gap and resyncs on. Redundant
// connections would then cause the exact outage they were added to prevent.
//
// One instance per (venue, stream). Six in total: three venues x depth + BBO.
// Never shared between streams - Binance depth `u` and bookTicker `u` are
// different id spaces, and comparing across them is meaningless.
//
// Not thread-safe, and does not need to be: all N sessions of a provider are
// read by one io_context on one thread, so every call is serialized.
class SeqDedup {
   public:
    // Consecutive drops before we suspect a missed reset rather than normal
    // duplicates. With N connections, normal operation never exceeds N-1 in a
    // row. 1000 is far above that and far below "we lost the whole session".
    static constexpr uint32_t kSuspiciousDropStreak = 1000;

    SeqDedup() = default;

    // True  -> first copy of this message, pass it downstream.
    // False -> already seen, drop it.
    //
    // `conn_index` is which socket delivered it, 0-based, < kMaxConnections.
    // Used only for health reporting, never for the accept/reject decision.
    //
    // `is_reset` means the venue restarted its sequence: a Bybit snapshot
    // (including the u == 1 service restart, which the parser normalises into
    // is_snapshot) or OKX's documented maintenance reset where seqId jumps
    // backwards while prevSeqId still chains. Binance never resets on the
    // stream, so it always passes false.
    bool Accept(uint64_t id, uint32_t conn_index, bool is_reset);

    // Bitmask of the connections that delivered the PREVIOUS id: bit i set
    // means connection i produced a copy of it. Health telemetry only.
    //   0b111 -> all three connections healthy
    //   0b110 -> connection 0 missed it or is behind
    // Reported one message late by construction - a message's copies are only
    // all counted once the next id arrives.
    uint8_t LastMask() const { return last_mask_; }

    // How many connections delivered the previous id.
    uint32_t LastConnectionCount() const { return static_cast<uint32_t>(std::popcount(last_mask_)); }

    // Drops since the last accepted message. See kSuspiciousDropStreak: a
    // large value means we are rejecting everything, which is what a missed
    // reset looks like from in here.
    uint32_t ConsecutiveDrops() const { return consecutive_drops_; }

    // the silent failure this guards. If a venue resets its sequence and
    // is_reset is NOT passed, every following id sits below last_, so every
    // message is dropped forever. The book freezes and nothing errors, because
    // the filter is doing exactly what it was told. The caller polls this and
    // logs loudly.
    bool LooksStuck() const { return consecutive_drops_ >= kSuspiciousDropStreak; }

   private:
    // Accept a new id: publish the mask for the id being left behind, and
    // start a fresh one for this message.
    void Rotate(uint64_t id, uint8_t bit);

    uint64_t last_ = 0;      // high-water mark: anything <= this was already taken
    bool seen_ = false;      // nothing accepted yet - last_ is not yet meaningful
    uint8_t seen_mask_ = 0;  // connections that have delivered last_ so far
    uint8_t last_mask_ = 0;  // ... and for the id before it, once rotated
    uint32_t consecutive_drops_ = 0;
};

}  // namespace market_data
