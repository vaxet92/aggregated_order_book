#include "flat_order_book.h"

#include <algorithm>
#include <functional>

namespace market_data {

FlatOrderBook::FlatOrderBook(VenueId venue, InstrumentKey instrument) : venue_(venue), instrument_(instrument) {
    // Reserved once, here, so a warmed-up book never allocates while applying
    // an update - including the resize() inside Relocate, which only adjusts
    // the size when the capacity already covers it.
    bids_.reserve(kInitialLevelCapacity);
    asks_.reserve(kInitialLevelCapacity);
    scratch_.reserve(kInitialLevelCapacity);
    merged_.reserve(kInitialLevelCapacity);
    last_bid_changes_.reserve(kInitialChangeCapacity);
    last_ask_changes_.reserve(kInitialChangeCapacity);
}

std::optional<std::pair<PriceTicks, QtyUnits>> FlatOrderBook::BestBid() const {
    return bids_.empty() ? std::nullopt : std::make_optional(std::make_pair(bids_.back().price, bids_.back().qty));
}

std::optional<std::pair<PriceTicks, QtyUnits>> FlatOrderBook::BestAsk() const {
    return asks_.empty() ? std::nullopt : std::make_optional(std::make_pair(asks_.back().price, asks_.back().qty));
}

template <typename StorageLess>
uint64_t FlatOrderBook::Relocate(std::vector<PriceLevel>& side, size_t deepest, const PriceLevel* base, ptrdiff_t step,
                                 size_t m, StorageLess storage_less) {
    auto delta = [&](size_t i) -> const PriceLevel& { return base[step * static_cast<ptrdiff_t>(i)]; };

    const size_t n = side.size();

    // Merge the touched region with the delta into the staging buffer. Both run
    // FORWARD here, in storage order, because merged_ is not one of the inputs
    // and there is no aliasing to reason about - see the header for the in-place
    // version this replaced and the delta that broke it.
    merged_.clear();  // keeps capacity
    size_t read = deepest;
    size_t j = 0;

    while (read < n && j < m) {
        const PriceLevel& book_level = side[read];
        const PriceLevel& new_level = delta(j);

        if (storage_less(book_level.price, new_level.price)) {
            merged_.push_back(book_level);
            ++read;
        } else if (storage_less(new_level.price, book_level.price)) {
            // A price the book does not have. qty 0 here removes something
            // already absent - a no-op, not an error: venues resend deletions,
            // and after a snapshot the level may genuinely be gone.
            if (new_level.qty != 0) {
                merged_.push_back(new_level);
            }
            ++j;
        } else {
            // Same price: a BookUpdate is absolute, so it replaces the quantity
            // outright, and qty 0 means remove this level (types.h).
            if (new_level.qty != 0) {
                merged_.push_back(new_level);
            }
            ++read;
            ++j;
        }
    }
    while (read < n) {
        merged_.push_back(side[read++]);
    }
    while (j < m) {
        if (delta(j).qty != 0) {
            merged_.push_back(delta(j));
        }
        ++j;
    }

    // resize before the copy: growing needs the slots to exist, shrinking drops
    // the tail the erases freed. Capacity is reserved in the constructor, so
    // this adjusts the size without reallocating.
    side.resize(deepest + merged_.size());
    std::copy(merged_.begin(), merged_.end(), side.begin() + static_cast<ptrdiff_t>(deepest));

    // Counts the WRITE-BACK only. Staging costs an equal-sized pass again, so
    // the true traffic is twice this - stated rather than folded in, so the
    // number stays readable as "levels relocated".
    return sizeof(PriceLevel) * merged_.size();
}

template <typename StorageLess>
uint64_t FlatOrderBook::ApplySide(std::vector<PriceLevel>& side, const std::vector<PriceLevel>& levels,
                                  std::vector<LevelChange>* changes, StorageLess storage_less) {
    if (levels.empty()) {
        return 0;  // counted as neither fast path nor rebuild: no work was done
    }
    ++last_applied_sides_;

    const size_t m = levels.size();

    // Resolve the delta into STORAGE order without copying it where possible.
    //
    // in the common case the delta arrives in exactly REVERSE storage
    // order, on both sides. Venues send best-first; bids are stored ascending
    // and asks descending, so best-first is the reverse of storage both times.
    // Stepping backwards costs nothing and avoids copying the delta.
    //
    // the third branch is not defensive padding. The std::map version's
    // insertion hint is ADVISORY - unsorted input there costs a normal tree
    // search and the book is still correct (map_order_book.cpp). A merge pass
    // has no such fallback: unsorted input would silently corrupt the book. Two
    // linear scans of a 1-20 element array cost a few nanoseconds and remove
    // that failure mode entirely, rather than trusting every venue forever.
    auto ordered_from = [&](const PriceLevel* first, ptrdiff_t stride) {
        for (size_t i = 1; i < m; ++i) {
            if (storage_less(first[stride * static_cast<ptrdiff_t>(i)].price,
                             first[stride * static_cast<ptrdiff_t>(i - 1)].price)) {
                return false;
            }
        }
        return true;
    };

    const PriceLevel* base = levels.data();
    ptrdiff_t step = 1;
    if (ordered_from(levels.data(), 1)) {
        // already in storage order
    } else if (ordered_from(levels.data() + m - 1, -1)) {
        base = levels.data() + m - 1;
        step = -1;
    } else {
        scratch_.assign(levels.begin(), levels.end());
        std::sort(scratch_.begin(), scratch_.end(),
                  [&](const PriceLevel& a, const PriceLevel& b) { return storage_less(a.price, b.price); });
        base = scratch_.data();
        step = 1;
    }
    auto delta = [&](size_t i) -> const PriceLevel& { return base[step * static_cast<ptrdiff_t>(i)]; };

    // ---- one walk: apply quantities, count what enters and leaves -----------
    //
    // Backward from back() - the best price - against the delta best-first.
    // Best-first is the resolved storage order read from m-1 down to 0, so no
    // second ordering pass is needed.
    //
    // this stops the moment the delta runs out. A delta touching only the
    // top of book never reads the deep end at all, which is the entire reason
    // the best price lives at back(). Cost is O(delta + how deep the delta
    // reaches), never O(book).
    //
    // the quantity writes are committed here, BEFORE we know whether the
    // delta also inserts or erases. That is safe because a BookUpdate is
    // absolute, not incremental (types.h) - if Relocate runs, it re-reads every
    // value from the delta and writes the same numbers again. Idempotence is
    // what makes this one walk instead of classify-then-apply.
    size_t inserts = 0;
    size_t erases = 0;
    size_t deepest = 0;
    {
        size_t i = side.size();  // side[i - 1] is the best book level not yet seen
        size_t j = m;            // delta(j - 1) is the best delta level not yet seen
        while (i > 0 && j > 0) {
            PriceLevel& book_level = side[i - 1];
            const PriceLevel& new_level = delta(j - 1);

            if (storage_less(book_level.price, new_level.price)) {
                // The delta's price beats the best book level left, so no book
                // level carries it - a new price, landing at some index >= i.
                // qty 0 would remove something already absent: no change, and
                // nothing to tell the consolidated book about either.
                if (new_level.qty != 0) {
                    ++inserts;
                    if (changes) {
                        changes->push_back({new_level.price, 0, new_level.qty});
                    }
                }
                --j;
            } else if (storage_less(new_level.price, book_level.price)) {
                --i;  // the delta says nothing about this level
            } else if (new_level.qty == 0) {
                ++erases;
                // Read from the BOOK, not the delta: the delta carries qty 0
                // by definition, and the consolidated level needs to know how
                // much this venue is withdrawing in order to subtract it.
                if (changes) {
                    changes->push_back({book_level.price, book_level.qty, 0});
                }
                --i;
                --j;
            } else {
                // A venue REPEATING a quantity it already sent is not a change.
                // Testing before storing costs one comparison against a line
                // already loaded, and it is what makes a duplicate message free
                // for the consolidated book rather than merely cheap - no
                // change emitted, so no level rewrite and no prefix repair.
                if (book_level.qty != new_level.qty) {
                    if (changes) {
                        changes->push_back({book_level.price, book_level.qty, new_level.qty});
                    }
                    book_level.qty = new_level.qty;  // in place; nothing shifts
                }
                --i;
                --j;
            }
        }
        deepest = i;
        // Anything left in the delta is worse than every level in the book, so
        // it extends the far end.
        while (j > 0) {
            const PriceLevel& new_level = delta(j - 1);
            if (new_level.qty != 0) {
                ++inserts;
                if (changes) {
                    changes->push_back({new_level.price, 0, new_level.qty});
                }
            }
            --j;
        }
    }

    if (inserts == 0 && erases == 0) {
        ++last_fast_path_sides_;
        return 0;  // quantities only: every write was in place
    }
    return Relocate(side, deepest, base, step, m, storage_less);
}

void FlatOrderBook::ApplyUpdate(const BookUpdate& update) {
    // Stamped here rather than in Core's dispatch, for the same reason as
    // MapOrderBook: ApplyUpdate is only reached by a real, sequence-validated
    // depth message, so "a heartbeat must not feed the watchdog" holds by
    // construction instead of by remembering.
    last_update_mono_ns_ = update.recv_mono_ns;

    last_fast_path_sides_ = 0;
    last_applied_sides_ = 0;

    last_bid_changes_.clear();  // keeps capacity
    last_ask_changes_.clear();

    // A snapshot REPLACES both sides, so the levels it carries describe a new
    // book rather than a transition out of the old one: every one of them would
    // look like an insert, and nothing at all would say that the levels they
    // replaced are gone. Collecting is therefore switched off rather than
    // filtered afterwards, and the flag tells the consolidated layer to rebuild
    // instead of repairing - see last_changes_complete().
    last_changes_complete_ = !update.is_snapshot;
    std::vector<LevelChange>* const bid_changes = update.is_snapshot ? nullptr : &last_bid_changes_;
    std::vector<LevelChange>* const ask_changes = update.is_snapshot ? nullptr : &last_ask_changes_;

    if (update.is_snapshot) {
        bids_.clear();
        asks_.clear();
    }

    // std::less for bids gives ascending storage, so back() is the HIGHEST
    // price; std::greater for asks gives descending storage, so back() is the
    // LOWEST. Both put the best price - and therefore nearly all the churn - at
    // the cheap end of the vector.
    last_bytes_moved_ = ApplySide(bids_, update.bids, bid_changes, std::less<PriceTicks>{}) +
                        ApplySide(asks_, update.asks, ask_changes, std::greater<PriceTicks>{});
    total_bytes_moved_ += last_bytes_moved_;

    last_seq_ = update.seq;
}

}  // namespace market_data
