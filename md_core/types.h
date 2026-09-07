#pragma once

// Domain types shared across md_core: scaled-integer price/qty aliases,
// PriceLevel/LevelChange (one venue's book state and its transitions),
// BookUpdate/BboQuote (what a provider hands to Core), and CoreConfig. This is
// the vocabulary every provider produces and every core structure consumes -
// no venue protocol semantics, no floating point.

#include <array>
#include <cstdint>
#include <vector>

#include "types/venue.h"
#include "types/venue_registry.h"

namespace market_data {

using PriceTicks = uint64_t;         // price x 1e8, integral on the canonical grid. Never negative.
using QtyUnits = uint64_t;           // base quantity x 1e8. Never negative.
using Notional = unsigned __int128;  // ticks * units - needs the extra width. Never negative.

// The fixed scale behind PriceTicks/QtyUnits: the real value x 1e8. Both
// forms are needed - the exponent goes on the wire (Update.price_scale), the
// factor is used in band arithmetic to convert between the raw price x qty
// product (x 1e16) and a notional in quote currency (x 1e8).
inline constexpr uint32_t kScaleExponent = 8;
inline constexpr uint64_t kScaleFactor = 100'000'000;

inline constexpr int64_t kTsNsMultiplier = 1'000'000LL;  // ms -> ns

// Any place that needs a *signed* difference between two PriceTicks/QtyUnits
// (e.g. a crossed book: bid - ask) must compute it explicitly as
// (bool sign, PriceTicks magnitude) - compare first, then subtract the
// smaller from the larger. Never subtract two of these directly; the result
// can silently wrap instead of going negative.

struct PriceLevel {
    PriceTicks price;
    QtyUnits qty;  // absolute qty AT this price; qty ={} means "remove this level"
};

// One venue level's TRANSITION, produced by the book that applied it and
// consumed by the incremental consolidated merge.
//
// both quantities are ABSOLUTE quantities at `price` for ONE venue, not a
// signed increment - which is what lets a single untagged struct express every
// transition the merge has to handle:
//
//     insert   old_qty == 0, new_qty != 0
//     update   both non-zero
//     erase    old_qty != 0, new_qty == 0
//     no-op    NEVER emitted - see FlatOrderBook::ApplySide
//
// Carrying old_qty rather than a delta is what makes the consolidated update
// O(1): the merged level already holds this venue's previous contribution, so
// the new total is (total - old_qty + new_qty) with no re-reading of the other
// venues. Both quantities are needed because the consolidated layer must also
// decide whether this venue's ATTRIBUTION entry is being created, updated or
// removed, which a signed delta cannot say.
//
// never compute new_qty - old_qty. QtyUnits is unsigned (see the note
// above PriceLevel), so a shrinking level wraps instead of going negative.
// Subtract old_qty from the merged total first - that direction is always
// safe, because old_qty is by construction one of the summands in it - then
// add new_qty.
//
// A change is emitted only when the book actually moved, so a venue resending
// an unchanged level costs the consolidated book nothing at all.
struct LevelChange {
    PriceTicks price;
    QtyUnits old_qty;  // 0 = the level did not exist
    QtyUnits new_qty;  // 0 = the level was removed
};

struct BookUpdate {
    BookUpdate() = default;

    explicit BookUpdate(const VenueId venue, const InstrumentKey instrument, const size_t reserve_levels,
                        bool is_snapshot = false, uint64_t seq = 0)
        : venue(venue), instrument(instrument), seq(seq), is_snapshot(is_snapshot) {
        bids.reserve(reserve_levels);
        asks.reserve(reserve_levels);
    }

