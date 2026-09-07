#include <gtest/gtest.h>

#include <algorithm>
#include <random>

#include "md_core/consolidated_bbo.h"

using namespace market_data;

namespace {

BookUpdate MakeSnapshot(std::vector<PriceLevel> bids, std::vector<PriceLevel> asks) {
    BookUpdate update{};
    update.seq = 1;
    update.is_snapshot = true;
    update.bids = std::move(bids);
    update.asks = std::move(asks);
    return update;
}

void SetBook(FlatBookArray& books, VenueId venue, std::vector<PriceLevel> bids, std::vector<PriceLevel> asks) {
    books[static_cast<size_t>(venue)] =
        std::make_unique<FlatOrderBook>(venue, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
    books[static_cast<size_t>(venue)]->ApplyUpdate(MakeSnapshot(std::move(bids), std::move(asks)));
}

BboQuote MakeQuote(VenueId venue, PriceTicks bid_px, QtyUnits bid_qty, PriceTicks ask_px, QtyUnits ask_qty) {
    BboQuote quote;
    quote.venue = venue;
    quote.instrument = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);
    quote.bid_price = bid_px;
    quote.bid_qty = bid_qty;
    quote.ask_price = ask_px;
    quote.ask_qty = ask_qty;
    return quote;
}

// These tests build the quote array directly instead of going through
// Core::RegisterVenue, so they pick their own slot layout - and they pick
// slot == VenueId, which keeps every expectation below valid. The helper names
// that choice once rather than repeating the cast at each call site, and marks
// the places that would need revisiting if a test ever wants a different
// layout.
constexpr VenueSlot SlotOf(VenueId venue) {
    return static_cast<VenueSlot>(static_cast<size_t>(venue));
}

// Applies a quote the way Core does: store into the array FIRST (the rescan
// path re-reads it), then fold it in.
void Apply(VenueQuoteArray& quotes, consolidated::BBO& bbo, const BboQuote& quote) {
    const VenueSlot slot = SlotOf(quote.venue);
    quotes[VenueSlotIndex(slot)] = quote;
    consolidated::UpdateBBOWithQuote(bbo, quote, slot, quotes);
}

// The incremental path appends venues in arrival order; the full scan
// appends in venue-index order. The sets must match, the order need not -
// so compare sorted copies. Comparing only .size() would miss a
// wrong-venue or swapped-qty bug.
void ExpectSameVenues(const std::vector<consolidated::VenueQuote>& a, const std::vector<consolidated::VenueQuote>& b) {
    auto sorted_a = a;
    auto sorted_b = b;
    auto by_venue = [](const consolidated::VenueQuote& l, const consolidated::VenueQuote& r) {
        return l.slot < r.slot;
    };
    std::sort(sorted_a.begin(), sorted_a.end(), by_venue);
    std::sort(sorted_b.begin(), sorted_b.end(), by_venue);

    ASSERT_EQ(sorted_a.size(), sorted_b.size());
    for (size_t i = 0; i < sorted_a.size(); ++i) {
        EXPECT_EQ(sorted_a[i].slot, sorted_b[i].slot);
        EXPECT_EQ(sorted_a[i].qty, sorted_b[i].qty);
    }
}

}  // namespace

TEST(ConsolidatedBboTest, EmptyBooksHaveNoConsolidatedBBO) {
    FlatBookArray books{};  // all null - no venue configured

    auto bbo = consolidated::ComputeBBO(books);

    EXPECT_EQ(bbo.best_bid.total_qty, 0u);
    EXPECT_TRUE(bbo.best_bid.venues.empty());
    EXPECT_EQ(bbo.best_ask.total_qty, 0u);
    EXPECT_TRUE(bbo.best_ask.venues.empty());
    EXPECT_FALSE(bbo.crossed);
}

TEST(ConsolidatedBboTest, SingleVenueContributesBBO) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}}, {{101, 7}});

    auto bbo = consolidated::ComputeBBO(books);

    EXPECT_EQ(bbo.best_bid.price, 100u);
    EXPECT_EQ(bbo.best_bid.total_qty, 5u);
    ASSERT_EQ(bbo.best_bid.venues.size(), 1u);
    EXPECT_EQ(bbo.best_bid.venues[0].slot, SlotOf(VenueId::BINANCE));
    EXPECT_EQ(bbo.best_bid.venues[0].qty, 5u);

    EXPECT_EQ(bbo.best_ask.price, 101u);
}

