#pragma once

// The consolidated depth book (Book, MergedLevel) and the k-way merge that
// fills it (MergeBooks) - now the REBUILD path and differential-test oracle,
// superseded on the hot path by consolidated_state.h's incremental repair.
// Also the band math: FillToNotional(Bands) for VWAP-to-notional volume
// bands, FillToBps(Bands) for cumulative-liquidity-within-bps price bands,
// each a single forward walk that fills every configured band at once.

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "consolidated_bbo.h"
#include "flat_order_book.h"
#include "types.h"
#include "venue_health.h"

namespace market_data {
namespace consolidated {

// Reads the price and the quantity from ONE level of a venue book, whichever
// implementation it came from: std::map yields pair<const PriceTicks,
// QtyUnits>, FlatOrderBook yields PriceLevel. Two overloads each, so the merge
// body is written once and compiled for both instead of duplicated - and two
// copies of that k-way merge drifting apart is exactly the bug an oracle
// cannot catch, because it would be checking one copy against itself.
inline PriceTicks PriceOf(const PriceLevel& level) {
    return level.price;
}
inline QtyUnits QtyOf(const PriceLevel& level) {
    return level.qty;
}
inline PriceTicks PriceOf(const std::pair<const PriceTicks, QtyUnits>& node) {
    return node.first;
}
inline QtyUnits QtyOf(const std::pair<const PriceTicks, QtyUnits>& node) {
    return node.second;
}

// One price level of the merged book, with per-venue attribution and the
// running totals up to and including this level.
//
// Attribution is a FIXED array, not a vector: there can never be more
// contributors than VenueId::COUNT, and a vector here would mean one heap
// allocation per level - ~500 per merge, ~50k/sec at depth-update rates
struct MergedLevel {
    PriceTicks price = 0;

    // Sized by kMaxVenues, NOT kVenueCount. The merge loop is bounded by
    // Core's runtime venue count, which reaches kMaxVenues (8) - so an
    // enum-sized array here (3) meant the FOURTH venue quoting a given price
    // wrote past the end, corrupting the neighbouring MergedLevel in the
    // vector. Silent, and latent only because exactly three venues run today.
    //
    // this array's bound and the merge loop's bound are the SAME quantity
    // and must be written as the same constant. They drifted apart once
    // already, which is why the write site also carries a runtime guard
    // rather than trusting the two to stay in step.
    //
    // MEASURED, and the reason this is inline rather than out of line: an
    // attempt to pack attribution into a side array (MergedLevel 176 -> 48
    // bytes) made the merge 40% SLOWER - ratio 1.13 -> 1.57, ~11 us -> ~15.8
    // us. push_back per contributor costs a size+capacity load, a branch, and
    // a store to a second write stream, ~3000 times per merge; the inline
    // store wins because this level is already in L1 from being written a
    // moment ago. The out-of-line layout should only win in the BAND WALK,
    // which reads none of this - and nothing measures the band walk yet, so
    // that gain is unproven and the loss is not.
    // DELIBERATELY NOT zero-initialized. With the `{}` that used to be here,
    // emplace_back() value-initialized all 176 bytes and the merge then wrote
    // ~81 bytes of real data on top - ~257 bytes of stores per level, of which
    // 128 was zero-filling EIGHT attribution slots when three venues exist.
    //
    // MEASURED: removing it changed NOTHING. Per-level merge cost went 19.3 ns
    // to 18.1 ns in a controlled run, with the after-windows straddling the
    // before-windows. The merge is compute- and
    // latency-bound - roughly 15 cycles per output level for three venue
    // comparisons, three iterator steps and a 128-bit multiply - so its stores
    // were never the constraint.
    //
    // Kept anyway: it removes work that cannot help, and it forced Venues()
    // below, which is a contract improvement independent of performance. Do
    // NOT cite this as an optimisation.
    //
    // this trades zero-fill cost for an undefined-behaviour hazard, and
    // Venues() is what pays for it. Entries at or beyond venue_count now hold
    // indeterminate values, so reading them is UB rather than a harmless read
    // of zeros. Use Venues(); never iterate this array directly.
    std::array<VenueQuote, kMaxVenues> venues;
    uint8_t venue_count = 0;

    // The only sanctioned way to read attribution. Returns exactly the entries
    // the merge filled, so the bound lives in the type instead of in every
    // caller remembering to check venue_count.
    std::span<const VenueQuote> Venues() const { return {venues.data(), venue_count}; }