    VenueId venue{};
    // Carries the MARKET as well as the symbol, so a message is
    // self-describing: Core routes on it directly and nothing has to
    // remember which market a given callback was bound to. That matters
    // more once these are serialised across a process boundary.
    InstrumentKey instrument{};
    uint64_t seq{};  // venue-native monotonic sequence number
    // Venue-specific continuity field: the sequence this update claims to
    // follow. OKX `prevSeqId` (-1 on a snapshot), Binance `U` (first update
    // id in the event). 0 when the venue has no such field - Bybit chains
    // by u+1 instead. Signed because OKX uses -1.
    int64_t prev_seq{};
    // TWO receive stamps, deliberately. They answer different questions and
    // need different clocks.
    //
    // recv_ts_ns is WALL clock: the only thing it is subtracted from is
    // exch_ts_ns, the venue's own wall clock, and it is what goes on the
    // wire. It can jump - NTP, an operator, a VM resume.
    //
    // recv_mono_ns is monotonic and is the ONLY stamp the staleness
    // watchdog may use. Staleness is `now_mono - recv_mono_ns`; both
    // readings come from the same never-jumping clock, so the clock offset
    // cancels exactly. With recv_ts_ns instead, an NTP step backwards blinds
    // the watchdog, and a step forwards marks every venue stale at once and
    // publishes an empty book that looks like a total exchange outage.
    //
    // never use recv_ts_ns - exch_ts_ns as a staleness measure. Our
    // clock and the venue's are not synchronised, so that difference is
    // staleness PLUS an unknown clock offset PLUS network delay - three
    // unknowns, one equation. Fit for drift estimation only, never for
    // admission to the merge.
    int64_t recv_ts_ns{};    // ours, wall clock. Signed: used in drift subtraction.
    int64_t exch_ts_ns{};    // venue's own timestamp - drift estimation only, never compared across venues.
    int64_t recv_mono_ns{};  // ours, monotonic. Staleness only.
    bool is_snapshot{};      // true = full replace, false = incremental delta
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
};

// Top-of-book from a venue's fast-BBO stream (Binance @bookTicker, Bybit
// orderbook.1, OKX bbo-tbt). Deliberately NOT a BookUpdate and never
// applied to a MapOrderBook: the two streams are not mutually sequenced, and
// splicing fast-BBO into the depth book corrupts it.
struct BboQuote {
    VenueId venue;
    InstrumentKey instrument;
    uint64_t seq{};  // venue-native: Binance `u`, Bybit `seq`, OKX `seqId`
    // Same two-clock split as BookUpdate above, and for the same reasons.
    // The depth and fast-BBO streams are separate sockets, so their
    // staleness is tracked independently - one can die while the other
    // keeps streaming.
    int64_t recv_ts_ns{};    // ours, wall clock. Stamped by the provider.
    int64_t exch_ts_ns{};    // venue's, where available (Binance bookTicker has none)
    int64_t recv_mono_ns{};  // ours, monotonic. Staleness only.
    PriceTicks bid_price{};
    QtyUnits bid_qty{};
    PriceTicks ask_price{};
    QtyUnits ask_qty{};
};

// One latest fast-BBO quote per venue, indexed by VenueId. A quote with
// price = 0 means that venue hasn't sent one yet. Raw per-venue input to
// consolidation, not consolidated output - which is why it lives here at
// market_data scope, mirroring MapOrderBookArray, rather than in the
// `consolidated` namespace.
//
// Sized by kMaxVenues (fixed CAPACITY) rather than kVenueCount (compile-time
// venue LIST), same reason and same step as MapOrderBookArray.
//
// unlike MapOrderBookArray this holds values, not pointers, so there is no
// null to check. Safety comes from the sentinel that already exists: an unused
// slot is a default-constructed BboQuote with price = 0, which every reader
// already treats as "this venue hasn't sent one yet". The new slots are in a
// state the code has always handled - they are simply venues that never spoke.
//
// MEASURED COST, accepted deliberately: bbo_fullscan is ~104 ns for 3 venues
// in the md_core latency benchmark. Until the loop bounds move from kVenueCount to the
// registry's runtime size, that scan walks 8 slots instead of 3 and is
// correspondingly slower. bbo_incremental - the common path, ~0-42 ns - is
// untouched, and the fullscan only runs when a BBO health verdict changes
// (md_core.h), not per quote. The regression is real, temporary, and closes
// when the loop bounds migrate.
//
// Space: BboQuote is ~72 bytes, so 5 unused slots is ~360 bytes per instrument
// - roughly 3.6 KB at 10 instruments. An order of magnitude more than
// MapOrderBookArray's pointers, still small enough not to trade correctness for.
using VenueQuoteArray = std::array<BboQuote, kMaxVenues>;

// Core's own config, typed (VenueId/InstrumentKey), not raw strings.
// Whoever loads the config file (md_core_main.cpp / md_provider_main.cpp)
// translates strings to enums once, at the boundary - Core never parses a
// string.
struct CoreConfig {
    std::vector<VenueId> venues;                     // which venues are enabled
    std::vector<InstrumentKey> default_instruments;  // subscribed at startup
};

}  // namespace market_data
