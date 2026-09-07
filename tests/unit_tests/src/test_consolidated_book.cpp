#include <gtest/gtest.h>

#include "md_core/consolidated_book.h"

using namespace market_data;
using namespace market_data::consolidated;

namespace {

// These fixtures fill the book array by index, so slot == VenueId here.
// Named once rather than cast at each assertion.
constexpr VenueSlot SlotOf(VenueId venue) {
    return static_cast<VenueSlot>(static_cast<size_t>(venue));
}

void SetBook(FlatBookArray& books, VenueId venue, std::vector<PriceLevel> bids, std::vector<PriceLevel> asks) {
    BookUpdate update{venue, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot), bids.size(), true, 1};
    update.bids = std::move(bids);
    update.asks = std::move(asks);

    books[static_cast<size_t>(venue)] =
        std::make_unique<FlatOrderBook>(venue, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
    books[static_cast<size_t>(venue)]->ApplyUpdate(update);
}

}  // namespace

// ------------------------------------------------------------- MergeBooks ---

TEST(ConsolidatedBookTest, EmptyVenuesProduceEmptyBook) {
    FlatBookArray books{};
    Book merged;

    MergeBooks(books, kVenueCount, merged);

    EXPECT_TRUE(merged.bids.empty());
    EXPECT_TRUE(merged.asks.empty());
}

TEST(ConsolidatedBookTest, SingleVenueIsCopiedWithPerLevelQuantities) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}, {99, 3}}, {{101, 2}});
    Book merged;

    MergeBooks(books, kVenueCount, merged);

    ASSERT_EQ(merged.bids.size(), 2u);
    EXPECT_EQ(merged.bids[0].price, 100u);
    // Per level, NOT cumulative. The merge used to carry prefix sums here;
    // the band walks accumulate them locally now (consolidated_book.h), so
    // these are each level's own quantity.
    EXPECT_EQ(merged.bids[0].qty, 5u);
    EXPECT_EQ(merged.bids[1].price, 99u);
    EXPECT_EQ(merged.bids[1].qty, 3u) << "this level alone, not 5 + 3";

    ASSERT_EQ(merged.asks.size(), 1u);
    EXPECT_EQ(merged.asks[0].price, 101u);
    EXPECT_EQ(merged.asks[0].qty, 2u);
}

// Bids must come out descending and asks ascending, regardless of which
// venue contributed which level.
TEST(ConsolidatedBookTest, DisjointPricesInterleaveInSortedOrder) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 1}, {98, 1}}, {{105, 1}, {107, 1}});
    SetBook(books, VenueId::OKX, {{99, 1}, {97, 1}}, {{106, 1}, {108, 1}});
    Book merged;

    MergeBooks(books, kVenueCount, merged);

    ASSERT_EQ(merged.bids.size(), 4u);
    EXPECT_EQ(merged.bids[0].price, 100u);
    EXPECT_EQ(merged.bids[1].price, 99u);
    EXPECT_EQ(merged.bids[2].price, 98u);
    EXPECT_EQ(merged.bids[3].price, 97u);

    ASSERT_EQ(merged.asks.size(), 4u);
    EXPECT_EQ(merged.asks[0].price, 105u);
    EXPECT_EQ(merged.asks[1].price, 106u);
    EXPECT_EQ(merged.asks[2].price, 107u);
    EXPECT_EQ(merged.asks[3].price, 108u);
}

// The case the whole attribution design exists for: two venues at the
// same price collapse into ONE level carrying both.
TEST(ConsolidatedBookTest, SamePriceAcrossVenuesMergesWithAttribution) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}}, {});
    SetBook(books, VenueId::OKX, {{100, 3}}, {});
    SetBook(books, VenueId::BYBIT, {{100, 2}}, {});
    Book merged;

    MergeBooks(books, kVenueCount, merged);

    ASSERT_EQ(merged.bids.size(), 1u) << "one price, one level";
    const MergedLevel& level = merged.bids[0];
    EXPECT_EQ(level.price, 100u);
    EXPECT_EQ(level.qty, 10u);  // 5 + 3 + 2, across venues at ONE price
    ASSERT_EQ(level.venue_count, 3);

    // Attribution is written in venue-index order by the merge.
    EXPECT_EQ(level.venues[0].slot, SlotOf(VenueId::BINANCE));
    EXPECT_EQ(level.venues[0].qty, 5u);
    EXPECT_EQ(level.venues[1].slot, SlotOf(VenueId::BYBIT));
    EXPECT_EQ(level.venues[1].qty, 2u);
    EXPECT_EQ(level.venues[2].slot, SlotOf(VenueId::OKX));
    EXPECT_EQ(level.venues[2].qty, 3u);
}

