#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
#include <span>
#include <vector>

#include "flat_order_book.h"

using namespace market_data;

namespace {

const InstrumentKey kSpotBtc = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);

BookUpdate MakeUpdate(uint64_t seq, bool is_snapshot, std::vector<PriceLevel> bids, std::vector<PriceLevel> asks) {
    BookUpdate update{VenueId::BINANCE, kSpotBtc, bids.size(), is_snapshot, seq};
    update.bids = std::move(bids);
    update.asks = std::move(asks);
    return update;
}

// Materialises a side in ITERATION order. Works for both books because both
// yield something destructurable into (price, qty): std::map yields a pair,
// FlatOrderBook yields a PriceLevel.
template <typename Range>
std::vector<PriceLevel> Levels(Range&& side) {
    std::vector<PriceLevel> out;
    for (const auto& [price, qty] : side) {
        out.push_back({price, qty});
    }
    return out;
}

// A trivially-correct order book used as the oracle: one
// std::map per side, with each delta level applied directly - qty 0 erases,
// anything else assigns. std::map removed as a production book; this is the
// two-line reimplementation the flat book's ApplySide merge is still checked
// against, because two unrelated implementations agreeing from the same input
// is what an oracle is for.
struct RefBook {
    std::map<PriceTicks, QtyUnits> bids;  // ascending; best (highest) is rbegin()
    std::map<PriceTicks, QtyUnits> asks;  // ascending; best (lowest) is begin()

    void Apply(const BookUpdate& update) {
        auto apply_side = [](std::map<PriceTicks, QtyUnits>& side, const std::vector<PriceLevel>& levels) {
            for (const PriceLevel& level : levels) {
                if (level.qty == 0) {
                    side.erase(level.price);
                } else {
                    side[level.price] = level.qty;
                }
            }
        };
        apply_side(bids, update.bids);
        apply_side(asks, update.asks);
    }
};

// The oracle comparison. Compares SEQUENCES, not sets, so it verifies ordering
// as well as contents: the flat book presents best-first by reversing a vector
// stored worst-first, the map by iterating its ordered keys - two unrelated
// mechanisms arriving at the same order. Comparing membership alone would pass
// even with a side stored backwards.
void ExpectSameBook(const RefBook& oracle, const FlatOrderBook& flat, int update_index) {
    std::vector<PriceLevel> oracle_bids;  // best-first == descending price
    for (auto it = oracle.bids.rbegin(); it != oracle.bids.rend(); ++it) {
        oracle_bids.push_back({it->first, it->second});
    }
    const std::vector<PriceLevel> flat_bids = Levels(flat.bids());
    ASSERT_EQ(flat_bids.size(), oracle_bids.size()) << "bid level count, update " << update_index;
    for (size_t i = 0; i < oracle_bids.size(); ++i) {
        ASSERT_EQ(flat_bids[i].price, oracle_bids[i].price) << "bid price at " << i << ", update " << update_index;
        ASSERT_EQ(flat_bids[i].qty, oracle_bids[i].qty) << "bid qty at " << i << ", update " << update_index;
    }

    std::vector<PriceLevel> oracle_asks;  // best-first == ascending price
    for (const auto& [price, qty] : oracle.asks) {
        oracle_asks.push_back({price, qty});
    }
    const std::vector<PriceLevel> flat_asks = Levels(flat.asks());
    ASSERT_EQ(flat_asks.size(), oracle_asks.size()) << "ask level count, update " << update_index;
    for (size_t i = 0; i < oracle_asks.size(); ++i) {
        ASSERT_EQ(flat_asks[i].price, oracle_asks[i].price) << "ask price at " << i << ", update " << update_index;
        ASSERT_EQ(flat_asks[i].qty, oracle_asks[i].qty) << "ask qty at " << i << ", update " << update_index;
    }

    // BestBid/BestAsk read back() on the flat book - a separate code path from
    // the iteration above, not a restatement of it.
    ASSERT_EQ(flat.BestBid().has_value(), !oracle.bids.empty()) << "update " << update_index;
    if (!oracle.bids.empty()) {
        EXPECT_EQ(flat.BestBid()->first, oracle.bids.rbegin()->first) << "best bid price, update " << update_index;
        EXPECT_EQ(flat.BestBid()->second, oracle.bids.rbegin()->second) << "best bid qty, update " << update_index;
    }
    ASSERT_EQ(flat.BestAsk().has_value(), !oracle.asks.empty()) << "update " << update_index;
    if (!oracle.asks.empty()) {
        EXPECT_EQ(flat.BestAsk()->first, oracle.asks.begin()->first) << "best ask price, update " << update_index;
        EXPECT_EQ(flat.BestAsk()->second, oracle.asks.begin()->second) << "best ask qty, update " << update_index;
    }
}