    // This level's OWN total across every contributor - NOT a prefix sum.
    //
    // prefix sums used to live here (cum_qty / cum_notional, filled by the
    // merge) and were REMOVED. They were read by exactly one thing - the four
    // band functions - and those already walk the side forward from index 0, so
    // they can accumulate as they go at no extra traversal. Storing the prefixes
    // instead bought a random-access query nobody made, and cost an O(depth)
    // repair pass on EVERY update once the consolidated book became incremental:
    // a change at the top of book, which is where changes cluster, invalidates
    // every prefix below it.
    //
    // That repair is what stood between the incremental merge and its win.
    // Measured: the merge work itself fell 9417 ns -> 167 ns, but publishing
    // still cost ~8.2 us, and prefix maintenance would have put an O(depth)
    // pass straight back onto the update path.
    //
    // Kept as a stored field rather than re-summed from Venues() on demand
    // because the band walk reads it once per level and the incremental path
    // maintains it in O(1) - total minus the venue's old quantity plus its new
    // one, which is exactly what LevelChange carries.
    QtyUnits qty = 0;
};

// venue_count is uint8_t, so the venue cap must fit in it. Failing here beats
// silently wrapping to 0 contributors on a level.
static_assert(kMaxVenues <= 255, "MergedLevel::venue_count is uint8_t");

// LevelQty() was here. It existed only to recover a level's own quantity from
// two prefix sums, and MergedLevel::qty now stores it directly - so the helper
// and the index-0 boundary condition it wrapped are both gone. Read `.qty`.

// The consolidated depth book. Sorted vectors, not maps - this is read
// strictly in order by the band walk, so a map's O(log n) key lookup would be
// paid for and never used.
//
// this is no longer "rebuilt at publish time, never maintained per
// update", which is what this comment used to say and what the design doc still says. The merge is now incremental, so
// this the lazy path. Core now MAINTAINS one of these per instrument across updates (consolidated_state.h) and
// publishes it by reference; MergeBooks fills it only on a rebuild - startup, snapshot/resync, health transition, venue
// add/remove - and as the differential-test oracle.
struct Book {
    std::vector<MergedLevel> bids;  // descending price - bids[0] is the best bid
    std::vector<MergedLevel> asks;  // ascending price  - asks[0] is the best ask

    // Monotonic arrival stamp of the update that triggered this merge, copied
    // straight from BookUpdate::recv_mono_ns.
    //
    // Core does not read a clock - it forwards a number it was given, so
    // md_core keeps its no-I/O, no-clock rule. The publisher, which is allowed
    // a clock, subtracts this from its own reading to get the full pipeline
    // latency: provider parse -> handoff -> book apply -> merge -> published.
    //
    // That end-to-end figure is the ONLY fair way to compare the current
    // mutex handoff against the per-venue SPSC queues.
    // Timing ApplyUpdate itself would not work: after the change it becomes a
    // queue push that returns immediately, which measures work MOVING to
    // another thread rather than work getting cheaper.
    //
    // Not only instrumentation - this is also the honest input for the wire's
    // server_ts_ns and for answering "how old is this snapshot?".
    int64_t source_mono_ns = 0;

    // Bid-side depth of each venue's book at the moment of this merge, indexed
    // by VenueId. Zero for a venue that is not configured.
    //
    // Diagnostic first: the live publish latency came in ~10x above what
    // bench_md_core predicted, and the leading suspect is that the benchmark
    // modelled 1000-level maps (~16 KB, comfortably in L2) while the real
    // Binance book grows WITHOUT BOUND - its diff stream reports changes
    // across a $30,000 price range and nothing trims them. A megabyte-scale
    // red-black tree turns every `++it` in the merge into a cache miss, and
    // the merge is ~100% traversal.
    //
    // Needs no clock, so it costs md_core nothing architecturally - it is read
    // while the lock is already held, from state already in hand.
    //
    // Also the natural input for `VenueStatus` on the wire, which currently
    // reports no depth at all.
    //
    // Sized by kMaxVenues and indexed by SLOT, like every other per-venue
    // array. Pure capacity - the entries are counts, with no
    // venue identity in them, so an unused slot reads 0, which is already what
    // "this venue contributed nothing" means.
    std::array<uint32_t, kMaxVenues> venue_levels{};

