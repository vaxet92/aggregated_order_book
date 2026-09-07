#pragma once

// Consolidated best bid/offer: BBO/ConsolidatedPriceLevel, and the three ways
// to build one - ComputeBBO (eager, from each venue's cached depth-book best),
// ComputeBBOFromQuotes (a full scan of the fast-BBO stream, used to build from
// scratch and as the oracle), and UpdateBBOWithQuote (the incremental path a
// live quote takes, O(venues at the best level) instead of O(venues)).

#include <vector>

#include "flat_order_book.h"
#include "types.h"
#include "venue_health.h"

namespace market_data {
namespace consolidated {

// Attribution: which venue contributed, and how much.
//
// carries a VenueSlot, not a VenueId. md_core has no compile-time list of
// venues - a slot is whatever registered, and the name is
// resolved at the WIRE boundary via Core::VenueName. Keeping a VenueId here
// would put the enum back in the middle of the merged output, which is the one
// place it is hardest to remove later.
//
// It is also smaller and cheaper: VenueSlot is one byte against VenueId's two,
// and the merge writes this per output level, so the array below sits in every
// MergedLevel. And because the merge loop's index IS the slot, filling it
// needs no lookup at all - not even the array read the VenueId version needed.
struct VenueQuote {
    VenueSlot slot;
    QtyUnits qty;
};

struct ConsolidatedPriceLevel {
    // Default member initializers, not just relying on the caller to
    // zero-init: PriceTicks/QtyUnits are plain uint64_t, and `BBO result;`
    // (default-init, not value-init) leaves fundamental-type members as
    // indeterminate garbage, not 0. The merge logic in ComputeBBO depends
    // on price==0 meaning "nothing seen yet" - without these, that check
    // silently reads garbage instead.
    PriceTicks price = 0;
    QtyUnits total_qty = 0;
    std::vector<VenueQuote> venues;  // only venues quoting exactly this price - client decides how to use ties
};

// price=0, total_qty=0, venues={} if genuinely no data yet - precondition is
// the publisher never calls this before the first ApplyUpdate.
struct BBO {
    ConsolidatedPriceLevel best_bid;
    ConsolidatedPriceLevel best_ask;
    bool crossed = false;  // best_bid.price >= best_ask.price. false if either side is empty.
};

// Eager BBO merge: compares each venue's cached best
// bid/ask, no k-way merge over full depth. O(venues) per call, not O(depth).
// Named ComputeBBO, not BBO, to avoid colliding with the struct above -
// C++ lets a function and a struct share a name, but the function then
// hides the struct name for lookup inside its own body, which breaks
// `BBO result;` in the implementation.
//
// Templated on the book array for the same reason as MergeBooks, but with a
// much smaller blast radius: this function only ever calls BestBid()/BestAsk(),
// so a new book implementation needs no change to its BODY.
//
// Explicitly instantiated in consolidated_bbo.cpp for FlatBookArray, the one
// production book.
template <typename BookArray>
BBO ComputeBBO(const BookArray& books);

// Consolidates the fast-BBO stream quotes. Same
// max-bid / min-ask / tie-attribution logic as ComputeBBO, but sourced
// from the venues' own top-of-book channels instead of our depth books:
// lower latency, and independent of depth-book sync state.
// VenueQuoteArray comes from types.h - unqualified here because
// `consolidated` is nested inside market_data.
//
// Full scan over all venues, O(venues). Used to build from scratch, as the
// rescan fallback inside UpdateBBOWithQuote, and as the test oracle the
// incremental UpdateBBOWithQuote path is checked against - a full recomputation
// is trivially correct, so it is the reference the stateful path must match.
// `health` excludes venues whose verdict is not kLive, exactly as in
// MergeBooks; nullptr admits everyone. Same reasoning for the default: a pure
// function merges what it is handed, and admission is the caller's policy.
BBO ComputeBBOFromQuotes(const VenueQuoteArray& quotes, const VenueHealthArray* health = nullptr);

// Same computation, filling an existing BBO instead of returning a new one.
//
// This exists for the health-change rescan. Assigning the by-value version
// over Core's running BBO would replace the attribution vectors, throwing
// away the capacity Core::Init reserves so the hot path never allocates
// Filling in place keeps those buffers.
void ComputeBBOFromQuotesInto(const VenueQuoteArray& quotes, BBO& out, const VenueHealthArray* health = nullptr);

// Incremental: folds one venue's new quote into an existing BBO.
// O(venues at the best level) - typically 1 - instead of O(venues), falling
// back to a full rescan of one side only when the last venue leaves that
// side's best level. Built for larger venue counts; at 3 venues it is a
// wash with ComputeBBOFromQuotes.
//
// PRECONDITION: `quotes` must ALREADY contain `update` - the caller stores
// the quote before calling this, because the rescan path re-reads it from
// there. Passing a stale array silently produces a wrong book, and only on
// the rare rescan path, so the bug would hide for a long time.
//
// A quote from a venue that is not kLive is treated exactly as a quote with
// price 0 - "this venue has no bid/ask" - which runs the existing
// remove-and-rescan branch. Nothing special is needed for it.
//
// filtering here is NOT sufficient on its own. This function maintains
// PERSISTENT state, so a stale venue's price is already folded into
// `current`, and a venue that has gone quiet sends nothing more to displace
// it. Excluding it requires the caller to force a full ComputeBBOFromQuotes
// rescan when a venue's health CHANGES. Filtering a stateless
// recomputation is easy; filtering an incremental one needs the transition.
//
// `slot` is where this venue's entry lives in `quotes` and in `health`, and it
// is passed rather than derived from update.venue.
//
// those two arrays are indexed by SLOT, while update.venue is a VenueId.
// They coincide today only because venues happen to register in enum order
// Deriving the index from the VenueId would read ANOTHER
// venue's health verdict once they diverge - admitting a stale venue, or
// excluding a live one, with nothing in the output to show for it.
//
// Passed in rather than looked up so this function stays pure: it takes no
// registry, holds no state, and md_core keeps exactly one place where a
// VenueId becomes a slot.
void UpdateBBOWithQuote(BBO& current, const BboQuote& update, VenueSlot slot, const VenueQuoteArray& quotes,
                        const VenueHealthArray* health = nullptr);

}  // namespace consolidated
}  // namespace market_data