// One side of a random multi-level delta.
//
// distinct prices only. A delta naming the same price twice has no defined
// meaning, and no venue sends one. Generating them would test our tolerance of a
// malformed message rather than our handling of a real one.
//
// `order` picks which of ApplySide's three input-ordering branches is exercised:
//   0: BEST FIRST - what every venue actually sends, and the reverse of how
//      FlatOrderBook stores it (the common branch).
//   1: WORST FIRST - already in storage order.
//   2: shuffled - the branch that must sort before merging.
std::vector<PriceLevel> MakeRandomSide(std::mt19937& rng, uint64_t px_low, uint64_t px_high, bool best_is_high,
                                       int order) {
    std::uniform_int_distribution<uint64_t> price_dist(px_low, px_high);
    std::uniform_int_distribution<uint64_t> qty(0, 9);  // 0 means remove
    std::uniform_int_distribution<int> level_count(1, 8);

    // Drawn ONCE. Inside the loop condition it would be re-drawn every
    // iteration, so the delta size would be whatever the last draw happened to
    // be rather than a chosen count.
    const size_t wanted = static_cast<size_t>(level_count(rng));
    std::vector<uint64_t> prices;
    while (prices.size() < wanted) {
        const uint64_t price = price_dist(rng);
        if (std::find(prices.begin(), prices.end(), price) == prices.end()) {
            prices.push_back(price);
        }
    }

    if (order == 0) {
        std::sort(prices.begin(), prices.end(), [&](uint64_t a, uint64_t b) { return best_is_high ? a > b : a < b; });
    } else if (order == 1) {
        std::sort(prices.begin(), prices.end(), [&](uint64_t a, uint64_t b) { return best_is_high ? a < b : a > b; });
    } else {
        std::shuffle(prices.begin(), prices.end(), rng);
    }

    std::vector<PriceLevel> levels;
    levels.reserve(prices.size());
    for (uint64_t price : prices) {
        levels.push_back({price, qty(rng)});
    }
    return levels;
}

// A side rebuilt ONLY from the LevelChange stream - never read from the book.
//
// that independence is the whole point. If this were seeded or repaired
// from FlatOrderBook it would agree with it trivially; built purely from the
// changes, agreeing means the stream is COMPLETE (nothing the book did went
// unreported) and CORRECTLY VALUED.
using ShadowSide = std::map<PriceTicks, QtyUnits>;

// Replays one side's changes into the shadow, checking the three properties the
// incremental consolidated merge is going to depend on.
//
// old_qty is checked against what the shadow ALREADY believes, not against
// the book. That is the property that matters downstream: the consolidated level
// subtracts old_qty from a total it accumulated itself, so an old_qty that
// disagrees with the previous state silently corrupts every future total at that
// price rather than failing at the point of the mistake.
void ReplayChanges(ShadowSide& shadow, std::span<const LevelChange> changes, bool best_is_high, int update_index) {
    bool have_previous = false;
    PriceTicks previous = 0;

    for (const LevelChange& change : changes) {
        // Emitted best-first, which is the consolidated book's storage order on
        // both sides. Phase 2's price search relies on it advancing rather than
        // rewinding, so it is asserted here where a violation is one update from
        // its cause.
        if (have_previous) {
            const bool worsening = best_is_high ? (change.price < previous) : (change.price > previous);
            ASSERT_TRUE(worsening) << "changes not best-first: " << previous << " then " << change.price << ", update "
                                   << update_index;
        }
        previous = change.price;
        have_previous = true;

        ASSERT_NE(change.old_qty, change.new_qty)
            << "no-op change emitted at " << change.price << ", update " << update_index;

        const auto it = shadow.find(change.price);
        if (change.old_qty == 0) {
            ASSERT_EQ(it, shadow.end()) << "insert at " << change.price << " which already exists, update "
                                        << update_index;
        } else {
            ASSERT_NE(it, shadow.end()) << "change at " << change.price << " which does not exist, update "
                                        << update_index;
            ASSERT_EQ(it->second, change.old_qty)
                << "old_qty disagrees with prior state at " << change.price << ", update " << update_index;
        }

        if (change.new_qty == 0) {
            shadow.erase(change.price);
        } else {
            shadow[change.price] = change.new_qty;
        }
    }
}