TEST(ConsolidatedBboTest, HighestBidWinsAcrossVenues) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}}, {});
    SetBook(books, VenueId::OKX, {{102, 3}}, {});
    SetBook(books, VenueId::BYBIT, {{99, 1}}, {});

    auto bbo = consolidated::ComputeBBO(books);

    EXPECT_EQ(bbo.best_bid.price, 102u);
    EXPECT_EQ(bbo.best_bid.total_qty, 3u);
    ASSERT_EQ(bbo.best_bid.venues.size(), 1u);
    EXPECT_EQ(bbo.best_bid.venues[0].slot, SlotOf(VenueId::OKX));
}

TEST(ConsolidatedBboTest, TiedBidsAtBestPriceAreSummedWithAttribution) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}}, {});
    SetBook(books, VenueId::OKX, {{100, 3}}, {});
    SetBook(books, VenueId::BYBIT, {{95, 1}}, {});

    auto bbo = consolidated::ComputeBBO(books);

    EXPECT_EQ(bbo.best_bid.price, 100u);
    EXPECT_EQ(bbo.best_bid.total_qty, 8u);  // 5 + 3
    ASSERT_EQ(bbo.best_bid.venues.size(), 2u);
}

TEST(ConsolidatedBboTest, LowestAskWinsAcrossVenues) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {}, {{105, 2}});
    SetBook(books, VenueId::OKX, {}, {{103, 4}});

    auto bbo = consolidated::ComputeBBO(books);

    EXPECT_EQ(bbo.best_ask.price, 103u);
    EXPECT_EQ(bbo.best_ask.total_qty, 4u);
}

TEST(ConsolidatedBboTest, NonCrossedBookIsNotFlagged) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}}, {{101, 7}});

    auto bbo = consolidated::ComputeBBO(books);

    EXPECT_FALSE(bbo.crossed);
}

TEST(ConsolidatedBboTest, CrossedBookSetsFlag) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{105, 5}}, {});  // bid above the other venue's ask
    SetBook(books, VenueId::OKX, {}, {{100, 7}});

    auto bbo = consolidated::ComputeBBO(books);

    EXPECT_TRUE(bbo.crossed);
}

// The case that can't be solved from the consolidated BBO alone: the only
// venue at the best level steps down, so the new best must come from the
// per-venue array (the rescan path).
TEST(ConsolidatedBboTest, IncrementalSoleBestVenueSteppingDownRescans) {
    VenueQuoteArray quotes{};
    consolidated::BBO bbo;

    Apply(quotes, bbo, MakeQuote(VenueId::BINANCE, 100, 5, 110, 5));
    Apply(quotes, bbo, MakeQuote(VenueId::OKX, 99, 3, 111, 3));
    Apply(quotes, bbo, MakeQuote(VenueId::BYBIT, 98, 1, 112, 1));
    ASSERT_EQ(bbo.best_bid.price, 100u);  // Binance alone at the top

    // Binance drops below both others - answer must be OKX at 99, not 95.
    Apply(quotes, bbo, MakeQuote(VenueId::BINANCE, 95, 5, 110, 5));

    EXPECT_EQ(bbo.best_bid.price, 99u);
    EXPECT_EQ(bbo.best_bid.total_qty, 3u);
    ASSERT_EQ(bbo.best_bid.venues.size(), 1u);
    EXPECT_EQ(bbo.best_bid.venues[0].slot, SlotOf(VenueId::OKX));
}