TEST(ConsolidatedBookTest, PartiallyOverlappingPricesMergeCorrectly) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}, {98, 2}}, {});
    SetBook(books, VenueId::OKX, {{100, 3}, {99, 4}}, {});
    Book merged;

    MergeBooks(books, kVenueCount, merged);

    ASSERT_EQ(merged.bids.size(), 3u);
    EXPECT_EQ(merged.bids[0].price, 100u);
    EXPECT_EQ(merged.bids[0].qty, 8u);  // both venues: 5 + 3
    EXPECT_EQ(merged.bids[0].venue_count, 2);
    EXPECT_EQ(merged.bids[1].price, 99u);
    EXPECT_EQ(merged.bids[1].qty, 4u);  // OKX only
    EXPECT_EQ(merged.bids[1].venue_count, 1);
    EXPECT_EQ(merged.bids[2].price, 98u);
    EXPECT_EQ(merged.bids[2].qty, 2u);  // Binance only
}

TEST(ConsolidatedBookTest, MaxDepthTruncatesBothSides) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 1}, {99, 1}, {98, 1}, {97, 1}}, {{101, 1}, {102, 1}, {103, 1}, {104, 1}});
    Book merged;

    MergeBooks(books, kVenueCount, merged, /*max_depth=*/2);

    EXPECT_EQ(merged.bids.size(), 2u);
    EXPECT_EQ(merged.asks.size(), 2u);
    EXPECT_EQ(merged.bids[1].price, 99u) << "keeps the TOP levels, not arbitrary ones";
    EXPECT_EQ(merged.asks[1].price, 102u);
}

// Every level carries its own total across contributors. This used to require
// LevelQty() to subtract two prefix sums; MergedLevel::qty stores it directly.
TEST(ConsolidatedBookTest, EachLevelCarriesItsOwnQuantity) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}, {99, 3}, {98, 7}}, {});
    Book merged;

    MergeBooks(books, kVenueCount, merged);

    ASSERT_EQ(merged.bids.size(), 3u);
    EXPECT_EQ(merged.bids[0].qty, 5u);
    EXPECT_EQ(merged.bids[1].qty, 3u);
    EXPECT_EQ(merged.bids[2].qty, 7u);
}

// The Book is reused across publishes (Clear keeps capacity) - a second merge
// must not leave levels from the first.
TEST(ConsolidatedBookTest, ReusedBookHasNoStaleLevels) {
    Book merged;

    FlatBookArray first{};
    SetBook(first, VenueId::BINANCE, {{100, 1}, {99, 1}, {98, 1}}, {{101, 1}});
    MergeBooks(first, kVenueCount, merged);
    ASSERT_EQ(merged.bids.size(), 3u);

    FlatBookArray second{};
    SetBook(second, VenueId::OKX, {{200, 9}}, {});
    MergeBooks(second, kVenueCount, merged);

    ASSERT_EQ(merged.bids.size(), 1u) << "levels from the first merge must be gone";
    EXPECT_EQ(merged.bids[0].price, 200u);
    EXPECT_EQ(merged.bids[0].qty, 9u);
    EXPECT_TRUE(merged.asks.empty());
}

// ---------------------------------------------------------- volume bands ---

namespace {

// Band math takes prices/quantities at production scale (x 1e8), and
// FillToNotional scales its target the same way. Raw single-digit fixtures
// make the target unreachable, so every band would report insufficient_depth
// - the merge tests above can stay raw only because they never touch
// notional.
constexpr PriceTicks Px(uint64_t whole) {
    return whole * kScaleFactor;
}
constexpr QtyUnits Qty(uint64_t whole) {
    return whole * kScaleFactor;
}

// Asks: 100@5, 101@10, 102@20. Cumulative notional: 500, 1510, 3550 USDT.
Book MakeBandBook() {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {}, {{Px(100), Qty(5)}, {Px(101), Qty(10)}, {Px(102), Qty(20)}});
    Book merged;
    MergeBooks(books, kVenueCount, merged);
    return merged;
}

}  // namespace

