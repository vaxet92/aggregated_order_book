#include "consolidated_book.h"

#include <utility>

namespace market_data {
namespace consolidated {

template <typename BookArray>
void MergeBooks(const BookArray& books, size_t venue_count, Book& out, size_t max_depth,
                const VenueHealthArray* health) {
    // Deduced rather than hardcoded, so the same body serves the std::map
    // oracle and the flat book: one yields pair<const PriceTicks, QtyUnits>,
    // the other PriceLevel, and PriceOf/QtyOf hide the difference.
    //
    // FlatOrderBook::bids() returns a VIEW BY VALUE, so the expression
    // below takes an iterator from a temporary that dies at the end of the
    // statement. That is safe, and the reason is worth knowing: a view is a
    // handle, not a container. The reverse_iterator it hands back wraps a
    // vector::const_iterator pointing into the vector's own buffer, and never
    // refers to the view object - so destroying the view cannot invalidate it.
    using BookType = typename BookArray::value_type::element_type;
    using BidIt = decltype(std::declval<const BookType&>().bids().begin());
    using AskIt = decltype(std::declval<const BookType&>().asks().begin());

    out.Clear();  // keeps capacity - no allocation after warm-up

    // Every loop below runs to `venue_count`, never to kVenueCount. Bounding
    // by the enum is what made a fourth venue register successfully and then
    // never appear in the output - no error, just a venue quietly missing
    // from the merge.
    //
    // The caller passes Core's high-water mark, so a slot whose venue was
    // removed is still iterated and skipped as a null book. That is correct:
    // slots are dense and a removed venue leaves a HOLE, so stopping early
    // would drop every venue above it.
    const size_t count = std::min(venue_count, books.size());

    // Admission is decided once, here, and then read by both sides. Deciding
    // it per side could let bids and asks disagree about which venues are in,
    // which would produce a merged book whose two halves come from different
    // sets of venues - a crossed or inverted book with no single cause to
    // trace it back to.
    //
    // nullptr admits everything: an absent policy is not the same as a policy
    // that excludes, and a pure merge with no verdict supplied must merge
    // what it was given.
    std::array<bool, kMaxVenues> admitted{};

    // No venue-id lookup here any more. An earlier version read
    // books[i]->venue() to attribute each level, which cost a MEASURED
    // ~3.5-4 us per merge when done per level, and one array read per level
    // after being hoisted here. Carrying a VenueSlot in
    // VenueQuote removes both: the merge index already IS the slot.
    for (size_t i = 0; i < count; ++i) {
        admitted[i] = (health == nullptr) || IsAdmissible((*health)[i]);
    }

    // --- bids: highest price first ---
    {
        std::array<BidIt, kMaxVenues> it{};
        std::array<BidIt, kMaxVenues> end{};
        std::array<bool, kMaxVenues> active{};
        for (size_t i = 0; i < count; ++i) {
            // A venue that is not admitted is simply never made active, so
            // the k-way merge below never looks at it. No branch is added to
            // the inner loop - the exclusion costs nothing per level.
            if (books[i] && admitted[i]) {
                it[i] = books[i]->bids().begin();
                end[i] = books[i]->bids().end();
                active[i] = it[i] != end[i];
            }
        }

        while (out.bids.size() < max_depth) {
            PriceTicks best = 0;
            bool found = false;
            for (size_t i = 0; i < count; ++i) {
                if (active[i] && (!found || PriceOf(*it[i]) > best)) {
                    best = PriceOf(*it[i]);
                    found = true;
                }
            }
            if (!found) {
                break;  // every venue exhausted
            }

            out.bids.emplace_back();
            MergedLevel& level = out.bids.back();
            level.price = best;

            // This level's own quantity is a local: MergedLevel stores only
            // the running total, and LevelQty() recovers the per-level value.
            QtyUnits level_qty = 0;
            for (size_t i = 0; i < count; ++i) {
                if (active[i] && PriceOf(*it[i]) == best) {
                    const QtyUnits qty = QtyOf(*it[i]);
                    level_qty += qty;
                    // Attribution IS the loop index: `i` is the slot, and
                    // VenueQuote carries a slot rather than a VenueId, so
                    // there is nothing to look up. The name is resolved once
                    // at the wire boundary (Core::VenueName), never per level.
                    //
                    // The bound is provable - the inner loop visits each of
                    // `count` slots at most once, and count <= kMaxVenues - so
                    // this guard should never fire. It is here because the
                    // proof depends on two constants agreeing, and they have
                    // already disagreed once: overrunning corrupts the next
                    // MergedLevel in the vector, which is silent, whereas
                    // losing one level's attribution is visible and bounded.
                    if (level.venue_count < level.venues.size()) {
                        level.venues[level.venue_count++] = {static_cast<VenueSlot>(i), qty};
                    }
                    ++it[i];
                    active[i] = it[i] != end[i];
                }
            }
            // The level's own total. Prefix sums used to be accumulated here
            // and stored per level; the band walks now do that themselves, on
            // the traversal they were already making (consolidated_book.h).
            level.qty = level_qty;
        }
    }

    // --- asks: lowest price first. Same shape as bids, opposite comparison.
    // The two sides are different types (different map comparators), so they
    // cannot share one function without a template. ---
    {
        std::array<AskIt, kMaxVenues> it{};
        std::array<AskIt, kMaxVenues> end{};
        std::array<bool, kMaxVenues> active{};
        for (size_t i = 0; i < count; ++i) {
            if (books[i] && admitted[i]) {
                it[i] = books[i]->asks().begin();
                end[i] = books[i]->asks().end();
                active[i] = it[i] != end[i];
            }
        }

        while (out.asks.size() < max_depth) {
            PriceTicks best = 0;
            bool found = false;
            for (size_t i = 0; i < count; ++i) {
                if (active[i] && (!found || PriceOf(*it[i]) < best)) {  // asks: lower is better
                    best = PriceOf(*it[i]);
                    found = true;
                }
            }
            if (!found) {
                break;  // every venue exhausted
            }

            out.asks.emplace_back();
            MergedLevel& level = out.asks.back();
            level.price = best;

            QtyUnits level_qty = 0;
            for (size_t i = 0; i < count; ++i) {
                if (active[i] && PriceOf(*it[i]) == best) {
                    const QtyUnits qty = QtyOf(*it[i]);
                    level_qty += qty;
                    // Attribution IS the loop index: `i` is the slot, and
                    // VenueQuote carries a slot rather than a VenueId, so
                    // there is nothing to look up. The name is resolved once
                    // at the wire boundary (Core::VenueName), never per level.
                    //
                    // The bound is provable - the inner loop visits each of
                    // `count` slots at most once, and count <= kMaxVenues - so
                    // this guard should never fire. It is here because the
                    // proof depends on two constants agreeing, and they have
                    // already disagreed once: overrunning corrupts the next
                    // MergedLevel in the vector, which is silent, whereas
                    // losing one level's attribution is visible and bounded.
                    if (level.venue_count < level.venues.size()) {
                        level.venues[level.venue_count++] = {static_cast<VenueSlot>(i), qty};
                    }
                    ++it[i];
                    active[i] = it[i] != end[i];
                }
            }
            // The level's own total. Prefix sums used to be accumulated here
            // and stored per level; the band walks now do that themselves, on
            // the traversal they were already making (consolidated_book.h).
            level.qty = level_qty;
        }
    }
}

// The set of book implementations (consolidated_book.h): one production book
// today. Adding a line here is the deliberate, visible cost of adding a second
// - and the reason the merge body can stay in this .cpp instead of the header.
template void MergeBooks<FlatBookArray>(const FlatBookArray&, size_t, Book&, size_t, const VenueHealthArray*);

// Builds one band's result at the level that STRADDLES the target: that level
// is partially consumed, everything above it fully.
//
// `qty_before` / `filled_before` are the running totals EXCLUDING side[crossing].
// They are passed in rather than read off the level, because the prefix sums
// that used to carry them are gone - the caller accumulates them on the walk it
// was already making (consolidated_book.h).
static NotionalFill FillAtCrossing(std::span<const MergedLevel> side, size_t crossing, QtyUnits qty_before,
                                   Notional filled_before, Notional target_raw) {
    NotionalFill result;
    const Notional still_needed = target_raw - filled_before;

    // needed / price: 1e16 / 1e8 = 1e8, already a QtyUnits. Integer division
    // truncates, so we under-fill by a sub-satoshi amount rather than over.
    QtyUnits partial_qty = static_cast<QtyUnits>(still_needed / side[crossing].price);
    const QtyUnits available = side[crossing].qty;
    if (partial_qty > available) {
        partial_qty = available;  // guard against rounding overshoot
    }

    const Notional filled_raw = filled_before + static_cast<Notional>(side[crossing].price) * partial_qty;
    result.filled_qty = qty_before + partial_qty;
    result.filled_notional = static_cast<uint64_t>(filled_raw / kScaleFactor);
    result.worst_price = side[crossing].price;
    result.level_count = static_cast<uint32_t>(crossing + 1);
    result.vwap = result.filled_qty ? static_cast<PriceTicks>(filled_raw / result.filled_qty) : 0;
    return result;
}

// The book ran out before the target - a legitimate answer on BTCUSDT, not an
// error (section 5.2). The totals passed in cover the whole side.
static NotionalFill FillExhausted(std::span<const MergedLevel> side, QtyUnits cum_qty, Notional cum_notional) {
    NotionalFill result;
    result.filled_qty = cum_qty;
    result.filled_notional = static_cast<uint64_t>(cum_notional / kScaleFactor);
    result.worst_price = side.back().price;
    result.level_count = static_cast<uint32_t>(side.size());
    result.vwap = cum_qty ? static_cast<PriceTicks>(cum_notional / cum_qty) : 0;
    result.insufficient_depth = true;
    return result;
}

NotionalFill FillToNotional(std::span<const MergedLevel> side, uint64_t target_notional) {
    // Target arrives as USDT x 1e8; the raw price x qty product is x 1e16.
    const Notional target_raw = static_cast<Notional>(target_notional) * kScaleFactor;

    if (side.empty()) {
        NotionalFill result;
        result.insufficient_depth = target_raw > 0;
        return result;
    }
    if (target_raw == 0) {
        return NotionalFill{};
    }

    // ONE forward walk, accumulating as it goes.
    //
    // this used to be a SEARCH over stored prefix sums - but that search
    // was itself a linear scan of the same levels, so the stored prefixes
    // bought nothing here while costing an O(depth) repair on every incremental
    // update (consolidated_book.h). Accumulating locally is the same traversal
    // plus two adds.
    QtyUnits cum_qty = 0;
    Notional cum_notional = 0;
    for (size_t i = 0; i < side.size(); ++i) {
        const Notional next = cum_notional + static_cast<Notional>(side[i].price) * side[i].qty;
        if (next >= target_raw) {
            return FillAtCrossing(side, i, cum_qty, cum_notional, target_raw);
        }
        cum_notional = next;
        cum_qty += side[i].qty;
    }
    return FillExhausted(side, cum_qty, cum_notional);
}

void FillToNotionalBands(std::span<const MergedLevel> side, const std::vector<uint64_t>& targets,
                         std::vector<NotionalFill>& out) {
    out.clear();  // keeps capacity
    out.reserve(targets.size());

    // ONE walk for all targets. The targets are sorted ascending and the
    // running notional is monotonic, so the crossing level for target N+1 can
    // never be before the one for target N - `i` never rewinds and the loop is
    // O(levels + bands), not O(levels x bands).
    //
    // the crossing level is deliberately NOT consumed. This target fills
    // only part of it, so a wider target must still see it whole - which is
    // why cum_qty/cum_notional exclude it and `i` stays put.
    QtyUnits cum_qty = 0;
    Notional cum_notional = 0;
    size_t i = 0;

    for (uint64_t target : targets) {
        const Notional target_raw = static_cast<Notional>(target) * kScaleFactor;

        if (side.empty()) {
            NotionalFill result;
            result.insufficient_depth = target_raw > 0;
            out.push_back(result);
            continue;
        }
        if (target_raw == 0) {
            out.push_back(NotionalFill{});
            continue;
        }

        bool crossed = false;
        while (i < side.size()) {
            const Notional next = cum_notional + static_cast<Notional>(side[i].price) * side[i].qty;
            if (next >= target_raw) {
                crossed = true;
                break;
            }
            cum_notional = next;
            cum_qty += side[i].qty;
            ++i;
        }

        out.push_back(crossed ? FillAtCrossing(side, i, cum_qty, cum_notional, target_raw)
                              : FillExhausted(side, cum_qty, cum_notional));
    }
}

BpsFill FillToBps(std::span<const MergedLevel> side, uint32_t bps, bool is_bid) {
    BpsFill result;
    if (side.empty()) {
        return result;
    }

    // A bid band wider than 100% would underflow (10000 - bps); clamp it to
    // a limit of zero, which simply includes the whole side.
    const uint32_t clamped_bps = (is_bid && bps > 10000) ? 10000 : bps;
    const PriceTicks top = side.front().price;
    const Notional scaled = static_cast<Notional>(top) * (is_bid ? (10000 - clamped_bps) : (10000 + clamped_bps));
    result.limit_price = static_cast<PriceTicks>(scaled / 10000);

    // Unlike FillToNotional there is no partial level - a level is either
    // inside the boundary or outside it.
    QtyUnits cum_qty = 0;
    Notional cum_notional = 0;
    size_t i = 0;
    while (i < side.size()) {
        const bool inside = is_bid ? (side[i].price >= result.limit_price) : (side[i].price <= result.limit_price);
        if (!inside) {
            break;
        }
        cum_qty += side[i].qty;
        cum_notional += static_cast<Notional>(side[i].price) * side[i].qty;
        ++i;
    }
    // Exiting because the book ended, rather than because a level fell
    // outside the boundary, means the totals are a lower bound.
    result.insufficient_depth = (i == side.size());
    if (i == 0) {
        return result;  // nothing within the band
    }

    result.cum_qty = cum_qty;
    result.cum_notional = static_cast<uint64_t>(cum_notional / kScaleFactor);
    result.level_count = static_cast<uint32_t>(i);
    result.vwap = cum_qty ? static_cast<PriceTicks>(cum_notional / cum_qty) : 0;
    return result;
}

void FillToBpsBands(std::span<const MergedLevel> side, const std::vector<uint32_t>& bps_bands, bool is_bid,
                    std::vector<BpsFill>& out) {
    out.clear();  // keeps capacity
    out.reserve(bps_bands.size());

    if (side.empty()) {
        out.resize(bps_bands.size());  // all default-constructed: nothing in any band
        return;
    }

    const PriceTicks top = side.front().price;

    // ONE walk for all bands, same reasoning as FillToNotionalBands: wider
    // bands include narrower ones, so with bps_bands sorted ascending the
    // boundary only moves forward and the running totals carry over.
    QtyUnits cum_qty = 0;
    Notional cum_notional = 0;
    size_t i = 0;

    for (uint32_t bps : bps_bands) {
        const uint32_t clamped_bps = (is_bid && bps > 10000) ? 10000 : bps;
        const Notional scaled = static_cast<Notional>(top) * (is_bid ? (10000 - clamped_bps) : (10000 + clamped_bps));
        const PriceTicks limit = static_cast<PriceTicks>(scaled / 10000);

        while (i < side.size()) {
            const bool inside = is_bid ? (side[i].price >= limit) : (side[i].price <= limit);
            if (!inside) {
                break;
            }
            cum_qty += side[i].qty;
            cum_notional += static_cast<Notional>(side[i].price) * side[i].qty;
            ++i;
        }

        BpsFill result;
        result.limit_price = limit;
        result.insufficient_depth = (i == side.size());
        if (i > 0) {
            result.cum_qty = cum_qty;
            result.cum_notional = static_cast<uint64_t>(cum_notional / kScaleFactor);
            result.level_count = static_cast<uint32_t>(i);
            result.vwap = cum_qty ? static_cast<PriceTicks>(cum_notional / cum_qty) : 0;
        }
        out.push_back(result);
    }
}

}  // namespace consolidated
}  // namespace market_data