// A tied level losing one venue must keep the other - no rescan, no phantom
// level, and total_qty must drop by exactly the leaver's old qty.
TEST(ConsolidatedBboTest, IncrementalTiedLevelLosingOneVenueKeepsTheOther) {
    VenueQuoteArray quotes{};
    consolidated::BBO bbo;

    Apply(quotes, bbo, MakeQuote(VenueId::BINANCE, 100, 5, 110, 5));
    Apply(quotes, bbo, MakeQuote(VenueId::OKX, 100, 3, 111, 3));
    ASSERT_EQ(bbo.best_bid.total_qty, 8u);  // 5 + 3, tied at 100

    Apply(quotes, bbo, MakeQuote(VenueId::BINANCE, 90, 5, 110, 5));

    EXPECT_EQ(bbo.best_bid.price, 100u);
    EXPECT_EQ(bbo.best_bid.total_qty, 3u);  // 8 - Binance's old 5
    ASSERT_EQ(bbo.best_bid.venues.size(), 1u);
    EXPECT_EQ(bbo.best_bid.venues[0].slot, SlotOf(VenueId::OKX));
}

// UpdateBBOWithQuote maintains persistent state, so a bug accumulates
// silently instead of failing loudly. Drive a random stream through it and
// assert after EVERY update that it exactly equals a full rescan
// (ComputeBBOFromQuotes) - a stateless recomputation is trivially correct, so
// it is the reference the stateful path is held to.
TEST(ConsolidatedBboTest, IncrementalMatchesFullScanOverRandomStream) {
    std::mt19937 rng(12345);  // fixed seed - reproducible failures

    // Deliberately narrow price range: with wide prices two venues would
    // essentially never collide, so the tie and rescan branches - the ones
    // most likely to be wrong - would never execute. Bid and ask draw from
    // the SAME range, so crossed books occur constantly too.
    std::uniform_int_distribution<uint64_t> price_dist(100, 104);
    std::uniform_int_distribution<uint64_t> qty_dist(1, 9);
    std::uniform_int_distribution<size_t> venue_dist(0, kVenueCount - 1);

    VenueQuoteArray quotes{};
    consolidated::BBO incremental;

    for (int i = 0; i < 20000; ++i) {
        auto quote = MakeQuote(static_cast<VenueId>(venue_dist(rng)), price_dist(rng), qty_dist(rng), price_dist(rng),
                               qty_dist(rng));
        Apply(quotes, incremental, quote);

        auto expected = consolidated::ComputeBBOFromQuotes(quotes);

        ASSERT_EQ(incremental.best_bid.price, expected.best_bid.price) << "bid price diverged at update " << i;
        ASSERT_EQ(incremental.best_bid.total_qty, expected.best_bid.total_qty) << "bid qty diverged at update " << i;
        ASSERT_EQ(incremental.best_ask.price, expected.best_ask.price) << "ask price diverged at update " << i;
        ASSERT_EQ(incremental.best_ask.total_qty, expected.best_ask.total_qty) << "ask qty diverged at update " << i;
        ASSERT_EQ(incremental.crossed, expected.crossed) << "crossed diverged at update " << i;
        ExpectSameVenues(incremental.best_bid.venues, expected.best_bid.venues);
        ExpectSameVenues(incremental.best_ask.venues, expected.best_ask.venues);
    }
}

TEST(ConsolidatedBboTest, UnconfiguredVenueSlotIsSkippedNotCrashed) {
    FlatBookArray books{};  // every slot null except one
    SetBook(books, VenueId::BYBIT, {{100, 5}}, {{101, 7}});

    auto bbo = consolidated::ComputeBBO(books);

    ASSERT_EQ(bbo.best_bid.venues.size(), 1u);
    EXPECT_EQ(bbo.best_bid.venues[0].slot, SlotOf(VenueId::BYBIT));
}

// ------------------------------------------------- staleness admission ------

namespace {

using market_data::consolidated::BBO;
using market_data::consolidated::ComputeBBOFromQuotes;
using market_data::consolidated::ComputeBBOFromQuotesInto;
using market_data::consolidated::UpdateBBOWithQuote;

VenueHealthArray BboHealth(VenueHealth binance, VenueHealth bybit, VenueHealth okx) {
    VenueHealthArray health{};
    health[static_cast<size_t>(VenueId::BINANCE)] = binance;
    health[static_cast<size_t>(VenueId::BYBIT)] = bybit;
    health[static_cast<size_t>(VenueId::OKX)] = okx;
    return health;
}

constexpr auto kLive = VenueHealth::kLive;
constexpr auto kStale = VenueHealth::kStale;

}  // namespace