    // Clears without releasing capacity, so a Book reused across publishes
    // stops allocating after warm-up.
    void Clear() {
        bids.clear();
        asks.clear();
    }
};

// N must cover the deepest question asked - the 50M notional band and
// the 1000bps price band.
//
// Sized to the depth the venues actually publish, so nothing already in
// memory is discarded: Bybit orderbook.50 gives 50 levels, OKX books gives
// 400, Binance's REST snapshot up to 1000 - about 1450 merged. At 500 we
// were throwing away roughly two thirds of what we already had.
//
// Bands will STILL exhaust this, and that is correct rather than a bug:
// 1000bps is ~10% away on BTCUSDT and no venue publishes anywhere near that
// far. Exhaustion is reported through insufficient_depth, never hidden.
inline constexpr size_t kDefaultMaxDepth = 1500;

// Merges into `out`, reusing its buffers. Caller keeps one Book alive across
// publishes rather than constructing a fresh one each time.
//
// `health` is the staleness verdict per venue. A venue that is
// not kLive contributes nothing to the merge.
//
// a stale venue must be EXCLUDED, not merely reported. The merge takes
// max(bid) and min(ask); a frozen venue never moves, so when the market falls
// it always looks like the best bid and when it rises it always looks like
// the best ask. Staleness is not noise that averages out - the merge actively
// selects for it, so one frozen venue out of three corrupts the output nearly
// every time the market moves.
//
// nullptr admits every venue. That is the correct neutral default for a pure
// merge function: it merges what it is given, and deciding what it is given
// is the caller's policy decision, made in Core where the timestamps live.
// The tests that exercise merge behaviour alone rely on this default, so the
// guarantee that production never forgets to pass it is a Core-level test,
// not this signature.
// `venue_count` bounds every per-venue loop. Pass Core's high-water mark, not
// kVenueCount and not books.size(): the enum bound silently drops any venue
// registered beyond it, and the capacity bound iterates empty slots on a path
// measured in microseconds. A slot whose venue was removed is still counted
// and skipped as a null book - slots are dense, so a removal leaves a hole and
// stopping early would drop every venue above it.
//
// Defined in consolidated_book.cpp and explicitly instantiated there for
// FlatBookArray, the one production book.
//
// an unlisted book type is a LINK error, and that is deliberate rather
// than a limitation. Explicit instantiation keeps the merge body out of this
// header and out of every TU that includes it;
template <typename BookArray>
void MergeBooks(const BookArray& books, size_t venue_count, Book& out, size_t max_depth = kDefaultMaxDepth,
                const VenueHealthArray* health = nullptr);

// ---------------------------------------------------------------------------
// Band math. Both walk the same prefix-sum book; they differ
// only in the stopping condition - notional reached vs price limit passed.
// ---------------------------------------------------------------------------

// Volume band: sweep until `target_notional` of quote currency is
// filled, splitting the final level proportionally.
struct NotionalFill {
    PriceTicks vwap = 0;         // filled_notional / filled_qty
    PriceTicks worst_price = 0;  // last level touched
    QtyUnits filled_qty = 0;
    uint64_t filled_notional = 0;     // USDT x 1e8, same scale as target_notional
    uint32_t level_count = 0;         // levels consumed (the partial one counts)
    bool insufficient_depth = false;  // book ran out before reaching the target
};

// target_notional is USDT x 1e8 (1M -> 100'000'000'000'000).
//
// Accumulation happens in unsigned __int128 at the raw price x qty scale
// (x 1e16): 50M USDT raw is 5e23, which overflows uint64 and a double's
// exact-integer range alike. vwap falls out as 1e16 / 1e8 = 1e8, already a
// correctly scaled PriceTicks.
//
// Exhausting the book is a legitimate answer on BTCUSDT, not an error - the
// partial fill is returned with insufficient_depth set.
NotionalFill FillToNotional(std::span<const MergedLevel> side, uint64_t target_notional);

// Fills EVERY band in ONE forward walk. The bands are nested
// (1M subset 5M subset 10M subset 25M subset 50M), so crossing a threshold
// just records that band's result and the walk continues.
// Calling FillToNotional once per band would rewalk the book each time.
//
// `targets` must be sorted ascending. `out` is filled in the same order and
// is reused across publishes rather than reallocated.
void FillToNotionalBands(std::span<const MergedLevel> side, const std::vector<uint64_t>& targets,
                         std::vector<NotionalFill>& out);

// Price band: cumulative liquidity within `bps` of the top of this
// side. Measured from the BBO, per the assignment's literal wording; measuring from the mid is the more common
// convention and is left as a config flag, not built.
struct BpsFill {
    PriceTicks vwap = 0;         // cum_notional / cum_qty
    PriceTicks limit_price = 0;  // the bps boundary itself
    QtyUnits cum_qty = 0;
    uint64_t cum_notional = 0;  // USDT x 1e8
    uint32_t level_count = 0;

    // The walk reached the end of the book before crossing limit_price, so
    // the totals are a LOWER BOUND on the liquidity within the band rather
    // than the whole of it. Distinguishing the two matters: on BTCUSDT the
    // wider bands are always truncated, because no venue publishes anything
    // near 10% of depth, and a truncated result otherwise looks identical to
    // a complete one.
    bool insufficient_depth = false;
};

// `is_bid` picks the direction: bids walk DOWN from the best bid to
// best_bid x (1 - bps/10000), asks walk UP to best_ask x (1 + bps/10000).
//
// Unlike FillToNotional there is no partial level - a level is either inside
// the boundary or outside it. Running out of book before reaching the
// boundary is still reported, via insufficient_depth, because a truncated
// total is otherwise indistinguishable from a complete one.
BpsFill FillToBps(std::span<const MergedLevel> side, uint32_t bps, bool is_bid);

// One forward walk for every bps band, same reasoning as
// FillToNotionalBands. `bps_bands` must be sorted ascending; `out` is filled
// in the same order and reused across publishes.
void FillToBpsBands(std::span<const MergedLevel> side, const std::vector<uint32_t>& bps_bands, bool is_bid,
                    std::vector<BpsFill>& out);

}  // namespace consolidated
}  // namespace market_data
