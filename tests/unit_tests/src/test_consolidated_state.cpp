#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <span>
#include <vector>

#include "consolidated_book.h"
#include "consolidated_state.h"
#include "flat_order_book.h"

using namespace market_data;
using consolidated::Book;
using consolidated::MergedLevel;
using consolidated::State;

namespace {

const InstrumentKey kSpotBtc = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);

constexpr size_t kVenues = 3;
constexpr VenueId kVenueIds[kVenues] = {VenueId::BINANCE, VenueId::BYBIT, VenueId::OKX};

// The differential comparison, and the whole reason this file exists.
//
// an incremental consolidated book fails SILENTLY. A mis-subtracted
// quantity or a dropped attribution entry leaves a book that is still sorted,
// still the right length, and still publishes plausible prices - so every
// property one could assert about the incremental book ALONE would pass. Only
// comparing it against an independent full rebuild of the same inputs catches
// it, which is exactly the role MergeBooks now plays.
//
// Every field is compared, including attribution ORDER: a level whose
// contributors are the same set in a different order is a real difference,
// because it means the incremental path is not maintaining the ordering
// MergeBooks produces and the two would diverge on the wire.
void ExpectSameSide(std::span<const MergedLevel> oracle, std::span<const MergedLevel> actual, const char* what,
                    int step) {
    ASSERT_EQ(actual.size(), oracle.size()) << what << " depth, step " << step;

    for (size_t i = 0; i < oracle.size(); ++i) {
        ASSERT_EQ(actual[i].price, oracle[i].price) << what << " price at " << i << ", step " << step;
        ASSERT_EQ(actual[i].qty, oracle[i].qty) << what << " qty at " << i << ", step " << step;

        ASSERT_EQ(actual[i].venue_count, oracle[i].venue_count)
            << what << " contributor count at " << i << " (price " << oracle[i].price << "), step " << step;
        for (size_t v = 0; v < oracle[i].venue_count; ++v) {
            ASSERT_EQ(actual[i].venues[v].slot, oracle[i].venues[v].slot)
                << what << " attribution slot " << v << " at level " << i << ", step " << step;
            ASSERT_EQ(actual[i].venues[v].qty, oracle[i].venues[v].qty)
                << what << " attribution qty " << v << " at level " << i << ", step " << step;
        }
    }
}

// Truncates a side the way PublishBook does. The State is deliberately
// unbounded (consolidated_state.h), so the depth cap is applied by the reader -
// and comparing a capped view against a merge run at the same cap is what makes
// "the client sees the same book" a checked claim rather than a hope.
std::span<const MergedLevel> Capped(const std::vector<MergedLevel>& side, size_t max_depth) {
    return std::span<const MergedLevel>(side).first(std::min(side.size(), max_depth));
}

void ExpectMatchesFullRebuild(const State& state, const FlatBookArray& books, size_t max_depth, int step) {
    Book oracle;
    consolidated::MergeBooks(books, kVenues, oracle, max_depth);

    ASSERT_NO_FATAL_FAILURE(ExpectSameSide(oracle.bids, Capped(state.book().bids, max_depth), "bid", step));
    ASSERT_NO_FATAL_FAILURE(ExpectSameSide(oracle.asks, Capped(state.book().asks, max_depth), "ask", step));
}

BookUpdate MakeUpdate(VenueId venue, uint64_t seq, bool is_snapshot, std::vector<PriceLevel> bids,
                      std::vector<PriceLevel> asks) {
    BookUpdate update{venue, kSpotBtc, bids.size(), is_snapshot, seq};
    update.bids = std::move(bids);
    update.asks = std::move(asks);
    return update;
}