TEST(ConsolidatedBboTest, FullScanExcludesAStaleVenue) {
    VenueQuoteArray quotes{};
    quotes[static_cast<size_t>(VenueId::BINANCE)] = MakeQuote(VenueId::BINANCE, 50000, 2, 50010, 2);
    quotes[static_cast<size_t>(VenueId::BYBIT)] = MakeQuote(VenueId::BYBIT, 49900, 3, 49910, 3);

    const auto health = BboHealth(kStale, kLive, kLive);
    BBO bbo = ComputeBBOFromQuotes(quotes, &health);

    EXPECT_EQ(bbo.best_bid.price, 49900u);
    ASSERT_EQ(bbo.best_bid.venues.size(), 1u);
    EXPECT_EQ(bbo.best_bid.venues[0].slot, SlotOf(VenueId::BYBIT));
    EXPECT_EQ(bbo.best_ask.price, 49910u);
}

// A stale venue's quote is folded in as though it carried no prices. Not a
// shortcut - "has no bid" and "its bid may not be used" have the same effect
// on the top of book, so the existing price == 0 branch does the work.
TEST(ConsolidatedBboTest, AStaleVenuesOwnQuoteIsTreatedAsNoPrice) {
    VenueQuoteArray quotes{};
    BBO bbo;
    const auto health = BboHealth(kStale, kLive, kLive);

    quotes[static_cast<size_t>(VenueId::BYBIT)] = MakeQuote(VenueId::BYBIT, 49900, 3, 49910, 3);
    UpdateBBOWithQuote(bbo, quotes[static_cast<size_t>(VenueId::BYBIT)], SlotOf(VenueId::BYBIT), quotes, &health);
    ASSERT_EQ(bbo.best_bid.price, 49900u);

    // BINANCE is stale and quotes a BETTER bid. It must not take the level.
    quotes[static_cast<size_t>(VenueId::BINANCE)] = MakeQuote(VenueId::BINANCE, 50000, 2, 50010, 2);
    UpdateBBOWithQuote(bbo, quotes[static_cast<size_t>(VenueId::BINANCE)], SlotOf(VenueId::BINANCE), quotes, &health);

    EXPECT_EQ(bbo.best_bid.price, 49900u) << "a stale venue must not win the best bid";
    EXPECT_EQ(bbo.best_bid.venues[0].slot, SlotOf(VenueId::BYBIT));
}

// THE REASON THE RESCAN EXISTS. This test asserts the BROKEN behaviour on
// purpose, to document why filtering alone is not enough.
//
// BINANCE's price is folded in while it is live. It then goes stale and stops
// quoting. Filtering its FUTURE quotes achieves nothing - it sends none - and
// the incremental path only ever holds the top level, so nothing displaces
// the price already sitting there.
TEST(ConsolidatedBboTest, IncrementalCannotRemoveAPriceAlreadyFoldedIn) {
    VenueQuoteArray quotes{};
    BBO bbo;

    // Both live. BINANCE owns the best bid.
    auto health = BboHealth(kLive, kLive, kLive);
    quotes[static_cast<size_t>(VenueId::BINANCE)] = MakeQuote(VenueId::BINANCE, 50000, 2, 50010, 2);
    UpdateBBOWithQuote(bbo, quotes[static_cast<size_t>(VenueId::BINANCE)], SlotOf(VenueId::BINANCE), quotes, &health);
    quotes[static_cast<size_t>(VenueId::BYBIT)] = MakeQuote(VenueId::BYBIT, 49900, 3, 49910, 3);
    UpdateBBOWithQuote(bbo, quotes[static_cast<size_t>(VenueId::BYBIT)], SlotOf(VenueId::BYBIT), quotes, &health);
    ASSERT_EQ(bbo.best_bid.price, 50000u);
    ASSERT_EQ(bbo.best_bid.venues[0].slot, SlotOf(VenueId::BINANCE));

    // BINANCE goes stale. Only BYBIT keeps quoting.
    health = BboHealth(kStale, kLive, kLive);
    quotes[static_cast<size_t>(VenueId::BYBIT)] = MakeQuote(VenueId::BYBIT, 49895, 3, 49905, 3);
    UpdateBBOWithQuote(bbo, quotes[static_cast<size_t>(VenueId::BYBIT)], SlotOf(VenueId::BYBIT), quotes, &health);

    // Asserted deliberately: the incremental path leaves the stale price in
    // place. If this ever starts failing, the incremental path learned to
    // handle transitions and Core's version counter may be removable.
    EXPECT_EQ(bbo.best_bid.price, 50000u) << "incremental filtering alone cannot evict an already-folded price";
    EXPECT_EQ(bbo.best_bid.venues[0].slot, SlotOf(VenueId::BINANCE));

    // The rescan is what fixes it - this is what Core does on a health change.
    ComputeBBOFromQuotesInto(quotes, bbo, &health);
    EXPECT_EQ(bbo.best_bid.price, 49895u);
    ASSERT_EQ(bbo.best_bid.venues.size(), 1u);
    EXPECT_EQ(bbo.best_bid.venues[0].slot, SlotOf(VenueId::BYBIT));
}

