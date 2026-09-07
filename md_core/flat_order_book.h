#pragma once

// The per-venue order book: FlatOrderBook, two sorted std::vectors (best
// price at back()) applied in place in O(delta) rather than O(book). This is
// the only production book implementation - the class comment below explains
// why a contiguous layout replaced the original std::map design, and what an
// in-place apply has to get right (Relocate) to stay correct under a delta
// that both inserts and erases.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include "types.h"
#include "types/venue_registry.h"

namespace market_data {

// A venue's depth book stored as two sorted, contiguous vectors instead of two
// red-black trees. Same public API as MapOrderBook, which stays in the tree as the
// permanent test oracle - the two are driven by identical update
// streams and must always agree.
//
// WHY: the merge is the measured bottleneck, and it is almost entirely tree
// WALKING, not merge logic. From the md_core latency benchmark, same run,
// ~4800 node visits each:
//
//     merge_full     8500 ns    full merge: selection, attribution, prefix sums
//     iterate_only   9000 ns    walking the same std::maps, NO merge logic
//
// Iterating the trees costs MORE than the entire merge. That is what a
// contiguous layout removes: ~4800 pointer-chases to scattered heap nodes
// become a sequential scan the hardware prefetcher can follow.
//
// this is DESIGN.md step 16, whose stated condition was "only if step 11
// shows std::map is the bottleneck". Step 11 ran and it does. The decision is
// not being reversed on taste - the gate it was waiting on opened.
class FlatOrderBook {
   public:
    // Same signature as MapOrderBook, deliberately: Core constructs one or the
    // other and nothing else about its construction changes.
    FlatOrderBook(VenueId venue, InstrumentKey instrument);

    void ApplyUpdate(const BookUpdate& update);

    std::optional<std::pair<PriceTicks, QtyUnits>> BestBid() const;
    std::optional<std::pair<PriceTicks, QtyUnits>> BestAsk() const;

    VenueId venue() const { return venue_; }
    InstrumentKey instrument() const { return instrument_; }
    uint64_t last_seq() const { return last_seq_; }

    // BEST FIRST, matching MapOrderBook::bids()/asks() exactly, so every existing
    // reader - the merge, ComputeBBO, the tests - keeps its semantics and only
    // swaps ->first/->second for .price/.qty.
    //
    // storage order is the OPPOSITE of this, and stays private to this
    // class. A reverse view costs nothing at runtime (it is a pointer plus a
    // direction) and it means no caller can accidentally depend on the
    // physical layout, which is the thing most likely to change next.
    auto bids() const { return std::views::reverse(bids_); }
    auto asks() const { return std::views::reverse(asks_); }

    // Bytes memmoved by the most recent ApplyUpdate, and cumulatively.
    //
    // this, not ns/call, is the metric that transfers. A nanosecond figure
    // describes this laptop; bytes moved describes the algorithm and predicts
    // it on any machine. It also isolates the flat book's one real weakness -
    // shifting elements - from cache effects and scheduler noise, which a
    // latency number blends together. The average should approach zero once
    // in-place application lands; the p99 is where a new price at the top of a
    // deep book shows up, and a median hides it completely.
    uint64_t last_bytes_moved() const { return last_bytes_moved_; }
    uint64_t total_bytes_moved() const { return total_bytes_moved_; }

    // How many sides (0-2) the most recent ApplyUpdate resolved with the
    // in-place fast path, against how many carried a non-empty delta at all.
    //
    // this exists because the live book_apply is ~40x slower than the
    // benchmarked fast path for the same delta size, which suggests most real
    // deltas take the region rebuild instead - Binance sends qty=0 removals
    // constantly, and any level entering or leaving routes to Relocate. That is
    // currently a HYPOTHESIS repeated in three documents, and this is the
    // cheapest thing that turns it into a number.
    //
    // Counted per SIDE, not per update: bids and asks are applied
    // independently and can take different paths, so counting whole updates
    // would blur two answers into one. A side with an empty delta counts as
    // NEITHER - it did no work, and including it would inflate the fast-path
    // ratio with applies that never happened.
    uint8_t last_fast_path_sides() const { return last_fast_path_sides_; }
    uint8_t last_applied_sides() const { return last_applied_sides_; }

