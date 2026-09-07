#include "consolidated_state.h"

#include <algorithm>
#include <functional>

namespace market_data {
namespace consolidated {
namespace {

// Enough that a warmed-up State never reallocates while repairing a level.
// Sized like FlatOrderBook's own reserve rather than by kDefaultMaxDepth: the
// state is the union of the venue books, so it starts around the deepest one
// and grows once if Binance's book outruns it.
constexpr size_t kInitialStateCapacity = 2048;

// Finds `price` in a side stored best-first, or the index it would be inserted
// at. `better(a, b)` is true when price a ranks ahead of price b on this side:
// std::greater for bids (higher first), std::less for asks (lower first).
template <typename Better>
size_t LowerBoundByRank(const std::vector<MergedLevel>& side, PriceTicks price, Better better) {
    const auto it = std::lower_bound(side.begin(), side.end(), price, [&](const MergedLevel& level, PriceTicks target) {
        return better(level.price, target);
    });
    return static_cast<size_t>(it - side.begin());
}

// Position of `slot` in this level's attribution, or venue_count if absent.
//
// a linear scan, and a map here would be strictly worse. The array holds
// at most kMaxVenues (8) entries in ONE cache line's worth of memory that the
// level being modified has already pulled in - so the scan is a few compares
// against data that is already there, while a map would add an allocation per
// level and a pointer chase per lookup to avoid them.
uint8_t FindVenue(const MergedLevel& level, VenueSlot slot) {
    uint8_t i = 0;
    for (; i < level.venue_count; ++i) {
        if (level.venues[i].slot == slot) {
            break;
        }
    }
    return i;
}

// Applies one venue's transition at one price. Returns the bytes memmoved by a
// level insert or erase - zero for the common case, which changes only a
// quantity inside an existing level.
//
// attribution is kept sorted by SLOT, because MergeBooks fills it in slot
// order (its merge loop index IS the slot) and the differential test compares
// the two element by element. Leaving the order to fall out of arrival order
// would make the incremental book unequal to a full rebuild in a way that is
// invisible in every published number and fails only the oracle.
template <typename Better>
uint64_t ApplyOneChange(std::vector<MergedLevel>& side, VenueSlot slot, const LevelChange& change, Better better) {
    const size_t index = LowerBoundByRank(side, change.price, better);
    const bool present = index < side.size() && side[index].price == change.price;

    if (!present) {
        // A price no venue was quoting. A removal here is a no-op rather than
        // an error: FlatOrderBook does not emit one (it drops deletes of absent
        // levels), but a full rebuild would also simply not produce the level.
        if (change.new_qty == 0) {
            return 0;
        }
        MergedLevel level;  // deliberately not zero-initialised - see MergedLevel
        level.price = change.price;
        level.venues[0] = {slot, change.new_qty};
        level.venue_count = 1;
        level.qty = change.new_qty;

        const uint64_t moved = sizeof(MergedLevel) * (side.size() - index);
        side.insert(side.begin() + static_cast<ptrdiff_t>(index), level);
        return moved;
    }

    MergedLevel& level = side[index];
    const uint8_t at = FindVenue(level, slot);

    if (change.new_qty == 0) {
        if (at == level.venue_count) {
            return 0;  // this venue was not contributing here anyway
        }
        // subtract what WE recorded for this venue, not change.old_qty.
        // The two should always agree, but only one of them is the number this
        // level's total was actually built from - so using ours keeps the total
        // and the attribution beside it consistent by construction, even if the
        // venue book and the consolidated book ever disagreed. It is also why
        // nothing here reads old_qty at all.
        level.qty -= level.venues[at].qty;

        // Close the gap, preserving slot order.
        for (uint8_t i = at; i + 1 < level.venue_count; ++i) {
            level.venues[i] = level.venues[i + 1];
        }
        --level.venue_count;

        if (level.venue_count == 0) {
            // The last contributor left, so the consolidated price is gone. A
            // level kept at qty 0 would still be published, and would still be
            // swept by the band walk as a real price with no liquidity behind
            // it.
            const uint64_t moved = sizeof(MergedLevel) * (side.size() - index - 1);
            side.erase(side.begin() + static_cast<ptrdiff_t>(index));
            return moved;
        }
        return 0;
    }

    if (at < level.venue_count) {
        // The common case: two stores, and the level total moves by the
        // difference without re-summing the other contributors.
        level.qty = level.qty - level.venues[at].qty + change.new_qty;
        level.venues[at].qty = change.new_qty;
        return 0;
    }

    // A venue joining a price others already quote. Shift the higher slots up
    // by one to keep the array sorted.
    //
    // The bound is provable - a venue appears at most once in a level, and
    // there are at most kMaxVenues of them - so this guard should never fire.
    // It is here because the proof depends on two constants agreeing, and in
    // this codebase they have disagreed once already (consolidated_book.h):
    // overrunning corrupts the neighbouring level silently, whereas dropping
    // one contributor is visible and bounded.
    if (level.venue_count < level.venues.size()) {
        uint8_t insert_at = 0;
        while (insert_at < level.venue_count && level.venues[insert_at].slot < slot) {
            ++insert_at;
        }
        for (uint8_t i = level.venue_count; i > insert_at; --i) {
            level.venues[i] = level.venues[i - 1];
        }
        level.venues[insert_at] = {slot, change.new_qty};
        ++level.venue_count;
        level.qty += change.new_qty;
    }
    return 0;
}

}  // namespace

State::State() {
    book_.bids.reserve(kInitialStateCapacity);
    book_.asks.reserve(kInitialStateCapacity);
}

void State::ApplyDepthDelta(VenueSlot slot, std::span<const LevelChange> bid_changes,
                            std::span<const LevelChange> ask_changes) {
    uint64_t moved = 0;

    // Bids rank high-price-first, asks low-price-first. That single difference
    // is the whole of the two sides' asymmetry, which is why it is a comparator
    // rather than two copies of this loop.
    for (const LevelChange& change : bid_changes) {
        moved += ApplyOneChange(book_.bids, slot, change, std::greater<PriceTicks>{});
    }
    for (const LevelChange& change : ask_changes) {
        moved += ApplyOneChange(book_.asks, slot, change, std::less<PriceTicks>{});
    }

    last_bytes_moved_ = moved;
    total_bytes_moved_ += moved;
}

template <typename BookArray>
void State::FullRebuild(const BookArray& books, size_t venue_count, const VenueHealthArray* health) {
    // Straight into the published book. There is no scratch buffer and no
    // swap: nothing is reading this object while the consolidator thread is
    // inside FullRebuild, because the only reader is the callback that this
    // same thread invokes afterwards.
    MergeBooks(books, venue_count, book_, kUnboundedDepth, health);

    last_bytes_moved_ = 0;
    needs_rebuild_ = false;
}

// FlatOrderBook only. MapOrderBook is still the per-venue oracle, but it emits
// no LevelChange, so it can never drive the incremental path - and a State that
// could only ever be rebuilt is not a State. The oracle for THIS class is
// MergeBooks itself, compared against the incremental result.
template void State::FullRebuild<FlatBookArray>(const FlatBookArray&, size_t, const VenueHealthArray*);

void State::StampForPublish(int64_t source_mono_ns, const std::array<uint32_t, kMaxVenues>& venue_levels) {
    // Set here rather than by the merge, for the same reason it always was:
    // MergeBooks knows nothing about provenance, and Core is the one component
    // holding both the originating update and the output.
    book_.source_mono_ns = source_mono_ns;
    book_.venue_levels = venue_levels;
}

}  // namespace consolidated
}  // namespace market_data
