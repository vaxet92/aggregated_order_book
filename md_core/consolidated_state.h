#pragma once

// consolidated::State: the persistent consolidated book, repaired in place
// from one venue's LevelChanges (ApplyDepthDelta) instead of rebuilt from
// every venue on every update. FullRebuild (MergeBooks under the hood) is the
// exceptional path - snapshot, health transition, venue add/remove - and the
// oracle every incremental result is checked against. Published by const
// reference, valid only for the duration of the publish callback.

#include <cstddef>
#include <limits>
#include <span>
#include <vector>

#include "consolidated_book.h"
#include "flat_order_book.h"
#include "types.h"
#include "venue_health.h"

namespace market_data {
namespace consolidated {

// The PERSISTENT consolidated book, repaired in place from one venue's changes
// instead of rebuilt from every venue's depth on every update.
//
// WHY, and the measurement it comes from: MergeBooks costs ~9.5 us and is
// WRITE-BOUND roughly 4.6:1 - ~352 KB of MergedLevel
// written per merge against ~77 KB of venue book read. Walking the venue books
// is only ~584 ns of that. So the waste is not that the merge RE-READS three
// venues; it is that it RE-DERIVES ~3000 output levels when a depth update
// normally moves a handful. This class keeps those levels between updates.
//
// MergeBooks is not replaced, it is RE-ROLED. It stays as FullRebuild
// below - startup, snapshot/resync, health transitions, venue add/remove - and
// as the differential-test oracle. The invariant the tests assert is:
//
//     State after N incremental updates  ==  FullRebuild of the same books
//
// That is the whole safety story. An incremental consolidated book fails
// SILENTLY: a mis-subtracted quantity leaves a well-formed, correctly sorted
// book with one wrong number in it, and nothing downstream can tell.
class State {
   public:
    State();

    // --- the hot path -------------------------------------------------------
    //
    // Folds ONE venue's level transitions into the merged book. `slot` is that
    // venue's attribution slot; the spans come straight from
    // FlatOrderBook::last_bid_changes()/last_ask_changes().
    //
    // PRECONDITION: the caller has checked FlatOrderBook::last_changes_complete()
    // and the venue's admission verdict. Neither is checked here - this function
    // merges what it is given, exactly like MergeBooks, and the policy decisions
    // live in Core where the health verdicts do.
    //
    // the two spans are separate rather than one tagged list because bids
    // and asks are independent sorted sequences with opposite orderings. A
    // single list would need a side tag per element and a branch per element to
    // pick the comparator.
    void ApplyDepthDelta(VenueSlot slot, std::span<const LevelChange> bid_changes,
                         std::span<const LevelChange> ask_changes);

    // --- the exceptional path -----------------------------------------------
    //
    // Discards everything and re-derives from the venue books with the existing
    // k-way merge. Used where an incremental repair is impossible or not worth
    // the complexity: a snapshot (which REPLACES a venue's side, so its levels
    // describe no transition), a health transition (which can add or remove
    // thousands of contributions at once), and venue registration/removal.
    //
    // NOT depth-truncated, deliberately - see kUnboundedDepth below.
    // Truncation belongs to the band walk, not to the state.
    template <typename BookArray>
    void FullRebuild(const BookArray& books, size_t venue_count, const VenueHealthArray* health = nullptr);

    // --- publication --------------------------------------------------------
    //
    // THE authoritative consolidated book, published BY REFERENCE. There is no
    // snapshot copy and no buffer pool.
    //
    // this replaced an 8.2 us copy per publish, which was 98% of what
    // publishing cost once the merge itself had fallen to 167 ns. The copy
    // existed so a subscriber could hold an immutable book after the callback
    // returned - and no subscriber does: PublishBook computes its bands
    // synchronously, pushes protobuf messages, and drops the book before it
    // returns (aggregator_service.cpp).
    //
    // the contract that replaces it - THIS REFERENCE IS VALID ONLY FOR THE
    // DURATION OF THE CALLBACK. The consolidator thread resumes mutating this
    // book on the very next update. A consumer that wants to keep it must COPY
    // it, and the three test harnesses that retain a book do exactly that.
    //
    // When a genuinely asynchronous consumer appears - a depth feed, or the
    // process split in DESIGN.md section 17 - the answer is versioned buffers
    // caught up by replaying LevelChange, NOT a return to copying. Phase 2
    // already produces that change stream; nothing else is needed to build it.
    const Book& book() const { return book_; }