    // What the most recent ApplyUpdate actually CHANGED, one span per side.
    // This is the input to the incremental consolidated merge: a depth update
    // belongs to one venue and normally moves a handful of levels, so the
    // consolidated book can be repaired from these instead of re-merging every
    // venue's full depth.
    //
    // emitted BEST-FIRST, which is already the consolidated book's storage
    // order on both sides (bids descending, asks ascending). The consumer's
    // price search therefore advances monotonically across a whole update
    // rather than restarting per change.
    //
    // Valid only until the next ApplyUpdate - these are the book's own reused
    // buffers, not copies. The consolidated merge consumes them immediately,
    // on the same thread, before the next update is drained.
    std::span<const LevelChange> last_bid_changes() const { return last_bid_changes_; }
    std::span<const LevelChange> last_ask_changes() const { return last_ask_changes_; }

    // False when the spans above do NOT describe the last update's transition
    // and must not be applied to a consolidated book - today, exactly the
    // snapshot case.
    //
    // a snapshot CLEARS both sides, so replaying its levels as changes
    // would describe every price it contains as an insert and say nothing at
    // all about the levels it replaced - the consolidated book would keep them
    // forever. The spans are left empty there, and empty is indistinguishable
    // from "nothing moved", which is a legitimate and common outcome. This flag
    // is what separates the two, so a caller that forgets the distinction fails
    // an assertion instead of silently publishing a stale merged book.
    bool last_changes_complete() const { return last_changes_complete_; }

   private:
    // Applies one side's delta. `storage_less` defines the STORAGE order -
    // std::less for bids (ascending, so back() is the highest price),
    // std::greater for asks (descending, so back() is the lowest). Returns the
    // bytes memmoved.
    //
    // ONE walk, BACKWARD from back(), stopping as soon as the delta is
    // exhausted - so a top-of-book delta never reads the deep end at all. That
    // walk writes matched quantities in place as it goes while counting the
    // levels that enter and leave. When nothing enters or leaves - the common
    // case in a live feed - it is already finished: 0 bytes moved, and a cost
    // set by the DELTA size rather than the book size.
    //
    // That last property is the point. The live Binance book grows without
    // bound (consolidated_book.h), so an apply whose cost scales with the book
    // gets worse the longer the process runs. Scaling with the delta - which
    // the venue bounds for us - is what makes the flat book safe to ship.
    //
    // committing those quantity writes BEFORE knowing whether the delta
    // also inserts or erases is safe because a BookUpdate is absolute, not
    // incremental (types.h). If the relocation pass does run, it re-reads every
    // value from the delta and writes the same numbers again. Idempotence is
    // what collapses two walks into one.
    //
    // `changes` collects this side's transitions, or is null to collect none -
    // which is what the snapshot path passes, because a cleared side makes
    // every level look like an insert (see last_changes_complete()). The same
    // backward walk that classifies each delta level already holds both the old
    // and the new quantity at that moment, so collecting costs one push_back
    // per level that actually moved and no second pass.
    template <typename StorageLess>
    uint64_t ApplySide(std::vector<PriceLevel>& side, const std::vector<PriceLevel>& levels,
                       std::vector<LevelChange>* changes, StorageLess storage_less);

    // Rewrites only `side[deepest..end)`, where `deepest` is the lowest index
    // the delta reached. Staged through merged_ and copied back.
    //
    // an earlier version did this in place, choosing the walk direction
    // from the NET size change - backward when growing, forward when shrinking.
    // That is wrong, and the oracle test caught it. The invariant has to hold at
    // every step, not just at the end:
    //
    //     book  [96, 97, 98, 99]        delta best-first: 100 -> 5, 98 -> 0
    //     inserts 1, erases 1, net 0 -> "backward"
    //     first step writes the new 100 into side[3], which still held 99
    //
    // In the backward pass `write - read` starts at inserts - erases and each
    // insert shrinks it, so meeting the inserts first drives it negative; the
    // forward pass fails the mirror image. ANY delta holding both an insert and
    // an erase can break either direction depending on the order they appear
    // in - and the corruption is silent, because the side stays sorted and the
    // right length. Staging removes the question: the destination is never an
    // input.
    //
    // Still O(region), never O(book) - which is the property that made this
    // worth doing. The cost is two write passes over the region instead of one.
    //
    // `deepest` is safe to under-estimate: passing 0 degenerates to a full
    // rebuild, which is slower but never wrong.
    template <typename StorageLess>
    uint64_t Relocate(std::vector<PriceLevel>& side, size_t deepest, const PriceLevel* base, ptrdiff_t step, size_t m,
                      StorageLess storage_less);