// Target lands exactly on a level boundary: no partial fill.
TEST(ConsolidatedBookTest, FillToNotionalExactLevelBoundary) {
    Book book = MakeBandBook();

    auto fill = FillToNotional(book.asks, 500 * kScaleFactor);

    EXPECT_EQ(fill.filled_qty, Qty(5));
    EXPECT_EQ(fill.filled_notional, 500u * kScaleFactor);
    EXPECT_EQ(fill.worst_price, Px(100));
    EXPECT_EQ(fill.level_count, 1u);
    EXPECT_EQ(fill.vwap, Px(100));  // 500 / 5
    EXPECT_FALSE(fill.insufficient_depth);
}

// Target falls inside level 1: 500 filled at price 100, then 505 more of the
// 1010 available at price 101 -> 5 extra units.
TEST(ConsolidatedBookTest, FillToNotionalSplitsFinalLevelProportionally) {
    Book book = MakeBandBook();

    auto fill = FillToNotional(book.asks, 1005 * kScaleFactor);

    EXPECT_EQ(fill.filled_qty, Qty(10));  // 5 @100 + 5 @101
    EXPECT_EQ(fill.filled_notional, 1005u * kScaleFactor);
    EXPECT_EQ(fill.worst_price, Px(101));
    EXPECT_EQ(fill.level_count, 2u);
    // 1005 / 10 = 100.5 exactly. At production scale the fractional half
    // survives - at raw scale it would truncate to 100 and hide whether the
    // proportional split worked at all.
    EXPECT_EQ(fill.vwap, 1005u * kScaleFactor / 10);
    EXPECT_FALSE(fill.insufficient_depth);
}

// Asking for more than the book holds is a legitimate answer, not an error.
TEST(ConsolidatedBookTest, FillToNotionalExhaustedBookFlagsInsufficientDepth) {
    Book book = MakeBandBook();

    auto fill = FillToNotional(book.asks, 10'000 * kScaleFactor);

    EXPECT_TRUE(fill.insufficient_depth);
    EXPECT_EQ(fill.filled_qty, Qty(35));  // everything: 5 + 10 + 20
    EXPECT_EQ(fill.filled_notional, 3550u * kScaleFactor);
    EXPECT_EQ(fill.worst_price, Px(102));
    EXPECT_EQ(fill.level_count, 3u);
}

TEST(ConsolidatedBookTest, FillToNotionalOnEmptySideFlagsInsufficientDepth) {
    Book book;
    auto fill = FillToNotional(book.bids, 1000 * kScaleFactor);

    EXPECT_TRUE(fill.insufficient_depth);
    EXPECT_EQ(fill.filled_qty, 0u);
}

// The multi-band walk must produce exactly what repeated single calls do -
// that equivalence is the whole justification for the single-pass version.
TEST(ConsolidatedBookTest, MultiBandMatchesRepeatedSingleBandCalls) {
    Book book = MakeBandBook();
    const std::vector<uint64_t> targets{500 * kScaleFactor, 1005 * kScaleFactor, 3550 * kScaleFactor,
                                        10'000 * kScaleFactor};

    std::vector<NotionalFill> multi;
    FillToNotionalBands(book.asks, targets, multi);

    ASSERT_EQ(multi.size(), targets.size());
    for (size_t i = 0; i < targets.size(); ++i) {
        auto single = FillToNotional(book.asks, targets[i]);
        EXPECT_EQ(multi[i].filled_qty, single.filled_qty) << "band " << i;
        EXPECT_EQ(multi[i].filled_notional, single.filled_notional) << "band " << i;
        EXPECT_EQ(multi[i].worst_price, single.worst_price) << "band " << i;
        EXPECT_EQ(multi[i].level_count, single.level_count) << "band " << i;
        EXPECT_EQ(multi[i].vwap, single.vwap) << "band " << i;
        EXPECT_EQ(multi[i].insufficient_depth, single.insufficient_depth) << "band " << i;
    }
}

// ----------------------------------------------------------- price bands ---

// Asks from 100: a 100bps band reaches 101, a 200bps band reaches 102.
TEST(ConsolidatedBookTest, FillToBpsAskSideWalksUp) {
    Book book = MakeBandBook();

    auto fill = FillToBps(book.asks, 100, /*is_bid=*/false);

    EXPECT_EQ(fill.limit_price, Px(101));  // 100 * 10100 / 10000
    EXPECT_EQ(fill.level_count, 2u);       // 100 and 101 are inside, 102 is not
    EXPECT_EQ(fill.cum_qty, Qty(15));      // 5 + 10
    EXPECT_EQ(fill.cum_notional, 1510u * kScaleFactor);
    EXPECT_EQ(fill.vwap, 10066666666u);  // 1510 / 15 = 100.666..., truncated at 1e8 scale
}

TEST(ConsolidatedBookTest, FillToBpsBidSideWalksDown) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{Px(100), Qty(5)}, {Px(99), Qty(10)}, {Px(98), Qty(20)}}, {});
    Book merged;
    MergeBooks(books, kVenueCount, merged);

    auto fill = FillToBps(merged.bids, 100, /*is_bid=*/true);

    EXPECT_EQ(fill.limit_price, Px(99));  // 100 * 9900 / 10000
    EXPECT_EQ(fill.level_count, 2u);      // 100 and 99 inside, 98 is not
    EXPECT_EQ(fill.cum_qty, Qty(15));
}

