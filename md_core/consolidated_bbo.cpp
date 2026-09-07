#include "consolidated_bbo.h"

#include <algorithm>

namespace market_data {
namespace consolidated {

template <typename BookArray>
BBO ComputeBBO(const BookArray& books) {
    BBO result;
    auto& best_bid = result.best_bid;
    auto& best_ask = result.best_ask;

    for (size_t i = 0; i < books.size(); ++i) {
        if (!books[i]) {
            continue;  // venue not configured for this instrument
        }

        // The loop index IS the slot - no lookup, and no VenueId anywhere.
        const VenueSlot slot = static_cast<VenueSlot>(i);

        if (auto bid = books[i]->BestBid()) {
            auto [price, qty] = *bid;
            if (0 == best_bid.price || price > best_bid.price) {
                best_bid = ConsolidatedPriceLevel{price, qty, {{slot, qty}}};
            } else if (price == best_bid.price) {
                best_bid.total_qty += qty;
                best_bid.venues.push_back({slot, qty});
            }
        }

        if (auto ask = books[i]->BestAsk()) {
            auto [price, qty] = *ask;
            if (0 == best_ask.price || price < best_ask.price) {
                best_ask = ConsolidatedPriceLevel{price, qty, {{slot, qty}}};
            } else if (price == best_ask.price) {
                best_ask.total_qty += qty;
                best_ask.venues.push_back({slot, qty});
            }
        }
    }

    if (best_bid.price && best_ask.price) {
        // Compare-then-subtract, never a raw subtraction of two unsigned
        // values that could go negative - same convention as types.h.
        result.crossed = best_bid.price >= best_ask.price;
    } else {
        result.crossed = false;
    }

    return result;
}

// The set of book implementations (consolidated_bbo.h): one production book
// today. A second is one more line here.
template BBO ComputeBBO<FlatBookArray>(const FlatBookArray&);

// Resets a level to a single venue WITHOUT reallocating: clear() keeps the
// vector's capacity, so after Core reserves once at init this never
// allocates again. Assigning a
// whole ConsolidatedPriceLevel instead would move-steal the temporary's
// 1-element buffer and drop the reserved one.
static void SetSingleVenue(ConsolidatedPriceLevel& level, PriceTicks price, VenueSlot slot, QtyUnits qty) {
    level.price = price;
    level.total_qty = qty;
    level.venues.clear();
    level.venues.push_back({slot, qty});
}

// One side's full scan, factored out so ComputeBBOFromQuotes and the rescan
// path share exactly one implementation - and so a rescan only recomputes
// the side that actually collapsed, not both. Fills `out` in place rather
// than returning by value, so the rescan path also reuses its buffer.
static void ScanBestBid(const VenueQuoteArray& quotes, ConsolidatedPriceLevel& out, const VenueHealthArray* health) {
    out.price = 0;
    out.total_qty = 0;
    out.venues.clear();
    for (size_t i = 0; i < quotes.size(); ++i) {
        const BboQuote& quote = quotes[i];
        if (quote.bid_price == 0) {
            continue;  // venue has sent no quote yet
        }
        // The staleness filter lives HERE, in the shared scan, rather than in
        // each caller - which is what makes ComputeBBOFromQuotes and the
        // rescan path inside UpdateBBOWithQuote agree by construction. Two
        // copies of this rule could disagree, and the disagreement would only
        // show on the rare rescan path.
        if (health != nullptr && !IsAdmissible((*health)[i])) {
            continue;
        }
        // The array index IS the slot: ApplyQuote stores each venue's quote
        // at its own slot, so position and identity are the same thing now.
        const VenueSlot slot = static_cast<VenueSlot>(i);
        if (out.price == 0 || quote.bid_price > out.price) {
            SetSingleVenue(out, quote.bid_price, slot, quote.bid_qty);
        } else if (quote.bid_price == out.price) {
            out.total_qty += quote.bid_qty;
            out.venues.push_back({slot, quote.bid_qty});
        }
    }
}

static void ScanBestAsk(const VenueQuoteArray& quotes, ConsolidatedPriceLevel& out, const VenueHealthArray* health) {
    out.price = 0;
    out.total_qty = 0;
    out.venues.clear();
    for (size_t i = 0; i < quotes.size(); ++i) {
        const BboQuote& quote = quotes[i];
        if (quote.ask_price == 0) {
            continue;
        }
        if (health != nullptr && !IsAdmissible((*health)[i])) {
            continue;
        }
        // Same as ScanBestBid: the array index is the slot.
        const VenueSlot slot = static_cast<VenueSlot>(i);
        if (out.price == 0 || quote.ask_price < out.price) {  // asks: lower is better
            SetSingleVenue(out, quote.ask_price, slot, quote.ask_qty);
        } else if (quote.ask_price == out.price) {
            out.total_qty += quote.ask_qty;
            out.venues.push_back({slot, quote.ask_qty});
        }
    }
}

// New price == best
// - venue already in array -> update qty, total = total - old + new
// - venue not in array     -> append, total += qty
//
// Takes venue+qty explicitly rather than a quote: a BboQuote has separate
// bid_qty and ask_qty, and this is called for both sides.
static void UpdateEqualPrice(ConsolidatedPriceLevel& bbo_level, VenueSlot slot, QtyUnits qty) {
    auto exist_it = std::find_if(bbo_level.venues.begin(), bbo_level.venues.end(),
                                 [slot](const VenueQuote& vq) { return vq.slot == slot; });
    if (exist_it != bbo_level.venues.end()) {
        bbo_level.total_qty -= exist_it->qty;
        exist_it->qty = qty;
        bbo_level.total_qty += qty;
    } else {
        bbo_level.total_qty += qty;
        bbo_level.venues.push_back({slot, qty});
    }
}

// New price worse
//  venue not in array -> ignore
//  venue in array     -> erase, total -= its OLD qty (not the incoming one)
//
// Returns true if the venue was present, so the caller can detect the level
// losing its last venue and trigger a rescan.
static bool UpdateWorstPrice(ConsolidatedPriceLevel& bbo_level, VenueSlot slot) {
    auto exist_it = std::find_if(bbo_level.venues.begin(), bbo_level.venues.end(),
                                 [slot](const VenueQuote& vq) { return vq.slot == slot; });
    if (exist_it == bbo_level.venues.end()) {
        return false;
    }
    bbo_level.total_qty -= exist_it->qty;
    bbo_level.venues.erase(exist_it);
    return true;
}

static bool IsCrossed(const ConsolidatedPriceLevel& bid, const ConsolidatedPriceLevel& ask) {
    return (bid.price && ask.price) && bid.price >= ask.price;
}

void ComputeBBOFromQuotesInto(const VenueQuoteArray& quotes, BBO& out, const VenueHealthArray* health) {
    ScanBestBid(quotes, out.best_bid, health);
    ScanBestAsk(quotes, out.best_ask, health);
    out.crossed = IsCrossed(out.best_bid, out.best_ask);
}

BBO ComputeBBOFromQuotes(const VenueQuoteArray& quotes, const VenueHealthArray* health) {
    // Returns by value: this is the oracle and the initial build, not a hot
    // path, so the fresh vectors here are fine. Delegates rather than
    // duplicating, so the two can never disagree.
    BBO result;
    ComputeBBOFromQuotesInto(quotes, result, health);
    return result;
}

void UpdateBBOWithQuote(BBO& current, const BboQuote& update, VenueSlot slot, const VenueQuoteArray& quotes,
                        const VenueHealthArray* health) {
    // A quote from a venue that is not admitted is folded in as though it
    // carried no prices at all. That is not a shortcut: "this venue has no
    // bid" and "this venue's bid may not be used" have the SAME effect on the
    // consolidated top of book, and the price == 0 branches below already
    // remove the venue from the best level and rescan if it was the last one
    // there. Adding a separate exclusion path would duplicate that logic.
    //
    // Indexed by SLOT, not by VenueId - `health` is a per-slot array, and the
    // two are the same number only while venues register in enum order. The
    // bound is kMaxVenues for the same reason: it is the size of the array
    // actually being indexed.
    const size_t venue_index = VenueSlotIndex(slot);
    const bool admitted = health == nullptr || venue_index >= kMaxVenues || IsAdmissible((*health)[venue_index]);
    const PriceTicks bid_price = admitted ? update.bid_price : 0;
    const PriceTicks ask_price = admitted ? update.ask_price : 0;

    // ---- bid side: better means HIGHER ----
    ConsolidatedPriceLevel& best_bid = current.best_bid;
    if (bid_price == 0) {
        // Venue has no bid at all. If it was holding the best level, it left.
        if (UpdateWorstPrice(best_bid, slot) && best_bid.venues.empty()) {
            ScanBestBid(quotes, best_bid, health);
        }
    } else if (best_bid.price == 0 || bid_price > best_bid.price) {
        // Strictly better (or first data): this venue alone owns the level.
        SetSingleVenue(best_bid, bid_price, slot, update.bid_qty);
    } else if (bid_price == best_bid.price) {
        UpdateEqualPrice(best_bid, slot, update.bid_qty);
    } else {
        // Worse than the best. Only matters if this venue WAS holding the
        // best level and has now moved off it.
        if (UpdateWorstPrice(best_bid, slot) && best_bid.venues.empty()) {
            // The best level lost its last venue. The new best is not
            // recoverable from `current` alone - it only ever kept the top
            // level - so rescan the per-venue array.
            ScanBestBid(quotes, best_bid, health);
        }
    }

    // ---- ask side: better means LOWER ----
    ConsolidatedPriceLevel& best_ask = current.best_ask;
    if (ask_price == 0) {
        if (UpdateWorstPrice(best_ask, slot) && best_ask.venues.empty()) {
            ScanBestAsk(quotes, best_ask, health);
        }
    } else if (best_ask.price == 0 || ask_price < best_ask.price) {
        SetSingleVenue(best_ask, ask_price, slot, update.ask_qty);
    } else if (ask_price == best_ask.price) {
        UpdateEqualPrice(best_ask, slot, update.ask_qty);
    } else {
        if (UpdateWorstPrice(best_ask, slot) && best_ask.venues.empty()) {
            ScanBestAsk(quotes, best_ask, health);
        }
    }

    current.crossed = IsCrossed(best_bid, best_ask);
}

}  // namespace consolidated
}  // namespace market_data