    VenueId venue_;
    InstrumentKey instrument_;
    uint64_t last_seq_ = 0;
    int64_t last_update_mono_ns_ = 0;  // 0 = never received anything

    // REVERSE layout: worst price first, BEST price at back().
    //
    // top-of-book is where nearly every update lands, and back() is the
    // only end of a vector that is cheap to grow and shrink. With the best
    // price at front() instead, a new best bid on a 1000-level book memmoves
    // ~16 KB (1000 x sizeof(PriceLevel)); at back() it is a push_back and moves
    // NOTHING. A level five deep costs 80 bytes rather than ~15.9 KB.
    //
    // The cost is that every reader walks backwards. That is close to free -
    // hardware prefetchers detect descending strides as well as ascending ones
    // - which is why the expensive end was chosen for the rare direction.
    std::vector<PriceLevel> bids_;  // ascending price:  back() = best bid
    std::vector<PriceLevel> asks_;  // descending price: back() = best ask

    // Staging for a delta that arrives in neither storage order nor its exact
    // reverse. Held as a member, not a local, so the rare unsorted case does
    // not allocate on the hot path after the first time.
    std::vector<PriceLevel> scratch_;

    // Staging for Relocate's merged region. Cannot be scratch_ or `side`: a
    // merge writes while both inputs are still being read, so the destination
    // has to be a third buffer.
    //
    // this holds the REGION the delta touched, not the whole side - the
    // first version merged and swapped whole sides, which is what made a
    // 5-level delta cost a 1000-level rewrite. An attempt to remove this buffer
    // entirely, by relocating in place, is what introduced the aliasing bug
    // documented on Relocate above. It is here deliberately, not by omission.
    std::vector<PriceLevel> merged_;

    // What the last ApplyUpdate moved, per side. Members rather than locals for
    // the same reason as scratch_ and merged_: reserved once in the constructor,
    // cleared per update, so a warmed-up book never allocates to report its own
    // changes.
    //
    // Sized by the DELTA, not the book - a level only lands here if the venue
    // sent it and it actually moved the book - so these stay tiny in a live
    // feed regardless of how deep Binance's book has grown.
    std::vector<LevelChange> last_bid_changes_;
    std::vector<LevelChange> last_ask_changes_;

    // See last_changes_complete(). Starts false: a book that has never applied
    // an update has no transition to describe, and the first thing it applies
    // is a snapshot anyway.
    bool last_changes_complete_ = false;

    uint64_t last_bytes_moved_ = 0;
    uint64_t total_bytes_moved_ = 0;

    // Reset at the top of every ApplyUpdate; ApplySide increments them.
    uint8_t last_fast_path_sides_ = 0;
    uint8_t last_applied_sides_ = 0;
};

// Enough for the deepest tier any venue actually publishes to us today
// (Binance 1000). Reserved once in the constructor so a warmed-up book never
// allocates while applying an update.
//
// A deeper configured depth simply grows the vector once and keeps that
// capacity - vectors here are never shrunk, so the allocation happens at most
// a handful of times in the life of the process rather than per message.
inline constexpr size_t kInitialLevelCapacity = 1024;

// Reserved for the per-side change lists. Sized by the DELTA, not the book:
// only a level the venue actually sent, and that actually moved the book, ever
// lands there. Live diffs run to a handful of levels, so this is already
// generous - and unlike kInitialLevelCapacity it does not have to grow with
// Binance's unbounded book, because a deeper book does not produce a wider
// diff. Overflowing it grows the vector once and keeps that capacity, exactly
// like every other buffer here.
inline constexpr size_t kInitialChangeCapacity = 256;

// The production book array - one book per venue slot, null until a provider
// registers that venue.
using FlatBookArray = std::array<std::unique_ptr<FlatOrderBook>, kMaxVenues>;

}  // namespace market_data