// A band narrower than the first gap contains only the top level.
TEST(ConsolidatedBookTest, FillToBpsNarrowBandTakesOnlyTopLevel) {
    Book book = MakeBandBook();

    auto fill = FillToBps(book.asks, 10, /*is_bid=*/false);

    EXPECT_EQ(fill.limit_price, 10010000000u);  // 100 * 10010 / 10000 = 100.1
    EXPECT_EQ(fill.level_count, 1u);            // only 100 is inside; 101 is past the limit
    EXPECT_EQ(fill.cum_qty, Qty(5));
}

// A band wide enough to cover the whole book runs out before reaching its
// limit price. Without the flag, that lower bound is indistinguishable from
// "this is all the liquidity within the band" - the exact confusion a
// 1000bps band on live data produces, where the walk stops at the depth
// budget nowhere near 10% from the top.
TEST(ConsolidatedBookTest, FillToBpsFlagsExhaustedBook) {
    Book book = MakeBandBook();  // asks 100, 101, 102

    auto wide = FillToBps(book.asks, 1000, /*is_bid=*/false);  // limit 110 - past every level
    EXPECT_TRUE(wide.insufficient_depth) << "walked the whole book without reaching the limit";
    EXPECT_EQ(wide.level_count, 3u);

    auto narrow = FillToBps(book.asks, 100, /*is_bid=*/false);  // limit 101 - stops inside the book
    EXPECT_FALSE(narrow.insufficient_depth) << "found the boundary, so the total is complete";
    EXPECT_EQ(narrow.level_count, 2u);
}

TEST(ConsolidatedBookTest, FillToBpsOnEmptySideReturnsNothing) {
    Book book;
    auto fill = FillToBps(book.bids, 100, /*is_bid=*/true);

    EXPECT_EQ(fill.level_count, 0u);
    EXPECT_EQ(fill.cum_qty, 0u);
}

TEST(ConsolidatedBookTest, MultiBpsBandMatchesRepeatedSingleCalls) {
    Book book = MakeBandBook();
    const std::vector<uint32_t> bands{10, 100, 200, 1000};

    std::vector<BpsFill> multi;
    FillToBpsBands(book.asks, bands, /*is_bid=*/false, multi);

    ASSERT_EQ(multi.size(), bands.size());
    for (size_t i = 0; i < bands.size(); ++i) {
        auto single = FillToBps(book.asks, bands[i], /*is_bid=*/false);
        EXPECT_EQ(multi[i].limit_price, single.limit_price) << "band " << i;
        EXPECT_EQ(multi[i].cum_qty, single.cum_qty) << "band " << i;
        EXPECT_EQ(multi[i].cum_notional, single.cum_notional) << "band " << i;
        EXPECT_EQ(multi[i].level_count, single.level_count) << "band " << i;
        EXPECT_EQ(multi[i].vwap, single.vwap) << "band " << i;
    }
}

// ------------------------------------------------- staleness admission ------
//
// These cover the MECHANISM only: given a health verdict, does the merge
// honour it. HOW a venue is judged stale is policy, decided in Core, and is
// tested separately - which is the point of keeping MergeBooks a pure
// function of what it is handed.

namespace {

VenueHealthArray Health(VenueHealth binance, VenueHealth bybit, VenueHealth okx) {
    VenueHealthArray health{};
    health[static_cast<size_t>(VenueId::BINANCE)] = binance;
    health[static_cast<size_t>(VenueId::BYBIT)] = bybit;
    health[static_cast<size_t>(VenueId::OKX)] = okx;
    return health;
}

constexpr auto kLive = VenueHealth::kLive;
constexpr auto kStale = VenueHealth::kStale;
constexpr auto kNoData = VenueHealth::kNoData;
constexpr auto kDisconnected = VenueHealth::kDisconnected;

}  // namespace