// The shadow and the book must hold exactly the same levels - neither extra nor
// missing, which is why the count is compared as well as every entry.
template <typename Range>
void ExpectShadowMatches(const ShadowSide& shadow, Range&& side, const char* what, int update_index) {
    size_t seen = 0;
    for (const auto& [price, qty] : side) {
        const auto it = shadow.find(price);
        ASSERT_NE(it, shadow.end()) << what << " " << price << " is in the book but not the change stream, update "
                                    << update_index;
        ASSERT_EQ(it->second, qty) << what << " qty at " << price << ", update " << update_index;
        ++seen;
    }
    ASSERT_EQ(shadow.size(), seen) << what << " change stream holds levels the book does not, update " << update_index;
}

}  // namespace

TEST(FlatOrderBookTest, EmptyBookHasNoBestBidOrAsk) {
    FlatOrderBook book(VenueId::BINANCE, kSpotBtc);

    EXPECT_FALSE(book.BestBid().has_value());
    EXPECT_FALSE(book.BestAsk().has_value());
    EXPECT_EQ(book.total_bytes_moved(), 0u);
}

// The main correctness check for ApplySide's merge pass.
//
// the deltas here are MULTI-LEVEL and their ordering VARIES, and both
// properties are load-bearing. A single-level delta cannot exercise a merge at
// all - with one level there is nothing to merge and no order to get wrong. And
// ApplySide takes three different branches depending on the delta's order
// (storage order, reverse-storage order, neither), so a generator that only
// ever emitted sorted input would leave the branch that PREVENTS SILENT BOOK
// CORRUPTION completely untested.
TEST(FlatOrderBookTest, RandomMultiLevelDeltasMatchTheOracle) {
    std::mt19937 rng(4242);  // fixed seed - reproducible failures
    std::uniform_int_distribution<int> ordering(0, 2);

    RefBook oracle;
    FlatOrderBook flat(VenueId::BINANCE, kSpotBtc);

    for (int i = 0; i < 5000; ++i) {
        const int order = ordering(rng);
        const BookUpdate update =
            MakeUpdate(static_cast<uint64_t>(i + 1), false, MakeRandomSide(rng, 900, 999, /*best_is_high=*/true, order),
                       MakeRandomSide(rng, 1000, 1099, /*best_is_high=*/false, order));

        oracle.Apply(update);
        flat.ApplyUpdate(update);

        ASSERT_NO_FATAL_FAILURE(ExpectSameBook(oracle, flat, i));

        // Bid and ask ranges cannot overlap by construction, so a cross here
        // means the book is corrupt, not that the market moved.
        if (auto bid = flat.BestBid(); bid && flat.BestAsk()) {
            ASSERT_LT(bid->first, flat.BestAsk()->first) << "flat book crossed at update " << i;
        }
    }
}

// The safety net for the incremental consolidated merge, and it has to exist
// BEFORE that merge does: the merge repairs a persistent book from these
// changes, so a change stream that is merely mostly right produces a
// consolidated book that drifts silently over thousands of updates and never
// fails at the point of the mistake.
//
// this checks something the existing oracle test cannot. That test proves
// the flat book ENDS UP correct; this proves the book correctly REPORTS how it
// got there. A book that applied every delta perfectly while emitting nothing
// at all would pass the test above and fail this one.
TEST(FlatOrderBookTest, LevelChangeStreamReproducesTheBook) {
    std::mt19937 rng(4242);
    std::uniform_int_distribution<int> ordering(0, 2);

    FlatOrderBook flat(VenueId::BINANCE, kSpotBtc);
    ShadowSide shadow_bids;
    ShadowSide shadow_asks;

    for (int i = 0; i < 5000; ++i) {
        const int order = ordering(rng);
        const BookUpdate update =
            MakeUpdate(static_cast<uint64_t>(i + 1), false, MakeRandomSide(rng, 900, 999, /*best_is_high=*/true, order),
                       MakeRandomSide(rng, 1000, 1099, /*best_is_high=*/false, order));

        flat.ApplyUpdate(update);

        ASSERT_TRUE(flat.last_changes_complete()) << "delta update reported an incomplete change set, update " << i;
        ASSERT_NO_FATAL_FAILURE(ReplayChanges(shadow_bids, flat.last_bid_changes(), /*best_is_high=*/true, i));
        ASSERT_NO_FATAL_FAILURE(ReplayChanges(shadow_asks, flat.last_ask_changes(), /*best_is_high=*/false, i));

        ASSERT_NO_FATAL_FAILURE(ExpectShadowMatches(shadow_bids, flat.bids(), "bid", i));
        ASSERT_NO_FATAL_FAILURE(ExpectShadowMatches(shadow_asks, flat.asks(), "ask", i));
    }
}

