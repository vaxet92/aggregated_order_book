#pragma once

// Staleness policy: the VenueHealth enum, the pure classifier functions
// (ClassifyVenue / ClassifyFeed) that turn a last-message timestamp and a
// backstop into a verdict, and VenueHealthEvent - the POD a provider pushes
// to Core when its own verdict changes. No clock is read here; every function
// takes `now` as a parameter, which is what keeps md_core I/O-free.

#include <array>
#include <cstdint>

#include "types/venue.h"
#include "types/venue_registry.h"

namespace market_data {

// Staleness classification for one venue's feed.
//
// Why this exists at all: the merge takes max(bid) and min(ask). A frozen
// venue never moves, so when the market falls the frozen venue ALWAYS looks
// like the best bid, and when it rises it ALWAYS looks like the best ask.
//
// staleness is not random error that averages out - the merge actively
// selects for it. One stale venue out of three corrupts the output nearly
// every time the market moves, not one third of the time. That is why a
// stale venue must be excluded from the merge rather than merely reported.
enum class VenueHealth : uint8_t {
    kNoData,
    kLive,
    kDisconnected,
    kResyncing,
    kStale,
};

// Pure function: reads no clock, holds no state. The caller supplies `now`,
// which is what keeps md_core free of I/O and makes every test three
// integers and an expected enum - no sleeping, no injected fake clock.
constexpr VenueHealth ClassifyVenue(int64_t last_mono_ns, int64_t now_mono_ns, int64_t stale_after_ns) {
    // Checked FIRST, before any arithmetic. A venue that never spoke has no
    // age to compute - without this, `now - 0` reports the process uptime as
    // if it were the feed's age, which is meaningless and always huge enough
    // to look stale.
    if (last_mono_ns == 0) {
        return VenueHealth::kNoData;
    }

    // Strictly `>`: a venue exactly at the threshold is still live. Arbitrary
    // either way, but it has to be decided once and tested rather than left
    // to whatever the code happens to do.
    //
    // A NEGATIVE age is handled for free and correctly. Two threads reading
    // steady_clock can produce a `now` marginally older than `last`; the
    // subtraction goes negative, the comparison is false, and the answer is
    // kLive - which is right, since a message from the immediate future is
    // not stale.
    if (now_mono_ns - last_mono_ns > stale_after_ns) {
        return VenueHealth::kStale;
    }

    return VenueHealth::kLive;
}

// Only a live venue may contribute to the consolidated output. kNoData is
// excluded too, though its book is empty anyway - the distinction is carried
// for reporting, not for the merge.
constexpr bool IsAdmissible(VenueHealth health) {
    return health == VenueHealth::kLive;
}

// For logs and, later, the wire's venue_status block. Returns a literal, not
// a std::string: this is called from log lines on the provider thread and has
// no business allocating.
constexpr const char* ToString(VenueHealth health) {
    switch (health) {
        case VenueHealth::kNoData:
            return "NO_DATA";
        case VenueHealth::kLive:
            return "LIVE";
        case VenueHealth::kDisconnected:
            return "DISCONNECTED";
        case VenueHealth::kResyncing:
            return "RESYNCING";
        case VenueHealth::kStale:
            return "STALE";
    }
    return "UNKNOWN";
}

// One health verdict per venue, indexed by VenueId - the same shape as
// MapOrderBookArray and VenueQuoteArray, so the three are indexed identically.
//
// This is the OUTPUT of the policy, not the policy itself. Core classifies
// each venue and hands the result to the merge; the merge does no
// classification of its own and reads no clock. Keeping admission out of the
// merge is what lets the merge stay a pure function of its inputs.
//
// Sized by kMaxVenues (fixed CAPACITY) rather than kVenueCount (compile-time
// venue LIST) -, same step as MapOrderBookArray and
// VenueQuoteArray. The three must stay the same size: they are indexed
// identically, so sizing them differently would make one of them the real
// bound and the agreement silent.
//
// this is the safest of the three capacity changes, and for a reason
// already designed in. kNoData is enumerator 0, so a value-initialized array
// (md_core.h's `depth_health_{}` / `bbo_health_{}`) puts every unused slot in
// kNoData, and IsAdmissible(kNoData) is false. The new slots are therefore
// excluded from the merge by the SAME fail-safe default that already keeps a
// venue out until a provider affirms its feed is alive. Nothing is admitted by
// accident: an unregistered slot and a venue that has never spoken are
// genuinely the same thing, and the enum already says so.
//
// Cost: 5 extra bytes per array (VenueHealth is uint8_t), two arrays in Core.
// Not per instrument - these are per-stream, not per-book.
using VenueHealthArray = std::array<VenueHealth, kMaxVenues>;

// Which of a venue's two feeds an event refers to.
//
// depth and fast-BBO are SEPARATE SOCKETS, so a venue can be dead on one
// and healthy on the other. They therefore carry independent stamps and
// independent verdicts, and this is what keeps them apart. Sharing one verdict
// would hide the exact failure the policy exists to catch.
enum class StreamKind : uint8_t {
    kDepth,
    kBbo,
};

// One verdict, decided by the provider and delivered to Core.
//
// A plain POD on purpose: trivially copyable, fixed size, no heap. Today it
// arrives as a callback; when the per-venue SPSC queues land the same
// struct drops into a ring slot with no changes, travelling in-band with that
// venue's book updates.
//
// `decided_mono_ns` exists BECAUSE of that queue. The event is consumed
// later than it was produced, so it must carry the moment the provider
// decided rather than letting Core assume the verdict is current. Health that
// arrives through a side channel while the data arrives through a queue gives
// an inconsistent view of one venue - Core would exclude a venue whose
// updates it is still applying.
struct VenueHealthEvent {
    VenueId venue;
    StreamKind stream;
    VenueHealth health;
    int64_t decided_mono_ns;
};

// The full verdict for one feed: connection state first, then the timer.
//
// the two signals are not symmetric. A closed socket is direct evidence
// and settles the question. A socket that is OPEN settles nothing - TCP stays
// ESTABLISHED while an exchange's publisher thread wedges or a middlebox
// holds the connection open. Connection state can CONDEMN a venue but never
// clear one, which is why it cannot simply replace the timer.
//
// `backstop_ns` is per venue and per stream, and is derived rather than
// invented where the venue documents its own behaviour: Bybit republishes L1
// with the same `u` after 3s of no change, OKX sends seqId == prevSeqId after
// ~60s. Binance publishes no keepalive at all, so its silence carries no
// information and its health has to come from connection state plus
// cross-venue comparison (signal 3, not built yet).
//
// Deliberately returns no kDisconnected-with-no-data distinction: an
// unreachable venue reads as kDisconnected regardless of whether it ever
// spoke. In practice startup does not hit that, because CreateDepthSession
// increments the live count at creation, before the connect completes.
constexpr VenueHealth ClassifyFeed(bool connected, int64_t last_message_mono_ns, int64_t now_mono_ns,
                                   int64_t backstop_ns) {
    if (!connected) {
        return VenueHealth::kDisconnected;
    }
    return ClassifyVenue(last_message_mono_ns, now_mono_ns, backstop_ns);
}

}  // namespace market_data