// Venues quote from the SAME narrow price range on purpose. Disjoint ranges
// would give every consolidated level exactly one contributor, and the
// attribution add/update/remove logic - where the incremental merge is hardest
// and a full rebuild is trivial - would never run.
std::vector<PriceLevel> MakeSide(std::mt19937& rng, uint64_t px_low, uint64_t px_high, bool best_is_high) {
    std::uniform_int_distribution<uint64_t> price_dist(px_low, px_high);
    std::uniform_int_distribution<uint64_t> qty(0, 6);  // 0 removes - roughly one level in seven
    std::uniform_int_distribution<int> level_count(1, 8);

    const size_t wanted = static_cast<size_t>(level_count(rng));
    std::vector<uint64_t> prices;
    while (prices.size() < wanted) {
        const uint64_t price = price_dist(rng);
        if (std::find(prices.begin(), prices.end(), price) == prices.end()) {
            prices.push_back(price);
        }
    }
    // Best first, which is what every venue sends.
    std::sort(prices.begin(), prices.end(), [&](uint64_t a, uint64_t b) { return best_is_high ? a > b : a < b; });

    std::vector<PriceLevel> levels;
    levels.reserve(prices.size());
    for (uint64_t price : prices) {
        levels.push_back({price, qty(rng)});
    }
    return levels;
}

// Drives one update into one venue's book and folds it into the state the way
// Core will: rebuild when the change set cannot describe the transition,
// otherwise repair.
void FeedUpdate(FlatBookArray& books, State& state, size_t slot_index, const BookUpdate& update) {
    FlatOrderBook& book = *books[slot_index];
    book.ApplyUpdate(update);

    if (state.needs_rebuild() || !book.last_changes_complete()) {
        state.FullRebuild(books, kVenues);
    } else {
        state.ApplyDepthDelta(static_cast<VenueSlot>(slot_index), book.last_bid_changes(), book.last_ask_changes());
    }
}

FlatBookArray MakeBooks() {
    FlatBookArray books;
    for (size_t i = 0; i < kVenues; ++i) {
        books[i] = std::make_unique<FlatOrderBook>(kVenueIds[i], kSpotBtc);
    }
    return books;
}

}  // namespace

// The main safety net: thousands of interleaved per-venue updates, with the
// incremental book checked against a full k-way merge after EVERY one.
//
// checking only at the end would be far weaker. Incremental state drifts -
// a wrong intermediate value can be overwritten by a later update at the same
// price and disappear before the final comparison. Asserting per step is what
// pins a failure to the update that caused it.
TEST(ConsolidatedStateTest, IncrementalUpdatesMatchFullRebuild) {
    std::mt19937 rng(1337);
    std::uniform_int_distribution<size_t> venue_pick(0, kVenues - 1);

    FlatBookArray books = MakeBooks();
    State state;

    for (int step = 0; step < 3000; ++step) {
        const size_t slot_index = venue_pick(rng);
        const BookUpdate update =
            MakeUpdate(kVenueIds[slot_index], static_cast<uint64_t>(step + 1), false,
                       MakeSide(rng, 900, 949, /*best_is_high=*/true), MakeSide(rng, 950, 999, /*best_is_high=*/false));

        FeedUpdate(books, state, slot_index, update);
        ASSERT_NO_FATAL_FAILURE(ExpectMatchesFullRebuild(state, books, consolidated::kDefaultMaxDepth, step));
    }
}

// The published book is capped; the state is not. This asserts the two agree at
// the cap, which is the property that makes the uncapped state worth its memory.
//
// a cap on the STATE would fail this test only after an erase - the level
// that should move up into the last published slot lives beyond the cap, and an
// incremental repair cannot see it, so the book would lose a level and keep
// losing them. Running the same stream at a shallow depth is what exercises
// that boundary on nearly every update instead of almost never.
TEST(ConsolidatedStateTest, TruncatedPublicationMatchesTruncatedMerge) {
    std::mt19937 rng(99);
    std::uniform_int_distribution<size_t> venue_pick(0, kVenues - 1);

    FlatBookArray books = MakeBooks();
    State state;

    for (int step = 0; step < 2000; ++step) {
        const size_t slot_index = venue_pick(rng);
        const BookUpdate update =
            MakeUpdate(kVenueIds[slot_index], static_cast<uint64_t>(step + 1), false,
                       MakeSide(rng, 900, 949, /*best_is_high=*/true), MakeSide(rng, 950, 999, /*best_is_high=*/false));

        FeedUpdate(books, state, slot_index, update);
        ASSERT_NO_FATAL_FAILURE(ExpectMatchesFullRebuild(state, books, /*max_depth=*/5, step));
    }
}