    // Publication metadata, written by Core immediately before it publishes.
    //
    // Separate from book() so the levels stay const to every caller: this is
    // the ONLY sanctioned mutation of the published book from outside, and
    // confining it to two named fields is what keeps "the state owns the
    // levels" true while Core still stamps provenance onto them.
    void StampForPublish(int64_t source_mono_ns, const std::array<uint32_t, kMaxVenues>& venue_levels);

    // --- rebuild latch ------------------------------------------------------
    //
    // Set when something happened that an incremental repair cannot express;
    // cleared by FullRebuild. A latch rather than an immediate rebuild because
    // the events that set it (a health transition, a venue disconnect) are per
    // VENUE while a rebuild is per INSTRUMENT, and the caller may hold many
    // instruments - so the work is deferred to the next update on each.
    //
    // Starts TRUE: a fresh State has never merged anything, so the first update
    // must rebuild rather than repair an empty book into existence one level at
    // a time.
    void MarkForRebuild() { needs_rebuild_ = true; }
    bool needs_rebuild() const { return needs_rebuild_; }

    // Read access for tests and diagnostics.
    const std::vector<MergedLevel>& bids() const { return book_.bids; }
    const std::vector<MergedLevel>& asks() const { return book_.asks; }

    // Bytes memmoved by vector insert/erase in the most recent ApplyDepthDelta,
    // and cumulatively.
    //
    // this is the metric that decides whether the consolidated side is
    // stored in the wrong direction. Both sides are stored BEST-FIRST to match
    // the published Book, so a new best price memmoves the entire side - the
    // exact mistake FlatOrderBook documents and reverses by putting the best
    // price at back(). The cheaper layout was not adopted here on reasoning
    // alone: bytes moved says whether it is worth the reversal, in a unit that
    // describes the algorithm rather than this laptop.
    uint64_t last_bytes_moved() const { return last_bytes_moved_; }
    uint64_t total_bytes_moved() const { return total_bytes_moved_; }

   private:
    // Bids DESCENDING, asks ASCENDING. Held as a Book rather than two loose
    // vectors so it can be published by reference with no conversion - the
    // published type and the stored type are now the same object.
    //
    // FullRebuild merges straight into this. There is no scratch buffer,
    // because there is nothing to double-buffer against: the only reader is the
    // callback, and it runs on this thread.
    Book book_;

    bool needs_rebuild_ = true;

    uint64_t last_bytes_moved_ = 0;
    uint64_t total_bytes_moved_ = 0;
};

// What FullRebuild passes to MergeBooks as its depth budget.
//
// the STATE is unbounded while the PUBLISHED book is capped at
// kDefaultMaxDepth, and the two must not be confused. Capping the state would
// be silently wrong: erase a level inside the window and the level that should
// take the last slot lives at index 1500 of some venue's book, which an
// incremental repair cannot see - so the published book would lose one level
// per erase and shrink for as long as the process runs.
//
// The cost is memory: the state is the full union of the venue books, and
// Binance's grows without bound (consolidated_book.h). That is a known,
// measurable cost accepted in exchange for the incremental path being exactly
// equal to a full rebuild, which is the property every test here rests on.
inline constexpr size_t kUnboundedDepth = std::numeric_limits<size_t>::max();

}  // namespace consolidated
}  // namespace market_data