// The in-place rescan must reuse the caller's buffers rather than replacing
// them - Core::Init reserves those vectors so the hot path never allocates
// and assigning a by-value BBO over them would discard it.
TEST(ConsolidatedBboTest, InPlaceRescanKeepsReservedCapacity) {
    VenueQuoteArray quotes{};
    quotes[static_cast<size_t>(VenueId::BYBIT)] = MakeQuote(VenueId::BYBIT, 49900, 3, 49910, 3);

    BBO bbo;
    bbo.best_bid.venues.reserve(kVenueCount);
    bbo.best_ask.venues.reserve(kVenueCount);
    const size_t bid_capacity = bbo.best_bid.venues.capacity();

    ComputeBBOFromQuotesInto(quotes, bbo, nullptr);

    EXPECT_GE(bbo.best_bid.venues.capacity(), bid_capacity) << "rescan must not shrink the reserved buffer";
    EXPECT_EQ(bbo.best_bid.price, 49900u);
}

// The by-value and in-place forms must agree - one delegates to the other, and
// this pins that so they cannot drift into two implementations.
TEST(ConsolidatedBboTest, InPlaceAndByValueAgree) {
    VenueQuoteArray quotes{};
    quotes[static_cast<size_t>(VenueId::BINANCE)] = MakeQuote(VenueId::BINANCE, 50000, 2, 50010, 2);
    quotes[static_cast<size_t>(VenueId::BYBIT)] = MakeQuote(VenueId::BYBIT, 50000, 5, 49990, 1);
    const auto health = BboHealth(kLive, kLive, kStale);

    BBO by_value = ComputeBBOFromQuotes(quotes, &health);
    BBO in_place;
    ComputeBBOFromQuotesInto(quotes, in_place, &health);

    EXPECT_EQ(by_value.best_bid.price, in_place.best_bid.price);
    EXPECT_EQ(by_value.best_bid.total_qty, in_place.best_bid.total_qty);
    EXPECT_EQ(by_value.best_bid.venues.size(), in_place.best_bid.venues.size());
    EXPECT_EQ(by_value.best_ask.price, in_place.best_ask.price);
    EXPECT_EQ(by_value.crossed, in_place.crossed);
}

// Every venue stale: publish nothing rather than three frozen prices.
TEST(ConsolidatedBboTest, AllVenuesStaleProducesAnEmptyBbo) {
    VenueQuoteArray quotes{};
    quotes[static_cast<size_t>(VenueId::BINANCE)] = MakeQuote(VenueId::BINANCE, 50000, 2, 50010, 2);
    quotes[static_cast<size_t>(VenueId::BYBIT)] = MakeQuote(VenueId::BYBIT, 49900, 3, 49910, 3);

    const auto health = BboHealth(kStale, kStale, kStale);
    BBO bbo = ComputeBBOFromQuotes(quotes, &health);

    EXPECT_EQ(bbo.best_bid.price, 0u);
    EXPECT_EQ(bbo.best_ask.price, 0u);
    EXPECT_TRUE(bbo.best_bid.venues.empty());
    EXPECT_FALSE(bbo.crossed) << "an empty book is not a crossed book";
}