// Snapshots are interleaved with deltas. A snapshot REPLACES a venue's side, so
// the incremental path must not be taken - FlatOrderBook reports the change set
// as incomplete and the state rebuilds.
//
// this is the test that fails if anyone "optimises" the snapshot path by
// applying its levels as inserts. The book would then keep every price the
// snapshot replaced, which the comparison catches on the very next step.
TEST(ConsolidatedStateTest, SnapshotsForceARebuild) {
    std::mt19937 rng(2024);
    std::uniform_int_distribution<size_t> venue_pick(0, kVenues - 1);
    std::uniform_int_distribution<int> snapshot_roll(0, 19);

    FlatBookArray books = MakeBooks();
    State state;

    for (int step = 0; step < 2000; ++step) {
        const size_t slot_index = venue_pick(rng);
        const bool is_snapshot = snapshot_roll(rng) == 0;

        // A snapshot lands in a SHIFTED price range, so it genuinely replaces
        // the venue's book rather than overwriting the same prices - which is
        // what makes "the old levels are gone" an observable claim.
        const BookUpdate update = is_snapshot ? MakeUpdate(kVenueIds[slot_index], static_cast<uint64_t>(step + 1), true,
                                                           MakeSide(rng, 910, 939, /*best_is_high=*/true),
                                                           MakeSide(rng, 960, 989, /*best_is_high=*/false))
                                              : MakeUpdate(kVenueIds[slot_index], static_cast<uint64_t>(step + 1),
                                                           false, MakeSide(rng, 900, 949, /*best_is_high=*/true),
                                                           MakeSide(rng, 950, 999, /*best_is_high=*/false));

        FeedUpdate(books, state, slot_index, update);
        ASSERT_NO_FATAL_FAILURE(ExpectMatchesFullRebuild(state, books, consolidated::kDefaultMaxDepth, step));
    }
}

// A venue leaving and rejoining the merge. Health is a per-venue verdict that
// can add or remove thousands of contributions at once, so it latches a rebuild
// rather than being unwound level by level.
TEST(ConsolidatedStateTest, HealthTransitionsRebuild) {
    std::mt19937 rng(7);
    std::uniform_int_distribution<size_t> venue_pick(0, kVenues - 1);

    FlatBookArray books = MakeBooks();
    State state;
    VenueHealthArray health{};
    for (size_t i = 0; i < kVenues; ++i) {
        health[i] = VenueHealth::kLive;
    }

    for (int step = 0; step < 600; ++step) {
        // Venue 1 goes stale a third of the way in and comes back two thirds
        // through, so the run covers admitted -> excluded -> re-admitted.
        if (step == 200 || step == 400) {
            health[1] = (step == 200) ? VenueHealth::kStale : VenueHealth::kLive;
            state.MarkForRebuild();
        }

        const size_t slot_index = venue_pick(rng);
        const BookUpdate update =
            MakeUpdate(kVenueIds[slot_index], static_cast<uint64_t>(step + 1), false,
                       MakeSide(rng, 900, 949, /*best_is_high=*/true), MakeSide(rng, 950, 999, /*best_is_high=*/false));

        FlatOrderBook& book = *books[slot_index];
        book.ApplyUpdate(update);

        // The admission check lives in the CALLER, exactly as it will in Core:
        // an excluded venue's changes are simply not folded in. State merges
        // what it is handed, like MergeBooks.
        const bool admitted = IsAdmissible(health[slot_index]);
        if (state.needs_rebuild() || !book.last_changes_complete()) {
            state.FullRebuild(books, kVenues, &health);
        } else if (admitted) {
            state.ApplyDepthDelta(static_cast<VenueSlot>(slot_index), book.last_bid_changes(), book.last_ask_changes());
        }

        Book oracle;
        consolidated::MergeBooks(books, kVenues, oracle, consolidated::kDefaultMaxDepth, &health);
        ASSERT_NO_FATAL_FAILURE(
            ExpectSameSide(oracle.bids, Capped(state.book().bids, consolidated::kDefaultMaxDepth), "bid", step));
        ASSERT_NO_FATAL_FAILURE(
            ExpectSameSide(oracle.asks, Capped(state.book().asks, consolidated::kDefaultMaxDepth), "ask", step));
    }
}