// Pins the defaulted nullptr. Every other MergeBooks test in this file relies
// on it, so if it ever changed meaning those tests would start asserting
// something different without saying so.
TEST(ConsolidatedBookTest, NullHealthAdmitsEveryVenue) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}}, {{101, 1}});
    SetBook(books, VenueId::BYBIT, {{99, 7}}, {{102, 1}});
    Book merged;

    MergeBooks(books, kVenueCount, merged, kDefaultMaxDepth, nullptr);

    ASSERT_EQ(merged.bids.size(), 2u);
    EXPECT_EQ(merged.bids[0].price, 100u);
    EXPECT_EQ(merged.bids[1].price, 99u);
}

TEST(ConsolidatedBookTest, StaleVenueContributesNoLevels) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}}, {{101, 1}});
    SetBook(books, VenueId::BYBIT, {{99, 7}}, {{102, 3}});
    Book merged;

    const auto health = Health(kStale, kLive, kLive);
    MergeBooks(books, kVenueCount, merged, kDefaultMaxDepth, &health);

    ASSERT_EQ(merged.bids.size(), 1u);
    EXPECT_EQ(merged.bids[0].price, 99u);
    EXPECT_EQ(merged.bids[0].qty, 7u);
    ASSERT_EQ(merged.asks.size(), 1u);
    EXPECT_EQ(merged.asks[0].price, 102u);
}

// The reason the whole staleness policy exists.
//
// BINANCE is frozen at prices from before the market moved. Because the merge
// takes max(bid) and min(ask), a frozen venue does not merely add noise - it
// WINS. Here it wins both sides at once and produces a CROSSED consolidated
// book: a phantom 90-tick arbitrage that no one can trade.
TEST(ConsolidatedBookTest, FrozenVenueNoLongerWinsTheBestBid) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{50000, 2}}, {{50010, 2}});  // frozen, pre-move
    SetBook(books, VenueId::BYBIT, {{49900, 3}}, {{49910, 3}});    // live, market fell
    SetBook(books, VenueId::OKX, {{49899, 4}}, {{49911, 4}});      // live
    Book merged;

    // Admitting the frozen venue: it takes the best bid, and the book crosses.
    MergeBooks(books, kVenueCount, merged, kDefaultMaxDepth, nullptr);
    EXPECT_EQ(merged.bids[0].price, 50000u);
    EXPECT_EQ(merged.asks[0].price, 49910u);
    EXPECT_GT(merged.bids[0].price, merged.asks[0].price) << "expected the frozen venue to cross the book";

    // Excluding it: both sides come from live venues, and the cross is gone.
    const auto health = Health(kStale, kLive, kLive);
    MergeBooks(books, kVenueCount, merged, kDefaultMaxDepth, &health);

    ASSERT_FALSE(merged.bids.empty());
    ASSERT_FALSE(merged.asks.empty());
    EXPECT_EQ(merged.bids[0].price, 49900u);
    EXPECT_EQ(merged.asks[0].price, 49910u);
    EXPECT_LT(merged.bids[0].price, merged.asks[0].price);
}

// kNoData must be excluded on its own merits, not because its book happens to
// be empty. Relying on "a venue with no data has nothing to contribute" makes
// admission depend on a second, unrelated invariant - so this test gives the
// kNoData venue a full book to make sure the verdict is what excludes it.
TEST(ConsolidatedBookTest, NoDataVenueIsExcludedToo) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}}, {{101, 1}});
    SetBook(books, VenueId::BYBIT, {{99, 7}}, {{102, 3}});
    Book merged;

    const auto health = Health(kNoData, kLive, kLive);
    MergeBooks(books, kVenueCount, merged, kDefaultMaxDepth, &health);

    ASSERT_EQ(merged.bids.size(), 1u);
    EXPECT_EQ(merged.bids[0].price, 99u);
}

// Total outage. Publishing nothing is honest; publishing three frozen books
// is a lie the client cannot detect.
TEST(ConsolidatedBookTest, AllVenuesStaleProducesEmptyBook) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}}, {{101, 1}});
    SetBook(books, VenueId::BYBIT, {{99, 7}}, {{102, 3}});
    SetBook(books, VenueId::OKX, {{98, 2}}, {{103, 2}});
    Book merged;

    const auto health = Health(kStale, kStale, kStale);
    MergeBooks(books, kVenueCount, merged, kDefaultMaxDepth, &health);

    EXPECT_TRUE(merged.bids.empty());
    EXPECT_TRUE(merged.asks.empty());
}