// The two ways a delta level can say nothing, both of which must cost the
// consolidated book nothing.
//
// "emits no change" is a stronger claim than "leaves the book unchanged",
// and only the first one matters here. A duplicate that emitted a change with
// old_qty == new_qty would leave the book correct and still force a level
// rewrite plus an O(depth) prefix repair on every repeated message - which on a
// feed that resends unchanged levels is most of the traffic.
TEST(FlatOrderBookTest, UnchangedLevelsEmitNoChange) {
    FlatOrderBook flat(VenueId::BINANCE, kSpotBtc);

    flat.ApplyUpdate(MakeUpdate(1, false, {{100, 5}, {99, 7}}, {{101, 3}}));
    EXPECT_EQ(flat.last_bid_changes().size(), 2u);
    EXPECT_EQ(flat.last_ask_changes().size(), 1u);

    // Same quantities again: the venue is repeating itself.
    flat.ApplyUpdate(MakeUpdate(2, false, {{100, 5}, {99, 7}}, {{101, 3}}));
    EXPECT_TRUE(flat.last_bid_changes().empty()) << "repeated quantity reported as a change";
    EXPECT_TRUE(flat.last_ask_changes().empty()) << "repeated quantity reported as a change";

    // qty 0 at a price the book does not hold: removing something already
    // absent. Venues resend deletions, so this is routine, not malformed.
    flat.ApplyUpdate(MakeUpdate(3, false, {{50, 0}}, {{500, 0}}));
    EXPECT_TRUE(flat.last_bid_changes().empty()) << "delete of an absent level reported as a change";
    EXPECT_TRUE(flat.last_ask_changes().empty()) << "delete of an absent level reported as a change";

    // One real move mixed in with two repeats - only the move is reported.
    flat.ApplyUpdate(MakeUpdate(4, false, {{100, 5}, {99, 8}}, {{101, 3}}));
    ASSERT_EQ(flat.last_bid_changes().size(), 1u);
    EXPECT_EQ(flat.last_bid_changes()[0].price, 99u);
    EXPECT_EQ(flat.last_bid_changes()[0].old_qty, 7u);
    EXPECT_EQ(flat.last_bid_changes()[0].new_qty, 8u);
    EXPECT_TRUE(flat.last_ask_changes().empty());
}

// A removal must carry the quantity being WITHDRAWN, read from the book, not
// the 0 the delta carries. The consolidated level subtracts old_qty from a
// total it accumulated itself, so a 0 here would leave that venue's liquidity
// in the merged book forever.
TEST(FlatOrderBookTest, RemovalCarriesTheOutgoingQuantity) {
    FlatOrderBook flat(VenueId::BINANCE, kSpotBtc);

    flat.ApplyUpdate(MakeUpdate(1, false, {{100, 5}, {99, 7}}, {}));
    flat.ApplyUpdate(MakeUpdate(2, false, {{99, 0}}, {}));

    ASSERT_EQ(flat.last_bid_changes().size(), 1u);
    EXPECT_EQ(flat.last_bid_changes()[0].price, 99u);
    EXPECT_EQ(flat.last_bid_changes()[0].old_qty, 7u) << "removal must report what it withdraws";
    EXPECT_EQ(flat.last_bid_changes()[0].new_qty, 0u);
}

// A snapshot REPLACES the side, so its levels are not a transition out of the
// previous book and must never be applied to a consolidated one.
//
// the spans are left EMPTY, which is indistinguishable from "nothing
// moved" - a legitimate and common outcome. last_changes_complete() is what
// separates the two, and a consolidated layer that ignored it would keep every
// level the snapshot replaced, forever.
TEST(FlatOrderBookTest, SnapshotReportsAnIncompleteChangeSet) {
    FlatOrderBook flat(VenueId::BINANCE, kSpotBtc);

    flat.ApplyUpdate(MakeUpdate(1, false, {{100, 5}, {99, 7}}, {{101, 3}}));
    EXPECT_TRUE(flat.last_changes_complete());

    // A snapshot with entirely different prices - the old ones are gone, and
    // nothing in a change stream could say so.
    flat.ApplyUpdate(MakeUpdate(2, true, {{200, 1}}, {{201, 2}}));

    EXPECT_FALSE(flat.last_changes_complete()) << "snapshot must not present its levels as a transition";
    EXPECT_TRUE(flat.last_bid_changes().empty());
    EXPECT_TRUE(flat.last_ask_changes().empty());

    // And the very next delta is complete again - the flag is per update, not
    // sticky.
    flat.ApplyUpdate(MakeUpdate(3, false, {{200, 4}}, {}));
    EXPECT_TRUE(flat.last_changes_complete());
    ASSERT_EQ(flat.last_bid_changes().size(), 1u);
    EXPECT_EQ(flat.last_bid_changes()[0].old_qty, 1u);
    EXPECT_EQ(flat.last_bid_changes()[0].new_qty, 4u);
}