// The four attribution transitions, spelled out so a failure names which one
// broke instead of pointing at update 1483 of a random stream.
TEST(ConsolidatedStateTest, AttributionTransitions) {
    FlatBookArray books = MakeBooks();
    State state;

    // Venue 0 alone at two prices.
    FeedUpdate(books, state, 0, MakeUpdate(VenueId::BINANCE, 1, false, {{100, 5}, {99, 10}}, {}));

    // Venue 1 JOINS an existing price and creates a new one.
    FeedUpdate(books, state, 1, MakeUpdate(VenueId::BYBIT, 1, false, {{100, 8}, {98, 20}}, {}));
    {
        const Book& snapshot = state.book();
        ASSERT_EQ(snapshot.bids.size(), 3u);
        EXPECT_EQ(snapshot.bids[0].price, 100u);
        EXPECT_EQ(snapshot.bids[0].venue_count, 2);
        EXPECT_EQ(snapshot.bids[0].qty, 13u) << "5 from venue 0 + 8 from venue 1";
        EXPECT_EQ(snapshot.bids[1].price, 99u);
        EXPECT_EQ(snapshot.bids[2].price, 98u);
    }

    // Venue 0 UPDATES its contribution at a shared price: 5 -> 7.
    FeedUpdate(books, state, 0, MakeUpdate(VenueId::BINANCE, 2, false, {{100, 7}}, {}));
    {
        const Book& snapshot = state.book();
        EXPECT_EQ(snapshot.bids[0].qty, 15u) << "7 + 8";
        EXPECT_EQ(snapshot.bids[0].venue_count, 2);
    }

    // Venue 0 WITHDRAWS from the shared price. The level survives on venue 1.
    FeedUpdate(books, state, 0, MakeUpdate(VenueId::BINANCE, 3, false, {{100, 0}}, {}));
    {
        const Book& snapshot = state.book();
        ASSERT_EQ(snapshot.bids.size(), 3u) << "level must survive while another venue quotes it";
        EXPECT_EQ(snapshot.bids[0].price, 100u);
        ASSERT_EQ(snapshot.bids[0].venue_count, 1);
        EXPECT_EQ(snapshot.bids[0].venues[0].slot, static_cast<VenueSlot>(1));
        EXPECT_EQ(snapshot.bids[0].qty, 8u);
    }

    // The LAST contributor withdraws. Now the consolidated price disappears -
    // a level left behind at qty 0 would still be published and still be swept
    // by the band walk as a real price with nothing behind it.
    FeedUpdate(books, state, 1, MakeUpdate(VenueId::BYBIT, 2, false, {{100, 0}}, {}));
    {
        const Book& snapshot = state.book();
        ASSERT_EQ(snapshot.bids.size(), 2u) << "level must be erased when its last contributor leaves";
        EXPECT_EQ(snapshot.bids[0].price, 99u);
        EXPECT_EQ(snapshot.bids[1].price, 98u);
    }

    ASSERT_NO_FATAL_FAILURE(ExpectMatchesFullRebuild(state, books, consolidated::kDefaultMaxDepth, 0));
}

// Attribution must come out in slot order regardless of the order the venues
// arrive in, because that is the order MergeBooks produces and the two are
// compared element by element.
TEST(ConsolidatedStateTest, AttributionStaysInSlotOrder) {
    FlatBookArray books = MakeBooks();
    State state;

    // Deliberately backwards: the highest slot quotes the price first.
    FeedUpdate(books, state, 2, MakeUpdate(VenueId::OKX, 1, false, {{100, 3}}, {}));
    FeedUpdate(books, state, 0, MakeUpdate(VenueId::BINANCE, 1, false, {{100, 1}}, {}));
    FeedUpdate(books, state, 1, MakeUpdate(VenueId::BYBIT, 1, false, {{100, 2}}, {}));

    const Book& snapshot = state.book();
    ASSERT_EQ(snapshot.bids.size(), 1u);
    ASSERT_EQ(snapshot.bids[0].venue_count, 3);
    EXPECT_EQ(snapshot.bids[0].venues[0].slot, static_cast<VenueSlot>(0));
    EXPECT_EQ(snapshot.bids[0].venues[1].slot, static_cast<VenueSlot>(1));
    EXPECT_EQ(snapshot.bids[0].venues[2].slot, static_cast<VenueSlot>(2));
    EXPECT_EQ(snapshot.bids[0].qty, 6u);

    ASSERT_NO_FATAL_FAILURE(ExpectMatchesFullRebuild(state, books, consolidated::kDefaultMaxDepth, 0));
}