// Excluding a venue must remove it from the per-level attribution too, not
// only from the totals. A client reading `venues` to see who is quoting must
// never be told a stale venue is still there.
TEST(ConsolidatedBookTest, ExcludedVenueDisappearsFromAttribution) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}}, {{101, 1}});
    SetBook(books, VenueId::BYBIT, {{100, 7}}, {{101, 3}});  // same price - ties
    Book merged;

    const auto health = Health(kStale, kLive, kLive);
    MergeBooks(books, kVenueCount, merged, kDefaultMaxDepth, &health);

    ASSERT_EQ(merged.bids.size(), 1u);
    EXPECT_EQ(merged.bids[0].venue_count, 1u);
    EXPECT_EQ(merged.bids[0].venues[0].slot, SlotOf(VenueId::BYBIT));
    EXPECT_EQ(merged.bids[0].qty, 7u) << "BINANCE's quantity must not be counted";
}

// A Book is reused across publishes and Clear() deliberately keeps capacity. So "the merge is a full rebuild" is only
// true if Clear() resets CONTENT, not just size. If it did not, a venue that went stale would leave its levels behind
// in a recycled buffer - and the bug would appear only after warm-up, when reuse begins.
TEST(ConsolidatedBookTest, ReusedBufferDropsAVenueThatWentStale) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{100, 5}}, {{101, 1}});
    SetBook(books, VenueId::BYBIT, {{99, 7}}, {{102, 3}});

    Book merged;
    MergeBooks(books, kVenueCount, merged, kDefaultMaxDepth, nullptr);
    ASSERT_EQ(merged.bids.size(), 2u);

    // Same buffer, second merge, BINANCE now stale.
    const auto health = Health(kStale, kLive, kLive);
    MergeBooks(books, kVenueCount, merged, kDefaultMaxDepth, &health);

    ASSERT_EQ(merged.bids.size(), 1u);
    EXPECT_EQ(merged.bids[0].price, 99u);
    EXPECT_EQ(merged.bids[0].qty, 7u);
    EXPECT_EQ(merged.bids[0].venue_count, 1u);
    EXPECT_EQ(merged.bids[0].venues[0].slot, SlotOf(VenueId::BYBIT));
}

// kDisconnected is the verdict we can make with certainty - every socket for
// this venue's stream is down - so it must be excluded just as firmly as
// kStale. Unlike kStale it says nothing about the book's CONTENT: the levels
// may be only milliseconds old. They are still refused, because without a
// connection there is no way to learn that they have stopped being true.
TEST(ConsolidatedBookTest, DisconnectedVenueIsExcluded) {
    FlatBookArray books{};
    SetBook(books, VenueId::BINANCE, {{50000, 2}}, {{50010, 2}});
    SetBook(books, VenueId::BYBIT, {{49900, 3}}, {{49910, 3}});
    Book merged;

    const auto health = Health(kDisconnected, kLive, kLive);
    MergeBooks(books, kVenueCount, merged, kDefaultMaxDepth, &health);

    ASSERT_EQ(merged.bids.size(), 1u);
    EXPECT_EQ(merged.bids[0].price, 49900u);
    EXPECT_EQ(merged.bids[0].venue_count, 1u);
    EXPECT_EQ(merged.bids[0].venues[0].slot, SlotOf(VenueId::BYBIT));
}

// Each non-live state must exclude on its own. Testing them together would
// let one of them silently start admitting - the merge would still look
// correct, because the other two still exclude.
TEST(ConsolidatedBookTest, EveryNonLiveStateExcludesIndependently) {
    for (VenueHealth bad : {kStale, kNoData, kDisconnected, VenueHealth::kResyncing}) {
        FlatBookArray books{};
        SetBook(books, VenueId::BINANCE, {{100, 5}}, {{101, 1}});
        SetBook(books, VenueId::BYBIT, {{99, 7}}, {{102, 3}});
        Book merged;

        const auto health = Health(bad, kLive, kLive);
        MergeBooks(books, kVenueCount, merged, kDefaultMaxDepth, &health);

        ASSERT_EQ(merged.bids.size(), 1u) << "state " << static_cast<int>(bad);
        EXPECT_EQ(merged.bids[0].price, 99u) << "state " << static_cast<int>(bad);
    }
}
